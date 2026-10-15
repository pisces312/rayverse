# release 包验证：weak `__android_log_is_loggable` 方案（2026-10-15 10:47）

## 结论

`raylog-weak-is-loggable` 分支在 **release 包**上通过跨进程读档全链路验证：
dlsym 缓存删除后，日志能力未退化，崩溃消失。

| 判据 | 结果 |
|---|---|
| `assembleRelease`（NDK 29） | 成功，APK 1,955,364 B |
| `llvm-nm libmain.so \| grep is_loggable` | `w __android_log_is_loggable`（weak undefined，无本地副本） |
| release 上现场 `setprop log.tag.Rayverse-SS VERBOSE` | **仍可开** —— 保留下来的核心能力 |
| 存档 | `slot 1 written: world=1 level=1 payload=5860K in 6ms` |
| 跨进程读档 | `loaded: world=1 level=1` |
| crash 缓冲区 | 空 |
| 进程存活 / 画面推进 | 是（按右键后蝴蝶、音符在动） |

## 为什么必须单独验 release

debug（-O0）从不复现这个 bug，而 release 是唯一能复现的环境。
所以「debug 通过」不等于修好，必须有一个 release 包跑完整闭环。

同时 release 也是唯一需要确认「日志通道没被自己改坏」的地方：
weak 引用在 `-O2` + 不同链接顺序下是否仍解析为 weak undefined，
不实测不能推定。

## 验证步骤（模拟器 pixel6，root，2400x1080 横屏）

```bash
cd android && gradle :app:assembleRelease
adb -s emulator-5554 install -r app/build/outputs/apk/release/app-release.apk

# 现场开日志：release 默认只有 ERROR，靠 setprop 打开
for T in Rayverse-SS Rayverse-IO Rayverse-DBG Rayverse-JNI; do
  adb -s emulator-5554 shell "setprop log.tag.$T VERBOSE"
done
adb -s emulator-5554 shell am force-stop com.rayverse.rayman
adb -s emulator-5554 shell monkey -p com.rayverse.rayman -c android.intent.category.LAUNCHER 1
```

流程：Play `(1200,252)` → 选档 → 长按 OK `(1842,943)` 2.5 s 进关卡 →
gear `(2195,142)` → 即时存档 `(650,351)` → 槽位 2 `(638,585)` →
`am force-stop` → 重启 → 回关卡 → gear → 即时读档 `(650,522)` → 槽位 2 `(638,585)`。

注意：release 与 debug 的 gear 对话框坐标一致，UI 是 1-based 而日志是 0-based
（点"槽位2"打出 `slot=1`）。

## 读档日志（release，setprop 已开）

```
load step: backups done (app=1088 ss=544 seg_slots=50478)
load step: keep-live scan done
load step: segment restored
load step: platform state restored
load: kept 1647 code/rodata references live
load step: pools/flocon/rvb restored
load step: SetPalette done
load step: audio fixup start live: bnkF=0x774d5b91b210 bnkW=0x774d60cb5160 ...
load step: LoadBnkWorld(1) begin
load step: LoadBnkWorld end
load step: stop_all_snd #2 done
loaded: world=1 level=1
```

`kept 1647 code/rodata references live` 是 relocate 的常规统计，
不是 keep-live 字段数（后者只有 6 个平台静态量）。

## 这轮改动为什么能根治

原实现把 `__android_log_is_loggable` 的指针缓存成 `static`，
每 TU 一份，落在被快照的 `.data` 里；跨进程读档把它回滚成写档进程的地址，
下一次 `RAY_LOG` 就调用悬垂指针。

weak 方案下符号在 **GOT** 里，而 GOT 位于 `.data` 之前（快照窗口外），
且**没有任何进程内指针状态**需要保存恢复 —— 不是"兜住了"，是"不存在了"。

## 未完成

- S20 真机复验：无线 adb 不在线（`adb devices` 只剩模拟器，mdns 扫不到）。
  签名环境变量齐备，手机一连上即可直接 `install -r`。
  旧档会被 `region size X != local Y` 拒绝（预期保护），需**重存一档**再跨进程读档。
- 复验通过后把 `raylog-weak-is-loggable` 合并回 `main`。

## 相关

- `docs/2026-10-15_0855_raylog-dlsym-snapshot-safety.md` —— 方案对比与段序判据
- `docs/2026-10-15_0050_emulator-repro-is-loggable-dangling-fnptr.md` —— 根因定位
- `agents.md` 「Native 代码修改与存档兼容性（必读）」（本地文件，被 .gitignore 忽略）
