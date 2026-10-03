# Rayverse for Android v1.0.2

**中文**：Rayman 1（1995 年 PC/DOS 版）的 C 语言反编译工程 [rayverse](https://github.com/pdromnt/rayverse) 的 Android 移植。本 APK 只包含引擎，不含任何游戏素材。

**English**: An Android port of [rayverse](https://github.com/pdromnt/rayverse), the C decompilation of Rayman 1 (1995, PC/DOS). This APK is the engine only — it ships no game assets.

---

## 更新内容 / What's new in 1.0.2

- **新增存档导出/导入（备份与跨设备迁移）**：启动后进入新的首页菜单，含 `Play / Export saves / Import saves / Change game data folder`。`Export saves` 可把存档（`RAYMAN*.SAV`、`RAYMAN.CFG`）复制到你选择的任意目录（含云盘/Syncthing 同步目录）；`Import saves` 从所选目录读回并覆盖本机存档（导入前有确认框）。跨设备共享即"导出到同步目录 → 另一台导入"。
- **New: save export / import (backup & cross-device transfer).** Launching now opens a home menu with `Play / Export saves / Import saves / Change game data folder`. `Export saves` copies your saves (`RAYMAN*.SAV`, `RAYMAN.CFG`) to any folder you pick (including a cloud/Syncthing-synced folder); `Import saves` reads them back and overwrites the local saves (with a confirmation prompt). Sharing between phones = export to a synced folder, then import on the other device.
- **启动行为变化**：配置过游戏目录后，启动不再直接进游戏，而是先显示首页菜单，点 `Play` 进入（多一次点击）。这样导入的存档能在引擎读盘前就位。/ **Launch behavior change:** once a game folder is configured, launching shows the home menu first instead of booting straight into the game; tap `Play` to start (one extra tap). This guarantees imported saves are in place before the engine reads them.

> 注意 / Note：和原版一致，游戏**不会**在关卡中途自动存档；进度只在「新游戏开始」或世界地图的存档点（save game）触发时写入。导出的存档反映最近一次存档点。/ As in the original, there is **no** mid-level autosave; an export reflects the most recent save point.

## 当前状态 / Status

- 已在真机（Samsung Galaxy S20 / Android 13 / arm64）验证：存档导出/导入、首页菜单、进游戏与读档均正常；存档跨重启保留（v1.0.1 修复延续）。
- Verified on a real device (Samsung Galaxy S20 / Android 13 / arm64): save export/import, the home menu, launching and loading all work; saves persist across restarts (carried over from the v1.0.1 fix).
- 后续关卡尚未逐一过机，进度以通关原版为准 / Later levels have not been play-tested one by one yet.

## 安装 / Installation

**中文**

1. 设备要求：Android 5.0（API 21）及以上，ARM64。
2. 准备游戏数据（**需自备原版 Rayman 1 PC 版**），把整个 `RAYMAN` 目录放到手机存储的**根目录**下，至少要包含：
   - `RAYMAN.EXE`（引擎用它定位并校验数据）
   - `PCMAP/`（`ALLFIX.DAT`、`*.WLD`、`*.LEV` 等）
   - 可选：`INTRO.DAT`、`CONCLU.DAT`、`Music/*.ogg`（CD 音轨转出的 OGG）
3. 下载安装 `rayverse-v1.0.2-android-arm64.apk` 并安装（需允许"未知来源"）。
4. 首次启动显示目录选择页，点击 **Select Game Data Directory**，选中上面的 `RAYMAN` 目录。
5. 之后每次启动显示首页菜单，点 **Play** 进游戏；需要备份/迁移存档时用 **Export / Import saves**。

**English**

1. Requires Android 5.0 (API 21) or newer, ARM64.
2. Bring your own original Rayman 1 PC game data. Put the whole `RAYMAN` folder in the **root of your device storage**; it must contain:
   - `RAYMAN.EXE` (the engine locates and validates the data through it)
   - `PCMAP/` (`ALLFIX.DAT`, `*.WLD`, `*.LEV`, ...)
   - Optional:`INTRO.DAT`, `CONCLU.DAT`, `Music/*.ogg` (OGG rips of the CD audio tracks)
3. Download and install `rayverse-v1.0.2-android-arm64.apk` (allow "unknown sources").
4. On first launch, tap **Select Game Data Directory** and pick that `RAYMAN` folder.
5. Later launches show the home menu; tap **Play** to start, or use **Export / Import saves** to back up or transfer progress.

> 从 v1.0.1 升级 / Upgrading from v1.0.1：release 包签名一致，可直接覆盖安装；存档目录不受影响。/ The release signing key is unchanged, so you can install over v1.0.1; the save directory is unaffected.

## 操作 / Controls

屏幕上有虚拟手柄，位置可自定义：点右上角齿轮进入编辑模式，拖动按钮，再点齿轮保存。

A virtual gamepad is drawn on screen; tap the gear in the top-right corner to enter edit mode, drag the buttons, then tap the gear again to save.

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

## 已知限制 / Known limitations

- 仅编译 arm64；x86_64 模拟器和 32 位设备不可用 / arm64 only.
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

- debug 与 release 现在可并排安装：debug 包名为 `com.rayverse.rayman.debug`、绿色图标、标签 "Rayverse (debug)"；release 为 `com.rayverse.rayman`、粉色图标。/ Debug and release can now be installed side by side: debug is `com.rayverse.rayman.debug` with a green icon and the label "Rayverse (debug)"; release is `com.rayverse.rayman` with the pink icon.
- 存档导出/导入设计见 [`docs/android-save-export-import.md`](android-save-export-import.md)。/ Save export/import design: [`docs/android-save-export-import.md`](android-save-export-import.md).

Details: [`docs/build-android.md`](build-android.md) · controls: [`docs/controls-android.md`](controls-android.md) · device debugging notes: [`docs/android-debug-log.md`](android-debug-log.md)

## 版权 / Credits & license

- Engine: based on [pdromnt/rayverse](https://github.com/pdromnt/rayverse) (decompilation, keep the original license terms).
- SDL2 for Android rendering/input, GLES renderer.
- Rayman is a trademark of Ubisoft. **You must own the original game** — no game data is distributed here.

## 截图 / Screenshots

片头 / Intro：

![Ubisoft intro](https://github.com/pisces312/rayverse/releases/download/v1.0.2/screenshot-1-ubisoft.jpg)

标题菜单 / Title menu：

![Title menu](https://github.com/pisces312/rayverse/releases/download/v1.0.2/screenshot-2-title-menu.jpg)

存档选择 / Save slot select：

![Save slot select](https://github.com/pisces312/rayverse/releases/download/v1.0.2/screenshot-3-save-slot.jpg)

世界地图 / World map：

![World map](https://github.com/pisces312/rayverse/releases/download/v1.0.2/screenshot-4-world-map.jpg)

关卡内 / In-game：

![In-game](https://github.com/pisces312/rayverse/releases/download/v1.0.2/screenshot-5-ingame.jpg)
