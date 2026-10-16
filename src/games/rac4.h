/*
 * Ratchet: Deadlocked (NPEA00423): the address table and the seam between rac4.c
 * (numbers, hot block, descriptors, vtable) and rac4_panel.c (the handlers).
 * Nothing outside those two files includes this.
 *
 * The addresses come from racman's RaCTrainer/Games/RAC4 (rac4.cs, RAC4Form.cs,
 * BotsUnlocksFactory.cs); the quit hook and the crash patches come from the
 * Deadlocked autosplitter SPRX, rac4-autosplitter-src/ratchetron.c. Where the
 * old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC4_H
#define QWARK_RAC4_H

#include "game.h"

/* -------------------------------------------------------- rac4.cs addresses */

#define RAC4_BOLTS             0x009C32E8u
#define RAC4_DREAD_POINTS      0x009C32F4u
#define RAC4_SKIN              0x009C32FBu  /* one byte, 22 skins */
#define RAC4_BADGES            0x009C3308u  /* six bytes */
#define RAC4_CHALLENGE_MODE    0x009C330Eu  /* one byte */
#define RAC4_RANGE             0x009C331Fu  /* one byte, 0 Marauder .. 4 Liberator */
#define RAC4_BOTS_UNLOCK_SAVE  0x009C3325u  /* 16 bytes, the saved copy */
#define RAC4_CAMERA_MODE       0x009C287Cu
#define RAC4_LOAD_PLANET_OLD   0x009C3240u  /* the autosplitter's "current planet" */
#define RAC4_BOTS_UNLOCK       0x009D2775u  /* 16 bytes, the live copy */
#define RAC4_CURRENT_CHALLENGE 0x009D195Cu

#define RAC4_SKIN_APPLY        0x0110D975u  /* written 1 after the skin byte */

#define RAC4_INPUTS            0x00B11F30u  /* u32 pad mask, OG layout */
#define RAC4_ANALOGS           0x00B1210Cu  /* f32 rx, ry, lx, ly */

#define RAC4_IN_GAME           0x00B1F460u  /* 0 in the main menu, 1 in game */
#define RAC4_PREV_PLANET       0x00B1F465u
#define RAC4_TUTORIAL_FLAGS    0x00B1F46Cu  /* the softlock fix watches byte +3 */
#define RAC4_IS_LOADING        0x00B0FD84u

#define RAC4_LEVEL_SAVES       0x00B27FF0u  /* MF_LevelSave[15], 0x304 each */
/*
 * g_GadgetData, the live weapon table: GadgetEntry[32], sixty-eight bytes each,
 * a big-endian s16 level at +0 and a big-endian s16 ammo count at +2.
 *
 * The level halfword is one below the level the game shows: V1 is 0, V2 is 1,
 * and so on up to V99 at 98. A weapon the player does not have reads -1, which
 * is why the halfword has to be read signed — an unsigned read makes a locked
 * weapon look like V65536. Checked on hardware, along with the fact that a
 * write here survives a game save, so unlike the bot upgrades there is no
 * second copy to keep in step.
 *
 * Entries 17 and 18 are not weapons but the Charge Boots and the Gravity
 * Boots, and they share the encoding: locked at -1, owned at 0. A community
 * address list gives them as single bytes, "255 = Locked, 0 = Unlocked", at
 * 0xB2BBE5 and 0xB2BC29, which are the low bytes of those two level halfwords.
 * Boots have no version to raise and no ammunition, so the unlock table gives
 * them an owned checkbox and nothing else.
 *
 * It sits at +0xA30 inside the object at 0xB2AD30, which nothing here needs:
 * the absolute address is the seam.
 */
#define RAC4_GADGETS           0x00B2B760u  /* GadgetEntry[32], 0xB2B760..0xB2BFE0 */
#define RAC4_GAME_STATE        0x00B3C5A0u  /* the fast-load restore watches +3 */
#define RAC4_FRAME_COUNTER     0x00B3C59Cu  /* the IL timer readout */
#define RAC4_LOAD_PLANET2      0x00B36DCCu  /* write 1 to start the load */
#define RAC4_TARGET_PLANET     0x00B36DD0u  /* the planet to load */
#define RAC4_CUTSCENE_PTR      0x00B36DE8u

#define RAC4_MOBY_TABLE        0x00C845A0u
#define RAC4_MOBY_TABLE_END    0x00C845A4u
#define RAC4_MOBY_STRIDE       0x100u

/*
 * Everything from here to RAC4_PLAYER_STATE is a field of the one static Hero
 * object the local player owns, which the game's own symbols call MyHero and
 * which lives at 0x010D4430: the EBOOT builds its address with `lis rX,0x10d;
 * addic rX,rX,0x4430` (0x0013FFD0, 0x002E2E60 and a dozen more sites). The four
 * addresses RaCMAN already knew line up with that struct exactly, which is what
 * makes the field offsets below trustworthy: +0x00A0 is the position, +0x25CC
 * the state byte RAC4_PLAYER_STATE names, +0x2E20 the hit points
 * RAC4_PLAYER_HEALTH names and +0x2F04 the three debug-teleport floats
 * RAC4_COORDS2 names.
 *
 * Build 53: +0x2F24 is the maximum the hit points are filled to, a float like
 * them. The hero's setup writes both from the same float (0x3421A0, 0x3421B8),
 * the game fills the hit points from it (0x17CAC, 0x1A818, 0x1D7C90), and
 * other code writes maxima of its own to it (50.0 at 0x2EF8FC). Freeze health
 * holds the hit points at it (classic.h); the old form froze the integer 200
 * into the float, which reads as 0.
 */
#define RAC4_COORDS            0x010D44D0u  /* +0x00A0 Vec4 pos then +0x00B0 Vec4 rot */
#define RAC4_COORDS2           0x010D7334u  /* +0x2F04 cheatX/Y/Z, position only */
#define RAC4_GHOST_TIMER       0x010D47CEu
#define RAC4_CAM_YAW           0x010D5DE0u  /* +0x19B0 f32 camera yaw, left/right */
#define RAC4_CAMERA_LR         0x010D5DF0u  /* +0x19C0 NOT a rotation, see below */
#define RAC4_CAMERA_UD         0x010D5E00u  /* +0x19D0 f32 camera pitch, up/down */
#define RAC4_PLAYER_HEALTH     0x010D7250u  /* +0x2E20 f32, the hit points */
#define RAC4_MAX_HEALTH        0x010D7354u  /* +0x2F24 f32, their maximum */
#define RAC4_PLAYER_STATE      0x010D69FCu

#define RAC4_CURRENT_PLANET    0x0119353Cu  /* 0 in the main menu */
#define RAC4_SAVE_INFO         0x011B1BD8u
#define RAC4_CHALLENGE_SEL     0x011A0AE0u
#define RAC4_SOFTLOCK          0x011C04C0u  /* written 0 on a fresh file */

/*
 * Act tuning. Each is the low byte of a word counter the boss's own code reads
 * with lwz and adds to (0xA947D0 at 0x38610, 0xA969E0 at 0x70DF4, 0xA96E40 at
 * 0x809B8, 0xA07C7C at 0x5CA2F0), bar the Reactor's, which is named by the
 * word itself (lwz at 0x3C444, 0x3CA54, 0x3CAD0): the old button wrote a byte
 * there, which is the counter's high byte, and build 53 writes the word.
 */
#define RAC4_TUNE_SHELLSHOCK   0x00A947D3u
#define RAC4_TUNE_REACTOR      0x00A94944u  /* the whole word */
#define RAC4_TUNE_EVISCERATOR  0x00A969E3u
#define RAC4_TUNE_ACE          0x00A96E43u
#define RAC4_TUNE_VOX          0x00A07C7Fu

/* The instruction sites RAC4Form patched. */
#define RAC4_AMMO_INSTR        0x001D7D18u  /* also the fingerprint site */

/*
 * Fast loads. RAC4Form patched two of these six words; build 53 patches all
 * six, on RaC2's model: the screen black from the start of a load to the
 * level's first frame, no flight, the level read as fast as the disk gives it,
 * and a movie the flight has queued played first, at once.
 *
 * Every planet load is Transition_DoTransition (0x2A8AA0). A single-player or
 * co-op trip out of a level first saves (memcard_Save_QE_func(0, level) at
 * 0x2A8AF4, the autosave), then calls Transition_Pre (0x2A81D0), which reads
 * the flight's files and the hero's textures and can queue a movie, and then
 * loops until its exit word r27 is non-zero. Each time round, the loop
 *
 *   - calls the transition's update through g_fnTransitionUpdate (the bcctrl
 *     at 0x2A8C50, given g_bLevelLoadAlmostDone at 0x10B7E88 in r26), copies
 *     its answer into r27 (0x2A8C5C) and calls the post-update;
 *   - draws a frame: BeginFrame (0x14A0A8, its argument set at 0x2A8CC8),
 *     Transition_Draw (0x2A6DA0) and EndFrame (0x14A0FC), which flips. The
 *     flip is the loop's only wait: sceGsSyncV (0x720238), where the PS2
 *     waited for the vertical blank, is an empty function on the PS3;
 *   - takes a step of LoadLevelData (0x1C7120) unless a movie is queued
 *     (0x2A8D1C), and sets g_bLevelLoadAlmostDone once LevelLoad.state is 10.
 *
 * A load the loop leaves unfinished, the second loop after it (0x2A8DFC)
 * finishes, calling LoadLevelData until it is done with no frame and no wait.
 *
 * The space flight's update (0x2AA9A0) answers 1 only once two segments and
 * the arrival have been flown with the load almost done, and it counts a
 * segment in frames (0x2A9908). Transition_SetupMidSceneOrMovie (0x2A7F98)
 * queues a movie in the word at 0x10B7FE0 when the save's progress has earned
 * one - Dread points, badges and the like - or on the first trip to Stygia,
 * and the save has not seen it (the bits at 0xB1F58C). The flight's
 * post-update (0x2AAB5C) plays it through 0x2AA004 when the first segment ends,
 * and 0x2AA004 plays it in a loop of its own frames (0x2B571C), fades to black
 * and clears the word. So a trip with a movie queued flew a whole segment at
 * the display's pace before the movie, and its load started after the movie.
 *
 * RAC4Form nopped the update's call, which left r26 in r3 as the answer: the
 * loop ended once the load was almost done, but a queued movie never played,
 * LoadLevelData never ran and the game sat on a black screen until fast loads
 * went off. Build 53 first let the update run and or'd r26 into the copy: the
 * movie played, but only after the first segment, so a load with one was slow
 * until it had, and every load showed the flight until the load cut it off.
 *
 * RaC2's fast loads nop its transition's draw, a call that begins a frame,
 * draws the flight and ends the frame (rac2.h, RAC2_FASTLOAD_INSTR). With it
 * gone RaC2's loop never flips, so nothing of the flight is shown; nothing in
 * the loop waits, so the flight, counted in frames, is over at once; and the
 * load steps as fast as the loop goes round. The six words here:
 *
 *   0x2A8AE8  li r3, 0        -> li r3, 2         the autosave's arguments
 *   0x2A8AEC  lwz r4, level   -> li r4, 0         become FadeToBlack's, and
 *   0x2A8AF4  bl memcard_Save -> bl 0x154B38      its call FadeToBlack
 *   0x2A8C50  bcctrl          -> bl 0x2AA004      a queued movie, no flight
 *   0x2A8C5C  ori r27, r3, 0  -> li r27, 1        once round the loop
 *   0x2A8CC8  li r3, 1        -> b 0x2A8CE8       and no frame
 *
 * FadeToBlack(2, 0) takes the screen black at once, as the game's own
 * LOADING-screen load does (0x132A8); a load the planet selector starts would
 * otherwise leave the level's last frame up while Transition_Pre reads. The
 * autosave goes, as it did in RAC4Form: it has nothing to do with the movie,
 * and a save written to the disk while the level is read from it is time out
 * of the load.
 *
 * Then the loop goes round once. A queued movie plays first, before anything
 * of the flight; with none queued 0x2AA004 returns at once. The one time round
 * takes LoadLevelData's first step, the movie having cleared the word, draws
 * and flips nothing, and leaves the rest of the level to the second loop,
 * which reads it with no wait at all: RaC2's load, here. The screen is black
 * from the fade to the level's first frame, but for a movie, which shows its
 * own frames and fades out again. Nothing waits on a movie any more: 0x2AA004
 * always clears the word and the second loop never looks at it, so the hang
 * cannot come back.
 *
 * The game takes the same road itself on that LOADING-screen load: 0x132A8
 * sets the byte the flight's update answers before anything else (+200 of
 * *0xA34290), so its transition goes round once and the second loop reads the
 * whole level.
 *
 * rac4.c lists the words in the order they go in: the call before its
 * arguments, since memcard_Save's first argument forces a save, and the
 * movie's call before the loop's exit, so the loop is never cut to once round
 * while a queued movie still waits for the flight to play it.
 *
 * From the NPEA00423 executable; not yet run on hardware.
 */
#define RAC4_FASTLOAD_FADE_R3  0x002A8AE8u  /* li r3, 0 */
#define RAC4_FASTLOAD_FADE_R4  0x002A8AECu  /* lwz r4, g_newlevelnum */
#define RAC4_FASTLOAD_SAVE     0x002A8AF4u  /* bl memcard_Save */
#define RAC4_FASTLOAD_UPDATE   0x002A8C50u  /* bcctrl: the transition's update */
#define RAC4_FASTLOAD_EXIT     0x002A8C5Cu  /* ori r27, r3, 0: its answer */
#define RAC4_FASTLOAD_FRAME    0x002A8CC8u  /* li r3, 1: BeginFrame's argument */
#define RAC4_TRANSITION_MOVIE  0x010B7FE0u  /* the movie the flight plays, 0 for none */

/*
 * The autosplitter's quit hook: three words replacing some logging inside the
 * game's quit-requested callback, so the game itself stores 0xFF at
 * RAC4_QUIT_FLAG when the player asks to quit. That is the only game-side quit
 * signal any of the four titles has.
 */
#define RAC4_QUIT_HOOK_ADDR    0x00013780u
#define RAC4_QUIT_FLAG         0x01700000u

/*
 * The autosplitter's other hook, ADDR_LOADING_HOOK_1 / _2 and ADDR_LOADING_VAL:
 * ten words of trampoline at 0x11904 and a branch to them at 0x11884, so the
 * game stores 0xFF at RAC4_LOADING_VAL the moment it has put the SCE logo up.
 * That is the SPRX's STATUS_INGAME_PENDING gate — the point at which it decided
 * the game was really back and sent CMD_UNPAUSE — and it is where qwark's RESUME
 * belongs too, because the process reappears well before the game is playable.
 */
#define RAC4_LOADING_HOOK_1    0x00011884u  /* the branch into the trampoline */
#define RAC4_LOADING_HOOK_2    0x00011904u  /* the trampoline itself, ten words */
#define RAC4_LOADING_VAL       0x01710000u  /* reads 0xFF once the logo is up */

/* ------------------------------------------- the manips' in-file test, 1.21 */

/*
 * What rac4.c's in-file test for the manips reads, from the NPEA00423
 * executable. (The file-load trigger itself is the tutorial flags' drop, which
 * the flags hot block already carries.)
 *
 * RAC4_IN_FRONTEND is the byte that says the main menu (the front end) is up:
 * startlevel (0x11BC0) sets it to 1 at 0x11C3C, the multiplayer front end at
 * 0x132F8, and the only thing that clears it is the code that runs as the
 * front end exits into a level (0x140B8, beside the load request's own
 * clear). RAC4_LEVEL is the level the game is in, the word the autosplitter
 * calls its current planet: startlevel sets it to -1 (0x11C70) and main()
 * runs the front end while it is negative (0x1541C); single-player levels are
 * 1..20, co-op 21..40, multiplayer 0 and 41..79.
 */
#define RAC4_IN_FRONTEND       0x00B3C7D4u  /* u8, 1 while the main menu is up */
#define RAC4_LEVEL             0x009C3240u  /* s32, -1 on the way to the menu */
#define RAC4_LEVEL_SP_CO_OP_MAX 40          /* past it is multiplayer */

/* ------------------------------------------------- autosplit watcher, rev 1.5 */

/*
 * Deadlocked is the one game whose detection already ran on the console: the old
 * SPRX (rac4-autosplitter-src/ratchetron.c) polled these words itself and sent
 * commands to the PC, and rac4-LC-autosplitter.asl consumed them. The addresses
 * are that SPRX's ADDR_* constants, and where the two disagree the LC script
 * wins, so the watcher below reproduces its `command` stream.
 *
 * The current planet is the SPRX's ADDR_CURRENT_PLANET, which is not the same
 * word as RAC4_CURRENT_PLANET: it reads 0 when the box is beaten, and both it
 * and the destination are taken modulo 0x14 for co-op. It is saved with the
 * file, so it is not the origin of a planet change: rac4.c keeps its own origin
 * planet for that, and this word is only read for the Vox split and the readout.
 */
#define RAC4_AS_PLANET       RAC4_LOAD_PLANET_OLD  /* 0x009C3240 */
#define RAC4_AS_PLANET_MOD   0x14u
#define RAC4_VOX_HP          0x449BEAD0u  /* f32, negative once Vox is beaten */

#define RAC4_PLANET_MAINMENU 0
#define RAC4_PLANET_DREADZONE 1
#define RAC4_PLANET_INTERIOR 15

/* Reason codes. Code 1 is "planet entered" in every game. */
#define R4_AS_PLANET 1
#define R4_AS_VOX    2
#define R4_AS_QUIT   3   /* PAUSE / RESUME around a quit to the XMB, not a split */

/*
 * What the LC script added back on the way in: it stopped game time for the
 * whole quit and reload and then added 14.8 s, so a quit always costs exactly
 * that. NORMALISE with this parameter is the same arithmetic.
 */
#define RAC4_QUIT_PAUSE_US   14800000u

/*
 * The savefile helper's byte. qwark embeds the helper and installs it on the
 * first request of a session (src/core/savefile.c), and it writes 1 here on
 * every call, so this reads 1 as soon as the game has reached the hook once.
 * The load and set-aside requests at 0x15CD71E and 0x15CD71F are driven through
 * the shared table in src/games/sfhelper_bins.c.
 */
#define RAC4_SF_HELPER         0x015CD71Du

/* MF_LevelSave / MF_MissionSave, from rac4.cs. */
#define RAC4_LEVEL_SIZE        0x304u
#define RAC4_MISSION_SIZE      0x0Cu
#define RAC4_MISSION_STATUS    8u
#define RAC4_PLANET_SAVES      15
#define RAC4_MISSIONS_DREAD    64      /* planet 0 has the whole array */
#define RAC4_MISSIONS_OTHER    16

/*
 * The game's two mode words, which are what says whether a save may be taken or
 * put back. `gameMode` is RAC4_GAME_STATE above, the word the fast-load watcher
 * already reads; `pendingGameMode` is the mode the game has been asked to change
 * to and has not changed to yet.
 *
 * Both come out of the Ghidra project's own symbols, and the addresses are the
 * ones the shipped EBOOT builds: mode_requestChange at 0x0022BE00 reads
 * pendingGameMode (`lis r3,0x9c; lwz r3,0x37f4(r3)`) and refuses with -1 unless
 * it holds -2, writes the caller's mode there when it accepts, and reads
 * gameMode at 0x0022BF08 (`lis r3,0xb4; lwz r3,-0x3a60(r3)`); mode_doChange
 * finishes a change with `gameMode = pendingGameMode; pendingGameMode = -2` at
 * 0x0022CBA0.
 *
 * So -2 in pendingGameMode means nothing is queued, and the modes themselves are
 * the game's own gameMode_t: -2 none, -1 debug, 0 normal, 1 movie, 2 scene,
 * 3 pause, 4 freeze, 5 vendor, 6 the space transition, 7 puzzle, 8 weapon
 * upgrade, 9 credits, 10 lobby, 11 flyby, 12 thermal, 13 and 14 the pre-lobby
 * pair, 15 waiting for a multiplayer start, 16 a memory-card command, 17 IOP
 * debug.
 *
 * Only 0 and 3 are safe: ordinary play, and the pause the savefile helper parks
 * the game in itself to run Deadlocked's load menu. Everything else is a
 * transition, a cutscene, a menu or a lobby that is already moving the save
 * about underneath whatever qwark would do.
 */
#define RAC4_GAME_MODE         RAC4_GAME_STATE
#define RAC4_PENDING_GAME_MODE 0x009C37F4u

#define RAC4_GAME_MODE_NORMAL  0u
#define RAC4_GAME_MODE_PAUSE   3u
/* gameMode while the ship is in the space transition; the load ends when it leaves. */
#define RAC4_GAME_MODE_SPACE   6
/* pendingGameMode when no mode change is queued: GAME_MODE_NONE, -2. */
#define RAC4_GAME_MODE_NONE    0xFFFFFFFEu

#define RAC4_BOTS_COUNT        16
#define RAC4_SKIN_COUNT        22

/*
 * The gadget table's shape, and the level a weapon can reach: V10 in a first
 * playthrough, V99 in challenge mode, which is the number the field descriptor
 * advertises.
 *
 * The game's own accessors index the table as i * 68 (i * 64 plus i * 4), so
 * the stride is 0x44 and only the first four bytes of an entry are the level
 * and the ammo. The other sixty-four are fields qwark neither reads nor writes.
 *
 * RAC4_MAX_LEVEL is the number the player sees, the one the field descriptor
 * advertises and the one UNLOCK_SET takes. RAC4_LEVEL_MEM_MAX is what V99 is in
 * memory, and nothing may write above it: the game breaks past V99.
 * RAC4_LEVEL_LOCKED is the halfword a weapon the player has not got reads.
 */
#define RAC4_GADGET_COUNT      32
#define RAC4_GADGET_STRIDE     68      /* 0x44 */
#define RAC4_MAX_LEVEL         99
#define RAC4_LEVEL_MEM_MAX     98      /* V99, the highest the halfword may hold */
#define RAC4_LEVEL_LOCKED      ((s16)-1)

/*
 * What the game's own maximum-ammunition arithmetic reads, decoded from the
 * function around RAC4_AMMO_INSTR: a base out of the gadget's stats record,
 * one figure for single player and another for multiplayer, plus the amount an
 * ammo mod adds for every ammo mod fitted in the entry's ten mod slots. The
 * numbers are all read live, so nothing here is a copy of a table that a patch
 * or a different build could move.
 */
#define RAC4_GAME_TYPE         0x00B36DF4u  /* u32 g_gameType, 1 = multiplayer */
#define RAC4_GAME_TYPE_MP      1u
#define RAC4_GADGET_STATS      0x009DE970u  /* one record per gadget */
#define RAC4_STATS_STRIDE      176u         /* 0xB0 */
#define RAC4_STATS_AMMO_SP     0x42u        /* u16 base ammo, single player */
#define RAC4_STATS_AMMO_MP     0x44u        /* u16 base ammo, multiplayer */
#define RAC4_AMMO_PER_MOD      0x009DD434u  /* u32 per gadget, added per ammo mod */

/* The mod slots inside a GadgetEntry: ten of them, one word each, type 2 is ammo. */
#define RAC4_MOD_SLOT0         0x14u
#define RAC4_MOD_SLOT_SIZE     4u
#define RAC4_MOD_SLOTS         10u
#define RAC4_MOD_AMMO          2u

/*
 * Position slots. The blob is unchanged from build 41 and from rac4.cs before
 * it, so every slot a user has ever saved stays readable: 0x20 bytes of Vec4
 * position then Vec4 rotation out of RAC4_COORDS, then the RAC4_CAMERA_LR float
 * and the RAC4_CAMERA_UD float.
 *
 * Loading used to write only the position and rotation, and the facing never
 * survived it. The reason is in the EBOOT. The hero's rot.z is not the authority
 * on where he is pointing: HeroTurn.ideal (MyHero+0x17A0) is loaded straight out
 * of the camera yaw at 0x0032B25C (`lfs f1,6576(r31)` -> 0x0032B264 `stfs
 * f1,6048(r31)`), and rot.z is then dragged towards that ideal every frame at
 * 0x002F3410..0x002F341C (`lfs f1,184(r31)`, one call, `stfs f1,184(r31)`). Write
 * rot.z on its own and the camera pulls it back inside a frame or two, which is
 * exactly what a user sees.
 *
 * The game's own teleport, the function at 0x0030C5D8, shows the full set. It
 * writes the position at +0x00A0, the rotation at +0x00B0..+0x00BC (0x0030C828
 * through 0x0030C840), rebuilds the hero matrix and the hero moby's rMtx from
 * it, then loads rot.z at 0x0030CAF4 (`lfs f6,184(r31)`) and stores that same
 * word into the camera yaw at 0x0030CC58 (`stfs f6,6576(r31)`, MyHero+0x19B0 =
 * RAC4_CAM_YAW) while zeroing the camera pitch at 0x0030CC54 (`stw
 * r24,6608(r31)` with r24 = 0 from 0x0030C7F8, MyHero+0x19D0 = RAC4_CAMERA_UD).
 * It ends by copying pos.x/y/z into the three cheat floats at +0x2F04, which is
 * RAC4_COORDS2. Nothing needs the matrices written by hand: they are rebuilt
 * every frame, which is why the position alone always did come back.
 *
 * So a load writes, in this order:
 *
 *   RAC4_COORDS      0x20 bytes, blob[0x00..0x1F]: the Vec4 position and the
 *                    Vec4 rotation at their real home, MyHero+0x00A0/+0x00B0.
 *   RAC4_COORDS2     0x10 bytes, blob[0x00..0x0F]: the position copy the teleport
 *                    also keeps, MyHero+0x2F04. Note the fourth word lands on
 *                    MyHero+0x2F10; that is what rac4.cs did and what build 41
 *                    shipped, and it is left alone here.
 *   RAC4_CAM_YAW     4 bytes, blob[0x18], which is rot.z: the camera yaw the
 *                    teleport sets from exactly that word. Without this the
 *                    facing is pulled back to wherever the camera was.
 *   RAC4_CAMERA_UD   4 bytes, blob[0x24]: the camera pitch. The teleport zeroes
 *                    it, but the blob has carried the real one all along, so the
 *                    saved pitch goes back instead. Skipped for a short blob.
 *
 * RAC4_CAMERA_LR, blob[0x20], is *not* written back. It is MyHero+0x19C0, one
 * field past the camera yaw, and it is the camera's quick-turn acceleration
 * constant, not a rotation: rac4.cs named it cameraRotationLeftRight and was off
 * by 0x10. It is still saved so the blob keeps its shape.
 */
#define RAC4_POS_MAIN_LEN  0x20
#define RAC4_POS_ALT_LEN   0x10
#define RAC4_POS_BLOB_LEN  (RAC4_POS_MAIN_LEN + 8)
#define RAC4_POS_ROT_Z     0x18    /* rot.z inside the blob: the facing */
#define RAC4_POS_CAM_UD    0x24    /* the saved camera pitch inside the blob */

/*
 * Fingerprint: the original instruction at the refill-ammo patch site,
 * 0x4182000C ("beq +12"), which RAC4Form restores when the checkbox is cleared.
 * qwark's own patched form, the nop, is accepted too.
 *
 * Build 50: and the branch into the refill-ammo trampoline, which is what the
 * site holds on every boot under RPCS3 once the code switches' patch is in
 * (src/games/switches/sw_rac4.s).
 */
#define RAC4_FP_ADDR RAC4_AMMO_INSTR
extern const u8 rac4_fp[4];
extern const u8 rac4_fp_patched[4];
extern const u8 rac4_fp_switch[4];

/* ---------------------------------------------------------------- hot block */

#define RAC4_HOT_INPUTS_ADDR RAC4_INPUTS
#define RAC4_HOT_INPUTS_LEN  0x1EC
#define RAC4_HOT_STATS_LEN   0x40      /* bolts through the saved bot unlocks */
#define RAC4_HOT_FLAGS_LEN   0x10      /* in-game byte through the tutorial flags */
/* autosplit: load request +0x00, target planet +0x04, cutscene pointer +0x1C */
#define RAC4_HOT_LOAD_ADDR   RAC4_LOAD_PLANET2
#define RAC4_HOT_LOAD_LEN    0x20

/* ----------------------------------------------------------- readout slots */

#define RAC4_RO_BOLTS      0
#define RAC4_RO_SAVEFILE   1
#define RAC4_RO_DREAD      2
#define RAC4_RO_CHALLENGE  3
#define RAC4_RO_FRAMES     4
#define RAC4_RO_CUR_CHAL   5
#define RAC4_RO_SKIN       6
#define RAC4_RO_IN_GAME    7

/* ------------------------------------------------------------- feature ids */

/*
 * Stable: new features go on the end, a retired one leaves its number behind.
 * Retired, never to be reused: 16 (reset all weapon levels), and since build 53
 * 12 (act tune bosses), which is the NG+ manip group's setup now (protocol
 * 1.21).
 */
#define R4_CRASH_PATCHES   0
#define R4_SOFTLOCK_FIX    1
#define R4_FAST_LOADS      2
#define R4_REFILL_AMMO     3
#define R4_FREEZE_HEALTH   4
#define R4_GHOST           5

#define R4_DIE             6
#define R4_BOLTS           7
#define R4_DREAD_POINTS    8
#define R4_CHALLENGE       9
#define R4_SKIN            10

#define R4_UNLOCK_PLANETS  11
/* 12 retired: act tune bosses */

#define R4_SET_ASIDE       13
#define R4_LOAD_ASIDE      14

#define R4_MAX_LEVELS      15
/*
 * 16 retired, never to be reused: "Reset all weapon levels" put every owned
 * weapon back to V1, and nobody wanted the button. The number stays spoken for
 * so a client that still remembers it can never be handed something else.
 */
#define R4_RESET_LEVELS    16
#define R4_MAX_AMMO        17

/* ------------------------------------------------------ rac4_panel.c exports */

int rac4_trigger(u8 id);
int rac4_set_value(u8 id, u32 value);
int rac4_get_options(u8 id, const char * const **options, u8 *count);
int rac4_load_setaside(void);
u8  rac4_savefile_ready(u8 what);
int rac4_die(void);

int rac4_planet_load(u8 planet, u8 flags);

int rac4_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac4_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac4_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac4.c exports */

u8  rac4_planet_count(void);

/* The fast-load patch, forced on for a planet load and restored when it ends. */
int rac4_fastload_force(void);

#endif /* QWARK_RAC4_H */
