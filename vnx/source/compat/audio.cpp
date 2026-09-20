// ─── SimpleAudioEngine backend ────────────────────────────────────────────────
// Cocos2d-x routes all audio through JNI to the Java-side Cocos2dxSound /
// Cocos2dxMusic helpers. jni_env.cpp forwards those calls here, where they're
// served by SDL2_mixer (OGG via vorbisfile, MP3 via mpg123) reading the asset
// files extracted from the APK. Lazy-initialized on the first audio call.

#include "compat/loader.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <cstring>
#include <string>
#include <cmath>
#include <map>
#include <unordered_map>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

static Mutex g_audio_lock;
struct AudioLock {
    AudioLock()  { mutexLock(&g_audio_lock); }
    ~AudioLock() { mutexUnlock(&g_audio_lock); }
};

static bool        g_inited = false;
static bool        g_failed = false;
static std::string g_assets;
static Mix_Music*  g_music = nullptr;
static float       g_music_vol = 1.0f;
static float       g_fx_vol    = 1.0f;
static std::unordered_map<std::string, Mix_Chunk*> g_chunks;
static std::unordered_map<std::string, Mix_Music*> g_musicCache;

// Effects-mute window (SDL ticks). While active, every effect channel is forced
// silent — used to cover the "drop the vehicle onto the map" transition at the
// start of a stage, where HCR revs the looping engine sound through a pitch/rate
// sweep (setEffectRate) that SDL_mixer can't reproduce, so it comes out as a
// loud, broken drone until gameplay actually begins.
// Fixed at init and never changed, so nothing needs to ask SDL_mixer how
// many channels exist while holding a lock.
static const int kChannels = 24;

static Uint32 g_fx_mute_until = 0;

// Playback rate per channel, as the game last set it.
//
// HCR rides the engine loop's rate continuously — that is its RPM. SDL_mixer
// cannot change playback rate, so whatever the game asks for, the sample keeps
// playing at its recorded pitch. At normal RPM that is merely wrong; during the
// stage-start sweep, where the rate runs far from 1.0, it is a loud broken
// drone, because the sound being played is nothing like the sound intended.
//
// A fixed mute window was covering that with a timer, which has to guess how
// long the sweep lasts. The rate itself says exactly when it is over.
static std::map<int, float> g_ch_rate;

// How far from 1.0 the rate has to be before the sample is misleading enough to
// pull down. Normal engine RPM stays inside this; the drop-in sweep does not.
static const float kRateBand = 0.35f;

static float rateAttenuation(int ch) {
    auto it = g_ch_rate.find(ch);
    if (it == g_ch_rate.end()) return 1.0f;
    float dev = fabsf(it->second - 1.0f);
    if (dev <= kRateBand) return 1.0f;
    // Fade rather than cut: a hard gate on a value the game sweeps through
    // would chatter every time it crossed the threshold.
    float t = (dev - kRateBand) / kRateBand;
    if (t > 1.0f) t = 1.0f;
    return 1.0f - t;
}
static bool fxMuted() { return SDL_GetTicks() < g_fx_mute_until; }

static bool ensureInit() {
    if (g_inited) return true;
    if (g_failed) return false;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        compatLogFmt("audio: SDL audio init FAIL: %s", SDL_GetError());
        g_failed = true;
        return false;
    }
    int got = Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
        compatLogFmt("audio: Mix_OpenAudio FAIL: %s", Mix_GetError());
        g_failed = true;
        return false;
    }
    Mix_AllocateChannels(kChannels);
    compatLogFmt("audio: SDL_mixer ready (codecs=0x%x)", got);
    g_inited = true;
    return true;
}

// Game passes asset-relative paths ("sounds/engine.ogg"); absolute paths pass through.
static std::string resolve(const char* p) {
    if (!p || !p[0]) return "";
    if (p[0] == '/' || strstr(p, ":/")) return p;
    return g_assets + "/" + p;
}

void compatAudioSetAssetsDir(const char* dir) {
    AudioLock al;
    g_assets = dir ? dir : "";
    compatLogFmt("audio: assets dir = %s", g_assets.c_str());
}

// Initialize the mixer up front (called before the game loop starts) so
// device/thread setup doesn't happen lazily in the middle of a rendered frame.
void compatAudioWarmup() {
    AudioLock al;
    ensureInit();
}

// Cache decoded Mix_Music by resolved path — cocos2d-x switches tracks on
// every scene change (loading -> gameplay, gameplay -> menu, ...), and
// Mix_LoadMUS() re-opening + re-parsing the OGG header from the SD card was
// happening synchronously right at that exact transition moment (same one
// where the branding overlay hides and CPU boost mode resets) — a real
// stutter suspect for "audio goes out of sync for a bit right as loading
// ends". A repeat play of any track (returning to a stage, revisiting a
// menu) now skips the reload entirely.
static Mix_Music* getMusic(const std::string& full) {
    auto it = g_musicCache.find(full);
    if (it != g_musicCache.end()) return it->second;
    Mix_Music* m = Mix_LoadMUS(full.c_str());
    if (!m) compatLogFmt("audio: music load FAIL %s (%s)", full.c_str(), Mix_GetError());
    g_musicCache[full] = m;  // cache the failure too (nullptr) — don't retry every call
    return m;
}

void compatAudioPreloadMusic(const char* p) {
    AudioLock al;
    if (ensureInit()) getMusic(resolve(p));
}

void compatAudioPlayMusic(const char* path, bool loop) {
    AudioLock al;
    if (!ensureInit()) return;
    std::string full = resolve(path);
    Mix_HaltMusic();
    Mix_Music* m = getMusic(full);
    if (!m) return;
    g_music = m;
    Mix_VolumeMusic((int)(g_music_vol * MIX_MAX_VOLUME));
    Mix_PlayMusic(g_music, loop ? -1 : 1);
    compatLogFmt("audio: music %s loop=%d", full.c_str(), loop ? 1 : 0);
}

void compatAudioStopMusic()   { AudioLock al; if (g_inited) Mix_HaltMusic(); }
void compatAudioPauseMusic()  { AudioLock al; if (g_inited) Mix_PauseMusic(); }
void compatAudioResumeMusic() { AudioLock al; if (g_inited) Mix_ResumeMusic(); }
void compatAudioRewindMusic() { AudioLock al; if (g_inited && g_music) Mix_PlayMusic(g_music, -1); }

void compatAudioSetMusicVolume(float v) {
    AudioLock al;
    g_music_vol = v < 0 ? 0 : v > 1 ? 1 : v;
    if (g_inited) Mix_VolumeMusic((int)(g_music_vol * MIX_MAX_VOLUME));
}

bool compatAudioMusicPlaying() {
    AudioLock al;
    return g_inited && Mix_PlayingMusic() != 0;
}

// Effects — chunk cache keyed by resolved path. Failed loads cache nullptr so
// a missing/unsupported file is logged once, not every frame.
static Mix_Chunk* getChunk(const std::string& full) {
    auto it = g_chunks.find(full);
    if (it != g_chunks.end()) return it->second;
    Mix_Chunk* c = Mix_LoadWAV(full.c_str());   // decodes OGG/MP3/WAV
    if (!c) compatLogFmt("audio: effect load FAIL %s (%s)", full.c_str(), Mix_GetError());
    g_chunks[full] = c;
    return c;
}

void compatAudioPreloadEffect(const char* p) {
    AudioLock al;
    if (ensureInit()) getChunk(resolve(p));
}

void compatAudioUnloadEffect(const char* p) {
    AudioLock al;
    if (!g_inited) return;
    auto it = g_chunks.find(resolve(p));
    if (it != g_chunks.end()) {
        if (it->second) Mix_FreeChunk(it->second);
        g_chunks.erase(it);
    }
}

// Each channel's own gain, kept separately from the global effects volume so
// the two can be combined instead of one overwriting the other. Without this,
// a setEffectsVolume call flattened every playing effect to the global level
// and threw away the per-effect gain — which is what made effects blare.
static std::map<int, float> g_ch_gain;

static void applyChannelVolume(int ch) {
    if (ch < 0) return;
    float gain = 1.0f;
    auto it = g_ch_gain.find(ch);
    if (it != g_ch_gain.end()) gain = it->second;
    Mix_Volume(ch, fxMuted() ? 0
                             : (int)(g_fx_vol * gain * rateAttenuation(ch) * MIX_MAX_VOLUME));
}

int compatAudioPlayEffect(const char* p, bool loop, float gain) {
    AudioLock al;
    if (!ensureInit()) return -1;
    Mix_Chunk* c = getChunk(resolve(p));
    if (!c) return -1;
    if (gain < 0) gain = 0; else if (gain > 1) gain = 1;
    int ch = Mix_PlayChannel(-1, c, loop ? -1 : 0);
    if (ch >= 0) { g_ch_gain[ch] = gain; g_ch_rate.erase(ch); applyChannelVolume(ch); }
    return ch;
}

// Per-channel volume for an already-playing effect (the looping engine sound
// rides this continuously as RPM changes, and it's how the game silences the
// engine on crash/pause — must be wired for the engine to ever go quiet).
void compatAudioSetEffectVolume(int ch, float vol) {
    AudioLock al;
    if (!g_inited || ch < 0) return;
    // During the stage-start window, hold the channel silent regardless of what
    // the game asks for — it rides this every frame, so the real volume snaps
    // back the moment the window ends.
    if (vol < 0) vol = 0; else if (vol > 1) vol = 1;
    // Remember it even while muted, so the real level is restored when the
    // stage-start window ends rather than being lost.
    g_ch_gain[ch] = vol;
    applyChannelVolume(ch);
}

// Silence effect channels for the next `ms` milliseconds (extends, never
// shortens, any window already in progress). Called at stage start. The engine
// loop begins right after this, so the per-channel fxMuted() checks in
// playEffect/setEffectVolume keep it silent without a blanket Mix_Volume(-1,0)
// here — which could strand a loop the game never re-sets.
// The game's own playback-rate change. We cannot honour it, but knowing it is
// what lets the mixer stay quiet exactly as long as the sound would be wrong,
// instead of for a fixed guess at how long that is.
void compatAudioSetEffectRate(int ch, float rate) {
    AudioLock al;
    if (!g_inited || ch < 0) return;
    if (rate < 0.0f) rate = 0.0f;
    g_ch_rate[ch] = rate;
    applyChannelVolume(ch);
}

void compatAudioMuteEffectsFor(int ms) {
    AudioLock al;
    if (ms <= 0) return;
    Uint32 until = SDL_GetTicks() + (Uint32)ms;
    if (until > g_fx_mute_until) g_fx_mute_until = until;
}

void compatAudioStopEffect(int ch)   { AudioLock al; if (g_inited && ch >= 0) { Mix_HaltChannel(ch); g_ch_gain.erase(ch); g_ch_rate.erase(ch); } }
void compatAudioPauseEffect(int ch)  { AudioLock al; if (g_inited && ch >= 0) Mix_Pause(ch); }
void compatAudioResumeEffect(int ch) { AudioLock al; if (g_inited && ch >= 0) Mix_Resume(ch); }
void compatAudioStopAllEffects()     { AudioLock al; if (g_inited) { Mix_HaltChannel(-1); g_ch_gain.clear(); g_ch_rate.clear(); } }
void compatAudioPauseAllEffects()    { AudioLock al; if (g_inited) Mix_Pause(-1); }
void compatAudioResumeAllEffects()   { AudioLock al; if (g_inited) Mix_Resume(-1); }

void compatAudioSetEffectsVolume(float v) {
    AudioLock al;
    g_fx_vol = v < 0 ? 0 : v > 1 ? 1 : v;
    if (!g_inited) return;
    // Rescale each channel against its own gain rather than Mix_Volume(-1, …),
    // which set every channel to the global level and discarded the per-effect
    // gain — so one setEffectsVolume call made quiet effects as loud as loud
    // ones, and the engine loop jumped to full until the game next rode it.
    // Channel count is the constant it was allocated with. It used to be
    // Mix_AllocateChannels(-1) — in the loop CONDITION, so it re-entered
    // SDL_mixer and took its audio lock on every iteration, 25 times per volume
    // change, while this already holds AudioLock. That is a lock-ordering
    // hazard against the audio callback in code every game runs, and volume
    // changes are frequent.
    for (int ch = 0; ch < kChannels; ch++) applyChannelVolume(ch);
}

float compatAudioGetMusicVolume()   { return g_music_vol; }
float compatAudioGetEffectsVolume() { return g_fx_vol; }
