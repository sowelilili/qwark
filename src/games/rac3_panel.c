/*
 * Ratchet & Clank 3 (NPEA00387, BCES01503): the handlers behind the descriptor
 * table, the planet load, the level flag region and the unlock table.
 *
 * The addresses live in rac3.h. Everything here is a straight port of what
 * RAC3Form and UYAUnlocks did, and where the old code was ambiguous the comment
 * says what was assumed so it can be checked on hardware.
 *
 * All of it runs on the tick thread, through the command ring.
 */
#include "rac3.h"
#include "classic.h"
#include "../core/mem.h"
#include "../core/savefile.h"
#include "../core/util.h"
#include "sfhelper_bins.h"

#include <string.h>

/* ------------------------------------------------------------ the unlocks */

/*
 * UYAUnlocks.cs, item for item. The three offsets in that file are the raw
 * spreadsheet columns; the UYAItem constructor subtracts a base from each, so
 * the live addresses are
 *
 *   unlock  RAC3_UNLOCK_ARRAY + (unlock - 0x4A8)
 *   exp     RAC3_EXP_ARRAY    + (exp    - 0x5F0)
 *   ammo    RAC3_AMMO_ARRAY   + (ammo   - 0x243)
 *
 * A raw 0 in the exp or ammo column means the item has none: the five vid comics
 * carry 0 there and the C# subtraction wrapped, so the old form never touched
 * those words either.
 *
 * The gadgets keep their exp and ammo columns because that is what the
 * spreadsheet holds, but no gadget row declares either slot: UYA gives a gadget
 * no version, no experience and no ammunition, so those words are not counts
 * the player can act on. Only the weapons carry the other three slots.
 *
 * `levels` is how many versions the weapon has. Anything above 1 gets the level
 * field; SetVersion writes one byte into the item array, and the five GC weapons
 * have a table of their own for versions 2 and up, which is what `v2` and
 * `vbase` are.
 */
#define CAT_WEAPONS 0
#define CAT_GADGETS 1
#define CAT_COMICS  2

struct rac3_item {
	u16 id;
	u16 unlock;    /* raw 0x4A8-based offset */
	u16 exp;       /* raw 0x5F0-based offset, 0 = none */
	u16 ammo;      /* raw 0x243-based offset, 0 = none */
	u8  levels;
	u8  gc;        /* a GC weapon: its live version cannot be read back */
	u16 v2;        /* table offset for version 2, 0 = the ordinary id + n - 1 */
	u16 vbase;     /* table base for versions 3 and up */
};

static const char * const rac3_categories[] = { "Weapons", "Gadgets and items", "Vid comics" };

/*
 * Protocol 1.3, the four value slots as RaC3 uses them. Gold weapons are a RaC1
 * idea and RaC3 has none, so slot 1 is the weapon VERSION that UYAUnlocks drew
 * as a v1..v8 combo box, and slot 2 is the experience its xBox edited. Before
 * 1.3 those two slots were called "Gold" and "Level" on the wire, so a client
 * drew the version as a checkbox and could only ever write v1; the field
 * descriptors are what stop that.
 *
 * The version maximum is game-wide: every levelled weapon goes to 8 except the
 * R3YNO, which stops at 5. UNLOCK_SET clamps per entry, so a client that offers
 * 8 everywhere still cannot push the R3YNO past v5.
 *
 * Slots 1 to 3 belong to the weapons alone. A gadget and a vid comic are owned
 * or not owned and nothing else, so their rows declare slot 0 by itself and a
 * client draws no level, XP or ammo cell against them.
 */
static const struct unlock_field_desc rac3_fields[4] = {
	{ "Owned", UNLOCK_KIND_FLAG,   0 },
	{ "Level", UNLOCK_KIND_NUMBER, RAC3_MAX_LEVELS },
	{ "XP",    UNLOCK_KIND_NUMBER, 0 },
	{ "Ammo",  UNLOCK_KIND_NUMBER, 0 }
};

#define OWNED UNLOCK_FIELD_0
#define LEVEL UNLOCK_FIELD_1
#define XP    UNLOCK_FIELD_2
#define AMMO  UNLOCK_FIELD_3

/*
 * Id 0, the Bomb Glove, is retired: UYA keeps it out of both menus and there is
 * no way to reach it in game, so an entry for it only ever promised something
 * the player could not have. Ids are never renumbered, so the table starts at 1
 * and 0 answers BAD_ARG.
 */
static const struct game_unlock rac3_unlocks[] = {
	{  1, CAT_GADGETS, OWNED, "Heli Pack" },
	{  2, CAT_GADGETS, OWNED, "Thruster Pack" },
	{  3, CAT_GADGETS, OWNED, "Charge Boots" },
	{  4, CAT_GADGETS, OWNED, "Gravity Boots" },
	{  5, CAT_GADGETS, OWNED, "Tyhrra Guise" },
	{  6, CAT_GADGETS, OWNED, "Refractor" },
	{  7, CAT_GADGETS, OWNED, "Hypershot" },
	{  8, CAT_GADGETS, OWNED, "Nano Pak" },
	{  9, CAT_GADGETS, OWNED, "PDA" },
	{ 10, CAT_GADGETS, OWNED, "Bolt Grabber v2" },
	{ 11, CAT_GADGETS, OWNED, "Map-o-matic" },
	{ 12, CAT_GADGETS, OWNED, "Master Plan" },
	{ 13, CAT_GADGETS, OWNED, "Star Map" },
	{ 14, CAT_GADGETS, OWNED, "The Hacker" },
	{ 15, CAT_GADGETS, OWNED, "Warp Pad" },

	{ 16, CAT_COMICS, OWNED, "Vid Comic 1" },
	{ 17, CAT_COMICS, OWNED, "Vid Comic 2" },
	{ 18, CAT_COMICS, OWNED, "Vid Comic 3" },
	{ 19, CAT_COMICS, OWNED, "Vid Comic 4" },
	{ 20, CAT_COMICS, OWNED, "Vid Comic 5" },

	{ 21, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Agents of Doom" },
	{ 22, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Annihilator" },
	{ 23, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Bouncer" },
	{ 24, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Disc Blade Gun" },
	{ 25, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Flux Rifle" },
	{ 26, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Holoshield" },
	{ 27, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Infector" },
	{ 28, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Lava Gun" },
	{ 29, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Miniturret" },
	{ 30, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Nitro Launcher" },
	{ 31, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "N60 Storm" },
	{ 32, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Plasma Coil" },
	{ 33, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Plasma Whip" },
	{ 34, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Quack-O-Ray" },
	{ 35, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Rift Inducer" },
	{ 36, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "R3YNO" },
	{ 37, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Shield Charger" },
	{ 38, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Shock Blaster" },
	{ 39, CAT_WEAPONS, OWNED | LEVEL | XP | AMMO, "Spitting Hydra" },
	/* The Suck Cannon carries no ammo in game; its ammo word is not the count. */
	{ 40, CAT_WEAPONS, OWNED | LEVEL | XP,        "Suck Cannon" }
};

/*
 * OWNED, LEVEL, XP and AMMO stay defined for the rest of the file: unlock_read
 * and unlock_set test the same four slot bits, and spelling them out there is
 * what keeps the read and the descriptor table honest with each other.
 */

/*
 * Parallel to rac3_unlocks, row by row. It stopped running id by id when the
 * Bomb Glove was retired, so both handlers look a row up by its id.
 */
static const struct rac3_item rac3_items[] = {
	{ 0x02, 0x4AA, 0x5F8, 0x24B, 1, 0, 0, 0 },      /* Heli Pack */
	{ 0x03, 0x4AB, 0x5FC, 0x24F, 1, 0, 0, 0 },      /* Thruster Pack */
	{ 0x1D, 0x4C5, 0x664, 0x2B7, 1, 0, 0, 0 },      /* Charge Boots */
	{ 0x0D, 0x4B5, 0x624, 0x277, 1, 0, 0, 0 },      /* Gravity Boots */
	{ 0x1E, 0x4C6, 0x668, 0x2BB, 1, 0, 0, 0 },      /* Tyhrra Guise */
	{ 0x12, 0x4BA, 0x638, 0x28B, 1, 0, 0, 0 },      /* Refractor */
	{ 0x0B, 0x4B3, 0x61C, 0x26F, 1, 0, 0, 0 },      /* Hypershot */
	{ 0x20, 0x4C8, 0x670, 0x2C3, 1, 0, 0, 0 },      /* Nano Pak */
	{ 0x23, 0x4CB, 0x67C, 0x2CF, 1, 0, 0, 0 },      /* PDA */
	{ 0x07, 0x4AF, 0x60C, 0x25F, 1, 0, 0, 0 },      /* Bolt Grabber v2 */
	{ 0x05, 0x4AD, 0x604, 0x257, 1, 0, 0, 0 },      /* Map-o-matic */
	{ 0x22, 0x4CA, 0x678, 0x2CB, 1, 0, 0, 0 },      /* Master Plan */
	{ 0x21, 0x4C9, 0x674, 0x2C7, 1, 0, 0, 0 },      /* Star Map */
	{ 0x14, 0x4BC, 0x640, 0x293, 1, 0, 0, 0 },      /* The Hacker */
	{ 0x1F, 0x4C7, 0x66C, 0x2BF, 1, 0, 0, 0 },      /* Warp Pad */

	{ 0x00, 0x12C7, 0, 0, 1, 0, 0, 0 },             /* Vid Comic 1 */
	{ 0x00, 0x12C9, 0, 0, 1, 0, 0, 0 },             /* Vid Comic 2 */
	{ 0x00, 0x12CA, 0, 0, 1, 0, 0, 0 },             /* Vid Comic 3 */
	{ 0x00, 0x12C8, 0, 0, 1, 0, 0, 0 },             /* Vid Comic 4 */
	{ 0x00, 0x12CB, 0, 0, 1, 0, 0, 0 },             /* Vid Comic 5 */

	{ 0x57, 0x4FF, 0x74C, 0x39F, 8, 0, 0, 0 },      /* Agents of Doom */
	{ 0x3F, 0x4E7, 0x6EC, 0x33F, 8, 0, 0, 0 },      /* Annihilator */
	{ 0x13, 0x4BB, 0x63C, 0x28F, 8, 1, 0xA6, 0xB1 },/* Bouncer, GC */
	{ 0x4F, 0x4F7, 0x72C, 0x37F, 8, 0, 0, 0 },      /* Disc Blade Gun */
	{ 0x6F, 0x517, 0x7AC, 0x3FF, 8, 0, 0, 0 },      /* Flux Rifle */
	{ 0x67, 0x50F, 0x78C, 0x3DF, 8, 0, 0, 0 },      /* Holoshield */
	{ 0x37, 0x4DF, 0x6CC, 0x31F, 8, 0, 0, 0 },      /* Infector */
	{ 0x11, 0x4B9, 0x634, 0x287, 8, 1, 0xA1, 0xAB },/* Lava Gun, GC */
	{ 0x15, 0x4BD, 0x644, 0x297, 8, 1, 0xA2, 0xA5 },/* Miniturret, GC */
	{ 0x77, 0x51F, 0x7CC, 0x41F, 8, 0, 0, 0 },      /* Nitro Launcher */
	{ 0x2F, 0x4D7, 0x6AC, 0x2FF, 8, 0, 0, 0 },      /* N60 Storm */
	{ 0x10, 0x4B8, 0x630, 0x283, 8, 1, 0xA0, 0xB7 },/* Plasma Coil, GC */
	{ 0x7F, 0x527, 0x7EC, 0x43F, 8, 0, 0, 0 },      /* Plasma Whip */
	{ 0x8F, 0x537, 0x82C, 0x47F, 8, 0, 0, 0 },      /* Quack-O-Ray */
	{ 0x5F, 0x507, 0x76C, 0x3BF, 8, 0, 0, 0 },      /* Rift Inducer */
	{ 0x97, 0x53F, 0x84C, 0x49F, 5, 0, 0, 0 },      /* R3YNO */
	{ 0x16, 0x4BE, 0x648, 0x29B, 8, 1, 0xA7, 0xBD },/* Shield Charger, GC */
	{ 0x27, 0x4CF, 0x68C, 0x2DF, 8, 0, 0, 0 },      /* Shock Blaster */
	{ 0x47, 0x4EF, 0x70C, 0x35F, 8, 0, 0, 0 },      /* Spitting Hydra */
	{ 0x87, 0x52F, 0x80C, 0x45F, 8, 0, 0, 0 }       /* Suck Cannon */
};

#define RAC3_UNLOCK_COUNT ((u8)(sizeof(rac3_unlocks) / sizeof(rac3_unlocks[0])))

/*
 * The row an id names, or -1 for an id no row carries, which is every retired
 * one. Both tables are indexed by the row this returns.
 */
static int rac3_row_for_id(u8 id)
{
	u8 i;

	for (i = 0; i < RAC3_UNLOCK_COUNT; i++)
		if (rac3_unlocks[i].id == id) return (int)i;

	return -1;
}

static u32 item_unlock_addr(const struct rac3_item *it)
{
	return RAC3_UNLOCK_ARRAY + ((u32)it->unlock - 0x4A8u);
}

static u32 item_exp_addr(const struct rac3_item *it)
{
	return it->exp == 0 ? 0 : RAC3_EXP_ARRAY + ((u32)it->exp - 0x5F0u);
}

static u32 item_ammo_addr(const struct rac3_item *it)
{
	return it->ammo == 0 ? 0 : RAC3_AMMO_ARRAY + ((u32)it->ammo - 0x243u);
}

/* UYAItem.VersionNTableOffset. */
static u8 item_version_byte(const struct rac3_item *it, u32 version)
{
	if (version <= 1) return (u8)it->id;
	if (it->v2 != 0) {
		if (version == 2) return (u8)it->v2;
		return (u8)(it->vbase + version);
	}
	return (u8)(it->id + version - 1);
}

static int item_set_version(const struct rac3_item *it, u32 version)
{
	if (it->levels <= 1) return ST_UNSUPPORTED;

	/*
	 * Clamp rather than refuse. The level field's advertised maximum is the
	 * game-wide 8, because UnlockFieldDesc.max is one number for the whole
	 * table; the R3YNO stops at v5 and its own `levels` is what decides here.
	 */
	if (version < 1) version = 1;
	if (version > it->levels) version = it->levels;

	return mem_write_u8(RAC3_ITEM_ARRAY + it->id, item_version_byte(it, version));
}

/*
 * Three reads cover the live table:
 *
 *   A  0xDA5240 .. 0xDA5A84   the ammo array, the unlock array and the exp array,
 *                             which happen to sit in that order
 *   B  0xDA650B .. 0xDA6513   the five vid comic bytes, far past the others
 *   C  0xC1E43C .. 0xC1E4D4   the item array, for the weapon versions
 */
#define SNAP_A_ADDR RAC3_AMMO_ARRAY
#define SNAP_A_LEN  0x844u
#define SNAP_B_ADDR RAC3_VID_COMICS
#define SNAP_B_LEN  8u
#define SNAP_C_ADDR RAC3_ITEM_ARRAY
#define SNAP_C_LEN  0x98u

static u8 g_snap_a[SNAP_A_LEN];
static u8 g_snap_b[SNAP_B_LEN];
static u8 g_snap_c[SNAP_C_LEN];
static int g_snap_valid;

static int snap_byte(u32 addr, u8 *out)
{
	if (!g_snap_valid || addr == 0) return 0;

	if (addr >= SNAP_A_ADDR && addr < SNAP_A_ADDR + SNAP_A_LEN) {
		*out = g_snap_a[addr - SNAP_A_ADDR];
		return 1;
	}
	if (addr >= SNAP_B_ADDR && addr < SNAP_B_ADDR + SNAP_B_LEN) {
		*out = g_snap_b[addr - SNAP_B_ADDR];
		return 1;
	}
	if (addr >= SNAP_C_ADDR && addr < SNAP_C_ADDR + SNAP_C_LEN) {
		*out = g_snap_c[addr - SNAP_C_ADDR];
		return 1;
	}
	return 0;
}

static int snap_word(u32 addr, u32 *out)
{
	if (!g_snap_valid || addr == 0) return 0;
	if (addr < SNAP_A_ADDR || addr + 4 > SNAP_A_ADDR + SNAP_A_LEN) return 0;

	*out = be32_get(g_snap_a + (addr - SNAP_A_ADDR));
	return 1;
}

int rac3_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields)
{
	int rc;

	*list        = rac3_unlocks;
	*count       = RAC3_UNLOCK_COUNT;
	*categories  = rac3_categories;
	*ncategories = (u8)(sizeof(rac3_categories) / sizeof(rac3_categories[0]));
	*fields      = rac3_fields;

	g_snap_valid = 0;

	rc = mem_read(SNAP_A_ADDR, g_snap_a, SNAP_A_LEN);
	if (rc != ST_OK) return rc;
	rc = mem_read(SNAP_B_ADDR, g_snap_b, SNAP_B_LEN);
	if (rc != ST_OK) return rc;
	rc = mem_read(SNAP_C_ADDR, g_snap_c, SNAP_C_LEN);
	if (rc != ST_OK) return rc;

	g_snap_valid = 1;
	return ST_OK;
}

int rac3_unlock_read(const struct game_unlock *entry, u32 values[4])
{
	const struct rac3_item *it;
	int row;
	u8 b = 0;
	u32 w = 0;

	if (entry == NULL) return ST_BAD_ARG;
	row = rac3_row_for_id(entry->id);
	if (row < 0) return ST_BAD_ARG;
	it = &rac3_items[row];

	values[0] = values[1] = values[2] = values[3] = 0;

	if (snap_byte(item_unlock_addr(it), &b)) values[0] = b;

	/*
	 * UYAItem.GetVersionHeuristic: the item array byte minus the item id plus
	 * one. It cannot work for the five GC weapons, whose versions 2 and up live
	 * in a table of their own, so those report 0 exactly as the old form did.
	 *
	 * The old form did that subtraction in unsigned arithmetic, so a byte below
	 * the item id (an unowned weapon, or a stray GC table offset) came back as
	 * a four-billion "level" that its combo box then ignored. A number box on
	 * the client would show it, so anything outside 1..levels reports 0 here,
	 * which is already the GC weapons' way of saying "not known".
	 */
	if ((entry->fields & LEVEL) != 0 && !it->gc &&
	    snap_byte(RAC3_ITEM_ARRAY + it->id, &b)) {
		int v = (int)b - (int)it->id + 1;
		if (v >= 1 && v <= (int)it->levels) values[1] = (u32)v;
	}

	if ((entry->fields & XP) != 0 && snap_word(item_exp_addr(it), &w))
		values[2] = w;

	if ((entry->fields & AMMO) != 0 && snap_word(item_ammo_addr(it), &w))
		values[3] = w;

	return ST_OK;
}

int rac3_unlock_set(u8 id, u8 field, u32 value)
{
	const struct rac3_item *it;
	int row = rac3_row_for_id(id);

	if (row < 0) return ST_BAD_ARG;
	if (field > 3) return ST_UNSUPPORTED;

	/*
	 * The row's declared fields are the contract, so a slot it does not offer
	 * is refused even where the address behind it happens to exist: a gadget's
	 * exp and ammo words are not counts the game keeps, and the Suck Cannon's
	 * ammo word is not the count the game uses.
	 */
	if ((rac3_unlocks[row].fields & (u8)(1u << field)) == 0) return ST_UNSUPPORTED;

	it = &rac3_items[row];

	switch (field) {
	case 0:
		return mem_write_u8(item_unlock_addr(it), value != 0 ? 1 : 0);

	case 1:
		return item_set_version(it, value);

	case 2:
		if (item_exp_addr(it) == 0) return ST_UNSUPPORTED;
		return mem_write_u32(item_exp_addr(it), value);

	default:
		if (item_ammo_addr(it) == 0) return ST_UNSUPPORTED;
		return mem_write_u32(item_ammo_addr(it), value);
	}
}

/* ------------------------------------------------------------ level flags */

/*
 * The old client's flag viewer read levelFlags + planetToLoad * 0x10, where
 * planetToLoad is the one-based planet id, and rac3.cs never implemented a
 * reset. The region is kept exactly as the viewer had it and the reset writes
 * sixteen zeros over it, which is what the other games' resets do.
 */
int rac3_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len)
{
	int rc;

	*len = 0;
	if (planet == 0 || planet >= rac3_planet_count()) return ST_BAD_ARG;
	if (cap < RAC3_LF_LEN) return ST_FULL;

	rc = mem_read(RAC3_LEVEL_FLAGS + (u32)planet * RAC3_LF_LEN, out, RAC3_LF_LEN);
	if (rc != ST_OK) return rc;

	*len = RAC3_LF_LEN;
	return ST_OK;
}

int rac3_levelflags_set(u8 planet, u16 offset, u8 value)
{
	if (planet == 0 || planet >= rac3_planet_count()) return ST_BAD_ARG;
	if (offset >= RAC3_LF_LEN) return ST_BAD_ARG;

	return mem_write_u8(RAC3_LEVEL_FLAGS + (u32)planet * RAC3_LF_LEN + offset, value);
}

int rac3_levelflags_reset(u8 planet)
{
	if (planet == 0 || planet >= rac3_planet_count()) return ST_BAD_ARG;
	return mem_write_zeros(RAC3_LEVEL_FLAGS + (u32)planet * RAC3_LF_LEN, RAC3_LF_LEN);
}

/* ---------------------------------------------------------------- planets */

int rac3_planet_load(u8 planet, u8 flags)
{
	int rc;

	/* Index 0 is the placeholder that keeps the list aligned with the ids. */
	if (planet == 0 || planet >= rac3_planet_count()) return ST_BAD_ARG;

	rc = classic_planet_request(RAC3_LOAD_PLANET, planet);
	if (rc != ST_OK) return rc;

	/* rac3.cs LoadPlanetSafe: every planet but Aquatos also gets fast loads. */
	if (planet != RAC3_PLANET_AQUATOS) {
		rc = rac3_arm_fast_loads();
		if (rc != ST_OK) return rc;
	}

	if (flags & PLANET_FLAG_RESET_LEVELFLAGS) {
		rc = rac3_levelflags_reset(planet);
		if (rc != ST_OK) return rc;
	}

	/* RaC3 has no per-planet special bolt, so PLANET_FLAG_RESET_BOLTS is a no-op. */
	return ST_OK;
}

/* --------------------------------------------------------------- savefile */

/*
 * Both requests go through src/core/savefile.c, which installs the helper if
 * this process has not had it yet.
 */
int rac3_load_setaside(void)
{
	return savefile_load_aside();
}

/*
 * Build 53. Whether a save starts at `addr`: the two block sizes the game's
 * restore accepts at its head. 1 yes, 0 no, -1 when the read failed.
 */
static int rac3_is_save(u32 addr)
{
	u8 head[8];
	u32 global, planet;

	if (mem_read(addr, head, sizeof(head)) != ST_OK) return -1;
	global = be32_get(head);
	planet = be32_get(head + 4);
	if (global != RAC3_SAVE_HEAD_GLOBAL && global != RAC3_SAVE_HEAD_GLOBAL_OLD) return 0;
	return planet == RAC3_SAVE_HEAD_PLANET || planet == RAC3_SAVE_HEAD_PLANET_OLD;
}

/*
 * Build 53, the savefile_ready seam, which this game answered with NULL - always
 * ready - until its helper started pausing the game before a load and checking
 * for a save. It makes the helper's own tests on the tick before a request goes
 * out, so the client hears why at once.
 *
 * A set-aside needs the game's save buffer and a save in it, which there is not
 * until the game has saved or loaded once in this process: before then the
 * helper used to copy two megabytes from address 0, or from address 4 when the
 * save manager itself was not there yet. A load needs no planet load on its way
 * and a mode the helper can ask the pause from - any but space, debug and the
 * ones past 9 - and a save to hand over: the aside buffer, or the game's own
 * when the aside buffer is empty. A RESTORE needs the game state alone; its
 * file is asked about as a load once it is in the aside buffer.
 *
 * A read that fails answers "not idle", as the other games' do.
 */
u8 rac3_savefile_ready(u8 what)
{
	const struct sf_desc *d = sf_desc_for_game(GAME_RAC3);
	u32 info = 0;
	u32 buf = 0;

	if (d == NULL) return SAVEFILE_ERR_NONE;

	if (what != GAME_SF_SET_ASIDE) {
		u32 request = 0, mode = 0, first = 0;

		if (mem_read_u32(RAC3_LOAD_PLANET, &request) != ST_OK ||
		    mem_read_u32(RAC3_GAME_STATE, &mode) != ST_OK)
			return SAVEFILE_ERR_NOT_IDLE;
		if (request != 0) return SAVEFILE_ERR_NOT_IDLE;
		if (mode > RAC3_GAME_MODE_LAST || mode == RAC3_GAME_MODE_SPACE) return SAVEFILE_ERR_NOT_IDLE;
		if (what == GAME_SF_RESTORE) return SAVEFILE_ERR_NONE;

		if (mem_read_u32(d->aside_addr, &first) != ST_OK) return SAVEFILE_ERR_NOT_IDLE;
		if (first != 0) {
			int save = rac3_is_save(d->aside_addr);
			if (save < 0) return SAVEFILE_ERR_NOT_IDLE;
			return save ? SAVEFILE_ERR_NONE : SAVEFILE_ERR_NO_SAVE;
		}
	}

	if (mem_read_u32(RAC3_SAVEDATA_INFO, &info) != ST_OK) return SAVEFILE_ERR_NOT_IDLE;
	if (info == 0) return SAVEFILE_ERR_NO_SAVE;
	if (mem_read_u32(info + RAC3_SAVEDATA_FIELD, &buf) != ST_OK) return SAVEFILE_ERR_NOT_IDLE;
	if (buf == 0) return SAVEFILE_ERR_NO_SAVE;

	switch (rac3_is_save(buf)) {
	case 1:  return SAVEFILE_ERR_NONE;
	case 0:  return SAVEFILE_ERR_NO_SAVE;
	default: return SAVEFILE_ERR_NOT_IDLE;
	}
}

/* ----------------------------------------------------------------- enums */

static const char * const rac3_armour_options[RAC3_ARMOUR_COUNT] = {
	"Alpha Combat Suit",
	"Magnaplate Armor",
	"Adamantine Armor",
	"Aegis Mark V Armor",
	"Infernox Armor",
	"OG Ratchet Skin",
	"Snowman Skin",
	"Tux Skin"
};

static const char * const rac3_ship_options[RAC3_SHIP_COUNT] = {
	"Blargian Red",
	"Orxon Green",
	"Bogon Blue",
	"Insomniac Special",
	"Dark Nebula",
	"Drek's Black Heart",
	"Space Storm",
	"Lunar Eclipse",
	"Plaidtastic",
	"Supernova",
	"Solar Wind",
	"Clowner",
	"Silent Strike",
	"Lombax Orange",
	"Neutron Star",
	"Star Traveller",
	"Hooked on Onyx",
	"Tyhrranoid Void",
	"Zeldrin Sunset",
	"Ghost Pirate Purple",
	"Qwark Green",
	"Agent Orange",
	"Helga Hues",
	"Amoeboid Green",
	"Obani Orange",
	"Pulsing Purple",
	"Low Rider",
	"Black Hole",
	"Sun Storm",
	"Sasha Scarlet",
	"Florana Breeze",
	"Ozzy Kamikaze"
};

int rac3_get_options(u8 id, const char * const **options, u8 *count)
{
	if (id == R3_ARMOUR) {
		*options = rac3_armour_options;
		*count = RAC3_ARMOUR_COUNT;
		return ST_OK;
	}

	if (id == R3_SHIP_COLOUR) {
		*options = rac3_ship_options;
		*count = RAC3_SHIP_COUNT;
		return ST_OK;
	}

	return ST_NOT_FOUND;
}

/* ---------------------------------------------------------------- trophies */

/*
 * Build 53: the trophy reset, behind R3_RESET_TROPHIES and the R3_AUTO_TROPHIES
 * toggle, and what the old trainer's Reset Trophies button did with the rc3-save
 * mod loaded. Two halves, in this order:
 *
 *   1. The console's trophy data for this game goes. The PS3 keeps a user's
 *      unlocks of a trophy set in /dev_hdd0/home/<user>/trophy/<NP comm id>
 *      (TROPUSR.DAT among the rest), and sceNpTrophyUnlockTrophy on a trophy
 *      that file already has unlocked answers SCE_NP_TROPHY_ERROR_ALREADY_
 *      UNLOCKED and shows nothing: no amount of re-registering in the game makes
 *      an earned trophy pop again while that file is there. The old button
 *      deleted the folder over Ratchetron's DeleteDirectory before it wrote its
 *      request (RAC3Form.cs DeleteTrophyFolder, commit f987a1c, the same day
 *      Ratchetron's delete command went in for it). qwark deletes it itself.
 *   2. The game forgets and re-reads its trophies: the savefile helper's reset
 *      (sf_trophy_reset, sf_rac3.h), on the game's thread, destroys the game's
 *      trophy context and registers it again - which installs the trophy set
 *      afresh, nothing unlocked - and leaves the game's own cache saying that
 *      trophies 10, 25 and 32 are still to be won and every other one is won,
 *      exactly as rc3-save left it. The game stands still for three seconds
 *      while it does.
 *
 * The folder is found, not named. Its name is the communication id the running
 * game registers under, read out of the game (RAC3_TROPHY_COMMID_PTR); its
 * parent is the home folder of the user signed in now (plat_user_id, the XMB's
 * own number), and it counts as this game's only if its TROPCONF.SFM or TROP.SFM
 * names that same id. Anything else refuses the whole reset with NOT_FOUND
 * before a byte is deleted: no id, a malformed id, no user, no such home or
 * trophy folder, a folder whose files name another set, or a folder holding
 * folders of its own. A folder that is not there at all is a set with nothing
 * to delete, and the reset goes on to the game's half. Only the files directly
 * in that one folder are deleted, a few a tick, then the folder; nothing is
 * walked into, and every step is logged.
 *
 * Everything here is refused under RPCS3 (UNSUPPORTED). The emulator keeps a
 * trophy set's data in its own dev_hdd0, which qwark-rpcs3 cannot reach - its
 * file ops are rooted in a folder of its own - and nothing in qwark touches
 * RPCS3's files; the game's half alone would leave every earned trophy earned
 * and only freeze the game. The time a console spends showing a trophy, which
 * the runs use, is not something the emulator reproduces either.
 *
 * Tick thread throughout: the trigger runs from the command ring and the rest
 * from rac3_on_tick.
 */

#define TR_IDLE   0
#define TR_DELETE 1   /* deleting the folder's files, a few a tick */
#define TR_ASK    2   /* the folder is done; ask the helper */
#define TR_WAIT   3   /* the request is out; watching the byte */

/*
 * Files deleted per tick, and the most unlink attempts one reset makes in all.
 * A trophy set is a few dozen files (UYA's is 42), so the delete is a handful of
 * ticks; the cap only ends a delete that keeps failing.
 */
#define TR_DELETE_PER_TICK 8
#define TR_DELETE_MAX      256

/*
 * How long qwark waits for the helper to pick a request up (the pad routine
 * runs every frame, so this only runs out on a game that is not running
 * frames), and for a reset it has started to finish: three seconds of waits,
 * the calls between them, and room to spare. Ticks of the 120 Hz loop.
 */
#define TR_SEEN_TICKS (5u * 120u)
#define TR_RUN_TICKS  (15u * 120u)

/*
 * The auto toggle's request asked again after the helper refused it because a
 * trophy thread of the game's was running, while the load lasts: this many
 * times, this many ticks apart.
 */
#define TR_RETRIES     8
#define TR_RETRY_TICKS 30

#define TR_DIR_MAX   64
#define TR_CHILD_MAX 128

/* "NPWR02347_00": nine characters, an underscore, two digits. */
#define TR_ID_LEN 12

static struct {
	u8   state;
	u8   request;     /* RAC3_TROPHY_NOW or RAC3_TROPHY_IN_LOAD */
	u8   retries;
	u16  deleted;
	u16  failed;
	u16  attempts;
	u32  ticks;
	char dir[TR_DIR_MAX];
} g_tr;

/* The helper's request byte, or 0 when this build's table has none. */
static u32 tr_request_addr(void)
{
	const struct sf_desc *d = sf_desc_for_game(GAME_RAC3);

	return d != NULL ? d->api_trophy : 0;
}

static void tr_idle(void)
{
	memset(&g_tr, 0, sizeof(g_tr));
}

void rac3_trophy_forget(void)
{
	if (g_tr.state == TR_DELETE)
		plat_log("trophies: the game went away while %s was being deleted (%d files gone)",
		         g_tr.dir, (int)g_tr.deleted);
	else if (g_tr.state != TR_IDLE)
		plat_log("trophies: the game went away before its trophies were re-read");
	tr_idle();
}

/* Whether `hay` (len bytes, not terminated) holds the string `needle`. */
static int tr_contains(const u8 *hay, u32 len, const char *needle)
{
	u32 n = qstrlen(needle);
	u32 i;

	if (n == 0 || n > len) return 0;
	for (i = 0; i + n <= len; i++) {
		if (memcmp(hay + i, needle, n) == 0) return 1;
	}
	return 0;
}

/*
 * Whether the file `name` in `dir` names `tag`, the <npcommid> element the set's
 * own configuration carries. The element is near the top of the file, after the
 * signature comment, so the first eight kilobytes are enough.
 */
static int tr_file_names(const char *dir, const char *name, const char *tag)
{
	char path[TR_CHILD_MAX];
	u8 *buf = qscratch();
	plat_file_t f;
	u32 got = 0;

	path[0] = 0;
	qstrcat(path, sizeof(path), dir);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), name);

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return 0;
	if (plat_file_read(f, buf, QSCRATCH_BYTES, &got) != 0) got = 0;
	plat_file_close(f);

	return tr_contains(buf, got, tag);
}

/*
 * Finds the folder, as the block comment above says. ST_OK with `*present` 1
 * and `*files` its file count, ST_OK with `*present` 0 when there is no such
 * folder to delete, ST_NOT_FOUND - logged - for everything that is not certain.
 */
static int tr_find_dir(char *dir, u32 cap, int *present, u32 *files)
{
	u8 id[RAC3_TROPHY_COMMID_LEN];
	char name[TR_ID_LEN + 1];
	char tag[TR_ID_LEN + 24];
	char num[12];
	u32 blk = 0, user, n, i;
	int is_dir = 0;
	plat_dir_t d;
	struct plat_dirent ent;

	*present = 0;
	*files = 0;

	if (mem_read_u32(RAC3_TROPHY_COMMID_PTR, &blk) != ST_OK ||
	    blk == 0 ||
	    mem_read(blk + RAC3_TROPHY_COMMID_OFF, id, sizeof(id)) != ST_OK) {
		plat_log("trophies: refused, the game has no trophy id set up yet");
		return ST_NOT_FOUND;
	}

	/* An SceNpCommunicationId: nine characters, a NUL, the number. */
	if (memcmp(id, "NPWR", 4) != 0 || id[9] != 0 || id[10] > 99) {
		plat_log("trophies: refused, the game's trophy id is not one qwark knows the shape of");
		return ST_NOT_FOUND;
	}
	for (i = 4; i < 9; i++) {
		if (id[i] < '0' || id[i] > '9') {
			plat_log("trophies: refused, the game's trophy id is not one qwark knows the shape of");
			return ST_NOT_FOUND;
		}
	}
	memcpy(name, id, 9);
	name[9]  = '_';
	name[10] = (char)('0' + id[10] / 10);
	name[11] = (char)('0' + id[10] % 10);
	name[12] = 0;

	/* The XMB's user number, which names the home folder: eight digits. */
	user = plat_user_id();
	if (user == 0 || user > 99999999u) {
		plat_log("trophies: refused, no user is signed in");
		return ST_NOT_FOUND;
	}
	n = qfmt_u32(num, sizeof(num), user);

	dir[0] = 0;
	qstrcat(dir, cap, "/dev_hdd0/home/");
	for (i = n; i < 8; i++) qstrcat(dir, cap, "0");
	qstrcat(dir, cap, num);
	if (!plat_path_exists(dir, &is_dir, NULL) || !is_dir) {
		plat_log("trophies: refused, there is no home folder %s", dir);
		return ST_NOT_FOUND;
	}

	qstrcat(dir, cap, "/trophy");
	if (!plat_path_exists(dir, &is_dir, NULL) || !is_dir) {
		plat_log("trophies: refused, there is no trophy folder %s", dir);
		return ST_NOT_FOUND;
	}

	qstrcat(dir, cap, "/");
	qstrcat(dir, cap, name);
	if (qstrlen(dir) + 1 >= cap) return ST_NOT_FOUND;

	if (!plat_path_exists(dir, &is_dir, NULL)) {
		plat_log("trophies: %s is not there, so there is nothing to delete", dir);
		return ST_OK;
	}
	if (!is_dir) {
		plat_log("trophies: refused, %s is not a folder", dir);
		return ST_NOT_FOUND;
	}

	/* The set's own configuration has to say it is this set. */
	tag[0] = 0;
	qstrcat(tag, sizeof(tag), "<npcommid>");
	qstrcat(tag, sizeof(tag), name);
	qstrcat(tag, sizeof(tag), "</npcommid>");
	if (!tr_file_names(dir, "TROPCONF.SFM", tag) && !tr_file_names(dir, "TROP.SFM", tag)) {
		plat_log("trophies: refused, neither TROPCONF.SFM nor TROP.SFM in %s names %s", dir, name);
		return ST_NOT_FOUND;
	}

	/* Files only: a trophy set has no folders, and qwark walks into none. */
	if (plat_dir_open(dir, &d) != 0) {
		plat_log("trophies: refused, %s cannot be read", dir);
		return ST_NOT_FOUND;
	}
	n = 0;
	is_dir = 0;
	while (plat_dir_next(&d, &ent) == 1) {
		if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;
		if (ent.is_dir) is_dir = 1;
		else n++;
	}
	plat_dir_close(&d);
	if (is_dir) {
		plat_log("trophies: refused, %s holds a folder, which no trophy set does", dir);
		return ST_NOT_FOUND;
	}

	*present = 1;
	*files = n;
	return ST_OK;
}

/*
 * The game's side of the checks the helper makes again on its own frame: the
 * trophy object is set up and none of its threads runs (BUSY otherwise, which is
 * what the auto toggle retries on), and the cache-fill site holds the game's
 * store or the nop qwark puts there (UNSUPPORTED otherwise: not the executable
 * this was written against).
 */
static int tr_game_ready(void)
{
	u32 obj = 0, busy = 0, word = 0;

	if (mem_read_u32(RAC3_TROPHY_FILL_SITE, &word) != ST_OK) return ST_IO_ERROR;
	if (word != RAC3_TROPHY_FILL_WORD && word != RAC3_TROPHY_FILL_NOP) {
		plat_log("trophies: refused, 0x985318 holds neither the game's store nor its nop");
		return ST_UNSUPPORTED;
	}

	if (mem_read_u32(RAC3_TROPHY_OBJ_PTR, &obj) != ST_OK) return ST_IO_ERROR;
	if (obj == 0) return ST_BUSY;
	if (mem_read_u32(obj + RAC3_TROPHY_THREAD, &busy) != ST_OK) return ST_IO_ERROR;
	return busy == 0 ? ST_OK : ST_BUSY;
}

/* Whether a planet load's transition is under way: mode 6, the request up. */
static int tr_in_load(void)
{
	u32 request = 0, mode = 0;

	if (mem_read_u32(RAC3_LOAD_PLANET, &request) != ST_OK) return 0;
	if (mem_read_u32(RAC3_GAME_STATE, &mode) != ST_OK) return 0;
	return request != 0 && mode == RAC3_GAME_STATE_LOAD;
}

int rac3_trophy_reset(u8 request)
{
	u32 addr = tr_request_addr();
	u32 files = 0;
	u8 byte = 0;
	int present = 0;
	int rc;

	if (!plat_can_patch_code()) {
		plat_log("trophies: refused, RPCS3 keeps its trophy data where qwark cannot reach it");
		return ST_UNSUPPORTED;
	}
	if (request != RAC3_TROPHY_NOW && request != RAC3_TROPHY_IN_LOAD) return ST_BAD_ARG;
	if (g_tr.state != TR_IDLE) return ST_BUSY;
	if (addr == 0) return ST_UNSUPPORTED;
	if (request == RAC3_TROPHY_IN_LOAD && !tr_in_load()) return ST_BUSY;

	rc = tr_game_ready();
	if (rc != ST_OK) return rc;

	rc = tr_find_dir(g_tr.dir, sizeof(g_tr.dir), &present, &files);
	if (rc != ST_OK) {
		tr_idle();
		return rc;
	}

	/* The helper carries the game's half, so it has to be in before anything goes. */
	rc = savefile_install();
	if (rc != ST_OK) {
		plat_log("trophies: refused, the savefile helper cannot be installed (%d)", rc);
		tr_idle();
		return rc;
	}
	if (mem_read_u8(addr, &byte) != ST_OK || byte != 0) {
		tr_idle();
		return byte != 0 ? ST_BUSY : ST_IO_ERROR;
	}

	g_tr.request = request;
	if (present) {
		g_tr.state = TR_DELETE;
		plat_log("trophies: resetting UYA's trophies, deleting the %d files of %s",
		         (int)files, g_tr.dir);
	} else {
		g_tr.state = TR_ASK;
	}
	return ST_OK;
}

int rac3_trophy_auto(void)
{
	int rc;

	/* A reset already under way is this load's. */
	if (g_tr.state != TR_IDLE) return 1;

	rc = rac3_trophy_reset(RAC3_TROPHY_IN_LOAD);
	return rc == ST_BUSY ? 0 : 1;
}

/* One tick of the delete: a handful of files out of the one folder. */
static void tr_delete_step(void)
{
	plat_dir_t d;
	struct plat_dirent ent;
	u32 tried = 0, gone = 0;
	int cut = 0;

	if (plat_dir_open(g_tr.dir, &d) != 0) {
		/* Gone already, or unreadable: there is nothing more to do here either way. */
		cut = 0;
	} else {
		while (plat_dir_next(&d, &ent) == 1) {
			char child[TR_CHILD_MAX];

			if (qstreq(ent.name, ".") || qstreq(ent.name, "..") || ent.is_dir) continue;
			if (tried >= TR_DELETE_PER_TICK || g_tr.attempts >= TR_DELETE_MAX) {
				cut = 1;
				break;
			}

			tried++;
			g_tr.attempts++;
			if (qstrlen(g_tr.dir) + 1 + qstrlen(ent.name) + 1 > sizeof(child)) {
				g_tr.failed++;
				continue;
			}
			child[0] = 0;
			qstrcat(child, sizeof(child), g_tr.dir);
			qstrcat(child, sizeof(child), "/");
			qstrcat(child, sizeof(child), ent.name);

			if (plat_file_unlink(child) == 0) {
				gone++;
				g_tr.deleted++;
			} else {
				g_tr.failed++;
				plat_log("trophies: could not delete %s", child);
			}
		}
		plat_dir_close(&d);
	}

	/*
	 * Another pass while this one deleted something or was cut short, so a
	 * directory read that skipped an entry under the deletes is caught; done
	 * when a whole pass deletes nothing, or the attempts run out.
	 */
	if ((cut || gone != 0) && g_tr.attempts < TR_DELETE_MAX) return;

	if (plat_dir_remove(g_tr.dir) == 0)
		plat_log("trophies: deleted %s, %d files", g_tr.dir, (int)g_tr.deleted);
	else if (plat_path_exists(g_tr.dir, NULL, NULL))
		plat_log("trophies: %s stays, %d files deleted and %d could not be",
		         g_tr.dir, (int)g_tr.deleted, (int)g_tr.failed);
	else
		plat_log("trophies: %s is gone, %d files deleted", g_tr.dir, (int)g_tr.deleted);

	g_tr.state = TR_ASK;
}

/* The folder is done with: ask the helper, once the cache fill is a nop. */
static void tr_ask(void)
{
	u32 addr = tr_request_addr();
	u32 word = 0;

	/*
	 * The auto toggle's reset freezes the game only inside the load it was
	 * asked for in; a load that is over by now leaves the game's half for the
	 * next one. The helper makes the same test on its own frame.
	 */
	if (g_tr.request == RAC3_TROPHY_IN_LOAD && !tr_in_load()) {
		plat_log("trophies: the load ended before the game could be asked; the next load into Veldin asks again");
		tr_idle();
		return;
	}

	if (mem_read_u32(RAC3_TROPHY_FILL_SITE, &word) != ST_OK) return;   /* next tick */
	if (word == RAC3_TROPHY_FILL_WORD) {
		if (mem_write_u32(RAC3_TROPHY_FILL_SITE, RAC3_TROPHY_FILL_NOP) != ST_OK) return;
	} else if (word != RAC3_TROPHY_FILL_NOP) {
		plat_log("trophies: 0x985318 changed under the reset, nothing asked of the game");
		tr_idle();
		return;
	}

	if (mem_write_u8(addr, g_tr.request) != ST_OK) return;   /* next tick */
	g_tr.state = TR_WAIT;
	g_tr.ticks = 0;
}

/* Watches the request byte until the helper has answered it. */
static void tr_wait(void)
{
	u32 addr = tr_request_addr();
	u8 byte = 0;

	g_tr.ticks++;
	if (mem_read_u8(addr, &byte) != ST_OK) {
		if (g_tr.ticks > TR_RUN_TICKS) tr_idle();
		return;
	}

	if (byte == 0) {
		plat_log("trophies: reset; the game re-read its trophies, and 10, 25 and 32 can be won again");
		tr_idle();
		return;
	}

	if (byte == RAC3_TROPHY_RUNNING) {
		if (g_tr.ticks > TR_RUN_TICKS) {
			plat_log("trophies: the game started the reset and has not finished it");
			tr_idle();
		}
		return;
	}

	if (byte == SAVEFILE_HELPER_REFUSED) {
		mem_write_u8(addr, 0);
		if (g_tr.request == RAC3_TROPHY_IN_LOAD && g_tr.retries < TR_RETRIES && tr_in_load()) {
			/* A trophy thread of the game's was running; it ends in moments. */
			g_tr.retries++;
			g_tr.state = TR_ASK;
			g_tr.ticks = 0;
			return;
		}
		plat_log("trophies: the game refused the reset: its trophies were not set up, a trophy thread of its own was running, or the load was over");
		tr_idle();
		return;
	}

	if (byte == g_tr.request) {
		if (g_tr.ticks > TR_SEEN_TICKS) {
			mem_write_u8(addr, 0);
			plat_log("trophies: the game never picked the reset up, and it is withdrawn");
			tr_idle();
		}
		return;
	}

	/* Something else wrote the byte: put it back and stop. */
	mem_write_u8(addr, 0);
	tr_idle();
}

void rac3_trophy_tick(void)
{
	switch (g_tr.state) {
	case TR_DELETE:
		tr_delete_step();
		break;
	case TR_ASK:
		/* A refused auto request waits a moment before it asks again. */
		if (g_tr.retries != 0 && g_tr.ticks++ < TR_RETRY_TICKS) break;
		tr_ask();
		break;
	case TR_WAIT:
		tr_wait();
		break;
	default:
		break;
	}
}

/* --------------------------------------------------------- weapon levels */

/*
 * Build 53: Setup NG+ manips, CC early setup, Untune bosses and Reset dropship
 * health are retired here, ids and all. Their writes are UYA's manip groups in
 * rac3.c; Untune bosses belongs to no group and is dropped.
 */

/* UYAUnlocks buttonUpgrade / buttonDowngrade. */
static int rac3_all_versions(int max)
{
	u8 i;

	for (i = 0; i < RAC3_UNLOCK_COUNT; i++) {
		const struct rac3_item *it = &rac3_items[i];

		if (it->levels <= 1) continue;

		if (max) {
			item_set_version(it, it->levels);
		} else {
			item_set_version(it, 1);
			/* Or it upgrades again on the next kill. */
			if (item_exp_addr(it) != 0) mem_write_u32(item_exp_addr(it), 0);
		}
	}

	return ST_OK;
}

/* ------------------------------------------------------------ the handlers */

int rac3_set_value(u8 id, u32 value)
{
	switch (id) {
	case R3_BOLTS:     return mem_write_u32(RAC3_BOLTS, value);
	case R3_CHALLENGE: return mem_write_u8(RAC3_CHALLENGE_MODE, (u8)value);
	case R3_HEALTH_XP: return mem_write_u32(RAC3_HEALTH_XP, value);
	case R3_HEALTH:    return mem_write_u32(RAC3_PLAYER_HEALTH, value);
	case R3_FILE_TIME: return mem_write_u32(RAC3_FILE_TIME, value);

	case R3_ARMOUR: {
		/* rac3.cs SetArmor writes the index as a big-endian u16. */
		u8 halfword[2];
		be16_put(halfword, (u16)value);
		return mem_write(RAC3_CURRENT_ARMOR, halfword, 2);
	}

	case R3_SHIP_COLOUR:
		return mem_write_u8(RAC3_SHIP_COLOUR, (u8)value);

	case R3_QE_OFFSET: {
		/*
		 * qeTextBox took a signed 16-bit number and wrote it big-endian. The
		 * wire carries a u32, so the low sixteen bits are the offset.
		 */
		u8 halfword[2];
		be16_put(halfword, (u16)value);
		return mem_write(RAC3_QE_OFFSET, halfword, 2);
	}

	case R3_CB_PRIMARY_FRONT:
		return classic_cb_write(RAC3_CB_PRIMARY_FRONT, CLASSIC_CB_ALPHA_FRONT, value);
	case R3_CB_PRIMARY_BACK:
		return classic_cb_write(RAC3_CB_PRIMARY_BACK, CLASSIC_CB_ALPHA_BACK, value);
	case R3_CB_TINT_FRONT: {
		/* Tint front and back mirror each other, so one control writes both words. */
		int rc = classic_cb_write(RAC3_CB_TINT_FRONT, CLASSIC_CB_ALPHA_FRONT, value);
		if (rc != ST_OK) return rc;
		return classic_cb_write(RAC3_CB_TINT_BACK, CLASSIC_CB_ALPHA_FRONT, value);
	}

	default:           return ST_NOT_FOUND;
	}
}

int rac3_trigger(u8 id)
{
	switch (id) {
	case R3_DIE:             return classic_die_set_z(RAC3_COORDS);

	case R3_VENDOR_QE:
		/* vendorQeEnableButton wrote the two bytes 00 17. */
		return mem_write(RAC3_QE_OFFSET, rac3_vendor_qe, sizeof(rac3_vendor_qe));

	case R3_RESET_TROPHIES:  return rac3_trophy_reset(RAC3_TROPHY_NOW);

	case R3_UNLOCK_SKILL:    return mem_write_fill(RAC3_SKILL_POINTS, 1, 30);
	case R3_RESET_SKILL:     return mem_write_zeros(RAC3_SKILL_POINTS, 30);
	case R3_UNLOCK_TITANIUM: return mem_write_fill(RAC3_TITANIUM_BOLTS, 1, 128);
	case R3_RESET_TITANIUM:  return mem_write_zeros(RAC3_TITANIUM_BOLTS, 128);

	case R3_UPGRADE_ALL:     return rac3_all_versions(1);
	case R3_DOWNGRADE_ALL:   return rac3_all_versions(0);

	case R3_SET_ASIDE:       return savefile_set_aside();
	case R3_LOAD_ASIDE:      return rac3_load_setaside();

	default:                 return ST_NOT_FOUND;
	}
}
