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
 * Phase 2a: the state is described as a *region table* so it can
 *   be written to one of 3 slot files in the save dir and read back; plus a
 *   pointer-classification probe that counts what kind of absolute addresses
 *   the snapshot actually contains (docs/savestate-phase2.md).
 * Phase 2b (this file): a slot written by another process is restored too.
 *   ASLR has moved every address in it, so before the snapshot is applied
 *   every pointer-sized value in the payloads is translated from the saved
 *   region layout onto this process' one (ss_relocate_buffer).
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
#define SS_FMT_VERSION 2u             /* v2: region table carries ADDRESS_ONLY
                                       * entries and pool sizes are spans, so
                                       * v1 files are rejected by magic/fmt */
#define SS_STAMP_MISSING 0u

/* Regions are the addressable pieces of the snapshot. COPY regions carry a
 * payload in the file; ADDRESS_ONLY regions record only the block the state
 * points at (jumelle lookup tables, EffetBufferNormal, Def_Bande) so the
 * relocation pass can map saved references onto this process' blocks. */
enum {
	SS_RGN_SEG = 0,
	SS_RGN_POOL0 = 1,               /* +i for each arena pool */
	SS_RGN_FLOCON = SS_RGN_POOL0 + SS_POOL_COUNT,
	SS_RGN_RVB,
	SS_RGN_EFFET,                   /* EffetBufferNormal */
	SS_RGN_DISTX,                   /* DistPointX */
	SS_RGN_DISTY,                   /* DistPointY */
	SS_RGN_EXP,                     /* ExpPoint */
	SS_RGN_JZOOM,                   /* JumelleZoomDef */
	SS_RGN_BANDE,                   /* Def_Bande */
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
	u64 size;      /* address span covered by the region — for pools this is
	                * sizeof(mem_t) + capacity (the header plus the data the
	                * snapshot's pointers can address), not the payload bytes */
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

	/* DO NOT REMOVE / RESIZE: ss lives inside the snapshotted data segment,
	 * so sizeof(ss_ctx_t) is part of every engine global's offset that the
	 * linker places after it — and of the layout stamp written into slot
	 * files. These 24 bytes stand in for the removed armed-resume fields so
	 * slots written by the phase-2b build stay byte-layout compatible. */
	s64 reserved_layout[3];

	uintptr_t seg_base;
	size_t seg_size;
	u8* seg_copy;
	uintptr_t seg_file_base;   /* as recorded by the file we loaded */
	uintptr_t mod_lo, mod_hi;  /* libmain.so's full mapped span (live) */

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

	/* Per-region base as recorded by the file we loaded (0 = none): the
	 * relocation pass translates payload values from these onto the live
	 * bases. */
	u64 saved_base[SS_RGN_COUNT];

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
		if (ph->p_type != PT_LOAD) continue;
		uintptr_t lo = (uintptr_t)(info->dlpi_addr + ph->p_vaddr);
		uintptr_t hi = lo + ph->p_memsz;
		if (lo < ss.mod_lo) ss.mod_lo = lo;
		if (hi > ss.mod_hi) ss.mod_hi = hi;
		if ((ph->p_flags & PF_W) && ph->p_memsz > ss.seg_size) {
			ss.seg_base = lo;
			ss.seg_size = ph->p_memsz;
		}
	}
	return 1;
}

static int ss_locate_segment(void) {
	if (ss.seg_base) return 1;
	ss.mod_lo = (uintptr_t)-1;
	ss.mod_hi = 0;
	dl_iterate_phdr(ss_phdr_cb, NULL);
	if (!ss.seg_base) {
		SSLOGE("writable PT_LOAD of libmain.so not found");
		return 0;
	}
	SSLOGI("data segment base=%p size=%zuK module=%p..%p",
	       (void*)ss.seg_base, ss.seg_size / 1024, (void*)ss.mod_lo, (void*)ss.mod_hi);
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
	h = ss_hash_u64(h, (u64)sizeof(ss_ctx_t));
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

/* One-shot heap blocks that hold lookup tables or scratch screens: their
 * content is deterministic (or rebuilt every frame) but their addresses are
 * referenced from the state, so the file records where they were and the
 * relocation pass maps those references onto this process' block. */
static void ss_addr_only_block(int id, u8** ptr, size_t* size) {
	*ptr = NULL;
	*size = 0;
	switch (id) {
		case SS_RGN_EFFET: *ptr = EffetBufferNormal;     *size = 128000; break;
		case SS_RGN_DISTX: *ptr = (u8*)DistPointX;       *size = 161 * sizeof(s32); break;
		case SS_RGN_DISTY: *ptr = (u8*)DistPointY;       *size = 65 * sizeof(s32); break;
		case SS_RGN_EXP:   *ptr = (u8*)ExpPoint;         *size = 5120 * sizeof(s32); break;
		case SS_RGN_JZOOM: *ptr = (u8*)JumelleZoomDef;   *size = 29025 * sizeof(s32); break;
		case SS_RGN_BANDE: *ptr = (u8*)Def_Bande;        *size = 18000 * sizeof(def_bande_t); break;
		default: break;
	}
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
		/* The span includes the mem_t header: the engine's pool globals point
		 * at the header and block_malloc() can hand out the very last bytes of
		 * the data area, so every address in [p, p+sizeof(mem_t)+capacity) is
		 * a legitimate relocation target. */
		*base = (uintptr_t)p;
		*size = sizeof(mem_t) + p->capacity;
		*len = p->len;
		*cursor = p->cursor;
		return;
	}
	u8* blk;
	size_t blk_size;
	ss_addr_only_block(id, &blk, &blk_size);
	if (blk) {
		*base = (uintptr_t)blk;
		*size = blk_size;
	}
}

static const char* ss_region_name(int id) {
	static const char* const names[SS_RGN_COUNT] = {
		"seg", "pool0", "pool1", "pool2", "pool3", "pool4", "flocon", "rvb",
		"EffetBufferNormal", "DistPointX", "DistPointY", "ExpPoint",
		"JumelleZoomDef", "Def_Bande"
	};
	return (id >= 0 && id < SS_RGN_COUNT) ? names[id] : "?";
}

static int ss_region_kind(int id) {
	return (id >= SS_RGN_EFFET && id < SS_RGN_COUNT)
	     ? SS_KIND_ADDRESS_ONLY : SS_KIND_COPY;
}

static void ss_build_records(ss_rec_t* rec) {
	for (int id = 0; id < SS_RGN_COUNT; id++) {
		uintptr_t base;
		u64 size, len;
		s64 cursor;
		memset(&rec[id], 0, sizeof(rec[id]));
		rec[id].id = (u32)id;
		rec[id].kind = (u32)ss_region_kind(id);
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
/* bionic's allocator hands out TBI-tagged pointers (top byte 0xb4.. on the
 * Magic8 Pro) and ASLR can place libmain anywhere in the low VA range
 * (0x006d_8c51_8a80 for the data segment on one observed run), so neither
 * "real pointers are 0x0000_7xxx_xxxx_xxxx" nor "anything under 2^40 is
 * packed data" holds. What holds on every layout is that a pointer is
 * recognizable by its low 56 bits landing inside a known region window:
 * a shifted read of a real pointer sits a factor 256 away from every window,
 * and packed game data hits a ~15 MB window out of 2^56 by chance only.
 * The top byte is a memory tag, ignored by the CPU (TBI), and is re-applied
 * from the live region base after the translate. */
#define SS_PTR_TAG_MASK 0x00FFFFFFFFFFFFFFull
/* Values below this that match no region are packed game data, not pointers.
 * Counted apart so the "unknown mapping" buckets stay meaningful. */
#define SS_NOISE_FLOOR 0x10000ull

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
	/* Compare on the tag-stripped value: heap blocks carry TBI tags in both
	 * the window bases and the pointers stored into the snapshot. */
	u64 vm = (u64)v & SS_PTR_TAG_MASK;
	for (int i = 0; i < set->n; i++)
		if (vm >= ((u64)set->rng[i].lo & SS_PTR_TAG_MASK) &&
		    vm < (((u64)set->rng[i].lo & SS_PTR_TAG_MASK) + (set->rng[i].hi - set->rng[i].lo)))
			return i;
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
		/* A pointer into the buffer being scanned is a snapshot-internal
		 * reference: source and target move together across a restart, so its
		 * delta must stay 0. Counted apart from the region table on purpose —
		 * this is the bucket a naive relocation pass would silently corrupt. */
		if ((v & SS_PTR_TAG_MASK) >= ((uintptr_t)self_lo & SS_PTR_TAG_MASK) &&
		    (v & SS_PTR_TAG_MASK) < ((uintptr_t)self_hi & SS_PTR_TAG_MASK)) {
			pr->self++;
			continue;
		}
		int idx = ss_probe_classify(set, v);
		if (idx < 0) {
			/* No region claims its low 56 bits. A junk top byte then means two
			 * packed 32-bit game values rather than a pointer — on arm64
			 * Android the real thing is either tag-free or carries an 0x80..
			 * 0xbf allocator tag, and either way its address bits are mapped. */
			if (v >> 48) {
				pr->nonaddr++;
				continue;
			}
			const char* name = NULL;
			int rw = 0;
			if (ss_find_map(maps, n_maps, v & SS_PTR_TAG_MASK, &name, &rw) < 0) {
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

/* ---- pointer relocation (phase 2b) ----
 *
 * A slot file is raw bytes full of absolute addresses. Within one process
 * every one of them is still valid at load time; across a process restart
 * ASLR has moved the module, every malloc arena and every one-shot block,
 * so before the snapshot is applied each 8-byte value that looks like an
 * address is translated from the *saved* region layout onto this process'
 * one: v += (live_base - saved_base) of the region it points into.
 *
 * The targets are exactly the region table plus libmain's text/rodata span
 * (which shifts by the same module delta as the data segment). Pointers
 * *inside* the snapshot (global -> global, pool A -> pool B) need no special
 * case: they are translated by the delta of the region they point INTO, and
 * that is what makes e.g. the pool globals in the restored data segment line
 * up with this process' pools again. When two targets with *different*
 * deltas both claim a value it is left untouched and counted as ambiguous;
 * values that match no target are packed game data and are left alone too.
 *
 * The scan is byte-granular on purpose: obj_t is a packed 155-byte struct,
 * so inside an obj_t array 7 of 8 elements hold their pointer fields at
 * unaligned offsets (the phase 2a probe only saw the aligned 1/8). Matching
 * runs on the low 56 bits only: heap pointers carry an allocator TBI tag in
 * the top byte (0xb4.. on the Magic8 Pro) while module addresses carry none,
 * and ASLR can place either anywhere in the low VA range — libmain at
 * 0x006d_8c.. on one observed run, where the old "below 2^40 is packed data"
 * gate silently skipped every pointer and the first dereference of a restored
 * global segfaulted. A shifted read of a real pointer lands a factor 256 away
 * from every region window, and packed game data hits a ~15 MB window out of
 * 2^56 only by accident, so the window test is the filter; the translated
 * value is re-tagged with the live region's top byte (TBI ignores it, pointer
 * equality against live values does not). */

typedef struct {
	u64 lo, hi;     /* tag-stripped window */
	u64 delta;      /* tag-stripped (live - saved), mod 2^56 */
	u64 tag;        /* live base' top byte, re-applied after the shift */
	const char* name;
	u64 hits;
} ss_reloc_tgt_t;

#define SS_RELOC_MAX (SS_RGN_COUNT + 2)

static int ss_build_reloc_targets(ss_reloc_tgt_t* t, int max) {
	int nt = 0;
	u64 seg_delta = 0;
	if (ss.saved_base[SS_RGN_SEG] && ss.seg_base)
		seg_delta = ((u64)ss.seg_base - ss.saved_base[SS_RGN_SEG]) & SS_PTR_TAG_MASK;
	for (int id = 0; id < SS_RGN_COUNT && nt < max; id++) {
		uintptr_t live;
		u64 size, len;
		s64 cursor;
		u64 saved = ss.saved_base[id];
		ss_region_live(id, &live, &size, &len, &cursor);
		if (!saved || !live || !size) continue;
		t[nt].lo = saved & SS_PTR_TAG_MASK;
		t[nt].hi = t[nt].lo + size;
		t[nt].delta = ((u64)live - saved) & SS_PTR_TAG_MASK;
		t[nt].tag = (u64)live & ~SS_PTR_TAG_MASK;
		t[nt].name = ss_region_name(id);
		t[nt].hits = 0;
		nt++;
	}
	/* libmain.so's text/rodata/relro: the function pointers (command tables,
	 * collision callbacks, object behaviours) all live there and shift by the
	 * very same bias as the data segment. The span also contains the segment
	 * mapping itself — harmless, same delta, and the scanner treats equal
	 * deltas as one answer. Module addresses are tag-free, so this target
	 * re-tags to 0. */
	if (seg_delta && ss.mod_lo < ss.mod_hi && nt < max) {
		t[nt].lo = ((u64)ss.mod_lo - seg_delta) & SS_PTR_TAG_MASK;
		t[nt].hi = t[nt].lo + ((u64)ss.mod_hi - (u64)ss.mod_lo);
		t[nt].delta = seg_delta;
		t[nt].tag = 0;
		t[nt].name = "libmain";
		t[nt].hits = 0;
		nt++;
	}
	return nt;
}

static u64 ss_relocate_buffer(u8* buf, size_t size, ss_reloc_tgt_t* t, int nt, u64* ambiguous) {
	u64 fixed = 0;
	if (!buf || !size) return 0;
	for (size_t off = 0; off + 8 <= size; off++) {
		u64 v, vm;
		int m = -1, amb = 0;
		u64 delta = 0;
		memcpy(&v, buf + off, sizeof v);
		vm = v & SS_PTR_TAG_MASK;
		if (vm < SS_NOISE_FLOOR) continue;   /* packed game data */
		for (int i = 0; i < nt; i++) {
			if (vm < t[i].lo || vm >= t[i].hi) continue;
			if (m < 0) {
				m = i;
				delta = t[i].delta;
			} else if (t[i].delta != delta) {
				amb = 1;   /* two regions moving differently claim it */
				break;
			}
		}
		if (m < 0) continue;
		if (amb) {
			(*ambiguous)++;
			continue;
		}
		vm = ((vm + delta) & SS_PTR_TAG_MASK) | t[m].tag;
		memcpy(buf + off, &vm, sizeof vm);
		t[m].hits++;
		fixed++;
	}
	return fixed;
}

/* Translate every payload buffer onto this process' layout. No-op when all
 * deltas are zero (same process). Called from ss_read_slot() after the file
 * hash verified and before the snapshot can be applied. */
static void ss_relocate_snapshot(void) {
	ss_reloc_tgt_t t[SS_RELOC_MAX];
	int nt = ss_build_reloc_targets(t, SS_RELOC_MAX);
	int moving = 0;
	for (int i = 0; i < nt; i++)
		if (t[i].delta) moving = 1;
	if (!moving) {
		SSLOGI("relocate: layout identical to capture, nothing to fix");
		return;
	}
	long long t0 = ss_now_ms();
	u64 ambiguous = 0, fixed = 0;
	for (int id = 0; id < SS_RGN_COUNT; id++) {
		size_t n = 0;
		u8* buf = ss_region_payload(id, &n);
		if (!buf || !n) continue;
		fixed += ss_relocate_buffer(buf, n, t, nt, &ambiguous);
	}
	SSLOGI("relocate: fixed=%llu ambiguous=%llu targets=%d in %lldms",
	       (unsigned long long)fixed, (unsigned long long)ambiguous, nt, ss_now_ms() - t0);
	for (int i = 0; i < nt; i++)
		SSLOGI("relocate: %s win=%llx..%llx delta=%llx tag=%02llx hits=%llu", t[i].name,
		       (unsigned long long)t[i].lo, (unsigned long long)t[i].hi,
		       (unsigned long long)t[i].delta,
		       (unsigned long long)(t[i].tag >> 56),
		       (unsigned long long)t[i].hits);
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
		size_t n = 0;
		u8* buf = ss_region_payload(id, &n);
		if (!n) continue;
		payload += n;
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

	/* Phase 2b: raw absolute addresses only stay valid inside the process
	 * that wrote them. Record each region's saved base and translate every
	 * pointer-sized value in the payloads onto this process' layout — for a
	 * same-process load every delta is zero and the pass is a no-op. A region
	 * with no live counterpart was already rejected by the size check above. */
	for (int id = 0; id < SS_RGN_COUNT; id++)
		ss.saved_base[id] = rec[id].base;
	ss_relocate_snapshot();

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

	/* The payloads now hold this process' addresses, so ss_do_load() can
	 * apply them without further address work. */
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

	SSLOGI("load step: audio state captured");
	stop_all_snd();
	SSLOGI("load step: stop_all_snd #1 done");
	SDL_ClearQueuedAudio(global_app_state.sdl.sound_output.audio_device);
	SSLOGI("load step: ClearQueuedAudio done");

	app_state_t* app_backup = (app_state_t*)malloc(sizeof(app_state_t));
	ss_ctx_t* ss_backup = (ss_ctx_t*)malloc(sizeof(ss_ctx_t));
	/* android_jni.c's own statics (log ring pointer, fd table, dir strings):
	 * the segment restore below would roll them back to the writing
	 * process' values. Taken after the slot read (file IO is done) and
	 * before the memcpy — nothing in between touches them. */
	void* plat_backup = android_plat_state_save();
	/* See the keep-live scan below. */
	size_t seg_slots = ss.seg_size / 8;
	u64* code_keep_val = (u64*)malloc(seg_slots * sizeof(u64));
	u8* code_keep_mask = (u8*)calloc(seg_slots, 1);
	if (!app_backup || !ss_backup || !plat_backup || !code_keep_val || !code_keep_mask) {
		free(app_backup);
		free(ss_backup);
		free(plat_backup);
		free(code_keep_val);
		free(code_keep_mask);
		atomic_store(&ss.status, 3); /* failed */
		SSLOGE("load failed: backup allocation");
		return;
	}
	memcpy(app_backup, &global_app_state, sizeof(app_state_t));
	memcpy(ss_backup, &ss, sizeof(ss_ctx_t));
	SSLOGI("load step: backups done (app=%zu ss=%zu seg_slots=%zu)",
	       sizeof(app_state_t), sizeof(ss_ctx_t), seg_slots);

	/* Code/rodata references (function pointers in ObjectsFonctions/cptr_tab,
	 * the draw-dispatch, input-test and screen-init globals, the
	 * key_descriptions string tables) are baked to the binary's own code
	 * layout. The module delta of the relocation pass fixes ASLR, but it
	 * cannot fix a code shift between builds: a slot written by the previous
	 * build carries the OLD function offsets, and translating them landed
	 * mid-function of unrelated code (seen as a crash in the object script
	 * engine, returning into &global_app_state). The pools hold zero module
	 * pointers — every obj_t reference is seg/pool/heap — so this scan covers
	 * the whole surface. Each slot whose value is a code/rodata reference in
	 * either the live segment or the snapshot keeps this process' value:
	 * constant tables stay correct for this binary, and the runtime-assigned
	 * ones (draw fns, TestKey*, anim_func, pINIT_*) keep this process'
	 * input config and screen mode — neither is game state. */
	{
		u64 code_lo = (u64)ss.mod_lo, code_hi = (u64)ss.seg_base;
		const u64* live_seg = (const u64*)ss_backup->seg_base;
		const u64* snap_seg = (const u64*)ss_backup->seg_copy;
		for (size_t i = 0; i < seg_slots; i++) {
			u64 a = live_seg[i], b = snap_seg[i];
			if ((a >= code_lo && a < code_hi) || (b >= code_lo && b < code_hi)) {
				code_keep_mask[i] = 1;
				code_keep_val[i] = a;
			}
		}
		SSLOGI("load step: keep-live scan done");
	}

	/* Whole-segment restore, then put the platform-owned bytes back. The
	 * android_jni.c statics go back FIRST — before this function logs
	 * anything — or the very next SSLOGI would hit the rolled-back
	 * (cross-process: dangling) log-ring pointer. */
	memcpy((void*)ss_backup->seg_base, ss_backup->seg_copy, ss_backup->seg_size);
	android_plat_state_restore(plat_backup);
	SSLOGI("load step: segment restored");
	memcpy(&global_app_state, app_backup, sizeof(app_state_t));
	memcpy(&ss, ss_backup, sizeof(ss_ctx_t));
	SSLOGI("load step: platform state restored");
	{
		u64* out = (u64*)ss_backup->seg_base;
		u64 kept = 0;
		for (size_t i = 0; i < seg_slots; i++)
			if (code_keep_mask[i]) { out[i] = code_keep_val[i]; kept++; }
		SSLOGI("load: kept %llu code/rodata references live", (unsigned long long)kept);
	}
	free(code_keep_val);
	free(code_keep_mask);
	free(app_backup);
	free(ss_backup);
	free(plat_backup);

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
	SSLOGI("load step: pools/flocon/rvb restored");

	/* draw_palette (the palette the frame blit actually uses) lives in
	 * global_app_state, which is platform-owned and excluded from the
	 * restore, and gameplay only refreshes it through SetPalette() on
	 * palette transitions — not per frame. Re-upload the snapshot's current
	 * palette or the restored pixels are shown through whatever palette the
	 * previous level left behind until the next level load. */
	SetPalette(&current_rvb, 0, 256);
	SSLOGI("load step: SetPalette done");

	/* If the sound bank or the music stream was reloaded since the save, the
	 * restored pointer is dangling: keep the live object and reload the bank
	 * that belongs to the restored world. */
	SSLOGI("load step: audio fixup start live: bnkF=%p bnkW=%p tch=%p dec=%p file=%p | snap: bnkF=%p bnkW=%p tch=%p dec=%p file=%p",
	       (void*)live_bnk_fixe, (void*)live_bnk_world, (void*)live_tchatch,
	       (void*)live_ogg.decoder, (void*)live_ogg.file,
	       (void*)ss.aud_bnk_fixe, (void*)ss.aud_bnk_world, (void*)ss.aud_ptr_tchatch,
	       (void*)ss.aud_decoder, (void*)ss.aud_ogg_file);
	if (live_bnk_fixe != ss.aud_bnk_fixe) bnkDataFixe = live_bnk_fixe;
	if (live_tchatch != ss.aud_ptr_tchatch) ptrTchatch = live_tchatch;
	if (live_bnk_world != ss.aud_bnk_world) {
		bnkDataWorld = live_bnk_world;
		SSLOGI("load step: LoadBnkWorld(%d) begin", (int)num_world_choice);
		LoadBnkWorld(num_world_choice);
		SSLOGI("load step: LoadBnkWorld end");
	}
	if (live_ogg.decoder != ss.aud_decoder || live_ogg.file != ss.aud_ogg_file) {
		SSLOGI("load step: ogg fixup (live dec=%p file=%p)", (void*)live_ogg.decoder, (void*)live_ogg.file);
		ogg_cd_track = live_ogg;
		if (!ogg_cd_track.decoder) is_ogg_playing = false;
	}
	/* The restored voices may still point into the sound banks as they were
	 * at capture (snd_t::data into bnkDataFixe/bnkDataWorld); across a
	 * restart those buffers are gone, so silence everything — the next sound
	 * resolves its sample through the live bank again. Also sync the second
	 * pointer to the tchatch buffer, which the segment restore rewound. */
	stop_all_snd();
	SSLOGI("load step: stop_all_snd #2 done");
	if (ptrTchatch && TchachPerduPtr == ss.aud_ptr_tchatch)
		TchachPerduPtr = ptrTchatch;

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
