package com.rayverse.rayman;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;
import android.widget.FrameLayout;
import org.libsdl.app.SDLActivity;

/**
 * Pure SDL activity. SetupActivity handles SAF directory selection
 * before launching this.
 *
 * Orientation is user-selectable (landscape/portrait) and persisted; the
 * device will not auto-rotate. Swiping in from the left screen edge opens the
 * settings menu (handled by GamepadOverlay).
 */
public class RayverseActivity extends SDLActivity {
    private static final String TAG = "Rayverse";
    private static final String PREFS = "rayverse_prefs";
    private static final String KEY_ORIENTATION = "orientation"; /* "landscape" | "portrait" */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        applyOrientation();
        super.onCreate(savedInstanceState);

        GameDataBridge dataBridge = new GameDataBridge(this);
        if (!dataBridge.loadSavedUri()) {
            Log.e(TAG, "No saved URI, finishing");
            finish();
            return;
        }

        /* Re-scan SAF and open fds (native libs are now loaded by SDL) */
        GameDataBridge.ScanResult result = dataBridge.scanAndStore();
        if (!result.isComplete()) {
            Log.e(TAG, "Missing required files: " + result.missingRequired);
            finish();
            return;
        }

        /* Register all fds with native code */
        dataBridge.registerWithNative();
        Log.i(TAG, "Game data OK (" + result.found.size() + " files), SDL running");

        /* Attach gamepad overlay */
        if (mLayout != null) {
            GamepadOverlay overlay = new GamepadOverlay(this);
            overlay.setOnSettingsClickListener(this::showSettingsMenu);
            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            );
            mLayout.addView(overlay, params);
        }
    }

    private void applyOrientation() {
        String o = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString(KEY_ORIENTATION, "landscape");
        setRequestedOrientation("portrait".equals(o)
                ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    private void showSettingsMenu() {
        final SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        boolean isLandscape = !"portrait".equals(prefs.getString(KEY_ORIENTATION, "landscape"));
        String targetLabel = isLandscape ? "切换到竖屏" : "切换到横屏";

        new AlertDialog.Builder(this)
            .setTitle("设置")
            .setItems(new String[]{ targetLabel }, (dialog, which) -> {
                String newOrientation = isLandscape ? "portrait" : "landscape";
                prefs.edit().putString(KEY_ORIENTATION, newOrientation).apply();
                setRequestedOrientation("portrait".equals(newOrientation)
                        ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                        : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            })
            .setNegativeButton("关闭", null)
            .show();
    }

    /**
     * Override SDL's orientation handling so the game respects the user's
     * choice and never auto-rotates. SDL otherwise infers orientation from the
     * native window size and can force a sensor-based orientation.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        String o = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString(KEY_ORIENTATION, "landscape");
        int req = "portrait".equals(o)
                ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
        Log.v(TAG, "Forcing orientation to " + o + " (req=" + req + ")");
        setRequestedOrientation(req);
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{ "SDL2", "main" };
    }
}
