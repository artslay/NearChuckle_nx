// ─── Android NDK Sensor API (android/sensor.h) — real accelerometer + gyro ───
// Unlike battery status (Java-only on real Android, no pure NDK path), motion
// sensors have a stable, well-documented native ABI that NDK games call
// directly with no JNI involved — so unlike battery, we can implement this
// against the EXACT real API surface rather than guessing at method names.
// Both sensors are backed by the SAME Switch six-axis sensor reading via libnx
// hid — one hidGetSixAxisSensorStates() call per frame feeds both the
// accelerometer and gyroscope events. No current test game (Hill Climb Racing)
// uses either — forward-looking groundwork for a future tilt/motion-control
// game, requested explicitly so "some games" that expect real Android sensor
// behaviour get it.
//
// Which IMU that reading comes from used to be one answer: the handheld
// Joy-Cons. That is wrong in two ways, both raised in issue #5 (thanks to
// @hippydave for the report), and both are about hardware that exists and was
// never asked for:
//
//   1. A Pro Controller, a detached Joy-Con pair, and a single sideways
//      Joy-Con all carry six-axis sensors. Requesting only
//      HidNpadIdType_Handheld meant that with the Joy-Cons off the console —
//      the normal way to play docked, and a perfectly normal way to play
//      handheld on a stand — there were no motion readings at all, from
//      hardware sitting right there reporting them.
//   2. The console itself has an IMU. This is what Labo VR uses for head
//      tracking with the Joy-Cons detached, and libnx exposes it as the
//      SevenSixAxisSensor. It is the last resort here, for when there is no
//      controller with a sensor at all.
//
// So acquisition now walks: handheld → every connected pad → the console.
//
// Known limitation: Android's ASensorEventQueue is normally driven through
// ALooper's fd-based event notification (the queue has a pollable fd; the
// game either polls it directly or gets called back via ALooper_pollAll).
// We don't have a real fd-backed ALooper here, so games that rely on
// ALooper waking them up when new samples arrive won't be notified that way
// — but ASensorEventQueue_getEvents() always returns fresh real data when
// polled directly, which covers games that just poll every frame (the more
// common simple pattern for a tilt-steering control scheme).

#include "compat/loader.h"
#include <switch.h>
#include <cstring>
#include <cstdint>
#include <cstdio>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

// ─── Exact Android NDK ABI layout (android/sensor.h) ─────────────────────────
// Struct shapes/offsets must match real Android exactly — games compiled
// against the real NDK header read these fields directly.
struct ASensorVector {
    union { float v[3]; struct { float x, y, z; }; struct { float azimuth, pitch, roll; }; };
    int8_t status;
    uint8_t reserved[3];
};
struct AMetaDataEvent { int32_t what; int32_t sensor; };
struct AUncalibratedEvent {
    union { float uncalib[3]; struct { float x_uncalib, y_uncalib, z_uncalib; }; };
    union { float bias[3];    struct { float x_bias,    y_bias,    z_bias;    }; };
};
struct AHeartRateEvent { float bpm; int8_t status; };
struct ADynamicSensorEvent { int32_t connected; int32_t handle; };
struct AAdditionalInfoEvent {
    int32_t type;
    int32_t serial;
    union { int32_t data_int32[14]; float data_float[14]; };
};
struct ASensorEvent {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        union {
            float data[16];
            ASensorVector vector;
            ASensorVector acceleration;
            ASensorVector magnetic;
            float temperature;
            float distance;
            float light;
            float pressure;
            float relative_humidity;
            AUncalibratedEvent uncalibrated_gyro;
            AUncalibratedEvent uncalibrated_magnetic;
            AMetaDataEvent meta_data;
            AHeartRateEvent heart_rate;
            ADynamicSensorEvent dynamic_sensor;
            AAdditionalInfoEvent additional_info;
        };
        struct { uint64_t data[8]; } u64;
    };
    int32_t flags;
    int32_t reserved1[3];
};

#define ASENSOR_TYPE_ACCELEROMETER 1
#define ASENSOR_TYPE_GYROSCOPE     4

// Opaque handles — games only ever hold pointers to these, never dereference
// their layout directly, so our own internal shape doesn't need to match Android.
struct ASensorManager { int unused; };
struct ASensor        { int32_t type; };
// Where the readings come from. A controller's sensor reports acceleration and
// angular velocity in documented units; the console's reports a payload libnx
// still describes as ten unnamed floats, which is the whole reason these are
// not interchangeable (see readConsole below).
enum class SensorSource { None, Pad, Console };

struct ASensorEventQueue {
    bool accelEnabled = false;
    bool gyroEnabled  = false;
    SensorSource source = SensorSource::None;
    HidSixAxisSensorHandle handle;   // source == Pad
    const char* sourceName = "none";
};

static ASensorManager g_manager;
static ASensor        g_accelSensor  = { ASENSOR_TYPE_ACCELEROMETER };
static ASensor        g_gyroSensor   = { ASENSOR_TYPE_GYROSCOPE };

// The console's SevenSixAxisSensor is a process-wide resource, not a per-queue
// one: it is initialized once and must be finalized before hidExit.
static bool g_consoleInited = false;

// ─── Console IMU payload mapping ─────────────────────────────────────────────
// libnx declares the console sensor's sample as:
//
//     typedef struct {
//         u64 timestamp0; u64 sampling_number; u64 unk_x10;
//         float unk_x18[10];
//     } HidSevenSixAxisSensorState;
//
// Ten floats, none of them named. The layout is not documented in libnx and is
// not published anywhere this could be checked against, and inventing one would
// mean feeding a game numbers that look like motion and are not — worse than
// reporting nothing, because a tilt control that drifts is indistinguishable
// from a game bug.
//
// So the mapping is a runtime setting rather than a guess in the source. With
// no file, the console sensor logs its raw floats and emits no events; with
// one, it emits from the offsets given. That way identifying the layout is a
// single hardware session with a log — rest the console flat, roll it on each
// axis in turn, read which triple moves — and not a rebuild per hypothesis,
// which is the expensive part of every round trip on this project.
//
//   sdmc:/Viridite/console_imu.txt   one line:  <accel_index> <gyro_index>
//   e.g. "4 7"  → accel = unk_x18[4..6], gyro = unk_x18[7..9]
//
// Indices must leave room for three floats, so each is 0..7.
static const char* CONSOLE_IMU_PATH = "sdmc:/Viridite/console_imu.txt";
static int g_consoleAccelIdx = -1;
static int g_consoleGyroIdx  = -1;

// Re-read every time the console sensor is acquired rather than latched on
// first use. It happens once per event queue, never per frame, so it costs
// nothing — and it means dropping the file onto the SD card and restarting the
// game is enough to try a mapping, without a rebuild.
static void consoleMapLoad() {
    g_consoleAccelIdx = -1;
    g_consoleGyroIdx  = -1;
    FILE* f = fopen(CONSOLE_IMU_PATH, "r");
    if (!f) return;
    int a = -1, g = -1;
    bool ok = fscanf(f, "%d %d", &a, &g) == 2;
    fclose(f);
    if (!ok) { compatLogFmt("sensors: %s unreadable — ignoring", CONSOLE_IMU_PATH); return; }
    // A bad index would read past the end of a ten-float array.
    if (a < 0 || a > 7 || g < 0 || g > 7) {
        compatLogFmt("sensors: console IMU indices %d/%d out of range 0..7 — ignoring", a, g);
        return;
    }
    g_consoleAccelIdx = a;
    g_consoleGyroIdx  = g;
    compatLogFmt("sensors: console IMU mapping accel=unk_x18[%d..%d] gyro=unk_x18[%d..%d]",
                 a, a + 2, g, g + 2);
}

// Try one pad's six-axis sensor. JoyDual is the only style that reports two
// handles (one per Joy-Con); everything else takes exactly one, and asking for
// two is an error rather than a fallback.
static bool tryPad(ASensorEventQueue* q, HidNpadIdType id, HidNpadStyleTag style,
                   const char* what) {
    HidSixAxisSensorHandle handles[2] = {};
    const s32 total = (style == HidNpadStyleTag_NpadJoyDual) ? 2 : 1;
    Result rc = hidGetSixAxisSensorHandles(handles, total, id, style);
    if (R_FAILED(rc)) return false;
    // With a Joy-Con pair, the left one is the reference: a game wants one
    // device's motion, and picking a side consistently beats averaging two
    // hands that are not attached to each other.
    rc = hidStartSixAxisSensor(handles[0]);
    if (R_FAILED(rc)) {
        compatLogFmt("sensors: %s six-axis start failed 0x%x", what, rc);
        return false;
    }
    q->handle     = handles[0];
    q->source     = SensorSource::Pad;
    q->sourceName = what;
    compatLogFmt("sensors: six-axis sensor started (%s)", what);
    return true;
}

// The console's own IMU — the one Labo VR head-tracks with, and the reason
// issue #5 was filed. 5.0.0+; older firmware simply has no such service and
// the initialize call fails, which is not an error worth shouting about.
static bool tryConsole(ASensorEventQueue* q) {
    if (!g_consoleInited) {
        Result rc = hidInitializeSevenSixAxisSensor();
        if (R_FAILED(rc)) {
            compatLogFmt("sensors: console IMU unavailable (init 0x%x)", rc);
            return false;
        }
        g_consoleInited = true;
    }
    Result rc = hidStartSevenSixAxisSensor();
    if (R_FAILED(rc)) {
        compatLogFmt("sensors: console IMU start failed 0x%x", rc);
        return false;
    }
    consoleMapLoad();
    q->source     = SensorSource::Console;
    q->sourceName = "console";
    if (g_consoleAccelIdx < 0)
        compatLogFmt("sensors: console IMU started, but its ten-float payload has no "
                     "mapping yet — logging raw samples instead of guessing. Write "
                     "\"<accel> <gyro>\" to %s once the axes are identified.",
                     CONSOLE_IMU_PATH);
    else
        compatLog("sensors: console IMU started");
    return true;
}

// Everything with a six-axis sensor, best first: whatever is in the player's
// hands, then the console. Called once per queue, when a game first enables a
// sensor.
static bool ensureSixAxisStarted(ASensorEventQueue* q) {
    if (q->source != SensorSource::None) return true;

    if (tryPad(q, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, "handheld"))
        return true;

    // Players 1-8. A pad paired into a later slot is still the pad someone is
    // holding, and its sensor is no less real for it.
    for (int i = 0; i < 8; i++) {
        const HidNpadIdType id = (HidNpadIdType)(HidNpadIdType_No1 + i);
        const u32 st = hidGetNpadStyleSet(id);
        if ((st & HidNpadStyleTag_NpadFullKey) &&
            tryPad(q, id, HidNpadStyleTag_NpadFullKey, "Pro Controller")) return true;
        if ((st & HidNpadStyleTag_NpadJoyDual) &&
            tryPad(q, id, HidNpadStyleTag_NpadJoyDual, "Joy-Con pair")) return true;
        if ((st & HidNpadStyleTag_NpadJoyLeft) &&
            tryPad(q, id, HidNpadStyleTag_NpadJoyLeft, "left Joy-Con")) return true;
        if ((st & HidNpadStyleTag_NpadJoyRight) &&
            tryPad(q, id, HidNpadStyleTag_NpadJoyRight, "right Joy-Con")) return true;
        if ((st & HidNpadStyleTag_NpadHandheld) &&
            tryPad(q, id, HidNpadStyleTag_NpadHandheld, "handheld")) return true;
    }

    // Nothing in anyone's hands. The console has its own.
    if (tryConsole(q)) return true;

    compatLog("sensors: no six-axis sensor available from any pad or the console");
    return false;
}

// The console sensor is initialized process-wide and libnx requires it to be
// finalized before hidExit. A queue being destroyed is not the end of the
// process, so the teardown is here, called once on the way out (see
// sensorsShutdown in compat/sensors.h). Deliberately outside the extern "C"
// block below: this is Viridite's own call, not part of the Android ABI.
void sensorsShutdown(void) {
    if (!g_consoleInited) return;
    hidStopSevenSixAxisSensor();
    hidFinalizeSevenSixAxisSensor();
    g_consoleInited = false;
    compatLog("sensors: console IMU finalized");
}

extern "C" {

ASensorManager* ASensorManager_getInstance(void) {
    return &g_manager;
}
ASensorManager* ASensorManager_getInstanceForPackage(const char*) {
    return &g_manager;
}

int ASensorManager_getSensorList(ASensorManager*, ASensor const*** list) {
    static ASensor const* two[2];
    two[0] = &g_accelSensor;
    two[1] = &g_gyroSensor;
    if (list) *list = two;
    compatLog("sensors: ASensorManager_getSensorList → [accelerometer, gyroscope]");
    return 2;
}

ASensor const* ASensorManager_getDefaultSensor(ASensorManager*, int type) {
    compatLogFmt("sensors: ASensorManager_getDefaultSensor(type=%d)", type);
    if (type == ASENSOR_TYPE_ACCELEROMETER) return &g_accelSensor;
    if (type == ASENSOR_TYPE_GYROSCOPE)     return &g_gyroSensor;
    return nullptr;  // magnetic/etc not wired up yet
}

ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager*, void* /*ALooper*/,
                                                    int /*ident*/, void* /*callback*/, void* /*data*/) {
    compatLog("sensors: ASensorManager_createEventQueue");
    return new ASensorEventQueue();
}

int ASensorManager_destroyEventQueue(ASensorManager*, ASensorEventQueue* q) {
    if (q) {
        if (q->source == SensorSource::Pad)          hidStopSixAxisSensor(q->handle);
        else if (q->source == SensorSource::Console) hidStopSevenSixAxisSensor();
    }
    delete q;
    return 0;
}


int ASensorEventQueue_enableSensor(ASensorEventQueue* q, ASensor const* sensor) {
    if (!q || !sensor) return -1;
    if (!ensureSixAxisStarted(q)) return -1;
    if (sensor->type == ASENSOR_TYPE_GYROSCOPE) {
        q->gyroEnabled = true;
        compatLog("sensors: gyroscope enabled");
    } else {
        q->accelEnabled = true;
        compatLog("sensors: accelerometer enabled");
    }
    return 0;
}

int ASensorEventQueue_disableSensor(ASensorEventQueue* q, ASensor const* sensor) {
    if (!q) return -1;
    if (sensor && sensor->type == ASENSOR_TYPE_GYROSCOPE) q->gyroEnabled = false;
    else                                                  q->accelEnabled = false;
    return 0;
}

int ASensorEventQueue_setEventRate(ASensorEventQueue*, ASensor const*, int32_t) {
    return 0;  // libnx doesn't expose a sample-rate knob here; always full rate
}

// One sample, in Android's units: acceleration in m/s^2, angular velocity in
// rad/s. Returns false when there is nothing to report — including the case
// where the console sensor is the source and its payload has no mapping yet.
static bool readSample(ASensorEventQueue* q, float accel[3], float gyro[3]) {
    // Switch acceleration is in G; Android wants m/s^2. Switch angular
    // velocity is in revolutions/sec; Android wants rad/s.
    const float G = 9.80665f;
    const float TWO_PI = 6.283185307f;

    if (q->source == SensorSource::Pad) {
        HidSixAxisSensorState state = {};
        if (hidGetSixAxisSensorStates(q->handle, &state, 1) == 0) return false;
        accel[0] = state.acceleration.x * G;
        accel[1] = state.acceleration.y * G;
        accel[2] = state.acceleration.z * G;
        gyro[0]  = state.angular_velocity.x * TWO_PI;
        gyro[1]  = state.angular_velocity.y * TWO_PI;
        gyro[2]  = state.angular_velocity.z * TWO_PI;
        return true;
    }

    if (q->source != SensorSource::Console) return false;

    HidSevenSixAxisSensorState state = {};
    size_t total = 0;
    if (R_FAILED(hidGetSevenSixAxisSensorStates(&state, 1, &total)) || total == 0)
        return false;

    // No mapping yet: log what arrived, throttled, so a hardware session can
    // identify the axes — and report nothing, because numbers pulled from
    // unknown offsets would be indistinguishable from a broken sensor.
    if (g_consoleAccelIdx < 0 || g_consoleGyroIdx < 0) {
        static int shown = 0;
        if (shown < 40) {
            shown++;
            bool atRest = false;
            hidIsSevenSixAxisSensorAtRest(&atRest);
            compatLogFmt("sensors: console raw%s "
                         "[0]=%.4f [1]=%.4f [2]=%.4f [3]=%.4f [4]=%.4f "
                         "[5]=%.4f [6]=%.4f [7]=%.4f [8]=%.4f [9]=%.4f",
                         atRest ? " (at rest)" : "",
                         state.unk_x18[0], state.unk_x18[1], state.unk_x18[2],
                         state.unk_x18[3], state.unk_x18[4], state.unk_x18[5],
                         state.unk_x18[6], state.unk_x18[7], state.unk_x18[8],
                         state.unk_x18[9]);
            if (shown == 40)
                compatLogFmt("sensors: (console raw sample log capped; write the two "
                             "indices to %s to start reporting)", CONSOLE_IMU_PATH);
        }
        return false;
    }

    // The console sensor's units are as undocumented as its layout, so the
    // same G/rev-per-sec conversion the pads use is applied — it is the
    // convention every other Switch IMU reading follows, and it is the part
    // the same hardware session confirms or corrects.
    for (int i = 0; i < 3; i++) {
        accel[i] = state.unk_x18[g_consoleAccelIdx + i] * G;
        gyro[i]  = state.unk_x18[g_consoleGyroIdx  + i] * TWO_PI;
    }
    return true;
}

ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* q, ASensorEvent* events, size_t count) {
    if (!q || (!q->accelEnabled && !q->gyroEnabled) || !events || count == 0) return 0;
    float accel[3] = {0, 0, 0}, gyro[3] = {0, 0, 0};
    if (!readSample(q, accel, gyro)) return 0;

    int64_t now = (int64_t)armGetSystemTick();
    size_t n = 0;

    if (q->accelEnabled && n < count) {
        ASensorEvent& ev = events[n++];
        memset(&ev, 0, sizeof(ev));
        ev.version   = sizeof(ASensorEvent);
        ev.sensor    = (int32_t)(intptr_t)&g_accelSensor;
        ev.type      = ASENSOR_TYPE_ACCELEROMETER;
        ev.timestamp = now;
        ev.acceleration.x = accel[0];
        ev.acceleration.y = accel[1];
        ev.acceleration.z = accel[2];
        ev.acceleration.status = 3;  // SENSOR_STATUS_ACCURACY_HIGH
    }

    if (q->gyroEnabled && n < count) {
        ASensorEvent& ev = events[n++];
        memset(&ev, 0, sizeof(ev));
        ev.version   = sizeof(ASensorEvent);
        ev.sensor    = (int32_t)(intptr_t)&g_gyroSensor;
        ev.type      = ASENSOR_TYPE_GYROSCOPE;
        ev.timestamp = now;
        ev.vector.x = gyro[0];
        ev.vector.y = gyro[1];
        ev.vector.z = gyro[2];
        ev.vector.status = 3;  // SENSOR_STATUS_ACCURACY_HIGH
    }

    return (ssize_t)n;
}

int ASensor_getType(ASensor const* s) { return s ? s->type : 0; }
const char* ASensor_getName(ASensor const* s) {
    return (s && s->type == ASENSOR_TYPE_GYROSCOPE) ? "Switch Gyroscope" : "Switch Accelerometer";
}
const char* ASensor_getVendor(ASensor const*) { return "Nintendo"; }
float ASensor_getResolution(ASensor const*) { return 0.0001f; }
int ASensor_getMinDelay(ASensor const*) { return 10000; }  // 10ms ~= 100Hz, matches hid polling

} // extern "C"
