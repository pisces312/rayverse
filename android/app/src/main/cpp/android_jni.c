/*
 * android_jni.c — SAF directory setup, fd table management, and rayverse main() bridge.
 *
 * Flow:
 *   Java.onCreate() -> nativeSetup(gameDataPath)
 *     -> chdir(gameDataPath) if accessible directly, OR
 *     -> store SAF URI for fd-based fopen interception
 *   Java loads libSDL2 + libmain
 *   SDLActivity.handleNativeState() -> SDL_main -> rayverse main()
 */

#ifdef ANDROID

#include <jni.h>
#include <android/log.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define LOG_TAG "Rayverse"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ---- SAF fd table ---- */

#define MAX_FD_ENTRIES 256

typedef struct {
    char path[512];   /* relative path like "PCMAP/ALLFIX.DAT" */
    int  fd;           /* file descriptor from ContentResolver */
    int  mode;         /* 0=read, 1=write */
} fd_entry_t;

static fd_entry_t g_fd_table[MAX_FD_ENTRIES];
static int g_fd_count = 0;

/* The SAF base URI string (for logging) */
static char g_saf_uri[1024] = {0};

/* Path to game data — may be a direct path or SAF-managed */
static char g_game_data_path[1024] = {0};
static int  g_use_saf = 0;  /* 1 if we must use SAF fd bridge */

/* Save/config dir — always internal storage (writable) */
static char g_save_dir[1024] = {0};

/* Called from Java to register a SAF fd for a relative path */
JNIEXPORT void JNICALL
Java_com_rayverse_rayman_GameDataBridge_nativeRegisterFd(
    JNIEnv* env, jobject thiz,
    jstring jPath, jint fd, jint mode)
{
    const char* path = (*env)->GetStringUTFChars(env, jPath, NULL);
    fd_entry_t* e = NULL;
    for (int i = 0; i < g_fd_count; i++) {
        if (strcmp(g_fd_table[i].path, path) == 0) {
            e = &g_fd_table[i];
            /* Registering again happens when the game restarts inside the same process.
             * The game holds dup()'d descriptors, so the old one can be closed here. */
            if (e->fd != fd) close(e->fd);
            break;
        }
    }
    if (e == NULL) {
        if (g_fd_count >= MAX_FD_ENTRIES) {
            LOGE("fd table full, cannot register '%s'", path);
            close(fd);
            (*env)->ReleaseStringUTFChars(env, jPath, path);
            return;
        }
        e = &g_fd_table[g_fd_count++];
    }
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    e->fd = fd;
    e->mode = mode;
    LOGI("Registered fd %d for '%s' (mode=%d)", fd, path, mode);
    (*env)->ReleaseStringUTFChars(env, jPath, path);
}

/* Called from Java to set SAF URI (for logging) */
JNIEXPORT void JNICALL
Java_com_rayverse_rayman_GameDataBridge_nativeSetSafUri(
    JNIEnv* env, jobject thiz, jstring jUri)
{
    const char* uri = (*env)->GetStringUTFChars(env, jUri, NULL);
    strncpy(g_saf_uri, uri, sizeof(g_saf_uri) - 1);
    g_saf_uri[sizeof(g_saf_uri) - 1] = '\0';
    LOGI("SAF URI set to: %s", g_saf_uri);
    (*env)->ReleaseStringUTFChars(env, jUri, uri);
}

/* Called from Java to set the game data path */
JNIEXPORT void JNICALL
Java_com_rayverse_rayman_GameDataBridge_nativeSetGameDataPath(
    JNIEnv* env, jobject thiz, jstring jPath, jint useSaf)
{
    const char* path = (*env)->GetStringUTFChars(env, jPath, NULL);
    strncpy(g_game_data_path, path, sizeof(g_game_data_path) - 1);
    g_game_data_path[sizeof(g_game_data_path) - 1] = '\0';
    g_use_saf = useSaf;
    LOGI("Game data path: %s (SAF=%d)", g_game_data_path, g_use_saf);
    (*env)->ReleaseStringUTFChars(env, jPath, path);
}

/* Called from Java to set the save directory */
JNIEXPORT void JNICALL
Java_com_rayverse_rayman_GameDataBridge_nativeSetSaveDir(
    JNIEnv* env, jobject thiz, jstring jPath)
{
    const char* path = (*env)->GetStringUTFChars(env, jPath, NULL);
    strncpy(g_save_dir, path, sizeof(g_save_dir) - 1);
    g_save_dir[sizeof(g_save_dir) - 1] = '\0';
    LOGI("Save dir: %s", g_save_dir);
    (*env)->ReleaseStringUTFChars(env, jPath, path);
}

/*
 * Lookup an fd from the table by relative path.
 * Returns the fd if found, -1 otherwise.
 * If found, also sets *out_mode.
 */
int android_fd_lookup(const char* relpath, int* out_mode) {
    for (int i = 0; i < g_fd_count; i++) {
        if (strcmp(g_fd_table[i].path, relpath) == 0) {
            if (out_mode) *out_mode = g_fd_table[i].mode;
            return g_fd_table[i].fd;
        }
    }
    return -1;
}

int android_use_saf(void) {
    return g_use_saf;
}

const char* android_get_save_dir(void) {
    return g_save_dir;
}

const char* android_get_game_data_path(void) {
    return g_game_data_path;
}

/* ---- Disable accelerometer as joystick ---- */
static void disable_accelerometer(void) {
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    LOGI("Accelerometer-as-joystick disabled");
}

/* ---- Main entry ---- */

JNIEXPORT int JNICALL
Java_com_rayverse_rayman_RayverseActivity_nativeMain(
    JNIEnv* env, jclass cls, jobjectArray args)
{
    disable_accelerometer();

    /* Set HOME to save dir so rayverse can find/save files there */
    if (g_save_dir[0]) {
        setenv("HOME", g_save_dir, 1);
        chdir(g_save_dir);
        LOGI("HOME and CWD set to: %s", g_save_dir);
    }

    /* Convert Java String[] to char** */
    int argc = (*env)->GetArrayLength(env, args);
    char** argv = (char**)calloc(argc + 1, sizeof(char*));
    for (int i = 0; i < argc; i++) {
        jstring jarg = (jstring)(*env)->GetObjectArrayElement(env, args, i);
        const char* carg = (*env)->GetStringUTFChars(env, jarg, NULL);
        argv[i] = strdup(carg);
        (*env)->ReleaseStringUTFChars(env, jarg, carg);
    }
    argv[argc] = NULL;

    LOGI("Calling main(%d, ...)", argc);
    extern int main(int argc, char** argv);
    int ret = main(argc, argv);

    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return ret;
}

#endif /* ANDROID */
