/*
 * Ratchet & Clank (NPEA00385, and BCES01503 when the disc trilogy boots it):
 * addresses, patch words, planets and the fingerprint. Under BCES01503 the
 * fingerprint is also what says RaC1 rather than RaC2 or RaC3 is the executable
 * that is mapped. The feature handlers, unlocks and level flags live next door in
 * rac1_panel.c; this file owns the numbers and the vtable.
 *
 * Every address here comes from racman's RaCTrainer/Games/RAC1: rac1.cs for the
 * address table, ResetLevelFlags, SetShootSkillPoints and the debug options,
 * RAC1Form.cs for the button handlers, NewUnlocks.cs for the unlock table and
 * JankpotForm.cs for the jankpot block. Where the old code was ambiguous the
 * comment says so.
 */
#include "rac1.h"
#include "classic.h"
#include "../core/mem.h"
#include "../core/autosplit.h"

#include <string.h>

/* See rac1.h for what these two are and why both are accepted. */
const u8 rac1_fp[4]         = { 0x30, 0x64, 0x9C, 0xE0 };
const u8 rac1_fp_patched[4] = { 0x30, 0x64, 0x00, 0x00 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Seven reads every tick and five staggered slow ones:
 *
 *   0  inputs   analogs at +0x00, pad mask at +0xB0                every tick
 *   1  player   planet +0x00, bolts +0x30, NG+ goodies +0x60,      every tick
 *               goodies menu +0x63, coords +0xF0. The gold-item bytes
 *               at +0x38 ride along but the unlock snapshot owns those
 *   2  state    player state +0x02, codebot +0x28D, rari +0x28E    every tick
 *   3  load     load request +0x00, destination planet +0x07,      every tick
 *               game state +0x08, planet frame count +0x10
 *   4  helper   the four autosplit counters, low byte of each      every tick
 *   5  kalebo   the Kalebo3 gold bolt byte                         every tick
 *   6  loadscr  the loading-screen id, low byte                    every tick
 *   7  debug    update options +0x00, mode control +0x0C           every 8th
 *   8  ngplus   challenge mode                                     every 8th
 *   9  jankpot  timer +0x00, bolts +0x04                           every 8th
 *  10  jankpot  state                                              every 8th
 *  11  savefile helper byte, then its three request bytes          every 8th
 *
 * Blocks 2 to 6 are the autosplit watcher's, and are per-tick because a split
 * has to reach the PC in milliseconds and the loading screen bounds the time
 * the client subtracts. The phases are 0..4, so a tick costs seven reads plus at
 * most one: 7.625 reads per tick on average, eight at worst.
 */
static const struct game_hot_block rac1_hot[] = {
	{ RAC1_HOT_INPUTS_ADDR,   RAC1_HOT_INPUTS_LEN,   1, 0 },
	{ RAC1_HOT_PLAYER_ADDR,   RAC1_HOT_PLAYER_LEN,   1, 0 },
	{ RAC1_HOT_STATE_ADDR,    RAC1_HOT_STATE_LEN,    1, 0 },
	{ RAC1_HOT_LOAD_ADDR,     RAC1_HOT_LOAD_LEN,     1, 0 },
	{ RAC1_AS_COUNTERS,       RAC1_HOT_AS_LEN,       1, 0 },
	{ RAC1_KALEBO_BOLT,       1,                     1, 0 },
	{ RAC1_LOADING_SCREEN,    1,                     1, 0 },
	{ RAC1_DEBUG_UPDATE,      0x10,                  8, 0 },
	{ RAC1_NGPLUS_STATE,      4,                     8, 1 },
	{ RAC1_JANKPOT_TIMER,     8,                     8, 2 },
	{ RAC1_JANKPOT_STATE,     4,                     8, 3 },
	{ RAC1_SAVEFILE_HELPER,   4,                     8, 4 }
};

#define HOT_INPUTS   0
#define HOT_PLAYER   1
#define HOT_STATE    2
#define HOT_LOAD     3
#define HOT_ASCOUNT  4
#define HOT_KALEBO   5
#define HOT_LOADSCR  6
#define HOT_DEBUG    7
#define HOT_NGPLUS   8
#define HOT_JANKPOT  9
#define HOT_JKSTATE  10
#define HOT_SAVEFILE 11

#define OFF_INPUTS      (RAC1_INPUTS - RAC1_ANALOGS)                  /* 0xB0 */
#define OFF_BOLTS       (RAC1_BOLTS - RAC1_CURRENT_PLANET)            /* 0x30 */
#define OFF_NGGOODIES   (RAC1_NGPLUS_GOODIES - RAC1_CURRENT_PLANET)   /* 0x60 */
#define OFF_GOODIES     (RAC1_GOODIES_MENU - RAC1_CURRENT_PLANET)     /* 0x63 */
#define OFF_COORDS      (RAC1_COORDS - RAC1_CURRENT_PLANET)           /* 0xF0 */
#define OFF_MODECONTROL (RAC1_DEBUG_MODE - RAC1_DEBUG_UPDATE)         /* 0x0C */

#define OFF_CODEBOT     (RAC1_ITEM_CODEBOT - RAC1_HOT_STATE_ADDR)     /* 0x28D */
#define OFF_RARI        (RAC1_ITEM_RARI    - RAC1_HOT_STATE_ADDR)     /* 0x28E */
#define OFF_DESTPLANET  (RAC1_DEST_PLANET  - RAC1_HOT_LOAD_ADDR)      /* 0x04 */
#define OFF_GAMESTATE   (RAC1_GAME_STATE   - RAC1_HOT_LOAD_ADDR)      /* 0x08 */
#define OFF_FRAMES      (RAC1_PLANET_FRAMES - RAC1_HOT_LOAD_ADDR)     /* 0x10 */
#define OFF_AS_SP       (RAC1_AS_SKILLPOINTS - RAC1_AS_COUNTERS)      /* 0x10 */
#define OFF_AS_ITEMS    (RAC1_AS_ITEMS       - RAC1_AS_COUNTERS)      /* 0x20 */
#define OFF_AS_INFOBOTS (RAC1_AS_INFOBOTS    - RAC1_AS_COUNTERS)      /* 0x30 */

/*
 * What the autosplit watcher reads, filled by the decode so the watcher itself
 * costs no read of its own. The names are the ASL's, in its own order.
 */
struct rac1_as_state {
	f32 x, y;
	u8  dest_planet;
	u8  planet;
	u16 player_state;
	u32 planet_frames;
	u32 game_state;
	u8  gold_bolts;
	u8  skill_points;
	u8  items;
	u8  kalebo_bolt;
	u8  infobots;
	u8  codebot;
	u8  rari;
	u8  loading_screen;
};

static struct rac1_as_state g_as;

static void rac1_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *inputs = blocks[HOT_INPUTS];
	const u8 *player = blocks[HOT_PLAYER];
	const u8 *debug  = blocks[HOT_DEBUG];
	int i;

	memset(out, 0, sizeof(*out));

	if (inputs != NULL) {
		out->pad_mask = be32_get(inputs + OFF_INPUTS);
		classic_decode_analogs(inputs, out->analog);
	}

	if (player != NULL) {
		out->current_planet = (u8)be32_get(player);
		out->readout[RAC1_RO_BOLTS] = be32_get(player + OFF_BOLTS);
		out->readout[RAC1_RO_NGPLUS_GOODIES] = be32_get(player + OFF_NGGOODIES);
		/*
		 * The goodies menu is a byte the game owns, so unlike the patch-backed
		 * toggles its truth is in memory rather than in toggle_state: a client
		 * that connects mid-session reads it here.
		 */
		out->readout[RAC1_RO_GOODIES] = player[OFF_GOODIES];
		for (i = 0; i < 3; i++)
			out->pos[i] = bef32_get(player + OFF_COORDS + i * 4);
	}

	if (debug != NULL) {
		u32 update = be32_get(debug);
		out->readout[RAC1_RO_CAMERA]    = be32_get(debug + OFF_MODECONTROL);
		out->readout[RAC1_RO_DBG_HERO]  = (update & RAC1_DBG_RATCHET)   ? 1 : 0;
		out->readout[RAC1_RO_DBG_MOBYS] = (update & RAC1_DBG_MOBYS)     ? 1 : 0;
		out->readout[RAC1_RO_DBG_PART]  = (update & RAC1_DBG_PARTICLES) ? 1 : 0;
	}

	if (blocks[HOT_NGPLUS] != NULL)
		out->readout[RAC1_RO_NGPLUS_STATE] = be32_get(blocks[HOT_NGPLUS]);

	if (blocks[HOT_JANKPOT] != NULL) {
		out->readout[RAC1_RO_JANK_TIMER] = be32_get(blocks[HOT_JANKPOT]);
		out->readout[RAC1_RO_JANK_BOLTS] = be32_get(blocks[HOT_JANKPOT] + 4);
	}

	if (blocks[HOT_JKSTATE] != NULL)
		out->readout[RAC1_RO_JANK_STATE] = be32_get(blocks[HOT_JKSTATE]);

	/* The client greys the three savefile actions on this one. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC1_RO_SAVEFILE] = blocks[HOT_SAVEFILE][0];

	/* --------------------------------------------- the autosplit watcher's view */

	g_as.x = out->pos[0];
	g_as.y = out->pos[1];
	g_as.planet = out->current_planet;

	if (blocks[HOT_STATE] != NULL) {
		const u8 *s = blocks[HOT_STATE];
		g_as.player_state = be16_get(s + 2);
		g_as.codebot = s[OFF_CODEBOT];
		g_as.rari    = s[OFF_RARI];
	}

	if (blocks[HOT_LOAD] != NULL) {
		const u8 *l = blocks[HOT_LOAD];
		g_as.dest_planet   = l[OFF_DESTPLANET + 3];
		g_as.game_state    = be32_get(l + OFF_GAMESTATE);
		g_as.planet_frames = be32_get(l + OFF_FRAMES);
	}

	if (blocks[HOT_ASCOUNT] != NULL) {
		const u8 *a = blocks[HOT_ASCOUNT];
		g_as.gold_bolts   = a[3];
		g_as.skill_points = a[OFF_AS_SP + 3];
		g_as.items        = a[OFF_AS_ITEMS + 3];
		g_as.infobots     = a[OFF_AS_INFOBOTS + 3];
	}

	if (blocks[HOT_KALEBO] != NULL)
		g_as.kalebo_bolt = blocks[HOT_KALEBO][0];

	if (blocks[HOT_LOADSCR] != NULL)
		g_as.loading_screen = blocks[HOT_LOADSCR][0];
}

/* ------------------------------------------------- the autosplit watcher */

/*
 * rac1-autosplitter.asl, condition for condition. Everything the script gates on
 * a *setting* is emitted anyway with its own reason code, because the client owns
 * the settings; everything it gates on game state is a condition below.
 *
 * The script's isLoading block is ported as a LOAD_START / LOAD_END pair with
 * NORMALISE: it started a 7.56 s WinForms timer when the loading-screen id left
 * 4 and only reported "loading" once that timer had run out, so the first 7.56 s
 * of every load counted towards game time and the rest did not. The client sees
 * the pair, subtracts max(0, duration - 7.56 s), and lands on the same total.
 */
static struct rac1_as_state g_as_prev;
static int g_as_primed;

/* vars.veldinFix: the Veldin split can otherwise fire twice. */
static int g_veldin_fix;

/* The four Drek buttons, from vars.buttons. */
static const f32 rac1_drek_buttons[4][2] = {
	{ 477.9081f, 601.4653f },
	{ 453.7222f, 643.2076f },
	{ 436.0573f, 577.0817f },
	{ 411.8376f, 619.1204f }
};

#define RAC1_DREK_PLANET      18
#define RAC1_DREK_STATE       34
#define RAC1_DREK_RADIUS_SQ   1.7f

static int rac1_on_a_drek_button(void)
{
	int i;

	for (i = 0; i < 4; i++) {
		f32 dx = g_as.x - rac1_drek_buttons[i][0];
		f32 dy = g_as.y - rac1_drek_buttons[i][1];

		if (dx * dx + dy * dy < RAC1_DREK_RADIUS_SQ) return 1;
	}
	return 0;
}

static void rac1_on_tick(const struct game_hot *hot)
{
	const struct rac1_as_state *p = &g_as_prev;

	(void)hot;

	/*
	 * LiveSplit's first update after init has old == current, so no edge fires
	 * on the frame the script attaches. Same here: prime and wait a tick.
	 */
	if (!g_as_primed) {
		g_as_prev = g_as;
		g_as_primed = 1;
		return;
	}

	/*
	 * start and reset are the same expression in this script, as they are in all
	 * four. Emit both and let the client apply whichever suits its timer: that is
	 * exactly what LiveSplit does with the two blocks.
	 */
	if (g_as.planet == 0 && p->game_state == 6 && g_as.game_state == 0) {
		autosplit_emit(AUTOSPLIT_RESET, 0, 0);
		g_veldin_fix = 0;
		autosplit_emit(AUTOSPLIT_START, 0, 0);
	}

	/* Split everything: any change of destination that is a real planet change. */
	if (g_as.dest_planet != p->dest_planet && g_as.planet != g_as.dest_planet &&
	    g_as.dest_planet != 0 && g_as.planet != 0) {
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_PLANET, g_as.dest_planet);
	}

	/* Veldin split. */
	if (!g_veldin_fix && g_as.game_state == 2 && p->game_state == 0 &&
	    g_as.planet == 0 && g_as.planet_frames > 5) {
		g_veldin_fix = 1;
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_VELDIN, 0);
	}

	/* Drek button split. */
	if (g_as.planet == RAC1_DREK_PLANET && g_as.player_state == RAC1_DREK_STATE &&
	    p->player_state != RAC1_DREK_STATE && rac1_on_a_drek_button()) {
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_DREK_BUTTON, 0);
	}

	/* Gold bolt split, including the Kalebo3 bolt the counter misses. */
	if (g_as.gold_bolts != p->gold_bolts)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_GOLD_BOLT, g_as.gold_bolts);
	if (g_as.kalebo_bolt != p->kalebo_bolt && g_as.kalebo_bolt != 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_GOLD_BOLT, g_as.gold_bolts);

	/* Skill point split. */
	if (g_as.skill_points != p->skill_points)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_SKILL_POINT, g_as.skill_points);

	/* Item split, plus the two items with no index of their own. */
	if (g_as.items != p->items)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_ITEM, g_as.items);
	if (g_as.codebot != p->codebot && g_as.codebot != 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_ITEM, g_as.items);
	if (g_as.rari != p->rari && g_as.rari != 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_ITEM, g_as.items);

	/* Infobot split. */
	if (g_as.infobots != p->infobots)
		autosplit_emit(AUTOSPLIT_SPLIT, R1_AS_INFOBOT, g_as.infobots);

	/*
	 * The loading screen, as an interval rather than a split. `arg` carries the
	 * screen id the game moved to, which is 4 on the LOAD_END by definition.
	 */
	if (p->loading_screen == RAC1_LOADING_IDLE &&
	    g_as.loading_screen != RAC1_LOADING_IDLE) {
		autosplit_emit(AUTOSPLIT_LOAD_START, R1_AS_LOADING, g_as.loading_screen);
	} else if (p->loading_screen != RAC1_LOADING_IDLE &&
	           g_as.loading_screen == RAC1_LOADING_IDLE) {
		autosplit_emit(AUTOSPLIT_LOAD_END, R1_AS_LOADING, g_as.loading_screen);
	}

	g_as_prev = g_as;
}

/* ------------------------------------------------ the embedded helper mod */

/*
 * racman's mods/NPEA00385/gb_sp_as_helper, byte for byte: four code caves and
 * the four words that branch into them. It is what keeps the counters at
 * 0xAFF000 / 10 / 20 / 30 that codes 4 to 7 read, and the old autosplitter was
 * useless for collectables unless the runner remembered to load it.
 *
 * qwark writes it on every entry instead. Nothing reverts it: without a run in
 * progress it is four counters nobody reads, and taking a branch back out from
 * under code that may be executing in the cave is the crash mods.c documents.
 */

/* gold_bolt.bin, 156 bytes */
static const u8 rac1_helper_gold_bolt[] = {
	0x89, 0x23, 0x00, 0x20, 0x81, 0x43, 0x00, 0x78, 0x2C, 0x09, 0x00, 0x00,
	0x40, 0x82, 0x00, 0x24, 0x81, 0x4A, 0x00, 0x00, 0x3D, 0x4A, 0x00, 0xB0,
	0x99, 0x2A, 0xF0, 0x04, 0x3D, 0x20, 0x00, 0x1D, 0x61, 0x29, 0x9D, 0x48,
	0x7D, 0x29, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x20, 0x60, 0x00, 0x00, 0x00,
	0x28, 0x09, 0x00, 0x02, 0x40, 0x82, 0xFF, 0xE8, 0x81, 0x2A, 0x00, 0x00,
	0x3D, 0x29, 0x00, 0xB0, 0x89, 0x29, 0xF0, 0x04, 0x2C, 0x09, 0x00, 0x00,
	0x40, 0x82, 0xFF, 0xD4, 0x3D, 0x20, 0x00, 0xAF, 0x38, 0xE0, 0x00, 0x01,
	0x61, 0x29, 0xF0, 0x00, 0x81, 0x09, 0x00, 0x00, 0x39, 0x08, 0x00, 0x01,
	0x91, 0x09, 0x00, 0x00, 0x81, 0x2A, 0x00, 0x00, 0x3D, 0x29, 0x00, 0xB0,
	0x98, 0xE9, 0xF0, 0x04, 0x4B, 0xFF, 0xFF, 0xAC, 0x00, 0x00, 0x00, 0x10,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x7A, 0x52, 0x00, 0x04, 0x7C, 0x41, 0x01,
	0x1B, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x18,
	0xFF, 0xFF, 0xFF, 0x70, 0x00, 0x00, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00
};

/* skillpoint.bin, 68 bytes */
static const u8 rac1_helper_skillpoint[] = {
	0x60, 0x00, 0x00, 0x00, 0x3D, 0x20, 0x00, 0xAF, 0x61, 0x29, 0xF0, 0x10,
	0x81, 0x49, 0x00, 0x00, 0x39, 0x4A, 0x00, 0x01, 0x91, 0x49, 0x00, 0x00,
	0x4E, 0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x7A, 0x52, 0x00, 0x04, 0x7C, 0x41, 0x01, 0x1B, 0x0C, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x18, 0xFF, 0xFF, 0xFF, 0xCC,
	0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00
};

/* item.bin, 64 bytes */
static const u8 rac1_helper_item[] = {
	0x3D, 0x20, 0x00, 0xAF, 0x61, 0x29, 0xF0, 0x20, 0x81, 0x49, 0x00, 0x00,
	0x39, 0x4A, 0x00, 0x01, 0x91, 0x49, 0x00, 0x00, 0x4E, 0x80, 0x00, 0x20,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0x7A, 0x52, 0x00,
	0x04, 0x7C, 0x41, 0x01, 0x1B, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10,
	0x00, 0x00, 0x00, 0x18, 0xFF, 0xFF, 0xFF, 0xCC, 0x00, 0x00, 0x00, 0x18,
	0x00, 0x00, 0x00, 0x00
};

/* infobots.bin, 68 bytes */
static const u8 rac1_helper_infobots[] = {
	0x60, 0x00, 0x00, 0x00, 0x3D, 0x20, 0x00, 0xAF, 0x61, 0x29, 0xF0, 0x30,
	0x81, 0x49, 0x00, 0x00, 0x39, 0x4A, 0x00, 0x01, 0x91, 0x49, 0x00, 0x00,
	0x4E, 0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x7A, 0x52, 0x00, 0x04, 0x7C, 0x41, 0x01, 0x1B, 0x0C, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x18, 0xFF, 0xFF, 0xFF, 0xCC,
	0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00
};

struct rac1_helper_cave {
	u32 addr;
	const u8 *bytes;
	u32 len;
};

static const struct rac1_helper_cave rac1_helper_caves[] = {
	{ RAC1_HELPER_CAVE_GB,   rac1_helper_gold_bolt,  (u32)sizeof(rac1_helper_gold_bolt) },
	{ RAC1_HELPER_CAVE_SP,   rac1_helper_skillpoint, (u32)sizeof(rac1_helper_skillpoint) },
	{ RAC1_HELPER_CAVE_ITEM, rac1_helper_item,       (u32)sizeof(rac1_helper_item) },
	{ RAC1_HELPER_CAVE_IB,   rac1_helper_infobots,   (u32)sizeof(rac1_helper_infobots) }
};

/* patch.txt's four hook words, in its order. */
static const struct patch_word rac1_helper_hooks[] = {
	{ RAC1_HELPER_HOOK_GB,   0x004F5BE4u },
	{ RAC1_HELPER_HOOK_SP,   0x483DA4EDu },
	{ RAC1_HELPER_HOOK_ITEM, 0x483E2E09u },
	{ RAC1_HELPER_HOOK_IB,   0x484F5D77u }
};

static void rac1_install_helper(void)
{
	unsigned i;

	/*
	 * The helper is four code caves and four branches into them, so it does
	 * nothing at all on a platform that cannot patch code (RPCS3). Autosplit
	 * codes 4 to 7 - gold bolt, skill point, item, infobot - read counters only
	 * this mod maintains, so they simply never fire there; every other RaC1
	 * split is a plain memory read and is unaffected.
	 */
	if (!plat_can_patch_code()) {
		plat_log("rac1: autosplit helper skipped, this platform cannot patch code");
		return;
	}

	/*
	 * Caves first, then the words, the way mods.c orders them: the words branch
	 * into the caves, so the target exists before anything can jump to it.
	 */
	plat_rsx_pause(1);
	for (i = 0; i < sizeof(rac1_helper_caves) / sizeof(rac1_helper_caves[0]); i++) {
		mem_write(rac1_helper_caves[i].addr, rac1_helper_caves[i].bytes,
		          rac1_helper_caves[i].len);
	}
	plat_rsx_pause(0);

	for (i = 0; i < sizeof(rac1_helper_hooks) / sizeof(rac1_helper_hooks[0]); i++)
		mem_write_u32(rac1_helper_hooks[i].addr, rac1_helper_hooks[i].value);

	/* The three counter words patch.txt zeroes, in its order. */
	mem_write_u32(RAC1_HELPER_ZERO_1, 0);
	mem_write_u32(RAC1_HELPER_ZERO_2, 0);
	mem_write_u32(RAC1_HELPER_ZERO_3, 0);
}

static void rac1_on_enter(void)
{
	/* A fresh process: the script's vars start over and nothing has been seen. */
	memset(&g_as, 0, sizeof(g_as));
	memset(&g_as_prev, 0, sizeof(g_as_prev));
	g_as_primed = 0;
	g_veldin_fix = 0;

	/* Codes 4 to 7 read counters only this mod maintains, so it goes in first. */
	rac1_install_helper();
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define NM AUTOSPLIT_FLAG_NORMALISE

/*
 * The script's settings.Add list, in its order, with the three unconditional
 * splits in front. A setting that defaults to true is DF here; the four
 * collectable splits default to false and a client leaves them unticked.
 *
 * The loading row is last and is not a split: DF because the time adjustment is
 * not a user option, and NORMALISE with the 7.56 s the script's WinForms timer
 * let through before it started calling the game paused.
 */
static const struct autosplit_desc rac1_autosplits[] = {
	{ R1_AS_PLANET,      AUTOSPLIT_SPLIT,      DF | RT, 0,       "Planet entered" },
	{ R1_AS_VELDIN,      AUTOSPLIT_SPLIT,      DF,      0,       "Veldin" },
	{ R1_AS_DREK_BUTTON, AUTOSPLIT_SPLIT,      DF,      0,       "Drek button" },
	{ R1_AS_GOLD_BOLT,   AUTOSPLIT_SPLIT,      0,       0,       "Gold bolt collected" },
	{ R1_AS_SKILL_POINT, AUTOSPLIT_SPLIT,      0,       0,       "Skill point" },
	{ R1_AS_ITEM,        AUTOSPLIT_SPLIT,      0,       0,       "Item collected" },
	{ R1_AS_INFOBOT,     AUTOSPLIT_SPLIT,      0,       0,       "Infobot" },
	{ R1_AS_LOADING,     AUTOSPLIT_LOAD_START, DF | NM, 7560000, "Loading screen" }
};

#undef DF
#undef RT
#undef NM

static const struct autosplit_desc *rac1_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(rac1_autosplits) / sizeof(rac1_autosplits[0]));
	return rac1_autosplits;
}

/* --------------------------------------------------------------- patches */

static const struct patch_word rac1_fastload_words[] = {
	{ 0x165490u, 0x60000000u },   /* nop the loading-screen call */
	{ 0x160060u, 0x38600006u },   /* li r3, 6: play a sound that does not exist */
	{ 0x1641F8u, 0x38600006u }
};

static const struct patch_word rac1_ammo_words[] = {
	{ 0x0AA2DCu, 0x60000000u }    /* nop the ammo decrement */
};

static const struct patch_word rac1_health_words[] = {
	{ RAC1_FP_ADDR, 0x30640000u } /* addic r3, r4, 0 instead of -25376 */
};

static struct patch_def rac1_fastload = {
	"Fast loads", PATCH_KIND_FEATURE, rac1_fastload_words, 3, NULL
};
static struct patch_def rac1_ammo = {
	"Infinite ammo", PATCH_KIND_FEATURE, rac1_ammo_words, 1, NULL
};
static struct patch_def rac1_health = {
	"Infinite health", PATCH_KIND_FEATURE, rac1_health_words, 1, NULL
};

static void rac1_init(void)
{
	static int done = 0;

	if (done) return;
	done = 1;

	rac1_fastload.originals = patch_pool_alloc(rac1_fastload.count);
	rac1_ammo.originals     = patch_pool_alloc(rac1_ammo.count);
	rac1_health.originals   = patch_pool_alloc(rac1_health.count);
}

/* -------------------------------------------------------------- describe */

static const char * const rac1_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Savefile",
	"Jankpot",
	"Debug"
};

#define G_CHEATS   0
#define G_PLAYER   1
#define G_PROGRESS 2
#define G_SAVEFILE 3
#define G_JANKPOT  4
#define G_DEBUG    5

/* Indexed by RAC1_RO_*; the order is part of the wire contract for this game. */
static const char * const rac1_readouts[] = {
	"Bolts",
	"Savefile helper",
	"Jankpot state",
	"Jankpot bolts",
	"Jankpot timer",
	"NG+ goodies",
	"NG+ state",
	"Camera mode",
	"Update Ratchet",
	"Update mobys",
	"Update particles",
	"Goodies menu"
};

#define NR (u8)(sizeof(rac1_readouts) / sizeof(rac1_readouts[0]))
#define NO FEATURE_NO_READOUT
#define WC FEATURE_FLAG_WRITES_CODE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE
/* Protocol 1.3: a toggle qwark reads back out of game memory. */
#define LV FEATURE_FLAG_LIVE

static const struct feature_desc rac1_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
	{ F_FAST_LOADS,      FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ F_INFINITE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite ammo" },
	{ F_INFINITE_HEALTH, FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite health" },
	{ F_GHOST,           FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },
	{ F_GOODIES,         FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "Goodies menu" },

	{ F_DIE,             FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ F_BOLTS,           FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC1_RO_BOLTS, 0, 0, "Bolts" },
	{ F_MAX_AMMO,        FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Max ammo, all weapons" },

	{ F_DREK_SKIP,       FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Drek skip" },
	{ F_DREK_CUTSCENE,   FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Drek cutscene" },
	{ F_RESET_SHOOT_SP,  FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset shooting skill points" },
	{ F_SETUP_SHOOT_SP,  FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Setup shooting skill points" },
	{ F_RESET_GOLDBOLTS, FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset all gold bolts" },
	{ F_UNLOCK_GOLDBOLTS,FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all gold bolts" },
	{ F_RESET_STYLE,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset skill points" },
	{ F_UNLOCK_STYLE,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all skill points" },
	{ F_ANYPCT_RESET,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Any% all-missions reset" },

	/* Protocol 1.2: the pair a client's save-file manager drives. */
	{ F_LOAD_SETASIDE,   FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" },
	{ F_SET_ASIDE_FILE,  FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ F_FORCE_AUTOSAVE,  FEATURE_ACTION, G_SAVEFILE, 0, 0,  NO, 0, 0, "Force autosave" },

	{ F_JANK_BOLTS,      FEATURE_VALUE,  G_JANKPOT,  0, 0,  RAC1_RO_JANK_BOLTS, 0, 0, "Jankpot bolts" },
	{ F_JANK_TIMER,      FEATURE_VALUE,  G_JANKPOT,  0, 0,  RAC1_RO_JANK_TIMER, 0, 0, "Jankpot timer (frames)" },
	{ F_JANK_ACTIVATE,   FEATURE_ACTION, G_JANKPOT,  0, 0,  NO, 0, 0, "Activate jankpot" },

	{ F_DBG_RATCHET,     FEATURE_TOGGLE, G_DEBUG,    0, LV, NO, 0, 0, "Update Ratchet" },
	{ F_DBG_MOBYS,       FEATURE_TOGGLE, G_DEBUG,    0, LV, NO, 0, 0, "Update mobys" },
	{ F_DBG_PARTICLES,   FEATURE_TOGGLE, G_DEBUG,    0, LV, NO, 0, 0, "Update particles" },
	{ F_DBG_CAMERA,      FEATURE_ENUM,   G_DEBUG,    3, 0,  RAC1_RO_CAMERA, 0, 2, "Camera mode" }
};

#undef NO
#undef WC
#undef SA
#undef LA
#undef LV

static const struct game_describe rac1_describe_table = {
	rac1_groups,   (u8)(sizeof(rac1_groups) / sizeof(rac1_groups[0])),
	rac1_readouts, NR,
	rac1_features, (u8)(sizeof(rac1_features) / sizeof(rac1_features[0])),
	0              /* no toggle ships auto-flagged */
};

#undef NR

static const struct game_describe *rac1_describe(void)
{
	return &rac1_describe_table;
}

/* --------------------------------------------------------------- toggles */

/*
 * The three debug update bits are a read-modify-write of one word, exactly as
 * rac1.cs SetDebugOption does. It clears a bit with XOR, which sets the bit
 * when it was already clear; we use AND-NOT, which agrees with it whenever the
 * bit really was set and does the obvious thing otherwise.
 */
int rac1_debug_bit(u32 bit, int on)
{
	u32 word = 0;
	int rc = mem_read_u32(RAC1_DEBUG_UPDATE, &word);

	if (rc != ST_OK) return rc;

	if (on) word |= bit;
	else    word &= ~bit;

	return mem_write_u32(RAC1_DEBUG_UPDATE, word);
}

static int rac1_set_toggle(u8 id, int on)
{
	switch (id) {
	case F_FAST_LOADS:      return classic_patch_toggle(&rac1_fastload, on);
	case F_INFINITE_AMMO:   return classic_patch_toggle(&rac1_ammo, on);
	case F_INFINITE_HEALTH: return classic_patch_toggle(&rac1_health, on);
	case F_GHOST:           return classic_ghost(RAC1_GHOST_TIMER, on);
	case F_GOODIES:         return mem_write_u8(RAC1_GOODIES_MENU, on ? 1 : 0);
	case F_DBG_RATCHET:     return rac1_debug_bit(RAC1_DBG_RATCHET, on);
	case F_DBG_MOBYS:       return rac1_debug_bit(RAC1_DBG_MOBYS, on);
	case F_DBG_PARTICLES:   return rac1_debug_bit(RAC1_DBG_PARTICLES, on);
	default:                return ST_NOT_FOUND;
	}
}

/*
 * Protocol 1.3. The four RaC1 toggles whose state is a byte the game owns: the
 * goodies menu flag, which lives in the save file, and the three debug update
 * bits of one word. Read-only, and only ever from the tick thread.
 */
static u32 rac1_debug_bit_for(u8 id)
{
	switch (id) {
	case F_DBG_RATCHET:   return RAC1_DBG_RATCHET;
	case F_DBG_MOBYS:     return RAC1_DBG_MOBYS;
	case F_DBG_PARTICLES: return RAC1_DBG_PARTICLES;
	default:              return 0;
	}
}

static int rac1_toggle_read(u8 id, int *on)
{
	u32 bit;
	int rc;

	if (id == F_GOODIES) {
		u8 b = 0;
		rc = mem_read_u8(RAC1_GOODIES_MENU, &b);
		if (rc != ST_OK) return rc;
		*on = (b != 0);
		return ST_OK;
	}

	bit = rac1_debug_bit_for(id);
	if (bit != 0) {
		u32 word = 0;
		rc = mem_read_u32(RAC1_DEBUG_UPDATE, &word);
		if (rc != ST_OK) return rc;
		*on = ((word & bit) != 0);
		return ST_OK;
	}

	return ST_NOT_FOUND;
}

/* -------------------------------------------------------------- positions */

static int rac1_save_blob(u8 *blob, u8 *len)
{
	int rc = mem_read(RAC1_COORDS, blob, RAC1_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = RAC1_POS_BLOB_LEN;
	return ST_OK;
}

static int rac1_load_blob(const u8 *blob, u8 len)
{
	if (len == 0 || len > QWARK_MAX_BLOB) return ST_BAD_ARG;
	return mem_write(RAC1_COORDS, blob, len);
}

static int rac1_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

static const char * const rac1_planets[] = {
	"Veldin",
	"Novalis",
	"Aridia",
	"Kerwan",
	"Eudora",
	"Rilgar",
	"Blarg",
	"Umbris",
	"Batalia",
	"Gaspar",
	"Orxon",
	"Pokitaru",
	"Hoven",
	"Gemlik",
	"Oltanis",
	"Quartu",
	"Kalebo3",
	"Fleet",
	"Veldin2"
};

u8 rac1_planet_count(void)
{
	return (u8)(sizeof(rac1_planets) / sizeof(rac1_planets[0]));
}

static const char * const *rac1_planet_names(u8 *count)
{
	*count = rac1_planet_count();
	return rac1_planets;
}

static int rac1_die(void)
{
	return classic_die_set_z(RAC1_COORDS);
}

static int rac1_moby_table(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride)
{
	*table_ptr_addr     = RAC1_MOBY_TABLE;
	*table_end_ptr_addr = RAC1_MOBY_TABLE_END;
	*stride             = RAC1_MOBY_STRIDE;
	return ST_OK;
}

/* ---------------------------------------------------------------- vtable */

const struct game_api rac1_game = {
	{ "NPEA00385", "BCES01503", NULL, NULL },
	GAME_RAC1,

	RAC1_FP_ADDR,
	rac1_fp,
	rac1_fp_patched,
	sizeof(rac1_fp),

	0,                    /* no quit hook is known for RaC1 yet */
	RAC1_INPUTS,

	rac1_init,

	rac1_hot,
	(u8)(sizeof(rac1_hot) / sizeof(rac1_hot[0])),
	rac1_hot_decode,

	rac1_describe,

	rac1_set_toggle,
	rac1_toggle_read,
	rac1_trigger,
	rac1_set_value,
	rac1_get_options,

	rac1_save_blob,
	rac1_load_blob,
	rac1_blob_xyz,

	rac1_planet_names,
	rac1_planet_load,
	rac1_die,
	rac1_load_setaside,

	rac1_unlock_list,
	rac1_unlock_read,
	rac1_unlock_set,

	/*
	 * Level flags are withheld for RaC1: the region racman read is not laid out
	 * the way rac1_levelflags_get assumes, so what it hands back is not the flag
	 * set the client would be editing. A NULL entry makes net.c answer
	 * UNSUPPORTED, which is the client's cue to hide the Level flags panel until
	 * the real format has been reverse-engineered. The handlers stay in
	 * rac1_panel.c; rac1_planet_load still calls rac1_levelflags_reset for
	 * PLANET_FLAG_RESET_LEVELFLAGS, which is the zeroing racman itself did.
	 */
	NULL,                 /* levelflags_get */
	NULL,                 /* levelflags_reset */
	NULL,                 /* levelflags_set */

	rac1_moby_table,

	rac1_on_enter,
	NULL,                 /* on_quit */
	rac1_on_tick,         /* the autosplit watcher */

	rac1_autosplit_describe
};
