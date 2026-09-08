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

#include <string.h>

/* ------------------------------------------------------------ the unlocks */

/*
 * RC2Unlocks.cs in full: three lists of owned bytes and nothing else. The old
 * form had no gold, level or ammo column for RaC2, so every entry declares only
 * field 0.
 *
 * Its allItemAddresses list is the union of the three lists, so "unlock all" and
 * "remove all" are just this table walked end to end; UNLOCK_SET per row gives
 * the client the same reach without another opcode.
 */
#define CAT_WEAPONS 0
#define CAT_GADGETS 1
#define CAT_ITEMS   2

static const char * const rac2_categories[] = { "Weapons", "Gadgets", "Items" };

#define OWNED UNLOCK_FIELD_OWNED

static const struct game_unlock rac2_unlocks[] = {
	/* Weapons */
	{  0, CAT_WEAPONS, OWNED, "Lancer" },
	{  1, CAT_WEAPONS, OWNED, "Gravity-Bomb" },
	{  2, CAT_WEAPONS, OWNED, "Chopper" },
	{  3, CAT_WEAPONS, OWNED, "Seeker-Gun" },
	{  4, CAT_WEAPONS, OWNED, "Pulse-Rifle" },
	{  5, CAT_WEAPONS, OWNED, "Miniturret-Glove" },
	{  6, CAT_WEAPONS, OWNED, "Blitz-Gun" },
	{  7, CAT_WEAPONS, OWNED, "Shield-Charger" },
	{  8, CAT_WEAPONS, OWNED, "Synthenoid" },
	{  9, CAT_WEAPONS, OWNED, "Lava-Gun" },
	{ 10, CAT_WEAPONS, OWNED, "Bouncer" },
	{ 11, CAT_WEAPONS, OWNED, "Minirocket-Tube" },
	{ 12, CAT_WEAPONS, OWNED, "Plasma-Coil" },
	{ 13, CAT_WEAPONS, OWNED, "Hoverbomb-Gun" },
	{ 14, CAT_WEAPONS, OWNED, "Spiderbot-Glove" },
	{ 15, CAT_WEAPONS, OWNED, "Sheepinator" },
	{ 16, CAT_WEAPONS, OWNED, "Tesla-Claw" },
	{ 17, CAT_WEAPONS, OWNED, "Bomb-Glove" },
	{ 18, CAT_WEAPONS, OWNED, "Wolloper" },
	{ 19, CAT_WEAPONS, OWNED, "Visi-bomb-Gun" },
	{ 20, CAT_WEAPONS, OWNED, "Decoy Glove" },
	{ 21, CAT_WEAPONS, OWNED, "Zodiac" },
	{ 22, CAT_WEAPONS, OWNED, "RYNO-II" },
	{ 23, CAT_WEAPONS, OWNED, "Clank-Zapper" },

	/* Gadgets */
	{ 24, CAT_GADGETS, OWNED, "Swingshot" },
	{ 25, CAT_GADGETS, OWNED, "Dynamo" },
	{ 26, CAT_GADGETS, OWNED, "Therminator" },
	{ 27, CAT_GADGETS, OWNED, "Tractor-Beam" },
	{ 28, CAT_GADGETS, OWNED, "Hypnomatic" },
	{ 29, CAT_GADGETS, OWNED, "Heli-Pack" },
	{ 30, CAT_GADGETS, OWNED, "Thruster-Pack" },
	{ 31, CAT_GADGETS, OWNED, "Gravity Boots" },
	{ 32, CAT_GADGETS, OWNED, "Grindboots" },
	{ 33, CAT_GADGETS, OWNED, "Charge Boots" },

	/* Items */
	{ 34, CAT_ITEMS, OWNED, "Biker-Helmet" },
	{ 35, CAT_ITEMS, OWNED, "Glider" },
	{ 36, CAT_ITEMS, OWNED, "Quark-Statuette" },
	{ 37, CAT_ITEMS, OWNED, "Armor-Magnetizer" },
	{ 38, CAT_ITEMS, OWNED, "Box-Breaker" },
	{ 39, CAT_ITEMS, OWNED, "Mapper" },
	{ 40, CAT_ITEMS, OWNED, "Electrolyzer" },
	{ 41, CAT_ITEMS, OWNED, "Infiltrator" },
	{ 42, CAT_ITEMS, OWNED, "HydroPack" },
	{ 43, CAT_ITEMS, OWNED, "Levitator" }
};

#undef OWNED

/* Parallel to rac2_unlocks, id by id: the owned byte from RC2Unlocks.cs. */
static const u32 rac2_unlock_addrs[] = {
	0x1481A9E, 0x1481AAA, 0x1481A96, 0x1481A98, 0x1481A97, 0x1481AA9,
	0x1481A9A, 0x1481AAD, 0x1481A9F, 0x1481A9D, 0x1481AA5, 0x1481A9B,
	0x1481A9C, 0x1481A99, 0x1481AA0, 0x1481A90, 0x1481A92, 0x1481A8C,
	0x1481AB5, 0x1481A8E, 0x1481A91, 0x1481AAB, 0x1481AAC, 0x1481A89,

	0x1481A8D, 0x1481AA4, 0x1481AA7, 0x1481AAE, 0x1481AB7, 0x1481A82,
	0x1481A83, 0x1481A93, 0x1481A94, 0x1481AB6,

	0x1481AB0, 0x1481A95, 0x1481AB1, 0x1481A87, 0x1481AB2, 0x1481A85,
	0x1481AA6, 0x1481AB3, 0x1481A84, 0x1481A88
};

#define RAC2_UNLOCK_COUNT ((u8)(sizeof(rac2_unlocks) / sizeof(rac2_unlocks[0])))

/*
 * Every owned byte lives between 0x1481A82 and 0x1481AB7, so one read serves the
 * whole table. unlock_list fills it and unlock_read serves each row from it; the
 * core calls the pair back to back on the tick thread, so nothing goes stale.
 */
#define SNAP_ADDR RAC2_UNLOCK_BASE
#define SNAP_LEN  0x36u

static u8 g_snap[SNAP_LEN];
static int g_snap_valid;

int rac2_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories)
{
	int rc;

	*list        = rac2_unlocks;
	*count       = RAC2_UNLOCK_COUNT;
	*categories  = rac2_categories;
	*ncategories = (u8)(sizeof(rac2_categories) / sizeof(rac2_categories[0]));

	g_snap_valid = 0;

	rc = mem_read(SNAP_ADDR, g_snap, SNAP_LEN);
	if (rc != ST_OK) return rc;

	g_snap_valid = 1;
	return ST_OK;
}

int rac2_unlock_read(const struct game_unlock *entry, u32 values[4])
{
	u32 addr;

	if (entry == NULL || entry->id >= RAC2_UNLOCK_COUNT) return ST_BAD_ARG;

	values[0] = values[1] = values[2] = values[3] = 0;
	if (!g_snap_valid) return ST_OK;

	addr = rac2_unlock_addrs[entry->id];
	if (addr >= SNAP_ADDR && addr < SNAP_ADDR + SNAP_LEN)
		values[0] = g_snap[addr - SNAP_ADDR];

	return ST_OK;
}

int rac2_unlock_set(u8 id, u8 field, u32 value)
{
	if (id >= RAC2_UNLOCK_COUNT) return ST_BAD_ARG;
	if (field != 0) return ST_UNSUPPORTED;

	return mem_write_u8(rac2_unlock_addrs[id], value != 0 ? 1 : 0);
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
 * The five savefile bytes only mean anything while the savefile helper mod is
 * loaded; without it they are somebody else's memory. The helper byte is read
 * live rather than taken from the readout, because an action is rare and a stale
 * answer here writes into a game that is not listening.
 */
static int savefile_request(u32 addr, u8 value)
{
	u8 present = 0;
	int rc = mem_read_u8(RAC2_SF_HELPER, &present);

	if (rc != ST_OK) return rc;
	if (present != 1) return ST_UNSUPPORTED;

	return mem_write_u8(addr, value);
}

/*
 * rac2.cs loadSetAsideFile forces fast loads on first, exactly as the planet
 * load does, and the loading-screen watcher puts them back afterwards.
 */
int rac2_load_setaside(void)
{
	u8 present = 0;
	int rc = mem_read_u8(RAC2_SF_HELPER, &present);

	if (rc != ST_OK) return rc;
	if (present != 1) return ST_UNSUPPORTED;

	rc = rac2_fastload_force();
	if (rc != ST_OK && rc != ST_NOT_INGAME) return rc;

	return mem_write_u8(RAC2_SF_LOAD_ASIDE, 1);
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

	case R2_LOAD_ASIDE:      return rac2_load_setaside();
	case R2_SET_ASIDE:       return savefile_request(RAC2_SF_SET_ASIDE, 1);
	case R2_MGR_SAVE:        return savefile_request(RAC2_SF_MGR_SAVE, 1);
	case R2_MGR_LOAD:        return savefile_request(RAC2_SF_MGR_LOAD, 1);

	default:                 return ST_NOT_FOUND;
	}
}
