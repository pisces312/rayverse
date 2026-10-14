# 跨进程读档崩溃/挂起：.bss 静态量回滚（调研与修复）

> 对应 `android-debug-log.md` §15。本文是完整调研记录：症状矩阵 → 证据链 →
> 根因 → 修复设计 → **新问题风险评估** → 验证计划。

## TL;DR

`ss_do_load()` 的整段 memcpy 把 libmain.so 整个 `.data/.bss` 回滚成**存档进程**
的值，其中包括 `android_jni.c` 的 7 个进程专属静态量——日志环指针 `g_log_ring`
被回滚成旧进程的堆地址（悬垂），恢复后的第一次 `RAY_LOG` 就踩进去。三种症状
（SIGSEGV / FORTIFY abort / 挂死）是同一根因的三种落点表现。修复：比照
`global_app_state` 的既有 keep-live 套路，把这 7 个静态量在段恢复前后
save/restore，`g_log_ring` 用原子写、最先回写。

## 1. 症状矩阵

场景统一为：**同版本**存档 → 彻底退出进程 → 重新进入 → 进关卡读档。

| 设备 | 症状 | 关键日志/证据 |
|------|------|--------------|
| Honor (BKQ-AN80), 1.0.3 正式版 | 画面卡死不崩，无 tombstone | 日志停在 `slot 0 read` 之后，`ss_do_load` 内预期的后续一条没有 |
| S20 (SM-G9810), 带步进日志 release 第一轮 | abort | `F libc : FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x723bee84a8)`，**崩溃线程 (tid 23069) 不是加载线程 (tid 23065)** |
| S20, 带步进日志 release 第二轮 | SIGSEGV | 完整 backtrace（见 §2） |

debug 包（-O0）复现不了 → 优化相关 UB 或地址落点运气问题（见 §4）。

## 2. 证据链（S20 第二轮，带指针日志的 release 包）

1. 步进日志推进到 `load step: SetPalette done`——覆盖 backups → keep-live
   scan → segment restored → platform state restored → kept 1647 → pools →
   SetPalette 全部正常。
2. 随后 SIGSEGV，backtrace：

   ```
   #00 ray_log_ring_capture+344
   #01 savestate_frame_hook+11176      ← ss_do_load 内联于此
   ```

   寄存器 `x12/x13` 残留正在格式化的字符串常量 `"segment"` / `"restored"`。

3. 反查：`"segment"`/`"restored"` 正是 `ss_do_load()` 里 segment memcpy 之后
   那条 `SSLOGI("load step: segment restored")` 的实参 → 崩在这条日志上，
   即 `SSLOGI → RAY_LOG → ray_log_ring_capture → g_log_ring`（ray_log.h:40）。
4. 第一轮 FORTIFY 的 mutex 地址 `0x723bee84a8`：环结构体第一个成员是
   `pthread_mutex_t`（android_jni.c:169），悬垂指针指向旧进程环体 → 本进程
   该地址处恰好是被销毁/异常的 mutex 模式 → abort；崩溃线程另有其人 =
   **其他线程**在加载线程还在 fixup 时通过被回滚的指针踩进去。

结论：卡点窗口 = segment memcpy 之后、音频 fixup 完成之前；直接死因 =
`g_log_ring` 悬垂。

## 3. 根因机制

- `ss_do_save()` 通过程序头（`PT_LOAD`，`ph->p_memsz`，savestate.c:243-244）
  记录**整个模块数据段**范围；`ss_do_load()` 用一份大 memcpy 整段回滚。
  libmain.so 里所有编译单元的静态量都在内——包括 `android_jni.c` 的 7 个：
  `g_fd_table[256]`、`g_fd_count`、`g_saf_uri[1024]`、`g_game_data_path[1024]`、
  `g_use_saf`、`g_save_dir[1024]`、`g_log_ring`（指针）。
- `android_jni.c` 环的设计注释（~160-170 行）本意就是防回滚：环体放堆上、
  指针"published on the very first log line … **so restoring the pointer
  always writes back the same value**"。该假设**只在同一进程内成立**
  （快照值 == live 值）。跨进程时快照里存的是旧进程的环体地址，回滚后
  `g_log_ring` 悬垂。
- 同理被回滚的 `g_fd_table`/`g_fd_count` 会变成旧进程的 fd 号集合——
  `LoadBnkWorld → android_fopen` 查表命中错 fd，属于同根因的次生隐患。
- 三种症状的落点解释：

  | 悬垂地址落在本进程的 | 表现 | 实例 |
  |---------------------|------|------|
  | 未映射页 | SIGSEGV | S20 第二轮 |
  | 已映射、含 destroyed-mutex 模式 | FORTIFY abort | S20 第一轮（tid 23069） |
  | 已映射、锁状态异常 | 挂死 | Honor |

- debug 不复现的原因：-O0 + 两次启动环体恰好落在同一地址（同 build 的
  ASLR 布局接近），回滚值碰巧仍有效 → 属于运气，不是没有 bug。

## 4. 修复设计

比照 `global_app_state` / `ss` 两个既有的"段内平台保活"先例（savestate.c
1371-1376：整段 memcpy 后立刻用堆上备份写回），扩展到 android_jni.c：

1. **android_jni.c** 新增（放日志环之后、savestate JNI 段之前）：

   ```c
   typedef struct {
       ray_log_ring_t* log_ring;                     /* 最先回写，原子 */
       fd_entry_t fd_table[MAX_FD_ENTRIES];
       int fd_count;
       char saf_uri[sizeof(g_saf_uri)];
       char game_data_path[sizeof(g_game_data_path)];
       int use_saf;
       char save_dir[sizeof(g_save_dir)];
   } android_plat_state_t;                           /* 仅堆上分配 */

   void* android_plat_state_save(void);              /* NULL = OOM */
   void  android_plat_state_restore(void* blob);     /* 无日志；ring 最先原子回写 */
   ```

2. **savestate.c `ss_do_load()`** 接线：
   - 备份块（1323-1337）里 `plat_backup = android_plat_state_save()`，
     与 app_backup 等共用 OOM 失败路径（status 3）；
   - **`memcpy(seg)` 与其后第一条 `SSLOGI` 之间**调用 restore——晚于这条
     日志就等于没修（本轮崩溃正是死在这条日志上）；
   - 与其他备份一起 free。
3. **savestate.h**：两个函数的声明放在 `android_get_save_dir` 旁。
4. 槽位文件格式**零改动**：blob 不持久化、`ss_ctx_t`（sizeof=544 冻结）不动、
   stamp 不变 → 旧槽照常可读，跨版本加载路径不受影响。

关键实现约束：
- blob 必须 malloc 在堆上——静态备份结构自身会被段 memcpy 回滚；
- restore 内**禁止任何 RAY_LOG**（ring 指针修好之前打日志 = 复现原 bug）；
- `g_log_ring` 读侧是 `__atomic_load_n(ACQUIRE)`（log_ring_get），写侧用
  `__atomic_store_n(RELEASE)` 配对，避免 torn read；
- 备份点必须在槽位文件读完之后（`ss_read_slot` 会走文件 IO）、段 memcpy
  之前——二者之间只有 keep-live 扫描，不触碰这些静态量，安全。

## 5. 新问题风险评估（本节是采纳本修复前的显式权衡）

| # | 风险 | 评估 | 结论 |
|---|------|------|------|
| 1 | 残差竞态：段 memcpy 返回 → ring 原子回写之间，其他线程 RAY_LOG 读到悬垂指针 | 窗口从原来的"整段恢复后到下一条日志（多次 memcpy + 循环，ms 级）"缩到一条 call + 一条 store（ns 级）；彻底消除需停世界。且整段 memcpy 对全部引擎全局量本就非原子（既有设计已接受） | 残差可接受，无回归 |
| 2 | fd 表 133KB 回写期间被其他线程 `android_fopen` 撕裂读 | 该表实际只在启动期由 Java 线程注册、game thread 查询；load 入口已 `stop_all_snd`；同 #1 的既有非原子性质 | 无回归 |
| 3 | restore 内误加日志 → 复现原 bug | 函数内零日志，注释+本文双重标注；调用点日志在 restore 返回之后 | 已规避 |
| 4 | blob OOM（~137KB） | 走既有 backup allocation 失败路径，load 干净终止（status 3），不留半恢复状态 | 已处理 |
| 5 | 会话内（2a）行为回归 | 同进程 save→load 时快照值 == live 值，回写等于无操作；fd 表回写 live 值反而顺带修掉"回滚成 save 时刻旧 fd 表"的存量隐患 | 无回归，有增益 |
| 6 | 槽位格式/兼容性变化 | blob 不入槽、`ss_ctx_t` 不动、stamp 不变 | 无影响 |
| 7 | 内存泄漏 | 备份点与 restore 点之间无 early return；失败路径统一 free | 无泄漏 |
| 8 | 维护风险：android_jni.c 未来新增 file-scope 变量忘记加入 keep-live | 在结构定义处注释注明"新增静态量必须同步加入本结构" | 文档化约束 |

## 6. 验证计划

1. S20（release + 步进日志包）：存 → 彻底退出 → 重进 → 读档，连续 3 轮
   （含读其他关卡存的档）；预期：不再崩/挂，日志完整走到 `loaded: world=…`。
2. 会话内回归：不退出进程，连续 save→load 数次（2a 原场景不回退）。
3. 旧槽跨版本回归：用修复前版本写的槽在修复后版本读（§14 场景，
   预期行为与修复前一致——本次不动槽位格式）。
4. 通过后：移除/降级本轮临时步进日志（"load step:" 系列 + 音频 fixup
   指针日志），仅保留关键节点日志。
5. Honor 复验挂死路径（签名问题解决后）：预期同根因下不再挂。

## 7. 环境备注

- 无 root 设备拿不到 `/data/tombstones/`；native backtrace 靠 logcat
  `-b all` 里的 DEBUG 输出（信息量等同 tombstone），必要时 `adb bugreport`。
- Honor 上 1.0.3 正式版与本机 keystore 签名不一致无法覆盖安装
  （`INSTALL_FAILED_UPDATE_INCOMPATIBLE`），本轮全部在 S20 验证。

## 8. 第二阶段：跨进程画面冻结（g_log_ring 修复后暴露，2b 真机验收）

- **g_log_ring 修复已验证有效**：S20 多轮 load（同进程 + 跨进程）全部完整走到
  `loaded: world=1 level=1`，SIGSEGV / FORTIFY abort / 挂起全部消失。
- **新症状**：**跨进程** load（relocate 实际发生，`delta=c000`，进程 27903）完成后
  画面冻结，但——
  - 游戏线程 27957 状态 **R（running）**、CPU ~22%（正常节流，非忙等死循环）；
  - 输入照常（`key sc=79/80/81/82 down/up` 日志持续打）；
  - GL 每帧正常 swap（无渲染报错、无崩溃）。
  → 主循环在跑、渲染在跑、输入在响应，但**游戏世界不推进**（角色不动）=
  游戏逻辑状态机卡在"不推进"态，不是死锁/ANR/渲染崩。
- **判定**：这是 **Phase 2b 的独立问题**，此前被 g_log_ring 崩溃挡住——跨进程 load
  根本走不到画面阶段就崩了，修好 g_log_ring 之后才暴露。phase2.md §169 标注 2b
  "已实现、待真机验收"，本次正是真机验收暴露。
- **根因方向**：relocate 只翻译落入 region table（seg/pool0-3/flocon/rvb/6 个特殊
  块/libmain）窗口的指针。phase2.md §272-4 的**未完成待办**明确指出：探针只覆盖
  世界 1 关卡 1 一个静态时刻，"函数指针表可能在别的关卡被填进新的全局"。若游戏逻辑
  持有指向 region 之外的 heap 指针，跨进程后悬垂 → 读错数据 → 状态机卡住但不崩。
- **佐证**：relocate hits 明细（`fixed=5903 ambiguous=0 targets=14`，libmain 1647 /
  pool 2327+708+137+955 / seg 121）与 §9 基线吻合，region 覆盖范围内无异常——
  进一步指向 region 之外被遗漏的指针。
- **下一步（待定方案）**：
  1. 抓游戏线程 `main_Ray` 卡点。release 非 debuggable（`debuggerd` 报 root required、
     `/proc/<pid>/task/<tid>/stack` 无权限）→ **用 debug 包复现跨进程读档卡住后
     `debuggerd -b <pid>` 抓 native 堆栈**，直接看卡在哪个状态机分支/解引用哪个指针。
  2. 或在复现的关卡/状态下**扩展探针**跑一遍，看 `heap_other`/`unmapped` 样本的
     `off`+`where` 定位具体是哪个全局指针悬垂，再决定把它纳入 region table 还是别的处理。

### 同进程 vs 跨进程对照（同进程是否可靠）

- **同进程 load**：快照地址 == 本进程地址 → relocate 走 `delta=0` 分支
  （`relocate: layout identical to capture, nothing to fix`，跳过重定位）；整段 memcpy
  回滚天然精确。keep-live 回写等于无操作（`g_log_ring` 原子 store 同值）。2a 已在模拟器
  验收（phase2.md §9：位置/Lums/Tings 精确回滚、读后可继续玩），本次 S20 多轮同进程
  load 全部成功（如 22:44:45 那次）——**未观察到任何异常，是最成熟的路径**。
- **跨进程 load**：地址变 → relocate 翻译 region 内指针；region 外指针悬垂 → 本轮画面冻结。
- 本轮 S20 现象与之完全一致：多轮中同进程 load 全成功，**仅最后跨进程（重开游戏=新进程）
  卡住**。

### debug 包部署（抓堆栈用）

- debug 包 applicationId = `com.rayverse.rayman.debug`，与 release 共存、存档目录各自独立。
  已构建（含 keep-live 修复 + 步进日志）并装到 S20，采集 `logcat-s20-debug-fix.txt`，
  待复现后 `debuggerd -b <pid>` 抓 `main_Ray` 卡点堆栈。
- **复现失败（2026-10-14 23:01）**：debug 版按同流程无法重现跨进程画面冻结。与之前
  g_log_ring 崩溃在 debug 包"碰巧不炸"同因——`-O0` + 不同 heap 布局下，悬垂指针恰好落在
  可读地址，读错数据但不卡死。进一步佐证"画面冻结依赖 release 优化布局/地址落点"。
  抓堆栈路径受阻，改用 release 包加更细的定位日志（在 `main_Ray` 状态机关键分支打点，
  看跨进程 load 后卡在哪个分支、哪个全局量不推进）。

## 9. 提交记录

- 本轮修复（keep-live）+ 步进日志已提交推送；跨进程画面冻结仍未定位（见 §8），
  待 release 定位日志跟进。


