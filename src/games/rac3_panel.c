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
 * those words either. They are owned-only entries here.
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

static const struct game_unlock rac3_unlocks[] = {
	/* levels 0: the bomb glove is deliberately kept out of both menus. */
	{  0, CAT_GADGETS, OWNED | XP | AMMO, "Bomb Glove" },

	{  1, CAT_GADGETS, OWNED | XP | AMMO, "Heli Pack" },
	{  2, CAT_GADGETS, OWNED | XP | AMMO, "Thruster Pack" },
	{  3, CAT_GADGETS, OWNED | XP | AMMO, "Charge Boots" },
	{  4, CAT_GADGETS, OWNED | XP | AMMO, "Gravity Boots" },
	{  5, CAT_GADGETS, OWNED | XP | AMMO, "Tyhrra Guise" },
	{  6, CAT_GADGETS, OWNED | XP | AMMO, "Refractor" },
	{  7, CAT_GADGETS, OWNED | XP | AMMO, "Hypershot" },
	{  8, CAT_GADGETS, OWNED | XP | AMMO, "Nano Pak" },
	{  9, CAT_GADGETS, OWNED | XP | AMMO, "PDA" },
	{ 10, CAT_GADGETS, OWNED | XP | AMMO, "Bolt Grabber v2" },
	{ 11, CAT_GADGETS, OWNED | XP | AMMO, "Map-o-matic" },
	{ 12, CAT_GADGETS, OWNED | XP | AMMO, "Master Plan" },
	{ 13, CAT_GADGETS, OWNED | XP | AMMO, "Star Map" },
	{ 14, CAT_GADGETS, OWNED | XP | AMMO, "The Hacker" },
	{ 15, CAT_GADGETS, OWNED | XP | AMMO, "Warp Pad" },

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

/* Parallel to rac3_unlocks, id by id. */
static const struct rac3_item rac3_items[] = {
	{ 0x0A, 0x4B2, 0x618, 0x26B, 0, 0, 0, 0 },      /* Bomb Glove */

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
	u8 b = 0;
	u32 w = 0;

	if (entry == NULL || entry->id >= RAC3_UNLOCK_COUNT) return ST_BAD_ARG;
	it = &rac3_items[entry->id];

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

	if (id >= RAC3_UNLOCK_COUNT) return ST_BAD_ARG;
	if (field > 3) return ST_UNSUPPORTED;

	/*
	 * The row's declared fields are the contract, so a slot it does not offer
	 * is refused even where the address behind it happens to exist: the Suck
	 * Cannon's ammo word is not the count the game uses.
	 */
	if ((rac3_unlocks[id].fields & (u8)(1u << field)) == 0) return ST_UNSUPPORTED;

	it = &rac3_items[id];

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

/* -------------------------------------------------------------- the setups */

/* rac3.cs SetupFile, the "Setup NG+ Manips" button. */
static int rac3_setup_file(void)
{
	mem_write_u32(RAC3_KLUNK_TUNING_1, 7);
	mem_write_u32(RAC3_KLUNK_TUNING_2, 3);
	mem_write_u32(RAC3_NEFFY_TUNING, 0xE);
	mem_write_u32(RAC3_VID_COMIC_MENU, 2);
	mem_write_u32(RAC3_CC_HELP_DESK, 1);

	plat_notify("Klunk, Neffy, vid comic menu and CC helpdesk set up for runs");
	return ST_OK;
}

/* rac3.cs UntuneBosses. */
static int rac3_untune_bosses(void)
{
	mem_write_u32(RAC3_DROPSHIP_HEALTH, 100);   /* the Veldin default */
	mem_write_u32(RAC3_NEFFY_TUNING, 0);
	mem_write_u32(RAC3_KLUNK_TUNING_1, 0);
	mem_write_u32(RAC3_KLUNK_TUNING_2, 0);
	return ST_OK;
}

/* RAC3Form.ccEarlyButton_Click. */
static int rac3_cc_early(void)
{
	mem_write_u32(RAC3_CC_FAKE_ITEM_A, 0x41);
	mem_write_u32(RAC3_CC_FAKE_ITEM_B, 0x41414141u);
	plat_notify("Annihilator V4 fake item has been setup for CC Early runs!");
	return ST_OK;
}

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

	case R3_VENDOR_QE: {
		/* vendorQeEnableButton wrote the two bytes 00 17. */
		static const u8 vendor[2] = { 0x00, 0x17 };
		return mem_write(RAC3_QE_OFFSET, vendor, sizeof(vendor));
	}

	case R3_SETUP_NGPLUS:   return rac3_setup_file();
	case R3_CC_EARLY:       return rac3_cc_early();
	case R3_UNTUNE_BOSSES:  return rac3_untune_bosses();
	case R3_RESET_DROPSHIP: return mem_write_u32(RAC3_DROPSHIP_HEALTH, 100);

	case R3_RESET_TROPHIES:
		/*
		 * The old button also deleted /dev_hdd0/home/<user>/trophy/NPWR02347_00
		 * over the wire before writing this. The client owns the file ops, so it
		 * does the delete with DIR_DELETE and USER_ID and then fires this.
		 */
		return mem_write_u32(RAC3_TROPHY_REFRESH, 1);

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
