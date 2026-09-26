#include <cstdint>
#include <cstring>
#include <vector>

#include "compat/loader.h"

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

namespace {

struct CS_STREAM;
using CS_StreamCallback =
    signed char (*)(CS_STREAM*, void*, int, void*);

struct SDL_AudioStream;
struct SDL_AudioSpec {
    uint32_t format;
    int channels;
    int freq;
};

using SDL_InitSubSystemFn = bool (*)(uint32_t);
using SDL_QuitSubSystemFn = void (*)(uint32_t);
using SDL_GetErrorFn = const char* (*)();
using SDL_OpenAudioDeviceStreamFn =
    SDL_AudioStream* (*)(uint32_t, const SDL_AudioSpec*, void*, void*);
using SDL_ResumeAudioStreamDeviceFn = bool (*)(SDL_AudioStream*);
using SDL_PauseAudioStreamDeviceFn = bool (*)(SDL_AudioStream*);
using SDL_ClearAudioStreamFn = bool (*)(SDL_AudioStream*);
using SDL_PutAudioStreamDataFn = bool (*)(SDL_AudioStream*, const void*, int);
using SDL_DestroyAudioStreamFn = void (*)(SDL_AudioStream*);

struct SDLApi {
    SDL_InitSubSystemFn init_subsystem = nullptr;
    SDL_QuitSubSystemFn quit_subsystem = nullptr;
    SDL_GetErrorFn get_error = nullptr;
    SDL_OpenAudioDeviceStreamFn open_device_stream = nullptr;
    SDL_ResumeAudioStreamDeviceFn resume_stream_device = nullptr;
    SDL_PauseAudioStreamDeviceFn pause_stream_device = nullptr;
    SDL_ClearAudioStreamFn clear_stream = nullptr;
    SDL_PutAudioStreamDataFn put_stream_data = nullptr;
    SDL_DestroyAudioStreamFn destroy_stream = nullptr;
    bool resolved = false;
};

struct BinkStream {
    CS_StreamCallback callback = nullptr;
    void* userdata = nullptr;
    std::vector<uint8_t> scratch;
    SDL_AudioStream* sdl_stream = nullptr;
    int len = 0;
    int sample_rate = 44100;
    bool playing = false;
    int channel = -1;
};

constexpr uint32_t SDL_INIT_AUDIO = 0x00000010u;
constexpr uint32_t SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK = 0xFFFFFFFFu;
constexpr uint32_t SDL_AUDIO_S16 = 0x8010u;

SDLApi g_sdl;
std::vector<BinkStream*> g_streams;
int g_next_channel = 0;
void* g_real_cs_update = nullptr;
bool g_sdl_audio_init = false;

template<typename T>
T resolveSDL(const char* name) {
    LoadedSo* sdl = elfFindLoaded("libSDL3.so");
    if (!sdl)
        return nullptr;
    return reinterpret_cast<T>(sdl->findSym(name));
}

bool resolveSDLApi() {
    if (g_sdl.resolved)
        return g_sdl.open_device_stream != nullptr &&
               g_sdl.resume_stream_device != nullptr &&
               g_sdl.put_stream_data != nullptr;

    g_sdl.resolved = true;
    g_sdl.init_subsystem = resolveSDL<SDL_InitSubSystemFn>("SDL_InitSubSystem");
    g_sdl.quit_subsystem = resolveSDL<SDL_QuitSubSystemFn>("SDL_QuitSubSystem");
    g_sdl.get_error = resolveSDL<SDL_GetErrorFn>("SDL_GetError");
    g_sdl.open_device_stream =
        resolveSDL<SDL_OpenAudioDeviceStream>("SDL_OpenAudioDeviceStream");
    g_sdl.resume_stream_device =
        resolveSDL<SDL_ResumeAudioStreamDeviceFn>("SDL_ResumeAudioStreamDevice");
    g_sdl.pause_stream_device =
        resolveSDL<SDL_PauseAudioStreamDeviceFn>("SDL_PauseAudioStreamDevice");
    g_sdl.clear_stream =
        resolveSDL<SDL_ClearAudioStreamFn>("SDL_ClearAudioStream");
    g_sdl.put_stream_data =
        resolveSDL<SDL_PutAudioStreamDataFn>("SDL_PutAudioStreamData");
    g_sdl.destroy_stream =
        resolveSDL<SDL_DestroyAudioStreamFn>("SDL_DestroyAudioStream");

    compatLogFmt(
        "BINK SDL: resolve init=%p open=%p resume=%p pause=%p clear=%p put=%p destroy=%p",
        reinterpret_cast<void*>(g_sdl.init_subsystem),
        reinterpret_cast<void*>(g_sdl.open_device_stream),
        reinterpret_cast<void*>(g_sdl.resume_stream_device),
        reinterpret_cast<void*>(g_sdl.pause_stream_device),
        reinterpret_cast<void*>(g_sdl.clear_stream),
        reinterpret_cast<void*>(g_sdl.put_stream_data),
        reinterpret_cast<void*>(g_sdl.destroy_stream));

    return g_sdl.open_device_stream != nullptr &&
           g_sdl.resume_stream_device != nullptr &&
           g_sdl.put_stream_data != nullptr &&
           g_sdl.destroy_stream != nullptr;
}

void logSDLError(const char* stage) {
    const char* err = g_sdl.get_error ? g_sdl.get_error() : nullptr;
    compatLogFmt("BINK SDL: %s failed%s%s",
                 stage,
                 err && *err ? " error=" : "",
                 err && *err ? err : "");
}

bool ensureSDLAudio() {
    if (!resolveSDLApi())
        return false;

    if (g_sdl_audio_init)
        return true;

    if (g_sdl.init_subsystem && !g_sdl.init_subsystem(SDL_INIT_AUDIO)) {
        logSDLError("SDL_InitSubSystem(AUDIO)");
        return false;
    }

    g_sdl_audio_init = true;
    compatLog("BINK SDL: audio subsystem ready");
    return true;
}

void releaseSDLAudio() {
    if (!g_sdl_audio_init || !g_sdl.quit_subsystem)
        return;
    g_sdl.quit_subsystem(SDL_INIT_AUDIO);
    g_sdl_audio_init = false;
}

BinkStream* asBink(CS_STREAM* stream) {
    return reinterpret_cast<BinkStream*>(stream);
}

void removeStream(BinkStream* stream) {
    for (auto it = g_streams.begin(); it != g_streams.end(); ++it) {
        if (*it == stream) {
            g_streams.erase(it);
            break;
        }
    }
}

void updateStream(BinkStream* stream) {
    if (!stream || !stream->playing || !stream->sdl_stream ||
        !stream->callback || stream->len <= 0)
        return;

    if (stream->scratch.size() != static_cast<size_t>(stream->len))
        stream->scratch.resize(static_cast<size_t>(stream->len));

    // UIVideoBinkDec's callback clears the buffer with 0xFF and returns zero
    // when no new video frame was decoded. Feed only freshly decoded Bink PCM.
    std::memset(stream->scratch.data(), 0xFF, stream->scratch.size());
    const signed char got_audio =
        stream->callback(reinterpret_cast<CS_STREAM*>(stream),
                         stream->scratch.data(), stream->len, stream->userdata);
    if (!got_audio)
        return;

    if (!g_sdl.put_stream_data(
            stream->sdl_stream, stream->scratch.data(), stream->len)) {
        logSDLError("SDL_PutAudioStreamData");
        return;
    }

    static unsigned update_log_count = 0;
    if (update_log_count < 24) {
        int peak = 0;
        const size_t samples = stream->scratch.size() / sizeof(int16_t);
        const int16_t* pcm =
            reinterpret_cast<const int16_t*>(stream->scratch.data());
        for (size_t i = 0; i < samples; ++i) {
            const int v = pcm[i] < 0 ? -static_cast<int>(pcm[i])
                                     : static_cast<int>(pcm[i]);
            if (v > peak)
                peak = v;
        }

        compatLogFmt("BINK SDL: push[%u] ch=%d bytes=%d rate=%d peak=%d",
                     update_log_count, stream->channel, stream->len,
                     stream->sample_rate, peak);
        ++update_log_count;
    }
}

} // namespace

extern "C" {

void* near_bink_cs_stream_create(CS_StreamCallback callback,
                                  int length,
                                  unsigned int,
                                  int samplerate,
                                  void* userdata) {
    if (!callback || length <= 0) {
        compatLog("BINK SDL: CS_Stream_Create invalid callback/length");
        return nullptr;
    }

    if (!ensureSDLAudio())
        return nullptr;

    auto* stream = new BinkStream;
    stream->callback = callback;
    stream->userdata = userdata;
    stream->len = length;
    stream->sample_rate = samplerate > 0 ? samplerate : 44100;
    stream->scratch.resize(static_cast<size_t>(length));

    SDL_AudioSpec spec = {
        SDL_AUDIO_S16,
        2,
        stream->sample_rate
    };

    stream->sdl_stream = g_sdl.open_device_stream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream->sdl_stream) {
        logSDLError("SDL_OpenAudioDeviceStream");
        delete stream;
        if (g_streams.empty())
            releaseSDLAudio();
        return nullptr;
    }

    stream->channel = 64 + g_next_channel++;
    g_streams.push_back(stream);

    compatLogFmt(
        "BINK SDL: create channel=%d len=%d rate=%d userdata=%p stream=%p",
        stream->channel, stream->len, stream->sample_rate,
        stream->userdata, static_cast<void*>(stream->sdl_stream));

    return stream;
}

int near_bink_cs_stream_play(int channel, CS_STREAM* opaque_stream) {
    auto* stream = asBink(opaque_stream);
    if (!stream || channel != -1 || !stream->sdl_stream)
        return -1;

    stream->playing = true;

    if (!g_sdl.resume_stream_device(stream->sdl_stream)) {
        logSDLError("SDL_ResumeAudioStreamDevice");
        stream->playing = false;
        return -1;
    }

    compatLogFmt("BINK SDL: play channel=%d rate=%d len=%d",
                 stream->channel, stream->sample_rate, stream->len);
    return stream->channel;
}

signed char near_bink_cs_stream_stop(CS_STREAM* opaque_stream) {
    auto* stream = asBink(opaque_stream);
    if (!stream)
        return 0;

    stream->playing = false;
    if (g_sdl.pause_stream_device)
        g_sdl.pause_stream_device(stream->sdl_stream);
    if (g_sdl.clear_stream)
        g_sdl.clear_stream(stream->sdl_stream);

    compatLogFmt("BINK SDL: stop channel=%d", stream->channel);
    stream->channel = -1;
    return 1;
}

signed char near_bink_cs_stream_close(CS_STREAM* opaque_stream) {
    auto* stream = asBink(opaque_stream);
    if (!stream)
        return 0;

    near_bink_cs_stream_stop(opaque_stream);
    if (stream->sdl_stream && g_sdl.destroy_stream)
        g_sdl.destroy_stream(stream->sdl_stream);

    removeStream(stream);
    delete stream;

    if (g_streams.empty())
        releaseSDLAudio();

    compatLog("BINK SDL: close");
    return 1;
}

void near_bink_cs_update(void) {
    using GuestUpdateFn = void (*)();

    if (!g_real_cs_update) {
        LoadedSo* sound = elfFindLoaded("libCrySoundSystem.so");
        if (sound)
            g_real_cs_update = sound->findSym("CS_Update");

        if (g_real_cs_update)
            compatLogFmt("BINK SDL: guest CS_Update=%p", g_real_cs_update);
        else
            compatLog("BINK SDL: guest CS_Update not found");
    }

    // Preserve the original CrySoundSystem streaming path (OGG/music/etc.).
    if (g_real_cs_update)
        reinterpret_cast<GuestUpdateFn>(g_real_cs_update)();

    // Bink audio stays synchronized with the video Present() call.
    for (BinkStream* stream : g_streams)
        updateStream(stream);
}

} // extern "C"
