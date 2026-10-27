/*
 * Tools of Destruction (NPEA00452 v01.00): the savefile manager, revision 1.28
 * (pending). docs/tod-savefile.md has the reverse engineering and the design.
 *
 * The four original games need a helper of qwark's in the game, because their
 * save data sits in a buffer only the game's own code can be asked to fill or
 * load. Tools of Destruction needs none. Its save system (ToDSaveManager,
 * g_saveManager) keeps the image of the last GAME.SAV it wrote or read in a
 * buffer of its own, and it loads that buffer, on its own main thread at the
 * end of a frame, as soon as one of its words says a load thread has finished
 * reading a file. So:
 *
 *   - the aside buffer is that image (TOD_SAVE_IMAGE), 0x906F0 bytes, and a
 *     set-aside has nothing to copy: the buffer already is the game's last
 *     save, exactly as the other games' set-aside takes their last save;
 *   - a load is four data writes that make the save system's update find a
 *     load thread that has read its file and gone (load_from_hold below), and
 *     the game then does what it does after a load from its own menu:
 *     SaveManager_onLoaded copies the image into g_saveData and calls
 *     LoadLevel(level, 1, 0);
 *   - while qwark copies a file into the buffer or out of it, the save system
 *     is held in a state of qwark's own, which its update and its request
 *     both leave alone, so no autosave rewrites the buffer under the copy.
 *
 * All of it is data, so it works on a console and under RPCS3 alike, with no
 * install, no ui.pak entry and no RPCS3 patch. src/core/savefile.c drives it
 * through struct sf_driver (savefile.h).
 *
 * The set-aside itself lives on the HDD, <QWARK_SAVEDIR>/NPEA00452/
 * set-aside.sav, because the game's next autosave writes over its buffer: the
 * Set aside action is a STORE into that file and Load set-aside a RESTORE of
 * it (aside_file below), so a set-aside survives autosaves and reboots.
 *
 * Tick thread throughout, through the command ring and savefile_tick. No state
 * of its own: the hold and the load are read back out of the game every time.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "sfhelper_bins.h"
#include "../core/mem.h"
#include "../core/savefile.h"
#include "../core/util.h"

/*
 * How long a load may wait for the game's update to take it before it is put
 * back: five seconds of the 120 Hz tick. The update runs every frame in every
 * game state, so a load the game takes at all is taken within a frame or two;
 * one that is still waiting here means sys_ppu_thread_get_priority did not
 * answer ESRCH for TOD_THREAD_GONE (docs/tod-savefile.md, the live checks).
 */
#define TOD_LOAD_WAIT_TICKS 600u

/* -------------------------------------------------------- the save system */

/* A word of the manager's head as tod_ss_read left it. */
static u32 ss_word(const u8 *h, u32 off)
{
	return be32_get(h + off);
}

/* Whether one of the three worker threads has none running. */
static int ss_no_thread(const u8 *h, u32 thread)
{
	return ss_word(h, thread + TOD_ST_ID) == 0 && ss_word(h, thread + TOD_ST_ID + 4) == TOD_THREAD_NONE;
}

/*
 * The manager's head, TOD_SM_HEAD_LEN bytes, into `h`: 1 when the game's save
 * system is the one this table describes (g_saveSystem points at it and its
 * vtable is the manager's), 0 when a read failed or it is not.
 */
static int ss_read(u8 *h)
{
	u32 sys = 0;

	if (mem_read_u32(TOD_SAVE_SYSTEM, &sys) != ST_OK || sys != TOD_SAVE_MANAGER) return 0;
	if (mem_read(TOD_SAVE_MANAGER, h, TOD_SM_HEAD_LEN) != ST_OK) return 0;
	return ss_word(h, 0) == TOD_SAVE_VTABLE;
}

/*
 * Idle: no save, load or delete under way and none waiting. In qwark's hold
 * the autosave the game asked for meanwhile is not in the way: a load drops it
 * (load_from_hold) and a STORE's release lets it go ahead.
 */
static int ss_idle(const u8 *h)
{
	u32 st = ss_word(h, TOD_SM_STATE);
	int held = st == TOD_SS_HELD;

	if (!held && st != TOD_SS_IDLE) return 0;
	if (!ss_no_thread(h, TOD_SM_SAVE_THREAD) || !ss_no_thread(h, TOD_SM_LOAD_THREAD) ||
	    !ss_no_thread(h, TOD_SM_DELETE_THREAD)) return 0;
	return held || ss_word(h, TOD_SM_AUTOSAVE) == 0;
}

static u8 tod_sf_holding(void)
{
	u8 h[TOD_SM_HEAD_LEN];

	if (!ss_read(h)) return 0;
	return ss_word(h, TOD_SM_STATE) == TOD_SS_HELD && ss_idle(h);
}

/*
 * The hold. SaveSystem_request asks for state 1 and does nothing in any other
 * (an autosave is noted in TOD_SM_AUTOSAVE for later), and SaveSystem_update
 * runs nothing for a state past 4, so TOD_SS_HELD keeps the game's saves and
 * loads off the buffer until it goes back to 1.
 *
 * The game's request runs on whichever thread asked (the menu's, for the save
 * and load screens), so it can read state 1 just before qwark writes the hold
 * and start its thread just after. The look after the write catches a request
 * that got that far; one caught later still is what holding() is for, which
 * the copy asks on every tick.
 */
static u8 tod_sf_hold(void)
{
	u8 h[TOD_SM_HEAD_LEN];

	if (!ss_read(h) || !ss_idle(h)) return SAVEFILE_ERR_NOT_IDLE;
	if (ss_word(h, TOD_SM_STATE) == TOD_SS_HELD) return SAVEFILE_ERR_NONE;

	if (mem_write_u32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_HELD) != ST_OK) return SAVEFILE_ERR_NOT_IDLE;
	return tod_sf_holding() ? SAVEFILE_ERR_NONE : SAVEFILE_ERR_NOT_IDLE;
}

static void tod_sf_release(void)
{
	u32 st = 0;

	if (mem_read_u32(TOD_SAVE_MANAGER + TOD_SM_STATE, &st) != ST_OK) return;
	if (st == TOD_SS_HELD) mem_write_u32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_IDLE);
}

/*
 * The load, from the hold: the load thread reads as one that has read its
 * file (state 5) and is gone (an id no thread has), the manager as loading.
 * The update's LoadThread_poll then finds sys_ppu_thread_get_priority answering
 * ESRCH for the id, calls SaveManager_onLoaded, and puts the id back to none;
 * the update itself goes back to idle in the same call. `lastResult` 0 keeps
 * SaveManager_onOperationDone out of it, and the autosave the game may have
 * asked for during the hold is dropped, so the loaded save is not written over
 * the user's slot the moment it is in. The manager's state goes last.
 */
static u8 tod_sf_load(void)
{
	static const u8 gone[8] = { 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFE };
	const u32 lt = TOD_SAVE_MANAGER + TOD_SM_LOAD_THREAD;

	if (!tod_sf_holding()) return SAVEFILE_ERR_NOT_IDLE;

	if (mem_write_u32(lt + TOD_ST_STATE, TOD_LT_READ_OK) != ST_OK ||
	    mem_write(lt + TOD_ST_ID, gone, sizeof(gone)) != ST_OK ||
	    mem_write_u32(TOD_SAVE_MANAGER + TOD_SM_RESULT, 0) != ST_OK ||
	    mem_write_u32(TOD_SAVE_MANAGER + TOD_SM_AUTOSAVE, 0) != ST_OK ||
	    mem_write_u32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_LOADING) != ST_OK) {
		/* Nothing goes to the game half set up: the thread as it was, the hold kept. */
		mem_write_u32(lt + TOD_ST_STATE, 0);
		mem_write_u32(lt + TOD_ST_ID + 4, TOD_THREAD_NONE);
		return SAVEFILE_ERR_NOT_IDLE;
	}
	return SAVEFILE_ERR_NONE;
}

/* The load thread back to none and nothing read: the update then goes idle. */
static void load_put_back(void)
{
	const u32 lt = TOD_SAVE_MANAGER + TOD_SM_LOAD_THREAD;

	mem_write_u32(lt + TOD_ST_ID + 4, TOD_THREAD_NONE);
	mem_write_u32(lt + TOD_ST_ID, 0);
	mem_write_u32(lt + TOD_ST_STATE, 0);
}

static u8 tod_sf_load_poll(u16 ticks)
{
	u8 h[TOD_SM_HEAD_LEN];
	const u32 lt = TOD_SM_LOAD_THREAD;

	/* A read that fails is a game that cannot be asked this tick, not an answer. */
	if (!ss_read(h)) return ticks < TOD_LOAD_WAIT_TICKS ? 1 : SAVEFILE_HELPER_REFUSED;

	if (ss_word(h, lt + TOD_ST_ID) == 0 && ss_word(h, lt + TOD_ST_ID + 4) == TOD_THREAD_GONE) {
		if (ticks < TOD_LOAD_WAIT_TICKS) return 1;
		load_put_back();
		return SAVEFILE_HELPER_REFUSED;
	}

	/*
	 * The id is no longer qwark's, which only the poll's ESRCH path does, and
	 * that path applies the load whenever the thread's state is 5, as qwark
	 * left it: the load is in. Whatever the game starts after that (its own
	 * load screen, a quarter of a second later) is the game's.
	 */
	return 0;
}

/*
 * A save of this executable: weapon slot i's first word is its WPN_ id
 * (WeaponSlot_init), the level is one LoadLevel has, and the challenge count
 * is no higher than SaveData_prepareChallengeMode takes it. Every GAME.SAV
 * RPCS3 keeps reads that way; a console's protected copy, still encrypted,
 * does not.
 */
static u8 tod_sf_aside_ok(void)
{
	u8 head[4 * TOD_WEAPON_STRIDE];
	u8 tail[8];
	u32 i;

	if (mem_read(TOD_SAVE_IMAGE, head, sizeof(head)) != ST_OK ||
	    mem_read(TOD_SAVE_IMAGE + TOD_SAVE_LEVEL_OFF, tail, sizeof(tail)) != ST_OK)
		return SAVEFILE_ERR_NOT_IDLE;

	for (i = 0; i < 4; i++) {
		if (be32_get(head + i * TOD_WEAPON_STRIDE) != i) return SAVEFILE_ERR_NO_SAVE;
	}
	if (be32_get(tail) >= TOD_NPLANETS) return SAVEFILE_ERR_NO_SAVE;
	if (be32_get(tail + 4) > TOD_MAX_CHALLENGE) return SAVEFILE_ERR_NO_SAVE;
	return SAVEFILE_ERR_NONE;
}

static u8 tod_sf_running(void)
{
	u8 h[TOD_SM_HEAD_LEN];

	return ss_read(h) && ss_word(h, TOD_SM_STATE) != 0;
}

const struct sf_driver tod_sf_driver = {
	tod_sf_hold,
	tod_sf_holding,
	tod_sf_release,
	tod_sf_load,
	tod_sf_load_poll,
	tod_sf_aside_ok,
	tod_sf_running,
	1                     /* the set-aside is a file */
};

/* The aside buffer and nothing else: no caves, no hooks, no request bytes. */
static const struct sf_desc tod_sf_desc = {
	GAME_TOD,
	NULL, 0,
	NULL, 0,
	0, 0, 0,
	TOD_SAVE_IMAGE, TOD_SAVE_SIZE,
	0
};

const struct sf_desc *tod_savefile_desc(u8 game_id)
{
	(void)game_id;
	return &tod_sf_desc;
}

/* ------------------------------------------------------------ readiness */

/*
 * struct game_api's savefile_ready. The save system first: idle, or in qwark's
 * own hold. Then a set-aside (a STORE) needs a save, which the game has once it
 * has saved into a slot or loaded one in this process (TOD_SM_SLOT, which an
 * autosave needs too), and a load or a RESTORE needs gameplay with no planet
 * load queued and the front end not on its way out: the frames the game's own
 * load from its pause menu runs in. SaveManager_onLoaded's LoadLevel then
 * fades out and loads like any planet load.
 */
u8 tod_savefile_ready(u8 what)
{
	u8 h[TOD_SM_HEAD_LEN];
	u32 state = 0, request = 0, slot;
	u8 leaving = 0;

	if (!ss_read(h) || !ss_idle(h)) return SAVEFILE_ERR_NOT_IDLE;

	if (what == GAME_SF_SET_ASIDE) {
		slot = ss_word(h, TOD_SM_SLOT);
		if ((slot & 0x80000000u) != 0) return SAVEFILE_ERR_NO_SAVE;
		return tod_sf_aside_ok();
	}

	if (mem_read_u32(TOD_GAME_STATE, &state) != ST_OK ||
	    mem_read_u32(TOD_LOAD_REQUEST, &request) != ST_OK ||
	    mem_read_u8(TOD_FRONTEND_EXIT, &leaving) != ST_OK) return SAVEFILE_ERR_NOT_IDLE;
	if (state != TOD_STATE_GAMEPLAY || request != 0xFFFFFFFFu || leaving != 0) return SAVEFILE_ERR_NOT_IDLE;
	return SAVEFILE_ERR_NONE;
}

/* ------------------------------------------------------------- actions */

int tod_set_aside(void)
{
	return savefile_set_aside();
}

int tod_load_aside(void)
{
	return savefile_load_aside();
}

/*
 * A session's arrival. A qwark that went away in the middle of a copy (a
 * crash, a module update) leaves the save system held, and the game would
 * then never save again; one that went away while a load waited leaves a load
 * thread the game cannot finish. Both are put back here: a hold is only ever
 * qwark's, and the id is one no thread of the game's has.
 */
void tod_save_recover(void)
{
	u8 h[TOD_SM_HEAD_LEN];
	const u32 lt = TOD_SM_LOAD_THREAD;

	if (!ss_read(h)) return;
	if (ss_word(h, TOD_SM_STATE) == TOD_SS_HELD) {
		mem_write_u32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_IDLE);
		plat_log("tod: the save system was still held; given back");
	}
	if (ss_word(h, lt + TOD_ST_ID) == 0 && ss_word(h, lt + TOD_ST_ID + 4) == TOD_THREAD_GONE) {
		load_put_back();
		plat_log("tod: a load was still waiting; put back");
	}
}
