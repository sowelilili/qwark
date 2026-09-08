/*
 * The per-game vtable. One C file per game fills one of these in; the core never
 * knows an address, a patch word or a planet name.
 *
 * Every entry point returns a status code from proto.h (ST_OK is 0). An entry
 * that is NULL means the game does not offer the feature and the core answers
 * UNSUPPORTED without calling anything.
 */
#ifndef QWARK_GAME_H
#define QWARK_GAME_H

#include "../plat/plat.h"
#include "../core/proto.h"

/*
 * One contiguous read the tick thread performs for the game.
 *
 * `period` is how often it is read, in ticks: 0 or 1 every tick, 8 every eighth.
 * `phase` staggers the slow blocks so no single tick pays for all of them. The
 * buffer keeps the last bytes read in between, so hot_decode always sees a full
 * set; the core re-reads every block once on entering INGAME.
 */
struct game_hot_block {
	u32 addr;
	u16 len;
	u8  period;
	u8  phase;
};

/* What hot_decode fills in from those blocks. */
struct game_hot {
	u32 pad_mask;
	f32 analog[4];      /* rx, ry, lx, ly */
	u8  current_planet;
	f32 pos[3];
	u32 readout[QWARK_MAX_READOUTS];
};

/* One row of the DESCRIBE table. */
struct feature_desc {
	u8  id;
	u8  kind;       /* FEATURE_TOGGLE, ACTION, VALUE, ENUM, COLOR */
	u8  group;      /* index into the group name table */
	u8  aux;        /* ENUM: option count. 0 for every other kind */
	u8  flags;      /* FEATURE_FLAG_WRITES_CODE; AUTO is added by the core */
	u8  readout;    /* VALUE, ENUM, COLOR: mirroring readout index, else 0xFF */
	u32 min;
	u32 max;
	const char *label;
};

struct game_describe {
	const char * const *groups;
	u8 ngroups;
	const char * const *readouts;
	u8 nreadouts;
	const struct feature_desc *features;
	u8 nfeatures;
	/*
	 * Bit i set = TOGGLE feature id i is auto-flagged unless config says
	 * otherwise. Deadlocked's crash patches and softlock fix want to come back
	 * on their own the way the DL autosplitter SPRX applies them today; every
	 * other toggle defaults to 0, which is what a zero here means.
	 */
	u64 auto_default;
};

/* One row of UNLOCK_LIST. Values are read live through unlock_read. */
struct game_unlock {
	u8 id;
	u8 category;
	u8 fields;      /* bit f set = field f is meaningful */
	const char *name;
};

struct game_api {
	/* Up to four title ids, NULL-terminated. */
	const char *title_ids[4];
	u8 game_id;                     /* GAME_RAC1 .. GAME_RAC4 */

	/*
	 * Proof the executable is mapped: fp_len bytes at fp_addr must equal
	 * fp_bytes, or fp_alt when that is not NULL. The alternative exists so a
	 * console where another tool already patched the fingerprint site does not
	 * stall in BOOTING for ever.
	 */
	u32 fp_addr;
	const u8 *fp_bytes;
	const u8 *fp_alt;
	u16 fp_len;

	/*
	 * A game-side byte that goes non-zero when the player asked to quit, or 0
	 * when no such hook is known. Without one the session falls back to
	 * IS_INGAME plus the PID check.
	 */
	u32 quit_hook_addr;

	/*
	 * Where the controller mask lives. The core reads it out of the hot block,
	 * but the host simulator needs the address on its own so its `pad` command
	 * can drive combos.
	 */
	u32 pad_mask_addr;

	/* Called once, the first time the registry hands this game out. */
	void (*init)(void);

	const struct game_hot_block *hot;
	u8 nhot;
	void (*hot_decode)(const u8 * const *blocks, struct game_hot *out);

	const struct game_describe *(*describe)(void);

	int (*set_toggle)(u8 id, int on);
	int (*trigger)(u8 id);
	int (*set_value)(u8 id, u32 value);
	int (*get_options)(u8 id, const char * const **options, u8 *count);

	/* Position slots. The blob is opaque to the core, at most QWARK_MAX_BLOB. */
	int (*save_blob)(u8 *blob, u8 *len);
	int (*load_blob)(const u8 *blob, u8 len);
	int (*blob_xyz)(const u8 *blob, u8 len, f32 out[3]);

	const char * const *(*planet_names)(u8 *count);
	int (*planet_load)(u8 planet, u8 flags);
	int (*die)(void);

	/* COMBO_LOAD_SETASIDE. NULL where the game has no savefile helper. */
	int (*load_setaside)(void);

	/*
	 * unlock_list may snapshot the live values so that unlock_read can serve
	 * every row from a couple of reads. The core calls the two together on the
	 * tick thread under the core lock, so the snapshot is never stale.
	 */
	int (*unlock_list)(const struct game_unlock **list, u8 *count,
	                   const char * const **categories, u8 *ncategories);
	int (*unlock_read)(const struct game_unlock *entry, u32 values[4]);
	int (*unlock_set)(u8 id, u8 field, u32 value);

	int (*levelflags_get)(u8 planet, u8 *out, u16 cap, u16 *len);
	int (*levelflags_reset)(u8 planet);
	/* `offset` indexes the concatenated region levelflags_get returns. */
	int (*levelflags_set)(u8 planet, u16 offset, u8 value);

	int (*moby_table)(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride);

	void (*on_enter)(void);
	void (*on_quit)(void);

	/*
	 * Called every tick while INGAME, right after hot_decode. Game-side
	 * watchers live here: RaC2's loading-screen watcher, RaC3's delayed
	 * fast-load arm, Deadlocked's softlock fix and fast-load restore. They read
	 * the decoded hot block rather than memory; a write only goes out on the
	 * tick the watcher actually fires.
	 */
	void (*on_tick)(const struct game_hot *hot);
};

/*
 * The registry. Returns the first game registered for the title, or NULL for a
 * title qwark does not know. BCES01503 hosts three of them, so BOOTING asks for
 * the candidate list instead and fingerprints each one.
 */
const struct game_api *game_for_title(const char *title_id);

/*
 * Every game registered for `title_id`, in registration order, at most `cap`.
 * Returns how many were written. Each candidate has had its init() run.
 */
u32 game_candidates_for_title(const char *title_id, const struct game_api **out, u32 cap);

/* At most this many games share one title id (BCES01503 hosts RaC1..RaC3). */
#define GAME_MAX_CANDIDATES 4

/* Walk every registered game, for the host simulator's fingerprint seeding. */
const struct game_api *game_at(u32 index);

#endif /* QWARK_GAME_H */
