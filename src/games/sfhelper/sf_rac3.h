/*
 * Ratchet & Clank 3, Up Your Arsenal (NPEA00387): the savefile helper's address
 * table.
 *
 * From https://github.com/Torrbullen/Gigahelper: src/c/mod.c for the logic and
 * the addresses, src/asm/hooks.s and Makefile for the cave and the hook. The
 * cave and the hook site agree with racman's mods/NPEA00387/rc3-save/patch.txt.
 *
 * What was left out of the port, and why:
 *
 *   the tempsave file  Gigahelper's api_savefile and api_loadfile bytes wrote and
 *            read /dev_hdd0/game/NPEA00387/USRDIR/tempsave through syscalls. That
 *            is the whole of what qwark replaces, so both bytes are gone; only
 *            0xD9FF03 and 0xD9FF04 are left unused where they were.
 *   the fast-load restore  after its load it left api_load at 2 and, thirty
 *            planet-timer ticks later, wrote the two fast-load words. Fast loads
 *            are a qwark feature with a checkbox of their own in this game, and
 *            the helper writing them behind the user's back would fight it. The
 *            planet_timer reset that armed the countdown goes with it.
 *   the intro skip  two ASM patch words in that repo's patch.txt (0x1113C,
 *            0x9027A0) that belong to Gigahelper's other features, not to the
 *            savefile path.
 *
 * Build 53 carries the trophy reset that racman's rc3-save 1.1 added to the mod
 * (its rack.bin 0x980E08, and the third ASM word, 0x985318): see "the trophy
 * reset" below and sf_trophy_reset in sfhelper.c. qwark writes the 0x985318 word
 * itself, on a console, just before it asks for a reset (src/games/rac3_panel.c),
 * so it is not one of this helper's patch words.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC3_H
#define QWARK_SF_RAC3_H

/* ------------------------------------------------------------------- caves */

/*
 * CODE_CAVE_START in Gigahelper's Makefile, which says the cave runs to
 * 0x983754. It does not: the dead function it starts in ends at 0x981064, and
 * 0x981064..0x983754 is live code.
 *
 * The Freeze ammo code switch has its cave in the same dead function, at
 * 0x980F00 (src/games/switches/sw_rac3.ld), so the helper stops there: build.sh
 * fails a helper that would run past SF_CAVE_END. Up to build 52 nothing held
 * it to that, at 372 bytes it did not need it; the pause made it grow.
 */
#define SF_CAVE     0x00980AC0
#define SF_CAVE_END 0x00980F00

/*
 * Build 53: the trophy reset's own cave, the second of this helper's two
 * (sf_trophy_reset in sfhelper.c, which the helper's entry calls). It is the top
 * of 0x980304..0x980AC0, which is strictly dead: the seven functions there
 * (0x980304, 0x980380, 0x9804C8, 0x98055C, 0x9805C4, 0x98072C, 0x9807CC) are
 * reached by no branch but from 0x980AC0 and 0x981120, which nothing reaches
 * either; no word in the executable's data, and no lis/addi pair in its code,
 * names any of them or their descriptors at 0xBF8148..0xBF817F. No mod racman
 * ever shipped for this game puts anything below 0x980AC0.
 *
 * The reset takes 0x9808C0..0x980AC0, 512 bytes, and is 412 of them; the rest
 * of the span, 0x980304..0x9808C0, is left for whatever needs it next. build.sh
 * fails a reset that would run past SF_CAVE_TROPHY_END, the helper's own cave.
 */
#define SF_CAVE_TROPHY     0x009808C0
#define SF_CAVE_TROPHY_END 0x00980AC0

/*
 * The hook site is the cellPadSetActDirect call Gigahelper replaces with a call
 * of its own. Relative, as its `bl hook` is.
 *
 * The site is in the pad object's read (0x97C828, the method at 0xB25F70 that
 * 0x235950 calls for each pad), and 0x235950 is the first thing every mode's
 * frame runs: the helper runs before that frame's update and render, as RaC1's
 * does, and once a frame for each pad that reaches the site.
 */
#define SF_HOOK_ADDR     0x0097CBA4
#define SF_HOOK_ABSOLUTE 0

/* -------------------------------------------------------- the request bytes */

#define SF_API_MOD      0x00D9FF00
#define SF_API_LOAD     0x00D9FF01
#define SF_API_SETASIDE 0x00D9FF02

/*
 * The copy loop's counter. Gigahelper kept its own `i` in the globals region at
 * 0xD9E000; this one sits just past the five request bytes, in the same region
 * and aligned, so the helper needs nothing outside its caves and these words.
 */
#define SF_API_I 0x00D9FF08

/*
 * The pause's two words, as in RaC1 and RaC2: SF_PAUSE_WAIT counts the calls a
 * load has waited for the pause, 0 while none waits, and SF_PAUSE_FRAME holds
 * the game's mode timer as the helper read it in the call whose request for the
 * pause the game took. They follow the Freeze ammo switch's flag byte at
 * 0xD9FF10 (src/games/switches/sw_rac3.s) in the same stretch of zero-filled
 * data: nothing in the executable names anything from 0xD9E9E0 to 0xDA0120,
 * so they are zero at every boot and an RPCS3 patch need not write them.
 */
#define SF_PAUSE_WAIT     0x00D9FF14
#define SF_PAUSE_FRAME    0x00D9FF18
#define SF_PAUSE_WAIT_MAX 600

/*
 * Build 53: the trophy reset's request byte, after the pause's words in the same
 * zero-filled stretch. The old trainer's request was the word at 0xD9E020, which
 * nothing in the executable names either; the reset's byte moved here, beside the
 * helper's others, so the console install zeroes it with them and a stray write
 * of the old word by an old tool starts nothing.
 *
 *   0  idle
 *   1  reset now (the Reset trophies button)
 *   2  reset only if the game is in a planet load's transition (the auto toggle,
 *      which asks as a load into Veldin starts, before a run's timer starts at
 *      that load's end)
 *   3  the helper is resetting; it writes 0 when it is done
 *   SF_API_REFUSED  the helper dropped the request: the game's trophy object is
 *      not set up, one of its trophy threads is running, or a 2 found the game
 *      out of the load
 */
#define SF_API_TROPHY       0x00D9FF1C
#define SF_TROPHY_NOW       1
#define SF_TROPHY_IN_LOAD   2
#define SF_TROPHY_RUNNING   3

/* ------------------------------------------------------- the aside buffer */

#define SF_ASIDE_ADDR 0x01100000
#define SF_ASIDE_SIZE 0x00200000

/* --------------------------------------------------------- game addresses */

/*
 * savedata_buf is the word at +4 of the block savedata_info points at.
 *
 * Neither is there from the start, as in RaC2: the block is the save manager
 * the getter 0x976FE8 allocates, 0 until the first call, and its +4 is 0 until
 * the game's first save or load through the save-data utility copies the
 * buffer pointer (+1120) into it (0x977D5C, 0x978F2C, 0x9792A4, 0x9795DC). Up
 * to this build the helper read through both regardless: a set-aside before
 * then, or a load with nothing set aside, copied two megabytes from address 0.
 */
#define SF_SAVEDATA_INFO  0x00CB0A98
#define SF_SAVEDATA_FIELD 0x00000004

/*
 * The first two words of a save are the sizes of its global block and of one
 * planet's block, computed by the restore (0x1DF5CC, which perform_load calls
 * at 0x1E1D94) from the field tables at 0xC3CC8C and 0xC3CF2C. The restore
 * accepts those or the two older values beside them - RaC2's, 0x1CF8 and
 * 0xCD0 - and refuses anything else without loading. The game's own saves
 * start 00 00 37 B0 00 00 84 C0.
 */
#define SF_SAVE_HEAD_GLOBAL     0x000037B0
#define SF_SAVE_HEAD_GLOBAL_OLD 0x00001CF8
#define SF_SAVE_HEAD_PLANET     0x000084C0
#define SF_SAVE_HEAD_PLANET_OLD 0x00000CD0

/*
 * The game's modes, RaC2's scheme and Deadlocked's ancestor, whose gameMode_t
 * names them. gameMode is what the main loop (0x1D6CE0) dispatches on, at
 * 0x1D7934, and each mode's frame calls the pad routine first:
 *
 *   0  ordinary play  update 0x286D08, render 0x1220BC (the world, 0x121610);
 *                     -1, debug, runs the same, and the mode change traps on
 *                     leaving it (0x229A8C), so the helper never asks there
 *   1  movie          0x28A284
 *   2  scene          0x289008, 0x122154 (the world)
 *   3  pause          the pause menu: 0x2D2CF8, 0x1223AC
 *   4  freeze         a message over a stopped game: 0x144DC0 runs it and
 *                     0x122190 draws it, over the world (0x121610) unless it
 *                     came up over the pause
 *   5  vendor         0x2960F8, 0x29B950
 *   6  space          the transition a planet load runs, 0x271AF8 and 0x273B14
 *   7..9              0x287CFC/0x12227C, 0x2A9F90/0x2A9E70, 0x1BAB68/0x1BAB8C
 *  10 and up          the lobby and the other multiplayer modes
 *
 * Freeze does not update the world but draws it; the pause alone runs neither
 * the world's update nor its render, nor anything that reaches them, and the
 * helper loads from the pause.
 *
 * It asks for it the way RaC2's does and for the same reason (sf_rac2.h): the
 * pause menu is set up by the mode change at the end of the frame (0x229998,
 * at 0x1D85E0: the leaving mode's exit routine, then the pause's entry 0x2D2464
 * with the screen asked for), which also puts the mode timer back to 0 when the
 * mode changed (0x1D8604). mode_requestChange (0x229598: mode, 1 to push it,
 * the screen, 0, 0) answers 0 when it took the change and refuses while another
 * change is queued, with eight modes pushed, while the game blocks the pause
 * (0xEE9318[3]) or, in single player, while the player has no health; the
 * helper asks again on its next call. The Start button asks for mode 3 with
 * screen 1, or 5 on the vid comics, planets 30 to 36 (0x28723C, 0x14AD88).
 * pendingGameMode is -2 when nothing is queued.
 *
 * A planet load leaves the main loop at the end of the frame that finds the
 * load request word set (0x1D86AC); perform_load raises it for the save's
 * planet (0x1E2048, or through 0x14AA54 for Veldin). The game loop then
 * clears the mode stack before the next phase (0x13E10, at 0x154F4), and the
 * main loop starts a single-player planet in ordinary play with nothing queued
 * (0x1D7058), so the pause ends with the load.
 */
#define SF_GAME_MODE         0x00EE9334
#define SF_PENDING_GAME_MODE 0x00C1E840
#define SF_MODE_TIMER        0x00EE9330
#define SF_LOAD_REQUEST      0x00EE9310
#define SF_CURRENT_PLANET    0x00C1E438

#define SF_MODE_PAUSE     0x00000003
#define SF_MODE_SPACE     0x00000006
#define SF_MODE_LAST      0x00000009
#define SF_MODE_NONE      0xFFFFFFFE
#define SF_PAUSE_SCREEN   1

/* The vid comics' pause, and the planets that have it. */
#define SF_PAUSE_SCREEN_COMIC 5
#define SF_PLANET_COMIC_FIRST 30
#define SF_PLANET_COMIC_COUNT 7

/* ------------------------------------------------------- the trophy reset */

/*
 * The game's trophy object, the static at 0xD1CC14 that main's init (0x15914)
 * points this word at before anything else runs. Its layout, from the routines
 * that use it:
 *
 *   +4     registered: 1 once the register thread has read the game's trophy
 *          info, 0 after a destroy or a failure. Nothing is awarded while 0
 *   +8+8i  trophy i's id, and +12+8i its unlocked flag: the game's cache of the
 *          console's trophy state, which the register thread fills from
 *          sceNpTrophyGetTrophyInfo (0x9850E0, the flag at 0x985318) and the
 *          unlock thread sets when an unlock succeeds (0x9859E8). The award
 *          routine (0x2290D8, ids 0..32) asks for nothing whose flag is set,
 *          and the unlock thread (0x9858BC) checks it again
 *   +392   1 while the register thread (0x9856F8) runs, 2 while the unlock
 *          thread runs, 0 when neither does
 *   +428   the sceNpTrophy context, +432 its handle
 *   +436   SceNpTrophyGameDetails, whose first word is the trophy count, 33
 *
 * 0x984E40 destroys the handle and the context and clears +4. 0x984FA0 makes a
 * new context and handle for the game's NP communication id and starts the
 * register thread, which registers the context (installing the trophy set if
 * the console has none) and fills the cache. Both are the game's own, called at
 * boot (0x15434) and at exit (0x13704), and both run on the calling thread.
 *
 * What the reset does is rc3-save 1.1's handler, rack.bin 0x980E08, call for
 * call: destroy; mark every trophy unlocked in the cache, then trophies 10 (the
 * RY3NO), 25 (the Metropolis blimp) and 32 (the snowman skin) locked again;
 * wait a second; register; a second; destroy; a second; register. With the
 * cache fill at 0x985318 a nop, which qwark writes before it asks, the
 * registrations leave the cache as the reset set it, so those three are the
 * only trophies the game will try to award in the run that follows, and it
 * does try. Whether the console then shows them, and spends the time doing it
 * that the runs rely on, is the console's trophy data: qwark deletes the game's
 * folder of it first (rac3_panel.c).
 */
#define SF_TROPHY_OBJ_PTR   0x00CB150C
#define SF_TROPHY_FLAGS     0x0000000C
#define SF_TROPHY_THREAD    0x00000188
#define SF_TROPHY_COUNT     0x000001B4
#define SF_TROPHY_MAX       33
#define SF_TROPHY_KEEP_1    10
#define SF_TROPHY_KEEP_2    25
#define SF_TROPHY_KEEP_3    32

/* rc3-save's three waits were a second each (lv2 sys_timer_sleep, 1). */
#define SF_TROPHY_WAIT_US   1000000

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY       0x0099256C
#define SF_FN_PERFORM_LOAD 0x001E1CF4
#define SF_FN_REQUEST_MODE 0x00229598

/* Build 53: the trophy object's destroy and register, both taking the object. */
#define SF_FN_TROPHY_DESTROY  0x00984E40
#define SF_FN_TROPHY_REGISTER 0x00984FA0

#endif /* QWARK_SF_RAC3_H */
