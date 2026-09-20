#pragma once
#include <cstddef>
#include <cstring>

// ─── Viridite game database ─────────────────────────────────────────────────
// One table of what is known about each Android title Viridite can be pointed
// at: which library is the game (as opposed to its audio backend or its C++
// runtime), which engine it is built on, what extra data it needs beyond the
// APK, and — separately from all of that — whether it has ever actually run
// here.
//
// CANONICAL COPY: VNX-Translation-Core/include/compat/gamedb.h
// It is mirrored byte-identically as Viridite/include/gamedb.h so the launcher
// and the Core can never disagree about a title. It is deliberately header-only
// and dependency-free (no libnx, no SDL, no logging) so keeping the two in step
// is a file copy, not a merge.
//
// ── Where the facts come from ───────────────────────────────────────────────
// Most of the per-title facts below are transcribed from the READMEs of
// NaGaa95's independent Switch ports (https://github.com/NaGaa95), who has
// built working per-title ports of these games using the same basic technique
// Viridite uses generically: load the original Android arm64 .so, resolve its
// imports against native Switch implementations, run it in a minimal
// Android-shaped environment. Their ports are the evidence that these titles
// are portable at all, and their READMEs name the exact library and data files
// each one needs. Full credit to them; every entry carries the port it was
// read from in `reference`.
//
// ── What this table does NOT claim ──────────────────────────────────────────
// A row here is not a statement that the game runs in Viridite. That is the
// `support` field alone, and only `Support::Playable` means "run on real
// hardware". Everything else is knowledge about the title — useful for picking
// the right entry library and for telling someone up front what a game will
// need — held separately from any claim about whether it works.
namespace gamedb {

// The native runtime a title is built on. This decides which path the Core
// takes, not how well it works.
enum class Engine {
    Unknown,
    Cocos2dx,      // exported Java_org_cocos2dx_..._nativeRender — the Core drives it directly
    UnityIl2cpp,   // libmain/libunity/libil2cpp — VNX-Unity-Runtime bringup
    TtFusion,      // TT Games' Fusion engine (the LEGO titles)
    Source,        // nillerusr's Android Source engine port
    AdobeAir,      // Harman Adobe AIR — the game itself is ActionScript bytecode
    Dalvik,        // the game's logic is DEX, not a native library at all
    Native,        // the game's own engine, no shared runtime to key off
};

// How far a title has actually got *in Viridite*. Deliberately not a guess at
// how well it might do.
enum class Support {
    Playable,     // played on real hardware, not just booted
    Loads,        // libraries load and link clean here; it does not run yet
    Untested,     // known portable elsewhere; never run through Viridite
    Unsupported,  // needs an engine path Viridite does not have
};

// What a library in a game's lib/arm64-v8a/ actually is. The Core used to
// assume the largest .so was the game; for several of the titles below that is
// its audio backend or its C++ runtime instead, so the roles are named.
enum class SoRole {
    Entry,       // the game itself — the library to load and enter
    Dependency,  // audio backend, C++ runtime, ads/analytics — never the game
    Unknown,
};

struct SoSignature {
    const char* soname;
    Engine      engine;
    SoRole      role;
    const char* what;
};

struct Title {
    // Package id, or nullptr when the port's README does not document one. A
    // null pkg is never matched at runtime — the row is reference only.
    const char* pkg;
    const char* name;
    const char* version;     // the release the facts below were read against
    const char* entrySo;     // the library that IS the game
    Engine      engine;
    Support     support;
    bool        controller;  // has a real gamepad path of its own
    int         marketVariation;  // Cocos2d-x getMarketVariation, or -1
    const char* extraData;   // what it needs beyond the APK, or nullptr
    const char* reference;   // where these facts came from
    // True only when the game's data is in an OBB shipped *beside* the APK, so
    // copying the APK alone cannot work. Deliberately not set for the titles
    // whose OBB rides inside the APK's own assets/ (the Square Enix ports do
    // this) — those install correctly from the APK alone — nor for one that is
    // optional. A launch that needs a file nobody has can then say which file,
    // instead of failing later somewhere unrecognisable.
    bool        needsObb = false;
};

// ── Library roles ───────────────────────────────────────────────────────────
// Keyed by library name, not by title: "libfmod.so is an audio backend" is true
// of every game that ships it, so this stays correct for titles nobody has
// written a row for yet.
inline constexpr SoSignature kSoSignatures[] = {
    // Engine entry points shared across many titles.
    {"libmain.so",        Engine::UnityIl2cpp, SoRole::Entry,      "Unity NativeLoader bootstrap — the entry point, though not the largest lib"},
    {"libunity.so",       Engine::UnityIl2cpp, SoRole::Dependency, "Unity player runtime — loaded by libmain, not entered directly"},
    {"libil2cpp.so",      Engine::UnityIl2cpp, SoRole::Dependency, "IL2CPP-compiled C# — usually the largest lib, never the entry point"},
    {"libcocos2dcpp.so",  Engine::Cocos2dx,    SoRole::Entry,      "cocos2d-x game binary"},
    {"libgame.so",        Engine::Cocos2dx,    SoRole::Entry,      "cocos2d-x game binary (Fingersoft naming)"},

    // Runtime/dependency libraries — mistaking one of these for the game is the
    // failure mode the size heuristic has.
    {"libc++_shared.so",  Engine::Unknown,     SoRole::Dependency, "NDK C++ runtime"},
    {"libfmod.so",        Engine::Unknown,     SoRole::Dependency, "FMOD audio backend"},
    {"libfmodL.so",       Engine::Unknown,     SoRole::Dependency, "FMOD audio backend (logging build)"},
    {"libfmodex.so",      Engine::Unknown,     SoRole::Dependency, "FMOD Ex audio backend"},
    {"libfmodstudio.so",  Engine::Unknown,     SoRole::Dependency, "FMOD Studio audio backend"},
    {"libopenal.so",      Engine::Unknown,     SoRole::Dependency, "OpenAL audio backend"},
    {"libTone.so",        Engine::Unknown,     SoRole::Dependency, "Animal Crossing: Pocket Camp audio helper"},
    {"libff4proxy.so",    Engine::Unknown,     SoRole::Dependency, "FF IV 3D JNI proxy — the port's README says it is not needed"},
    {"libRMS.so",         Engine::Unknown,     SoRole::Dependency, "FF IV 3D helper — the port's README says it is not needed"},
    {"libquack.so",       Engine::Unknown,     SoRole::Dependency, "Fingersoft analytics"},
    {"libapplovin-native-crash-reporter.so",
                          Engine::Unknown,     SoRole::Dependency, "AppLovin crash reporter"},

    // Per-title entry libraries, from the ports that name them.
    {"libctro.so",        Engine::Native,      SoRole::Entry,      "Cut the Rope"},
    {"libctr2.so",        Engine::Native,      SoRole::Entry,      "Cut the Rope 2"},
    {"libmortargame.so",  Engine::Native,      SoRole::Entry,      "Jetpack Joyride"},
    {"libswordigo.so",    Engine::Native,      SoRole::Entry,      "Swordigo"},
    {"libsotn.so",        Engine::Native,      SoRole::Entry,      "Castlevania: Symphony of the Night"},
    {"libmcfandroid.so",  Engine::Native,      SoRole::Entry,      "Adventures of Mana"},
    {"libchrono.so",      Engine::Cocos2dx,    SoRole::Entry,      "Chrono Trigger"},
    {"libcrx.so",         Engine::Native,      SoRole::Entry,      "Chaos Rings III"},
    {"libViewer_GP.so",   Engine::Native,      SoRole::Entry,      "Castle of Illusion"},
    {"libacb.so",         Engine::Native,      SoRole::Entry,      "After Burner Climax"},
    {"libjniproxy.so",    Engine::Native,      SoRole::Entry,      "Final Fantasy Dimensions"},
    {"libchange.so",      Engine::Native,      SoRole::Entry,      "Final Fantasy Dimensions II"},
    {"libff3.so",         Engine::Native,      SoRole::Entry,      "Final Fantasy III 3D"},
    {"libff4.so",         Engine::Native,      SoRole::Entry,      "Final Fantasy IV 3D"},
    {"libff4a.so",        Engine::Native,      SoRole::Entry,      "Final Fantasy IV: The After Years"},
    {"libll1.so",         Engine::Native,      SoRole::Entry,      "Professor Layton: Curious Village HD"},
    {"libll2.so",         Engine::Native,      SoRole::Entry,      "Professor Layton: Pandora's Box HD"},
    {"libll3.so",         Engine::Native,      SoRole::Entry,      "Professor Layton: Lost Future HD"},
    {"libTTapp.so",       Engine::TtFusion,    SoRole::Entry,      "LEGO Star Wars: The Complete Saga"},
    {"libLEGO_Pixel_Mobile.so",
                          Engine::TtFusion,    SoRole::Entry,      "LEGO Ninjago: Shadow of Ronin"},
    {"libLEGO_Black_Mobile.so",
                          Engine::TtFusion,    SoRole::Entry,      "LEGO Batman 3: Beyond Gotham"},
    {"libProject_Douglas_HH.so",
                          Engine::TtFusion,    SoRole::Entry,      "LEGO Star Wars: The Force Awakens"},
    {"libGame.so",        Engine::Native,      SoRole::Entry,      "Rockstar mobile engine (GTA SA / LCS / CTW share this name)"},

    // Source engine. Named so the engine is recognised and reported honestly;
    // there is no path for it here (a Source game is ~30 libraries with the
    // entry point in the launcher wrapper, not in any one of them).
    {"libCore.so",        Engine::AdobeAir,    SoRole::Dependency, "Harman Adobe AIR runtime — the game is ActionScript inside it, not here"},
    {"libysshared.so",    Engine::AdobeAir,    SoRole::Dependency, "Adobe AIR support library"},

    {"libclient.so",      Engine::Source,      SoRole::Dependency, "Source engine client"},
    {"libserver.so",      Engine::Source,      SoRole::Dependency, "Source engine server"},
    {"libengine.so",      Engine::Source,      SoRole::Dependency, "Source engine core"},
};
inline constexpr size_t kSoSignatureCount =
    sizeof(kSoSignatures) / sizeof(kSoSignatures[0]);

// ── Titles ──────────────────────────────────────────────────────────────────
// `support` is the only field that says anything about Viridite. Every other
// field is a fact about the game, and stays true whether or not it runs here.
//
// A row with pkg == nullptr is reference only: that port's README does not
// document a package id, and guessing one would put an unverified string in
// the path that decides how a game is loaded. Those rows are never matched at
// runtime — they are here so the entry library and data requirements have one
// home, and so the compatibility docs are generated from the same list the code
// uses rather than drifting from it.
inline constexpr Title kTitles[] = {
    // ── Runs here ───────────────────────────────────────────────────────────
    {"com.fingersoft.hillclimb", "Hill Climb Racing", "1.67.0 (played) / 1.71.1 (arm64)",
     "libgame.so", Engine::Cocos2dx, Support::Playable, true, /*market=*/1,
     nullptr,
     "Viridite's own reference title — see source/compat/games/game_hillclimb.cpp. "
     "The shop is filled by replaying the game's own Java-side catalogue rather "
     "than by patching addresses, so it is no longer pinned to one build; 1.71.1 "
     "is the version xflipperkast's HCR_NX targets and ships arm64."},

    // ── Loads here, does not run ────────────────────────────────────────────
    {"com.orbital.brainiton", "Brain It On!", "Unity 2018.4.13f1",
     "libmain.so", Engine::UnityIl2cpp, Support::Loads, false, -1,
     nullptr,
     "Viridite's Unity reference title — see docs/BRAIN_IT_ON_FINDINGS.md"},

    // ── Known portable, never run here ──────────────────────────────────────
    // Package ids below are taken from the port's own README, or from an OBB /
    // APK filename it quotes (an OBB is named main.<ver>.<package>.obb, so it
    // carries the package id exactly). Where neither exists, pkg is null.

    // Square Enix — the package id is quoted directly in each README.
    {"com.square_enix.android_googleplay.khuxww",
     "Kingdom Hearts Union \xcf\x87 Dark Road", "5.0.1",
     "libcocos2dcpp.so", Engine::Cocos2dx, Support::Untested, false, -1,
     "two OBBs alongside the APK (main.76 and patch.87), left packed",
     "https://github.com/NaGaa95/KHUx_nx", /*needsObb=*/true},
    {"com.square_enix.adventures", "Adventures of Mana", "1.1.4",
     "libmcfandroid.so", Engine::Native, Support::Untested, false, -1,
     "assets/sk1/*.mpk plus the bgm*.ogg set",
     "https://github.com/NaGaa95/aom_nx"},
    {"com.square_enix.android_googleplay.chrono", "Chrono Trigger", "2.1.5",
     "libchrono.so", Engine::Cocos2dx, Support::Untested, false, -1,
     "the APK's whole assets/ tree (resources.bin, 001-008.dat, Shaders/)",
     "https://github.com/NaGaa95/ct_nx"},
    {"com.square_enix.android_googleplay.FFIII_GP", "Final Fantasy III (3D)", "2.0.6",
     "libff3.so", Engine::Native, Support::Untested, false, -1,
     "main.obb from the APK's assets",
     "https://github.com/NaGaa95/ff3_3d_nx"},
    {"com.square_enix.android_googleplay.FFIV_GP", "Final Fantasy IV (3D)", "2.0.5",
     "libff4.so", Engine::Native, Support::Untested, false, -1,
     "main.obb from the APK's assets",
     "https://github.com/NaGaa95/ff4_3d_nx"},
    {"com.square_enix.android_googleplay.FF4AY_GP", "Final Fantasy IV: The After Years", "1.0.13",
     "libff4a.so", Engine::Native, Support::Untested, false, -1,
     "main.obb from the APK's assets",
     "https://github.com/NaGaa95/ff4tay_nx"},
    {"com.square_enix.android_googleplay.ffl_gp", "Final Fantasy Dimensions", "1.1.9",
     "libjniproxy.so", Engine::Native, Support::Untested, false, -1,
     "main.obb plus res/raw art",
     "https://github.com/NaGaa95/ffd_nx"},
    {"com.square_enix.android_googleplay.ffl2w", "Final Fantasy Dimensions II", "1.0.7",
     "libchange.so", Engine::Native, Support::Untested, false, -1,
     "main.obb (movies in res/raw are optional)",
     "https://github.com/NaGaa95/ffd2_nx"},
    {"com.square_enix.chaosrings3gp", "Chaos Rings III", "1.1.4",
     "libcrx.so", Engine::Native, Support::Untested, false, -1,
     "the .mvgl archives (main/movies/se/voice/bgm) and the F000*.bin fonts",
     "https://github.com/NaGaa95/cr3_nx"},

    // SEGA / Disney — package id quoted in the README or carried by the OBB name.
    {"com.disney.castleofillusion_goo", "Castle of Illusion", "1.4.5",
     "libViewer_GP.so", Engine::Native, Support::Untested, false, -1,
     "main.154.<package>.obb, left packed",
     "https://github.com/NaGaa95/coi_nx", /*needsObb=*/true},
    {"com.sega.afterburnerclimax", "After Burner Climax", "0.1.7",
     "libacb.so", Engine::Native, Support::Untested, false, -1,
     "main.<ver>.<package>.obb — note the game also ships libfmod/libfmodstudio, "
     "either of which can be larger than the game itself",
     "https://github.com/NaGaa95/abc_nx", /*needsObb=*/true},

    // WB / TT Games — package ids quoted in the READMEs.
    {"com.wb.goog.lnjgo", "LEGO Ninjago: Shadow of Ronin", "2.2.1.02",
     "libLEGO_Pixel_Mobile.so", Engine::TtFusion, Support::Untested, false, -1,
     "the .fib packs plus music/ and cutscenes/",
     "https://github.com/NaGaa95/lnsor_nx"},
    {"com.wb.goog.lbbg", "LEGO Batman 3: Beyond Gotham", "2.2.1.05",
     "libLEGO_Black_Mobile.so", Engine::TtFusion, Support::Untested, false, -1,
     "the data01 and data02 archives",
     "https://github.com/NaGaa95/lbbg_nx"},
    {"com.wb.goog.legoswtfa", "LEGO Star Wars: The Force Awakens", "2.2.1.06",
     "libProject_Douglas_HH.so", Engine::TtFusion, Support::Untested, false, -1,
     "assetpack1-3 unpacked into a gamedata/ tree",
     "https://github.com/NaGaa95/lswtfa_nx"},

    // Unity IL2CPP titles. All three share libmain/libunity/libil2cpp, so the
    // package id is the only thing that tells them apart.
    {"eu.bandainamcoent.verylittlenightmares", "Very Little Nightmares", "1.2.6",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, false, -1,
     "assets/bin/Data; the main.147.<package>.obb is optional",
     "https://github.com/NaGaa95/vln_nx"},
    {"com.rovio.abcasual", "Angry Birds Journey", "3.8.4",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, false, -1,
     "assets/bin/Data",
     "https://github.com/NaGaa95/angrybirdsjourney_nx"},
    {"com.Level_5.MysteryRoomENG", "Layton Brothers: Mystery Room", "1.1.3",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, false, -1,
     "assets/bin/Data merged from the base APK and the UnityDataAssetPack",
     "https://github.com/NaGaa95/laytonbmr_nx"},

    // ── Reference only: the port does not document a package id ─────────────
    {nullptr, "Subway Surfers", "3.66.1",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, true, -1,
     "assets/bin/Data; maps stick and d-pad to synthetic swipes",
     "https://github.com/NaGaa95/subwaysurfers_nx"},
    {nullptr, "Geometry Dash", "2.2.14x",
     "libcocos2dcpp.so", Engine::Cocos2dx, Support::Untested, false, -1,
     "the APK's assets/ tree plus libfmod.so; online play needs a cacert.pem "
     "regenerated from the Switch trust store",
     "https://github.com/NaGaa95/gdash_nx"},
    {nullptr, "Animal Crossing: Pocket Camp Complete", "7.1.3",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, false, -1,
     "assets/bin/Data; it also ships libTone.so, and still reaches the network "
     "to download content even though normal play doesn't need a connection",
     "https://github.com/NaGaa95/acpc_nx"},
    {nullptr, "Max Payne Mobile", "2.1.131",
     "libGame.so", Engine::Native, Support::Untested, false, -1,
     "the .msf sound files, x_*.ras language archives, data/ and es2/ — all in "
     "the APK's assets, no OBB",
     "https://github.com/NaGaa95/max_nx_v2.1.131"},
    {nullptr, "Angry Birds 2", "26.4.3",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, false, -1,
     "assets/bin/Data",
     "https://github.com/NaGaa95/angrybirds2_nx"},
    {nullptr, "Cut the Rope", "3.79.0",
     "libctro.so", Engine::Native, Support::Untested, false, -1,
     "the APK's assets/ and res/ trees",
     "https://github.com/NaGaa95/ctr_nx"},
    {nullptr, "Cut the Rope 2", "1.47.1",
     "libctr2.so", Engine::Native, Support::Untested, false, -1,
     "the APK's assets/ and res/ trees (the MP4 movies matter)",
     "https://github.com/NaGaa95/ctr2_nx"},
    {nullptr, "Jetpack Joyride", "1.104.1",
     "libmortargame.so", Engine::Native, Support::Untested, false, -1,
     "assets/assets.zip",
     "https://github.com/NaGaa95/jetpackjoyride_nx"},
    {nullptr, "Swordigo", "1.4.12",
     "libswordigo.so", Engine::Native, Support::Untested, false, -1,
     "assets/resources plus the res/*.mp3 music",
     "https://github.com/NaGaa95/swordigo_nx"},
    {nullptr, "Castlevania: Symphony of the Night", "1.0.6",
     "libsotn.so", Engine::Native, Support::Untested, false, -1,
     "assets/res2 — a zip wrapping an OBB that is itself a zip, unpacked twice",
     "https://github.com/NaGaa95/sotn_nx"},
    {nullptr, "LEGO Star Wars: The Complete Saga", "2.0.2.02",
     "libTTapp.so", Engine::TtFusion, Support::Untested, false, -1,
     "Audio.dat, Levels.dat, Others.dat, Textures.dat",
     "https://github.com/NaGaa95/lswtcs_nx"},
    {nullptr, "Professor Layton: Curious Village HD", "1.0.8",
     "libll1.so", Engine::Native, Support::Untested, false, -1,
     "the assets/ language trees (data, data-en, data-EU, ...)",
     "https://github.com/NaGaa95/layton_nx"},
    {nullptr, "Professor Layton: Pandora's Box HD", "1.0.6",
     "libll2.so", Engine::Native, Support::Untested, false, -1,
     "the assets/ language trees",
     "https://github.com/NaGaa95/layton2_nx"},
    {nullptr, "Professor Layton: Lost Future HD", "1.0.3",
     "libll3.so", Engine::Native, Support::Untested, false, -1,
     "the assets/ language trees",
     "https://github.com/NaGaa95/layton3_nx"},
    {nullptr, "GTA: San Andreas", "2.11.311",
     "libGame.so", Engine::Native, Support::Untested, false, -1,
     "main.*.obb unpacked into data/models/texdb/audio/text/anim/es2",
     "https://github.com/NaGaa95/gtasa_nx", /*needsObb=*/true},
    {nullptr, "GTA: Liberty City Stories", "2.4.379",
     "libGame.so", Engine::Native, Support::Untested, false, -1,
     "data_main.wad, data_music.wad, intro.m4v",
     "https://github.com/NaGaa95/gtalcs_nx"},
    {nullptr, "GTA: Chinatown Wars", "4.4.243",
     "libGame.so", Engine::Native, Support::Untested, false, -1,
     "game.pak, dxt.bin, the .gxt text and .mp3 music from assets/",
     "https://github.com/NaGaa95/gtactw_nx"},

    {nullptr, "Angry Birds Epic All Stars", "1.2.8.1",
     "libmain.so", Engine::UnityIl2cpp, Support::Untested, false, -1,
     "the APK is kept whole and unpacked on first launch",
     "https://github.com/xflipperkast/angrybirdsas_nx"},
    {nullptr, "Plants vs. Zombies 2", "13.3.1",
     nullptr, Engine::Native, Support::Untested, false, -1,
     "ships as an XAPK, unpacked on first launch",
     "https://github.com/xflipperkast/PVZ2_NX"},
    {nullptr, "Plants vs. Zombies 2: Reflourished", "1.4.2",
     nullptr, Engine::Native, Support::Untested, false, -1,
     "the modded APK, unpacked on first launch",
     "https://github.com/xflipperkast/PVZ2RF_NX"},

    // ── Engine Viridite has no path for ─────────────────────────────────────
    // Source games are ~30 libraries with no single game binary to enter, and
    // the Android builds are themselves a port (nillerusr's) rather than a
    // stock NDK game. Listed so they are recognised and refused with a reason
    // rather than loaded until something faults.
    // Adobe AIR: the .so files are the AIR runtime, and the game itself is
    // ActionScript bytecode inside a signed SWF. A title like this looks like
    // an NDK game from the outside — it has native libraries — and there is no
    // native game code in it to enter. xflipperkast's ports run it by writing
    // an AVM2 interpreter, which is a different project from this one.
    {nullptr, "slither.io", "3.06",
     nullptr, Engine::AdobeAir, Support::Unsupported, false, -1,
     "libCore.so + libysshared.so and a signed SWF; online play",
     "https://github.com/xflipperkast/slither_nx"},
    {nullptr, "Papa's Pizzeria To Go", "1.1.6",
     nullptr, Engine::AdobeAir, Support::Unsupported, false, -1,
     "a SWF run by an AVM2 interpreter rather than the Android AIR runtime",
     "https://github.com/xflipperkast/papapizzatg_nx"},

    // Dalvik: the game's logic is DEX, executed by the Android runtime. Viridite
    // runs no Java bytecode at all — the same fact that left Hill Climb Racing's
    // shop empty, except here it is the whole game rather than one screen.
    {nullptr, "Pou", "1.4.135",
     nullptr, Engine::Dalvik, Support::Unsupported, false, -1,
     "the APK's DEX, which needs a Dalvik runtime to execute",
     "https://github.com/xflipperkast/pou_nx"},

    {nullptr, "Half-Life 2", "1.16.29 - 1.17.0025",
     nullptr, Engine::Source, Support::Unsupported, false, -1,
     "VPK game data plus ~27 engine libraries",
     "https://github.com/NaGaa95/hl2_nx"},
    {nullptr, "Counter-Strike: Source", "1.09_96",
     nullptr, Engine::Source, Support::Unsupported, false, -1,
     "VPK game data plus the Source engine libraries",
     "https://github.com/NaGaa95/css_nx"},
    {nullptr, "Team Fortress 2 ReClassic", "1.0.0",
     nullptr, Engine::Source, Support::Unsupported, false, -1,
     "VPK game data plus ~29 engine libraries",
     "https://github.com/NaGaa95/tf2_nx"},
};
inline constexpr size_t kTitleCount = sizeof(kTitles) / sizeof(kTitles[0]);

// ── Lookups ─────────────────────────────────────────────────────────────────

// The title for a package id, or nullptr. Rows without a documented package id
// are skipped — they can't be identified from a package, and matching them on
// anything looser would attach one game's data requirements to another's.
inline const Title* findByPackage(const char* pkg) {
    if (!pkg || !*pkg) return nullptr;
    for (size_t i = 0; i < kTitleCount; i++)
        if (kTitles[i].pkg && strcmp(kTitles[i].pkg, pkg) == 0) return &kTitles[i];
    return nullptr;
}

// What a library is, by name, or nullptr if it isn't a name we know.
inline const SoSignature* findSo(const char* soname) {
    if (!soname || !*soname) return nullptr;
    for (size_t i = 0; i < kSoSignatureCount; i++)
        if (strcmp(kSoSignatures[i].soname, soname) == 0) return &kSoSignatures[i];
    return nullptr;
}

// True if this library is known to be something other than the game — an audio
// backend, a C++ runtime, an ad SDK. Used to keep the "largest .so is the game"
// fallback from entering FMOD.
inline bool isKnownDependency(const char* soname) {
    const SoSignature* s = findSo(soname);
    return s && s->role == SoRole::Dependency;
}

inline const char* engineName(Engine e) {
    switch (e) {
        case Engine::Cocos2dx:    return "cocos2d-x";
        case Engine::UnityIl2cpp: return "Unity IL2CPP";
        case Engine::TtFusion:    return "TT Fusion";
        case Engine::Source:      return "Source";
        case Engine::AdobeAir:    return "Adobe AIR / ActionScript";
        case Engine::Dalvik:      return "Dalvik (DEX bytecode)";
        case Engine::Native:      return "native (own engine)";
        default:                  return "unknown";
    }
}

// Short tag for a list row. Says what is known, never more.
inline const char* supportTag(Support s) {
    switch (s) {
        case Support::Playable:    return "PLAYABLE";
        case Support::Loads:       return "LOADS, DOESN'T RUN";
        case Support::Untested:    return "UNTESTED";
        default:                   return "ENGINE UNSUPPORTED";
    }
}


// ── Entry-library selection ─────────────────────────────────────────────────
// Which of a game's libraries is the game. The Core's original rule was "the
// largest .so", which is right for a game that ships one big binary and a
// couple of small helpers — and wrong for several titles above: a Unity game's
// largest library is libil2cpp.so while the entry point is libmain.so, and
// After Burner Climax ships two FMOD libraries either of which can outweigh
// libacb.so. So the size heuristic is kept, but only as the last of four
// answers, each of which is checked against something known rather than
// assumed:
//
//   1. the entry library this exact package documents,
//   2. the largest library whose *name* is a known engine entry point,
//   3. the largest library not known to be a dependency (FMOD, libc++_shared,
//      an ad SDK),
//   4. the largest library, i.e. what the Core did before.
//
// `sonames` are basenames ("libgame.so"), parallel to `sizes`, `n` long. `pkg`
// may be null or unknown. Returns an index into those arrays, or -1 for an
// empty set. `why` (optional) is set to which of the four rules answered.
inline int chooseEntrySo(const char* pkg, const char* const* sonames,
                         const size_t* sizes, size_t n, const char** why = nullptr) {
    auto answer = [&](int idx, const char* reason) {
        if (why) *why = reason;
        return idx;
    };
    if (!sonames || !sizes || n == 0) return answer(-1, "no libraries");

    // 1. The package's own documented entry library.
    if (const Title* t = findByPackage(pkg)) {
        if (t->entrySo) {
            for (size_t i = 0; i < n; i++)
                if (sonames[i] && strcmp(sonames[i], t->entrySo) == 0)
                    return answer((int)i, "documented entry library for this title");
        }
    }

    // 2. Largest library that is a known engine entry point by name.
    int best = -1;
    for (size_t i = 0; i < n; i++) {
        const SoSignature* s = findSo(sonames[i]);
        if (!s || s->role != SoRole::Entry) continue;
        if (best < 0 || sizes[i] > sizes[best]) best = (int)i;
    }
    if (best >= 0) return answer(best, "known engine entry point");

    // 3. Largest library not known to be a dependency.
    for (size_t i = 0; i < n; i++) {
        if (isKnownDependency(sonames[i])) continue;
        if (best < 0 || sizes[i] > sizes[best]) best = (int)i;
    }
    if (best >= 0) return answer(best, "largest library that isn't a known dependency");

    // 4. Largest, as before. Reached only when every library present is a known
    //    dependency, which means the set is wrong rather than the rule.
    best = 0;
    for (size_t i = 1; i < n; i++) if (sizes[i] > sizes[best]) best = (int)i;
    return answer(best, "largest library (every library here looks like a dependency)");
}

}  // namespace gamedb
