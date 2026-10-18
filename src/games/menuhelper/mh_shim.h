/*
 * The console menu's renderers: the interface between the shared core and a
 * game's shim (revision 1.22, mailbox version 2).
 *
 * A renderer is two pieces of PowerPC code that qwark writes into the game,
 * built together for each game like the savefile helper (build.sh):
 *
 *   the core   mh_core.c, the same source for every game. Everything that is
 *              not the game's own UI: reading the mailbox (src/core/menubox.h)
 *              into stable buffers, the page and cursor state, casing and the
 *              little decorations of a value, turning presses into qwark's
 *              events (one at a time, with the "Waiting for qwark" title when
 *              qwark is slow), the open and close requests, which widget a page
 *              is drawn with and the fallback when a game has only one, and the
 *              counters. It names no game address and calls no game function.
 *   the shim   mh_<game>.c and its hook stubs: only what talks to that game's
 *              menus. It shows the rows the core composed with the game's own
 *              widgets, reads the game's input and hands the core presses, plays
 *              the game's sounds, and opens and closes the menu the game's way.
 *
 * The core calls the shim through the seven shim_* functions below; the shim
 * calls the core through the mh_core_* functions and reads what the core
 * composed through the mh_* macros. Both are compiled with the game's header
 * (mh_<game>.h) included first, which defines the numbers in the first table.
 * PROTOCOL.md section 9.5 and rewrite/SPEC-console-ui.md describe the same for
 * someone writing a shim.
 *
 * Plain #defines and declarations; the assembler includes this file too, so
 * the C parts are fenced off with __ASSEMBLER__.
 */
#ifndef QWARK_MH_SHIM_H
#define QWARK_MH_SHIM_H

#include "../../core/menubox.h"

/*
 * ------------------------------------------------- what a game header defines
 *
 *   MH_GAME_ID       the game, as qwark numbers it (GAME_RAC1 .. GAME_RAC4)
 *   MH_BOX           the mailbox's address in the game
 *   MH_STATE         the core's state (below), MH_CORE_SIZE bytes
 *   MH_SHIM          the shim's own state, after the core's
 *   MH_ROWS          the mailbox's row slots: the larger of the two below
 *   MH_ROW_BYTES     MBOX_ROW_SIZE, or MBOX_ROW_SIZE_DESC to get descriptions
 *   MH_MAIN_ROWS     rows a MAIN page has at most (the mailbox's max_rows)
 *   MH_CFG_ROWS      rows a CONFIG page has at most, 0 for a shim without one
 *   MH_CAPS          MBOX_CAP_* (Left and Right, page keys, back, requests)
 *   MH_CASE_MAIN     MH_CASE_* for a MAIN page's title and rows
 *   MH_CASE_HEAD     the same for a CONFIG page's title,
 *   MH_CASE_LABEL    its labels,
 *   MH_CASE_VALUE    and its values (descriptions are left as qwark wrote them)
 *   MH_STYLE         MH_STYLE_* bits
 *   MH_MAIN_SEP      the string between a MAIN row's label and value, ": "
 *   MH_WAIT_FRAMES   frames an event may go unanswered before the title says so
 *   MH_MAGIC         the state's first word, which names the renderer
 */

#define MH_CASE_AS_IS       0
#define MH_CASE_UPPER       1
#define MH_CASE_TITLE       2      /* the first letter of every word */

#define MH_STYLE_DOTS       0x01   /* an action with nothing to say shows "..." on a CONFIG page */
#define MH_STYLE_ARROWS     0x02   /* a choice shows "< value >" */
#define MH_STYLE_LINKS      0x04   /* a link shows ">" as its value */
#define MH_STYLE_STAR       0x08   /* the current option of a MAIN list ends in " *" */

/* --------------------------------------------------------- the core's state */

/*
 * At MH_STATE. The first words are counters, so a memory view of them reads
 * as a log. Fixed offsets up to ST_KIND; the arrays after it are sized by the
 * game's MH_ROWS.
 */
#define ST_MAGIC            0x00   /* u32 MH_MAGIC */
#define ST_STARTS           0x04   /* u32 times the menu's screen started */
#define ST_OPEN_REQS        0x08   /* u32 open requests the shim took */
#define ST_OPENS            0x0C   /* u32 times the game's open path answered with the menu */
#define ST_EVENTS           0x10   /* u32 events posted */
#define ST_PAGES            0x14   /* u32 pages taken */
#define ST_RETRIES          0x18   /* u32 pages caught half written */
#define ST_DROPPED          0x1C   /* u32 presses dropped, an event already in flight */
#define ST_SHOWN_SEQ        0x20   /* u32 page_seq of the page shown */
#define ST_SHOWN_KEY        0x24   /* u32 its key */
#define ST_BUSY_SINCE       0x28   /* u32 MBOX frames when the event in flight went out */
#define ST_HAVE_PAGE        0x2C   /* u8 a page has been taken */
#define ST_NROWS            0x2D   /* u8 rows shown */
#define ST_CURSOR_HINT      0x2E   /* u8 the page's cursor */
#define ST_PFLAGS           0x2F   /* u8 the page's flags */
#define ST_WAITING          0x30   /* u8 the title says qwark is not answering */
#define ST_LAST_CURSOR      0x31   /* u8 the cursor when the screen last stopped */
#define ST_NEW_KEY          0x32   /* u8 the page taken has a key not shown before */
#define ST_PTYPE            0x33   /* u8 the type the page is drawn with (after the fallback) */
#define ST_SHOWN            0x34   /* u8 the type on screen now, MH_NONE when none */
#define ST_SWITCHING        0x35   /* u8 the screen is being started again for another type */
#define ST_DESC_ROW         0x36   /* u8 the row the description line is about, MH_NONE */
#define ST_KIND             0x38   /* u8[MH_ROWS] */

#define MH_NONE             0xFF

#define MH_TITLE_LEN        0x24   /* a title, terminated */
#define MH_DESC_LEN         0x44   /* a description, terminated */
#define MH_TEXT_LEN         0x44   /* a row: MAIN, one string; CONFIG, the label, then the value */
#define MH_LABEL_LEN        0x2C
#define MH_VALUE_LEN        0x18   /* MH_TEXT_LEN - MH_LABEL_LEN */

#define ST_FLAGS            (ST_KIND + MH_ROWS)
#define ST_ID               ((ST_FLAGS + MH_ROWS + 1) & ~1)
#define ST_TITLE            ((ST_ID + 2 * MH_ROWS + 3) & ~3)
#define ST_DESC             (ST_TITLE + MH_TITLE_LEN)
#define ST_TEXT             (ST_DESC + MH_DESC_LEN)
/* A copy of the mailbox's page, key to hint, then the first 0x40 bytes of each row. */
#define ST_STAGE            (ST_TEXT + MH_ROWS * MH_TEXT_LEN)
#define ST_DSTAGE           (ST_STAGE + MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + MH_ROWS * MBOX_ROW_SIZE)
#define MH_CORE_SIZE        ((ST_DSTAGE + MROW_DESC_LEN + 7) & ~7)

/* Presses a shim hands the core (mh_core_press). */
#define MH_BTN_CROSS        1      /* select */
#define MH_BTN_LEFT         2
#define MH_BTN_RIGHT        3
#define MH_BTN_L1           4      /* the window before */
#define MH_BTN_R1           5      /* the window after */
#define MH_BTN_BACK         6      /* triangle */
#define MH_BTN_CLOSE        7      /* Start or Select */

/* shim_show's `how`. */
#define MH_SHOW_NEW         0x01   /* a page not shown before: the cursor to mh_core_cursor */
#define MH_SHOW_ROWS        0x02   /* it has a different number of rows */

/* shim_close's `how`. */
#define MH_CLOSE_BACK       1      /* back from the root: the game's own back */
#define MH_CLOSE_EXIT       2      /* Start, Select or a close request: the game's own close */

/* shim_sound's sounds. */
#define MH_SND_MOVE         0
#define MH_SND_FORWARD      1
#define MH_SND_BACK         2
#define MH_SND_STEP         3      /* a value or a choice changed */

/* mh_core_frame's answer. */
#define MH_GONE             1      /* the screen is closing or being started again: stop this frame */

#ifndef __ASSEMBLER__

/*
 * Both halves are one program on the console. The host tests compile a core
 * and a shim into each of two test files, so there everything is file-local.
 */
#ifdef MH_HOST
#define MH_API static __attribute__((unused))
#else
#define MH_API
#endif

/* ------------------------------------------------------- the core, for shims */

/*
 * The menu's screen has started: takes the page qwark posted, and answers the
 * type to draw it with (MBOX_PTYPE_*). The shim builds that widget and shows
 * the rows, the cursor on mh_core_cursor(last), with the title mh_title().
 */
MH_API int  mh_core_start(int p);

/* The screen is stopping (also on the way to a start with the other widget). */
MH_API void mh_core_stop(int p);

/*
 * Every frame the screen is up, before the shim reads its input: counts the
 * frame, acts on a close request (shim_close), takes a new page (shim_show,
 * or shim_switch for the other widget), keeps the title and the description
 * line. Answers MH_GONE when the shim must do nothing more this frame.
 */
MH_API int  mh_core_frame(int p);

/* A press of `button` with the cursor on `row`: an event for qwark, or a close. */
MH_API void mh_core_press(int p, u32 button, u32 row);

/* Where the cursor goes on the page just taken: its own row when it is new, else `keep`. */
MH_API u32  mh_core_cursor(u32 keep);

/* For a shim that opens the menu from C: 1, and the request taken, when qwark asked. */
MH_API u32  mh_core_open_request(void);

/* What the core composed, by game address: */
#define mh_nrows()          r8(MH_STATE + ST_NROWS)
#define mh_row_kind(i)      r8(MH_STATE + ST_KIND + (u32)(i))
#define mh_row_flags(i)     r8(MH_STATE + ST_FLAGS + (u32)(i))
#define mh_root()           ((r8(MH_STATE + ST_PFLAGS) & MBOX_PAGE_ROOT) != 0)
#define mh_title()          ((u32)MH_STATE + ST_TITLE)
#define mh_desc()           ((u32)MH_STATE + ST_DESC)
#define mh_text(i)          ((u32)MH_STATE + ST_TEXT + (u32)(i) * MH_TEXT_LEN)   /* MAIN */
#define mh_label(i)         mh_text(i)                                           /* CONFIG */
#define mh_value(i)         (mh_text(i) + MH_LABEL_LEN)                          /* CONFIG */
#define mh_count(off)       w32(MH_STATE + (off), r32(MH_STATE + (off)) + 1)

/* ------------------------------------------------------- the shim, for the core */

/*
 * Show the page the core composed: `how` is MH_SHOW_* (0 when only text or
 * colours changed). The widget keeps the pointers mh_text, mh_label and
 * mh_value give it; they do not move.
 */
MH_API void shim_show(int p, u32 how);

/* The next page is drawn with the other widget: start the screen again with it. */
MH_API void shim_switch(int p);

/* Close the menu, or go back from its root (MH_CLOSE_*), the game's way. */
MH_API void shim_close(int p, u32 how);

/* mh_title() changed. */
MH_API void shim_title(int p);

/* mh_desc() changed. A shim with no line for it does nothing. */
MH_API void shim_desc(int p);

/* The row the game's cursor is on. */
MH_API u32  shim_cursor(int p);

/* One of the game's sounds, MH_SND_*; nothing for one the game plays itself. */
MH_API void shim_sound(u32 snd);

#endif /* __ASSEMBLER__ */

#endif /* QWARK_MH_SHIM_H */
