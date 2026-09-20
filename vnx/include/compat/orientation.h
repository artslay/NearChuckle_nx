#pragma once
// ─── Screen orientation ─────────────────────────────────────────────────────
// Android games declare an orientation and expect the device to honour it; a
// Switch has a fixed landscape panel and, depending on how it is being held,
// either can or cannot know which way up it is. This works out what to show.
//
// The rules, in the order they are applied:
//
//   Docked                      → landscape, always. A TV does not rotate.
//   Handheld, Joy-Cons attached → landscape, same as docked. You are holding
//                                 it like a gamepad; rotating the picture
//                                 would put the controls on the wrong edges.
//   Handheld, Joy-Cons detached → follow the console, like a phone.
//
// A portrait game under a landscape rule is not stretched — it is drawn at its
// own aspect in the middle of the screen with black bars either side, which is
// what "portrait on a landscape display" means everywhere else.
//
// One hardware caveat shapes the last rule. A Switch console has no motion
// sensor of its own: the accelerometers live in the Joy-Cons. While they are
// attached they are rigidly part of the console and report its orientation
// faithfully — but once detached they report their *own* orientation, which
// says nothing about which way the console is being held. So on a standard
// Switch, the one case that asks for auto-rotation is the one case that cannot
// sense it. Rather than rotate the screen because a Joy-Con was set down on a
// table, orientUpdate holds the last known orientation whenever no sensor can
// speak for the console. A Switch Lite has a built-in sensor and does not have
// this problem.

#include <stdint.h>

// What the game asked for in its manifest (android:screenOrientation).
enum class GameOrient {
    Unspecified,      // no preference — treat as landscape, which is the panel
    Landscape,
    Portrait,
    Sensor,           // let the device decide
    SensorLandscape,
    SensorPortrait,
};

// Where the game's pixels go, and how the compositor should turn them.
struct Presentation {
    int      screen_w   = 1280;   // physical framebuffer
    int      screen_h   = 720;
    int      content_x  = 0;      // the rect the game draws into
    int      content_y  = 0;
    int      content_w  = 1280;
    int      content_h  = 720;
    uint32_t transform  = 0;      // NATIVE_WINDOW_TRANSFORM_* (0 = upright)
    bool     pillarboxed = false; // there are black bars to preserve
};

// Call once, after the manifest is parsed and HID is up.
void orientInit(GameOrient want);

// Poll. Returns true when the presentation changed and the caller should
// re-apply it (window dimensions, viewport, touch mapping).
bool orientUpdate(void);

const Presentation& orientGet(void);

// Screen coordinates (what HID reports) to content coordinates (what the game
// believes it is being touched at). Needed whenever the content rect is not
// the whole screen, or the picture is rotated.
void orientMapTouch(int* x, int* y);

// Human-readable, for the log.
const char* orientDescribe(void);
