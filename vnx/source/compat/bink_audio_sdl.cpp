#include <switch.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "compat/loader.h"

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

// Bink video audio uses CrySoundSystem's CS_Stream_* callback interface.
// The Android implementation ultimately queues signed 16-bit stereo PCM into
// OpenAL. On Switch the OpenAL entry points are already backed by libnx audout,
// so keep the original CS_Stream contract and feed that backend directly.
//
// Do not route this through SDL3's Android audio backend: SDL_InitSubSystem(AUDIO)
// selects the Android/OpenSL path and correctly reports "operation not supported"
// on Switch.

struct CS_STREAM;
using CS_StreamCallback = signed char (*)(CS_STREAM*, void*, int, void*);

using ALboolean = int8_t;
using ALenum = int32_t;
using ALuint = uint32_t;
using ALint = int32_t;
using ALsizei = int32_t;
using ALfloat = float;
using ALvoid = void;

extern "C" {
void alGenBuffers(ALsizei, ALuint*);
void alDeleteBuffers(ALsizei, const ALuint*);
void alBufferData(ALuint, ALenum, const ALvoid*, ALsizei, ALsizei);
void alSourcePlay(ALuint);
void alSourceStop(ALuint);
void alSourcei(ALuint, ALenum, ALint);
void alSource3f(ALuint, ALenum, ALfloat, ALfloat, ALfloat);
void alSourceQueueBuffers(ALuint, ALsizei, const ALuint*);
void alSourceUnqueueBuffers(ALuint, ALsizei, ALuint*);
void alGetSourcei(ALuint, ALenum, ALint*);
}

namespace {

constexpr ALenum AL_TRUE = 1;
constexpr ALenum AL_SOURCE_RELATIVE = 0x0202;
constexpr ALenum AL_POSITION = 0x1004;
constexpr AL_VELOCITY = 0x1006;
constexpr AL_BUFFER = 0x1009;
constexpr AL_SOURCE_STATE = 0x1010;
constexpr AL_INITIAL = 0x1011;
constexpr AL_PLAYING = 0x1012;
constexpr AL_PAUSED = 0x1013;
constexpr AL_BUFFERS_QUEUED = 0x1015;
constexpr AL_BUFFERS_PROCESSED = 0x1016;
constexpr AL_FORMAT_STEREO16 = 0x1103;

constexpr int CS_FREE = -1;
constexpr int MIN_QUEUED_BUFFERS = 20;

struct BinkStream {
    CS_StreamCallback callback = nullptr;
    void* userdata = nullptr;
    std::vector<uint8_t> scratch;
    int len = 0;
    int sample_rate = 44100;
    ALuint source = 0;
    int channel = CS_FREE;
};

std::vector<BinkStream*> g_streams;
unsigned g_update_log_count = 0;

// BinkDecAudioCallback initializes the complete callback buffer to 0xFF and
// Bink_GetAudioData() overwrites only the decoded bytes. The original Android
// OpenAL backend strips the trailing 0xFF region before queueing it.
static int BytesFromBinkDec(const uint8_t* buffer, int len) {
    if (!buffer || len <= 0)
        return 0;

    constexpr int MAX_END_CHECK = 100;
    if (len <= MAX_END_CHECK)
        return len;

    int bytes_processed = 0;

    for (int i = 0; i < len - MAX_END_CHECK; ++i) {
        if (buffer[i] != 0xFF) {
            ++bytes_processed;
            continue;
        }

        bool hit_end = true;
        for (int j = 1; j < MAX_END_CHECK; ++j) {
            if (buffer[i + j] != 0xFF) {
                hit_end = false;
                break;
            }
        }

        if (hit_end)
            break;

        ++bytes_processed;
    }

    if (bytes_processed == len - MAX_END_CHECK)
        bytes_processed += MAX_END_CHECK;

    return bytes_processed;
}

BinkStream* asBink(CS_STREAM* stream) {
    return reinterpret_cast<BinkStream*>(stream);
}

void removeStream(BinkStream* stream) {
    for (auto it = g_streams.begin(); it != g_streams.end(); ++it) {
        if (*it == stream) {
            g_streams.erase(it);
            return;
        }
    }
}

void updateStream(BinkStream* stream) {
    if (!stream || !stream->callback || stream->channel == CS_FREE ||
        !stream->source || stream->len <= 0)
        return;

    ALint state = AL_INITIAL;
    alGetSourcei(stream->source, AL_SOURCE_STATE, &state);
    if (state == AL_PAUSED)
        return;

    ALint processed = 0;
    alGetSourcei(stream->source, AL_BUFFERS_PROCESSED, &processed);

    for (ALint i = 0; i < processed; ++i) {
        ALuint buffer = 0;
        alSourceUnqueueBuffers(stream->source, 1, &buffer);
        if (buffer)
            alDeleteBuffers(1, &buffer);
    }

    ALint queued = 0;
    alGetSourcei(stream->source, AL_BUFFERS_QUEUED, &queued);

    if (queued >= MIN_QUEUED_BUFFERS)
        return;

    const int missing = MIN_QUEUED_BUFFERS - queued;
    for (int i = 0; i < missing; ++i) {
        std::memset(stream->scratch.data(), 0xFF, stream->scratch.size());

        const signed char got_audio =
            stream->callback(reinterpret_cast<CS_STREAM*>(stream),
                             stream->scratch.data(), stream->len,
                             stream->userdata);
        if (!got_audio)
            break;

        int bytes_processed = stream->len;
        if (stream->len == 138240)
            bytes_processed = BytesFromBinkDec(stream->scratch.data(), stream->len);

        if (bytes_processed <= 0)
            break;

        ALuint stream_buffer = 0;
        alGenBuffers(1, &stream_buffer);
        if (!stream_buffer)
            break;

        alBufferData(stream_buffer, AL_FORMAT_STEREO16,
                     stream->scratch.data(), bytes_processed,
                     stream->sample_rate);
        alSourceQueueBuffers(stream->source, 1, &stream_buffer);

        ++queued;
    }

    if (state != AL_PLAYING && state != AL_PAUSED &&
        stream->channel != CS_FREE && queued > 0)
        alSourcePlay(stream->source);

    if (g_update_log_count < 24) {
        compatLogFmt("BINK AUDIO: update channel=%d rate=%d queued=%d processed=%d",
                     stream->channel, stream->sample_rate,
                     queued, static_cast<int>(processed));
        ++g_update_log_count;
    }
}

} // namespace

extern "C" {

void* near_bink_cs_stream_create(CS_StreamCallback callback,
                                  int length,
                                  unsigned int,
                                  int samplerate,
                                  void* userdata) {
    if (!callback || length <= 0)
        return nullptr;

    auto* stream = new BinkStream;
    stream->callback = callback;
    stream->userdata = userdata;
    stream->len = length;
    stream->sample_rate = samplerate > 0 ? samplerate : 44100;
    stream->scratch.resize(static_cast<size_t>(length));

    alGenBuffers(0, nullptr); // keep the OpenAL shim link self-contained

    alGenBuffers(0, nullptr);

    ALuint source = 0;
    extern void alGenSources(ALsizei, ALuint*);
    alGenSources(1, &source);
    if (!source) {
        delete stream;
        return nullptr;
    }

    stream->source = source;
    stream->channel = 30 + static_cast<int>(g_streams.size());
    g_streams.push_back(stream);

    compatLogFmt("BINK AUDIO: create channel=%d len=%d rate=%d source=%u",
                 stream->channel, stream->len, stream->sample_rate,
                 static_cast<unsigned>(stream->source));

    return reinterpret_cast<void*>(stream);
}

int near_bink_cs_stream_play(int, CS_STREAM* opaque_stream) {
    BinkStream* stream = asBink(opaque_stream);
    if (!stream || !stream->source)
        return -1;

    alSourcei(stream->source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(stream->source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSource3f(stream->source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    stream->channel = 30 + static_cast<int>(
        std::find(g_streams.begin(), g_streams.end(), stream) - g_streams.begin());

    // The Android backend calls alSourcePlay() before the first CS_Update()
    // has queued a Bink buffer. The Switch OpenAL shim therefore keeps this
    // as a pending play request until the first buffer arrives.
    alSourcePlay(stream->source);

    compatLogFmt("BINK AUDIO: play channel=%d rate=%d len=%d source=%u",
                 stream->channel, stream->sample_rate, stream->len,
                 static_cast<unsigned>(stream->source));
    return stream->channel;
}

signed char near_bink_cs_stream_stop(CS_STREAM* opaque_stream) {
    BinkStream* stream = asBink(opaque_stream);
    if (!stream || !stream->source)
        return 0;

    alSourceStop(stream->source);

    ALint queued = 0;
    alGetSourcei(stream->source, AL_BUFFERS_QUEUED, &queued);
    for (ALint i = 0; i < queued; ++i) {
        ALuint buffer = 0;
        alSourceUnqueueBuffers(stream->source, 1, &buffer);
        if (buffer)
            alDeleteBuffers(1, &buffer);
    }

    stream->channel = CS_FREE;

    compatLogFmt("BINK AUDIO: stop source=%u",
                 static_cast<unsigned>(stream->source));
    return 1;
}

signed char near_bink_cs_stream_close(CS_STREAM* opaque_stream) {
    BinkStream* stream = asBink(opaque_stream);
    if (!stream)
        return 0;

    near_bink_cs_stream_stop(opaque_stream);

    extern void alDeleteSources(ALsizei, const ALuint*);
    const ALuint source = stream->source;
    if (source)
        alDeleteSources(1, &source);

    removeStream(stream);
    delete stream;

    compatLog("BINK AUDIO: close");
    return 1;
}

void near_bink_cs_update(void) {
    // The CS_Update symbol from CrySoundSystem is also used for OGG/music.
    // Keep the real guest updater, then pump the Bink streams.
    using GuestUpdateFn = void (*)();

    static void* real_cs_update = nullptr;
    if (!real_cs_update) {
        LoadedSo* sound = elfFindLoaded("libCrySoundSystem.so");
        if (sound)
            real_cs_update = sound->findSym("CS_Update");

        if (real_cs_update)
            compatLogFmt("BINK AUDIO: guest CS_Update=%p", real_cs_update);
        else
            compatLog("BINK AUDIO: guest CS_Update not found");
    }

    if (real_cs_update)
        reinterpret_cast<GuestUpdateFn>(real_cs_update)();

    for (BinkStream* stream : g_streams)
        updateStream(stream);
}

} // extern "C"
