#ifndef SAVESTATE_H
#define SAVESTATE_H

/* savestate.c — emulator-style instant save/load for the Android port.
 * The core lives inside the unity build (needs access to every engine
 * global); these entry points are consumed from android_jni.c and from
 * the advance_frame() hook in engine.c.
 *
 * Phase 1 = one in-memory snapshot. Phase 2a = 3 on-disk slots in the save
 * dir plus a pointer-classification probe (docs/savestate-phase2.md).
 * Phase 2b = restoring a slot written by another process: the snapshot's
 * absolute addresses are relocated onto this process' layout. Loading only
 * works while playing a level — there is deliberately no boot-time "resume"
 * steering the engine into the snapshot's level. */

#ifdef ANDROID

/* Called on the game thread at every frame boundary; executes a pending
 * save/load request if one was queued from the UI thread. */
void savestate_frame_hook(void);

/* Queue a request from any thread.
 * op: 1 = capture + write to slot, 2 = read slot + restore. slot: 0..2.
 * The work happens on the game thread at the next frame boundary. */
void savestate_request(int op, int slot);

/* Returns the last status code and resets it to idle (read-once).
 * 1=saved to slot 2=save rejected 3=save failed 4=loaded from slot
 * 5=no snapshot 6=load rejected 7=load mismatch
 * 8=writing the slot file failed 9=slot file unusable (magic/format/layout
 * stamp/checksum) 10=slot file cannot be mapped into this process (a
 * region has no live counterpart to relocate onto) */
int savestate_get_status(void);

/* 1 once a snapshot exists in memory. */
int savestate_has_snapshot(void);

/* Metadata of a slot file, for the UI. Safe to call from the UI thread:
 * it only reads the file header, never engine state.
 * Returns 1 when the slot exists and the header parses (out params may be
 * NULL), 0 when there is no such file, -1 when the file is not a snapshot.
 * *created is the save timestamp in seconds since the epoch; *usable is 0
 * when the file was written by a different build of the engine. */
int savestate_slot_info(int slot, int* world, int* level, long long* created, int* usable);

/* Path of the directory owned by this app (internal storage, writable),
 * set from Java through nativeSetSaveDir. Implemented in android_jni.c. */
const char* android_get_save_dir(void);

/* Keep-live backup of android_jni.c's process-owned statics (log ring
 * pointer, SAF fd table, data/save dir strings). ss_do_load() calls save()
 * just before the whole-segment memcpy and restore() right after it —
 * the segment rollback would otherwise leave the old process' values in
 * place (dangling ring pointer, stale fds). Implemented in android_jni.c;
 * see docs/savestate-cross-process-crash.md. */
void* android_plat_state_save(void);
void android_plat_state_restore(void* blob);

#endif /* ANDROID */

#endif /* SAVESTATE_H */
