# 存档导出/导入与跨设备共享 — 方案 A

> 状态：**已实现（E1 首页菜单）**，待真机验证。debug/release 共存（`applicationIdSuffix=".debug"`）
> 与 debug 专属图标已单独实现，见文末「关联改动」。

## 1. 现状（事实）

- 存档写在 App 私有内部存储：`context.getFilesDir()/rayverse_save/`
  （设备上即 `/data/data/com.rayverse.rayman/files/rayverse_save/`）。
  来源：`GameDataBridge.registerWithNative()` → `nativeSetSaveDir(...)`，
  引擎侧经 `android_fopen`/`build_save_path` 写入。
- 存档文件集合（见 `src/save.c`）：
  - `RAYMAN1.SAV` / `RAYMAN2.SAV` / `RAYMAN3.SAV`（三个存档槽）
  - `RAYMAN<n>_ALL_WORLD.SAV`（世界地图进度，`sav_set_ALL_WORLD`）
  - `RAYMAN.CFG`（配置）
- 该目录**用户不可见**；release 包非 debuggable，无 root 也无法 `run-as` 取出。
- manifest 里 `android:allowBackup="false"`，**没有系统自动备份**。
- 目前**没有任何导出/导入入口**。
- `SetupActivity` 是 launcher，且**数据有效时 `onCreate` 直接 `launchGame()` 自动进游戏**，
  正常情况下用户看不到设置页 —— 这决定了入口 UX（见 §4）。
- `androidx.documentfile:1.1.0` 已是依赖，SAF 读写可直接用 `DocumentFile`。

## 2. 目标

1. 用户能在**不 root、release 包也可用**的前提下，把存档导出到共享存储/云盘目录。
2. 能从外部目录导入存档，覆盖到私有存档目录。
3. 支持在不同手机间迁移/共享存档（含云盘同步目录）。
4. 保持引擎与 native 代码零改动（存档已经落在 `filesDir/rayverse_save`，只需在 Java 层搬运）。

## 3. 方案 A 设计（SAF 显式导出/导入）

存档的**运行时位置不变**（仍在私有 `filesDir/rayverse_save`，快、始终可写）。
导出/导入只是把这个目录里的存档文件与用户选定的 SAF 目录之间**双向复制**。

### 3.1 导出

- 触发 `Intent.ACTION_OPEN_DOCUMENT_TREE`（选一个**目标文件夹**，一次授权即可写多个文件）。
- 用 `DocumentFile.fromTreeUri(ctx, treeUri)` 遍历私有存档目录里的
  `RAYMAN*.SAV` / `RAYMAN*.CFG`，逐个写入目标文件夹：
  - `dest.findFile(name)` 命中 → `openOutputStream()`（"w" 截断覆盖）；
  - 未命中 → `dest.createFile("application/octet-stream", name)` 再写。
- 状态栏反馈导出文件数与失败项。
- **可选增强**：打成单个 `.zip` 再用 `ACTION_CREATE_DOCUMENT` 导出（便于聊天/邮件分享），
  列为后续项，不在首版。

### 3.2 导入

- 触发 `Intent.ACTION_OPEN_DOCUMENT_TREE`（选一个**源文件夹**）。
- 遍历该文件夹，只挑文件名匹配 `RAYMAN*.SAV` / `RAYMAN*.CFG` 的项，
  复制进 `filesDir/rayverse_save`（`openOutputStream` 截断覆盖）。
- **导入前弹确认框**（会覆盖现有进度，属破坏性操作）。
- 状态栏反馈导入文件数。

### 3.3 跨设备共享

- 存档是纯游戏状态文件，**与设备无关**，只要两端引擎版本兼容即可互用。
- 推荐路径：导出到一个**被同步的目录**（Syncthing / Drive / Dropbox / Nextcloud），
  另一台手机同一目录里即出现存档 → 导入。
- 或任意文件传输（USB / 聊天 / 邮件）后再导入。
- 兼容性告警：`.SAV` 结构绑定引擎版本，跨 rayverse 大版本的存档可能读不出；
  导入时若引擎加载失败，保持「读不出就当无存档」的现有行为，不崩。

## 4. 入口 UX（已定：E1 首页菜单）

**存档读取时机（实测代码，回答"点 Start 还是启动时读"）**：两者都不是进程启动时读。
- 存档槽**信息**在进入"存档选择界面"时读：`worldmap.c:638 INIT_SAVE_CHOICE()` → 循环 `LoadInfoGame(i+1)`（save.c:654）。
- 完整存档在玩家选中槽位并确认 Start 时读：`worldmap.c:869 LoadGameOnDisk(...)`（save.c:606）。
即读取发生在**游戏内的存档选择/Start 环节**，进程 boot 到标题时并不读盘。

**为什么仍然选 E1（进游戏前的首页菜单），而不是游戏内右上角设置按钮**：
- **导入必须在引擎读取/缓存槽信息之前完成**。若在游戏内（已到存档选择界面）导入，`LoadInfoRay` 可能已缓存旧值，需重进存档选择界面才生效；E1 在 boot 前写盘，引擎随后读到的是新文件，语义最干净。
- 游戏内设置按钮是一个**overlay**，从运行中的 SDL 游戏里发起 SAF `ACTION_OPEN_DOCUMENT_TREE`（Activity for result）需要暂停游戏循环、处理 GL 上下文恢复，侵入性大；该按钮当前职责是屏幕方向/手柄编辑，与文件选择器不是同一层。
- E1 让导出/导入对称、可发现，且是普通 Android Activity，实现最简单、最稳。
- 代价：启动多一次点击（首页点 Play）。

结论：**E1**。首页菜单 = `Play / Export saves / Import saves / Change folder`。
（游戏内设置按钮保持只负责屏幕方向/手柄编辑，不放文件选择器。）

## 5. 代码触点

- `GameDataBridge.java`（新增，纯 Java）：
  - `File getSaveDir()`（复用现有 `filesDir/rayverse_save` 逻辑，去重两处硬编码）
  - `List<String> listSaveFiles()`
  - `int exportSavesTo(Uri treeUri)` / `int importSavesFrom(Uri treeUri)`（返回成功文件数）
- `SetupActivity.java`（或新的 `SaveManagerActivity`，取决于 §4）：
  - 菜单/按钮、两个 `ACTION_OPEN_DOCUMENT_TREE` 请求码、`onActivityResult` 处理、
    导入确认框、状态反馈。
- `res/values/strings.xml`：新增按钮/对话框文案（当前无 strings.xml，需新建）。
- **native / 引擎：无改动。**
- 权限：`OPEN_DOCUMENT_TREE` 已含读写授权；是否 `takePersistableUriPermission`
  记住导出/导入目录为可选。

## 6. 边界与风险

- 只搬运匹配 `RAYMAN*.SAV` / `RAYMAN*.CFG` 的文件，避免把无关文件带进存档目录。
- 必须包含 `*_ALL_WORLD.SAV`，否则世界地图进度会缺。
- 导入是破坏性覆盖 → 需二次确认。
- 无「关卡中途自动存档」：进度只在新游戏 / 世界地图存档点写盘，
  导出的存档反映的是最近一次存档点，与游戏内一致（非本方案引入）。
- 分区存储下不能直接写任意共享路径，SAF 是正确机制（本方案即基于 SAF）。

## 7. 测试计划（真机，用户执行游戏内验证）

1. 装 debug 包，玩到某存档点，确认 `filesDir/rayverse_save` 有 `.SAV`（debug 可 `run-as` 查）。
2. 导出到某目录 → 在文件管理器里确认 `RAYMAN*.SAV`/`.CFG` 全部落地。
3. 清空/重装 App（或换第二台机）→ 导入刚才的目录 → 进游戏确认进度正确加载。
4. 跨设备：导出到 Syncthing/云盘目录 → 另一台导入 → 验证。
5. release 包重复 2–3（验证免 root、release 可用）。

## 8. 里程碑

1. `GameDataBridge` 存档枚举 + 导出/导入辅助（纯 Java）。
2. 定 §4 入口 UX，接线按钮与 `onActivityResult`。
3. 导入确认框 + 状态反馈 + strings。
4. 真机联调（§7）。
5. 文档：更新 `docs/controls-android.md` 或新增用户指南小节；release notes 提及。

## 关联改动（已完成）

- debug/release 共存：`android/app/build.gradle.kts` 给 debug 加
  `applicationIdSuffix = ".debug"`，debug 包名变为 `com.rayverse.rayman.debug`，
  与 release（`com.rayverse.rayman`）并排安装、各自独立存档目录与 SAF 授权。
- 标签区分：build-type 源集提供 `app_name`
  （`src/debug/res/values/strings.xml` = "Rayverse (debug)"，
  `src/release/res/values/strings.xml` = "Rayverse"），manifest 用 `@string/app_name`。
  （`resValue` 在 buildType 里被 AGP 禁用，故改用源集。）
- 图标区分：`src/debug/res/mipmap-*/ic_launcher.png` 为绿色主色版
  （把 release 的粉色 `(233,69,96)` 外环+R 换成绿色 `(0,200,83)`），
  与 release 粉色图标一眼可分。
