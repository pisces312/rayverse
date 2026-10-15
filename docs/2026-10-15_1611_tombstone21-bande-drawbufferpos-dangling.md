# tombstone_21：swap-buffer 修复后读档仍崩 —— Bande[] 派生指针同类悬垂（未决）

> 接续 `2026-10-15_1522_savestate-swapbuffer-dangling-globals.md`（对应
> `android-debug-log.md` §16）。15:22 修复合入后，同日 15:40 真机**同一旧槽**
> 读档再次闪退（tombstone_21），fault addr 与之前三次完全相同。
> 本文记录 tombstone_21 的符号化与机制推导，以及"用户实测新存档多次
> 重启读档不崩"这一反证带来的未决分歧。

## TL;DR

15:40 崩溃的包**已包含 15:22 修复**（BuildId 与本地 release 构建逐位一致），
崩溃点从 blit 读端挪到 `Display_Back_Screen → Copy_Plan0Diff_To_Buf` 的 memcpy
**写端**，写入目标正是那个来自存档快照的悬垂地址 `0xb40000752d0dc0e0`。
机制与 §16 同一类：`Bande[300]`（`.bss` 全局，`data.c:3328`）里的
`draw_buffer_pos` 字段是关卡初始化时从 `DrawBufferNormal` 派生的指针
（`menu.c:8 → screen.c:102/105`），整段回滚后变回写档进程地址；它指向的
裸 `calloc(64000)` 没有登记重定位窗口，重定位跳过，15:22 的"重派三个全局"
也够不到数组里的 300 个派生值。

**但**用户随后报告：用当前版本重新存档多次、重启读档多次**均不崩**
（退出方式确认非划卡片），且三个槽（旧档 + 两个新档）都是 world 2 level 1。
矛盾已在 §8 定论：**旧档是跨构建槽**——同构建进程的 draw buffer 地址在
这台机器上确定性复用，同构建槽天然有效；旧档写档进程的地址是
`0xb40000752d0dc0e0`，与当前任何构建的确定性地址都不同，故必悬垂。

## 1. 症状与抓证

- tombstone_21：2026-10-15 15:40，pid 8023，uptime 15s，SIGSEGV
  **write**（esr 0x45），fault addr `0xb40000752d0dc0e0`。
- backtrace：

  ```
  #00 libc.so (__memmove_aarch64_nt+308)
  #01 libmain.so (Copy_Plan0Diff_To_Buf+60)     BuildId: 0b60c535b3b5dd6e740a76165d578708f3d378cf
  #02 libmain.so (Display_Back_Screen+3836)
  #03 libmain.so (DO_MAIN_LOOP_PC_NORMAL+656)
  #04 libmain.so (PcMain+4720)
  ```

- **修复确实在包里**：本地 `android/app/build/outputs/apk/release/app-release.apk`
  内 `lib/arm64-v8a/libmain.so` 用 NDK `llvm-readelf -n` 读出的 BuildId 为
  `0b60c535b3b5dd6e740a76165d578708f3d378cf`，与 tombstone 逐位一致；源码
  `savestate.c:1446-1448` 的重派生补丁也在。即 15:40 崩的是**修复后的构建**。
- 关键寄存器（memcpy 内，x0=dest，x1=src，x2=len）：
  - `x0 = 0xb40000752d0dc0e0` —— **写目标**，与前三次崩溃的 fault addr
    逐位相同（存档快照里写档进程的 draw buffer 基址）；
  - `x1 = 0xb400006c7eb88743` —— 读源，本进程**有效**（tombstone 成功 dump
    了 x1 附近内存，内容为 `e4/e2/f0` 等像素字节）；
  - `x2 = 0x140 = 320` —— 整屏宽（`w2 - w1` 满幅）。
- 读源有效、写目标悬垂，正好对应两个缓冲区不同的重定位命运（§2）。

证据文件：`android/crashlogs/2026-10-15/tombstone_21.txt`（+ tombstone_20.txt，
未入仓，`.gitignore` 已覆盖 crashlogs）。

## 2. 根因推导

写入目标链条：

```
menu.c:8   LOAD_FND(): Init_Bande(no_fnd, plan0_width, plan0_height,
                                   PLAN0BIT, DrawBufferNormal)
screen.c:102/105   bande->draw_buffer_pos = dest_buf + cumulative_length * 320
                   （dest_buf == 当时的 DrawBufferNormal）
data.c:3328        bande_t Bande[300]  ← .bss 全局数组，在 seg 区域内
```

读档时：

1. `ss_do_load()` 整段 `.data/.bss` memcpy → `Bande[]` 全部字段还原为
   **写档进程**的地址值；
2. 重定位只平移"落在已登记区域窗口内的值"。`draw_buffer_pos` 指向的
   `engine.c:16` 裸 `calloc(1, 64000)` 无窗口 → 原样保留 → 悬垂；
3. 15:22 修复只重派生 `DrawBufferNormal` / `draw_buffer` /
   `DrawBufferNormalIni` 三个**独立全局**，够不到 `Bande[]` 里的派生值；
4. 读档不重跑 `LOAD_FND()`/`Init_Bande()`（15:07 日志证明读档同帧直接
   继续渲染），下一帧 `pcmain.c:93 → Display_Back_Screen` 用
   `bande->draw_buffer_pos`（`screen.c:154`）作 memcpy 写目标 → SIGSEGV。

读源为什么活着：`source_buffer_pos` 派生自 `PLAN0BIT`（`data.c:2643`，
`load.c:884` 赋为 `buffer2`），属于已登记/可重定位的缓冲区，跨进程后被
重定位平移到本进程活地址 —— tombstone 里 x1 可读印证。`savestate.c`
全文无任何 `Bande`（指数组）/`PLAN0BIT` 的处理，只有 `Def_Bande`
（查找表，`SS_RGN_BANDE`）已登记，与 `Bande[]` 是两回事。

与 §16 的关系：同一个洞（"指向未登记裸 calloc 块的派生指针，存在 .bss
槽位里"），§16 修的是三个独立全局，本次暴露的是数组字段。§16 §3 的
完备性审计表没有覆盖 `Bande[].draw_buffer_pos`（审计只扫了独立全局，
漏了结构体数组内的指针字段）——**审计方法本身需要修正**：凡是
`.bss` 里的结构体/数组，其指针字段都要按同一判据过一遍。

## 3. 反证：新存档多次重启读档不崩

用户实测（15:40 崩溃后）：当前版本**重新存档多次 → 重启 → 读档多次，
均无问题**。若 §2 推导完整成立，任何槽（新旧）跨进程读档后第一帧
`Display_Back_Screen` 都应必崩。矛盾。候选解释：

| # | 假设 | 支持/反对 |
|---|---|---|
| A | 新测试**没有真正杀进程**（划卡片/返回桌面后进程存活），读档实为同进程读档 —— 同进程下槽内地址本来就是活值，天然不崩 | 旧槽崩溃时的复现步骤明确是"彻底退出进程"；新测试若只是划走卡片，进程可能存活，无法区分 |
| B | 新测试的关卡/滚动状态**没走到 bande 写入路径**（`NbBande==0`、所有 `bande->length==0`，或 `world_5_cave lv10` 走 `Init_Effet_Chaleur`） | 旧槽是 `world=2 level=1`（Band Land，视差背景，bande 活跃）；新测试若换关卡或未滚动，路径可能未触发 |
| C | 旧槽本身有额外的内容问题（如由更早构建写出、`.bss` 内部布局有不可见差异），新槽没有 | 机制不明：同尺寸校验通过即按当前布局解释，无法解释"值恰好是写档进程 draw buffer 基址"；且同构建跨进程悬垂已被 14:28/15:07 三次复现证实，不依赖此假设 |

注意：**不存在"重新存档能修复"的机制**。新槽里的 `Bande[].draw_buffer_pos`
同样是写档进程的堆地址，跨进程读档后同样悬垂——若 A、B 都排除后仍不崩，
才需要回头质疑 §2 的某个环节（首要疑点：`Bande[]` 是否真的在 seg 回滚
窗口内、读档后是否有未发现的再初始化路径）。

## 4. 判别实验（决定下一步）

用当前版本，严格按旧槽崩溃场景复测：

1. 冷启动 → 进 **world 2 level 1**（与旧槽同关）；
2. 存档 → **`adb shell am force-stop com.rayverse.rayman`**（不能用划卡片代替；
   重进后用 `adb shell pidof com.rayverse.rayman` 或 logcat 确认 pid 已变）；
3. 重进 → 进 2-1 → 读档 → **走动/跳跃数秒让背景滚动**（`v68` 随滚动变化，
   写目标 = 悬垂基址 ± 滚动偏移，只要 bande 活跃就必踩）；
4. 重复 2–3 轮。

判据：

- **崩** → §2 实锤，实施 §5 修复；
- **稳定不崩且 pid 确已变更** → §2 有误，需重新审视（先做：
  `setprop log.tag.Rayverse-SS VERBOSE` + force-stop 后复测，读档日志里
  抓 `relocate` 的 `hits` 分布与 `draw buffer re-derived` 行，确认 Bande
  槽位到底经历了什么）。

建议同时保留**出问题的旧槽文件**（SAF 目录里 `rayverse_ss_*.bin`，可用
游戏内导出功能拷出，见 `android-save-export-import.md`）——它是唯一能
100% 复现的样本，修复前后都该用它回归。

## 5. 若实锤：修复方案

**首选：把 draw buffer 登记为重定位 region**（新增 `SS_RGN_DRAWBUF`，
`ptr = game.draw_buffer.memory`，`size = 64000`；先例是
`savestate.c:421` 的 `SS_RGN_EFFET`）。重定位会自动平移**所有**指向该块的
槽位——三个 swap-buffer 全局（使 15:22 的手工重派生变为冗余但无害）、
`Bande[].draw_buffer_pos`、以及未来任何同类消费者，一次收敛整类问题。

代价与注意：

- region 数变化会改 stamp（`savestate.c:298` 哈希含
  `sizeof(ss_rec_t) * SS_RGN_COUNT`）→ 旧槽按预期保护被拒，**需要重新存档**
  （这是修复的副作用，不是修复手段）；
- 不动 `ss_ctx_t` 布局（沿用 544 字节冻结约束）；
- 该区域内容每帧被重画，payload 保存/恢复无实际意义，但按现有结构会
  随槽多占 64000 字节，可接受；若要省，可给 region 加"只重定位、不存
  payload"的标志位（改动更大，非必要不做）。

备选（打地鼠，不推荐）：读档时记
`delta = 活值 - 快照里的旧 DrawBufferNormal`，平移
`Bande[0..NbBande].draw_buffer_pos`。改动小，但下一个同类消费者出现时
还要再补一刀；且 §16 §3 的审计已证明"枚举独立全局"这种方法会漏
结构体数组字段。

## 6. 附带结论：与 release 日志标记无关

本次崩溃栈全在渲染路径（`Display_Back_Screen` / memcpy），无任何日志帧；
fault addr 来自存档快照。`is_loggable`/dlsym 那类问题已按
`2026-10-15_1047_release-weak-is-loggable-verified.md` 验证修复。
"release 用宏剥掉全部日志"可作为卫生措施降低海森堡效应（AGENTS.md
规则 6），但防不住这一类存档回滚悬垂指针的崩溃，两件事不要混在一起。

## 7. 证据文件

| 文件 | 内容 |
|---|---|
| `android/crashlogs/2026-10-15/tombstone_21.txt` | 15:40 崩溃（修复后构建，本次主角） |
| `android/crashlogs/2026-10-15/tombstone_20.txt` | 15:07 崩溃（修复前，对照） |
| 真机 `/data/tombstones/tombstone_18/19` | 14:28/14:29 两次（已在 `crashlogs/20261015-1428/`，见 §16 文档） |
| `savestates/2026-10-15_slot1_w2-1_repro-crossbuild-drawbuf.bin` | 问题槽原档（§8 的解剖对象，修复后回归样本） |
| `savestates/2026-10-15_slot2_w2-1_ok.bin` / `..._slot3_...ok.bin` | 同构建正常槽（对照） |

## 8. 复盘：槽文件解剖定论（16:40 补记）

用户用「首页 Export saves」把三个槽导出（导出功能 2026-10-15 起纳入
`rayverse_ss_*.bin`），adb pull 后直接解剖文件内部指针值，一次性定论。

### 8.1 方法

- 槽文件布局：`ss_file_hdr_t`（88 字节）+ `ss_rec_t[14]`（40 字节/条）+
  各 region payload，seg payload 从文件偏移 **648** 开始；
- seg 内符号偏移 = 符号 vaddr − 可写 LOAD 的 `p_vaddr 0xfd160`（seg 大小
  `0x62970`≈395K，与模拟器判据吻合）；符号取自未剥离的
  `intermediates/cxx/Release/527143x1/.../libmain.so`（BuildId `0b60c535…`）；
- `bande_t`：`source_buffer_pos` @+16、`draw_buffer_pos` @+24、stride 40。

### 8.2 结果

| | UI 槽 1（旧） | UI 槽 2/3（新，16:00/16:01 写） |
|---|---|---|
| num_world/level | 2-1 | 2-1 |
| draw == DrawBufferNormal == Ini == Bande[0].dst | `0xb40000752d0dc0e0` | `0xb400006edfd15120`（两档相同） |
| Bande[0].src == PLAN0BIT | `0xb40000732a4f07f4` | `0xb400006c7e3787f4` / `…7d7787f4` |
| 内容自洽性 | ✅ 四槽互洽 | ✅ 四槽互洽 |

三个结论：

1. **旧档不是坏档**：所有指针槽互洽（draw == dbn == ini == Bande[0].dst，
   src == PLAN0BIT），`.bss` 布局与当前一致（跨版本错位假设排除）。
   它崩是因为**它的 draw buffer 地址 `0xb40000752d0dc0e0` 在任何当前构建
   的进程里都不存在** —— 该值就是 tombstone 的 fault addr，写于 12:37:58
   （v1.0.4 构建 12:41 之前），写档构建与 `f71f2f03`/`0b60c535` 的分配布局
   都不同。
2. **同构建跨进程读档有效的真正原因**：UI 2/3 由 `0b60c535` 写、同一构建
   新进程读，多次稳定 —— 说明**这台机器上同构建进程的 draw buffer 每次
   启动落在同一地址**（`0xb400006edfd15120`，分配序列确定性）。裸 calloc
   指针靠这个地址巧合活着，不是靠重定位。PLAN0BIT 侧两档地址不同
   （pool 内分配），靠**已登记的 pool 窗口**重定位修好，与 draw buffer 形成
   直接对照。
3. **机制是"跨构建"而非"跨版本布局"**：stamp 只覆盖 `ss_ctx_t` 尺寸与
   region 尺寸，不覆盖符号布局，也不覆盖"地址巧合"。任何写档构建与读档
   构建分配布局不同的槽（如 UI 1），读档必在 draw-buffer 指针家族上悬垂：
   `f71f2f03` 三次崩 blit，`0b60c535`（15:22 修复只重派三个全局）崩到
   下一个消费者 `Bande[].draw_buffer_pos`。

### 8.3 对 §3/§4 的修正与结论

- 假设 A（进程没真死）：排除（用户确认退出方式，且同流程下 UI 1 必崩）；
- 假设 B（未触发 bande 路径）：排除（三档全部 2-1、`Bande[0].length=288`）；
- 假设 C 修正为「**跨构建槽**」：不是内容损坏，是写档进程的堆地址在
  读档进程无效——用户"旧存档的内容问题"的直觉正确。

**修复结论不变且更明确**：登记 `SS_RGN_DRAWBUF`（§5 首选方案）。它把
"地址巧合"变成"重定位保证"，跨构建槽也能读——**UI 1 槽就是最好的回归
样本**：修复构建读它应不崩（stamp 变化被拒读也正常，届时用槽内指针
delta 验证）。在修复之前，**跨版本/跨构建读档不承诺支持**：keep-live 只
保代码/字符串指针，裸 calloc 堆指针同构建靠巧合、跨构建必炸。
