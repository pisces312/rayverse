# Rayverse Android 构建指南

## 环境要求

| 工具 | 版本 |
|------|------|
| Android SDK | API 36 |
| NDK | 28.2.13676358 |
| AGP | 9.2.0 |
| Gradle | 9.4.1 (Wrapper) |
| JDK | AndroidStudio JBR |
| SDL2 | 2.32.10（`3rd/SDL/`，源码编译） |

## 构建步骤

```bash
# 1. 克隆（含子模块）
git clone --recursive <repo-url>
cd rayverse

# 2. 准备 SDL2
# SDL2 源码放在 3rd/SDL/（.gitmodules 已配置）

# 3. 构建
cd android
gradle assembleDebug

# 4. 产物
# android/app/build/outputs/apk/debug/app-debug.apk (~4.35 MB)
```

## 项目结构（Android 部分）

```
android/
├── app/src/main/
│   ├── cpp/
│   │   ├── Android.mk          # ndk-build 脚本（SDL2 shared lib + rayverse unity build）
│   │   ├── Application.mk      # arm64-v8a, c++_static, android-21
│   │   ├── android_jni.c       # JNI：SAF fd 管理、游戏启动、环境初始化
│   │   └── android_fileio.c    # fopen/fclose 拦截 → fd 表查找 → fdopen
│   ├── java/com/rayverse/rayman/
│   │   ├── RayverseActivity.java   # 继承 SDLActivity，SAF 目录选择
│   │   ├── GameDataBridge.java     # ContentResolver 遍历文件并注册 fd
│   │   └── GamepadOverlay.java     # 虚拟手柄触控 overlay
│   └── AndroidManifest.xml
├── build.gradle.kts
└── settings.gradle.kts
```

## 移植方案

APK 不含游戏数据。用户首次启动时通过 SAF 选择手机存储中的 Rayman 游戏目录（含 `RAYMAN.EXE` + `PCMAP/`），Java 层为每个数据文件打开 fd 传给 native 层，C 层的 `fopen()` 被重定向到 fd 表。

## 源码修改

| 文件 | 改动说明 |
|------|----------|
| `src/rayverse.c` | `#ifdef ANDROID` 分支：GLES 头文件 + stb_vorbis 全量编译 |
| `src/linux_opengl.c` | 跳过 GLEW、VAO、`glDrawBuffer`、`GL_BGRA`→`GL_RGBA` |
| `src/linux_main.c` | 跳过 `glDrawBuffer` |
| `src/sysutils.c` | `ANDROID` 宏重定向 `fopen`/`fclose` 到 fd 桥接层 |

## 运行要求

- Android 5.0+（API 21）
- ARM64 设备
- 游戏数据：将 `RAYMAN/` 目录（含 `RAYMAN.EXE`、`PCMAP/*.DAT`、`*.WLD`、`*.LEV`）放到手机存储
- 可选：`INTRO.DAT`、`CONCLU.DAT`、`Music/*.ogg`（CD 音轨）
