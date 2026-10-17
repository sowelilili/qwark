/*
 * Ratchet & Clank 3: Up Your Arsenal (NPEA00387, and BCES01503 when the disc
 * trilogy boots it): the address table and the seam between rac3.c (numbers, hot
 * block, descriptors, vtable) and rac3_panel.c (the handlers). Nothing outside
 * those two files includes this.
 *
 * Every constant is from racman's RaCTrainer/Games/RAC3: rac3.cs for the address
 * table and the setups, RAC3Form.cs for the button handlers, UYAUnlocks.cs for
 * the item table and RAC3Form.Designer.cs for the armour and ship-colour names.
 * Where the old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC3_H
#define QWARK_RAC3_H

#include "game.h"

/* -------------------------------------------------------- rac3.cs addresses */

#define RAC3_INPUTS            0x00D99370u  /* u32 pad mask, OG layout */
#define RAC3_ANALOGS           0x00D9954Cu  /* f32 rx, ry, lx, ly */
#define RAC3_LOADING_SCREEN_ID 0x00D99114u

#define RAC3_QE_OFFSET         0x00C1E2C0u  /* s16 */
#define RAC3_CURRENT_PLANET    0x00C1E438u
#define RAC3_ITEM_ARRAY        0x00C1E43Cu  /* one byte per item id: its version */
#define RAC3_BOLTS             0x00C1E4DCu
/*
 * Build 53, Freeze health (classic.h): the game's maximum, a word, from the
 * NPEA00387 executable. The game works it out from the health XP and the ten
 * bytes at +0x50 beside it, writes it at 0x14EF68 when it changes, with a
 * message of its own at 200, the most there is, and sets the hit points to it
 * when it fills them (0x64E58, 0x6AD808).
 */
#define RAC3_MAX_HEALTH        0x00C1E4E4u  /* s32, up to 200 */
#define RAC3_CHALLENGE_MODE    0x00C1E50Eu  /* one byte */
#define RAC3_HEALTH_XP         0x00C1E510u
#define RAC3_CURRENT_ARMOR     0x00C1E51Cu  /* u16 */
#define RAC3_QUICK_SELECT      0x00C1E652u  /* one byte, quick-select pause */
#define RAC3_CC_FAKE_ITEM_A    0x00C1E660u  /* CC early: 0x41 */
#define RAC3_CC_FAKE_ITEM_B    0x00C1E664u  /* CC early: 0x41414141 */

#define RAC3_CB_PRIMARY_FRONT  0x00C33AB0u
#define RAC3_CB_PRIMARY_BACK   0x00C33AB4u
#define RAC3_CB_TINT_FRONT     0x00C33AC0u
#define RAC3_CB_TINT_BACK      0x00C33AC4u

#define RAC3_KLUNK_TUNING_2    0x00C36BCCu
#define RAC3_VID_COMIC_MENU    0x00C4F918u
#define RAC3_KLUNK_TUNING_1    0x00C9165Cu
#define RAC3_DROPSHIP_HEALTH   0x00C956A0u

/*
 * Build 53: the trophy reset (rac3_panel.c, and sf_trophy_reset in the savefile
 * helper, src/games/sfhelper/sf_rac3.h, which has the object's layout).
 *
 * The old trainer wrote 1 to the word at 0xD9E020 after deleting
 * /dev_hdd0/home/<user>/trophy/NPWR02347_00 over Ratchetron; the rc3-save mod
 * read it. qwark's request is the helper's fourth byte, `api_trophy` in its
 * table (0xD9FF1C), and qwark does the delete itself, on the console.
 *
 * The game's trophy object pointer, and in the object the word that is 1 while
 * its register thread runs and 2 while its unlock thread does. The reset is
 * refused while either runs.
 */
#define RAC3_TROPHY_OBJ_PTR    0x00CB150Cu
#define RAC3_TROPHY_THREAD     0x188u

/*
 * Where the game keeps the NP communication id it registers its trophies under:
 * the word here points at a block (0xD05CFC, set at 0x15870) whose +4 is the
 * SceNpCommunicationId 0x984FA0 hands sceNpTrophyCreateContext, copied there by
 * 0x984768 out of the executable's table of the four games' ids (UYA's is
 * NPWR02347, number 0). The console keeps that trophy set's data, the user's
 * unlocks included, in /dev_hdd0/home/<user>/trophy/<id>_<number>, which is
 * how qwark finds the one folder it deletes rather than naming it.
 */
#define RAC3_TROPHY_COMMID_PTR 0x00CBAA04u
#define RAC3_TROPHY_COMMID_OFF 4u
#define RAC3_TROPHY_COMMID_LEN 12u

/*
 * rc3-save's third ASM word: the register thread's store of each trophy's
 * unlocked flag into the game's cache (0x9850E0's `stw r4,12(r7)`), made a nop,
 * so a registration leaves the cache as the reset set it. qwark writes it on a
 * console just before it asks for a reset, never under RPCS3, and accepts the
 * site holding either word and nothing else.
 */
#define RAC3_TROPHY_FILL_SITE  0x00985318u
#define RAC3_TROPHY_FILL_WORD  0x9087000Cu
#define RAC3_TROPHY_FILL_NOP   0x60000000u

/*
 * What qwark writes in `api_trophy`, and what the helper answers there;
 * sf_rac3.h has the helper's side. RUNNING lasts while the helper's own thread
 * resets the game's trophies, a few seconds the game goes on through, and the
 * thread answers 0; UNREGISTERED when every registration it made failed, STILL
 * when the last was still going when it stopped waiting.
 *
 * After the request byte, the helper's own: its busy byte, and then what the
 * thread leaves of the last reset for the log (SF_TROPHY_TRIES and on in
 * sf_rac3.h): how many registrations it made, how each ended (RAC3_TROPHY_REG_*,
 * two bits each, the first in the low bits), and how long each took in quarters
 * of a second, a byte each. Offsets from the request byte.
 */
#define RAC3_TROPHY_NOW          1u
#define RAC3_TROPHY_IN_LOAD      2u
#define RAC3_TROPHY_RUNNING      3u
#define RAC3_TROPHY_UNREGISTERED 4u
#define RAC3_TROPHY_STILL        5u

#define RAC3_TROPHY_BUSY_OFF     1u
#define RAC3_TROPHY_TRIES_OFF    2u
#define RAC3_TROPHY_RESULTS_OFF  3u
#define RAC3_TROPHY_TIMES_OFF    4u
#define RAC3_TROPHY_TRIES_MAX    3u

#define RAC3_TROPHY_REG_OK       0u
#define RAC3_TROPHY_REG_FAILED   1u
#define RAC3_TROPHY_REG_LOST     2u
#define RAC3_TROPHY_REG_RUNNING  3u

#define RAC3_COORDS            0x00DA2870u  /* f32 x, y, z (Vec4) */
#define RAC3_GHOST_TIMER       0x00DA29DEu
#define RAC3_PLAYER_STATE      0x00DA4DB4u
#define RAC3_PLAYER_HEALTH     0x00DA5040u  /* s32, the hit points */
#define RAC3_SKILL_POINTS      0x00DA521Du  /* 30 bytes */
#define RAC3_AMMO_ARRAY        0x00DA5240u  /* + (ammo offset - 0x243) */
#define RAC3_SHIP_COLOUR       0x00DA55E8u  /* one byte */
#define RAC3_UNLOCK_ARRAY      0x00DA56ECu  /* + (unlock offset - 0x4A8) */
#define RAC3_EXP_ARRAY         0x00DA5824u  /* + (exp offset - 0x5F0) */
#define RAC3_VID_COMICS        0x00DA650Bu  /* five bytes, also in the unlock array */
#define RAC3_FILE_TIME         0x00DA64E0u  /* the IGT the old form edited */

#define RAC3_TITANIUM_BOLTS    0x00ECE53Du  /* 128 bytes */
#define RAC3_LEVEL_FLAGS       0x00ECE675u  /* 0x10 per planet, planet ids are 1-based */
#define RAC3_LOAD_PLANET       0x00EE9310u  /* request word, then the planet id */
#define RAC3_DEST_PLANET       0x00EE9314u
#define RAC3_GAME_STATE        0x00EE9334u
#define RAC3_NEFFY_TUNING      0x00EF6098u

#define RAC3_MOBY_TABLE        0x00F22260u
#define RAC3_MOBY_TABLE_END    0x00F22268u
#define RAC3_MOBY_STRIDE       0x100u

#define RAC3_CC_HELP_DESK      0x0148A100u

#define RAC3_AMMO_INSTR        0x00182A88u  /* the ammo decrement; the fingerprint */

/*
 * Fast loads, a code patch since build 53, and in the same build made over on
 * RaC2's model: the screen stays black from the start of the load until the new
 * level's first frame, and the load takes as long as the disk does. From the
 * NPEA00386 and NPEA00387 executables; not yet run on hardware.
 *
 * RaC2's, the model (src/games/rac2.h, RAC2_FASTLOAD_INSTR). Every planet load
 * there runs 0xBEA188. Its setup, 0xBE9588, fades the screen to black with the
 * game's own fade, 0xB111C0, which shows the last frame six more times, darker
 * each time, and leaves black behind; then a frame loop (0xBEA868) runs the
 * transition's update (0xBE7400, for a load 0xBE5470), whose answer ends the
 * loop, its draw (0xBE9308: begin the frame, draw the space scene, end the frame
 * and flip it, which is where a pass waits for the vblank), and a step of the
 * level loader (0xB4A600) unless a story scene is owed (0x15D4670). The
 * transition plays frame-counted segments, a random one at each segment end
 * while the loader is below stage 2 (0x147A1D4), a closing one once the loader
 * has read the level - releasing the loader's hold at stage 2 (0x147A1D8) as
 * that one starts - and is over when the closing one is. Fast loads nop the
 * draw's call at 0xBEA8A0. Nothing is drawn and nothing flips, so the screen
 * keeps the setup's black, and a pass no longer waits for a vblank: every
 * segment's frames run out in no time and the transition lasts as long as the
 * loader takes to read the level. An owed scene still holds the loader until
 * the first segment's end plays it (0xBE402C) in frame loops of its own, which
 * draw and flip; that first segment is over at once, so the scene is up almost
 * at once. The rest of the load runs behind the same black after the loop
 * (0xBEA900), and the new level's init draws its first frame as it does after
 * any load.
 *
 * UYA's transition is the same machine. Every planet load runs 0x27B360 (from
 * 0x275B18). Its setup, 0x27ACCC, picks a transition from the from/to table at
 * 0xC488BC - the number at RAC3_TRANSITION_ID, rc3/ps3data/global/transitionN -
 * puts the planet word to the destination, resets the level loader
 * (RAC3_LOADER_STAGE 0), sets RAC3_TRANSITION_HOLD, notes the story scene the
 * trip owes (RAC3_SCENE_OWED, 0x27AB84: 17 on the first trip into Aquatos, 18
 * on the first trip out of it once its sewers have been visited) and runs the
 * transition's init, which loads its files and sound banks and starts its first
 * clip. Then a frame loop, a pass a frame: the pads (0x235950, which is where
 * the savefile helper runs), the pass count (RAC3_TRANSITION_FRAME), mode 6,
 * the transition's update (*0xC48D00), whose answer ends the loop, and its
 * second routine (*0xC48D04), where the space flight and the sub keep their
 * clip logic; then the frame itself, 0x27B550..0x27B56C (begin it, 0x11AD44 and
 * 0x11B434; draw the transition, 0x27A014; end it and flip, 0x11B4EC); then a
 * loader step (0x1D27B8) unless a scene is owed. The clip transitions, 0 to 5 -
 * Aquatos's sub and pipe (0, 1), the three ranger dropships (2 to 4) and the
 * ordinary space flight (5) - count clip ends at 0x134EBD4, play the owed scene
 * at the first (0x27C890), pick clips at random until that count is 3 and the
 * loader has read the level (stage 2, where it waits for the hold), then play a
 * closing clip whose start unloads the transition's sound banks
 * (Transition_UnloadSounds, RAC3_TRANSITION_UNLOAD, which also clears the hold)
 * and are over when it ends. The rest are timed cards: the vid comics (6, a
 * title card going in and a results card coming out), Holostar Clank's way back
 * (8, a line of text), and any pair the table does not list - the planet
 * selector makes those - which is a black screen of 300 frames (8 as well, a
 * table row of its own). Multiplayer is 7.
 *
 * What RaC2 has and UYA does not: nothing fades the screen at the start of a
 * load (the galactic map does, at 0x27388C, before it raises the request; a
 * request qwark writes comes straight out of play), and a clip's end does not
 * stop the sound the clip started, where each of RaC2's segment ends stops every
 * sound (0x11E0B88). Undrawn and spun at the CPU's pace, UYA's clips would start
 * a whoosh apiece, dozens in one load. So the patch is two sites and a cave:
 *
 *   RAC3_FASTLOAD_DRAW_SITE, the head of the frame: every pass goes past the
 *   frame to RAC3_FASTLOAD_SKIP. That is RaC2's nop.
 *
 *   RAC3_FASTLOAD_SITE, just before the update's call, with the pads read and
 *   the mode set:
 *   - the first pass fades the screen to black, RAC3_SCREEN_FADE for six
 *     frames as RaC2's setup does (the game's own calls ask for 6.5 times the
 *     word at 0xC1D598, 1.0 in the executable), and runs no update. It puts the
 *     renderer's count of flips not to show (RAC3_RENDERER_HELD, which the
 *     flip counts down at 0x96E178) to 0 first: the black card's init leaves
 *     it at 2, and the fade does nothing while it is not 0;
 *   - while a scene is owed the transition runs as the game runs it, undrawn:
 *     the first clip's frames run out at once, and its end plays the scene,
 *     which draws itself, fades to black when it is over and clears the word;
 *   - otherwise, while the loader is below stage 2, a pass skips the
 *     transition's two routines and goes on at RAC3_FASTLOAD_RESUME: no clip
 *     moves and none starts a sound, while the pads, the loader and the rest
 *     of the pass run as they do;
 *   - once the loader has read the level the transition is over: the fade word
 *     at black, as a clip's end leaves it; Transition_UnloadSounds, when the
 *     transition is one that loads banks of its own (0 to 6) and still holds
 *     them; and the game's own finish (0x27B430) runs the rest of the load
 *     behind the black, as it does after any transition.
 *
 * So every planet and every trip is fast but multiplayer's, which the patch
 * leaves to the game (7): qwark has no business in a networked load, and nothing
 * here was looked at with one under way. The vid comics' cards and Holostar's
 * line are not shown, as the clips are not. Story scenes still play, and a
 * load's other cutscenes are not the transition's to skip: a new game's opening
 * films play in the setup (0x275678), before the loop, and a planet's arrival
 * scene is the new level's. The one sound a fast load makes is the first clip's
 * on a trip that owes a scene, which goes on under the scene's start.
 *
 * Why the cave does not play an owed scene itself and skip the clip: the clip's
 * end deletes the clip's mobys (0x279BAC) before the scene clears the cinematic
 * state they are listed in (0xD79310, at 0x274BB0), so the game's own path is
 * the one that leaves nothing behind.
 *
 * Before build 53 fast loads were the two data words beside the count:
 * 0x134EBD4 = 3 and, a fifth of a second into the trip, 0x134EE70 = 01 01 (the
 * space flight's "clip ended" and "closing clip started" bytes), so it ended at
 * the first clip's end. Writing 3 took the count past 1, where an owed scene
 * plays and lets the loader go, so a trip owing one waited for the loader
 * forever, behind the black of that clip's end: the Aquatos hang rac3.cs
 * LoadPlanetSafe's exception kept away from trips into Aquatos. Build 53's first
 * patch ended a clip transition the frame the loader reached stage 2, from the
 * loop's "is it over" test at 0x27B428: fast, and the scenes played, but the
 * first clip showed until then, and the cards were not fast at all.
 */
#define RAC3_FASTLOAD_SITE       0x0027B49Cu  /* ori r3, r26, 0: the update's argument */
#define RAC3_FASTLOAD_SITE_WORD  0x63430000u
#define RAC3_FASTLOAD_UPDATE     0x0027B4A0u  /* the update's call, after the site */
#define RAC3_FASTLOAD_RESUME     0x0027B538u  /* the pass after the transition's routines */
#define RAC3_FASTLOAD_DRAW_SITE  0x0027B550u  /* bl 0x11AD44: the head of the frame */
#define RAC3_FASTLOAD_DRAW_WORD  0x4BE9F7F5u
#define RAC3_FASTLOAD_DRAW_RET   0x0027B554u
#define RAC3_FASTLOAD_SKIP       0x0027B570u  /* the pass after the frame */
#define RAC3_FRAME_BEGIN         0x0011AD44u  /* the draw site's own call */
#define RAC3_SCREEN_FADE         0x00123988u  /* RaC2's 0xB111C0; frames in r3 */
#define RAC3_SCREEN_FADE_FRAMES  6u
#define RAC3_TRANSITION_UNLOAD   0x00278A00u  /* Transition_UnloadSounds */
#define RAC3_TRANSITION_ID       0x00D990DEu  /* u16, the transitionN folder */
#define RAC3_TRANSITION_FRAME    0x00D990D2u  /* u16, 0xFFFF at setup, 0 on the first pass */
#define RAC3_TRANSITION_LAST_BANKS  6u        /* 0..6 load sound banks of their own */
#define RAC3_TRANSITION_MULTIPLAYER 7u
#define RAC3_LOADER_STAGE        0x00D99094u  /* u32, 0x1D27B8's stage */
#define RAC3_TRANSITION_HOLD     0x00D99098u  /* u32, the transition's banks are in */
#define RAC3_SCENE_OWED          0x00F44E90u  /* u32, 17, 18 or 0 */
#define RAC3_TRANSITION_FADE     0x0134EBE0u  /* f32, 1.0 is black */
#define RAC3_RENDERER            0x00CB09BCu  /* the renderer object's pointer */
#define RAC3_RENDERER_HELD       0x1720u      /* in it: flips that will not show */

/*
 * Where the console writes the cave: 0x9807F0..0x9808B0, forty-eight words at
 * the top of what the trophy reset left of the strictly dead 0x980304..0x980AC0
 * (src/games/sfhelper/sf_rac3.h), under its cave at SF_CAVE_TROPHY (0x9808C0)
 * and above its thread's at 0x980380..0x980700; 0x980304..0x980380 and
 * 0x980700..0x9807F0 stay free. Not the savefile helper's dead function at
 * 0x980AC0, where the Infinite ammo and Fast loads code switches are: the rack
 * mods racman shipped for this game (COOLCAM, SUCKMAN, rc3-save) load their
 * code there, and a console runs Fast loads beside them, which it never had to
 * before.
 */
#define RAC3_FASTLOAD_CAVE       0x009807F0u
#define RAC3_FASTLOAD_CAVE_WORDS 48

/*
 * The savefile helper's byte. qwark embeds the helper and installs it on the
 * first request of a session (src/core/savefile.c), and it writes 1 here on
 * every call, so this reads 1 as soon as the game has reached the hook once.
 *
 * 0xD9FF01 and 0xD9FF02 beside it are the load and set-aside requests, driven
 * through the shared table in src/games/sfhelper_bins.c. 0xD9FF03 and 0xD9FF04
 * were Gigahelper's tempsave writer and reader; nothing writes a file any more,
 * so the "Save manager: load file" row that drove one of them is retired.
 */
#define RAC3_SF_HELPER         0x00D9FF00u

/*
 * Build 53: what the savefile's readiness test reads (rac3_savefile_ready).
 *
 * The save manager is the block RAC3_SAVEDATA_INFO points at, and its save
 * buffer is the word at +4 - as in RaC2, the block is not there until the
 * getter 0x976FE8 first runs, and +4 is 0 until the game's first save or load
 * through the save-data utility in this process. A save starts with the sizes
 * of its global block and of one planet's block; the restore (0x1DF5CC) takes
 * those or the older pair beside them, RaC2's, and nothing else.
 *
 * A load goes from the pause, which the helper asks the game for itself from
 * any single-player mode but space (RAC3_GAME_STATE 6, the transition a planet
 * load runs) and debug (-1), which the mode change will not leave; with no
 * planet load already on its way (RAC3_LOAD_PLANET). The modes past 9 are the
 * lobby and multiplayer. src/games/sfhelper/sf_rac3.h has the modes.
 */
#define RAC3_SAVEDATA_INFO          0x00CB0A98u
#define RAC3_SAVEDATA_FIELD         4u
#define RAC3_SAVE_HEAD_GLOBAL       0x37B0u
#define RAC3_SAVE_HEAD_GLOBAL_OLD   0x1CF8u
#define RAC3_SAVE_HEAD_PLANET       0x84C0u
#define RAC3_SAVE_HEAD_PLANET_OLD   0x0CD0u
#define RAC3_GAME_MODE_SPACE        6u
#define RAC3_GAME_MODE_LAST         9u

#define RAC3_POS_BLOB_LEN  30          /* what racman stores for a position slot */
#define RAC3_LF_LEN        0x10        /* level flag bytes per planet */

/* ------------------------------------------------- autosplit watcher, rev 1.5 */

/*
 * rac3.cs AutosplitterAddresses, in the order rac3-autosplitter.asl reads them.
 * The Marcadia mission byte is in that list only "for backwards compatibility"
 * and nothing reads it; the planet frame count is read by the old client but
 * used by none of the script's conditions, so it is not read here. The
 * loading-screen id is: the script's `update` block subtracted a second of game
 * time whenever it became 1 on a load that was not to or from one of the five
 * planets whose loading screen is a different one that is not actually longer.
 */
#define RAC3_LOADING_SCREEN (RAC3_LOADING_SCREEN_ID + 3)  /* the low byte */
#define RAC3_LOADING_LONG   1        /* the id the script watched for */
#define RAC3_LONG_LOAD_US   1000000u /* the second it took off */
#define RAC3_NEFFY_HEALTH  0x00C4DF80u  /* f32, 1.0 full and 0.0 dead */
#define RAC3_NEFFY_PHASE   0x00DA50FCu  /* u32, odd during a Biobliterator phase */
#define RAC3_LOADED_CHUNK  0x00F08100u  /* rac3.cs loadedChunk, s32 */
/* Tyhrraguise: RAC3_UNLOCK_ARRAY + (0x4C6 - 0x4A8), the way the old list wrote it. */
#define RAC3_TYHRRAGUISE   0x00DA570Au

/* Planet ids the split conditions name. PLANET_LIST index == game planet id. */
#define RAC3_PLANET_VELDIN     1
#define RAC3_PLANET_MARCADIA   4
#define RAC3_PLANET_KOROS      14
#define RAC3_PLANET_LAUNCHSITE 20

#define RAC3_KOROS_BOLT_STATE  0x74   /* playerState while a titanium bolt is taken */
#define RAC3_GAME_STATE_LOAD   6      /* the frame before gameplay resumes */

/*
 * Revision 1.21, the manips' file-load trigger and the trophy auto-reset
 * (rac3.c). RAC3_MODE_TIMER counts the frames spent in the current game mode:
 * the level loop puts it to 0 as it starts (0x1D7078), adds one a frame
 * (0x1D82FC) and puts it back to 0 on a mode change (0x1D8604).
 */
#define RAC3_MODE_TIMER        0x00EE9330u

/* Reason codes. Code 1 is "planet entered" in every game. */
#define R3_AS_PLANET        1
#define R3_AS_LDF           2
#define R3_AS_TYHRRAGUISE   3
#define R3_AS_KOROS_BOLT    4
#define R3_AS_BIOBLITERATOR 5
#define R3_AS_LONG_LOAD     6   /* LOAD_START / LOAD_END, not a split */

/*
 * vars.llIgnorePlanets: the loading screen on these five is a different one, and
 * its "long load" is not actually longer, so the script left those alone. The
 * check is on both ends of the trip, the destination and the origin.
 */
#define RAC3_LL_IGNORE_LAUNCHSITE 20  /* Launch site (ranger ship) */
#define RAC3_LL_IGNORE_METRO      26  /* Metro rangers (ranger ship) */
#define RAC3_LL_IGNORE_AQ_CLANK   27  /* Aquatos Clank (submarine / down the pipe) */
#define RAC3_LL_IGNORE_AQ_SEWERS  28  /* Aquatos sewers (down the pipe / submarine) */
#define RAC3_LL_IGNORE_TYHRRA     29  /* Tyhrranosis rangers (ranger ship) */

/*
 * Fingerprint: the original instruction at the infinite-ammo patch site,
 * 0x7C85312E, which racman restores when the toggle is turned off. qwark's own
 * patched form, the nop, is accepted too.
 *
 * Build 50: and the branch into the freeze-ammo trampoline, which is what the
 * site holds on every boot under RPCS3 once the code switch's patch is in
 * (src/games/switches/sw_rac3.s).
 */
#define RAC3_FP_ADDR RAC3_AMMO_INSTR
extern const u8 rac3_fp[4];
extern const u8 rac3_fp_patched[4];
extern const u8 rac3_fp_switch[4];

/* ---------------------------------------------------------------- hot block */

/* inputs: pad at +0, analogs at +0x1DC */
#define RAC3_HOT_INPUTS_ADDR  RAC3_INPUTS
#define RAC3_HOT_INPUTS_LEN   0x1EC
/* state: QE offset at +0 through the quick-select pause byte at +0x392 */
#define RAC3_HOT_STATE_ADDR   RAC3_QE_OFFSET
#define RAC3_HOT_STATE_LEN    0x394
/* player: the coordinate Vec4 plus the rotation behind it */
#define RAC3_HOT_PLAYER_ADDR  RAC3_COORDS
#define RAC3_HOT_PLAYER_LEN   0x20
/*
 * pstate: player state at +0x02, the health word at +0x28C and the Neffyrious
 * phase at +0x348. Health used to have a slow block of its own; the autosplit
 * watcher needs the other two every tick and they all sit inside one read.
 */
#define RAC3_HOT_PSTATE_ADDR  RAC3_PLAYER_STATE
#define RAC3_HOT_PSTATE_LEN   0x34C
/*
 * load: the load request word at +0x00, the destination planet +0x04 (+0x07
 * its low byte), mode timer +0x20, game state +0x24. Build 53: the request word
 * joined the block, which used to start at the destination, so the fast-load
 * watcher reads it and the mode out of the same read (rac3_fastload_watch).
 */
#define RAC3_HOT_LOAD_ADDR    RAC3_LOAD_PLANET
#define RAC3_HOT_LOAD_LEN     0x28

/* ----------------------------------------------------------- readout slots */

#define RAC3_RO_BOLTS      0
#define RAC3_RO_SAVEFILE   1
#define RAC3_RO_CHALLENGE  2
#define RAC3_RO_HEALTH_XP  3
#define RAC3_RO_HEALTH     4
#define RAC3_RO_ARMOUR     5
#define RAC3_RO_QE_OFFSET  6
#define RAC3_RO_FILE_TIME  7
#define RAC3_RO_SHIP       8
#define RAC3_RO_CB_FRONT   9
#define RAC3_RO_CB_BACK    10
/* Tint front and back mirror each other; one readout and one control cover both. */
#define RAC3_RO_CB_TINT    11

/* ------------------------------------------------------------- feature ids */

/*
 * Stable: new features go on the end, a retired one leaves its number behind.
 * Retired, never to be reused: 4 (freeze Klunk tuning), 17 (make NG+ no-QE
 * file), 28 (give 1337 ammo), 29 (setup NG+ weapons) and 30 (equip the bomb
 * glove), and since build 53 the manip buttons the manip groups replaced
 * (protocol 1.21): 16 (setup NG+ manips), 18 (CC early setup), 19 (untune
 * bosses) and 20 (reset dropship health); and 2 (one-hit KO).
 */
#define R3_FREEZE_AMMO       0
#define R3_FREEZE_HEALTH     1
/* 2 retired in build 53: one-hit KO */
#define R3_GHOST             3
/* 4 retired */
#define R3_QS_PAUSE          5

#define R3_DIE               6
#define R3_BOLTS             7
#define R3_CHALLENGE         8
#define R3_HEALTH_XP         9
#define R3_HEALTH            10
#define R3_ARMOUR            11
#define R3_SHIP_COLOUR       12
#define R3_FILE_TIME         13
#define R3_QE_OFFSET         14
#define R3_VENDOR_QE         15

/* 16 retired: setup NG+ manips */
/* 17 retired */
/* 18, 19 and 20 retired: CC early setup, untune bosses, reset dropship health */
#define R3_RESET_TROPHIES    21
#define R3_UNLOCK_SKILL      22
#define R3_RESET_SKILL       23
#define R3_UNLOCK_TITANIUM   24
#define R3_RESET_TITANIUM    25
#define R3_UPGRADE_ALL       26
#define R3_DOWNGRADE_ALL     27
/* 28, 29 and 30 retired */

#define R3_SET_ASIDE         31
#define R3_LOAD_ASIDE        32
/*
 * 33 retired: "Save manager: load file" drove Gigahelper's tempsave reader,
 * which protocol 1.9 does away with.
 */

#define R3_CB_PRIMARY_FRONT  34
#define R3_CB_PRIMARY_BACK   35
#define R3_CB_TINT_FRONT     36
#define R3_CB_TINT_BACK      37

/*
 * Build 11. RaC1 and RaC2 have had a Fast loads checkbox all along; this game's
 * was the one thing rac3.cs only ever did as a side effect of its own Load
 * planet button. Now a code patch and WRITES_CODE, as theirs are
 * (RAC3_FASTLOAD_SITE above), where it used to be two data words.
 */
#define R3_FAST_LOADS        38

/*
 * Build 53, revision 1.21: the trophy reset's auto toggle, qwark-side state
 * like the per-death toggles, off by default and kept across boots through its
 * auto flag. MANIP_LIST's "Trophies" header names it beside R3_RESET_TROPHIES.
 */
#define R3_AUTO_TROPHIES     39

/* ENUM option counts, from RAC3Form.Designer.cs. */
#define RAC3_ARMOUR_COUNT 8
#define RAC3_SHIP_COUNT   32

/*
 * The highest weapon version any RaC3 item has, and so the maximum the level
 * field advertises in UNLOCK_LIST. The R3YNO stops at 5 and every other
 * levelled weapon goes to 8; UNLOCK_SET clamps to the entry's own `levels`.
 */
#define RAC3_MAX_LEVELS 8

/* ------------------------------------------------------ rac3_panel.c exports */

int rac3_trigger(u8 id);
int rac3_set_value(u8 id, u32 value);
int rac3_get_options(u8 id, const char * const **options, u8 *count);
int rac3_load_setaside(void);
u8  rac3_savefile_ready(u8 what);

int rac3_planet_load(u8 planet, u8 flags);

/*
 * Build 53: the trophy reset. rac3_trophy_reset is R3_RESET_TROPHIES's handler
 * (RAC3_TROPHY_NOW) and what rac3_trophy_auto asks with (RAC3_TROPHY_IN_LOAD);
 * rac3_trophy_auto answers 1 once this load's question is settled and 0 to be
 * asked again on the next tick. rac3_trophy_tick runs the delete and watches
 * the request on every INGAME tick, and rac3_trophy_forget drops whatever was
 * under way when a process ends. All tick thread.
 */
int  rac3_trophy_reset(u8 request);
int  rac3_trophy_auto(void);
void rac3_trophy_tick(void);
void rac3_trophy_forget(void);

int rac3_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac3_levelflags_reset(u8 planet);
int rac3_levelflags_set(u8 planet, u16 offset, u8 value);

int rac3_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac3_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac3_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac3.c exports */

u8  rac3_planet_count(void);

/*
 * The fast loads a planet load and a set-aside load force on behind the
 * checkbox, and their undoing, as RaC2's rac2_fastload_force and
 * rac2_fastload_restore: the force applies the patch (or, under RPCS3 with the
 * code switches in, writes their flag) and remembers that the checkbox did not;
 * rac3.c puts the checkbox's setting back when the load is over, which is the
 * load request word down again with the game out of mode 6 after the load has
 * been seen to run (rac3_fastload_watch), or at once when the helper drops a
 * set-aside load. rac3_fastload_forced says whether a force is outstanding, so
 * a request that fails after its force can take back only its own, as RaC1's
 * do (rac1_panel.c).
 */
int rac3_fastload_force(void);
int rac3_fastload_restore(void);
int rac3_fastload_forced(void);

/*
 * Vendor QE: the two bytes 00 17 at RAC3_QE_OFFSET. Shared by the "Enable
 * vendor QE" action and the NG+ manip groups' setup.
 */
extern const u8 rac3_vendor_qe[2];

#endif /* QWARK_RAC3_H */
