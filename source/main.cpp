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
        if (a.name == "libFarCry.so") return false;
        if (b.name == "libFarCry.so") return true;
        if (a.size != b.size) return a.size < b.size;
        return a.name < b.name;
    });

    return result;
}

static void setup_environment() {
    setenv("FARCRY_DATA_DIR", config.data_root, 1);
    setenv("MODULE_PATH", config.lib_dir, 1);
    setenv("HOME", config.data_root, 1);
    setenv("USER", "FarCryPlayer", 1);
    setenv("LOGNAME", "FarCryPlayer", 1);
    setenv("TMPDIR", config.data_root, 1);

    if (config.mesa_driver[0]) {
        setenv("MESA_LOADER_DRIVER_OVERRIDE", config.mesa_driver, 1);
        setenv("GALLIUM_DRIVER", config.mesa_driver, 1);
    }

    setenv("MESA_GL_VERSION_OVERRIDE", "2.1COMPAT", 1);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "140", 1);
    setenv("MESA_EXTENSION_OVERRIDE",
           "+GL_ARB_vertex_program +GL_ARB_fragment_program", 1);

    chdir(config.data_root);
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
    consoleInit(nullptr);
    romfsInit();

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
        romfsExit();
        consoleExit(nullptr);
        return 1;
    }

    LoadedSo* game_so = nullptr;

    for (const SoFile& file : libs) {
        compatLogFmt("ELF load: %s (%llu bytes)",
                     file.name.c_str(),
                     static_cast<unsigned long long>(file.size));

        LoadedSo* so = elfLoad(file.path.c_str(), nullptr);
        if (!so) {
            compatLogFmt("WARN: failed to load %s", file.name.c_str());
            continue;
        }

        if (file.name == "libFarCry.so")
            game_so = so;
    }

    if (!game_so) {
        compatLog("ERROR: libFarCry.so was not loaded");
        compatLogFlush();
        romfsExit();
        consoleExit(nullptr);
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

    int rc = run_farcry(game_so);

    compatLogFmt("Far Cry returned %d", rc);
    compatLogFlush();

    vnxSetGameSo(nullptr);
    romfsExit();
    consoleExit(nullptr);
    return rc;
}
