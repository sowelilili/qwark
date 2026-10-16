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
#include "switches_bins.h"
#include "../core/mem.h"
#include "../core/autosplit.h"
#include "../core/manips.h"
#include "../core/savefile.h"

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
 *  12  frontend the front end's byte, for the manips' trigger         every tick
 *
 * Blocks 2 to 6 are the autosplit watcher's, and are per-tick because a split
 * has to reach the PC in milliseconds and the loading screen bounds the time
 * the client subtracts. The phases are 0..4, so a tick costs eight reads plus at
 * most one: 8.625 reads per tick on average, nine at worst. Block 12 and the
 * load block 3 carry the manips' file-load trigger (revision 1.21).
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
	{ RAC1_SAVEFILE_HELPER,   4,                     8, 4 },
	{ RAC1_IN_FRONTEND,       1,                     1, 0 }
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
#define HOT_FRONTEND 12

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

/*
 * What the manips' file-load trigger reads (revision 1.21), filled by the
 * decode: the load request word, the planet as the signed word it is, and the
 * front end's byte.
 */
static u32 g_load_request;
static u32 g_planet_word;
static u8  g_frontend;

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
		g_planet_word = be32_get(player);
		out->current_planet = (u8)g_planet_word;
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

	/* 1 once the helper has run a frame; SAVEFILE_INFO reports the same byte. */
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
		g_load_request     = be32_get(l);
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

	if (blocks[HOT_FRONTEND] != NULL) g_frontend = blocks[HOT_FRONTEND][0];
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

/* -------------------------------------- the manips' file-load trigger, 1.21 */

/*
 * A load of Veldin, for the manip groups (src/core/manips.h): the user's rule
 * is the end of a planet load whose planet is the first one, which every run
 * starts on and none comes back to. From the NPEA00385 executable; not yet run
 * on hardware.
 *
 * Every planet load starts with the load request word RAC1_LOAD_PLANET set
 * through 0x15EA84 (which also sets the destination): a new game from the
 * title (0x14EBF4, 0x14E97C) or the post-game menu, a challenge-mode start
 * (0x14EB9C), a save loaded from the title or the pause menu (the save-data
 * dialog, 0x7A1F8, with the restored planet), and qwark's own savefile helper,
 * which writes the word itself; ship travel sets it at 0x163588. The main loop
 * and the title loop both run only while it is 0 (0xE1AA0, 0x12744), so the
 * game leaves for the planet transition 0x164C58, which sets the game state to
 * 6 (0x164CE4) and the planet to the destination; the top loop then clears the
 * word (0x13DF4, the only place that does) and enters the main loop, whose
 * prologue sets the state to 6 again (0xE0FB8), sets the level up and at
 * 0x15EB34 sets it to 0 for planet 0 - most other planets get 6 for their
 * fly-in. A death writes none of it.
 *
 * So: latch whenever the request word is seen non-zero; when it is back at 0,
 * the front end is closed (RAC1_IN_FRONTEND 0) and the state has left 6, the
 * load is over, and it was a load of Veldin if the planet is 0. The latch is
 * what keeps out the state's other 6-to-0 edges, the ship cutscenes in the
 * middle of a level (0x15E7E8, 0x15E8A0). The writes land after the level
 * setup and before the first frame of play (0xE1360), when the challenge
 * counter is long final: the new-game reset (0xE71FC), a challenge-mode start
 * (0x13E990) and the restore (0xE6FE4, the counter is block 2 of the global
 * save table 0x726690) all write it before the request. None of the manip
 * addresses is written by the restore, the new-game reset, the challenge start
 * or the transition.
 *
 * Fast loads do not move any of this: they patch the loading screen, not the
 * request word or the state. The first tick of a process only learns whether a
 * load is under way, and never fires.
 *
 * Build 53: the same moment ends every load for fast loads too, whatever the
 * planet, and puts back what a load forced (rac1_fastload_restore). It
 * is the first moment all three fast-load sites are behind the game: the
 * loading screen's call (0x165490) and one sound pick (0x1641F8, in 0x163AA0,
 * from 0x165434) are in the transition, but the other (0x160060, in 0x15FE00)
 * is reached from the main loop's state 6 (0xE15C0 -> 0x161788 -> 0x1638B8),
 * which the level runs after the request word is cleared - so the word's going
 * back to 0 alone is too early. A fly-in keeps the state at 6 for its length,
 * and the patch with it, which is what the checkbox being on does anyway.
 */
static int g_fl_primed;
static int g_fl_latched;

/*
 * Build 53. g_fastload_on is what the Fast loads checkbox holds.
 * g_fastload_forced says they are on because a load - a planet load or a
 * set-aside load - forced them and not because of it: from rac1_fastload_force
 * until the watch below restores them. On a console that is invisible, since
 * the checkbox is qwark's own bit. With the code switches in, the checkbox is
 * read back out of the flag byte, and while this is set the read-back reports
 * the checkbox rather than the force, so it does not flick on for the length
 * of every load.
 */
static int g_fastload_on;
static int g_fastload_forced;

/*
 * Build 53. savefile_load_refusals as rac1_on_tick last saw it, as on RaC2. A
 * set-aside load forces fast loads before its request goes out, and the helper
 * can still drop the request on the game's frame, when no load follows to put
 * them back.
 */
static u32 g_sf_refusals;

static void rac1_file_load_watch(void)
{
	if (!g_fl_primed) {
		g_fl_primed = 1;
		g_fl_latched = (g_load_request != 0);
		return;
	}

	if (g_load_request != 0) {
		g_fl_latched = 1;
		return;
	}

	if (!g_fl_latched || g_frontend != 0 || g_as.game_state == RAC1_STATE_TRANSITION)
		return;

	g_fl_latched = 0;
	rac1_fastload_restore();
	if (g_planet_word == 0) manips_file_loaded();
}

/*
 * Whether a save file is in play, for the manips: the front end closed and a
 * planet underfoot. RAC1_IN_FRONTEND is 1 from the front end's start (0x12F00)
 * until it hands over to the first level (0x13728); the planet is 0 on the
 * title and -1 during the logos. Tick thread.
 */
static int rac1_in_file(void)
{
	u8 frontend = 1;
	u32 planet = 0;

	if (mem_read_u8(RAC1_IN_FRONTEND, &frontend) != ST_OK) return -1;
	if (mem_read_u32(RAC1_CURRENT_PLANET, &planet) != ST_OK) return -1;
	return frontend == 0 && planet < RAC1_PLANET_COUNT;
}

static void rac1_on_tick(const struct game_hot *hot)
{
	const struct rac1_as_state *p = &g_as_prev;

	(void)hot;

	/* Build 53: a set-aside load the helper dropped puts the forced fast loads back. */
	if (savefile_load_refusals() != g_sf_refusals) {
		g_sf_refusals = savefile_load_refusals();
		if (g_fastload_forced) rac1_fastload_restore();
	}

	rac1_file_load_watch();

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

	/* Collectable reasons 4-7 are retired while their helper is disabled.
	 * Do not emit hidden splits from stale counters or partial fallbacks. */

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

static void rac1_on_enter(void)
{
	/* A fresh process: the script's vars start over and nothing has been seen. */
	memset(&g_as, 0, sizeof(g_as));
	memset(&g_as_prev, 0, sizeof(g_as_prev));
	g_as_primed = 0;
	g_veldin_fix = 0;

	g_load_request = 0;
	g_planet_word = 0;
	g_frontend = 0;
	g_fl_primed = 0;
	g_fl_latched = 0;

	/*
	 * Nothing forced, and the checkbox off until the auto-apply after this call
	 * or PREVIOUS_REAPPLY turns it on through rac1_set_toggle, as on RaC2.
	 */
	g_fastload_on = 0;
	g_fastload_forced = 0;
	g_sf_refusals = savefile_load_refusals();

	/* Removing the embedded collectable hooks fixed the reported RaC1 boot
	 * black screen. Keep supported read-only events; inject no helper. */
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define NM AUTOSPLIT_FLAG_NORMALISE

/*
 * The script's settings.Add list, in its order, with the three unconditional
 * splits in front. Only supported reasons are advertised. Collectable codes
 * 4-7 remain reserved while their helper is disabled.
 *
 * The loading row is last and is not a split: DF because the time adjustment is
 * not a user option, and NORMALISE with the 7.56 s the script's WinForms timer
 * let through before it started calling the game paused.
 */
static const struct autosplit_desc rac1_autosplits[] = {
	{ R1_AS_PLANET,      AUTOSPLIT_SPLIT,      DF | RT, 0,       "Planet entered" },
	{ R1_AS_VELDIN,      AUTOSPLIT_SPLIT,      DF,      0,       "Veldin" },
	{ R1_AS_DREK_BUTTON, AUTOSPLIT_SPLIT,      DF,      0,       "Drek button" },
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

/*
 * Build 50. Under RPCS3 these three are code switches instead
 * (src/games/switches/sw_rac1.s): the same instructions, both forms of each in
 * a trampoline, chosen by the flag bytes SW_RAC1_FLAG_* name. Infinite health's
 * switch hooks the word after the one patched here, so the fingerprint stays.
 */

static struct patch_def rac1_fastload = {
	"Fast loads", PATCH_KIND_FEATURE, rac1_fastload_words, 3, NULL
};
static struct patch_def rac1_ammo = {
	"Infinite ammo", PATCH_KIND_FEATURE, rac1_ammo_words, 1, NULL
};
static struct patch_def rac1_health = {
	"Infinite health", PATCH_KIND_FEATURE, rac1_health_words, 1, NULL
};

/*
 * Build 53: a planet load and a set-aside load force fast loads on, as RaC2's
 * always have, and the end of the load puts the checkbox's setting back
 * (rac1_file_load_watch, or rac1_on_tick for a set-aside load the helper
 * dropped). g_fastload_on and g_fastload_forced, beside that watch, say why
 * they are on. patch_apply is a no-op when the toggle already applied the
 * patch, so nothing is captured twice and nothing is written twice.
 */
int rac1_fastload_force(void)
{
	int rc = classic_code_force(&rac1_fastload, SW_RAC1_FLAG_FAST_LOADS);

	if (rc == ST_OK && !g_fastload_on) g_fastload_forced = 1;
	return rc;
}

int rac1_fastload_forced(void)
{
	return g_fastload_forced;
}

int rac1_fastload_restore(void)
{
	g_fastload_forced = 0;
	if (g_fastload_on) return ST_OK;
	return classic_code_toggle(&rac1_fastload, SW_RAC1_FLAG_FAST_LOADS, 0);
}

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

	/*
	 * Build 53: Drek skip, Drek cutscene, the two shooting skill point rows and
	 * the any% all-missions reset are retired, ids and all (rac1.h). They are
	 * the manip groups below now.
	 */
	{ F_RESET_GOLDBOLTS, FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset all gold bolts" },
	{ F_UNLOCK_GOLDBOLTS,FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all gold bolts" },
	{ F_RESET_STYLE,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset skill points" },
	{ F_UNLOCK_STYLE,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Unlock all skill points" },

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
	int rc;

	switch (id) {
	case F_FAST_LOADS:
		rc = classic_code_toggle(&rac1_fastload, SW_RAC1_FLAG_FAST_LOADS, on);
		if (rc == ST_OK) {
			g_fastload_on = on;
			/* Whatever a load forced, the checkbox has the say now. */
			g_fastload_forced = 0;
		}
		return rc;

	case F_INFINITE_AMMO:   return classic_code_toggle(&rac1_ammo, SW_RAC1_FLAG_INFINITE_AMMO, on);
	case F_INFINITE_HEALTH: return classic_code_toggle(&rac1_health, SW_RAC1_FLAG_INFINITE_HEALTH, on);
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
 *
 * Build 50: and the three code switches' flag bytes, while the switches are in.
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

	switch (id) {
	case F_FAST_LOADS:
		rc = classic_code_read(SW_RAC1_FLAG_FAST_LOADS, on);
		/* Build 53: a load's force is not the checkbox; see g_fastload_forced. */
		if (rc == ST_OK && g_fastload_forced) *on = g_fastload_on;
		return rc;
	case F_INFINITE_AMMO:   return classic_code_read(SW_RAC1_FLAG_INFINITE_AMMO, on);
	case F_INFINITE_HEALTH: return classic_code_read(SW_RAC1_FLAG_INFINITE_HEALTH, on);
	default:                break;
	}

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

static int rac1_blob_set_xyz(u8 *blob, u8 len, const f32 xyz[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) bef32_put(blob + i * 4, xyz[i]);
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

/* ----------------------------------------------------------------- manips */


/*
 * Revision 1.21, build 53: RaC1's manip groups (SPEC-manips.md section 2),
 * every write in the order the spec lists it. RaC1 has no setup writes: the
 * final-cutscene flags are used up when a run reaches them, so they go back
 * every run, and each NG+ group clears the other group's flag. Both flags are
 * words, written whole as the game writes them (rac1.h).
 */
static const struct manip_write r1_poki[] = {
	MW_U32(RAC1_DREK_CUTSCENE, 1),
	MW_U32(RAC1_DREK_SKIP, 0)
};

static const struct manip_write r1_no_poki[] = {
	MW_U32(RAC1_DREK_SKIP, 1),
	MW_U32(RAC1_DREK_CUTSCENE, 0)
};

/* The Blarg bridge word and the Rilgar race's three floats. */
static const struct manip_write r1_all_missions[] = {
	MW_U32(RAC1_ANYPCT_FLAG, 0),
	MW_ZEROS(RAC1_ANYPCT_RACE, 12)
};

/* The Sonic Summoner skill point and the eight shooting skill point counters. */
static const struct manip_write r1_all_checkmarks[] = {
	MW_ZEROS(RAC1_SONIC_SP, 8),
	MW_ZEROS(RAC1_SHOOT_SP, 32)
};

static const u8 r1_gbsp_sonic[8] = { 0, 0, 0, 1, 0, 0, 0, 1 };
static const u8 r1_gbsp_shoot[32] = {
	0, 0, 0, 0x20, 0, 0, 0, 0x20, 0, 0, 0, 0x20, 0, 0, 0, 0x20,
	0, 0, 0, 0x20, 0, 0, 0, 0x20, 0, 0, 0, 0x20, 0, 0, 0, 0x20
};

static const struct manip_write r1_gbsp[] = {
	MW_BYTES(RAC1_SONIC_SP, r1_gbsp_sonic, 8),
	MW_BYTES(RAC1_SHOOT_SP, r1_gbsp_shoot, 32)
};

#define L(a) { a, (u8)(sizeof(a) / sizeof(a[0])) }
#define NONE { NULL, 0 }
#define NG   MANIP_FLAG_NGPLUS
#define EXT  MANIP_FLAG_EXTENSION

static const struct manip_group rac1_manip_groups[] = {
	{ 1, NG, "NG+ (poki skip)", "NG+",
	  { NONE, NONE }, { L(r1_poki), NONE } },
	{ 2, NG, "NG+ (no poki skip)", "NG+",
	  { NONE, NONE }, { L(r1_no_poki), NONE } },
	{ 3, 0, "All Missions", "All Missions",
	  { NONE, NONE }, { L(r1_all_missions), NONE } },
	{ 4, 0, "All Checkmarks", "All Checkmarks",
	  { NONE, NONE }, { L(r1_all_checkmarks), NONE } },
	{ 5, NG | EXT, "NG+ GBSP", "NG+ GBSP",
	  { NONE, NONE }, { L(r1_no_poki), L(r1_gbsp) } }
};

#undef L
#undef NONE
#undef NG
#undef EXT

static const struct game_manips rac1_manips = {
	"rac1",
	rac1_manip_groups,
	(u8)(sizeof(rac1_manip_groups) / sizeof(rac1_manip_groups[0])),
	RAC1_NGPLUS_STATE, 4,      /* the challenge counter, a word */
	NULL, 0,                   /* no headers */
	rac1_in_file
};

/* ---------------------------------------------------------------- vtable */

const struct game_api rac1_game = {
	{ "NPEA00385", "BCES01503", NULL, NULL },
	GAME_RAC1,

	RAC1_FP_ADDR,
	rac1_fp,
	rac1_fp_patched,
	NULL,                 /* fp_switch: the health switch hooks the word after it */
	NULL,                 /* so no earlier build's branch is there either */
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
	rac1_blob_set_xyz,
	RAC1_POS_BLOB_LEN,

	rac1_planet_names,
	rac1_planet_load,
	rac1_die,
	rac1_load_setaside,
	rac1_savefile_ready,  /* build 53: no movie, no planet load; a save to act on */

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
	rac1_on_tick,         /* the autosplit watcher and the file-load trigger */

	rac1_autosplit_describe,

	&rac1_manips          /* revision 1.21 */
};
