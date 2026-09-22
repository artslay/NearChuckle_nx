#include "compat/loader.h"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static CompatLayer g_compat = {};
static Mutex g_log_lock;
static FILE* g_log = nullptr;
static bool g_log_initialized = false;
static bool g_log_closed = false;
static LoadedSo* g_game_so = nullptr;

static bool g_boot_console = false;
static char g_ui_lines[18][128] = {};
static int g_ui_line_count = 0;

static bool bootUiInteresting(const char* msg) {
    if (!msg || !*msg) return false;
    static const char* const keys[] = {
        "=== NearChuckle_nx start ===", "data_root=", "lib_dir=",
        "mesa_driver=", "resolution=", "shader source", "PAK PROBE",
        "pak DIR", "SHADER fopen CALL", "fopen FAIL: Shaders",
        "bind: fopen", "bind: fopen64", "bind: opendir",
        "bind: _findfirst64", "Starting Far Cry", "SDL:", "EGL:",
        "GL context:", "ERROR:", "FATAL:", "UNRECOVERED FAULT"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (std::strstr(msg, keys[i])) return true;
    return false;
}

static void bootUiRender() {
    if (!g_boot_console) return;
    consoleClear();
    std::printf("NearChuckle_nx | startup / shader diagnostics\n");
    std::printf("------------------------------------------------------------\n");
    for (int i = 0; i < g_ui_line_count; ++i)
        std::printf("%s\n", g_ui_lines[i]);
    consoleUpdate(nullptr);
}

void compatUiInit() {
    if (g_boot_console) return;
    consoleInit(nullptr);
    g_boot_console = true;
    g_ui_line_count = 0;
    bootUiRender();
}

void compatUiShutdown() {
    if (!g_boot_console) return;
    consoleUpdate(nullptr);
    consoleExit(nullptr);
    g_boot_console = false;
}

static char g_android_tls[1024] __attribute__((aligned(16)));
static char g_android_tls_sub[512] __attribute__((aligned(16)));

static void log_open() {
    if (g_log || g_log_closed)
        return;

    const char* mode = g_log_initialized ? "a" : "w";
    g_log = std::fopen("/switch/NearChuckle_nx/nearchuckle_debug.log", mode);
    if (g_log)
        g_log_initialized = true;
}

static void log_write(const char* text) {
    if (!text || g_log_closed)
        return;

    log_open();
    if (!g_log)
        return;

    std::fprintf(g_log, "%s\n", text);
    std::fflush(g_log);
}

static bool is_main_loop_marker(const char* msg) {
    return msg && std::strstr(msg, "CXGame::Run: entered main game loop") != nullptr;
}

static void log_close_locked() {
    if (g_log) {
        std::fflush(g_log);
        std::fclose(g_log);
        g_log = nullptr;
    }
    g_log_closed = true;
}

CompatLayer* compatGet() {
    return &g_compat;
}

void compatLog(const char* msg) {
    mutexLock(&g_log_lock);

    const bool main_loop_marker = is_main_loop_marker(msg);
    log_write(msg);

    // Once CryEngine has entered its real main loop, stop all startup logging.
    // Do this after writing the marker itself, and never reopen the file again.
    if (main_loop_marker) {
        log_close_locked();
    } else if (g_boot_console && bootUiInteresting(msg)) {
        if (g_ui_line_count < 18) {
            std::snprintf(g_ui_lines[g_ui_line_count],
                          sizeof(g_ui_lines[g_ui_line_count]), "%s", msg ? msg : "");
            ++g_ui_line_count;
        } else {
            for (int i = 1; i < 18; ++i)
                std::memmove(g_ui_lines[i - 1], g_ui_lines[i],
                             sizeof(g_ui_lines[i - 1]));
            std::snprintf(g_ui_lines[17], sizeof(g_ui_lines[17]),
                          "%s", msg ? msg : "");
        }
        bootUiRender();
    }

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
    mutexLock(&g_log_lock);
    const bool main_loop_marker = is_main_loop_marker(msg);
    log_write(msg);
    if (main_loop_marker)
        log_close_locked();
    mutexUnlock(&g_log_lock);
}

void compatLogFlush() {
    mutexLock(&g_log_lock);
    if (!g_log_closed) {
        log_open();
        if (g_log)
            std::fflush(g_log);
    }
    mutexUnlock(&g_log_lock);
}

void compatLogClose() {
    mutexLock(&g_log_lock);
    log_close_locked();
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
