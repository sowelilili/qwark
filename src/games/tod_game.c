/*
 * Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00), game 5: the
 * hot block, the descriptors and the vtable. The handlers are next door in
 * tod_panel.c, the addresses in tod.h, the fact sheet in tod.c.
 *
 * Built where the games are modules (`make`: tod.sprx) or linked into a PC
 * build (qwark-host.exe, qwark-rpcs3.exe, the tests). A release links the four
 * originals alone and has no ToD at all: titles.c lists no ToD title there.
 *
 * Everything a readout or an unlock shows is a plain read of a fixed address
 * or of the three-read hero chain, every tick, in C; the game's Lua is only
 * for what needs it (LUA_EXEC, src/core/luacore.c), and its helper is the
 * core's to install, through lua_desc below.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "classic.h"
#include "luahelper_bins.h"
#include "../core/mem.h"

#include <string.h>

/* ------------------------------------------------------------- hot blocks */

/*
 *   0  inputs     the analogs +0x00, the pad mask +0x58           every tick
 *   1  level obj  the per-level object, 0 between levels          every tick
 *   2  level      the current LEVEL_ +0, challenge mode +4         every tick
 *   3  money      bolts +0, raritanium +4                          every 4th
 *   4  ammo flag  g_infiniteAmmo, which Infinite ammo's hold keys  every 4th
 *
 * The position is the hero's, two pointers deep: hot_decode reads the hero's
 * pointer again whenever the level object changes and every eighth tick
 * otherwise, and the three coordinates every tick, so a tick costs three or
 * four reads, and one more on the slow phases.
 */
static const struct game_hot_block tod_hot[] = {
	{ TOD_HOT_INPUTS,    TOD_HOT_INPUTS_LEN, 1, 0 },
	{ TOD_LEVEL_OBJ,     4,                  1, 0 },
	{ TOD_CURRENT_LEVEL, 8,                  1, 0 },
	{ TOD_BOLTS,         8,                  4, 1 },
	{ TOD_INFINITE_AMMO, 4,                  4, 2 }
};

#define HOT_INPUTS 0
#define HOT_ROOT   1
#define HOT_LEVEL  2
#define HOT_MONEY  3
#define HOT_AMMO   4

#define HERO_EVERY 8u

/*
 * Revision 1.26: what changes, in one struct: the lent state area's game part
 * in a module, a static linked in. The core zeroes the area before a load.
 */
struct tod_state {
	u32 root;          /* the level object, as the last tick read it */
	u32 hero;          /* the hero it had, 0 for none */
	u32 ammo_flag;     /* g_infiniteAmmo, as last read */
	u32 ticks;
};

#if QWARK_GAME_MODULE
#define T QWARK_STATE_AT(struct tod_state, QWARK_STATE_GAME_OFF)
typedef char tod_state_fits[sizeof(struct tod_state) <= QWARK_STATE_GAME_MAX ? 1 : -1];
#else
static struct tod_state g_tod_state;
#define T (&g_tod_state)
#endif

/*
 * The hero's pointer, for the handlers: the one the hot block last read when
 * there is one, otherwise the chain read now. Tick thread.
 */
int tod_hero(u32 *hero)
{
	u32 root = 0, h = 0;

	*hero = 0;
	if (mem_read_u32(TOD_LEVEL_OBJ, &root) != ST_OK) return ST_IO_ERROR;
	if (root == 0 || (root & 3u) != 0) return ST_BUSY;
	if (mem_read_u32(root + TOD_HERO_PTR, &h) != ST_OK) return ST_IO_ERROR;
	if (h == 0 || (h & 3u) != 0) return ST_BUSY;
	*hero = h;
	return ST_OK;
}

static void tod_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *in = blocks[HOT_INPUTS];
	u32 root = 0;
	int i;

	memset(out, 0, sizeof(*out));
	T->ticks++;

	if (in != NULL) {
		out->pad_mask = tod_pad_to_og(be32_get(in + TOD_OFF_MASK));
		/* The game keeps LX, LY, RX, RY; the wire's order is rx, ry, lx, ly. */
		out->analog[0] = bef32_get(in + 8);
		out->analog[1] = bef32_get(in + 12);
		out->analog[2] = bef32_get(in + 0);
		out->analog[3] = bef32_get(in + 4);
	}

	if (blocks[HOT_LEVEL] != NULL) {
		u32 level = be32_get(blocks[HOT_LEVEL]);

		out->current_planet = level < TOD_NPLANETS ? (u8)level : 0;
		out->readout[TOD_RO_CHALLENGE] = be32_get(blocks[HOT_LEVEL] + 4);
	}

	if (blocks[HOT_MONEY] != NULL) {
		out->readout[TOD_RO_BOLTS] = be32_get(blocks[HOT_MONEY]);
		out->readout[TOD_RO_RARITANIUM] = be32_get(blocks[HOT_MONEY] + 4);
	}

	if (blocks[HOT_AMMO] != NULL) T->ammo_flag = be32_get(blocks[HOT_AMMO]);

	if (blocks[HOT_ROOT] != NULL) root = be32_get(blocks[HOT_ROOT]);
	if ((root & 3u) != 0) root = 0;

	/* The hero, two pointers deep. */
	if (root == 0) {
		T->hero = 0;
	} else if (root != T->root || T->hero == 0 || (T->ticks % HERO_EVERY) == 0) {
		u32 h = 0;

		if (mem_read_u32(root + TOD_HERO_PTR, &h) != ST_OK || (h & 3u) != 0) h = 0;
		T->hero = h;
	}
	T->root = root;

	if (T->hero != 0) {
		u8 pos[12];

		if (mem_read(T->hero + TOD_HERO_POS, pos, sizeof(pos)) == ST_OK) {
			for (i = 0; i < 3; i++) out->pos[i] = bef32_get(pos + i * 4);
		}
	}
}

static void tod_on_tick(const struct game_hot *hot)
{
	(void)hot;
	tod_ammo_tick(T->ammo_flag, T->root);
}

static void tod_on_enter(void)
{
	memset(T, 0, sizeof(*T));
	tod_panel_reset();
}

static void tod_init(void)
{
	/* Nothing to set up: every field starts at 0, as the core leaves the area. */
}

/* -------------------------------------------------------------- describe */

static const char * const tod_groups[] = {
	"Cheats",
	"Player",
	"Progress"
};

#define G_CHEATS   0
#define G_PLAYER   1
#define G_PROGRESS 2

/* Indexed by TOD_RO_*; the order is part of the wire contract for this game. */
static const char * const tod_readouts[] = {
	"Bolts",
	"Raritanium",
	"Challenge mode"
};

#define NO FEATURE_NO_READOUT
#define LV FEATURE_FLAG_LIVE

/*
 * The ids are tod.h's; 0 to 3 are reserved for the proof of concept's rows.
 * Both cheats are a word of the game's own, so both are LIVE, and neither
 * writes code: they work under RPCS3 as they do on a console.
 */
static const struct feature_desc tod_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
	{ TF_INFINITE_AMMO, FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "Infinite ammo" },
	{ TF_GOD_RATCHET,   FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "God Ratchet" },

	{ TF_DIE,           FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ TF_BOLTS,         FEATURE_VALUE,  G_PLAYER,   0, 0,  TOD_RO_BOLTS,      0, TOD_MAX_BOLTS, "Bolts" },
	{ TF_RARITANIUM,    FEATURE_VALUE,  G_PLAYER,   0, 0,  TOD_RO_RARITANIUM, 0, TOD_MAX_BOLTS, "Raritanium" },
	{ TF_CHALLENGE,     FEATURE_VALUE,  G_PLAYER,   0, 0,  TOD_RO_CHALLENGE,  0, 99, "Challenge mode" },

	{ TF_UNLOCK_SP,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all skill points" },
	{ TF_RESET_SP,      FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset skill points" },
	{ TF_UNLOCK_GB,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all gold bolts" },
	{ TF_RESET_GB,      FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset gold bolts" },
	{ TF_UNLOCK_RYNO,   FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all RYNO plans" },
	{ TF_RESET_RYNO,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset RYNO plans" }
};

#undef NO
#undef LV

static const struct game_describe tod_describe_table = {
	tod_groups,   (u8)(sizeof(tod_groups) / sizeof(tod_groups[0])),
	tod_readouts, (u8)(sizeof(tod_readouts) / sizeof(tod_readouts[0])),
	tod_features, (u8)(sizeof(tod_features) / sizeof(tod_features[0])),
	0,            /* no toggle ships auto-flagged */
	NULL, 0       /* nothing relabelled */
};

static const struct game_describe *tod_describe(void)
{
	return &tod_describe_table;
}

/* ------------------------------------------------------- positions, planets */

static int tod_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

static int tod_blob_set_xyz(u8 *blob, u8 len, const f32 xyz[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) bef32_put(blob + i * 4, xyz[i]);
	return ST_OK;
}

static const char * const *tod_planet_names(u8 *count)
{
	*count = tod_facts.nplanets;
	return tod_facts.planet_names;
}

/* The Lua helper the core installs and talks to (src/core/luacore.c). */
static const struct lh_desc *tod_lua_desc(void)
{
	return lh_desc_for_title(TOD_TITLE);
}

/* ---------------------------------------------------------------- vtable */

const struct game_api tod_game = {
	GAME_TOD,

	tod_init,

	tod_hot,
	(u8)(sizeof(tod_hot) / sizeof(tod_hot[0])),
	tod_hot_decode,

	tod_describe,

	tod_set_toggle,
	tod_toggle_read,
	tod_trigger,
	tod_set_value,
	NULL,                 /* get_options: no ENUM */

	tod_save_blob,
	tod_load_blob,
	tod_blob_xyz,
	tod_blob_set_xyz,
	TOD_POS_BLOB_LEN,

	tod_planet_names,
	tod_planet_load,
	tod_die,
	NULL,                 /* load_setaside: no savefile helper yet */
	NULL,                 /* savefile_ready */

	tod_unlock_list,
	tod_unlock_read,
	tod_unlock_set,

	tod_levelflags_get,
	tod_levelflags_reset,
	tod_levelflags_set,

	NULL,                 /* moby_table: not mapped yet */

	tod_on_enter,
	NULL,                 /* on_quit */
	tod_on_tick,          /* Infinite ammo's hold */

	NULL,                 /* autosplit_describe: a later round */

	NULL,                 /* manips */
	NULL,                 /* planet_order: the LEVEL_ ids are the game's order */
	NULL,                 /* no console menu renderer for ToD */
	0,                    /* no fast loads */
	NULL,                 /* savefile_desc */
	tod_lua_desc,         /* revision 1.27 */
	tod_levelflag_names   /* revision 1.28 (pending): LEVELFLAGS_NAMES */
};
