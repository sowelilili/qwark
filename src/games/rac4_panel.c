/*
 * Ratchet: Deadlocked (NPEA00423): the handlers behind the descriptor table, the
 * planet load and the unlock table.
 *
 * The addresses live in rac4.h. Everything here is a straight port of what
 * RAC4Form and BotsUnlocksFactory did, and where the old code was ambiguous the
 * comment says what was assumed so it can be checked on hardware.
 *
 * All of it runs on the tick thread, through the command ring.
 */
#include "rac4.h"
#include "classic.h"
#include "../core/mem.h"
#include "../core/savefile.h"

#include <string.h>

/* ------------------------------------------------------------ the unlocks */

/*
 * Two categories.
 *
 * BotsUnlocksFactory.GetUpgrades: sixteen bot upgrades, one byte each. rac4.cs
 * writes both the live byte and the saved copy, because an unlock that only
 * lands in the live array is lost the moment the game saves over it.
 *
 * The weapons are g_GadgetData, the thirty-two entry table at RAC4_GADGETS:
 * a level halfword and an ammo halfword at the head of each 68-byte entry,
 * with level 0 meaning locked. Ten of those entries are the weapons the player
 * buys, and they are the rows below; the rest of the table is not a weapon the
 * trainer has any business handing out.
 */
#define CAT_BOTS    0
#define CAT_WEAPONS 1

static const char * const rac4_categories[] = { "Bot upgrades", "Weapons" };

/*
 * Protocol 1.3, the four value slots as Deadlocked uses them. A bot upgrade is
 * one owned byte and declares slot 0 alone; a weapon carries the level and the
 * ammo of its gadget entry as well, and no game of the four has anything to put
 * in slot 3, so it stays unnamed.
 *
 * The level maximum is the challenge-mode V99. A first playthrough stops at
 * V10, but nothing in the entry says which mode the file is in, so the
 * descriptor advertises the number the game itself can reach.
 */
static const struct unlock_field_desc rac4_fields[4] = {
	{ "Owned", UNLOCK_KIND_FLAG,   0 },
	{ "Level", UNLOCK_KIND_NUMBER, RAC4_MAX_LEVEL },
	{ "Ammo",  UNLOCK_KIND_NUMBER, 0 },
	{ NULL,    UNLOCK_KIND_FLAG,   0 }
};

#define OWNED UNLOCK_FIELD_0
#define LEVEL UNLOCK_FIELD_1
#define AMMO  UNLOCK_FIELD_2

/*
 * A bot's id is its index into the sixteen-byte arrays, and a weapon's is 32
 * plus its index into g_GadgetData. Thirty-two is past the last bot for good,
 * so the two halves of the table can never grow into each other, and no row
 * ever has to be renumbered to make room.
 */
static const struct game_unlock rac4_unlocks[] = {
	{  0, CAT_BOTS, OWNED, "Pistol Flux LX" },
	{  1, CAT_BOTS, OWNED, "Range Warrior" },
	{  2, CAT_BOTS, OWNED, "Bogo" },
	{  3, CAT_BOTS, OWNED, "Alpha Ravager" },
	{  4, CAT_BOTS, OWNED, "Beta Ravager" },
	{  5, CAT_BOTS, OWNED, "EMP Grenade" },
	{  6, CAT_BOTS, OWNED, "Hacker Ray" },
	{  7, CAT_BOTS, OWNED, "Shield Link" },
	{  8, CAT_BOTS, OWNED, "Grind Cable" },
	{  9, CAT_BOTS, OWNED, "Go-Comet A" },
	{ 10, CAT_BOTS, OWNED, "Go-Comet X" },
	{ 11, CAT_BOTS, OWNED, "Go-Comet XR" },
	{ 12, CAT_BOTS, OWNED, "Hyper-Tron" },
	{ 13, CAT_BOTS, OWNED, "Dreadinator" },
	{ 14, CAT_BOTS, OWNED, "DZ Ultra" },
	{ 15, CAT_BOTS, OWNED, "Ultranator" },

	{ 34, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Dual Vipers" },
	{ 35, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Magma Cannon" },
	{ 36, CAT_WEAPONS, OWNED | LEVEL | AMMO, "The Arbiter" },
	{ 37, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Fusion Rifle" },
	{ 38, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Hunter Mine Launcher" },
	{ 39, CAT_WEAPONS, OWNED | LEVEL | AMMO, "B6-Obliterator" },
	{ 40, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Holoshield Launcher" },
	{ 41, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Mini-Turret Launcher" },
	{ 42, CAT_WEAPONS, OWNED | LEVEL | AMMO, "The Harbinger" },
	{ 47, CAT_WEAPONS, OWNED | LEVEL | AMMO, "Scorpion Flail" }
};

#define RAC4_UNLOCK_COUNT ((u8)(sizeof(rac4_unlocks) / sizeof(rac4_unlocks[0])))

/*
 * Parallel to the weapon rows above, row for row: the entry each one edits in
 * g_GadgetData. Those indices plus 32 are the ids above, and this table is what
 * the handlers read, so an id stays a wire number and never becomes arithmetic
 * on a game address.
 */
static const u8 rac4_weapon_gadget[] = {
	 2,   /* Dual Vipers */
	 3,   /* Magma Cannon */
	 4,   /* The Arbiter */
	 5,   /* Fusion Rifle */
	 6,   /* Hunter Mine Launcher */
	 7,   /* B6-Obliterator */
	 8,   /* Holoshield Launcher */
	 9,   /* Mini-Turret Launcher */
	10,   /* The Harbinger */
	15    /* Scorpion Flail */
};

#define RAC4_WEAPON_COUNT \
	((u8)(sizeof(rac4_weapon_gadget) / sizeof(rac4_weapon_gadget[0])))

/* The row an id names, or -1 for an id no row carries. */
static int rac4_row_for_id(u8 id)
{
	u8 i;

	for (i = 0; i < RAC4_UNLOCK_COUNT; i++)
		if (rac4_unlocks[i].id == id) return (int)i;

	return -1;
}

/* The gadget entry a row edits, or -1 when the row is a bot upgrade. */
static int rac4_gadget_for_row(int row)
{
	if (row < 0 || (u8)row >= RAC4_UNLOCK_COUNT) return -1;
	if (rac4_unlocks[row].category != CAT_WEAPONS) return -1;

	/* The weapon rows come after the bots, one for each gadget index below. */
	row -= RAC4_BOTS_COUNT;
	if (row < 0 || (u8)row >= RAC4_WEAPON_COUNT) return -1;

	return (int)rac4_weapon_gadget[row];
}

static u32 gadget_addr(int gadget)
{
	return RAC4_GADGETS + (u32)gadget * RAC4_GADGET_STRIDE;
}

/*
 * Two reads cover both categories: the live bot array, which is the one the old
 * form read back, and the whole gadget table. That is 2176 bytes, well inside
 * one PLAT_MEM_MAX read, and static rather than on the tick thread's stack.
 */
static u8 g_snap[RAC4_BOTS_COUNT];
static u8 g_gadgets[RAC4_GADGET_COUNT * RAC4_GADGET_STRIDE];
static int g_snap_valid;

int rac4_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields)
{
	int rc;

	*list        = rac4_unlocks;
	*count       = RAC4_UNLOCK_COUNT;
	*categories  = rac4_categories;
	*ncategories = (u8)(sizeof(rac4_categories) / sizeof(rac4_categories[0]));
	*fields      = rac4_fields;

	g_snap_valid = 0;

	rc = mem_read(RAC4_BOTS_UNLOCK, g_snap, sizeof(g_snap));
	if (rc != ST_OK) return rc;
	rc = mem_read(RAC4_GADGETS, g_gadgets, sizeof(g_gadgets));
	if (rc != ST_OK) return rc;

	g_snap_valid = 1;
	return ST_OK;
}

int rac4_unlock_read(const struct game_unlock *entry, u32 values[4])
{
	int row, gadget;

	if (entry == NULL) return ST_BAD_ARG;
	row = rac4_row_for_id(entry->id);
	if (row < 0) return ST_BAD_ARG;

	values[0] = values[1] = values[2] = values[3] = 0;
	if (!g_snap_valid) return ST_OK;

	gadget = rac4_gadget_for_row(row);
	if (gadget < 0) {
		values[0] = g_snap[rac4_unlocks[row].id];
		return ST_OK;
	}

	{
		const u8 *e = g_gadgets + (u32)gadget * RAC4_GADGET_STRIDE;
		u16 level = be16_get(e);

		/* A weapon is owned when it has a version at all. */
		values[0] = level != 0 ? 1 : 0;
		values[1] = level;
		values[2] = be16_get(e + 2);
	}

	return ST_OK;
}

static int rac4_bot_set(u8 id, u32 value)
{
	u8 on = value != 0 ? 1 : 0;
	int rc = mem_write_u8(RAC4_BOTS_UNLOCK + id, on);

	if (rc != ST_OK) return rc;

	/* Without this the unlock is dropped the next time the game saves. */
	return mem_write_u8(RAC4_BOTS_UNLOCK_SAVE + id, on);
}

/*
 * A weapon's three slots, all of them halfwords at the head of its 68-byte
 * gadget entry, so nothing here ever writes past the level and the ammo.
 *
 * 99 is a hard cap on the level, not a clamp: the game misbehaves above it, so
 * a client asking for more is told no rather than quietly given 99. The ammo
 * has no such ceiling, only the sixteen bits the entry holds, and a wire value
 * past those is clamped the way rac3_unlock_set clamps its own ranges.
 */
static int rac4_weapon_set(int gadget, u8 field, u32 value)
{
	u32 addr = gadget_addr(gadget);
	u8 halfword[2];
	int rc;

	if (field == 0) {
		u16 level;

		/*
		 * Owning a locked weapon hands it V1. A weapon that already has a
		 * version keeps it: the checkbox is there to give the player the
		 * weapon, not to quietly undo the levels it has earned.
		 */
		rc = mem_read(addr, halfword, sizeof(halfword));
		if (rc != ST_OK) return rc;

		level = be16_get(halfword);
		if (value != 0) {
			if (level != 0) return ST_OK;
			be16_put(halfword, 1);
		} else {
			be16_put(halfword, 0);
		}

		return mem_write(addr, halfword, sizeof(halfword));
	}

	if (field == 1) {
		if (value > RAC4_MAX_LEVEL) return ST_BAD_ARG;
		be16_put(halfword, (u16)value);
		return mem_write(addr, halfword, sizeof(halfword));
	}

	if (value > 0xFFFFu) value = 0xFFFFu;
	be16_put(halfword, (u16)value);
	return mem_write(addr + 2, halfword, sizeof(halfword));
}

int rac4_unlock_set(u8 id, u8 field, u32 value)
{
	int row = rac4_row_for_id(id);
	int gadget;

	if (row < 0) return ST_BAD_ARG;
	if (field > 3) return ST_UNSUPPORTED;

	/*
	 * The row's declared fields are the contract: a bot upgrade is one byte
	 * and has no level or ammo to write, and nothing declares slot 3.
	 */
	if ((rac4_unlocks[row].fields & (u8)(1u << field)) == 0) return ST_UNSUPPORTED;

	gadget = rac4_gadget_for_row(row);
	if (gadget < 0) return rac4_bot_set(id, value);

	return rac4_weapon_set(gadget, field, value);
}

/* ---------------------------------------------------- the weapon actions */

/*
 * All three walk the weapon rows and pass over anything the player has not got:
 * a locked weapon reads level 0, and in Deadlocked that zero is the ownership
 * itself, so handing it a level or a magazine would be handing out the weapon.
 * Giving one is the Owned checkbox's job.
 */
static int rac4_all_levels(u16 level)
{
	u8 i;

	for (i = 0; i < RAC4_WEAPON_COUNT; i++) {
		u32 addr = gadget_addr((int)rac4_weapon_gadget[i]);
		u8 halfword[2];

		if (mem_read(addr, halfword, sizeof(halfword)) != ST_OK) continue;
		if (be16_get(halfword) == 0) continue;

		be16_put(halfword, level);
		mem_write(addr, halfword, sizeof(halfword));
	}

	return ST_OK;
}

/*
 * The game's own arithmetic for a full magazine: the gadget's base ammunition,
 * which is one number in single player and another in multiplayer, plus what an
 * ammo mod adds for each ammo mod the entry carries. Everything is read out of
 * the live process, so a mod slot the player changes is accounted for.
 */
static int rac4_weapon_max_ammo(int gadget, const u8 *entry, u32 gametype,
                                u32 per_mod, u16 *out)
{
	u32 off = gametype == RAC4_GAME_TYPE_MP ? RAC4_STATS_AMMO_MP : RAC4_STATS_AMMO_SP;
	u32 addr = RAC4_GADGET_STATS + (u32)gadget * RAC4_STATS_STRIDE + off;
	u8 halfword[2];
	u32 mods = 0;
	u32 k;
	int rc = mem_read(addr, halfword, sizeof(halfword));

	if (rc != ST_OK) return rc;

	for (k = 0; k < RAC4_MOD_SLOTS; k++)
		if (entry[RAC4_MOD_SLOT0 + k * RAC4_MOD_SLOT_SIZE] == RAC4_MOD_AMMO) mods++;

	*out = (u16)(be16_get(halfword) + per_mod * mods);
	return ST_OK;
}

static int rac4_all_max_ammo(void)
{
	u8 per_mod[RAC4_GADGET_COUNT * 4];
	u32 gametype = 0;
	u8 i;
	int rc;

	rc = mem_read_u32(RAC4_GAME_TYPE, &gametype);
	if (rc != ST_OK) return rc;

	/* One read of the per-gadget amounts rather than one per weapon. */
	rc = mem_read(RAC4_AMMO_PER_MOD, per_mod, sizeof(per_mod));
	if (rc != ST_OK) return rc;

	for (i = 0; i < RAC4_WEAPON_COUNT; i++) {
		int gadget = (int)rac4_weapon_gadget[i];
		u8 entry[RAC4_GADGET_STRIDE];
		u8 halfword[2];
		u16 max = 0;

		if (mem_read(gadget_addr(gadget), entry, sizeof(entry)) != ST_OK) continue;
		if (be16_get(entry) == 0) continue;

		if (rac4_weapon_max_ammo(gadget, entry, gametype,
		                         be32_get(per_mod + gadget * 4), &max) != ST_OK)
			continue;

		be16_put(halfword, max);
		mem_write(gadget_addr(gadget) + 2, halfword, sizeof(halfword));
	}

	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

/*
 * Deadlocked does not use the RaC1..RaC3 request word. RAC4Form writes the
 * planet id and then a 1 into a second word, and forces fast loads on first; the
 * game-state watcher in rac4.c puts them back when the load ends.
 */
int rac4_planet_load(u8 planet, u8 flags)
{
	int rc;

	(void)flags;   /* Deadlocked has no level flag region and no special bolts. */

	if (planet == 0 || planet >= rac4_planet_count()) return ST_BAD_ARG;

	rc = rac4_fastload_force();
	if (rc != ST_OK && rc != ST_NOT_INGAME) return rc;

	rc = mem_write_u32(RAC4_TARGET_PLANET, planet);
	if (rc != ST_OK) return rc;

	return mem_write_u32(RAC4_LOAD_PLANET2, 1);
}

/* -------------------------------------------------------------------- die */

int rac4_die(void)
{
	int rc = mem_write_u32(RAC4_COORDS + 8, 0);

	if (rc != ST_OK) return rc;
	return mem_write_u32(RAC4_COORDS2 + 8, 0);
}

/* --------------------------------------------------------------- savefile */

/*
 * Both requests go through src/core/savefile.c, which installs the helper if
 * this process has not had it yet.
 */
int rac4_load_setaside(void)
{
	return savefile_load_aside();
}

/* ----------------------------------------------------------------- skins */

static const char * const rac4_skin_options[RAC4_SKIN_COUNT] = {
	"Marauder",
	"Avenger",
	"Crusader",
	"Vindicator",
	"Liberator",
	"Alpha Clank",
	"Squidzor",
	"Land Shark",
	"The Muscle",
	"W3RM",
	"Starshield",
	"King Claude",
	"Vernon",
	"Kid Nova",
	"Venus",
	"Jak",
	"Ninja",
	"Saurus Ratchet",
	"Genome Ratchet",
	"Santa Ratchet",
	"Pipo Saru Ratchet",
	"Clankchet"
};

int rac4_get_options(u8 id, const char * const **options, u8 *count)
{
	if (id != R4_SKIN) return ST_NOT_FOUND;

	*options = rac4_skin_options;
	*count = RAC4_SKIN_COUNT;
	return ST_OK;
}

/* ------------------------------------------------------------ the setups */

/* RAC4Form.buttonActTune_Click. */
static int rac4_act_tune(void)
{
	mem_write_u8(RAC4_TUNE_SHELLSHOCK, 20);
	mem_write_u8(RAC4_TUNE_REACTOR, 20);
	mem_write_u8(RAC4_TUNE_EVISCERATOR, 20);
	mem_write_u8(RAC4_TUNE_ACE, 20);
	mem_write_u8(RAC4_TUNE_VOX, 20);

	plat_notify("Act tuning done!");
	return ST_OK;
}

/*
 * RAC4Form.unlockPlanetsButton_Click: badges, rank, a million dread points, a
 * load of DreadZone, and every mission on every planet marked complete.
 *
 * The old handler wrote the status byte of each mission one call at a time, 288
 * writes in all. One read-modify-write of each planet's mission array does the
 * same in thirty, which matters when every one of them is a syscall.
 */
static int rac4_unlock_all_planets(void)
{
	static const u8 badges[6] = { 0x00, 0x02, 0x00, 0x04, 0x02, 0x02 };
	u8 missions[RAC4_MISSIONS_DREAD * RAC4_MISSION_SIZE];
	int planet;
	int rc;

	rc = mem_write(RAC4_BADGES, badges, sizeof(badges));
	if (rc != ST_OK) return rc;

	mem_write_u8(RAC4_RANGE, 0x04);            /* Liberator */
	mem_write_u32(RAC4_DREAD_POINTS, 1000000);
	mem_write_u32(RAC4_TARGET_PLANET, 1);
	mem_write_u32(RAC4_LOAD_PLANET2, 1);

	for (planet = 0; planet < RAC4_PLANET_SAVES; planet++) {
		u32 base = RAC4_LEVEL_SAVES + (u32)planet * RAC4_LEVEL_SIZE;
		u32 n = (planet == 0) ? RAC4_MISSIONS_DREAD : RAC4_MISSIONS_OTHER;
		u32 span = n * RAC4_MISSION_SIZE;
		u32 i;

		if (mem_read(base, missions, span) != ST_OK) continue;

		/*
		 * The old code wrote a 2 here, whatever its comment said about 7. Two is
		 * what a completed mission reads as on a real save, so two it is.
		 */
		for (i = 0; i < n; i++)
			missions[i * RAC4_MISSION_SIZE + RAC4_MISSION_STATUS] = 2;

		mem_write(base, missions, span);
	}

	return ST_OK;
}

/*
 * RAC4Form.skinsButton_Click wrote the skin byte, then a 1 at RAC4_SKIN_APPLY,
 * and then killed the player, because outside Dread Station the skin only takes
 * effect on a respawn. The kill is left out here: an ENUM that kills you is a
 * surprise, and the client already has a Die button to press afterwards.
 */
static int rac4_set_skin(u32 skin)
{
	int rc = mem_write_u8(RAC4_SKIN, (u8)skin);

	if (rc != ST_OK) return rc;
	return mem_write_u32(RAC4_SKIN_APPLY, 1);
}

/* ------------------------------------------------------------ the handlers */

int rac4_set_value(u8 id, u32 value)
{
	switch (id) {
	case R4_BOLTS:        return mem_write_u32(RAC4_BOLTS, value);
	case R4_DREAD_POINTS: return mem_write_u32(RAC4_DREAD_POINTS, value);
	case R4_CHALLENGE:    return mem_write_u8(RAC4_CHALLENGE_MODE, (u8)value);
	case R4_SKIN:         return rac4_set_skin(value);
	default:              return ST_NOT_FOUND;
	}
}

int rac4_trigger(u8 id)
{
	switch (id) {
	case R4_DIE:            return rac4_die();
	case R4_UNLOCK_PLANETS: return rac4_unlock_all_planets();
	case R4_ACT_TUNE:       return rac4_act_tune();

	case R4_MAX_LEVELS:     return rac4_all_levels(RAC4_MAX_LEVEL);
	case R4_RESET_LEVELS:   return rac4_all_levels(1);
	case R4_MAX_AMMO:       return rac4_all_max_ammo();
	case R4_SET_ASIDE:      return savefile_set_aside();
	case R4_LOAD_ASIDE:     return savefile_load_aside();
	default:                return ST_NOT_FOUND;
	}
}
