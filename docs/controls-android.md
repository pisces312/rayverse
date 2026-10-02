# Rayverse 操作按键说明

本文列出引擎实际读取的按键、Android 虚拟手柄的键位，以及按键事件的传递链路。

## 输入传递链路（Android）

```
GamepadOverlay.java        SDLActivity.onNativeKeyDown(keycode)
      │  KeyEvent.KEYCODE_*
      ▼
SDL  src/video/android/SDL_androidkeyboard.c   Android_Keycodes[keycode]
      │  SDL_Scancode
      ▼
移植层 src/linux_main.c    sdl_scancode_to_dos_scancode[]
      │  DOS/BIOS scancode（增强键码，如 0x4B）
      ▼
引擎 src/input.c           PC_keyboard_interrupt_handler → Touche_Enfoncee[128]
                             leftjoy()/rightjoy()/but0pressed() …
```

- DOS 键码默认值定义在 `src/data.c`（`key_left` / `key_right` / `key_up` / `key_down` / `key_jump` / `key_fist` / `key_action`）。
- 游戏内动作与按键的绑定还经过 `options_jeu`（`POINTEUR_BOUTONS_OPTIONS_BIS`，`src/input.c`），出厂默认 `jump=1`、`fist=0`、`action=2`。

## 引擎按键表

| 功能 | DOS 键 (scancode) | 引擎入口 |
|------|-------------------|----------|
| 移动 左 / 右 / 上 / 下 | ← (0x4B) / → (0x4D) / ↑ (0x48) / ↓ (0x50) | `leftjoy()` `rightjoy()` `upjoy()` `downjoy()` |
| 跳跃 | Ctrl (0x1D) | `but1pressed()` → `options_jeu.test_fire1` |
| 出拳 / 蓄拳 | Alt (0x38) | `but0pressed()` → `options_jeu.test_fire0` |
| 抓握 / 悬吊 / 攀爬 | X (0x2D) | `but2pressed()` → `options_jeu.test_button3` |
| 确认（标题画面、世界地图选关、对话框） | Enter (0x1C) 或 Space (0x39) | `ValidButPressed()` |
| 游戏内选项菜单 / 返回 / 存档选择 | Esc (0x01) | `GoMenu`、`ExitButPressed()`、`SelectButPressed()` |
| 选项中取消 | Tab (0x0F) | `CancelButPressed()` |
| 暂停 | 先按 Ctrl，再按 NumLock (0x45) | `PC_keyboard_interrupt_handler` 里的 `byte_967CC` 序列 |
| 全屏切换 | F11；桌面版另支持 Alt+Enter | `toggle_fullscreen()`（仅非 Android 编译时接受 Alt+Enter） |

### 没有"跑步键"

`RayEvts.run` 不由键盘直接控制，而是来自加成与触发区：

- 蓝精灵（Fee）奖励：`src/fee.c`
- 强制跑动区 `RayEvts.force_run`：`src/dark.c`、`src/ray.c`（`RAY_RESPOND_TO_ALL_DIRS` 中 `force_run != 0` 时忽略左方向键）

因此早期版本里标的 "Y - Run toggle" 是无效键位。

## Android 虚拟手柄键位

按键定义见 `android/app/src/main/java/com/rayverse/rayman/GamepadOverlay.java` 的 `KEY_CODES`。

| 虚拟键 | 屏幕标签 | Android keycode | SDL scancode | DOS 键 | 作用 |
|--------|----------|-----------------|--------------|--------|------|
| BTN_UP | ▲ | `KEYCODE_DPAD_UP` | `SDL_SCANCODE_UP` | ← 引擎 SC_UP | 上 |
| BTN_DOWN | ▼ | `KEYCODE_DPAD_DOWN` | `SDL_SCANCODE_DOWN` | SC_DOWN | 下 |
| BTN_LEFT | ◀ | `KEYCODE_DPAD_LEFT` | `SDL_SCANCODE_LEFT` | SC_LEFT | 左 |
| BTN_RIGHT | ▶ | `KEYCODE_DPAD_RIGHT` | `SDL_SCANCODE_RIGHT` | SC_RIGHT | 右 |
| BTN_A | J | `KEYCODE_CTRL_LEFT` | `SDL_SCANCODE_LCTRL` | SC_CONTROL | 跳跃 |
| BTN_B | P | `KEYCODE_ALT_LEFT` | `SDL_SCANCODE_LALT` | SC_ALT | 出拳 |
| BTN_X | G | `KEYCODE_X` | `SDL_SCANCODE_X` | SC_X | 抓握 |
| BTN_Y | OK | `KEYCODE_ENTER` | `SDL_SCANCODE_RETURN` | SC_ENTER | 确认 |
| BTN_MENU | M | `KEYCODE_ESCAPE` | `SDL_SCANCODE_ESCAPE` | SC_ESCAPE | 选项菜单 / 返回 |

尚未提供虚拟键的动作：取消（Tab）、暂停（Ctrl+NumLock）、全屏（F11）。

## 自定义布局

- 点右上角齿轮进入编辑模式，拖动按钮调整位置，再点齿轮保存；坐标按屏幕宽高的比例存储（`SharedPreferences` 的 `btn_<i>_x/_y/_size`）。
- 竖屏时游戏画面按 320:200 信箱化，控制键落在画面下方的空白区；横屏时方向键在左下、动作键在右下、M 键居中。
- 存储的布局必须覆盖全部按钮才会生效；少一个键（例如从旧版本升级）会整体回退到默认布局。
- 齿轮菜单里的 "重置布局" 会清掉所有自定义坐标。

## 键码表的已知边界

`sdl_scancode_to_dos_scancode[]` 只有 256 项，SDL 的扩展键（`SDL_SCANCODE_AC_BACK` = 4112、媒体键、右 GUI 键等）超出范围；`src/linux_main.c` 会直接丢弃 scancode ≥ 0x100 的事件，不再用 `& 0xFF` 折叠成随机 DOS 键。
