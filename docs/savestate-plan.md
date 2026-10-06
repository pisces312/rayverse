# Savestate（即时存档）实现计划

目标：像模拟器一样，在游戏进行中任意时刻保存完整游戏状态并随时恢复，不依赖游戏自身的
存档点/存档文件机制。

## 现状分析（调研结论）

引擎结构对 savestate 非常友好，关键事实：

1. **单线程 + tick 驱动**。游戏逻辑全部在主线程，按固定 tick 推进
   （`horloges()`/`map_time`/`dead_time`），真实时钟只用于外层帧率控制
   （`linux_main.c`），不渗入游戏状态。音频为推送模式（`spec.callback = NULL`，
   主线程每帧 `linux_produce_sound_for_frame` 推给 SDL 队列），无音频回调线程竞争。

2. **动态内存全部走 arena 内存池**，且 `block_free()` 只重置游标、不真正释放
   （`src/sysutils.c`）。5 个主池在 `LOAD_ALL_FIX`/`FIRST_INIT` 一次性 `calloc`，
   整个会话期间**基地址不变**：

   | 池 | 大小（已 ×2） | 内容 |
   |----|------|------|
   | `main_mem_tmp` | 0x44000 (~272K) | 临时画面（注意：`FIN_PC` 会 free，见风险） |
   | `main_mem_world` | 0x1E9800 (~1.9M) | 世界地图/精灵 |
   | `main_mem_level` | 0x10F800 (~1.06M) | 关卡数据、`level.objects`、`link_init` |
   | `main_mem_sprite` | 0x1BE800 (~1.75M) | 精灵图形 |
   | `main_mem_fix` | 0x9B000 (~622K) | 固定资源、语言文本 |

   `block_malloc` 是顺序 bump 分配，同一关卡重新加载时布局**完全确定**，
   因此"覆写池内容"后所有池内指针天然有效，无需指针修复。

3. **游戏状态几乎全在 unity build 的全局变量（.data/.bss）+ 上述池里**。
   libmain.so 只有一个可写 PT_LOAD 段，`dl_iterate_phdr` 可整体捕获。

4. **少量堆状态由全局指针持有**，需要单独快照内容或做指针一致性处理：
   - `flocon_tab`（512 × flocon_t，雪花粒子，可变，一次分配不释放）
   - `rvb_special`（16 × rgb_palette_t，调色板，一次分配不释放）
   - 音频侧：`bnkDataFixe` / `bnkDataWorld` / `ptrTchatch`（音效样本，
     `LoadBnkWorld` 时 realloc/free，**指针会变**）、
     `ogg_cd_track`（音乐流，`stop_ogg` 时 free，**指针会变**）

5. **平台状态混在同一段里，恢复时必须排除/回填**：
   - `global_app_state`（SDL 窗口、GL、音频设备、帧时钟、surface）
   - savestate 模块自身的静态上下文
   - `android_jni.c` 的 fd 表等静态量（游戏进行中不变，v1 不排除，见风险）

6. **帧边界钩子点**：`advance_frame()`（`src/engine.c`），由 `endsynchro()` 和
   `WaitNSynchro()` 每帧调用，是唯一收口。

## 方案设计

### 快照内容（单槽，Phase 1 仅驻内存）

```
savestate 快照 = {
  libmain.so 可写段整体副本（排除区回填处理）
  + 5 个 arena 池的 header(len/capacity/cursor) + data[0..capacity] 副本
  + flocon_tab、rvb_special 堆内容副本
  + 音频堆指针记录（bnkDataFixe/bnkDataWorld/ptrTchatch/ogg_cd_track）
  + 元数据（num_world、num_level、快照时的游戏上下文标志）
}
```

预计体积 ~8MB（段 + 池），Phase 1 直接 malloc 驻留。

### 保存流程（游戏线程，帧边界执行）

1. 上下文门禁：`RaymanDansUneMapDuJeu && !During_The_Menu && !GoMenu && !gele`
   ——只在正常关卡游玩帧允许（此时调用栈形态稳定，`main_mem_tmp` 已释放）。
2. `dl_iterate_phdr` 定位 libmain.so 可写段，整段 memcpy。
3. 逐池快照（NULL 池记录为 NULL）。
4. 快照 `flocon_tab`、`rvb_special` 内容，记录音频指针。
5. 置状态码供 Java 层查询。

### 恢复流程（游戏线程，`advance_frame` 开头执行）

1. 同样的上下文门禁 + 校验：有快照、各池非空且 capacity 与快照一致，否则拒绝
   （返回对应状态码）。
2. 备份平台区：`global_app_state` 与 savestate 自身上下文先拷到堆上临时副本。
3. 记录**当前**音频堆指针，与快照中记录的对比：
   - 相同 → 音效库/音乐流自快照以来未重载，恢复后指针依然有效；
   - 不同 → 已被 free，快照指针是悬垂的。恢复段后**回填当前活指针**，
     再调 `LoadBnkWorld(num_world_choice)`（恢复后的值）重载正确音效库；
     音乐流若不一致则保留当前流（不重启），`decoder==NULL` 时同步
     `is_ogg_playing=false`。
4. `stop_all_snd()` + `SDL_ClearQueuedAudio()` 清掉残留音频。
5. 整段 memcpy 回去，再把步骤 2 的平台区副本写回（覆盖被恢复的 stale 值）。
6. 覆写各池 header + data，覆写 `flocon_tab`、`rvb_special` 内容。
7. 清输入状态（`Touche_Enfoncee` 等清零，防止恢复出"按键卡住"）。
8. 下一帧引擎按恢复后的全局变量自然重绘（软渲染每帧上传 GL，无需 GPU 状态恢复；
   调色板随段恢复，`DO_SWAP_PALETTE` 每帧执行自愈）。

跨关卡恢复（存 A 关、死回 B 点后读档）天然支持：池基地址会话内不变，
覆写内容即完成"换关"，外层循环条件（`new_level`/`new_world`/`dead_time`）
都是被恢复的全局变量，逻辑自洽。

### 线程与入口

- Java 设置菜单（`RayverseActivity.showSettingsMenu`，右上角齿轮按钮）新增
  **"即时存档" / "即时读档"** 两项。
- 点击 → JNI（`android_jni.c` 新增 `nativeRequestSaveState/nativeRequestLoadState/
  nativeGetSaveStateStatus`）→ 只置 `atomic_int` 请求标志。
- 游戏线程在 `advance_frame()` 开头的钩子（`savestate_frame_hook()`，
  `#ifdef ANDROID`）消费标志并执行，天然规避跨线程竞争。
- Java 侧 `postDelayed(~300ms)` 查询状态码 → toast 反馈。

### 代码落点

| 文件 | 改动 |
|------|------|
| `src/savestate.c` | 新建，快照/恢复核心（内部整体 `#ifdef ANDROID`） |
| `src/savestate.h` | 新建，跨 TU API（供 android_jni.c extern） |
| `src/rayverse.c` | unity build 追加 `#include "savestate.c"` |
| `src/engine.c` | `advance_frame()` 顶部加 `#ifdef ANDROID` 钩子（一行） |
| `android/app/src/main/cpp/android_jni.c` | 新增 3 个 JNI 入口 |
| `android/.../RayverseActivity.java` | 设置菜单加两项 + native 声明 + toast |

不改任何游戏核心逻辑文件；引擎侧仅 `engine.c` 一行 guarded hook。

## 分阶段

- **Phase 1（本次）**：会话内单槽内存快照，关卡游玩中存/读，跨关卡恢复，
  音频指针一致性处理。真机验证。
- **Phase 2**：持久化到文件（`g_save_dir`，含版本号/校验和），多槽位，
  应用重启后引导引擎进对应关卡再恢复（需处理"启动路径走到 gameplay"的编排）。
- **Phase 3（可选）**：任意上下文读档（菜单/世界地图中），用 longjmp 回卷到
  PcMain 安全点再恢复；导出/导入 savestate 文件（复用现有 save export 通道）。

## 风险与对策

| 风险 | 说明 | 对策 |
|------|------|------|
| 平台 stale 值回填不全 | 段内含 JNI/SDL 相关静态量 | 排除 `global_app_state` + savestate 上下文；fd 表游戏中不变（重启进程才会重注册），v1 接受，Phase 2 重启恢复时天然重置 |
| 音频堆指针悬垂 | 存档后换世界/换曲导致 free | 步骤 3 的对比-回填-重载逻辑 |
| `main_mem_tmp` 悬垂指针 | 引擎 `bonus.c`/`display.c` 里 `free(main_mem_tmp)` 后**不清 NULL**，正常游玩帧该全局是悬垂指针；已释放块的内容（清零/分配器 poison/复用数据）随设备与分配器版本而异，Magic8 Pro 上读出 capacity≈128TB 导致快照 malloc ENOMEM（存档 status 3），往悬垂结构回写更是堆破坏 | `savestate.c` 的 `ss_pool_ptr()` 净化：`capacity==0 || capacity>64MB || len>capacity` 一律视为不存在，alloc/save/load/回写四条路径统一使用；回写只认存档时记录的净化后指针。见 `android-debug-log.md` 第 10 条 |
| 栈上局部变量不参与恢复 | `PcMain` 的 `v1` 等少量栈局部 | 均为过渡性标志，下一关卡循环会重初始化；真机观察死亡→读档→过关流程 |
| 读档后一帧时间跳变 | `frame_clock` 属平台区已排除 | 无 |
| 段快照包含 GOT | GOT/`.data.rel.ro` 是独立的 RELRO PT_LOAD（18K），不参与快照；实测命中的是最大可写段（`.data+.bss`，约 395K） | 见 `android-debug-log.md` 第 7 条：必须按最大 memsz 选段，否则漏掉全部引擎全局 |

## 验证清单（真机）

1. 关卡中存档 → 继续走 → 读档：位置/生命/Tings/敌人状态回滚，无卡键。
2. 存档 → 故意死亡 → 重生后读档：回到存档点（跨"关卡重载"验证池覆写正确性）。
3. 存档 → 过关进下一关 → 读档：回到原关卡，音效正常（验证 LoadBnkWorld 重载路径）。
4. 存档 → 世界地图 → 进另一关 → 读档（音乐/音效指针变化路径）。
5. 读档后画面、调色板、雪花/雨粒子正常。
6. 反复存读 ≥10 次无内存增长异常（快照缓冲复用，不重复 malloc）。
7. 非游玩上下文（菜单中）点读档 → toast 提示拒绝，游戏不崩。

## 验证记录

- 2026-10-06 pixel6 模拟器（x86_64 + ndk_translation，debug 包）：清单第 1、2 条通过。
  存档日志 `saved: world=1 level=1 segment=395K`；存档→死亡→重生→读档（`loaded: world=1 level=1`）位置与 Lum 状态精确还原，读档后移动/物理/关卡循环正常，无 SIGSEGV。
  首轮实现误抓 18K RELRO 段（见 `android-debug-log.md` 第 7 条），已修复。
- 清单 3-7 待真机验证。
