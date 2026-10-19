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

/*
 * One row of the DESCRIBE table.
 *
 * `aux` is the kind-dependent extra, and the two kinds that use it never share a
 * row: an ENUM's option count goes out in the wire's `aux` byte, a VALUE's field
 * width in the `bits` byte revision 1.7 took from the padding. The core sorts
 * that out in features_describe; a row only ever writes the one number its kind
 * has a use for.
 */
struct feature_desc {
	u8  id;
	u8  kind;       /* FEATURE_TOGGLE, ACTION, VALUE, ENUM, COLOR */
	u8  group;      /* index into the group name table */
	/*
	 * ENUM: the option count. VALUE: the width in bits of the field behind it,
	 * 8, 16 or 32, and 0 for the default 32 (protocol 1.7). A row flagged
	 * FEATURE_FLAG_SIGNED says the width here, because that is what a client
	 * needs in order to read the value back. 0 for every other kind.
	 */
	u8  aux;
	u8  flags;      /* FEATURE_FLAG_WRITES_CODE, SIGNED; AUTO is added by the core */
	u8  readout;    /* VALUE, ENUM, COLOR: mirroring readout index; ACTION (1.21): status readout; else 0xFF */
	u32 min;
	u32 max;
	const char *label;
};

/*
 * Build 53: a TOGGLE whose label changed, and the label it had. config.txt
 * keeps a toggle's auto flag under its label (config.h, feature_key), so a
 * relabelled toggle would otherwise forget the box its user ticked:
 * features_auto_mask reads the flag under the old label until one has been
 * written under the new.
 */
struct feature_former {
	u8 id;
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
	/* Build 53: the toggles that were relabelled, NULL and 0 for none. */
	const struct feature_former *former;
	u8 nformer;
};

/* One row of UNLOCK_LIST. Values are read live through unlock_read. */
struct game_unlock {
	u8 id;
	u8 category;
	u8 fields;      /* bit f set = field f is meaningful */
	const char *name;
};

/*
 * Protocol 1.3. One of UNLOCK_LIST's four value slots, as this game uses it.
 * Every game returns exactly four, in slot order; a slot the game never touches
 * has a NULL or empty name and kind UNLOCK_KIND_FLAG. `max` bounds a number
 * slot game-wide (RaC3's weapon level is 8, even though the R3YNO stops at 5),
 * and 0 means the slot has no meaningful upper bound.
 */
struct unlock_field_desc {
	const char *name;   /* at most UNLOCK_FIELD_NAME_LEN characters on the wire */
	u8 kind;            /* UNLOCK_KIND_FLAG or UNLOCK_KIND_NUMBER */
	u8 max;
};

/*
 * Protocol 1.5. One row of AUTOSPLIT_DESCRIBE: a reason code the game's watcher
 * can emit, what a client shows beside its checkbox, and the game-time
 * adjustment the old ASL script made at that point. The table is static per game
 * and never depends on the session. Code 1 is "planet entered" in every game and
 * is the only row that carries AUTOSPLIT_FLAG_ROUTE.
 *
 * A load row's `kind` is LOAD_START and its LOAD_END carries the same code; a
 * pause row's `kind` is PAUSE and its RESUME carries the same code.
 */
struct autosplit_desc {
	u8  code;           /* 1..255, the Event.code this row describes */
	u8  kind;           /* the Event.kind it is emitted with */
	u8  flags;          /* AUTOSPLIT_FLAG_DEFAULT, ROUTE, FLAT, NORMALISE */
	u32 param_us;       /* the FLAT subtraction, or the NORMALISE target */
	const char *label;  /* at most AUTOSPLIT_LABEL_LEN characters on the wire */
};

/*
 * Revision 1.21, build 53: manip categories. A game that has them describes
 * them in a struct game_manips, which src/core/manips.c drives; the game file
 * holds every address and value, the core holds none.
 *
 * One write is a byte, a big-endian word or a run of bytes, at one address.
 * A u32 at an odd address writes the three bytes after it too, exactly as the
 * old buttons did. A run with no bytes writes zeros.
 */
#define MANIP_W_U8    0
#define MANIP_W_U32   1
#define MANIP_W_BYTES 2

struct manip_write {
	u32 addr;
	u32 value;          /* MANIP_W_U8 and MANIP_W_U32 */
	const u8 *bytes;    /* MANIP_W_BYTES: `len` bytes, or NULL for zeros */
	u8  kind;
	u8  len;            /* MANIP_W_BYTES: how many */
};

#define MW_U8(a, v)       { (a), (v), NULL, MANIP_W_U8, 1 }
#define MW_U32(a, v)      { (a), (v), NULL, MANIP_W_U32, 4 }
#define MW_ZEROS(a, n)    { (a), 0, NULL, MANIP_W_BYTES, (n) }
#define MW_BYTES(a, p, n) { (a), 0, (p), MANIP_W_BYTES, (n) }

/* A run of writes, applied in order. A group's lists are applied in order too. */
struct manip_list {
	const struct manip_write *w;
	u8 n;
};

#define MANIP_LISTS 2

/*
 * One group. `id` is 1..31 and stable, 0 being None. `flags` is the wire's
 * MANIP_FLAG_NGPLUS, EXTENSION and NO_CHALLENGE; PER_RUN is worked out from
 * `run`. `cats` is the leaderboard categories, joined with '\n'.
 *
 * `setup` is what the PS3 port never resets until the game reboots, applied
 * once per game boot; `run` is put back on every file load while auto-reset
 * is on. An unused list has n 0.
 */
struct manip_group {
	u8 id;
	u8 flags;
	const char *name;
	const char *cats;
	struct manip_list setup[MANIP_LISTS];
	struct manip_list run[MANIP_LISTS];
};

/*
 * One of MANIP_LIST's headers: features that are not part of a category but
 * belong beside the manips, which a client draws under `name` there instead of
 * in their DESCRIBE groups. Deadlocked's "Fixes", UYA's "Trophies".
 */
struct manip_header {
	const char *name;
	const u8 *features;
	u8 nfeatures;
};

struct game_manips {
	/* The config.txt key of the game's options: "rac1" .. "rac4". */
	const char *key;

	const struct manip_group *groups;
	u8 ngroups;

	/*
	 * The challenge-mode counter: 0 on a fresh file, 1 and up in challenge
	 * mode. `challenge_width` is 1 or 4, big-endian.
	 */
	u32 challenge_addr;
	u8  challenge_width;

	/* MANIP_LIST's headers, NULL and 0 where the game has none. */
	const struct manip_header *headers;
	u8 nheaders;

	/*
	 * Whether a save file is in play right now: 1 in a file (play, the pause,
	 * a cutscene, a load between planets), 0 on the title screen and the menus
	 * before one is chosen, negative when the read failed. Tick thread only.
	 */
	int (*in_file)(void);
};

/*
 * Revision 1.22, round 2: what the console menu (src/core/menu.c) needs to
 * know about a game beyond its DESCRIBE table. A game without one gets round
 * 1's menu, a page per DESCRIBE group. How the menu is cased is the game's
 * renderer's business (src/games/menuhelper/), not this table's.
 *
 * A section is a page of rows the game lists in order: its own features by
 * id, and the position rows, which are not features, by the MENU_ITEM_*
 * numbers below (no feature id reaches them).
 */
#define MENU_ITEM_SLOT      0xF0   /* the position slot Save and Load use */
#define MENU_ITEM_POS_SAVE  0xF1
#define MENU_ITEM_POS_LOAD  0xF2

struct menu_section {
	const char *title;
	const u8 *items;
	u8 nitems;
};

struct game_menu {
	/* The pages the root links to first, in order (Game, Cheats). */
	const struct menu_section *sections;
	u8 nsections;

	/* Actions listed on the Unlocks page under its categories. */
	const u8 *unlock_actions;
	u8 nunlock_actions;

	/*
	 * The values an unlock row cycles through, for the rows that declare
	 * `unlock_field`: names[0] is the locked one, and names[i] owns the item
	 * with `unlock_field` set to values[i]. A field value that is not on the
	 * list shows as `unlock_prefix` and the number, or as the name below it
	 * when there is no prefix. Rows without the field are LOCKED or UNLOCKED.
	 */
	const char * const *unlock_names;
	const u32 *unlock_values;
	u8 nunlock;
	u8 unlock_field;
	const char *unlock_prefix;

	/*
	 * 1 when the planet may be loaded from the menu, NULL for every named
	 * one. Deadlocked crashes loading a planet the save has not unlocked.
	 * Tick thread: it may read the game.
	 */
	int (*planet_ok)(u8 planet);

	/*
	 * UYA (the renderer's round): the item's own last value of `unlock_field`,
	 * where it stops short of the set's last (the R3YNO stops at V5 where the
	 * other weapons go to V8), or 0 for the set's own. The set's last name
	 * then stands for that value, so MAX is each item's maximum. NULL where
	 * every item goes as far as the set.
	 */
	u32 (*unlock_top)(u8 id);

	/*
	 * UYA: an ACTION's status readout (PROTOCOL.md 5.3) as a short text for the
	 * menu, whose layout only the game knows (UYA's Reset trophies, section
	 * 5.14). The Manips page puts a row of it under each header ACTION that
	 * names one, labelled with the readout's name; "" says nothing. NULL where
	 * the game has no such action.
	 */
	void (*action_status)(u8 id, u32 word, char *out, u32 cap);

	/*
	 * Round 3: where the Unlocks page's actions go. NULL lists them on the
	 * Unlocks page under its categories, as buttons (Deadlocked's two weapon
	 * buttons); a name moves them all onto a CONFIG page of that name, reached
	 * from one row after the categories, so their long labels sit in its label
	 * column (UYA's six, "Collectables", the client's Unlocks tab for the skill
	 * point and titanium bolt ones). RaC1 and RaC2 do the same with theirs.
	 */
	const char *unlock_actions_title;

	/*
	 * The planet pickers' round 2: the planets the game's own planet picker
	 * cannot show, which the player asks it for (mailbox event
	 * MBOX_EV_PICKER_OTHERS: UYA's GALACTIC MAP has no vid comics, and its
	 * Square asks for them): a CONFIG page of that title with these planets
	 * (PLANET_LIST ids, as `items`), in order, each a button that loads it as
	 * the Planets page's do. NULL where the picker shows every planet, or a
	 * game has none; the event then opens the whole Planets page.
	 */
	const struct menu_section *picker_others;
};

/* Build 53: what struct game_api's savefile_ready is asked about. */
#define GAME_SF_SET_ASIDE 0
#define GAME_SF_LOAD      1
#define GAME_SF_RESTORE   2

struct game_api {
	/* Up to four title ids, NULL-terminated. */
	const char *title_ids[4];
	u8 game_id;                     /* GAME_RAC1 .. GAME_RAC4 */

	/*
	 * Proof the executable is mapped: fp_len bytes at fp_addr must equal
	 * fp_bytes, or fp_alt when that is not NULL. The alternative exists so a
	 * console where another tool already patched the fingerprint site does not
	 * stall in BOOTING for ever.
	 *
	 * Build 50: or fp_switch when that is not NULL. Three games' fingerprints
	 * are instructions their code switches replace (src/games/switches/), so
	 * under RPCS3 with the switch patch in, the site holds the branch into the
	 * trampoline instead, and that proves the executable just as well. RaC1's
	 * switch leaves its fingerprint alone and it has none. A console never
	 * has the switch patch, so the games name it through GAME_FP_SWITCH below
	 * and on the PS3 it is NULL (build 51).
	 *
	 * Build 53: or fp_switch_old when that is not NULL, the branch an earlier
	 * build's switch patch put at the site before this build moved the
	 * trampoline it goes to. A game with that patch is still the game; its
	 * switches are from another build, which the switches' own look says
	 * (src/core/switches.h), and the client offers to write the patch again.
	 * Named through GAME_FP_SWITCH as well.
	 */
	u32 fp_addr;
	const u8 *fp_bytes;
	const u8 *fp_alt;
	const u8 *fp_switch;
	const u8 *fp_switch_old;
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
	/*
	 * Protocol 1.3, FEATURE_FLAG_LIVE. Reads the game byte behind TOGGLE `id`
	 * and reports whether it is on, so the core can make toggle_state agree
	 * with memory instead of with the last thing it wrote. NULL where a game
	 * has no live toggle; an id that is not one is NOT_FOUND. Tick thread only.
	 */
	int (*toggle_read)(u8 id, int *on);
	int (*trigger)(u8 id);
	int (*set_value)(u8 id, u32 value);
	int (*get_options)(u8 id, const char * const **options, u8 *count);

	/* Position slots. The blob is opaque to the core, at most QWARK_MAX_BLOB. */
	int (*save_blob)(u8 *blob, u8 *len);
	int (*load_blob)(const u8 *blob, u8 len);
	int (*blob_xyz)(const u8 *blob, u8 len, f32 out[3]);
	/*
	 * Protocol 1.13, POS_EDIT: put x, y and z back into a stored blob and leave
	 * every other byte of it, rotation and camera included, exactly as it was.
	 * The inverse of blob_xyz and the only writer the core has for a slot it is
	 * not saving from the live game.
	 */
	int (*blob_set_xyz)(u8 *blob, u8 len, const f32 xyz[3]);
	/*
	 * Protocol 1.13, POS_STORE: the length save_blob produces for this game, so
	 * the core can refuse a whole blob of any other shape rather than store one
	 * load_blob would misread. 0 where the game has no position slots.
	 */
	u8 blob_len;

	const char * const *(*planet_names)(u8 *count);
	int (*planet_load)(u8 planet, u8 flags);
	int (*die)(void);

	/* COMBO_LOAD_SETASIDE. NULL where the game has no savefile helper. */
	int (*load_setaside)(void);

	/*
	 * Build 43. Whether the game is somewhere a save may be copied aside or
	 * handed back right now. src/core/savefile.c asks before it raises a
	 * set-aside or a load, and refuses with BUSY rather than start work the game
	 * will walk over.
	 *
	 * Build 53: `what` is GAME_SF_SET_ASIDE, GAME_SF_LOAD or GAME_SF_RESTORE
	 * (a library RESTORE, asked before it copies the file into the aside buffer,
	 * which is then asked again as a GAME_SF_LOAD), and the answer is
	 * SAVEFILE_ERR_NONE (0) when the work may start, otherwise the
	 * SAVEFILE_ERR_* code the client is told: SAVEFILE_ERR_NOT_IDLE when the
	 * game is busy, SAVEFILE_ERR_NO_SAVE when there is no save to act on.
	 *
	 * NULL where a game has no such notion, and a NULL reads as "always ready";
	 * all four games answer since build 53. Tick thread only: it reads game
	 * memory.
	 */
	u8 (*savefile_ready)(u8 what);

	/*
	 * unlock_list may snapshot the live values so that unlock_read can serve
	 * every row from a couple of reads. The core calls the two together on the
	 * tick thread under the core lock, so the snapshot is never stale.
	 *
	 * `fields` comes back pointing at four unlock_field_desc, in slot order.
	 */
	int (*unlock_list)(const struct game_unlock **list, u8 *count,
	                   const char * const **categories, u8 *ncategories,
	                   const struct unlock_field_desc **fields);
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
	 *
	 * Protocol 1.4: the autosplit watchers live here too, and call
	 * autosplit_emit() for every run event they see.
	 */
	void (*on_tick)(const struct game_hot *hot);

	/*
	 * Protocol 1.5. The reason codes this game's watcher can emit, in the
	 * order a client should list them. NULL where the game has no watcher, which
	 * is what makes AUTOSPLIT_DESCRIBE answer UNSUPPORTED. Static: it never
	 * depends on the session, so a network thread may call it under the core
	 * lock without going through the ring.
	 */
	const struct autosplit_desc *(*autosplit_describe)(u8 *count);

	/*
	 * Revision 1.21. The game's manip groups, or NULL where it has none, which
	 * is what makes the MANIP_* ops answer UNSUPPORTED. The game's on_tick
	 * calls manips_file_loaded() (src/core/manips.h) the tick it sees a save
	 * loaded or a new game started.
	 */
	const struct game_manips *manips;

	/*
	 * Build 53, revision 1.21. The order a planet menu lists this game's
	 * planets in, which is the order the game visits them rather than the
	 * order it numbers them: planet_names' count entries, every index from 0
	 * to count - 1 exactly once, placeholders last. PLANET_LIST sends it after
	 * the names, and the index a name sits at is still the planet's id. NULL
	 * where the numbering already is the game's order (RaC1, RaC2), and
	 * PLANET_LIST then sends 0 to count - 1.
	 */
	const u8 *planet_order;

	/* Revision 1.22, round 2: the console menu's table, NULL for round 1's menu. */
	const struct game_menu *menu;
};

/*
 * fp_switch and fp_switch_old, as a game names them. The branch a code switch
 * puts at a fingerprint site is only ever there under RPCS3, so the PS3 module
 * (no QWARK_EMULATOR, plat.h) has NULL for both and no fingerprint test against
 * either.
 */
#ifdef QWARK_EMULATOR
#define GAME_FP_SWITCH(fp) (fp)
#else
#define GAME_FP_SWITCH(fp) NULL
#endif

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
