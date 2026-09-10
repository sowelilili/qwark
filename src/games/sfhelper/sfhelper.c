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
 * Its own section so the link map can put it at the front of the cave, which is
 * where the mods this comes from have their entry point.
 */
__attribute__((section(".text.entry")))
void sf_entry(void)
{
	SF_BYTE(SF_API_MOD) = 1;

#if SF_GAME == SF_RAC4
	if (SF_WORD(SF_FRAME_TIMER) > 0) SF_WORD(SF_FRAME_TIMER) -= 1;
#endif

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
		sf_copy_aside();
		SF_BYTE(SF_API_SETASIDE) = 0;
	}

#if SF_GAME == SF_RAC4
	/*
	 * Deadlocked's loader is the load *menu*, so this runs over several frames.
	 *
	 *   api_load 1, timer 0   pause the game (mode 3) and start a four-frame
	 *                         countdown, so the pause has taken by the time the
	 *                         buffer goes in
	 *   api_load 1, timer 1   hand the buffer over and move to state 2
	 *   api_load 2            watch the menu's status word: 15 or 24 is the
	 *                         player confirming, and the load goes through; 28
	 *                         is a cancel, and the game mode goes back
	 */
	if (SF_BYTE(SF_API_LOAD) == 1) {
		if (SF_WORD(SF_FRAME_TIMER) == 0) {
			sf_fill_if_empty();

			SF_WORD(SF_FRAME_TIMER) = 4;
			SF_WORD(SF_PREV_GAME_MODE) = SF_WORD(SF_GAME_MODE);
			SF_WORD(SF_GAME_MODE) = 3;
		} else if (SF_WORD(SF_FRAME_TIMER) == 1) {
			sf_restore_saved_game(0, (void *)SF_ASIDE_ADDR);
			SF_BYTE(SF_API_LOAD) = 2;
		}
	} else if (SF_BYTE(SF_API_LOAD) == 2) {
		if (SF_WORD(SF_MAGIC_ADDR) == 15 || SF_WORD(SF_MAGIC_ADDR) == 24) {
			SF_BYTE(SF_API_LOAD) = 0;
			sf_magic_load();
		} else if (SF_WORD(SF_MAGIC_ADDR) == 28) {
			SF_BYTE(SF_API_LOAD) = 0;
			SF_WORD(SF_GAME_MODE) = SF_WORD(SF_PREV_GAME_MODE);
		}
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
