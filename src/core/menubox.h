/*
 * The console menu's mailbox, version 3: the block of game memory that qwark
 * and a game's menu renderer talk through (PROTOCOL.md section 9.4 says the
 * same in prose).
 *
 * Version 2 (round 2, still revision 1.22) adds the page type: a MAIN page is
 * a short list of links and buttons, the game's own button list; a CONFIG page
 * is a two-column list of labels and controls, the kind the game's Cheats
 * screen draws. A renderer says how many rows it takes on a CONFIG page
 * (cfg_rows), and one that has no CONFIG page says 0 and is sent every page as
 * MAIN. A qwark and a renderer of different versions never meet: the install
 * refuses a renderer that is not this build's.
 *
 * Version 3 (round 4, still revision 1.22): no renderer shows a description,
 * so a row is 0x40 bytes and nothing else; and a page longer than the window
 * qwark posts has no More and no Previous row: the page's flags say it goes
 * on above or below the window (MBOX_PAGE_MORE_*), and the renderer asks for
 * the window that continues it when the cursor comes to its edge
 * (MBOX_EV_SCROLL_*), so the window slides under the cursor.
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
 * full: the title, the hint, and per row its label and its value. A renderer
 * finds row i's strings at a fixed place whatever it draws them with, the
 * game's own text calls, a widget that keeps a char *, or a hook that resolves
 * a text id to row i's label.
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
#define MBOX_VERSION        3

/* --------------------------------------------------------------- the header */

#define MBOX_OFF_MAGIC      0x00   /* u32 'QMNU'                              install */
#define MBOX_OFF_VERSION    0x04   /* u16 MBOX_VERSION                        install */
#define MBOX_OFF_SIZE       0x06   /* u16 bytes in the whole block            install */
#define MBOX_OFF_ROW_CAP    0x08   /* u8 row slots in the block               install */
#define MBOX_OFF_ROW_SIZE   0x09   /* u8 MBOX_ROW_SIZE                        install */
#define MBOX_OFF_MAX_ROWS   0x0A   /* u8 rows the renderer shows at once on a MAIN page  renderer */
#define MBOX_OFF_CAPS       0x0B   /* u8 MBOX_CAP_* bits                      renderer */
#define MBOX_OFF_HSTATE     0x0C   /* u8 MBOX_HSTATE_*                        renderer */
#define MBOX_OFF_REQUEST    0x0D   /* u8 MBOX_REQ_*            qwark sets, renderer clears */
#define MBOX_OFF_RENDERER   0x0E   /* u8 the renderer's game id (4 Deadlocked) install */
#define MBOX_OFF_CFG_ROWS   0x0F   /* u8 the same on a CONFIG page, 0 for none  renderer */
#define MBOX_OFF_FRAMES     0x10   /* u32 frames the menu has been up since it opened  renderer */
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
#define MBOX_OFF_PTYPE      0x2F   /* u8 MBOX_PTYPE_*                           qwark */
#define MBOX_OFF_TITLE      0x30   /* char[32], NUL-padded                      qwark */
#define MBOX_OFF_HINT       0x50   /* char[48], a line about the page: the last result qwark */
#define MBOX_OFF_ROWS       0x80   /* rows[row_cap], row_size each              qwark */

#define MBOX_TITLE_LEN      32
#define MBOX_HINT_LEN       48
#define MBOX_HEAD_SIZE      MBOX_OFF_ROWS
#define MBOX_READ_HEAD      0x28   /* what qwark polls: magic .. page_seq */

/* --------------------------------------------------------------------- a row */

/*
 * A row is MBOX_ROW_SIZE bytes, the block's row_size. (Version 2 had rows of
 * 0x80 as well, with a description of 64 characters at +0x40 for a renderer
 * that showed one; round 4 dropped them, no renderer showing any.)
 */
#define MBOX_ROW_SIZE       0x40
#define MROW_OFF_KIND       0x00   /* u8 MROW_* */
#define MROW_OFF_FLAGS      0x01   /* u8 MROWF_* */
#define MROW_OFF_ID         0x02   /* u16 opaque to the renderer, echoed in events */
#define MROW_OFF_LABEL      0x04   /* char[40], NUL-padded */
#define MROW_OFF_VALUE      0x2C   /* char[20], NUL-padded */
#define MROW_LABEL_LEN      40
#define MROW_VALUE_LEN      20

/*
 * Round 3: a VALUE row flagged MROWF_RANGE has a label of 23 characters at
 * most, terminated inside its first 24 bytes, and the label's last 16 bytes
 * hold four big-endian words for a native slider: the value, the lowest, the
 * highest and the step Left and Right move it by. (value - lowest) and
 * (highest - lowest), modulo 2^32, are the distances along the bar whatever
 * the field's sign.
 */
#define MROW_OFF_RANGE      0x1C   /* u32[4], MROWF_RANGE rows only */
#define MROW_RANGE_LABEL    24     /* such a row's label, with its NUL */

/* Row kinds: what the row is, for a renderer that draws them differently. */
#define MROW_INFO           0      /* read-only text */
#define MROW_LINK           1      /* opens another page */
#define MROW_TOGGLE         2      /* on/off; ON says which */
#define MROW_ACTION         3      /* does something once */
#define MROW_CHOICE         4      /* one of a list; activating opens the list */
#define MROW_VALUE          5      /* a number; Left/Right step it */
#define MROW_OPTION         6      /* one entry of a pick-one list; ON is the current one */
#define MROW_MORE           7      /* retired in round 4: never sent (the window slides instead) */
#define MROW_PREV           8      /* retired in round 4: never sent */
#define MROW_BACK           9      /* the page before, for a renderer with no back button */

/* Row flags. */
#define MROWF_ENABLED       0x01   /* the cursor may land on it and activate it */
#define MROWF_ON            0x02   /* active: a toggle that is on, the current option */
#define MROWF_STEPS         0x04   /* Left and Right do something on this row */
#define MROWF_CLOSES        0x08   /* activating it closes the menu before it acts */
#define MROWF_RANGE         0x10   /* a VALUE with bounds: its four words at MROW_OFF_RANGE */
#define MROWF_SLIDER        0x20   /* and few enough steps from end to end for a bar (with RANGE) */

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
#define MBOX_EV_PAGE_PREV   5      /* L1: a window up, on a page that goes on above */
#define MBOX_EV_PAGE_NEXT   6      /* R1: a window down, on a page that goes on below */
#define MBOX_EV_SCROLL_UP   7      /* round 4: the cursor came to the window's first row, the page going on above */
#define MBOX_EV_SCROLL_DOWN 8      /* round 4: to its last row, the page going on below */

/* pflags. */
#define MBOX_PAGE_ROOT      0x01   /* back here leaves the menu, which the renderer does itself */
#define MBOX_PAGE_MORE_UP   0x02   /* round 4: the page goes on above this window */
#define MBOX_PAGE_MORE_DOWN 0x04   /* round 4: and below it */

/*
 * ptype: the widget a page is drawn with. A renderer whose cfg_rows is 0 is
 * only ever sent MAIN pages.
 */
#define MBOX_PTYPE_MAIN     0      /* a short list: links and buttons, "label: value" */
#define MBOX_PTYPE_CONFIG   1      /* labels and controls in two columns, scrolled */

#endif /* QWARK_MENUBOX_H */
