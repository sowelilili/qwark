/*
 * The console menu (revision 1.22, build 54): a menu on the TV that gives a
 * console with no PC the features the client offers. PROTOCOL.md section 9 has
 * the rules; rewrite/SPEC-console-ui.md the design.
 *
 * qwark owns it. This file holds two halves:
 *
 *   the model     renderer-agnostic pages of rows, built on demand from what
 *                 qwark already has (the running game's DESCRIBE table and its
 *                 menu table, its toggle state and auto flags, its readouts,
 *                 positions, planets, manips, unlocks and the console's
 *                 savefile library), a navigation
 *                 stack of page ids, and what each row does when the player
 *                 acts on it, through the same functions the protocol ops use
 *                 (features_set, features_trigger), so config, auto flags and
 *                 the previous-session record behave exactly as when a client
 *                 does it;
 *   the driver    the game's renderer: installing it (src/games/menuhelper_bins.c)
 *                 a few seconds into a session, which puts the menu behind the
 *                 game's own Help row and the OPEN_MENU combo, polling the
 *                 mailbox it shares with qwark
 *                 (menubox.h), posting pages into it and applying the events it
 *                 posts back.
 *
 * Tick thread only, under the core lock, like everything that touches game
 * memory: session.c calls menu_tick from its INGAME pass and menu_combo from
 * the combos.
 */
#ifndef QWARK_MENU_H
#define QWARK_MENU_H

#include "proto.h"
#include "menubox.h"
#include "../games/game.h"

/* ------------------------------------------------------------- the model */

/*
 * Rows a renderer may ask for at once (its mailbox's row_cap): RaC2's 32 are
 * the most (round 4). A page longer than a renderer takes is posted a window
 * at a time, and the window slides under the cursor (menu.c).
 */
#define MENU_VIEW_MAX     32

/* How deep the pages go: the root, a section, a list under it, and room for later. */
#define MENU_NAV_DEPTH    8

/* The most rows one page lists (round 4: 128, a save category of the console's library being any length). */
#define MENU_ITEMS_MAX    128

/*
 * Page ids: a kind in the high byte, its argument in the low byte. Round 2
 * (rewrite/console-ui-framework.md): the root, the game's sections, planets,
 * manips, unlocks and save files. A game with no menu table (game.h, struct
 * game_menu) gets round 1's page per DESCRIBE group instead of sections.
 */
#define MENU_PAGE_ROOT       0x00   /* MAIN */
#define MENU_PAGE_GROUP      0x01   /* CONFIG, arg: the DESCRIBE group */
#define MENU_PAGE_CHOICE     0x02   /* MAIN, arg: the ENUM feature */
#define MENU_PAGE_SECTION    0x03   /* CONFIG, arg: the game's section */
#define MENU_PAGE_PLANETS    0x04   /* MAIN */
#define MENU_PAGE_MANIPS     0x05   /* CONFIG */
#define MENU_PAGE_UNLOCKS    0x06   /* MAIN */
#define MENU_PAGE_UNLOCK_CAT 0x07   /* CONFIG, arg: the unlock category */
#define MENU_PAGE_SAVES      0x08   /* MAIN */
#define MENU_PAGE_SAVE_CAT   0x09   /* MAIN, arg: the category's place in the library */
#define MENU_PAGE_UNLOCK_ACTIONS 0x0A /* CONFIG: the Unlocks page's buttons, where the game names them a page (round 3) */
#define MENU_PAGE_ID(kind, arg) ((u16)(((u16)(kind) << 8) | (u8)(arg)))
/* MENU_PAGE_PLANETS' arg (the pickers' round 2): the planets the game's picker cannot show, struct game_menu's picker_others. */
#define MENU_PLANETS_OTHERS  0x01

/*
 * Row ids, which a renderer echoes and never reads: a tag in the high byte, an
 * argument in the low byte. The tag is what menu_event dispatches on.
 */
#define MENU_ROW_GROUP    0x01      /* arg: the group a LINK opens */
#define MENU_ROW_FEATURE  0x02      /* arg: the feature id */
#define MENU_ROW_OPTION   0x03      /* arg: the option's index in its ENUM */
#define MENU_ROW_NAV      0x04      /* arg: MENU_NAV_* */
#define MENU_ROW_TEXT     0x05      /* arg: which line of text */
#define MENU_ROW_PAGE     0x06      /* arg: the page kind a LINK opens (argument 0) */
#define MENU_ROW_SECTION  0x07      /* arg: the section a LINK opens */
#define MENU_ROW_POS      0x08      /* arg: MENU_ITEM_SLOT .. POS_LOAD, low byte */
#define MENU_ROW_PLANET   0x09      /* arg: the planet */
#define MENU_ROW_MANIP    0x0A      /* arg: MENU_MANIP_* */
#define MENU_ROW_UCAT     0x0B      /* arg: the unlock category a LINK opens */
#define MENU_ROW_UNLOCK   0x0C      /* arg: the unlock's id */
#define MENU_ROW_SCAT     0x0D      /* arg: the save category a LINK opens */
#define MENU_ROW_SAVE     0x0E      /* arg: the save's place in its category */
#define MENU_ROW_STATUS   0x0F      /* arg: the ACTION whose status readout it shows */
#define MENU_ROW_VALUE    0x10      /* round 5, arg: the game's menu value (struct menu_value) */
#define MENU_ROW_ID(tag, arg) ((u16)(((u16)(tag) << 8) | (u8)(arg)))

/* MENU_ROW_TEXT's lines. */
#define MENU_TEXT_BUILD   0         /* "qwark build 54", grey, the root's last row */
#define MENU_TEXT_EMPTY   1         /* "Nothing here", a page with no rows */
#define MENU_TEXT_SPACER  2         /* round 5: blank, between a page's parts; the cursor passes over it */

/*
 * Round 5: the root's grey "qwark build 54" row, which the user's testing
 * reads; the release builds it out (0), as rewrite/SPEC-console-ui.md section
 * 10 lists.
 */
#ifndef MENU_BUILD_ROW
#define MENU_BUILD_ROW    1
#endif

#define MENU_NAV_MORE     1         /* retired in round 4, with the More row */
#define MENU_NAV_PREV     2         /* retired in round 4 */
#define MENU_NAV_BACK     3

/*
 * The Manips page's own rows. Round 5: Auto-reset is Off, On or On boot, which
 * is MANIP_OPTIONS' auto-reset and remember together (menu.c, manip_reset),
 * and Remember (2) has no row of its own any more.
 */
#define MENU_MANIP_CATEGORY 0
#define MENU_MANIP_AUTO     1
#define MENU_MANIP_REMEMBER 2       /* retired in round 5 */
#define MENU_MANIP_APPLY    3

/*
 * One row, as the model builds it and the mailbox carries it. (Round 4: no
 * description; no renderer showed one, and the client has its own.)
 */
struct menu_row {
	u16  id;
	u8   kind;                          /* MROW_* */
	u8   flags;                         /* MROWF_* */
	char label[MROW_LABEL_LEN];
	char value[MROW_VALUE_LEN];
	/*
	 * Round 3, a VALUE row flagged MROWF_RANGE (its label then 23 characters
	 * at most): the value, the lowest, the highest and the step, which the
	 * mailbox carries in the label's last 16 bytes (menubox.h).
	 */
	u32  range[4];
};

/*
 * A VALUE row whose range spans this many steps of 1 or fewer is flagged
 * MROWF_SLIDER as well: few enough for a bar to mean something (Challenge
 * mode's 0..255, not Bolts').
 */
#define MENU_SLIDER_SPAN  1000

/*
 * Round 4: a slider's Left and Right move it a tenth of its range, rounded
 * up, so ten presses go from one end to the other (Challenge mode's 0..255:
 * 26); the cross switches to a fine step of 1 and back.
 */
#define MENU_SLIDER_STEPS 10

/* What a renderer is shown at once: one window of one page. */
struct menu_view {
	u32  key;                           /* page id << 16 | window start << 8 | visit */
	u8   nrows;
	u8   cursor;                        /* where the cursor goes on a new key */
	u8   pflags;                        /* MBOX_PAGE_*: the root, and whether the page goes on above or below */
	u8   ptype;                         /* MBOX_PTYPE_* */
	char title[MBOX_TITLE_LEN];
	char hint[MBOX_HINT_LEN];           /* the last row's answer, as a sentence, for two seconds */
	struct menu_row rows[MENU_VIEW_MAX];
};

/*
 * Starts the model over for the running game: the root page, no steps, nothing
 * remembered. `max_rows` is what the renderer takes on a MAIN page and
 * `cfg_rows` on a CONFIG page, 0 for a renderer that has no CONFIG page: both
 * held to 3..MENU_VIEW_MAX. `caps` is the renderer's MBOX_CAP_* bits.
 */
void menu_model_reset(u8 max_rows, u8 cfg_rows, u8 caps);

/* The window of the current page the renderer is shown, built now. */
void menu_model_view(struct menu_view *out);

/*
 * Applies one event to the view last built, as the mailbox carries it: the
 * kind, the row's index and its id, and the key of the view it was made on. An
 * event for a view that is no longer current, or a row that is not there, is
 * refused with ST_BAD_ARG and changes nothing. Otherwise the answer is what
 * the row's action answered (ST_OK, or a refusal, which the row then shows for
 * a moment).
 */
int  menu_model_event(u8 kind, u8 row, u16 id, u32 key);

/* The current page id and the stack's depth, for the tests. */
u16  menu_model_page(void);
u8   menu_model_depth(void);

/*
 * Round 3: back to the root, the cursor on its first row, as the menu closes
 * (the driver calls it when the renderer says so), so the next open, from the
 * game's row or the combo, starts there. The values' steps and a load waiting
 * for the close stay.
 */
void menu_model_home(void);

#ifdef QWARK_TEST
/* The tests' own menu table in place of the running game's; NULL for the game's. */
void menu_test_menu(const struct game_menu *gm);
#endif

/* -------------------------------------------------------------- the driver */

/*
 * Where the driver is in a session. The renderer goes into the game a few
 * seconds into the session, once the game is in ordinary play (round 2: the
 * pause menu's Help row is a way in, and has to be there before the pause),
 * or sooner on a press of the menu combo.
 */
#define MENU_OFF         0     /* no renderer here: not a console, not a game that has one */
#define MENU_IDLE        1     /* nothing in the game yet; it goes in when due and in play */
#define MENU_READY       2     /* installed, and serving the mailbox */
#define MENU_REFUSED     3     /* a hook site held a word not the game's or ours; tried again later */
#define MENU_REMOVING    4     /* menu.enabled went to 0: the hooks come out once it is safe */

/* The session reached INGAME with `game` (NULL for a title qwark does not know). */
void menu_enter(const struct game_api *game);

/* The session is leaving the game: everything about this process goes. */
void menu_forget(void);

/* Every INGAME tick, after the game's own watchers: the install when due, then the mailbox. */
void menu_tick(void);

/*
 * The OPEN_MENU combo: open the menu, or close it if it is up. A press before
 * the renderer is in installs it first, in the same tick.
 */
void menu_combo(void);

int  menu_state(void);
/* The delay before a session's install, three seconds; the tests shorten it. */
u64  menu_install_delay_us(void);
void menu_set_install_delay_us(u64 us);
/* The mailbox's game address while a renderer is in and served, 0 otherwise. */
u32  menu_mailbox(void);

#endif /* QWARK_MENU_H */
