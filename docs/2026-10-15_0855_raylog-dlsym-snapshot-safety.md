# ray_log 的 dlsym 缓存与快照安全边界（2026-10-15 08:55）

调研性质：**只读**。本轮没有改任何代码（修复已在 `5eed0ec` 提交），本文只回答三件事：
这个日志机制是什么、能不能去掉、以及「提 minSdk 到 30 用原生 API」划算吗。

## 1. 结论速览

| 问题 | 结论 |
|------|------|
| `ray_log_enabled()` 的 dlsym 是原有代码吗 | **是**。`ae49332`（2026-10-02）引入 `src/ray_log.h` 时就是这个形态，不是本轮调试加的 |
| 去掉它有什么影响 | 性能上**零收益**（见 §5）；能力上会失去「release 现场用 setprop 开日志」，而这正是这次破案的唯一通道 |
| 提 minSdk 到 30 直接调原生 API | 技术上最干净（无状态、走 GOT、天生免疫快照回滚），代价是**丢掉约 13% 设备**（覆盖率 99.8% → 86.9%）。为一条日志不值得，除非本来就要提 |
| 推荐 | **保留机制**。想更干净就走 §4 的 B 方案（weak 符号），比提 minSdk 划算得多 |

## 2. 这个机制到底是什么

`src/ray_log.h` 里 `RAY_LOG(tag, level, ...)` 做三件事：

1. `snprintf` 到 512 字节栈缓冲；
2. `ray_log_ring_capture()` 写进 `g_log_ring`（游戏内「查看调试日志」菜单的数据源）——**不受级别门控，全量记录**；
3. `ray_log_enabled()` 决定要不要 `__android_log_write()` 进 logcat。

第 3 步的存在理由只有一条：`__android_log_is_loggable()` 是 **API 30** 才有的符号，而 `minSdk = 21`（`build.gradle.kts:28`），直接引用会让低版本设备加载 `libmain.so` 失败。所以运行时 `dlsym`，并缓存函数指针避免每条日志查一次。

它换来的能力：release 包默认只打 ERROR，但现场可以

```
adb shell setprop log.tag.Rayverse-SS VERBOSE
```

后重启应用就拿到全部诊断，**不用重新编译**。

### 规模

- 12 个包装宏（`SSLOGI/D/E`、`KDBG`、`LDBG`、`RDBG`、`WDBG`、`RAYGL_LOGI/E`、`LOGI/E`×2 处）
- 73 处调用，其中 `src/savestate.c` 独占 65 处，其余在 `load.c` / `worldmap.c` / `linux_opengl.c` / `android_fileio.c` / `linux_main.c`

## 3. 为什么它成了跨进程崩溃的根因

`ray_log_enabled()` 原本把缓存写成函数内的 `static`：

```c
static inline int ray_log_enabled(int level, const char* tag) {
    static is_loggable_fn is_loggable;   // 每 TU 一份
    ...
}
```

`static inline` 里的 `static` 局部变量**每个包含该头文件的编译单元各有一份**，都在 `libmain.so` 的 `.data` 里（实测 3 份：`0x13E388` / `0x15F7A8` / `0x15F7B8`）。而 savestate 的 region `seg` 正好覆盖整个 `.data`+`.bss`，跨进程读档时被**整段 memcpy 成写档进程的内容** —— 于是这 3 个指针变成上一进程的 liblog 地址，下一次 `RAY_LOG` 就 `call` 了悬垂地址。

- 模拟器（可 root，能取 tombstone）：**100% SIGSEGV**，`SEGV_ACCERR`，guest call 到 16MB `PROT_NONE` 保留区
- 真机：画面冻结（同一悬垂 call 的另一种落点）

修复（`5eed0ec`）：提为具名全局 `ray_log_is_loggable_fn`（定义在 `android_jni.c`），加入 `android_plat_state_t` 的 keep-live。完整推导见 `docs/savestate-cross-process-crash.md` 与 `docs/2026-10-15_0050_emulator-repro-is-loggable-dangling-fnptr.md`。

## 4. 快照安全边界（通用判据，比本 bug 更重要）

release `libmain.so` 的段布局（`llvm-readelf -S`）：

| 段 | vaddr | 大小 | 在快照窗口内 |
|----|-------|------|--------------|
| `.data.rel.ro` | `0xf5280` | `0x458` | 否 |
| `.got` | `0xf5918` | `0x1c38` | 否 |
| `.got.plt` | `0xf7550` | `0x1228` | 否（末尾 `0xf8778`） |
| `.data` ← region 起点 | `0xfc778` | `0xaeee` | **是** |
| `.bss` | `0x107668` | `0x57a80` | **是** |

**关键推论：GOT / GOT.PLT 完全位于 `.data` 之前，不在快照窗口内。** 所以经由 PLT/GOT 调用外部库函数（SDL2、libc、libGLES、liblog…）**天然免疫跨进程回滚** —— 这解释了为什么整个 libmain 到处调外部库都没事，唯独这一个手写缓存崩了。

由此得到今后审计 savestate 的唯一判据：

> 只有**手写在 `.data` / `.bss` 里、指向本进程外部地址**的全局变量有风险。
> 值类型状态、`.rodata` 里的表、PLT/GOT 调用全都安全。

## 5. 性能：不是问题，别为它删

- `ray_log_enabled()` 每次调用 = 一次指针判空 + 一次函数指针调用；`dlsym` 全程只跑一次
- 真正开销在 `RAY_LOG` 宏：`snprintf(512B)` + 进 ring **不受门控**，每条都跑
- 最热调用点 `src/ray.c:664`：按住摇杆时每 6 帧一次 ≈ 10 次/秒；其余 72 处在 save/load、按键、GL 初始化，**不在每帧路径**

真要优化也轮不到它：应当把 gate 提到 `snprintf` 之前（但会牺牲 ring 的全量记录，需要另想办法），而不是删掉整个机制。

## 6. 去掉它的四种方案对比

| 方案 | 做法 | 能力变化 | 快照安全 | 代价 |
|------|------|----------|----------|------|
| **A** 只去 dlsym | 退回编译期 `RAY_LOG_DEFAULT` | **release 现场永远开不了 VERBOSE**（SS 探针、GL、fileio、DBG 全哑） | 安全（无状态） | 自断现场排障：以后只能改码重编，而重编就挪布局、现象消失（海森堡） |
| **B** weak 符号（推荐的可选项） | `-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__=1`，直接调 `__android_log_is_loggable()` 并判空 | 与现在**完全等价** | 安全（走 GOT，不在窗口内） | 需实测 NDK r28 是否放行；此前手写 `__attribute__((weak))` 声明与 `log.h` 的 `__INTRODUCED_IN(30)` 冲突报错，宏方式未必拦 |
| **C** 提 minSdk 到 30 | 直接调原生 API | 等价 | 安全（走 GOT） | **覆盖率 99.8% → 86.9%**，丢约 13% 设备（详见 §7） |
| **D** 彻底删 RAY_LOG | 拆 12 宏 + 73 处调用 + `g_log_ring` + 「查看调试日志」菜单 | savestate 65 处诊断全没 | 安全 | 省下可忽略的 snprintf；未来排障只能重编 |

## 7. 方案 C 单独评估：minSdk 21 → 30

设备覆盖率（StatCounter 2026-04 全球数据，apilevels.com 汇总）：

| minSdk | 版本 | 累计覆盖 |
|--------|------|----------|
| 21 | Android 5.0 | **99.8%** |
| 26 | Android 8.0 | 96.1% |
| 29 | Android 10 | 91.1% |
| **30** | **Android 11** | **86.9%** |
| 33 | Android 13 | 68.9% |

也就是提一级到 30 要**放弃约 13% 的在用设备**，且被排除的是 2014–2020 年间出厂的老机器——恰好是「拿旧手机当游戏机」这类玩家的典型机型。

其他连带影响：

- **纯收益**：`__android_log_is_loggable()` 可直接调用，dlsym 与缓存全部消失；`android_jni.c` 的 keep-live 字段也能删；同理可简化其他 API 分级兼容代码
- **无运行时行为变化**：minSdk 只影响安装门槛与编译期 API 可用性，不改动系统对你的行为（行为由 targetSdk 决定，当前已是 36）
- **不可逆**：一旦提上去，Play/侧载用户对老设备的支持就断了，回头要改回来

**结论**：为一条日志的缓存提 minSdk 是**用 13% 的用户换 20 行代码**——不划算。
只有当项目本来就要提 minSdk（比如需要 API 30 的存储/音频能力）时，才顺手把 dlsym 删掉。
在那之前，**B（weak 符号）是同效果、零覆盖率代价的替代**。

## 8. 待办

- [ ] 真机 S20 装 `5eed0ec` 之后的包复验：注意修复版 `.so` 数据段 +32B，旧槽会被 `region size 403856 != local 403824` 校验拒绝（预期保护），**必须先重存一档**再跨进程读档
- [ ] 若要走 B 方案：实测试编译 `-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__=1`，通过则替换并删除 keep-live 字段；不通过则维持现状
