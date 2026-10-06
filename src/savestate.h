#ifndef SAVESTATE_H
#define SAVESTATE_H

/* savestate.c — emulator-style instant save/load for the Android port.
 * The core lives inside the unity build (needs access to every engine
 * global); these entry points are consumed from android_jni.c and from
 * the advance_frame() hook in engine.c. */

#ifdef ANDROID

/* Called on the game thread at every frame boundary; executes a pending
 * save/load request if one was queued from the UI thread. */
void savestate_frame_hook(void);

/* Queue a request from any thread. op: 1 = save, 2 = load. */
void savestate_request(int op);

/* Returns the last status code and resets it to idle (read-once).
 * 1=saved 2=save rejected 3=save failed 4=loaded
 * 5=no snapshot 6=load rejected 7=load mismatch */
int savestate_get_status(void);

/* 1 once a snapshot exists in memory. */
int savestate_has_snapshot(void);

#endif /* ANDROID */

#endif /* SAVESTATE_H */
