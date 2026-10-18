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
 * The reset takes 0x9808C0..0x980AC0, 512 bytes: the part that runs on the
 * game's main thread, sf_trophy_reset. Right under it, 0x9807F0..0x9808B0, is
 * the console's Fast loads cave (RAC3_FASTLOAD_CAVE in src/games/rac3.h).
 * build.sh fails a reset that would run past SF_CAVE_TROPHY_END, the helper's
 * own cave.
 *
 * The part that runs on the helper's own thread (sf_trophy_worker, what it
 * calls, and its two descriptors) did not fit beside it, and has a third cave,
 * the fourth section the link map places: 0x980308..0x9807F0, 1256 bytes, from
 * the first eight-byte boundary of the span to the console's Fast loads cave.
 * It was 512 bytes at 0x980380 until the thread learnt to make a registration
 * again and keep what each one did, 896 until it made the registration's
 * calls itself. It starts on an eight-byte boundary, as the descriptors in it
 * needs: build.sh fails a section the link put anywhere but its cave's first
 * byte, and one that would run past SF_CAVE_TROPHY_THREAD_END. Of the span only
 * 0x980304..0x980308 is left.
 */
#define SF_CAVE_TROPHY            0x009808C0
#define SF_CAVE_TROPHY_END        0x00980AC0
#define SF_CAVE_TROPHY_THREAD     0x00980308
#define SF_CAVE_TROPHY_THREAD_END 0x009807F0

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
 * The pause's two words, as in RaC1: SF_PAUSE_WAIT holds the low word of the
 * timebase when a load began to wait for the pause, with bit 0 set, and 0 while
 * none waits (the wait runs out after SF_PAUSE_WAIT_TICKS, sfhelper.h, ten
 * seconds), and SF_PAUSE_FRAME holds the game's mode timer as the helper read
 * it in the call whose request for the pause the game took. They follow the
 * Freeze ammo switch's flag byte at 0xD9FF10 (src/games/switches/sw_rac3.s) in
 * the same stretch of zero-filled data: nothing in the executable names
 * anything from 0xD9E9E0 to 0xDA0120, so they are zero at every boot and an
 * RPCS3 patch need not write them.
 */
#define SF_PAUSE_WAIT     0x00D9FF14
#define SF_PAUSE_FRAME    0x00D9FF18

/*
 * Build 53: the trophy reset's request byte, after the pause's words in the same
 * zero-filled stretch. The old trainer's request was the word at 0xD9E020, which
 * nothing in the executable names either; the reset's byte moved here, beside the
 * helper's others, so the console install zeroes it with them and a stray write
 * of the old word by an old tool starts nothing.
 *
 *   0  idle, or the reset is done and the game has its trophies back
 *   1  reset now (the Reset trophies button)
 *   2  reset only if the game is in a planet load's transition (the auto toggle,
 *      which asks as a load into Veldin starts, before a run's timer starts at
 *      that load's end)
 *   3  the reset is running, on the helper's own thread; that thread writes 0
 *      or 4 when it is done
 *   4  the reset ran, but the game's trophies did not come back: every one of
 *      its registrations failed
 *   6  the helper's thread has destroyed the game's trophy context and waits
 *      for qwark to delete the console's trophy data for the game
 *   7  qwark is done with that folder, deleted or not (qwark writes it); the
 *      thread writes 3 again and registers
 *   SF_API_REFUSED  the helper dropped the request: the game's trophy object is
 *      not set up, one of its trophy threads is running, a reset is already
 *      running, a 2 found the game out of the load, or the helper's thread
 *      could not be started
 *
 * 5 is not used: it was "a registration still running when the helper stopped
 * waiting", which the helper's own registration (below) cannot leave behind.
 *
 * The byte beside it, SF_TROPHY_BUSY, is 1 from the frame the helper takes a
 * request until its thread has finished, whatever happens to the request byte
 * in between (the console install zeroes that one): the helper never runs two
 * resets at once.
 */
#define SF_API_TROPHY          0x00D9FF1C
#define SF_TROPHY_BUSY         0x00D9FF1D
#define SF_TROPHY_NOW          1
#define SF_TROPHY_IN_LOAD      2
#define SF_TROPHY_RUNNING      3
#define SF_TROPHY_UNREGISTERED 4
#define SF_TROPHY_DESTROYED    6
#define SF_TROPHY_DELETED      7

/*
 * What the helper's thread leaves of each reset for qwark's log, in the same
 * zero-filled stretch, kept up to date as it goes, so that qwark can log each
 * step as it happens and a reset that never finishes still shows where it is:
 *
 *   0xD9FF1E  the registrations it has started (SF_TROPHY_TRIES)
 *   0xD9FF1F  what it is doing now (SF_TROPHY_PHASE, SF_TROPHY_PHASE_*)
 *   0xD9FF20  what the first destroy's sceNpTrophyDestroyHandle and
 *             sceNpTrophyDestroyContext answered, a word each
 *             (SF_TROPHY_DESTROY_ST); 0 for a call not made
 *   0xD9FF28  for each registration, a word: what its call that failed
 *             answered, or 0 (SF_TROPHY_STATUS)
 *   0xD9FF34  for each registration, a byte: how far it got
 *             (SF_TROPHY_STEPS, SF_TROPHY_STEP_*)
 *   0xD9FF38  for each registration, a word: what its
 *             sceNpTrophyGetRequiredDiskSpace answered (SF_TROPHY_SPACE_ST)
 *   0xD9FF44  four bytes: how many times sceNpTrophyRegisterContext called back
 *             with the install's progress, and the last call's status,
 *             completed and total (SF_TROPHY_CB), each at most 255
 *   0xD9FF48  what the last destroy before a later registration answered, as
 *             at 0xD9FF20 (SF_TROPHY_LAST_DESTROY_ST)
 *
 * 0xD9FF37 stays 0. Nothing in the executable names 0xD9E9E0..0xDA0120.
 */
#define SF_TROPHY_TRIES        0x00D9FF1E
#define SF_TROPHY_PHASE        0x00D9FF1F
#define SF_TROPHY_DESTROY_ST   0x00D9FF20
#define SF_TROPHY_STATUS       0x00D9FF28
#define SF_TROPHY_STEPS        0x00D9FF34
#define SF_TROPHY_SPACE_ST     0x00D9FF38
#define SF_TROPHY_CB           0x00D9FF44
#define SF_TROPHY_LAST_DESTROY_ST 0x00D9FF48

#define SF_TROPHY_PHASE_DESTROY  1
#define SF_TROPHY_PHASE_DELETE   2
#define SF_TROPHY_PHASE_SETTLE   3
#define SF_TROPHY_PHASE_CREATE   4
#define SF_TROPHY_PHASE_SPACE    5
#define SF_TROPHY_PHASE_REGISTER 6
#define SF_TROPHY_PHASE_DONE     8

/*
 * Phase 7 and step 4 were the game's own reader of the game and trophy info
 * (0x9850E0) and its failure, which the reset no longer calls (below); qwark
 * still names them, for a log from the build before.
 */
#define SF_TROPHY_STEP_CONTEXT_FAILED  1
#define SF_TROPHY_STEP_HANDLE_FAILED   2
#define SF_TROPHY_STEP_REGISTER_FAILED 3
#define SF_TROPHY_STEP_REGISTERED      5

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
 * It asks the game for it, as RaC2's did until build 53 changed that
 * (sf_rac2.h): the pause menu is set up by the mode change at the end of the
 * frame (0x229998, at 0x1D85E0: the leaving mode's exit routine, then the
 * pause's entry 0x2D2464 with the screen asked for), which also puts the mode
 * timer back to 0 when the mode changed (0x1D8604). RaC2's helper writes the
 * mode now, which is enough there because it runs last in its frame. This one
 * runs first: the frame then runs the update and render of the mode it began
 * in whatever the helper writes, the dispatch having picked them already, so a
 * pause written here would need the pause menu's entry run by hand, outside
 * the mode change whose target it reads (0x2298B0), and the old mode's update
 * in the same frame could queue a change that takes the pause away again
 * before the load. It stays with the mode change, and with what sf_rac2.h
 * found that costs a load during a cutscene: here too a movie's exit
 * (0x28A0A4) runs the cutscene's callback (0xC1E984) on the way into the
 * pause. From the NPEA00387 executable; not yet run on hardware.
 *
 * mode_requestChange (0x229598: mode, 1 to push it, the screen, 0, 0) answers
 * 0 when it took the change and refuses while another change is queued, with
 * eight modes pushed, while the game blocks the pause (0xEE9318[3]) or, in
 * single player, while the player has no health; the helper asks again on its
 * next call. The Start button asks for mode 3 with screen 1, or 5 on the vid
 * comics, planets 30 to 36 (0x28723C, 0x14AD88).
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
 *   +4     registered, a byte: 1 once the register thread has read the game's
 *          trophy info (0x9850E0, after sceNpTrophyGetGameInfo), 0 after a
 *          destroy or a failure. Nothing is awarded while 0: the award
 *          (0x98535C) drops the request, it does not queue it
 *   +8+8i  trophy i's id, and +12+8i its unlocked flag: the game's cache of the
 *          console's trophy state, which the register thread fills from
 *          sceNpTrophyGetTrophyInfo (0x9850E0, the flag at 0x985318) and the
 *          unlock thread sets when an unlock succeeds (0x9859E8). The award
 *          routine (0x2290D8, ids 0..32) asks for nothing whose flag is set,
 *          nor does the skill points' award of trophy 2 (UnlockSkillPoint,
 *          0x229228), and the unlock thread (0x9858BC) checks it again. Those
 *          two are the only callers of the award, 0x98535C
 *   +392   -1 from main's init until the first register thread starts, 1 while
 *          the register thread (0x9856F8) runs, 2 while the unlock thread
 *          runs, 0 when neither does
 *   +400   1 from main's init: trophies are on. The award checks it with +4
 *   +428   the sceNpTrophy context, +432 its handle
 *   +436   SceNpTrophyGameDetails, whose first word is the trophy count, 33
 *
 * 0x984E40 destroys the handle and the context and clears +4. 0x984FA0 calls
 * sceNpTrophyInit (which answers "already initialised" after boot), makes a new
 * context and handle for the game's NP communication id and starts the register
 * thread, "NPTrophyRegister" (priority 1001, 16 KB of stack), and returns. That
 * thread does the slow part: sceNpTrophyGetRequiredDiskSpace, then
 * sceNpTrophyRegisterContext, which installs the trophy set when the console
 * has none, then the game info and a sceNpTrophyGetTrophyInfo per trophy with
 * 32 ms between them (a second for the 33), and puts +392 back to 0 at the very
 * end. Both functions are the game's own, called on the main thread at boot
 * (0x15434) and at exit (0x13704); nothing else in the executable calls an
 * sceNpTrophy function.
 *
 * What the reset does comes from rc3-save 1.1's handler, rack.bin 0x980E08:
 * destroy; mark every trophy unlocked in the cache, then trophies 10 (the
 * RY3NO), 25 (the Metropolis blimp) and 32 (the snowman skin) locked again;
 * register; destroy; register. With the cache fill at 0x985318 a nop, which
 * qwark writes before it asks, the registrations leave the cache as the reset
 * set it, so those three are the only trophies the game will try to award in
 * the run that follows, and it does try. Whether the console then shows them,
 * and spends the time doing it that the runs rely on, is the console's trophy
 * data, which qwark deletes (rac3_panel.c).
 *
 * rc3-save ran it on the game's main thread with a second's sleep before each
 * register and before the second destroy (lv2 sys_timer_sleep, 1), so the game
 * stood still for three seconds, and on hardware build 53's port did the same.
 * The helper runs it on a thread of its own (sfhelper.c has how).
 *
 * What the hardware tests of that thread showed, and what the reset does now:
 *
 *   - qwark deleted the folder first, under the game's live, registered
 *     context, as the old trainer did, and the helper then destroyed that
 *     context and had the game's register thread register a new one. With no
 *     sleeps (destroy, register, wait, destroy, register) two resets of one
 *     boot worked and the four of the next all failed; with a second's sleep
 *     before each register, every reset of six went the same way: the first
 *     registration's thread ended about a second in with +4 never up, and the
 *     next one's was still running forty-five seconds on. Every way out of the
 *     register thread puts +392 back to 0 (0x985898, right before its
 *     sys_ppu_thread_exit: a failed GetRequiredDiskSpace is only printed, a
 *     failed RegisterContext skips the game info, and 0x9850E0 returns from
 *     each of its failures), nothing else writes +392 but main's init (-1) and
 *     the unlock thread (2, then 0), and no unlock thread can start while a
 *     reset runs; so that thread was still inside one of its calls. The game
 *     did not award anything meanwhile: a reset holds every award off until it
 *     is over.
 *   - So the order is the one a console expects: the helper's thread destroys
 *     the context first, and only then does qwark delete the folder (request
 *     byte 6, then qwark's 7), so the system never sees a registered set's
 *     files go from under a live context; and then it registers, which with
 *     no data for the set on the console installs it afresh, as on a console
 *     that never had the game.
 *   - The thread makes the registration's calls itself, the ones the game's
 *     register thread makes, from the same stubs: CreateContext and
 *     CreateHandle into the object, as 0x984FA0 does,
 *     GetRequiredDiskSpace, and RegisterContext with a status callback of the
 *     helper's own. It leaves out the game's boot check (0x97EC94:
 *     cellGameBootCheck and cellGameContentPermit, which the game means to call
 *     once, at boot, and the register thread calls on every registration) and
 *     sceNpTrophyInit, which answers "already initialised". Each call's answer
 *     goes where qwark logs it, so the next log says exactly which call fails
 *     and with what, and the registration is over when the call returns: no
 *     thread word to watch, and no register thread left behind to share +392
 *     with the next or destroy another's context on its failure path. +392 is
 *     1 while it registers, as it is while the game's register thread does.
 *   - Before each registration it waits a second after the destroy, as
 *     rc3-save did (SF_TROPHY_SETTLE_US), and it makes up to three, stopping
 *     at the first that leaves the game registered.
 *
 * The console's log of that order, two resets alike: the first registration
 * after the delete always fails, its RegisterContext answering 0x80022D1E
 * (not one of the documented SCE_NP_TROPHY_ERROR codes, 0x800229xx) a second
 * in, and the second, a destroy and a second later, installs the set in about
 * ten seconds and registers; so rc3-save's second registration was the one
 * that counted all along, and the retry stays. What took the time after that
 * was the game's own reader of the game and trophy info (0x9850E0, which the
 * register thread calls next): about thirty-five seconds, its
 * GetGameInfo and each of its 33 GetTrophyInfo calls taking a second each on
 * the freshly installed set, where at boot all 33 take one. For all that time
 * the game awarded nothing, and a trophy earned a quarter of a minute after
 * the reset never popped.
 *
 * The reset does not call it any more. With the cache fill at 0x985318 a nop
 * it writes nothing the reset keeps: each trophy's id at +8+8i, which it
 * writes again with the same number, and the game details at +436, which
 * hold the same set's numbers since boot. What it does that counts is put +4
 * up once the game info has been read, and the thread does that itself the
 * moment RegisterContext has answered 0. Nothing the award needs comes from
 * it: the award (0x98535C) reads +4, +400 (which only the save code at
 * 0x9786E8..0x97AC30 writes), the count at +436, the cache flags and its queue
 * at +404; the unlock thread (0x9858BC) the context and handle at +428 and
 * +432, which CreateContext and CreateHandle have just written, and the
 * flags. So the trophies are back as soon as the set is installed, about
 * thirteen seconds after the request.
 *
 * The order changes in one place against rc3-save: 10, 25 and 32 go back to
 * locked only once the last registration is over, so the game awards nothing
 * while the reset runs, and the game's registered byte goes to 0 on the main
 * thread as the request is taken (sfhelper.c has why). From the NPEA00387
 * executable; not yet run on hardware.
 */
#define SF_TROPHY_OBJ_PTR    0x00CB150C
#define SF_TROPHY_REGISTERED 0x00000004
#define SF_TROPHY_FLAGS      0x0000000C
#define SF_TROPHY_THREAD     0x00000188
#define SF_TROPHY_COUNT      0x000001B4
#define SF_TROPHY_MAX        33
#define SF_TROPHY_KEEP_1     10
#define SF_TROPHY_KEEP_2     25
#define SF_TROPHY_KEEP_3     32

/*
 * The context's id and the handle's, which CreateContext and CreateHandle write
 * into the object and every trophy call of the game's reads from it. The
 * game's destroy (0x984E40) leaves both as they were; the helper's thread puts
 * them to 0 after its own, so nothing destroys a dead id a second time and a
 * context that was not made reads as none, as the game's destroy and failure
 * paths take a 0.
 */
#define SF_TROPHY_CONTEXT    0x000001AC
#define SF_TROPHY_HANDLE     0x000001B0

/*
 * The game's NP communication id block (rac3.h has where it comes from): its +4
 * is the SceNpCommunicationId and its +16 the signature, which 0x984FA0 hands
 * sceNpTrophyCreateContext.
 */
#define SF_TROPHY_COMMID_PTR  0x00CBAA04
#define SF_TROPHY_COMMID_ID   0x00000004
#define SF_TROPHY_COMMID_SIGN 0x00000010

/*
 * The thread's waits: a second after each destroy, and up to thirty seconds
 * (3000 polls of 10 ms) for qwark to answer 6 with 7; past that it registers
 * anyway, the game's trophies before anything.
 */
#define SF_TROPHY_POLL_US     10000
#define SF_TROPHY_DELETE_POLLS 3000
#define SF_TROPHY_SETTLE_US   1000000
#define SF_TROPHY_TRIES_MAX   3

/*
 * The helper's thread, made as the game makes its own trophy threads (0x984FA0,
 * 0x98535C): the game's priority, 1001 (the main thread's too, from the
 * process parameters), 16 KB of stack, not joinable, and the register thread's
 * name, "NPTrophyRegister" at 0xB16ACC, since the helper may carry no string of
 * its own. Its descriptor's TOC is the game's, 0xC0A4E0, which every
 * descriptor in the game's .opd carries (0xBF83A8, the register thread's,
 * among them): the game routines the thread calls run on it.
 */
#define SF_TROPHY_THREAD_PRIO  0x000003E9
#define SF_TROPHY_THREAD_STACK 0x00004000
#define SF_TROPHY_THREAD_NAME  0x00B16ACC
#define SF_GAME_TOC            0x00C0A4E0

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY       0x0099256C
#define SF_FN_PERFORM_LOAD 0x001E1CF4
#define SF_FN_REQUEST_MODE 0x00229598

/*
 * Build 53: the game's import stubs for the sceNpTrophy calls the reset makes
 * (the module's stub table at 0xBD2034, FNIDs at 0xAEFCC4). The stubs take
 * the game's r2 as the thread-create stub below does.
 */
#define SF_FN_NP_TROPHY_REGISTER_CONTEXT 0x00AEE484
#define SF_FN_NP_TROPHY_CREATE_HANDLE    0x00AEE4A4
#define SF_FN_NP_TROPHY_DISK_SPACE       0x00AEE4C4
#define SF_FN_NP_TROPHY_DESTROY_CONTEXT  0x00AEE4E4
#define SF_FN_NP_TROPHY_DESTROY_HANDLE   0x00AEE544
#define SF_FN_NP_TROPHY_CREATE_CONTEXT   0x00AEE584

/*
 * Build 53: the game's import stubs for sysPrxForUser's sys_ppu_thread_create
 * and sys_ppu_thread_exit, the ones its trophy code calls (0x98509C,
 * 0x98589C). A stub loads the library's TOC into r2 and leaves the caller to
 * take its own back from 40(r1), where the stub saved it, as every call to one
 * in the game does with `ld r2,40(r1)`; sfhelper.c does the same.
 */
#define SF_FN_PPU_THREAD_CREATE 0x00AEF604
#define SF_FN_PPU_THREAD_EXIT   0x00AEF824

#endif /* QWARK_SF_RAC3_H */
