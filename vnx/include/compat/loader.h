#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include "android.h"

// ELF64 types (no system elf.h in devkitA64)
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type, e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff, e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize, e_phentsize, e_phnum;
    Elf64_Half e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type, p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info, st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

#define ET_DYN      3
#define EM_AARCH64  183
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_SYMENT   11
#define DT_JMPREL   23
#define DT_PLTREL   20
#define DT_INIT         12
#define DT_FINI         13
#define DT_INIT_ARRAY    25
#define DT_FINI_ARRAY    26
#define DT_INIT_ARRAYSZ  27
#define DT_FINI_ARRAYSZ  28
#define DT_STRSZ    10
#define SHN_UNDEF   0
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xFFFFFFFFULL))
#define ELF64_ST_BIND(i) ((i) >> 4)
#define R_AARCH64_NONE      0
#define R_AARCH64_ABS64   257
#define R_AARCH64_COPY   1024
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027
#define ALIGN_UP(x,a)   (((uint64_t)(x) + (uint64_t)(a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x,a) ((uint64_t)(x) & ~((uint64_t)(a) - 1))

// ─── LoadedSo ─────────────────────────────────────────────────────────────────
// One loaded ARM64 .so
struct LoadedSo {
    typedef void(*InitFn)();

    Jit          jit_mem;     // JIT memory handle (valid when using_jit is true)
    bool         using_jit = false;

    uint8_t*     alloc;       // exec (RX) allocation base pointer
    uint8_t*     write_alloc; // RW twin of alloc (JitType_CodeMemory: always accessible)
    uint8_t*     data_alloc;  // rx_addr of the data-segment JIT (kept Rw, never Rx)
    size_t       alloc_size;
    uint64_t     min_vaddr;   // first PT_LOAD p_vaddr
    uint64_t     data_vaddr;  // page-aligned vaddr of data_alloc base (0 if single-jit)
    uint8_t*     base;        // alloc - min_vaddr (exec side; base+vaddr = runtime ptr)

    // Heap copies of strtab/symtab so they survive jitTransitionToExecutable
    // (the JIT RW mapping is unmapped after the transition)
    char*        strtab_heap = nullptr;
    Elf64_Sym*   symtab_heap = nullptr;

    const char*  strtab;
    Elf64_Sym*   symtab;
    uint32_t     sym_count;
    uint64_t     strsz = 0;  // DT_STRSZ — bounds-checked in findSym against st_name

    // DT_INIT: single init function, runs before DT_INIT_ARRAY on Android
    InitFn   init_fn        = nullptr;
    // DT_INIT_ARRAY: stored here so constructors can be run after all SOs load
    InitFn*  init_arr       = nullptr;
    size_t   init_arr_count = 0;

    std::string  path;        // path on SD card

    // Find a symbol by name; returns runtime (exec) ptr or nullptr
    void* findSym(const char* name) const;
};

// ─── CompatLayer ─────────────────────────────────────────────────────────────
// High-level Android compatibility state
struct CompatLayer {
    ANativeActivity      activity;
    ANativeActivityCallbacks callbacks;
    ANativeWindow        window;
    AAssetManager        asset_mgr;
    ALooper              looper;
    AInputQueue          input_queue;

    // Fake JNI storage — owned by jni_env.cpp
    void*  vm_outer;   // passed to ANativeActivity.vm (JavaVM*)
    void*  env_outer;  // passed to ANativeActivity.env (JNIEnv*)
};

// ─── Launch result ────────────────────────────────────────────────────────────
struct LaunchResult {
    // Set when the game runs under the ARM32 interpreter, which is driven
    // differently from a native arm64 game once loading is done.
    bool is_arm32 = false;
    bool        ok          = false;
    std::string errorStage;   // which step failed (e.g. "Extracting APK")
    std::string errorDetail;  // human-readable reason
    int         unresolved  = 0;  // number of unresolved ELF symbols
    uint32_t    svcPermCode = 0;  // result of svcSetMemoryPermission (0 = OK)
    // Set when ELF loading succeeds; main thread must call runGameOnMainThread.
    void*       game_so     = nullptr;  // LoadedSo*
};

// Progress callback invoked at each major launch stage.
// stage  = short label ("Extracting APK", "Loading ELF", …)
// detail = one-liner with more info (filename, size, etc.)
typedef void (*ProgressCb)(const char* stage, const char* detail);

// ─── Public API ───────────────────────────────────────────────────────────────
// Returns the global compat layer (singleton)
CompatLayer* compatGet();

// Load a single .so from disk — does NOT run constructors (call elfRunCtors after)
LoadedSo*    elfLoad(const char* path, ProgressCb cb = nullptr);
// Find an already-loaded .so by its basename (e.g. "libunity.so"), or nullptr.
LoadedSo*    elfFindLoaded(const char* basename);
// Backs a real dlopen(): returns the already-loaded .so with this basename, or
// loads it on demand from the game's lib dir (set via elfSetDlopenDir). Runs its
// constructors too, matching real dlopen semantics. Returns nullptr on failure.
LoadedSo*    elfDlopen(const char* name);
// Directory real-dlopen() searches when a requested lib isn't already loaded.
void         elfSetDlopenDir(const char* lib_dir);
// Reset accumulated unresolved-symbol count and JIT error code (call before loading a batch)
void         elfResetCounts();
// Run a loaded SO's DT_INIT_ARRAY constructors. cb (optional) is called periodically
// during the run so the UI can show sub-step progress.
void         elfRunCtors(LoadedSo* so, ProgressCb cb = nullptr);
// Number of unresolved symbols accumulated since last elfResetCounts() call
int          elfGetUnresolvedCount();

// UI ring buffer — write a short message to the on-screen rolling log
void         compatUiLog(const char* msg);
// Set the progress bar percentage (0–100)
void         compatUiSetPct(int pct);
// First JIT failure code since last elfResetCounts() call (0 = all OK)
uint32_t     elfGetLastSvcPermCode();
// Resolve a symbol against the override shim table (checked before game libs).
void*        shimResolve(const char* name);
// Fallback shim resolver (Unity/IL2CPP libc gap fillers) — checked AFTER game
// libraries so it never shadows a game that ships its own copies (e.g. HCR).
void*        shimResolveFallback(const char* name);

// Find the nearest symbol at or before `vaddr` (ELF virtual address) in `so`.
// Writes "symbol+0x<offset>" (or "0x<vaddr>" if none found) into buf[sz].
// Returns buf. buf must be at least 128 bytes.
const char*  elfNearestSym(const LoadedSo* so, uint64_t vaddr, char* buf, size_t sz);

// Run JNI_OnLoad + Cocos2d-x game loop from the MAIN thread (which has SDL2's
// EGL context active).  sdl_win is SDL_Window* for buffer swap after each frame.
// apk_path and data_path are the strings passed to nativeSetPaths.
// Closes compat_log when it returns.
void runGameOnMainThread(void* game_so_ptr,
                         void* sdl_win,
                         const std::string& apk_path,
                         const std::string& data_path);
// Called from jni_env.cpp to install JNI/VM tables into compat layer
void         jniSetup(CompatLayer* cl);

// Find an exported symbol in the currently running game .so (nullptr if no
// game is running or the symbol doesn't exist). Used by jni_env.cpp to invoke
// the game's registered Java_ native callbacks.
void*        compatFindGameSym(const char* name);

// Look up a native method the game registered via JNI RegisterNatives (by
// method name, e.g. "nativeRender"). Unity/IL2CPP register their whole player
// API this way instead of exporting Java_ symbols. Returns the fn ptr or null.
void*        jniFindRegisteredNative(const char* name);

// Enable the Unity-only Android Java object model (see jni_env.cpp). Off by
// default; the Unity runtime turns it on before driving initJni so cocos2d-x
// games are completely unaffected.
void         jniSetUnityMode(bool on);

// Tell the file shims where this game's expansion files (OBBs) were installed,
// so a path a game hardcoded against Android's external storage resolves to
// them. Set once per launch, before any game code runs. See compat/obb.h.
void         compatSetObbDir(const char* dir, const char* pkg);

// Release the Core's SDL2 EGL window surface so Unity can create its own on the
// display window (see compatUnityReleaseWindow in loader.cpp). Returns the
// shared EGLDisplay. Unity-path only.
void*        compatUnityReleaseWindow();

// Normal (mutex-serialized, dedup'd) logger to compat_log.txt. compatLog
// writes one line; compatLogFmt is printf-style; compatLogFlush forces the
// buffered file to disk. Defined in loader.cpp.
void         compatLog(const char* msg);
void         compatLogFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void         compatLogFlush(void);

// Lock-free logger for crash-forensics paths only (see loader.cpp) — never
// use this for normal logging, it can interleave with concurrent compatLog
// calls. Exists so a fault can always be recorded even if the crashing
// thread died while holding the normal logger's mutex.
void         compatLogRaw(const char* msg);

// Publish install progress for the HOME-menu overlay, which is a separate
// process and can't see any of this directly. Cheap and rate-limited; safe to
// call from a progress callback. state is "installing" | "done" | "error" |
// "idle".
void         installStatusWrite(const char* state, const char* pkg, const char* name,
                                const char* stage, int pct);
void         installStatusClear(void);

// Faults recovered from while running game constructors. Only meaningful next
// to the main thread's watchdog phase — the question is always whether the two
// line up in time.
int          elfGetCtorFaultCount(void);

// How many times the game has reached our free() shim. Read together with the
// fault count: frees that fault without passing through here were bypassing it.
unsigned long shimFreeCallCount(void);
void          shimAllocCounts(unsigned long* malloc_n, unsigned long* calloc_n,
                              unsigned long* realloc_n, unsigned long* free_n);

// Heap integrity. shimHeapAnchor must be called before any game code runs;
// shimHeapCheck then walks forward from that point and returns false with a
// reason once the chunk chain stops making sense.
const char*   shimAddrRegion(uint64_t addr);
void          shimHeapAnchor(void);
bool          shimHeapCheck(char* why, size_t whysz);
// Head-of-arena check only. Cheap enough to sample continuously, which is what
// lets a corruption report name the constructor that caused it.
bool          shimHeapCheckFast(char* why, size_t whysz);
// How far the last walk got, and why it stopped. A clean result is only
// meaningful alongside these.
void          shimHeapWalkStats(int* steps, const char** stop);
// Heap region bounds and the current break — tells exhaustion from corruption.
void          shimHeapExtent(uint64_t* lo, uint64_t* hi, uint64_t* brk);

// Tell the loader what the game's manifest asked for, before launchApk.
// Raw android:screenOrientation constant; -1 for unspecified.
void         loaderSetScreenOrient(int screen_orientation);

// Load a game without running any of its code. Everything up to the
// constructors is exercised; the constructors themselves are skipped, so this
// tests the loader rather than the game.
void         elfSetDryRun(bool on);
bool         elfIsDryRun(void);
// What the loader is executing right now — read by the integrity monitor so a
// corruption report can name the constructor instead of a range of them.
int          elfCurrentCtor(void);
const char*  elfCurrentModule(void);
int          elfGetUnresolvedCount(void);

// Called by jni_env.cpp when the game signals its own loading/splash screen
// is done (splashScreenHasCompleted). Hides the Viridite branding
// overlay drawn over the game's loading screen (see loader.cpp).
void         compatMarkSplashDone();

// Called by jni_env.cpp the FIRST time trackPage(...) ever fires, regardless
// of page name. The pixel-fingerprint used to hide the branding overlay
// turned out to be insufficient — vehicle-select/garage/upgrade screens
// share the same dark-vignette corners as the loading screen (confirmed on
// hardware: the probe kept matching for 80+ seconds after loading actually
// finished). trackPage firing at all is a reliable semantic signal that
// we've navigated to a REAL, named screen — the loading screen itself never
// calls this — so it's a much better "hide for good" trigger than more
// pixel-probe tuning.
void         compatMarkPastLoading();

// UserDefault persistence (jni_env.cpp): loaded before nativeInit, saved on
// every UserDefault.flush and at game exit.
void jniUserDefaultsLoad(const char* path);
// force=true bypasses the 2s write-debounce (used at game exit so the final
// state is never dropped); a plain flush() call from the game leaves it false.
void jniUserDefaultsSave(bool force = false);

// ─── SimpleAudioEngine backend (audio.cpp, SDL2_mixer) ───────────────────────
// jni_env.cpp forwards the Cocos2dxSound/Cocos2dxMusic JNI calls here.
void  compatAudioSetAssetsDir(const char* dir);
void  compatAudioWarmup();
void  compatAudioPlayMusic(const char* path, bool loop);
void  compatAudioPreloadMusic(const char* path);
void  compatAudioStopMusic();
void  compatAudioPauseMusic();
void  compatAudioResumeMusic();
void  compatAudioRewindMusic();
void  compatAudioSetMusicVolume(float v);
bool  compatAudioMusicPlaying();
void  compatAudioPreloadEffect(const char* path);
void  compatAudioUnloadEffect(const char* path);
int   compatAudioPlayEffect(const char* path, bool loop, float gain = 1.0f);
void  compatAudioSetEffectVolume(int id, float vol);
void  compatAudioStopEffect(int id);
void  compatAudioPauseEffect(int id);
void  compatAudioResumeEffect(int id);
void  compatAudioStopAllEffects();
void  compatAudioPauseAllEffects();
void  compatAudioResumeAllEffects();
void  compatAudioSetEffectsVolume(float v);
void  compatAudioMuteEffectsFor(int ms);
// Playback rate as the game set it. SDL_mixer cannot honour it, but it is the
// only reliable signal for when the sample being played bears no resemblance to
// the one intended — which is what a pitch sweep sounds like through a mixer
// that ignores pitch.
void  compatAudioSetEffectRate(int ch, float rate);
float compatAudioGetMusicVolume();
float compatAudioGetEffectsVolume();

// Extract APK libs+assets to sdmc:/Viridite/games/<pkg_name>/ and write
// a .installed marker.  Returns false if the APK cannot be opened.
// Safe to call even if already installed — just re-extracts.
bool apkInstall(const std::string& apk_path,
                const std::string& pkg_name,
                ProgressCb         cb = nullptr);

// High-level launcher — loads libs and runs the game.
// If already_installed=true the APK extraction step is skipped entirely.
LaunchResult launchApk(const std::string& apk_path,
                       const std::string& pkg_name,
                       ProgressCb         cb = nullptr,
                       bool               already_installed = false);

// ─── Controller-guide labelling ─────────────────────────────────────────────
// The controller diagram patched into a game's help screen can be annotated
// with what each control actually does. Positions live per-diagram and
// meanings live per-game (see guide_labels.cpp), so the two stay independent.

struct SDL_Surface;   // avoids pulling SDL into every translation unit

enum class GuideController { Pro, Handheld, JoyDual, JoyLeft, JoyRight };

// Controls a diagram can point at. Face/DPad cover the whole cluster rather
// than individual buttons, since that's the granularity a help image reads at.
enum class GuideButton { L, R, DPad, Face, Plus };

struct GuideLabel { GuideButton button; const char* text; };

// Annotates `img` in place. No font, or a controller with no layout, simply
// leaves the diagram unannotated rather than failing the patch.
void guideDrawLabels(SDL_Surface* img, GuideController controller,
                     const GuideLabel* labels, int labelCount);

// Heap-integrity probes used to localise the corruption behind the free()
// faults documented in docs/BRAIN_IT_ON_FINDINGS.md. Arm before a large
// allocation, check after each phase; the first phase to report damage is the
// one that overran.
void elfHeapCanaryArm(void);
void elfHeapCanaryCheck(const char* stage);
