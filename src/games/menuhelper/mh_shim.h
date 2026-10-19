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
 *   MH_RESTART_MAIN  MH_SHOW_* bits a MAIN widget cannot take in place: a page
 *                    shown with any of them makes the core start the menu's
 *                    screen again (shim_switch) instead of calling shim_show.
 *                    0 for a widget that takes every page in place (Deadlocked's
 *                    list refits itself); UYA's main pause list lays its rows
 *                    and frame out only as it starts, so MH_SHOW_NEW | ROWS.
 *   MH_RESTART_CFG   the same for the CONFIG widget
 *   MH_HOLD_MAIN     (round 3) frames a MAIN screen that is going still draws
 *                    after the next screen has started (mh_core_start): the
 *                    core leaves its text alone that long (below, "the two text
 *                    sets"). 0 when it is gone by then. The default for every
 *                    switch away from a MAIN screen; mh_core_hold overrides it
 *                    for one switch. Deadlocked: 2 (its list's container is
 *                    hidden the frame after GuiSupport_CloseMenu); UYA: 0.
 *   MH_HOLD_CFG      the same for a CONFIG screen
 *   MH_ENTRY_LABEL   (round 3) the label of the game's own pause-menu row the
 *                    renderer takes over, as the game's list cases it: "TRAINER"
 *                    in Deadlocked and UYA, "Trainer" in a title-case game. The
 *                    stubs answer it raw; the core cases it for the title of
 *                    the page it shows before qwark has posted one.
 *
 * ------------------------------------------------------- the two text sets
 *
 * (Round 3.) Everything the core composes for a page - its rows' kinds, flags,
 * ids and text, its title and its number of rows - is a text set, and there
 * are two. mh_nrows, mh_row_*, mh_title, mh_text, mh_label, mh_value and
 * mh_row_range answer the set on show. The contract for a shim:
 *
 *   - A page refreshed in place (shim_show) is written into the set on show,
 *     so a widget keeps every pointer it was given and the new text shows at
 *     once.
 *   - A page that needs the screen started again (shim_switch: the other type,
 *     or MH_RESTART_*) is composed into the other set, and the screen going
 *     keeps the old page's text to its last frame. The new set takes over in
 *     mh_core_start: from then on the macros answer it. So take every pointer
 *     again in mh_core_start, and never hand an outgoing screen a pointer after
 *     it (a text hook that answers mh_text(i) at draw time answers the new set
 *     once mh_core_start has run: if the game still draws the old screen then,
 *     answer it from the set it showed, mh_set_text(set, i), set = mh_set()
 *     taken before the switch).
 *   - A screen that still draws for a while after the next one started (a
 *     fade) says how long in MH_HOLD_* or, per switch, with mh_core_hold. For
 *     that many of the core's frames the other set stays as it is: a page that
 *     would need it again (a further switch) waits in the mailbox, while a
 *     page taken in place is written straight into the set on show. Keep it as
 *     short as the game allows: every frame of it delays a quick second switch.
 *   - The description line (mh_desc) is one buffer, written only for the screen
 *     on show, after mh_core_start; a shim whose outgoing screen keeps drawing
 *     its description line blanks it at stop.
 *   - Whenever the menu closes (mh_core_stop that is not a switch) the core
 *     forgets where the cursor was, and qwark puts its root back in the
 *     mailbox: the next open shows the root, the cursor on its first row.
 *
 * A VALUE row flagged MROWF_RANGE (menubox.h) carries four big-endian words a
 * native slider needs, at mh_row_range(i): the value, the lowest, the highest
 * and the step. (value - lowest) and (highest - lowest), modulo 2^32, are the
 * distances along the bar whatever the field's sign. MROWF_SLIDER marks the
 * rows whose range is short enough for a bar to mean something. The text of
 * such a row stays as it is; a shim that draws a bar may draw the text too.
 */

#define MH_CASE_AS_IS       0
#define MH_CASE_UPPER       1
#define MH_CASE_TITLE       2      /* the first letter of every word */

#define MH_STYLE_DOTS       0x01   /* an action with nothing to say shows "..." on a CONFIG page */
#define MH_STYLE_ARROWS     0x02   /* retired in round 3: a choice shows its bare value in every game */
#define MH_STYLE_LINKS      0x04   /* a link shows ">" as its value */
#define MH_STYLE_STAR       0x08   /* the current option of a MAIN list ends in " *" */
#define MH_STYLE_COLON      0x10   /* a CONFIG label with a value ends in ":" (Deadlocked's GAME SETTINGS) */

/* --------------------------------------------------------- the core's state */

/*
 * At MH_STATE. The first words are counters, so a memory view of them reads
 * as a log. Fixed offsets up to ST_SET0; the two text sets after it are sized
 * by the game's MH_ROWS. (Round 3 replaced round 2's single set and its copy of
 * the mailbox's page: the core composes straight out of the mailbox into a set
 * and keeps it only if page_seq held, as the copy was kept.)
 */
#define ST_MAGIC            0x00   /* u32 MH_MAGIC */
#define ST_STARTS           0x04   /* u32 times the menu's screen started */
#define ST_OPEN_REQS        0x08   /* u32 open requests the shim took */
#define ST_OPENS            0x0C   /* u32 times the game's open path answered with the menu */
#define ST_EVENTS           0x10   /* u32 events posted */
#define ST_PAGES            0x14   /* u32 pages taken */
#define ST_RETRIES          0x18   /* u32 pages caught half written */
#define ST_DROPPED          0x1C   /* u32 presses dropped, an event already in flight */
#define ST_SHOWN_SEQ        0x20   /* u32 page_seq of the page last taken */
#define ST_SHOWN_KEY        0x24   /* u32 its key */
#define ST_BUSY_SINCE       0x28   /* u32 MBOX frames when the event in flight went out */
#define ST_HAVE_PAGE        0x2C   /* u8 a page has been taken */
#define ST_FRONT            0x2D   /* u8 the text set on show, 0 or 1 */
#define ST_CURSOR_HINT      0x2E   /* u8 the page's cursor */
#define ST_PFLAGS           0x2F   /* u8 the page's flags */
#define ST_WAITING          0x30   /* u8 the title says qwark is not answering */
#define ST_LAST_CURSOR      0x31   /* u8 the cursor when the screen last stopped; 0 after a close */
#define ST_NEW_KEY          0x32   /* u8 the page taken has a key not shown before */
#define ST_PTYPE            0x33   /* u8 the type the page last taken is drawn with (after the fallback) */
#define ST_SHOWN            0x34   /* u8 the type on screen now, MH_NONE when none */
#define ST_SWITCHING        0x35   /* u8 the screen is being started again; the other set holds its page */
#define ST_DESC_ROW         0x36   /* u8 the row the description line is about, MH_NONE */
#define ST_HOLD             0x37   /* u8 frames the other set stays on show by a screen that is going */
#define ST_DESC             0x38   /* char[MH_DESC_LEN] the description line */
#define ST_SET0             0x7C   /* the two text sets, MH_SET_SIZE each */

#define MH_NONE             0xFF

#define MH_TITLE_LEN        0x24   /* a title, terminated */
#define MH_DESC_LEN         0x44   /* a description, terminated */
#define MH_TEXT_LEN         0x44   /* a row: MAIN, one string; CONFIG, the label, then the value */
#define MH_LABEL_LEN        0x2C
#define MH_VALUE_LEN        0x18   /* MH_TEXT_LEN - MH_LABEL_LEN */
/* Where a MROWF_RANGE row's four words sit in its text, past the shorter label such a row has. */
#define MH_RANGE_MAIN       0x30
#define MH_RANGE_CFG        0x1C

/* A text set, from its start. */
#define SET_NROWS           0x00   /* u8 rows */
#define SET_PTYPE           0x01   /* u8 the type its page is drawn with */
#define SET_KIND            0x04   /* u8[MH_ROWS] */
#define SET_FLAGS           (SET_KIND + MH_ROWS)
#define SET_ID              ((SET_FLAGS + MH_ROWS + 1) & ~1)
#define SET_TITLE           ((SET_ID + 2 * MH_ROWS + 3) & ~3)
#define SET_TEXT            (SET_TITLE + MH_TITLE_LEN)
#define MH_SET_SIZE         (SET_TEXT + MH_ROWS * MH_TEXT_LEN)
#define MH_CORE_SIZE        ((ST_SET0 + 2 * MH_SET_SIZE + 7) & ~7)

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

/* A game header that predates MH_RESTART_*: every page is taken in place. */
#ifndef MH_RESTART_MAIN
#define MH_RESTART_MAIN     0
#endif
#ifndef MH_RESTART_CFG
#define MH_RESTART_CFG      0
#endif

/* One that predates MH_HOLD_*: a screen going is gone once the next has started. */
#ifndef MH_HOLD_MAIN
#define MH_HOLD_MAIN        0
#endif
#ifndef MH_HOLD_CFG
#define MH_HOLD_CFG         0
#endif

#ifndef MH_ENTRY_LABEL
#define MH_ENTRY_LABEL      "Trainer"
#endif

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
 * The menu's screen has started: the page a switch was for takes over (its
 * text set becomes the one on show), or on a fresh open the page qwark posted
 * is taken; answers the type to draw it with (MBOX_PTYPE_*). The shim builds
 * that widget and shows the rows, the cursor on mh_core_cursor(last), with the
 * title mh_title().
 */
MH_API int  mh_core_start(int p);

/*
 * The screen is stopping: on the way to a start with the other widget, or a
 * close, after which the next open starts over at qwark's root.
 */
MH_API void mh_core_stop(int p);

/*
 * Every frame the screen is up, before the shim reads its input: counts the
 * frame, acts on a close request (shim_close), takes a new page (shim_show,
 * or shim_switch for the other widget), keeps the title and the description
 * line. Answers MH_GONE when the shim must do nothing more this frame. While a
 * switch is under way (the screen asked to start again and not started yet) it
 * takes no page, and presses but Start and Select are ignored.
 */
MH_API int  mh_core_frame(int p);

/* A press of `button` with the cursor on `row`: an event for qwark, or a close. */
MH_API void mh_core_press(int p, u32 button, u32 row);

/* Where the cursor goes on the page just taken: its own row when it is new, else `keep`. */
MH_API u32  mh_core_cursor(u32 keep);

/* For a shim that opens the menu from C: 1, and the request taken, when qwark asked. */
MH_API u32  mh_core_open_request(void);

/*
 * Round 3: how many more of the core's frames the screen going still draws
 * the other text set, which the core leaves alone that long (a page that
 * would need it waits in the mailbox). The core sets it from MH_HOLD_MAIN or
 * MH_HOLD_CFG, by the type of the screen going, just before it calls
 * shim_switch; a shim that knows better for a given transition calls this in
 * shim_switch or after mh_core_start (0: the screen going is gone already). A
 * fresh open sets it to 0, so calling it before mh_core_start then is lost.
 */
MH_API void mh_core_hold(u32 frames);

/*
 * What the core composed, by game address, from the text set on show (above).
 * mh_set() is that set's start; mh_set_text(set, i) row i's text in a given
 * set, for a shim that must keep answering an outgoing screen from its own.
 */
#define mh_set()            ((u32)MH_STATE + ST_SET0 + r8(MH_STATE + ST_FRONT) * (u32)MH_SET_SIZE)
#define mh_set_text(set, i) ((u32)(set) + SET_TEXT + (u32)(i) * MH_TEXT_LEN)
#define mh_nrows()          r8(mh_set() + SET_NROWS)
#define mh_row_kind(i)      r8(mh_set() + SET_KIND + (u32)(i))
#define mh_row_flags(i)     r8(mh_set() + SET_FLAGS + (u32)(i))
#define mh_root()           ((r8(MH_STATE + ST_PFLAGS) & MBOX_PAGE_ROOT) != 0)
#define mh_title()          (mh_set() + SET_TITLE)
#define mh_desc()           ((u32)MH_STATE + ST_DESC)
#define mh_text(i)          mh_set_text(mh_set(), i)                             /* MAIN */
#define mh_label(i)         mh_text(i)                                           /* CONFIG */
#define mh_value(i)         (mh_text(i) + MH_LABEL_LEN)                          /* CONFIG */
/* A MROWF_RANGE row's four words: value, lowest, highest, step. */
#define mh_row_range(i)     (mh_text(i) + (r8(mh_set() + SET_PTYPE) == MBOX_PTYPE_CONFIG ? MH_RANGE_CFG : MH_RANGE_MAIN))
#define mh_count(off)       w32(MH_STATE + (off), r32(MH_STATE + (off)) + 1)

/* ------------------------------------------------------- the shim, for the core */

/*
 * Show the page the core composed: `how` is MH_SHOW_* (0 when only text or
 * colours changed). The widget keeps the pointers mh_text, mh_label and
 * mh_value give it; they do not move.
 */
MH_API void shim_show(int p, u32 how);

/*
 * Start the menu's screen again, so that mh_core_start runs: the next page is
 * drawn with the other widget, or it is one the widget cannot take in place
 * (MH_RESTART_MAIN, MH_RESTART_CFG). The stop on the way is not a close.
 */
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
