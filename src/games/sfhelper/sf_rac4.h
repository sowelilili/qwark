/*
 * Ratchet: Deadlocked (NPEA00423): the savefile helper's address table.
 *
 * From https://github.com/sowelilili/rc4-save, src/patch1.c and src/rc4-save.h.
 * The cave and the hook word agree with racman's mods/NPEA00423/rc4-save.
 *
 * Deadlocked is the one game that cannot simply hand its loader a buffer and
 * carry on. Its load is a menu operation: the helper pauses the game by putting
 * it in game mode 3, waits three frames for the pause to take, hands the buffer
 * to restore_saved_game, and then watches a status word until the game says the
 * player confirmed (15 or 24) or backed out (28). sfhelper.c keeps that shape.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC4_H
#define QWARK_SF_RAC4_H

/* ------------------------------------------------------------------- caves */

#define SF_CAVE 0x00661F9C

/* The hook site, relative as the repo's own word (0x4BF5AE01) is. */
#define SF_HOOK_ADDR     0x0070719C
#define SF_HOOK_ABSOLUTE 0

/* -------------------------------------------------------- the request bytes */

/*
 * The same three bytes qwark's rac4.h already names, plus the two scratch words
 * the load state machine keeps between frames.
 */
#define SF_API_MOD      0x015CD71D
#define SF_API_LOAD     0x015CD71E
#define SF_API_SETASIDE 0x015CD71F
#define SF_API_I        0x015CD720

#define SF_PREV_GAME_MODE 0x015CD750
#define SF_FRAME_TIMER    0x015CD760

/* ------------------------------------------------------- the aside buffer */

#define SF_ASIDE_ADDR 0x01600000
#define SF_ASIDE_SIZE 0x00100000

/* --------------------------------------------------------- game addresses */

/* savedata_buf is the word at +4 of the block savedata_info points at. */
#define SF_SAVEDATA_INFO  0x00A28E68
#define SF_SAVEDATA_FIELD 0x00000004

/* The load menu's status word, and the game mode the helper parks the game in. */
#define SF_MAGIC_ADDR 0x0119F8DC
#define SF_GAME_MODE  0x00B3C5A0

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY             0x007E61F4
#define SF_FN_RESTORE_SAVED_GAME 0x003C902C
#define SF_FN_MAGIC_LOAD         0x003C9398

#endif /* QWARK_SF_RAC4_H */
