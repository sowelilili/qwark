/*
 * Tools of Destruction (NPEA00452 v01.00): the autosplit watcher (revision
 * 1.28, reworked in 1.29; PROTOCOL.md 8.5). The hot block (tod_game.c) hands
 * it the gameplay lua_State, the level, g_gameState, the front end's byte and
 * the load screen's every tick; it reads the level flags, the missions and
 * Tachyon itself, on the ticks that want them. The quit to the XMB is the
 * title index's pause pair (titles.c), which outlives the module.
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
 *   a new run           MOVIE_METROPOLIS_TACHYON (level flag 185) clear as a
 *                       load into Metropolis starts, from anywhere. The game
 *                       sets it as the first planet is beaten, so a new file
 *                       and a challenge mode (SaveData_prepareChallengeMode
 *                       builds the file from a fresh one) have it clear and
 *                       every later save has it set. The file is in place by
 *                       the load's start, and the RESET goes out there.
 *   control             the START (revision 1.29): Metropolis up, g_gameState
 *                       4 and no realtime cinematic in g_moviePlayer or
 *                       g_movieQueue. Level_Enter makes the lua_State, puts
 *                       the screen at black and starts its 1 s fade in, and
 *                       its Level_StartGameplay asks for state 4, which
 *                       GameState_ApplyPending makes the next frame. A new
 *                       game's intro FMV (l01_s01_ntro_fmv) plays inside the
 *                       load, from LevelTransition_begin's FMV list, so it is
 *                       over (or skipped) before any of that.
 *   the long load       the old autosplitter's is_loading, exactly: the low
 *                       byte of g_currentLevel 0 and the low byte of
 *                       g_loadScreenType above 1 (tod.h). Game time stops
 *                       while it holds, as it did.
 *   Tachyon's defeat    a ToDTachyonBoss's health (vtbl_TachyonBoss, +0x24)
 *                       going from above 0 to 0. TachyonBoss_OnDamage holds it
 *                       at each phase's threshold and lets it reach 0 only on
 *                       the killing hit, in the same call that sets the boss's
 *                       state to 15. Read on a running game (RPCS3, the fight's
 *                       last moment): slot 761, health 0.0 of 106192, state 15.
 *   the end of a run    the screen black before l4b_s04_dimx_rt (revision
 *                       1.29). TachyonEnding_update queues that cinematic,
 *                       then l4b_s05_end1_fmv and l7b_s01_end2_fmv, once
 *                       Tachyon's death has played out; MoviePlayer_update
 *                       fades to black first and holds its state 1 until
 *                       ScreenFade's progress reads 1.0. The FMVs follow the
 *                       whole cinematic, as g_gameState 7 (Movie_requestPlay).
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

static void boss_forget(struct tod_as_state *as)
{
	as->nboss = 0;
	as->scan = 0;
	as->tachyon_done = 0;
	memset(as->boss, 0, sizeof(as->boss));
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

/* A health that counts as alive: a float above 0, compared as bits. */
static int health_alive(u32 bits)
{
	return bits != 0 && (bits & 0x80000000u) == 0;
}

static int boss_known(const struct tod_as_state *as, u32 moby)
{
	u8 i;

	for (i = 0; i < as->nboss; i++) {
		if (as->boss[i].moby == moby) return 1;
	}
	return 0;
}

/*
 * Fastoon II's Tachyons. The pool is walked a few slots a tick, round and
 * round while the level is up, for objects whose vtable is vtbl_TachyonBoss;
 * each one found is then read every tick. One found already dead (a module
 * that came in after the fight) is learnt, not split on. A slot whose object
 * or handle changes has been given to something else, and is forgotten.
 */
static void tachyon_tick(struct tod_as_state *as)
{
	u8 pool[0x14];
	u8 look[TOD_MOBY_LOOK_LEN];
	u32 slots, max, i;
	u8 k;

	if (mem_read(TOD_MOBY_POOL, pool, sizeof(pool)) != ST_OK) return;
	slots = be32_get(pool);
	max = be32_get(pool + TOD_MOBY_POOL_MAX);
	if (slots == 0 || (slots & 3u) != 0 || max >= TOD_MOBY_MAX) return;

	for (k = 0; k < TOD_SCAN_PER_TICK; k++) {
		u32 moby, obj, vt = 0, hp = 0;

		if (as->scan > max) as->scan = 0;
		moby = slots + (u32)as->scan * TOD_MOBY_STRIDE;
		as->scan++;
		if (as->nboss >= TOD_NBOSS || boss_known(as, moby)) continue;
		if (mem_read(moby + TOD_MOBY_OBJECT, look, sizeof(look)) != ST_OK) continue;
		obj = be32_get(look);
		if (obj == 0 || (obj & 3u) != 0) continue;
		if (mem_read_u32(obj, &vt) != ST_OK || vt != TOD_VTBL_TACHYON) continue;
		if (mem_read_u32(obj + TOD_TACHYON_HEALTH, &hp) != ST_OK) continue;
		as->boss[as->nboss].moby = moby;
		as->boss[as->nboss].obj = obj;
		as->boss[as->nboss].handle = be32_get(look + (TOD_MOBY_HANDLE - TOD_MOBY_OBJECT));
		as->boss[as->nboss].alive = (u8)health_alive(hp);
		as->nboss++;
	}

	for (i = 0; i < as->nboss; ) {
		struct tod_boss *b = &as->boss[i];
		u32 hp = 0;
		int alive;

		if (mem_read(b->moby + TOD_MOBY_OBJECT, look, sizeof(look)) != ST_OK) { i++; continue; }
		if (be32_get(look) != b->obj || be32_get(look + (TOD_MOBY_HANDLE - TOD_MOBY_OBJECT)) != b->handle) {
			as->boss[i] = as->boss[as->nboss - 1];
			as->nboss--;
			continue;
		}
		if (mem_read_u32(b->obj + TOD_TACHYON_HEALTH, &hp) != ST_OK) { i++; continue; }
		alive = health_alive(hp);
		if (b->alive && !alive && !as->tachyon_done) {
			autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_TACHYON, 0);
			as->tachyon_done = 1;
		}
		b->alive = (u8)alive;
		i++;
	}
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
 * Ratchet is the player's: the game is in its gameplay state (Level_Enter's
 * Level_StartGameplay asks for state 4 as the level comes up, and every
 * cinematic, FMV, menu and the credits is another state), and no realtime
 * cinematic is playing or waiting to (g_moviePlayer idle, g_movieQueue
 * empty). Nothing that cannot be read is control.
 */
static int control_now(const struct tod_as_state *as)
{
	u8 look[TOD_MP_LOOK_LEN];
	u32 queued = 0;

	if (as->cur_state != TOD_STATE_GAMEPLAY) return 0;
	if (mem_read(TOD_MOVIE_PLAYER + TOD_MP_STATE, look, sizeof(look)) != ST_OK) return 0;
	if (movie_busy(be32_get(look))) return 0;
	if (mem_read_u32(TOD_MOVIE_QUEUE_N, &queued) != ST_OK) return 0;
	return queued == 0;
}

/*
 * The screen black before the ending cinematic, and the ending FMV, on
 * Fastoon II. TachyonEnding_update queues TOD_END_CINEMATIC once Tachyon's
 * death has played out, with a fade to black first: MoviePlayer_update holds
 * it in state 1 until ScreenFade's progress reads 1.0, and only then asks for
 * the cinematic. So code 7 is that test, the same tick the game makes it (or
 * the player already past it, should a tick have missed state 1). The two
 * FMVs follow the cinematic; code 8 is the first of them taking the game
 * state, 7, with its record on top of the state stack. A first look that
 * finds either already under way (a module that came in late) learns it.
 */
static void ending_tick(struct tod_as_state *as)
{
	u8 look[TOD_MP_LOOK_LEN];
	int fade_now = 0, fmv_now = 0;

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

	if (as->cur_state == TOD_STATE_FMV && !as->end_fmv_done) {
		u32 depth = 0, id = 0;

		if (mem_read_u32(TOD_GS_DEPTH, &depth) == ST_OK && depth <= TOD_GS_MAX_DEPTH) {
			u32 entry = TOD_GS_STACK + depth * TOD_GS_STRIDE, rec = 0;

			if (mem_read_u32(entry + TOD_GS_ID, &id) == ST_OK && id == (u32)TOD_STATE_FMV &&
			    mem_read_u32(entry, &rec) == ST_OK && rec != 0)
				fmv_now = name_is(rec, TOD_END_FMV);   /* ToDFmvRecord.name, +0 */
		}
	}

	if (!as->end_primed) {
		as->end_primed = 1;
		if (fade_now) as->end_fade_done = 1;
		if (fmv_now) as->end_fmv_done = 1;
		return;
	}
	if (fade_now && !as->end_fade_done) {
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_END_FADE, 0);
		as->end_fade_done = 1;
	}
	if (fmv_now && !as->end_fmv_done) {
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_END_FMV, 0);
		as->end_fmv_done = 1;
	}
}

static void ending_forget(struct tod_as_state *as)
{
	as->end_primed = 0;
	as->end_fade_done = 0;
	as->end_fmv_done = 0;
	as->movie_seen = -1;
	as->movie_is_end = 0;
}

void tod_autosplit_tick(struct tod_as_state *as)
{
	u32 lua = as->cur_lua;
	s32 level = as->cur_level;
	s32 state = as->cur_state;
	u8 front = as->cur_front;
	int left, entered, front_left, started = 0, long_load;

	as->ticks++;

	/* A new process, or the module just in: learn where things stand. */
	if (!as->primed) {
		as->primed = 1;
		as->lua = lua;
		as->state = state;
		as->front = front;
		as->seen = (lua != 0 || front != 0);
		as->long_load = 0;
		as->origin = (lua != 0 && front == 0 && level_ok(level)) ? (u32)level : TOD_AS_NOWHERE;
		return;
	}

	if (lua != 0 || front != 0) as->seen = 1;

	left       = (as->lua != 0 && lua == 0);      /* Level_Exit: a load has begun */
	entered    = (as->lua == 0 && lua != 0);      /* levelInit: the level is up */
	front_left = (as->front != 0 && front == 0);  /* the title gave way to a load */

	/*
	 * A whole load between two ticks, which Fast loads (the load screen cut
	 * once the data is in, tod.h) makes shorter but nothing seen yet makes
	 * that short: a new
	 * lua_State where the last tick read another. Its split is not lost. The
	 * pool the state comes from is reset each level, so the same pointer back
	 * proves nothing either way.
	 */
	if (as->lua != 0 && lua != 0 && lua != as->lua) left = entered = 1;

	/*
	 * A run is a load into Metropolis on a file that has not beaten it: out
	 * of the title (a new game, or a Metropolis save made before its end),
	 * out of another level (a challenge mode started from the credits), or
	 * out of Metropolis itself (a new game or such a save loaded while
	 * there). The RESET goes out as the load begins, which is when the runner
	 * threw the last run away; the START waits for control (revision 1.29),
	 * below. Any other load, or the title coming back, drops a START still
	 * waiting. With the flag set it is an ordinary planet change, below.
	 */
	if (left || front_left) {
		as->start_wait = TOD_SW_NONE;
		if (level == TOD_LEVEL_METROPOLIS && fresh_file()) {
			autosplit_emit(AUTOSPLIT_RESET, 0, 0);
			as->start_wait = entered ? TOD_SW_LEVEL : TOD_SW_LOADING;
			started = 1;
		}
	}
	if (front != 0) as->start_wait = TOD_SW_NONE;
	if (entered && as->start_wait == TOD_SW_LOADING) as->start_wait = TOD_SW_LEVEL;

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
	 * was on. A reload of the same planet, a death, a checkpoint or a quit to
	 * the title (which loads nothing) is not one, and neither is the load out
	 * of the title: the title has no origin.
	 */
	if (left && !started && level_ok(level) && as->origin != TOD_AS_NOWHERE && (u32)level != as->origin)
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_PLANET, (u32)level);
	if (left) as->origin = level_ok(level) ? (u32)level : TOD_AS_NOWHERE;
	if (entered && front == 0) as->origin = level_ok(level) ? (u32)level : TOD_AS_NOWHERE;
	if (front != 0) as->origin = TOD_AS_NOWHERE;

	/*
	 * Control (revision 1.29): Metropolis is up and the game is in gameplay
	 * with no cinematic, which on a new game is the frame after Level_Enter,
	 * the first of the fade in from black, the intro FMV having played (or
	 * been skipped) inside the load. A death or a checkpoint before it keeps
	 * the START waiting; nothing reloads Metropolis without a new RESET.
	 */
	if (as->start_wait == TOD_SW_LEVEL && lua != 0 && front == 0 &&
	    level == TOD_LEVEL_METROPOLIS && control_now(as)) {
		autosplit_emit(AUTOSPLIT_START, 0, 0);
		as->start_wait = TOD_SW_NONE;
	}

	/* The credits, from a level (Lua_show_credits), never the title's. */
	if (state == TOD_STATE_CREDITS && as->state != TOD_STATE_CREDITS && front == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, TOD_AS_CREDITS, level_ok(level) ? (u32)level : 0);

	/* Missions and Tachyon, only while a level is up; between levels both are forgotten. */
	if (lua == 0 || front != 0 || !level_ok(level) || left) {
		as->missions_primed = 0;
		boss_forget(as);
		ending_forget(as);
	} else {
		if ((as->ticks % MISSIONS_EVERY) == 0 || !as->missions_primed) missions_tick(as, level);
		if (level == TOD_LEVEL_FASTOON_II) {
			tachyon_tick(as);
			ending_tick(as);
		} else {
			if (as->nboss != 0) boss_forget(as);
			if (as->end_primed) ending_forget(as);
		}
	}

	as->lua = lua;
	as->state = state;
	as->front = front;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define NM AUTOSPLIT_FLAG_NORMALISE
#define HD AUTOSPLIT_FLAG_HOLD

/*
 * Code 3 is the title index's pause pair: the old script held game time for
 * the whole quit and reboot and then added 19.95 s, NORMALISE's sum. Code 4
 * is the old autosplitter's long load, game time held for as long as it
 * lasts (HOLD, revision 1.29). Of the final splits, code 7, the screen black
 * after Tachyon's last hit, is the community's end of a run and the one on
 * by default (revision 1.29); Tachyon's health reaching 0 (code 2) is 2.5 s
 * or so before it, and the ending FMV (code 8) after the whole cinematic.
 */
static const struct autosplit_desc tod_autosplits[] = {
	{ TOD_AS_PLANET,   AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ TOD_AS_END_FADE, AUTOSPLIT_SPLIT,      DF,      0, "Final fade to black" },
	{ TOD_AS_TACHYON,  AUTOSPLIT_SPLIT,      0,       0, "Tachyon defeated" },
	{ TOD_AS_END_FMV,  AUTOSPLIT_SPLIT,      0,       0, "Ending FMV" },
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
