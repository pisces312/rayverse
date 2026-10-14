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
- The user's real S20 often connects wirelessly at the same time → **always** address the emulator as `adb -s emulator-5554 …` (phone serial `RFCNC0MT5VV`); otherwise every command fails with `more than one device/emulator`.
- **Never `pm clear com.rayverse.rayman.debug`** — it wipes the SAF folder grant *and* the game's saves. `adb install -r` keeps both; that is the only sanctioned reinstall path.
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

Never `adb push` a directory (bad_alloc crash). First run (or after `pm clear`, which you should avoid) needs the SAF folder grant: tap **Change folder** → in DocumentsUI tap **"USE THIS FOLDER"** ≈ (1200,891) → in the confirmation dialog tap **"ALLOW"** ≈ (1793,640) (that dialog's buttons are reachable via `uiautomator dump` — read `android:id/button1`'s `bounds` and tap its center, since this is a framework dialog, not the game's). `GameDataBridge` logs `Game data OK (100 files)` when the fd table is populated.

### 3. Enable verbose logs (restart required)

```bash
for T in Rayverse-SS Rayverse-IO Rayverse-DBG Rayverse-JNI; do
  MSYS_NO_PATHCONV=1 adb shell "setprop log.tag.$T VERBOSE"; done
adb shell am force-stop com.rayverse.rayman.debug
adb shell monkey -p com.rayverse.rayman.debug -c android.intent.category.LAUNCHER 1
```

Properties are cached per process — the restart is mandatory, not optional.

### 4. Driving menus with adb

**Always use `input keyevent --longpress <code>`** — plain `keyevent` sends down+up within 1 ms and the engine polls `Touche_Enfoncee` per frame, so presses silently drop at translated-emulator FPS. Codes: Enter=66, Space=62, DPAD_UP=19/DOWN=20/LEFT=21/RIGHT=22, BACK=4. Screenshot after every step.

`uiautomator dump` is **useless for this app's Java AlertDialogs too**, not just the GL surface: it returns a ~2.8 KB tree with no `text=`/usable bounds. Decide "dialog open / which row" from the screenshot only (`exec-out screencap` is 1:1 with device pixels on this AVD, so measure coordinates off it).

Known-good taps on this AVD (device coords, 2400x1080 landscape): setup-screen **Play** ≈ (1200,309); in-game **gear** ≈ **(2315,130)** (the old (2198,118) misses — recompute from `layoutSettingsButton`: 12·density margin, icon = min(w,h)·0.10). Gear-dialog rows: **即时存档** (1200,346), **即时读档** (1200,515), **查看调试日志** (1200,684), 关闭/取消 (1200,828). The Phase 2a **slot dialog** reuses the same band: rows 槽位1/2/3 ≈ (1200,352) / (1200,517) / (1200,681), 取消 (1200,830); row band ≈140 px, so ±60 px in y is still the same row. The in-game OK button overlaps the START slot — verify with a screenshot before trusting a tap. S20 real device (1080x2400 override @480dpi, landscape coord space 2400x1080): Play ≈ (1162,257), gear ≈ (2174,155), dialog rows 即时存档 ≈ (1150,348), 即时读档 ≈ (1150,515), 关闭 ≈ (1165,838).

### Getting to gameplay

`bash android/_scratch/emu_into_level.sh` does install → launch → Play tap → 4 spaced Enters with the timings that make it reliable (~2.5 min). Run it from the repo root, then **verify with a screenshot** — it ends blind. If the script is gone, the manual sequence is:

1. `--longpress 66` on the title → "CHOOSE A GAME" slot screen.
2. `--longpress 66` on an empty slot → name-entry sub-screen: **each Enter advances one character position** (posx 1→2→3→checkmark); arrows are ignored there. Send 4 spaced Enters.
3. Enter on the slot screen defaults to START → `LoadGameOnDisk` → vignette, one more Enter skips it → **world map, NOT gameplay**. Rayman spawns on the save point — stepping onto it shows "GAME SAVED" (that is the engine's own RAYMANn.SAV write, unrelated to 即时存档).
4. The world map is a **node graph**, not a walk: from the save node, one Enter/OK on the highlighted node enters `RAY1.LEV` directly (`t_world_info[].index_*` + `RESPOND_TO_*`), so the adb path is Enter → `RAY1.LEV` in the log → intro animation → Enter skips it → gameplay. (Walking the map by taps is what the old "wind up/right" note was about; don't bother, and let the user drive if a specific position is needed.) During the world map and the intro animation the savestate gate reports `map=0 menu=1` and rejects saves (status 2) — expected, not a bug.

Level-1 caveat: spawn sits next to water — walking RIGHT kills Rayman quickly. Useful as a deliberate state disturbance.

### 5. Savestate check (Phase 1 in-session, Phase 2a slots)

In gameplay: gear → 即时存档 → **slot dialog** → pick a row; then disturb state (die, wander);
gear → 即时读档 → same slot. Read evidence:

```bash
MSYS_NO_PATHCONV=1 adb logcat -d -s Rayverse-SS:V Rayverse-IO:V Rayverse-DBG:V | tail -40
MSYS_NO_PATHCONV=1 adb exec-out screencap -p > android/_scratch/emu_<n>_<label>.png
# slot files live in internal storage — run-as works on the debug build:
MSYS_NO_PATHCONV=1 adb shell "run-as com.rayverse.rayman.debug ls -l /data/data/com.rayverse.rayman.debug/files/rayverse_save"
```

Pass criteria:

- `captured: world=W level=L segment=395K stamp=<hex>` — **segment must be ≈395K**; a 18K value means the RELRO-segment regression (`docs/android-debug-log.md` §7) is back.
- `slot N written: world=W level=L payload=5861K in <ms>` — the file must actually appear, 6,002,720 B
  (payload 5861K + 416 B header/region table). Writes measured 26–85 ms, reads 57–155 ms; if `written:`
  is missing while the toast said saved, that is the status-publish race of `docs/android-debug-log.md` §11.
- `slot N read: …` + `loaded: world=W level=L`; position/Lums/tings match the pre-save screenshot;
  other slots keep their own content (cross-slot isolation).
- No SIGSEGV (`adb logcat -d -b crash`), game stays playable after load (move, jump, die, respawn).
- Probe line (`Rayverse-SS:V`, DEBUG for samples): `probe TOTAL: slots=750289 … self=1 seg=121
  libmain=1652 pool=[2286,314,53,627,0] …`. `libmain` must be **≈1.5–1.7K, not 0** — 0 means the
  maps path parse regressed and the module range was never registered (check `regions=14` in the
  `probe(capture)` line). Baseline and interpretation: `docs/savestate-phase2.md` §9.

Status codes (Java toasts / `nativeGetSaveStateStatus`): 1 saved-to-slot, 2 save rejected (not in gameplay), 3 alloc failed, 4 loaded-from-slot, 5 no snapshot, 6 load rejected, 7 world/level mismatch, 8 slot write failed, 9 file unusable (magic/format/stamp/checksum), 10 layout moved — i.e. the slot was written by **another process**, which phase 2a refuses by design (`docs/android-debug-log.md` §12). The gate is `RaymanDansUneMapDuJeu && !During_The_Menu && !GoMenu && !gele` — codes 2/6 in menus are expected behavior, not failures. Java polls status every 250 ms up to 40 times, so logs land within ~3 s. Every code (including rejections) has a breadcrumb; the chain `request: save → frame hook: op=1 → captured → written/rejected` pinpoints where a failure stopped.

Without adb (real device): gear → **查看调试日志** opens the full ring-buffer dump (Java + native, release included) and copies it to the clipboard; the dialog auto-scrolls to the tail. Its header line (version, debug flag, model) also proves which build is installed.

### 6. Cleanup / hand-off

- Screenshots and log dumps go to `android/_scratch/` (git-ignored).
- `adb shell wm size reset` / `density reset` if a narrow-screen override was used.
- Emulator acceptance ≠ real-device acceptance: report which of `docs/savestate-plan.md` verification checklist items the emulator covered, and leave physics/audio feel items to the user's device.

## More detail

- Root-cause write-ups and adb pitfalls: `docs/android-debug-log.md` (§7 savestate segment bug, §8 tooling table, §9 log capture, §11 status-publish race, §12 cross-process refusal).
- Savestate Phase 1 design/checklist: `docs/savestate-plan.md`; Phase 2 file format, slot UI, probe baseline: `docs/savestate-phase2.md` (§9 is the measured 2a record).
