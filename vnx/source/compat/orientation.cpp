#include "compat/orientation.h"
#include "compat/loader.h"

#include <switch.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Portrait content is drawn at 9:16 — the shape virtually every portrait
// Android game is authored for. Anything taller would waste more of the panel
// than it gained.
constexpr float kPortraitAspect = 9.0f / 16.0f;

GameOrient   g_want = GameOrient::Unspecified;
Presentation g_pres;
bool         g_have_sensor = false;
HidSixAxisSensorHandle g_sensors[2] = {};
char         g_desc[128] = "landscape";

// Last orientation the sensor was confident about. Held across polls where no
// sensor can speak for the console, so the picture stays put rather than
// snapping back to landscape the moment a Joy-Con is detached.
bool g_phys_portrait = false;

bool docked(void) {
    return appletGetOperationMode() == AppletOperationMode_Console;
}

// Joy-Cons attached to the console show up as the Handheld npad. When they are
// detached this style disappears even though the same controllers are still
// connected as JoyDual/JoyLeft/JoyRight.
bool joyconsAttached(void) {
    return (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) != 0;
}

// True only when a sensor exists that is physically part of the console — i.e.
// one whose orientation *is* the console's. See the header for why that is a
// narrower set than "a sensor exists".
bool consoleSensorUsable(void) {
    return g_have_sensor && joyconsAttached();
}

// Read gravity and decide which edge is down. Hysteresis is deliberately wide:
// a console held at an angle should not flicker between orientations, and the
// cost of being slightly slow to rotate is far lower than the cost of
// oscillating.
void pollSensor(void) {
    if (!consoleSensorUsable()) return;

    HidSixAxisSensorState st = {};
    if (hidGetSixAxisSensorStates(g_sensors[0], &st, 1) == 0) return;

    // Held normally, gravity is along -y in the sensor's frame; turned onto its
    // side it moves into x. Compare magnitudes rather than testing an angle, so
    // the console being tipped forwards or backwards doesn't matter.
    float ax = fabsf(st.acceleration.x);
    float ay = fabsf(st.acceleration.y);

    if (!g_phys_portrait && ax > ay * 1.6f)      g_phys_portrait = true;
    else if (g_phys_portrait && ay > ax * 1.6f)  g_phys_portrait = false;
}

void describe(const char* rule, const char* shape) {
    snprintf(g_desc, sizeof(g_desc), "%s → %s%s", rule, shape,
             g_pres.pillarboxed ? " (pillarboxed)" : "");
}

}  // namespace

void orientInit(GameOrient want) {
    g_want = want;

    // Grab the handheld sensor handles up front. These are the Joy-Cons docked
    // into the console (or, on a Lite, its built-in sensor) — the only ones
    // whose orientation means anything about the screen.
    // The npad has to be configured before its sensors can be asked for, and
    // SDL only sets up what it needs for buttons. Ask for the handheld and
    // dual styles explicitly; this is idempotent and does not disturb SDL's
    // own reading of the same pads.
    hidSetSupportedNpadStyleSet(HidNpadStyleTag_NpadHandheld |
                                HidNpadStyleTag_NpadJoyDual |
                                HidNpadStyleTag_NpadFullKey);
    const HidNpadIdType ids[] = { HidNpadIdType_Handheld, HidNpadIdType_No1 };
    hidSetSupportedNpadIdType(ids, 2);

    // libnx validates the (id, style, count) triple and rejects mismatches with
    // BadInput — which is what the first attempt got, so the count it wants is
    // not the one that seemed obvious. Try the plausible shapes rather than
    // guessing: handheld carries two Joy-Cons, but the API may only hand back
    // one handle for the pair.
    struct Attempt { s32 count; HidNpadIdType id; HidNpadStyleTag style; const char* what; };
    static const Attempt kAttempts[] = {
        {2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, "handheld x2"},
        {1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, "handheld x1"},
        {2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual,  "joydual x2"},
        {1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey,  "fullkey x1"},
    };
    for (const Attempt& a : kAttempts) {
        Result rc = hidGetSixAxisSensorHandles(g_sensors, a.count, a.id, a.style);
        if (R_FAILED(rc)) {
            compatLogFmt("orientation: sensor handles (%s) rc=0x%x", a.what, rc);
            continue;
        }
        Result rs = hidStartSixAxisSensor(g_sensors[0]);
        if (R_FAILED(rs)) {
            compatLogFmt("orientation: sensor start (%s) rc=0x%x", a.what, rs);
            continue;
        }
        if (a.count > 1) hidStartSixAxisSensor(g_sensors[1]);
        g_have_sensor = true;
        compatLogFmt("orientation: sensor acquired (%s)", a.what);
        break;
    }

    const char* w = "unspecified";
    switch (want) {
        case GameOrient::Landscape:       w = "landscape";       break;
        case GameOrient::Portrait:        w = "portrait";        break;
        case GameOrient::Sensor:          w = "sensor";          break;
        case GameOrient::SensorLandscape: w = "sensorLandscape"; break;
        case GameOrient::SensorPortrait:  w = "sensorPortrait";  break;
        default: break;
    }
    compatLogFmt("orientation: game wants %s; console sensor %s", w,
                 g_have_sensor ? "available" : "absent");

    orientUpdate();
}

bool orientUpdate(void) {
    Presentation next;

    // The panel is landscape in both modes; only the resolution differs.
    next.screen_w = docked() ? 1920 : 1280;
    next.screen_h = docked() ? 1080 : 720;

    pollSensor();

    // Does the *picture* get to rotate? Only when nothing is bolted to the
    // console telling us how it is meant to be held.
    const bool lockLandscape = docked() || joyconsAttached();

    // What shape does the game's content want to be?
    bool wantPortrait = false;
    switch (g_want) {
        case GameOrient::Portrait:
        case GameOrient::SensorPortrait:
            wantPortrait = true;
            break;
        case GameOrient::Sensor:
        case GameOrient::Unspecified:
            // No fixed preference — follow the console if it can be sensed,
            // otherwise stay landscape, which is the shape of the panel.
            wantPortrait = !lockLandscape && consoleSensorUsable() && g_phys_portrait;
            break;
        default:  // Landscape, SensorLandscape
            wantPortrait = false;
            break;
    }

    const char* rule = docked()          ? "docked"
                     : joyconsAttached() ? "handheld, Joy-Cons attached"
                                         : "handheld, Joy-Cons detached";

    if (!wantPortrait) {
        next.content_x = 0; next.content_y = 0;
        next.content_w = next.screen_w; next.content_h = next.screen_h;
        next.transform = 0;
        next.pillarboxed = false;
    } else if (lockLandscape || !consoleSensorUsable()) {
        // Portrait content on a screen that is staying landscape: draw it at
        // its own aspect down the middle. Stretching a portrait game across a
        // landscape panel is the one thing that is never right.
        next.content_h = next.screen_h;
        next.content_w = (int)(next.screen_h * kPortraitAspect + 0.5f);
        next.content_x = (next.screen_w - next.content_w) / 2;
        next.content_y = 0;
        next.transform = 0;
        next.pillarboxed = true;
    } else {
        // The console is genuinely being held upright and can prove it, so turn
        // the whole surface over to the compositor rotated. The game keeps
        // rendering to a portrait-shaped buffer and never learns about any of
        // this.
        next.content_x = 0; next.content_y = 0;
        next.content_w = next.screen_h;   // swapped: the buffer is portrait
        next.content_h = next.screen_w;
        next.transform = NATIVE_WINDOW_TRANSFORM_ROT_90;
        next.pillarboxed = false;
    }

    const bool changed = memcmp(&next, &g_pres, sizeof(Presentation)) != 0;
    g_pres = next;
    if (changed) {
        describe(rule, wantPortrait ? "portrait" : "landscape");
        compatLogFmt("orientation: %s — content %dx%d at (%d,%d) on %dx%d%s",
                     g_desc, g_pres.content_w, g_pres.content_h,
                     g_pres.content_x, g_pres.content_y,
                     g_pres.screen_w, g_pres.screen_h,
                     g_pres.transform ? ", rotated 90°" : "");
    }
    return changed;
}

const Presentation& orientGet(void) { return g_pres; }

void orientMapTouch(int* x, int* y) {
    if (!x || !y) return;

    if (g_pres.transform == NATIVE_WINDOW_TRANSFORM_ROT_90) {
        // The compositor turned the picture; the touch panel did not. Undo the
        // rotation so a tap lands where the player saw the thing they tapped.
        int sx = *x, sy = *y;
        *x = sy;
        *y = g_pres.screen_w - 1 - sx;
        return;
    }

    // Pillarboxed: shift into the content rect, and report taps on the bars as
    // just outside it rather than clamping them onto the edge, which would
    // make the bars behave like a very tall button.
    *x -= g_pres.content_x;
    *y -= g_pres.content_y;
}

const char* orientDescribe(void) { return g_desc; }
