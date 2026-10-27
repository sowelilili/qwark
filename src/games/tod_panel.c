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
#include "../core/mem.h"
#include "../core/util.h"

#include <string.h>

/* -------------------------------------------------------------- the state */

/*
 * Infinite ammo's hold (below): each weapon's ammo as last held, f32 bits, and
 * the level object they were read under. The devices are kept full instead.
 */
struct tod_panel_state {
	u32 held[TOD_WPN_DEVICES];
	u32 hold_root;
	u8  hold_primed;
	u8  hold_wait;
};

#if QWARK_GAME_MODULE
#define TP QWARK_STATE_AT(struct tod_panel_state, QWARK_STATE_PANEL_OFF)
typedef char tod_panel_state_fits[sizeof(struct tod_panel_state) <= QWARK_STATE_PANEL_MAX ? 1 : -1];
#else
static struct tod_panel_state g_tod_panel;
#define TP (&g_tod_panel)
#endif

void tod_panel_reset(void)
{
	memset(TP, 0, sizeof(*TP));
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

int tod_trigger(u8 id)
{
	switch (id) {
	case TF_DIE:         return tod_die();
	case TF_UNLOCK_SP:   return skill_points_all(1);
	case TF_RESET_SP:    return skill_points_all(0);
	case TF_UNLOCK_GB:   return gold_bolts_all(1);
	case TF_RESET_GB:    return gold_bolts_all(0);
	case TF_SET_ASIDE:   return tod_set_aside();
	case TF_LOAD_ASIDE:  return tod_load_aside();
	default:             return ST_NOT_FOUND;
	}
}

int tod_set_value(u8 id, u32 value)
{
	switch (id) {
	case TF_BOLTS:      return mem_write_u32(TOD_BOLTS, value);
	case TF_RARITANIUM: return mem_write_u32(TOD_RARITANIUM, value);
	case TF_CHALLENGE:  return mem_write_u32(TOD_CHALLENGE, value);
	default:            return ST_NOT_FOUND;
	}
}

/* --------------------------------------------------------------- the cheats */

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
 * Infinite ammo: g_infiniteAmmo, the word hero_set_infinite_ammo sets, makes
 * every weapon fire with none left; the game still counts the ammo down, so
 * while the word is up the tick holds every weapon's ammo where it was and
 * keeps the devices full (tod_ammo_tick). No code is patched either way.
 *
 * Fast loads (id 17): g_levelTransitionMinTime, the 5 seconds
 * LevelTransition_waitLoad holds every planet load's transition for before it
 * even waits for the load itself, to 0, and back to the game's 5 when off. A
 * word of .data the game never writes, so a plain data write that works under
 * RPCS3; a new boot of the game starts at 5 again, and the toggle's auto flag
 * puts it back.
 */
int tod_set_toggle(u8 id, int on)
{
	switch (id) {
	case TF_INFINITE_HEALTH: return mem_write_u32(TOD_NO_DAMAGE, on ? 1u : 0u);
	case TF_GOD_RATCHET:     return mem_write_u8(TOD_PROXY_LOW, on ? (u8)TOD_PROXY_FAKE : 0u);
	case TF_FAST_LOADS:
		return mem_write_u32(TOD_TRANSITION_MIN, on ? TOD_TRANSITION_FAST : TOD_TRANSITION_GAME);
	case TF_INFINITE_AMMO:
		TP->hold_primed = 0;
		return mem_write_u32(TOD_INFINITE_AMMO, on ? 1u : 0u);
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
	case TF_INFINITE_HEALTH: rc = mem_read_u32(TOD_NO_DAMAGE, &v); break;
	case TF_INFINITE_AMMO:   rc = mem_read_u32(TOD_INFINITE_AMMO, &v); break;
	case TF_GOD_RATCHET:
		rc = mem_read_u8(TOD_PROXY_LOW, &b);
		v = b;
		break;
	case TF_FAST_LOADS:
		rc = mem_read_u32(TOD_TRANSITION_MIN, &v);
		v = v != TOD_TRANSITION_GAME;
		break;
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
 * WeaponDef_getMaxAmmo would add, and they have one.
 */
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

/*
 * Every fourth tick while g_infiniteAmmo is up: the weapons' and devices'
 * slots in one read. Any owned weapon whose ammo went down since the last
 * look gets the last value back, while a rise (a pickup, a vendor, a level
 * up) becomes the value held. A new level object, or the toggle going on,
 * starts the hold over from what is there.
 *
 * 2026-10-27, the user's first hardware test: that did not make the
 * Visi-Copter infinite. g_infiniteAmmo is read by Weapon_hasAmmo alone, which
 * lets the weapon in hand fire on an empty count, and the hold only puts back
 * a count that went down: a device at 0 (given or ticked Owned with none, or
 * whose last one went before the toggle) was never raised. So every owned
 * device is kept full instead, the way the game's own challenge 14 keeps
 * the Visi-Copters coming (hero_give_weapon with 999, every half second).
 */
#define HOLD_EVERY 4u

void tod_ammo_tick(u32 flag, u32 root)
{
	u8 *s;
	u8 w;

	if (flag == 0 || root == 0) {
		TP->hold_primed = 0;
		return;
	}
	if (TP->hold_wait != 0) {
		TP->hold_wait--;
		return;
	}
	TP->hold_wait = HOLD_EVERY - 1u;

	s = qscratch();
	if (mem_read(TOD_WEAPONS, s, TOD_WPN_GADGETS * TOD_WEAPON_STRIDE) != ST_OK) return;

	for (w = TOD_WPN_DEVICES; w < TOD_WPN_GADGETS; w++) {
		const u8 *slot = s + (u32)w * TOD_WEAPON_STRIDE;
		u32 full = tod_device_max_ammo(w);

		if (slot[TOD_WS_OWNED] == 0) continue;
		if (f32_from_bits(be32_get(slot + TOD_WS_AMMO)) < f32_from_bits(full))
			mem_write_u32(slot_addr(w) + TOD_WS_AMMO, full);
	}

	if (!TP->hold_primed || TP->hold_root != root) {
		for (w = 0; w < TOD_WPN_DEVICES; w++)
			TP->held[w] = be32_get(s + (u32)w * TOD_WEAPON_STRIDE + TOD_WS_AMMO);
		TP->hold_root = root;
		TP->hold_primed = 1;
		return;
	}

	for (w = TOD_WPN_FIRST; w < TOD_WPN_DEVICES; w++) {
		const u8 *slot = s + (u32)w * TOD_WEAPON_STRIDE;
		u32 now = be32_get(slot + TOD_WS_AMMO);

		if (slot[TOD_WS_OWNED] == 0) continue;
		if (f32_from_bits(now) < f32_from_bits(TP->held[w]))
			mem_write_u32(slot_addr(w) + TOD_WS_AMMO, TP->held[w]);
		else
			TP->held[w] = now;
	}
}

/* ---------------------------------------------------- positions, die, planets */

int tod_save_blob(u8 *blob, u8 *len)
{
	u32 hero;
	int rc = tod_hero(&hero);

	if (rc != ST_OK) return rc;
	rc = mem_read(hero + TOD_HERO_POS, blob, TOD_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = TOD_POS_BLOB_LEN;
	return ST_OK;
}

/*
 * The blob back, and its position also as the hero's previous one and the
 * moby's: the motion update copies pos to the moby every frame anyway, but
 * without prevPos the first frame derives a velocity from the jump.
 */
int tod_load_blob(const u8 *blob, u8 len)
{
	u32 hero, moby = 0;
	int rc;

	if (len != TOD_POS_BLOB_LEN) return ST_BAD_ARG;
	rc = tod_hero(&hero);
	if (rc != ST_OK) return rc;
	rc = mem_write(hero + TOD_HERO_POS, blob, TOD_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	rc = mem_write(hero + TOD_HERO_PREV_POS, blob, 16);
	if (rc != ST_OK) return rc;
	if (mem_read_u32(hero + TOD_HERO_MOBY, &moby) == ST_OK && moby != 0 && (moby & 3u) == 0)
		mem_write(moby + TOD_MOBY_POS, blob, 16);
	return ST_OK;
}

/*
 * Die: the hit points to 0. Hero_checkDeath, every frame, then puts the hero
 * into its death state, which zeroes the rest itself; the maximum is left as
 * it is.
 */
int tod_die(void)
{
	u32 hero;
	int rc = tod_hero(&hero);

	if (rc != ST_OK) return rc;
	return mem_write_u32(hero + TOD_HERO_HEALTH, 0);
}

/*
 * A planet: the level id into g_planetLoadRequest, which LoadLevel_ProcessPending
 * polls from Game_UpdateFrame: a fade, then the load, then -1 back. Exactly what
 * the game's own set_level does in gameplay, so only in gameplay (g_gameState 4,
 * a level object up), and never over a load already asked for: a request is
 * never taken back once written, as that would leave a fade with no load.
 */
int tod_planet_load(u8 planet, u8 flags)
{
	u32 state = 0, request = 0, root = 0;
	int rc;

	(void)flags;
	if (planet >= TOD_NPLANETS) return ST_BAD_ARG;
	rc = mem_read_u32(TOD_GAME_STATE, &state);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LOAD_REQUEST, &request);
	if (rc == ST_OK) rc = mem_read_u32(TOD_LEVEL_OBJ, &root);
	if (rc != ST_OK) return rc;
	if (state != TOD_STATE_GAMEPLAY || root == 0 || request != 0xFFFFFFFFu) return ST_BUSY;
	return mem_write_u32(TOD_LOAD_REQUEST, planet);
}
