# 2026-10-15 00:01 — 探针分析：`mapped_other` 采样也是噪声，region 外悬垂指针假设彻底证伪

> 本轮试验：扩展探针采样 `mapped_other`（指向其他库的指针，relocate 明确不覆盖），
> 验证"relocate 漏修了指向其他 native 库的指针"这个假设。结论：**证伪**。
> 两轮探针合并后，region 外悬垂指针假设彻底排除，根因需重新定向。

## 1. 本轮采样结果（第三次覆盖保存，槽位 3，00:01:14）

各区域 `mapped_other` 计数与采样：

| 区域 | mapped_other 计数 | 采样到 |
|------|------------------|--------|
| data-seg | 3 | 全部 |
| pool0 | 49 | 28 |
| pool1 | 48 | — |
| pool2 | 15 | 15 |
| pool3 | 83 | 64 |
| **合计** | **198** | — |

### data-seg 3 个样本（全局变量区，最该是"真指针"的地方）

```
+0x1dfe0 -> 0x6f0000006b   in <空>                        ← "k\0\0\0o" 数据字节
+0x34160 -> 0x98e80000     in /memfd:jit-zygote-cache     ← 页对齐基址值，数据
+0x34778 -> 0x97b80000     in /memfd:jit-zygote-cache     ← 页对齐基址值，数据
```

### pool0 / pool2 / pool3 样本（全是重复字节填充）

```
0x702a2b2c2c  0x702a2b2c2d  0x7029292a2c  ← ASCII ")*+,)" 重复，数据
0x75757575   0x74747474   0x747575f2     ← 0x75='u' 0x74='t' 填充
0x9e792d01   0x9e752d01   0x9e1a4301     ← jit-cache 里的填充模式
0x7101710000 0x71017a0000 0x7101840000   ← 页对齐"基址值"，非指针
```

**映射目标统计**：`[anon:dalvik-non moving space]` / `/memfd:jit-cache` /
`/memfd:jit-zygote-cache` / `[anon:cfi shadow]` / `boot-*.oat` / `boot-framework.oat`
—— **全部是 Java/ART 层，无一指向 native 库（libSDL2 / libc / libGLES / libm）。**

## 2. 综合结论（heap_other + mapped_other 两轮）

> **快照里没有指向 region 外 native 区域的真指针。**
> - `heap_other`（指向游戏 malloc 的 scudo 堆）：0 个真指针（全 dalvik 堆 = Java 对象地址 = 数据误判）。
> - `mapped_other`（指向其他 native 库）：0 个真指针（全 Java/ART 层地址 = 数据误判）。
>
> 因此 **relocate 的"漏修 region 外指针"不是画面冻结的根因**——快照里根本没有
> 这类指针可漏。relocate 只翻译 region 内指针（fixed=5903），在指针层面是完整的。

## 3. 新线索：relocate hits 与探针计数不一致

对照跨进程 load 那次（进程 27903，`fixed=5903 ambiguous=0`）的 relocate hits
与探针（对齐扫描）的 region 内计数：

| 目标 | relocate hits（字节扫描） | 探针计数（对齐扫描） | 比值 |
|------|-------------------------|---------------------|------|
| seg | 121 | 121 | 1.0× |
| pool0 | 2327 | 2286 | 1.02× |
| pool1 | 708 | 314 | **2.25×** |
| pool2 | 137 | 53 | **2.58×** |
| pool3 | 955 | 627 | 1.52× |
| libmain | 1647 | 1647 | 1.0× |

pool1 / pool2 的 relocate hits 是探针计数的 2.25~2.58 倍。这可能：

- **(a) 良性**：relocate 是字节粒度（byte-granular），能发现 packed struct
  （`obj_t` 155 字节）里**未对齐位置的真指针**，探针对齐扫描只看到 1/8。注释
  （`savestate.c` relocate 头注释）正是这么设计的。
- **(b) 危险**：字节扫描在错位位置读"半个指针 + 半个数据"，若错位值恰好落在
  region 窗口内（窗口 ~1-2MB，`SS_NOISE_FLOOR=0x10000` 挡不住高位地址的错位值），
  relocate 会把它当指针翻译 → **破坏游戏数据**。注释声称"错位读落在窗口 256 倍
  之外"，但该论断对 packed struct 里**相邻的未对齐指针**并不总是成立。

**(a)/(b) 无法从现有日志区分**，这是当前最高优先级要厘清的点。

## 4. 剩余假设（重新排序）

| # | 假设 | 支撑 | 反证 |
|---|------|------|------|
| 1 | **relocate 字节扫描误翻译**（false positive 破坏数据） | 字节扫描无法区分未对齐真指针 vs 错位数据；破坏数据 → 状态机卡住不崩（吻合"世界不推进"） | 注释称碰撞概率低 |
| 2 | **游戏状态有部分不在快照里**（堆/mmap 块未捕获） | 快照只覆盖 seg/pool/flocon/rvb/6 缓冲；堆块内容不入槽 | 探针未见 seg 里指向 scudo 的指针 |
| 3 | **时序竞争**（load 恢复期间其他线程读半恢复状态） | 崩溃线程曾非加载线程 | 主循环空转更像确定性状态错乱 |

## 5. 下一步（不改代码、不触发海森堡）

**方法 A — "冻结后存档"对比**（推荐，立刻可做）：探针代码已在 release 里，且
画面冻结时主循环仍在跑、输入仍响应 → 存档菜单应仍可触发。

1. 进关卡 → 存档到**槽位 A**（探针输出"正常快照"）；
2. 退出 → 重进 → 读槽位 A → 画面冻结；
3. 冻结时**再存档到槽位 B**（探针输出"冻结快照"）；
4. 对比两次探针的 `heap_other / mapped_other / unmapped / pool` 计数与样本，
   找"冻结快照"新增的悬垂指针或异常。

- 若冻结快照出现**新的 region 外指针** → 悬垂（假设 2/3 方向）；
- 若冻结快照 pool 计数**剧变** → relocate 误翻译（假设 1 方向）。

**方法 B — 离线分析 slot 文件**（兜底）：从 S20 拉取 `rayverse_ss_*.bin`，在 PC
上按日志里的 region 布局（`relocate: ... win=... delta=...`）离线重放 relocate，
逐个审视被翻译的值是否"像指针"，坐实或排除误翻译。不改设备代码。

## 6. 环境备注

- 探针常量：`SS_SAMPLE_MAX=64`、`SS_PTR_TAG_MASK=0x00FF_FFFF_FFFF_FFFF`（低 56 位）、
  `SS_NOISE_FLOOR=0x10000`。
- `libmain.so` 未 strip：`android/app/build/intermediates/cxx/Release/3y1c2n11/obj/local/arm64-v8a/libmain.so`。
- `.data` vaddr `0xfce40`（size 0xaeee）、`.bss` vaddr `0x107d30`（size 0x57aa0）。
- 反查工具：`llvm-nm -n` / `llvm-readelf -S`（NDK 28.2.13676358）。
- android 层静态量仅 `android_jni.c` 7 个（已 keep-live）；`android_fileio.c` 无 file-scope 变量。
