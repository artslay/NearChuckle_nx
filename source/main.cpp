#include "config.h"
#include "compat/loader.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

extern void androidTlsInstall();
extern void vnxSetGameSo(LoadedSo* so);

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

static void normalize_engine_data_dirs() {
    struct Pair {
        const char* from;
        const char* to;
    };

    const Pair root_dirs[] = {
        {"FCData", "fcdata"},
        {"Shaders", "shaders"},
    };

    for (const Pair& p : root_dirs) {
        const std::string from = std::string(config.data_root) + "/" + p.from;
        const std::string to = std::string(config.data_root) + "/" + p.to;

        struct stat st_from = {};
        struct stat st_to = {};
        const bool has_from = (stat(from.c_str(), &st_from) == 0) && S_ISDIR(st_from.st_mode);
        const bool has_to = (stat(to.c_str(), &st_to) == 0) && S_ISDIR(st_to.st_mode);

        if (has_from && !has_to) {
            if (rename(from.c_str(), to.c_str()) == 0) {
                compatLogFmt("data casefix: %s -> %s", from.c_str(), to.c_str());
            } else {
                compatLogFmt("data casefix FAILED: %s -> %s errno=%d",
                             from.c_str(), to.c_str(), errno);
            }
        } else {
            compatLogFmt("data dir: %s=%s %s=%s",
                         p.from, has_from ? "present" : "missing",
                         p.to, has_to ? "present" : "missing");
        }
    }

    const std::string localized_from =
        std::string(config.data_root) + "/fcdata/Localized";
    const std::string localized_to =
        std::string(config.data_root) + "/fcdata/localized";

    struct stat loc_from_st = {};
    struct stat loc_to_st = {};
    const bool has_loc_from =
        (stat(localized_from.c_str(), &loc_from_st) == 0) &&
        S_ISDIR(loc_from_st.st_mode);
    const bool has_loc_to =
        (stat(localized_to.c_str(), &loc_to_st) == 0) &&
        S_ISDIR(loc_to_st.st_mode);

    if (has_loc_from && !has_loc_to) {
        if (rename(localized_from.c_str(), localized_to.c_str()) == 0) {
            compatLogFmt("data casefix: %s -> %s",
                         localized_from.c_str(), localized_to.c_str());
        } else {
            compatLogFmt("data casefix FAILED: %s -> %s errno=%d",
                         localized_from.c_str(), localized_to.c_str(), errno);
        }
    } else {
        compatLogFmt("data dir: FCData/Localized=%s FCData/localized=%s",
                     has_loc_from ? "present" : "missing",
                     has_loc_to ? "present" : "missing");
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

    setenv("MESA_GL_VERSION_OVERRIDE", "2.1COMPAT", 1);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "140", 1);
    setenv("MESA_EXTENSION_OVERRIDE",
           "+GL_ARB_vertex_program +GL_ARB_fragment_program", 1);

    chdir(config.data_root);
    normalize_engine_data_dirs();
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
    char arg1[] = "r_Driver OpenGL";
    char arg2[64];
    char arg3[64];
    char arg4[] = "r_Fullscreen 1";
    char arg5[64];
    char arg6[] = "r_VSync 1";

    std::snprintf(arg2, sizeof(arg2), "r_Width %d", config.screen_width);
    std::snprintf(arg3, sizeof(arg3), "r_Height %d", config.screen_height);
    std::snprintf(arg5, sizeof(arg5), "game_fov %d", config.fov);

    char* argv[7];
    argv[0] = arg0;
    argv[1] = arg1;
    argv[2] = arg2;
    argv[3] = arg3;
    argv[4] = arg4;
    argv[5] = arg5;

    int argc = 6;
    if (config.vsync) {
        argv[6] = arg6;
        argc = 7;
    }

    compatLogFmt("Starting Far Cry: %p argc=%d", reinterpret_cast<void*>(game_main), argc);
    compatLogFlush();

    return game_main(argc, argv);
}

int main(int, char**) {
    if (read_config("/switch/NearChuckle_nx/config.txt") != 0)
        std::printf("NearChuckle: config.txt not found, using defaults\n");

    setup_environment();

    compatLog("=== NearChuckle_nx start ===");
    compatLogFmt("data_root=%s", config.data_root);
    compatLogFmt("lib_dir=%s", config.lib_dir);
    compatLogFmt("mesa_driver=%s", config.mesa_driver);
    compatLogFmt("resolution=%dx%d", config.screen_width, config.screen_height);

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

    int rc = run_farcry(game_so);

    compatLogFmt("Far Cry returned %d", rc);
    compatLogFlush();

    vnxSetGameSo(nullptr);
    return rc;
}