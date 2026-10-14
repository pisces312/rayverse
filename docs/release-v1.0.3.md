# Rayverse for Android v1.0.3

**中文**：Rayman 1（1995 年 PC/DOS 版）的 C 语言反编译工程 [rayverse](https://github.com/pdromnt/rayverse) 的 Android 移植。本 APK 只包含引擎，不含任何游戏素材。

**English**: An Android port of [rayverse](https://github.com/pdromnt/rayverse), the C decompilation of Rayman 1 (1995, PC/DOS). This APK is the engine only — it ships no game assets.

---

## 更新内容 / What's new in 1.0.3

- **新增即时存档 / 即时读档（Instant savestate & loadstate）**：点右上角齿轮打开设置菜单，选「即时存档 / 即时读档」，有 3 个槽位，槽位里已有的内容会显示出来方便选择。存档写在应用存档目录里，**关掉应用再开也还在**；与原版的 `RAYMAN*.SAV` 互不影响。只能在关卡游玩中存/读（标题、世界地图界面不行）。/ **New: instant savestate & loadstate.** Tap the gear in the top-right corner and pick 「即时存档 / 即时读档」(save / load); there are 3 slots, each labeled with what it already holds. Slots live in the app's save directory and **persist across app restarts**; they are separate from the original `RAYMAN*.SAV` saves. Saving/loading only works while playing a level (not on the title or world-map screens).
- **新增蓝牙手柄支持（可重映射按键）**：主菜单新增 `Controller settings`，可连接蓝牙手柄并逐键重映射；接上手柄后虚拟按键自动隐藏（齿轮仍保留）。/ **New: Bluetooth gamepad support with remappable keys.** A `Controller settings` entry on the home menu lets you pair a gamepad and remap each button; the on-screen touch buttons hide themselves while a pad is connected (the gear stays).
- **新增游戏内调试日志查看器**：设置菜单 →「查看调试日志」，弹框显示日志全文并可一键复制，不便连电脑时排障用。/ **New: in-app debug log viewer** — settings menu → 「查看调试日志」; shows the full log in a dialog with copy-to-clipboard, for troubleshooting without a PC.
- **设置/首页界面横屏适配**，首次启动的目录选择页也支持直接导入存档。/ Setup and home screens reworked for landscape; the first-run screen also offers save import.

### 修复 / Fixes

- **启动不再弹"是否恢复存档"**：即时读档只在关卡内手动触发，应用启动后点 Play 直接进游戏。/ No more "resume?" prompt at launch — instant load is manual and in-level only; Play starts the game as usual.
- **修复关卡中读档后颜色严重失真的问题**（调色板在读档后未重设）。/ Fixed severe color distortion after loading a savestate mid-level (the palette was not re-applied after the restore).
- **修复旧版本即时存档读取闪退**：以前版本存的即时存档槽位现在可以正常读出（存档数据布局做了兼容处理，并修正了跨进程恢复时的地址重定位）。/ Fixed crashes when loading instant savestates written by older versions; old slots load again (snapshot layout compatibility plus corrected cross-process address relocation).

> 注意 / Note：即时存档是"整个游戏状态的快照"，和原版存档点（世界地图 save game）是两套机制；和原版一致，游戏**不会**在关卡中途自动写 `RAYMAN*.SAV`。/ Instant savestates are full-engine snapshots, a separate mechanism from the original save points; as in the original there is **no** mid-level autosave of `RAYMAN*.SAV`.

## 当前状态 / Status

- 已在真机（arm64 / Android 15）验证：即时存档/读档（含应用重启后读取、旧版本槽位读取）、蓝牙手柄、调试日志查看器、进游戏与原版存档读写均正常。
- Verified on a real device (arm64 / Android 15): instant save/load (including across app restarts and loading old-version slots), the Bluetooth gamepad, the debug log viewer, and normal gameplay / original save read-write all work.
- 后续关卡尚未逐一过机，进度以通关原版为准 / Later levels have not been play-tested one by one yet.

## 安装 / Installation

**中文**

1. 设备要求：Android 5.0（API 21）及以上，ARM64。
2. 准备游戏数据（**需自备原版 Rayman 1 PC 版**），把整个 `RAYMAN` 目录放到手机存储的**根目录**下，至少要包含：
   - `RAYMAN.EXE`（引擎用它定位并校验数据）
   - `PCMAP/`（`ALLFIX.DAT`、`*.WLD`、`*.LEV` 等）
   - 可选：`INTRO.DAT`、`CONCLU.DAT`、`Music/*.ogg`（CD 音轨转出的 OGG）
3. 下载安装 `rayverse-v1.0.3-android-arm64.apk` 并安装（需允许"未知来源"）。
4. 首次启动显示目录选择页，点击 **Select Game Data Directory**，选中上面的 `RAYMAN` 目录。
5. 之后每次启动显示首页菜单，点 **Play** 进游戏；备份/迁移存档用 **Export / Import saves**，即时存档在游戏内齿轮菜单里。

**English**

1. Requires Android 5.0 (API 21) or newer, ARM64.
2. Bring your own original Rayman 1 PC game data. Put the whole `RAYMAN` folder in the **root of your device storage**; it must contain:
   - `RAYMAN.EXE` (the engine locates and validates the data through it)
   - `PCMAP/` (`ALLFIX.DAT`, `*.WLD`, `*.LEV`, ...)
   - Optional:`INTRO.DAT`, `CONCLU.DAT`, `Music/*.ogg` (OGG rips of the CD audio tracks)
3. Download and install `rayverse-v1.0.3-android-arm64.apk` (allow "unknown sources").
4. On first launch, tap **Select Game Data Directory** and pick that `RAYMAN` folder.
5. Later launches show the home menu; tap **Play** to start. Use **Export / Import saves** to back up or transfer progress; instant savestates are in the in-game gear menu.

> 从旧版本升级 / Upgrading from an earlier release：release 包签名一致，可直接覆盖安装；原版存档目录不受影响，旧的即时存档槽位也兼容。/ The release signing key is unchanged, so you can install over an older version; the original save directory is unaffected, and old instant-savestate slots stay compatible.

## 操作 / Controls

屏幕上有虚拟手柄，位置可自定义：点右上角齿轮进入编辑模式，拖动按钮，再点齿轮保存。接蓝牙手柄后虚拟按键自动隐藏。

A virtual gamepad is drawn on screen; tap the gear in the top-right corner to enter edit mode, drag the buttons, then tap the gear again to save. The touch buttons hide themselves when a Bluetooth gamepad is connected.

| 虚拟键 / Button | 作用 / Action |
|-----------------|----------------|
| ▲ ▼ ◀ ▶ | 移动 / Move（含攀爬、进入方向） |
| **J** | 跳跃 / Jump |
| **P** | 出拳、蓄拳 / Punch, charge punch |
| **G** | 抓握、悬吊、攀爬 / Grab, hang, climb |
| **OK** | 确认（标题画面、选关、对话）/ Confirm (title, level select, dialogs) |
| **M** | 游戏内选项菜单 / 返回 / In-game options menu, back |

- 菜单里的 YES / NO：**左/右**切换选中项（当前选中项显示为红色），**OK** 确认 / In YES–NO prompts use **left/right** to move the selection (the active item is drawn in red), **OK** to confirm.
- 没有"跑步键"：原版引擎的跑步由加成（蓝精灵）和触发区给出，不是按键。/ There is no run button — running comes from power-ups and trigger zones, as in the original.
- 游戏退出路径：`M` 打开菜单 → Quit → 回到首页菜单 → 可再 Play 或导出存档。/ To leave the game: `M` → Quit → back to the home menu.
- 蓝牙手柄按键可在首页 `Controller settings` 里逐键重映射。/ Gamepad buttons can be remapped one by one in `Controller settings` on the home menu.

## 已知限制 / Known limitations

- 仅编译 arm64；x86_64 模拟器和 32 位设备不可用 / arm64 only.
- 即时存档只能在关卡游玩中使用，不能从标题/世界地图/首页恢复；没有启动时自动恢复 / Instant savestates only work while playing a level; there is no boot-time auto-resume.
- 即时存档固定 3 个槽位，不做自动轮转 / Exactly 3 instant-savestate slots, no auto-rotation.
- 暂停（Ctrl+NumLock）、取消（Tab）、全屏（F11）暂未提供虚拟按键 / No virtual keys for pause, cancel or fullscreen yet.
- 退出游戏后进程会结束（引擎的内存池是一次性的），再次点图标会重新加载，约 1 秒 / The process exits when you quit; relaunching reloads the engine (~1 s).
- 切后台/旋转已锁定横竖屏信箱化，个别机型比例可能略有黑边 / Letterboxing is fixed to the original 320:200 aspect; some devices may show slightly larger bars.
- 音量/音乐依赖 `Music/*.ogg`，没有该目录时只有 PC 版音效 / Music requires the optional OGG rips; without them only the original PC sound effects play.
- 存档导出/导入为手动操作，没有自动云同步 / Save export/import is manual; there is no automatic cloud sync.

## 构建 / Building from source

```bash
cd android
gradle assembleDebug        # 调试包（包名 com.rayverse.rayman.debug，绿色图标）
gradle assembleRelease      # 签名发布包，签名参数取自 KEY_STORE / KEY_STORE_PASSWORD / KEY_ALIAS / KEY_PASSWORD 环境变量
```

- debug 与 release 可并排安装：debug 包名为 `com.rayverse.rayman.debug`、绿色图标、标签 "Rayverse (debug)"；release 为 `com.rayverse.rayman`、粉色图标。/ Debug and release install side by side: debug is `com.rayverse.rayman.debug` with a green icon and the label "Rayverse (debug)"; release is `com.rayverse.rayman` with the pink icon.
- 存档导出/导入设计见 [`docs/android-save-export-import.md`](android-save-export-import.md)；即时存档设计见 [`docs/savestate-phase2.md`](savestate-phase2.md)。/ Save export/import design: [`docs/android-save-export-import.md`](android-save-export-import.md); instant savestate design: [`docs/savestate-phase2.md`](savestate-phase2.md).

Details: [`docs/build-android.md`](build-android.md) · controls: [`docs/controls-android.md`](controls-android.md) · device debugging notes: [`docs/android-debug-log.md`](android-debug-log.md)

## 版权 / Credits & license

- Engine: based on [pdromnt/rayverse](https://github.com/pdromnt/rayverse) (decompilation, keep the original license terms).
- SDL2 for Android rendering/input, GLES renderer.
- Rayman is a trademark of Ubisoft. **You must own the original game** — no game data is distributed here.

## 截图 / Screenshots

片头 / Intro：

![Ubisoft intro](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-1-ubisoft.jpg)

标题菜单 / Title menu：

![Title menu](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-2-title-menu.jpg)

存档选择 / Save slot select：

![Save slot select](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-3-save-slot.jpg)

世界地图 / World map：

![World map](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-4-world-map.jpg)

关卡内 / In-game：

![In-game](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-5-ingame.jpg)
