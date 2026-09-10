/*
 * Ratchet: Deadlocked (NPEA00423): the handlers behind the descriptor table, the
 * planet load and the bot unlocks.
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
 * BotsUnlocksFactory.GetUpgrades: sixteen bot upgrades, one byte each. rac4.cs
 * writes both the live byte and the saved copy, because an unlock that only
 * lands in the live array is lost the moment the game saves over it.
 */
#define CAT_BOTS 0

static const char * const rac4_categories[] = { "Bot upgrades" };

/*
 * Protocol 1.3. A bot upgrade is one owned byte, so only slot 0 is named.
 * Deadlocked's weapons are not in this table at all, so there is no level, no
 * XP and no ammo column to describe.
 */
static const struct unlock_field_desc rac4_fields[4] = {
	{ "Owned", UNLOCK_KIND_FLAG, 0 },
	{ NULL,    UNLOCK_KIND_FLAG, 0 },
	{ NULL,    UNLOCK_KIND_FLAG, 0 },
	{ NULL,    UNLOCK_KIND_FLAG, 0 }
};

static const struct game_unlock rac4_unlocks[] = {
	{  0, CAT_BOTS, UNLOCK_FIELD_OWNED, "Pistol Flux LX" },
	{  1, CAT_BOTS, UNLOCK_FIELD_OWNED, "Range Warrior" },
	{  2, CAT_BOTS, UNLOCK_FIELD_OWNED, "Bogo" },
	{  3, CAT_BOTS, UNLOCK_FIELD_OWNED, "Alpha Ravager" },
	{  4, CAT_BOTS, UNLOCK_FIELD_OWNED, "Beta Ravager" },
	{  5, CAT_BOTS, UNLOCK_FIELD_OWNED, "EMP Grenade" },
	{  6, CAT_BOTS, UNLOCK_FIELD_OWNED, "Hacker Ray" },
	{  7, CAT_BOTS, UNLOCK_FIELD_OWNED, "Shield Link" },
	{  8, CAT_BOTS, UNLOCK_FIELD_OWNED, "Grind Cable" },
	{  9, CAT_BOTS, UNLOCK_FIELD_OWNED, "Go-Comet A" },
	{ 10, CAT_BOTS, UNLOCK_FIELD_OWNED, "Go-Comet X" },
	{ 11, CAT_BOTS, UNLOCK_FIELD_OWNED, "Go-Comet XR" },
	{ 12, CAT_BOTS, UNLOCK_FIELD_OWNED, "Hyper-Tron" },
	{ 13, CAT_BOTS, UNLOCK_FIELD_OWNED, "Dreadinator" },
	{ 14, CAT_BOTS, UNLOCK_FIELD_OWNED, "DZ Ultra" },
	{ 15, CAT_BOTS, UNLOCK_FIELD_OWNED, "Ultranator" }
};

#define RAC4_UNLOCK_COUNT ((u8)(sizeof(rac4_unlocks) / sizeof(rac4_unlocks[0])))

/* The live array is the one the old form read back, so one read serves the list. */
static u8 g_snap[RAC4_BOTS_COUNT];
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

	g_snap_valid = 1;
	return ST_OK;
}

int rac4_unlock_read(const struct game_unlock *entry, u32 values[4])
{
	if (entry == NULL || entry->id >= RAC4_UNLOCK_COUNT) return ST_BAD_ARG;

	values[0] = values[1] = values[2] = values[3] = 0;
	if (g_snap_valid) values[0] = g_snap[entry->id];

	return ST_OK;
}

int rac4_unlock_set(u8 id, u8 field, u32 value)
{
	u8 on;
	int rc;

	if (id >= RAC4_UNLOCK_COUNT) return ST_BAD_ARG;
	if (field != 0) return ST_UNSUPPORTED;

	on = value != 0 ? 1 : 0;

	rc = mem_write_u8(RAC4_BOTS_UNLOCK + id, on);
	if (rc != ST_OK) return rc;

	/* Without this the unlock is dropped the next time the game saves. */
	return mem_write_u8(RAC4_BOTS_UNLOCK_SAVE + id, on);
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
 * load of Dread Zone, and every mission on every planet marked complete.
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
	case R4_SET_ASIDE:      return savefile_set_aside();
	case R4_LOAD_ASIDE:     return savefile_load_aside();
	default:                return ST_NOT_FOUND;
	}
}
