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

#include <string.h>

/* See rac1.h for what these two are and why both are accepted. */
const u8 rac1_fp[4]         = { 0x30, 0x64, 0x9C, 0xE0 };
const u8 rac1_fp_patched[4] = { 0x30, 0x64, 0x00, 0x00 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Two reads every tick and five staggered slow ones:
 *
 *   0  inputs   analogs at +0x00, pad mask at +0xB0                every tick
 *   1  player   planet +0x00, bolts +0x30, NG+ goodies +0x60,      every tick
 *               goodies menu +0x63, coords +0xF0. The gold-item bytes
 *               at +0x38 ride along but the unlock snapshot owns those
 *   2  debug    update options +0x00, mode control +0x0C           every 8th
 *   3  ngplus   challenge mode                                     every 8th
 *   4  jankpot  timer +0x00, bolts +0x04                           every 8th
 *   5  jankpot  state                                              every 8th
 *   6  savefile helper byte, then its three request bytes          every 8th
 *
 * The phases are 0..4, so a tick costs two reads plus at most one: 2.625 reads
 * per tick on average, three at worst.
 */
static const struct game_hot_block rac1_hot[] = {
	{ RAC1_HOT_INPUTS_ADDR,   RAC1_HOT_INPUTS_LEN,   1, 0 },
	{ RAC1_HOT_PLAYER_ADDR,   RAC1_HOT_PLAYER_LEN,   1, 0 },
	{ RAC1_DEBUG_UPDATE,      0x10,                  8, 0 },
	{ RAC1_NGPLUS_STATE,      4,                     8, 1 },
	{ RAC1_JANKPOT_TIMER,     8,                     8, 2 },
	{ RAC1_JANKPOT_STATE,     4,                     8, 3 },
	{ RAC1_SAVEFILE_HELPER,   4,                     8, 4 }
};

#define HOT_INPUTS   0
#define HOT_PLAYER   1
#define HOT_DEBUG    2
#define HOT_NGPLUS   3
#define HOT_JANKPOT  4
#define HOT_JKSTATE  5
#define HOT_SAVEFILE 6

#define OFF_INPUTS      (RAC1_INPUTS - RAC1_ANALOGS)                  /* 0xB0 */
#define OFF_BOLTS       (RAC1_BOLTS - RAC1_CURRENT_PLANET)            /* 0x30 */
#define OFF_NGGOODIES   (RAC1_NGPLUS_GOODIES - RAC1_CURRENT_PLANET)   /* 0x60 */
#define OFF_GOODIES     (RAC1_GOODIES_MENU - RAC1_CURRENT_PLANET)     /* 0x63 */
#define OFF_COORDS      (RAC1_COORDS - RAC1_CURRENT_PLANET)           /* 0xF0 */
#define OFF_MODECONTROL (RAC1_DEBUG_MODE - RAC1_DEBUG_UPDATE)         /* 0x0C */

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

static const struct feature_desc rac1_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
	{ F_FAST_LOADS,      FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ F_INFINITE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite ammo" },
	{ F_INFINITE_HEALTH, FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite health" },
	{ F_GHOST,           FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },
	{ F_GOODIES,         FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Goodies menu" },

	{ F_DIE,             FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ F_BOLTS,           FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC1_RO_BOLTS, 0, 0, "Bolts" },
	{ F_MAX_AMMO,        FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Max ammo, all weapons" },

	{ F_DREK_SKIP,       FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Drek skip" },
	{ F_DREK_CUTSCENE,   FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Drek cutscene" },
	{ F_FORCE_OKAY_LOAD, FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Force okay load" },
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

	{ F_DBG_RATCHET,     FEATURE_TOGGLE, G_DEBUG,    0, 0,  NO, 0, 0, "Update Ratchet" },
	{ F_DBG_MOBYS,       FEATURE_TOGGLE, G_DEBUG,    0, 0,  NO, 0, 0, "Update mobys" },
	{ F_DBG_PARTICLES,   FEATURE_TOGGLE, G_DEBUG,    0, 0,  NO, 0, 0, "Update particles" },
	{ F_DBG_CAMERA,      FEATURE_ENUM,   G_DEBUG,    3, 0,  RAC1_RO_CAMERA, 0, 2, "Camera mode" }
};

#undef NO
#undef WC
#undef SA
#undef LA

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

	NULL,                 /* on_enter */
	NULL,                 /* on_quit */
	NULL                  /* on_tick: RaC1 has no game-side watcher */
};
