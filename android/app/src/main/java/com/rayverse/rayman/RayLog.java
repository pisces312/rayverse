package com.rayverse.rayman;

import android.util.Log;

import java.util.ArrayDeque;

/**
 * Debug builds log everything. Release builds log errors only; switch a tag on
 * over logcat and restart the app to get the rest:
 *   adb shell setprop log.tag.GameDataBridge VERBOSE
 *
 * Every call is also appended to an in-memory ring (dump() reads it back) so
 * the in-game "查看调试日志" menu can surface Java-side logs without adb.
 */
final class RayLog {
    private static final int RING_MAX_LINES = 400;
    private static final ArrayDeque<String> RING = new ArrayDeque<>();
    private static final long START_MS = System.currentTimeMillis();

    private RayLog() {
    }

    private static void record(char level, String tag, String msg) {
        String line = String.format("%d.%03ds %c/%s: %s",
                (System.currentTimeMillis() - START_MS) / 1000,
                (System.currentTimeMillis() - START_MS) % 1000,
                level, tag, msg);
        synchronized (RING) {
            RING.addLast(line);
            while (RING.size() > RING_MAX_LINES) {
                RING.removeFirst();
            }
        }
    }

    /** All buffered Java-side lines, oldest first. */
    static String dump() {
        synchronized (RING) {
            return String.join("\n", RING);
        }
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
        record('V', tag, msg);
        if (chatty(tag)) {
            Log.v(tag, msg);
        }
    }

    static void i(String tag, String msg) {
        record('I', tag, msg);
        if (chatty(tag)) {
            Log.i(tag, msg);
        }
    }

    static void e(String tag, String msg) {
        record('E', tag, msg);
        Log.e(tag, msg);
    }

    static void e(String tag, String msg, Throwable tr) {
        record('E', tag, msg + " / " + Log.getStackTraceString(tr));
        Log.e(tag, msg, tr);
    }
}
