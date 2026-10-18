/*
 * Deadlocked's console menu renderer: qwark's current page, drawn with the
 * game's own pause-menu list. mh_rac4.h says where it lives and what it hooks;
 * src/core/menubox.h is the mailbox it shares with qwark; src/core/menu.c is
 * the other end.
 *
 * It is a renderer and nothing else. It never decides what a row does: it
 * shows the page qwark posted, moves the cursor the way the game moves it, and
 * posts what the player pressed on which row. qwark answers by posting the next
 * page. The game does the rest - the pause, the drawing, the sounds, gameplay
 * input held off - because screen 36 is one of its own menu screens.
 *
 * Built as the probes were, and N0 before it: ppu-lv2-gcc, a TOC of its own,
 * the game called through its own descriptors (src/games/menuhelper/build.sh).
 * The game reaches it through
 *   mh_options_stub  the Options hook, which calls mh_options_hook;
 *   mh_inject_stub, mh_screen_stub  GuiHud's Start path (assembler only);
 *   mh_frame_stub    UpdateMenus' input call, which calls mh_frame;
 *   mh_start, mh_stop  screen 36's fcStart and fcStop, through descriptors;
 *   mh_open, mh_row, mh_close  the callbacks the rows and the list carry.
 *
 * Everything the game keeps a pointer to - the rows, their text, the title -
 * lives in the renderer's state at MH_STATE, which stays put for as long as the
 * game runs, and only this code changes it, on the game's thread, just before it
 * hands the rows over again: GuiSupport's text widgets keep the char * they are
 * given and read it at every draw, and the mailbox is written by qwark from
 * another thread whenever it likes, so the game is never pointed into it. Every access to game memory goes through r8..w32 (mh_common.h), so
 * the host tests run this file on qwark's fake console with the real bytes.
 */
#include "mh_common.h"
#include "mh_rac4.h"
#include "../../core/menubox.h"

#define BOX(o)   ((u32)MH_DATA + (u32)(o))
#define ST(o)    ((u32)MH_STATE + (u32)(o))
/* A mailbox offset from MBOX_OFF_PAGE_KEY on, in the renderer's copy of the page. */
#define STAGE(o) (ST(ST_STAGE) + (u32)(o) - MBOX_OFF_PAGE_KEY)

void mh_row(int arg);
void mh_close(int arg);
void mh_open(int arg);
void mh_start(int p);
void mh_stop(int p);
int  mh_frame(int p);
int  mh_options_hook(int p, int n, u32 rows);

/* The descriptors of this renderer's own callbacks, as the game will hold them. */
#ifdef MH_HOST
#define MH_FN_ROW   1u
#define MH_FN_CLOSE 2u
#define MH_FN_OPEN  3u
#else
#define MH_FN_ROW   ((u32)mh_row)
#define MH_FN_CLOSE ((u32)mh_close)
#define MH_FN_OPEN  ((u32)mh_open)
#endif

typedef int  (*fn_i_v)(void);
typedef int  (*fn_i_i)(int);
typedef int  (*fn_i_ii)(int, int);
typedef void (*fn_v_v)(void);
typedef void (*fn_v_i)(int);
typedef void (*fn_v_ii)(int, int);
typedef void (*fn_v_iu)(int, u32);
typedef void (*fn_v_iiu)(int, int, u32);
typedef void (*fn_v_iffff)(int, float, float, float, float);

#define get_canvas     GAME_FN(fn_i_v,     OPD_GET_CANVAS,     mock_get_canvas)
#define get_cursor     GAME_FN(fn_i_i,     OPD_GET_CURSOR,     mock_get_cursor)
#define push_screen    GAME_FN(fn_v_ii,    OPD_PUSH_SCREEN,    mock_push_screen)
#define get_screen     GAME_FN(fn_i_i,     OPD_GET_SCREEN,     mock_get_screen)
#define set_cursor     GAME_FN(fn_v_ii,    OPD_SET_CURSOR,     mock_set_cursor)
#define set_title      GAME_FN(fn_v_iu,    OPD_SET_TITLE,      mock_set_title)
#define screen_mode    GAME_FN(fn_i_i,     OPD_SCREEN_MODE,    mock_screen_mode)
#define set_target     GAME_FN(fn_v_iffff, OPD_SET_TARGET,     mock_set_target)
#define set_header     GAME_FN(fn_v_ii,    OPD_SET_HEADER,     mock_set_header)
#define set_buttons    GAME_FN(fn_v_iiu,   OPD_SET_BUTTONS,    mock_set_buttons)
#define show_menu      GAME_FN(fn_v_iu,    OPD_SHOW_MENU,      mock_show_menu)
#define select_row     GAME_FN(fn_v_v,     OPD_SELECT,         mock_select_row)
#define update_widgets GAME_FN(fn_v_i,     OPD_UPDATE_WIDGETS, mock_update_widgets)
#define takes_input    GAME_FN(fn_i_ii,    OPD_TAKES_INPUT,    mock_takes_input)
#define play_sound     GAME_FN(fn_v_i,     OPD_PLAY_SOUND,     mock_play_sound)
#define go_back        GAME_FN(fn_v_i,     OPD_GO_BACK_CB,     mock_go_back)
#define menu_stop      GAME_FN(fn_v_i,     OPD_MENU_STOP,      mock_menu_stop)

/* ------------------------------------------------------------- small things */

static void inc(u32 off)
{
	w32(ST(off), r32(ST(off)) + 1);
}

/*
 * Game bytes from `src` (at most `len`, up to a NUL) to `dst`, never at or past
 * `end - 1`. Plain ASCII only: the game's font reads 0x10..0x1F as button glyphs
 * and a byte from 0x80 up as the first of a double-byte glyph, so anything
 * outside 0x20..0x7E shows as a question mark instead.
 */
static u32 put_mem(u32 dst, u32 end, u32 src, u32 len)
{
	u32 i;

	for (i = 0; i < len && dst < end - 1; i++) {
		u32 c = r8(src + i);
		if (c == 0) break;
		if (c < 0x20 || c > 0x7E) c = '?';
		w8(dst, c);
		dst++;
	}
	return dst;
}

/* The same from one of this file's own strings. */
static u32 put_lit(u32 dst, u32 end, const char *s)
{
	while (*s != 0 && dst < end - 1) {
		w8(dst, (u8)*s);
		dst++;
		s++;
	}
	return dst;
}

static u32 nrows(void)
{
	return r8(ST(ST_NROWS));
}

static u32 row_flags(u32 i)
{
	return r8(ST(ST_FLAGS) + i);
}

/* -------------------------------------------------------------- the page */

/*
 * Row i of the copied page as the list shows it: one text, the label and then
 * the value after a colon, the current option of a pick-one list marked with a
 * star. MenuButton_s is {u8 enabled; char *label; cb}; a row the cursor cannot
 * land on carries no callback.
 */
static void compose(u32 i)
{
	u32 src = STAGE(MBOX_OFF_ROWS) + i * MBOX_ROW_SIZE;
	u32 text = ST(ST_TEXT) + i * MH_TEXT_LEN;
	u32 end = text + MH_TEXT_LEN;
	u32 kind = r8(src + MROW_OFF_KIND);
	u32 flags = r8(src + MROW_OFF_FLAGS);
	u32 button = ST(ST_BUTTONS) + i * MH_BUTTON_SIZE;
	u32 p;

	p = put_mem(text, end, src + MROW_OFF_LABEL, MROW_LABEL_LEN);
	if (kind == MROW_OPTION && (flags & MROWF_ON) != 0) p = put_lit(p, end, " *");
	if (r8(src + MROW_OFF_VALUE) != 0) {
		p = put_lit(p, end, ": ");
		p = put_mem(p, end, src + MROW_OFF_VALUE, MROW_VALUE_LEN);
	}
	w8(p, 0);

	w8(ST(ST_KIND) + i, kind);
	w8(ST(ST_FLAGS) + i, flags);
	w16(ST(ST_ID) + 2 * i, r16(src + MROW_OFF_ID));

	w32(button, (flags & MROWF_ENABLED) != 0 ? 0x01000000u : 0);
	w32(button + 4, text);
	w32(button + 8, (flags & MROWF_ENABLED) != 0 ? MH_FN_ROW : 0);
}

/*
 * Takes the page qwark last posted, if there is one this renderer has not
 * shown: page_seq is a seqlock, odd while qwark is writing, so the page is
 * copied whole and kept only if page_seq read the same even number before and
 * after. A page caught half written is left for the next frame. Answers 1 when
 * a page was taken.
 */
static int take_page(void)
{
	u32 s1 = r32(BOX(MBOX_OFF_PAGE_SEQ));
	u32 n, len, i, key;

	if (s1 == 0 || (s1 & 1) != 0) return 0;
	if (r8(ST(ST_HAVE_PAGE)) != 0 && s1 == r32(ST(ST_SHOWN_SEQ))) return 0;

	mh_sync();
	n = r8(BOX(MBOX_OFF_NROWS));
	if (n > MH_ROWS) n = MH_ROWS;
	len = MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + n * MBOX_ROW_SIZE;
	for (i = 0; i < len; i += 4)
		w32(ST(ST_STAGE) + i, r32(BOX(MBOX_OFF_PAGE_KEY) + i));
	mh_sync();

	if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != s1) {
		inc(ST_RETRIES);
		return 0;
	}

	/* A page of no rows is none qwark posts; show one blank row rather than divide by 0. */
	if (n == 0) {
		n = 1;
		for (i = 0; i < MBOX_ROW_SIZE; i += 4) w32(STAGE(MBOX_OFF_ROWS) + i, 0);
	}

	key = r32(STAGE(MBOX_OFF_PAGE_KEY));
	w8(ST(ST_NEW_KEY), r8(ST(ST_HAVE_PAGE)) == 0 || key != r32(ST(ST_SHOWN_KEY)) ||
	                   r8(ST(ST_NEW_KEY)) != 0);
	w32(ST(ST_SHOWN_SEQ), s1);
	w32(ST(ST_SHOWN_KEY), key);
	w8(ST(ST_NROWS), n);
	w8(ST(ST_CURSOR_HINT), r8(STAGE(MBOX_OFF_CURSOR)));
	w8(ST(ST_PFLAGS), r8(STAGE(MBOX_OFF_PFLAGS)));

	w8(put_mem(ST(ST_TITLE), ST(ST_TITLE) + MBOX_TITLE_LEN, STAGE(MBOX_OFF_TITLE),
	           MBOX_TITLE_LEN), 0);
	for (i = 0; i < n; i++) compose(i);

	w8(ST(ST_HAVE_PAGE), 1);
	inc(ST_PAGES);
	return 1;
}

/* Before qwark has posted anything: one grey row, and Triangle leaves. */
static void placeholder(void)
{
	u32 text = ST(ST_TEXT);
	u32 button = ST(ST_BUTTONS);

	w8(put_lit(ST(ST_TITLE), ST(ST_TITLE) + MBOX_TITLE_LEN, "RaCMAN"), 0);
	w8(put_lit(text, text + MH_TEXT_LEN, "Waiting for qwark"), 0);
	w8(ST(ST_KIND), MROW_INFO);
	w8(ST(ST_FLAGS), 0);
	w16(ST(ST_ID), 0);
	w32(button, 0);
	w32(button + 4, text);
	w32(button + 8, 0);
	w8(ST(ST_NROWS), 1);
	w8(ST(ST_CURSOR_HINT), 0);
	w8(ST(ST_PFLAGS), MBOX_PAGE_ROOT);
}

/* `c` moved onto the page, and off a row the cursor cannot land on. */
static u32 pick_cursor(u32 c)
{
	u32 n = nrows();
	u32 i;

	if (c >= n) c = n - 1;
	if ((row_flags(c) & MROWF_ENABLED) != 0) return c;
	for (i = 0; i < n; i++) {
		if ((row_flags(i) & MROWF_ENABLED) != 0) return i;
	}
	return c;
}

/* Where the cursor goes: the page's own row for a page not shown before, else `keep`. */
static u32 cursor_for(u32 keep)
{
	u32 c = r8(ST(ST_NEW_KEY)) != 0 ? r8(ST(ST_CURSOR_HINT)) : keep;

	w8(ST(ST_NEW_KEY), 0);
	return pick_cursor(c);
}

/*
 * A new page while the list is up: the rows handed over again and the
 * highlight put back, as N0's toggle did, and the list laid out again when it
 * has a different number of rows.
 */
static void redraw(int p, u32 old_n)
{
	u32 n = nrows();

	set_cursor(p, (int)cursor_for((u32)get_cursor(p)));
	set_title(p, ST(ST_TITLE));
	set_buttons(p, (int)n, ST(ST_BUTTONS));
	if (n != old_n) update_widgets(p);
	select_row();
}

/* ------------------------------------------------------------ the events */

/*
 * One event in flight at a time: the record, then ev_seq. A press while qwark
 * has not answered the last one is dropped, which is what keeps a slow answer
 * from turning into a queue of stale presses. Answers 1 when it went out.
 */
static int post(u32 kind, u32 row)
{
	u32 seq = r32(BOX(MBOX_OFF_EV_SEQ));

	if (r8(ST(ST_HAVE_PAGE)) == 0) return 0;
	if (seq != r32(BOX(MBOX_OFF_EV_ACK))) {
		inc(ST_DROPPED);
		return 0;
	}

	w8(BOX(MBOX_OFF_EV_KIND), kind);
	w8(BOX(MBOX_OFF_EV_ROW), row);
	w16(BOX(MBOX_OFF_EV_ID), row < nrows() ? r16(ST(ST_ID) + 2 * row) : 0);
	w32(BOX(MBOX_OFF_EV_KEY), r32(ST(ST_SHOWN_KEY)));
	mh_sync();
	w32(BOX(MBOX_OFF_EV_SEQ), seq + 1);

	w32(ST(ST_BUSY_SINCE), r32(BOX(MBOX_OFF_FRAMES)));
	inc(ST_EVENTS);
	return 1;
}

/* Whether the page has a row of `kind`. */
static int has_kind(u32 kind)
{
	u32 n = nrows();
	u32 i;

	for (i = 0; i < n; i++) {
		if (r8(ST(ST_KIND) + i) == kind) return 1;
	}
	return 0;
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
	u32 pad, bits, kind, c;

	if (p != 0) return 0;
	pad = r32(DL_PAD0_PTR);
	if (pad == 0) return 0;
	bits = r32(pad + DL_PAD_HUDBITS);
	if (bits == 0) return 0;
	if ((bits & (PADB_CROSS | PADB_TRIANGLE | PADB_SQUARE | PADB_CIRCLE |
	             PADB_UP | PADB_DOWN)) != 0)
		return 0;

	if ((bits & PADB_LEFT) != 0)       kind = MBOX_EV_LEFT;
	else if ((bits & PADB_RIGHT) != 0) kind = MBOX_EV_RIGHT;
	else if ((bits & PADB_L1) != 0)    kind = MBOX_EV_PAGE_PREV;
	else if ((bits & PADB_L2) != 0)    return 0;
	else if ((bits & PADB_R1) != 0)    kind = MBOX_EV_PAGE_NEXT;
	else return 0;

	if (!takes_input(p, 6)) return 0;
	w32(pad + DL_PAD_HUDBITS, 0);

	c = (u32)get_cursor(p);
	if (kind == MBOX_EV_LEFT || kind == MBOX_EV_RIGHT) {
		if (c >= nrows() || (row_flags(c) & MROWF_STEPS) == 0) return 1;
	} else if (!has_kind(kind == MBOX_EV_PAGE_PREV ? MROW_PREV : MROW_MORE)) {
		return 1;
	}

	if (post(kind, c)) play_sound(SND_MOVE);
	return 1;
}

/* ---------------------------------------------------------- the entries */

/*
 * The RaCMAN row: what Settings' callback does for screen 12, for 36. The
 * Options cursor is kept so Triangle comes back to the row.
 */
void mh_open(int arg)
{
	int p = get_canvas();

	(void)arg;
	inc(ST_OPENS);
	w32(DL_MENU_PLAYER + (u32)p * DL_MENU_PLAYER_SZ + DL_MENU_OPT_CURSOR, (u32)get_cursor(p));
	push_screen(p, MH_SCREEN_ID);
}

/*
 * From mh_options_stub, with the arguments MenuMain_StartOptions is about to
 * hand GuiSupport_SetMenuButtons: the player, the row count and the rows,
 * MenuMain_PlayerData[p].vButtons, whose count follows them at +0x60. The last
 * row is "Go back"; the RaCMAN row goes in front of it. Not in multiplayer,
 * whose pause does not stop the game. The answer is the count the stub passes on.
 */
int mh_options_hook(int p, int n, u32 rows)
{
	u32 at;
	u32 i;

	(void)p;
	if (r32(ST(ST_MAGIC)) != MH_MAGIC) return n;
	if (r32(DL_GAME_TYPE) == 1) return n;
	if (n < 1 || n >= 8) {
		inc(ST_OPTIONS_FULL);
		return n;
	}

	w8(put_lit(ST(ST_OPTION_LABEL), ST(ST_OPTION_LABEL) + 16, "RaCMAN"), 0);
	at = rows + (u32)(n - 1) * MH_BUTTON_SIZE;
	for (i = 0; i < MH_BUTTON_SIZE; i += 4) w32(at + MH_BUTTON_SIZE + i, r32(at + i));
	w32(at, 0x01000000u);
	w32(at + 4, ST(ST_OPTION_LABEL));
	w32(at + 8, MH_FN_OPEN);
	w32(rows + DL_ROWS_COUNT_OFF, (u32)(n + 1));
	inc(ST_OPTIONS_ROWS);
	return n + 1;
}

/*
 * Screen 36's fcStart: MenuMain_StartOptions' shape, with qwark's page. A
 * request still in the mailbox is spent: the menu is up.
 */
void mh_start(int p)
{
	int mode;
	u32 r;

	if (r32(ST(ST_MAGIC)) != MH_MAGIC) w32(ST(ST_MAGIC), MH_MAGIC);
	inc(ST_STARTS);
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	w8(ST(ST_WAITING), 0);

	if (!take_page() && r8(ST(ST_HAVE_PAGE)) == 0) placeholder();

	set_cursor(p, (int)cursor_for(r8(ST(ST_LAST_CURSOR))));
	set_title(p, ST(ST_TITLE));

	mode = screen_mode(p);
	if (mode < 0 || mode > 4) mode = 0;
	r = DL_MENU_VSCREENS + 16u * (u32)mode;
	set_target(p, rf32(r), rf32(r + 4), rf32(r + 8), rf32(r + 12));

	/* The game's own expression: the header unless this is multiplayer. */
	set_header(p, r32(DL_GAME_TYPE) != 1);
	set_buttons(p, (int)nrows(), ST(ST_BUTTONS));
	show_menu(p, MH_FN_CLOSE);
	play_sound(SND_SCREEN);
}

/* Screen 36's fcStop: the game's MenuMain_Stop, once the mailbox says closed. */
void mh_stop(int p)
{
	w8(ST(ST_LAST_CURSOR), get_cursor(p));
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_CLOSED);
	w8(ST(ST_WAITING), 0);
	menu_stop(p);
}

/* A row: the cross on whatever the cursor is on. */
void mh_row(int arg)
{
	int p = get_canvas();
	u32 c = (u32)get_cursor(p);

	(void)arg;
	if (c >= nrows() || (row_flags(c) & MROWF_ENABLED) == 0) return;
	if (post(MBOX_EV_ACTIVATE, c)) play_sound(SND_FORWARD);
}

/*
 * The list's close callback: 1 for Start or Select, which closes the menu, 0
 * for Triangle, which goes back. At the root, and before any page, back is the
 * game's own (Options again, or out of the pause); anywhere else qwark goes
 * back a page. Start always works, whatever qwark is doing.
 */
void mh_close(int arg)
{
	int p = get_canvas();

	if (arg != 0 || r8(ST(ST_HAVE_PAGE)) == 0 ||
	    (r8(ST(ST_PFLAGS)) & MBOX_PAGE_ROOT) != 0) {
		go_back(arg);
		return;
	}
	if (post(MBOX_EV_BACK, (u32)get_cursor(p))) play_sound(SND_BACK);
}

/*
 * From mh_frame_stub, every frame any list menu is fully shown, in place of
 * GuiSupport_HandleMenuInput. Another screen's list answers 0 at once and the
 * stub goes on to the game's input. For screen 36: count the frame, take a
 * request, take a new page, say so in the title when qwark is not answering,
 * and take Left, Right, L1 and R1. Answers 1 when the game's input is to be
 * skipped this frame.
 */
int mh_frame(int p)
{
	u32 req, old_n, frames, ev, ack;

	if (get_screen(p) != MH_SCREEN_ID) return 0;
	if (r32(ST(ST_MAGIC)) != MH_MAGIC) return 0;

	frames = r32(BOX(MBOX_OFF_FRAMES)) + 1;
	w32(BOX(MBOX_OFF_FRAMES), frames);
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);

	req = r8(BOX(MBOX_OFF_REQUEST));
	if (req != MBOX_REQ_NONE) {
		w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
		if (req == MBOX_REQ_CLOSE) {
			go_back(1);
			return 1;
		}
	}

	old_n = nrows();
	if (take_page()) {
		w8(ST(ST_WAITING), 0);
		redraw(p, old_n);
	} else {
		ev = r32(BOX(MBOX_OFF_EV_SEQ));
		ack = r32(BOX(MBOX_OFF_EV_ACK));
		if (r8(ST(ST_WAITING)) != 0) {
			if (ev == ack) {
				w8(ST(ST_WAITING), 0);
				set_title(p, ST(ST_TITLE));
			}
		} else if (ev != ack && frames - r32(ST(ST_BUSY_SINCE)) >= MH_WAIT_FRAMES) {
			w8(put_lit(ST(ST_WAIT_TITLE), ST(ST_WAIT_TITLE) + MBOX_TITLE_LEN,
			           "Waiting for qwark..."), 0);
			w8(ST(ST_WAITING), 1);
			set_title(p, ST(ST_WAIT_TITLE));
		}
	}

	return steps(p);
}
