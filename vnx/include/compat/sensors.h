#pragma once
#include <cstddef>
#include <cstdint>

// Android NDK Sensor API (android/sensor.h) shims — see source/compat/sensors.cpp.
// Declared with generic pointer types here so shim_table.cpp can reference
// them without needing the full ASensorEvent/etc. struct definitions (those
// live in sensors.cpp, matching Android's exact real ABI layout internally).
// Release the console's SevenSixAxisSensor if it was ever started. libnx
// requires it to be finalized before hidExit, and it is process-wide rather
// than per-queue, so this is called once on the way out rather than when a
// game's event queue goes away.
void sensorsShutdown(void);

extern "C" {
void* ASensorManager_getInstance(void);
void* ASensorManager_getInstanceForPackage(const char* packageName);
int   ASensorManager_getSensorList(void* manager, void* list);
void* ASensorManager_getDefaultSensor(void* manager, int type);
void* ASensorManager_createEventQueue(void* manager, void* looper, int ident, void* callback, void* data);
int   ASensorManager_destroyEventQueue(void* manager, void* queue);
int   ASensorEventQueue_enableSensor(void* queue, void const* sensor);
int   ASensorEventQueue_disableSensor(void* queue, void const* sensor);
int   ASensorEventQueue_setEventRate(void* queue, void const* sensor, int32_t usec);
long  ASensorEventQueue_getEvents(void* queue, void* events, size_t count);
int         ASensor_getType(void const* sensor);
const char* ASensor_getName(void const* sensor);
const char* ASensor_getVendor(void const* sensor);
float       ASensor_getResolution(void const* sensor);
int         ASensor_getMinDelay(void const* sensor);
}
