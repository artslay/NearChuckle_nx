#pragma once
// ─── Google Play Games achievements, on a console with no Google ────────────
//
// These games already have an achievement system: they ship Google Play Games
// Services and call Games.Achievements.unlock() at exactly the right moments,
// because their developers wired it up. Nothing here invents achievements or
// decides when they fire — the game does. What is missing on a Switch is
// everything on the other side of that call: no Play Services, no signed-in
// account, no server. So the JNI shim answers the call, and this owns what
// happens next.
//
// Three pieces, deliberately split by who can do what:
//
//   * The **game** supplies the moment. It calls unlock() with a GPGS id, an
//     opaque string like "CgkI3-jvspEBEAIQAg". That id is all it ever tells us.
//   * The **launcher** supplies the meaning. It has network and libcurl, and
//     before handing off it writes a catalogue (name, description, points,
//     rarity, icon) next to the game, scraped from Exophase's Google Play
//     listing. The Core never makes an HTTP request while a game is running.
//   * The **Core** — this — supplies the memory and the moment on screen: it
//     records the unlock, refuses to fire the same one twice, and hands the
//     render loop a toast to draw.
//
// If the catalogue is missing, or the id cannot be matched to an entry in it,
// the unlock is still recorded and still toasted — with the id in place of a
// name. An achievement the player earned must never be silently dropped
// because we could not look up what it was called.

#include <stdint.h>

namespace achievements {

// Open the store for one package. Reads the recorded unlocks and, if the
// launcher left one, the catalogue and id map. Safe to call once per run;
// never fails in a way the caller must handle — a game with no achievement
// data simply gets an empty store.
void init(const char* pkg);

// ── What the JNI shim calls ─────────────────────────────────────────────────
//
// These mirror the GPGS surface. Each returns true if this call *changed*
// anything, which is what the shim reports back to the game as success.

// Games.Achievements.unlock / unlockImmediate.
bool unlock(const char* gpgsId);

// Games.Achievements.increment / incrementImmediate — advances a stepped
// achievement by `steps` and unlocks it when it reaches its total.
bool increment(const char* gpgsId, int steps);

// Games.Achievements.setSteps / setStepsImmediate — absolute, not relative.
// Per GPGS semantics this is ignored if it would move the count backwards.
bool setSteps(const char* gpgsId, int steps);

// Games.Achievements.reveal — a secret becoming visible is not an unlock.
bool reveal(const char* gpgsId);

// True once the id has been unlocked in this or any previous session.
bool isUnlocked(const char* gpgsId);

// ── Totals, for a summary screen ────────────────────────────────────────────

int  unlockedCount(void);
int  totalCount(void);          // 0 when there is no catalogue to count against

// Write the store back to the card. Called on a change (cheap — the file is a
// few hundred bytes) and again at shutdown, because a game that is killed by
// the HOME button never gets an orderly exit and its unlocks must survive it.
void flush(void);

}  // namespace achievements
