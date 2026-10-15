/*
 * Ratchet & Clank 2, Going Commando (NPEA00386): the savefile helper's address
 * table, and the template the other three are read against.
 *
 * From https://github.com/king-dedede1/rc2-save, src/patch1.c and
 * src/rc2-save.h. That repo builds at 0x10CD11C, and the mod racman shipped was
 * placed at 0x22A368 (mods/NPEA00386/rc2-save/patch.txt), which is where the
 * helper ran up to build 52.
 *
 * The rack.bin racman ships is a later build of the same helper that also wrote
 * /dev_hdd0/game/NPEA00386/USRDIR/tempsave through sys_fs_open and friends. That
 * file is what qwark replaces: the save is parked in the aside buffer and qwark
 * streams it, so the helper makes no syscall and touches no file.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC2_H
#define QWARK_SF_RAC2_H

/* ------------------------------------------------------------------- caves */

/*
 * Build 53. The function at 0x22A368, where the mod's cave was, is not dead: on
 * Joba (planet 11) the planet setup 0x5BB78 points the per-planet hook at
 * 0x1462DAC to 0x205018, which calls 0x22A368 every frame (at 0x205254, or
 * through 0x1F6E18). With the helper there Joba's routine never ran and the
 * helper was called with Joba's arguments.
 *
 * The helper is in a span of library code nothing reaches now,
 * 0x113EC64..0x11414FC (10,392 bytes): the functions in it are called only from
 * functions whose descriptors nothing refers to (0x10C98E0, 0x10C99AC,
 * 0x10C9C04, 0x10C9C6C, 0x10CA168, 0x11456D4), and the one routine that reads
 * their descriptor table (0x1136268) has no caller and no descriptor either. No
 * mod in the library writes there.
 *
 * The code switches take the far end of the same span, 0x1141400..0x11414FC
 * (src/games/switches/sw_rac2.ld), so the helper stops where they start:
 * build.sh fails a helper that would run past SF_CAVE_END, which leaves it
 * 10,140 bytes to grow into.
 */
#define SF_CAVE     0x0113EC64
#define SF_CAVE_END 0x01141400

/*
 * The hook site, from the repo's own patch.txt: a `bla` into the helper, which
 * returns the way any other call does. Absolute, as it is upstream: the cave is
 * below 32 MB, so the absolute form still reaches it.
 *
 * It replaces the `nop` after the pad update in the pad routine 0xB08698, which
 * the gameplay loop (0xB4C8E8) calls last in each state's frame, after that
 * frame's update and render and before the end-of-frame mode change.
 */
#define SF_HOOK_ADDR     0x00B086B0
#define SF_HOOK_ABSOLUTE 1

/* -------------------------------------------------------- the request bytes */

/*
 * Build 50. rc2-save kept these at 0x10CD71D..0x10CD720, which is inside the
 * executable's code segment - inside its .text section, even, loading as
 * 80 FE A4 and a 0x38 - and the helper stores to api_mod every frame. A console
 * does not mind. RPCS3 does: once it has applied its patches it takes write
 * permission away from every segment of the executable that has none of its
 * own, so under RPCS3 that first store would fault the game the moment the
 * patched-in hook ran.
 *
 * They live at the top of the read-write segment instead, in the gap between
 * the end of the TLS template and the next data, 0x1317714..0x131773F. It is
 * part of the segment's file data, reads zero there and nothing in the
 * executable refers to it. The code switches' two flag bytes take its first
 * bytes (src/games/switches/sw_rac2.s) and these follow, the copy counter on a
 * word boundary, and since build 53 the pause's two words after it.
 */
#define SF_API_MOD      0x01317728
#define SF_API_LOAD     0x01317729
#define SF_API_SETASIDE 0x0131772A
#define SF_API_I        0x0131772C

/*
 * Build 53, as in Deadlocked: SF_PAUSE_FRAME holds the game's mode timer as the
 * helper read it in the call whose request for the pause the game took, and
 * SF_PAUSE_WAIT counts the calls a load has waited for the pause, 0 while none
 * waits.
 */
#define SF_PAUSE_WAIT     0x01317730
#define SF_PAUSE_FRAME    0x01317734
#define SF_PAUSE_WAIT_MAX 600

/* ------------------------------------------------------- the aside buffer */

#define SF_ASIDE_ADDR 0x01C00000
#define SF_ASIDE_SIZE 0x00200000

/* --------------------------------------------------------- game addresses */

/*
 * savedata_buf is the word at +4 of the block savedata_info points at.
 *
 * Build 53: neither is there from the start. The block is the save manager the
 * getter 0x10BA488 allocates, and its +4 is 0 until the game's first save or
 * load through the save-data utility (0x10BC324, 0x10BC644) copies the buffer
 * pointer (+1120) into it - on a new game, the first autosave. Up to build 52
 * a set-aside before then, or a load with nothing set aside, copied two
 * megabytes from address 0.
 */
#define SF_SAVEDATA_INFO  0x01410E50
#define SF_SAVEDATA_FIELD 0x00000004

/*
 * Build 53. The first two words of a save are the sizes of its global block and
 * of one planet's block, computed by the restore (0x81070) from the field tables
 * at 0x131AC94 and 0x131AED4. The restore accepts those or the two older values
 * beside them and refuses anything else.
 */
#define SF_SAVE_HEAD_GLOBAL     0x00001D08
#define SF_SAVE_HEAD_GLOBAL_OLD 0x00001CF8
#define SF_SAVE_HEAD_PLANET     0x000084D0
#define SF_SAVE_HEAD_PLANET_OLD 0x00000CD0

/*
 * The game's modes, the scheme Deadlocked inherited, whose gameMode_t names
 * them. gameMode is what the gameplay loop dispatches on (0xB4D1B0), and every
 * mode's frame runs its update, its render and then the pad routine, and so
 * this helper, last:
 *
 *   0  ordinary play  update 0xBF5DB0, render 0xB101D8 (the world, 0xB0F908);
 *                     -1, debug, runs the same, and the mode change traps on
 *                     leaving it (0xB8C39C), so the helper never asks there
 *   1  movie          0xBF849C
 *   2  scene          0xBF77A8, 0xB10208 (the world)
 *   3  pause          the pause menu: 0xBB93D0, 0xB105E4 (0xBBA610)
 *   4  freeze         a message over a stopped game: 0xB23568 runs the message
 *                     and 0xB10240 draws it, over the world (0xB0F908) unless
 *                     it came up over the pause (0xB222EC); over the vendor it
 *                     runs the vendor's two instead
 *   5  vendor         0xBFEB38, 0xC04DB8
 *   6  space          the ship between planets, 0xBE7400 and 0xBE9308, which
 *                     never calls the pad routine
 *   7..9              0xBF66D0/0xB10338, 0xC1108C/0xC10F70, 0xB3D960/0xB3D984
 *
 * and anything else is a trap (0xB4D448). Freeze does not update the world but
 * draws it, so it would not stop the render that crashed RaC1; the pause alone
 * runs neither the world's update nor its render, nor anything that reaches
 * them, and the helper loads from the pause.
 *
 * It does not write gameMode itself. The pause's update and render run the
 * pause menu, which is set up by the mode change (0xB8C320, at the end of the
 * frame: the leaving mode's exit routine - a movie's 0xBF8304, a scene's
 * 0xBF6D38, the vendor's 0xBFB9D8 - then the pause's own entry 0xBB70E0 with
 * the screen asked for), and a gameMode written over another mode would run
 * that menu from whatever it was left at, and leave the movie or the scene it
 * replaced half torn down. So the helper asks mode_requestChange (0xB8BF48:
 * mode, 1 to push it, the screen, 0, 0) for the pause, as the Start button does
 * (mode 3, screen 1, 0xBF619C), from any mode the helper runs in, without the
 * Start button's own tests (0xBF6134: eight frames of ordinary play, player
 * state 111 and so on). mode_requestChange answers 0 when it took the change
 * and refuses while another change is queued, with eight modes pushed, while
 * the game blocks the pause (0x156B058[3]) or while the player has no health;
 * the helper asks again on its next call. The change puts the mode timer back
 * to 0 when it is made. pendingGameMode is -2 when nothing is queued.
 *
 * Leaving for a planet load resets the mode stack to the mode being left
 * (0xB4DBD8) and the new planet starts in ordinary play (0xB4CA84), so the
 * pause ends with the load.
 *
 * The current planet is -1 on the title screen (set at 0x11648) and from the
 * planet's number once one has loaded. The load request word is set by the
 * game's loader (perform_load sets it itself, 0x83478) and the gameplay loop
 * leaves for the load at the end of the frame that finds it set.
 */
#define SF_GAME_MODE         0x0156B064
#define SF_PENDING_GAME_MODE 0x01329F90
#define SF_MODE_TIMER        0x0156B06C
#define SF_LOAD_REQUEST      0x0156B050
#define SF_CURRENT_PLANET    0x01329A3C

#define SF_MODE_PAUSE     0x00000003
#define SF_MODE_SPACE     0x00000006
#define SF_MODE_LAST      0x00000009
#define SF_MODE_NONE      0xFFFFFFFE
#define SF_PAUSE_SCREEN   1

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY         0x010D4FFC
#define SF_FN_PERFORM_LOAD   0x00083250
#define SF_FN_REQUEST_MODE   0x00B8BF48

#endif /* QWARK_SF_RAC2_H */
