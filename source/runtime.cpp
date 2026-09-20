#include "compat/loader.h"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static CompatLayer g_compat = {};
static Mutex g_log_lock;
static FILE* g_log = nullptr;
static LoadedSo* g_game_so = nullptr;

static char g_android_tls[1024] __attribute__((aligned(16)));
static char g_android_tls_sub[512] __attribute__((aligned(16)));

static void log_open() {
    if (g_log)
        return;
    g_log = std::fopen("/switch/NearChuckle_nx/nearchuckle_debug.log", "w");
}

static void log_write(const char* text) {
    if (!text)
        return;

    log_open();
    if (!g_log)
        return;

    std::fprintf(g_log, "%s\n", text);
    std::fflush(g_log);
}

CompatLayer* compatGet() {
    return &g_compat;
}

void compatLog(const char* msg) {
    mutexLock(&g_log_lock);
    log_write(msg);
    mutexUnlock(&g_log_lock);
}

void compatLogFmt(const char* fmt, ...) {
    char buf[1024];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    compatLog(buf);
}

void compatLogRaw(const char* msg) {
    log_write(msg);
}

void compatLogFlush() {
    mutexLock(&g_log_lock);
    log_open();
    if (g_log)
        std::fflush(g_log);
    mutexUnlock(&g_log_lock);
}

void compatUiLog(const char*) {}
void compatUiSetPct(int) {}

void androidTlsInstall() {
    std::memset(g_android_tls, 0, sizeof(g_android_tls));
    std::memset(g_android_tls_sub, 0, sizeof(g_android_tls_sub));

    *(void**)(g_android_tls + 0x00) = g_android_tls;
    *(void**)(g_android_tls + 0x28) = g_android_tls_sub;

    const uint64_t tp = reinterpret_cast<uint64_t>(g_android_tls);
    asm volatile("msr tpidr_el0, %0" :: "r"(tp) : "memory");
}

void androidTlsInstallThread() {
    uint8_t* block = static_cast<uint8_t*>(std::calloc(1, 1024));
    if (!block)
        return;

    *(void**)(block + 0x00) = block;
    *(void**)(block + 0x28) = block + 512;

    const uint64_t tp = reinterpret_cast<uint64_t>(block);
    asm volatile("msr tpidr_el0, %0" :: "r"(tp) : "memory");
}

void vnxSetGameSo(LoadedSo* so) {
    g_game_so = so;
}

void* compatFindGameSym(const char* name) {
    return g_game_so ? g_game_so->findSym(name) : nullptr;
}

// OpenAL Soft's C++ TLS wrapper for ALCcontext::sLocalContext has a weak
// definition in the Android build. On Switch there is no guest dynamic linker
// to provide that wrapper, but the variable is trivially zero-initialized, so
// its initialization thunk can safely be a no-op.
extern "C" void near_openal_tls_local_context_init() {}

// The Android/Bionic ABI uses __cxa_thread_atexit_impl to register TLS
// destructors. The guest pthread layer does not currently emulate per-thread
// C++ destructor lists, so report successful registration and keep the object
// alive until process exit. This is sufficient for the current OpenAL Soft
// ThreadCtx object and avoids branching through an unresolved weak import.
extern "C" int near_openal_cxa_thread_atexit(void (*dtor)(void*), void* obj, void* dso) {
    (void)dtor;
    (void)obj;
    (void)dso;
    return 0;
}

void compatMarkSplashDone() {}
void compatMarkPastLoading() {}

void compatAudioSetAssetsDir(const char*) {}
void compatAudioWarmup() {}
void compatAudioPlayMusic(const char*, bool) {}
void compatAudioPreloadMusic(const char*) {}
void compatAudioStopMusic() {}
void compatAudioPauseMusic() {}
void compatAudioResumeMusic() {}
void compatAudioRewindMusic() {}
void compatAudioSetMusicVolume(float) {}
bool compatAudioMusicPlaying() { return false; }
void compatAudioPreloadEffect(const char*) {}
void compatAudioUnloadEffect(const char*) {}
int compatAudioPlayEffect(const char*, bool, float) { return -1; }
void compatAudioSetEffectVolume(int, float) {}
void compatAudioStopEffect(int) {}
void compatAudioPauseEffect(int) {}
void compatAudioResumeEffect(int) {}
void compatAudioStopAllEffects() {}
void compatAudioPauseAllEffects() {}
void compatAudioResumeAllEffects() {}
void compatAudioSetEffectsVolume(float) {}
void compatAudioMuteEffectsFor(int) {}
void compatAudioSetEffectRate(int, float) {}
float compatAudioGetMusicVolume() { return 1.0f; }
float compatAudioGetEffectsVolume() { return 1.0f; }
