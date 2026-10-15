#ifndef RAY_LOG_H
#define RAY_LOG_H

#include <android/log.h>
#include <dlfcn.h>
#include <stdio.h>

// Threshold used when log.tag.<TAG> is not set. Gradle injects it per build
// type: debug = ANDROID_LOG_VERBOSE (everything), release = ANDROID_LOG_ERROR.
#ifndef RAY_LOG_DEFAULT
#define RAY_LOG_DEFAULT ANDROID_LOG_ERROR
#endif

// __android_log_is_loggable() only exists from API 30 while this APK runs down
// to API 21, so it is looked up at runtime. The dlsym cache MUST be this one
// shared global (defined in android_jni.c), not a `static` inside this inline
// function: a per-TU static lives in the snapshotted data segment, and
// savestate's whole-segment rollback turns every copy into the writing
// process' liblog address — after a cross-process slot load the next RAY_LOG
// called a dangling pointer (SIGSEGV via PROT_NONE on the emulator, freezes
// on real devices). android_plat_state_save/restore() keeps this global live
// across the restore.
extern int (*ray_log_is_loggable_fn)(int, const char*, int);

static inline int ray_log_enabled(int level, const char* tag) {
    if (!ray_log_is_loggable_fn) {
        ray_log_is_loggable_fn =
            (int (*)(int, const char*, int))dlsym(RTLD_DEFAULT, "__android_log_is_loggable");
    }
    if (!ray_log_is_loggable_fn) {
        return level >= RAY_LOG_DEFAULT;
    }
    return ray_log_is_loggable_fn(level, tag, RAY_LOG_DEFAULT) > 0;
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
