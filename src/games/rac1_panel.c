/*
 * Ratchet & Clank (NPEA00385): the handlers behind the descriptor table, the
 * planet load, the level flag regions and the unlock table.
 *
 * The addresses live in rac1.h. Everything here is a straight port of what the
 * old client's forms did, and where the old code was ambiguous the comment says
 * what was assumed so it can be checked on hardware.
 *
 * All of it runs on the tick thread, through the command ring.
 */
#include "rac1.h"
#include "classic.h"
#include "../core/mem.h"

#include <string.h>

/* ------------------------------------------------------------ the unlocks */

/*
 * NewUnlocks.cs in full. `ammo_base` is the address that file passes to the
 * RaC1Item constructor; the constructor adds 8 to it ("accidentally messed up
 * the offset oops oh well"), so the live ammo word is at ammo_base + 8 and this
 * file adds it in one place, rac1_ammo_addr, rather than baking it in here.
 *
 * The five entries with no index at the end are the item flags at 0x96BFF0, not
 * part of the unlock array; they own nothing but their byte.
 */
#define CAT_WEAPONS 0
#define CAT_GADGETS 1
#define CAT_ITEMS   2

struct rac1_item {
	u32 unlock;
	u32 ammo_base;   /* 0 when the entry has no ammo word */
	u32 gold;        /* 0 when the entry has no gold byte */
	u16 max_ammo;
};

static const char * const rac1_categories[] = { "Weapons", "Gadgets", "Items" };

static const struct game_unlock rac1_unlocks[] = {
	/* Weapons */
	{  0, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Bomb Glove" },
	{  1, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Pyrocitor" },
	{  2, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Blaster" },
	{  3, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Glove Of Doom" },
	{  4, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Mine Glove" },
	{  5, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD,                     "Taunter" },
	{  6, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD,                     "Suck Cannon" },
	{  7, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Devastator" },
	{  8, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD,                     "Walloper" },
	{  9, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Visibomb" },
	{ 10, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Decoy Glove" },
	{ 11, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Drone Device" },
	{ 12, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "Tesla Claw" },
	{ 13, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD,                     "Morph-O-Ray" },
	{ 14, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD | UNLOCK_FIELD_AMMO, "RYNO" },
	{ 15, CAT_WEAPONS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD,                     "Wrench" },

	/* Gadgets */
	{ 16, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Heli-Pack" },
	{ 17, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Thruster-Pack" },
	{ 18, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Hydro-Pack" },
	{ 19, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "PDA" },
	{ 20, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Swingshot" },
	{ 21, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "O2 Mask" },
	{ 22, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Pilots Helmet" },
	{ 23, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Magneboots" },
	{ 24, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Grindboots" },
	{ 25, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Trespasser" },
	{ 26, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Hydrodisplacer" },
	{ 27, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Sonic Summoner" },
	{ 28, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Metal Detector" },
	{ 29, CAT_GADGETS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Hologuise" },

	/* Items */
	{ 30, CAT_ITEMS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Map-O-Matic" },
	{ 31, CAT_ITEMS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Bolt Grabber" },
	{ 32, CAT_ITEMS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Persuader" },
	{ 33, CAT_ITEMS, UNLOCK_FIELD_OWNED | UNLOCK_FIELD_GOLD, "Hoverboard" },
	{ 34, CAT_ITEMS, UNLOCK_FIELD_OWNED, "Zoomerator" },
	{ 35, CAT_ITEMS, UNLOCK_FIELD_OWNED, "Raritanium" },
	{ 36, CAT_ITEMS, UNLOCK_FIELD_OWNED, "Codebot" },
	{ 37, CAT_ITEMS, UNLOCK_FIELD_OWNED, "Premium Nanotech" },
	{ 38, CAT_ITEMS, UNLOCK_FIELD_OWNED, "Ultra Nanotech" }
};

/* Parallel to rac1_unlocks, id by id. */
static const struct rac1_item rac1_items[] = {
	{ 0x96C14Au, 0x96C0CCu, 0x969CB2u,  40 },   /* Bomb Glove */
	{ 0x96C150u, 0x96C0E4u, 0x969CB8u, 240 },   /* Pyrocitor */
	{ 0x96C14Fu, 0x96C0E0u, 0x969CB7u, 200 },   /* Blaster */
	{ 0x96C154u, 0x96C0F4u, 0x969CBCu,  10 },   /* Glove Of Doom */
	{ 0x96C151u, 0x96C0E8u, 0x969CB9u,  50 },   /* Mine Glove */
	{ 0x96C14Eu, 0x96C0DCu, 0x969CB6u,   0 },   /* Taunter */
	{ 0x96C149u, 0x96C0C8u, 0x969CB1u,   0 },   /* Suck Cannon */
	{ 0x96C14Bu, 0x96C0D0u, 0x969CB3u,  20 },   /* Devastator */
	{ 0x96C152u, 0x96C0ECu, 0x969CBAu,   0 },   /* Walloper */
	{ 0x96C14Du, 0x96C0D8u, 0x969CB5u,  20 },   /* Visibomb */
	{ 0x96C159u, 0x96C108u, 0x969CC1u,  20 },   /* Decoy Glove */
	{ 0x96C158u, 0x96C104u, 0x969CC0u,  10 },   /* Drone Device */
	{ 0x96C153u, 0x96C0F0u, 0x969CBBu, 240 },   /* Tesla Claw */
	{ 0x96C155u, 0x96C0F8u, 0x969CBDu,   0 },   /* Morph-O-Ray */
	{ 0x96C157u, 0x96C100u, 0x969CBFu,  50 },   /* RYNO */
	{ 0x96C148u, 0x96C0C4u, 0x969CB0u,   0 },   /* Wrench */

	{ 0x96C142u, 0x96C0ACu, 0x969CAAu, 0 },     /* Heli-Pack */
	{ 0x96C143u, 0x96C0B0u, 0x969CABu, 0 },     /* Thruster-Pack */
	{ 0x96C144u, 0x96C0B4u, 0x969CACu, 0 },     /* Hydro-Pack */
	{ 0x96C160u, 0x96C124u, 0x969CC8u, 0 },     /* PDA */
	{ 0x96C14Cu, 0x96C0D4u, 0x969CB4u, 0 },     /* Swingshot */
	{ 0x96C146u, 0x96C0BCu, 0x969CAEu, 0 },     /* O2 Mask */
	{ 0x96C147u, 0x96C0C0u, 0x969CAFu, 0 },     /* Pilots Helmet */
	{ 0x96C15Cu, 0x96C114u, 0x969CC4u, 0 },     /* Magneboots */
	{ 0x96C15Du, 0x96C118u, 0x969CC5u, 0 },     /* Grindboots */
	{ 0x96C15Au, 0x96C10Cu, 0x969CC2u, 0 },     /* Trespasser */
	{ 0x96C156u, 0x96C0FCu, 0x969CBEu, 0 },     /* Hydrodisplacer */
	{ 0x96C145u, 0x96C0B8u, 0x969CADu, 0 },     /* Sonic Summoner */
	{ 0x96C15Bu, 0x96C110u, 0x969CC3u, 0 },     /* Metal Detector */
	{ 0x96C15Fu, 0x96C120u, 0x969CC7u, 0 },     /* Hologuise */

	{ 0x96C161u, 0x96C128u, 0x969CC9u, 0 },     /* Map-O-Matic */
	{ 0x96C162u, 0x96C12Cu, 0x969CCAu, 0 },     /* Bolt Grabber */
	{ 0x96C163u, 0x96C130u, 0x969CCBu, 0 },     /* Persuader */
	{ 0x96C15Eu, 0x96C11Cu, 0x969CC6u, 0 },     /* Hoverboard */
	{ 0x96BFF0u, 0, 0, 0 },                     /* Zoomerator */
	{ 0x96BFF1u, 0, 0, 0 },                     /* Raritanium */
	{ 0x96BFF2u, 0, 0, 0 },                     /* Codebot */
	{ 0x96BFF4u, 0, 0, 0 },                     /* Premium Nanotech */
	{ 0x96BFF5u, 0, 0, 0 }                      /* Ultra Nanotech */
};

#define RAC1_UNLOCK_COUNT ((u8)(sizeof(rac1_unlocks) / sizeof(rac1_unlocks[0])))

/* The +8 NewUnlocks.cs bakes into every RaC1Item. */
static u32 rac1_ammo_addr(const struct rac1_item *it)
{
	return it->ammo_base == 0 ? 0 : it->ammo_base + 8;
}

/*
 * Two reads cover every live value in the table:
 *
 *   A  0x96BFF0 .. 0x96C163   the five item flags, the ammo words and the
 *                             unlock array, which happen to sit in that order
 *   B  0x969CA8 .. 0x969CCB   the gold bytes
 *
 * unlock_list fills them and unlock_read serves each row from them; the core
 * calls the pair back to back on the tick thread, so nothing goes stale between.
 */
#define SNAP_A_ADDR RAC1_MOVIE_FLAGS
#define SNAP_A_LEN  0x174u
#define SNAP_B_ADDR RAC1_GOLD_ITEMS
#define SNAP_B_LEN  0x24u

static u8 g_snap_a[SNAP_A_LEN];
static u8 g_snap_b[SNAP_B_LEN];
static int g_snap_valid;

static int snap_byte(u32 addr, u8 *out)
{
	if (!g_snap_valid) return 0;

	if (addr >= SNAP_A_ADDR && addr < SNAP_A_ADDR + SNAP_A_LEN) {
		*out = g_snap_a[addr - SNAP_A_ADDR];
		return 1;
	}
	if (addr >= SNAP_B_ADDR && addr < SNAP_B_ADDR + SNAP_B_LEN) {
		*out = g_snap_b[addr - SNAP_B_ADDR];
		return 1;
	}
	return 0;
}

static int snap_word(u32 addr, u32 *out)
{
	if (!g_snap_valid) return 0;
	if (addr < SNAP_A_ADDR || addr + 4 > SNAP_A_ADDR + SNAP_A_LEN) return 0;

	*out = be32_get(g_snap_a + (addr - SNAP_A_ADDR));
	return 1;
}

int rac1_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories)
{
	int rc;

	*list        = rac1_unlocks;
	*count       = RAC1_UNLOCK_COUNT;
	*categories  = rac1_categories;
	*ncategories = (u8)(sizeof(rac1_categories) / sizeof(rac1_categories[0]));

	g_snap_valid = 0;

	rc = mem_read(SNAP_A_ADDR, g_snap_a, SNAP_A_LEN);
	if (rc != ST_OK) return rc;
	rc = mem_read(SNAP_B_ADDR, g_snap_b, SNAP_B_LEN);
	if (rc != ST_OK) return rc;

	g_snap_valid = 1;
	return ST_OK;
}

int rac1_unlock_read(const struct game_unlock *entry, u32 values[4])
{
	const struct rac1_item *it;
	u8 b = 0;
	u32 w = 0;

	if (entry == NULL || entry->id >= RAC1_UNLOCK_COUNT) return ST_BAD_ARG;
	it = &rac1_items[entry->id];

	values[0] = values[1] = values[2] = values[3] = 0;

	if (snap_byte(it->unlock, &b)) values[0] = b;
	if (it->gold != 0 && snap_byte(it->gold, &b)) values[1] = b;
	if ((entry->fields & UNLOCK_FIELD_AMMO) != 0 &&
	    snap_word(rac1_ammo_addr(it), &w))
		values[3] = w;

	return ST_OK;
}

int rac1_unlock_set(u8 id, u8 field, u32 value)
{
	const struct rac1_item *it;
	int rc;

	if (id >= RAC1_UNLOCK_COUNT) return ST_BAD_ARG;
	it = &rac1_items[id];

	switch (field) {
	case 0:
		rc = mem_write_u8(it->unlock, value != 0 ? 1 : 0);
		if (rc != ST_OK) return rc;

		/*
		 * NewUnlocks.Unlock() also hands a weapon its full ammo. It does that
		 * for every weapon, so the ones whose max ammo is 0 (Wrench, Taunter,
		 * Suck Cannon, Walloper, Morph-O-Ray) get a 0 written; that is what the
		 * old client did and the ammo word is theirs either way.
		 */
		if (value != 0 && rac1_ammo_addr(it) != 0 &&
		    (rac1_unlocks[id].category == CAT_WEAPONS || it->max_ammo != 0))
			return mem_write_u32(rac1_ammo_addr(it), it->max_ammo);

		return ST_OK;

	case 1:
		if (it->gold == 0) return ST_UNSUPPORTED;
		return mem_write_u8(it->gold, value != 0 ? 1 : 0);

	case 3:
		if (rac1_ammo_addr(it) == 0) return ST_UNSUPPORTED;
		return mem_write_u32(rac1_ammo_addr(it), value);

	default:
		/* RaC1 has no level or XP field. */
		return ST_UNSUPPORTED;
	}
}

/* Every weapon gets its maximum, the "give all ammo" button from NewUnlocks. */
static int rac1_max_ammo_all(void)
{
	u8 i;

	for (i = 0; i < RAC1_UNLOCK_COUNT; i++) {
		const struct rac1_item *it = &rac1_items[i];
		int rc;

		if (rac1_unlocks[i].category != CAT_WEAPONS) continue;
		if (rac1_ammo_addr(it) == 0) continue;

		rc = mem_write_u32(rac1_ammo_addr(it), it->max_ammo);
		if (rc != ST_OK) return rc;
	}

	return ST_OK;
}

/* ------------------------------------------------------------ level flags */

/*
 * rac1.cs ResetLevelFlags, for the planet that is about to be loaded. The
 * per-planet blocks and their unlock take-backs are copied verbatim; what each
 * one is has never been written down anywhere but the addresses.
 */
static int rac1_reset_level_flags(u8 planet)
{
	int rc;

	if (planet >= rac1_planet_count()) return ST_BAD_ARG;

	rc = mem_write_zeros(RAC1_LEVEL_FLAGS + (u32)planet * RAC1_LF_MAIN_LEN,
	                     RAC1_LF_MAIN_LEN);
	if (rc != ST_OK) return rc;

	rc = mem_write_zeros(RAC1_MISC_LEVEL_FLAGS + (u32)planet * RAC1_LF_MISC_LEN,
	                     RAC1_LF_MISC_LEN);
	if (rc != ST_OK) return rc;

	/*
	 * Eighteen bytes at infobotFlags + planet, a byte offset rather than an
	 * index into anything: that is literally what the old code writes.
	 */
	rc = mem_write_zeros(RAC1_INFOBOT_FLAGS + planet, 18);
	if (rc != ST_OK) return rc;

	rc = mem_write_zeros(0x96BFF8u, 0x89);
	if (rc != ST_OK) return rc;

	switch (planet) {
	case 3:    /* Kerwan */
		rc = mem_write_zeros(0x96C378u, 0xF0);
		if (rc != ST_OK) return rc;
		mem_write_u8(RAC1_UNLOCK_ARRAY + 2, 0);    /* Heli-Pack */
		mem_write_u8(RAC1_UNLOCK_ARRAY + 12, 0);   /* Swingshot */
		break;

	case 4:    /* Eudora */
		rc = mem_write_zeros(0x96C468u, 0x40);
		if (rc != ST_OK) return rc;
		mem_write_u8(RAC1_UNLOCK_ARRAY + 9, 0);    /* Suck Cannon */
		break;

	case 5:    /* Rilgar */
		rc = mem_write_zeros(0x96C498u, 0xA0);
		if (rc != ST_OK) return rc;
		break;

	case 6:    /* Blarg */
		mem_write_u8(RAC1_UNLOCK_ARRAY + 29, 0);   /* Grindboots */
		break;

	case 8:    /* Batalia */
		rc = mem_write_zeros(0x96C5A8u, 0x40);
		if (rc != ST_OK) return rc;
		break;

	case 9:    /* Gaspar */
		rc = mem_write_zeros(0x96C5E8u, 0x20);
		if (rc != ST_OK) return rc;
		mem_write_u8(RAC1_UNLOCK_ARRAY + 7, 0);    /* Pilots Helmet */
		break;

	case 10: { /* Orxon */
		u8 o2 = 0;

		mem_write_u8(RAC1_UNLOCK_ARRAY + 28, 0);   /* Magneboots */

		/*
		 * The O2 mask case. The old code reads as "if you own the O2 mask, put
		 * its infobot back", and writes a 32-bit 1 at infobotFlags + 11, which
		 * lands the 1 four bytes further along than a byte write would. Its own
		 * comment there is "Figure it out", so the exact bytes are kept rather
		 * than a guess at the intent.
		 */
		if (mem_read_u8(RAC1_UNLOCK_ARRAY + 6, &o2) == ST_OK && o2 != 0)
			mem_write_u32(RAC1_INFOBOT_FLAGS + 11, 1);
		break;
	}

	case 11:   /* Pokitaru */
		mem_write_u8(RAC1_UNLOCK_ARRAY + 3, 0);    /* Thruster-Pack */
		mem_write_u8(RAC1_UNLOCK_ARRAY + 6, 0);    /* O2 Mask */
		break;

	default:
		break;
	}

	return ST_OK;
}

int rac1_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len)
{
	int rc;

	*len = 0;
	if (planet >= rac1_planet_count()) return ST_BAD_ARG;
	if (cap < RAC1_LF_TOTAL) return ST_FULL;

	rc = mem_read(RAC1_LEVEL_FLAGS + (u32)planet * RAC1_LF_MAIN_LEN,
	              out, RAC1_LF_MAIN_LEN);
	if (rc != ST_OK) return rc;

	rc = mem_read(RAC1_MISC_LEVEL_FLAGS + (u32)planet * RAC1_LF_MISC_LEN,
	              out + RAC1_LF_MAIN_LEN, RAC1_LF_MISC_LEN);
	if (rc != ST_OK) return rc;

	*len = RAC1_LF_TOTAL;
	return ST_OK;
}

int rac1_levelflags_set(u8 planet, u16 offset, u8 value)
{
	if (planet >= rac1_planet_count()) return ST_BAD_ARG;

	if (offset < RAC1_LF_MAIN_LEN)
		return mem_write_u8(RAC1_LEVEL_FLAGS +
		                    (u32)planet * RAC1_LF_MAIN_LEN + offset, value);

	if (offset < RAC1_LF_TOTAL)
		return mem_write_u8(RAC1_MISC_LEVEL_FLAGS +
		                    (u32)planet * RAC1_LF_MISC_LEN +
		                    (offset - RAC1_LF_MAIN_LEN), value);

	return ST_BAD_ARG;
}

int rac1_levelflags_reset(u8 planet)
{
	return rac1_reset_level_flags(planet);
}

/* ---------------------------------------------------------------- planets */

int rac1_planet_load(u8 planet, u8 flags)
{
	int rc;

	if (planet >= rac1_planet_count()) return ST_BAD_ARG;

	/*
	 * IGame.LoadPlanet writes the request first and only then resets, so the
	 * flags land while the game is already on its way out of the level. Kept in
	 * that order deliberately: it is what runners' muscle memory is built on.
	 */
	rc = classic_planet_request(RAC1_LOAD_PLANET, planet);
	if (rc != ST_OK) return rc;

	if (flags & PLANET_FLAG_RESET_LEVELFLAGS) {
		rc = rac1_reset_level_flags(planet);
		if (rc != ST_OK) return rc;
	}

	if (flags & PLANET_FLAG_RESET_BOLTS) {
		/* ResetGoldBolts: one 32-bit zero, four gold bolts per planet. */
		rc = mem_write_u32(RAC1_GOLD_BOLTS + (u32)planet * 4, 0);
		if (rc != ST_OK) return rc;
	}

	return ST_OK;
}

/* --------------------------------------------------------------- savefile */

/*
 * The three savefile requests only mean anything while the savefile helper mod
 * is loaded; without it the bytes are somebody else's memory. The helper byte
 * is read live rather than taken from the readout, because an action is rare
 * and a stale answer here writes into a game that is not listening.
 */
static int savefile_request(u32 addr, u8 value)
{
	u8 present = 0;
	int rc = mem_read_u8(RAC1_SAVEFILE_HELPER, &present);

	if (rc != ST_OK) return rc;
	if (present != 1) return ST_UNSUPPORTED;

	return mem_write_u8(addr, value);
}

int rac1_load_setaside(void)
{
	return savefile_request(RAC1_SAVEFILE_LOAD, 1);
}

/* ---------------------------------------------------------------- jankpot */

/*
 * JankpotForm writes the state as the four raw bytes 01 00 00 00. On a
 * big-endian console that is the word 0x01000000, not 1, and its own reader
 * only ever tests the word against zero, so which the game means is not
 * knowable from the old client. The bytes it wrote are what goes out here.
 */
static const u8 rac1_jankpot_on[4] = { 0x01, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------ the handlers */

static const char * const rac1_camera_options[] = {
	"Normal",
	"Freecam",
	"Freecam character"
};

int rac1_get_options(u8 id, const char * const **options, u8 *count)
{
	if (id != F_DBG_CAMERA) return ST_NOT_FOUND;

	*options = rac1_camera_options;
	*count = (u8)(sizeof(rac1_camera_options) / sizeof(rac1_camera_options[0]));
	return ST_OK;
}

/*
 * SetDebugOption's camera cases: normal hands the camera back to the game by
 * setting the update-camera bit, the two freecams take it away, and the mode
 * word follows.
 */
static int rac1_set_camera(u32 mode)
{
	int rc;

	if (mode > 2) return ST_BAD_ARG;

	rc = rac1_debug_bit(RAC1_DBG_CAMERA, mode == 0);
	if (rc != ST_OK) return rc;

	return mem_write_u32(RAC1_DEBUG_MODE, mode);
}

int rac1_set_value(u8 id, u32 value)
{
	switch (id) {
	case F_BOLTS:      return mem_write_u32(RAC1_BOLTS, value);
	case F_JANK_BOLTS: return mem_write_u32(RAC1_JANKPOT_BOLTS, value);
	case F_JANK_TIMER: return mem_write_u32(RAC1_JANKPOT_TIMER, value);
	case F_DBG_CAMERA: return rac1_set_camera(value);
	default:           return ST_NOT_FOUND;
	}
}

/* SetShootSkillPoints, both directions. */
static int rac1_shoot_skill_points(int setup)
{
	u8 sonic[8];
	u8 block[32];
	int i;
	int rc;

	memset(sonic, 0, sizeof(sonic));
	memset(block, 0, sizeof(block));

	if (setup) {
		be32_put(sonic, 1);
		be32_put(sonic + 4, 1);
		for (i = 0; i < 8; i++) be32_put(block + i * 4, 0x20);
	}

	rc = mem_write(RAC1_SONIC_SP, sonic, sizeof(sonic));
	if (rc != ST_OK) return rc;

	return mem_write(RAC1_SHOOT_SP, block, sizeof(block));
}

/* The resetAllMissionsStuff button: Blarg bridge plus the Rilgar race floats. */
static int rac1_anypct_reset(void)
{
	int rc = mem_write_u32(RAC1_ANYPCT_FLAG, 0);

	if (rc != ST_OK) return rc;

	rc = mem_write_zeros(RAC1_ANYPCT_RACE, 12);
	if (rc != ST_OK) return rc;

	plat_notify("Blarg bridge and rilgar race reset for any% all missions. "
	            "Good luck!");
	return ST_OK;
}

/* ForceOkayLoad: the twelve bytes RAC1Form writes over the loading screen block. */
static int rac1_force_okay_load(void)
{
	static const u8 okay[12] = {
		0x00, 0x00, 0x00, 0x1A,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x02
	};

	return mem_write(RAC1_LOADING_SCREEN, okay, sizeof(okay));
}

int rac1_trigger(u8 id)
{
	switch (id) {
	case F_DIE:
		return classic_die_set_z(RAC1_COORDS);

	case F_DREK_SKIP:
		plat_notify("Drek skip done for non poki skip NG+ runs :)");
		return mem_write_u8(RAC1_DREK_SKIP, 1);

	case F_DREK_CUTSCENE:
		plat_notify("Drek cutscene done for NG+ poki skip runs :)");
		return mem_write_u8(RAC1_DREK_CUTSCENE, 1);

	case F_FORCE_OKAY_LOAD:
		return rac1_force_okay_load();

	case F_RESET_SHOOT_SP:
		return rac1_shoot_skill_points(0);

	case F_SETUP_SHOOT_SP:
		return rac1_shoot_skill_points(1);

	case F_RESET_GOLDBOLTS:
		/* Four per planet, twenty planets' worth of slots. */
		return mem_write_zeros(RAC1_GOLD_BOLTS, 80);

	case F_UNLOCK_GOLDBOLTS:
		return mem_write_fill(RAC1_GOLD_BOLTS, 1, 80);

	case F_RESET_STYLE:
		return mem_write_zeros(RAC1_STYLE_POINTS, 30);

	case F_UNLOCK_STYLE:
		return mem_write_fill(RAC1_STYLE_POINTS, 1, 30);

	case F_ANYPCT_RESET:
		return rac1_anypct_reset();

	case F_MAX_AMMO:
		return rac1_max_ammo_all();

	case F_LOAD_SETASIDE:
		return savefile_request(RAC1_SAVEFILE_LOAD, 1);

	case F_SET_ASIDE_FILE:
		return savefile_request(RAC1_SAVEFILE_ASIDE, 1);

	case F_FORCE_AUTOSAVE:
		/* Three, not one: RAC1Form's forceAutosave writes a 3. */
		return savefile_request(RAC1_SAVEFILE_AUTO, 3);

	case F_JANK_ACTIVATE:
		return mem_write(RAC1_JANKPOT_STATE, rac1_jankpot_on,
		                 sizeof(rac1_jankpot_on));

	default:
		return ST_NOT_FOUND;
	}
}
