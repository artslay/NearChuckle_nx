#include "config.h"
#include "compat/loader.h"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <string>
#include <algorithm>
#include <vector>
#include <sys/iosupport.h>
#include <switch/services/hid.h>
#include <switch/runtime/pad.h>

// SDL3's Android glue registers these callbacks with JNI_OnLoad. We invoke the
// registered native functions directly from the Switch main/render thread so
// the guest receives the same key/mouse events it would receive from SDL's
// Android Java frontend.
extern void* jniFindRegisteredNative(const char* name, int occurrence);
extern void compatProcessPendingFarCryProfile();
extern "C" void compatMarkFarCryMainLoopReady();
extern "C" bool compatProfileListRecentlyScanned();
extern "C" bool compatActivateFarCryProfile(const char* profile);

static CompatLayer g_compat = {};
static Mutex g_log_lock;
static FILE* g_log = nullptr;
static bool g_log_initialized = false;
static bool g_log_closed = false;
static LoadedSo* g_game_so = nullptr;

static u64 g_startup_timer_tick = 0;
static bool g_startup_timing_fs = false;
static bool g_startup_timing_stream = false;
static bool g_startup_timing_script = false;
static bool g_startup_timing_renderer = false;
static bool g_startup_timing_main_loop = false;

static void log_write(const char* text, bool force_flush);

void compatStartupTimerBegin() {
    g_startup_timer_tick = armGetSystemTick();
    g_startup_timing_fs = false;
    g_startup_timing_stream = false;
    g_startup_timing_script = false;
    g_startup_timing_renderer = false;
    g_startup_timing_main_loop = false;
}

static void startupTimingMaybeLog(const char* msg) {
    if (!g_startup_timer_tick || !msg)
        return;

    const char* label = nullptr;
    bool* once = nullptr;

    if (!g_startup_timing_fs && std::strstr(msg, "File System Initialization")) {
        label = "File System Initialization";
        once = &g_startup_timing_fs;
    } else if (!g_startup_timing_stream && std::strstr(msg, "Stream Engine Initialization")) {
        label = "Stream Engine Initialization";
        once = &g_startup_timing_stream;
    } else if (!g_startup_timing_script && std::strstr(msg, "Script System Initialization")) {
        label = "Script System Initialization";
        once = &g_startup_timing_script;
    } else if (!g_startup_timing_renderer && std::strstr(msg, "InitRenderer")) {
        label = "InitRenderer";
        once = &g_startup_timing_renderer;
    } else if (!g_startup_timing_main_loop && std::strstr(msg, "CXGame::Run: entered main game loop")) {
        label = "CXGame::Run: entered main game loop";
        once = &g_startup_timing_main_loop;
    }

    if (!label || !once)
        return;

    *once = true;
    const u64 elapsed_ms =
        (armGetSystemTick() - g_startup_timer_tick) * 1000 / armGetSystemTickFreq();

    char buf[256];
    std::snprintf(buf, sizeof(buf), "STARTUP TIMING: %s = %llu ms",
                  label, static_cast<unsigned long long>(elapsed_ms));
    log_write(buf, label[0] != 'C');
}


static bool g_boot_console = false;
static const devoptab_t* g_boot_stdout_dotab = nullptr;
static unsigned g_boot_ui_pending_lines = 0;
static unsigned g_log_pending_lines = 0;

// Dedicated PAK lookup diagnostic file is intentionally disabled. Keep the
// public logging entry point so existing PAK I/O call sites remain unchanged.
void compatPakLog(const char* fmt, ...) {
    (void)fmt;
}

static void compatPakLogClose() {}

static constexpr unsigned kBootUiUpdateBatch = 32;
static constexpr unsigned kLogFlushBatch = 128;

static void bootUiHeader() {
    if (!g_boot_console) return;
    consoleClear();
    std::printf("NearChuckle_nx | full startup log (until CXGame::Run main loop)\n");
    std::printf("------------------------------------------------------------------\n");
    consoleUpdate(nullptr);
}

static void bootUiWrite(const char* msg, bool force_update = false) {
    if (!g_boot_console || !msg || !*msg)
        return;

    // Printing is cheap enough to keep every startup line visible, but
    // consoleUpdate() is expensive on the Switch. Batch framebuffer updates
    // so the startup console does not turn into thousands of full redraws.
    std::printf("%s\n", msg);
    if (force_update || ++g_boot_ui_pending_lines >= kBootUiUpdateBatch) {
        consoleUpdate(nullptr);
        g_boot_ui_pending_lines = 0;
    }
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

static void log_write(const char* text, bool force_flush = false) {
    if (!text || g_log_closed)
        return;

    log_open();
    if (!g_log)
        return;

    std::fprintf(g_log, "%s\n", text);

    // fflush() for every diagnostic line is extremely expensive when the
    // log lives on the Switch filesystem. Keep the log buffered and flush
    // periodically, while still forcing a flush for important boundaries.
    if (force_flush || ++g_log_pending_lines >= kLogFlushBatch) {
        std::fflush(g_log);
        g_log_pending_lines = 0;
    }
}

static bool is_main_loop_marker(const char* msg) {
    return msg && std::strstr(msg, "CXGame::Run: entered main game loop") != nullptr;
}

static bool suppressSuccessfulPakDiag(const char* msg) {
    if (!msg || !*msg)
        return false;

    std::string normalized(msg);
    for (char& c : normalized) {
        if ((unsigned char)c == 92)
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    // PAK discovery/index construction is normal successful work and is no
    // longer useful in the runtime log. Keep only actual PAK failures/misses.
    return normalized.find("pak index:") != std::string::npos ||
           normalized.find("pak open diag:") != std::string::npos ||
           normalized.find("pak virtual open:") != std::string::npos ||
           normalized.find("pak exact match:") != std::string::npos ||
           normalized.find("pak basename match:") != std::string::npos ||
           normalized.find("pak dir ready:") != std::string::npos;
}

static bool suppressCompatNoise(const char* msg) {
    if (!msg || !*msg)
        return false;

    std::string normalized(msg);
    for (char& c : normalized) {
        if ((unsigned char)c == 92)
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    // These diagnostics are either high-frequency compatibility chatter or
    // known optional texture lookups. Suppressing them must not alter the
    // underlying filesystem/PAK behavior.
    const bool optionalMapTextureMiss =
        normalized.find("fopen fail:") == 0 &&
        (normalized.find("/gui/map_player.") != std::string::npos ||
         normalized.find("/textures/gui/map_player.") != std::string::npos ||
         normalized.find("/gui/map_vehicle.") != std::string::npos ||
         normalized.find("/textures/gui/map_vehicle.") != std::string::npos ||
         normalized.find("/gui/map_building.") != std::string::npos ||
         normalized.find("/textures/gui/map_building.") != std::string::npos ||
         normalized.find("/gui/map_unknown.") != std::string::npos ||
         normalized.find("/textures/gui/map_unknown.") != std::string::npos);

    // Low-value loader/graphics diagnostics. Keep actual failures/warnings,
    // but hide successful per-ELF bookkeeping and per-draw buffer chatter.
    const bool elfBookkeeping =
        normalized.find("elf: loading ") == 0 ||
        normalized.find("elf: stage alloc ok") == 0 ||
        normalized.find("elf: stage zeroed") == 0 ||
        normalized.find("elf: seg[") == 0 ||
        normalized.find("elf: dyn:") == 0 ||
        normalized.find("elf: so built ") == 0 ||
        normalized.find("elf: registered") == 0 ||
        normalized.find("elf: rela ") == 0 ||
        normalized.find("elf: jmprel ") == 0 ||
        normalized.find("elf: strtab/symtab copied") == 0 ||
        normalized.find("elf: dt_init fn deferred") == 0 ||
        (normalized.find("elf: ") == 0 &&
         (normalized.find("constructors deferred") != std::string::npos ||
          normalized.find("loaded ok ") != std::string::npos ||
          normalized.find("process-code copy complete") != std::string::npos));

    const bool glDrawNoise =
        normalized.find("gl buffer data") == 0 ||
        normalized.find("gl vertex upload") == 0 ||
        normalized.find("gl draw") == 0;

    const bool switchTouchNoise =
        normalized.find("switch touch:") == 0;

    const bool audioNoise =
        normalized.find("audio:") == 0 ||
        normalized.find("bink audio:") == 0;

    return normalized.find("pak mem trace") == 0 ||
           normalized.find("farcry getfilesize") == 0 ||
           normalized.find("opendir ") == 0 ||
           normalized.find("pak virtual") == 0 ||
           normalized.find("switch input") == 0 ||
           switchTouchNoise ||
           audioNoise ||
           normalized.find("sdl: swap heartbeat[") == 0 ||
           normalized.find("pak caf hit:") == 0 ||
           normalized.find("texture format '.tga' is deprecated") != std::string::npos ||
           elfBookkeeping ||
           glDrawNoise ||
           optionalMapTextureMiss;
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

namespace {

struct SwitchKeyBinding {
    u64 mask;
    int androidKeycode;
    const char* name;
};

static constexpr SwitchKeyBinding kSwitchKeyBindings[] = {
    // Menu / common actions.
    {HidNpadButton_A,          66,  "A -> ENTER"},
    {HidNpadButton_B,          62,  "B -> SPACE"},
    {HidNpadButton_X,          46,  "X -> R"},
    {HidNpadButton_Y,          33,  "Y -> E"},
    {HidNpadButton_L,          31,  "L -> C"},
    {HidNpadButton_R,          113, "R -> LCTRL"},
    {HidNpadButton_Plus,       111, "PLUS -> ESCAPE"},
    {HidNpadButton_Minus,       61, "MINUS -> TAB"},

    // Digital pad stays available as Android DPAD keys for menu navigation.
    {HidNpadButton_Up,          19, "DPAD UP"},
    {HidNpadButton_Down,        50, "DPAD DOWN -> V"},
    {HidNpadButton_Left,        21, "DPAD LEFT"},
    {HidNpadButton_Right,       22, "DPAD RIGHT"},

    // Left stick -> classic Far Cry WASD movement. Each direction is sampled
    // as a digital key so it also works with the original Android input path.
    {HidNpadButton_StickLUp,    51, "STICK L UP -> W"},
    {HidNpadButton_StickLDown,  47, "STICK L DOWN -> S"},
    {HidNpadButton_StickLLeft,  29, "STICK L LEFT -> A"},
    {HidNpadButton_StickLRight, 32, "STICK L RIGHT -> D"},
};

static bool g_switch_input_started = false;
static bool g_switch_pad_initialized = false;
static bool g_switch_touch_initialized = false;
static bool g_switch_touch_down = false;
static PadState g_switch_pad = {};
static u64 g_switch_input_previous = 0;
static float g_switch_touch_x = 0.0f;
static float g_switch_touch_y = 0.0f;
static float g_switch_cursor_virtual_x = 400.0f;
static float g_switch_cursor_virtual_y = 300.0f;
static void* g_sdl_key_down = nullptr;
static void* g_sdl_key_up = nullptr;
static void* g_sdl_mouse = nullptr;

static void switchEmitKey(void* fn_ptr, int keycode, const char* name, bool down) {
    if (!fn_ptr)
        return;

    using KeyFn = void (*)(void*, void*, int);
    const KeyFn fn = reinterpret_cast<KeyFn>(fn_ptr);
    fn(compatGet()->env_outer,
       reinterpret_cast<void*>(0x1001),
       keycode);

    compatLogFmt("SWITCH INPUT: %s %s", down ? "DOWN" : "UP", name);
}

static void switchEmitMouse(void* fn_ptr, int button, int action,
                              float x, float y, bool relative) {
    if (!fn_ptr)
        return;

    using MouseFn = void (*)(void*, void*, int, int, float, float, jboolean);
    const MouseFn fn = reinterpret_cast<MouseFn>(fn_ptr);
    fn(compatGet()->env_outer,
       reinterpret_cast<void*>(0x1001),
       button, action, x, y, relative ? JNI_TRUE : JNI_FALSE);
}

static void switchEmitRelativeMouse(void* fn_ptr, float dx, float dy) {
    if (!fn_ptr || (dx == 0.0f && dy == 0.0f))
        return;

    using MouseFn = void (*)(void*, void*, int, int, float, float, jboolean);
    const MouseFn fn = reinterpret_cast<MouseFn>(fn_ptr);
    fn(compatGet()->env_outer,
       reinterpret_cast<void*>(0x1001),
       0, 2, dx, dy, JNI_TRUE); // MotionEvent.ACTION_MOVE
}

static void switchInputResolveCallbacks() {
    if (g_sdl_key_down && g_sdl_key_up && g_sdl_mouse)
        return;

    g_sdl_key_down = jniFindRegisteredNative("onNativeKeyDown", 0);
    g_sdl_key_up = jniFindRegisteredNative("onNativeKeyUp", 0);
    g_sdl_mouse = jniFindRegisteredNative("onNativeMouse", 0);

    if (!g_switch_input_started) {
        g_switch_input_started = true;
        compatLogFmt("SWITCH INPUT: SDL callbacks keyDown=%p keyUp=%p mouse=%p",
                     g_sdl_key_down, g_sdl_key_up, g_sdl_mouse);
    }
}

static bool switchSelectFarCryProfileAtVirtualPoint(float x, float y) {
    if (!compatProfileListRecentlyScanned())
        return false;

    // Exact geometry from the original Far Cry Profiles.lua.
    constexpr float kListLeft = 205.0f;
    constexpr float kListTop = 147.0f;
    constexpr float kListWidth = 570.0f;
    constexpr float kListHeight = 238.0f;
    constexpr float kItemHeight = 18.0f;

    if (x < kListLeft || x >= kListLeft + kListWidth ||
        y < kListTop || y >= kListTop + kListHeight)
        return false;

    const int row = static_cast<int>((y - kListTop) / kItemHeight);
    if (row < 0)
        return false;

    std::string profileDir = config.data_root[0]
        ? std::string(config.data_root) + "/Profiles/Player"
        : std::string("/switch/NearChuckle_nx/game/Profiles/Player");

    DIR* dir = ::opendir(profileDir.c_str());
    if (!dir)
        return false;

    std::vector<std::string> profiles;
    while (dirent* ent = ::readdir(dir)) {
        if (!ent->d_name[0])
            continue;

        std::string name(ent->d_name);
        const std::string lower = [&]() {
            std::string v = name;
            for (char& c : v)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return v;
        }();

        const std::string suffix = "_system.cfg";
        if (lower.size() <= suffix.size() ||
            lower.compare(lower.size() - suffix.size(),
                          suffix.size(), suffix) != 0)
            continue;

        name.resize(name.size() - suffix.size());
        if (!name.empty())
            profiles.push_back(name);
    }
    ::closedir(dir);

    std::sort(profiles.begin(), profiles.end(),
              [](const std::string& a, const std::string& b) {
                  std::string al = a;
                  std::string bl = b;
                  for (char& c : al)
                      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  for (char& c : bl)
                      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  return al < bl;
              });

    if (row >= static_cast<int>(profiles.size()))
        return false;

    const std::string& selected = profiles[static_cast<size_t>(row)];
    if (!selected.empty() && compatActivateFarCryProfile(selected.c_str())) {
        compatLogFmt("PROFILE TOUCH SELECT: row=%d name=%s",
                     row, selected.c_str());
        return true;
    }

    return false;
}

static void pollSwitchInputInternal() {
    switchInputResolveCallbacks();
    if (!g_sdl_key_down && !g_sdl_key_up && !g_sdl_mouse)
        return;

    if (!g_switch_pad_initialized) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_switch_pad);
        g_switch_pad_initialized = true;
        compatLog("SWITCH INPUT: libnx PadState initialized");
    }

    if (g_sdl_mouse && !g_switch_touch_initialized) {
        hidInitializeTouchScreen();
        g_switch_touch_initialized = true;
        compatLog("SWITCH TOUCH: libnx TouchScreen initialized");
    }

    padUpdate(&g_switch_pad);
    const u64 held = padGetButtons(&g_switch_pad);

    if (g_sdl_key_down && g_sdl_key_up) {
        for (const SwitchKeyBinding& b : kSwitchKeyBindings) {
            const bool was_down = (g_switch_input_previous & b.mask) != 0;
            const bool is_down = (held & b.mask) != 0;
            if (was_down == is_down)
                continue;

            switchEmitKey(is_down ? g_sdl_key_down : g_sdl_key_up,
                          b.androidKeycode, b.name, is_down);
        }
    }

    // ZR/ZL are the two mouse buttons in the PC control scheme. SDL's Android
    // glue treats the integer argument as the complete MotionEvent button-state
    // bitmask, not just the button that changed. Keep both held-button bits in
    // that state so pressing/releasing the second mouse button cannot corrupt
    // SDL's internal last_state tracking.
    if (g_sdl_mouse) {
        const bool old_zr = (g_switch_input_previous & HidNpadButton_ZR) != 0;
        const bool new_zr = (held & HidNpadButton_ZR) != 0;
        const bool old_zl = (g_switch_input_previous & HidNpadButton_ZL) != 0;
        const bool new_zl = (held & HidNpadButton_ZL) != 0;

        const int new_mouse_state = (new_zr ? 1 : 0) | (new_zl ? 2 : 0);

        if (old_zr != new_zr)
            switchEmitMouse(g_sdl_mouse, new_mouse_state,
                            new_zr ? 0 : 1, 0.0f, 0.0f, true);
        if (old_zl != new_zl)
            switchEmitMouse(g_sdl_mouse, new_mouse_state,
                            new_zl ? 0 : 1, 0.0f, 0.0f, true);

        const bool rs_left  = (held & HidNpadButton_StickRLeft)  != 0;
        const bool rs_right = (held & HidNpadButton_StickRRight) != 0;
        const bool rs_up    = (held & HidNpadButton_StickRUp)    != 0;
        const bool rs_down  = (held & HidNpadButton_StickRDown)  != 0;
        if (rs_left || rs_right || rs_up || rs_down) {
            const float dx = rs_right ? 10.0f : (rs_left ? -10.0f : 0.0f);
            const float dy = rs_down ? 10.0f : (rs_up ? -10.0f : 0.0f);
            switchEmitRelativeMouse(g_sdl_mouse, dx, dy);
            // CSDLMouse defaults to sensitivity=0.2 and multiplies the
            // resulting delta by 4, so each raw SDL relative unit moves the
            // Far Cry virtual cursor by 0.8 pixels.
            g_switch_cursor_virtual_x =
                std::max(0.0f, std::min(799.0f,
                    g_switch_cursor_virtual_x + dx * 0.8f));
            g_switch_cursor_virtual_y =
                std::max(0.0f, std::min(599.0f,
                    g_switch_cursor_virtual_y + dy * 0.8f));
        }
    }

    // Switch touch -> move the Far Cry virtual cursor to the touch point
    // and perform a normal left click there.
    //
    // CSDLMouse is permanently in relative mode. Its default sensitivity is
    // 0.2 and it multiplies the resulting delta by 4 when updating the
    // 800x600 virtual cursor, so one raw SDL relative unit = 0.8 virtual px.
    if (g_sdl_mouse && g_switch_touch_initialized) {
        HidTouchScreenState touch = {};
        const size_t touch_samples = hidGetTouchScreenStates(&touch, 1);
        const bool touching = touch_samples > 0 && touch.count > 0;

        if (touching && !g_switch_touch_down) {
            const float target_x = std::max(0.0f, std::min(
                799.0f,
                static_cast<float>(touch.touches[0].x) * 799.0f /
                    std::max(1.0f, static_cast<float>(config.screen_width - 1))));
            const float target_y = std::max(0.0f, std::min(
                599.0f,
                static_cast<float>(touch.touches[0].y) * 599.0f /
                    std::max(1.0f, static_cast<float>(config.screen_height - 1))));

            const float dx = (target_x - g_switch_cursor_virtual_x) / 0.8f;
            const float dy = (target_y - g_switch_cursor_virtual_y) / 0.8f;

            if (dx != 0.0f || dy != 0.0f)
                switchEmitRelativeMouse(g_sdl_mouse, dx, dy);

            g_switch_cursor_virtual_x = target_x;
            g_switch_cursor_virtual_y = target_y;
            g_switch_touch_down = true;
            g_switch_touch_x = target_x;
            g_switch_touch_y = target_y;

            // Profile selection is a compatibility-only layer on top of the
            // already-working mouse click. It does not alter touch/mouse events.
            (void)switchSelectFarCryProfileAtVirtualPoint(
                target_x, target_y);

            switchEmitMouse(g_sdl_mouse, 1, 0, 0.0f, 0.0f, true); // LMB down
        } else if (!touching && g_switch_touch_down) {
            g_switch_touch_down = false;
            switchEmitMouse(g_sdl_mouse, 0, 1, 0.0f, 0.0f, true); // LMB up
        }
    }
    g_switch_input_previous = held;
}

} // namespace

void compatPollSwitchInput() {
    pollSwitchInputInternal();

    // Profile creation is initiated from the guest filesystem callback.
    // Complete the system-config serialization only after that callback has
    // returned, using the real IConsole::DumpCVars() path.
    compatProcessPendingFarCryProfile();
}

void compatLog(const char* msg) {
    const bool main_loop = is_main_loop_marker(msg);
    if (main_loop)
        compatMarkFarCryMainLoopReady();

    const bool suppress_pak_success = suppressSuccessfulPakDiag(msg);
    const bool suppress_noise = suppressCompatNoise(msg);
    const bool suppress_gl_texture_diag =
        msg && (std::strncmp(msg, "GL TEX ", 7) == 0 ||
                std::strncmp(msg, "GL PIXELSTORE", 13) == 0);

    mutexLock(&g_log_lock);

    // Emit a one-time elapsed timestamp for the important CryEngine startup
    // milestones. This runs inside the existing logger lock and adds no I/O
    // beyond the same buffered log write path.
    startupTimingMaybeLog(msg);

    // High-frequency compatibility diagnostics are omitted from both the
    // startup console and file log, but the underlying operations still run.
    if (!suppress_pak_success && !suppress_noise)
        bootUiWrite(msg, main_loop);

    if (!suppress_pak_success &&
        !suppress_noise &&
        !suppressCompatShaderDiag(msg) &&
        !suppress_gl_texture_diag)
        log_write(msg, main_loop);

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
    bootUiWrite(msg, main_loop);
    log_write(msg, main_loop);

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
        if (g_log) {
            std::fflush(g_log);
            g_log_pending_lines = 0;
        }
    }
    mutexUnlock(&g_log_lock);
}

void compatLogClose() {
    mutexLock(&g_log_lock);
    log_close_locked();
    mutexUnlock(&g_log_lock);
    compatPakLogClose();
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
