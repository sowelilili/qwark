/*
 * The savefile helper: one source, four games.
 *
 * qwark cannot ask a Ratchet game for its save data. The save lives in a buffer
 * the game allocates and fills only while it is saving, and putting one back is
 * a call into the game's own loader from a thread the game owns. So a few dozen
 * instructions go into the game instead: this file, compiled at a code cave
 * address, called once a frame from a hook qwark writes, doing nothing at all
 * until a request byte says otherwise.
 *
 * The three request bytes are the whole interface:
 *
 *   api_mod       the helper writes 1 here every call, so a byte that reads 1 is
 *                 proof the code is installed and the hook is being reached
 *   api_setaside  the client writes 1; the helper copies the game's live save
 *                 buffer into the aside buffer and then clears the byte, in
 *                 that order, because the cleared byte is what says the bytes
 *                 are there to be read
 *   api_load      the client writes 1; the helper hands the aside buffer to the
 *                 game's loader and clears the byte
 *
 * Build 43 adds one value to the two request bytes rather than a fourth byte:
 * the helper writes SF_API_REFUSED into either of them when it sees a request
 * it will not act on, which today means Deadlocked was in a cutscene, a
 * transition or a menu when the request came in. The request is dropped, never
 * deferred, and qwark reads the code, puts the byte back to 0 and tells the
 * client why. See sf_game_idle below.
 *
 * Build 53 gives RaC1 and RaC2 Deadlocked's shape for a load: the helper asks
 * the game for its pause, waits for a frame that starts in it, and loads from
 * there (sf_entry below has the why), and it drops a request with
 * SF_API_NO_SAVE when there is no save to act on. RaC3 and Deadlocked compile
 * exactly as they did in build 52.
 *
 * qwark reads and writes the aside buffer with ordinary memory reads, which is
 * the whole point: the save never becomes a file on the console. The mods this
 * is ported from wrote /dev_hdd0/game/<TITLEID>/USRDIR/tempsave with syscalls
 * and the PC then read that file back; none of that is left.
 *
 * Rules this file lives by, because the code it produces is dropped into a
 * running game rather than linked into a program:
 *
 *   - No global or static variable, and no string constant. Either would make
 *     the compiler reach through r2, the game's TOC pointer, for something that
 *     is not in the game's TOC. Every piece of state is a fixed address.
 *   - No C pointer holds a game address. The compiler is 64-bit and would make
 *     one eight bytes wide; the game's own pointers are four. Addresses travel
 *     as sf_u32 and are only turned into a pointer at the moment of a call.
 *   - Every game function is an undefined symbol the link binds to a hard
 *     address, so a call is one relative branch and r2 is left alone.
 *   - The copy counter lives at a fixed address rather than on the stack, as it
 *     does in all four mods this comes from. It costs nothing and it lets qwark
 *     watch a long copy go by.
 */
#include "sfhelper.h"

#if SF_GAME == SF_RAC1 || SF_GAME == SF_RAC2
/* Build 53: the two games that pause before they load. */
#define SF_PAUSE_FIRST 1
#endif

/*
 * Where the game keeps the save data it is working on, right now.
 *
 * Build 53, RaC1 and RaC2: 0 while the game has not set the pointer up, which
 * the callers take as "no save". Neither game has one from the first frame.
 */
static sf_u32 sf_savedata_buf(void)
{
#if SF_GAME == SF_RAC1
	/* A pointer plus a bias, which is how this game spells it. */
	sf_u32 base = SF_WORD(SF_SAVEDATA_PTR);

	if (base == 0) return 0;
	return base + SF_SAVEDATA_BIAS;
#elif SF_GAME == SF_RAC2
	/* A pointer to an info block whose second word is the buffer. */
	sf_u32 info = SF_WORD(SF_SAVEDATA_INFO);

	if (info == 0) return 0;
	return SF_WORD(info + SF_SAVEDATA_FIELD);
#else
	/* A pointer to an info block whose second word is the buffer. */
	return SF_WORD(SF_WORD(SF_SAVEDATA_INFO) + SF_SAVEDATA_FIELD);
#endif
}

#ifdef SF_PAUSE_FIRST
/*
 * Build 53. A save buffer into the aside buffer, one 32 KB chunk at a time,
 * from a source the caller has already checked. The chunking is the mods' own:
 * the game's memcpy is called with a size it is comfortable with rather than a
 * two-megabyte one.
 */
static void sf_copy_from(sf_u32 src)
{
	for (SF_WORD(SF_API_I) = 0; SF_WORD(SF_API_I) < SF_ASIDE_SIZE;
	     SF_WORD(SF_API_I) += SF_CHUNK) {
		sf_u32 i = SF_WORD(SF_API_I);

		sf_memcpy((void *)(SF_ASIDE_ADDR + i), (const void *)(src + i),
		          (int)SF_CHUNK);
	}
}

/*
 * Build 53. Whether a save of this executable starts at `addr`: its first two
 * words are the two block sizes the game's own restore insists on (sf_rac1.h,
 * sf_rac2.h). The game refuses anything else, and RaC1's helper would then have
 * sent it to a planet read out of whatever was there.
 */
static int sf_is_save(sf_u32 addr)
{
	sf_u32 global = SF_WORD(addr);
	sf_u32 planet = SF_WORD(addr + 4);

#if SF_GAME == SF_RAC1
	if (global != SF_SAVE_HEAD_GLOBAL || planet != SF_SAVE_HEAD_PLANET) return 0;
	/* And the planet the helper sends the game to is one it has. */
	return SF_WORD(addr + SF_ASIDE_PLANET_OFFSET) < SF_PLANET_COUNT;
#else
	if (global != SF_SAVE_HEAD_GLOBAL && global != SF_SAVE_HEAD_GLOBAL_OLD) return 0;
	return planet == SF_SAVE_HEAD_PLANET || planet == SF_SAVE_HEAD_PLANET_OLD;
#endif
}

/*
 * Build 53. What a load hands the game: the aside buffer, or - when nothing has
 * been put there, which is what every mod this comes from did for a load asked
 * for before any set-aside - the game's own save, copied in first. 0 when that
 * is not a save either. An aside buffer that holds something and not a save is
 * not quietly replaced with the game's save: the load the user asked for was of
 * what they put there.
 */
static sf_u32 sf_load_source(void)
{
	sf_u32 src = SF_ASIDE_ADDR;

	if (SF_WORD(SF_ASIDE_ADDR) == 0) {
		src = sf_savedata_buf();
		if (src == 0) return 0;
	}
	return sf_is_save(src) ? src : 0;
}

/*
 * Build 53. The load itself, from the pause. The request byte is cleared before
 * the loader runs rather than after it: both loaders can spin a frame loop of
 * their own for a message box (0xE8E8C in RaC1, 0x83508 in RaC2), which calls
 * the pad routine and so this helper, and a byte still reading 1 there would
 * start the same load inside itself.
 */
static void sf_load(sf_u32 src)
{
	if (src != SF_ASIDE_ADDR) sf_copy_from(src);

	SF_BYTE(SF_API_LOAD) = 0;
	sf_perform_load(0, (void *)SF_ASIDE_ADDR);

#if SF_GAME == SF_RAC1
	/*
	 * RaC1's loader fills the save state in but does not go anywhere, so the
	 * helper asks for the planet the save was made on. RaC2's travels on its own.
	 */
	SF_WORD(SF_DEST_PLANET) = SF_WORD(SF_ASIDE_ADDR + SF_ASIDE_PLANET_OFFSET);
	SF_WORD(SF_SHOULD_LOAD) = 1;
#endif
}
#else
/*
 * The game's save buffer into the aside buffer, one 32 KB chunk at a time. The
 * chunking is the mods' own: the game's memcpy is called with a size it is
 * comfortable with rather than a two-megabyte one.
 */
static void sf_copy_aside(void)
{
	sf_u32 src = sf_savedata_buf();

	for (SF_WORD(SF_API_I) = 0; SF_WORD(SF_API_I) < SF_ASIDE_SIZE;
	     SF_WORD(SF_API_I) += SF_CHUNK) {
		sf_u32 i = SF_WORD(SF_API_I);

		sf_memcpy((void *)(SF_ASIDE_ADDR + i), (const void *)(src + i),
		          (int)SF_CHUNK);
	}
}

/*
 * A load the client asked for before it ever asked for a set-aside would hand
 * the loader an empty buffer, so every mod this comes from fills it from the
 * live save first. Kept: it is cheap and it is what a console has run.
 */
static void sf_fill_if_empty(void)
{
	if (SF_WORD(SF_ASIDE_ADDR) == 0) sf_copy_aside();
}
#endif /* SF_PAUSE_FIRST */

#if SF_GAME == SF_RAC1
/*
 * Build 53. Whether the game would open its start menu if the player pressed
 * Start now, by the tests its own Start handling makes (sf_rac1.h): the helper
 * asks for the pause only where the game would have let the player pause.
 */
static int sf_may_pause(void)
{
	sf_u32 player = SF_WORD(SF_PLAYER_STATE);

	if (SF_WORD(SF_GAME_STATE) != SF_STATE_PLAY) return 0;
	if ((int)SF_WORD(SF_STATE_FRAMES) < SF_STATE_FRAMES_MIN) return 0;
	if (player == SF_PSTATE_NO_PAUSE1 || player == SF_PSTATE_NO_PAUSE2 ||
	    player == SF_PSTATE_NO_PAUSE3) return 0;
	if (SF_BYTE(SF_PLAYER_FLAG_A) == 2 || SF_BYTE(SF_PLAYER_FLAG_B) != 0) return 0;
	if (SF_WORD(SF_PLAYER_WORD) == 0) return 0;
	if (SF_WORD(SF_PLAYER_STATE_B) == SF_PSTATE_B_MENU) return 0;
	if (SF_WORD(SF_CURRENT_PLANET) == SF_PLANET_QUARTU &&
	    SF_BYTE(SF_PLAYER_FLAG_C) == 2) return 0;
	return SF_WORD(SF_DEBUG_MODE) == 0;
}
#endif

/*
 * Build 43. Whether the game is somewhere a save may be copied out of it or
 * handed back to it on this frame.
 *
 * Deadlocked keeps working on its save data from inside the game: the load is a
 * menu, the autosave writes the same buffer, and every planet load, lobby
 * screen and cutscene transition moves the game between modes that read or
 * rewrite it. A copy taken across one of those is a save with the game's
 * fingerprints half way through it, which is what the file coming back off the
 * console was; a load raised across one hands the loader a buffer something
 * else is already walking.
 *
 * The test is the one the game's own mode_requestChange makes, plus the two
 * modes where the save sits still: nothing may happen while a mode change is
 * queued, and gameMode must be ordinary play or the pause. The pause is in
 * because it is the mode this helper parks the game in itself, further down,
 * to run the load menu - so the load's own later steps still see an idle game,
 * which is exactly what they should.
 *
 * qwark makes the same test on its own tick before it writes the request byte
 * at all, and refuses with SAVEFILE_ERR_NOT_IDLE so the client can say why.
 * This is the half of it that cannot be raced: qwark's tick and the game's
 * frame are different clocks, and the game gets frames of its own in between.
 *
 * The other three games have no such check here. Their loaders take a buffer
 * and return, with no menu of their own, and nobody has reported a save coming
 * back dirty from them. Build 53 gives RaC1 and RaC2 tests of their own, in
 * their load paths below: the game's pause before a load, and a save to act
 * on before either request.
 */
#if SF_GAME == SF_RAC4
static int sf_game_idle(void)
{
	sf_u32 mode = SF_WORD(SF_GAME_MODE);

	if (mode != SF_MODE_NORMAL && mode != SF_MODE_PAUSE) return 0;
	return SF_WORD(SF_PENDING_GAME_MODE) == SF_MODE_NONE;
}
#else
/* Folded away at -O2, so the other three games' bytes do not move. */
#define sf_game_idle() 1
#endif

/*
 * Its own section so the link map can put it at the front of the cave, which is
 * where the mods this comes from have their entry point.
 */
__attribute__((section(".text.entry")))
void sf_entry(void)
{
	SF_BYTE(SF_API_MOD) = 1;

	/*
	 * The byte goes to zero after the copy, never before it. A zero here is what
	 * qwark reports the request answered on, and the client reads the aside
	 * buffer out on that answer, so clearing first hands out a buffer the copy
	 * loop is still walking: the file gets a torn tail, and loading it later
	 * crashes the game. The mods this comes from all cleared it first, and RaC2
	 * copies two megabytes in 32 KB steps, which is the window that showed up on
	 * hardware.
	 */
	if (SF_BYTE(SF_API_SETASIDE) == 1) {
#ifdef SF_PAUSE_FIRST
		/*
		 * Build 53. Only a save is copied: until the game has saved or loaded
		 * once there is no buffer (RaC2) or nothing in it (RaC1), and the copy
		 * used to read two megabytes from address 0, or set aside whatever the
		 * memory held. The byte carries the reason out; see SF_API_NO_SAVE.
		 */
		sf_u32 src = sf_savedata_buf();

		if (src == 0 || !sf_is_save(src)) {
			SF_BYTE(SF_API_SETASIDE) = SF_API_NO_SAVE;
		} else {
			sf_copy_from(src);
			SF_BYTE(SF_API_SETASIDE) = 0;
		}
#else
		/*
		 * Build 43, Deadlocked only: a request seen while the game is busy is
		 * dropped, not held for a quieter frame. The byte carries the reason
		 * out; see SF_API_REFUSED.
		 */
		if (!sf_game_idle()) {
			SF_BYTE(SF_API_SETASIDE) = SF_API_REFUSED;
		} else {
			sf_copy_aside();
			SF_BYTE(SF_API_SETASIDE) = 0;
		}
#endif
	}

#if SF_GAME == SF_RAC4
	/*
	 * Deadlocked's loader is the load *menu*, so this runs over several frames.
	 *
	 *   api_load 1, first frame   pause the game (mode 3), note the frame it
	 *                             paused in; SF_PAUSE_WAIT goes to 1
	 *   api_load 1, then          on the first call from a later frame that finds
	 *                             the game in the pause with no mode change
	 *                             queued, hand the buffer over and move to
	 *                             state 2
	 *   api_load 2                watch the menu's status word: 15 or 24 is the
	 *                             player confirming, and the load goes through;
	 *                             28 is a cancel: note the frame, move to state 3
	 *   api_load 3                on the first call from a later frame that finds
	 *                             the game in play or in the pause with no mode
	 *                             change queued, put back the mode the pause
	 *                             replaced if the game is still paused, and
	 *                             finish (build 45)
	 *
	 * The wait is on the game's own mode rather than a frame count. The helper
	 * runs from the pad routine, part way through a frame, so the frame it
	 * pauses in still finishes its world update, and anything that frame
	 * queues - an autosave, a message, a cutscene - is applied at its end and
	 * takes the game out of the pause the helper put it in. Build 43's
	 * four-frame countdown restored the save regardless, and whatever the game
	 * was doing then went on writing into the save it had just been handed:
	 * the carried-over data. The restore waits for the game to be in mode 3
	 * with nothing queued, up to SF_PAUSE_WAIT_MAX frames.
	 *
	 * Build 45 drops build 44's extra quarter second in the pause: mode 3 with
	 * nothing queued is enough. It still has to be a later frame than the one
	 * that paused - that frame's own mode change is only applied at its end,
	 * and the pad routine can run twice in one frame - so the helper keeps the
	 * game's mode timer (SF_MODE_TIMER), which moves once per frame, from the
	 * call that paused, and restores once it reads something else.
	 *
	 * A request the game will not settle for is dropped, as build 43 drops one
	 * seen while the game is busy: the game back in ordinary play means the
	 * pause is gone and the player is playing, so the load is not raised at
	 * them later; running out of frames means the same. If the helper's own
	 * pause is still in place when it gives up, the mode it replaced goes back,
	 * so the game is not left paused with no menu.
	 *
	 * Build 45 also gives a cancel the same wait. Up to build 44 the helper put
	 * the old mode back on the call that saw the 28, and on hardware a
	 * cancelled load left the game paused. The game's own close of the load
	 * screen can queue a pause of its own - callstack_4 (0x003A46B0) in state
	 * 28 pops the GUI screen, and the layer pop under it (0x003600F0) asks
	 * mode_requestChange for the pause when the layer it returns to is the
	 * pause menu's - and a mode the game queues is applied at the end of the
	 * frame, after the helper's write. Waiting for a later frame with nothing
	 * queued lets the game finish first, and the mode goes back only if the
	 * game is still in the pause then.
	 */
	{
		/* Each word read once, so the helper stays inside its cave. */
		sf_u32 load   = SF_BYTE(SF_API_LOAD);
		sf_u32 mode   = SF_WORD(SF_GAME_MODE);
		sf_u32 queued = SF_WORD(SF_PENDING_GAME_MODE) != SF_MODE_NONE;
		sf_u32 frame  = SF_WORD(SF_MODE_TIMER);
		sf_u32 wait   = SF_WORD(SF_PAUSE_WAIT);
		sf_u32 idle   = !queued && (mode == SF_MODE_NORMAL || mode == SF_MODE_PAUSE);

		/*
		 * A wait belongs to one request. Anything that clears the byte from
		 * outside - qwark, a game that restarted - must not leave the next
		 * request starting half way through a wait that never paused anything.
		 */
		if (load != 1 && load != 3) wait = 0;

		if (load == 1 && wait == 0) {
			if (!idle) {
				SF_BYTE(SF_API_LOAD) = SF_API_REFUSED;
			} else {
				sf_fill_if_empty();

				SF_WORD(SF_PREV_GAME_MODE) = mode;
				SF_WORD(SF_GAME_MODE) = SF_MODE_PAUSE;
				SF_WORD(SF_PAUSE_FRAME) = frame;
				wait = 1;
			}
		} else if (load == 1 || load == 3) {
			/*
			 * The two waits: for the pause before the load, and for the game
			 * to finish closing the menu after a cancel. Either ends on a later
			 * frame than the one that started it, with the game in play or in
			 * the pause and nothing queued, or when it runs out of frames.
			 */
			wait += 1;
			if (idle && frame != SF_WORD(SF_PAUSE_FRAME)) {
				wait = 0;
				if (load == 3) {
					if (mode == SF_MODE_PAUSE)
						SF_WORD(SF_GAME_MODE) = SF_WORD(SF_PREV_GAME_MODE);
					SF_BYTE(SF_API_LOAD) = 0;
				} else if (mode == SF_MODE_PAUSE) {
					sf_restore_saved_game(0, (void *)SF_ASIDE_ADDR);
					SF_BYTE(SF_API_LOAD) = 2;
				} else {
					SF_BYTE(SF_API_LOAD) = SF_API_REFUSED;
				}
			} else if (wait > SF_PAUSE_WAIT_MAX) {
				wait = 0;
				if (mode == SF_MODE_PAUSE)
					SF_WORD(SF_GAME_MODE) = SF_WORD(SF_PREV_GAME_MODE);
				SF_BYTE(SF_API_LOAD) = load == 3 ? 0 : SF_API_REFUSED;
			}
		} else if (load == 2) {
			sf_u32 magic = SF_WORD(SF_MAGIC_ADDR);

			if (magic == 15 || magic == 24) {
				SF_BYTE(SF_API_LOAD) = 0;
				sf_magic_load();
			} else if (magic == 28) {
				SF_WORD(SF_PAUSE_FRAME) = frame;
				SF_BYTE(SF_API_LOAD) = 3;
			}
		}

		SF_WORD(SF_PAUSE_WAIT) = wait;
	}
#elif SF_GAME == SF_RAC1
	/*
	 * Build 53. RaC1's load waits for the start menu.
	 *
	 * The loader (0xE8CA0, through the restore 0xE6FE4) writes every field of
	 * the save over the live game the moment it runs, and the first field is the
	 * current planet, 0x969C70; the planet's own data - its sky, its mobies -
	 * stays the old planet's until the load the helper asks for runs, at the end
	 * of the frame. Up to build 52 the helper loaded from the pad routine at the
	 * top of a frame of ordinary play, so the rest of that frame updated and drew
	 * the old planet with the new planet's number. The world render picks a
	 * planet's sky routine by that number (0xD93F4): Veldin's (0x153E00) draws
	 * five sky layers whatever the loaded sky holds, and layer 3 of a planet
	 * with fewer is a null pointer, which is the crash at 0x50A9C8 in the draw
	 * routine 0x50A538 that a load of a Veldin save hit on some planets.
	 *
	 * The start menu runs neither the world update nor the world render (state 3
	 * runs 0x13B838 and 0x6C558), so the helper puts the game there first, the
	 * way the player does: it calls the menu's opener where the Start button
	 * would have (sf_may_pause), which sets the state to 3 at once, and on a
	 * later frame - one that started in the menu, which the frames-in-state
	 * counter going back to 0 shows - it loads. The planet load that follows
	 * starts the new planet in ordinary play, as any load from the menu does.
	 *
	 *   SF_PAUSE_WAIT 0   a request in the start menu loads at once; one in
	 *                     ordinary play asks for the menu, notes the counter and
	 *                     moves to 1; anything else is dropped
	 *   SF_PAUSE_WAIT n   waits for the menu on a later frame, and drops the
	 *                     request when the game is no longer in it or after
	 *                     SF_PAUSE_WAIT_MAX calls. The menu is the game's real
	 *                     one, so a dropped request leaves the player in it.
	 *
	 * A request while a planet load is already on its way is dropped too.
	 */
	{
		sf_u32 load = SF_BYTE(SF_API_LOAD);
		sf_u32 wait = SF_WORD(SF_PAUSE_WAIT);

		if (load != 1) wait = 0;

		if (load == 1) {
			sf_u32 state = SF_WORD(SF_GAME_STATE);
			sf_u32 frame = SF_WORD(SF_STATE_FRAMES);
			sf_u32 code  = 0;
			sf_u32 src;

			if (SF_WORD(SF_SHOULD_LOAD) != 0) {
				code = SF_API_REFUSED;
			} else if (wait == 0) {
				src = sf_load_source();
				if (src == 0) {
					code = SF_API_NO_SAVE;
				} else if (state == SF_STATE_PAUSE) {
					sf_load(src);
				} else if (sf_may_pause()) {
					sf_open_pause(0);
					if (SF_WORD(SF_GAME_STATE) == SF_STATE_PAUSE) {
						SF_WORD(SF_PAUSE_FRAME) = frame;
						wait = 1;
					} else {
						code = SF_API_REFUSED;
					}
				} else {
					code = SF_API_REFUSED;
				}
			} else {
				wait += 1;
				if (state != SF_STATE_PAUSE) {
					code = SF_API_REFUSED;
				} else if (frame != SF_WORD(SF_PAUSE_FRAME)) {
					wait = 0;
					src = sf_load_source();
					if (src == 0) code = SF_API_NO_SAVE;
					else sf_load(src);
				} else if (wait > SF_PAUSE_WAIT_MAX) {
					code = SF_API_REFUSED;
				}
			}

			if (code != 0) {
				SF_BYTE(SF_API_LOAD) = code;
				wait = 0;
			}
		}

		SF_WORD(SF_PAUSE_WAIT) = wait;
	}
#elif SF_GAME == SF_RAC2
	/*
	 * Build 53. RaC2's load waits for the pause, as Deadlocked's does.
	 *
	 * Its loader (0x83250, through the restore 0x81070) also writes the save over
	 * the live game at once, current planet first (0x1329A3C), and asks for the
	 * planet load itself. The helper runs at the end of a frame here, after the
	 * update and the render, so less of the frame is left to run with the wrong
	 * planet than in RaC1; what is left in ordinary play (0xAFC23C) does not run
	 * in the pause, and a load from the pause is the path the game's own pause
	 * menu takes.
	 *
	 * The modes are Deadlocked's ancestors, so the flow is Deadlocked's: in
	 * ordinary play with nothing queued, the helper asks mode_requestChange for
	 * the pause exactly as the Start button does (mode 3, pushed, screen 1); the
	 * game makes the change at the end of the frame and puts the mode timer back
	 * to 0, so the load goes on the first later call that finds the pause with
	 * nothing queued and the timer moved. A request already in the pause loads
	 * at once. Leaving for the planet load resets the mode stack (0xB4DBD8) and
	 * the new planet starts in ordinary play (0xB4CA84).
	 *
	 * A request on the title screen (planet -1), while a planet load is on its
	 * way, or that the game will not pause for is dropped; so is one whose wait
	 * finds the game anywhere but the pause, or runs out. The pause is the game's
	 * real one, so a dropped request leaves the player in it.
	 */
	{
		sf_u32 load = SF_BYTE(SF_API_LOAD);
		sf_u32 wait = SF_WORD(SF_PAUSE_WAIT);

		if (load != 1) wait = 0;

		if (load == 1) {
			sf_u32 mode   = SF_WORD(SF_GAME_MODE);
			sf_u32 queued = SF_WORD(SF_PENDING_GAME_MODE) != SF_MODE_NONE;
			sf_u32 frame  = SF_WORD(SF_MODE_TIMER);
			sf_u32 code   = 0;
			sf_u32 src;

			if ((int)SF_WORD(SF_CURRENT_PLANET) < 0 || SF_WORD(SF_LOAD_REQUEST) != 0) {
				code = SF_API_REFUSED;
			} else if (wait == 0) {
				src = sf_load_source();
				if (src == 0) {
					code = SF_API_NO_SAVE;
				} else if (mode == SF_MODE_PAUSE && !queued) {
					sf_load(src);
				} else if (mode == SF_MODE_NORMAL && !queued &&
				           (int)frame >= SF_MODE_FRAMES_MIN &&
				           SF_WORD(SF_PLAYER_STATE) != SF_PLAYER_NO_PAUSE &&
				           sf_request_mode(SF_MODE_PAUSE, 1, SF_PAUSE_SCREEN, 0, 0) == 0) {
					SF_WORD(SF_PAUSE_FRAME) = frame;
					wait = 1;
				} else {
					code = SF_API_REFUSED;
				}
			} else {
				wait += 1;
				if (!queued && frame != SF_WORD(SF_PAUSE_FRAME)) {
					wait = 0;
					if (mode != SF_MODE_PAUSE) {
						code = SF_API_REFUSED;
					} else {
						src = sf_load_source();
						if (src == 0) code = SF_API_NO_SAVE;
						else sf_load(src);
					}
				} else if (wait > SF_PAUSE_WAIT_MAX) {
					code = SF_API_REFUSED;
				}
			}

			if (code != 0) {
				SF_BYTE(SF_API_LOAD) = code;
				wait = 0;
			}
		}

		SF_WORD(SF_PAUSE_WAIT) = wait;
	}
#else
	if (SF_BYTE(SF_API_LOAD) == 1) {
		sf_fill_if_empty();
		sf_perform_load(0, (void *)SF_ASIDE_ADDR);
		SF_BYTE(SF_API_LOAD) = 0;
	}
#endif

#if SF_GAME == SF_RAC1
	/*
	 * RaC1 alone keeps a fourth byte, the one the Force autosave action drives.
	 * 0 is idle, and the modes are the game's own: 0 save menu, 1 load menu,
	 * 3 autosave. Mode 2 is not a menu, so it opens the save menu first, which is
	 * what the mod did and what the old client relied on.
	 */
	if (SF_BYTE(SF_API_SAVEMODE) != 0) {
		if (SF_BYTE(SF_API_SAVEMODE) == 2) sf_save_handler(0);

		sf_save_handler((int)SF_BYTE(SF_API_SAVEMODE));
		SF_BYTE(SF_API_SAVEMODE) = 0;
	}
#endif
}
