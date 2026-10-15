# 2026-10-15 00:50 — 模拟器复现成功 + 跨进程读档崩溃根因坐实（is_loggable 悬垂函数指针）

## 一句话

**在 root 模拟器（pixel6 AVD）上用 release 包复现了跨进程读档崩溃**，拿到完整
tombstone + 槽文件离线比对，根因坐实：`ray_log.h` 的 `ray_log_enabled()` 里
`static is_loggable_fn is_loggable`（dlsym 缓存的函数指针，每个 TU 一份副本，共 3 处）
被整段数据段回滚成**写进程的 liblog 地址**，跨进程 load 后第一次 RAY_LOG 就
`call` 悬垂函数指针 → SIGSEGV。

## 为什么模拟器成了突破口

真机死结：**复现（非 debuggable）与可 ptrace 抓栈（debuggable）互斥**，且任何
代码改动都挪动布局（海森堡）。模拟器绕开两条：

- `pixel6` AVD 是 `google_apis`（`PlayStore.enabled=false`）→ **`adb root` 可用**
  （uid=0），root 的 `debuggerd`/tombstone 读取**不依赖 app 的 debuggable 标志**
  → 直接用"能复现的原始 release"抓栈，不再触发海森堡。
- 复现流程全部 adb 自动化（tap/keyevent 坐标见 `.qoder/skills/rayverse-emulator-test`），
  无需手动操作。

## 复现与证据链（全自动 adb 驱动，约 10 分钟）

1. 装 release（`com.rayverse.rayman`，23:56 构建 = 带 mapped_other 探针版）；
   首启 SAF 授权 RAYMAN 目录（tap 路径：Select folder→RAYMAN→USE THIS
   FOLDER→ALLOW），`Scan complete: 100 found`。
2. Play→Enter×N 进 world1-level1 → gear→即时存档→槽位1
   （`slot 0 written: world=1 level=1 payload=5860K in 10ms`）。
3. `am force-stop` → 重启 → 导航回关卡 → gear→即时读档→槽位1。
4. relocate 正常（`fixed=5903 ambiguous=0`，libmain delta=0x323b000），
   `keep-live scan done` 之后 **SIGSEGV**：

```
Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x774a550a1444
  in tid 4924 (SDLThread)
#00 syscall (libc) ← ndk_translation HandleNoExec（guest 取指到不可执行页）
```

- tombstone memory map：fault addr 落在 **16MB PROT_NONE 保留区**
  `774a54308000-55304fff ---`（读进程里未提交的分配器保留区）。
- fault addr 直接躺在崩溃线程栈上（call 目标）。
- guest 侧解释：游戏 **call** 了 0x774a550a1444 —— 该值是**写进程里
  `__android_log_is_loggable`（liblog）的地址**。

## 槽文件离线比对（决定性）

root 拉槽文件（`exec-out cat`，注意 `adb shell cat` 会 \n→\r\n 污染二进制），
在 6,001,688 B 里搜 `0x774a550a1444`（小端 8 字节），**命中 3 处**，全部落在
seg payload 内，按 `.data` vaddr 0xfce40 换算后 nm 反查：

| 文件偏移 | seg 内偏移 | vaddr | 符号 |
|---|---|---|---|
| 0x417d0 | 0x41548 | 0x13E388 | `ray_log_enabled.is_loggable`（android_fileio.c TU） |
| 0x62bf0 | 0x62968 | 0x15F7A8 | `ray_log_enabled.is_loggable`（紧跟 g_log_ring，android_jni.c TU） |
| 0x62c00 | 0x62978 | 0x15F7B8 | `ray_log_enabled.is_loggable`（android_jni.c 第二份） |

nm 全库扫描确认 **只有这 3 处** dlsym 缓存的函数指针，全部来自 `ray_log_enabled`
（`ray_log.h` 是 header，`static inline` 的 static 变量在每个包含它的 TU 各一份）。

机理：整段 seg memcpy 回滚把 3 份 `is_loggable` 还原成写进程的 liblog 地址
→ keep-live 修复（android_jni.c 7 个静态量）不含它们
→ load 完成后第一次 RAY_LOG 走 `ray_log_enabled()` → `is_loggable(...)` 间接调用
→ 读进程里该地址 = PROT_NONE → SIGSEGV ACCERR（取指）。

## 真机三种表现与本根因的关系

- **模拟器**：落点 PROT_NONE → 干净的 SIGSEGV（本次）。
- **真机**：同一悬垂 call，落点行为随布局而定——可能崩、可能跳进无关可执行
  内存产生不可预测行为（**画面冻结、主循环空转**与此吻合：日志开关函数被调飞后
  引擎状态机行为未定义）。g_log_ring 修复后真机的"冻结"很可能就是本根因的
  另一种落点表现，待修复后真机复验确认。

## 修复（已实施）

`ray_log.h`：弃用 dlsym 缓存，改 **weak 引用**：

```c
__attribute__((weak)) int __android_log_is_loggable(int prio, const char* tag, int def);

static inline int ray_log_enabled(int level, const char* tag) {
    if (!__android_log_is_loggable) return level >= RAY_LOG_DEFAULT;
    return __android_log_is_loggable(level, tag, RAY_LOG_DEFAULT) > 0;
}
```

- weak 未解析时加载期为 NULL（低版本设备），运行时判空 → 与原 fallback 一致；
- **零静态状态** → seg 回滚后无物可悬垂；顺带 `looked_up` 一并消失；
- `dlfcn.h` include 移除（dlsym 是其唯一用途）。

## 验证状态

- [x] 根因证据闭环（tombstone + 槽文件 + nm 三方互证）
- [x] 修复版在模拟器复跑跨进程读档（2026-10-15 00:52）：
  - 修复方案落地踩坑：NDK r28 对 `__android_log_is_loggable` 标 strict
    availability（`__INTRODUCED_IN(30)`），weak 声明与 `__builtin_available`
    两条路都被 unavailable 硬错误拦死。
  - 最终方案：dlsym 缓存收敛为**一个具名全局** `ray_log_is_loggable_fn`
    （定义在 android_jni.c，`ray_log.h` extern），加入
    `android_plat_state_t` keep-live（save/restore 各一行）。nm 确认
    3 份 `ray_log_enabled.is_loggable` static 副本消失，只剩具名全局。
  - 跨进程复跑：`slot 0 read → loaded: world=1 level=1`，
    **crash buffer 0 条**，进程存活，按键后 Rayman 移动/镜头滚动（可玩）。
    修复前同流程 100% SIGSEGV。
  - 注意：修复版 .so 数据段比旧版大 32 B → 旧槽被 region-size 校验拒绝
    （`region 0 size 403856 != local 403824`，预期保护），真机验证需先重存。
- [ ] 真机 S20 复验（装新包 → 重存 → 跨进程读档；重点看"画面冻结"
  是否同根因消失）

## 复用：模拟器跨进程复现流水线（已验证可全自动）

```
emulator -avd pixel6 -no-boot-anim -no-snapshot-save   # 后台任务
adb root                                               # uid=0，tombstone 可读
adb install -r app-release.apk
# 首启：Select folder(1200,195)→RAYMAN(725,1841 portrait)→USE THIS FOLDER(540,2249)→ALLOW(791,1385)
# 每轮：Play(1200,244)→Enter 链进关卡→gear(2190,140)→即时存档(1200,349)/即时读档(1200,515)
#       →槽位行 y=407/573/749→am force-stop→重启→再进关卡→读档
```

release 的 gear 对话框比 debug 多一行"切换到竖屏"，行 y：即时存档 349、
即时读档 515、关闭 955（skill 里的 828 已过时）。槽位行 y=407/573/749。
