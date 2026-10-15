#ifndef RAY_LOG_H
#define RAY_LOG_H

// __android_log_is_loggable() landed in API 30 while this APK still installs
// on API 21. Opting into weak unavailable symbols lets the reference resolve
// to NULL there instead of failing to load libmain.so, so the call site only
// needs a null check — no dlsym, and no function pointer cached in .data that
// a cross-process savestate load could roll back to a dead address.
// Must be defined before <android/log.h> is included.
#ifndef __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ 1
#endif

#include <android/log.h>
#include <stdio.h>

// Threshold used when log.tag.<TAG> is not set. Gradle injects it per build
// type: debug = ANDROID_LOG_VERBOSE (everything), release = ANDROID_LOG_ERROR.
#ifndef RAY_LOG_DEFAULT
#define RAY_LOG_DEFAULT ANDROID_LOG_ERROR
#endif

static inline int ray_log_enabled(int level, const char* tag) {
    if (__builtin_available(android 30, *)) {
        return __android_log_is_loggable(level, tag, RAY_LOG_DEFAULT) > 0;
    }
    return level >= RAY_LOG_DEFAULT;
}

/* In-app debug-log ring, implemented in android_jni.c. Every RAY_LOG line is
 * captured there regardless of the logcat gate, so the game's "查看调试日志"
 * menu can show and copy the log even on a release build without adb. */
void ray_log_ring_capture(int level, const char* tag, const char* msg);

#define RAY_LOG(tag, level, ...)                                            \
    do {                                                                    \
        char ray_log_line_[512];                                            \
        snprintf(ray_log_line_, sizeof(ray_log_line_), __VA_ARGS__);       \
        ray_log_ring_capture((level), (tag), ray_log_line_);                \
        if (ray_log_enabled(level, tag)) {                                  \
            __android_log_write((level), (tag), ray_log_line_);             \
        }                                                                   \
    } while (0)

#endif /* RAY_LOG_H */
