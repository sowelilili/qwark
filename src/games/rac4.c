/*
 * Ratchet: Deadlocked (NPEA00423): addresses, patch words, planets, the
 * fingerprint, the quit hook and the two game-side watchers. The feature
 * handlers and the bot unlocks live next door in rac4_panel.c; this file owns
 * the numbers, the hot block and the vtable.
 *
 * Deadlocked is the only one of the four with a quit hook, because the
 * Deadlocked autosplitter SPRX already patches one in: three words that make the
 * game store 0xFF where qwark can see it the moment the player asks to quit.
 */
#include "rac4.h"
#include "classic.h"
#include "../core/mem.h"

#include <string.h>

/* See rac4.h for what these two are and why both are accepted. */
const u8 rac4_fp[4]         = { 0x41, 0x82, 0x00, 0x0C };
const u8 rac4_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Three reads every tick, one every other tick and four staggered slow ones:
 *
 *   0  inputs     pad +0x00, analogs +0x1DC                          every tick
 *   1  coords     the position Vec4 and the rotation behind it       every tick
 *   2  planet     current planet, 0 in the main menu                 every tick
 *   3  timing     frame counter +0x00, game state +0x04              every 2nd
 *   4  stats      bolts +0x00, dread +0x0C, skin +0x13, CM +0x26     every 8th
 *   5  flags      in game +0x00, tutorial flags +0x0C                every 8th
 *   6  challenge  the challenge currently being run                  every 8th
 *   7  savefile   helper byte and its two request bytes              every 8th
 *
 * Block 3 takes the even ticks and blocks 4 to 7 the odd ones, so every tick
 * costs exactly four reads: no tick pays for two slow blocks at once.
 */
static const struct game_hot_block rac4_hot[] = {
	{ RAC4_HOT_INPUTS_ADDR, RAC4_HOT_INPUTS_LEN, 1, 0 },
	{ RAC4_COORDS,          RAC4_POS_MAIN_LEN,   1, 0 },
	{ RAC4_CURRENT_PLANET,  4,                   1, 0 },
	{ RAC4_FRAME_COUNTER,   8,                   2, 0 },
	{ RAC4_BOLTS,           RAC4_HOT_STATS_LEN,  8, 1 },
	{ RAC4_IN_GAME,         RAC4_HOT_FLAGS_LEN,  8, 3 },
	{ RAC4_CURRENT_CHALLENGE, 4,                 8, 5 },
	{ RAC4_SF_HELPER,       4,                   8, 7 }
};

#define HOT_INPUTS    0
#define HOT_COORDS    1
#define HOT_PLANET    2
#define HOT_TIMING    3
#define HOT_STATS     4
#define HOT_FLAGS     5
#define HOT_CHALLENGE 6
#define HOT_SAVEFILE  7

#define OFF_ANALOGS   (RAC4_ANALOGS         - RAC4_INPUTS)        /* 0x1DC */
#define OFF_GAMESTATE (RAC4_GAME_STATE      - RAC4_FRAME_COUNTER) /* 0x04 */
#define OFF_DREAD     (RAC4_DREAD_POINTS    - RAC4_BOLTS)         /* 0x0C */
#define OFF_SKIN      (RAC4_SKIN            - RAC4_BOLTS)         /* 0x13 */
#define OFF_CM        (RAC4_CHALLENGE_MODE  - RAC4_BOLTS)         /* 0x26 */
#define OFF_TUTORIAL  (RAC4_TUTORIAL_FLAGS  - RAC4_IN_GAME)       /* 0x0C */

/* What the two watchers need and struct game_hot has no room for. */
static u8 g_game_mode;      /* game state, low byte */
static u8 g_tutorial;       /* tutorial flags, low byte */

static void rac4_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *inputs = blocks[HOT_INPUTS];
	const u8 *coords = blocks[HOT_COORDS];
	const u8 *stats  = blocks[HOT_STATS];
	int i;

	memset(out, 0, sizeof(*out));

	if (inputs != NULL) {
		out->pad_mask = be32_get(inputs);
		classic_decode_analogs(inputs + OFF_ANALOGS, out->analog);
	}

	if (coords != NULL) {
		for (i = 0; i < 3; i++) out->pos[i] = bef32_get(coords + i * 4);
	}

	if (blocks[HOT_PLANET] != NULL)
		out->current_planet = blocks[HOT_PLANET][3];

	if (blocks[HOT_TIMING] != NULL) {
		out->readout[RAC4_RO_FRAMES] = be32_get(blocks[HOT_TIMING]);
		g_game_mode = blocks[HOT_TIMING][OFF_GAMESTATE + 3];
	}

	if (stats != NULL) {
		out->readout[RAC4_RO_BOLTS]     = be32_get(stats);
		out->readout[RAC4_RO_DREAD]     = be32_get(stats + OFF_DREAD);
		out->readout[RAC4_RO_SKIN]      = stats[OFF_SKIN];
		out->readout[RAC4_RO_CHALLENGE] = stats[OFF_CM];
	}

	if (blocks[HOT_FLAGS] != NULL) {
		out->readout[RAC4_RO_IN_GAME] = be32_get(blocks[HOT_FLAGS]);
		g_tutorial = blocks[HOT_FLAGS][OFF_TUTORIAL + 3];
	}

	if (blocks[HOT_CHALLENGE] != NULL)
		out->readout[RAC4_RO_CUR_CHAL] = be32_get(blocks[HOT_CHALLENGE]);

	/* The client greys the two savefile actions on this one. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC4_RO_SAVEFILE] = blocks[HOT_SAVEFILE][0];
}

/* --------------------------------------------------------------- patches */

/*
 * The autosplitter's quit hook, verbatim:
 *   li   r3, 0xFF
 *   lis  r4, 0x0170
 *   stb  r3, 0(r4)
 * so the game writes 0xFF to RAC4_QUIT_FLAG on its way out.
 */
static const struct patch_word rac4_quit_hook_words[] = {
	{ RAC4_QUIT_HOOK_ADDR + 0, 0x386000FFu },
	{ RAC4_QUIT_HOOK_ADDR + 4, 0x3C800170u },
	{ RAC4_QUIT_HOOK_ADDR + 8, 0x98640000u }
};

/*
 * robo's "DL Crash Patches" mod, the same eleven words the Deadlocked
 * autosplitter SPRX writes every time it finds the process. The toggle ships
 * auto-flagged so qwark behaves the same way without anyone remembering to load
 * a mod before a run.
 */
static const struct patch_word rac4_crash_words[] = {
	/* Bot crash / flying */
	{ 0x0080883Cu, 0x14151617u },
	/* Dropship crash */
	{ 0x0051E1E8u, 0xECC33028u },
	{ 0x0051E21Cu, 0xECDB3028u },
	{ 0x0051E240u, 0x60000000u },
	{ 0x0051E200u, 0x10C7304Au },
	{ 0x0051E228u, 0x10DC304Au },
	{ 0x0051E24Cu, 0x60000000u },
	/* Vendor crash */
	{ 0x00618008u, 0x7CC65810u },
	{ 0x00618024u, 0x10C92886u },
	{ 0x00618034u, 0x7CE73010u },
	{ 0x0061806Cu, 0x7D296078u }
};

static const struct patch_word rac4_fastload_words[] = {
	{ RAC4_FASTLOAD_INSTR_1, 0x60000000u },   /* do not call memcard_Save */
	{ RAC4_FASTLOAD_INSTR_2, 0x60000000u }    /* do not call g_fnTransitionUpdate */
};

static const struct patch_word rac4_ammo_words[] = {
	{ RAC4_AMMO_INSTR, 0x60000000u }
};

static struct patch_def rac4_quit_hook = {
	"Quit hook", PATCH_KIND_FEATURE, rac4_quit_hook_words, 3, NULL
};
static struct patch_def rac4_crash = {
	"Crash patches", PATCH_KIND_FEATURE, rac4_crash_words, 11, NULL
};
static struct patch_def rac4_fastload = {
	"Fast loads", PATCH_KIND_FEATURE, rac4_fastload_words, 2, NULL
};
static struct patch_def rac4_ammo = {
	"Refill ammo", PATCH_KIND_FEATURE, rac4_ammo_words, 1, NULL
};

static int g_fastload_on;
static int g_softlock_on;
static u8  g_prev_tutorial  = 0xFF;
static u8  g_prev_game_mode;

int rac4_fastload_force(void)
{
	return patch_apply(&rac4_fastload);
}

static void rac4_init(void)
{
	static int done = 0;

	if (done) return;
	done = 1;

	rac4_quit_hook.originals = patch_pool_alloc(rac4_quit_hook.count);
	rac4_crash.originals     = patch_pool_alloc(rac4_crash.count);
	rac4_fastload.originals  = patch_pool_alloc(rac4_fastload.count);
	rac4_ammo.originals      = patch_pool_alloc(rac4_ammo.count);
}

static void rac4_on_enter(void)
{
	g_fastload_on   = 0;
	g_softlock_on   = 0;
	g_prev_tutorial = 0xFF;
	g_prev_game_mode = 0;
	g_game_mode     = 0;
	g_tutorial      = 0xFF;

	/*
	 * The quit hook goes in permanently and is never reverted: it is what tells
	 * the session a quit is coming, and a game that is quitting is not a good
	 * time to write four bytes back. Clear the flag byte right after, or a fresh
	 * boot reads whatever was left in that page and the session quits instantly.
	 */
	patch_apply(&rac4_quit_hook);
	mem_write_u8(RAC4_QUIT_FLAG, 0);
}

/*
 * The two watchers RAC4Form kept as memory subscriptions.
 *
 *   softlock fix: when the tutorial flag drops to 0, which is what a freshly
 *                 loaded file looks like, clear the qualifier softlock.
 *   fast loads:   the planet load forces them on, and they go back to the
 *                 toggle's setting when the game leaves the space transition.
 */
static void rac4_on_tick(const struct game_hot *hot)
{
	(void)hot;

	if (g_tutorial != g_prev_tutorial) {
		g_prev_tutorial = g_tutorial;
		if (g_softlock_on && g_tutorial == 0)
			mem_write_u8(RAC4_SOFTLOCK, 0);
	}

	if (g_game_mode != g_prev_game_mode) {
		int load_finished = (g_prev_game_mode == RAC4_GAME_MODE_SPACE &&
		                     g_game_mode != RAC4_GAME_MODE_SPACE);

		g_prev_game_mode = g_game_mode;

		if (load_finished && !g_fastload_on)
			classic_patch_toggle(&rac4_fastload, 0);
	}
}

/* -------------------------------------------------------------- describe */

static const char * const rac4_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Savefile"
};

#define G_CHEATS   0
#define G_PLAYER   1
#define G_PROGRESS 2
#define G_SAVEFILE 3

/* Indexed by RAC4_RO_*; the order is part of the wire contract for this game. */
static const char * const rac4_readouts[] = {
	"Bolts",
	"Savefile helper",
	"Dread points",
	"Challenge mode",
	"Frame counter",
	"Current challenge",
	"Skin",
	"In game"
};

#define NR (u8)(sizeof(rac4_readouts) / sizeof(rac4_readouts[0]))
#define NO FEATURE_NO_READOUT
#define WC FEATURE_FLAG_WRITES_CODE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE

static const struct feature_desc rac4_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
	{ R4_CRASH_PATCHES, FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Crash patches" },
	{ R4_SOFTLOCK_FIX,  FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Fix reset softlocks" },
	{ R4_FAST_LOADS,    FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ R4_REFILL_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Refill ammo" },
	{ R4_FREEZE_HEALTH, FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Freeze health" },
	{ R4_GHOST,         FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },

	{ R4_DIE,           FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ R4_BOLTS,         FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC4_RO_BOLTS,     0, 0, "Bolts" },
	{ R4_DREAD_POINTS,  FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC4_RO_DREAD,     0, 0, "Dread points" },
	{ R4_CHALLENGE,     FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC4_RO_CHALLENGE, 0, 255, "Challenge mode" },
	{ R4_SKIN,          FEATURE_ENUM,   G_PLAYER,   RAC4_SKIN_COUNT, 0, RAC4_RO_SKIN, 0, RAC4_SKIN_COUNT - 1, "Skin" },

	{ R4_UNLOCK_PLANETS, FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Unlock all planets" },
	{ R4_ACT_TUNE,       FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Act tune bosses" },

	/* Protocol 1.2: Deadlocked's helper has exactly the one pair. */
	{ R4_SET_ASIDE,  FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ R4_LOAD_ASIDE, FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" }
};

#undef NO
#undef WC
#undef SA
#undef LA

static const struct game_describe rac4_describe_table = {
	rac4_groups,   (u8)(sizeof(rac4_groups) / sizeof(rac4_groups[0])),
	rac4_readouts, NR,
	rac4_features, (u8)(sizeof(rac4_features) / sizeof(rac4_features[0])),
	/*
	 * The crash patches and the softlock fix ship auto-flagged, which is what the
	 * Deadlocked autosplitter SPRX does today. Config still overrides either.
	 */
	((u64)1 << R4_CRASH_PATCHES) | ((u64)1 << R4_SOFTLOCK_FIX)
};

#undef NR

static const struct game_describe *rac4_describe(void)
{
	return &rac4_describe_table;
}

/* --------------------------------------------------------------- toggles */

static int rac4_freeze_health(int on)
{
	int id;

	/* RAC4Form froze it to 200, with its own "idk why 200 tbh" beside it. */
	if (on) return freeze_add(RAC4_PLAYER_HEALTH, 4, 200, NULL);

	id = freeze_find(RAC4_PLAYER_HEALTH, 4);
	if (id < 0) return ST_OK;
	return freeze_remove((u8)id);
}

static int rac4_set_toggle(u8 id, int on)
{
	int rc;

	switch (id) {
	case R4_CRASH_PATCHES:  return classic_patch_toggle(&rac4_crash, on);

	case R4_SOFTLOCK_FIX:
		g_softlock_on = on;
		/* Re-arm, so a fresh file already sitting at 0 is caught immediately. */
		if (on) g_prev_tutorial = 0xFF;
		return ST_OK;

	case R4_FAST_LOADS:
		rc = classic_patch_toggle(&rac4_fastload, on);
		if (rc == ST_OK) g_fastload_on = on;
		return rc;

	case R4_REFILL_AMMO:    return classic_patch_toggle(&rac4_ammo, on);
	case R4_FREEZE_HEALTH:  return rac4_freeze_health(on);
	case R4_GHOST:          return classic_ghost(RAC4_GHOST_TIMER, on);
	default:                return ST_NOT_FOUND;
	}
}

/* -------------------------------------------------------------- positions */

/*
 * rac4.cs SavePosition stores position and rotation from RAC4_COORDS and the two
 * camera floats beside them. LoadPositionRac4 puts the 0x20 back at RAC4_COORDS
 * and the position half at RAC4_COORDS2; the camera restore was commented out in
 * the old client, so it stays saved and unused here too.
 */
static int rac4_save_blob(u8 *blob, u8 *len)
{
	int rc = mem_read(RAC4_COORDS, blob, RAC4_POS_MAIN_LEN);

	if (rc != ST_OK) return rc;

	rc = mem_read(RAC4_CAMERA_LR, blob + RAC4_POS_MAIN_LEN, 4);
	if (rc != ST_OK) return rc;
	rc = mem_read(RAC4_CAMERA_UD, blob + RAC4_POS_MAIN_LEN + 4, 4);
	if (rc != ST_OK) return rc;

	*len = RAC4_POS_BLOB_LEN;
	return ST_OK;
}

static int rac4_load_blob(const u8 *blob, u8 len)
{
	int rc;

	if (len < RAC4_POS_MAIN_LEN) return ST_BAD_ARG;

	rc = mem_write(RAC4_COORDS, blob, RAC4_POS_MAIN_LEN);
	if (rc != ST_OK) return rc;

	return mem_write(RAC4_COORDS2, blob, RAC4_POS_ALT_LEN);
}

static int rac4_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

/*
 * rac4.cs planetsList already carries the game's own numbering: index 0 is the
 * unused slot, index 1 is Dread Zone, and RAC4Form turned its combo box index
 * into `index + 1` to reach the same ids.
 */
static const char * const rac4_planets[] = {
	"(unused)",
	"Dread Zone",
	"Catacrom",
	"(infinite loop)",
	"Sarathos",
	"Kronos",
	"Shaar",
	"Valix",
	"Orxon",
	"(infinite loop)",
	"Torval",
	"Stygia",
	"(infinite loop)",
	"Maraxus",
	"Ghost Station",
	"Interior"
};

u8 rac4_planet_count(void)
{
	return (u8)(sizeof(rac4_planets) / sizeof(rac4_planets[0]));
}

static const char * const *rac4_planet_names(u8 *count)
{
	*count = rac4_planet_count();
	return rac4_planets;
}

static int rac4_moby_table(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride)
{
	*table_ptr_addr     = RAC4_MOBY_TABLE;
	*table_end_ptr_addr = RAC4_MOBY_TABLE_END;
	*stride             = RAC4_MOBY_STRIDE;
	return ST_OK;
}

/* ---------------------------------------------------------------- vtable */

const struct game_api rac4_game = {
	{ "NPEA00423", NULL, NULL, NULL },
	GAME_RAC4,

	RAC4_FP_ADDR,
	rac4_fp,
	rac4_fp_patched,
	sizeof(rac4_fp),

	RAC4_QUIT_FLAG,       /* the autosplitter's quit hook, installed on_enter */
	RAC4_INPUTS,

	rac4_init,

	rac4_hot,
	(u8)(sizeof(rac4_hot) / sizeof(rac4_hot[0])),
	rac4_hot_decode,

	rac4_describe,

	rac4_set_toggle,
	NULL,                 /* toggle_read: Deadlocked has no live toggle */
	rac4_trigger,
	rac4_set_value,
	rac4_get_options,

	rac4_save_blob,
	rac4_load_blob,
	rac4_blob_xyz,

	rac4_planet_names,
	rac4_planet_load,
	rac4_die,
	rac4_load_setaside,

	rac4_unlock_list,
	rac4_unlock_read,
	rac4_unlock_set,

	NULL, NULL, NULL,     /* Deadlocked has no level flag region */

	rac4_moby_table,

	rac4_on_enter,
	NULL,                 /* on_quit */
	rac4_on_tick
};
