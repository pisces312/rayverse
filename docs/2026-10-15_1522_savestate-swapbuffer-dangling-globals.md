# 读档后画面 blit 崩溃：app_backup 覆盖不到的"编外" swap-buffer 全局

> 对应 `android-debug-log.md` §16。本文是完整调研记录：抓证 → 符号化 →
> 根因 → 完备性审计 → 修复 → 验证。
>
> 排查于 2026-10-15，v1.0.4 release 真机上 **三次复现 pc / fault addr 完全一致**，
> 属于确定性 bug，不是时序竞争。

## TL;DR

`ss_do_load()` 的整段 `.data/.bss` memcpy 把 `DrawBufferNormal` / `draw_buffer` /
`DrawBufferNormalIni` 三个**独立全局**回滚成写档进程的堆地址。这三个名字带
"Normal" 的 swap-buffer 指针**不在 `global_app_state` 里**，所以既有的
`app_backup`（只盖回 `sizeof(app_state_t)`）够不到它们；重定位也救不了，因为它们
指向的 `calloc(64000)` 是裸堆块、没有登记区域窗口。

失效只差一帧：下一帧 `SWAP_BUFFERS()` 执行 `draw_buffer = DrawBufferNormal;`
把悬垂值传播开，`advance_frame()` 里 `draw_buffer != game->draw_buffer.memory`
条件必然成立，于是 blit 前把好的指针换成悬垂值 → `render.c:61` 解引用即
SIGSEGV。

修复：在 `ss_do_load()` 的 `global_app_state` 恢复之后，把这三个全局按
live 活值重新派生（`DrawBufferNormalIni = DrawBufferNormal = draw_buffer =
global_app_state.game.draw_buffer.memory`）。

## 1. 症状与抓证

### 复现步骤

冷启动 → 进关卡 → 存档 → **彻底退出进程** → 重进 → 读档 → 闪退。

### 三次复现的签名

| 来源 | pc | fault addr | 备注 |
|---|---|---|---|
| `crashlogs/20261015-1428/tombstone_18.txt` | `surface_blit_palettized_image+212` | `0xb40000752d0dc0e0` | pid 12552 |
| `crashlogs/20261015-1428/tombstone_19.txt` | 同上 | 同上 | pid 13132 |
| `crashlogs/20261015-1506/tombstone_20.txt` | 同上 | 同上 | pid 8842，带 VERBOSE 日志 |

**三次 fault addr 完全相同，且跨两个不同进程、模块基址都不同** —— 这是最关键
的判据。Scudo 的 64000 字节大块走 secondary mmap、基址随机，两个进程不可能碰巧
撞上同一地址，所以这个值**必然来自存档文件**（被 memcpy 写回来的快照内容），
而不是本进程的分配。

### backtrace（tombstone_20）

```
#00 pc 0x7458c  libmain.so (surface_blit_palettized_image+212)
#01 pc 0x71148  libmain.so (advance_frame+300)
#02 pc 0x7a604  libmain.so (DO_MAIN_LOOP_PC_NORMAL+2340)
#03 pc 0x849cc  libmain.so (PcMain+4720)
#04 pc 0x70b98  libmain.so (SDL_main+932)
#05 ... libSDL2.so (SDLActivity_nativeRunMain+680)
```

对应源码：

- `src/render.c:61` —— `palette->colors[*source_pixel]`
- `src/engine.c:75` —— `surface_blit_palettized_image(&game->draw_buffer, ...)`

反汇编确认崩溃指令就是加载 `source->memory`：

```
0x74530: ldr x5, [x0]        ; x5 == fault addr
```

寄存器 `x0 = 0x6c7633a0e0`，落在 `anon:.bss`；用回号回溯的 pc 偏移反算模块基址
（`base = pc_abs - pc_off`，memory map 那列 offset 在这台机上会偏 0x8000，不可信），
得 `x0 == &global_app_state.game.draw_buffer`。

### 带日志复现证明了什么

`setprop log.tag.Rayverse-SS VERBOSE` + `am force-stop` + 复现，日志（`logcat_rayverse.txt`）：

```
15:07:38.479  relocate: fixed=11661 ambiguous=0 targets=14 in 40ms
15:07:38.479  relocate: seg win=... hits=123
15:07:38.479  relocate: EffetBufferNormal win=... hits=1
15:07:38.479  relocate: libmain win=... hits=1648
15:07:38.479  load: kept 1648 code/rodata references live
15:07:38.480  load step: audio fixup start live: bnkF=0xb400006f2fd2f040 bnkW=0xb400006d3fd0b050 ... | snap: bnkF=0xb4000075dd0c8030 bnkW=0xb40000749d0c7050 ...
15:07:38.483  loaded: world=2 level=1
15:07:38.483  F libc: Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xb40000752d0dc0e0
```

两条结论：

1. **读档完整成功**（`loaded: world=2 level=1`，58 个 `load step` 全部走完，
   无任何 `rejected`）→ 排除"读档被拒后残留脏值"这条路径。
2. `loaded` 与 `Fatal signal 11` **同在 15:07:38.483** —— 加载与崩溃在同一帧内。
   读档由 `advance_frame()` 开头的 frame hook 触发，返回后同一帧立刻走 blit。

## 2. 根因

### 2.1 三个全局都不在 `global_app_state` 里

`app_state_t` 里的游戏态是内嵌结构体：

```c
typedef struct app_state_t {
    ...
    game_state_t game;      /* draw_buffer / prev_frame / draw_palette / sound_buffer */
    ...
} app_state_t;
```

而 `DrawBufferNormal` / `draw_buffer` / `DrawBufferNormalIni` 是 **`data.c` 里
并列声明的独立 `u8*` 全局**（`data.c:2614/2618/2624`），只是**碰巧**指向
`game.draw_buffer.memory` 那一块内存。`ss_do_load()` 的保护是：

```c
memcpy(app_backup, &global_app_state, sizeof(app_state_t));   /* 1329 行备份 */
memcpy((void*)ss_backup->seg_base, ss_backup->seg_copy, ...); /* 1387 整段回滚 */
memcpy(&global_app_state, app_backup, sizeof(app_state_t));   /* 1390 只盖回这个 */
```

`app_backup` 只有 `sizeof(app_state_t)` 字节，**盖不到段内其他位置的这三个全局**。
保护留了一个洞。

### 2.2 重定位为什么没兜住

`ss_build_reloc_targets()` 的窗口是"区域表 + libmain 模块 span"：

```
seg / pool0..4 / flocon / rvb / EffetBufferNormal / DistPointX / DistPointY /
ExpPoint / JumelleZoomDef / Def_Bande / libmain  = 14 个 target（日志 targets=14 吻合）
```

这三个 swap-buffer 全局**指向的块是 `engine.c:16` 的裸 `calloc(1, 64000)`**，
没有登记为任何 region。而重定位对**值**做的是"窗口命中才平移"，一个没有窗口的
裸堆地址不匹配任何 target → 原样保留 → 悬垂。

**关键区分**：重定位修的是"快照里指向已登记区域的**值**"，而这三个**全局变量本身
所在的槽位**虽然落在 `seg` 窗口内、会被平移，但平移的只是"上一次进程分配出来的
那块堆地址"——`calloc` 的地址属于 libc 的 mmap 区，不在任何 region 窗口里，
所以**槽位里的值被当成 packed game data 跳过了**，仍是旧进程地址。
（`seg` 窗口 `hits=123` 修的是段内指向段内/池/flocon/rvb 的那些槽位。）

### 2.3 失效链条

```
① ss_do_load() 1387：整段 .bss 回滚
   → DrawBufferNormal = 写档进程的 calloc 地址（悬垂）
   → draw_buffer      = 同上
   → DrawBufferNormalIni = 同上

② frame.c:41  SWAP_BUFFERS()（每帧执行）
   draw_buffer = DrawBufferNormal;      ← 悬垂值持续传播，补丁必须覆盖源头

③ engine.c:70-77（blit 前的关键三行）
   u8* saved_memory = game->draw_buffer.memory;
   if (draw_buffer && draw_buffer != game->draw_buffer.memory)
       game->draw_buffer.memory = draw_buffer;    ← 好指针被换成悬垂值
   surface_blit_palettized_image(&game->draw_buffer, ...);   ← render.c:61 崩

   条件必然成立：一个是本进程 calloc 地址，一个是快照里的旧进程地址，
   两者永不相等。
```

时间线：`loaded: world=2 level=1` 打印 → 同帧返回 → blit → 崩。**一帧都不多。**

### 2.4 第三个全局 `DrawBufferNormalIni` 为什么也必须修

它不只是个冗余副本，而是**被 `update_display_map()` 每帧回写**的来源：

```c
/* map.c:55 */
void update_display_map(big_map_t* a1) {
    ...
    DrawBufferNormal = DrawBufferNormalIni;   /* 每帧回写！ */
    draw_buffer      = DrawBufferNormalIni;
}
```

`update_display_map()` 由 `DO_GROS_MOTEUR_NORMAL(1)` 调用（`pcmain.c:84`），
在 `DO_MAIN_LOOP_PC_NORMAL` 主循环里**每帧**执行。所以只修
`DrawBufferNormal`/`draw_buffer` 两个是不够的 —— 下一帧回来 `map.c:77` 又会从
`DrawBufferNormalIni` 把悬垂值重新灌回去。

同理 `INIT_GAME_MODE_NORMAL()`（`map.c:302`）与 `FIN_GAME_MODE_NORMAL()`
（`map.c:312-313`）也从这个全局双向派生。**源头是 `DrawBufferNormalIni`**。

## 3. 完备性审计

按"是否存在指向堆/模块、且会被后续代码重新传播的编外全局指针"扫了一遍：

| 符号 | 位置 | 是否需修 | 理由 |
|---|---|---|---|
| `draw_buffer` | `data.c:2624` | ✅ | 快照回滚 + `SWAP_BUFFERS()`/`map.c` 每帧重传 |
| `DrawBufferNormal` | `data.c:2618` | ✅ | 同上，且是 blit 三行判断的另一端 |
| `DrawBufferNormalIni` | `data.c:2614` | ✅ | **第三个洞**：`update_display_map()` 每帧回写 `DrawBufferNormal` |
| `DisplayBufferNormal` | `data.c:1536` | ❌ | 初始化 `NULL` 后再无赋值点（全仓库唯一引用是 `frame.c:42/349/617` 读）→ 不会悬垂 |
| `display_buffer` | `data.c:2630` | ❌ | 唯一赋值 `frame.c:42` 来自 `DisplayBufferNormal`（NULL）；`affiche.c:89-111` 是成对的 save/restore 局部替换 |
| `EffetBufferNormal` | `data.c:2616` | ❌（已覆盖） | **已在 region 表**（`SS_RGN_EFFET`，窗口 128000），重定位会修；日志 `hits=1` 印证 |
| `temp_mem_buf` | `data.c:2663` | ❌ | `FIRST_INIT` 里 `init_memory()` 重建，非快照内容，且无重传路径 |
| `main_mem_*` | `data.c:2653+` | ❌（已覆盖） | 属于 pool 机制，`ss_pool_ptr()` + `ss.pools[]` 已专门处理 |

结论：缺口就是这三个。`DisplayBufferNormal` / `display_buffer` 是 NULL 或无传播
路径，按最小改动原则不动。

## 4. 修复

`src/savestate.c`，插入位置：`global_app_state` 恢复之后、音频 fixup 之前
（紧挨 `SetPalette` 那段，因为都属"恢复后的平台/派生状态重算"）。

```c
/* DrawBufferNormal and draw_buffer alias game->draw_buffer.memory
 * (display.c assigns one from the other, SWAP_BUFFERS() re-derives
 * draw_buffer from DrawBufferNormal every frame). Both are plain .bss
 * globals sitting next to — not inside — global_app_state, so the
 * segment restore rewound them to the writing process' heap address
 * while the app_state backup could not cover them; relocation cannot
 * fix them either, because the block is a bare calloc with no
 * registered region window. Left alone, advance_frame() finds
 * draw_buffer != game->draw_buffer.memory and swaps the live pointer
 * out for the dangling one immediately before the blit. */
DrawBufferNormal = global_app_state.game.draw_buffer.memory;
draw_buffer = DrawBufferNormal;
DrawBufferNormalIni = DrawBufferNormal;   /* map.c: update_display_map() re-derives from this every frame */
SSLOGI("load step: draw buffer re-derived draw=%p source=%p",
       (void*)draw_buffer, (void*)global_app_state.game.draw_buffer.memory);
```

要点：

- **`global_app_state.game.draw_buffer.memory` 是权威活值**：它经 `app_backup`
  保护，且 `FIRST_INIT()`（`display.c:610`）本来就是这么给 `DrawBufferNormal`
  赋值的 —— 同一条派生关系，只是读档后要重算一次。
- 三个全局必须**同一趟全部重派**，且 `DrawBufferNormalIni` 不能被忽略，否则
  `update_display_map()` 下一帧就把悬垂值灌回来。
- `data.c`（unity build 第 61 个 include）在 `savestate.c`（第 137 个）之前，
  三个符号在编译单元内已可见，**无需改 `proto.h`**。
- 改动天然只在 Android 生效：`rayverse.c:136-138` 用 `#ifdef ANDROID` 包住
  `#include "savestate.c"`，桌面/Windows 构建根本不编译本文件。
- **不动 `ss_ctx_t`**：本次不新增字段，`sizeof(ss_ctx_t)` 不变，stamp 不变，
  旧槽文件仍可读（这一点很关键，见 §5）。

## 5. 风险评估

| 风险 | 评估 |
|---|---|
| 破坏同进程读档 | 无。同进程时 `game.draw_buffer.memory` 本来就是正确值，重派是无操作（幂等） |
| 破坏跨版本读档 | 无。重派读的是 `app_backup` 保下来的**本进程活值**，与快照内容无关；keep-live 机制不受影响 |
| 旧槽文件失效 | 无。未动 `ss_ctx_t` 布局，stamp 不变 |
| 掩盖真实 bug | 低。这三个全局的设计语义就是"永远等于 `game.draw_buffer.memory`"，重派是恢复不变式，不是绕过 |
| 遗漏别的同类全局 | 已做完备性扫描（§3），缺口收敛为这三个 |

## 6. 验证计划

1. **构建**：`cd android && gradle assembleRelease`。
2. **真机验收**（用户手动）：
   - 同版本：存档 → 杀进程 → 重进 → 读档 → **不闪退**，画面正常；
   - 日志确认出现 `load step: draw buffer re-derived draw=0x... source=0x...`
     且两者**相等**；
   - 读档后移动/跳跃数秒，确认 `update_display_map()` 每帧重传路径也干净
     （这是第三个全局必须修的验证点）。
3. **回归**：跨版本旧槽（`docs/savestate-phase2.md` §11.5 场景）仍可读。
4. **符号化备案**：如仍崩，用 `docs/android-debug-log.md` §9 的流程重新符号化，
   对比 fault addr 是否变化。

## 7. 证据文件

| 文件 | 内容 |
|---|---|
| `android/crashlogs/20261015-1428/tombstone_18.txt` | 14:28 崩溃 |
| `android/crashlogs/20261015-1428/tombstone_19.txt` | 14:29 崩溃 |
| `android/crashlogs/20261015-1506/tombstone_20.txt` | 15:07 崩溃（带日志复现） |
| `android/crashlogs/20261015-1506/logcat_rayverse.txt` | 精简日志 833 行（关键行见 §1） |
| `android/crashlogs/20261015-1506/live_logcat.txt` | 原始抓包 35 MB（已停） |

crashlogs 目录为排查产物，未入仓（已在 `.gitignore`）。

## 8. 环境备注（可复用）

- 真机是**无线 adb**（HONOR BKQ-AN80，Android 16 user build，`ro.debuggable=0`）。
  `-d`/`-s <serial>` 都取不到，用 `-t <transport_id>`；transport id **每次 adb
  重启会变**。
- **Git Bash 会把 `adb shell ls /data/tombstones/` 的 `/data/...` 做 MSYS 路径转换**，
  必须先 `export MSYS_NO_PATHCONV=1`。
- **这台 user build 非 root 也能直接 `adb pull /data/tombstones/*`**（目录属
  system、权限 775）—— 之前"真机抓不到 tombstone"的前提不成立。
- release 里 `__android_log_is_loggable` 结果**每进程缓存**，`setprop` 后必须
  `am force-stop` 冷启动才生效；prop 一直保留到手机重启。
- `savestate.c:1425` 附近旧注释说 `global_app_state` "excluded from the restore"，
  与实际不符（它先被覆写、再被 `app_backup` 盖回）。别照该注释推断。
