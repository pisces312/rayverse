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
 * Phase 1: single slot, memory only.
 * Phase 2a (this file): the state is described as a *region table* so it can
 *   be written to one of 3 slot files in the save dir and read back; plus a
 *   pointer-classification probe that counts what kind of absolute addresses
 *   the snapshot actually contains. The probe data is what gates phase 2b
 *   (restoring across a process restart, where every one of those addresses
 *   has been re-randomised by ASLR) — see docs/savestate-phase2.md.
 */

#ifdef ANDROID

#include <link.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "ray_log.h"
#include "savestate.h"

#define SS_TAG "Rayverse-SS"
#define SSLOGI(...) RAY_LOG(SS_TAG, ANDROID_LOG_INFO, __VA_ARGS__)
#define SSLOGD(...) RAY_LOG(SS_TAG, ANDROID_LOG_DEBUG, __VA_ARGS__)
#define SSLOGE(...) RAY_LOG(SS_TAG, ANDROID_LOG_ERROR, __VA_ARGS__)

enum {
	SS_OP_NONE = 0,
	SS_OP_SAVE_TO_SLOT = 1,
	SS_OP_LOAD_FROM_SLOT = 2,
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

/* ---- slot files ---- */

#define SS_MAX_SLOTS 3
#define SS_SLOT_FILE "rayverse_ss_%d.bin"
#define SS_MAGIC 0x53535652u          /* 'R''V''S''S' */
#define SS_FMT_VERSION 1u
#define SS_STAMP_MISSING 0u

/* Regions are the addressable pieces of the snapshot. Phase 2a only has COPY
 * ones; phase 2b adds ADDRESS_ONLY/COPY_RELOAD entries (jumelle tables,
 * EffetBufferNormal, sound banks) so the relocation pass knows about them. */
enum {
	SS_RGN_SEG = 0,
	SS_RGN_POOL0 = 1,               /* +i for each arena pool */
	SS_RGN_FLOCON = SS_RGN_POOL0 + SS_POOL_COUNT,
	SS_RGN_RVB,
	SS_RGN_COUNT
};

enum {
	SS_KIND_COPY = 1,       /* payload stored in the file */
	SS_KIND_COPY_RELOAD,    /* content rebuildable from game data */
	SS_KIND_ADDRESS_ONLY,   /* only its address is referenced */
	SS_KIND_KEEP_LIVE       /* platform-owned: never restored */
};

typedef struct {
	u32 id;
	u32 kind;
	u64 base;      /* address inside the process that wrote the file */
	u64 size;      /* payload bytes */
	u64 len;       /* mem_t::len — pools only */
	s64 cursor;    /* mem_t::cursor — pools only */
} ss_rec_t;

typedef struct {
	u32 magic;
	u32 fmt;
	u32 stamp;     /* memory-layout fingerprint, see ss_compute_stamp() */
	u32 rec_count;
	s32 world;
	s32 level;
	u64 created;   /* seconds since epoch, for the UI */
	u64 payload_bytes;
	u64 payload_hash;
	u64 aud[5];    /* bnkDataFixe, bnkDataWorld, ptrTchatch, ogg decoder, ogg file */
} ss_file_hdr_t;

typedef struct {
	mem_t* pool;      /* engine pointer to write back into (NULL if unallocated) */
	uintptr_t base;   /* address the snapshot's pointers refer to */
	size_t capacity;
	size_t len;
	s32 cursor;
	u8* data;         /* malloc'd copy of data[0..capacity] */
} ss_pool_snap_t;

typedef struct {
	atomic_int request;
	atomic_int request_slot;
	atomic_int status;
	int has_snapshot;

	uintptr_t seg_base;
	size_t seg_size;
	u8* seg_copy;
	uintptr_t seg_file_base;   /* as recorded by the file we loaded */

	ss_pool_snap_t pools[SS_POOL_COUNT];
	u8* flocon_copy;
	u8* rvb_copy;
	size_t flocon_size;
	size_t rvb_size;
	uintptr_t flocon_base;
	uintptr_t rvb_base;

	/* audio heap pointers as they were when the snapshot was taken */
	u8* aud_bnk_fixe;
	u8* aud_bnk_world;
	u8* aud_ptr_tchatch;
	stb_vorbis* aud_decoder;
	mem_t* aud_ogg_file;

	s16 meta_num_world;
	s16 meta_num_level;

	u32 stamp;      /* computed once; 0 until the segment is located */
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

static long long ss_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* ---- hashing / layout stamp ---- */

#define SS_FNV_OFFSET 1469598103934665603ull
#define SS_FNV_PRIME  1099511628211ull

static u64 ss_hash_bytes_raw(u64 h, const u8* p, size_t n) {
	while (n--) {
		h ^= *p++;
		h *= SS_FNV_PRIME;
	}
	return h;
}

static u64 ss_hash_u64(u64 h, u64 v) {
	return ss_hash_bytes_raw(h, (const u8*)&v, sizeof v);
}

/* A snapshot is only meaningful for the binary that produced it: the whole
 * format is raw bytes at layout offsets. ASLR moves every address, so the
 * identity has to be expressed with *relative* offsets — distances between
 * globals are fixed by the link and change as soon as a global is added,
 * removed or reordered, and struct sizes catch layout changes inside them.
 * Pure function of the binary: safe to call from any thread at any time,
 * unlike digging a build-id out of a PT_NOTE the Android linker may not map. */
#define SS_STAMP_DIST(a, b) ((u64)(ptrdiff_t)((char*)&(a) - (char*)&(b)))

static u32 ss_compute_stamp(void) {
	u64 h = SS_FNV_OFFSET;
	h = ss_hash_u64(h, (u64)sizeof(app_state_t));
	h = ss_hash_u64(h, (u64)sizeof(mem_t));
	h = ss_hash_u64(h, (u64)sizeof(flocon_t) * SS_FLOCON_COUNT);
	h = ss_hash_u64(h, (u64)sizeof(rgb_palette_t) * SS_RVB_COUNT);
	h = ss_hash_u64(h, (u64)sizeof(ogg_t));
	h = ss_hash_u64(h, (u64)sizeof(ss_rec_t) * SS_RGN_COUNT);
	h = ss_hash_u64(h, SS_STAMP_DIST(During_The_Menu, global_app_state));
	h = ss_hash_u64(h, SS_STAMP_DIST(RaymanDansUneMapDuJeu, GoMenu));
	h = ss_hash_u64(h, SS_STAMP_DIST(gele, num_world));
	h = ss_hash_u64(h, SS_STAMP_DIST(num_level, num_world_choice));
	h = ss_hash_u64(h, SS_STAMP_DIST(ogg_cd_track, Touche_Enfoncee));
	h = ss_hash_u64(h, SS_STAMP_DIST(flocon_tab, rvb_special));
	h = ss_hash_u64(h, SS_STAMP_DIST(bnkDataWorld, ptrTchatch));
	h = ss_hash_u64(h, SS_STAMP_DIST(main_mem_level, main_mem_fix));
	h = ss_hash_u64(h, SS_STAMP_DIST(main_mem_sprite, main_mem_tmp));
	h = ss_hash_u64(h, SS_STAMP_DIST(num_world, global_app_state));
	return (u32)(h >> 32) ^ (u32)h;
}

/* ---- buffers ---- */

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
		ss.flocon_size = SS_FLOCON_COUNT * sizeof(flocon_t);
		ss.flocon_copy = (u8*)malloc(ss.flocon_size);
		if (!ss.flocon_copy) {
			SSLOGE("alloc flocon %zuK failed: errno=%d (%s)",
			       ss.flocon_size / 1024, errno, strerror(errno));
			ss_log_proc_mem();
			return 0;
		}
	}
	if (!ss.rvb_copy) {
		ss.rvb_size = SS_RVB_COUNT * sizeof(rgb_palette_t);
		ss.rvb_copy = (u8*)malloc(ss.rvb_size);
		if (!ss.rvb_copy) {
			SSLOGE("alloc rvb failed: errno=%d (%s)", errno, strerror(errno));
			ss_log_proc_mem();
			return 0;
		}
	}
	if (!ss.stamp) ss.stamp = ss_compute_stamp();
	return 1;
}

/* ---- region accessors ---- */

/* Bytes of a region as they live in this module's buffers (the snapshot). */
static u8* ss_region_payload(int id, size_t* size) {
	*size = 0;
	if (id == SS_RGN_SEG) {
		*size = ss.seg_size;
		return ss.seg_copy;
	}
	if (id == SS_RGN_FLOCON) {
		*size = ss.flocon_size;
		return ss.flocon_copy;
	}
	if (id == SS_RGN_RVB) {
		*size = ss.rvb_size;
		return ss.rvb_copy;
	}
	int i = id - SS_RGN_POOL0;
	if (i >= 0 && i < SS_POOL_COUNT) {
		/* Sized from the live pool, not from the last capture: a pool that is
		 * currently freed/dangling has no payload, whatever the stale field
		 * says. */
		mem_t* p = ss_pool_ptr(i);
		*size = p ? p->capacity : 0;
		return p ? ss.pools[i].data : NULL;
	}
	return NULL;
}

/* Live state only — never the recorded snapshot: ss_read_slot() validates the
 * file against this process before anything has been captured into ss. */
static void ss_region_live(int id, uintptr_t* base, u64* size, u64* len, s64* cursor) {
	*base = 0; *size = 0; *len = 0; *cursor = 0;
	if (id == SS_RGN_SEG) {
		*base = ss.seg_base;
		*size = ss.seg_size;
		return;
	}
	if (id == SS_RGN_FLOCON) {
		*base = (uintptr_t)flocon_tab;
		*size = ss.flocon_size;
		return;
	}
	if (id == SS_RGN_RVB) {
		*base = (uintptr_t)rvb_special;
		*size = ss.rvb_size;
		return;
	}
	int i = id - SS_RGN_POOL0;
	if (i >= 0 && i < SS_POOL_COUNT) {
		mem_t* p = ss_pool_ptr(i);   /* sanitized: NULL when freed/dangling */
		if (!p) return;
		*base = (uintptr_t)p;
		*size = p->capacity;
		*len = p->len;
		*cursor = p->cursor;
	}
}

static const char* ss_region_name(int id) {
	static const char* const names[SS_RGN_COUNT] = {
		"seg", "pool0", "pool1", "pool2", "pool3", "pool4", "flocon", "rvb"
	};
	return (id >= 0 && id < SS_RGN_COUNT) ? names[id] : "?";
}

static void ss_build_records(ss_rec_t* rec) {
	for (int id = 0; id < SS_RGN_COUNT; id++) {
		uintptr_t base;
		u64 size, len;
		s64 cursor;
		memset(&rec[id], 0, sizeof(rec[id]));
		rec[id].id = (u32)id;
		rec[id].kind = SS_KIND_COPY;
		ss_region_live(id, &base, &size, &len, &cursor);
		rec[id].base = base;
		rec[id].size = size;
		rec[id].len = len;
		rec[id].cursor = cursor;
	}
}

/* ---- pointer classification probe (phase 2b evidence) ----
 *
 * The snapshot is raw bytes full of absolute addresses. Across a process
 * restart ASLR has moved every one of them, so phase 2b needs to know, per
 * region, how many slots point where. This scan answers exactly that, and it
 * is the only part of phase 2 that is measurement rather than speculation:
 * unknown pointers into memory nobody re-creates are what would crash a
 * cross-restart restore, and they show up here as heap_other/unmapped hits. */

enum {
	SS_RNGK_SEG = 1,    /* libmain.so's writable data segment */
	SS_RNGK_POOL,       /* an arena pool (pool_index says which) */
	SS_RNGK_HEAP,       /* a named one-shot heap blob */
	SS_RNGK_MODULE      /* libmain.so's other mappings (text/rodata/relro) */
};

#define SS_RNG_MAX 32

typedef struct {
	uintptr_t lo, hi;
	const char* name;
	u32 kind;
	int pool_index;
} ss_rng_t;

typedef struct {
	ss_rng_t rng[SS_RNG_MAX];
	int n;
} ss_probe_rng_t;

typedef struct {
	uintptr_t lo, hi;
	int rw;
	char name[128];
} ss_map_t;

#define SS_MAP_MAX 1024
#define SS_SAMPLE_MAX 6
/* Values below this that match no region are treated as packed game data, not
 * as pointers: real arm64 Android user addresses observed here start around
 * 0x5e00_0000_0000, while the noise (two packed u32s, three u16s) stays under
 * 0x1000_0000_0000. Counted apart so the "unknown mapping" buckets stay
 * meaningful. */
#define SS_NOISE_FLOOR (1ull << 40)

typedef struct {
	uintptr_t value;
	size_t off;
	char where[72];
} ss_sample_t;

typedef struct {
	u64 total;        /* 8-byte aligned slots looked at */
	u64 small;        /* below 0x10000: plain game data, not a pointer */
	u64 nonaddr;      /* >= 2^48: two packed 32-bit values, not an address */
	u64 self;         /* into the buffer being scanned (internal reference) */
	u64 seg;          /* into the snapshotted data segment */
	u64 module;       /* into libmain.so's other mappings (text/rodata/relro) */
	u64 pool[SS_POOL_COUNT];
	u64 rng_hit[SS_RNG_MAX];  /* per named heap blob */
	u64 heap_other;   /* into an anonymous rw mapping we do not track */
	u64 mapped_other; /* into any other mapping (file-backed, libc, SDL...) */
	u64 unmapped;     /* looks like an address but nothing is mapped there */
	u64 noise_low;    /* of the two above: below SS_NOISE_FLOOR, i.e. packed data */
	ss_sample_t s_heap_other[SS_SAMPLE_MAX];
	ss_sample_t s_unmapped[SS_SAMPLE_MAX];
	int n_heap_other, n_unmapped;
} ss_probe_t;

static int ss_load_maps(ss_map_t* maps, int max) {
	FILE* f = fopen("/proc/self/maps", "r");
	if (!f) return -1;
	int n = 0;
	char line[512];
	while (n < max && fgets(line, sizeof(line), f)) {
		unsigned long lo = 0, hi = 0;
		char perms[8] = {0};
		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) < 3)
			continue;
		char* nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		/* The path is whatever follows perms/offset/dev/inode. Skipping by field
		 * count matters: the previous strstr(line, perms) + one skip landed on the
		 * *file offset*, and with a 72-byte name buffer the install path
		 * ("/data/app/~~<32>==/com.rayverse.rayman.debug-<24>==/lib/arm64/libmain.so")
		 * got truncated before "libmain.so" — so the module range was never
		 * registered and the probe reported libmain=0 for a snapshot that really
		 * holds ~1500 pointers into libmain's own text segment. */
		char* p = strstr(line, perms);
		int path_at = 0;
		if (p) {
			p += strlen(perms);
			for (int f = 0; f < 3; f++) {          /* offset, dev, inode */
				while (*p == ' ') p++;
				while (*p && *p != ' ') p++;
			}
			while (*p == ' ') p++;
			path_at = (int)(p - line);
		}
		maps[n].lo = (uintptr_t)lo;
		maps[n].hi = (uintptr_t)hi;
		maps[n].rw = (perms[1] == 'w');
		snprintf(maps[n].name, sizeof(maps[n].name), "%s",
		         path_at ? line + path_at : "[anon]");
		n++;
	}
	fclose(f);
	return n;
}

static int ss_find_map(const ss_map_t* maps, int n, uintptr_t v, const char** name, int* rw) {
	for (int i = 0; i < n; i++) {
		if (v >= maps[i].lo && v < maps[i].hi) {
			if (name) *name = maps[i].name;
			if (rw) *rw = maps[i].rw;
			return i;
		}
	}
	return -1;
}

static void ss_probe_add_range(ss_probe_rng_t* set, uintptr_t lo, size_t size,
                               const char* name, u32 kind, int pool_index) {
	if (!lo || !size || set->n >= SS_RNG_MAX) return;
	ss_rng_t* r = &set->rng[set->n];
	r->lo = lo;
	r->hi = lo + size;
	r->name = name;
	r->kind = kind;
	r->pool_index = pool_index;
	set->n++;
}

static int ss_probe_classify(const ss_probe_rng_t* set, uintptr_t v) {
	for (int i = 0; i < set->n; i++)
		if (v >= set->rng[i].lo && v < set->rng[i].hi) return i;
	return -1;
}

static void ss_probe_sample(ss_sample_t* out, int* count, size_t off,
                            uintptr_t v, const char* where) {
	if (*count >= SS_SAMPLE_MAX) return;
	out[*count].value = v;
	out[*count].off = off;
	snprintf(out[*count].where, sizeof(out[*count].where), "%s", where ? where : "");
	(*count)++;
}

static void ss_probe_buffer(ss_probe_t* pr, const ss_probe_rng_t* set,
                            const ss_map_t* maps, int n_maps,
                            const u8* buf, size_t size) {
	if (!buf || !size) return;
	uintptr_t self_lo = (uintptr_t)buf;
	uintptr_t self_hi = self_lo + size;
	pr->total += size / 8;
	for (size_t off = 0; off + 8 <= size; off += 8) {
		uintptr_t v;
		memcpy(&v, buf + off, sizeof v);
		if (v < 0x10000) {
			pr->small++;
			continue;
		}
		/* Anything at or above 2^48 cannot be a user address on arm64 Android,
		 * so it is two packed 32-bit game values rather than a pointer. Without
		 * this gate the raw scan drowns the real hits in that noise. */
		if (v >> 48) {
			pr->nonaddr++;
			continue;
		}
		/* A pointer into the buffer being scanned is a snapshot-internal
		 * reference: source and target move together across a restart, so its
		 * delta must stay 0. Counted apart from the region table on purpose —
		 * this is the bucket a naive relocation pass would silently corrupt. */
		if (v >= self_lo && v < self_hi) {
			pr->self++;
			continue;
		}
		int idx = ss_probe_classify(set, v);
		if (idx < 0) {
			const char* name = NULL;
			int rw = 0;
			if (ss_find_map(maps, n_maps, v, &name, &rw) < 0) {
				pr->unmapped++;
				if (v < SS_NOISE_FLOOR) pr->noise_low++;
				ss_probe_sample(pr->s_unmapped, &pr->n_unmapped, off, v, NULL);
			} else if (rw) {
				pr->heap_other++;
				if (v < SS_NOISE_FLOOR) pr->noise_low++;
				ss_probe_sample(pr->s_heap_other, &pr->n_heap_other, off, v, name);
			} else {
				pr->mapped_other++;
			}
			continue;
		}
		const ss_rng_t* r = &set->rng[idx];
		if (r->kind == SS_RNGK_SEG) pr->seg++;
		else if (r->kind == SS_RNGK_MODULE) pr->module++;
		else if (r->kind == SS_RNGK_POOL) pr->pool[r->pool_index]++;
		else pr->rng_hit[idx]++;
	}
}

static void ss_probe_add(ss_probe_t* agg, const ss_probe_t* one) {
	agg->total += one->total;
	agg->small += one->small;
	agg->nonaddr += one->nonaddr;
	agg->self += one->self;
	agg->seg += one->seg;
	agg->module += one->module;
	agg->heap_other += one->heap_other;
	agg->mapped_other += one->mapped_other;
	agg->unmapped += one->unmapped;
	agg->noise_low += one->noise_low;
	for (int i = 0; i < SS_POOL_COUNT; i++) agg->pool[i] += one->pool[i];
	for (int i = 0; i < SS_RNG_MAX; i++) agg->rng_hit[i] += one->rng_hit[i];
}

static void ss_probe_report(const char* label, const ss_probe_t* pr, const ss_probe_rng_t* set) {
	SSLOGI("probe %s: slots=%llu small=%llu nonaddr=%llu self=%llu seg=%llu libmain=%llu "
	       "pool=[%llu,%llu,%llu,%llu,%llu] heap_other=%llu mapped_other=%llu unmapped=%llu "
	       "noise_low=%llu",
	       label,
	       (unsigned long long)pr->total, (unsigned long long)pr->small,
	       (unsigned long long)pr->nonaddr,
	       (unsigned long long)pr->self, (unsigned long long)pr->seg,
	       (unsigned long long)pr->module,
	       (unsigned long long)pr->pool[0], (unsigned long long)pr->pool[1],
	       (unsigned long long)pr->pool[2], (unsigned long long)pr->pool[3],
	       (unsigned long long)pr->pool[4],
	       (unsigned long long)pr->heap_other, (unsigned long long)pr->mapped_other,
	       (unsigned long long)pr->unmapped, (unsigned long long)pr->noise_low);
	for (int i = 0; i < set->n; i++) {
		if (pr->rng_hit[i])
			SSLOGI("probe %s: -> %s = %llu", label, set->rng[i].name,
			       (unsigned long long)pr->rng_hit[i]);
	}
	for (int i = 0; i < pr->n_heap_other; i++)
		SSLOGD("probe %s: heap_other sample %d: +0x%zx -> 0x%lx in %s", label, i,
		       pr->s_heap_other[i].off, (unsigned long)pr->s_heap_other[i].value,
		       pr->s_heap_other[i].where);
	for (int i = 0; i < pr->n_unmapped; i++)
		SSLOGD("probe %s: unmapped sample %d: +0x%zx -> 0x%lx", label, i,
		       pr->s_unmapped[i].off, (unsigned long)pr->s_unmapped[i].value);
}

static void ss_run_probe(const char* why) {
	ss_map_t* maps = (ss_map_t*)malloc(sizeof(ss_map_t) * SS_MAP_MAX);
	if (!maps) return;
	int n_maps = ss_load_maps(maps, SS_MAP_MAX);
	if (n_maps <= 0) {
		free(maps);
		return;
	}

	ss_probe_rng_t set;
	memset(&set, 0, sizeof(set));
	/* First match wins, and libmain's whole span contains the data segment —
	 * register the specific ranges before the module span. */
	ss_probe_add_range(&set, ss.seg_base, ss.seg_size, "seg", SS_RNGK_SEG, -1);
	for (int i = 0; i < SS_POOL_COUNT; i++) {
		mem_t* p = ss.pools[i].pool;
		if (p)
			ss_probe_add_range(&set, (uintptr_t)p, sizeof(mem_t) + p->capacity,
			                   "pool", SS_RNGK_POOL, i);
	}
	ss_probe_add_range(&set, (uintptr_t)flocon_tab, ss.flocon_size,
	                   "flocon_tab", SS_RNGK_HEAP, -1);
	ss_probe_add_range(&set, (uintptr_t)rvb_special, ss.rvb_size,
	                   "rvb_special", SS_RNGK_HEAP, -1);
	/* One-shot heap blobs that hold engine state or lookup tables: for a
	 * cross-restart restore each of them needs its save-time address mapped
	 * onto this process' equivalent block. The probe counts who points there. */
	ss_probe_add_range(&set, (uintptr_t)EffetBufferNormal, 128000,
	                   "EffetBufferNormal", SS_RNGK_HEAP, -1);
	ss_probe_add_range(&set, (uintptr_t)DistPointX, 161 * sizeof(s32),
	                   "DistPointX", SS_RNGK_HEAP, -1);
	ss_probe_add_range(&set, (uintptr_t)DistPointY, 65 * sizeof(s32),
	                   "DistPointY", SS_RNGK_HEAP, -1);
	ss_probe_add_range(&set, (uintptr_t)ExpPoint, 5120 * sizeof(s32),
	                   "ExpPoint", SS_RNGK_HEAP, -1);
	ss_probe_add_range(&set, (uintptr_t)JumelleZoomDef, 29025 * sizeof(s32),
	                   "JumelleZoomDef", SS_RNGK_HEAP, -1);
	ss_probe_add_range(&set, (uintptr_t)Def_Bande, 18000 * sizeof(def_bande_t),
	                   "Def_Bande", SS_RNGK_HEAP, -1);

	/* libmain.so's other mappings (text/rodata/relro) as one span: those are
	 * the function pointers and static strings, and they all shift by the
	 * same module delta. */
	uintptr_t mod_lo = (uintptr_t)-1, mod_hi = 0;
	for (int i = 0; i < n_maps; i++) {
		if (strstr(maps[i].name, "libmain.so")) {
			if (maps[i].lo < mod_lo) mod_lo = maps[i].lo;
			if (maps[i].hi > mod_hi) mod_hi = maps[i].hi;
		}
	}
	if (mod_lo < mod_hi)
		ss_probe_add_range(&set, mod_lo, mod_hi - mod_lo, "libmain", SS_RNGK_MODULE, -1);

	ss_probe_t agg;
	memset(&agg, 0, sizeof(agg));
	SSLOGI("probe(%s): regions=%d seg=%p+%zuK stamp=%08x", why, set.n,
	       (void*)ss.seg_base, ss.seg_size / 1024, ss.stamp);
	{
		ss_probe_t one;
		memset(&one, 0, sizeof(one));
		ss_probe_buffer(&one, &set, maps, n_maps, ss.seg_copy, ss.seg_size);
		ss_probe_report("data-seg", &one, &set);
		ss_probe_add(&agg, &one);
		for (int i = 0; i < SS_POOL_COUNT; i++) {
			size_t n = 0;
			u8* buf = ss_region_payload(SS_RGN_POOL0 + i, &n);
			if (!n) continue;
			memset(&one, 0, sizeof(one));
			ss_probe_buffer(&one, &set, maps, n_maps, buf, n);
			char label[24];
			snprintf(label, sizeof(label), "pool%d", i);
			ss_probe_report(label, &one, &set);
			ss_probe_add(&agg, &one);
		}
		memset(&one, 0, sizeof(one));
		ss_probe_buffer(&one, &set, maps, n_maps, ss.flocon_copy, ss.flocon_size);
		ss_probe_report("flocon", &one, &set);
		ss_probe_add(&agg, &one);
		memset(&one, 0, sizeof(one));
		ss_probe_buffer(&one, &set, maps, n_maps, ss.rvb_copy, ss.rvb_size);
		ss_probe_report("rvb", &one, &set);
		ss_probe_add(&agg, &one);
	}
	ss_probe_report("TOTAL", &agg, &set);
	free(maps);
}

/* ---- save ---- */

/* Returns the status code the caller should publish. It deliberately does not
 * store into ss.status itself: the slot file is written after this returns, and
 * publishing "saved" before that let the UI's poll consume a success code that a
 * later write failure would contradict (seen on the emulator: the poll took the
 * 1 and the write was skipped). */
static int ss_do_save(void) {
	if (!ss_in_gameplay()) {
		SSLOGI("save rejected: not in gameplay (map=%d menu=%d gomenu=%d gele=%d)",
		       (int)RaymanDansUneMapDuJeu, (int)During_The_Menu, (int)GoMenu, (int)gele);
		return 2; /* rejected: not in a level */
	}
	if (!ss_alloc_buffers()) {
		SSLOGE("save failed: buffer allocation (seg=%zuK)", ss.seg_size / 1024);
		return 3; /* failed */
	}

	memcpy(ss.seg_copy, (void*)ss.seg_base, ss.seg_size);

	for (int i = 0; i < SS_POOL_COUNT; i++) {
		mem_t* raw = *ss_pool_slot(i);
		mem_t* p = ss_pool_ptr(i);
		if (raw && !p)
			SSLOGI("pool %d header implausible (len=%zu capacity=%zu) — treating as freed",
			       i, raw->len, raw->capacity);
		ss.pools[i].pool = p;
		ss.pools[i].base = p ? (uintptr_t)p : 0;
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

	if (flocon_tab) memcpy(ss.flocon_copy, flocon_tab, ss.flocon_size);
	if (rvb_special) memcpy(ss.rvb_copy, rvb_special, ss.rvb_size);
	ss.flocon_base = (uintptr_t)flocon_tab;
	ss.rvb_base = (uintptr_t)rvb_special;

	ss.aud_bnk_fixe = bnkDataFixe;
	ss.aud_bnk_world = bnkDataWorld;
	ss.aud_ptr_tchatch = ptrTchatch;
	ss.aud_decoder = ogg_cd_track.decoder;
	ss.aud_ogg_file = ogg_cd_track.file;

	ss.meta_num_world = num_world;
	ss.meta_num_level = num_level;
	ss.has_snapshot = 1;
	SSLOGI("captured: world=%d level=%d segment=%zuK stamp=%08x",
	       (int)num_world, (int)num_level, ss.seg_size / 1024, ss.stamp);

	if (ray_log_enabled(ANDROID_LOG_DEBUG, SS_TAG)) ss_run_probe("capture");
	return 1; /* saved */
}

/* ---- slot file I/O ---- */

static int ss_slot_path(int slot, char* out, size_t outlen, const char* suffix) {
	const char* dir = android_get_save_dir();
	if (slot < 0 || slot >= SS_MAX_SLOTS) return 0;
	if (!dir || !dir[0]) {
		SSLOGE("slot %d: save dir not set yet", slot);
		return 0;
	}
	snprintf(out, outlen, "%s/" SS_SLOT_FILE "%s", dir, slot, suffix ? suffix : "");
	return 1;
}

/* Capture is done; write the region table + payload. Written to a .tmp file
 * and renamed so an interrupted write can never clobber a good slot. */
static int ss_write_slot(int slot) {
	char path[1200], tmp[1200];
	if (!ss_slot_path(slot, tmp, sizeof(tmp), ".tmp")) return 0;
	if (!ss_slot_path(slot, path, sizeof(path), NULL)) {
		tmp[0] = '\0';
		return 0;
	}

	ss_rec_t rec[SS_RGN_COUNT];
	ss_build_records(rec);

	u64 payload = 0;
	u64 h = SS_FNV_OFFSET;
	for (int id = 0; id < SS_RGN_COUNT; id++) {
		payload += rec[id].size;
		size_t n = 0;
		u8* buf = ss_region_payload(id, &n);
		h = ss_hash_bytes_raw(h, buf, n);
	}

	FILE* f = fopen(tmp, "wb");
	if (!f) {
		SSLOGE("slot %d: cannot create %s: errno=%d (%s)", slot, tmp, errno, strerror(errno));
		return 0;
	}
	ss_file_hdr_t hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SS_MAGIC;
	hdr.fmt = SS_FMT_VERSION;
	hdr.stamp = ss.stamp;
	hdr.rec_count = SS_RGN_COUNT;
	hdr.world = ss.meta_num_world;
	hdr.level = ss.meta_num_level;
	hdr.created = (u64)time(NULL);
	hdr.payload_bytes = payload;
	hdr.payload_hash = h;
	hdr.aud[0] = (u64)(uintptr_t)ss.aud_bnk_fixe;
	hdr.aud[1] = (u64)(uintptr_t)ss.aud_bnk_world;
	hdr.aud[2] = (u64)(uintptr_t)ss.aud_ptr_tchatch;
	hdr.aud[3] = (u64)(uintptr_t)ss.aud_decoder;
	hdr.aud[4] = (u64)(uintptr_t)ss.aud_ogg_file;

	long long t0 = ss_now_ms();
	int ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1
	      && fwrite(rec, sizeof(rec[0]), SS_RGN_COUNT, f) == SS_RGN_COUNT;
	for (int id = 0; ok && id < SS_RGN_COUNT; id++) {
		size_t n = 0;
		u8* buf = ss_region_payload(id, &n);
		if (!n) continue;
		if (!buf) {
			SSLOGE("slot %d: region %d has size %llu but no buffer",
			       slot, id, (unsigned long long)n);
			ok = 0;
			break;
		}
		ok = fwrite(buf, 1, n, f) == n;
	}
	fclose(f);
	if (!ok) {
		SSLOGE("slot %d: write error errno=%d (%s)", slot, errno, strerror(errno));
		remove(tmp);
		return 0;
	}
	if (rename(tmp, path) != 0) {
		SSLOGE("slot %d: rename to %s failed: errno=%d (%s)",
		       slot, path, errno, strerror(errno));
		remove(tmp);
		return 0;
	}
	SSLOGI("slot %d written: world=%d level=%d payload=%lluK in %lldms",
	       slot, (int)hdr.world, (int)hdr.level,
	       (unsigned long long)(payload / 1024), ss_now_ms() - t0);
	return 1;
}

/* Read a slot file into this process' buffers. The engine state is only
 * touched afterwards, through ss_do_load(). Returns 1 on success and sets
 * ss.status itself on every rejection path. */
static int ss_read_slot(int slot) {
	char path[1200];
	if (!ss_slot_path(slot, path, sizeof(path), NULL)) {
		atomic_store(&ss.status, 9);
		return 0;
	}
	FILE* f = fopen(path, "rb");
	if (!f) {
		SSLOGI("slot %d: no snapshot file (%s)", slot, strerror(errno));
		atomic_store(&ss.status, 9);
		return 0;
	}
	ss_file_hdr_t hdr;
	ss_rec_t rec[SS_RGN_COUNT];
	int ok = fread(&hdr, sizeof(hdr), 1, f) == 1 && fread(rec, sizeof(rec[0]), SS_RGN_COUNT, f) == SS_RGN_COUNT;
	if (!ok) {
		SSLOGE("slot %d: short header", slot);
		fclose(f);
		atomic_store(&ss.status, 9);
		return 0;
	}
	if (hdr.magic != SS_MAGIC || hdr.fmt != SS_FMT_VERSION) {
		SSLOGE("slot %d: bad magic %08x or format %u", slot, hdr.magic, hdr.fmt);
		fclose(f);
		atomic_store(&ss.status, 9);
		return 0;
	}
	if (hdr.rec_count != SS_RGN_COUNT) {
		SSLOGE("slot %d: %u regions, expected %d", slot, hdr.rec_count, SS_RGN_COUNT);
		fclose(f);
		atomic_store(&ss.status, 9);
		return 0;
	}

	if (!ss_alloc_buffers()) {
		fclose(f);
		atomic_store(&ss.status, 3);
		return 0;
	}

	/* Every byte of the payload sits at a layout offset: a file written by a
	 * different build must be refused outright rather than relocated blind. */
	if (hdr.stamp != ss.stamp) {
		SSLOGE("slot %d: layout stamp %08x != this build %08x (different binary)",
		       slot, hdr.stamp, ss.stamp);
		fclose(f);
		atomic_store(&ss.status, 9);
		return 0;
	}

	/* Region sizes must match what this process actually has. Across a process
	 * restart the sizes still match (they are compile-time constants) — the
	 * addresses do not, and rec[].base keeps the information phase 2b's
	 * relocation pass needs to fix them up. */
	for (int id = 0; id < SS_RGN_COUNT; id++) {
		uintptr_t base;
		u64 size, len;
		s64 cursor;
		ss_region_live(id, &base, &size, &len, &cursor);
		if (size != rec[id].size) {
			SSLOGE("slot %d: region %d size %llu != local %llu",
			       slot, id, (unsigned long long)rec[id].size,
			       (unsigned long long)size);
			fclose(f);
			atomic_store(&ss.status, 10);
			return 0;
		}
	}

	u64 h = SS_FNV_OFFSET;
	long long t0 = ss_now_ms();
	for (int id = 0; ok && id < SS_RGN_COUNT; id++) {
		size_t n = 0;
		u8* buf = ss_region_payload(id, &n);
		if (!n) continue;
		ok = fread(buf, 1, n, f) == n;
		h = ss_hash_bytes_raw(h, buf, n);
	}
	fclose(f);
	if (!ok) {
		SSLOGE("slot %d: short payload", slot);
		atomic_store(&ss.status, 9);
		return 0;
	}
	if (h != hdr.payload_hash) {
		SSLOGE("slot %d: payload hash %016llx != %016llx (corrupt file)",
		       slot, (unsigned long long)h, (unsigned long long)hdr.payload_hash);
		atomic_store(&ss.status, 9);
		return 0;
	}

	/* Phase 2a stores raw absolute addresses, so a file written by another
	 * process cannot be restored here: ASLR moved every region, and memcpying
	 * those pointers back would hand the engine a dead layout. Reject it with a
	 * distinct code — phase 2b's relocation pass is what removes this gate. */
	for (int id = 0; id < SS_RGN_COUNT; id++) {
		uintptr_t live_base;
		u64 size, len;
		s64 cursor;
		if (!rec[id].base) continue;
		ss_region_live(id, &live_base, &size, &len, &cursor);
		if (live_base && live_base != rec[id].base) {
			SSLOGI("slot %d: region %s moved since capture (saved=%p live=%p delta=%td) "
			       "— cross-process restore needs phase 2b",
			       slot, ss_region_name(id), (void*)rec[id].base, (void*)live_base,
			       (ptrdiff_t)(live_base - rec[id].base));
			atomic_store(&ss.status, 10);
			return 0;
		}
	}

	ss.seg_file_base = rec[SS_RGN_SEG].base;
	for (int i = 0; i < SS_POOL_COUNT; i++) {
		int id = SS_RGN_POOL0 + i;
		/* Write back into this process' pool, but remember the address the
		 * snapshot's own pointers refer to. */
		ss.pools[i].base = rec[id].base;
		ss.pools[i].pool = ss_pool_ptr(i);
		ss.pools[i].len = (size_t)rec[id].len;
		ss.pools[i].cursor = (s32)rec[id].cursor;
	}
	ss.flocon_base = rec[SS_RGN_FLOCON].base;
	ss.rvb_base = rec[SS_RGN_RVB].base;
	ss.aud_bnk_fixe = (u8*)(uintptr_t)hdr.aud[0];
	ss.aud_bnk_world = (u8*)(uintptr_t)hdr.aud[1];
	ss.aud_ptr_tchatch = (u8*)(uintptr_t)hdr.aud[2];
	ss.aud_decoder = (stb_vorbis*)(uintptr_t)hdr.aud[3];
	ss.aud_ogg_file = (mem_t*)(uintptr_t)hdr.aud[4];
	ss.meta_num_world = (s16)hdr.world;
	ss.meta_num_level = (s16)hdr.level;
	ss.has_snapshot = 1;

	/* Every recorded address matched this process' layout, so the deltas are 0
	 * by construction (the check above rejected anything else). */
	SSLOGI("slot %d read: world=%d level=%d payload=%lluK in %lldms stamp=%08x",
	       slot, (int)hdr.world, (int)hdr.level,
	       (unsigned long long)(hdr.payload_bytes / 1024), ss_now_ms() - t0, hdr.stamp);
	return 1;
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

	if (flocon_tab) memcpy(flocon_tab, ss.flocon_copy, ss.flocon_size);
	if (rvb_special) memcpy(rvb_special, ss.rvb_copy, ss.rvb_size);

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
	int slot = atomic_exchange(&ss.request_slot, -1);
	SSLOGI("frame hook: op=%d slot=%d at frame boundary", op, slot);
	if (op == SS_OP_SAVE_TO_SLOT) {
		int st = ss_do_save();
		if (st == 1 && !ss_write_slot(slot)) st = 8; /* write failed */
		atomic_store(&ss.status, st);
	} else if (op == SS_OP_LOAD_FROM_SLOT) {
		if (!ss_in_gameplay()) {
			atomic_store(&ss.status, 6);
			SSLOGI("load rejected: not in gameplay (map=%d menu=%d gomenu=%d gele=%d)",
			       (int)RaymanDansUneMapDuJeu, (int)During_The_Menu, (int)GoMenu, (int)gele);
		} else if (ss_read_slot(slot)) {
			ss_do_load();
		}
	}
}

void savestate_request(int op, int slot) {
	SSLOGI("request: %s slot=%d", op == SS_OP_SAVE_TO_SLOT ? "save" : "load", slot);
	atomic_store(&ss.request_slot, slot);
	atomic_store(&ss.request, op);
}

int savestate_get_status(void) {
	return atomic_exchange(&ss.status, 0);
}

int savestate_has_snapshot(void) {
	return ss.has_snapshot;
}

int savestate_slot_info(int slot, int* world, int* level, long long* created, int* usable) {
	char path[1200];
	if (slot < 0 || slot >= SS_MAX_SLOTS) return -1;
	if (!ss_slot_path(slot, path, sizeof(path), NULL)) return -1;
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	ss_file_hdr_t hdr;
	int ok = fread(&hdr, sizeof(hdr), 1, f) == 1;
	fclose(f);
	if (!ok || hdr.magic != SS_MAGIC || hdr.fmt != SS_FMT_VERSION) return -1;
	if (world) *world = hdr.world;
	if (level) *level = hdr.level;
	if (created) *created = (long long)hdr.created;
	if (usable) {
		/* A file written by another build is still listed (so the UI can say
		 * "版本不符") but marked unusable. */
		*usable = (hdr.stamp == ss_compute_stamp()) ? 1 : 0;
	}
	return 1;
}

#endif /* ANDROID */
