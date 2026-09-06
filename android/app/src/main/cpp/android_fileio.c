/*
 * android_fileio.c — Intercept fopen() on Android to redirect through SAF fd table.
 *
 * Strategy:
 *   1. For READ operations: check if the relative path has a registered SAF fd.
 *      If yes, dup() the fd and fdopen() it. If no, try the original fopen().
 *   2. For WRITE operations (save files, config): always write to the save directory
 *      (internal storage), not through SAF.
 *   3. For paths starting with "Music/": look up in SAF fd table (OGG files).
 */

#ifdef ANDROID

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <android/log.h>

#define LOG_TAG "Rayverse-IO"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* From android_jni.c */
extern int android_fd_lookup(const char* relpath, int* out_mode);
extern int android_use_saf(void);
extern const char* android_get_save_dir(void);

/*
 * Check if a path is a write operation (save/config).
 * These always go to the save directory.
 */
static int is_write_mode(const char* mode) {
    return (mode && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL));
}

/*
 * Check if a path is a save/config file (should go to save dir).
 */
static int is_save_file(const char* path) {
    /* Save files: RAYMAN*.SAV, RAYMAN.CFG */
    if (strncmp(path, "RAYMAN", 6) == 0 && (strstr(path, ".SAV") || strstr(path, ".CFG"))) {
        return 1;
    }
    return 0;
}

/*
 * Check if a path is a debug/test file (should be suppressed or redirected).
 */
static int is_debug_file(const char* path) {
    if (strcmp(path, "test_out.pcx") == 0) return 1;
    if (strcmp(path, "language_dump.txt") == 0) return 1;
    return 0;
}

/*
 * Build the full path for save directory files.
 */
static void build_save_path(char* out, size_t outsize, const char* relpath) {
    const char* savedir = android_get_save_dir();
    if (savedir && savedir[0]) {
        snprintf(out, outsize, "%s/%s", savedir, relpath);
    } else {
        strncpy(out, relpath, outsize - 1);
        out[outsize - 1] = '\0';
    }
}

/*
 * Our fopen replacement. Called from sysutils.c via #define.
 */
FILE* android_fopen(const char* path, const char* mode) {
    if (!path || !mode) {
        return NULL;
    }

    /* Debug files — redirect to /dev/null */
    if (is_debug_file(path)) {
        return fopen("/dev/null", mode);
    }

    /* Write mode or save files — go to save dir */
    if (is_write_mode(mode) || is_save_file(path)) {
        char fullpath[1024];
        build_save_path(fullpath, sizeof(fullpath), path);
        LOGI("fopen save: '%s' -> '%s' mode=%s", path, fullpath, mode);
        return fopen(fullpath, mode);
    }

    /* SAF mode: check fd table first */
    if (android_use_saf()) {
        int fd_mode = -1;
        int fd = android_fd_lookup(path, &fd_mode);
        if (fd >= 0) {
            int dupfd = dup(fd);
            if (dupfd >= 0) {
                /* dup() shares the file offset with the original fd, so a
                 * second open of the same file would start at the previous
                 * EOF. Reset to the beginning — the game reads files
                 * sequentially (single reader at a time), so this is safe. */
                lseek(dupfd, 0, SEEK_SET);
                FILE* fp = fdopen(dupfd, mode);
                if (fp) {
                    LOGI("fopen SAF hit: '%s' -> fd=%d (dup=%d)", path, fd, dupfd);
                    return fp;
                } else {
                    LOGE("fdopen failed for dupfd=%d (path='%s')", dupfd, path);
                    close(dupfd);
                }
            } else {
                LOGE("dup failed for fd=%d (path='%s')", fd, path);
            }
        }
    }

    /* Fallback: try direct fopen (works if path is accessible directly) */
    return fopen(path, mode);
}

#endif /* ANDROID */
