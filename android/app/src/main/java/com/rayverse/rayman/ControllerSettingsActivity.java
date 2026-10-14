package com.rayverse.rayman;

/* 手柄/键盘设置界面 (docs/gamepad-bt-plan.md)
 * 移植自 mario-android-port 的 org.libsdl.app.ControllerSettingsActivity。
 * 原生 View 代码构建布局 (项目无 androidx 依赖), 横屏。
 * 1. 设备状态: 实时列出输入设备 + 手柄选择 (任意 / 按 descriptor)
 * 2. 按键映射: 动作行 + 捕获对话框 (按键 down 捕获 / 摇杆 |v|>0.7 判定, 追加式, 可清空本项)
 * 3. 操作: 恢复默认 / 测试模式 (大字显示 物理输入 -> 动作) / 自动隐藏虚拟手柄开关
 * 4. 说明: 配对指引与已知限制 (系统保留键)
 * 所有改动立即写入 MappingStore (config_version 自增), 游戏 onResume 时热重载。
 */

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class ControllerSettingsActivity extends Activity implements InputManager.InputDeviceListener {

    private MappingStore store;
    private InputManager inputManager;
    private Map<String, List<MappingStore.Binding>> working; // 工作副本, 改动即保存

    private LinearLayout deviceListBox;
    private RadioGroup deviceRadio;
    private LinearLayout mappingRows;
    private TextView testView;
    private boolean suppressRadioListener = false;
    private boolean testMode = false;

    // 捕获状态
    private String capturingAction = null;
    private TextView captureText = null;
    private AlertDialog captureDialog = null;
    private final Set<Integer> captureAxisBlocked = new HashSet<Integer>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        store = new MappingStore(this);
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        working = store.getMapping();
        buildLayout();
        refreshDevices();
        refreshMappingRows();
        updateTestView(null);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, null);
        }
    }

    @Override
    protected void onPause() {
        if (inputManager != null) {
            try {
                inputManager.unregisterInputDeviceListener(this);
            } catch (Throwable ignored) {}
        }
        super.onPause();
    }

    // ---------------- 布局 ----------------

    private int dp(int v) {
        return (int) (getResources().getDisplayMetrics().density * v + 0.5f);
    }

    private TextView header(String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(16);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextColor(0xFF88CCFF);
        tv.setPadding(0, dp(14), 0, dp(4));
        return tv;
    }

    private void buildLayout() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        col.setPadding(p, p, p, p);

        TextView title = new TextView(this);
        title.setText("手柄 / 键盘设置");
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);

        // ① 设备状态
        col.addView(header("① 已连接输入设备"));
        deviceListBox = new LinearLayout(this);
        deviceListBox.setOrientation(LinearLayout.VERTICAL);
        col.addView(deviceListBox);

        // ② 手柄选择
        col.addView(header("② 手柄选择（按键映射只对选定手柄生效；键盘改键始终生效）"));
        deviceRadio = new RadioGroup(this);
        deviceRadio.setOrientation(RadioGroup.VERTICAL);
        col.addView(deviceRadio);

        // ③ 按键映射
        col.addView(header("③ 按键映射（点“改”后按下手柄按键或推动摇杆，捕获即追加绑定）"));
        mappingRows = new LinearLayout(this);
        mappingRows.setOrientation(LinearLayout.VERTICAL);
        col.addView(mappingRows);

        // ④ 操作
        col.addView(header("④ 操作"));
        LinearLayout ops = new LinearLayout(this);
        ops.setOrientation(LinearLayout.HORIZONTAL);

        Button resetBtn = new Button(this);
        resetBtn.setText("恢复默认映射");
        resetBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                confirmReset();
            }
        });
        ops.addView(resetBtn, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        CheckBox testBox = new CheckBox(this);
        testBox.setText("测试模式");
        testBox.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean checked) {
                testMode = checked;
                updateTestView(null);
            }
        });
        ops.addView(testBox, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        CheckBox hideBox = new CheckBox(this);
        hideBox.setText("手柄连接时自动隐藏虚拟手柄");
        hideBox.setChecked(store.getAutoHide());
        hideBox.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean checked) {
                store.setAutoHide(checked);
            }
        });
        ops.addView(hideBox, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        col.addView(ops);

        testView = new TextView(this);
        testView.setTextSize(20);
        testView.setTypeface(Typeface.MONOSPACE);
        testView.setTextColor(Color.YELLOW);
        testView.setBackgroundColor(0x33000000);
        testView.setPadding(p, dp(8), p, dp(8));
        col.addView(testView);

        // ⑤ 说明
        col.addView(header("⑤ 说明"));
        TextView help = new TextView(this);
        help.setTextSize(13);
        help.setTextColor(0xFFBBBBBB);
        help.setText(
                "· 蓝牙手柄/键盘请先在系统“设置 > 蓝牙”中配对，再进游戏或本页。\n" +
                "· 默认映射：十字键/左摇杆=移动，A=跳跃(菜单=确认)，X=拳击，Y/L1/R1=抓，B/START/SELECT=菜单/返回。\n" +
                "· 蓝牙键盘无需映射，方向/Ctrl/Alt/X/Enter/Esc 直接可玩；也可用本页把键改到别的位置。\n" +
                "· 一个物理键可绑定多个动作（如 A 同时是“跳跃”和“菜单确认”），互不冲突。\n" +
                "· HOME/电源/音量/截图为系统保留键，无法捕获。\n" +
                "· 改键立即生效并永久保存（游戏回到前台时自动热重载）。");
        col.addView(help);

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        setContentView(sv);
    }

    // ---------------- ①② 设备列表与选择 ----------------

    private String deviceTags(InputDevice d) {
        StringBuilder sb = new StringBuilder();
        if (d.isVirtual()) sb.append("[虚拟] ");
        if (GamepadInputMapper.isGamepadLike(d)) sb.append("[手柄] ");
        if ((d.getSources() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD) sb.append("[键盘] ");
        if ((d.getSources() & InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN) sb.append("[触屏] ");
        return sb.toString().trim();
    }

    private void refreshDevices() {
        // 设备列表
        deviceListBox.removeAllViews();
        int[] ids = InputDevice.getDeviceIds();
        boolean any = false;
        for (int id : ids) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            any = true;
            TextView tv = new TextView(this);
            tv.setTextSize(13);
            tv.setText(deviceTags(d) + "  " + d.getName() + "  (id " + id + ")");
            tv.setTextColor(GamepadInputMapper.isGamepadLike(d) ? 0xFFA5D6A7 : 0xFFCCCCCC);
            deviceListBox.addView(tv);
        }
        if (!any) {
            TextView tv = new TextView(this);
            tv.setTextSize(13);
            tv.setText("（无外部输入设备）");
            deviceListBox.addView(tv);
        }

        // 手柄选择 radio
        suppressRadioListener = true;
        deviceRadio.removeAllViews();
        String selected = store.getSelectedDevice();

        RadioButton anyBtn = new RadioButton(this);
        anyBtn.setText("任意手柄设备（默认，插上即玩）");
        anyBtn.setTag("");
        deviceRadio.addView(anyBtn);

        boolean foundSelected = selected.isEmpty();
        for (int id : ids) {
            InputDevice d = InputDevice.getDevice(id);
            if (!GamepadInputMapper.isGamepadLike(d)) continue;
            RadioButton rb = new RadioButton(this);
            rb.setText(d.getName() + "  (id " + id + ")");
            rb.setTag(d.getDescriptor());
            deviceRadio.addView(rb);
            if (selected.equals(d.getDescriptor())) {
                rb.setChecked(true);
                foundSelected = true;
            }
        }
        if (!foundSelected || selected.isEmpty()) {
            anyBtn.setChecked(true);
        }
        suppressRadioListener = false;

        deviceRadio.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int checkedId) {
                if (suppressRadioListener) return;
                View v = group.findViewById(checkedId);
                if (v == null) return;
                Object tag = v.getTag();
                store.setSelectedDevice(tag == null ? "" : tag.toString());
            }
        });
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        refreshDevices();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        refreshDevices();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        refreshDevices();
    }

    // ---------------- ③ 按键映射 ----------------

    private String summarize(String action) {
        List<MappingStore.Binding> list = working.get(action);
        if (list == null || list.isEmpty()) return "（未绑定）";
        StringBuilder sb = new StringBuilder();
        for (MappingStore.Binding b : list) {
            if (sb.length() > 0) sb.append(" + ");
            sb.append(MappingStore.bindingLabel(b));
        }
        return sb.toString();
    }

    private void refreshMappingRows() {
        mappingRows.removeAllViews();
        for (final String action : MappingStore.ACTIONS) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);

            TextView label = new TextView(this);
            label.setTextSize(15);
            label.setText(MappingStore.labelFor(action) + "  ⇒  " + shortKey(MappingStore.virtualKeyFor(action)));
            row.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 3));

            TextView binds = new TextView(this);
            binds.setTextSize(13);
            binds.setTextColor(0xFFFFE082);
            binds.setText(summarize(action));
            row.addView(binds, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 5));

            Button edit = new Button(this);
            edit.setText("改");
            edit.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    openCapture(action);
                }
            });
            row.addView(edit, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

            mappingRows.addView(row);
        }
    }

    private static String shortKey(int keycode) {
        String s = KeyEvent.keyCodeToString(keycode);
        return s != null && s.startsWith("KEYCODE_") ? s.substring(8) : s;
    }

    // ---- 捕获对话框 ----

    private void openCapture(final String action) {
        capturingAction = action;
        captureAxisBlocked.clear();

        final TextView tv = new TextView(this);
        tv.setTextSize(16);
        int p = dp(16);
        tv.setPadding(p, p, p, p);
        tv.setText("请按下手柄按键，或推动摇杆…\n（捕获即追加到本项；返回键取消）");
        captureText = tv;

        captureDialog = new AlertDialog.Builder(this)
                .setTitle("绑定：" + MappingStore.labelFor(action))
                .setView(tv)
                .setPositiveButton("完成", null)
                .setNegativeButton("清空本项", null)
                .create();
        captureDialog.setOnCancelListener(new DialogInterface.OnCancelListener() {
            @Override
            public void onCancel(DialogInterface d) {
                closeCapture();
            }
        });
        captureDialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface d) {
                closeCapture();
            }
        });
        captureDialog.show();

        captureDialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                captureDialog.dismiss();
            }
        });
        captureDialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                List<MappingStore.Binding> list = working.get(action);
                if (list != null) list.clear();
                saveMapping();
                tv.setText("已清空本项绑定。\n请按下手柄按键，或推动摇杆…");
            }
        });
    }

    private void closeCapture() {
        capturingAction = null;
        captureText = null;
        captureDialog = null;
        captureAxisBlocked.clear();
    }

    private void addCapture(MappingStore.Binding nb, String desc) {
        List<MappingStore.Binding> list = working.get(capturingAction);
        if (list == null) {
            list = new ArrayList<MappingStore.Binding>();
            working.put(capturingAction, list);
        }
        for (MappingStore.Binding b : list) {
            if (b.equalsTo(nb)) {
                if (captureText != null) captureText.append("\n（重复，已忽略）" + desc);
                return;
            }
        }
        list.add(nb);
        saveMapping();
        if (captureText != null) captureText.append("\n已追加：" + desc);
    }

    private void saveMapping() {
        store.setMapping(working);
        refreshMappingRows();
    }

    // ---------------- ④ 测试模式 / 恢复默认 ----------------

    private void confirmReset() {
        new AlertDialog.Builder(this)
                .setTitle("恢复默认映射")
                .setMessage("将丢弃全部自定义绑定，恢复 Xbox 布局默认映射。")
                .setPositiveButton("恢复", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        store.resetMappingToDefault();
                        working = store.getMapping();
                        refreshMappingRows();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void updateTestView(String text) {
        if (!testMode) {
            testView.setText("测试模式关闭：勾选“测试模式”后，按手柄/键盘可在此查看 物理输入 ⇒ 动作。");
        } else if (text == null) {
            testView.setText("测试模式：等待输入…（返回键退出本页）");
        } else {
            testView.setText(text);
        }
    }

    private String actionsForKey(int keycode) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, List<MappingStore.Binding>> en : working.entrySet()) {
            for (MappingStore.Binding b : en.getValue()) {
                if (b.type == MappingStore.Binding.TYPE_KEY && b.keycode == keycode) {
                    if (sb.length() > 0) sb.append(", ");
                    sb.append(en.getKey()).append("(").append(shortKey(MappingStore.virtualKeyFor(en.getKey()))).append(")");
                }
            }
        }
        return sb.toString();
    }

    private String actionsForAxis(int axis, int dir) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, List<MappingStore.Binding>> en : working.entrySet()) {
            for (MappingStore.Binding b : en.getValue()) {
                if (b.type == MappingStore.Binding.TYPE_AXIS && b.axis == axis && b.dir == dir) {
                    if (sb.length() > 0) sb.append(", ");
                    sb.append(en.getKey()).append("(").append(shortKey(MappingStore.virtualKeyFor(en.getKey()))).append(")");
                }
            }
        }
        return sb.toString();
    }

    // ---------------- 输入分发：捕获优先，其次测试模式 ----------------

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (capturingAction != null) {
            int kc = e.getKeyCode();
            if (kc == KeyEvent.KEYCODE_BACK) {
                if (e.getAction() == KeyEvent.ACTION_UP && captureDialog != null) {
                    captureDialog.dismiss();
                }
                return true;
            }
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) {
                addCapture(MappingStore.Binding.key(kc), shortKey(kc) + " (" + kc + ")");
            }
            return true;
        }
        if (testMode) {
            if (e.getKeyCode() == KeyEvent.KEYCODE_BACK) {
                return super.dispatchKeyEvent(e); // 返回键退出本页
            }
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) {
                String acts = actionsForKey(e.getKeyCode());
                updateTestView("按键 " + shortKey(e.getKeyCode()) + " (" + e.getKeyCode() + ")  ⇒  "
                        + (acts.isEmpty() ? "（未绑定，直通游戏）" : acts));
            }
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_CLASS_JOYSTICK)) {
            return super.dispatchGenericMotionEvent(e);
        }
        InputDevice d = e.getDevice();
        if (d == null) return super.dispatchGenericMotionEvent(e);

        if (capturingAction != null) {
            for (InputDevice.MotionRange r : d.getMotionRanges()) {
                int axis = r.getAxis();
                float v = e.getAxisValue(axis);
                float av = Math.abs(v);
                if (av > 0.7f && !captureAxisBlocked.contains(axis)) {
                    captureAxisBlocked.add(axis);
                    int dir = v < 0 ? -1 : 1;
                    String axisName;
                    try {
                        axisName = MotionEvent.axisToString(axis);
                    } catch (Throwable t) {
                        axisName = "AXIS_" + axis;
                    }
                    addCapture(MappingStore.Binding.axis(axis, dir), axisName + (dir < 0 ? "-" : "+"));
                } else if (av < 0.3f) {
                    captureAxisBlocked.remove(axis);
                }
            }
            return true;
        }
        if (testMode) {
            StringBuilder sb = new StringBuilder();
            for (InputDevice.MotionRange r : d.getMotionRanges()) {
                int axis = r.getAxis();
                float v = e.getAxisValue(axis);
                if (Math.abs(v) > 0.5f) {
                    int dir = v < 0 ? -1 : 1;
                    String acts = actionsForAxis(axis, dir);
                    if (acts.isEmpty()) continue;
                    if (sb.length() > 0) sb.append("\n");
                    String axisName;
                    try {
                        axisName = MotionEvent.axisToString(axis);
                    } catch (Throwable t) {
                        axisName = "AXIS_" + axis;
                    }
                    sb.append("摇杆 ").append(axisName).append(dir < 0 ? "-" : "+")
                            .append("  ⇒  ").append(acts);
                }
            }
            if (sb.length() > 0) updateTestView(sb.toString());
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
}
