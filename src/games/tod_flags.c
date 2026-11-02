/*
 * Tools of Destruction (NPEA00452 v01.00): the level flags, revision 1.28
 * (pending). tod.h has the layout; this file has the three LEVELFLAGS hooks
 * and which planet each flag belongs to, for LEVELFLAGS_RESET.
 *
 * The game keeps one table of progression flags for the whole save, not a
 * region per planet as the four originals do: GLOBAL_FLAG_TEST (0) to
 * GLOBAL_FLAG_COUNT (292), a bit each in five u64 words at g_saveData+0x5528.
 * Lua_check_flag, Lua_set_flag and Lua_clear_flag (the level scripts'
 * check_flag, set_flag, clear_flag) and SetProgressionFlag test and change
 * them, and the hero's code sets its hint flags in word 0 inline
 * (Inventory_giveWeapon HERO_HAS_TWO_ITEMS, the Gelanator's empty hint
 * HERO_GEL_NO_AMMO). Everything here is a data read or write, so it works
 * under RPCS3 as on a console.
 *
 * The flags' names (the game's own, as tolua_gameplay_open registers them,
 * and the old RaCMAN's RaCTrainer/Games/TOD/tod.cs LevelFlags) and their
 * groups are the client's since revision 1.29 (RaCMAN Reloaded's
 * data/levelflags/tod.json; LEVELFLAGS_NAMES, 0x0057, is retired), which made
 * room in tod.sprx's one page. What stays is each flag's group, by number:
 * the legacy viewer's, the longest planet prefix of the name (LVL_ dropped),
 * the LEVEL_ id for a planet and TOD_LF_OTHER for the hints, RYNO messages and
 * items, which no planet's reset clears. The client's file has the same
 * groups, so a planet's Reset clears the flags it lists under that planet.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/mem.h"

#define TOD_LF_OTHER 19u

/* Flag n's group: its planet's LEVEL_ id, or TOD_LF_OTHER. */
static const u8 tod_lf_group[TOD_NLEVEL_FLAGS] = {
	19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,   /*   0.. */
	19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,   /*  16.. */
	19, 19, 19, 19, 19, 19,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,   /*  32.. */
	 1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,   /*  48.. */
	 1,  1,  1,  1,  1,  1,  1,  1,  5,  5,  5,  5,  5,  5,  5,  5,   /*  64.. */
	 5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,   /*  80.. */
	 5,  5,  5,  5,  5,  5,  5,  5,  2,  3,  3,  3,  3,  3,  3,  3,   /*  96.. */
	 3,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  6,   /* 112.. */
	 6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  9,  9,  9,  9,   /* 128.. */
	 9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9, 10, 12, 12, 12, 12,   /* 144.. */
	12, 12, 12, 12, 12, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,   /* 160.. */
	18, 18, 18, 18, 18, 18, 18,  0,  0,  0,  1,  1,  2,  2,  2,  6,   /* 176.. */
	 6,  6,  7,  7,  2,  2,  2,  2,  2,  2,  2,  2,  2, 16, 16, 16,   /* 192.. */
	16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17,  3,  3,  3,   /* 208.. */
	 3,  3,  3, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,   /* 224.. */
	11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 14, 14, 14, 14, 15,   /* 240.. */
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  4,  8, 13,  4, 13, 19,   /* 256.. */
	 7,  7, 19, 19, 19,  7, 13, 11, 11, 11,  9,  9, 19, 19, 19, 19,   /* 272.. */
	 6,  6,  6,  3,   /* 288.. */
};

/* The whole region, whatever the planet: it is the save's, not a planet's. */
int tod_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len)
{
	int rc;

	*len = 0;
	if (planet >= TOD_NPLANETS) return ST_BAD_ARG;
	if (cap < TOD_LEVEL_FLAGS_LEN) return ST_FULL;
	rc = mem_read(TOD_LEVEL_FLAGS, out, TOD_LEVEL_FLAGS_LEN);
	if (rc != ST_OK) return rc;
	*len = TOD_LEVEL_FLAGS_LEN;
	return ST_OK;
}

int tod_levelflags_set(u8 planet, u16 offset, u8 value)
{
	if (planet >= TOD_NPLANETS) return ST_BAD_ARG;
	if (offset >= TOD_LEVEL_FLAGS_LEN) return ST_BAD_ARG;
	return mem_write_u8(TOD_LEVEL_FLAGS + offset, value);
}

/*
 * The flags of `planet`'s group, cleared; every other bit is left as it is.
 * Only the bytes that change are written, a byte at a time, so a flag the
 * game sets in another byte meanwhile is never put back.
 */
int tod_levelflags_reset(u8 planet)
{
	u8 cur[TOD_LEVEL_FLAGS_LEN], clear[TOD_LEVEL_FLAGS_LEN];
	u32 i;
	int rc;

	if (planet >= TOD_NPLANETS) return ST_BAD_ARG;
	for (i = 0; i < TOD_LEVEL_FLAGS_LEN; i++) clear[i] = 0;
	for (i = 0; i < TOD_NLEVEL_FLAGS; i++)
		if (tod_lf_group[i] == planet) clear[TOD_LF_BYTE(i)] |= (u8)(1u << (i & 7u));
	rc = mem_read(TOD_LEVEL_FLAGS, cur, TOD_LEVEL_FLAGS_LEN);
	if (rc != ST_OK) return rc;
	for (i = 0; i < TOD_LEVEL_FLAGS_LEN; i++) {
		if ((cur[i] & clear[i]) == 0) continue;
		rc = mem_write_u8(TOD_LEVEL_FLAGS + i, (u8)(cur[i] & (u8)~clear[i]));
		if (rc != ST_OK) return rc;
	}
	return ST_OK;
}
