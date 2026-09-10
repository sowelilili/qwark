/*
 * Ratchet & Clank 3 (NPEA00387, BCES01503): addresses, patch words, planets, the
 * fingerprint and the delayed fast-load arm. The feature handlers, unlocks and
 * level flags live next door in rac3_panel.c; this file owns the numbers, the
 * hot block and the vtable.
 *
 * Every address comes from racman's RaCTrainer/Games/RAC3. Where the old code
 * was ambiguous the comment says what was assumed.
 */
#include "rac3.h"
#include "classic.h"
#include "../core/mem.h"
#include "../core/autosplit.h"

#include <string.h>

/* See rac3.h for what these two are and why both are accepted. */
const u8 rac3_fp[4]         = { 0x7C, 0x85, 0x31, 0x2E };
const u8 rac3_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Nine reads every tick and four staggered slow ones:
 *
 *   0  inputs   pad +0x00, analogs +0x1DC                             every tick
 *   1  state    QE offset +0x000, planet +0x178, bolts +0x21C,        every tick
 *               challenge +0x24E, health XP +0x250, armour +0x25C
 *   2  player   the coordinate Vec4 and the rotation behind it        every tick
 *   3  pstate   player state +0x002, health +0x28C, Neffy phase +0x348 every tick
 *   4  load     destination planet +0x03, game state +0x20            every tick
 *   5  neffy    the Biobliterator's health bar                        every tick
 *   6  chunk    the loaded chunk                                      every tick
 *   7  guise    the Tyhrraguise unlock byte                           every tick
 *   8  loadscr  the loading-screen id, low byte                       every tick
 *   9  ship colour                                                   every 8th
 *  10  file time                                                     every 8th
 *  11  savefile the helper's own byte                                  every 8th
 *  12  chargeboot colour words                                       every 8th
 *
 * Blocks 3 to 8 belong to the autosplit watcher and are per-tick because a split
 * has to reach the PC in milliseconds; block 3 also carries the health readout
 * that used to have a slow block of its own. The slow blocks sit on phases 0..3,
 * so a tick costs nine reads plus at most one: 9.5 on average, ten at worst.
 */
static const struct game_hot_block rac3_hot[] = {
	{ RAC3_HOT_INPUTS_ADDR, RAC3_HOT_INPUTS_LEN, 1, 0 },
	{ RAC3_HOT_STATE_ADDR,  RAC3_HOT_STATE_LEN,  1, 0 },
	{ RAC3_HOT_PLAYER_ADDR, RAC3_HOT_PLAYER_LEN, 1, 0 },
	{ RAC3_HOT_PSTATE_ADDR, RAC3_HOT_PSTATE_LEN, 1, 0 },
	{ RAC3_HOT_LOAD_ADDR,   RAC3_HOT_LOAD_LEN,   1, 0 },
	{ RAC3_NEFFY_HEALTH,    4,                   1, 0 },
	{ RAC3_LOADED_CHUNK,    4,                   1, 0 },
	{ RAC3_TYHRRAGUISE,     1,                   1, 0 },
	{ RAC3_LOADING_SCREEN,  1,                   1, 0 },
	{ RAC3_SHIP_COLOUR,     4,                   8, 0 },
	{ RAC3_FILE_TIME,       4,                   8, 1 },
	{ RAC3_SF_HELPER,       1,                   8, 2 },
	{ RAC3_CB_PRIMARY_FRONT, 0x18,               8, 3 }
};

#define HOT_INPUTS   0
#define HOT_STATE    1
#define HOT_PLAYER   2
#define HOT_PSTATE   3
#define HOT_LOAD     4
#define HOT_NEFFY    5
#define HOT_CHUNK    6
#define HOT_GUISE    7
#define HOT_LOADSCR  8
#define HOT_SHIP     9
#define HOT_TIME     10
#define HOT_SAVEFILE 11
#define HOT_COLOURS  12

#define OFF_ANALOGS   (RAC3_ANALOGS        - RAC3_HOT_INPUTS_ADDR)   /* 0x1DC */

#define OFF_PLANET    (RAC3_CURRENT_PLANET - RAC3_HOT_STATE_ADDR)    /* 0x178 */
#define OFF_BOLTS     (RAC3_BOLTS          - RAC3_HOT_STATE_ADDR)    /* 0x21C */
#define OFF_CHALLENGE (RAC3_CHALLENGE_MODE - RAC3_HOT_STATE_ADDR)    /* 0x24E */
#define OFF_HEALTH_XP (RAC3_HEALTH_XP      - RAC3_HOT_STATE_ADDR)    /* 0x250 */
#define OFF_ARMOUR    (RAC3_CURRENT_ARMOR  - RAC3_HOT_STATE_ADDR)    /* 0x25C */

#define OFF_CB_BACK   (RAC3_CB_PRIMARY_BACK - RAC3_CB_PRIMARY_FRONT) /* 0x04 */
#define OFF_CB_TINT_F (RAC3_CB_TINT_FRONT   - RAC3_CB_PRIMARY_FRONT) /* 0x10 */
#define OFF_CB_TINT_B (RAC3_CB_TINT_BACK    - RAC3_CB_PRIMARY_FRONT) /* 0x14 */

#define OFF_HEALTH     (RAC3_PLAYER_HEALTH - RAC3_HOT_PSTATE_ADDR)   /* 0x28C */
#define OFF_NEFFY_PH   (RAC3_NEFFY_PHASE   - RAC3_HOT_PSTATE_ADDR)   /* 0x348 */
#define OFF_GAMESTATE  (RAC3_GAME_STATE    - RAC3_HOT_LOAD_ADDR)     /* 0x20 */

/* What the autosplit watcher reads, named the way rac3-autosplitter.asl names it. */
struct rac3_as_state {
	u8  dest_planet;
	u8  planet;
	u16 player_state;
	u32 game_state;
	f32 neffy_health;
	u32 neffy_phase;
	u32 chunk;
	u8  guise;
	u8  loading_screen;
};

static struct rac3_as_state g_as;

static void rac3_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *inputs = blocks[HOT_INPUTS];
	const u8 *state  = blocks[HOT_STATE];
	const u8 *player = blocks[HOT_PLAYER];
	int i;

	memset(out, 0, sizeof(*out));

	if (inputs != NULL) {
		out->pad_mask = be32_get(inputs);
		classic_decode_analogs(inputs + OFF_ANALOGS, out->analog);
	}

	if (state != NULL) {
		/*
		 * RaC3's planet ids start at 1 and the autosplitter reads the low byte of
		 * the word, so PLANET_LIST index 0 is a placeholder and index n is the id
		 * the game reports here.
		 */
		out->current_planet = state[OFF_PLANET + 3];
		out->readout[RAC3_RO_BOLTS]     = be32_get(state + OFF_BOLTS);
		out->readout[RAC3_RO_CHALLENGE] = state[OFF_CHALLENGE];
		out->readout[RAC3_RO_HEALTH_XP] = be32_get(state + OFF_HEALTH_XP);
		out->readout[RAC3_RO_ARMOUR]    = be16_get(state + OFF_ARMOUR);
		/*
		 * The QE offset is a signed 16-bit field. The readout carries the raw
		 * halfword and the feature row says SIGNED, 16 bits, so the client is
		 * the one that sign-extends it (protocol 1.7).
		 */
		out->readout[RAC3_RO_QE_OFFSET] = be16_get(state);
	}

	if (player != NULL) {
		for (i = 0; i < 3; i++) out->pos[i] = bef32_get(player + i * 4);
	}

	if (blocks[HOT_PSTATE] != NULL)
		out->readout[RAC3_RO_HEALTH] = be32_get(blocks[HOT_PSTATE] + OFF_HEALTH);

	if (blocks[HOT_SHIP] != NULL)
		out->readout[RAC3_RO_SHIP] = blocks[HOT_SHIP][0];

	if (blocks[HOT_TIME] != NULL)
		out->readout[RAC3_RO_FILE_TIME] = be32_get(blocks[HOT_TIME]);

	/* 1 once the helper has run a frame; SAVEFILE_INFO reports the same byte. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC3_RO_SAVEFILE] = blocks[HOT_SAVEFILE][0];

	if (blocks[HOT_COLOURS] != NULL) {
		const u8 *c = blocks[HOT_COLOURS];
		out->readout[RAC3_RO_CB_FRONT] = classic_cb_to_rgb(be32_get(c));
		out->readout[RAC3_RO_CB_BACK]  = classic_cb_to_rgb(be32_get(c + OFF_CB_BACK));
		out->readout[RAC3_RO_CB_TINT]  = classic_cb_to_rgb(be32_get(c + OFF_CB_TINT_F));
	}

	/* --------------------------------------------- the autosplit watcher's view */

	g_as.planet = out->current_planet;

	if (blocks[HOT_PSTATE] != NULL) {
		g_as.player_state = be16_get(blocks[HOT_PSTATE] + 2);
		g_as.neffy_phase  = be32_get(blocks[HOT_PSTATE] + OFF_NEFFY_PH);
	}

	if (blocks[HOT_LOAD] != NULL) {
		g_as.dest_planet = blocks[HOT_LOAD][3];
		g_as.game_state  = be32_get(blocks[HOT_LOAD] + OFF_GAMESTATE);
	}

	if (blocks[HOT_NEFFY] != NULL) g_as.neffy_health = bef32_get(blocks[HOT_NEFFY]);
	if (blocks[HOT_CHUNK] != NULL) g_as.chunk        = be32_get(blocks[HOT_CHUNK]);
	if (blocks[HOT_GUISE] != NULL) g_as.guise        = blocks[HOT_GUISE][0];

	if (blocks[HOT_LOADSCR] != NULL)
		g_as.loading_screen = blocks[HOT_LOADSCR][0];
}

/* --------------------------------------------------------------- patches */

static const struct patch_word rac3_ammo_words[] = {
	{ RAC3_AMMO_INSTR, 0x60000000u }   /* nop the ammo decrement */
};

static struct patch_def rac3_ammo = {
	"Freeze ammo", PATCH_KIND_FEATURE, rac3_ammo_words, 1, NULL
};

static void rac3_init(void)
{
	static int done = 0;

	if (done) return;
	done = 1;

	rac3_ammo.originals = patch_pool_alloc(rac3_ammo.count);
}

/*
 * rac3.cs armed fastLoad2 from a 200 ms WinForms timer after writing fastLoad1.
 * Here the countdown is in ticks and on_tick does the second write, so the
 * delay costs nothing until it fires.
 */
static int g_fastload_arm;

/*
 * The Fast loads toggle, and the planet load it has to survive.
 *
 * The two fast-load values are game data rather than patched code, and the game
 * writes its own over them every time it loads a planet. One arm is therefore
 * good for one trip: rac3.cs never noticed, because the only thing that ever
 * armed them was its own Load planet button and the next click armed them
 * again. A toggle that stays on until the user turns it off cannot work that
 * way, so g_fastload_on is what the checkbox holds and the watcher below puts
 * the values back on every planet load, the ones the client asks for and the
 * ones the game does on its own.
 */
static int g_fastload_on;

/* Whether the last tick saw a planet load in progress; see rac3_fastload_watch. */
static int g_fastload_loading;

int rac3_arm_fast_loads(void)
{
	int rc = mem_write_u32(RAC3_FAST_LOAD_1, 3);

	if (rc != ST_OK) return rc;

	/*
	 * rac3.cs guards this with "planetIndex != 26 || planetIndex != 20 ||
	 * planetIndex != 29", which is true for every planet. The arm is therefore
	 * unconditional here too, which is what the old client actually did.
	 */
	g_fastload_arm = RAC3_FASTLOAD_DELAY_TICKS;
	return ST_OK;
}

/* ------------------------------------------------- the autosplit watcher */

/*
 * rac3-autosplitter.asl, condition for condition. Everything it gates on a
 * *setting* is emitted anyway with its own reason code, because the client owns
 * the settings; everything it gates on game state is a condition below. The
 * split-route bookkeeping (vars.SplitRoute, vars.SplitCount, STRICT_ORDER) is
 * the client's business.
 *
 * The long load is not: the script took a second off game time whenever the
 * loading screen became 1, unless either end of the trip was one of the five
 * planets whose loading screen is a different one that only looks long. That is
 * code 6, a LOAD_START with FLAT, emitted under exactly that condition, and a
 * LOAD_END when the screen leaves 1 so a client can draw the interval.
 */
static struct rac3_as_state g_as_prev;
static int g_as_primed;

/* vars.biobliterator and vars.korosTBs. */
static int g_biobliterator;
static int g_koros_bolts;

/* vars.originPlanet, and whether the load in progress was counted as long. */
static u8  g_origin_planet;
static int g_long_load_open;

/* vars.llIgnorePlanets, as a test rather than a 37-entry table. */
static int rac3_ll_ignored(u8 planet)
{
	return planet == RAC3_LL_IGNORE_LAUNCHSITE ||
	       planet == RAC3_LL_IGNORE_METRO ||
	       planet == RAC3_LL_IGNORE_AQ_CLANK ||
	       planet == RAC3_LL_IGNORE_AQ_SEWERS ||
	       planet == RAC3_LL_IGNORE_TYHRRA;
}

static void rac3_autosplit_tick(void)
{
	const struct rac3_as_state *p = &g_as_prev;

	/* LiveSplit's first update has old == current; prime and wait a tick. */
	if (!g_as_primed) {
		g_as_prev = g_as;
		g_as_primed = 1;
		return;
	}

	/*
	 * The script's origin tracking, verbatim: the planet you left when a planet
	 * change is under way, and the planet you are standing on when one is not.
	 */
	if (g_as.planet != p->planet && g_as.dest_planet != 0)
		g_origin_planet = p->planet;
	else if (g_as.planet == p->planet && g_as.dest_planet == 0)
		g_origin_planet = g_as.planet;

	/* The long load, both ends of it. */
	if (g_as.loading_screen == RAC3_LOADING_LONG &&
	    p->loading_screen != RAC3_LOADING_LONG) {
		if (!rac3_ll_ignored(g_as.dest_planet) &&
		    !rac3_ll_ignored(g_origin_planet)) {
			g_long_load_open = 1;
			autosplit_emit(AUTOSPLIT_LOAD_START, R3_AS_LONG_LOAD,
			               g_as.dest_planet);
		}
	} else if (g_as.loading_screen != RAC3_LOADING_LONG &&
	           p->loading_screen == RAC3_LOADING_LONG && g_long_load_open) {
		g_long_load_open = 0;
		autosplit_emit(AUTOSPLIT_LOAD_END, R3_AS_LONG_LOAD, g_as.dest_planet);
	}

	/* The script's update block, which runs before start/split/reset. */
	if (!g_biobliterator && g_as.game_state == 0 &&
	    g_as.planet == RAC3_PLANET_LAUNCHSITE &&
	    (g_as.neffy_phase % 2) == 1 && g_as.neffy_health == 1.0f) {
		g_biobliterator = 1;
	}

	if (g_as.player_state == RAC3_KOROS_BOLT_STATE &&
	    p->player_state != RAC3_KOROS_BOLT_STATE &&
	    g_as.planet == RAC3_PLANET_KOROS) {
		g_koros_bolts++;
	}

	/*
	 * start and reset are the same expression in this script. Emit both and let
	 * the client apply whichever suits its timer.
	 */
	if (g_as.planet == RAC3_PLANET_VELDIN &&
	    p->game_state == RAC3_GAME_STATE_LOAD && g_as.game_state == 0) {
		autosplit_emit(AUTOSPLIT_RESET, 0, 0);
		g_biobliterator = 0;
		g_koros_bolts = 0;
		autosplit_emit(AUTOSPLIT_START, 0, 0);
	}

	if (g_as.planet == RAC3_PLANET_MARCADIA && g_as.chunk == 1 && p->chunk != 1)
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_LDF, 0);

	if (g_as.guise == 1 && p->guise == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_TYHRRAGUISE, 0);

	if (g_as.planet == RAC3_PLANET_KOROS && g_koros_bolts >= 2) {
		g_koros_bolts = 0;
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_KOROS_BOLT, 0);
	}

	/*
	 * The script writes these two as if/else, so a Biobliterator split takes the
	 * tick and the planet split does not also fire on it.
	 */
	if (g_as.neffy_health == 0.0f && g_biobliterator &&
	    g_as.planet == RAC3_PLANET_LAUNCHSITE) {
		g_biobliterator = 0;
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_BIOBLITERATOR, 0);
	} else if (g_as.dest_planet != p->dest_planet &&
	           g_as.planet != g_as.dest_planet &&
	           g_as.dest_planet != 0 && g_as.planet != 0) {
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_PLANET, g_as.dest_planet);
	}

	g_as_prev = g_as;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define FL AUTOSPLIT_FLAG_FLAT

/*
 * The script's settings.Add list, in its order; DF is what it defaults to true.
 * The long-load row is last and is not a split: DF because the second the script
 * took off was not a setting either.
 */
static const struct autosplit_desc rac3_autosplits[] = {
	{ R3_AS_PLANET,        AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ R3_AS_BIOBLITERATOR, AUTOSPLIT_SPLIT,      DF,      0, "Biobliterator defeated" },
	{ R3_AS_LDF,           AUTOSPLIT_SPLIT,      0,       0, "LDF entered" },
	{ R3_AS_KOROS_BOLT,    AUTOSPLIT_SPLIT,      0,       0, "Koros bolt 2" },
	{ R3_AS_TYHRRAGUISE,   AUTOSPLIT_SPLIT,      0,       0, "Tyhrraguise obtained" },
	{ R3_AS_LONG_LOAD,     AUTOSPLIT_LOAD_START, DF | FL, RAC3_LONG_LOAD_US,
	  "Long load" }
};

#undef DF
#undef RT
#undef FL

static const struct autosplit_desc *rac3_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(rac3_autosplits) / sizeof(rac3_autosplits[0]));
	return rac3_autosplits;
}

static void rac3_on_enter(void)
{
	g_fastload_arm = 0;
	g_fastload_on = 0;
	g_fastload_loading = 0;

	memset(&g_as, 0, sizeof(g_as));
	memset(&g_as_prev, 0, sizeof(g_as_prev));
	g_as_primed = 0;
	g_biobliterator = 0;
	g_koros_bolts = 0;
	g_origin_planet = 0;
	g_long_load_open = 0;
}

/*
 * Fast loads, re-armed around every planet load while the toggle is on.
 *
 * Two edges of the same trip are watched, and both are worth having. The start
 * is the game's destination planet becoming one other than the planet underfoot,
 * which is where rac3.cs re-applied the values and is what makes *this* load
 * fast. The arrival is the planet underfoot changing, which is what makes the
 * *next* load fast whatever the game wrote over the values on its way out of
 * this one. Aquatos is left alone at both ends, as rac3.cs LoadPlanetSafe left
 * it alone.
 *
 * Runs before rac3_autosplit_tick, because that is what moves g_as_prev on.
 */
static void rac3_fastload_watch(void)
{
	int loading = g_as.dest_planet != 0 && g_as.dest_planet != g_as.planet;
	int started = loading && !g_fastload_loading;
	int arrived = g_as.planet != g_as_prev.planet;
	u8  planet;

	g_fastload_loading = loading;

	/* g_as_prev is a previous state only once the watcher has primed it. */
	if (!g_fastload_on || !g_as_primed) return;
	if (!started && !arrived) return;

	planet = started ? g_as.dest_planet : g_as.planet;
	if (planet == RAC3_PLANET_AQUATOS) return;

	rac3_arm_fast_loads();
}

static void rac3_on_tick(const struct game_hot *hot)
{
	static const u8 force[2] = { 0x01, 0x01 };

	(void)hot;

	rac3_fastload_watch();
	rac3_autosplit_tick();

	if (g_fastload_arm == 0) return;
	if (--g_fastload_arm != 0) return;

	mem_write(RAC3_FAST_LOAD_2, force, sizeof(force));
}

/* -------------------------------------------------------------- describe */

static const char * const rac3_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Savefile",
	"Cosmetics"
};

#define G_CHEATS    0
#define G_PLAYER    1
#define G_PROGRESS  2
#define G_SAVEFILE  3
#define G_COSMETICS 4

/* Indexed by RAC3_RO_*; the order is part of the wire contract for this game. */
static const char * const rac3_readouts[] = {
	"Bolts",
	"Savefile helper",
	"Challenge mode",
	"Health XP",
	"Health",
	"Armour",
	"QE offset",
	"File time",
	"Ship colour",
	"Chargeboot front",
	"Chargeboot back",
	"Chargeboot tint"
};

#define NR (u8)(sizeof(rac3_readouts) / sizeof(rac3_readouts[0]))
#define NO FEATURE_NO_READOUT
#define WC FEATURE_FLAG_WRITES_CODE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE
/* Protocol 1.3: a toggle qwark reads back out of game memory. */
#define LV FEATURE_FLAG_LIVE
/* Protocol 1.7: the field behind this VALUE is two's complement, `bits` wide. */
#define SG FEATURE_FLAG_SIGNED

static const struct feature_desc rac3_features[] = {
	/* id, kind, group, aux (ENUM: options; VALUE: field bits), flags, readout, min, max, label */
	{ R3_FREEZE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Freeze ammo" },
	{ R3_FREEZE_HEALTH, FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Freeze health" },
	{ R3_OHKO,          FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "One-hit KO" },
	{ R3_GHOST,         FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },
	{ R3_QS_PAUSE,      FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "Quick-select pause" },
	/*
	 * Two game words rather than a patch, which is why this one has no
	 * WRITES_CODE: see rac3_fastload_watch for what keeps them written.
	 */
	{ R3_FAST_LOADS,    FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Fast loads" },

	{ R3_DIE,           FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ R3_BOLTS,         FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_BOLTS,     0, 0, "Bolts" },
	{ R3_CHALLENGE,     FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_CHALLENGE, 0, 255, "Challenge mode" },
	/* Health XP is a signed 32-bit field, so the row says so and stays unbounded. */
	{ R3_HEALTH_XP,     FEATURE_VALUE,  G_PLAYER,  32, SG, RAC3_RO_HEALTH_XP, 0, 0, "Health XP" },
	{ R3_HEALTH,        FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_HEALTH,    0, 0, "Health" },
	{ R3_ARMOUR,        FEATURE_ENUM,   G_PLAYER,   RAC3_ARMOUR_COUNT, 0, RAC3_RO_ARMOUR, 0, RAC3_ARMOUR_COUNT - 1, "Armour" },
	{ R3_SHIP_COLOUR,   FEATURE_ENUM,   G_PLAYER,   RAC3_SHIP_COUNT,   0, RAC3_RO_SHIP,   0, RAC3_SHIP_COUNT - 1, "Ship colour" },
	{ R3_FILE_TIME,     FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_FILE_TIME, 0, 0, "File time" },
	/*
	 * The QE offset is the signed halfword at RAC3_QE_OFFSET, the one qeTextBox
	 * edited. min and max stay 0: the width is the range.
	 */
	{ R3_QE_OFFSET,     FEATURE_VALUE,  G_PLAYER,  16, SG, RAC3_RO_QE_OFFSET, 0, 0, "QE offset" },
	{ R3_VENDOR_QE,     FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Enable vendor QE" },

	{ R3_SETUP_NGPLUS,   FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Setup NG+ manips" },
	{ R3_CC_EARLY,       FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "CC early setup" },
	{ R3_UNTUNE_BOSSES,  FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Untune bosses" },
	{ R3_RESET_DROPSHIP, FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset dropship health" },
	{ R3_RESET_TROPHIES, FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Refresh trophy state" },
	{ R3_UNLOCK_SKILL,   FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Unlock all skill points" },
	{ R3_RESET_SKILL,    FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset all skill points" },
	{ R3_UNLOCK_TITANIUM,FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Unlock all titanium bolts" },
	{ R3_RESET_TITANIUM, FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset all titanium bolts" },
	{ R3_UPGRADE_ALL,    FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Max all weapon levels" },
	{ R3_DOWNGRADE_ALL,  FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset all weapon levels" },

	/*
	 * Protocol 1.2: the two the save-file manager drives. The set-aside byte is
	 * the save manager's own, because RAC3Form's dedicated set-aside byte
	 * (0xD9FF02) only ever existed in a commented-out line.
	 */
	/*
	 * Protocol 1.9: one pair. "Save manager: load file" drove Gigahelper's
	 * tempsave reader and is retired with its id.
	 */
	{ R3_SET_ASIDE,  FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ R3_LOAD_ASIDE, FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" },

	{ R3_CB_PRIMARY_FRONT, FEATURE_COLOR, G_COSMETICS, 0, 0, RAC3_RO_CB_FRONT,  0, 0, "Chargeboots primary front" },
	{ R3_CB_PRIMARY_BACK,  FEATURE_COLOR, G_COSMETICS, 0, 0, RAC3_RO_CB_BACK,   0, 0, "Chargeboots primary back" },
	{ R3_CB_TINT_FRONT,    FEATURE_COLOR, G_COSMETICS, 0, 0, RAC3_RO_CB_TINT,   0, 0, "Chargeboots tint" }
};

#undef NO
#undef WC
#undef SA
#undef LA
#undef LV
#undef SG

static const struct game_describe rac3_describe_table = {
	rac3_groups,   (u8)(sizeof(rac3_groups) / sizeof(rac3_groups[0])),
	rac3_readouts, NR,
	rac3_features, (u8)(sizeof(rac3_features) / sizeof(rac3_features[0])),
	0              /* no toggle ships auto-flagged */
};

#undef NR

static const struct game_describe *rac3_describe(void)
{
	return &rac3_describe_table;
}

/* --------------------------------------------------------------- toggles */

/*
 * Freeze health and one-hit KO drive the same address at the same width, so the
 * freeze table holds one entry for both and turning either on takes the other
 * off. The old client left two subscriptions fighting over the word; this is the
 * same thing said out loud.
 *
 * OHKO was a conditional freeze ("only when health is above 1"). The freeze
 * table writes unconditionally, so this pins health at 1 instead: the same one
 * hit kills, but a pickup can no longer raise it either.
 */
static int rac3_health_freeze(u64 value, int on)
{
	int id;

	if (on) return freeze_add(RAC3_PLAYER_HEALTH, 4, value, NULL);

	id = freeze_find(RAC3_PLAYER_HEALTH, 4);
	if (id < 0) return ST_OK;
	return freeze_remove((u8)id);
}

static int rac3_set_toggle(u8 id, int on)
{
	switch (id) {
	case R3_FREEZE_AMMO:   return classic_patch_toggle(&rac3_ammo, on);
	case R3_FREEZE_HEALTH: return rac3_health_freeze(200, on);
	case R3_OHKO:          return rac3_health_freeze(1, on);
	case R3_GHOST:         return classic_ghost(RAC3_GHOST_TIMER, on);
	case R3_QS_PAUSE:      return mem_write_u8(RAC3_QUICK_SELECT, on ? 1 : 0);
	/*
	 * On arms the values now and leaves the watcher to put them back around
	 * every planet load. Off only stops the re-arming: the two values are the
	 * game's own and the next load writes over them, so there is nothing to
	 * restore and nothing that would take effect before then anyway.
	 */
	case R3_FAST_LOADS:
		g_fastload_on = (on != 0);
		return on ? rac3_arm_fast_loads() : ST_OK;
	default:               return ST_NOT_FOUND;
	}
}

/*
 * Protocol 1.3. The quick-select pause is one byte the game owns, so its
 * checkbox follows memory rather than the last thing qwark wrote. It rides in
 * the state hot block already, but the poll reads it straight so a client that
 * connects mid-session does not have to wait for a decode. Tick thread only.
 */
static int rac3_toggle_read(u8 id, int *on)
{
	u8 b = 0;
	int rc;

	if (id != R3_QS_PAUSE) return ST_NOT_FOUND;

	rc = mem_read_u8(RAC3_QUICK_SELECT, &b);
	if (rc != ST_OK) return rc;

	*on = (b != 0);
	return ST_OK;
}

/* -------------------------------------------------------------- positions */

static int rac3_save_blob(u8 *blob, u8 *len)
{
	int rc = mem_read(RAC3_COORDS, blob, RAC3_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = RAC3_POS_BLOB_LEN;
	return ST_OK;
}

static int rac3_load_blob(const u8 *blob, u8 len)
{
	if (len == 0 || len > QWARK_MAX_BLOB) return ST_BAD_ARG;
	return mem_write(RAC3_COORDS, blob, len);
}

static int rac3_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

/*
 * RaC3's planet ids are one-based: RAC3Form turned the combo box index into
 * `index + 1` before handing it to LoadPlanet, and current_planet reports the
 * same numbering. PLANET_LIST index has to equal the id, so index 0 is a
 * placeholder that planet_load refuses; index 1 is Veldin, and so on.
 */
static const char * const rac3_planets[] = {
	"(none)",
	"Veldin",
	"Florana",
	"Starship Phoenix",
	"Marcadia",
	"Daxx",
	"Phoenix Rescue",
	"Annihilation Nation",
	"Aquatos",
	"Tyhrranosis",
	"Zeldrin Starport",
	"Obani Gemini",
	"Blackwater City",
	"Holostar",
	"Koros",
	"Unknown",
	"Metropolis",
	"Crash Site",
	"Aridia",
	"Qwark's Hideout",
	"Launch Site",
	"Obani Draco",
	"Command Center",
	"Holostar Clank",
	"Insomniac Museum",
	"Unknown 2",
	"Metropolis Rangers",
	"Aquatos Clank",
	"Aquatos Sewers",
	"Tyhrranosis Rangers",
	"Vid Comic 6",
	"Vid Comic 1",
	"Vid Comic 4",
	"Vid Comic 2",
	"Vid Comic 3",
	"Vid Comic 5",
	"Vid Comic 1 SE"
};

u8 rac3_planet_count(void)
{
	return (u8)(sizeof(rac3_planets) / sizeof(rac3_planets[0]));
}

static const char * const *rac3_planet_names(u8 *count)
{
	*count = rac3_planet_count();
	return rac3_planets;
}

static int rac3_die(void)
{
	return classic_die_set_z(RAC3_COORDS);
}

static int rac3_moby_table(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride)
{
	*table_ptr_addr     = RAC3_MOBY_TABLE;
	*table_end_ptr_addr = RAC3_MOBY_TABLE_END;
	*stride             = RAC3_MOBY_STRIDE;
	return ST_OK;
}

/* ---------------------------------------------------------------- vtable */

const struct game_api rac3_game = {
	{ "NPEA00387", "BCES01503", NULL, NULL },
	GAME_RAC3,

	RAC3_FP_ADDR,
	rac3_fp,
	rac3_fp_patched,
	sizeof(rac3_fp),

	0,                    /* no quit hook is known for RaC3 yet */
	RAC3_INPUTS,

	rac3_init,

	rac3_hot,
	(u8)(sizeof(rac3_hot) / sizeof(rac3_hot[0])),
	rac3_hot_decode,

	rac3_describe,

	rac3_set_toggle,
	rac3_toggle_read,
	rac3_trigger,
	rac3_set_value,
	rac3_get_options,

	rac3_save_blob,
	rac3_load_blob,
	rac3_blob_xyz,

	rac3_planet_names,
	rac3_planet_load,
	rac3_die,
	rac3_load_setaside,

	rac3_unlock_list,
	rac3_unlock_read,
	rac3_unlock_set,

	rac3_levelflags_get,
	rac3_levelflags_reset,
	rac3_levelflags_set,

	rac3_moby_table,

	rac3_on_enter,
	NULL,                 /* on_quit */
	rac3_on_tick,

	rac3_autosplit_describe
};
