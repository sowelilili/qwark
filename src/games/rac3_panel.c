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

/*
 * The console menu's MAX for an unlock row (struct game_menu, unlock_top): the
 * weapon's own last version, where it stops short of the field's game-wide 8
 * (the R3YNO's v5), else 0, the set's own.
 */
u32 rac3_unlock_levels(u8 id)
{
	int row = rac3_row_for_id(id);

	if (row < 0 || rac3_items[row].levels >= RAC3_MAX_LEVELS) return 0;
	return rac3_items[row].levels;
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

/* --------------------------------------------------------- titanium bolts */

/*
 * Build 53. The forty titanium bolts, each with the planet it is collected on
 * and its byte in the table (rac3.h, RAC3_TITANIUM_BOLTS: eight bytes per
 * planet id), given as the slot it is in and its index there. They are the
 * community address list's NTSC PS2 column, one row per bolt in the list's
 * order, less that table's base 0x201BBB18; the column lays the bolts out by
 * planet id and index exactly as this executable does. Holostar's third bolt
 * (slot 13, index 3) is byte 0x6B, which 0x1DD1AC reads, and its first 0x69,
 * read at 0x1DD344; Phoenix's first (3, 0) is byte 0x18, read at 0x640470 and
 * 0x642320; and the game's own per-planet totals (0xC3FC58, an s32 per id,
 * which its map adds up with each planet's mission levels at 0x2CFE68) are
 * these rows counted, forty in all.
 *
 * The five vid comic bolts are kept in Starship Phoenix's slot, bytes 2 to 6,
 * in comic order 1, 4, 2, 3, 5, which is the order of their planet ids 31 to
 * 35, and the game counts them as Phoenix's seven. A bolt here belongs to the
 * level it is found in, so each is listed under its comic: a Phoenix load
 * takes back Phoenix's own two, and a comic's load the comic's one. The
 * mission levels (Aquatos's sewers, the Metropolis rangers mission) have slots
 * of their own and are listed under themselves. From the NPEA00387 executable;
 * not yet run on hardware.
 */
struct rac3_ti_bolt {
	u8 planet;
	u8 byte;     /* offset into RAC3_TITANIUM_BOLTS */
};

/* A bolt's byte: its slot's planet id, and its index within the slot. */
#define TI_AT(slot, index) ((u8)((slot) * RAC3_TI_STRIDE + (index)))

static const struct rac3_ti_bolt rac3_ti_bolts[] = {
	{  2, TI_AT(2, 1) },  {  2, TI_AT(2, 2) },                        /* Florana */
	{  3, TI_AT(3, 0) },  {  3, TI_AT(3, 1) },                        /* Starship Phoenix */
	{ 31, TI_AT(3, 2) },  { 32, TI_AT(3, 3) },  { 33, TI_AT(3, 4) },  /* Vid Comics 1, 4, 2 */
	{ 34, TI_AT(3, 5) },  { 35, TI_AT(3, 6) },                        /* Vid Comics 3, 5 */
	{  4, TI_AT(4, 1) },  {  4, TI_AT(4, 2) },  {  4, TI_AT(4, 3) },  /* Marcadia */
	{  5, TI_AT(5, 1) },  {  5, TI_AT(5, 2) },                        /* Daxx */
	{  7, TI_AT(7, 1) },  {  7, TI_AT(7, 2) },                        /* Annihilation Nation */
	{  8, TI_AT(8, 1) },  {  8, TI_AT(8, 2) },  {  8, TI_AT(8, 3) },  /* Aquatos */
	{  9, TI_AT(9, 1) },  {  9, TI_AT(9, 2) },                        /* Tyhrranosis */
	{ 10, TI_AT(10, 1) }, { 10, TI_AT(10, 2) },                       /* Zeldrin Starport */
	{ 11, TI_AT(11, 1) }, { 11, TI_AT(11, 2) },                       /* Obani Gemini */
	{ 13, TI_AT(13, 1) }, { 13, TI_AT(13, 2) }, { 13, TI_AT(13, 3) }, /* Holostar */
	{ 14, TI_AT(14, 1) }, { 14, TI_AT(14, 2) },                       /* Koros */
	{ 16, TI_AT(16, 1) }, { 16, TI_AT(16, 2) },                       /* Metropolis */
	{ 17, TI_AT(17, 1) },                                             /* Crash Site */
	{ 18, TI_AT(18, 1) }, { 18, TI_AT(18, 2) },                       /* Aridia */
	{ 19, TI_AT(19, 1) },                                             /* Qwark's Hideout */
	{ 22, TI_AT(22, 1) },                                             /* Command Center */
	{ 26, TI_AT(26, 1) },                                             /* Metropolis Rangers */
	{ 28, TI_AT(28, 1) }, { 28, TI_AT(28, 2) }                        /* Aquatos Sewers */
};

#undef TI_AT

#define RAC3_TI_BOLTS ((u8)(sizeof(rac3_ti_bolts) / sizeof(rac3_ti_bolts[0])))

/*
 * "Reset special bolts" for one planet: every bolt collected there back to not
 * collected, as RaC1's gold bolt reset does for its planet. A planet with none
 * has nothing to write and that is not an error.
 */
static int rac3_reset_titanium(u8 planet)
{
	u8 i;
	int rc;

	for (i = 0; i < RAC3_TI_BOLTS; i++) {
		if (rac3_ti_bolts[i].planet != planet) continue;
		rc = mem_write_u8(RAC3_TITANIUM_BOLTS + rac3_ti_bolts[i].byte, 0);
		if (rc != ST_OK) return rc;
	}
	return ST_OK;
}

/*
 * "Unlock all titanium bolts": the table exactly as a file with every bolt
 * collected holds it, the forty bytes at 1 and every other byte 0, so the
 * game's per-planet counts read 2 of 2 rather than 8 of 2. One write.
 */
static int rac3_unlock_titanium(void)
{
	u8 table[RAC3_TI_LEN];
	u8 i;

	memset(table, 0, sizeof(table));
	for (i = 0; i < RAC3_TI_BOLTS; i++) table[rac3_ti_bolts[i].byte] = 1;
	return mem_write(RAC3_TITANIUM_BOLTS, table, sizeof(table));
}

/* ---------------------------------------------------------------- planets */

int rac3_planet_load(u8 planet, u8 flags)
{
	int was, rc;

	/* Index 0 is the placeholder that keeps the list aligned with the ids. */
	if (planet == 0 || planet >= rac3_planet_count()) return ST_BAD_ARG;

	/*
	 * Fast loads go on first, behind the checkbox, and rac3.c's watcher puts
	 * the checkbox's setting back when the load is over, as RaC2's planet load
	 * does. Every planet: rac3.cs LoadPlanetSafe left Aquatos out, and the
	 * data-word fast loads did hang a trip that owes one of Aquatos's story
	 * scenes; the patch lets the scene play (rac3.h, RAC3_FASTLOAD_SITE).
	 *
	 * Fast loads are a code patch, so a platform that refuses one (RPCS3)
	 * loads at the game's own pace rather than not at all, unless the code
	 * switches are in, when forcing them is a write of their flag byte.
	 *
	 * A request that fails takes back its own force and nobody else's, as
	 * RaC1's set-aside load does: one still outstanding from a load before
	 * stays.
	 */
	was = rac3_fastload_forced();
	rc = rac3_fastload_force();
	if (rc == ST_UNSUPPORTED && !plat_can_patch_code()) rc = ST_OK;
	if (rc != ST_OK && rc != ST_NOT_INGAME) return rc;

	rc = classic_planet_request(RAC3_LOAD_PLANET, planet);
	if (rc != ST_OK) {
		if (!was) rac3_fastload_restore();
		return rc;
	}

	if (flags & PLANET_FLAG_RESET_LEVELFLAGS) {
		rc = rac3_levelflags_reset(planet);
		if (rc != ST_OK) return rc;
	}

	/*
	 * Build 53: the titanium bolts are UYA's special bolts, and bit1 takes the
	 * destination's back the way RaC1's takes back its gold bolts - after the
	 * request has gone out, with the level flags, and only when it has. Until
	 * this build the bit did nothing here. A data write, so it works under
	 * RPCS3 with or without the code switches.
	 */
	if (flags & PLANET_FLAG_RESET_BOLTS) {
		rc = rac3_reset_titanium(planet);
		if (rc != ST_OK) return rc;
	}

	return ST_OK;
}

/* --------------------------------------------------------------- savefile */

/*
 * Both requests go through src/core/savefile.c, which installs the helper if
 * this process has not had it yet.
 */
int rac3_load_setaside(void)
{
	/*
	 * Build 53. A set-aside load is fast too, as RaC2's is: fast loads go on
	 * behind the checkbox before the request, and the end of the load puts the
	 * checkbox's setting back (rac3.c, rac3_fastload_watch). A load the game
	 * will not take is refused before anything is done for it - the install,
	 * and above all the force, which nothing would put back until the next load
	 * the player makes; one the helper drops later, on the game's own frame, is
	 * caught by rac3_on_tick.
	 *
	 * Under RPCS3 the helper and the code switches are separate patches: with
	 * the switches in, the force is a write of their flag byte; without them the
	 * load goes ahead at the game's own pace rather than being refused.
	 *
	 * A request that fails as it goes out takes back its own force, as RaC1's
	 * does (rac1_panel.c, rac1_load_setaside), and leaves one an earlier load
	 * still has outstanding.
	 */
	int was, rc = savefile_load_ready();

	if (rc != ST_OK) return rc;

	rc = savefile_install();
	if (rc != ST_OK) return rc;

	was = rac3_fastload_forced();
	rc = rac3_fastload_force();
	if (rc == ST_UNSUPPORTED && !plat_can_patch_code()) rc = ST_OK;
	if (rc != ST_OK && rc != ST_NOT_INGAME) return rc;

	rc = savefile_load_aside();
	if (rc != ST_OK && !was) rac3_fastload_restore();
	return rc;
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
 * mod loaded. Two halves:
 *
 *   1. The console's trophy data for this game goes. The PS3 keeps a user's
 *      unlocks of a trophy set in /dev_hdd0/home/<user>/trophy/<NP comm id>
 *      (TROPUSR.DAT among the rest), and sceNpTrophyUnlockTrophy on a trophy
 *      that file already has unlocked answers SCE_NP_TROPHY_ERROR_ALREADY_
 *      UNLOCKED and shows nothing: no amount of re-registering in the game makes
 *      an earned trophy pop again while that file is there. The old button
 *      deleted the folder over Ratchetron's DeleteDirectory before it wrote its
 *      request (RAC3Form.cs DeleteTrophyFolder, commit f987a1c, the same day
 *      Ratchetron's delete command went in for it). qwark deletes it itself,
 *      but since the console's log of the third hardware test not first: the
 *      helper's thread destroys the game's context, says so in the request byte
 *      (6), and qwark deletes the folder then and answers 7, so the console
 *      never sees a registered set's files go from under a live context.
 *   2. The game forgets and re-reads its trophies: the savefile helper's reset
 *      (sf_trophy_reset, sf_rac3.h) destroys the game's trophy context and
 *      registers it again - which installs the trophy set afresh, nothing
 *      unlocked - and leaves the game's own cache saying that trophies 10, 25
 *      and 32 are still to be won and every other one is won, exactly as
 *      rc3-save left it. Up to the first hardware test it did that on the
 *      game's main thread with rc3-save's three one-second sleeps, and the game
 *      froze for three seconds. The helper now takes the request on the main
 *      thread and runs the calls on a thread of its own, so the game goes on
 *      while it runs: a few seconds, most of them the console installing the
 *      set. It registers as the game's boot does on a console with no data for
 *      the set - once, a second after the destroy and the delete - making the
 *      registration's calls itself rather than through the game's register
 *      thread, checks that it took, and makes it again when it did not, up to
 *      three times; the first after the delete fails on every console log so
 *      far, and the second installs the set. It leaves out the game's half
 *      minute of reading back the game and trophy info, which the reset needs
 *      none of (sf_rac3.h has the why), so the trophies are back about
 *      thirteen seconds after the request. Each reset logs every step as it
 *      happens and every call's answer, and UYA's readout RAC3_RO_TROPHIES
 *      says when the trophies can be won again (rac3_trophy_status): until
 *      then the game awards nothing.
 *
 * The folder is found, not named. Its name is the communication id the running
 * game registers under, read out of the game (RAC3_TROPHY_COMMID_PTR); its
 * parent is the home folder of the user signed in now (plat_user_id, the XMB's
 * own number), and it counts as this game's only if its TROPCONF.SFM or TROP.SFM
 * names that same id. Anything else refuses the whole reset with NOT_FOUND
 * before a byte is deleted: no id, a malformed id, no user, no such home or
 * trophy folder, a folder whose files name another set, or a folder holding
 * folders of its own. A folder that is not there at all is a set with nothing
 * to delete, and the reset goes on to the game's half; so is an empty one,
 * which only the folder itself is deleted of. Only the files directly in that
 * one folder are deleted, a few a tick, then the folder; nothing is walked into,
 * and every step is logged.
 *
 * The folder is found and checked when the reset is asked for, before the game
 * is asked anything; it is deleted when the helper's thread says the context is
 * gone. The two files that say whose set the folder is, TROPCONF.SFM and
 * TROP.SFM, go last, and the first file that will not go stops the delete
 * there. A folder left half deleted keeps what identifies it, so the next reset
 * finds it again and finishes the job, where deleting in directory order could
 * leave the unlock record (TROPUSR.DAT) with nothing to say it was UYA's and
 * every later reset refused. The game's context is down by then, so the thread
 * is told to go on all the same and registers the game again over what is
 * left: the reset does not leave the game without its trophies.
 * FEATURE_TRIGGER has long answered OK by then, so a delete that stops says so
 * on the console's screen (plat_notify) as well as in the log; so does a reset
 * whose game half did not end with the game's trophies back, one that does not
 * end at all, and an auto reset whose load into Veldin ended before the game
 * could be asked: each of those leaves a run whose trophies will not pop. A
 * reset that ends well says nothing on the screen. Build 53 showed "Trophies
 * reset: ready to win (N s)" then, which helped the hardware tests and would
 * only distract a runner mid-attempt; the readout carries it now, for a client
 * to show beside the button, with where a reset under way has got to and why
 * the last one failed (rac3_trophy_status).
 *
 * The nop qwark writes over the cache fill (0x985318) is never put back, and it
 * is not one of PATCH_LIST's patches: it is part of the reset, as it was the
 * rc3-save mod's third patch word. Nothing but a registration runs that store
 * (0x9850E0, called by the game's register thread 0x9856F8 and by the reset's
 * own registration), and nothing registers but the game's boot (0x15434, the
 * only call to 0x984FA0) and this reset; a boot loads the executable afresh,
 * word and all. So the nop changes nothing but the registrations the reset
 * itself makes.
 *
 * Everything here is refused under RPCS3 (UNSUPPORTED). The emulator keeps a
 * trophy set's data in its own dev_hdd0, which qwark-rpcs3 cannot reach - its
 * file ops are rooted in a folder of its own - and nothing in qwark touches
 * RPCS3's files; the game's half alone would leave every earned trophy earned
 * and change nothing a run can see. The time a console spends showing a
 * trophy, which the runs use, is not something the emulator reproduces either.
 *
 * Tick thread throughout: the trigger runs from the command ring and the rest
 * from rac3_on_tick. Everything the trigger can refuse for a while (BUSY) is
 * checked before the folder is looked for, so the auto toggle, which asks again
 * on every tick of the load while it hears BUSY, costs a few memory reads a
 * tick and not a directory listing.
 */

#define TR_IDLE   0
#define TR_DELETE 1   /* the context is down: deleting the folder, a few files a tick */
#define TR_ASK    2   /* ask the helper */
#define TR_WAIT   3   /* the request is out; watching the byte */

/*
 * Files deleted per tick, and the most unlink attempts one reset makes in all.
 * A trophy set is a few dozen files (UYA's is 42), so the delete is a handful of
 * ticks; the cap only ends a delete on a folder that keeps showing files.
 */
#define TR_DELETE_PER_TICK 8
#define TR_DELETE_MAX      256

/*
 * How long qwark waits for the helper to pick a request up (the pad routine
 * runs every frame, so this only runs out on a game that is not running
 * frames), and for a reset it has started to finish. The helper's thread makes
 * the registration's calls itself, and a call the console does not answer
 * holds it for as long as that lasts: qwark logs where it is every
 * TR_PROGRESS_TICKS and lets it go after ten minutes, saying so on the screen.
 * Ticks of the 120 Hz loop.
 */
#define TR_SEEN_TICKS     (5u * 120u)
#define TR_RUN_TICKS      (600u * 120u)
#define TR_PROGRESS_TICKS (30u * 120u)

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
	u8   conf;        /* TR_DELETE: 1 once only the two .SFM files are left */
	u8   present;     /* there is a folder to delete when the context is down */
	u8   delete_failed;
	u8   seen_tries;  /* the helper's step qwark last logged */
	u8   seen_phase;
	u8   registering; /* the folder is done with: the helper's thread registers */
	u16  deleted;
	u16  failed;
	u16  attempts;
	u32  ticks;
	u32  elapsed;     /* ticks since the request went out */
	char dir[TR_DIR_MAX];
} g_tr;

/*
 * Revision 1.21: how the last reset ended, for RAC3_RO_TROPHIES once g_tr is
 * idle again. A boot forgets it (rac3_trophy_forget) but for `seq`, the count
 * of resets that have ended since the module started, so that a client can
 * tell a new result from the one it saw before, the same words or not.
 */
static struct {
	u8  state;    /* RAC3_TS_NONE, READY or FAILED */
	u8  reason;   /* FAILED: RAC3_TS_DELETE_STOPPED.. */
	u8  arg;      /* REFUSED: the status the reset was refused with */
	u8  seq;
	u16 secs;
} g_trs;

/* The helper's request byte, or 0 when this build's table has none. */
static u32 tr_request_addr(void)
{
	const struct sf_desc *d = sf_desc_for_game(GAME_RAC3);

	return d != NULL ? d->api_trophy : 0;
}

/*
 * The helper's busy byte, the one after the request byte (SF_TROPHY_BUSY in
 * sf_rac3.h): 1 from the frame the helper takes a request until its thread has
 * finished, whatever the request byte says meanwhile.
 */
static u32 tr_busy_addr(void)
{
	return tr_request_addr() + RAC3_TROPHY_BUSY_OFF;
}

/* What the helper would say to a new request, read before anything is looked for. */
static int tr_helper_idle(int *stale)
{
	u8 byte = 0, busy = 0;

	*stale = 0;
	if (mem_read_u8(tr_request_addr(), &byte) != ST_OK) return ST_IO_ERROR;
	if (mem_read_u8(tr_busy_addr(), &busy) != ST_OK) return ST_IO_ERROR;

	/* The helper's thread is still at it: the reset before this one is not over. */
	if (busy != 0) return ST_BUSY;

	/*
	 * With qwark idle and no thread of the helper's running, whatever the byte
	 * holds is over: an answer nobody read (a 4, or a refusal that landed just
	 * as qwark withdrew a request the game had not picked up), or a 3 qwark
	 * stopped watching. It goes back to 0 once the helper is known to be in;
	 * until then the install zeroes it.
	 */
	*stale = (byte != 0);
	return ST_OK;
}

/* The trophy files that say whose set a folder is; they are deleted last. */
static int tr_is_conf(const char *name)
{
	return qstreq(name, "TROPCONF.SFM") || qstreq(name, "TROP.SFM");
}

static void tr_idle(void)
{
	memset(&g_tr, 0, sizeof(g_tr));
}

/* Whole seconds since the request went out, rounded, as the log's milliseconds are. */
static u32 tr_secs(void)
{
	return (g_tr.elapsed + 60u) / 120u;
}

/*
 * The reset is over, one way or the other: what RAC3_RO_TROPHIES says until
 * the next one starts or the game boots. Every place that ends a reset that
 * was under way, or refuses one that would otherwise have started, calls this
 * before it lets g_tr go.
 */
static void tr_end(u8 state, u8 reason, u8 arg)
{
	u32 secs = tr_secs();

	g_trs.state  = state;
	g_trs.reason = reason;
	g_trs.arg    = arg;
	g_trs.secs   = (u16)(secs > 0xFFFu ? 0xFFFu : secs);
	g_trs.seq++;
}

void rac3_trophy_forget(void)
{
	if (g_tr.state == TR_DELETE)
		plat_log("trophies: the game went away while %s was being deleted (%d files gone)",
		         g_tr.dir, (int)g_tr.deleted);
	else if (g_tr.state != TR_IDLE)
		plat_log("trophies: the game went away before its trophies were re-read");
	tr_idle();

	/* A new boot has had no reset yet; the count goes on. */
	g_trs.state  = RAC3_TS_NONE;
	g_trs.reason = 0;
	g_trs.arg    = 0;
	g_trs.secs   = 0;
}

/*
 * Revision 1.21: the RAC3_RO_TROPHIES word (rac3.h has the layout, PROTOCOL.md
 * 5.14 the contract). A reset under way is RUNNING at the step g_tr is at: the
 * game asked and letting go of its trophies, then the delete, then the helper's
 * registrations, whose count is the one tr_log_progress last read out of the
 * helper's diagnostics (0 in the second's wait before the first). Otherwise it
 * is the last reset's result this boot, or NONE. Read by rac3_hot_decode on
 * every tick, so it is a few fields and no memory read.
 */
u32 rac3_trophy_status(void)
{
	u32 state, detail, arg, secs;

	if (g_tr.state != TR_IDLE) {
		state = RAC3_TS_RUNNING;
		arg = 0;
		if (g_tr.state == TR_DELETE) {
			detail = RAC3_TS_DELETING;
		} else if (g_tr.registering) {
			detail = RAC3_TS_REGISTERING;
			arg = g_tr.seen_tries;
		} else {
			detail = RAC3_TS_STARTING;
		}
		secs = tr_secs();
	} else {
		state  = g_trs.state;
		detail = g_trs.reason;
		arg    = g_trs.arg;
		secs   = g_trs.secs;
	}

	if (arg > 15u) arg = 15u;
	if (secs > 0xFFFu) secs = 0xFFFu;
	return (state << 28) | ((detail & 15u) << 24) | (arg << 20) | (secs << 8) | g_trs.seq;
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

	/*
	 * An empty folder has nothing to identify and nothing to lose: a delete
	 * that went as far as the last file and could not take the folder leaves
	 * one, and it would otherwise refuse every reset after it. Only the folder
	 * goes.
	 */
	if (plat_dir_open(dir, &d) != 0) {
		plat_log("trophies: refused, %s cannot be read", dir);
		return ST_NOT_FOUND;
	}
	n = 0;
	while (plat_dir_next(&d, &ent) == 1) {
		if (!qstreq(ent.name, ".") && !qstreq(ent.name, "..")) n++;
	}
	plat_dir_close(&d);
	if (n == 0) {
		*present = 1;
		return ST_OK;
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

static int tr_start(u8 request)
{
	u32 addr = tr_request_addr();
	u32 files = 0;
	int present = 0;
	int stale = 0;
	int rc;

	if (!plat_can_patch_code()) {
		plat_log("trophies: refused, RPCS3 keeps its trophy data where qwark cannot reach it");
		return ST_UNSUPPORTED;
	}
	if (request != RAC3_TROPHY_NOW && request != RAC3_TROPHY_IN_LOAD) return ST_BAD_ARG;
	if (g_tr.state != TR_IDLE) return ST_BUSY;
	if (addr == 0) return ST_UNSUPPORTED;
	if (!savefile_enabled()) {
		plat_log("trophies: refused, the savefile helper is switched off (savefile_helper = 0)");
		return ST_UNSUPPORTED;
	}
	if (request == RAC3_TROPHY_IN_LOAD && !tr_in_load()) return ST_BUSY;

	rc = tr_game_ready();
	if (rc != ST_OK) return rc;
	rc = tr_helper_idle(&stale);
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
	if (stale && mem_write_u8(addr, 0) != ST_OK) {
		tr_idle();
		return ST_IO_ERROR;
	}

	g_tr.request = request;
	g_tr.present = (u8)(present != 0);
	g_tr.state = TR_ASK;
	if (present)
		plat_log("trophies: resetting UYA's trophies; the %d files of %s go once the game "
		         "has let go of them", (int)files, g_tr.dir);
	else
		plat_log("trophies: resetting UYA's trophies; %s is not there to delete", g_tr.dir);
	return ST_OK;
}

int rac3_trophy_reset(u8 request)
{
	int rc = tr_start(request);

	/*
	 * Revision 1.21. A refusal that leaves this reset undone is the newest
	 * result, and RAC3_RO_TROPHIES says so with the status it was answered
	 * with: the button's status code says it once, but the auto toggle's
	 * refusal, inside a Veldin load, has nobody to say it to, and a READY left
	 * over from the last run would say the trophies were ready when they are
	 * not. BUSY is not one: a reset under way, or a game not ready yet, which
	 * the auto toggle asks again about on the next tick. Under RPCS3 there is
	 * no reset to report on, and the readout stays NONE.
	 */
	if (rc != ST_OK && rc != ST_BUSY && rc != ST_BAD_ARG && plat_can_patch_code())
		tr_end(RAC3_TS_FAILED, RAC3_TS_REFUSED, (u8)rc);
	return rc;
}

int rac3_trophy_auto(void)
{
	int rc;

	/* A reset already under way is this load's. */
	if (g_tr.state != TR_IDLE) return 1;

	rc = rac3_trophy_reset(RAC3_TROPHY_IN_LOAD);
	return rc == ST_BUSY ? 0 : 1;
}

/* The folder is done with, one way or the other: the helper's thread goes on. */
static void tr_delete_done(void)
{
	mem_write_u8(tr_request_addr(), RAC3_TROPHY_DELETED);
	g_tr.state = TR_WAIT;
	g_tr.registering = 1;
}

/*
 * One tick of the delete, while the helper's thread waits with the game's
 * context down: a handful of files out of the one folder, everything but the
 * two .SFM files first and those two after, then the folder.
 *
 * A pass that deleted something, or stopped at the tick's share, is followed by
 * another, so a directory read that skipped an entry under the deletes is
 * caught; a stage is over when a whole pass finds nothing left for it. The first
 * file that will not go ends the delete there and then: nothing is tried twice,
 * and the files after it are not reached.
 */
static void tr_delete_step(void)
{
	plat_dir_t d;
	struct plat_dirent ent;
	u32 tried = 0;
	int cut = 0;

	if (plat_dir_open(g_tr.dir, &d) == 0) {
		while (plat_dir_next(&d, &ent) == 1) {
			char child[TR_CHILD_MAX];

			if (qstreq(ent.name, ".") || qstreq(ent.name, "..") || ent.is_dir) continue;
			if (!g_tr.conf && tr_is_conf(ent.name)) continue;
			if (tried >= TR_DELETE_PER_TICK || g_tr.attempts >= TR_DELETE_MAX) {
				cut = 1;
				break;
			}

			tried++;
			g_tr.attempts++;
			child[0] = 0;
			if (qstrlen(g_tr.dir) + 1 + qstrlen(ent.name) + 1 <= sizeof(child)) {
				qstrcat(child, sizeof(child), g_tr.dir);
				qstrcat(child, sizeof(child), "/");
				qstrcat(child, sizeof(child), ent.name);
			}

			if (child[0] != 0 && plat_file_unlink(child) == 0) {
				g_tr.deleted++;
			} else {
				g_tr.failed++;
				plat_log("trophies: could not delete %s/%s", g_tr.dir, ent.name);
				break;
			}
		}
		plat_dir_close(&d);
	}
	/* A folder that cannot be read now has gone, or will show in the remove below. */

	if (g_tr.failed != 0 || (cut && g_tr.attempts >= TR_DELETE_MAX)) {
		plat_log("trophies: the delete stopped with %d files of %s deleted; the game registers "
		         "again over what is left, and the next reset finds the folder again",
		         (int)g_tr.deleted, g_tr.dir);
		plat_notify("Trophy reset stopped: a trophy file could not be deleted");
		g_tr.delete_failed = 1;
		tr_delete_done();
		return;
	}
	if (cut || tried != 0) return;
	if (!g_tr.conf) {
		g_tr.conf = 1;
		return;
	}

	/*
	 * Every file is gone, so the set is: a folder that will not go after them is
	 * empty, which the console fills again as the game registers, and which the
	 * next reset takes as a set with nothing in it.
	 */
	if (plat_dir_remove(g_tr.dir) == 0)
		plat_log("trophies: deleted %s, %d files", g_tr.dir, (int)g_tr.deleted);
	else if (plat_path_exists(g_tr.dir, NULL, NULL))
		plat_log("trophies: %d files of %s deleted, and the empty folder stays",
		         (int)g_tr.deleted, g_tr.dir);
	else
		plat_log("trophies: %s is gone, %d files deleted", g_tr.dir, (int)g_tr.deleted);

	tr_delete_done();
}

/* Ask the helper, once the cache fill is a nop. */
static void tr_ask(void)
{
	u32 addr = tr_request_addr();
	u32 word = 0;

	/*
	 * The auto toggle's reset starts only inside the load it was asked for in;
	 * a load that is over by now leaves the whole reset, folder and all, for the
	 * next one. The helper makes the same test on its own frame.
	 */
	if (g_tr.request == RAC3_TROPHY_IN_LOAD && !tr_in_load()) {
		plat_log("trophies: the load ended before the game could be asked; the next load into Veldin asks again");
		plat_notify("Trophy reset: the Veldin load ended first; the next Veldin load finishes it");
		tr_end(RAC3_TS_FAILED, RAC3_TS_LOAD_ENDED, 0);
		tr_idle();
		return;
	}

	if (mem_read_u32(RAC3_TROPHY_FILL_SITE, &word) != ST_OK) return;   /* next tick */
	if (word == RAC3_TROPHY_FILL_WORD) {
		if (mem_write_u32(RAC3_TROPHY_FILL_SITE, RAC3_TROPHY_FILL_NOP) != ST_OK) return;
	} else if (word != RAC3_TROPHY_FILL_NOP) {
		plat_log("trophies: 0x985318 changed under the reset, nothing asked of the game");
		tr_end(RAC3_TS_FAILED, RAC3_TS_STOPPED, 0);
		tr_idle();
		return;
	}

	if (mem_write_u8(addr, g_tr.request) != ST_OK) return;   /* next tick */
	g_tr.state = TR_WAIT;
	g_tr.ticks = 0;
	g_tr.elapsed = 0;
	g_tr.seen_tries = 0;
	g_tr.seen_phase = 0;
}

/* What the helper's thread is doing, by RAC3_TROPHY_PHASE_* (1..8). */
static const char *tr_phase_name(u8 phase)
{
	static const char * const names[9] = {
		"starting",
		"destroying the game's trophy context",
		"waiting for qwark to delete the trophy data",
		"waiting a second",
		"making a context and a handle",
		"in sceNpTrophyGetRequiredDiskSpace",
		"in sceNpTrophyRegisterContext",
		"reading the game's and each trophy's info (the build before)",
		"done"
	};

	return names[phase <= RAC3_TROPHY_PHASE_DONE ? phase : 0];
}

/* The helper's diagnostics block (rac3.h), or 0 when it cannot be read. */
static int tr_read_diag(u8 *diag)
{
	return mem_read(tr_request_addr() + RAC3_TROPHY_DIAG_OFF, diag, RAC3_TROPHY_DIAG_LEN) == ST_OK;
}

/*
 * Logs the helper's step when it has moved on since the last tick qwark looked,
 * with how long the reset has run, and where it is every TR_PROGRESS_TICKS.
 */
static void tr_log_progress(void)
{
	u8 diag[RAC3_TROPHY_DIAG_LEN];

	if (!tr_read_diag(diag)) return;
	if (diag[RAC3_TD_TRIES] == g_tr.seen_tries && diag[RAC3_TD_PHASE] == g_tr.seen_phase &&
	    (g_tr.elapsed == 0 || g_tr.elapsed % TR_PROGRESS_TICKS != 0)) return;

	g_tr.seen_tries = diag[RAC3_TD_TRIES];
	g_tr.seen_phase = diag[RAC3_TD_PHASE];
	if (g_tr.seen_tries == 0)
		plat_log("trophies: %d ms: %s", (int)(g_tr.elapsed * 25u / 3u),
		         tr_phase_name(g_tr.seen_phase));
	else
		plat_log("trophies: %d ms: registration %d: %s", (int)(g_tr.elapsed * 25u / 3u),
		         (int)g_tr.seen_tries, tr_phase_name(g_tr.seen_phase));
}

/*
 * Everything the helper's thread answered, from its diagnostics block: every
 * call's status in hex, as the SDK's error codes read (0x8002xxxx).
 */
static void tr_log_result(void)
{
	static const char * const steps[6] = {
		"did not get as far as a call",
		"sceNpTrophyCreateContext failed",
		"sceNpTrophyCreateHandle failed",
		"sceNpTrophyRegisterContext failed",
		"registered, but the game's or a trophy's info could not be read (the build before)",
		"registered"
	};
	u8 diag[RAC3_TROPHY_DIAG_LEN];
	u8 tries, i;

	if (!tr_read_diag(diag)) return;
	tries = diag[RAC3_TD_TRIES];
	if (tries > RAC3_TROPHY_TRIES_MAX) tries = RAC3_TROPHY_TRIES_MAX;

	plat_log("trophies: destroy: sceNpTrophyDestroyHandle 0x%08x, sceNpTrophyDestroyContext 0x%08x",
	         be32_get(diag + RAC3_TD_DESTROY), be32_get(diag + RAC3_TD_DESTROY + 4));
	for (i = 0; i < tries; i++) {
		u8 step = diag[RAC3_TD_STEPS + i];

		plat_log("trophies: registration %d of %d: %s (0x%08x); "
		         "sceNpTrophyGetRequiredDiskSpace 0x%08x",
		         (int)i + 1, (int)tries, steps[step <= RAC3_TROPHY_STEP_REGISTERED ? step : 0],
		         be32_get(diag + RAC3_TD_STATUS + 4u * i), be32_get(diag + RAC3_TD_SPACE + 4u * i));
	}
	if (tries > 1)
		plat_log("trophies: the destroy before registration %d: 0x%08x, 0x%08x", (int)tries,
		         be32_get(diag + RAC3_TD_LAST_DESTROY), be32_get(diag + RAC3_TD_LAST_DESTROY + 4));
	plat_log("trophies: sceNpTrophyRegisterContext called back %d times, the last with status %d, "
	         "%d of %d", (int)diag[RAC3_TD_CB], (int)diag[RAC3_TD_CB + 1],
	         (int)diag[RAC3_TD_CB + 2], (int)diag[RAC3_TD_CB + 3]);
}

/* Watches the request byte until the helper has answered it. */
static void tr_wait(void)
{
	u32 addr = tr_request_addr();
	u8 byte = 0;

	g_tr.ticks++;
	g_tr.elapsed++;
	if (mem_read_u8(addr, &byte) != ST_OK) {
		if (g_tr.ticks > TR_RUN_TICKS) {
			tr_end(RAC3_TS_FAILED, RAC3_TS_STOPPED, 0);
			tr_idle();
		}
		return;
	}
	if (byte != g_tr.request) tr_log_progress();

	if (byte == 0) {
		tr_log_result();
		plat_log("trophies: reset; the game has its trophies back %d ms after the request, and 10, "
		         "25 and 32 can be won again", (int)(g_tr.elapsed * 25u / 3u));

		/*
		 * Until now the game awarded nothing, and a trophy earned in that time
		 * is lost, so this is the moment a client says the trophies are ready.
		 * Revision 1.21: the readout says it, and the console's screen no
		 * longer does. A delete that stopped earlier leaves the trophies won
		 * before it still won, so that reset is a failure for all that the
		 * game registered again: it said so on the screen when it stopped.
		 */
		if (g_tr.delete_failed)
			tr_end(RAC3_TS_FAILED, RAC3_TS_DELETE_STOPPED, 0);
		else
			tr_end(RAC3_TS_READY, 0, 0);
		tr_idle();
		return;
	}

	if (byte == RAC3_TROPHY_UNREGISTERED) {
		mem_write_u8(addr, 0);
		tr_log_result();
		plat_log("trophies: the reset ran, but every registration failed and the game has no trophies "
		         "to award; reset again or restart the game");
		plat_notify("Trophy reset failed: the game could not re-register its trophies");
		tr_end(RAC3_TS_FAILED, RAC3_TS_UNREGISTERED, 0);
		tr_idle();
		return;
	}

	/* The context is down: the folder goes now, and the thread waits for the 7. */
	if (byte == RAC3_TROPHY_DESTROYED) {
		if (g_tr.present) {
			g_tr.state = TR_DELETE;
		} else {
			tr_delete_done();
		}
		return;
	}

	if (byte == RAC3_TROPHY_RUNNING || byte == RAC3_TROPHY_DELETED) {
		if (g_tr.ticks > TR_RUN_TICKS) {
			tr_log_result();
			plat_log("trophies: the reset has run for ten minutes without an answer, and qwark "
			         "stops watching it; restart the game");
			plat_notify("Trophy reset: the console has not answered the game; restart the game");
			tr_end(RAC3_TS_FAILED, RAC3_TS_NO_ANSWER, 0);
			tr_idle();
		}
		return;
	}

	if (byte == SAVEFILE_HELPER_REFUSED) {
		int in_load = tr_in_load();

		mem_write_u8(addr, 0);
		if (g_tr.request == RAC3_TROPHY_IN_LOAD && g_tr.retries < TR_RETRIES && in_load) {
			/* A trophy thread of the game's was running; it ends in moments. */
			g_tr.retries++;
			g_tr.state = TR_ASK;
			g_tr.ticks = 0;
			return;
		}
		plat_log("trophies: the game refused the reset: its trophies were not set up, a trophy thread of its own or a reset was running, the load was over, or no thread could be started");
		/* The auto toggle's request outside its load is refused for that alone. */
		tr_end(RAC3_TS_FAILED,
		       g_tr.request == RAC3_TROPHY_IN_LOAD && !in_load ? RAC3_TS_LOAD_ENDED
		                                                       : RAC3_TS_GAME_REFUSED, 0);
		tr_idle();
		return;
	}

	if (byte == g_tr.request) {
		if (g_tr.ticks > TR_SEEN_TICKS) {
			mem_write_u8(addr, 0);
			plat_log("trophies: the game never picked the reset up, and it is withdrawn");
			tr_end(RAC3_TS_FAILED, RAC3_TS_NOT_TAKEN, 0);
			tr_idle();
		}
		return;
	}

	/* Something else wrote the byte: put it back and stop. */
	mem_write_u8(addr, 0);
	tr_end(RAC3_TS_FAILED, RAC3_TS_STOPPED, 0);
	tr_idle();
}

void rac3_trophy_tick(void)
{
	switch (g_tr.state) {
	case TR_DELETE:
		g_tr.elapsed++;
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
	case R3_UNLOCK_TITANIUM: return rac3_unlock_titanium();
	case R3_RESET_TITANIUM:  return mem_write_zeros(RAC3_TITANIUM_BOLTS, RAC3_TI_LEN);

	case R3_UPGRADE_ALL:     return rac3_all_versions(1);
	case R3_DOWNGRADE_ALL:   return rac3_all_versions(0);

	case R3_SET_ASIDE:       return savefile_set_aside();
	case R3_LOAD_ASIDE:      return rac3_load_setaside();

	default:                 return ST_NOT_FOUND;
	}
}
