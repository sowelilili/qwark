/*
 * Tools of Destruction (NPEA00452 v01.00): the handlers behind tod_game.c's
 * descriptors, the positions, the planet load and the unlock table. Every
 * address is tod.h's; everything is a data write, so all of it works under
 * RPCS3 over PINE exactly as on a console.
 *
 * All of it runs on the tick thread, through the command ring.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "classic.h"
#include "switches_bins.h"
#include "../core/mem.h"
#include "../core/switches.h"
#include "../core/util.h"

#include <string.h>

/* -------------------------------------------------------------- the state */

/*
 * Build 59: Infinite ammo is a code patch, as the other games' is, and has no
 * state here; until then a hold kept each weapon's ammo from this struct.
 */
struct tod_panel_state {
	/* Revision 1.29: qwark's own load's short fade (tod_fade_arm). */
	u8  fade_armed;
	u8  fade_seen;         /* its request was seen waiting */
	u16 fade_ticks;

	/* Revision 1.29: Fast loads forced for qwark's own load (tod_fl_force), and the map's pick. */
	u8  fl_on;             /* the toggle's setting */
	u8  fl_forced;         /* on for a load only, to go back after it */
	u8  fl_loading;        /* that load seen under way */
	u8  map_armed;         /* the galactic map qwark opened is up (tod_galmap_up) */
	u16 fl_ticks;
	s16 map_level;         /* its pick seen, -1 none yet */

	/* Revision 1.29: Remove cutscenes, wanted, and the ticks to the next look. */
	u8  cut_wanted;
	u16 cut_wait;
	/* The table last seen, and the bytes put at 0 in it: a look checks these alone. */
	u32 cut_toc;
	u32 cut_table;
	u8  cut_n;
	u32 cut_at[TOD_CUT_KEEP];

	/* 2026-11-12: Remove black screens (tod_black_tick). */
	u8  bs_on;
	u8  bs_leadin;         /* the movie player's hold under way began as a lead-in */
	u8  bs_state;          /* its state at the last look */
	u16 bs_age;            /* ticks since the lua_State last changed, held at 0xFFFF */
	u32 bs_lua;            /* that lua_State */

	/* 2026-11-13: a Die asked for from the pause, for the game's next play (tod_die). */
	u8  die_pending;
	u8  die_playing;       /* the pause seen closed: play has begun */
	u16 die_ticks;         /* ticks of play since */
	u32 die_hero;          /* the hero it was for */
};

/*
 * 2026-10-28: and what outlives a process, which tod_panel_reset leaves alone:
 * Fast loads' patch, whose originals come out of the core's pool once (init),
 * and since build 59 Infinite ammo's.
 */
struct tod_panel_area {
	struct tod_panel_state s;
	struct patch_def fastload;
	struct patch_def ammo;
	u8 init_done;
};

#if QWARK_GAME_MODULE
#define TPA QWARK_STATE_AT(struct tod_panel_area, QWARK_STATE_PANEL_OFF)
typedef char tod_panel_state_fits[sizeof(struct tod_panel_area) <= QWARK_STATE_PANEL_MAX ? 1 : -1];
#else
static struct tod_panel_area g_tod_panel;
#define TPA (&g_tod_panel)
#endif
#define TP (&TPA->s)

static void tod_die_tick(void);

void tod_panel_reset(void)
{
	memset(TP, 0, sizeof(*TP));
}

/*
 * Fast loads, tod.h (TOD_FASTLOAD_AT): LoadScreen_Update's twenty-five words
 * from 0x25E588 on a console. Under RPCS3 the code switch's second half is the
 * same words (src/games/switches/sw_tod.s, fast_loads_on), but for the five
 * branches out of them, which the link points at the same places; test/
 * test_tod_game.c holds the two to that. Revision 1.29: then the three sites of
 * qwark's own pieces, the branches the switches' link computed, whose caves
 * (switches_own.c) go in first and read the flag byte themselves.
 */
static const struct patch_word tod_fastload_words[] = {
	{ 0x0025E588u, 0x81227D74u },   /* lwz r9, 0x7D74(r2): &g_loadScreenLoadDone */
	{ 0x0025E58Cu, 0x89290000u },   /* lbz r9, 0(r9) */
	{ 0x0025E590u, 0x83A27D78u },   /* lwz r29, 0x7D78(r2): &g_loadScreenType, the pick's too */
	{ 0x0025E594u, 0x807D0000u },   /* lwz r3, 0(r29) */
	{ 0x0025E598u, 0x2C830001u },   /* cmpwi cr1, r3, 1 */
	{ 0x0025E59Cu, 0x4186004Cu },   /* beq cr1, 0x25E5E8: the arrival is up, as the game */
	{ 0x0025E5A0u, 0x2F090000u },   /* cmpwi cr6, r9, 0 */
	{ 0x0025E5A4u, 0x409A000Cu },   /* bne cr6, 0x25E5B0: the load is done, the cut */
	{ 0x0025E5A8u, 0x409E0040u },   /* bne cr7, 0x25E5E8: a cinematic plays, as the game */
	{ 0x0025E5ACu, 0x48000068u },   /* b 0x25E614: none, the next long-load screen */
	{ 0x0025E5B0u, 0xC0227D84u },   /* lfs f1, 0x7D84(r2): 1.0f */
	{ 0x0025E5B4u, 0x48000785u },   /* bl 0x25ED38: the screen black at once */
	{ 0x0025E5B8u, 0x4BE2693Du },   /* bl 0x84EF4: the cinematic stopped */
	{ 0x0025E5BCu, 0x4BE2686Du },   /* bl 0x84E28: the player reset */
	{ 0x0025E5C0u, 0x3D20101Du },   /* lis r9, 0x101D: revision 1.29, the transition prop's handles */
	{ 0x0025E5C4u, 0x3D400400u },   /* lis r10, 0x0400 */
	{ 0x0025E5C8u, 0x394AFFFFu },   /* addi r10, r10, -1: 0x03FFFFFF */
	{ 0x0025E5CCu, 0x3960FFFFu },   /* li r11, -1 */
	{ 0x0025E5D0u, 0x914960E0u },   /* stw r10, 0x60E0(r9): g_transitionPropOwnerHandle, invalid */
	{ 0x0025E5D4u, 0x916960E4u },   /* stw r11, 0x60E4(r9) */
	{ 0x0025E5D8u, 0x914960E8u },   /* stw r10, 0x60E8(r9): g_transitionPropHandle, invalid */
	{ 0x0025E5DCu, 0x916960ECu },   /* stw r11, 0x60EC(r9) */
	{ 0x0025E5E0u, 0x38000001u },   /* li r0, 1 */
	{ 0x0025E5E4u, 0x901D0000u },   /* stw r0, 0(r29): the arrival's 1 */
	{ 0x0025E5E8u, 0x48000014u },   /* b 0x25E5FC: the return */
	{ SW_TOD_SITE_OWN_DEPARTURE_ADDR, SW_TOD_SITE_OWN_DEPARTURE },   /* LevelTransition_update, a planet load */
	{ SW_TOD_SITE_OWN_FMV_CUT_ADDR,   SW_TOD_SITE_OWN_FMV_CUT },     /* LevelTransition_update, an FMV open */
	{ SW_TOD_SITE_OWN_FMV_IN_ADDR,    SW_TOD_SITE_OWN_FMV_IN }       /* LevelTransition_waitLoad, the data read */
};

#define TOD_FASTLOAD_DEF_WORDS (TOD_FASTLOAD_NWORDS + TOD_FASTLOAD_NSITES)

typedef char tod_fastload_count_ok[sizeof(tod_fastload_words) / sizeof(tod_fastload_words[0]) ==
                                   TOD_FASTLOAD_DEF_WORDS ? 1 : -1];
typedef char tod_fastload_sites_ok[SW_TOD_OWN_NSITES == TOD_FASTLOAD_NSITES && SW_TOD_OWN_NCAVES == 1 ? 1 : -1];

static const struct patch_def tod_fastload_init = {
	"Fast loads", PATCH_KIND_FEATURE, tod_fastload_words, (u16)TOD_FASTLOAD_DEF_WORDS, NULL
};

/* Build 59: Infinite ammo, Weapon_useAmmo's call of WeaponSlot_addAmmo nopped (tod.h). */
static const struct patch_word tod_ammo_words[] = {
	{ TOD_AMMO_INSTR, 0x60000000u }
};

static const struct patch_def tod_ammo_init = {
	"Infinite ammo", PATCH_KIND_FEATURE, tod_ammo_words, 1, NULL
};

void tod_panel_init(void)
{
	if (TPA->init_done) return;
	TPA->init_done = 1;
	TPA->fastload = tod_fastload_init;
	TPA->fastload.originals = patch_pool_alloc(TPA->fastload.count);
	TPA->ammo = tod_ammo_init;
	TPA->ammo.originals = patch_pool_alloc(TPA->ammo.count);
}

/* ----------------------------------------------------------------- tables */

/*
 * The weapons' XP per level, V1 to V10, from the game's weapon.csv (WeaponConfig
 * XP, which WeaponSlot_setXp counts the level from). Combuster and the Fusion
 * Grenade share a row. Whole points; the game keeps them as f32.
 */
static const u32 tod_xp[15][10] = {
	{ 0, 1000, 2200,  3640,  5368,  5500,  20000,  37400,  58280,  83336 },  /*  1 Combuster */
	{ 0, 1000, 2200,  3640,  5368,  5500,  20000,  37400,  58280,  83336 },  /*  2 Fusion Grenade */
	{ 0,  800, 1760,  2912,  4294,  8500,  16000,  25000,  35800,  48760 },  /*  3 Shock Ravager */
	{ 0, 1000, 2200,  3640,  5368,  6500,  14000,  23000,  33800,  46760 },  /*  4 Tornado Launcher */
	{ 0, 2500, 5500,  9100, 13420, 16500,  39000,  66000,  98400, 137280 },  /*  5 Buzz Blades */
	{ 0, 1500, 3300,  5460,  8052,  8100,  23000,  40880,  62336,  88083 },  /*  6 Predator Launcher */
	{ 0, 3500, 7700, 12740, 18788, 22500,  30000,  39000,  49800,  62760 },  /*  7 Alpha Disruptor */
	{ 0, 4000, 8800, 14560, 21472, 33000,  48000,  66000,  87600, 113520 },  /*  8 Pyro Blaster */
	{ 0,  600, 1320,  2184,  3221,  6500,  14000,  23000,  33800,  46760 },  /*  9 Plasma Beasts */
	{ 0, 2000, 4400,  7280, 10736, 11000,  33500,  60500,  92900, 131780 },  /* 10 Shard Reaper */
	{ 0, 6000, 13200, 21840, 32208, 40500, 63000,  90000, 122400, 161280 },  /* 11 Negotiator */
	{ 0, 1500, 3300,  5460,  8052, 11000,  26000,  44000,  65600,  91520 },  /* 12 Nano-Swarmers */
	{ 0, 3000, 6600, 10920, 16104, 32500,  40000,  49000,  59800,  72760 },  /* 13 Mag-Net Launcher */
	{ 0, 5000, 11000, 18200, 26840, 40000, 55000,  73000,  94600, 120520 },  /* 14 Razor Claws */
	{ 0, 15000, 33000, 54600, 80520, 81000, 111000, 147000, 190200, 242040 } /* 15 R.Y.N.O. IV */
};

u32 tod_weapon_xp(u8 wpn, u8 level)
{
	if (wpn < TOD_WPN_FIRST || wpn >= TOD_WPN_DEVICES || level >= TOD_MAX_LEVEL) return 0;
	return tod_xp[wpn - TOD_WPN_FIRST][level];
}

/* Gold bolts per level, the game's own table at 0x1006E58C (g_levelGoldBoltCount). */
static const u8 tod_gb_count[TOD_NPLANETS] = {
	0, 1, 2, 0, 1, 4, 1, 3, 1, 2, 4, 1, 2, 1, 2, 2, 2, 2, 1
};

int tod_gold_bolt(u8 index, u8 *level, u8 *bit)
{
	u8 l, n = index;

	for (l = 0; l < TOD_NPLANETS; l++) {
		if (n < tod_gb_count[l]) {
			*level = l;
			*bit = n;
			return 1;
		}
		n = (u8)(n - tod_gb_count[l]);
	}
	return 0;
}

/* -------------------------------------------------------------- the unlocks */

#define CAT_WEAPONS 0
#define CAT_DEVICES 1
#define CAT_GADGETS 2
#define CAT_ITEMS   3

/*
 * 2026-10-27, the user's first hardware test: the skill points' and the gold
 * bolts' categories are gone (the Collectables buttons cover them), and so are
 * the three items with no use on the panel (tod_unlocks below). Their row ids
 * stay reserved (tod.h).
 */
static const char * const tod_categories[] = {
	"Weapons", "Devices", "Gadgets", "Items"
};

/*
 * Owned, then the weapon's level V1 to V10 (the level byte with the XP the
 * game counts it from), and the ammo, which the game keeps as a float.
 */
static const struct unlock_field_desc tod_fields[4] = {
	{ "Owned", UNLOCK_KIND_FLAG,   0 },
	{ "Level", UNLOCK_KIND_NUMBER, TOD_MAX_LEVEL },
	{ NULL,    UNLOCK_KIND_FLAG,   0 },
	{ "Ammo",  UNLOCK_KIND_NUMBER, 0 }
};

#define OWNED UNLOCK_FIELD_0
#define LEVEL UNLOCK_FIELD_1
#define AMMO  UNLOCK_FIELD_3
#define WPN   (OWNED | LEVEL | AMMO)
#define DEV   (OWNED | AMMO)

/*
 * The weapons' names are the game's text keys' (GEN_KEY_WEAP_*, in WPN_
 * order); the devices' and gadgets' are the inventory keys' (GEN_KEY_INV_*),
 * matched to the WPN_ enum by name, and the items' the OBJ_ enum's.
 */
static const struct game_unlock tod_unlocks[] = {
	{  1, CAT_WEAPONS, WPN, "Combuster" },
	{  2, CAT_WEAPONS, WPN, "Fusion Grenade" },
	{  3, CAT_WEAPONS, WPN, "Shock Ravager" },
	{  4, CAT_WEAPONS, WPN, "Tornado Launcher" },
	{  5, CAT_WEAPONS, WPN, "Buzz Blades" },
	{  6, CAT_WEAPONS, WPN, "Predator Launcher" },
	{  7, CAT_WEAPONS, WPN, "Alpha Disruptor" },
	{  8, CAT_WEAPONS, WPN, "Pyro Blaster" },
	{  9, CAT_WEAPONS, WPN, "Plasma Beasts" },
	{ 10, CAT_WEAPONS, WPN, "Shard Reaper" },
	{ 11, CAT_WEAPONS, WPN, "Negotiator" },
	{ 12, CAT_WEAPONS, WPN, "Nano-Swarmers" },
	{ 13, CAT_WEAPONS, WPN, "Mag-Net Launcher" },
	{ 14, CAT_WEAPONS, WPN, "Razor Claws" },
	{ 15, CAT_WEAPONS, WPN, "R.Y.N.O. IV" },

	{ 16, CAT_DEVICES, DEV, "Mr. Zurkon" },
	{ 17, CAT_DEVICES, DEV, "Confuzzler" },
	{ 18, CAT_DEVICES, DEV, "Deathspring" },
	{ 19, CAT_DEVICES, DEV, "Groovitron" },
	{ 20, CAT_DEVICES, DEV, "Leech Mine" },
	{ 21, CAT_DEVICES, DEV, "Mega Leech" },
	{ 22, CAT_DEVICES, DEV, "Transmorpher" },
	{ 23, CAT_DEVICES, DEV, "Visi-Copter" },

	{ 24, CAT_GADGETS, OWNED, "Heli-Pods" },
	{ 25, CAT_GADGETS, OWNED, "Swingshot" },
	{ 26, CAT_GADGETS, OWNED, "Geo-Laser" },
	{ 27, CAT_GADGETS, OWNED, "Gelanator" },
	{ 28, CAT_GADGETS, OWNED, "Robo-Wings" },
	{ 29, CAT_GADGETS, OWNED, "Gyro-Cycle" },
	{ 30, CAT_GADGETS, OWNED, "Pirate Disguise" },
	{ 31, CAT_GADGETS, OWNED, "Decryptor" },

	{ TOD_UL_OBJ +  0, CAT_ITEMS, OWNED, "Heli-Pack" },
	{ TOD_UL_OBJ +  1, CAT_ITEMS, OWNED, "Thruster-Pack" },
	{ TOD_UL_OBJ +  2, CAT_ITEMS, OWNED, "Hydro-Pack" },
	{ TOD_UL_OBJ +  3, CAT_ITEMS, OWNED, "Grind Boots" },
	{ TOD_UL_OBJ +  4, CAT_ITEMS, OWNED, "Gravity Boots" },
	{ TOD_UL_OBJ +  5, CAT_ITEMS, OWNED, "Charge Boots" },
	{ TOD_UL_OBJ +  6, CAT_ITEMS, OWNED, "Treasure Mapper" },
	{ TOD_UL_OBJ +  7, CAT_ITEMS, OWNED, "Box Breaker" },
	{ TOD_UL_OBJ +  8, CAT_ITEMS, OWNED, "Armor Magnetizer" },
	{ TOD_UL_OBJ +  9, CAT_ITEMS, OWNED, "O2 Mask" },
	{ TOD_UL_OBJ + 10, CAT_ITEMS, OWNED, "Qwark Infobot" },
	{ TOD_UL_OBJ + 11, CAT_ITEMS, OWNED, "Golden Groovitron" },
	/*
	 * OBJ_ 12..14 are not listed. OBJ_VERDIGRIS_UPGRADE is the ship part Pirate
	 * Base's last cutscene hands over with Rykan V, and OBJ_PRAXUS_UPGRADE the one
	 * Zordoom Prison's ship-part vendor would sell; nothing that runs reads either
	 * (the vendors' script, shippart.lua, is loaded by neither level's main.lua),
	 * and neither shows in a menu. OBJ_HEXAGONAL_WASHER is the plumber's joke at
	 * Sargasso, and owning it does nothing.
	 */
};

#undef WPN
#undef DEV

#define TOD_UNLOCK_COUNT ((u8)(sizeof(tod_unlocks) / sizeof(tod_unlocks[0])))

/*
 * The snapshot unlock_list takes (classic.h): the 32 weapon slots and the
 * listed objects, two reads per list.
 */
#define SNAP_WEAPONS   0u
#define SNAP_OBJ       (TOD_NWEAPONS * TOD_WEAPON_STRIDE)              /* 0x280 */
#define SNAP_LEN       (SNAP_OBJ + TOD_NOBJ_LISTED * 4u)
#define SNAP_VALID     (classic_snap_game == GAME_TOD)
typedef char tod_snap_fits[SNAP_LEN <= CLASSIC_SNAP_BYTES ? 1 : -1];

static u32 gb_addr(u8 level)
{
	return TOD_GOLD_BOLTS + (u32)level * TOD_LEVEL_SAVE_LEN;
}

int tod_unlock_list(const struct game_unlock **list, u8 *count,
                    const char * const **categories, u8 *ncategories,
                    const struct unlock_field_desc **fields)
{
	u8 *s = classic_snap;
	int rc;

	classic_snap_game = 0;
	rc = mem_read(TOD_WEAPONS, s + SNAP_WEAPONS, SNAP_OBJ);
	if (rc == ST_OK) rc = mem_read(TOD_OBJECTS, s + SNAP_OBJ, TOD_NOBJ_LISTED * 4u);
	if (rc != ST_OK) return rc;
	classic_snap_game = GAME_TOD;

	*list = tod_unlocks;
	*count = TOD_UNLOCK_COUNT;
	*categories = tod_categories;
	*ncategories = (u8)(sizeof(tod_categories) / sizeof(tod_categories[0]));
	*fields = tod_fields;
	return ST_OK;
}

static u32 slot_addr(u8 wpn)
{
	return TOD_WEAPONS + (u32)wpn * TOD_WEAPON_STRIDE;
}

/* A float the game keeps as a count, as the whole number it is. */
static u32 f32_to_count(u32 bits)
{
	f32 f = f32_from_bits(bits);

	if (!(f > 0.0f)) return 0;
	if (f > 4.0e9f) return 0xFFFFFFFFu;
	return (u32)f;
}

int tod_unlock_read(const struct game_unlock *e, u32 values[4])
{
	const u8 *s = classic_snap;
	u8 id = e->id;

	if (!SNAP_VALID) return ST_NOT_FOUND;

	if (id >= TOD_WPN_FIRST && id < TOD_NWEAPONS) {
		const u8 *w = s + SNAP_WEAPONS + (u32)id * TOD_WEAPON_STRIDE;

		values[0] = w[TOD_WS_OWNED] != 0;
		values[1] = id < TOD_WPN_DEVICES ? (u32)w[TOD_WS_LEVEL] + 1u : 0;
		values[3] = id < TOD_WPN_GADGETS ? f32_to_count(be32_get(w + TOD_WS_AMMO)) : 0;
		return ST_OK;
	}
	if (id >= TOD_UL_OBJ && id < TOD_UL_OBJ + TOD_NOBJ_LISTED) {
		values[0] = be32_get(s + SNAP_OBJ + (u32)(id - TOD_UL_OBJ) * 4u) != 0;
		return ST_OK;
	}
	return ST_NOT_FOUND;
}

/*
 * A weapon's level, the way the game sets one: the level byte, and the XP the
 * game counts the level from (WeaponSlot_setXp resets the level and counts it
 * up from the XP on the next kill, so a level without its XP falls back).
 * Levels past V5 are the challenge mode's Omega ones, which the level byte
 * itself lets through (WeaponSlot_getMaxLevel).
 */
static int set_level(u8 wpn, u32 level)
{
	u32 at = slot_addr(wpn);
	int rc;

	if (wpn >= TOD_WPN_DEVICES) return ST_BAD_ARG;
	if (level < 1 || level > TOD_MAX_LEVEL) return ST_BAD_ARG;
	rc = mem_write_u32(at + TOD_WS_XP, f32_bits((f32)tod_weapon_xp(wpn, (u8)(level - 1))));
	if (rc != ST_OK) return rc;
	return mem_write_u8(at + TOD_WS_LEVEL, (u8)(level - 1));
}

int tod_unlock_set(u8 id, u8 field, u32 value)
{
	if (id >= TOD_WPN_FIRST && id < TOD_NWEAPONS) {
		u32 at = slot_addr(id);

		switch (field) {
		case 0: return mem_write_u8(at + TOD_WS_OWNED, value ? 1 : 0);
		case 1: return set_level(id, value);
		case 3:
			if (id >= TOD_WPN_GADGETS) return ST_BAD_ARG;
			return mem_write_u32(at + TOD_WS_AMMO, f32_bits((f32)value));
		default: return ST_BAD_ARG;
		}
	}
	if (field != 0) return ST_BAD_ARG;

	if (id >= TOD_UL_OBJ && id < TOD_UL_OBJ + TOD_NOBJ_LISTED)
		return mem_write_u32(TOD_OBJECTS + (u32)(id - TOD_UL_OBJ) * 4u, value ? 1u : 0u);
	return ST_NOT_FOUND;
}

/* ------------------------------------------------------- the progress actions */

static int gold_bolts_all(int on)
{
	u8 l;

	for (l = 0; l < TOD_NPLANETS; l++) {
		int rc;

		if (tod_gb_count[l] == 0) continue;
		rc = mem_write_u32(gb_addr(l), on ? (1u << tod_gb_count[l]) - 1u : 0u);
		if (rc != ST_OK) return rc;
	}
	return ST_OK;
}

static int skill_points_all(int on)
{
	u8 b[8];

	/* Bits 0..59, as the game's own table has them; the four above stay 0. */
	be64_put(b, on ? (((u64)1u << TOD_NSKILL_POINTS) - 1u) : 0u);
	return mem_write(TOD_SKILL_POINTS, b, 8);
}

/* --------------------------------------------- the weapon actions (build 59) */

/*
 * A weapon's config and its number of levels, through g_weaponDefTable as
 * Weapon_getDef reads it (tod.h, TOD_WEAPON_DEFS): `levels` is 0 for one whose
 * def says it cannot level. NOT_FOUND while the table has no def for it.
 */
static int weapon_config(u8 wpn, u32 *cfg, u32 *levels)
{
	u32 def = 0, flags = 0;
	int rc;

	*cfg = 0;
	*levels = 0;
	rc = mem_read_u32(TOD_WEAPON_DEFS + (u32)wpn * 4u, &def);
	if (rc != ST_OK) return rc;
	if (def == 0) return ST_NOT_FOUND;
	rc = mem_read_u32(def + TOD_WD_FLAGS, &flags);
	if (rc == ST_OK) rc = mem_read_u32(def + TOD_WD_CONFIG, cfg);
	if (rc != ST_OK) return rc;
	if (*cfg == 0) return ST_NOT_FOUND;
	if ((flags & 3u) != 0) return ST_OK;
	return mem_read_u32(*cfg + TOD_WC_NLEVELS, levels);
}

/*
 * The top level index a weapon of `levels` levels has, whatever the
 * playthrough (the user's call, build 59): WeaponSlot_getMaxLevel's cap for
 * an Omega slot, min(numLevels, 9), V10 for a weapon of ten levels. The game
 * itself stops a weapon at min(numLevels, 4), V5, until the vendor sells the
 * Omega levels in challenge mode; past V5 the slot is Omega
 * (WeaponSlot_isOmega), which is all the game keeps of that purchase.
 */
static u8 weapon_top(u32 levels)
{
	return (u8)(levels < TOD_LEVEL_TOP_OMEGA ? levels : TOD_LEVEL_TOP_OMEGA);
}

/*
 * The three walk the weapons the player owns and pass over the rest, as
 * RaC2's and Deadlocked's do: handing a weapon out is the Owned checkbox's
 * job. A weapon whose def the game has not loaded, or that cannot level, is
 * left as it is. BUSY when the player owns weapons and the game has no def
 * for any of them (no level up, say).
 */
static int weapon_levels_all(int max)
{
	u8 slots[TOD_WPN_DEVICES * TOD_WEAPON_STRIDE];
	u8 w, owned = 0, known = 0;
	int rc = mem_read(TOD_WEAPONS, slots, sizeof(slots));

	if (rc != ST_OK) return rc;

	for (w = TOD_WPN_FIRST; w < TOD_WPN_DEVICES; w++) {
		const u8 *slot = slots + (u32)w * TOD_WEAPON_STRIDE;
		u32 cfg, levels;
		u8 top;

		if (slot[TOD_WS_OWNED] == 0) continue;
		owned++;
		if (weapon_config(w, &cfg, &levels) != ST_OK) continue;
		known++;
		if (levels == 0) continue;

		if (max) {
			top = weapon_top(levels);
			/* Never down: a weapon already at its top is left as it is. */
			if (slot[TOD_WS_LEVEL] >= top) continue;
			rc = set_level(w, (u32)top + 1u);
		} else {
			if (slot[TOD_WS_LEVEL] == 0 && be32_get(slot + TOD_WS_XP) == 0) continue;
			rc = set_level(w, 1);
		}
		if (rc != ST_OK) return rc;
	}
	return owned != 0 && known == 0 ? ST_BUSY : ST_OK;
}

/*
 * WeaponDef_getMaxAmmo for the slot as it is: the config's max ammo at the
 * slot's level, plus each ammo mod the slot carries that adds, times one plus
 * each that multiplies (WeaponDef_applyMods), as f32 bits.
 */
static int weapon_max_ammo(u32 cfg, u8 level, u32 mods, u32 *bits)
{
	u8 table[TOD_WC_NMODS_MAX * TOD_WC_MOD_LEN];
	u32 base = 0, n = 0, i;
	f32 add, mult = 1.0f;
	int rc;

	if (level > TOD_LEVEL_TOP_OMEGA) level = TOD_LEVEL_TOP_OMEGA;
	rc = mem_read_u32(cfg + TOD_WC_MAX_AMMO + (u32)level * 4u, &base);
	if (rc == ST_OK) rc = mem_read_u32(cfg + TOD_WC_NMODS, &n);
	if (rc != ST_OK) return rc;
	add = f32_from_bits(base);
	if (n > TOD_WC_NMODS_MAX) n = TOD_WC_NMODS_MAX;

	if (n != 0 && mods != 0) {
		rc = mem_read(cfg + TOD_WC_MODS, table, n * TOD_WC_MOD_LEN);
		if (rc != ST_OK) return rc;
		for (i = 0; i < n; i++) {
			const u8 *m = table + i * TOD_WC_MOD_LEN;

			if ((mods & (1u << i)) == 0) continue;
			if (f32_from_bits(be32_get(m)) != TOD_MOD_STAT_AMMO) continue;
			if (f32_from_bits(be32_get(m + 4)) == 0.0f)
				add = add + f32_from_bits(be32_get(m + 8));
			else
				mult = mult + f32_from_bits(be32_get(m + 8));
		}
	}
	*bits = f32_bits(mult * add);
	return ST_OK;
}

static int weapon_ammo_all(void)
{
	u8 slots[TOD_WPN_GADGETS * TOD_WEAPON_STRIDE];
	u8 w, owned = 0, known = 0;
	int rc = mem_read(TOD_WEAPONS, slots, sizeof(slots));

	if (rc != ST_OK) return rc;

	/* The weapons and the devices: the gadgets have no ammo. */
	for (w = TOD_WPN_FIRST; w < TOD_WPN_GADGETS; w++) {
		const u8 *slot = slots + (u32)w * TOD_WEAPON_STRIDE;
		u32 cfg, levels, full;

		if (slot[TOD_WS_OWNED] == 0) continue;
		owned++;
		if (weapon_config(w, &cfg, &levels) != ST_OK) continue;
		if (weapon_max_ammo(cfg, slot[TOD_WS_LEVEL], be32_get(slot + TOD_WS_MODS), &full) != ST_OK) continue;
		known++;
		rc = mem_write_u32(slot_addr(w) + TOD_WS_AMMO, full);
		if (rc != ST_OK) return rc;
	}
	return owned != 0 && known == 0 ? ST_BUSY : ST_OK;
}

int tod_trigger(u8 id)
{
	switch (id) {
	case TF_DIE:         return tod_die();
	case TF_UNLOCK_SP:   return skill_points_all(1);
	case TF_RESET_SP:    return skill_points_all(0);
	case TF_UNLOCK_GB:   return gold_bolts_all(1);
	case TF_RESET_GB:    return gold_bolts_all(0);
	case TF_MAX_LEVELS:  return weapon_levels_all(1);
	case TF_RESET_LEVELS: return weapon_levels_all(0);
	case TF_MAX_AMMO:    return weapon_ammo_all();
	case TF_SET_ASIDE:   return tod_set_aside();
	case TF_LOAD_ASIDE:  return tod_load_aside();
	default:             return ST_NOT_FOUND;
	}
}

/* ------------------------------------------------ armour and skin, build 59 */

/*
 * The game's own names (core_text_tags.gb): its armour vendor's and SKIN
 * SELECT's; option 0 takes the armour or the skin off.
 */
static const char * const tod_armour_options[TOD_ARMOUR_COUNT] = {
	"None",
	"Blackstar Armour",
	"Helios Armour",
	"Terraflux Armour",
	"Trillium Armour"
};

static const char * const tod_skin_options[TOD_SKIN_COUNT] = {
	"None",
	"Dan Johnson",
	"Snowman",
	"Cragmite Skin",
	"Rusty Pete",
	"Cronk",
	"Zephyr",
	"Convict Ratchet",
	"Mustachio Furioso"
};

int tod_get_options(u8 id, const char * const **options, u8 *count)
{
	if (id == TF_ARMOUR) {
		*options = tod_armour_options;
		*count = (u8)TOD_ARMOUR_COUNT;
		return ST_OK;
	}
	if (id == TF_SKIN) {
		*options = tod_skin_options;
		*count = (u8)TOD_SKIN_COUNT;
		return ST_OK;
	}
	return ST_NOT_FOUND;
}

/*
 * Hero_setArmor's three writes (tod.h): the armour, its owned word, and the
 * hero's damage scale out of the game's own table. Between levels there is no
 * hero, and the next one's Hero_Init gives itself the scale from the save.
 */
static int tod_set_armour(u32 armour)
{
	u32 hero = 0, scale = 0;
	int rc;

	rc = mem_write_u32(TOD_ARMOUR, armour);
	if (rc == ST_OK) rc = mem_write_u32(TOD_ARMOUR_OWNED + armour * 4u, 1);
	if (rc != ST_OK) return rc;
	if (tod_hero(&hero) != ST_OK) return ST_OK;
	rc = mem_read_u32(TOD_ARMOUR_CONFIGS + armour * TOD_ARMOUR_CONFIG_LEN, &scale);
	if (rc != ST_OK) return rc;
	/* A table the game has not filled yet gives the hero nothing. */
	if (scale == 0) return ST_OK;
	return mem_write_u32(hero + TOD_HERO_DAMAGE_SCALE, scale);
}

int tod_set_value(u8 id, u32 value)
{
	switch (id) {
	case TF_ARMOUR:     return tod_set_armour(value);
	/* Skin_select's one write, without its owned check: the picker wears what it picks. */
	case TF_SKIN:       return mem_write_u32(TOD_SKIN, value);
	case TF_BOLTS:      return mem_write_u32(TOD_BOLTS, value);
	case TF_RARITANIUM: return mem_write_u32(TOD_RARITANIUM, value);
	case TF_CHALLENGE:  return mem_write_u32(TOD_CHALLENGE, value);
	default:            return ST_NOT_FOUND;
	}
}

/* ------------------------------------------------ Fast loads, revision 1.29 */

/*
 * qwark's own pieces' caves on a console (switches_own.c), the way Deadlocked's
 * HUD colours go in: written where they differ, before any site branches to
 * them, and never taken out, since nothing reaches them without the sites.
 */
static int tod_own_caves(void)
{
	u8 buf[64];
	u32 i, off, n;

	for (i = 0; i < SW_TOD_OWN_NCAVES; i++) {
		const struct sw_cave *c = &sw_tod_own_caves[i];

		for (off = 0; off < c->len; off += n) {
			n = c->len - off > sizeof(buf) ? (u32)sizeof(buf) : c->len - off;
			if (mem_read(c->addr + off, buf, n) == ST_OK && memcmp(buf, c->bytes + off, n) == 0) continue;
			if (mem_write(c->addr + off, c->bytes + off, n) != ST_OK) return ST_IO_ERROR;
		}
	}
	return ST_OK;
}

/*
 * Fast loads, either way (tod.h). Under RPCS3 with the switches in, the flag
 * byte, which the switch and qwark's own pieces both read. On a console, after
 * plat_can_patch_code(): on, the caves, the flag at 1 and the patch (the
 * twenty-five words and the three sites); off, the patch out and the flag at 0.
 * Then, both ways, g_levelTransitionMinTime: 0 while on, the game's 5.0 back
 * off. With neither (RPCS3 without the switches) it is UNSUPPORTED and writes
 * nothing, as before.
 */
static int tod_fast_loads(int on)
{
	int rc;

	if (switches_present()) {
		rc = mem_write_u8(SW_TOD_FLAG_FAST_LOADS, on ? 1u : 0u);
	} else if (!plat_can_patch_code()) {
		return ST_UNSUPPORTED;
	} else if (on) {
		rc = tod_own_caves();
		if (rc == ST_OK) rc = mem_write_u8(SW_TOD_FLAG_FAST_LOADS, 1);
		if (rc == ST_OK) rc = classic_patch_toggle(&TPA->fastload, 1);
	} else {
		rc = classic_patch_toggle(&TPA->fastload, 0);
		if (rc == ST_OK) rc = mem_write_u8(SW_TOD_FLAG_FAST_LOADS, 0);
	}
	if (rc != ST_OK) return rc;
	return mem_write_u32(TOD_TRANSITION_MIN, on ? 0u : TOD_TRANSITION_GAME);
}

/*
 * Revision 1.29: Fast loads forced for a load qwark starts (tod.h,
 * tod_galmap_up), as rac1_fastload_force: a flag at 1 under RPCS3 that this
 * process did not force is the user's setting; the toggle on, nothing is
 * forced. UNSUPPORTED (RPCS3 without the switches) loads at the game's pace.
 */
static int tod_fl_force(void)
{
	int on = 0, rc;

	TP->fl_loading = 0;
	TP->fl_ticks = 0;
	if (!TP->fl_forced && classic_code_read(SW_TOD_FLAG_FAST_LOADS, &on) == ST_OK && on) TP->fl_on = 1;
	if (TP->fl_on) return ST_OK;
	rc = tod_fast_loads(1);
	if (rc == ST_OK) TP->fl_forced = 1;
	return rc;
}

/* The forced load over (or never begun): the toggle's setting back. */
static void tod_fl_restore(void)
{
	TP->fl_forced = 0;
	TP->fl_loading = 0;
	if (!TP->fl_on) tod_fast_loads(0);
}

/*
 * Once a tick while forced: the load seen under way (the watcher's level
 * load, tod_autosplit.c), then over with the new level's lua_State, puts it
 * back; that is after LevelTransition_waitLoad has returned, the load screen
 * and qwark's own pieces done. No load begun in TOD_FADE_WAIT_TICKS (outside
 * the map's wait) puts it back too.
 */
static void tod_fl_tick(const struct tod_as_state *as)
{
	if (!TP->fl_forced) return;
	if (tod_level_loading(as->cur_job, as->cur_tier)) {
		TP->fl_loading = 1;
		return;
	}
	if (TP->fl_loading ? (as->cur_tier != 0 && as->cur_lua != 0) : ++TP->fl_ticks >= TOD_FADE_WAIT_TICKS)
		tod_fl_restore();
}

/*
 * Revision 1.30: Load set-aside, as RaC1's to UYA's (rac1_load_setaside): the force once its
 * RESTORE is under way, this load's from then on even when an earlier load's
 * was still running, as rac1_fastload_claim makes it. A force that cannot be
 * had (RPCS3 without the switches) leaves the load at the game's pace.
 */
int tod_load_aside_force(void)
{
	int rc = tod_fl_force();

	if (rc != ST_OK) plat_log("tod: Load set-aside goes at the game's pace: Fast loads could not be forced (%d)", rc);
	return rc;
}

/* ------------------------------------- qwark's own loads' fade, revision 1.29 */

/* LevelTransition_begin's test (tod.h, TOD_LOAD_FADE_STORY). */
int tod_story_load(u32 level, u32 flags_at)
{
	u8 b = 0;

	if (level != TOD_LEVEL_METROPOLIS && level != TOD_LEVEL_COBALIA) return 0;
	if (mem_read_u8(flags_at + TOD_LF_BYTE(TOD_LF_FAST_SHIP_FIXED), &b) != ST_OK) return 1;
	return (b & (1u << (TOD_LF_FAST_SHIP_FIXED & 7u))) == 0;
}

void tod_fade_arm(int story)
{
	TP->fade_armed = 1;
	TP->fade_seen = 0;
	TP->fade_ticks = 0;
	mem_write_u32(TOD_LOAD_FADE_TIME, story ? TOD_LOAD_FADE_STORY : TOD_LOAD_FADE_QUICK);
}

void tod_fade_disarm(void)
{
	TP->fade_armed = 0;
	mem_write_u32(TOD_LOAD_FADE_TIME, TOD_LOAD_FADE_GAME);
}

/*
 * Once a tick while armed: the game's time back once the load has left the
 * request (seen waiting, then -1: RequestLevelLoadState is in), once the level
 * is gone (Level_Exit, whichever tick saw the request), or after
 * TOD_FADE_WAIT_TICKS, a load the game never took. A read that fails is no
 * answer; the tick count still runs.
 */
static void tod_fade_tick(void)
{
	u32 request = 0, lua = 1;

	if (!TP->fade_armed) return;
	if (mem_read_u32(TOD_LOAD_REQUEST, &request) == ST_OK) {
		if (request != 0xFFFFFFFFu) TP->fade_seen = 1;
		else if (TP->fade_seen) { tod_fade_disarm(); return; }
	}
	if (mem_read_u32(TOD_LUA_STATE, &lua) == ST_OK && lua == 0) { tod_fade_disarm(); return; }
	if (++TP->fade_ticks >= TOD_FADE_WAIT_TICKS) tod_fade_disarm();
}

/* -------------------------- a pick on qwark's galactic map, revision 1.29 */

/*
 * The core's menu_picker `up` (menu.c): the map qwark opened is up, so a pick
 * on it is qwark's load. Both are armed now, before any pick: the map's exit
 * (GalMapState_exit, LoadLevel) and LoadLevel_ProcessPending's first fade
 * frame may come before qwark's next look, and ProcessPending reads
 * k_loadLevelFadeTime every frame (ScreenFade_Start, the rate 1/time), so a
 * later write still holds from the next frame on. A story load may only be
 * Metropolis or Cobalia with flag 106 clear: then the longer fade, which
 * never flashes; else the frame.
 */
void tod_galmap_up(void)
{
	TP->map_armed = 1;
	TP->map_level = -1;
	tod_fl_force();
	tod_fade_arm(tod_story_load(TOD_LEVEL_METROPOLIS, TOD_LEVEL_FLAGS));
}

/*
 * Once a tick while armed. g_gameState first, then the pick: GalMap_selectLevel
 * (or the glue's lm_pick) writes the pick before the map's exit, so a state
 * past 6 read first means the pick read after is final. 1 while the map
 * holds the load's watches (up: nothing times out), 0 once it has handed a
 * pick to them; a pick arms its own fade; the map gone without one puts both
 * back at once.
 */
static int tod_map_tick(void)
{
	u32 state = 0, pick = 0xFFFFFFFFu;

	if (!TP->map_armed) return 0;
	if (mem_read_u32(TOD_GAME_STATE, &state) != ST_OK || mem_read_u32(TOD_GALMAP_PICK, &pick) != ST_OK) return 1;
	if (pick < TOD_NPLANETS && (s32)pick != TP->map_level) {
		TP->map_level = (s16)pick;
		tod_fade_arm(tod_story_load(pick, TOD_LEVEL_FLAGS));
	}
	if (state == TOD_STATE_GALMAP) return 1;
	TP->map_armed = 0;
	if (TP->map_level >= 0) return 0;
	tod_fade_disarm();
	if (TP->fl_forced) tod_fl_restore();
	return 1;
}

/* ------------------------------------------ Remove cutscenes, revision 1.29 */

/* The user's eight, exactly: the runners delete these files. */
static const char * const tod_cut_names[] = {
	"l00_s00_attr_fmv",
	"l00_s00_attr_fmv_textless",
	"l01_s01_ntro_fmv",
	"l01_s02_walk_fmv",
	"l01_s03_tach_fmv",
	"l02_s01_hypr_fmv",
	"l7b_s01_end2_fmv",
	"l17_s03a_rmid_fmv"
};

#define TOD_NCUT ((u32)(sizeof(tod_cut_names) / sizeof(tod_cut_names[0])))

static char cut_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Whether `stem` (up to a '.' or the end, `n` bytes at most) is `name` from its letter `from`. */
static int cut_stem_is(const char *stem, u32 n, const char *name, u32 from)
{
	u32 i = 0;

	name += from;
	while (i < n && stem[i] != 0 && stem[i] != '.') {
		if (name[i] == 0 || cut_lower(stem[i]) != name[i]) return 0;
		i++;
	}
	return name[i] == 0;
}

/*
 * One file's path, `TOD_FMV_PATH_LEN` bytes of it read at `at`. As the game
 * has it: the name after the last '/' one of the eight, and `remove` writes 0
 * over its first letter. As this left it: a 0 at the start or after a '/'
 * before any other, and one of the eight's from its second letter after it;
 * `!remove` writes the letter back. 1 for a write, 0 for none; `*blank` the
 * byte that is 0 when the path is one of the eight's left missing, or 0.
 */
static int cut_path(u32 at, int remove, u32 *blank)
{
	char p[TOD_FMV_PATH_LEN];
	u32 i, end, base = 0, k;

	*blank = 0;
	if (at == 0) return 0;
	if (mem_read(at, p, sizeof(p)) != ST_OK) return 0;
	for (end = 0; end < sizeof(p) && p[end] != 0; end++)
		if (p[end] == '/') base = end + 1;
	if (end >= sizeof(p) - 1) return 0;

	if (base == end) {
		/* Ours: the 0 stands for the letter. */
		for (k = 0; k < TOD_NCUT; k++) {
			if (!cut_stem_is(p + end + 1, (u32)sizeof(p) - end - 1, tod_cut_names[k], 1)) continue;
			if (remove) {
				*blank = at + end;
				return 0;
			}
			return mem_write_u8(at + end, (u8)tod_cut_names[k][0]) == ST_OK;
		}
		return 0;
	}
	if (!remove) return 0;
	for (i = 0; i < TOD_NCUT; i++)
		if (cut_stem_is(p + base, end - base, tod_cut_names[i], 0)) {
			if (mem_write_u8(at + base, 0) != ST_OK) return 0;
			*blank = at + base;
			return 1;
		}
	return 0;
}

static void cut_keep(u32 blank)
{
	if (blank != 0 && TP->cut_n < TOD_CUT_KEEP) TP->cut_at[TP->cut_n++] = blank;
}

int tod_cutscenes_apply(int remove)
{
	u8 rec[TOD_FMV_LOOK_LEN];
	u32 toc = 0, table = 0, count = 0, i, blank;
	int changed = 0;

	TP->cut_toc = 0;
	TP->cut_table = 0;
	TP->cut_n = 0;
	if (mem_read_u32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, &toc) != ST_OK || toc == 0 || (toc & 3u)) return -1;
	if (mem_read_u32(toc + TOD_TOC_FMVS, &table) != ST_OK || table == 0 || (table & 3u)) return -1;
	if (mem_read_u32(table - 4u, &count) != ST_OK || count == 0 || count > TOD_FMV_MAX) return -1;

	for (i = 0; i < count; i++) {
		if (mem_read(table + i * TOD_FMV_STRIDE, rec, sizeof(rec)) != ST_OK) return -1;
		changed += cut_path(be32_get(rec + TOD_FMV_FILE), remove, &blank);
		cut_keep(blank);
		changed += cut_path(be32_get(rec + TOD_FMV_ALT), remove, &blank);
		cut_keep(blank);
	}
	if (remove) {
		TP->cut_toc = toc;
		TP->cut_table = table;
	}
	return changed;
}

/*
 * Whether the table is the one last seen and every byte put at 0 in it still
 * is: then a look reads no more than this, which under RPCS3 is a few PINE
 * round trips rather than the whole table's.
 */
static int cut_still(void)
{
	u32 toc = 0, table = 0;
	u8 b = 1, i;

	if (TP->cut_toc == 0 || TP->cut_n == 0) return 0;
	if (mem_read_u32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, &toc) != ST_OK || toc != TP->cut_toc) return 0;
	if (mem_read_u32(toc + TOD_TOC_FMVS, &table) != ST_OK || table != TP->cut_table) return 0;
	for (i = 0; i < TP->cut_n; i++)
		if (mem_read_u8(TP->cut_at[i], &b) != ST_OK || b != 0) return 0;
	return 1;
}

static void tod_cut_tick(void)
{
	if (!TP->cut_wanted) return;
	if (TP->cut_wait != 0) {
		TP->cut_wait--;
		return;
	}
	TP->cut_wait = TOD_CUT_EVERY;
	if (cut_still()) return;
	if (tod_cutscenes_apply(1) > 0) plat_log("tod: Remove cutscenes: the FMVs' files are missing again");
}

/* ------------------------------------------- Remove black screens, 2026-11-12 */

/*
 * The fade's drawn alpha at 0 while it is still (tod.h, TOD_FADE_ALPHA), or
 * with `restore`, 1.0 again where it is still at black; the progress and the
 * target are never written.
 */
static void black_alpha(int restore)
{
	u8 f[TOD_FADE_DRAW_LEN];
	u32 root = 0, alpha, progress;

	if (mem_read_u32(TOD_LEVEL_OBJ, &root) != ST_OK || root == 0 || (root & 3u) != 0) return;
	if (mem_read(root + TOD_FADE_ALPHA, f, sizeof(f)) != ST_OK) return;
	alpha = be32_get(f);
	progress = be32_get(f + (TOD_FADE_PROGRESS - TOD_FADE_ALPHA));
	/* A fade under way: ScreenFade_update draws it again every frame. */
	if (progress != be32_get(f + (TOD_FADE_TARGET - TOD_FADE_ALPHA))) return;
	if (restore) {
		if (progress == TOD_FADE_BLACK && alpha != TOD_FADE_BLACK) mem_write_u32(root + TOD_FADE_ALPHA, TOD_FADE_BLACK);
	} else if ((alpha & 0x7FFFFFFFu) != 0) {
		mem_write_u32(root + TOD_FADE_ALPHA, 0);
	}
}

/*
 * Each tick while the toggle is on, in a level: the movie player's state.
 * A state 1 hold (black before a realtime cinematic) that begins within
 * TOD_LEADIN_TICKS of the lua_State changing is a lead-in, and its black is
 * kept off the screen until the hold ends. A read that fails changes nothing.
 */
static void tod_black_tick(const struct tod_as_state *as)
{
	u32 state = 0;

	if (!TP->bs_on) return;
	if (as->cur_lua != TP->bs_lua) {
		TP->bs_lua = as->cur_lua;
		TP->bs_age = 0;
	} else if (TP->bs_age != 0xFFFFu) {
		TP->bs_age++;
	}
	if (as->cur_lua == 0) {
		TP->bs_leadin = 0;
		TP->bs_state = 0;
		return;
	}
	if (mem_read_u32(TOD_MOVIE_PLAYER + TOD_MP_STATE, &state) != ST_OK) return;
	if (state != TOD_MP_PREROLL) TP->bs_leadin = 0;
	else if (TP->bs_state != TOD_MP_PREROLL) TP->bs_leadin = TP->bs_age <= TOD_LEADIN_TICKS;
	TP->bs_state = (u8)state;
	if (TP->bs_leadin) black_alpha(0);
}

/*
 * On: the level counts as just up, so a lead-in already holding is cleared at
 * once. Off: a hold this cleared is black again.
 */
static int tod_remove_black(int on)
{
	if (!on && TP->bs_on && TP->bs_leadin) black_alpha(1);
	TP->bs_on = on ? 1u : 0u;
	TP->bs_leadin = 0;
	TP->bs_state = 0;
	TP->bs_age = 0;
	TP->bs_lua = 0;
	return ST_OK;
}

void tod_panel_enter(void)
{
	tod_fade_disarm();
	tod_cutscenes_apply(0);
}

void tod_panel_tick(const struct tod_as_state *as)
{
	/* The map's wait holds the fade's and the force's own watches: no load begins under it. */
	if (!tod_map_tick()) {
		tod_fade_tick();
		tod_fl_tick(as);
	}
	tod_cut_tick();
	tod_black_tick(as);
	tod_die_tick();
}

/* --------------------------------------------------------------- the cheats */

/*
 * Build 59, Infinite ammo (tod_set_toggle says what it is). The fill is Max
 * all weapon ammo's; one that finds owned weapons but no weapon table yet (on
 * the title, where an "on boot" flag applies it) fills nothing and the patch
 * still goes in, so the save's counts stay as they load. Under RPCS3 the
 * patch is the switch's flag (classic_code_toggle); without the switches the
 * core refuses the toggle before it gets here (WRITES_CODE).
 */
static int tod_infinite_ammo(int on)
{
	int rc;

	if (on) {
		rc = weapon_ammo_all();
		if (rc != ST_OK && rc != ST_BUSY) return rc;
	}
	return classic_code_toggle(&TPA->ammo, SW_TOD_FLAG_INFINITE_AMMO, on);
}

/*
 * Infinite health (id 9, "God Ratchet" until 2026-10-27): the game's own
 * "hero takes no damage" word, g_debugHeroNoDamage, which Hero_Update checks
 * before it applies any damage event, on foot and in the ship; nothing in the
 * game writes it.
 *
 * God Ratchet (id 16): the old RaCMAN's, byte for byte, because speedrunners
 * use the glitch it is. On writes 154 into the low byte of
 * g_heroProxyMobyHandle (TOD_PROXY_LOW), off writes 0 there; the other three
 * bytes are left alone, as the old trainer left them. While the handle is
 * non-zero Hero_Update drops the hero's damage and the enemies stop targeting
 * him (Hero_registerAsTarget); a real proxy moby's spawn or death overwrites
 * the fake handle. It reads on while that byte is non-zero, which is what the
 * old trainer's own test amounted to. Neither cheat stops a death plane or a
 * fall.
 *
 * Infinite ammo (build 59, as every game's): on fills each owned weapon and
 * device to what it holds at its level with its mods (weapon_ammo_all, Max all
 * weapon ammo), then nops the shot's decrement in Weapon_useAmmo (tod.h,
 * TOD_AMMO_INSTR) on a console, or sets its switch's flag under RPCS3
 * (tod_infinite_ammo below); off puts the instruction back and leaves the
 * ammo where it is. Every weapon and every device takes its ammo there, the
 * Visi-Copter among them, so the devices stay full as the weapons do. Until
 * build 59 it set g_infiniteAmmo, which lets the weapon in hand fire on an
 * empty count while the game still counted down, and a tick held each
 * weapon's count and kept the devices full; neither is written any more.
 *
 * 2026-10-28: Infinite ammo and Infinite health are no longer LIVE (tod_game.c),
 * so they come back on boot when flagged, and their state is qwark's bit;
 * God Ratchet alone is read back here.
 *
 * Fast loads (id 17), WRITES_CODE since 2026-10-28: the load screen's cut
 * (tod.h), the patch above on a console, after plat_can_patch_code() as every
 * code toggle (patch_apply asks), and under RPCS3 the code switch's flag byte
 * (tod_fast_loads below). A new boot of the game has neither, and the toggle's
 * auto flag or the previous-session prompt puts it back, as the other games'.
 *
 * Remove cutscenes (id 18, revision 1.29): data writes alone (tod.h,
 * TOD_ARCHIVE_GAME), not LIVE either, so it can come back at boot; the tick
 * keeps it while it is wanted, since the table may come after the toggle.
 *
 * Remove black screens (id 24, 2026-11-12): a data write of the fade's drawn
 * alpha during a lead-in's hold (tod.h, TOD_FADE_ALPHA, tod_black_tick), so
 * RPCS3 as a console; qwark's own bit, not LIVE, like the two above.
 */
int tod_set_toggle(u8 id, int on)
{
	int rc;

	switch (id) {
	case TF_INFINITE_HEALTH: return mem_write_u32(TOD_NO_DAMAGE, on ? 1u : 0u);
	case TF_GOD_RATCHET:     return mem_write_u8(TOD_PROXY_LOW, on ? (u8)TOD_PROXY_FAKE : 0u);
	case TF_FAST_LOADS:
		rc = tod_fast_loads(on);
		if (rc == ST_OK) {
			/* Whatever a load forced, the toggle has the say now. */
			TP->fl_on = on ? 1u : 0u;
			TP->fl_forced = 0;
			TP->fl_loading = 0;
		}
		return rc;
	case TF_REMOVE_CUTSCENES:
		TP->cut_wanted = on ? 1u : 0u;
		TP->cut_wait = TOD_CUT_EVERY;
		tod_cutscenes_apply(on);
		return ST_OK;
	case TF_REMOVE_BLACK:
		return tod_remove_black(on);
	case TF_INFINITE_AMMO:
		return tod_infinite_ammo(on);
	default:
		return ST_NOT_FOUND;
	}
}

int tod_toggle_read(u8 id, int *on)
{
	u32 v = 0;
	u8 b = 0;
	int rc;

	switch (id) {
	case TF_GOD_RATCHET:
		rc = mem_read_u8(TOD_PROXY_LOW, &b);
		v = b;
		break;
	/* The switch's flag byte under RPCS3; NOT_FOUND without it, where qwark's bit says. */
	case TF_FAST_LOADS:
		rc = classic_code_read(SW_TOD_FLAG_FAST_LOADS, on);
		/* Revision 1.29: a load's force is not the toggle (tod_fl_force). */
		if (rc == ST_OK && TP->fl_forced) *on = TP->fl_on;
		return rc;
	/* Build 59: and Infinite ammo's, the same way. */
	case TF_INFINITE_AMMO:
		return classic_code_read(SW_TOD_FLAG_INFINITE_AMMO, on);
	default:
		return ST_NOT_FOUND;
	}
	if (rc != ST_OK) return rc;
	*on = v != 0;
	return ST_OK;
}

/*
 * The devices' MaxAmmo, WPN_ 16..23, from the game's weapon.csv (Zurkon,
 * ConfusionGas, Slinkonator, Groovitron, MiniLeech, MaxiLeech, Morph,
 * Copter), as f32 bits. The devices take no mods, so the slot's level is all
 * WeaponDef_getMaxAmmo would add, and they have one. Until build 59 Infinite
 * ammo's hold kept every owned device at this count; the game's own table
 * (weapon_ammo_all) is what fills them now, and the simulator's seed of that
 * table (backend_fake.c) is all this is for, so a game module carries none
 * of it.
 */
#if !QWARK_GAME_MODULE
static const u32 tod_device_max[TOD_WPN_GADGETS - TOD_WPN_DEVICES] = {
	0x40800000u,   /* 16 Mr. Zurkon     4 */
	0x40000000u,   /* 17 Confuzzler     2 */
	0x40400000u,   /* 18 Deathspring    3 */
	0x40400000u,   /* 19 Groovitron     3 */
	0x40000000u,   /* 20 Leech Mine     2 */
	0x3F800000u,   /* 21 Mega Leech     1 */
	0x40400000u,   /* 22 Transmorpher   3 */
	0x40400000u    /* 23 Visi-Copter    3 */
};

u32 tod_device_max_ammo(u8 wpn)
{
	if (wpn < TOD_WPN_DEVICES || wpn >= TOD_WPN_GADGETS) return 0;
	return tod_device_max[wpn - TOD_WPN_DEVICES];
}
#endif

/* ---------------------------------------------------- positions, die, planets */

/* A float that is neither an infinity nor a NaN. */
static int f32_ok(u32 bits)
{
	return (bits & 0x7F800000u) != 0x7F800000u;
}

/* A square root for the camera's lengths (no libm in a module): Newton from above. */
static f32 cam_sqrt(f32 sq)
{
	f32 r = sq > 1.0f ? sq : 1.0f;
	int i;

	if (!(sq > 0.0f)) return 0.0f;
	for (i = 0; i < 24; i++) r = 0.5f * (r + sq / r);
	return r;
}

/*
 * One camera slot, if it is one a hero plays with (tod.h, TOD_CAM_*): the
 * follow camera (3) or the lock-strafe camera (26), whose target is within
 * 32 units of `hero_xyz` (so a camera following something else is never
 * taken), and, for the lock-strafe camera, whose look direction is one (about
 * unit long). Its type and its state (+0xC0, four vectors) on success.
 */
static int cam_take(u32 slot, const u8 *hero_xyz, u32 *type, u8 state[TOD_CAM_STATE_LEN])
{
	f32 d, sq = 0.0f;
	u32 i;

	if (mem_read_u32(slot + TOD_CAM_TYPE, type) != ST_OK) return ST_IO_ERROR;
	if (*type != TOD_CAM_FOLLOW && *type != TOD_CAM_STRAFE) return ST_NOT_FOUND;
	if (mem_read(slot + TOD_CAM_STATE, state, TOD_CAM_STATE_LEN) != ST_OK) return ST_IO_ERROR;
	for (i = 0; i < 3; i++) {
		if (!f32_ok(be32_get(state + i * 4)) || !f32_ok(be32_get(hero_xyz + i * 4))) return ST_NOT_FOUND;
		d = bef32_get(state + i * 4) - bef32_get(hero_xyz + i * 4);
		sq += d * d;
	}
	if (!(sq <= TOD_CAM_NEAR_SQ)) return ST_NOT_FOUND;
	if (*type == TOD_CAM_STRAFE) {
		sq = 0.0f;
		for (i = 0; i < 3; i++) {
			if (!f32_ok(be32_get(state + TOD_CAM_OFFSET + i * 4))) return ST_NOT_FOUND;
			d = bef32_get(state + TOD_CAM_OFFSET + i * 4);
			sq += d * d;
		}
		if (!(sq >= 0.5f && sq <= 2.0f)) return ST_NOT_FOUND;
	}
	return ST_OK;
}

/*
 * 2026-11-14: the camera that follows the hero. First the slot each camera
 * client is using (ToDCamClient.active), whichever of the four clients is the
 * hero's; then, should none be (a camera cut's blend in its place for a few
 * frames), every client's slots, as CamClient_findFollowCam looks, for either
 * type. Until 2026-11-14 only client 0's first type-3 slot was looked at, so
 * with Lock-Strafe on, where the hero's camera is the lock-strafe camera and
 * there is no type-3 slot, no slot ever carried the camera.
 */
static int tod_find_cam(const u8 *hero_xyz, u32 *cam, u32 *type, u8 state[TOD_CAM_STATE_LEN])
{
	u32 c, i, client, active = 0;

	for (c = 0; c < TOD_CAM_NCLIENTS; c++) {
		client = TOD_CAM_CLIENT + c * TOD_CAM_CLIENT_LEN;
		if (mem_read_u32(client + TOD_CAM_ACTIVE, &active) != ST_OK) return ST_IO_ERROR;
		if (active < client + TOD_CAM_SLOTS || active >= client + TOD_CAM_SLOTS + TOD_CAM_NSLOTS * TOD_CAM_SLOT_LEN ||
		    (active - client - TOD_CAM_SLOTS) % TOD_CAM_SLOT_LEN != 0)
			continue;
		if (cam_take(active, hero_xyz, type, state) == ST_OK) {
			*cam = active;
			return ST_OK;
		}
	}
	for (c = 0; c < TOD_CAM_NCLIENTS; c++) {
		client = TOD_CAM_CLIENT + c * TOD_CAM_CLIENT_LEN;
		for (i = 0; i < TOD_CAM_NSLOTS; i++) {
			u32 slot = client + TOD_CAM_SLOTS + i * TOD_CAM_SLOT_LEN;

			if (cam_take(slot, hero_xyz, type, state) == ST_OK) {
				*cam = slot;
				return ST_OK;
			}
		}
	}
	return ST_NOT_FOUND;
}

/* An offset a camera can have: finite, and neither on top of its target nor far off. */
static int tod_cam_offset_ok(const u8 *off)
{
	f32 sq = 0.0f, v;
	u32 i;

	for (i = 0; i < 4; i++) {
		if (!f32_ok(be32_get(off + i * 4))) return 0;
	}
	for (i = 0; i < 3; i++) {
		v = bef32_get(off + i * 4);
		sq += v * v;
	}
	return sq >= TOD_CAM_MIN_SQ && sq <= TOD_CAM_MAX_SQ;
}

/*
 * A slot: the hero's 48 bytes, then the camera's offset from its target when
 * a camera follows the hero (64 bytes): the follow camera's +0xD0 as it is,
 * the lock-strafe camera's look direction turned back and 5.5 long, where it
 * puts itself (w 0); otherwise the 48 alone, which load as the hero alone.
 */
int tod_save_blob(u8 *blob, u8 *len)
{
	u8 state[TOD_CAM_STATE_LEN], off[16];
	u32 hero, cam = 0, type = 0, i;
	int rc = tod_hero(&hero);

	if (rc != ST_OK) return rc;
	rc = mem_read(hero + TOD_HERO_POS, blob, TOD_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = TOD_POS_BLOB_LEN;
	if (tod_find_cam(blob, &cam, &type, state) != ST_OK) return ST_OK;
	if (type == TOD_CAM_STRAFE) {
		for (i = 0; i < 3; i++)
			bef32_put(off + i * 4, -bef32_get(state + TOD_CAM_OFFSET + i * 4) * TOD_CAM_STRAFE_DIST);
		be32_put(off + 12, 0);
	} else {
		memcpy(off, state + TOD_CAM_OFFSET, 16);
	}
	if (tod_cam_offset_ok(off)) {
		memcpy(blob + TOD_POS_CAM_AT, off, 16);
		*len = TOD_POS_SLOT_LEN;
	}
	return ST_OK;
}

/*
 * The camera of a 64-byte slot (tod.h, TOD_CAM_*), into whichever camera the
 * hero has now. Either way the target moves by the hero's own jump (`from`
 * the position before the load, `to` the slot's) and the camera's position
 * is the target plus the slot's offset, so nothing lags or swings in from the
 * old place; each vector's w is the camera's own.
 *
 *   follow camera   the offset as both the offset and the desired one, so
 *                   the next frame keeps it, and the desired position.
 *   lock-strafe     the look direction, the offset turned back and made unit
 *                   long; its up vector (+0xE0) is left to StrafeCam_alignUp.
 */
static int tod_load_cam(u32 cam, u32 type, u8 state[TOD_CAM_STATE_LEN], const u8 *from, const u8 *to, const u8 *off)
{
	u8 pos[16];
	f32 len = 0.0f;
	u32 i;

	for (i = 0; i < 3; i++) {
		f32 o = bef32_get(off + i * 4);

		bef32_put(state + i * 4, bef32_get(state + i * 4) + (bef32_get(to + i * 4) - bef32_get(from + i * 4)));
		len += o * o;
	}
	len = cam_sqrt(len);
	for (i = 0; i < 4; i++) {
		f32 o = i < 3 ? bef32_get(off + i * 4) : bef32_get(state + TOD_CAM_OFFSET + 12);

		bef32_put(pos + i * 4, bef32_get(state + i * 4) + o);
		if (type == TOD_CAM_STRAFE) {
			if (i < 3) bef32_put(state + TOD_CAM_OFFSET + i * 4, -o / len);
			continue;
		}
		bef32_put(state + TOD_CAM_OFFSET + i * 4, o);
		bef32_put(state + TOD_CAM_DESIRED + i * 4, o);
		memcpy(state + TOD_CAM_DESIRED_POS + i * 4, pos + i * 4, 4);
	}
	if (mem_write(cam + TOD_CAM_STATE, state, type == TOD_CAM_STRAFE ? TOD_CAM_DESIRED : TOD_CAM_STATE_LEN) != ST_OK)
		return ST_IO_ERROR;
	return mem_write(cam + TOD_CAM_POS, pos, sizeof(pos));
}

/*
 * The blob back, and its position also as the hero's previous one and the
 * moby's: the motion update copies pos to the moby every frame anyway, but
 * without prevPos the first frame derives a velocity from the jump. A
 * 64-byte slot puts the camera back too (tod_load_cam), found with the hero
 * where he was; a 48-byte slot, or one whose camera is not usable, leaves
 * the camera alone.
 */
int tod_load_blob(const u8 *blob, u8 len)
{
	u8 state[TOD_CAM_STATE_LEN], from[12];
	u32 hero, moby = 0, cam = 0, type = 0;
	int rc, with_cam = 0;

	if (len != TOD_POS_BLOB_LEN && len != TOD_POS_SLOT_LEN) return ST_BAD_ARG;
	rc = tod_hero(&hero);
	if (rc != ST_OK) return rc;
	if (len == TOD_POS_SLOT_LEN && tod_cam_offset_ok(blob + TOD_POS_CAM_AT) &&
	    mem_read(hero + TOD_HERO_POS, from, sizeof(from)) == ST_OK &&
	    tod_find_cam(from, &cam, &type, state) == ST_OK)
		with_cam = 1;
	rc = mem_write(hero + TOD_HERO_POS, blob, TOD_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	rc = mem_write(hero + TOD_HERO_PREV_POS, blob, 16);
	if (rc != ST_OK) return rc;
	if (mem_read_u32(hero + TOD_HERO_MOBY, &moby) == ST_OK && moby != 0 && (moby & 3u) == 0)
		mem_write(moby + TOD_MOBY_POS, blob, 16);
	if (with_cam) return tod_load_cam(cam, type, state, from, blob, blob + TOD_POS_CAM_AT);
	return ST_OK;
}

/*
 * Die, since 2026-11-13 (tod.h, TOD_RESPAWN_FADE): the respawn the game makes
 * once a death's fade is black, with no death animation and no fade. The
 * fade as ScreenFade_Set(1.0) leaves it, then g_heroRespawnFadeState 1, in
 * that order: the game's next frame calls GameState_requestLevelRespawn
 * itself. Only in play, with no state change, planet load or movie under
 * way (Hero_beginRespawnFade's own test is the movie). The hit points are
 * not touched: the respawn's new hero has them full.
 */
static int tod_respawn(void)
{
	u8 fade[12];
	u32 state = 0, root = 0, request = 0, requested = 0, movie = 0;
	int rc;

	rc = mem_read_u32(TOD_GAME_STATE, &state);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LEVEL_OBJ, &root);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LOAD_REQUEST, &request);
	if (rc == ST_OK) rc = mem_read_u32(TOD_REQUESTED_STATE, &requested);
	if (rc == ST_OK) rc = mem_read_u32(TOD_MOVIE_PLAYER + TOD_MP_STATE, &movie);
	if (rc != ST_OK) return rc;
	if (state != TOD_STATE_GAMEPLAY || root == 0 || (root & 3u) != 0 || request != 0xFFFFFFFFu ||
	    requested != TOD_REQUESTED_NONE || (movie >= 1u && movie <= 4u))
		return ST_BUSY;
	be32_put(fade, TOD_FADE_BLACK);      /* progress */
	be32_put(fade + 4, 0);               /* rate */
	be32_put(fade + 8, TOD_FADE_BLACK);  /* target */
	rc = mem_write(root + TOD_FADE_PROGRESS, fade, sizeof(fade));
	if (rc == ST_OK) rc = mem_write_u32(root + TOD_FADE_ALPHA, TOD_FADE_BLACK);
	if (rc == ST_OK) rc = mem_write_u32(TOD_RESPAWN_FADE, 1);
	return rc;
}

/*
 * From the game's own pause menu (the Trainer's Game page), where no
 * gameplay frame runs: kept for the pause's end, and made on the first tick
 * of play (tod_die_tick).
 */
int tod_die(void)
{
	u32 hero, state = 0;
	int rc = tod_hero(&hero);

	if (rc != ST_OK) return rc;
	rc = mem_read_u32(TOD_GAME_STATE, &state);
	if (rc != ST_OK) return rc;
	if (state == TOD_STATE_PAUSE) {
		TP->die_pending = 1;
		TP->die_playing = 0;
		TP->die_ticks = 0;
		TP->die_hero = hero;
		return ST_OK;
	}
	TP->die_pending = 0;
	return tod_respawn();
}

/*
 * A Die kept from the pause: made once play is back (the same hero, g_gameState
 * 4), retried while the respawn is BUSY for a moment after, and dropped
 * when the hero goes or the game goes anywhere but the pause and play.
 */
static void tod_die_tick(void)
{
	u32 hero = 0, state = 0;

	if (!TP->die_pending) return;
	if (tod_hero(&hero) != ST_OK || hero != TP->die_hero || mem_read_u32(TOD_GAME_STATE, &state) != ST_OK) {
		TP->die_pending = 0;
		return;
	}
	if (state == TOD_STATE_PAUSE && !TP->die_playing) return;
	if (state != TOD_STATE_GAMEPLAY) {
		TP->die_pending = 0;
		return;
	}
	TP->die_playing = 1;
	if (tod_respawn() != ST_BUSY || ++TP->die_ticks >= TOD_DIE_WAIT_TICKS) TP->die_pending = 0;
}

/*
 * A planet: the level id into g_planetLoadRequest, which LoadLevel_ProcessPending
 * polls from Game_UpdateFrame: a fade, then the load, then -1 back. Exactly what
 * the game's own set_level does in gameplay, so only in gameplay (g_gameState 4,
 * a level object up), and never over a load already asked for: a request is
 * never taken back once written, as that would leave a fade with no load.
 * Revision 1.29: the fade a frame long, as for every load qwark asks for, but
 * a few frames for a story FMV load, which has no load screen (tod.h,
 * TOD_LOAD_FADE_STORY): the level flags are the game's own here.
 * Revision 1.29: and Fast loads forced for it, as the four original
 * games' planet load forces them (tod.h, tod_galmap_up); a request that fails
 * takes back its own force and nobody else's.
 * 2026-11-14: in the pause and in a realtime cinematic, where no gameplay
 * frame runs to take a request, the level loads at once, as LoadLevel itself
 * does there and the pause menu's own Load does (tod_level_load_now): no
 * fade, the level's unload taking the pause or the cinematic down. Until then
 * both were BUSY ("only in gameplay"), a guard that kept a request from being
 * written where nothing would take it, not one a crash needed. The galactic
 * map, an FMV and a pause still being built stay BUSY (tod.h).
 */
/*
 * Build 59 (D3): the planet flags, on the destination, as the four games'
 * loads act on them. bit0 clears the destination's level flags (its group of
 * the one table, tod_levelflags_reset, as LEVELFLAGS_RESET does), bit1 its
 * gold bolts (the level's mask in its level save, LevelSave_collectGoldBolt's,
 * as Reset all gold bolts writes it for every level). Both before the request,
 * once every check has passed and Fast loads are forced: unlike the four,
 * where they follow it, the load itself reads the destination's flags first
 * (tod_story_load picks the fade from them, and the story FMV it plays depends
 * on them), so a flag cleared after the request could leave a one-frame fade
 * in front of an FMV that now plays. Only a failed write of the request's own
 * word, the game gone, leaves them cleared with no load.
 */
static int tod_planet_resets(u8 planet, u8 flags)
{
	int rc;

	if (flags & PLANET_FLAG_RESET_LEVELFLAGS) {
		rc = tod_levelflags_reset(planet);
		if (rc != ST_OK) return rc;
	}
	if ((flags & PLANET_FLAG_RESET_BOLTS) && tod_gb_count[planet] != 0) {
		rc = mem_write_u32(gb_addr(planet), 0u);
		if (rc != ST_OK) return rc;
	}
	return ST_OK;
}

int tod_planet_load(u8 planet, u8 flags)
{
	u32 state = 0, request = 0, root = 0, pause = 0;
	int rc, was = TP->fl_forced, now;

	if (planet >= TOD_NPLANETS) return ST_BAD_ARG;
	rc = mem_read_u32(TOD_GAME_STATE, &state);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LOAD_REQUEST, &request);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LEVEL_OBJ, &root);
	if (rc == ST_OK && state == TOD_STATE_PAUSE) rc = mem_read_u32(TOD_PAUSE_STATE, &pause);
	if (rc != ST_OK) return rc;
	/* 2026-11-14: from the pause or a cinematic, at once, as the game's own load there (tod.h). */
	now = tod_load_leaves_now(state, pause);
	if ((state != TOD_STATE_GAMEPLAY && !now) || root == 0 || request != 0xFFFFFFFFu) return ST_BUSY;
	rc = tod_fl_force();
	if (rc != ST_OK && rc != ST_UNSUPPORTED) return rc;
	rc = tod_planet_resets(planet, flags);
	if (rc != ST_OK) {
		if (!was && TP->fl_forced) tod_fl_restore();
		return rc;
	}
	if (now) {
		rc = tod_level_load_now(planet);
		if (rc != ST_OK && !was && TP->fl_forced) tod_fl_restore();
		if (rc == ST_OK) plat_log("tod: level %d loads at once out of g_gameState %d, as the game's own load there", (int)planet, (int)state);
		return rc;
	}
	/* Revision 1.29: no fade-out first (tod.h, TOD_LOAD_FADE_TIME). */
	tod_fade_arm(tod_story_load(planet, TOD_LEVEL_FLAGS));
	rc = mem_write_u32(TOD_LOAD_REQUEST, planet);
	if (rc != ST_OK) {
		tod_fade_disarm();
		if (!was && TP->fl_forced) tod_fl_restore();
	}
	return rc;
}

/*
 * 2026-11-14 (tod.h, TOD_LEVEL_NAMES): the pause (g_gameState 5) once its
 * loading thread is done, and a realtime cinematic (9). The level's unload
 * runs both one's exits itself.
 */
int tod_load_leaves_now(u32 state, u32 pause)
{
	if (state == TOD_STATE_PAUSE) return pause != TOD_PAUSE_LOADING;
	return state == (u32)TOD_STATE_CINEMATIC;
}

/*
 * 2026-11-14: LoadLevel's branch for a game not in play, as data: the
 * level's name set as Level_setName sets it, g_saveData.currentLevel, then
 * GameState_Request(3, 3), the top three tiers marked and g_requestedGameState
 * 3 last, which is the word the game's GameState_Update acts on (and the one
 * that makes the load: tiers marked with no request are left as they are, so
 * a failure on the way puts the marks back). Never over a planet load or a
 * state change already asked for, and only with a level's tiers up. The
 * autosplitter and Fast loads see it as any planet load: the same tier
 * loader's job, g_currentLevel the destination as it begins.
 */
int tod_level_load_now(u32 level)
{
	u32 count = 0, requested = 0, request = 0, name = 0, old = 0, tiers[TOD_LEVEL_LOAD_TIERS], i;
	int rc;

	if (level >= TOD_NPLANETS) return ST_BAD_ARG;
	rc = mem_read_u32(TOD_REQUESTED_STATE, &requested);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LOAD_REQUEST, &request);
	if (rc == ST_OK) rc = mem_read_u32(TOD_TIER_STACK + TOD_TS_COUNT, &count);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LEVEL_NAMES + level * 4u, &name);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LEVEL_NAME, &old);
	if (rc != ST_OK) return rc;
	if (requested != TOD_REQUESTED_NONE || request != 0xFFFFFFFFu) return ST_BUSY;
	if (count < TOD_LEVEL_LOAD_TIERS || count > TOD_TIER_MAX || name == 0) return ST_BUSY;
	for (i = 0; i < TOD_LEVEL_LOAD_TIERS; i++) {
		rc = mem_read_u32(TOD_TIER_STACK + TOD_TS_TIERS + (count - 1u - i) * 4u, &tiers[i]);
		if (rc != ST_OK) return rc;
		if (tiers[i] == 0 || (tiers[i] & 3u) != 0) return ST_BUSY;
	}
	rc = mem_write_u32(TOD_LEVEL_NAME_LAST, name);
	if (rc == ST_OK) rc = mem_write_u32(TOD_LEVEL_NAME, name);
	if (rc == ST_OK) rc = mem_write_u32(TOD_PREV_LEVEL_NAME, old);
	if (rc == ST_OK) rc = mem_write_u32(TOD_CURRENT_LEVEL, level);
	for (i = 0; rc == ST_OK && i < TOD_LEVEL_LOAD_TIERS; i++) rc = mem_write_u8(tiers[i] + TOD_TIER_DYING, 1);
	if (rc == ST_OK) rc = mem_write_u32(TOD_REQUESTED_STATE, TOD_LEVEL_LOAD_STATE);
	if (rc != ST_OK)
		for (i = 0; i < TOD_LEVEL_LOAD_TIERS; i++) mem_write_u8(tiers[i] + TOD_TIER_DYING, 0);
	return rc;
}
