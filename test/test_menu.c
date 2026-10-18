/*
 * The console menu (revision 1.22), on qwark's fake console.
 *
 * Three layers, each against the real core:
 *
 *   the model     menu_model_view / menu_model_event for Deadlocked and UYA, with
 *                 renderers of every shape: pages, windows, every event kind,
 *                 stale events, and every row doing what the protocol op does;
 *   the mailbox   the driver against a renderer simulated at the byte level: the
 *                 install and its refusals, the page seqlock, events and acks, the
 *                 open and close requests and the combo, menu.enabled, taking a
 *                 renderer over, a CLOSES action, RPCS3 and a game with none;
 *   the renderer  Deadlocked's renderer itself, src/games/menuhelper/mh_rac4.c,
 *                 compiled for the PC (MH_HOST) and run on the same fake memory
 *                 qwark writes, against mocks of the game's menu functions, the
 *                 way the probes' host tests stepped N0.
 *
 * The renderer's hook stubs are assembler and do not run here;
 * src/games/menuhelper/check.py steps those against a model of the game.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/mem.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/games/rac4.h"
#include "../src/games/menuhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- the renderer */

#define MH_HOST 1

u8 *mh_host_ptr(u32 addr)
{
	return host_mem_ptr(addr);
}

/*
 * The game's menu, as far as the renderer can see it: one player, a screen
 * stack, the list GuiSupport shows and what the renderer asked of it.
 */
static struct {
	int screen;              /* GuiMenu screen: 0 none, 6 Options, 36 the menu */
	int stack[10];
	int depth;
	int shown;
	int cursor;
	int nbuttons;
	u32 buttons;
	u32 title;
	u32 close_cb;
	int sound;
	int selects;
	int widgets;
	int stops;
	int input_ok;
} G;

static int mock_get_canvas(void) { return 0; }
static int mock_get_cursor(int p) { (void)p; return G.cursor; }
static int mock_get_screen(int p) { (void)p; return G.screen; }
static void mock_set_cursor(int p, int i) { (void)p; G.cursor = i; }
static void mock_set_title(int p, u32 s) { (void)p; G.title = s; }
static int mock_screen_mode(int p) { (void)p; return 0; }
static void mock_set_target(int p, float x, float y, float w, float h)
{
	(void)p; (void)x; (void)y; (void)w; (void)h;
}
static void mock_set_header(int p, int on) { (void)p; (void)on; }
static void mock_set_buttons(int p, int n, u32 rows) { (void)p; G.nbuttons = n; G.buttons = rows; }
static void mock_show_menu(int p, u32 cb) { (void)p; G.shown = 1; G.close_cb = cb; }
static void mock_select_row(void) { G.selects++; }
static void mock_update_widgets(int p) { (void)p; G.widgets++; }
static int mock_takes_input(int p, int kind) { (void)p; (void)kind; return G.input_ok; }
static void mock_play_sound(int id) { G.sound = id; }
static void mock_menu_stop(int p) { (void)p; G.shown = 0; G.stops++; }
static void mock_go_back(int arg);
static void mock_push_screen(int p, int id);

#include "../src/games/menuhelper/mh_rac4.c"

/* GuiMenu_PushScreen: the screen before goes on the stack; 36's fcStart runs. */
static void mock_push_screen(int p, int id)
{
	(void)p;
	if (G.depth < 10) G.stack[G.depth++] = G.screen;
	G.screen = id;
	G.shown = 0;
	if (id == MH_SCREEN_ID) mh_start(0);
}

/* MenuMain_OptionsCMDGoBackCB: 0 back a screen (PopScreen), 1 out of the menu. */
static void mock_go_back(int arg)
{
	G.sound = arg == 0 ? SND_BACK : G.sound;
	if (G.screen == MH_SCREEN_ID) mh_stop(0);
	if (arg == 0 && G.depth > 0) {
		G.screen = G.stack[--G.depth];
	} else {
		G.screen = 0;
		G.depth = 0;
	}
	G.shown = G.screen != 0;
}

/* GuiHud's Start path with the menu asked to open: the two stubs' logic, in C. */
static void game_hud_frame(void)
{
	u8 req = r8(BOX(MBOX_OFF_REQUEST));
	int force = 0;

	w8(ST(ST_FORCE36), 0);
	if (req != 0) {
		w8(BOX(MBOX_OFF_REQUEST), 0);
		if (req == MBOX_REQ_OPEN && r32(DL_GAME_TYPE) != 1) {
			w8(ST(ST_FORCE36), 1);
			inc(ST_INJECTS);
			force = 1;
		}
	}
	if (force && r8(ST(ST_FORCE36))) {
		w8(ST(ST_FORCE36), 0);
		inc(ST_COMBO_OPENS);
		G.depth = 0;
		G.screen = MH_SCREEN_ID;
		mh_start(0);
	}
}

/* A callback the game holds, by the token the renderer gave it. */
static void call_token(u32 token, int arg)
{
	if (token == MH_FN_ROW) mh_row(arg);
	else if (token == MH_FN_CLOSE) mh_close(arg);
	else if (token == MH_FN_OPEN) mh_open(arg);
}

#define PAD0 0x00B11E80u

/*
 * One frame of GuiSupport_UpdateMenus with `bits` pressed: the renderer's frame
 * first, as the hook runs it, and the game's own input only when it answers 0,
 * as GuiMain_GetMenuCommand would rank the presses.
 */
static void game_frame(u32 bits)
{
	w32(PAD0 + DL_PAD_HUDBITS, bits);
	if (mh_frame(0)) return;

	bits = r32(PAD0 + DL_PAD_HUDBITS);
	w32(PAD0 + DL_PAD_HUDBITS, 0);
	if (!G.shown || G.nbuttons == 0) return;

	if (bits & PADB_CROSS) {
		call_token(r32(G.buttons + (u32)G.cursor * MH_BUTTON_SIZE + 8), 0);
	} else if (bits & PADB_TRIANGLE) {
		call_token(G.close_cb, 0);
	} else if (bits & (PADB_UP | PADB_DOWN)) {
		int i, c = G.cursor;
		for (i = 0; i < G.nbuttons; i++) {
			c = (bits & PADB_DOWN) ? (c + 1) % G.nbuttons : (c + G.nbuttons - 1) % G.nbuttons;
			if (r8(G.buttons + (u32)c * MH_BUTTON_SIZE) != 0) break;
		}
		G.cursor = c;
	} else if (bits & (PADB_START | PADB_SELECT)) {
		call_token(G.close_cb, 1);
	}
}

/* Row i of the list as the game shows it. */
static const char *shown_text(int i, char *out, u32 cap)
{
	u32 a = r32(G.buttons + (u32)i * MH_BUTTON_SIZE + 4);
	u32 n = 0;

	while (n + 1 < cap) {
		u8 c = (u8)r8(a + n);
		if (c == 0) break;
		out[n++] = (char)c;
	}
	out[n] = 0;
	return out;
}

static int shown_row(const char *text)
{
	char buf[80];
	int i;

	for (i = 0; i < G.nbuttons; i++) {
		if (strcmp(shown_text(i, buf, sizeof(buf)), text) == 0) return i;
	}
	return -1;
}

static const char *shown_title(char *out, u32 cap)
{
	u32 n = 0;

	while (n + 1 < cap) {
		u8 c = (u8)r8(G.title + n);
		if (c == 0) break;
		out[n++] = (char)c;
	}
	out[n] = 0;
	return out;
}

/* ------------------------------------------------------------- helpers */

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

static u32 peek32(u32 a)
{
	u8 b[4];
	host_peek(a, b, 4);
	return be32_get(b);
}

static u8 peek8(u32 a)
{
	u8 b = 0;
	host_peek(a, &b, 1);
	return b;
}

static void poke32(u32 a, u32 v)
{
	u8 b[4];
	be32_put(b, v);
	host_poke(a, b, 4);
}

static void poke8(u32 a, u8 v)
{
	host_poke(a, &v, 1);
}

static const struct menu_row *view_row(const struct menu_view *v, const char *label)
{
	u8 i;
	for (i = 0; i < v->nrows; i++) {
		if (strcmp(v->rows[i].label, label) == 0) return &v->rows[i];
	}
	return NULL;
}

static int view_index(const struct menu_view *v, const char *label)
{
	u8 i;
	for (i = 0; i < v->nrows; i++) {
		if (strcmp(v->rows[i].label, label) == 0) return i;
	}
	return -1;
}

/* An event on the row labelled `label` of the view `v` was built as. */
static int act(struct menu_view *v, u8 kind, const char *label)
{
	int i = view_index(v, label);
	int rc;

	if (i < 0) return -1;
	rc = menu_model_event(kind, (u8)i, v->rows[i].id, v->key);
	menu_model_view(v);
	return rc;
}

/* The page in the mailbox, decoded. */
struct box_page {
	u32 seq, key;
	u8 n, cursor, pflags;
	char title[MBOX_TITLE_LEN + 1];
	struct { u8 kind, flags; u16 id; char label[MROW_LABEL_LEN + 1]; char value[MROW_VALUE_LEN + 1]; } rows[8];
};

static void read_box(struct box_page *p)
{
	u32 mb = menu_mailbox();
	u8 buf[MH_MBOX_SIZE];
	u8 i;

	memset(p, 0, sizeof(*p));
	host_peek(mb, buf, sizeof(buf));
	p->seq = be32_get(buf + MBOX_OFF_PAGE_SEQ);
	p->key = be32_get(buf + MBOX_OFF_PAGE_KEY);
	p->n = buf[MBOX_OFF_NROWS];
	p->cursor = buf[MBOX_OFF_CURSOR];
	p->pflags = buf[MBOX_OFF_PFLAGS];
	memcpy(p->title, buf + MBOX_OFF_TITLE, MBOX_TITLE_LEN);
	for (i = 0; i < p->n && i < 8; i++) {
		const u8 *r = buf + MBOX_OFF_ROWS + i * MBOX_ROW_SIZE;
		p->rows[i].kind = r[MROW_OFF_KIND];
		p->rows[i].flags = r[MROW_OFF_FLAGS];
		p->rows[i].id = be16_get(r + MROW_OFF_ID);
		memcpy(p->rows[i].label, r + MROW_OFF_LABEL, MROW_LABEL_LEN);
		memcpy(p->rows[i].value, r + MROW_OFF_VALUE, MROW_VALUE_LEN);
	}
}

static int box_index(const struct box_page *p, const char *label)
{
	u8 i;
	for (i = 0; i < p->n; i++) {
		if (strcmp(p->rows[i].label, label) == 0) return i;
	}
	return -1;
}

/* What a renderer does to post an event: the record, then ev_seq. */
static void box_event(u8 kind, int row)
{
	u32 mb = menu_mailbox();
	struct box_page p;
	u8 rec[8];

	read_box(&p);
	rec[0] = kind;
	rec[1] = (u8)(row < 0 ? 0 : row);
	be16_put(rec + 2, row >= 0 && row < p.n ? p.rows[row].id : 0);
	be32_put(rec + 4, p.key);
	host_poke(mb + MBOX_OFF_EV_KIND, rec, 8);
	poke32(mb + MBOX_OFF_EV_SEQ, peek32(mb + MBOX_OFF_EV_SEQ) + 1);
}

static int box_acked(void)
{
	u32 mb = menu_mailbox();
	return peek32(mb + MBOX_OFF_EV_SEQ) == peek32(mb + MBOX_OFF_EV_ACK);
}

static int hooks_are(const struct mh_desc *d, int ours)
{
	u8 i;
	for (i = 0; i < d->nhooks; i++) {
		u32 want = ours ? d->hooks[i].value : d->hooks[i].original;
		if (peek32(d->hooks[i].addr) != want) return 0;
	}
	return 1;
}

static void press(u32 mask)
{
	host_set_pad(0);
	tick(2);
	host_set_pad(mask);
	tick(2);
	host_set_pad(0);
	tick(2);
}

/* Deadlocked, up and its menu installed (the delay cut to nothing). */
static int dl_ready(void)
{
	int i;

	menu_set_install_delay_us(0);
	if (!boot("NPEA00423")) return 0;
	for (i = 0; i < 20 && menu_state() != MENU_READY; i++) tick(1);
	/* The game's pause and pending-mode words as a game at play has them. */
	poke32(0x00B3C5A0u, 0);
	poke32(0x009C37F4u, 0xFFFFFFFEu);
	return menu_state() == MENU_READY;
}

/* ------------------------------------------------------------- the model */

static void test_model_dl(void)
{
	struct menu_view v;
	const struct menu_row *r;
	u8 i;
	int rc;

	group("console menu: the model, Deadlocked");

	check(boot("NPEA00423"), "Deadlocked boots");
	menu_model_reset(8, MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST);
	menu_model_view(&v);

	check(strcmp(v.title, "RaCMAN") == 0 && (v.pflags & MBOX_PAGE_ROOT) != 0,
	      "the root is titled RaCMAN and says it is the root");
	check(v.nrows == 6 && strcmp(v.rows[0].label, "Cheats") == 0 &&
	      strcmp(v.rows[1].label, "Player") == 0 && strcmp(v.rows[4].label, "Fixes") == 0,
	      "and lists Deadlocked's five DESCRIBE groups in order");
	for (i = 0; i < 5; i++) {
		if (v.rows[i].kind != MROW_LINK || v.rows[i].flags != MROWF_ENABLED) break;
	}
	check(i == 5, "each a link the cursor can land on");
	check(v.rows[5].kind == MROW_INFO && v.rows[5].flags == 0 &&
	      strcmp(v.rows[5].label, "qwark build 54") == 0, "then the build, grey");
	check(menu_model_depth() == 1 && menu_model_page() == MENU_PAGE_ID(MENU_PAGE_ROOT, 0),
	      "the stack holds the root alone");

	/* A stale event changes nothing. */
	rc = menu_model_event(MBOX_EV_ACTIVATE, 0, v.rows[0].id, v.key + 1);
	check(rc == ST_BAD_ARG && menu_model_depth() == 1, "an event on another page's key is refused");
	rc = menu_model_event(MBOX_EV_ACTIVATE, 0, v.rows[1].id, v.key);
	check(rc == ST_BAD_ARG && menu_model_depth() == 1, "an event whose row id does not match is refused");
	rc = menu_model_event(MBOX_EV_ACTIVATE, 9, 0, v.key);
	check(rc == ST_BAD_ARG, "an event past the last row is refused");
	rc = menu_model_event(MBOX_EV_BACK, 0, v.rows[0].id, v.key);
	check(rc == ST_BAD_ARG && menu_model_depth() == 1, "back at the root has nowhere to go");

	/* Cheats. */
	check(act(&v, MBOX_EV_ACTIVATE, "Cheats") == ST_OK, "the cross on Cheats opens it");
	check(strcmp(v.title, "Cheats") == 0 && v.pflags == 0 && v.nrows == 4 && v.cursor == 0,
	      "a page of four rows titled Cheats, not the root, the cursor on the first");
	r = view_row(&v, "Ghost Ratchet");
	check(r != NULL && r->kind == MROW_TOGGLE &&
	      r->flags == (MROWF_ENABLED | MROWF_STEPS) && strcmp(r->value, "OFF") == 0,
	      "Ghost Ratchet is a toggle, off, that Left and Right reach");

	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0,
	      "the cross turns it on, through features_set, as FEATURE_SET does");
	r = view_row(&v, "Ghost Ratchet");
	check(r != NULL && (r->flags & MROWF_ON) != 0 && strcmp(r->value, "ON") == 0, "and the row says ON");
	check(act(&v, MBOX_EV_LEFT, "Ghost Ratchet") == ST_OK &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) == 0, "Left turns it off");
	check(act(&v, MBOX_EV_LEFT, "Ghost Ratchet") == ST_OK &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) == 0, "Left again leaves it off");
	check(act(&v, MBOX_EV_RIGHT, "Ghost Ratchet") == ST_OK &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0, "Right turns it on");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) == 0, "and the cross off again");

	rc = menu_model_event(MBOX_EV_PAGE_NEXT, 0, v.rows[0].id, v.key);
	check(rc == ST_BAD_ARG && strcmp(v.title, "Cheats") == 0, "R1 on a page of one window does nothing");

	rc = menu_model_event(MBOX_EV_BACK, 2, v.rows[2].id, v.key);
	menu_model_view(&v);
	check(rc == ST_OK && strcmp(v.title, "RaCMAN") == 0 && v.cursor == 0,
	      "back goes to the root, the cursor on Cheats");

	/* Player: an action, three values and a choice. */
	check(act(&v, MBOX_EV_ACTIVATE, "Player") == ST_OK && strcmp(v.title, "Player") == 0,
	      "Player opens");
	r = view_row(&v, "Die");
	check(r != NULL && r->kind == MROW_ACTION && r->flags == MROWF_ENABLED && r->value[0] == 0,
	      "Die is an action");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_VALUE && r->flags == (MROWF_ENABLED | MROWF_STEPS) &&
	      strcmp(r->value, "0") == 0, "Bolts is a value stepper reading 0");
	r = view_row(&v, "Skin");
	check(r != NULL && r->kind == MROW_CHOICE && (r->flags & MROWF_STEPS) != 0 &&
	      r->value[0] != 0, "Skin is a choice showing its option's name");

	poke32(RAC4_BOLTS, 500);
	tick(9);
	menu_model_view(&v);
	r = view_row(&v, "Bolts");
	check(r != NULL && strcmp(r->value, "500") == 0, "Bolts follows its readout");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 501,
	      "Right adds one, written as FEATURE_SET writes it");
	r = view_row(&v, "Bolts");
	check(r != NULL && strcmp(r->value, "501") == 0, "and the row shows it before the readout catches up");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 502,
	      "a second Right before the readout moved starts from what was written");
	check(act(&v, MBOX_EV_ACTIVATE, "Bolts") == ST_OK, "the cross changes the step");
	r = view_row(&v, "Bolts");
	check(r != NULL && strcmp(r->value, "502 [10]") == 0, "to 10, which the row says");
	check(act(&v, MBOX_EV_LEFT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 492, "Left takes ten");
	for (i = 0; i < 5; i++) act(&v, MBOX_EV_ACTIVATE, "Bolts");
	r = view_row(&v, "Bolts");
	check(r != NULL && strcmp(r->value, "492 [1000000]") == 0, "the step goes up to a million");
	act(&v, MBOX_EV_ACTIVATE, "Bolts");
	r = view_row(&v, "Bolts");
	check(r != NULL && strcmp(r->value, "492") == 0, "and back to one");
	check(act(&v, MBOX_EV_ACTIVATE, "Bolts") == ST_OK && act(&v, MBOX_EV_ACTIVATE, "Bolts") == ST_OK &&
	      act(&v, MBOX_EV_ACTIVATE, "Bolts") == ST_OK && act(&v, MBOX_EV_LEFT, "Bolts") == ST_OK &&
	      peek32(RAC4_BOLTS) == 0, "a step past zero stops at zero");
	while (view_row(&v, "Bolts") != NULL && strcmp(view_row(&v, "Bolts")->value, "0") != 0)
		act(&v, MBOX_EV_ACTIVATE, "Bolts");

	/* Challenge mode, 0..255: the step stops at 100. */
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	r = view_row(&v, "Challenge mode");
	check(r != NULL && strcmp(r->value, "0 [100]") == 0, "Challenge mode's step reaches 100");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	r = view_row(&v, "Challenge mode");
	check(r != NULL && strcmp(r->value, "0") == 0, "and 1000 is past its range of 0..255, so back to 1");
	check(act(&v, MBOX_EV_LEFT, "Challenge mode") == ST_OK && peek8(RAC4_CHALLENGE_MODE) == 0,
	      "Left at 0 stays at 0");
	for (i = 0; i < 2; i++) act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	act(&v, MBOX_EV_RIGHT, "Challenge mode");
	act(&v, MBOX_EV_RIGHT, "Challenge mode");
	act(&v, MBOX_EV_RIGHT, "Challenge mode");
	check(peek8(RAC4_CHALLENGE_MODE) == 255, "Right by 100 stops at 255");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	act(&v, MBOX_EV_LEFT, "Challenge mode");
	tick(9);
	act(&v, MBOX_EV_LEFT, "Challenge mode");

	/* Skin: Left and Right cycle, the cross opens the list. */
	poke8(RAC4_SKIN, 0);
	tick(9);
	menu_model_view(&v);
	check(act(&v, MBOX_EV_LEFT, "Skin") == ST_OK && peek8(RAC4_SKIN) == RAC4_SKIN_COUNT - 1,
	      "Left on the first skin wraps to the last");
	check(act(&v, MBOX_EV_RIGHT, "Skin") == ST_OK && peek8(RAC4_SKIN) == 0, "Right wraps back");
	check(act(&v, MBOX_EV_RIGHT, "Skin") == ST_OK && peek8(RAC4_SKIN) == 1, "and on to the second");
	tick(9);
	menu_model_view(&v);

	i = (u8)view_index(&v, "Skin");
	check(act(&v, MBOX_EV_ACTIVATE, "Skin") == ST_OK && menu_model_depth() == 3 &&
	      menu_model_page() == MENU_PAGE_ID(MENU_PAGE_CHOICE, R4_SKIN),
	      "the cross on Skin opens its list as a page of its own");
	check(strcmp(v.title, "Skin 1/4") == 0, "twenty-two skins on screens of eight are four windows");
	check(v.nrows == 8 && v.rows[0].kind == MROW_OPTION && v.rows[7].kind == MROW_MORE &&
	      strcmp(v.rows[7].label, "More") == 0, "seven skins and a More row");
	check(v.cursor == 1 && (v.rows[1].flags & MROWF_ON) != 0 &&
	      (v.rows[0].flags & MROWF_ON) == 0, "the cursor on the current skin, the only one marked");

	check(act(&v, MBOX_EV_ACTIVATE, "More") == ST_OK && strcmp(v.title, "Skin 2/4") == 0 &&
	      v.rows[0].kind == MROW_PREV && v.rows[7].kind == MROW_MORE && v.cursor == 1,
	      "More: a Previous row, six skins, a More row, the cursor on the first skin");
	check(v.rows[1].id == MENU_ROW_ID(MENU_ROW_OPTION, 7), "starting at the eighth");
	act(&v, MBOX_EV_ACTIVATE, "More");
	act(&v, MBOX_EV_ACTIVATE, "More");
	check(strcmp(v.title, "Skin 4/4") == 0 && v.nrows == 4 && v.rows[0].kind == MROW_PREV &&
	      v.rows[3].id == MENU_ROW_ID(MENU_ROW_OPTION, 21), "the last window: Previous and the last three");
	check(act(&v, MBOX_EV_ACTIVATE, "Previous") == ST_OK && strcmp(v.title, "Skin 3/4") == 0 &&
	      v.cursor == 6, "Previous goes back a window, the cursor last before More");
	rc = menu_model_event(MBOX_EV_PAGE_PREV, 0, 0, v.key);
	menu_model_view(&v);
	check(rc == ST_OK && strcmp(v.title, "Skin 2/4") == 0 && v.cursor == 1, "L1 does the same, the cursor first");
	rc = menu_model_event(MBOX_EV_PAGE_NEXT, 0, 0, v.key);
	menu_model_view(&v);
	check(rc == ST_OK && strcmp(v.title, "Skin 3/4") == 0, "R1 goes forward");
	rc = menu_model_event(MBOX_EV_LEFT, 1, v.rows[1].id, v.key);
	check(rc == ST_OK && peek8(RAC4_SKIN) == 1, "Left on an option changes nothing");

	rc = menu_model_event(MBOX_EV_ACTIVATE, 2, v.rows[2].id, v.key);
	menu_model_view(&v);
	check(rc == ST_OK && peek8(RAC4_SKIN) == 14, "the cross on the fifteenth skin sets it");
	check(menu_model_depth() == 2 && strcmp(v.title, "Player") == 0 && v.cursor == i,
	      "and goes back to Player, the cursor on Skin");
	r = view_row(&v, "Skin");
	check(r != NULL && r->value[0] != 0, "which names the new skin");

	/* An action's answer stands in the row for two seconds. */
	check(act(&v, MBOX_EV_ACTIVATE, "Die") == ST_OK, "Die runs");
	r = view_row(&v, "Die");
	check(r != NULL && strcmp(r->value, "done") == 0, "and its row says done");
	check(strcmp(v.hint, "Die: done") == 0, "and the page's hint says it as a sentence");
	check(r != NULL && strcmp(r->desc, "Runs once.") == 0, "an action's description");
	r = view_row(&v, "Bolts");
	check(r != NULL && strcmp(r->desc, "Left and Right step it; select changes the step.") == 0,
	      "a value's description");
	tick(250);
	menu_model_view(&v);
	r = view_row(&v, "Die");
	check(r != NULL && r->value[0] == 0, "for two seconds");

	/* The savefile actions: loading closes the menu first. */
	rc = menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Savefile");
	r = view_row(&v, "Load set-aside file");
	check(r != NULL && (r->flags & MROWF_CLOSES) != 0, "Load set-aside file closes the menu before it runs");
	r = view_row(&v, "Set aside file");
	check(r != NULL && (r->flags & MROWF_CLOSES) == 0, "Set aside file does not");
	check(act(&v, MBOX_EV_ACTIVATE, "Load set-aside file") == ST_OK, "the cross on it answers OK");
	r = view_row(&v, "Load set-aside file");
	check(r != NULL && strcmp(r->value, "after closing") == 0, "and the row says it waits");

	/* A renderer without Left and Right, nor a back control of its own. */
	menu_model_reset(8, 0);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Player");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_INFO && r->flags == 0 && strcmp(r->value, "0") == 0,
	      "with no Left and Right a value is shown and nothing more");
	r = view_row(&v, "Skin");
	check(r != NULL && r->kind == MROW_CHOICE && r->flags == MROWF_ENABLED, "a choice still opens its list");
	check(v.rows[v.nrows - 1].kind == MROW_BACK && strcmp(v.rows[v.nrows - 1].label, "Back") == 0,
	      "and every page but the root ends in a Back row");
	check(act(&v, MBOX_EV_ACTIVATE, "Back") == ST_OK && menu_model_depth() == 1, "which goes back");
	check(v.rows[v.nrows - 1].kind == MROW_INFO, "the root has none");
	rc = menu_model_event(MBOX_EV_LEFT, 0, v.rows[0].id, v.key);
	check(rc == ST_OK && menu_model_depth() == 1, "Left on a link does nothing");
	poke8(RAC4_SKIN, 0);
	tick(9);
	act(&v, MBOX_EV_ACTIVATE, "Player");
	act(&v, MBOX_EV_ACTIVATE, "Skin");
	check(strcmp(v.title, "Skin 1/4") == 0 && v.nrows == 8 && v.rows[5].kind == MROW_OPTION &&
	      v.rows[6].kind == MROW_MORE && v.rows[7].kind == MROW_BACK,
	      "a choice list pages one row shorter, for its Back row: six skins, More, Back");

	/* The smallest screen a renderer may declare. */
	menu_model_reset(1, MBOX_CAP_BACK);
	menu_model_view(&v);
	check(v.nrows == 3 && v.rows[2].kind == MROW_MORE && strcmp(v.title, "RaCMAN 1/4") == 0,
	      "a renderer of one row is taken as three: two rows and More");
	act(&v, MBOX_EV_ACTIVATE, "More");
	check(v.nrows == 3 && v.rows[0].kind == MROW_PREV && v.rows[1].kind == MROW_LINK &&
	      v.rows[2].kind == MROW_MORE, "then Previous, one row, More");

	check(quit(), "quit Deadlocked");
}

/* Every group of every window of UYA's longest page, and nothing twice. */
static void test_model_paging(void)
{
	struct menu_view v;
	const struct game_describe *d;
	u8 g, best = 0, bestn = 0, seen, k;
	u16 ids[64];
	int guard;

	group("console menu: the model, paging UYA's groups");

	check(boot("NPEA00387"), "UYA boots");
	d = session_game()->describe();
	for (g = 0; g < d->ngroups; g++) {
		u8 n = 0, i;
		for (i = 0; i < d->nfeatures; i++) if (d->features[i].group == g) n++;
		if (n > bestn) { bestn = n; best = g; }
	}
	check(bestn > 8, "UYA has a group longer than a screen");

	menu_model_reset(8, MBOX_CAP_STEPS | MBOX_CAP_BACK);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, d->groups[best]);
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_GROUP, best), "its page opens");

	seen = 0;
	for (guard = 0; guard < 20; guard++) {
		int more = 0;
		u8 i;
		for (i = 0; i < v.nrows; i++) {
			if (v.rows[i].kind == MROW_MORE) more = 1;
			else if (v.rows[i].kind != MROW_PREV && seen < 64) ids[seen++] = v.rows[i].id;
		}
		check(v.nrows <= 8, "a window never has more rows than the screen");
		if (!more) break;
		act(&v, MBOX_EV_ACTIVATE, "More");
	}
	check(seen == bestn, "the windows show every feature of the group");
	for (k = 1; k < seen; k++) {
		if (ids[k] == ids[k - 1]) break;
	}
	check(k == seen, "each once");
	for (guard = 0; guard < 20 && v.rows[0].kind == MROW_PREV; guard++)
		act(&v, MBOX_EV_ACTIVATE, "Previous");
	check(v.rows[0].kind != MROW_PREV && v.rows[v.nrows - 1].kind == MROW_MORE,
	      "and Previous walks back to the first");

	check(quit(), "quit UYA");
}

/* ------------------------------------------------------------ the mailbox */

static void test_mailbox(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC4);
	struct box_page p;
	u32 mb, seq0, n0;
	int i;

	group("console menu: the install and the mailbox");

	check(d != NULL && d->mailbox == MH_DATA && d->nhooks == 8 && d->ncaves == 2,
	      "Deadlocked has a renderer: a data image, the code, eight hook words");
	check(mh_desc_for_game(GAME_RAC1) == NULL && mh_desc_for_game(GAME_NONE) == NULL,
	      "RaC1 has none yet, and no game none");
	if (d == NULL) return;

	/* The default delay: nothing for three seconds. */
	menu_set_install_delay_us(3000000ull);
	check(boot("NPEA00423"), "Deadlocked boots");
	poke32(0x00B3C5A0u, 0);
	poke32(0x009C37F4u, 0xFFFFFFFEu);
	check(menu_state() == MENU_WAITING && hooks_are(d, 0), "the renderer waits, every hook word the game's");
	tick(340);
	check(menu_state() == MENU_WAITING && peek32(MH_DATA) == 0,
	      "nothing written two and a half seconds in");
	for (i = 0; i < 40 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "three seconds in every hook is the renderer's");

	mb = menu_mailbox();
	check(mb == MH_DATA && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_VERSION + 1) == 1 &&
	      peek8(mb + MBOX_OFF_MAX_ROWS) == 8 && peek8(mb + MBOX_OFF_CAPS) == MH_CAPS,
	      "the mailbox: magic, version 1, eight rows, the renderer's caps");
	check(peek32(MH_STATE) == MH_MAGIC, "the renderer's state, its magic in place");
	{
		u8 buf[64];
		host_peek(MH_CAVE, buf, sizeof(buf));
		check(memcmp(buf, d->caves[1].bytes, sizeof(buf)) == 0, "the code at the cave");
	}

	read_box(&p);
	check(p.seq != 0 && (p.seq & 1) == 0, "a page is in, page_seq even");
	check(strcmp(p.title, "RaCMAN") == 0 && p.n == 6 && (p.pflags & MBOX_PAGE_ROOT) != 0 &&
	      strcmp(p.rows[0].label, "Cheats") == 0, "the root page");

	/* Events. */
	box_event(MBOX_EV_ACTIVATE, 0);
	check(!box_acked(), "an event posted is not acked yet");
	tick(4);
	read_box(&p);
	check(box_acked() && strcmp(p.title, "Cheats") == 0 && p.n == 4,
	      "within a poll it is acked, and the Cheats page is posted");

	seq0 = p.seq;
	tick(20);
	read_box(&p);
	check(p.seq == seq0, "a page that has not changed is not written again");

	i = box_index(&p, "Ghost Ratchet");
	box_event(MBOX_EV_ACTIVATE, i);
	tick(4);
	read_box(&p);
	check(box_acked() && (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0 &&
	      strcmp(p.rows[i].value, "ON") == 0, "the cross on Ghost Ratchet turns it on, and the page says so");

	/* A stale key, a wrong id: acked, nothing done. */
	{
		u8 rec[8];
		seq0 = p.seq;
		rec[0] = MBOX_EV_ACTIVATE;
		rec[1] = (u8)i;
		be16_put(rec + 2, p.rows[i].id);
		be32_put(rec + 4, p.key ^ 0x100u);
		host_poke(mb + MBOX_OFF_EV_KIND, rec, 8);
		poke32(mb + MBOX_OFF_EV_SEQ, peek32(mb + MBOX_OFF_EV_SEQ) + 1);
		tick(4);
		check(box_acked() && (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0,
		      "an event on a page no longer shown is acked and does nothing");
	}

	/* The live values follow, ten times a second while the menu is up. */
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	features_set(R4_GHOST, 0);
	tick(13);
	read_box(&p);
	check(strcmp(p.rows[i].value, "OFF") == 0, "a toggle set elsewhere shows in the page within a tenth of a second");

	box_event(MBOX_EV_BACK, 0);
	tick(2);
	read_box(&p);
	check(box_acked() && strcmp(p.title, "RaCMAN") == 0 && p.cursor == 0, "back: the root, cursor hint 0");
	n0 = p.key;
	box_event(MBOX_EV_ACTIVATE, 1);
	tick(2);
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	read_box(&p);
	check(p.cursor == 1 && p.key != n0, "back from Player: cursor hint 1, and a key not seen before");

	/* The seqlock: a page is written with page_seq odd. */
	host_write_log_reset();
	features_set(R4_GHOST, 1);
	box_event(MBOX_EV_ACTIVATE, 0);
	tick(2);
	{
		u32 a0 = 0, a1 = 0, a2 = 0, l0 = 0, l1 = 0, l2 = 0, k;
		int found = 0;
		for (k = 0; k + 2 < host_write_log_count() && k + 2 < 64; k++) {
			host_write_log_at(k, &a0, &l0);
			host_write_log_at(k + 1, &a1, &l1);
			host_write_log_at(k + 2, &a2, &l2);
			if (a0 == mb + MBOX_OFF_PAGE_SEQ && a1 == mb + MBOX_OFF_PAGE_KEY &&
			    a2 == mb + MBOX_OFF_PAGE_SEQ) { found = 1; break; }
		}
		check(found, "a page goes out as page_seq, the page, page_seq: three writes");
	}
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(4);

	/* The combo, default L1 + R1 + L2 + R2 + L3 + R3. */
	check(config_combo(COMBO_OPEN_MENU) == COMBO_MENU_DEFAULT && COMBO_MENU_DEFAULT == 0x060Fu,
	      "the menu combo is L1 + R1 + L2 + R2 + L3 + R3 out of the box");
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "pressing it asks the renderer to open");
	tick(62);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "a request nobody takes is withdrawn half a second on");

	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "with the menu up it asks it to close");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(4);

	config_set_combo_enabled(0);
	tick(2);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "with every combo switched off the menu combo still opens the menu");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	config_set_combo_enabled(1);
	tick(2);

	session_combo_suspend(1);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "a client's capture hold holds it off");
	session_combo_suspend(0);

	config_set_combo(COMBO_OPEN_MENU, 0);
	tick(2);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "combo.menu = 0 turns it off");
	config_set_combo(COMBO_OPEN_MENU, 0x0300);
	tick(2);
	press(0x0300);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "and another mask works like any combo");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	config_set_combo(COMBO_OPEN_MENU, COMBO_MENU_DEFAULT);
	tick(2);

	/* A CLOSES action: the menu is asked to close, the action runs once it has. */
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	read_box(&p);
	box_event(MBOX_EV_ACTIVATE, box_index(&p, "Savefile"));
	tick(2);
	read_box(&p);
	box_event(MBOX_EV_ACTIVATE, box_index(&p, "Load set-aside file"));
	tick(2);
	read_box(&p);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE &&
	      strcmp(p.rows[box_index(&p, "Load set-aside file")].value, "after closing") == 0,
	      "loading the set-aside file asks the menu to close first");
	check(peek8(0x015CD71Eu) == 0, "and asks the game for nothing yet");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	poke32(0x00B3C5A0u, 3);
	tick(8);
	check(peek8(0x015CD71Eu) == 0, "not while the game is still paused");
	poke32(0x00B3C5A0u, 0);
	tick(8);
	check(peek8(0x015CD71Eu) == 1, "and once it plays again the load goes out, as LOAD_ASIDE sends it");
	poke8(0x015CD71Eu, 0);
	tick(40);

	n0 = host_notify_count();
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	read_box(&p);
	box_event(MBOX_EV_ACTIVATE, box_index(&p, "Load set-aside file"));
	tick(2);
	tick(5 * 120 + 10);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "dropped") != NULL,
	      "a menu that never closes drops the load after five seconds, and says so");
	box_event(MBOX_EV_BACK, 0);
	poke8(mb + MBOX_OFF_REQUEST, 0);
	tick(4);

	/* menu.enabled = 0: out once it is safe, in again when it is 1. */
	config_set_u32("menu.enabled", 0);
	tick(4);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1),
	      "menu.enabled = 0 with the menu up leaves the hooks in");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	poke32(0x00B3C5A0u, 3);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "and with the game paused");
	poke32(0x00B3C5A0u, 0);
	poke32(0x0110D784u, MH_SCREEN_ID);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "and with screen 36 the current one");
	poke32(0x0110D784u, 0);
	tick(8);
	check(menu_state() == MENU_REMOVED && hooks_are(d, 0), "the menu down, the game in play: every word back");
	n0 = host_notify_count();
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "menu.enabled") != NULL,
	      "the combo says the menu is switched off");
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1, "once a session");
	config_set_u32("menu.enabled", 1);
	tick(4);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "menu.enabled = 1 puts it back");

	/* Taken over: what a module loaded again into a running game finds. */
	menu_set_install_delay_us(0);
	poke32(MH_STATE + ST_STARTS, 7);
	poke32(mb + MBOX_OFF_EV_SEQ, peek32(mb + MBOX_OFF_EV_ACK) + 1);
	menu_enter(session_game());
	tick(4);
	check(menu_state() == MENU_READY && peek32(MH_STATE + ST_STARTS) == 7,
	      "a renderer of this build already in is taken over, its state as it was");
	check(box_acked(), "an event left from before is answered unread");

	/*
	 * A renderer with a description line declares rows of 0x80 bytes: the same
	 * mailbox, fewer rows, and each row's description where there is room.
	 */
	poke8(mb + MBOX_OFF_ROW_CAP, 4);
	poke8(mb + MBOX_OFF_ROW_SIZE, MBOX_ROW_SIZE_DESC);
	poke8(mb + MBOX_OFF_MAX_ROWS, 4);
	menu_enter(session_game());
	tick(4);
	{
		u8 row[MBOX_ROW_SIZE_DESC];
		u8 rec[8];
		u32 r1 = mb + MBOX_OFF_ROWS + MBOX_ROW_SIZE_DESC;

		host_peek(r1, row, sizeof(row));
		check(menu_state() == MENU_READY && peek8(mb + MBOX_OFF_NROWS) == 4 &&
		      strcmp((const char *)row + MROW_OFF_LABEL, "Player") == 0 &&
		      peek8(mb + MBOX_OFF_ROWS + 3 * MBOX_ROW_SIZE_DESC) == MROW_MORE,
		      "rows of 0x80 bytes: four of them, the second Player, the last More");
		rec[0] = MBOX_EV_ACTIVATE;
		rec[1] = 0;
		host_peek(mb + MBOX_OFF_ROWS + MROW_OFF_ID, rec + 2, 2);
		be32_put(rec + 4, peek32(mb + MBOX_OFF_PAGE_KEY));
		host_poke(mb + MBOX_OFF_EV_KIND, rec, 8);
		poke32(mb + MBOX_OFF_EV_SEQ, peek32(mb + MBOX_OFF_EV_SEQ) + 1);
		tick(4);
		host_peek(r1, row, sizeof(row));
		check(box_acked() && strcmp((const char *)row + MROW_OFF_LABEL, "Ghost Ratchet") == 0 &&
		      strcmp((const char *)row + MROW_OFF_DESC, "On or off.") == 0,
		      "and each row carries its description at +0x40");
	}
	poke8(mb + MBOX_OFF_ROW_SIZE, 0x50);
	menu_enter(session_game());
	tick(4);
	check(menu_state() == MENU_REFUSED, "a row size this build does not know is refused");
	poke8(mb + MBOX_OFF_ROW_CAP, 8);
	poke8(mb + MBOX_OFF_ROW_SIZE, MBOX_ROW_SIZE);
	poke8(mb + MBOX_OFF_MAX_ROWS, 8);
	menu_enter(session_game());
	tick(4);
	check(menu_state() == MENU_READY, "and the renderer's own shape is taken over again");

	/* Refused: another mod at a site. */
	check(quit(), "quit");
	host_boot("NPEA00423");
	poke32(MH_SITE_OPTIONS, 0x48000001u);
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	tick(4);
	check(menu_state() == MENU_REFUSED && peek32(MH_DATA) == 0 && peek32(MH_SITE_FRAME) == MH_ORIG_FRAME,
	      "a hook site holding another mod's word: nothing written at all");
	n0 = host_notify_count();
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "another mod") != NULL,
	      "the combo says why");
	poke32(MH_SITE_OPTIONS, MH_ORIG_OPTIONS);
	tick(10 * 120 + 10);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "ten seconds after the word is the game's again, it goes in");

	/* Refused: a renderer of another build. */
	poke8(MH_CAVE + 0x100, (u8)(peek8(MH_CAVE + 0x100) ^ 0xFF));
	menu_enter(session_game());
	tick(4);
	check(menu_state() == MENU_REFUSED, "hooks that are ours over code that is not this build's are left alone");

	check(quit(), "quit Deadlocked");

	/* RPCS3: nothing at all. */
	host_set_can_patch_code(0);
	check(boot("NPEA00423"), "Deadlocked boots under RPCS3");
	tick(20);
	check(menu_state() == MENU_OFF && peek32(MH_DATA) == 0 && hooks_are(d, 0),
	      "the menu is off and writes nothing");
	n0 = host_notify_count();
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "RPCS3") != NULL,
	      "the combo says it is not available under RPCS3");
	check(quit(), "quit");
	host_set_can_patch_code(1);

	/* A game with no renderer. */
	check(boot("NPEA00385"), "RaC1 boots");
	tick(20);
	n0 = host_notify_count();
	check(menu_state() == MENU_OFF, "RaC1 has no menu");
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "not available in this game") != NULL,
	      "the combo says so");
	check(quit(), "quit RaC1");

	menu_set_install_delay_us(3000000ull);
}

/* ----------------------------------------------------------- the renderer */

static void test_renderer(void)
{
	char text[80], title[40];
	u32 rows = 0x0119F9A8u + 0x194u;    /* MenuMain_PlayerData[0].vButtons */
	u32 mb;
	int i, n;

	group("console menu: Deadlocked's renderer on the fake console");

	memset(&G, 0, sizeof(G));
	G.input_ok = 1;
	check(dl_ready(), "Deadlocked up, the renderer installed");
	poke32(DL_PAD0_PTR, PAD0);
	mb = menu_mailbox();

	/* Options: the RaCMAN row in front of Go back. */
	for (i = 0; i < 6; i++) {
		poke32(rows + 12u * (u32)i, 0x01000000u);
		poke32(rows + 12u * (u32)i + 4, 0x1000u + (u32)i);
		poke32(rows + 12u * (u32)i + 8, i == 5 ? OPD_GO_BACK_CB : 0x2000u + (u32)i);
	}
	poke32(rows + 0x60, 6);
	n = mh_options_hook(0, 6, rows);
	check(n == 7 && peek32(rows + 0x60) == 7, "Options' six rows become seven, iButtons with them");
	check(peek32(rows + 12 * 6 + 8) == OPD_GO_BACK_CB && peek32(rows + 12 * 5 + 8) == MH_FN_OPEN,
	      "Go back last, the RaCMAN row before it");
	check(mh_options_hook(0, 8, rows) == 8 && peek32(MH_STATE + ST_OPTIONS_FULL) == 1,
	      "a full list is left alone, and counted");
	poke32(DL_GAME_TYPE, 1);
	check(mh_options_hook(0, 6, rows) == 6, "no row in multiplayer");
	poke32(DL_GAME_TYPE, 0);

	/* The row: screen 36 starts with qwark's page. */
	G.screen = 6;
	G.cursor = 5;
	call_token(MH_FN_OPEN, 0);
	check(G.screen == MH_SCREEN_ID && G.shown && peek32(0x0119F9A8u + 4) == 5,
	      "the RaCMAN row pushes screen 36, the Options cursor kept");
	check(peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && G.sound == SND_SCREEN,
	      "the mailbox says open, with the screen's sound");
	check(strcmp(shown_title(title, sizeof(title)), "RaCMAN") == 0 && G.nbuttons == 6,
	      "the list shows the root, titled RaCMAN");
	check(strcmp(shown_text(0, text, sizeof(text)), "Cheats") == 0 &&
	      strcmp(shown_text(5, text, sizeof(text)), "qwark build 54") == 0 &&
	      peek8(G.buttons + 5 * 12) == 0 && peek32(G.buttons + 5 * 12 + 8) == 0,
	      "Cheats first, the build last, grey and with no callback");
	check(G.close_cb == MH_FN_CLOSE, "the list's close callback is the renderer's");

	tick(4);

	/* The cross on Cheats. */
	G.cursor = 0;
	game_frame(PADB_CROSS);
	check(G.sound == SND_FORWARD && !box_acked(), "the cross posts an event, with the forward sound");
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Cheats") == 0 && G.nbuttons == 4 && G.widgets == 1,
	      "the next frame shows Cheats, laid out again for its four rows");
	check(strcmp(shown_text(1, text, sizeof(text)), "Ghost Ratchet: OFF") == 0,
	      "each row its label and value");

	game_frame(PADB_DOWN);
	check(G.cursor == 1, "down moves the game's cursor, no qwark involved");
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(shown_text(1, text, sizeof(text)), "Ghost Ratchet: ON") == 0 && G.cursor == 1 &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0,
	      "the cross turns Ghost Ratchet on, the cursor kept on it");
	i = G.selects;

	game_frame(PADB_LEFT);
	check(peek32(PAD0 + DL_PAD_HUDBITS) == 0 && G.sound == SND_MOVE, "Left is the renderer's, and consumed");
	tick(2);
	game_frame(0);
	check(strcmp(shown_text(1, text, sizeof(text)), "Ghost Ratchet: OFF") == 0 && G.selects > i,
	      "and turns it off, the highlight put back");
	game_frame(PADB_LEFT | PADB_CROSS);
	check(peek32(PAD0 + DL_PAD_HUDBITS) == 0 && !box_acked(),
	      "with the cross in the same frame the game takes the cross, which outranks Left");
	tick(2);
	game_frame(0);

	/* One event in flight. */
	game_frame(PADB_CROSS);
	game_frame(PADB_CROSS);
	check(peek32(MH_STATE + ST_DROPPED) == 1, "a second press before qwark answers is dropped");
	for (i = 0; i < 31; i++) game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Waiting for qwark...") == 0,
	      "half a second unanswered and the title says so");
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Cheats") == 0, "the answer puts it back");

	/* Back, then the root's back. */
	game_frame(PADB_TRIANGLE);
	check(G.sound == SND_BACK && G.screen == MH_SCREEN_ID, "triangle on a page posts back");
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "RaCMAN") == 0 && G.cursor == 0,
	      "the root again, the cursor on Cheats");
	game_frame(PADB_TRIANGLE);
	check(G.screen == 6 && G.stops == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "triangle at the root is the game's own: back to Options, the mailbox closed");

	/* The combo: qwark asks, the Start path opens screen 36. */
	G.screen = 0;
	tick(8);
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	check(G.screen == MH_SCREEN_ID && G.shown && peek8(mb + MBOX_OFF_REQUEST) == 0 &&
	      peek32(MH_STATE + ST_COMBO_OPENS) == 1, "the combo opens the menu through the Start path");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "and again asks it to close");
	game_frame(PADB_CROSS);
	check(G.screen == 0 && !G.shown && peek8(mb + MBOX_OFF_REQUEST) == 0,
	      "which the next frame does, before the frame's cross");
	tick(4);

	/* Start closes from anywhere, whatever qwark is doing. */
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	game_frame(PADB_START);
	check(G.screen == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED, "Start closes the menu");

	/* Values and a choice list, opened again on the page it was left on. */
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	tick(4);
	G.cursor = 1;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Player") == 0, "Player");
	poke32(RAC4_BOLTS, 41);
	tick(14);
	game_frame(0);
	i = shown_row("Bolts: 41");
	check(i >= 0, "Bolts: 41");
	G.cursor = i;
	game_frame(PADB_RIGHT);
	tick(2);
	game_frame(0);
	check(shown_row("Bolts: 42") == i && peek32(RAC4_BOLTS) == 42, "Right steps it to 42");
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(shown_row("Bolts: 42 [10]") == i, "the cross shows a step of 10");

	poke8(RAC4_SKIN, 3);
	tick(14);
	game_frame(0);
	for (i = 0; i < G.nbuttons; i++) {
		if (strncmp(shown_text(i, text, sizeof(text)), "Skin: ", 6) == 0) break;
	}
	check(i < G.nbuttons, "Skin and its name");
	G.cursor = i;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Skin 1/4") == 0 && G.cursor == 3 &&
	      strstr(shown_text(3, text, sizeof(text)), " *") != NULL,
	      "the list opens, the cursor on the current skin, marked");
	game_frame(PADB_R1);
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Skin 2/4") == 0 && G.cursor == 1, "R1 turns the page");
	game_frame(PADB_L1);
	tick(2);
	game_frame(0);
	check(strcmp(shown_title(title, sizeof(title)), "Skin 1/4") == 0, "L1 turns it back");
	G.cursor = 5;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(peek8(RAC4_SKIN) == 5 && strcmp(shown_title(title, sizeof(title)), "Player") == 0,
	      "the cross on the sixth skin sets it and goes back to Player");
	game_frame(PADB_R1);
	check(peek32(MH_STATE + ST_EVENTS) == peek32(mb + MBOX_OFF_EV_SEQ), "R1 on a page of one window posts nothing");

	/* A page qwark is still writing is left for the next frame. */
	{
		u32 seq = peek32(mb + MBOX_OFF_PAGE_SEQ);
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 1);
		n = (int)peek32(MH_STATE + ST_PAGES);
		game_frame(0);
		check(peek32(MH_STATE + ST_PAGES) == (u32)n, "page_seq odd: the page is not taken");
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq);
	}

	game_frame(PADB_START);
	check(G.screen == 0, "closed");

	/* Before qwark has posted anything: one grey row, and back leaves. */
	poke32(MH_STATE + ST_MAGIC, MH_MAGIC);
	poke8(MH_STATE + ST_HAVE_PAGE, 0);
	poke32(mb + MBOX_OFF_PAGE_SEQ, 0);
	G.screen = MH_SCREEN_ID;
	mh_start(0);
	check(G.nbuttons == 1 && strcmp(shown_text(0, text, sizeof(text)), "Waiting for qwark") == 0,
	      "with no page yet the menu says it waits");
	game_frame(PADB_TRIANGLE);
	check(G.screen == 0, "and triangle leaves it");

	check(quit(), "quit Deadlocked");
	menu_set_install_delay_us(3000000ull);
}

void test_menu(void)
{
	test_model_dl();
	test_model_paging();
	test_mailbox();
	test_renderer();
}
