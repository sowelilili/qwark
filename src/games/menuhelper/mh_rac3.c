/*
 * Up Your Arsenal's console menu shim: qwark's pages on the TV with the game's
 * own pause-menu widgets. mh_rac3.h says where it lives, what it hooks and what
 * it calls; mh_shim.h is the interface to the core (mh_core.c), which reads the
 * mailbox, composes the text and posts the events. This file only talks to the
 * game:
 *
 *   MAIN    a page of links and buttons on the main pause's own list: its init,
 *           update and deinit run on the shim's row table, whose labels are
 *           text ids the msg_string hook answers with the core's rows (probe
 *           N3b, on hardware);
 *   CONFIG  a page of labels and controls in the look of Options' AUDIO /
 *           VISUAL screen: the submenu frame with the title fitted to its
 *           banner, the picture panel above the list with one of that screen's
 *           pictures in it (round 4), and the grid list of raw strings in two
 *           columns with that screen's configuration and font, eight rows on
 *           screen and the rest scrolled to, the label left and the value
 *           right, and its sliders for a value the core marks as one.
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
 *   mh_row           a MAIN row's callback, which the main pause's update calls on the cross;
 *   mh_map_list_stub, mh_map_init, mh_map_update, mh_map_deinit   the GALACTIC
 *                    MAP the Planets row opens over the menu: its list, and its
 *                    GuiState's calls wrapped (the end of this file).
 *
 * Everything the game keeps a pointer to - the GuiStates, the rows, their text,
 * the title, the grid's cells - is in the renderer's state or its cells' span
 * (round 4), which stay put for as long as the game runs and change only on
 * the game's thread. Every access to game memory goes through r8..w32 (mh_common.h), so
 * the host tests run this file on qwark's fake console with the real bytes.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define SH(o)    ((u32)MH_SHIM + (u32)(o))
#define GSA(k)   SH(SH_GS + (u32)(k) * MH_GS_SIZE)
#define ROWS     SH(SH_ROWS)
#define CFG      SH(SH_GRIDCFG)
#define CELLS    ((u32)U_CELLS)

/* The shim's state starts where the core's ends, and the cells' span holds a CONFIG page. */
typedef char mh_rac3_layout[(MH_STATE + MH_CORE_SIZE <= MH_SHIM &&
                             MH_SHIM + SH_SIZE <= MH_DATA_END &&
                             U_CELLS + U_CELLS_SIZE <= U_CELLS_END && (U_CELLS & 3) == 0 &&
                             MH_CFG_ROWS * U_GRID_COLS * U_CELL_SIZE < U_CELLS_SIZE &&
                             SH_ROWS + MH_MAIN_ROWS * U_ROW_SIZE <= SH_GRIDCFG &&
                             SH_GRIDCFG + 24 * 4 <= SH_TITLE_SCALE &&
                             SH_SLOT_ROW + U_GRID_VISIBLE <= SH_PIC_WAIT &&
                             SH_SLOT_KNOB + 4 * U_GRID_VISIBLE <= SH_PIC_BUF && SH_PIC_BUF + 8 <= SH_MAP &&
                             SH_MAP_LIST < SH_MAP_HINT && SH_MAP_HINT + 16 <= SH_SIZE &&
                             SH_GS + 2 * MH_GS_SIZE <= SH_ROWS &&
                             MH_MAP_NAMES == MH_SHIM + SH_SIZE && (MH_MAP_NAMES & 15) == 0 &&
                             MH_MAP_LIST_LEN <= MH_MAP_PLANETS && MH_MAP_PLANETS <= 0x40 &&
                             (MH_MSG_PLANET & 0x3F) == 0 && MH_MSG_PLANET > MH_MSG_ROW0 + MH_MAIN_ROWS &&
                             MH_PICKER == 1 && MH_PICKER_OTHERS == 1) ? 1 : -1];

/* The Trainer's map's planets, in the stubs' read-only data after the names' pointers (mh_rac3_stubs.S). */
#define MAP_LIST ((u32)MH_MAP_NAMES + 4u * MH_MAP_PLANETS)

/* The entries the game reaches; file-local in the host tests, which build two renderers. */
MH_API void mh_init(u32 gs);
MH_API void mh_deinit(u32 gs);
MH_API void mh_update(u32 gs);
MH_API void mh_help(void);
MH_API void mh_row(void);
MH_API u32  mh_launch(void);
MH_API void mh_map_init(u32 gs);
MH_API void mh_map_deinit(u32 gs);
MH_API void mh_map_update(u32 gs);

/* The descriptors of the shim's own functions, as the game will hold them. */
#ifdef MH_HOST
#define MH_FN_INIT       1u
#define MH_FN_DEINIT     2u
#define MH_FN_UPDATE     3u
#define MH_FN_ROW        4u
#define MH_FN_MAP_INIT   5u
#define MH_FN_MAP_DEINIT 6u
#define MH_FN_MAP_UPDATE 7u
#else
#define MH_FN_INIT       ((u32)mh_init)
#define MH_FN_DEINIT     ((u32)mh_deinit)
#define MH_FN_UPDATE     ((u32)mh_update)
#define MH_FN_ROW        ((u32)mh_row)
#define MH_FN_MAP_INIT   ((u32)mh_map_init)
#define MH_FN_MAP_DEINIT ((u32)mh_map_deinit)
#define MH_FN_MAP_UPDATE ((u32)mh_map_update)
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
typedef void (*fn_v_uuu)(u32, u32, u32);
typedef void (*fn_v_uff)(u32, float, float);
typedef void (*fn_v_uufff)(u32, u32, float, float, float);
typedef void (*fn_rrect)(u32, u32, float, float, float, float, float, float);
typedef void (*fn_rect)(u32, u32, float, float, float, float, u32);
typedef int  (*fn_i_uuuuu)(u32, u32, u32, u32, u32);
typedef int  (*fn_i_i)(int);

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
#define title_fit       GAME_FN(fn_v_uuu,  OPD_TITLE_FIT,       mock_title_fit)
#define grid_rules      GAME_FN(fn_v_uufff, OPD_GRID_RULES,     mock_grid_rules)
#define grid_font       GAME_FN(fn_v_uuu,  OPD_GRID_FONT,       mock_grid_font)
#define set_scale       GAME_FN(fn_v_uff,  OPD_SET_SCALE,       mock_set_scale)
#define set_pos         GAME_FN(fn_v_uff,  OPD_SET_POS,         mock_set_pos)
#define set_visible     GAME_FN(fn_v_uu,   OPD_SET_VISIBLE,     mock_set_visible)
#define set_color       GAME_FN(fn_v_uu,   OPD_SET_COLOR,       mock_set_color)
#define set_anchor      GAME_FN(fn_v_uuu,  OPD_SET_ANCHOR,      mock_set_anchor)
#define set_layer       GAME_FN(fn_v_uu,   OPD_SET_LAYER,       mock_set_layer)
#define container       GAME_FN(fn_v_u,    OPD_CONTAINER,       mock_container)
#define add_frame       GAME_FN(fn_v_uu,   OPD_ADD_FRAME,       mock_add_frame)
#define round_rect      GAME_FN(fn_rrect,  OPD_ROUND_RECT,      mock_round_rect)
#define rect            GAME_FN(fn_rect,   OPD_RECT,            mock_rect)
#define text_box        GAME_FN(fn_v_uff,  OPD_TEXT_BOX,        mock_text_box)
#define text_fit        GAME_FN(fn_v_uu,   OPD_TEXT_FIT,        mock_text_fit)
#define pic_buffer      GAME_FN(fn_u_i,    OPD_PIC_BUFFER,      mock_pic_buffer)
#define pic_buffer_drop GAME_FN(fn_i_u,    OPD_PIC_BUFFER_DROP, mock_pic_buffer_drop)
#define pic_source      GAME_FN(fn_u_i,    OPD_PIC_SOURCE,      mock_pic_source)
#define pic_source_set  GAME_FN(fn_i_uuuuu, OPD_PIC_SOURCE_SET, mock_pic_source_set)
#define pic_source_stop GAME_FN(fn_i_u,    OPD_PIC_SOURCE_STOP, mock_pic_source_stop)
#define pic_tie         GAME_FN(fn_v_uu,   OPD_PIC_TIE,         mock_pic_tie)
#define pic_show        GAME_FN(fn_v_uu,   OPD_PIC_SHOW,        mock_pic_show)
#define pic_fade        GAME_FN(fn_v_uii,  OPD_PIC_FADE,        mock_pic_fade)
#define disc_wait       GAME_FN(fn_i_i,    OPD_DISC_WAIT,       mock_disc_wait)
#define map_init        GAME_FN(fn_v_u,    OPD_MAP_INIT,        mock_map_init)
#define map_update      GAME_FN(fn_v_u,    OPD_MAP_UPDATE,      mock_map_update)
#define map_deinit      GAME_FN(fn_v_u,    OPD_MAP_DEINIT,      mock_map_deinit)

/*
 * AUDIO / VISUAL's grid configuration, as its list builds it on the stack
 * (0x2DC6F0): the rows at 0.445, 0.0532 apart, in two columns at 0.115 and
 * 0.885, the labels in a box 0.53 wide and the values in one 0.24 wide, each
 * cell's text shrinking to fit its box (no least scale). The cell buffer, its
 * size and the rows are the shim's.
 */
static const u32 cfg_template[24] = {
	U_CELLS_SIZE, 0, U_GRID_LAYER, 0, 0, U_GRID_COLS, 0x3F800000, AV_ROW_Y0,
	AV_ROW_PITCH, AV_COL0_X, AV_COL1_X, 0x3F800000, 0x3F800000, AV_COL0_W, AV_COL1_W, 0,
	0, 0, 0, 0, 0, 0, 0, 0
};

/* A float from its bits, which is how the header gives AUDIO / VISUAL's numbers. */
static float fb(u32 bits)
{
	union { u32 u; float f; } x;

	x.u = bits;
	return x.f;
}

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

/* A GuiState {state; init, deinit, update, pre, post}, stopped, as 0x930C8 makes one. */
static u32 gs_fill(u32 g, u32 init, u32 deinit, u32 update, u32 pre, u32 post)
{
	w32(g, 0);
	w32(g + 4, init);
	w32(g + 8, deinit);
	w32(g + 12, update);
	w32(g + 16, pre);
	w32(g + 20, post);
	return g;
}

/*
 * GuiState k made ready to start: stopped, its calls the shim's, its renders
 * the pause screens' own. The thread runner calls its init once SwapTo makes it
 * the next.
 */
static u32 gs_ready(u32 k)
{
	w8(SH(SH_CUR), k);
	return gs_fill(GSA(k), MH_FN_INIT, MH_FN_DEINIT, MH_FN_UPDATE, U_DESC_PRE, U_DESC_POST);
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
 * as the game's bands for eight and nine rows are (0xC529D0, 0xC529D8). The
 * game's rows are short words; one of the menu's wider than the frame shrinks
 * to fit it, as the grid's cells do, down to 0.6 of its size.
 */
static void main_start(u32 gs)
{
	u32 n = mh_nrows(), i;

	main_rows();
	w32(U_CURSOR, mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
	wf32(SH(SH_BAND), 0.40f - 0.03f * (float)n);
	wf32(SH(SH_BAND) + 4, 0.61f + 0.03f * (float)n);
	w32(SH(SH_LIST_COUNT), n);
	w32(SH(SH_LIST_TABLE), ROWS);
	main_init(gs);
	w32(SH(SH_LIST_TABLE), 0);
	for (i = 0; i < n; i++) {
		text_box(U_ROW_HANDLE + i, fb(U_ROW_FIT_W), fb(U_ROW_FIT_MIN));
		text_fit(U_ROW_HANDLE + i, 1);
	}
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

/*
 * Row i's slider: 1, and where its knob goes from 0 (the bottom) to 1, when
 * the core marks the row as one that suits a slider.
 */
static int slider_at(u32 i, float *at)
{
#ifdef MROWF_SLIDER
	u32 a, off, span;

	if (i >= mh_nrows() || mh_row_kind(i) != MROW_VALUE ||
	    (mh_row_flags(i) & (MROWF_RANGE | MROWF_SLIDER)) != (MROWF_RANGE | MROWF_SLIDER))
		return 0;
	a = mh_row_range(i);
	off = r32(a) - r32(a + 4);
	span = r32(a + 8) - r32(a + 4);
	if (off <= span) *at = span == 0 ? 0.0f : (float)off / (float)span;
	/* Off the bar: at the end it is nearer, past the highest or under the lowest. */
	else *at = r32(a) - r32(a + 8) < r32(a + 4) - r32(a) ? 1.0f : 0.0f;
	return 1;
#else
	(void)i;
	(void)at;
	return 0;
#endif
}

/*
 * A grid cell's text: char[64] at its start, which the grid hands SetText
 * every frame. From its `n`th character: the game string at `s` (none for 0),
 * then `tail`. Answers where the text ends.
 */
static u32 cell_put(u32 c, u32 n, u32 s, const char *tail)
{
	u32 i = 0, ch;

	while (s != 0 && n < U_CELL_SIZE - 6) {
		ch = r8(s + i);
		if (ch == 0) break;
		w8(c + n, ch);
		n++;
		i++;
	}
	while (*tail != 0 && n < U_CELL_SIZE - 5) {
		w8(c + n, (u8)*tail);
		tail++;
		n++;
	}
	w8(c + n, 0);
	return n;
}

/*
 * The rows into the grid's cells, as AUDIO / VISUAL fills its own (0x2DC0B0):
 * the label left and the value right; a slider's value cell a blank, as that
 * screen's, the bar there, and its number after the label ("CHALLENGE MODE
 * 51"), which a trainer's slider owes its user and the label's box has room
 * for. Every row in the list's own colour but one the cursor may not land on,
 * grey (round 4: round 3 drew a row that was on white, and the user would
 * rather not; round 4 has no colon either).
 */
static void cfg_fill(u32 g)
{
	u32 n = mh_nrows(), i, f, c;
	float at;

	for (i = 0; i < n; i++) {
		c = grid_cell(g, 0, (int)i);
		if (slider_at(i, &at)) {
			cell_put(c, cell_put(c, 0, mh_label(i), " "), mh_value(i), "");
			cell_put(grid_cell(g, 1, (int)i), 0, 0, " ");
		} else {
			cell_put(c, 0, mh_label(i), "");
			cell_put(grid_cell(g, 1, (int)i), 0, mh_value(i), "");
		}
		f = mh_row_flags(i);
		grid_color(g, (int)i, (f & MROWF_ENABLED) == 0 ? U_COLOR_SKIP : U_COLOR_OFF);
	}
}

/*
 * Round 4: the picture source, as AUDIO / VISUAL's init sets its own up
 * before it builds anything (0x2DD320 .. 0x2DD370): two of the pause screens'
 * picture buffers, and source 1 reading AUDIO / VISUAL's set out of the
 * hud.wad. The game takes neither answer back; the shim does, and without
 * both buffers or with the source busy it shows no picture, the buffers given
 * back.
 */
static void pic_open(void)
{
	u32 a = pic_buffer(0), b = pic_buffer(0);

	w8(SH(SH_PIC_ON), 0);
	w8(SH(SH_PIC_WAIT), 0);
	w32(SH(SH_PIC_BUF), a);
	w32(SH(SH_PIC_BUF) + 4, b);
	if (a != 0 && b != 0 && pic_source_set(pic_source(U_PIC_SOURCE), a, b, r32(U_PIC_HUD_WAD), U_PIC_SET) != 0) {
		w8(SH(SH_PIC_ON), 1);
		return;
	}
	if (a != 0) pic_buffer_drop(a);
	if (b != 0) pic_buffer_drop(b);
	w32(SH(SH_PIC_BUF), 0);
	w32(SH(SH_PIC_BUF) + 4, 0);
}

/*
 * The picture panel above the list, as AUDIO / VISUAL's widget table builds it
 * (0xC4FC60 through 0x2DCE60): the root on layer 17 with the panel's container
 * and its rounded box, and in it the picture, a rectangle (round 3 had the
 * description line there).
 */
static void cfg_panel(void)
{
	container(AV_ROOT);
	container(AV_PANEL);
	add_frame(AV_ROOT, AV_PANEL);
	round_rect(AV_PANEL_BOX, AV_PANEL_COLOR, fb(AV_FRAME_X), fb(AV_PANEL_Y), fb(AV_PANEL_W), fb(AV_PANEL_H),
	           fb(AV_PANEL_R), fb(AV_PANEL_R));
	add_frame(AV_PANEL, AV_PANEL_BOX);
	rect(AV_PIC, AV_PIC_COLOR, fb(AV_PIC_X), fb(AV_PIC_Y), fb(AV_PIC_W), fb(AV_PIC_H), 0);
	add_frame(AV_PANEL, AV_PIC);
	set_layer(U_LAYER_FRAME2, AV_ROOT);
}

/*
 * The end of AUDIO / VISUAL's init (0x2DD9B0 .. 0x2DD9F0): the rectangle tied
 * to the source with picture 0, hidden, fading in over ten frames; the
 * picture itself once the screen has been up as long as AUDIO / VISUAL lets
 * the cursor rest before it shows a row's (cfg_frame).
 */
static void pic_tie_up(void)
{
	if (r8(SH(SH_PIC_ON)) == 0) return;
	pic_tie(AV_PIC, pic_source(U_PIC_SOURCE));
	pic_show(AV_PIC, 0);
	set_visible(AV_PIC, 0);
	pic_fade(AV_PIC, 1, 10);
	w8(SH(SH_PIC_WAIT), U_PIC_WAIT);
}

/* The picture, U_PIC_WAIT frames in, as 0x2DC0B0 shows a row's (0x2DC664). */
static void pic_frame(void)
{
	u32 t = r8(SH(SH_PIC_WAIT));

	if (t == 0) return;
	w8(SH(SH_PIC_WAIT), t - 1);
	if (t != 1) return;
	pic_show(AV_PIC, U_PICTURE);
	set_visible(AV_PIC, 1);
}

/*
 * As AUDIO / VISUAL's deinit (0x2DE1D8) after its layers: the source stopped
 * (its vtable's second entry), the buffers given back, and the disc's reads
 * waited for, so none lands in a buffer another screen has taken.
 */
static void pic_close(void)
{
	if (r8(SH(SH_PIC_ON)) == 0) return;
	pic_source_stop(pic_source(U_PIC_SOURCE));
	pic_buffer_drop(r32(SH(SH_PIC_BUF)));
	pic_buffer_drop(r32(SH(SH_PIC_BUF) + 4));
	w32(SH(SH_PIC_BUF), 0);
	w32(SH(SH_PIC_BUF) + 4, 0);
	w8(SH(SH_PIC_ON), 0);
	w8(SH(SH_PIC_WAIT), 0);
	disc_wait(1);
}

/* The list's slots on screen, top to bottom: their y. */
static float slot_y(u32 s)
{
	return fb(AV_ROW_Y0) + fb(AV_ROW_PITCH) * (float)s;
}

/*
 * A page with a slider gets AUDIO / VISUAL's slider widgets (0x2DD218) for each
 * row the list shows at once: the groove, the track and the knob, in a
 * container under the root, hidden until a slider's row is in that slot.
 */
static void cfg_sliders(u32 vis)
{
	u32 n = mh_nrows(), i, s, h;
	float at;

	w8(SH(SH_SLOTS), 0);
	for (i = 0; i < n && !slider_at(i, &at); i++) { }
	if (i == n) return;

	container(AV_SLIDERS);
	for (s = 0; s < vis; s++) {
		h = AV_SLOT0 + 3 * s;
		rect(h, AV_TRACK_COLOR, fb(AV_GROOVE_X), slot_y(s), fb(AV_GROOVE_W), fb(AV_GROOVE_H), 0);
		set_anchor(h, 1, 3);
		set_color(h, AV_GROOVE_COLOR2);
		round_rect(h + 1, AV_TRACK_COLOR, fb(AV_TRACK_X), slot_y(s), fb(AV_TRACK_W), fb(AV_TRACK_H),
		           fb(AV_ROUND_A), fb(AV_ROUND_B));
		set_color(h + 1, AV_TRACK_COLOR2);
		round_rect(h + 2, AV_KNOB_COLOR, fb(AV_KNOB_LEFT), slot_y(s), fb(AV_KNOB_W), fb(AV_KNOB_H),
		           fb(AV_ROUND_A), fb(AV_ROUND_B));
		add_frame(AV_SLIDERS, h);
		add_frame(AV_SLIDERS, h + 1);
		add_frame(AV_SLIDERS, h + 2);
		set_visible(h, 0);
		set_visible(h + 1, 0);
		set_visible(h + 2, 0);
		w8(SH(SH_SLOT_ROW) + s, MH_NONE);
	}
	add_frame(AV_ROOT, AV_SLIDERS);
	w8(SH(SH_SLOTS), vis);
}

/*
 * The sliders follow the list: each slot shows the slider of the row the grid
 * has in it, its knob where the value is, and none while the list scrolls (the
 * rows slide then; the sliders come back where the rows stop).
 */
static void cfg_slide(u32 g)
{
	u32 slots = r8(SH(SH_SLOTS)), n = mh_nrows(), s, r, want, h;
	int moving;
	float at = 0.0f, x;

	if (slots == 0) return;
	moving = r32(g + U_GRID_SCROLL) != 0 || (r32(g + U_GRID_SLIDE) & 0x7FFFFFFFu) != 0;
	for (s = 0; s < slots; s++) {
		r = r32(g + U_GRID_TOP) + s;
		want = !moving && r < n && slider_at(r, &at) ? r : MH_NONE;
		h = AV_SLOT0 + 3 * s;
		if (want != r8(SH(SH_SLOT_ROW) + s)) {
			set_visible(h, want != MH_NONE);
			set_visible(h + 1, want != MH_NONE);
			set_visible(h + 2, want != MH_NONE);
			w8(SH(SH_SLOT_ROW) + s, want);
			wf32(SH(SH_SLOT_KNOB) + 4 * s, -1.0f);
		}
		if (want == MH_NONE) continue;
		x = fb(AV_KNOB_LEFT) + fb(AV_KNOB_SPAN) * at;
		if (x != rf32(SH(SH_SLOT_KNOB) + 4 * s)) {
			set_pos(h + 2, x, slot_y(s));
			wf32(SH(SH_SLOT_KNOB) + 4 * s, x);
		}
	}
}

/*
 * AUDIO / VISUAL's screen with the core's page (its init 0x2DD218): the
 * picture source, the frame with the page's title, its banner and the title's
 * scale fitted to it as Options' screens fit theirs (0x2D4DB0), the picture
 * panel, the grid built for the page's rows with eight on screen, AUDIO /
 * VISUAL's frame, cursor bar and rules, the cells, labels left and values
 * right, both in the Options screens' font, the sliders, the cursor on its
 * row, and the picture tied to its source.
 */
static void cfg_start(void)
{
	u32 n = mh_nrows(), vis = n < U_GRID_VISIBLE ? n : U_GRID_VISIBLE, g, i;

	pic_open();
	wf32(SH(SH_TITLE_SCALE), 1.0f);
	wf32(SH(SH_TITLE_BANNER), 8.5f);
	title_fit(mh_title(), SH(SH_TITLE_SCALE), SH(SH_TITLE_BANNER));
	frame(U_LAYER_FRAME, U_FRAME_CONTAINER, 0x000D0000, 0x000D0001, 0x000D0002, 0x000D0003, 0x000D0004,
	      U_TITLE_HANDLE, 0x000D0008, mh_title(), 0x000D0006,
	      r8(U_JAPANESE) != 0 ? U_MSG_SELECT_JP : U_MSG_SELECT, 0x000D0007, U_MSG_BACK,
	      rf32(SH(SH_TITLE_BANNER)));
	set_scale(U_TITLE_HANDLE, rf32(SH(SH_TITLE_SCALE)), rf32(SH(SH_TITLE_SCALE)));
	cfg_panel();

	for (i = 0; i < 24; i++) w32(CFG + 4 * i, cfg_template[i]);
	w32(CFG + 4, CELLS);
	w32(CFG + 12, n);
	w32(CFG + 16, vis);
	g = get_grid(0);
	grid_build(g, CFG);
	frame_rect(g, fb(AV_FRAME_X), fb(AV_FRAME_W), fb(AV_FRAME_H), fb(AV_FRAME_F4), fb(AV_FRAME_F5));
	cursor_bar(g, fb(AV_FRAME_X), fb(AV_BAR_Y), fb(AV_FRAME_W), fb(AV_BAR_H));
	grid_rules(g, 1, fb(AV_RULE_Y), fb(AV_RULE_H), fb(AV_RULE_INSET));
	grid_align(g, 0, 1, 3);
	grid_align(g, 1, 2, 3);
	cfg_fill(g);
	grid_font(g, 0, U_FONT_MENU);
	grid_font(g, 1, U_FONT_MENU);
	cfg_sliders(vis);
	grid_set_cursor(g, (int)mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
	pic_tie_up();
}

/*
 * A CONFIG page's frame, as AUDIO / VISUAL's update (0x2DDA54): the core's,
 * the grid (Up, Down, the scroll, their sound, the cells pushed to the
 * widgets), the sliders after it, one press on the grid's row, then the Start
 * close.
 */
static void cfg_frame(u32 gs)
{
	u32 g, e, b = 0;

	if (mh_core_frame(0) == MH_GONE) return;
	e = edges();
	g = get_grid(0);
	grid_update(g, (int)e);
	cfg_slide(g);
	pic_frame();

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
 * change sound, but a slider's step and a window moving, which are the
 * cursor's tick, as AUDIO / VISUAL's sliders tick (0x2DE508).
 */
MH_API void shim_sound(u32 snd)
{
	float at;

	if (snd == MH_SND_FORWARD && shown() != MBOX_PTYPE_CONFIG) return;
	if (snd == MH_SND_STEP && shown() == MBOX_PTYPE_CONFIG && slider_at(shim_cursor(0), &at)) snd = MH_SND_MOVE;
	play_sound(snd == MH_SND_MOVE ? SND_MOVE : SND_CHANGE, 0, 0);
}

/* ------------------------------------------------------------ the entries */

/*
 * The menu's GuiState starts: the page qwark posted, with the widget it asks
 * for. Back from the GALACTIC MAP's Square, the cross on Planets, which the
 * picker answers this once with MH_PICK_OTHERS: qwark's page of the vid
 * comics (the pickers' round 2).
 */
MH_API void mh_init(u32 gs)
{
	arrive();
	if (mh_core_start(0) == MBOX_PTYPE_CONFIG) cfg_start();
	else main_start(gs);
	if (r8(SH(SH_MAP_LIST)) != 0) {
		mh_core_press(0, MH_BTN_CROSS, shim_cursor(0));
		w8(SH(SH_MAP_LIST), 0);
	}
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

/*
 * It stops: the widget's own end, as AUDIO / VISUAL's deinit (0x2DE1D8: layers
 * 16, 17 and the grid's 20, which take the panel, the picture and the sliders
 * with them, then the picture source) and the main pause's, and the cursor
 * back.
 */
MH_API void mh_deinit(u32 gs)
{
	u32 was = shown();

	mh_core_stop(0);
	if (was == MBOX_PTYPE_CONFIG) {
		clear_layer(U_LAYER_FRAME);
		clear_layer(U_LAYER_FRAME2);
		grid_free(get_grid(0));
		pic_close();
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

/* ------------------------------------------------- the GALACTIC MAP (picker) */

/* SwapTo the next GuiState and reset this one, the way every pause screen moves on. */
static __attribute__((noinline)) void go(u32 next, u32 self)
{
	swap_to(next);
	reset_state(self);
}

/*
 * The cross on the Planets row (the core, MROWF_PICKER): the game's GALACTIC
 * MAP over the menu, on the GuiState the menu's screen is not on, with the
 * map's own init, deinit and update wrapped (mh_map_*) and its own renders. The
 * menu's screen stops as the runner starts the map (its deinit first, so a
 * CONFIG page's picture source and buffers, which the map takes too, are back
 * before the map's init asks for them); the core counts that stop no close.
 * Not with a Start close pending this frame, which the main pause's update
 * takes after the row's callback, nor in a pause whose Start does not leave it
 * (U_NO_EXIT): the core posts the cross then, and qwark shows its own list;
 * and once after the map's Square, which asks for qwark's page of the vid
 * comics instead (MH_PICK_OTHERS: the core posts MBOX_EV_PICKER_OTHERS).
 */
MH_API u32 shim_picker(int p)
{
	(void)p;
	if (r8(SH(SH_MAP_LIST)) != 0) {
		w8(SH(SH_MAP_LIST), 0);
		return MH_PICK_OTHERS;
	}
	if (r8(U_NO_EXIT) != 0 || start_close()) return MH_PICK_REFUSED;
	w8(SH(SH_MAP_BACK), 0);
	go(gs_fill(GSA(r8(SH(SH_CUR)) ^ 1), MH_FN_MAP_INIT, MH_FN_MAP_DEINIT, MH_FN_MAP_UPDATE,
	           U_DESC_MAP_PRE, U_DESC_MAP_POST), current());
	return MH_PICK_OPENED;
}

/*
 * The map's own init (0x2CED28), with the list build handed the Trainer's
 * planets (mh_map_list_stub, while SH_MAP is up) and every one of them named
 * as the client's planet picker names it (the pickers' round 2, the user's
 * call): while the init runs, both name words of each listed planet (its
 * location's and its planet's, whichever the build and the init read) say
 * MH_MSG_PLANET + p, which the msg hook answers with PLANET_LIST's name, and
 * the game's words go back as it returns. All of it on the game's thread,
 * inside this one call: the game's own map, and every other screen that names
 * a planet, never sees either. Square's hint, MISSIONS in the game's map,
 * says what Square does in this one: VID-COMICS, qwark's page of the vid
 * comics, which the map cannot show, in the hint's own glyph (0x13) and the
 * capitals of the map's other hints ("\x10 SELECT", "\x12 EXIT").
 */
MH_API void mh_map_init(u32 gs)
{
	u32 save[2 * MH_MAP_LIST_LEN], i, p, a;

	for (i = 0; i < MH_MAP_LIST_LEN; i++) {
		p = r8(MAP_LIST + i);
		if (p >= MH_MAP_PLANETS) continue;
		a = U_MAP_TEXTS + 8 * p;
		save[2 * i] = r32(a);
		save[2 * i + 1] = r32(a + 4);
		w32(a, MH_MSG_PLANET + p);
		w32(a + 4, MH_MSG_PLANET + p);
	}
	w8(SH(SH_MAP), 1);
	map_init(gs);
	w8(SH(SH_MAP), 0);
	for (i = MH_MAP_LIST_LEN; i-- > 0;) {
		p = r8(MAP_LIST + i);
		if (p >= MH_MAP_PLANETS) continue;
		a = U_MAP_TEXTS + 8 * p;
		w32(a, save[2 * i]);
		w32(a + 4, save[2 * i + 1]);
	}
	cell_put(SH(SH_MAP_HINT), 0, 0, "\x13 VID-COMICS");
	set_text(U_MAP_HINT, SH(SH_MAP_HINT));
}

/*
 * Every frame of the Trainer's map. What would leave it is the Trainer's, taken
 * before the map's own update could see it (each of its exits travels from
 * play: X the pick, Triangle and Start a reload of the planet, or Blackwater
 * City's and Aridia's BATTLE MISSIONS, Square MISSIONS and its travel):
 *   the Start close, or qwark's close request (the combo): out of the pause,
 *                     a close of the menu;
 *   the cross         the pick to qwark and the close (mh_core_pick), the
 *                     map's travel sound, and out of the pause plainly:
 *                     qwark loads it once the game plays;
 *   Triangle          back to the menu's screen, the page it left;
 *   Square            the same, and then the cross on Planets, which the
 *                     picker answers this once (SH_MAP_LIST) with
 *                     MH_PICK_OTHERS: qwark's page of the vid comics.
 * Anything else is the map's own update (the cursor, its views, the bolts).
 */
MH_API void mh_map_update(u32 gs)
{
	u32 e = edges(), c, snd = 0, next = 0;

	if (start_close() || mh_core_close_request()) {
		start_ack();
	} else if ((e & r32(U_CONFIRM_MASK)) != 0) {
		c = r32(U_MAP_CURSOR);
		if (c >= r32(U_MAP_COUNT)) return;
		mh_core_pick(0, (s32)r32(U_MAP_ENTRIES + c * U_MAP_ENTRY_SIZE));
		snd = SND_MAP_GO;
	} else if ((e & (PADB_TRIANGLE | PADB_SQUARE)) != 0) {
		if ((e & PADB_TRIANGLE) == 0) w8(SH(SH_MAP_LIST), 1);
		w8(SH(SH_MAP_BACK), 1);
		snd = SND_CHANGE;
		next = gs_ready(r8(SH(SH_CUR)));
	} else {
		map_update(gs);
		return;
	}
	if (snd != 0) play_sound(snd, 0, 0);
	go(next != 0 ? next : pause_exit(), gs);
}

/*
 * The map ends (0x2D0600): back to the menu, whose screen then starts on the
 * page it left; any other way (Start, the combo, something of the game's) the
 * menu has closed with it, as a pick closed it already.
 */
MH_API void mh_map_deinit(u32 gs)
{
	map_deinit(gs);
	if (r8(SH(SH_MAP_BACK)) == 0) mh_core_picker_gone(0);
	w8(SH(SH_MAP_BACK), 0);
}
