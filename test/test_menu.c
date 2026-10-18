/*
 * The console menu (revision 1.22, mailbox version 2), on qwark's fake console.
 *
 * Three layers, each against the real core:
 *
 *   the model     menu_model_view / menu_model_event for Deadlocked and UYA:
 *                 Deadlocked's pages (the root, Game, Cheats, Planets, Manips,
 *                 Unlocks, Save files), the page types, every control (the
 *                 three-way toggle, selectors, buttons, steppers), windows,
 *                 stale events, and every row doing what the protocol op does;
 *   the mailbox   the driver against a renderer simulated at the byte level: the
 *                 install a few seconds into the session and its refusals, the
 *                 Help row's words, the page seqlock and the page type, events
 *                 and acks, the open and close requests and the combo, a CLOSES
 *                 action of each kind, menu.enabled, taking a renderer over,
 *                 RPCS3 and a game with none;
 *   the renderer  Deadlocked's, the shared core (src/games/menuhelper/mh_core.c)
 *                 and the shim (mh_rac4.c) compiled for the PC (MH_HOST) and run
 *                 on the same fake memory qwark writes, against mocks of the
 *                 game's menu - GuiMenu's screens, GuiSupport's list, the
 *                 CtlList and the GuiState thread - close enough to the game's
 *                 own code to show the frame fitting a page, the move between
 *                 the list and the two-column page, the Help row and the closes.
 *
 * test_menu_core.c tests the core alone against a fake shim. The hook stubs are
 * assembler and do not run here; src/games/menuhelper/check.py steps those.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/mem.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/manips.h"
#include "../src/core/savefile.h"
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

static u8 *mh_host_ptr(u32 addr)
{
	return host_mem_ptr(addr);
}

#include "../src/games/menuhelper/mh_rac4.h"
#include "../src/games/menuhelper/mh_common.h"
#include "../src/games/menuhelper/mh_shim.h"

/*
 * The game's menu, as far as the renderer can see it: one player, GuiMenu's
 * screens and stack, the pause, GuiSupport's list, the GuiState thread slot,
 * one CtlList and the texts and colours the widgets were given.
 */
#define NTEXT 96
static struct {
	int screen, next;
	int stack[10];
	int depth;
	int paused;
	int main_up;             /* the game's own main menu (screen 1) is shown */
	int shown;               /* GuiSupport's list is shown */
	int cursor;
	int nbuttons;
	u32 buttons;
	u32 title;
	u32 close_cb;
	int targets;             /* SetMenuTarget calls: the frame fitted again */
	int sound;
	int selects;
	int stops;
	int closes;
	int input_ok;
	int cmd;                 /* what GetMenuCommand answers next */
	u32 gs;
	int thread;              /* 0 none, 1 Init due, 2 running, 3 leaving */
	u32 handles;
	int screen_tpl;
	u32 list, items;
	int list_tpl, count, cap;
	int dirty[16];
	u32 validator;
	int layer;               /* the canvas layer that holds a screen panel */
	int images;
	int cursor_attached;
	int cursor_moves;
	u32 text_h[NTEXT], text_s[NTEXT];
	u32 color_h[NTEXT], color_c[NTEXT];
} G;

/* Strings the game's msg_string answers, in fake game memory. */
#define MSG_AT 0x00A80000u

static void mock_go_back(int arg);
static void mock_menu_stop(int p);
static void mock_go_to_screen(int p, int s);
static void mock_start_next(int p);

static int mock_get_canvas(void) { return 0; }
static void mock_set_canvas(int c) { (void)c; }
static int mock_get_cursor(int p) { (void)p; return G.cursor; }
static int mock_get_screen(int p) { (void)p; return G.screen; }
static void mock_set_cursor(int p, int i) { (void)p; G.cursor = i; }
static void mock_set_title(int p, u32 s) { (void)p; G.title = s; }
static int mock_screen_mode(int p) { (void)p; return 0; }
static void mock_set_target(int p, float x, float y, float w, float h)
{
	(void)p; (void)x; (void)y; (void)w; (void)h;
	G.targets++;
}
static void mock_set_header(int p, int on) { (void)p; (void)on; }
static void mock_set_buttons(int p, int n, u32 rows) { (void)p; G.nbuttons = n; G.buttons = rows; }
static void mock_show_menu(int p, u32 cb) { (void)p; G.shown = 1; G.close_cb = cb; }
static void mock_select_row(void) { G.selects++; }
static int mock_takes_input(int p, int kind) { (void)p; (void)kind; return G.input_ok; }
static void mock_play_sound(int id) { G.sound = id; }
static int mock_get_command(int kind) { int c = G.cmd; (void)kind; G.cmd = 0; return c; }
static int mock_base_frame(int p) { (void)p; return 0; }
static void mock_reset_state(u32 gs) { w32(gs, 0); }
static void mock_exit_state(u32 gs) { w32(gs, 0); G.thread = 3; }
static int mock_start_thread(u32 gs)
{
	if (G.thread == 1 || G.thread == 2) return -1;
	G.gs = gs;
	G.thread = 1;
	return 0;
}
static void mock_panel_create(u32 at, int tpl, int canvas)
{
	(void)canvas;
	w32(at, G.handles);
	G.handles += 0x100;
	if (tpl == DL_TPL_SCREEN) G.screen_tpl = tpl;
}
static void mock_panel_frame(u32 at, float f, int canvas) { (void)at; (void)f; (void)canvas; }
static void mock_canvas_layer(int layer, u32 h) { (void)h; G.layer = layer; }
static void mock_clear_layer(int layer) { if (layer == G.layer) G.layer = 0; }
static void mock_add_frame(u32 h, u32 f) { (void)h; (void)f; }
static void mock_set_texture(u32 h, int on) { (void)h; (void)on; }
static void mock_rect_source(u32 h, u32 img) { (void)h; (void)img; }
static u32 mock_msg_string(int id) { return id == DL_MSG_SELECT ? MSG_AT : MSG_AT + 0x20; }
static u32 mock_image_get(u32 from, int n) { (void)from; (void)n; G.images++; return 0x1230 + (u32)G.images; }
static void mock_image_drop(u32 img) { (void)img; G.images--; }
static void mock_box_target(u32 l, u32 h, int jump) { (void)l; (void)h; (void)jump; }
static void mock_box_shown(u32 l, int on) { (void)l; (void)on; }
static void mock_frame_target(int p, int i, u32 h) { (void)p; (void)i; (void)h; }
static void mock_show_frames(int p, int on) { (void)p; (void)on; }
static void mock_attach_cursor(int p, int i, u32 h, u32 t, int z) { (void)p; (void)i; (void)h; (void)t; (void)z; G.cursor_attached = 1; }
static void mock_show_cursors(int p, int on) { (void)p; (void)on; }
static void mock_detach_cursor(int p, int i, u32 h, int z) { (void)p; (void)i; (void)h; (void)z; G.cursor_attached = 0; }
static void mock_cursor_target(int p, int i, float x, float y, float w, float h, int jump)
{
	(void)p; (void)i; (void)x; (void)y; (void)w; (void)h; (void)jump;
	G.cursor_moves++;
}

static void remember(u32 *hs, u32 *vs, u32 h, u32 v)
{
	int i;
	for (i = 0; i < NTEXT; i++) {
		if (hs[i] == h || hs[i] == 0) {
			hs[i] = h;
			vs[i] = v;
			return;
		}
	}
}

static u32 recall(const u32 *hs, const u32 *vs, u32 h)
{
	int i;
	for (i = 0; i < NTEXT && hs[i] != 0; i++) {
		if (hs[i] == h) return vs[i];
	}
	return 0;
}

static void mock_set_text(u32 h, u32 s) { remember(G.text_h, G.text_s, h, s); }
static void mock_set_color(u32 h, u32 c) { remember(G.color_h, G.color_c, h, c); }

/* The CtlList, as ctllist.cpp has it (REPORT-dl-ui-catalogue.md section 3). */
static int focus(void) { return (int)r32(G.list + DL_LIST_FOCUS); }
static u32 item(int i) { return G.items + (u32)i * DL_ITEM_SIZE; }
static int mh_cfg_valid(int index);

static void mock_list_create(u32 l, u32 items, int n, u32 cb, int tpl, int bounds)
{
	int i;
	(void)cb; (void)bounds;
	G.list = l;
	G.items = items;
	G.cap = n;
	G.list_tpl = tpl;
	G.count = 0;
	w32(l + DL_LIST_HANDLE, G.handles);
	G.handles += 0x100;
	w32(l + DL_LIST_FOCUS, 0xFFFFFFFFu);
	for (i = 0; i < n; i++) w8(item(i) + 0xD, 0);
}
static void mock_list_clear(u32 l) { (void)l; G.count = 0; w32(G.list + DL_LIST_FOCUS, 0xFFFFFFFFu); }
static void mock_list_insert(u32 l, u32 data, int at)
{
	u32 it = item(G.count);
	(void)l; (void)at;
	if (r8(it + 0xD) == 0) {
		w8(it + 0xD, 1);
		w32(it + DL_ITEM_PANEL, G.handles);
		G.handles += 0x100;
	}
	w32(it + 8, data);
	G.dirty[G.count] = 1;
	G.count++;
	if (focus() < 0) w32(G.list + DL_LIST_FOCUS, 0);
}
static void mock_list_validator(u32 l, u32 fc) { (void)l; G.validator = fc; }
static void mock_list_next(u32 l);
static void mock_list_focus(u32 l, int i)
{
	(void)l;
	if (G.count == 0) return;
	if (i > G.count - 1) i = G.count - 1;
	if (i < 0) i = 0;
	if (focus() >= 0) G.dirty[focus()] = 1;
	w32(G.list + DL_LIST_FOCUS, (u32)i);
	G.dirty[i] = 1;
	if (G.validator != 0 && !mh_cfg_valid(i)) mock_list_next(l);
}
static void mock_list_step(int dir)
{
	int i = focus(), n;
	for (n = 0; n < G.count; n++) {
		i = (i + G.count + dir) % G.count;
		if (G.validator == 0 || mh_cfg_valid(i)) break;
	}
	if (n == G.count) return;
	G.dirty[focus()] = 1;
	w32(G.list + DL_LIST_FOCUS, (u32)i);
	G.dirty[i] = 1;
}
static void mock_list_next(u32 l) { (void)l; mock_list_step(1); }
static void mock_list_prev(u32 l) { (void)l; mock_list_step(-1); }
static void mock_list_rect(u32 l, float *x, float *y, float *w, float *h)
{
	(void)l;
	*x = 0.5f; *y = 0.1f * (float)focus(); *w = 0.8f; *h = 0.05f;
}
static void mh_cfg_item(u32 list, u32 item, int index);
static int mock_list_update(u32 l)
{
	int i;
	for (i = 0; i < G.count; i++) {
		if (!G.dirty[i]) continue;
		mh_cfg_item(l, item(i), i);
		G.dirty[i] = 0;
	}
	return 1;
}

#include "../src/games/menuhelper/mh_core.c"
#include "../src/games/menuhelper/mh_rac4.c"

/* A text the game was given, by the widget's handle. */
static const char *text_of(u32 h, char *out, u32 cap)
{
	u32 a = recall(G.text_h, G.text_s, h), n = 0;

	while (a != 0 && n + 1 < cap) {
		u8 c = (u8)r8(a + n);
		if (c == 0) break;
		out[n++] = (char)c;
	}
	out[n] = 0;
	return out;
}

/* Row i of the CONFIG list: its label, its value and its colour. */
static const char *cfg_label(int i, char *out) { return text_of(r32(item(i) + DL_ITEM_PANEL) + DL_ROW_LABEL, out, 64); }
static const char *cfg_value(int i, char *out) { return text_of(r32(item(i) + DL_ITEM_PANEL) + DL_ROW_VALUE, out, 64); }
static u32 cfg_color(int i) { return recall(G.color_h, G.color_c, r32(item(i) + DL_ITEM_PANEL) + DL_ROW_LABEL); }
/*
 * The GuiMenu screens the menu moves between, as guimenu.cpp has them:
 * GoToScreen stops the current screen, whose stop (MenuMain_Stop for a list)
 * starts the next; PushScreen keeps the one it leaves; PopScreen goes back to
 * it, or with nothing under closes the menu. Screen 1 is the game's own main
 * menu, which needs nothing but to be shown.
 */
static void start_screen(int s)
{
	G.screen = s;
	if (s == MH_SCREEN_ID) mh_start(0);
	else if (s == 1) { G.main_up = 1; G.shown = 1; G.close_cb = 0; G.nbuttons = 8; }
}

static void mock_start_next(int p) { (void)p; start_screen(G.next); }

static void stop_screen(void)
{
	if (G.screen == MH_SCREEN_ID) mh_stop(0);
	else if (G.screen == 1) { G.main_up = 0; G.shown = 0; mock_start_next(0); }
}

static void mock_go_to_screen(int p, int s)
{
	(void)p;
	G.next = s;
	stop_screen();
	G.screen = s;
}

static void mock_menu_stop(int p)
{
	(void)p;
	G.shown = 0;
	G.stops++;
	mock_start_next(p);
}

/* GuiMenu_CloseMenu: the menu state popped, the screen stopped, the game playing. */
static void mock_close_menu(int sound)
{
	if (sound >= 0) G.sound = sound;
	G.next = 0;
	stop_screen();
	G.screen = 0;
	G.shown = 0;
	G.main_up = 0;
	G.paused = 0;
	G.closes++;
}

/* MenuMain_OptionsCMDGoBackCB: 1 closes with sound 5, 0 plays 8 and pops. */
static void mock_go_back(int arg)
{
	if (arg != 0) {
		mock_close_menu(5);
		return;
	}
	G.sound = SND_BACK;
	if (G.depth < 1) {
		mock_close_menu(-1);
		return;
	}
	mock_go_to_screen(0, G.stack[--G.depth]);
}

/* A frame of the canvas's threads: a new one is Init and Update at once, one leaving Deinit. */
static void canvas_frame(void)
{
	if (G.thread == 3) {
		G.thread = 0;
		mh_cfg_deinit(G.gs);
	} else if (G.thread == 1) {
		G.thread = 2;
		mh_cfg_init(G.gs);
		mh_cfg_update(G.gs);
	} else if (G.thread == 2) {
		mh_cfg_update(G.gs);
	}
}

/*
 * One frame of GuiHud with the menu asked to open: the two stubs' logic in C
 * (check.py steps the real ones), then the game's own Start path,
 * GuiMenu_GoToScreen(0, 36) and GuiMain_PushState(0, 2), whose thread's init
 * (0x36270C) zeroes the screen stack's depth.
 */
static void game_hud_frame(void)
{
	u8 req = r8(MH_BOX + MBOX_OFF_REQUEST);
	int force = 0;

	w8(MH_SHIM + SH_FORCE36, 0);
	if (req != 0) {
		w8(MH_BOX + MBOX_OFF_REQUEST, 0);
		if (req == MBOX_REQ_OPEN && r32(DL_GAME_TYPE) != 1) {
			mh_count(ST_OPEN_REQS);
			force = 1;
		}
	}
	if (force) {
		mh_count(ST_OPENS);
		G.depth = 0;
		G.paused = 1;
		G.next = MH_SCREEN_ID;
		start_screen(MH_SCREEN_ID);
	}
}

/*
 * The pause main menu, opened with Start, and its fifth row: with the menu
 * installed the row's label is the shim's and its callback, Help's own,
 * pushes screen 36 (mh_rac4.h, MH_SITE_HELP_*), keeping the main menu's cursor.
 */
static void game_open_pause(void)
{
	G.depth = 0;
	G.paused = 1;
	G.next = 1;
	start_screen(1);
	G.cursor = 4;
}

static void game_help_row(void)
{
	G.stack[G.depth++] = G.screen;
	mock_go_to_screen(0, MH_SCREEN_ID);
	mock_play_sound(SND_FORWARD);
}

/* A callback the game holds, by the token the renderer gave it. */
static void call_token(u32 token, int arg)
{
	if (token == MH_FN_ROW) mh_row(arg);
	else if (token == MH_FN_CLOSE) mh_close(arg);
}

#define PAD0 0x00B11E80u

/*
 * One frame of the game with `bits` pressed: GuiSupport_UpdateMenus for a list
 * of screen 36 (the shim's frame first, as the hook runs it, then the game's
 * own input only when it answers 0), or the canvas's threads for its CONFIG
 * page, given `cmd` as GetMenuCommand's answer.
 */
static void game_frame(u32 bits)
{
	int cmd = 0;

	if (bits & PADB_CROSS) cmd = DL_CMD_CROSS;
	else if (bits & PADB_TRIANGLE) cmd = DL_CMD_TRIANGLE;
	else if (bits & PADB_UP) cmd = DL_CMD_UP;
	else if (bits & PADB_DOWN) cmd = DL_CMD_DOWN;
	else if (bits & PADB_LEFT) cmd = DL_CMD_LEFT;
	else if (bits & PADB_RIGHT) cmd = DL_CMD_RIGHT;
	else if (bits & PADB_L1) cmd = DL_CMD_L1;
	else if (bits & PADB_R1) cmd = DL_CMD_R1;
	else if (bits & (PADB_START | PADB_SELECT)) cmd = DL_CMD_CLOSE;

	if (G.thread != 0) {
		G.cmd = cmd;
		canvas_frame();
		G.cmd = 0;
		return;
	}

	w32(PAD0 + DL_PAD_HUDBITS, bits);
	if (G.screen != MH_SCREEN_ID || !G.shown) return;
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

static const char *read_str(u32 a, char *out, u32 cap)
{
	u32 n = 0;

	while (a != 0 && n + 1 < cap) {
		u8 c = (u8)r8(a + n);
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

static u16 peek16(u32 a)
{
	u8 b[2];
	host_peek(a, b, 2);
	return be16_get(b);
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

static void poke16(u32 a, u16 v)
{
	u8 b[2];
	be16_put(b, v);
	host_poke(a, b, 2);
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

static const char *value_of(const struct menu_view *v, const char *label)
{
	const struct menu_row *r = view_row(v, label);
	return r != NULL ? r->value : "(no row)";
}

/* The page in the mailbox, decoded. */
struct box_page {
	u32 seq, key;
	u8 n, cursor, pflags, ptype;
	char title[MBOX_TITLE_LEN + 1];
	struct { u8 kind, flags; u16 id; char label[MROW_LABEL_LEN + 1]; char value[MROW_VALUE_LEN + 1];
	         char desc[MROW_DESC_LEN + 1]; } rows[16];
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
	p->ptype = buf[MBOX_OFF_PTYPE];
	memcpy(p->title, buf + MBOX_OFF_TITLE, MBOX_TITLE_LEN);
	for (i = 0; i < p->n && i < 16; i++) {
		const u8 *r = buf + MBOX_OFF_ROWS + i * MH_ROW_BYTES;
		p->rows[i].kind = r[MROW_OFF_KIND];
		p->rows[i].flags = r[MROW_OFF_FLAGS];
		p->rows[i].id = be16_get(r + MROW_OFF_ID);
		memcpy(p->rows[i].label, r + MROW_OFF_LABEL, MROW_LABEL_LEN);
		memcpy(p->rows[i].value, r + MROW_OFF_VALUE, MROW_VALUE_LEN);
		memcpy(p->rows[i].desc, r + MROW_OFF_DESC, MROW_DESC_LEN);
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

static void box_activate(const char *label)
{
	struct box_page p;
	read_box(&p);
	box_event(MBOX_EV_ACTIVATE, box_index(&p, label));
	tick(2);
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

/*
 * The call in MenuMain_StartOptions where round 1 put its RaCMAN row, with the
 * game's own word, which the fake console does not seed: the tests put it
 * there and watch that nothing changes it.
 */
#define DL_OPTIONS_SITE 0x003A6F2Cu
#define DL_OPTIONS_WORD 0x4BFC456Du

/* Deadlocked in play: the pause and pending-mode words as a game at play has them. */
static void dl_playing(void)
{
	poke32(DL_GAME_MODE, 0);
	poke32(DL_PENDING_MODE, DL_MODE_NONE);
}

/* Deadlocked up, in play, and nothing of the menu in it yet (the install is due later). */
static int dl_boot(void)
{
	if (!boot("NPEA00423")) return 0;
	dl_playing();
	poke32(DL_OPTIONS_SITE, DL_OPTIONS_WORD);
	return menu_state() == MENU_IDLE;
}

/* Waits out the install's delay in play; answers whether the menu serves. */
static int dl_installed(void)
{
	int i;

	dl_playing();
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	return menu_state() == MENU_READY;
}

/* A planet the save has unlocked, or not: the level save's status byte. */
static void dl_planet(u8 planet, int unlocked)
{
	poke8(RAC4_LEVEL_SAVES + (u32)(planet - 1) * RAC4_LEVEL_SIZE + 0x300, unlocked ? 0x02 : 0x00);
}

/* A weapon's level halfword: -1 locked, else the version less one. */
static void dl_weapon(u8 gadget, int version)
{
	poke16(RAC4_GADGETS + (u32)gadget * RAC4_GADGET_STRIDE, (u16)(version - 1));
}

static int dl_weapon_now(u8 gadget)
{
	return (int)(s16)peek16(RAC4_GADGETS + (u32)gadget * RAC4_GADGET_STRIDE) + 1;
}

#define MENU_SAVES "/dev_hdd0/qwark/savefiles/NPEA00423"

static void save_put(const char *cat, const char *name, u32 size)
{
	char path[160];
	plat_file_t f;
	static u8 chunk[4096];
	u32 done = 0;

	plat_dir_create("/dev_hdd0/qwark/savefiles");
	plat_dir_create(MENU_SAVES);
	qstrcpy(path, sizeof(path), MENU_SAVES "/");
	qstrcat(path, sizeof(path), cat);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), name);
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	memset(chunk, 0x5A, sizeof(chunk));
	while (done < size) {
		u32 n = size - done < sizeof(chunk) ? size - done : (u32)sizeof(chunk);
		plat_file_write(f, chunk, n);
		done += n;
	}
	plat_file_close(f);
}

static void save_drop(const char *cat, const char *name)
{
	char path[160];

	qstrcpy(path, sizeof(path), MENU_SAVES "/");
	qstrcat(path, sizeof(path), cat);
	if (name != NULL) {
		qstrcat(path, sizeof(path), "/");
		qstrcat(path, sizeof(path), name);
		plat_file_unlink(path);
		qstrcat(path, sizeof(path), SAVEFILE_SUM_EXT);
		plat_file_unlink(path);
	} else {
		plat_dir_remove(path);
	}
}

#define CAPS_ALL (MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST)

/* ------------------------------------------------------------- the model */

static void test_model_dl(void)
{
	struct menu_view v;
	const struct menu_row *r;
	u8 i;
	int rc;

	group("console menu: the model, Deadlocked's pages");

	check(boot("NPEA00423"), "Deadlocked boots");
	menu_model_reset(8, 16, CAPS_ALL);
	menu_model_view(&v);

	check(strcmp(v.title, "RaCMAN") == 0 && (v.pflags & MBOX_PAGE_ROOT) != 0 && v.ptype == MBOX_PTYPE_MAIN,
	      "the root is a MAIN page titled RaCMAN that says it is the root");
	check(v.nrows == 7 && strcmp(v.rows[0].label, "Game") == 0 && strcmp(v.rows[1].label, "Cheats") == 0 &&
	      strcmp(v.rows[2].label, "Planets") == 0 && strcmp(v.rows[3].label, "Manips") == 0 &&
	      strcmp(v.rows[4].label, "Unlocks") == 0 && strcmp(v.rows[5].label, "Save files") == 0,
	      "Game, Cheats, Planets, Manips, Unlocks, Save files, as the design has them");
	for (i = 0; i < 6; i++) {
		if (v.rows[i].kind != MROW_LINK || v.rows[i].flags != MROWF_ENABLED) break;
	}
	check(i == 6, "each a link");
	check(v.rows[6].kind == MROW_INFO && strcmp(v.rows[6].label, "qwark build 54") == 0, "then the build, grey");

	rc = menu_model_event(MBOX_EV_ACTIVATE, 0, v.rows[0].id, v.key + 1);
	check(rc == ST_BAD_ARG && menu_model_depth() == 1, "an event on another page's key is refused");
	rc = menu_model_event(MBOX_EV_ACTIVATE, 0, v.rows[1].id, v.key);
	check(rc == ST_BAD_ARG && menu_model_depth() == 1, "an event whose row id does not match is refused");
	rc = menu_model_event(MBOX_EV_BACK, 0, v.rows[0].id, v.key);
	check(rc == ST_BAD_ARG && menu_model_depth() == 1, "back at the root has nowhere to go");

	/* Cheats: four checkboxes of three ways, and a button. */
	check(act(&v, MBOX_EV_ACTIVATE, "Cheats") == ST_OK && strcmp(v.title, "Cheats") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 5, "Cheats is a CONFIG page of five rows");
	check(strcmp(v.rows[0].label, "Fast loads") == 0 && strcmp(v.rows[1].label, "Ghost Ratchet") == 0 &&
	      strcmp(v.rows[2].label, "Refill ammo") == 0 && strcmp(v.rows[3].label, "Freeze health") == 0 &&
	      strcmp(v.rows[4].label, "Unlock all planets") == 0,
	      "the four cheats in their order, then Unlock all planets");
	r = view_row(&v, "Ghost Ratchet");
	check(r != NULL && r->kind == MROW_TOGGLE && r->flags == (MROWF_ENABLED | MROWF_STEPS) &&
	      strcmp(r->value, "Off") == 0 && strstr(r->desc, "Auto") != NULL,
	      "Ghost Ratchet: Off, a three-way checkbox that says what Auto is");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0 &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0 && (features_toggle_auto() & ((u64)1 << R4_GHOST)) == 0,
	      "the cross: On, the cheat on and not on its own at boot");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "Auto") == 0 &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0 && (features_toggle_auto() & ((u64)1 << R4_GHOST)) != 0 &&
	      (view_row(&v, "Ghost Ratchet")->flags & MROWF_ON) != 0,
	      "again: Auto, on and its auto flag ticked, as the client's A box ticks it");
	check(config_feature_auto("NPEA00423", "Ghost Ratchet") == 1, "and config.txt has the flag");
	check(act(&v, MBOX_EV_RIGHT, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "Off") == 0 &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) == 0 && (features_toggle_auto() & ((u64)1 << R4_GHOST)) == 0,
	      "Right from Auto wraps to Off: off, and the flag down");
	check(act(&v, MBOX_EV_LEFT, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "Auto") == 0,
	      "Left from Off wraps to Auto");
	act(&v, MBOX_EV_LEFT, "Ghost Ratchet");
	act(&v, MBOX_EV_LEFT, "Ghost Ratchet");
	check(strcmp(value_of(&v, "Ghost Ratchet"), "Off") == 0, "Left twice more: On, then Off");

	r = view_row(&v, "Unlock all planets");
	check(r != NULL && r->kind == MROW_ACTION && r->flags == MROWF_ENABLED && r->value[0] == 0,
	      "Unlock all planets is a button with nothing to say yet");
	check(act(&v, MBOX_EV_ACTIVATE, "Unlock all planets") == ST_OK &&
	      strcmp(value_of(&v, "Unlock all planets"), "done") == 0 && strcmp(v.hint, "Unlock all planets: done") == 0,
	      "the cross runs it: the row says done, the hint says it as a sentence");
	tick(250);
	menu_model_view(&v);
	check(value_of(&v, "Unlock all planets")[0] == 0, "for two seconds");
	rc = menu_model_event(MBOX_EV_PAGE_NEXT, 0, v.rows[0].id, v.key);
	check(rc == ST_BAD_ARG, "R1 on a page of one window does nothing");
	menu_model_event(MBOX_EV_BACK, 1, v.rows[1].id, v.key);
	menu_model_view(&v);
	check(strcmp(v.title, "RaCMAN") == 0 && v.cursor == 1, "back: the root, the cursor on Cheats");

	/* Game: the quick block's buttons, the slot, and the values. */
	check(act(&v, MBOX_EV_ACTIVATE, "Game") == ST_OK && strcmp(v.title, "Game") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 9, "Game is a CONFIG page of nine rows");
	check(strcmp(v.rows[0].label, "Die") == 0 && strcmp(v.rows[1].label, "Position slot") == 0 &&
	      strcmp(v.rows[2].label, "Save position") == 0 && strcmp(v.rows[3].label, "Load position") == 0 &&
	      strcmp(v.rows[4].label, "Set aside file") == 0 && strcmp(v.rows[5].label, "Load set-aside file") == 0 &&
	      strcmp(v.rows[6].label, "Bolts") == 0 && strcmp(v.rows[7].label, "Dread points") == 0 &&
	      strcmp(v.rows[8].label, "Challenge mode") == 0,
	      "Die, the slot, Save and Load position, the two savefile buttons, Bolts, Dread points, Challenge mode");
	check(view_row(&v, "Skin") == NULL, "and no Skin: cosmetics are left out");
	config_set_selected_slot(0);
	menu_model_view(&v);
	r = view_row(&v, "Position slot");
	check(r != NULL && r->kind == MROW_CHOICE && strcmp(r->value, "1") == 0, "the slot is a selector on 1");
	check(act(&v, MBOX_EV_RIGHT, "Position slot") == ST_OK && config_selected_slot() == 1 &&
	      strcmp(value_of(&v, "Position slot"), "2") == 0, "Right: 2, the slot the client's selection has");
	act(&v, MBOX_EV_LEFT, "Position slot");
	check(act(&v, MBOX_EV_LEFT, "Position slot") == ST_OK && config_selected_slot() == 7 &&
	      strcmp(value_of(&v, "Position slot"), "8") == 0, "Left twice: 1, then 8, wrapping");
	act(&v, MBOX_EV_ACTIVATE, "Position slot");
	check(config_selected_slot() == 0, "the cross moves it on as well");
	poke32(RAC4_COORDS, 0x41200000u);
	check(act(&v, MBOX_EV_ACTIVATE, "Save position") == ST_OK && strcmp(value_of(&v, "Save position"), "done") == 0,
	      "Save position keeps where the player stands");
	poke32(RAC4_COORDS, 0);
	check(act(&v, MBOX_EV_ACTIVATE, "Load position") == ST_OK && peek32(RAC4_COORDS) == 0x41200000u,
	      "Load position puts the player back");
	r = view_row(&v, "Load set-aside file");
	check(r != NULL && (r->flags & MROWF_CLOSES) != 0, "Load set-aside file closes the menu before it runs");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_VALUE && strcmp(r->value, "0") == 0, "Bolts is a stepper reading 0");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 1, "Right adds one");
	act(&v, MBOX_EV_ACTIVATE, "Bolts");
	check(strcmp(value_of(&v, "Bolts"), "1 [10]") == 0, "the cross makes the step 10");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 11, "and Right adds ten");
	while (strcmp(value_of(&v, "Bolts"), "11") != 0) act(&v, MBOX_EV_ACTIVATE, "Bolts");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	check(strcmp(value_of(&v, "Challenge mode"), "0") == 0, "Challenge mode's step stops at 100, its range 0..255");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: only those the save has unlocked, each a button that loads it once the menu is closed. */
	for (i = 1; i <= 15; i++) dl_planet(i, 0);
	dl_planet(1, 1);
	dl_planet(2, 1);
	dl_planet(5, 1);
	dl_planet(3, 1);
	check(act(&v, MBOX_EV_ACTIVATE, "Planets") == ST_OK && strcmp(v.title, "Planets") == 0 &&
	      v.ptype == MBOX_PTYPE_MAIN && v.nrows == 3,
	      "Planets is a MAIN page of the three the save has unlocked");
	check(strcmp(v.rows[0].label, "DreadZone") == 0 && strcmp(v.rows[1].label, "Catacrom") == 0 &&
	      strcmp(v.rows[2].label, "Kronos") == 0,
	      "DreadZone, Catacrom, Kronos, in the game's order, the unlocked placeholder left out");
	check(v.rows[1].kind == MROW_ACTION && (v.rows[1].flags & MROWF_CLOSES) != 0, "each closes the menu first");
	check(act(&v, MBOX_EV_ACTIVATE, "Catacrom") == ST_OK && strcmp(value_of(&v, "Catacrom"), "after closing") == 0 &&
	      peek32(RAC4_TARGET_PLANET) != 2, "the cross on Catacrom: the row waits, nothing loads yet");
	dl_planet(4, 1);
	menu_model_view(&v);
	check(v.nrows == 4 && strcmp(v.rows[2].label, "Sarathos") == 0, "a planet unlocked since shows when the page is built");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Manips: the category, two checkboxes, Apply now, then the Fixes. */
	check(act(&v, MBOX_EV_ACTIVATE, "Manips") == ST_OK && v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 6,
	      "Manips is a CONFIG page of six rows");
	check(strcmp(v.rows[0].label, "Category") == 0 && strcmp(v.rows[0].value, "None") == 0 &&
	      strcmp(v.rows[1].label, "Auto-reset") == 0 && strcmp(v.rows[1].value, "On") == 0 &&
	      strcmp(v.rows[2].label, "Remember") == 0 && strcmp(v.rows[2].value, "Off") == 0 &&
	      strcmp(v.rows[3].label, "Apply now") == 0 && strcmp(v.rows[4].label, "Crash patches") == 0 &&
	      strcmp(v.rows[5].label, "Fix reset softlocks") == 0,
	      "Category None, Auto-reset On, Remember Off, Apply now, Crash patches, Fix reset softlocks");
	check(act(&v, MBOX_EV_RIGHT, "Category") == ST_OK && manips_selected() == 1 &&
	      strcmp(value_of(&v, "Category"), "NG+") == 0, "Right: NG+, selected as MANIP_SELECT selects it");
	check(act(&v, MBOX_EV_ACTIVATE, "Category") == ST_OK && manips_selected() == 0, "the cross: back to None");
	check(act(&v, MBOX_EV_ACTIVATE, "Auto-reset") == ST_OK && (manips_options_now() & 1) == 0 &&
	      strcmp(value_of(&v, "Auto-reset"), "Off") == 0, "the cross on Auto-reset turns it off");
	act(&v, MBOX_EV_LEFT, "Auto-reset");
	check((manips_options_now() & 1) == 1, "and Left back on");
	check(act(&v, MBOX_EV_ACTIVATE, "Apply now") == ST_OK && strcmp(value_of(&v, "Apply now"), "done") == 0,
	      "Apply now with None applies nothing, and says done");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Unlocks: the tabs, then each a CONFIG page of selectors. */
	check(act(&v, MBOX_EV_ACTIVATE, "Unlocks") == ST_OK && v.ptype == MBOX_PTYPE_MAIN && v.nrows == 5 &&
	      strcmp(v.rows[0].label, "Weapons") == 0 && strcmp(v.rows[1].label, "Gadgets") == 0 &&
	      strcmp(v.rows[2].label, "Bot upgrades") == 0 && strcmp(v.rows[3].label, "Max all weapon levels") == 0 &&
	      strcmp(v.rows[4].label, "Max all weapon ammo") == 0,
	      "Unlocks lists Weapons, Gadgets, Bot upgrades, and the two buttons the client has above its table");
	check(act(&v, MBOX_EV_ACTIVATE, "Weapons") == ST_OK && strcmp(v.title, "Weapons") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 10, "Weapons: a CONFIG page of the ten weapons");
	dl_weapon(2, 0);
	menu_model_view(&v);
	r = view_row(&v, "Dual Vipers");
	check(r != NULL && r->kind == MROW_CHOICE && strcmp(r->value, "LOCKED") == 0 && (r->flags & MROWF_ON) == 0,
	      "a locked weapon reads LOCKED");
	check(act(&v, MBOX_EV_RIGHT, "Dual Vipers") == ST_OK && dl_weapon_now(2) == 1 &&
	      strcmp(value_of(&v, "Dual Vipers"), "V1") == 0 && (view_row(&v, "Dual Vipers")->flags & MROWF_ON) != 0,
	      "Right: V1, owned, as UNLOCK_SET owns it");
	act(&v, MBOX_EV_RIGHT, "Dual Vipers");
	check(dl_weapon_now(2) == 10 && strcmp(value_of(&v, "Dual Vipers"), "V10") == 0, "Right: V10");
	act(&v, MBOX_EV_ACTIVATE, "Dual Vipers");
	check(dl_weapon_now(2) == 99 && strcmp(value_of(&v, "Dual Vipers"), "V99") == 0, "the cross: V99");
	act(&v, MBOX_EV_RIGHT, "Dual Vipers");
	check(dl_weapon_now(2) == 0 && strcmp(value_of(&v, "Dual Vipers"), "LOCKED") == 0, "Right wraps to LOCKED");
	act(&v, MBOX_EV_LEFT, "Dual Vipers");
	check(dl_weapon_now(2) == 99, "and Left from LOCKED to V99");
	dl_weapon(2, 5);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Dual Vipers"), "V5") == 0, "a version off the list shows as itself, V5");
	act(&v, MBOX_EV_RIGHT, "Dual Vipers");
	check(dl_weapon_now(2) == 10, "and Right goes to the next on the list, V10");
	dl_weapon(2, 0);
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Gadgets");
	r = view_row(&v, "Charge Boots");
	check(v.nrows == 2 && r != NULL && (strcmp(r->value, "LOCKED") == 0 || strcmp(r->value, "UNLOCKED") == 0),
	      "Gadgets: the two pairs of boots, LOCKED or UNLOCKED");
	poke16(RAC4_GADGETS + 17u * RAC4_GADGET_STRIDE, 0xFFFF);
	menu_model_view(&v);
	check(act(&v, MBOX_EV_RIGHT, "Charge Boots") == ST_OK && peek16(RAC4_GADGETS + 17u * RAC4_GADGET_STRIDE) == 0 &&
	      strcmp(value_of(&v, "Charge Boots"), "UNLOCKED") == 0, "Right owns the boots");
	act(&v, MBOX_EV_RIGHT, "Charge Boots");
	check(peek16(RAC4_GADGETS + 17u * RAC4_GADGET_STRIDE) == 0xFFFF, "and again takes them away");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Bot upgrades");
	check(v.nrows == 16 && v.rows[15].kind == MROW_CHOICE && strcmp(v.title, "Bot upgrades") == 0,
	      "Bot upgrades: all sixteen in one window, which a CONFIG page of sixteen rows scrolls");
	poke8(RAC4_BOTS_UNLOCK + 2, 0);
	menu_model_view(&v);
	check(act(&v, MBOX_EV_RIGHT, "Bogo") == ST_OK && peek8(RAC4_BOTS_UNLOCK + 2) == 1 &&
	      strcmp(value_of(&v, "Bogo"), "UNLOCKED") == 0, "a bot upgrade is LOCKED or UNLOCKED too");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Save files: the library's categories, then their saves, each a button that loads it. */
	save_put("menu-test", "first.sav", 16);
	save_put("menu-test", "second.sav", 16);
	check(act(&v, MBOX_EV_ACTIVATE, "Save files") == ST_OK && v.ptype == MBOX_PTYPE_MAIN &&
	      view_row(&v, "menu-test") != NULL, "Save files lists the library's categories, menu-test among them");
	check(act(&v, MBOX_EV_ACTIVATE, "menu-test") == ST_OK && strcmp(v.title, "menu-test") == 0 && v.nrows == 2 &&
	      view_row(&v, "first") != NULL && view_row(&v, "second") != NULL,
	      "its page lists its two saves by name, without the .sav");
	r = view_row(&v, "first");
	check(r != NULL && r->kind == MROW_ACTION && (r->flags & MROWF_CLOSES) != 0, "each loads once the menu is closed");
	check(act(&v, MBOX_EV_ACTIVATE, "first") == ST_OK && strcmp(value_of(&v, "first"), "after closing") == 0,
	      "the cross on one: it waits");
	save_drop("menu-test", "first.sav");
	save_drop("menu-test", "second.sav");
	save_drop("menu-test", NULL);
	menu_model_view(&v);
	check(v.nrows == 1 && v.rows[0].kind == MROW_INFO && strcmp(v.rows[0].label, "Nothing here") == 0,
	      "a category that has gone says so");

	/* A renderer without Left and Right, nor a back control, nor a CONFIG page. */
	menu_model_reset(8, 0, 0);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(v.ptype == MBOX_PTYPE_MAIN, "with no CONFIG page every page is MAIN");
	check(v.nrows == 8 && v.rows[6].kind == MROW_MORE && v.rows[7].kind == MROW_BACK,
	      "nine rows on screens of eight with a Back row: six, More, Back");
	act(&v, MBOX_EV_ACTIVATE, "More");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_INFO && r->flags == 0, "and with no Left and Right a value is shown and nothing more");
	check(act(&v, MBOX_EV_ACTIVATE, "Back") == ST_OK && menu_model_depth() == 1, "Back goes back");

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

	group("console menu: the model, paging UYA's groups (round 1's pages, no menu table yet)");

	check(boot("NPEA00387"), "UYA boots");
	d = session_game()->describe();
	for (g = 0; g < d->ngroups; g++) {
		u8 n = 0, i;
		for (i = 0; i < d->nfeatures; i++) if (d->features[i].group == g) n++;
		if (n > bestn) { bestn = n; best = g; }
	}
	check(bestn > 8, "UYA has a group longer than a screen");

	menu_model_reset(8, 0, MBOX_CAP_STEPS | MBOX_CAP_BACK);
	menu_model_view(&v);
	check(view_row(&v, d->groups[0]) != NULL, "its root: its DESCRIBE groups first");
	for (guard = 0; guard < 4 && view_row(&v, "Unlocks") == NULL && v.rows[v.nrows - 1].kind == MROW_MORE; guard++)
		act(&v, MBOX_EV_ACTIVATE, "More");
	check(view_row(&v, "Unlocks") != NULL && view_row(&v, "Save files") != NULL,
	      "then the pages every game has, Unlocks and Save files among them");
	menu_model_reset(8, 0, MBOX_CAP_STEPS | MBOX_CAP_BACK);
	menu_model_view(&v);
	while (view_row(&v, d->groups[best]) == NULL && v.rows[v.nrows - 1].kind == MROW_MORE)
		act(&v, MBOX_EV_ACTIVATE, "More");
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
	u32 mb, seq0, n0, total = 0, done = 0;
	u8 err = 0;
	int i;

	group("console menu: the install and the mailbox");

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 9 && d->ncaves == 4,
	      "Deadlocked has a renderer: the mailbox, the state, the read-only data, the code, nine hook words");
	check(d != NULL && d->caves[0].zeros > 2000 && d->caves[1].zeros > 3000 && d->caves[0].len < 64 &&
	      d->caves[1].len < 64, "the mailbox and the state are carried as a few bytes and a count of zeros");
	check(mh_desc_for_game(GAME_RAC1) == NULL && mh_desc_for_game(GAME_NONE) == NULL,
	      "RaC1 has none yet, and no game none");
	if (d == NULL) return;
	for (i = 0; i < d->nhooks; i++) {
		if (d->hooks[i].addr >= 0x003A6858u && d->hooks[i].addr < 0x003A6F74u) break;
	}
	check(i == d->nhooks, "none of them in MenuMain_StartOptions: Options is the game's own list");
	check(d->hooks[7].addr == MH_SITE_HELP_LABEL && d->hooks[8].addr == MH_SITE_HELP_SCREEN &&
	      d->hooks[8].value == 0x38800024u, "the last two are the Help row's label call and its li r4,36");

	/* The install, a few seconds into the session and only in play. */
	check(menu_install_delay_us() == 3000000ull, "the install is due three seconds into a session");
	check(dl_boot(), "Deadlocked boots");
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_BOX) == 0,
	      "two seconds in: nothing of the menu in the game yet");
	poke32(DL_GAME_MODE, 3);
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "four, in the game's pause: still nothing");
	poke32(DL_GAME_MODE, 0);
	for (i = 0; i < 40 && menu_state() != MENU_READY; i++) {
		host_write_log_reset();
		tick(1);
	}
	check(menu_state() == MENU_READY && hooks_are(d, 1) && i <= 31,
	      "back in play, within a quarter of a second the renderer goes in, every hook word with it");
	check(peek8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "and nothing asks it to open");
	{
		u32 k, a = 0, l = 0, n = host_write_log_count(), bytes = 0;
		u32 at_box = 999, at_data = 999, at_code = 999, hook_first = 999, hook_last = 0, at_page = 999;
		u8 h;

		for (k = 0; k < n && k < 64; k++) {
			host_write_log_at(k, &a, &l);
			bytes += l;
			if (a == MH_BOX && at_box == 999) at_box = k;
			else if (a == MH_DATA && at_data == 999) at_data = k;
			else if (a == MH_BOX + MBOX_OFF_PAGE_KEY && at_page == 999) at_page = k;
			else if (a == MH_CAVE) at_code = k;
			for (h = 0; h < d->nhooks; h++) {
				if (a != d->hooks[h].addr) continue;
				if (k < hook_first) hook_first = k;
				hook_last = k;
			}
		}
		check(at_box < at_page && at_data < at_page && at_page < at_code && at_code < hook_first &&
		      hook_last == n - 1,
		      "in the order that keeps the game off a half-written renderer: the mailbox and the data, "
		      "the first page, the code, the hook words");
		check(n <= 20 && bytes <= 18u * 1024u, "twenty writes at most, under 18 KB in all");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	check(peek32(DL_OPTIONS_SITE) == DL_OPTIONS_WORD, "Options' call to SetMenuButtons keeps the game's word");

	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek16(mb + MBOX_OFF_VERSION) == 2 &&
	      peek8(mb + MBOX_OFF_ROW_CAP) == 16 && peek8(mb + MBOX_OFF_ROW_SIZE) == 0x80 &&
	      peek8(mb + MBOX_OFF_MAX_ROWS) == 8 && peek8(mb + MBOX_OFF_CFG_ROWS) == 16 &&
	      peek8(mb + MBOX_OFF_CAPS) == MH_CAPS,
	      "the mailbox: magic, version 2, sixteen rows of 0x80, eight a MAIN page, sixteen a CONFIG page");
	check(peek32(MH_STATE) == MH_MAGIC, "the renderer's state, its magic in place");

	read_box(&p);
	check(p.seq != 0 && (p.seq & 1) == 0 && p.ptype == MBOX_PTYPE_MAIN && strcmp(p.title, "RaCMAN") == 0 &&
	      p.n == 7 && (p.pflags & MBOX_PAGE_ROOT) != 0 && strcmp(p.rows[0].label, "Game") == 0,
	      "the root page is in, page_seq even, a MAIN page");

	/* Events and the page type. */
	box_event(MBOX_EV_ACTIVATE, 1);
	check(!box_acked(), "an event posted is not acked yet");
	tick(4);
	read_box(&p);
	check(box_acked() && strcmp(p.title, "Cheats") == 0 && p.n == 5 && p.ptype == MBOX_PTYPE_CONFIG,
	      "within a poll it is acked, and the Cheats page is posted as a CONFIG page");
	check(strcmp(p.rows[1].desc, "Auto also turns it on whenever the game starts.") == 0,
	      "each row with its description at +0x40");
	seq0 = p.seq;
	tick(20);
	read_box(&p);
	check(p.seq == seq0, "a page that has not changed is not written again");

	i = box_index(&p, "Ghost Ratchet");
	box_event(MBOX_EV_ACTIVATE, i);
	tick(4);
	read_box(&p);
	check(box_acked() && (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0 && strcmp(p.rows[i].value, "On") == 0,
	      "the cross on Ghost Ratchet turns it on, and the page says so");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	features_set(R4_GHOST, 0);
	tick(13);
	read_box(&p);
	check(strcmp(p.rows[i].value, "Off") == 0, "a toggle set elsewhere shows within a tenth of a second while open");

	/* A page whose rows do not move is not built again on the timer. */
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	box_activate("Save files");
	read_box(&p);
	seq0 = p.seq;
	host_write_log_reset();
	tick(60);
	check(strcmp(p.title, "Save files") == 0 && host_write_log_count() == 0,
	      "Save files, a directory listing, is built once and not again ten times a second");
	box_event(MBOX_EV_BACK, 0);
	tick(2);

	/* The seqlock. */
	host_write_log_reset();
	box_activate("Cheats");
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

	/* The combo. */
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
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "with every combo switched off it still opens the menu");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	config_set_combo_enabled(1);
	tick(2);
	config_set_combo(COMBO_OPEN_MENU, 0);
	tick(2);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_NONE && menu_state() == MENU_READY,
	      "combo.menu = 0 turns the combo off, and the menu stays in for the Help row");
	config_set_combo(COMBO_OPEN_MENU, COMBO_MENU_DEFAULT);
	tick(2);

	/* CLOSES actions of each kind: asked to close first, run once the game plays. */
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_activate("Game");
	box_activate("Load set-aside file");
	read_box(&p);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE &&
	      strcmp(p.rows[box_index(&p, "Load set-aside file")].value, "after closing") == 0,
	      "loading the set-aside file asks the menu to close first");
	check(peek8(0x015CD71Eu) == 0, "and asks the game for nothing yet");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	poke32(DL_GAME_MODE, 3);
	tick(8);
	check(peek8(0x015CD71Eu) == 0, "not while the game is still paused");
	poke32(DL_GAME_MODE, 0);
	tick(8);
	check(peek8(0x015CD71Eu) == 1, "once it plays again the load goes out, as LOAD_ASIDE sends it");
	poke8(0x015CD71Eu, 0);
	tick(40);

	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	for (i = 1; i <= 15; i++) dl_planet((u8)i, i == 1 || i == 2);
	box_activate("Planets");
	poke32(RAC4_TARGET_PLANET, 0);
	box_activate("Catacrom");
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE && peek32(RAC4_TARGET_PLANET) == 0,
	      "Catacrom: the menu is asked to close, the planet not loaded yet");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 2 && peek32(RAC4_LOAD_PLANET2) == 1,
	      "closed and in play: Catacrom loads, as PLANET_LOAD loads it");
	poke32(RAC4_LOAD_PLANET2, 0);
	tick(40);

	save_put("menu-test", "only.sav", 0x00100000u);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	box_activate("Save files");
	box_activate("menu-test");
	box_activate("only");
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "a save of the library: the menu is asked to close");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(2);
	savefile_transfer(&done, &total, &err);
	check(total == 0x00100000u, "closed: its RESTORE starts, the file coming into the aside buffer");
	tick(120);
	save_drop("menu-test", "only.sav");
	save_drop("menu-test", NULL);

	n0 = host_notify_count();
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	box_activate("Game");
	box_activate("Load set-aside file");
	tick(5 * 120 + 10);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "dropped") != NULL,
	      "a menu that never closes drops the action after five seconds, and says so");
	box_event(MBOX_EV_BACK, 0);
	poke8(mb + MBOX_OFF_REQUEST, 0);
	tick(4);

	/* menu.enabled = 0: every word back once it is safe, the Help row's too. */
	config_set_u32("menu.enabled", 0);
	tick(4);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "menu.enabled = 0 with the menu up leaves the hooks in");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	poke32(DL_GAME_MODE, 3);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "and with the game paused");
	poke32(DL_GAME_MODE, 0);
	poke32(DL_MENU_SCREEN, MH_SCREEN_ID);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "and with screen 36 the current one");
	poke32(DL_MENU_SCREEN, 0);
	tick(8);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_SITE_HELP_LABEL) == MH_ORIG_HELP_LABEL &&
	      peek32(MH_SITE_HELP_SCREEN) == MH_ORIG_HELP_SCREEN,
	      "the menu down, the game in play: every word back, Help its own row again");
	tick(12 * 120);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "and nothing goes back in while it is 0");
	n0 = host_notify_count();
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "menu.enabled") != NULL && hooks_are(d, 0),
	      "the combo says the menu is switched off, and installs nothing");
	config_set_u32("menu.enabled", 1);
	tick(4);
	check(menu_state() == MENU_READY && hooks_are(d, 1),
	      "menu.enabled = 1: the renderer goes back in at once, the game being in play");

	/* Taken over: what a module loaded again into a running game finds. */
	poke32(MH_STATE + ST_STARTS, 7);
	poke32(mb + MBOX_OFF_EV_SEQ, peek32(mb + MBOX_OFF_EV_ACK) + 1);
	menu_enter(session_game());
	check(menu_state() == MENU_IDLE, "a module loaded again starts idle");
	check(dl_installed() && peek32(MH_STATE + ST_STARTS) == 7 && box_acked(),
	      "and takes a renderer of this build over when due, its state as it was, an event left answered");

	/* Refused: another mod at a site. */
	check(quit(), "quit");
	host_boot("NPEA00423");
	poke32(MH_SITE_HELP_LABEL, 0x48000001u);
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	dl_playing();
	tick(4);
	check(menu_state() == MENU_IDLE, "a session starts idle");
	tick(4 * 120);
	check(menu_state() == MENU_REFUSED && peek32(MH_BOX) == 0 && peek32(MH_SITE_FRAME) == MH_ORIG_FRAME &&
	      peek32(MH_CAVE) == 0, "its install finds the Help row's call holding another mod's word, and writes nothing");
	n0 = host_notify_count();
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_REFUSED && host_notify_count() == n0 + 1 && strstr(host_notify_last(), "another mod") != NULL,
	      "the combo tries again at once, and says why");
	poke32(MH_SITE_HELP_LABEL, MH_ORIG_HELP_LABEL);
	tick(11 * 120);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "ten seconds on, the word the game's again, it goes in");

	/* Refused: a renderer of another build. */
	poke8(MH_CAVE + 0x100, (u8)(peek8(MH_CAVE + 0x100) ^ 0xFF));
	menu_enter(session_game());
	tick(4 * 120);
	check(menu_state() == MENU_REFUSED, "hooks that are ours over code that is not this build's are left alone");
	check(quit(), "quit Deadlocked");

	/* The combo before the install is due puts it in at once. */
	menu_set_install_delay_us(60000000ull);
	check(dl_boot(), "Deadlocked boots");
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_READY && hooks_are(d, 1) && peek8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "a press before the install is due installs at once and asks the renderer to open");
	menu_set_install_delay_us(3000000ull);
	check(quit(), "quit Deadlocked");

	/* RPCS3: nothing at all. */
	host_set_can_patch_code(0);
	check(boot("NPEA00423"), "Deadlocked boots under RPCS3");
	dl_playing();
	tick(4 * 120);
	check(menu_state() == MENU_OFF && peek32(MH_BOX) == 0 && hooks_are(d, 0), "the menu is off and writes nothing");
	n0 = host_notify_count();
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "RPCS3") != NULL && hooks_are(d, 0),
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
}

/* ----------------------------------------------------------- the renderer */

static void test_renderer(void)
{
	char text[80], title[40];
	u32 mb;
	int i, n, row;

	group("console menu: Deadlocked's renderer, core and shim, on the fake console");

	memset(&G, 0, sizeof(G));
	G.input_ok = 1;
	G.handles = 0xF0000000u;
	check(dl_boot() && dl_installed(), "Deadlocked up and the renderer in, three seconds into the session");
	{
		const char *sel = "\x10 SELECT", *back = "\x12 BACK";
		for (i = 0; i <= (int)strlen(sel); i++) w8(MSG_AT + (u32)i, (u8)sel[i]);
		for (i = 0; i <= (int)strlen(back); i++) w8(MSG_AT + 0x20 + (u32)i, (u8)back[i]);
	}
	poke32(DL_PAD0_PTR, PAD0);
	mb = menu_mailbox();

	/* The Help row: the pause main menu's fifth row opens the menu, back returns there. */
	game_open_pause();
	check(G.main_up && G.screen == 1, "Start: the game's own main menu");
	game_help_row();
	check(G.screen == MH_SCREEN_ID && G.shown && !G.main_up && G.depth == 1 && G.stack[0] == 1,
	      "its Help row pushes screen 36 over the main menu");
	check(strcmp(read_str(G.title, title, sizeof(title)), "RACMAN") == 0 && G.nbuttons == 7 &&
	      strcmp(shown_text(0, text, sizeof(text)), "GAME") == 0 &&
	      strcmp(shown_text(5, text, sizeof(text)), "SAVE FILES") == 0 &&
	      strcmp(shown_text(6, text, sizeof(text)), "QWARK BUILD 54") == 0,
	      "the root in the main menu's capitals: GAME .. SAVE FILES, then QWARK BUILD 54, under RACMAN");
	check(peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && G.close_cb == MH_FN_CLOSE,
	      "the mailbox says open, the list's close is the shim's");
	game_frame(PADB_TRIANGLE);
	check(G.screen == 1 && G.main_up && G.paused && G.closes == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "triangle at the root pops back to the main menu, still paused, the menu down");
	mock_close_menu(5);
	tick(8);

	/* The combo opens the root from play, with nothing under it. */
	G.depth = 2;
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "the combo asks the renderer to open");
	game_hud_frame();
	check(G.screen == MH_SCREEN_ID && G.shown && G.paused && G.depth == 0 && peek8(mb + MBOX_OFF_REQUEST) == 0 &&
	      peek32(MH_STATE + ST_OPENS) == 1, "the next GuiHud frame opens screen 36 through the Start path");
	tick(4);

	/* The frame fits the page: SetMenuTarget again when the number of rows changes. */
	n = G.targets;
	G.cursor = 2;
	game_frame(PADB_CROSS);
	check(G.sound == SND_FORWARD && !box_acked(), "the cross on PLANETS posts an event, with the forward sound");
	tick(2);
	game_frame(0);
	check(strcmp(read_str(G.title, title, sizeof(title)), "PLANETS") == 0 && G.targets == n + 1,
	      "the next frame shows PLANETS, its rows laid out again and the frame fitted to them");
	n = G.nbuttons;
	game_frame(PADB_TRIANGLE);
	tick(2);
	game_frame(0);
	check(G.nbuttons == 7 && G.targets > n && G.cursor == 2, "back to the root: seven rows, refitted, the cursor on PLANETS");
	n = G.targets;
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(30);
	game_frame(0);
	check(G.targets == n, "a page with as many rows is not laid out again");

	/* The two-column page: a CONFIG page starts the screen again with the CtlList. */
	G.cursor = 1;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(!G.shown && G.stops >= 1 && G.thread == 1 && G.screen == MH_SCREEN_ID,
	      "the cross on CHEATS: the list stops, screen 36 starts again with the shim's thread");
	game_frame(0);
	check(G.thread == 2 && G.screen_tpl == DL_TPL_SCREEN && G.list_tpl == DL_TPL_ROW && G.count == 5 &&
	      G.layer == DL_LAYER && G.images == 3 && G.cursor_attached,
	      "the thread's first frame builds the Cheats screen's panel and list, five rows, its plates and cursor");
	check(r32(MH_SHIM + SH_LIST) == DL_CTLLIST_VTABLE, "the list has CtlList's vtable at +0, as its constructor writes it");
	check(strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_TITLE, title, sizeof(title)), "CHEATS") == 0 &&
	      strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_LEGEND, text, sizeof(text)), "\x10 SELECT \x12 BACK") == 0,
	      "titled CHEATS, with the Cheats screen's own button legend");
	check(strcmp(cfg_label(1, text), "Ghost Ratchet") == 0 && strcmp(cfg_value(1, title), "OFF") == 0 &&
	      strcmp(cfg_label(4, text), "Unlock All Planets") == 0 && strcmp(cfg_value(4, title), "...") == 0,
	      "labels in the Cheats screen's title case, values in its capitals, a button's \"...\"");
	check(focus() == 0 && cfg_color(0) == DL_RGBA_FOCUS && cfg_color(1) == DL_RGBA_OFF,
	      "the cursor on the first row, which is lit; the others the game's grey");
	game_frame(0);
	check(strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_DESC, text, sizeof(text)), "Auto also turns it on whenever the game starts.") == 0,
	      "the description line is the row's");

	game_frame(PADB_DOWN);
	check(focus() == 1 && G.cursor_moves > 0, "down moves the list's focus, and the cursor frame with it");
	game_frame(PADB_RIGHT);
	check(G.sound == SND_CHANGE && !box_acked(), "Right on Ghost Ratchet posts an event, with the Cheats screen's sound");
	tick(2);
	game_frame(0);
	check(strcmp(cfg_value(1, text), "ON") == 0 && cfg_color(1) == DL_RGBA_FOCUS &&
	      (features_toggle_state() & ((u64)1 << R4_GHOST)) != 0, "the cheat on, the row ON");
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(cfg_value(1, text), "AUTO") == 0, "the cross: AUTO");
	game_frame(PADB_UP);
	game_frame(0);
	check(focus() == 0 && cfg_color(1) == DL_RGBA_ACTIVE, "off the row, an active cheat is the game's gold");
	game_frame(PADB_LEFT);
	tick(2);
	game_frame(0);
	game_frame(PADB_DOWN);
	game_frame(PADB_LEFT);
	tick(2);
	game_frame(0);
	game_frame(PADB_LEFT);
	tick(2);
	game_frame(0);
	check(strcmp(cfg_value(1, text), "OFF") == 0 && (features_toggle_state() & ((u64)1 << R4_GHOST)) == 0,
	      "Left twice from AUTO: ON, then OFF");
	for (i = 0; i < 3; i++) game_frame(PADB_DOWN);
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(focus() == 4 && strcmp(cfg_value(4, text), "DONE") == 0 &&
	      strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_DESC, title, sizeof(title)), "Unlock all planets: done") == 0,
	      "the button runs: DONE in its value, the sentence on the description line");

	/* One event in flight, and the waiting title. */
	game_frame(PADB_CROSS);
	game_frame(PADB_CROSS);
	check(peek32(MH_STATE + ST_DROPPED) >= 1, "a second press before qwark answers is dropped");
	for (i = 0; i < 31; i++) game_frame(0);
	check(strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_TITLE, title, sizeof(title)), "WAITING FOR QWARK...") == 0,
	      "half a second unanswered and the title says so");
	tick(2);
	game_frame(0);
	check(strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_TITLE, title, sizeof(title)), "CHEATS") == 0,
	      "the answer puts it back");

	/* Back to the list: the thread ends and the screen starts again with GuiSupport's list. */
	game_frame(PADB_TRIANGLE);
	check(G.sound == SND_BACK, "triangle on the page posts back");
	tick(2);
	game_frame(0);
	game_frame(0);
	game_frame(0);
	check(G.thread == 0 && G.layer == 0 && G.images == 0 && !G.cursor_attached && G.shown &&
	      G.screen == MH_SCREEN_ID && G.depth == 0,
	      "the thread ends - the panel cleared, the plates dropped, the cursor detached - and the list is back");
	check(strcmp(read_str(G.title, title, sizeof(title)), "RACMAN") == 0 && G.cursor == 1,
	      "on the root, the cursor on CHEATS");

	/* Closes: Start from the CONFIG page, and a close request. */
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	game_frame(0);
	n = G.closes;
	game_frame(PADB_START);
	check(G.closes == n + 1 && G.screen == 0 && !G.paused && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "Start on the CONFIG page closes the menu and unpauses");
	game_frame(0);
	game_frame(0);
	check(G.thread == 0 && G.layer == 0, "and its thread ends behind it");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	game_frame(0);
	check(G.thread == 2 && G.count == 5, "the combo opens the menu again on the page it was closed on");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo again asks it to close");
	game_frame(0);
	check(G.screen == 0 && !G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0, "which the thread's next frame does");
	game_frame(0);
	game_frame(0);
	check(G.thread == 0 && G.layer == 0, "the thread ends in the frames after, as the game plays on");
	tick(4);

	/* A MAIN page with a choice list, through the Unlocks page's buttons and Select. */
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	game_frame(0);
	game_frame(PADB_TRIANGLE);
	tick(4);
	for (i = 0; i < 4; i++) game_frame(0);
	row = shown_row("UNLOCKS");
	G.cursor = row;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(read_str(G.title, title, sizeof(title)), "UNLOCKS") == 0 && shown_row("MAX ALL WEAPON AMMO") == 4,
	      "UNLOCKS lists the tabs and the two buttons");
	G.cursor = 4;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(shown_row("MAX ALL WEAPON AMMO: DONE") == 4, "a MAIN button says DONE beside its label");
	game_frame(PADB_SELECT);
	check(G.screen == 0 && !G.paused, "Select closes the menu");
	tick(4);

	/* A page qwark is still writing is left for the next frame. */
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
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

	check(quit(), "quit Deadlocked");
}

void test_menu(void)
{
	test_model_dl();
	test_model_paging();
	test_mailbox();
	test_renderer();
	test_menu_core();
}
