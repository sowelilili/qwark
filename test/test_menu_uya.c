/*
 * The console menu in Up Your Arsenal (revision 1.22, mailbox version 2), on
 * qwark's fake console, against the real core: three layers, as test_menu.c
 * has them for Deadlocked.
 *
 *   the model     UYA's pages, built from its menu table (src/games/rac3.c):
 *                 the root, Game, Cheats, Planets in the game's order, Manips
 *                 with the trophy reset and its status, Unlocks with its
 *                 buttons, and the unlock values LOCKED, UNLOCKED and MAX
 *                 (each weapon's own MAX, the R3YNO's v5);
 *   the install   the thirteen words a few seconds into the session in play,
 *                 the refusals, menu.enabled = 0 and the word that says one of
 *                 the menu's GuiStates runs;
 *   the renderer  the shared core and UYA's shim (mh_rac3.c) compiled for the
 *                 PC (MH_HOST), against mocks of the game's pause: the IGE
 *                 thread runner and SwapTo, the main pause's init, update and
 *                 deinit on a row table, the submenu frame, the grid list, the
 *                 Start path and the launcher. The hook stubs are assembler
 *                 and do not run here: src/games/menuhelper/check.py steps
 *                 them, and the mocks below do in C what they do.
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
#include "../src/games/rac3.h"
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

#include "../src/games/menuhelper/mh_rac3.h"
#include "../src/games/menuhelper/mh_common.h"
#include "../src/games/menuhelper/mh_shim.h"

/* Where the fake game keeps what the mocks need. */
#define U_PAD          0x00D992C0u      /* port 0's pad */
#define U_GAME_STATE   0x00CC0000u      /* what *U_GAME_PTR points at */
#define MAIN_GS        0x01385B50u      /* the main pause's GuiState */
#define EXIT_GS        0x0137A1C8u      /* the pause exit's */
#define HELP_GS        0x01385860u      /* HELP's */
#define GRID0          0x0137C208u      /* grid 0 */
#define MSG_AT         0x00A80000u      /* strings msg_string answers */
#define TOK_EXIT       0x000000E1u
#define TOK_HELP       0x000000E2u
#define GAME_TABLE8    0x00C527C8u

/*
 * The game's pause as far as the renderer can see it: the thread runner's one
 * slot (the active GuiState and the one SwapTo queued), the main pause's list,
 * the submenu frame, grid 0, and what the widgets were given.
 */
#define NTEXT 64
static struct {
	u32 active, pending;
	int paused;
	int gate_closed;
	int closing;              /* the pause's Start-close request (0x2D21C8) */
	/* the main pause's list */
	u32 table;
	int count, main_up, main_inits, main_deinits, hang;
	float band[2];
	u32 game_row_cb;
	int game_help;
	/* the frame */
	int frame_up;
	u32 title;
	int hint_a, hint_b;
	/* grid 0 */
	int built, rows, vis, cols, top, off, aligned, set_cursor;
	u32 buf;
	int grid_bad;
	int sound, sounds;
	u32 text_h[NTEXT], text_s[NTEXT];
} G;

static void remember(u32 h, u32 s)
{
	int i;
	for (i = 0; i < NTEXT; i++) {
		if (G.text_h[i] == h || G.text_h[i] == 0) {
			G.text_h[i] = h;
			G.text_s[i] = s;
			return;
		}
	}
}

static u32 recall(u32 h)
{
	int i;
	for (i = 0; i < NTEXT && G.text_h[i] != 0; i++) {
		if (G.text_h[i] == h) return G.text_s[i];
	}
	return 0;
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

static const char *text_of(u32 h, char *out, u32 cap) { return read_str(recall(h), out, cap); }

static void put_str(u32 a, const char *s)
{
	u32 i;
	for (i = 0; i <= strlen(s); i++) w8(a + i, (u8)s[i]);
}

/*
 * msg_string with the label hook in: what mh_msg_stub answers (check.py steps
 * the real one), else a game text.
 */
static u32 game_msg(u32 id)
{
	if ((id & ~0xFu) == MH_MSG_BASE) {
		u32 i = id & 0xF;
		if (i == 0) return MSG_AT;                     /* "TRAINER" */
		if (i <= MH_MAIN_ROWS) return mh_text(i - 1);  /* the set on show (round 3) */
	}
	return id == 0x106 ? MSG_AT + 0x10 : MSG_AT + 0x20; /* "HELP", "ROW" */
}

static void mock_swap_to(u32 gs) { G.pending = gs; }
static void mock_reset_state(u32 gs) { w32(gs, 0); }
static void mock_set_text(u32 h, u32 s) { remember(h, s); }
static void mock_play_sound(int id, int a, int b) { (void)a; (void)b; G.sound = id; G.sounds++; }
static int mock_start_close(void) { return G.closing; }
static void mock_start_ack(void) { G.closing = 0; }

static u32 mock_main_pause(void)
{
	w32(MAIN_GS + 4, OPD_MAIN_INIT);
	w32(MAIN_GS + 8, OPD_MAIN_DEINIT);
	w32(MAIN_GS + 12, OPD_MAIN_UPDATE);
	return MAIN_GS;
}

static u32 mock_pause_exit(void)
{
	w32(EXIT_GS + 4, TOK_EXIT);
	return EXIT_GS;
}

/*
 * The main pause's init (0x2F4160) on the table its rows stubs give it: the
 * shim's while its table word is up (mh_rows*_stub), the game's eight rows
 * otherwise; the cursor clamped, each row's label through msg_string.
 */
static void mock_main_init(u32 gs)
{
	u32 band, i, c;

	(void)gs;
	if (r32(MH_SHIM + SH_LIST_TABLE) != 0) {
		G.table = r32(MH_SHIM + SH_LIST_TABLE);
		G.count = (int)r32(MH_SHIM + SH_LIST_COUNT);
		band = MH_SHIM + SH_BAND;
	} else {
		G.table = GAME_TABLE8;
		G.count = 8;
		band = 0x00C529D0u;
	}
	c = r32(U_CURSOR);
	if ((int)c > G.count) c = (u32)G.count;
	w32(U_CURSOR, c);
	for (i = 0; i < (u32)G.count; i++) {
		u32 row = G.table + 20 * i;
		remember(r32(row), game_msg(r32(row + 4)));
	}
	G.band[0] = rf32(band);
	G.band[1] = rf32(band + 4);
	G.main_up = 1;
	G.main_inits++;
}

static void mock_main_deinit(u32 gs)
{
	int i;
	(void)gs;
	G.main_up = 0;
	G.main_deinits++;
	for (i = 0; i < 9; i++) remember(U_ROW_HANDLE + (u32)i, 0);
}

static void mock_main_update(u32 gs);

/* The submenu frame builder and the canvas layers. */
static void mock_frame(u32 layer, u32 cont, u32 h0, u32 h1, u32 h2, u32 h3, u32 h4, u32 title_h, u32 h8,
                       u32 title, u32 h6, u32 hint_a, u32 h7, u32 hint_b, float f)
{
	(void)layer; (void)cont; (void)h0; (void)h1; (void)h2; (void)h3; (void)h4; (void)h8; (void)h6;
	(void)h7; (void)f;
	G.frame_up = 1;
	G.title = title;
	G.hint_a = (int)hint_a;
	G.hint_b = (int)hint_b;
	remember(title_h, title);
}

static void mock_clear_layer(int layer) { if (layer == U_LAYER_FRAME) G.frame_up = 0; }

/* Grid 0, as 0x2D5D14 .. 0x2D76E0 have it. */
static u32 mock_get_grid(int i) { (void)i; return GRID0; }
static u32 cell(int col, int row) { return G.buf + (u32)(row * G.cols + col) * U_CELL_SIZE; }
static int cell_color(int row) { return (int)r32(cell(0, row) + 64); }

static void mock_grid_build(u32 g, u32 cfg)
{
	u32 i, size = r32(cfg);

	(void)g;
	G.rows = (int)r32(cfg + 12);
	G.vis = (int)r32(cfg + 16);
	G.cols = (int)r32(cfg + 20);
	if ((u32)(G.rows * G.cols) * U_CELL_SIZE >= size) {
		G.grid_bad = 1;
		return;
	}
	G.buf = r32(cfg + 4);
	for (i = 0; i < (u32)(G.rows * G.cols) * U_CELL_SIZE; i++) w8(G.buf + i, 0);
	G.top = G.off = 0;
	G.built = 1;
	G.aligned = 0;
}

static u32 mock_grid_cell(u32 g, int col, int row) { (void)g; return cell(col, row); }

static void mock_grid_color(u32 g, int row, int idx)
{
	int c;
	(void)g;
	for (c = 0; c < G.cols; c++) w32(cell(c, row) + 64, (u32)idx);
}

static void mock_grid_align(u32 g, int col, int x, int y) { (void)g; (void)col; (void)x; (void)y; G.aligned++; }
static void mock_frame_rect(u32 g, float a, float b, float c, float d, float e) { (void)g; (void)a; (void)b; (void)c; (void)d; (void)e; }
static void mock_cursor_bar(u32 g, float a, float b, float c, float d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
static void mock_grid_free(u32 g) { (void)g; G.built = 0; }
static int mock_grid_cursor(u32 g) { (void)g; return G.top + G.off; }

static void mock_grid_set_cursor(u32 g, int row)
{
	(void)g;
	if (row >= G.rows) return;
	G.top = row >= G.vis ? row - G.vis + 1 : 0;
	G.off = row - G.top;
	G.set_cursor++;
}

/* Up and Down: no wrap, a row of colour 4 skipped, the rows scrolled to keep the cursor on screen. */
static void mock_grid_update(u32 g, int edges)
{
	int pos = G.top + G.off, r;

	(void)g;
	if ((edges & PADB_UP) != 0 && pos > 0) {
		for (r = pos - 1; r > 0 && cell_color(r) == U_COLOR_SKIP; r--) { }
		if (cell_color(r) != U_COLOR_SKIP) {
			if (r < G.top) G.top = r;
			G.off = r - G.top;
		}
		mock_play_sound(3, 0, 0);
	} else if ((edges & PADB_DOWN) != 0 && pos < G.rows - 1) {
		for (r = pos + 1; r < G.rows - 1 && cell_color(r) == U_COLOR_SKIP; r++) { }
		if (cell_color(r) != U_COLOR_SKIP) {
			if (r >= G.top + G.vis) G.top = r - G.vis + 1;
			G.off = r - G.top;
		}
		mock_play_sound(3, 0, 0);
	}
}

#include "../src/games/menuhelper/mh_core.c"
#include "../src/games/menuhelper/mh_rac3.c"

static void call_row(u32 cb)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC3);

	if (cb == MH_FN_ROW) mh_row();
	else if (d != NULL && cb == d->hooks[8].value) mh_help();
	else if (cb == MH_ORIG_HELP_CB) { G.game_help++; mock_swap_to(HELP_GS); }
	else G.game_row_cb = cb;
}

/*
 * The main pause's update (0x2F4830): Down and Up skip a row that is not
 * enabled, and loop for ever when none is (the mock gives up and says so),
 * the cross calls the row's callback and resets the list after when the row
 * asks, Triangle leaves the pause, and the Start close.
 */
static void mock_main_update(u32 gs)
{
	u32 e = r32(U_PAD + U_PAD_EDGES), c = r32(U_CURSOR), row;
	int guard;

	if ((e & (PADB_DOWN | PADB_UP)) != 0) {
		for (guard = 0; guard < 100; guard++) {
			if ((e & PADB_DOWN) != 0) c = (c + 1) % (u32)G.count;
			else c = (c + (u32)G.count - 1) % (u32)G.count;
			if (r8(G.table + 20 * c + 8) != 0) break;
		}
		if (guard == 100) G.hang = 1;
		w32(U_CURSOR, c);
		mock_play_sound(3, 0, 0);
	} else if ((e & r32(U_CONFIRM_MASK)) != 0) {
		row = G.table + 20 * c;
		if (r8(row + 8) != 0 && r32(row + 12) != 0) {
			call_row(r32(row + 12));
			if (r8(row + 16) != 0) mock_reset_state(gs);
			mock_play_sound(4, 0, 0);
		}
	} else if ((e & PADB_TRIANGLE) != 0) {
		mock_swap_to(mock_pause_exit());
		mock_reset_state(gs);
	}
	if (mock_start_close()) {
		mock_start_ack();
		mock_reset_state(gs);
		if (r8(U_NO_EXIT) == 0) mock_swap_to(mock_pause_exit());
	}
}

/* ------------------------------------------------------- the thread runner */

static void call_gs(u32 gs, int which)
{
	u32 tok = r32(gs + 4 + 4 * (u32)which);

	if (tok == MH_FN_INIT) mh_init(gs);
	else if (tok == MH_FN_DEINIT) mh_deinit(gs);
	else if (tok == MH_FN_UPDATE) mh_update(gs);
	else if (tok == OPD_MAIN_INIT) mock_main_init(gs);
	else if (tok == OPD_MAIN_DEINIT) mock_main_deinit(gs);
	else if (tok == OPD_MAIN_UPDATE) mock_main_update(gs);
}

/*
 * One pass of the runner (0x91AF8): an active GuiState reset is ended
 * (deinit), a queued one started (init) and run (update). The pause exit
 * ends the pause.
 */
static void runner(void)
{
	if (G.active != 0 && r32(G.active) == 0) {
		call_gs(G.active, 1);
		G.active = 0;
	}
	if (G.pending != 0) {
		G.active = G.pending;
		G.pending = 0;
		if (G.active == EXIT_GS) {
			G.active = 0;
			G.paused = 0;
			return;
		}
		if (G.active == HELP_GS) return;
		call_gs(G.active, 0);
		w32(G.active, 1);
	}
	if (G.active != 0 && r32(G.active) == 1) call_gs(G.active, 2);
}

/* A frame of the pause with `edges` pressed this frame. */
static void pframe(u32 edges)
{
	w32(U_PAD + U_PAD_EDGES, edges);
	runner();
	w32(U_PAD + U_PAD_EDGES, 0);
}

/*
 * A frame of play: the mode 0 update's Start test with the inject stub's logic
 * (check.py steps the real one), the gate, the combo stub, the pause launcher
 * calling through mh_launch_stub (mh_launch), SwapTo and the runner's first
 * pass. `start` is the player's own Start.
 */
static void play_frame(int start)
{
	u32 req = r8(MH_BOX + MBOX_OFF_REQUEST), r6 = start ? PADB_START : 0;

	w8(MH_SHIM + SH_PRESSED, 0);
	if (req != 0) {
		w8(MH_BOX + MBOX_OFF_REQUEST, 0);
		if (req == MBOX_REQ_OPEN && r8(U_GAME_STATE + U_GAME_MP) == 0 &&
		    !(r32(U_PLANET) >= U_COMIC_FIRST && r32(U_PLANET) <= U_COMIC_LAST)) {
			r6 = PADB_START;
			w8(MH_SHIM + SH_PRESSED, 1);
			mh_count(ST_OPEN_REQS);
		}
	}
	if (r6 == 0 || G.gate_closed || G.paused) return;
	w8(MH_SHIM + SH_FORCE, r8(MH_SHIM + SH_PRESSED));
	G.paused = 1;
	mock_swap_to(mh_launch());
	runner();
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

static int hooks_are(const struct mh_desc *d, int ours)
{
	u8 i;
	for (i = 0; i < d->nhooks; i++) {
		u32 want = ours ? d->hooks[i].value : d->hooks[i].original;
		if (peek32(d->hooks[i].addr) != want) return 0;
	}
	return 1;
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

static int act(struct menu_view *v, u8 kind, const char *label)
{
	int i = view_index(v, label), rc;

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

static int labels_are(const struct menu_view *v, const char * const *want, u8 n)
{
	u8 i;
	if (v->nrows != n) return 0;
	for (i = 0; i < n; i++) {
		if (strcmp(v->rows[i].label, want[i]) != 0) return 0;
	}
	return 1;
}

/* UYA in play: gameMode 0, no mode change queued, the pad, the confirm mask, single player. */
static void uya_playing(void)
{
	poke32(RAC3_GAME_STATE, 0);
	poke32(U_PENDING_MODE, U_MODE_NONE);
}

/* The weapon `id` of rac3_panel.c's table at `version` (0 locked). */
#define R3_LAVA_GUN   28
#define R3_R3YNO      36
#define R3_AGENTS     21
static void uya_weapon(u32 unlock_off, u32 item_id, int version)
{
	poke8(RAC3_UNLOCK_ARRAY + unlock_off - 0x4A8u, version > 0 ? 1 : 0);
	poke8(RAC3_ITEM_ARRAY + item_id, (u8)(version > 0 ? item_id + (u32)version - 1 : item_id));
}

#define CAPS_ALL (MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST)

/* ------------------------------------------------------------- the model */

static void test_model_uya(void)
{
	static const char * const root[] = { "Game", "Cheats", "Planets", "Manips", "Unlocks", "Save files",
	                                     "qwark build 54" };
	static const char * const game[] = { "Die", "Position slot", "Save position", "Load position",
	                                     "Set aside file", "Load set-aside file", "Bolts", "Challenge mode",
	                                     "Health XP", "Health", "File time" };
	static const char * const cheats[] = { "Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health",
	                                       "Quick-select pause" };
	static const char * const manips[] = { "Category", "Auto-reset", "Remember", "Apply now", "Reset trophies",
	                                       "Trophy reset", "Reset trophies on Veldin load" };
	static const char * const unlocks[] = { "Weapons", "Gadgets and items", "Vid comics", "Collectables" };
	static const char * const collectables[] = { "Max all weapon levels", "Reset all weapon levels",
	                                             "Unlock all skill points", "Reset all skill points",
	                                             "Unlock all titanium bolts", "Reset all titanium bolts" };
	struct menu_view v;
	const struct menu_row *r;
	u8 i;

	group("console menu: the model, UYA's pages");

	check(boot("NPEA00387"), "UYA boots");
	menu_model_reset(9, 16, CAPS_ALL);
	menu_model_view(&v);
	check(labels_are(&v, root, 7) && v.ptype == MBOX_PTYPE_MAIN && v.rows[6].kind == MROW_INFO,
	      "the root: Game, Cheats, Planets, Manips, Unlocks, Save files, and the build, grey: the design's");

	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(labels_are(&v, game, 11) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Game: a CONFIG page, the quick block and the client's Values (no QE offset, no cosmetics)");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_VALUE, "Bolts is a stepper");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC3_BOLTS) == 1, "and Right adds one");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Cheats");
	check(labels_are(&v, cheats, 5) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Cheats: the four cheats every game has, then the quick-select pause");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0,
	      "Ghost Ratchet: the cross turns it On");
	r = view_row(&v, "Quick-select pause");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0,
	      "the quick-select pause, a LIVE toggle, is Off or On");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: the plain list, in the game's order, nine a window. */
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.ptype == MBOX_PTYPE_MAIN && strcmp(v.rows[0].label, "Veldin") == 0 &&
	      strcmp(v.rows[5].label, "Obani Gemini") == 0 && v.rows[8].kind == MROW_MORE && v.nrows == 9,
	      "Planets: Veldin, Florana, Starship Phoenix, Marcadia, Daxx, Obani Gemini ..., eight and More");
	check((v.rows[0].flags & MROWF_CLOSES) != 0 && v.rows[0].kind == MROW_ACTION, "each a button that closes the menu first");
	for (i = 0; i < 6 && v.rows[v.nrows - 1].kind == MROW_MORE; i++) act(&v, MBOX_EV_ACTIVATE, "More");
	check(view_row(&v, "Vid Comic 1 SE") != NULL && view_row(&v, "(unused)") == NULL && view_row(&v, "(none)") == NULL,
	      "the last window ends with the vid comics; the gaps and the placeholder are left out");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Manips, with the Trophies header and the reset's status under its button. */
	act(&v, MBOX_EV_ACTIVATE, "Manips");
	check(labels_are(&v, manips, 7) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Manips: the category, Auto-reset, Remember, Apply now, then Reset trophies, its status and the toggle");
	r = view_row(&v, "Trophy reset");
	check(r != NULL && r->kind == MROW_INFO && (r->flags & MROWF_ENABLED) == 0 && strcmp(r->value, "Not this boot") == 0,
	      "the status, grey: no reset this boot");
	check(strcmp(value_of(&v, "Category"), "None") == 0 && act(&v, MBOX_EV_RIGHT, "Category") == ST_OK &&
	      strcmp(value_of(&v, "Category"), "NG+ no QE") == 0, "Category: Right goes from None to NG+ no QE");
	act(&v, MBOX_EV_LEFT, "Category");
	r = view_row(&v, "Reset trophies on Veldin load");
	check(r != NULL && r->kind == MROW_TOGGLE && act(&v, MBOX_EV_ACTIVATE, "Reset trophies on Veldin load") == ST_OK &&
	      (features_toggle_state() & ((u64)1 << R3_AUTO_TROPHIES)) != 0, "the auto toggle turns on from the menu");
	act(&v, MBOX_EV_LEFT, "Reset trophies on Veldin load");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Unlocks: the categories and a page of the six buttons; the values LOCKED, UNLOCKED, MAX. */
	act(&v, MBOX_EV_ACTIVATE, "Unlocks");
	check(labels_are(&v, unlocks, 4) && v.ptype == MBOX_PTYPE_MAIN && v.rows[3].kind == MROW_LINK,
	      "round 3: Unlocks is Weapons, Gadgets and items, Vid comics and a link, Collectables");
	act(&v, MBOX_EV_ACTIVATE, "Collectables");
	check(labels_are(&v, collectables, 6) && v.ptype == MBOX_PTYPE_CONFIG && strcmp(v.title, "Collectables") == 0 &&
	      v.rows[0].kind == MROW_ACTION && v.rows[5].kind == MROW_ACTION,
	      "Collectables: a CONFIG page of the six buttons, their long labels in its label column");
	check(act(&v, MBOX_EV_ACTIVATE, "Unlock all skill points") == ST_OK &&
	      strcmp(value_of(&v, "Unlock all skill points"), "done") == 0, "each runs, and says done");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	check(labels_are(&v, unlocks, 4) && v.cursor == 3, "back: Unlocks, the cursor on Collectables");
	act(&v, MBOX_EV_ACTIVATE, "Weapons");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 16 && v.rows[15].kind == MROW_MORE && strcmp(v.title, "Weapons 1/2") == 0,
	      "Weapons: twenty, fifteen and More in a page of sixteen");
	uya_weapon(0x4FF, 0x57, 0);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Agents of Doom"), "LOCKED") == 0, "a locked weapon: LOCKED");
	check(act(&v, MBOX_EV_RIGHT, "Agents of Doom") == ST_OK && strcmp(value_of(&v, "Agents of Doom"), "UNLOCKED") == 0 &&
	      peek8(RAC3_ITEM_ARRAY + 0x57) == 0x57, "Right: UNLOCKED, owned at v1");
	check(act(&v, MBOX_EV_RIGHT, "Agents of Doom") == ST_OK && strcmp(value_of(&v, "Agents of Doom"), "MAX") == 0 &&
	      peek8(RAC3_ITEM_ARRAY + 0x57) == 0x57 + 7, "Right: MAX, v8");
	check(act(&v, MBOX_EV_RIGHT, "Agents of Doom") == ST_OK && strcmp(value_of(&v, "Agents of Doom"), "LOCKED") == 0,
	      "Right wraps to LOCKED");
	uya_weapon(0x4FF, 0x57, 3);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Agents of Doom"), "V3") == 0, "a version between shows as itself, V3");
	act(&v, MBOX_EV_RIGHT, "Agents of Doom");
	check(peek8(RAC3_ITEM_ARRAY + 0x57) == 0x57 + 7, "and Right steps on to MAX");
	uya_weapon(0x4B9, 0x11, 1);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Lava Gun"), "UNLOCKED") == 0,
	      "an owned GameCube weapon, whose version cannot be read back, shows UNLOCKED");
	act(&v, MBOX_EV_ACTIVATE, "More");
	uya_weapon(0x53F, 0x97, 5);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "R3YNO"), "MAX") == 0, "the R3YNO at v5, its own last: MAX");
	uya_weapon(0x53F, 0x97, 1);
	menu_model_view(&v);
	act(&v, MBOX_EV_RIGHT, "R3YNO");
	check(peek8(RAC3_ITEM_ARRAY + 0x97) == 0x97 + 4 && strcmp(value_of(&v, "R3YNO"), "MAX") == 0,
	      "and MAX writes its v5");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Gadgets and items");
	r = view_row(&v, "Heli Pack");
	check(v.nrows == 15 && r != NULL && (strcmp(r->value, "LOCKED") == 0 || strcmp(r->value, "UNLOCKED") == 0),
	      "Gadgets and items: fifteen, LOCKED or UNLOCKED");

	check(quit(), "quit UYA");
}

/* ------------------------------------------------------------ the install */

static void test_install_uya(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC3);
	u32 mb;
	int i;

	group("console menu: UYA's install and mailbox");

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 13 && d->ncaves == 4,
	      "UYA has a renderer: the mailbox, the state, the read-only data, the code, thirteen words");
	if (d == NULL) return;
	check(d->hooks[0].addr == MH_SITE_MSG && d->hooks[4].addr == MH_SITE_INJECT &&
	      d->hooks[6].addr == MH_SITE_LAUNCH && d->hooks[7].addr == MH_SITE_HELP8_TEXT &&
	      d->hooks[7].value == MH_MSG_HELP && d->hooks[12].addr == MH_SITE_HELP9B_CB &&
	      d->hooks[8].value == d->hooks[10].value && d->hooks[10].value == d->hooks[12].value,
	      "the label hook first, the HELP rows' text and callback last, one callback for all three tables");
	check(d->mode_addr == RAC3_GAME_STATE && d->mode_paused == 3 && d->pending_addr == U_PENDING_MODE &&
	      d->screen_addr == MH_SHIM + SH_UP && d->screen_id == U_SCREEN_UP,
	      "the menu is down when gameMode is not 3, nothing is queued and the shim's word says none of its "
	      "GuiStates runs");

	check(boot("NPEA00387"), "UYA boots");
	uya_playing();
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_BOX) == 0, "two seconds in: nothing yet");
	poke32(RAC3_GAME_STATE, 3);
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "four, paused: still nothing");
	poke32(RAC3_GAME_STATE, 0);
	for (i = 0; i < 40 && menu_state() != MENU_READY; i++) {
		host_write_log_reset();
		tick(1);
	}
	check(menu_state() == MENU_READY && hooks_are(d, 1), "in play again, the renderer goes in, all thirteen words");
	{
		u32 k, a = 0, l = 0, n = host_write_log_count(), bytes = 0, at_code = 999, first_hook = 999;
		for (k = 0; k < n && k < 64; k++) {
			host_write_log_at(k, &a, &l);
			bytes += l;
			if (a == MH_CAVE) at_code = k;
			if (a == MH_SITE_MSG && first_hook == 999) first_hook = k;
		}
		check(at_code < first_hook && n <= 24 && bytes <= 18u * 1024u,
		      "the caves before the words: twenty-two writes, under 18 KB");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 16 &&
	      peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 && peek8(mb + MBOX_OFF_MAX_ROWS) == 9 &&
	      peek8(mb + MBOX_OFF_CFG_ROWS) == 16 && peek8(mb + MBOX_OFF_RENDERER) == 3,
	      "the mailbox: sixteen rows of 0x40, nine a MAIN page, sixteen a CONFIG page, renderer 3");
	check(peek32(MH_SITE_HELP8_TEXT) == 0xF000 && peek32(MH_SITE_HELP9A_TEXT) == 0xF000 &&
	      peek32(MH_SITE_HELP9B_TEXT) == 0xF000, "each table's HELP row says TRAINER");

	/* menu.enabled = 0: every word back once the menu is down and the pause closed. */
	config_set_u32("menu.enabled", 0);
	poke32(MH_SHIM + SH_UP, 1);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "menu.enabled = 0 while one of its GuiStates runs: nothing yet");
	poke32(MH_SHIM + SH_UP, 0);
	poke32(RAC3_GAME_STATE, 3);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "nor in the pause");
	poke32(RAC3_GAME_STATE, 0);
	tick(8);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_SITE_HELP8_TEXT) == 0x106 &&
	      peek32(MH_SITE_HELP8_CB) == MH_ORIG_HELP_CB && peek32(MH_SITE_MSG) == MH_ORIG_MSG,
	      "in play: every word the game's again, HELP its own row");
	config_set_u32("menu.enabled", 1);
	tick(4);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "menu.enabled = 1: back in at once");
	check(quit(), "quit UYA");

	/* Refused: a prototype's word at a site. */
	host_boot("NPEA00387");
	poke32(MH_SITE_MSG, 0x48A80310u);           /* probe N3b's b n3b_msg_stub */
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	uya_playing();
	tick(4 * 120);
	check(menu_state() == MENU_REFUSED && peek32(MH_BOX) == 0 && peek32(MH_SITE_HELP8_TEXT) == 0x106 &&
	      peek32(MH_CAVE) == 0, "probe N3b's label hook in the game: the install writes nothing");
	poke32(MH_SITE_MSG, MH_ORIG_MSG);
	tick(11 * 120);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "unloaded, the renderer goes in ten seconds on");
	check(quit(), "quit UYA");

	/* The combo before the install is due puts it in at once. */
	menu_set_install_delay_us(60000000ull);
	check(boot("NPEA00387"), "UYA boots");
	uya_playing();
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_READY && hooks_are(d, 1) && peek8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "a press of the combo installs at once and asks the renderer to open");
	menu_set_install_delay_us(3000000ull);
	check(quit(), "quit UYA");

	/* RPCS3: nothing at all. */
	host_set_can_patch_code(0);
	check(boot("NPEA00387"), "UYA boots under RPCS3");
	uya_playing();
	tick(4 * 120);
	check(menu_state() == MENU_OFF && peek32(MH_BOX) == 0 && hooks_are(d, 0), "no menu under RPCS3");
	check(quit(), "quit");
	host_set_can_patch_code(1);
}

/* ----------------------------------------------------------- the renderer */

/* The game's eight-row main pause table, which the fake console does not hold (bar the HELP words). */
static void seed_game_table(void)
{
	static const u32 texts[8] = { 0x174, 0x102, 0x103, 0x104, 0x105, 0x107, 0x106, 0x522 };
	u32 i, row;

	for (i = 0; i < 8; i++) {
		row = GAME_TABLE8 + 20 * i;
		poke32(row, U_ROW_HANDLE + i);
		if (i != 6) poke32(row + 4, texts[i]);
		poke32(row + 8, 0x01000000u);
		if (i != 6) poke32(row + 12, 0x00BDE400u + 8 * i);
		poke32(row + 16, 0x01000000u);
	}
}

static const char *row_text(int i, char *out) { return text_of(U_ROW_HANDLE + (u32)i, out, 80); }
static const char *cfg_label(int i, char *out) { return read_str(cell(0, i), out, 80); }
static const char *cfg_value(int i, char *out) { return read_str(cell(1, i), out, 80); }

static void test_renderer_uya(void)
{
	char a[80], b[80];
	u32 mb;
	int i, n;

	group("console menu: UYA's renderer, core and shim, on the fake console");

	memset(&G, 0, sizeof(G));
	check(boot("NPEA00387"), "UYA boots");
	uya_playing();
	seed_game_table();
	poke32(U_PAD_PTR, U_PAD);
	poke32(U_CONFIRM_MASK, 0x40);
	poke32(U_GAME_PTR, U_GAME_STATE);
	poke32(U_PLANET, 5);
	put_str(MSG_AT, "TRAINER");
	put_str(MSG_AT + 0x10, "HELP");
	put_str(MSG_AT + 0x20, "ROW");
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	check(menu_state() == MENU_READY, "the renderer is in");
	mb = menu_mailbox();

	/* The HELP row: TRAINER in its place, opening the menu over the main pause. */
	play_frame(1);
	check(G.paused && G.active == MAIN_GS && G.main_up && G.table == GAME_TABLE8 && G.count == 8 &&
	      strcmp(row_text(6, a), "TRAINER") == 0 && strcmp(row_text(5, b), "ROW") == 0,
	      "Start: the game's main pause, eight rows, the seventh TRAINER where HELP was");
	w32(U_CURSOR, 6);
	pframe(0x40);
	check(G.pending == MH_SHIM + SH_GS && r32(MAIN_GS) == 0 && r8(MH_SHIM + SH_FROM_HELP) == 1,
	      "the cross on TRAINER: the menu's GuiState queued, the main pause reset as after HELP");
	pframe(0);
	check(G.active == MH_SHIM + SH_GS && G.main_up && G.table == MH_SHIM + SH_ROWS && G.count == 7 &&
	      r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN,
	      "the next frame: the main pause's init on the menu's table, seven rows, the mailbox open");
	check(strcmp(row_text(0, a), "GAME") == 0 && strcmp(row_text(5, b), "SAVE FILES") == 0 &&
	      strcmp(text_of(U_ROW_HANDLE + 6, a, 80), "QWARK BUILD 54") == 0 &&
	      r8(MH_SHIM + SH_ROWS + 6 * 20 + 8) == 0 && r8(MH_SHIM + SH_ROWS + 8) == 1,
	      "GAME .. SAVE FILES in capitals, then QWARK BUILD 54, not enabled");
	check(G.band[0] > 0.189f && G.band[0] < 0.191f && G.band[1] > 0.819f && G.band[1] < 0.821f &&
	      r32(U_CURSOR) == 0, "the frame's band for seven rows, 0.19 and 0.82; the cursor on GAME");
	pframe(PADB_TRIANGLE);
	check(G.pending == MAIN_GS && G.sound == SND_CHANGE, "Triangle at the root: back to the main pause");
	pframe(0);
	check(G.active == MAIN_GS && G.table == GAME_TABLE8 && r32(U_CURSOR) == 6 && r32(MH_SHIM + SH_UP) == 0 &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "the main pause again, its cursor back on TRAINER, the menu down");
	G.closing = 1;
	pframe(0);
	pframe(0);
	check(!G.paused && G.active == 0, "Start closes the pause");
	tick(4);

	/* The combo: through the Start path and the launcher, straight onto the root. */
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "the combo asks the renderer to open");
	play_frame(0);
	check(G.paused && G.active == MH_SHIM + SH_GS && G.count == 7 && peek8(mb + MBOX_OFF_REQUEST) == 0 &&
	      r32(MH_STATE + ST_OPENS) == 1 && r8(MH_SHIM + SH_FROM_HELP) == 0 && r8(MH_SHIM + SH_FORCE) == 0,
	      "the next frame of play opens the pause on the menu's root, counted, not from HELP");
	tick(4);

	/* CHEATS: the two-column page, the screen started again with the grid. */
	w32(U_CURSOR, 1);
	pframe(0x40);
	check(G.sound == SND_CHANGE && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "the cross on CHEATS posts an event; the main pause plays its own sound");
	tick(2);
	pframe(0);
	check(G.pending == MH_SHIM + SH_GS + MH_GS_SIZE, "the CONFIG page: the other GuiState queued");
	check(strcmp(row_text(0, a), "GAME") == 0 && strcmp(row_text(1, b), "CHEATS") == 0 &&
	      strcmp(row_text(6, a), "QWARK BUILD 54") == 0,
	      "round 3: the frame it draws before the runner swaps still reads the root's rows, not the Cheats page");
	pframe(0);
	check(!G.main_up && G.main_deinits >= 2 && G.frame_up && G.built && G.rows == 5 && G.vis == 5 &&
	      G.cols == 2 && G.aligned == 2 && G.set_cursor >= 1,
	      "the main pause's list ends, the frame and a grid of five rows begin, both columns aligned");
	check(strcmp(read_str(G.title, a, 80), "CHEATS") == 0 && G.hint_a == 292 && G.hint_b == 3285,
	      "titled CHEATS, with SELECT and BACK");
	check(strcmp(cfg_label(0, a), "FAST LOADS") == 0 && strcmp(cfg_value(0, b), "OFF") == 0 &&
	      strcmp(cfg_label(4, a), "QUICK-SELECT PAUSE") == 0 && cell_color(1) == U_COLOR_OFF,
	      "labels and values in capitals, an off cheat in the normal colour");
	pframe(PADB_DOWN);
	check(G.top + G.off == 1, "Down moves the grid's cursor");
	pframe(PADB_RIGHT);
	check(G.sound == SND_CHANGE && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "Right on Ghost Ratchet posts an event, with the pause's change sound");
	tick(2);
	pframe(0);
	check(strcmp(cfg_value(1, a), "ON") == 0 && cell_color(1) == U_COLOR_ON && G.top + G.off == 1 &&
	      (features_toggle_state() & ((u64)1 << R3_GHOST)) != 0,
	      "the same page refreshed in place: ON, CHEATS' colour of an active cheat, the cursor where it was");
	pframe(0x40);
	tick(2);
	pframe(0);
	check(strcmp(cfg_value(1, a), "AUTO") == 0, "the cross: AUTO");
	pframe(PADB_LEFT);
	tick(2);
	pframe(0);
	pframe(PADB_LEFT);
	tick(2);
	pframe(0);
	check(strcmp(cfg_value(1, a), "OFF") == 0 && (features_toggle_state() & ((u64)1 << R3_GHOST)) == 0,
	      "Left twice: ON, then OFF");

	/* Back: the root, on the main pause's list again, the cursor on CHEATS. */
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.active == MH_SHIM + SH_GS && G.main_up && !G.frame_up && !G.built && G.count == 7 &&
	      r32(U_CURSOR) == 1, "Triangle: the grid and frame go, the root's list is back on CHEATS");

	/* GAME: eleven rows, seven on screen, scrolled. */
	w32(U_CURSOR, 0);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.rows == 11 && G.vis == 7, "GAME: a grid of eleven rows, seven on screen");
	for (i = 0; i < 10; i++) pframe(PADB_DOWN);
	check(G.top + G.off == 10 && G.top == 4, "Down to the last row scrolls the grid");
	pframe(PADB_RIGHT);
	tick(2);
	pframe(0);
	check(strcmp(cfg_label(10, a), "FILE TIME") == 0 && peek32(RAC3_FILE_TIME) == 1 && G.top + G.off == 10,
	      "Right on File time steps it, the cursor still there");

	/* Start on the CONFIG page closes the pause, as CHEATS' does. */
	G.closing = 1;
	pframe(0);
	check(G.pending == EXIT_GS && G.closing == 0, "Start: acknowledged, the pause's exit queued");
	pframe(0);
	check(!G.paused && !G.built && !G.frame_up && r32(MH_SHIM + SH_UP) == 0 &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED, "the grid freed, the frame gone, the menu down");
	tick(4);

	/* Round 3: the combo opens on the root, not on that page; again, it closes, through the close request. */
	check(menu_model_depth() == 1, "closed, qwark's stack is back on the root");
	press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(G.main_up && !G.built && G.count == 7 && r32(U_CURSOR) == 0 && strcmp(row_text(0, a), "GAME") == 0,
	      "the combo opens the menu on the root, the cursor on its first row, not on GAME");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with it up asks it to close");
	pframe(0);
	pframe(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0 && !G.built, "which its next frame does");
	tick(4);

	/* The HELP row after a close deep in the menu: the root too, the cursor on its first row. */
	play_frame(1);
	w32(U_CURSOR, 6);
	pframe(0x40);
	pframe(0);
	check(G.main_up && G.count == 7 && r32(U_CURSOR) == 0 && strcmp(row_text(0, a), "GAME") == 0,
	      "TRAINER from the pause opens the root, the cursor on GAME");
	tick(4);
	w32(U_CURSOR, 4);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.main_up && strcmp(row_text(0, a), "WEAPONS") == 0 && strcmp(row_text(3, b), "COLLECTABLES") == 0 &&
	      G.count == 4, "UNLOCKS: WEAPONS .. COLLECTABLES, four rows that fit the list");
	G.closing = 1;
	pframe(0);
	pframe(0);
	check(!G.paused, "Start closes the pause from it");
	tick(4);
	play_frame(1);
	w32(U_CURSOR, 6);
	pframe(0x40);
	pframe(0);
	check(G.main_up && G.count == 7 && r32(U_CURSOR) == 0, "and TRAINER opens the root again");
	tick(4);

	/* A page with no row the cursor may land on: the main pause's list must not loop. */
	check(G.main_up && G.count == 7, "on the root");
	{
		u32 seq = peek32(mb + MBOX_OFF_PAGE_SEQ), r = mb + MBOX_OFF_ROWS;
		u8 page[MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + MBOX_ROW_SIZE];

		memset(page, 0, sizeof(page));
		be32_put(page, 0x7F000001u);
		page[MBOX_OFF_NROWS - MBOX_OFF_PAGE_KEY] = 1;
		memcpy(page + MBOX_OFF_TITLE - MBOX_OFF_PAGE_KEY, "Empty", 5);
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 1);
		host_poke(mb + MBOX_OFF_PAGE_KEY, page, sizeof(page));
		poke8(r + MROW_OFF_KIND, MROW_INFO);
		host_poke(r + MROW_OFF_LABEL, (const u8 *)"Nothing here", 12);
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 2);
	}
	pframe(0);
	pframe(0);
	n = G.hang;
	pframe(PADB_DOWN);
	check(G.count == 1 && strcmp(row_text(0, a), "NOTHING HERE") == 0 && r8(MH_SHIM + SH_ROWS + 8) == 1 &&
	      !G.hang && n == 0, "a page of one grey row keeps it enabled in the table, and Down does not loop");
	G.closing = 1;
	pframe(0);
	pframe(0);
	check(!G.paused && r32(U_CURSOR) == 6, "closed, the main pause's cursor put back as it was kept");

	check(quit(), "quit UYA");
}

void test_menu_uya(void)
{
	test_model_uya();
	test_install_uya();
	test_renderer_uya();
}
