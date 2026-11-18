/*
 * Revision 1.34, build 59: the page combos (proto.h COMBO_PAGE_*), one per
 * page of the Trainer's root, on qwark's fake console against the real core,
 * in the four games whose Trainer is a renderer of qwark's (Tools of
 * Destruction's own pause menu is test_tod_menu.c's). The renderer is played
 * here as the other menu tests play it: its hstate, its events, its request
 * byte left untaken.
 *
 *   every page    each combo asks the renderer to open with its page already
 *                 in the mailbox (not the root, not ROOT-flagged); Back is the
 *                 root with the cursor on that page's row;
 *   up already    a press goes to the page at once and asks for nothing;
 *   no taker      an open nobody took puts the root back for the Help row;
 *   no page       a game whose root has no such row: nothing asked, the
 *                 console told once a session;
 *   the rules     OPEN_MENU's: the arming rule, the switch, the capture's
 *                 hold, a load the menu closed for still waiting, RPCS3.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/core/menubox.h"
#include "../src/games/game.h"
#include "../src/games/menuhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

/* Select + R3 and the d-pad: no other combo of the suite's. */
static const u32 g_masks[COMBO_PAGES] = {
	PAD_SELECT | PAD_R3 | PAD_UP,
	PAD_SELECT | PAD_R3 | PAD_RIGHT,
	PAD_SELECT | PAD_R3 | PAD_DOWN,
	PAD_SELECT | PAD_R3 | PAD_LEFT,
	PAD_SELECT | PAD_R3 | PAD_UP | PAD_RIGHT,
	PAD_SELECT | PAD_R3 | PAD_DOWN | PAD_LEFT,
	PAD_SELECT | PAD_R3 | PAD_UP | PAD_DOWN
};

/* The page each combo opens, as the root's row and the page's title say it. */
static const char * const g_want[COMBO_PAGES] = {
	"Game", "Cheats", "Planets", "Manips", "Unlocks", "Mods", "Save files"
};

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot(const char *title)
{
	int i;

	host_boot(title);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME) return 1;
	}
	return 0;
}

static int quit(void)
{
	int i;

	host_quit();
	for (i = 0; i < 2000; i++) {
		tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

static u32 peek32(u32 a) { u8 b[4]; host_peek(a, b, 4); return be32_get(b); }
static u8 peek8(u32 a) { u8 b = 0; host_peek(a, &b, 1); return b; }
static void poke32(u32 a, u32 v) { u8 b[4]; be32_put(b, v); host_poke(a, b, 4); }
static void poke8(u32 a, u8 v) { host_poke(a, &v, 1); }

static void press(u32 mask)
{
	host_set_pad(0);
	tick(2);
	host_set_pad(mask);
	tick(2);
	host_set_pad(0);
	tick(2);
}

/* The page in the mailbox. */
struct pbox {
	u32 key;
	u8 n, cursor, pflags, ptype;
	char title[MBOX_TITLE_LEN + 1];
	u16 ids[32];
	char labels[32][MROW_LABEL_LEN + 1];
};

static void read_box(struct pbox *p)
{
	u32 mb = menu_mailbox();
	u8 buf[MBOX_OFF_ROWS + 32 * 0x40];
	u8 i;

	memset(p, 0, sizeof(*p));
	host_peek(mb, buf, sizeof(buf));
	p->key = be32_get(buf + MBOX_OFF_PAGE_KEY);
	p->n = buf[MBOX_OFF_NROWS];
	p->cursor = buf[MBOX_OFF_CURSOR];
	p->pflags = buf[MBOX_OFF_PFLAGS];
	p->ptype = buf[MBOX_OFF_PTYPE];
	memcpy(p->title, buf + MBOX_OFF_TITLE, MBOX_TITLE_LEN);
	for (i = 0; i < p->n && i < 32; i++) {
		const u8 *r = buf + MBOX_OFF_ROWS + i * 0x40;
		p->ids[i] = be16_get(r + MROW_OFF_ID);
		memcpy(p->labels[i], r + MROW_OFF_LABEL, MROW_LABEL_LEN);
	}
}

static int box_row(const struct pbox *p, const char *label)
{
	u8 i;
	for (i = 0; i < p->n; i++) {
		if (strcmp(p->labels[i], label) == 0) return i;
	}
	return -1;
}

/* What a renderer does to post an event: the record, then ev_seq; then qwark's ticks. */
static void box_event(u8 kind, int row)
{
	u32 mb = menu_mailbox();
	struct pbox p;
	u8 rec[8];

	read_box(&p);
	rec[0] = kind;
	rec[1] = (u8)(row < 0 ? 0 : row);
	be16_put(rec + 2, row >= 0 && row < p.n ? p.ids[row] : 0);
	be32_put(rec + 4, p.key);
	host_poke(mb + MBOX_OFF_EV_KIND, rec, 8);
	poke32(mb + MBOX_OFF_EV_SEQ, peek32(mb + MBOX_OFF_EV_SEQ) + 1);
	tick(2);
}

static void hstate(u8 h)
{
	poke8(menu_mailbox() + MBOX_OFF_HSTATE, h);
	tick(4);
}

static u8 request(void)
{
	return peek8(menu_mailbox() + MBOX_OFF_REQUEST);
}

static void request_clear(void)
{
	poke8(menu_mailbox() + MBOX_OFF_REQUEST, MBOX_REQ_NONE);
	tick(2);
}

static void bind_all(int on)
{
	u8 p;
	for (p = 0; p < COMBO_PAGES; p++) config_set_combo((u8)(COMBO_PAGE_FIRST + p), on ? g_masks[p] : 0);
	tick(2);
}

/* The game in ordinary play, by its renderer's own words: the mode 0 and no mode change queued. */
static void playing(const struct mh_desc *d)
{
	poke32(d->mode_addr, 0);
	poke32(d->pending_addr, d->pending_none);
}

static int installed(const struct mh_desc *d)
{
	int i;

	playing(d);
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	return menu_state() == MENU_READY;
}

static void pages_in(const char *title, const char *name, u8 game_id)
{
	const struct mh_desc *d = mh_desc_for_game(game_id);
	struct pbox root, p;
	char what[200];
	u8 k;
	int at;

	snprintf(what, sizeof(what), "%s boots and its renderer goes in", name);
	check(d != NULL && boot(title) && installed(d), what);
	if (d == NULL || menu_state() != MENU_READY) {
		quit();
		return;
	}
	read_box(&root);
	snprintf(what, sizeof(what), "%s: the root in the mailbox, the menu down, nothing asked", name);
	check(strcmp(root.title, "Trainer") == 0 && (root.pflags & MBOX_PAGE_ROOT) != 0 && request() == MBOX_REQ_NONE, what);

	for (k = 0; k < COMBO_PAGES; k++) {
		at = box_row(&root, g_want[k]);
		snprintf(what, sizeof(what), "%s: the root has a %s row", name, g_want[k]);
		check(at >= 0, what);
		if (at < 0) continue;

		press(g_masks[k]);
		read_box(&p);
		snprintf(what, sizeof(what), "%s: the %s combo asks the renderer to open, the %s page already in the mailbox",
		         name, g_want[k], g_want[k]);
		check(request() == MBOX_REQ_OPEN && strcmp(p.title, g_want[k]) == 0 && (p.pflags & MBOX_PAGE_ROOT) == 0 &&
		      menu_model_depth() == 2, what);
		request_clear();
		hstate(MBOX_HSTATE_OPEN);
		box_event(MBOX_EV_BACK, 0);
		read_box(&p);
		snprintf(what, sizeof(what), "%s: up on %s, Back is the root, the cursor on its row", name, g_want[k]);
		check(strcmp(p.title, "Trainer") == 0 && p.cursor == (u8)at && menu_model_depth() == 1, what);
		hstate(MBOX_HSTATE_CLOSED);
	}

	/* Up already: straight to the page, nothing asked; another combo moves on from there. */
	hstate(MBOX_HSTATE_OPEN);
	press(g_masks[COMBO_PAGE_PLANETS - COMBO_PAGE_FIRST]);
	read_box(&p);
	snprintf(what, sizeof(what), "%s: with the menu up, Planets' combo shows Planets at once and asks for nothing", name);
	check(strcmp(p.title, "Planets") == 0 && request() == MBOX_REQ_NONE, what);
	press(g_masks[COMBO_PAGE_CHEATS - COMBO_PAGE_FIRST]);
	read_box(&p);
	at = box_row(&root, "Cheats");
	snprintf(what, sizeof(what), "%s: then Cheats' combo: Cheats, one page over the root", name);
	check(strcmp(p.title, "Cheats") == 0 && menu_model_depth() == 2 && request() == MBOX_REQ_NONE, what);
	box_event(MBOX_EV_BACK, 0);
	read_box(&p);
	snprintf(what, sizeof(what), "%s: and Back the root, the cursor on Cheats, not on Planets", name);
	check(strcmp(p.title, "Trainer") == 0 && p.cursor == (u8)at, what);
	hstate(MBOX_HSTATE_CLOSED);

	/* An open nobody took (a load, a cutscene): withdrawn, and the root back for the Help row. */
	press(g_masks[COMBO_PAGE_UNLOCKS - COMBO_PAGE_FIRST]);
	read_box(&p);
	check(request() == MBOX_REQ_OPEN && strcmp(p.title, "Unlocks") == 0, "Unlocks' combo asks to open on Unlocks");
	tick(70);
	read_box(&p);
	snprintf(what, sizeof(what), "%s: half a second untaken, the request is withdrawn and the root posted again", name);
	check(request() == MBOX_REQ_NONE && strcmp(p.title, "Trainer") == 0 && menu_model_depth() == 1, what);

	check(quit(), "quit");
}

/* The tests' own table: a root with Game alone of the game's sections, so it has no Cheats page. */
static const u8 g_one_items[] = { MENU_ITEM_SLOT, MENU_ITEM_POS_SAVE, MENU_ITEM_POS_LOAD };
static const struct menu_section g_one_sections[] = { { "Game", g_one_items, (u8)sizeof(g_one_items) } };
static struct game_menu g_one_menu;

/* Deadlocked's set-aside load flag (rac4.h), which a load the menu closed for writes once the game plays. */
#define DL_LOAD_ASIDE 0x015CD71Eu

static void rules_dl(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC4);
	const u32 game = g_masks[COMBO_PAGE_GAME - COMBO_PAGE_FIRST];
	const u32 cheats = g_masks[COMBO_PAGE_CHEATS - COMBO_PAGE_FIRST];
	struct pbox p;
	u32 n0;
	int at;

	group("page combos: OPEN_MENU's rules, in Deadlocked");
	check(boot("NPEA00423") && installed(d), "Deadlocked boots and its renderer goes in");

	/* The arming rule: a press that runs into another without the pad going empty fires once. */
	host_set_pad(0);
	tick(2);
	host_set_pad(game);
	tick(2);
	host_set_pad(cheats);
	tick(4);
	read_box(&p);
	check(strcmp(p.title, "Game") == 0, "Game's combo held on into Cheats' buttons: Game alone, Cheats not armed");
	host_set_pad(0);
	tick(70);
	press(cheats);
	read_box(&p);
	check(strcmp(p.title, "Cheats") == 0 && request() == MBOX_REQ_OPEN, "the pad empty again, Cheats' combo fires");
	tick(70);

	/* The switch over every combo, and the capture's hold. */
	config_set_combo_enabled(0);
	tick(2);
	press(cheats);
	read_box(&p);
	check(request() == MBOX_REQ_NONE && strcmp(p.title, "Trainer") == 0, "with every combo switched off: nothing");
	config_set_combo_enabled(1);
	tick(2);
	session_combo_suspend(1);
	press(cheats);
	read_box(&p);
	check(request() == MBOX_REQ_NONE && strcmp(p.title, "Trainer") == 0, "under a capture's hold: nothing");
	session_combo_suspend(0);
	tick(2);
	press(cheats);
	check(request() == MBOX_REQ_OPEN, "the hold lifted: it fires again");
	tick(70);

	/* A load the menu closed for, still waiting: the press changes nothing. */
	hstate(MBOX_HSTATE_OPEN);
	read_box(&p);
	box_event(MBOX_EV_ACTIVATE, box_row(&p, "Game"));
	read_box(&p);
	box_event(MBOX_EV_ACTIVATE, box_row(&p, "Load set-aside file"));
	check(request() == MBOX_REQ_CLOSE, "Load set-aside file: the menu asked to close first");
	press(cheats);
	read_box(&p);
	check(request() == MBOX_REQ_CLOSE && strcmp(p.title, "Game") == 0,
	      "Cheats' combo while that load waits: the close still asked for, Game still the page");
	request_clear();
	hstate(MBOX_HSTATE_CLOSED);
	playing(d);
	tick(8);
	check(peek8(DL_LOAD_ASIDE) == 1, "and the load goes out once the game plays, as it would have");
	poke8(DL_LOAD_ASIDE, 0);
	tick(40);

	/* A page the game's root has not: nothing asked, said once a session. */
	memset(&g_one_menu, 0, sizeof(g_one_menu));
	g_one_menu.sections = g_one_sections;
	g_one_menu.nsections = 1;
	menu_test_menu(&g_one_menu);
	tick(4);
	n0 = host_notify_count();
	press(cheats);
	read_box(&p);
	check(request() == MBOX_REQ_NONE && strcmp(p.title, "Trainer") == 0 &&
	      host_notify_count() == n0 + 1 && strcmp(host_notify_last(), "Trainer: no Cheats page in this game") == 0,
	      "a root with no Cheats: Cheats' combo asks for nothing, and the console says so");
	press(cheats);
	check(request() == MBOX_REQ_NONE && host_notify_count() == n0 + 1, "a second press: still nothing, said once");
	at = box_row(&p, "Game");
	press(game);
	read_box(&p);
	check(at >= 0 && request() == MBOX_REQ_OPEN && strcmp(p.title, "Game") == 0, "while Game's still opens its page");
	tick(70);
	menu_test_menu(NULL);
	tick(4);
	check(quit(), "quit");

	/* Under RPCS3 there is no menu in these games: the page combos say what OPEN_MENU says, once. */
	group("page combos: under RPCS3");
	host_set_can_patch_code(0);
	check(boot("NPEA00423"), "Deadlocked boots under RPCS3");
	tick(400);
	n0 = host_notify_count();
	press(cheats);
	check(menu_state() == MENU_OFF && host_notify_count() == n0 + 1 &&
	      strcmp(host_notify_last(), "Trainer menu: not available under RPCS3") == 0,
	      "Cheats' combo: OPEN_MENU's own refusal on the console");
	press(COMBO_MENU_DEFAULT);
	press(game);
	check(host_notify_count() == n0 + 1, "and then the menu combo and Game's say nothing more: one reason, said once a session");
	check(quit(), "quit");
	host_set_can_patch_code(1);
}

void test_menu_pages(void)
{
	u8 p;
	int none = 1;

	group("page combos: the actions");
	for (p = 0; p < COMBO_PAGES; p++) {
		if (config_combo((u8)(COMBO_PAGE_FIRST + p)) != 0) none = 0;
		if (menu_page_name(p) == NULL || strcmp(menu_page_name(p), g_want[p]) != 0) none = 0;
	}
	check(none && menu_page_name(COMBO_PAGES) == NULL,
	      "seven page combos after the seven of before, Game to Save files, none bound out of the box");
	check(config_set_combo(COMBO_COUNT, 1) == ST_BAD_ARG, "an action past them is BAD_ARG");
	bind_all(1);

	group("page combos: Ratchet & Clank");
	pages_in("NPEA00385", "RaC1", GAME_RAC1);
	group("page combos: Going Commando");
	pages_in("NPEA00386", "RaC2", GAME_RAC2);
	group("page combos: Up Your Arsenal");
	pages_in("NPEA00387", "UYA", GAME_RAC3);
	group("page combos: Deadlocked");
	pages_in("NPEA00423", "Deadlocked", GAME_RAC4);
	rules_dl();

	bind_all(0);
}
