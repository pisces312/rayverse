# 蓝牙游戏手柄支持（移植自 mario-android-port）

参照 `D:\3rd-party-projects\mario-android-port\mario-and-luigi` 的 `docs/BT-INPUT.md` 方案（路线 A：Java 输入映射层）。

## 总体思路

手柄事件不启用 SDL controller 通路、不改任何 C 代码，在 Java 层翻译成虚拟键盘事件，走与虚拟手柄（GamepadOverlay）同一条注入通道：

```
蓝牙手柄按键/摇杆
  → RayverseActivity.dispatchKeyEvent / dispatchGenericMotionEvent 最前端拦截
  → GamepadInputMapper 查映射表（key / axis+dir 两种 Binding）
  → 命中 → 翻译成虚拟 Android keycode（与 GamepadOverlay 同一套 9 键）
  → SDLActivity.onNativeKeyDown/Up → SDL_KEYDOWN/UP
  → linux_process_keyboard_event → DOS scancode → Touche_Enfoncee[] → 游戏
```

原生侧 `process_input()`（src/linux_main.c）只消费 `SDL_KEYDOWN/UP`，因此物理手柄按钮此前完全无效；本方案后键盘通道即可覆盖全部游戏动作。

## 动作表与默认映射（Xbox 布局基准）

| 动作 | 虚拟键（与 GamepadOverlay 一致） | 默认绑定 |
|---|---|---|
| LEFT/RIGHT/UP/DOWN | DPAD_LEFT/RIGHT/UP/DOWN | 十字键 + AXIS_X/Y + AXIS_HAT_X/Y |
| JUMP（跳跃） | CTRL_LEFT | BUTTON_A（+CONFIRM 同键） |
| PUNCH（拳击） | ALT_LEFT | BUTTON_X |
| GRAB（抓） | KEYCODE_X | BUTTON_Y / L1 / R1 |
| CONFIRM（确认） | ENTER | BUTTON_A |
| MENU（菜单/返回） | ESCAPE | BUTTON_B / START / SELECT |

一个动作可挂多个绑定，一个物理键也可绑多个动作（如 A = 跳 + 确认）。

## 文件

新增（`android/app/src/main/java/com/rayverse/rayman/`）：

- `MappingStore.java` — SharedPreferences `rayverse_controller_prefs`：`selected_device`（InputDevice descriptor，空=任意手柄）、`mapping_json`（`{"version":1,"actions":{...}}`，损坏/全空回退默认）、`auto_hide_overlay`（手柄在线时隐藏虚拟手柄，默认 true）、`config_version`（写入自增，供热重载）
- `GamepadInputMapper.java` — 运行时映射器：keycode→actions 反向索引；轴数字化状态机（进入阈值 0.5 / 回差 0.3）；虚拟键引用计数 + token 跟踪防卡键；设备断开/onPause/重载时 `releaseAll()`；`InputDeviceListener` 热插拔回调联动虚拟手柄显隐
- `ControllerSettingsActivity.java` — 设置 UI（纯原生 View）：① 已连接设备列表 ② 手柄选择 ③ 每动作一行 + "改"（捕获对话框：`dispatchKeyEvent` DOWN 且 repeatCount==0 即追加绑定，摇杆 |v|>0.7 判一次 <0.3 解锁，BACK 取消，可清空本项）④ 恢复默认 / 测试模式 / 自动隐藏开关 ⑤ 配对说明

修改：

- `SetupActivity.java` — 主菜单加「Controller settings」入口
- `RayverseActivity.java` — onCreate 建 mapper；覆写 `dispatchKeyEvent`/`dispatchGenericMotionEvent` 先交 mapper 未命中走 super；onPause `releaseAll()`；onResume `reloadIfChanged()`；onDestroy 注销
- `AndroidManifest.xml` — 声明 ControllerSettingsActivity（无独立桌面图标）

## 已知限制

- HOME / 音量 / 电源等系统保留键无法捕获
- 游戏内引擎键位重绑定（tab_key / menu.c）与 Java 映射层相互独立
- 手柄在线时自动隐藏虚拟手柄的游戏按键（可在设置中关闭）；右上角齿轮设置按钮始终保留，即时存档/读档入口不受影响
