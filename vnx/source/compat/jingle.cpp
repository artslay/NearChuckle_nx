#include "compat/jingle.h"
#include "compat/loader.h"

#include <SDL2/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int    kRate = 48000;
constexpr double kTail = 1.0;          // seconds of decay allowed past the last note off

struct Note { double t, dur; int pitch, vel, prog; };

// ── Standard MIDI file reading ──────────────────────────────────────────────
//
// Only what a file we generate ourselves can contain: format 0/1, ticks per
// quarter note (never SMPTE), note on/off, program change, and the tempo meta.
// Anything else is skipped by length rather than interpreted, so an unexpected
// event can't desynchronise the stream.

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint8_t  u8(void)  { if (p >= end) { ok = false; return 0; } return *p++; }
    uint16_t u16(void) { uint16_t a = u8(); return (uint16_t)((a << 8) | u8()); }
    uint32_t u32(void) { uint32_t a = u16(); return (a << 16) | u16(); }
    uint32_t varlen(void) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t b = u8();
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return v;
    }
};

struct TempoPoint { uint32_t tick; uint32_t usec_per_qn; };

bool parseMidi(const std::vector<uint8_t>& data, std::vector<Note>& out, double& total) {
    if (data.size() < 14 || memcmp(data.data(), "MThd", 4) != 0) return false;

    Reader h{ data.data() + 4, data.data() + data.size() };
    uint32_t hlen  = h.u32();
    h.u16();                                    // format — 0 and 1 are read identically
    uint16_t ntrks = h.u16();
    uint16_t div   = h.u16();
    if (!h.ok || (div & 0x8000) || div == 0) return false;   // SMPTE timing unsupported
    const uint8_t* cur = data.data() + 8 + hlen;

    struct Ev { uint32_t tick; int pitch, vel, prog; bool on; };
    std::vector<Ev>         evs;
    std::vector<TempoPoint> tempos;

    for (uint16_t t = 0; t < ntrks && cur + 8 <= data.data() + data.size(); t++) {
        if (memcmp(cur, "MTrk", 4) != 0) break;
        Reader r{ cur + 4, data.data() + data.size() };
        uint32_t tlen = r.u32();
        const uint8_t* tend = r.p + tlen;
        if (tend > data.data() + data.size()) tend = data.data() + data.size();
        r.end = tend;

        uint32_t tick = 0;
        uint8_t  status = 0;
        int      prog[16] = {0};

        while (r.p < tend && r.ok) {
            tick += r.varlen();
            uint8_t b = r.u8();
            if (b & 0x80) { status = b; }
            else          { r.p--; }             // running status: reuse the last one
            const uint8_t hi = status & 0xF0, ch = status & 0x0F;

            if (status == 0xFF) {                // meta
                uint8_t type = r.u8();
                uint32_t len = r.varlen();
                if (type == 0x51 && len == 3) {
                    uint32_t us = (uint32_t)r.u8() << 16;
                    us |= (uint32_t)r.u8() << 8;
                    us |= r.u8();
                    tempos.push_back({ tick, us });
                } else {
                    r.p += len;
                }
                if (type == 0x2F) break;
            } else if (status == 0xF0 || status == 0xF7) {
                r.p += r.varlen();               // sysex — skipped wholesale
            } else if (hi == 0x90 || hi == 0x80) {
                int pitch = r.u8(), vel = r.u8();
                bool on = (hi == 0x90) && vel > 0;
                evs.push_back({ tick, pitch, vel, prog[ch], on });
            } else if (hi == 0xC0) {
                prog[ch] = r.u8();
            } else if (hi == 0xD0) {
                r.u8();
            } else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
                r.u8(); r.u8();
            } else {
                break;                           // unrecognised — stop this track cleanly
            }
        }
        cur = tend;
    }

    if (evs.empty()) return false;

    // Ticks to seconds. With one tempo this is a multiply, but walking the map
    // costs nothing and means an edited jingle can change tempo mid-phrase.
    auto toSecs = [&](uint32_t tick) -> double {
        double   secs = 0.0;
        uint32_t prev = 0;
        uint32_t us   = 500000;                  // MIDI default: 120 BPM
        for (const TempoPoint& tp : tempos) {
            if (tp.tick >= tick) break;
            secs += (double)(tp.tick - prev) / div * us / 1e6;
            prev = tp.tick; us = tp.usec_per_qn;
        }
        return secs + (double)(tick - prev) / div * us / 1e6;
    };

    // Pair each note-on with the next note-off of the same pitch.
    for (size_t i = 0; i < evs.size(); i++) {
        if (!evs[i].on) continue;
        double start = toSecs(evs[i].tick), stop = start + 0.5;
        for (size_t j = i + 1; j < evs.size(); j++)
            if (!evs[j].on && evs[j].pitch == evs[i].pitch) { stop = toSecs(evs[j].tick); break; }
        out.push_back({ start, stop - start, evs[i].pitch, evs[i].vel, evs[i].prog });
    }
    if (out.empty()) return false;

    total = 0.0;
    for (const Note& n : out) total = fmax(total, n.t + n.dur);
    total += kTail;
    return true;
}

// ── The instrument ──────────────────────────────────────────────────────────
//
// Each partial is a frequency ratio, a starting amplitude and a decay rate in
// nepers per second. Bells are inharmonic — the ratios are deliberately not
// whole numbers, which is what stops this sounding like an organ.

struct Partial { float ratio, amp, decay; };

struct Voice {
    const Partial* parts;
    int            count;
    float          attack;              // seconds; 0 for a struck sound
};

const Partial kBell[] = {               // GM 98 "crystal" — the main voice
    {1.000f, 1.00f, 1.9f}, {2.000f, 0.62f, 2.5f}, {2.404f, 0.42f, 3.1f},
    {3.011f, 0.28f, 3.7f}, {4.166f, 0.17f, 4.8f}, {5.433f, 0.11f, 6.0f},
};
const Partial kCelesta[] = {            // GM 8 — brighter, shorter, nearly harmonic
    {1.000f, 1.00f, 3.4f}, {2.000f, 0.48f, 4.4f}, {4.000f, 0.22f, 6.0f},
    {6.180f, 0.10f, 8.0f},
};
const Partial kTinkle[] = {             // GM 112 — tiny and glassy
    {1.000f, 1.00f, 5.0f}, {2.756f, 0.40f, 7.0f}, {5.404f, 0.18f, 9.0f},
};
const Partial kPad[] = {                // GM 94 — the bed the bells hang in
    {0.500f, 0.55f, 0.30f}, {1.000f, 1.00f, 0.32f},
    {2.000f, 0.30f, 0.40f}, {3.000f, 0.12f, 0.50f},
};

Voice voiceFor(int prog) {
    switch (prog) {
        case 8:   return { kCelesta, 4, 0.0f };
        case 94:  return { kPad,     4, 0.45f };
        case 112: return { kTinkle,  3, 0.0f };
        default:  return { kBell,    6, 0.0f };
    }
}

// ── Playback state ──────────────────────────────────────────────────────────

SDL_AudioDeviceID g_dev    = 0;
float             g_len    = 0.0f;
uint64_t          g_start  = 0;
bool              g_opened = false;

bool readRomfs(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

}  // namespace

namespace jingle {

bool play(void) {
    if (g_opened) return true;

    std::vector<uint8_t> midi;
    if (!readRomfs("romfs:/audio/viridite.mid", midi)) {
        compatLog("jingle: romfs:/audio/viridite.mid missing — booting silently");
        return false;
    }

    std::vector<Note> notes;
    double total = 0.0;
    if (!parseMidi(midi, notes, total)) {
        compatLog("jingle: could not read the MIDI — booting silently");
        return false;
    }

    // SDL's audio subsystem is separate from video and is not brought up by the
    // main init, which only asks for what drawing needs.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        compatLogFmt("jingle: SDL_INIT_AUDIO failed (%s) — booting silently", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want = {}, have = {};
    want.freq     = kRate;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = nullptr;                     // queue-driven: render once, push once
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        compatLogFmt("jingle: no audio device (%s) — booting silently", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    const size_t frames = (size_t)(total * kRate) + 1;

    // One interleaved float buffer rather than two mixes plus a separate
    // conversion pass — a third of the peak memory. This runs at the exact
    // moment the guest heap and the game's textures are both live, so a
    // transient couple of megabytes here is not free.
    //
    // calloc, not std::vector, and the result is checked. The Core is built
    // with -fno-exceptions, so a container that cannot get its memory does not
    // throw something catchable — it calls std::terminate, and the console
    // shows "The software has closed because an error occurred" with nothing
    // in the log. An allocation this size, made at the tightest moment of the
    // whole run, must be allowed to fail quietly.
    float* mix = (float*)calloc(frames * 2, sizeof(float));
    if (!mix) {
        compatLogFmt("jingle: could not allocate %zu KB to render — booting silently",
                     (frames * 2 * sizeof(float)) / 1024);
        SDL_CloseAudioDevice(g_dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_dev = 0;
        return false;
    }

    for (const Note& n : notes) {
        const Voice  v    = voiceFor(n.prog);
        const double freq = 440.0 * pow(2.0, (n.pitch - 69) / 12.0);
        const float  gain = (float)(n.vel / 127.0) * 0.22f;

        // Spread by pitch: the low bed sits centre, the sparkle at the edges.
        const float  pan  = (float)(n.pitch - 74) / 60.0f;
        const float  gl   = gain * (0.5f - 0.28f * pan);
        const float  gr   = gain * (0.5f + 0.28f * pan);

        // A struck note rings past its note-off; only the pad is cut short by
        // it. Bells that stopped dead on note-off would sound damped.
        const double ring = (v.attack > 0.0f) ? n.dur + 0.35 : n.dur + kTail;
        const size_t i0   = (size_t)(n.t * kRate);
        const size_t i1   = (size_t)fmin((double)frames, (n.t + ring) * kRate);

        for (size_t i = i0; i < i1; i++) {
            const float t = (float)(i - i0) / kRate;
            float s = 0.0f;
            for (int k = 0; k < v.count; k++) {
                const Partial& pa = v.parts[k];
                s += pa.amp * expf(-pa.decay * t) *
                     sinf(6.2831853f * (float)freq * pa.ratio * t);
            }
            if (v.attack > 0.0f) s *= 1.0f - expf(-t / v.attack);
            else                 s *= 1.0f - expf(-t / 0.004f);   // soften the click
            mix[i * 2 + 0] += s * gl;
            mix[i * 2 + 1] += s * gr;
        }
    }

    // Fade the last 250ms so the buffer cannot end on a step.
    const size_t fade = (size_t)(0.25 * kRate);
    for (size_t i = 0; i < fade && i < frames; i++) {
        const float g = (float)i / fade;
        mix[(frames - 1 - i) * 2 + 0] *= g;
        mix[(frames - 1 - i) * 2 + 1] *= g;
    }

    // In place: the float samples are consumed strictly before the int16 that
    // replaces them, so no second buffer is needed.
    int16_t* pcm = reinterpret_cast<int16_t*>(mix);
    for (size_t i = 0; i < frames * 2; i++) {
        // tanh rather than a hard clip: the arpeggio piles up six partials per
        // note and four notes deep, and a clip there is audible as a buzz.
        pcm[i] = (int16_t)(tanhf(mix[i]) * 30000.0f);
    }

    SDL_QueueAudio(g_dev, pcm, (uint32_t)(frames * 2 * sizeof(int16_t)));
    free(mix);                            // SDL_QueueAudio copies into its own queue
    SDL_PauseAudioDevice(g_dev, 0);

    g_len    = (float)total;
    g_start  = SDL_GetPerformanceCounter();
    g_opened = true;
    compatLogFmt("jingle: %zu notes, %.2fs", notes.size(), total);
    return true;
}

float length(void) { return g_len; }

float elapsed(void) {
    if (!g_opened) return 0.0f;
    const double f = (double)SDL_GetPerformanceFrequency();
    const float  e = (float)((SDL_GetPerformanceCounter() - g_start) / f);
    return e < g_len ? e : g_len;
}

void stop(void) {
    if (!g_opened) return;
    SDL_CloseAudioDevice(g_dev);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_dev = 0; g_opened = false;
}

}  // namespace jingle
