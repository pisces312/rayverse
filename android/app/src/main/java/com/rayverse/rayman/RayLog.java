package com.rayverse.rayman;

import android.util.Log;

/**
 * Debug builds log everything. Release builds log errors only; switch a tag on
 * over logcat and restart the app to get the rest:
 *   adb shell setprop log.tag.GameDataBridge VERBOSE
 */
final class RayLog {
    private RayLog() {
    }

    /**
     * Log.isLoggable() treats an unset property as "INFO and above", which would
     * keep release chatty, so the opt-in probe is DEBUG - true only once the
     * property is explicitly raised to DEBUG or VERBOSE.
     */
    private static boolean chatty(String tag) {
        return BuildConfig.DEBUG || Log.isLoggable(tag, Log.DEBUG);
    }

    static void v(String tag, String msg) {
        if (chatty(tag)) {
            Log.v(tag, msg);
        }
    }

    static void i(String tag, String msg) {
        if (chatty(tag)) {
            Log.i(tag, msg);
        }
    }

    static void e(String tag, String msg) {
        Log.e(tag, msg);
    }

    static void e(String tag, String msg, Throwable tr) {
        Log.e(tag, msg, tr);
    }
}
