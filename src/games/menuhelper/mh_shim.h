/*
 * The console menu's renderers: the interface between the shared core and a
 * game's shim (revision 1.22, mailbox version 3).
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
 * The core calls the shim through the six shim_* functions below; the shim
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
 *   MH_ROW_BYTES     MBOX_ROW_SIZE (round 4: rows have no description any more)
 *   MH_MAIN_ROWS     rows a MAIN page has at most (the mailbox's max_rows)
 *   MH_CFG_ROWS      rows a CONFIG page has at most, 0 for a shim without one:
 *                    a window of a longer page, which slides under the cursor
 *                    (round 4, below)
 *   MH_CAPS          MBOX_CAP_* (Left and Right, page keys, back, requests)
 *   MH_CASE_MAIN     MH_CASE_* for a MAIN page's title and rows
 *   MH_CASE_HEAD     the same for a CONFIG page's title,
 *   MH_CASE_LABEL    its labels,
 *   MH_CASE_VALUE    and its values
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
 *
 * ------------------------------------------------------ the sliding window
 *
 * (Round 4.) A page longer than the window qwark posts (MH_CFG_ROWS rows on
 * a CONFIG page, MH_MAIN_ROWS on a MAIN one) has no More and no Previous
 * row: the page's flags say it goes on above or below the window
 * (MBOX_PAGE_MORE_*). The core watches shim_cursor every frame, and when the
 * cursor comes to the window's last row it can stand on with the page going
 * on below (its first, going on above), it asks qwark for the window that
 * continues it (MBOX_EV_SCROLL_*); that comes as a new page, the cursor on the
 * same row of the page, never on an edge the page goes on past. L1 and R1 jump
 * a window up or down on such a page (PAGE_PREV, PAGE_NEXT), and do nothing on
 * one that fits. A shim needs nothing for it but a shim_cursor that answers
 * the widget's cursor every frame; every game's CONFIG look scrolls its window
 * natively.
 *
 * ------------------------------------------------- the game's planet picker
 *
 * (Mailbox version 3.) A shim that can open the game's own planet picker puts
 * MBOX_CAP_PICKER in MH_CAPS, which compiles the picker in (MH_PICKER), and
 * defines shim_picker. qwark then flags the root's Planets row MROWF_PICKER,
 * and the contract is:
 *
 *   - The cross on that row: the core marks the picker up (mh_picker_up())
 *     and calls shim_picker. The shim opens the picker over the menu's screen
 *     the game's way and answers 1, or answers 0 (a state the game would not
 *     open it in, a page it cannot open it from), when the core posts ACTIVATE
 *     as for any link and qwark opens its own Planets page instead. On 1 the
 *     core plays MH_SND_FORWARD, as for a link.
 *   - The menu's screen stopping while the picker is up (mh_core_stop, from
 *     the picker's push or swap) is not a close: hstate reads
 *     MBOX_HSTATE_PICKER, the page and the cursor are kept, and qwark leaves
 *     its stack alone. A shim whose picker is a page inside the menu's own
 *     screen need not stop it: hstate then stays open, which is no close
 *     either. While the picker is up the core takes no press.
 *   - Back from the picker to the menu's screen: mh_core_start sees the
 *     picker up, takes it down, and goes on as a start after a switch would
 *     (no opening: frames go on counting), the cursor back on the Planets row
 *     through mh_core_cursor(ST_LAST_CURSOR). A shim whose picker never
 *     stopped the screen calls mh_core_picker_back instead.
 *   - A pick: the shim's hook calls mh_core_pick(p, planet) with qwark's
 *     planet id, which posts it (pick_planet, then pick_seq) and counts as
 *     the menu closing (hstate CLOSED, the root next time), and then closes
 *     the game's menu its own way. qwark loads the planet once the game plays.
 *   - The picker going any other way (Start, Select, a screen of its own the
 *     shim does not follow, the combo): the shim calls mh_core_picker_gone(p),
 *     which is the same close without a pick. Nothing else ever takes the
 *     picker down, so a shim must see every way out of it.
 *   - The combo while the picker is up asks for a close (the request byte,
 *     2), as with the menu up. A picker that can serve it takes it with
 *     mh_core_close_request(); one that cannot (Deadlocked's Worlds, the
 *     game's own screen) leaves it, and qwark withdraws it half a second on.
 *   - qwark asks for no close for a pick: the game's menu is closing already.
 *     It loads pick_planet once hstate reads closed and the game plays again,
 *     or drops it five seconds on; a planet the game has not is dropped.
 *   - The mailbox: pick_seq at MBOX_OFF_PICK_SEQ (0x28) and pick_planet at
 *     MBOX_OFF_PICK_PLANET (0x2C), the last two words of the header qwark
 *     polls, so a pick costs qwark no read of its own; the core writes them.
 *   - Everything the shim changes in the game for the picker (a list that
 *     shows more, a gate passed, the pick hook) acts only while mh_picker_up()
 *     says the menu opened it; the game's own picker stays as it is.
 *   - (The pickers' round 2.) A picker that cannot show every planet (UYA's
 *     GALACTIC MAP has no vid comics) can hand the player qwark's page of the
 *     others: its game header sets MH_PICKER_OTHERS, and shim_picker answers
 *     MH_PICK_OTHERS instead of opening, after which the core posts
 *     MBOX_EV_PICKER_OTHERS on the row rather than ACTIVATE, and qwark opens
 *     that page (struct game_menu's picker_others). A game without it builds
 *     the core as before.
 */

#define MH_CASE_AS_IS       0
#define MH_CASE_UPPER       1
#define MH_CASE_TITLE       2      /* the first letter of every word */

#define MH_STYLE_DOTS       0x01   /* retired in round 4: an action is blank until it answers, in every game */
#define MH_STYLE_ARROWS     0x02   /* retired in round 3: a choice shows its bare value in every game */
#define MH_STYLE_LINKS      0x04   /* a link shows ">" as its value */
#define MH_STYLE_STAR       0x08   /* the current option of a MAIN list ends in " *" */
#define MH_STYLE_COLON      0x10   /* a CONFIG label with a value ends in ":" (no game since round 4) */

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
#define ST_SEEN_ROW         0x36   /* u8 the cursor's row the window's edge was last checked at, MH_NONE (round 4) */
#define ST_HOLD             0x37   /* u8 frames the other set stays on show by a screen that is going */
#define ST_PICKER           0x38   /* u8 mailbox version 3: 1 while the game's planet picker the menu opened is up */
/* 0x39..0x7B: round 3's description line, unused since round 4 (the sets keep their offset). */
#define ST_SET0             0x7C   /* the two text sets, MH_SET_SIZE each */

#define MH_NONE             0xFF

#define MH_TITLE_LEN        0x24   /* a title, terminated */
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
#define MH_BTN_L1           4      /* a window up, on a page that goes on above */
#define MH_BTN_R1           5      /* a window down, on a page that goes on below */
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

/* Version 3: the picker's part of the core is built only for a shim that has one. */
#if (MH_CAPS & MBOX_CAP_PICKER) != 0
#define MH_PICKER           1
#else
#define MH_PICKER           0
#endif

/* shim_picker's answers (version 3; MH_PICK_OTHERS since the pickers' round 2). */
#define MH_PICK_REFUSED     0      /* qwark's Planets page instead: the core posts ACTIVATE */
#define MH_PICK_OPENED      1      /* the game's picker is opening over the menu */
#define MH_PICK_OTHERS      2      /* not opened: qwark's page of the planets the picker cannot show */

/* The pickers' round 2: 1 in a game header whose shim answers MH_PICK_OTHERS (UYA). */
#ifndef MH_PICKER_OTHERS
#define MH_PICKER_OTHERS    0
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
 * or shim_switch for the other widget), keeps the title, and asks for the
 * next window when the cursor is at the window's edge (round 4). Answers
 * MH_GONE when the shim must do nothing more this frame. While a
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

#if MH_PICKER
/*
 * Version 3, the planet picker (above). mh_core_pick: the player picked
 * `planet`, qwark's planet id, in the picker the menu opened; posted to qwark,
 * and the menu counts as closed. The shim then closes the game's menu. Only
 * while mh_picker_up(); otherwise nothing.
 */
MH_API void mh_core_pick(int p, s32 planet);

/* The picker the menu opened went some other way than back to the menu: a close. */
MH_API void mh_core_picker_gone(int p);

#endif

/* Whether the game's planet picker that is up is the one the menu opened. */
#define mh_picker_up()      (r8(MH_STATE + ST_PICKER) != 0)

/*
 * Back from the picker for a shim whose picker never stopped the menu's
 * screen (a page inside the menu's own screen, so no mh_core_stop and no
 * mh_core_start around it): the picker down, the menu open as it was. A shim
 * whose picker stops the screen comes back through mh_core_start instead.
 * (Macros, so a game that does not use them carries no code and no
 * descriptor for them.)
 */
#define mh_core_picker_back(p) 	((void)(p), w8(MH_STATE + ST_PICKER, 0), w8(MH_BOX + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN))

/*
 * For a picker that serves the menu combo's close while it is up: 1, and the
 * request taken, when qwark asked the menu to close (the combo with the
 * picker up). The picker then closes the game's menu and calls
 * mh_core_picker_gone.
 */
#define mh_core_close_request() 	(r8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE ? (w8(MH_BOX + MBOX_OFF_REQUEST, MBOX_REQ_NONE), 1u) : 0u)

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

/* mh_title() changed. (Round 4: shim_desc is gone with the description line.) */
MH_API void shim_title(int p);

/* The row the game's cursor is on. */
MH_API u32  shim_cursor(int p);

/* One of the game's sounds, MH_SND_*; nothing for one the game plays itself. */
MH_API void shim_sound(u32 snd);

#if MH_PICKER
/*
 * Version 3: open the game's own planet picker over the menu's screen, from
 * the cross on the Planets row (above). MH_PICK_OPENED (1) when it did;
 * MH_PICK_REFUSED (0) to fall back to qwark's Planets page; MH_PICK_OTHERS
 * (2, only with MH_PICKER_OTHERS) for qwark's page of the planets the picker
 * cannot show.
 */
MH_API u32  shim_picker(int p);
#endif

#endif /* __ASSEMBLER__ */

#endif /* QWARK_MH_SHIM_H */
