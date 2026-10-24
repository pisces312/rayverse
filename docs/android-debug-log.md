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
| `adb shell run-as … cat` 拉槽文件 | 文件比设备上大 ~10K，二进制被 0x0A→CRLF 撑坏，格式解析全错 | 与截图同一个坑：用 `adb exec-out "run-as … cat …" > f.bin` |
| 崩溃只有 logcat 头、回溯 1 帧 | 定位不了谁把返回地址砸了 | `adb shell dumpsys dropbox --print SYSTEM_TOMBSTONE` 取全文（含 **memory near sp**）：栈上 {fp, lr} 帧记录链 + `llvm-nm` 偏移反查即可手工还原调用链。pc == lr 是 `ret` 进坏地址的签名 |
| 怀疑"旧版本二进制布局不同"但手头没有旧产物 | 无法对比 | 槽文件本身就是证据：函数指针值 − saved 模块基址（`rec[0].base - 0x111358`）= 旧 build 的代码偏移，直接和当前 `llvm-nm` 对比 |

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

- 原生 `src/ray_log.h`：`RAY_LOG(tag, level, …)` 先无条件写入应用内日志环，再由 `ray_log_enabled()` 决定是否写 logcat。`__android_log_is_loggable()` 是 API 30 符号，而 minSdk 为 21；当前通过 `__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__` 的 weak undefined 引用 + `__builtin_available(android 30, *)` 守卫调用，API 21–29 退化为 `level >= RAY_LOG_DEFAULT`。**不再使用 `dlsym` 或缓存函数指针**；宏由 `Android.mk` 全局注入，`ray_log.h` 也在包含 `<android/log.h>` 前提供兜底定义。
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

## 13. 跨进程读档真机崩溃：地址门槛按模拟器形态写死（savestate）

- **症状**：Magic8 Pro 上关卡中读即时存档直接崩（tombstone：`SEGV_ACCERR` 写
  `0x6d8c5240e8`，栈 `SetPalette ← ss_do_load`）；同一份包在 pixel6 模拟器上跨进程
  读档正常。槽文件本身没问题：校验通过、列表可见。
- **排查**：日志缓冲已被冲掉，改拉槽文件本体分析（`adb exec-out "run-as
  com.rayverse.rayman.debug cat files/rayverse_save/rayverse_ss_0.bin"`，注意
  `shell cat` 走 CRLF 会把二进制撑大 10K，必须 exec-out）。槽里 `rec[0].base =
  0x6d8c518a80`，而崩溃写地址 = 槽0 seg base + 0xB668，即旧进程
  `&global_app_state.game.draw_palette` —— 指针被原样写回、重定位根本没跑。
- **根因**：`ss_relocate_buffer()` 的 `SS_NOISE_FLOOR = 2^40`（"低于 2^40 是打包
  数据"）和 `v >> 48`（"高 16 位非 0 是两个打包 u32"）两个门槛都是照模拟器的地址
  形态（模块在 0x7xxx…、堆无标签）写的。真机上模块在 0x6d8c…（2^38.6），堆指针带
  TBI 标签 0xb4…，两个门槛把**全部**真指针都滤掉了。修法见
  `docs/savestate-phase2.md` §11.4：匹配改在低 56 位上做，标签按活区域基址回填。
- **教训**：地址形态假设（"真指针长什么样"）是设备相关的，不能写死。判"是不是指针"
  只能靠"落不落在已知区域窗口里"，窗口匹配前先剥标签。探针里同类注释/门槛同病，
  一并改了。

## 14. 跨版本读档闪退：代码布局偏移，ASLR 重定位救不了（savestate）

- **症状**：§13 的重定位修复后，同版本新槽存/读完全正常，旧版本写的槽读取仍闪退。
  tombstone 形态变了：`trying to execute non-executable memory`，pc == lr ==
  `global_app_state` 的地址（数据段内），栈帧链是
  `DO_MAIN_LOOP → DO_GROS_MOTEUR_NORMAL → DO_MOTEUR → DO_OBJECTS →
  DO_PNG_COLL_STONEMAN → skipToLabel`——对象脚本解释器把返回地址砸了。
- **根因**：快照数据段里 1654 个函数/字符串指针（`ObjectsFonctions`、`cptr_tab`、
  绘制分发、`TEST_KEY_BUTTON*` 等）带的是**写档 build 的代码偏移**；unity build 里
  改 `savestate.c` 会让其后所有函数平移（实测 +0x438）。模块 delta 只修 ASLR，
  平移后旧函数地址落到别的函数中段 → 乱执行。"游戏核心代码没改就不该不兼容"对
  **数据布局**成立（stamp 冻结），对**代码指针**不成立。
- **修复**：`ss_do_load()` 恢复数据段时对 `[mod_lo, seg_base)` 值域的槽位 keep-live
  （保留本进程活值）；池里零模块指针，覆盖完备。完整调研过程（tombstone 栈帧链
  还原、旧档偏移对比、危险面盘点）见 `docs/savestate-phase2.md` §11.5。
- **验收**：真机读旧版本槽（world 1 level 9）成功恢复、无闪退（2026-10-14）。

## 15. 同版本跨进程读档：真机挂起/abort（平台静态量根因已修复，savestate）

- **症状**（1.0.3 真机，同版本存档→退出→重进→读档）：
  - Honor（BKQ-AN80）：读档后画面卡死不崩，无 tombstone。日志停在
    `slot 0 read` 之后，`ss_do_load` 里预期的 `load: kept …` 一条都没打出来
    （当时装的 1.0.3 无步进日志，只能定位到"进入 ss_do_load 后挂"）。
  - S20（SM-G9810）：直接 abort——`F libc: FORTIFY: pthread_mutex_lock called
    on a destroyed mutex (0x723bee84a8)`，线程不是加载线程（加载线程 tid 23065，
    abort 线程 tid 23069）。无 root 拿不到 tombstone，crash buffer 里也只有这一行。
  - **debug 包（-O0）复现不了**，同代码 release 才出事 → 指向优化相关 UB 或时序竞争。
- **排查手段**：在 `ss_do_load()` 全程插步进日志（`load step:` 系列）重打 release。
  S20 上的推进结果：`stop_all_snd #1 → ClearQueuedAudio → backups → keep-live
  scan → segment restored → platform state restored → kept 1647 → pools →
  SetPalette done` 全部打印，**卡点窗口收窄到 SetPalette 之后、`stop_all_snd #2`
  之前**——即音频 fixup 段：bank 指针回填 / `LoadBnkWorld(num_world_choice)` /
  ogg fixup / `stop_all_snd()`。注意崩溃线程另有其人，说明是**恢复期间其他线程
  通过被恢复的（悬垂）指针踩进去**，而不是加载线程本身顺序走崩。
- **当前假设**（按嫌疑排序）：
  1. 整段 memcpy 恢复把 `android_jni.c` 的 .bss 静态量（`g_fd_table`、
     `g_log_ring` 等）也回滚成**旧进程**的值——会话内无害（同值），跨进程全是
     悬垂/错乱值。`g_log_ring` 若回滚，任何线程下一次 RAY_LOG 都会
     `pthread_mutex_lock` 到旧进程 ring 的锁上（ring 结构第一个成员就是
     pthread_mutex_t）→ 与 S20 的 FORTIFY 报文吻合。加载线程自己的日志能打出来
     目前解释不通（待指针值日志验证）。
  2. 恢复后的 `ogg_cd_track`/声音 voices 指向旧进程堆（heap_other 未重定位），
     其他线程（音频相关）在 fixup 完成前使用。
  3. `LoadBnkWorld` 走 `android_fopen` → 被回滚的 `g_fd_table` 命中旧 fd 号。
- **下一步**：
  1. 已在音频 fixup 段加指针值日志（live vs snap 的 bnk/tchatch/dec/file）+
     `LoadBnkWorld` 前后日志，重打 release 复现 → 看最后一条日志定加载线程位置，
     并核对哪个指针是旧进程地址。
  2. 若坐实 .bss 静态量回滚：修法方向是把 android_jni/android_fileio 的静态量
     （至少 `g_fd_table`、`g_fd_count`、`g_log_ring`、`g_save_dir`、`g_saf_uri`、
     `g_use_saf`）并入 keep-live/排除面——类比 `ss` 的单区间排除（§注释 184），
     或扩展 keep-live 扫描把这些符号的地址区间整个保活。`ss_ctx_t` 冻结约束
     （sizeof=544）不涉及，无需动布局。
  3. 复验 Honor 挂起与 S20 abort 是否同根因（预期：是，不同 libc FORTIFY 行为
     下的两种表现）。
  4. 修复后按惯例真机验收：同版本跨进程存→退→读，三个世界各一档；再加跨版本
     旧槽回归（§14 场景不回退）。
- **环境坑**：Honor 上 1.0.3 release 与本机 keystore 签名不一致无法覆盖装
  （`INSTALL_FAILED_UPDATE_INCOMPATIBLE`），换 S20 测试；S20 需先卸载旧 1.0
  （同为异签名）。无 root 设备拿 tombstone 只有 logcat crash buffer 一条 F 行，
  native backtrace 缺失时靠步进日志收窄窗口。

- **根因坐实**（2026-10-14 22:27 第二轮 release 复现，带指针日志包）：SIGSEGV 的
  backtrace 是 `ray_log_ring_capture+344 ← savestate_frame_hook`，寄存器里还留着
  正在格式化的字符串 "segment"/"restored"——正是 segment memcpy 之后的第一条
  SSLOGI。**整段 memcpy 把 `android_jni.c` 的 .bss 静态量 `g_log_ring` 回滚成旧进程
  的 ring 指针（悬垂）**，恢复后第一次 RAY_LOG 就踩进去：落在未映射页 → SIGSEGV；
  落在已映射但含 destroyed-mutex 模式 → FORTIFY abort（第一轮 tid 23069 的崩法）；
  落在映射但锁状态异常 → 挂死（Honor）。同一根因三种表现，随悬垂地址落点而定，
  这也解释了 debug 复现不稳定。`g_fd_table`/`g_fd_count`/`g_saf_uri`/
  `g_game_data_path`/`g_use_saf`/`g_save_dir` 同样会被回滚（fd 表回滚为旧进程
  fd 号，`LoadBnkWorld`→`android_fopen` 会命中错 fd），必须一并保活。

- **完整调研与修复设计**（含风险评估、验证计划）见
  `docs/savestate-cross-process-crash.md`。

## 16. 读档后画面 blit 闪退：app_backup 覆盖不到的"编外" swap-buffer 全局

- **症状**：v1.0.4 release 真机，同版本存档 → 杀进程 → 重进 → 读档 → 立即闪退。
  **三次复现 pc（`surface_blit_palettized_image+212`）与 fault addr
  （`0xb40000752d0dc0e0`）完全相同**，确定性 bug。
- **关键判据**：该 fault addr 在**两个不同进程**里一模一样；Scudo 的 64000 字节
  大块走 secondary mmap、基址随机，不可能撞同一地址 ⇒ 该值**来自存档文件**。
- **根因**：`ss_do_load()` 的整段 `.bss` 回滚把 `DrawBufferNormal` / `draw_buffer` /
  `DrawBufferNormalIni` 三个**独立全局**（`data.c:2614/2618/2624`，都不在
  `global_app_state` 内）还原成写档进程的堆地址；`app_backup` 只盖
  `sizeof(app_state_t)`，够不到它们；重定位也没兜住，因为它们指向的是
  `engine.c:16` 的裸 `calloc(64000)`，没有登记区域窗口。下一帧
  `SWAP_BUFFERS()`（`frame.c:41`）把悬垂值传给 `draw_buffer`，
  `advance_frame()`（`engine.c:70-77`）的 `draw_buffer != game.draw_buffer.memory`
  必然成立 → blit 前换指针 → `render.c:61` 解引用即崩。
- **第三个洞**：`DrawBufferNormalIni` 不能漏 —— `update_display_map()`
  （`map.c:77`，由 `DO_GROS_MOTEUR_NORMAL(1)` **每帧**调用）会从它把悬垂值重新
  灌回 `DrawBufferNormal` / `draw_buffer`。只修两个只能撑过一帧。
- **修复**：`ss_do_load()` 在 `global_app_state` 恢复之后按活值重派生这三个全局
  （源头就是 `global_app_state.game.draw_buffer.memory`，`display.c:610` 的
  `FIRST_INIT` 本来就是这么赋的）。不动 `ss_ctx_t`，stamp 不变，旧槽仍可读。
- **同类审计**：`DisplayBufferNormal`（NULL 后无赋值）/ `display_buffer`（唯一赋值
  来自前者）/ `temp_mem_buf`（`FIRST_INIT` 重建）无需处理；
  `EffetBufferNormal` 与 `main_mem_*` 已被区域表 + pool 机制覆盖。
- **完整调研**（抓证链路、符号化方法、完备性审计表、风险评估、验证计划、环境坑）
  见 `docs/2026-10-15_1522_savestate-swapbuffer-dangling-globals.md`。

## 17. 修复后读档仍崩：Bande[] 数组里的派生指针（同类，未决）

- **症状**：§16 修复合入后的构建（BuildId 已与本地 release 包逐位核对一致），
  同日 15:40 用**同一旧槽**读档再次闪退（tombstone_21）。fault addr 与前三次
  完全相同（`0xb40000752d0dc0e0`），但崩溃点挪到
  `Display_Back_Screen → Copy_Plan0Diff_To_Buf` 的 memcpy **写端**。
- **根因推导**：`Bande[300]`（`.bss`，`data.c:3328`）的 `draw_buffer_pos` 是
  `Init_Bande`（`menu.c:8 → screen.c:102/105`）从 `DrawBufferNormal` 派生的
  指针，整段回滚后悬垂；指向的裸 `calloc(64000)` 无重定位窗口，§16 的
  "重派三个全局"够不到数组字段。读源（`PLAN0BIT` 派生）被重定位兜住所以
  没崩，只有写端崩。§16 §3 的审计漏了结构体数组内的指针字段。
- **定论**（槽文件解剖，见对应文档 §8）：旧槽是**跨构建槽**——写于 12:37
  （早于 v1.0.4 构建），其 draw buffer 地址 `0xb40000752d0dc0e0` 与当前任何
  构建的地址都不同，读档必悬垂；同构建新槽（2-1 同关、同流程）多次重启读档
  正常，因为**同构建进程的 draw buffer 地址确定性复用**，裸 calloc 指针靠
  地址巧合活着。旧槽内容自洽、布局一致，不是坏档。
- **修复方案**：登记 `SS_RGN_DRAWBUF` 重定位区域（把地址巧合变成重定位
  保证，跨构建槽也能读；代价是 stamp 变、现有槽作废，旧槽是回归样本）。
  在修复前**跨版本/跨构建读档不承诺支持**。样本库 `savestates/INDEX.md`，
  完整调研见 `docs/2026-10-15_1611_tombstone21-bande-drawbufferpos-dangling.md`。
