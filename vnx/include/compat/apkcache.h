#pragma once
#include <cstdio>
#include <cstddef>
#include <cstdint>

// ─── APK read cache ─────────────────────────────────────────────────────────
// The game is handed its own .apk path (nativeSetPaths), and cocos2d-x's
// FileUtils opens it as a zip and reads assets straight out of it. Minizip's
// access pattern is the worst case for an SD card: thousands of tiny reads and
// seeks, and a fresh linear walk of the zip's central directory — which lives
// at the end of the file — every time an asset is opened.
//
// On Android that is absorbed by the page cache. Here every one of those reads
// is a real IPC round trip to the FS sysmodule, on the render thread, while the
// game is streaming in scenery.
//
// Two caches, sized for a console where the game needs the memory more than we
// do:
//   - the tail: one shared copy of the last 2MB, which is where the central
//     directory is. This is the one that matters — it turns every repeated
//     directory walk into memory reads.
//   - a small per-stream block cache for everything else, so a sequential
//     asset read doesn't go back to the card for each chunk.
//
// The design is taken from xflipperkast's HCR_NX
// (https://github.com/xflipperkast/HCR_NX), which identified the access pattern
// and the central-directory rewalk; their version also keeps a 32MB shared LRU
// behind these, which is left out here until there is a hardware measurement to
// justify spending that much of a game's memory on it.
//
// Correctness before speed: a cached stream keeps the real FILE*'s position in
// step with its own, so any stdio call that isn't routed through here still
// sees the file exactly where it should be.
namespace apkcache {

// Adopt a newly-opened stream if `path` is the file worth caching. Cheap and
// safe to call for every fopen; returns true if the stream is now cached.
bool adopt(FILE* f, const char* path);

// Which file to cache — the APK this launch is running. Adopting anything else
// would spend memory on files that are read once.
void setApkPath(const char* apk_path);

// True if this stream is cached, i.e. the calls below apply to it.
bool owns(FILE* f);

// stdio, served from the caches. Only valid for a stream `owns` accepts.
size_t  read(FILE* f, void* buf, size_t size, size_t n);
int     seek(FILE* f, int64_t off, int whence);
int64_t tell(FILE* f);
int     getc(FILE* f);
int     eof(FILE* f);
void    close(FILE* f);   // releases the cache; the caller still fcloses

// Bytes served from cache vs. read from the card, for the log.
void stats(uint64_t* from_cache, uint64_t* from_card);

// Drop everything (end of a launch, or a test).
void reset(void);

}  // namespace apkcache
