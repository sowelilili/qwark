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
 * Infinite ammo's hold (below): each weapon's and device's ammo as last held,
 * f32 bits, and the level object they were read under.
 */
struct tod_panel_state {
	u32 held[TOD_WPN_GADGETS];
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
#define CAT_SP      4
#define CAT_GB      5

static const char * const tod_categories[] = {
	"Weapons", "Devices", "Gadgets", "Items", "Skill points", "Gold bolts"
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
	{ TOD_UL_OBJ + 12, CAT_ITEMS, OWNED, "Verdigris Upgrade" },
	{ TOD_UL_OBJ + 13, CAT_ITEMS, OWNED, "Praxus Upgrade" },
	{ TOD_UL_OBJ + 14, CAT_ITEMS, OWNED, "Hexagonal Washer" },

	/* The skill points, SKILLPOINT 0..59, named for their label keys (GEN_LABEL_SKILLPOINT_*). */
	{ TOD_UL_SP +  0, CAT_SP, OWNED, "Smashing" },
	{ TOD_UL_SP +  1, CAT_SP, OWNED, "In a Barrel" },
	{ TOD_UL_SP +  2, CAT_SP, OWNED, "Giant Hunter" },
	{ TOD_UL_SP +  3, CAT_SP, OWNED, "Wrench Ninja" },
	{ TOD_UL_SP +  4, CAT_SP, OWNED, "No Bridges" },
	{ TOD_UL_SP +  5, CAT_SP, OWNED, "Surface to Air" },
	{ TOD_UL_SP +  6, CAT_SP, OWNED, "Been Around" },
	{ TOD_UL_SP +  7, CAT_SP, OWNED, "Gonna Be Sick" },
	{ TOD_UL_SP +  8, CAT_SP, OWNED, "Fast and Fiery" },
	{ TOD_UL_SP +  9, CAT_SP, OWNED, "One Heckuva" },
	{ TOD_UL_SP + 10, CAT_SP, OWNED, "Alphabet" },
	{ TOD_UL_SP + 11, CAT_SP, OWNED, "Down to Size" },
	{ TOD_UL_SP + 12, CAT_SP, OWNED, "Dancin' Stars" },
	{ TOD_UL_SP + 13, CAT_SP, OWNED, "Medicine" },
	{ TOD_UL_SP + 14, CAT_SP, OWNED, "Preemptive" },
	{ TOD_UL_SP + 15, CAT_SP, OWNED, "Mutanty" },
	{ TOD_UL_SP + 16, CAT_SP, OWNED, "Consumer" },
	{ TOD_UL_SP + 17, CAT_SP, OWNED, "Live Strong" },
	{ TOD_UL_SP + 18, CAT_SP, OWNED, "Untouchable" },
	{ TOD_UL_SP + 19, CAT_SP, OWNED, "Freight Train" },
	{ TOD_UL_SP + 20, CAT_SP, OWNED, "Examiner" },
	{ TOD_UL_SP + 21, CAT_SP, OWNED, "Extinction" },
	{ TOD_UL_SP + 22, CAT_SP, OWNED, "Like Cold" },
	{ TOD_UL_SP + 23, CAT_SP, OWNED, "Hoedown" },
	{ TOD_UL_SP + 24, CAT_SP, OWNED, "Saucy Wrench" },
	{ TOD_UL_SP + 25, CAT_SP, OWNED, "On the Ceiling" },
	{ TOD_UL_SP + 26, CAT_SP, OWNED, "Seared Ahi" },
	{ TOD_UL_SP + 27, CAT_SP, OWNED, "Dead Aim" },
	{ TOD_UL_SP + 28, CAT_SP, OWNED, "Fire with Fire" },
	{ TOD_UL_SP + 29, CAT_SP, OWNED, "Six Gun Salute" },
	{ TOD_UL_SP + 30, CAT_SP, OWNED, "Gotta Catch" },
	{ TOD_UL_SP + 31, CAT_SP, OWNED, "Stay Still" },
	{ TOD_UL_SP + 32, CAT_SP, OWNED, "Now Boarding" },
	{ TOD_UL_SP + 33, CAT_SP, OWNED, "Low Flying" },
	{ TOD_UL_SP + 34, CAT_SP, OWNED, "Nowhere" },
	{ TOD_UL_SP + 35, CAT_SP, OWNED, "Collectors" },
	{ TOD_UL_SP + 36, CAT_SP, OWNED, "Minesweeper" },
	{ TOD_UL_SP + 37, CAT_SP, OWNED, "What's That R2" },
	{ TOD_UL_SP + 38, CAT_SP, OWNED, "You Sank My" },
	{ TOD_UL_SP + 39, CAT_SP, OWNED, "Pretty Lights" },
	{ TOD_UL_SP + 40, CAT_SP, OWNED, "Places to Be" },
	{ TOD_UL_SP + 41, CAT_SP, OWNED, "Marksman" },
	{ TOD_UL_SP + 42, CAT_SP, OWNED, "Can't Touch" },
	{ TOD_UL_SP + 43, CAT_SP, OWNED, "Pyew Pyew" },
	{ TOD_UL_SP + 44, CAT_SP, OWNED, "No Up Yours" },
	{ TOD_UL_SP + 45, CAT_SP, OWNED, "Roflcopter" },
	{ TOD_UL_SP + 46, CAT_SP, OWNED, "Golden Child" },
	{ TOD_UL_SP + 47, CAT_SP, OWNED, "Cheapskate" },
	{ TOD_UL_SP + 48, CAT_SP, OWNED, "Everybody" },
	{ TOD_UL_SP + 49, CAT_SP, OWNED, "Chorus Line" },
	{ TOD_UL_SP + 50, CAT_SP, OWNED, "Happy Feet" },
	{ TOD_UL_SP + 51, CAT_SP, OWNED, "Disco Inferno" },
	{ TOD_UL_SP + 52, CAT_SP, OWNED, "Bolts in the Bank" },
	{ TOD_UL_SP + 53, CAT_SP, OWNED, "South Pole" },
	{ TOD_UL_SP + 54, CAT_SP, OWNED, "Say Hello" },
	{ TOD_UL_SP + 55, CAT_SP, OWNED, "For the Hoard" },
	{ TOD_UL_SP + 56, CAT_SP, OWNED, "Inspector" },
	{ TOD_UL_SP + 57, CAT_SP, OWNED, "Thermonuclear" },
	{ TOD_UL_SP + 58, CAT_SP, OWNED, "Second Time" },
	{ TOD_UL_SP + 59, CAT_SP, OWNED, "Hardest Core" },

	/* The gold bolts, level by level in tod_gb_count's order (tod_gold_bolt). */
	{ TOD_UL_GB +  0, CAT_GB, OWNED, "Cobalia" },
	{ TOD_UL_GB +  1, CAT_GB, OWNED, "Stratus City 1" },
	{ TOD_UL_GB +  2, CAT_GB, OWNED, "Stratus City 2" },
	{ TOD_UL_GB +  3, CAT_GB, OWNED, "Space Combat I" },
	{ TOD_UL_GB +  4, CAT_GB, OWNED, "Imperial Fight Fest 1" },
	{ TOD_UL_GB +  5, CAT_GB, OWNED, "Imperial Fight Fest 2" },
	{ TOD_UL_GB +  6, CAT_GB, OWNED, "Imperial Fight Fest 3" },
	{ TOD_UL_GB +  7, CAT_GB, OWNED, "Imperial Fight Fest 4" },
	{ TOD_UL_GB +  8, CAT_GB, OWNED, "Apogee Space Station" },
	{ TOD_UL_GB +  9, CAT_GB, OWNED, "Pirate Base 1" },
	{ TOD_UL_GB + 10, CAT_GB, OWNED, "Pirate Base 2" },
	{ TOD_UL_GB + 11, CAT_GB, OWNED, "Pirate Base 3" },
	{ TOD_UL_GB + 12, CAT_GB, OWNED, "Space Combat II" },
	{ TOD_UL_GB + 13, CAT_GB, OWNED, "Rykan V 1" },
	{ TOD_UL_GB + 14, CAT_GB, OWNED, "Rykan V 2" },
	{ TOD_UL_GB + 15, CAT_GB, OWNED, "Sargasso 1" },
	{ TOD_UL_GB + 16, CAT_GB, OWNED, "Sargasso 2" },
	{ TOD_UL_GB + 17, CAT_GB, OWNED, "Sargasso 3" },
	{ TOD_UL_GB + 18, CAT_GB, OWNED, "Sargasso 4" },
	{ TOD_UL_GB + 19, CAT_GB, OWNED, "Iris" },
	{ TOD_UL_GB + 20, CAT_GB, OWNED, "Zordoom Prison 1" },
	{ TOD_UL_GB + 21, CAT_GB, OWNED, "Zordoom Prison 2" },
	{ TOD_UL_GB + 22, CAT_GB, OWNED, "Space Combat III" },
	{ TOD_UL_GB + 23, CAT_GB, OWNED, "Kerchu City 1" },
	{ TOD_UL_GB + 24, CAT_GB, OWNED, "Kerchu City 2" },
	{ TOD_UL_GB + 25, CAT_GB, OWNED, "Slag's Fleet 1" },
	{ TOD_UL_GB + 26, CAT_GB, OWNED, "Slag's Fleet 2" },
	{ TOD_UL_GB + 27, CAT_GB, OWNED, "Cragmite Ruins 1" },
	{ TOD_UL_GB + 28, CAT_GB, OWNED, "Cragmite Ruins 2" },
	{ TOD_UL_GB + 29, CAT_GB, OWNED, "Meridian City 1" },
	{ TOD_UL_GB + 30, CAT_GB, OWNED, "Meridian City 2" },
	{ TOD_UL_GB + 31, CAT_GB, OWNED, "Fastoon II" }
};

#undef WPN
#undef DEV

#define TOD_UNLOCK_COUNT ((u8)(sizeof(tod_unlocks) / sizeof(tod_unlocks[0])))

/*
 * The snapshot unlock_list takes (classic.h): the 32 weapon slots, the owned
 * objects, the skill points and each level's gold bolt word, which is a read
 * of its own per level that has any: 3 + 17 reads per list.
 */
#define SNAP_WEAPONS   0u
#define SNAP_OBJ       (TOD_NWEAPONS * TOD_WEAPON_STRIDE)              /* 0x280 */
#define SNAP_SP        (SNAP_OBJ + TOD_NOBJ_OWNED * 4u)
#define SNAP_GB        (SNAP_SP + 8u)
#define SNAP_LEN       (SNAP_GB + TOD_NPLANETS * 4u)
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
	u8 l;
	int rc;

	classic_snap_game = 0;
	rc = mem_read(TOD_WEAPONS, s + SNAP_WEAPONS, SNAP_OBJ);
	if (rc == ST_OK) rc = mem_read(TOD_OBJECTS, s + SNAP_OBJ, TOD_NOBJ_OWNED * 4u);
	if (rc == ST_OK) rc = mem_read(TOD_SKILL_POINTS, s + SNAP_SP, 8);
	for (l = 0; rc == ST_OK && l < TOD_NPLANETS; l++) {
		if (tod_gb_count[l] == 0) be32_put(s + SNAP_GB + l * 4u, 0);
		else rc = mem_read(gb_addr(l), s + SNAP_GB + l * 4u, 4);
	}
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

static int sp_bit(u64 word, u8 i)
{
	return (int)((word >> i) & 1u);
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
	if (id >= TOD_UL_OBJ && id < TOD_UL_OBJ + TOD_NOBJ_OWNED) {
		values[0] = be32_get(s + SNAP_OBJ + (u32)(id - TOD_UL_OBJ) * 4u) != 0;
		return ST_OK;
	}
	if (id >= TOD_UL_SP && id < TOD_UL_SP + TOD_NSKILL_POINTS) {
		values[0] = (u32)sp_bit(be64_get(s + SNAP_SP), (u8)(id - TOD_UL_SP));
		return ST_OK;
	}
	if (id >= TOD_UL_GB && id < TOD_UL_GB + TOD_NGOLD_BOLTS) {
		u8 level, bit;

		if (!tod_gold_bolt((u8)(id - TOD_UL_GB), &level, &bit)) return ST_NOT_FOUND;
		values[0] = (be32_get(s + SNAP_GB + (u32)level * 4u) >> bit) & 1u;
		return ST_OK;
	}
	return ST_NOT_FOUND;
}

/* A read-modify-write of one bit of a big-endian word of `width` bytes, 4 or 8. */
static int set_bit(u32 addr, u8 width, u8 bit, int on)
{
	u8 b[8];
	u64 v;
	int rc = mem_read(addr, b, width);

	if (rc != ST_OK) return rc;
	v = width == 8 ? be64_get(b) : (u64)be32_get(b);
	if (on) v |= (u64)1u << bit;
	else    v &= ~((u64)1u << bit);
	if (width == 8) be64_put(b, v);
	else            be32_put(b, (u32)v);
	return mem_write(addr, b, width);
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

	if (id >= TOD_UL_OBJ && id < TOD_UL_OBJ + TOD_NOBJ_OWNED)
		return mem_write_u32(TOD_OBJECTS + (u32)(id - TOD_UL_OBJ) * 4u, value ? 1u : 0u);
	if (id >= TOD_UL_SP && id < TOD_UL_SP + TOD_NSKILL_POINTS)
		return set_bit(TOD_SKILL_POINTS, 8, (u8)(id - TOD_UL_SP), value != 0);
	if (id >= TOD_UL_GB && id < TOD_UL_GB + TOD_NGOLD_BOLTS) {
		u8 level, bit;

		if (!tod_gold_bolt((u8)(id - TOD_UL_GB), &level, &bit)) return ST_NOT_FOUND;
		return set_bit(gb_addr(level), 4, bit, value != 0);
	}
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

static int ryno_all(int on)
{
	u32 v = 0;
	int rc;

	if (!on) return mem_write_u32(TOD_RYNO_PLANS, 0);
	rc = mem_read_u32(TOD_RYNO_PLANS, &v);
	if (rc != ST_OK) return rc;
	return mem_write_u32(TOD_RYNO_PLANS, v | TOD_RYNO_ALL);
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
	case TF_UNLOCK_RYNO: return ryno_all(1);
	case TF_RESET_RYNO:  return ryno_all(0);
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
 * God Ratchet: the game's own "hero takes no damage" word, g_debugHeroNoDamage,
 * which Hero_Update checks before it applies any damage event, on foot and in
 * the ship; nothing in the game writes it. The old RaCMAN's god ratchet wrote
 * a byte of g_heroProxyMobyHandle (0x1020BD48) instead, the remote-control
 * moby's handle, which drops the damage as a side effect and makes the enemies
 * ignore Ratchet; a fake handle there is what a real proxy moby's spawn or
 * death overwrites. Neither stops a death plane or a fall.
 *
 * Infinite ammo: g_infiniteAmmo, the word hero_set_infinite_ammo sets, makes
 * every weapon fire with none left; the game still counts the ammo down, so
 * while the word is up the tick holds every weapon's ammo where it was
 * (tod_ammo_tick). No code is patched either way.
 */
int tod_set_toggle(u8 id, int on)
{
	switch (id) {
	case TF_GOD_RATCHET: return mem_write_u32(TOD_NO_DAMAGE, on ? 1u : 0u);
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
	int rc;

	switch (id) {
	case TF_GOD_RATCHET:   rc = mem_read_u32(TOD_NO_DAMAGE, &v); break;
	case TF_INFINITE_AMMO: rc = mem_read_u32(TOD_INFINITE_AMMO, &v); break;
	default:               return ST_NOT_FOUND;
	}
	if (rc != ST_OK) return rc;
	*on = v != 0;
	return ST_OK;
}

/*
 * Every fourth tick while g_infiniteAmmo is up: the weapons' and devices'
 * slots in one read, and any owned one whose ammo went down since the last
 * look gets the last value back, while a rise (a pickup, a vendor, a level
 * up) becomes the value held. A new level object, or the toggle going on,
 * starts the hold over from what is there.
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

	if (!TP->hold_primed || TP->hold_root != root) {
		for (w = 0; w < TOD_WPN_GADGETS; w++)
			TP->held[w] = be32_get(s + (u32)w * TOD_WEAPON_STRIDE + TOD_WS_AMMO);
		TP->hold_root = root;
		TP->hold_primed = 1;
		return;
	}

	for (w = TOD_WPN_FIRST; w < TOD_WPN_GADGETS; w++) {
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
