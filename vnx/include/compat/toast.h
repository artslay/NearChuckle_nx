#pragma once
// ─── In-game toast ──────────────────────────────────────────────────────────
//
// Draws a small panel over the running game's own frame — currently only used
// to announce an achievement unlock, which is the one thing that has to be
// visible *while* someone is playing rather than on a screen of ours.
//
// This is the branding-overlay machinery from loader.cpp, generalised. That
// code worked and then had nothing left to say once the branding overlay was
// retired; the GL discipline in it was learned the hard way and is worth more
// than the feature it was written for:
//
//   * Everything it touches is read back and restored bit-for-bit. cocos2d-x
//     keeps its own software cache of GL state and skips calls it believes are
//     redundant. Changing real state behind that cache is what once turned the
//     screen solid black one frame after the overlay appeared.
//   * Vertex attributes 8 and 9, deliberately. Engines use low indices; using
//     ones nothing looks at means our enable/disable cannot desync anybody.
//
// Nothing here allocates or uploads until there is something to show, so a
// session with no unlocks pays one branch per frame.

#include <stdint.h>

namespace toast {

// Queue a panel. `iconPath` may be null. Text is copied; the caller keeps
// ownership of nothing. Safe to call from a JNI thread — the panel is built
// and uploaded later, on the thread that owns the GL context.
void show(const char* title, const char* subtitle, const char* iconPath);

// True while a panel is on screen or waiting to be.
bool active(void);

// Draw the current panel, if any. Call once per frame immediately before the
// buffer swap, from the thread holding the GL context. Cheap and self-
// disabling when there is nothing queued.
void draw(void);

// Drop the GL objects. Call when the game loop ends, before the context goes.
void shutdown(void);

}  // namespace toast
