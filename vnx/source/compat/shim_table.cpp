#include "compat/loader.h"
#include "compat/orientation.h"
#include "compat/android.h"
#include "compat/sensors.h"
#include "compat/obb.h"
#include "compat/apkcache.h"
#include <switch.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <sys/statvfs.h>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cerrno>
#include <ctime>
#include <cctype>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <setjmp.h>
#include <semaphore.h>

extern void elfDescribePc(uint64_t pc, char* buf, size_t sz);
// zlib API declarations. Some devkitA64 installations do not ship a zlib header,
// while libz is still available for linking. Keep the ABI declarations local.
extern "C" {
    struct z_stream_s {
        unsigned char* next_in;
        unsigned int avail_in;
        unsigned long total_in;
        unsigned char* next_out;
        unsigned int avail_out;
        unsigned long total_out;
        const char* msg;
        void* state;
        void* zalloc;
        void* zfree;
        void* opaque;
        int data_type;
        unsigned long adler;
        unsigned long reserved;
    };
    typedef struct z_stream_s z_stream;
    typedef z_stream* z_streamp;

    const char* zlibVersion(void);
    int inflate(z_streamp, int);
    int inflateEnd(z_streamp);
    int inflateInit_(z_streamp, const char*, int);
    int inflateInit2_(z_streamp, int, const char*, int);
    int inflateReset(z_streamp);
    int deflate(z_streamp, int);
    int deflateEnd(z_streamp);
    int deflateInit_(z_streamp, int, const char*, int);
    int deflateInit2_(z_streamp, int, int, int, int, int, const char*, int);
    unsigned long crc32(unsigned long, const unsigned char*, unsigned int);
    unsigned long adler32(unsigned long, const unsigned char*, unsigned int);
    int uncompress(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
    int compress(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
}
#include <fnmatch.h>
#include <libgen.h>
#include <sys/lock.h>
#include <climits>
#include <fenv.h>
#include <poll.h>
#include <sys/socket.h>
#include <utime.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>

// Diagnostic logging policy. Keep crash/loader/path failures visible, but suppress
// high-frequency allocator and successful realpath chatter that can drown the
// actual failure. Flip these back to true only for a focused memory investigation.
static constexpr bool kVerboseAllocatorLogs = false;
static constexpr bool kVerboseSuccessfulPathLogs = false;

static std::string asciiLower(std::string value);

// Case-insensitive filesystem resolver used by file wrappers below.
static bool resolvePathCaseInsensitive(const char* input, std::string& resolved);

static bool compatIsPakPath(const char* path);
static bool patchCommonSubroutinesIntoShaderMacro(const char* macroPath,
                                                  const char* programPath);
static bool tryMaterializeUniquePakBasename(const char* targetPath);
static bool tryMaterializePakPath(const char* targetPath, std::string& materialized);
static std::string pakNormalizeName(const char* name);
static bool isShaderCacheLookupPath(const char* path);
static void compatLogPakOpenState(FILE* f, const char* path);

// Normalize Switch virtual-device paths before they reach newlib's POSIX I/O.
//
// libnx normally accepts paths such as "sdmc:/switch/Foo". The guest Android
// build can also hand CryPak an already-rooted path like:
//   sdmc:/switch/Foo/sdmc:/switch/Foo/FCData/...
// In the newlib file API, "sdmc:/..." is not an absolute POSIX path here, so
// leaving it untouched makes the CWD get prepended and produces a duplicated
// path. Collapse both forms to the real POSIX path rooted at "/".
static std::string normalizeSwitchFsPath(const char* input) {
    if (!input)
        return std::string();

    std::string path(input);
    if (path.size() < 6 || path.compare(0, 6, "sdmc:/") != 0)
        return path;

    // If the path contains the virtual device prefix twice, the second prefix
    // is the real root of the requested path. Keep only that second path.
    const size_t second = path.find("sdmc:/", 6);
    if (second != std::string::npos)
        return path.substr(second + 5); // keep the leading '/'

    // A normal sdmc:/ absolute path maps directly to the Switch POSIX root.
    return path.substr(5); // "sdmc:" -> "/switch/..."
}



// pread is not exported by the devkitA64/newlib runtime used here.
// The APK cache only needs positional reads, so emulate it with lseek/read
// while preserving the caller's file position.
extern "C" ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved == (off_t)-1) return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    ssize_t rc = read(fd, buf, count);
    int saved_errno = errno;
    lseek(fd, saved, SEEK_SET);
    errno = saved_errno;
    return rc;
}

// Newlib stubs for POSIX functions that may be missing
static size_t stub_strnlen(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}
static char* stub_strtok_r(char* s, const char* d, char** save) {
    (void)save;
    return strtok(s, d);
}
static long stub_ftello(FILE* f) { return ftell(f); }
static int  sh_fseek(FILE* f, long o, int w);
static int  stub_fseeko(FILE* f, long o, int w) { return sh_fseek(f, o, w); }
static int  stub_setenv(const char*, const char*, int) { return 0; }
static int  stub_unsetenv(const char*)               { return 0; }
static int  stub_posix_memalign(void** p, size_t a, size_t s) {
    *p = memalign(a, s);
    return *p ? 0 : 12; // ENOMEM
}

// ─── New stubs for batch 3 (all symbols unresolved in the latest run) ─────────

// dladdr: crash reporters call this to resolve their own address → return failure
// Dl_info layout: 4 pointers (fname, fbase, sname, saddr) — zero them all
static int stub_dladdr(const void*, void* info) {
    if (info) memset(info, 0, 4 * sizeof(void*));
    return 0;
}
// sigaltstack: crash reporters use this to set up signal alt-stack → no-op
static int stub_sigaltstack(const void*, void*) { return 0; }
// signal: forward to newlib (returns SIG_DFL on failure)
// strsignal: return a static string
static char g_signame_buf[32];
static char* stub_strsignal(int sig) {
    snprintf(g_signame_buf, sizeof(g_signame_buf), "Signal %d", sig);
    return g_signame_buf;
}
// sys_signame: Android/BSD symbol — pointer to array of signal name strings
static const char* g_sys_signames[32] = {
    "", "HUP","INT","QUIT","ILL","TRAP","ABRT","BUS",
    "FPE","KILL","USR1","SEGV","USR2","PIPE","ALRM","TERM",
    "STKFLT","CHLD","CONT","STOP","TSTP","TTIN","TTOU","URG",
    "XCPU","XFSZ","VTALRM","PROF","WINCH","IO","PWR","SYS"
};
// environ: standard POSIX pointer to environment strings — expose newlib's
extern char** environ;
// gmtime_r / localtime_r — forward to newlib (may already exist, but explicit shim)
static struct tm* stub_gmtime_r(const time_t* t, struct tm* tm_) {
    struct tm* r = gmtime(t);
    if (r && tm_) { *tm_ = *r; return tm_; }
    return nullptr;
}
static struct tm* stub_localtime_r(const time_t* t, struct tm* tm_) {
    struct tm* r = localtime(t);
    if (r && tm_) { *tm_ = *r; return tm_; }
    return nullptr;
}
// __memset_chk / __strchr_chk — Bionic security wrappers
static void* stub_memset_chk(void* d, int c, size_t n, size_t /*dstlen*/) {
    return memset(d, c, n);
}
static char* stub_strchr_chk(const char* s, int c, size_t /*slen*/) {
    return (char*)strchr(s, c);
}
static int stub___FD_SET_chk(int fd, void* set, size_t /*setsize*/) {
    if (set && fd >= 0 && fd < 1024) { ((uint32_t*)set)[fd/32] |= (1u << (fd%32)); }
    return 0;
}
static int stub___FD_ISSET_chk(int fd, const void* set, size_t /*setsize*/) {
    if (!set || fd < 0 || fd >= 1024) return 0;
    return (((const uint32_t*)set)[fd/32] >> (fd%32)) & 1;
}
// Process stubs — Switch has no fork/exec/wait
static int stub_fork()                              { return -1; }
static int stub_execve(const char*, char* const*, char* const*) { errno = ENOSYS; return -1; }
static int stub_waitpid(int, int*, int)             { errno = ECHILD; return -1; }
// Filesystem stubs missing from existing table
static int stub_symlink(const char*, const char*)   { errno = ENOSYS; return -1; }
static int stub_utimes(const char*, const void*)    { return 0; }
static char* stub_realpath(const char* p, char* out) {
    if (!p || !*p)
        return nullptr;

    // CryPak's Linux implementation expects normal POSIX absolute paths.
    // The Switch C runtime reports the current directory as "sdmc:/...", but
    // the rest of this compatibility layer canonicalizes that namespace to
    // "/switch/...". Returning the sdmc-prefixed form breaks the path model
    // used by OpenPacksCommon/OpenPackCommon and can prevent root FCData PAKs
    // from reaching ZipDir at all.
    bool callerOwnsBuffer = (out != nullptr);
    if (!out) {
        out = (char*)malloc(PATH_MAX);
        if (!out)
            return nullptr;
    }

    auto writeCanonical = [&](const std::string& value) -> char* {
        const std::string canonical = normalizeSwitchFsPath(value.c_str());
        if (canonical.size() >= PATH_MAX) {
            errno = ENAMETOOLONG;
            if (!callerOwnsBuffer)
                free(out);
            return nullptr;
        }
        if (p && strstr(p, "sidle_loop.caf")) {
            char caller[256];
            elfDescribePc((uint64_t)__builtin_return_address(0),
                          caller, sizeof(caller));
            compatLogFmt("realpath WRITE DIAG: out=%p len=%zu caller=%s",
                         (void*)out, canonical.size() + 1, caller);
        }
        memcpy(out, canonical.c_str(), canonical.size() + 1);
        if (kVerboseSuccessfulPathLogs)
            compatLogFmt("realpath RESULT: %s -> %s", p, out);
        return out;
    };

    if (strcmp(p, ".") == 0 || strcmp(p, "./") == 0) {
        char cwd[PATH_MAX];
        if (!::getcwd(cwd, sizeof(cwd))) {
            if (!callerOwnsBuffer)
                free(out);
            compatLogFmt("realpath RESULT: %s -> FAIL errno=%d", p, errno);
            return nullptr;
        }
        return writeCanonical(cwd);
    }

    // Wildcards are deliberately not resolved. CryPak passes patterns such as
    // "FCData/*.pak" here; returning failure makes AdjustFileName() construct
    // the absolute wildcard path from realpath(".").
    if (strpbrk(p, "*?[]") != nullptr) {
        if (!callerOwnsBuffer)
            free(out);
        errno = ENOENT;
        return nullptr;
    }

    // Shader cache files must never be considered by realpath() in this
    // experiment, even when an older run already materialized one on disk.
    // The previous check lived only in the PAK fallback below, so stale
    // Shaders/Cache/*.cgps files still passed the initial stat() and reached
    // CCGPShader_GL::mfLoad. Force these runtime cache artifacts to behave as
    // missing and let CryEngine select its embedded fallback.
    if (isShaderCacheLookupPath(p)) {
        if (!callerOwnsBuffer)
            free(out);
        errno = ENOENT;
        compatLogFmt("realpath SHADER CACHE BYPASS: %s", p);
        return nullptr;
    }

    struct stat st = {};
    if (::stat(p, &st) == 0) {
        if (p[0] == '/') {
            return writeCanonical(p);
        }

        char cwd[PATH_MAX];
        if (!::getcwd(cwd, sizeof(cwd))) {
            if (!callerOwnsBuffer)
                free(out);
            return nullptr;
        }

        std::string absolute = cwd;
        if (!absolute.empty() && absolute.back() != '/')
            absolute += '/';
        absolute += p;
        return writeCanonical(absolute);
    }

    // CryPak may keep normal game assets exclusively inside FCData/*.pak.
    // In that case a host-side stat() legitimately fails even though the guest
    // asset exists. Materialize a unique PAK entry at the requested virtual
    // path, then canonicalize it normally. This is particularly important for
    // scripts/classregistry.lua: Lua's loader calls realpath() before entering
    // the lexer, so returning failure leaves the parser with no valid source
    // filename even though the script is present in Scripts.pak.
    // Shader cache files are deliberately excluded from the PAK-backed
    // realpath fallback. These .cgps/.cgvp/.cgasm files are generated/runtime
    // cache artifacts and must stay unavailable here so CryEngine can take its
    // embedded shader fallback path instead of feeding a cached Cg program into
    // CCGPShader_GL::mfLoad on Switch.
    if (!isShaderCacheLookupPath(p) &&
        (strchr(p, '/') || strchr(p, '\\'))) {
        std::string materialized;
        if (tryMaterializePakPath(p, materialized)) {
            compatLogFmt("realpath PAK EXACT: %s -> %s", p, materialized.c_str());
            return writeCanonical(materialized);
        }

        // Keep the basename fallback for legacy content whose virtual path is
        // known only by filename. This is deliberately second: exact virtual
        // paths are authoritative and avoid collisions between PAKs.
        if (tryMaterializeUniquePakBasename(p)) {
            std::string normalizedOut = pakNormalizeName(p);
            // The basename helper writes using the normalized target path below;
            // use the canonical game-rooted path only when the file now exists.
            char cwd[PATH_MAX];
            if (::getcwd(cwd, sizeof(cwd))) {
                std::string absolute = cwd;
                if (!absolute.empty() && absolute.back() != '/')
                    absolute += '/';
                for (char c : normalizedOut) absolute += c;
                struct stat pakSt = {};
                if (::stat(absolute.c_str(), &pakSt) == 0 && S_ISREG(pakSt.st_mode)) {
                    compatLogFmt("realpath PAK BASENAME: %s -> %s", p, absolute.c_str());
                    return writeCanonical(absolute);
                }
            }
        }
    }

    if (!callerOwnsBuffer)
        free(out);
    errno = ENOENT;
    compatLogFmt("realpath RESULT: %s -> FAIL errno=%d", p, errno);
    return nullptr;
}
static int stub_readlink(const char*, char* buf, size_t sz) {
    if (sz > 0 && buf) buf[0] = '\0';
    errno = EINVAL; return -1;
}
static int stub_chdir(const char* path) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    if (::chdir(ioPath) == 0)
        return 0;
    if (ioPath) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath)
            return ::chdir(resolved.c_str());
    }
    return -1;
}
static int stub_isatty(int)             { return 0; }
// Network stubs
static int stub_setsockopt(int, int, int, const void*, unsigned) { errno = ENOTSUP; return -1; }
static int stub_accept(int, void*, void*)  { errno = ENOTSUP; return -1; }
static int stub_poll(void*, unsigned, int) { return 0; }
// User/group stubs
static int stub_setuid(unsigned) { return 0; }
static int stub_setgid(unsigned) { return 0; }
// popen/pclose stubs
static FILE* stub_popen(const char*, const char*) { return nullptr; }
static int   stub_pclose(FILE*)                   { return -1; }
// Terminal stubs
static int stub_tcgetattr(int, void*)         { errno = ENOTTY; return -1; }
static int stub_tcsetattr(int, int, const void*) { errno = ENOTTY; return -1; }
// malloc_usable_size — dlmalloc/newlib provides this
static size_t stub_malloc_usable_size(void* p) { return p ? malloc_usable_size(p) : 0; }
// Locale-variant char/string functions — ignore locale, call base version
static int stub_isdigit_l(int c, void*)   { return isdigit(c); }
static int stub_islower_l(int c, void*)   { return islower(c); }
static int stub_isupper_l(int c, void*)   { return isupper(c); }
static int stub_isxdigit_l(int c, void*)  { return isxdigit(c); }
static int stub_tolower_l(int c, void*)   { return tolower(c); }
static int stub_toupper_l(int c, void*)   { return toupper(c); }
static int stub_iswalpha_l(wint_t c, void*)   { return iswalpha(c); }
static int stub_iswblank_l(wint_t c, void*)   { return iswblank(c); }
static int stub_iswcntrl_l(wint_t c, void*)   { return iswcntrl(c); }
static int stub_iswdigit_l(wint_t c, void*)   { return iswdigit(c); }
static int stub_iswlower_l(wint_t c, void*)   { return iswlower(c); }
static int stub_iswprint_l(wint_t c, void*)   { return iswprint(c); }
static int stub_iswpunct_l(wint_t c, void*)   { return iswpunct(c); }
static int stub_iswspace_l(wint_t c, void*)   { return iswspace(c); }
static int stub_iswupper_l(wint_t c, void*)   { return iswupper(c); }
static int stub_iswxdigit_l(wint_t c, void*)  { return iswxdigit(c); }
static wint_t stub_towlower_l(wint_t c, void*) { return towlower(c); }
static wint_t stub_towupper_l(wint_t c, void*) { return towupper(c); }
static int stub_strcoll_l(const char* a, const char* b, void*) { return strcoll(a, b); }
static size_t stub_strxfrm_l(char* d, const char* s, size_t n, void*) { return strxfrm(d, s, n); }
static size_t stub_strftime_l(char* s, size_t m, const char* f, const struct tm* t, void*) {
    return strftime(s, m, f, t);
}
static int stub_wcscoll_l(const wchar_t* a, const wchar_t* b, void*) { return wcscoll(a, b); }
static size_t stub_wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, void*) {
    return wcsxfrm(d, s, n);
}
// Math: forward to newlib (these exist but may be missing from our list)
static double stub_acosh(double x)  { return acosh(x); }
static double stub_asinh(double x)  { return asinh(x); }
static double stub_atanh(double x)  { return atanh(x); }
static double stub_log1p(double x)  { return log1p(x); }
static double stub_expm1(double x)  { return expm1(x); }
static double stub_difftime(time_t a, time_t b) { return difftime(a, b); }
// fesetround — forward to newlib
static int stub_fesetround(int r) { return fesetround(r); }
// strptime — newlib stub (may not exist in devkitA64 newlib)
static char* stub_strptime(const char*, const char*, struct tm*) { return nullptr; }
// clearerr / fileno / stat ABI / fdopen
static void  stub_clearerr(FILE* f)              { clearerr(f); }
static int   stub_fileno(FILE* f)                { return f ? ::fileno(f) : -1; }

// Guest libraries are Android arm64 binaries, while this compatibility layer
// is compiled against devkitA64/newlib. Their struct stat ABI is therefore not
// interchangeable. Bionic arm64 lays out st_size at offset 48 and the three
// timespecs starting at offsets 72/88/104, for a 128-byte structure.
struct AndroidArm64Stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atim_sec;
    int64_t  st_atim_nsec;
    int64_t  st_mtim_sec;
    int64_t  st_mtim_nsec;
    int64_t  st_ctim_sec;
    int64_t  st_ctim_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
};

static_assert(sizeof(AndroidArm64Stat) == 128, "AndroidArm64Stat size");
static_assert(offsetof(AndroidArm64Stat, st_size) == 48, "AndroidArm64Stat st_size");
static_assert(offsetof(AndroidArm64Stat, st_mtim_sec) == 88, "AndroidArm64Stat st_mtime");

static void fillAndroidArm64Stat(const struct stat& nativeSt, void* out) {
    if (!out)
        return;

    AndroidArm64Stat guest = {};
    guest.st_mode = static_cast<uint32_t>(nativeSt.st_mode);
    guest.st_nlink = static_cast<uint32_t>(nativeSt.st_nlink);
    guest.st_uid = static_cast<uint32_t>(nativeSt.st_uid);
    guest.st_gid = static_cast<uint32_t>(nativeSt.st_gid);
    guest.st_size = static_cast<int64_t>(nativeSt.st_size);
    guest.st_blksize = static_cast<int32_t>(nativeSt.st_blksize);
    guest.st_blocks = static_cast<int64_t>(nativeSt.st_blocks);
    guest.st_atim_sec = static_cast<int64_t>(nativeSt.st_atime);
    guest.st_mtim_sec = static_cast<int64_t>(nativeSt.st_mtime);
    guest.st_ctim_sec = static_cast<int64_t>(nativeSt.st_ctime);
    std::memcpy(out, &guest, sizeof(guest));
}

static int stub_stat(const char* p, struct stat* ignored) {
    (void)ignored;
    if (!p) {
        errno = EINVAL;
        return -1;
    }

    const std::string ioPathStorage = normalizeSwitchFsPath(p);
    const char* ioPath = ioPathStorage.c_str();

    struct stat nativeSt = {};
    int rc = ::stat(ioPath, &nativeSt);
    if (rc != 0) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath)
            rc = ::stat(resolved.c_str(), &nativeSt);
    }

    if (rc != 0)
        return rc;

    fillAndroidArm64Stat(nativeSt, ignored);
    return 0;
}

static int stub_fstat64(int fd, void* out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }

    struct stat st = {};
    const int rc = ::fstat(fd, &st);
    if (rc != 0)
        return rc;

    fillAndroidArm64Stat(st, out);
    return 0;
}
static FILE* stub_fdopen(int fd, const char* m)  { (void)fd; (void)m; return nullptr; }
// tmpfile — forward to newlib
static FILE* stub_tmpfile()                      { return tmpfile(); }

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);
extern void compatLogFlush();
extern void elfDescribePc(uint64_t pc, char* buf, size_t sz);

// Game-initiated termination is otherwise invisible (process just returns to
// the Home menu with nothing in the log) — record it, plus the caller's
// return address so the log names the code path that pulled the trigger.
static void logTermCaller(const char* what, void* ret_addr) {
    char where[256];
    elfDescribePc((uint64_t)ret_addr, where, sizeof(where));
    compatLogFmt("%s from %s", what, where);
    compatLogFlush();
}

// Walk the AArch64 frame-pointer chain ([x29] = caller fp, [x29+8] = lr) and
// symbolize every return address. NDK arm64 builds keep frame pointers, so
// this names the whole game call path that led to abort/exit. Each line is
// flushed as it's written — if the walk hits a bogus fp and faults, everything
// up to that frame is already on disk (and we were dying anyway).
static void logBacktrace(void* fp) {
    struct Frame { Frame* fp; void* lr; };
    Frame* f = (Frame*)fp;
    for (int i = 0; i < 24 && f; i++) {
        if (((uintptr_t)f & 0xF) != 0) { compatLogFmt("  bt[%d]: fp misaligned — stop", i); break; }
        void* lr = f->lr;
        if (!lr) break;
        char where[256];
        elfDescribePc((uint64_t)lr, where, sizeof(where));
        compatLogFmt("  bt[%d]: %s", i, where);
        compatLogFlush();
        Frame* next = f->fp;
        if (next <= f) break;   // frames must strictly ascend the stack
        f = next;
    }
    compatLogFlush();
}
static void sh_exit(int code) {
    compatLogFmt("game called exit(%d)", code);
    logTermCaller("exit called", __builtin_return_address(0));
    logBacktrace(__builtin_frame_address(0));
    exit(code);
}
static void sh_abort() {
    compatLog("game called abort()");
    logTermCaller("abort called", __builtin_return_address(0));
    logBacktrace(__builtin_frame_address(0));
    abort();
}

extern "C" void __stack_chk_fail(void);
static void sh_stack_chk_fail(void) {
    void* ra = __builtin_return_address(0);
    char where[256];
    elfDescribePc((uint64_t)ra, where, sizeof(where));
    compatLogFmt("STACK CHK FAIL from %p %s", ra, where);
    compatLogFlush();
    __stack_chk_fail();
}
static void sh_exit_raw(int code) {
    compatLogFmt("game called _exit(%d)", code);
    logTermCaller("_exit called", __builtin_return_address(0));
    exit(code);
}

// ─── Guarded free/realloc ─────────────────────────────────────────────────────
// Build 60 forensics: the game free()d a pointer into our NRO's RX segment
// (a JNI string constant), and newlib's _free_r faulted writing a free-list
// link into read-only memory. Android's allocator tolerates some of this and
// ART hands out heap copies, so be equally forgiving: only pass real heap
// pointers to the allocator, and log (symbolized, rate-limited) who tried.
// The heap is one contiguous region that only ever grows, so its extent is
// worth caching: the validation below needs half a dozen range checks per
// free, and an svcQueryMemory apiece would put a syscall storm in the
// allocator's hot path.
static uintptr_t g_heap_lo = 0, g_heap_hi = 0;

static bool memIsHeap(const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (a >= g_heap_lo && a < g_heap_hi) return true;
    MemoryInfo mi = {};
    u32 pageinfo = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pageinfo, (u64)p))) return false;
    if (mi.type != MemType_Heap) return false;
    g_heap_lo = (uintptr_t)mi.addr;
    g_heap_hi = (uintptr_t)mi.addr + mi.size;
    return true;
}

// newlib's malloc_chunk on aarch64. The mem pointer callers hold is chunk+16,
// so the size field sits at p-8, and fd/bk — the pointers the consolidation
// code stores *through* — are at chunk+16 and chunk+24.
struct NlChunk {
    size_t  prev_size;
    size_t  size;
    NlChunk* fd;
    NlChunk* bk;
};
static const size_t NL_PREV_INUSE = 1;
static inline size_t nlSize(const NlChunk* c) { return c->size & ~(size_t)7; }

static bool nlChunkSane(const NlChunk* c) {
    if (!c || ((uintptr_t)c & 15) != 0) return false;
    if (!memIsHeap(c) || !memIsHeap((const char*)c + 16)) return false;
    size_t sz = nlSize(c);
    if (sz < 32 || (sz & 15) != 0) return false;             // MINSIZE, 16-aligned
    if (!memIsHeap((const char*)c + sz)) return false;
    return true;
}

// True if handing p to _free_r would make it walk into something that is not a
// chunk. This is the fault Brain It On hits 155 times: free() decides to
// consolidate with a neighbour, reads that neighbour's fd, and stores through
// it — but the neighbour was never a chunk, so fd is zero and the store lands
// on address 0x18. Predicting the consolidation is the only way to catch it,
// because the pointer being freed is itself perfectly valid.
//
// fd/bk are only tested for null, deliberately. The head of a bin lives in
// newlib's static arena rather than the heap, so a legitimate fd can point
// outside it — anything stricter would reject good frees and leak.
static bool freeWouldCorrupt(void* p) {
    NlChunk* c = (NlChunk*)((char*)p - 16);
    if (!nlChunkSane(c)) return true;

    // Forward: newlib merges with the next chunk when the chunk after *that*
    // reports its predecessor as free.
    NlChunk* next = (NlChunk*)((char*)c + nlSize(c));
    if (!nlChunkSane(next)) return true;
    NlChunk* after = (NlChunk*)((char*)next + nlSize(next));
    if (memIsHeap(after) && !(after->size & NL_PREV_INUSE)) {
        if (!next->fd || !next->bk) return true;
    }

    // Backward: the same one chunk earlier, reached through prev_size.
    if (!(c->size & NL_PREV_INUSE)) {
        if (c->prev_size < 32 || (c->prev_size & 15) != 0) return true;
        NlChunk* prev = (NlChunk*)((char*)c - c->prev_size);
        if (!nlChunkSane(prev)) return true;
        if (!prev->fd || !prev->bk) return true;
    }
    return false;
}
// A pointer that is in the heap page range (memIsHeap) can still be something
// newlib never handed out: an interior pointer into a larger chunk, a slice out
// of a memalign'd block, or a sub-allocation from a foreign allocator (il2cpp /
// libgpg define their own operator new). Passing any of those to _free_r walks a
// bogus chunk header and corrupts the free-list, which then faults on a *later*
// free (observed: 155 Unity ctors crashing in _free_r+0x78's list insert on the
// 1.6.234 Brain It On! build). newlib on aarch64 always returns pointers aligned
// to MALLOC_ALIGNMENT (16) with a usable size that fits the heap, so anything
// failing those cheap, zero-false-positive checks is provably not a real chunk —
// leak it rather than corrupt the arena.
static bool looksLikeNewlibChunk(void* p) {
    if (((uintptr_t)p & 0xF) != 0) return false;          // newlib pointers are 16-aligned
    size_t us = malloc_usable_size(p);                     // reads header; in-heap so fault-safe
    if (us == 0 || us > (256u * 1024u * 1024u)) return false;
    // usable_size only reads p-8, so a foreign pointer whose preceding bytes
    // happen to look like a plausible size sails through. Walk the chunk graph
    // and check the consolidation newlib would actually perform.
    if (freeWouldCorrupt(p)) return false;
    return true;
}
// Counted so the fault report can say whether the game's frees reach us at
// all. Zero SKIP lines is ambiguous on its own: it means either every pointer
// passed validation, or nothing ever came through here.
volatile unsigned long g_sh_free_calls = 0;
volatile unsigned long g_sh_malloc_calls = 0;
volatile unsigned long g_sh_calloc_calls = 0;
volatile unsigned long g_sh_realloc_calls = 0;
unsigned long shimFreeCallCount(void) { return g_sh_free_calls; }
void shimAllocCounts(unsigned long* m, unsigned long* c, unsigned long* r, unsigned long* f) {
    if (m) *m = g_sh_malloc_calls;
    if (c) *c = g_sh_calloc_calls;
    if (r) *r = g_sh_realloc_calls;
    if (f) *f = g_sh_free_calls;
}

// malloc and calloc were pointing straight at newlib, so nothing counted them.
// Knowing how many allocations the game makes against how many frees reach us
// is the difference between "the game barely allocates through libc" and "it
// allocates through libc and frees through something else entirely" — and only
// the second explains a fault in _free_r with our free shim barely called.
// Check the arena immediately before delegating.
//
// Sampling av->top every 20ms never caught anything, and the reason is
// structural: sysmalloc sets av->top to the NEW top before freeing the old
// one, so by the time anything else can look, the pointer is valid again. The
// bad value is old_top, a local, read from av->top at the instant the game
// called malloc. This is that instant — the last point we control before
// newlib reads it.
static void arenaGate(const char* who) {
    static int reported = 0;
    if (reported >= 3) return;
    char why[400];
    if (shimHeapCheckFast(why, sizeof(why))) return;
    reported++;
    compatLogFmt("ARENA: bad on entry to %s during %s ctor[%d] — %s",
                 who, elfCurrentModule(), elfCurrentCtor(), why);
}

static void* sh_malloc(size_t n) { g_sh_malloc_calls++; arenaGate("malloc"); return malloc(n); }
static void* sh_calloc(size_t a, size_t b) { g_sh_calloc_calls++; arenaGate("calloc"); return calloc(a, b); }

// ─── Heap integrity walk ────────────────────────────────────────────────────
// The arena is corrupt before anything faults: _malloc_r calls _free_r
// internally, so the constructors that "fail" are simply the first ones to
// allocate after the damage, and the render thread wedges for the same reason
// when SDL_ttf allocates. Constructor 117 is the first to fault in every run,
// which means something in the 116 before it does the damage — but nothing
// logs, so which one is unknown.
//
// Chunks are contiguous, so walking forward from a block allocated before any
// game code ran covers everything allocated since. This reports where the
// chain first stops making sense, and the caller can run it between
// constructors to name the one that broke it.
extern "C" void* _sbrk_r(struct _reent*, ptrdiff_t);   // current break = arena end

// newlib's malloc arena. It lives in our .bss, not in the heap — which is why
// a dozen clean heap walks meant nothing. av_[2] is the top chunk pointer, and
// sysmalloc frees chunk2mem(top) when it extends the arena; that is precisely
// the call that faults, with top pointing 375MB past the break at memory that
// was never a chunk. Watching this one pointer catches the moment it goes bad.
extern "C" void*  __malloc_av_[];
extern "C" char*  __malloc_sbrk_base;
extern "C" size_t __malloc_max_sbrked_mem;

// The end of what newlib has actually taken from the system.
//
// _sbrk_r(_REENT, 0) is not usable for this: the walk and the extent report
// call it identically and disagree — the walk covered 68000 chunks and claimed
// to reach the break while the report put the break 4KB above the base, and
// 68000 chunks do not fit in 4KB. newlib's own accounting does not have that
// problem, and it is the number the allocator itself works from.
static uintptr_t arenaEnd(void) {
    return (uintptr_t)__malloc_sbrk_base + (uintptr_t)__malloc_max_sbrked_mem;
}

static void*  g_heap_anchor = nullptr;

// One-word description of where an address lives, for fault reports. Naming
// the region turns "x6 is some number" into "free() was called on something
// that was never heap".
const char* shimAddrRegion(uint64_t a) {
    if (!a) return "null";
    MemoryInfo mi = {}; u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return "unmapped";
    switch (mi.type) {
        case MemType_Heap:              return "heap";
        case MemType_CodeStatic:
        case MemType_CodeMutable:       return "code";
        // The game's modules are mapped with svcMapProcessCodeMemory, so a
        // chunk pointer landing here means free() was handed something out of
        // the game's own image rather than an allocation.
        case MemType_ModuleCodeStatic:
        case MemType_ModuleCodeMutable: return "game-module";
        case MemType_MappedMemory:
        case MemType_WeirdMappedMem:    return "mapped";
        case MemType_ThreadLocal:       return "tls";
        case MemType_Unmapped:          return "unmapped";
        default:                        return "other";
    }
}

void shimHeapAnchor(void) {
    if (!g_heap_anchor) g_heap_anchor = malloc(64);
}

// Coverage of the last walk. A clean result means nothing unless it is known
// how much ground was covered — twice now I have reported "the heap is clean"
// when the walk had stopped after a few hundred chunks and never looked at the
// rest.
static int         g_walk_steps  = 0;
static const char* g_walk_stop   = "not run";

// Heap extent and how much of it is left. The block _free_r faults on is a
// zero-size chunk at a 256MB-aligned address, which is what the top of the
// arena looks like when the break has run into the end of the region — so
// whether that address IS the end is the difference between "the game
// corrupted the heap" and "the game ran out of it".
void shimHeapExtent(uint64_t* lo, uint64_t* hi, uint64_t* brk) {
    if (!g_heap_lo) { MemoryInfo mi = {}; u32 pi = 0;
                      if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&g_heap_lo)))
                          { } }
    // Force the cache to populate off a known heap pointer.
    if (!g_heap_lo && g_heap_anchor) memIsHeap(g_heap_anchor);
    if (lo)  *lo  = g_heap_lo;
    if (hi)  *hi  = g_heap_hi;
    if (brk) *brk = (uint64_t)arenaEnd();
}

void shimHeapWalkStats(int* steps, const char** stop) {
    if (steps) *steps = g_walk_steps;
    if (stop)  *stop  = g_walk_stop;
}

// Arena head only — no walking. Cheap enough to run fifty times a second,
// which is what turns "one of these constructors" into "this one".
bool shimHeapCheckFast(char* why, size_t whysz) {
    const uintptr_t base = (uintptr_t)__malloc_sbrk_base;
    if (!base) return true;
    const uintptr_t end = arenaEnd();
    const uintptr_t top = (uintptr_t)__malloc_av_[2];
    if (!top) { snprintf(why, whysz, "av->top is null"); return false; }
    if (top < base || top >= end) {
        snprintf(why, whysz, "av->top=%p outside [%p,%p)",
                 (void*)top, (void*)base, (void*)end);
        return false;
    }
    const size_t tsz = nlSize((const NlChunk*)top);
    if (tsz == 0) {
        snprintf(why, whysz, "av->top=%p has size 0 (points at zeroed memory)",
                 (void*)top);
        return false;
    }
    if (top + tsz > end + 0x1000) {
        snprintf(why, whysz, "av->top=%p size %zu runs past arena end %p",
                 (void*)top, tsz, (void*)end);
        return false;
    }
    return true;
}

bool shimHeapCheck(char* why, size_t whysz) {
    if (!g_heap_anchor) return true;

    // sbrk(0) is the true end of the arena. The previous version stopped at the
    // first zero-size chunk, which it reached after 612 steps every run — so it
    // examined a fraction of the heap and reported the rest clean without ever
    // looking at it. That is why the corruption below went unseen: it sits past
    // that point.
    const uintptr_t arena_end = arenaEnd();
    g_walk_steps = 0;
    g_walk_stop  = "completed";

    // Check the arena before the heap. The top chunk must sit between the
    // base sbrk handed out and the current break; anything else means the
    // allocator's own state has been overwritten, and no amount of walking
    // chunks would ever show it.
    {
        const uintptr_t top  = (uintptr_t)__malloc_av_[2];
        const uintptr_t base = (uintptr_t)__malloc_sbrk_base;
        if (base && top && (top < base || top >= arena_end)) {
            snprintf(why, whysz,
                     "malloc arena top=%p is outside [%p, %p) — newlib's av_ "
                     "has been overwritten (av_ at %p)",
                     (void*)top, (void*)base, (void*)arena_end,
                     (void*)__malloc_av_);
            g_walk_stop = "arena top invalid";
            return false;
        }
        // Being in range is not enough, and that is exactly why this check has
        // never fired. At the fault, top IS in range — 355MB into a 512MB
        // arena — it just points at memory that is entirely zero. sysmalloc
        // then frees chunk2mem(top), reads a null forward pointer out of those
        // zeros, and stores through it.
        //
        // A real top chunk has a size, and that size reaches the end of the
        // arena: it is by definition the last chunk. Checking the size is what
        // catches "top points at nothing", which is the actual failure.
        if (base && top) {
            const NlChunk* tc  = (const NlChunk*)top;
            const size_t   tsz = nlSize(tc);
            if (tsz == 0) {
                snprintf(why, whysz,
                         "malloc arena top=%p has size 0 — it points at zeroed "
                         "memory, so the next allocation will free a chunk that "
                         "is not there (av_ at %p, arena ends %p)",
                         (void*)top, (void*)__malloc_av_, (void*)arena_end);
                g_walk_stop = "arena top has no size";
                return false;
            }
            if (top + tsz > arena_end + 0x1000) {
                snprintf(why, whysz,
                         "malloc arena top=%p size %zu runs past the arena end "
                         "%p (av_ at %p)",
                         (void*)top, tsz, (void*)arena_end, (void*)__malloc_av_);
                g_walk_stop = "arena top oversized";
                return false;
            }
        }
    }
    // A break of 0 or -1 would make every bound below trivially true and turn
    // the whole walk into an immediate "clean" — say so rather than pretend.
    if (arena_end < 0x1000) { g_walk_stop = "no arena accounting"; return true; }

    // Start where newlib actually started: __malloc_sbrk_base is the first
    // address it took from the system, so it is the arena base by definition.
    //
    // The previous version scanned upward from the heap *region* base for
    // something that looked like a chunk, and duly found one — the region base
    // holds libnx's own data, whose first two words are pointers that happened
    // to pass a size check. Everything after that was a misread, which is where
    // the "prev_size 0 != prev size 4096" report came from.
    const NlChunk* c = (const NlChunk*)__malloc_sbrk_base;
    if (!__malloc_sbrk_base) { g_walk_stop = "no arena base"; return true; }

    size_t prev_sz = 0;
    const NlChunk* prev_c = nullptr;
    for (int i = 0; i < 400000; i++) {
        g_walk_steps = i;
        if (!memIsHeap(c)) { g_walk_stop = "left the heap region"; return true; }
        if ((uintptr_t)c + 32 > arena_end) { g_walk_stop = "reached the break"; return true; }
        size_t sz = nlSize(c);

        // The invariant the fault actually violates. When PREV_INUSE is clear
        // the previous chunk is free, and prev_size must equal its size —
        // that pair is the boundary tag. _free_r trusts it to walk backwards,
        // so a mismatch is precisely what sends it into untouched memory with a
        // null forward pointer, which is the fault we keep seeing.
        if (i > 0 && !(c->size & NL_PREV_INUSE) && c->prev_size != prev_sz) {
            // Dump the overrun block's contents. The chain advanced correctly
            // to get here, so this is a real boundary and these bytes are what
            // the writer left behind — a string, a struct, a repeated pattern:
            // whatever it is will identify the owner far faster than tracing
            // allocations backwards would.
            char hex[3 * 48 + 1] = {}, asc[48 + 1] = {};
            const unsigned char* d = (const unsigned char*)prev_c + 16;
            size_t n = prev_sz > 16 ? prev_sz - 16 : 0;
            if (n > 40) n = 40;
            for (size_t j = 0; j < n; j++) {
                snprintf(hex + j * 3, 4, "%02x ", d[j]);
                asc[j] = (d[j] >= 32 && d[j] < 127) ? (char)d[j] : '.';
            }
            snprintf(why, whysz,
                     "chunk %p (step %d): PREV_INUSE clear but prev_size %llu != "
                     "prev size %zu | prev %p data: %s| %s",
                     (const void*)c, i, (unsigned long long)c->prev_size, prev_sz,
                     (const void*)prev_c, hex, asc);
            return false;
        }
        const NlChunk* this_prev_c  = prev_c;
        const size_t   this_prev_sz = prev_sz;
        (void)this_prev_sz;
        prev_c  = c;
        prev_sz = sz;
        // Size zero is the end of the arena, not damage. memIsHeap answers for
        // the whole reserved heap region, but newlib has only sbrk'd part of
        // it — walk past the top chunk and the rest is mapped, readable and
        // entirely zero. Real damage shows up as a garbage non-zero size, so
        // this stops the walk rather than reporting it. (The previous run
        // reported exactly this at the same address and step in two different
        // modules, which is what a fixed arena boundary looks like and what
        // corruption does not.)
        if (sz == 0) {
            snprintf(why, whysz, "chunk %p has size 0 below the break (step %d)",
                     (const void*)c, i);
            return false;
        }
        if (sz < 32 || (sz & 15) != 0) {
            snprintf(why, whysz, "chunk %p has bad size %zu (step %d)", (const void*)c, sz, i);
            return false;
        }
        const NlChunk* next = (const NlChunk*)((const char*)c + sz);
        if (next <= c) {
            snprintf(why, whysz, "chunk %p does not advance (step %d)", (const void*)c, i);
            return false;
        }
        if (!memIsHeap(next)) return true;          // reached the top

        // The size chain being intact is not the same as the heap being
        // healthy, which is what the last run showed: it walked cleanly right
        // up to the constructor that faulted. The value that faults is a bin
        // pointer, and those live in the *body* of a free chunk, not in the
        // chain — so a write into freed memory zeroes them while leaving every
        // size perfectly valid.
        //
        // A chunk is free when the following chunk says its predecessor is not
        // in use. Any such chunk is on a bin, so its fd and bk are non-null by
        // construction; a null one is precisely the state that kills _free_r
        // at the unlink.
        // The top chunk belongs to no bin, so its fd and bk are whatever was
        // last there — reading them as bin pointers is what produced four
        // "corruption" reports in a row, including one at the first constructor
        // of the first module, before any game code had run. av_[2] names it
        // exactly, so skip it by identity rather than by heuristic.
        if (c == (const NlChunk*)__malloc_av_[2]) { c = next; continue; }

        if (!(next->size & NL_PREV_INUSE)) {
            if (!c->fd || !c->bk) {
                // Report the neighbours too. Once one size is misread every
                // later "chunk" is user data reinterpreted as a header, and
                // fd/bk full of small integers is exactly what that looks
                // like — so a plausible predecessor means real corruption and
                // an implausible one means the walk lost sync earlier.
                g_walk_stop = "null bin pointers";
                snprintf(why, whysz,
                         "free chunk %p (size %zu) fd=%p bk=%p at step %d; "
                         "prev %p size %zu",
                         (const void*)c, sz, (const void*)c->fd, (const void*)c->bk,
                         i, (const void*)this_prev_c, this_prev_sz);
                return false;
            }
        }
        c = next;
    }
    g_walk_stop = "hit the step limit";
    return true;
}

static void sh_free(void* p) {
    if (!p) return;
    g_sh_free_calls++;
    arenaGate("free");

    // Android's original CMTSafeHeap::Free() calls plain ::free(p) on Linux.
    // Do not second-guess the allocator's chunk metadata here: pointers coming
    // from CrySystem/CMTSafeHeap can have allocator headers that differ from
    // the conservative newlib chunk heuristic above. For this A/B experiment,
    // only reject addresses that are not part of a Switch heap at all.
    if (!memIsHeap(p)) {
        static int warned = 0;
        if (warned < 20) {
            warned++;
            char where[256];
            elfDescribePc((uint64_t)__builtin_return_address(0), where, sizeof(where));
            compatLogFmt("free: SKIP non-heap ptr %p from %s", p, where);
        }
        return;
    }

    free(p);
}
static void* sh_realloc(void* p, size_t n) {
    g_sh_realloc_calls++;
    arenaGate("realloc");

    if (!p)
        return malloc(n);

    // Do not apply free()-specific chunk heuristics to realloc(). A pointer can
    // be a valid live newlib allocation even when freeWouldCorrupt() cannot
    // prove that the surrounding free-list topology is safe. Rejecting such a
    // pointer changes realloc semantics into malloc+memcpy+leak and changes the
    // heap layout, which is particularly sensitive during CryEngine shader
    // preprocessing.
    if (memIsHeap(p)) {
        const size_t oldUsable = malloc_usable_size(p);

        if (kVerboseAllocatorLogs) {
            char where[256];
            elfDescribePc((uint64_t)__builtin_return_address(0),
                          where, sizeof(where));
            compatLogFmt("realloc: DIRECT heap ptr=%p size=%zu old_usable=%zu from %s",
                         p, n, oldUsable, where);
        }

        return realloc(p, n);
    }

    // Keep protection for pointers that are not part of the Switch heap at all.
    // There is no trustworthy old size/owner information for these pointers,
    // so do not pass them to newlib realloc.
    char where[256];
    elfDescribePc((uint64_t)__builtin_return_address(0),
                  where, sizeof(where));

    compatLogFmt("realloc: SKIP non-heap ptr=%p size=%zu from %s",
                 p, n, where);
    return malloc(n);
}

// ─── /dev/urandom virtual fd ─────────────────────────────────────────────────
// libc++'s std::random_device ctor opens /dev/urandom — through bionic's
// fortified __open_2, so no logged shim showed the failure. The path doesn't
// exist on Switch, the ctor got -1, and __throw_system_error → abort() killed
// the game ~170s in (confirmed by backtrace, build 54 log). Serve the open on
// a magic fd backed by the Switch CSRNG instead.
static const int URANDOM_FD = 0x55AA;
static int devUrandomOpen(const char* p) {
    if (p && (strcmp(p, "/dev/urandom") == 0 || strcmp(p, "/dev/random") == 0)) {
        compatLogFmt("open %s → CSRNG virtual fd", p);
        return URANDOM_FD;
    }
    return -1;
}
static ssize_t sh_read(int fd, void* b, size_t n) {
    if (fd == URANDOM_FD) { if (b && n) randomGet(b, n); return (ssize_t)n; }
    return read(fd, b, n);
}
static int sh_close(int fd) {
    if (fd == URANDOM_FD) return 0;
    return close(fd);
}

// write() routed through the log for stdout/stderr — libc++abi terminate
// messages ("terminating with uncaught exception of type ...") land on fd 2.
static ssize_t sh_write(int fd, const void* buf, size_t n) {
    if ((fd == 1 || fd == 2) && buf && n > 0) {
        char tmp[512];
        size_t c = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
        memcpy(tmp, buf, c);
        tmp[c] = '\0';
        while (c > 0 && (tmp[c - 1] == '\n' || tmp[c - 1] == '\r')) tmp[--c] = '\0';
        if (c > 0) compatLogFmt("game %s: %s", fd == 2 ? "stderr" : "stdout", tmp);
        return (ssize_t)n;
    }
    return write(fd, buf, n);
}

// ─── Expansion files ─────────────────────────────────────────────────────────
// Where this game's OBBs were installed, and which package they belong to. A
// game that hardcodes /sdcard/Android/obb/<pkg>/main.<ver>.<pkg>.obb — plenty
// do, rather than calling getObbDir() — would otherwise open nothing and
// report its own data as missing.
static std::string g_obb_dir;
static std::string g_obb_pkg;
void compatSetObbDir(const char* dir, const char* pkg) {
    g_obb_dir = dir ? dir : "";
    g_obb_pkg = pkg ? pkg : "";
}
// Returns the rewritten path, or an empty string to leave the call alone.
static std::string obbRemap(const char* path) {
    if (!path || g_obb_dir.empty()) return "";
    return obb::remapPath(path, g_obb_pkg, g_obb_dir);
}

static std::string cdataToFcdata(const char* path) {
    if (!path || !*path)
        return "";

    std::string p = path;
    for (char& c : p) {
        if ((unsigned char)c == 92)
            c = '/';
    }

    std::string lower = asciiLower(p);

    if (lower == "cdata")
        return "FCData";
    if (lower.rfind("cdata/", 0) == 0)
        return "FCData/" + p.substr(6);

    // After the realpath fix above, CryPak may pass absolute CData paths to
    // opendir/open/fopen. Remap only when CData is exactly below the current
    // game working directory; do not rewrite arbitrary system paths.
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof(cwd))) {
        std::string root = cwd;
        for (char& c : root) {
            if ((unsigned char)c == 92)
                c = '/';
        }
        std::string rootLower = asciiLower(root);
        if (!root.empty() && root.back() == '/')
            root.pop_back(), rootLower.pop_back();

        const std::string absPrefix = rootLower + "/cdata";
        if (lower == absPrefix)
            return root + "/FCData";
        if (lower.rfind(absPrefix + "/", 0) == 0)
            return root + "/FCData/" + p.substr(absPrefix.size() + 1);
    }

    return "";
}


// Missing PAK compatibility.
// CryPak can throw ZipDir::Error when an Android-packaging path requests a
// PAK that is not present on the Switch filesystem. Treat every missing PAK as
// an empty, valid ZIP archive instead of allowing the exception to escape.
// Only read-mode opens are handled here; existing PAK files are never changed.
static bool isPakArchivePath(const char* path) {
    if (!path || !*path)
        return false;

    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;

    const size_t len = std::strlen(base);
    return len >= 4 &&
           std::tolower((unsigned char)base[len - 4]) == '.' &&
           std::tolower((unsigned char)base[len - 3]) == 'p' &&
           std::tolower((unsigned char)base[len - 2]) == 'a' &&
           std::tolower((unsigned char)base[len - 1]) == 'k';
}

static FILE* makeEmptyPak(const char* path, const char* mode) {
    // Standard ZIP End Of Central Directory for an archive with zero entries.
    // CryEngine's ZipDir reader accepts PAK files through this ZIP structure.
    static const unsigned char empty_zip_eocd[] = {
        0x50, 0x4B, 0x05, 0x06,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };

    if (!path || !mode)
        return nullptr;

    // tmpfile() is not reliable on the Switch/newlib runtime used by this
    // project. Create the missing archive at the exact path the game requested,
    // then reopen it normally so ZipDir sees an ordinary file with a stable
    // pathname and descriptor.
    FILE* out = fopen(path, "wb");
    if (!out)
        return nullptr;

    const size_t written = fwrite(empty_zip_eocd, 1, sizeof(empty_zip_eocd), out);
    const int close_rc = fclose(out);
    if (written != sizeof(empty_zip_eocd) || close_rc != 0)
        return nullptr;

    FILE* f = fopen(path, mode);
    if (!f)
        return nullptr;

    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    compatLogFmt("fopen FALLBACK: %s -> created empty PAK", path);
    return f;
}

static std::string pakNormalizeName(const char* name) {
    std::string out = name ? name : "";
    for (char& c : out) {
        if ((unsigned char)c == 92) c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/')
        out.erase(0, 2);
    return out;
}

static uint16_t pakRd16(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t pakRd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool pakReadExact(FILE* f, void* dst, size_t n) {
    return n == 0 || fread(dst, 1, n, f) == n;
}

static bool pakInflateRaw(const unsigned char* src, size_t srcSize,
                          unsigned char* dst, size_t dstSize) {
    if (!src || !dst || srcSize > 0xffffffffu || dstSize > 0xffffffffu)
        return false;

    z_stream zs = {};
    zs.next_in = const_cast<unsigned char*>(src);
    zs.avail_in = (unsigned int)srcSize;
    zs.next_out = dst;
    zs.avail_out = (unsigned int)dstSize;

    const char* version = zlibVersion();
    if (!version || inflateInit2_(&zs, -15, version, (int)sizeof(z_stream)) != 0)
        return false;

    // Keep these zlib status/action values local because this source intentionally
    // declares the zlib ABI itself and does not depend on zlib.h being installed.
    constexpr int kZOk = 0;
    constexpr int kZStreamEnd = 1;
    constexpr int kZBufError = -5;
    constexpr int kZFinish = 4;

    int rc = kZOk;
    while (rc == kZOk && zs.avail_out > 0)
        rc = inflate(&zs, kZFinish);

    // Z_BUF_ERROR can be returned when the output buffer is exactly full before
    // zlib gets a chance to report stream end. Treat that as success only when
    // all requested output bytes were produced; otherwise the stream is bad.
    const bool ok = (zs.total_out == dstSize) &&
                    (rc == kZStreamEnd || rc == kZBufError);
    if (!ok) {
        (void)srcSize;
        (void)dstSize;
        (void)rc;
        (void)zs;
    }

    inflateEnd(&zs);
    return ok;
}

static bool pakFindEntry(FILE* pak, const std::string& wanted,
                         uint32_t& localOffset, uint32_t& compressedSize,
                         uint32_t& uncompressedSize, uint16_t& method,
                         uint32_t& expectedCrc) {
    if (!pak || fseek(pak, 0, SEEK_END) != 0)
        return false;

    const long fileSize = ftell(pak);
    if (fileSize < 22)
        return false;

    const size_t tailSize = (size_t)((fileSize < 0x10016L) ? fileSize : 0x10016L);
    std::vector<unsigned char> tail(tailSize);
    if (fseek(pak, fileSize - (long)tailSize, SEEK_SET) != 0 ||
        !pakReadExact(pak, tail.data(), tail.size()))
        return false;

    size_t eocd = tail.size();
    while (eocd >= 22) {
        --eocd;
        if (eocd + 4 <= tail.size() &&
            pakRd32(tail.data() + eocd) == 0x06054b50u)
            break;
    }
    if (eocd + 22 > tail.size())
        return false;

    const uint16_t entries = pakRd16(tail.data() + eocd + 10);
    const uint32_t cdSize = pakRd32(tail.data() + eocd + 12);
    const uint32_t cdOffset = pakRd32(tail.data() + eocd + 16);

    if (!entries || !cdSize || cdSize > 128 * 1024 * 1024u)
        return false;

    std::vector<unsigned char> cd(cdSize);
    if (fseek(pak, (long)cdOffset, SEEK_SET) != 0 ||
        !pakReadExact(pak, cd.data(), cd.size()))
        return false;

    size_t pos = 0;
    for (uint16_t i = 0; i < entries && pos + 46 <= cd.size(); ++i) {
        const unsigned char* h = cd.data() + pos;
        if (pakRd32(h) != 0x02014b50u)
            break;

        const uint16_t nameLen = pakRd16(h + 28);
        const uint16_t extraLen = pakRd16(h + 30);
        const uint16_t commentLen = pakRd16(h + 32);
        const size_t recordSize = 46u + nameLen + extraLen + commentLen;
        if (pos + recordSize > cd.size())
            break;

        std::string name((const char*)h + 46, nameLen);
        if (pakNormalizeName(name.c_str()) == pakNormalizeName(wanted.c_str())) {
            method = pakRd16(h + 10);
            compressedSize = pakRd32(h + 20);
            uncompressedSize = pakRd32(h + 24);
            expectedCrc = pakRd32(h + 16);
            localOffset = pakRd32(h + 42);
            return true;
        }
        pos += recordSize;
    }
    return false;
}

static bool pakExtractEntry(const std::string& pakPath, const std::string& wanted,
                            const std::string& outPath) {
    const std::string normalizedWanted = pakNormalizeName(wanted.c_str());
    FILE* pak = fopen(pakPath.c_str(), "rb");
    if (!pak) {
        return false;
    }

    uint32_t localOffset = 0, compressedSize = 0, uncompressedSize = 0;
    uint32_t expectedCrc = 0;
    uint16_t method = 0;
    if (!pakFindEntry(pak, wanted, localOffset, compressedSize,
                      uncompressedSize, method, expectedCrc)) {
        fclose(pak);
        return false;
    }

    if (!compressedSize || !uncompressedSize ||
        compressedSize > 128 * 1024 * 1024u ||
        uncompressedSize > 128 * 1024 * 1024u) {
        fclose(pak);
        return false;
    }

    unsigned char local[30];
    if (fseek(pak, (long)localOffset, SEEK_SET) != 0 ||
        !pakReadExact(pak, local, sizeof(local)) ||
        pakRd32(local) != 0x04034b50u) {
        fclose(pak);
        return false;
    }

    const uint16_t localMethod = pakRd16(local + 8);
    const uint16_t nameLen = pakRd16(local + 26);
    const uint16_t extraLen = pakRd16(local + 28);
    const long dataOffset = (long)localOffset + 30L + nameLen + extraLen;

    if (method != localMethod) {
        fclose(pak);
        return false;
    }

    if (fseek(pak, 0, SEEK_END) != 0) {
        fclose(pak);
        return false;
    }
    const long fileSize = ftell(pak);
    if (fileSize < 0 ||
        dataOffset < 0 ||
        (uint64_t)dataOffset + (uint64_t)compressedSize > (uint64_t)fileSize) {
        fclose(pak);
        return false;
    }

    if (fseek(pak, dataOffset, SEEK_SET) != 0) {
        fclose(pak);
        return false;
    }

    std::vector<unsigned char> compressed(compressedSize);
    std::vector<unsigned char> plain(uncompressedSize);
    const bool readOk = pakReadExact(pak, compressed.data(), compressed.size());
    fclose(pak);
    if (!readOk) {
        return false;
    }

    bool ok = false;
    if (method == 0 && compressedSize == uncompressedSize) {
        memcpy(plain.data(), compressed.data(), uncompressedSize);
        ok = true;
    } else if (method == 8) {
        ok = pakInflateRaw(compressed.data(), compressed.size(),
                           plain.data(), plain.size());
    } else {
        return false;
    }

    if (!ok) {
        return false;
    }

    const bool classRegistryEntry =
        normalizedWanted == "scripts/classregistry.lua";

    if (classRegistryEntry) {
        const uint32_t actualCrc =
            (uint32_t)crc32(0, plain.data(), (unsigned int)plain.size());


        if (actualCrc != expectedCrc) {
            return false;
        }
    }

    size_t slash = outPath.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string dir = outPath.substr(0, slash);
        std::string cur;
        size_t pos = 0;
        while (pos <= dir.size()) {
            size_t end = dir.find('/', pos);
            std::string part = dir.substr(pos, end == std::string::npos ?
                                           dir.size() - pos : end - pos);
            if (!part.empty()) {
                if (!cur.empty()) cur += "/";
                cur += part;
                mkdir(cur.c_str(), 0755);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }

    FILE* out = fopen(outPath.c_str(), "wb");
    if (!out) return false;
    const bool wrote = fwrite(plain.data(), 1, plain.size(), out) == plain.size();
    fclose(out);
    return wrote;
}


// Materialize an exact CryPak virtual path from its PAK entry. Unlike the
// basename fallback below, this preserves the directory portion of the entry and
// can handle Windows-style '\\' paths used by the Android CryEngine build.
static bool tryMaterializePakPath(const char* targetPath, std::string& materialized) {
    materialized.clear();
    if (!targetPath || !*targetPath)
        return false;

    std::string wanted = pakNormalizeName(targetPath);
    if (wanted.empty())
        return false;

    const std::string gameMarker = "/game/";
    const size_t gamePos = wanted.find(gameMarker);
    if (gamePos != std::string::npos)
        wanted.erase(0, gamePos + gameMarker.size());

    while (wanted.rfind("./", 0) == 0)
        wanted.erase(0, 2);
    if (wanted.rfind("fcdata/", 0) == 0)
        wanted.erase(0, 7);

    const bool inCgfCache = wanted.rfind("ccgf_cache/", 0) == 0;
    if (inCgfCache)
        wanted.erase(0, 11);

    if (wanted.empty() || wanted[0] == '/')
        return false;

    std::vector<std::string> pakCandidates;
    if (inCgfCache) {
        pakCandidates.push_back("FCData/CCGF_CACHE.PAK");
        pakCandidates.push_back("FCData/ccgf_cache.pak");
        pakCandidates.push_back("fcdata/CCGF_CACHE.PAK");
        pakCandidates.push_back("fcdata/ccgf_cache.pak");
    } else {
        const char* roots[] = {
            "FCData", "fcdata", ".", nullptr
        };
        for (size_t r = 0; roots[r]; ++r) {
            DIR* dir = opendir(roots[r]);
            if (!dir) continue;
            while (dirent* ent = readdir(dir)) {
                const char* n = ent->d_name;
                const size_t len = strlen(n);
                if (len < 4 || std::tolower((unsigned char)n[len-4]) != '.' ||
                    std::tolower((unsigned char)n[len-3]) != 'p' ||
                    std::tolower((unsigned char)n[len-2]) != 'a' ||
                    std::tolower((unsigned char)n[len-1]) != 'k')
                    continue;
                std::string pp = std::string(roots[r]);
                if (pp != ".") pp += "/";
                pp += n;
                pakCandidates.push_back(pp);
            }
            closedir(dir);
        }
    }

    // realpath() must return an absolute canonical path. Keep the PAK
    // materialization cache under the current game directory, but never hand
    // the guest CryEngine a relative "_pakcache_v3/..." path.
    char cwd[PATH_MAX];
    if (!::getcwd(cwd, sizeof(cwd)))
        return false;

    std::string cacheRoot = cwd;
    if (!cacheRoot.empty() && cacheRoot.back() != '/')
        cacheRoot += '/';
    cacheRoot += "_pakcache_v3";

    std::string safe = wanted;
    for (char& c : safe) if (c == '/') c = '_';
    const std::string outPath = cacheRoot + "/" + safe;

    struct stat cached = {};
    if (::stat(outPath.c_str(), &cached) == 0 && S_ISREG(cached.st_mode)) {
        materialized = outPath;
        return true;
    }

    for (const std::string& pakPath : pakCandidates) {
        struct stat st = {};
        if (::stat(pakPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (pakExtractEntry(pakPath, wanted, outPath)) {
            materialized = outPath;
            compatLogFmt("PAK EXACT MATCH: %s <- %s", wanted.c_str(), pakPath.c_str());
            return true;
        }
    }

    compatLogFmt("PAK EXACT MISS: %s%s", inCgfCache ? "CCGF_CACHE/" : "", wanted.c_str());
    return false;
}

static bool pakFindUniqueBasename(const std::string& pakPath,
                                  const std::string& wantedBasename,
                                  std::string& matchedEntry) {
    matchedEntry.clear();

    FILE* pak = fopen(pakPath.c_str(), "rb");
    if (!pak)
        return false;

    if (fseek(pak, 0, SEEK_END) != 0) {
        fclose(pak);
        return false;
    }

    const long fileSize = ftell(pak);
    if (fileSize < 22) {
        fclose(pak);
        return false;
    }

    const size_t tailSize =
        (size_t)((fileSize < 0x10016L) ? fileSize : 0x10016L);
    std::vector<unsigned char> tail(tailSize);
    if (fseek(pak, fileSize - (long)tailSize, SEEK_SET) != 0 ||
        !pakReadExact(pak, tail.data(), tail.size())) {
        fclose(pak);
        return false;
    }

    size_t eocd = tail.size();
    while (eocd >= 22) {
        --eocd;
        if (eocd + 4 <= tail.size() &&
            pakRd32(tail.data() + eocd) == 0x06054b50u)
            break;
    }
    if (eocd + 22 > tail.size()) {
        fclose(pak);
        return false;
    }

    const uint16_t entries = pakRd16(tail.data() + eocd + 10);
    const uint32_t cdSize = pakRd32(tail.data() + eocd + 12);
    const uint32_t cdOffset = pakRd32(tail.data() + eocd + 16);
    if (!entries || !cdSize || cdSize > 128 * 1024 * 1024u) {
        fclose(pak);
        return false;
    }

    std::vector<unsigned char> cd(cdSize);
    if (fseek(pak, (long)cdOffset, SEEK_SET) != 0 ||
        !pakReadExact(pak, cd.data(), cd.size())) {
        fclose(pak);
        return false;
    }

    const std::string wanted =
        pakNormalizeName(wantedBasename.c_str());

    std::string firstMatch;
    int matches = 0;
    size_t pos = 0;

    for (uint16_t i = 0; i < entries && pos + 46 <= cd.size(); ++i) {
        const unsigned char* h = cd.data() + pos;
        if (pakRd32(h) != 0x02014b50u)
            break;

        const uint16_t nameLen = pakRd16(h + 28);
        const uint16_t extraLen = pakRd16(h + 30);
        const uint16_t commentLen = pakRd16(h + 32);
        const size_t recordSize =
            46u + nameLen + extraLen + commentLen;

        if (pos + recordSize > cd.size())
            break;

        const std::string entry(
            (const char*)h + 46, nameLen);
        const std::string normalized =
            pakNormalizeName(entry.c_str());

        size_t slash = normalized.find_last_of('/');
        const std::string base =
            (slash == std::string::npos)
                ? normalized
                : normalized.substr(slash + 1);

        if (base == wanted) {
            ++matches;
            if (matches == 1) {
                firstMatch = entry;
                compatLogFmt(
                    "PAK BASENAME MATCH: %s <- %s :: %s",
                    wantedBasename.c_str(),
                    pakPath.c_str(),
                    normalized.c_str());
            }
        }

        pos += recordSize;
    }

    fclose(pak);

    if (matches == 1) {
        matchedEntry = firstMatch;
        return true;
    }

    if (matches > 1) {
        compatLogFmt(
            "PAK BASENAME AMBIGUOUS: %s <- %s (%d matches)",
            wantedBasename.c_str(), pakPath.c_str(), matches);
    }

    return false;
}

static bool tryMaterializeUniquePakBasename(const char* targetPath) {
    if (!targetPath || !*targetPath)
        return false;

    const std::string normalizedTarget =
        pakNormalizeName(targetPath);

    const size_t slash =
        normalizedTarget.find_last_of('/');
    const std::string wantedBasename =
        (slash == std::string::npos)
            ? normalizedTarget
            : normalizedTarget.substr(slash + 1);

    if (wantedBasename.empty())
        return false;

    std::string resolvedRoot;
    if (!resolvePathCaseInsensitive("FCData", resolvedRoot)) {
        compatLogFmt(
            "PAK BASENAME NOT FOUND: %s (FCData missing)",
            targetPath);
        return false;
    }

    DIR* root = opendir(resolvedRoot.c_str());
    if (!root) {
        compatLogFmt(
            "PAK BASENAME NOT FOUND: %s (cannot open FCData)",
            targetPath);
        return false;
    }

    std::string uniquePak;
    std::string uniqueEntry;
    int globalMatches = 0;

    for (struct dirent* ent = readdir(root); ent; ent = readdir(root)) {
        const char* name = ent->d_name;
        const size_t len = std::strlen(name);
        if (len < 4 ||
            std::tolower((unsigned char)name[len - 4]) != '.' ||
            std::tolower((unsigned char)name[len - 3]) != 'p' ||
            std::tolower((unsigned char)name[len - 2]) != 'a' ||
            std::tolower((unsigned char)name[len - 1]) != 'k')
            continue;

        const std::string pakPath =
            resolvedRoot + "/" + name;
        std::string matchedEntry;

        if (!pakFindUniqueBasename(
                pakPath, wantedBasename, matchedEntry))
            continue;

        ++globalMatches;
        if (globalMatches == 1) {
            uniquePak = pakPath;
            uniqueEntry = matchedEntry;
        }
    }

    closedir(root);

    if (globalMatches == 0) {
        compatLogFmt(
            "PAK BASENAME NOT FOUND: %s",
            targetPath);
        return false;
    }

    if (globalMatches > 1) {
        compatLogFmt(
            "PAK BASENAME AMBIGUOUS: %s (%d PAKs)",
            targetPath, globalMatches);
        return false;
    }

    if (!pakExtractEntry(
            uniquePak, uniqueEntry, targetPath)) {
        return false;
    }

    return true;
}



// ─── Shader source discovery ────────────────────────────────────────────────
// Only check the expected loose paths. The old recursive scan walked the entire
// game tree (~12k entries) on every launch just to report that these files were
// not loose. CryPak can load shader content from PAKs, so a full-tree crawl is
// unnecessary for runtime and is kept out of startup.
static void prepareShaderSourceFiles(const char* /*dataRoot*/) {
    const char* wanted[] = {
        "Shaders/statenocull.ext",
        "Shaders/hdrprocess.ext",
        "Shaders/sunflares.ext",
        "Shaders/lightstyles.ext",
        "Shaders/glare.ext",
        "Shaders/cgvprogramms.ext",
        "Shaders/cgpshaders.ext",
        "Shaders/templfog.ext",
        "Shaders/templvfog.ext",
        "Shaders/templfog_fp.ext",
        "Shaders/templfogcaustics.ext",
        "Shaders/templvfogcaustics.ext",
        "Shaders/templfogcaustics_fp.ext",
        "Shaders/white.ext",
        "Shaders/whiteshadow.ext",
        "Shaders/templdecal.ext",
        "Shaders/templheatvis_sources.ext",
        "Shaders/templinvlight.ext",
        "Shaders/templdof.ext",
        "Shaders/Scripts/CommonSubroutines.csl",
        "Shaders/Scripts/CommonSubroutines.csi",
        "Shaders/HWScripts/CommonSubroutines.csl",
        "Shaders/HWScripts/CommonSubroutines.csi",
        nullptr
    };

    int foundCount = 0;
    int materializedCount = 0;

    for (size_t i = 0; wanted[i]; ++i) {
        std::string resolved;
        if (!resolvePathCaseInsensitive(wanted[i], resolved)) {
            // The renderer expects these shader control files as normal files
            // during Init Shaders. Do not rely on CryPak directory enumeration
            // to discover them from FCData/*.pak: materialize a unique PAK entry
            // into the exact requested path first.
            if (tryMaterializeUniquePakBasename(wanted[i])) {
                ++materializedCount;
            }
            if (!resolvePathCaseInsensitive(wanted[i], resolved))
                continue;
        }

        struct stat st = {};
        if (stat(resolved.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            ++foundCount;
    }

    compatLogFmt("shader source files: %d/24 present, materialized=%d",
                 foundCount, materializedCount);
}

static std::string pakAssetRelativeName(const char* requested) {
    if (!requested || !*requested)
        return std::string();

    std::string wanted = pakNormalizeName(requested);

    // Prefer the explicit game root marker. This remains reliable even when
    // CryPak changes the process CWD or passes an already-absolute Switch path.
    const std::string gameMarker = "/game/";
    const size_t gamePos = wanted.find(gameMarker);
    if (gamePos != std::string::npos)
        wanted.erase(0, gamePos + gameMarker.size());

    // Fall back to the actual CWD when the request did not contain /game/.
    if (wanted.size() && wanted[0] == '/') {
        char cwd[PATH_MAX];
        if (::getcwd(cwd, sizeof(cwd))) {
            std::string cwdNorm = pakNormalizeName(cwd);
            while (cwdNorm.size() > 1 && cwdNorm.back() == '/')
                cwdNorm.pop_back();

            const std::string prefix = cwdNorm + "/";
            if (wanted.rfind(prefix, 0) == 0)
                wanted.erase(0, prefix.size());
        }
    }

    // Explicit FCData paths are virtual mount paths, not PAK entry prefixes.
    if (wanted.rfind("fcdata/", 0) == 0)
        wanted.erase(0, 7);

    // CryPak exposes the compiled geometry cache through the virtual
    // CCGF_CACHE namespace, but the actual archive entries are rooted at
    // objects/... (CCGF_CACHE.PAK is the archive, not an entry-directory).
    // Keep the namespace for the physical file lookup, but remove it when
    // comparing against PAK entry names.
    if (wanted.rfind("ccgf_cache/", 0) == 0)
        wanted.erase(0, 11);

    while (wanted.rfind("./", 0) == 0)
        wanted.erase(0, 2);

    return wanted;
}

static FILE* tryOpenFromPaks(const char* requested, const char* mode) {
    if (!requested || !mode || mode[0] != 'r')
        return nullptr;

    const std::string wanted = pakAssetRelativeName(requested);
    if (wanted.empty())
        return nullptr;

    // v3 intentionally bypasses caches produced by earlier PAK path/lookup implementations.
    // All PAK assets still use the same resolver; this only prevents stale extracted
    // files from hiding whether the current archive reader produced valid data.
    const std::string cacheRoot = "_pakcache_v3";
    std::string safeName = wanted;
    for (char& c : safeName)
        if (c == '/') c = '_';
    const std::string outPath = cacheRoot + "/" + safeName;


    struct stat cached = {};
    if (::stat(outPath.c_str(), &cached) == 0 && S_ISREG(cached.st_mode)) {
        FILE* f = fopen(outPath.c_str(), mode);
        if (f) {
            return f;
        }
    }

    // All normal assets use the same PAK search path. There is no special case
    // for scripts, textures, audio, etc.: normalize the guest path once and
    // search the same PAK roots for the same relative entry name.
    const char* roots[] = {
        ".",
        "FCData",
        "fcdata",
        "FCData/Localized",
        "fcdata/Localized",
        "FCData/localized",
        "fcdata/localized",
        nullptr
    };

    for (size_t r = 0; roots[r]; ++r) {
        DIR* dir = opendir(roots[r]);
        if (!dir)
            continue;

        while (dirent* ent = readdir(dir)) {
            const char* name = ent->d_name;
            const size_t len = std::strlen(name);
            if (len < 4)
                continue;

            const char c0 = (char)std::tolower((unsigned char)name[len - 4]);
            const char c1 = (char)std::tolower((unsigned char)name[len - 3]);
            const char c2 = (char)std::tolower((unsigned char)name[len - 2]);
            const char c3 = (char)std::tolower((unsigned char)name[len - 1]);
            if (c0 != '.' || c1 != 'p' || c2 != 'a' || c3 != 'k')
                continue;

            std::string pakPath = std::string(roots[r]);
            if (pakPath != ".")
                pakPath += "/";
            pakPath += name;

            struct stat st = {};
            if (::stat(pakPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            if (pakExtractEntry(pakPath, wanted, outPath)) {
                if (wanted.rfind("objects/", 0) == 0)
                    compatLogFmt("PAK EXACT: %s <- %s", wanted.c_str(), pakPath.c_str());
                FILE* f = fopen(outPath.c_str(), mode);
                if (f) {
                    closedir(dir);
                    return f;
                }
            }
        }

        closedir(dir);
    }

    return nullptr;
}


// ─── Startup shader overlay ───────────────────────────────────────────────────
// Show shader file lookups directly on the active Switch GL surface during
// engine startup. This is limited to the first 64 shader-related fopen calls.
static unsigned g_shader_overlay_events = 0;

struct StartupGlyph { char c; uint8_t rows[7]; };

static const StartupGlyph g_startup_font[] = {
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0F,0x10,0x10,0x17,0x11,0x11,0x0F}},
    {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'J',{0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
    {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
    {' ',{0,0,0,0,0,0,0}},
    {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'.',{0,0,0,0,0,0,0x04}},
    {'_',{0,0,0,0,0,0,0x1F}},
    {'-',{0,0,0,0x1F,0,0,0}},
    {':',{0,0x04,0,0,0,0x04,0}},
    {0,{0,0,0,0,0,0,0}}
};

static const StartupGlyph* startupGlyph(char c) {
    const char up = (char)std::toupper((unsigned char)c);
    for (const StartupGlyph* g = g_startup_font; g->c; ++g)
        if (g->c == up) return g;
    return &g_startup_font[36];
}

static void startupDrawText(float x, float y, float scale, const char* text) {
    if (!text) return;
    glBegin(GL_QUADS);
    float pen = x;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p == '\n') { y += 9.0f * scale; pen = x; continue; }
        const StartupGlyph* glyph = startupGlyph((char)*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (!(glyph->rows[row] & (1u << (4 - col)))) continue;
                const float x0 = pen + col * scale;
                const float y0 = y + row * scale;
                const float x1 = x0 + scale;
                const float y1 = y0 + scale;
                glVertex2f(x0, y0); glVertex2f(x1, y0);
                glVertex2f(x1, y1); glVertex2f(x0, y1);
            }
        }
        pen += 6.0f * scale;
    }
    glEnd();
}


// Compact shader-loader diagnostics. We inspect only shader script files and only
// report the tokens relevant to CommonSubroutines. This avoids the old full-tree
// scan while showing whether the source that should register the script was read.
static bool shaderPathHasExt(const char* path, const char* ext) {
    if (!path || !ext)
        return false;

    const size_t pathLen = std::strlen(path);
    const size_t extLen = std::strlen(ext);
    if (pathLen < extLen)
        return false;

    const char* p = path + pathLen - extLen;
    for (size_t i = 0; i < extLen; ++i) {
        if (std::tolower((unsigned char)p[i]) !=
            std::tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static void logShaderScriptDiagnostics(FILE* f, const char* path) {
    if (!f || !path)
        return;

    const bool isCsl = shaderPathHasExt(path, ".csl");
    const bool isCsi = shaderPathHasExt(path, ".csi");
    const bool isCrycg = shaderPathHasExt(path, ".crycg");
    if (!isCsl && !isCsi && !isCrycg)
        return;

    const long saved = ftell(f);
    if (saved < 0 || fseek(f, 0, SEEK_END) != 0) {
        return;
    }

    const long fileSize = ftell(f);
    if (fileSize < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fseek(f, saved, SEEK_SET);
        return;
    }

    // Shader control files are small. Cap the diagnostic read so a malformed
    // asset can never turn logging into a large allocation/read operation.
    const size_t maxRead = 256 * 1024;
    const size_t toRead = static_cast<size_t>(
        fileSize > static_cast<long>(maxRead) ? maxRead : fileSize);
    std::vector<char> buf(toRead + 1, '\0');
    const size_t got = toRead ? fread(buf.data(), 1, toRead, f) : 0;
    buf[got] = '\0';
    fseek(f, saved, SEEK_SET);

    if (!got)
        return;

    const std::string lower = asciiLower(std::string(buf.data(), got));
    const bool hasCommon = lower.find("commonsubroutines") != std::string::npos;
    const bool hasSubrScript = lower.find("subrscript") != std::string::npos;
    const bool hasDeclareCommon =
        lower.find("declarecgscript") != std::string::npos && hasCommon;
    const bool hasShaderCgvProgramms =
        lower.find("shader 'cgvprogramms'") != std::string::npos;

    if (isCsi && hasCommon) {
        compatLogFmt("shader csi: %s size=%ld SubrScript(CommonSubroutines)=%d",
                     path, fileSize, hasSubrScript ? 1 : 0);
        return;
    }

    if (isCsl && (hasDeclareCommon || hasShaderCgvProgramms)) {
        compatLogFmt("shader csl: %s size=%ld CGVProgramms=%d DeclareCGScript(CommonSubroutines)=%d",
                     path, fileSize, hasShaderCgvProgramms ? 1 : 0,
                     hasDeclareCommon ? 1 : 0);
        return;
    }

    if (isCrycg && hasCommon) {
        compatLogFmt("shader crycg: %s size=%ld CommonSubroutines=1",
                     path, fileSize);
    }
}

// Forward declaration: shader directory diagnostics are defined with the
// directory-enumeration helpers below, after stub_fopen().
static bool isShaderPathForDiag(const char* path);

// Shader cache files are generated/runtime cache artifacts, not language assets.
// When a cache file is missing, sending the request through tryOpenFromPaks()
// makes every lookup scan every FCData PAK and emit an entry-not-found message for
// each archive. With shader compilation disabled there is nothing to extract from
// a PAK for these paths, so let the normal filesystem miss fall through directly
// to the renderer's embedded shader fallback.
static bool isShaderCacheLookupPath(const char* path) {
    if (!path || !*path)
        return false;

    std::string normalized(path);
    for (char& c : normalized) {
        if ((unsigned char)c == 92)
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    const size_t cachePos = normalized.find("shaders/cache/");
    if (cachePos == std::string::npos)
        return false;

    const size_t ext = normalized.rfind('.');
    if (ext == std::string::npos)
        return false;

    return normalized.compare(ext, std::string::npos, ".cgasm") == 0 ||
           normalized.compare(ext, std::string::npos, ".cgps") == 0 ||
           normalized.compare(ext, std::string::npos, ".cgvp") == 0;
}

// fopen wrapper — logs failed opens so we can see what paths game code requests
static FILE* stub_fopen(const char* path, const char* mode) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    const bool shaderIo = isShaderPathForDiag(ioPath);
    const bool shaderSourceIo =
        shaderIo &&
        (shaderPathHasExt(ioPath, ".csl") ||
         shaderPathHasExt(ioPath, ".csi") ||
         shaderPathHasExt(ioPath, ".crycg"));
    static unsigned g_shader_cache_miss_logs = 0;
    const bool shaderCacheIo = shaderIo && isShaderCacheLookupPath(ioPath);
    if (shaderSourceIo)
        compatLogFmt("fopen SHADER REQUEST: path=%s mode=%s",
                     ioPath ? ioPath : "?", mode ? mode : "?");

    if (path && ioPathStorage != path)
        compatLogFmt("path NORMALIZE: fopen %s -> %s", path, ioPathStorage.c_str());

    if (std::string mapped = obbRemap(ioPath); !mapped.empty()) {
        FILE* mf = fopen(mapped.c_str(), mode);
        compatLogFmt("obb: fopen %s -> %s (%s)", ioPath ? ioPath : "?",
                     mapped.c_str(), mf ? "ok" : "still not there");
        if (mf) { setvbuf(mf, nullptr, _IOFBF, 64 * 1024); return mf; }
    }

    if (ioPath && compatIsPakPath(ioPath))
        compatLogFmt("PAK FOPEN REQUEST: %s", ioPath);

    FILE* f = fopen(ioPath, mode);
    if (shaderSourceIo)
        compatLogFmt("fopen SHADER DIRECT: path=%s result=%s",
                     ioPath ? ioPath : "?", f ? "OK" : "FAIL");
    if (!f && ioPath) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath) {
            FILE* rf = fopen(resolved.c_str(), mode);
            if (rf) {
                compatLogFmt("fopen CASEFIX: %s -> %s", ioPath, resolved.c_str());
                if (shaderSourceIo)
                    compatLogFmt("fopen SHADER CASEFIX: requested=%s resolved=%s result=OK",
                                 ioPath, resolved.c_str());
                f = rf;
            }
        }
    }

    if (!f && ioPath && ioPath[0] != '/' && ioPath[0] != '\\' &&
        strncasecmp(ioPath, "FCData/", 7) != 0 &&
        strncasecmp(ioPath, "fcdata/", 7) != 0) {
        const std::string virtualPath = std::string("FCData/") + ioPath;
        std::string resolved;
        if (resolvePathCaseInsensitive(virtualPath.c_str(), resolved)) {
            FILE* rf = fopen(resolved.c_str(), mode);
            if (rf) {
                compatLogFmt("fopen FCDATA: %s -> %s", ioPath, resolved.c_str());
                f = rf;
            }
        }
    }

    if (!f) {
        // Shader cache entries are never language-pack assets. In particular,
        // do not scan FCData PAKs for them: a cache miss should reach the
        // renderer's embedded fallback immediately.
        if (!isShaderCacheLookupPath(ioPath)) {
            FILE* pakFile = tryOpenFromPaks(ioPath, mode);
            if (pakFile)
                return pakFile;
        }

        // Missing PAKs are not fatal packaging artifacts on Switch. Return
        // a valid zero-entry ZIP so CryPak can register the archive and continue.
        if (mode && mode[0] == 'r' && ioPath && isPakArchivePath(ioPath)) {
            FILE* fallback = makeEmptyPak(ioPath, mode);
            if (fallback)
                return fallback;
        }

        // Android Far Cry's startup code can probe packaging artifacts that
        // are not present in the shipped Switch FCData set. These are optional
        // empty ZIP/PAK placeholders in the Android packaging path.
        if (mode && mode[0] == 'r' && ioPath) {
            const std::string normalizedPath = pakNormalizeName(ioPath);

            const bool startupPak0 =
                normalizedPath == "cdata/0.pak" ||
                normalizedPath == "fcdata/0.pak" ||
                (normalizedPath.size() >= 12 &&
                 normalizedPath.compare(normalizedPath.size() - 12, 12,
                                         "/cdata/0.pak") == 0) ||
                (normalizedPath.size() >= 13 &&
                 normalizedPath.compare(normalizedPath.size() - 13, 13,
                                         "/fcdata/0.pak") == 0);

            const bool startupPak517 =
                normalizedPath == "cdata/517.pak" ||
                normalizedPath == "fcdata/517.pak" ||
                (normalizedPath.size() >= 14 &&
                 normalizedPath.compare(normalizedPath.size() - 14, 14,
                                         "/cdata/517.pak") == 0) ||
                (normalizedPath.size() >= 15 &&
                 normalizedPath.compare(normalizedPath.size() - 15, 15,
                                         "/fcdata/517.pak") == 0);

            if (startupPak0 || startupPak517) {
                if (normalizedPath == "cdata/0.pak" ||
                    normalizedPath == "cdata/517.pak") {
                    mkdir("CData", 0755);
                }

                FILE* fallback = makeEmptyPak(ioPath, mode);
                if (fallback) {
                    compatLogFmt("fopen FALLBACK: %s -> empty ZIP for Android startup compatibility",
                                 ioPath);
                    return fallback;
                }
            }
        }

        if (shaderSourceIo)
            compatLogFmt("fopen SHADER FINAL FAIL: path=%s mode=%s",
                         ioPath ? ioPath : "?", mode ? mode : "?");

        if (!shaderCacheIo || g_shader_cache_miss_logs < 64) {
            compatLogFmt("fopen FAIL: %s (mode=%s)",
                         ioPath ? ioPath : "?", mode ? mode : "?");
            if (shaderCacheIo) {
                ++g_shader_cache_miss_logs;
                if (g_shader_cache_miss_logs == 64)
                    compatLog("further shader cache misses are no longer logged");
            }
        }
        return f;
    }

    if (compatIsPakPath(ioPath)) {
        compatLogPakOpenState(f, ioPath);
    }

    logShaderScriptDiagnostics(f, ioPath);

    if (apkcache::adopt(f, ioPath)) {
        setvbuf(f, nullptr, _IOFBF, 16 * 1024);
        compatLogFmt("apkcache: caching reads from %s", ioPath ? ioPath : "?");
        return f;
    }
    setvbuf(f, nullptr, _IOFBF, 64 * 1024);
    return f;
}
// ─── Cached APK stream ───────────────────────────────────────────────────────
// cocos2d-x reads the game's assets straight out of the .apk it was handed, and
// minizip's access pattern — thousands of tiny reads plus a fresh walk of the
// zip's central directory for every asset — is the worst case for an SD card,
// where each read is an IPC round trip rather than a page-cache hit. These
// route a cached stream through compat/apkcache.h and leave every other file
// exactly as it was.
static size_t sh_fread(void* p, size_t sz, size_t n, FILE* f) {
    size_t rc;
    if (apkcache::owns(f))
        rc = apkcache::read(f, p, sz, n);
    else
        rc = fread(p, sz, n, f);
    return rc;
}
static int sh_fseek(FILE* f, long off, int whence) {
    int rc;
    if (apkcache::owns(f))
        rc = apkcache::seek(f, (int64_t)off, whence);
    else
        rc = fseek(f, off, whence);
    return rc;
}
static long sh_ftell(FILE* f) {
    if (apkcache::owns(f)) return (long)apkcache::tell(f);
    return ftell(f);
}
static int sh_fgetc(FILE* f) {
    int rc;
    if (apkcache::owns(f))
        rc = apkcache::getc(f);
    else
        rc = fgetc(f);
    return rc;
}
static int sh_feof(FILE* f) {
    int rc;
    if (apkcache::owns(f))
        rc = apkcache::eof(f);
    else
        rc = feof(f);
    return rc;
}
static void sh_rewind(FILE* f) {
    if (apkcache::owns(f)) { apkcache::seek(f, 0, SEEK_SET); return; }
    rewind(f);
}
static int sh_fclose(FILE* f) {
    apkcache::close(f);   // no-op unless this stream was cached
    return fclose(f);
}

// open() wrapper — logs every call so we can trace early constructor I/O
static int stub_open(const char* path, int flags, ...) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    if (path && ioPathStorage != path)
        compatLogFmt("path NORMALIZE: open %s -> %s", path, ioPathStorage.c_str());

    int vfd = devUrandomOpen(ioPath);
    if (vfd >= 0) return vfd;

    va_list va;
    va_start(va, flags);
    int mode = 0;
    if (flags & O_CREAT)
        mode = va_arg(va, int);
    va_end(va);

    auto doOpen = [&](const char* p) -> int {
        return (flags & O_CREAT) ? open(p, flags, mode) : open(p, flags);
    };

    if (std::string mapped = obbRemap(ioPath); !mapped.empty()) {
        int mfd = doOpen(mapped.c_str());
        compatLogFmt("obb: open %s -> %s (fd=%d)", ioPath ? ioPath : "?",
                     mapped.c_str(), mfd);
        if (mfd >= 0) return mfd;
    }

    const bool shaderSourceOpen =
        ioPath &&
        (shaderPathHasExt(ioPath, ".csl") ||
         shaderPathHasExt(ioPath, ".csi") ||
         shaderPathHasExt(ioPath, ".crycg"));
    if (shaderSourceOpen)
        compatLogFmt("open SHADER REQUEST: path=%s flags=0x%x",
                     ioPath, flags);

    int fd = doOpen(ioPath);
    if (shaderSourceOpen)
        compatLogFmt("open SHADER DIRECT: path=%s result=%s fd=%d",
                     ioPath, fd >= 0 ? "OK" : "FAIL", fd);
    if (fd < 0 && ioPath) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath) {
            int rfd = doOpen(resolved.c_str());
            if (rfd >= 0) {
                compatLogFmt("open CASEFIX: %s -> %s fd=%d", ioPath, resolved.c_str(), rfd);
                if (shaderSourceOpen)
                    compatLogFmt("open SHADER CASEFIX: requested=%s resolved=%s result=OK fd=%d",
                                 ioPath, resolved.c_str(), rfd);
                return rfd;
            }
            if (shaderSourceOpen)
                compatLogFmt("open SHADER CASEFIX: requested=%s resolved=%s result=FAIL",
                             ioPath, resolved.c_str());
        }
    }

    if (fd < 0) compatLogFmt("open FAIL: %s flags=0x%x", ioPath ? ioPath : "?", flags);
    else        compatLogFmt("open OK:   %s flags=0x%x fd=%d", ioPath ? ioPath : "?", flags, fd);
    return fd;
}

// ─── __android_log_print (liblog) ────────────────────────────────────────────
// This turned out to be THE dominant stutter source during actual gameplay,
// not anything overlay/rendering related: games call this constantly (touch
// state, pedal values, per-frame telemetry), each call's message differs
// slightly (an embedded changing number), so compatLog's exact-match dedup
// never collapses them — every single call was a distinct message hitting a
// REAL SD-card fflush via the normal compatLog path. That's a steady stream
// of disk writes throughout gameplay. Time-throttle instead of dropping
// entirely: still useful for diagnostics, just capped to 2/sec regardless of
// how often the game actually calls this.
static bool androidLogThrottleOk() {
    static uint64_t s_lastTick = 0;
    uint64_t now = armGetSystemTick();
    uint64_t elapsedMs = (now - s_lastTick) * 1000 / armGetSystemTickFreq();
    if (elapsedMs < 500) return false;
    s_lastTick = now;
    return true;
}
static int android_log_print(int, const char* tag, const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (androidLogThrottleOk()) compatLogFmt("[%s] %s", tag ? tag : "?", buf);
    return (int)strlen(buf);
}
static int android_log_write(int, const char* tag, const char* msg) {
    if (androidLogThrottleOk()) compatLogFmt("[%s] %s", tag ? tag : "?", msg ? msg : "");
    return 0;
}
static int android_log_vprint(int, const char* tag, const char* fmt, va_list va) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, va);
    if (androidLogThrottleOk()) compatLogFmt("[%s] %s", tag ? tag : "?", buf);
    return (int)strlen(buf);
}
static int android_log_buf_print(int, int, const char* tag, const char* fmt, ...) {
    va_list va; va_start(va, fmt);
    int r = android_log_vprint(0, tag, fmt, va);
    va_end(va); return r;
}

// ─── pthreads — REAL implementation over libnx/newlib primitives ─────────────
// The game's asset-loader thread is a persistent worker loop; running it
// synchronously (the old approach) froze the game on the HCR loading screen.
// Now pthread_create makes a real libnx thread, and the sync primitives are
// backed by newlib's recursive locks / condvars *embedded inside the game's
// own (larger) Bionic structs*:
//   Bionic pthread_mutex_t = 40 B  ⊇ _LOCK_RECURSIVE_T (8 B, zero-init valid)
//   Bionic pthread_cond_t  = 48 B  ⊇ _COND_T           (4 B, zero-init valid)
//   Bionic pthread_rwlock_t= 56 B  ⊇ _LOCK_RECURSIVE_T (writer-lock semantics)
// Recursive semantics everywhere: cocos2d-x uses std::recursive_mutex, and
// plain mutexes tolerate it.
static void* g_pthread_tls[64] = {};
static int   g_tls_key_count   = 0;

// Per-key zeroed scratch buffers (512 bytes each).  Returned by pt_getspecific
// when a slot is uninitialized, so code that doesn't null-check can read/write
// without faulting (e.g. ios_base::Init accessing [tls+0x28]).
static uint8_t g_tls_scratch[64][512];

// Bionic's PTHREAD_RECURSIVE/ERRORCHECK_MUTEX_INITIALIZER put the mutex type
// in bits 14-15 of the first word (0x8000 / 0x4000). To a libnx Mutex that
// looks like "locked by owner tag 0x8000" — sanitize before first use.
static _LOCK_RECURSIVE_T* bnxMutex(void* m) {
    _LOCK_RECURSIVE_T* rl = (_LOCK_RECURSIVE_T*)m;
    uint32_t v = rl->lock;
    if ((v == 0x4000 || v == 0x8000 || v == 0xC000) && rl->counter == 0)
        rl->lock = 0;
    return rl;
}
static int pt_mutex_init(void* m, const void*)  { memset(m, 0, 40); return 0; }
static int pt_mutex_lock(void* m)                { __libc_lock_acquire_recursive(bnxMutex(m)); return 0; }
static int pt_mutex_unlock(void* m)              { __libc_lock_release_recursive((_LOCK_RECURSIVE_T*)m); return 0; }
static int pt_mutex_trylock(void* m)             { return __libc_lock_try_acquire_recursive(bnxMutex(m)) ? 16 /*EBUSY*/ : 0; }
static int pt_mutex_destroy(void*)               { return 0; }

static int pt_cond_init(void* c, const void*)    { memset(c, 0, 48); return 0; }
static int pt_cond_signal(void* c)               { __libc_cond_signal((_COND_T*)c); return 0; }
static int pt_cond_broadcast(void* c)            { __libc_cond_broadcast((_COND_T*)c); return 0; }
static int pt_cond_wait(void* c, void* m) {
    __libc_cond_wait_recursive((_COND_T*)c, bnxMutex(m), UINT64_MAX);
    return 0;
}
// Bionic timespec is ABSOLUTE (CLOCK_REALTIME); convert to a relative wait.
static int pt_cond_timedwait(void* c, void* m, const struct timespec* abs) {
    uint64_t delta_ns = 10 * 1000 * 1000;  // fallback: 10 ms
    if (abs) {
        struct timeval now;
        gettimeofday(&now, nullptr);
        int64_t d = (int64_t)(abs->tv_sec - now.tv_sec) * 1000000000ll +
                    ((int64_t)abs->tv_nsec - (int64_t)now.tv_usec * 1000ll);
        delta_ns = d > 0 ? (uint64_t)d : 0;
    }
    int r = __libc_cond_wait_recursive((_COND_T*)c, bnxMutex(m), delta_ns);
    return r ? 110 /* Bionic ETIMEDOUT */ : 0;
}
static int pt_cond_destroy(void*)                { return 0; }

static int pt_rwlock_init(void* l, const void*)  { memset(l, 0, 56); return 0; }
static int pt_rwlock_rdlock(void* l)             { __libc_lock_acquire_recursive(bnxMutex(l)); return 0; }
static int pt_rwlock_wrlock(void* l)             { __libc_lock_acquire_recursive(bnxMutex(l)); return 0; }
static int pt_rwlock_unlock(void* l)             { __libc_lock_release_recursive((_LOCK_RECURSIVE_T*)l); return 0; }
static int pt_rwlock_destroy(void*)              { return 0; }

// Real threads. pthread_t is the address of the embedded libnx Thread, which
// equals threadGetSelf() inside that thread — so pthread_self()/pthread_equal
// stay consistent between creator and thread. Worker threads are pinned to
// cores 1/2 (main renders on core 0): a spinning worker at equal priority on
// the same core would never yield to the game loop on HOS.
extern void androidTlsInstallThread();  // loader.cpp — per-thread fake Bionic TLS
struct GameThread {
    Thread t;               // must stay first: pthread_t == &gt->t
    void* (*fn)(void*);
    void* arg;
    void* ret;
};
static void gameThreadTrampoline(void* p) {
    GameThread* gt = (GameThread*)p;
    androidTlsInstallThread();
    gt->ret = gt->fn ? gt->fn(gt->arg) : nullptr;
    compatLogFmt("game thread fn=%p exited", (void*)gt->fn);
}
static int pt_create(void** t, const void*, void* (*fn)(void*), void* arg) {
    static int s_core_rr = 0;
    GameThread* gt = (GameThread*)calloc(1, sizeof(GameThread));
    if (!gt) return 11;  // EAGAIN
    gt->fn = fn; gt->arg = arg;
    int core = 1 + (s_core_rr++ % 2);
    Result rc = threadCreate(&gt->t, gameThreadTrampoline, gt, nullptr,
                             1024 * 1024, 0x2C, core);
    if (R_SUCCEEDED(rc)) rc = threadStart(&gt->t);
    if (R_FAILED(rc)) {
        // Last resort: old synchronous behavior, so the game still progresses
        compatLogFmt("pthread_create: threadCreate/Start failed 0x%x — running fn=%p inline",
                     rc, (void*)fn);
        free(gt);
        if (fn) fn(arg);
        if (t) *t = (void*)0x1;
        return 0;
    }
    compatLogFmt("pthread_create: real thread fn=%p handle=%p core=%d",
                 (void*)fn, (void*)&gt->t, core);
    if (t) *t = &gt->t;
    return 0;
}
static int pt_join(void* th, void** ret) {
    if (!th || th == (void*)0x1) { if (ret) *ret = nullptr; return 0; }
    GameThread* gt = (GameThread*)th;  // Thread is the first member
    threadWaitForExit(&gt->t);
    if (ret) *ret = gt->ret;
    threadClose(&gt->t);
    // gt leaks by design: a stale pthread_t must never point at freed memory
    return 0;
}
static int pt_detach(void*)            { return 0; }  // struct leaks; harmless
static void* pt_self(void)             { return threadGetSelf(); }
static int pt_equal(void* a, void* b)  { return a == b ? 1 : 0; }
static int pt_key_create(int* k, void (*dtor)(void*)) {
    if (g_tls_key_count >= 64) return 11; // EAGAIN
    *k = g_tls_key_count++;
    compatLogFmt("pthread_key_create → key=%d dtor=%p", *k, (void*)dtor);
    return 0;
}
static int pt_key_delete(int) { return 0; }
static void* pt_getspecific(int k) {
    if (k < 0 || k >= 64) return nullptr;
    void* v = g_pthread_tls[k];
    if (!v) {
        // Return per-key scratch buffer instead of null so code that skips
        // null-checks (e.g. Bionic libc++ accessing [tls+0x28] for locale/EH
        // state) can read and write without faulting.  If the game later calls
        // setspecific, pt_setspecific replaces this with the real value.
        compatLogFmt("pthread_getspecific(key=%d) → scratch buf (first use)", k);
        g_pthread_tls[k] = g_tls_scratch[k];
        return g_tls_scratch[k];
    }
    return v;
}
static int pt_setspecific(int k, const void* v) {
    if (k < 0 || k >= 64) return 22; // EINVAL
    g_pthread_tls[k] = (void*)v;
    compatLogFmt("pthread_setspecific(key=%d, val=%p)", k, v);
    return 0;
}
static int pt_once(int* ctrl, void (*fn)(void)) {
    static Mutex s_once_lock;
    mutexLock(&s_once_lock);
    if (*ctrl == 0) {
        compatLogFmt("pthread_once: calling fn @%p", (void*)fn);
        fn();
        *ctrl = 1;
        compatLog("pthread_once: fn returned");
    }
    mutexUnlock(&s_once_lock);
    return 0;
}
static int pt_attr_init(void* a)    { memset(a, 0, 56); return 0; }
static int pt_attr_destroy(void*)   { return 0; }
static int pt_attr_setdetachstate(void*, int) { return 0; }
static int pt_attr_setstacksize(void*, size_t){ return 0; }
static int pt_attr_getstacksize(const void*, size_t* s) { if (s) *s = 65536; return 0; }

// ─── errno access (Bionic uses __errno() function) ───────────────────────────
static int* bionic_errno(void) { return &errno; }

// ─── dlopen / dlsym ──────────────────────────────────────────────────────────
// Real, ELF-loader-backed (see elfDlopen). A dlopen handle IS the LoadedSo*, so
// dlsym(handle, name) resolves against that specific library — which is what
// Unity's NativeLoader.load → dlopen("libunity.so") + dlsym(...) chain needs.
// A null/RTLD_DEFAULT handle falls back to the global shim + cross-library
// resolver, preserving the old behavior for games that dlsym without a handle.
static void* fake_dlopen(const char* path, int) {
    if (!path) return nullptr;

    const char* bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;

    // Mesa's EGL/GLES libraries are linked into the Switch executable through
    // the Switch portlibs. They are not Android guest ELF files and therefore
    // must not be searched for under game/lib. fake_dlsym() resolves their
    // functions from the global Mesa-backed shim table.
    if (!strcmp(bn, "libEGL.so") || !strcmp(bn, "libGLESv1_CM.so") ||
        !strcmp(bn, "libGLESv2.so") || !strcmp(bn, "libGL.so")) {
        compatLogFmt("dlopen: %s -> Mesa host shim handle", bn);
        return (void*)0xDEAD;
    }

    LoadedSo* so = elfDlopen(path);
    if (so) return (void*)so;

    // Unknown library: hand back a non-null sentinel so callers that only
    // null-check the handle still proceed, and route their dlsym through the
    // global resolver (old behavior) rather than hard-failing the load.
    compatLogFmt("dlopen: %s not resolvable to a loaded .so — sentinel handle", path);
    return (void*)0xDEAD;
}
static bool isCryAllocatorSym(const char* n) {
    if (!n) return false;
    return strcmp(n, "CryMalloc") == 0 ||
           strcmp(n, "CryRealloc") == 0 ||
           strcmp(n, "CryReallocSize") == 0 ||
           strcmp(n, "CryFree") == 0 ||
           strcmp(n, "CryFreeSize") == 0 ||
           strcmp(n, "CryModuleMalloc") == 0 ||
           strcmp(n, "CryModuleRealloc") == 0 ||
           strcmp(n, "CryModuleReallocSize") == 0 ||
           strcmp(n, "CryModuleFree") == 0 ||
           strcmp(n, "CryModuleFreeSize") == 0;
}

static void* fake_dlsym(void* handle, const char* sym) {
    if (!sym) return nullptr;
    const bool trace = isCryAllocatorSym(sym);

    // Handle-scoped lookup when dlopen returned a real LoadedSo*.
    if (handle && handle != (void*)0xDEAD) {
        LoadedSo* so = (LoadedSo*)handle;
        void* p = so->findSym(sym);
        if (p) {
            if (trace)
                compatLogFmt("dlsym: %s -> %s %p",
                             sym, so->path.c_str(), p);
            return p;
        }
        // Fall through to the global resolver — Unity libs reference plenty of
        // libc/GLES symbols our shim table owns, not just their own exports.
    }

    void* p = shimResolve(sym);
    if (p) {
        if (trace)
            compatLogFmt("dlsym: %s -> global/shim %p", sym, p);
        return p;
    }

    if (trace || sym[0] == '_')
        compatLogFmt("dlsym: unresolved %s", sym);
    return nullptr;
}
static int fake_dlclose(void*) { return 0; }
static const char* fake_dlerror(void) { return nullptr; }

// ─── EGL graphics-init logging wrappers ───────────────────────────────────────
// The EGL entries otherwise map straight to the real Switch EGL. Unity sets up
// its own EGL surface + context in nativeRecreateGfxState, and when that path
// fails there's nothing in the log to say where. These thin wrappers log the
// key graphics-init calls (and their results) once, then forward to real EGL —
// so a Unity bringup shows exactly how its context creation went. cocos2d-x is
// unaffected (it renders through the Core's own EGL setup, made before any game
// code runs, and doesn't call these).
static EGLSurface w_eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint* a) {
    // SDL's Android backend hands us the fake ANativeWindow object. Switch Mesa's
    // EGL backend expects the underlying libnx NWindow pointer instead.
    EGLNativeWindowType mesa_w = w;
    CompatLayer* cl = compatGet();
    if (cl && w == reinterpret_cast<EGLNativeWindowType>(&cl->window) && cl->window.nwin)
        mesa_w = reinterpret_cast<EGLNativeWindowType>(cl->window.nwin);

    EGLSurface s = eglCreateWindowSurface(d, c, mesa_w, a);
    compatLogFmt("EGL: eglCreateWindowSurface(win=%p -> %p) -> %p (err=0x%x)",
                 (void*)w, (void*)mesa_w, (void*)s, eglGetError());
    return s;
}
static EGLContext w_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint* a) {
    EGLContext ctx = eglCreateContext(d, c, share, a);
    compatLogFmt("EGL: eglCreateContext(share=%p) -> %p (err=0x%x)", (void*)share, (void*)ctx, eglGetError());
    return ctx;
}
static EGLBoolean w_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    EGLBoolean ok = eglMakeCurrent(d, draw, read, ctx);
    static int n = 0;
    if (n++ < 4)
        compatLogFmt("EGL: eglMakeCurrent(draw=%p ctx=%p) -> %d (err=0x%x)", (void*)draw, (void*)ctx, (int)ok, eglGetError());
    return ok;
}

// XRenderOGL resolves a number of legacy/extension GL entry points at runtime.
// Mesa's eglGetProcAddress() does not necessarily expose every compatibility
// symbol that we already provide through the ELF shim table. Fall back to the
// same shim resolver before returning NULL, otherwise the renderer can cache a
// null function pointer and later jump to PC=0 during pipeline setup/shutdown.
static void* w_eglGetProcAddress(const char* name) {
    if (!name || !*name)
        return nullptr;

    __eglMustCastToProperFunctionPointerType egl_p = eglGetProcAddress(name);
    void* p = reinterpret_cast<void*>(egl_p);
    if (p)
        return p;

    p = shimResolve(name);
    if (p) {
        compatLogFmt("EGL: eglGetProcAddress(%s) -> shim %p", name, p);
        return p;
    }

    compatLogFmt("EGL: eglGetProcAddress(%s) -> NULL", name);
    return nullptr;
}

// ─── SDL3 graphics-init probes ───────────────────────────────────────────────
// libXRenderOGL is an Android build of CryEngine and asks the guest SDL3 to
// create the Android/EGL window and GL context. These wrappers call the real
// guest SDL3 export, but log the result and SDL_GetError so a failed renderer
// init cannot masquerade as a later null function-pointer crash in shutdown.
static LoadedSo* sdl3_loaded() {
    static LoadedSo* cached = nullptr;
    if (!cached) cached = elfFindLoaded("libSDL3.so");
    return cached;
}

static void* sdl3_sym(const char* name) {
    LoadedSo* so = sdl3_loaded();
    return so ? so->findSym(name) : nullptr;
}

static const char* sdl3_error() {
    using Fn = const char* (*)();
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GetError"));
    return fn ? fn() : "";
}

static void* w_SDL_CreateWindow(const char* title, int w, int h, uint32_t flags) {
    using Fn = void* (*)(const char*, int, int, uint32_t);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_CreateWindow"));
    if (!fn) {
        compatLog("SDL: SDL_CreateWindow export not found");
        return nullptr;
    }
    void* win = fn(title, w, h, flags);
    compatLogFmt("SDL: SDL_CreateWindow(%s,%d,%d,0x%x) -> %p err=%s",
                 title ? title : "", w, h, flags, win, sdl3_error());
    return win;
}

static void* w_SDL_GL_CreateContext(void* window) {
    using Fn = void* (*)(void*);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GL_CreateContext"));
    if (!fn) {
        compatLog("SDL: SDL_GL_CreateContext export not found");
        return nullptr;
    }
    void* ctx = fn(window);
    compatLogFmt("SDL: SDL_GL_CreateContext(%p) -> %p err=%s",
                 window, ctx, sdl3_error());
    return ctx;
}

static int w_SDL_GL_MakeCurrent(void* window, void* context) {
    using Fn = bool (*)(void*, void*);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GL_MakeCurrent"));
    if (!fn) {
        compatLog("SDL: SDL_GL_MakeCurrent export not found");
        return 0;
    }

    bool ok = fn(window, context);
    compatLogFmt("SDL: SDL_GL_MakeCurrent(%p,%p) -> %d err=%s",
                 window, context, ok ? 1 : 0, sdl3_error());

    if (ok) {
        const GLubyte* version = glGetString(GL_VERSION);
        const GLubyte* renderer = glGetString(GL_RENDERER);
        const GLubyte* glsl = glGetString(GL_SHADING_LANGUAGE_VERSION);

        compatLogFmt("GL context: version=%s renderer=%s glsl=%s",
                     version ? reinterpret_cast<const char*>(version) : "?",
                     renderer ? reinterpret_cast<const char*>(renderer) : "?",
                     glsl ? reinterpret_cast<const char*>(glsl) : "?");

        const char* probes[] = {
            "glGenProgramsARB",
            "glBindProgramARB",
            "glProgramStringARB",
            "glVertexAttribPointerARB",
            "glProgramEnvParameter4fARB"
        };

        for (const char* name : probes) {
            __eglMustCastToProperFunctionPointerType p = eglGetProcAddress(name);
            compatLogFmt("GL proc probe: %s -> %p", name,
                         reinterpret_cast<void*>(p));
        }
    }

    return ok ? 1 : 0;
}

// ─── libandroid shims ────────────────────────────────────────────────────────
// AAssetManager
static AAsset* asset_open(AAssetManager* mgr, const char* fn, int) {
    if (!mgr || !fn) return nullptr;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", mgr->base_path, fn);
    FILE* f = fopen(path, "rb");
    if (!f) {
        compatLogFmt("AAsset_open: not found: %s", path);
        return nullptr;
    }
    // This is cocos2d-x's REAL texture/asset loading path on Android
    // (FileUtilsAndroid reads through AAssetManager, not plain fopen) — the
    // same 64KB-buffer reasoning as stub_fopen applies here too, but this
    // call site is a separate real fopen() our own compat code makes on the
    // game's behalf, so it never picked up that fix. Thread-tagged logging
    // confirmed textures decode synchronously on the main render thread
    // during driving as new scenery streams in — this is almost certainly
    // the actual file-read path behind that stutter, more so than the
    // game's own direct fopen calls.
    setvbuf(f, nullptr, _IOFBF, 64 * 1024);
    AAsset* a = (AAsset*)calloc(1, sizeof(AAsset));
    a->fp = f;
    fseek(f, 0, SEEK_END);
    a->size = (int64_t)ftell(f);
    rewind(f);
    const size_t path_len = std::strlen(path);
    const size_t path_copy = path_len < sizeof(a->path) - 1 ? path_len : sizeof(a->path) - 1;
    std::memcpy(a->path, path, path_copy);
    a->path[path_copy] = '\0';
    return a;
}
static void   asset_close(AAsset* a)  { if (a) { fclose(a->fp); free(a); } }
static int    asset_read(AAsset* a, void* buf, size_t n) {
    if (!a) return -1;
    return (int)fread(buf, 1, n, a->fp);
}
static int64_t asset_seek(AAsset* a, int64_t off, int whence) {
    if (!a) return -1;
    fseek(a->fp, (long)off, whence);
    return (int64_t)ftell(a->fp);
}
static int64_t asset_seek64(AAsset* a, int64_t off, int whence) {
    return asset_seek(a, off, whence);
}
static int64_t asset_length(AAsset* a)         { return a ? a->size : 0; }
static int64_t asset_remain(AAsset* a) {
    if (!a) return 0;
    return a->size - (int64_t)ftell(a->fp);
}
static const void* asset_buffer(AAsset*) { return nullptr; }  // no mmap on Switch
static int asset_isAllocated(AAsset*)    { return 0; }
static AAssetManager* assetMgr_fromJava(void*) {
    // Return the global compat layer's asset manager
    return &compatGet()->asset_mgr;
}
static AAssetDir* asset_openDir(AAssetManager*, const char* d) {
    (void)d; return (AAssetDir*)0x1; // stub
}
static const char* asset_dirNext(AAssetDir*) { return nullptr; }
static void        asset_dirRewind(AAssetDir*) {}
static void        asset_dirClose(AAssetDir*) {}

// ANativeWindow
static int32_t nwin_getWidth(ANativeWindow* w)  { return w ? w->width  : 1280; }
static int32_t nwin_getHeight(ANativeWindow* w) { return w ? w->height : 720;  }
static int32_t nwin_getFormat(ANativeWindow* w) { return w ? w->format : 1; }
static int32_t nwin_setBuffersGeometry(ANativeWindow* w, int32_t wd, int32_t ht, int32_t fmt) {
    if (w) { w->width = wd; w->height = ht; w->format = fmt; }
    return 0;
}
static void nwin_acquire(ANativeWindow*) {}
static void nwin_release(ANativeWindow*) {}
static int  nwin_lock(ANativeWindow*, void* buf, const void* dirtyBounds) {
    (void)buf; (void)dirtyBounds; return -1;
}
static int  nwin_unlockAndPost(ANativeWindow*) { return -1; }

// ALooper
static ALooper* looper_forThread(void) { return &compatGet()->looper; }
static ALooper* looper_prepare(int)    { return &compatGet()->looper; }
static void     looper_acquire(ALooper*) {}
static void     looper_release(ALooper*) {}
static int looper_pollOnce(int timeout, int* fd, int* events, void** data) {
    (void)timeout; (void)fd; (void)events; (void)data;
    return ALOOPER_POLL_TIMEOUT;
}
static int looper_pollAll(int timeout, int* fd, int* events, void** data) {
    return looper_pollOnce(timeout, fd, events, data);
}
static void looper_wake(ALooper*) {}
static int  looper_addFd(ALooper*, int, int, int, void*, void*) { return 1; }
static int  looper_removeFd(ALooper*, int) { return 1; }

// AInputEvent
static int32_t  ev_getType(const AInputEvent* e) { return e ? e->type   : 0; }
static int32_t  ev_getAction(const AInputEvent* e){ return e ? e->action : 0; }
static float    ev_getX(const AInputEvent* e, size_t) { return e ? e->x  : 0; }
static float    ev_getY(const AInputEvent* e, size_t) { return e ? e->y  : 0; }
static int32_t  ev_getKeyCode(const AInputEvent*)  { return 0; }
static int32_t  ev_getMetaState(const AInputEvent*){ return 0; }
static int32_t  ev_getSource(const AInputEvent*)   { return 0; }
static size_t   ev_getPointerCount(const AInputEvent*){ return 1; }
static float    ev_getPressure(const AInputEvent*, size_t) { return 1.0f; }
static float    ev_getSize(const AInputEvent*, size_t) { return 0.0f; }
static int32_t  ev_getPointerId(const AInputEvent*, size_t idx) { return (int32_t)idx; }
[[maybe_unused]] static void ev_setAction(AInputEvent*, int) {}

static int iq_getEvent(AInputQueue*, AInputEvent** e) { (void)e; return -1; }
static int iq_hasEvents(AInputQueue*) { return 0; }
static void iq_finishEvent(AInputQueue*, AInputEvent*, int) {}

// Configuration (AConfiguration — minimal stubs)
static void* acfg_new(void)    { return calloc(1, 256); }
static void  acfg_delete(void* c) { free(c); }
static void  acfg_fromAssetManager(void*, AAssetManager*) {}
static int32_t acfg_getDensity(void*) { return 320; } // xhdpi (Switch screen)
static int32_t acfg_getOrientation(void*) { return 2; } // landscape
static int32_t acfg_getScreenSize(void*) { return 3; } // large
static int32_t acfg_getSdkVersion(void*) { return 26; }

// ─── sincosf ─────────────────────────────────────────────────────────────────
static void stub_sincosf(float x, float* s, float* c) { *s = sinf(x); *c = cosf(x); }

// ─── stpcpy (POSIX — may not be exposed by newlib header) ────────────────────
static char* stub_stpcpy(char* dst, const char* src) {
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    return dst + n;
}

// ─── vasprintf (GNU extension) ────────────────────────────────────────────────
static int stub_vasprintf(char** out, const char* fmt, va_list va) {
    va_list va2; va_copy(va2, va);
    int n = vsnprintf(nullptr, 0, fmt, va2); va_end(va2);
    if (n < 0) { *out = nullptr; return -1; }
    *out = (char*)malloc((size_t)n + 1);
    if (!*out) return -1;
    vsprintf(*out, fmt, va);
    return n;
}

// ─── POSIX semaphores — real, blocking (game threads are real now) ───────────
// Bionic sem_t is 16 bytes on LP64; our lock+cond+count fits in 12.
struct BnxSem { _LOCK_T lock; _COND_T cv; volatile int value; };
static int stub_sem_init(BnxSem* s, int, unsigned int v) {
    memset(s, 0, sizeof(BnxSem));
    s->value = (int)v;
    return 0;
}
static int stub_sem_destroy(BnxSem*) { return 0; }
static int stub_sem_post(BnxSem* s) {
    __libc_lock_acquire(&s->lock);
    s->value++;
    __libc_cond_signal(&s->cv);
    __libc_lock_release(&s->lock);
    return 0;
}
static int stub_sem_wait(BnxSem* s) {
    __libc_lock_acquire(&s->lock);
    while (s->value <= 0)
        __libc_cond_wait(&s->cv, &s->lock, UINT64_MAX);
    s->value--;
    __libc_lock_release(&s->lock);
    return 0;
}
static int stub_sem_trywait(BnxSem* s) {
    int r = 0;
    __libc_lock_acquire(&s->lock);
    if (s->value > 0) s->value--;
    else { errno = EAGAIN; r = -1; }
    __libc_lock_release(&s->lock);
    return r;
}

// ─── Network stubs (no BSD socket service configured) ────────────────────────
// Network probes are logged, not just refused.
//
// The console these run on is offline, and games gate content on that — Hill
// Climb Racing stops at the end of its tutorial. Failing fast is right, and
// these already do (no timeouts, nothing hangs), but a silent refusal leaves
// "it needs internet" as the whole diagnosis. Naming the host the game asked
// for turns that into something specific: a sign-in endpoint, an ads network
// and a save-sync service are three different problems, and only one of them
// is worth patching around.
//
// Rate-limited, because a game that has decided it wants the network will ask
// repeatedly and there is no value in a thousand identical lines.
static void logNetProbe(const char* what, const char* detail) {
    static int n = 0;
    if (n >= 24) return;
    n++;
    compatLogFmt("net: %s(%s) → offline%s", what, detail ? detail : "",
                 n == 24 ? "  [further network calls not logged]" : "");
}

static int stub_socket(int, int, int)               { errno = ENOTSUP; return -1; }
static int stub_bind(int, const void*, unsigned)    { errno = ENOTSUP; return -1; }
static int stub_connect(int, const void*, unsigned) { errno = ECONNREFUSED; return -1; }
static int stub_listen(int, int)                    { errno = ENOTSUP; return -1; }
static int stub_select(int, void*, void*, void*, void*) { errno = ENOTSUP; return -1; }
static ssize_t stub_recv(int, void*, size_t, int)   { errno = ENOTSUP; return -1; }
static ssize_t stub_send(int, const void*, size_t, int) { errno = ENOTSUP; return -1; }
static int stub_getsockname(int, void*, unsigned*)  { errno = ENOTSUP; return -1; }
static int stub_getsockopt(int, int, int, void*, unsigned*) { errno = ENOTSUP; return -1; }
static void* stub_gethostbyname(const char* h)      { logNetProbe("gethostbyname", h); return nullptr; }
static int stub_fcntl_sock(int, int, ...)           { return 0; }
static int* stub_get_h_errno(void)                  { static int h = 0; return &h; }

// ─── Scheduling ──────────────────────────────────────────────────────────────
static int stub_sched_yield(void) { svcSleepThread(0); return 0; }

// ─── System ──────────────────────────────────────────────────────────────────
static long stub_sysconf(int n) {
    switch (n) {
        case 30: return 4096;  // _SC_PAGESIZE
        case 84: return 4;     // _SC_NPROCESSORS_ONLN — Tegra X1 has 4 ARM cores
        default:
            compatLogFmt("sysconf(%d) -> -1 (unknown)", n);
            return -1;
    }
}
static char* stub_getcwd(char* buf, size_t sz) {
    if (!buf || sz == 0)
        return nullptr;

    // CryEngine uses getcwd() when building its master root and when its
    // internal filesystem enumeration resolves relative paths. Returning "/"
    // here makes paths such as Shaders/Scripts resolve from the Switch root
    // instead of the game's data directory.
    return ::getcwd(buf, sz);
}
// getrandom(2) works (libnx CSRNG) — libc++'s std::random_device may use it.
// Other syscall numbers are logged so the compat log shows what the game wanted.
// Defined further down, next to their named-symbol siblings.
static pid_t stub_getpid(void);
static uid_t stub_getuid(void);
static gid_t stub_getgid(void);
static pid_t stub_gettid(void);

static long stub_syscall(long n, ...) {
    va_list va; va_start(va, n);
    long r;
    switch (n) {
        // Numbers are the arm64 (asm-generic) ABI. A game reaching libc through
        // raw syscall() rather than the named entry point got ENOSYS even where
        // we already had a perfectly good shim for it — Brain It On asks for
        // 178 (gettid) during its constructors and was told the kernel has no
        // such call. Route the ones we can answer to the same implementations
        // the named symbols use, so it doesn't matter which door the game came
        // through. Anything genuinely unsupported still logs and fails.
        case 278: {  // getrandom
            void*  buf = va_arg(va, void*);
            size_t len = va_arg(va, size_t);
            if (buf && len) randomGet(buf, len);
            r = (long)len;
            break;
        }
        case 101: {  // nanosleep
            const struct timespec* req = va_arg(va, const struct timespec*);
            if (req) svcSleepThread((u64)req->tv_sec * 1000000000ULL + (u64)req->tv_nsec);
            r = 0;
            break;
        }
        case 113: {  // clock_gettime
            int              cid = va_arg(va, int);
            struct timespec* ts  = va_arg(va, struct timespec*);
            r = ts ? clock_gettime(cid, ts) : -1;
            break;
        }
        case 169: {  // gettimeofday
            struct timeval* tv = va_arg(va, struct timeval*);
            void*           tz = va_arg(va, void*);
            (void)tz;
            r = tv ? gettimeofday(tv, nullptr) : -1;
            break;
        }
        case 124: r = stub_sched_yield();      break;
        case 172: r = stub_getpid();           break;  // getpid
        case 173: r = stub_getpid();           break;  // getppid — no parent here
        case 174: r = stub_getuid();           break;  // getuid
        case 175: r = stub_getuid();           break;  // geteuid
        case 176: r = stub_getgid();           break;  // getgid
        case 177: r = stub_getgid();           break;  // getegid
        case 178: r = stub_gettid();           break;  // gettid
        default:
            va_end(va);
            compatLogFmt("syscall(%ld) → ENOSYS", n);
            errno = ENOSYS;
            return -1;
    }
    va_end(va);
    return r;
}
// Bionic API-28 entropy entry points (in case the game links them directly)
static int stub_getentropy(void* buf, size_t len) {
    if (buf && len) randomGet(buf, len);
    return 0;
}
static ssize_t stub_getrandom(void* buf, size_t len, unsigned) {
    if (buf && len) randomGet(buf, len);
    return (ssize_t)len;
}
static int  stub_dl_iterate_phdr(void*, void*) { return 0; }

// ─── Letterboxing a portrait game onto a landscape panel ────────────────────
// The game sets its own viewport and scissor from the window size we reported,
// so it draws a correctly-shaped picture — in the bottom-left corner. These
// three shims shift its rendering into the content rect and keep it there.
//
// glClear is the one that matters most: a clear ignores the viewport entirely
// and would wipe the black bars with the game's background colour, which is
// how a "letterboxed" game ends up with coloured bars that flicker. Scissoring
// the clear to the content rect is what actually keeps the bars black.
static void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    const Presentation& p = orientGet();
    glViewport(x + p.content_x, y + p.content_y, w, h);
}

static void w_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    const Presentation& p = orientGet();
    glScissor(x + p.content_x, y + p.content_y, w, h);
}

static void w_glClear(GLbitfield mask) {
    const Presentation& p = orientGet();
    if (!p.pillarboxed) { glClear(mask); return; }

    // Confine the clear, then put the scissor box back exactly as the game left
    // it — it may be mid-frame and relying on it.
    GLboolean had = glIsEnabled(GL_SCISSOR_TEST);
    GLint     box[4];
    glGetIntegerv(GL_SCISSOR_BOX, box);

    glEnable(GL_SCISSOR_TEST);
    glScissor(p.content_x, p.content_y, p.content_w, p.content_h);
    glClear(mask);

    glScissor(box[0], box[1], box[2], box[3]);
    if (!had) glDisable(GL_SCISSOR_TEST);
}


// ─── ARM32 (AArch32) EABI helpers ───────────────────────────────────────────
// armeabi-v7a compilers emit these instead of plain memcpy/memset for aggregate
// copies and initialisers, so a 32-bit game reaches them constantly even though
// its source never mentions them. They do not exist on arm64 at all, which is
// why nothing needed them until now.
//
// The argument order is the trap: __aeabi_memset takes (dest, n, c) — length
// before the fill byte — which is the reverse of memset. Wiring it straight
// through would fill with the length and set the count from the colour value,
// and the corruption would look nothing like a missing symbol.
extern "C" {
static void* ae_memcpy (void* d, const void* s, size_t n) { return memcpy(d, s, n); }
static void* ae_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
static void  ae_memset (void* d, size_t n, int c)         { memset(d, c, n); }
static void  ae_memclr (void* d, size_t n)                { memset(d, 0, n); }
}

// ─── OpenSL ES ──────────────────────────────────────────────────────────────
// cocos2d-x asks for OpenSL when it is present and falls back to its Java audio
// path when the engine cannot be created. Returning failure from slCreateEngine
// is therefore not a stub that breaks sound — it is the documented way to tell
// the game to use the path we already implement, which is the one HCR's audio
// runs through today. The SL_IID_* symbols are data, and cocos2d-x takes their
// addresses before it ever checks whether the engine exists, so they have to
// resolve to something readable.
static const uint32_t kSlIidStorage[16] = {0};
static int32_t sl_createEngine(void*, uint32_t, const void*, uint32_t,
                               const void*, const void*) {
    compatLog("OpenSL: slCreateEngine → not supported, game will use its Java audio path");
    return 0x0000000C;   // SL_RESULT_FEATURE_UNSUPPORTED
}

// OpenAL Soft Android/Bionic imports that must resolve even though the
// Android-only OpenSL backend is explicitly disabled on Switch.
static int stub_sched_get_priority_min(int) { return 0; }
static int stub_sched_get_priority_max(int) { return 0; }
static int stub_pthread_setschedparam(void*, int, const void*) { return 0; }
static int stub_pthread_rwlock_tryrdlock(void* m) { return pt_rwlock_rdlock(m); }
static int stub_pthread_rwlock_trywrlock(void* m) { return pt_rwlock_wrlock(m); }

static ssize_t stub___readlink_chk(const char* path, char* buf,
                                   size_t bufsz, size_t) {
    return stub_readlink(path, buf, bufsz);
}

static int stub___cxa_thread_atexit_impl(void (*)(void*), void*, void*) {
    return 0;
}

// GNU C++ ABI TLS initialization hook emitted by OpenAL Soft for its
// thread_local current-context pointer. Switch does not need Android's TLS
// runtime here because the context pointer starts as zero.
extern "C" void openal_tls_context_init(void)
    __asm__("_ZTHN10ALCcontext13sLocalContextE");
extern "C" void openal_tls_context_init(void) {}

// ─── Assorted libc the 32-bit build reaches for ─────────────────────────────
static long   stub_lround (double x)      { return (long)(x < 0 ? x - 0.5 : x + 0.5); }
static long   stub_lroundf(float  x)      { return (long)(x < 0 ? x - 0.5f : x + 0.5f); }
static long long stub_llround(double x)   { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }
static float  stub_remainderf(float a, float b) { return remainderf(a, b); }
static float  stub_erff (float x)         { return erff(x); }
static float  stub_erfcf(float x)         { return erfcf(x); }

static void*  stub_memmem(const void* h, size_t hl, const void* n, size_t nl) {
    if (!nl || hl < nl) return nullptr;
    const unsigned char* hp = (const unsigned char*)h;
    for (size_t i = 0; i + nl <= hl; i++)
        if (memcmp(hp + i, n, nl) == 0) return (void*)(hp + i);
    return nullptr;
}

static char* stub_inet_ntoa(uint32_t addr) {
    static char buf[16];
    unsigned char* b = (unsigned char*)&addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}
static const char* stub_gai_strerror(int) { return "name resolution unavailable"; }
static void* stub_getprotobyname(const char*) { return nullptr; }
static int   stub_mlock(const void*, size_t) { return 0; }   // nothing is paged out here
static long  stub_pathconf(const char*, int) { return -1; }

// The *at() family, resolved against the process CWD — Switch has no directory
// file descriptors, and every caller here passes AT_FDCWD anyway.
static int stub_openat(int, const char* path, int flags, ...) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    va_list va;
    va_start(va, flags);
    int mode = 0;
    if (flags & O_CREAT)
        mode = va_arg(va, int);
    va_end(va);

    auto doOpen = [&](const char* p) -> int {
        return (flags & O_CREAT) ? open(p, flags, mode) : open(p, flags);
    };

    int fd = doOpen(ioPath);
    if (fd < 0 && ioPath) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath)
            fd = doOpen(resolved.c_str());
    }
    return fd;
}

// ─── Case-insensitive directory/file enumeration for CryPak ─────────────────
//
// The original Far Cry CryPak normalizes paths to lower-case before calling
// _findfirst64(). The Switch filesystem is case-sensitive, while shipped game
// trees commonly still use names such as FCData/ and Shaders/. Let directory
// enumeration recover the real on-disk spelling, so CryPak can actually find
// its *.pak archives instead of silently treating the directory as empty.

static std::string asciiLower(std::string value) {
    for (char& c : value)
        c = (char)std::tolower((unsigned char)c);
    return value;
}

static bool sameNameNoCase(const char* a, const char* b) {
    return a && b && asciiLower(a) == asciiLower(b);
}

// Resolve a path component-by-component without changing the path returned to
// callers. This is only used after the normal direct lookup fails.
static bool resolvePathCaseInsensitive(const char* input, std::string& resolved) {
    if (!input || !*input)
        return false;

    std::string path(input);
    for (char& c : path) {
        if ((unsigned char)c == 92)
            c = '/';
    }

    const bool absolute = !path.empty() && path[0] == '/';
    std::string current = absolute ? "/" : ".";

    size_t pos = absolute ? 1 : 0;
    while (pos <= path.size()) {
        size_t end = path.find('/', pos);
        if (end == std::string::npos)
            end = path.size();

        const std::string part = path.substr(pos, end - pos);
        pos = end + 1;

        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            size_t slash = current.find_last_of('/');
            if (slash == std::string::npos)
                current = ".";
            else if (slash == 0)
                current = "/";
            else
                current.erase(slash);
            continue;
        }

        std::string direct = current;
        if (direct.empty() || direct == ".") {
            direct = part;
        } else if (direct == "/") {
            direct += part;
        } else {
            direct += "/";
            direct += part;
        }

        struct stat st = {};
        if (stat(direct.c_str(), &st) == 0) {
            current = direct;
            continue;
        }

        std::string parent = current.empty() ? "." : current;
        DIR* d = opendir(parent.c_str());
        if (!d)
            return false;

        std::string actual;
        while (struct dirent* ent = readdir(d)) {
            if (sameNameNoCase(ent->d_name, part.c_str())) {
                actual = ent->d_name;
                break;
            }
        }
        closedir(d);

        if (actual.empty())
            return false;

        if (parent == "/" || parent.empty())
            current = "/" + actual;
        else if (parent == ".")
            current = actual;
        else
            current = parent + "/" + actual;
    }

    resolved = current.empty() ? (absolute ? "/" : ".") : current;
    return true;
}




static bool pakExtractPrefix(const std::string& pakPath,
                             const std::string& prefix,
                             bool& extractedAny) {
    extractedAny = false;

    FILE* pak = fopen(pakPath.c_str(), "rb");
    if (!pak)
        return false;

    if (fseek(pak, 0, SEEK_END) != 0) {
        fclose(pak);
        return false;
    }

    const long fileSize = ftell(pak);
    if (fileSize < 22) {
        fclose(pak);
        return false;
    }

    const size_t tailSize = (size_t)((fileSize < 0x10016L) ? fileSize : 0x10016L);
    std::vector<unsigned char> tail(tailSize);
    if (fseek(pak, fileSize - (long)tailSize, SEEK_SET) != 0 ||
        !pakReadExact(pak, tail.data(), tail.size())) {
        fclose(pak);
        return false;
    }

    size_t eocd = tail.size();
    while (eocd >= 22) {
        --eocd;
        if (eocd + 4 <= tail.size() &&
            pakRd32(tail.data() + eocd) == 0x06054b50u)
            break;
    }
    if (eocd + 22 > tail.size()) {
        fclose(pak);
        return false;
    }

    const uint16_t entries = pakRd16(tail.data() + eocd + 10);
    const uint32_t cdSize = pakRd32(tail.data() + eocd + 12);
    const uint32_t cdOffset = pakRd32(tail.data() + eocd + 16);
    if (!entries || !cdSize || cdSize > 128 * 1024 * 1024u) {
        fclose(pak);
        return false;
    }

    std::vector<unsigned char> cd(cdSize);
    if (fseek(pak, (long)cdOffset, SEEK_SET) != 0 ||
        !pakReadExact(pak, cd.data(), cd.size())) {
        fclose(pak);
        return false;
    }

    std::vector<std::string> names;
    size_t pos = 0;
    for (uint16_t i = 0; i < entries && pos + 46 <= cd.size(); ++i) {
        const unsigned char* h = cd.data() + pos;
        if (pakRd32(h) != 0x02014b50u)
            break;

        const uint16_t nameLen = pakRd16(h + 28);
        const uint16_t extraLen = pakRd16(h + 30);
        const uint16_t commentLen = pakRd16(h + 32);
        const size_t recordSize = 46u + nameLen + extraLen + commentLen;
        if (pos + recordSize > cd.size())
            break;

        std::string name((const char*)h + 46, nameLen);
        const std::string normalized = pakNormalizeName(name.c_str());

        std::string wantedPrefix = pakNormalizeName(prefix.c_str());
        if (!wantedPrefix.empty() && wantedPrefix.back() != '/')
            wantedPrefix.push_back('/');

        if (normalized.size() > wantedPrefix.size() &&
            normalized.compare(0, wantedPrefix.size(), wantedPrefix) == 0) {
            names.push_back(name);
        }

        pos += recordSize;
    }

    fclose(pak);

    for (const std::string& name : names) {
        const std::string normalized = pakNormalizeName(name.c_str());
        if (normalized.empty())
            continue;

        // CryPak::FindFirst() lowercases the directory path before calling
        // the platform _findfirst64().  On the Switch's case-sensitive
        // filesystem that means we need a real lowercase mirror of the PAK
        // tree, while FOpen() can still use the archive's original spelling.
        //
        // Extract the lowercase path directly from the archive rather than
        // copying the just-created file. This also handles archive entries that
        // use '\\' as path separators, which must become real POSIX '/' paths
        // on the Switch filesystem.
        if (!pakExtractEntry(pakPath, normalized, name))
            continue;

        if (name != normalized &&
            !pakExtractEntry(pakPath, normalized, normalized)) {
            // The original spelling is still useful for FOpen(), but a missing
            // lowercase mirror means CryPak's FindFirst() cannot recurse into it.
            continue;
        }

        extractedAny = true;
    }

    return true;
}


static void countShaderScriptsRecursive(const std::string& directory,
                                        int& cslCount,
                                        int& csiCount,
                                        int depth = 0) {
    if (depth > 16)
        return;

    DIR* d = opendir(directory.c_str());
    if (!d)
        return;

    while (struct dirent* ent = readdir(d)) {
        const char* name = ent->d_name;
        if (!name || !*name || !std::strcmp(name, ".") || !std::strcmp(name, ".."))
            continue;

        std::string full = directory;
        if (full.empty() || full == ".")
            full = name;
        else if (full == "/")
            full += name;
        else {
            full += "/";
            full += name;
        }

        struct stat st = {};
        if (stat(full.c_str(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            countShaderScriptsRecursive(full, cslCount, csiCount, depth + 1);
            continue;
        }

        std::string lower = asciiLower(name);
        if (lower.size() >= 4 &&
            lower.compare(lower.size() - 4, 4, ".csl") == 0) {
            ++cslCount;
        } else if (lower.size() >= 4 &&
                   lower.compare(lower.size() - 4, 4, ".csi") == 0) {
            ++csiCount;
        }
    }

    closedir(d);
}




static bool tryMaterializePakDirectory(const char* path) {
    if (!path || !*path)
        return false;

    std::string wanted = path;
    for (char& c : wanted) {
        if ((unsigned char)c == 92)
            c = '/';
    }
    while (!wanted.empty() && wanted.back() == '/')
        wanted.pop_back();

    if (wanted.empty())
        return false;

    // CryPak::ScanZips() checks every opened archive whose bind root matches
    // the requested directory. Do the same here: shader scripts are not
    // guaranteed to live in Shaders.pak specifically; they may be packed into
    // another FCData archive such as Scripts.pak.
    std::string resolvedRoot;
    if (!resolvePathCaseInsensitive("FCData", resolvedRoot))
        return false;

    DIR* root = opendir(resolvedRoot.c_str());
    if (!root)
        return false;

    bool extractedAny = false;
    int pakCount = 0;
    for (struct dirent* ent = readdir(root); ent; ent = readdir(root)) {
        const char* name = ent->d_name;
        const size_t len = std::strlen(name);
        if (len < 4 ||
            std::tolower((unsigned char)name[len - 4]) != '.' ||
            std::tolower((unsigned char)name[len - 3]) != 'p' ||
            std::tolower((unsigned char)name[len - 2]) != 'a' ||
            std::tolower((unsigned char)name[len - 1]) != 'k')
            continue;

        std::string pakPath = resolvedRoot + "/" + name;
        struct stat st = {};
        if (::stat(pakPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        ++pakCount;
        bool thisPak = false;
        if (pakExtractPrefix(pakPath, wanted, thisPak) && thisPak)
            extractedAny = true;
    }
    closedir(root);

    if (extractedAny)
        compatLogFmt("pak DIR READY: %s (scanned %d FCData paks)", path, pakCount);
    return extractedAny;
}

void compatProbePakArchives(const char* dataRoot) {
    const std::string root = dataRoot ? dataRoot : "";
    std::string fcdata = root;
    if (!fcdata.empty())
        fcdata += "/FCData";

    compatLogFmt("PAK BIND MODEL: Android uses an empty bind root for FCData/*.pak; expected virtual root=%s",
                 root.empty() ? "<cwd>" : root.c_str());
    compatLogFmt("PAK BIND MODEL: physical FCData=%s",
                 fcdata.empty() ? "FCData" : fcdata.c_str());
}

static bool compatIsPakPath(const char* path) {
    if (!path)
        return false;
    const std::string p = asciiLower(path);
    return p.size() >= 4 && p.compare(p.size() - 4, 4, ".pak") == 0;
}

static void compatLogPakOpenState(FILE* f, const char* path) {
    if (!f || !compatIsPakPath(path))
        return;

    const long saved = ftell(f);
    if (saved < 0 || fseek(f, 0, SEEK_END) != 0) {
        compatLogFmt("PAK OPEN DIAG: %s size=UNKNOWN seek=FAIL",
                     path ? path : "?");
        if (saved >= 0)
            fseek(f, saved, SEEK_SET);
        return;
    }

    const long fileSize = ftell(f);
    bool zipValid = false;
    uint16_t entries = 0;
    uint32_t cdSize = 0;
    uint32_t cdOffset = 0;

    if (fileSize >= 22) {
        const size_t tailSize =
            (size_t)((fileSize < 0x10016L) ? fileSize : 0x10016L);
        std::vector<unsigned char> tail(tailSize);
        if (fseek(f, fileSize - (long)tailSize, SEEK_SET) == 0 &&
            pakReadExact(f, tail.data(), tail.size())) {
            size_t eocd = tail.size();
            while (eocd >= 22) {
                --eocd;
                if (eocd + 4 <= tail.size() &&
                    pakRd32(tail.data() + eocd) == 0x06054b50u)
                    break;
            }
            if (eocd + 22 <= tail.size()) {
                entries = pakRd16(tail.data() + eocd + 10);
                cdSize = pakRd32(tail.data() + eocd + 12);
                cdOffset = pakRd32(tail.data() + eocd + 16);
                zipValid = cdOffset <= (uint32_t)fileSize &&
                           cdSize <= (uint32_t)fileSize &&
                           (uint64_t)cdOffset + (uint64_t)cdSize <= (uint64_t)fileSize;
            }
        }
    }

    compatLogFmt("PAK OPEN DIAG: %s size=%ld zip=%d entries=%u cd_size=%u cd_offset=%u",
                 path ? path : "?", fileSize, zipValid ? 1 : 0,
                 (unsigned)entries, (unsigned)cdSize, (unsigned)cdOffset);

    if (saved >= 0)
        fseek(f, saved, SEEK_SET);
}

static bool patchCommonSubroutinesIntoShaderMacro(const char* macroPath,
                                                  const char* programPath);

void compatPrepareShaderDirectories(const char* dataRoot) {
    const char* dirs[] = {
        "Shaders/HWScripts/Declarations",
        "Shaders/Scripts",
        "Shaders/HWScripts",
        nullptr
    };

    prepareShaderSourceFiles(dataRoot);

    for (size_t i = 0; dirs[i]; ++i) {
        const bool ready = tryMaterializePakDirectory(dirs[i]);
        int cslCount = 0;
        int csiCount = 0;

        if (ready) {
            std::string resolved = dirs[i];
            if (!resolvePathCaseInsensitive(dirs[i], resolved))
                resolved = dirs[i];
            countShaderScriptsRecursive(resolved, cslCount, csiCount);
        }

        compatLogFmt("shader dir: %s ready=%d csl=%d csi=%d",
                     dirs[i], ready ? 1 : 0, cslCount, csiCount);
    }

    // The Android CryPak implementation used by the guest shader loader can
    // enumerate the physical directory without reproducing the ZIP directory
    // tree exactly as desktop CryPak does. In particular, the two declaration
    // CSL files live below HWScripts/Declarations and are required to register
    // logical Cg scripts such as CommonSubroutines. Materialize these few
    // declaration sources at the HWScripts root as a compatibility fallback.
    //
    // CGVProgramms.csl includes CGVPMacro.csi with a relative include, so both
    // files must be present together at the fallback location.
    const bool rootCgvProgramms =
        tryMaterializeUniquePakBasename(
            "Shaders/HWScripts/CGVProgramms.csl");
    const bool rootCgvMacro =
        tryMaterializeUniquePakBasename(
            "Shaders/HWScripts/CGVPMacro.csi");
    const bool rootCgpShaders =
        tryMaterializeUniquePakBasename(
            "Shaders/HWScripts/CGPShaders.csl");

    compatLogFmt("shader root fallback: CGVProgramms=%s CGVPMacro=%s CGPShaders=%s",
                 rootCgvProgramms ? "ready" : "missing",
                 rootCgvMacro ? "ready" : "missing",
                 rootCgpShaders ? "ready" : "missing");

    // Android CGP shader files can reference CommonSubroutines before
    // the guest loader has successfully registered CGVProgramms.csl.
    // Materialize the exact declaration tree and make the macro self-contained
    // so the shared script is available regardless of the guest loader path.
    const bool commonPatch = patchCommonSubroutinesIntoShaderMacro(
        "Shaders/HWScripts/Declarations/CGVPMacro.csi",
        "Shaders/HWScripts/Declarations/CGVProgramms.csl");
    compatLogFmt("shader common patch result: %s",
                 commonPatch ? "ready" : "not-applied");

    const bool commonCsl =
        tryMaterializeUniquePakBasename(
            "Shaders/Scripts/CommonSubroutines.csl");
    const bool commonCsi =
        tryMaterializeUniquePakBasename(
            "Shaders/Scripts/CommonSubroutines.csi");

    compatLogFmt("shader common: csl=%s csi=%s",
                 commonCsl ? "ready" : "missing",
                 commonCsi ? "ready" : "missing");
}


static bool patchCommonSubroutinesIntoShaderMacro(const char* macroPath,
                                                  const char* programPath) {
    if (!macroPath || !programPath)
        return false;

    std::string macroResolved;
    std::string programResolved;
    if (!resolvePathCaseInsensitive(macroPath, macroResolved) ||
        !resolvePathCaseInsensitive(programPath, programResolved)) {
        compatLogFmt("shader common patch: source missing macro=%s program=%s",
                     macroPath, programPath);
        return false;
    }

    auto readFileText = [](const std::string& path, std::string& out) -> bool {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f)
            return false;

        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return false;
        }

        const long size = ftell(f);
        if (size < 0 || size > 1024 * 1024) {
            fclose(f);
            return false;
        }

        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return false;
        }

        out.resize((size_t)size);
        const size_t got = size ? fread(&out[0], 1, (size_t)size, f) : 0;
        fclose(f);
        if (got != (size_t)size) {
            out.clear();
            return false;
        }
        return true;
    };

    std::string macro;
    std::string program;
    if (!readFileText(macroResolved, macro) ||
        !readFileText(programResolved, program)) {
        compatLogFmt("shader common patch: read failed macro=%s program=%s",
                     macroResolved.c_str(), programResolved.c_str());
        return false;
    }

    const std::string declaration = "SubrScript 'CommonSubroutines'";
    const size_t macroPos = macro.find(declaration);
    if (macroPos == std::string::npos) {
        compatLogFmt("shader common patch: declaration missing in %s",
                     macroResolved.c_str());
        return false;
    }

    // Already patched: keep the operation idempotent across repeated startup
    // runs on the same extracted shader tree.
    size_t afterDecl = macroPos + declaration.size();
    while (afterDecl < macro.size() &&
           (macro[afterDecl] == ' ' || macro[afterDecl] == '\t' ||
            macro[afterDecl] == '\r' || macro[afterDecl] == '\n')) {
        ++afterDecl;
    }
    if (afterDecl < macro.size() && macro[afterDecl] == '{') {
        compatLogFmt("shader common patch: already present in %s",
                     macroResolved.c_str());
        return true;
    }

    const std::string sourceDecl = "DeclareCGScript 'CommonSubroutines'";
    const size_t sourcePos = program.find(sourceDecl);
    if (sourcePos == std::string::npos) {
        compatLogFmt("shader common patch: source declaration missing in %s",
                     programResolved.c_str());
        return false;
    }

    const size_t openBrace = program.find('{', sourcePos + sourceDecl.size());
    if (openBrace == std::string::npos) {
        compatLogFmt("shader common patch: source brace missing in %s",
                     programResolved.c_str());
        return false;
    }

    int depth = 0;
    size_t closeBrace = std::string::npos;
    for (size_t i = openBrace; i < program.size(); ++i) {
        if (program[i] == '{')
            ++depth;
        else if (program[i] == '}') {
            --depth;
            if (depth == 0) {
                closeBrace = i;
                break;
            }
        }
    }

    if (closeBrace == std::string::npos || depth != 0) {
        compatLogFmt("shader common patch: unbalanced source braces in %s",
                     programResolved.c_str());
        return false;
    }

    const std::string body = program.substr(openBrace + 1,
                                             closeBrace - openBrace - 1);

    size_t lineEnd = macro.find('\n', macroPos);
    if (lineEnd == std::string::npos)
        lineEnd = macro.size();
    else
        ++lineEnd;

    std::string replacement = declaration;
    replacement += "\n{\n";
    replacement += body;
    replacement += "\n}\n";

    macro.replace(macroPos, lineEnd - macroPos, replacement);

    FILE* out = fopen(macroResolved.c_str(), "wb");
    if (!out) {
        compatLogFmt("shader common patch: write failed %s",
                     macroResolved.c_str());
        return false;
    }

    const size_t written = fwrite(macro.data(), 1, macro.size(), out);
    fclose(out);

    if (written != macro.size()) {
        compatLogFmt("shader common patch: short write %s (%u/%u)",
                     macroResolved.c_str(), (unsigned)written,
                     (unsigned)macro.size());
        return false;
    }

    compatLogFmt("shader common patch: injected CommonSubroutines into %s from %s",
                 macroResolved.c_str(), programResolved.c_str());
    return true;
}

static bool isShaderEnumerationDirectory(const char* path) {
    if (!path || !*path)
        return false;

    std::string p = path;
    for (char& c : p) {
        if ((unsigned char)c == 92)
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    while (p.size() > 1 && p.back() == '/')
        p.pop_back();

    return p == "shaders/scripts" ||
           p == "shaders/hwscripts" ||
           p == "shaders/hwscripts/declarations";
}


static bool isShaderPathForDiag(const char* path) {
    if (!path || !*path)
        return false;

    std::string p = asciiLower(path);
    for (char& c : p)
        if ((unsigned char)c == 92)
            c = '/';

    return p.find("shaders/") != std::string::npos ||
           p.find("/shaders/") != std::string::npos ||
           p == "shaders" ||
           p == "/shaders";
}

static std::unordered_map<DIR*, std::string> g_readdirPaths;
static std::unordered_map<DIR*, unsigned> g_readdirCounts;

static struct dirent* stub_readdir(DIR* dir) {
    if (!dir)
        return nullptr;

    struct dirent* ent = ::readdir(dir);
    if (!ent)
        return nullptr;

    auto it = g_readdirPaths.find(dir);
    if (it != g_readdirPaths.end()) {
        unsigned& count = g_readdirCounts[dir];
        if (count < 32) {
            compatLogFmt("readdir[%u] %s -> %s",
                         count, it->second.c_str(), ent->d_name);
        }
        ++count;
    }
    return ent;
}

static int stub_closedir(DIR* dir) {
    if (!dir)
        return -1;
    g_readdirPaths.erase(dir);
    g_readdirCounts.erase(dir);
    return ::closedir(dir);
}

// Android/Bionic also exposes the 64-bit directory iterator as readdir64.
// On AArch64 the returned directory-entry layout is compatible with the
// newlib dirent used by this compatibility layer, so route it through the
// same native iterator while keeping a distinct diagnostic tag.
static struct dirent* stub_readdir64(DIR* dir) {
    if (!dir)
        return nullptr;

    struct dirent* ent = ::readdir(dir);
    if (!ent)
        return nullptr;

    auto it = g_readdirPaths.find(dir);
    if (it != g_readdirPaths.end()) {
        unsigned& count = g_readdirCounts[dir];
        if (count < 32) {
            compatLogFmt("readdir64[%u] %s -> %s",
                         count, it->second.c_str(), ent->d_name);
        }
        ++count;
    }
    return ent;
}

static DIR* stub_opendir(const char* path) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    if (path && ioPathStorage != path)
        compatLogFmt("path NORMALIZE: opendir %s -> %s", path, ioPathStorage.c_str());

    DIR* d = opendir(ioPath);
    if (d) {
        std::string low = ioPath ? asciiLower(ioPath) : std::string();
        if (low.find("/fcdata") != std::string::npos ||
            low == "fcdata" ||
            low.find("/localized") != std::string::npos ||
            isShaderPathForDiag(ioPath)) {
            g_readdirPaths[d] = ioPath;
            g_readdirCounts[d] = 0;
        }
        if (isShaderPathForDiag(ioPath))
            compatLogFmt("opendir SHADER OK: requested=%s", ioPath);
        return d;
    }

    std::string resolved;
    if (resolvePathCaseInsensitive(ioPath, resolved)) {
        d = opendir(resolved.c_str());
        if (d) {
            if (ioPath) {
                std::string low = asciiLower(ioPath);
                if (low.find("/fcdata") != std::string::npos ||
                    low == "fcdata" ||
                    low.find("/localized") != std::string::npos ||
                    isShaderPathForDiag(ioPath)) {
                    g_readdirPaths[d] = resolved;
                    g_readdirCounts[d] = 0;
                }
            }
            if (isShaderPathForDiag(ioPath))
                compatLogFmt("opendir SHADER CASEFIX: requested=%s resolved=%s",
                             ioPath ? ioPath : "?", resolved.c_str());
            compatLogFmt("opendir CASEFIX: %s -> %s",
                         ioPath ? ioPath : "?", resolved.c_str());
            return d;
        }
    }

    if (ioPath && tryMaterializePakDirectory(ioPath)) {
        d = opendir(ioPath);
        if (!d) {
            std::string dirResolved;
            if (resolvePathCaseInsensitive(ioPath, dirResolved))
                d = opendir(dirResolved.c_str());
        }
        if (d) {
            if (isShaderPathForDiag(ioPath))
                compatLogFmt("opendir SHADER PAK: %s", ioPath);
            compatLogFmt("opendir PAK: %s", ioPath);
            return d;
        }
    }

    if (isShaderPathForDiag(ioPath))
        compatLogFmt("opendir SHADER FAIL: %s", ioPath ? ioPath : "?");
    compatLogFmt("opendir FAIL: %s", ioPath ? ioPath : "?");
    return nullptr;
}

struct NearFindData64 {
    uint32_t attrib;
    uint32_t reserved;
    int64_t  time_create;
    int64_t  time_access;
    int64_t  time_write;
    int64_t  size;
    char     name[256];
};

static_assert(offsetof(NearFindData64, time_create) == 8, "NearFindData64 layout");
static_assert(offsetof(NearFindData64, name) == 40, "NearFindData64 layout");
static_assert(sizeof(NearFindData64) == 296, "NearFindData64 layout");

struct NearFind64State {
    DIR* dir;
    std::string directory;
    std::string pattern;
};

static std::string findDirPart(const char* pattern, std::string& filePattern) {
    std::string p = pattern ? pattern : "";
    for (char& c : p) {
        if ((unsigned char)c == 92)
            c = '/';
    }

    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        filePattern = p.empty() ? "*" : p;
        return ".";
    }

    filePattern = p.substr(slash + 1);
    std::string dir = p.substr(0, slash);
    if (dir.empty())
        dir = "/";
    return dir;
}

static bool findPatternMatches(const std::string& pattern, const char* name) {
    const std::string pat = asciiLower(pattern.empty() ? "*" : pattern);
    const std::string item = asciiLower(name ? name : "");

    // CryEngine/CryPak uses the Windows-style "*.*" enumeration pattern for
    // recursive directory scans. POSIX fnmatch() treats "*.*" literally and
    // therefore does NOT match directory names such as "Declarations" that
    // contain no dot. That prevents mfLoadSubdir() from descending into
    // Shaders/HWScripts/Declarations and also makes Shaders/Scripts appear empty.
    // Match "*.*" as Windows does: every visible name.
    if (pat == "*.*")
        return true;

    return fnmatch(pat.c_str(), item.c_str(), 0) == 0;
}

static bool fillFindData64(NearFindData64* out,
                           const std::string& directory,
                           const char* name) {
    if (!out || !name)
        return false;

    std::string full = directory;
    if (full.empty() || full == ".")
        full = name;
    else if (full == "/")
        full += name;
    else {
        full += "/";
        full += name;
    }

    struct stat st = {};
    if (stat(full.c_str(), &st) != 0)
        return false;

    memset(out, 0, sizeof(*out));
    if (S_ISDIR(st.st_mode))
        out->attrib |= 0x10; // _A_SUBDIR
    if (!(st.st_mode & S_IWUSR))
        out->attrib |= 0x01; // _A_RDONLY

    out->time_create = (int64_t)st.st_mtime;
    out->time_access = (int64_t)st.st_atime;
    out->time_write  = (int64_t)st.st_mtime;
    out->size        = (int64_t)st.st_size;
    const size_t name_len = std::strlen(name);
    const size_t name_copy = name_len < sizeof(out->name) - 1 ? name_len : sizeof(out->name) - 1;
    std::memcpy(out->name, name, name_copy);
    out->name[name_copy] = '\0';
    return true;
}

static bool findNextMatch(NearFind64State* state, NearFindData64* out) {
    if (!state || !state->dir || !out)
        return false;

    while (struct dirent* ent = readdir(state->dir)) {
        if (!findPatternMatches(state->pattern, ent->d_name))
            continue;
        if (!fillFindData64(out, state->directory, ent->d_name))
            continue;
        return true;
    }

    return false;
}

static intptr_t stub_findfirst64(const char* pattern, NearFindData64* out) {
    if (!pattern || !out) {
        errno = EINVAL;
        return -1;
    }

    std::string filePattern;
    std::string directory = findDirPart(pattern, filePattern);

    if (isShaderPathForDiag(pattern))
        compatLogFmt("findfirst64 SHADER REQUEST: pattern=%s dir=%s filePattern=%s",
                     pattern, directory.c_str(), filePattern.c_str());

    // Do NOT bypass stub_opendir() here. CryPak commonly asks for a lowercase
    // shader directory on the case-sensitive Switch filesystem. stub_opendir()
    // is the point where we recover the real directory spelling and materialize
    // an existing-but-empty shader directory from FCData/*.pak.
    DIR* d = stub_opendir(directory.c_str());
    std::string resolvedDirectory = directory;
    if (!d && resolvePathCaseInsensitive(directory.c_str(), resolvedDirectory))
        d = stub_opendir(resolvedDirectory.c_str());

    if (!d) {
        compatLogFmt("findfirst64 FAIL: %s (dir=%s pattern=%s)",
                     pattern, directory.c_str(), filePattern.c_str());
        errno = ENOENT;
        return -1;
    }

    NearFind64State* state = new NearFind64State;
    state->dir = d;
    state->directory = resolvedDirectory;
    state->pattern = filePattern;

    compatLogFmt("findfirst64: %s -> dir=%s pattern=%s",
                 pattern, state->directory.c_str(), state->pattern.c_str());

    if (!findNextMatch(state, out)) {
        // The directory can exist before the PAK tree has been materialized.
        // Give the shader directories one final materialization pass instead
        // of treating an empty enumeration as "not found".
        const bool shaderDir = isShaderEnumerationDirectory(directory.c_str());
        closedir(state->dir);

        if (shaderDir && tryMaterializePakDirectory(directory.c_str())) {
            resolvedDirectory = directory;
            d = stub_opendir(directory.c_str());
            if (!d && resolvePathCaseInsensitive(directory.c_str(), resolvedDirectory))
                d = stub_opendir(resolvedDirectory.c_str());

            if (d) {
                state->dir = d;
                state->directory = resolvedDirectory;

                if (findNextMatch(state, out)) {
                    compatLogFmt("findfirst64 MATCH AFTER PAK: %s -> %s",
                                 pattern, out->name);
                    return (intptr_t)state;
                }

                closedir(state->dir);
            }
        }

        delete state;
        errno = ENOENT;
        compatLogFmt("findfirst64: no match for %s", pattern);
        return -1;
    }

    compatLogFmt("findfirst64 MATCH: %s -> %s",
                 pattern, out->name);
    return (intptr_t)state;
}

static int stub_findnext64(intptr_t handle, NearFindData64* out) {
    NearFind64State* state = reinterpret_cast<NearFind64State*>(handle);
    if (!state || !out) {
        errno = EBADF;
        return -1;
    }

    if (findNextMatch(state, out))
        return 0;

    compatLogFmt("findnext64: end dir=%s pattern=%s",
                 state->directory.c_str(), state->pattern.c_str());
    return -1;
}

static int stub_findclose64(intptr_t handle) {
    NearFind64State* state = reinterpret_cast<NearFind64State*>(handle);
    if (!state)
        return -1;

    if (state->dir)
        closedir(state->dir);
    delete state;
    return 0;
}

// The original Android game can ask the C runtime to remove configuration
// files while resetting profiles. Never let a guest-side cleanup operation
// delete the root configs that control the Switch launch.
//
// Profile-specific files such as Profiles/Player/Maximk_system.cfg are NOT
// protected because their basename is different.
static bool isProtectedRootConfigPath(const char* path) {
    if (!path || !*path)
        return false;

    const char* base = strrchr(path, '/');
    const char* back = strrchr(path, '\\');
    if (back && (!base || back > base))
        base = back;
    if (base)
        ++base;
    else
        base = path;

    return strcasecmp(base, "system.cfg") == 0 ||
           strcasecmp(base, "game.cfg") == 0;
}

static int stub_remove(const char* path) {
    if (isProtectedRootConfigPath(path)) {
        compatLogFmt("remove BLOCKED: protected config %s", path);
        errno = EROFS;
        return -1;
    }
    return ::remove(path);
}

static int stub_rename(const char* old_path, const char* new_path) {
    if (isProtectedRootConfigPath(old_path) ||
        isProtectedRootConfigPath(new_path)) {
        compatLogFmt("rename BLOCKED: protected config %s -> %s",
                     old_path ? old_path : "?",
                     new_path ? new_path : "?");
        errno = EROFS;
        return -1;
    }
    return ::rename(old_path, new_path);
}

static int stub_unlinkat(int, const char* path, int) {
    if (isProtectedRootConfigPath(path)) {
        compatLogFmt("unlinkat BLOCKED: protected config %s", path);
        errno = EROFS;
        return -1;
    }
    return ::remove(path);
}
static int stub_utimensat(int, const char*, const void*, int) { return 0; }
static int stub_fchmodat(int, const char*, mode_t, int) { return 0; }

static void stub_assert2(const char* f, int l, const char* fn, const char* m) {
    compatLogFmt("game assert: %s:%d %s: %s", f ? f : "?", l, fn ? fn : "?", m ? m : "?");
    abort();
}
static void stub_android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    char b[512]; va_list va; va_start(va, fmt);
    vsnprintf(b, sizeof(b), fmt ? fmt : "", va); va_end(va);
    compatLogFmt("android_log_assert [%s] %s: %s", tag ? tag : "?", cond ? cond : "?", b);
    abort();
}
static void stub_FD_CLR_chk(int, void*, size_t) {}

// newlib has no fdopendir — its DIR is not built from a descriptor. Every
// caller in this game reaches it through the *at() family it also cannot use,
// so failing cleanly with EBADF is both honest and what those callers already
// handle.
static void* stub_fdopendir(int) { errno = EBADF; return nullptr; }

// newlib has no sigsetjmp/siglongjmp — it has no signal masks to save. The
// plain pair is behaviourally identical here, and the saved-mask argument is
// simply ignored rather than pretending a mask was restored.
static int  stub_sigsetjmp(jmp_buf env, int) { return setjmp(env); }
static void stub_siglongjmp(jmp_buf env, int v) { longjmp(env, v); }

// GL_OES_mapbuffer. Desktop-style glMapBuffer exists in our GLES headers under
// the core name; the OES entry points are the same call under the extension's
// spelling, which is what a 2012-era cocos2d-x build asks for.
static void* stub_glMapBufferOES(unsigned target, unsigned access) {
    return glMapBufferRange(target, 0, 0, access);
}
static unsigned char stub_glUnmapBufferOES(unsigned target) {
    return (unsigned char)glUnmapBuffer(target);
}

// ARM exception-index lookup. Only reached when C++ unwinds through 32-bit
// frames; returning null means "no unwind data here", which turns an unwind
// into a terminate rather than a jump through a wild pointer.
static const void* stub_dl_unwind_find_exidx(const void*, int* count) {
    if (count) *count = 0;
    return nullptr;
}


// ─── Remaining 32-bit imports ───────────────────────────────────────────────
static int   stub_AAsset_openFileDescriptor(void*, off_t* start, off_t* len) {
    // Assets live inside the APK, and we extract rather than mmap it, so there
    // is no descriptor-and-range to hand back. Callers treat -1 as "read it the
    // normal way", which is the path that already works.
    if (start) *start = 0;
    if (len)   *len   = 0;
    return -1;
}

// Bionic's _FORTIFY_SOURCE variants. The extra argument is the compiler's idea
// of the destination size; honouring it is the whole point, so these bound the
// copy rather than forwarding and ignoring it.
static char* stub_fgets_chk(char* d, int n, FILE* f, size_t dsz) {
    if ((size_t)n > dsz) n = (int)dsz;
    return fgets(d, n, f);
}
static char* stub_strncpy_chk(char* d, const char* s, size_t n, size_t dsz) {
    if (n > dsz) n = dsz;
    return strncpy(d, s, n);
}
static char* stub_strrchr_chk(const char* s, int c, size_t) { return (char*)strrchr(s, c); }

static const char* g_progname = "viridite";
static const char* stub_getprogname(void) { return g_progname; }

static void     stub_arc4random_buf(void* b, size_t n) { if (b && n) randomGet(b, n); }
static uint32_t stub_arc4random_uniform(uint32_t upper) {
    if (upper < 2) return 0;
    uint32_t r = 0; randomGet(&r, sizeof(r));
    return r % upper;
}

static int stub_asprintf(char** out, const char* fmt, ...) {
    if (!out) return -1;
    va_list a, b; va_start(a, fmt); va_copy(b, a);
    int n = vsnprintf(nullptr, 0, fmt, a); va_end(a);
    if (n < 0) { va_end(b); return -1; }
    *out = (char*)malloc((size_t)n + 1);
    if (!*out) { va_end(b); return -1; }
    vsnprintf(*out, (size_t)n + 1, fmt, b); va_end(b);
    return n;
}
static void stub_vsyslog(int, const char* fmt, va_list a) {
    char b[512]; vsnprintf(b, sizeof(b), fmt ? fmt : "", a); compatLog(b);
}
static char* stub_strndup(const char* s, size_t n) {
    if (!s) return nullptr;
    size_t l = 0; while (l < n && s[l]) l++;
    char* r = (char*)malloc(l + 1);
    if (r) { memcpy(r, s, l); r[l] = 0; }
    return r;
}
static void stub_setbuf(FILE* f, char* b) { setvbuf(f, b, b ? _IOFBF : _IONBF, BUFSIZ); }
static int  stub_pause(void) { errno = EINTR; return -1; }   // no signals to wait for

// ARM32 cache maintenance. The interpreter fetches through the same memory it
// writes, so there is no instruction cache to keep coherent — but a game that
// self-modifies expects the call to exist and to succeed.
static int stub_cacheflush(long, long, long) { return 0; }

// epoll. Nothing here polls descriptors that would ever become ready, so these
// fail rather than pretending to watch something.
static int stub_epoll_create(int)            { errno = ENOSYS; return -1; }
static int stub_epoll_ctl(int,int,int,void*) { errno = ENOSYS; return -1; }
static int stub_epoll_wait(int,void*,int,int){ errno = ENOSYS; return -1; }
static int stub_inotify_rm_watch(int,int)    { errno = ENOSYS; return -1; }
static int stub_tgkill(int,int,int)          { errno = ENOSYS; return -1; }

// Process creation. There is one process here and no way to make another, so
// these fail rather than appearing to fork — the crash-reporter libraries that
// call them handle the failure by not reporting, which is the outcome we want
// anyway.
static int   stub_execv (const char*, char* const[])             { errno = ENOSYS; return -1; }
static int   stub_execvpe(const char*, char* const[], char* const[]) { errno = ENOSYS; return -1; }
static pid_t stub_getppid(void)                                   { return 1; }
static pid_t stub_wait4(pid_t, int* st, int, void*) { if (st) *st = 0; errno = ECHILD; return -1; }
static long  stub_lrint(double x) { return (long)(x < 0 ? x - 0.5 : x + 0.5); }

static time_t stub_timegm(struct tm* t) {
    // mktime applies the local timezone; UTC here means undoing that, and the
    // Switch runs UTC anyway, so the correction is normally zero.
    if (!t) return (time_t)-1;
    time_t local = mktime(t);
    if (local == (time_t)-1) return local;
    struct tm probe = {};
    time_t zero = 0;
    gmtime_r(&zero, &probe);
    return local - mktime(&probe);
}

static intmax_t  stub_strtoimax (const char* s, char** e, int b) { return strtoll(s, e, b); }
static uintmax_t stub_strtoumax (const char* s, char** e, int b) { return strtoull(s, e, b); }

// ─── Android-specific ────────────────────────────────────────────────────────
static void stub_android_abort_msg(const char* msg) {
    compatLogFmt("android_abort_message: %s", msg ? msg : "");
}

// ─── syslog ──────────────────────────────────────────────────────────────────
static void stub_openlog(const char* id, int, int) {
    compatLogFmt("openlog: %s", id ? id : "");
}
static void stub_closelog(void) {}
static void stub_syslog(int, const char* fmt, ...) {
    char buf[512]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    compatLog(buf);
}

// ─── Android UTF-8 multibyte compatibility ───────────────────────────────────
// CryEngine's Linux path uses mbstowcs() when loading LANGUAGES/*.xml. The
// Android ARM64 build leaves mbstowcs unresolved, so the ELF loader otherwise
// poisons the PLT slot and jumps to 0xBAD0BAD0BAD00000 on the first localized
// string. Android's locale path is UTF-8; decode it explicitly instead of
// relying on the process locale being configured on Switch.
static size_t stub_mbstowcs(wchar_t* dst, const char* src, size_t len) {
    if (!src) {
        errno = EINVAL;
        return (size_t)-1;
    }
    if (!dst || len == 0)
        return 0;

    size_t out = 0;
    const unsigned char* p = (const unsigned char*)src;
    while (*p && out < len) {
        uint32_t cp = 0;
        size_t need = 0;
        unsigned char c = *p++;
        if (c < 0x80) {
            cp = c;
            need = 0;
        } else if (c >= 0xC2 && c <= 0xDF) {
            cp = c & 0x1F;
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            cp = c & 0x0F;
            need = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            cp = c & 0x07;
            need = 3;
        } else {
            errno = EILSEQ;
            return (size_t)-1;
        }

        for (size_t i = 0; i < need; ++i) {
            unsigned char d = p[i];
            if ((d & 0xC0) != 0x80) {
                errno = EILSEQ;
                return (size_t)-1;
            }
            cp = (cp << 6) | (d & 0x3F);
        }

        // Reject overlong encodings, UTF-16 surrogate code points and values
        // outside Unicode. wchar_t on devkitA64 is 32-bit, matching the
        // wchar_t representation expected by this CryEngine path.
        if ((need == 1 && cp < 0x80) ||
            (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            errno = EILSEQ;
            return (size_t)-1;
        }

        p += need;
        dst[out++] = (wchar_t)cp;
    }
    if (out < len)
        dst[out] = L'\0';
    return out;
}

// ─── Locale POSIX extensions (stub — newlib may lack these) ──────────────────
typedef void* bnx_locale_t;
static bnx_locale_t stub_newlocale(int, const char*, bnx_locale_t) { return (bnx_locale_t)1; }
static void stub_freelocale(bnx_locale_t) {}
static bnx_locale_t stub_uselocale(bnx_locale_t) { return (bnx_locale_t)1; }
static int stub_mb_cur_max(void) { return 1; }

// ─── strtod/strtoll locale variants ─────────────────────────────────────────
static long long        stub_strtoll_l(const char* s, char** e, int b, void*) { return strtoll(s, e, b); }
static unsigned long long stub_strtoull_l(const char* s, char** e, int b, void*) { return strtoull(s, e, b); }
static long double      stub_strtold_l(const char* s, char** e, void*)        { return strtold(s, e); }

// ─── Bionic fortified wrappers ────────────────────────────────────────────────
static size_t  chk_strlen(const char* s, size_t)               { return strlen(s); }
static void*   chk_memcpy(void* d, const void* s, size_t n, size_t)   { return memcpy(d,s,n); }
static void*   chk_memmove(void* d, const void* s, size_t n, size_t)  { return memmove(d,s,n); }
static char*   chk_strcat(char* d, const char* s, size_t)     { return strcat(d,s); }
static char*   chk_strncat(char* d, const char* s, size_t n, size_t) { return strncat(d,s,n); }
static char*   chk_strcpy(char* d, const char* s, size_t)     { return strcpy(d,s); }
static char*   chk_strncpy(char* d, const char* s, size_t n, size_t, size_t) { return strncpy(d,s,n); }
static int     chk_vsprintf(char* d, int, size_t, const char* f, va_list v)  { return vsprintf(d,f,v); }
static int     chk_vsnprintf(char* d, size_t n, int, size_t, const char* f, va_list v) { return vsnprintf(d,n,f,v); }
static ssize_t chk_read(int fd, void* b, size_t n, size_t)    { return sh_read(fd,b,n); }
static int     chk_open2(const char* p, int fl) {
    const std::string ioPathStorage = normalizeSwitchFsPath(p);
    const char* ioPath = p ? ioPathStorage.c_str() : nullptr;
    int vfd = devUrandomOpen(ioPath);
    if (vfd >= 0) return vfd;
    return open(ioPath, fl, 0666);
}

// ─── pthread_mutexattr extras ─────────────────────────────────────────────────
static int pt_mattr_init(void* a)       { if (a) memset(a, 0, 4); return 0; }
static int pt_mattr_destroy(void*)      { return 0; }
static int pt_mattr_settype(void*, int) { return 0; }

// ─── prctl (Linux process/thread control — crash reporters use PR_SET_NAME) ───
static int stub_prctl(int, unsigned long, unsigned long, unsigned long, unsigned long) {
    return 0;
}

// ─── gettid (Linux syscall — thread ID, not in newlib) ──────────────────────
// This returned a constant 1, which made every thread in the process look like
// the same thread to anything that keys on the tid — and il2cpp registers its
// threads that way, so its per-thread bookkeeping all collided on one slot.
// Worse, `gettid() == getpid()` is the standard "am I the main thread?" test,
// and with both pinned to 1 every thread answered yes.
//
// The main thread still returns 1 so that test stays true where it should be;
// every other thread gets a stable id folded out of its TLS block address,
// which is unique per thread, needs no allocation and no lock.
static pid_t stub_gettid(void) {
    if (threadGetCurHandle() == envGetMainThreadHandle()) return 1;
    uintptr_t t = (uintptr_t)armGetTls();
    return (pid_t)(((t >> 8) & 0x3FFFFF) + 1000);
}

// ─── getpid / getuid / getgid ────────────────────────────────────────────────
// newlib has getpid() but shimming it makes it available to the game's .so
static pid_t stub_getpid(void)  { return 1; }
static uid_t stub_getuid(void)  { return 0; }
static gid_t stub_getgid(void)  { return 0; }

// ─── Signal stubs (crash reporters install SIGSEGV/SIGBUS handlers) ─────────
// Return success without actually doing anything — Switch uses its own fault handler
struct BnxSigaction { void* handler; unsigned long flags; void* restorer; uint64_t mask; };
static int stub_sigaction(int, const BnxSigaction*, BnxSigaction*) { return 0; }
static int stub_sigemptyset(uint64_t* s) { if (s) *s = 0; return 0; }
static int stub_sigfillset(uint64_t* s)  { if (s) *s = ~(uint64_t)0; return 0; }
static int stub_sigaddset(uint64_t* s, int sig) {
    if (s && sig > 0 && sig < 64) *s |= (1ULL << (sig - 1));
    return 0;
}
static int stub_sigdelset(uint64_t* s, int sig) {
    if (s && sig > 0 && sig < 64) *s &= ~(1ULL << (sig - 1));
    return 0;
}
static int stub_sigismember(const uint64_t* s, int sig) {
    if (!s || sig <= 0 || sig >= 64) return 0;
    return (*s >> (sig - 1)) & 1;
}
static int stub_kill(pid_t, int)    { return 0; }
static int stub_raise(int)          { return 0; }  // prevent crash reporter self-test
static int stub_pthread_kill(void*, int) { return 0; }
static int stub_sigprocmask(int, const uint64_t*, uint64_t*) { return 0; }
static int stub_pthread_sigmask(int, const uint64_t*, uint64_t*) { return 0; }

// ─── mprotect stub (crash reporters may set guard-page permissions) ──────────
static int stub_mprotect(void*, size_t, int) { return 0; }

// ─── pipe / dup / dup2 (crash reporter IPC) ─────────────────────────────────
static int stub_pipe(int fd[2])          { (void)fd; errno = ENOTSUP; return -1; }
static int stub_dup(int)                 { errno = ENOTSUP; return -1; }
static int stub_dup2(int, int)           { errno = ENOTSUP; return -1; }

// ─── ioctl stub ──────────────────────────────────────────────────────────────
static int stub_ioctl(int, unsigned long, void*) { errno = ENOTSUP; return -1; }

// ─── access stub (file existence check) ──────────────────────────────────────
static int stub_access(const char* path, int mode) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = ioPathStorage.c_str();

    if (::access(ioPath, mode) == 0)
        return 0;

    std::string resolved;
    if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath)
        return ::access(resolved.c_str(), mode);

    errno = ENOENT;
    return -1;
}

// ─── chmod / fchmod / lstat ──────────────────────────────────────────────────
static int stub_chmod(const char*, mode_t)   { return 0; }
static int stub_fchmod(int, mode_t)          { return 0; }
static int stub_lstat(const char* p, struct stat* s) {
    if (!p || !s) {
        errno = EINVAL;
        return -1;
    }

    const std::string ioPathStorage = normalizeSwitchFsPath(p);
    const char* ioPath = ioPathStorage.c_str();

    if (::lstat(ioPath, s) == 0)
        return 0;
    std::string resolved;
    if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath)
        return ::lstat(resolved.c_str(), s);
    return -1;
}

// ─── pthread_setname_np / pthread_getname_np (thread naming, GNU ext) ────────
static int stub_pthread_setname_np(void*, const char*) { return 0; }
static int stub_pthread_getname_np(void*, char* buf, size_t sz) {
    if (buf && sz > 0) buf[0] = '\0';
    return 0;
}
static int stub_pthread_attr_setstack(void*, void*, size_t) { return 0; }
static int stub_pthread_attr_getstack(const void*, void** s, size_t* z) {
    if (s) *s = nullptr;
    if (z) *z = 65536;
    return 0;
}
static int stub_pthread_attr_setschedpolicy(void*, int) { return 0; }
static int stub_pthread_attr_setschedparam(void*, const void*) { return 0; }
static int stub_pthread_attr_getschedparam(const void*, void*) { return 0; }
static int stub_pthread_barrier_init(void*, const void*, unsigned) { return 0; }
static int stub_pthread_barrier_wait(void*) { return 0; }
static int stub_pthread_barrier_destroy(void*) { return 0; }
// OpenGL extension entry points that are not declared by the Switch Mesa
// compatibility headers. Map ABI-compatible variants to core functions and
// keep unsupported NVIDIA/ATI fence operations harmless.
static void shim_glActiveStencilFaceEXT(GLenum) {}
static void shim_glBindBufferARB(GLenum target, GLuint buffer) { glBindBuffer(target, buffer); }
static void shim_glBufferDataARB(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    glBufferData(target, size, data, usage);
}
static void shim_glGenBuffersARB(GLsizei n, GLuint* buffers) {
    glGenBuffers(n, buffers);
}
static void shim_glBufferSubDataARB(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    glBufferSubData(target, offset, size, data);
}
static void shim_glColorTableEXT(GLenum target, GLenum internalformat, GLsizei width,
                                 GLenum format, GLenum type, const void* table) {
    glColorTable(target, internalformat, width, format, type, table);
}
static void shim_glCompressedTexImage2DARB(GLenum target, GLint level, GLenum internalformat,
                                           GLsizei width, GLsizei height, GLint border,
                                           GLsizei imageSize, const void* data) {
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}
static void shim_glCompressedTexSubImage2DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                              GLsizei width, GLsizei height, GLenum format,
                                              GLsizei imageSize, const void* data) {
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}
static void shim_glFinishFenceNV(GLuint) {}
static void shim_glGenFencesNV(GLsizei n, GLuint* fences) {
    if (fences && n > 0)
        memset(fences, 0, sizeof(GLuint) * (size_t)n);
}
static void shim_glGetCompressedTexImageARB(GLenum target, GLint level, void* img) {
    glGetCompressedTexImage(target, level, img);
}
static void shim_glSetFenceNV(GLuint, GLenum) {}
static GLboolean shim_glTestFenceNV(GLuint) { return GL_TRUE; }
static void shim_glStencilFuncSeparateATI(GLenum face, GLenum func, GLint ref, GLuint mask) {
    glStencilFuncSeparate(face, func, ref, mask);
}
static void shim_glStencilOpSeparateATI(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
    glStencilOpSeparate(face, sfail, dpfail, dppass);
}
static void shim_glTexImage3DEXT(GLenum target, GLint level, GLenum internalformat,
                                 GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                 GLenum format, GLenum type, const void* pixels) {
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

// ARB vertex/fragment program entry points used by Far Cry's OpenGL renderer.
//
// The Switch Mesa headers intentionally expose the modern/core API, but do not
// declare the legacy ARB program entry points as direct C functions. Resolve
// the extension functions from the active EGL context instead. This also keeps
// the compatibility layer independent from Mesa's private symbol exports.
template <typename T>
static T resolveGLProc(const char* name) {
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

using PFN_glBindProgramARB = void (*)(GLenum, GLuint);
using PFN_glDeleteProgramsARB = void (*)(GLsizei, const GLuint*);
using PFN_glGenProgramsARB = void (*)(GLsizei, GLuint*);
using PFN_glIsProgramARB = GLboolean (*)(GLuint);
using PFN_glProgramStringARB = void (*)(GLenum, GLenum, GLsizei, const void*);
using PFN_glProgramEnvParameter4fARB = void (*)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glProgramEnvParameter4fvARB = void (*)(GLenum, GLuint, const GLfloat*);
using PFN_glProgramEnvParameter4dARB = void (*)(GLenum, GLuint, GLdouble, GLdouble, GLdouble, GLdouble);
using PFN_glProgramEnvParameter4dvARB = void (*)(GLenum, GLuint, const GLdouble*);
using PFN_glProgramLocalParameter4fARB = void (*)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glProgramLocalParameter4fvARB = void (*)(GLenum, GLuint, const GLfloat*);
using PFN_glProgramLocalParameter4dARB = void (*)(GLenum, GLuint, GLdouble, GLdouble, GLdouble, GLdouble);
using PFN_glProgramLocalParameter4dvARB = void (*)(GLenum, GLuint, const GLdouble*);
using PFN_glGetProgramivARB = void (*)(GLenum, GLenum, GLint*);
using PFN_glGetProgramStringARB = void (*)(GLenum, GLenum, void*);
using PFN_glGetProgramEnvParameterfvARB = void (*)(GLenum, GLuint, GLfloat*);
using PFN_glGetProgramEnvParameterdvARB = void (*)(GLenum, GLuint, GLdouble*);
using PFN_glGetProgramLocalParameterfvARB = void (*)(GLenum, GLuint, GLfloat*);
using PFN_glGetProgramLocalParameterdvARB = void (*)(GLenum, GLuint, GLdouble*);

static void shim_glBindProgramARB(GLenum target, GLuint program) {
    static PFN_glBindProgramARB fn = resolveGLProc<PFN_glBindProgramARB>("glBindProgramARB");
    if (fn) fn(target, program);
}
static void shim_glDeleteProgramsARB(GLsizei n, const GLuint* programs) {
    static PFN_glDeleteProgramsARB fn = resolveGLProc<PFN_glDeleteProgramsARB>("glDeleteProgramsARB");
    if (fn) fn(n, programs);
}
static void shim_glGenProgramsARB(GLsizei n, GLuint* programs) {
    static PFN_glGenProgramsARB fn = resolveGLProc<PFN_glGenProgramsARB>("glGenProgramsARB");
    if (fn) fn(n, programs);
    else if (programs && n > 0) memset(programs, 0, sizeof(GLuint) * (size_t)n);
}
static GLboolean shim_glIsProgramARB(GLuint program) {
    static PFN_glIsProgramARB fn = resolveGLProc<PFN_glIsProgramARB>("glIsProgramARB");
    return fn ? fn(program) : GL_FALSE;
}
static void shim_glProgramStringARB(GLenum target, GLenum format,
                                    GLsizei len, const void* string) {
    static PFN_glProgramStringARB fn = resolveGLProc<PFN_glProgramStringARB>("glProgramStringARB");
    if (fn) fn(target, format, len, string);
}
static void shim_glProgramEnvParameter4fARB(GLenum target, GLuint index,
                                            GLfloat x, GLfloat y,
                                            GLfloat z, GLfloat w) {
    static PFN_glProgramEnvParameter4fARB fn =
        resolveGLProc<PFN_glProgramEnvParameter4fARB>("glProgramEnvParameter4fARB");
    if (fn) fn(target, index, x, y, z, w);
}
static void shim_glProgramEnvParameter4fvARB(GLenum target, GLuint index,
                                             const GLfloat* params) {
    static PFN_glProgramEnvParameter4fvARB fn =
        resolveGLProc<PFN_glProgramEnvParameter4fvARB>("glProgramEnvParameter4fvARB");
    if (fn) fn(target, index, params);
}
static void shim_glProgramEnvParameter4dARB(GLenum target, GLuint index,
                                            GLdouble x, GLdouble y,
                                            GLdouble z, GLdouble w) {
    static PFN_glProgramEnvParameter4dARB fn =
        resolveGLProc<PFN_glProgramEnvParameter4dARB>("glProgramEnvParameter4dARB");
    if (fn) fn(target, index, x, y, z, w);
}
static void shim_glProgramEnvParameter4dvARB(GLenum target, GLuint index,
                                             const GLdouble* params) {
    static PFN_glProgramEnvParameter4dvARB fn =
        resolveGLProc<PFN_glProgramEnvParameter4dvARB>("glProgramEnvParameter4dvARB");
    if (fn) fn(target, index, params);
}
static void shim_glProgramLocalParameter4fARB(GLenum target, GLuint index,
                                              GLfloat x, GLfloat y,
                                              GLfloat z, GLfloat w) {
    static PFN_glProgramLocalParameter4fARB fn =
        resolveGLProc<PFN_glProgramLocalParameter4fARB>("glProgramLocalParameter4fARB");
    if (fn) fn(target, index, x, y, z, w);
}
static void shim_glProgramLocalParameter4fvARB(GLenum target, GLuint index,
                                               const GLfloat* params) {
    static PFN_glProgramLocalParameter4fvARB fn =
        resolveGLProc<PFN_glProgramLocalParameter4fvARB>("glProgramLocalParameter4fvARB");
    if (fn) fn(target, index, params);
}
static void shim_glProgramLocalParameter4dARB(GLenum target, GLuint index,
                                              GLdouble x, GLdouble y,
                                              GLdouble z, GLdouble w) {
    static PFN_glProgramLocalParameter4dARB fn =
        resolveGLProc<PFN_glProgramLocalParameter4dARB>("glProgramLocalParameter4dARB");
    if (fn) fn(target, index, x, y, z, w);
}
static void shim_glProgramLocalParameter4dvARB(GLenum target, GLuint index,
                                               const GLdouble* params) {
    static PFN_glProgramLocalParameter4dvARB fn =
        resolveGLProc<PFN_glProgramLocalParameter4dvARB>("glProgramLocalParameter4dvARB");
    if (fn) fn(target, index, params);
}
static void shim_glGetProgramivARB(GLenum target, GLenum pname, GLint* params) {
    static PFN_glGetProgramivARB fn =
        resolveGLProc<PFN_glGetProgramivARB>("glGetProgramivARB");
    if (fn) fn(target, pname, params);
}
static void shim_glGetProgramStringARB(GLenum target, GLenum pname, void* string) {
    static PFN_glGetProgramStringARB fn =
        resolveGLProc<PFN_glGetProgramStringARB>("glGetProgramStringARB");
    if (fn) fn(target, pname, string);
}
static void shim_glGetProgramEnvParameterfvARB(GLenum target, GLuint index,
                                               GLfloat* params) {
    static PFN_glGetProgramEnvParameterfvARB fn =
        resolveGLProc<PFN_glGetProgramEnvParameterfvARB>("glGetProgramEnvParameterfvARB");
    if (fn) fn(target, index, params);
}
static void shim_glGetProgramEnvParameterdvARB(GLenum target, GLuint index,
                                               GLdouble* params) {
    static PFN_glGetProgramEnvParameterdvARB fn =
        resolveGLProc<PFN_glGetProgramEnvParameterdvARB>("glGetProgramEnvParameterdvARB");
    if (fn) fn(target, index, params);
}
static void shim_glGetProgramLocalParameterfvARB(GLenum target, GLuint index,
                                                 GLfloat* params) {
    static PFN_glGetProgramLocalParameterfvARB fn =
        resolveGLProc<PFN_glGetProgramLocalParameterfvARB>("glGetProgramLocalParameterfvARB");
    if (fn) fn(target, index, params);
}
static void shim_glGetProgramLocalParameterdvARB(GLenum target, GLuint index,
                                                 GLdouble* params) {
    static PFN_glGetProgramLocalParameterdvARB fn =
        resolveGLProc<PFN_glGetProgramLocalParameterdvARB>("glGetProgramLocalParameterdvARB");
    if (fn) fn(target, index, params);
}

// These ARB vertex-attrib names are ABI-compatible aliases of the core entry
// points exposed by the Switch headers, so no extension lookup is necessary.
static void shim_glVertexAttribPointerARB(GLuint index, GLint size, GLenum type,
                                          GLboolean normalized, GLsizei stride,
                                          const void* pointer) {
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}
static void shim_glEnableVertexAttribArrayARB(GLuint index) {
    glEnableVertexAttribArray(index);
}
static void shim_glDisableVertexAttribArrayARB(GLuint index) {
    glDisableVertexAttribArray(index);
}



// ─── getauxval (Android uses AT_HWCAP for NEON detection) ───────────────────
static unsigned long stub_getauxval(unsigned long type) {
    switch (type) {
        case 16: return 0x1001;   // AT_HWCAP: NEON (bit 12) + basic ARM64
        case 26: return 0;        // AT_HWCAP2
        default: return 0;
    }
}

// ─── sleep / usleep (common in init code) ────────────────────────────────────
static unsigned int stub_sleep(unsigned int sec) {
    svcSleepThread((uint64_t)sec * 1000000000ULL); return 0;
}
static int stub_usleep(unsigned int usec) {
    svcSleepThread((uint64_t)usec * 1000ULL); return 0;
}

// ─── clock_nanosleep ──────────────────────────────────────────────────────────
static int stub_clock_nanosleep(int, int, const struct timespec* req, struct timespec*) {
    if (req) svcSleepThread(req->tv_sec * 1000000000LL + req->tv_nsec);
    return 0;
}

// ─── strtod_l / strtof_l locale variants ────────────────────────────────────
static double      stub_strtod_l(const char* s, char** e, void*) { return strtod(s, e); }
static float       stub_strtof_l(const char* s, char** e, void*) { return strtof(s, e); }

// ─── mmap / munmap (crash reporters may use anon mmap for stack unwinding) ───
// Map via memalign as a best-effort fallback; executable mapping not supported.
static void* stub_mmap(void*, size_t len, int, int, int, long) {
    void* p = memalign(0x1000, len);
    if (!p) { errno = ENOMEM; return (void*)(uintptr_t)-1; }
    memset(p, 0, len);
    return p;
}
static int stub_munmap(void* p, size_t) { free(p); return 0; }

// ─── __register_atfork (pthread fork support — no-op on Switch) ─────────────
static int stub_register_atfork(void*, void*, void*, void*) { return 0; }

// ─── wcsnrtombs / mbsnrtowcs (GNU ext — stub in case newlib lacks them) ──────
static size_t stub_wcsnrtombs(char* d, const wchar_t** src, size_t nwc, size_t len, void*) {
    if (!src || !*src || !len) return 0;
    size_t w = 0;
    while (nwc-- && **src) {
        char mb[MB_LEN_MAX];
        int n = wctomb(mb, *(*src)++);
        if (n <= 0 || w + (size_t)n >= len) break;
        if (d) memcpy(d + w, mb, n);
        w += n;
    }
    if (d && w < len) d[w] = '\0';
    return w;
}
static size_t stub_mbsnrtowcs(wchar_t* d, const char** src, size_t nmc, size_t len, void*) {
    if (!src || !*src) return 0;
    size_t count = 0;
    while (nmc > 0 && **src && count < len) {
        wchar_t wc;
        int n = mbtowc(&wc, *src, nmc);
        if (n <= 0) break;
        if (d) d[count] = wc;
        count++; *src += n; nmc -= (size_t)n;
    }
    return count;
}

// ─── __sF (Bionic stdio backing array for stdin/stdout/stderr) ───────────────
// Bionic defines FILE __sF[3]; our games may reference &__sF[N] as a FILE*.
// Provide a zero-filled placeholder so GOT entries are non-null.
static uint8_t g_fake_sF[3 * 256] = {};

// Guest Android ELF files can import stdin/stdout/stderr as data symbols.
// These are pointer variables, so the relocation must receive their ADDRESS,
// not the FILE* value itself. Point them at the Switch/newlib standard streams.
// The values stay valid for the lifetime of the process.
static FILE* g_guest_stdin  = stdin;
static FILE* g_guest_stdout = stdout;
static FILE* g_guest_stderr = stderr;

// system() is not part of the Android runtime we want to emulate here.
// Returning failure is preferable to attempting to execute an Android shell
// command on Switch.
static int stub_system(const char* command) {
    compatLogFmt("game system(%s) -> unsupported", command ? command : "null");
    errno = ENOSYS;
    return -1;
}


// Anything the game prints to its stdout/stderr (&__sF[1]/&__sF[2]) would hit
// the zeroed fake FILE and vanish — libc++abi's terminate/verbose-abort
// messages included. Detect fake-__sF FILE* and divert the text to the log.
static bool isFakeStdio(FILE* f) {
    uint8_t* p = (uint8_t*)f;
    return p >= g_fake_sF && p < g_fake_sF + sizeof(g_fake_sF);
}
static int sh_vfprintf(FILE* f, const char* fmt, va_list va) {
    if (isFakeStdio(f)) {
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt ? fmt : "", va);
        // tid tags which thread produced this — e.g. lets us tell whether a
        // libpng decode warning fired on the main/render thread (a real
        // stutter suspect) or a background asset-loader thread (harmless).
        compatLogFmt("game stdio[tid=%p]: %s", (void*)threadGetSelf(), buf);
        return (int)strlen(buf);
    }
    return vfprintf(f, fmt, va);
}
static int sh_fprintf(FILE* f, const char* fmt, ...) {
    va_list va; va_start(va, fmt);
    int r = sh_vfprintf(f, fmt, va);
    va_end(va);
    return r;
}
static int sh_fputs(const char* s, FILE* f) {
    if (isFakeStdio(f)) { compatLogFmt("game stdio[tid=%p]: %s", (void*)threadGetSelf(), s ? s : ""); return 0; }
    return fputs(s, f);
}
static size_t sh_fwrite(const void* p, size_t sz, size_t n, FILE* f) {
    if (isFakeStdio(f)) {
        char buf[512];
        size_t c = sz * n < sizeof(buf) - 1 ? sz * n : sizeof(buf) - 1;
        memcpy(buf, p, c); buf[c] = '\0';
        compatLogFmt("game stdio[tid=%p]: %s", (void*)threadGetSelf(), buf);
        return n;
    }
    return fwrite(p, sz, n, f);
}
static int sh_fputc(int c, FILE* f)  { if (isFakeStdio(f)) return c; return fputc(c, f); }
static int sh_fflush(FILE* f)        { if (isFakeStdio(f)) return 0; return fflush(f); }

// ─── Stack protection (Bionic provides these; stub for newlib) ────────────────
extern "C" { uintptr_t __stack_chk_guard = 0xDEAD0BEEF; }
extern "C" void __stack_chk_fail(void) {
    compatLog("FATAL: stack smash detected");
    abort();
}
extern "C" void __cxa_pure_virtual(void) {
    compatLog("game called __cxa_pure_virtual (pure virtual method call)");
    logTermCaller("pure virtual call", __builtin_return_address(0));
    abort();
}
extern "C" void __cxa_atexit(void*, void*, void*) {}

// ─── Unwind stubs ────────────────────────────────────────────────────────────
// _Unwind_* are already in libgcc; just declare externs so we can take their address.
// __gnu_unwind_frame is ARM-specific; define a stub only if libgcc doesn't have it.
extern "C" {
    void _Unwind_Resume(void*);
    void* _Unwind_GetLanguageSpecificData(void*);
    uintptr_t _Unwind_GetIP(void*);
    void _Unwind_SetIP(void*, uintptr_t);
    uintptr_t _Unwind_GetRegionStart(void*);
}
static void stub_gnu_unwind_frame(void*, void*) {}

// ─── Unity / IL2CPP unresolved-symbol gap fillers ─────────────────────────────
// libunity.so + libil2cpp.so import a batch of Bionic libc / Android symbols
// that our shim table didn't list, so the ELF loader poisoned them with
// 0xBAD0BAD0BAD00000 — and the first libunity static constructor to touch one
// (ctor 32, which indexed the poisoned `_ctype_` table) hard-faulted to the
// Atmosphère screen. These provide safe implementations/stubs so those symbols
// resolve instead of poisoning. Network/process ops are deliberately failed
// (there is no networking or multi-process on this target, same posture as the
// existing socket stubs); filesystem ops fail gracefully; profiling markers are
// no-ops. Signatures are approximate C-ABI — extra register args are harmless.

// _ctype_: Bionic's ctype classification table, referenced directly by inlined
// isX()/toX() macros in Unity/IL2CPP. It's indexed `_ctype_[c+1]` for a byte c,
// so the symbol address must point at the "EOF slot", one before the byte-0
// entry. We over-allocate 128 bytes of zeros on each side so a stray signed-
// char index (the exact thing that faulted at poison-4) lands in mapped,
// zeroed memory instead of crashing.
static unsigned char g_ctype_storage[128 + 1 + 256 + 128];
// Bionic exports _ctype_ as a DATA symbol of type "const char*": the symbol
// itself is the pointer variable, whose contents point at the classification
// table. Keep the pointer initialized statically so relocation can always use
// a valid object address even before CtypeInit runs.
static const unsigned char* g_ctype_base = g_ctype_storage + 128;  // _ctype_ variable -> table
struct CtypeInit {
    CtypeInit() {
        // Bit flags match Bionic/BSD <ctype.h>: _U _L _N _S _P _C _X _B.
        enum { _CU=0x01,_CL=0x02,_CN=0x04,_CS=0x08,_CP=0x10,_CC=0x20,_CX=0x40,_CB=0x80 };
        unsigned char* t = g_ctype_storage + 128;  // t[0] = EOF slot, t[c+1] = byte c
        for (int c = 0; c < 256; c++) {
            unsigned char f = 0;
            if (c >= 'A' && c <= 'Z') f |= _CU;
            if (c >= 'a' && c <= 'z') f |= _CL;
            if (c >= '0' && c <= '9') f |= _CN;
            if (c == ' ' || (c >= '\t' && c <= '\r')) f |= _CS;
            if (c == ' ') f |= _CB;
            if ((c>='!'&&c<='/')||(c>=':'&&c<='@')||(c>='['&&c<='`')||(c>='{'&&c<='~')) f |= _CP;
            if (c < 0x20 || c == 0x7f) f |= _CC;
            if ((c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f')) f |= _CX;
            t[c + 1] = f;
        }
        g_ctype_base = t;  // _ctype_ symbol resolves here
    }
};
static CtypeInit g_ctype_init;
// Resolved lazily in shimResolve so the static-init ordering can't hand back a
// null base (see the _ctype_ special-case there).

// ── string / memory ──
static char* stub_strdup(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static char* stub_strsep(char** stringp, const char* delim) {
    char* s = stringp ? *stringp : nullptr;
    if (!s) return nullptr;
    char* p = s + strcspn(s, delim);
    if (*p) { *p = '\0'; *stringp = p + 1; } else { *stringp = nullptr; }
    return s;
}
static size_t stub_strlcpy(char* dst, const char* src, size_t size) {
    size_t sl = strlen(src);
    if (size) { size_t n = (sl >= size) ? size - 1 : sl; memcpy(dst, src, n); dst[n] = '\0'; }
    return sl;
}

// ── math ──
static float  stub_isnanf(float x)  { return x != x; }
static double stub_remainder(double x, double y) { return remainder(x, y); }
typedef struct { int quot; int rem; } stub_div_t;
static stub_div_t stub_div(int n, int d) { stub_div_t r; r.quot = d ? n/d : 0; r.rem = d ? n%d : 0; return r; }

// ── filesystem (fail gracefully; nothing here needs them to succeed) ──
static int  stub_unlink(const char* path) {
    if (isProtectedRootConfigPath(path))
        compatLogFmt("unlink BLOCKED: protected config %s", path);
    errno = EROFS;
    return -1;
}
static int  stub_rmdir(const char*)              { errno = EROFS; return -1; }
static int  stub_truncate(const char*, long)     { errno = EROFS; return -1; }
static int  stub_ftruncate(int, long)            { errno = EROFS; return -1; }
static long stub_lseek64(int fd, long off, int w){ return lseek(fd, off, w); }
static int  stub_flock(int, int)                 { return 0; }
static int  stub_utime(const char*, const void*) { return 0; }
static int  stub_statfs(const char*, void* buf)  { if (buf) memset(buf, 0, 64); return 0; }
static int  stub_fscanf(FILE*, const char*, ...) { return -1; /* EOF */ }
static int  stub_tcflush(int, int)               { return 0; }

// ── process / scheduling / signals (single-process, no ptrace) ──
static int    stub_getpriority(int, int)         { return 0; }
static int    stub_setpriority(int, int, int)    { return 0; }
static long   stub_ptrace(int, ...)              { errno = EPERM; return -1; }
static int    stub_getpagesize(void)             { return 0x1000; }
static void*  stub_getpwuid(unsigned)            { return nullptr; }
static int    stub_sigsuspend(const void*)       { errno = EINTR; return -1; }
static int    stub_clock_getres(int, struct timespec* ts) { if (ts) { ts->tv_sec = 0; ts->tv_nsec = 1; } return 0; }
static int    stub_uname(void* buf)              { if (buf) memset(buf, 0, 6 * 65); return 0; }
static int    stub_madvise(void*, size_t, int)   { return 0; }

// ── pthread extras ──
static void   stub_pthread_exit(void*)                 { while (true) svcSleepThread(1000000000ULL); }
static int    stub_pthread_atfork(void*, void*, void*) { return 0; }
static int    stub_pthread_getattr_np(void*, void*)    { return 0; }
static int    stub_pthread_condattr_init(void*)        { return 0; }
static int    stub_pthread_condattr_destroy(void*)     { return 0; }
static int    stub_pthread_condattr_setclock(void*, int) { return 0; }
static int    stub_sem_getvalue(void*, int* v)         { if (v) *v = 0; return 0; }

// ── networking (no network on this target — fail like the existing socket stubs) ──
static unsigned long stub_inet_addr(const char*)                 { return 0xFFFFFFFFUL; }
static const char*   stub_inet_ntop(int, const void*, char* dst, unsigned) { if (dst) dst[0] = '\0'; return dst; }
static int           stub_inet_pton(int, const char*, void*)     { return 0; }
static int           stub_getaddrinfo(const char* host, const char* svc, const void*, void** res) {
    char d[192];
    snprintf(d, sizeof(d), "%s%s%s", host ? host : "?", svc ? ":" : "", svc ? svc : "");
    logNetProbe("getaddrinfo", d);
    if (res) *res = nullptr;
    return -2;  // EAI_NONAME
}
static void          stub_freeaddrinfo(void*)                    {}
static int           stub_getnameinfo(const void*, unsigned, char*, unsigned, char*, unsigned, int) { return -2; }
static int           stub_gethostname(char* n, size_t l)         { if (n && l) { strncpy(n, "switch", l - 1); n[l-1] = '\0'; } return 0; }
static void*         stub_gethostbyaddr(const void*, int, int)   { return nullptr; }
static long          stub_recvfrom(int, void*, size_t, int, void*, void*) { errno = ENOTCONN; return -1; }
static long          stub_recvmsg(int, void*, int)               { errno = ENOTCONN; return -1; }
static long          stub_sendmsg(int, const void*, int)         { errno = ENOTCONN; return -1; }
static int           stub_shutdown(int, int)                     { return 0; }

// ── Android platform ──
static int   stub_system_property_get(const char*, char* value) { if (value) value[0] = '\0'; return 0; }
static void* stub_ANativeWindow_fromSurface(void*, void*)        { return &compatGet()->window; }
static int   stub_ASensorEventQueue_hasEvents(void*)            { return 0; }
static void  stub_google_region(void)                           {}  // profiling markers — no-op

// ── wide char extras ──
static unsigned stub_wctype(const char*)             { return 0; }
static int      stub_iswctype(unsigned, unsigned)    { return 0; }
static size_t   stub_wcsftime(wchar_t* s, size_t m, const wchar_t*, const void*) { if (m) s[0] = 0; return 0; }

// I/O vector write — implement over the existing fd write path.
static long stub_writev(int fd, const void* iov_in, int cnt) {
    struct IoVec { void* base; size_t len; };
    const IoVec* iov = (const IoVec*)iov_in;
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        long n = write(fd, iov[i].base, iov[i].len);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < iov[i].len) break;
    }
    return total;
}

// ─── Shim table ──────────────────────────────────────────────────────────────

// ─── Coverage gaps found by auditing a real session's unresolved list ────────
// Every symbol an .so imports and we don't provide is a landmine: it's poisoned
// and the game only dies if it ever reaches it, which is what makes these show
// up as unrelated game-specific bugs much later. These came from Brain It On's
// 30 unresolved imports, but nothing here is game-specific — they're ordinary
// libc/zlib/Android surface that any NDK title can reference.

// zlib. The Core already links -lz, so these are straight passthroughs; games
// use them to decompress their own assets, and an unresolved inflate is a
// silent asset-loading failure rather than an obvious crash.
// (declarations come from <zlib.h>, included above)

// Android's system-property store. There is no property service here, so the
// honest answer is "no such property" — 0 length, empty value. Games use it for
// device fingerprinting and feature checks and cope with absence.
static const void* stub_system_property_find(const char*) { return nullptr; }
static int stub_system_property_read(const void*, char* name, char* value) {
    if (name)  name[0]  = '\0';
    if (value) value[0] = '\0';
    return 0;
}

// The remainder of the audited gap. Each is either a real implementation the
// toolchain lacks, or a stub whose failure mode is the one the caller already
// has to handle.
static void* stub_memrchr(const void* sv, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)sv;
    while (n--) if (p[n] == (unsigned char)c) return (void*)(p + n);
    return nullptr;
}
// sincos is a GNU convenience wrapper; computing both is exactly its contract.
static void stub_sincos(double x, double* s, double* c) {
    if (s) *s = sin(x);
    if (c) *c = cos(x);
}
// Timed semaphore wait. Horizon's condvar-based semaphores don't expose a
// deadline through this shape, so this waits without one — a caller that
// expected a timeout blocks longer than it asked, which is far better than
// returning "timed out" while the resource was actually available.
static int stub_sem_timedwait(void* sem, const void*) {
    return sem_wait((sem_t*)sem);
}
// Socket calls that can't work without a Berkeley socket a game actually owns.
// Returning an error is honest; pretending success would corrupt its state.
static int stub_getpeername(int, void*, unsigned*) { errno = ENOTCONN; return -1; }
static long stub_sendto(int, const void*, size_t, int, const void*, unsigned) {
    errno = ENOTSOCK; return -1;
}
static long stub_sendfile(int, int, void*, size_t) { errno = ENOSYS; return -1; }

// basename. <libgen.h> declares it but this libc doesn't implement it, so the
// table entry linked against nothing. POSIX semantics: last path component,
// "." for an empty or null path, and trailing slashes ignored.
static char* stub_basename(char* path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';   // strip trailing /
    char* slash = strrchr(path, '/');
    if (!slash) return path;
    if (slash[1]) return slash + 1;
    return slash == path ? path : dot;                       // "/" -> "/"
}

// drand48 family. Not exposed by this toolchain's headers, and games use it
// for ordinary pseudo-randomness rather than anything reproducible across
// platforms, so the standard 48-bit LCG is implemented directly.
static unsigned long long g_rand48 = 0x1234ABCD330EULL;
static void stub_srand48(long seed) {
    g_rand48 = ((unsigned long long)(unsigned long)seed << 16) | 0x330EULL;
}
static long stub_lrand48(void) {
    g_rand48 = (0x5DEECE66DULL * g_rand48 + 0xB) & 0xFFFFFFFFFFFFULL;
    return (long)(g_rand48 >> 17);          // top 31 bits, as specified
}
static long stub_mrand48(void) {
    g_rand48 = (0x5DEECE66DULL * g_rand48 + 0xB) & 0xFFFFFFFFFFFFULL;
    return (long)(int)(g_rand48 >> 16);     // signed 32-bit
}
static double stub_drand48(void) {
    g_rand48 = (0x5DEECE66DULL * g_rand48 + 0xB) & 0xFFFFFFFFFFFFULL;
    return (double)g_rand48 / 281474976710656.0;   // / 2^48
}

// Process identity. Horizon has no users or groups; report root consistently
// rather than inventing an id that some check might treat as unprivileged.
static unsigned stub_getegid(void) { return 0; }
static unsigned stub_geteuid(void) { return 0; }
static int stub_getpwuid_r(unsigned, void*, char*, size_t, void** result) {
    if (result) *result = nullptr;
    return 0;                       // "no such user" — not an error
}

// CPU affinity. The Switch does pin threads to cores, but not through this
// interface, and a game asking is only ever optimising. Report success with an
// empty set rather than failing a call it doesn't check.
static int stub_sched_getaffinity(int, size_t len, void* mask) {
    if (mask && len) memset(mask, 0, len);
    return 0;
}
static int stub_sched_setaffinity(int, size_t, const void*) { return 0; }

// Sockets a game may reference but can't meaningfully use here.
static int stub_socketpair(int, int, int, int sv[2]) {
    if (sv) { sv[0] = -1; sv[1] = -1; }
    errno = EAFNOSUPPORT;
    return -1;
}
static unsigned stub_if_nametoindex(const char*) { return 0; }

// Filesystem calls newlib doesn't carry. Failing loudly here is better than a
// silent no-op: a game that relies on a hard link and gets a lie will corrupt
// its own state later, whereas EPERM is a case it already has to handle.
static int stub_link(const char*, const char*)      { errno = EPERM;  return -1; }
static int stub_futimens(int, const void*)          { return 0; }   // timestamps are cosmetic

struct ShimEntry { const char* name; void* ptr; };

extern "C" void near_openal_tls_local_context_init();
extern "C" int near_openal_cxa_thread_atexit(void (*dtor)(void*), void* obj, void* dso);


static const ShimEntry g_shims[] = {
    // ── OpenAL Soft Android TLS ABI fallbacks ─────────────────────────────
    {"_ZTHN10ALCcontext13sLocalContextE", (void*)near_openal_tls_local_context_init},
    {"__cxa_thread_atexit_impl", (void*)near_openal_cxa_thread_atexit},
    // ── zlib (linked; games decompress their own assets with it) ───────────
    {"inflate",         (void*)inflate},
    {"inflateEnd",      (void*)inflateEnd},
    {"inflateInit_",    (void*)inflateInit_},
    {"inflateInit2_",   (void*)inflateInit2_},
    {"inflateReset",    (void*)inflateReset},
    {"deflate",         (void*)deflate},
    {"deflateEnd",      (void*)deflateEnd},
    {"deflateInit_",    (void*)deflateInit_},
    {"deflateInit2_",   (void*)deflateInit2_},
    {"crc32",           (void*)crc32},
    {"adler32",         (void*)adler32},
    {"uncompress",      (void*)uncompress},
    {"compress",        (void*)compress},

    // ── libc the toolchain already provides ────────────────────────────────
    {"strcspn",         (void*)strcspn},
    {"memrchr",         (void*)stub_memrchr},
    {"sincos",          (void*)stub_sincos},
    {"setvbuf",         (void*)setvbuf},
    {"basename",        (void*)stub_basename},
    {"lldiv",           (void*)lldiv},
    {"logb",            (void*)logb},
    {"scalbn",          (void*)scalbn},
    {"srand48",         (void*)stub_srand48},
    {"lrand48",         (void*)stub_lrand48},
    {"mrand48",         (void*)stub_mrand48},
    {"drand48",         (void*)stub_drand48},
    {"fnmatch",         (void*)fnmatch},

    // ── Android / POSIX surface with no Horizon equivalent ─────────────────
    {"__system_property_find", (void*)stub_system_property_find},
    {"__system_property_read", (void*)stub_system_property_read},
    {"getegid",         (void*)stub_getegid},
    {"geteuid",         (void*)stub_geteuid},
    {"getpwuid_r",      (void*)stub_getpwuid_r},
    {"sched_getaffinity", (void*)stub_sched_getaffinity},
    {"sched_setaffinity", (void*)stub_sched_setaffinity},
    {"socketpair",      (void*)stub_socketpair},
    {"getpeername",     (void*)stub_getpeername},
    {"sendto",          (void*)stub_sendto},
    {"sendfile",        (void*)stub_sendfile},
    {"sem_timedwait",   (void*)stub_sem_timedwait},
    {"if_nametoindex",  (void*)stub_if_nametoindex},
    {"link",            (void*)stub_link},
    {"futimens",        (void*)stub_futimens},

    // ── liblog ──────────────────────────────────────────────────────────────
    {"__android_log_print",     (void*)android_log_print},
    {"__android_log_write",     (void*)android_log_write},
    {"__android_log_vprint",    (void*)android_log_vprint},
    {"__android_log_buf_print", (void*)android_log_buf_print},

    // ── libc / newlib passthrough ────────────────────────────────────────────
    // Guest stdio data symbols.
    {"stdin",       (void*)&g_guest_stdin},
    {"stdout",      (void*)&g_guest_stdout},
    {"stderr",      (void*)&g_guest_stderr},
    {"tmpnam",      (void*)tmpnam},
    {"system",      (void*)stub_system},
    {"malloc",      (void*)sh_malloc},
    {"free",        (void*)sh_free},
    {"calloc",      (void*)sh_calloc},
    {"realloc",     (void*)sh_realloc},
    {"memalign",    (void*)memalign},
    {"posix_memalign",(void*)stub_posix_memalign},
    {"memcpy",      (void*)memcpy},
    {"memmove",     (void*)memmove},
    {"memset",      (void*)memset},
    {"memcmp",      (void*)memcmp},
    {"memchr",      (void*)memchr},
    {"strlen",      (void*)strlen},
    {"strnlen",     (void*)stub_strnlen},
    {"strcmp",      (void*)strcmp},
    {"strncmp",     (void*)strncmp},
    {"strcasecmp",  (void*)strcasecmp},
    {"strncasecmp", (void*)strncasecmp},
    {"strcpy",      (void*)strcpy},
    {"strncpy",     (void*)strncpy},
    {"strcat",      (void*)strcat},
    {"strncat",     (void*)strncat},
    {"strchr",      (void*)strchr},
    {"strrchr",     (void*)strrchr},
    {"strstr",      (void*)strstr},
    {"strtok",      (void*)strtok},
    {"strtok_r",    (void*)stub_strtok_r},
    {"strtol",      (void*)strtol},
    {"strtoul",     (void*)strtoul},
    {"strtoll",     (void*)strtoll},
    {"strtoull",    (void*)strtoull},
    {"strtof",      (void*)strtof},
    {"strtod",      (void*)strtod},
    {"atoi",        (void*)atoi},
    {"atol",        (void*)atol},
    {"atoll",       (void*)atoll},
    {"atof",        (void*)atof},
    {"sprintf",     (void*)sprintf},
    {"snprintf",    (void*)snprintf},
    {"sscanf",      (void*)sscanf},
    {"printf",      (void*)printf},
    {"fprintf",     (void*)sh_fprintf},
    {"vprintf",     (void*)vprintf},
    {"vfprintf",    (void*)sh_vfprintf},
    {"vsprintf",    (void*)vsprintf},
    {"vsnprintf",   (void*)vsnprintf},
    {"fopen",       (void*)stub_fopen},
    {"fopen64",     (void*)stub_fopen},
    {"fclose",      (void*)sh_fclose},
    {"fread",       (void*)sh_fread},
    {"fwrite",      (void*)sh_fwrite},
    {"fseek",       (void*)sh_fseek},
    {"ftell",       (void*)sh_ftell},
    {"fseeko",      (void*)stub_fseeko},
    {"ftello",      (void*)stub_ftello},
    {"rewind",      (void*)sh_rewind},
    {"fflush",      (void*)sh_fflush},
    {"feof",        (void*)sh_feof},
    {"ferror",      (void*)ferror},
    {"fgets",       (void*)fgets},
    {"fputs",       (void*)sh_fputs},
    {"fgetc",       (void*)sh_fgetc},
    {"fputc",       (void*)sh_fputc},
    {"getc",        (void*)sh_fgetc},
    {"putc",        (void*)putc},
    {"ungetc",      (void*)ungetc},
    {"open",        (void*)stub_open},
    {"close",       (void*)sh_close},
    {"read",        (void*)sh_read},
    {"write",       (void*)sh_write},
    {"lseek",       (void*)lseek},
    {"stat",        (void*)stub_stat},
    {"stat64",       (void*)stub_stat},
    {"fstat",       (void*)fstat},
    {"fstat64",      (void*)stub_fstat64},
    {"mkdir",       (void*)mkdir},
    {"opendir",     (void*)stub_opendir},
    {"readdir",     (void*)stub_readdir},
    {"readdir64",   (void*)stub_readdir64},
    {"closedir",    (void*)stub_closedir},
    {"_findfirst64", (void*)stub_findfirst64},
    {"_findnext64",  (void*)stub_findnext64},
    {"_findclose",   (void*)stub_findclose64},
    // Some Android/NDK CRT variants spell the 64-bit directory enumeration
    // helpers with a double underscore or without the Windows-compatible
    // leading underscore. Map those spellings to the same implementation.
    {"__findfirst64", (void*)stub_findfirst64},
    {"__findnext64",  (void*)stub_findnext64},
    {"__findclose64", (void*)stub_findclose64},
    {"findfirst64",   (void*)stub_findfirst64},
    {"findnext64",    (void*)stub_findnext64},
    {"findclose64",   (void*)stub_findclose64},
    {"abort",       (void*)sh_abort},
    {"exit",        (void*)sh_exit},
    {"qsort",       (void*)qsort},
    {"bsearch",     (void*)bsearch},
    {"rand",        (void*)rand},
    {"srand",       (void*)srand},
    {"time",        (void*)time},
    {"clock",       (void*)clock},
    {"getenv",      (void*)getenv},
    {"setenv",      (void*)stub_setenv},
    {"unsetenv",    (void*)stub_unsetenv},
    {"__errno",     (void*)bionic_errno},
    {"__stack_chk_fail",   (void*)sh_stack_chk_fail},
    {"__cxa_atexit",       (void*)__cxa_atexit},
    {"__cxa_pure_virtual", (void*)__cxa_pure_virtual},
    {"__cxa_thread_atexit_impl", (void*)stub___cxa_thread_atexit_impl},

    // ── libm passthrough ─────────────────────────────────────────────────────
    {"sin",   (void*)sin},   {"sinf",  (void*)sinf},
    {"cos",   (void*)cos},   {"cosf",  (void*)cosf},
    {"tan",   (void*)tan},   {"tanf",  (void*)tanf},
    {"asin",  (void*)asin},  {"asinf", (void*)asinf},
    {"acos",  (void*)acos},  {"acosf", (void*)acosf},
    {"atan",  (void*)atan},  {"atanf", (void*)atanf},
    {"atan2", (void*)atan2}, {"atan2f",(void*)atan2f},
    {"sqrt",  (void*)sqrt},  {"sqrtf", (void*)sqrtf},
    {"pow",   (void*)pow},   {"powf",  (void*)powf},
    {"exp",   (void*)exp},   {"expf",  (void*)expf},
    {"exp2",  (void*)exp2},  {"exp2f", (void*)exp2f},
    {"log",   (void*)log},   {"logf",  (void*)logf},
    {"log2",  (void*)log2},  {"log2f", (void*)log2f},
    {"log10", (void*)log10}, {"log10f",(void*)log10f},
    {"floor", (void*)floor}, {"floorf",(void*)floorf},
    {"ceil",  (void*)ceil},  {"ceilf", (void*)ceilf},
    {"round", (void*)round}, {"roundf",(void*)roundf},
    {"fabs",  (void*)fabs},  {"fabsf", (void*)fabsf},
    {"fmod",  (void*)fmod},  {"fmodf", (void*)fmodf},
    {"fmin",  (void*)fmin},  {"fminf", (void*)fminf},
    {"fmax",  (void*)fmax},  {"fmaxf", (void*)fmaxf},
    {"hypot", (void*)hypot}, {"hypotf",(void*)hypotf},
    {"ldexp", (void*)ldexp}, {"ldexpf",(void*)ldexpf},
    {"frexp", (void*)frexp}, {"frexpf",(void*)frexpf},
    {"modf",  (void*)modf},  {"modff", (void*)modff},
    {"trunc", (void*)trunc}, {"truncf",(void*)truncf},
    {"copysign",(void*)copysign}, {"copysignf",(void*)copysignf},
    {"cbrt",  (void*)cbrt},  {"cbrtf", (void*)cbrtf},
    {"sinh",  (void*)sinh},  {"sinhf", (void*)sinhf},
    {"cosh",  (void*)cosh},  {"coshf", (void*)coshf},
    {"tanh",  (void*)tanh},  {"tanhf", (void*)tanhf},
    // isinf/isnan are macros in C99; provide lambda-wrapped stubs
    {"isinf", (void*)+[](double x) -> int { return std::isinf(x); }},
    {"isnan", (void*)+[](double x) -> int { return std::isnan(x); }},

    // ── pthread stubs ────────────────────────────────────────────────────────
    {"pthread_mutex_init",     (void*)pt_mutex_init},
    {"pthread_mutex_lock",     (void*)pt_mutex_lock},
    {"pthread_mutex_unlock",   (void*)pt_mutex_unlock},
    {"pthread_mutex_trylock",  (void*)pt_mutex_trylock},
    {"pthread_mutex_destroy",  (void*)pt_mutex_destroy},
    {"pthread_cond_init",      (void*)pt_cond_init},
    {"pthread_cond_signal",    (void*)pt_cond_signal},
    {"pthread_cond_broadcast", (void*)pt_cond_broadcast},
    {"pthread_cond_wait",      (void*)pt_cond_wait},
    {"pthread_cond_timedwait", (void*)pt_cond_timedwait},
    {"pthread_cond_destroy",   (void*)pt_cond_destroy},
    {"pthread_rwlock_init",    (void*)pt_rwlock_init},
    {"pthread_rwlock_rdlock",  (void*)pt_rwlock_rdlock},
    {"pthread_rwlock_wrlock",  (void*)pt_rwlock_wrlock},
    {"pthread_rwlock_unlock",  (void*)pt_rwlock_unlock},
    {"pthread_rwlock_destroy", (void*)pt_rwlock_destroy},
    {"pthread_create",         (void*)pt_create},
    {"pthread_join",           (void*)pt_join},
    {"pthread_detach",         (void*)pt_detach},
    {"pthread_self",           (void*)pt_self},
    {"pthread_equal",          (void*)pt_equal},
    {"pthread_key_create",     (void*)pt_key_create},
    {"pthread_key_delete",     (void*)pt_key_delete},
    {"pthread_getspecific",    (void*)pt_getspecific},
    {"pthread_setspecific",    (void*)pt_setspecific},
    {"pthread_once",           (void*)pt_once},
    {"pthread_attr_init",       (void*)pt_attr_init},
    {"pthread_attr_destroy",    (void*)pt_attr_destroy},
    {"pthread_attr_setdetachstate",(void*)pt_attr_setdetachstate},
    {"pthread_attr_setstacksize", (void*)pt_attr_setstacksize},
    {"pthread_attr_getstacksize", (void*)pt_attr_getstacksize},
    {"pthread_rwlock_tryrdlock", (void*)stub_pthread_rwlock_tryrdlock},
    {"pthread_rwlock_trywrlock", (void*)stub_pthread_rwlock_trywrlock},
    {"pthread_setschedparam", (void*)stub_pthread_setschedparam},

    // ── libdl ────────────────────────────────────────────────────────────────
    {"dlopen",  (void*)fake_dlopen},
    {"dlsym",   (void*)fake_dlsym},
    {"dlclose", (void*)fake_dlclose},
    {"dlerror", (void*)fake_dlerror},

    // ── SDL3 graphics-init probes ───────────────────────────────────────────
    {"SDL_CreateWindow",     (void*)w_SDL_CreateWindow},
    {"SDL_GL_CreateContext",(void*)w_SDL_GL_CreateContext},
    {"SDL_GL_MakeCurrent",  (void*)w_SDL_GL_MakeCurrent},

    // ── libandroid ───────────────────────────────────────────────────────────
    {"AAssetManager_fromJava",      (void*)assetMgr_fromJava},
    {"AAssetManager_open",          (void*)asset_open},
    {"AAssetManager_openDir",       (void*)asset_openDir},
    {"AAssetDir_getNextFileName",   (void*)asset_dirNext},
    {"AAssetDir_rewind",            (void*)asset_dirRewind},
    {"AAssetDir_close",             (void*)asset_dirClose},
    {"AAsset_close",                (void*)asset_close},
    {"AAsset_read",                 (void*)asset_read},
    {"AAsset_seek",                 (void*)asset_seek},
    {"AAsset_seek64",               (void*)asset_seek64},
    {"AAsset_getLength",            (void*)asset_length},
    {"AAsset_getLength64",          (void*)asset_length},
    {"AAsset_getRemainingLength",   (void*)asset_remain},
    {"AAsset_getRemainingLength64", (void*)asset_remain},
    {"AAsset_getBuffer",            (void*)asset_buffer},
    {"AAsset_isAllocated",          (void*)asset_isAllocated},
    {"ANativeWindow_getWidth",      (void*)nwin_getWidth},
    {"ANativeWindow_getHeight",     (void*)nwin_getHeight},
    {"ANativeWindow_getFormat",     (void*)nwin_getFormat},
    {"ANativeWindow_setBuffersGeometry", (void*)nwin_setBuffersGeometry},
    {"ANativeWindow_acquire",       (void*)nwin_acquire},
    {"ANativeWindow_release",       (void*)nwin_release},
    {"ANativeWindow_lock",          (void*)nwin_lock},
    {"ANativeWindow_unlockAndPost", (void*)nwin_unlockAndPost},
    {"ALooper_forThread",           (void*)looper_forThread},
    {"ALooper_prepare",             (void*)looper_prepare},
    {"ALooper_acquire",             (void*)looper_acquire},
    {"ALooper_release",             (void*)looper_release},
    {"ALooper_pollOnce",            (void*)looper_pollOnce},
    {"ALooper_pollAll",             (void*)looper_pollAll},
    {"ALooper_wake",                (void*)looper_wake},
    {"ALooper_addFd",               (void*)looper_addFd},
    {"ALooper_removeFd",            (void*)looper_removeFd},
    {"AInputQueue_getEvent",        (void*)iq_getEvent},
    {"AInputQueue_hasEvents",       (void*)iq_hasEvents},
    {"AInputQueue_finishEvent",     (void*)iq_finishEvent},
    {"AInputEvent_getType",         (void*)ev_getType},
    {"AInputEvent_getSource",       (void*)ev_getSource},
    {"AMotionEvent_getAction",      (void*)ev_getAction},
    {"AMotionEvent_getX",           (void*)ev_getX},
    {"AMotionEvent_getY",           (void*)ev_getY},
    {"AMotionEvent_getPointerCount",(void*)ev_getPointerCount},
    {"AMotionEvent_getPointerId",   (void*)ev_getPointerId},
    {"AMotionEvent_getPressure",    (void*)ev_getPressure},
    {"AMotionEvent_getSize",        (void*)ev_getSize},
    {"AKeyEvent_getKeyCode",        (void*)ev_getKeyCode},
    {"AKeyEvent_getMetaState",      (void*)ev_getMetaState},
    {"AKeyEvent_getAction",         (void*)ev_getAction},
    {"AConfiguration_new",          (void*)acfg_new},
    {"AConfiguration_delete",       (void*)acfg_delete},
    {"AConfiguration_fromAssetManager", (void*)acfg_fromAssetManager},
    {"AConfiguration_getDensity",   (void*)acfg_getDensity},
    {"AConfiguration_getOrientation",(void*)acfg_getOrientation},
    {"AConfiguration_getScreenSize",(void*)acfg_getScreenSize},
    {"AConfiguration_getSdkVersion",(void*)acfg_getSdkVersion},

    // ── unwinding / misc runtime ──────────────────────────────────────────────
    {"_Unwind_Resume",                     (void*)_Unwind_Resume},
    {"_Unwind_GetLanguageSpecificData",    (void*)_Unwind_GetLanguageSpecificData},
    {"_Unwind_GetIP",                      (void*)_Unwind_GetIP},
    {"_Unwind_SetIP",                      (void*)_Unwind_SetIP},
    {"_Unwind_GetRegionStart",             (void*)_Unwind_GetRegionStart},
    {"__gnu_unwind_frame",                 (void*)stub_gnu_unwind_frame},

    // ── EGL passthrough (switch-mesa) ─────────────────────────────────────────
    {"eglGetDisplay",       (void*)eglGetDisplay},
    {"eglInitialize",       (void*)eglInitialize},
    {"eglTerminate",        (void*)eglTerminate},
    {"eglBindAPI",          (void*)eglBindAPI},
    {"eglChooseConfig",     (void*)eglChooseConfig},
    {"eglGetConfigs",       (void*)eglGetConfigs},
    {"eglGetConfigAttrib",  (void*)eglGetConfigAttrib},
    {"eglCreateContext",    (void*)w_eglCreateContext},
    {"eglDestroyContext",   (void*)eglDestroyContext},
    {"eglCreateWindowSurface", (void*)w_eglCreateWindowSurface},
    {"eglCreatePbufferSurface",(void*)eglCreatePbufferSurface},
    {"eglDestroySurface",   (void*)eglDestroySurface},
    {"eglMakeCurrent",      (void*)w_eglMakeCurrent},
    {"eglSurfaceAttrib", (void*)eglSurfaceAttrib},
    {"eglSwapBuffers",      (void*)eglSwapBuffers},
    {"eglSwapInterval",     (void*)eglSwapInterval},

    // ── Android NDK Sensor API (source/compat/sensors.cpp) — real accelerometer ──
    {"ASensorManager_getInstance",           (void*)ASensorManager_getInstance},
    {"ASensorManager_getInstanceForPackage", (void*)ASensorManager_getInstanceForPackage},
    {"ASensorManager_getSensorList",         (void*)ASensorManager_getSensorList},
    {"ASensorManager_getDefaultSensor",      (void*)ASensorManager_getDefaultSensor},
    {"ASensorManager_createEventQueue",      (void*)ASensorManager_createEventQueue},
    {"ASensorManager_destroyEventQueue",     (void*)ASensorManager_destroyEventQueue},
    {"ASensorEventQueue_enableSensor",       (void*)ASensorEventQueue_enableSensor},
    {"ASensorEventQueue_disableSensor",      (void*)ASensorEventQueue_disableSensor},
    {"ASensorEventQueue_setEventRate",       (void*)ASensorEventQueue_setEventRate},
    {"ASensorEventQueue_getEvents",          (void*)ASensorEventQueue_getEvents},
    {"ASensor_getType",                      (void*)ASensor_getType},
    {"ASensor_getName",                      (void*)ASensor_getName},
    {"ASensor_getVendor",                    (void*)ASensor_getVendor},
    {"ASensor_getResolution",                (void*)ASensor_getResolution},
    {"ASensor_getMinDelay",                  (void*)ASensor_getMinDelay},
    {"eglGetCurrentContext",(void*)eglGetCurrentContext},
    {"eglGetCurrentSurface",(void*)eglGetCurrentSurface},
    {"eglGetCurrentDisplay",(void*)eglGetCurrentDisplay},
    {"eglQueryString",      (void*)eglQueryString},
    {"eglQuerySurface",     (void*)eglQuerySurface},
    {"eglQueryContext",     (void*)eglQueryContext},
    {"eglGetError",         (void*)eglGetError},
    {"eglGetProcAddress",   (void*)w_eglGetProcAddress},
    {"eglReleaseThread",    (void*)eglReleaseThread},
    {"eglWaitGL",           (void*)eglWaitGL},
    {"eglWaitClient",       (void*)eglWaitClient},
    {"eglWaitNative",       (void*)eglWaitNative},
    {"eglCopyBuffers",      (void*)eglCopyBuffers},
    {"eglBindTexImage",     (void*)eglBindTexImage},
    {"eglReleaseTexImage",  (void*)eglReleaseTexImage},

    // ── GLES 2 passthrough ────────────────────────────────────────────────────
    {"glActiveTexture",     (void*)glActiveTexture},
    {"glAttachShader",      (void*)glAttachShader},
    {"glBindAttribLocation",(void*)glBindAttribLocation},
    {"glBindBuffer",        (void*)glBindBuffer},
    {"glBindFramebuffer",   (void*)glBindFramebuffer},
    {"glBindRenderbuffer",  (void*)glBindRenderbuffer},
    {"glBindTexture",       (void*)glBindTexture},
    {"glBlendColor",        (void*)glBlendColor},
    {"glBlendEquation",     (void*)glBlendEquation},
    {"glBlendEquationSeparate",(void*)glBlendEquationSeparate},
    {"glBlendFunc",         (void*)glBlendFunc},
    {"glBlendFuncSeparate", (void*)glBlendFuncSeparate},
    {"glBufferData",        (void*)glBufferData},
    {"glBufferSubData",     (void*)glBufferSubData},
    {"glCheckFramebufferStatus",(void*)glCheckFramebufferStatus},
    {"glClear",             (void*)w_glClear},
    {"glClearColor",        (void*)glClearColor},
    {"glClearDepthf",       (void*)glClearDepthf},
    {"glClearStencil",      (void*)glClearStencil},
    {"glColorMask",         (void*)glColorMask},
    {"glCompileShader",     (void*)glCompileShader},
    {"glCompressedTexImage2D",  (void*)glCompressedTexImage2D},
    {"glCompressedTexSubImage2D",(void*)glCompressedTexSubImage2D},
    {"glCopyTexImage2D",    (void*)glCopyTexImage2D},
    {"glCopyTexSubImage2D", (void*)glCopyTexSubImage2D},
    {"glCreateProgram",     (void*)glCreateProgram},
    {"glCreateShader",      (void*)glCreateShader},
    {"glCullFace",          (void*)glCullFace},
    {"glDeleteBuffers",     (void*)glDeleteBuffers},
    {"glDeleteFramebuffers",(void*)glDeleteFramebuffers},
    {"glDeleteProgram",     (void*)glDeleteProgram},
    {"glDeleteRenderbuffers",(void*)glDeleteRenderbuffers},
    {"glDeleteShader",      (void*)glDeleteShader},
    {"glDeleteTextures",    (void*)glDeleteTextures},
    {"glDepthFunc",         (void*)glDepthFunc},
    {"glDepthMask",         (void*)glDepthMask},
    {"glDepthRangef",       (void*)glDepthRangef},
    {"glDetachShader",      (void*)glDetachShader},
    {"glDisable",           (void*)glDisable},
    {"glDisableVertexAttribArray",(void*)glDisableVertexAttribArray},
    {"glDrawArrays",        (void*)glDrawArrays},
    {"glDrawElements",      (void*)glDrawElements},
    {"glEnable",            (void*)glEnable},
    {"glEnableVertexAttribArray",(void*)glEnableVertexAttribArray},
    {"glFinish",            (void*)glFinish},
    {"glFlush",             (void*)glFlush},
    {"glFramebufferRenderbuffer",(void*)glFramebufferRenderbuffer},
    {"glFramebufferTexture2D",  (void*)glFramebufferTexture2D},
    {"glFrontFace",         (void*)glFrontFace},
    {"glGenBuffers",        (void*)glGenBuffers},
    {"glGenerateMipmap",    (void*)glGenerateMipmap},
    {"glGenFramebuffers",   (void*)glGenFramebuffers},
    {"glGenRenderbuffers",  (void*)glGenRenderbuffers},
    {"glGenTextures",       (void*)glGenTextures},
    {"glGetActiveAttrib",   (void*)glGetActiveAttrib},
    {"glGetActiveUniform",  (void*)glGetActiveUniform},
    {"glGetAttachedShaders",(void*)glGetAttachedShaders},
    {"glGetAttribLocation", (void*)glGetAttribLocation},
    {"glGetBooleanv",       (void*)glGetBooleanv},
    {"glGetBufferParameteriv",(void*)glGetBufferParameteriv},
    {"glGetError",          (void*)glGetError},
    {"glGetFloatv",         (void*)glGetFloatv},
    {"glGetFramebufferAttachmentParameteriv",(void*)glGetFramebufferAttachmentParameteriv},
    {"glGetIntegerv",       (void*)glGetIntegerv},
    {"glGetProgramiv",      (void*)glGetProgramiv},
    {"glGetProgramInfoLog", (void*)glGetProgramInfoLog},
    {"glGetRenderbufferParameteriv",(void*)glGetRenderbufferParameteriv},
    {"glGetShaderiv",       (void*)glGetShaderiv},
    {"glGetShaderInfoLog",  (void*)glGetShaderInfoLog},
    {"glGetShaderPrecisionFormat",(void*)glGetShaderPrecisionFormat},
    {"glGetShaderSource",   (void*)glGetShaderSource},
    {"glGetString",         (void*)glGetString},
    {"glGetTexParameterfv", (void*)glGetTexParameterfv},
    {"glGetTexParameteriv", (void*)glGetTexParameteriv},
    {"glGetUniformfv",      (void*)glGetUniformfv},
    {"glGetUniformiv",      (void*)glGetUniformiv},
    {"glGetUniformLocation",(void*)glGetUniformLocation},
    {"glGetVertexAttribfv", (void*)glGetVertexAttribfv},
    {"glGetVertexAttribiv", (void*)glGetVertexAttribiv},
    {"glGetVertexAttribPointerv",(void*)glGetVertexAttribPointerv},
    {"glHint",              (void*)glHint},
    {"glIsBuffer",          (void*)glIsBuffer},
    {"glIsEnabled",         (void*)glIsEnabled},
    {"glIsFramebuffer",     (void*)glIsFramebuffer},
    {"glIsProgram",         (void*)glIsProgram},
    {"glIsRenderbuffer",    (void*)glIsRenderbuffer},
    {"glIsShader",          (void*)glIsShader},
    {"glIsTexture",         (void*)glIsTexture},
    {"glLineWidth",         (void*)glLineWidth},
    {"glLinkProgram",       (void*)glLinkProgram},
    {"glPixelStorei",       (void*)glPixelStorei},
    {"glPolygonOffset",     (void*)glPolygonOffset},
    {"glReadPixels",        (void*)glReadPixels},
    {"glReleaseShaderCompiler",(void*)glReleaseShaderCompiler},
    {"glRenderbufferStorage",(void*)glRenderbufferStorage},
    {"glSampleCoverage",    (void*)glSampleCoverage},
    {"glScissor",           (void*)w_glScissor},
    {"glShaderBinary",      (void*)glShaderBinary},
    {"glShaderSource",      (void*)glShaderSource},
    {"glStencilFunc",       (void*)glStencilFunc},
    {"glStencilFuncSeparate",(void*)glStencilFuncSeparate},
    {"glStencilMask",       (void*)glStencilMask},
    {"glStencilMaskSeparate",(void*)glStencilMaskSeparate},
    {"glStencilOp",         (void*)glStencilOp},
    {"glStencilOpSeparate", (void*)glStencilOpSeparate},
    {"glTexImage2D",        (void*)glTexImage2D},
    {"glTexParameterf",     (void*)glTexParameterf},
    {"glTexParameterfv",    (void*)glTexParameterfv},
    {"glTexParameteri",     (void*)glTexParameteri},
    {"glTexParameteriv",    (void*)glTexParameteriv},
    {"glTexSubImage2D",     (void*)glTexSubImage2D},
    {"glUniform1f",         (void*)glUniform1f},
    {"glUniform1fv",        (void*)glUniform1fv},
    {"glUniform1i",         (void*)glUniform1i},
    {"glUniform1iv",        (void*)glUniform1iv},
    {"glUniform2f",         (void*)glUniform2f},
    {"glUniform2fv",        (void*)glUniform2fv},
    {"glUniform2i",         (void*)glUniform2i},
    {"glUniform2iv",        (void*)glUniform2iv},
    {"glUniform3f",         (void*)glUniform3f},
    {"glUniform3fv",        (void*)glUniform3fv},
    {"glUniform3i",         (void*)glUniform3i},
    {"glUniform3iv",        (void*)glUniform3iv},
    {"glUniform4f",         (void*)glUniform4f},
    {"glUniform4fv",        (void*)glUniform4fv},
    {"glUniform4i",         (void*)glUniform4i},
    {"glUniform4iv",        (void*)glUniform4iv},
    {"glUniformMatrix2fv",  (void*)glUniformMatrix2fv},
    {"glUniformMatrix3fv",  (void*)glUniformMatrix3fv},
    {"glUniformMatrix4fv",  (void*)glUniformMatrix4fv},
    {"glUseProgram",        (void*)glUseProgram},
    {"glValidateProgram",   (void*)glValidateProgram},
    {"glVertexAttrib1f",    (void*)glVertexAttrib1f},
    {"glVertexAttrib2f",    (void*)glVertexAttrib2f},
    {"glVertexAttrib3f",    (void*)glVertexAttrib3f},
    {"glVertexAttrib4f",    (void*)glVertexAttrib4f},
    {"glVertexAttrib1fv",   (void*)glVertexAttrib1fv},
    {"glVertexAttrib2fv",   (void*)glVertexAttrib2fv},
    {"glVertexAttrib3fv",   (void*)glVertexAttrib3fv},
    {"glVertexAttrib4fv",   (void*)glVertexAttrib4fv},
    {"glVertexAttribPointer",(void*)glVertexAttribPointer},
    {"glViewport",          (void*)w_glViewport},

    // ── GLES 3 passthrough ────────────────────────────────────────────────────
    {"glBeginQuery",           (void*)glBeginQuery},
    {"glBeginTransformFeedback",(void*)glBeginTransformFeedback},
    {"glBindBufferBase",       (void*)glBindBufferBase},
    {"glBindBufferRange",      (void*)glBindBufferRange},
    {"glBindSampler",          (void*)glBindSampler},
    {"glBindTransformFeedback",(void*)glBindTransformFeedback},
    {"glBindVertexArray",      (void*)glBindVertexArray},
    {"glBlitFramebuffer",      (void*)glBlitFramebuffer},
    {"glCopyBufferSubData",    (void*)glCopyBufferSubData},
    {"glDeleteQueries",        (void*)glDeleteQueries},
    {"glDeleteSamplers",       (void*)glDeleteSamplers},
    {"glDeleteSync",           (void*)glDeleteSync},
    {"glDeleteTransformFeedbacks",(void*)glDeleteTransformFeedbacks},
    {"glDeleteVertexArrays",   (void*)glDeleteVertexArrays},
    {"glDrawArraysInstanced",  (void*)glDrawArraysInstanced},
    {"glDrawBuffers",          (void*)glDrawBuffers},
    {"glDrawElementsInstanced",(void*)glDrawElementsInstanced},
    {"glEndQuery",             (void*)glEndQuery},
    {"glEndTransformFeedback", (void*)glEndTransformFeedback},
    {"glFenceSync",            (void*)glFenceSync},
    {"glFlushMappedBufferRange",(void*)glFlushMappedBufferRange},
    {"glFramebufferTextureLayer",(void*)glFramebufferTextureLayer},
    {"glGenQueries",           (void*)glGenQueries},
    {"glGenSamplers",          (void*)glGenSamplers},
    {"glGenTransformFeedbacks",(void*)glGenTransformFeedbacks},
    {"glGenVertexArrays",      (void*)glGenVertexArrays},
    {"glGetActiveUniformBlockiv",(void*)glGetActiveUniformBlockiv},
    {"glGetActiveUniformBlockName",(void*)glGetActiveUniformBlockName},
    {"glGetActiveUniformsiv",  (void*)glGetActiveUniformsiv},
    {"glGetInteger64v",        (void*)glGetInteger64v},
    {"glGetIntegeri_v",        (void*)glGetIntegeri_v},
    {"glGetFragDataLocation",  (void*)glGetFragDataLocation},
    {"glGetInternalformativ",  (void*)glGetInternalformativ},
    {"glGetStringi",           (void*)glGetStringi},
    {"glGetUniformBlockIndex", (void*)glGetUniformBlockIndex},
    {"glGetUniformuiv",        (void*)glGetUniformuiv},
    {"glInvalidateFramebuffer",(void*)glInvalidateFramebuffer},
    {"glIsQuery",              (void*)glIsQuery},
    {"glIsSampler",            (void*)glIsSampler},
    {"glIsSync",               (void*)glIsSync},
    {"glIsTransformFeedback",  (void*)glIsTransformFeedback},
    {"glIsVertexArray",        (void*)glIsVertexArray},
    {"glMapBufferRange",       (void*)glMapBufferRange},
    {"glPauseTransformFeedback",(void*)glPauseTransformFeedback},
    {"glReadBuffer",           (void*)glReadBuffer},
    {"glRenderbufferStorageMultisample",(void*)glRenderbufferStorageMultisample},
    {"glResumeTransformFeedback",(void*)glResumeTransformFeedback},
    {"glSamplerParameterf",    (void*)glSamplerParameterf},
    {"glSamplerParameterfv",   (void*)glSamplerParameterfv},
    {"glSamplerParameteri",    (void*)glSamplerParameteri},
    {"glSamplerParameteriv",   (void*)glSamplerParameteriv},
    {"glTexImage3D",           (void*)glTexImage3D},
    {"glTexStorage2D",         (void*)glTexStorage2D},
    {"glTexStorage3D",         (void*)glTexStorage3D},
    {"glTexSubImage3D",        (void*)glTexSubImage3D},
    {"glTransformFeedbackVaryings",(void*)glTransformFeedbackVaryings},
    {"glUniform1ui",           (void*)glUniform1ui},
    {"glUniform1uiv",          (void*)glUniform1uiv},
    {"glUniform2ui",           (void*)glUniform2ui},
    {"glUniform2uiv",          (void*)glUniform2uiv},
    {"glUniform3ui",           (void*)glUniform3ui},
    {"glUniform3uiv",          (void*)glUniform3uiv},
    {"glUniform4ui",           (void*)glUniform4ui},
    {"glUniform4uiv",          (void*)glUniform4uiv},
    {"glUniformBlockBinding",  (void*)glUniformBlockBinding},
    {"glUniformMatrix2x3fv",   (void*)glUniformMatrix2x3fv},
    {"glUniformMatrix2x4fv",   (void*)glUniformMatrix2x4fv},
    {"glUniformMatrix3x2fv",   (void*)glUniformMatrix3x2fv},
    {"glUniformMatrix3x4fv",   (void*)glUniformMatrix3x4fv},
    {"glUniformMatrix4x2fv",   (void*)glUniformMatrix4x2fv},
    {"glUniformMatrix4x3fv",   (void*)glUniformMatrix4x3fv},
    {"glUnmapBuffer",          (void*)glUnmapBuffer},
    {"glVertexAttribDivisor",  (void*)glVertexAttribDivisor},
    {"glVertexAttribI4i",      (void*)glVertexAttribI4i},
    {"glVertexAttribI4iv",     (void*)glVertexAttribI4iv},
    {"glVertexAttribI4ui",     (void*)glVertexAttribI4ui},
    {"glVertexAttribI4uiv",    (void*)glVertexAttribI4uiv},
    {"glVertexAttribIPointer", (void*)glVertexAttribIPointer},
    {"glWaitSync",             (void*)glWaitSync},
    {"glClientWaitSync",       (void*)glClientWaitSync},
    {"glProgramBinary",        (void*)glProgramBinary},
    {"glProgramParameteri",    (void*)glProgramParameteri},
    {"glGetProgramBinary",     (void*)glGetProgramBinary},
    {"glGetBufferPointerv",    (void*)glGetBufferPointerv},

    // ── ctype passthrough ────────────────────────────────────────────────────
    {"toupper",  (void*)toupper},
    {"tolower",  (void*)tolower},
    {"isalnum",  (void*)isalnum},
    {"isalpha",  (void*)isalpha},
    {"islower",  (void*)islower},
    {"isupper",  (void*)isupper},
    {"isdigit",  (void*)isdigit},
    {"isspace",  (void*)isspace},
    {"isprint",  (void*)isprint},
    {"iscntrl",  (void*)iscntrl},
    {"ispunct",  (void*)ispunct},
    {"isblank",  (void*)isblank},
    {"isxdigit", (void*)isxdigit},

    // ── stdio extras ─────────────────────────────────────────────────────────
    {"puts",      (void*)puts},
    {"putchar",   (void*)putchar},
    {"vsscanf",   (void*)vsscanf},
    {"vasprintf", (void*)stub_vasprintf},

    // ── string extras ────────────────────────────────────────────────────────
    {"stpcpy",    (void*)stub_stpcpy},
    {"strpbrk",   (void*)strpbrk},
    {"strcoll",   (void*)strcoll},
    {"strxfrm",   (void*)strxfrm},
    {"strerror",  (void*)strerror},
    {"strerror_r",(void*)_strerror_r},
    {"strtold",   (void*)strtold},

    // ── file I/O extras ──────────────────────────────────────────────────────
    {"rename",  (void*)stub_rename},
    {"remove",  (void*)stub_remove},
    {"getcwd",  (void*)stub_getcwd},
    {"fcntl",   (void*)stub_fcntl_sock},

    // ── time ─────────────────────────────────────────────────────────────────
    {"clock_gettime", (void*)clock_gettime},
    {"nanosleep",     (void*)nanosleep},
    {"gettimeofday",  (void*)gettimeofday},
    {"gmtime",        (void*)gmtime},
    {"localtime",     (void*)localtime},
    {"mktime",        (void*)mktime},
    {"strftime",      (void*)strftime},

    // ── POSIX semaphores ─────────────────────────────────────────────────────
    {"sem_init",    (void*)stub_sem_init},
    {"sem_destroy", (void*)stub_sem_destroy},
    {"sem_post",    (void*)stub_sem_post},
    {"sem_wait",    (void*)stub_sem_wait},
    {"sem_trywait", (void*)stub_sem_trywait},

    // ── pthread_mutexattr ────────────────────────────────────────────────────
    {"pthread_mutexattr_init",    (void*)pt_mattr_init},
    {"pthread_mutexattr_destroy", (void*)pt_mattr_destroy},
    {"pthread_mutexattr_settype", (void*)pt_mattr_settype},

    // ── setjmp / longjmp (ARM64: real functions, not macros) ─────────────────
    {"setjmp",  (void*)setjmp},
    {"longjmp", (void*)longjmp},

    // ── wide char (wchar.h / wctype.h) ───────────────────────────────────────
    {"wcslen",    (void*)wcslen},
    {"wcscpy",    (void*)wcscpy},
    {"wcsncpy",   (void*)wcsncpy},
    {"wcscat",    (void*)wcscat},
    {"wcsncat",   (void*)wcsncat},
    {"wcscmp",    (void*)wcscmp},
    {"wcsncmp",   (void*)wcsncmp},
    {"wcschr",    (void*)wcschr},
    {"wcsrchr",   (void*)wcsrchr},
    {"wcsstr",    (void*)wcsstr},
    {"wcstol",    (void*)wcstol},
    {"wcstoul",   (void*)wcstoul},
    {"wcstoll",   (void*)wcstoll},
    {"wcstoull",  (void*)wcstoull},
    {"wcstod",    (void*)wcstod},
    {"wcstof",    (void*)wcstof},
    {"wcstold",   (void*)wcstold},
    {"wcscoll",   (void*)wcscoll},
    {"wcsxfrm",   (void*)wcsxfrm},
    {"wcsrtombs", (void*)wcsrtombs},
    {"mbsrtowcs", (void*)mbsrtowcs},
    {"wcsnrtombs",(void*)stub_wcsnrtombs},
    {"mbsnrtowcs",(void*)stub_mbsnrtowcs},
    {"wcrtomb",   (void*)wcrtomb},
    {"mbtowc",    (void*)mbtowc},
    {"mbrtowc",   (void*)mbrtowc},
    {"mbrlen",    (void*)mbrlen},
    {"wctob",     (void*)wctob},
    {"btowc",     (void*)btowc},
    {"wmemcpy",   (void*)wmemcpy},
    {"wmemmove",  (void*)wmemmove},
    {"wmemset",   (void*)wmemset},
    {"wmemcmp",   (void*)wmemcmp},
    {"wmemchr",   (void*)wmemchr},
    {"swprintf",  (void*)swprintf},
    {"towupper",  (void*)towupper},
    {"towlower",  (void*)towlower},
    {"iswupper",  (void*)iswupper},
    {"iswlower",  (void*)iswlower},
    {"iswdigit",  (void*)iswdigit},
    {"iswalpha",  (void*)iswalpha},
    {"iswspace",  (void*)iswspace},
    {"iswpunct",  (void*)iswpunct},
    {"iswcntrl",  (void*)iswcntrl},
    {"iswprint",  (void*)iswprint},
    {"iswblank",  (void*)iswblank},
    {"iswxdigit", (void*)iswxdigit},
    {"iswgraph",  (void*)iswgraph},
    {"iswascii",  (void*)+[](wint_t c) -> int { return (unsigned)c <= 127; }},

    // ── locale ───────────────────────────────────────────────────────────────
    {"setlocale",              (void*)setlocale},
    {"localeconv",             (void*)localeconv},
    {"newlocale",              (void*)stub_newlocale},
    {"freelocale",             (void*)stub_freelocale},
    {"uselocale",              (void*)stub_uselocale},
    {"mbstowcs",               (void*)stub_mbstowcs},
    {"__ctype_get_mb_cur_max", (void*)stub_mb_cur_max},

    // ── strtod locale variants ───────────────────────────────────────────────
    {"strtoll_l",  (void*)stub_strtoll_l},
    {"strtoull_l", (void*)stub_strtoull_l},
    {"strtold_l",  (void*)stub_strtold_l},

    // ── networking (all stubbed — no sockets without bsd service) ────────────
    {"socket",       (void*)stub_socket},
    {"bind",         (void*)stub_bind},
    {"connect",      (void*)stub_connect},
    {"listen",       (void*)stub_listen},
    {"select",       (void*)stub_select},
    {"recv",         (void*)stub_recv},
    {"send",         (void*)stub_send},
    {"getsockname",  (void*)stub_getsockname},
    {"getsockopt",   (void*)stub_getsockopt},
    {"gethostbyname",(void*)stub_gethostbyname},
    {"__get_h_errno",(void*)stub_get_h_errno},

    // ── scheduling / system ──────────────────────────────────────────────────
    {"sched_yield",     (void*)stub_sched_yield},
    {"sysconf",         (void*)stub_sysconf},

    // ── ARM32 EABI helpers (armeabi-v7a only) ────────────────────────────────
    {"__aeabi_memcpy",   (void*)ae_memcpy},  {"__aeabi_memcpy4",  (void*)ae_memcpy},
    {"__aeabi_memcpy8",  (void*)ae_memcpy},  {"__aeabi_memmove",  (void*)ae_memmove},
    {"__aeabi_memmove4", (void*)ae_memmove}, {"__aeabi_memmove8", (void*)ae_memmove},
    {"__aeabi_memset",   (void*)ae_memset},  {"__aeabi_memset4",  (void*)ae_memset},
    {"__aeabi_memset8",  (void*)ae_memset},  {"__aeabi_memclr",   (void*)ae_memclr},
    {"__aeabi_memclr4",  (void*)ae_memclr},  {"__aeabi_memclr8",  (void*)ae_memclr},

    // ── OpenSL ES: fail the engine so cocos2d-x uses its Java audio path ─────
    {"slCreateEngine",                  (void*)sl_createEngine},
    {"SL_IID_NULL",                     (void*)kSlIidStorage},
    {"SL_IID_ENGINE",                   (void*)kSlIidStorage},
    {"SL_IID_PLAY",                     (void*)kSlIidStorage},
    {"SL_IID_SEEK",                     (void*)kSlIidStorage},
    {"SL_IID_VOLUME",                   (void*)kSlIidStorage},
    {"SL_IID_PREFETCHSTATUS",           (void*)kSlIidStorage},
    {"SL_IID_METADATAEXTRACTION",       (void*)kSlIidStorage},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void*)kSlIidStorage},
    {"SL_IID_ANDROIDCONFIGURATION", (void*)kSlIidStorage},
    {"SL_IID_RECORD",               (void*)kSlIidStorage},

    // ── assorted libc the 32-bit build reaches for ───────────────────────────
    {"lround",   (void*)stub_lround},   {"lroundf",  (void*)stub_lroundf},
    {"llround",  (void*)stub_llround},  {"remainderf", (void*)stub_remainderf},
    {"erff",     (void*)stub_erff},     {"erfcf",    (void*)stub_erfcf},
    {"memmem",   (void*)stub_memmem},   {"inet_ntoa", (void*)stub_inet_ntoa},
    {"gai_strerror", (void*)stub_gai_strerror},
    {"getprotobyname", (void*)stub_getprotobyname},
    {"mlock",    (void*)stub_mlock},    {"pathconf", (void*)stub_pathconf},
    {"openat",   (void*)stub_openat},   {"unlinkat", (void*)stub_unlinkat},
    {"utimensat",(void*)stub_utimensat},{"fchmodat", (void*)stub_fchmodat},
    {"__assert2",(void*)stub_assert2},
    {"__android_log_assert", (void*)stub_android_log_assert},
    {"__FD_CLR_chk", (void*)stub_FD_CLR_chk},
    {"dl_unwind_find_exidx", (void*)stub_dl_unwind_find_exidx},
    {"freopen",  (void*)freopen},       {"fdopendir", (void*)stub_fdopendir},
    {"statvfs",  (void*)statvfs},
    {"fputwc",   (void*)fputwc},        {"getwc",    (void*)getwc},
    {"ungetwc",  (void*)ungetwc},
    {"sigsetjmp",(void*)stub_sigsetjmp}, {"siglongjmp", (void*)stub_siglongjmp},
    {"glMapBufferOES",   (void*)stub_glMapBufferOES},
    {"glUnmapBufferOES", (void*)stub_glUnmapBufferOES},

    // ── remaining armeabi-v7a imports ────────────────────────────────────────
    {"AAsset_openFileDescriptor", (void*)stub_AAsset_openFileDescriptor},
    {"__fgets_chk",   (void*)stub_fgets_chk},   {"__strncpy_chk", (void*)stub_strncpy_chk},
    {"__strrchr_chk", (void*)stub_strrchr_chk},
    {"__progname",    (void*)&g_progname},      {"getprogname",   (void*)stub_getprogname},
    {"arc4random_buf",(void*)stub_arc4random_buf},
    {"arc4random_uniform", (void*)stub_arc4random_uniform},
    {"asprintf",      (void*)stub_asprintf},    {"vsyslog",       (void*)stub_vsyslog},
    {"strndup",       (void*)stub_strndup},     {"setbuf",        (void*)stub_setbuf},
    {"pause",         (void*)stub_pause},       {"cacheflush",    (void*)stub_cacheflush},
    {"epoll_create",  (void*)stub_epoll_create},{"epoll_create1", (void*)stub_epoll_create},
    {"epoll_ctl",     (void*)stub_epoll_ctl},   {"epoll_wait",    (void*)stub_epoll_wait},
    {"inotify_rm_watch", (void*)stub_inotify_rm_watch},
    {"tgkill",        (void*)stub_tgkill},      {"timegm",        (void*)stub_timegm},
    {"execv",         (void*)stub_execv},       {"execvpe",       (void*)stub_execvpe},
    {"getppid",       (void*)stub_getppid},     {"wait4",         (void*)stub_wait4},
    {"lrint",         (void*)stub_lrint},
    {"strtoimax",     (void*)stub_strtoimax},   {"strtoumax",     (void*)stub_strtoumax},
    {"__gnu_Unwind_Find_exidx", (void*)stub_dl_unwind_find_exidx},
    {"_toupper_tab_", (void*)&_ctype_},
    {"syscall",         (void*)stub_syscall},
    {"getentropy",      (void*)stub_getentropy},
    {"getrandom",       (void*)stub_getrandom},
    {"dl_iterate_phdr", (void*)stub_dl_iterate_phdr},
    {"getpid",          (void*)stub_getpid},
    {"getuid",          (void*)stub_getuid},
    {"getgid",          (void*)stub_getgid},
    {"gettid",          (void*)stub_gettid},
    {"prctl",           (void*)stub_prctl},
    {"access",          (void*)stub_access},
    {"chmod",           (void*)stub_chmod},
    {"fchmod",          (void*)stub_fchmod},
    {"lstat",           (void*)stub_lstat},
    {"mprotect",        (void*)stub_mprotect},
    {"mmap",            (void*)stub_mmap},
    {"munmap",          (void*)stub_munmap},
    {"pipe",            (void*)stub_pipe},
    {"dup",             (void*)stub_dup},
    {"dup2",            (void*)stub_dup2},
    {"ioctl",           (void*)stub_ioctl},
    {"getauxval",       (void*)stub_getauxval},
    {"sleep",           (void*)stub_sleep},
    {"usleep",          (void*)stub_usleep},
    {"clock_nanosleep", (void*)stub_clock_nanosleep},
    {"strtod_l",        (void*)stub_strtod_l},
    {"strtof_l",        (void*)stub_strtof_l},
    {"__register_atfork",(void*)stub_register_atfork},

    // ── signals (crash reporters install SIGSEGV/SIGBUS handlers) ────────────
    {"sigaction",       (void*)stub_sigaction},
    {"sigemptyset",     (void*)stub_sigemptyset},
    {"sigfillset",      (void*)stub_sigfillset},
    {"sigaddset",       (void*)stub_sigaddset},
    {"sigdelset",       (void*)stub_sigdelset},
    {"sigismember",     (void*)stub_sigismember},
    {"kill",            (void*)stub_kill},
    {"raise",           (void*)stub_raise},
    {"pthread_kill",    (void*)stub_pthread_kill},
    {"sigprocmask",     (void*)stub_sigprocmask},
    {"pthread_sigmask", (void*)stub_pthread_sigmask},

    // ── pthread extras ───────────────────────────────────────────────────────
    {"pthread_setname_np",            (void*)stub_pthread_setname_np},
    {"pthread_getname_np",            (void*)stub_pthread_getname_np},
    {"pthread_attr_setstack",         (void*)stub_pthread_attr_setstack},
    {"pthread_attr_getstack",         (void*)stub_pthread_attr_getstack},
    {"pthread_attr_setschedpolicy",   (void*)stub_pthread_attr_setschedpolicy},
    {"pthread_attr_setschedparam",    (void*)stub_pthread_attr_setschedparam},
    {"pthread_attr_getschedparam",    (void*)stub_pthread_attr_getschedparam},
    {"pthread_barrier_init",          (void*)stub_pthread_barrier_init},
    {"pthread_barrier_wait",          (void*)stub_pthread_barrier_wait},
    {"pthread_barrier_destroy",       (void*)stub_pthread_barrier_destroy},

    // ── syslog ───────────────────────────────────────────────────────────────
    {"openlog",  (void*)stub_openlog},
    {"closelog", (void*)stub_closelog},
    {"syslog",   (void*)stub_syslog},

    // ── Android specifics ────────────────────────────────────────────────────
    {"android_set_abort_message", (void*)stub_android_abort_msg},

    // ── Bionic fortified string/IO wrappers ──────────────────────────────────
    {"__strlen_chk",    (void*)chk_strlen},
    {"__memcpy_chk",    (void*)chk_memcpy},
    {"__memmove_chk",   (void*)chk_memmove},
    {"__strcat_chk",    (void*)chk_strcat},
    {"__strncat_chk",   (void*)chk_strncat},
    {"__strcpy_chk",    (void*)chk_strcpy},
    {"__strncpy_chk2",  (void*)chk_strncpy},
    {"__vsprintf_chk",  (void*)chk_vsprintf},
    {"__vsnprintf_chk", (void*)chk_vsnprintf},
    {"__read_chk",      (void*)chk_read},
    {"__open_2",        (void*)chk_open2},

    // ── sincosf ──────────────────────────────────────────────────────────────
    {"sincosf", (void*)stub_sincosf},

    // ── data symbols (address of variable, not value) ─────────────────────────
    {"__stack_chk_guard", (void*)&__stack_chk_guard},
    {"__sF",              (void*)g_fake_sF},

    // ── C++ runtime extras ───────────────────────────────────────────────────
    {"__cxa_finalize", (void*)+[](void*) {}},

    // ── Batch 3 — unresolved from 2026-06-30 run ────────────────────────────
    // crash reporter / signal handling
    {"dladdr",          (void*)stub_dladdr},
    {"sigaltstack",     (void*)stub_sigaltstack},
    {"signal",          (void*)signal},
    {"strsignal",       (void*)stub_strsignal},
    {"sys_signame",     (void*)g_sys_signames},
    // process info / environment
    {"environ",         (void*)&environ},
    {"fork",            (void*)stub_fork},
    {"execve",          (void*)stub_execve},
    {"waitpid",         (void*)stub_waitpid},
    {"_exit",           (void*)sh_exit_raw},
    {"setuid",          (void*)stub_setuid},
    {"setgid",          (void*)stub_setgid},
    // filesystem
    {"chdir",           (void*)stub_chdir},
    {"realpath",        (void*)stub_realpath},
    {"readlink",        (void*)stub_readlink},
    {"symlink",         (void*)stub_symlink},
    {"utimes",          (void*)stub_utimes},
    {"isatty",          (void*)stub_isatty},
    {"tmpfile",         (void*)stub_tmpfile},
    // network / polling
    {"accept",          (void*)stub_accept},
    {"setsockopt",      (void*)stub_setsockopt},
    {"poll",            (void*)stub_poll},
    // stdio extras
    {"clearerr",        (void*)stub_clearerr},
    {"fileno",          (void*)stub_fileno},
    {"fdopen",          (void*)stub_fdopen},
    {"popen",           (void*)stub_popen},
    {"pclose",          (void*)stub_pclose},
    // terminal
    {"tcgetattr",       (void*)stub_tcgetattr},
    {"tcsetattr",       (void*)stub_tcsetattr},
    // time
    {"gmtime_r",        (void*)stub_gmtime_r},
    {"localtime_r",     (void*)stub_localtime_r},
    {"difftime",        (void*)stub_difftime},
    {"strptime",        (void*)stub_strptime},
    {"fesetround",      (void*)stub_fesetround},
    // math
    {"acosh",           (void*)stub_acosh},
    {"asinh",           (void*)stub_asinh},
    {"atanh",           (void*)stub_atanh},
    {"log1p",           (void*)stub_log1p},
    {"expm1",           (void*)stub_expm1},
    // memory
    {"malloc_usable_size", (void*)stub_malloc_usable_size},
    // Bionic fortified wrappers
    {"__memset_chk",    (void*)stub_memset_chk},
    {"__strchr_chk",    (void*)stub_strchr_chk},
    {"__FD_SET_chk",    (void*)stub___FD_SET_chk},
    {"__FD_ISSET_chk",  (void*)stub___FD_ISSET_chk},
    // locale-variant char classification
    {"isdigit_l",       (void*)stub_isdigit_l},
    {"islower_l",       (void*)stub_islower_l},
    {"isupper_l",       (void*)stub_isupper_l},
    {"isxdigit_l",      (void*)stub_isxdigit_l},
    {"tolower_l",       (void*)stub_tolower_l},
    {"toupper_l",       (void*)stub_toupper_l},
    {"iswalpha_l",      (void*)stub_iswalpha_l},
    {"iswblank_l",      (void*)stub_iswblank_l},
    {"iswcntrl_l",      (void*)stub_iswcntrl_l},
    {"iswdigit_l",      (void*)stub_iswdigit_l},
    {"iswlower_l",      (void*)stub_iswlower_l},
    {"iswprint_l",      (void*)stub_iswprint_l},
    {"iswpunct_l",      (void*)stub_iswpunct_l},
    {"iswspace_l",      (void*)stub_iswspace_l},
    {"iswupper_l",      (void*)stub_iswupper_l},
    {"iswxdigit_l",     (void*)stub_iswxdigit_l},
    {"towlower_l",      (void*)stub_towlower_l},
    {"towupper_l",      (void*)stub_towupper_l},
    {"strcoll_l",       (void*)stub_strcoll_l},
    {"strxfrm_l",       (void*)stub_strxfrm_l},
    {"strftime_l",      (void*)stub_strftime_l},
    {"wcscoll_l",       (void*)stub_wcscoll_l},
    {"wcsxfrm_l",       (void*)stub_wcsxfrm_l},
    // string extras that newlib provides but weren't forwarded
    {"strspn",          (void*)strspn},
    {"sched_get_priority_min", (void*)stub_sched_get_priority_min},
    {"sched_get_priority_max", (void*)stub_sched_get_priority_max},
    {"__readlink_chk",         (void*)stub___readlink_chk},

    // sentinel
    // Desktop OpenGL 1.x/2.1 entry points used by Far Cry's XRenderOGL.
    {"glActiveStencilFaceEXT", (void*)shim_glActiveStencilFaceEXT},
    {"glAlphaFunc", (void*)glAlphaFunc},
    {"glAreTexturesResident", (void*)glAreTexturesResident},
    {"glBegin", (void*)glBegin},
    {"glBindBufferARB", (void*)shim_glBindBufferARB},
    {"glBufferDataARB", (void*)shim_glBufferDataARB},
    {"glBufferSubDataARB", (void*)shim_glBufferSubDataARB},
    {"glBindProgramARB", (void*)shim_glBindProgramARB},
    {"glDeleteProgramsARB", (void*)shim_glDeleteProgramsARB},
    {"glGenProgramsARB", (void*)shim_glGenProgramsARB},
    {"glGetProgramEnvParameterdvARB", (void*)shim_glGetProgramEnvParameterdvARB},
    {"glGetProgramEnvParameterfvARB", (void*)shim_glGetProgramEnvParameterfvARB},
    {"glGetProgramLocalParameterdvARB", (void*)shim_glGetProgramLocalParameterdvARB},
    {"glGetProgramLocalParameterfvARB", (void*)shim_glGetProgramLocalParameterfvARB},
    {"glGetProgramStringARB", (void*)shim_glGetProgramStringARB},
    {"glGetProgramivARB", (void*)shim_glGetProgramivARB},
    {"glIsProgramARB", (void*)shim_glIsProgramARB},
    {"glProgramEnvParameter4dARB", (void*)shim_glProgramEnvParameter4dARB},
    {"glProgramEnvParameter4dvARB", (void*)shim_glProgramEnvParameter4dvARB},
    {"glProgramEnvParameter4fARB", (void*)shim_glProgramEnvParameter4fARB},
    {"glProgramEnvParameter4fvARB", (void*)shim_glProgramEnvParameter4fvARB},
    {"glProgramLocalParameter4dARB", (void*)shim_glProgramLocalParameter4dARB},
    {"glProgramLocalParameter4dvARB", (void*)shim_glProgramLocalParameter4dvARB},
    {"glProgramLocalParameter4fARB", (void*)shim_glProgramLocalParameter4fARB},
    {"glProgramLocalParameter4fvARB", (void*)shim_glProgramLocalParameter4fvARB},
    {"glProgramStringARB", (void*)shim_glProgramStringARB},
    {"glVertexAttribPointerARB", (void*)shim_glVertexAttribPointerARB},
    {"glEnableVertexAttribArrayARB", (void*)shim_glEnableVertexAttribArrayARB},
    {"glDisableVertexAttribArrayARB", (void*)shim_glDisableVertexAttribArrayARB},
    {"glClearDepth", (void*)glClearDepth},
    {"glClipPlane", (void*)glClipPlane},
    {"glColor3f", (void*)glColor3f},
    {"glColor3fv", (void*)glColor3fv},
    {"glColor4f", (void*)glColor4f},
    {"glColor4fv", (void*)glColor4fv},
    {"glColorPointer", (void*)glColorPointer},
    {"glColorTableEXT", (void*)shim_glColorTableEXT},
    {"glCompressedTexImage2DARB", (void*)shim_glCompressedTexImage2DARB},
    {"glCompressedTexSubImage2DARB", (void*)shim_glCompressedTexSubImage2DARB},
    {"glDepthRange", (void*)glDepthRange},
    {"glDisableClientState", (void*)glDisableClientState},
    {"glDrawBuffer", (void*)glDrawBuffer},
    {"glEnableClientState", (void*)glEnableClientState},
    {"glEnd", (void*)glEnd},
    {"glFinishFenceNV", (void*)shim_glFinishFenceNV},
    {"glFogf", (void*)glFogf},
    {"glFogfv", (void*)glFogfv},
    {"glFogi", (void*)glFogi},
    {"glGenBuffersARB", (void*)shim_glGenBuffersARB},
    {"glGenFencesNV", (void*)shim_glGenFencesNV},
    {"glGetCompressedTexImageARB", (void*)shim_glGetCompressedTexImageARB},
    {"glGetDoublev", (void*)glGetDoublev},
    {"glGetTexImage", (void*)glGetTexImage},
    {"glGetTexLevelParameteriv", (void*)glGetTexLevelParameteriv},
    {"glLightModelfv", (void*)glLightModelfv},
    {"glLightModeli", (void*)glLightModeli},
    {"glLightf", (void*)glLightf},
    {"glLightfv", (void*)glLightfv},
    {"glLoadIdentity", (void*)glLoadIdentity},
    {"glLoadMatrixf", (void*)glLoadMatrixf},
    {"glMaterialf", (void*)glMaterialf},
    {"glMaterialfv", (void*)glMaterialfv},
    {"glMatrixMode", (void*)glMatrixMode},
    {"glMultMatrixf", (void*)glMultMatrixf},
    {"glNormalPointer", (void*)glNormalPointer},
    {"glOrtho", (void*)glOrtho},
    {"glPointSize", (void*)glPointSize},
    {"glPolygonMode", (void*)glPolygonMode},
    {"glPopMatrix", (void*)glPopMatrix},
    {"glPushMatrix", (void*)glPushMatrix},
    {"glRotatef", (void*)glRotatef},
    {"glScalef", (void*)glScalef},
    {"glSetFenceNV", (void*)shim_glSetFenceNV},
    {"glShadeModel", (void*)glShadeModel},
    {"glStencilFuncSeparateATI", (void*)shim_glStencilFuncSeparateATI},
    {"glStencilOpSeparateATI", (void*)shim_glStencilOpSeparateATI},
    {"glTestFenceNV", (void*)shim_glTestFenceNV},
    {"glTexCoord2f", (void*)glTexCoord2f},
    {"glTexCoord3f", (void*)glTexCoord3f},
    {"glTexCoordPointer", (void*)glTexCoordPointer},
    {"glTexEnvf", (void*)glTexEnvf},
    {"glTexEnvfv", (void*)glTexEnvfv},
    {"glTexEnvi", (void*)glTexEnvi},
    {"glTexGenf", (void*)glTexGenf},
    {"glTexGenfv", (void*)glTexGenfv},
    {"glTexGeni", (void*)glTexGeni},
    {"glTexImage3DEXT", (void*)shim_glTexImage3DEXT},
    {"glTranslatef", (void*)glTranslatef},
    {"glVertex2f", (void*)glVertex2f},
    {"glVertex2i", (void*)glVertex2i},
    {"glVertex3f", (void*)glVertex3f},
    {"glVertex3fv", (void*)glVertex3fv},
    {"glVertexPointer", (void*)glVertexPointer},
    {nullptr, nullptr}
};

// ── Unity / IL2CPP gap fillers — FALLBACK priority ────────────────────────────
// Resolved AFTER the game's own libraries (see resolveSymbol), NOT before like
// g_shims. This matters: a cocos2d-x game like Hill Climb Racing statically
// links its own libc (its own fscanf/_ctype_/div/…) and imports them as
// undefined; putting these in the override table shadowed the game's real libc
// with our stubs and broke it (HCR's driving physics regressed). Unity/IL2CPP
// import the very same names but ship no libc of their own, so for them the
// game-symbol lookup misses and these fill the gap. _ctype_ is special-cased in
// shimResolveFallback (its symbol address IS the classification table).
static const ShimEntry g_unity_fallback_shims[] = {
    {"strdup",          (void*)stub_strdup},
    {"strsep",          (void*)stub_strsep},
    {"strlcpy",         (void*)stub_strlcpy},
    {"__isnanf",        (void*)stub_isnanf},
    {"remainder",       (void*)stub_remainder},
    {"div",             (void*)stub_div},
    {"unlink",          (void*)stub_unlink},
    {"rmdir",           (void*)stub_rmdir},
    {"truncate",        (void*)stub_truncate},
    {"ftruncate",       (void*)stub_ftruncate},
    {"lseek64",         (void*)stub_lseek64},
    {"flock",           (void*)stub_flock},
    {"utime",           (void*)stub_utime},
    {"statfs",          (void*)stub_statfs},
    {"fscanf",          (void*)stub_fscanf},
    {"tcflush",         (void*)stub_tcflush},
    {"getpriority",     (void*)stub_getpriority},
    {"setpriority",     (void*)stub_setpriority},
    {"ptrace",          (void*)stub_ptrace},
    {"getpagesize",     (void*)stub_getpagesize},
    {"getpwuid",        (void*)stub_getpwuid},
    {"sigsuspend",      (void*)stub_sigsuspend},
    {"clock_getres",    (void*)stub_clock_getres},
    {"uname",           (void*)stub_uname},
    {"madvise",         (void*)stub_madvise},
    {"pthread_exit",            (void*)stub_pthread_exit},
    {"pthread_atfork",          (void*)stub_pthread_atfork},
    {"pthread_getattr_np",      (void*)stub_pthread_getattr_np},
    {"pthread_condattr_init",   (void*)stub_pthread_condattr_init},
    {"pthread_condattr_destroy",(void*)stub_pthread_condattr_destroy},
    {"pthread_condattr_setclock",(void*)stub_pthread_condattr_setclock},
    {"sem_getvalue",    (void*)stub_sem_getvalue},
    {"inet_addr",       (void*)stub_inet_addr},
    {"inet_ntop",       (void*)stub_inet_ntop},
    {"inet_pton",       (void*)stub_inet_pton},
    {"getaddrinfo",     (void*)stub_getaddrinfo},
    {"freeaddrinfo",    (void*)stub_freeaddrinfo},
    {"getnameinfo",     (void*)stub_getnameinfo},
    {"gethostname",     (void*)stub_gethostname},
    {"gethostbyaddr",   (void*)stub_gethostbyaddr},
    {"recvfrom",        (void*)stub_recvfrom},
    {"recvmsg",         (void*)stub_recvmsg},
    {"sendmsg",         (void*)stub_sendmsg},
    {"shutdown",        (void*)stub_shutdown},
    {"__system_property_get",   (void*)stub_system_property_get},
    {"ANativeWindow_fromSurface",(void*)stub_ANativeWindow_fromSurface},
    {"ASensorEventQueue_hasEvents",(void*)stub_ASensorEventQueue_hasEvents},
    {"__google_potentially_blocking_region_begin",(void*)stub_google_region},
    {"__google_potentially_blocking_region_end",  (void*)stub_google_region},
    {"wctype",          (void*)stub_wctype},
    {"iswctype",        (void*)stub_iswctype},
    {"wcsftime",        (void*)stub_wcsftime},
    {"writev",          (void*)stub_writev},
    {nullptr, nullptr}
};

static constexpr size_t NUM_SHIMS = sizeof(g_shims)/sizeof(g_shims[0]) - 1;

// ─── shimResolve — linear scan (fast enough for N < 400) ──────────────────────
void* shimResolve(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < NUM_SHIMS; i++) {
        if (strcmp(g_shims[i].name, name) == 0)
            return g_shims[i].ptr;
    }
    return nullptr;
}

// Fallback resolver — checked only AFTER the game's own libraries (see
// resolveSymbol). Holds the Unity/IL2CPP libc gap fillers, which must never
// shadow a game that ships its own copies of these symbols.
void* shimResolveFallback(const char* name) {
    if (!name) return nullptr;
    // Bionic defines _ctype_ as a data object of type "const char*".
    // Relocations therefore need the ADDRESS OF THE POINTER VARIABLE; code
    // first loads [GOT] -> &_ctype_, then loads [_ctype_] -> table address.
    // Returning the table directly makes that second load read the first eight
    // bytes of the table as a pointer (which was exactly our 0x202020... crash).
    if (strcmp(name, "_ctype_") == 0)
        return (void*)&g_ctype_base;
    for (size_t i = 0; g_unity_fallback_shims[i].name; i++) {
        if (strcmp(g_unity_fallback_shims[i].name, name) == 0)
            return g_unity_fallback_shims[i].ptr;
    }
    return nullptr;
}