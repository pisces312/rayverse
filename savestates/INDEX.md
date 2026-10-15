# savestates/ — 真机即时存档样本库

管理读档问题调试用的槽文件样本（`rayverse_ss_*.bin`）：记录每个样本的
写档出处（构建 / commit / BuildId）、复现状态与关联调研文档。

## 命名规则

```
YYYY-MM-DD_slot<N>_w<世界>-<关卡>_<状态>.bin
```

- 日期取槽文件头 `created`（写档时间），不是拉取日期；
- `<N>` 是**游戏内 UI 槽号 1..3**；设备上文件名是 `rayverse_ss_<N-1>.bin`（0 起）；
- 状态：`repro-<关键词>` = 可稳定复现某问题；`ok` = 标注时的构建读档正常；
- 同一槽位的多个样本在状态后追加 `_2`、`_3` 区分。

## 管理约定

- 样本从真机导出：首页菜单 → **Export saves**（2026-10-15 起导出范围含
  `rayverse_ss_*.bin`，见 `docs/android-save-export-import.md` §1），再从
  共享目录 adb pull 回来。
- 每入库一个样本必须在下表登记：写档构建（commit + 是否含未提交改动 +
  BuildId，BuildId 用 `llvm-readelf -n libmain.so` 查）、复现状态、关联文档。
- 已修复问题的 repro 样本**保留用于回归**：把样本 Import 回真机（Import 会
  覆盖同名槽位，先导出现状）验证修复；stamp 变化导致旧样本被拒读属预期
  保护，在备注里记一笔即可。
- 头部字段用 `python tools/dump_savestate_hdr.py <file...>` 解析
  （world / level / created / stamp / payload 大小）。内部指针指纹的解剖方法
  见 `docs/2026-10-15_1611_tombstone21-bande-drawbufferpos-dangling.md` §8
  （seg 内符号偏移 = 符号 vaddr − 可写 LOAD 的 p_vaddr `0xfd160`，payload
  从文件偏移 648 开始）。

## 样本索引

| 文件（UI 槽 / 设备文件名） | 关卡 | 写档构建 | 状态 | 说明 |
|---|---|---|---|---|
| `2026-10-15_slot1_w2-1_repro-crossbuild-drawbuf.bin`（UI 1 / `rayverse_ss_0.bin`） | 2-1 | **跨构建**：写于 12:37:58，早于 v1.0.4 构建（12:41），确切 commit 未知；其 draw buffer 地址 `0xb40000752d0dc0e0` 与 `f71f2f03`/`0b60c535` 两个构建的确定性地址都不同 | **repro：跨进程读档必崩** | 内容自洽的健康快照，但 draw-buffer 指针家族全部指向写档进程的裸 calloc 地址。`f71f2f03` 读它三次崩 blit（tombstone_18/19/20），`0b60c535`（含 15:22 修复）崩 `Display_Back_Screen→Copy_Plan0Diff_To_Buf` 写端（tombstone_21）。**SS_RGN_DRAWBUF 修复后应拿它做跨构建回归**。详见 `docs/2026-10-15_1611_...` |
| `2026-10-15_slot2_w2-1_ok.bin`（UI 2 / `rayverse_ss_1.bin`） | 2-1 | `0b60c535…` = `d26be0b` + 未提交的 15:22 swap-buffer 修复，写于 16:00:02 | ok：多次杀进程重启读档正常 | 同构建槽。与 UI 1 同关卡、同测试流程，对照证明"新档不崩"是**同构建地址确定性**，不是流程差异 |
| `2026-10-15_slot3_w2-1_ok.bin`（UI 3 / `rayverse_ss_2.bin`） | 2-1 | 同上，写于 16:01:02 | ok：同上 | 同上 |

### 内部指纹（seg 内偏移按 `0b60c535` 符号表解析，三档布局一致）

| 样本 | draw/DrawBufferNormal/Ini | PLAN0BIT | Bande[0] src / dst |
|---|---|---|---|
| UI 1 | `0xb40000752d0dc0e0`（= tombstone fault addr） | `0xb40000732a4f07f4` | = PLAN0BIT / = draw buffer |
| UI 2 | `0xb400006edfd15120` | `0xb400006c7e3787f4` | = PLAN0BIT / = draw buffer |
| UI 3 | `0xb400006edfd15120` | `0xb400006c7d7787f4` | = PLAN0BIT / = draw buffer |

三档 `Bande[0].length=288`、`num_world=2 num_level=1`，四个指针槽互洽
（无错位/损坏）。UI 2/3 的 draw buffer 地址相同（同进程写的两档；跨进程
读回成功证明该地址在同构建各进程间确定性复用）。
