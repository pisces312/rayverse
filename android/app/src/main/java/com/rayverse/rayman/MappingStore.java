package com.rayverse.rayman;

/* 手柄/键盘映射存储层 (docs/gamepad-bt-plan.md)
 * 移植自 mario-android-port 的 org.libsdl.app.MappingStore。
 * SharedPreferences "rayverse_controller_prefs":
 *   selected_device   String  选定手柄 InputDevice.getDescriptor(), 空串 = 任意手柄 (默认)
 *   mapping_json      String  映射表 JSON, 缺省/损坏/为空 -> 默认映射
 *   auto_hide_overlay boolean 手柄在线时自动隐藏虚拟手柄层, 默认 true
 *   config_version    int     每次写入自增, GamepadInputMapper 据此热重载
 * 读取失败一律回退默认, 不向游戏抛异常。
 */

import android.content.Context;
import android.content.SharedPreferences;
import android.view.KeyEvent;
import android.view.MotionEvent;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class MappingStore {

    public static final String PREFS = "rayverse_controller_prefs";
    private static final String K_DEVICE = "selected_device";
    private static final String K_MAPPING = "mapping_json";
    private static final String K_AUTOHIDE = "auto_hide_overlay";
    private static final String K_VERSION = "config_version";

    // 动作表
    public static final String[] ACTIONS = {
            "LEFT", "RIGHT", "UP", "DOWN", "JUMP", "PUNCH", "GRAB", "CONFIRM", "MENU"
    };

    public static class Binding {
        public static final int TYPE_KEY = 0;
        public static final int TYPE_AXIS = 1;
        public int type;
        public int keycode; // TYPE_KEY
        public int axis;    // TYPE_AXIS: MotionEvent.AXIS_*
        public int dir;     // TYPE_AXIS: -1 / +1

        public static Binding key(int keycode) {
            Binding b = new Binding();
            b.type = TYPE_KEY;
            b.keycode = keycode;
            return b;
        }

        public static Binding axis(int axis, int dir) {
            Binding b = new Binding();
            b.type = TYPE_AXIS;
            b.axis = axis;
            b.dir = dir < 0 ? -1 : 1;
            return b;
        }

        public boolean equalsTo(Binding o) {
            if (o == null || type != o.type) return false;
            if (type == TYPE_KEY) return keycode == o.keycode;
            return axis == o.axis && dir == o.dir;
        }
    }

    private final SharedPreferences sp;

    public MappingStore(Context ctx) {
        sp = ctx.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public int getVersion() {
        return sp.getInt(K_VERSION, 0);
    }

    private void bump(SharedPreferences.Editor e) {
        e.putInt(K_VERSION, sp.getInt(K_VERSION, 0) + 1);
    }

    public String getSelectedDevice() {
        String s = sp.getString(K_DEVICE, "");
        return s == null ? "" : s;
    }

    public void setSelectedDevice(String descriptor) {
        SharedPreferences.Editor e = sp.edit();
        e.putString(K_DEVICE, descriptor == null ? "" : descriptor);
        bump(e);
        e.apply();
    }

    public boolean getAutoHide() {
        return sp.getBoolean(K_AUTOHIDE, true);
    }

    public void setAutoHide(boolean b) {
        SharedPreferences.Editor e = sp.edit();
        e.putBoolean(K_AUTOHIDE, b);
        bump(e);
        e.apply();
    }

    /** 读取映射; 缺省/损坏/全空 -> 默认映射 */
    public Map<String, List<Binding>> getMapping() {
        String json = sp.getString(K_MAPPING, null);
        Map<String, List<Binding>> m = null;
        if (json != null) {
            try {
                m = parseMapping(json);
            } catch (Exception ignored) {
                m = null;
            }
        }
        if (m == null || isEmpty(m)) {
            m = defaultMapping();
        }
        return m;
    }

    public void setMapping(Map<String, List<Binding>> m) {
        SharedPreferences.Editor e = sp.edit();
        e.putString(K_MAPPING, toJson(m));
        bump(e);
        e.apply();
    }

    public void resetMappingToDefault() {
        setMapping(defaultMapping());
    }

    /** 动作 -> 注入虚拟键 (Android keycode, 与 GamepadOverlay 同一套) */
    public static int virtualKeyFor(String action) {
        switch (action) {
            case "LEFT":    return KeyEvent.KEYCODE_DPAD_LEFT;
            case "RIGHT":   return KeyEvent.KEYCODE_DPAD_RIGHT;
            case "UP":      return KeyEvent.KEYCODE_DPAD_UP;
            case "DOWN":    return KeyEvent.KEYCODE_DPAD_DOWN;
            case "JUMP":    return KeyEvent.KEYCODE_CTRL_LEFT;
            case "PUNCH":   return KeyEvent.KEYCODE_ALT_LEFT;
            case "GRAB":    return KeyEvent.KEYCODE_X;
            case "CONFIRM": return KeyEvent.KEYCODE_ENTER;
            case "MENU":    return KeyEvent.KEYCODE_ESCAPE;
        }
        return 0;
    }

    public static String labelFor(String action) {
        switch (action) {
            case "LEFT":    return "左移";
            case "RIGHT":   return "右移";
            case "UP":      return "上看/进入";
            case "DOWN":    return "下蹲";
            case "JUMP":    return "跳跃";
            case "PUNCH":   return "拳击";
            case "GRAB":    return "抓/悬吊";
            case "CONFIRM": return "菜单确认";
            case "MENU":    return "菜单/返回";
        }
        return action;
    }

    public static String bindingLabel(Binding b) {
        if (b.type == Binding.TYPE_KEY) {
            String s = KeyEvent.keyCodeToString(b.keycode);
            if (s != null && s.startsWith("KEYCODE_")) s = s.substring(8);
            return s;
        }
        String s;
        try {
            s = MotionEvent.axisToString(b.axis);
        } catch (Throwable t) {
            s = "AXIS_" + b.axis;
        }
        return s + (b.dir < 0 ? "-" : "+");
    }

    /** 默认映射 (Xbox 布局基准) */
    public static Map<String, List<Binding>> defaultMapping() {
        Map<String, List<Binding>> m = new LinkedHashMap<String, List<Binding>>();
        for (String a : ACTIONS) m.put(a, new ArrayList<Binding>());

        // 十字键 + 左摇杆 + HAT -> 方向
        add(m, "LEFT",  Binding.key(KeyEvent.KEYCODE_DPAD_LEFT));
        add(m, "LEFT",  Binding.axis(MotionEvent.AXIS_X, -1));
        add(m, "LEFT",  Binding.axis(MotionEvent.AXIS_HAT_X, -1));
        add(m, "RIGHT", Binding.key(KeyEvent.KEYCODE_DPAD_RIGHT));
        add(m, "RIGHT", Binding.axis(MotionEvent.AXIS_X, 1));
        add(m, "RIGHT", Binding.axis(MotionEvent.AXIS_HAT_X, 1));
        add(m, "UP",    Binding.key(KeyEvent.KEYCODE_DPAD_UP));
        add(m, "UP",    Binding.axis(MotionEvent.AXIS_Y, -1));
        add(m, "UP",    Binding.axis(MotionEvent.AXIS_HAT_Y, -1));
        add(m, "DOWN",  Binding.key(KeyEvent.KEYCODE_DPAD_DOWN));
        add(m, "DOWN",  Binding.axis(MotionEvent.AXIS_Y, 1));
        add(m, "DOWN",  Binding.axis(MotionEvent.AXIS_HAT_Y, 1));

        // A -> 跳 + 菜单确认 (一键多绑)
        add(m, "JUMP",    Binding.key(KeyEvent.KEYCODE_BUTTON_A));
        add(m, "CONFIRM", Binding.key(KeyEvent.KEYCODE_BUTTON_A));
        // X -> 拳; Y/L1/R1 -> 抓
        add(m, "PUNCH", Binding.key(KeyEvent.KEYCODE_BUTTON_X));
        add(m, "GRAB",  Binding.key(KeyEvent.KEYCODE_BUTTON_Y));
        add(m, "GRAB",  Binding.key(KeyEvent.KEYCODE_BUTTON_L1));
        add(m, "GRAB",  Binding.key(KeyEvent.KEYCODE_BUTTON_R1));
        // B / START / SELECT -> 菜单/返回
        add(m, "MENU", Binding.key(KeyEvent.KEYCODE_BUTTON_B));
        add(m, "MENU", Binding.key(KeyEvent.KEYCODE_BUTTON_START));
        add(m, "MENU", Binding.key(KeyEvent.KEYCODE_BUTTON_SELECT));
        return m;
    }

    private static void add(Map<String, List<Binding>> m, String action, Binding b) {
        List<Binding> l = m.get(action);
        if (l == null) {
            l = new ArrayList<Binding>();
            m.put(action, l);
        }
        l.add(b);
    }

    private static boolean isEmpty(Map<String, List<Binding>> m) {
        for (List<Binding> l : m.values()) {
            if (l != null && !l.isEmpty()) return false;
        }
        return true;
    }

    // ---- JSON ----

    private static Map<String, List<Binding>> parseMapping(String json) throws Exception {
        JSONObject root = new JSONObject(json);
        JSONObject actions = root.getJSONObject("actions");
        Map<String, List<Binding>> m = new LinkedHashMap<String, List<Binding>>();
        for (String a : ACTIONS) m.put(a, new ArrayList<Binding>());
        JSONArray names = actions.names();
        if (names == null) return m;
        for (int i = 0; i < names.length(); i++) {
            String action = names.getString(i);
            if (!m.containsKey(action)) continue;
            JSONArray arr = actions.getJSONArray(action);
            List<Binding> list = m.get(action);
            for (int j = 0; j < arr.length(); j++) {
                JSONObject o = arr.getJSONObject(j);
                String type = o.optString("type", "");
                if ("key".equals(type)) {
                    list.add(Binding.key(o.getInt("keycode")));
                } else if ("axis".equals(type)) {
                    list.add(Binding.axis(o.getInt("axis"), o.optInt("dir", 1)));
                }
            }
        }
        return m;
    }

    private static String toJson(Map<String, List<Binding>> m) {
        try {
            JSONObject root = new JSONObject();
            root.put("version", 1);
            JSONObject actions = new JSONObject();
            for (Map.Entry<String, List<Binding>> en : m.entrySet()) {
                JSONArray arr = new JSONArray();
                for (Binding b : en.getValue()) {
                    JSONObject o = new JSONObject();
                    if (b.type == Binding.TYPE_KEY) {
                        o.put("type", "key");
                        o.put("keycode", b.keycode);
                    } else {
                        o.put("type", "axis");
                        o.put("axis", b.axis);
                        o.put("dir", b.dir);
                    }
                    arr.put(o);
                }
                actions.put(en.getKey(), arr);
            }
            root.put("actions", actions);
            return root.toString();
        } catch (Exception e) {
            return "{\"version\":1,\"actions\":{}}";
        }
    }
}
