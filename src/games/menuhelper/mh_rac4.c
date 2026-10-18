/*
 * Deadlocked's console menu shim: qwark's pages on the TV with the game's own
 * pause-menu widgets. mh_rac4.h says where it lives, what it hooks and what
 * it calls; mh_shim.h is the interface to the core (mh_core.c), which reads
 * the mailbox, composes the text and posts the events. This file only talks
 * to the game:
 *
 *   MAIN    a page of links and buttons, in GuiSupport's button list, the one
 *           Options uses: MenuButton_s rows whose text is the core's;
 *   CONFIG  a page of labels and controls, in a CtlList on the Cheats screen's
 *           templates, run by a GuiState thread of the shim's own the way the
 *           Cheats screen runs one (MenuCheats_* in the PS2 v6 symbols).
 *
 * The game reaches it through
 *   mh_inject_stub, mh_screen_stub  GuiHud's Start path (assembler), the combo:
 *                    qwark asks, the stubs press Start and open screen 36;
 *   mh_help_label    the pause main menu's Help row, whose label it gives;
 *                    the row's own callback pushes screen 36 instead of Help's
 *                    (a word of the install, mh_rac4.h);
 *   mh_frame_stub    UpdateMenus' input call, which calls mh_frame;
 *   mh_start, mh_stop  screen 36's fcStart and fcStop, through descriptors;
 *   mh_row, mh_close  the callbacks a MAIN page's rows and list carry;
 *   mh_cfg_*         the CONFIG page's thread, its row callback and validator.
 *
 * Everything the game keeps a pointer to - the rows, their text, the title, the
 * thread, the list - is in the renderer's state, which stays put for as long
 * as the game runs and changes only on the game's thread. Every access to game
 * memory goes through r8..w32 (mh_common.h), so the host tests run this file
 * on qwark's fake console with the real bytes.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define SH(o)    ((u32)MH_SHIM + (u32)(o))
#define LIST     SH(SH_LIST)
#define ITEMS    SH(SH_ITEMS)
#define GS       SH(SH_GS)

/* The shim's state starts where the core's ends. */
typedef char mh_rac4_layout[(MH_STATE + MH_CORE_SIZE <= MH_SHIM &&
                             MH_SHIM + SH_SIZE <= MH_DATA_END) ? 1 : -1];

void mh_row(int arg);
void mh_close(int arg);
void mh_start(int p);
void mh_stop(int p);
int  mh_frame(int p);
void mh_cfg_init(u32 gs);
void mh_cfg_deinit(u32 gs);
void mh_cfg_update(u32 gs);
void mh_cfg_item(u32 list, u32 item, int index);
int  mh_cfg_valid(int index);

/* The descriptors of the shim's own callbacks, as the game will hold them. */
#ifdef MH_HOST
#define MH_FN_ROW        1u
#define MH_FN_CLOSE      2u
#define MH_FN_CFG_INIT   3u
#define MH_FN_CFG_DEINIT 4u
#define MH_FN_CFG_UPDATE 5u
#define MH_FN_CFG_ITEM   6u
#define MH_FN_CFG_VALID  7u
#else
#define MH_FN_ROW        ((u32)mh_row)
#define MH_FN_CLOSE      ((u32)mh_close)
#define MH_FN_CFG_INIT   ((u32)mh_cfg_init)
#define MH_FN_CFG_DEINIT ((u32)mh_cfg_deinit)
#define MH_FN_CFG_UPDATE ((u32)mh_cfg_update)
#define MH_FN_CFG_ITEM   ((u32)mh_cfg_item)
#define MH_FN_CFG_VALID  ((u32)mh_cfg_valid)
#endif

typedef int  (*fn_i_v)(void);
typedef int  (*fn_i_i)(int);
typedef int  (*fn_i_ii)(int, int);
typedef int  (*fn_i_u)(u32);
typedef u32  (*fn_u_i)(int);
typedef u32  (*fn_u_ui)(u32, int);
typedef void (*fn_v_v)(void);
typedef void (*fn_v_i)(int);
typedef void (*fn_v_u)(u32);
typedef void (*fn_v_ii)(int, int);
typedef void (*fn_v_iu)(int, u32);
typedef void (*fn_v_uu)(u32, u32);
typedef void (*fn_v_ui)(u32, int);
typedef void (*fn_v_iiu)(int, int, u32);
typedef void (*fn_v_uii)(u32, int, int);
typedef void (*fn_v_uui)(u32, u32, int);
typedef void (*fn_v_ufi)(u32, float, int);
typedef void (*fn_v_iiuu)(int, int, u32, u32);
typedef void (*fn_v_iiuui)(int, int, u32, u32, int);
typedef void (*fn_v_iiui)(int, int, u32, int);
typedef void (*fn_v_uuiuii)(u32, u32, int, u32, int, int);
typedef void (*fn_v_iffff)(int, float, float, float, float);
typedef void (*fn_v_iiffffi)(int, int, float, float, float, float, int);
typedef void (*fn_v_uffff)(u32, float *, float *, float *, float *);

#define get_canvas     GAME_FN(fn_i_v,      OPD_GET_CANVAS,    mock_get_canvas)
#define set_canvas     GAME_FN(fn_v_i,      OPD_SET_CANVAS,    mock_set_canvas)
#define get_cursor     GAME_FN(fn_i_i,      OPD_GET_CURSOR,    mock_get_cursor)
#define get_screen     GAME_FN(fn_i_i,      OPD_GET_SCREEN,    mock_get_screen)
#define set_cursor     GAME_FN(fn_v_ii,     OPD_SET_CURSOR,    mock_set_cursor)
#define set_title      GAME_FN(fn_v_iu,     OPD_SET_TITLE,     mock_set_title)
#define screen_mode    GAME_FN(fn_i_i,      OPD_SCREEN_MODE,   mock_screen_mode)
#define set_target     GAME_FN(fn_v_iffff,  OPD_SET_TARGET,    mock_set_target)
#define set_header     GAME_FN(fn_v_ii,     OPD_SET_HEADER,    mock_set_header)
#define set_buttons    GAME_FN(fn_v_iiu,    OPD_SET_BUTTONS,   mock_set_buttons)
#define show_menu      GAME_FN(fn_v_iu,     OPD_SHOW_MENU,     mock_show_menu)
#define select_row     GAME_FN(fn_v_v,      OPD_SELECT,        mock_select_row)
#define takes_input    GAME_FN(fn_i_ii,     OPD_TAKES_INPUT,   mock_takes_input)
#define play_sound     GAME_FN(fn_v_i,      OPD_PLAY_SOUND,    mock_play_sound)
#define go_back        GAME_FN(fn_v_i,      OPD_GO_BACK_CB,    mock_go_back)
#define menu_stop      GAME_FN(fn_v_i,      OPD_MENU_STOP,     mock_menu_stop)
#define go_to_screen   GAME_FN(fn_v_ii,     OPD_GO_TO_SCREEN,  mock_go_to_screen)
#define start_next     GAME_FN(fn_v_i,      OPD_START_NEXT,    mock_start_next)
#define get_command    GAME_FN(fn_i_i,      OPD_GET_COMMAND,   mock_get_command)
#define base_frame     GAME_FN(fn_i_i,      OPD_BASE_FRAME,    mock_base_frame)
#define reset_state    GAME_FN(fn_v_u,      OPD_RESET_STATE,   mock_reset_state)
#define exit_state     GAME_FN(fn_v_u,      OPD_EXIT_STATE,    mock_exit_state)
#define start_thread   GAME_FN(fn_i_u,      OPD_START_THREAD,  mock_start_thread)
#define panel_create   GAME_FN(fn_v_uii,    OPD_PANEL_CREATE,  mock_panel_create)
#define panel_frame    GAME_FN(fn_v_ufi,    OPD_PANEL_FRAME,   mock_panel_frame)
#define canvas_layer   GAME_FN(fn_v_iu,     OPD_CANVAS_LAYER,  mock_canvas_layer)
#define clear_layer    GAME_FN(fn_v_i,      OPD_CLEAR_LAYER,   mock_clear_layer)
#define add_frame      GAME_FN(fn_v_uu,     OPD_ADD_FRAME,     mock_add_frame)
#define set_text       GAME_FN(fn_v_uu,     OPD_SET_TEXT,      mock_set_text)
#define set_color      GAME_FN(fn_v_uu,     OPD_SET_COLOR,     mock_set_color)
#define set_texture    GAME_FN(fn_v_ui,     OPD_SET_TEXTURE,   mock_set_texture)
#define rect_source    GAME_FN(fn_v_uu,     OPD_RECT_SOURCE,   mock_rect_source)
#define msg_string     GAME_FN(fn_u_i,      OPD_MSG_STRING,    mock_msg_string)
#define image_get      GAME_FN(fn_u_ui,     OPD_IMAGE_GET,     mock_image_get)
#define image_drop     GAME_FN(fn_v_u,      OPD_IMAGE_DROP,    mock_image_drop)
#define list_create    GAME_FN(fn_v_uuiuii, OPD_LIST_CREATE,   mock_list_create)
#define list_clear     GAME_FN(fn_v_u,      OPD_LIST_CLEAR,    mock_list_clear)
#define list_focus     GAME_FN(fn_v_ui,     OPD_LIST_FOCUS,    mock_list_focus)
#define list_next      GAME_FN(fn_v_u,      OPD_LIST_NEXT,     mock_list_next)
#define list_prev      GAME_FN(fn_v_u,      OPD_LIST_PREV,     mock_list_prev)
#define list_rect      GAME_FN(fn_v_uffff,  OPD_LIST_RECT,     mock_list_rect)
#define list_insert    GAME_FN(fn_v_uui,    OPD_LIST_INSERT,   mock_list_insert)
#define list_update    GAME_FN(fn_i_u,      OPD_LIST_UPDATE,   mock_list_update)
#define list_validator GAME_FN(fn_v_uu,     OPD_LIST_VALID,    mock_list_validator)
#define box_target     GAME_FN(fn_v_uui,    OPD_BOX_TARGET,    mock_box_target)
#define box_shown      GAME_FN(fn_v_ui,     OPD_BOX_SHOWN,     mock_box_shown)
#define frame_target   GAME_FN(fn_v_iiu,    OPD_FRAME_TARGET,  mock_frame_target)
#define show_frames    GAME_FN(fn_v_ii,     OPD_SHOW_FRAMES,   mock_show_frames)
#define attach_cursor  GAME_FN(fn_v_iiuui,  OPD_ATTACH_CURSOR, mock_attach_cursor)
#define show_cursors   GAME_FN(fn_v_ii,     OPD_SHOW_CURSORS,  mock_show_cursors)
#define detach_cursor  GAME_FN(fn_v_iiui,   OPD_DETACH_CURSOR, mock_detach_cursor)
#define cursor_target  GAME_FN(fn_v_iiffffi, OPD_CURSOR_TARGET, mock_cursor_target)

static u32 shown(void)
{
	return r8(MH_STATE + ST_SHOWN);
}

static u32 panel(void)
{
	return r32(SH(SH_PANEL));
}

/* ------------------------------------------------------- the MAIN widget */

/*
 * The core's rows as GuiSupport's buttons: MenuButton_s is {u8 enabled; char
 * *label; cb}, and a row the cursor cannot land on carries no callback.
 */
static void main_buttons(void)
{
	u32 n = mh_nrows(), i, b, on;

	for (i = 0; i < n; i++) {
		b = SH(SH_BUTTONS) + i * MH_BUTTON_SIZE;
		on = (mh_row_flags(i) & MROWF_ENABLED) != 0;
		w32(b, on ? 0x01000000u : 0);
		w32(b + 4, mh_text(i));
		w32(b + 8, on ? MH_FN_ROW : 0);
	}
}

/*
 * The list's place and size, MenuMain_vScreens' rectangle for the screen
 * mode. While the list is shown SetMenuTarget also lays the rows out again and
 * fits the frame round them (GetMenuRect, SetFrameTarget), the way the game
 * sizes the frame of each list it opens, so a page with fewer rows has a
 * smaller frame.
 */
static void main_target(int p)
{
	int mode = screen_mode(p);
	u32 r;

	if (mode < 0 || mode > 4) mode = 0;
	r = DL_MENU_VSCREENS + 16u * (u32)mode;
	set_target(p, rf32(r), rf32(r + 4), rf32(r + 8), rf32(r + 12));
}

/* Screen 36 as MenuMain_StartOptions starts Options, with the core's page. */
static void main_start(int p)
{
	set_cursor(p, (int)mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
	set_title(p, mh_title());
	main_target(p);
	/* The game's own expression: the header unless this is multiplayer. */
	set_header(p, r32(DL_GAME_TYPE) != 1);
	main_buttons();
	set_buttons(p, (int)mh_nrows(), SH(SH_BUTTONS));
	show_menu(p, MH_FN_CLOSE);
	play_sound(SND_SCREEN);
}

/* ----------------------------------------------------- the CONFIG widget */

/*
 * The rows into the list: Clear, then Insert each one, as UpdateStatsList and
 * the Cheats screen fill theirs, and its two texts into the row's panel, which
 * Insert makes on a row's first use. The scroll offset survives the Clear.
 */
static void cfg_fill(u32 cursor)
{
	u32 n = mh_nrows(), i, row;

	list_clear(LIST);
	for (i = 0; i < n; i++) {
		list_insert(LIST, i, -1);
		row = r32(ITEMS + i * DL_ITEM_SIZE + DL_ITEM_PANEL);
		set_text(row + DL_ROW_LABEL, mh_label(i));
		set_text(row + DL_ROW_VALUE, mh_value(i));
	}
	list_focus(LIST, (int)cursor);
	w8(SH(SH_CFG_CURSOR), 1);
}

/* An image of the game's plates, `from`, onto element `el` of the screen. */
static void plate(u32 n, u32 from, u32 el)
{
	u32 img = image_get(from, -1);

	w32(SH(SH_IMAGES) + 4 * n, img);
	set_texture(panel() + el, 1);
	rect_source(panel() + el, img);
}

/* The thread, as MenuCheats_Start starts the Cheats screen's: GuiState.cpp's Init comes next frame. */
static void cfg_start(int p)
{
	int c;

	w32(GS, 0);
	w32(GS + 4, MH_FN_CFG_INIT);
	w32(GS + 8, MH_FN_CFG_DEINIT);
	w32(GS + 12, MH_FN_CFG_UPDATE);
	w32(GS + 16, 0);
	w32(GS + 20, 0);
	c = get_canvas();
	set_canvas(p);
	reset_state(GS);
	start_thread(GS);
	set_canvas(c);
	play_sound(SND_SCREEN);
}

/* The Cheats screen's CreateWidgets and Init, with the core's page. */
void mh_cfg_init(u32 gs)
{
	int p = get_canvas();
	u32 l = SH(SH_LEGEND), e = l + 40, s;

	(void)gs;
	panel_create(SH(SH_PANEL), DL_TPL_SCREEN, -1);
	canvas_layer(DL_LAYER, panel());
	panel_frame(SH(SH_PANEL), (float)base_frame(p), -1);

	/* "\x10 SELECT \x12 BACK", glyphs and all, as the Cheats screen writes it. */
	s = msg_string(DL_MSG_SELECT);
	while (r8(s) != 0 && l < e - 2) { w8(l, r8(s)); l++; s++; }
	w8(l++, ' ');
	s = msg_string(DL_MSG_BACK);
	while (r8(s) != 0 && l < e - 1) { w8(l, r8(s)); l++; s++; }
	w8(l, 0);

	w8(SH(SH_EMPTY), 0);
	set_text(panel() + DL_EL_LEGEND, SH(SH_LEGEND));
	set_text(panel() + DL_EL_HEAD_L, SH(SH_EMPTY));
	set_text(panel() + DL_EL_HEAD_R, SH(SH_EMPTY));
	set_text(panel() + DL_EL_TITLE, mh_title());
	w8(mh_desc(), 0);
	set_text(panel() + DL_EL_DESC, mh_desc());

	/* CtlList::CtlList writes the vtable and nothing else (mh_rac4.h). */
	w32(LIST, DL_CTLLIST_VTABLE);
	list_create(LIST, ITEMS, MH_CFG_ROWS, MH_FN_CFG_ITEM, DL_TPL_ROW, 1);
	add_frame(panel(), r32(LIST + DL_LIST_HANDLE));
	box_target(LIST, panel() + DL_EL_LIST, 1);
	box_shown(LIST, 1);
	list_validator(LIST, MH_FN_CFG_VALID);
	cfg_fill(mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));

	frame_target(p, 0, panel() + DL_EL_CURSOR);
	show_frames(p, 1);
	attach_cursor(p, 0, panel(), panel() + DL_EL_CURSOR, 0);
	show_cursors(p, 1);

	plate(0, DL_PLATE_TOP, DL_EL_PLATE_TOP);
	plate(1, DL_PLATE_BOTTOM, DL_EL_PLATE_BOTTOM);
	plate(2, DL_PLATE_FOOT, DL_EL_PLATE_FOOT);

	w8(SH(SH_CFG_STATE), 1);
}

/* The input, as MenuCheats_HandleInput reads it: one command a frame. */
static void cfg_input(int p)
{
	int cmd = get_command(6);
	u32 c = shim_cursor(p);

	switch (cmd) {
	case DL_CMD_CROSS:    mh_core_press(p, MH_BTN_CROSS, c); return;
	case DL_CMD_LEFT:     mh_core_press(p, MH_BTN_LEFT, c); return;
	case DL_CMD_RIGHT:    mh_core_press(p, MH_BTN_RIGHT, c); return;
	case DL_CMD_L1:       mh_core_press(p, MH_BTN_L1, c); return;
	case DL_CMD_R1:       mh_core_press(p, MH_BTN_R1, c); return;
	case DL_CMD_TRIANGLE: mh_core_press(p, MH_BTN_BACK, c); return;
	case DL_CMD_CLOSE:    mh_core_press(p, MH_BTN_CLOSE, c); return;
	case DL_CMD_UP:
	case DL_CMD_DOWN:
		if (mh_nrows() < 2) return;
		if (cmd == DL_CMD_UP) list_prev(LIST);
		else list_next(LIST);
		play_sound(SND_MOVE);
		w8(SH(SH_CFG_CURSOR), 1);
		return;
	default:
		return;
	}
}

/* Every frame of the thread: the Cheats screen's Update. */
void mh_cfg_update(u32 gs)
{
	int p = get_canvas();
	u32 st = r8(SH(SH_CFG_STATE));
	float x, y, w, h;

	if (st == 2) {
		exit_state(gs);
	} else if (st == 1) {
		w8(SH(SH_CFG_STATE), 0);
	} else if (mh_core_frame(p) != MH_GONE) {
		cfg_input(p);
	}

	list_update(LIST);
	if (r8(SH(SH_CFG_CURSOR)) != 0) {
		list_rect(LIST, &x, &y, &w, &h);
		cursor_target(p, 0, x, y, w, h, 0);
		w8(SH(SH_CFG_CURSOR), 0);
	}
}

/* The thread's end: the Cheats screen's Deinit, then the next screen. */
void mh_cfg_deinit(u32 gs)
{
	int p = get_canvas();
	u32 i, img;

	(void)gs;
	detach_cursor(p, 0, panel(), 0);
	clear_layer(DL_LAYER);
	for (i = 0; i < 3; i++) {
		img = r32(SH(SH_IMAGES) + 4 * i);
		if (img != 0) image_drop(img);
		w32(SH(SH_IMAGES) + 4 * i, 0);
	}
	start_next(p);
}

/* A visible row that changed: MenuCheats_UpdateCheatCB's colours, by the core's flags. */
void mh_cfg_item(u32 list, u32 item, int index)
{
	u32 row = r32(item + DL_ITEM_PANEL), f = mh_row_flags(index), c;

	if (index == (int)r32(list + DL_LIST_FOCUS)) c = DL_RGBA_FOCUS;
	else if ((f & MROWF_ENABLED) == 0) c = DL_RGBA_LOCKED;
	else if ((f & MROWF_ON) != 0) c = DL_RGBA_ACTIVE;
	else c = DL_RGBA_OFF;
	set_color(row + DL_ROW_LABEL, c);
	set_color(row + DL_ROW_VALUE, c);
}

/* The rows the cursor may land on. */
int mh_cfg_valid(int index)
{
	return index >= 0 && (u32)index < mh_nrows() && (mh_row_flags(index) & MROWF_ENABLED) != 0;
}

/* ------------------------------------------------------- the shim, for the core */

MH_API void shim_show(int p, u32 how)
{
	if (shown() == MBOX_PTYPE_CONFIG) {
		cfg_fill(mh_core_cursor(shim_cursor(p)));
		set_text(panel() + DL_EL_TITLE, mh_title());
		return;
	}
	/*
	 * A new page while the list is up: the rows handed over again and the
	 * highlight put back, as N0's toggle did; with another number of rows the
	 * list is laid out again and its frame fitted round them.
	 */
	set_cursor(p, (int)mh_core_cursor((u32)get_cursor(p)));
	set_title(p, mh_title());
	main_buttons();
	set_buttons(p, (int)mh_nrows(), SH(SH_BUTTONS));
	if ((how & MH_SHOW_ROWS) != 0) main_target(p);
	select_row();
}

/*
 * The other widget: GoToScreen(36) again stops this one (mh_stop) and the game
 * starts the screen anew (mh_start), at once for the list and once the thread
 * has ended for the CONFIG page. GoToScreen pushes nothing, so back goes where
 * it went before.
 */
MH_API void shim_switch(int p)
{
	go_to_screen(p, MH_SCREEN_ID);
}

/* Back from the root, or Start: Options' own callback (mh_rac4.h, OPD_GO_BACK_CB). */
MH_API void shim_close(int p, u32 how)
{
	(void)p;
	go_back(how == MH_CLOSE_BACK ? 0 : 1);
}

MH_API void shim_title(int p)
{
	if (shown() == MBOX_PTYPE_CONFIG) set_text(panel() + DL_EL_TITLE, mh_title());
	else set_title(p, mh_title());
}

MH_API void shim_desc(int p)
{
	(void)p;
	if (shown() == MBOX_PTYPE_CONFIG) set_text(panel() + DL_EL_DESC, mh_desc());
}

MH_API u32 shim_cursor(int p)
{
	int c;

	if (shown() == MBOX_PTYPE_CONFIG) c = (int)r32(LIST + DL_LIST_FOCUS);
	else c = get_cursor(p);
	return c < 0 ? 0 : (u32)c;
}

MH_API void shim_sound(u32 snd)
{
	static const u8 sounds[] = { SND_MOVE, SND_FORWARD, SND_BACK, SND_CHANGE };

	/* On the Cheats screen's page the cross changes a row too, with the same sound. */
	if (snd == MH_SND_FORWARD && shown() == MBOX_PTYPE_CONFIG) snd = MH_SND_STEP;
	if (snd < sizeof(sounds)) play_sound(sounds[snd]);
}

/* ------------------------------------------------------------ the entries */

/* Screen 36's fcStart: the page qwark posted, with the widget it asks for. */
void mh_start(int p)
{
	if (mh_core_start(p) == MBOX_PTYPE_CONFIG) cfg_start(p);
	else main_start(p);
}

/* Screen 36's fcStop: the list's own stop, or the thread told to end, as Cheats' Stop does. */
void mh_stop(int p)
{
	u32 was = shown();

	mh_core_stop(p);
	if (was == MBOX_PTYPE_CONFIG) {
		w8(SH(SH_CFG_STATE), 2);
		play_sound(SND_SCREEN_CLOSE);
	} else {
		menu_stop(p);
	}
}

/* A MAIN row: the cross on whatever the cursor is on. */
void mh_row(int arg)
{
	int p = get_canvas();

	(void)arg;
	mh_core_press(p, MH_BTN_CROSS, (u32)get_cursor(p));
}

/* The list's close callback: 1 for Start or Select, 0 for Triangle. */
void mh_close(int arg)
{
	int p = get_canvas();

	mh_core_press(p, arg != 0 ? MH_BTN_CLOSE : MH_BTN_BACK, (u32)get_cursor(p));
}

/*
 * Left, Right, L1 and R1, which the game's list ignores. They are read the way
 * GuiMain_GetMenuCommand reads them - player one's hudBitsOn, the presses of
 * this frame - and only when no command the game ranks higher (cross,
 * triangle, square, circle, up, down) is in the same frame, so the game still
 * gets every one of those. A press taken here is cleared, as the game clears
 * the ones it takes. Answers 1 when it took one.
 */
static int steps(int p)
{
	u32 pad, bits, button;

	if (p != 0) return 0;
	pad = r32(DL_PAD0_PTR);
	if (pad == 0) return 0;
	bits = r32(pad + DL_PAD_HUDBITS);
	if (bits == 0) return 0;
	if ((bits & (PADB_CROSS | PADB_TRIANGLE | PADB_SQUARE | PADB_CIRCLE |
	             PADB_UP | PADB_DOWN)) != 0)
		return 0;

	if ((bits & PADB_LEFT) != 0)       button = MH_BTN_LEFT;
	else if ((bits & PADB_RIGHT) != 0) button = MH_BTN_RIGHT;
	else if ((bits & PADB_L1) != 0)    button = MH_BTN_L1;
	else if ((bits & PADB_L2) != 0)    return 0;
	else if ((bits & PADB_R1) != 0)    button = MH_BTN_R1;
	else return 0;

	if (!takes_input(p, 6)) return 0;
	w32(pad + DL_PAD_HUDBITS, 0);
	mh_core_press(p, button, (u32)get_cursor(p));
	return 1;
}

/*
 * From mh_frame_stub, every frame any list menu is fully shown, in place of
 * GuiSupport_HandleMenuInput. Another screen's list answers 0 at once and the
 * stub goes on to the game's input. For screen 36's MAIN page the core takes
 * its frame, and the shim Left, Right, L1 and R1. Answers 1 when the game's
 * input is to be skipped this frame.
 */
int mh_frame(int p)
{
	if (get_screen(p) != MH_SCREEN_ID) return 0;
	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC || shown() != MBOX_PTYPE_MAIN) return 0;
	if (mh_core_frame(p) == MH_GONE) return 1;
	return steps(p);
}
