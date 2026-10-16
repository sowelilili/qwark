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
 * it will not act on, which for Deadlocked means it was in a cutscene, a
 * transition or a menu when the request came in. The request is dropped, never
 * deferred, and qwark reads the code, puts the byte back to 0 and tells the
 * client why. See sf_game_idle below.
 *
 * Build 53 gives RaC1, RaC2 and RaC3 a load that goes from the game's pause,
 * which neither updates nor draws the world (sf_entry below has the why): the
 * helper puts the game there itself, from whatever it is doing, waits for a
 * frame that runs in it, and loads. It drops a request with SF_API_NO_SAVE when
 * there is no save to act on. Deadlocked compiles exactly as it did in build
 * 52.
 *
 * Build 53 also gives RaC3 a fourth request byte, api_trophy, and a second cave
 * for what it asks for: the trophy reset rc3-save carried (sf_trophy_reset).
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

#if SF_GAME == SF_RAC1 || SF_GAME == SF_RAC2 || SF_GAME == SF_RAC3
/* Build 53: the three games that pause before they load. */
#define SF_PAUSE_FIRST 1
#endif

/*
 * Where the game keeps the save data it is working on, right now.
 *
 * Build 53, RaC1, RaC2 and RaC3: 0 while the game has not set the pointer up,
 * which the callers take as "no save". None of the three has one from the
 * first frame.
 */
static sf_u32 sf_savedata_buf(void)
{
#if SF_GAME == SF_RAC1
	/* A pointer plus a bias, which is how this game spells it. */
	sf_u32 base = SF_WORD(SF_SAVEDATA_PTR);

	if (base == 0) return 0;
	return base + SF_SAVEDATA_BIAS;
#elif SF_GAME == SF_RAC2 || SF_GAME == SF_RAC3
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
 * sf_rac2.h, sf_rac3.h). The game refuses anything else, and RaC1's helper
 * would then have sent it to a planet read out of whatever was there.
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
 * the loader runs rather than after it: the loaders can spin a frame loop of
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
	 * helper asks for the planet the save was made on. RaC2's and RaC3's travel
	 * on their own.
	 */
	SF_WORD(SF_DEST_PLANET) = SF_WORD(SF_ASIDE_ADDR + SF_ASIDE_PLANET_OFFSET);
	SF_WORD(SF_SHOULD_LOAD) = 1;
#endif
}

/*
 * Whether the helper may take the game into its pause from `mode`, the game
 * state in RaC1 and gameMode in the other two. A load is refused only where it
 * cannot be made safe by pausing first:
 *
 *   - a planet load already on its way, which the load would be raced against
 *     (RaC1's should-load word, the others' load request word);
 *   - RaC2's title screen, planet -1, where no planet is loaded to pause;
 *   - a mode the pause cannot be entered from. RaC1's movie (state 1) plays in a
 *     frame loop of its own that ignores the state until the movie is over, so
 *     a pause asked for there would come at whatever moment it ended; a state
 *     outside -1..7 runs nothing, not even this helper. In RaC2 and RaC3, debug
 *     (-1) traps in the mode change on the way out, space (6) is the transition
 *     a planet load runs, and anything past 9 is the lobby and multiplayer.
 */
static int sf_may_pause(sf_u32 mode)
{
#if SF_GAME == SF_RAC1
	if (SF_WORD(SF_SHOULD_LOAD) != 0) return 0;
	return mode - SF_STATE_FIRST < SF_STATE_COUNT && mode != SF_STATE_MOVIE;
#else
#if SF_GAME == SF_RAC2
	if ((int)SF_WORD(SF_CURRENT_PLANET) < 0) return 0;
#endif
	if (SF_WORD(SF_LOAD_REQUEST) != 0) return 0;
	return mode <= SF_MODE_LAST && mode != SF_MODE_SPACE;
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
 * back dirty from them. Build 53 gives them tests of their own, in their load
 * paths below: the game's pause before a load, and a save to act on before
 * either request.
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

#if SF_GAME == SF_RAC3
/*
 * Build 53. lv2's sys_timer_usleep, syscall 141, the call the game's own trophy
 * threads wait with (0x985304, 0x985A84). rc3-save waited with sys_timer_sleep,
 * 142, a second at a time; the wait is the same. The registers the kernel may
 * change are the ones PSL1GHT's syscall macros name.
 */
static __inline__ void sf_usleep(sf_u32 us) __attribute__((always_inline));
static __inline__ void sf_usleep(sf_u32 us)
{
	register unsigned long long r3 __asm__("r3") = us;
	register unsigned long long r11 __asm__("r11") = 141;

	__asm__ __volatile__("sc"
		: "+r"(r3), "+r"(r11)
		:
		: "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r12",
		  "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
}

/*
 * Build 53. The trophy reset, rc3-save 1.1's handler (rack.bin 0x980E08) call
 * for call; sf_rac3.h has the object and what each step does. It lives in a cave
 * of its own, at SF_CAVE_TROPHY, because the helper's cave has no room for it.
 *
 * It runs on the game's main thread, from the pad routine, and it blocks that
 * thread for its three waits: the game stands still for three seconds. That is
 * why the auto toggle's request (SF_TROPHY_IN_LOAD) is refused unless the game
 * is in the transition a planet load runs (mode 6, the load request up), which
 * for a run is the load into Veldin whose end starts the timer: the three
 * seconds go into that load, never into the run.
 *
 * The request is refused, not held, when the object is not set up yet or one of
 * its threads is running: destroying the context under the register thread or
 * the unlock thread would leave either working on a handle that is gone. The
 * byte reads SF_TROPHY_RUNNING for as long as the reset runs, so qwark can tell
 * a reset under way from one not yet seen.
 */
__attribute__((noinline, section(".text.trophy")))
static void sf_trophy_reset(sf_u32 request)
{
	sf_u32 obj = SF_WORD(SF_TROPHY_OBJ_PTR);
	sf_u32 n, i;

	if (obj == 0 || SF_WORD(obj + SF_TROPHY_THREAD) != 0 ||
	    (request == SF_TROPHY_IN_LOAD &&
	     (SF_WORD(SF_GAME_MODE) != SF_MODE_SPACE || SF_WORD(SF_LOAD_REQUEST) == 0))) {
		SF_BYTE(SF_API_TROPHY) = SF_API_REFUSED;
		return;
	}

	SF_BYTE(SF_API_TROPHY) = SF_TROPHY_RUNNING;

	sf_trophy_destroy(obj);

	n = SF_WORD(obj + SF_TROPHY_COUNT);
	if (n > SF_TROPHY_MAX) n = SF_TROPHY_MAX;
	for (i = 0; i < n; i++) SF_WORD(obj + SF_TROPHY_FLAGS + i * 8) = 1;
	SF_WORD(obj + SF_TROPHY_FLAGS + SF_TROPHY_KEEP_1 * 8) = 0;
	SF_WORD(obj + SF_TROPHY_FLAGS + SF_TROPHY_KEEP_2 * 8) = 0;
	SF_WORD(obj + SF_TROPHY_FLAGS + SF_TROPHY_KEEP_3 * 8) = 0;

	sf_usleep(SF_TROPHY_WAIT_US);
	sf_trophy_register(obj);
	sf_usleep(SF_TROPHY_WAIT_US);
	sf_trophy_destroy(obj);
	sf_usleep(SF_TROPHY_WAIT_US);
	sf_trophy_register(obj);

	SF_BYTE(SF_API_TROPHY) = 0;
}
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
		 * once there is no buffer (RaC2, RaC3) or nothing in it (RaC1), and the
		 * copy used to read two megabytes from address 0, or set aside whatever
		 * the memory held. The byte carries the reason out; see
		 * SF_API_NO_SAVE. A set-aside copies the game's last save, not its live
		 * state, so it needs no pause.
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
#else
	/*
	 * Build 53. RaC1's, RaC2's and RaC3's loads go from the game's pause.
	 *
	 * Each game's loader (RaC1 0xE8CA0, RaC2 0x83250, RaC3 0x1E1CF4) writes every
	 * field of the save over the live game the moment it runs, the current
	 * planet first, while the planet's own data - its sky, its mobies - stays
	 * the old planet's until the planet load that follows, which starts at the
	 * end of the frame. Up to build 52 the helper loaded wherever the request
	 * found the game, and RaC1 then updated and drew the rest of that frame with
	 * the new planet's number over the old planet's world: the world render
	 * picks a planet's sky routine by that number (0xD93F4), Veldin's (0x153E00)
	 * draws five sky layers whatever the loaded sky holds, and layer 3 of a
	 * planet with fewer is a null pointer - the crash at 0x50A9C8 in 0x50A538
	 * that loading a Veldin save hit on some planets. RaC2's helper runs at the
	 * end of a frame, so less of it was left to run, and RaC3's had not been
	 * seen to crash; both go the same way now as a precaution.
	 *
	 * None of the three has a mode that runs nothing. Each has a freeze of
	 * sorts - RaC1's prompt, RaC2's and RaC3's freeze, the gameMode_t mode
	 * Deadlocked names GAME_MODE_FREEZE - but it is a message over a stopped
	 * game and draws the world under it, which is the render that crashed. Each
	 * game's pause neither updates nor draws the world (sf_rac1.h, sf_rac2.h,
	 * sf_rac3.h), so the helper loads from the pause:
	 *
	 *   1. A request finds the game somewhere it may be paused (sf_may_pause)
	 *      and a save to hand over (sf_load_source), or is dropped with
	 *      SF_API_REFUSED or SF_API_NO_SAVE.
	 *   2. Already in the pause - the player's own, nothing queued - it loads at
	 *      once: the frame it runs in is a pause frame.
	 *   3. Otherwise the helper puts the game in the pause, from whatever mode it
	 *      is in and without any of the Start button's tests. RaC1 changes state
	 *      by writing it, and the helper does too (sf_enter_menu, which also puts
	 *      the start menu's own words back, sf_rac1.h); RaC2 and RaC3 change
	 *      mode through mode_requestChange, which runs the leaving mode's exit
	 *      and the pause menu's entry at the end of the frame, and the helper
	 *      asks it (sf_rac2.h). Where the game refuses for now - a mode change
	 *      already queued, the player with no health - it asks again on the
	 *      next call, and RaC1's state put back by the rest of the frame is
	 *      written again.
	 *   4. It loads on the first call from a later frame that runs in the
	 *      pause: the state or mode reads the pause and the game's frame counter
	 *      for it - RaC1's frames in state, the others' mode timer, both put back
	 *      to 0 when it changes - has moved since the call that entered it. The
	 *      pad routine can run twice in a frame, and RaC1's state is the pause
	 *      from the moment the helper writes it, part way through a frame that
	 *      still runs the old state's update and render.
	 *   5. A request still waiting after SF_PAUSE_WAIT_MAX calls is dropped with
	 *      SF_API_REFUSED, and so is one that finds the game somewhere it may not
	 *      be paused while it waits. The pause the helper entered stays: it is
	 *      the game's own, and the player leaves it as any other.
	 *
	 * The planet load the loader starts begins the new planet in its ordinary
	 * opening mode, whatever the old one was left in: RaC1's main loop starts
	 * every planet in state 6, RaC2's leaving resets the mode stack and the
	 * gameplay loop starts in mode 0, and RaC3's game loop clears the mode stack
	 * and its main loop starts in mode 0.
	 *
	 * RaC1's and RaC3's helpers run at the top of a frame, before its update;
	 * RaC2's at the end, after its render. In RaC1 and RaC3 the load is the
	 * first thing in its pause frame, and in RaC2 it follows a pause frame's
	 * update and render; either way, what is left of the frame is the pause's,
	 * and the planet load follows it.
	 */
	{
		sf_u32 load = SF_BYTE(SF_API_LOAD);
		sf_u32 wait = SF_WORD(SF_PAUSE_WAIT);

		if (load != 1) wait = 0;

		if (load == 1) {
#if SF_GAME == SF_RAC1
			sf_u32 mode   = SF_WORD(SF_GAME_STATE);
			sf_u32 frame  = SF_WORD(SF_STATE_FRAMES);
			sf_u32 pause  = SF_STATE_PAUSE;
			sf_u32 paused = mode == pause;
#else
			sf_u32 mode   = SF_WORD(SF_GAME_MODE);
			sf_u32 frame  = SF_WORD(SF_MODE_TIMER);
			sf_u32 pause  = SF_MODE_PAUSE;
			sf_u32 paused = mode == pause &&
			                SF_WORD(SF_PENDING_GAME_MODE) == SF_MODE_NONE;
#endif
			sf_u32 code = 0;
			sf_u32 src;

			if (!sf_may_pause(mode)) {
				code = SF_API_REFUSED;
			} else if ((src = sf_load_source()) == 0) {
				code = SF_API_NO_SAVE;
			} else if (paused && (wait == 0 || frame != SF_WORD(SF_PAUSE_FRAME))) {
				wait = 0;
				sf_load(src);
			} else if (++wait > SF_PAUSE_WAIT_MAX) {
				code = SF_API_REFUSED;
			} else if (mode != pause) {
#if SF_GAME == SF_RAC1
				sf_enter_menu();
				SF_WORD(SF_MENU_SCREEN) = SF_MENU_FIRST_SCREEN;
				SF_WORD(SF_PAUSE_FRAME) = frame;
#else
				sf_u32 screen = SF_PAUSE_SCREEN;

#if SF_GAME == SF_RAC3
				/* The vid comics have a pause of their own, as Start opens it. */
				if (SF_WORD(SF_CURRENT_PLANET) - SF_PLANET_COMIC_FIRST <
				    SF_PLANET_COMIC_COUNT)
					screen = SF_PAUSE_SCREEN_COMIC;
#endif
				if (sf_request_mode((int)pause, 1, (int)screen, 0, 0) == 0)
					SF_WORD(SF_PAUSE_FRAME) = frame;
#endif
			}

			if (code != 0) {
				SF_BYTE(SF_API_LOAD) = code;
				wait = 0;
			}
		}

		SF_WORD(SF_PAUSE_WAIT) = wait;
	}
#endif

#if SF_GAME == SF_RAC3
	/* Build 53: the trophy reset, in its own cave. */
	{
		sf_u32 trophy = SF_BYTE(SF_API_TROPHY);

		if (trophy == SF_TROPHY_NOW || trophy == SF_TROPHY_IN_LOAD)
			sf_trophy_reset(trophy);
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
