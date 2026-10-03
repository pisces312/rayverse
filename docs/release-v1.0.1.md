# Rayverse for Android v1.0.1

**中文**：Rayman 1（1995 年 PC/DOS 版）的 C 语言反编译工程 [rayverse](https://github.com/pdromnt/rayverse) 的 Android 移植。本 APK 只包含引擎，不含任何游戏素材。

**English**: An Android port of [rayverse](https://github.com/pdromnt/rayverse), the C decompilation of Rayman 1 (1995, PC/DOS). This APK is the engine only — it ships no game assets.

---

## 更新内容 / What's new in 1.0.1

- **修复存档无法保存的问题**：此前在 Android 上存档（`RAYMAN*.SAV`）永远写不出来——`SaveGameOnDisk` 用相对路径调原生 `fopen`，而进程工作目录是只读的 `/`，写入静默失败，退出后进度全丢。现在写存档改走与读档相同的 `android_fopen` 路径，落到应用私有目录 `filesDir/rayverse_save/RAYMAN*.SAV`，进度可跨重启保留。该修复已在真机（debug 包）上验证：存档 → 杀进程 → 重进读档，进度保留。
- **Fixed: saved games were never persisted.** On Android the engine wrote `RAYMAN*.SAV` through a relative-path raw `fopen`, but the process working directory is the read-only `/`, so the write failed silently and all progress was lost on exit. Saves now go through the same `android_fopen` path as loads, resolving to the app-private `filesDir/rayverse_save/RAYMAN*.SAV`, so progress survives a restart. Verified on-device (debug build): save → force-stop → relaunch and load keeps the progress.

> 注意 / Note：和原版一致，游戏**不会**在关卡中途自动存档；进度只在「新游戏开始」或世界地图的存档点（save game）触发 `SaveGameOnDisk` 时写入。/ As in the original, there is **no** mid-level autosave; progress is written only when the game triggers a save (starting a new game, or the world-map "save game" spot).

## 当前状态 / Status

- 已在真机（Samsung Galaxy S20 / Android 13 / arm64）验证：片头动画、世界地图、第 1 关可正常游玩，退出后再次进入正常，存档/读档跨重启保留。
- Verified on a real device (Samsung Galaxy S20 / Android 13 / arm64): intro, world map and level 1 are playable, quitting then relaunching works, and save/load now persists across restarts.
- 后续关卡尚未逐一过机，进度以通关原版为准 / Later levels have not been play-tested one by one yet.

## 安装 / Installation

**中文**

1. 设备要求：Android 5.0（API 21）及以上，ARM64。
2. 准备游戏数据（**需自备原版 Rayman 1 PC 版**），把整个 `RAYMAN` 目录放到手机存储的**根目录**下，至少要包含：
   - `RAYMAN.EXE`（引擎用它定位并校验数据）
   - `PCMAP/`（`ALLFIX.DAT`、`*.WLD`、`*.LEV` 等）
   - 可选：`INTRO.DAT`、`CONCLU.DAT`、`Music/*.ogg`（CD 音轨转出的 OGG）
3. 下载安装 `rayverse-v1.0.1-android-arm64.apk` 并安装（需允许"未知来源"）。
4. 首次启动会显示目录选择页，点击 **Select folder**，选中上面的 `RAYMAN` 目录，然后点 **Start**。
5. 之后每次启动会自动记住该目录，直接进游戏。

**English**

1. Requires Android 5.0 (API 21) or newer, ARM64.
2. Bring your own original Rayman 1 PC game data. Put the whole `RAYMAN` folder in the **root of your device storage**; it must contain:
   - `RAYMAN.EXE` (the engine locates and validates the data through it)
   - `PCMAP/` (`ALLFIX.DAT`, `*.WLD`, `*.LEV`, ...)
   - Optional: `INTRO.DAT`, `CONCLU.DAT`, `Music/*.ogg` (OGG rips of the CD audio tracks)
3. Download and install `rayverse-v1.0.1-android-arm64.apk` (allow "unknown sources").
4. On first launch, tap **Select folder**, pick that `RAYMAN` directory, then tap **Start**.
5. The folder is remembered, so later launches go straight into the game.

> 从 v1.0 升级 / Upgrading from v1.0：release 包签名一致，可直接覆盖安装；存档目录不受影响。/ The release signing key is unchanged, so you can install over v1.0; the save directory is unaffected.

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
- 游戏退出路径：`M` 打开菜单 → Quit → 回到首页 → Exit。/ To leave the game: `M` → Quit → home screen → Exit.

## 已知限制 / Known limitations

- 仅编译 arm64；x86_64 模拟器和 32 位设备不可用 / arm64 only.
- 暂停（Ctrl+NumLock）、取消（Tab）、全屏（F11）暂未提供虚拟按键 / No virtual keys for pause, cancel or fullscreen yet.
- 退出游戏后进程会结束（引擎的内存池是一次性的），再次点图标会重新加载，约 1 秒 / The process exits when you quit; relaunching reloads the engine (~1 s).
- 切后台/旋转已锁定横竖屏信箱化，个别机型比例可能略有黑边 / Letterboxing is fixed to the original 320:200 aspect; some devices may show slightly larger bars.
- 音量/音乐依赖 `Music/*.ogg`，没有该目录时只有 PC 版音效 / Music requires the optional OGG rips; without them only the original PC sound effects play.

## 构建 / Building from source

```bash
cd android
gradle assembleDebug        # 调试包
gradle assembleRelease      # 签名发布包，签名参数取自 KEY_STORE / KEY_STORE_PASSWORD / KEY_ALIAS / KEY_PASSWORD 环境变量
```

Details: [`docs/build-android.md`](docs/build-android.md) · controls: [`docs/controls-android.md`](docs/controls-android.md) · device debugging notes: [`docs/android-debug-log.md`](docs/android-debug-log.md)

## 版权 / Credits & license

- Engine: based on [pdromnt/rayverse](https://github.com/pdromnt/rayverse) (decompilation, keep the original license terms).
- SDL2 for Android rendering/input, GLES renderer.
- Rayman is a trademark of Ubisoft. **You must own the original game** — no game data is distributed here.

## 截图 / Screenshots

片头 / Intro：

![Ubisoft intro](https://github.com/pisces312/rayverse/releases/download/v1.0.1/screenshot-1-ubisoft.jpg)

标题菜单 / Title menu：

![Title menu](https://github.com/pisces312/rayverse/releases/download/v1.0.1/screenshot-2-title-menu.jpg)

存档选择 / Save slot select：

![Save slot select](https://github.com/pisces312/rayverse/releases/download/v1.0.1/screenshot-3-save-slot.jpg)

世界地图 / World map：

![World map](https://github.com/pisces312/rayverse/releases/download/v1.0.1/screenshot-4-world-map.jpg)

关卡内 / In-game：

![In-game](https://github.com/pisces312/rayverse/releases/download/v1.0.1/screenshot-5-ingame.jpg)
