// ─── OBB (Android expansion file) support ───────────────────────────────────
// See include/compat/obb.h for what this is and why it does not unpack.
//
// Everything here is plain stdio and string work, no libnx and no logging
// callbacks, so the whole module builds and runs on a dev machine — which is
// the only way any of it could be checked, since it exists precisely for games
// nobody here has an APK for. test/obb/harness.cpp exercises it against a real
// directory tree.
#include "compat/obb.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace obb {
namespace {

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

long long fileSize(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long long)st.st_size;
}

std::string dirOf(const std::string& path) {
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? std::string(".") : path.substr(0, sl);
}

std::string baseOf(const std::string& path) {
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

std::string stripExt(const std::string& name) {
    size_t d = name.find_last_of('.');
    return d == std::string::npos ? name : name.substr(0, d);
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Everything in `dir` that parses as an OBB for `pkg`. Missing directories are
// not an error: most of the searched layouts won't exist for any given game.
void scanDir(const std::string& dir, const std::string& pkg, bool dirIsGameSpecific,
             std::vector<File>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (!endsWith(name, ".obb")) continue;
        File f;
        if (!parseName(name, pkg, &f)) continue;
        // A name carrying no package ("main.obb") only identifies a game by
        // where it sits. Beside an APK that could be any of several games'
        // data, so it is taken only from a folder that is already this game's.
        if (f.canonicalName.empty()) {
            if (!dirIsGameSpecific) continue;
            f.canonicalName = name;
        }
        f.path = dir + "/" + name;
        // A duplicate of one already found (the same canonical name in an
        // earlier, more specific location) does not replace it.
        bool seen = false;
        for (const File& e : out)
            if (e.canonicalName == f.canonicalName) { seen = true; break; }
        if (!seen) out.push_back(f);
    }
    closedir(d);
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    // An OBB is routinely over a gigabyte, so this is a big sequential copy on
    // an SD card: a chunk far larger than the default stdio buffer is the
    // difference between one long write and tens of thousands of small ones.
    static const size_t kChunk = 1u << 20;
    char* buf = (char*)malloc(kChunk);
    if (!buf) { fclose(in); fclose(out); return false; }
    bool ok = true;
    for (;;) {
        size_t n = fread(buf, 1, kChunk, in);
        if (n == 0) break;
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(to.c_str());
    return ok;
}

}  // namespace

bool parseName(const std::string& filename, const std::string& pkg, File* out) {
    if (!out) return false;
    if (!endsWith(filename, ".obb")) return false;

    File f;
    std::string rest;
    if (filename.rfind("main.", 0) == 0)       { f.patch = false; rest = filename.substr(5); }
    else if (filename.rfind("patch.", 0) == 0) { f.patch = true;  rest = filename.substr(6); }
    else return false;

    // "main.obb" — no version, no package. Identified by location alone, which
    // is the caller's call; signalled by leaving canonicalName empty.
    if (rest == "obb") { *out = f; return true; }

    // "main.<version>.<package>.obb"
    size_t dot = rest.find('.');
    if (dot == std::string::npos || dot == 0) return false;
    for (size_t i = 0; i < dot; i++)
        if (rest[i] < '0' || rest[i] > '9') return false;
    long v = strtol(rest.substr(0, dot).c_str(), nullptr, 10);
    std::string tail = rest.substr(dot + 1);          // "<package>.obb"
    if (!endsWith(tail, ".obb")) return false;
    std::string named = tail.substr(0, tail.size() - 4);
    // An empty package in the filename can't be checked, and a mismatch is
    // another game's data sitting in the same folder.
    if (named.empty() || (!pkg.empty() && named != pkg)) return false;

    f.version       = (int)v;
    f.canonicalName = filename;
    *out = f;
    return true;
}

void sortForInstall(std::vector<File>& files) {
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) {
        if (a.patch != b.patch) return !a.patch;      // main before patch
        return a.version < b.version;                  // older before newer
    });
}

std::vector<File> find(const std::string& apk_path, const std::string& pkg) {
    std::vector<File> out;
    const std::string dir = dirOf(apk_path);
    const std::string apkStem = stripExt(baseOf(apk_path));

    // Most specific first, so a copy in a folder that is unambiguously this
    // game's wins over a loose file beside the APK.
    if (!pkg.empty()) {
        scanDir(dir + "/Android/obb/" + pkg, pkg, true, out);
        scanDir(dir + "/obb/" + pkg,         pkg, true, out);
        scanDir(dir + "/" + pkg,             pkg, true, out);
    }
    scanDir(dir + "/" + apkStem, pkg, true,  out);   // <name>.apk + <name>/main.obb
    scanDir(dir + "/obb",        pkg, false, out);
    scanDir(dir,                 pkg, false, out);

    sortForInstall(out);
    return out;
}

std::string dirFor(const std::string& data_dir) {
    return data_dir + "/obb";
}

int install(const std::vector<File>& files, const std::string& obb_dir) {
    if (files.empty()) return 0;
    if (!isDir(obb_dir) && mkdir(obb_dir.c_str(), 0777) != 0 && !isDir(obb_dir))
        return -1;

    int inPlace = 0;
    for (const File& f : files) {
        const std::string dest = obb_dir + "/" + f.canonicalName;
        long long srcSize = fileSize(f.path);
        // Same size means the same file for something this large and this
        // immutable — an OBB is a shipped, versioned artefact, not a document
        // someone edits. Re-copying a gigabyte on every launch would be a far
        // worse trade than the vanishing chance of a same-size replacement.
        if (fileSize(dest) == srcSize && srcSize >= 0) { inPlace++; continue; }
        if (copyFile(f.path, dest)) inPlace++;
    }
    return inPlace;
}

std::string remapPath(const std::string& path, const std::string& pkg,
                      const std::string& obb_dir) {
    if (path.empty()) return "";
    // The prefixes an Android game may have baked in for external storage. All
    // of them mean the same directory on a real device.
    static const char* kRoots[] = {
        "/storage/emulated/0/", "/storage/emulated/legacy/", "/storage/sdcard0/",
        "/sdcard/", "/mnt/sdcard/", "/storage/self/primary/",
    };
    std::string rest;
    bool rooted = false;
    for (const char* r : kRoots) {
        size_t n = strlen(r);
        if (path.compare(0, n, r) == 0) { rest = path.substr(n); rooted = true; break; }
    }
    if (!rooted) return "";
    // Only the OBB tree. Anything else under external storage is somebody
    // else's problem, and silently rewriting it would hide a real miss.
    const std::string wanted = "Android/obb/" + pkg + "/";
    if (rest.compare(0, wanted.size(), wanted) != 0) return "";
    std::string file = rest.substr(wanted.size());
    if (file.empty() || file.find('/') != std::string::npos) return "";
    return obb_dir + "/" + file;
}

}  // namespace obb
