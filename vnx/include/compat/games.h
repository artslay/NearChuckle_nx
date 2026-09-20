#pragma once
#include <cstdint>
#include <cstddef>
#include "compat/gamedb.h"

// ─── Per-game compatibility profiles ────────────────────────────────────────
// Game-specific fixups live here, isolated from the shared translation layer
// (elf_loader/jni_env/shim_table) and from the Unity runtime, so one title's
// quirks can never collide with another's. Each supported title has its own
// source/compat/games/game_<title>.cpp; this header is the only surface the
// shared code calls into.
//
// What is known *about* each title — its engine, its entry library, the data
// it needs — is separate, in compat/gamedb.h, because that is shared with the
// launcher and holds rows for titles that have no code here at all. This
// header is only the executable side: the fixups, and the dispatcher in
// source/compat/games/game_registry.cpp that routes to them.

// Apply any in-place instruction patches a game needs to a freshly-staged,
// still-writable .so image. Called once per loaded library, before the image is
// made executable. `soname` is the library basename (e.g. "libgame.so");
// `pkg` is the owning package id (e.g. "com.fingersoft.hillclimb") or "" if
// unknown. A no-op for anything without a registered profile.
void gameApplyQuirks(const char* pkg, const char* soname,
                     uint8_t* stage_base, uint64_t min_vaddr, size_t alloc_size);

// The store variation a Cocos2d-x game should believe it is (getMarketVariation;
// 1 = Google Play). Returns the game's value, or -1 if it has no opinion (the
// caller then uses its own default).
int gameMarketVariation(const char* pkg);

// Resolve an exported symbol in the running game. Passed in rather than
// reached for, so the per-title code stays independent of the ELF loader and
// can be exercised on a dev machine with a stub.
typedef void* (*HcrSymResolver)(const char* symbol);

// Give a title whatever initialisation its Java side would have performed
// before the engine starts. Called once, after nativeSetPaths and before
// nativeInit — the same window Android's activity does its own setup in.
// A no-op for anything without one.
void gameBeforeNativeInit(const char* pkg, void* env, void* thiz,
                          HcrSymResolver resolve);

// A file the game expects to exist but that nothing here would have created —
// on Android it is written by a backend fetch or by Java before the game reads
// it. Returns the content to create it with, or nullptr to let the open fail
// normally. Called only after a read-mode open has already failed, so it can
// never shadow a file that is really there.
const char* gameMissingFileContent(const char* pkg, const char* path);

// ─── Per-title hooks ────────────────────────────────────────────────────────
// Implemented in that title's game_<name>.cpp, called only by the dispatcher in
// game_registry.cpp. Declared here rather than in each other's translation
// units so a new title is one file plus one line in the dispatcher.
void hcrApplyQuirks(const char* soname, uint8_t* stage_base,
                    uint64_t min_vaddr, size_t alloc_size);

// Hill Climb Racing's shop catalogue: replays the 69 setInAppItem() calls
// Android's NewBillingHandle.Init() makes before the engine builds the shop.
// Returns true if the product vector was populated. See
// source/compat/games/game_hillclimb_shop.cpp.
bool hcrPopulateShop(void* env, void* thiz, HcrSymResolver resolve);

// Exposed for the host test in test/hcrshop/.
void        hcrFormatShopAmount(char out[24], int value);
size_t      hcrShopCatalogCount(void);
const char* hcrShopCatalogId(size_t index);
