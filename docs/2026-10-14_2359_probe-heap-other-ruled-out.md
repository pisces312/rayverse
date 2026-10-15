# 2026-10-14 23:59 — 探针分析：`heap_other` 假设证伪，嫌疑收窄到 `mapped_other`

> 本轮试验：用指针分类探针分析**保存时**快照内容，验证"relocate 遗漏了指向
> native 堆的指针"这个假设。结论：**证伪**。剩余嫌疑收窄到"指向其他库的指针"
> （`mapped_other`），已扩展探针待采样。

## 1. 为什么用探针（绕开海森堡 bug）

前几轮规律已经非常硬：唯一复现"跨进程读档画面冻结"的是**原始 release**；任何为
观测而做的改动（-O0 / isDebuggable / 加 backtrace 采样代码）都会挪动 `.so` / `.bss` /
堆布局，让悬垂指针落点漂走，现象随之消失。运行时加代码抓现场这条路已到死路。

探针是现成的出口，关键在于它**绕开海森堡**：

- 探针代码**已编译进 release**（无 `#ifdef` 排除），只是运行时靠日志级别开关决定
  是否执行，不改变 `.so` 布局。
- `setprop log.tag.Rayverse-SS VERBOSE` 只改日志过滤，不动代码、不动布局。
- 探针分析的是**保存时**快照内容（`seg_copy` 里每个指针指向哪里），是**确定性事实**，
  与 load 时的悬垂落点无关。

触发点：`ss_do_save()` 末尾 `if (ray_log_enabled(DEBUG, SS_TAG)) ss_run_probe("capture")`。

## 2. 第一轮：样本上限 6，只看到噪声

- `SS_SAMPLE_MAX=6`，data-seg 报告 `heap_other=32`，但只采样前 6 个。
- nm 反查：前 6 个 `off` 全落在 `block_types_img_data`（`0xfc9ec`，图像数据数组），
  值 `0x25/0x26/0x27` 是填充字节 —— 探针按 8 字节扫描把图像数据误判成指针的噪声。
- 真正的悬垂指针可能藏在 32 个的后 26 个里，被上限挡住看不到。
- 处理：`SS_SAMPLE_MAX` 6 → 64（只改栈上结构大小，不碰 `.bss/.data` 布局，不触发海森堡）。

## 3. 第二轮：完整 32 个 `heap_other` 样本

对全部 32 个样本用 `llvm-nm -n` 反查偏移：

| 样本 | 落点 | 判定 |
|------|------|------|
| 0–29 | `block_types_img_data` 图像数组 | 填充字节 `0x25/0x26/0x27/0x66`，噪声 |
| 31 | 调试缓冲区残留值 | 精确等于 module 末尾地址，噪声 |

**关键判定来自 257 个 `heap_other` 采样的目标映射统计**（指向"匿名 rw 映射"的指针）：

```
189  dalvik-main space
 52  dalvik-free list
  7  dalvik-zygote
  5  boot-framework.art
  2  boot-bouncycastle.vdex
  1  boot-okhttp.vdex
  1  [anon:.bss]
```

**没有任何一个指向 native 堆 scudo（游戏自己 malloc 的独立块）。**

## 4. 结论（本轮确定性结果）

> **"relocate 遗漏了指向 native 堆的指针"这个假设被证伪。**
> 快照里没有指向游戏自己 malloc 的独立块的指针，因此不存在"跨进程后指向旧进程
> heap 悬垂"的问题。data-seg 那 32 个 `heap_other` 里没有一个是真悬垂指针。

## 5. 剩余嫌疑：`mapped_other`

relocate / keep-live 明确不覆盖"指向**其他库**（libSDL2 / libc / libGLES 等）的指针"，
探针分类为 `mapped_other`（已知映射、非本 module 段、非 region 表内）。计数：

| 区域 | mapped_other 计数 |
|------|------------------|
| data-seg | 2 |
| pool0 | 49 |
| pool1 | 48 |
| pool2 | 15 |
| pool3 | 83 |
| **合计** | **≈197** |

原探针**只计数不采样**，看不到具体值。这是 relocate 的明确缺口，也是下一步要定位的。

## 6. 下一步（已部署，待采样）

- 扩展探针采样 `mapped_other`：在 `ss_probe_t` 里加 `s_mapped_other[SS_SAMPLE_MAX]` +
  `n_mapped_other`，在分类分支里记录 `+0x<off> -> 0x<value> in <libpath>`，打印时逐条输出。
- 只改栈上局部结构 + 打印，**不动 `.bss/.data` 布局，不触发海森堡**。
- 已构建 release 装回 S20（升级安装、槽保留），采集 `logcat-s20-probe2.txt`。
- **待操作**：进复现关卡 → 存档 → 从日志取 `mapped_other sample ... in <libpath>`，
  用 `off` 反查是哪个全局变量指向了别的库。

## 7. 环境备注

- `libmain.so` 未 strip 路径：`android/app/build/intermediates/cxx/Release/3y1c2n11/obj/local/arm64-v8a/libmain.so`。
- `.data` vaddr = `0xfc9e0`（seg 段基准），`.bss` 紧邻其后。
- 反查工具：`llvm-nm -n` / `llvm-readelf -S`（NDK 28.2.13676358，arm64-v8a）。
- 探针样本上限现为 `SS_SAMPLE_MAX=64`。
