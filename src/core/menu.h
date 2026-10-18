/*
 * The console menu (revision 1.22, build 54): a menu on the TV that gives a
 * console with no PC the features the client offers. PROTOCOL.md section 9 has
 * the rules; rewrite/SPEC-console-ui.md the design.
 *
 * qwark owns it. This file holds two halves:
 *
 *   the model     renderer-agnostic pages of rows, built on demand from what
 *                 qwark already has (the running game's DESCRIBE table, its
 *                 toggle state, its readouts, its ENUM options), a navigation
 *                 stack of page ids, and what each row does when the player
 *                 acts on it, through the same functions the protocol ops use
 *                 (features_set, features_trigger), so config, auto flags and
 *                 the previous-session record behave exactly as when a client
 *                 does it;
 *   the driver    the game's renderer: installing it (src/games/menuhelper_bins.c)
 *                 on the first OPEN_MENU combo of a session, which is the menu's
 *                 only way in, polling the mailbox it shares with qwark
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

/* How deep the pages go: the root, a group, a pick-one list, and room for later. */
#define MENU_NAV_DEPTH    8

/*
 * Page ids: a kind in the high byte, its argument in the low byte. Round 1 has
 * the root, one page per DESCRIBE group and one pick-one list per ENUM; the
 * planets, positions, manips and save files of a later round are more kinds.
 */
#define MENU_PAGE_ROOT    0x00
#define MENU_PAGE_GROUP   0x01      /* arg: the DESCRIBE group */
#define MENU_PAGE_CHOICE  0x02      /* arg: the ENUM feature */
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
#define MENU_ROW_ID(tag, arg) ((u16)(((u16)(tag) << 8) | (u8)(arg)))

#define MENU_NAV_MORE     1
#define MENU_NAV_PREV     2
#define MENU_NAV_BACK     3

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
	u8   pad;
	char title[MBOX_TITLE_LEN];
	char hint[MBOX_HINT_LEN];           /* the last row's answer, as a sentence, for two seconds */
	struct menu_row rows[MENU_VIEW_MAX];
};

/*
 * Starts the model over for the running game: the root page, no steps, nothing
 * remembered. `max_rows` and `caps` are the renderer's (MBOX_CAP_*); max_rows
 * is held to 3..MENU_VIEW_MAX.
 */
void menu_model_reset(u8 max_rows, u8 caps);

/* The window of the current page the renderer is shown, built now. */
void menu_model_view(struct menu_view *out);

/*
 * Applies one event to the view last built, as the mailbox carries it: the
 * kind, the row's index and its id, and the key of the view it was made on. An
 * event for a view that is no longer current, or a row that is not there, is
 * refused with ST_BAD_ARG and changes nothing. Otherwise the answer is what
 * the row's action answered (ST_OK, or features_set's refusal, which the row
 * then shows for a moment).
 */
int  menu_model_event(u8 kind, u8 row, u16 id, u32 key);

/* The current page id and the stack's depth, for the tests. */
u16  menu_model_page(void);
u8   menu_model_depth(void);

/* -------------------------------------------------------------- the driver */

/*
 * Where the driver is in a session. The renderer goes into the game on the
 * first combo press, not before: the combo is the menu's only way in, so qwark
 * sees every open coming, and a session in which nobody opens the menu has
 * none of it in the game.
 */
#define MENU_OFF         0     /* no renderer here: not a console, not a game that has one */
#define MENU_IDLE        1     /* nothing in the game; the next combo press installs it */
#define MENU_READY       2     /* installed, and serving the mailbox */
#define MENU_REFUSED     3     /* the last press found a hook word not the game's or ours */
#define MENU_REMOVING    4     /* menu.enabled went to 0: the hooks come out once it is safe */

/* The session reached INGAME with `game` (NULL for a title qwark does not know). */
void menu_enter(const struct game_api *game);

/* The session is leaving the game: everything about this process goes. */
void menu_forget(void);

/* Every INGAME tick, after the game's own watchers. Nothing to do until the combo. */
void menu_tick(void);

/*
 * The OPEN_MENU combo: open the menu, or close it if it is up. The first press
 * of a session installs the renderer first, in the same tick.
 */
void menu_combo(void);

int  menu_state(void);
/* The mailbox's game address while a renderer is in and served, 0 otherwise. */
u32  menu_mailbox(void);

#endif /* QWARK_MENU_H */
