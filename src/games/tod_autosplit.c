/*
 * Tools of Destruction (NPEA00452 v01.00): the autosplit watcher (revision
 * 1.28; PROTOCOL.md 8.5). The hot block (tod_game.c) hands it the
 * gameplay lua_State, the level, g_gameState and the front end's byte every
 * tick; it reads the play time and the completed missions itself, on the
 * ticks that want them. The quit to the XMB is the title index's pause pair
 * (titles.c), which outlives the module.
 *
 * What the game says, from its executable (Ghidra, DL-ghidra:/NPEA00452.ELF):
 *
 *   a level is up       g_gameplayLuaVM's lua_State: levelInit, at the end of
 *                       Level_StartGameplay(..., 0) under Level_Enter, makes
 *                       it; Level_Exit's Level_StopGameplay(..., 0) puts it
 *                       back to 0. Every planet load goes through that pair,
 *                       whichever way it was asked for (LoadLevel, the
 *                       planet request, a save, the front end), and nothing
 *                       else writes the word. A death or a checkpoint goes
 *                       through Level_StartGameplay(..., 1), which keeps it.
 *   where it goes       g_currentLevel, which LoadLevel and
 *                       LoadLevel_ProcessPending write before they ask for
 *                       the load state, so by Level_Exit it is the
 *                       destination. Lua_set_save_level writes it too, with
 *                       no load, so it is only read at a load's start.
 *   the title           g_inFrontEnd, 1 from FrontEnd_Enter to FrontEnd_Exit.
 *                       LoadLevel there only notes the level; the load starts
 *                       when the front end gives way.
 *   a new file          g_playTime, which SaveData_init sets to 0 and which
 *                       only gameplay frames add to: a new game and a
 *                       challenge-mode start (SaveData_prepareChallengeMode
 *                       builds the file from a fresh one) both load
 *                       Metropolis with it at 0; a save never has it there.
 *   Tachyon's defeat    Fastoon II's (LEVEL_ 18) completed-mission list
 *                       gaining L19_LABEL_MISSION_DEFEATTACHYON (640), which
 *                       Lua_complete_mission moves there from the open list.
 *   the credits         g_gameState becoming 11: Lua_show_credits ->
 *                       GameState_RequestCredits, the only request for 11.
 *
 * Nothing in here has been read on a running game yet.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/mem.h"
#include "../core/autosplit.h"

#include <string.h>

#define MISSIONS_EVERY 4u   /* ticks between two looks at the missions, but every tick on Fastoon II */

static int level_ok(s32 level)
{
	return level >= 0 && level < (s32)TOD_NPLANETS;
}

/*
 * A new file: less than a second of play time. Compared as bits, which
 * order like the floats for anything not negative.
 */
static int fresh_file(void)
{
	u32 bits = 0;

	if (mem_read_u32(TOD_PLAY_TIME, &bits) != ST_OK) return 0;
	return (bits & 0x80000000u) == 0 && bits < TOD_FRESH_PLAY_TIME;
}

void tod_autosplit_forget(struct tod_as_state *as)
{
	memset(as, 0, sizeof(*as));
	as->origin = TOD_AS_NOWHERE;
	as->mission_level = TOD_AS_NOWHERE;
}

static int mission_known(const struct tod_as_state *as, u32 id)
{
	u8 i;

	for (i = 0; i < as->nmissions; i++) {
		if (as->missions[i] == id) return 1;
	}
	return 0;
}

/*
 * The level's completed missions. The first look at a level only learns
 * them; after that each new one is a split candidate. A save loads in the
 * front end, with no level up, so a whole list arriving at once is never
 * taken for missions just done.
 */
static void missions_tick(struct tod_as_state *as, s32 level)
{
	u8 buf[TOD_MISSION_LIST_LEN];
	u32 ids[TOD_NMISSIONS];
	u32 n, i;

	if (mem_read(TOD_MISSIONS_DONE + (u32)level * TOD_MISSION_LIST_LEN, buf, sizeof(buf)) != ST_OK) return;
	n = be32_get(buf + TOD_MISSION_COUNT);
	if (n > TOD_NMISSIONS) return;   /* not a list: leave what was known alone */
	for (i = 0; i < n; i++) ids[i] = be32_get(buf + i * TOD_MISSION_STRIDE);

	if (as->missions_primed && as->mission_level == (u32)level) {
		for (i = 0; i < n; i++) {
			if (mission_known(as, ids[i])) continue;
			if (level == TOD_LEVEL_FASTOON_II && ids[i] == TOD_MISSION_TACHYON)
				autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_TACHYON, ids[i]);
			autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_MISSION, ((u32)level << 16) | (ids[i] & 0xFFFFu));
		}
	}

	for (i = 0; i < n; i++) as->missions[i] = ids[i];
	as->nmissions = (u8)n;
	as->mission_level = (u32)level;
	as->missions_primed = 1;
}

void tod_autosplit_tick(struct tod_as_state *as)
{
	u32 lua = as->cur_lua;
	s32 level = as->cur_level;
	s32 state = as->cur_state;
	u8 front = as->cur_front;
	int left, entered, front_left, started = 0, loading;

	as->ticks++;

	/* A new process, or the module just in: learn where things stand. */
	if (!as->primed) {
		as->primed = 1;
		as->lua = lua;
		as->state = state;
		as->front = front;
		as->seen = (lua != 0 || front != 0);
		as->loading = 0;
		as->origin = (lua != 0 && front == 0 && level_ok(level)) ? (u32)level : TOD_AS_NOWHERE;
		return;
	}

	if (lua != 0 || front != 0) as->seen = 1;

	left       = (as->lua != 0 && lua == 0);      /* Level_Exit: a load has begun */
	entered    = (as->lua == 0 && lua != 0);      /* levelInit: the level is up */
	front_left = (as->front != 0 && front == 0);  /* the title gave way to a load */

	/*
	 * A whole load between two ticks, which Fast loads (g_levelTransitionMinTime
	 * at 0) makes shorter but nothing seen yet makes that short: a new
	 * lua_State where the last tick read another. Its split is not lost; the
	 * load is under a tick, so there is no time to take off. The pool the
	 * state comes from is reset each level, so the same pointer back proves
	 * nothing either way.
	 */
	if (as->lua != 0 && lua != 0 && lua != as->lua) left = entered = 1;

	/*
	 * A run starts with a load into Metropolis on a new file, out of the
	 * title (a new game) or out of a level (a challenge mode). Both ends of a
	 * script's start and reset, as in the other games.
	 */
	if ((left || front_left) && level == 0 && fresh_file()) {
		autosplit_emit(AUTOSPLIT_RESET, 0, 0);
		autosplit_emit(AUTOSPLIT_START, 0, 0);
		started = 1;
	}

	/*
	 * Loading: no level up, outside the title, once this process has shown
	 * either. A boot's first seconds are the pause pair's, never a load.
	 */
	loading = as->seen && front == 0 && lua == 0;
	if (loading && !as->loading) autosplit_emit(AUTOSPLIT_LOAD_START, TOD_AS_LOAD, level_ok(level) ? (u32)level : 0);
	if (!loading && as->loading) autosplit_emit(AUTOSPLIT_LOAD_END, TOD_AS_LOAD, level_ok(level) ? (u32)level : 0);
	as->loading = (u8)loading;

	/*
	 * A planet change: a load whose destination is not the level the player
	 * was on. A reload of the same planet, a death, a checkpoint or a quit to
	 * the title (which loads nothing) is not one, and neither is the load out
	 * of the title: the title has no origin.
	 */
	if (left && !started && level_ok(level) && as->origin != TOD_AS_NOWHERE && (u32)level != as->origin)
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_PLANET, (u32)level);
	if (left) as->origin = level_ok(level) ? (u32)level : TOD_AS_NOWHERE;
	if (entered && front == 0) as->origin = level_ok(level) ? (u32)level : TOD_AS_NOWHERE;
	if (front != 0) as->origin = TOD_AS_NOWHERE;

	/* The credits, from a level (Lua_show_credits), never the title's. */
	if (state == TOD_STATE_CREDITS && as->state != TOD_STATE_CREDITS && front == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_CREDITS, level_ok(level) ? (u32)level : 0);

	/* Missions, only while a level is up; between levels the list is forgotten. */
	if (lua == 0 || front != 0 || !level_ok(level)) {
		as->missions_primed = 0;
	} else if (level == TOD_LEVEL_FASTOON_II || (as->ticks % MISSIONS_EVERY) == 0 || !as->missions_primed) {
		missions_tick(as, level);
	}

	as->lua = lua;
	as->state = state;
	as->front = front;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define NM AUTOSPLIT_FLAG_NORMALISE

/*
 * Code 3 is the title index's pause pair: the old script held game time for
 * the whole quit and reboot and then added 19.95 s, NORMALISE's sum. Code 4
 * is every level load, removed whole: NORMALISE with no allowance (revision
 * 1.28, lets the parameter be 0 for exactly that). The two final
 * splits are both offered; Tachyon's is the one on by default.
 */
static const struct autosplit_desc tod_autosplits[] = {
	{ TOD_AS_PLANET,  AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ TOD_AS_TACHYON, AUTOSPLIT_SPLIT,      DF,      0, "Tachyon defeated" },
	{ TOD_AS_CREDITS, AUTOSPLIT_SPLIT,      0,       0, "Credits" },
	{ TOD_AS_MISSION, AUTOSPLIT_SPLIT,      0,       0, "Mission completed" },
	{ TOD_AS_LOAD,    AUTOSPLIT_LOAD_START, DF | NM, 0, "Level load" },
	{ TOD_AS_QUIT,    AUTOSPLIT_PAUSE,      DF | NM, TOD_QUIT_PAUSE_US, "Quit to XMB" }
};

#undef DF
#undef RT
#undef NM

const struct autosplit_desc *tod_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(tod_autosplits) / sizeof(tod_autosplits[0]));
	return tod_autosplits;
}
