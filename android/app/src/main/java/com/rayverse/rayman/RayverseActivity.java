package com.rayverse.rayman;

import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.FrameLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
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
    /* Must match SS_MAX_SLOTS in src/savestate.c. */
    private static final int SS_SLOTS = 3;

    private GamepadOverlay overlay;
    private GamepadInputMapper padMapper;
    private final Handler ssHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        applyOrientation();
        super.onCreate(savedInstanceState);

        GameDataBridge dataBridge = new GameDataBridge(this);
        if (!dataBridge.loadSavedUri()) {
            RayLog.e(TAG, "No saved URI, finishing");
            finish();
            return;
        }

        /* Re-scan SAF and open fds (native libs are now loaded by SDL) */
        GameDataBridge.ScanResult result = dataBridge.scanAndStore();
        if (!result.isComplete()) {
            RayLog.e(TAG, "Missing required files: " + result.missingRequired);
            finish();
            return;
        }

        /* Register all fds with native code */
        dataBridge.registerWithNative();
        RayLog.i(TAG, "Game data OK (" + result.found.size() + " files), SDL running");

        /* Offer to resume a snapshot saved by an earlier run (savestate 2b) */
        offerArmedResume();

        /* Attach gamepad overlay */
        if (mLayout != null) {
            overlay = new GamepadOverlay(this);
            overlay.setOnSettingsClickListener(this::showSettingsMenu);
            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            );
            mLayout.addView(overlay, params);
        }

        /* Bluetooth gamepad/keyboard mapping layer (docs/gamepad-bt-plan.md):
         * translates physical input into virtual key events injected through the
         * same SDLActivity.onNativeKeyDown/Up channel the overlay uses. */
        padMapper = new GamepadInputMapper(this, online -> updateOverlayVisibility());
        updateOverlayVisibility();
    }

    private void updateOverlayVisibility() {
        if (overlay == null || padMapper == null) return;
        boolean hide = padMapper.isAutoHideEnabled() && padMapper.isGamepadOnline();
        /* Hide only the game buttons; the settings gear stays reachable. */
        overlay.setButtonsHidden(hide);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (padMapper != null && padMapper.handleKeyEvent(event)) return true;
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (padMapper != null && padMapper.handleMotionEvent(event)) return true;
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    protected void onPause() {
        if (padMapper != null) padMapper.releaseAll();
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (padMapper != null) {
            padMapper.reloadIfChanged();
            updateOverlayVisibility();
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
        String orientLabel = isLandscape ? "切换到竖屏" : "切换到横屏";

        new AlertDialog.Builder(this)
            .setTitle("设置")
            .setItems(new String[]{
                    "即时存档",
                    "即时读档",
                    "查看调试日志",
                    orientLabel,
                    "编辑按钮位置",
                    "恢复默认布局"
                }, (dialog, which) -> {
                    switch (which) {
                        case 0:
                            showSlotDialog(true);
                            break;
                        case 1:
                            showSlotDialog(false);
                            break;
                        case 2:
                            showDebugLogDialog();
                            break;
                        case 3:
                            String newOrientation = isLandscape ? "portrait" : "landscape";
                            prefs.edit().putString(KEY_ORIENTATION, newOrientation).apply();
                            setRequestedOrientation("portrait".equals(newOrientation)
                                    ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                                    : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
                            break;
                        case 4:
                            if (overlay != null) overlay.setEditMode(true);
                            break;
                        case 5:
                            if (overlay != null) overlay.resetLayout();
                            break;
                    }
                })
            .setNegativeButton("关闭", null)
            .show();
    }

    /**
     * Ask which slot to use. The list shows what each slot already holds so a
     * save can be aimed at the right place without guessing.
     */
    private void showSlotDialog(boolean forSave) {
        stopArmedPoll();
        String[] labels = new String[SS_SLOTS];
        for (int i = 0; i < SS_SLOTS; i++) labels[i] = slotLabel(i, forSave);

        new AlertDialog.Builder(this)
            .setTitle(forSave ? "保存到槽位" : "读取槽位")
            .setItems(labels, (dialog, which) -> {
                if (forSave) nativeRequestSaveState(which);
                else nativeRequestLoadState(which);
                pollSaveStateStatus(0, which, forSave);
            })
            .setNegativeButton("取消", null)
            .show();
    }

    /**
     * "world,level,epoch,usable" comes back for a readable slot. A slot from
     * another build is listed rather than hidden - saving overwrites it, while
     * loading gets a clear "版本不符" instead of a silent failure.
     */
    private String slotLabel(int slot, boolean forSave) {
        String info = nativeGetSaveStateSlotInfo(slot);
        String num = "槽位 " + (slot + 1);
        if (info == null || "empty".equals(info)) return num + " · 空";
        if ("broken".equals(info)) return num + (forSave ? " · 旧文件（覆盖）" : " · 版本不符");
        String[] p = info.split(",");
        if (p.length < 4) return num + " · 不可用";
        String where = "世界" + p[0] + "-关卡" + p[1];
        if (!"1".equals(p[3])) {
            return num + " · " + where + (forSave ? "（覆盖）" : " · 版本不符");
        }
        return num + " · " + where + " · " + formatSlotTime(p[2]);
    }

    private String formatSlotTime(String epochSec) {
        try {
            long secs = Long.parseLong(epochSec.trim());
            if (secs <= 0) return "未知时间";
            return new java.text.SimpleDateFormat("MM-dd HH:mm",
                    java.util.Locale.getDefault()).format(new java.util.Date(secs * 1000L));
        } catch (NumberFormatException e) {
            return "未知时间";
        }
    }

    /**
     * The native request is consumed on the game thread at the next frame
     * boundary, so poll for the status code instead of expecting it inline.
     * Frames can take a while during loading screens, hence the retries.
     * Writing a slot means hashing and dumping ~5.7 MB, so the window is
     * wider than the in-memory-only case.
     */
    private void pollSaveStateStatus(int attempt, int slot, boolean forSave) {
        ssHandler.postDelayed(() -> {
            int s = nativeGetSaveStateStatus();
            RayLog.i(TAG, "savestate poll: status=" + s + " slot=" + slot
                    + " forSave=" + forSave + " (attempt " + attempt + ")");
            if (s == 0) {
                if (attempt < 40) {
                    pollSaveStateStatus(attempt + 1, slot, forSave);
                } else {
                    toast("即时存档操作超时");
                }
                return;
            }
            switch (s) {
                case 1: toast("已保存到槽位 " + (slot + 1)); break;
                case 2: toast("只能在关卡游玩中存档"); break;
                case 3: toast("即时存档失败（内存不足）"); break;
                case 4: toast("已读取槽位 " + (slot + 1)); break;
                case 5: toast("没有可用的即时存档"); break;
                case 6: toast("只能在关卡游玩中读档"); break;
                case 7: toast("存档与当前状态不兼容"); break;
                case 8: toast("写入槽位失败"); break;
                case 9: toast("槽位文件已损坏"); break;
                case 10: toast("该存档无法在当前进程中恢复"); break;
                default: break;
            }
        }, 250);
    }

    /**
     * Cross-process resume (savestate phase 2b): if a slot written by an
     * earlier run is still usable, ask once whether to jump back in.
     * Confirming arms the native side; the engine then walks itself to the
     * snapshot's world/level on the next world map and restores it there.
     * A manual save/load from the settings menu cancels the resume.
     */
    private void offerArmedResume() {
        int best = -1;
        long bestTime = 0;
        String bestInfo = null;
        for (int i = 0; i < SS_SLOTS; i++) {
            String info = nativeGetSaveStateSlotInfo(i);
            if (info == null || "empty".equals(info) || "broken".equals(info)) continue;
            String[] p = info.split(",");
            if (p.length < 4 || !"1".equals(p[3])) continue;
            long t = 0;
            try { t = Long.parseLong(p[2].trim()); } catch (NumberFormatException ignored) {}
            if (best < 0 || t > bestTime) { best = i; bestTime = t; bestInfo = info; }
        }
        if (best < 0) return;

        String[] p = bestInfo.split(",");
        String where = "世界" + p[0] + "-关卡" + p[1];
        final int slot = best;
        new AlertDialog.Builder(this)
            .setTitle("检测到即时存档")
            .setMessage("槽位 " + (slot + 1) + " · " + where + " · " + formatSlotTime(p[2])
                    + "\n启动后自动前往该关卡并恢复？")
            .setPositiveButton("恢复", (dialog, which) -> {
                RayLog.i(TAG, "armed resume confirmed: slot " + slot);
                nativeArmSaveState(slot);
                pollArmedStatus(0);
            })
            .setNegativeButton("不用", (dialog, which) ->
                    RayLog.i(TAG, "armed resume declined"))
            .show();
    }

    /**
     * The armed restore lands minutes later (after the menus and the level
     * reload), so poll until it happens instead of waiting inline. Any other
     * action on ssHandler (the manual save/load poll) replaces this one.
     */
    private void pollArmedStatus(int attempt) {
        ssHandler.postDelayed(() -> {
            int s = nativeGetSaveStateStatus();
            if (s == 0) {
                if (attempt < 600) pollArmedStatus(attempt + 1); /* ~5 min */
                return;
            }
            if (s == 4) {
                toast("已恢复即时存档");
            } else if (s != 1) {
                toast("即时存档恢复失败（代码 " + s + "）");
            }
        }, 500);
    }

    private void stopArmedPoll() {
        ssHandler.removeCallbacksAndMessages(null);
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }

    /**
     * Shows the debug log (Java ring + native RAY_LOG ring) in a scrollable
     * dialog and copies the whole text to the clipboard, so a failure on a
     * real device can be reported without adb.
     */
    private void showDebugLogDialog() {
        StringBuilder sb = new StringBuilder();
        sb.append("Rayverse ").append(BuildConfig.VERSION_NAME)
          .append(BuildConfig.DEBUG ? " (debug)" : " (release)")
          .append(" | ").append(android.os.Build.MODEL)
          .append(" | Android ").append(android.os.Build.VERSION.RELEASE)
          .append(" (API ").append(android.os.Build.VERSION.SDK_INT).append(")\n");
        sb.append("----- Java -----\n").append(RayLog.dump()).append('\n');
        String nativeDump = nativeGetDebugLogDump();
        sb.append("----- Native -----\n")
          .append(nativeDump == null ? "(no data)" : nativeDump);
        String dump = sb.toString();

        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText("rayverse-debug-log", dump));
            toast("调试日志已复制到剪贴板");
        }
        RayLog.i(TAG, "debug log viewed (" + dump.length() + " chars)");

        TextView tv = new TextView(this);
        tv.setTextIsSelectable(true);
        tv.setTextSize(11f);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        tv.setPadding(pad, pad, pad, pad);
        /* Drop the per-file fd noise from the *displayed* text; the clipboard
         * still holds everything. Show the tail only. */
        String shown = dump.replaceAll("(?m)^.*(Opened|Registered) fd .*\\r?\\n", "");
        int maxShown = 20000;
        tv.setText(shown.length() > maxShown
                ? "…(前文见剪贴板)…\n" + shown.substring(shown.length() - maxShown)
                : shown);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(tv);
        scroll.post(() -> scroll.fullScroll(android.view.View.FOCUS_DOWN));

        new AlertDialog.Builder(this)
            .setTitle("调试日志")
            .setView(scroll)
            .setPositiveButton("关闭", null)
            .show();
    }

    private static native void nativeRequestSaveState(int slot);
    private static native void nativeRequestLoadState(int slot);
    private static native int nativeGetSaveStateStatus();
    private static native String nativeGetSaveStateSlotInfo(int slot);
    private static native void nativeArmSaveState(int slot);
    private static native String nativeGetDebugLogDump();

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
        RayLog.v(TAG, "Forcing orientation to " + o + " (req=" + req + ")");
        setRequestedOrientation(req);
    }

    @Override
    protected void onDestroy() {
        if (padMapper != null) {
            padMapper.destroy();
            padMapper = null;
        }
        super.onDestroy();
        /* SDL only finishes this activity when SDL_main returns, so the process stays
         * cached with the engine's memory arenas already freed - the next launch would
         * reopen a dead engine. End the process once the activity is really gone.
         * (Killing it while the activity is still resumed makes ActivityManager
         * relaunch the task, which looks like the game restarting itself.) */
        if (isFinishing()) {
            RayLog.i(TAG, "Game over, ending process");
            android.os.Process.killProcess(android.os.Process.myPid());
            System.exit(0);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{ "SDL2", "main" };
    }
}
