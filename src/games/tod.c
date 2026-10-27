/*
 * Tools of Destruction (NPEA00452 v01.00): the fact sheet (tod.h).
 *
 * Executable: EBOOT.ELF md5 df9d68ada99b3f44ffe798ce03a1549c, the PSN EU v01.00
 * the user runs. BCES00052 is another executable with every address moved.
 * Sources: REPORT-tod-lua.md section 7, and the Ghidra project's NPEA00452.ELF,
 * re-read for this file on 2026-10-27 where the mark says TOD_DEC or TOD_DIS.
 */
#include "tod.h"
#include "../core/proto.h"

/* The game's own LEVEL_ enum, in its order (the legacy list was off by one). */
static const char * const tod_planets[] = {
	"Metropolis",              /*  0 LEVEL_METROPOLIS */
	"Cobalia",                 /*  1 LEVEL_COBALIA */
	"Stratus City",            /*  2 LEVEL_STRATUS_CITY */
	"Fastoon",                 /*  3 LEVEL_FASTOON */
	"Space Combat I",          /*  4 LEVEL_SPACE_COMBAT_I */
	"Imperial Fight Fest",     /*  5 LEVEL_IMPERIAL_FIGHT_FEST */
	"Apogee Space Station",    /*  6 LEVEL_APOGEE_SPACE_STATION */
	"Pirate Base",             /*  7 LEVEL_PIRATE_BASE */
	"Space Combat II",         /*  8 LEVEL_SPACE_COMBAT_II */
	"Rykan V",                 /*  9 LEVEL_RYKAN_V */
	"Sargasso",                /* 10 LEVEL_SARGASSO */
	"Iris",                    /* 11 LEVEL_IRIS */
	"Zordoom Prison",          /* 12 LEVEL_ZORDOOM_PRISON */
	"Space Combat III",        /* 13 LEVEL_SPACE_COMBAT_III */
	"Kerchu City",             /* 14 LEVEL_KERCHU_CITY */
	"Slag's Fleet",            /* 15 LEVEL_SLAGS_FLEET */
	"Cragmite Ruins",          /* 16 LEVEL_CRAGMITE_RUINS */
	"Meridian City",           /* 17 LEVEL_MERIDIAN_CITY */
	"Fastoon II",              /* 18 LEVEL_FASTOON_II */
};

#define TOD_NPLANETS ((u8)(sizeof(tod_planets) / sizeof(tod_planets[0])))

/*
 * The proof of concept's rows (docs/tod-poc.md section 3).
 *
 *   0 Lua test: the first chunk to send. It answers "Lua 5.0.2", which proves
 *     the round trip, and halves the game's time scale, which proves it ran
 *     in the game: everything moves at half speed until row 1.
 *   1 the time scale back to 1.
 *   2 the planet picker as a data write: the option to 0x1006E764, the request
 *     the game polls (LoadLevel_ProcessPending fades, then loads it).
 *   3 the same through Lua, set_level(id, true), the binding level scripts use;
 *     during gameplay it lands in the same word (LoadLevel defers to it).
 */
static const struct tod_row tod_rows[] = {
	{ 0, FEATURE_ACTION, TOD_ROW_NEEDS_LUA | TOD_ROW_GAMEPLAY, 0,
	  "Lua test (version, half speed)",
	  "time_set_scale(0.5) return _VERSION", 0, 0 },
	{ 1, FEATURE_ACTION, TOD_ROW_NEEDS_LUA | TOD_ROW_GAMEPLAY, 0,
	  "Lua: normal speed",
	  "time_set_scale(1) return _VERSION", 0, 0 },
	{ 2, FEATURE_ENUM, TOD_ROW_GAMEPLAY, TOD_NPLANETS,
	  "Load planet",
	  NULL, 0x1006E764u, 4 },
	{ 3, FEATURE_ENUM, TOD_ROW_NEEDS_LUA | TOD_ROW_GAMEPLAY, TOD_NPLANETS,
	  "Load planet (Lua)",
	  "set_level(%v, true) return get_level()", 0, 0 },
};

/*
 * How each fact is known. The numbers are the ones in tod_facts below; the
 * unit tests hold the two tables to each other.
 */
static const struct tod_mark tod_marks[] = {
	{ "fingerprint",     0x002EA5F0u, TOD_DIS,    "stw r31,0x6e8(r11) in LoadLevel, 0x93EB06E8, never patched" },
	{ "hot block",       0x10691918u, TOD_DIS,    "Pad_Update/Input_UpdatePads stores; analogs, edges, mask" },
	{ "pad remap",       0x10691970u, TOD_DIS,    "bit order from the pad routine's mask build; not pressed live" },
	{ "current level",   0x1029C558u, TOD_DEC,    "g_saveData+0x906E8: LoadLevel and LoadLevel_ProcessPending write it after the fade; writing it alone loads nothing (the legacy 0x1029C55B is its low byte)" },
	{ "planet request",  0x1006E764u, TOD_DEC,    "g_planetLoadRequest: LoadLevel stores the level there while g_gameState is 4; LoadLevel_ProcessPending (from Game_UpdateFrame) fades, loads it and puts -1 back" },
	{ "game state",      0x10387F4Cu, TOD_INFER,  "LoadLevel defers to the request when it is 4; read live to name the other values" },
	{ "requested state", 0x100FA21Cu, TOD_DIS,    "3 = level load (REPORT 7)" },
	{ "quit byte",       0x1037A8E8u, TOD_DIS,    "SystemQuitCallback sets 1 before GameState_PopAll and sys_process_exit" },
	{ "position root",   0x10740BB4u, TOD_LEGACY, "[[root]+0xC684]+0x1260, 64 bytes; health +0x1784; root 0 between levels (dec)" },
	{ "save data",       0x1020BE70u, TOD_DEC,    "g_saveData; the hero's inventory pointer (+0x1A68) points here: hero_get_bolts reads +0x41C through it, hero_set_bolts writes g_saveData+0x41C" },
	{ "bolts",           0x1020C28Cu, TOD_DEC,    "Lua_hero_set_bolts, Lua_hero_get_bolts" },
	{ "raritanium",      0x1020C290u, TOD_DEC,    "Lua_hero_set_minerals" },
	{ "weapons",         0x1020BE70u, TOD_DEC,    "Inventory_giveWeapon: slot = save + WPN_*0x14, +0x10 owned; 32 slots; the legacy XP/ammo/mods/level offsets fit the same slots" },
	{ "weapon fields",   0x1020BE74u, TOD_LEGACY, "xp +4, ammo +8, mods +0xE, level +0x11 from the legacy forms, which index from Combuster = 0 (WPN_ 1)" },
	{ "weapon unlocked", 0x102115C4u, TOD_DEC,    "Lua_unlock_weapon: g_saveData[0x5754 + WPN_] = 1" },
	{ "cheats",          0x10214568u, TOD_DEC,    "Cheats_enableAll: g_levelFlags+0x31D0, CHEAT_COUNT 14 bytes set to 1 (and +0x31E0 = 0x348)" },
	{ "level flags",     0x10211398u, TOD_SCAN,   "g_saveData+0x5528" },
	{ "lua vm",          0x102043CCu, TOD_DEC,    "g_gameplayLuaVM; L at +4, allocator at +8 (CLuaVM_executeBuffer)" },
	{ "lua state",       0x102043D0u, TOD_DEC,    "0 between levels; a new pointer each level" },
	{ "game clock",      0x1074DDA8u, TOD_DEC,    "Lua_time_set_scale writes the f32 at +0; +0x14 is game time" },
};

const struct tod_facts tod_facts = {
	TOD_TITLE,

	0x002EA5F0u, 0x93EB06E8u,

	0x10691918u, 0x5C,
	0x00, 0x50, 0x54, 0x58,

	/*
	 * The game's bit i -> the OG layout's bit. Game: Triangle 0x1, Circle 0x2,
	 * Cross 0x4, Square 0x8, L2 0x10, R2 0x20, L1 0x40, R1 0x80, Select 0x100,
	 * Start 0x200, L3 0x400, R3 0x800, then Up, Right, Down, Left. OG: L2 0x1,
	 * R2 0x2, L1 0x4, R1 0x8, Triangle 0x10, Circle 0x20, Cross 0x40, Square
	 * 0x80, Select 0x100, L3 0x200, R3 0x400, Start 0x800, the same d-pad.
	 */
	{ 4, 5, 6, 7, 0, 1, 2, 3, 8, 11, 9, 10, 12, 13, 14, 15 },

	0x1029C558u, 0x1006E764u, 0x10387F4Cu, 0x100FA21Cu,
	TOD_NPLANETS, tod_planets,

	0x1037A8E8u,

	0x10740BB4u, 0x0000C684u, 0x00001260u, 64, 0x00001784u,

	0x1020BE70u, 0x1020C28Cu, 0x1020C290u,
	0x1020BE70u, 0x14, 32, 0x10, 0x11, 0x04, 0x08, 0x0E,
	0x102115C4u,
	0x10214568u, 14,
	0x10211398u,

	0x102043CCu, 0x102043D0u, 0x1074DDA8u,

	tod_rows, (u8)(sizeof(tod_rows) / sizeof(tod_rows[0])),
	tod_marks, (u8)(sizeof(tod_marks) / sizeof(tod_marks[0])),
};

u32 tod_pad_to_og(u32 tod_mask)
{
	u32 og = 0;
	u32 i;

	for (i = 0; i < 16; i++) {
		if (tod_mask & (1u << i)) og |= 1u << tod_facts.pad_remap[i];
	}
	return og;
}

int tod_row_chunk(const struct tod_row *row, u32 value, char *dst, u32 cap)
{
	const char *s;
	u32 n = 0;

	if (row == NULL || row->lua == NULL || cap == 0) return -1;
	for (s = row->lua; *s != 0; s++) {
		if (s[0] == '%' && s[1] == 'v') {
			char digits[10];
			u32 nd = 0, v = value;

			do {
				digits[nd++] = (char)('0' + v % 10u);
				v /= 10u;
			} while (v != 0 && nd < sizeof(digits));
			while (nd > 0) {
				if (n + 1 >= cap) return -1;
				dst[n++] = digits[--nd];
			}
			s++;
			continue;
		}
		if (n + 1 >= cap) return -1;
		dst[n++] = *s;
	}
	dst[n] = 0;
	return (int)n;
}
