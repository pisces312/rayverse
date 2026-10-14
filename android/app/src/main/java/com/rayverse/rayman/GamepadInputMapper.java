package com.rayverse.rayman;

/* 手柄/键盘事件 -> 虚拟键盘注入 (docs/gamepad-bt-plan.md)
 * 移植自 mario-android-port 的 org.libsdl.app.GamepadInputMapper。
 * - 按键通路: keycode -> 动作集合 -> 各动作虚拟键 onNativeKeyDown/Up, 命中即 consume
 *   (未命中 return false 走原 SDL 流程)
 * - 摇杆通路: 轴数字化状态机, 进入阈值 0.5 / 释放回差 0.3
 * - 防卡键: 虚拟键引用计数 + token 跟踪; 设备断开 / onPause / 配置重载 -> releaseAll()
 * - 热插拔: InputDeviceListener -> PresenceListener 回调 (虚拟手柄显隐联动)
 * - 设备过滤: selected_device 非空时仅处理匹配设备; 键盘来源始终过映射表 (未绑定键直通)
 */

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class GamepadInputMapper implements InputManager.InputDeviceListener {

    public interface PresenceListener {
        void onGamepadPresenceChanged(boolean online);
    }

    private static final float THRESH_ON = 0.5f;   // 轴进入阈值
    private static final float THRESH_OFF = 0.3f;  // 轴释放回差

    private final Context appCtx;
    private final MappingStore store;
    private final PresenceListener presence;
    private final InputManager inputManager;

    // 反向索引 (load 时从映射表构建)
    private final Map<Integer, Set<String>> keyToActions = new HashMap<Integer, Set<String>>();
    private final List<AxisBind> axisBinds = new ArrayList<AxisBind>();

    // 注入状态
    private final Map<Integer, Integer> virtualRef = new HashMap<Integer, Integer>(); // 虚拟键 -> 引用计数
    private final Map<String, List<Integer>> activeTokens = new HashMap<String, List<Integer>>(); // 物理源 token -> 虚拟键
    private final Set<String> axisActive = new HashSet<String>(); // 处于激活态的轴 token

    private String selected = "";   // 生效的 descriptor, "" = 任意手柄
    private boolean autoHide = true;
    private int loadedVersion = -1;
    private boolean mismatchNotified = false;

    private static class AxisBind {
        final String action;
        final int axis;
        final int dir;
        final String token;

        AxisBind(String action, int axis, int dir) {
            this.action = action;
            this.axis = axis;
            this.dir = dir;
            this.token = "a:" + axis + ":" + dir + ":" + action;
        }
    }

    public GamepadInputMapper(Context ctx, PresenceListener l) {
        appCtx = ctx.getApplicationContext();
        store = new MappingStore(ctx);
        presence = l;
        inputManager = (InputManager) appCtx.getSystemService(Context.INPUT_SERVICE);
        load(true);
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
        }
    }

    // ---- 配置加载 ----

    public void reloadIfChanged() {
        load(false);
    }

    private void load(boolean force) {
        int v = store.getVersion();
        if (!force && v == loadedVersion) return;
        releaseAll();
        loadedVersion = v;

        keyToActions.clear();
        axisBinds.clear();
        Map<String, List<MappingStore.Binding>> m = store.getMapping();
        for (Map.Entry<String, List<MappingStore.Binding>> en : m.entrySet()) {
            String action = en.getKey();
            for (MappingStore.Binding b : en.getValue()) {
                if (b.type == MappingStore.Binding.TYPE_KEY) {
                    Set<String> set = keyToActions.get(b.keycode);
                    if (set == null) {
                        set = new HashSet<String>();
                        keyToActions.put(b.keycode, set);
                    }
                    set.add(action);
                } else {
                    axisBinds.add(new AxisBind(action, b.axis, b.dir));
                }
            }
        }

        String sel = store.getSelectedDevice();
        if (!sel.isEmpty() && findDeviceByDescriptor(sel) == null) {
            // descriptor 失配 (罕见) -> 回退"任意手柄"并提示
            if (!mismatchNotified) {
                mismatchNotified = true;
                Toast.makeText(appCtx, "选定手柄不在线，本次按任意手柄处理", Toast.LENGTH_LONG).show();
            }
            sel = "";
        } else {
            mismatchNotified = false;
        }
        selected = sel;
        autoHide = store.getAutoHide();
        notifyPresence();
    }

    private static InputDevice findDeviceByDescriptor(String descriptor) {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && descriptor.equals(d.getDescriptor())) return d;
        }
        return null;
    }

    // ---- 设备判定 ----

    public static boolean isGamepadLike(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static boolean isKeyboard(InputDevice d) {
        return d != null && (d.getSources() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD;
    }

    private boolean deviceMatches(InputDevice d) {
        if (selected.isEmpty()) return true;
        return d != null && selected.equals(d.getDescriptor());
    }

    public boolean isGamepadOnline() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (isGamepadLike(d) && deviceMatches(d)) return true;
        }
        return false;
    }

    public boolean isAutoHideEnabled() {
        return autoHide;
    }

    // ---- 按键通路 ----

    /** @return true = 已 consume, SDL 不再处理 */
    public boolean handleKeyEvent(KeyEvent e) {
        InputDevice d = e.getDevice();
        // 设备过滤: 匹配选定设备, 或键盘来源 (键盘改键始终生效, 未绑定键直通)
        if (!deviceMatches(d) && !isKeyboard(d)) return false;

        Set<String> actions = keyToActions.get(e.getKeyCode());
        if (actions == null) return false; // 未绑定 -> 原 SDL 流程

        String token = "k:" + e.getKeyCode();
        switch (e.getAction()) {
            case KeyEvent.ACTION_DOWN:
                if (e.getRepeatCount() > 0) return true; // 吞掉长按重复, 不重复注入
                pressToken(token, actions);
                return true;
            case KeyEvent.ACTION_UP:
                releaseToken(token);
                return true;
            default:
                return true;
        }
    }

    // ---- 摇杆通路 ----

    /** @return true = 已 consume */
    public boolean handleMotionEvent(MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_CLASS_JOYSTICK)) return false;
        if (!deviceMatches(e.getDevice())) return false;
        if (axisBinds.isEmpty()) return false;

        for (AxisBind b : axisBinds) {
            float v = e.getAxisValue(b.axis);
            boolean active = axisActive.contains(b.token);
            if (!active) {
                if (b.dir < 0 ? v <= -THRESH_ON : v >= THRESH_ON) {
                    axisActive.add(b.token);
                    Set<String> one = new HashSet<String>();
                    one.add(b.action);
                    pressToken(b.token, one);
                }
            } else {
                if (Math.abs(v) < THRESH_OFF) {
                    axisActive.remove(b.token);
                    releaseToken(b.token);
                }
            }
        }
        return true;
    }

    // ---- 注入核心 (引用计数, 多物理源可指同一虚拟键) ----

    private void pressToken(String token, Set<String> actions) {
        if (activeTokens.containsKey(token)) return; // 防重复 down
        List<Integer> vks = new ArrayList<Integer>();
        for (String a : actions) {
            int vk = MappingStore.virtualKeyFor(a);
            if (vk != 0) vks.add(vk);
        }
        activeTokens.put(token, vks);
        for (int vk : vks) pressVirtual(vk);
    }

    private void releaseToken(String token) {
        List<Integer> vks = activeTokens.remove(token);
        if (vks == null) return;
        for (int vk : vks) releaseVirtual(vk);
    }

    private void pressVirtual(int vk) {
        Integer ref = virtualRef.get(vk);
        int r = ref == null ? 0 : ref;
        r++;
        virtualRef.put(vk, r);
        if (r == 1) injectDown(vk);
    }

    private void releaseVirtual(int vk) {
        Integer ref = virtualRef.get(vk);
        int r = ref == null ? 0 : ref;
        if (r <= 0) return;
        r--;
        virtualRef.put(vk, r);
        if (r == 0) injectUp(vk);
    }

    private static void injectDown(int keycode) {
        try {
            SDLActivity.onNativeKeyDown(keycode);
        } catch (Throwable ignored) {}
    }

    private static void injectUp(int keycode) {
        try {
            SDLActivity.onNativeKeyUp(keycode);
        } catch (Throwable ignored) {}
    }

    /** 防卡键: 释放全部已注入按键 (设备断开/onPause/配置重载/销毁时调用) */
    public void releaseAll() {
        activeTokens.clear();
        axisActive.clear();
        for (Map.Entry<Integer, Integer> en : virtualRef.entrySet()) {
            if (en.getValue() != null && en.getValue() > 0) {
                injectUp(en.getKey());
            }
        }
        virtualRef.clear();
    }

    // ---- 热插拔 ----

    @Override
    public void onInputDeviceAdded(int deviceId) {
        notifyPresence();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        releaseAll(); // 防卡键: 断开设备的按键可能再也收不到 up
        notifyPresence();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        notifyPresence();
    }

    private void notifyPresence() {
        if (presence != null) presence.onGamepadPresenceChanged(isGamepadOnline());
    }

    public void destroy() {
        if (inputManager != null) {
            try {
                inputManager.unregisterInputDeviceListener(this);
            } catch (Throwable ignored) {}
        }
        releaseAll();
    }
}
