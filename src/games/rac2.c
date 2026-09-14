/*
 * Ratchet & Clank 2 (NPEA00386, BCES01503): addresses, patch words, planets, the
 * fingerprint and the loading-screen watcher. The feature handlers, unlocks and
 * level flags live next door in rac2_panel.c; this file owns the numbers, the
 * hot block and the vtable.
 *
 * Every address comes from racman's RaCTrainer/Games/RAC2. Where the old code
 * was ambiguous the comment says what was assumed.
 */
#include "rac2.h"
#include "classic.h"
#include "../core/mem.h"
#include "../core/features.h"
#include "../core/autosplit.h"

#include <string.h>

/* See rac2.h for what these two are and why both are accepted. */
const u8 rac2_fp[4]         = { 0x4B, 0xFF, 0xEA, 0x69 };
const u8 rac2_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Seven reads every tick and two staggered slow ones:
 *
 *   0  inputs   load screen type +0x07, count +0x0B, pad +0x1E0,       every tick
 *               analogs +0x3BC
 *   1  state    QE offset +0x00, planet +0x170, bolts +0x1C4,          every tick
 *               raritanium +0x1C8, challenge +0x1D6, health XP +0x1D8
 *   2  player   the coordinate Vec4 and the rotation behind it         every tick
 *   3  pstate   player state +0x00, hero type +0x20                    every tick
 *   4  lflags   Endako exit +0x31, Barlow race +0x47, A2 Clank +0xE9   every tick
 *   5  chunk    the current chunk byte                                 every tick
 *   6  yeedil   the Yeedil scene byte                                  every tick
 *   7  savefile the helper's own byte                                   every 8th
 *   8  chargeboot colour words                                        every 8th
 *
 * Blocks 3 to 6 are the autosplit watcher's, and are per-tick because a split has
 * to reach the PC in milliseconds. The two slow blocks sit on phases 0 and 4, so
 * a tick costs seven reads plus at most one: 7.25 on average, eight at worst.
 */
static const struct game_hot_block rac2_hot[] = {
	{ RAC2_HOT_INPUTS_ADDR, RAC2_HOT_INPUTS_LEN, 1, 0 },
	{ RAC2_HOT_STATE_ADDR,  RAC2_HOT_STATE_LEN,  1, 0 },
	{ RAC2_HOT_PLAYER_ADDR, RAC2_HOT_PLAYER_LEN, 1, 0 },
	{ RAC2_HOT_PSTATE_ADDR, RAC2_HOT_PSTATE_LEN, 1, 0 },
	{ RAC2_HOT_FLAGS_ADDR,  RAC2_HOT_FLAGS_LEN,  1, 0 },
	{ RAC2_CHUNK,           1,                   1, 0 },
	{ RAC2_YEEDIL_SCENE,    1,                   1, 0 },
	{ RAC2_SF_HELPER,       1,                   8, 0 },
	{ RAC2_CB_PRIMARY_FRONT, 0x14,               8, 4 }
};

#define HOT_INPUTS   0
#define HOT_STATE    1
#define HOT_PLAYER   2
#define HOT_PSTATE   3
#define HOT_LFLAGS   4
#define HOT_CHUNK    5
#define HOT_YEEDIL   6
#define HOT_SAVEFILE 7
#define HOT_COLOURS  8

#define OFF_LOADTYPE  (RAC2_LOADSCREEN_TYPE  - RAC2_HOT_INPUTS_ADDR)   /* 0x007 */
#define OFF_LOADCOUNT (RAC2_LOADSCREEN_COUNT - RAC2_HOT_INPUTS_ADDR)   /* 0x00B */
#define OFF_PAD       (RAC2_INPUTS           - RAC2_HOT_INPUTS_ADDR)   /* 0x1E0 */
#define OFF_ANALOGS   (RAC2_ANALOGS          - RAC2_HOT_INPUTS_ADDR)   /* 0x3BC */

#define OFF_PLANET     (RAC2_CURRENT_PLANET - RAC2_HOT_STATE_ADDR)     /* 0x170 */
#define OFF_BOLTS      (RAC2_BOLTS          - RAC2_HOT_STATE_ADDR)     /* 0x1C4 */
#define OFF_RARITANIUM (RAC2_RARITANIUM     - RAC2_HOT_STATE_ADDR)     /* 0x1C8 */
#define OFF_CHALLENGE  (RAC2_CHALLENGE_MODE - RAC2_HOT_STATE_ADDR)     /* 0x1D6 */
#define OFF_HEALTH_XP  (RAC2_HEALTH_XP      - RAC2_HOT_STATE_ADDR)     /* 0x1D8 */

#define OFF_CB_BACK    (RAC2_CB_PRIMARY_BACK - RAC2_CB_PRIMARY_FRONT)  /* 0x04 */
#define OFF_CB_TINT    (RAC2_CB_TINT_FRONT   - RAC2_CB_PRIMARY_FRONT)  /* 0x10 */

#define OFF_HERO_TYPE  (RAC2_HERO_TYPE - RAC2_HOT_PSTATE_ADDR)         /* 0x20 */

#define OFF_LF_ENDAKO_EXIT   (RAC2_LF_ENDAKO_EXIT   - RAC2_HOT_FLAGS_ADDR)  /* 0x31 */
#define OFF_LF_BARLOW_RACE   (RAC2_LF_BARLOW_RACE   - RAC2_HOT_FLAGS_ADDR)  /* 0x47 */
#define OFF_LF_ARANOS2_CLANK (RAC2_LF_ARANOS2_CLANK - RAC2_HOT_FLAGS_ADDR)  /* 0xE9 */

/*
 * What the watcher needs and struct game_hot has no room for. Filled by the
 * decode, read by on_tick, so the watcher costs no syscall of its own.
 */
static u8 g_load_count;

/* What the autosplit watcher reads, named the way rac2-autosplitter.asl names it. */
struct rac2_as_state {
	u32 player_state;
	u8  planet;
	u8  chunk;
	u8  clank;
	u8  yeedil_scene;
	u8  endako_exit;
	u8  barlow_entry;
	u8  hero_type;
	u8  load_screen;
};

static struct rac2_as_state g_as;

static void rac2_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *inputs = blocks[HOT_INPUTS];
	const u8 *state  = blocks[HOT_STATE];
	const u8 *player = blocks[HOT_PLAYER];
	int i;

	memset(out, 0, sizeof(*out));

	if (inputs != NULL) {
		g_load_count  = inputs[OFF_LOADCOUNT];
		out->pad_mask = be32_get(inputs + OFF_PAD);
		classic_decode_analogs(inputs + OFF_ANALOGS, out->analog);
		/* The watcher's, but the block is already read: see below. */
		g_as.load_screen = inputs[OFF_LOADTYPE];
	}

	if (state != NULL) {
		out->current_planet = state[OFF_PLANET + 3];
		out->readout[RAC2_RO_BOLTS]      = be32_get(state + OFF_BOLTS);
		out->readout[RAC2_RO_RARITANIUM] = be32_get(state + OFF_RARITANIUM);
		out->readout[RAC2_RO_CHALLENGE]  = state[OFF_CHALLENGE];
		out->readout[RAC2_RO_HEALTH_XP]  = be32_get(state + OFF_HEALTH_XP);
		/*
		 * The QE offset is a signed 16-bit field. The readout carries the raw
		 * halfword and the feature row says SIGNED, 16 bits, so the client is
		 * the one that sign-extends it (protocol 1.7).
		 */
		out->readout[RAC2_RO_QE_OFFSET]  = be16_get(state);
	}

	if (player != NULL) {
		for (i = 0; i < 3; i++) out->pos[i] = bef32_get(player + i * 4);
	}

	/* 1 once the helper has run a frame; SAVEFILE_INFO reports the same byte. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC2_RO_SAVEFILE] = blocks[HOT_SAVEFILE][0];

	if (blocks[HOT_COLOURS] != NULL) {
		const u8 *c = blocks[HOT_COLOURS];
		out->readout[RAC2_RO_CB_FRONT] = classic_cb_to_rgb(be32_get(c));
		out->readout[RAC2_RO_CB_BACK]  = classic_cb_to_rgb(be32_get(c + OFF_CB_BACK));
		out->readout[RAC2_RO_CB_TINT]  = classic_cb_to_rgb(be32_get(c + OFF_CB_TINT));
	}

	/* --------------------------------------------- the autosplit watcher's view */

	g_as.planet = out->current_planet;

	if (blocks[HOT_PSTATE] != NULL) {
		g_as.player_state = be32_get(blocks[HOT_PSTATE]);
		g_as.hero_type    = blocks[HOT_PSTATE][OFF_HERO_TYPE];
	}

	if (blocks[HOT_LFLAGS] != NULL) {
		const u8 *f = blocks[HOT_LFLAGS];
		g_as.endako_exit  = f[OFF_LF_ENDAKO_EXIT];
		g_as.barlow_entry = f[OFF_LF_BARLOW_RACE];
		g_as.clank        = f[OFF_LF_ARANOS2_CLANK];
	}

	if (blocks[HOT_CHUNK] != NULL)  g_as.chunk        = blocks[HOT_CHUNK][0];
	if (blocks[HOT_YEEDIL] != NULL) g_as.yeedil_scene = blocks[HOT_YEEDIL][0];
}

/* --------------------------------------------------------------- patches */

static const struct patch_word rac2_fastload_words[] = {
	{ RAC2_FASTLOAD_INSTR, 0x60000000u }   /* nop the load-screen branch */
};

static const struct patch_word rac2_ammo_words[] = {
	{ RAC2_AMMO_RESET_INSTR, 0x60000000u } /* nop the ammo decrement */
};

static struct patch_def rac2_fastload = {
	"Fast loads", PATCH_KIND_FEATURE, rac2_fastload_words, 1, NULL
};
static struct patch_def rac2_ammo = {
	"Infinite ammo", PATCH_KIND_FEATURE, rac2_ammo_words, 1, NULL
};

/* Live toggle state the watcher and the planet load need to see. */
static int g_fastload_on;
static int g_auto_anypct;
static int g_auto_ngplus;
static int g_death_bosses;
static int g_death_pbolts;
static u8  g_prev_load_count = 0xFF;

/*
 * RAC2Form's desiredShortcutIndex: the NG+ setups remember which shortcut the
 * menu should come back to, and the menu-storage reset re-applies it. 0xFFFFFFFF
 * means no setup has run this session, which is the C# null.
 */
static u32 g_shortcut_index = 0xFFFFFFFFu;

void rac2_set_shortcut_index(u32 index) { g_shortcut_index = index; }
u32  rac2_shortcut_index(void)          { return g_shortcut_index; }

int rac2_death_bosses(void) { return g_death_bosses; }
int rac2_death_pbolts(void) { return g_death_pbolts; }

/*
 * Every path that loads something (a planet, a set-aside file) forced fast loads
 * on in the old client and let the loading-screen watcher put the checkbox back.
 * patch_apply is a no-op when the toggle already applied it, so nothing is
 * captured twice and nothing is written twice.
 */
int rac2_fastload_force(void)
{
	return patch_apply(&rac2_fastload);
}

int rac2_fastload_restore(void)
{
	if (g_fastload_on) return ST_OK;
	return classic_patch_toggle(&rac2_fastload, 0);
}

static void rac2_init(void)
{
	static int done = 0;

	if (done) return;
	done = 1;

	rac2_fastload.originals = patch_pool_alloc(rac2_fastload.count);
	rac2_ammo.originals     = patch_pool_alloc(rac2_ammo.count);
}

/* ------------------------------------------------- the autosplit watcher */

/*
 * rac2-autosplitter.asl, condition for condition. Everything it gates on a
 * *setting* is emitted anyway with its own reason code, because the client owns
 * the settings; everything it gates on game state is a condition below.
 *
 * The script's `update` block subtracted a fixed slice of game time every time
 * the load-screen byte changed, keyed on the value it changed *to*: 1 frame for
 * 0, 9 for 1, 21 for 3, nothing for anything else. Those are three LOAD_START
 * codes with the FLAT flag, so the client applies the same three subtractions at
 * the same three moments. Nothing closes them: the script never timed the load,
 * it only paid a toll on entering one.
 */
static struct rac2_as_state g_as_prev;
static int g_as_primed;

#define RAC2_START_STATE 98   /* playerState when the file starts on Aranos */
#define RAC2_FLAG_SET    128  /* what the three level-flag bytes read once set */
#define RAC2_YEEDIL_PROTO 6   /* the Protopet cutscene's scene id */

static void rac2_autosplit_tick(void)
{
	const struct rac2_as_state *p = &g_as_prev;

	/* LiveSplit's first update has old == current; prime and wait a tick. */
	if (!g_as_primed) {
		g_as_prev = g_as;
		g_as_primed = 1;
		return;
	}

	/*
	 * The script's `update` block runs before start, reset and split, so the load
	 * transition is emitted first and the client's subtraction lands before
	 * anything else this tick can take a split.
	 */
	if (g_as.load_screen != p->load_screen) {
		switch (g_as.load_screen) {
		case RAC2_LOADSCREEN_SLIDE:
			autosplit_emit(AUTOSPLIT_LOAD_START, R2_AS_LOAD_SLIDE, 0);
			break;
		case RAC2_LOADSCREEN_CURVED:
			autosplit_emit(AUTOSPLIT_LOAD_START, R2_AS_LOAD_CURVED, 0);
			break;
		case RAC2_LOADSCREEN_WIPE:
			autosplit_emit(AUTOSPLIT_LOAD_START, R2_AS_LOAD_WIPE, 0);
			break;
		default:
			/* The script's `norm == 0.0` case: a change that costs nothing. */
			break;
		}
	}

	/*
	 * start and reset are the same expression in this script. Emit both and let
	 * the client apply whichever suits its timer.
	 */
	if (g_as.planet == RAC2_PLANET_ARANOS &&
	    g_as.player_state == RAC2_START_STATE && p->player_state == 0) {
		autosplit_emit(AUTOSPLIT_RESET, 0, 0);
		autosplit_emit(AUTOSPLIT_START, 0, 0);
	}

	/* Never split entering the Insomniac Museum. */
	if (g_as.planet != p->planet && g_as.planet != RAC2_PLANET_MUSEUM)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_PLANET, g_as.planet);

	if (g_as.planet == RAC2_PLANET_MAKTAR && g_as.chunk == 1 && p->chunk == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_MAKTAR_ARENA, 0);

	if (g_as.planet == RAC2_PLANET_ARANOS2 &&
	    g_as.clank == RAC2_FLAG_SET && p->clank == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_A2_CLANK, 0);

	if (g_as.planet == RAC2_PLANET_BARLOW &&
	    g_as.barlow_entry == RAC2_FLAG_SET && p->barlow_entry == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_BARLOW_RACE, 0);

	/* The entry split is the hero type, not the flag byte the old client read. */
	if (g_as.planet == RAC2_PLANET_ENDAKO &&
	    g_as.hero_type == 1 && p->hero_type == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_ENDAKO_ENTER, 0);

	if (g_as.planet == RAC2_PLANET_ENDAKO &&
	    g_as.endako_exit == RAC2_FLAG_SET && p->endako_exit == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_ENDAKO_EXIT, 0);

	if (g_as.planet == RAC2_PLANET_TABORA && g_as.chunk == 0 && p->chunk == 1)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_TABORA_CAVES, 0);

	if (g_as.planet == RAC2_PLANET_YEEDIL &&
	    g_as.yeedil_scene == RAC2_YEEDIL_PROTO &&
	    p->yeedil_scene != RAC2_YEEDIL_PROTO)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_PROTOPET, 0);

	g_as_prev = g_as;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define FL AUTOSPLIT_FLAG_FLAT

/*
 * The script's settings.Add list, in its order; DF is what it defaults to true.
 * Then the three load transitions, which are not splits and not optional: the
 * script subtracted their frames whatever the settings said, and so does the
 * client. The Protopet row carries the seven frames the script took off just
 * before it returned true for that split.
 */
static const struct autosplit_desc rac2_autosplits[] = {
	{ R2_AS_PLANET,       AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ R2_AS_PROTOPET,     AUTOSPLIT_SPLIT,      DF | FL, RAC2_PROTOPET_US,
	  "Protopet defeated" },
	{ R2_AS_A2_CLANK,     AUTOSPLIT_SPLIT,      DF,      0, "Aranos 2 Clank swap" },
	{ R2_AS_MAKTAR_ARENA, AUTOSPLIT_SPLIT,      0,       0, "Maktar arena entry" },
	{ R2_AS_BARLOW_RACE,  AUTOSPLIT_SPLIT,      0,       0, "Barlow race entry" },
	{ R2_AS_ENDAKO_ENTER, AUTOSPLIT_SPLIT,      0,       0, "Endako Clank entry" },
	{ R2_AS_ENDAKO_EXIT,  AUTOSPLIT_SPLIT,      0,       0, "Endako Clank exit" },
	{ R2_AS_TABORA_CAVES, AUTOSPLIT_SPLIT,      0,       0, "Tabora caves" },
	{ R2_AS_LOAD_SLIDE,   AUTOSPLIT_LOAD_START, DF | FL, RAC2_LOAD_SLIDE_US,
	  "Load transition: slide" },
	{ R2_AS_LOAD_CURVED,  AUTOSPLIT_LOAD_START, DF | FL, RAC2_LOAD_CURVED_US,
	  "Load transition: curved" },
	{ R2_AS_LOAD_WIPE,    AUTOSPLIT_LOAD_START, DF | FL, RAC2_LOAD_WIPE_US,
	  "Load transition: wipe" }
};

#undef DF
#undef RT
#undef FL

static const struct autosplit_desc *rac2_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(rac2_autosplits) / sizeof(rac2_autosplits[0]));
	return rac2_autosplits;
}

static void rac2_on_enter(void)
{
	/* A fresh process: nothing is forced, nothing has been seen yet. */
	g_fastload_on     = 0;
	g_prev_load_count = 0xFF;
	g_shortcut_index  = 0xFFFFFFFFu;

	memset(&g_as, 0, sizeof(g_as));
	memset(&g_as_prev, 0, sizeof(g_as_prev));
	g_as_primed = 0;
}

/*
 * RAC2Form_Load's loading-screen watcher, plus the autosplit watcher. The
 * loading-screen half fires once per change of the load counter, acts only on
 * the final load screen, and only the A1 (planet 0) part of it is conditional
 * on the planet.
 */
static void rac2_on_tick(const struct game_hot *hot)
{
	u8 count = g_load_count;

	rac2_autosplit_tick();

	if (count == g_prev_load_count) return;
	g_prev_load_count = count;

	if (count != 2) return;

	/* The load is over: put fast loads back where the toggle wants them. */
	rac2_fastload_restore();

	/* Everything below is A1-only, exactly as the old handler had it. */
	if (hot->current_planet != 0) return;

	/* Auto buffer charge for resets. */
	mem_write_u32(RAC2_CHARGE_BUFFER, 30);

	/* A new file starts without infinite ammo, checkbox and all. */
	features_set(R2_INFINITE_AMMO, 0);

	if (g_auto_anypct) rac2_reset_anypct();
	if (g_auto_ngplus) rac2_reset_menu_storage();
}

/* -------------------------------------------------------------- describe */

static const char * const rac2_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Collectables",
	"Savefile",
	"Cosmetics"
};

#define G_CHEATS       0
#define G_PLAYER       1
#define G_PROGRESS     2
#define G_COLLECTABLES 3
#define G_SAVEFILE     4
#define G_COSMETICS    5

/* Indexed by RAC2_RO_*; the order is part of the wire contract for this game. */
static const char * const rac2_readouts[] = {
	"Bolts",
	"Savefile helper",
	"Raritanium",
	"Challenge mode",
	"Health XP",
	"QE save offset",
	"Chargeboot front",
	"Chargeboot back",
	"Chargeboot tint"
};

#define NR (u8)(sizeof(rac2_readouts) / sizeof(rac2_readouts[0]))
#define NO FEATURE_NO_READOUT
#define WC FEATURE_FLAG_WRITES_CODE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE
/* Protocol 1.3: a toggle qwark reads back out of game memory. */
#define LV FEATURE_FLAG_LIVE
/* Protocol 1.7: the field behind this VALUE is two's complement, `bits` wide. */
#define SG FEATURE_FLAG_SIGNED

static const struct feature_desc rac2_features[] = {
	/* id, kind, group, aux (ENUM: options; VALUE: field bits), flags, readout, min, max, label */
	{ R2_FAST_LOADS,      FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ R2_INFINITE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite ammo" },
	{ R2_FREEZE_HEALTH,   FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Freeze health" },
	{ R2_GHOST,           FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },
	{ R2_INSTA_UPGRADE,   FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Weapon insta-upgrades" },
	{ R2_DEBUG_MODE,      FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "Enable debug mode" },

	{ R2_DIE,             FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ R2_BOLTS,           FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_BOLTS,      0, 0, "Bolts" },
	{ R2_RARITANIUM,      FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_RARITANIUM, 0, 0, "Raritanium" },
	{ R2_CHALLENGE,       FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_CHALLENGE,  0, 255, "Challenge mode" },
	/* Health XP is a signed 32-bit field, so the row says so and stays unbounded. */
	{ R2_HEALTH_XP,       FEATURE_VALUE,  G_PLAYER,  32, SG, RAC2_RO_HEALTH_XP,  0, 0, "Health XP" },
	{ R2_SET_RESPAWN,     FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Set respawn point" },
	{ R2_STORE_SWINGSHOT, FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Store Swingshot" },
	{ R2_DEATH_BOSSES,    FEATURE_TOGGLE, G_PLAYER,   0, 0,  NO, 0, 0, "Reset bosses on death" },
	{ R2_DEATH_PBOLTS,    FEATURE_TOGGLE, G_PLAYER,   0, 0,  NO, 0, 0, "Reset platinum bolts on death" },

	{ R2_RESET_ANYPCT,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset any% manips" },
	{ R2_RESET_MENUS,     FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Reset NG+ manips/menus" },
	{ R2_SETUP_NGPLUS,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "NG+ setup" },
	{ R2_SETUP_NO_IMG,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "NG+ No IMG setup" },
	{ R2_SETUP_ALL_MISSION, FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "NG+ All Missions setup" },
	{ R2_MAKTAR_SLOTS,    FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Maktar slots" },
	/*
	 * The two weapon actions the other three games have. Both pass over a weapon
	 * the player does not have rather than handing it out, and the labels are the
	 * other games' word for word so the client lays them out together.
	 */
	{ R2_MAX_LEVELS,      FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Max all weapon levels" },
	{ R2_MAX_AMMO,        FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Max all weapon ammo" },
	{ R2_AUTO_ANYPCT,     FEATURE_TOGGLE, G_PROGRESS, 0, 0,  NO, 0, 0, "Auto-reset (any%)" },
	{ R2_AUTO_NGPLUS,     FEATURE_TOGGLE, G_PROGRESS, 0, 0,  NO, 0, 0, "Auto-reset (NG+)" },
	/*
	 * The QE offset is the signed halfword at selectedSaveSlot, and the old
	 * dialog's default was -1. min and max stay 0: the width is the range.
	 */
	{ R2_QE_OFFSET,       FEATURE_VALUE,  G_PROGRESS, 16, SG, RAC2_RO_QE_OFFSET, 0, 0, "QE save write-offset" },

	{ R2_UNLOCK_PBOLTS,   FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all platinum bolts" },
	{ R2_RESET_PBOLTS,    FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset platinum bolts" },
	{ R2_UNLOCK_NANOTECH, FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all nanotech boosts" },
	{ R2_RESET_NANOTECH,  FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset nanotech boosts" },
	{ R2_UNLOCK_SKILL,    FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all skill points" },
	{ R2_RESET_SKILL,     FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset skill points" },

	/*
	 * Protocol 1.9: one pair, and it is the same pair the PC's save-file manager
	 * drives. The two "Save manager" rows that used to carry the flags wrote a
	 * tempsave file and are retired with their ids.
	 */
	{ R2_SET_ASIDE,       FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ R2_LOAD_ASIDE,      FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" },

	{ R2_CB_PRIMARY_FRONT, FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_FRONT, 0, 0, "Chargeboots primary front" },
	{ R2_CB_PRIMARY_BACK,  FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_BACK,  0, 0, "Chargeboots primary back" },
	{ R2_CB_TINT_FRONT,    FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_TINT,  0, 0, "Chargeboots tint" }
};

#undef NO
#undef WC
#undef SA
#undef LA
#undef LV
#undef SG

static const struct game_describe rac2_describe_table = {
	rac2_groups,   (u8)(sizeof(rac2_groups) / sizeof(rac2_groups[0])),
	rac2_readouts, NR,
	rac2_features, (u8)(sizeof(rac2_features) / sizeof(rac2_features[0])),
	0              /* no toggle ships auto-flagged */
};

#undef NR

static const struct game_describe *rac2_describe(void)
{
	return &rac2_describe_table;
}

/* --------------------------------------------------------------- toggles */

/*
 * freezeAmmoCheckbox did two things: filled 136 bytes of the ammo array with
 * 0x7FFFFFFF and nopped the decrement. Turning it off only restored the
 * instruction, so the ammo stays where it was put; that is kept. The 136 bytes
 * are ammo[12] through ammo[45] now that the array has a base and a stride, and
 * the window is the one the old checkbox wrote and no wider.
 */
static int rac2_infinite_ammo(int on)
{
	u8 full[RAC2_AMMO_FILL_LEN];
	u32 i;
	int rc;

	if (!on) return classic_patch_toggle(&rac2_ammo, 0);

	for (i = 0; i < sizeof(full); i += 4) be32_put(full + i, 0x7FFFFFFFu);

	rc = mem_write(RAC2_AMMO_ARRAY + 4u * RAC2_AMMO_FILL_FIRST, full, sizeof(full));
	if (rc != ST_OK) return rc;

	return patch_apply(&rac2_ammo);
}

/*
 * checkBoxExp froze the experience economy byte to 100 and wrote a 0 on the way
 * out. The freeze table writes unconditionally where the old sub only wrote on
 * change; the value in memory ends up the same either way.
 */
static int rac2_insta_upgrade(int on)
{
	int id;

	if (on) return freeze_add(RAC2_EXP_ECONOMY, 1, 100, NULL);

	id = freeze_find(RAC2_EXP_ECONOMY, 1);
	if (id >= 0) freeze_remove((u8)id);
	return mem_write_u8(RAC2_EXP_ECONOMY, 0);
}

static int rac2_freeze_health(int on)
{
	int id;

	if (on) return freeze_add(RAC2_FREEZE_HEALTH, 4, 42069, NULL);

	id = freeze_find(RAC2_FREEZE_HEALTH, 4);
	if (id < 0) return ST_OK;
	return freeze_remove((u8)id);
}

static int rac2_set_toggle(u8 id, int on)
{
	int rc;

	switch (id) {
	case R2_FAST_LOADS:
		rc = classic_patch_toggle(&rac2_fastload, on);
		if (rc == ST_OK) g_fastload_on = on;
		return rc;

	case R2_INFINITE_AMMO:  return rac2_infinite_ammo(on);
	case R2_FREEZE_HEALTH:  return rac2_freeze_health(on);
	case R2_GHOST:          return classic_ghost(RAC2_GHOST_TIMER, on);
	case R2_INSTA_UPGRADE:  return rac2_insta_upgrade(on);
	case R2_DEBUG_MODE:     return mem_write_u8(RAC2_DEBUG_FEATURES, on ? 1 : 0);

	/* The last four are qwark-side state, not memory. */
	case R2_DEATH_BOSSES:   g_death_bosses = on; return ST_OK;
	case R2_DEATH_PBOLTS:   g_death_pbolts = on; return ST_OK;
	case R2_AUTO_ANYPCT:    g_auto_anypct  = on; return ST_OK;
	case R2_AUTO_NGPLUS:    g_auto_ngplus  = on; return ST_OK;

	default:                return ST_NOT_FOUND;
	}
}

/*
 * Protocol 1.3. Debug mode is one byte the game owns, so its checkbox follows
 * memory rather than the last thing qwark wrote. Tick thread only.
 */
static int rac2_toggle_read(u8 id, int *on)
{
	u8 b = 0;
	int rc;

	if (id != R2_DEBUG_MODE) return ST_NOT_FOUND;

	rc = mem_read_u8(RAC2_DEBUG_FEATURES, &b);
	if (rc != ST_OK) return rc;

	*on = (b != 0);
	return ST_OK;
}

/* -------------------------------------------------------------- positions */

static int rac2_save_blob(u8 *blob, u8 *len)
{
	int rc = mem_read(RAC2_COORDS, blob, RAC2_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = RAC2_POS_BLOB_LEN;
	return ST_OK;
}

static int rac2_load_blob(const u8 *blob, u8 len)
{
	if (len == 0 || len > QWARK_MAX_BLOB) return ST_BAD_ARG;
	return mem_write(RAC2_COORDS, blob, len);
}

static int rac2_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

/* rac2.cs planetsList, index by index: the game's own planet numbering. */
static const char * const rac2_planets[] = {
	"Aranos",
	"Oozla",
	"Maktar",
	"Endako",
	"Barlow",
	"Feltzin",
	"Notak",
	"Siberius",
	"Tabora",
	"Dobbo",
	"Hrugis",
	"Joba",
	"Todano",
	"Boldan",
	"Aranos 2",
	"Gorn",
	"Snivelak",
	"Smolg",
	"Damosel",
	"Grelbin",
	"Yeedil",
	"Insomniac Museum",
	"Dobbo Orbit",
	"Damosel Orbit",
	"Slim Cognito",
	"Wupash",
	"Jamming Array"
};

u8 rac2_planet_count(void)
{
	return (u8)(sizeof(rac2_planets) / sizeof(rac2_planets[0]));
}

static const char * const *rac2_planet_names(u8 *count)
{
	*count = rac2_planet_count();
	return rac2_planets;
}

static int rac2_moby_table(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride)
{
	*table_ptr_addr     = RAC2_MOBY_TABLE;
	*table_end_ptr_addr = RAC2_MOBY_TABLE_END;
	*stride             = RAC2_MOBY_STRIDE;
	return ST_OK;
}

/* ---------------------------------------------------------------- vtable */

const struct game_api rac2_game = {
	{ "NPEA00386", "BCES01503", NULL, NULL },
	GAME_RAC2,

	RAC2_FP_ADDR,
	rac2_fp,
	rac2_fp_patched,
	sizeof(rac2_fp),

	0,                    /* no quit hook is known for RaC2 yet */
	RAC2_INPUTS,

	rac2_init,

	rac2_hot,
	(u8)(sizeof(rac2_hot) / sizeof(rac2_hot[0])),
	rac2_hot_decode,

	rac2_describe,

	rac2_set_toggle,
	rac2_toggle_read,
	rac2_trigger,
	rac2_set_value,
	NULL,                 /* no ENUM features */

	rac2_save_blob,
	rac2_load_blob,
	rac2_blob_xyz,

	rac2_planet_names,
	rac2_planet_load,
	rac2_die,
	rac2_load_setaside,

	rac2_unlock_list,
	rac2_unlock_read,
	rac2_unlock_set,

	rac2_levelflags_get,
	rac2_levelflags_reset,
	rac2_levelflags_set,

	rac2_moby_table,

	rac2_on_enter,
	NULL,                 /* on_quit */
	rac2_on_tick,

	rac2_autosplit_describe
};
