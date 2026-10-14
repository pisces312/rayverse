# Rayverse Android 移植问题记录

按"症状 → 根因 → 修复 → 验证方式"记录真机调试中遇到的问题，便于同类问题复用思路。构建步骤见 `docs/build-android.md`，键位见 `docs/controls-android.md`。

## 1. 进世界地图后立即闪退（SIGSEGV）

- **症状**：世界地图能弹出，约 1 帧后进程崩溃，logcat 停在 `DO_ANIM`。
- **根因**：`LOAD_ALL_FIX()`（`src/load.c`）解析 ALLFIX.DAT 结尾的特殊对象索引时只读了 **7 个 s32**，而实际有 **8 个**。少读一个让后面的取值整体错位：`mapobj` 拿到第 3 号 ETA 表（第 5 行只有 14 项），而奖章需要 `eta[5][39..59]`。`get_eta()` 返回行外内存，`obj->animations + obj->anim_index` 解引用野指针。
- **修复**：补上第 8 次 `mem_read`，赋给 `alpha_numbers`（`src/data.c` 中声明但从未被赋值的全局）。
- **验证方式**：用 Python 脚本直接解析 `ALLFIX.DAT` 尾部字节，确认 8 个索引 `[1,5,6,7,3,4,8,2]` 恰好读到文件末尾，且第 4 号表第 5 行给出的奖章动画编号（38/45/46-51/32）连贯。这个解析器已收进 `tools/parse_allfix.py`（`LOAD_ALL_FIX()` 的 Python 复刻），以后查对象/动画数据直接用它，不要再写一次性脚本。

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
- **修复**：`SetupActivity` 加 `gameLaunched` 标记，在 `onStop()` 里 `finish()`（此时游戏已在栈顶，安全）。
- **踩过的坑**：直接在 `launchGame()`（由 `onCreate` 调用）里 `finish()` 会**把刚启动的 `RayverseActivity` 一起杀掉**——trampoline 在子 activity 真正起来之前结束自己，任务被折叠。日志特征：`Running main function` 后 1 ms 就 `Finished main function` + `onPause/onStop/onDestroy`。

## 6. 退出后再次点图标：闪回桌面 / 又回到片头

- **症状 A**：退出过一次，再点图标立刻回桌面。
- **症状 B**（第一次修复后）：Exit 之后游戏自己回到 Ubi Soft 片头，像"重启了一遍"。
- **根因**：引擎是**一次性程序**。`PcMain()` 末尾的 `FIN_GAME_LOOP()`/`END_GAME()` 已经 `block_free` 掉 `main_mem_level/world/sprite/fix` 与 `temp_mem_buf`；而 `main()` 里 `if (!app_state->game.initialized) game_init(...)` 因为全局仍是 1 而跳过，arena 永不重建 → 同进程第二次 `SDL_main` 直接返回。
  另一方面，SDL 在 `SDL_main` 返回后只 `finish()` activity 不杀进程，进程被系统缓存复用；而在 activity 仍处于 resumed 时强杀进程，ActivityManager 会**重建任务栈**，于是 `SetupActivity` 又被拉起、再次进游戏（症状 B）。
- **修复**：`RayverseActivity.onDestroy()` 中，`isFinishing()` 为真时 `killProcess(myPid()) + System.exit(0)`；配合第 5 条让 `SetupActivity` 提前离栈，任务栈里只剩游戏 activity，杀进程后无栈可重建。下次点图标是真正的冷启动。
- **验证**（一次完整的 Exit→再进）：

  ```
  main: main_Ray returned (0) → V/SDL: Finished main function
  V/SDL: onDestroy() → I/Rayverse: Game over, ending process
  I/ActivityManager: Process com.rayverse.rayman (pid 7936) has died: cch CRE
  topResumedActivity=com.sec.android.app.launcher/.activities.LauncherActivity
  ```

  再点图标：新 pid、`Game data OK (100 files)`、`Running main function` 且不再立刻 `Finished`。
- **顺带**：同进程重复注册 fd 会把表填满（`fd table full, cannot register 'PCMAP/RAY1.WLD'`）。`nativeRegisterFd` 改为按路径去重，命中旧项就 `close` 掉旧 fd（游戏侧持有的是 `dup()` 过的描述符）并复用表项；表真满时 `close(fd)` 防泄漏。
- **无害告警**：退出时 `E/SDL: SDLActivity thread ends (error=Try to release egl_surface with context probably still active)` 是 GLES 拆除顺序问题，不影响退出。

## 7. 即时存档只抓到 18K 数据段（savestate）

- **症状**：真机点"即时存档"提示失败；模拟器复现时存档"成功"，但日志显示 `data segment base=0x…710 size=18K` —— 引擎全局变量远不止 18K。
- **根因**：`libmain.so` 有**两个** PF_W 的 PT_LOAD：先是 RELRO 块（`.data.rel.ro/.got`，18K，重定位完成后被 linker `mprotect` 成只读），后才是真正的 `.data + .bss`（约 395K）。`ss_phdr_cb` 取"第一个可写段"命中了 RELRO：快照漏掉全部引擎全局；读档往只读页 memcpy 还会直接 SIGSEGV（真机的"失败"即此路径）。
- **修复**：`src/savestate.c` 的 `ss_phdr_cb` 改为遍历全部 PF_W PT_LOAD、取 **memsz 最大**的那段。
- **验证**（pixel6 模拟器，x86_64 + ndk_translation）：`llvm-readelf -l libmain.so` 确认 18K/395K 两段；修复后存档日志 `segment=395K`，关卡内 存档→走动/死亡→读档 位置与 Lum 状态精确还原、无崩溃，读档后引擎正常运行。
- **通用教训**：用 `dl_iterate_phdr` 找数据段时，"第一个 PF_W 的 PT_LOAD"≠数据段，Android 链接器会把 RELRO 单独成段；要么按最大 memsz 选，要么跳过 GNU_RELRO 覆盖的范围。

## 8. 调试回路中的工具坑

| 问题 | 现象 | 处理 |
|------|------|------|
| `adb push` 整个目录 | `libc++abi: std::bad_alloc`，目标目录也不创建 | 本地打包成单个 `tar`，push 后用设备端 `tar -xf` 解包 |
| Git Bash 改写远端路径 | `/sdcard/x.tar` 变成 `D:/dev/git/sdcard/x.tar`，`secure_mkdirs() failed` | 加 `MSYS_NO_PATHCONV=1`，本地路径用 Windows 形式 |
| 截图损坏 | `adb shell screencap -p > f.png` 得到带 `efbbbf` 开头的伪文本 | 用 `adb exec-out screencap -p > f.png` |
| 构建"过快" | gradle 2-3 秒完成，怀疑没重编 | 核对产物 mtime，或 `find android/app/build -name "*.so" -newer src/common.h` |
| Gradle 缓存 | 改 `Android.mk` 后不生效 | 删 `app/build` + `app/.cxx` + `.gradle` 重来 |
| `adb shell input keyevent` 驱动游戏 | 按键日志显示 down/up 只差 1 ms，引擎按 60 Hz 轮询 `Touche_Enfoncee`，经常一整帧都采不到 | 用 `input keyevent --longpress <code>`（按住 ~500 ms），每步截图确认；YES/NO 弹窗里"红色"才是当前选中项，需先用左/右切换 |
| 注入按键走到存档选择页就失效 | `CHOOSE A GAME` 页上 `key sc=40 down/up` 日志齐全，但画面不动；人手点 START 正常 | 已定位（2026-10-06）：① 该页 Enter 一次只推进一格名字输入（`SAISIE_NOM` 的 `positionx`），且此子页忽略左右方向键，看似"无反应"；新建档要连按 3 次 Enter 出确认勾再回选择页，第 4 次 Enter 才真正 START。② 快速 down/up（同毫秒）在低帧率下整帧采不到，必须 `--longpress`。另注意虚拟 OK 按钮与 START 弹窗区域重叠，点屏幕会先被 overlay 吃掉 |
| `uiautomator dump` 看不到本应用的对话框 | dump 出来是 2807 字节、没有任何 `text=` 的空树，误判"对话框没弹出"，白点了好几轮 | 该 dump 对 **Java `AlertDialog` 本来就取不到文本**（SDL/GL 表面也取不到游戏内画面）。判断对话框在不在只看截图；坐标按 `emu_p2a_33_labels.png` 这类 1:1 截图量，别信 dump 的 bounds |
| `adb shell pm clear <pkg>` | 之后首次启动要求重新授权文件夹，且**真机上的原生存档一起没了** | 不要用。重装只需 `adb install -r`，SAF 授权和 `files/rayverse_save/` 都保留；确实要清就先把 `files/` 用 `run-as … tar` 备份出来 |
| 真机和模拟器同时在线 | 所有 adb 命令报 `more than one device/emulator` | 一律 `adb -s emulator-5554`（真机是 `RFCNC0MT5VV`），脚本里把 `-s` 写死，别用默认设备 |
| 读 `/proc/self/maps` 分类地址 | 某个桶恒为 0，被当成"快照里没有这一类指针" | `name[72]` + 路径列解析错位把 `/data/app/~~…==/lib/arm64/libmain.so` 截断在文件名之前 → 模块区间从未登记。**计数为 0 先证明这一类被登记过**（本轮靠日志里的 `regions=13` vs 应有的 14 暴露） |

## 9. 抓日志的标准做法

日志级别是**运行时可配**的：debug 包默认全开，release 包默认只留 ERROR 以上。想在 release 包里看细节，先按 tag 打开再重启应用：

```bash
adb shell setprop log.tag.Rayverse-DBG VERBOSE   # 也可 DEBUG / INFO / WARN / ERROR
adb shell am force-stop com.rayverse.rayman      # 关掉再进，级别按进程缓存
```

属性删不掉（`setprop … ""` 会报 usage），要恢复默认就设回 `ERROR`，或者重启设备。

```bash
# 只取相关 tag，落盘后台跟踪
adb logcat -v time Rayverse:V Rayverse-DBG:V Rayverse-GL:V Rayverse-IO:V \
  GameDataBridge:V Rayverse-Setup:V SDL:V stdout:V stderr:V libc:F DEBUG:E AndroidRuntime:E '*:S' \
  > android/run.log 2>&1 &

# 关键日志标记
# Rayverse-DBG   键事件 key sc=… / 奖章 WDBG / ALLFIX LDBG / 移动状态 RDBG
# Rayverse-IO    fopen SAF hit
# Rayverse       JNI 侧 fd 注册、SAF URI（Java 的 RayverseActivity 也用这个 tag）
# GameDataBridge Java 侧目录扫描 + 每个文件的 Opened fd
# Rayverse-GL    GL 上下文/着色器信息（失败行是 ERROR，release 里也能看到）
```

实现位置：

- 原生 `src/ray_log.h`：`RAY_LOG(tag, level, …)` → `__android_log_is_loggable(level, tag, RAY_LOG_DEFAULT)`。该函数 API 30 才有，而 minSdk 是 21，所以用 `dlsym(RTLD_DEFAULT, …)` 取；取不到（API 21-29）时退化成只比较编译期默认值。
- Java `RayLog.java`：release 下用 `Log.isLoggable(tag, Log.DEBUG)` 判断是否打开。**不能问 INFO 档** —— 属性未设置时 `isLoggable` 的内置默认就是 INFO，会一直返回 true，release 就静默不下来。副作用：Java 侧只认 DEBUG/VERBOSE 两个开关值，设成 INFO 不放开 `RayLog.i/v`（`RayLog.e` 始终输出）。
- 默认阈值由 `app/build.gradle.kts` 的各 buildType 注入：debug `-DRAY_LOG_DEFAULT=ANDROID_LOG_VERBOSE`，release `-DRAY_LOG_DEFAULT=ANDROID_LOG_ERROR`。

**没有 adb 的场合（真机在路上）**：游戏内设置菜单 →"查看调试日志"。所有 `RAY_LOG` 行都会无条件写进一个 96K 的**堆上**环形缓冲（`android_jni.c` 的 `ray_log_ring_capture`，不受 logcat 门控影响，release 同样全量记录），Java 侧 `RayLog` 另留 400 行环形缓冲；对话框自动滚到末尾并滤掉启动期的 `Opened/Registered fd` 噪音，**全文同时复制到剪贴板**，直接粘贴就能发给开发者。dump 头部带版本号、debug/release、机型、Android 版本——顺便用来确认真机装的是不是含最新修复的包。两个设计约束：环形缓冲必须是堆分配而不是 `.bss` 静态数组，否则会被 savestate 的数据段整体回滚覆盖（连 mutex 一起，可能卡在已锁状态）；`savestate.c` 的每个状态码（含 2/3/5/6/7 的拒绝与失败路径）都有对应日志行，"request → frame hook → 结果"三段缺哪一段就能定位卡在哪。

定位原则：**先用日志确认事件是否到达、值是多少，再改代码**。第 3 条就是靠一行状态日志把"按键映射问题"直接翻案成类型定义问题，否则会一路在映射表里找不存在的 bug。

## 10. 即时存档在个别真机上 OOM：悬垂的 main_mem_tmp（savestate）

- **症状**：Magic8 Pro（BKQ-AN80，Android 17）点"即时存档"报 status 3；诊断日志 `alloc pool 4 8971876904722464K failed: errno=12 (Out of memory)`，但 `VmRSS` 只有 253MB，不是真缺内存。模拟器（x86_64 + ndk_translation）和 S20（Android 13）上同一份包完全正常。
- **根因**：`main_mem_tmp` 在正常关卡游玩时是**悬垂指针**——引擎在 `bonus.c:167`、`display.c:558` 里 `free(main_mem_tmp)` 后不清 NULL（只有 `ray.c:2790` 清了，那是核心逻辑，不能动）。已释放块的内容完全取决于分配器当下状态：模拟器和 S20 的 scudo 把空闲块读成全零（`capacity==0`，旧代码恰好跳过）；Magic8 Pro 上空闲块被写入了 free-list 头（`capacity≈0x7FE000000000` ≈ 128TB），快照缓冲按这个尺寸 malloc 必然 ENOMEM。旧代码还有个更危险的潜在 bug：把悬垂指针记进 pool 快照，读档时往已释放内存回写 header+数据 = 堆破坏。
- **修复**：`src/savestate.c` 新增 `ss_pool_ptr()` 池指针净化器——`capacity==0 || capacity>64MB || len>capacity` 一律视为"不存在"；`ss_alloc_buffers`、存档记账、读档校验、回写四条路径统一走它，跳过时记 `pool N header implausible … treating as freed`。回写循环改用**存档时记录的（已净化）指针**，保证任何情况下都不写已释放内存。引擎 free 点一行没碰。
- **验证**：S20 真机 1-5 关：`pool 4 header implausible (len=0 capacity=0) — treating as freed` → `saved: world=1 level=5 segment=395K` → `loaded: world=1 level=5`，无崩溃，试玩正常；模拟器同链路通过。Magic8 Pro 的 128TB 垃圾值走同一分支（>64MB 上限），待复测。
- **通用教训**：savestate 这类"信任引擎裸指针"的代码必须做 sanity check——freed-but-not-nulled 的全局是时间炸弹，内容在设备/系统版本间不具移植性；防御要覆盖所有读该指针的路径（分配、记录、校验、回写），漏一条就是崩溃或静默堆破坏。

## 11. 2a 落盘：状态码被 UI 抢先吃掉，槽文件从没写出来（savestate）

- **症状**：模拟器上选槽存档，toast 报"已保存到槽位 1"（状态码 1），日志里 `captured:` 也在，
  但 `files/rayverse_save/` 下没有文件，且**没有任何错误日志**。
- **根因**：`ss_do_save()` 自己把 `status=1` 写进原子量，UI 线程 250 ms 的轮询用
  `atomic_exchange` 读一次即清零；随后帧钩子里那句"如果 status==1 就写槽文件"读到的已经是 0，
  写盘分支被静默跳过。成功状态被消费 + 条件判断复用同一个量 = 没有任何一条路径会报错。
- **修复**：`ss_do_save()` 改为**返回**状态码而不写 `ss.status`；钩子拿到返回值后
  `if (st == 1 && !ss_write_slot(slot)) st = 8;` 再一次性 `atomic_store`。
  写盘失败从此有明确的 8，不再被"已保存"的 toast 掩盖。
- **验证**：轮询序列 `0,0 → 1`，日志 `slot 0 written: world=1 level=1 payload=5861K in 26ms`，
  文件 6,002,720 B 落盘。
- **通用教训**：跨线程的一次性状态（read-once 原子量）不能被生产方自己当作"后续动作的开关"。
  要么另设一个不被消费的标志，要么像这里把决定权收回同一处顺序执行。

## 12. 跨进程读档被拒是**设计行为**，不是回归（ASLR 实测）

同一份 debug 包、同一台模拟器，三个进程的可写段基址：
`0x735629311508` / `0x735628988418` / `0x73562931c418` —— 相邻两次启动差 **10,043,392 B（9.6 MB）**，
而布局指纹 `stamp` 恒为 `5aa80b63`（它只由符号间距离构成，ASLR 搬移整段时不变，这正是它的用途）。

所以 2a 在读槽时逐区域比对 `rec[].base` 与 `ss_region_live()` 的当前基址，任一不同就
`status=10` 并打一行：

```
slot 0: region seg moved since capture (saved=… live=… delta=10043392)
        — cross-process restore needs phase 2b
```

- 这条路径**不崩、不写坏内存**，UI 提示"该存档来自另一次运行，暂不支持跨进程读取"。
- 别把它当 bug 修：真机上"杀掉进程再读旧槽"目前必然走这里。能跨进程恢复是 2b（两遍重定位）的目标，
  探针数据见 `docs/savestate-phase2.md` §9。
- 反过来，会话内（同一次启动）存/读是 delta=0 的分支，2a 已完整验证。
