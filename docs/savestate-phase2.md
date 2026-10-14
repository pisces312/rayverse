# Savestate Phase 2 — 落盘、多槽位与跨进程恢复

Phase 1（会话内单槽，真机已验收）见 `docs/savestate-plan.md`。本文只解决 Phase 2 的三个新问题：
快照写文件、多槽位、**应用重启后还能恢复**。第三条是全部难点所在，前两条的设计也围绕它。

## 1. 硬问题：ASLR

快照的本质是"一堆绝对地址的原始字节"。同一进程内地址稳定，所以 Phase 1 成立；新进程里
libmain.so 的加载基址、每个 `calloc`/`malloc` 的返回地址全都重新随机化，原样 memcpy 回去就是
一批悬垂指针。要跨进程恢复，必须先量化"哪些字节是地址、它们指向谁"。

### 实测（2026-10-06，当前 arm64 debug 产物 `llvm-readelf`）

libmain.so 的 3 个 PT_LOAD：

| 段 | vaddr | memsz | 角色 |
|----|-------|-------|------|
| LOAD R_E | `0x0` | 0x104210（1.0 MB） | text + rodata，快照里大量指针的目标 |
| LOAD RW（GNU_RELRO） | `0x108210` | 0x4df0（19.7 KB） | `.got`/`.data.rel.ro`，重定位后只读，**不快照** |
| LOAD RW | `0x110418` | 0x62e88（**395 KB**） | `.data + .bss`，即 Phase 1 抓的可写段 |

偏移落在可写段内的重定位（`--relocs -W` 按 offset 过滤）：

| 类型 | 条数 | 目标 |
|------|------|------|
| `R_AARCH64_ABS64`（本模块定义的全局符号） | 1416 | R_E 段内的函数：`DoRaymanCollisionDefault`、`ObjectUTurnDefault`、`DO_ONE_CMD`、`handle_22_GO_Y` … |
| `R_AARCH64_RELATIVE` | 84 | R_E 段内 |
| `R_AARCH64_ABS64` | 91 | 可写段自身 |

即加载后 `.data` 里躺着约 **1500 个指向本模块 text 段的函数指针**（引擎的 command 表、碰撞回调表、
对象行为表都是函数指针数组），另有约 91 个段内自指。2a 的探针实测 `libmain=1652`、`seg=121`
（见 §9），与这张表逐项对得上。

结论：

1. 需要重定位的"绝对地址"是**千级**而不是几万级，且它们的**相对偏移在 ASLR 下不变**
   —— 一个 `delta = 新基址 - 存档时基址` 就能全部修正。
2. 池基址、`flocon_tab`、`rvb_special`、jumelle 查找表这些是**我们的堆**，不是模块内的；
   只要恢复时知道"存档时的地址 → 本进程当前等价块的地址"的映射，同样是一个 delta。
3. 音频堆块（`bnkDataFixe`/`bnkDataWorld`/`ptrTchatch`/`ogg_cd_track`）的内容不在快照里，
   Phase 1 的"对比-回填-重载"逻辑原样复用，只是 delta 来源改成 `LoadBnkWorld()` 之后的活指针。
4. 那 1500 个函数指针的目标全在 libmain 自己内部（探针 `mapped_other` 只剩 479，且集中在
   pool3，样本看是打包整数）——**没有**指向 libc/SDL 的指针混在快照里，2b 只需要一个模块 delta。

### 统一模型：区域表（region table）

Phase 1 里"段 + 5 池 + 2 个堆块 + 4 个音频指针"这套零散记录，泛化成一张区域表，每个区域带一个 `kind`：

| kind | 含义 | 快照里存什么 | 恢复时 delta 来源 |
|------|------|--------------|-------------------|
| `COPY` | 内容属于游戏状态 | 内容副本 | 本进程里同一逻辑块的新地址（池头 / `flocon_tab` / `rvb_special` / `EffetBufferNormal` / `Def_Bande` 等，若需） |
| `COPY_RELOAD` | 内容能从游戏数据重建 | 只存"存档时的地址" | 引擎重新加载后的活地址（`LoadBnkWorld`） |
| `KEEP_LIVE` | 平台/宿主拥有 | 不存 | 无（恢复后回填活值，Phase 1 已实现） |
| `ADDRESS_ONLY` | 内容由初始化代码确定、地址被状态引用 | 只存"存档时的地址" | 新进程里重新算出的同名块地址（jumelle 四张表；`Def_Bande` 若为纯查找表也归此类） |

需要额外接进区域表的堆块（来自 `grep malloc/calloc`，一次性分配、整个会话不释放）：
`display.c:611 EffetBufferNormal`(128 K)、`jumelle.c:170-173` 的 `DistPointX`/`DistPointY`/`ExpPoint`/`JumelleZoomDef`、
`screen.c:9 Def_Bande`；`engine.c:13 create_palettized_image` 的 image 缓冲是加载期临时量，
若探针发现被状态长期引用再补。

### 两遍重定位（关键正确性设计）

> **⚠ 2b 实现时已修正此方案**：落地的是"按值分类、加目标区 delta"的单遍扫描（字节粒度），
> 不是下面的两遍歧义表；原因与正确性论证见 §10。下面保留原文供对照。

朴素做法"逐槽判断落在哪个区域就加那个 delta"有一个隐藏错误：**快照内部自引用的指针**
（槽 X 指向同一快照里的槽 Y，例如全局指向全局、池 A 指向池 B 的块）在源和目标一起搬移时
delta 必须为 0 —— 而朴素做法会对它们加上目标区域的 delta，把正确的内部指针改坏，还悄无声息。

所以分两遍（恢复侧，全在帧边界一次性完成）：

1. **歧义标记**：把所有将被覆写的缓冲区（可写段副本 + 各 `COPY` 区域副本，它们就是"被搬移的整体"）
   的 8 字节对齐槽按 `saved_base` 归类，落在 ≥2 个这样区域计数里的槽记入歧义表。
2. **应用 delta**：歧义槽保持原值；非歧义槽按所属区域加 delta。
   落在"搬移区"之外的目标（text/rodata、`ADDRESS_ONLY`/`COPY_RELOAD` 块）永远唯一命中，正常修正。

误判风险可忽略：候选值要求 8 字节对齐、`≥0x1000` 且严格落在某区域 `[base, base+size)` 内。
arm64 用户态 mmap 地址在 `0x5e00…`～`0x7f…` 量级，游戏状态里这种量级的整数只可能是指针；
按区域总跨度 ~10 MB / 地址空间 2^40 估，70 万槽的期望误判 <1，而两遍法已经把最大的一类
（内部自引用）从"误判"变成"规则明确不动"。

> 探针（2a 已实现，`ss_run_probe()`，每次 dump 时按区域各打一行 + 一行 TOTAL）：把快照里每个
> 8 字节槽按"像不像指针"分桶 —— `small`(<0x10000) / `nonaddr`(≥2^48，两个打包 u32) /
> `self`(指向被扫描的缓冲自身，两遍法里 delta 必须为 0) / `seg` / `libmain` / `pool[i]` /
> 命名堆块 / `heap_other` / `mapped_other` / `unmapped`，并用 `noise_low` 标出其中 <2^40 的打包整数。
> 实测数据与判读见 §9。

## 2. 文件格式（v1，已实现）

```
rv_file_hdr_t（小端，64 字节，紧接 payload）
  u32 magic = 0x53535652 ("RVSS") | u32 fmt = 1 | u32 slot 版本号见 fmt
  u32 stamp          // 内存布局指纹，见 ss_compute_stamp()
  u32 rec_count      // = SS_RGN_COUNT = 8
  i32 world, i32 level
  u64 created        // epoch 秒，供槽位列表显示
  u64 payload_bytes, u64 payload_hash   // FNV-1a 64
  u64 aud[5]         // bnkDataFixe / bnkDataWorld / ptrTchatch / ogg decoder / ogg FILE*
region_table[]（8 × rv_rec_t，随头一起写）
  u32 id | u32 kind | u64 base | u64 size | u64 len | s64 cursor
payload: 按 region 顺序拼接的 COPY 内容（raw，不压缩）
```

两处与最初设计不同，都是实现时定的：

- **`build_id[16]` 换成 `stamp`（u32）**。`.note.gnu.build-id` 在 ndk_translation 下不保证已映射，
  而且它只覆盖"是不是同一个 so"，覆盖不到"全局变量的相对布局变没变"。`ss_compute_stamp()`
  改为一组**符号到符号距离**（`During_The_Menu-global_app_state`、`flocon_tab-rvb_special`、
  `main_mem_level-main_mem_fix` 等 10 对）+ 段大小的 FNV-1a 折叠：ASLR 搬移整个段时距离不变，
  改一个全局的定义或顺序就会变。实测同一构建的三个进程 stamp 恒为 `5aa80b63`，
  而段基址在它们之间差了 9.6 MB —— 正好是"指纹相同、布局搬移"这一类，交给状态码 10 拒绝。
  可在 UI 线程调用（`savestate_slot_info` 只读头，不碰引擎状态）。
- **`crc32` 换成 64 位 FNV-1a `payload_hash`**。只用于判断文件是否被截断/篡改，不需要 CRC 多项式。

- 体积 ≈ **5.9 MB/槽**（payload 5861 K），落在
  `filesDir/rayverse_save/rayverse_ss_<slot>.bin`（文件名 `SS_SLOT_FILE`，最多 3 个），与现有
  `RAYMAN*.SAV/.CFG` 同目录，于是**存档导出/导入天然带上即时存档**（2c 几乎零成本）。
  写入走 `<slot>.bin.tmp` + `rename()`，掉电/被杀不会留下半截文件。
- 拒绝条件与状态码（`savestate.h` 里是权威定义）：
  1 已存槽 / 2 存档被拒（不在游玩）/ 3 内存不足 / 4 已读槽 / 5 无快照 /
  6 读档被拒（不在游玩）/ 7 world-level 不符 / **8 写文件失败** /
  **9 文件不可用**（magic、fmt、stamp、payload_hash 任一不符）/
  **10 布局已搬移**（某 region 的 `saved_base` 与本进程 `live_base` 不同 → 跨进程，2b 前硬拒）。
- 压缩先不做；若嫌体积，区域级 RLE 对这些数据命中率很高，但不阻塞功能。

## 3. 多槽位与 UI（已实现）

- 槽位固定 **3 个**（`SS_MAX_SLOTS` in C / `SS_SLOTS` in Java，两处必须同步）。齿轮设置菜单里
  原有的两项变成二级 `AlertDialog`：
  - 「即时存档」→ `showSlotDialog(true)`，标题"保存到槽位"；
  - 「即时读档」→ `showSlotDialog(false)`，标题"读取槽位"。
  选定后 `nativeRequestSaveState(slot)` / `nativeRequestLoadState(slot)` 排队，真正的 dump/apply
  仍在游戏线程的帧边界执行。
- 列表项文案来自 `slotLabel()`：`槽位 N · 世界w-关卡l · MM-dd HH:mm`；无文件 → `槽位 N · 空`；
  stamp 不符 → `… · 版本不符`（存盘侧写作"旧文件（覆盖）"）；文件存在但头解析失败 → `不可用`。
  元信息只读文件头，UI 线程可直接调，不需要游戏处于任何特定状态。
- 内存侧仍是**一套复用缓冲**，槽位只存在于文件；反复存读不涨内存，也不会一次驻留 N 份快照。
- 状态回传：Java `pollSaveStateStatus()` 每 250 ms 读一次 `nativeGetSaveStateStatus()`（读一次即清零），
  最多 40 次，并按状态码 toast 提示。C 侧每个分支（含被拒）都留了 breadcrumb 日志。
- JNI 入口（`android_jni.c`）：`nativeRequestSaveState(int)` / `nativeRequestLoadState(int)` /
  `nativeGetSaveStateStatus()` / `nativeGetSaveStateSlotInfo(int)`（返回 `"world,level,epoch,usable"`
  或 `"empty"` / `"broken"`）。

## 4. 跨进程恢复的启动编排（2b 的 UX 难点）

引擎必须"已经站在目标关卡的游玩帧"才能应用快照（`ss_in_gameplay()` 门禁 + 调用栈形态一致）。
不打算改引擎，改用引擎本来就会写的全局做引导 —— **armed resume**：

1. `nativeMain` 之前，Java 读快照头，调 `nativeArmSaveState(slot)` 把目标 world/level 交给 C 侧。
2. 帧钩子里若已 arm 且当前处于世界地图态（`During_The_Menu` 且 `!RaymanDansUneMapDuJeu`），
   写 `num_world_choice`/`num_level_choice` = 目标值并置 `new_world = 1`。
   这正是 `SYNCHRO_LOOP(WORLD_CHOICE)` 的退出条件（`display.c:312`），
   引擎随后自然走 `DEPART_WORLD` → `LoadBnkWorld` → `DEPART_LEVEL` → `INIT_MOTEUR_LEVEL`，
   把目标关卡**完整重新加载一遍**，池/纹理/调色板等基础设施处于正常游玩形态。
3. 重载完成后门禁为真且 world/level 与元数据一致 → 读文件、重定位、应用，一次成功，用户只需再点一次开始。

代价与退路：

- 用户会看到一次世界地图过场 + 关卡开场动画（OK 可跳过，见技能文档）。
- 若目标关卡在原生 `RAYMANn.SAV` 里尚未解锁，`SELECT_LEVEL` 会改掉选择 → 引导失败；
  退化为"请自己走到该关并进入，进入后自动恢复"（同一套匹配逻辑，无需额外代码）。
- 即时状态不会写回原生存档：读档后若用户改用原生流程退出重进，进度仍是原生存档那份。
  文档和 UI 上要讲清"即时存档 ≠ 游戏存档"。

## 5. 分阶段

- **2a（可独立交付）✅ 已完成（2026-10-06 模拟器验收，实测记录见 §9）**：区域表重构 + 落盘/读盘 + 版本/`stamp`/payload 哈希校验 + 多槽 UI + 指针分类探针。
  同会话内"存到槽位 → 读回"即可全流程验证（地址未变，重定位走 delta=0 分支），风险低。
- **2b ✅ 已实现（2026-10-14，代码 + 构建通过；实现记录见 §10，待用户真机/模拟器手动验收）**：
  按值分类重定位（字节粒度）+ `ADDRESS_ONLY` 区域接表 + armed resume 启动编排（启动弹窗确认）。
- **2c**：即时存档文件走现有 SAF 导出/导入通道，同 `stamp` 的设备间可共享。

## 6. 验证清单

1. ✅（模拟器，见 §9）2a：关卡中存到槽 1 → 继续玩 → 读槽 1 回滚；跨槽位互不覆盖；文件头
   `world/level` 与画面一致。
2. ✅（模拟器，见 §9）2a：APK 重装（同版本同签名，进程重启）后读旧文件 → 因 ASLR **不应**被允许
   直接应用；验证 `stamp` 命中但重定位失败路径不崩（2b 前该用例提示"该存档来自另一次运行"）。
3. 2b：存 → 完全杀掉进程 → 重开 → 弹窗确认 → 自动引导 → 恢复到原位（生命/Tings/敌人/计时全回滚）。
4. 2b：存 A 世界 → 杀掉 → 重开 → 引导恢复；音效正常（bank 重载路径 + 恢复后 `stop_all_snd()`）。
5. 2b：`stamp` 不符（换新 APK）→ 状态码 9 拒绝（"版本不符"），无崩溃。
6. ✅（模拟器，见 §9）探针：三类命中数稳定，未知 rw 命中数为 0。
7. ✅（模拟器，见 §9）连续 10 次存读内存不增长；单次 dump/load 的帧耗时（写 5.9 MB）可接受。

## 7. 代码落点（预估，实际足迹见 §9 末尾）

| 文件 | 改动 |
|------|------|
| `src/savestate.c` | 区域表化重构；`ss_dump_file`/`ss_load_file`；两遍重定位；探针；状态码 8/9/10 |
| `src/savestate.h` | 槽位 API、arm API、元信息查询 |
| `src/engine.c` | 无新增（钩子已在 `advance_frame()` 顶部） |
| `android/.../android_jni.c` | 4 个新 JNI 入口 |
| `android/.../RayverseActivity.java` | 槽位对话框 + native 声明 |
| `android/.../RayverseActivity.java` / `SetupActivity.java` | 2b：启动时 arm |
| `docs/android-debug-log.md` | 新踩坑追加 |

引擎核心文件依旧零改动；所有新增仍在 `#ifdef ANDROID` 内。

## 8. 已定方案（2026-10-06 用户拍板）

1. 本轮只做 **2a + 指针探针**：区域表重构、落盘/读盘、版本/`stamp`/payload 哈希校验、3 槽 UI、
   指针分类统计。拿探针数据后再落地 2b（两遍重定位 + ADDRESS_ONLY 接表）。
2. **3 槽**（约 17 MB，raw 不压缩）。
3. 跨进程恢复入口用 **armed resume（启动自动引导）**，属于 2b。

## 9. 2a 实测记录（2026-10-06，pixel6 模拟器 / ndk_translation 跑 arm64 debug 包）

世界 1 关卡 1，四次进程（pid 9391 / 10240 / 10645 / 10953）。日志与截图在 `android/_scratch/`。

### 落盘与读盘

| 项 | 实测 |
|----|------|
| payload | 5861 K（区域表 8 项，头+表 416 B） |
| 单个槽文件 | 6,002,720 B，`files/rayverse_save/rayverse_ss_{0,1,2}.bin` |
| 写盘 | 26 / 39 / 62 / 85 ms（带探针 85 ms，探针本身约 40 ms） |
| 读盘 | 34 / 57 / 58 / 155 ms |
| stamp | 同一构建四个进程恒为 `5aa80b63` |
| 可写段基址 | `0x735629311508` / `0x735628988418` / `0x73562931c418` / `0x735626186518`（四个进程跨度 **51,994,368 B ≈ 49.6 MB**；相邻两次启动差 10,043,392 B） |

会话内：存槽 1 → 走动/摔死扰动 → 读槽 1，位置/Lums/Tings 精确回滚，读后仍可继续玩；
三个槽互不覆盖（截图 `emu_p2a_30/31/32/34/35`）。修完探针后在**新进程**（pid 10953）里把
存→读整条链复跑一遍，同样通过（`slot 0 read … in 34ms` → `loaded:`，截图 `emu_p2b_00/02/03`，
crash 缓冲与 `dumpsys activity exit-info` 均空）。杀进程后槽文件仍在（`ls` 见上），
但读它被明确拒绝：

```
slot 0: region seg moved since capture (saved=0x735628988418 live=0x73562931c418
        delta=10043392) — cross-process restore needs phase 2b      → 状态码 10，无崩溃
```

### 指针分类探针（TOTAL，750,289 个 8 字节槽）

```
slots=750289 small=475161 nonaddr=259775 self=1 seg=121 libmain=1652
pool=[2286,314,53,627,0] heap_other=1385 mapped_other=479 unmapped=8427 noise_low=7128
-> flocon_tab=1  rvb_special=1  EffetBufferNormal=1  DistPointX=1  DistPointY=1
   ExpPoint=1  JumelleZoomDef=1  Def_Bande=1
```

判读：

1. **需要重定位的地址共 5,054 个（0.67%）**：`libmain` 1652 + 池 3280 + `seg` 121 + `self` 1。
   与 §1 的 readelf 表逐项对得上（1416+84=1500 条重定位落在 R_E，实测 1652；91 条段内自指，
   实测 `seg` 121）。2b 的两遍法只需处理千级槽，成本可以忽略。
2. **`self=1`** —— 快照内部自引用只有 1 处（可写段内），两遍法的歧义表实际很薄；
   但仍必须实现，因为它的 delta 是 0 而不是目标区域的 delta。
3. **`libmain` 全部落在本模块**：`mapped_other` 从修探针前的 1661 降到 479 且集中在 pool3，
   样本是打包整数 → 快照里没有指向 libc/SDL 的函数指针，2b 只要一个模块 delta。
4. **8 个命名堆块各被指 1 次**（`flocon_tab`/`rvb_special`/`EffetBufferNormal`/
   `DistPointX`/`DistPointY`/`ExpPoint`/`JumelleZoomDef`/`Def_Bande`）：这些表基本按**下标**访问，
   不是靠指针链，所以 `ADDRESS_ONLY` 接表面很小。
5. **噪声占 98%**：`small` + `nonaddr` = 734,936；剩下的 `unmapped`/`heap_other` 里 7,128 个
   低于 2^40（`noise_low`），样本形如 `0xffc0006f00cc`、`0x292a2929`，是打包的 16/32 位游戏数值。
   → "未知命中归零"不能作为 2b 的判据，真正的判据是恢复后能正常玩。

### 踩到并修掉的两个测量假象

- `heap_other=1385 / unmapped=268202` 一度被当成"大量悬垂指针"。加 `nonaddr`(≥2^48) 与
  `noise_low`(<2^40) 两个桶后看清：它们是打包整数，不是地址。
- **`libmain=0` 是假的**：`ss_load_maps()` 用 `strstr(line, perms)` 定位路径，实际停在**文件偏移**列，
  加上 `name[72]` 把安装路径 `/data/app/~~<32>==/com.rayverse.rayman.debug-<24>==/lib/arm64/libmain.so`
  截断在 `libmain.so` 之前 → 模块区间从未注册（`regions=13` 而不是 14）。按字段数跳过
  perms/offset/dev/inode、`name[128]` 之后 `regions=14`、`libmain=1652`。
  教训：桶计数为 0 时先证明"这一类被登记过"，再当作结论。

### 改动足迹（本轮）

`src/savestate.c` +865、`src/savestate.h` +29、`android_jni.c` +32、`RayverseActivity.java` +84，
新增本文档；引擎核心文件与 `engine.c` 零改动，全部仍在 `#ifdef ANDROID` 内。

### 留给 2b 的待办（按探针结论排）→ 已按此实施，落地形态见 §10

1. ~~两遍重定位~~ → 单遍"按值分类"重定位（字节粒度），见 §10-1 的修正说明。
2. `ADDRESS_ONLY` 接表 ✅：jumelle 四张表 + `EffetBufferNormal` + `Def_Bande`。
3. armed resume 启动编排 ✅（§4 的变体：启动弹窗确认后自动引导，见 §10-4）。
4. 探针在**其他世界/关卡、特效进行中、CD 音乐播放中**复跑一遍 —— 仍待做；本轮数据只覆盖
   世界 1 关卡 1 的一个静态时刻，函数指针表可能在别的关卡被填进新的全局。

## 10. 2b 实现记录（2026-10-14；代码 + 构建完成，待手动验收）

改动足迹：`src/savestate.c`（重定位 + 区域表扩展 + armed 编排）、`src/savestate.h`、
`src/proto.h`（2 个 hook 声明）、`src/display.c`（2 行 `#ifdef ANDROID` hook）、
`android_jni.c`（`nativeArmSaveState`）、`RayverseActivity.java`（启动弹窗 + armed 轮询）。
引擎核心逻辑零改动。文件格式升到 **fmt=2**，2a 的旧槽文件会被拒（状态码 9，UI 显示"版本不符"）。

### 10.1 与 §1 设计的两处修正（实现时定，理由如下）

1. **重定位是"按值分类、加目标区 delta"，不是两遍歧义表**。§1 说"内部自引用 delta 必须为 0"
   ——该结论只在同进程（所有 delta=0）时成立。正确语义是标准指针搬移：
   `v += (live_base - saved_base)`，delta 由**值指向的区域**决定；段内自引用（global→global）
   要加的正是段自己的 delta，跨进程时它不是 0。目标按精确分配跨度登记、互不重叠，
   "歧义"只剩一种真实情形：两个 **delta 不同**的区域同时认领一个值 —— 这种槽保持原值并计数
   （`relocate: … ambiguous=N`，预期 0 或个位数）。段与 libmain text 目标互相重叠但
   delta 恒相同（同一模块同一偏移），按同一答案处理。
2. **扫描是字节粒度，不是 8 字节对齐**。`obj_t` 是 `#pragma pack(1)` 的 **155 字节**结构，
   数组里 7/8 的元素其指针字段落在非对齐偏移（`mapobj[25]`、`ray`/`raylittle`、
   `level.objects` 全是 obj_t 数组），2a 探针只扫对齐槽、只看到其中 1/8。字节粒度的
   混合读（移位重叠）误报由地址形态兜底：真实指针是 `0x0000_7xxx_xxxx_xxxx`，移位读出的
   混合值要么 <2^40（噪声地板）、要么 ≥2^48（打包 u32 对）、要么高低半字落在区域窗口的
   错误位置。期望误判 ≪1；判据仍是恢复后能正常玩，`relocate:` 的 hits 明细可对照 §9 基线
   （libmain 千级、pool 数千级、seg 百级）。

### 10.2 区域表扩展与文件格式 v2

- 新增 6 个 `ADDRESS_ONLY` 区域：`EffetBufferNormal`(128000)、`DistPointX`、`DistPointY`、
  `ExpPoint`、`JumelleZoomDef`、`Def_Bande` —— 只存"存档时地址 + 跨度"，无 payload；
  重定位把快照里指向它们的引用映射到本进程的同名块。
- **`rec.size` 语义改为地址跨度**：池 = `sizeof(mem_t) + capacity`（引擎的池全局指向 header，
  且 bump 分配可能用满数据区最后几字节），payload 仍只含 `data[0..capacity)`；
  seg/flocon/rvb 跨度 = payload。`SS_RGN_COUNT` 8 → 14，`SS_FMT_VERSION` 1 → 2。
- 重定位目标 = 14 个区域 + `libmain` text/rodata/relro 跨度（delta = 段 delta，由
  `dl_iterate_phdr` 的 PT_LOAD 并集给出）。
- 音频不进区域表：沿用 Phase 1 的"对比-回填-重载"，另加恢复后 `stop_all_snd()` 清掉
  `snd_t::data` 里指向旧 bank 的残留指针（下一次发声按活 bank 重新解析），并同步
  `TchachPerduPtr`。

### 10.3 重定位时机与状态码

`ss_read_slot()` 在 payload 哈希校验通过后、`ss_do_load()` 应用前执行重定位（就地改
快照缓冲）；同进程所有 delta=0 直接跳过。某区域"存档有、本进程无"仍由大小校验挡下 →
**状态码 10 语义收窄为"无法映射到本进程"**（不再是"来自另一次运行"，跨进程已被支持）。

### 10.4 armed resume（§4 的落地变体）

- **触发**：`RayverseActivity.onCreate` 弹窗"检测到即时存档（世界w-关卡l · 时间）"，
  [恢复] 才 `nativeArmSaveState(slot)`（选 epoch 最新的可用槽）；[不用] 则一切照旧。
  （用户拍板：启动引导需要确认，不劫持正常游玩。）
- **引导**：两个一行 hook（均 `#ifdef ANDROID`）——`WORLD_CHOICE()` 顶部 `new_world = 1`
  （世界地图第一帧即退出），`DO_WORLD_MAP()` 在 `DETER_WORLD_AND_LEVEL()`/`SELECT_LEVEL()`
  之后钉住 `num_world_choice/num_level_choice`（DETER 会用光标奖章覆盖这两个值，必须后钉）。
  引擎随后自然走 `DEPART_WORLD → LoadBnkWorld → DEPART_LEVEL → INIT_MOTEUR_LEVEL`。
- **应用**：目标关第一帧（`ss_in_gameplay()` 且 world/level 匹配）读档 + 重定位 + 应用；
  引导失败不丢档 —— 同一匹配检查每帧都在，手动走到该关也会触发。手动存/读档会取消 armed。
- 已知边界：`DETER_WORLD_AND_LEVEL()` 的副作用（`enter_casse_brique()`、`You_Win` 分支）用的是
  钉住前的光标值，极端情形（光标停在蛋糕奖章且已通关）可能走结局流程；引导不看
  `t_world_info` 解锁状态，直接加载目标关。

### 10.5 手动验收清单

1. **同会话回归**（2a 用例）：关卡中存槽 1 → 走动/死亡 → 读槽 1 精确回滚；日志应有
   `relocate: layout identical to capture, nothing to fix`。
2. **跨进程**：存槽 1 → 杀掉进程（或 `adb force-stop`）→ 重开 → 弹窗[恢复] → 自动进目标关
   → 位置/Lums/生命/Tings 回滚，继续玩正常，无 SIGSEGV（`adb logcat -d -b crash` 为空）。
3. **日志判据**（`Rayverse-SS:V`）：`armed resume: slot N -> world=W level=L` →
   `armed resume: applying slot N at world=W level=L` → `relocate: fixed=… ambiguous=… targets=15`
   → 每目标 `delta/hits` 明细 → `loaded: world=W level=L`。`libmain` hits 千级、pool 数千级、
   seg 百级、`ambiguous`≈0（对照 §9 探针基线）。
4. **音效**：跨进程恢复后音效/音乐正常（bank 重载路径）。
5. **弹窗[不用]**：正常进游戏不被打断；之后手动存/读档正常（armed 已取消）。
6. **版本不符**：换新构建（`stamp` 变）→ 旧槽文件标"版本不符"，读档拒绝（状态码 9），不崩。
