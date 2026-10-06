/*
 * savestate.c — emulator-style instant save/load (Android port)
 *
 * Captures the whole engine state at a frame boundary so the player can
 * save anywhere, independent of the game's own save-zone/RAYMANn.SAV system:
 *
 *   - libmain.so's writable data segment (all engine globals)
 *   - the arena pools (block_free() only rewinds the cursor, so the pool
 *     blocks stay at fixed addresses for the whole session and a replayed
 *     load_level()/load_world() reproduces the same internal layout — that
 *     is what keeps the pointers inside the snapshot valid)
 *   - small heap buffers owned by globals (flocon_tab, rvb_special)
 *
 * Save/load only run while Rayman is actually inside a level frame, so the C
 * call stack has the same shape at restore time as it had at capture time.
 * Platform-owned state that shares the data segment (global_app_state, this
 * module's own context) is backed up and written back around the restore.
 * Audio heap pointers are compared before/after: if the sound bank or music
 * stream was reloaded between save and load, the snapshot's pointer would be
 * dangling, so the live one is kept and the world bank is reloaded instead.
 *
 * Phase 1: single slot, memory only. See docs/savestate-plan.md.
 */

#ifdef ANDROID

#include <link.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "ray_log.h"
#include "savestate.h"

#define SS_TAG "Rayverse-SS"
#define SSLOGI(...) RAY_LOG(SS_TAG, ANDROID_LOG_INFO, __VA_ARGS__)
#define SSLOGE(...) RAY_LOG(SS_TAG, ANDROID_LOG_ERROR, __VA_ARGS__)

enum {
	SS_OP_NONE = 0,
	SS_OP_SAVE = 1,
	SS_OP_LOAD = 2,
};

enum {
	SS_POOL_LEVEL = 0,
	SS_POOL_WORLD,
	SS_POOL_SPRITE,
	SS_POOL_FIX,
	SS_POOL_TMP,
	SS_POOL_COUNT,
};

#define SS_FLOCON_COUNT 512
#define SS_RVB_COUNT 16

typedef struct {
	mem_t* pool;      /* engine pointer at save time (NULL if unallocated) */
	size_t capacity;
	size_t len;
	s32 cursor;
	u8* data;         /* malloc'd copy of data[0..capacity] */
} ss_pool_snap_t;

typedef struct {
	atomic_int request;
	atomic_int status;
	int has_snapshot;

	uintptr_t seg_base;
	size_t seg_size;
	u8* seg_copy;

	ss_pool_snap_t pools[SS_POOL_COUNT];
	u8* flocon_copy;
	u8* rvb_copy;

	/* audio heap pointers as they were at save time */
	u8* aud_bnk_fixe;
	u8* aud_bnk_world;
	u8* aud_ptr_tchatch;
	stb_vorbis* aud_decoder;
	mem_t* aud_ogg_file;

	s16 meta_num_world;
	s16 meta_num_level;
} ss_ctx_t;

/* Every piece of mutable state in this module lives inside ss, so that a
 * single exclusion range covers it when the data segment is restored. */
static ss_ctx_t ss;

static mem_t** ss_pool_slot(int i) {
	switch (i) {
		case SS_POOL_LEVEL:  return &main_mem_level;
		case SS_POOL_WORLD:  return &main_mem_world;
		case SS_POOL_SPRITE: return &main_mem_sprite;
		case SS_POOL_FIX:    return &main_mem_fix;
		default:             return &main_mem_tmp;
	}
}

/* main_mem_tmp is freed by the engine without NULLing the global
 * (bonus.c / display.c call free(main_mem_tmp) only), so during normal level
 * gameplay the global is a dangling pointer whose header now holds allocator
 * bookkeeping — on a real device that read as capacity≈9EB and the snapshot
 * malloc failed with ENOMEM. A pool with an implausible header is treated as
 * absent on BOTH the save and the load path, so it is skipped consistently
 * and never written back into freed memory. */
#define SS_POOL_CAP_MAX (64u * 1024u * 1024u)

static mem_t* ss_pool_ptr(int i) {
	mem_t* p = *ss_pool_slot(i);
	if (!p) return NULL;
	if (p->capacity == 0 || p->capacity > SS_POOL_CAP_MAX || p->len > p->capacity)
		return NULL;
	return p;
}

/* Only inside a level frame: stable call stack, and main_mem_tmp is freed
 * there (the bonus/perftime screens allocate it), so the pool set matches. */
static int ss_in_gameplay(void) {
	return RaymanDansUneMapDuJeu && !During_The_Menu && !GoMenu && !gele;
}

/* ---- data segment discovery ---- */

static int ss_phdr_cb(struct dl_phdr_info* info, size_t size, void* data) {
	(void)size; (void)data;
	const char* name = info->dlpi_name;
	if (!name || !strstr(name, "libmain.so")) return 0;
	// libmain.so has two PF_W PT_LOADs: the small RELRO block (.data.rel.ro,
	// .got — mprotect'ed read-only after relocation, never mutated at runtime)
	// and the real data segment (.data + .bss). Take the largest one.
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
		if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W) && ph->p_memsz > ss.seg_size) {
			ss.seg_base = (uintptr_t)(info->dlpi_addr + ph->p_vaddr);
			ss.seg_size = ph->p_memsz;
		}
	}
	return 1;
}

static int ss_locate_segment(void) {
	if (ss.seg_base) return 1;
	dl_iterate_phdr(ss_phdr_cb, NULL);
	if (!ss.seg_base) {
		SSLOGE("writable PT_LOAD of libmain.so not found");
		return 0;
	}
	SSLOGI("data segment base=%p size=%zuK", (void*)ss.seg_base, ss.seg_size / 1024);
	return 1;
}

/* Buffers are allocated once and reused: repeated save/load must not grow. */

/* Dump the process memory watermark so an allocation failure can be told
 * apart from a genuine OOM (real-device reports go through the log ring). */
static void ss_log_proc_mem(void) {
	FILE* f = fopen("/proc/self/status", "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "VmPeak:", 7) || !strncmp(line, "VmSize:", 7)
		 || !strncmp(line, "VmRSS:", 6) || !strncmp(line, "VmPin:", 6)) {
			line[strcspn(line, "\n")] = '\0';
			SSLOGE("mem %s", line);
		}
	}
	fclose(f);
}

static int ss_alloc_buffers(void) {
	if (!ss_locate_segment()) return 0;

	if (!ss.seg_copy) {
		ss.seg_copy = (u8*)malloc(ss.seg_size);
		if (!ss.seg_copy) {
			SSLOGE("alloc seg_copy %zuK failed: errno=%d (%s)",
			       ss.seg_size / 1024, errno, strerror(errno));
			ss_log_proc_mem();
			return 0;
		}
		SSLOGI("seg_copy allocated: %zuK", ss.seg_size / 1024);
	}
	for (int i = 0; i < SS_POOL_COUNT; i++) {
		mem_t* p = ss_pool_ptr(i);
		if (!p) continue;
		if (ss.pools[i].data && ss.pools[i].capacity == p->capacity) continue;
		free(ss.pools[i].data);
		ss.pools[i].data = (u8*)malloc(p->capacity);
		if (!ss.pools[i].data) {
			ss.pools[i].capacity = 0;
			SSLOGE("alloc pool %d %zuK failed: errno=%d (%s)",
			       i, p->capacity / 1024, errno, strerror(errno));
			ss_log_proc_mem();
			return 0;
		}
		ss.pools[i].capacity = p->capacity;
		SSLOGI("pool %d snapshot buffer: %zuK", i, p->capacity / 1024);
	}
	if (!ss.flocon_copy) {
		ss.flocon_copy = (u8*)malloc(SS_FLOCON_COUNT * sizeof(flocon_t));
		if (!ss.flocon_copy) {
			SSLOGE("alloc flocon %zuK failed: errno=%d (%s)",
			       (size_t)SS_FLOCON_COUNT * sizeof(flocon_t) / 1024, errno, strerror(errno));
			ss_log_proc_mem();
			return 0;
		}
	}
	if (!ss.rvb_copy) {
		ss.rvb_copy = (u8*)malloc(SS_RVB_COUNT * sizeof(rgb_palette_t));
		if (!ss.rvb_copy) {
			SSLOGE("alloc rvb failed: errno=%d (%s)", errno, strerror(errno));
			ss_log_proc_mem();
			return 0;
		}
	}
	return 1;
}

/* ---- save ---- */

static void ss_do_save(void) {
	if (!ss_in_gameplay()) {
		atomic_store(&ss.status, 2); /* rejected: not in a level */
		SSLOGI("save rejected: not in gameplay (map=%d menu=%d gomenu=%d gele=%d)",
		       (int)RaymanDansUneMapDuJeu, (int)During_The_Menu, (int)GoMenu, (int)gele);
		return;
	}
	if (!ss_alloc_buffers()) {
		atomic_store(&ss.status, 3); /* failed */
		SSLOGE("save failed: buffer allocation (seg=%zuK)", ss.seg_size / 1024);
		return;
	}

	memcpy(ss.seg_copy, (void*)ss.seg_base, ss.seg_size);

	for (int i = 0; i < SS_POOL_COUNT; i++) {
		mem_t* raw = *ss_pool_slot(i);
		mem_t* p = ss_pool_ptr(i);
		if (raw && !p)
			SSLOGI("pool %d header implausible (len=%zu capacity=%zu) — treating as freed",
			       i, raw->len, raw->capacity);
		ss.pools[i].pool = p;
		if (!p) {
			ss.pools[i].capacity = 0;
			continue;
		}
		ss.pools[i].capacity = p->capacity;
		ss.pools[i].len = p->len;
		ss.pools[i].cursor = p->cursor;
		if (p->capacity && ss.pools[i].data) {
			memcpy(ss.pools[i].data, p->data, p->capacity);
		}
	}

	if (flocon_tab) memcpy(ss.flocon_copy, flocon_tab, SS_FLOCON_COUNT * sizeof(flocon_t));
	if (rvb_special) memcpy(ss.rvb_copy, rvb_special, SS_RVB_COUNT * sizeof(rgb_palette_t));

	ss.aud_bnk_fixe = bnkDataFixe;
	ss.aud_bnk_world = bnkDataWorld;
	ss.aud_ptr_tchatch = ptrTchatch;
	ss.aud_decoder = ogg_cd_track.decoder;
	ss.aud_ogg_file = ogg_cd_track.file;

	ss.meta_num_world = num_world;
	ss.meta_num_level = num_level;
	ss.has_snapshot = 1;
	atomic_store(&ss.status, 1); /* saved */
	SSLOGI("saved: world=%d level=%d segment=%zuK",
	       (int)num_world, (int)num_level, ss.seg_size / 1024);
}

/* ---- load ---- */

static void ss_do_load(void) {
	if (!ss.has_snapshot) {
		atomic_store(&ss.status, 5); /* no snapshot */
		SSLOGI("load rejected: no snapshot in this session");
		return;
	}
	if (!ss_in_gameplay()) {
		atomic_store(&ss.status, 6); /* rejected: not in a level */
		SSLOGI("load rejected: not in gameplay (map=%d menu=%d gomenu=%d gele=%d)",
		       (int)RaymanDansUneMapDuJeu, (int)During_The_Menu, (int)GoMenu, (int)gele);
		return;
	}

	/* Pool layout must still match the snapshot, otherwise the pointers
	 * stored inside it would not line up with the live blocks. */
	for (int i = 0; i < SS_POOL_COUNT; i++) {
		mem_t* p = ss_pool_ptr(i);
		if ((p == NULL) != (ss.pools[i].pool == NULL)) {
			atomic_store(&ss.status, 7); /* mismatch */
			SSLOGE("load rejected: pool %d presence changed (now=%p was=%p)",
			       i, (void*)p, (void*)ss.pools[i].pool);
			return;
		}
		if (p && p->capacity != ss.pools[i].capacity) {
			atomic_store(&ss.status, 7);
			SSLOGE("load rejected: pool %d capacity %zu != %zu",
			       i, p->capacity, ss.pools[i].capacity);
			return;
		}
	}

	/* Live audio state, captured before the globals get overwritten. */
	u8* live_bnk_fixe = bnkDataFixe;
	u8* live_bnk_world = bnkDataWorld;
	u8* live_tchatch = ptrTchatch;
	ogg_t live_ogg = ogg_cd_track;

	stop_all_snd();
	SDL_ClearQueuedAudio(global_app_state.sdl.sound_output.audio_device);

	app_state_t* app_backup = (app_state_t*)malloc(sizeof(app_state_t));
	ss_ctx_t* ss_backup = (ss_ctx_t*)malloc(sizeof(ss_ctx_t));
	if (!app_backup || !ss_backup) {
		free(app_backup);
		free(ss_backup);
		atomic_store(&ss.status, 3); /* failed */
		SSLOGE("load failed: backup allocation");
		return;
	}
	memcpy(app_backup, &global_app_state, sizeof(app_state_t));
	memcpy(ss_backup, &ss, sizeof(ss_ctx_t));

	/* Whole-segment restore, then put the platform-owned bytes back. */
	memcpy((void*)ss_backup->seg_base, ss_backup->seg_copy, ss_backup->seg_size);
	memcpy(&global_app_state, app_backup, sizeof(app_state_t));
	memcpy(&ss, ss_backup, sizeof(ss_ctx_t));
	free(app_backup);
	free(ss_backup);

	/* Restore the pool headers from the recorded pointers, not the live
	 * globals: after the segment memcpy the tmp global may again be the
	 * dangling value that ss_pool_ptr() rejected at save time. */
	for (int i = 0; i < SS_POOL_COUNT; i++) {
		mem_t* p = ss.pools[i].pool;
		if (!p) continue;
		p->capacity = ss.pools[i].capacity;
		p->len = ss.pools[i].len;
		p->cursor = ss.pools[i].cursor;
		if (p->capacity && ss.pools[i].data) {
			memcpy(p->data, ss.pools[i].data, p->capacity);
		}
	}

	if (flocon_tab) memcpy(flocon_tab, ss.flocon_copy, SS_FLOCON_COUNT * sizeof(flocon_t));
	if (rvb_special) memcpy(rvb_special, ss.rvb_copy, SS_RVB_COUNT * sizeof(rgb_palette_t));

	/* If the sound bank or the music stream was reloaded since the save, the
	 * restored pointer is dangling: keep the live object and reload the bank
	 * that belongs to the restored world. */
	if (live_bnk_fixe != ss.aud_bnk_fixe) bnkDataFixe = live_bnk_fixe;
	if (live_tchatch != ss.aud_ptr_tchatch) ptrTchatch = live_tchatch;
	if (live_bnk_world != ss.aud_bnk_world) {
		bnkDataWorld = live_bnk_world;
		LoadBnkWorld(num_world_choice);
	}
	if (live_ogg.decoder != ss.aud_decoder || live_ogg.file != ss.aud_ogg_file) {
		ogg_cd_track = live_ogg;
		if (!ogg_cd_track.decoder) is_ogg_playing = false;
	}

	/* Don't leave keys stuck from the moment of the snapshot. */
	memset(Touche_Enfoncee, 0, sizeof(Touche_Enfoncee));

	atomic_store(&ss.status, 4); /* loaded */
	SSLOGI("loaded: world=%d level=%d", (int)num_world, (int)num_level);
}

/* ---- entry points ---- */

void savestate_frame_hook(void) {
	int op = atomic_exchange(&ss.request, SS_OP_NONE);
	if (op == SS_OP_NONE) return;
	SSLOGI("frame hook: op=%d at frame boundary", op);
	if (op == SS_OP_SAVE) ss_do_save();
	else if (op == SS_OP_LOAD) ss_do_load();
}

void savestate_request(int op) {
	SSLOGI("request: %s", op == SS_OP_SAVE ? "save" : "load");
	atomic_store(&ss.request, op);
}

int savestate_get_status(void) {
	return atomic_exchange(&ss.status, 0);
}

int savestate_has_snapshot(void) {
	return ss.has_snapshot;
}

#endif /* ANDROID */
