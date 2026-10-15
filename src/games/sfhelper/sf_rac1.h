/*
 * Ratchet & Clank (NPEA00385): the savefile helper's address table.
 *
 * From racman's mods/NPEA00385/sfhelper: input.c for the logic, npea00385.h for
 * the game addresses and patch.txt for the caves and the hook. That mod is
 * "originally written by doesthisusername for Rackets".
 *
 * What was left out of the port, and why:
 *
 *   lv2.bin  a syscall thunk at 0x4F6500, plus the pointer word at 0x710FC0 that
 *            registered it. The savefile path never makes a syscall - it moves
 *            bytes inside the process with the game's own memcpy - so nothing
 *            reads either of them.
 *   render.bin, tmp_fpu.bin  the on-screen display of a different mod. make.sh
 *            builds them and patch.txt never writes them.
 *   tramp.bin  a register-preserving trampoline at 0x4F63C0 that inputp called
 *            with the helper's address in r0. Its work is folded into the input
 *            hook below, which saves the same registers and branches straight to
 *            the helper, so one cave does what two did.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC1_H
#define QWARK_SF_RAC1_H

/* ------------------------------------------------------------------- caves */

/*
 * Two caves, both addresses racman's own patch.txt writes to.
 *
 *   SF_CAVE_STUB  the input hook, which took inputp.bin
 *   SF_CAVE       the helper itself, which took input.bin (644 bytes there)
 *
 * Both are in functions nothing reaches: the stub in FUN_004F66C4, the helper in
 * FUN_004F7EB8, whose function descriptors (0x710ED0, 0x710FC0) no code and no
 * data word in the executable refers to. The helper may run to the end of that
 * function, 0x4F8C68, and no further; build.sh fails a helper that would
 * (build 53, when the pause made it grow).
 */
#define SF_CAVE_STUB 0x004F6800
#define SF_CAVE      0x004F8000
#define SF_CAVE_END  0x004F8C68

/*
 * The hook site: the post-input-poll instruction patch.txt replaces, which is
 * `ld r0, 0xA0(r1)`. The stub runs the helper, puts every register back and then
 * executes that displaced instruction itself before returning.
 *
 * Absolute, because that is the form racman's word (0x484F6803) has: `bla`.
 *
 * The site is the epilogue of the pad routine (0x11E2D4), which the main loop
 * (0xE0E60) calls first thing in every frame, whatever the game state: the
 * helper runs before that frame's update and render.
 */
#define SF_HOOK_ADDR     0x0011E3A0
#define SF_HOOK_ABSOLUTE 1

/* -------------------------------------------------------- the request bytes */

/*
 * Build 53. The four bytes the client drives, the copy counter and the two words
 * the pause keeps between frames.
 *
 * Up to build 52 the bytes were the mod's, 0xB00070..0xB00073, which is inside
 * the game's bump arena (0xA572C4..0xE572C4), and the counter sat at 0xFFFFFC.
 * They live in the tail of a static buffer of the game's now: 0xFB0230 is a
 * 0xF0000-byte buffer (the next address the executable names is 0x10A0230)
 * that only the debug menu's save-to-memory command (0x2B1D98, through
 * 0x13EA48) ever serialises a save into, and a save is 0xA4A14 bytes, so not
 * even that command writes past 0x1054C44. No code or data word in the
 * executable names anything in 0x1054C44..0x10A0230.
 *
 * It is inside the read-write segment's memory size, so an RPCS3 patch may
 * write it (the segment ends at 0x1606310), and it is BSS, so it is zero when
 * the game loads.
 *
 * qwark's rac1.h names the helper byte and the autosave byte again.
 */
#define SF_API_MOD      0x01080000
#define SF_API_LOAD     0x01080001
#define SF_API_SETASIDE 0x01080002
#define SF_API_SAVEMODE 0x01080003
#define SF_API_I        0x01080004

/*
 * SF_PAUSE_WAIT counts the calls a load has waited for the pause it asked for,
 * 0 while no load waits; SF_PAUSE_FRAME is the game's frames-in-state counter
 * as the helper read it in the call that paused, so the load goes on a later
 * frame. Deadlocked's SF_PAUSE_WAIT_MAX, ten seconds at 60 frames a second.
 */
#define SF_PAUSE_WAIT     0x01080008
#define SF_PAUSE_FRAME    0x0108000C
#define SF_PAUSE_WAIT_MAX 600

/* ------------------------------------------------------- the aside buffer */

/*
 * Build 53. The mod's buffer was 0x1000000..0x10B0000, whose last 0xFDD0 bytes
 * are game variables: from 0x10A0230 on the executable names statics that the
 * moby code at 0x2B3468..0x2C9xxx reads and writes, so a set-aside wrote the tail
 * of a save over them and the game wrote back into the tail of the save. It is
 * in the same debug-only buffer as the request bytes now, below them, clear of
 * both. The size is unchanged, so every save file is still the same length.
 */
#define SF_ASIDE_ADDR 0x00FC0000
#define SF_ASIDE_SIZE 0x000B0000

/* Where the planet the save was made on sits inside the save data. */
#define SF_ASIDE_PLANET_OFFSET 0x18

/* The game's planets are 0..19; the render dispatch at 0xD93F4 knows no other. */
#define SF_PLANET_COUNT 20

/*
 * Build 53. The first two words of every save this executable writes and the
 * only ones its restore (0xE6FE4) accepts: the sizes of the global block and of
 * one planet's block, which the serialiser (0xE7984) computes from the two
 * field tables at 0x726690 and 0x7269A0. Anything else is not a save, and the
 * restore refuses it without loading anything - after which the helper would
 * still have sent the game to whatever word sat at SF_ASIDE_PLANET_OFFSET.
 */
#define SF_SAVE_HEAD_GLOBAL 0x0000153C
#define SF_SAVE_HEAD_PLANET 0x000082A4

/* --------------------------------------------------------- game addresses */

/*
 * savedata_buf is a pointer plus a bias in this game rather than a struct field:
 * *(0xA10928) + 0x100000, exactly as npea00385.h spells it. The game serialises a
 * save into it when it saves (save_handler 0xE8888), so it holds the last save,
 * not the live state, and nothing at all before the first one.
 */
#define SF_SAVEDATA_PTR  0x00A10928
#define SF_SAVEDATA_BIAS 0x00100000

/*
 * The planet load the helper kicks off once the save is in. The main loop runs
 * frames while SF_SHOULD_LOAD is 0 and leaves for the load at the end of the
 * frame that finds it set; the word goes back to 0 after the planet has loaded
 * (0x13DAC), so it is non-zero for the whole of a load.
 */
#define SF_DEST_PLANET 0x00A10704
#define SF_SHOULD_LOAD 0x00A10700

/*
 * Build 53: the pause. The game state picks what the main loop runs each frame
 * (0xE1438): 0 ordinary play (update 0x166FB0, render 0x6C424), 3 the start
 * menu (update 0x13B838, render 0x6C558), 4 a menu or dialog over the world, 6
 * the title, and so on. Of those, 3 alone neither updates nor renders the world:
 * 0x6C558 never reaches the world render 0x6BB78 and its sky dispatch 0xD93C0.
 * SF_STATE_FRAMES counts the frames spent in the current state; the loop puts it
 * back to 0 at the end of a frame whose state changed.
 */
#define SF_GAME_STATE   0x00A10708
#define SF_STATE_FRAMES 0x00A1070C
#define SF_STATE_PLAY   0x00000000
#define SF_STATE_PAUSE  0x00000003

/*
 * What the game's own Start button tests before it opens the start menu
 * (0x166FB0, the branch at 0x16734C that ends in the call at 0x1673C0), which
 * the helper tests too, so it never opens the menu where the game would not:
 * the state has lasted eight frames, the player is not in states 29, 50 or 114,
 * the three player words below say so, and Start would not open another menu
 * instead (0x96BD6C 22 opens menu 0 at 0x167340, Quartu with 0x96BD84 at 2 opens
 * menu 4 at 0x1671D0). The debug camera makes the opener return without
 * pausing (0x13AC44), so it is tested as well.
 */
#define SF_PLAYER_STATE     0x0096BD64
#define SF_PLAYER_STATE_B   0x0096BD6C
#define SF_PLAYER_FLAG_A    0x0096AD8C
#define SF_PLAYER_FLAG_B    0x0096BD91
#define SF_PLAYER_FLAG_C    0x0096BD84
#define SF_PLAYER_WORD      0x0096BF88
#define SF_DEBUG_MODE       0x0095C5D4
#define SF_CURRENT_PLANET   0x00969C70
#define SF_PLANET_QUARTU    15

#define SF_STATE_FRAMES_MIN 8
#define SF_PSTATE_NO_PAUSE1 29
#define SF_PSTATE_NO_PAUSE2 50
#define SF_PSTATE_NO_PAUSE3 114
#define SF_PSTATE_B_MENU    22

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY       0x005C5AD0
#define SF_FN_PERFORM_LOAD 0x000E8CA0
#define SF_FN_SAVE_HANDLER 0x000E8888

/*
 * Build 53. The start menu's opener, which the Start button calls with 0 for the
 * menu's first screen: it sets the game state to 3 before it returns.
 */
#define SF_FN_OPEN_PAUSE   0x0013ABCC

#endif /* QWARK_SF_RAC1_H */
