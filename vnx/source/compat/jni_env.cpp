#include "compat/loader.h"
#include "compat/achievements.h"
#include "compat/jni.h"
#include "compat/games.h"
#include <switch.h>
#include <string.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <unordered_map>
#include <string>
#include <unordered_set>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

static void* g_jni_funcs[JNI_NUM_SLOTS] = {};
static void* g_vm_funcs[VM_NUM_SLOTS]   = {};
static void* g_jni_inner = nullptr;
static void* g_vm_inner  = nullptr;
static void* g_jni_outer = nullptr;
static void* g_vm_outer  = nullptr;

static std::vector<JNINativeMethod> g_native_methods;

#define DUMMY_CLASS  ((void*)0x1001)
#define DUMMY_METHOD ((void*)0x2001)
#define DUMMY_FIELD  ((void*)0x3001)

// ─── Method registry ─────────────────────────────────────────────────────────
// Each unique (name, sig) pair gets a stable MethodEntry pointer used as jmethodID.
// g_method_pool holds the actual stable storage (jmethodID == &entry, must
// never move/realloc); g_method_index is a name|sig -> pointer hash map purely
// so repeated lookups don't do a linear scan of a pool that can grow past a
// hundred entries in a real game — GetMethodID/GetStaticMethodID resolution
// stays O(1) regardless of how many distinct methods the game has looked up
// so far, instead of degrading as more accumulate.
struct MethodEntry { char name[80]; char sig[128]; };
static MethodEntry g_method_pool[512];
static int g_method_count = 0;
static std::unordered_map<std::string, MethodEntry*> g_method_index;

// The Android Java object model (Unity first-frame reflection) is Unity-only.
// Declared up here so GetFieldID/GetObjectField can consult it; the model
// stays fully dormant for cocos2d-x games, whose JNI path is unchanged. Set by
// jniSetUnityMode().
static bool g_unity_mode = false;

static MethodEntry* lookupOrCreateMethod(const char* n, const char* sg) {
    std::string key = std::string(n ? n : "") + "|" + (sg ? sg : "");
    auto it = g_method_index.find(key);
    if (it != g_method_index.end()) return it->second;
    if (g_method_count < 512) {
        MethodEntry* e = &g_method_pool[g_method_count++];
        strncpy(e->name, n  ? n  : "", 79);  e->name[79]  = 0;
        strncpy(e->sig,  sg ? sg : "", 127); e->sig[127]  = 0;
        g_method_index.emplace(std::move(key), e);
        return e;
    }
    return nullptr;
}

// ─── UserDefault in-memory store (Cocos2d-x SharedPreferences emulation) ─────
// Game threads are real (see shim_table pt_create) and JNI calls arrive from
// worker threads too — serialize map access.
static std::unordered_map<std::string, int>         g_int_store;
static std::unordered_map<std::string, std::string> g_str_store;
static Mutex g_store_lock;
struct StoreLock {
    StoreLock()  { mutexLock(&g_store_lock); }
    ~StoreLock() { mutexUnlock(&g_store_lock); }
};

// Log-once helper: JNI lookups fire tens of thousands of times during loading
// and every compat log line fsyncs to the SD card — that I/O was the actual
// loading-screen bottleneck (~90 save-keys/second, build 64 log). Log each
// unique message once; repeats are silent.
static bool logOnce(const char* prefix, const char* a, const char* b = nullptr) {
    // unordered_set: this fires for every unique JNI lookup/key during
    // loading (tens of thousands of calls total across a session) — O(1)
    // average lookup instead of std::set's O(log n) tree walk.
    static std::unordered_set<std::string> seen;
    std::string key = std::string(prefix) + "|" + (a ? a : "") + "|" + (b ? b : "");
    StoreLock sl;
    return seen.insert(key).second;
}

static std::unordered_map<std::string, float> g_float_store;
static std::string g_ud_path;
static bool        g_ud_dirty = false;

// ─── UserDefault persistence ─────────────────────────────────────────────────
// The store was RAM-only, so every launch looked like a first run (ToS screen
// again, progress gone). Serialize to <game>/userdefaults.bin on every
// UserDefault.flush and at game exit; load before nativeInit.
// Record: [u8 type I/S/F][u32 klen][key][u32 vlen][value]
// Appends one record to an in-memory buffer. The old version did five separate
// fwrite() calls per key straight to the FILE*; across ~5300 keys that's ~26k
// libc calls per save, a measurable chunk of the ~700-900ms per-save stall on
// SD. Building the whole image in RAM and flushing it in a single fwrite (see
// udWriteSnapshot) turns that into one write.
static void udAppend(std::string& buf, char t, const std::string& k, const void* v, uint32_t vlen) {
    uint32_t klen = (uint32_t)k.size();
    buf.append(1, t);
    buf.append((const char*)&klen, 4);
    buf.append(k.data(), klen);
    buf.append((const char*)&vlen, 4);
    buf.append((const char*)v, vlen);
}

// A snapshot of the store at the moment a save was triggered — cheap map
// copies, taken under g_store_lock. The actual disk write (fopen/fwrite
// loop over potentially thousands of keys/fclose/rename) then happens
// against this private copy on a background thread, with no lock held, so
// concurrent UserDefault.set*() calls from the game are never blocked by it
// either.
struct UdSnapshot {
    std::unordered_map<std::string, int>         ints;
    std::unordered_map<std::string, float>       floats;
    std::unordered_map<std::string, std::string> strs;
    std::string path;
};

static void udWriteSnapshot(const UdSnapshot& snap) {
    // Serialize the whole store into one RAM buffer first, then write it with a
    // single fwrite — one SD transaction instead of ~26k tiny ones. Reserve up
    // front so the append loop never reallocates mid-build.
    std::string buf;
    buf.reserve(snap.ints.size() * 24 + snap.floats.size() * 24 + snap.strs.size() * 48 + 64);
    for (auto& kv : snap.ints)   { int32_t v = kv.second; udAppend(buf, 'I', kv.first, &v, 4); }
    for (auto& kv : snap.floats) { float   v = kv.second; udAppend(buf, 'F', kv.first, &v, 4); }
    for (auto& kv : snap.strs)   udAppend(buf, 'S', kv.first, kv.second.data(), (uint32_t)kv.second.size());

    std::string tmp = snap.path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { compatLogFmt("UserDefaults: save open FAIL %s", tmp.c_str()); return; }
    size_t wrote = fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (wrote != buf.size()) {
        compatLogFmt("UserDefaults: save write short (%zu/%zu) — keeping old file", wrote, buf.size());
        remove(tmp.c_str());
        return;
    }
    remove(snap.path.c_str());
    rename(tmp.c_str(), snap.path.c_str());
    compatLogFmt("UserDefaults: saved %zu ints, %zu floats, %zu strings (%zu bytes, one write)",
                 snap.ints.size(), snap.floats.size(), snap.strs.size(), buf.size());
}

// A background-thread hand-off for the actual disk write was tried here
// (2026-07-10) to avoid the ~700-900ms-per-save stall quantified on real
// hardware, then reverted the same day: the very next hardware test showed
// total save loss (every relaunch back to a fresh install) with saves
// exiting only through the Shop-screen hard crash (see Current Blockers #9
// in the README) — a crash that skips the synchronous exit-time save
// entirely, since it kills the process before game_loop_done runs. Whether
// the background thread itself was silently failing to complete writes, or
// the loss window it opened up was just enough combined with that crash, a
// stall is recoverable and a wiped save is not — so this stays synchronous
// (durable the instant flush() returns) until there's hardware evidence a
// background write path is actually reliable here. The 2s debounce still
// limits *how often* the slow part happens.
void jniUserDefaultsSave(bool force) {
    (void)force;  // kept for API/call-site clarity (exit-time save vs. a debounced one) — both paths are synchronous now, see comment above
    UdSnapshot snap;
    {
        StoreLock sl;
        if (g_ud_path.empty() || !g_ud_dirty) return;
        // The game calls UserDefault.flush() after each init phase (masteries,
        // event assets, IAP, ...) — on a fresh save that's several flushes
        // within a few seconds. Collapse bursts to one real write every 2s;
        // the exit-time call passes force=true so the final state is never
        // lost.
        static uint64_t s_lastSaveTick = 0;
        if (!force && s_lastSaveTick != 0) {
            uint64_t now = armGetSystemTick();
            uint64_t elapsedMs = (now - s_lastSaveTick) * 1000 / armGetSystemTickFreq();
            if (elapsedMs < 2000) return;  // stays dirty — a later call will flush it
        }
        snap.ints   = g_int_store;
        snap.floats = g_float_store;
        snap.strs   = g_str_store;
        snap.path   = g_ud_path;
        g_ud_dirty  = false;
        s_lastSaveTick = armGetSystemTick();
    }

    udWriteSnapshot(snap);
}
void jniUserDefaultsLoad(const char* path) {
    StoreLock sl;
    g_ud_path = path ? path : "";
    g_ud_dirty = false;
    FILE* f = fopen(g_ud_path.c_str(), "rb");
    if (!f) { compatLogFmt("UserDefaults: no save file (fresh run)"); return; }
    for (;;) {
        char t; uint32_t klen = 0, vlen = 0;
        if (fread(&t, 1, 1, f) != 1) break;
        if (fread(&klen, 4, 1, f) != 1 || klen > 4096) break;
        std::string key(klen, '\0');
        if (fread(&key[0], 1, klen, f) != klen) break;
        if (fread(&vlen, 4, 1, f) != 1 || vlen > 1u << 20) break;
        std::string val(vlen, '\0');
        if (vlen && fread(&val[0], 1, vlen, f) != vlen) break;
        if (t == 'I' && vlen == 4) g_int_store[key]   = *(const int32_t*)val.data();
        else if (t == 'F' && vlen == 4) g_float_store[key] = *(const float*)val.data();
        else if (t == 'S') g_str_store[key] = val;
    }
    fclose(f);
    compatLogFmt("UserDefaults: loaded %zu ints, %zu floats, %zu strings",
                 g_int_store.size(), g_float_store.size(), g_str_store.size());
}

// ─── Network state ────────────────────────────────────────────────────────────
// Report the Switch's REAL connectivity (nifm). Games handle "offline" via
// their normal Android code paths — pretending to be online sends them into
// ad/IAP/network flows that hit stubs.
static bool netAvailable() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
            NifmInternetConnectionType ct;
            u32 strength = 0;
            NifmInternetConnectionStatus st;
            if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&ct, &strength, &st)) &&
                st == NifmInternetConnectionStatus_Connected)
                cached = 1;
            nifmExit();
        }
        compatLogFmt("net: internet %s (nifm)", cached ? "CONNECTED" : "not connected");
    }
    return cached == 1;
}

// ─── Battery ──────────────────────────────────────────────────────────────────
// Unlike the accelerometer, there's no pure-NDK battery API on real Android —
// apps only get this via JNI (Java's BatteryManager, or a BATTERY_CHANGED
// broadcast receiver), and the exact method name an app/SDK uses for it isn't
// standardized. No current test game calls any of these, so this is
// forward-looking: real Switch battery data wired up under the handful of
// naming patterns actually seen across Android game SDKs, so a future game
// (or a bundled ad/analytics SDK) that happens to match gets real data
// instead of silently getting 0/false from the generic fallback.
static bool batteryPercent(u32* outPct) {
    static bool inited = false, ok = false;
    if (!inited) {
        inited = true;
        ok = R_SUCCEEDED(psmInitialize());
        if (ok) compatLog("battery: psm initialized");
        else    compatLog("battery: psmInitialize FAIL — battery queries will return 100");
    }
    if (ok && R_SUCCEEDED(psmGetBatteryChargePercentage(outPct))) return true;
    *outPct = 100;
    return false;
}
static bool batteryCharging() {
    PsmChargerType t;
    if (R_SUCCEEDED(psmGetChargerType(&t))) return t != PsmChargerType_Unconnected;
    return false;
}

// ─── All JNI stubs (must be defined before jniSetup uses their addresses) ─────

static jint     s_GetVersion(JNIEnv*)            { return JNI_VERSION_1_6; }
static jclass   s_FindClass(JNIEnv*, const char* n) {
    if (logOnce("FindClass", n)) compatLogFmt("JNI FindClass: %s", n ? n : "?");
    return DUMMY_CLASS;
}
static jclass   s_GetSuperclass(JNIEnv*, jclass)        { return DUMMY_CLASS; }
static jboolean s_IsAssignableFrom(JNIEnv*, jclass, jclass) { return JNI_TRUE; }
static jint     s_Throw(JNIEnv*, jthrowable)            { return 0; }
static jint     s_ThrowNew(JNIEnv*, jclass, const char*){ return 0; }
static jthrowable s_ExceptionOccurred(JNIEnv*)          { return nullptr; }
static void     s_ExceptionDescribe(JNIEnv*)             {}
static void     s_ExceptionClear(JNIEnv*)                {}
static void     s_FatalError(JNIEnv*, const char* m) {
    compatLogFmt("JNI FatalError: %s", m ? m : "?");
}
static jint     s_PushLocalFrame(JNIEnv*, jint)         { return 0; }
static jobject  s_PopLocalFrame(JNIEnv*, jobject o)     { return o; }
static jobject  s_NewGlobalRef(JNIEnv*, jobject o)      { return o; }
static void     s_DeleteGlobalRef(JNIEnv*, jobject)      {}
static void     s_DeleteLocalRef(JNIEnv*, jobject)       {}
static jboolean s_IsSameObject(JNIEnv*, jobject a, jobject b) {
    return a == b ? JNI_TRUE : JNI_FALSE;
}
static jobject  s_NewLocalRef(JNIEnv*, jobject o)       { return o; }
static jint     s_EnsureLocalCapacity(JNIEnv*, jint)    { return 0; }
static jobject  s_AllocObject(JNIEnv*, jclass)          { return nullptr; }
static jclass   s_GetObjectClass(JNIEnv*, jobject)      { return DUMMY_CLASS; }
static jboolean s_IsInstanceOf(JNIEnv*, jobject, jclass){ return JNI_TRUE; }

static jmethodID s_GetMethodID(JNIEnv*, jclass, const char* n, const char* sg) {
    if (logOnce("GetMethodID", n, sg))
        compatLogFmt("JNI GetMethodID: %s %s", n ? n : "?", sg ? sg : "?");
    MethodEntry* e = lookupOrCreateMethod(n, sg);
    return e ? (jmethodID)e : (jmethodID)DUMMY_METHOD;
}
static jmethodID s_GetStaticMethodID(JNIEnv*, jclass, const char* n, const char* sg) {
    if (logOnce("GetStaticMethodID", n, sg))
        compatLogFmt("JNI GetStaticMethodID: %s %s", n ? n : "?", sg ? sg : "?");
    MethodEntry* e = lookupOrCreateMethod(n, sg);
    return e ? (jmethodID)e : (jmethodID)DUMMY_METHOD;
}
static jfieldID s_GetFieldID(JNIEnv*, jclass, const char* n, const char* sg) {
    if (logOnce("GetFieldID", n, sg))
        compatLogFmt("JNI GetFieldID: %s %s", n ? n : "?", sg ? sg : "?");
    // Unity needs to read named fields (ApplicationInfo.metaData) — return an
    // entry that encodes the name so GetObjectField can dispatch on it. The
    // MethodEntry pool doubles as the field-name store (name+sig is all we
    // need). Cocos2d-x games keep the old opaque DUMMY_FIELD, unchanged.
    if (g_unity_mode) {
        MethodEntry* e = lookupOrCreateMethod(n, sg);
        if (e) return (jfieldID)e;
    }
    return DUMMY_FIELD;
}
static jfieldID s_GetStaticFieldID(JNIEnv*, jclass, const char* n, const char* sg) {
    compatLogFmt("JNI GetStaticFieldID: %s %s", n ? n : "?", sg ? sg : "?");
    return DUMMY_FIELD;
}

// Return-type stubs (generic, for slots that don't need per-call logging)
static jobject  s_RetObj(JNIEnv*, ...)   { return nullptr; }
static jobject  s_RetObjV(JNIEnv*, jobject, jmethodID, va_list) { return nullptr; }
static jboolean s_RetBool(JNIEnv*, ...)  { return JNI_FALSE; }
static jboolean s_RetBoolV(JNIEnv*, jobject, jmethodID, va_list) { return JNI_FALSE; }
static jint     s_RetInt(JNIEnv*, ...)   { return 0; }
static jint     s_RetIntV(JNIEnv*, jobject, jmethodID, va_list) { return 0; }
static jlong    s_RetLong(JNIEnv*, ...)  { return 0LL; }
static jlong    s_RetLongV(JNIEnv*, jobject, jmethodID, va_list) { return 0LL; }
static jfloat   s_RetFloat(JNIEnv*, ...) { return 0.0f; }
static jfloat   s_RetFloatV(JNIEnv*, jobject, jmethodID, va_list) { return 0.0f; }
static jdouble  s_RetDouble(JNIEnv*, ...) { return 0.0; }
static jdouble  s_RetDoubleV(JNIEnv*, jobject, jmethodID, va_list) { return 0.0; }
static void     s_RetVoid(JNIEnv*, ...)   {}
static void     s_RetVoidV(JNIEnv*, jobject, jmethodID, va_list) {}

// ─── Static-method dispatching stubs ─────────────────────────────────────────
// Helper: resolve a jmethodID to its MethodEntry (guards against DUMMY_METHOD)
static inline MethodEntry* methodEntry(jmethodID mid) {
    return (mid == (jmethodID)DUMMY_METHOD) ? nullptr : (MethodEntry*)mid;
}

// ─── Android Java object model (Unity/IL2CPP first-frame reflection) ──────────
// Unity's native core + IL2CPP come up without touching Java, but the first
// frame reads config through Android reflection: Activity.getIntent,
// Context.getAssets + AssetManager.open, PackageManager.getApplicationInfo,
// Bundle, java.util.Scanner. The base JNI layer above returns dummy objects,
// which strands those chains. Back the specific objects Unity touches with real
// tagged handles so the calls return usable data — asset bytes from the
// extracted APK, the real package name, filesystem paths. Everything dispatches
// by METHOD NAME, so the cocos2d-x path (which never makes these calls) is
// untouched: unknown method names fall through to the old dummy behavior.
namespace {
enum class JCls : uint8_t {
    Generic, Activity, Intent, Bundle, AssetManager, InputStream,
    PackageManager, AppInfo, Scanner, File
};
struct JavaObj {
    JCls                 cls = JCls::Generic;
    std::string          str;    // File path / package name / generic text
    std::vector<uint8_t> data;   // InputStream / Scanner content
    size_t               pos = 0;
};
static std::vector<JavaObj*> g_java_objs;  // few objects, leaked for process life

// strdup isn't exposed under -std=c++17 (newlib hides POSIX extensions), so
// roll our own. The result is handed back as a jstring (a char* in this layer);
// GetStringUTFChars copies it, so the leak is bounded to a few config strings.
static char* jdup(const char* s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static jobject jmake(JCls c, const std::string& s = "") {
    JavaObj* o = new JavaObj();
    o->cls = c; o->str = s;
    g_java_objs.push_back(o);
    return (jobject)o;
}
static JavaObj* jget(jobject o) {
    for (JavaObj* j : g_java_objs) if ((jobject)j == o) return j;
    return nullptr;  // not one of ours (a raw char* string, sentinel, or null)
}
static std::string dataDir() {
    const char* base = compatGet()->activity.internalDataPath;
    return base ? base : "";
}
static std::string packageName() {
    // internalDataPath is .../games/<pkg>; the basename is the package id.
    std::string d = dataDir();
    size_t s = d.rfind('/');
    return (s != std::string::npos) ? d.substr(s + 1) : d;
}
// AssetManager.open(rel) → an InputStream-tagged object holding the extracted
// asset's bytes (assets live under <dataDir>/assets, unpacked from the APK).
static jobject openAssetStream(const char* rel) {
    JavaObj* o = new JavaObj(); o->cls = JCls::InputStream;
    std::string path = dataDir() + "/assets/" + (rel ? rel : "");
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0) { o->data.resize((size_t)n); size_t rd = fread(o->data.data(), 1, (size_t)n, f); o->data.resize(rd); }
        fclose(f);
        compatLogFmt("JNI AssetManager.open(%s) → %zu bytes", rel ? rel : "?", o->data.size());
    } else {
        compatLogFmt("JNI AssetManager.open(%s) → not found (empty stream)", rel ? rel : "?");
    }
    g_java_objs.push_back(o);
    return (jobject)o;
}

// Object-returning instance method dispatch, by method name. va_list is
// positioned at the first Java argument.
static jobject dispatchObjectMethod(jobject recv, MethodEntry* e, va_list args) {
    if (!e) return (jobject)"";
    const char* m = e->name;
    JavaObj* self = jget(recv);

    // Context / Activity graph
    if (!strcmp(m, "getIntent"))           return jmake(JCls::Intent);
    if (!strcmp(m, "getExtras"))           return jmake(JCls::Bundle);
    if (!strcmp(m, "getAssets"))           return jmake(JCls::AssetManager);
    if (!strcmp(m, "getPackageManager"))   return jmake(JCls::PackageManager);
    if (!strcmp(m, "getApplicationInfo"))  return jmake(JCls::AppInfo);
    if (!strcmp(m, "getApplicationContext") || !strcmp(m, "getBaseContext"))
        return recv;
    if (!strcmp(m, "getClassLoader"))      return jmake(JCls::Generic);
    if (!strcmp(m, "getPackageName") || !strcmp(m, "getPackageCodePath"))
        return (jobject)jdup(packageName().c_str());
    // getObbDir is its own answer now that expansion files are actually
    // installed somewhere (see compat/obb.h). It used to fall in with the rest
    // and report the data dir, which was harmless only for as long as no OBB
    // ever arrived — a game that asked would have looked in the one directory
    // its data was guaranteed not to be in.
    if (!strcmp(m, "getObbDir")) {
        const char* obbp = compatGet()->activity.obbPath;
        return jmake(JCls::File, (obbp && *obbp) ? std::string(obbp) : dataDir());
    }
    if (!strcmp(m, "getFilesDir") || !strcmp(m, "getCacheDir") ||
        !strcmp(m, "getExternalFilesDir") || !strcmp(m, "getExternalCacheDir") ||
        !strcmp(m, "getDataDir"))
        return jmake(JCls::File, dataDir());
    if (!strcmp(m, "getAbsolutePath") || !strcmp(m, "getPath") ||
        !strcmp(m, "getCanonicalPath"))
        return (jobject)jdup(self ? self->str.c_str() : dataDir().c_str());
    if (!strcmp(m, "toString"))
        return (jobject)jdup(self ? self->str.c_str() : "");

    // AssetManager.open(String) → InputStream
    if (!strcmp(m, "open") || !strcmp(m, "openNonAssetFd") || !strcmp(m, "openFd")) {
        const char* rel = (const char*)va_arg(args, void*);
        return openAssetStream(rel);
    }

    // java.util.Scanner: useDelimiter is chainable; next/nextLine yield content.
    if (!strcmp(m, "useDelimiter") || !strcmp(m, "reset")) return recv;
    if (!strcmp(m, "next") || !strcmp(m, "nextLine")) {
        if (self && self->cls == JCls::Scanner && self->pos < self->data.size()) {
            std::string s((const char*)self->data.data() + self->pos,
                          self->data.size() - self->pos);
            self->pos = self->data.size();
            return (jobject)jdup(s.c_str());
        }
        return (jobject)jdup("");
    }

    // Bundle getters return null (→ Unity falls back to its defaults).
    if (!strcmp(m, "getString") || !strcmp(m, "get") || !strcmp(m, "getCharSequence"))
        return nullptr;

    compatLogFmt("JNI CallObjectMethodV: unhandled %s %s → \"\"", m, e->sig);
    return (jobject)"";
}

// ─── Google Play Games achievements ─────────────────────────────────────────
//
// The game already decides when an achievement is earned; it just has nowhere
// to send that decision on a console with no Play Services. These route it to
// compat/achievements.cpp instead, which records it and puts it on screen.
//
// Matching on the method name alone would be wrong. "unlock" is an ordinary
// word — Hill Climb Racing unlocks vehicles, stages and parts, and hijacking
// its own unlock(String) would record a car as an achievement. So a call is
// only taken as GPGS if its argument *is* a GPGS achievement id: Play issues
// them as base64url starting "Cgk", and nothing in a game's own vocabulary
// looks like that. If it does not match, the call falls through untouched.
static bool looksLikeGpgsId(const char* s) {
    if (!s || strncmp(s, "Cgk", 3) != 0) return false;
    size_t n = 0;
    for (const char* p = s; *p; p++, n++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return false;
    return n >= 10 && n <= 64;
}

// Walk a JNI signature to the first String parameter, consuming the arguments
// before it. Returns null unless every skipped parameter is an object — a
// float or a long ahead of the string would need a differently-typed va_arg,
// and guessing there would corrupt the list rather than miss a match.
static const char* gpgsIdArg(const MethodEntry* e, va_list& args) {
    const char* p = strchr(e->sig, '(');
    if (!p) return nullptr;
    for (p++; *p && *p != ')'; ) {
        if (*p == '[') { p++; continue; }
        if (*p == 'L') {
            const char* semi = strchr(p, ';');
            if (!semi) return nullptr;
            const bool isString = strncmp(p, "Ljava/lang/String;", 18) == 0;
            void* v = va_arg(args, void*);
            if (isString) return (const char*)v;
            p = semi + 1;
            continue;
        }
        return nullptr;                       // a primitive before the id
    }
    return nullptr;
}

// True if the call was ours and has been handled.
static bool dispatchAchievement(const MethodEntry* e, va_list& args) {
    if (!e) return false;
    const char* m = e->name;

    const bool isUnlock = !strcmp(m, "unlock")    || !strcmp(m, "unlockImmediate");
    const bool isInc    = !strcmp(m, "increment") || !strcmp(m, "incrementImmediate");
    const bool isSet    = !strcmp(m, "setSteps")  || !strcmp(m, "setStepsImmediate");
    const bool isReveal = !strcmp(m, "reveal")    || !strcmp(m, "revealImmediate");
    if (!isUnlock && !isInc && !isSet && !isReveal) return false;

    // The va_list is copied, because a non-match must leave the caller's own
    // list untouched for whatever handles it next.
    va_list probe;
    va_copy(probe, args);
    const char* id = gpgsIdArg(e, probe);
    if (!looksLikeGpgsId(id)) { va_end(probe); return false; }

    if      (isUnlock) achievements::unlock(id);
    else if (isReveal) achievements::reveal(id);
    else {
        const int steps = va_arg(probe, int);
        if (isInc) achievements::increment(id, steps);
        else       achievements::setSteps(id, steps);
    }
    va_end(probe);
    return true;
}

static jboolean dispatchBoolMethod(jobject recv, MethodEntry* e, va_list) {
    if (!e) return JNI_FALSE;
    const char* m = e->name;
    JavaObj* self = jget(recv);
    if (!strcmp(m, "hasNext") || !strcmp(m, "hasNextLine"))
        return (self && self->cls == JCls::Scanner && self->pos < self->data.size())
               ? JNI_TRUE : JNI_FALSE;
    // containsKey/getBoolean on our empty Bundles → false (Unity uses defaults).
    return JNI_FALSE;
}
} // namespace

// ─── Instance-method call stubs ───────────────────────────────────────────────
static jobject s_CallObjectMethodV(JNIEnv*, jobject recv, jmethodID mid, va_list args) {
    // The *Immediate achievement calls hand back a Task, so they arrive here
    // rather than as void calls. Checked before the Unity gate because GPGS is
    // just as common in the cocos2d titles.
    if (dispatchAchievement(methodEntry(mid), args)) return jmake(JCls::Generic);

    // Cocos2d-x games get the original dummy behavior, untouched. Only Unity
    // (which needs the real Android object graph for its first frame) routes
    // through the new dispatch.
    if (!g_unity_mode) { compatLog("JNI CallObjectMethodV"); return (jobject)""; }
    return dispatchObjectMethod(recv, methodEntry(mid), args);
}
static jobject s_CallObjectMethod(JNIEnv* env, jobject recv, jmethodID mid, ...) {
    va_list ap; va_start(ap, mid);
    jobject r = s_CallObjectMethodV(env, recv, mid, ap);
    va_end(ap);
    return r;
}
static void s_CallVoidMethodV(JNIEnv*, jobject, jmethodID mid, va_list args) {
    if (dispatchAchievement(methodEntry(mid), args)) return;
    compatLog("JNI CallVoidMethodV");
}
static void s_CallVoidMethod(JNIEnv* env, jobject recv, jmethodID mid, ...) {
    va_list ap; va_start(ap, mid);
    s_CallVoidMethodV(env, recv, mid, ap);
    va_end(ap);
}
static jboolean s_CallBoolMethodV(JNIEnv*, jobject recv, jmethodID mid, va_list args) {
    if (!g_unity_mode) { compatLog("JNI CallBooleanMethod"); return JNI_FALSE; }
    return dispatchBoolMethod(recv, methodEntry(mid), args);
}
static jboolean s_CallBoolMethod(JNIEnv* env, jobject recv, jmethodID mid, ...) {
    va_list ap; va_start(ap, mid);
    jboolean r = s_CallBoolMethodV(env, recv, mid, ap);
    va_end(ap);
    return r;
}
static jint s_CallIntMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallIntMethod"); return 0;
}
static jlong s_CallLongMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallLongMethod"); return 0LL;
}
// new Scanner(InputStream, charset) wraps the stream's bytes so next() can
// return them; other constructors stay null (unchanged behavior).
static jobject s_NewObject(JNIEnv*, jclass, jmethodID mid, ...) {
    if (!g_unity_mode) { compatLog("JNI NewObject"); return nullptr; }
    MethodEntry* e = methodEntry(mid);
    if (e && strstr(e->sig, "Ljava/io/InputStream;")) {
        va_list ap; va_start(ap, mid);
        jobject isObj = (jobject)va_arg(ap, void*);
        va_end(ap);
        JavaObj* is = jget(isObj);
        JavaObj* sc = new JavaObj(); sc->cls = JCls::Scanner;
        if (is) sc->data = is->data;
        g_java_objs.push_back(sc);
        compatLogFmt("JNI new Scanner(InputStream) → %zu bytes", sc->data.size());
        return (jobject)sc;
    }
    compatLog("JNI NewObject"); return nullptr;
}

static jint s_CallStaticIntMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (e && strcmp(e->name, "getIntegerForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        jint defval    = va_arg(args, jint);
        if (key) {
            // Reads are silent (thousands during loading; each log line fsyncs
            // to SD). A periodic counter shows liveness in the log instead.
            static int reads = 0;
            if (++reads % 2000 == 0)
                compatLogFmt("JNI getIntegerForKey: %d reads", reads);
            StoreLock sl;
            auto it = g_int_store.find(key);
            if (it != g_int_store.end()) return it->second;
        }
        return defval;
    }
    // (key, defaultValue) -> int config lookups with no backend. Same rule as the
    // String forms in s_CallStaticObjectMethodV: hand back the caller's in-app
    // default rather than the generic 0 fall-through, which silently zeroed every
    // remote-config tunable (counts, limits, feature gates).
    if (e && (strcmp(e->name, "getFirebaseRemoteConfigInt") == 0 ||
              strcmp(e->name, "getSettingInt") == 0)) {
        const char* key = (const char*)va_arg(args, jstring);
        jint defval     = va_arg(args, jint);
        if (logOnce("IntVDef", e->name, key ? key : "?"))
            compatLogFmt("JNI %s(%s) → caller default %d", e->name, key ? key : "?", (int)defval);
        return defval;
    }
    if (e && (strcmp(e->name, "getBatteryLevel") == 0 ||
              strcmp(e->name, "getBatteryPercentage") == 0 ||
              strcmp(e->name, "getBatteryPercent") == 0)) {
        u32 pct = 100;
        batteryPercent(&pct);
        compatLogFmt("JNI %s() → %u", e->name, pct);
        return (jint)pct;
    }
    // getMarketVariation() tells the game which store build it's running as. The
    // value that keeps a given title's shop path sane is a per-game decision, so
    // it lives in that game's profile (source/compat/games/) rather than here.
    if (e && strcmp(e->name, "getMarketVariation") == 0) {
        int v = gameMarketVariation(packageName().c_str());
        if (v < 0) v = 1;   // no profile opinion → claim Google Play
        compatLogFmt("JNI getMarketVariation() → %d", v);
        return v;
    }
    // SimpleAudioEngine: playEffect(path, loop, pitch, pan[, gain]) → effect id
    if (e && strcmp(e->name, "playEffect") == 0) {
        const char* p = (const char*)va_arg(args, jstring);
        int loop      = va_arg(args, int);
        // (Ljava/lang/String;ZFF)I → the trailing floats are pitch and gain;
        // apply gain per channel (pitch shifting is unsupported in SDL_mixer)
        double pitch = 1.0, gain = 1.0;
        if (strstr(e->sig, "ZFF")) { pitch = va_arg(args, double); gain = va_arg(args, double); }
        (void)pitch;
        return compatAudioPlayEffect(p, loop != 0, (float)gain);
    }
    return 0;
}
static jint s_CallStaticIntMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jint r = s_CallStaticIntMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

static jfloat s_CallStaticFloatMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) return 0.0f;
    if (strcmp(e->name, "getFloatForKey") == 0 || strcmp(e->name, "getDoubleForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        double defval   = va_arg(args, double);
        if (key) {
            StoreLock sl;
            auto it = g_float_store.find(key);
            if (it != g_float_store.end()) return it->second;
        }
        return (jfloat)defval;
    }
    if (strcmp(e->name, "getEffectsVolume") == 0)         return compatAudioGetEffectsVolume();
    if (strcmp(e->name, "getBackgroundMusicVolume") == 0) return compatAudioGetMusicVolume();
    if (logOnce("FloatV", e->name, e->sig))
        compatLogFmt("JNI CallStaticFloatMethodV: %s %s → 0", e->name, e->sig);
    return 0.0f;
}
static jfloat s_CallStaticFloatMethod(JNIEnv* env, jclass c, jmethodID m, ...) {
    va_list a; va_start(a, m);
    jfloat r = s_CallStaticFloatMethodV(env, c, m, a);
    va_end(a);
    return r;
}

static jboolean s_CallStaticBoolMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (e) {
        // EULA/consent checks — return true so the game doesn't wait forever
        if (strcmp(e->name, "eulaHasBeenAccepted") == 0 ||
            strcmp(e->name, "hasUserConsented")      == 0) {
            if (logOnce("BoolT", e->name)) compatLogFmt("JNI %s() → true", e->name);
            return JNI_TRUE;
        }
        if (strcmp(e->name, "isNetworkAvailable") == 0)
            return netAvailable() ? JNI_TRUE : JNI_FALSE;
        if (strcmp(e->name, "getBoolForKey") == 0) {
            const char* key = (const char*)va_arg(args, jstring);
            jint defval     = va_arg(args, int);
            if (key) {
                StoreLock sl;
                auto it = g_int_store.find(std::string("b:") + key);
                if (it != g_int_store.end()) return it->second ? JNI_TRUE : JNI_FALSE;
            }
            return defval ? JNI_TRUE : JNI_FALSE;
        }
        if (strcmp(e->name, "isBackgroundMusicPlaying") == 0)
            return compatAudioMusicPlaying() ? JNI_TRUE : JNI_FALSE;
        if (strcmp(e->name, "isCharging") == 0 || strcmp(e->name, "isBatteryCharging") == 0 ||
            strcmp(e->name, "isPlugged") == 0) {
            bool charging = batteryCharging();
            compatLogFmt("JNI %s() → %s", e->name, charging ? "true" : "false");
            return charging ? JNI_TRUE : JNI_FALSE;
        }
        if (logOnce("BoolV", e->name))
            compatLogFmt("JNI CallStaticBooleanMethodV: %s → false", e->name);
    }
    return JNI_FALSE;
}
static jboolean s_CallStaticBoolMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jboolean r = s_CallStaticBoolMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

static void s_CallStaticVoidMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) { compatLog("JNI CallStaticVoidMethodV"); return; }

    if (strcmp(e->name, "setIntegerForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        jint val        = va_arg(args, jint);
        if (logOnce("setInt", key ? key : "?"))
            compatLogFmt("JNI setIntegerForKey(%s, %d)", key ? key : "?", val);
        if (key) { StoreLock sl; g_int_store[key] = val; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "setStringForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        const char* val = (const char*)va_arg(args, jstring);
        if (logOnce("setStr", key ? key : "?"))
            compatLogFmt("JNI setStringForKey(%s, \"%s\")", key ? key : "?", val ? val : "null");
        if (key) { StoreLock sl; g_str_store[key] = val ? val : ""; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "setBoolForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        int val         = va_arg(args, int);
        // Was completely silent — this is likely how a mute/volume-on-off
        // toggle persists. Log every call (not logOnce) since we specifically
        // need to see the value change across calls, not just the first one.
        if (key) compatLogFmt("JNI setBoolForKey(%s, %d)", key, val);
        if (key) { StoreLock sl; g_int_store[std::string("b:") + key] = val ? 1 : 0; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "setFloatForKey") == 0 || strcmp(e->name, "setDoubleForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        double val      = va_arg(args, double);
        // Same gap as setBoolForKey above — a volume slider is almost
        // certainly persisted as a float and we couldn't see its value.
        if (key) compatLogFmt("JNI setFloatForKey(%s, %f)", key, val);
        if (key) { StoreLock sl; g_float_store[key] = (float)val; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "flush") == 0) {
        jniUserDefaultsSave();
        return;
    }
    if (strcmp(e->name, "splashScreenHasCompleted") == 0) {
        compatLog("JNI splashScreenHasCompleted — hiding branding overlay");
        compatMarkSplashDone();
        return;
    }
    if (strcmp(e->name, "debugStringOnAndroid") == 0) {
        const char* msg = (const char*)va_arg(args, jstring);
        // Exact-match dedup (logOnce) doesn't help here: many of these embed
        // a continuously-changing value ("Pedals down = 0.083333", a new
        // number practically every frame during driving), so every call
        // looked "new" and triggered a real SD-card fflush via the normal
        // compatLog path — a steady stream of disk writes throughout actual
        // gameplay, not just loading. That's a much bigger stutter source
        // than anything overlay-related. Time-throttle instead: at most
        // 2/sec regardless of content, which is still plenty to see what
        // the game's doing without flushing every single frame.
        static uint64_t s_lastDebugTick = 0;
        uint64_t nowTick = armGetSystemTick();
        uint64_t elapsedMs = (nowTick - s_lastDebugTick) * 1000 / armGetSystemTickFreq();
        if (elapsedMs >= 500) {
            compatLogFmt("game debug: %s", msg ? msg : "null");
            s_lastDebugTick = nowTick;
        }
        return;
    }
    // trackPage(pageName) fires as the game enters a new screen. An earlier
    // iap-guard here injected synthetic BACK presses on any IAP-looking page
    // name, trying to dodge the Shop crash — removed once the crash was
    // actually root-caused: the Shop screen builder calls trackPage at its
    // top and dereferences the empty shop-item vector in the SAME call
    // stack, microseconds later, so no injected key press could ever arrive
    // in time (proven by two hardware runs crashing identically with the
    // guard active). The real fix is getMarketVariation above, which stops
    // the item vector from being empty in the first place.
    if (strcmp(e->name, "trackPage") == 0) {
        const char* page = (const char*)va_arg(args, jstring);
        compatLogFmt("JNI trackPage(%s)", page ? page : "null");
        compatMarkPastLoading();
        return;
    }

    // fetchCountryCode: on Android this is an async Java web request whose
    // result comes back via the native returnCountryCode(jstring) — without a
    // reply the post-EULA flow spins forever. Answer immediately with "US"
    // (also keeps the game on the non-GDPR consent path).
    if (strcmp(e->name, "fetchCountryCode") == 0) {
        typedef void (*RetCC_fn)(JNIEnv*, jobject, jstring);
        RetCC_fn f = (RetCC_fn)compatFindGameSym(
            "Java_com_fingersoft_game_MainActivity_returnCountryCode");
        compatLogFmt("JNI fetchCountryCode → returnCountryCode(\"US\") cb=%p", (void*)f);
        if (f) f((JNIEnv*)g_jni_outer, nullptr, (jstring)"US");
        return;
    }

    // ── SimpleAudioEngine (Cocos2dxSound / Cocos2dxMusic) → audio.cpp ──
    // Note: jboolean promotes to int and jfloat to double in va_lists.
    if (strcmp(e->name, "playBackgroundMusic") == 0) {
        const char* p = (const char*)va_arg(args, jstring);
        int loop      = va_arg(args, int);
        compatAudioPlayMusic(p, loop != 0);
        return;
    }
    if (strcmp(e->name, "preloadBackgroundMusic") == 0) {
        compatAudioPreloadMusic((const char*)va_arg(args, jstring));
        return;
    }
    // enterInGame(): HCR fires this at the start of a stage, right before the
    // vehicle drops onto the map. During that drop-in it revs the looping engine
    // sound through a setEffectRate (pitch) sweep SDL_mixer can't do, so it plays
    // as a loud broken drone. Silence effects across the drop-in; the engine's
    // per-frame setEffectVolume restores real audio the instant the window ends.
    if (strcmp(e->name, "enterInGame") == 0) {
        compatLog("JNI enterInGame() → muting effects through the drop-in");
        compatAudioMuteEffectsFor(2500);
        return;
    }
    if (strcmp(e->name, "stopBackgroundMusic") == 0)    { compatAudioStopMusic(); return; }
    if (strcmp(e->name, "pauseBackgroundMusic") == 0)   { compatAudioPauseMusic(); return; }
    if (strcmp(e->name, "resumeBackgroundMusic") == 0)  { compatAudioResumeMusic(); return; }
    if (strcmp(e->name, "rewindBackgroundMusic") == 0)  { compatAudioRewindMusic(); return; }
    if (strcmp(e->name, "setBackgroundMusicVolume") == 0) {
        float v = (float)va_arg(args, double);
        compatLogFmt("JNI setBackgroundMusicVolume(%f)", v);
        compatAudioSetMusicVolume(v);
        return;
    }
    if (strcmp(e->name, "preloadEffect") == 0) {
        compatAudioPreloadEffect((const char*)va_arg(args, jstring));
        return;
    }
    if (strcmp(e->name, "unloadEffect") == 0) {
        compatAudioUnloadEffect((const char*)va_arg(args, jstring));
        return;
    }
    if (strcmp(e->name, "setEffectsVolume") == 0) {
        float v = (float)va_arg(args, double);
        compatLogFmt("JNI setEffectsVolume(%f)", v);
        compatAudioSetEffectsVolume(v);
        return;
    }
    if (strcmp(e->name, "stopEffect") == 0)      { compatAudioStopEffect(va_arg(args, int)); return; }
    if (strcmp(e->name, "pauseEffect") == 0)     { compatAudioPauseEffect(va_arg(args, int)); return; }
    if (strcmp(e->name, "resumeEffect") == 0)    { compatAudioResumeEffect(va_arg(args, int)); return; }
    // setEffectVolume(id, vol) — per-channel volume for a SPECIFIC playing
    // effect (distinct from the global setEffectsVolume above). HCR calls this
    // continuously to ramp the looping engine sound with RPM and to fade/mute
    // it on crash or pause. This was falling through to the generic no-op
    // logger, so the engine sound never changed volume or stopped — reported
    // as "engine noise constantly playing even after dying / in menus".
    if (strcmp(e->name, "setEffectVolume") == 0) {
        int id     = va_arg(args, int);
        double vol = va_arg(args, double);
        // Called continuously (RPM ramp) — same changing-value dedup problem
        // as debugStringOnAndroid above, so throttle logging, not the actual
        // volume call (that must still apply every time).
        static uint64_t s_lastTick = 0;
        uint64_t now = armGetSystemTick();
        uint64_t elapsedMs = (now - s_lastTick) * 1000 / armGetSystemTickFreq();
        if (elapsedMs >= 500) {
            compatLogFmt("JNI setEffectVolume(id=%d, vol=%f)", id, vol);
            s_lastTick = now;
        }
        compatAudioSetEffectVolume(id, (float)vol);
        return;
    }
    // setEffectRate(id, rate) — playback-rate/pitch change for a specific
    // effect (engine pitch rising with RPM). Not implemented: SDL_mixer's
    // Mix_Chunk playback rate isn't adjustable per-channel without a custom
    // resampling engine bypassing SDL_mixer's mixer entirely. No-op for now
    // (silent — this one is expected/logged in README, not a bug to chase).
    // setEffectRate(id, rate) — the game's playback-rate (pitch) control.
    //
    // Both arguments used to be read and thrown away. SDL_mixer cannot change
    // playback rate, so that was defensible as far as *doing* anything goes —
    // but the rate is also the only reliable signal for when the sound being
    // played bears no resemblance to the sound intended, which is what makes
    // the stage-start engine sweep come out as a loud drone. Passing it through
    // lets the mixer pull the channel down for exactly as long as the rate is
    // wrong, instead of muting on a 2.5s timer that has to guess.
    if (strcmp(e->name, "setEffectRate") == 0) {
        int   id   = va_arg(args, int);
        float rate = (float)va_arg(args, double);
        compatAudioSetEffectRate(id, rate);
        return;
    }
    if (strcmp(e->name, "stopAllEffects") == 0)  { compatAudioStopAllEffects(); return; }
    if (strcmp(e->name, "pauseAllEffects") == 0) { compatAudioPauseAllEffects(); return; }
    if (strcmp(e->name, "resumeAllEffects") == 0){ compatAudioResumeAllEffects(); return; }
    if (strcmp(e->name, "end") == 0) { compatAudioStopMusic(); compatAudioStopAllEffects(); return; }

    if (logOnce("VoidV", e->name, e->sig))
        compatLogFmt("JNI CallStaticVoidMethodV: %s %s", e->name, e->sig);
}
static void s_CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    s_CallStaticVoidMethodV(env, cls, mid, a);
    va_end(a);
}

static jobject s_CallStaticObjectMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) { compatLog("JNI CallStaticObjectMethodV"); return (jobject)""; }

    if (strcmp(e->name, "getStringForKey") == 0) {
        const char* key    = (const char*)va_arg(args, jstring);
        const char* defval = (const char*)va_arg(args, jstring);
        if (key) {
            StoreLock sl;
            auto it = g_str_store.find(key);
            if (it != g_str_store.end()) return (jobject)it->second.c_str();
        }
        return (jobject)(defval ? defval : "");
    }
    if (strcmp(e->name, "retrieveDefaultsString") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        if (key) {
            StoreLock sl;
            auto it = g_str_store.find(key);
            if (it != g_str_store.end()) return (jobject)it->second.c_str();
        }
        return (jobject)"";
    }
    if (strcmp(e->name, "aesDecrypt") == 0 || strcmp(e->name, "aesEncrypt") == 0) {
        const char* data = (const char*)va_arg(args, jstring);
        // Return input unchanged — identity cipher so round-trips are consistent
        return (jobject)(data ? data : "");
    }
    // (key, defaultValue) -> String lookups with no backend behind them. Firebase
    // Remote Config's documented offline behaviour is to hand back the in-app
    // default, so returning "" (the old fall-through) silently emptied every
    // config-driven list — HCR's SkinProvider map ends up with no skins, and its
    // unchecked `lookup("jeep")` then returns null and faults the shop.
    if (strcmp(e->name, "getFirebaseRemoteConfigString") == 0 ||
        strcmp(e->name, "getSettingString") == 0) {
        const char* key    = (const char*)va_arg(args, jstring);
        const char* defval = (const char*)va_arg(args, jstring);
        if (logOnce("ObjVDef", e->name, key ? key : "?"))
            compatLogFmt("JNI %s(%s) → caller default \"%s\"", e->name,
                         key ? key : "?", defval ? defval : "");
        return (jobject)(defval ? defval : "");
    }
    // The game concatenates this onto its save/config paths; "" produced the
    // empty-path fopen failures and libxml2's "failed to load external entity".
    // Android's getFilesDir() has no trailing separator — callers add their own.
    if (strcmp(e->name, "getFilesDirectory") == 0) {
        static std::string files_dir;   // filled once; callers keep the c_str()
        if (files_dir.empty()) {
            files_dir = dataDir();
            compatLogFmt("JNI getFilesDirectory() → %s", files_dir.c_str());
        }
        return (jobject)files_dir.c_str();
    }
    if (strcmp(e->name, "getDeviceLanguage") == 0 ||
        strcmp(e->name, "getCurrentLanguage") == 0) {
        return (jobject)"en";
    }
    if (strcmp(e->name, "getAndroidVersion") == 0) {
        return (jobject)"9";
    }
    if (logOnce("ObjV", e->name, e->sig))
        compatLogFmt("JNI CallStaticObjectMethodV: %s %s → \"\"", e->name, e->sig);
    return (jobject)"";
}
static jobject s_CallStaticObjectMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jobject r = s_CallStaticObjectMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

// Fields (get/set)
static jobject  s_GetObjField(JNIEnv*, jobject, jfieldID fid) {
    // Unity reads ApplicationInfo.metaData (a Bundle of manifest <meta-data>).
    // Hand back a real empty Bundle so its getString/containsKey calls resolve
    // to sane empties and Unity proceeds with defaults, instead of a null it
    // stalls/NPEs on. Field name comes from the GetFieldID entry above.
    if (g_unity_mode && (void*)fid != DUMMY_FIELD && fid) {
        const char* fname = ((MethodEntry*)fid)->name;
        if (!strcmp(fname, "metaData")) return jmake(JCls::Bundle);
    }
    return nullptr;
}
static jboolean s_GetBoolField(JNIEnv*, jobject, jfieldID){ return JNI_FALSE; }
static jbyte    s_GetByteField(JNIEnv*, jobject, jfieldID){ return 0; }
static jchar    s_GetCharField(JNIEnv*, jobject, jfieldID){ return 0; }
static jshort   s_GetShortField(JNIEnv*, jobject, jfieldID){ return 0; }
static jint     s_GetIntField(JNIEnv*, jobject, jfieldID) { return 0; }
static jlong    s_GetLongField(JNIEnv*, jobject, jfieldID){ return 0LL; }
static jfloat   s_GetFloatField(JNIEnv*, jobject, jfieldID){ return 0.0f; }
static jdouble  s_GetDoubleField(JNIEnv*, jobject, jfieldID){ return 0.0; }
static void     s_SetField(JNIEnv*, jobject, jfieldID, ...) {}

static jobject  s_GetStaticObjField(JNIEnv*, jclass, jfieldID) { return nullptr; }
static jint     s_GetStaticIntField(JNIEnv*, jclass, jfieldID)  { return 0; }
static jlong    s_GetStaticLongField(JNIEnv*, jclass, jfieldID) { return 0LL; }
static void     s_SetStaticField(JNIEnv*, jclass, jfieldID, ...) {}

// Strings
static jstring s_NewStringUTF(JNIEnv*, const char* str) { return (jstring)str; }
static jsize   s_GetStringUTFLength(JNIEnv*, jstring s) {
    return s ? (jsize)strlen((const char*)s) : 0;
}
static const char* s_GetStringUTFChars(JNIEnv*, jstring s, jboolean* cp) {
    if (cp) *cp = JNI_TRUE;
    // ART returns a malloc'd copy, and sloppy game code free()s it directly
    // instead of calling ReleaseStringUTFChars — that only survives if the
    // pointer really is heap. Returning the jstring (often a string literal
    // in OUR rodata) made such a free() corrupt the allocator (_free_r write
    // fault into the module's RX segment, build 60 log).
    const char* src = s ? (const char*)s : "";
    size_t n = strlen(src) + 1;
    char* c = (char*)malloc(n);
    if (c) memcpy(c, src, n);
    return c;
}
static void    s_ReleaseStringUTFChars(JNIEnv*, jstring, const char* c) { free((void*)c); }
static jstring s_NewString(JNIEnv*, const jchar*, jsize) { return nullptr; }
static jsize   s_GetStringLength(JNIEnv*, jstring)       { return 0; }
static const jchar* s_GetStringChars(JNIEnv*, jstring, jboolean* cp) {
    if (cp) *cp = JNI_FALSE; return nullptr;
}
static void    s_ReleaseStringChars(JNIEnv*, jstring, const jchar*) {}
static void    s_GetStringRegion(JNIEnv*, jstring, jsize, jsize, jchar*) {}
static void    s_GetStringUTFRegion(JNIEnv*, jstring, jsize, jsize, char*) {}
static const jchar* s_GetStringCritical(JNIEnv*, jstring, jboolean* cp) {
    if (cp) *cp = JNI_FALSE; return nullptr;
}
static void    s_ReleaseStringCritical(JNIEnv*, jstring, const jchar*) {}

// Arrays — blob layout is [jint len][payload]; len counts ELEMENTS.
static jsize s_GetArrayLength(JNIEnv*, jarray a) { return a ? *(jint*)a : 0; }
static jbyteArray s_NewByteArray(JNIEnv*, jsize len) {
    uint8_t* p = (uint8_t*)calloc(1, 4 + (size_t)(len > 0 ? len : 0));
    if (p && len > 0) *(jint*)p = len;
    return p;
}
static jobjectArray s_NewObjectArray(JNIEnv*, jsize, jclass, jobject) { return nullptr; }
static jobject      s_GetObjectArrayElement(JNIEnv*, jobjectArray, jsize) { return nullptr; }
static void         s_SetObjectArrayElement(JNIEnv*, jobjectArray, jsize, jobject) {}
static jbooleanArray s_NewBoolArray(JNIEnv*, jsize l)  { return (jbooleanArray)s_NewByteArray(nullptr, l); }
static jcharArray    s_NewCharArray(JNIEnv*, jsize l)  { return (jcharArray)s_NewByteArray(nullptr, l*2); }
static jshortArray   s_NewShortArray(JNIEnv*, jsize l) { return (jshortArray)s_NewByteArray(nullptr, l*2); }
static jintArray     s_NewIntArray(JNIEnv*, jsize l)   { return (jintArray)s_NewByteArray(nullptr, l*4); }
static jlongArray    s_NewLongArray(JNIEnv*, jsize l)  { return (jlongArray)s_NewByteArray(nullptr, l*8); }
static jfloatArray   s_NewFloatArray(JNIEnv*, jsize l) { return (jfloatArray)s_NewByteArray(nullptr, l*4); }
static jdoubleArray  s_NewDoubleArray(JNIEnv*, jsize l){ return (jdoubleArray)s_NewByteArray(nullptr, l*8); }

static jbyte*    s_GetByteElements(JNIEnv*, jbyteArray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (jbyte*)((uint8_t*)a + 4) : nullptr;
}
static void* s_GetElements(JNIEnv*, jarray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (uint8_t*)a + 4 : nullptr;
}
static void  s_ReleaseElements(JNIEnv*, jarray, void*, jint) {}
static void  s_GetByteRegion(JNIEnv*, jbyteArray a, jsize st, jsize l, jbyte* buf) {
    if (a && buf) memcpy(buf, (uint8_t*)a + 4 + st, (size_t)l);
}
static void  s_SetByteRegion(JNIEnv*, jbyteArray a, jsize st, jsize l, const jbyte* buf) {
    if (a && buf) memcpy((uint8_t*)a + 4 + st, buf, (size_t)l);
}
// Typed regions: the JNIEnv table has one slot per element type, so bake the
// element size into each function (cocos2d-x touch dispatch uses Int/Float).
template <size_t ES>
static void s_GetRegionT(JNIEnv*, jarray a, jsize st, jsize l, void* buf) {
    if (a && buf && l > 0) memcpy(buf, (uint8_t*)a + 4 + (size_t)st * ES, (size_t)l * ES);
}
template <size_t ES>
static void s_SetRegionT(JNIEnv*, jarray a, jsize st, jsize l, const void* buf) {
    if (a && buf && l > 0) memcpy((uint8_t*)a + 4 + (size_t)st * ES, buf, (size_t)l * ES);
}

static void* s_GetPrimArrayCritical(JNIEnv*, jarray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (uint8_t*)a + 4 : nullptr;
}
static void  s_ReleasePrimArrayCritical(JNIEnv*, jarray, void*, jint) {}

// Misc
static jint s_RegisterNatives(JNIEnv*, jclass, const JNINativeMethod* m, jint n) {
    for (jint i = 0; i < n; i++) {
        compatLogFmt("JNI RegisterNative: %s", m[i].name ? m[i].name : "?");
        g_native_methods.push_back(m[i]);
    }
    return JNI_OK;
}
static jint s_UnregisterNatives(JNIEnv*, jclass) { return JNI_OK; }

// Look up a RegisterNatives-registered native by method name (Unity/IL2CPP
// register their whole player API this way rather than exporting Java_ symbols,
// so the Unity runtime finds nativeRender/initJni/etc. through here).
void* jniFindRegisteredNative(const char* name) {
    if (!name) return nullptr;
    for (const auto& m : g_native_methods)
        if (m.name && strcmp(m.name, name) == 0) return (void*)m.fnPtr;
    return nullptr;
}

// Enable the Android Java object model (Unity first-frame reflection). Off by
// default so cocos2d-x games keep the original dummy JNI behavior untouched.
void jniSetUnityMode(bool on) { g_unity_mode = on; }
static jint s_MonitorEnter(JNIEnv*, jobject) { return 0; }
static jint s_MonitorExit(JNIEnv*, jobject)  { return 0; }
static jint s_GetJavaVM(JNIEnv*, JavaVM** out) {
    if (out) *out = (JavaVM*)g_vm_outer;
    return JNI_OK;
}
static jboolean s_ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
static jobject  s_NewDirectByteBuffer(JNIEnv*, void*, jlong) { return nullptr; }
static void*    s_GetDirectBufferAddress(JNIEnv*, jobject)   { return nullptr; }
static jlong    s_GetDirectBufferCapacity(JNIEnv*, jobject)  { return -1L; }
static jweak    s_NewWeakGlobalRef(JNIEnv*, jobject o)       { return o; }
static void     s_DeleteWeakGlobalRef(JNIEnv*, jweak)        {}
static jobjectRefType s_GetObjectRefType(JNIEnv*, jobject)   { return JNILocalRefType; }

// ─── JavaVM stubs ─────────────────────────────────────────────────────────────
static jint vm_DestroyJavaVM(JavaVM*) { return 0; }
static jint vm_AttachCurrentThread(JavaVM*, JNIEnv** e, void*) {
    if (e) *e = (JNIEnv*)g_jni_outer; return JNI_OK;
}
static jint vm_DetachCurrentThread(JavaVM*) { return 0; }
static jint vm_GetEnv(JavaVM*, void** e, jint) {
    if (e) *e = g_jni_outer; return JNI_OK;
}
static jint vm_AttachDaemon(JavaVM*, JNIEnv** e, void*) {
    if (e) *e = (JNIEnv*)g_jni_outer; return JNI_OK;
}

// ─── jniSetup ─────────────────────────────────────────────────────────────────
void jniSetup(CompatLayer* cl) {
    // Reserved slots 0-3 stay null
    g_jni_funcs[4]  = (void*)s_GetVersion;
    g_jni_funcs[5]  = (void*)s_RetObj;       // DefineClass (stub)
    g_jni_funcs[6]  = (void*)s_FindClass;
    g_jni_funcs[7]  = (void*)s_RetObj;       // FromReflectedMethod
    g_jni_funcs[8]  = (void*)s_RetObj;       // FromReflectedField
    g_jni_funcs[9]  = (void*)s_RetObj;       // ToReflectedMethod
    g_jni_funcs[10] = (void*)s_GetSuperclass;
    g_jni_funcs[11] = (void*)s_IsAssignableFrom;
    g_jni_funcs[12] = (void*)s_RetObj;       // ToReflectedField
    g_jni_funcs[13] = (void*)s_Throw;
    g_jni_funcs[14] = (void*)s_ThrowNew;
    g_jni_funcs[15] = (void*)s_ExceptionOccurred;
    g_jni_funcs[16] = (void*)s_ExceptionDescribe;
    g_jni_funcs[17] = (void*)s_ExceptionClear;
    g_jni_funcs[18] = (void*)s_FatalError;
    g_jni_funcs[19] = (void*)s_PushLocalFrame;
    g_jni_funcs[20] = (void*)s_PopLocalFrame;
    g_jni_funcs[21] = (void*)s_NewGlobalRef;
    g_jni_funcs[22] = (void*)s_DeleteGlobalRef;
    g_jni_funcs[23] = (void*)s_DeleteLocalRef;
    g_jni_funcs[24] = (void*)s_IsSameObject;
    g_jni_funcs[25] = (void*)s_NewLocalRef;
    g_jni_funcs[26] = (void*)s_EnsureLocalCapacity;
    g_jni_funcs[27] = (void*)s_AllocObject;
    g_jni_funcs[28] = (void*)s_NewObject;     // NewObject (varargs)
    g_jni_funcs[29] = (void*)s_RetObjV;       // NewObjectV
    g_jni_funcs[30] = (void*)s_NewObject;     // NewObjectA
    g_jni_funcs[31] = (void*)s_GetObjectClass;
    g_jni_funcs[32] = (void*)s_IsInstanceOf;
    g_jni_funcs[33] = (void*)s_GetMethodID;
    // CallObjectMethod 34-36
    g_jni_funcs[34] = (void*)s_CallObjectMethod;
    g_jni_funcs[35] = (void*)s_CallObjectMethodV;
    g_jni_funcs[36] = (void*)s_CallObjectMethod;
    // CallBooleanMethod 37-39
    g_jni_funcs[37] = (void*)s_CallBoolMethod;
    g_jni_funcs[38] = (void*)s_CallBoolMethodV;
    g_jni_funcs[39] = (void*)s_CallBoolMethod;
    // CallByte/Char/ShortMethod 40-48
    for (int i = 40; i <= 48; i++) g_jni_funcs[i] = (void*)s_RetInt;
    // CallIntMethod 49-51
    g_jni_funcs[49] = (void*)s_CallIntMethod;
    g_jni_funcs[50] = (void*)s_RetIntV;
    g_jni_funcs[51] = (void*)s_CallIntMethod;
    // CallLongMethod 52-54
    g_jni_funcs[52] = (void*)s_CallLongMethod;
    g_jni_funcs[53] = (void*)s_RetLongV;
    g_jni_funcs[54] = (void*)s_CallLongMethod;
    // CallFloat 55-57
    g_jni_funcs[55] = (void*)s_RetFloat;
    g_jni_funcs[56] = (void*)s_RetFloatV;
    g_jni_funcs[57] = (void*)s_RetFloat;
    // CallDouble 58-60
    g_jni_funcs[58] = (void*)s_RetDouble;
    g_jni_funcs[59] = (void*)s_RetDoubleV;
    g_jni_funcs[60] = (void*)s_RetDouble;
    // CallVoidMethod 61-63
    g_jni_funcs[61] = (void*)s_CallVoidMethod;
    g_jni_funcs[62] = (void*)s_CallVoidMethodV;
    g_jni_funcs[63] = (void*)s_CallVoidMethod;
    // Nonvirtual 64-93 (all void stubs)
    for (int i = 64; i <= 93; i++) g_jni_funcs[i] = (void*)s_RetVoid;
    // GetFieldID, Get/SetXxxField 94-112
    g_jni_funcs[94]  = (void*)s_GetFieldID;
    g_jni_funcs[95]  = (void*)s_GetObjField;
    g_jni_funcs[96]  = (void*)s_GetBoolField;
    g_jni_funcs[97]  = (void*)s_GetByteField;
    g_jni_funcs[98]  = (void*)s_GetCharField;
    g_jni_funcs[99]  = (void*)s_GetShortField;
    g_jni_funcs[100] = (void*)s_GetIntField;
    g_jni_funcs[101] = (void*)s_GetLongField;
    g_jni_funcs[102] = (void*)s_GetFloatField;
    g_jni_funcs[103] = (void*)s_GetDoubleField;
    for (int i = 104; i <= 112; i++) g_jni_funcs[i] = (void*)s_SetField;
    // GetStaticMethodID 113
    g_jni_funcs[113] = (void*)s_GetStaticMethodID;
    // CallStaticXxxMethod 114-143
    g_jni_funcs[114] = (void*)s_CallStaticObjectMethod;
    g_jni_funcs[115] = (void*)s_CallStaticObjectMethodV;
    g_jni_funcs[116] = (void*)s_CallStaticObjectMethod;  // A variant
    g_jni_funcs[117] = (void*)s_CallStaticBoolMethod;
    g_jni_funcs[118] = (void*)s_CallStaticBoolMethodV;
    g_jni_funcs[119] = (void*)s_CallStaticBoolMethod;    // A variant
    for (int i = 120; i <= 128; i++) g_jni_funcs[i] = (void*)s_RetInt; // byte/char/short
    g_jni_funcs[129] = (void*)s_CallStaticIntMethod;
    g_jni_funcs[130] = (void*)s_CallStaticIntMethodV;
    g_jni_funcs[131] = (void*)s_CallStaticIntMethod;     // A variant
    g_jni_funcs[132] = (void*)s_RetLong;
    g_jni_funcs[133] = (void*)s_RetLongV;
    g_jni_funcs[134] = (void*)s_RetLong;
    g_jni_funcs[135] = (void*)s_CallStaticFloatMethod;
    g_jni_funcs[136] = (void*)s_CallStaticFloatMethodV;
    g_jni_funcs[137] = (void*)s_CallStaticFloatMethod;   // A variant
    g_jni_funcs[138] = (void*)s_RetDouble;
    g_jni_funcs[139] = (void*)s_RetDoubleV;
    g_jni_funcs[140] = (void*)s_RetDouble;
    g_jni_funcs[141] = (void*)s_CallStaticVoidMethod;
    g_jni_funcs[142] = (void*)s_CallStaticVoidMethodV;
    g_jni_funcs[143] = (void*)s_CallStaticVoidMethod;    // A variant
    // GetStaticFieldID + Get/SetStaticXxxField 144-162
    g_jni_funcs[144] = (void*)s_GetStaticFieldID;
    g_jni_funcs[145] = (void*)s_GetStaticObjField;
    for (int i = 146; i <= 153; i++) g_jni_funcs[i] = (void*)s_GetStaticIntField;
    for (int i = 154; i <= 162; i++) g_jni_funcs[i] = (void*)s_SetStaticField;
    // Strings 163-170
    g_jni_funcs[163] = (void*)s_NewString;
    g_jni_funcs[164] = (void*)s_GetStringLength;
    g_jni_funcs[165] = (void*)s_GetStringChars;
    g_jni_funcs[166] = (void*)s_ReleaseStringChars;
    g_jni_funcs[167] = (void*)s_NewStringUTF;
    g_jni_funcs[168] = (void*)s_GetStringUTFLength;
    g_jni_funcs[169] = (void*)s_GetStringUTFChars;
    g_jni_funcs[170] = (void*)s_ReleaseStringUTFChars;
    // Arrays 171-214
    g_jni_funcs[171] = (void*)s_GetArrayLength;
    g_jni_funcs[172] = (void*)s_NewObjectArray;
    g_jni_funcs[173] = (void*)s_GetObjectArrayElement;
    g_jni_funcs[174] = (void*)s_SetObjectArrayElement;
    g_jni_funcs[175] = (void*)s_NewBoolArray;
    g_jni_funcs[176] = (void*)s_NewByteArray;
    g_jni_funcs[177] = (void*)s_NewCharArray;
    g_jni_funcs[178] = (void*)s_NewShortArray;
    g_jni_funcs[179] = (void*)s_NewIntArray;
    g_jni_funcs[180] = (void*)s_NewLongArray;
    g_jni_funcs[181] = (void*)s_NewFloatArray;
    g_jni_funcs[182] = (void*)s_NewDoubleArray;
    g_jni_funcs[183] = (void*)s_GetElements;  // GetBooleanArrayElements
    g_jni_funcs[184] = (void*)s_GetByteElements;
    g_jni_funcs[185] = (void*)s_GetElements;
    g_jni_funcs[186] = (void*)s_GetElements;
    g_jni_funcs[187] = (void*)s_GetElements;
    g_jni_funcs[188] = (void*)s_GetElements;
    g_jni_funcs[189] = (void*)s_GetElements;
    g_jni_funcs[190] = (void*)s_GetElements;
    for (int i = 191; i <= 198; i++) g_jni_funcs[i] = (void*)s_ReleaseElements;
    g_jni_funcs[199] = (void*)s_GetRegionT<1>;   // Boolean
    g_jni_funcs[200] = (void*)s_GetByteRegion;
    g_jni_funcs[201] = (void*)s_GetRegionT<2>;   // Char
    g_jni_funcs[202] = (void*)s_GetRegionT<2>;   // Short
    g_jni_funcs[203] = (void*)s_GetRegionT<4>;   // Int
    g_jni_funcs[204] = (void*)s_GetRegionT<8>;   // Long
    g_jni_funcs[205] = (void*)s_GetRegionT<4>;   // Float
    g_jni_funcs[206] = (void*)s_GetRegionT<8>;   // Double
    g_jni_funcs[207] = (void*)s_SetRegionT<1>;
    g_jni_funcs[208] = (void*)s_SetByteRegion;
    g_jni_funcs[209] = (void*)s_SetRegionT<2>;
    g_jni_funcs[210] = (void*)s_SetRegionT<2>;
    g_jni_funcs[211] = (void*)s_SetRegionT<4>;
    g_jni_funcs[212] = (void*)s_SetRegionT<8>;
    g_jni_funcs[213] = (void*)s_SetRegionT<4>;
    g_jni_funcs[214] = (void*)s_SetRegionT<8>;
    // Misc 215-232
    g_jni_funcs[215] = (void*)s_RegisterNatives;
    g_jni_funcs[216] = (void*)s_UnregisterNatives;
    g_jni_funcs[217] = (void*)s_MonitorEnter;
    g_jni_funcs[218] = (void*)s_MonitorExit;
    g_jni_funcs[219] = (void*)s_GetJavaVM;
    g_jni_funcs[220] = (void*)s_GetStringRegion;
    g_jni_funcs[221] = (void*)s_GetStringUTFRegion;
    g_jni_funcs[222] = (void*)s_GetPrimArrayCritical;
    g_jni_funcs[223] = (void*)s_ReleasePrimArrayCritical;
    g_jni_funcs[224] = (void*)s_GetStringCritical;
    g_jni_funcs[225] = (void*)s_ReleaseStringCritical;
    g_jni_funcs[226] = (void*)s_NewWeakGlobalRef;
    g_jni_funcs[227] = (void*)s_DeleteWeakGlobalRef;
    g_jni_funcs[228] = (void*)s_ExceptionCheck;
    g_jni_funcs[229] = (void*)s_NewDirectByteBuffer;
    g_jni_funcs[230] = (void*)s_GetDirectBufferAddress;
    g_jni_funcs[231] = (void*)s_GetDirectBufferCapacity;
    g_jni_funcs[232] = (void*)s_GetObjectRefType;

    // JavaVM table
    g_vm_funcs[3] = (void*)vm_DestroyJavaVM;
    g_vm_funcs[4] = (void*)vm_AttachCurrentThread;
    g_vm_funcs[5] = (void*)vm_DetachCurrentThread;
    g_vm_funcs[6] = (void*)vm_GetEnv;
    g_vm_funcs[7] = (void*)vm_AttachDaemon;

    // Build indirection chain
    g_jni_inner = (void*)g_jni_funcs;
    g_jni_outer = (void*)&g_jni_inner;
    g_vm_inner  = (void*)g_vm_funcs;
    g_vm_outer  = (void*)&g_vm_inner;

    cl->vm_outer  = g_vm_outer;
    cl->env_outer = g_jni_outer;
}
