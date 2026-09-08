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

#include <string.h>

/* See rac2.h for what these two are and why both are accepted. */
const u8 rac2_fp[4]         = { 0x4B, 0xFF, 0xEA, 0x69 };
const u8 rac2_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Three reads every tick and two staggered slow ones:
 *
 *   0  inputs   load screen type +0x07, count +0x0B, pad +0x1E0,       every tick
 *               analogs +0x3BC
 *   1  state    QE offset +0x00, planet +0x170, bolts +0x1C4,          every tick
 *               raritanium +0x1C8, challenge +0x1D6, health XP +0x1D8
 *   2  player   the coordinate Vec4 and the rotation behind it         every tick
 *   3  savefile helper byte and its four request bytes                 every 8th
 *   4  chargeboot colour words                                        every 8th
 *
 * The two slow blocks sit on phases 0 and 4, so a tick costs three reads plus at
 * most one: 3.25 reads per tick on average, four at worst.
 */
static const struct game_hot_block rac2_hot[] = {
	{ RAC2_HOT_INPUTS_ADDR, RAC2_HOT_INPUTS_LEN, 1, 0 },
	{ RAC2_HOT_STATE_ADDR,  RAC2_HOT_STATE_LEN,  1, 0 },
	{ RAC2_HOT_PLAYER_ADDR, RAC2_HOT_PLAYER_LEN, 1, 0 },
	{ RAC2_SF_LOAD_ASIDE,   8,                   8, 0 },
	{ RAC2_CB_PRIMARY_FRONT, 0x14,               8, 4 }
};

#define HOT_INPUTS   0
#define HOT_STATE    1
#define HOT_PLAYER   2
#define HOT_SAVEFILE 3
#define HOT_COLOURS  4

#define OFF_LOADTYPE  (RAC2_LOADSCREEN_TYPE  - RAC2_HOT_INPUTS_ADDR)   /* 0x007 */
#define OFF_LOADCOUNT (RAC2_LOADSCREEN_COUNT - RAC2_HOT_INPUTS_ADDR)   /* 0x00B */
#define OFF_PAD       (RAC2_INPUTS           - RAC2_HOT_INPUTS_ADDR)   /* 0x1E0 */
#define OFF_ANALOGS   (RAC2_ANALOGS          - RAC2_HOT_INPUTS_ADDR)   /* 0x3BC */

#define OFF_PLANET     (RAC2_CURRENT_PLANET - RAC2_HOT_STATE_ADDR)     /* 0x170 */
#define OFF_BOLTS      (RAC2_BOLTS          - RAC2_HOT_STATE_ADDR)     /* 0x1C4 */
#define OFF_RARITANIUM (RAC2_RARITANIUM     - RAC2_HOT_STATE_ADDR)     /* 0x1C8 */
#define OFF_CHALLENGE  (RAC2_CHALLENGE_MODE - RAC2_HOT_STATE_ADDR)     /* 0x1D6 */
#define OFF_HEALTH_XP  (RAC2_HEALTH_XP      - RAC2_HOT_STATE_ADDR)     /* 0x1D8 */

#define OFF_SF_HELPER  (RAC2_SF_HELPER      - RAC2_SF_LOAD_ASIDE)      /* 2 */

#define OFF_CB_BACK    (RAC2_CB_PRIMARY_BACK - RAC2_CB_PRIMARY_FRONT)  /* 0x04 */
#define OFF_CB_TINT    (RAC2_CB_TINT_FRONT   - RAC2_CB_PRIMARY_FRONT)  /* 0x10 */

/*
 * What the watcher needs and struct game_hot has no room for. Filled by the
 * decode, read by on_tick, so the watcher costs no syscall of its own.
 */
static u8 g_load_count;

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
	}

	if (state != NULL) {
		out->current_planet = state[OFF_PLANET + 3];
		out->readout[RAC2_RO_BOLTS]      = be32_get(state + OFF_BOLTS);
		out->readout[RAC2_RO_RARITANIUM] = be32_get(state + OFF_RARITANIUM);
		out->readout[RAC2_RO_CHALLENGE]  = state[OFF_CHALLENGE];
		out->readout[RAC2_RO_HEALTH_XP]  = be32_get(state + OFF_HEALTH_XP);
		/* The QE offset is a signed 16-bit field; report the raw halfword. */
		out->readout[RAC2_RO_QE_OFFSET]  = be16_get(state);
	}

	if (player != NULL) {
		for (i = 0; i < 3; i++) out->pos[i] = bef32_get(player + i * 4);
	}

	/* The client greys the savefile actions on this one. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC2_RO_SAVEFILE] = blocks[HOT_SAVEFILE][OFF_SF_HELPER];

	if (blocks[HOT_COLOURS] != NULL) {
		const u8 *c = blocks[HOT_COLOURS];
		out->readout[RAC2_RO_CB_FRONT] = classic_cb_to_rgb(be32_get(c));
		out->readout[RAC2_RO_CB_BACK]  = classic_cb_to_rgb(be32_get(c + OFF_CB_BACK));
		out->readout[RAC2_RO_CB_TINT]  = classic_cb_to_rgb(be32_get(c + OFF_CB_TINT));
	}
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

static void rac2_on_enter(void)
{
	/* A fresh process: nothing is forced, nothing has been seen yet. */
	g_fastload_on     = 0;
	g_prev_load_count = 0xFF;
	g_shortcut_index  = 0xFFFFFFFFu;
}

/*
 * RAC2Form_Load's loading-screen watcher. It fires once per change of the load
 * counter, acts only on the final load screen, and only the A1 (planet 0) part
 * of it is conditional on the planet.
 */
static void rac2_on_tick(const struct game_hot *hot)
{
	u8 count = g_load_count;

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

static const struct feature_desc rac2_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
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
	{ R2_HEALTH_XP,       FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_HEALTH_XP,  0, 0, "Health XP" },
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
	{ R2_AUTO_ANYPCT,     FEATURE_TOGGLE, G_PROGRESS, 0, 0,  NO, 0, 0, "Auto-reset (any%)" },
	{ R2_AUTO_NGPLUS,     FEATURE_TOGGLE, G_PROGRESS, 0, 0,  NO, 0, 0, "Auto-reset (NG+)" },
	{ R2_QE_OFFSET,       FEATURE_VALUE,  G_PROGRESS, 0, 0,  RAC2_RO_QE_OFFSET, 0, 0xFFFF, "QE save write-offset" },

	{ R2_RESET_PBOLTS,    FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset platinum bolts" },
	{ R2_UNLOCK_PBOLTS,   FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all platinum bolts" },
	{ R2_RESET_NANOTECH,  FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset nanotech boosts" },
	{ R2_UNLOCK_NANOTECH, FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all nanotech boosts" },
	{ R2_RESET_SKILL,     FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset skill points" },
	{ R2_UNLOCK_SKILL,    FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all skill points" },

	/*
	 * Four savefile requests, named after the four old buttons. The save
	 * manager's pair carries the 1.2 flags because those are the two the file
	 * manager on the PC drives; the other two are the main form's own buttons.
	 */
	{ R2_LOAD_ASIDE,      FEATURE_ACTION, G_SAVEFILE, 0, 0,  NO, 0, 0, "Load file" },
	{ R2_SET_ASIDE,       FEATURE_ACTION, G_SAVEFILE, 0, 0,  NO, 0, 0, "Set aside file" },
	{ R2_MGR_SAVE,        FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Save manager: save file" },
	{ R2_MGR_LOAD,        FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Save manager: load file" },

	{ R2_CB_PRIMARY_FRONT, FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_FRONT, 0, 0, "Chargeboots primary front" },
	{ R2_CB_PRIMARY_BACK,  FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_BACK,  0, 0, "Chargeboots primary back" },
	{ R2_CB_TINT_FRONT,    FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_TINT,  0, 0, "Chargeboots tint" }
};

#undef NO
#undef WC
#undef SA
#undef LA
#undef LV

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
 * freezeAmmoCheckbox did two things: filled the 136-byte ammo array with
 * 0x7FFFFFFF and nopped the decrement. Turning it off only restored the
 * instruction, so the ammo stays where it was put; that is kept.
 */
static int rac2_infinite_ammo(int on)
{
	u8 full[136];
	u32 i;
	int rc;

	if (!on) return classic_patch_toggle(&rac2_ammo, 0);

	for (i = 0; i < sizeof(full); i += 4) be32_put(full + i, 0x7FFFFFFFu);

	rc = mem_write(RAC2_AMMO_ARRAY, full, sizeof(full));
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
	rac2_on_tick
};
