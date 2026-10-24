# Rayverse
Work-in-progress modern port of Rayman 1 for PC (version 1.21), based on the disassembly of the original.

The aim is to provide a drop-in replacement for the original executable RAYMAN.EXE that works on modern platforms (including Windows, Linux and macOS).

## Android port (this fork)

This fork adds an **Android port** on top of the upstream desktop platforms: the same engine builds into an APK that renders through SDL2 and OpenGL ES. Android platform integration sits behind `#ifdef ANDROID` guards or inside the `android/` Gradle project; the fork also contains architecture and data-loading fixes documented in `docs/android-debug-log.md`.

The latest build is [v1.0.4](https://github.com/pisces312/rayverse/releases/latest), a signed arm64 APK (~1.9 MB). It ships the engine only: you still need the original Rayman 1 PC release, and the app reads its data folder directly from device storage.

What the port adds:

* **SDL2 + GLES backend** replacing the Win32 GDI/DirectSound paths, with audio through SDL2 and `stb_vorbis` decoding the optional `Music/*.ogg` CD rips.
* **SAF file bridge** — the game folder is picked once via the Storage Access Framework and handed to native code as a table of file descriptors behind an `fopen()` interception layer, so the engine can read `RAYMAN.EXE`, `PCMAP/ALLFIX.DAT`, `*.WLD` and `*.LEV` without legacy runtime storage permissions.
* **Virtual gamepad** with a draggable, persisted layout. Its buttons map onto the original DOS scancodes (jump / punch / grab / move), which is why there is no run key — running comes from power-ups and trigger zones, as in the original.
* **Persistent save games and three instant-savestate slots**, with export/import through SAF for backups and problem-sample collection. Instant states can be saved or loaded only during level gameplay; cross-build restore remains only partially supported.
* **Bluetooth gamepad mapping and an in-app debug-log viewer**, alongside the draggable virtual gamepad.
* Landscape lock, letterboxed to the original 320:200 aspect, and a home menu (`Play` / `Export saves` / `Import saves` / `Change data folder`).

Status: verified playable on a real device (Samsung Galaxy S20, Android 13, arm64) — intro, title menu, world map, level start, save/load and quit all work. Later levels have not been play-tested one by one, so expect progress to follow the original game. arm64 only; x86_64 emulators and 32-bit devices are not built.

### Screenshots

Captured on the device, with the game letterboxed at its original 320:200 aspect:

| Title menu | World map | In-game |
|:---:|:---:|:---:|
| <img src="https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-2-title-menu.jpg" alt="Rayverse Android title menu" width="280"> | <img src="https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-4-world-map.jpg" alt="Rayverse Android world map" width="280"> | <img src="https://github.com/pisces312/rayverse/releases/download/v1.0.3/screenshot-5-ingame.jpg" alt="Rayverse Android in-game" width="280"> |

*Rayman is a trademark of Ubisoft. These screenshots show the original game data running under this port; this repository and its APK distribute no game assets — you have to provide your own copy of Rayman 1 for PC.*

### Building for Android

Requires the Android SDK (API 36), NDK **29.0.14206865**, system Gradle **9.4.1**, and the SDL2 sources under `3rd/SDL/`. The repository has no runnable Gradle wrapper; invoke `gradle`, not `./gradlew`:

```
git clone --recursive https://github.com/pisces312/rayverse.git
cd rayverse/android
gradle assembleDebug    # app/build/outputs/apk/debug/app-debug.apk
gradle assembleRelease  # signed APK; signing config comes from KEY_STORE /
                        # KEY_STORE_PASSWORD / KEY_ALIAS / KEY_PASSWORD env vars
```

Debug and release installs can coexist: debug is `com.rayverse.rayman.debug` (green icon), release is `com.rayverse.rayman` (pink icon).

### Android documentation

* [`docs/build-android.md`](docs/build-android.md) — toolchain versions, release signing, Gradle/NDK gotchas
* [`docs/controls-android.md`](docs/controls-android.md) — the input path from virtual gamepad to the DOS scancode table
* [`docs/android-save-export-import.md`](docs/android-save-export-import.md) — save storage layout and the export/import flow
* [`docs/savestate-phase2.md`](docs/savestate-phase2.md) — current instant-savestate format, relocation model and compatibility limits
* [`docs/android-debug-log.md`](docs/android-debug-log.md) — real-device debugging notes (logcat capture, AArch64 pitfalls, ...)
* [`docs/release-v1.0.4.md`](docs/release-v1.0.4.md) — v1.0.4 release notes and installation instructions

## Build instructions

### Windows
For modern Windows platforms, you can build Rayverse using CMake in combination with your preferred toolchain, either MinGW-w64 or MSVC (e.g. using Visual Studio).

To build with Windows 9x compatibility, you can use Visual C++ 6.0 with the project file `rayverse.dsp`.

### Linux / macOS
The following dependencies are required to be installed: SDL2 (on Linux and macOS), GLEW (Linux only).
```
mkdir build && cd build
cmake .. && cd ..
cmake --build build
./rayverse
```

## Special thanks

Main collaborators on this project:
* **[RayCarrot](https://github.com/RayCarrot)**: many insights on the inner workings of the game; author of the [BinarySerializer](https://github.com/BinarySerializer/BinarySerializer.Ray1),
  [Rayman Control Panel](https://github.com/RayCarrot/RayCarrot.RCP.Metro), [Ray1Map](https://github.com/BinarySerializer/Ray1Map) 
  and [Ray1Editor](https://github.com/RayCarrot/RayCarrot.Ray1Editor) projects, among others.
* **[fuerchter](https://github.com/fuerchter)**: author of the matching decompilation of Rayman for PS1 project, see [rayman-ps1-decomp](https://github.com/fuerchter/rayman-ps1-decomp)
