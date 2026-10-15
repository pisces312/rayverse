# Rayverse for Android v1.0.4

**中文**：Rayman 1（1995 年 PC/DOS 版）的 C 语言反编译工程 [rayverse](https://github.com/pdromnt/rayverse) 的 Android 移植。本 APK 只包含引擎，不含任何游戏素材。

**English**: An Android port of [rayverse](https://github.com/pdromnt/rayverse), the C decompilation of Rayman 1 (1995, PC/DOS). This APK is the engine only — it ships no game assets.

---

## 更新内容 / What's new in 1.0.4

这一版只有一个改动，但它修的是 v1.0.3 里最难受的那个问题：**发布版（正式包）退出游戏后重进、再读即时存档会崩或卡死**。

- **修复：跨进程读档崩溃/冻结。** v1.0.3 里，在关卡中存好档 → 完全退出应用 → 重新打开 → 读同一个槽位，正式包会闪退或画面停住（只有正式包会，调试包不复现）。

  根因有点绕：引擎的日志系统会缓存一个系统库函数的地址，这个缓存落在即时存档要整段快照的内存区里。跨进程读档时，它被回滚成了**上一个进程**的地址，于是读档成功后打的第一条日志就跳到了一个已经不属于这个进程的地址上。

  现在改成弱引用：地址不再存在自家内存里，而是由系统在每次启动加载时填进跳转表（位于快照区之外）。**没有被回滚的东西，也就没有能指针失效的东西。**

  副作用为零：游戏内「查看调试日志」、连电脑时用 `adb shell setprop log.tag.Rayverse-SS VERBOSE` 现场开诊断日志，两个能力都还在。

- 构建侧：NDK 由 28.2 升到 29.0（只对源码编译者有影响，APK 行为与设备要求不变）。

### Fixes in 1.0.4

- **Fixed a crash/freeze when loading an instant savestate across process restarts in release builds.** Save in a level → kill the app → relaunch → load the same slot: the release package crashed or froze on a still frame (debug builds never reproduced it; only release did).

  Cause: the log subsystem cached the address of a system library function, and that cache lived inside the memory region the savestate snapshots wholesale. Restoring a slot across processes rolled it back to the **previous process's** address, so the first log line written after the successful load jumped to an address that no longer belonged to this process.

  It now uses a weak reference instead: the address is not held in our own memory but resolved by the loader into the jump table, which sits outside the snapshotted region. Nothing to roll back means nothing to invalidate.

  No functional regression: the in-app 「查看调试日志」 viewer and the `adb shell setprop log.tag.Rayverse-SS VERBOSE` field diagnostics both still work.

- Build-only: NDK 28.2 → 29.0. No change to APK behaviour or device requirements.

---

## 升级注意 / Upgrading from an earlier version

**之前版本存的即时存档槽位，这一版读不出来。** 这是保护机制在正常工作，不是坏掉了：槽文件头记录了存档时的内存布局尺寸，新版布局变了，引擎会拒绝跨版本还原（否则会回滚出大规模错误数据）。

- 升级后：**重新存档一次**即可，之后的跨进程读档正常。
- 游戏原版的 `RAYMAN*.SAV`（世界地图存档点）**不受影响**，和即时存档是两套独立机制。

> Slots written by an older build will be **rejected** by this one. That is the layout guard doing its job, not a breakage: the slot header records the snapshot size at write time, and the engine refuses to restore across a layout change (rolling back a mismatched layout would corrupt far more than it fixes). **Save again once** after upgrading; the original `RAYMAN*.SAV` save points are a separate mechanism and are unaffected.

---

## 当前状态 / Status

- 已在真机（Samsung S20，Android 13）与模拟器上验证：重存一档 → 杀进程 → 重进 → 读档，画面正常继续、无崩溃、无卡帧。Verifed on a real device (Samsung S20, Android 13) and on the emulator: save → kill process → relaunch → load resumes with no crash and no frozen frame.
- v1.0.3 的功能（即时存档/读档 3 槽、蓝牙手柄、游戏内调试日志查看器、存档导入导出）全部沿用，本版未改动。
- 后续关卡尚未逐一过机，进度以通关原版为准 / Later levels have not been play-tested one by one yet.

---

## 安装 / Installation

**中文**

1. 设备要求：Android 5.0（API 21）及以上，ARM64。
2. 准备游戏数据（**需自备原版 Rayman 1 PC 版**），把整个 `RAYMAN` 目录放到手机存储的**根目录**下，至少要包含：
   - `RAYMAN.EXE`（引擎用它定位并校验数据）
   - `PCMAP/`（`ALLFIX.DAT`、`*.WLD`、`*.LEV` 等）
   - 可选：`INTRO.DAT`、`CONCLU.DAT`、`Music/*.ogg`（CD 音轨转出的 OGG）
3. 下载安装 `rayverse-v1.0.4-android-arm64.apk`（需允许"未知来源"）。
4. 首次启动显示目录选择页，点击 **Select Game Data Directory**，选中上面的 `RAYMAN` 目录。
5. 之后每次启动显示首页菜单，点 **Play** 进游戏；即时存档在游戏内齿轮菜单里。

**English**

1. Requires Android 5.0 (API 21) or newer, ARM64.
2. Bring your own original Rayman 1 PC game data. Put the whole `RAYMAN` folder in the **root of your device storage**; it must contain:
   - `RAYMAN.EXE` (the engine locates and validates the data through it)
   - `PCMAP/` (`ALLFIX.DAT`, `*.WLD`, `*.LEV`, ...)
   - Optional: `INTRO.DAT`, `CONCLU.DAT`, `Music/*.ogg` (OGG rips of the CD audio tracks)
3. Download and install `rayverse-v1.0.4-android-arm64.apk` (allow "unknown sources").
4. On first launch, tap **Select Game Data Directory** and pick that `RAYMAN` folder.
5. Later launches show the home menu; tap **Play** to start. Instant savestates live in the in-game gear menu.

> 签名密钥未变，可直接覆盖安装 / The signing key is unchanged, so you can install over an older version.

---

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

- 即时存档 / 读档：关卡内点齿轮 →「即时存档 / 即时读档」→ 选槽位。/ Save/load instant states: in-level gear → 「即时存档 / 即时读档」→ pick a slot.
- 菜单里的 YES / NO：**左/右**切换选中项（当前选中项显示为红色），**OK** 确认 / In YES–NO prompts use **left/right** to move the selection (the active item is drawn in red), **OK** to confirm.
- 没有"跑步键"：跑由加成和触发区给出，不是按键。/ There is no run button — running comes from power-ups and trigger zones, as in the original.
- 退出游戏：`M` → Quit → 回首页菜单。/ To leave the game: `M` → Quit → back to the home menu.

---

## 已知限制 / Known limitations

- 仅编译 arm64；x86_64 模拟器和 32 位设备不可用 / arm64 only.
- 即时存档只能在关卡游玩中使用，不能从标题/世界地图/首页恢复 / Instant savestates only work while playing a level; there is no boot-time auto-resume.
- 即时存档固定 3 个槽位，不做自动轮转 / Exactly 3 instant-savestate slots, no auto-rotation.
- **即时存档槽跨版本不兼容**（见上方"升级注意"）/ Instant-savestate slots are **not** compatible across versions (see Upgrading above).
- 暂停（Ctrl+NumLock）、取消（Tab）、全屏（F11）暂未提供虚拟按键 / No virtual keys for pause, cancel or fullscreen yet.
- 退出游戏后进程会结束，再次点图标会重新加载，约 1 秒 / The process exits when you quit; relaunching reloads the engine (~1 s).
- 切后台/旋转已锁定横竖屏信箱化，个别机型比例可能略有黑边 / Letterboxing is fixed to the original aspect; some devices may show slightly larger bars.
- 音量/音乐依赖 `Music/*.ogg`，没有该目录时只有 PC 版音效 / Music requires the optional OGG rips.
- 存档导出/导入为手动操作，没有自动云同步 / Save export/import is manual; there is no automatic cloud sync.

---

## 构建 / Building from source

```bash
cd android
gradle assembleDebug        # 调试包（包名 com.rayverse.rayman.debug，绿色图标）
gradle assembleRelease      # 签名发布包，签名参数取自 KEY_STORE / KEY_STORE_PASSWORD / KEY_ALIAS / KEY_PASSWORD 环境变量
```

- 需要 **NDK 29.0.14206865**（版本钉在 `android/app/build.gradle.kts` 的 `ndkVersion`）；仓库没有可执行的 Gradle wrapper，请用系统 Gradle 9.4.1，参见 [`docs/build-android.md`](build-android.md)。
- Requires **NDK 29.0.14206865** (pinned via `ndkVersion`); there is no runnable Gradle wrapper in the repo — use system Gradle 9.4.1.
- debug 与 release 可并排安装：debug 包名 `com.rayverse.rayman.debug`、绿色图标；release 为 `com.rayverse.rayman`、粉色图标。/ Debug and release install side by side.
- 即时存档设计见 [`docs/savestate-phase2.md`](savestate-phase2.md)；本次崩溃的根因排查与修复见 [`docs/savestate-cross-process-crash.md`](savestate-cross-process-crash.md) 与 `docs/2026-10-15_0050_emulator-repro-is-loggable-dangling-fnptr.md`。

Details: [`docs/build-android.md`](build-android.md) · controls: [`docs/controls-android.md`](controls-android.md) · device debugging notes: [`docs/android-debug-log.md`](android-debug-log.md)

---

## 版权 / Credits & license

- Engine: based on [pdromnt/rayverse](https://github.com/pdromnt/rayverse) (decompilation, keep the original license terms).
- SDL2 for Android rendering/input, GLES renderer.
- Rayman is a trademark of Ubisoft. **You must own the original game** — no game data is distributed here.

---

## 截图 / Screenshots

沿用 v1.0.3 的截图（界面本版未变）/ Interface is unchanged from v1.0.3, reusing its screenshots.

关卡内 / In-game：

![In-game](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-5-ingame.jpg)

世界地图 / World map：

![World map](https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-4-world-map.jpg)
