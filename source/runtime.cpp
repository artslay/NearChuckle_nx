#include "compat/loader.h"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <sys/iosupport.h>

static CompatLayer g_compat = {};
static Mutex g_log_lock;
static FILE* g_log = nullptr;
static bool g_log_initialized = false;
static bool g_log_closed = false;
static LoadedSo* g_game_so = nullptr;

extern "C" void compatFrameDebugClose();

static bool g_boot_console = false;
static const devoptab_t* g_boot_stdout_dotab = nullptr;

static void bootUiHeader() {
    if (!g_boot_console) return;
    consoleClear();
    std::printf("NearChuckle_nx | full startup log (until CXGame::Run main loop)\n");
    std::printf("------------------------------------------------------------------\n");
    consoleUpdate(nullptr);
}

static void bootUiWrite(const char* msg) {
    if (!g_boot_console || !msg || !*msg)
        return;

    // Mirror every compatibility log line to the active Switch console.
    // Do not filter or keep only a small ring buffer: the console should show
    // the complete startup stream in order until the main-loop marker.
    std::printf("%s\n", msg);
    consoleUpdate(nullptr);
}

void compatUiInit() {
    if (g_boot_console) return;
    // libnx consoleInit() permanently installs its console write backend into
    // stdout. Save the previous device so the game cannot keep calling the
    // deinitialized software-console renderer after compatUiShutdown().
    g_boot_stdout_dotab = devoptab_list[STD_OUT];
    consoleInit(nullptr);
    g_boot_console = true;
    bootUiHeader();
}

void compatUiShutdown() {
    if (!g_boot_console) return;
    consoleUpdate(nullptr);
    consoleExit(nullptr);

    // consoleExit() deinitializes the renderer but libnx 4.12 does not restore
    // devoptab_list[STD_OUT]. Restore the original stdout backend ourselves so
    // any later guest/host printf cannot enter ConsoleSwRenderer_drawChar() with
    // a NULL framebuffer after the startup console has been shut down.
    devoptab_list[STD_OUT] = g_boot_stdout_dotab;
    g_boot_stdout_dotab = nullptr;
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

static bool suppressCompatShaderDiag(const char* msg) {
    if (!msg || !*msg)
        return false;

    std::string normalized(msg);
    for (char& c : normalized) {
        if ((unsigned char)c == 92)
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    // Keep the one-time preparation result visible. These messages are
    // compatibility diagnostics too, but they are needed to distinguish a
    // successful CommonSubroutines materialization from a failed directory scan.
    const bool keepShaderPrepDiag =
        normalized.find("shader common standalone:") != std::string::npos ||
        normalized.find("shader prep dir:") != std::string::npos ||
        normalized.find("shader prep tree:") != std::string::npos;

    if (keepShaderPrepDiag)
        return false;

    const bool shaderPath = normalized.find("shaders/") != std::string::npos;
    const bool shaderArchive = normalized.find("shaders.pak") != std::string::npos;
    const bool shaderDiagWord =
        normalized.find("shader source files:") != std::string::npos ||
        normalized.find("shader dir:") != std::string::npos ||
        normalized.find("shader root fallback:") != std::string::npos ||
        normalized.find("shader common") != std::string::npos ||
        normalized.find("shader cache") != std::string::npos;

    if (!(shaderPath || shaderArchive || shaderDiagWord))
        return false;

    // Suppress compatibility-layer lookup/path diagnostics only. CryEngine's
    // own warnings/errors are retained because they do not use these prefixes.
    return normalized.find("pak exact miss:") != std::string::npos ||
           normalized.find("pak basename match:") != std::string::npos ||
           normalized.find("pak basename not found:") != std::string::npos ||
           normalized.find("pak basename ambiguous:") != std::string::npos ||
           normalized.find("realpath result:") != std::string::npos ||
           normalized.find("realpath pak exact:") != std::string::npos ||
           normalized.find("realpath shader cache bypass:") != std::string::npos ||
           normalized.find("fopen fail:") != std::string::npos ||
           normalized.find("fopen casefix:") != std::string::npos ||
           normalized.find("fopen fcdata:") != std::string::npos ||
           normalized.find("pak fopen request:") != std::string::npos ||
           normalized.find("opendir ") != std::string::npos ||
           normalized.find("readdir[") != std::string::npos ||
           normalized.find("readdir64[") != std::string::npos ||
           normalized.find("pak dir ready:") != std::string::npos ||
           normalized.find("shader source files:") != std::string::npos ||
           normalized.find("shader dir:") != std::string::npos ||
           normalized.find("shader root fallback:") != std::string::npos ||
           normalized.find("shader common") != std::string::npos ||
           normalized.find("shader cache") != std::string::npos;
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
    const bool main_loop = is_main_loop_marker(msg);

    mutexLock(&g_log_lock);

    // The Switch startup console is intentionally the complete live log until
    // CXGame::Run enters the main game loop. Shader diagnostics are still
    // filtered from the file log below when appropriate, but never from the
    // visible console stream.
    bootUiWrite(msg);

    if (!suppressCompatShaderDiag(msg))
        log_write(msg);

    // Keep the file log open after the main-loop marker so we can capture
    // the first real CXGame::Update()/RenderEnd() activity. Only the visible
    // startup console is handed back to the game at the marker.

    mutexUnlock(&g_log_lock);

    // Return ownership of the framebuffer to the real SDL/EGL game window
    // immediately after the marker has been displayed. From this point onward
    // only the startup console is closed; nearchuckle_debug.log remains active
    // until Far Cry returns so the first game-frame path is captured.
    if (main_loop)
        compatUiShutdown();
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
    const bool main_loop = is_main_loop_marker(msg);

    mutexLock(&g_log_lock);
    bootUiWrite(msg);
    log_write(msg);

    if (main_loop) {
        log_close_locked();

    }

    mutexUnlock(&g_log_lock);

    if (main_loop)
        compatUiShutdown();
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
