#pragma once
#include <string>
#include <vector>

// ─── OBB (Android expansion file) support ───────────────────────────────────
// An APK is capped at 100MB on Google Play, so a large game ships its data
// beside the APK in an expansion file: an OBB, named
//   main.<version>.<package>.obb     (the base data)
//   patch.<version>.<package>.obb    (an overlay shipped later)
// and placed at /sdcard/Android/obb/<package>/ on the device.
//
// Viridite copied the APK and nothing else, so every title that keeps its data
// in an OBB could only ever fail on its first asset read — the data was never
// on the Switch. That is most of the Square Enix catalogue, GTA: San Andreas,
// Castle of Illusion, After Burner Climax and Kingdom Hearts Union χ.
//
// What this does NOT do is unpack them. Android never does: it hands the game
// the path and the game reads the OBB itself (usually as a zip). Unpacking
// would double the space a 2GB title needs and hand games a directory layout
// they never expect. So this finds the OBBs, puts them where getObbDir() says
// they are, and makes the paths a game might hardcode resolve there too.
namespace obb {

struct File {
    std::string path;         // where it is now
    std::string canonicalName;// main.<ver>.<pkg>.obb — the name Android would use
    bool        patch = false;// patch overlays main
    int         version = 0;  // <ver> from the name; 0 when the name doesn't carry one
};

// Does this filename name an OBB belonging to `pkg`? Fills `out` (minus path)
// when it does. Accepts the Android form (main.86.com.x.y.obb) and the bare
// form (main.obb) that ships inside some APKs — the bare form carries no
// package, so it is only ever accepted from a directory already specific to
// this game, which is the caller's business, not this function's.
bool parseName(const std::string& filename, const std::string& pkg, File* out);

// Android's layering order: main before patch, lower version before higher, so
// a later file overlays an earlier one.
void sortForInstall(std::vector<File>& files);

// Every OBB for `pkg` findable from `apk_path`, in install order. Searched in
// the layouts people actually end up with: the Android tree copied off a phone
// (Android/obb/<pkg>/), a folder named for the package or for the APK, an
// obb/ subfolder, and loose beside the APK.
std::vector<File> find(const std::string& apk_path, const std::string& pkg);

// Copy each OBB into `obb_dir` under its canonical name, creating the
// directory. A file already there with the same size is left alone, so a
// relaunch of a 2GB title doesn't recopy it. Returns how many are in place
// afterwards, or -1 if the directory could not be made.
int install(const std::vector<File>& files, const std::string& obb_dir);

// Where this game's OBBs live once installed. `data_dir` is the game's own
// directory — the same one getObbDir() reports.
std::string dirFor(const std::string& data_dir);

// A game that hardcodes an Android OBB path gets it pointed at the real one.
// Returns the rewritten path, or an empty string if `path` isn't an OBB path
// this should touch. Pure string work: no filesystem access, so the caller
// decides whether the result exists.
std::string remapPath(const std::string& path, const std::string& pkg,
                      const std::string& obb_dir);

}  // namespace obb
