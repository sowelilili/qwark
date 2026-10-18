/*
 * The console menu's mailbox, version 1: the block of game memory that qwark
 * and a game's menu renderer talk through (PROTOCOL.md section 9.4 says the
 * same in prose).
 *
 * qwark owns the menu (src/core/menu.c): the pages, the navigation, what a row
 * does. A renderer is a small piece of code qwark writes into the game, one per
 * game, that draws the page it finds here with the game's own menu widgets and
 * posts back what the player pressed. Nothing in this file knows a game: every
 * renderer, Deadlocked's today and RaC1's, RaC2's and UYA's later, speaks the
 * same block, and only its address, its row capacity and its capabilities
 * differ.
 *
 * Plain #defines and nothing else, so the renderer's C and assembler
 * (src/games/menuhelper/) include it exactly as qwark does.
 *
 * Everything is big-endian, as the PS3 is. Each field has one writer, named
 * below; the one exception is the request byte, a handshake like the savefile
 * helper's request bytes: qwark sets it, the renderer clears it when it takes
 * the request, and qwark clears it itself to withdraw one nobody took.
 *
 * Every string is a fixed slot, NUL-padded and not necessarily terminated when
 * full: the title, the hint, and per row its label, its value and, in a block
 * whose rows are MBOX_ROW_SIZE_DESC long, its description. A renderer finds
 * row i's strings at a fixed place whatever it draws them with, the game's own
 * text calls, a widget that keeps a char *, or a hook that resolves a text id
 * to row i's label.
 *
 * qwark writes the page while the game runs, from another thread. So a
 * renderer never hands the game a pointer into the mailbox: the games' text
 * widgets keep the pointer they are given and read it at every draw. It copies
 * a committed page (page_seq even, and the same before and after the copy)
 * into buffers of its own, and points the game at those, which only it changes,
 * on the game's own thread.
 */
#ifndef QWARK_MENUBOX_H
#define QWARK_MENUBOX_H

#define MBOX_MAGIC          0x514D4E55   /* 'QMNU' */
#define MBOX_VERSION        1

/* --------------------------------------------------------------- the header */

#define MBOX_OFF_MAGIC      0x00   /* u32 'QMNU'                              install */
#define MBOX_OFF_VERSION    0x04   /* u16 MBOX_VERSION                        install */
#define MBOX_OFF_SIZE       0x06   /* u16 bytes in the whole block            install */
#define MBOX_OFF_ROW_CAP    0x08   /* u8 row slots in the block               install */
#define MBOX_OFF_ROW_SIZE   0x09   /* u8 MBOX_ROW_SIZE                        install */
#define MBOX_OFF_MAX_ROWS   0x0A   /* u8 rows the renderer shows at once      renderer */
#define MBOX_OFF_CAPS       0x0B   /* u8 MBOX_CAP_* bits                      renderer */
#define MBOX_OFF_HSTATE     0x0C   /* u8 MBOX_HSTATE_*                        renderer */
#define MBOX_OFF_REQUEST    0x0D   /* u8 MBOX_REQ_*            qwark sets, renderer clears */
#define MBOX_OFF_RENDERER   0x0E   /* u8 the renderer's game id (4 Deadlocked) install */
#define MBOX_OFF_RES0F      0x0F   /* u8 0 */
#define MBOX_OFF_FRAMES     0x10   /* u32 frames the menu has been up         renderer */
#define MBOX_OFF_EV_SEQ     0x14   /* u32 bumped after the record below       renderer */
#define MBOX_OFF_EV_KIND    0x18   /* u8 MBOX_EV_*                            renderer */
#define MBOX_OFF_EV_ROW     0x19   /* u8 the row's index in the page          renderer */
#define MBOX_OFF_EV_ID      0x1A   /* u16 the row's id, echoed                renderer */
#define MBOX_OFF_EV_KEY     0x1C   /* u32 the page key the event was made on  renderer */
#define MBOX_OFF_EV_ACK     0x20   /* u32 the last ev_seq qwark has dealt with  qwark */
#define MBOX_OFF_PAGE_SEQ   0x24   /* u32 seqlock: odd while qwark writes       qwark */

/* The page: everything from here on is written between the two page_seq writes. */
#define MBOX_OFF_PAGE_KEY   0x28   /* u32 which page this is (below)            qwark */
#define MBOX_OFF_NROWS      0x2C   /* u8 rows in use, 1..row_cap                qwark */
#define MBOX_OFF_CURSOR     0x2D   /* u8 the row the cursor goes to on a new key qwark */
#define MBOX_OFF_PFLAGS     0x2E   /* u8 MBOX_PAGE_*                            qwark */
#define MBOX_OFF_RES2F      0x2F   /* u8 0 */
#define MBOX_OFF_TITLE      0x30   /* char[32], NUL-padded                      qwark */
#define MBOX_OFF_HINT       0x50   /* char[48], a line about the page: the last result qwark */
#define MBOX_OFF_ROWS       0x80   /* rows[row_cap], row_size each              qwark */

#define MBOX_TITLE_LEN      32
#define MBOX_HINT_LEN       48
#define MBOX_HEAD_SIZE      MBOX_OFF_ROWS
#define MBOX_READ_HEAD      0x28   /* what qwark polls: magic .. page_seq */

/* --------------------------------------------------------------------- a row */

/*
 * A row is MBOX_ROW_SIZE bytes, or MBOX_ROW_SIZE_DESC for a renderer that shows
 * a description line (the Cheats screen's kind of list): the block's row_size
 * says which, and qwark writes the description only where there is room.
 */
#define MBOX_ROW_SIZE       0x40
#define MBOX_ROW_SIZE_DESC  0x80
#define MROW_OFF_KIND       0x00   /* u8 MROW_* */
#define MROW_OFF_FLAGS      0x01   /* u8 MROWF_* */
#define MROW_OFF_ID         0x02   /* u16 opaque to the renderer, echoed in events */
#define MROW_OFF_LABEL      0x04   /* char[40], NUL-padded */
#define MROW_OFF_VALUE      0x2C   /* char[20], NUL-padded */
#define MROW_OFF_DESC       0x40   /* char[64], NUL-padded, rows of MBOX_ROW_SIZE_DESC only */
#define MROW_LABEL_LEN      40
#define MROW_VALUE_LEN      20
#define MROW_DESC_LEN       64

/* Row kinds: what the row is, for a renderer that draws them differently. */
#define MROW_INFO           0      /* read-only text */
#define MROW_LINK           1      /* opens another page */
#define MROW_TOGGLE         2      /* on/off; ON says which */
#define MROW_ACTION         3      /* does something once */
#define MROW_CHOICE         4      /* one of a list; activating opens the list */
#define MROW_VALUE          5      /* a number; Left/Right step it */
#define MROW_OPTION         6      /* one entry of a pick-one list; ON is the current one */
#define MROW_MORE           7      /* the next part of a page too long for the screen */
#define MROW_PREV           8      /* the part before */
#define MROW_BACK           9      /* the page before, for a renderer with no back button */

/* Row flags. */
#define MROWF_ENABLED       0x01   /* the cursor may land on it and activate it */
#define MROWF_ON            0x02   /* active: a toggle that is on, the current option */
#define MROWF_STEPS         0x04   /* Left and Right do something on this row */
#define MROWF_CLOSES        0x08   /* activating it closes the menu before it acts */

/* -------------------------------------------------------- the other fields */

/* caps: what the renderer can do. */
#define MBOX_CAP_STEPS      0x01   /* it posts LEFT and RIGHT */
#define MBOX_CAP_PAGE_KEYS  0x02   /* it posts PAGE_PREV and PAGE_NEXT */
#define MBOX_CAP_BACK       0x04   /* it has a back control of its own, so no BACK rows */
#define MBOX_CAP_REQUEST    0x08   /* it opens and closes the menu when asked */

/* hstate. */
#define MBOX_HSTATE_CLOSED  0
#define MBOX_HSTATE_OPEN    1

/* request. */
#define MBOX_REQ_NONE       0
#define MBOX_REQ_OPEN       1
#define MBOX_REQ_CLOSE      2

/* Event kinds. */
#define MBOX_EV_ACTIVATE    1      /* the cross, or whatever selects */
#define MBOX_EV_LEFT        2
#define MBOX_EV_RIGHT       3
#define MBOX_EV_BACK        4      /* back on a page that is not the root */
#define MBOX_EV_PAGE_PREV   5
#define MBOX_EV_PAGE_NEXT   6

/* pflags. */
#define MBOX_PAGE_ROOT      0x01   /* back here leaves the menu, which the renderer does itself */

#endif /* QWARK_MENUBOX_H */
