/*
 * Tools of Destruction (NPEA00452 v01.00): the autosplit watcher (revision
 * 1.28, reworked in 1.29; PROTOCOL.md 8.5). The hot block (tod_game.c) hands
 * it the gameplay lua_State, the level, g_gameState, the front end's byte,
 * the load screen's, the tier loader's job and g_levelLoadTier's loaded byte
 * every tick or two; it reads the level flags, the missions, the screen fade
 * and the movie player on the ticks that want them. The quit to the XMB is
 * the title index's pause pair (titles.c), which outlives the module.
 *
 * What the game says, from its executable (Ghidra, DL-ghidra:/NPEA00452.ELF):
 *
 *   a level load        the tier loader's job (0x1073B3A0) is state 3's tier
 *                       list and g_levelLoadTier is not loaded (tod.h). A
 *                       planet load asks for state 3 with its top three tiers
 *                       to go, the level's, its data's and its transition's;
 *                       the data's stays unloaded until the new level's data
 *                       is in. Every planet load does, whichever way it was
 *                       asked for (LoadLevel, the planet request, a save, the
 *                       front end, a challenge mode). Revision 1.29: it
 *                       replaces the lua_State's edges, which a death makes
 *                       too.
 *   a death             the hero's death states fade to black, then ask for
 *                       state 3 with the level tier alone to go
 *                       (Hero_respawnWhenBlack: GameState_Request(3, 1)). So a
 *                       death runs Level_Exit and Level_Enter, a new
 *                       lua_State and a new fade in, but never unloads
 *                       g_levelLoadTier: it is no load, and emits nothing.
 *   a level is up       g_gameplayLuaVM's lua_State: levelInit, in
 *                       Level_Enter's Level_StartGameplay(..., 0), makes it;
 *                       Level_Exit puts it back to 0.
 *   where it goes       g_currentLevel, which LoadLevel and
 *                       LoadLevel_ProcessPending write before they ask for
 *                       the load, so by the load's start it is the
 *                       destination. Lua_set_save_level writes it too, with
 *                       no load, so it is only read at a load's start.
 *   the title           g_inFrontEnd, 1 from FrontEnd_Enter to FrontEnd_Exit.
 *                       A quit to it asks for state 2, no level load.
 *   a new run           MOVIE_METROPOLIS_TACHYON (level flag 185) clear as a
 *                       load into Metropolis starts, from anywhere. The game
 *                       sets it as the first planet is beaten, so a new file
 *                       and a challenge mode (SaveData_prepareChallengeMode
 *                       builds the file from a fresh one) have it clear and
 *                       every later save has it set. The RESET goes out there.
 *   the reveal          the START (revision 1.29): the fade in that shows
 *                       Ratchet. Level_Enter, right after levelInit, puts the
 *                       screen at black (ScreenFade_Set(1.0)) and starts a one
 *                       second fade in (ScreenFade_StartIn: the fade's target
 *                       0.0). So the START is the first tick, with that level
 *                       up after the run's load, that the target reads 0.0.
 *                       A new game's intro FMV plays inside the load, from
 *                       LevelTransition_begin's list, before any of this.
 *   the long load       the old autosplitter's is_loading, exactly: the low
 *                       byte of g_currentLevel 0 and the low byte of
 *                       g_loadScreenType above 1 (tod.h). Game time stops
 *                       while it holds, as it did.
 *   the end of a run    code 7, "Tachyon defeated" (revision 1.29): the screen
 *                       black before l4b_s04_dimx_rt. TachyonEnding_update
 *                       queues that cinematic once Tachyon's death has played
 *                       out; MoviePlayer_update fades to black first and holds
 *                       its state 1 until ScreenFade's progress reads 1.0.
 *   the credits         g_gameState becoming 11: Lua_show_credits ->
 *                       GameState_RequestCredits, the only request for 11.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/mem.h"
#include "../core/util.h"
#include "../core/autosplit.h"

#include <string.h>

#define MISSIONS_EVERY 4u   /* ticks between two looks at the missions */

static int level_ok(s32 level)
{
	return level >= 0 && level < (s32)TOD_NPLANETS;
}

/*
 * The old autosplitter's is_loading (tod-autosplitter-src, ratchetron.c),
 * byte for byte: savePlanetID == 0 && loadScreenType > 1, where the two were
 * the bytes at 0x1029C55B and 0x102034FB, the low bytes of g_currentLevel and
 * g_loadScreenType. Its quirks are kept: only a load whose level reads 0
 * (Metropolis) can stop game time, and a level or a type of 256 or more is
 * judged by its low byte alone.
 */
int tod_long_load(s32 level, u8 screen)
{
	return ((u32)level & 0xFFu) == 0 && screen >= TOD_LONG_LOAD_MIN;
}

/*
 * A load into Metropolis is a new run when the file has not beaten Metropolis
 * yet: MOVIE_METROPOLIS_TACHYON clear. A flag that cannot be read is no run.
 */
static int fresh_file(void)
{
	u8 b = 0;

	if (mem_read_u8(TOD_LEVEL_FLAGS + TOD_LF_BYTE(TOD_LF_METROPOLIS_TACHYON), &b) != ST_OK) return 0;
	return (b & (1u << (TOD_LF_METROPOLIS_TACHYON & 7u))) == 0;
}

void tod_autosplit_forget(struct tod_as_state *as)
{
	memset(as, 0, sizeof(*as));
	as->origin = TOD_AS_NOWHERE;
	as->mission_level = TOD_AS_NOWHERE;
	as->movie_seen = -1;
}

/*
 * The process is going (the session's on_quit, before the quit's PAUSE). The
 * old autosplitter kept one "paused" byte for both its long load and the quit,
 * so a quit during a long load simply stayed paused; here they are two pairs,
 * and the long load is closed in the same moment the quit opens.
 */
void tod_autosplit_quit(struct tod_as_state *as)
{
	if (as->long_load) autosplit_emit(AUTOSPLIT_LOAD_END, TOD_AS_LOAD, 0);
	as->long_load = 0;
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
			autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_MISSION, ((u32)level << 16) | (ids[i] & 0xFFFFu));
		}
	}

	for (i = 0; i < n; i++) as->missions[i] = ids[i];
	as->nmissions = (u8)n;
	as->mission_level = (u32)level;
	as->missions_primed = 1;
}

/*
 * Whether the char* at `ptr_addr` names `want`: the pointer, then the name
 * and its NUL in one read. Anything that cannot be read is no match.
 */
static int name_is(u32 ptr_addr, const char *want)
{
	u8 buf[TOD_MOVIE_NAME_LEN];
	u32 p = 0, n = qstrlen(want) + 1u;

	if (n > sizeof(buf)) return 0;
	if (mem_read_u32(ptr_addr, &p) != ST_OK || p == 0) return 0;
	if (mem_read(p, buf, n) != ST_OK) return 0;
	return memcmp(buf, want, n) == 0;
}

/* A movie player state that is a cinematic under way: 1 fading out .. 4 done. */
static int movie_busy(u32 state)
{
	return state >= 1u && state <= 4u;
}

/*
 * The fade in that shows Ratchet is halfway (revision 1.29, the user's timing
 * of 2026-10-31): the screen fade's target reads 0.0, which ScreenFade_StartIn
 * puts there as Level_Enter starts its one second in from black (rate 1.0
 * from 1.0), and its progress has come down to 0.5 or less, half a second
 * later. The test is "at most", not "equal", so a tick that misses the
 * crossing still sees it, a little late, and a fade the game starts again
 * (Set(1.0), StartIn) is simply waited for again. A -0.0 is 0.0 too, and a
 * negative progress is below 0.5. Nothing that cannot be read is halfway.
 */
static int fade_in_halfway(void)
{
	u8 look[TOD_FADE_LOOK_LEN];
	u32 lo = 0, target, progress;

	if (mem_read_u32(TOD_LEVEL_OBJ, &lo) != ST_OK || lo == 0 || (lo & 3u) != 0) return 0;
	if (mem_read(lo + TOD_FADE_PROGRESS, look, sizeof(look)) != ST_OK) return 0;
	progress = be32_get(look);
	target = be32_get(look + (TOD_FADE_TARGET - TOD_FADE_PROGRESS));
	if ((target & 0x7FFFFFFFu) != 0) return 0;
	return (progress & 0x80000000u) != 0 || progress <= TOD_FADE_HALF;
}

/*
 * The screen black before the ending cinematic, on Fastoon II: code 7,
 * "Tachyon defeated". TachyonEnding_update queues TOD_END_CINEMATIC once
 * Tachyon's death has played out, with a fade to black first:
 * MoviePlayer_update holds it in state 1 until ScreenFade's progress reads
 * 1.0, and only then asks for the cinematic. So code 7 is that test, the
 * same tick the game makes it (or the player already past it, should a tick
 * have missed state 1). A first look that finds it already under way (a
 * module that came in late) learns it.
 */
static void ending_tick(struct tod_as_state *as)
{
	u8 look[TOD_MP_LOOK_LEN];
	int fade_now = 0;

	if (mem_read(TOD_MOVIE_PLAYER + TOD_MP_STATE, look, sizeof(look)) == ST_OK) {
		u32 state = be32_get(look);
		s32 movie = (s32)be32_get(look + (TOD_MP_MOVIE - TOD_MP_STATE));

		if (movie_busy(state) && movie >= 0 && movie < (s32)TOD_NMOVIES) {
			if (movie != as->movie_seen) {
				u32 rec = 0;

				as->movie_is_end = 0;
				if (mem_read_u32(TOD_MOVIE_RECORDS + (u32)movie * 4u, &rec) == ST_OK && rec > 4u)
					as->movie_is_end = (u8)name_is(rec - 4u, TOD_END_CINEMATIC);
				as->movie_seen = movie;
			}
			if (as->movie_is_end) {
				if (state >= 2u) {
					fade_now = 1;
				} else {
					u32 lo = 0, fade = 0;

					if (mem_read_u32(TOD_LEVEL_OBJ, &lo) == ST_OK && lo != 0 &&
					    mem_read_u32(lo + TOD_FADE_PROGRESS, &fade) == ST_OK)
						fade_now = (fade & 0x80000000u) == 0 && fade >= TOD_FADE_BLACK;
				}
			}
		}
	}

	if (!as->end_primed) {
		as->end_primed = 1;
		if (fade_now) as->end_fade_done = 1;
		return;
	}
	if (fade_now && !as->end_fade_done) {
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_END_FADE, 0);
		as->end_fade_done = 1;
	}
}

static void ending_forget(struct tod_as_state *as)
{
	as->end_primed = 0;
	as->end_fade_done = 0;
	as->movie_seen = -1;
	as->movie_is_end = 0;
}

void tod_autosplit_tick(struct tod_as_state *as)
{
	u32 lua = as->cur_lua;
	s32 level = as->cur_level;
	s32 state = as->cur_state;
	u8 front = as->cur_front;
	int loading, load_began, gone, started = 0, long_load;

	as->ticks++;

	/* A planet load under way: the loader on the level's tiers, the level's data out (tod.h). */
	loading = as->cur_job == TOD_LOAD_JOB_LEVEL && as->cur_tier == 0;

	/* A new process, or the module just in: learn where things stand. */
	if (!as->primed) {
		as->primed = 1;
		as->lua = lua;
		as->state = state;
		as->front = front;
		as->loading = (u8)loading;
		as->seen = (lua != 0 || front != 0);
		as->long_load = 0;
		as->origin = (lua != 0 && front == 0 && level_ok(level)) ? (u32)level : TOD_AS_NOWHERE;
		return;
	}

	if (lua != 0 || front != 0) as->seen = 1;

	load_began = loading && !as->loading;
	/* The level gone, or another in its place: a load, or a death, whose lua_State is new. */
	gone = as->lua != 0 && lua != as->lua;

	/*
	 * A run is a load into Metropolis on a file that has not beaten it: out
	 * of the title (a new game, or a Metropolis save made before its end),
	 * out of another level (a challenge mode started from the credits), or
	 * out of Metropolis itself (a new game or such a save loaded while
	 * there). The RESET goes out as the load begins, which is when the runner
	 * threw the last run away; the START waits for the level's fade in,
	 * below. Any other load, or the title coming back, drops a START still
	 * waiting. With the flag set it is an ordinary planet change, below. A
	 * death is no load (revision 1.29): it never gets here.
	 */
	if (load_began) {
		as->start_wait = TOD_SW_NONE;
		if (level == TOD_LEVEL_METROPOLIS && fresh_file()) {
			autosplit_emit(AUTOSPLIT_RESET, 0, 0);
			as->start_wait = TOD_SW_LOADING;
			started = 1;
		}
	}
	if (front != 0 && !loading) as->start_wait = TOD_SW_NONE;
	/* The run's level up: its data in, and Level_Enter's lua_State. */
	if (as->start_wait == TOD_SW_LOADING && !loading && as->cur_tier != 0 && lua != 0 && front == 0) {
		as->start_wait = TOD_SW_LEVEL;
		as->start_lua = lua;
	}
	/* A death before the fade in (a new lua_State, no load) is not the run's level any more. */
	if (as->start_wait == TOD_SW_LEVEL && lua != as->start_lua) as->start_wait = TOD_SW_NONE;

	/*
	 * The long load, the old autosplitter's pause and unpause on the edges of
	 * its is_loading, evaluated once this process has shown its front end or
	 * a level (the old SPRX's STATUS_INGAME began at its Memory_InitHeaps
	 * hook, which runs in FrontEnd_Enter). It sent the edge before the split
	 * in the same poll, and so does this.
	 */
	long_load = as->seen && tod_long_load(level, as->cur_screen);
	if (long_load && !as->long_load) autosplit_emit(AUTOSPLIT_LOAD_START, TOD_AS_LOAD, 0);
	if (!long_load && as->long_load) autosplit_emit(AUTOSPLIT_LOAD_END, TOD_AS_LOAD, 0);
	as->long_load = (u8)long_load;

	/*
	 * A planet change: a load whose destination is not the level the player
	 * was on. A reload of the same planet, a death or a checkpoint (no load),
	 * or a quit to the title (no level load) is not one, and neither is the
	 * load out of the title: the title has no origin. A module that came in
	 * with no level up learns the origin from the first level it sees.
	 */
	if (load_began && !started && level_ok(level) && as->origin != TOD_AS_NOWHERE && (u32)level != as->origin)
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_PLANET, (u32)level);
	if (load_began) as->origin = level_ok(level) ? (u32)level : TOD_AS_NOWHERE;
	if (front != 0 && !loading) as->origin = TOD_AS_NOWHERE;
	if (as->origin == TOD_AS_NOWHERE && lua != 0 && front == 0 && !loading && level_ok(level))
		as->origin = (u32)level;

	/*
	 * The START (revision 1.29): the run's Metropolis is up and its fade in
	 * from black, Level_Enter's ScreenFade_StartIn, which shows Ratchet, is
	 * halfway (2026-10-31: the user's timing, half a second after the fade
	 * in starts). Only a run's load gets here; a death's fade in never does.
	 */
	if (as->start_wait == TOD_SW_LEVEL && front == 0 && level == TOD_LEVEL_METROPOLIS && fade_in_halfway()) {
		autosplit_emit(AUTOSPLIT_START, 0, 0);
		as->start_wait = TOD_SW_NONE;
	}

	/* The credits, from a level (Lua_show_credits), never the title's. */
	if (state == TOD_STATE_CREDITS && as->state != TOD_STATE_CREDITS && front == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_CREDITS, level_ok(level) ? (u32)level : 0);

	/* Missions and the ending, only while a level is up; a new lua_State forgets both. */
	if (lua == 0 || front != 0 || !level_ok(level) || gone) {
		as->missions_primed = 0;
		ending_forget(as);
	} else {
		if ((as->ticks % MISSIONS_EVERY) == 0 || !as->missions_primed) missions_tick(as, level);
		if (level == TOD_LEVEL_FASTOON_II) ending_tick(as);
		else if (as->end_primed) ending_forget(as);
	}

	as->lua = lua;
	as->state = state;
	as->front = front;
	as->loading = (u8)loading;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define NM AUTOSPLIT_FLAG_NORMALISE
#define HD AUTOSPLIT_FLAG_HOLD

/*
 * Code 3 is the title index's pause pair: the old script held game time for
 * the whole quit and reboot and then added 19.95 s, NORMALISE's sum. Code 4
 * is the old autosplitter's long load, game time held for as long as it
 * lasts (HOLD, revision 1.29). Code 7, "Tachyon defeated", the screen black
 * after Tachyon's last hit, is the community's end of a run, on by default.
 * Codes 2 (Tachyon's health reaching 0) and 8 (the ending FMV) were retired
 * in revision 1.29 and are never reused.
 */
static const struct autosplit_desc tod_autosplits[] = {
	{ TOD_AS_PLANET,   AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ TOD_AS_END_FADE, AUTOSPLIT_SPLIT,      DF,      0, "Tachyon defeated" },
	{ TOD_AS_CREDITS,  AUTOSPLIT_SPLIT,      0,       0, "Credits" },
	{ TOD_AS_MISSION,  AUTOSPLIT_SPLIT,      0,       0, "Mission completed" },
	{ TOD_AS_LOAD,     AUTOSPLIT_LOAD_START, DF | HD, 0, "Long load" },
	{ TOD_AS_QUIT,     AUTOSPLIT_PAUSE,      DF | NM, TOD_QUIT_PAUSE_US, "Quit to XMB" }
};

#undef DF
#undef RT
#undef NM
#undef HD

const struct autosplit_desc *tod_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(tod_autosplits) / sizeof(tod_autosplits[0]));
	return tod_autosplits;
}
