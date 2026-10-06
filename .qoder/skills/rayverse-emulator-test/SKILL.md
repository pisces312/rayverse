---
name: rayverse-emulator-test
description: Fast end-to-end verification of the Rayverse Android port (Rayman 1 SDL2 C engine) on the local pixel6 emulator: build/install the debug APK, push game data, drive menus with adb keyevents, capture Rayverse-* logcat evidence, and take screenshots. Use when real-device is unavailable and a port feature (savestate, menus, input, file IO) must be re-checked on the emulator, or when a fix must be verified with runtime logs/images instead of code reading.
---

# Rayverse emulator quick-verify

## Environment facts (do not re-discover)

- AVD **`pixel6`** (API 34, x86_64 image with built-in ARM translation, `abilist=x86_64,arm64-v8a`). The arm64-only debug APK installs directly — **never build an x86_64/universal variant just for the emulator**.
- Display 1080x2400 @ 420 dpi; the game locks landscape, so device coords are x≤2400, y≤1080.
- Emulator must run as a **managed background task**: `emulator.exe -avd pixel6 -no-boot-anim -no-snapshot-save` (from `D:\dev\android_sdk\emulator\`). Then `adb wait-for-device shell getprop sys.boot_completed` until `1`.
- Package: `com.rayverse.rayman.debug` (debug suffix; coexists with release). Launch: `adb shell monkey -p com.rayverse.rayman.debug -c android.intent.category.LAUNCHER 1`.
- Every adb command with a **device path** needs `MSYS_NO_PATHCONV=1` (Git Bash rewrites `/sdcard/...`).
- Under ndk_translation, screen transitions (title→slot screen→level) take 30–60 s. Poll with screenshots; do not assume a hang.

## Workflow

### 1. Build & install

```bash
cd android && gradle assembleDebug
# apk: android/app/build/outputs/apk/debug/app-debug.apk
find app/build -name "*.so" -newer src/common.h   # proves native sources were rebuilt
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

A 3–5 s `BUILD SUCCESSFUL` is usually a real incremental build — trust the mtime check, not the clock.

### 2. Game data on the emulator

Check first: `MSYS_NO_PATHCONV=1 adb shell ls /sdcard/RAYMAN | head` (must contain `PCMAP`). If missing:

```bash
cd "/d/games/Rayman1_DosBox4.0" && tar -cf /tmp/rayman.tar RAYMAN
MSYS_NO_PATHCONV=1 adb push "$(cygpath -w /tmp/rayman.tar)" /sdcard/rayman.tar
MSYS_NO_PATHCONV=1 adb shell "cd /sdcard && tar -xf /sdcard/rayman.tar"
```

Never `adb push` a directory (bad_alloc crash). First run (or after reinstall) needs the SAF folder grant: tap **Change folder** → in DocumentsUI `uiautomator dump /sdcard/ui.xml`, read `bounds` of `android:id/button1` ("使用此文件夹"), tap its center, then tap "允许" in the follow-up dialog. `GameDataBridge` logs `Game data OK (100 files)` when the fd table is populated.

### 3. Enable verbose logs (restart required)

```bash
for T in Rayverse-SS Rayverse-IO Rayverse-DBG Rayverse-JNI; do
  MSYS_NO_PATHCONV=1 adb shell "setprop log.tag.$T VERBOSE"; done
adb shell am force-stop com.rayverse.rayman.debug
adb shell monkey -p com.rayverse.rayman.debug -c android.intent.category.LAUNCHER 1
```

Properties are cached per process — the restart is mandatory, not optional.

### 4. Driving menus with adb

**Always use `input keyevent --longpress <code>`** — plain `keyevent` sends down+up within 1 ms and the engine polls `Touche_Enfoncee` per frame, so presses silently drop at translated-emulator FPS. Codes: Enter=66, Space=62, DPAD_UP=19/DOWN=20/LEFT=21/RIGHT=22, BACK=4. Screenshot after every step; the SDL/GL surface makes `uiautomator dump` empty for in-game screens.

Known-good taps on this AVD (device coords): setup-screen **Play** ≈ (1201,251); in-game **gear** ≈ (2198,118); gear-dialog rows (uiautomator bounds are stable for this Java dialog) **即时存档** ≈ (1203,346), **即时读档** ≈ (1203,515), **查看调试日志** ≈ (1203,684). The dialog's OK button overlaps the START slot — verify with a screenshot before trusting a tap. S20 real device (1080x2400 override @480dpi, landscape coord space 2400x1080): Play ≈ (1162,257), gear ≈ (2174,155), dialog rows 即时存档 ≈ (1150,348), 即时读档 ≈ (1150,515), 关闭 ≈ (1165,838) — the row band is ~140 px tall, so a tap up to y≈430 still hits 即时存档.

Title → playable level sequence:

1. `--longpress 66` on the title → "CHOOSE A GAME" slot screen.
2. `--longpress 66` on an empty slot → name-entry sub-screen: **each Enter advances one character position** (posx 1→2→3→checkmark); arrows are ignored there. Send 4 spaced Enters.
3. Enter on the slot screen defaults to START → `LoadGameOnDisk` → vignette, one more Enter skips it → **world map, NOT gameplay**. Rayman spawns on the save point — stepping onto it shows "GAME SAVED" (that is the engine's own RAYMANn.SAV write, unrelated to 即时存档).
4. Walk on the world map to the level-1 entrance (the path winds up/right from the save point — straight RIGHT is not the way; navigating the map by adb taps is awkward, let the user drive), press OK/Enter → `RAY1.LEV` appears in the log → level intro animation ("WELCOME TO SKYFOOL ISLAND", Rayman auto-runs) — **OK/Enter skips it**. During the world map and the intro animation the savestate gate reports `map=0 menu=1` and rejects saves (status 2) — expected, not a bug.

Level-1 caveat: spawn sits next to water — walking RIGHT kills Rayman quickly. Useful as a deliberate state disturbance.

### 5. Savestate check (Phase 1 in-session slot)

In gameplay: gear → 即时存档 → confirm tap; then disturb state (die, wander); gear → 即时读档. Read evidence:

```bash
MSYS_NO_PATHCONV=1 adb logcat -d -s Rayverse-SS:V Rayverse-IO:V Rayverse-DBG:V | tail -40
MSYS_NO_PATHCONV=1 adb exec-out screencap -p > android/_scratch/emu_<n>_<label>.png
```

Pass criteria:

- `saved: world=W level=L segment=395K` — **segment must be ≈395K**; a 18K value means the RELRO-segment regression (`docs/android-debug-log.md` §7) is back.
- `loaded: world=W level=L` and status code returns via toast; position/Lums/ting visual match the pre-save screenshot.
- No SIGSEGV (`adb logcat -d -b crash`), game stays playable after load (move, jump, die, respawn).

Status codes (Java toasts / `nativeGetSaveStateStatus`): 1 saved, 2 save rejected (not in gameplay), 3 save failed, 4 loaded, 5 no snapshot, 6 load rejected, 7 world/level mismatch. The gate is `RaymanDansUneMapDuJeu && !During_The_Menu && !GoMenu && !gele` — codes 2/6 in menus are expected behavior, not failures. Java polls status 200 ms × 15, so logs land within ~3 s. Every code (including rejections) has a breadcrumb; the chain `request: save → frame hook: op=1 → saved/rejected` pinpoints where a failure stopped.

Without adb (real device): gear → **查看调试日志** opens the full ring-buffer dump (Java + native, release included) and copies it to the clipboard; the dialog auto-scrolls to the tail. Its header line (version, debug flag, model) also proves which build is installed.

### 6. Cleanup / hand-off

- Screenshots and log dumps go to `android/_scratch/` (git-ignored).
- `adb shell wm size reset` / `density reset` if a narrow-screen override was used.
- Emulator acceptance ≠ real-device acceptance: report which of `docs/savestate-plan.md` verification checklist items the emulator covered, and leave physics/audio feel items to the user's device.

## More detail

- Root-cause write-ups and adb pitfalls: `docs/android-debug-log.md` (§7 savestate segment bug, §8 tooling table, §9 log capture).
- Savestate design and verification checklist: `docs/savestate-plan.md`.
