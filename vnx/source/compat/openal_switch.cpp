#include <switch.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern void compatLogFmt(const char* fmt, ...);

struct ALCdevice {};
struct ALCcontext { ALCdevice* device; };

using ALboolean = int8_t;
using ALchar = char;
using ALshort = int16_t;
using ALuint = uint32_t;
using ALint = int32_t;
using ALsizei = int32_t;
using ALenum = int32_t;
using ALfloat = float;
using ALvoid = void;
using ALCboolean = int8_t;
using ALCchar = char;
using ALCint = int32_t;
using ALCsizei = int32_t;
using ALCenum = int32_t;

namespace {
constexpr ALenum AL_FALSE=0, AL_TRUE=1;
constexpr ALenum AL_SOURCE_RELATIVE=0x202, AL_PITCH=0x1003, AL_POSITION=0x1004;
constexpr ALenum AL_DIRECTION=0x1005, AL_VELOCITY=0x1006, AL_LOOPING=0x1007;
constexpr ALenum AL_BUFFER=0x1009, AL_GAIN=0x100A, AL_MIN_GAIN=0x100D, AL_MAX_GAIN=0x100E;
constexpr ALenum AL_ORIENTATION=0x100F, AL_SOURCE_STATE=0x1010, AL_INITIAL=0x1011;
constexpr ALenum AL_PLAYING=0x1012, AL_PAUSED=0x1013, AL_STOPPED=0x1014;
constexpr ALenum AL_BUFFERS_QUEUED=0x1015, AL_BUFFERS_PROCESSED=0x1016;
constexpr ALenum AL_SEC_OFFSET=0x1024, AL_SAMPLE_OFFSET=0x1025, AL_BYTE_OFFSET=0x1026;
constexpr ALenum AL_REFERENCE_DISTANCE=0x1020, AL_ROLLOFF_FACTOR=0x1021, AL_MAX_DISTANCE=0x1023;
constexpr ALenum AL_FORMAT_MONO8=0x1100, AL_FORMAT_MONO16=0x1101;
constexpr ALenum AL_FORMAT_STEREO8=0x1102, AL_FORMAT_STEREO16=0x1103;
constexpr ALenum AL_FORMAT_MONO_FLOAT32=0x10010, AL_FORMAT_STEREO_FLOAT32=0x10011;
constexpr ALenum AL_NO_ERROR=0, AL_INVALID_NAME=0xA001, AL_INVALID_ENUM=0xA002;
constexpr ALenum AL_INVALID_VALUE=0xA003, AL_INVALID_OPERATION=0xA004, AL_OUT_OF_MEMORY=0xA005;
constexpr ALCenum ALC_FALSE=0, ALC_TRUE=1;
constexpr ALCenum ALC_NO_ERROR=0, ALC_INVALID_DEVICE=0xA001, ALC_INVALID_CONTEXT=0xA002;
constexpr ALCenum ALC_INVALID_ENUM=0xA003, ALC_INVALID_VALUE=0xA004;
constexpr size_t kMaxBuffers=2048, kMaxSources=256, kFrames=1024, kOutBuffers=4;
constexpr uint32_t kRate=48000;

struct Buffer {
    bool used=false;
    ALenum format=AL_FORMAT_MONO16;
    ALsizei rate=48000;
    ALsizei channels=1;
    std::vector<int16_t> pcm;
};

struct Source {
    bool used=false;
    ALint state=AL_INITIAL;
    bool looping=false;
    bool relative=false;
    float gain=1.0f, pitch=1.0f;
    float pos[3]={0,0,0};
    float ref=1.0f, maxdist=1000000.0f, rolloff=1.0f, mingain=0.0f, maxgain=1.0f;
    std::vector<ALuint> queue;
    size_t current=0, processed=0;
    double sample_pos=0.0;
};

struct AudioState {
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    Thread thread={};
    Mutex lock={};
    alignas(0x1000) int16_t pcm[kOutBuffers][kFrames*2]={};
    AudioOutBuffer out[kOutBuffers]={};
};

Buffer g_buffers[kMaxBuffers];
Source g_sources[kMaxSources];
AudioState g_audio;
ALCdevice g_device;
ALCcontext g_context{&g_device};
ALCcontext* g_current_context=nullptr;
ALuint g_next_buffer=1, g_next_source=1;
thread_local ALenum g_al_error=AL_NO_ERROR;
thread_local ALCenum g_alc_error=ALC_NO_ERROR;
float g_listener_gain=1.0f;
float g_listener_pos[3]={0,0,0};
unsigned g_buffer_log_count=0;
unsigned g_queue_log_count=0;
unsigned g_play_log_count=0;
unsigned g_unqueue_log_count=0;
unsigned g_listener_log_count=0;
unsigned g_mix_log_count=0;
unsigned g_output_log_count=0;

Buffer* getBuffer(ALuint id) {
    return id<kMaxBuffers && id ? (g_buffers[id].used ? &g_buffers[id] : nullptr) : nullptr;
}
Source* getSource(ALuint id) {
    return id<kMaxSources && id ? (g_sources[id].used ? &g_sources[id] : nullptr) : nullptr;
}
void setError(ALenum e) { if(g_al_error==AL_NO_ERROR) g_al_error=e; }
void setAlcError(ALCenum e) { if(g_alc_error==ALC_NO_ERROR) g_alc_error=e; }

void resetSource(Source& s) {
    s.queue.clear(); s.current=0; s.processed=0; s.sample_pos=0.0;
}

void mixSource(Source& s, float* dst, size_t frames) {
    if(s.state!=AL_PLAYING || s.queue.empty()) return;

    // CryMovie uses dedicated OpenAL source 32 for the intro stream. Its Bink
    // decoder delivers valid PCM, but the decoded track is substantially quieter
    // than regular game audio on the Switch path. Apply a targeted gain only to
    // this movie source; all other OpenAL sources keep the game's requested gain.
    const float movie_boost = 1.0f;

    for(size_t o=0;o<frames;o++) {
        while(s.current<s.queue.size()) {
            Buffer* b=getBuffer(s.queue[s.current]);
            if(!b || b->pcm.empty()) {
                ++s.current; ++s.processed; s.sample_pos=0;
                continue;
            }
            const size_t src_frames=b->pcm.size()/static_cast<size_t>(b->channels);
            if(!src_frames) { ++s.current; ++s.processed; s.sample_pos=0; continue; }

            if(s.sample_pos>=src_frames) {
                s.sample_pos=0;
                if(s.looping && s.queue.size()==1) {
                    s.current=0; s.processed=0;
                } else {
                    ++s.current; ++s.processed;
                    if(s.current>=s.queue.size() && s.looping) {
                        s.current=0; s.processed=0;
                    }
                }
                continue;
            }

            const double step=std::max(0.01, static_cast<double>(b->rate)/kRate*
                                             std::max(0.01f,s.pitch));
            const size_t i0=static_cast<size_t>(s.sample_pos);
            const size_t i1=std::min(i0+1,src_frames-1);
            const float frac=static_cast<float>(s.sample_pos-i0);
            float left=0,right=0;

            if(b->channels==1) {
                const float a=b->pcm[i0]/32768.0f;
                const float c=b->pcm[i1]/32768.0f;
                const float mono=a+(c-a)*frac;

                // AL_SOURCE_RELATIVE means the source is attached to the
                // listener (2D audio). Movie/dialogue streams commonly use
                // this mode; applying world-space attenuation to them can
                // make the cutscene track effectively inaudible.
                if(s.relative) {
                    const float v=mono*s.gain*g_listener_gain*movie_boost;
                    left=v*0.70710678f;
                    right=v*0.70710678f;
                } else {
                    const float dx=s.pos[0]-g_listener_pos[0];
                    const float dy=s.pos[1]-g_listener_pos[1];
                    const float dz=s.pos[2]-g_listener_pos[2];
                    const float dist=std::sqrt(dx*dx+dy*dy+dz*dz);
                    float att=1.0f;
                    if(dist>s.ref)
                        att=s.ref/(s.ref+s.rolloff*(dist-s.ref));
                    if(dist>=s.maxdist) att=0.0f;
                    att=std::clamp(att,s.mingain,s.maxgain);
                    const float pan=std::clamp(dx/std::max(1.0f,dist),-1.0f,1.0f);
                    const float v=mono*s.gain*g_listener_gain*att*movie_boost;
                    left=v*0.5f*(1.0f-pan);
                    right=v*0.5f*(1.0f+pan);
                }
            } else {
                const size_t p0=i0*2, p1=i1*2;
                left=(b->pcm[p0]+(b->pcm[p1]-b->pcm[p0])*frac)/32768.0f*s.gain*g_listener_gain*movie_boost;
                right=(b->pcm[p0+1]+(b->pcm[p1+1]-b->pcm[p0+1])*frac)/32768.0f*s.gain*g_listener_gain*movie_boost;
            }
            dst[o*2]+=left;
            dst[o*2+1]+=right;
            s.sample_pos+=step;
            break;
        }

        if(s.current>=s.queue.size()) {
            if(s.looping && !s.queue.empty()) {
                s.current=0; s.processed=0; s.sample_pos=0;
            } else if(s.queue.empty()) {
                // True OpenAL streaming semantics: stop only after the
                // application has actually removed all processed buffers.
                s.state=AL_STOPPED;
                s.sample_pos=0;
                break;
            } else {
                // The movie streamer may be between QueueBuffers and
                // UnqueueBuffers calls. Keep the source in PLAYING state so
                // a newly queued buffer can continue without a stop/start gap.
                break;
            }
        }
    }
}

void mixBlock(int16_t* out) {
    float mix[kFrames*2]={};
    bool movie_active=false;
    mutexLock(&g_audio.lock);
    for(auto& s:g_sources) if(s.used) mixSource(s,mix,kFrames);

    Source& diag = g_sources[32];
    movie_active = diag.used && diag.state == AL_PLAYING && !diag.queue.empty();
    if(g_mix_log_count < 24 && movie_active) {
        float peak=0.0f, avg=0.0f;
        for(size_t i=0;i<kFrames*2;i++) {
            const float a=std::fabs(mix[i]);
            peak=std::max(peak,a);
            avg+=a;
        }
        avg/=(kFrames*2);
        compatLogFmt("AUDIO: mix[%u] ACTIVE src32 peak=%d avg=%d queue=%u processed=%u current=%u gain=%.3f listener=%.3f",
                     g_mix_log_count,
                     static_cast<int>(std::lrintf(peak*32767.0f)),
                     static_cast<int>(std::lrintf(avg*32767.0f)),
                     static_cast<unsigned>(diag.queue.size()),
                     static_cast<unsigned>(diag.processed),
                     static_cast<unsigned>(diag.current),
                     diag.gain, g_listener_gain);
        ++g_mix_log_count;
    }
    mutexUnlock(&g_audio.lock);

    int peak=0;
    int64_t accum=0;
    for(size_t i=0;i<kFrames*2;i++) {
        int sample=static_cast<int>(std::lrintf(std::clamp(mix[i],-1.0f,1.0f)*32767.0f));

        out[i]=static_cast<int16_t>(sample);
        const int a=std::abs(sample);
        if(a>peak) peak=a;
        accum+=a;
    }
    if(movie_active && g_output_log_count<32) {
        compatLogFmt("AUDIO: audout-ready[%u] peak=%d avg=%d bytes=%zu",
                     g_output_log_count, peak,
                     static_cast<int>(accum/static_cast<int64_t>(kFrames*2)),
                     sizeof(g_audio.pcm[0]));
        ++g_output_log_count;
    }
}

void audioThread(void*) {
    while(!g_audio.stop.load(std::memory_order_relaxed)) {
        AudioOutBuffer* released=nullptr;
        u32 released_count=0;
        Result rc=audoutWaitPlayFinish(&released,&released_count,20000000ULL);
        if(R_FAILED(rc)) {
            if(g_audio.stop.load(std::memory_order_relaxed)) break;
            svcSleepThread(1000000);
            continue;
        }
        if(!released_count || !released) continue;
        mixBlock(static_cast<int16_t*>(released->buffer));
        released->next=nullptr;
        released->data_offset=0;
        released->buffer_size=sizeof(g_audio.pcm[0]);
        released->data_size=sizeof(g_audio.pcm[0]);
        const Result append_rc=audoutAppendAudioOutBuffer(released);
        if(R_FAILED(append_rc)) {
            compatLogFmt("AUDIO: audoutAppend FAILED rc=0x%08x", static_cast<unsigned>(append_rc));
            if(!g_audio.stop.load(std::memory_order_relaxed))
                svcSleepThread(1000000);
        }
    }
    g_audio.started.store(false,std::memory_order_relaxed);
}

bool startAudio() {
    if(g_audio.started.load(std::memory_order_relaxed)) return true;
    Result rc=audoutInitialize();
    if(R_FAILED(rc)) {
        compatLogFmt("AUDIO: audoutInitialize FAILED rc=0x%08x", static_cast<unsigned>(rc));
        setAlcError(ALC_INVALID_DEVICE);
        return false;
    }

    compatLogFmt("AUDIO: audout initialized rate=%u channels=%u format=%d",
                 audoutGetSampleRate(), audoutGetChannelCount(),
                 static_cast<int>(audoutGetPcmFormat()));

    rc=audoutSetAudioOutVolume(1.0f);
    if(R_FAILED(rc))
        compatLogFmt("AUDIO: audoutSetAudioOutVolume FAILED rc=0x%08x", static_cast<unsigned>(rc));
    else
        compatLogFmt("%s", "AUDIO: audout volume=1.000");

    std::memset(g_audio.pcm,0,sizeof(g_audio.pcm));
    for(size_t i=0;i<kOutBuffers;i++) {
        g_audio.out[i]={};
        g_audio.out[i].next=nullptr;
        g_audio.out[i].buffer=g_audio.pcm[i];
        g_audio.out[i].buffer_size=sizeof(g_audio.pcm[i]);
        g_audio.out[i].data_size=sizeof(g_audio.pcm[i]);
        g_audio.out[i].data_offset=0;
        rc=audoutAppendAudioOutBuffer(&g_audio.out[i]);
        if(R_FAILED(rc)) { audoutExit(); setAlcError(ALC_INVALID_DEVICE); return false; }
    }
    rc=audoutStartAudioOut();
    if(R_FAILED(rc)) {
        compatLogFmt("AUDIO: audoutStartAudioOut FAILED rc=0x%08x", static_cast<unsigned>(rc));
        audoutExit();
        setAlcError(ALC_INVALID_DEVICE);
        return false;
    }
    compatLogFmt("%s", "AUDIO: audout playback started");

    g_audio.stop.store(false,std::memory_order_relaxed);
    rc=threadCreate(&g_audio.thread,audioThread,nullptr,nullptr,0x4000,0x2B,-2);
    if(R_FAILED(rc) || R_FAILED(threadStart(&g_audio.thread))) {
        audoutStopAudioOut(); audoutExit(); setAlcError(ALC_INVALID_DEVICE); return false;
    }
    g_audio.started.store(true,std::memory_order_relaxed);
    return true;
}

void stopAudio() {
    if(!g_audio.started.load(std::memory_order_relaxed) && !g_audio.stop.load()) return;
    g_audio.stop.store(true,std::memory_order_relaxed);
    if(g_audio.thread.handle!=INVALID_HANDLE) {
        threadWaitForExit(&g_audio.thread);
        threadClose(&g_audio.thread);
        g_audio.thread={};
    }
    audoutStopAudioOut();
    audoutExit();
    g_audio.started.store(false,std::memory_order_relaxed);
}

ALuint allocBufferId() {
    for(size_t n=0;n<kMaxBuffers;n++) {
        ALuint id=g_next_buffer++;
        if(g_next_buffer>=kMaxBuffers) g_next_buffer=1;
        if(!g_buffers[id].used) return id;
    }
    return 0;
}
ALuint allocSourceId() {
    for(size_t n=0;n<kMaxSources;n++) {
        ALuint id=g_next_source++;
        if(g_next_source>=kMaxSources) g_next_source=1;
        if(!g_sources[id].used) return id;
    }
    return 0;
}

} // namespace

extern "C" {
ALCdevice* alcOpenDevice(const ALCchar*) {
    return startAudio() ? &g_device : nullptr;
}
ALCboolean alcCloseDevice(ALCdevice*) { stopAudio(); return ALC_TRUE; }
ALCcontext* alcCreateContext(ALCdevice* device,const ALCint*) {
    if(!device){ setAlcError(ALC_INVALID_DEVICE); return nullptr; }
    g_context.device=device; return &g_context;
}
ALCboolean alcMakeContextCurrent(ALCcontext* ctx) { g_current_context=ctx; return ALC_TRUE; }
void alcDestroyContext(ALCcontext* ctx) { if(ctx==g_current_context) g_current_context=nullptr; }
ALCcontext* alcGetCurrentContext() { return g_current_context; }
ALCdevice* alcGetContextsDevice(ALCcontext* ctx) { return ctx ? ctx->device : nullptr; }
ALCenum alcGetError(ALCdevice*) { ALCenum e=g_alc_error; g_alc_error=ALC_NO_ERROR; return e; }
const ALCchar* alcGetString(ALCdevice*,ALCenum p) {
    if(p==0x1004 || p==0x1005 || p==0x1013) return "Nintendo Switch Audio";
    if(p==0x1006) return "";
    setAlcError(ALC_INVALID_ENUM); return "";
}
void alcGetIntegerv(ALCdevice*,ALCenum p,ALCsizei n,ALCint* v) {
    if(!v || n<=0) return;
    if(p==0x1000) v[0]=1;
    else if(p==0x1001) v[0]=1;
    else { v[0]=0; setAlcError(ALC_INVALID_ENUM); }
}
ALCboolean alcIsExtensionPresent(ALCdevice*,const ALCchar*) { return ALC_FALSE; }
void* alcGetProcAddress(ALCdevice*,const ALCchar*) { return nullptr; }

ALenum alGetError() { ALenum e=g_al_error; g_al_error=AL_NO_ERROR; return e; }
const ALchar* alGetString(ALenum p) {
    if(p==0xB001) return "NearChuckle_nx";
    if(p==0xB002) return "1.1";
    if(p==0xB003) return "Nintendo Switch OpenAL backend";
    if(p==0xB004) return "AL_EXT_FLOAT32";
    setError(AL_INVALID_ENUM); return "";
}
ALboolean alIsExtensionPresent(const ALchar* name) { return (name && std::strcmp(name,"AL_EXT_FLOAT32")==0) ? AL_TRUE : AL_FALSE; }

void alGenBuffers(ALsizei n,ALuint* ids) {
    if(n<0 || !ids){setError(AL_INVALID_VALUE);return;}
    mutexLock(&g_audio.lock);
    for(ALsizei i=0;i<n;i++){
        ALuint id=allocBufferId();
        if(!id){ids[i]=0;setError(AL_OUT_OF_MEMORY);continue;}
        g_buffers[id]=Buffer();g_buffers[id].used=true;ids[i]=id;
    }
    mutexUnlock(&g_audio.lock);
}
void alDeleteBuffers(ALsizei n,const ALuint* ids) {
    if(n<0 || (!ids&&n)){setError(AL_INVALID_VALUE);return;}
    mutexLock(&g_audio.lock);
    for(ALsizei i=0;i<n;i++) if(Buffer* b=getBuffer(ids[i])) {
        b->pcm.clear(); b->pcm.shrink_to_fit(); b->used=false;
    }
    mutexUnlock(&g_audio.lock);
}
ALboolean alIsBuffer(ALuint id) { return getBuffer(id) ? AL_TRUE : AL_FALSE; }

void alBufferData(ALuint id,ALenum fmt,const ALvoid* data,ALsizei size,ALsizei freq) {
    if(size<0 || freq<=0 || !data){setError(AL_INVALID_VALUE);return;}
    ALsizei ch=0,bytes=0;
    bool is_float=false;
    if(fmt==AL_FORMAT_MONO8||fmt==AL_FORMAT_STEREO8){bytes=1;ch=(fmt==AL_FORMAT_MONO8)?1:2;}
    else if(fmt==AL_FORMAT_MONO16||fmt==AL_FORMAT_STEREO16){bytes=2;ch=(fmt==AL_FORMAT_MONO16)?1:2;}
    else if(fmt==AL_FORMAT_MONO_FLOAT32||fmt==AL_FORMAT_STEREO_FLOAT32){bytes=4;ch=(fmt==AL_FORMAT_MONO_FLOAT32)?1:2;is_float=true;}
    else {
        if(g_buffer_log_count<16)
            compatLogFmt("AUDIO: unsupported alBufferData format=0x%x bytes=%d rate=%d",
                         static_cast<unsigned>(fmt), static_cast<int>(size), static_cast<int>(freq));
        setError(AL_INVALID_ENUM);
        return;
    }

    // size is the complete byte count. Do not multiply by channels again:
    // for stereo PCM the channel count is already part of the byte count.
    const size_t samples=static_cast<size_t>(size)/static_cast<size_t>(bytes);
    std::vector<int16_t> pcm(samples);
    const uint8_t* src=static_cast<const uint8_t*>(data);
    if(is_float) {
        const float* fsrc=static_cast<const float*>(data);
        for(size_t i=0;i<samples;i++)
            pcm[i]=static_cast<int16_t>(std::lrintf(std::clamp(fsrc[i],-1.0f,1.0f)*32767.0f));
    } else if(bytes==1) {
        for(size_t i=0;i<samples;i++)
            pcm[i]=static_cast<int16_t>((static_cast<int>(src[i])-128)<<8);
    } else {
        std::memcpy(pcm.data(),src,samples*sizeof(int16_t));
    }

    if(g_buffer_log_count<16) {
        int16_t peak=0;
        int64_t accum=0;
        for(size_t i=0;i<samples;i++) {
            const int v=std::abs(static_cast<int>(pcm[i]));
            if(v>peak) peak=static_cast<int16_t>(std::min(v,32767));
            accum+=static_cast<int64_t>(v);
        }
        const unsigned avg = samples ? static_cast<unsigned>(accum / samples) : 0;
        compatLogFmt("AUDIO: alBufferData[%u] fmt=0x%x bytes=%d rate=%d channels=%d peak=%d avg=%u",
                     g_buffer_log_count, static_cast<unsigned>(fmt),
                     static_cast<int>(size), static_cast<int>(freq), static_cast<int>(ch),
                     static_cast<int>(peak), avg);
        ++g_buffer_log_count;
    }
    mutexLock(&g_audio.lock);
    Buffer* b=getBuffer(id);
    if(!b) { mutexUnlock(&g_audio.lock); setError(AL_INVALID_NAME); return; }
    b->format=fmt;b->rate=freq;b->channels=ch;b->pcm.swap(pcm);
    mutexUnlock(&g_audio.lock);
}
void alGetBufferi(ALuint id,ALenum p,ALint* v) {
    Buffer* b=getBuffer(id);if(!b||!v){setError(AL_INVALID_VALUE);return;}
    if(p==0x2001)*v=static_cast<ALint>(b->pcm.size()/std::max(1,b->channels));
    else if(p==0x2002)*v=b->format;
    else if(p==0x2003)*v=b->rate;
    else if(p==0x2004)*v=b->channels;
    else if(p==0x200B)*v=(b->channels==1?8:16);
    else setError(AL_INVALID_ENUM);
}

void alGenSources(ALsizei n,ALuint* ids) {
    if(n<0||!ids){setError(AL_INVALID_VALUE);return;}
    mutexLock(&g_audio.lock);
    for(ALsizei i=0;i<n;i++){
        ALuint id=allocSourceId();
        if(!id){ids[i]=0;setError(AL_OUT_OF_MEMORY);continue;}
        g_sources[id]=Source();g_sources[id].used=true;ids[i]=id;
    }
    mutexUnlock(&g_audio.lock);
}
void alDeleteSources(ALsizei n,const ALuint* ids) {
    if(n<0||(!ids&&n)){setError(AL_INVALID_VALUE);return;}
    mutexLock(&g_audio.lock);
    for(ALsizei i=0;i<n;i++) if(Source* s=getSource(ids[i])) {resetSource(*s);s->used=false;}
    mutexUnlock(&g_audio.lock);
}
ALboolean alIsSource(ALuint id){return getSource(id)?AL_TRUE:AL_FALSE;}
void alSourcePlay(ALuint id){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    if(s->queue.empty()){mutexUnlock(&g_audio.lock);setError(AL_INVALID_OPERATION);return;}
    s->state=AL_PLAYING;
    if(g_play_log_count<16) {
        compatLogFmt("AUDIO: alSourcePlay[%u] source=%u queued=%u looping=%d gain=%.3f pitch=%.3f",
                     g_play_log_count, static_cast<unsigned>(id),
                     static_cast<unsigned>(s->queue.size()), s->looping ? 1 : 0,
                     s->gain, s->pitch);
        ++g_play_log_count;
    }
    mutexUnlock(&g_audio.lock);
}
void alSourcePause(ALuint id){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    if(s->state==AL_PLAYING)s->state=AL_PAUSED;
    mutexUnlock(&g_audio.lock);
}
void alSourceStop(ALuint id){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    s->state=AL_STOPPED;s->current=0;s->processed=0;s->sample_pos=0;
    mutexUnlock(&g_audio.lock);
}
void alSourceRewind(ALuint id){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    s->state=AL_INITIAL;s->current=0;s->processed=0;s->sample_pos=0;
    mutexUnlock(&g_audio.lock);
}
void alSourcei(ALuint id,ALenum p,ALint v){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    if(p==AL_LOOPING)s->looping=(v!=0);
    else if(p==AL_SOURCE_RELATIVE){
        s->relative=(v!=0);
        if(g_play_log_count<16)
            compatLogFmt("AUDIO: source=%u relative=%d", static_cast<unsigned>(id), s->relative ? 1 : 0);
    }
    else if(p==AL_BUFFER){
        if(v&&!getBuffer(v)){mutexUnlock(&g_audio.lock);setError(AL_INVALID_VALUE);return;}
        resetSource(*s);
        if(v)s->queue.push_back(v);
        s->state=AL_INITIAL;
    } else if(p!=AL_SOURCE_RELATIVE) {
        mutexUnlock(&g_audio.lock);setError(AL_INVALID_ENUM);return;
    }
    mutexUnlock(&g_audio.lock);
}
void alSourcef(ALuint id,ALenum p,ALfloat v){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    if(p==AL_PITCH)s->pitch=std::max(0.01f,v);
    else if(p==AL_GAIN)s->gain=std::max(0.0f,v);
    else if(p==AL_REFERENCE_DISTANCE)s->ref=std::max(0.001f,v);
    else if(p==AL_ROLLOFF_FACTOR)s->rolloff=std::max(0.0f,v);
    else if(p==AL_MAX_DISTANCE)s->maxdist=std::max(0.001f,v);
    else if(p==AL_SEC_OFFSET)s->sample_pos=std::max(0.0,double(v))*static_cast<double>(kRate);
    else {mutexUnlock(&g_audio.lock);setError(AL_INVALID_ENUM);return;}
    mutexUnlock(&g_audio.lock);
}
void alSource3f(ALuint id,ALenum p,ALfloat a,ALfloat b,ALfloat c){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s){mutexUnlock(&g_audio.lock);setError(AL_INVALID_NAME);return;}
    if(p==AL_POSITION){s->pos[0]=a;s->pos[1]=b;s->pos[2]=c;}
    else if(p!=AL_VELOCITY&&p!=AL_DIRECTION){
        mutexUnlock(&g_audio.lock);setError(AL_INVALID_ENUM);return;
    }
    mutexUnlock(&g_audio.lock);
}
void alSourcefv(ALuint id,ALenum p,const ALfloat*v){
    if(!v){setError(AL_INVALID_VALUE);return;}
    if(p==AL_POSITION||p==AL_VELOCITY||p==AL_DIRECTION)alSource3f(id,p,v[0],v[1],v[2]);
    else setError(AL_INVALID_ENUM);
}
void alSourceQueueBuffers(ALuint id,ALsizei n,const ALuint*v){
    Source*s=getSource(id);if(!s||n<0||(!v&&n)){setError(AL_INVALID_VALUE);return;}
    mutexLock(&g_audio.lock);
    for(ALsizei i=0;i<n;i++){
        if(!getBuffer(v[i])){setError(AL_INVALID_NAME);break;}
        s->queue.push_back(v[i]);
    }
    if(g_queue_log_count<16) {
        compatLogFmt("AUDIO: alSourceQueueBuffers[%u] source=%u n=%d total=%u",
                     g_queue_log_count, static_cast<unsigned>(id), static_cast<int>(n),
                     static_cast<unsigned>(s->queue.size()));
        ++g_queue_log_count;
    }
    mutexUnlock(&g_audio.lock);
}
void alSourceUnqueueBuffers(ALuint id,ALsizei n,ALuint*v){
    Source*s=getSource(id);if(!s||n<0||(!v&&n)){setError(AL_INVALID_VALUE);return;}
    mutexLock(&g_audio.lock);
    const size_t take=std::min(static_cast<size_t>(n),s->processed);
    if(g_unqueue_log_count<16) {
        compatLogFmt("AUDIO: alSourceUnqueueBuffers[%u] source=%u requested=%d processed=%u queue=%u",
                     g_unqueue_log_count, static_cast<unsigned>(id), static_cast<int>(n),
                     static_cast<unsigned>(s->processed), static_cast<unsigned>(s->queue.size()));
        ++g_unqueue_log_count;
    }
    for(size_t i=0;i<take;i++)v[i]=s->queue[i];
    if(take){s->queue.erase(s->queue.begin(),s->queue.begin()+static_cast<ptrdiff_t>(take));s->processed-=take;s->current=s->current>=take?s->current-take:0;}
    mutexUnlock(&g_audio.lock);
    if(take!=static_cast<size_t>(n))setError(AL_INVALID_VALUE);
}
void alGetSourcei(ALuint id,ALenum p,ALint*v){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s||!v){mutexUnlock(&g_audio.lock);setError(AL_INVALID_VALUE);return;}
    if(p==AL_SOURCE_STATE)*v=s->state;
    else if(p==AL_BUFFERS_QUEUED)*v=static_cast<ALint>(s->queue.size());
    else if(p==AL_BUFFERS_PROCESSED)*v=static_cast<ALint>(s->processed);
    else if(p==AL_SAMPLE_OFFSET)*v=static_cast<ALint>(s->sample_pos);
    else if(p==AL_BYTE_OFFSET)*v=static_cast<ALint>(s->sample_pos*2.0);
    else {mutexUnlock(&g_audio.lock);setError(AL_INVALID_ENUM);return;}
    mutexUnlock(&g_audio.lock);
}
void alGetSourcef(ALuint id,ALenum p,ALfloat*v){
    mutexLock(&g_audio.lock);
    Source*s=getSource(id);
    if(!s||!v){mutexUnlock(&g_audio.lock);setError(AL_INVALID_VALUE);return;}
    if(p==AL_PITCH)*v=s->pitch;
    else if(p==AL_GAIN)*v=s->gain;
    else if(p==AL_SEC_OFFSET)*v=static_cast<ALfloat>(s->sample_pos/static_cast<double>(kRate));
    else {mutexUnlock(&g_audio.lock);setError(AL_INVALID_ENUM);return;}
    mutexUnlock(&g_audio.lock);
}
void alListenerf(ALenum p,ALfloat v){
    if(p==AL_GAIN) {
        g_listener_gain=std::max(0.0f,v);
        if(g_listener_log_count<8) {
            compatLogFmt("AUDIO: alListenerf gain=%.3f", g_listener_gain);
            ++g_listener_log_count;
        }
    } else setError(AL_INVALID_ENUM);
}
void alListener3f(ALenum p,ALfloat a,ALfloat b,ALfloat c){
    if(p==AL_POSITION){g_listener_pos[0]=a;g_listener_pos[1]=b;g_listener_pos[2]=c;}
    else if(p!=AL_VELOCITY&&p!=AL_DIRECTION) setError(AL_INVALID_ENUM);
}
void alListenerfv(ALenum p,const ALfloat*v){
    if(!v){setError(AL_INVALID_VALUE);return;}
    if(p==AL_POSITION){g_listener_pos[0]=v[0];g_listener_pos[1]=v[1];g_listener_pos[2]=v[2];}
    else if(p!=AL_VELOCITY&&p!=AL_ORIENTATION) setError(AL_INVALID_ENUM);
}
void alDistanceModel(ALenum){} void alDopplerFactor(ALfloat){} void alDopplerVelocity(ALfloat){} void alSpeedOfSound(ALfloat){}
ALenum alGetEnumValue(const ALchar*n){
    if(!n)return 0;
    if(!std::strcmp(n,"AL_FORMAT_MONO8"))return AL_FORMAT_MONO8;
    if(!std::strcmp(n,"AL_FORMAT_MONO16"))return AL_FORMAT_MONO16;
    if(!std::strcmp(n,"AL_FORMAT_STEREO8"))return AL_FORMAT_STEREO8;
    if(!std::strcmp(n,"AL_FORMAT_STEREO16"))return AL_FORMAT_STEREO16;
    if(!std::strcmp(n,"AL_FORMAT_MONO_FLOAT32"))return AL_FORMAT_MONO_FLOAT32;
    if(!std::strcmp(n,"AL_FORMAT_STEREO_FLOAT32"))return AL_FORMAT_STEREO_FLOAT32;
    return 0;
}
void* alGetProcAddress(const ALchar*){return nullptr;}
ALboolean alIsEnabled(ALenum){return AL_FALSE;}
void alEnable(ALenum){} void alDisable(ALenum){} void alFinish(){} void alFlush(){}
}