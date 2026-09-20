// ─── APK read cache ─────────────────────────────────────────────────────────
// See include/compat/apkcache.h for what this is for and where the design came
// from. Plain POSIX and stdio, no libnx, so the whole thing runs on a dev
// machine — which is the only way to be sure a cache is transparent, and a
// cache that is not transparent corrupts assets rather than slowing them down.
#include "compat/apkcache.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

namespace apkcache {
namespace {

// Sized for a console: the tail is the one that pays for itself (a zip central
// directory is a few hundred KB and is walked again on every asset open), and
// the per-stream blocks only need to cover a sequential read in progress.
constexpr size_t   kTailBytes   = 2u * 1024 * 1024;
constexpr size_t   kBlockBytes  = 256u * 1024;
constexpr unsigned kBlockWays   = 4;
constexpr unsigned kMaxStreams  = 8;

struct Block {
    uint8_t* data = nullptr;
    int64_t  off  = -1;      // file offset this block starts at, -1 = empty
    size_t   len  = 0;
    uint64_t age  = 0;
};

struct Stream {
    FILE*   f    = nullptr;  // the handle the game holds
    int     fd   = -1;       // our own descriptor, so our reads never move theirs
    int64_t pos  = 0;        // where the game thinks it is
    int64_t size = 0;
    bool    eof  = false;
    uint64_t age = 0;
    Block   ways[kBlockWays];
};

std::mutex  g_lock;
Stream      g_streams[kMaxStreams];
std::string g_apk_path;

uint8_t* g_tail      = nullptr;
int64_t  g_tail_off  = 0;
size_t   g_tail_len  = 0;
int64_t  g_tail_size = -1;      // file size the tail was taken from

uint64_t g_from_cache = 0, g_from_card = 0;

// Read exactly `want` bytes at `off`, retrying short reads. Returns bytes read.
size_t readAt(int fd, int64_t off, uint8_t* dst, size_t want) {
    size_t got = 0;
    while (got < want) {
        ssize_t rc = pread(fd, dst + got, want - got, (off_t)(off + (int64_t)got));
        if (rc > 0) { got += (size_t)rc; continue; }
        if (rc < 0 && errno == EINTR) continue;
        break;
    }
    return got;
}

// The last kTailBytes of the file, shared by every stream on it. Taken once.
void prepareTail(int fd, int64_t size) {
    if (size <= 0) return;
    if (g_tail && g_tail_size == size) return;
    const size_t want = (size < (int64_t)kTailBytes) ? (size_t)size : kTailBytes;
    const int64_t off = size - (int64_t)want;
    uint8_t* buf = (uint8_t*)malloc(want);
    if (!buf) return;
    if (readAt(fd, off, buf, want) != want) { free(buf); return; }
    free(g_tail);
    g_tail      = buf;
    g_tail_off  = off;
    g_tail_len  = want;
    g_tail_size = size;
}

Stream* find(FILE* f) {
    if (!f) return nullptr;
    for (Stream& s : g_streams) if (s.f == f) return &s;
    return nullptr;
}

// Serve [off, off+len) from the tail if it lies entirely inside it.
bool fromTail(const Stream& s, int64_t off, size_t len, uint8_t* dst) {
    if (!g_tail || g_tail_size != s.size) return false;
    if (off < g_tail_off) return false;
    if (off + (int64_t)len > g_tail_off + (int64_t)g_tail_len) return false;
    memcpy(dst, g_tail + (off - g_tail_off), len);
    return true;
}

// The block holding `off`, filling one in if needed. Null if it can't be read.
Block* blockFor(Stream& s, int64_t off) {
    const int64_t base = off - (off % (int64_t)kBlockBytes);
    Block* victim = &s.ways[0];
    for (Block& b : s.ways) {
        if (b.off == base && b.len > 0) { b.age = ++s.age; return &b; }
        if (b.off < 0) victim = &b;
        else if (victim->off >= 0 && b.age < victim->age) victim = &b;
    }
    if (!victim->data) {
        victim->data = (uint8_t*)malloc(kBlockBytes);
        if (!victim->data) return nullptr;
    }
    const size_t want = (size_t)((s.size - base) < (int64_t)kBlockBytes
                                 ? (s.size - base) : (int64_t)kBlockBytes);
    const size_t got = readAt(s.fd, base, victim->data, want);
    if (got == 0) { victim->off = -1; victim->len = 0; return nullptr; }
    g_from_card += got;
    victim->off = base;
    victim->len = got;
    victim->age = ++s.age;
    return victim;
}

// Keep the game's own FILE* where it thinks it is, so anything not routed
// through this code — feof, fscanf, a stdio call nobody shimmed — still sees
// the right position. An fseek moves an offset; it reads nothing.
void syncHandle(Stream& s) {
    if (s.f) fseek(s.f, (long)s.pos, SEEK_SET);
}

void release(Stream& s) {
    if (s.fd >= 0) ::close(s.fd);
    for (Block& b : s.ways) free(b.data);
    s = Stream();
}

}  // namespace

void setApkPath(const char* apk_path) {
    std::lock_guard<std::mutex> g(g_lock);
    g_apk_path = apk_path ? apk_path : "";
}

bool adopt(FILE* f, const char* path) {
    if (!f || !path) return false;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_apk_path.empty() || g_apk_path != path) return false;

    for (Stream& s : g_streams) {
        if (s.f) continue;
        s = Stream();
        s.fd = open(path, O_RDONLY);
        if (s.fd < 0) { s = Stream(); return false; }
        struct stat st;
        if (fstat(s.fd, &st) != 0 || st.st_size <= 0) { ::close(s.fd); s = Stream(); return false; }
        s.f    = f;
        s.size = (int64_t)st.st_size;
        prepareTail(s.fd, s.size);
        return true;
    }
    return false;   // more streams than we cache; the extra ones pass through
}

bool owns(FILE* f) {
    std::lock_guard<std::mutex> g(g_lock);
    return find(f) != nullptr;
}

size_t read(FILE* f, void* buf, size_t size, size_t n) {
    if (!buf || size == 0 || n == 0) return 0;
    std::lock_guard<std::mutex> g(g_lock);
    Stream* s = find(f);
    if (!s) return 0;

    // Overflow would wrap the request into a small one and quietly truncate.
    if (n > SIZE_MAX / size) return 0;
    size_t want = size * n;
    if (s->pos >= s->size) { s->eof = true; return 0; }
    if ((int64_t)want > s->size - s->pos) want = (size_t)(s->size - s->pos);

    uint8_t* dst = (uint8_t*)buf;
    size_t done = 0;
    while (done < want) {
        const int64_t at = s->pos + (int64_t)done;
        const size_t  remaining = want - done;

        if (fromTail(*s, at, remaining, dst + done)) {
            g_from_cache += remaining;
            done = want;
            break;
        }
        Block* b = blockFor(*s, at);
        if (!b) break;
        const size_t inBlock = (size_t)(at - b->off);
        if (inBlock >= b->len) break;                 // short block: end of file
        size_t take = b->len - inBlock;
        if (take > remaining) take = remaining;
        memcpy(dst + done, b->data + inBlock, take);
        g_from_cache += take;
        done += take;
    }

    s->pos += (int64_t)done;
    if (s->pos >= s->size) s->eof = true;
    syncHandle(*s);
    // fread returns whole items, and a partial item is not one.
    return done / size;
}

int seek(FILE* f, int64_t off, int whence) {
    std::lock_guard<std::mutex> g(g_lock);
    Stream* s = find(f);
    if (!s) return -1;
    int64_t target;
    switch (whence) {
        case SEEK_SET: target = off; break;
        case SEEK_CUR: target = s->pos + off; break;
        case SEEK_END: target = s->size + off; break;
        default: return -1;
    }
    if (target < 0) return -1;
    // Seeking past the end is legal and clears EOF; the next read returns none.
    s->pos = target;
    s->eof = false;
    syncHandle(*s);
    return 0;
}

int64_t tell(FILE* f) {
    std::lock_guard<std::mutex> g(g_lock);
    Stream* s = find(f);
    return s ? s->pos : -1;
}

int getc(FILE* f) {
    unsigned char c;
    if (read(f, &c, 1, 1) != 1) return EOF;
    return (int)c;
}

int eof(FILE* f) {
    std::lock_guard<std::mutex> g(g_lock);
    Stream* s = find(f);
    return s && s->eof ? 1 : 0;
}

void close(FILE* f) {
    std::lock_guard<std::mutex> g(g_lock);
    if (Stream* s = find(f)) release(*s);
}

void stats(uint64_t* from_cache, uint64_t* from_card) {
    std::lock_guard<std::mutex> g(g_lock);
    if (from_cache) *from_cache = g_from_cache;
    if (from_card)  *from_card  = g_from_card;
}

void reset(void) {
    std::lock_guard<std::mutex> g(g_lock);
    for (Stream& s : g_streams) if (s.f) release(s);
    free(g_tail);
    g_tail = nullptr; g_tail_len = 0; g_tail_off = 0; g_tail_size = -1;
    g_from_cache = g_from_card = 0;
    g_apk_path.clear();
}

}  // namespace apkcache
