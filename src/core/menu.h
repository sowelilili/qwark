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

/* Rows a renderer may ask for on one screen; a page longer than that is paged. */
#define MENU_VIEW_MAX     16

/* How deep the pages go: the root, a section, a list under it, and room for later. */
#define MENU_NAV_DEPTH    8

/* The most rows one page lists before it is cut into windows. */
#define MENU_ITEMS_MAX    64

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
#define MENU_PAGE_ID(kind, arg) ((u16)(((u16)(kind) << 8) | (u8)(arg)))

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
#define MENU_ROW_ID(tag, arg) ((u16)(((u16)(tag) << 8) | (u8)(arg)))

#define MENU_NAV_MORE     1
#define MENU_NAV_PREV     2
#define MENU_NAV_BACK     3

#define MENU_MANIP_CATEGORY 0
#define MENU_MANIP_AUTO     1
#define MENU_MANIP_REMEMBER 2
#define MENU_MANIP_APPLY    3

/*
 * One row, as the model builds it and the mailbox carries it. The description
 * reaches only a renderer whose rows have room for it (MBOX_ROW_SIZE_DESC).
 */
struct menu_row {
	u16  id;
	u8   kind;                          /* MROW_* */
	u8   flags;                         /* MROWF_* */
	char label[MROW_LABEL_LEN];
	char value[MROW_VALUE_LEN];
	char desc[MROW_DESC_LEN];
};

/* What a renderer is shown at once: one window of one page. */
struct menu_view {
	u32  key;                           /* page id << 16 | window start << 8 | visit */
	u8   nrows;
	u8   cursor;                        /* where the cursor goes on a new key */
	u8   pflags;                        /* MBOX_PAGE_* */
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
