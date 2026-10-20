/*
 * The console menu in Up Your Arsenal (revision 1.22, mailbox version 3), on
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
 *                 deinit on a row table, the submenu frame and its title's
 *                 fit, the grid list with AUDIO / VISUAL's font and rules,
 *                 the widgets the shim makes (containers, rectangles, the
 *                 picture and its source, the sliders), the Start path and the
 *                 launcher, and the GALACTIC MAP the Planets row opens (its
 *                 list build, names, update and every exit of its own). The
 *                 hook stubs
 *                 are assembler and do not run here:
 *                 src/games/menuhelper/check.py steps them, and the mocks
 *                 below do in C what they do.
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
	/* AUDIO / VISUAL's look: the title's fit, the cells' font, the rules, layer 17 */
	float banner, title_sx, title_sy;
	int font[2], rules, layer17;
	/* the widgets the shim makes or changes, by handle */
	struct widget {
		u32 h, parent, color, text, source, picture;
		char kind;                /* C container, R rounded box, Q rectangle, ? only changed */
		int visible, fit, anchor_x, anchor_y, fade;
		float x, y, sx, sy, box_w, box_min;
	} w[96];
	int nw, widgets_bad;
	u32 slide;                    /* the grid's +0x58 while a scroll is in flight */
	/* the picture source and the pause screens' seven picture buffers (round 4) */
	int pic_taken, pic_dropped, pic_busy, pic_stops, disc_waits;
	u32 pic_src, pic_a, pic_b, pic_wad, pic_set, pic_state;
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
	/* The pickers' round 2: the Trainer's map's names, from the data image's table. */
	if ((id & ~0x3Fu) == MH_MSG_PLANET && (id & 0x3F) < MH_MAP_PLANETS) return r32(MH_MAP_NAMES + 4 * (id & 0x3F));
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
	(void)h7;
	G.banner = f;
	G.frame_up = 1;
	G.title = title;
	G.hint_a = (int)hint_a;
	G.hint_b = (int)hint_b;
	remember(title_h, title);
}

static void forget_layer17(void);
static void mock_clear_layer(int layer)
{
	if (layer == U_LAYER_FRAME) G.frame_up = 0;
	if (layer == U_LAYER_FRAME2) forget_layer17();
}

static void grid_mirror(void);

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
	G.font[0] = G.font[1] = 0;
	G.rules = 0;
	grid_mirror();
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

/* The grid's fields in game memory that the shim reads: the top row and the scroll in flight. */
static void grid_mirror(void)
{
	w32(GRID0 + U_GRID_TOP, (u32)G.top);
	w32(GRID0 + U_GRID_SCROLL, 0);
	w32(GRID0 + U_GRID_SLIDE, G.slide);
}

/* GridSetCursor (0x2D6934): the row in the middle of the screen where it can be. */
static void mock_grid_set_cursor(u32 g, int row)
{
	int top;

	(void)g;
	if (row >= G.rows) return;
	top = row - (G.vis / 2 - 1);
	if (top < 0) top = 0;
	if (top > G.rows - G.vis) top = G.rows - G.vis;
	G.top = top;
	G.off = row - G.top;
	G.set_cursor++;
	grid_mirror();
}

static void mock_grid_rules(u32 g, u32 on, float y, float h, float inset)
{
	(void)g; (void)y; (void)h; (void)inset;
	G.rules = (int)on;
}

static void mock_grid_font(u32 g, u32 col, u32 font) { (void)g; if (col < 2) G.font[col] = (int)font; }

/* The title's fit (0x2D4DB0): a banner and a scale for it, as the game measures the title. */
static void mock_title_fit(u32 s, u32 scale, u32 banner)
{
	(void)s;
	wf32(scale, 0.875f);
	wf32(banner, 7.0f);
}

/* The widgets: each handle's record, made on first sight. */
static struct widget *wid(u32 h)
{
	int i;

	for (i = 0; i < G.nw; i++) {
		if (G.w[i].h == h) return &G.w[i];
	}
	if (G.nw == (int)(sizeof(G.w) / sizeof(G.w[0]))) {
		G.widgets_bad = 1;
		return &G.w[0];
	}
	memset(&G.w[G.nw], 0, sizeof(G.w[0]));
	G.w[G.nw].h = h;
	G.w[G.nw].kind = '?';
	G.w[G.nw].visible = 1;
	return &G.w[G.nw++];
}

static int made(u32 h, char kind)
{
	int i;
	for (i = 0; i < G.nw; i++) {
		if (G.w[i].h == h) return G.w[i].kind == kind;
	}
	return 0;
}

static struct widget *create(u32 h, char kind, u32 color, float x, float y, float sx, float sy)
{
	struct widget *p = wid(h);

	if (p->kind != '?') G.widgets_bad = 1;          /* a handle made twice */
	p->kind = kind;
	p->color = color;
	p->x = x; p->y = y; p->sx = sx; p->sy = sy;
	return p;
}

static void mock_container(u32 h) { create(h, 'C', 0, 0, 0, 1, 1); }
static void mock_add_frame(u32 c, u32 h) { wid(h)->parent = c; }
static void mock_round_rect(u32 h, u32 c, float x, float y, float sx, float sy, float ra, float rb)
{
	(void)ra; (void)rb;
	create(h, 'R', c, x, y, sx, sy);
}
static void mock_rect(u32 h, u32 c, float x, float y, float sx, float sy, u32 tex)
{
	(void)tex;
	create(h, 'Q', c, x, y, sx, sy);
}
static void mock_set_visible(u32 h, u32 on) { wid(h)->visible = on != 0; }
static void mock_set_pos(u32 h, float x, float y) { wid(h)->x = x; wid(h)->y = y; }
static void mock_set_color(u32 h, u32 c) { wid(h)->color = c; }
static void mock_set_anchor(u32 h, u32 ax, u32 ay) { wid(h)->anchor_x = (int)ax; wid(h)->anchor_y = (int)ay; }
static void mock_set_layer(u32 layer, u32 h) { if (layer == U_LAYER_FRAME2) G.layer17 = (int)h; }
static void mock_text_box(u32 h, float bw, float least) { wid(h)->box_w = bw; wid(h)->box_min = least; }
static void mock_text_fit(u32 h, u32 on) { wid(h)->fit = on != 0; }

/*
 * The picture (round 4), as AUDIO / VISUAL's init, update and deinit use it:
 * the pause screens' seven picture buffers (0x2D3A4C, 0x2D3B94), picture
 * source i (0x2CCA44), set up only when it is not busy (0x2CCD48) and stopped
 * by its vtable's second entry (0x2CCD38), a rectangle tied to it and told
 * which picture to show, and the disc's reads waited for.
 */
#define PIC_SOURCES    0x01358730u
static u32 mock_pic_buffer(int kind)
{
	if (kind != 0 || G.pic_taken >= 7) return 0;
	G.pic_taken++;
	return 0x30000000u + 0x00100000u * (u32)G.pic_taken;
}
static int mock_pic_buffer_drop(u32 b) { if (b != 0) { G.pic_taken--; G.pic_dropped++; } return 0; }
static u32 mock_pic_source(int i) { return PIC_SOURCES + 0x44u * (u32)i; }
static int mock_pic_source_set(u32 src, u32 a, u32 b, u32 wad, u32 set)
{
	if (G.pic_busy) return 0;
	G.pic_src = src; G.pic_a = a; G.pic_b = b; G.pic_wad = wad; G.pic_set = set; G.pic_state = 1;
	return 1;
}
static int mock_pic_source_stop(u32 src) { if (src == G.pic_src) G.pic_state = 8; G.pic_stops++; return 1; }
static void mock_pic_tie(u32 h, u32 src) { wid(h)->source = src; }
static void mock_pic_show(u32 h, u32 n) { wid(h)->picture = n; }
static void mock_pic_fade(u32 h, int on, int frames) { wid(h)->fade = on ? frames : 0; }
static int mock_disc_wait(int all) { (void)all; G.disc_waits++; return 0; }

/* SetScale: the title's, and any widget's. */
static void mock_set_scale(u32 h, float sx, float sy)
{
	if (h == U_TITLE_HANDLE) { G.title_sx = sx; G.title_sy = sy; }
	wid(h)->sx = sx;
	wid(h)->sy = sy;
}

/* ClearCanvasLayer(17) takes AUDIO / VISUAL's widgets (0x1E00xx) with it. */
static void forget_layer17(void)
{
	int i, n = 0;

	for (i = 0; i < G.nw; i++) {
		if ((G.w[i].h >> 16) != 0x1E) G.w[n++] = G.w[i];
	}
	G.nw = n;
	G.layer17 = 0;
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
	grid_mirror();
}

/* ---------------------------------------------- the GALACTIC MAP, as the game has it */

#define U_UNLOCKED     0x00DA5A94u      /* the save's unlocked planets, u32[60] (UnlockPlanet's list) */
#define U_KIND         0x00C4F370u      /* the pause exit's kind: 1 travels to U_DEST (0x2D22DC sets both) */
#define U_DEST         0x00C4F374u
#define MAP_GS         0x01379F00u      /* the game's own map's GuiState (getter 0x2CE130) */
#define MISSIONS_GS    0x01385A00u      /* MISSIONS (0x2F5758), which Square swaps to */
#define BATTLE_GS      0x0137C658u      /* BATTLE MISSIONS (0x2DECB4), Triangle's on planets 12 and 18 */

/* What mh_map_list_stub hands the build (check.py steps the real stub on the data image's list). */
static const u8 map_list_c[MH_MAP_LIST_LEN] = { MH_MAP_LIST };

static struct {
	int up, inits, deinits, updates, flag_in_build;
	int pic_clash, pic_held;      /* the menu's picture source still set up, its buffers held, as the map began */
	u32 text[30];                 /* each entry's name, as the text id the build asked for */
	u32 exit_kind, exit_dest;     /* the pause exit's kind and planet when it ran */
	int missions, battle;         /* Square's and Triangle's other screens started */
} M;

/* The build's filter (0x2CE838 .. 0x2CE890): planets 2 .. 29 but the mission levels and Launch Site. */
static int game_map_filter(u32 p)
{
	return p >= 2 && p < 30 && p != 6 && p != 20 && p != 23 && p != 26 && p != 27 && p != 28 && p != 29;
}

/* The text the build names an entry with (0x2CEB04): the planet's for these, the location's for the rest. */
static u32 map_text_id(u32 p)
{
	static const u8 by_planet[] = { 1, 2, 4, 5, 8, 9, 10, 13, 14, 18, 23, 27, 28, 29 };
	u32 i;

	for (i = 0; i < sizeof(by_planet); i++) {
		if (by_planet[i] == p) return r32(U_MAP_TEXTS + 8 * p + 4);
	}
	return r32(U_MAP_TEXTS + 8 * p);
}

/*
 * The map's init (0x2CED28), as far as the list goes: the build reads the
 * save's sixty planets, each through its filter's first word, which is
 * mh_map_list_stub with the hook in (here in C: SH_MAP up, the stub's list;
 * down, the game's filter), and names each entry. The cursor on the first, and
 * Square's MISSIONS hint shown.
 */
static void mock_map_init(u32 gs)
{
	u32 i, p, n = 0, e;

	(void)gs;
	/* The map takes picture sources 0 .. 2 and four of the pause's picture buffers (round 4: the menu's share them). */
	M.pic_clash = G.pic_state == 1;
	M.pic_held = G.pic_taken;
	M.flag_in_build = r8(MH_SHIM + SH_MAP);
	for (i = 0; i < 60; i++) {
		p = r32(U_UNLOCKED + 4 * i);
		if (r8(MH_SHIM + SH_MAP) != 0) {
			p = i < MH_MAP_LIST_LEN ? map_list_c[i] : 0;
			if (p == 0) continue;
		} else if (!game_map_filter(p)) {
			continue;
		}
		if (n == 30) break;
		e = U_MAP_ENTRIES + n * U_MAP_ENTRY_SIZE;
		w32(e, p);
		w8(e + 4, 1);
		M.text[n] = map_text_id(p);
		n++;
	}
	w32(U_MAP_COUNT, n);
	w32(U_MAP_CURSOR, 0);
	remember(U_MAP_HINT, MSG_AT + 0x30);            /* "\x13 MISSIONS", text 0x15C6 */
	M.up = 1;
	M.inits++;
}

static void mock_map_deinit(u32 gs)
{
	(void)gs;
	M.up = 0;
	M.deinits++;
}

/* The travel setter 0x2D22DC: the pause exit loads that planet. */
static void map_travel(u32 p)
{
	w32(U_KIND, 1);
	w32(U_DEST, p);
}

/* Triangle's and the Start close's way out (0x2CFD54, 0x2D0534): a reload, or BATTLE MISSIONS on 12 and 18. */
static void map_leave(u32 gs)
{
	u32 cur = r32(U_PLANET);

	if (cur == 12 || cur == 18) {
		mock_play_sound(5, 0, 0);
		mock_swap_to(BATTLE_GS);
	} else {
		map_travel(cur);
		mock_swap_to(mock_pause_exit());
		mock_play_sound(0x14, 0, 0);
	}
	mock_reset_state(gs);
}

/*
 * The map's update (0x2CFB60): Up and Down; the cross the pick's callback
 * (0x2D078C: Veldin and the current planet refused, else the travel setter
 * and out); Square MISSIONS; Triangle and the Start close map_leave.
 */
static void mock_map_update(u32 gs)
{
	u32 e = r32(U_PAD + U_PAD_EDGES), c = r32(U_MAP_CURSOR), n = r32(U_MAP_COUNT), p;

	M.updates++;
	if ((e & PADB_DOWN) != 0 && c + 1 < n) c++;
	if ((e & PADB_UP) != 0 && c > 0) c--;
	w32(U_MAP_CURSOR, c);
	if ((e & r32(U_CONFIRM_MASK)) != 0) {
		p = r32(U_MAP_ENTRIES + c * U_MAP_ENTRY_SIZE);
		if (p > 1 && p != r32(U_PLANET)) {
			map_travel(p);
			mock_play_sound(0x14, 0, 0);
			mock_swap_to(mock_pause_exit());
			mock_reset_state(gs);
		}
		mock_play_sound(4, 0, 0);
	} else if ((e & PADB_SQUARE) != 0) {
		mock_play_sound(4, 0, 0);
		mock_swap_to(MISSIONS_GS);
		mock_reset_state(gs);
	}
	if ((e & PADB_TRIANGLE) != 0) map_leave(gs);
	if (mock_start_close()) {
		if (r8(U_NO_EXIT) == 0) map_leave(gs);
		mock_start_ack();
		mock_reset_state(gs);
	}
}

/* The getter 0x2CE130: the game's own GuiState for the map, its five calls the game's. */
static u32 game_map(void)
{
	w32(MAP_GS, 0);
	w32(MAP_GS + 4, OPD_MAP_INIT);
	w32(MAP_GS + 8, OPD_MAP_DEINIT);
	w32(MAP_GS + 12, OPD_MAP_UPDATE);
	w32(MAP_GS + 16, U_DESC_MAP_PRE);
	w32(MAP_GS + 20, U_DESC_MAP_POST);
	return MAP_GS;
}

#include "../src/games/menuhelper/mh_core.c"
#include "../src/games/menuhelper/mh_rac3.c"

static void call_row(u32 cb)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC3);

	if (cb == MH_FN_ROW) mh_row();
	else if (d != NULL && cb == d->hooks[9].value) mh_help();
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
	else if (tok == MH_FN_MAP_INIT) mh_map_init(gs);
	else if (tok == MH_FN_MAP_DEINIT) mh_map_deinit(gs);
	else if (tok == MH_FN_MAP_UPDATE) mh_map_update(gs);
	else if (tok == OPD_MAP_INIT) mock_map_init(gs);
	else if (tok == OPD_MAP_DEINIT) mock_map_deinit(gs);
	else if (tok == OPD_MAP_UPDATE) mock_map_update(gs);
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
			/* The pause exit's deinit (0x2D4748): kind 1 is goto_planet, a travel; 0 a plain close. */
			M.exit_kind = r32(U_KIND);
			M.exit_dest = r32(U_DEST);
			G.active = 0;
			G.paused = 0;
			return;
		}
		if (G.active == MISSIONS_GS) M.missions++;
		if (G.active == BATTLE_GS) M.battle++;
		if (G.active == HELP_GS || G.active == MISSIONS_GS || G.active == BATTLE_GS) return;
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
	w32(U_KIND, 0);                     /* the launcher's init (0x2D42D0) */
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
static u16 peek16(u32 a) { u8 b[2]; host_peek(a, b, 2); return be16_get(b); }
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

/*
 * Round 4's Game page, which round 5 took the values off (rac3.c): the tests'
 * own table keeps it for the grid's sliders and scrolling.
 */
static const u8 uya_tv_items[] = {
	R3_DIE, MENU_ITEM_SLOT, MENU_ITEM_POS_SAVE, MENU_ITEM_POS_LOAD, R3_SET_ASIDE, R3_LOAD_ASIDE,
	R3_BOLTS, R3_CHALLENGE, R3_HEALTH_XP, R3_HEALTH, R3_FILE_TIME
};
static const struct menu_section uya_tv_sections[] = { { "Game", uya_tv_items, (u8)sizeof(uya_tv_items) } };
static struct game_menu uya_tv_menu;

static void uya_tv_on(void)
{
	memset(&uya_tv_menu, 0, sizeof(uya_tv_menu));
	uya_tv_menu.sections = uya_tv_sections;
	uya_tv_menu.nsections = 1;
	menu_test_menu(&uya_tv_menu);
}

/* ------------------------------------------------------------- the model */

static void test_model_uya(void)
{
	static const char * const root[] = { "Game", "Cheats", "Planets", "Manips", "Unlocks", "Save files" };
	/*
	 * Round 5: no Die, Health XP, Health or File time; Max bolts on Cheats.
	 * Round 6: Challenge mode on Game, where round 5 had a spacer.
	 */
	static const char * const game[] = { "Position slot", "Save position", "Load position", "Challenge mode",
	                                     "Set aside file", "Load set-aside file" };
	static const char * const cheats[] = { "Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health",
	                                       "Quick-select pause", "Max bolts" };
	static const char * const manips[] = { "Category", "Auto-reset", "Apply now", "", "Reset trophies",
	                                       "Trophy reset", "Reset trophies on Veldin load" };
	static const char * const unlocks[] = { "Weapons", "Gadgets and items", "Vid comics", "Collectables" };
	static const char * const collectables[] = { "Max all weapon levels", "Reset all weapon levels",
	                                             "Unlock all skill points", "Reset all skill points",
	                                             "Unlock all titanium bolts", "Reset all titanium bolts" };
	struct menu_view v;
	const struct menu_row *r;

	group("console menu: the model, UYA's pages");

	check(boot("NPEA00387"), "UYA boots");
	menu_model_reset(9, 24, CAPS_ALL);
	menu_model_view(&v);
	check(labels_are(&v, root, 6) && v.ptype == MBOX_PTYPE_MAIN,
	      "the root: Game, Cheats, Planets, Manips, Unlocks, Save files: the design's six");

	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(labels_are(&v, game, 6) && v.ptype == MBOX_PTYPE_CONFIG && v.rows[3].kind == MROW_TOGGLE,
	      "round 6, Game: the slot and the positions, Challenge mode (round 5's spacer gone), the save-file "
	      "buttons; no Die");
	poke8(RAC3_CHALLENGE_MODE, 0);
	tick(2);
	menu_model_view(&v);
	r = view_row(&v, "Challenge mode");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0, "Challenge mode: a switch, Off at 0");
	check(act(&v, MBOX_EV_RIGHT, "Challenge mode") == ST_OK && peek8(RAC3_CHALLENGE_MODE) == 1 &&
	      strcmp(value_of(&v, "Challenge mode"), "On") == 0, "Right: On, the counter 1");
	poke8(RAC3_CHALLENGE_MODE, 4);
	tick(2);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Challenge mode"), "On") == 0 && act(&v, MBOX_EV_ACTIVATE, "Challenge mode") == ST_OK &&
	      peek8(RAC3_CHALLENGE_MODE) == 0 && strcmp(value_of(&v, "Challenge mode"), "Off") == 0,
	      "on its fourth run On, and the cross Off, 0");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Cheats");
	check(labels_are(&v, cheats, 6) && v.ptype == MBOX_PTYPE_CONFIG && view_row(&v, "Challenge mode") == NULL,
	      "Cheats: the four cheats every game has, the quick-select pause, then (round 5) Max bolts; round 6, no "
	      "Challenge mode");
	poke32(RAC3_BOLTS, 5);
	check(act(&v, MBOX_EV_ACTIVATE, "Max bolts") == ST_OK && peek32(RAC3_BOLTS) == 99999999u &&
	      value_of(&v, "Max bolts")[0] == 0 && view_row(&v, "Max bolts")->kind == MROW_ACTION,
	      "Max bolts: 99,999,999, the game's own cap, and the row blank");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0,
	      "Ghost Ratchet: the cross turns it On");
	r = view_row(&v, "Quick-select pause");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0,
	      "the quick-select pause, a LIVE toggle, is Off or On");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: with no picker in the renderer (CAPS_ALL), the row is a link like the others. */
	r = view_row(&v, "Planets");
	check(r != NULL && r->kind == MROW_LINK && (r->flags & MROWF_PICKER) == 0,
	      "Planets, for a renderer without the game's picker: a link to qwark's own list");
	menu_model_reset(9, 24, CAPS_ALL | MBOX_CAP_PICKER);
	menu_model_view(&v);
	r = view_row(&v, "Planets");
	check(r != NULL && r->kind == MROW_LINK && (r->flags & (MROWF_PICKER | MROWF_ENABLED)) == (MROWF_PICKER | MROWF_ENABLED) &&
	      (view_row(&v, "Cheats")->flags & MROWF_PICKER) == 0,
	      "for one with it, the Planets row, and only it, says the cross opens the game's picker");

	/* Planets: the plain list, in the game's order, on a CONFIG page (round 4), 24 a window: the fallback. */
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.ptype == MBOX_PTYPE_CONFIG && strcmp(v.rows[0].label, "Veldin") == 0 &&
	      strcmp(v.rows[5].label, "Obani Gemini") == 0 && v.nrows == 24 && strcmp(v.title, "Planets") == 0 &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == MBOX_PAGE_MORE_DOWN,
	      "Planets: a CONFIG page, Veldin, Florana, Starship Phoenix, Marcadia, Daxx, Obani Gemini ..., the first "
	      "24 of 34 and no More row, the page going on below");
	check((v.rows[0].flags & MROWF_CLOSES) != 0 && v.rows[0].kind == MROW_ACTION, "each a button that closes the menu first");
	check(menu_model_event(MBOX_EV_SCROLL_DOWN, 23, v.rows[23].id, v.key) == ST_OK, "the slide from the 24th");
	menu_model_view(&v);
	check(view_row(&v, "Vid Comic 1 SE") != NULL && view_row(&v, "(unused)") == NULL && view_row(&v, "(none)") == NULL &&
	      v.nrows == 24 && (v.pflags & MBOX_PAGE_MORE_DOWN) == 0,
	      "the last window ends with the vid comics; the gaps and the placeholder are left out");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/*
	 * The pickers' round 2: the GALACTIC MAP asks for the planets it cannot
	 * show (MBOX_EV_PICKER_OTHERS on the Planets row): rac3.c's picker_others,
	 * the base vid comics, on a CONFIG page of their own; back to the root.
	 */
	r = view_row(&v, "Planets");
	check(r != NULL && menu_model_event(MBOX_EV_PICKER_OTHERS, (u8)(r - v.rows), r->id, v.key) == ST_OK,
	      "the picker's event on the Planets row is taken");
	menu_model_view(&v);
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_PLANETS, MENU_PLANETS_OTHERS) && v.ptype == MBOX_PTYPE_CONFIG &&
	      strcmp(v.title, "Vid-Comics") == 0 && v.nrows == 5 && strcmp(v.rows[0].label, "Vid Comic 1") == 0 &&
	      strcmp(v.rows[1].label, "Vid Comic 2") == 0 && strcmp(v.rows[3].label, "Vid Comic 4") == 0 &&
	      strcmp(v.rows[4].label, "Vid Comic 5") == 0 && v.rows[0].id == MENU_ROW_ID(MENU_ROW_PLANET, 31) &&
	      v.rows[3].id == MENU_ROW_ID(MENU_ROW_PLANET, 32) && v.rows[4].id == MENU_ROW_ID(MENU_ROW_PLANET, 35) &&
	      (v.rows[2].flags & MROWF_CLOSES) != 0 && v.rows[2].kind == MROW_ACTION &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == 0 && view_row(&v, "Vid Comic 6") == NULL,
	      "Vid-Comics: Vid Comic 1 to 5 (planets 31, 33, 34, 32, 35), each a button that closes the menu first; no 6, "
	      "no 1 SE");
	check(act(&v, MBOX_EV_ACTIVATE, "Vid Comic 2") == ST_OK && value_of(&v, "Vid Comic 2")[0] == 0,
	      "round 5: the cross on one waits for the close, its row blank (no \"after closing\")");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	check(menu_model_depth() == 1 && v.cursor == (u8)(view_row(&v, "Planets") - v.rows),
	      "back: the root, the cursor on Planets");
	r = view_row(&v, "Cheats");
	check(menu_model_event(MBOX_EV_PICKER_OTHERS, (u8)(r - v.rows), r->id, v.key) == ST_BAD_ARG &&
	      menu_model_depth() == 1, "the same event on another row: refused, nothing opens");
	menu_model_view(&v);

	/* Manips, with the Trophies header and the reset's status under its button. */
	act(&v, MBOX_EV_ACTIVATE, "Manips");
	check(labels_are(&v, manips, 7) && v.ptype == MBOX_PTYPE_CONFIG && v.rows[3].kind == MROW_INFO &&
	      v.rows[3].flags == 0 && strcmp(value_of(&v, "Auto-reset"), "On") == 0,
	      "Manips: the category, Auto-reset (On), Apply now, a spacer (round 5), then Reset trophies, its status and "
	      "the toggle");
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
	      value_of(&v, "Unlock all skill points")[0] == 0, "each runs, and says nothing (round 5)");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	check(labels_are(&v, unlocks, 4) && v.cursor == 3, "back: Unlocks, the cursor on Collectables");
	act(&v, MBOX_EV_ACTIVATE, "Weapons");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 20 && v.rows[19].kind == MROW_CHOICE &&
	      strcmp(v.title, "Weapons") == 0 && (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == 0,
	      "Weapons: all twenty on one page, which the grid scrolls (round 4: no More row, no \"1/2\")");
	uya_weapon(0x4FF, 0x57, 0);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Agents of Doom"), "LOCKED") == 0, "a locked weapon: LOCKED");
	check(act(&v, MBOX_EV_RIGHT, "Agents of Doom") == ST_OK && strcmp(value_of(&v, "Agents of Doom"), "V1") == 0 &&
	      peek8(RAC3_ITEM_ARRAY + 0x57) == 0x57, "Right: V1, owned at v1 (round 5: it said UNLOCKED)");
	check(act(&v, MBOX_EV_RIGHT, "Agents of Doom") == ST_OK && strcmp(value_of(&v, "Agents of Doom"), "V8") == 0 &&
	      peek8(RAC3_ITEM_ARRAY + 0x57) == 0x57 + 7, "Right: V8, its last (it said MAX)");
	check(act(&v, MBOX_EV_RIGHT, "Agents of Doom") == ST_OK && strcmp(value_of(&v, "Agents of Doom"), "LOCKED") == 0,
	      "Right wraps to LOCKED");
	uya_weapon(0x4FF, 0x57, 3);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Agents of Doom"), "V3") == 0, "a version between shows as itself, V3");
	act(&v, MBOX_EV_RIGHT, "Agents of Doom");
	check(peek8(RAC3_ITEM_ARRAY + 0x57) == 0x57 + 7 && strcmp(value_of(&v, "Agents of Doom"), "V8") == 0,
	      "and Right steps on to the last, V8");
	uya_weapon(0x4B9, 0x11, 1);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Lava Gun"), "UNLOCKED") == 0,
	      "an owned GameCube weapon, whose version cannot be read back, shows UNLOCKED");
	uya_weapon(0x53F, 0x97, 5);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "R3YNO"), "V5") == 0, "the R3YNO at v5, its own last: V5");
	act(&v, MBOX_EV_RIGHT, "R3YNO");
	check(strcmp(value_of(&v, "R3YNO"), "LOCKED") == 0, "and Right from its last wraps to LOCKED");
	uya_weapon(0x53F, 0x97, 1);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "R3YNO"), "V1") == 0, "at v1: V1");
	act(&v, MBOX_EV_RIGHT, "R3YNO");
	check(peek8(RAC3_ITEM_ARRAY + 0x97) == 0x97 + 4 && strcmp(value_of(&v, "R3YNO"), "V5") == 0,
	      "and Right writes its v5, V5");
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

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 14 && d->ncaves == 4 &&
	      d->mailbox_size == MH_MBOX_SIZE,
	      "UYA has a renderer: the mailbox with the picker's two words, the state, the read-only data, the code, "
	      "fourteen words");
	if (d == NULL) return;
	check(d->hooks[0].addr == MH_SITE_MSG && d->hooks[4].addr == MH_SITE_INJECT &&
	      d->hooks[6].addr == MH_SITE_LAUNCH && d->hooks[7].addr == MH_SITE_MAP_LIST &&
	      d->hooks[7].original == MH_ORIG_MAP_LIST && (d->hooks[7].value >> 26) == 18 && (d->hooks[7].value & 3) == 1 &&
	      d->hooks[8].addr == MH_SITE_HELP8_TEXT &&
	      d->hooks[8].value == MH_MSG_HELP && d->hooks[13].addr == MH_SITE_HELP9B_CB &&
	      d->hooks[9].value == d->hooks[11].value && d->hooks[11].value == d->hooks[13].value,
	      "the label hook first, the GALACTIC MAP's list word (a bl) before the HELP rows' text and callback, "
	      "last, one callback for all three tables");
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
	check(menu_state() == MENU_READY && hooks_are(d, 1), "in play again, the renderer goes in, all fourteen words");
	{
		u32 k, a = 0, l = 0, n = host_write_log_count(), bytes = 0, at_code = 999, first_hook = 999;
		for (k = 0; k < n && k < 64; k++) {
			host_write_log_at(k, &a, &l);
			bytes += l;
			if (a == MH_CAVE) at_code = k;
			if (a == MH_SITE_MSG && first_hook == 999) first_hook = k;
		}
		check(at_code < first_hook && n <= 24 && bytes <= 24u * 1024u,
		      "the caves before the words: twenty-two writes, under 24 KB");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	mb = menu_mailbox();
	check(mb == MH_BOX && mb == 0x01ADB900u && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 24 &&
	      peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 && peek8(mb + MBOX_OFF_MAX_ROWS) == 9 &&
	      peek8(mb + MBOX_OFF_CFG_ROWS) == 24 && peek8(mb + MBOX_OFF_RENDERER) == 3 &&
	      peek16(mb + MBOX_OFF_VERSION) == 3 && (peek8(mb + MBOX_OFF_CAPS) & MBOX_CAP_PICKER) != 0 &&
	      peek16(mb + MBOX_OFF_SIZE) == 0x680,
	      "the mailbox (round 4): at 0x1ADB900, version 3, 24 rows of 0x40, nine a MAIN page, 24 a CONFIG page, "
	      "renderer 3, the planet picker (caps bit4)");
	check(peek32(U_CELLS) == 0 && peek32(U_CELLS + U_CELLS_SIZE - 4) == 0,
	      "the grid's cells' span, which the install does not write");
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
	check(peek32(MH_SITE_MAP_LIST) == MH_ORIG_MAP_LIST, "the GALACTIC MAP's list build its own again too");
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
static int near(float a, float b) { return a > b - 0.0005f && a < b + 0.0005f; }
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
	poke32(U_PIC_HUD_WAD, 0x00A1B2C3u);
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
	check(G.active == MH_SHIM + SH_GS && G.main_up && G.table == MH_SHIM + SH_ROWS && G.count == 6 &&
	      r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN,
	      "the next frame: the main pause's init on the menu's table, six rows, the mailbox open");
	check(strcmp(row_text(0, a), "GAME") == 0 && strcmp(row_text(5, b), "SAVE FILES") == 0 &&
	      r8(MH_SHIM + SH_ROWS + 5 * 20 + 8) == 1 && r8(MH_SHIM + SH_ROWS + 8) == 1,
	      "GAME .. SAVE FILES in capitals, all enabled");
	{
		/* The band is 0.40 - 0.03 * rows to 0.61 + 0.03 * rows: 0.22 and 0.79 for six. */
		float lo = 0.40f - 0.03f * 6.0f, hi = 0.61f + 0.03f * 6.0f;
		check(G.band[0] > lo - 0.001f && G.band[0] < lo + 0.001f && G.band[1] > hi - 0.001f && G.band[1] < hi + 0.001f &&
		      r32(U_CURSOR) == 0, "the frame's band for the root's six rows, 0.22 and 0.79; the cursor on GAME");
	}
	n = 1;
	for (i = 0; i < 6; i++) {
		struct widget *row = wid(U_ROW_HANDLE + (u32)i);
		n = n && row->fit && row->box_w > 0.439f && row->box_w < 0.441f && row->box_min > 0.59f && row->box_min < 0.61f;
	}
	check(n && !wid(U_ROW_HANDLE + 6)->fit,
	      "each of the menu's rows shrinks to fit the frame, 0.44 wide, no smaller than 0.6; the rows past them are left alone");
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
	check(G.paused && G.active == MH_SHIM + SH_GS && G.count == 6 && peek8(mb + MBOX_OFF_REQUEST) == 0 &&
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
	      strcmp(row_text(5, a), "SAVE FILES") == 0,
	      "round 3: the frame it draws before the runner swaps still reads the root's rows, not the Cheats page");
	pframe(0);
	check(!G.main_up && G.main_deinits >= 2 && G.frame_up && G.built && G.rows == 6 && G.vis == 6 &&
	      G.cols == 2 && G.aligned == 2 && G.set_cursor >= 1,
	      "the main pause's list ends, the frame and a grid of six rows begin, both columns aligned");
	check(strcmp(read_str(G.title, a, 80), "CHEATS") == 0 && G.hint_a == 292 && G.hint_b == 3285,
	      "titled CHEATS, with SELECT and BACK");
	check(G.banner == 7.0f && G.title_sx == 0.875f && G.title_sy == 0.875f,
	      "the banner and the title's scale as the game fits them to the title, as Options' screens do");
	check(strcmp(cfg_label(0, a), "FAST LOADS") == 0 && strcmp(cfg_value(0, b), "OFF") == 0 &&
	      strcmp(cfg_label(4, a), "QUICK-SELECT PAUSE") == 0 && cell_color(1) == U_COLOR_OFF,
	      "labels in capitals, without a colon (round 4), values in capitals, an off cheat in the normal colour");
	check(strcmp(cfg_label(5, b), "MAX BOLTS") == 0 && strcmp(cfg_value(5, a), "") == 0,
	      "round 5: MAX BOLTS, a blank button, ends it (round 6: CHALLENGE MODE is on GAME)");
	check(G.font[0] == 1 && G.font[1] == 1 && G.rules == 1, "the cells in the Options screens' font, with their rules");
	check(G.buf == U_CELLS && r32(CFG) == U_CELLS_SIZE, "the grid's cells in their own span (round 4), its size given");
	{
		struct widget *box = wid(AV_PANEL_BOX), *pic = wid(AV_PIC);
		check(G.layer17 == AV_ROOT && made(AV_ROOT, 'C') && made(AV_PANEL, 'C') && wid(AV_PANEL)->parent == AV_ROOT &&
		      made(AV_PANEL_BOX, 'R') && box->parent == AV_PANEL && box->color == AV_PANEL_COLOR &&
		      box->y > 0.265f && box->y < 0.266f && box->sx > 0.827f && box->sx < 0.828f,
		      "AUDIO / VISUAL's picture panel above the list, on layer 17");
		check(G.pic_taken == 2 && G.pic_src == PIC_SOURCES + 0x44u && G.pic_a != 0 && G.pic_b != 0 &&
		      G.pic_a != G.pic_b && G.pic_wad == 0x00A1B2C3u && G.pic_set == U_PIC_SET && G.pic_state == 1,
		      "round 4: two picture buffers, and AUDIO / VISUAL's picture source, 1, set up with them, the hud.wad's "
		      "word and the screen's picture set, as its init does");
		check(made(AV_PIC, 'Q') && pic->parent == AV_PANEL && pic->color == 0x80808080u && near(pic->y, 0.268f) &&
		      near(pic->sx, 0.8143f) && pic->source == G.pic_src && pic->picture == 0 && !pic->visible &&
		      pic->fade == 10,
		      "in the panel the picture: AUDIO / VISUAL's rectangle, tied to the source, picture 0, hidden, a fade "
		      "of ten frames");
		check(!made(AV_SLIDERS, 'C') && !G.widgets_bad, "no slider on a page without one; no handle made twice");
	}
	for (i = 0; i < 28; i++) pframe(0);
	check(!wid(AV_PIC)->visible && wid(AV_PIC)->picture == 0, "29 frames in, nothing shows yet");
	pframe(0);
	check(wid(AV_PIC)->visible && wid(AV_PIC)->picture == U_PICTURE && U_PICTURE == 4,
	      "the thirtieth, as AUDIO / VISUAL's cursor rests: picture 4, EFFECTS VOLUME's, the user's header");
	pframe(PADB_DOWN);
	check(G.top + G.off == 1, "Down moves the grid's cursor");
	for (i = 0; i < 3; i++) pframe(PADB_DOWN);
	pframe(0);
	check(wid(AV_PIC)->picture == U_PICTURE, "the picture stays as the cursor moves: no description any more");
	for (i = 0; i < 3; i++) pframe(PADB_UP);
	pframe(PADB_RIGHT);
	check(G.sound == SND_CHANGE && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "Right on Ghost Ratchet posts an event, with the pause's change sound");
	tick(2);
	pframe(0);
	check(strcmp(cfg_value(1, a), "ON") == 0 && cell_color(1) == U_COLOR_OFF && G.top + G.off == 1 &&
	      (features_toggle_state() & ((u64)1 << R3_GHOST)) != 0,
	      "the same page refreshed in place: ON, in the normal colour (round 4: no white for a row that is on), the "
	      "cursor where it was");
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
	check(G.active == MH_SHIM + SH_GS && G.main_up && !G.frame_up && !G.built && G.count == 6 &&
	      r32(U_CURSOR) == 1, "Triangle: the grid and frame go, the root's list is back on CHEATS");
	check(!made(AV_ROOT, 'C') && !made(AV_PIC, 'Q') && G.layer17 == 0, "and the panel and its picture with them (layer 17)");
	check(G.pic_state == 8 && G.pic_stops == 1 && G.pic_taken == 0 && G.pic_dropped == 2 && G.disc_waits == 1 &&
	      r8(MH_SHIM + SH_PIC_ON) == 0,
	      "the picture source stopped, both buffers given back and the disc's reads waited for, as AUDIO / VISUAL's "
	      "deinit does");

	/*
	 * Round 4's GAME, eleven rows, eight on screen, scrolled; Challenge mode at
	 * 51 of 0..255. Round 5 moved the values off it, so the page is the tests'
	 * own, the same rows (uya_tv_menu).
	 */
	uya_tv_on();
	poke8(RAC3_CHALLENGE_MODE, 51);
	tick(2);
	w32(U_CURSOR, 0);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.rows == 11 && G.vis == 8 && G.pic_taken == 2,
	      "GAME: a grid of eleven rows, eight on screen, as AUDIO / VISUAL's, the picture source again");
	check(strcmp(cfg_label(0, a), "DIE") == 0 && strcmp(cfg_value(0, b), "") == 0 &&
	      strcmp(cfg_label(6, a), "BOLTS") == 0,
	      "round 4: a button blank until it answers, no colons");
	check(strcmp(cfg_label(7, a), "CHALLENGE MODE 51") == 0 && strcmp(cfg_value(7, b), " ") == 0 &&
	      made(AV_SLIDERS, 'C') && wid(AV_SLOT0 + 21)->visible && wid(AV_SLOT0 + 23)->visible &&
	      near(wid(AV_SLOT0 + 23)->x, 0.653f + 0.232f * 0.2f) &&
	      !wid(AV_SLOT0 + 18)->visible,
	      "Challenge mode, the one value qwark gives a short range (0..255), on AUDIO / VISUAL's slider in the "
	      "last slot, its knob where the mode is; Bolts, beside it, a number");
	for (i = 0; i < 10; i++) pframe(PADB_DOWN);
	check(G.top + G.off == 10 && G.top == 3, "Down to the last row scrolls the grid");
	check(wid(AV_SLOT0 + 12)->visible && !wid(AV_SLOT0 + 21)->visible,
	      "and the slider with it, Challenge mode now in the fifth slot");
	pframe(PADB_RIGHT);
	tick(2);
	pframe(0);
	check(strcmp(cfg_label(10, a), "FILE TIME") == 0 && peek32(RAC3_FILE_TIME) == 1 && G.top + G.off == 10,
	      "Right on File time steps it, the cursor still there");
	menu_test_menu(NULL);

	/* Start on the CONFIG page closes the pause, as CHEATS' does. */
	G.closing = 1;
	pframe(0);
	check(G.pending == EXIT_GS && G.closing == 0, "Start: acknowledged, the pause's exit queued");
	pframe(0);
	check(!G.paused && !G.built && !G.frame_up && r32(MH_SHIM + SH_UP) == 0 && G.pic_taken == 0 &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "the grid freed, the frame gone, the picture buffers back, the menu down");
	tick(4);

	/* Round 3: the combo opens on the root, not on that page; again, it closes, through the close request. */
	check(menu_model_depth() == 1, "closed, qwark's stack is back on the root");
	press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(G.main_up && !G.built && G.count == 6 && r32(U_CURSOR) == 0 && strcmp(row_text(0, a), "GAME") == 0,
	      "the combo opens the menu on the root, the cursor on its first row, not on GAME");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with it up asks it to close");
	pframe(0);
	pframe(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0 && !G.built, "which its next frame does");
	tick(4);

	/* Round 6: GAME's CHALLENGE MODE, where round 5 had a blank cell pair the grid passed over. */
	poke8(RAC3_CHALLENGE_MODE, 0);
	press(COMBO_MENU_DEFAULT);
	play_frame(0);
	tick(4);
	w32(U_CURSOR, 0);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.rows == 6 && strcmp(cfg_label(2, a), "LOAD POSITION") == 0 &&
	      strcmp(cfg_label(3, b), "CHALLENGE MODE") == 0 && strcmp(cfg_value(3, a), "OFF") == 0 &&
	      cell_color(3) == U_COLOR_OFF && strcmp(cfg_label(4, b), "SET ASIDE FILE") == 0 && cell_color(4) == U_COLOR_OFF,
	      "GAME: six rows, the fourth CHALLENGE MODE, OFF, in the normal colour");
	pframe(PADB_DOWN);
	pframe(PADB_DOWN);
	check(G.top + G.off == 2, "Down twice: LOAD POSITION");
	pframe(PADB_DOWN);
	check(G.top + G.off == 3, "Down: CHALLENGE MODE, a row the cursor lands on");
	pframe(0x40);
	tick(2);
	pframe(0);
	check(strcmp(cfg_value(3, a), "ON") == 0 && peek8(RAC3_CHALLENGE_MODE) == 1, "the cross: ON, the counter 1");
	pframe(PADB_DOWN);
	check(G.top + G.off == 4, "Down: SET ASIDE FILE");
	pframe(PADB_UP);
	check(G.top + G.off == 3, "Up: CHALLENGE MODE again");
	poke8(RAC3_CHALLENGE_MODE, 0);
	press(COMBO_MENU_DEFAULT);
	pframe(0);
	pframe(0);
	check(!G.paused && !G.built, "the combo closes it");
	tick(4);

	/* The HELP row after a close deep in the menu: the root too, the cursor on its first row. */
	play_frame(1);
	w32(U_CURSOR, 6);
	pframe(0x40);
	pframe(0);
	check(G.main_up && G.count == 6 && r32(U_CURSOR) == 0 && strcmp(row_text(0, a), "GAME") == 0,
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
	check(G.main_up && G.count == 6 && r32(U_CURSOR) == 0, "and TRAINER opens the root again");
	tick(4);

	/*
	 * Round 4: PLANETS, a CONFIG page of 24 of the 34, the window sliding under
	 * the cursor at its last row. The row opens the GALACTIC MAP now (the
	 * picker's own test, below); qwark's list is its fallback, here in a pause
	 * whose Start does not leave it, where the map is refused.
	 */
	w32(U_CURSOR, 2);
	poke8(U_NO_EXIT, 1);
	pframe(0x40);
	poke8(U_NO_EXIT, 0);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.rows == 24 && strcmp(cfg_label(0, a), "VELDIN") == 0 && strcmp(cfg_value(0, b), "") == 0 &&
	      strcmp(read_str(G.title, b, 80), "PLANETS") == 0,
	      "PLANETS: AUDIO / VISUAL's look, the first 24 of the 34 planets, each blank, no More row, the title its own");
	for (i = 0; i < 23; i++) pframe(PADB_DOWN);
	check(G.top + G.off == 23 && strcmp(cfg_label(23, a), "LAUNCH SITE") == 0, "Down to the 24th, Launch Site");
	pframe(0);
	check(peek8(mb + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_DOWN && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "on the window's last row, the page going on below: the core asks for the next window");
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.rows == 24 && G.top + G.off == 13 && strcmp(cfg_label(13, a), "LAUNCH SITE") == 0 &&
	      strcmp(cfg_label(23, b), "VID COMIC 1 SE") == 0,
	      "the screen started again on the last window, the vid comics at its end, the cursor still on Launch Site");
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.main_up && G.count == 6 && r32(U_CURSOR) == 2, "Triangle: the root, the cursor on PLANETS");
	tick(4);

	/* A page with no row the cursor may land on: the main pause's list must not loop. */
	check(G.main_up && G.count == 6, "on the root");
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

/*
 * A page as qwark writes one into the mailbox, rows of 0x40: `sliders` rows
 * {index, value, lowest, highest, slider} are VALUE rows with their range,
 * the rest toggles.
 */
struct slider_row { int row; u32 value, lo, hi; int slider; const char *label; };

static void post_page(u32 mb, u32 key, int n, const struct slider_row *sl, int nsl)
{
	u32 seq = peek32(mb + MBOX_OFF_PAGE_SEQ), r;
	u8 head[MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY], row[MBOX_ROW_SIZE];
	char text[8];
	int i, k;

	memset(head, 0, sizeof(head));
	be32_put(head, key);
	head[MBOX_OFF_NROWS - MBOX_OFF_PAGE_KEY] = (u8)n;
	head[MBOX_OFF_PTYPE - MBOX_OFF_PAGE_KEY] = MBOX_PTYPE_CONFIG;
	memcpy(head + MBOX_OFF_TITLE - MBOX_OFF_PAGE_KEY, "Sliders", 7);
	poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 1);
	host_poke(mb + MBOX_OFF_PAGE_KEY, head, sizeof(head));
	for (i = 0; i < n; i++) {
		r = mb + MBOX_OFF_ROWS + (u32)i * MBOX_ROW_SIZE;
		memset(row, 0, sizeof(row));
		row[MROW_OFF_KIND] = MROW_TOGGLE;
		row[MROW_OFF_FLAGS] = MROWF_ENABLED | MROWF_STEPS;
		row[MROW_OFF_ID + 1] = (u8)(i + 1);
		text[0] = 'R'; text[1] = 'o'; text[2] = 'w'; text[3] = ' '; text[4] = (char)('0' + i); text[5] = 0;
		memcpy(row + MROW_OFF_LABEL, text, 6);
		memcpy(row + MROW_OFF_VALUE, "Off", 3);
		for (k = 0; k < nsl; k++) {
			if (sl[k].row != i) continue;
			memset(row + MROW_OFF_LABEL, 0, MROW_LABEL_LEN + MROW_VALUE_LEN);
			memcpy(row + MROW_OFF_LABEL, sl[k].label, strlen(sl[k].label));
			row[MROW_OFF_KIND] = MROW_VALUE;
			row[MROW_OFF_FLAGS] = MROWF_ENABLED | MROWF_STEPS | MROWF_RANGE | (sl[k].slider ? MROWF_SLIDER : 0);
			be32_put(row + MROW_OFF_RANGE, sl[k].value);
			be32_put(row + MROW_OFF_RANGE + 4, sl[k].lo);
			be32_put(row + MROW_OFF_RANGE + 8, sl[k].hi);
			be32_put(row + MROW_OFF_RANGE + 12, 1);
			qfmt_u32((char *)row + MROW_OFF_VALUE, MROW_VALUE_LEN, sl[k].value);
		}
		host_poke(r, row, sizeof(row));
	}
	poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 2);
}

/* The sliders: AUDIO / VISUAL's groove, track and knob for a VALUE row the core marks as one. */
static void test_sliders_uya(void)
{
	static const struct slider_row page1[] = {
		{ 0, 5, 0, 10, 1, "Health" }, { 5, 7, 0, 4000000000u, 0, "Bolts" }, { 9, 3, 1, 5, 1, "Speed" }
	};
	struct slider_row page2[3];
	char a[80];
	u32 mb, h;
	int i, n;

	group("console menu: UYA's sliders, on the fake console");

	memset(&G, 0, sizeof(G));
	check(boot("NPEA00387"), "UYA boots");
	uya_playing();
	seed_game_table();
	poke32(U_PAD_PTR, U_PAD);
	poke32(U_CONFIRM_MASK, 0x40);
	poke32(U_GAME_PTR, U_GAME_STATE);
	poke32(U_PLANET, 5);
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	mb = menu_mailbox();
	press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(G.active == MH_SHIM + SH_GS && G.main_up, "the menu is up on its root");

	/* A page of ten: a slider first, a value with a range too long for one, a slider last. */
	post_page(mb, 0x7E000101u, 10, page1, 3);
	pframe(0);
	pframe(0);
	check(G.built && G.rows == 10 && G.vis == 8 && made(AV_SLIDERS, 'C') && wid(AV_SLIDERS)->parent == AV_ROOT,
	      "the page on AUDIO / VISUAL's look, with a container of sliders under the panel's root");
	n = 1;
	for (i = 0; i < 8; i++) {
		h = AV_SLOT0 + 3 * (u32)i;
		n = n && made(h, 'Q') && made(h + 1, 'R') && made(h + 2, 'R') && wid(h)->parent == AV_SLIDERS &&
		    wid(h)->anchor_x == 1 && wid(h)->anchor_y == 3 && wid(h)->color == AV_GROOVE_COLOR2 &&
		    wid(h + 1)->color == AV_TRACK_COLOR2 && wid(h + 2)->color == AV_KNOB_COLOR &&
		    near(wid(h)->y, 0.445f + 0.0532f * (float)i) && near(wid(h + 1)->x, 0.765f) &&
		    near(wid(h + 1)->sx, 0.24f) && near(wid(h + 2)->sx, 0.01953f);
	}
	check(n && !made(AV_SLOT0 + 24, 'Q') && !G.widgets_bad,
	      "a slot for each of the eight rows on screen: the groove, the track and the knob, AUDIO / VISUAL's");
	check(wid(AV_SLOT0)->visible && wid(AV_SLOT0 + 1)->visible && wid(AV_SLOT0 + 2)->visible &&
	      near(wid(AV_SLOT0 + 2)->x, 0.653f + 0.232f * 0.5f) && near(wid(AV_SLOT0 + 2)->y, 0.445f),
	      "Health, 5 of 0..10: its slot's slider shown, the knob half way along");
	check(!wid(AV_SLOT0 + 3)->visible && !wid(AV_SLOT0 + 15)->visible && !wid(AV_SLOT0 + 17)->visible,
	      "a toggle's slot and Bolts' (a range too long for a bar) show none");
	check(strcmp(cfg_label(0, a), "HEALTH 5") == 0 && strcmp(cfg_value(0, a), " ") == 0 &&
	      strcmp(cfg_value(5, a), "7") == 0 && strcmp(cfg_label(5, a), "BOLTS") == 0,
	      "a slider's value cell is blank, as AUDIO / VISUAL's MUSIC VOLUME, its number after its label; Bolts keeps its "
	      "number in the value cell");

	/* The same page again, its value moved: the knob follows; off the bar, it sits at the end nearer. */
	memcpy(page2, page1, sizeof(page2));
	page2[0].value = 10;
	post_page(mb, 0x7E000101u, 10, page2, 3);
	pframe(0);
	check(near(wid(AV_SLOT0 + 2)->x, 0.885f), "10 of 0..10: the knob at the end");
	page2[0].value = 12;
	post_page(mb, 0x7E000101u, 10, page2, 3);
	pframe(0);
	check(near(wid(AV_SLOT0 + 2)->x, 0.885f), "12, past the highest: still at the end");
	page2[0].value = 0xFFFFFFFFu;
	post_page(mb, 0x7E000101u, 10, page2, 3);
	pframe(0);
	check(near(wid(AV_SLOT0 + 2)->x, 0.653f), "-1, under the lowest: at the start");

	/* Down to the last row: the list scrolls, and the sliders go with their rows. */
	for (i = 0; i < 9; i++) pframe(PADB_DOWN);
	check(G.top == 2 && G.top + G.off == 9 && wid(AV_SLOT0 + 21)->visible && !wid(AV_SLOT0)->visible &&
	      near(wid(AV_SLOT0 + 23)->x, 0.653f + 0.232f * 0.5f) && near(wid(AV_SLOT0 + 23)->y, 0.445f + 0.0532f * 7.0f),
	      "Speed, the tenth row, in the last slot on screen, its knob half way; the first slot now a toggle's, empty");
	G.slide = 0x3C000000u;
	pframe(0);
	check(!wid(AV_SLOT0 + 21)->visible, "while the rows slide, no slider shows");
	G.slide = 0;
	pframe(0);
	check(wid(AV_SLOT0 + 21)->visible, "and it comes back where they stop");
	G.sound = 0;
	pframe(PADB_RIGHT);
	check(G.sound == SND_MOVE && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "Right on a slider: an event, and the cursor's tick, as AUDIO / VISUAL's sliders tick");

	G.closing = 1;
	pframe(0);
	pframe(0);
	check(!G.paused && !made(AV_SLIDERS, 'C') && !made(AV_SLOT0 + 2, 'R'), "closed: the sliders go with layer 17");
	check(quit(), "quit UYA");
}

/* --------------------------------------------- the GALACTIC MAP, the Planets row's picker */

/* The game's name table for planets 0 .. 29, {location, planet} (0xC409E4, as the executable has it). */
static const u32 map_texts[30][2] = {
	{ 0, 0 }, { 0xC9, 0xBB7 }, { 0xCA, 0xBB8 }, { 0xCB, 0xBB9 }, { 0xCC, 0xBBA }, { 0xCD, 0xE8 },
	{ 0x135B, 0xBB9 }, { 0xCF, 0xBBB }, { 0xD0, 0xBBC }, { 0xD1, 0xBBD }, { 0xD2, 0xBBE },
	{ 0xD3, 0xBBF }, { 0xD4, 0xBC0 }, { 0xD5, 0xBC1 }, { 0xD6, 0xBC2 }, { 0x135C, 0x1362 },
	{ 0xD8, 0xBC3 }, { 0xD9, 0xBC4 }, { 0xDA, 0xBC5 }, { 0xDB, 0xBC6 }, { 0xD3B, 0xBC8 },
	{ 0xDD, 0xBBF }, { 0xDE, 0xBC8 }, { 0xDF, 0xBC1 }, { 0xD3C, 0xD3F }, { 0, 0 },
	{ 0xD3D, 0xBC3 }, { 0x135D, 0xBBC }, { 0xC8E, 0xBBC }, { 0xD3E, 0xBBD }
};

static int map_texts_are_the_games(void)
{
	u32 p;
	for (p = 0; p < 30; p++) {
		if (r32(U_MAP_TEXTS + 8 * p) != map_texts[p][0] || r32(U_MAP_TEXTS + 8 * p + 4) != map_texts[p][1]) return 0;
	}
	return 1;
}

/* The entry of the map's list that holds planet p, or -1. */
static int map_index(u32 p)
{
	u32 i;
	for (i = 0; i < r32(U_MAP_COUNT); i++) {
		if (r32(U_MAP_ENTRIES + i * U_MAP_ENTRY_SIZE) == p) return (int)i;
	}
	return -1;
}

/* Down on the map until its cursor is on planet p (at most 40 frames, the list being 30 at most). */
static void map_down_to(u32 p)
{
	int i;
	for (i = 0; i < 40 && r32(U_MAP_ENTRIES + r32(U_MAP_CURSOR) * U_MAP_ENTRY_SIZE) != p; i++) pframe(PADB_DOWN);
}

/* The save's own list, as UnlockPlanet keeps it: these planets, then zeros. */
static const u32 save_list[] = { 1, 2, 3, 4, 5, 11, 12, 7, 6 };

static int save_list_is_intact(void)
{
	u32 i;
	for (i = 0; i < 60; i++) {
		if (r32(U_UNLOCKED + 4 * i) != (i < sizeof(save_list) / sizeof(save_list[0]) ? save_list[i] : 0)) return 0;
	}
	return 1;
}

/* Open the menu with the combo and press the cross on PLANETS: the map is queued. */
static void open_picker(void)
{
	press(COMBO_MENU_DEFAULT);
	play_frame(0);
	tick(2);
	w32(U_CURSOR, 2);
	pframe(0x40);
}

static void test_picker_uya(void)
{
	const struct game_api *g;
	const char * const *pl;
	const u8 *order;
	char a[80], b[80];
	u32 mb, slot, seq, frames, i, k, n;
	u8 count;
	int ok;

	group("console menu: UYA's Planets row, the game's own GALACTIC MAP");

	memset(&G, 0, sizeof(G));
	memset(&M, 0, sizeof(M));
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
	put_str(MSG_AT + 0x30, "\x13 MISSIONS");
	for (i = 0; i < 30; i++) {
		poke32(U_MAP_TEXTS + 8 * i, map_texts[i][0]);
		poke32(U_MAP_TEXTS + 8 * i + 4, map_texts[i][1]);
	}
	for (i = 0; i < 60; i++) poke32(U_UNLOCKED + 4 * i, i < sizeof(save_list) / sizeof(save_list[0]) ? save_list[i] : 0);
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	mb = menu_mailbox();
	check(menu_state() == MENU_READY && peek32(MH_SITE_MAP_LIST) != MH_ORIG_MAP_LIST,
	      "the renderer is in, the map's list word with it");

	/* The list: every planet qwark loads that the map can draw, in PLANET_LIST's menu order. */
	g = session_game();
	order = g != NULL ? g->planet_order : NULL;
	pl = g->planet_names(&count);
	n = 0;
	ok = order != NULL;
	for (i = 0; ok && i < count; i++) {
		u32 p = order[i];
		if (p == 0 || p == 15 || p == 25 || p >= 30) continue;
		ok = n < MH_MAP_LIST_LEN && map_list_c[n] == p;
		n++;
	}
	check(ok && n == MH_MAP_LIST_LEN && n == 27,
	      "the Trainer's list is rac3.c's menu order less the gaps and the vid comics (no picture in the map): 27");

	/* CHEATS first (a CONFIG page, round 4's picture), then back to the root, so the map follows a picture. */
	press(COMBO_MENU_DEFAULT);
	play_frame(0);
	tick(2);
	w32(U_CURSOR, 1);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.pic_state == 1 && G.pic_taken == 2, "CHEATS: the picture source set up, two buffers taken");
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.main_up && G.pic_state == 8 && G.pic_taken == 0, "back on the root: the picture torn down");

	/* The cross on PLANETS: the map over the menu, no event. */
	w32(U_CURSOR, 2);
	pframe(0x40);
	slot = MH_SHIM + SH_GS + MH_GS_SIZE * (r8(MH_SHIM + SH_CUR) ^ 1u);
	check(G.pending == slot && r32(slot + 4) == MH_FN_MAP_INIT && r32(slot + 8) == MH_FN_MAP_DEINIT &&
	      r32(slot + 12) == MH_FN_MAP_UPDATE && r32(slot + 16) == U_DESC_MAP_PRE && r32(slot + 20) == U_DESC_MAP_POST &&
	      r32(MH_SHIM + SH_GS + MH_GS_SIZE * r8(MH_SHIM + SH_CUR)) == 0 &&
	      peek32(mb + MBOX_OFF_EV_SEQ) == peek32(mb + MBOX_OFF_EV_ACK),
	      "the cross on PLANETS: the map queued on the menu's other GuiState, its init, deinit and update the "
	      "Trainer's, its renders the map's; the menu's screen reset; no event posted");
	frames = peek32(mb + MBOX_OFF_FRAMES);
	pframe(0);
	check(G.active == slot && M.up && M.inits == 1 && !G.main_up && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER &&
	      mh_picker_up() &&
	      r32(MH_SHIM + SH_UP) == 0, "the next frame: the menu's screen ends, not closed but under the map");
	check(M.flag_in_build == 1 && r8(MH_SHIM + SH_MAP) == 0 && r32(U_MAP_COUNT) == 27,
	      "the build ran on the Trainer's list (SH_MAP up for the game's init only): 27 entries");
	check(!M.pic_clash && M.pic_held == 0,
	      "the map found picture source 1 and the pause's picture buffers free: nothing of the menu's held");
	ok = 1;
	for (i = 0; i < 27; i++) ok = ok && r32(U_MAP_ENTRIES + i * U_MAP_ENTRY_SIZE) == map_list_c[i];
	check(ok && map_index(1) == 0 && map_index(6) >= 0 && map_index(29) >= 0 && map_index(20) >= 0,
	      "in the menu's order, Veldin, Phoenix Rescue, Launch Site and the mission levels among them");
	/*
	 * The pickers' round 2, the user's call: every entry reads as the client's
	 * planet picker names it, PLANET_LIST's name, through the msg hook's
	 * 0xF040 + p (the build asked for that id, which game_msg answers as the
	 * stub does, out of the data image's table).
	 */
	ok = 1;
	for (i = 0; i < 27; i++) {
		u32 p = r32(U_MAP_ENTRIES + i * U_MAP_ENTRY_SIZE);
		ok = ok && M.text[i] == MH_MSG_PLANET + p && p < count &&
		     strcmp(read_str(game_msg(M.text[i]), a, 80), pl[p]) == 0;
		if (!ok) {
			printf("    entry %u, planet %u: text 0x%X, \"%s\"\n", (unsigned)i, (unsigned)p, (unsigned)M.text[i], a);
			break;
		}
	}
	for (i = 0; ok && i < 27; i++) {
		for (k = i + 1; k < 27; k++) ok = ok && M.text[i] != M.text[k];
	}
	check(ok, "every entry named as the client's planet picker names it, PLANET_LIST's name (Phoenix Rescue, "
	      "Holostar Clank, Aquatos Clank, Aquatos Sewers, Tyhrranosis Rangers ...), each its own");
	read_str(game_msg(M.text[map_index(6)]), a, 80);
	check(strcmp(a, "Phoenix Rescue") == 0 && strcmp(read_str(game_msg(M.text[map_index(23)]), a, 80), "Holostar Clank") == 0 &&
	      strcmp(read_str(game_msg(M.text[map_index(13)]), a, 80), "Holostar") == 0,
	      "Phoenix Rescue (round 1: \"The Phoenix Attacked\"), Holostar Clank (\"Lot 43\"), Holostar (the game's "
	      "\"Holostar Studios\")");
	check(map_texts_are_the_games() && save_list_is_intact(),
	      "and the name table is the game's again after the init; the save's list was never written");
	check(strcmp(text_of(U_MAP_HINT, a, 80), "\x13 VID-COMICS") == 0,
	      "Square's hint, MISSIONS in the game's map, says VID-COMICS, in its own glyph and capitals");
	tick(8);
	check(menu_model_depth() == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER,
	      "qwark leaves the menu where it was while the map is up");

	/* The map's own update for the cursor. */
	n = M.updates;
	pframe(PADB_DOWN);
	check(M.updates == n + 1 && r32(U_MAP_CURSOR) == 1, "Down: the map's own update moves the cursor");

	/* Square: VID-COMICS (the pickers' round 2), back to the menu and on to qwark's page of the vid comics. */
	G.sound = 0;
	pframe(PADB_SQUARE);
	check(M.updates == n + 1 && M.missions == 0 && G.pending == MH_SHIM + SH_GS + MH_GS_SIZE * r8(MH_SHIM + SH_CUR) &&
	      G.sound == SND_CHANGE && r8(MH_SHIM + SH_MAP_LIST) == 1,
	      "Square: no MISSIONS (whose travel would leave from play); the menu's screen queued, for the vid comics");
	pframe(0);
	check(!M.up && G.main_up && r32(U_CURSOR) == 2 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN &&
	      !mh_picker_up() && peek8(mb + MBOX_OFF_EV_KIND) == MBOX_EV_PICKER_OTHERS &&
	      peek8(mb + MBOX_OFF_EV_ROW) == 2 && peek16(mb + MBOX_OFF_EV_ID) == MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_PLANETS) &&
	      peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK) &&
	      r8(MH_SHIM + SH_MAP_LIST) == 0 && G.pending == 0,
	      "the root again, and the cross on PLANETS, which the picker answers this once with MH_PICK_OTHERS: "
	      "MBOX_EV_PICKER_OTHERS on the Planets row, not ACTIVATE");
	tick(2);
	pframe(0);
	pframe(0);
	check(menu_model_depth() == 2 && menu_model_page() == MENU_PAGE_ID(MENU_PAGE_PLANETS, MENU_PLANETS_OTHERS) &&
	      G.built && G.rows == 5 && M.inits == 1 && strcmp(read_str(G.title, b, 80), "VID-COMICS") == 0,
	      "qwark's page of the vid comics, a CONFIG page titled VID-COMICS, five rows");
	ok = 1;
	for (i = 0; i < 5; i++) {
		static const char *const comics[5] = { "VID COMIC 1", "VID COMIC 2", "VID COMIC 3", "VID COMIC 4", "VID COMIC 5" };
		ok = ok && strcmp(cfg_label((int)i, a), comics[i]) == 0;
	}
	check(ok, "the base vid comics 1 to 5, in their order, PLANET_LIST's names in UYA's capitals (no 6, no 1 SE)");
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.main_up && r32(U_CURSOR) == 2 && menu_model_depth() == 1, "Triangle: the root, the cursor on PLANETS");

	/* The map again, Square, and a vid comic loaded from qwark's page. */
	pframe(0x40);
	pframe(0);
	check(M.up && M.inits == 2, "the map again");
	pframe(PADB_SQUARE);
	pframe(0);
	tick(2);
	pframe(0);
	pframe(0);
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_PLANETS, MENU_PLANETS_OTHERS) && G.rows == 5,
	      "Square: the vid comics' page again");
	pframe(PADB_DOWN);
	pframe(PADB_DOWN);
	pframe(PADB_DOWN);
	check(strcmp(cfg_label((int)shim_cursor(0), a), "VID COMIC 4") == 0, "Down three times: VID COMIC 4");
	pframe(0x40);
	for (i = 0; i < 80 && peek32(RAC3_LOAD_PLANET) != 1; i++) {
		tick(1);
		if (G.paused) pframe(0);
	}
	check(peek32(RAC3_LOAD_PLANET) == 1 && peek32(RAC3_DEST_PLANET) == 32 && !G.paused &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "the cross on it: the menu closes, and qwark's planet load of Vid Comic 4 (planet 32) once the game plays, "
	      "as the Planets page's rows load");
	poke32(RAC3_LOAD_PLANET, 0);
	poke32(RAC3_DEST_PLANET, 0);
	tick(8);

	/* The map again, for Triangle. */
	open_picker();
	pframe(0);
	check(M.up && M.inits == 3 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER, "the map again");
	n = M.updates;
	frames = peek32(mb + MBOX_OFF_FRAMES);

	/* Triangle: back to the menu, on PLANETS. */
	G.sound = 0;
	pframe(PADB_TRIANGLE);
	check(G.pending == MH_SHIM + SH_GS + MH_GS_SIZE * r8(MH_SHIM + SH_CUR) && G.sound == SND_CHANGE &&
	      M.missions == 0 && M.battle == 0 && M.updates == n, "Triangle: the menu's screen queued, the change sound");
	pframe(0);
	check(!M.up && M.deinits == 3 && G.main_up && G.count == 6 && r32(U_CURSOR) == 2 && G.paused &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && peek32(mb + MBOX_OFF_FRAMES) >= frames &&
	      !mh_picker_up() && r32(U_KIND) == 0,
	      "the root again, the cursor on PLANETS, the pause still up, the menu open with its frames counting on, "
	      "nothing set to travel");
	tick(8);
	check(menu_model_depth() == 1, "qwark: the same root");
	w32(U_CURSOR, 1);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.built && G.pic_state == 1 && G.pic_taken == 2, "and CHEATS from there: its picture set up again");
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	check(G.main_up && r32(U_CURSOR) == 1 && G.pic_taken == 0, "back to the root");

	/* X on a planet: the pick to qwark, a plain close, qwark's load. */
	w32(U_CURSOR, 2);
	pframe(0x40);
	pframe(0);
	check(M.up && M.inits == 4, "the cross on PLANETS again: the map");
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	map_down_to(9);
	G.sound = 0;
	pframe(0x40);
	check(peek32(mb + MBOX_OFF_PICK_PLANET) == 9 &&
	      peek32(mb + MBOX_OFF_PICK_SEQ) == seq + 1 && G.sound == SND_MAP_GO && G.pending == EXIT_GS,
	      "X on Tyhrranosis: the planet, then the seq, in the mailbox; the map's travel sound; the pause's exit queued");
	pframe(0);
	check(!G.paused && !M.up && M.exit_kind == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      !mh_picker_up(), "the pause closes plainly (kind 0, no travel of the game's), the menu closed");
	for (i = 0; i < 40 && peek32(RAC3_LOAD_PLANET) != 1; i++) tick(1);
	check(peek32(RAC3_LOAD_PLANET) == 1 && peek32(RAC3_DEST_PLANET) == 9 && menu_model_depth() == 1,
	      "qwark takes the pick once the game plays: its planet load of Tyhrranosis; the menu back on its root");
	poke32(RAC3_LOAD_PLANET, 0);
	poke32(RAC3_DEST_PLANET, 0);
	tick(8);

	/* Veldin, which the game's own map refuses, and the planet you are on. */
	open_picker();
	pframe(0);
	pframe(0x40);
	pframe(0);
	for (i = 0; i < 40 && peek32(RAC3_LOAD_PLANET) != 1; i++) tick(1);
	check(M.exit_kind == 0 && peek32(RAC3_LOAD_PLANET) == 1 && peek32(RAC3_DEST_PLANET) == 1,
	      "X on Veldin, the first: picked and loaded (the game's map takes no Veldin)");
	poke32(RAC3_LOAD_PLANET, 0);
	tick(8);
	open_picker();
	pframe(0);
	map_down_to(5);
	pframe(0x40);
	pframe(0);
	for (i = 0; i < 40 && peek32(RAC3_LOAD_PLANET) != 1; i++) tick(1);
	check(M.exit_kind == 0 && peek32(RAC3_DEST_PLANET) == 5,
	      "X on Daxx, the planet you are on: loaded again, as the Planets list would");
	poke32(RAC3_LOAD_PLANET, 0);
	tick(8);

	/* Start: everything closes, nothing picked. */
	open_picker();
	pframe(0);
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	G.closing = 1;
	pframe(0);
	check(G.closing == 0 && G.pending == EXIT_GS && M.updates > 0, "Start: acknowledged, the pause's exit queued");
	pframe(0);
	tick(8);
	check(!G.paused && M.exit_kind == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peek32(mb + MBOX_OFF_PICK_SEQ) == seq && peek32(RAC3_LOAD_PLANET) == 0 &&
	      menu_model_depth() == 1, "a plain close: no reload, no pick, the menu closed on its root");

	/* The combo with the map up: qwark asks for a close, which the Trainer's map serves. */
	open_picker();
	pframe(0);
	check(M.up && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER, "the map up");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with the map up asks for a close");
	pframe(0);
	check(peek8(mb + MBOX_OFF_REQUEST) == 0 && G.pending == EXIT_GS, "the map takes it: the pause's exit queued");
	pframe(0);
	tick(8);
	check(!G.paused && M.exit_kind == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peek32(RAC3_LOAD_PLANET) == 0 && menu_model_depth() == 1, "everything closed plainly, nothing loaded");

	/* Blackwater City: Triangle and Start would go to BATTLE MISSIONS in the game's map. */
	poke32(U_PLANET, 12);
	open_picker();
	pframe(0);
	pframe(PADB_TRIANGLE);
	pframe(0);
	check(M.battle == 0 && G.main_up && r32(U_CURSOR) == 2 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN,
	      "on Blackwater City, Triangle: back to the menu, not BATTLE MISSIONS");
	pframe(0x40);
	pframe(0);
	G.closing = 1;
	pframe(0);
	pframe(0);
	tick(8);
	check(M.battle == 0 && !G.paused && M.exit_kind == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "and Start: the pause closes plainly");
	poke32(U_PLANET, 5);

	/* The game's own map (the ship's): its own list, names, and travel, untouched. */
	n = M.inits;
	G.paused = 1;
	w32(U_KIND, 0);
	mock_swap_to(game_map());
	runner();
	check(M.up && M.inits == n + 1 && M.flag_in_build == 0 && r32(U_MAP_COUNT) == 7 && map_index(1) < 0 &&
	      map_index(6) < 0 && map_index(2) == 0 && map_index(7) == 6,
	      "the game's own map: the save's list through the game's filter (no Veldin, no Phoenix Rescue), seven");
	check(M.text[map_index(3)] == 0xCB && strcmp(text_of(U_MAP_HINT, a, 80), "\x13 MISSIONS") == 0 &&
	      map_texts_are_the_games(), "the game's names, its MISSIONS hint");
	map_down_to(4);
	pframe(0x40);
	pframe(0);
	check(!G.paused && M.exit_kind == 1 && M.exit_dest == 4 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peek32(RAC3_LOAD_PLANET) == 0, "X on Marcadia: the game's own travel (kind 1), nothing of the Trainer's");
	G.paused = 1;
	w32(U_KIND, 0);
	mock_swap_to(game_map());
	runner();
	pframe(PADB_TRIANGLE);
	pframe(0);
	check(!G.paused && M.exit_kind == 1 && M.exit_dest == 5, "Triangle there: the game's own reload of Daxx, as ever");
	tick(8);

	/* A pause whose Start does not leave it: no map, qwark's own list. */
	poke8(U_NO_EXIT, 1);
	open_picker();
	check(G.pending == 0 && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK) &&
	      !mh_picker_up(), "with U_NO_EXIT set the picker refuses, and the cross posts the event");
	tick(2);
	pframe(0);
	pframe(0);
	check(menu_model_depth() == 2 && G.built && G.rows == 24 && !G.main_up,
	      "qwark's own Planets list, the fallback: a CONFIG page, 24 of the 34 a window");
	poke8(U_NO_EXIT, 0);
	G.closing = 1;
	pframe(0);
	pframe(0);
	tick(8);
	check(!G.paused && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED, "closed");

	check(quit(), "quit UYA");
}

void test_menu_uya(void)
{
	test_model_uya();
	test_install_uya();
	test_renderer_uya();
	test_sliders_uya();
	test_picker_uya();
}
