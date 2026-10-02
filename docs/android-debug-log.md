# Rayverse Android 移植问题记录

按"症状 → 根因 → 修复 → 验证方式"记录真机调试中遇到的问题，便于同类问题复用思路。构建步骤见 `docs/build-android.md`，键位见 `docs/controls-android.md`。

## 1. 进世界地图后立即闪退（SIGSEGV）

- **症状**：世界地图能弹出，约 1 帧后进程崩溃，logcat 停在 `DO_ANIM`。
- **根因**：`LOAD_ALL_FIX()`（`src/load.c`）解析 ALLFIX.DAT 结尾的特殊对象索引时只读了 **7 个 s32**，而实际有 **8 个**。少读一个让后面的取值整体错位：`mapobj` 拿到第 3 号 ETA 表（第 5 行只有 14 项），而奖章需要 `eta[5][39..59]`。`get_eta()` 返回行外内存，`obj->animations + obj->anim_index` 解引用野指针。
- **修复**：补上第 8 次 `mem_read`，赋给 `alpha_numbers`（`src/data.c` 中声明但从未被赋值的全局）。
- **验证方式**：用 Python 脚本直接解析 `ALLFIX.DAT` 尾部字节，确认 8 个索引 `[1,5,6,7,3,4,8,2]` 恰好读到文件末尾，且第 4 号表第 5 行给出的奖章动画编号（38/45/46-51/32）连贯。

## 2. "couldn't locate PCMAP/JUNGLE/RAY1.LEV"

- **症状**：世界地图正常，选关后提示找不到关卡文件。
- **根因**：不是路径问题。SAF 扫描端 `GameDataBridge.scanDir()` 只对一份 15 项的文件名白名单开 fd，白名单外的文件（各世界的 `RAYn.LEV` 等）根本没进 fd 表，native 侧 `fopen` 拦截未命中就失败。
- **修复**：改为对目录树里的**每个文件**都开 fd 并注册；`MAX_FD_ENTRIES` 从 128 提到 256。
- **验证方式**：logcat 中 `Rayverse-IO` 的 `fopen SAF hit` 行；一次扫描注册 100 个文件，`PCMAP/JUNGLE/` 下 22 个文件全部命中。

## 3. 按左方向键：头朝左却向右冲

- **症状**：右/上/下都正确，唯独左键让人朝左转向后高速向右弹。
- **排查**：先排除映射链路。SDL 的 Android 键码表是 `Android_Keycodes[keycode]`（`3rd/SDL/src/video/android/SDL_androidkeyboard.c`），`KEYCODE_DPAD_LEFT → SDL_SCANCODE_LEFT(80) → SC_LEFT(0x4B)`，与 `src/linux_main.c` 的 DOS 表逐格核对无误；在 `RAY_SWIP()` 里加日志打出实际状态：

  ```
  RAY me=1 se=0 flip=0 spd=224 etaL=224 etaR=32 anim=29 L=1 R=0
  ```

  `flip=0` 说明朝向与输入一致，但 `eta_t.speed_x_left` 是 **+224**。
- **根因**：`src/common.h` 里 `typedef char s8;`。AArch64 上 `char` 默认**无符号**（x86 Linux / MSVC 默等有符号，所以桌面版一直正常），数据段读出的 `0xE0` 被当作 224 而不是 -32。
- **修复**：改成 `typedef signed char s8;`。
- **连带影响**：所有 `s8` 字段之前都被读错，包括 `obj_t.offset_hy`（挂附/落地时的精灵偏移）、`world_info_t.color`、`cmd_context_depth`。这一条修复的收益远超方向键本身。
- **教训**：跨架构移植时不要依赖 plain `char` 的符号性，涉及二进制格式的结构体必须写死 `signed char` / `int8_t`。

## 4. 虚拟手柄动作键全部无效

- **症状**：关卡里只有方向键能用，跳/拳/抓无反应。
- **根因**：`GamepadOverlay` 发送 `SPACE / SHIFT_LEFT / ESCAPE / TAB`，而引擎读的 DOS 默认键是 `SC_CONTROL`（跳）/ `SC_ALT`（拳）/ `SC_X`（抓）（`src/data.c:1541-1547`）。键位表是按"菜单按键"设计的，从未对齐关卡内动作。另有一处无效设计：标注 "Run toggle" 的键，游戏根本没有跑步键（`RayEvts.run` 来自蓝精灵加成与 `force_run` 触发区）。
- **修复**：重映射为 `CTRL_LEFT / ALT_LEFT / X / ENTER`，新增第 9 个键 `ESCAPE`（选项菜单/返回）；顺带修了自定义布局兼容问题（旧 8 键布局缺第 9 键时整体回退默认布局，而不是把新键放在 (0,0)）。
- **验证**：真机试玩确认跳、抓（空按=做鬼脸）、确认键生效；拳在第一章无反应属正常，需要 `RayEvts.poing`（剧情授予）才生效。
- **附带**：Android 上全屏只保留 F11，避免 Alt+Enter 被当成两个手柄键误触发；scancode ≥ 0x100 的 SDL 扩展键直接丢弃，不再被 `& 0xFF` 折叠成随机 DOS 键。

## 5. 游戏内 Quit → 首页 Exit 后停在黑屏

- **症状**：不返回安卓桌面，屏幕黑住；进程仍存活，前台 activity 是 `SetupActivity`。
- **根因**：`SetupActivity.launchGame()` 只 `startActivity` 没有 `finish()`，任务栈是 `[SetupActivity, RayverseActivity]`。而 `SetupActivity.onCreate` 走"已保存 URI 直接进游戏"分支时提前 `return`，从未 `setContentView`。游戏结束、`RayverseActivity` 销毁后回到这个没有内容的 activity，即黑屏。
- **修复**：`launchGame()` 里 `startActivity` 之后调用 `finish()`，游戏退出时任务栈清空，直接回桌面。
- **相关日志**（说明 SDL 层退出本身是干净的）：

  ```
  V/SDL: Finished main function
  V/SDL: onPause() / surfaceDestroyed() / onStop() / onDestroy()
  E/SDL: SDLActivity thread ends (error=Try to release egl_surface with context probably still active)
  ```

  最后一条是 GLES 拆除顺序的告警，不影响退出。

## 6. 调试回路中的工具坑

| 问题 | 现象 | 处理 |
|------|------|------|
| `adb push` 整个目录 | `libc++abi: std::bad_alloc`，目标目录也不创建 | 本地打包成单个 `tar`，push 后用设备端 `tar -xf` 解包 |
| Git Bash 改写远端路径 | `/sdcard/x.tar` 变成 `D:/dev/git/sdcard/x.tar`，`secure_mkdirs() failed` | 加 `MSYS_NO_PATHCONV=1`，本地路径用 Windows 形式 |
| 截图损坏 | `adb shell screencap -p > f.png` 得到带 `efbbbf` 开头的伪文本 | 用 `adb exec-out screencap -p > f.png` |
| 构建"过快" | gradle 2-3 秒完成，怀疑没重编 | 核对产物 mtime，或 `find android/app/build -name "*.so" -newer src/common.h` |
| Gradle 缓存 | 改 `Android.mk` 后不生效 | 删 `app/build` + `app/.cxx` + `.gradle` 重来 |

## 7. 抓日志的标准做法

```bash
# 只取相关 tag，落盘后台跟踪
adb logcat -v time Rayverse:V Rayverse-DBG:V Rayverse-GL:V Rayverse-IO:V \
  Rayverse-SAF:V Rayverse-Setup:V SDL:V stdout:V stderr:V libc:F DEBUG:E AndroidRuntime:E '*:S' \
  > android/run.log 2>&1 &

# 关键日志标记
# Rayverse-DBG  键事件 key sc=… / 奖章 WDBG / ALLFIX LDBG / 移动状态 RDBG
# Rayverse-IO   fopen SAF hit / fd 注册
# Rayverse-SAF  Java 侧扫描（注意 GameDataBridge 这个 tag 默认不在过滤列表里）
```

定位原则：**先用日志确认事件是否到达、值是多少，再改代码**。第 3 条就是靠一行状态日志把"按键映射问题"直接翻案成类型定义问题，否则会一路在映射表里找不存在的 bug。
