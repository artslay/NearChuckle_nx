#pragma once
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

typedef void*  jobject;
typedef void*  jclass;
typedef void*  jstring;
typedef void*  jarray;
typedef void*  jbooleanArray;
typedef void*  jbyteArray;
typedef void*  jcharArray;
typedef void*  jshortArray;
typedef void*  jintArray;
typedef void*  jlongArray;
typedef void*  jfloatArray;
typedef void*  jdoubleArray;
typedef void*  jobjectArray;
typedef void*  jthrowable;
typedef void*  jweak;
typedef void*  jmethodID;
typedef void*  jfieldID;

typedef union {
    jboolean z; jbyte b; jchar c; jshort s;
    jint i; jlong j; jfloat f; jdouble d; jobject l;
} jvalue;

typedef struct {
    const char* name;
    const char* signature;
    void*       fnPtr;
} JNINativeMethod;

typedef enum {
    JNIInvalidRefType = 0, JNILocalRefType = 1,
    JNIGlobalRefType = 2,  JNIWeakGlobalRefType = 3
} jobjectRefType;

#define JNI_FALSE  0
#define JNI_TRUE   1
#define JNI_OK     0
#define JNI_ERR   (-1)
#define JNI_EDETACHED (-2)
#define JNI_EVERSION  (-3)
#define JNI_COMMIT 1
#define JNI_ABORT  2
#define JNI_VERSION_1_6 0x00010006

// JNIEnv is void** in our implementation:
//   *env = pointer to the function table (g_jni_funcs[])
//   (**env)[slot] = the function pointer
// This matches the ABI that Android C code expects.
typedef void** JNIEnv;
typedef void** JavaVM;

// JNI function table slot indices (ARM64: 8 bytes each)
#define JNI_SLOT_GetVersion          4
#define JNI_SLOT_FindClass           6
#define JNI_SLOT_GetSuperclass       10
#define JNI_SLOT_Throw               13
#define JNI_SLOT_ThrowNew            14
#define JNI_SLOT_ExceptionOccurred   15
#define JNI_SLOT_ExceptionDescribe   16
#define JNI_SLOT_ExceptionClear      17
#define JNI_SLOT_FatalError          18
#define JNI_SLOT_NewGlobalRef        21
#define JNI_SLOT_DeleteGlobalRef     22
#define JNI_SLOT_DeleteLocalRef      23
#define JNI_SLOT_IsSameObject        24
#define JNI_SLOT_NewLocalRef         25
#define JNI_SLOT_EnsureLocalCapacity 26
#define JNI_SLOT_GetObjectClass      31
#define JNI_SLOT_GetMethodID         33
#define JNI_SLOT_CallObjectMethod    34
#define JNI_SLOT_CallObjectMethodV   35
#define JNI_SLOT_CallObjectMethodA   36
#define JNI_SLOT_CallBooleanMethod   37
#define JNI_SLOT_CallBooleanMethodV  38
#define JNI_SLOT_CallBooleanMethodA  39
#define JNI_SLOT_CallIntMethod       49
#define JNI_SLOT_CallIntMethodV      50
#define JNI_SLOT_CallIntMethodA      51
#define JNI_SLOT_CallLongMethod      52
#define JNI_SLOT_CallLongMethodV     53
#define JNI_SLOT_CallLongMethodA     54
#define JNI_SLOT_CallVoidMethod      61
#define JNI_SLOT_CallVoidMethodV     62
#define JNI_SLOT_CallVoidMethodA     63
#define JNI_SLOT_GetFieldID          94
#define JNI_SLOT_GetIntField         100
#define JNI_SLOT_SetIntField         109
#define JNI_SLOT_GetStaticMethodID   113
#define JNI_SLOT_CallStaticObjectMethod   114
#define JNI_SLOT_CallStaticObjectMethodV  115
#define JNI_SLOT_CallStaticObjectMethodA  116
#define JNI_SLOT_CallStaticBooleanMethod  117
#define JNI_SLOT_CallStaticIntMethod      129
#define JNI_SLOT_CallStaticIntMethodV     130
#define JNI_SLOT_CallStaticVoidMethod     141
#define JNI_SLOT_CallStaticVoidMethodV    142
#define JNI_SLOT_GetStaticFieldID         144
#define JNI_SLOT_GetStaticIntField        150
#define JNI_SLOT_GetStaticObjectField     145
#define JNI_SLOT_NewStringUTF        167
#define JNI_SLOT_GetStringUTFLength  168
#define JNI_SLOT_GetStringUTFChars   169
#define JNI_SLOT_ReleaseStringUTFChars 170
#define JNI_SLOT_GetArrayLength      171
#define JNI_SLOT_NewByteArray        176
#define JNI_SLOT_GetByteArrayElements   184
#define JNI_SLOT_ReleaseByteArrayElements 192
#define JNI_SLOT_GetByteArrayRegion  200
#define JNI_SLOT_SetByteArrayRegion  208
#define JNI_SLOT_RegisterNatives     215
#define JNI_SLOT_UnregisterNatives   216
#define JNI_SLOT_MonitorEnter        217
#define JNI_SLOT_MonitorExit         218
#define JNI_SLOT_GetJavaVM           219
#define JNI_SLOT_ExceptionCheck      228
#define JNI_SLOT_NewDirectByteBuffer 229
#define JNI_SLOT_GetDirectBufferAddress 230
#define JNI_SLOT_GetDirectBufferCapacity 231
#define JNI_NUM_SLOTS 233

// JavaVM (JNIInvokeInterface_) slots
#define VM_SLOT_DestroyJavaVM          3
#define VM_SLOT_AttachCurrentThread    4
#define VM_SLOT_DetachCurrentThread    5
#define VM_SLOT_GetEnv                 6
#define VM_SLOT_AttachCurrentThreadAsDaemon 7
#define VM_NUM_SLOTS 8
