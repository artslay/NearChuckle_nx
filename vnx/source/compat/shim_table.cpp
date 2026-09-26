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
#include <unordered_map>
#include <unordered_set>

extern void compatPollSwitchInput();

// Bink video audio bridge. These are the exported C entry points implemented
// in bink_audio_sdl.cpp; bind the guest CS_* imports to the SDL3-backed path.
extern "C" {
void* near_bink_cs_stream_create(void (*callback)(void), int length, unsigned int flags, int samplerate, void* userdata);
int near_bink_cs_stream_play(int channel, void* stream);
signed char near_bink_cs_stream_stop(void* stream);
signed char near_bink_cs_stream_close(void* stream);
void near_bink_cs_update(void);
}
extern void compatPakLog(const char* fmt, ...);

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
#include <memory>
#include <string>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <atomic>

// Diagnostic logging policy. Keep crash/loader/path failures visible, but suppress
// high-frequency allocator and successful realpath chatter that can drown the
// actual failure. Flip these back to true only for a focused memory investigation.
static constexpr bool kVerboseAllocatorLogs = false;

static std::string asciiLower(std::string value);
static int stub_clock_gettime(int clock_id, struct timespec* ts);

// Case-insensitive filesystem resolver used by file wrappers below.
static bool resolvePathCaseInsensitive(const char* input, std::string& resolved);

struct PakEntryMeta {
    uint32_t localOffset = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t method = 0;
    uint32_t expectedCrc = 0;
};
static bool pakFindVirtualEntry(const char* requested,
                                std::string& pakPath,
                                PakEntryMeta& meta);
static std::string pakNormalizeName(const char* name);
static std::string pakAssetRelativeName(const char* requested);
static int vpakFdOpen(const char* requested, int flags);
static bool vpakFdOwns(int fd);
static ssize_t vpakFdRead(int fd, void* dst, size_t count);
static off_t vpakFdSeek(int fd, off_t off, int whence);
static ssize_t vpakFdPread(int fd, void* dst, size_t count, off_t offset);
static int vpakFdClose(int fd);
static int vpakFdFstat(int fd, struct stat* st);
static bool pakReadEntryToMemory(const std::string& pakPath,
                                 const PakEntryMeta& meta,
                                 std::vector<unsigned char>& plain);
static bool pakGetMemory(
    const std::string& pakPath,
    const PakEntryMeta& meta,
    std::shared_ptr<std::vector<unsigned char>>& data,
    bool& cacheHit);
static bool pakVirtualDirectoryExists(const char* directory);
static bool isShaderCacheLookupPath(const char* path);
// These two Android intro movies are not shipped with the Switch game data.
// Treat a missing open as an optional asset so the video sequencer can continue.
// A real file with either name is still opened normally.
static bool isOptionalMissingVideoPath(const char* path) {
    if (!path || !*path)
        return false;

    std::string normalized = asciiLower(path);
    for (char& c : normalized) {
        if ((unsigned char)c == 92)
            c = '/';
    }

    const size_t slash = normalized.find_last_of('/');
    const std::string base = slash == std::string::npos
        ? normalized
        : normalized.substr(slash + 1);

    return base == "amd64.bik" ||
           base == "governmental_message.bik";
}

static bool isShaderPathForDiag(const char* path);
 
extern "C" {
volatile int g_near_video_open_failed = 0;
volatile uint32_t g_near_video_panel_finished_offset = 0xffffffffu;
}

extern "C" {
void* g_near_original_refstream_activate = nullptr;
void* g_near_refstream_on_io_complete = nullptr;
void* g_near_original_refstream_call_read = nullptr;
}

extern "C" bool compatGuestActivateReadStream(void* self) {
    if (!self)
        return false;

    // CRefReadStream layout on AArch64:
    //   +0x00 vptr
    //   +0x08 m_pEngine
    //   +0x10 std::string m_strFileName
    // The HANDLE follows the string. Both libc++ (24-byte string) and
    // libstdc++ (32-byte string) are handled by detecting INVALID_HANDLE_VALUE.
    const char* name = *reinterpret_cast<const char* const*>(
        reinterpret_cast<uint8_t*>(self) + 0x10);
    if (name && *name) {
        std::string pakPath;
        PakEntryMeta meta;
        if (pakFindVirtualEntry(name, pakPath, meta)) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(self);
            size_t handleOff = 0;

            // HANDLE is platform/configuration dependent here. On the
            // Android/libc++ layout used by the original game it may occupy
            // four bytes even though the surrounding object is AArch64, so
            // reading it only as UINTPTR_MAX (0xffffffffffffffff) misses the
            // actual INVALID_HANDLE_VALUE (0xffffffff).
            //
            // The known libc++ layouts are:
            //   m_hFile       +0x28
            //   m_nFileSize   +0x3c
            // or, with the wider string layout:
            //   m_hFile       +0x30
            //   m_nFileSize   +0x44
            const uint32_t h28_32 = *reinterpret_cast<const uint32_t*>(base + 0x28);
            const uint32_t h30_32 = *reinterpret_cast<const uint32_t*>(base + 0x30);
            const uint64_t h28_64 = *reinterpret_cast<const uint64_t*>(base + 0x28);
            const uint64_t h30_64 = *reinterpret_cast<const uint64_t*>(base + 0x30);

            if (h28_32 == 0xffffffffu || h28_64 == UINT64_MAX)
                handleOff = 0x28;
            else if (h30_32 == 0xffffffffu || h30_64 == UINT64_MAX)
                handleOff = 0x30;
            else {
                compatPakLog("PAK STREAM ACTIVATE: invalid HANDLE slot for %s (h28=%08x/%p h30=%08x/%p)",
                             name, h28_32, (void*)h28_64, h30_32, (void*)h30_64);
            }

            if (handleOff) {
                // m_nFileSize is immediately after HANDLE, CCachedFileDataPtr,
                // sector size: HANDLE+20 for both supported layouts.
                *reinterpret_cast<uint32_t*>(base + handleOff + 20) =
                    meta.uncompressedSize;                return true;
            }
        }
    }

    using ActivateFn = bool (*)(void*);
    ActivateFn original =
        reinterpret_cast<ActivateFn>(g_near_original_refstream_activate);
    if (original)
        return original(self);

    return false;
}

extern "C" uint32_t compatGuestCallReadFileEx(void* proxy) {
    if (!proxy)
        return 0xF0000008u;

    const uintptr_t base = reinterpret_cast<uintptr_t>(proxy);

    // CRefReadStreamProxy Android/libc++ object layout:
    //   +0x08 m_numRetries
    //   +0x10 m_pStream
    //   +0x18 m_Params (sizeof(StreamReadParams) == 0x30)
    //
    // StreamReadParams:
    //   +0x00 dwUserData
    //   +0x08 nPriority
    //   +0x0c nLoadTime
    //   +0x10 nMaxLoadTime
    //   +0x18 pBuffer
    //   +0x20 nOffset
    //   +0x24 nSize
    //   +0x28 nFlags
    //
    // Proxy fields after m_Params:
    //   +0x48 m_strClient
    //   +0x60 m_pCallback
    //   +0x68 m_pBuffer
    //   +0x70 m_numBytesRead
    //   +0x74 m_nPieceOffset
    //   +0x78 m_nPieceLength
    // IMPORTANT: m_Params.pBuffer (+0x30) is only the caller-supplied buffer.
    // StartRead() normally allocates the actual streaming buffer in m_pBuffer
    // (+0x68), and CallReadFileEx() reads into that field.
    void* stream = *reinterpret_cast<void**>(base + 0x10);
    void* buffer = *reinterpret_cast<void**>(base + 0x68);
    const uint32_t paramsOffset =
        *reinterpret_cast<const uint32_t*>(base + 0x38);
    const uint32_t paramsSize =
        *reinterpret_cast<const uint32_t*>(base + 0x3c);
    const uint32_t pieceOffset =
        *reinterpret_cast<const uint32_t*>(base + 0x74);
    const uint32_t pieceLength =
        *reinterpret_cast<const uint32_t*>(base + 0x78);

    auto complete = reinterpret_cast<void (*)(void*, uint32_t, uint32_t)>(
        g_near_refstream_on_io_complete);

    if (!stream || !buffer || !complete) {
        if (complete)
            complete(proxy, 0xF0000008u, 0);
        return 0;
    }

    const char* name = *reinterpret_cast<const char* const*>(
        reinterpret_cast<uint8_t*>(stream) + 0x10);
    std::string pakPath;
    PakEntryMeta meta;

    if (!name || !*name || !pakFindVirtualEntry(name, pakPath, meta)) {
        // Non-PAK streams keep the original Android/Linux implementation.
        using CallReadFn = uint32_t (*)(void*);
        CallReadFn original =
            reinterpret_cast<CallReadFn>(g_near_original_refstream_call_read);
        if (original)
            return original(proxy);
        complete(proxy, 0xF0000008u, 0);
        return 0;
    }

    std::shared_ptr<std::vector<unsigned char>> data;
    bool cacheHit = false;
    if (!pakGetMemory(pakPath, meta, data, cacheHit)) {
        complete(proxy, 0xF000000Cu, 0);
        return 0;
    }

    const uint64_t dataSize = data->size();
    const uint64_t srcOffset =
        (uint64_t)paramsOffset + (uint64_t)pieceOffset;
    if (srcOffset > dataSize ||
        (uint64_t)pieceLength > dataSize - srcOffset ||
        (uint64_t)paramsSize > dataSize ||
        (uint64_t)paramsOffset > dataSize) {
        complete(proxy, 0xF0000008u, 0);
        return 0;
    }

    std::memcpy(reinterpret_cast<unsigned char*>(buffer) + pieceOffset,
                data->data() + srcOffset,
                pieceLength);

    complete(proxy, 0, pieceLength);
    return 0;
}

extern "C" int compatVideoPanelIsPlaying(void* self) {
    if (g_near_video_open_failed)
        return 0;

    const uint32_t offset = g_near_video_panel_finished_offset;
    if (!self || offset == 0xffffffffu || offset >= 0x10000u)
        return 0;

    const uint8_t finished = *(const volatile uint8_t*)
        ((const uint8_t*)self + offset);
    return finished ? 0 : 1;
}

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
    if (vpakFdOwns(fd))
        return vpakFdPread(fd, buf, count, offset);

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
static bool vpakOwns(FILE* f);
static size_t vpakRead(FILE* f, void* dst, size_t size, size_t count);
static int vpakSeek(FILE* f, int64_t off, int whence);
static long long vpakTell64(FILE* f);
static int vpakGetc(FILE* f);
static int vpakEof(FILE* f);
static int vpakClose(FILE* f);

static long stub_ftello(FILE* f) {
    if (vpakOwns(f))
        return (long)vpakTell64(f);
    return ftell(f);
}
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
        memcpy(out, canonical.c_str(), canonical.size() + 1);
        return out;
    };

    if (strcmp(p, ".") == 0 || strcmp(p, "./") == 0) {
        char cwd[PATH_MAX];
        if (!::getcwd(cwd, sizeof(cwd))) {
            if (!callerOwnsBuffer)
                free(out);
            return nullptr;
        }
        return writeCanonical(cwd);
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
        // Shader cache misses are intentionally silent in the compatibility log.
        return nullptr;
    }

    // Android CryPak passes wildcard paths through AdjustFileName() before
    // OpenPacksCommon(). A wildcard itself is not a filesystem object, so only
    // resolve the existing parent directory and preserve the wildcard tail.
    // This also fixes case-sensitive Switch paths such as FCData/*.pak without
    // renaming the directory on SD.
    if (strpbrk(p, "*?[]") != nullptr) {
        const std::string normalized = normalizeSwitchFsPath(p);
        const size_t wildcardPos = normalized.find_first_of("*?[]");
        const size_t slashPos = normalized.rfind('/', wildcardPos);

        std::string parent = slashPos == std::string::npos
            ? std::string(".")
            : normalized.substr(0, slashPos);
        const std::string tail = slashPos == std::string::npos
            ? normalized
            : normalized.substr(slashPos + 1);

        if (parent.empty())
            parent = ".";

        std::string resolvedParent;
        if (resolvePathCaseInsensitive(parent.c_str(), resolvedParent)) {
            std::string resolvedPattern;

            if (!normalized.empty() && normalized[0] == '/') {
                resolvedPattern = resolvedParent;
            } else {
                char cwd[PATH_MAX];
                if (!::getcwd(cwd, sizeof(cwd))) {
                    if (!callerOwnsBuffer)
                        free(out);
                    return nullptr;
                }

                resolvedPattern = cwd;
                if (!resolvedParent.empty() && resolvedParent != ".") {
                    if (!resolvedPattern.empty() && resolvedPattern.back() != '/')
                        resolvedPattern += '/';
                    resolvedPattern += resolvedParent;
                }
            }

            if (!resolvedPattern.empty() && resolvedPattern.back() != '/')
                resolvedPattern += '/';
            resolvedPattern += tail;            return writeCanonical(resolvedPattern);
        }

        if (!callerOwnsBuffer)
            free(out);
        errno = ENOENT;        return nullptr;
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

    // A PAK entry is a real CryPak file even though there is no loose file on
    // the Switch filesystem. Android CryPak keeps this virtual path alive and
    // only opens the ZIP entry when FOpen()/GetFileData() is requested. Return
    // the canonical virtual path here instead of materializing the entry.
    if (!isShaderCacheLookupPath(p) &&
        (strchr(p, '/') || strchr(p, '\\'))) {
        std::string virtualPakPath;
        PakEntryMeta virtualMeta;
        if (pakFindVirtualEntry(p, virtualPakPath, virtualMeta)) {
            std::string virtualPath = p;
            if (virtualPath.empty() || virtualPath[0] != '/') {
                char cwd[PATH_MAX];
                if (::getcwd(cwd, sizeof(cwd))) {
                    std::string absolute = cwd;
                    if (!absolute.empty() && absolute.back() != '/')
                        absolute += '/';
                    absolute += virtualPath;
                    virtualPath.swap(absolute);
                }
            }
            return writeCanonical(virtualPath);
        }
    }

    if (!callerOwnsBuffer)
        free(out);
    errno = ENOENT;    return nullptr;
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
static int stub_fileno(FILE* f) {
    if (vpakOwns(f))
        return -1;
    return f ? ::fileno(f) : -1;
}

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

    if (rc != 0) {
        std::string pakPath;
        PakEntryMeta meta;
        if (pakFindVirtualEntry(ioPath, pakPath, meta)) {
            nativeSt = {};
            nativeSt.st_mode = S_IFREG | 0444;
            nativeSt.st_nlink = 1;
            nativeSt.st_uid = 0;
            nativeSt.st_gid = 0;
            nativeSt.st_size = (off_t)meta.uncompressedSize;
            nativeSt.st_blksize = 4096;
            nativeSt.st_blocks =
                (blkcnt_t)(((uint64_t)meta.uncompressedSize + 511u) / 512u);
            fillAndroidArm64Stat(nativeSt, ignored);
            compatPakLog("PAK VIRTUAL STAT: %s <- %s size=%u",
                         ioPath, pakPath.c_str(),
                         (unsigned)meta.uncompressedSize);
            return 0;
        }

        if (pakVirtualDirectoryExists(ioPath)) {
            nativeSt = {};
            nativeSt.st_mode = S_IFDIR | 0555;
            nativeSt.st_nlink = 2;
            nativeSt.st_uid = 0;
            nativeSt.st_gid = 0;
            nativeSt.st_blksize = 4096;
            fillAndroidArm64Stat(nativeSt, ignored);
            return 0;
        }

        return rc;
    }

    fillAndroidArm64Stat(nativeSt, ignored);
    return 0;
}

static int stub_fstat64(int fd, void* out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }

    struct stat st = {};
    int rc = 0;
    if (vpakFdOwns(fd))
        rc = vpakFdFstat(fd, &st);
    else
        rc = ::fstat(fd, &st);
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
    compatLogFlush();

    // The Android game expects exit() to terminate the process immediately.
    // Running newlib/SDL atexit handlers on Switch tears down objects that are
    // still referenced by the guest stack and can fault after "Quit-Yes".
    svcExitProcess();
    __builtin_unreachable();
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
    compatLogFlush();

    // _exit() must bypass host-side atexit/SDL teardown as well.
    svcExitProcess();
    __builtin_unreachable();
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
[[maybe_unused]] static bool looksLikeNewlibChunk(void* p) {
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
volatile uint32_t g_last_allocator_kind = 0;
volatile uint32_t g_last_allocator_phase = 0;
volatile uint64_t g_last_allocator_pc = 0;
volatile uint64_t g_last_allocator_ptr = 0;
volatile uint64_t g_last_allocator_size = 0;
volatile uint64_t g_last_allocator_size2 = 0;

static void recordAllocatorEvent(uint32_t kind, uint64_t ptr, uint64_t size,
                                  uint64_t size2, uint32_t phase) {
    g_last_allocator_kind = kind;
    g_last_allocator_ptr = ptr;
    g_last_allocator_size = size;
    g_last_allocator_size2 = size2;
    g_last_allocator_pc = (uint64_t)(uintptr_t)__builtin_return_address(0);
    g_last_allocator_phase = phase;
}

void shimLastAllocatorEvent(uint32_t* kind, uint32_t* phase, uint64_t* caller,
                            uint64_t* ptr, uint64_t* size, uint64_t* size2) {
    if (kind)  *kind  = g_last_allocator_kind;
    if (phase) *phase = g_last_allocator_phase;
    if (caller) *caller = g_last_allocator_pc;
    if (ptr)   *ptr   = g_last_allocator_ptr;
    if (size)  *size  = g_last_allocator_size;
    if (size2) *size2 = g_last_allocator_size2;
}

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
    reported++;}

static void* sh_malloc(size_t n) {
    g_sh_malloc_calls++;
    recordAllocatorEvent(1, 0, n, 0, 1);
    arenaGate("malloc");
    void* r = malloc(n);
    g_last_allocator_phase = 2;
    g_last_allocator_ptr = (uint64_t)(uintptr_t)r;
    return r;
}
static void* sh_calloc(size_t a, size_t b) {
    g_sh_calloc_calls++;
    recordAllocatorEvent(2, 0, a, b, 1);
    arenaGate("calloc");
    void* r = calloc(a, b);
    g_last_allocator_phase = 2;
    g_last_allocator_ptr = (uint64_t)(uintptr_t)r;
    return r;
}

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
    const uintptr_t top_addr = (uintptr_t)__malloc_av_[2];
    const NlChunk* top_chunk = (const NlChunk*)top_addr;
    const size_t top_size = top_chunk ? nlSize(top_chunk) : 0;
    static bool bounds_logged = false;
    if (!bounds_logged) {
        bounds_logged = true;    }

    for (int i = 0; i < 400000; i++) {
        g_walk_steps = i;
        if (!memIsHeap(c)) { g_walk_stop = "left the heap region"; return true; }
        if ((uintptr_t)c + 32 > arena_end) { g_walk_stop = "reached the break"; return true; }

        // av_[2] is newlib's top chunk. It is not a normal free-bin chunk and
        // therefore must be recognized before checking fd/bk or the boundary
        // tag of the following zero-filled arena padding.
        if (c == top_chunk) {
            g_walk_stop = "reached top chunk";
            return true;
        }

        size_t sz = nlSize(c);

        // A zero-sized header immediately after the real top chunk is the
        // unused arena tail, not heap corruption. This must be checked BEFORE
        // PREV_INUSE/prev_size: the zeroed tail has prev_size=0 while prev_sz is
        // the valid size of the top chunk, which otherwise creates the exact
        // false-positive seen in the logs.
        if (sz == 0) {
            if (top_addr && top_size &&
                (uintptr_t)c >= top_addr + top_size) {
                g_walk_stop = "reached arena tail after top";
                return true;
            }
            snprintf(why, whysz,
                     "chunk %p has size 0 below the break (step %d)",
                     (const void*)c, i);
            g_walk_stop = "zero-size chunk";
            return false;
        }

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
            // allocations backwards.
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
            g_walk_stop = "prev_size mismatch";
            return false;
        }

        const NlChunk* this_prev_c  = prev_c;
        const size_t   this_prev_sz = prev_sz;
        (void)this_prev_sz;
        prev_c  = c;
        prev_sz = sz;
        if (sz < 32 || (sz & 15) != 0) {
            snprintf(why, whysz, "chunk %p has bad size %zu (step %d)", (const void*)c, sz, i);
            g_walk_stop = "bad chunk size";
            return false;
        }
        const NlChunk* next = (const NlChunk*)((const char*)c + sz);
        if (next <= c) {
            snprintf(why, whysz, "chunk %p does not advance (step %d)", (const void*)c, i);
            g_walk_stop = "non-advancing chunk";
            return false;
        }
        if (!memIsHeap(next)) {
            g_walk_stop = "left heap after chunk";
            return true;
        }

        // A chunk is free when the following chunk says its predecessor is not
        // in use. Any such chunk is on a bin, so its fd and bk should be non-null.
        // The top chunk was already handled above by identity.
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
    recordAllocatorEvent(3, (uint64_t)(uintptr_t)p, 0, 0, 1);
    arenaGate("free");

    // Android's original CMTSafeHeap::Free() calls plain ::free(p) on Linux.
    // Do not second-guess the allocator's chunk metadata here: pointers coming
    // from CrySystem/CMTSafeHeap can have allocator headers that differ from
    // the conservative newlib chunk heuristic above. For this A/B experiment,
    // only reject addresses that are not part of a Switch heap at all.
    if (!memIsHeap(p))
        return;

    free(p);
    g_last_allocator_phase = 2;
}
static void* sh_realloc(void* p, size_t n) {
    g_sh_realloc_calls++;
    recordAllocatorEvent(4, (uint64_t)(uintptr_t)p, n, 0, 1);
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
        void* r = realloc(p, n);
        g_last_allocator_phase = 2;
        g_last_allocator_ptr = (uint64_t)(uintptr_t)r;
        return r;
    }

    // Keep protection for pointers that are not part of the Switch heap at all.
    // There is no trustworthy old size/owner information for these pointers,
    // so do not pass them to newlib realloc.
    void* r = malloc(n);
    g_last_allocator_phase = 2;
    g_last_allocator_ptr = (uint64_t)(uintptr_t)r;
    return r;
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
    if (vpakFdOwns(fd))
        return vpakFdRead(fd, b, n);
    return read(fd, b, n);
}

static off_t sh_lseek(int fd, off_t off, int whence) {
    if (vpakFdOwns(fd))
        return vpakFdSeek(fd, off, whence);
    return lseek(fd, off, whence);
}

static int sh_close(int fd) {
    if (fd == URANDOM_FD) return 0;
    if (vpakFdOwns(fd))
        return vpakFdClose(fd);
    return close(fd);
}

static int sh_fstat(int fd, struct stat* st) {
    if (vpakFdOwns(fd))
        return vpakFdFstat(fd, st);
    return fstat(fd, st);
}

// write() routed through the log for stdout/stderr — libc++abi terminate
// messages ("terminating with uncaught exception of type ...") land on fd 2.
static bool isCompressedBumpWarning(const char* text) {
    if (!text)
        return false;

    // Android CryEngine's CTexMan path deliberately accepts compressed bump
    // maps: it decodes DXT to RGBA and continues loading. The retail warning
    // describes the old unsupported path and is therefore misleading here.
    return std::strstr(text, "Loading of compressed bump-maps is not supported") != nullptr;
}

static ssize_t sh_write(int fd, const void* buf, size_t n) {
    if ((fd == 1 || fd == 2) && buf && n > 0) {
        char tmp[512];
        size_t c = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
        memcpy(tmp, buf, c);
        tmp[c] = '\0';
        while (c > 0 && (tmp[c - 1] == '\n' || tmp[c - 1] == '\r')) tmp[--c] = '\0';

        if (c > 0 && !isCompressedBumpWarning(tmp))
            compatLogFmt("game %s: %s", fd == 2 ? "stderr" : "stdout", tmp);
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

[[maybe_unused]] static std::string cdataToFcdata(const char* path) {
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
static std::string pakNormalizeName(const char* name) {
    std::string out = name ? name : "";

    // Match CryPak::BeautifyPath semantics closely enough for the virtual PAK
    // index: native/non-native slashes become '/', names are case-folded, and
    // redundant separators plus "/./" path components disappear. In
    // particular, weapon animation lists routinely contain "dir\\.\\file.caf".
    for (char& c : out) {
        if ((unsigned char)c == 92)
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    // Canonicalize path components, including "..". The old byte-wise
    // implementation treated the second dot in "../" as a standalone "."
    // component, turning "../BumpDiffuse.csi" into "./BumpDiffuse.csi".
    // That broke the shader preprocessor's relative includes and made valid
    // files in Shaders.pak appear to be missing.
    std::vector<std::string> components;
    components.reserve(16);

    size_t i = 0;
    while (i < out.size()) {
        while (i < out.size() && out[i] == '/')
            ++i;
        if (i >= out.size())
            break;

        const size_t begin = i;
        while (i < out.size() && out[i] != '/')
            ++i;

        const std::string part = out.substr(begin, i - begin);
        if (part.empty() || part == ".")
            continue;

        if (part == "..") {
            if (!components.empty() && components.back() != "..")
                components.pop_back();
            else if (out.empty() || out[0] != '/')
                components.push_back(part);
            continue;
        }

        components.push_back(part);
    }

    std::string normalized;
    normalized.reserve(out.size());
    for (size_t n = 0; n < components.size(); ++n) {
        if (n)
            normalized.push_back('/');
        normalized += components[n];
    }

    // Do not leave a leading slash for ordinary relative PAK entries.
    while (normalized.size() > 1 && normalized[0] == '/' &&
           normalized[1] != '/')
        normalized.erase(0, 1);

    return normalized;
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

struct PakIndex {
    bool valid = false;
    std::unordered_map<std::string, PakEntryMeta> entries;
};

static std::atomic<unsigned> g_pakMemoryTraceEvents{0};
static std::atomic<unsigned> g_cafLookupDiagEvents{0};

static bool pakMemoryTracePath(const char* path) {
    if (!path || !*path)
        return false;

    std::string lower(path);
    for (char& c : lower)
        c = (char)std::tolower((unsigned char)c);

    return lower.find(".caf") != std::string::npos ||
           lower.find(".cgf") != std::string::npos;
}

static bool pakMemoryTraceBudget(const char* path) {
    if (!pakMemoryTracePath(path))
        return false;

    const unsigned n = g_pakMemoryTraceEvents.fetch_add(1);
    return n < 160;
}

struct VirtualPakFile {
    std::shared_ptr<std::vector<unsigned char>> data;
    size_t pos = 0;
    std::string name;
    unsigned traceReads = 0;
    bool trace = false;
};

static Mutex g_vpak_lock;
static std::unordered_set<FILE*> g_vpak_handles;

struct PakMemoryCacheEntry {
    std::shared_ptr<std::vector<unsigned char>> data;
    size_t bytes = 0;
};

static Mutex g_pak_memory_cache_lock;
static std::unordered_map<std::string, PakMemoryCacheEntry> g_pak_memory_cache;
static size_t g_pak_memory_cache_bytes = 0;
static constexpr size_t kPakMemoryCacheMaxBytes = 192u * 1024u * 1024u;
static constexpr size_t kPakMemoryCacheMaxEntryBytes = 16u * 1024u * 1024u;

static std::string pakMemoryCacheKey(const std::string& pakPath,
                                     const PakEntryMeta& meta) {
    char key[160];
    std::snprintf(key, sizeof(key), "%s|%u|%u|%u|%u",
                  pakPath.c_str(),
                  (unsigned)meta.localOffset,
                  (unsigned)meta.compressedSize,
                  (unsigned)meta.uncompressedSize,
                  (unsigned)meta.method);
    return std::string(key);
}

static bool pakGetCachedMemory(
    const std::string& pakPath,
    const PakEntryMeta& meta,
    std::shared_ptr<std::vector<unsigned char>>& data) {
    const std::string key = pakMemoryCacheKey(pakPath, meta);
    mutexLock(&g_pak_memory_cache_lock);
    auto it = g_pak_memory_cache.find(key);
    if (it == g_pak_memory_cache.end()) {
        mutexUnlock(&g_pak_memory_cache_lock);
        return false;
    }
    data = it->second.data;
    mutexUnlock(&g_pak_memory_cache_lock);
    return (bool)data;
}

static void pakRememberMemory(
    const std::string& pakPath,
    const PakEntryMeta& meta,
    const std::shared_ptr<std::vector<unsigned char>>& data) {
    if (!data || data->empty() ||
        data->size() > kPakMemoryCacheMaxEntryBytes)
        return;

    const size_t bytes = data->size();
    const std::string key = pakMemoryCacheKey(pakPath, meta);

    mutexLock(&g_pak_memory_cache_lock);
    if (g_pak_memory_cache.find(key) != g_pak_memory_cache.end() ||
        g_pak_memory_cache_bytes + bytes > kPakMemoryCacheMaxBytes) {
        mutexUnlock(&g_pak_memory_cache_lock);
        return;
    }

    g_pak_memory_cache.emplace(key, PakMemoryCacheEntry{data, bytes});
    g_pak_memory_cache_bytes += bytes;
    mutexUnlock(&g_pak_memory_cache_lock);
}

static bool pakGetMemory(
    const std::string& pakPath,
    const PakEntryMeta& meta,
    std::shared_ptr<std::vector<unsigned char>>& data,
    bool& cacheHit) {
    cacheHit = pakGetCachedMemory(pakPath, meta, data);
    compatPakLog("MEMORY_LOOKUP: pak=%s local=%u c=%u u=%u cache=%d",
                 pakPath.c_str(), (unsigned)meta.localOffset,
                 (unsigned)meta.compressedSize,
                 (unsigned)meta.uncompressedSize,
                 cacheHit ? 1 : 0);
    if (cacheHit)
        return true;

    std::vector<unsigned char> plain;
    if (!pakReadEntryToMemory(pakPath, meta, plain)) {
        compatPakLog("MEMORY_READ_FAIL: pak=%s local=%u c=%u u=%u method=%u",
                     pakPath.c_str(), (unsigned)meta.localOffset,
                     (unsigned)meta.compressedSize,
                     (unsigned)meta.uncompressedSize,
                     (unsigned)meta.method);
        return false;
    }

    data = std::make_shared<std::vector<unsigned char>>(std::move(plain));
    if (!data)
        return false;

    pakRememberMemory(pakPath, meta, data);
    compatPakLog("MEMORY_READY: pak=%s bytes=%zu cacheable=%d",
                 pakPath.c_str(), data->size(),
                 data->size() <= kPakMemoryCacheMaxEntryBytes ? 1 : 0);
    return true;
}

static bool vpakOwns(FILE* f) {
    if (!f)
        return false;
    mutexLock(&g_vpak_lock);
    const bool found = g_vpak_handles.find(f) != g_vpak_handles.end();
    mutexUnlock(&g_vpak_lock);
    return found;
}

static VirtualPakFile* vpakLookupLocked(FILE* f) {
    if (!f || g_vpak_handles.find(f) == g_vpak_handles.end())
        return nullptr;
    return reinterpret_cast<VirtualPakFile*>(f);
}

static FILE* vpakOpen(std::shared_ptr<std::vector<unsigned char>> data,
                           const char* requested,
                           bool trace) {
    VirtualPakFile* v = new VirtualPakFile();
    if (!v || !data)
        return nullptr;
    v->data = std::move(data);
    if (requested)
        v->name = requested;
    v->trace = trace;
    FILE* handle = reinterpret_cast<FILE*>(v);
    mutexLock(&g_vpak_lock);
    g_vpak_handles.insert(handle);
    mutexUnlock(&g_vpak_lock);
    return handle;
}

static size_t vpakRead(FILE* f, void* dst, size_t size, size_t count) {
    if (!dst || size == 0 || count == 0)
        return 0;

    mutexLock(&g_vpak_lock);
    VirtualPakFile* v = vpakLookupLocked(f);
    if (!v || v->pos >= v->data->size()) {
        mutexUnlock(&g_vpak_lock);
        return 0;
    }

    const size_t maxBytes = v->data->size() - v->pos;
    const size_t requested =
        (count > SIZE_MAX / size) ? SIZE_MAX : size * count;
    const size_t bytes = requested < maxBytes ? requested : maxBytes;
    const size_t whole = bytes - (bytes % size);

    if (whole) {
        if (v->trace && v->traceReads < 8) {
            compatPakLog("PAK MEM TRACE READ: %s src=%p dst=%p pos=%zu bytes=%zu size=%zu",
                         v->name.c_str(),
                         (void*)(v->data->data() + v->pos),
                         dst, v->pos, whole, v->data->size());
            ++v->traceReads;
        }
        memcpy(dst, v->data->data() + v->pos, whole);
        v->pos += whole;
    }

    mutexUnlock(&g_vpak_lock);
    return whole / size;
}

static int vpakSeek(FILE* f, int64_t off, int whence) {
    mutexLock(&g_vpak_lock);
    VirtualPakFile* v = vpakLookupLocked(f);
    if (!v) {
        mutexUnlock(&g_vpak_lock);
        return -1;
    }

    int64_t base = 0;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (int64_t)v->pos;
    else if (whence == SEEK_END)
        base = (int64_t)v->data->size();
    else {
        mutexUnlock(&g_vpak_lock);
        return -1;
    }

    const int64_t next = base + off;
    if (next < 0 || (uint64_t)next > (uint64_t)v->data->size()) {
        mutexUnlock(&g_vpak_lock);
        return -1;
    }

    v->pos = (size_t)next;
    mutexUnlock(&g_vpak_lock);
    return 0;
}

static long long vpakTell64(FILE* f) {
    mutexLock(&g_vpak_lock);
    VirtualPakFile* v = vpakLookupLocked(f);
    const long long pos = v ? (long long)v->pos : -1;
    mutexUnlock(&g_vpak_lock);
    return pos;
}

static int vpakGetc(FILE* f) {
    mutexLock(&g_vpak_lock);
    VirtualPakFile* v = vpakLookupLocked(f);
    if (!v || v->pos >= v->data->size()) {
        mutexUnlock(&g_vpak_lock);
        return EOF;
    }
    const int c = (*v->data)[v->pos++];
    mutexUnlock(&g_vpak_lock);
    return c;
}

static int vpakEof(FILE* f) {
    mutexLock(&g_vpak_lock);
    VirtualPakFile* v = vpakLookupLocked(f);
    const int eof = !v || v->pos >= v->data->size();
    mutexUnlock(&g_vpak_lock);
    return eof ? 1 : 0;
}

static int vpakClose(FILE* f) {
    mutexLock(&g_vpak_lock);
    auto it = g_vpak_handles.find(f);
    if (it == g_vpak_handles.end()) {
        mutexUnlock(&g_vpak_lock);
        return -1;
    }
    g_vpak_handles.erase(it);
    VirtualPakFile* v = reinterpret_cast<VirtualPakFile*>(f);
    mutexUnlock(&g_vpak_lock);
    delete v;
    return 0;
}

// POSIX file-descriptor equivalent of the Android/CryPak pseudo-file.
// Some guest code (notably CControllerManager::LoadAnimation) bypasses
// stdio and uses open/read/lseek directly. Keep those handles entirely in
// memory too; never create a loose copy of the PAK entry.
struct VirtualPakFd {
    std::shared_ptr<std::vector<unsigned char>> data;
    off_t pos = 0;
    std::string name;
    unsigned traceReads = 0;
    bool trace = false;
};

static constexpr int VPAK_FD_BASE = 0x6000;
static constexpr int VPAK_FD_LIMIT = 0x6fff;
static Mutex g_vpak_fd_lock;
static std::unordered_map<int, VirtualPakFd*> g_vpak_fds;
static int g_next_vpak_fd = VPAK_FD_BASE;

static bool vpakFdOwns(int fd) {
    mutexLock(&g_vpak_fd_lock);
    const bool found = g_vpak_fds.find(fd) != g_vpak_fds.end();
    mutexUnlock(&g_vpak_fd_lock);
    return found;
}

static int vpakFdOpen(const char* requested, int flags) {
    if (!requested || !*requested)
        return -1;

    // Virtual PAK entries are read-only. Writes/creates must continue to use
    // the real filesystem rather than silently redirecting to an archive.
    const int writeFlags = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
    if (flags & writeFlags)
        return -1;

    const bool trace = pakMemoryTraceBudget(requested);
    const std::string wantedTrace = pakAssetRelativeName(requested);

    std::string pakPath;
    PakEntryMeta meta;
    if (!pakFindVirtualEntry(requested, pakPath, meta)) {
        if (trace) {
            compatPakLog("PAK MEM TRACE FD_MISS: requested=%s wanted=%s",
                         requested, wantedTrace.c_str());
        }
        return -1;
    }

    if (trace) {
        compatPakLog("PAK MEM TRACE FD_FIND: requested=%s wanted=%s pak=%s c=%u u=%u method=%u local=%u",
                     requested, wantedTrace.c_str(), pakPath.c_str(),
                     meta.compressedSize, meta.uncompressedSize,
                     (unsigned)meta.method, meta.localOffset);
    }

    std::shared_ptr<std::vector<unsigned char>> data;
    bool cacheHit = false;
    if (!pakGetMemory(pakPath, meta, data, cacheHit)) {
        if (trace) {
            compatPakLog("PAK MEM TRACE FD_READ_FAILED: %s <- %s",
                         wantedTrace.c_str(), pakPath.c_str());
        } else {
            compatPakLog("PAK VIRTUAL FD READ FAILED: %s <- %s",
                         wantedTrace.c_str(),
                         pakPath.c_str());
        }
        return -1;
    }

    if (trace) {
        compatPakLog("PAK MEM TRACE FD_%s: %s plain=%p plain_size=%zu pak=%s",
                     cacheHit ? "CACHE" : "DECOMP",
                     wantedTrace.c_str(), (void*)data->data(), data->size(),
                     pakPath.c_str());
    }

    VirtualPakFd* v = new VirtualPakFd();
    if (!v)
        return -1;
    v->data = std::move(data);
    v->name = requested;
    v->trace = trace;

    mutexLock(&g_vpak_fd_lock);
    int chosen = -1;
    for (int i = 0; i <= (VPAK_FD_LIMIT - VPAK_FD_BASE); ++i) {
        const int candidate =
            VPAK_FD_BASE + ((g_next_vpak_fd - VPAK_FD_BASE + i) %
                            (VPAK_FD_LIMIT - VPAK_FD_BASE + 1));
        if (g_vpak_fds.find(candidate) == g_vpak_fds.end()) {
            chosen = candidate;
            g_next_vpak_fd = candidate + 1;
            if (g_next_vpak_fd > VPAK_FD_LIMIT)
                g_next_vpak_fd = VPAK_FD_BASE;
            break;
        }
    }

    if (chosen >= 0)
        g_vpak_fds.emplace(chosen, v);
    mutexUnlock(&g_vpak_fd_lock);

    if (chosen < 0) {
        delete v;
        errno = EMFILE;
        return -1;
    }

    compatPakLog("PAK VIRTUAL FD OPEN: %s <- %s fd=%d size=%zu",
                 pakAssetRelativeName(requested).c_str(),
                 pakPath.c_str(), chosen, v->data->size());
    return chosen;
}

static ssize_t vpakFdRead(int fd, void* dst, size_t count) {
    if (!dst && count) {
        errno = EFAULT;
        return -1;
    }

    mutexLock(&g_vpak_fd_lock);
    auto it = g_vpak_fds.find(fd);
    if (it == g_vpak_fds.end()) {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EBADF;
        return -1;
    }

    VirtualPakFd* v = it->second;
    if (v->pos >= (off_t)v->data->size()) {
        mutexUnlock(&g_vpak_fd_lock);
        return 0;
    }

    const size_t available = v->data->size() - (size_t)v->pos;
    const size_t bytes = count < available ? count : available;
    if (bytes) {
        if (v->trace && v->traceReads < 8) {
            compatPakLog("PAK MEM TRACE FDREAD: %s fd=%d src=%p dst=%p pos=%lld bytes=%zu size=%zu",
                         v->name.c_str(), fd,
                         (void*)(v->data->data() + v->pos),
                         dst, (long long)v->pos, bytes, v->data->size());
            ++v->traceReads;
        }
        memcpy(dst, v->data->data() + v->pos, bytes);
    }
    v->pos += (off_t)bytes;
    mutexUnlock(&g_vpak_fd_lock);
    return (ssize_t)bytes;
}

static off_t vpakFdSeek(int fd, off_t off, int whence) {
    mutexLock(&g_vpak_fd_lock);
    auto it = g_vpak_fds.find(fd);
    if (it == g_vpak_fds.end()) {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EBADF;
        return (off_t)-1;
    }

    VirtualPakFd* v = it->second;
    off_t base = 0;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = v->pos;
    else if (whence == SEEK_END)
        base = (off_t)v->data->size();
    else {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EINVAL;
        return (off_t)-1;
    }

    const off_t next = base + off;
    if (next < 0 || next > (off_t)v->data->size()) {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EINVAL;
        return (off_t)-1;
    }

    v->pos = next;
    mutexUnlock(&g_vpak_fd_lock);
    return next;
}

static ssize_t vpakFdPread(int fd, void* dst, size_t count, off_t offset) {
    if (!dst && count) {
        errno = EFAULT;
        return -1;
    }

    mutexLock(&g_vpak_fd_lock);
    auto it = g_vpak_fds.find(fd);
    if (it == g_vpak_fds.end()) {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EBADF;
        return -1;
    }

    VirtualPakFd* v = it->second;
    if (offset < 0 || offset >= (off_t)v->data->size()) {
        mutexUnlock(&g_vpak_fd_lock);
        return 0;
    }

    const size_t available = v->data->size() - (size_t)offset;
    const size_t bytes = count < available ? count : available;
    if (bytes) {
        if (v->trace && v->traceReads < 8) {
            compatPakLog("PAK MEM TRACE FDPREAD: %s fd=%d src=%p dst=%p pos=%lld bytes=%zu size=%zu",
                         v->name.c_str(), fd,
                         (void*)(v->data->data() + offset),
                         dst, (long long)offset, bytes, v->data->size());
            ++v->traceReads;
        }
        memcpy(dst, v->data->data() + offset, bytes);
    }
    mutexUnlock(&g_vpak_fd_lock);
    return (ssize_t)bytes;
}

static int vpakFdClose(int fd) {
    mutexLock(&g_vpak_fd_lock);
    auto it = g_vpak_fds.find(fd);
    if (it == g_vpak_fds.end()) {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EBADF;
        return -1;
    }

    VirtualPakFd* v = it->second;
    g_vpak_fds.erase(it);
    mutexUnlock(&g_vpak_fd_lock);
    delete v;
    return 0;
}

static int vpakFdFstat(int fd, struct stat* st) {
    if (!st) {
        errno = EINVAL;
        return -1;
    }

    mutexLock(&g_vpak_fd_lock);
    auto it = g_vpak_fds.find(fd);
    if (it == g_vpak_fds.end()) {
        mutexUnlock(&g_vpak_fd_lock);
        errno = EBADF;
        return -1;
    }

    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (off_t)it->second->data->size();
    st->st_blksize = 4096;
    st->st_blocks = (blkcnt_t)((it->second->data->size() + 511u) / 512u);
    mutexUnlock(&g_vpak_fd_lock);
    return 0;
}

static Mutex g_pak_index_lock;
static std::unordered_map<std::string, PakIndex> g_pak_indexes;
static std::vector<std::string> g_active_level_paks;

struct PakLookupCacheEntry {
    std::string pakPath;
    PakEntryMeta meta;
};

static std::vector<std::string> g_global_pak_paths;
static bool g_global_pak_paths_ready = false;
static std::unordered_map<std::string, PakLookupCacheEntry> g_pak_lookup_cache;
static std::unordered_set<std::string> g_pak_lookup_misses;

// CRefStreamEngine may ask for the same CAF size repeatedly. Once a file has
// been positively resolved from a PAK, keep its size independent of subsequent
// lookup state so a later GetFileSize() can never regress to zero.
static std::unordered_map<std::string, uint32_t> g_caf_size_cache;

static void rememberActiveLevelPak(const char* path) {
    if (!path || !*path)
        return;

    std::string candidate = normalizeSwitchFsPath(path);
    std::string lower = candidate;
    for (char& c : lower)
        c = (char)std::tolower((unsigned char)c);

    const bool levelPak =
        (lower.find("/levels/") != std::string::npos ||
         lower.rfind("levels/", 0) == 0) &&
        lower.size() >= 4 &&
        lower.compare(lower.size() - 4, 4, ".pak") == 0;
    if (!levelPak)
        return;

    struct stat st = {};
    if (::stat(candidate.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        // CryPak can hand us a lower-case level PAK name (for example
        // "levellm.pak") while the real Switch filesystem contains
        // "LevelLM.pak". Resolve the existing path without changing what
        // CryPak sees, then remember the actual on-disk spelling.
        std::string resolved;
        if (!resolvePathCaseInsensitive(candidate.c_str(), resolved))
            return;

        st = {};
        if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return;

        candidate = resolved;
    }

    compatPakLog("PAK LEVEL REGISTER: requested=%s actual=%s",
                 path, candidate.c_str());

    if (!candidate.empty() && candidate[0] != '/') {
        char cwd[PATH_MAX];
        if (::getcwd(cwd, sizeof(cwd))) {
            std::string absolute(cwd);
            if (!absolute.empty() && absolute.back() != '/')
                absolute += '/';
            absolute += candidate;
            candidate = absolute;
        }
    }

    mutexLock(&g_pak_index_lock);
    if (std::find(g_active_level_paks.begin(),
                  g_active_level_paks.end(), candidate) ==
        g_active_level_paks.end()) {
        g_active_level_paks.emplace_back(candidate);
        g_pak_lookup_cache.clear();
        g_pak_lookup_misses.clear();
        g_caf_size_cache.clear();
    }
    mutexUnlock(&g_pak_index_lock);
}

static std::vector<std::string> collectGlobalPakPaths() {
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;

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
        std::string actualDir = roots[r];
        DIR* dir = ::opendir(actualDir.c_str());
        if (!dir && resolvePathCaseInsensitive(roots[r], actualDir))
            dir = ::opendir(actualDir.c_str());
        if (!dir)
            continue;

        while (dirent* ent = ::readdir(dir)) {
            const char* name = ent->d_name;
            if (!name || !*name)
                continue;

            const size_t len = std::strlen(name);
            if (len < 4)
                continue;

            const char c0 = (char)std::tolower((unsigned char)name[len - 4]);
            const char c1 = (char)std::tolower((unsigned char)name[len - 3]);
            const char c2 = (char)std::tolower((unsigned char)name[len - 2]);
            const char c3 = (char)std::tolower((unsigned char)name[len - 1]);
            if (c0 != '.' || c1 != 'p' || c2 != 'a' || c3 != 'k')
                continue;

            std::string pakPath = actualDir;
            if (pakPath.empty() || pakPath == ".")
                pakPath = name;
            else {
                if (pakPath.back() != '/')
                    pakPath += '/';
                pakPath += name;
            }

            std::string resolved;
            if (resolvePathCaseInsensitive(pakPath.c_str(), resolved))
                pakPath = resolved;

            std::string key = asciiLower(pakPath);
            for (char& ch : key)
                if ((unsigned char)ch == '\\')
                    ch = '/';

            if (seen.insert(key).second)
                paths.emplace_back(std::move(pakPath));
        }
        ::closedir(dir);
    }

    std::sort(paths.begin(), paths.end());
    return paths;
}

static std::vector<std::string> getGlobalPakPathsSnapshot() {
    mutexLock(&g_pak_index_lock);
    if (g_global_pak_paths_ready) {
        std::vector<std::string> snapshot = g_global_pak_paths;
        mutexUnlock(&g_pak_index_lock);
        return snapshot;
    }
    mutexUnlock(&g_pak_index_lock);

    std::vector<std::string> discovered = collectGlobalPakPaths();

    mutexLock(&g_pak_index_lock);
    if (!g_global_pak_paths_ready) {
        g_global_pak_paths = std::move(discovered);
        g_global_pak_paths_ready = true;
        compatPakLog("PAK PATHS: cached %zu unique global archives",
                     g_global_pak_paths.size());
        for (const std::string& pakPath : g_global_pak_paths)
            compatPakLog("GLOBAL_PAK_PATH: %s", pakPath.c_str());
    }
    std::vector<std::string> snapshot = g_global_pak_paths;
    mutexUnlock(&g_pak_index_lock);
    return snapshot;
}

static bool buildPakIndexLocked(const std::string& pakPath, PakIndex& index) {
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

    if (!entries || !cdSize || cdSize > 128 * 1024 * 1024u ||
        (uint64_t)cdOffset + (uint64_t)cdSize > (uint64_t)fileSize) {
        fclose(pak);
        return false;
    }

    std::vector<unsigned char> cd(cdSize);
    if (fseek(pak, (long)cdOffset, SEEK_SET) != 0 ||
        !pakReadExact(pak, cd.data(), cd.size())) {
        fclose(pak);
        return false;
    }
    fclose(pak);

    index.entries.reserve((size_t)entries * 2u);

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

        const std::string name((const char*)h + 46, nameLen);
        const std::string normalized = pakNormalizeName(name.c_str());

        PakEntryMeta meta;
        meta.method = pakRd16(h + 10);
        meta.compressedSize = pakRd32(h + 20);
        meta.uncompressedSize = pakRd32(h + 24);
        meta.expectedCrc = pakRd32(h + 16);
        meta.localOffset = pakRd32(h + 42);

        // Keep the first matching entry, matching the old linear lookup.
        index.entries.emplace(normalized, meta);

        pos += recordSize;
    }

    index.valid = true;    return true;
}

static bool pakFindEntryCached(const std::string& pakPath,
                               const std::string& wanted,
                               PakEntryMeta& meta) {
    const std::string normalizedWanted = pakNormalizeName(wanted.c_str());
    compatPakLog("INDEX_QUERY: pak=%s wanted=%s normalized=%s",
                 pakPath.c_str(), wanted.c_str(), normalizedWanted.c_str());

    mutexLock(&g_pak_index_lock);

    auto it = g_pak_indexes.find(pakPath);
    if (it == g_pak_indexes.end()) {
        PakIndex fresh;
        compatPakLog("INDEX_BUILD_START: pak=%s", pakPath.c_str());
        const bool ok = buildPakIndexLocked(pakPath, fresh);
        compatPakLog("INDEX_BUILD_RESULT: pak=%s ok=%d valid=%d entries=%zu",
                     pakPath.c_str(), ok ? 1 : 0, fresh.valid ? 1 : 0,
                     fresh.entries.size());
        auto inserted = g_pak_indexes.emplace(pakPath, std::move(fresh));
        it = inserted.first;
        if (!ok) {
            mutexUnlock(&g_pak_index_lock);
            return false;
        }
    }

    const bool found = it->second.valid &&
                       it->second.entries.find(normalizedWanted) !=
                           it->second.entries.end();
    if (found) {
        meta = it->second.entries.find(normalizedWanted)->second;
        compatPakLog("INDEX_ENTRY_HIT: pak=%s wanted=%s c=%u u=%u method=%u local=%u crc=%08x",
                     pakPath.c_str(), normalizedWanted.c_str(),
                     (unsigned)meta.compressedSize,
                     (unsigned)meta.uncompressedSize,
                     (unsigned)meta.method,
                     (unsigned)meta.localOffset,
                     (unsigned)meta.expectedCrc);
    } else {
        compatPakLog("INDEX_ENTRY_MISS: pak=%s wanted=%s valid=%d entries=%zu",
                     pakPath.c_str(), normalizedWanted.c_str(),
                     it->second.valid ? 1 : 0, it->second.entries.size());
    }

    mutexUnlock(&g_pak_index_lock);
    return found;
}

static bool pakReadEntryToMemory(const std::string& pakPath,
                                   const PakEntryMeta& meta,
                                   std::vector<unsigned char>& plain) {
    compatPakLog("READ_START: pak=%s local=%u c=%u u=%u method=%u",
                 pakPath.c_str(), (unsigned)meta.localOffset,
                 (unsigned)meta.compressedSize,
                 (unsigned)meta.uncompressedSize,
                 (unsigned)meta.method);

    if (!meta.compressedSize || !meta.uncompressedSize ||
        meta.compressedSize > 128 * 1024 * 1024u ||
        meta.uncompressedSize > 128 * 1024 * 1024u) {
        compatPakLog("READ_REJECT_SIZE: pak=%s c=%u u=%u",
                     pakPath.c_str(), (unsigned)meta.compressedSize,
                     (unsigned)meta.uncompressedSize);
        return false;
    }

    FILE* pak = fopen(pakPath.c_str(), "rb");
    if (!pak) {
        compatPakLog("READ_OPEN_FAIL: pak=%s", pakPath.c_str());
        return false;
    }

    unsigned char local[30];
    if (fseek(pak, (long)meta.localOffset, SEEK_SET) != 0) {
        compatPakLog("READ_FSEEK_LOCAL_FAIL: pak=%s local=%u",
                     pakPath.c_str(), (unsigned)meta.localOffset);
        fclose(pak);
        return false;
    }
    if (!pakReadExact(pak, local, sizeof(local))) {
        compatPakLog("READ_LOCAL_HEADER_FAIL: pak=%s local=%u",
                     pakPath.c_str(), (unsigned)meta.localOffset);
        fclose(pak);
        return false;
    }
    if (pakRd32(local) != 0x04034b50u) {
        compatPakLog("READ_LOCAL_MAGIC_FAIL: pak=%s local=%u magic=%08x",
                     pakPath.c_str(), (unsigned)meta.localOffset,
                     (unsigned)pakRd32(local));
        fclose(pak);
        return false;
    }

    const uint16_t localMethod = pakRd16(local + 8);
    const uint16_t nameLen = pakRd16(local + 26);
    const uint16_t extraLen = pakRd16(local + 28);
    const long dataOffset =
        (long)meta.localOffset + 30L + nameLen + extraLen;

    if (meta.method != localMethod) {
        compatPakLog("READ_METHOD_MISMATCH: pak=%s central=%u local=%u",
                     pakPath.c_str(), (unsigned)meta.method,
                     (unsigned)localMethod);
        fclose(pak);
        return false;
    }
    if (dataOffset < 0 ||
        fseek(pak, dataOffset, SEEK_SET) != 0) {
        compatPakLog("READ_FSEEK_DATA_FAIL: pak=%s data_offset=%ld",
                     pakPath.c_str(), dataOffset);
        fclose(pak);
        return false;
    }

    std::vector<unsigned char> compressed(meta.compressedSize);
    plain.resize(meta.uncompressedSize);

    const bool readOk =
        pakReadExact(pak, compressed.data(), compressed.size());
    fclose(pak);

    if (!readOk) {
        compatPakLog("READ_DATA_FAIL: pak=%s bytes=%u",
                     pakPath.c_str(), (unsigned)meta.compressedSize);
        return false;
    }

    if (meta.method == 0 &&
        meta.compressedSize == meta.uncompressedSize) {
        memcpy(plain.data(), compressed.data(), plain.size());
        compatPakLog("READ_DONE: pak=%s method=STORE bytes=%zu",
                     pakPath.c_str(), plain.size());
        return true;
    }

    if (meta.method == 8) {
        const bool ok = pakInflateRaw(compressed.data(), compressed.size(),
                                      plain.data(), plain.size());
        compatPakLog("READ_DONE: pak=%s method=DEFLATE ok=%d bytes=%zu",
                     pakPath.c_str(), ok ? 1 : 0, plain.size());
        return ok;
    }

    compatPakLog("READ_REJECT_METHOD: pak=%s method=%u",
                 pakPath.c_str(), (unsigned)meta.method);
    return false;
}

// ─── Shader source discovery ────────────────────────────────────────────────
// Only check the expected loose paths. The old recursive scan walked the entire
// game tree (~12k entries) on every launch just to report that these files were
// not loose. CryPak can load shader content from PAKs, so a full-tree crawl is
// unnecessary for runtime and is kept out of startup.
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

// Level-local PAKs are mounted by CryPak at the level directory itself.
// Keep this lookup isolated from the global FCData search: changing the global
// root order/entry name would affect scripts and other shared assets.
static bool pakFindLevelLocalEntry(const std::string& wanted,
                                   std::string& pakPathOut,
                                   PakEntryMeta& metaOut) {
    pakPathOut.clear();
    metaOut = {};

    const std::string wantedNorm = pakNormalizeName(wanted.c_str());
    const size_t slash = wantedNorm.find_last_of('/');
    if (slash == std::string::npos)
        return false;

    const std::string requestedDir = wantedNorm.substr(0, slash);
    if (requestedDir.empty() ||
        requestedDir.rfind("levels/", 0) != 0) {
        return false;
    }

    // Resolve "levels/training" to the actual on-disk spelling without
    // modifying the pathname exposed to CryPak.
    std::string resolvedDir;
    if (!resolvePathCaseInsensitive(requestedDir.c_str(), resolvedDir))
        return false;

    DIR* dir = opendir(resolvedDir.c_str());
    if (!dir)
        return false;

    const std::string resolvedNorm = pakNormalizeName(resolvedDir.c_str());
    const std::string prefix = resolvedNorm + "/";
    std::string lookup = wantedNorm;
    if (wantedNorm.rfind(prefix, 0) == 0)
        lookup = wantedNorm.substr(prefix.size());

    while (dirent* ent = readdir(dir)) {
        const char* name = ent->d_name;
        if (!name)
            continue;

        const size_t len = std::strlen(name);
        if (len < 4)
            continue;

        const char c0 = (char)std::tolower((unsigned char)name[len - 4]);
        const char c1 = (char)std::tolower((unsigned char)name[len - 3]);
        const char c2 = (char)std::tolower((unsigned char)name[len - 2]);
        const char c3 = (char)std::tolower((unsigned char)name[len - 1]);
        if (c0 != '.' || c1 != 'p' || c2 != 'a' || c3 != 'k')
            continue;

        std::string pakPath = resolvedDir;
        if (!pakPath.empty() && pakPath.back() != '/')
            pakPath += '/';
        pakPath += name;

        PakEntryMeta meta;
        if (!pakFindEntryCached(pakPath, lookup, meta) &&
            lookup != wantedNorm &&
            !pakFindEntryCached(pakPath, wantedNorm, meta)) {
            continue;
        }

        pakPathOut = pakPath;
        metaOut = meta;
        closedir(dir);        return true;
    }

    closedir(dir);
    return false;
}

[[maybe_unused]] static void pakTraceMissDetails(const std::string& wanted) {
    const std::string::size_type basenamePos = wanted.find_last_of('/');
    const std::string wantedBase =
        basenamePos == std::string::npos ? wanted : wanted.substr(basenamePos + 1);

    // Cry3D constructs optional low-LOD names (foo_lod1.cgf, foo_lod2.cgf, ...)
    // from the base object foo.cgf. A missing external LOD companion is normal,
    // so keep a separate exact lookup for the corresponding base object.
    std::string lodBaseWanted;
    const std::string::size_type lodPos = wantedBase.rfind("_lod");
    if (lodPos != std::string::npos &&
        wantedBase.size() > lodPos + 8 &&
        wantedBase.compare(wantedBase.size() - 4, 4, ".cgf") == 0) {
        bool digitsOnly = true;
        for (std::string::size_type i = lodPos + 4; i < wantedBase.size() - 4; ++i) {
            if (wantedBase[i] < '0' || wantedBase[i] > '9') {
                digitsOnly = false;
                break;
            }
        }

        if (digitsOnly) {
            const std::string::size_type fullLodPos =
                wanted.size() - (wantedBase.size() - lodPos);
            lodBaseWanted = wanted.substr(0, fullLodPos) + ".cgf";
        }
    }

    size_t lodBaseMatches = 0;
    std::string firstLodBasePak;
    std::string firstLodBaseEntry;

    auto tracePak = [&](const std::string& pakPath) {
        mutexLock(&g_pak_index_lock);
        auto it = g_pak_indexes.find(pakPath);
        if (it == g_pak_indexes.end()) {
            PakIndex fresh;
            const bool ok = buildPakIndexLocked(pakPath, fresh);
            auto inserted = g_pak_indexes.emplace(pakPath, std::move(fresh));
            it = inserted.first;
            if (!ok) {
                mutexUnlock(&g_pak_index_lock);
                compatPakLog("PAK MEM TRACE INDEX: pak=%s build=FAIL",
                             pakPath.c_str());
                return;
            }
        }

        size_t baseMatches = 0;
        std::string firstMatch;
        for (const auto& item : it->second.entries) {
            const std::string& entry = item.first;
            const size_t slash = entry.find_last_of('/');
            const std::string base =
                slash == std::string::npos ? entry : entry.substr(slash + 1);
            if (base == wantedBase) {
                ++baseMatches;
                if (firstMatch.empty())
                    firstMatch = entry;
            }
        }

        const size_t entryCount = it->second.entries.size();
        const bool valid = it->second.valid;

        if (!lodBaseWanted.empty()) {
            auto baseIt = it->second.entries.find(lodBaseWanted);
            if (baseIt != it->second.entries.end()) {
                ++lodBaseMatches;
                if (firstLodBasePak.empty()) {
                    firstLodBasePak = pakPath;
                    firstLodBaseEntry = lodBaseWanted;
                }
            }
        }

        mutexUnlock(&g_pak_index_lock);

        if (baseMatches) {
            compatPakLog("PAK MEM TRACE INDEX: pak=%s valid=%d entries=%zu basename_matches=%zu first=%s",
                         pakPath.c_str(), valid ? 1 : 0, entryCount,
                         baseMatches, firstMatch.c_str());
        } else {
            compatPakLog("PAK MEM TRACE INDEX: pak=%s valid=%d entries=%zu basename_matches=0",
                         pakPath.c_str(), valid ? 1 : 0, entryCount);
        }
    };

    std::vector<std::string> activeLevelPaks;
    mutexLock(&g_pak_index_lock);
    activeLevelPaks = g_active_level_paks;
    mutexUnlock(&g_pak_index_lock);

    for (const std::string& pakPath : activeLevelPaks)
        tracePak(pakPath);

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

            std::string pakPath = roots[r];
            if (pakPath != ".")
                pakPath += "/";
            pakPath += name;

            bool alreadySeen = false;
            for (const std::string& active : activeLevelPaks) {
                if (active == pakPath) {
                    alreadySeen = true;
                    break;
                }
            }
            if (!alreadySeen)
                tracePak(pakPath);
        }

        closedir(dir);
    }

    if (!lodBaseWanted.empty()) {
        if (lodBaseMatches) {
            compatPakLog("PAK MEM TRACE LODBASE: wanted=%s matches=%zu first_pak=%s first=%s",
                         lodBaseWanted.c_str(), lodBaseMatches,
                         firstLodBasePak.c_str(), firstLodBaseEntry.c_str());
        } else {
            compatPakLog("PAK MEM TRACE LODBASE: wanted=%s matches=0",
                         lodBaseWanted.c_str());
        }
    }
}

static void getAnimationAliasCandidates(const std::string& wanted,
                                               std::vector<std::string>& candidates) {
    candidates.clear();

    if (wanted == "objects/characters/animations/shared/pidle_loop.caf") {
        candidates.emplace_back("objects/characters/animations/human_male/pidle_loop.caf");
    } else if (wanted == "objects/characters/animations/shared/humvee_passenger2_out.caf") {
        candidates.emplace_back("objects/characters/animations/vehicles/humvee_passenger2_out.caf");
    } else if (wanted == "objects/characters/animations/shared/humvee_passenger3_sit_loop.caf") {
        candidates.emplace_back("objects/characters/animations/vehicles/humvee_passenger3_sit_loop.caf");
    } else if (wanted == "objects/characters/animations/shared/humvee_gunner_in.caf") {
        candidates.emplace_back("objects/characters/animations/vehicles/humvee_gunner_in.caf");
    } else if (wanted == "objects/characters/animations/shared/humvee_passenger3_out.caf") {
        candidates.emplace_back("objects/characters/animations/vehicles/humvee_passenger3_out.caf");
    } else if (wanted == "objects/characters/animations/shared/humvee_passenger4_sit_loop.caf") {
        candidates.emplace_back("objects/characters/animations/vehicles/humvee_passenger4_sit_loop.caf");
    } else if (wanted == "objects/characters/animations/shared/humvee_passenger5_sit_loop.caf") {
        candidates.emplace_back("objects/characters/animations/vehicles/humvee_passenger5_sit_loop.caf");
    } else if (wanted == "objects/characters/animations/human_male/awalkfwd_loop.caf") {
        // The standard Far Cry animation list maps awalkfwd to xwalkfwd.
        // Keep this at the PAK layer so the animation record is untouched.
        candidates.emplace_back("objects/characters/animations/human_male/xwalkfwd_loop.caf");
    } else if (wanted == "objects/characters/animations/human_male/awalkfwd_upaim_loop.caf") {
        candidates.emplace_back("objects/characters/animations/human_male/xwalkfwd_upaim_loop.caf");
    } else if (wanted == "objects/characters/animations/human_male/awalkback_loop.caf") {
        // The standard Far Cry animation list uses xwalkback_loop.caf for
        // the awalkback animation. Keep the alias at the PAK layer so the
        // animation record itself remains untouched.
        candidates.emplace_back("objects/characters/animations/human_male/xwalkback_loop.caf");
    } else if (wanted == "objects/characters/animations/human_male/awalkback_utaim_loop.caf") {
        candidates.emplace_back("objects/characters/animations/human_male/xwalkback_utaim_loop.caf");
    } else if (wanted == "objects/characters/animations/human_male/awalkback_upaim_loop.caf") {
        candidates.emplace_back("objects/characters/animations/human_male/xwalkback_upaim_loop.caf");
    }
}

static thread_local std::string g_lastTexturePakName;
static Mutex g_textureDiagLock;
static std::string g_lastTexturePakNameGlobal;
static std::atomic<unsigned> g_textureUploadDiagCount{0};

static bool isTexturePakName(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    const std::string ext = name.substr(dot);
    return ext == ".dds" || ext == ".ddn" || ext == ".ddp" ||
           ext == ".ddt" || ext == ".tga" || ext == ".jpg";
}

static bool isInterestingTextureDiagName(const std::string& name) {
    static const char* const needles[] = {
        "causq", "caust", "water_bubbles2", "w01blue03",
        "water_splash", "concrete", "iron", "rust", "moss",
        "stone", "metal", "door", "cylinderbump", "fresnel14"
    };
    for (const char* needle : needles) {
        if (name.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

static bool pakFindVirtualEntry(const char* requested,
                               std::string& pakPathOut,
                               PakEntryMeta& metaOut) {
    pakPathOut.clear();
    metaOut = {};

    if (!requested || !*requested)
        return false;

    const std::string wanted = pakAssetRelativeName(requested);
    if (wanted.empty()) {
        compatPakLog("QUERY INVALID: requested=%s normalized=<empty>",
                     requested ? requested : "<null>");
        return false;
    }

    compatPakLog("QUERY: requested=%s normalized=%s",
                 requested, wanted.c_str());

    // Keep the diagnostic texture candidate alive across unrelated CryPak
    // lookups. The old code cleared it for every request, so a shader/CAF/etc.
    // query between a DDS lookup and glTexImage2D erased the texture name.
    // Keep both a TLS copy (most precise) and a global fallback for GL work
    // that happens on another thread.
    if (isTexturePakName(wanted) &&
        isInterestingTextureDiagName(wanted)) {
        g_lastTexturePakName = wanted;
        mutexLock(&g_textureDiagLock);
        g_lastTexturePakNameGlobal = wanted;
        mutexUnlock(&g_textureDiagLock);
    }

    // Check the positive lookup cache before touching the filesystem or
    // rescanning level-local PAKs. rememberActiveLevelPak() clears this cache
    // whenever a newly opened level PAK can change the lookup result, so a
    // cached hit remains valid while avoiding repeated opendir()/index work
    // for the thousands of CGF/CAF accesses performed during level loading.
    {
        mutexLock(&g_pak_index_lock);
        auto hit = g_pak_lookup_cache.find(wanted);
        if (hit != g_pak_lookup_cache.end()) {
            pakPathOut = hit->second.pakPath;
            metaOut = hit->second.meta;
            compatPakLog("QUERY CACHE_HIT: wanted=%s pak=%s c=%u u=%u method=%u local=%u",
                         wanted.c_str(), pakPathOut.c_str(),
                         (unsigned)metaOut.compressedSize,
                         (unsigned)metaOut.uncompressedSize,
                         (unsigned)metaOut.method,
                         (unsigned)metaOut.localOffset);
            mutexUnlock(&g_pak_index_lock);
            return true;
        }
        mutexUnlock(&g_pak_index_lock);
        compatPakLog("QUERY CACHE_MISS: wanted=%s", wanted.c_str());
    }

    std::vector<std::string> activeLevelPaks;
    {
        mutexLock(&g_pak_index_lock);
        activeLevelPaks = g_active_level_paks;
        mutexUnlock(&g_pak_index_lock);
    }

    compatPakLog("LEVEL_PAK_SNAPSHOT: wanted=%s count=%zu",
                 wanted.c_str(), activeLevelPaks.size());
    for (const std::string& levelPak : activeLevelPaks) {
        compatPakLog("LEVEL_PAK_CHECK: wanted=%s pak=%s",
                     wanted.c_str(), levelPak.c_str());
        if (pakFindEntryCached(levelPak, wanted, metaOut)) {
            pakPathOut = levelPak;
            compatPakLog("LEVEL_PAK_HIT: wanted=%s pak=%s c=%u u=%u method=%u local=%u",
                         wanted.c_str(), pakPathOut.c_str(),
                         (unsigned)metaOut.compressedSize,
                         (unsigned)metaOut.uncompressedSize,
                         (unsigned)metaOut.method,
                         (unsigned)metaOut.localOffset);
            mutexLock(&g_pak_index_lock);
            g_pak_lookup_cache[wanted] =
                PakLookupCacheEntry{pakPathOut, metaOut};
            g_pak_lookup_misses.erase(wanted);
            mutexUnlock(&g_pak_index_lock);
            return true;
        }
        compatPakLog("LEVEL_PAK_MISS: wanted=%s pak=%s",
                     wanted.c_str(), levelPak.c_str());
    }

    compatPakLog("LEVEL_LOCAL_LOOKUP: wanted=%s", wanted.c_str());
    if (pakFindLevelLocalEntry(wanted, pakPathOut, metaOut)) {
        compatPakLog("LEVEL_LOCAL_HIT: wanted=%s pak=%s c=%u u=%u method=%u local=%u",
                     wanted.c_str(), pakPathOut.c_str(),
                     (unsigned)metaOut.compressedSize,
                     (unsigned)metaOut.uncompressedSize,
                     (unsigned)metaOut.method,
                     (unsigned)metaOut.localOffset);
        mutexLock(&g_pak_index_lock);
        g_pak_lookup_cache[wanted] =
            PakLookupCacheEntry{pakPathOut, metaOut};
        g_pak_lookup_misses.erase(wanted);
        mutexUnlock(&g_pak_index_lock);
        return true;
    }
    compatPakLog("LEVEL_LOCAL_MISS: wanted=%s", wanted.c_str());

    {
        mutexLock(&g_pak_index_lock);
        const bool wantedIsCaf =
            wanted.size() >= 4 &&
            wanted.compare(wanted.size() - 4, 4, ".caf") == 0;
        if (!wantedIsCaf &&
            g_pak_lookup_misses.find(wanted) != g_pak_lookup_misses.end()) {
            compatPakLog("QUERY NEGATIVE_CACHE_HIT: wanted=%s", wanted.c_str());
            mutexUnlock(&g_pak_index_lock);
            return false;
        }
        mutexUnlock(&g_pak_index_lock);
    }

    const std::vector<std::string> globalPaks =
        getGlobalPakPathsSnapshot();

    compatPakLog("GLOBAL_PAK_SNAPSHOT: wanted=%s count=%zu",
                 wanted.c_str(), globalPaks.size());

    // CryPak loads PAKs alphabetically and later PAKs override earlier ones.
    // Search in reverse order to preserve that priority.
    for (auto it = globalPaks.rbegin(); it != globalPaks.rend(); ++it) {
        const std::string& pakPath = *it;
        PakEntryMeta meta;
        compatPakLog("GLOBAL_PAK_CHECK: wanted=%s pak=%s",
                     wanted.c_str(), pakPath.c_str());
        if (!pakFindEntryCached(pakPath, wanted, meta)) {
            compatPakLog("GLOBAL_PAK_MISS: wanted=%s pak=%s",
                         wanted.c_str(), pakPath.c_str());
            continue;
        }

        compatPakLog("GLOBAL_PAK_HIT: wanted=%s pak=%s c=%u u=%u method=%u local=%u",
                     wanted.c_str(), pakPath.c_str(),
                     (unsigned)meta.compressedSize,
                     (unsigned)meta.uncompressedSize,
                     (unsigned)meta.method,
                     (unsigned)meta.localOffset);

        pakPathOut = pakPath;
        metaOut = meta;

        mutexLock(&g_pak_index_lock);
        g_pak_lookup_cache[wanted] =
            PakLookupCacheEntry{pakPathOut, metaOut};
        g_pak_lookup_misses.erase(wanted);
        mutexUnlock(&g_pak_index_lock);

        if (wanted.size() >= 4 &&
            wanted.compare(wanted.size() - 4, 4, ".caf") == 0) {
            const unsigned n =
                g_cafLookupDiagEvents.fetch_add(1);
            if (n < 64) {
                compatPakLog("PAK CAF HIT: %s <- %s size=%u method=%u",
                             wanted.c_str(), pakPathOut.c_str(),
                             (unsigned)metaOut.uncompressedSize,
                             (unsigned)metaOut.method);
            }
        }
        return true;
    }

    // Some Android Far Cry data builds contain stale/relocated
    // animation filenames in the .cal tables. The canonical CAF is still
    // present in Objects.pak, but under its original animation directory.
    // Resolve only the four verified compatibility aliases instead of doing
    // a broad basename/fuzzy search.
    {
        std::vector<std::string> aliases;
        getAnimationAliasCandidates(wanted, aliases);
        if (!aliases.empty())
            compatPakLog("ALIAS_CANDIDATES: wanted=%s count=%zu",
                         wanted.c_str(), aliases.size());
        for (const std::string& alias : aliases) {
            compatPakLog("ALIAS_CANDIDATE: wanted=%s alias=%s",
                         wanted.c_str(), alias.c_str());
            // Some canonical animation paths have already been resolved by
            // the normal preload pass. Reuse that positive lookup before
            // rebuilding the PAK index search for the alias candidate.
            {
                mutexLock(&g_pak_index_lock);
                auto cachedAlias = g_pak_lookup_cache.find(alias);
                if (cachedAlias != g_pak_lookup_cache.end()) {
                    pakPathOut = cachedAlias->second.pakPath;
                    metaOut = cachedAlias->second.meta;
                    mutexUnlock(&g_pak_index_lock);
                    compatPakLog("PAK ANIM ALIAS CACHE HIT: %s -> %s <- %s size=%u",
                                 wanted.c_str(), alias.c_str(), pakPathOut.c_str(),
                                 (unsigned)metaOut.uncompressedSize);
                    return true;
                }
                mutexUnlock(&g_pak_index_lock);
            }

            for (const std::string& levelPak : activeLevelPaks) {
                if (pakFindEntryCached(levelPak, alias, metaOut)) {
                    pakPathOut = levelPak;
                    mutexLock(&g_pak_index_lock);
                    g_pak_lookup_cache[wanted] =
                        PakLookupCacheEntry{pakPathOut, metaOut};
                    mutexUnlock(&g_pak_index_lock);
                    compatPakLog("PAK ANIM ALIAS HIT: %s -> %s <- %s size=%u",
                                 wanted.c_str(), alias.c_str(), pakPathOut.c_str(),
                                 (unsigned)metaOut.uncompressedSize);
                    return true;
                }
            }

            const std::vector<std::string> globalPaks =
                getGlobalPakPathsSnapshot();
            for (auto it = globalPaks.rbegin(); it != globalPaks.rend(); ++it) {
                if (!pakFindEntryCached(*it, alias, metaOut))
                    continue;

                pakPathOut = *it;
                mutexLock(&g_pak_index_lock);
                g_pak_lookup_cache[wanted] =
                    PakLookupCacheEntry{pakPathOut, metaOut};
                mutexUnlock(&g_pak_index_lock);
                compatPakLog("PAK ANIM ALIAS HIT: %s -> %s <- %s size=%u",
                             wanted.c_str(), alias.c_str(), pakPathOut.c_str(),
                             (unsigned)metaOut.uncompressedSize);
                return true;
            }
        }
    }

    const bool wantedIsCaf =
        wanted.size() >= 4 &&
        wanted.compare(wanted.size() - 4, 4, ".caf") == 0;
    if (!wantedIsCaf) {
        mutexLock(&g_pak_index_lock);
        g_pak_lookup_misses.emplace(wanted);
        mutexUnlock(&g_pak_index_lock);
    }

    if (wanted.size() >= 4 &&
        wanted.compare(wanted.size() - 4, 4, ".caf") == 0) {
        const unsigned n =
            g_cafLookupDiagEvents.fetch_add(1);
        if (n < 64) {
            compatPakLog("PAK CAF MISS: %s", wanted.c_str());
        }
    }

    compatPakLog("QUERY FINAL_MISS: requested=%s normalized=%s",
                 requested, wanted.c_str());
    return false;
}

// Return the uncompressed size seen by the Android stream engine without
// opening/extracting the PAK entry. CRefStreamEngine::GetFileSize() in the
// Android source contains an unfinished __linux stub that returns 0 after
// fopen(), which makes every PAK-backed .caf/.cgf look missing to the
// animation/model loaders.
// The retail symbol is mangled as a member, but the observed ARM64 call site
// can also reach the Linux implementation with x0 already holding the filename
// (x1 then contains the flags, not a char*). Accept both forms:
//   member ABI:   x0=this, x1=path, x2=flags
//   direct/helper: x0=path, x1=flags
static const char* compatGetFileSizePath(void* a0, const char* a1) {
    auto readableString = [](const void* p) -> const char* {
        if (!p)
            return nullptr;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        MemoryInfo mi = {};
        u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)))
            return nullptr;
        if (!(mi.perm & Perm_R))
            return nullptr;
        const uintptr_t endAddr = mi.addr + mi.size;
        if (addr >= endAddr)
            return nullptr;
        const size_t maxLen = std::min<size_t>(256, endAddr - addr);
        const char* str = reinterpret_cast<const char*>(p);
        for (size_t i = 0; i < maxLen; ++i) {
            const unsigned char ch = (unsigned char)str[i];
            if (ch == 0)
                return i ? str : nullptr;
            if (ch < 0x20 || ch > 0x7e)
                return nullptr;
        }
        return nullptr;
    };

    const char* p0 = readableString(a0);
    const char* p1 = readableString(a1);

    // Normal ABI: x0=this, x1=path. Compatibility/direct ABI: x0=path,
    // x1=flags. Prefer a string that actually looks like a filesystem path;
    // this prevents a coincidentally readable x1 value from being mistaken
    // for the filename.
    auto looksLikePath = [](const char* p) -> bool {
        if (!p || !*p)
            return false;
        if (std::strchr(p, '/') || std::strchr(p, '\\'))
            return true;
        const char* dot = std::strrchr(p, '.');
        return dot && dot != p && dot[1] != 0;
    };

    if (looksLikePath(p1))
        return p1;
    if (looksLikePath(p0))
        return p0;
    return p1 ? p1 : p0;
}

static bool isCriticalCafDiagPath(const char* path) {
    if (!path || !*path)
        return false;

    std::string normalized(path);
    for (char& c : normalized) {
        if (c == '\\')
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }

    return normalized.find("objects/characters/animations/shared/pidle_loop.caf") != std::string::npos ||
           normalized.find("objects/characters/animations/shared/humvee_passenger2_out.caf") != std::string::npos ||
           normalized.find("objects/characters/animations/shared/humvee_passenger3_sit_loop.caf") != std::string::npos ||
           normalized.find("objects/characters/animations/shared/humvee_passenger5_sit_loop.caf") != std::string::npos ||
           normalized.find("objects/characters/animations/human_male/heavy_runfwd_usaim_loop.bip.caf") != std::string::npos ||
           normalized.find("objects/characters/animations/human_male/awalkback_loop.caf") != std::string::npos;
}

extern "C" unsigned compatGuestGetFileSize(void* a0, const char* a1, unsigned a2) {
    static unsigned g_cafDiag = 0;
    static unsigned g_cafMissDiag = 0;

    const char* requested = compatGetFileSizePath(a0, a1);
    bool isCaf = false;
    if (requested && *requested) {
        const size_t n = std::strlen(requested);
        if (n >= 4) {
            const char c0 = (char)std::tolower((unsigned char)requested[n - 4]);
            const char c1 = (char)std::tolower((unsigned char)requested[n - 3]);
            const char c2 = (char)std::tolower((unsigned char)requested[n - 2]);
            const char c3 = (char)std::tolower((unsigned char)requested[n - 1]);
            isCaf = (c0 == '.' && c1 == 'c' && c2 == 'a' && c3 == 'f');
        }
    }

    if (!requested || !*requested)
        return 0;

    const std::string pathStorage = normalizeSwitchFsPath(requested);
    const std::string normalizedPath = pakNormalizeName(pathStorage.c_str());
    const char* path = pathStorage.c_str();
    const bool criticalCaf = isCaf && isCriticalCafDiagPath(requested);
    if (criticalCaf) {
        compatLogFmt("FARCRY CAF GS CALL: a0=%p a1=%p requested=%s normalized=%s",
                     a0, (const void*)a1, requested, normalizedPath.c_str());
    }

    if (isCaf) {
        mutexLock(&g_pak_index_lock);
        auto cached = g_caf_size_cache.find(normalizedPath);
        if (cached != g_caf_size_cache.end()) {
            const unsigned size = cached->second;
            mutexUnlock(&g_pak_index_lock);
            if (criticalCaf)
                compatLogFmt("FARCRY CAF GS CACHE HIT: %s size=%u", requested, size);
            return size;
        }
        mutexUnlock(&g_pak_index_lock);
    }

    const bool diag = isCaf && g_cafDiag < 128;

    struct stat st = {};
    if (::stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
        const unsigned size =
            (unsigned)std::min<off_t>(st.st_size, (off_t)UINT_MAX);
        if (isCaf) {
            mutexLock(&g_pak_index_lock);
            g_caf_size_cache[normalizedPath] = size;
            mutexUnlock(&g_pak_index_lock);
        }
        if (criticalCaf)
            compatLogFmt("FARCRY CAF GS LOOSE HIT: %s size=%u", requested, size);
        return size;
    }

    std::string resolved;
    if (resolvePathCaseInsensitive(path, resolved) && resolved != path) {
        st = {};
        if (::stat(resolved.c_str(), &st) == 0 &&
            S_ISREG(st.st_mode) && st.st_size >= 0) {
            const unsigned size =
                (unsigned)std::min<off_t>(st.st_size, (off_t)UINT_MAX);
            if (isCaf) {
                mutexLock(&g_pak_index_lock);
                g_caf_size_cache[normalizedPath] = size;
                mutexUnlock(&g_pak_index_lock);
            }
            if (criticalCaf)
                compatLogFmt("FARCRY CAF GS CASEFIX HIT: %s -> %s size=%u",
                             requested, resolved.c_str(), size);
            return size;
        }
    }

    std::string pakPath;
    PakEntryMeta meta;
    if (pakFindVirtualEntry(path, pakPath, meta)) {
        if (isCaf) {
            mutexLock(&g_pak_index_lock);
            g_caf_size_cache[normalizedPath] = meta.uncompressedSize;
            mutexUnlock(&g_pak_index_lock);
        }
        if (diag) {
            compatLogFmt("FARCRY GETFILESIZE HIT: %s size=%u pak=%s",
                         requested,
                         (unsigned)meta.uncompressedSize,
                         pakPath.c_str());
            ++g_cafDiag;
        }
        if (criticalCaf) {
            compatLogFmt("FARCRY CAF GS PAK HIT: %s size=%u pak=%s",
                         requested, (unsigned)meta.uncompressedSize,
                         pakPath.c_str());
        }
        return meta.uncompressedSize;
    }

    if (isCaf && g_cafMissDiag < 256) {
        compatLogFmt("FARCRY GETFILESIZE MISS: %s normalized=%s",
                     requested, normalizedPath.c_str());
        ++g_cafMissDiag;
    }
    if (criticalCaf)
        compatLogFmt("FARCRY CAF GS MISS: %s normalized=%s",
                     requested, normalizedPath.c_str());
    return 0;
}


static FILE* tryOpenFromPaks(const char* requested, const char* mode) {
    if (!requested || !mode || mode[0] != 'r')
        return nullptr;

    const bool trace = pakMemoryTraceBudget(requested);
    const std::string wantedTrace = pakAssetRelativeName(requested);

    std::string pakPath;
    PakEntryMeta meta;
    if (!pakFindVirtualEntry(requested, pakPath, meta)) {
        if (trace) {
            compatPakLog("PAK MEM TRACE MISS: requested=%s wanted=%s",
                         requested, wantedTrace.c_str());
        }
        return nullptr;
    }
    if (trace) {
        compatPakLog("PAK MEM TRACE FIND: requested=%s wanted=%s pak=%s c=%u u=%u method=%u local=%u",
                     requested,
                     wantedTrace.c_str(),
                     pakPath.c_str(),
                     meta.compressedSize,
                     meta.uncompressedSize,
                     (unsigned)meta.method,
                     meta.localOffset);
    }

    std::shared_ptr<std::vector<unsigned char>> data;
    bool cacheHit = false;
    if (!pakGetMemory(pakPath, meta, data, cacheHit)) {
        if (trace) {
            compatPakLog("PAK MEM TRACE READ_FAILED: %s <- %s",
                         wantedTrace.c_str(), pakPath.c_str());
        } else {
            compatPakLog("PAK VIRTUAL READ FAILED: %s <- %s",
                         wantedTrace.c_str(),
                         pakPath.c_str());
        }
        return nullptr;
    }

    if (trace) {
        compatPakLog("PAK MEM TRACE %s: %s plain=%p plain_size=%zu pak=%s",
                     cacheHit ? "CACHE" : "DECOMP",
                     wantedTrace.c_str(),
                     (void*)data->data(),
                     data->size(),
                     pakPath.c_str());
    }

    FILE* handle = vpakOpen(std::move(data), requested, trace);
    if (trace && handle) {
        VirtualPakFile* v = reinterpret_cast<VirtualPakFile*>(handle);
        compatPakLog("PAK MEM TRACE VFILE[%s]: %s handle=%p src=%p size=%zu",
                     cacheHit ? "CACHE" : "DECOMP",
                     wantedTrace.c_str(),
                     (void*)handle,
                     (void*)v->data->data(),
                     v->data->size());
    }
    return handle;
}


// ─── Startup shader overlay ───────────────────────────────────────────────────
// Show shader file lookups directly on the active Switch GL surface during
// engine startup. This is limited to the first 64 shader-related fopen calls.
[[maybe_unused]] static unsigned g_shader_overlay_events = 0;

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

[[maybe_unused]] static void startupDrawText(float x, float y, float scale, const char* text) {
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

// Far Cry creates $AlphaGradient procedurally in the original renderer
// (256x1, eTF_8000, values 0..255). The Android texture loader can still ask
// its filesystem layer for "textures/$alphagradient.dds", so provide the same
// data as a tiny legacy DDS luminance texture instead of reporting a real miss.
static bool isSyntheticAlphaGradientDds(const char* path) {
    if (!path || !*path)
        return false;

    std::string normalized = asciiLower(path);
    for (char& c : normalized) {
        if ((unsigned char)c == 92)
            c = '/';
    }

    const size_t slash = normalized.find_last_of('/');
    const std::string base = slash == std::string::npos
        ? normalized
        : normalized.substr(slash + 1);
    return base == "$alphagradient.dds";
}

static FILE* makeSyntheticAlphaGradientDds() {
    // DDS header (128 bytes including the magic) + 256 bytes of 8-bit
    // luminance. The original Far Cry loader maps DDS_LUMINANCE/8-bit to
    // eTF_8000, which is exactly the format used by $AlphaGradient.
    std::vector<unsigned char> blob(128u + 256u, 0);

    auto put32 = [&](size_t off, uint32_t value) {
        blob[off + 0] = (unsigned char)(value & 0xffu);
        blob[off + 1] = (unsigned char)((value >> 8) & 0xffu);
        blob[off + 2] = (unsigned char)((value >> 16) & 0xffu);
        blob[off + 3] = (unsigned char)((value >> 24) & 0xffu);
    };

    blob[0] = 'D'; blob[1] = 'D'; blob[2] = 'S'; blob[3] = ' ';
    put32(4,   124u);       // dwSize
    put32(8,   0x100Fu);    // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    put32(12,  1u);         // height
    put32(16,  256u);       // width
    put32(20,  256u);       // pitch
    put32(24,  0u);         // depth
    put32(28,  1u);         // mip map count

    put32(76,  32u);        // DDS_PIXELFORMAT.dwSize
    put32(80,  0x00020000u); // DDS_LUMINANCE
    put32(84,  0u);         // fourCC
    put32(88,  8u);         // RGB bit count
    put32(92,  0x000000FFu); // red/luminance mask
    put32(96,  0u);
    put32(100, 0u);
    put32(104, 0u);
    put32(108, 0x1000u);    // DDSCAPS_TEXTURE

    for (size_t i = 0; i < 256u; ++i)
        blob[128u + i] = (unsigned char)i;

    // Do not use tmpfile() here. On the Switch target there is no guarantee
    // that newlib can create a native temporary FILE*. Reuse the same virtual
    // FILE abstraction as PAK-backed streams instead; fread/fseek/fgetc/fclose
    // already route through vpak* for these handles.
    auto data = std::make_shared<std::vector<unsigned char>>(std::move(blob));
    FILE* f = vpakOpen(std::move(data), "textures/$AlphaGradient.dds", false);
    if (!f)
        return nullptr;

    static bool logged = false;
    if (!logged) {
        logged = true;
        compatLog("GL/TEX COMPAT: synthetic textures/$AlphaGradient.dds (256x1 luminance alpha gradient)");
    }
    return f;
}

// fopen wrapper — logs failed opens so we can see what paths game code requests
static FILE* stub_fopen(const char* path, const char* mode) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    rememberActiveLevelPak(ioPath);

    const bool shaderIo = isShaderPathForDiag(ioPath);
    const bool videoIo =
        ioPath && (shaderPathHasExt(ioPath, ".bik") ||
                   shaderPathHasExt(ioPath, ".avi"));

    if (isSyntheticAlphaGradientDds(ioPath)) {
        if (FILE* synthetic = makeSyntheticAlphaGradientDds())
            return synthetic;
    }

    if (path && ioPathStorage != path && !shaderIo)
    if (std::string mapped = obbRemap(ioPath); !mapped.empty()) {
        FILE* mf = fopen(mapped.c_str(), mode);
        compatLogFmt("obb: fopen %s -> %s (%s)", ioPath ? ioPath : "?",
                     mapped.c_str(), mf ? "ok" : "still not there");
        if (mf) { if (videoIo) g_near_video_open_failed = 0; setvbuf(mf, nullptr, _IOFBF, 64 * 1024); return mf; }
    }

    FILE* f = fopen(ioPath, mode);

    if (!f && ioPath) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) && resolved != ioPath) {
            FILE* rf = fopen(resolved.c_str(), mode);
            if (rf) {
                if (videoIo) g_near_video_open_failed = 0;
                if (!shaderIo)                f = rf;
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
                if (!shaderIo)                f = rf;
            }
        }
    }

    if (!f) {
        // Shader-cache files are not language assets, but valid precompiled
        // .cgps/.cgvp/.cgasm entries may be present in the game's Shaders.pak.
        // The renderer can consume them directly through the virtual PAK FILE
        // path; only an actual miss should fall back to the embedded ARB shader.
        FILE* pakFile = tryOpenFromPaks(ioPath, mode);
        if (pakFile) {
            if (videoIo) g_near_video_open_failed = 0;
            if (isShaderCacheLookupPath(ioPath)) {
                static std::atomic<unsigned> shaderCachePakHits{0};
                const unsigned hit = shaderCachePakHits.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                if (hit <= 32)
                    compatLogFmt("shader cache: PAK hit %u -> %s", hit,
                                 ioPath ? ioPath : "?");
            }
            return pakFile;
        }

        if (videoIo && isOptionalMissingVideoPath(ioPath)) {
            g_near_video_open_failed = 0;
            compatLogFmt("VIDEO SKIP OPTIONAL: %s (missing, continuing)",
                         ioPath ? ioPath : "?");
            return nullptr;
        }

        if (videoIo)
            g_near_video_open_failed = 1;

        if (videoIo)
            compatLogFmt("VIDEO OPEN FAIL: %s (mode=%s)",
                         ioPath ? ioPath : "?", mode ? mode : "?");
        return f;
    }


    if (!shaderIo && !vpakOwns(f))
        logShaderScriptDiagnostics(f, ioPath);

    if (videoIo)
        g_near_video_open_failed = 0;

    if (apkcache::adopt(f, ioPath)) {
        setvbuf(f, nullptr, _IOFBF, 16 * 1024);
        if (!shaderIo)        return f;
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
    if (vpakOwns(f))
        return vpakRead(f, p, sz, n);
    if (apkcache::owns(f))
        return apkcache::read(f, p, sz, n);
    return fread(p, sz, n, f);
}
static int sh_fseek(FILE* f, long off, int whence) {
    if (vpakOwns(f))
        return vpakSeek(f, (int64_t)off, whence);
    if (apkcache::owns(f))
        return apkcache::seek(f, (int64_t)off, whence);
    return fseek(f, off, whence);
}
static long sh_ftell(FILE* f) {
    if (vpakOwns(f))
        return (long)vpakTell64(f);
    if (apkcache::owns(f))
        return (long)apkcache::tell(f);
    return ftell(f);
}
static int sh_fgetc(FILE* f) {
    if (vpakOwns(f))
        return vpakGetc(f);
    if (apkcache::owns(f))
        return apkcache::getc(f);
    return fgetc(f);
}

// Keep fgets on the same stream abstraction as fread/fgetc. CryPak-backed
// streams are not native libc FILEs from the engine's point of view, so routing
// guest fgets through sh_fgetc() avoids feeding an APK/Pak stream to newlib's
// native fgets implementation. The first few calls are logged for startup
// diagnostics so a stdio fault can be separated from the later engine code.
static char* sh_fgets(char* dst, int n, FILE* f) {
    static unsigned int diag_count = 0;

    if (!dst || n <= 0 || !f) {
        if (diag_count < 8) {
            compatLogFmt("FGETS invalid: dst=%p n=%d file=%p",
                         (void*)dst, n, (void*)f);
            ++diag_count;
        }
        return nullptr;
    }

    const unsigned int diag = diag_count;
    if (diag_count < 8) {
        compatLogFmt("FGETS[%u] enter: dst=%p n=%d file=%p apk=%d",
                     diag, (void*)dst, n, (void*)f,
                     apkcache::owns(f) ? 1 : 0);
    }

    int i = 0;
    while (i < n - 1) {
        const int ch = sh_fgetc(f);
        if (ch == EOF)
            break;

        dst[i++] = (char)ch;
        if (ch == '\n')
            break;
    }

    if (i == 0) {
        if (diag_count < 8) {
            compatLogFmt("FGETS[%u] exit: EOF", diag);
            ++diag_count;
        }
        return nullptr;
    }

    dst[i] = '\0';

    if (diag_count < 8) {
        compatLogFmt("FGETS[%u] exit: len=%d text=%s", diag, i, dst);
        ++diag_count;
    }

    return dst;
}

static int sh_feof(FILE* f) {
    if (vpakOwns(f))
        return vpakEof(f);
    if (apkcache::owns(f))
        return apkcache::eof(f);
    return feof(f);
}
static void sh_rewind(FILE* f) {
    if (vpakOwns(f)) {
        (void)vpakSeek(f, 0, SEEK_SET);
        return;
    }
    if (apkcache::owns(f)) {
        (void)apkcache::seek(f, 0, SEEK_SET);
        return;
    }
    rewind(f);
}
static int sh_fclose(FILE* f) {
    if (vpakOwns(f))
        return vpakClose(f);
    apkcache::close(f);   // no-op unless this stream was cached
    return fclose(f);
}

// open() wrapper — Android-compatible path resolution for guest stdio/file-stream users
static int stub_open(const char* path, int flags, ...) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    rememberActiveLevelPak(ioPath);

    const bool videoIo =
        ioPath && (shaderPathHasExt(ioPath, ".bik") ||
                   shaderPathHasExt(ioPath, ".avi"));

    if (path && ioPathStorage != path) {
        const int vfd = devUrandomOpen(ioPath);
        if (vfd >= 0)
            return vfd;
    }

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
        compatLogFmt("obb: open %s -> %s (fd=%d)",
                     ioPath ? ioPath : "?", mapped.c_str(), mfd);
        if (mfd >= 0) {
            if (videoIo) g_near_video_open_failed = 0;
            return mfd;
        }
    }

    const bool shaderSourceOpen =
        ioPath &&
        (shaderPathHasExt(ioPath, ".csl") ||
         shaderPathHasExt(ioPath, ".csi") ||
         shaderPathHasExt(ioPath, ".crycg"));

    if (shaderSourceOpen)
        compatLogFmt("open SHADER REQUEST: path=%s flags=0x%x", ioPath, flags);

    int fd = doOpen(ioPath);

    if (shaderSourceOpen)
        compatLogFmt("open SHADER DIRECT: path=%s result=%s fd=%d",
                     ioPath, fd >= 0 ? "OK" : "FAIL", fd);

    if (fd < 0 && ioPath) {
        std::string resolved;
        if (resolvePathCaseInsensitive(ioPath, resolved) &&
            resolved != ioPath) {
            int rfd = doOpen(resolved.c_str());
            if (rfd >= 0) {
                compatLogFmt("open CASEFIX: %s -> %s fd=%d",
                             ioPath, resolved.c_str(), rfd);
                if (videoIo) g_near_video_open_failed = 0;
                return rfd;
            }
        }
    }

    if (fd < 0 && ioPath) {
        const int pakFd = vpakFdOpen(ioPath, flags);
        if (pakFd >= 0) {
            if (videoIo)
                g_near_video_open_failed = 0;
            return pakFd;
        }
    }

    if (fd < 0) {
        if (videoIo && isOptionalMissingVideoPath(ioPath)) {
            g_near_video_open_failed = 0;
            compatLogFmt("VIDEO SKIP OPTIONAL: %s (missing, continuing)",
                         ioPath ? ioPath : "?");
            return fd;
        }

        if (videoIo)
            g_near_video_open_failed = 1;
        compatLogFmt("open FAIL: %s flags=0x%x", ioPath ? ioPath : "?", flags);
    } else if (videoIo) {
        g_near_video_open_failed = 0;
    }

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

    if (isCompressedBumpWarning(buf))
        return (int)strlen(buf);

    // Never throttle the exact CryEngine main-loop marker. runtime.cpp uses it
    // as the definitive point where startup diagnostics end and the log is
    // permanently closed.
    const bool main_loop_marker =
        std::strstr(buf, "CXGame::Run: entered main game loop") != nullptr;
    if (main_loop_marker || androidLogThrottleOk())
        compatLogFmt("[%s] %s", tag ? tag : "?", buf);
    return (int)strlen(buf);
}
static int android_log_write(int, const char* tag, const char* msg) {
    if (isCompressedBumpWarning(msg))
        return 0;

    const bool main_loop_marker =
        msg && std::strstr(msg, "CXGame::Run: entered main game loop") != nullptr;
    if (main_loop_marker || androidLogThrottleOk())
        compatLogFmt("[%s] %s", tag ? tag : "?", msg ? msg : "");
    return 0;
}
static int android_log_vprint(int, const char* tag, const char* fmt, va_list va) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, va);

    if (isCompressedBumpWarning(buf))
        return (int)strlen(buf);

    const bool main_loop_marker =
        std::strstr(buf, "CXGame::Run: entered main game loop") != nullptr;
    if (main_loop_marker || androidLogThrottleOk())
        compatLogFmt("[%s] %s", tag ? tag : "?", buf);
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
// pthread keys are process-global, but every key's value is thread-local.
// The old implementation used one global value array for every thread. That is
// incorrect for Bionic/POSIX and lets one CryEngine worker thread consume another
// thread's locale/EH/TLS state.
//
// Keep a fixed, allocation-free table keyed by the current libnx Thread handle.
// Allocation-free lookup is important because pthread TLS can be queried from
// inside allocator/libc internals where taking a C++ heap allocation would be
// unsafe.
static constexpr size_t PTHREAD_TLS_ROWS = 32;

struct PthreadTlsRow {
    std::atomic<uintptr_t> owner;
    void* values[64];
    uint8_t scratch[64][512];
};

static PthreadTlsRow g_pthread_tls[PTHREAD_TLS_ROWS] = {};
static int g_tls_key_count = 0;

static PthreadTlsRow* pthreadTlsRowForCurrent() {
    const uintptr_t self =
        static_cast<uintptr_t>(threadGetCurHandle());
    if (!self)
        return nullptr;

    for (size_t i = 0; i < PTHREAD_TLS_ROWS; ++i) {
        if (g_pthread_tls[i].owner.load(std::memory_order_acquire) == self)
            return &g_pthread_tls[i];
    }

    for (size_t i = 0; i < PTHREAD_TLS_ROWS; ++i) {
        uintptr_t expected = 0;
        if (g_pthread_tls[i].owner.compare_exchange_strong(
                expected, self, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return &g_pthread_tls[i];
        }
    }

    // More than 32 simultaneously-live guest threads is not expected by the
    // Android game. Failing here is preferable to cross-wiring TLS state.
    compatLogFmt("pthread TLS: no free row for thread=%p",
                 (void*)self);
    return nullptr;
}

static int pt_key_create(int* k, void (*dtor)(void*)) {
    if (!k || g_tls_key_count >= 64)
        return 11; // EAGAIN
    *k = g_tls_key_count++;
    compatLogFmt("pthread_key_create → key=%d dtor=%p", *k, (void*)dtor);
    return 0;
}
static int pt_key_delete(int) { return 0; }

static void* pt_getspecific(int k) {
    if (k < 0 || k >= 64)
        return nullptr;

    PthreadTlsRow* row = pthreadTlsRowForCurrent();
    if (!row)
        return nullptr;

    void* v = row->values[k];
    if (!v) {
        // Preserve the old compatibility behavior for callers that assume
        // Bionic's locale/EH TLS object exists, but make the scratch state
        // genuinely thread-local.
        v = row->scratch[k];
        row->values[k] = v;
        compatLogFmt("pthread_getspecific(key=%d) → thread-local scratch=%p",
                     k, v);
    }
    return v;
}

static int pt_setspecific(int k, const void* v) {
    if (k < 0 || k >= 64)
        return 22; // EINVAL

    PthreadTlsRow* row = pthreadTlsRowForCurrent();
    if (!row)
        return 11; // EAGAIN

    row->values[k] = (void*)v;
    compatLogFmt("pthread_setspecific(key=%d, val=%p thread=%p)",
                 k, v, reinterpret_cast<void*>(static_cast<uintptr_t>(threadGetCurHandle())));
    return 0;
}

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

    // SDL_GL_SwapWindow is frequently obtained through handle-scoped dlsym()
    // by CryEngine's renderer. The guest libSDL3 exports its Android
    // implementation, so a handle-scoped lookup would otherwise return that
    // function before the global shim table gets a chance to replace it.
    // Force this single presentation entry point through our Switch/EGL
    // implementation so the Android Java/UI swap path can never block.
    if (strcmp(sym, "SDL_GL_SwapWindow") == 0 ||
        strcmp(sym, "eglSwapBuffers") == 0 ||
        strcmp(sym, "glTexImage2D") == 0 ||
        strcmp(sym, "glPixelStorei") == 0 ||
        strcmp(sym, "glTexSubImage2D") == 0 ||
        strcmp(sym, "glCompressedTexImage2DARB") == 0 ||
        strcmp(sym, "glCompressedTexSubImage2DARB") == 0 ||
        strcmp(sym, "glVertexAttribPointerNV") == 0 ||
        strcmp(sym, "glMapBufferARB") == 0 ||
        strcmp(sym, "glUnmapBufferARB") == 0 ||
        strcmp(sym, "glEnableClientState") == 0 ||
        strcmp(sym, "glDisableClientState") == 0) {
        void* forced = shimResolve(sym);
        compatLogFmt("dlsym: %s -> forced shim %p", sym, forced);
        if (forced)
            return forced;
    }

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

    // XRenderOGL's Android GL loader asks for many legacy entry points with
    // one leading underscore (for example "_glVertex2f" and
    // "_glActiveTextureARB"). Our shim table and Mesa expose the normal
    // "gl..." spelling. Normalize that loader convention instead of returning
    // NULL function pointers that later make the renderer silently skip or
    // crash during its first frame.
    if (sym[0] == '_' && sym[1] == 'g' && sym[2] == 'l' && sym[3] != '\0') {
        const char* normalized = sym + 1;

        p = shimResolve(normalized);
        if (p) {
            static unsigned int normalized_logs = 0;
            if (normalized_logs++ < 32)
                compatLogFmt("dlsym: %s -> normalized shim %s %p",
                             sym, normalized, p);
            return p;
        }

        __eglMustCastToProperFunctionPointerType egl_p =
            eglGetProcAddress(normalized);
        p = reinterpret_cast<void*>(egl_p);
        if (p) {
            static unsigned int egl_gl_logs = 0;
            if (egl_gl_logs++ < 32)
                compatLogFmt("dlsym: %s -> Mesa/EGL %s %p",
                             sym, normalized, p);
            return p;
        }
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



static EGLBoolean w_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    compatPollSwitchInput();

    return (d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE)
        ? eglSwapBuffers(d, s)
        : EGL_FALSE;
}

// XRenderOGL resolves a number of legacy/extension GL entry points at runtime.
// Mesa's eglGetProcAddress() does not necessarily expose every compatibility
// symbol that we already provide through the ELF shim table. Fall back to the
// same shim resolver before returning NULL, otherwise the renderer can cache a
// null function pointer and later jump to PC=0 during pipeline setup/shutdown.
// Forward declarations for legacy texture shims defined later in this file.
static void shim_glPixelStorei(GLenum pname, GLint param);
static void shim_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void* pixels);
static void shim_glTexSubImage2D(GLenum target, GLint level,
                                 GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height,
                                 GLenum format, GLenum type, const void* pixels);
static void shim_glCompressedTexImage2DARB(GLenum target, GLint level,
                                           GLenum internalformat,
                                           GLsizei width, GLsizei height,
                                           GLint border, GLsizei imageSize,
                                           const void* data);
static void shim_glCompressedTexSubImage2DARB(GLenum target, GLint level,
                                              GLint xoffset, GLint yoffset,
                                              GLsizei width, GLsizei height,
                                              GLenum format, GLsizei imageSize,
                                              const void* data);
static void shim_glVertexAttribPointerNV(GLuint index, GLint fsize, GLenum type,
                                         GLsizei stride, const void* pointer);
static void shim_glEnableClientStateCompat(GLenum array);
static void shim_glDisableClientStateCompat(GLenum array);
static void* shim_glMapBufferARB(GLenum target, GLenum access);
static GLboolean shim_glUnmapBufferARB(GLenum target);

static void* w_eglGetProcAddress(const char* name) {
    if (!name || !*name)
        return nullptr;
    // XRenderOGL may resolve these core texture entry points dynamically.
    // Force them through the Switch compatibility shim so legacy Android
    // texture formats (notably NVIDIA DSDT) cannot bypass our translation.
    if (strcmp(name, "glPixelStorei") == 0) {
        compatLog("EGL: eglGetProcAddress(glPixelStorei) -> Switch texture diagnostic shim");
        return reinterpret_cast<void*>(shim_glPixelStorei);
    }
    if (strcmp(name, "glTexImage2D") == 0) {
        compatLog("EGL: eglGetProcAddress(glTexImage2D) -> Switch texture shim");
        return reinterpret_cast<void*>(shim_glTexImage2D);
    }
    if (strcmp(name, "glTexSubImage2D") == 0) {
        compatLog("EGL: eglGetProcAddress(glTexSubImage2D) -> Switch texture shim");
        return reinterpret_cast<void*>(shim_glTexSubImage2D);
    }
    if (strcmp(name, "glCompressedTexImage2DARB") == 0) {
        compatLog("EGL: eglGetProcAddress(glCompressedTexImage2DARB) -> Switch compressed-texture shim");
        return reinterpret_cast<void*>(shim_glCompressedTexImage2DARB);
    }
    if (strcmp(name, "glCompressedTexSubImage2DARB") == 0) {
        compatLog("EGL: eglGetProcAddress(glCompressedTexSubImage2DARB) -> Switch compressed-texture shim");
        return reinterpret_cast<void*>(shim_glCompressedTexSubImage2DARB);
    }
    if (strcmp(name, "glVertexAttribPointerNV") == 0) {
        compatLog("EGL: eglGetProcAddress(glVertexAttribPointerNV) -> NV vertex attrib shim");
        return reinterpret_cast<void*>(shim_glVertexAttribPointerNV);
    }
    if (strcmp(name, "glMapBufferARB") == 0) {
        compatLog("EGL: eglGetProcAddress(glMapBufferARB) -> VBO deformation shim");
        return reinterpret_cast<void*>(shim_glMapBufferARB);
    }
    if (strcmp(name, "glUnmapBufferARB") == 0) {
        compatLog("EGL: eglGetProcAddress(glUnmapBufferARB) -> VBO deformation shim");
        return reinterpret_cast<void*>(shim_glUnmapBufferARB);
    }
    if (strcmp(name, "glEnableClientState") == 0) {
        compatLog("EGL: eglGetProcAddress(glEnableClientState) -> client-state shim");
        return reinterpret_cast<void*>(shim_glEnableClientStateCompat);
    }
    if (strcmp(name, "glDisableClientState") == 0) {
        compatLog("EGL: eglGetProcAddress(glDisableClientState) -> client-state shim");
        return reinterpret_cast<void*>(shim_glDisableClientStateCompat);
    }

    if (strcmp(name, "eglSwapBuffers") == 0) {
        void* shim = shimResolve(name);
        if (shim) {
            compatLogFmt("EGL: eglGetProcAddress(eglSwapBuffers) -> shim %p", shim);
            return shim;
        }
    }

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

// SDL3's Android display structures are needed here without depending on the
// Android build's SDL headers. The layout matches SDL 3.2+ exactly.
struct NearSDLDisplayMode {
    uint32_t displayID;
    uint32_t format;
    int w;
    int h;
    float pixel_density;
    float refresh_rate;
    int refresh_rate_numerator;
    int refresh_rate_denominator;
    void* internal;
};

static uint32_t w_SDL_GetPrimaryDisplay() {
    using Fn = uint32_t (*)();
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GetPrimaryDisplay"));
    const uint32_t id = fn ? fn() : 1u;
    return id ? id : 1u;
}

static const NearSDLDisplayMode* w_SDL_GetCurrentDisplayMode(uint32_t displayID) {
    using Fn = const NearSDLDisplayMode* (*)(uint32_t);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GetCurrentDisplayMode"));
    const NearSDLDisplayMode* real = fn ? fn(displayID) : nullptr;

    static NearSDLDisplayMode mode = {};
    if (real)
        mode = *real;
    else
        std::memset(&mode, 0, sizeof(mode));

    CompatLayer* cl = compatGet();
    if (cl && cl->window.width > 0 && cl->window.height > 0) {
        mode.displayID = displayID;
        mode.w = cl->window.width;
        mode.h = cl->window.height;
        if (mode.pixel_density <= 0.0f)
            mode.pixel_density = 1.0f;
        if (mode.refresh_rate <= 0.0f)
            mode.refresh_rate = 60.0f;
        if (mode.refresh_rate_numerator == 0)
            mode.refresh_rate_numerator = 60;
        if (mode.refresh_rate_denominator == 0)
            mode.refresh_rate_denominator = 1;
    }

    static bool logged = false;
    if (!logged) {
        logged = true;
        compatLogFmt("SDL: display mode forced to %dx%d (display=%u, real=%dx%d)",
                     mode.w, mode.h, displayID,
                     real ? real->w : 0, real ? real->h : 0);
    }
    return &mode;
}

static const NearSDLDisplayMode* w_SDL_GetDesktopDisplayMode(uint32_t displayID) {
    // The Android CryEngine only needs the current mode for renderer startup,
    // but keeping desktop mode consistent prevents later fullscreen queries
    // from reintroducing a 1024x768/phone-sized mode.
    using Fn = const NearSDLDisplayMode* (*)(uint32_t);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GetDesktopDisplayMode"));
    const NearSDLDisplayMode* real = fn ? fn(displayID) : nullptr;

    static NearSDLDisplayMode mode = {};
    if (real)
        mode = *real;

    CompatLayer* cl = compatGet();
    if (cl && cl->window.width > 0 && cl->window.height > 0) {
        mode.displayID = displayID;
        mode.w = cl->window.width;
        mode.h = cl->window.height;
        if (mode.pixel_density <= 0.0f)
            mode.pixel_density = 1.0f;
    }
    return &mode;
}

static bool w_SDL_GetWindowSizeInPixels(void* window, int* w, int* h) {
    using Fn = bool (*)(void*, int*, int*);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_GetWindowSizeInPixels"));
    const bool ok = fn ? fn(window, w, h) : false;

    // The guest Android SDL window can report its pre-surface/default size
    // (commonly 1024x768) even though the Switch window was created for the
    // configured display. Always expose the Switch presentation size so
    // CryEngine's Android viewport code uses the actual render target.
    CompatLayer* cl = compatGet();
    if (cl && w && h && cl->window.width > 0 && cl->window.height > 0) {
        const int old_w = *w;
        const int old_h = *h;
        *w = cl->window.width;
        *h = cl->window.height;
        static bool logged = false;
        if (!logged || old_w != *w || old_h != *h) {
            logged = true;
            compatLogFmt("SDL: SDL_GetWindowSizeInPixels -> Switch size %dx%d (guest=%dx%d)",
                         *w, *h, old_w, old_h);
        }
        return true;
    }

    return ok;
}



static bool w_SDL_GL_SwapWindow(void* window) {
    (void)window;

    // SDL_GL_SwapWindow is our per-frame presentation hook. Poll Switch HID
    // immediately before presenting so button/DPAD/stick transitions reach the
    // guest SDL Android input callbacks without requiring a separate thread.
    compatPollSwitchInput();

    static unsigned int swap_count = 0;
    ++swap_count;

    // Do not call the guest Android SDL_GL_SwapWindow(). Its Android backend can
    // block in the Java/UI presentation path, which is not present on Switch.
    // CryEngine is already rendering into the active EGL surface, so direct
    // eglSwapBuffers is the correct native presentation operation here.
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    const EGLBoolean egl_ok =
        (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE)
            ? w_eglSwapBuffers(display, surface)
            : EGL_FALSE;
    const bool ok = (egl_ok == EGL_TRUE);
    const bool egl_fallback = true;

    // Match the Android renderer's frame lifecycle: after presenting the
    // completed frame, clear the newly available back buffer so stale pixels
    // from the previous frame cannot survive when a later frame draws only a
    // partial/2D region. This is especially important for the CryEngine menu,
    // where mouse interaction can otherwise expose stretched frame remnants.
    if (ok)
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Swap runs once per rendered frame. Keep the regular log quiet;
    // only emit a sparse heartbeat, plus every failure.
    if (swap_count <= 3 || !ok || (swap_count % 600u) == 0u) {
        compatLogFmt("SDL: Swap heartbeat[%u] result=%d window=%p",
                     swap_count, ok ? 1 : 0, window);
    }

    // Keep presentation diagnostics attached to the normal compatibility log.
    // runtime.cpp closes that log at the CXGame::Run main-loop marker; do not
    // close it early based on the number of swaps.
    if (swap_count <= 3 || !ok) {
        GLint viewport[4] = {0, 0, 0, 0};
        GLint framebuffer = 0;
        GLint current_program = 0;
        GLfloat clear_color[4] = {0, 0, 0, 0};

        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
        const GLenum gl_error = glGetError();

        compatLogFmt(
            "SDL: Swap[%u] window=%p guest=0 final=%d egl_fallback=%d "
            "viewport=%d,%d %dx%d fbo=%d program=%d clear=%.3f,%.3f,%.3f,%.3f "
            "glerr=0x%x sdlerr=%s",
            swap_count, window, ok ? 1 : 0,
            egl_fallback ? 1 : 0,
            viewport[0], viewport[1], viewport[2], viewport[3],
            framebuffer, current_program,
            clear_color[0], clear_color[1], clear_color[2], clear_color[3],
            (unsigned)gl_error, sdl3_error());
    }

    return ok;
}

static void* w_SDL_CreateWindow(const char* title, int w, int h, uint32_t flags) {
    using Fn = void* (*)(const char*, int, int, uint32_t);
    Fn fn = reinterpret_cast<Fn>(sdl3_sym("SDL_CreateWindow"));
    if (!fn) {
        compatLog("SDL: SDL_CreateWindow export not found");
        return nullptr;
    }

    CompatLayer* cl = compatGet();
    if (cl && cl->window.width > 0 && cl->window.height > 0) {
        const int old_w = w;
        const int old_h = h;
        // Normalize common desktop defaults to the Switch surface dimensions.
        // Do not replace a deliberately smaller/offscreen UI window.
        if ((w <= 0 || (w == 800 && h == 600) || (w == 1024 && h == 768)) ||
            ((w == cl->window.width) && (h == cl->window.height))) {
            w = cl->window.width;
            h = cl->window.height;
            compatLogFmt("SDL: SDL_CreateWindow size normalized %dx%d -> %dx%d",
                         old_w, old_h, w, h);
        }
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
            r = stub_clock_gettime(cid, ts);
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

    CompatLayer* cl = compatGet();
    if (cl && cl->window.width > 0 && cl->window.height > 0 &&
        x == 0 && y == 0 &&
        ((w == 800 && h == 600) || (w == 1024 && h == 768))) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            compatLogFmt("GL: full viewport normalized %dx%d -> %dx%d",
                         w, h, cl->window.width, cl->window.height);
        }
        w = cl->window.width;
        h = cl->window.height;
    }

    glViewport(x + p.content_x, y + p.content_y, w, h);
}

static void w_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    const Presentation& p = orientGet();

    CompatLayer* cl = compatGet();
    if (cl && cl->window.width > 0 && cl->window.height > 0 &&
        x == 0 && y == 0 &&
        ((w == 800 && h == 600) || (w == 1024 && h == 768))) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            compatLogFmt("GL: full scissor normalized %dx%d -> %dx%d",
                         w, h, cl->window.width, cl->window.height);
        }
        w = cl->window.width;
        h = cl->window.height;
    }

    glScissor(x + p.content_x, y + p.content_y, w, h);
}

static bool nearLooksLikeClientPointer(const void* pointer);
static void nearLogWaterDrawState();
static bool nearPrepareTextureShaderEmulation();
static void nearFinishTextureShaderEmulation();

static void w_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    nearLogWaterDrawState();
    const bool emu = nearPrepareTextureShaderEmulation();
    glDrawArrays(mode, first, count);
    if (emu) nearFinishTextureShaderEmulation();
}

struct NearVertexPointerState {
    GLint size = 0;
    GLenum type = 0;
    GLsizei stride = 0;
    uintptr_t pointer = 0;
    GLint buffer = 0;
    bool clientPointer = false;
};

static NearVertexPointerState g_nearVertexPointerState;

// Track the legacy ARB program bindings ourselves. Some Mesa/Zink paths accept
// GL_VERTEX_PROGRAM_ARB/GL_FRAGMENT_PROGRAM_ARB in glIsEnabled() but reject the
// corresponding binding query enums with GL_INVALID_ENUM.
static thread_local GLuint g_nearArbVertexProgramBinding = 0;
static thread_local GLuint g_nearArbFragmentProgramBinding = 0;

static unsigned g_nearDrawElementsDiagCalls = 0;
static unsigned g_nearDrawContentDiagCalls = 0;

static void nearDiagDrawBufferContents(GLint arrayBuffer, GLint elementBuffer,
                                       GLint vertexBufferSize, GLint indexBufferSize,
                                       GLenum type, GLsizei count, const void* indices) {
    if (g_nearDrawContentDiagCalls >= 8 || arrayBuffer <= 0 || elementBuffer <= 0 ||
        vertexBufferSize <= 0 || indexBufferSize <= 0)
        return;

    const uintptr_t indexOffset = reinterpret_cast<uintptr_t>(indices);
    const GLint stride = g_nearVertexPointerState.stride;
    const uintptr_t vertexOffset = g_nearVertexPointerState.pointer;

    // This diagnostic is intentionally read-only: map the exact GPU buffers
    // immediately before the draw, inspect them, then unmap without changing
    // any vertex/index data.
    if (g_nearVertexPointerState.buffer != arrayBuffer ||
        g_nearVertexPointerState.clientPointer ||
        stride <= 0 || g_nearVertexPointerState.type != GL_FLOAT) {
        compatLogFmt(
            "GL DRAW CONTENT SKIP[%u]: vbo=%d ptrBuf=%d client=%d type=0x%x stride=%d ptrOff=0x%llx",
            g_nearDrawContentDiagCalls + 1, (int)arrayBuffer,
            (int)g_nearVertexPointerState.buffer,
            g_nearVertexPointerState.clientPointer ? 1 : 0,
            (unsigned)g_nearVertexPointerState.type, (int)stride,
            (unsigned long long)vertexOffset);
        ++g_nearDrawContentDiagCalls;
        return;
    }

    if (indexOffset >= (uintptr_t)indexBufferSize) {
        compatLogFmt(
            "GL DRAW CONTENT BAD INDEX OFFSET: off=0x%llx iboSize=%d count=%d type=0x%x",
            (unsigned long long)indexOffset, (int)indexBufferSize,
            (int)count, (unsigned)type);
        ++g_nearDrawContentDiagCalls;
        return;
    }

    const GLsizeiptr vSize = (GLsizeiptr)vertexBufferSize;
    const GLsizeiptr iSize = (GLsizeiptr)indexBufferSize;

    void* vb = glMapBufferRange(GL_ARRAY_BUFFER, 0, vSize, GL_MAP_READ_BIT);
    if (!vb) {
        compatLogFmt("GL DRAW CONTENT VBO MAP FAIL: vbo=%d size=%d err=0x%x",
                     (int)arrayBuffer, (int)vertexBufferSize, (unsigned)glGetError());
        ++g_nearDrawContentDiagCalls;
        return;
    }

    void* ib = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, iSize, GL_MAP_READ_BIT);
    if (!ib) {
        compatLogFmt("GL DRAW CONTENT IBO MAP FAIL: ibo=%d size=%d err=0x%x",
                     (int)elementBuffer, (int)indexBufferSize, (unsigned)glGetError());
        glUnmapBuffer(GL_ARRAY_BUFFER);
        ++g_nearDrawContentDiagCalls;
        return;
    }

    const uint8_t* vbytes = reinterpret_cast<const uint8_t*>(vb);
    const uint8_t* ibytes = reinterpret_cast<const uint8_t*>(ib);

    size_t vertexCount = 0;
    if (vertexOffset < (uintptr_t)vertexBufferSize)
        vertexCount = ((size_t)vertexBufferSize - (size_t)vertexOffset) / (size_t)stride;

    size_t indexCount = 0;
    size_t indexElemSize = 0;
    if (type == GL_UNSIGNED_BYTE) indexElemSize = 1;
    else if (type == GL_UNSIGNED_SHORT) indexElemSize = 2;
    else if (type == GL_UNSIGNED_INT) indexElemSize = 4;

    if (indexElemSize && indexOffset < (uintptr_t)indexBufferSize)
        indexCount = std::min<size_t>(
            (size_t)count,
            ((size_t)indexBufferSize - (size_t)indexOffset) / indexElemSize);

    uint32_t badIndexCount = 0;
    uint32_t nanInfCount = 0;
    uint32_t hugeCoordCount = 0;
    uint32_t referencedCount = 0;
    uint32_t minIndex = UINT32_MAX;
    uint32_t maxIndex = 0;

    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    bool haveCoord = false;

    auto readIndex = [&](size_t n) -> uint32_t {
        const uint8_t* p = ibytes + (size_t)indexOffset + n * indexElemSize;
        if (indexElemSize == 1) return p[0];
        if (indexElemSize == 2) {
            uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        uint32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };

    for (size_t i = 0; i < indexCount; ++i) {
        const uint32_t idx = readIndex(i);
        minIndex = std::min(minIndex, idx);
        maxIndex = std::max(maxIndex, idx);

        if ((size_t)idx >= vertexCount) {
            ++badIndexCount;
            continue;
        }

        const uint8_t* vp = vbytes + (size_t)vertexOffset + (size_t)idx * (size_t)stride;
        float xyz[3] = {};
        std::memcpy(&xyz[0], vp + 0, sizeof(float));
        std::memcpy(&xyz[1], vp + 4, sizeof(float));
        std::memcpy(&xyz[2], vp + 8, sizeof(float));

        ++referencedCount;
        if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
            ++nanInfCount;
        } else {
            if (!haveCoord) {
                minX = maxX = xyz[0];
                minY = maxY = xyz[1];
                minZ = maxZ = xyz[2];
                haveCoord = true;
            } else {
                minX = std::min(minX, xyz[0]); maxX = std::max(maxX, xyz[0]);
                minY = std::min(minY, xyz[1]); maxY = std::max(maxY, xyz[1]);
                minZ = std::min(minZ, xyz[2]); maxZ = std::max(maxZ, xyz[2]);
            }
            if (std::fabs(xyz[0]) > 100000.0f ||
                std::fabs(xyz[1]) > 100000.0f ||
                std::fabs(xyz[2]) > 100000.0f)
                ++hugeCoordCount;
        }
    }

    // Clear any pending GL error first so this diagnostic does not confuse an
    // earlier renderer error with one generated by the state queries below.
    const GLenum vpPreError = glGetError();

    const GLint vertexProgram =
        (GLint)g_nearArbVertexProgramBinding;
    const GLint fragmentProgram =
        (GLint)g_nearArbFragmentProgramBinding;
    const GLboolean vertexProgramEnabled = glIsEnabled(0x8620);   // GL_VERTEX_PROGRAM_ARB
    const GLenum vpIsEnabledError = glGetError();
    const GLboolean fragmentProgramEnabled = glIsEnabled(0x8804); // GL_FRAGMENT_PROGRAM_ARB
    const GLenum fpIsEnabledError = glGetError();
    const GLenum vpBindingError = GL_NO_ERROR;
    const GLenum fpBindingError = GL_NO_ERROR;

    compatLogFmt(
        "GL DRAW VP STATE[%u]: vertex_enabled=%d vertex_program=%d "
        "fragment_enabled=%d fragment_program=%d pre_error=0x%x "
        "err_isEnabled_v=0x%x err_isEnabled_f=0x%x "
        "err_binding_v=0x%x err_binding_f=0x%x",
        g_nearDrawContentDiagCalls + 1,
        vertexProgramEnabled ? 1 : 0, (int)vertexProgram,
        fragmentProgramEnabled ? 1 : 0, (int)fragmentProgram,
        (unsigned)vpPreError,
        (unsigned)vpIsEnabledError, (unsigned)fpIsEnabledError,
        (unsigned)vpBindingError, (unsigned)fpBindingError);

    // ARB vertex-program mode uses generic attributes as vertex-program inputs.
    // Capture them directly; a correct glVertexPointer() alone is not enough.
    if (vertexProgramEnabled) {
        for (GLuint attrib = 0; attrib < 16; ++attrib) {
            GLint enabled = 0;
            GLint size = 0;
            GLint attribType = 0;
            GLint attribStride = 0;
            GLint normalized = 0;
            GLint attribBuffer = 0;
            void* attribPointer = nullptr;

            glGetVertexAttribiv(attrib, 0x8622 /* GL_VERTEX_ATTRIB_ARRAY_ENABLED */, &enabled);
            glGetVertexAttribiv(attrib, 0x8623 /* GL_VERTEX_ATTRIB_ARRAY_SIZE */, &size);
            glGetVertexAttribiv(attrib, 0x8625 /* GL_VERTEX_ATTRIB_ARRAY_TYPE */, &attribType);
            glGetVertexAttribiv(attrib, 0x8624 /* GL_VERTEX_ATTRIB_ARRAY_STRIDE */, &attribStride);
            glGetVertexAttribiv(attrib, 0x886A /* GL_VERTEX_ATTRIB_ARRAY_NORMALIZED */, &normalized);
            glGetVertexAttribiv(attrib, 0x889F /* GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING */, &attribBuffer);
            glGetVertexAttribPointerv(attrib, 0x8645 /* GL_VERTEX_ATTRIB_ARRAY_POINTER */,
                                      &attribPointer);

            const GLenum attribError = glGetError();
            if (attribError != GL_NO_ERROR || enabled || attrib == 0 || attrib == 8) {
                compatLogFmt(
                    "GL DRAW ATTRIB[%u]: a=%u enabled=%d size=%d type=0x%x "
                    "norm=%d stride=%d buffer=%d ptr=0x%llx err=0x%x",
                    g_nearDrawContentDiagCalls + 1,
                    (unsigned)attrib, (int)enabled, (int)size,
                    (unsigned)attribType, (int)normalized, (int)attribStride,
                    (int)attribBuffer,
                    (unsigned long long)reinterpret_cast<uintptr_t>(attribPointer),
                    (unsigned)attribError);
            }
        }
    }

    if (vertexProgramEnabled && vertexProgram > 0) {
        // Inspect the actual bound ARB vertex program source without changing it.
        using NearGetProgramivARB = void (*)(GLenum, GLenum, GLint*);
        using NearGetProgramStringARB = void (*)(GLenum, GLenum, void*);
        static NearGetProgramivARB getProgramiv =
            reinterpret_cast<NearGetProgramivARB>(
                eglGetProcAddress("glGetProgramivARB"));
        static NearGetProgramStringARB getProgramString =
            reinterpret_cast<NearGetProgramStringARB>(
                eglGetProcAddress("glGetProgramStringARB"));

        if (getProgramiv && getProgramString) {
            GLint programLength = 0;
            getProgramiv(0x8620 /* GL_VERTEX_PROGRAM_ARB */,
                         0x8627 /* GL_PROGRAM_LENGTH_ARB */, &programLength);

            if (programLength > 0 && programLength <= 1024 * 1024) {
                std::string programSource((size_t)programLength, '\0');
                getProgramString(0x8620 /* GL_VERTEX_PROGRAM_ARB */,
                                 0x8628 /* GL_PROGRAM_STRING_ARB */,
                                 &programSource[0]);

                auto has = [&](const char* token) -> int {
                    return programSource.find(token) != std::string::npos ? 1 : 0;
                };

                const int usesPosition =
                    has("vertex.position") || has("vertex.attrib[0]") || has("$vin.ATTR0");
                const int usesNormal =
                    has("vertex.normal") || has("vertex.attrib[2]") || has("$vin.ATTR2");
                const int usesTexcoord =
                    has("vertex.texcoord") || has("vertex.attrib[8]") || has("$vin.ATTR8");
                const int usesLocalParams = has("program.local");
                const int usesEnvParams = has("program.env");
                const int usesTexcoord0 =
                    has("vertex.texcoord") || has("vertex.texcoord[0]") ||
                    has("$vin.ATTR8") || has("vertex.attrib[8]");

                compatLogFmt(
                    "GL DRAW VP SOURCE[%u]: program=%d len=%d "
                    "position=%d normal=%d texcoord=%d texcoord0=%d "
                    "attrib0=%d attrib2=%d attrib8=%d local=%d env=%d "
                    "mvp=%d result_pos=%d",
                    g_nearDrawContentDiagCalls + 1, (int)vertexProgram,
                    (int)programLength,
                    usesPosition,
                    usesNormal,
                    usesTexcoord,
                    usesTexcoord0,
                    has("vertex.attrib[0]") || has("$vin.ATTR0"),
                    has("vertex.attrib[2]") || has("$vin.ATTR2"),
                    has("vertex.attrib[8]") || has("$vin.ATTR8"),
                    usesLocalParams,
                    usesEnvParams,
                    has("state.matrix.mvp") || has("ModelViewProj"),
                    has("result.position"));

                std::string compactSource = programSource;
                for (char& ch : compactSource) {
                    if (ch == '\n' || ch == '\r')
                        ch = ' ';
                }
                compatLogFmt(
                    "GL DRAW VP TEXT[%u]: program=%d %s",
                    g_nearDrawContentDiagCalls + 1, (int)vertexProgram,
                    compactSource.c_str());

                // Also emit only executable ARBvp instructions. The full
                // compiler text above is useful but can be too long for a
                // single console/log record; the instruction-only view makes
                // position/data dependencies immediately visible.
                {
                    size_t bodyStart = programSource.find("#program main");
                    if (bodyStart != std::string::npos)
                        bodyStart = programSource.find('\n', bodyStart);

                    size_t bodyEnd = std::string::npos;
                    if (bodyStart != std::string::npos)
                        bodyEnd = programSource.find("\n#end", bodyStart);

                    if (bodyStart != std::string::npos) {
                        if (bodyEnd == std::string::npos)
                            bodyEnd = programSource.size();

                        std::string instructions = programSource.substr(
                            bodyStart + 1, bodyEnd - (bodyStart + 1));
                        size_t cursor = 0;
                        unsigned instructionNo = 0;
                        while (cursor < instructions.size() && instructionNo < 128) {
                            size_t next = instructions.find('\n', cursor);
                            if (next == std::string::npos)
                                next = instructions.size();

                            std::string line = instructions.substr(cursor, next - cursor);
                            while (!line.empty() &&
                                   (line.front() == ' ' || line.front() == '\t'))
                                line.erase(line.begin());

                            if (!line.empty() && line[0] != '#') {
                                compatLogFmt(
                                    "GL DRAW VP INST[%u]: program=%d n=%u %s",
                                    g_nearDrawContentDiagCalls + 1,
                                    (int)vertexProgram,
                                    instructionNo,
                                    line.c_str());
                                ++instructionNo;
                            }
                            cursor = next + (next < instructions.size() ? 1 : 0);
                        }

                        compatLogFmt(
                            "GL DRAW VP INST END[%u]: program=%d count=%u",
                            g_nearDrawContentDiagCalls + 1,
                            (int)vertexProgram, instructionNo);
                    }
                }

                // vertex.texcoord (without [n]) and vertex.texcoord[0] both
                // consume the conventional texture-coordinate set 0. Capture
                // that exact client-array state at the same draw.
                {
                    GLint savedClientActive = (GLint)GL_TEXTURE0;
                    glGetIntegerv(0x84E1 /* GL_CLIENT_ACTIVE_TEXTURE */, &savedClientActive);
                    glClientActiveTexture(GL_TEXTURE0);

                    GLint tcEnabled = glIsEnabled(GL_TEXTURE_COORD_ARRAY) ? 1 : 0;
                    const GLenum tcEnableError = glGetError();
                    GLint tcSize = 0, tcType = 0, tcStride = 0, tcBuffer = 0;
                    void* tcPointer = nullptr;
                    glGetIntegerv(0x8088 /* GL_TEXTURE_COORD_ARRAY_SIZE */, &tcSize);
                    glGetIntegerv(0x8089 /* GL_TEXTURE_COORD_ARRAY_TYPE */, &tcType);
                    glGetIntegerv(0x808A /* GL_TEXTURE_COORD_ARRAY_STRIDE */, &tcStride);
                    glGetPointerv(0x8092 /* GL_TEXTURE_COORD_ARRAY_POINTER */, &tcPointer);
                    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &tcBuffer);
                    const GLenum tcStateError = glGetError();

                    compatLogFmt(
                        "GL DRAW TCARRAY[%u]: unit=0 enabled=%d size=%d type=0x%x "
                        "stride=%d buffer=%d ptr=0x%llx enable_err=0x%x state_err=0x%x",
                        g_nearDrawContentDiagCalls + 1,
                        tcEnabled, tcSize, (unsigned)tcType, tcStride, tcBuffer,
                        (unsigned long long)reinterpret_cast<uintptr_t>(tcPointer),
                        (unsigned)tcEnableError, (unsigned)tcStateError);

                    glClientActiveTexture((GLenum)savedClientActive);
                }

                // Local program parameters are persistent per program. Dump a
                // bounded prefix only when the shader actually references them.
                if (usesLocalParams) {
                    using NearGetProgramLocalParameterfvARB =
                        void (*)(GLenum, GLuint, GLfloat*);
                    static NearGetProgramLocalParameterfvARB getLocal =
                        reinterpret_cast<NearGetProgramLocalParameterfvARB>(
                            eglGetProcAddress("glGetProgramLocalParameterfvARB"));
                    using NearGetProgramivARBLocal = void (*)(GLenum, GLenum, GLint*);
                    static NearGetProgramivARBLocal getProgramivLocal =
                        reinterpret_cast<NearGetProgramivARBLocal>(
                            eglGetProcAddress("glGetProgramivARB"));

                    GLint parameterCount = -1;
                    if (getProgramivLocal)
                        getProgramivLocal(0x8620 /* GL_VERTEX_PROGRAM_ARB */,
                                          0x88A8 /* GL_PROGRAM_PARAMETERS_ARB */,
                                          &parameterCount);

                    compatLogFmt(
                        "GL DRAW VP LOCAL[%u]: program=%d parameters=%d get=%d",
                        g_nearDrawContentDiagCalls + 1, (int)vertexProgram,
                        parameterCount, getLocal ? 1 : 0);

                    if (getLocal) {
                        const GLint lim = std::max(0, std::min(parameterCount, 32));
                        for (GLint localIndex = 0; localIndex < lim; ++localIndex) {
                            GLfloat local[4] = {};
                            getLocal(0x8620 /* GL_VERTEX_PROGRAM_ARB */,
                                     (GLuint)localIndex, local);
                            compatLogFmt(
                                "GL DRAW VP LOCAL[%u]: program=%d local=%d %g,%g,%g,%g",
                                g_nearDrawContentDiagCalls + 1, (int)vertexProgram,
                                (int)localIndex,
                                (double)local[0], (double)local[1],
                                (double)local[2], (double)local[3]);
                        }
                    }
                }
            }
        } else {
            compatLog("GL DRAW VP SOURCE: ARB program query unavailable");
        }

        // This diagnostic function is above the shared ARB typedef block below.
        using NearGetProgramEnvParameterfvARB = void (*)(GLenum, GLuint, GLfloat*);
        static NearGetProgramEnvParameterfvARB getEnv =
            reinterpret_cast<NearGetProgramEnvParameterfvARB>(
                eglGetProcAddress("glGetProgramEnvParameterfvARB"));
        if (getEnv) {
            GLfloat env[4] = {};
            for (GLuint envIndex = 0; envIndex < 8; ++envIndex) {
                getEnv(0x8620 /* GL_VERTEX_PROGRAM_ARB */, envIndex, env);
                compatLogFmt(
                    "GL DRAW VP ENV[%u]: program=%d env=%u %g,%g,%g,%g",
                    g_nearDrawContentDiagCalls + 1, (int)vertexProgram,
                    (unsigned)envIndex,
                    (double)env[0], (double)env[1], (double)env[2], (double)env[3]);
            }
        } else {
            compatLog("GL DRAW VP ENV: glGetProgramEnvParameterfvARB unavailable");
        }
    }

    const GLenum drawGlError = glGetError();
    compatLogFmt("GL DRAW VP STATE[%u]: gl_error_before_draw=0x%x",
                 g_nearDrawContentDiagCalls + 1, (unsigned)drawGlError);

    compatLogFmt(
        "GL DRAW CONTENT[%u]: vbo=%d verts=%zu stride=%d ptrOff=0x%llx "
        "ibo=%d indices=%zu/%d indexOff=0x%llx min=%u max=%u bad=%u "
        "nanInf=%u huge=%u XYZ=%g,%g,%g..%g,%g,%g",
        g_nearDrawContentDiagCalls + 1,
        (int)arrayBuffer, vertexCount, (int)stride,
        (unsigned long long)vertexOffset, (int)elementBuffer,
        indexCount, (int)count, (unsigned long long)indexOffset,
        minIndex == UINT32_MAX ? 0u : minIndex, maxIndex, badIndexCount,
        nanInfCount, hugeCoordCount,
        haveCoord ? (double)minX : 0.0, haveCoord ? (double)minY : 0.0,
        haveCoord ? (double)minZ : 0.0, haveCoord ? (double)maxX : 0.0,
        haveCoord ? (double)maxY : 0.0, haveCoord ? (double)maxZ : 0.0);

    const size_t sampleCount = std::min<size_t>(indexCount, 12);
    for (size_t i = 0; i < sampleCount; ++i) {
        const uint32_t idx = readIndex(i);
        if ((size_t)idx >= vertexCount) {
            compatLogFmt("GL DRAW CONTENT INDEX[%u]: i=%u idx=%u BAD",
                         g_nearDrawContentDiagCalls + 1, (unsigned)i, (unsigned)idx);
            continue;
        }
        const uint8_t* vp = vbytes + (size_t)vertexOffset + (size_t)idx * (size_t)stride;
        float xyz[3] = {};
        std::memcpy(&xyz[0], vp + 0, sizeof(float));
        std::memcpy(&xyz[1], vp + 4, sizeof(float));
        std::memcpy(&xyz[2], vp + 8, sizeof(float));
        compatLogFmt(
            "GL DRAW CONTENT INDEX[%u]: i=%u idx=%u XYZ=%g,%g,%g",
            g_nearDrawContentDiagCalls + 1, (unsigned)i, (unsigned)idx,
            (double)xyz[0], (double)xyz[1], (double)xyz[2]);
    }

    glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    glUnmapBuffer(GL_ARRAY_BUFFER);

    ++g_nearDrawContentDiagCalls;
}

static void w_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_nearDrawElementsDiagCalls < 24) {
        GLint arrayBuffer = 0;
        GLint elementBuffer = 0;
        GLint vertexBufferSize = 0;
        GLint indexBufferSize = 0;
        glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &arrayBuffer);
        glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &elementBuffer);

        if (arrayBuffer != 0)
            glGetBufferParameteriv(GL_ARRAY_BUFFER, 0x8764 /* GL_BUFFER_SIZE */, &vertexBufferSize);
        if (elementBuffer != 0)
            glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, 0x8764 /* GL_BUFFER_SIZE */, &indexBufferSize);

        compatLogFmt(
            "GL DRAW ELEMENTS[%u]: mode=0x%x count=%d type=0x%x indexOff=0x%llx "
            "vbo=%d vboSize=%d ibo=%d iboSize=%d vertexArray=%d",
            g_nearDrawElementsDiagCalls + 1,
            (unsigned)mode, (int)count, (unsigned)type,
            (unsigned long long)(uintptr_t)indices,
            (int)arrayBuffer, (int)vertexBufferSize,
            (int)elementBuffer, (int)indexBufferSize,
            glIsEnabled(GL_VERTEX_ARRAY) ? 1 : 0);

        nearDiagDrawBufferContents(arrayBuffer, elementBuffer,
                                   vertexBufferSize, indexBufferSize,
                                   type, count, indices);
    }
    ++g_nearDrawElementsDiagCalls;

    nearLogWaterDrawState();

    // DrawElements normally interprets indices as an offset when an element
    // VBO is bound. Legacy client-array paths can still pass a real CPU pointer,
    // especially special NV_vertex_program paths. Detect that case and record
    // the draw with ELEMENT_ARRAY_BUFFER=0, then restore the previous binding.
    GLint savedElementBuffer = 0;
    glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &savedElementBuffer);
    const bool clientIndices =
        savedElementBuffer != 0 && nearLooksLikeClientPointer(indices);
    if (clientIndices)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    const GLenum errAfterWaterDiag = glGetError();
    const bool emu = nearPrepareTextureShaderEmulation();
    const GLenum errAfterPrepare = glGetError();

    glDrawElements(mode, count, type, indices);
    const GLenum drawError = glGetError();
    if (g_nearDrawElementsDiagCalls <= 24) {
        compatLogFmt(
            "GL DRAW RESULT[%u]: err=0x%x pre_prepare=0x%x "
            "prepare=0x%x vertex_prog=%u fragment_prog=%u",
            g_nearDrawElementsDiagCalls,
            (unsigned)drawError,
            (unsigned)errAfterWaterDiag,
            (unsigned)errAfterPrepare,
            (unsigned)g_nearArbVertexProgramBinding,
            (unsigned)g_nearArbFragmentProgramBinding);
    }
    if (emu) nearFinishTextureShaderEmulation();

    if (clientIndices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)savedElementBuffer);
        compatLogFmt(
            "GL DRAW ELEMENTS CLIENT FIX: mode=0x%x count=%d type=0x%x ptr=%p savedIbo=%d",
            (unsigned)mode, (int)count, (unsigned)type,
            indices, (int)savedElementBuffer);
    }
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


// ─── GLES/OES compatibility aliases ─────────────────────────────────────────
// libSDL3.so still imports a small set of Android GLES/OES entry points that
// Mesa's desktop GL context does not expose under their OES names. The
// corresponding operations are identical to the core desktop entry points, so
// provide direct ABI-compatible aliases instead of leaving the guest relocations
// unresolved.
extern "C" {
static void shim_glBlendEquationOES(GLenum mode) {
    glBlendEquation(mode);
}

static void shim_glBlendEquationSeparateOES(GLenum rgb, GLenum alpha) {
    glBlendEquationSeparate(rgb, alpha);
}

static void shim_glBlendFuncSeparateOES(GLenum srcRGB, GLenum dstRGB,
                                        GLenum srcAlpha, GLenum dstAlpha) {
    glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
}

static void shim_glOrthof(GLfloat left, GLfloat right, GLfloat bottom,
                          GLfloat top, GLfloat zNear, GLfloat zFar) {
    glOrtho((GLdouble)left, (GLdouble)right,
            (GLdouble)bottom, (GLdouble)top,
            (GLdouble)zNear, (GLdouble)zFar);
}

static void shim_glGenFramebuffersOES(GLsizei n, GLuint* framebuffers) {
    glGenFramebuffers(n, framebuffers);
}

static void shim_glBindFramebufferOES(GLenum target, GLuint framebuffer) {
    glBindFramebuffer(target, framebuffer);
}

static void shim_glFramebufferTexture2DOES(GLenum target, GLenum attachment,
                                           GLenum textarget, GLuint texture,
                                           GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

static GLenum shim_glCheckFramebufferStatusOES(GLenum target) {
    return glCheckFramebufferStatus(target);
}

static void shim_glDeleteFramebuffersOES(GLsizei n, const GLuint* framebuffers) {
    glDeleteFramebuffers(n, framebuffers);
}

// OES_draw_texture is used by Android/SDL helper code for screen-space quads.
// Recreate its conventional rectangle operation with the current fixed-function
// texture state in the desktop compatibility profile.
static void shim_glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z,
                               GLfloat width, GLfloat height) {
    const GLfloat x2 = x + width;
    const GLfloat y2 = y + height;
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(x,  y,  z);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(x2, y,  z);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(x2, y2, z);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(x,  y2, z);
    glEnd();
}
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

// The Android Cry3DEngine implementation uses strcasecmp() to locate
// "LevelLM.pak" in the level directory. Keep this comparison on the same
// ASCII/case-folding rules as the Switch filesystem helper. This is
// intentionally narrow: every other strcasecmp() call keeps normal libc
// semantics.
static int stub_strcasecmp(const char* a, const char* b) {
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);

    const std::string la = asciiLower(a);
    const std::string lb = asciiLower(b);

    if (la == "levellm.pak" || lb == "levellm.pak")
        return la.compare(lb);

    if (la == lb)
        return 0;
    return la < lb ? -1 : 1;
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




static void countLuaScriptsRecursive(const std::string& directory,
                                     int& luaCount,
                                     int depth = 0) {
    if (depth > 32)
        return;

    DIR* d = opendir(directory.c_str());
    if (!d)
        return;

    while (struct dirent* ent = readdir(d)) {
        const char* name = ent->d_name;
        if (!name || !*name || !std::strcmp(name, ".") || !std::strcmp(name, ".."))
            continue;

        std::string full = directory;
        if (!full.empty() && full.back() != '/')
            full += '/';
        full += name;

        struct stat st = {};
        if (stat(full.c_str(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            countLuaScriptsRecursive(full, luaCount, depth + 1);
            continue;
        }

        const size_t len = std::strlen(name);
        if (len >= 4 &&
            std::tolower((unsigned char)name[len - 4]) == '.' &&
            std::tolower((unsigned char)name[len - 3]) == 'l' &&
            std::tolower((unsigned char)name[len - 2]) == 'u' &&
            std::tolower((unsigned char)name[len - 1]) == 'a') {
            ++luaCount;
        }
    }

    closedir(d);
}


void compatPrepareScriptDirectories(const char* dataRoot) {
    (void)dataRoot;
}
void compatProbePakArchives(const char* dataRoot) {
    const std::string root = dataRoot ? dataRoot : "";
    std::string fcdata = root;
    if (!fcdata.empty())
        fcdata += "/FCData";

    (void)root;
    (void)fcdata;
}


void compatPrepareShaderDirectories(const char* dataRoot) {
    (void)dataRoot;
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

// Android/Bionic AArch64 struct dirent layout:
// d_ino=0, d_off=8, d_reclen=16, d_type=18, d_name=19.
// The Switch/newlib struct dirent has a different layout, so an Android
// library reading d_name from the native object sees padding/empty bytes.
struct AndroidDirentCompat {
    uint64_t d_ino = 0;
    int64_t d_off = 0;
    uint16_t d_reclen = 0;
    uint8_t d_type = 0;
    char d_name[256] = {};
};

static_assert(offsetof(AndroidDirentCompat, d_name) == 19,
              "Android dirent d_name offset must be 19");
static_assert(sizeof(AndroidDirentCompat) == 280,
              "Android dirent size must be 280");

struct VirtualPakDir {
    std::string directory;
    std::vector<std::string> names;
    size_t pos = 0;
    AndroidDirentCompat current = {};
};

static Mutex g_vpak_dir_lock;
static std::unordered_set<DIR*> g_vpak_dirs;

static bool vpakDirOwns(DIR* dir) {
    if (!dir)
        return false;
    mutexLock(&g_vpak_dir_lock);
    const bool found = g_vpak_dirs.find(dir) != g_vpak_dirs.end();
    mutexUnlock(&g_vpak_dir_lock);
    return found;
}

static bool collectPakDirectoryEntries(const char* directory,
                                       std::vector<std::string>& outNames) {
    outNames.clear();
    if (!directory)
        return false;

    std::string wanted = pakAssetRelativeName(directory);
    while (!wanted.empty() && wanted.back() == '/')
        wanted.pop_back();

    const std::string prefix =
        wanted.empty() ? std::string() : wanted + "/";

    std::unordered_map<std::string, bool> seen;

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
        DIR* dir = ::opendir(roots[r]);
        if (!dir)
            continue;

        while (dirent* ent = ::readdir(dir)) {
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

            std::string pakPath = roots[r];
            if (pakPath != ".")
                pakPath += "/";
            pakPath += name;

            mutexLock(&g_pak_index_lock);
            auto it = g_pak_indexes.find(pakPath);
            if (it == g_pak_indexes.end()) {
                PakIndex fresh;
                const bool ok = buildPakIndexLocked(pakPath, fresh);
                auto inserted = g_pak_indexes.emplace(pakPath, std::move(fresh));
                it = inserted.first;
                if (!ok) {
                    mutexUnlock(&g_pak_index_lock);
                    continue;
                }
            }

            for (const auto& item : it->second.entries) {
                const std::string& entry = item.first;
                if (!prefix.empty()) {
                    if (entry.rfind(prefix, 0) != 0)
                        continue;
                }

                std::string rest = prefix.empty()
                    ? entry
                    : entry.substr(prefix.size());
                if (rest.empty())
                    continue;

                const size_t slash = rest.find('/');
                const std::string child =
                    slash == std::string::npos ? rest : rest.substr(0, slash);
                if (!child.empty())
                    seen.emplace(child, slash != std::string::npos);
            }

            mutexUnlock(&g_pak_index_lock);
        }

        ::closedir(dir);
    }

    outNames.reserve(seen.size());
    for (const auto& item : seen)
        outNames.push_back(item.first);
    std::sort(outNames.begin(), outNames.end());
    return !outNames.empty();
}

static bool pakVirtualDirectoryExists(const char* directory) {
    std::vector<std::string> names;
    return collectPakDirectoryEntries(directory, names);
}

static DIR* vpakDirOpen(const char* directory) {
    std::vector<std::string> names;
    if (!collectPakDirectoryEntries(directory, names))
        return nullptr;

    // Android CryPak enumerates the union of loose filesystem entries and
    // entries contributed by PAKs. Preserve any real children as well when a
    // physical directory exists as an empty/mount-point directory.
    std::unordered_set<std::string> seen(names.begin(), names.end());
    if (directory && *directory) {
        if (DIR* physical = ::opendir(directory)) {
            while (dirent* ent = ::readdir(physical)) {
                if (!ent->d_name[0] ||
                    std::strcmp(ent->d_name, ".") == 0 ||
                    std::strcmp(ent->d_name, "..") == 0)
                    continue;
                if (seen.insert(ent->d_name).second)
                    names.emplace_back(ent->d_name);
            }
            ::closedir(physical);
        }
    }

    std::sort(names.begin(), names.end());

    VirtualPakDir* state = new VirtualPakDir();
    if (!state)
        return nullptr;
    state->directory = directory ? directory : "";
    state->names = std::move(names);

    DIR* handle = reinterpret_cast<DIR*>(state);
    mutexLock(&g_vpak_dir_lock);
    g_vpak_dirs.insert(handle);
    mutexUnlock(&g_vpak_dir_lock);
    return handle;
}

static struct dirent* vpakDirRead(DIR* dir) {
    mutexLock(&g_vpak_dir_lock);
    if (g_vpak_dirs.find(dir) == g_vpak_dirs.end()) {
        mutexUnlock(&g_vpak_dir_lock);
        return nullptr;
    }

    VirtualPakDir* state = reinterpret_cast<VirtualPakDir*>(dir);
    if (state->pos >= state->names.size()) {
        mutexUnlock(&g_vpak_dir_lock);
        return nullptr;
    }

    state->current = AndroidDirentCompat{};
    const std::string& name = state->names[state->pos++];
    const size_t maxName = sizeof(state->current.d_name) - 1;
    const size_t copy = name.size() < maxName ? name.size() : maxName;
    std::memcpy(state->current.d_name, name.c_str(), copy);
    state->current.d_name[copy] = '\0';
    state->current.d_type = DT_REG;
    state->current.d_reclen =
        (uint16_t)((offsetof(AndroidDirentCompat, d_name) + copy + 1 + 7) & ~7u);
    mutexUnlock(&g_vpak_dir_lock);
    return reinterpret_cast<struct dirent*>(&state->current);
}

static int vpakDirClose(DIR* dir) {
    mutexLock(&g_vpak_dir_lock);
    auto it = g_vpak_dirs.find(dir);
    if (it == g_vpak_dirs.end()) {
        mutexUnlock(&g_vpak_dir_lock);
        errno = EBADF;
        return -1;
    }
    g_vpak_dirs.erase(it);
    VirtualPakDir* state = reinterpret_cast<VirtualPakDir*>(dir);
    mutexUnlock(&g_vpak_dir_lock);
    delete state;
    return 0;
}

static std::unordered_map<DIR*, std::string> g_readdirPaths;
static std::unordered_map<DIR*, unsigned> g_readdirCounts;
static std::unordered_map<DIR*, AndroidDirentCompat> g_readdirCompat;

// Android's CXGame::GetPlayerProfilePath() checks dirent::d_type directly.
// newlib on Switch can return DT_UNKNOWN for a valid filesystem entry, which
// makes the Android/Linux code incorrectly conclude that Profiles/Player does
// not exist. Recover the type from the real filesystem before returning the
// entry to the guest.
static void fixDirentType(const std::string& directory, struct dirent* ent) {
    if (!ent || ent->d_type == DT_DIR || ent->d_type == DT_LNK)
        return;

    std::string full;
    if (directory.empty() || directory == ".")
        full = std::string("./") + ent->d_name;
    else {
        full = directory;
        if (full.back() != '/')
            full.push_back('/');
        full += ent->d_name;
    }

    struct stat st = {};
    if (stat(full.c_str(), &st) != 0)
        return;

    if (S_ISDIR(st.st_mode))
        ent->d_type = DT_DIR;
    else if (S_ISREG(st.st_mode))
        ent->d_type = DT_REG;
}

static struct dirent* stub_readdir(DIR* dir) {
    if (!dir)
        return nullptr;

    if (vpakDirOwns(dir))
        return vpakDirRead(dir);

    struct dirent* ent = ::readdir(dir);
    if (!ent)
        return nullptr;

    auto it = g_readdirPaths.find(dir);
    if (it != g_readdirPaths.end()) {
        fixDirentType(it->second, ent);

        AndroidDirentCompat& compat = g_readdirCompat[dir];
        compat = AndroidDirentCompat{};
        compat.d_ino = (uint64_t)ent->d_ino;
        compat.d_off = 0;
        compat.d_type = (uint8_t)ent->d_type;

        const size_t srcLen = std::strlen(ent->d_name);
        const size_t maxName = sizeof(compat.d_name) - 1;
        const size_t copy = srcLen < maxName ? srcLen : maxName;
        std::memcpy(compat.d_name, ent->d_name, copy);
        compat.d_name[copy] = '\0';
        compat.d_reclen =
            (uint16_t)((offsetof(AndroidDirentCompat, d_name) + copy + 1 + 7) & ~7u);

        unsigned& count = g_readdirCounts[dir];
        ++count;
        return reinterpret_cast<struct dirent*>(&compat);
    }
    return ent;
}

static int stub_closedir(DIR* dir) {
    if (!dir)
        return -1;
    if (vpakDirOwns(dir))
        return vpakDirClose(dir);

    g_readdirPaths.erase(dir);
    g_readdirCounts.erase(dir);
    g_readdirCompat.erase(dir);
    return ::closedir(dir);
}

// Android/Bionic also exposes the 64-bit directory iterator as readdir64.
// On AArch64 the returned directory-entry layout is compatible with the
// newlib dirent used by this compatibility layer, so route it through the
// same native iterator while keeping a distinct diagnostic tag.
static struct dirent* stub_readdir64(DIR* dir) {
    if (!dir)
        return nullptr;

    if (vpakDirOwns(dir))
        return vpakDirRead(dir);

    struct dirent* ent = ::readdir(dir);
    if (!ent)
        return nullptr;

    auto it = g_readdirPaths.find(dir);
    if (it != g_readdirPaths.end()) {
        fixDirentType(it->second, ent);

        AndroidDirentCompat& compat = g_readdirCompat[dir];
        compat = AndroidDirentCompat{};
        compat.d_ino = (uint64_t)ent->d_ino;
        compat.d_off = 0;
        compat.d_type = (uint8_t)ent->d_type;

        const size_t srcLen = std::strlen(ent->d_name);
        const size_t maxName = sizeof(compat.d_name) - 1;
        const size_t copy = srcLen < maxName ? srcLen : maxName;
        std::memcpy(compat.d_name, ent->d_name, copy);
        compat.d_name[copy] = '\0';
        compat.d_reclen =
            (uint16_t)((offsetof(AndroidDirentCompat, d_name) + copy + 1 + 7) & ~7u);

        unsigned& count = g_readdirCounts[dir];
        ++count;
        return reinterpret_cast<struct dirent*>(&compat);
    }
    return ent;
}

static DIR* stub_opendir(const char* path) {
    const std::string ioPathStorage = normalizeSwitchFsPath(path);
    const char* ioPath = path ? ioPathStorage.c_str() : nullptr;

    rememberActiveLevelPak(ioPath);

    if (ioPath) {
        const size_t len = std::strlen(ioPath);
        if (len >= 4 &&
            std::tolower((unsigned char)ioPath[len - 4]) == '.' &&
            std::tolower((unsigned char)ioPath[len - 3]) == 'p' &&
            std::tolower((unsigned char)ioPath[len - 2]) == 'a' &&
            std::tolower((unsigned char)ioPath[len - 1]) == 'k') {
            return nullptr;
        }
    }

    // Shader directories are often present as empty loose mount points while
    // their real children live in FCData PAKs. Prefer the merged virtual view
    // for shader paths so the engine sees both sources, like Android CryPak.
    if (ioPath && isShaderPathForDiag(ioPath)) {
        if (DIR* virtualDir = vpakDirOpen(ioPath))
            return virtualDir;
    }

    DIR* d = opendir(ioPath);
    if (d) {
        g_readdirPaths[d] = ioPath ? ioPath : "";
        g_readdirCounts[d] = 0;
        return d;
    }

    std::string resolved;
    if (resolvePathCaseInsensitive(ioPath, resolved)) {
        d = opendir(resolved.c_str());
        if (d) {
            if (ioPath) {
                g_readdirPaths[d] = resolved;
                g_readdirCounts[d] = 0;
            }
            if (!isShaderPathForDiag(ioPath))
                compatLogFmt("opendir CASEFIX: %s -> %s",
                             ioPath ? ioPath : "?", resolved.c_str());
            return d;
        }
    }

    // A resource directory can exist only inside PAK content. Expose it as
    // an in-memory DIR* just like Android CryPak's ZipDir enumeration; do not
    // create a directory on the filesystem.
    if (ioPath) {
        if (DIR* virtualDir = vpakDirOpen(ioPath)) {
            if (!isShaderPathForDiag(ioPath))
                compatLogFmt("opendir PAK VIRTUAL: %s", ioPath);
            return virtualDir;
        }
    }

    if (!isShaderPathForDiag(ioPath))
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
    if (::stat(full.c_str(), &st) != 0) {
        std::string pakPath;
        PakEntryMeta meta;
        if (pakFindVirtualEntry(full.c_str(), pakPath, meta)) {
            st = {};
            st.st_mode = S_IFREG | 0444;
            st.st_nlink = 1;
            st.st_size = (off_t)meta.uncompressedSize;
            st.st_blksize = 4096;
            st.st_blocks =
                (blkcnt_t)(((uint64_t)meta.uncompressedSize + 511u) / 512u);
        } else if (pakVirtualDirectoryExists(full.c_str())) {
            st = {};
            st.st_mode = S_IFDIR | 0555;
            st.st_nlink = 2;
            st.st_blksize = 4096;
        } else {
            return false;
        }
    }

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

    while (struct dirent* ent = stub_readdir(state->dir)) {
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
        stub_closedir(state->dir);

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
        stub_closedir(state->dir);
    delete state;
    return 0;
}

// The original Android game can remove configuration files while
// resetting profiles. Keep these operations real on Switch so the filesystem
// behaves like the Android port; there is intentionally no root-config guard.
static int stub_remove(const char* path) {
    return ::remove(path);
}

static int stub_rename(const char* old_path, const char* new_path) {
    return ::rename(old_path, new_path);
}

// libnx/newlib does not provide the POSIX unlinkat() symbol. The Android game
// uses ordinary file-removal semantics for the configuration cleanup paths,
// so route the supported form through plain unlink().
static int stub_unlinkat(int dirfd, const char* path, int flags) {
    (void)dirfd;

    if (!path) {
        errno = EINVAL;
        return -1;
    }

    // The current callers use flags=0 (remove a file, not a directory).
    if (flags != 0) {
        errno = EINVAL;
        return -1;
    }

    return ::unlink(path);
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


// Diagnostic wrappers for wide-character conversion APIs. The current faulting
// instruction is a UTF-16-style STRH into x10, so record the API boundary before
// changing any conversion semantics.
static size_t g_wide_diag_calls = 0;

static size_t diag_mbsrtowcs(wchar_t* dst, const char** src, size_t len, mbstate_t* ps) {
    const size_t n = ++g_wide_diag_calls;
    if (n <= 24) {
        const char* s = (src && *src) ? *src : "";
        compatLogFmt("WIDE DIAG mbsrtowcs[%zu]: dst=%p src=%p len=%zu ps=%p text=%.96s",
                     n, (void*)dst, (void*)s, len, (void*)ps, s);
    }
    size_t rc = ::mbsrtowcs(dst, src, len, ps);
    if (n <= 24)
        compatLogFmt("WIDE DIAG mbsrtowcs[%zu]: rc=%zu dst=%p", n, rc, (void*)dst);
    return rc;
}

static size_t diag_mbtowc(wchar_t* dst, const char* src, size_t len) {
    const size_t n = ++g_wide_diag_calls;
    if (n <= 24) {
        compatLogFmt("WIDE DIAG mbtowc[%zu]: dst=%p src=%p len=%zu text=%.96s",
                     n, (void*)dst, (const void*)src, len, src ? src : "");
    }
    size_t rc = (size_t)::mbtowc(dst, src, len);
    if (n <= 24)
        compatLogFmt("WIDE DIAG mbtowc[%zu]: rc=%zu dst=%p", n, rc, (void*)dst);
    return rc;
}

static size_t diag_mbrtowc(wchar_t* dst, const char* src, size_t len, mbstate_t* ps) {
    const size_t n = ++g_wide_diag_calls;
    if (n <= 24) {
        compatLogFmt("WIDE DIAG mbrtowc[%zu]: dst=%p src=%p len=%zu ps=%p text=%.96s",
                     n, (void*)dst, (const void*)src, len, (void*)ps, src ? src : "");
    }
    size_t rc = ::mbrtowc(dst, src, len, ps);
    if (n <= 24)
        compatLogFmt("WIDE DIAG mbrtowc[%zu]: rc=%zu dst=%p", n, rc, (void*)dst);
    return rc;
}

static size_t diag_wcsrtombs(char* dst, const wchar_t** src, size_t len, mbstate_t* ps) {
    const size_t n = ++g_wide_diag_calls;
    if (n <= 24) {
        compatLogFmt("WIDE DIAG wcsrtombs[%zu]: dst=%p src=%p len=%zu ps=%p",
                     n, (void*)dst, (void*)((src && *src) ? *src : nullptr),
                     len, (void*)ps);
    }
    size_t rc = ::wcsrtombs(dst, src, len, ps);
    if (n <= 24)
        compatLogFmt("WIDE DIAG wcsrtombs[%zu]: rc=%zu dst=%p", n, rc, (void*)dst);
    return rc;
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

    if (mode == F_OK || !(mode & W_OK)) {
        std::string pakPath;
        PakEntryMeta meta;
        if (pakFindVirtualEntry(ioPath, pakPath, meta))
            return 0;

        // Directories that exist only inside PAK archives are also visible to
        // the Android-style virtual filesystem.
        if (pakVirtualDirectoryExists(ioPath))
            return 0;
    }

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
// Legacy NVIDIA texture formats used by the Android Far Cry renderer.
// Zink/NVK does not expose their old texture semantics, so translate the
// floating-point DSDT uploads into ordinary core texture formats while keeping
// the guest data layout intact.
#ifndef GL_RG32F
#define GL_RG32F 0x8230
#endif
#ifndef GL_RGB32F
#define GL_RGB32F 0x8815
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif

static constexpr GLenum kNearGL_HILO_NV              = 0x86F4;
static constexpr GLenum kNearGL_DSDT_NV              = 0x86F5;
static constexpr GLenum kNearGL_DSDT_MAG_NV          = 0x86F6;
static constexpr GLenum kNearGL_HILO16_NV            = 0x86F8;
static constexpr GLenum kNearGL_SIGNED_HILO_NV       = 0x86F9;
static constexpr GLenum kNearGL_SIGNED_HILO16_NV     = 0x86FA;
static constexpr GLenum kNearGL_SIGNED_RGB8_NV       = 0x86FF;
static constexpr GLenum kNearGL_COMPRESSED_3DC_ATI   = 0x8837;

// Core/EXT texture formats used as the closest normalized representation of
// the old NVIDIA/ATI formats. The Switch GL context advertises the required
// RG/RGTC and signed-normalized functionality through Mesa extensions.
#ifndef GL_RG8
#define GL_RG8 0x822B
#endif
#ifndef GL_RG16
#define GL_RG16 0x822C
#endif
#ifndef GL_RG8_SNORM
#define GL_RG8_SNORM 0x8F95
#endif
#ifndef GL_RG16_SNORM
#define GL_RG16_SNORM 0x8F99
#endif
#ifndef GL_RGB8_SNORM
#define GL_RGB8_SNORM 0x8F96
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#endif
#ifndef GL_TEXTURE_SWIZZLE_G
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#endif
#ifndef GL_TEXTURE_SWIZZLE_B
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif
#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif

static inline bool isNearDsdtFormat(GLenum format) {
    return format == kNearGL_DSDT_NV || format == kNearGL_DSDT_MAG_NV;
}

static inline bool isNearDdsDdtTextureName(const std::string& name) {
    return name.find("_ddt") != std::string::npos;
}

static inline bool isNearLegacyHiloFormat(GLenum format) {
    return format == kNearGL_HILO_NV ||
           format == kNearGL_HILO16_NV ||
           format == kNearGL_SIGNED_HILO_NV ||
           format == kNearGL_SIGNED_HILO16_NV ||
           format == kNearGL_SIGNED_RGB8_NV;
}

struct NearLegacyTexFormat {
    GLenum internalformat = 0;
    GLenum format = 0;
    GLenum type = 0;
    const char* label = nullptr;
};

// Map the old NVIDIA HILO/signed texture internal formats to ordinary
// normalized GL texture formats while keeping the original component count
// and numeric signedness. For the 16-bit variants the storage width follows
// the guest upload type (BYTE/UNSIGNED_BYTE -> 8-bit, SHORT/UNSIGNED_SHORT ->
// 16-bit). This also covers the Android renderer's HILO_NV + GL_BYTE path.
static bool nearMapLegacyHiloFormat(GLenum internalformat, GLenum format,
                                    GLenum type, NearLegacyTexFormat& out) {
    const bool signedRgb =
        internalformat == kNearGL_SIGNED_RGB8_NV ||
        format == kNearGL_SIGNED_RGB8_NV;

    if (signedRgb) {
        // Android's texture-format helpers may use the legacy
        // GL_SIGNED_RGB8_NV token in either the internalformat or format slot.
        // Both forms describe a 3-component signed normalized texture.
        if (type != GL_BYTE && type != GL_UNSIGNED_BYTE)
            return false;
        out.internalformat = GL_RGB8_SNORM;
        out.format = GL_RGB;
        out.type = type;
        out.label = "SIGNED_RGB8";
        return true;
    }

    if (!isNearLegacyHiloFormat(internalformat) ||
        format != kNearGL_HILO_NV)
        return false;

    const bool signedFormat =
        internalformat == kNearGL_SIGNED_HILO_NV ||
        internalformat == kNearGL_SIGNED_HILO16_NV;

    if (type == GL_BYTE) {
        out.internalformat = signedFormat ? GL_RG8_SNORM : GL_RG8;
        out.format = GL_RG;
        out.type = signedFormat ? GL_BYTE : GL_UNSIGNED_BYTE;
        out.label = signedFormat ? "SIGNED_HILO8" : "HILO8";
        return true;
    }

    if (type == GL_UNSIGNED_BYTE) {
        out.internalformat = signedFormat ? GL_RG8_SNORM : GL_RG8;
        out.format = GL_RG;
        out.type = GL_UNSIGNED_BYTE;
        out.label = signedFormat ? "SIGNED_HILO8" : "HILO8";
        return true;
    }

    if (type == GL_SHORT) {
        out.internalformat = signedFormat ? GL_RG16_SNORM : GL_RG16;
        out.format = GL_RG;
        out.type = GL_SHORT;
        out.label = signedFormat ? "SIGNED_HILO16" : "HILO16";
        return true;
    }

    if (type == GL_UNSIGNED_SHORT) {
        out.internalformat = signedFormat ? GL_RG16_SNORM : GL_RG16;
        out.format = GL_RG;
        out.type = GL_UNSIGNED_SHORT;
        out.label = signedFormat ? "SIGNED_HILO16" : "HILO16";
        return true;
    }

    return false;
}

static inline bool isNear3dcCompressedFormat(GLenum format) {
    return format == kNearGL_COMPRESSED_3DC_ATI;
}

// ATI 3Dc is a two-channel 4x4 block compression format. RGTC2 exposes the
// corresponding two-channel block layout on modern GL. The original ATI
// format has LUMINANCE+ALPHA sampling semantics (R=L, G=L, B=L, A=A), so
// apply the texture swizzle after uploading as RGTC2 to retain the guest
// shader-visible channels.
static void nearApply3dcSwizzle(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
}

// Guest GL unpack state used by the texture diagnostic. These are per-thread
// because CryEngine's texture workers keep their own current GL context/state.
// Defaults match OpenGL: alignment=4, row/skip fields=0.
static thread_local GLint g_glUnpackAlignment = 4;
static thread_local GLint g_glUnpackRowLength = 0;
static thread_local GLint g_glUnpackSkipPixels = 0;
static thread_local GLint g_glUnpackSkipRows = 0;
#if defined(GL_UNPACK_IMAGE_HEIGHT)
static thread_local GLint g_glUnpackImageHeight = 0;
#endif
#if defined(GL_UNPACK_SWAP_BYTES)
static thread_local GLint g_glUnpackSwapBytes = GL_FALSE;
#endif
#if defined(GL_UNPACK_LSB_FIRST)
static thread_local GLint g_glUnpackLsbFirst = GL_FALSE;
#endif

static const char* nearGlPixelStoreName(GLenum pname) {
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: return "UNPACK_ALIGNMENT";
#ifdef GL_PACK_ALIGNMENT
    case GL_PACK_ALIGNMENT: return "PACK_ALIGNMENT";
#endif
#if defined(GL_UNPACK_ROW_LENGTH)
    case GL_UNPACK_ROW_LENGTH: return "UNPACK_ROW_LENGTH";
#endif
#if defined(GL_UNPACK_SKIP_PIXELS)
    case GL_UNPACK_SKIP_PIXELS: return "UNPACK_SKIP_PIXELS";
#endif
#if defined(GL_UNPACK_SKIP_ROWS)
    case GL_UNPACK_SKIP_ROWS: return "UNPACK_SKIP_ROWS";
#endif
#if defined(GL_UNPACK_IMAGE_HEIGHT)
    case GL_UNPACK_IMAGE_HEIGHT: return "UNPACK_IMAGE_HEIGHT";
#endif
#if defined(GL_UNPACK_SWAP_BYTES)
    case GL_UNPACK_SWAP_BYTES: return "UNPACK_SWAP_BYTES";
#endif
#if defined(GL_UNPACK_LSB_FIRST)
    case GL_UNPACK_LSB_FIRST: return "UNPACK_LSB_FIRST";
#endif
#ifdef GL_PACK_ROW_LENGTH
    case GL_PACK_ROW_LENGTH: return "PACK_ROW_LENGTH";
#endif
#ifdef GL_PACK_SKIP_PIXELS
    case GL_PACK_SKIP_PIXELS: return "PACK_SKIP_PIXELS";
#endif
#ifdef GL_PACK_SKIP_ROWS
    case GL_PACK_SKIP_ROWS: return "PACK_SKIP_ROWS";
#endif
#ifdef GL_PACK_IMAGE_HEIGHT
    case GL_PACK_IMAGE_HEIGHT: return "PACK_IMAGE_HEIGHT";
#endif
#ifdef GL_PACK_SWAP_BYTES
    case GL_PACK_SWAP_BYTES: return "PACK_SWAP_BYTES";
#endif
#ifdef GL_PACK_LSB_FIRST
    case GL_PACK_LSB_FIRST: return "PACK_LSB_FIRST";
#endif
    default: return nullptr;
    }
}

static void shim_glPixelStorei(GLenum pname, GLint param) {
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:
        g_glUnpackAlignment = param;
        break;
#if defined(GL_UNPACK_ROW_LENGTH)
    case GL_UNPACK_ROW_LENGTH:
        g_glUnpackRowLength = param;
        break;
#endif
#if defined(GL_UNPACK_SKIP_PIXELS)
    case GL_UNPACK_SKIP_PIXELS:
        g_glUnpackSkipPixels = param;
        break;
#endif
#if defined(GL_UNPACK_SKIP_ROWS)
    case GL_UNPACK_SKIP_ROWS:
        g_glUnpackSkipRows = param;
        break;
#endif
#if defined(GL_UNPACK_IMAGE_HEIGHT)
    case GL_UNPACK_IMAGE_HEIGHT:
        g_glUnpackImageHeight = param;
        break;
#endif
#if defined(GL_UNPACK_SWAP_BYTES)
    case GL_UNPACK_SWAP_BYTES:
        g_glUnpackSwapBytes = param;
        break;
#endif
#if defined(GL_UNPACK_LSB_FIRST)
    case GL_UNPACK_LSB_FIRST:
        g_glUnpackLsbFirst = param;
        break;
#endif
    default:
        break;
    }

    const char* label = nearGlPixelStoreName(pname);
    if (label) {
        compatPakLog("GL PIXELSTORE: %s pname=0x%x param=%d",
                     label, (unsigned)pname, (int)param);
    } else {
        compatPakLog("GL PIXELSTORE: UNKNOWN pname=0x%x param=%d",
                     (unsigned)pname, (int)param);
    }

    glPixelStorei(pname, param);
}

static unsigned nearGlFormatComponents(GLenum format) {
    switch (format) {
    case GL_RED:
    case GL_ALPHA:
    case GL_LUMINANCE:
        return 1;
    case GL_RG:
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
        return 3;
#ifdef GL_BGR
    case GL_BGR:
        return 3;
#endif
    case GL_RGBA:
        return 4;
#ifdef GL_BGRA
    case GL_BGRA:
        return 4;
#endif
    default:
        return 0;
    }
}

static unsigned nearGlTypeBytes(GLenum type, bool& packed) {
    packed = false;
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT:
        return 2;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_FLOAT:
        return 4;
#ifdef GL_UNSIGNED_SHORT_5_6_5
    case GL_UNSIGNED_SHORT_5_6_5:
        packed = true;
        return 2;
#endif
#ifdef GL_UNSIGNED_SHORT_4_4_4_4
    case GL_UNSIGNED_SHORT_4_4_4_4:
        packed = true;
        return 2;
#endif
#ifdef GL_UNSIGNED_SHORT_5_5_5_1
    case GL_UNSIGNED_SHORT_5_5_5_1:
        packed = true;
        return 2;
#endif
#ifdef GL_UNSIGNED_INT_24_8
    case GL_UNSIGNED_INT_24_8:
        packed = true;
        return 4;
#endif
#ifdef GL_FLOAT_32_UNSIGNED_INT_24_8_REV
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        packed = true;
        return 8;
#endif
    default:
        return 0;
    }
}

static void nearLogTextureBytes(const std::string& name,
                                GLsizei width, GLsizei height,
                                GLenum format, GLenum type) {
    const unsigned components = nearGlFormatComponents(format);
    bool packed = false;
    const unsigned typeBytes = nearGlTypeBytes(type, packed);
    const unsigned bytesPerPixel =
        packed ? typeBytes : (components && typeBytes ? components * typeBytes : 0);

    const uint64_t rowPixels = (g_glUnpackRowLength > 0)
        ? (uint64_t)g_glUnpackRowLength
        : (uint64_t)std::max<GLsizei>(width, 0);
    const uint64_t tightRow = bytesPerPixel ? rowPixels * bytesPerPixel : 0;
    const unsigned alignment =
        (g_glUnpackAlignment > 0) ? (unsigned)g_glUnpackAlignment : 1u;
    const uint64_t paddedRow =
        tightRow ? ((tightRow + alignment - 1u) / alignment) * alignment : 0;
    const uint64_t skipOffset =
        (uint64_t)std::max<GLint>(g_glUnpackSkipRows, 0) * paddedRow +
        (uint64_t)std::max<GLint>(g_glUnpackSkipPixels, 0) * bytesPerPixel;
    const uint64_t requiredBytes =
        (height > 0 && paddedRow && bytesPerPixel)
            ? skipOffset +
              (uint64_t)(height - 1) * paddedRow +
              (uint64_t)std::max<GLsizei>(width, 0) * bytesPerPixel
            : skipOffset;

    compatPakLog(
        "GL TEX LAYOUT: name=%s format=0x%x type=0x%x components=%u typeBytes=%u packed=%u "
        "alignment=%d rowLength=%d skipPixels=%d skipRows=%d"
#if defined(GL_UNPACK_IMAGE_HEIGHT)
        " imageHeight=%d"
#endif
#if defined(GL_UNPACK_SWAP_BYTES)
        " swapBytes=%d"
#endif
#if defined(GL_UNPACK_LSB_FIRST)
        " lsbFirst=%d"
#endif
        " bpp=%u rowPixels=%" PRIu64 " tightRow=%" PRIu64
        " paddedRow=%" PRIu64 " skipOffset=%" PRIu64
        " requiredBytes=%" PRIu64 " width=%d height=%d",
        name.c_str(), (unsigned)format, (unsigned)type,
        components, typeBytes, packed ? 1u : 0u,
        (int)g_glUnpackAlignment, (int)g_glUnpackRowLength,
        (int)g_glUnpackSkipPixels, (int)g_glUnpackSkipRows,
#if defined(GL_UNPACK_IMAGE_HEIGHT)
        (int)g_glUnpackImageHeight,
#endif
#if defined(GL_UNPACK_SWAP_BYTES)
        (int)g_glUnpackSwapBytes,
#endif
#if defined(GL_UNPACK_LSB_FIRST)
        (int)g_glUnpackLsbFirst,
#endif
        bytesPerPixel, rowPixels, tightRow, paddedRow,
        skipOffset, requiredBytes, (int)width, (int)height);
}

static bool nearIsWaterTextureDiagName(const std::string& name) {
    std::string s(name);
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);

    return s.find("caust") != std::string::npos ||
           s.find("causq") != std::string::npos ||
           s.find("water_") != std::string::npos ||
           s.find("oldforest_trunk_moss") != std::string::npos ||
           s.find("rust") != std::string::npos ||
           s.find("_ddn") != std::string::npos ||
           s.find("_ddp") != std::string::npos ||
           s.find("/moss") != std::string::npos;
}

static uint64_t nearFnv1a64(const unsigned char* data, size_t size) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool nearHashSourceAsRgba(const void* pixels,
                                 GLsizei width, GLsizei height,
                                 GLenum format, GLenum type,
                                 uint64_t& outRgbaHash,
                                 uint64_t& outRgbHash,
                                 unsigned char first[16]) {
    if (!pixels || width <= 0 || height <= 0 || type != GL_UNSIGNED_BYTE)
        return false;

    const unsigned components = nearGlFormatComponents(format);
    if (components < 3 || components > 4)
        return false;
    if (format != GL_RGB && format != GL_RGBA &&
        format != GL_BGR && format != GL_BGRA)
        return false;

    const uint64_t rowPixels = g_glUnpackRowLength > 0
        ? (uint64_t)g_glUnpackRowLength
        : (uint64_t)width;
    const uint64_t tightRow = rowPixels * (uint64_t)components;
    const unsigned alignment =
        g_glUnpackAlignment > 0 ? (unsigned)g_glUnpackAlignment : 1u;
    const uint64_t paddedRow =
        ((tightRow + alignment - 1u) / alignment) * alignment;
    const uint64_t skipOffset =
        (uint64_t)std::max<GLint>(g_glUnpackSkipRows, 0) * paddedRow +
        (uint64_t)std::max<GLint>(g_glUnpackSkipPixels, 0) * components;
    const uint64_t sourceSpan =
        height > 0
            ? skipOffset +
              (uint64_t)(height - 1) * paddedRow +
              (uint64_t)width * components
            : skipOffset;

    // Diagnostic only: never scan arbitrarily large guest buffers.
    if (sourceSpan > 4ULL * 1024ULL * 1024ULL)
        return false;

    uint64_t rgbaHash = 1469598103934665603ULL;
    uint64_t rgbHash = 1469598103934665603ULL;
    size_t firstCount = 0;
    const unsigned char* base =
        reinterpret_cast<const unsigned char*>(pixels) + skipOffset;

    for (GLsizei y = 0; y < height; ++y) {
        const unsigned char* row =
            base + (uint64_t)y * paddedRow;
        for (GLsizei x = 0; x < width; ++x) {
            const unsigned char* p = row + (size_t)x * components;
            unsigned char px[4];

            if (format == GL_BGRA) {
                px[0] = p[2];
                px[1] = p[1];
                px[2] = p[0];
                px[3] = p[3];
            } else if (format == GL_BGR) {
                px[0] = p[2];
                px[1] = p[1];
                px[2] = p[0];
                px[3] = 255;
            } else if (format == GL_RGBA) {
                px[0] = p[0];
                px[1] = p[1];
                px[2] = p[2];
                px[3] = p[3];
            } else {
                px[0] = p[0];
                px[1] = p[1];
                px[2] = p[2];
                px[3] = 255;
            }

            for (unsigned k = 0; k < 4; ++k) {
                rgbaHash ^= (uint64_t)px[k];
                rgbaHash *= 1099511628211ULL;
                if (firstCount < 16)
                    first[firstCount++] = px[k];
                if (k < 3) {
                    rgbHash ^= (uint64_t)px[k];
                    rgbHash *= 1099511628211ULL;
                }
            }
        }
    }

    outRgbaHash = rgbaHash;
    outRgbHash = rgbHash;
    return true;
}

static void nearVerifyUploadedTexture(const std::string& name,
                                      GLenum target, GLint level,
                                      GLsizei width, GLsizei height,
                                      GLenum format, GLenum type,
                                      const void* pixels,
                                      GLint unpackBuffer,
                                      GLenum uploadError) {
    if (!nearIsWaterTextureDiagName(name) ||
        target != GL_TEXTURE_2D || level != 0 ||
        unpackBuffer != 0 || !pixels ||
        width <= 0 || height <= 0 ||
        width > 512 || height > 512 ||
        type != GL_UNSIGNED_BYTE)
        return;

    unsigned char sourceFirst[16] = {};
    uint64_t sourceRgbaHash = 0;
    uint64_t sourceRgbHash = 0;
    const bool sourceOk = nearHashSourceAsRgba(
        pixels, width, height, format, type,
        sourceRgbaHash, sourceRgbHash, sourceFirst);

    GLint storedW = -1;
    GLint storedH = -1;
    GLint storedInternal = -1;
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &storedW);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &storedH);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT,
                             &storedInternal);
    const GLenum queryError = glGetError();

    const size_t readbackBytes =
        (size_t)width * (size_t)height * 4u;
    unsigned char* readback =
        (unsigned char*)std::malloc(readbackBytes);
    uint64_t gpuHash = 0;
    uint64_t gpuRgbHash = 1469598103934665603ULL;
    unsigned char gpuFirst[16] = {};
    GLenum readbackError = GL_NO_ERROR;

    if (!readback) {
        readbackError = GL_OUT_OF_MEMORY;
    } else {
        std::memset(readback, 0, readbackBytes);
        glGetTexImage(target, level, GL_RGBA, GL_UNSIGNED_BYTE, readback);
        readbackError = glGetError();
        if (readbackError == GL_NO_ERROR) {
            gpuHash = nearFnv1a64(readback, readbackBytes);
            for (size_t i = 0; i + 2 < readbackBytes; i += 4) {
                gpuRgbHash ^= (uint64_t)readback[i + 0];
                gpuRgbHash *= 1099511628211ULL;
                gpuRgbHash ^= (uint64_t)readback[i + 1];
                gpuRgbHash *= 1099511628211ULL;
                gpuRgbHash ^= (uint64_t)readback[i + 2];
                gpuRgbHash *= 1099511628211ULL;
            }
            const size_t n = readbackBytes < 16 ? readbackBytes : 16;
            std::memcpy(gpuFirst, readback, n);
        }
        std::free(readback);
    }

    compatPakLog(
        "GL TEX VERIFY: name=%s level=%d source_ok=%d source_rgba_hash=0x%016" PRIx64
        " source_rgb_hash=0x%016" PRIx64
        " source_rgba16=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
        "stored=%dx%d stored_internal=0x%x queryerr=0x%x uploaderr=0x%x "
        "gpu_hash=0x%016" PRIx64
        " gpu_rgba16=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
        " gpu_rgb_hash=0x%016" PRIx64 " readbackerr=0x%x rgb_match=%s",

        name.c_str(), level, sourceOk ? 1 : 0, sourceRgbaHash, sourceRgbHash,
        sourceFirst[0], sourceFirst[1], sourceFirst[2], sourceFirst[3],
        sourceFirst[4], sourceFirst[5], sourceFirst[6], sourceFirst[7],
        sourceFirst[8], sourceFirst[9], sourceFirst[10], sourceFirst[11],
        sourceFirst[12], sourceFirst[13], sourceFirst[14], sourceFirst[15],
        storedW, storedH, (unsigned)storedInternal, (unsigned)queryError,
        (unsigned)uploadError, gpuHash,
        gpuFirst[0], gpuFirst[1], gpuFirst[2], gpuFirst[3],
        gpuFirst[4], gpuFirst[5], gpuFirst[6], gpuFirst[7],
        gpuFirst[8], gpuFirst[9], gpuFirst[10], gpuFirst[11],
        gpuFirst[12], gpuFirst[13], gpuFirst[14], gpuFirst[15],
        gpuRgbHash, (unsigned)readbackError,
        (sourceOk && readbackError == GL_NO_ERROR &&
         sourceRgbHash == gpuRgbHash)
            ? "YES" : "NO");
}

static const char* nearGlTexTargetName(GLenum target) {
    switch (target) {
    case GL_TEXTURE_2D: return "TEXTURE_2D";
#ifdef GL_TEXTURE_CUBE_MAP_POSITIVE_X
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: return "CUBE_POSITIVE_X";
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X: return "CUBE_NEGATIVE_X";
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: return "CUBE_POSITIVE_Y";
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y: return "CUBE_NEGATIVE_Y";
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: return "CUBE_POSITIVE_Z";
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: return "CUBE_NEGATIVE_Z";
#endif
    default: return "OTHER";
    }
}

static Mutex g_textureStateDiagLock;
static std::unordered_map<GLuint, std::string> g_glTextureDiagNames;

static std::string nearBoundTextureDiagName(GLuint texture) {
    if (!texture)
        return {};
    mutexLock(&g_textureStateDiagLock);
    auto it = g_glTextureDiagNames.find(texture);
    const std::string name =
        (it != g_glTextureDiagNames.end()) ? it->second : std::string();
    mutexUnlock(&g_textureStateDiagLock);
    return name;
}

static void nearRememberTextureDiagName(GLuint texture, const std::string& name) {
    if (!texture || !nearIsWaterTextureDiagName(name))
        return;
    mutexLock(&g_textureStateDiagLock);
    g_glTextureDiagNames[texture] = name;
    mutexUnlock(&g_textureStateDiagLock);
}

static bool nearGetDiagTextureState(GLint& activeTexture, GLint& textureBinding,
                                    std::string& name) {
    activeTexture = 0;
    textureBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    name = nearBoundTextureDiagName((GLuint)textureBinding);
    return !name.empty();
}

static void shim_glActiveTexture(GLenum texture) {
    glActiveTexture(texture);

    GLint active = 0;
    GLint binding = 0;
    std::string name;
    if (nearGetDiagTextureState(active, binding, name)) {
        compatPakLog(
            "GL TEX STATE: active_texture=0x%x bound_texture=%d name=%s event=active",
            (unsigned)active, binding, name.c_str());
    }
}

static void shim_glBindTexture(GLenum target, GLuint texture) {
    glBindTexture(target, texture);

    if (target != GL_TEXTURE_2D)
        return;

    GLint active = 0;
    GLint binding = 0;
    std::string name;
    if (nearGetDiagTextureState(active, binding, name)) {
        compatPakLog(
            "GL TEX STATE: active_texture=0x%x bound_texture=%d name=%s event=bind",
            (unsigned)active, binding, name.c_str());
    }
}

static void shim_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    glTexParameteri(target, pname, param);

    if (target != GL_TEXTURE_2D)
        return;

    GLint active = 0;
    GLint binding = 0;
    std::string name;
    if (nearGetDiagTextureState(active, binding, name)) {
        compatPakLog(
            "GL TEX STATE: active_texture=0x%x texture=%d name=%s event=texParameteri pname=0x%x param=%d",
            (unsigned)active, binding, name.c_str(), (unsigned)pname, param);
    }
}

static void shim_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameterf(target, pname, param);

    if (target != GL_TEXTURE_2D)
        return;

    GLint active = 0;
    GLint binding = 0;
    std::string name;
    if (nearGetDiagTextureState(active, binding, name)) {
        compatPakLog(
            "GL TEX STATE: active_texture=0x%x texture=%d name=%s event=texParameterf pname=0x%x param=%g",
            (unsigned)active, binding, name.c_str(), (unsigned)pname, (double)param);
    }
}

static void shim_glTexParameteriv(GLenum target, GLenum pname, const GLint* params) {
    glTexParameteriv(target, pname, params);

    if (target != GL_TEXTURE_2D || !params)
        return;

    GLint active = 0;
    GLint binding = 0;
    std::string name;
    if (nearGetDiagTextureState(active, binding, name)) {
        compatPakLog(
            "GL TEX STATE: active_texture=0x%x texture=%d name=%s event=texParameteriv pname=0x%x param0=%d",
            (unsigned)active, binding, name.c_str(), (unsigned)pname, params[0]);
    }
}

static void shim_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params) {
    glTexParameterfv(target, pname, params);

    if (target != GL_TEXTURE_2D || !params)
        return;

    GLint active = 0;
    GLint binding = 0;
    std::string name;
    if (nearGetDiagTextureState(active, binding, name)) {
        compatPakLog(
            "GL TEX STATE: active_texture=0x%x texture=%d name=%s event=texParameterfv pname=0x%x param0=%g",
            (unsigned)active, binding, name.c_str(), (unsigned)pname, (double)params[0]);
    }
}

static const char* nearArbProgramTargetName(GLenum target);

// GL_NV_texture_shader compatibility state. Mesa/Zink exposes the
// legacy ARB program path used by Far Cry but not the original NVIDIA texture
// shader extension. Keep the extension state locally and emulate the common
// bump/EMBM operations in a small ARB fragment program at draw time.
static constexpr GLenum kNearGL_TEXTURE_SHADER_NV = 0x86DE;
static constexpr GLenum kNearGL_SHADER_OPERATION_NV = 0x86DF;
static constexpr GLenum kNearGL_CULL_MODES_NV = 0x86E0;
static constexpr GLenum kNearGL_OFFSET_TEXTURE_MATRIX_NV = 0x86E1;
static constexpr GLenum kNearGL_OFFSET_TEXTURE_SCALE_NV = 0x86E2;
static constexpr GLenum kNearGL_OFFSET_TEXTURE_BIAS_NV = 0x86E3;
static constexpr GLenum kNearGL_PREVIOUS_TEXTURE_INPUT_NV = 0x86E4;
static constexpr GLenum kNearGL_CONST_EYE_NV = 0x86E5;
static constexpr GLenum kNearGL_PASS_THROUGH_NV = 0x86E6;
static constexpr GLenum kNearGL_CULL_FRAGMENT_NV = 0x86E7;
static constexpr GLenum kNearGL_OFFSET_TEXTURE_2D_NV = 0x86E8;
static constexpr GLenum kNearGL_DEPENDENT_AR_TEXTURE_2D_NV = 0x86E9;
static constexpr GLenum kNearGL_DEPENDENT_GB_TEXTURE_2D_NV = 0x86EA;
static constexpr GLenum kNearGL_DOT_PRODUCT_NV = 0x86EC;
static constexpr GLenum kNearGL_DOT_PRODUCT_DEPTH_REPLACE_NV = 0x86ED;
static constexpr GLenum kNearGL_DOT_PRODUCT_TEXTURE_2D_NV = 0x86EE;
static constexpr GLenum kNearGL_DOT_PRODUCT_TEXTURE_3D_NV = 0x86EF;
static constexpr GLenum kNearGL_DOT_PRODUCT_TEXTURE_CUBE_MAP_NV = 0x86F0;
static constexpr GLenum kNearGL_DOT_PRODUCT_DIFFUSE_CUBE_MAP_NV = 0x86F1;
static constexpr GLenum kNearGL_DOT_PRODUCT_REFLECT_CUBE_MAP_NV = 0x86F2;
static constexpr GLenum kNearGL_DOT_PRODUCT_CONST_EYE_REFLECT_CUBE_MAP_NV = 0x86F3;
static constexpr GLenum kNearGL_OFFSET_TEXTURE_RECTANGLE_NV = 0x864C;
static constexpr GLenum kNearGL_OFFSET_TEXTURE_RECTANGLE_SCALE_NV = 0x864D;
static constexpr GLenum kNearGL_DOT_PRODUCT_TEXTURE_RECTANGLE_NV = 0x864E;
static constexpr GLenum kNearGL_TEXTURE_RECTANGLE_NV = 0x84F5;
static constexpr GLenum kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_NV = 0x8850;
static constexpr GLenum kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_SCALE_NV = 0x8851;
static constexpr GLenum kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_NV = 0x8852;
static constexpr GLenum kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_SCALE_NV = 0x8853;
static constexpr GLenum kNearGL_RGBA_UNSIGNED_DOT_PRODUCT_MAPPING_NV = 0x86D9;
static constexpr GLenum kNearGL_PREVIOUS_TEXTURE_INPUT_BASE = 0x84C0;
static constexpr GLenum kNearGL_FRAGMENT_PROGRAM_ARB = 0x8804;
static constexpr GLenum kNearGL_VERTEX_PROGRAM_ARB = 0x8620;
static constexpr GLenum kNearGL_FRAGMENT_PROGRAM_BINDING_ARB = 0x8873;
static constexpr GLenum kNearGL_PROGRAM_FORMAT_ASCII_ARB = 0x8875;
static constexpr GLenum kNearGL_PROGRAM_ERROR_POSITION_ARB = 0x864B;

static constexpr GLint kNearGL_UNSIGNED_IDENTITY_NV = 0x8536;
static constexpr GLint kNearGL_EXPAND_NORMAL_NV = 0x8538;
static constexpr unsigned kNearTextureShaderUnits = 8;

struct NearTextureShaderUnitState {
    GLenum op = GL_NONE;
    GLint previous = 0;
    GLfloat offset[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    GLfloat scale = 1.0f;
    GLfloat bias = 0.0f;
    GLfloat cullModes[4] = {
        (GLfloat)GL_GEQUAL, (GLfloat)GL_GEQUAL,
        (GLfloat)GL_GEQUAL, (GLfloat)GL_GEQUAL
    };
    GLfloat constEye[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    GLint dotMapping = kNearGL_UNSIGNED_IDENTITY_NV;
};

static thread_local GLint g_nearTextureEnvModes[kNearTextureShaderUnits] = {
    GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE,
    GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE
};

struct NearTextureShaderState {
    NearTextureShaderUnitState units[kNearTextureShaderUnits];
    bool enabled = false;
    GLuint program = 0;
    std::string source;
};

static thread_local NearTextureShaderState g_nearTextureShaderState;

struct NearTextureShaderDrawRestore {
    bool active = false;
    GLboolean fragmentEnabled = GL_FALSE;
    GLint fragmentBinding = 0;
};

static thread_local NearTextureShaderDrawRestore g_nearTextureShaderDrawRestore;


static int nearTextureShaderActiveUnit() {
    GLint active = (GLint)GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    const GLint unit = active - (GLint)GL_TEXTURE0;
    return (unit >= 0 && unit < (GLint)kNearTextureShaderUnits) ? unit : -1;
}

static bool nearTextureShaderHasStage() {
    if (!g_nearTextureShaderState.enabled)
        return false;
    for (unsigned i = 0; i < kNearTextureShaderUnits; ++i)
        if (g_nearTextureShaderState.units[i].op != GL_NONE)
            return true;
    return false;
}

static const char* nearTextureShaderStageName(unsigned i) {
    static const char* kNames[kNearTextureShaderUnits] =
        {"ts0","ts1","ts2","ts3","ts4","ts5","ts6","ts7"};
    return kNames[i];
}

static const char* nearTextureShaderTexTarget(GLenum op) {
    switch (op) {
    case GL_TEXTURE_1D: return "1D";
    case GL_TEXTURE_3D: return "3D";
    case GL_TEXTURE_CUBE_MAP:
    case kNearGL_DOT_PRODUCT_TEXTURE_CUBE_MAP_NV:
    case kNearGL_DOT_PRODUCT_DIFFUSE_CUBE_MAP_NV:
    case kNearGL_DOT_PRODUCT_REFLECT_CUBE_MAP_NV:
    case kNearGL_DOT_PRODUCT_CONST_EYE_REFLECT_CUBE_MAP_NV:
        return "CUBE";
    case kNearGL_TEXTURE_RECTANGLE_NV:
    case kNearGL_OFFSET_TEXTURE_RECTANGLE_NV:
    case kNearGL_OFFSET_TEXTURE_RECTANGLE_SCALE_NV:
    case kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_NV:
    case kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_SCALE_NV:
    case kNearGL_DOT_PRODUCT_TEXTURE_RECTANGLE_NV:
        return "RECT";
    default:
        return "2D";
    }
}

static void nearTextureShaderAppendFloat(std::string& s, GLfloat v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.9g", (double)v);
    s += b;
}

static void nearTextureShaderAppendParam(std::string& s, const std::string& name,
                                         GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    s += "PARAM ";
    s += name;
    s += " = { ";
    nearTextureShaderAppendFloat(s, a); s += ", ";
    nearTextureShaderAppendFloat(s, b); s += ", ";
    nearTextureShaderAppendFloat(s, c); s += ", ";
    nearTextureShaderAppendFloat(s, d);
    s += " };\\n";
}

static bool nearTextureShaderBuildSource(std::string& out) {
    if (!nearTextureShaderHasStage())
        return false;

    out.clear();
    out += "!!ARBfp1.0\\n";
    out += "TEMP ts0, ts1, ts2, ts3, ts4, ts5, ts6, ts7;\\n";
    out += "TEMP coord, tmp, color;\\n";

    // Parameter declarations must precede executable instructions in ARBfp.
    for (unsigned i = 0; i < kNearTextureShaderUnits; ++i) {
        const NearTextureShaderUnitState& st = g_nearTextureShaderState.units[i];
        if (st.op == GL_NONE)
            continue;
        nearTextureShaderAppendParam(out, "m" + std::to_string(i),
                                     st.offset[0], st.offset[1],
                                     st.offset[2], st.offset[3]);
        nearTextureShaderAppendParam(out, "sb" + std::to_string(i),
                                     st.scale, st.bias, 0.0f, 0.0f);
    }

    out += "MOV color, fragment.color.primary;\\n";

    bool any = false;
    for (unsigned i = 0; i < kNearTextureShaderUnits; ++i) {
        const NearTextureShaderUnitState& st = g_nearTextureShaderState.units[i];
        if (st.op == GL_NONE)
            continue;
        any = true;

        const unsigned p = (unsigned)std::max(
            0, std::min(st.previous, (GLint)kNearTextureShaderUnits - 1));
        const char* dst = nearTextureShaderStageName(i);
        const char* prevName = nearTextureShaderStageName(p);

        switch (st.op) {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_CUBE_MAP:
            out += "TEX " + std::string(dst) + ", fragment.texcoord[" +
                   std::to_string(i) + "], texture[" + std::to_string(i) +
                   "], " + nearTextureShaderTexTarget(st.op) + ";\\n";
            break;

        case kNearGL_PASS_THROUGH_NV:
            out += "MOV " + std::string(dst) + ", fragment.texcoord[" +
                   std::to_string(i) + "];\\n";
            break;

        case kNearGL_OFFSET_TEXTURE_2D_NV:
        case kNearGL_OFFSET_TEXTURE_SCALE_NV:
        case kNearGL_OFFSET_TEXTURE_RECTANGLE_NV:
        case kNearGL_OFFSET_TEXTURE_RECTANGLE_SCALE_NV:
        case kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_NV:
        case kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_SCALE_NV:
        case kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_NV:
        case kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_SCALE_NV: {
            const bool projective =
                st.op == kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_NV ||
                st.op == kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_SCALE_NV ||
                st.op == kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_NV ||
                st.op == kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_SCALE_NV;

            out += "MOV coord, fragment.texcoord[" + std::to_string(i) + "];\\n";
            if (projective) {
                out += "RCP tmp.x, coord.w;\\n";
                out += "MUL coord.x, coord.x, tmp.x;\\n";
                out += "MUL coord.y, coord.y, tmp.x;\\n";
            }

            out += "MUL tmp.x, " + std::string(prevName) + ".x, m" +
                   std::to_string(i) + ".x;\\n";
            out += "MAD tmp.x, " + std::string(prevName) + ".y, m" +
                   std::to_string(i) + ".z, tmp.x;\\n";
            out += "ADD coord.x, coord.x, tmp.x;\\n";

            out += "MUL tmp.x, " + std::string(prevName) + ".x, m" +
                   std::to_string(i) + ".y;\\n";
            out += "MAD tmp.x, " + std::string(prevName) + ".y, m" +
                   std::to_string(i) + ".w, tmp.x;\\n";
            out += "ADD coord.y, coord.y, tmp.x;\\n";

            out += "TEX " + std::string(dst) + ", coord, texture[" +
                   std::to_string(i) + "], " +
                   nearTextureShaderTexTarget(st.op) + ";\\n";

            if (st.op == kNearGL_OFFSET_TEXTURE_SCALE_NV ||
                st.op == kNearGL_OFFSET_TEXTURE_RECTANGLE_SCALE_NV ||
                st.op == kNearGL_OFFSET_PROJECTIVE_TEXTURE_2D_SCALE_NV ||
                st.op == kNearGL_OFFSET_PROJECTIVE_TEXTURE_RECTANGLE_SCALE_NV) {
                out += "MAD_SAT " + std::string(dst) + ".xyz, " +
                       std::string(dst) + ".xyz, sb" + std::to_string(i) +
                       ".x, sb" + std::to_string(i) + ".y;\\n";
            }
            break;
        }

        case kNearGL_DEPENDENT_AR_TEXTURE_2D_NV:
            out += "MOV coord.x, " + std::string(prevName) + ".a;\\n";
            out += "MOV coord.y, " + std::string(prevName) + ".r;\\n";
            out += "TEX " + std::string(dst) + ", coord, texture[" +
                   std::to_string(i) + "], 2D;\\n";
            break;

        case kNearGL_DEPENDENT_GB_TEXTURE_2D_NV:
            out += "MOV coord.x, " + std::string(prevName) + ".g;\\n";
            out += "MOV coord.y, " + std::string(prevName) + ".b;\\n";
            out += "TEX " + std::string(dst) + ", coord, texture[" +
                   std::to_string(i) + "], 2D;\\n";
            break;

        case kNearGL_DOT_PRODUCT_NV:
        case kNearGL_DOT_PRODUCT_TEXTURE_2D_NV:
        case kNearGL_DOT_PRODUCT_TEXTURE_3D_NV:
        case kNearGL_DOT_PRODUCT_TEXTURE_CUBE_MAP_NV:
        case kNearGL_DOT_PRODUCT_TEXTURE_RECTANGLE_NV: {
            if (st.dotMapping == kNearGL_EXPAND_NORMAL_NV)
                out += "MAD tmp, " + std::string(prevName) + ", 2.0, -1.0;\\n";
            else
                out += "MOV tmp, " + std::string(prevName) + ";\\n";
            out += "DP3 " + std::string(dst) + ".x, fragment.texcoord[" +
                   std::to_string(i) + "], tmp;\\n";
            out += "MOV " + std::string(dst) + ".yzw, " +
                   std::string(dst) + ".xxxx;\\n";

            if (st.op != kNearGL_DOT_PRODUCT_NV) {
                out += "MOV coord.xy, " + std::string(dst) + ".xx;\\n";
                out += "TEX " + std::string(dst) + ", coord, texture[" +
                       std::to_string(i) + "], " +
                       nearTextureShaderTexTarget(st.op) + ";\\n";
            }
            break;
        }

        case kNearGL_DOT_PRODUCT_DEPTH_REPLACE_NV:
            if (st.dotMapping == kNearGL_EXPAND_NORMAL_NV)
                out += "MAD tmp, " + std::string(prevName) + ", 2.0, -1.0;\\n";
            else
                out += "MOV tmp, " + std::string(prevName) + ";\\n";
            out += "DP3 " + std::string(dst) + ".x, fragment.texcoord[" +
                   std::to_string(i) + "], tmp;\\n";
            out += "MOV " + std::string(dst) + ".yzw, " +
                   std::string(dst) + ".xxxx;\\n";
            break;

        default:
            compatPakLog("GL TS EMU: unsupported operation 0x%x on unit %u -> passthrough",
                         (unsigned)st.op, i);
            out += "MOV " + std::string(dst) +
                   ", fragment.texcoord[" + std::to_string(i) + "];\\n";
            break;
        }

        const GLint envMode = g_nearTextureEnvModes[i];
        if (envMode == GL_REPLACE) {
            out += "MOV color, " + std::string(dst) + ";\\n";
        } else if (envMode == GL_ADD) {
            out += "ADD color, color, " + std::string(dst) + ";\\n";
        } else if (envMode == GL_DECAL) {
            out += "LRP color.xyz, " + std::string(dst) + ".a, " +
                   std::string(dst) + ", color;\\n";
        } else {
            out += "MUL color, color, " + std::string(dst) + ";\\n";
        }
    }

    if (!any)
        return false;

    out += "MOV result.color, color;\\n";
    out += "END\\n";
    return true;
}

using NearTsGenFn = void (*)(GLsizei, GLuint*);
using NearTsBindFn = void (*)(GLenum, GLuint);
using NearTsDeleteFn = void (*)(GLsizei, const GLuint*);
using NearTsStringFn = void (*)(GLenum, GLenum, GLsizei, const void*);

static bool nearTextureShaderCompileProgram() {
    std::string source;
    if (!nearTextureShaderBuildSource(source))
        return false;

    if (g_nearTextureShaderState.program &&
        source == g_nearTextureShaderState.source)
        return true;

    static NearTsGenFn gen =
        reinterpret_cast<NearTsGenFn>(eglGetProcAddress("glGenProgramsARB"));
    static NearTsBindFn bind =
        reinterpret_cast<NearTsBindFn>(eglGetProcAddress("glBindProgramARB"));
    static NearTsDeleteFn del =
        reinterpret_cast<NearTsDeleteFn>(eglGetProcAddress("glDeleteProgramsARB"));
    static NearTsStringFn stringFn =
        reinterpret_cast<NearTsStringFn>(eglGetProcAddress("glProgramStringARB"));

    if (!gen || !bind || !stringFn) {
        compatPakLog("GL TS EMU: ARB fragment program entry points unavailable");
        return false;
    }

    if (!g_nearTextureShaderState.program) {
        gen(1, &g_nearTextureShaderState.program);
        if (!g_nearTextureShaderState.program)
            return false;
    }

    bind(kNearGL_FRAGMENT_PROGRAM_ARB, g_nearTextureShaderState.program);
    stringFn(kNearGL_FRAGMENT_PROGRAM_ARB, kNearGL_PROGRAM_FORMAT_ASCII_ARB,
             (GLsizei)source.size(), source.c_str());

    const GLenum err = glGetError();
    GLint errorPos = -1;
    glGetIntegerv(kNearGL_PROGRAM_ERROR_POSITION_ARB, &errorPos);
    if (err != GL_NO_ERROR || errorPos >= 0) {
        compatPakLog("GL TS EMU: ARBfp compile FAILED err=0x%x error_pos=%d",
                     (unsigned)err, errorPos);
        if (del) {
            del(1, &g_nearTextureShaderState.program);
            g_nearTextureShaderState.program = 0;
        }
        return false;
    }

    g_nearTextureShaderState.source = source;
    compatPakLog("GL TS EMU: ARBfp program ready id=%u", (unsigned)g_nearTextureShaderState.program);
    return true;
}

static bool nearPrepareTextureShaderEmulation() {
    if (!nearTextureShaderHasStage())
        return false;

    if (g_nearTextureShaderDrawRestore.active)
        return false;

    GLint currentProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    if (currentProgram != 0)
        return false;

    const GLboolean fragmentEnabled = glIsEnabled(kNearGL_FRAGMENT_PROGRAM_ARB);
    if (fragmentEnabled)
        return false;

    if (!nearTextureShaderCompileProgram())
        return false;

    glGetIntegerv(kNearGL_FRAGMENT_PROGRAM_BINDING_ARB,
                  &g_nearTextureShaderDrawRestore.fragmentBinding);
    g_nearTextureShaderDrawRestore.fragmentEnabled = fragmentEnabled;
    g_nearTextureShaderDrawRestore.active = true;

    static NearTsBindFn bind =
        reinterpret_cast<NearTsBindFn>(eglGetProcAddress("glBindProgramARB"));
    if (!bind) {
        g_nearTextureShaderDrawRestore.active = false;
        return false;
    }

    bind(kNearGL_FRAGMENT_PROGRAM_ARB, g_nearTextureShaderState.program);
    glEnable(kNearGL_FRAGMENT_PROGRAM_ARB);
    return true;
}

static void nearFinishTextureShaderEmulation() {
    if (!g_nearTextureShaderDrawRestore.active)
        return;

    static NearTsBindFn bind =
        reinterpret_cast<NearTsBindFn>(eglGetProcAddress("glBindProgramARB"));
    if (bind)
        bind(kNearGL_FRAGMENT_PROGRAM_ARB,
             (GLuint)g_nearTextureShaderDrawRestore.fragmentBinding);

    if (g_nearTextureShaderDrawRestore.fragmentEnabled)
        glEnable(kNearGL_FRAGMENT_PROGRAM_ARB);
    else
        glDisable(kNearGL_FRAGMENT_PROGRAM_ARB);

    g_nearTextureShaderDrawRestore = {};
}

static GLboolean shim_glIsEnabledCompat(GLenum cap) {
    if (cap == kNearGL_TEXTURE_SHADER_NV)
        return g_nearTextureShaderState.enabled ? GL_TRUE : GL_FALSE;
    return glIsEnabled(cap);
}

static void shim_glTexEnviCompat(GLenum target, GLenum pname, GLint param) {
    if (target == kNearGL_TEXTURE_SHADER_NV) {
        const int unit = nearTextureShaderActiveUnit();
        if (unit < 0)
            return;

        NearTextureShaderUnitState& st = g_nearTextureShaderState.units[unit];
        switch (pname) {
        case kNearGL_SHADER_OPERATION_NV:
            st.op = (GLenum)param;
            compatPakLog("GL TS EMU: unit=%d op=0x%x", unit, (unsigned)st.op);
            break;
        case kNearGL_PREVIOUS_TEXTURE_INPUT_NV:
            st.previous = (int)param - (int)kNearGL_PREVIOUS_TEXTURE_INPUT_BASE;
            break;
        case kNearGL_RGBA_UNSIGNED_DOT_PRODUCT_MAPPING_NV:
            st.dotMapping = param;
            break;
        default:
            compatPakLog("GL TS EMU: unhandled TexEnvi pname=0x%x param=0x%x",
                         (unsigned)pname, (unsigned)param);
            break;
        }
        return;
    }

    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) {
        const int unit = nearTextureShaderActiveUnit();
        if (unit >= 0)
            g_nearTextureEnvModes[unit] = param;
    }

    glTexEnvi(target, pname, param);
}

static void shim_glTexEnvfCompat(GLenum target, GLenum pname, GLfloat param) {
    if (target == kNearGL_TEXTURE_SHADER_NV) {
        const int unit = nearTextureShaderActiveUnit();
        if (unit < 0)
            return;
        NearTextureShaderUnitState& st = g_nearTextureShaderState.units[unit];
        if (pname == kNearGL_OFFSET_TEXTURE_SCALE_NV)
            st.scale = param;
        else if (pname == kNearGL_OFFSET_TEXTURE_BIAS_NV)
            st.bias = param;
        else
            compatPakLog("GL TS EMU: unhandled TexEnvf pname=0x%x value=%g",
                         (unsigned)pname, (double)param);
        return;
    }
    glTexEnvf(target, pname, param);
}

static void shim_glTexEnvfvCompat(GLenum target, GLenum pname, const GLfloat* params) {
    if (target == kNearGL_TEXTURE_SHADER_NV) {
        const int unit = nearTextureShaderActiveUnit();
        if (unit < 0 || !params)
            return;
        NearTextureShaderUnitState& st = g_nearTextureShaderState.units[unit];
        if (pname == kNearGL_OFFSET_TEXTURE_MATRIX_NV)
            std::memcpy(st.offset, params, sizeof(st.offset));
        else if (pname == kNearGL_CULL_MODES_NV)
            std::memcpy(st.cullModes, params, sizeof(st.cullModes));
        else if (pname == kNearGL_CONST_EYE_NV)
            std::memcpy(st.constEye, params, sizeof(st.constEye));
        else
            compatPakLog("GL TS EMU: unhandled TexEnvfv pname=0x%x",
                         (unsigned)pname);
        return;
    }
    glTexEnvfv(target, pname, params);
}

static void shim_glTexEnvivCompat(GLenum target, GLenum pname, const GLint* params) {
    if (target == kNearGL_TEXTURE_SHADER_NV) {
        if (!params)
            return;
        shim_glTexEnviCompat(target, pname, *params);
        return;
    }
    glTexEnviv(target, pname, params);
}

static void shim_glEnableCompat(GLenum cap) {
    if (cap == kNearGL_TEXTURE_SHADER_NV) {
        g_nearTextureShaderState.enabled = true;
        compatPakLog("GL TS EMU: enable");
        return;
    }
    glEnable(cap);
    if (cap == kNearGL_VERTEX_PROGRAM_ARB || cap == kNearGL_FRAGMENT_PROGRAM_ARB)
        compatPakLog("GL ARB PROG: event=enable cap=0x%x(%s)",
                     (unsigned)cap, nearArbProgramTargetName(cap));
}

static void shim_glDisableCompat(GLenum cap) {
    if (cap == kNearGL_TEXTURE_SHADER_NV) {
        g_nearTextureShaderState.enabled = false;
        compatPakLog("GL TS EMU: disable");
        return;
    }
    glDisable(cap);
    if (cap == kNearGL_VERTEX_PROGRAM_ARB || cap == kNearGL_FRAGMENT_PROGRAM_ARB)
        compatPakLog("GL ARB PROG: event=disable cap=0x%x(%s)",
                     (unsigned)cap, nearArbProgramTargetName(cap));
}

static std::atomic<unsigned> g_waterDrawDiagCalls{0};

static void nearLogWaterDrawState() {
    if (g_waterDrawDiagCalls.load(std::memory_order_relaxed) >= 256)
        return;

    GLint maxUnits = 0;
    GLint program = 0;
    const GLint vertexProgram = (GLint)g_nearArbVertexProgramBinding;
    const GLint fragmentProgram = (GLint)g_nearArbFragmentProgramBinding;

    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);

    const GLboolean fragmentEnabled = glIsEnabled(0x8804); // GL_FRAGMENT_PROGRAM_ARB
    const GLboolean vertexEnabled = glIsEnabled(0x8620);   // GL_VERTEX_PROGRAM_ARB

    // First identify whether this draw actually uses one of the affected
    // rust/moss/normal-map textures. Only then spend the diagnostic budget.
    const GLint maxScanUnits = std::max(0, std::min(maxUnits, 4));
    GLint affectedUnits[4] = {};
    GLint affectedTextures[4] = {};
    std::string affectedNames[4];
    GLint affectedCount = 0;

    for (GLint unit = 0; unit < maxScanUnits; ++unit) {
        GLint texture = 0;
        glGetIntegeri_v(GL_TEXTURE_BINDING_2D, (GLuint)unit, &texture);
        if (!texture)
            continue;

        const std::string name = nearBoundTextureDiagName((GLuint)texture);
        if (name.empty())
            continue;

        affectedUnits[affectedCount] = unit;
        affectedTextures[affectedCount] = texture;
        affectedNames[affectedCount] = name;
        ++affectedCount;
    }

    if (!affectedCount)
        return;

    // Capture the fixed-function/client-array state exactly as it exists at the
    // affected draw. This is the important part for selective mesh displacement:
    // the position stream can be correct while the normal/tangent streams point
    // at the wrong VBO or stale offset.
    GLint vertexSize = 0, vertexType = 0, vertexStride = 0;
    GLint normalType = 0, normalStride = 0;
    GLint colorSize = 0, colorType = 0, colorStride = 0;
    void* vertexPtr = nullptr;
    void* normalPtr = nullptr;
    void* colorPtr = nullptr;

    glGetIntegerv(0x807A /* GL_VERTEX_ARRAY_SIZE */, &vertexSize);
    glGetIntegerv(0x807B /* GL_VERTEX_ARRAY_TYPE */, &vertexType);
    glGetIntegerv(0x807C /* GL_VERTEX_ARRAY_STRIDE */, &vertexStride);
    glGetIntegerv(0x807E /* GL_NORMAL_ARRAY_TYPE */, &normalType);
    glGetIntegerv(0x807F /* GL_NORMAL_ARRAY_STRIDE */, &normalStride);
    glGetIntegerv(0x8081 /* GL_COLOR_ARRAY_SIZE */, &colorSize);
    glGetIntegerv(0x8082 /* GL_COLOR_ARRAY_TYPE */, &colorType);
    glGetIntegerv(0x8083 /* GL_COLOR_ARRAY_STRIDE */, &colorStride);

    glGetPointerv(0x808E /* GL_VERTEX_ARRAY_POINTER */, &vertexPtr);
    glGetPointerv(0x808F /* GL_NORMAL_ARRAY_POINTER */, &normalPtr);
    glGetPointerv(0x8090 /* GL_COLOR_ARRAY_POINTER */, &colorPtr);

    GLint arrayBufferBefore = 0;
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &arrayBufferBefore);

    compatPakLog(
        "GL AFFECTED DRAW ARRAYS: program=%d vertex_prog=%d vertex_en=%d "
        "fragment_prog=%d fragment_en=%d array_vbo=%d "
        "pos=%p pos_size=%d pos_type=0x%x pos_stride=%d "
        "normal=%p normal_type=0x%x normal_stride=%d "
        "color=%p color_size=%d color_type=0x%x color_stride=%d",
        program, vertexProgram, vertexEnabled ? 1 : 0,
        fragmentProgram, fragmentEnabled ? 1 : 0,
        arrayBufferBefore,
        vertexPtr, vertexSize, (unsigned)vertexType, vertexStride,
        normalPtr, (unsigned)normalType, normalStride,
        colorPtr, colorSize, (unsigned)colorType, colorStride);

    GLint savedClientActive = (GLint)GL_TEXTURE0;
    glGetIntegerv(0x84E1 /* GL_CLIENT_ACTIVE_TEXTURE */, &savedClientActive);

    for (GLint i = 0; i < affectedCount; ++i) {
        const GLenum unitEnum = GL_TEXTURE0 + (GLenum)affectedUnits[i];
        glClientActiveTexture(unitEnum);

        GLint texArraySize = 0, texArrayType = 0, texArrayStride = 0;
        void* texPtr = nullptr;
        glGetIntegerv(0x8088 /* GL_TEXTURE_COORD_ARRAY_SIZE */, &texArraySize);
        glGetIntegerv(0x8089 /* GL_TEXTURE_COORD_ARRAY_TYPE */, &texArrayType);
        glGetIntegerv(0x808A /* GL_TEXTURE_COORD_ARRAY_STRIDE */, &texArrayStride);
        glGetPointerv(0x8092 /* GL_TEXTURE_COORD_ARRAY_POINTER */, &texPtr);

        GLint enabled = glIsEnabled(GL_TEXTURE_COORD_ARRAY) ? 1 : 0;
        compatPakLog(
            "GL AFFECTED TEXARRAY: unit=%d texture=%d name=%s enabled=%d "
            "ptr=%p size=%d type=0x%x stride=%d",
            affectedUnits[i], affectedTextures[i], affectedNames[i].c_str(),
            enabled, texPtr, texArraySize, (unsigned)texArrayType, texArrayStride);
    }

    glClientActiveTexture((GLenum)savedClientActive);

    g_waterDrawDiagCalls.fetch_add(1, std::memory_order_relaxed);

    // Sampler uniforms are only meaningful for a core/GLSL program.
    if (!program)
        return;

    GLint uniformCount = 0;
    glGetProgramiv((GLuint)program, GL_ACTIVE_UNIFORMS, &uniformCount);
    const GLint limit = std::max(0, std::min(uniformCount, 256));

    char uname[256] = {};
    for (GLint i = 0; i < limit; ++i) {
        GLsizei nameLen = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform((GLuint)program, (GLuint)i, sizeof(uname),
                           &nameLen, &size, &type, uname);
        if (type != GL_SAMPLER_2D)
            continue;

        uname[std::min<int>(nameLen, (int)sizeof(uname) - 1)] = '\0';
        const GLint location = glGetUniformLocation((GLuint)program, uname);
        if (location < 0)
            continue;

        GLint samplerUnit = -1;
        glGetUniformiv((GLuint)program, location, &samplerUnit);
        compatPakLog(
            "GL TEX SAMPLER: program=%d uniform=%s location=%d unit=%d",
            program, uname, location, samplerUnit);
    }
}

// This file is intentionally comprehensive but bounded: the first 4096
// TexImage2D calls are enough to capture the startup/level-load texture path
// without turning the diagnostic into another multi-hundred-MB log.
static std::atomic<unsigned> g_glTexImageShimCalls{0};
static std::atomic<unsigned> g_legacyTextureNormalizeCalls{0};

// gl4es exposes explicit compatibility controls for BGRA and 24-bit RGB
// textures. Far Cry's Android renderer relies on these legacy upload forms.
// Normalize them here so Zink/NVK receives a plain RGBA8 client image.
static bool nearIsBumpOrNormalTextureName(const std::string& name) {
    const std::string lower = asciiLower(name);
    return lower.find("_ddn") != std::string::npos ||
           lower.find("_ddp") != std::string::npos ||
           lower.find("normal") != std::string::npos ||
           lower.find("bump") != std::string::npos;
}

static bool nearPrepareLegacyRgbaPixels(GLsizei width, GLsizei height,
                                        GLenum format, GLenum type,
                                        const void* pixels, GLint unpackBuffer,
                                        void*& outPixels, GLenum& outFormat) {
    outPixels = nullptr;
    outFormat = format;

    if (!pixels || unpackBuffer != 0 || type != GL_UNSIGNED_BYTE ||
        width <= 0 || height <= 0)
        return false;

    unsigned components = 0;
    bool swapRB = false;
    if (format == GL_BGRA) {
        components = 4;
        swapRB = true;
    } else if (format == GL_BGR) {
        components = 3;
        swapRB = true;
    } else if (format == GL_RGBA) {
        components = 4;
    } else if (format == GL_RGB) {
        components = 3;
    } else {
        return false;
    }

    const uint64_t rowPixels = g_glUnpackRowLength > 0
        ? (uint64_t)g_glUnpackRowLength
        : (uint64_t)width;
    const uint64_t srcRowBytes = rowPixels * (uint64_t)components;
    const unsigned alignment =
        g_glUnpackAlignment > 0 ? (unsigned)g_glUnpackAlignment : 1u;
    const uint64_t paddedRow =
        ((srcRowBytes + alignment - 1u) / alignment) * alignment;
    const uint64_t skipOffset =
        (uint64_t)std::max<GLint>(g_glUnpackSkipRows, 0) * paddedRow +
        (uint64_t)std::max<GLint>(g_glUnpackSkipPixels, 0) * components;
    const uint64_t sourceSpan =
        skipOffset +
        (uint64_t)(height - 1) * paddedRow +
        (uint64_t)width * components;
    const uint64_t rgbaBytes =
        (uint64_t)width * (uint64_t)height * 4u;

    // Conversion is deliberately bounded; large/unknown uploads use the
    // original path rather than risking an oversized temporary allocation.
    if (!srcRowBytes || !paddedRow ||
        sourceSpan > 128ULL * 1024ULL * 1024ULL ||
        rgbaBytes > 128ULL * 1024ULL * 1024ULL ||
        rgbaBytes > (uint64_t)SIZE_MAX)
        return false;

    unsigned char* converted =
        reinterpret_cast<unsigned char*>(std::malloc((size_t)rgbaBytes));
    if (!converted)
        return false;

    const unsigned char* src =
        reinterpret_cast<const unsigned char*>(pixels) + skipOffset;

    for (GLsizei y = 0; y < height; ++y) {
        const unsigned char* srow =
            src + (uint64_t)y * paddedRow;
        unsigned char* drow =
            converted + (size_t)y * (size_t)width * 4u;
        for (GLsizei x = 0; x < width; ++x) {
            const unsigned char* s =
                srow + (size_t)x * components;
            unsigned char* d =
                drow + (size_t)x * 4u;
            if (swapRB) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
            } else {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
            d[3] = components == 4 ? s[3] : 255;
        }
    }

    outPixels = converted;
    outFormat = GL_RGBA;
    return true;
}

static void nearUploadUnpackReset(GLint alignment, GLint rowLength,
                                  GLint skipPixels, GLint skipRows) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
#if defined(GL_UNPACK_ROW_LENGTH)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
#endif
#if defined(GL_UNPACK_SKIP_PIXELS)
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
#endif
#if defined(GL_UNPACK_SKIP_ROWS)
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
#endif
}

static void shim_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void* pixels) {
    const unsigned traceIndex =
        g_glTexImageShimCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool traceThis = traceIndex <= 4096;

    std::string traceName = g_lastTexturePakName;
    const char* traceNameSource = "tls";
    if (traceName.empty()) {
        mutexLock(&g_textureDiagLock);
        traceName = g_lastTexturePakNameGlobal;
        mutexUnlock(&g_textureDiagLock);
        traceNameSource = "global";
    }
    if (traceName.empty()) {
        traceName = "<unknown>";
        traceNameSource = "none";
    }

    GLint activeTexture = 0;
    GLint textureBinding = 0;
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
    GLint cubeBinding = 0;
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
    GLint unpackBuffer = 0;
#else
    const GLint unpackBuffer = 0;
#endif
    const void* caller = nullptr;

    if (traceThis || (target == GL_TEXTURE_2D && nearIsWaterTextureDiagName(traceName))) {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        if (target == GL_TEXTURE_2D) {
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
        }
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
        else if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                 target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
            glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &cubeBinding);
        }
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
#endif
#if defined(__GNUC__) || defined(__clang__)
        caller = __builtin_return_address(0);
#endif

        const GLenum preerr = glGetError();

        compatLogFmt(
            "GL TEX SHIM ENTER[%u]: name=%s source=%s target=0x%x(%s) level=%d size=%dx%d "
            "border=%d internal=0x%x format=0x%x type=0x%x pixels=%p "
            "activeTex=0x%x tex2DBinding=%d"
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
            " cubeBinding=%d"
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
            " unpackBuffer=%d"
#endif
            " caller=%p preerr=0x%x",
            traceIndex, traceName.c_str(), traceNameSource,
            (unsigned)target, nearGlTexTargetName(target), level, width, height, border,
            (unsigned)(GLenum)internalformat,
            (unsigned)format, (unsigned)type, pixels, activeTexture, textureBinding
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
            , cubeBinding
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
            , unpackBuffer
#endif
            , caller, (unsigned)preerr);

        compatPakLog(
            "GL TEX SHIM ENTER[%u]: name=%s source=%s target=0x%x(%s) level=%d size=%dx%d "
            "border=%d internal=0x%x format=0x%x type=0x%x pixels=%p activeTex=0x%x "
            "tex2DBinding=%d"
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
            " cubeBinding=%d"
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
            " unpackBuffer=%d"
#endif
            " caller=%p preerr=0x%x",
            traceIndex, traceName.c_str(), traceNameSource, (unsigned)target,
            nearGlTexTargetName(target), level, width, height, border,
            (unsigned)(GLenum)internalformat, (unsigned)format, (unsigned)type,
            pixels, activeTexture, textureBinding
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
            , cubeBinding
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
            , unpackBuffer
#endif
            , caller, (unsigned)preerr);

        compatPakLog(
            "GL TEX UPLOAD[%u]: name=%s target=0x%x(%s) level=%d size=%dx%d border=%d "
            "internal=0x%x format=0x%x type=0x%x pixels=%p activeTex=0x%x "
            "tex2DBinding=%d"
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
            " cubeBinding=%d"
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
            " unpackBuffer=%d"
#endif
            " caller=%p preerr=0x%x",
            traceIndex, traceName.c_str(), (unsigned)target, nearGlTexTargetName(target),
            level, width, height, border, (unsigned)(GLenum)internalformat,
            (unsigned)format, (unsigned)type, pixels, activeTexture, textureBinding
#ifdef GL_TEXTURE_BINDING_CUBE_MAP
            , cubeBinding
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
            , unpackBuffer
#endif
            , caller, (unsigned)preerr);

        nearLogTextureBytes(traceName, width, height, format, type);
    }

    NearLegacyTexFormat legacyHilo;
    if (nearMapLegacyHiloFormat((GLenum)internalformat, format, type, legacyHilo)) {
        compatLogFmt(
            "GL COMPAT: %s texture -> internal=0x%x format=0x%x type=0x%x size=%dx%d",
            legacyHilo.label, (unsigned)legacyHilo.internalformat,
            (unsigned)legacyHilo.format, (unsigned)legacyHilo.type,
            width, height);

        glTexImage2D(target, level, (GLint)legacyHilo.internalformat,
                     width, height, border,
                     legacyHilo.format, legacyHilo.type, pixels);
        const GLenum err = glGetError();
        nearRememberTextureDiagName((GLuint)textureBinding, traceName);
        if (traceThis)
            compatPakLog(
                "GL TEX RESULT[%u]: name=%s path=%s internal=0x%x format=0x%x type=0x%x glerr=0x%x",
                traceIndex, traceName.c_str(), legacyHilo.label,
                (unsigned)legacyHilo.internalformat,
                (unsigned)legacyHilo.format,
                (unsigned)legacyHilo.type, (unsigned)err);
        return;
    }

    // DSDT_MAG fallback from the Android renderer stores three signed
    // normalized channels in a 4-byte/pixel buffer. The original Android
    // BuildMips path uses GL_RGB8 + GL_RGBA + GL_BYTE when NV_texture_shader
    // is unavailable. Map that exact pattern to RGB8_SNORM so shader reads
    // retain the original [-1,1] semantics without changing the 4-byte stride.
    if (isNearDdsDdtTextureName(traceName) &&
        (GLenum)internalformat == GL_RGB8 &&
        format == GL_RGBA &&
        type == GL_BYTE &&
        pixels) {
        // Keep routine DSDT compatibility silent; GL errors are logged below.

        glTexImage2D(target, level, (GLint)GL_RGB8_SNORM,
                     width, height, border,
                     GL_RGBA, GL_BYTE, pixels);
        const GLenum uploadError = glGetError();
        if (uploadError != GL_NO_ERROR)
            compatLogFmt("GL COMPAT ERROR: DSDT byte texture %s glerr=0x%x",
                         traceName.c_str(), (unsigned)uploadError);
        nearRememberTextureDiagName((GLuint)textureBinding, traceName);
        if (traceThis) {
            compatPakLog(
                "GL TEX RESULT[%u]: name=%s path=DSDT-BYTE "
                "mapped_internal=0x%x mapped_format=0x%x original_internal=0x%x "
                "original_format=0x%x type=0x%x glerr=0x%x",
                traceIndex, traceName.c_str(),
                (unsigned)GL_RGB8_SNORM, (unsigned)GL_RGBA,
                (unsigned)(GLenum)internalformat, (unsigned)format,
                (unsigned)type, (unsigned)uploadError);
        }
        nearVerifyUploadedTexture(
            traceName, target, level, width, height,
            GL_RGBA, GL_BYTE, pixels, unpackBuffer, uploadError);
        return;
    }

    if (type == GL_FLOAT &&
        (isNearDsdtFormat(format) || isNearDsdtFormat((GLenum)internalformat))) {
        const bool mag = (format == kNearGL_DSDT_MAG_NV ||
                          (GLenum)internalformat == kNearGL_DSDT_MAG_NV);
        const GLenum mappedInternal = mag ? GL_RGB32F : GL_RG32F;
        const GLenum mappedFormat = mag ? GL_RGB : GL_RG;

        // Keep routine DSDT compatibility silent; GL errors are logged below.

        glTexImage2D(target, level, (GLint)mappedInternal, width, height, border,
                     mappedFormat, type, pixels);
        const GLenum uploadError = glGetError();
        if (uploadError != GL_NO_ERROR)
            compatLogFmt("GL COMPAT ERROR: DSDT float texture %s glerr=0x%x",
                         traceName.c_str(), (unsigned)uploadError);
        nearRememberTextureDiagName((GLuint)textureBinding, traceName);
        if (traceThis) {
            compatPakLog(
                "GL TEX RESULT[%u]: name=%s path=DSDT mapped_internal=0x%x "
                "mapped_format=0x%x original_internal=0x%x original_format=0x%x "
                "type=0x%x glerr=0x%x",
                traceIndex, traceName.c_str(), (unsigned)mappedInternal,
                (unsigned)mappedFormat, (unsigned)(GLenum)internalformat,
                (unsigned)format, (unsigned)type, (unsigned)uploadError);
        }
        nearVerifyUploadedTexture(traceName, target, level, width, height,
                                  mappedFormat, type, pixels, unpackBuffer,
                                  uploadError);
        return;
    }

    void* convertedPixels = nullptr;
    GLenum uploadError = GL_NO_ERROR;
    bool normalized = false;

    const bool legacy8BitColorTarget =
        (GLenum)internalformat == GL_RGB ||
        (GLenum)internalformat == GL_RGB8 ||
        (GLenum)internalformat == GL_RGBA ||
        (GLenum)internalformat == GL_RGBA8;
    // CryEngine's bump/normal-map data is intentionally supplied as BGRA.
    // GenerateNormalMap() writes Z/Y/X/A into that buffer and the Android
    // renderer relies on GL_BGRA to expose it as R/G/B/A. Do NOT run the
    // generic RB swap on those textures or the normal's X/Z axes are exchanged.
    const bool channelSensitive = nearIsBumpOrNormalTextureName(traceName);
    const bool legacyColorFormat =
        legacy8BitColorTarget &&
        !channelSensitive &&
        (format == GL_RGB || format == GL_BGR || format == GL_BGRA);

    if (type == GL_UNSIGNED_BYTE && legacyColorFormat &&
        unpackBuffer == 0 && width > 0 && height > 0) {
        const GLint savedAlignment = g_glUnpackAlignment;
        const GLint savedRowLength = g_glUnpackRowLength;
        const GLint savedSkipPixels = g_glUnpackSkipPixels;
        const GLint savedSkipRows = g_glUnpackSkipRows;

        GLenum mappedFormat = format;
        if (nearPrepareLegacyRgbaPixels(
                width, height, format, type, pixels, unpackBuffer,
                convertedPixels, mappedFormat)) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#if defined(GL_UNPACK_ROW_LENGTH)
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#if defined(GL_UNPACK_SKIP_PIXELS)
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#if defined(GL_UNPACK_SKIP_ROWS)
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif

            glTexImage2D(target, level, (GLint)GL_RGBA8,
                         width, height, border,
                         mappedFormat, GL_UNSIGNED_BYTE, convertedPixels);
            uploadError = glGetError();
            nearUploadUnpackReset(savedAlignment, savedRowLength,
                                  savedSkipPixels, savedSkipRows);
            normalized = true;
        }
    }

    if (convertedPixels) {
        g_legacyTextureNormalizeCalls.fetch_add(1, std::memory_order_relaxed);
        if (uploadError != GL_NO_ERROR)
            compatLogFmt("GL COMPAT ERROR: legacy texture normalization name=%s glerr=0x%x",
                         traceName.c_str(), (unsigned)uploadError);
        std::free(convertedPixels);
        convertedPixels = nullptr;
    }

    if (!normalized) {
        glTexImage2D(target, level, internalformat, width, height, border,
                     format, type, pixels);
        uploadError = glGetError();
    }

    nearRememberTextureDiagName((GLuint)textureBinding, traceName);
    if (traceThis) {
        compatPakLog(
            "GL TEX RESULT[%u]: name=%s path=%s internal=0x%x format=0x%x "
            "type=0x%x glerr=0x%x",
            traceIndex, traceName.c_str(),
            normalized ? "legacy-rgba8" : "normal",
            (unsigned)(GLenum)(normalized ? GL_RGBA8 : internalformat),
            (unsigned)(GLenum)(normalized ? GL_RGBA : format),
            (unsigned)(GLenum)type, (unsigned)uploadError);
    }
    nearVerifyUploadedTexture(traceName, target, level, width, height,
                              normalized ? GL_RGBA : format, type,
                              normalized ? nullptr : pixels, unpackBuffer,
                              uploadError);
}

static bool nearMapLegacySubImageFormat(GLenum format, GLenum type,
                                          GLenum& mappedFormat, GLenum& mappedType) {
    mappedFormat = format;
    mappedType = type;

    // The Android CryEngine source has a few historical call sites that pass
    // the texture *internal format* in the format argument of glTexSubImage2D,
    // e.g. GL_RGBA8. Desktop OpenGL rejects GL_RGBA8 there; the corresponding
    // client pixel format is GL_RGBA.
    if (type == GL_UNSIGNED_BYTE) {
        if (format == GL_RGBA8) {
            mappedFormat = GL_RGBA;
            return true;
        }
        if (format == GL_RGB8) {
            mappedFormat = GL_RGB;
            return true;
        }
    }

    // Dynamic/static DSDT byte uploads use the legacy internal-format token
    // as the client format too. Preserve the signed two's-complement bytes.
    if ((type == GL_UNSIGNED_BYTE || type == GL_BYTE) &&
        isNearDsdtFormat(format)) {
        mappedFormat = (format == kNearGL_DSDT_MAG_NV) ? GL_RGB : GL_RG;
        // DSDT byte data is stored as two's-complement signed deltas even at
        // the Android call sites that use GL_UNSIGNED_BYTE. The legacy NV
        // format interprets those bytes as signed offset components; replay
        // the same bit pattern through the core signed type.
        mappedType = GL_BYTE;
        return true;
    }

    // CREOcean::UpdateTexture() uses this exact legacy call:
    //   glTexSubImage2D(..., GL_DSDT_NV, GL_FLOAT, data)
    // The matching glTexImage2D path maps GL_DSDT_NV to RG32F, so the
    // subsequent sub-upload must use the ordinary two-component float format.
    if (type == GL_FLOAT && isNearDsdtFormat(format)) {
        mappedFormat = (format == kNearGL_DSDT_MAG_NV) ? GL_RGB : GL_RG;
        mappedType = GL_FLOAT;
        return true;
    }

    return false;
}

static void shim_glTexSubImage2D(GLenum target, GLint level,
                                 GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height,
                                 GLenum format, GLenum type,
                                 const void* pixels) {
    if (isNearLegacyHiloFormat(format)) {
        NearLegacyTexFormat legacyHilo;
        GLint targetInternal = 0;
        glGetTexLevelParameteriv(target, level,
                                 GL_TEXTURE_INTERNAL_FORMAT, &targetInternal);
        if (nearMapLegacyHiloFormat((GLenum)targetInternal, format, type, legacyHilo) ||
            nearMapLegacyHiloFormat(
                (targetInternal == (GLint)GL_RG8_SNORM ||
                 targetInternal == (GLint)GL_RG16_SNORM)
                    ? kNearGL_SIGNED_HILO_NV
                    : kNearGL_HILO_NV,
                format, type, legacyHilo)) {
            compatLogFmt(
                "GL COMPAT: %s subtexture -> format=0x%x type=0x%x size=%dx%d",
                legacyHilo.label, (unsigned)legacyHilo.format,
                (unsigned)legacyHilo.type, width, height);
            glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                            legacyHilo.format, legacyHilo.type, pixels);
            return;
        }
    }

    GLenum mappedLegacyFormat = format;
    GLenum mappedLegacyType = type;
    if (nearMapLegacySubImageFormat(
            format, type, mappedLegacyFormat, mappedLegacyType)) {
        compatLogFmt(
            "GL COMPAT: legacy subimage format 0x%x/0x%x -> 0x%x/0x%x size=%dx%d",
            (unsigned)format, (unsigned)type,
            (unsigned)mappedLegacyFormat, (unsigned)mappedLegacyType,
            width, height);
        glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                        mappedLegacyFormat, mappedLegacyType, pixels);
        return;
    }

    if (type == GL_UNSIGNED_BYTE &&
        (format == GL_RGB || format == GL_BGR || format == GL_BGRA) &&
        pixels) {
        GLint unpackBuffer = 0;
#ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
#endif
        GLint targetInternal = 0;
        glGetTexLevelParameteriv(target, level,
                                 GL_TEXTURE_INTERNAL_FORMAT, &targetInternal);
        const bool legacySubTarget =
            targetInternal == (GLint)GL_RGB ||
            targetInternal == (GLint)GL_RGB8 ||
            targetInternal == (GLint)GL_RGBA ||
            targetInternal == (GLint)GL_RGBA8;

        void* convertedPixels = nullptr;
        GLenum mappedFormat = format;
        if (legacySubTarget &&
            nearPrepareLegacyRgbaPixels(
                width, height, format, type, pixels, unpackBuffer,
                convertedPixels, mappedFormat)) {
            const GLint savedAlignment = g_glUnpackAlignment;
            const GLint savedRowLength = g_glUnpackRowLength;
            const GLint savedSkipPixels = g_glUnpackSkipPixels;
            const GLint savedSkipRows = g_glUnpackSkipRows;

            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#if defined(GL_UNPACK_ROW_LENGTH)
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#if defined(GL_UNPACK_SKIP_PIXELS)
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#if defined(GL_UNPACK_SKIP_ROWS)
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif

            glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                            mappedFormat, GL_UNSIGNED_BYTE, convertedPixels);
            const GLenum err = glGetError();

            nearUploadUnpackReset(savedAlignment, savedRowLength,
                                  savedSkipPixels, savedSkipRows);
            std::free(convertedPixels);

            const unsigned normalizeIndex =
                g_legacyTextureNormalizeCalls.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            if (normalizeIndex <= 128) {
                compatLogFmt(
                    "GL COMPAT: legacy subtexture normalized[%u] "
                    "original_format=0x%x mapped_format=0x%x size=%dx%d err=0x%x",
                    normalizeIndex, (unsigned)format, (unsigned)mappedFormat,
                    width, height, (unsigned)err);
            }
            return;
        }
    }

    glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                    format, type, pixels);
}

// NV_vertex_program vertex-attrib compatibility.
// Far Cry's Android ocean renderer feeds its generated vertex data through
// glVertexAttribPointerNV(0/1/2) and enables GL_VERTEX_ATTRIB_ARRAY{0,1,2}_NV.
// Mesa/Zink exposes the same storage through core generic vertex attributes.
static bool nearNvVertexAttribArray(GLenum array, GLuint& index) {
    if (array < 0x8650 || array > 0x865F)
        return false;
    index = (GLuint)(array - 0x8650); // GL_VERTEX_ATTRIB_ARRAY0_NV .. 15_NV
    return true;
}

// Legacy client-array pointers are real process addresses on Switch. A VBO
// offset is a small integer (0, 0x324, ...), while a client pointer is a
// normal high virtual address. Keep the distinction explicit when translating
// old desktop-OpenGL calls to the modern core entry points.
static bool nearLooksLikeClientPointer(const void* pointer) {
    if (!pointer)
        return false;

    const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    // Switch guest/compatibility heap pointers are well above this range;
    // practical VBO offsets used by Far Cry are tiny.
    return value > 0x00100000u;
}

// NV_vertex_program specifies VertexAttribPointerNV as an entirely client-side
// command. It must NOT inherit the current ARRAY_BUFFER binding. The Android
// renderer passes pointers into CPU-generated arrays here, so bind ARRAY_BUFFER=0
// while recording the generic attribute state, then restore the previous binding.
static void shim_glVertexAttribPointerNV(GLuint index, GLint fsize, GLenum type,
                                         GLsizei stride, const void* pointer) {
    if (index >= 16 || fsize <= 0 || fsize > 4) {
        compatPakLog("GL NV VERTEX ATTR: rejected index=%u size=%d",
                     (unsigned)index, (int)fsize);
        return;
    }

    GLint savedArrayBuffer = 0;
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &savedArrayBuffer);

    if (savedArrayBuffer != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(index, fsize, type, GL_FALSE, stride, pointer);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedArrayBuffer);
        compatLogFmt(
            "GL NV VERTEX ATTR CLIENT FIX: index=%u size=%d type=0x%x stride=%d ptr=%p savedVbo=%d",
            (unsigned)index, (int)fsize, (unsigned)type, (int)stride,
            pointer, (int)savedArrayBuffer);
    } else {
        glVertexAttribPointer(index, fsize, type, GL_FALSE, stride, pointer);
    }

    compatPakLog("GL NV VERTEX ATTR: pointer index=%u size=%d type=0x%x stride=%d ptr=%p",
                 (unsigned)index, (int)fsize, (unsigned)type, (int)stride, pointer);
}

static void shim_glEnableClientStateCompat(GLenum array) {
    GLuint index = 0;
    if (nearNvVertexAttribArray(array, index)) {
        glEnableVertexAttribArray(index);
        compatPakLog("GL NV VERTEX ATTR: enable index=%u", (unsigned)index);
        return;
    }
    glEnableClientState(array);
}

static void shim_glDisableClientStateCompat(GLenum array) {
    GLuint index = 0;
    if (nearNvVertexAttribArray(array, index)) {
        glDisableVertexAttribArray(index);
        compatPakLog("GL NV VERTEX ATTR: disable index=%u", (unsigned)index);
        return;
    }
    glDisableClientState(array);
}

static unsigned g_nearVertexPointerDiagCalls = 0;

static void shim_glVertexPointerCompat(GLint size, GLenum type, GLsizei stride,
                                        const void* pointer) {
    GLint buffer = 0;
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &buffer);

    // Far Cry has both true VBO-offset calls and legacy client-array calls.
    // If a real client pointer arrives while some unrelated VBO is still bound,
    // core OpenGL/Zink would interpret that address as a giant byte offset into
    // the VBO and the affected mesh would acquire bogus vertex coordinates.
    if (buffer != 0 && nearLooksLikeClientPointer(pointer)) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexPointer(size, type, stride, pointer);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
        compatLogFmt(
            "GL VERTEX POINTER CLIENT FIX: size=%d type=0x%x stride=%d ptr=%p savedVbo=%d",
            (int)size, (unsigned)type, (int)stride, pointer, (int)buffer);
    } else {
        if (buffer != 0 && (g_nearVertexPointerDiagCalls < 16)) {
            compatLogFmt(
                "GL VERTEX POINTER[%u]: buffer=%d size=%d type=0x%x stride=%d offset=0x%llx",
                g_nearVertexPointerDiagCalls + 1, (int)buffer, (int)size,
                (unsigned)type, (int)stride,
                (unsigned long long)(uintptr_t)pointer);
        }
        glVertexPointer(size, type, stride, pointer);
    }

    g_nearVertexPointerState.size = size;
    g_nearVertexPointerState.type = type;
    g_nearVertexPointerState.stride = stride;
    g_nearVertexPointerState.pointer = reinterpret_cast<uintptr_t>(pointer);
    g_nearVertexPointerState.buffer = buffer;
    g_nearVertexPointerState.clientPointer =
        (buffer == 0) || nearLooksLikeClientPointer(pointer);

    ++g_nearVertexPointerDiagCalls;
}

static void shim_glActiveStencilFaceEXT(GLenum) {}
static void shim_glBindBufferARB(GLenum target, GLuint buffer) { glBindBuffer(target, buffer); }

// Switch-only compatibility for Far Cry's software vertex deformation path.
// The Android renderer maps VBO storage with glMapBufferARB(), modifies the
// vertex coordinates in-place, then releases it with glUnmapBufferARB().
// Route the legacy ARB names to Mesa/Zink's core map/unmap entry points.
static unsigned g_nearArrayMapDiagCalls = 0;
static unsigned g_nearArrayUnmapDiagCalls = 0;
static unsigned g_nearIndexMapDiagCalls = 0;
static unsigned g_nearIndexUnmapDiagCalls = 0;

static void* shim_glMapBufferARB(GLenum target, GLenum access) {
    // devkitA64 headers expose glMapBufferRange(), but not desktop glMapBuffer().
    // Translate the legacy ARB access enum and map the entire currently bound
    // buffer so the Android vertex-deformation code can edit it in place.
    GLint size = 0;
    glGetBufferParameteriv(target, 0x8764 /* GL_BUFFER_SIZE */, &size);

    GLbitfield flags = 0;
    if (access == 0x88B8 /* GL_READ_ONLY_ARB */)
        flags = GL_MAP_READ_BIT;
    else if (access == 0x88B9 /* GL_WRITE_ONLY_ARB */)
        // Far Cry's deformation code reads the old XYZ values before writing
        // the deformed coordinates, even though the legacy API requests
        // GL_WRITE_ONLY_ARB. Keep the mapping readable on Switch/Zink so the
        // read-modify-write operation sees the original VBO contents.
        flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    else if (access == 0x88BA /* GL_READ_WRITE_ARB */)
        flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    else {
        compatPakLog("GL VERTEX BUFFER: glMapBufferARB unsupported access=0x%x",
                     (unsigned)access);
        return nullptr;
    }

    if (size <= 0) {
        compatPakLog("GL VERTEX BUFFER: glMapBufferARB target=0x%x size=%d -> NULL",
                     (unsigned)target, (int)size);
        return nullptr;
    }

    void* p = glMapBufferRange(target, 0, (GLsizeiptr)size, flags);
    const bool vertexTarget = (target == 0x8892 /* GL_ARRAY_BUFFER */);
    const bool indexTarget = (target == 0x8893 /* GL_ELEMENT_ARRAY_BUFFER */);

    if (vertexTarget) {
        if (g_nearArrayMapDiagCalls < 32 || !p) {
            GLint buffer = 0;
            glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &buffer);
            compatLogFmt(
                "GL DEFORM MAP[%u]: target=ARRAY_BUFFER buffer=%d access=0x%x size=%d flags=0x%x ptr=%p",
                g_nearArrayMapDiagCalls + 1, (int)buffer, (unsigned)access,
                (int)size, (unsigned)flags, p);

            // The map path is where the large/merged VBOs are populated. Dump
            // the first 64 bytes as raw 32-bit words so we can inspect the
            // actual vertex representation without assuming a format here.
            if (p && (g_nearArrayMapDiagCalls < 16 || buffer >= 500)) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(p);
                const size_t words = std::min<size_t>((size_t)size / sizeof(uint32_t), 16);
                compatLogFmt(
                    "GL VERTEX MAP WORDS: buffer=%d words=%u "
                    "%08x %08x %08x %08x %08x %08x %08x %08x "
                    "%08x %08x %08x %08x %08x %08x %08x %08x",
                    (int)buffer, (unsigned)words,
                    words > 0 ? (unsigned)w[0] : 0u,
                    words > 1 ? (unsigned)w[1] : 0u,
                    words > 2 ? (unsigned)w[2] : 0u,
                    words > 3 ? (unsigned)w[3] : 0u,
                    words > 4 ? (unsigned)w[4] : 0u,
                    words > 5 ? (unsigned)w[5] : 0u,
                    words > 6 ? (unsigned)w[6] : 0u,
                    words > 7 ? (unsigned)w[7] : 0u,
                    words > 8 ? (unsigned)w[8] : 0u,
                    words > 9 ? (unsigned)w[9] : 0u,
                    words > 10 ? (unsigned)w[10] : 0u,
                    words > 11 ? (unsigned)w[11] : 0u,
                    words > 12 ? (unsigned)w[12] : 0u,
                    words > 13 ? (unsigned)w[13] : 0u,
                    words > 14 ? (unsigned)w[14] : 0u,
                    words > 15 ? (unsigned)w[15] : 0u);
            }
        }
        ++g_nearArrayMapDiagCalls;
    } else if (indexTarget) {
        if (g_nearIndexMapDiagCalls < 16 || !p) {
            GLint buffer = 0;
            glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &buffer);
            compatLogFmt(
                "GL INDEX MAP[%u]: buffer=%d access=0x%x size=%d flags=0x%x ptr=%p",
                g_nearIndexMapDiagCalls + 1, (int)buffer, (unsigned)access,
                (int)size, (unsigned)flags, p);

            if (p && (g_nearIndexMapDiagCalls < 8 || buffer >= 500)) {
                const uint16_t* idx = reinterpret_cast<const uint16_t*>(p);
                const size_t count = std::min<size_t>((size_t)size / sizeof(uint16_t), 2048);
                uint16_t minIndex = 0xFFFF;
                uint16_t maxIndex = 0;
                for (size_t i = 0; i < count; ++i) {
                    minIndex = std::min(minIndex, idx[i]);
                    maxIndex = std::max(maxIndex, idx[i]);
                }
                compatLogFmt(
                    "GL INDEX MAP DATA: buffer=%d count16=%u min=%u max=%u "
                    "first=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                    (int)buffer, (unsigned)count, (unsigned)minIndex,
                    (unsigned)maxIndex,
                    count > 0 ? (unsigned)idx[0] : 0u,
                    count > 1 ? (unsigned)idx[1] : 0u,
                    count > 2 ? (unsigned)idx[2] : 0u,
                    count > 3 ? (unsigned)idx[3] : 0u,
                    count > 4 ? (unsigned)idx[4] : 0u,
                    count > 5 ? (unsigned)idx[5] : 0u,
                    count > 6 ? (unsigned)idx[6] : 0u,
                    count > 7 ? (unsigned)idx[7] : 0u,
                    count > 8 ? (unsigned)idx[8] : 0u,
                    count > 9 ? (unsigned)idx[9] : 0u,
                    count > 10 ? (unsigned)idx[10] : 0u,
                    count > 11 ? (unsigned)idx[11] : 0u);
            }
        }
        ++g_nearIndexMapDiagCalls;
    }
    return p;
}
static GLboolean shim_glUnmapBufferARB(GLenum target) {
    const GLboolean ok = glUnmapBuffer(target);
    const bool vertexTarget = (target == 0x8892 /* GL_ARRAY_BUFFER */);
    const bool indexTarget = (target == 0x8893 /* GL_ELEMENT_ARRAY_BUFFER */);

    if (vertexTarget) {
        if (g_nearArrayUnmapDiagCalls < 8 || !ok) {
            GLint buffer = 0;
            glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &buffer);
            compatLogFmt("GL DEFORM UNMAP[%u]: target=ARRAY_BUFFER buffer=%d ok=%d",
                         g_nearArrayUnmapDiagCalls + 1, (int)buffer, (int)ok);
        }
        ++g_nearArrayUnmapDiagCalls;
    } else if (indexTarget) {
        if (g_nearIndexUnmapDiagCalls < 2 || !ok) {
            GLint buffer = 0;
            glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &buffer);
            compatLogFmt("GL INDEX UNMAP[%u]: buffer=%d ok=%d",
                         g_nearIndexUnmapDiagCalls + 1, (int)buffer, (int)ok);
        }
        ++g_nearIndexUnmapDiagCalls;
    }
    return ok;
}
static unsigned g_nearBufferDataDiagCalls = 0;
static unsigned g_nearVertexUploadDiagCalls = 0;
static unsigned g_nearIndexUploadDiagCalls = 0;

static void shim_glBufferDataARB(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    glBufferData(target, size, data, usage);

    const bool vertexTarget = (target == 0x8892 /* GL_ARRAY_BUFFER */);
    const bool indexTarget = (target == 0x8893 /* GL_ELEMENT_ARRAY_BUFFER */);
    if ((vertexTarget || indexTarget) && g_nearBufferDataDiagCalls < 32) {
        GLint buffer = 0;
        glGetIntegerv(vertexTarget ? 0x8894 /* GL_ARRAY_BUFFER_BINDING */
                                   : 0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */,
                      &buffer);
        compatLogFmt(
            "GL BUFFER DATA[%u]: target=0x%x buffer=%d size=%lld usage=0x%x initialData=%p",
            g_nearBufferDataDiagCalls + 1, (unsigned)target, (int)buffer,
            (long long)size, (unsigned)usage, data);
        ++g_nearBufferDataDiagCalls;
    }
}
static void shim_glGenBuffersARB(GLsizei n, GLuint* buffers) {
    glGenBuffers(n, buffers);
}
static void shim_glBufferSubDataARB(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    glBufferSubData(target, offset, size, data);

    const bool vertexTarget = (target == 0x8892 /* GL_ARRAY_BUFFER */);
    const bool indexTarget = (target == 0x8893 /* GL_ELEMENT_ARRAY_BUFFER */);

    if (vertexTarget && data && size >= 12 && g_nearVertexUploadDiagCalls < 64) {
        GLint buffer = 0;
        GLint totalSize = 0;
        glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &buffer);
        glGetBufferParameteriv(GL_ARRAY_BUFFER, 0x8764 /* GL_BUFFER_SIZE */, &totalSize);

        const float* p = reinterpret_cast<const float*>(data);
        const bool hasSecond28 = size >= 40;
        compatLogFmt(
            "GL VERTEX UPLOAD[%u]: buffer=%d offset=%lld size=%lld total=%d "
            "XYZ0=%g,%g,%g RAW12=%g,%g,%g%s",
            g_nearVertexUploadDiagCalls + 1, (int)buffer,
            (long long)offset, (long long)size, (int)totalSize,
            (double)p[0], (double)p[1], (double)p[2],
            size >= 24 ? (double)p[3] : 0.0,
            size >= 24 ? (double)p[4] : 0.0,
            size >= 24 ? (double)p[5] : 0.0,
            hasSecond28 ? " SECOND28=" : "");
        if (hasSecond28) {
            const float* p28 = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(data) + 28);
            compatLogFmt(
                "GL VERTEX UPLOAD SECOND28: buffer=%d XYZ=%g,%g,%g",
                (int)buffer, (double)p28[0], (double)p28[1], (double)p28[2]);
        }
        ++g_nearVertexUploadDiagCalls;
    }

    if (indexTarget && data && size >= 2 && g_nearIndexUploadDiagCalls < 16) {
        GLint buffer = 0;
        GLint totalSize = 0;
        glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &buffer);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, 0x8764 /* GL_BUFFER_SIZE */, &totalSize);

        const size_t count = std::min<size_t>((size_t)size / sizeof(uint16_t), 1024);
        const uint16_t* p = reinterpret_cast<const uint16_t*>(data);
        uint16_t minIndex = 0xFFFF;
        uint16_t maxIndex = 0;
        for (size_t i = 0; i < count; ++i) {
            minIndex = std::min(minIndex, p[i]);
            maxIndex = std::max(maxIndex, p[i]);
        }

        compatLogFmt(
            "GL INDEX UPLOAD[%u]: buffer=%d offset=%lld size=%lld total=%d "
            "count16=%u min=%u max=%u first=%u,%u,%u,%u,%u,%u,%u,%u",
            g_nearIndexUploadDiagCalls + 1, (int)buffer,
            (long long)offset, (long long)size, (int)totalSize,
            (unsigned)count, (unsigned)minIndex, (unsigned)maxIndex,
            count > 0 ? (unsigned)p[0] : 0u,
            count > 1 ? (unsigned)p[1] : 0u,
            count > 2 ? (unsigned)p[2] : 0u,
            count > 3 ? (unsigned)p[3] : 0u,
            count > 4 ? (unsigned)p[4] : 0u,
            count > 5 ? (unsigned)p[5] : 0u,
            count > 6 ? (unsigned)p[6] : 0u,
            count > 7 ? (unsigned)p[7] : 0u);
        ++g_nearIndexUploadDiagCalls;
    }
}
static void shim_glColorTableEXT(GLenum target, GLenum internalformat, GLsizei width,
                                 GLenum format, GLenum type, const void* table) {
    glColorTable(target, internalformat, width, format, type, table);
}
static void shim_glCompressedTexImage2DARB(GLenum target, GLint level, GLenum internalformat,
                                           GLsizei width, GLsizei height, GLint border,
                                           GLsizei imageSize, const void* data) {
    if (isNear3dcCompressedFormat(internalformat)) {
        glCompressedTexImage2D(
            target, level, GL_COMPRESSED_RG_RGTC2,
            width, height, border, imageSize, data);
        const GLenum err = glGetError();
        if (err == GL_NO_ERROR)
            nearApply3dcSwizzle(target);
        compatLogFmt(
            "GL COMPAT: 3DC compressed texture -> RGTC2 size=%dx%d bytes=%d err=0x%x",
            width, height, (int)imageSize, (unsigned)err);
        return;
    }

    glCompressedTexImage2D(
        target, level, internalformat, width, height, border, imageSize, data);
}

static void shim_glCompressedTexSubImage2DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                              GLsizei width, GLsizei height, GLenum format,
                                              GLsizei imageSize, const void* data) {
    if (isNear3dcCompressedFormat(format)) {
        glCompressedTexSubImage2D(
            target, level, xoffset, yoffset, width, height,
            GL_COMPRESSED_RG_RGTC2, imageSize, data);
        const GLenum err = glGetError();
        if (err == GL_NO_ERROR)
            nearApply3dcSwizzle(target);
        compatLogFmt(
            "GL COMPAT: 3DC compressed subtexture -> RGTC2 size=%dx%d bytes=%d err=0x%x",
            width, height, (int)imageSize, (unsigned)err);
        return;
    }

    glCompressedTexSubImage2D(
        target, level, xoffset, yoffset, width, height, format, imageSize, data);
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

static std::atomic<unsigned> g_arbProgramDiagCalls{0};

static const char* nearArbProgramTargetName(GLenum target) {
    if (target == 0x8620) return "VERTEX_PROGRAM_ARB";
    if (target == 0x8804) return "FRAGMENT_PROGRAM_ARB";
    return "OTHER";
}

static GLenum nearArbProgramBindingEnum(GLenum target) {
    if (target == 0x8620) return 0x864A; // GL_VERTEX_PROGRAM_BINDING_ARB
    if (target == 0x8804) return 0x8873; // GL_FRAGMENT_PROGRAM_BINDING_ARB
    return 0;
}

static void nearLogArbProgramState(const char* event, GLenum target, GLuint requested) {
    const unsigned n = g_arbProgramDiagCalls.fetch_add(1, std::memory_order_relaxed);
    if (n >= 512)
        return;

    GLint binding = -1;
    if (target == 0x8620)
        binding = (GLint)g_nearArbVertexProgramBinding;
    else if (target == 0x8804)
        binding = (GLint)g_nearArbFragmentProgramBinding;

    GLint length = -1;
    GLint errorPos = -2;
    if (binding > 0) {
        static PFN_glGetProgramivARB getProgramiv =
            resolveGLProc<PFN_glGetProgramivARB>("glGetProgramivARB");
        if (getProgramiv) {
            getProgramiv(target, 0x8627, &length); // GL_PROGRAM_LENGTH_ARB
            glGetIntegerv(0x864B, &errorPos);      // GL_PROGRAM_ERROR_POSITION_ARB
        }
    }

    GLboolean isProgram = GL_FALSE;
    if (requested) {
        static PFN_glIsProgramARB isProgramFn =
            resolveGLProc<PFN_glIsProgramARB>("glIsProgramARB");
        if (isProgramFn)
            isProgram = isProgramFn(requested);
    }

    compatPakLog(
        "GL ARB PROG: event=%s target=0x%x(%s) requested=%u binding=%d "
        "is_program=%d length=%d error_pos=%d",
        event, (unsigned)target, nearArbProgramTargetName(target),
        (unsigned)requested, binding, isProgram ? 1 : 0, length, errorPos);
}

static void shim_glBindProgramARB(GLenum target, GLuint program) {
    static PFN_glBindProgramARB fn = resolveGLProc<PFN_glBindProgramARB>("glBindProgramARB");
    if (fn)
        fn(target, program);

    if (target == 0x8620)
        g_nearArbVertexProgramBinding = program;
    else if (target == 0x8804)
        g_nearArbFragmentProgramBinding = program;

    if (target == 0x8620 || target == 0x8804)
        nearLogArbProgramState("bind", target, program);
}
static void shim_glDeleteProgramsARB(GLsizei n, const GLuint* programs) {
    static PFN_glDeleteProgramsARB fn = resolveGLProc<PFN_glDeleteProgramsARB>("glDeleteProgramsARB");
    if (fn) fn(n, programs);
}
static void shim_glGenProgramsARB(GLsizei n, GLuint* programs) {
    static PFN_glGenProgramsARB fn = resolveGLProc<PFN_glGenProgramsARB>("glGenProgramsARB");
    if (fn) fn(n, programs);
    else if (programs && n > 0) memset(programs, 0, sizeof(GLuint) * (size_t)n);

    if (programs && n > 0 && g_arbProgramDiagCalls.load(std::memory_order_relaxed) < 512) {
        const unsigned lim = std::min<unsigned>((unsigned)n, 16);
        for (unsigned i = 0; i < lim; ++i)
            compatPakLog("GL ARB PROG: event=gen program=%u",
                         (unsigned)programs[i]);
    }
}
static GLboolean shim_glIsProgramARB(GLuint program) {
    static PFN_glIsProgramARB fn = resolveGLProc<PFN_glIsProgramARB>("glIsProgramARB");
    return fn ? fn(program) : GL_FALSE;
}
static void shim_glProgramStringARB(GLenum target, GLenum format,
                                    GLsizei len, const void* string) {
    static PFN_glProgramStringARB fn = resolveGLProc<PFN_glProgramStringARB>("glProgramStringARB");

    // Fingerprint the vertex programs before handing them to Mesa. This lets
    // us distinguish a real cached/generated ARBvp program from the tiny
    // embedded fallback used when a CGV shader cannot be found.
    bool likelyEmbeddedFallback = false;
    bool hasPositionAttr0 = false;
    bool hasBaseTcAttr8 = false;
    bool hasModelViewProj = false;
    if (target == 0x8620 && string && len > 0 && len <= 1024 * 1024) {
        const char* text = reinterpret_cast<const char*>(string);
        std::string src(text, text + len);
        hasPositionAttr0 =
            src.find("vertex.attrib[0]") != std::string::npos ||
            src.find("$vin.ATTR0") != std::string::npos;
        hasBaseTcAttr8 =
            src.find("vertex.attrib[8]") != std::string::npos ||
            src.find("$vin.ATTR8") != std::string::npos;
        hasModelViewProj = src.find("ModelViewProj") != std::string::npos;
        likelyEmbeddedFallback =
            hasPositionAttr0 && hasBaseTcAttr8 &&
            src.find("program.env[0]") != std::string::npos &&
            src.find("program.env[3]") != std::string::npos &&
            src.find("result.position") != std::string::npos &&
            src.find("result.color") != std::string::npos;
    }

    if (fn) fn(target, format, len, string);

    if ((target == 0x8620 || target == 0x8804) &&
        g_arbProgramDiagCalls.load(std::memory_order_relaxed) < 512) {
        GLint binding = -1;
        GLint length = -1;
        GLint errorPos = -2;
        glGetIntegerv(nearArbProgramBindingEnum(target), &binding);
        glGetIntegerv(0x864B, &errorPos); // GL_PROGRAM_ERROR_POSITION_ARB
        if (binding > 0) {
            static PFN_glGetProgramivARB getProgramiv =
                resolveGLProc<PFN_glGetProgramivARB>("glGetProgramivARB");
            if (getProgramiv)
                getProgramiv(target, 0x8627, &length); // GL_PROGRAM_LENGTH_ARB
        }
        compatPakLog(
            "GL ARB PROG: event=string target=0x%x(%s) binding=%d "
            "input_len=%d stored_len=%d error_pos=%d "
            "fallback=%d pos_attr0=%d base_tc_attr8=%d modelviewproj=%d",
            (unsigned)target, nearArbProgramTargetName(target),
            binding, len, length, errorPos,
            likelyEmbeddedFallback ? 1 : 0,
            hasPositionAttr0 ? 1 : 0,
            hasBaseTcAttr8 ? 1 : 0,
            hasModelViewProj ? 1 : 0);
    }
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
    if (vpakOwns(f))
        return EOF;
    if (isFakeStdio(f)) {
        compatLogFmt("game stdio[tid=%p]: %s",
                     (void*)threadGetSelf(), s ? s : "");
        return 0;
    }
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
    return ::unlink(path);
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

// Android/Bionic libc++ calls clock_gettime(CLOCK_REALTIME) from
// std::chrono::system_clock::now(). The devkitA64/newlib implementation can
// return an error for that guest call; libc++ then throws std::system_error and
// Far Cry aborts while writing the savegame header. Provide the POSIX clock ABI
// from Switch services instead. CLOCK_REALTIME is 0, CLOCK_MONOTONIC is 1.
static int stub_clock_gettime(int clock_id, struct timespec* ts) {
    if (!ts) {
        errno = EINVAL;
        return -1;
    }

    if (clock_id == 0) { // CLOCK_REALTIME
        const time_t sec = ::time(nullptr);
        if (sec == (time_t)-1) {
            errno = EIO;
            return -1;
        }
        ts->tv_sec = sec;
        ts->tv_nsec = 0;
        return 0;
    }

    // Monotonic/boottime/performance clocks: use the Switch system tick.
    const uint64_t freq = armGetSystemTickFreq();
    if (freq == 0) {
        errno = EIO;
        return -1;
    }
    const uint64_t tick = armGetSystemTick();
    ts->tv_sec = (time_t)(tick / freq);
    ts->tv_nsec = (long)(((tick % freq) * 1000000000ULL) / freq);
    return 0;
}

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

// Android/Bionic libc compatibility used by SDL3 and the CryEngine libraries.
static size_t shim_wcsnlen(const wchar_t* s, size_t maxlen) {
    if (!s) return 0;
    size_t n = 0;
    while (n < maxlen && s[n] != L'\0')
        ++n;
    return n;
}

static size_t shim_wcslcpy(wchar_t* dst, const wchar_t* src, size_t size) {
    if (!src) {
        if (dst && size) dst[0] = L'\0';
        return 0;
    }
    const size_t src_len = wcslen(src);
    if (dst && size) {
        const size_t n = (src_len < size - 1) ? src_len : size - 1;
        if (n) wmemcpy(dst, src, n);
        dst[n] = L'\0';
    }
    return src_len;
}

static size_t shim_wcslcat(wchar_t* dst, const wchar_t* src, size_t size) {
    if (!dst || !src || size == 0)
        return dst ? wcslen(dst) : 0;

    const size_t dst_len = shim_wcsnlen(dst, size);
    if (dst_len == size)
        return size + wcslen(src);

    const size_t src_len = wcslen(src);
    const size_t avail = size - dst_len - 1;
    const size_t n = src_len < avail ? src_len : avail;
    if (n) wmemcpy(dst + dst_len, src, n);
    dst[dst_len + n] = L'\0';
    return dst_len + src_len;
}

static size_t shim_strlcat(char* dst, const char* src, size_t size) {
    if (!dst || !src || size == 0)
        return dst ? strlen(dst) : 0;

    size_t dst_len = 0;
    while (dst_len < size && dst[dst_len] != '\0')
        ++dst_len;
    if (dst_len == size)
        return size + strlen(src);

    const size_t src_len = strlen(src);
    const size_t avail = size - dst_len - 1;
    const size_t n = src_len < avail ? src_len : avail;
    if (n) memcpy(dst + dst_len, src, n);
    dst[dst_len + n] = '\0';
    return dst_len + src_len;
}

static float shim_scalbnf(float x, int exp) {
    return scalbnf(x, exp);
}

static int shim_fdatasync(int fd) {
    return fsync(fd);
}

static int shim_fseeko64(FILE* f, long long off, int whence) {
    if (vpakOwns(f))
        return vpakSeek(f, (int64_t)off, whence);
    return f ? fseeko(f, (off_t)off, whence) : -1;
}

static long long shim_ftello64(FILE* f) {
    if (vpakOwns(f))
        return vpakTell64(f);
    return f ? (long long)ftello(f) : -1;
}

static size_t shim___fread_chk(void* ptr, size_t, size_t size,
                               size_t nmemb, FILE* stream) {
    return stream ? fread(ptr, size, nmemb, stream) : 0;
}

static int shim_pthread_getschedparam(void*, int* policy, void* param) {
    if (policy) *policy = 0;
    if (param) *reinterpret_cast<int*>(param) = 0;
    return 0;
}

struct ShimEntry { const char* name; void* ptr; };

extern "C" void near_openal_tls_local_context_init();
extern "C" int near_openal_cxa_thread_atexit(void (*dtor)(void*), void* obj, void* dso);

extern "C" {
    struct CS_STREAM;
    using NearCSStreamCallback =
        signed char (*)(CS_STREAM*, void*, int, void*);

    void* near_bink_cs_stream_create(NearCSStreamCallback callback,
                                      int length,
                                      unsigned int mode,
                                      int samplerate,
                                      void* userdata);
    int near_bink_cs_stream_play(int channel, CS_STREAM* stream);
    signed char near_bink_cs_stream_stop(CS_STREAM* stream);
    signed char near_bink_cs_stream_close(CS_STREAM* stream);
    void near_bink_cs_update(void);
}

// Switch-native OpenAL compatibility backend (libnx audout).
extern "C" {
    struct ALCdevice;
    struct ALCcontext;
    using NearALbool = int8_t;
    using NearALenum = int32_t;
    using NearALint = int32_t;
    using NearALsizei = int32_t;
    using NearALuint = uint32_t;
    using NearALfloat = float;

    ALCdevice* alcOpenDevice(const char*);
    NearALbool alcCloseDevice(ALCdevice*);
    ALCcontext* alcCreateContext(ALCdevice*, const NearALint*);
    NearALbool alcMakeContextCurrent(ALCcontext*);
    void alcDestroyContext(ALCcontext*);
    ALCcontext* alcGetCurrentContext(void);
    ALCdevice* alcGetContextsDevice(ALCcontext*);
    NearALenum alcGetError(ALCdevice*);
    const char* alcGetString(ALCdevice*, NearALenum);
    void alcGetIntegerv(ALCdevice*, NearALenum, NearALsizei, NearALint*);
    NearALbool alcIsExtensionPresent(ALCdevice*, const char*);
    void* alcGetProcAddress(ALCdevice*, const char*);

    NearALenum alGetError(void);
    const char* alGetString(NearALenum);
    NearALbool alIsExtensionPresent(const char*);
    void alGenBuffers(NearALsizei, NearALuint*);
    void alDeleteBuffers(NearALsizei, const NearALuint*);
    NearALbool alIsBuffer(NearALuint);
    void alBufferData(NearALuint, NearALenum, const void*, NearALsizei, NearALsizei);
    void alGetBufferi(NearALuint, NearALenum, NearALint*);
    void alGenSources(NearALsizei, NearALuint*);
    void alDeleteSources(NearALsizei, const NearALuint*);
    NearALbool alIsSource(NearALuint);
    void alSourcePlay(NearALuint);
    void alSourcePause(NearALuint);
    void alSourceStop(NearALuint);
    void alSourceRewind(NearALuint);
    void alSourcei(NearALuint, NearALenum, NearALint);
    void alSourcef(NearALuint, NearALenum, NearALfloat);
    void alSource3f(NearALuint, NearALenum, NearALfloat, NearALfloat, NearALfloat);
    void alSourcefv(NearALuint, NearALenum, const NearALfloat*);
    void alSourceQueueBuffers(NearALuint, NearALsizei, const NearALuint*);
    void alSourceUnqueueBuffers(NearALuint, NearALsizei, NearALuint*);
    void alGetSourcei(NearALuint, NearALenum, NearALint*);
    void alGetSourcef(NearALuint, NearALenum, NearALfloat*);
    void alListenerf(NearALenum, NearALfloat);
    void alListener3f(NearALenum, NearALfloat, NearALfloat, NearALfloat);
    void alListenerfv(NearALenum, const NearALfloat*);
    void alDistanceModel(NearALenum);
    void alDopplerFactor(NearALfloat);
    void alDopplerVelocity(NearALfloat);
    void alSpeedOfSound(NearALfloat);
    NearALenum alGetEnumValue(const char*);
    void* alGetProcAddress(const char*);
    NearALbool alIsEnabled(NearALenum);
    void alEnable(NearALenum);
    void alDisable(NearALenum);
    void alFinish(void);
    void alFlush(void);
}


static const ShimEntry g_shims[] = {
    // ── OpenAL Soft Android TLS ABI fallbacks ─────────────────────────────
    {"_ZTHN10ALCcontext13sLocalContextE", (void*)near_openal_tls_local_context_init},
    {"__cxa_thread_atexit_impl", (void*)near_openal_cxa_thread_atexit},
    // ── Switch-native OpenAL core ─────────────────────────────────────────
    {"alcOpenDevice", (void*)alcOpenDevice},
    {"alcCloseDevice", (void*)alcCloseDevice},
    {"alcCreateContext", (void*)alcCreateContext},
    {"alcMakeContextCurrent", (void*)alcMakeContextCurrent},
    {"alcDestroyContext", (void*)alcDestroyContext},
    {"alcGetCurrentContext", (void*)alcGetCurrentContext},
    {"alcGetContextsDevice", (void*)alcGetContextsDevice},
    {"alcGetError", (void*)alcGetError},
    {"alcGetString", (void*)alcGetString},
    {"alcGetIntegerv", (void*)alcGetIntegerv},
    {"alcIsExtensionPresent", (void*)alcIsExtensionPresent},
    {"alcGetProcAddress", (void*)alcGetProcAddress},
    {"alGetError", (void*)alGetError},
    {"alGetString", (void*)alGetString},
    {"alIsExtensionPresent", (void*)alIsExtensionPresent},
    {"alGenBuffers", (void*)alGenBuffers},
    {"alDeleteBuffers", (void*)alDeleteBuffers},
    {"alIsBuffer", (void*)alIsBuffer},
    {"alBufferData", (void*)alBufferData},
    {"alGetBufferi", (void*)alGetBufferi},
    {"alGenSources", (void*)alGenSources},
    {"alDeleteSources", (void*)alDeleteSources},
    {"alIsSource", (void*)alIsSource},
    {"alSourcePlay", (void*)alSourcePlay},
    {"alSourcePause", (void*)alSourcePause},
    {"alSourceStop", (void*)alSourceStop},
    {"alSourceRewind", (void*)alSourceRewind},
    {"alSourcei", (void*)alSourcei},
    {"alSourcef", (void*)alSourcef},
    {"alSource3f", (void*)alSource3f},
    {"alSourcefv", (void*)alSourcefv},
    {"alSourceQueueBuffers", (void*)alSourceQueueBuffers},
    {"alSourceUnqueueBuffers", (void*)alSourceUnqueueBuffers},
    {"alGetSourcei", (void*)alGetSourcei},
    {"alGetSourcef", (void*)alGetSourcef},
    {"alListenerf", (void*)alListenerf},
    {"alListener3f", (void*)alListener3f},
    {"alListenerfv", (void*)alListenerfv},
    {"alDistanceModel", (void*)alDistanceModel},
    {"alDopplerFactor", (void*)alDopplerFactor},
    {"alDopplerVelocity", (void*)alDopplerVelocity},
    {"alSpeedOfSound", (void*)alSpeedOfSound},
    {"alGetEnumValue", (void*)alGetEnumValue},
    {"alGetProcAddress", (void*)alGetProcAddress},
    {"alIsEnabled", (void*)alIsEnabled},
    {"alEnable", (void*)alEnable},
    {"alDisable", (void*)alDisable},
    {"alFinish", (void*)alFinish},
    {"alFlush", (void*)alFlush},

    // ── Bink video audio: SDL3 stream backend ───────────────────────────────
    {"CS_Stream_Create", (void*)near_bink_cs_stream_create},
    {"CS_Stream_Play",   (void*)near_bink_cs_stream_play},
    {"CS_Stream_Stop",   (void*)near_bink_cs_stream_stop},
    {"CS_Stream_Close",  (void*)near_bink_cs_stream_close},
    {"CS_Update",        (void*)near_bink_cs_update},

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
    {"ldiv",           (void*)ldiv},
    {"scalbnf",        (void*)shim_scalbnf},
    {"feholdexcept",   (void*)feholdexcept},
    {"fesetenv",       (void*)fesetenv},

    // ── SDL/Bionic libc64 compatibility ────────────────────────────────────
    {"fdatasync",       (void*)shim_fdatasync},
    {"fseeko64",        (void*)shim_fseeko64},
    {"ftello64",        (void*)shim_ftello64},
    {"__fread_chk",     (void*)shim___fread_chk},
    {"wcsnlen",         (void*)shim_wcsnlen},
    {"wcslcpy",         (void*)shim_wcslcpy},
    {"wcslcat",         (void*)shim_wcslcat},
    {"strlcat",         (void*)shim_strlcat},
    {"pthread_getschedparam", (void*)shim_pthread_getschedparam},

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
    {"strcasecmp",  (void*)stub_strcasecmp},
    {"__strcasecmp", (void*)stub_strcasecmp},
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
    {"fgets",       (void*)sh_fgets},
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
    {"lseek",       (void*)sh_lseek},
    {"stat",        (void*)stub_stat},
    {"stat64",       (void*)stub_stat},
    {"fstat",       (void*)sh_fstat},
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
    {"SDL_GetPrimaryDisplay",      (void*)w_SDL_GetPrimaryDisplay},
    {"SDL_GetCurrentDisplayMode",  (void*)w_SDL_GetCurrentDisplayMode},
    {"SDL_GetDesktopDisplayMode",  (void*)w_SDL_GetDesktopDisplayMode},
    {"SDL_GetWindowSizeInPixels",  (void*)w_SDL_GetWindowSizeInPixels},
    {"SDL_CreateWindow",           (void*)w_SDL_CreateWindow},
    {"SDL_GL_CreateContext",       (void*)w_SDL_GL_CreateContext},
    {"SDL_GL_MakeCurrent",         (void*)w_SDL_GL_MakeCurrent},
    {"SDL_GL_SwapWindow",          (void*)w_SDL_GL_SwapWindow},

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
    {"eglSwapBuffers",      (void*)w_eglSwapBuffers},
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
    {"glActiveTexture",     (void*)shim_glActiveTexture},
    {"glAttachShader",      (void*)glAttachShader},
    {"glBindAttribLocation",(void*)glBindAttribLocation},
    {"glBindBuffer",        (void*)glBindBuffer},
    {"glBindFramebuffer",   (void*)glBindFramebuffer},
    {"glBindRenderbuffer",  (void*)glBindRenderbuffer},
    {"glBindTexture",       (void*)shim_glBindTexture},
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
    {"glDisable",           (void*)shim_glDisableCompat},
    {"glDisableVertexAttribArray",(void*)glDisableVertexAttribArray},
    {"glDrawArrays",        (void*)w_glDrawArrays},
    {"glDrawElements",      (void*)w_glDrawElements},
    {"glEnable",            (void*)shim_glEnableCompat},
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
    {"glIsEnabled",         (void*)shim_glIsEnabledCompat},
    {"glIsFramebuffer",     (void*)glIsFramebuffer},
    {"glIsProgram",         (void*)glIsProgram},
    {"glIsRenderbuffer",    (void*)glIsRenderbuffer},
    {"glIsShader",          (void*)glIsShader},
    {"glIsTexture",         (void*)glIsTexture},
    {"glLineWidth",         (void*)glLineWidth},
    {"glLinkProgram",       (void*)glLinkProgram},
    {"glPixelStorei",       (void*)shim_glPixelStorei},
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
    {"glTexImage2D",        (void*)shim_glTexImage2D},
    {"glTexParameterf",     (void*)shim_glTexParameterf},
    {"glTexParameterfv",    (void*)shim_glTexParameterfv},
    {"glTexParameteri",     (void*)shim_glTexParameteri},
    {"glTexParameteriv",    (void*)shim_glTexParameteriv},
    {"glTexSubImage2D",     (void*)shim_glTexSubImage2D},
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
    {"clock_gettime", (void*)stub_clock_gettime},
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
    {"wcsrtombs", (void*)diag_wcsrtombs},
    {"mbsrtowcs", (void*)diag_mbsrtowcs},
    {"wcsnrtombs",(void*)stub_wcsnrtombs},
    {"mbsnrtowcs",(void*)stub_mbsnrtowcs},
    {"wcrtomb",   (void*)wcrtomb},
    {"mbtowc",    (void*)diag_mbtowc},
    {"mbrtowc",   (void*)diag_mbrtowc},
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

    // ── Bink video audio (SDL3) ─────────────────────────────────────────────
    // Far Cry's Android Bink library imports these CS_* functions directly.
    // Without explicit bindings they fall through to the old OpenAL path.
    {"CS_Stream_Create", (void*)near_bink_cs_stream_create},
    {"CS_Stream_Play",   (void*)near_bink_cs_stream_play},
    {"CS_Stream_Stop",   (void*)near_bink_cs_stream_stop},
    {"CS_Stream_Close",  (void*)near_bink_cs_stream_close},
    {"CS_Update",        (void*)near_bink_cs_update},

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
    // Android GLES/OES aliases used by libSDL3.so.
    {"glBlendEquationOES",             (void*)shim_glBlendEquationOES},
    {"glBlendEquationSeparateOES",     (void*)shim_glBlendEquationSeparateOES},
    {"glBlendFuncSeparateOES",        (void*)shim_glBlendFuncSeparateOES},
    {"glDrawTexfOES",                  (void*)shim_glDrawTexfOES},
    {"glGenFramebuffersOES",           (void*)shim_glGenFramebuffersOES},
    {"glOrthof",                       (void*)shim_glOrthof},
    {"glBindFramebufferOES",           (void*)shim_glBindFramebufferOES},
    {"glFramebufferTexture2DOES",     (void*)shim_glFramebufferTexture2DOES},
    {"glCheckFramebufferStatusOES",   (void*)shim_glCheckFramebufferStatusOES},
    {"glDeleteFramebuffersOES",        (void*)shim_glDeleteFramebuffersOES},

    // Desktop OpenGL 1.x/2.1 entry points used by Far Cry's XRenderOGL.
    {"glActiveStencilFaceEXT", (void*)shim_glActiveStencilFaceEXT},
    {"glAlphaFunc", (void*)glAlphaFunc},
    {"glAreTexturesResident", (void*)glAreTexturesResident},
    {"glBegin", (void*)glBegin},
    {"glBindBufferARB", (void*)shim_glBindBufferARB},
    {"glBufferDataARB", (void*)shim_glBufferDataARB},
    {"glMapBufferARB", (void*)shim_glMapBufferARB},
    {"glUnmapBufferARB", (void*)shim_glUnmapBufferARB},
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
    {"glVertexAttribPointerNV", (void*)shim_glVertexAttribPointerNV},
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
    {"glDisableClientState", (void*)shim_glDisableClientStateCompat},
    {"glDrawBuffer", (void*)glDrawBuffer},
    {"glEnableClientState", (void*)shim_glEnableClientStateCompat},
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
    {"glVertexPointer", (void*)shim_glVertexPointerCompat},
    {"glTexCoord2f", (void*)glTexCoord2f},
    {"glTexCoord3f", (void*)glTexCoord3f},
    {"glTexCoordPointer", (void*)glTexCoordPointer},
    {"glTexEnvf", (void*)shim_glTexEnvfCompat},
    {"glTexEnvfv", (void*)shim_glTexEnvfvCompat},
    {"glTexEnvi", (void*)shim_glTexEnviCompat},
    {"glTexEnviv", (void*)shim_glTexEnvivCompat},
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

// ─── shimResolve ──────────────────────────────────────────────────────────────
// Relocation binding calls this for every imported symbol. Build the table once
// on first use so repeated lookups are O(1) instead of scanning every shim.
void* shimResolve(const char* name) {
    if (!name) return nullptr;

    static std::unordered_map<std::string, void*> index;
    static bool initialized = false;
    if (!initialized) {
        index.reserve(NUM_SHIMS * 2);
        for (size_t i = 0; i < NUM_SHIMS; ++i)
            index.emplace(g_shims[i].name, g_shims[i].ptr);
        initialized = true;
    }

    auto it = index.find(name);
    return it != index.end() ? it->second : nullptr;
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