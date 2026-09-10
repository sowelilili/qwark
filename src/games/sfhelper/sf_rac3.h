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
 *   the trophy unlocks and the intro skip  three ASM patch words in that repo's
 *            patch.txt (0x1113C, 0x9027A0, 0x985318) that belong to Gigahelper's
 *            other features, not to the savefile path.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC3_H
#define QWARK_SF_RAC3_H

/* ------------------------------------------------------------------- caves */

/* CODE_CAVE_START in Gigahelper's Makefile; it runs to 0x983754. */
#define SF_CAVE 0x00980AC0

/*
 * The hook site is the cellPadSetActDirect call Gigahelper replaces with a call
 * of its own. Relative, as its `bl hook` is.
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

/* ------------------------------------------------------- the aside buffer */

#define SF_ASIDE_ADDR 0x01100000
#define SF_ASIDE_SIZE 0x00200000

/* --------------------------------------------------------- game addresses */

/* savedata_buf is the word at +4 of the block savedata_info points at. */
#define SF_SAVEDATA_INFO  0x00CB0A98
#define SF_SAVEDATA_FIELD 0x00000004

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY       0x0099256C
#define SF_FN_PERFORM_LOAD 0x001E1CF4

#endif /* QWARK_SF_RAC3_H */
