/*
 * Up Your Arsenal's console menu shim: qwark's pages on the TV with the game's
 * own pause-menu widgets. mh_rac3.h says where it lives, what it hooks and what
 * it calls; mh_shim.h is the interface to the core (mh_core.c), which reads the
 * mailbox, composes the text and posts the events. This file only talks to the
 * game, the way probe N3b did on hardware:
 *
 *   MAIN    a page of links and buttons on the main pause's own list: its init,
 *           update and deinit run on the shim's row table, whose labels are
 *           text ids the msg_string hook answers with the core's rows;
 *   CONFIG  a page of labels and controls on CHEATS' screen: the submenu frame
 *           and the grid list of raw strings, two columns, seven rows on
 *           screen and the rest scrolled to.
 *
 * The menu's screen is one of two GuiStates of the shim's own, whose init,
 * update and deinit are the three entries below whatever the page is: the init
 * asks the core which widget the page wants and builds it. Moving to another
 * page the widget cannot take in place, or to the other widget, is the game's
 * own way between screens: SwapTo the other GuiState and reset this one, so the
 * game's thread runner ends the one and starts the other.
 *
 * The game reaches it through
 *   mh_msg_stub, mh_rows*_stub   the main pause's labels and its init's tables (assembler);
 *   mh_inject_stub, mh_combo_stub, mh_launch_stub   the combo: the Start path
 *                    opens the pause for an open request, and the launcher
 *                    starts the menu instead of the main pause (assembler,
 *                    the last one calling mh_launch);
 *   mh_help          the HELP row's callback, in the main pause's tables;
 *   mh_init, mh_update, mh_deinit   the menu's GuiStates, through descriptors;
 *   mh_row           a MAIN row's callback, which the main pause's update calls on the cross.
 *
 * Everything the game keeps a pointer to - the GuiStates, the rows, their text,
 * the title, the grid's cells - is in the renderer's state, which stays put for
 * as long as the game runs and changes only on the game's thread. Every access
 * to game memory goes through r8..w32 (mh_common.h), so the host tests run this
 * file on qwark's fake console with the real bytes.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define SH(o)    ((u32)MH_SHIM + (u32)(o))
#define GSA(k)   SH(SH_GS + (u32)(k) * MH_GS_SIZE)
#define ROWS     SH(SH_ROWS)
#define CFG      SH(SH_GRIDCFG)
#define CELLS    SH(SH_CELLS)

/* The shim's state starts where the core's ends, and the cells hold a CONFIG page. */
typedef char mh_rac3_layout[(MH_STATE + MH_CORE_SIZE <= MH_SHIM &&
                             MH_SHIM + SH_SIZE <= MH_DATA_END &&
                             SH_CELLS + SH_CELLS_SIZE <= SH_SIZE &&
                             MH_CFG_ROWS * U_GRID_COLS * U_CELL_SIZE < SH_CELLS_SIZE &&
                             SH_ROWS + MH_MAIN_ROWS * U_ROW_SIZE <= SH_GRIDCFG &&
                             SH_GS + 2 * MH_GS_SIZE <= SH_ROWS) ? 1 : -1];

/* The entries the game reaches; file-local in the host tests, which build two renderers. */
MH_API void mh_init(u32 gs);
MH_API void mh_deinit(u32 gs);
MH_API void mh_update(u32 gs);
MH_API void mh_help(void);
MH_API void mh_row(void);
MH_API u32  mh_launch(void);

/* The descriptors of the shim's own functions, as the game will hold them. */
#ifdef MH_HOST
#define MH_FN_INIT       1u
#define MH_FN_DEINIT     2u
#define MH_FN_UPDATE     3u
#define MH_FN_ROW        4u
#else
#define MH_FN_INIT       ((u32)mh_init)
#define MH_FN_DEINIT     ((u32)mh_deinit)
#define MH_FN_UPDATE     ((u32)mh_update)
#define MH_FN_ROW        ((u32)mh_row)
#endif

typedef int  (*fn_i_v)(void);
typedef u32  (*fn_u_v)(void);
typedef u32  (*fn_u_i)(int);
typedef int  (*fn_i_u)(u32);
typedef void (*fn_v_v)(void);
typedef void (*fn_v_i)(int);
typedef void (*fn_v_u)(u32);
typedef void (*fn_v_uu)(u32, u32);
typedef void (*fn_v_ui)(u32, int);
typedef void (*fn_v_uii)(u32, int, int);
typedef void (*fn_v_uiii)(u32, int, int, int);
typedef void (*fn_v_iii)(int, int, int);
typedef u32  (*fn_u_uii)(u32, int, int);
typedef void (*fn_v_uf5)(u32, float, float, float, float, float);
typedef void (*fn_v_uf4)(u32, float, float, float, float);
typedef void (*fn_frame)(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, float);

#define swap_to         GAME_FN(fn_v_u,    OPD_SWAP_TO,         mock_swap_to)
#define reset_state     GAME_FN(fn_v_u,    OPD_RESET_STATE,     mock_reset_state)
#define set_text        GAME_FN(fn_v_uu,   OPD_SET_TEXT,        mock_set_text)
#define main_init       GAME_FN(fn_v_u,    OPD_MAIN_INIT,       mock_main_init)
#define main_update     GAME_FN(fn_v_u,    OPD_MAIN_UPDATE,     mock_main_update)
#define main_deinit     GAME_FN(fn_v_u,    OPD_MAIN_DEINIT,     mock_main_deinit)
#define main_pause      GAME_FN(fn_u_v,    OPD_MAIN_PAUSE,      mock_main_pause)
#define pause_exit      GAME_FN(fn_u_v,    OPD_PAUSE_EXIT,      mock_pause_exit)
#define start_close     GAME_FN(fn_i_v,    OPD_START_CLOSE,     mock_start_close)
#define start_ack       GAME_FN(fn_v_v,    OPD_START_ACK,       mock_start_ack)
#define frame           GAME_FN(fn_frame,  OPD_FRAME,           mock_frame)
#define get_grid        GAME_FN(fn_u_i,    OPD_GET_GRID,        mock_get_grid)
#define grid_build      GAME_FN(fn_v_uu,   OPD_GRID_BUILD,      mock_grid_build)
#define grid_update     GAME_FN(fn_v_ui,   OPD_GRID_UPDATE,     mock_grid_update)
#define grid_cursor     GAME_FN(fn_i_u,    OPD_GRID_CURSOR,     mock_grid_cursor)
#define grid_set_cursor GAME_FN(fn_v_ui,   OPD_GRID_SET_CURSOR, mock_grid_set_cursor)
#define grid_cell       GAME_FN(fn_u_uii,  OPD_GRID_CELL,       mock_grid_cell)
#define grid_color      GAME_FN(fn_v_uii,  OPD_GRID_COLOR,      mock_grid_color)
#define grid_align      GAME_FN(fn_v_uiii, OPD_GRID_ALIGN,      mock_grid_align)
#define frame_rect      GAME_FN(fn_v_uf5,  OPD_FRAME_RECT,      mock_frame_rect)
#define cursor_bar      GAME_FN(fn_v_uf4,  OPD_CURSOR_BAR,      mock_cursor_bar)
#define grid_free       GAME_FN(fn_v_u,    OPD_GRID_FREE,       mock_grid_free)
#define clear_layer     GAME_FN(fn_v_i,    OPD_CLEAR_LAYER,     mock_clear_layer)
#define play_sound      GAME_FN(fn_v_iii,  OPD_PLAY_SOUND,      mock_play_sound)

/*
 * CHEATS' grid configuration (0xC506EC, with the two floats CHEATS writes on its
 * first use): the cell buffer, its size and the rows are the shim's.
 */
static const u32 cfg_template[24] = {
	SH_CELLS_SIZE, 0, U_GRID_LAYER, 0, 0, U_GRID_COLS, 0x3F800000, 0x3EE66666,
	0x3D591687, 0x3DE147AE, 0x3F63D70A, 0, 0, 0x3EB33333, 0x3EB33333, 0,
	0, 0x3F400000, 0x3F400000, 0, 0, 0xFFFFFFFF, 0, 1
};

static u32 shown(void)
{
	return r8(MH_STATE + ST_SHOWN);
}

/* The GuiState of the menu's screen now. */
static u32 current(void)
{
	return GSA(r8(SH(SH_CUR)));
}

/* This frame's presses, as every pause screen reads them. */
static u32 edges(void)
{
	u32 pad = r32(U_PAD_PTR);
	return pad != 0 ? r32(pad + U_PAD_EDGES) : 0;
}

/*
 * GuiState k made ready to start: stopped, its calls the shim's, its renders
 * the pause screens' own. The thread runner calls its init once SwapTo makes it
 * the next.
 */
static u32 gs_ready(u32 k)
{
	u32 g = GSA(k);

	w32(g, 0);
	w32(g + 4, MH_FN_INIT);
	w32(g + 8, MH_FN_DEINIT);
	w32(g + 12, MH_FN_UPDATE);
	w32(g + 16, U_DESC_PRE);
	w32(g + 20, U_DESC_POST);
	w8(SH(SH_CUR), k);
	return g;
}

/* The menu's screen started again, on the other GuiState, the way a pause screen starts the next. */
static void restart(void)
{
	u32 self = current();

	swap_to(gs_ready(r8(SH(SH_CUR)) ^ 1));
	reset_state(self);
}

/*
 * The main pause's cursor is the one word the menu's list shares with the
 * game's: kept as the menu's screen first starts, put back each time it stops,
 * so the main pause finds its own again (the runner stops one GuiState before
 * it starts the next).
 */
static void arrive(void)
{
	if (r32(SH(SH_UP)) == 0) w32(SH(SH_GAME_CURSOR), r32(U_CURSOR));
	w32(SH(SH_UP), U_SCREEN_UP);
}

static void leave(void)
{
	w32(U_CURSOR, r32(SH(SH_GAME_CURSOR)));
	w32(SH(SH_UP), 0);
}

/* ------------------------------------------------------- the MAIN widget */

/*
 * The core's rows as the main pause's: {widget, text id, enabled, callback,
 * reset after}. The main pause's update moves the cursor past a row that is
 * not enabled and loops for ever on a list of none (0x2F4ABC), so a page whose
 * rows all are grey keeps its first enabled; the core ignores the cross there.
 */
static void main_rows(void)
{
	u32 n = mh_nrows(), i, r, any = 0;

	for (i = 0; i < n; i++) {
		if ((mh_row_flags(i) & MROWF_ENABLED) != 0) any = 1;
	}
	for (i = 0; i < n; i++) {
		r = ROWS + i * U_ROW_SIZE;
		w32(r, U_ROW_HANDLE + i);
		w32(r + 4, MH_MSG_ROW0 + i);
		w32(r + 8, ((mh_row_flags(i) & MROWF_ENABLED) != 0 || (any == 0 && i == 0)) ? 0x01000000u : 0);
		w32(r + 12, MH_FN_ROW);
		w32(r + 16, 0);
	}
}

/*
 * The main pause's init on the shim's table: the rows stubs hand it the table,
 * the count and the band while the table word is set. Its rows sit 0.06 apart
 * about 0.5 and its end pieces 0.13 above the first and 0.14 below the last,
 * as the game's bands for eight and nine rows are (0xC529D0, 0xC529D8).
 */
static void main_start(u32 gs)
{
	u32 n = mh_nrows();

	main_rows();
	w32(U_CURSOR, mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
	wf32(SH(SH_BAND), 0.40f - 0.03f * (float)n);
	wf32(SH(SH_BAND) + 4, 0.61f + 0.03f * (float)n);
	w32(SH(SH_LIST_COUNT), n);
	w32(SH(SH_LIST_TABLE), ROWS);
	main_init(gs);
	w32(SH(SH_LIST_TABLE), 0);
}

/*
 * Left, Right, L1 and R1, which the main pause's list ignores, from the same
 * presses, unless the cross, Up, Down or Triangle came in the same frame.
 */
static void main_steps(u32 e)
{
	u32 b;

	if ((e & (r32(U_CONFIRM_MASK) | PADB_UP | PADB_DOWN | PADB_TRIANGLE)) != 0) return;
	if ((e & PADB_LEFT) != 0)       b = MH_BTN_LEFT;
	else if ((e & PADB_RIGHT) != 0) b = MH_BTN_RIGHT;
	else if ((e & PADB_L1) != 0)    b = MH_BTN_L1;
	else if ((e & PADB_R1) != 0)    b = MH_BTN_R1;
	else return;
	mh_core_press(0, b, r32(U_CURSOR));
}

/*
 * A MAIN page's frame: the core's, then Triangle, which is the menu's back
 * rather than the main pause's close, then the main pause's own update (the
 * cursor, the cross on a row, the Start close, the colours), then the steps.
 */
static void main_frame(u32 gs)
{
	u32 e;

	if (mh_core_frame(0) == MH_GONE) return;
	e = edges();
	if ((e & PADB_TRIANGLE) != 0) {
		mh_core_press(0, MH_BTN_BACK, r32(U_CURSOR));
		return;
	}
	main_update(gs);
	if (r32(gs) == 0) return;
	main_steps(e);
}

/* ----------------------------------------------------- the CONFIG widget */

/* A grid cell's text: char[64] at its start, which the grid hands SetText every frame. */
static void cell_put(u32 c, u32 s)
{
	u32 n = 0, ch;

	while (n < U_CELL_SIZE - 5) {
		ch = r8(s + n);
		if (ch == 0) break;
		w8(c + n, ch);
		n++;
	}
	w8(c + n, 0);
}

/* The rows into the grid's cells: labels left, values right, the colours by the core's flags. */
static void cfg_fill(u32 g)
{
	u32 n = mh_nrows(), i, f;

	for (i = 0; i < n; i++) {
		cell_put(grid_cell(g, 0, (int)i), mh_label(i));
		cell_put(grid_cell(g, 1, (int)i), mh_value(i));
		f = mh_row_flags(i);
		grid_color(g, (int)i, (f & MROWF_ENABLED) == 0 ? U_COLOR_SKIP :
		                      (f & MROWF_ON) != 0 ? U_COLOR_ON : U_COLOR_OFF);
	}
}

/*
 * CHEATS' screen with the core's page (CHEATS' init 0x2E2558 and probe N3b):
 * the frame with the page's title, the grid built for the page's rows with
 * seven on screen, CHEATS' frame and cursor bar, the cells, the alignment
 * that keeps both columns inside the frame, and the cursor on its row.
 */
static void cfg_start(void)
{
	u32 n = mh_nrows(), g, i;

	frame(U_LAYER_FRAME, U_FRAME_CONTAINER, 0x000D0002, 0x000D0003, 0x000D0004, 0x000D0000, 0x000D0001,
	      U_TITLE_HANDLE, 0x000D0008, mh_title(), 0x000D0006,
	      r8(U_JAPANESE) != 0 ? U_MSG_SELECT_JP : U_MSG_SELECT, 0x000D0007, U_MSG_BACK, 8.5f);
	for (i = 0; i < 24; i++) w32(CFG + 4 * i, cfg_template[i]);
	w32(CFG + 4, CELLS);
	w32(CFG + 12, n);
	w32(CFG + 16, n < U_GRID_VISIBLE ? n : U_GRID_VISIBLE);
	g = get_grid(0);
	grid_build(g, CFG);
	frame_rect(g, 0.5f, 0.79f, 0.045f, 0.38f, 0.35f);
	cursor_bar(g, 0.5f, 0.58f, 0.81f, 0.56f);
	cfg_fill(g);
	grid_align(g, 0, 1, 3);
	grid_align(g, 1, 2, 3);
	grid_set_cursor(g, (int)mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
}

/*
 * A CONFIG page's frame, as CHEATS' update (0x2E2DF8): the core's, the grid
 * (Up, Down, the scroll, their sound, the cells pushed to the widgets), one
 * press on the grid's row, then the Start close.
 */
static void cfg_frame(u32 gs)
{
	u32 g, e, b = 0;

	if (mh_core_frame(0) == MH_GONE) return;
	e = edges();
	g = get_grid(0);
	grid_update(g, (int)e);

	if ((e & r32(U_CONFIRM_MASK)) != 0)  b = MH_BTN_CROSS;
	else if ((e & PADB_LEFT) != 0)       b = MH_BTN_LEFT;
	else if ((e & PADB_RIGHT) != 0)      b = MH_BTN_RIGHT;
	else if ((e & PADB_L1) != 0)         b = MH_BTN_L1;
	else if ((e & PADB_R1) != 0)         b = MH_BTN_R1;
	else if ((e & PADB_TRIANGLE) != 0)   b = MH_BTN_BACK;
	if (b != 0) mh_core_press(0, b, shim_cursor(0));
	if (r32(gs) == 0) return;

	if (start_close()) {
		start_ack();
		reset_state(gs);
		if (r8(U_NO_EXIT) == 0) swap_to(pause_exit());
	}
}

/* ------------------------------------------------------- the shim, for the core */

/*
 * The page shown refreshed in place. MH_RESTART_* name both of `how`'s bits for
 * both widgets, so a page that is new or has another number of rows never gets
 * here: the core starts the screen again for it instead.
 */
MH_API void shim_show(int p, u32 how)
{
	u32 i, n = mh_nrows();

	(void)p;
	(void)how;
	if (shown() == MBOX_PTYPE_CONFIG) {
		cfg_fill(get_grid(0));
		set_text(U_TITLE_HANDLE, mh_title());
		return;
	}
	/* The same page with new values: each row's text again, and whether it is enabled. */
	main_rows();
	for (i = 0; i < n; i++) set_text(U_ROW_HANDLE + i, mh_text(i));
}

MH_API void shim_switch(int p)
{
	(void)p;
	restart();
}

/*
 * Back from the root: to the main pause when the menu came from its HELP row,
 * whose init finds its cursor back on HELP; out of the pause from the combo,
 * as the main pause's own Triangle goes out (0x2F4C94). Start, Select and a
 * close request: out of the pause.
 */
MH_API void shim_close(int p, u32 how)
{
	u32 self = current();

	(void)p;
	if (how == MH_CLOSE_BACK && r8(SH(SH_FROM_HELP)) != 0) {
		swap_to(main_pause());
		reset_state(self);
		play_sound(SND_CHANGE, 0, 0);
		return;
	}
	swap_to(pause_exit());
	reset_state(self);
}

MH_API void shim_title(int p)
{
	(void)p;
	if (shown() == MBOX_PTYPE_CONFIG) set_text(U_TITLE_HANDLE, mh_title());
}

/* The game's pause screens have no description line. */
MH_API void shim_desc(int p)
{
	(void)p;
}

MH_API u32 shim_cursor(int p)
{
	int c;

	(void)p;
	if (shown() == MBOX_PTYPE_CONFIG) c = grid_cursor(get_grid(0));
	else c = (int)r32(U_CURSOR);
	return c < 0 ? 0 : (u32)c;
}

/*
 * The main pause plays its change sound after a row's callback itself, so the
 * cross on a MAIN row is the game's; everything else is the pause screens'
 * change sound, and moving a window the cursor's tick.
 */
MH_API void shim_sound(u32 snd)
{
	if (snd == MH_SND_FORWARD && shown() != MBOX_PTYPE_CONFIG) return;
	play_sound(snd == MH_SND_MOVE ? SND_MOVE : SND_CHANGE, 0, 0);
}

/* ------------------------------------------------------------ the entries */

/* The menu's GuiState starts: the page qwark posted, with the widget it asks for. */
MH_API void mh_init(u32 gs)
{
	arrive();
	if (mh_core_start(0) == MBOX_PTYPE_CONFIG) cfg_start();
	else main_start(gs);
}

/* Every frame it runs. */
MH_API void mh_update(u32 gs)
{
	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC) {
		/* The state is not the renderer's: leave the pause rather than run on it. */
		swap_to(pause_exit());
		reset_state(gs);
		return;
	}
	if (shown() == MBOX_PTYPE_CONFIG) cfg_frame(gs);
	else main_frame(gs);
}

/* It stops: the widget's own end, as CHEATS' deinit and the main pause's, and the cursor back. */
MH_API void mh_deinit(u32 gs)
{
	u32 was = shown();

	mh_core_stop(0);
	if (was == MBOX_PTYPE_CONFIG) {
		clear_layer(U_LAYER_FRAME);
		clear_layer(U_LAYER_FRAME2);
		grid_free(get_grid(0));
	} else {
		main_deinit(gs);
	}
	leave();
}

/*
 * The HELP row's callback, from the main pause's update, which resets itself
 * after it: the menu's screen starts over the main pause.
 */
MH_API void mh_help(void)
{
	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC) return;
	w8(SH(SH_FROM_HELP), 1);
	swap_to(gs_ready(0));
}

/* A MAIN row: the cross on whatever the cursor is on. */
MH_API void mh_row(void)
{
	mh_core_press(0, MH_BTN_CROSS, r32(U_CURSOR));
}

/*
 * From mh_launch_stub, in place of the launcher's call of the main pause's
 * getter: the menu's GuiState when the Start path opened this pause for an
 * open request, the main pause otherwise. The launcher then swaps to what this
 * answers.
 */
MH_API u32 mh_launch(void)
{
	if (r8(SH(SH_FORCE)) == 0 || r32(MH_STATE + ST_MAGIC) != MH_MAGIC) return main_pause();
	w8(SH(SH_FORCE), 0);
	w8(SH(SH_FROM_HELP), 0);
	mh_count(ST_OPENS);
	return gs_ready(0);
}
