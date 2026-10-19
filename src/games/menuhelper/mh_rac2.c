/*
 * Going Commando's console menu shim: qwark's pages on the TV with the game's
 * own pause-menu looks. mh_rac2.h says where it lives, what it hooks and what
 * it calls; mh_shim.h is the interface to the core (mh_core.c), which reads the
 * mailbox, composes the text and posts the events. This file only talks to the
 * game, the way probe N2c did on hardware:
 *
 *   MAIN    a page of links and buttons on the pause root's vertical bars:
 *           their labels are text ids the msg_string hook answers with the
 *           core's rows, their cursor, its sound and the grey of a bar the
 *           cursor skips are the game's, and the root's own draw draws them;
 *   CONFIG  a page of labels and controls drawn the way Special > Cheats is
 *           drawn: its frame, the page's title, the "Toggle" and "Exit" hints,
 *           the label at the left and the value centred at the right, the
 *           cursor's row in Cheats' highlight, a row it skips grey, scrolled
 *           when the page has more rows than the frame.
 *
 * The menu's screen is one of six screens of the shim's own (mh_rac2_stubs.S),
 * all with the one item whose update, draw, enter and exit are the four entries
 * below, whatever the page is: the enter asks the core which look the page
 * wants and sets it up. Moving to a page the look cannot take in place, or to
 * the other look, is the game's own way between pause screens: the next of the
 * shim's screens goes into the pause manager's next screen, and the manager
 * calls this one's exit, plays its screen sound and enters that one.
 *
 * The game reaches it through
 *   mh_msg_stub      the bars' labels and the Help bar's "Trainer" (assembler);
 *   mh_inject_stub, mh_combo_stub, mh_root_stub   the combo: the Start path
 *                    opens the pause for an open request, and the manager
 *                    starts it on the menu's screen instead of the root
 *                    (assembler, the first calling mh_inject);
 *   mh_update, mh_draw, mh_enter, mh_exit   the item, through descriptors.
 *
 * Everything the game keeps a pointer to - the screens, the item, the bars'
 * text ids and their text - is in the renderer's data, which stays put for as
 * long as the game runs and changes only on the game's thread. Every access to
 * game memory goes through r8..w32 (mh_common.h), so the host tests run this
 * file on qwark's fake console with the real bytes.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define SH(o)    ((u32)MH_SHIM + (u32)(o))
#define SCR(k)   ((u32)MH_SCREENS + (u32)(k) * R2_SCREEN_SIZE)
#define IDS      ((u32)MH_SCREENS + MS_IDS)
#define MORE     ((u32)MH_SCREENS + MS_MORE)
#define MGR(o)   ((u32)R2_PAUSE_MGR + (u32)(o))

/* The shim's state starts where the core's ends, and the screens where the shim's ends. */
typedef char mh_rac2_layout[(MH_STATE + MH_CORE_SIZE == MH_SHIM &&
                             MH_SHIM + SH_SIZE == MH_SCREENS && (MH_SCREENS & 15) == 0 &&
                             MH_SCREENS + MS_SIZE <= MH_DATA_END &&
                             SH_CUTS + 4 <= SH_SIZE &&
                             MS_ITEM == MS_NSCREENS * R2_SCREEN_SIZE &&
                             MS_IDS == MS_ITEM + R2_ITEM_SIZE &&
                             MS_TRAINER == MS_IDS + 4 * (MH_MAIN_ROWS + 1) &&
                             MS_MORE == MS_TRAINER + 8 && MS_SIZE == MS_MORE + 4 &&
                             MH_MAIN_ROWS <= 7 && MH_CFG_ROWS <= MH_ROWS &&
                             CFG_VIS < CFG_VIS_MAX && CFG_VIS_MAX <= MH_CFG_ROWS) ? 1 : -1];

/* The entries the game reaches; file-local in the host tests, which build several renderers. */
MH_API int mh_update(u32 item);
MH_API int mh_draw(u32 item);
MH_API int mh_enter(u32 item, int arg);
MH_API int mh_exit(u32 item, int arg);
MH_API u32 mh_inject(u32 test);

typedef int  (*fn_i_v)(void);
typedef int  (*fn_i_u)(u32);
typedef int  (*fn_i_uu)(u32, u32);
typedef int  (*fn_i_ui)(u32, int);
typedef u32  (*fn_u_i)(int);
typedef u32  (*fn_u_ui)(u32, int);
typedef void (*fn_v_v)(void);
typedef void (*fn_v_i)(int);
typedef void (*fn_v_ui)(u32, int);
typedef void (*fn_v_uii)(u32, int, int);
typedef void (*fn_v_uiu)(u32, int, u32);
typedef void (*fn_v_iii)(int, int, int);
typedef void (*fn_text)(int, int, u32, u32, int);
typedef void (*fn_sprite)(u32, u32, u32, u32, float, float, float, float, float);

#define root_draw    GAME_FN(fn_i_u,    OPD_ROOT_DRAW,   mock_root_draw)
#define bars_setup   GAME_FN(fn_v_v,    OPD_BARS_SETUP,  mock_bars_setup)
#define bars_update  GAME_FN(fn_i_uu,   OPD_BARS_UPDATE, mock_bars_update)
#define bars_enable  GAME_FN(fn_v_uii,  OPD_BARS_ENABLE, mock_bars_enable)
#define bars_labels  GAME_FN(fn_v_uiu,  OPD_BARS_LABELS, mock_bars_labels)
#define bars_eighth  GAME_FN(fn_v_ui,   OPD_BARS_EIGHTH, mock_bars_eighth)
#define msg_string   GAME_FN(fn_u_i,    OPD_MSG_STRING,  mock_msg_string)
#define draw_begin   GAME_FN(fn_v_i,    OPD_DRAW_BEGIN,  mock_draw_begin)
#define draw_end     GAME_FN(fn_v_v,    OPD_DRAW_END,    mock_draw_end)
#define sprite       GAME_FN(fn_u_ui,   OPD_SPRITE,      mock_sprite)
#define sprite_draw  GAME_FN(fn_sprite, OPD_SPRITE_DRAW, mock_sprite_draw)
#define text_centre  GAME_FN(fn_text,   OPD_TEXT_CENTRE, mock_text_centre)
#define text_left    GAME_FN(fn_text,   OPD_TEXT_LEFT,   mock_text_left)
#define text_mid     GAME_FN(fn_text,   OPD_TEXT_MID,    mock_text_mid)
#define text_width   GAME_FN(fn_i_ui,   OPD_TEXT_WIDTH,  mock_text_width)
#define play_sound   GAME_FN(fn_v_iii,  OPD_PLAY_SOUND,  mock_play_sound)
#define lv_race      GAME_FN(fn_i_v,    OPD_LV_RACE,     mock_lv_race)
#define lv_ship      GAME_FN(fn_i_v,    OPD_LV_SHIP,     mock_lv_ship)
#define lv_ship_on   GAME_FN(fn_i_v,    OPD_LV_SHIP_ON,  mock_lv_ship_on)
#define lv_orbit     GAME_FN(fn_i_v,    OPD_LV_ORBIT,    mock_lv_orbit)
#define lv_other     GAME_FN(fn_i_v,    OPD_LV_OTHER,    mock_lv_other)
#define lv_pstate    GAME_FN(fn_i_v,    OPD_LV_PSTATE,   mock_lv_pstate)

static u32 type(void)
{
	return r8(SH(SH_TYPE));
}

/* This frame's presses, as every pause screen reads them. */
static u32 edges(void)
{
	return r32(R2_PAD_EDGES);
}

/* The root's bars, or 0 before the UI manager exists. */
static u32 bars(void)
{
	u32 m = r32(R2_UI_MANAGER);
	return m != 0 ? m + R2_BARS_OFF : 0;
}

/* Which of the shim's screens `s` is (MS_*), MH_NONE for any other. */
static u32 screen_of(u32 s)
{
	u32 k;

	for (k = 0; k < MS_NSCREENS; k++) {
		if (s == SCR(k)) return k;
	}
	return MH_NONE;
}

/* Start or Select (close) and Triangle (back), which are no row's, on either look. */
static u32 shared_press(u32 e)
{
	if ((e & (PADB_START | PADB_SELECT)) != 0) return MH_BTN_CLOSE;
	if ((e & PADB_TRIANGLE) != 0) return MH_BTN_BACK;
	return 0;
}

/* Left, Right, L1 or R1, in that order of rank. */
static u32 step_press(u32 e)
{
	if ((e & PADB_LEFT) != 0) return MH_BTN_LEFT;
	if ((e & PADB_RIGHT) != 0) return MH_BTN_RIGHT;
	if ((e & PADB_L1) != 0) return MH_BTN_L1;
	if ((e & PADB_R1) != 0) return MH_BTN_R1;
	return 0;
}

/* A press on `row`, the cross remembered for its sound. */
static void press(u32 b, u32 row)
{
	if (b == MH_BTN_CROSS) w8(SH(SH_PRESS_ROW), row);
	mh_core_press(0, b, row);
}

/* ------------------------------------------------------- the MAIN look */

/*
 * The page's rows on the bars: their text ids, which the msg hook answers with
 * the core's rows and the bars keep as pointers, so the texts follow the core
 * without another call; which bars the cursor may land on; the eighth bar,
 * the root's own, off.
 */
static void main_rows(u32 r)
{
	u32 n = mh_nrows(), i;

	bars_labels(r, (int)n, IDS);
	for (i = 0; i < n; i++) bars_enable(r, (int)i, (mh_row_flags(i) & MROWF_ENABLED) != 0);
	bars_eighth(r, 0);
}

static void main_start(void)
{
	u32 c = mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)), r = bars();

	if (r == 0) return;
	main_rows(r);
	w32(r + R2_R_CURSOR, c);
	bars_update(r, 0);
}

/*
 * A MAIN page's frame: the core's, then Start and Select (close) and Triangle
 * (back), then the bars' own update, which moves the cursor with Up and Down
 * and its sound and answers the bar it is on, then the cross, Left, Right, L1
 * and R1 on that bar.
 */
static void main_frame(void)
{
	u32 e, r, b, c;

	if (mh_core_frame(0) == MH_GONE) return;
	e = edges();
	b = shared_press(e);
	if (b != 0) {
		press(b, shim_cursor(0));
		return;
	}
	r = bars();
	if (r == 0) return;
	c = (u32)bars_update(r, e);
	b = (e & r32(R2_CONFIRM_MASK)) != 0 ? MH_BTN_CROSS : step_press(e);
	if (b != 0) press(b, c);
}

/* ----------------------------------------------------- the CONFIG look */

static u32 cfg_visible(u32 n)
{
	return n <= CFG_VIS_MAX ? n : CFG_VIS;
}

/* The first row on screen, so that the cursor's is. */
static void cfg_scroll(void)
{
	u32 n = mh_nrows(), v = cfg_visible(n), c = r8(SH(SH_CCUR)), t = r8(SH(SH_CTOP));

	if (n <= v) {
		t = 0;
	} else {
		if (c < t) t = c;
		if (c >= t + v) t = c - v + 1;
		if (t > n - v) t = n - v;
	}
	w8(SH(SH_CTOP), t);
}

/*
 * Each label ends CFG_GAP before its value, centred on CFG_X_VALUE (Cheats'
 * columns): a label that would reach it is cut short with "..", measured with
 * the game's own width (N2c's fit), in the text set on show. The core writes a
 * page refreshed in place over that set, so every page shown is fitted again.
 */
static void cfg_fit(void)
{
	u32 n = mh_nrows(), i, lab, len, max, cuts = 0;
	int room;

	for (i = 0; i < n; i++) {
		lab = mh_label(i);
		room = (int)CFG_X_VALUE - text_width(mh_value(i), -1) / 2 - (int)CFG_GAP - (int)CFG_X_LABEL;
		if (text_width(lab, -1) <= room) continue;
		/* A slider's row keeps its four words past a shorter label (round 3). */
		max = (mh_row_flags(i) & MROWF_RANGE) != 0 ? MH_RANGE_CFG - 3 : MH_LABEL_LEN - 3;
		len = 0;
		while (len < max && r8(lab + len) != 0) len++;
		while (len > 0) {
			len--;
			w8(lab + len, '.');
			w8(lab + len + 1, '.');
			w8(lab + len + 2, 0);
			if (text_width(lab, -1) <= room) break;
		}
		cuts++;
	}
	w32(SH(SH_CUTS), cuts);
}

static void cfg_start(void)
{
	w8(SH(SH_CCUR), mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
	w8(SH(SH_CTOP), 0);
	cfg_scroll();
	cfg_fit();
}

/* The next row from `from` in direction `d` the cursor may stand on, wrapping; `from` when none. */
static u32 cfg_step(u32 from, int d)
{
	u32 n = mh_nrows(), r = from, k;

	for (k = 0; k < n; k++) {
		r = (r + n + (u32)d) % n;
		if ((mh_row_flags(r) & MROWF_ENABLED) != 0) return r;
	}
	return from;
}

/*
 * A CONFIG page's frame, as Cheats' update (0xBC17B8) reads the pad: the
 * core's, then Start and Select, Triangle, Up and Down (the tick, past a grey
 * row, wrapping, the page scrolled), then one of the cross, Left, Right, L1
 * and R1 on the cursor's row; Left or Right on a row they do nothing on is
 * Cheats' refusal.
 */
static void cfg_frame(void)
{
	u32 e, b, c;

	if (mh_core_frame(0) == MH_GONE) return;
	e = edges();
	c = r8(SH(SH_CCUR));
	b = shared_press(e);
	if (b != 0) {
		press(b, c);
		return;
	}
	if ((e & (PADB_UP | PADB_DOWN)) != 0) {
		w8(SH(SH_CCUR), cfg_step(c, (e & PADB_UP) != 0 ? -1 : 1));
		cfg_scroll();
		play_sound(SND_MOVE, 0, 0);
		return;
	}
	b = (e & r32(R2_CONFIRM_MASK)) != 0 ? MH_BTN_CROSS : step_press(e);
	if ((b == MH_BTN_LEFT || b == MH_BTN_RIGHT) && (mh_row_flags(c) & MROWF_STEPS) == 0) {
		play_sound(SND_NO, 0, 0);
		return;
	}
	if (b != 0) press(b, c);
}

/*
 * What Cheats' draw (0xBC1A50) draws, with the core's page: the frame's three
 * sprites centred on the screen, the title, the hints, and the rows on screen
 * in two columns, the cursor's in Cheats' highlight, one it skips grey, and
 * "..." over the first or under the last where more are scrolled away.
 */
static void cfg_draw(void)
{
	u32 m = r32(R2_UI_MANAGER), tex, n = mh_nrows(), v, t, c, i, row, colour;
	int cx, y;
	float x, scale;

	if (m == 0) return;
	tex = m + R2_TEXSET_OFF;
	cx = (int)r32(R2_SCREEN_W) / 2;
	x = (float)cx;
	scale = rf32(R2_SPRITE_SCALE);
	v = cfg_visible(n);
	t = r8(SH(SH_CTOP));
	c = r8(SH(SH_CCUR));

	draw_begin(0);
	sprite_draw(sprite(tex, CFG_SPRITE_FRAME), 0, 0, CFG_COLOR_BACK, x, 208.0f, 1.0f, scale, 0.84f);
	sprite_draw(sprite(tex, CFG_SPRITE_FRAME + 1), 0, 0, CFG_COLOR_EDGE, x, 208.0f, 1.0f, scale, 0.84f);
	sprite_draw(sprite(tex, CFG_SPRITE_FRAME + 2), 0, 0, CFG_COLOR_EDGE, x, 208.0f, 1.0f, scale, 0.84f);
	text_centre(cx, CFG_Y_TITLE, CFG_COLOR_TEXT, mh_title(), -1);
	text_centre(cx, CFG_Y_HINT1, CFG_COLOR_TEXT, msg_string(R2_ID_TOGGLE), -1);
	text_centre(cx, CFG_Y_HINT2, CFG_COLOR_TEXT, msg_string(R2_ID_EXIT), -1);
	for (i = 0, y = CFG_Y_ROW0; i < v && t + i < n; i++, y += CFG_DY) {
		row = t + i;
		colour = row == c ? CFG_COLOR_CURSOR :
		         (mh_row_flags(row) & MROWF_ENABLED) != 0 ? CFG_COLOR_TEXT : CFG_COLOR_GREY;
		text_left(CFG_X_LABEL, y, colour, mh_label(row), -1);
		text_mid(CFG_X_VALUE, y, colour, mh_value(row), -1);
	}
	if (t > 0) text_centre(cx, CFG_Y_ROW0 - CFG_DY, CFG_COLOR_GREY, MORE, -1);
	if (t + v < n) text_centre(cx, CFG_Y_ROW0 + (int)v * CFG_DY, CFG_COLOR_GREY, MORE, -1);
	draw_end();
}

/* ------------------------------------------------------- the shim, for the core */

/*
 * The page shown refreshed in place. MH_RESTART_* name both of `how`'s bits for
 * both looks, so a page that is new or has another number of rows never gets
 * here: the core opens another screen for it instead. A CONFIG page draws its
 * texts every frame and only has its labels fitted again; the bars keep
 * pointers to theirs and have which may be landed on set again.
 */
MH_API void shim_show(int p, u32 how)
{
	u32 r;

	(void)p;
	(void)how;
	if (type() == MBOX_PTYPE_CONFIG) {
		cfg_fit();
		return;
	}
	r = bars();
	if (r == 0) return;
	main_rows(r);
	bars_update(r, 0);
}

/*
 * The next page in the next of the shim's screens of its look: the manager
 * calls this one's exit, plays the game's screen sound and enters that one,
 * whose enter sets the page up. Nothing is drawn until it has.
 */
MH_API void shim_switch(int p)
{
	u32 cur = r8(SH(SH_SCREEN)), next;

	(void)p;
	if (r8(MH_STATE + ST_PTYPE) == MBOX_PTYPE_CONFIG) next = cur == MS_CFG_A ? MS_CFG_B : MS_CFG_A;
	else next = cur == MS_MAIN_A ? MS_MAIN_B : MS_MAIN_A;
	w8(SH(SH_READY), 0);
	w32(MGR(R2_MGR_NEXT), SCR(next));
}

/*
 * Back from the root: to the pause's own root when the menu came from its Help
 * bar, with the game's own bars put back and the cursor on Help's, as N2c did;
 * out of the pause from the combo, as the root's own Triangle goes out. Start,
 * Select and a close request: out of the pause, with the sound the root makes
 * as it closes. The update answers 1 and the manager closes the pause, calling
 * the exit, once it has been open ten frames.
 */
MH_API void shim_close(int p, u32 how)
{
	u32 r;

	(void)p;
	if (how == MH_CLOSE_BACK && r8(SH(SH_FROM_HELP)) != 0) {
		r = bars();
		if (r != 0) {
			bars_setup();
			w32(r + R2_R_CURSOR, R2_HELP_BAR);
			bars_update(r, 0);
		}
		w8(SH(SH_READY), 0);
		w32(MGR(R2_MGR_NEXT), R2_ROOT_SCREEN);
		return;
	}
	if (r8(SH(SH_RET)) == 0) play_sound(SND_SCREEN, 0, 0);
	w8(SH(SH_RET), 1);
}

/* The CONFIG page draws its title every frame; the bars have none. */
MH_API void shim_title(int p)
{
	(void)p;
}

/* Neither look has a line for a description. */
MH_API void shim_desc(int p)
{
	(void)p;
}

MH_API u32 shim_cursor(int p)
{
	u32 r;

	(void)p;
	if (type() == MBOX_PTYPE_CONFIG) return r8(SH(SH_CCUR));
	r = bars();
	return r != 0 ? r32(r + R2_R_CURSOR) : 0;
}

/*
 * The cross on a row that goes to another page or window, or closes the
 * menu, has the sound the manager plays as the screen changes, and so do back
 * and a window turned with L1 or R1; a change in place has Cheats' change
 * sound.
 */
MH_API void shim_sound(u32 snd)
{
	u32 row = r8(SH(SH_PRESS_ROW)), kind = mh_row_kind(row);

	if (snd == MH_SND_FORWARD) {
		if (kind == MROW_LINK || kind == MROW_MORE || kind == MROW_PREV || kind == MROW_BACK ||
		    (mh_row_flags(row) & MROWF_CLOSES) != 0 ||
		    (type() == MBOX_PTYPE_MAIN && (kind == MROW_CHOICE || kind == MROW_OPTION)))
			return;
	} else if (snd != MH_SND_STEP) {
		return;
	}
	play_sound(SND_CHANGE, 0, 0);
}

/* ------------------------------------------------------------ the entries */

/*
 * One of the shim's screens enters, on the first frame it is the manager's:
 * the page qwark posted, with the look it asks for. A screen entered from none
 * of the shim's is the menu opening: from the Help bar's screen, or from the
 * combo's (the manager's previous screen is the root, or nothing).
 */
MH_API int mh_enter(u32 item, int arg)
{
	u32 k = screen_of(r32(MGR(R2_MGR_CURRENT))), t;

	(void)item;
	(void)arg;
	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC) return 0;
	if (screen_of(r32(MGR(R2_MGR_PREVIOUS))) == MH_NONE) w8(SH(SH_FROM_HELP), k == MS_HELP);
	w32(SH(SH_UP), R2_SCREEN_UP);
	w8(SH(SH_SCREEN), k);
	w8(SH(SH_RET), 0);
	t = (u32)mh_core_start(0);
	w8(SH(SH_TYPE), t);
	if (t == MBOX_PTYPE_CONFIG) cfg_start();
	else main_start();
	w8(SH(SH_READY), 1);
	return 0;
}

/*
 * Every frame it is up: 1 closes the pause. Once it has said so it says so
 * until the manager closes it, which waits for the pause's tenth frame.
 */
MH_API int mh_update(u32 item)
{
	(void)item;
	if (r8(SH(SH_RET)) != 0) return 1;
	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC) {
		/* The state is not the renderer's: leave the pause rather than run on it. */
		shim_close(0, MH_CLOSE_EXIT);
		return 1;
	}
	if (type() == MBOX_PTYPE_CONFIG) cfg_frame();
	else main_frame();
	return r8(SH(SH_RET));
}

/* Every frame it is drawn, once it has entered: the root's draw on the bars, or Cheats' look. */
MH_API int mh_draw(u32 item)
{
	if (r8(SH(SH_READY)) == 0 || r32(MH_STATE + ST_MAGIC) != MH_MAGIC) return 0;
	if (type() == MBOX_PTYPE_CONFIG) cfg_draw();
	else root_draw(item);
	return 0;
}

/*
 * It stops being the manager's screen: on the way to the next of the shim's
 * (the core's switch), back to the root, or as the pause closes. Only the
 * first exit counts; the menu is down unless the core is switching.
 */
MH_API int mh_exit(u32 item, int arg)
{
	(void)item;
	(void)arg;
	w8(SH(SH_READY), 0);
	if (r32(SH(SH_UP)) == 0) return 0;
	if (r8(MH_STATE + ST_SWITCHING) == 0) w32(SH(SH_UP), 0);
	mh_core_stop(0);
	return 0;
}

/*
 * Whether the pause would open on the root with Help: the questions the
 * root's setup asks (0xBB6970), in its order. Not in a hoverbike race, a ship
 * level in the ship, an orbit, on the hoverbike or in the other states that
 * root has bars of their own for: the menu does not open there.
 */
static int may_open(void)
{
	u32 ps;

	if (r32(R2_UI_MANAGER) == 0) return 0;
	if (lv_race() != 0) return 0;
	if (lv_ship() != 0 && lv_ship_on() != 0) return 0;
	if (lv_orbit() != 0) return 0;
	ps = (u32)lv_pstate();
	if (ps == R2_PS_HOVERBIKE || lv_other() != 0) return 0;
	return (u32)lv_pstate() != R2_PS_OTHER;
}

/*
 * From mh_inject_stub, with a request in the mailbox, in place of the mode 0
 * update's test of the pad for Start (`test`, the game's own answer): an open
 * request, where the pause would open on the root with Help, is taken and
 * answered with Start, and the Start path tells the root stub; anything else
 * is taken and dropped, and the test is the game's.
 */
MH_API u32 mh_inject(u32 test)
{
	if (r32(MH_STATE + ST_MAGIC) == MH_MAGIC && r8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN &&
	    may_open() && mh_core_open_request()) {
		w8(SH(SH_PRESSED), 1);
		return PADB_START;
	}
	w8(MH_BOX + MBOX_OFF_REQUEST, MBOX_REQ_NONE);
	return test;
}
