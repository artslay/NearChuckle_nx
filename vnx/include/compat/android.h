#pragma once
#include <stdint.h>
#include <EGL/egl.h>
#include "jni.h"

// ─── ANativeWindow ───────────────────────────────────────────────────────────
// Our fake ANativeWindow starts with a NWindow so switch-mesa's EGL accepts it
// as an EGLNativeWindowType directly.
#include <switch.h>
struct ANativeWindow {
    NWindow* nwin;  // pointer to libnx NWindow; switch-mesa EGL accepts this directly
    int32_t  width;
    int32_t  height;
    int32_t  format; // WINDOW_FORMAT_RGBA_8888 = 1
};

// ─── ARect ───────────────────────────────────────────────────────────────────
typedef struct { int32_t left, top, right, bottom; } ARect;

// ─── AAssetManager / AAsset ──────────────────────────────────────────────────
struct AAssetManager { char base_path[512]; };
struct AAssetDir { int dummy; };  // opaque stub
struct AAsset {
    FILE*   fp;
    int64_t size;
    int64_t pos;
    char    path[512];
};

#define AASSET_MODE_UNKNOWN   0
#define AASSET_MODE_RANDOM    1
#define AASSET_MODE_STREAMING 2
#define AASSET_MODE_BUFFER    3
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ─── ALooper ─────────────────────────────────────────────────────────────────
struct ALooper { int dummy; };
#define ALOOPER_POLL_WAKE    (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_POLL_ERROR    (-4)
#define ALOOPER_EVENT_INPUT  1
#define ALOOPER_EVENT_OUTPUT 2
#define ALOOPER_EVENT_HANGUP 4
#define ALOOPER_EVENT_ERROR  8
#define ALOOPER_PREPARE_ALLOW_NON_CALLBACKS 1

// ─── AInputQueue / AInputEvent ───────────────────────────────────────────────
struct AInputQueue { int dummy; };
struct AInputEvent { int32_t type; int32_t action; float x, y; };
#define AINPUT_EVENT_TYPE_KEY    1
#define AINPUT_EVENT_TYPE_MOTION 2
#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP   1
#define AMOTION_EVENT_ACTION_MOVE 2

// ─── ANativeActivityCallbacks ─────────────────────────────────────────────────
struct ANativeActivity;
typedef struct ANativeActivityCallbacks {
    void (*onStart)             (ANativeActivity*);
    void (*onResume)            (ANativeActivity*);
    void* (*onSaveInstanceState)(ANativeActivity*, size_t*);
    void (*onPause)             (ANativeActivity*);
    void (*onStop)              (ANativeActivity*);
    void (*onDestroy)           (ANativeActivity*);
    void (*onWindowFocusChanged)(ANativeActivity*, int hasFocus);
    void (*onNativeWindowCreated)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowResized)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowDestroyed)(ANativeActivity*, ANativeWindow*);
    void (*onInputQueueCreated) (ANativeActivity*, AInputQueue*);
    void (*onInputQueueDestroyed)(ANativeActivity*, AInputQueue*);
    void (*onContentRectChanged)(ANativeActivity*, const ARect*);
    void (*onConfigurationChanged)(ANativeActivity*);
    void (*onLowMemory)         (ANativeActivity*);
} ANativeActivityCallbacks;

// ─── ANativeActivity ─────────────────────────────────────────────────────────
typedef struct ANativeActivity {
    ANativeActivityCallbacks* callbacks;
    JavaVM*    vm;
    JNIEnv*    env;
    void*      clazz;  // jobject — the NativeActivity Java object
    const char* internalDataPath;
    const char* externalDataPath;
    int32_t    sdkVersion;
    void*      instance;  // user-defined state pointer
    AAssetManager* assetManager;
    const char* obbPath;
    ANativeWindow* window;
} ANativeActivity;
