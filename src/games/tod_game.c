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
#include "luahelper/lh_menu.h"
#include "../core/savefile.h"
#include "../core/mem.h"

#include <string.h>

/* ------------------------------------------------------------- hot blocks */

/*
 *   0  inputs     the analogs +0x00, the pad mask +0x58           every tick
 *   1  level obj  the per-level object, 0 between levels          every tick
 *   2  level      the current LEVEL_ +0, challenge mode +4         every tick
 *   3  money      bolts +0, raritanium +4                          every 4th
 *   4  ammo flag  g_infiniteAmmo, which Infinite ammo's hold keys  every 4th
 *   5  lua state  the gameplay lua_State, 0 between levels         every tick
 *   6  front end  g_inFrontEnd, 1 on the title                     every 2nd
 *   7  game state g_gameState, 4 in gameplay                       every 2nd
 *   8  load screen g_loadScreenType, the long load's                every tick
 *   9  load job   the tier loader's job, a level load's               every 2nd
 *  10  load tier  g_levelLoadTier's loaded byte, with it              every 2nd
 *
 * Blocks 5 to 10 are the autosplit watcher's (tod_autosplit.c, revision 1.28,
 * pending): the level coming up is read every tick; the title and the game
 * state (Cache cutscenes' test for gameplay since revision 1.29 retired the
 * credits split) take the even and the odd ticks between them, and a level
 * load (revision 1.29), which lasts seconds, is the two words of blocks 9 and 10,
 * read on the same tick so that they agree.
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
	{ TOD_INFINITE_AMMO, 4,                  4, 2 },
	{ TOD_LUA_STATE,     4,                  1, 0 },
	{ TOD_FRONT_END,     1,                  2, 0 },
	{ TOD_GAME_STATE,    4,                  2, 1 },
	{ TOD_LOAD_SCREEN,   4,                  1, 0 },  /* revision 1.29 */
	{ TOD_LOAD_JOB,      4,                  2, 0 },  /* revision 1.29 */
	{ TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED, 1, 2, 0 }
};

#define HOT_INPUTS 0
#define HOT_ROOT   1
#define HOT_LEVEL  2
#define HOT_MONEY  3
#define HOT_AMMO   4
#define HOT_LUA    5
#define HOT_FRONT  6
#define HOT_GSTATE 7
#define HOT_SCREEN 8
#define HOT_JOB    9
#define HOT_TIER   10

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
	struct tod_as_state as;   /* the autosplit watcher's, revision 1.28 */
	struct tod_cc_state cc;   /* the cutscene cache's, revision 1.29 (tod_cache.c) */
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
		T->as.cur_level = (s32)level;
		out->readout[TOD_RO_CHALLENGE] = be32_get(blocks[HOT_LEVEL] + 4);
	}

	if (blocks[HOT_MONEY] != NULL) {
		out->readout[TOD_RO_BOLTS] = be32_get(blocks[HOT_MONEY]);
		out->readout[TOD_RO_RARITANIUM] = be32_get(blocks[HOT_MONEY] + 4);
	}

	if (blocks[HOT_AMMO] != NULL) T->ammo_flag = be32_get(blocks[HOT_AMMO]);

	/* Revision 1.29: Cache cutscenes' status, qwark's own state and no memory read. */
	out->readout[TOD_RO_CUTSCENE_CACHE] = tod_cc_status(&T->cc);

	/* The autosplit watcher's view; a slow block keeps its last value. */
	if (blocks[HOT_LUA] != NULL)    T->as.cur_lua = be32_get(blocks[HOT_LUA]);
	if (blocks[HOT_FRONT] != NULL)  T->as.cur_front = blocks[HOT_FRONT][0];
	if (blocks[HOT_GSTATE] != NULL) T->as.cur_state = (s32)be32_get(blocks[HOT_GSTATE]);
	/* The low byte, the one the old autosplitter read (0x102034FB). */
	if (blocks[HOT_SCREEN] != NULL) T->as.cur_screen = blocks[HOT_SCREEN][3];
	if (blocks[HOT_JOB] != NULL)    T->as.cur_job = be32_get(blocks[HOT_JOB]);
	if (blocks[HOT_TIER] != NULL)   T->as.cur_tier = blocks[HOT_TIER][0];

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
	tod_panel_tick(&T->as);   /* revision 1.29: qwark's own loads' fade and fast load, Remove cutscenes */
	tod_autosplit_tick(&T->as);
	tod_cc_tick(&T->cc, &T->as);   /* revision 1.29: the cutscene cache, after the watcher (the same hot block) */
}

static void tod_on_enter(void)
{
	memset(T, 0, sizeof(*T));
	tod_autosplit_forget(&T->as);
	tod_cc_forget(&T->cc);   /* a new boot: the cache was cleared with it */
	tod_panel_reset();
	tod_panel_enter();    /* revision 1.29: the fade time and the FMVs as the game has them */
	tod_save_recover();   /* the savefile manager's round */
}

/*
 * Revision 1.29: the game is going. A long load still open closes before the
 * session's PAUSE for the quit goes out. No memory is read here.
 */
static void tod_on_quit(void)
{
	tod_autosplit_quit(&T->as);
	tod_cc_quit(&T->cc);
}

/*
 * Revision 1.29: the cutscene cache's three features are tod_cache.c's, with
 * their state here; everything else goes on to tod_panel.c.
 */
static int tod_game_set_toggle(u8 id, int on)
{
	if (id == TF_CACHE_ON_RESET || id == TF_NO_CUTSCENE_CACHE) return tod_cc_set(&T->cc, id, on);
	return tod_set_toggle(id, on);
}

static int tod_game_trigger(u8 id)
{
	if (id == TF_CACHE_CUTSCENES) return tod_cc_trigger(&T->cc, &T->as);
	return tod_trigger(id);
}

static void tod_init(void)
{
	/* Every field starts at 0, as the core leaves the area; Fast loads' patch is the panel's. */
	tod_panel_init();
}

/* -------------------------------------------------------------- describe */

static const char * const tod_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Savefile"
};

#define G_CHEATS   0
#define G_PLAYER   1
#define G_PROGRESS 2
#define G_SAVEFILE 3

/* Indexed by TOD_RO_*; the order is part of the wire contract for this game. */
static const char * const tod_readouts[] = {
	"Bolts",
	"Raritanium",
	"Challenge mode",
	"Cutscene cache"      /* revision 1.29: Cache cutscenes' status word (tod.h, TOD_CC_*) */
};

#define NO FEATURE_NO_READOUT
#define LV FEATURE_FLAG_LIVE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE
#define WC FEATURE_FLAG_WRITES_CODE

/*
 * The ids are tod.h's; 0 to 3 are reserved for the proof of concept's rows.
 * The cheats but one are a word of the game's own each and write no code:
 * they work under RPCS3 as they do on a console. Fast loads (17) is
 * WRITES_CODE since 2026-10-28 (the load screen's cut, tod.h), a patch on a
 * console and its code switch's flag byte under RPCS3.
 *
 * 2026-10-28: only God Ratchet (16) is LIVE, read back out of the proxy
 * handle's byte, which the game itself overwrites, as RaC1's goodies are.
 * Infinite ammo (8) and Infinite health (9) are not, as the other games' ammo
 * and health cheats are not, so like Fast loads they can apply themselves at
 * boot ("on boot", config's <title>.auto.<label>): each is a word only qwark
 * writes (Lua's hero_set_infinite_ammo writes g_infiniteAmmo too, and no
 * script of the game calls it), zero at every boot, so qwark's own bit says
 * what it holds. The ammo hold follows the game's word, whoever wrote it.
 *
 * 2026-10-27, the user's first hardware test: id 9, the game's no-damage word,
 * is "Infinite health" (it was "God Ratchet"), and God Ratchet is the old
 * RaCMAN's again, id 16, the proxy handle speedrunners use; the client draws
 * it beside the values, as it does RaC1's goodies menu (gamelayout.json).
 * The RYNO plans' two buttons, 14 and 15, are retired: the RYNO itself is an
 * Unlocks row.
 *
 * Revision 1.29: Remove cutscenes (18), the user's eight FMVs' files made
 * missing by data writes (tod.h, TOD_ARCHIVE_GAME), so not WRITES_CODE, and
 * not LIVE, so it can come back at boot like Fast loads beside it.
 *
 * Revision 1.29: the cutscene cache (tod_cache.c), data writes alone. Cache
 * cutscenes (19) names its status readout, as UYA's Reset trophies does;
 * Cache cutscenes on reset (20) and Disable cutscene caching (21) are qwark's
 * own state and not LIVE, so they can come back at boot. The client's ToD
 * layout puts all three on its Manips page.
 */
static const struct feature_desc tod_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
	{ TF_FAST_LOADS,      FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ TF_REMOVE_CUTSCENES, FEATURE_TOGGLE, G_CHEATS,  0, 0,  NO, 0, 0, "Remove cutscenes" },
	{ TF_INFINITE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Infinite ammo" },
	{ TF_INFINITE_HEALTH, FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Infinite health" },
	{ TF_GOD_RATCHET,     FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "God Ratchet" },
	{ TF_CACHE_CUTSCENES, FEATURE_ACTION, G_CHEATS,   0, 0,  TOD_RO_CUTSCENE_CACHE, 0, 0, "Cache cutscenes" },
	{ TF_CACHE_ON_RESET,  FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Cache cutscenes on reset" },
	{ TF_NO_CUTSCENE_CACHE, FEATURE_TOGGLE, G_CHEATS, 0, 0,  NO, 0, 0, "Disable cutscene caching" },

	{ TF_DIE,           FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ TF_BOLTS,         FEATURE_VALUE,  G_PLAYER,   0, 0,  TOD_RO_BOLTS,      0, TOD_MAX_BOLTS, "Bolts" },
	{ TF_RARITANIUM,    FEATURE_VALUE,  G_PLAYER,   0, 0,  TOD_RO_RARITANIUM, 0, TOD_MAX_BOLTS, "Raritanium" },
	{ TF_CHALLENGE,     FEATURE_VALUE,  G_PLAYER,   0, 0,  TOD_RO_CHALLENGE,  0, 99, "Challenge mode" },

	{ TF_UNLOCK_SP,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all skill points" },
	{ TF_RESET_SP,      FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset skill points" },
	{ TF_UNLOCK_GB,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all gold bolts" },
	{ TF_RESET_GB,      FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset gold bolts" },

	/* The savefile manager (tod_save.c): the game's own save system, data writes alone. */
	{ TF_SET_ASIDE,     FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ TF_LOAD_ASIDE,    FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" }
};

#undef NO
#undef LV
#undef SA
#undef LA
#undef WC

/*
 * Revision 1.30 (pending), the run-start warnings: what counts as a cheat
 * when it is on as a run starts (game.h). Fast loads, Infinite ammo,
 * Infinite health and God Ratchet. Remove cutscenes is run-legal (the
 * user's call, 2026-10-31), and the cutscene cache's two toggles are manips:
 * all three stay out, though Cheats is their group on the wire.
 */
#define TOD_RUN_CHEATS (((u64)1 << TF_FAST_LOADS) | ((u64)1 << TF_INFINITE_AMMO) | \
                        ((u64)1 << TF_INFINITE_HEALTH) | ((u64)1 << TF_GOD_RATCHET))

static const struct game_describe tod_describe_table = {
	tod_groups,   (u8)(sizeof(tod_groups) / sizeof(tod_groups[0])),
	tod_readouts, (u8)(sizeof(tod_readouts) / sizeof(tod_readouts[0])),
	tod_features, (u8)(sizeof(tod_features) / sizeof(tod_features[0])),
	/*
	 * 2026-10-31, the user's call: Remove cutscenes ships auto-flagged, as
	 * Deadlocked's crash patches do: run-legal, and no runner plays with the
	 * eight FMVs. A config.txt key for it, on or off, still wins.
	 */
	(u64)1 << TF_REMOVE_CUTSCENES,
	NULL, 0,      /* nothing relabelled */
	TOD_RUN_CHEATS
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

/* ----------------------------------------------------------- console menu */

/*
 * The console menu's pages for ToD (src/core/menu.c), shown by the game's own
 * pause menu (revision 1.29, docs/tod-menu.md), after the four games' tables
 * and the client's ToD panels. Game: the position rows, Die, Challenge mode
 * as a switch, the set-aside pair. Cheats: the toggles, then Max bolts and Max
 * raritanium. Manips: Remove cutscenes, where the client's ToD layout puts it
 * (its DESCRIBE group stays Cheats). Unlocks: the weapons Locked,
 * V1 and V10 (the level field, as the game numbers it), the rest Locked or
 * Unlocked, and the four skill point and gold bolt buttons on a page of their
 * own, Collectables, as the other games have theirs. Every planet with a name
 * may be loaded, as PLANET_LOAD loads it.
 */
static const u8 tod_menu_game[] = {
	MENU_ITEM_SLOT, MENU_ITEM_POS_SAVE, MENU_ITEM_POS_LOAD,
	TF_DIE, MENU_ITEM_VALUE(0),
	TF_SET_ASIDE, TF_LOAD_ASIDE
};

static const u8 tod_menu_cheats[] = {
	TF_FAST_LOADS, TF_INFINITE_AMMO, TF_INFINITE_HEALTH, TF_GOD_RATCHET,
	MENU_ITEM_VALUE(1), MENU_ITEM_VALUE(2)
};

/* Revision 1.29, the fifth console test: a blank row between Remove cutscenes and the cache's rows. */
static const u8 tod_menu_manips[] = {
	TF_REMOVE_CUTSCENES, MENU_ITEM_SPACER,
	TF_CACHE_CUTSCENES, TF_CACHE_ON_RESET, TF_NO_CUTSCENE_CACHE   /* revision 1.29 */
};

static const struct menu_value tod_menu_values[] = {
	{ TF_CHALLENGE,  MENU_VALUE_SWITCH, 1,             NULL },
	{ TF_BOLTS,      MENU_VALUE_SET,    TOD_MAX_BOLTS, "Max bolts" },
	{ TF_RARITANIUM, MENU_VALUE_SET,    TOD_MAX_BOLTS, "Max raritanium" }
};

static const struct menu_section tod_menu_sections[] = {
	{ "Game",   tod_menu_game,   (u8)sizeof(tod_menu_game) },
	{ "Cheats", tod_menu_cheats, (u8)sizeof(tod_menu_cheats) },
	{ "Manips", tod_menu_manips, (u8)sizeof(tod_menu_manips) }
};

static const u8 tod_menu_unlock_actions[] = {
	TF_UNLOCK_SP, TF_RESET_SP, TF_UNLOCK_GB, TF_RESET_GB
};

static const char * const tod_unlock_names[] = { "Locked", "Unlocked", "Max" };
static const u32 tod_unlock_values[] = { 0, 1, TOD_MAX_LEVEL };

static const struct game_menu tod_menu = {
	tod_menu_sections, (u8)(sizeof(tod_menu_sections) / sizeof(tod_menu_sections[0])),
	tod_menu_unlock_actions, (u8)sizeof(tod_menu_unlock_actions),
	tod_unlock_names, tod_unlock_values,
	(u8)(sizeof(tod_unlock_names) / sizeof(tod_unlock_names[0])),
	1, "V",               /* the level, field 1: V1 .. V10 */
	NULL,                 /* every planet with a name */
	NULL,                 /* every weapon goes to V10 */
	tod_cc_menu_status,   /* revision 1.29: Cache cutscenes' status (a page that shows status rows) */
	"Collectables",       /* the four buttons on a page of their own */
	NULL,                 /* no planet picker */
	tod_menu_values, (u8)(sizeof(tod_menu_values) / sizeof(tod_menu_values[0]))
};

/*
 * Revision 1.29 (pending): the Trainer's Planets row opens the galactic map,
 * the game's own planet picker (tod.h, docs/tod-menu.md 16), with the
 * gameplay package's gmap(), as Fastoon's ship does after its repair: Back
 * goes back to play, a pick is the game's own load. While it is up every
 * level the map lists is unlocked (levels 1 to 17; Fastoon II keeps the
 * save's own byte), which shows every planet of the map's 3D galaxy
 * (GalMap_setupView hides those the save has not unlocked), and the three
 * space combat levels count as visited; the core puts every byte back once
 * the map has gone. Revision 1.29, every planet (docs/tod-menu.md 17): once
 * the map is up the Lua helper's map glue (lh_gmap.lua) goes into the map's
 * own VM: Kerwan and both Fastoons are rows of their own and every pick is
 * the level picked (lm_pick). Should the glue not go in, the map is the
 * game's own with the save's planets unlocked as before, the pokes making
 * every row it has a pick of its own level (no space combat level first).
 */
#define TOD_LS(level, off) (TOD_LEVEL_SAVE + (level) * TOD_LEVEL_SAVE_LEN + (off))

static const struct menu_poke tod_galmap_pokes[] = {
	{ TOD_LS(1, TOD_LS_UNLOCKED), TOD_LEVEL_SAVE_LEN, 17, 1 },
	{ TOD_LS(1, TOD_LS_HIDDEN),   TOD_LEVEL_SAVE_LEN, 17, 0 },
	{ TOD_LS(TOD_LEVEL_SC1, TOD_LS_VISITED), 0, 1, 1 },
	{ TOD_LS(TOD_LEVEL_SC2, TOD_LS_VISITED), 0, 1, 1 },
	{ TOD_LS(TOD_LEVEL_SC3, TOD_LS_VISITED), 0, 1, 1 }
};

static const struct menu_picker tod_galmap = {
	"the galactic map",
	"gmap()",
	TOD_STATE_GALMAP,
	TOD_GALMAP_PICK,
	tod_galmap_pokes, (u8)(sizeof(tod_galmap_pokes) / sizeof(tod_galmap_pokes[0])),
	1,                    /* the map's glue: every planet (docs/tod-menu.md 17) */
	tod_galmap_up         /* a pick is qwark's load: Fast loads forced, the short fade (tod.h) */
};

/*
 * Revision 1.29: the menu's renderer is the game's own pause menu, through
 * the Lua helper's pause-menu half and its glue (src/games/luahelper/lh_menu.h).
 * The mailbox is the console menu's, 32 row slots, a MAIN page nine rows at
 * once (the pause's list) and a CONFIG page six (Game Options); TRAINER
 * takes the pause list's Inventory row (menu.help, the help byte, still says
 * whether); the pause menu's state, the state stack and the
 * game state say when no menu can be up.
 */
static const struct menu_native tod_menu_native = {
	LM_BOX, LM_BOX_SIZE, LM_ROWS, LM_CAPS,
	LM_STATE, LM_STATE_SIZE,
	LM_STATE + LM_HELP,
	LM_STATE + LM_GLUE,
	LM_STATE + LM_LOADS,
	LM_GLUE_WANTED, LM_GLUE_STOPPED,
	TOD_PAUSE_STATE, TOD_PAUSE_UP,
	TOD_STATE_PENDING_OP, 0,
	TOD_GAME_STATE, TOD_STATE_PAUSE,
	LM_STATE + LM_WHY, LM_STATE + LM_WAITS,
	LM_STATE + LM_NOTE, LM_NOTE_IN, LM_NOTE_NONE,
	LM_STATE + LM_ROOM, LM_STATE + LM_SHORT,
	LM_NOTE_NOCH, LM_NOTE_IN_LIST, LM_NOTE_LATE,
	LM_MAX_ROWS, LM_CFG_ROWS,
	TOD_STATE_GAMEPLAY,   /* the fifth console test: a load from the pause waits for play (g_gameState 4) */
	LM_GLUE_LOADED,       /* and the glue in the pause closes it for one the client asked for */
	&tod_galmap           /* revision 1.29: the Planets row opens the galactic map */
};

/* ---------------------------------------------------------------- vtable */

const struct game_api tod_game = {
	GAME_TOD,

	tod_init,

	tod_hot,
	(u8)(sizeof(tod_hot) / sizeof(tod_hot[0])),
	tod_hot_decode,

	tod_describe,

	tod_game_set_toggle,  /* tod_panel.c's, and the cutscene cache's toggles */
	tod_toggle_read,
	tod_game_trigger,     /* tod_panel.c's, and Cache cutscenes */
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
	tod_load_aside,       /* load_setaside: the set-aside file (tod_save.c) */
	tod_savefile_ready,   /* the save system idle; gameplay for a load */

	tod_unlock_list,
	tod_unlock_read,
	tod_unlock_set,

	tod_levelflags_get,
	tod_levelflags_reset,
	tod_levelflags_set,

	NULL,                 /* moby_table: not mapped yet */

	tod_on_enter,
	tod_on_quit,          /* revision 1.29: closes the long load */
	tod_on_tick,          /* Infinite ammo's hold */

	tod_autosplit_describe, /* revision 1.28, tod_autosplit.c */

	NULL,                 /* manips */
	NULL,                 /* planet_order: the LEVEL_ ids are the game's order */
	&tod_menu,            /* the console menu's pages, shown by the game's pause menu (revision 1.29) */
	GAME_FEATURE(TF_FAST_LOADS),   /* the fast-loads combo's toggle */
	tod_savefile_desc,    /* the aside buffer: the game's own save image */
	tod_lua_desc,         /* revision 1.27 */
	&tod_sf_driver,       /* revision 1.28: the save system, driven by data */
	&tod_menu_native      /* revision 1.29: the console menu is the game's pause menu */
};
