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
 * word boundary.
 *
 * Build 53 first put the pause's two words after the counter, at 0x1317730 and
 * 0x1317734, for a load that waited for the pause it had asked the game for.
 * This helper enters the pause itself and loads in the same call (below), so
 * it waits for nothing and they are unused again.
 */
#define SF_API_MOD      0x01317728
#define SF_API_LOAD     0x01317729
#define SF_API_SETASIDE 0x0131772A
#define SF_API_I        0x0131772C

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
 *                     -1, debug, runs the same
 *   1  movie          0xBF849C, a story cutscene: a frame of a Bink file a
 *                     call, out of the movie player at 0x1410C18
 *   2  scene          0xBF77A8, 0xB10208 (the world): a cutscene the engine
 *                     runs, with mobies of its own
 *   3  pause          the pause menu: 0xBB93D0, 0xB105E4 (0xBBA610)
 *   4  freeze         a message over a stopped game: 0xB23568 runs the message
 *                     and 0xB10240 draws it, over the world (0xB0F908) unless
 *                     it came up over the pause (0xB222EC); over the vendor it
 *                     runs the vendor's two instead
 *   5  vendor         0xBFEB38, 0xC04DB8
 *   6  space          the ship's trip between planets, 0xBE7400, and
 *                     0xBE9308, which draws and then calls the pad routine
 *   7..9              0xBF66D0/0xB10338, 0xC1108C/0xC10F70, 0xB3D960/0xB3D984
 *
 * and anything else is a trap (0xB4D448). A cutscene is a queue of movies and
 * scenes (the object at 0x157840C: 0xB2862C adds one, 0xB287B0 asks for the
 * first, pushed over the mode it interrupts) that ends by popping back to that
 * mode, and the movie or the scene that is its last runs the callback at
 * 0x132A010 on its way out (below).
 *
 * Freeze does not update the world but draws it, so it would not stop the
 * render that crashed RaC1; the pause alone runs neither the world's update nor
 * its render, nor anything that reaches them, and the helper loads from the
 * pause.
 *
 * Build 53 first asked the game for it, through mode_requestChange (0xB8BF48)
 * as the Start button does, and loaded on a later frame that ran in it. On
 * hardware a load during a cutscene still failed. The ways it could, read from
 * the executable: the request is refused while the cutscene has a change
 * queued (it queues its next movie or scene, or its own end, from its update,
 * which runs before the helper); the change, made at the end of the frame
 * (0xB8C320), waits while 0x1345524 counts down or the player has no health;
 * and once it is made it runs the leaving mode's exit, and a movie's
 * (0xBF8304) or a scene's (0xBF6D38) runs the cutscene's callback - the moby's
 * own continuation, which deletes the cutscene's mobies, hands out what it
 * earned, saves the game (0x83060) or starts the next planet's load,
 * as 0xE5DE1C does (0xB2810C, planet 2) - after which the load request was set
 * and the helper refused the load.
 *
 * So the helper does what RaC1's does and writes the mode itself, from
 * whatever mode it finds and without asking: the pause in gameMode, nothing
 * queued in pendingGameMode (-2) and nothing in the queued change's flag
 * pointer (0x1345534, through which the change writes a 1 when it is made).
 * RaC2's helper runs last in the frame, and that is what makes the write
 * enough on its own. The frame's update and render are over, and drew the old
 * planet as it still is; everything the gameplay loop runs after the pad
 * routine (from 0xB4D44C) reads gameMode afresh, and with the pause in it none
 * of it reaches the world: the catch-up that can run ordinary play's update
 * again (0xB4D8D4) or the scene's (0xB4D988) takes the pause's path (0xB4D9C0),
 * the clock tick at 0xB4D470 is ordinary play's alone, and the mode change has
 * nothing to make (0xB8C35C). The loader runs in the same call and sets the
 * load request itself (perform_load, 0x83478), and the gameplay loop leaves for
 * the load at the end of this same frame (0xB4DB90): no frame of the pause, and
 * so no pause menu, ever runs, which is why neither the pause's entry nor the
 * leaving mode's exit is needed. Leaving resets the mode stack (0xB8C2BC, at
 * 0xB4DBD8), 0xBEA188 puts the game in space for the trip, and the new planet's
 * gameplay loop starts in ordinary play with nothing queued (0xB4CA84). From
 * the NPEA00386 executable; not yet run on hardware.
 *
 * The current planet is -1 on the title screen (set at 0x11648) and from the
 * planet's number once one has loaded. The load request word is set by the
 * game's loader and by every planet load (0xB2810C), and the gameplay loop
 * leaves for the load at the end of the frame that finds it set.
 */
#define SF_GAME_MODE         0x0156B064
#define SF_PENDING_GAME_MODE 0x01329F90
#define SF_MODE_CHANGE_FLAG  0x01345534
#define SF_LOAD_REQUEST      0x0156B050
#define SF_CURRENT_PLANET    0x01329A3C

#define SF_MODE_PAUSE     0x00000003
#define SF_MODE_SPACE     0x00000006
#define SF_MODE_NONE      0xFFFFFFFE

/* The modes the gameplay loop runs, -1 to 9, as an unsigned range from -1. */
#define SF_MODE_FIRST     0xFFFFFFFF
#define SF_MODE_COUNT     11

/*
 * What a cutscene leaves behind that the planet load does not put back. The
 * movie's and the scene's exits would, and the helper runs neither, so it does
 * the four things they and the Start button's skip do that the load needs:
 *
 *   - the movie player's Bink file, open while a movie plays (the player is the
 *     object at 0x1410C18, made on first use). 0x10A7990 closes it and gives
 *     its textures back, and does nothing when none is open; it is what Start
 *     calls to skip a movie (0xBF8938), at the same point in the frame.
 *   - the cutscene's queue at 0x157840C, whose movies and scenes not yet shown
 *     would otherwise play first in the next cutscene the new planet asks for.
 *     0xB28534 empties it, as both skips do (0xBF8944, 0xBF7830); the bits it
 *     sets on the way, the movies seen at 0x1481980, are a field of the save
 *     (the table at 0x131AC94) and the load writes them over.
 *   - the callback the cutscene's end runs, at 0x132A010 with its argument at
 *     0x132A014: the exits run it once the queue is empty and clear it
 *     (0xBF8460, 0xBF71C0), and left set it would run at the end of the new
 *     planet's next cutscene, with a moby of the old planet's for an argument.
 *     The helper clears it without running it: what it would have done is the
 *     old planet's business, and the save being loaded carries its own.
 *   - SF_MOVIE_HELD, the byte a movie's exit sets when the console's freeze
 *     (below) holds the movie rather than ending it, so that the movie goes on
 *     where it was once the freeze is over. It stays set until that movie's own
 *     exit clears it (0xBF83E0), and while it is set the movie entry (0xBF8010)
 *     starts nothing: left set, the new planet's next movie would never play.
 *
 * Everything else a cutscene sets up - its mobies, its camera, its sound - is
 * the planet's, and the planet load puts it back. From the NPEA00386
 * executable; not yet run on hardware.
 */
#define SF_MOVIE_PLAYER    0x01410C18
#define SF_SCENE_QUEUE     0x0157840C
#define SF_SCENE_DONE_FN   0x0132A010
#define SF_SCENE_DONE_ARG  0x0132A014
#define SF_MOVIE_HELD      0x019E3660

/*
 * The one thing outside a cutscene that a mode's entry sets and only its exit
 * puts back. The vendor's entry (0xBFAFD0) keeps a word of the renderer's -
 * the object 0x1410D78 points at, +0x170C, set once when the renderer is made
 * (0x10AA27C) and read by its draws - at SF_VENDOR_KEPT and zeroes it, and the
 * vendor's exit (0xBFB9D8) writes it back; nothing else does, the planet load
 * included. Left at 0 it would stay 0 for the rest of the process, through
 * every later visit to a vendor, which keeps and restores the 0. So when the
 * helper leaves the vendor - mode 5, or a freeze over it, which the gameplay
 * loop knows by the mode under it (0xB4D37C: 0xB8C254 and 0xB8C288 read the
 * mode stack's top and the slot past it) - it writes the word back as the
 * exit would. The mode stack is SF_MODE_STACK, a word a mode, SF_MODE_DEPTH
 * deep. The exits of the other modes (the freeze's 0xB220F4, 7's 0xBD7C10, 8's
 * 0xC0F9E0, 9's 0xB3D93C) undo nothing the planet load does not: sounds, the
 * stream volumes and flags on the planet's own mobies. From the NPEA00386
 * executable; not yet run on hardware.
 */
#define SF_MODE_FREEZE     0x00000004
#define SF_MODE_VENDOR     0x00000005
#define SF_MODE_STACK      0x015B22E0
#define SF_MODE_DEPTH      0x01345530
#define SF_RENDERER_PTR    0x01410D78
#define SF_RENDERER_VENDOR 0x0000170C
#define SF_VENDOR_KEPT     0x019E7680

/*
 * The console's own freeze. When the console takes the pad - the PS button's
 * menu, a controller lost - the pad driver sets a byte (0x147A8E0, at
 * 0x10BFEE8) and the gameplay loop pushes a freeze over whatever the game was
 * doing and sets this latch (0xB4D0B4); a movie (0xBF8368) and a scene
 * (0xBF6DA0) are held under it rather than ended. When the console lets go the
 * loop clears the latch and pops back (0xB4D0F0), the one place that clears
 * it, and only from the freeze. A load raised under it would leave the latch
 * set on the new planet, and the game would never again freeze for the
 * console in that process, so the helper refuses it until the console gives
 * the game back. From the NPEA00386 executable; not yet run on hardware.
 */
#define SF_CONSOLE_FREEZE  0x0157CEDC

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY         0x010D4FFC
#define SF_FN_PERFORM_LOAD   0x00083250
#define SF_FN_MOVIE_STOP     0x010A7990
#define SF_FN_SCENE_CLEAR    0x00B28534

#endif /* QWARK_SF_RAC2_H */
