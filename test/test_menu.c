/*
 * The console menu (revision 1.22, mailbox version 3), on qwark's fake console.
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
#define NTEXT 1024
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
	int sounds[8];           /* the last sounds played, newest first */
	u32 text_h[NTEXT], text_s[NTEXT];
	u32 color_h[NTEXT], color_c[NTEXT];
	u32 vis_h[NTEXT], vis_v[NTEXT];
	u32 rect_h[NTEXT];
	float rect_v[NTEXT][4];
	u32 fill_h, fill_c0, fill_c1;
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
static void mock_play_sound(int id)
{
	int i;
	for (i = 7; i > 0; i--) G.sounds[i] = G.sounds[i - 1];
	G.sounds[0] = id;
	G.sound = id;
}
static int mock_get_command(int kind) { int c = G.cmd; (void)kind; G.cmd = 0; return c; }
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
static void mock_set_visible(u32 h, int on) { remember(G.vis_h, G.vis_v, h, on ? 1u : 2u); }
static void mock_set_rect(u32 h, float l, float t, float r, float b)
{
	int i;
	for (i = 0; i < NTEXT; i++) {
		if (G.rect_h[i] == h || G.rect_h[i] == 0) {
			G.rect_h[i] = h;
			G.rect_v[i][0] = l; G.rect_v[i][1] = t; G.rect_v[i][2] = r; G.rect_v[i][3] = b;
			return;
		}
	}
}
static void mock_vertex_colors(u32 h, u32 c0, u32 c1, u32 c2, u32 c3)
{
	(void)c2; (void)c3;
	G.fill_h = h; G.fill_c0 = c0; G.fill_c1 = c1;
}
static u32 mock_team_color(int team, int i) { return 0x80000000u | ((u32)team << 8) | (u32)i; }
static void mock_close_menu_p(int p, int sound);

/*
 * Worlds, screen 3, the planet picker (mailbox version 3), as far as the
 * renderer sees it: its start and stop (MenuPlanet_Start plays 5, its stop 6),
 * the active mission its Triangle and Start test, and the game's own X
 * (HandleSelection), counted. Its entries are in game memory, as
 * SetupPlanetTourneyList writes them (worlds_entry below).
 */
static struct {
	int up, starts, game_stops, selections;
	int mission;             /* mf_activeMissionIndex: -1, the trap */
} W;

static void mock_push_screen(int p, int s);
static int mock_next_screen(int p) { (void)p; return G.next; }
static void mock_sound_flag(void) { w8(DL_MENU_SOUND_FLAG, 1); }
static void mock_worlds_stop(int p) { (void)p; W.up = 0; W.game_stops++; mock_play_sound(6); }

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
	else if (s == DL_SCREEN_WORLDS) { W.up = 1; W.starts++; mock_play_sound(5); }
}

static void mock_start_next(int p) { (void)p; start_screen(G.next); }

/* Worlds' fcStop is the renderer's (MH_WORLDS_STOP) while it is installed, as here. */
static void stop_screen(void)
{
	if (G.screen == MH_SCREEN_ID) mh_stop(0);
	else if (G.screen == 1) { G.main_up = 0; G.shown = 0; mock_start_next(0); }
	else if (G.screen == DL_SCREEN_WORLDS) mh_worlds_stop(0);
}

/* GuiMenu_PushScreen: the screen it leaves onto the stack, its stop, the next one current. */
static void mock_push_screen(int p, int s)
{
	(void)p;
	G.stack[G.depth++] = G.screen;
	G.next = s;
	stop_screen();
	if (G.screen != s) {
		G.screen = s;
		if (s == DL_SCREEN_WORLDS) start_screen(s);
	}
}

/* GuiMenu_PopScreen: the next screen the one under, the stop, then its start; none under closes. */
static void mock_close_menu(int sound);
static void mock_pop_screen(void)
{
	if (G.depth < 1) {
		mock_close_menu(-1);
		return;
	}
	G.next = G.stack[--G.depth];
	stop_screen();
	start_screen(G.next);
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
	w8(DL_MENU_SOUND_FLAG, 1);
	if (sound >= 0) mock_play_sound(sound);
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
	mock_play_sound(SND_BACK);
	if (G.depth < 1) {
		mock_close_menu(-1);
		return;
	}
	mock_go_to_screen(0, G.stack[--G.depth]);
}

static void mock_close_menu_p(int p, int sound) { (void)p; mock_close_menu(sound); }

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
 * pushes screen 36 (mh_rac4.h, MH_SITE_HELP_*), keeping the main menu's cursor,
 * having cleared g_bMenuExit as each of the main menu's row callbacks does.
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
	w8(DL_MENU_EXIT, 0);
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

/*
 * Worlds' list as SetupPlanetTourneyList and UpdatePlanets leave it: entry i
 * {u8 bLocked, u8 complete, u8 bNew, pad, s32 iLink, s32 type} at the game's
 * table, item i of player 0's CtlList naming it, `count` items, the focus on
 * `focus`.
 */
#define WORLDS_ITEMS   0x011A2000u
#define WORLDS_ENTRIES 0x011A1374u

static void worlds_entry(int i, int locked, int link, int type)
{
	u32 e = WORLDS_ENTRIES + 12u * (u32)i;

	w8(e, (u32)locked);
	w8(e + 1, 0);
	w8(e + 2, 0);
	w8(e + 3, 0);
	w32(e + 4, (u32)link);
	w32(e + 8, (u32)type);
	w32(WORLDS_ITEMS + DL_ITEM_SIZE * (u32)i + 8, e);
}

static void worlds_focus(int count, int focus)
{
	w32(DL_PLANET_PD + DL_PLANET_ITEMS, WORLDS_ITEMS);
	w32(DL_PLANET_PD + DL_PLANET_COUNT, (u32)count);
	w32(DL_PLANET_PD + DL_PLANET_FOCUS, (u32)focus);
}

/*
 * A press on Worlds, MenuPlanet_HandleInput with the renderer's words in: X is
 * mh_select_stub (mh_worlds_select, else the game's HandleSelection), Triangle
 * and Start pass their gates (mh_gate_back, mh_gate_close: a mission active,
 * or the picker up; check.py steps the real ones), Circle pushes Skills but
 * in the menu's Worlds (mh_gate_skills, the pickers' round 2).
 */
static void worlds_press(int cmd)
{
	if (G.screen != DL_SCREEN_WORLDS) return;
	switch (cmd) {
	case DL_CMD_CROSS:
		if (!mh_worlds_select()) W.selections++;
		break;
	case DL_CMD_TRIANGLE:
		if (W.mission != -1 || mh_picker_up()) {
			w8(DL_PLANET_FROM + 1, 0);
			mock_pop_screen();
		}
		break;
	case DL_CMD_CLOSE:
		if (W.mission != -1 || mh_picker_up()) {
			mock_sound_flag();
			w8(DL_PLANET_FROM, 0);
			w8(DL_PLANET_FROM + 1, 0);
			w8(DL_PLANET_FROM + 2, 0);
			mock_close_menu_p(0, -1);
		}
		break;
	case 4:
		if (!mh_picker_up()) mock_push_screen(0, 21);
		break;
	default:
		break;
	}
}

/*
 * UpdatePlanets' bLocked for entry i, a planet whose level the save has
 * `unlocked` or not, with the renderer's word at its store (mh_gate_locked,
 * the pickers' round 2; check.py steps the real one): 0 in the menu's Worlds.
 */
static void worlds_lock(int i, int unlocked)
{
	w8(WORLDS_ENTRIES + 12u * (u32)i, (!unlocked && !mh_picker_up()) ? 1u : 0u);
}

/*
 * Round 6: whether UpdatePlanets lists an entry of SetupPlanetTourneyList's,
 * with the renderer's word at its type test (mh_gate_tourney; check.py steps
 * the real one): every planet, and tournament 0, DreadZone Station's own row;
 * tournaments 1 to 4 only in the game's own Worlds.
 */
static int worlds_listed(int link, int type)
{
	return type == DL_TYPE_PLANET || link == 0 || !mh_picker_up();
}

/*
 * Round 6: the legend MenuPlanet_CreateWidgets writes as Worlds starts, with
 * the renderer's word at its mission test (mh_gate_legend; check.py steps the
 * real one): SELECT and BACK in the menu's Worlds, the game's otherwise.
 */
static const char *worlds_legend(void)
{
	if (mh_picker_up()) return "\x10 SELECT \x12 BACK";
	return W.mission == -1 ? "\x11 SKILLS \x10 SELECT" : "\x11 SKILLS \x10 SELECT \x12 BACK";
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
	struct { u8 kind, flags; u16 id; char label[MROW_LABEL_LEN + 1]; char value[MROW_VALUE_LEN + 1]; } rows[32];
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
	for (i = 0; i < p->n && i < MH_ROWS; i++) {
		const u8 *r = buf + MBOX_OFF_ROWS + i * MH_ROW_BYTES;
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

/*
 * Round 5: no game lists a VALUE as a stepper any more, so the steppers and
 * sliders, still the controls of round 1's pages of DESCRIBE groups, are
 * tried on a menu table of the test's own: Deadlocked's three values.
 */
static const u8 tv_items[] = { R4_BOLTS, R4_DREAD_POINTS, R4_CHALLENGE };
/* Round 5: spacers that would open the page, follow another or end it are left out. */
static const u8 tv_spaced[] = {
	MENU_ITEM_SPACER, R4_BOLTS, MENU_ITEM_SPACER, MENU_ITEM_SPACER, R4_SKIN, MENU_ITEM_SPACER,
	MENU_ITEM_VALUE(7), MENU_ITEM_SPACER
};
static const struct menu_section tv_sections[] = {
	{ "Values", tv_items, (u8)sizeof(tv_items) },
	{ "Spacers", tv_spaced, (u8)sizeof(tv_spaced) }
};
static struct game_menu tv_menu;

static void tv_menu_on(void)
{
	memset(&tv_menu, 0, sizeof(tv_menu));
	tv_menu.sections = tv_sections;
	tv_menu.nsections = 2;
	menu_test_menu(&tv_menu);
}

static void test_model_dl_values(void)
{
	struct menu_view v;
	const struct menu_row *r;
	u8 i;

	tv_menu_on();
	poke32(RAC4_BOLTS, 0);
	poke8(RAC4_CHALLENGE_MODE, 0);
	tick(10);
	menu_model_reset(8, 16, CAPS_ALL);
	menu_model_view(&v);
	check(act(&v, MBOX_EV_ACTIVATE, "Values") == ST_OK && v.nrows == 3, "a page of the three values as steppers");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_VALUE && strcmp(r->value, "0") == 0, "Bolts is a stepper reading 0");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 1, "Right adds one");
	act(&v, MBOX_EV_ACTIVATE, "Bolts");
	check(strcmp(value_of(&v, "Bolts"), "1 [10]") == 0, "the cross makes the step 10");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC4_BOLTS) == 11, "and Right adds ten");
	for (i = 0; i < 10 && strcmp(value_of(&v, "Bolts"), "11") != 0; i++) act(&v, MBOX_EV_ACTIVATE, "Bolts");
	check(strcmp(value_of(&v, "Bolts"), "11") == 0, "the cross on round the steps back to 1");
	r = view_row(&v, "Challenge mode");
	check(r != NULL && (r->flags & (MROWF_RANGE | MROWF_SLIDER)) == (MROWF_RANGE | MROWF_SLIDER) &&
	      r->range[0] == 0 && r->range[1] == 0 && r->range[2] == 255 && r->range[3] == 26 &&
	      strcmp(r->value, "0") == 0,
	      "Challenge mode carries its slider's numbers, 0 in 0..255, and (round 4) a step of a tenth, 26");
	for (i = 0; i < 9; i++) act(&v, MBOX_EV_RIGHT, "Challenge mode");
	check(strcmp(value_of(&v, "Challenge mode"), "234") == 0 && peek8(RAC4_CHALLENGE_MODE) == 234,
	      "nine presses of Right: 234");
	act(&v, MBOX_EV_RIGHT, "Challenge mode");
	check(strcmp(value_of(&v, "Challenge mode"), "255") == 0 && peek8(RAC4_CHALLENGE_MODE) == 255,
	      "the tenth: the top, 255, ten presses from end to end");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	r = view_row(&v, "Challenge mode");
	check(r != NULL && r->range[3] == 1 && strcmp(r->value, "255 [1]") == 0,
	      "the cross: the fine step, 1, which the value shows");
	act(&v, MBOX_EV_LEFT, "Challenge mode");
	check(peek8(RAC4_CHALLENGE_MODE) == 254, "Left by one");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	r = view_row(&v, "Challenge mode");
	check(r != NULL && r->range[3] == 26 && strcmp(r->value, "254") == 0, "the cross again: back to the tenth");
	for (i = 0; i < 12; i++) act(&v, MBOX_EV_LEFT, "Challenge mode");
	check(strcmp(value_of(&v, "Challenge mode"), "0") == 0, "and Left to the bottom, where it stops");
	r = view_row(&v, "Bolts");
	check(r != NULL && (r->flags & (MROWF_RANGE | MROWF_SLIDER)) == 0,
	      "Bolts, a whole 32-bit counter, has no bounds to carry and no slider");
	menu_model_reset(8, 0, 0);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Values");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_INFO && r->flags == 0, "with no Left and Right a value is shown and nothing more");

	menu_model_reset(8, 16, CAPS_ALL);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Spacers");
	check(v.nrows == 3 && strcmp(v.rows[0].label, "Bolts") == 0 && v.rows[1].kind == MROW_INFO &&
	      v.rows[1].flags == 0 && v.rows[1].label[0] == 0 && strcmp(v.rows[2].label, "Skin") == 0,
	      "round 5: a spacer opening the page, one after another and one ending it are left out, as is a "
	      "MENU_ITEM_VALUE the table does not have: Bolts, a spacer, Skin");
	menu_test_menu(NULL);
}

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

	check(strcmp(v.title, "Trainer") == 0 && (v.pflags & MBOX_PAGE_ROOT) != 0 && v.ptype == MBOX_PTYPE_MAIN,
	      "the root is a MAIN page titled Trainer (round 3) that says it is the root");
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

	/*
	 * Cheats: four checkboxes of three ways, then the buttons: Max bolts, Max
	 * Dread points, Unlock all planets (round 6: Challenge mode is on Game).
	 */
	check(act(&v, MBOX_EV_ACTIVATE, "Cheats") == ST_OK && strcmp(v.title, "Cheats") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 7, "Cheats is a CONFIG page of seven rows");
	check(strcmp(v.rows[0].label, "Fast loads") == 0 && strcmp(v.rows[1].label, "Ghost Ratchet") == 0 &&
	      strcmp(v.rows[2].label, "Refill ammo") == 0 && strcmp(v.rows[3].label, "Freeze health") == 0 &&
	      strcmp(v.rows[4].label, "Max bolts") == 0 && strcmp(v.rows[5].label, "Max Dread points") == 0 &&
	      strcmp(v.rows[6].label, "Unlock all planets") == 0 && view_row(&v, "Challenge mode") == NULL,
	      "round 6: the four cheats, then Max bolts, Max Dread points and Unlock all planets; no Challenge mode");
	r = view_row(&v, "Ghost Ratchet");
	check(r != NULL && r->kind == MROW_TOGGLE && r->flags == (MROWF_ENABLED | MROWF_STEPS) &&
	      strcmp(r->value, "Off") == 0, "Ghost Ratchet: Off, a three-way checkbox");
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
	      "Unlock all planets is a button with nothing to say");
	check(act(&v, MBOX_EV_ACTIVATE, "Unlock all planets") == ST_OK &&
	      value_of(&v, "Unlock all planets")[0] == 0 && v.hint[0] == 0 && peek32(RAC4_DREAD_POINTS) == 1000000,
	      "round 5: the cross runs it, and the row stays blank (no DONE: the game's sound says it)");

	/* Round 5: the values as buttons. */
	r = view_row(&v, "Max bolts");
	check(r != NULL && r->kind == MROW_ACTION && r->flags == MROWF_ENABLED && r->value[0] == 0,
	      "Max bolts is a button");
	poke32(RAC4_BOLTS, 1234);
	check(act(&v, MBOX_EV_RIGHT, "Max bolts") == ST_OK && peek32(RAC4_BOLTS) == 1234, "Right does nothing on it");
	check(act(&v, MBOX_EV_ACTIVATE, "Max bolts") == ST_OK && peek32(RAC4_BOLTS) == 99999999u &&
	      value_of(&v, "Max bolts")[0] == 0, "the cross: 99,999,999, the game's own cap, the row blank");
	check(act(&v, MBOX_EV_ACTIVATE, "Max Dread points") == ST_OK && peek32(RAC4_DREAD_POINTS) == 99999999u,
	      "Max Dread points: the same");
	check(view_row(&v, "Bolts") == NULL && view_row(&v, "Dread points") == NULL, "no stepper for either");
	rc = menu_model_event(MBOX_EV_PAGE_NEXT, 0, v.rows[0].id, v.key);
	check(rc == ST_BAD_ARG, "R1 on a page of one window does nothing");
	menu_model_event(MBOX_EV_BACK, 1, v.rows[1].id, v.key);
	menu_model_view(&v);
	check(strcmp(v.title, "Trainer") == 0 && v.cursor == 1, "back: the root, the cursor on Cheats");

	/*
	 * Game: the slot and the position buttons, Challenge mode (round 6, the
	 * user's call: "not really a cheat"; round 5 had a spacer there), the
	 * save-file buttons.
	 */
	check(act(&v, MBOX_EV_ACTIVATE, "Game") == ST_OK && strcmp(v.title, "Game") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 6, "Game is a CONFIG page of six rows");
	check(strcmp(v.rows[0].label, "Position slot") == 0 &&
	      strcmp(v.rows[1].label, "Save position") == 0 && strcmp(v.rows[2].label, "Load position") == 0 &&
	      strcmp(v.rows[3].label, "Challenge mode") == 0 && v.rows[3].kind == MROW_TOGGLE &&
	      strcmp(v.rows[4].label, "Set aside file") == 0 && strcmp(v.rows[5].label, "Load set-aside file") == 0,
	      "round 6: the slot, Save and Load position, Challenge mode, the two savefile buttons");
	for (i = 0; i < v.nrows && (v.rows[i].kind != MROW_INFO || v.rows[i].label[0] != 0); i++)
		;
	check(i == v.nrows, "no blank row: Challenge mode is where round 5's spacer was");
	check(view_row(&v, "Die") == NULL && view_row(&v, "Bolts") == NULL && view_row(&v, "Max bolts") == NULL,
	      "no Die (the combos have it) and no other value (Cheats has the buttons)");
	check(view_row(&v, "Skin") == NULL, "and no Skin: cosmetics are left out");
	/* Round 5's switch, on Game since round 6. */
	poke8(RAC4_CHALLENGE_MODE, 0);
	tick(10);
	menu_model_view(&v);
	r = view_row(&v, "Challenge mode");
	check(r != NULL && r->kind == MROW_TOGGLE && r->flags == (MROWF_ENABLED | MROWF_STEPS) &&
	      strcmp(r->value, "Off") == 0, "Challenge mode is a switch, Off on a file not in challenge mode");
	check(act(&v, MBOX_EV_ACTIVATE, "Challenge mode") == ST_OK && peek8(RAC4_CHALLENGE_MODE) == 1 &&
	      strcmp(value_of(&v, "Challenge mode"), "On") == 0 && (view_row(&v, "Challenge mode")->flags & MROWF_ON) != 0,
	      "the cross: On, the counter 1, the first run's");
	check(act(&v, MBOX_EV_RIGHT, "Challenge mode") == ST_OK && peek8(RAC4_CHALLENGE_MODE) == 0 &&
	      strcmp(value_of(&v, "Challenge mode"), "Off") == 0, "Right: Off, the counter 0");
	poke8(RAC4_CHALLENGE_MODE, 7);
	tick(10);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Challenge mode"), "On") == 0, "a file on its seventh run reads On");
	check(act(&v, MBOX_EV_LEFT, "Challenge mode") == ST_OK && peek8(RAC4_CHALLENGE_MODE) == 0 &&
	      strcmp(value_of(&v, "Challenge mode"), "Off") == 0, "and Left turns it Off, 0");
	act(&v, MBOX_EV_LEFT, "Challenge mode");
	check(peek8(RAC4_CHALLENGE_MODE) == 1, "Left again: On, 1");

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
	check(act(&v, MBOX_EV_ACTIVATE, "Save position") == ST_OK && value_of(&v, "Save position")[0] == 0,
	      "Save position keeps where the player stands, and says nothing (round 5)");
	poke32(RAC4_COORDS, 0);
	check(act(&v, MBOX_EV_ACTIVATE, "Load position") == ST_OK && peek32(RAC4_COORDS) == 0x41200000u,
	      "Load position puts the player back");
	r = view_row(&v, "Load set-aside file");
	check(r != NULL && (r->flags & MROWF_CLOSES) != 0, "Load set-aside file closes the menu before it runs");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* The steppers and sliders themselves, on a page of round 1's DESCRIBE groups (Deadlocked's Player). */
	test_model_dl_values();
	menu_model_reset(8, 16, CAPS_ALL);
	menu_model_view(&v);

	/*
	 * Planets: every planet, locked or not, each a button that loads it once
	 * the menu is closed (build 54: a load unlocks a locked planet first, so
	 * round 2's filter is gone).
	 */
	for (i = 1; i <= 15; i++) dl_planet(i, 0);
	dl_planet(1, 1);
	dl_planet(3, 1);
	check(act(&v, MBOX_EV_ACTIVATE, "Planets") == ST_OK && strcmp(v.title, "Planets") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 12,
	      "Planets is a CONFIG page (round 4) of every planet, twelve, the save's locks or not");
	check(strcmp(v.rows[0].label, "DreadZone") == 0 && strcmp(v.rows[1].label, "Catacrom") == 0 &&
	      strcmp(v.rows[2].label, "Sarathos") == 0 && strcmp(v.rows[6].label, "Valix") == 0 &&
	      strcmp(v.rows[11].label, "Interior") == 0,
	      "DreadZone, Catacrom, Sarathos .. Valix .. Interior in the game's order, the placeholders left out");
	check(v.rows[1].kind == MROW_ACTION && (v.rows[1].flags & MROWF_CLOSES) != 0, "each closes the menu first");
	check(act(&v, MBOX_EV_ACTIVATE, "Catacrom") == ST_OK && value_of(&v, "Catacrom")[0] == 0 &&
	      peek32(RAC4_TARGET_PLANET) != 2, "the cross on Catacrom: nothing loads yet, the row blank (round 5)");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	/* The pickers' round 2: Deadlocked names no planets its picker cannot show, so the event opens the whole page. */
	r = view_row(&v, "Planets");
	check(r != NULL && menu_model_event(MBOX_EV_PICKER_OTHERS, (u8)(r - v.rows), r->id, v.key) == ST_OK &&
	      menu_model_page() == MENU_PAGE_ID(MENU_PAGE_PLANETS, 0),
	      "the picker's MBOX_EV_PICKER_OTHERS, in a game with no picker_others: the whole Planets page");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Mailbox version 3: with a renderer that opens the game's own picker, the root's Planets row says so. */
	check((view_row(&v, "Planets")->flags & MROWF_PICKER) == 0, "a renderer without the picker: Planets a plain link");
	menu_model_reset(8, 16, CAPS_ALL | MBOX_CAP_PICKER);
	menu_model_view(&v);
	check(view_row(&v, "Planets")->flags == (MROWF_ENABLED | MROWF_PICKER) &&
	      (view_row(&v, "Unlocks")->flags & MROWF_PICKER) == 0,
	      "one with it: the Planets row flagged PICKER, and no other");
	menu_model_reset(8, 16, CAPS_ALL);
	menu_model_view(&v);

	/* Manips: the category, Auto-reset, Apply now, a spacer, then the Fixes (round 5). */
	check(act(&v, MBOX_EV_ACTIVATE, "Manips") == ST_OK && v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 6,
	      "Manips is a CONFIG page of six rows");
	check(strcmp(v.rows[0].label, "Category") == 0 && strcmp(v.rows[0].value, "None") == 0 &&
	      strcmp(v.rows[1].label, "Auto-reset") == 0 && strcmp(v.rows[1].value, "On") == 0 &&
	      strcmp(v.rows[2].label, "Apply now") == 0 &&
	      v.rows[3].kind == MROW_INFO && v.rows[3].flags == 0 && v.rows[3].label[0] == 0 &&
	      strcmp(v.rows[4].label, "Crash patches") == 0 && strcmp(v.rows[5].label, "Fix reset softlocks") == 0,
	      "Category None, Auto-reset On, Apply now, a blank grey row, Crash patches, Fix reset softlocks");
	check(view_row(&v, "Remember") == NULL, "no Remember row: Auto-reset says it (round 5)");
	check(act(&v, MBOX_EV_RIGHT, "Category") == ST_OK && manips_selected() == 1 &&
	      strcmp(value_of(&v, "Category"), "NG+") == 0, "Right: NG+, selected as MANIP_SELECT selects it");
	check(act(&v, MBOX_EV_ACTIVATE, "Category") == ST_OK && manips_selected() == 0, "the cross: back to None");
	check(manips_options_now() == 1, "MANIP_OPTIONS: auto-reset 1, remember 0, the defaults");
	check(act(&v, MBOX_EV_ACTIVATE, "Auto-reset") == ST_OK && manips_options_now() == 3 &&
	      strcmp(value_of(&v, "Auto-reset"), "On boot") == 0 && (view_row(&v, "Auto-reset")->flags & MROWF_ON) != 0,
	      "the cross on Auto-reset: On boot, auto-reset 1 and remember 1");
	check(act(&v, MBOX_EV_RIGHT, "Auto-reset") == ST_OK && manips_options_now() == 0 &&
	      strcmp(value_of(&v, "Auto-reset"), "Off") == 0 && (view_row(&v, "Auto-reset")->flags & MROWF_ON) == 0,
	      "Right wraps to Off: auto-reset 0, remember 0");
	check(act(&v, MBOX_EV_RIGHT, "Auto-reset") == ST_OK && manips_options_now() == 1 &&
	      strcmp(value_of(&v, "Auto-reset"), "On") == 0, "Right: On, auto-reset 1, remember 0");
	check(act(&v, MBOX_EV_LEFT, "Auto-reset") == ST_OK && manips_options_now() == 0, "Left: Off again");
	act(&v, MBOX_EV_LEFT, "Auto-reset");
	check(manips_options_now() == 3, "Left from Off wraps to On boot");
	check(manips_options(0, 1) == ST_OK && (menu_model_view(&v), strcmp(value_of(&v, "Auto-reset"), "Off") == 0),
	      "remember without auto-reset, which only the client sets, reads Off");
	check(act(&v, MBOX_EV_RIGHT, "Auto-reset") == ST_OK && manips_options_now() == 1, "and Right from it is On");
	check(act(&v, MBOX_EV_ACTIVATE, "Apply now") == ST_OK && value_of(&v, "Apply now")[0] == 0,
	      "Apply now with None applies nothing, and says nothing (round 5)");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/*
	 * Unlocks: the tabs, then each a CONFIG page of selectors; round 6, the
	 * two buttons the client has above its table on a page of their own,
	 * Collectables, as the other games have theirs.
	 */
	check(act(&v, MBOX_EV_ACTIVATE, "Unlocks") == ST_OK && v.ptype == MBOX_PTYPE_MAIN && v.nrows == 4 &&
	      strcmp(v.rows[0].label, "Weapons") == 0 && strcmp(v.rows[1].label, "Gadgets") == 0 &&
	      strcmp(v.rows[2].label, "Bot upgrades") == 0 && strcmp(v.rows[3].label, "Collectables") == 0 &&
	      v.rows[3].kind == MROW_LINK && view_row(&v, "Max all weapon levels") == NULL,
	      "Unlocks lists Weapons, Gadgets, Bot upgrades, then Collectables, a link (round 6)");
	check(act(&v, MBOX_EV_ACTIVATE, "Collectables") == ST_OK && strcmp(v.title, "Collectables") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 2 &&
	      strcmp(v.rows[0].label, "Max all weapon levels") == 0 && strcmp(v.rows[1].label, "Max all weapon ammo") == 0 &&
	      v.rows[0].kind == MROW_ACTION && v.rows[1].kind == MROW_ACTION,
	      "Collectables: a CONFIG page of Max all weapon levels and Max all weapon ammo, buttons");
	dl_weapon(2, 1);
	check(act(&v, MBOX_EV_ACTIVATE, "Max all weapon levels") == ST_OK && dl_weapon_now(2) == 99 &&
	      value_of(&v, "Max all weapon levels")[0] == 0, "the cross on Max all weapon levels: an owned weapon at V99");
	dl_weapon(2, 0);
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	check(strcmp(v.title, "Unlocks") == 0 && v.cursor == 3, "back: Unlocks, the cursor on Collectables");
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
	check(act(&v, MBOX_EV_ACTIVATE, "Save files") == ST_OK && v.ptype == MBOX_PTYPE_CONFIG &&
	      view_row(&v, "menu-test") != NULL && view_row(&v, "menu-test")->kind == MROW_LINK,
	      "Save files is a CONFIG page (round 4) of the library's categories, menu-test among them, links");
	check(act(&v, MBOX_EV_ACTIVATE, "menu-test") == ST_OK && strcmp(v.title, "menu-test") == 0 && v.nrows == 2 &&
	      v.ptype == MBOX_PTYPE_CONFIG && view_row(&v, "first") != NULL && view_row(&v, "second") != NULL,
	      "its page, CONFIG too, lists its two saves by name, without the .sav");
	r = view_row(&v, "first");
	check(r != NULL && r->kind == MROW_ACTION && (r->flags & MROWF_CLOSES) != 0 && r->value[0] == 0,
	      "each a button, blank, that loads once the menu is closed");
	check(act(&v, MBOX_EV_ACTIVATE, "first") == ST_OK && value_of(&v, "first")[0] == 0,
	      "the cross on one: it waits for the close, the row blank (round 5)");
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
	check(v.nrows == 7 && v.rows[6].kind == MROW_BACK && (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == 0,
	      "Game: its six rows and a Back row, one window");
	r = view_row(&v, "Challenge mode");
	check(r != NULL && r->kind == MROW_TOGGLE && r->flags == MROWF_ENABLED,
	      "with no Left and Right the switch takes the cross alone");
	check(act(&v, MBOX_EV_ACTIVATE, "Back") == ST_OK && menu_model_depth() == 1, "Back goes back");
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.nrows == 8 && strcmp(v.rows[6].label, "Valix") == 0 && v.rows[7].kind == MROW_BACK &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_DOWN,
	      "eight rows on screens of eight with a Back row: seven, then Back, the page going on below (round 4)");
	check(menu_model_event(MBOX_EV_SCROLL_DOWN, 6, v.rows[6].id, v.key) == ST_OK,
	      "the slide from the seventh, Valix");
	menu_model_view(&v);
	check(v.nrows == 8 && strcmp(v.rows[v.cursor].label, "Valix") == 0 && v.rows[7].kind == MROW_BACK &&
	      strcmp(v.rows[6].label, "Interior") == 0 &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_UP,
	      "the last window, the cursor still on Valix, Interior, the Back row last again");
	check(act(&v, MBOX_EV_ACTIVATE, "Back") == ST_OK && menu_model_depth() == 1, "Back goes back");

	check(quit(), "quit Deadlocked");
}

/*
 * Round 4: a page longer than its window has no More or Previous row; the
 * window slides under the cursor. Every row of RaC2's planets, twenty-seven of
 * them, through windows of eight: down the page by the slides at each window's
 * last row and back up by those at its first, the cursor on the same planet
 * each time, never on an edge the page goes on past; L1 and R1 a window; the
 * title the page's own.
 */
static void test_model_paging(void)
{
	struct menu_view v;
	u8 count, seen, k, i, start, at;
	u16 ids[64], cur;
	int guard, rows_ok = 1, kinds_ok = 1, same_ok = 1, cursor_ok = 1, title_ok = 1, slides = 0;

	group("console menu: the model, a page longer than its window (round 4: RaC2's planets, sliding)");

	check(boot("NPEA00386"), "RaC2 boots");
	session_game()->planet_names(&count);
	check(count > 8 && count <= 64, "RaC2 has more planets than a window of eight has rows");

	menu_model_reset(8, 0, MBOX_CAP_STEPS | MBOX_CAP_BACK);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_PLANETS, 0) && v.nrows == 8 && v.cursor == 0 &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_DOWN,
	      "its Planets page opens on a window of eight, the page going on below");
	check(menu_model_event(MBOX_EV_SCROLL_UP, 0, v.rows[0].id, v.key) == ST_BAD_ARG &&
	      menu_model_event(MBOX_EV_PAGE_PREV, 0, v.rows[0].id, v.key) == ST_BAD_ARG,
	      "nothing above the first window: SCROLL_UP and L1 refused");
	menu_model_view(&v);

	/* Down: the cursor to the window's last row, and the slide. */
	seen = 0;
	for (guard = 0; guard < 40; guard++) {
		start = (u8)(v.key >> 8);
		if (v.nrows > 8) rows_ok = 0;
		if (strchr(v.title, '/') != NULL || strcmp(v.title, "Planets") != 0) title_ok = 0;
		for (i = 0; i < v.nrows; i++) {
			if (v.rows[i].kind == MROW_MORE || v.rows[i].kind == MROW_PREV) kinds_ok = 0;
			if (start + i < seen && ids[start + i] != v.rows[i].id) same_ok = 0;
			if (start + i == seen && seen < 64) ids[seen++] = v.rows[i].id;
		}
		if ((v.pflags & MBOX_PAGE_MORE_DOWN) == 0) break;
		cur = v.rows[v.nrows - 1].id;
		if (menu_model_event(MBOX_EV_SCROLL_DOWN, (u8)(v.nrows - 1), cur, v.key) != ST_OK) break;
		menu_model_view(&v);
		slides++;
		if (v.rows[v.cursor].id != cur) cursor_ok = 0;
		if (v.cursor == v.nrows - 1 && (v.pflags & MBOX_PAGE_MORE_DOWN) != 0) cursor_ok = 0;
	}
	check(rows_ok && kinds_ok && title_ok, "every window eight rows at most, no More or Previous row, titled Planets");
	check(slides >= 3 && cursor_ok, "each slide keeps the cursor on the planet it was on, off the window's edge");
	check(seen == count && same_ok, "the windows show every planet, each at its own place");
	for (k = 1; k < seen; k++) {
		for (i = 0; i < k; i++) {
			if (ids[i] == ids[k]) break;
		}
		if (i < k) break;
	}
	check(k == seen, "each once");
	check((v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_UP &&
	      menu_model_event(MBOX_EV_SCROLL_DOWN, (u8)(v.nrows - 1), v.rows[v.nrows - 1].id, v.key) == ST_BAD_ARG,
	      "the last window: the page goes on above only, and a slide down is refused");
	menu_model_view(&v);

	/* Up again. */
	cursor_ok = 1;
	for (guard = 0; guard < 40 && (v.pflags & MBOX_PAGE_MORE_UP) != 0; guard++) {
		cur = v.rows[0].id;
		if (menu_model_event(MBOX_EV_SCROLL_UP, 0, cur, v.key) != ST_OK) break;
		menu_model_view(&v);
		if (v.rows[v.cursor].id != cur || (v.cursor == 0 && (v.pflags & MBOX_PAGE_MORE_UP) != 0)) cursor_ok = 0;
	}
	check((v.key >> 8 & 0xFF) == 0 && cursor_ok && v.rows[v.cursor].id == ids[v.cursor],
	      "and the slides at each window's first row walk back to the first, the cursor on its planet");

	/* L1 and R1: the cursor a window on, the window after it. */
	menu_model_event(MBOX_EV_PAGE_NEXT, 0, v.rows[0].id, v.key);
	menu_model_view(&v);
	at = (u8)((v.key >> 8 & 0xFF) + v.cursor);
	check(at == 6 && v.cursor == 1 && v.rows[v.cursor].id == ids[6],
	      "R1 on the first planet: the seventh, second in its window");
	menu_model_event(MBOX_EV_PAGE_PREV, v.cursor, v.rows[v.cursor].id, v.key);
	menu_model_view(&v);
	check((v.key >> 8 & 0xFF) == 0 && v.rows[v.cursor].id == ids[0], "L1: back to the first");

	check(quit(), "quit RaC2");
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

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 19 && d->ncaves == 4,
	      "Deadlocked has a renderer: the mailbox, the state, the read-only data, the code, nineteen hook words "
	      "(nine, and the planet picker's ten)");
	check(d != NULL && d->caves[0].zeros > 1000 && d->caves[1].zeros > 3000 && d->caves[0].len < 64 &&
	      d->caves[1].len < 64, "the mailbox and the state are carried as a few bytes and a count of zeros");
	check(mh_desc_for_game(GAME_RAC1) != NULL && mh_desc_for_game(GAME_RAC2) != NULL &&
	      mh_desc_for_game(GAME_RAC3) != NULL && mh_desc_for_game(GAME_NONE) == NULL,
	      "every game has a renderer, and no game none");
	if (d == NULL) return;
	for (i = 0; i < d->nhooks; i++) {
		if (d->hooks[i].addr >= 0x003A6858u && d->hooks[i].addr < 0x003A6F74u) break;
	}
	check(i == d->nhooks, "none of them in MenuMain_StartOptions: Options is the game's own list");
	check(d->hooks[17].addr == MH_SITE_HELP_LABEL && d->hooks[18].addr == MH_SITE_HELP_SCREEN &&
	      d->hooks[18].value == 0x38800024u, "the last two are the Help row's label call and its li r4,36");
	check(d->hooks[7].addr == MH_SITE_SELECT && d->hooks[8].addr == MH_SITE_GHOST && d->hooks[9].addr == MH_SITE_COUNT &&
	      d->hooks[10].addr == MH_SITE_BACK && d->hooks[11].addr == MH_SITE_CLOSE &&
	      d->hooks[16].addr == MH_WORLDS_STOP && d->hooks[16].original == MH_ORIG_WORLDS_STOP &&
	      d->hooks[7].original == MH_ORIG_SELECT && d->hooks[9].original == MH_ORIG_COUNT,
	      "before them the planet picker's: Worlds' X, its gates and its stop, with the game's words");
	check(d->hooks[12].addr == MH_SITE_LOCKED && d->hooks[12].original == MH_ORIG_LOCKED &&
	      (d->hooks[12].value >> 26) == 18 && (d->hooks[12].value & 3) == 0 &&
	      d->hooks[13].addr == MH_SITE_SKILLS && d->hooks[13].original == MH_ORIG_SKILLS &&
	      (d->hooks[13].value >> 26) == 18 && (d->hooks[13].value & 3) == 0,
	      "the pickers' round 2: the lock's gate and Circle's, each a plain branch to a stub, after the four, "
	      "before the stop");
	check(d->hooks[14].addr == MH_SITE_TOURNEY && d->hooks[14].original == MH_ORIG_TOURNEY &&
	      (d->hooks[14].value >> 26) == 18 && (d->hooks[14].value & 3) == 0 &&
	      d->hooks[15].addr == MH_SITE_LEGEND && d->hooks[15].original == MH_ORIG_LEGEND &&
	      (d->hooks[15].value >> 26) == 18 && (d->hooks[15].value & 3) == 0,
	      "round 6: the tournaments' gate and the legend's, plain branches to stubs, after those, before the stop");

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
		check(n <= 26 && bytes <= 22u * 1024u,
		      "twenty-six writes at most, under 22 KB in all (the planet picker's six words included)");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	check(peek32(DL_OPTIONS_SITE) == DL_OPTIONS_WORD, "Options' call to SetMenuButtons keeps the game's word");

	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek16(mb + MBOX_OFF_VERSION) == 3 &&
	      peek8(mb + MBOX_OFF_ROW_CAP) == 16 && peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 &&
	      peek8(mb + MBOX_OFF_MAX_ROWS) == 8 && peek8(mb + MBOX_OFF_CFG_ROWS) == 16 &&
	      peek8(mb + MBOX_OFF_CAPS) == MH_CAPS && peek16(mb + MBOX_OFF_SIZE) == 0x480,
	      "the mailbox: magic, version 3, sixteen rows of 0x40 (round 4: no descriptions), eight a MAIN page, "
	      "sixteen a CONFIG page");
	check(peek32(MH_STATE) == MH_MAGIC, "the renderer's state, its magic in place");

	read_box(&p);
	check(p.seq != 0 && (p.seq & 1) == 0 && p.ptype == MBOX_PTYPE_MAIN && strcmp(p.title, "Trainer") == 0 &&
	      p.n == 7 && (p.pflags & MBOX_PAGE_ROOT) != 0 && strcmp(p.rows[0].label, "Game") == 0,
	      "the root page is in, page_seq even, a MAIN page");

	/* Events and the page type. */
	box_event(MBOX_EV_ACTIVATE, 1);
	check(!box_acked(), "an event posted is not acked yet");
	tick(4);
	read_box(&p);
	check(box_acked() && strcmp(p.title, "Cheats") == 0 && p.n == 7 && p.ptype == MBOX_PTYPE_CONFIG,
	      "within a poll it is acked, and the Cheats page is posted as a CONFIG page");
	check(strcmp(p.rows[1].label, "Ghost Ratchet") == 0 && peek8(mb + MBOX_OFF_ROWS + 0x40 + MROW_OFF_LABEL) == 'G',
	      "each row 0x40 bytes, the next row's label where round 3's description was");
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

	/* Round 3: whenever the menu closes, the next open starts at the root, the cursor on its first row. */
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	read_box(&p);
	check(strcmp(p.title, "Cheats") == 0 && menu_model_depth() == 2, "on Cheats, the menu up");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(4);
	read_box(&p);
	check(strcmp(p.title, "Trainer") == 0 && p.cursor == 0 && (p.pflags & MBOX_PAGE_ROOT) != 0 &&
	      menu_model_depth() == 1, "the renderer says closed: the root is posted at once, the cursor on its first row");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	poke32(mb + MBOX_OFF_FRAMES, 0);
	tick(4);
	box_activate("Cheats");
	poke32(mb + MBOX_OFF_FRAMES, 400);
	tick(2);
	read_box(&p);
	check(strcmp(p.title, "Cheats") == 0, "open again, on Cheats, four hundred frames up");
	poke32(mb + MBOX_OFF_FRAMES, 2);
	tick(2);
	read_box(&p);
	check(strcmp(p.title, "Trainer") == 0 && p.cursor == 0 && menu_model_depth() == 1,
	      "its frame count back at 2, the menu never seen closed: an opening, so the root again");
	poke32(mb + MBOX_OFF_FRAMES, 3);
	tick(2);
	check(menu_model_depth() == 1, "and the count going on from there is no new opening");
	box_activate("Cheats");
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

	/* A slider's numbers, on the tests' own table of values (round 5: no game's page has a slider now). */
	tv_menu_on();
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_activate("Values");
	read_box(&p);
	i = box_index(&p, "Challenge mode");
	{
		u8 raw[MH_ROW_BYTES];
		host_peek(mb + MBOX_OFF_ROWS + (u32)i * MH_ROW_BYTES, raw, sizeof(raw));
		check(i >= 0 && (p.rows[i].flags & (MROWF_RANGE | MROWF_SLIDER)) == (MROWF_RANGE | MROWF_SLIDER) &&
		      raw[MROW_OFF_LABEL + 23] == 0 && be32_get(raw + MROW_OFF_RANGE + 8) == 255 &&
		      be32_get(raw + MROW_OFF_RANGE + 4) == 0 && be32_get(raw + MROW_OFF_RANGE + 12) == 26 &&
		      be32_get(raw + MROW_OFF_RANGE) == peek8(RAC4_CHALLENGE_MODE),
		      "Challenge mode's row carries value, 0, 255 and (round 4) step 26 at +0x1C, past its label's NUL");
	}
	box_event(MBOX_EV_BACK, 0);
	tick(2);
	menu_test_menu(NULL);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(4);

	/* CLOSES actions of each kind: asked to close first, run once the game plays. */
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_activate("Game");
	read_box(&p);
	i = box_index(&p, "Challenge mode");
	check(i == 3 && p.rows[i].kind == MROW_TOGGLE && (p.rows[i].flags & MROWF_ENABLED) != 0 &&
	      strcmp(p.rows[2].label, "Load position") == 0 && strcmp(p.rows[4].label, "Set aside file") == 0 &&
	      box_index(&p, "") < 0,
	      "round 6: Game's Challenge mode in the mailbox, a switch between the two parts, where round 5's spacer was");
	box_activate("Load set-aside file");
	read_box(&p);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE &&
	      p.rows[box_index(&p, "Load set-aside file")].value[0] == 0,
	      "loading the set-aside file asks the menu to close first, its row blank (round 5)");
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

	/* Round 4: a save category longer than the sixteen rows: the window slides, through the mailbox. */
	{
		char name[16];
		int k;

		for (k = 0; k < 20; k++) {
			name[0] = 's';
			name[1] = (char)('0' + k / 10);
			name[2] = (char)('0' + k % 10);
			qstrcpy(name + 3, sizeof(name) - 3, ".sav");
			save_put("menu-long", name, 16);
		}
		poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
		tick(4);
		box_event(MBOX_EV_BACK, 0);
		tick(2);
		box_activate("Save files");
		box_activate("menu-long");
		read_box(&p);
		check(p.ptype == MBOX_PTYPE_CONFIG && p.n == 16 && strcmp(p.title, "menu-long") == 0 &&
		      (p.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_DOWN &&
		      strcmp(p.rows[0].label, "s00") == 0 && strcmp(p.rows[15].label, "s15") == 0,
		      "twenty saves: the first sixteen, the page going on below, no More row, the title its own");
		box_event(MBOX_EV_SCROLL_DOWN, 15);
		tick(2);
		read_box(&p);
		check(box_acked() && p.n == 16 && (p.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_UP &&
		      strcmp(p.rows[0].label, "s04") == 0 && strcmp(p.rows[p.cursor].label, "s15") == 0,
		      "the renderer's slide from the sixteenth: the last window, s04 .. s19, the cursor still on s15");
		box_event(MBOX_EV_SCROLL_UP, 0);
		tick(2);
		read_box(&p);
		check(strcmp(p.rows[0].label, "s00") == 0 && strcmp(p.rows[p.cursor].label, "s04") == 0 &&
		      (p.pflags & MBOX_PAGE_MORE_DOWN) != 0, "and up from s04: the first window again, the cursor on s04");
		for (k = 0; k < 20; k++) {
			name[0] = 's';
			name[1] = (char)('0' + k / 10);
			name[2] = (char)('0' + k % 10);
			qstrcpy(name + 3, sizeof(name) - 3, ".sav");
			save_drop("menu-long", name);
		}
		save_drop("menu-long", NULL);
		box_event(MBOX_EV_BACK, 0);
		tick(2);
		box_event(MBOX_EV_BACK, 0);
		tick(2);
	}

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
	poke8(DL_MENU_EXIT, 1);
	G.sound = -1;
	memset(G.sounds, 0xFF, sizeof(G.sounds));
	game_help_row();
	check(G.screen == MH_SCREEN_ID && G.shown && !G.main_up && G.depth == 1 && G.stack[0] == 1,
	      "its Help row pushes screen 36 over the main menu");
	check(G.sounds[0] == SND_FORWARD && G.sounds[1] == -1,
	      "with the row's own sound and none of the list's: a list screen of the game's starts in silence");
	check(strcmp(read_str(G.title, title, sizeof(title)), "TRAINER") == 0 && G.nbuttons == 7 &&
	      strcmp(shown_text(0, text, sizeof(text)), "GAME") == 0 &&
	      strcmp(shown_text(5, text, sizeof(text)), "SAVE FILES") == 0 &&
	      strcmp(shown_text(6, text, sizeof(text)), "QWARK BUILD 54") == 0,
	      "the root in the main menu's capitals: GAME .. SAVE FILES, then QWARK BUILD 54, under TRAINER");
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
	G.sound = -1;
	game_hud_frame();
	check(G.screen == MH_SCREEN_ID && G.shown && G.paused && G.depth == 0 && peek8(mb + MBOX_OFF_REQUEST) == 0 &&
	      peek32(MH_STATE + ST_OPENS) == 1, "the next GuiHud frame opens screen 36 through the Start path");
	check(G.sound == SND_OPEN && peek8(DL_MENU_SOUND_FLAG) == 0,
	      "with the pause's own sound, as the main menu plays it after a close, and its flag taken");
	tick(4);

	/*
	 * A new page of the same type is the game's screen change: the list stops and starts again.
	 * (Round 4: Planets is a CONFIG page now; Unlocks is the MAIN page after the root.)
	 */
	n = G.targets;
	i = G.stops;
	G.cursor = 4;
	game_frame(PADB_CROSS);
	check(G.sound == SND_FORWARD && !box_acked(), "the cross on UNLOCKS posts an event, with the forward sound");
	tick(2);
	G.sound = -1;
	game_frame(0);
	check(strcmp(read_str(G.title, title, sizeof(title)), "UNLOCKS") == 0 && G.targets == n + 1 && G.stops == i + 1 &&
	      G.shown && G.screen == MH_SCREEN_ID,
	      "the next frame stops the list (MenuMain_Stop) and starts screen 36 again on UNLOCKS, the frame fitted to it");
	check(G.sound == -1, "the start again is silent, as Options' is after the main menu's 7");
	n = G.nbuttons;
	i = G.stops;
	game_frame(PADB_TRIANGLE);
	check(G.sound == SND_BACK, "triangle on UNLOCKS: a list's back sound");
	tick(2);
	game_frame(0);
	check(G.nbuttons == 7 && G.stops == i + 1 && G.cursor == 4,
	      "back to the root through the same stop and start: seven rows, the cursor on UNLOCKS");
	n = G.targets;
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(30);
	game_frame(0);
	check(G.targets == n, "a page with as many rows is not laid out again");

	/* The two-column page: a CONFIG page starts the screen again with the CtlList. */
	G.cursor = 1;
	game_frame(PADB_CROSS);
	tick(2);
	{
		u32 list_title = G.title, list_rows = G.buttons;
		game_frame(0);
		check(!G.shown && G.stops >= 1 && G.thread == 1 && G.screen == MH_SCREEN_ID,
		      "the cross on CHEATS: the list stops, screen 36 starts again with the shim's thread");
		check(G.sound == SND_SCREEN, "GAME SETTINGS' start sound as the panel screen starts");
		check(strcmp(read_str(list_title, title, sizeof(title)), "TRAINER") == 0 &&
		      strcmp(read_str(r32(list_rows + MH_BUTTON_SIZE + 4), text, sizeof(text)), "CHEATS") == 0 &&
		      strcmp(read_str(r32(list_rows + 6 * MH_BUTTON_SIZE + 4), text, sizeof(text)), "QWARK BUILD 54") == 0,
		      "round 3: the list fading out still reads TRAINER, CHEATS .. QWARK BUILD 54, not the Cheats page");
		for (i = 0; i < 25; i++) {
			if (i == 1) {
				game_frame(PADB_DOWN);
				game_frame(PADB_UP);
			}
			game_frame(0);
			if (strcmp(read_str(r32(list_rows + MH_BUTTON_SIZE + 4), text, sizeof(text)), "CHEATS") != 0) break;
		}
		check(i == 25, "and does on, the Cheats page's moves and all (nothing composes over it)");
	}
	game_frame(0);
	check(G.thread == 2 && G.screen_tpl == 37 && G.list_tpl == DL_TPL_ROW && G.count == 7 &&
	      G.layer == DL_LAYER && G.images == 3 && G.cursor_attached,
	      "the thread's first frame builds GAME SETTINGS' panel (template 37), the list, seven rows, its plates and cursor");
	check(r32(MH_SHIM + SH_LIST) == DL_CTLLIST_VTABLE, "the list has CtlList's vtable at +0, as its constructor writes it");
	for (i = DL_EL_ROWS_FIRST, n = 0; i <= DL_EL_ROWS_LAST; i++)
		n += recall(G.vis_h, G.vis_v, r32(MH_SHIM + SH_PANEL) + (u32)i) == 2;
	check(n == DL_EL_ROWS_LAST - DL_EL_ROWS_FIRST + 1, "GAME SETTINGS' own eight rows hidden, all 21 widgets");
	check(strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_TITLE, title, sizeof(title)), "CHEATS") == 0 &&
	      strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_LEGEND, text, sizeof(text)), "\x10 SELECT \x12 BACK") == 0,
	      "titled CHEATS, with GAME SETTINGS' own button legend");
	check(strcmp(cfg_label(1, text), "GHOST RATCHET") == 0 && strcmp(cfg_value(1, title), "OFF") == 0 &&
	      strcmp(cfg_label(6, text), "UNLOCK ALL PLANETS") == 0 && strcmp(cfg_value(6, title), "") == 0 &&
	      strcmp(cfg_label(4, text), "MAX BOLTS") == 0 && strcmp(cfg_label(5, title), "MAX DREAD POINTS") == 0 &&
	      strcmp(cfg_value(5, title), "") == 0,
	      "labels in GAME SETTINGS' capitals without its colon, values in its capitals, a button blank (round 4); "
	      "round 6: MAX BOLTS, MAX DREAD POINTS, then UNLOCK ALL PLANETS, no CHALLENGE MODE");
	check(focus() == 0 && cfg_color(0) == DL_RGBA_FOCUS && cfg_color(1) == DL_RGBA_OFF,
	      "the cursor on the first row, which is lit; the others the game's grey");
	check(recall(G.vis_h, G.vis_v, r32(item(1) + DL_ITEM_PANEL) + DL_ROW_BOX_A) == 2 &&
	      recall(G.vis_h, G.vis_v, r32(item(1) + DL_ITEM_PANEL) + DL_ROW_BOX_B) == 2,
	      "a toggle's row keeps its swatch containers hidden: no slider");
	game_frame(0);

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
	check(focus() == 0 && cfg_color(1) == DL_RGBA_OFF, "off the row, an active cheat is GAME SETTINGS' grey, as any row");
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
	for (i = 0; i < 5; i++) game_frame(PADB_DOWN);
	poke32(RAC4_DREAD_POINTS, 0);
	game_frame(PADB_CROSS);
	check(G.sound == SND_FORWARD, "the cross on a button: a row's forward sound, as a list's row");
	tick(2);
	game_frame(0);
	check(focus() == 6 && strcmp(cfg_value(6, text), "") == 0 && peek32(RAC4_DREAD_POINTS) == 1000000,
	      "the button runs, and its value stays blank (round 5: no DONE)");

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
	G.sound = -1;
	game_frame(PADB_TRIANGLE);
	check(G.sound == -1 && !box_acked(), "triangle on the page posts back, with no sound of its own, as GAME SETTINGS' back");
	tick(2);
	game_frame(0);
	check(G.sound == SND_SCREEN_CLOSE, "the panel's stop sound as the screen goes");
	check(r8(MH_SHIM + SH_CFG_STATE) == 2 && strcmp(cfg_label(1, text), "GHOST RATCHET") == 0 &&
	      strcmp(cfg_value(1, title), "OFF") == 0 &&
	      strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_TITLE, title, sizeof(title)), "CHEATS") == 0,
	      "round 3: the Cheats page ending still reads its own rows and title, not the root's");
	game_frame(0);
	game_frame(0);
	check(G.sound == SND_SCREEN_CLOSE, "and the list's start says nothing more, as Options' after GAME SETTINGS");
	check(G.thread == 0 && G.layer == 0 && G.images == 0 && !G.cursor_attached && G.shown &&
	      G.screen == MH_SCREEN_ID && G.depth == 0,
	      "the thread ends - the panel cleared, the plates dropped, the cursor detached - and the list is back");
	check(strcmp(read_str(G.title, title, sizeof(title)), "TRAINER") == 0 && G.cursor == 1,
	      "on the root, the cursor on CHEATS");

	/* Closes: Start from the CONFIG page, and a close request. */
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	game_frame(0);
	n = G.closes;
	G.sound = -1;
	poke8(DL_MENU_SOUND_FLAG, 0);
	game_frame(PADB_START);
	check(G.closes == n + 1 && G.screen == 0 && !G.paused && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "Start on the CONFIG page closes the menu and unpauses");
	check(G.sounds[0] == SND_SCREEN_CLOSE && G.sounds[1] != 5 && peek8(DL_MENU_SOUND_FLAG) == 1,
	      "with GAME SETTINGS' close: CloseMenu(p, -1), the main menu's sound flag set, the panel's stop sound alone");
	game_frame(0);
	game_frame(0);
	check(G.thread == 0 && G.layer == 0, "and its thread ends behind it");
	tick(4);
	check(menu_model_depth() == 1, "round 3: closed, qwark's stack is back on the root");
	press(COMBO_MENU_DEFAULT);
	G.sound = -1;
	game_hud_frame();
	check(G.sound == SND_OPEN && peek8(DL_MENU_SOUND_FLAG) == 0,
	      "the combo opens with the pause's sound, the flag the close set taken");
	game_frame(0);
	check(G.thread == 0 && G.shown && G.cursor == 0 && G.nbuttons == 7 &&
	      strcmp(read_str(G.title, title, sizeof(title)), "TRAINER") == 0,
	      "the combo opens the menu on the root, the cursor on its first row, not on the page it closed on");

	/* A CONFIG page after a CONFIG page: the thread ends and the next starts on the other GuiState. */
	tick(4);
	G.cursor = 1;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	game_frame(0);
	check(G.thread == 2 && G.count == 7, "CHEATS again");
	{
		u32 gs = G.gs;
		shim_switch(0);
		check(G.thread == 2 && r8(MH_SHIM + SH_CFG_STATE) == 2, "a new CONFIG page: the thread told to end");
		game_frame(0);
		game_frame(0);
		check(G.thread == 1 && G.gs != gs && (G.gs == MH_SHIM + SH_GS || G.gs == MH_SHIM + SH_GS + DL_GS_SIZE),
		      "its Deinit starts the next page's thread on the other GuiState");
		game_frame(0);
		check(G.thread == 2 && G.count == 7 && G.layer == DL_LAYER, "which builds the panel again");
	}
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo again asks it to close");
	game_frame(0);
	check(G.screen == 0 && !G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0, "which the thread's next frame does");
	game_frame(0);
	game_frame(0);
	check(G.thread == 0 && G.layer == 0, "the thread ends in the frames after, as the game plays on");
	tick(4);

	/*
	 * Round 6: UNLOCKS, a MAIN page of the tabs and COLLECTABLES, whose CONFIG
	 * page has the two weapon buttons, as the other games' COLLECTABLES pages.
	 */
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	tick(4);
	for (i = 0; i < 4; i++) game_frame(0);
	row = shown_row("UNLOCKS");
	G.cursor = row;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(read_str(G.title, title, sizeof(title)), "UNLOCKS") == 0 && G.nbuttons == 4 &&
	      shown_row("COLLECTABLES") == 3 && shown_row("MAX ALL WEAPON AMMO") < 0,
	      "UNLOCKS lists the tabs and COLLECTABLES (round 6), no button");
	G.cursor = 3;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	game_frame(0);
	check(G.thread == 2 && G.count == 2 &&
	      strcmp(text_of(r32(MH_SHIM + SH_PANEL) + DL_EL_TITLE, title, sizeof(title)), "COLLECTABLES") == 0 &&
	      strcmp(cfg_label(0, text), "MAX ALL WEAPON LEVELS") == 0 && strcmp(cfg_label(1, title), "MAX ALL WEAPON AMMO") == 0,
	      "COLLECTABLES: GAME SETTINGS' look with MAX ALL WEAPON LEVELS and MAX ALL WEAPON AMMO");
	game_frame(PADB_DOWN);
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(focus() == 1 && strcmp(cfg_value(1, text), "") == 0 && box_acked(),
	      "a button runs and says nothing beside its label (round 5: no DONE)");
	game_frame(PADB_SELECT);
	check(G.screen == 0 && !G.paused, "Select closes the menu");
	game_frame(0);
	game_frame(0);
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
	tick(4);

	/*
	 * GAME SETTINGS' slider on a value the core marks for one (round 3:
	 * Challenge mode, 0..255), on the tests' own page of values (round 5).
	 */
	poke8(RAC4_CHALLENGE_MODE, 51);
	poke32(RAC4_BOLTS, 0);
	tick(10);
	tv_menu_on();
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	tick(4);
	G.cursor = 0;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	game_frame(0);
	check(G.thread == 2 && G.count == 3 && strcmp(cfg_label(2, text), "CHALLENGE MODE") == 0,
	      "VALUES: three rows, the last CHALLENGE MODE");
	{
		u32 row = r32(item(2) + DL_ITEM_PANEL), bolts = r32(item(0) + DL_ITEM_PANEL);
		float *fill = NULL, *track = NULL, *outline = NULL;

		for (i = 0; i < NTEXT; i++) {
			if (G.rect_h[i] == row + DL_ROW_FILL) fill = G.rect_v[i];
			if (G.rect_h[i] == row + DL_ROW_TRACK) track = G.rect_v[i];
			if (G.rect_h[i] == row + DL_ROW_OUTLINE) outline = G.rect_v[i];
		}
		check(recall(G.vis_h, G.vis_v, row + DL_ROW_BOX_A) == 1 && recall(G.vis_h, G.vis_v, row + DL_ROW_BOX_B) == 1 &&
		      recall(G.vis_h, G.vis_v, row + DL_ROW_SPARE) == 2 && strcmp(cfg_value(2, title), "") == 0,
		      "its row shows its outline and swatch containers, the spare outline hidden, no number");
		check(outline != NULL && track != NULL && fill != NULL &&
		      outline[2] - outline[0] > 0.355f && outline[2] - outline[0] < 0.356f && outline[2] > 0.900f &&
		      outline[2] < 0.901f && track[3] - track[1] > 0.0191f && track[3] - track[1] < 0.0193f &&
		      recall(G.color_h, G.color_c, row + DL_ROW_TRACK) == DL_RGBA_TRACK,
		      "GAME SETTINGS' slider: an outline 0.3555 wide ending at 0.9004, a dark track 0.0192 high");
		check(fill != NULL && track != NULL && fill[0] == track[0] && fill[1] == track[1] && fill[3] == track[3] &&
		      fill[2] - fill[0] > DL_TRACK_W * 0.199f && fill[2] - fill[0] < DL_TRACK_W * 0.201f &&
		      G.fill_h == row + DL_ROW_FILL && G.fill_c0 == 0x80000102u && G.fill_c1 == 0x80000100u,
		      "the fill: the track cut at 51 of 0..255, a fifth, in GetTeamColor(1, 2) and (1, 0)");
		check(recall(G.vis_h, G.vis_v, bolts + DL_ROW_BOX_A) == 2 && strcmp(cfg_value(0, text), "0") == 0,
		      "BOLTS, with no bounds, keeps its number and no slider");
		for (i = 0; i < 2; i++) game_frame(PADB_DOWN);
		check(focus() == 2, "down to it");
		game_frame(PADB_RIGHT);
		check(G.sound == SND_SLIDE, "Right on it: GAME SETTINGS' slider sound");
		tick(2);
		game_frame(0);
		check(peek8(RAC4_CHALLENGE_MODE) == 77 && fill[2] - fill[0] > DL_TRACK_W * 0.301f &&
		      fill[2] - fill[0] < DL_TRACK_W * 0.303f, "77, the fill a tenth longer (round 4: a step of 26)");
		poke8(RAC4_CHALLENGE_MODE, 255);
		tick(40);
		game_frame(0);
		G.sound = -1;
		game_frame(PADB_RIGHT);
		check(G.sound == SND_LIMIT && fill[2] - fill[0] > DL_TRACK_W * 0.999f,
		      "at 255, full, Right sounds as a slider at its end");
		tick(2);
		game_frame(0);
	}
	game_frame(PADB_START);
	check(G.screen == 0, "closed");
	game_frame(0);
	game_frame(0);
	menu_test_menu(NULL);
	tick(4);

	/*
	 * Round 6: GAME's CHALLENGE MODE, where round 5 had a blank row the
	 * cursor passed over: a checkbox between LOAD POSITION and SET ASIDE FILE.
	 */
	poke8(RAC4_CHALLENGE_MODE, 0);
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	tick(4);
	G.cursor = 0;
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	game_frame(0);
	check(G.thread == 2 && G.count == 6 && strcmp(cfg_label(2, text), "LOAD POSITION") == 0 &&
	      strcmp(cfg_label(3, title), "CHALLENGE MODE") == 0 && strcmp(cfg_value(3, title), "OFF") == 0 &&
	      strcmp(cfg_label(4, text), "SET ASIDE FILE") == 0 && mh_cfg_valid(3) && mh_cfg_valid(4),
	      "GAME: six rows, the fourth CHALLENGE MODE, OFF, a row the cursor lands on");
	game_frame(PADB_DOWN);
	game_frame(PADB_DOWN);
	game_frame(PADB_DOWN);
	check(focus() == 3, "down from LOAD POSITION to CHALLENGE MODE");
	game_frame(PADB_CROSS);
	tick(2);
	game_frame(0);
	check(strcmp(cfg_value(3, text), "ON") == 0 && peek8(RAC4_CHALLENGE_MODE) == 1 && cfg_color(3) == DL_RGBA_FOCUS,
	      "the cross: ON, the counter 1, the row lit");
	game_frame(PADB_DOWN);
	check(focus() == 4, "down again: SET ASIDE FILE");
	game_frame(PADB_UP);
	check(focus() == 3, "and up: CHALLENGE MODE");
	poke8(RAC4_CHALLENGE_MODE, 0);
	game_frame(PADB_START);
	check(G.screen == 0, "closed");
	game_frame(0);
	game_frame(0);

	check(quit(), "quit Deadlocked");
}

/* -------------------------------------------------------- the planet picker */

/* A planet's level status byte, and its first mission's. */
static u8 dl_status(u8 planet)
{
	return peek8(RAC4_LEVEL_SAVES + (u32)(planet - 1) * RAC4_LEVEL_SIZE + RAC4_LEVEL_STATUS);
}

static u8 dl_first_mission(u8 planet)
{
	return peek8(RAC4_LEVEL_SAVES + (u32)(planet - 1) * RAC4_LEVEL_SIZE + RAC4_MISSION_STATUS);
}

/* The combo, then the menu's first frames: the root up, the cursor where the shim put it. */
static void dl_menu_open(void)
{
	press(COMBO_MENU_DEFAULT);
	game_hud_frame();
	tick(4);
	game_frame(0);
}

/*
 * Mailbox version 3, the planet picker: the driver against a renderer
 * simulated at the byte level, then Deadlocked's renderer against Worlds'
 * mocks. PLANETS opens the game's own Worlds over the root; Back returns to
 * the root on PLANETS; X hands the planet to qwark, which unlocks a locked one
 * and loads it once the game plays; Start, and Circle's Skills, end it; the
 * game's own Worlds is left alone; where the main menu greys WORLDS the row
 * opens qwark's own Planets page, every planet on it.
 */
static void test_picker(void)
{
	struct box_page p;
	char title[40];
	u32 mb, seq, frames;
	int planets_row, i, n;

	group("console menu: the planet picker, qwark's side (mailbox version 3)");

	memset(&G, 0, sizeof(G));
	memset(&W, 0, sizeof(W));
	G.input_ok = 1;
	G.handles = 0xF0000000u;
	W.mission = -1;
	poke32(MH_BOX + MBOX_OFF_PICK_SEQ, 0);
	check(dl_boot() && dl_installed(), "Deadlocked up and the renderer in");
	poke32(DL_PAD0_PTR, PAD0);
	mb = menu_mailbox();
	for (i = 1; i <= 15; i++) {
		dl_planet((u8)i, i == 1 || i == 2 || i == 5);
		poke8(RAC4_LEVEL_SAVES + (u32)(i - 1) * RAC4_LEVEL_SIZE + RAC4_MISSION_STATUS, 0);
	}

	read_box(&p);
	planets_row = box_index(&p, "Planets");
	check(peek16(mb + MBOX_OFF_VERSION) == 3 && (peek8(mb + MBOX_OFF_CAPS) & MBOX_CAP_PICKER) != 0 &&
	      planets_row == 2 && (p.rows[planets_row].flags & MROWF_PICKER) != 0,
	      "mailbox version 3: the renderer says it opens the game's planet picker, and qwark flags the root's "
	      "Planets row for it");
	for (i = 0, n = 0; i < p.n; i++) n += (p.rows[i].flags & MROWF_PICKER) != 0;
	check(n == 1, "and no other row");

	/* While the picker is up the menu is neither open nor closed. */
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(4);
	box_activate("Unlocks");
	read_box(&p);
	seq = p.seq;
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_PICKER);
	tick(8);
	read_box(&p);
	check(p.seq == seq && strcmp(p.title, "Unlocks") == 0 && menu_model_depth() == 2,
	      "hstate PICKER: no close, qwark's stack and page stay as they are");
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo then asks for a close, as with the menu up");
	tick(62);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "which nobody takes under Worlds, so it is withdrawn");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(4);
	check(menu_model_depth() == 1, "closed: back to the root");

	/* A pick, as a renderer posts it: the planet, then the seq. */
	poke32(RAC4_TARGET_PLANET, 0);
	poke32(RAC4_LOAD_PLANET2, 0);
	poke32(mb + MBOX_OFF_PICK_PLANET, 99);
	poke32(mb + MBOX_OFF_PICK_SEQ, peek32(mb + MBOX_OFF_PICK_SEQ) + 1);
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 0 && peek32(RAC4_LOAD_PLANET2) == 0, "a pick of no planet the game has: nothing");
	poke32(DL_GAME_MODE, 3);
	poke32(mb + MBOX_OFF_PICK_PLANET, 7);
	poke32(mb + MBOX_OFF_PICK_SEQ, peek32(mb + MBOX_OFF_PICK_SEQ) + 1);
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 0 && dl_status(7) == 0, "Valix, picked with the game still paused: nothing yet");
	poke32(DL_GAME_MODE, 0);
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 7 && peek32(RAC4_LOAD_PLANET2) == 1,
	      "back in play: Valix loads, as PLANET_LOAD loads it");
	check(dl_status(7) == RAC4_PLANET_UNLOCK && dl_first_mission(7) == RAC4_PLANET_UNLOCK,
	      "locked, so unlocked first: the level and its first mission visible and unlocked");
	poke32(RAC4_LOAD_PLANET2, 0);
	tick(8);
	check(peek32(RAC4_LOAD_PLANET2) == 0, "taken once");

	/* A pick left from before an install is not one. */
	poke32(mb + MBOX_OFF_PICK_PLANET, 8);
	poke32(mb + MBOX_OFF_PICK_SEQ, peek32(mb + MBOX_OFF_PICK_SEQ) + 1);
	menu_enter(session_game());
	check(dl_installed(), "a module loaded again takes the renderer over");
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 7 && dl_status(8) == 0, "and the pick it finds in the mailbox is not loaded");

	group("console menu: Deadlocked's planet picker, Worlds from PLANETS");

	poke32(DL_LEVEL, 5);
	worlds_entry(0, 0, 0, DL_TYPE_TOURNAMENT);     /* tournament 0, DreadZone Station's own row */
	worlds_entry(1, 0, 2, DL_TYPE_PLANET);         /* Catacrom */
	worlds_entry(2, 1, 4, DL_TYPE_PLANET);         /* Sarathos, locked */
	worlds_entry(3, 1, 0, DL_TYPE_TOURNAMENT);     /* the same row, locked (its hub mission not yet open) */
	worlds_entry(4, 1, 25, DL_TYPE_PLANET);        /* Kronos in co-op, as SetupPlanetTourneyList adds 20 */
	worlds_entry(5, 1, 15, DL_TYPE_PLANET);        /* Interior, listed for the menu only */
	worlds_focus(6, 1);

	/* PLANETS opens Worlds over the root. */
	dl_menu_open();
	check(G.screen == MH_SCREEN_ID && G.shown && G.nbuttons == 7, "the combo: the root");
	G.cursor = planets_row;
	memset(G.sounds, 0xFF, sizeof(G.sounds));
	game_frame(0);
	frames = peek32(mb + MBOX_OFF_FRAMES);
	seq = peek32(mb + MBOX_OFF_PAGE_SEQ);
	game_frame(PADB_CROSS);
	check(G.screen == DL_SCREEN_WORLDS && W.up && W.starts == 1 && G.depth == 1 && G.stack[0] == MH_SCREEN_ID &&
	      !G.shown, "the cross on PLANETS pushes Worlds (screen 3) over screen 36, as the main menu's WORLDS row does");
	check(G.sounds[0] == SND_FORWARD && G.sounds[1] == 5 && G.sounds[2] == -1,
	      "Worlds' own opening sound and a row's forward sound, nothing else");
	check(peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER && mh_picker_up() && box_acked() &&
	      r8(MH_SHIM + SH_WORLDS_OPENS) == 1,
	      "the menu's screen stopped under it with no close: hstate PICKER, the picker up, no event posted");
	tick(8);
	check(peek32(mb + MBOX_OFF_PAGE_SEQ) == seq && menu_model_depth() == 1, "qwark posts nothing and leaves its stack");

	/* Back, with no mission active (Worlds' trap): the root again, on PLANETS. */
	G.sound = -1;
	worlds_press(DL_CMD_TRIANGLE);
	check(G.screen == MH_SCREEN_ID && G.shown && !W.up && G.depth == 0 && W.game_stops == 1,
	      "Triangle pops back to screen 36 though no mission is active: the gate passed for the menu's Worlds");
	check(strcmp(read_str(G.title, title, sizeof(title)), "TRAINER") == 0 && G.nbuttons == 7 &&
	      G.cursor == planets_row, "the root, the cursor on PLANETS");
	check(peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && !mh_picker_up() &&
	      peek32(mb + MBOX_OFF_FRAMES) >= frames && G.sound == 6,
	      "no opening: hstate open again, the frames counting on, Worlds' closing sound and no other");
	tick(4);
	game_frame(0);
	check(G.cursor == planets_row && menu_model_depth() == 1, "and qwark's root as it was");

	/* X on a locked planet: picked, unlocked, loaded once the game plays. */
	game_frame(PADB_CROSS);
	worlds_focus(6, 2);
	poke32(RAC4_TARGET_PLANET, 0);
	poke32(RAC4_LOAD_PLANET2, 0);
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	n = G.closes;
	poke8(DL_MENU_SOUND_FLAG, 0);
	worlds_press(DL_CMD_CROSS);
	check(peek32(mb + MBOX_OFF_PICK_SEQ) == seq + 1 && peek32(mb + MBOX_OFF_PICK_PLANET) == 4 &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && !mh_picker_up(),
	      "X on Sarathos, locked: pick_planet 4 and pick_seq posted, and the menu counts as closed");
	check(G.closes == n + 1 && G.screen == 0 && !G.paused && peek8(DL_MENU_SOUND_FLAG) == 1 &&
	      W.selections == 0 && W.game_stops == 2 && G.sound == 6,
	      "Worlds closes as Start closes it (the sound flag, CloseMenu(-1), its own stop and sound); "
	      "HandleSelection never runs");
	check(peek32(RAC4_TARGET_PLANET) == 0, "nothing loads before qwark's tick");
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 4 && peek32(RAC4_LOAD_PLANET2) == 1 &&
	      dl_status(4) == RAC4_PLANET_UNLOCK && dl_first_mission(4) == RAC4_PLANET_UNLOCK,
	      "the game playing: qwark unlocks Sarathos and loads it");
	check(menu_model_depth() == 1, "qwark's stack on the root");

	/*
	 * A locked tournament gets the game's refusal; an unlocked one is DreadZone.
	 * Round 6: DreadZone Station's own row is the only one the menu's Worlds
	 * lists (tournaments 1 to 4 left out, below).
	 */
	poke32(RAC4_LOAD_PLANET2, 0);
	dl_menu_open();
	check(G.cursor == 0, "the combo opens the root on its first row after the pick's close");
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	worlds_focus(6, 3);
	G.sound = -1;
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	worlds_press(DL_CMD_CROSS);
	check(G.sound == SND_REFUSED && G.screen == DL_SCREEN_WORLDS && peek32(mb + MBOX_OFF_PICK_SEQ) == seq,
	      "DreadZone Station's row locked: the game's own refusal sound, nothing picked, Worlds still up");
	worlds_focus(6, 0);
	worlds_press(DL_CMD_CROSS);
	tick(8);
	check(peek32(mb + MBOX_OFF_PICK_PLANET) == 1 && peek32(RAC4_TARGET_PLANET) == 1 && peek32(RAC4_LOAD_PLANET2) == 1,
	      "DreadZone Station's row unlocked: DreadZone");

	/* Co-op's links are 21..35; qwark's ids the single-player ones. */
	poke32(RAC4_LOAD_PLANET2, 0);
	dl_menu_open();
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	worlds_focus(6, 4);
	worlds_press(DL_CMD_CROSS);
	tick(8);
	check(peek32(mb + MBOX_OFF_PICK_PLANET) == 5 && peek32(RAC4_TARGET_PLANET) == 5 && dl_status(5) == 0x02,
	      "Kronos at link 25: planet 5, already unlocked, so its status untouched");

	/* Interior, which only the menu's Worlds lists. */
	poke32(RAC4_LOAD_PLANET2, 0);
	dl_menu_open();
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	worlds_focus(6, 5);
	worlds_press(DL_CMD_CROSS);
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 15 && dl_status(15) == RAC4_PLANET_UNLOCK,
	      "Interior: unlocked and loaded");

	/* Start ends the picker with no pick. */
	poke32(RAC4_LOAD_PLANET2, 0);
	poke32(RAC4_TARGET_PLANET, 0);
	dl_menu_open();
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	n = W.game_stops;
	worlds_press(DL_CMD_CLOSE);
	check(G.screen == 0 && !G.paused && W.game_stops == n + 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      !mh_picker_up() && peek32(mb + MBOX_OFF_PICK_SEQ) == seq,
	      "Start on the menu's Worlds, no mission active: the gate passed, everything closes, no pick, the menu closed");
	tick(8);
	check(peek32(RAC4_TARGET_PLANET) == 0 && menu_model_depth() == 1, "nothing loads");

	/*
	 * The pickers' round 2, the user's calls: Circle (Skills) does nothing in
	 * the menu's Worlds (mh_gate_skills; round 1 pushed Skills, after which
	 * Worlds was the game's own), and no planet there has a lock beside it,
	 * every one being one a pick loads (mh_gate_locked).
	 */
	dl_menu_open();
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	n = W.game_stops;
	G.sound = -1;
	worlds_press(4);
	check(G.screen == DL_SCREEN_WORLDS && W.up && mh_picker_up() && W.game_stops == n && G.sound == -1 &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER,
	      "Circle on the menu's Worlds: nothing, no Skills screen, no sound; Worlds and the picker stay up");
	worlds_entry(6, 0, 6, DL_TYPE_PLANET);
	worlds_lock(2, 0);                             /* Sarathos, which the save has not unlocked */
	worlds_lock(6, 1);                             /* Shaar, which it has */
	check(r8(WORLDS_ENTRIES + 24) == 0 && r8(WORLDS_ENTRIES + 72) == 0 && r8(WORLDS_ENTRIES + 36) == 1,
	      "and its planets are drawn unlocked, Sarathos too (no lock beside it); a locked tournament keeps its lock");
	/* Round 6, the user's calls: no tournament but DreadZone Station's row, and no SKILLS in the legend. */
	check(worlds_listed(0, DL_TYPE_TOURNAMENT) && worlds_listed(14, DL_TYPE_PLANET) &&
	      worlds_listed(15, DL_TYPE_PLANET) && !worlds_listed(1, DL_TYPE_TOURNAMENT) &&
	      !worlds_listed(4, DL_TYPE_TOURNAMENT),
	      "round 6: the menu's Worlds lists DreadZone Station's own row and every planet, no tournament 1 to 4");
	check(strcmp(worlds_legend(), "\x10 SELECT \x12 BACK") == 0,
	      "and its legend is SELECT and BACK (Triangle always goes back there), no SKILLS");
	worlds_press(DL_CMD_TRIANGLE);
	check(G.screen == MH_SCREEN_ID && G.shown && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && !mh_picker_up(),
	      "Triangle: back to the root as ever");
	game_frame(PADB_START);
	check(G.screen == 0, "Start closes it");
	worlds_entry(2, 1, 4, DL_TYPE_PLANET);
	tick(4);

	/* The game's own Worlds, from the main menu's WORLDS row: nothing of the menu's. */
	game_open_pause();
	mock_push_screen(0, DL_SCREEN_WORLDS);
	check(G.screen == DL_SCREEN_WORLDS && !mh_picker_up(), "the main menu's WORLDS row: the game's own Worlds");
	worlds_lock(2, 0);
	check(r8(WORLDS_ENTRIES + 24) == 1, "Sarathos with its lock there, as the game draws it");
	check(worlds_listed(1, DL_TYPE_TOURNAMENT) && worlds_listed(4, DL_TYPE_TOURNAMENT) &&
	      strcmp(worlds_legend(), "\x11 SKILLS \x10 SELECT") == 0,
	      "round 6: its tournaments listed and its legend the game's, SKILLS first");
	worlds_press(4);
	check(G.screen == 21 && W.game_stops > 0, "Circle: the game's own Skills screen, as ever");
	mock_pop_screen();
	check(G.screen == DL_SCREEN_WORLDS && W.up && !mh_picker_up(), "and back from it to the game's Worlds");
	worlds_focus(6, 2);
	n = W.selections;
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	worlds_press(DL_CMD_CROSS);
	worlds_press(DL_CMD_TRIANGLE);
	worlds_press(DL_CMD_CLOSE);
	check(W.selections == n + 1 && G.screen == DL_SCREEN_WORLDS && peek32(mb + MBOX_OFF_PICK_SEQ) == seq &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "X is HandleSelection's, the trap holds Triangle and Start, nothing is picked");
	W.mission = 2;
	worlds_press(DL_CMD_TRIANGLE);
	check(G.screen == 1 && G.main_up && W.game_stops > 0, "with a mission, back to the main menu, as the game goes");
	mock_close_menu(5);
	W.mission = -1;
	tick(4);

	/* Where the main menu greys WORLDS, PLANETS opens qwark's own page: every planet. */
	poke32(DL_LEVEL, 15);
	dl_menu_open();
	G.cursor = planets_row;
	n = W.starts;
	game_frame(PADB_CROSS);
	check(G.screen == MH_SCREEN_ID && W.starts == n && !mh_picker_up() && !box_acked(),
	      "in Interior (as the main menu greys WORLDS there): no Worlds, an event posted instead");
	tick(2);
	game_frame(0);
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_PLANETS, 0), "and qwark opens its own Planets page");
	{
		struct menu_view v;
		menu_model_view(&v);
		check(view_index(&v, "Sarathos") >= 0 || view_index(&v, "DreadZone") >= 0,
		      "which lists the planets, unlocked or not (round 2 left the locked ones out)");
	}
	game_frame(PADB_START);
	tick(4);
	poke32(DL_LEVEL, 21);
	poke8(DL_DZ_GATE, 0);
	dl_menu_open();
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	check(G.screen == MH_SCREEN_ID && !mh_picker_up(), "in DreadZone (co-op's too) before its mission 37: the same");
	tick(2);
	game_frame(0);
	game_frame(PADB_START);
	tick(4);
	poke8(DL_DZ_GATE, 1);
	dl_menu_open();
	G.cursor = planets_row;
	game_frame(PADB_CROSS);
	check(G.screen == DL_SCREEN_WORLDS && mh_picker_up(), "and once it has a completion, Worlds");
	worlds_press(DL_CMD_CLOSE);
	tick(4);

	check(quit(), "quit Deadlocked");
}

void test_menu(void)
{
	test_model_dl();
	test_model_paging();
	test_mailbox();
	test_renderer();
	test_picker();
	test_menu_core();
	test_menu_uya();
	test_menu_rac2();
	test_menu_rac1();
}
