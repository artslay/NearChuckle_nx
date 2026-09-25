#include "config.h"
#include "compat/loader.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

extern void androidTlsInstall();
extern void vnxSetGameSo(LoadedSo* so);
extern void compatUiInit();
extern void compatUiShutdown();
extern void* jniFindRegisteredNative(const char* name, int occurrence);

struct SoFile {
    std::string path;
    std::string name;
    uint64_t size;
};

static bool is_so_name(const char* name) {
    if (!name || !*name)
        return false;

    const size_t len = std::strlen(name);
    return len > 3 && !std::strcmp(name + len - 3, ".so");
}

// These are Android-side helpers from the original application package.
// They are not part of the Switch runtime: Mesa provides the native GL/Vulkan
// stack, while VNX provides the Android compatibility layer and file handling.
static bool skip_android_helper(const char* name) {
    if (!name) return false;

    static const char* const skip[] = {
        "libdriverloader.so",
        "libGL.so",
        "libXRenderNULL.so",
        "libfile_redirect_hook.so",
        "libmain_hook.so",
        "libgsl_alloc_hook.so",
        "libhook_impl.so",
        nullptr
    };

    for (size_t i = 0; skip[i]; ++i) {
        if (!std::strcmp(name, skip[i]))
            return true;
    }
    return false;
}

static int library_priority(const std::string& name) {
    // Load the guest C++ ABI and low-level runtime libraries before the CryEngine
    // libraries that import their symbols.
    if (name == "libc++_shared.so") return 0;
    if (name == "libogg.so")        return 10;
    if (name == "libvorbis.so")     return 11;
    if (name == "libvorbisenc.so")  return 12;
    if (name == "libvorbisfile.so") return 13;
    if (name == "libopenal.so")     return 20;
    if (name == "libSDL3.so")       return 21;
    if (name == "libXRenderOGL.so") return 90;
    if (name == "libFarCry.so")     return 100;
    return 50;
}

static std::vector<SoFile> find_guest_libraries() {
    std::vector<SoFile> result;

    DIR* dir = opendir(config.lib_dir);
    if (!dir) {
        std::printf("NearChuckle: cannot open lib directory: %s\n", config.lib_dir);
        return result;
    }

    while (dirent* ent = readdir(dir)) {
        if (!is_so_name(ent->d_name))
            continue;
        if (skip_android_helper(ent->d_name)) {
            std::printf("NearChuckle: skip Android helper: %s\n", ent->d_name);
            continue;
        }

        std::string full = std::string(config.lib_dir) + "/" + ent->d_name;
        struct stat st = {};
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        SoFile so;
        so.path = full;
        so.name = ent->d_name;
        so.size = static_cast<uint64_t>(st.st_size);
        result.push_back(so);
    }

    closedir(dir);

    std::sort(result.begin(), result.end(), [](const SoFile& a, const SoFile& b) {
        const int pa = library_priority(a.name);
        const int pb = library_priority(b.name);
        if (pa != pb) return pa < pb;
        if (a.size != b.size) return a.size < b.size;
        return a.name < b.name;
    });

    return result;
}

static void log_engine_data_dirs() {
    struct Check {
        const char* label;
        const char* relative;
    };

    const Check checks[] = {
        {"FCData", "FCData"},
        {"Shaders", "Shaders"},
        {"FCData/Localized", "FCData/Localized"},
    };

    for (const Check& c : checks) {
        const std::string path = std::string(config.data_root) + "/" + c.relative;
        struct stat st = {};
        const bool present =
            (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
        compatLogFmt("data dir: %s=%s",
                     c.label, present ? "present" : "missing");
    }
}

static void setup_environment() {
    setenv("FARCRY_DATA_DIR", config.data_root, 1);
    setenv("MODULE_PATH", config.lib_dir, 1);
    setenv("HOME", config.data_root, 1);
    setenv("USER", "FarCryPlayer", 1);
    setenv("LOGNAME", "FarCryPlayer", 1);
    setenv("TMPDIR", config.data_root, 1);

    // The Android libopenal shipped with Far Cry was built with the OpenSL
    // backend. There is no OpenSL ES service on Switch, and the game's
    // alcOpenDevice() path treats that backend's initialization failure as a
    // fatal exception rather than falling back. Force OpenAL's null backend
    // for this probe so the game can continue past sound initialization;
    // native Switch audio can be added separately without restoring OpenSL.
    setenv("ALSOFT_DRIVERS", "null", 1);

    if (config.mesa_driver[0]) {
        setenv("MESA_LOADER_DRIVER_OVERRIDE", config.mesa_driver, 1);
        setenv("GALLIUM_DRIVER", config.mesa_driver, 1);
    }

    setenv("ZINK_DESCRIPTORS", "lazy", 1);
    setenv("MESA_GL_VERSION_OVERRIDE", "2.1COMPAT", 1);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "140", 1);

    // Match the Android launcher environment used by the reference build.
    // CryEngine 1 expects desktop OpenGL 2.1 through the GLES compatibility
    // layer, and the Android build explicitly exposes its ARB program paths.
    setenv("MESA_EXTENSION_OVERRIDE",
           "+GL_ARB_vertex_program +GL_ARB_fragment_program", 1);
    setenv("LIBGL_ES", "2", 1);
    setenv("LIBGL_GL", "21", 1);
    setenv("LIBGL_NPOT", "2", 1);
    setenv("LIBGL_MIPMAP", "1", 1);
    setenv("LIBGL_NOBANNER", "1", 1);
    setenv("LIBGL_NORMALIZE", "1", 1);
    setenv("LIBGL_NOTEXMAT", "0", 1);
    setenv("LIBGL_NODOWNSAMPLING", "1", 1);

    chdir(config.data_root);

    // The Android/Linux build of CXGame::GetPlayerProfilePath() expects these
    // two directories to exist and traps when either one is missing. Android
    // normally creates the profile tree outside the native engine, so mirror
    // that small piece of launcher setup here. Do not create or remove any
    // system.cfg/game.cfg files.
    {
        const int profiles_rc = mkdir("Profiles", 0755);
        const int player_rc = mkdir("Profiles/Player", 0755);
        if ((profiles_rc == 0 || errno == EEXIST) &&
            (player_rc == 0 || errno == EEXIST)) {
            compatLog("profiles: ensured Profiles/Player");
        } else {
            compatLogFmt("profiles: failed to ensure Profiles/Player rc=%d errno=%d",
                         player_rc, errno);
        }
    }

    // Keep the Android data layout as-is. Do not rename or relocate resource
    // directories; the guest CryPak must see the original FCData/Shaders tree.
    log_engine_data_dirs();
    compatLog("PAK IO: Android-style virtual mode; no resource extraction/preload");
}

static void log_system_resources(const char* stage) {
    u64 res_total = 0;
    u64 res_used = 0;
    u64 mem_total = 0;
    u64 mem_used = 0;

    const Result r1 = svcGetInfo(&res_total, InfoType_SystemResourceSizeTotal,
                                 CUR_PROCESS_HANDLE, 0);
    const Result r2 = svcGetInfo(&res_used, InfoType_SystemResourceSizeUsed,
                                 CUR_PROCESS_HANDLE, 0);
    const Result r3 = svcGetInfo(&mem_total, InfoType_TotalMemorySize,
                                 CUR_PROCESS_HANDLE, 0);
    const Result r4 = svcGetInfo(&mem_used, InfoType_UsedMemorySize,
                                 CUR_PROCESS_HANDLE, 0);

    if (R_SUCCEEDED(r1) && R_SUCCEEDED(r2)) {
        compatLogFmt("resources[%s]: system=%llu/%llu KiB",
                     stage ? stage : "?",
                     static_cast<unsigned long long>(res_used / 1024),
                     static_cast<unsigned long long>(res_total / 1024));
    } else {
        compatLogFmt("resources[%s]: system query failed r1=0x%08X r2=0x%08X",
                     stage ? stage : "?", static_cast<unsigned>(r1),
                     static_cast<unsigned>(r2));
    }

    if (R_SUCCEEDED(r3) && R_SUCCEEDED(r4)) {
        compatLogFmt("resources[%s]: memory=%llu/%llu KiB",
                     stage ? stage : "?",
                     static_cast<unsigned long long>(mem_used / 1024),
                     static_cast<unsigned long long>(mem_total / 1024));
    } else {
        compatLogFmt("resources[%s]: memory query failed r3=0x%08X r4=0x%08X",
                     stage ? stage : "?", static_cast<unsigned>(r3),
                     static_cast<unsigned>(r4));
    }
}

static void setup_android_runtime() {
    CompatLayer* cl = compatGet();
    std::memset(cl, 0, sizeof(*cl));

    jniSetup(cl);

    cl->window.width = config.screen_width;
    cl->window.height = config.screen_height;
    cl->window.format = 1;
    cl->window.nwin = nwindowGetDefault();

    cl->activity.callbacks = &cl->callbacks;
    cl->activity.vm = static_cast<JavaVM*>(cl->vm_outer);
    cl->activity.env = static_cast<JNIEnv*>(cl->env_outer);
    cl->activity.clazz = reinterpret_cast<void*>(0x4001);
    cl->activity.internalDataPath = config.data_root;
    cl->activity.externalDataPath = config.data_root;
    cl->activity.sdkVersion = 34;
    cl->activity.instance = nullptr;
    cl->activity.obbPath = config.data_root;
    cl->activity.window = &cl->window;

    std::strncpy(cl->asset_mgr.base_path, config.data_root,
                 sizeof(cl->asset_mgr.base_path) - 1);
    cl->asset_mgr.base_path[sizeof(cl->asset_mgr.base_path) - 1] = '\0';
    cl->activity.assetManager = &cl->asset_mgr;

    compatSetObbDir(config.data_root, "com.nearchuckle.farcry");
}

static bool prepare_guest_sdl() {
    LoadedSo* sdl = elfFindLoaded("libSDL3.so");
    if (!sdl) {
        compatLog("SDL: libSDL3.so is not loaded");
        return false;
    }

    CompatLayer* cl = compatGet();
    if (!cl || !cl->env_outer || !cl->vm_outer) {
        compatLog("SDL: fake JNI environment/VM is not initialized");
        return false;
    }

    JNIEnv* env = reinterpret_cast<JNIEnv*>(cl->env_outer);
    JavaVM* vm = reinterpret_cast<JavaVM*>(cl->vm_outer);
    jclass activity_class = reinterpret_cast<jclass>(0x1001);

    // Android normally invokes JNI_OnLoad automatically when System.loadLibrary()
    // loads SDL3. We load ELF files ourselves, so reproduce that bootstrap first.
    void* onload_sym = sdl->findSym("JNI_OnLoad");
    if (!onload_sym) {
        compatLog("SDL: JNI_OnLoad export not found");
        return false;
    }

    using JNIOnLoadFn = jint (*)(JavaVM*, void*);
    JNIOnLoadFn onload = reinterpret_cast<JNIOnLoadFn>(onload_sym);
    jint version = onload(vm, nullptr);
    compatLogFmt("SDL: JNI_OnLoad(vm=%p) -> 0x%x",
                 static_cast<void*>(vm), static_cast<unsigned>(version));

    // SDL's Android entry points are registered with RegisterNatives. They are
    // not required to remain visible as Java_org_* ELF exports, so resolve their
    // actual fnPtr values from the JNI registry populated by JNI_OnLoad.
    using SetupFn = void (*)(JNIEnv*, jclass);

    void* activity_setup_sym =
        jniFindRegisteredNative("nativeSetupJNI", 0);
    if (!activity_setup_sym) {
        compatLog("SDL: registered SDLActivity.nativeSetupJNI not found");
        return false;
    }

    SetupFn activity_setup = reinterpret_cast<SetupFn>(activity_setup_sym);
    activity_setup(env, activity_class);
    compatLog("SDL: SDLActivity.nativeSetupJNI() called");

    // nativeSetupJNI() only reaches SDL_SetMainReady after the audio and
    // controller manager classes have also completed their own bootstrap.
    // SDL registers all three nativeSetupJNI methods in SDL's JNI_OnLoad order:
    // SDLActivity, SDLAudioManager, SDLControllerManager.
    void* audio_setup_sym =
        jniFindRegisteredNative("nativeSetupJNI", 1);
    if (audio_setup_sym) {
        SetupFn audio_setup = reinterpret_cast<SetupFn>(audio_setup_sym);
        audio_setup(env, activity_class);
        compatLog("SDL: SDLAudioManager.nativeSetupJNI() called");
    } else {
        compatLog("SDL: registered SDLAudioManager.nativeSetupJNI not found");
    }

    void* controller_setup_sym =
        jniFindRegisteredNative("nativeSetupJNI", 2);
    if (controller_setup_sym) {
        SetupFn controller_setup = reinterpret_cast<SetupFn>(controller_setup_sym);
        controller_setup(env, activity_class);
        compatLog("SDL: SDLControllerManager.nativeSetupJNI() called");
    } else {
        compatLog("SDL: registered SDLControllerManager.nativeSetupJNI not found");
    }

    void* main_thread_sym =
        jniFindRegisteredNative("nativeInitMainThread", 0);
    if (main_thread_sym) {
        SetupFn init_main_thread = reinterpret_cast<SetupFn>(main_thread_sym);
        init_main_thread(env, activity_class);
        compatLog("SDL: SDLActivity.nativeInitMainThread() called");
    } else {
        compatLog("SDL: registered SDLActivity.nativeInitMainThread not found");
        return false;
    }

    // Keep this explicit as a final guard. The manager setup normally calls
    // checkJNIReady(), but direct execution should not depend on optional SDL
    // configuration paths having supplied every Java callback.
    void* ready_sym = sdl->findSym("SDL_SetMainReady");
    if (ready_sym) {
        using SetMainReadyFn = void (*)();
        SetMainReadyFn set_main_ready =
            reinterpret_cast<SetMainReadyFn>(ready_sym);
        set_main_ready();
    }

    compatLog("SDL: Android JNI bootstrap complete for direct guest SDL_main");
    return true;
}

static bool prepare_guest_sdl_surface() {
    // Mirror the Android SDLActivity/SDLSurface lifecycle before SDL_main:
    // surfaceCreated -> screen resolution -> resize -> surfaceChanged.
    // The native callbacks are registered by libSDL3.so during JNI_OnLoad.
    using VoidFn = void (*)();
    using ScreenResolutionFn = void (*)(int, int, int, int, float, float);

    void* surface_created_sym = jniFindRegisteredNative("onNativeSurfaceCreated", 0);
    void* set_resolution_sym = jniFindRegisteredNative("nativeSetScreenResolution", 0);
    void* native_resize_sym = jniFindRegisteredNative("onNativeResize", 0);
    void* surface_changed_sym = jniFindRegisteredNative("onNativeSurfaceChanged", 0);

    if (!surface_created_sym || !set_resolution_sym ||
        !native_resize_sym || !surface_changed_sym) {
        compatLogFmt(
            "SDL: guest Surface lifecycle callbacks missing "
            "created=%p resolution=%p resize=%p changed=%p",
            surface_created_sym, set_resolution_sym,
            native_resize_sym, surface_changed_sym);
        return false;
    }

    reinterpret_cast<VoidFn>(surface_created_sym)();
    compatLog("SDL: onNativeSurfaceCreated() called");

    const float density = 1.0f;
    const float refresh_rate = 60.0f;
    reinterpret_cast<ScreenResolutionFn>(set_resolution_sym)(
        config.screen_width,
        config.screen_height,
        config.screen_width,
        config.screen_height,
        density,
        refresh_rate);
    compatLogFmt("SDL: nativeSetScreenResolution(%dx%d) called",
                 config.screen_width, config.screen_height);

    reinterpret_cast<VoidFn>(native_resize_sym)();
    compatLog("SDL: onNativeResize() called");

    reinterpret_cast<VoidFn>(surface_changed_sym)();
    compatLog("SDL: onNativeSurfaceChanged() called");

    return true;
}

static int run_farcry(LoadedSo* game_so) {
    if (!game_so)
        return -1;

    void* entry = game_so->findSym("SDL_main");
    if (!entry)
        entry = game_so->findSym("main");

    if (!entry) {
        compatLog("ERROR: libFarCry.so exports neither SDL_main nor main");
        return -1;
    }

    using MainFn = int (*)(int, char**);
    MainFn game_main = reinterpret_cast<MainFn>(entry);

    char arg0[] = "FarCry";

    // CryEngine's Android command-line parser keeps quoted arguments intact.
    // Each quoted entry below therefore becomes one complete console command,
    // e.g. "r_Width 1280", which ExecuteString() parses as CVar + value.
    // Without the quotes the parser receives "r_Width" and "1280" separately
    // and reports the value as an unknown command.
    char arg1[64];
    char arg2[64];
    char arg3[64];
    char arg4[64];
    char arg5[64];
    char arg6[64];
    char arg7[64];
    char arg8[64];
    char arg9[64];
    char arg10[64];
    char arg11[64];
    char arg12[64];
    char arg13[64];

    std::snprintf(arg1, sizeof(arg1), "\"r_Driver OpenGL\"");
    std::snprintf(arg2, sizeof(arg2), "\"r_Width %d\"", config.screen_width);
    std::snprintf(arg3, sizeof(arg3), "\"r_Height %d\"", config.screen_height);
    std::snprintf(arg4, sizeof(arg4), "\"r_Fullscreen 1\"");
    std::snprintf(arg5, sizeof(arg5), "\"game_fov %d\"", config.fov);
    std::snprintf(arg6, sizeof(arg6), "\"r_Quality_BumpMapping 3\"");
    std::snprintf(arg7, sizeof(arg7), "\"r_NoPS20 0\"");
    // r_GL_NV30_PS20 is initialized to 1 by Android SystemInit after the
    // command-line/config stage, so passing it here only creates a redundant
    // console command and is not needed for the Android shader path.
    std::snprintf(arg8, sizeof(arg8), "\"GL_NV30_PS20 1\"");
    std::snprintf(arg9, sizeof(arg9), "\"r_UseHWShaders 1\"");
    std::snprintf(arg10, sizeof(arg10), "\"r_VSync 0\"");
    std::snprintf(arg11, sizeof(arg11), "\"r_displayInfo 1\"");
    // The Switch build intentionally uses embedded fallback ARB shaders because
    // the Android-style runtime shader cache is not loaded. Android identified
    // the detail-overlay fallback as a source of garbled texture patterns; keep
    // the equivalent detail path disabled until real detail shaders are available.
    std::snprintf(arg12, sizeof(arg12), "\"r_DetailTextures 0\"");
    // Disable render-buffer merging for one A/B run. The original CryEngine
    // merge path rewrites vertex/index data in mfFillRB(), so this isolates
    // merge-specific geometry corruption without changing the GL stream path.
    std::snprintf(arg13, sizeof(arg13), "\"r_rb_merge 0\"");
    // CryEngine applies +CVar post-commands after renderer/system initialization.
    // ui_BackGroundVideo is created later by CUISystem::CreateCVars(), so a
    // startup command cannot override its default value of 1 reliably.
    // It is intentionally not included in the early command-line argument list.

    char* argv[14];
    argv[0] = arg0;
    argv[1] = arg1;
    argv[2] = arg2;
    argv[3] = arg3;
    argv[4] = arg4;
    argv[5] = arg5;
    argv[6] = arg6;
    argv[7] = arg7;
    argv[8] = arg8;
    argv[9] = arg9;
    argv[10] = arg10;
    argv[11] = arg11;
    argv[12] = arg12;
    argv[13] = arg13;

    const int argc = 14;

    // Keep shader compilation enabled, matching the working Android build.
    // Missing/experimental Switch shader caches must not turn the menu into a
    // black frame just because this wrapper was built from a shader-debug branch.

    compatLog("Far Cry: Android-style graphics CVars queued (quoted command syntax)");
    compatLog("Far Cry: ui_BackGroundVideo left at Android default until UI CVar creation");
    compatLog("Far Cry: r_UseHWShaders 1 queued for shader script registration");
    compatLog("Far Cry: r_displayInfo 1 queued for on-screen FPS/render statistics");
    compatLog("Far Cry: r_DetailTextures 0 queued to avoid fallback detail-overlay artifacts");
    compatLog("Far Cry: r_rb_merge 0 queued for merge-path A/B geometry test");
    compatLog("Far Cry: command-line CVars use quoted name + value commands");

    compatLogFmt("Starting Far Cry: %p argc=%d", reinterpret_cast<void*>(game_main), argc);
    compatLog("Startup diagnostics complete; waiting for CXGame::Run main-loop marker");

    // Keep the startup console and diagnostic log alive through the complete
    // CryEngine startup sequence. runtime.cpp closes the file and releases the
    // console exactly when the "CXGame::Run: entered main game loop" marker is
    // received, handing the framebuffer back to the real game menu at that point.
    compatLogFlush();

    return game_main(argc, argv);
}

int main(int, char**) {
    // Keep the full startup diagnostics visible on the Switch while game_main()
    // runs. runtime.cpp releases the console exactly at the main-loop marker,
    // after which the real SDL/EGL game window owns the framebuffer.
    compatUiInit();

    if (read_config("/switch/NearChuckle_nx/config.txt") != 0)
        std::printf("NearChuckle: config.txt not found, using defaults\n");

    setup_environment();

    compatLog("=== NearChuckle_nx start ===");
    compatLogFmt("data_root=%s", config.data_root);
    compatLogFmt("lib_dir=%s", config.lib_dir);
    compatLogFmt("mesa_driver=%s", config.mesa_driver);
    compatLogFmt("resolution=%dx%d", config.screen_width, config.screen_height);
    compatLogFmt("host diag anchors: compatLog=%p compatLogFmt=%p compatLogRaw=%p",
                 reinterpret_cast<void*>(&compatLog),
                 reinterpret_cast<void*>(&compatLogFmt),
                 reinterpret_cast<void*>(&compatLogRaw));

    setup_android_runtime();
    androidTlsInstall();

    elfSetDlopenDir(config.lib_dir);
    elfResetCounts();

    const std::vector<SoFile> libs = find_guest_libraries();
    if (libs.empty()) {
        compatLog("ERROR: no Android ARM64 .so files found");
        compatLogFlush();
        return 1;
    }

    LoadedSo* game_so = nullptr;

    log_system_resources("before ELF load");

    for (const SoFile& file : libs) {
        compatLogFmt("ELF load: %s (%llu bytes)",
                     file.name.c_str(),
                     static_cast<unsigned long long>(file.size));

        LoadedSo* so = elfLoad(file.path.c_str(), nullptr);
        if (!so) {
            compatLogFmt("WARN: failed to load %s", file.name.c_str());
            log_system_resources(file.name.c_str());
            continue;
        }

        log_system_resources(file.name.c_str());

        if (file.name == "libFarCry.so")
            game_so = so;
    }

    if (!game_so) {
        compatLog("ERROR: libFarCry.so was not loaded");
        compatLogFlush();
        return 1;
    }

    vnxSetGameSo(game_so);

    compatLogFmt("Loaded Android ARM64 libraries. unresolved=%d",
                 elfGetUnresolvedCount());

    for (const SoFile& file : libs) {
        LoadedSo* so = elfFindLoaded(file.name.c_str());
        if (!so)
            continue;

        compatLogFmt("ELF ctors: %s", file.name.c_str());
        elfRunCtors(so, nullptr);
    }

    compatLog("ELF constructors complete");
    compatLogFlush();

    // We invoke the guest Android SDL_main symbol directly instead of entering
    // through SDL's generated platform main. SDL3's Android build starts with
    // SDL_MainIsReady == false in that configuration, so SDL_Init() rejects
    // window/video initialization until SDL_SetMainReady() is called.
    if (!prepare_guest_sdl()) {
        compatLog("ERROR: could not prepare guest SDL3 main state");
        compatLogFlush();
        return 1;
    }

    if (!prepare_guest_sdl_surface()) {
        compatLog("ERROR: could not prepare guest SDL3 Surface state");
        compatLogFlush();
        return 1;
    }

    // The libnx software console owns the default NWindow/Framebuffer.
    // Release it BEFORE the guest SDL/EGL window is created so CryEngine's
    // presentation surface has exclusive ownership of the display buffers.
    // The startup file log remains active; only the temporary on-screen
    // diagnostics console is retired here.
    compatLog("SDL: releasing startup console before guest EGL window creation");
    compatLogFlush();
    compatUiShutdown();

    int rc = run_farcry(game_so);

    compatLogFmt("Far Cry returned %d", rc);
    compatLogFlush();
    compatLogClose();

    vnxSetGameSo(nullptr);
    compatUiShutdown();
    return rc;
}