# AGENTS.md — Rayverse (Android)

## 项目简介

Rayman 1 PC 版的 SDL2 移植（C 语言），当前正在移植到 Android 平台。

- **上游**: https://github.com/pdromnt/rayverse
- **Fork**: pisces312/rayverse
- **本地**: `D:\3rd-party-projects\rayverse`
- **游戏数据**: `E:\games\Rayman1_DosBox4.0\RAYMAN`

## 构建

详见 `docs/build-android.md`

```bash
cd android
gradle assembleDebug
# 产物: android/app/build/outputs/apk/debug/app-debug.apk
```

## 关键架构

### Unity Build

`src/rayverse.c` 是唯一编译单元，通过 `#include` 聚合所有其他 `.c` 文件。不要单独编译子 .c 文件。

### 平台分支

```c
#ifdef _WIN32
  // Windows: GDI + DirectSound
#elif defined(ANDROID)
  // Android: SDL2 + GLES + SAF fd 桥接
#else
  // Linux/macOS: SDL2 + GLEW + desktop GL
#endif
```

### Android fd 桥接

```
Java: SAF 选择目录 → ContentResolver 遍历文件 → openFd() → 注册到 native fd 表
C:    fopen() → android_fopen() → fd 表命中 → fdopen(fd) / 未命中 → 原始 fopen
```

关键文件：
- `android_jni.c` — JNI 层，fd 映射表（MAX_FILES=64）
- `android_fileio.c` — fopen/fclose 拦截
- `src/sysutils.c` — `#ifdef ANDROID` 宏重定向

## 注意事项

- **不要修改游戏核心逻辑**，移植改动仅限 `#ifdef ANDROID` guard
- **SDL2 路径**: `3rd/SDL/`，编译为 shared library
- **GLES 兼容**: 无 GLEW、无 VAO、无 `glDrawBuffer`、无 `GL_BGRA`
- **stb_vorbis**: Android 上用全量编译（非 HEADER_ONLY）
- **Gradle 缓存**: 改 Android.mk 后需删 `app/build` + `app/.cxx` + `.gradle` 重来
- **Gradle KTS 路径**: `import-add-path` 必须用绝对路径，`$(call my-dir)` 有副作用
