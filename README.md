# NearChuckle_nx

Nintendo Switch runtime for the Android ARM64 Far Cry build from NearChuckle.

The Switch branch is a direct runtime. It does not contain the Android application, Android launcher, Windows binaries, or the old desktop build system. Android ARM64 .so files are loaded directly by the VNX ELF loader.

Structure:

NearChuckle_nx/
  Makefile
  config.txt
  source/
  vnx/
  mesa-sdk/
  game/
    lib/

Put the Android ARM64 libraries in /switch/NearChuckle_nx/game/lib/.

The main entry library is libFarCry.so. Other CryEngine and Android ARM64 dependencies should be placed in the same directory.

Game data is expected under /switch/NearChuckle_nx/game/.

config.txt controls the data path, library path, resolution, FOV, VSync, and Mesa driver.

The NRO starts Far Cry directly. There is no launcher UI.

The build expects a local Mesa SDK at mesa-sdk/opt/devkitpro/portlibs/switch/. The Switch executable links Mesa OpenGL, EGL, GLES, and Vulkan/NVK libraries from this SDK.

Build from the devkitPro MSYS2 shell with:

  make

The runtime log is written to:

  /switch/NearChuckle_nx/nearchuckle_debug.log
