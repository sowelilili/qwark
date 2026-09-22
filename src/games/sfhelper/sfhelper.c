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

/* Where the game keeps the save data it is working on, right now. */
static sf_u32 sf_savedata_buf(void)
{
#if SF_GAME == SF_RAC1
	/* A pointer plus a bias, which is how this game spells it. */
	return SF_WORD(SF_SAVEDATA_PTR) + SF_SAVEDATA_BIAS;
#else
	/* A pointer to an info block whose second word is the buffer. */
	return SF_WORD(SF_WORD(SF_SAVEDATA_INFO) + SF_SAVEDATA_FIELD);
#endif
}

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
 * The other three games have no such check. Their loaders take a buffer and
 * return, with no menu and no mode of their own, and nobody has reported a save
 * coming back dirty from them; whether they want the same guard is a question
 * for a console, not for this build.
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
#else
	if (SF_BYTE(SF_API_LOAD) == 1) {
		sf_fill_if_empty();
		sf_perform_load(0, (void *)SF_ASIDE_ADDR);

#if SF_GAME == SF_RAC1
		/*
		 * RaC1's loader fills the save state in but does not go anywhere, so the
		 * helper asks for the planet the save was made on. The other three games
		 * travel on their own.
		 */
		SF_WORD(SF_DEST_PLANET) = SF_WORD(SF_ASIDE_ADDR + SF_ASIDE_PLANET_OFFSET);
		SF_WORD(SF_SHOULD_LOAD) = 1;
#endif

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
