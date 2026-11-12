/*
 * Tools of Destruction (NPEA00452 v01.00): the fact sheet (tod.h).
 *
 * Executable: EBOOT.ELF md5 df9d68ada99b3f44ffe798ce03a1549c, the PSN EU v01.00
 * the user runs. BCES00052 is another executable with every address moved.
 * Sources: REPORT-tod-lua.md section 7, and the Ghidra project's NPEA00452.ELF,
 * re-read for this file on 2026-10-27 where the mark says TOD_DEC or TOD_DIS.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/proto.h"

/*
 * The planets in the game's own LEVEL_ order (the legacy list was off by
 * one), named as the ship's planet selector names them (revision 1.29, the
 * polish): the galactic map's list (gmapHndler in built/anark/galacticmap/
 * built.dat) labels each level with get_location_name(level), the game's
 * text for the planet or place (US English, core_text_tags.us.pkg; the text
 * ids at 0x1006E624, GalMap_locationNameIds), "Planet Kortog" for Stratus
 * City. The word "Planet" is left off, as the user names them; the space
 * combat levels are the places the map names (Voron Asteroid Belt and on).
 * The map lists Fastoon and Fastoon II as one row, "Planet Fastoon", showing
 * whichever is visible (get_location_name gives both the same text); here
 * both can be loaded, so the return visit is "Fastoon II", as the game's
 * enum and speedrunners call it. The level names the map shows under each
 * (Stratus City ...) are in docs/tod-menu.md 15.
 */
static const char * const tod_planets[] = {
	"Kerwan",                  /*  0 LEVEL_METROPOLIS */
	"Cobalia",                 /*  1 LEVEL_COBALIA */
	"Kortog",                  /*  2 LEVEL_STRATUS_CITY */
	"Fastoon",                 /*  3 LEVEL_FASTOON */
	"Voron Asteroid Belt",     /*  4 LEVEL_SPACE_COMBAT_I */
	"Mukow",                   /*  5 LEVEL_IMPERIAL_FIGHT_FEST */
	"Nundac Asteroid Ring",    /*  6 LEVEL_APOGEE_SPACE_STATION */
	"Ardolis",                 /*  7 LEVEL_PIRATE_BASE */
	"Rakar Star Cluster",      /*  8 LEVEL_SPACE_COMBAT_II */
	"Rykan V",                 /*  9 LEVEL_RYKAN_V */
	"Sargasso",                /* 10 LEVEL_SARGASSO */
	"Kreeli Comet",            /* 11 LEVEL_IRIS */
	"Viceron",                 /* 12 LEVEL_ZORDOOM_PRISON */
	"Verdigris Black Hole",    /* 13 LEVEL_SPACE_COMBAT_III */
	"Jasindu",                 /* 14 LEVEL_KERCHU_CITY */
	"Ublik Passage",           /* 15 LEVEL_SLAGS_FLEET */
	"Reepor",                  /* 16 LEVEL_CRAGMITE_RUINS */
	"Igliak",                  /* 17 LEVEL_MERIDIAN_CITY */
	"Fastoon II",              /* 18 LEVEL_FASTOON_II */
};

typedef char tod_planets_all[sizeof(tod_planets) / sizeof(tod_planets[0]) == TOD_NPLANETS ? 1 : -1];

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
	{ 2, FEATURE_ENUM, TOD_ROW_GAMEPLAY, (u8)TOD_NPLANETS,
	  "Load planet",
	  NULL, 0x1006E764u, 4 },
	{ 3, FEATURE_ENUM, TOD_ROW_NEEDS_LUA | TOD_ROW_GAMEPLAY, (u8)TOD_NPLANETS,
	  "Load planet (Lua)",
	  "set_level(%v, true) return get_level()", 0, 0 },
};

/*
 * How each fact is known. The numbers are the ones in tod_facts below; the
 * unit tests hold the two tables to each other. Documentation, so a game
 * module, which has no use for the notes, carries none of them.
 */
#if !QWARK_GAME_MODULE
static const struct tod_mark tod_marks[] = {
	{ "fingerprint",     TOD_FP_ADDR,         TOD_DIS,    "stw r31,0x6e8(r11) in LoadLevel, 0x93EB06E8, never patched" },
	{ "hot block",       TOD_HOT_INPUTS,      TOD_DIS,    "Pad_Update/Input_UpdatePads stores; analogs, edges, mask" },
	{ "pad remap",       TOD_PAD_MASK,        TOD_DIS,    "bit order from the pad routine's mask build; not pressed live" },
	{ "current level",   TOD_CURRENT_LEVEL,   TOD_DEC,    "g_saveData+0x906E8: LoadLevel and LoadLevel_ProcessPending write it after the fade; writing it alone loads nothing (the legacy 0x1029C55B is its low byte)" },
	{ "challenge mode",  TOD_CHALLENGE,       TOD_DEC,    "g_challengeMode, g_saveData+0x906EC, a u32 count: ChallengeMode_getCount/isActive (0x2EA7CC/0x2EA7DC); the legacy byte at 0x1029C55D made it 0x10000" },
	{ "planet request",  TOD_LOAD_REQUEST,    TOD_DEC,    "g_planetLoadRequest: LoadLevel stores the level there while g_gameState is 4; LoadLevel_ProcessPending (from Game_UpdateFrame) fades, loads it and puts -1 back" },
	{ "game state",      TOD_GAME_STATE,      TOD_DIS,    "4 in gameplay (dozens of checks); GameState_Dispatch switches over -2..11, the others unnamed" },
	{ "requested state", TOD_REQUESTED_STATE, TOD_DIS,    "3 = level load (REPORT 7)" },
	{ "quit byte",       TOD_QUIT_BYTE,       TOD_DIS,    "SystemQuitCallback sets 1 before GameState_PopAll and sys_process_exit" },
	{ "position root",   TOD_LEVEL_OBJ,       TOD_DEC,    "[[root]+0xC684] is the hero, written every frame from HeroList_getCurrentHero (callstack_ratchetptr_1); hero+0x14 the moby, pos +0x30; hero+0x1250 prevPos, +0x1260 pos, +0x1270/0x1280 the centre; root 0 between levels" },
	{ "hero health",     TOD_HERO_HEALTH,     TOD_DEC,    "f32 off the hero, max at +0x1788; Hero_checkDeath (0x209E04) enters the death state at <= 0" },
	{ "save data",       TOD_SAVE_DATA,       TOD_DEC,    "g_saveData; the hero's inventory pointer (+0x1A68) points here: hero_get_bolts reads +0x41C through it, hero_set_bolts writes g_saveData+0x41C" },
	{ "bolts",           TOD_BOLTS,           TOD_DEC,    "Lua_hero_set_bolts, Lua_hero_get_bolts; Hero_addBolts caps at 999,999,999" },
	{ "raritanium",      TOD_RARITANIUM,      TOD_DEC,    "Lua_hero_set_minerals; Hero_addMinerals caps at 999,999,999" },
	{ "weapons",         TOD_WEAPONS,         TOD_DEC,    "Inventory_giveWeapon: slot = save + WPN_*0x14, +0x10 owned; 32 slots (ToDWeaponSlot)" },
	{ "weapon fields",   TOD_WEAPONS + 4,     TOD_DEC,    "xp f32 +4, ammo f32 +8, mods u32 +0xC, owned +0x10, level +0x11 (0..9); WeaponSlot_setXp recomputes the level from xp, so a level goes with its xp" },
	{ "weapon unlocked", 0x102115C4u,         TOD_DEC,    "Lua_unlock_weapon: g_saveData[0x5754 + WPN_] = 1" },
	{ "objects",         TOD_OBJECTS,         TOD_DEC,    "Hero_hasObject/Hero_addObject: s32[23] by OBJ_, a count, has = non-zero" },
	{ "gold bolts",      TOD_GOLD_BOLTS,      TOD_DEC,    "LevelSave_collectGoldBolt: a u32 bit mask per level at +0x3EC of its 0x408-byte block; counts per level at 0x1006E58C" },
	{ "skill points",    TOD_SKILL_POINTS,    TOD_DEC,    "SkillPoint_set: a u64, bit i = SKILLPOINT i; the table at 0x1002A180" },
	{ "ryno plans",      TOD_RYNO_PLANS,      TOD_DEC,    "a u32 bit mask; the game's give-all ORs 0x7DEE4" },
	{ "no damage",       TOD_NO_DAMAGE,       TOD_DEC,    "g_debugHeroNoDamage: Hero_Update drops the hero's damage events while non-zero; no code writes it" },
	{ "infinite ammo",   TOD_INFINITE_AMMO,   TOD_DEC,    "g_infiniteAmmo: Weapon_hasAmmo answers true while non-zero; only Lua_hero_set_infinite_ammo writes it; Weapon_useAmmo still counts down" },
	{ "proxy handle",    TOD_PROXY_HANDLE,    TOD_DEC,    "g_heroProxyMobyHandle: Hero_Update drops the hero's damage while non-zero; the old RaCMAN's god ratchet wrote 154 into its low byte, 0x1020BD4B" },
	{ "transition time", TOD_TRANSITION_MIN,  TOD_DEC,    "g_levelTransitionMinTime, f32 5.0 in .data: LevelTransition_waitLoad (0x3C5810) waits that long before the load, only on a load that plays a story FMV (Metropolis or Cobalia while LVL_FAST_SHIP_FIXED is clear); Fast loads writes 0 while on (revision 1.29)" },
	{ "cheats",          TOD_CHEATS,          TOD_DEC,    "Cheats_enableAll: g_levelFlags+0x31D0, CHEAT_COUNT 14 bytes set to 1 (and +0x31E0 = 0x348)" },
	{ "level flags",     TOD_LEVEL_FLAGS,     TOD_DEC,    "g_levelFlags, g_saveData+0x5528: GLOBAL_FLAG_ 0..291 as bit n&63 of u64 word n>>6 (Lua_check_flag, Lua_set_flag, Lua_clear_flag, SetProgressionFlag); tolua_gameplay_open names all 292" },
	{ "lua vm",          TOD_LUA_VM,          TOD_DEC,    "g_gameplayLuaVM; L at +4, allocator at +8 (CLuaVM_executeBuffer)" },
	{ "lua state",       TOD_LUA_STATE,       TOD_DEC,    "0 between levels; a new pointer each level" },
	{ "game clock",      TOD_GAME_CLOCK,      TOD_DEC,    "Lua_time_set_scale writes the f32 at +0; +0x14 is game time" },
	/* Revision 1.28: the autosplitter (tod_autosplit.c). */
	{ "front end",       TOD_FRONT_END,       TOD_DEC,    "g_inFrontEnd: FrontEnd_Enter sets 1 (the old autosplitter's Memory_InitHeaps hook runs in it), FrontEnd_Exit 0; LoadLevel only notes the level while it is up" },
	{ "missions done",   TOD_MISSIONS_DONE,   TOD_DEC,    "g_saveData+0x10AF8 + level*0x7C: Lua_complete_mission moves {label id, desc id, flags|2} there, count at +0x78; the galactic map indexes them by LEVEL_" },
	/* Revision 1.29. */
	{ "load screen",     TOD_LOAD_SCREEN,     TOD_DEC,    "g_loadScreenType, the loading screen's movie: LoadScreen_Init 0 (or a LoadScreen_PickType pick, 2..n-1 never 3, with no level to fly from), LoadScreen_Update a new pick while the load runs, 1 once LoadScreen_LoadDone; the old autosplitter read its low byte, 0x102034FB" },
	{ "Metropolis flag", TOD_LF_METROPOLIS_TACHYON, TOD_INFER, "MOVIE_METROPOLIS_TACHYON, level flag 185: set as the first planet is beaten (seen by the user on a game), clear on a new file" },
	{ "moby pool",       TOD_MOBY_POOL,       TOD_DEC,    "g_mobyPool: ToDMoby slots of 0x180 at +0, the highest index at +0x10; a handle's low half is the index, checked against the slot's +0x64" },
	{ "Tachyon",         TOD_VTBL_TACHYON,    TOD_DEC,    "vtbl_TachyonBoss: TachyonBoss_Init/Update/OnDamage/SetState; health component at +0x20 (TachyonBoss_GetComponent 0x6C), health +0x24; OnDamage clamps at each phase's threshold, and only the killing hit takes it to 0 and SetState(15); read live: health 0.0 of 106192, state 15" },
	{ "credits state",   TOD_STATE_CREDITS,   TOD_DEC,    "g_gameState 11: GameState_RequestCredits (op 2, replace), only from Lua_show_credits" },
	/* Revision 1.29: the run's end at the fade to black. */
	{ "movie player",    TOD_MOVIE_PLAYER,    TOD_DEC,    "g_moviePlayer, ToDMoviePlayer: MoviePlayer_update's state at +0x58, 0 idle, 1 fading out until ScreenFade_GetProgress reads 1.0, 2 playing (g_gameState 9), 3 fading in, 4 done; the movie's index at +0x64" },
	{ "movie queue",     TOD_MOVIE_QUEUE_N,   TOD_DEC,    "g_movieQueue+0x404, the count MovieQueue_push and MovieQueue_pop keep; MovieQueue_update plays the next when the player is done" },
	{ "movie records",   TOD_MOVIE_RECORDS,   TOD_SCAN,   "g_movieRecords, MovieTable_build at each level load: [index] a record whose name is the char* before it; read live on Fastoon II: 6 is l4b_s04_dimx_rt" },
	{ "screen fade",     TOD_FADE_PROGRESS,   TOD_DEC,    "[g_bigObj5B8F0]+0x5B62C, f32: ScreenFade_GetProgress; Level_Enter sets 1.0 and fades in over 1 s" },
	{ "state stack",     TOD_GS_STACK,        TOD_DEC,    "g_gameStateStack, 0x30 an entry, the top at g_gameStateDepth 0x10387DC8: GameState_ApplyPending; Movie_requestPlay puts state 7 and the FMV's record (name at +0) on top" },
	/* Revision 1.29: a level load apart from a death, and the fade in that starts a run. */
	{ "load job",        TOD_LOAD_JOB,        TOD_DIS,    "the tier loader's job: GameState_startTierLoads stores the requested state's ToDGameStateStack*, GameState_tierLoaderThread puts 0 back once the tier stack is that list (0x630794, 0x63081C)" },
	{ "level tiers",     TOD_LOAD_JOB_LEVEL,  TOD_DEC,    "g_gameStateStacks[3], state 3: the core tier, 0x1037AA38, the level transition's 0x1037AB30, g_levelLoadTier, g_levelTierState; RequestLevelLoadState asks GameState_Request(3, 3), Hero_respawnWhenBlack (a death) GameState_Request(3, 1)" },
	{ "level load tier", TOD_LEVEL_LOAD_TIER, TOD_DEC,    "g_levelLoadTier, ToDGameTier: load LevelTransition_waitLoad, enter LevelLoad_onComplete; +6 loaded (GameTier_load, GameTier_unload): 0 through a planet load until the data is in, never touched by a death" },
	{ "fade target",     TOD_FADE_TARGET,     TOD_DEC,    "[g_bigObj5B8F0]+0x5B634, f32: ScreenFade_Set(p) p, ScreenFade_StartIn 0.0, ScreenFade_Start 1.0; Level_Enter does Set(1.0) then StartIn(1.0, 0)" },
	/* 2026-11-12: Remove black screens. */
	{ "fade alpha",      TOD_FADE_ALPHA,      TOD_DEC,    "[g_bigObj5B8F0]+0x5B620, f32, ToDScreenFadeHud.sprite.alpha: ScreenFade_Set and ScreenFade_update (only while the progress moves) write (1 - cos(pi p)) / 2; Sprite_Draw draws no quad under 0.01" },
};
#define TOD_MARKS tod_marks, (u8)(sizeof(tod_marks) / sizeof(tod_marks[0]))
#else
#define TOD_MARKS NULL, 0
#endif

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
	(u8)TOD_NPLANETS, tod_planets,

	0x1037A8E8u,

	TOD_LEVEL_OBJ, TOD_HERO_PTR, TOD_HERO_POS, TOD_POS_BLOB_LEN, TOD_HERO_HEALTH,

	0x1020BE70u, 0x1020C28Cu, 0x1020C290u,
	0x1020BE70u, 0x14, 32, 0x10, 0x11, 0x04, 0x08, 0x0C,
	0x102115C4u,
	0x10214568u, 14,
	TOD_LEVEL_FLAGS,

	0x102043CCu, 0x102043D0u, 0x1074DDA8u,

	tod_rows, (u8)(sizeof(tod_rows) / sizeof(tod_rows[0])),
	TOD_MARKS,
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
