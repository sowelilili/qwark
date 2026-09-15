/*
 * Ratchet & Clank 2 (NPEA00386, BCES01503): the handlers behind the descriptor
 * table, the planet load, the level flag region and the unlock table.
 *
 * The addresses live in rac2.h. Everything here is a straight port of what
 * RAC2Form, RC2Unlocks and FormCollectables did, and where the old code was
 * ambiguous the comment says what was assumed so it can be checked on hardware.
 *
 * All of it runs on the tick thread, through the command ring.
 */
#include "rac2.h"
#include "classic.h"
#include "../core/mem.h"
#include "../core/savefile.h"

#include <string.h>

/* ------------------------------------------------------------ the unlocks */

/*
 * RC2Unlocks.cs was three lists of owned bytes and nothing else, and the old
 * form had no level or ammo column for RaC2. The addresses in those lists
 * are all one array, `u8 owned[item id]` at RAC2_OWNED_ARRAY, so the table below
 * carries each row's ITEM ID and every address is worked out from it: an item id
 * is the index into the owned, ammo and item arrays alike, exactly as RaC3
 * does. Every id here is the old address minus the array base, so the forty-four
 * rows keep the ids, categories and names they shipped with.
 *
 * "Unlock all" and "remove all" are this table walked end to end; UNLOCK_SET per
 * row gives the client the same reach without another opcode.
 */
#define CAT_WEAPONS 0
#define CAT_GADGETS 1
#define CAT_ITEMS   2

static const char * const rac2_categories[] = { "Weapons", "Gadgets", "Items" };

/*
 * Protocol 1.3, the four value slots as RaC2 uses them: gold weapons are a RaC1
 * idea and RaC2 has none, so slot 1 is the weapon VERSION, v1..v4.
 *
 * The maximum is game-wide, the way UnlockFieldDesc.max has to be: sixteen
 * weapons go to V4, the Clank Zapper and the five RaC1 carry-overs stop at V2,
 * so UNLOCK_SET clamps to the entry's own version count.
 *
 * Slot 2 is left unnamed. The game does keep an experience word per item, and
 * qwark read it out until build 33, but the column does not fit the table a
 * client draws and nothing was done with the number, so no row declares the
 * slot, UNLOCK_SET on it answers UNSUPPORTED and UNLOCK_LIST reports 0. The
 * Ammo slot keeps its number: slot numbers are part of the wire contract and a
 * client draws only the named ones, so shuffling it down would be a change to
 * the protocol for nothing.
 *
 * Slots 1 and 3 belong to the weapons. A gadget and an item are owned or not
 * owned and nothing else, so their rows declare slot 0 by itself and a client
 * draws no level or ammo cell against them.
 */
static const struct unlock_field_desc rac2_fields[4] = {
	{ "Owned", UNLOCK_KIND_FLAG,   0 },
	{ "Level", UNLOCK_KIND_NUMBER, RAC2_MAX_LEVELS },
	{ NULL,    UNLOCK_KIND_FLAG,   0 },
	{ "Ammo",  UNLOCK_KIND_NUMBER, 0 }
};

#define OWNED UNLOCK_FIELD_0
#define LEVEL UNLOCK_FIELD_1
#define AMMO  UNLOCK_FIELD_3

/* What a weapon with versions declares, and what one without them does. */
#define WPN_V  (OWNED | LEVEL | AMMO)
#define WPN_1  (OWNED | AMMO)

static const struct game_unlock rac2_unlocks[] = {
	/* Weapons */
	{  0, CAT_WEAPONS, WPN_V, "Lancer" },
	{  1, CAT_WEAPONS, WPN_V, "Gravity Bomb" },
	{  2, CAT_WEAPONS, WPN_V, "Chopper" },
	{  3, CAT_WEAPONS, WPN_V, "Seeker Gun" },
	{  4, CAT_WEAPONS, WPN_V, "Pulse Rifle" },
	{  5, CAT_WEAPONS, WPN_V, "Miniturret Glove" },
	{  6, CAT_WEAPONS, WPN_V, "Blitz Gun" },
	{  7, CAT_WEAPONS, WPN_V, "Shield Charger" },
	{  8, CAT_WEAPONS, WPN_V, "Synthenoid" },
	{  9, CAT_WEAPONS, WPN_V, "Lava Gun" },
	{ 10, CAT_WEAPONS, WPN_V, "Bouncer" },
	{ 11, CAT_WEAPONS, WPN_V, "Minirocket Tube" },
	{ 12, CAT_WEAPONS, WPN_V, "Plasma Coil" },
	{ 13, CAT_WEAPONS, WPN_V, "Hoverbomb Gun" },
	{ 14, CAT_WEAPONS, WPN_V, "Spiderbot Glove" },
	{ 15, CAT_WEAPONS, WPN_V, "Sheepinator" },
	{ 16, CAT_WEAPONS, WPN_V, "Tesla Claw" },
	{ 17, CAT_WEAPONS, WPN_V, "Bomb Glove" },
	{ 18, CAT_WEAPONS, WPN_V, "Walloper" },
	{ 19, CAT_WEAPONS, WPN_V, "Visibomb Gun" },
	{ 20, CAT_WEAPONS, WPN_V, "Decoy Glove" },
	{ 21, CAT_WEAPONS, WPN_1, "Zodiac" },
	{ 22, CAT_WEAPONS, WPN_1, "RYNO II" },
	{ 23, CAT_WEAPONS, WPN_V, "Clank Zapper" },

	/* Gadgets */
	{ 24, CAT_GADGETS, OWNED, "Swingshot" },
	{ 25, CAT_GADGETS, OWNED, "Dynamo" },
	{ 26, CAT_GADGETS, OWNED, "Thermanator" },
	{ 27, CAT_GADGETS, OWNED, "Tractor Beam" },
	{ 28, CAT_GADGETS, OWNED, "Hypnomatic" },
	{ 29, CAT_GADGETS, OWNED, "Heli-Pack" },
	{ 30, CAT_GADGETS, OWNED, "Thruster-Pack" },
	{ 31, CAT_GADGETS, OWNED, "Gravity Boots" },
	{ 32, CAT_GADGETS, OWNED, "Grindboots" },
	{ 33, CAT_GADGETS, OWNED, "Charge Boots" },

	/* Items */
	{ 34, CAT_ITEMS, OWNED, "Biker Helmet" },
	{ 35, CAT_ITEMS, OWNED, "Glider" },
	{ 36, CAT_ITEMS, OWNED, "Qwark Statuette" },
	{ 37, CAT_ITEMS, OWNED, "Armor Magnetizer" },
	{ 38, CAT_ITEMS, OWNED, "Box Breaker" },
	{ 39, CAT_ITEMS, OWNED, "Mapper" },
	{ 40, CAT_ITEMS, OWNED, "Electrolyzer" },
	{ 41, CAT_ITEMS, OWNED, "Infiltrator" },
	{ 42, CAT_ITEMS, OWNED, "Hydro-Pack" },
	{ 43, CAT_ITEMS, OWNED, "Levitator" }
};

#undef WPN_V
#undef WPN_1

/*
 * OWNED, LEVEL and AMMO stay defined for the rest of the file: unlock_read and
 * unlock_set test the same slot bits, and spelling them out there is what keeps
 * the read and the descriptor table honest with each other.
 */

/*
 * Parallel to rac2_unlocks, row by row. `id` is the item id, which is the index
 * into every array the game keeps, and `v` holds the item ids of versions 2, 3
 * and 4: a weapon version is an item of its own, the way it is in RaC3.
 *
 * The version ids come from the community item list, and the chain they make is
 * the one the game keeps in its stats records: record(id) + 0x46 is the next
 * version's id, which is what its own upgrade routine (0xB25078) follows.
 *
 * The five RaC1 carry-overs (Tesla Claw, Bomb Glove, Walloper, Visibomb Gun and
 * Decoy Glove) each have a second and last version, bought from Slim Cognito
 * rather than earned, and the item list gives those ids 114 to 118 in that
 * order. It calls them "V3", which is the list's naming and not the game's: they
 * are version 2 of a two-version weapon, so the rows below declare two levels
 * and the second one is placed through the same item-array byte every other
 * weapon uses. Those five ids are the list's; the ELF's own +0x46 halfwords
 * could not be read back through the tooling to check them, unlike the ordinary
 * chain above, which was. The Zodiac and the RYNO II have no second version.
 */
struct rac2_item {
	u8 id;       /* the item id, and so the index into every array */
	u8 levels;   /* versions this item has; 1 = no Level slot */
	u8 v[3];     /* item ids of versions 2, 3 and 4, 0 where there is none */
};

static const struct rac2_item rac2_items[] = {
	{  30, 4, {  60,  79,  80 } },   /* Lancer */
	{  42, 4, {  71,  81,  82 } },   /* Gravity Bomb */
	{  22, 4, {  65,  83,  84 } },   /* Chopper */
	{  24, 4, {  67,  85,  86 } },   /* Seeker Gun */
	{  23, 4, {  66,  87,  88 } },   /* Pulse Rifle */
	{  41, 4, {  64,  89,  90 } },   /* Miniturret Glove */
	{  26, 4, {  68,  91,  92 } },   /* Blitz Gun */
	{  45, 4, {  77, 107, 108 } },   /* Shield Charger */
	{  31, 4, {  61,  93,  94 } },   /* Synthenoid */
	{  29, 4, {  63,  95,  96 } },   /* Lava Gun */
	{  37, 4, {  76,  97,  98 } },   /* Bouncer */
	{  27, 4, {  69,  99, 100 } },   /* Minirocket Tube */
	{  28, 4, {  62, 101, 102 } },   /* Plasma Coil */
	{  25, 4, {  70, 103, 104 } },   /* Hoverbomb Gun */
	{  32, 4, {  78, 105, 106 } },   /* Spiderbot Glove */
	{  16, 4, {  72, 109, 110 } },   /* Sheepinator */
	{  18, 2, { 114,   0,   0 } },   /* Tesla Claw */
	{  12, 2, { 115,   0,   0 } },   /* Bomb Glove */
	{  53, 2, { 116,   0,   0 } },   /* Walloper */
	{  14, 2, { 117,   0,   0 } },   /* Visibomb Gun */
	{  17, 2, { 118,   0,   0 } },   /* Decoy Glove */
	{  43, 1, {   0,   0,   0 } },   /* Zodiac */
	{  44, 1, {   0,   0,   0 } },   /* RYNO II */
	{   9, 2, {  73,   0,   0 } },   /* Clank Zapper */

	{  13, 1, {   0,   0,   0 } },   /* Swingshot */
	{  36, 1, {   0,   0,   0 } },   /* Dynamo */
	{  39, 1, {   0,   0,   0 } },   /* Thermanator */
	{  46, 1, {   0,   0,   0 } },   /* Tractor Beam */
	{  55, 1, {   0,   0,   0 } },   /* Hypnomatic */
	{   2, 1, {   0,   0,   0 } },   /* Heli-Pack */
	{   3, 1, {   0,   0,   0 } },   /* Thruster-Pack */
	{  19, 1, {   0,   0,   0 } },   /* Gravity Boots */
	{  20, 1, {   0,   0,   0 } },   /* Grindboots */
	{  54, 1, {   0,   0,   0 } },   /* Charge Boots */

	{  48, 1, {   0,   0,   0 } },   /* Biker Helmet */
	{  21, 1, {   0,   0,   0 } },   /* Glider */
	{  49, 1, {   0,   0,   0 } },   /* Qwark Statuette */
	{   7, 1, {   0,   0,   0 } },   /* Armor Magnetizer */
	{  50, 1, {   0,   0,   0 } },   /* Box Breaker */
	{   5, 1, {   0,   0,   0 } },   /* Mapper */
	{  38, 1, {   0,   0,   0 } },   /* Electrolyzer */
	{  51, 1, {   0,   0,   0 } },   /* Infiltrator */
	{   4, 1, {   0,   0,   0 } },   /* Hydro-Pack */
	{   8, 1, {   0,   0,   0 } }    /* Levitator */
};

#define RAC2_UNLOCK_COUNT ((u8)(sizeof(rac2_unlocks) / sizeof(rac2_unlocks[0])))

/* No RaC2 unlock id is retired, so a row is its own id and the two tables run
 * side by side; both handlers still go through here rather than assume it. */
static int rac2_row_for_id(u8 id)
{
	return id < RAC2_UNLOCK_COUNT ? (int)id : -1;
}

/*
 * Every array is RAC2_ITEM_COUNT entries long and an item id above that is a
 * weapon version, which has no inventory of its own. An address of 0 means the
 * array has no cell for this id; nothing reads or writes one.
 */
static u32 item_owned_addr(u8 id)
{
	return id < RAC2_ITEM_COUNT ? RAC2_OWNED_ARRAY + id : 0;
}

static u32 item_ammo_addr(u8 id)
{
	return id < RAC2_ITEM_COUNT ? RAC2_AMMO_ARRAY + (u32)id * 4u : 0;
}

/* The item id of a version: v1 is the weapon itself, the rest are their own items. */
static u8 item_version_id(const struct rac2_item *it, u32 version)
{
	if (version <= 1 || version > 4) return it->id;
	return it->v[version - 2];
}

/*
 * How the level is decided, confirmed against the NPEA00386 ELF rather than
 * guessed at: RaC2 keeps the same thing RaC3 does, a byte per item id holding
 * the item id of the version in use, and RAC2_ITEM_ARRAY is where it lives.
 *
 * Its own upgrade routine at 0xB25078 writes that byte and nothing else — it
 * takes the next version's id from the weapon's stats record, stores it there,
 * tops the experience word up to the version's threshold and refills the
 * magazine — and its set-version routine at 0xB252CC walks the same chain to a
 * given step. The owned byte is not part of it: a weapon needs its owned byte to
 * be in the inventory at all and this byte to say which version it is, which is
 * exactly the pair RaC3 has.
 *
 * So the level is the step in the row's version chain whose id the byte holds,
 * and writing one writes that version's id. A byte that matches no version of
 * the weapon reads level 0, a number the field never otherwise takes, which is
 * Deadlocked's way of saying "nothing to show".
 */
static u32 item_level(const struct rac2_item *it, const u8 *items)
{
	u32 n;

	for (n = 1; n <= it->levels; n++)
		if (items[it->id] == item_version_id(it, n)) return n;

	return 0;
}

static int item_set_level(const struct rac2_item *it, u32 version)
{
	if (it->levels <= 1) return ST_UNSUPPORTED;

	/* Clamp rather than refuse: the advertised maximum is the game-wide 4. */
	if (version < 1) version = 1;
	if (version > it->levels) version = it->levels;

	/*
	 * Only the version byte, as the game's own two routines do. A weapon put
	 * back on an early version keeps the experience it has earned and will
	 * upgrade itself again at the next kill, which is the game working, not
	 * the write going astray.
	 */
	return mem_write_u8(RAC2_ITEM_ARRAY + it->id, item_version_id(it, version));
}

/*
 * Three reads cover every array this table touches:
 *
 *   A  0x148182C .. 0x148190C   the ammo array, up to the mods array
 *   B  0x1481A80 .. 0x1481AB8   the owned array
 *   C  0x1329A40 .. 0x1329A78   the item array, for the weapon versions
 *
 * Build 34 stopped offering the experience word, so B is the owned array and
 * nothing more: it used to reach past the exp array to cover both in one read.
 *
 * unlock_list fills them and unlock_read serves each row from them; the core
 * calls the two back to back on the tick thread, so nothing goes stale.
 */
#define SNAP_A_ADDR RAC2_AMMO_ARRAY
#define SNAP_A_LEN  (4u * RAC2_ITEM_COUNT)
#define SNAP_B_ADDR RAC2_OWNED_ARRAY
#define SNAP_B_LEN  ((u32)RAC2_ITEM_COUNT)
#define SNAP_C_ADDR RAC2_ITEM_ARRAY
#define SNAP_C_LEN  ((u32)RAC2_ITEM_COUNT)

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

/* The ammo array is the only one of the three that holds words. */
static int snap_word(u32 addr, u32 *out)
{
	if (!g_snap_valid || addr == 0) return 0;

	if (addr >= SNAP_A_ADDR && addr + 4 <= SNAP_A_ADDR + SNAP_A_LEN) {
		*out = be32_get(g_snap_a + (addr - SNAP_A_ADDR));
		return 1;
	}
	return 0;
}

int rac2_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields)
{
	int rc;

	*list        = rac2_unlocks;
	*count       = RAC2_UNLOCK_COUNT;
	*categories  = rac2_categories;
	*ncategories = (u8)(sizeof(rac2_categories) / sizeof(rac2_categories[0]));
	*fields      = rac2_fields;

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

int rac2_unlock_read(const struct game_unlock *entry, u32 values[4])
{
	const struct rac2_item *it;
	int row;
	u8 b = 0;
	u32 w = 0;

	if (entry == NULL) return ST_BAD_ARG;
	row = rac2_row_for_id(entry->id);
	if (row < 0) return ST_BAD_ARG;
	it = &rac2_items[row];

	values[0] = values[1] = values[2] = values[3] = 0;
	if (!g_snap_valid) return ST_OK;

	if (snap_byte(item_owned_addr(it->id), &b)) values[0] = b != 0 ? 1 : 0;

	/* Snapshot C is the item array itself, so it indexes by item id directly. */
	if ((entry->fields & LEVEL) != 0) values[1] = item_level(it, g_snap_c);

	/* Slot 2 is unnamed and no row declares it, so values[2] stays 0. */

	if ((entry->fields & AMMO) != 0 && snap_word(item_ammo_addr(it->id), &w))
		values[3] = w;

	return ST_OK;
}

int rac2_unlock_set(u8 id, u8 field, u32 value)
{
	const struct rac2_item *it;
	int row = rac2_row_for_id(id);

	if (row < 0) return ST_BAD_ARG;
	if (field > 3) return ST_UNSUPPORTED;

	/*
	 * The row's declared fields are the contract, so a slot it does not offer is
	 * refused even where the address behind it exists: a gadget has no version
	 * and no magazine, a weapon with no second version has no level to put it
	 * on, and slot 2 is unnamed, so every row refuses it.
	 */
	if ((rac2_unlocks[row].fields & (u8)(1u << field)) == 0) return ST_UNSUPPORTED;

	it = &rac2_items[row];

	switch (field) {
	case 0:
		/*
		 * The owned byte alone. The version byte beside it is the weapon's own
		 * and the game keeps both across a save, so taking a weapon away and
		 * handing it back leaves it on the version it had.
		 */
		if (item_owned_addr(it->id) == 0) return ST_UNSUPPORTED;
		return mem_write_u8(item_owned_addr(it->id), value != 0 ? 1 : 0);

	case 1:
		return item_set_level(it, value);

	case 3:
		if (item_ammo_addr(it->id) == 0) return ST_UNSUPPORTED;
		return mem_write_u32(item_ammo_addr(it->id), value);

	default:
		/* Slot 2: the gate above has already refused it for every row. */
		return ST_UNSUPPORTED;
	}
}

/* ---------------------------------------------------- the weapon actions */

/*
 * Both walk the weapon rows and pass over a weapon the player has not got,
 * the way Deadlocked's pair do: handing one out is the Owned checkbox's job.
 * One read of the owned array serves the whole walk.
 */
static int rac2_all_max_levels(void)
{
	u8 owned[RAC2_ITEM_COUNT];
	u8 i;
	int rc = mem_read(RAC2_OWNED_ARRAY, owned, sizeof(owned));

	if (rc != ST_OK) return rc;

	for (i = 0; i < RAC2_UNLOCK_COUNT; i++) {
		const struct rac2_item *it = &rac2_items[i];

		if ((rac2_unlocks[i].fields & LEVEL) == 0) continue;
		if (owned[it->id] == 0) continue;

		item_set_level(it, it->levels);
	}

	return ST_OK;
}

/*
 * The magazine a weapon holds is the ammo capacity of the version it is on: the
 * halfword at +0x8A of that version's stats record, which is the same number the
 * game's own upgrade routine refills to. The count itself is kept per BASE
 * weapon id, so a V4 Lancer's rounds still live in ammo[30] and only the
 * capacity comes from the V4 record.
 */
static int rac2_all_max_ammo(void)
{
	u8 owned[RAC2_ITEM_COUNT];
	u8 items[RAC2_ITEM_COUNT];
	u8 i;
	int rc = mem_read(RAC2_OWNED_ARRAY, owned, sizeof(owned));

	if (rc != ST_OK) return rc;
	rc = mem_read(RAC2_ITEM_ARRAY, items, sizeof(items));
	if (rc != ST_OK) return rc;

	for (i = 0; i < RAC2_UNLOCK_COUNT; i++) {
		const struct rac2_item *it = &rac2_items[i];
		u32 level, addr;
		u8 halfword[2];

		if ((rac2_unlocks[i].fields & AMMO) == 0) continue;
		if (owned[it->id] == 0) continue;

		/*
		 * The version the row knows about, not the raw byte: a byte that names
		 * no version of this weapon would index a stats record that has nothing
		 * to do with it, so such a weapon is refilled to its V1 magazine.
		 */
		level = item_level(it, items);
		if (level < 1) level = 1;

		addr = RAC2_STATS_TABLE +
		       (u32)item_version_id(it, level) * RAC2_STATS_STRIDE +
		       RAC2_STATS_CAPACITY;
		if (mem_read(addr, halfword, sizeof(halfword)) != ST_OK) continue;

		addr = item_ammo_addr(it->id);
		if (addr != 0) mem_write_u32(addr, be16_get(halfword));
	}

	return ST_OK;
}

/* ------------------------------------------------------------ level flags */

int rac2_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len)
{
	int rc;

	*len = 0;
	if (planet >= rac2_planet_count()) return ST_BAD_ARG;
	if (cap < RAC2_LF_LEN) return ST_FULL;

	rc = mem_read(RAC2_LEVEL_FLAGS + (u32)planet * RAC2_LF_LEN, out, RAC2_LF_LEN);
	if (rc != ST_OK) return rc;

	*len = RAC2_LF_LEN;
	return ST_OK;
}

int rac2_levelflags_set(u8 planet, u16 offset, u8 value)
{
	if (planet >= rac2_planet_count()) return ST_BAD_ARG;
	if (offset >= RAC2_LF_LEN) return ST_BAD_ARG;

	return mem_write_u8(RAC2_LEVEL_FLAGS + (u32)planet * RAC2_LF_LEN + offset, value);
}

int rac2_levelflags_reset(u8 planet)
{
	if (planet >= rac2_planet_count()) return ST_BAD_ARG;

	/* rac2.cs ResetLevelFlags: sixteen zeros over the destination planet. */
	return mem_write_zeros(RAC2_LEVEL_FLAGS + (u32)planet * RAC2_LF_LEN,
	                       RAC2_LF_LEN);
}

/* ---------------------------------------------------------------- planets */

int rac2_planet_load(u8 planet, u8 flags)
{
	int rc;

	if (planet >= rac2_planet_count()) return ST_BAD_ARG;

	/*
	 * The old load button forced fast loads on before the request and let the
	 * loading-screen watcher put the checkbox setting back when the load
	 * finished. Both halves are kept.
	 */
	rc = rac2_fastload_force();
	if (rc != ST_OK && rc != ST_NOT_INGAME) return rc;

	rc = classic_planet_request(RAC2_LOAD_PLANET, planet);
	if (rc != ST_OK) return rc;

	if (flags & PLANET_FLAG_RESET_LEVELFLAGS) {
		rc = rac2_levelflags_reset(planet);
		if (rc != ST_OK) return rc;
	}

	/*
	 * rac2.cs ResetGoldBolts is empty: RaC2 has no per-planet special bolt to
	 * take back, so PLANET_FLAG_RESET_BOLTS does nothing here. The platinum bolt
	 * array is whole-game and has its own two actions.
	 */
	return ST_OK;
}

/* -------------------------------------------------------------------- die */

int rac2_die(void)
{
	int rc = classic_die_set_z(RAC2_COORDS);

	if (rc != ST_OK) return rc;

	/* rac2.cs SelfDeathExtended, driven by the two toggles. */
	if (rac2_death_bosses()) {
		mem_write_u8(RAC2_BOSS_SIBERIUS, 0);
		mem_write_u8(RAC2_BOSS_SNIVELAK, 0);
	}

	if (rac2_death_pbolts())
		mem_write_zeros(RAC2_PLATINUM_BOLTS, 0x70);

	return ST_OK;
}

/* --------------------------------------------------------------- savefile */

/*
 * rac2.cs loadSetAsideFile forces fast loads on first, exactly as the planet
 * load does, and the loading-screen watcher puts them back afterwards. The
 * request itself goes through src/core/savefile.c, which installs the helper if
 * this process has not had it yet.
 */
int rac2_load_setaside(void)
{
	int rc = savefile_install();

	if (rc != ST_OK) return rc;

	rc = rac2_fastload_force();
	if (rc != ST_OK && rc != ST_NOT_INGAME) return rc;

	return savefile_load_aside();
}

/* -------------------------------------------------------------- the setups */

/* RAC2Form.resetMenuStorage. */
int rac2_reset_menu_storage(void)
{
	u32 shortcut = rac2_shortcut_index();

	if (shortcut != 0xFFFFFFFFu)
		mem_write_u32(RAC2_SHORTCUTS_INDEX, shortcut);

	mem_write_u32(RAC2_SAVED_RACE_INDEX, 0);      /* disable race storage */
	mem_write_u8(RAC2_FELTZIN_OPENING, 1);        /* disable ship openings */
	mem_write_u32(RAC2_GORN_OPENING, 1);
	mem_write_u32(RAC2_FELTZIN_MISSION, 0);       /* fix the ship mission menus */
	mem_write_u32(RAC2_HRUGIS_MISSION, 0);
	mem_write_u32(RAC2_GORN_MISSION, 0);

	return ST_OK;
}

/*
 * RAC2Form.resetAnyPercentVars. The widths matter: the old code wrote some of
 * these as a single byte and others as a 32-bit word, and both forms are kept
 * exactly as they were.
 */
int rac2_reset_anypct(void)
{
	mem_write_u32(RAC2_SAVED_RACE_INDEX, 0);
	mem_write_u8(RAC2_FELTZIN_OPENING, 0);
	mem_write_u32(RAC2_GORN_OPENING, 0);
	mem_write_u32(RAC2_GORN_MANIP, 0);
	mem_write_u32(RAC2_FELTZIN_MISSION, 0);
	mem_write_u32(RAC2_HRUGIS_MISSION, 0);
	mem_write_u32(RAC2_GORN_MISSION, 0);

	mem_write_u32(RAC2_JANKPOT_ACTIVE, 0);
	mem_write_u32(RAC2_BOLT_DEFICIT, 0);

	mem_write_u32(RAC2_ENDAKO_BOSS_CS, 0);
	mem_write_u32(RAC2_PYRAMID_BOLT, 0);

	mem_write_u32(RAC2_OLDSKOOL_SP, 0);
	mem_write_u32(RAC2_BOSS_HEALTHBAR, 0);
	mem_write_u32(RAC2_CS_STORAGE, 0);

	mem_write_u32(RAC2_FELTZIN_RARI, 0);
	mem_write_u32(RAC2_LAST_RARITANIUM, 0);

	mem_write_u8(RAC2_DORBIT_OPENING, 0);
	mem_write_u32(RAC2_MOBY3595_PTR, 0);          /* reset ptr for moby 3595 */

	mem_write_u8(RAC2_SNIV_BOSS, 0);
	mem_write_u8(RAC2_YEEDIL_BOSS, 0);
	mem_write_u8(RAC2_SIB_BOSS, 0);

	/* Pad manip back to 13.0f; Snivelak sets it every time it is visited. */
	mem_write_u32(RAC2_PAD_MANIP, 0x41500000u);

	return ST_OK;
}

/* RAC2Form.SetupGeneralNGPlusMenus, the tail of all three NG+ buttons. */
static int rac2_setup_general_ngplus(void)
{
	mem_write_u8(RAC2_FELTZIN_OPENING, 1);
	mem_write_u8(RAC2_SNIV_BOSS, 20);
	/* 25.0f: Snivelak sets the jump-pad speed, so a NG+ file must have it too. */
	mem_write_u32(RAC2_PAD_MANIP, 0x41C80000u);
	mem_write_u8(RAC2_YEEDIL_BOSS, 66);
	mem_write_u8(RAC2_SIB_BOSS, 20);
	mem_write_u32(RAC2_GORN_MANIP, 1);
	mem_write_u32(RAC2_GORN_OPENING, 1);
	mem_write_u32(RAC2_IM_SHORTCUTS, 1);

	plat_notify("Manips done!");
	return ST_OK;
}

static int rac2_setup_ngplus(u32 shortcut)
{
	rac2_set_shortcut_index(shortcut);
	mem_write_u32(RAC2_SHORTCUTS_INDEX, shortcut);
	return rac2_setup_general_ngplus();
}

static int rac2_setup_all_missions(void)
{
	rac2_set_shortcut_index(7);
	mem_write_u32(RAC2_SHORTCUTS_INDEX, 7);       /* Museum */
	mem_write_u32(RAC2_ENDAKO_BOSS_CS, 1);
	mem_write_u8(RAC2_DORBIT_OPENING, 1);
	return rac2_setup_general_ngplus();
}

static int rac2_maktar_slots(void)
{
	mem_write_u8(RAC2_SLOTS_HIT, 40);
	mem_write_u8(RAC2_PBOLTS, 74);
	mem_write_u8(RAC2_PJACKPOT, 45);
	plat_notify("Maktar slots done!");
	return ST_OK;
}

/* buttonRespawn: the current 32-byte position becomes the respawn point. */
static int rac2_set_respawn(void)
{
	u8 snapshot[32];
	int rc = mem_read(RAC2_COORDS, snapshot, sizeof(snapshot));

	if (rc != ST_OK) return rc;
	return mem_write(RAC2_RESPAWN_COORDS, snapshot, sizeof(snapshot));
}

/* ------------------------------------------------------------ the handlers */

int rac2_set_value(u8 id, u32 value)
{
	switch (id) {
	case R2_BOLTS:      return mem_write_u32(RAC2_BOLTS, value);
	case R2_RARITANIUM: return mem_write_u32(RAC2_RARITANIUM, value);
	case R2_CHALLENGE:  return mem_write_u8(RAC2_CHALLENGE_MODE, (u8)value);
	case R2_HEALTH_XP:  return mem_write_u32(RAC2_HEALTH_XP, value);

	case R2_QE_OFFSET: {
		/*
		 * activateQEToolStripMenuItem asked for a signed 16-bit number and wrote
		 * it big-endian over the two bytes at selectedSaveSlot. The wire carries
		 * a u32, so the low sixteen bits are the offset: 0xFFFF is the -1 the
		 * dialog offered as its default.
		 */
		u8 halfword[2];
		be16_put(halfword, (u16)value);
		return mem_write(RAC2_SAVE_SLOT, halfword, 2);
	}

	case R2_CB_PRIMARY_FRONT:
		return classic_cb_write(RAC2_CB_PRIMARY_FRONT, CLASSIC_CB_ALPHA_FRONT, value);
	case R2_CB_PRIMARY_BACK:
		return classic_cb_write(RAC2_CB_PRIMARY_BACK, CLASSIC_CB_ALPHA_BACK, value);
	case R2_CB_TINT_FRONT:
		/* Tint front and back are the same word on RaC2, so one control covers both. */
		return classic_cb_write(RAC2_CB_TINT_FRONT, CLASSIC_CB_ALPHA_FRONT, value);

	default:            return ST_NOT_FOUND;
	}
}

int rac2_trigger(u8 id)
{
	switch (id) {
	case R2_DIE:             return rac2_die();
	case R2_SET_RESPAWN:     return rac2_set_respawn();

	case R2_STORE_SWINGSHOT:
		/* The Swingshot has weapon id 0x0D. */
		return mem_write_u8(RAC2_PREV_HELD_WEAPON, 0x0D);

	case R2_RESET_ANYPCT:
		rac2_reset_anypct();
		plat_notify("Manips cleared, any% ready!");
		return ST_OK;

	case R2_RESET_MENUS:
		rac2_reset_menu_storage();
		plat_notify("Reset menu storage!");
		return ST_OK;

	case R2_SETUP_NGPLUS:      return rac2_setup_ngplus(7);   /* Museum */
	case R2_SETUP_NO_IMG:      return rac2_setup_ngplus(1);   /* Barlow */
	case R2_SETUP_ALL_MISSION: return rac2_setup_all_missions();
	case R2_MAKTAR_SLOTS:      return rac2_maktar_slots();

	case R2_RESET_PBOLTS:    return mem_write_zeros(RAC2_PLATINUM_BOLTS, 0x70);
	case R2_UNLOCK_PBOLTS:   return mem_write_fill(RAC2_PLATINUM_BOLTS, 0xFF, 0x70);
	case R2_RESET_NANOTECH:  return mem_write_zeros(RAC2_NANOTECH_BOOSTS, 10);
	case R2_UNLOCK_NANOTECH: return mem_write_fill(RAC2_NANOTECH_BOOSTS, 1, 10);
	case R2_RESET_SKILL:     return mem_write_zeros(RAC2_SKILL_POINTS, 30);
	case R2_UNLOCK_SKILL:    return mem_write_fill(RAC2_SKILL_POINTS, 1, 30);

	case R2_MAX_LEVELS:      return rac2_all_max_levels();
	case R2_MAX_AMMO:        return rac2_all_max_ammo();

	case R2_LOAD_ASIDE:      return rac2_load_setaside();
	case R2_SET_ASIDE:       return savefile_set_aside();

	default:                 return ST_NOT_FOUND;
	}
}
