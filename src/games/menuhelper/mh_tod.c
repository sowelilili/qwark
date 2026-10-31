/*
 * Tools of Destruction's console menu shim (a prototype, 2026-10-31): qwark's
 * pages on the TV, drawn over the frame with the game's own HUD calls.
 * mh_tod.h says where it lives, what it hooks and what it calls; mh_shim.h is
 * the interface to the core (mh_core.c), which reads the mailbox, composes the
 * text and posts the events; docs/tod-menu.md has the reverse engineering.
 *
 * ToD's pause menu is an Anark presentation run by Lua, with no row a word
 * could turn into the menu's, so there is no native look to borrow: the shim
 * draws one panel for both page types, a darkened box with the page's title,
 * its rows (a MAIN row's one string, a CONFIG row's label at the left and its
 * value at the right), the cursor's bar and a line of controls, with the
 * HUD's sprite and the font of the load screen's arrival text. Every page is
 * taken in place, so a switch to the other type just starts the screen again
 * in the same frame.
 *
 * The game reaches it through two stubs (mh_tod_stubs.S):
 *   mh_frame   every frame of a level, at the frame's end: takes an open
 *              request in play or in the pause, runs the core's frame and the
 *              pad, and draws;
 *   mh_pad     every time the pads are read: notes pad 0's buttons, and while
 *              the menu is up blanks every pad's block, so the game sees
 *              nothing pressed. The menu's frame puts pad 0's held mask back
 *              at its end, so the game's next read takes no press for a
 *              button that was down all along, and qwark's combos see it.
 *
 * Opened from play the menu freezes it with the game clock's time scale, 0
 * while the menu is up and the game's own back as it closes; opened from the
 * game's pause it draws over that, the game paused already. A load, a
 * cinematic or anything else that takes the game out of those two states
 * closes it.
 *
 * Every access to game memory goes through r8..w32 (mh_common.h), so the host
 * tests run this file on qwark's fake console with the real bytes, and the
 * game's calls are mocks there.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define SH(o)       ((u32)MH_SHIM + (u32)(o))
#define ST(o)       ((u32)MH_STATE + (u32)(o))
#define BOX(o)      ((u32)MH_BOX + (u32)(o))
#define PAD(i, o)   ((u32)TM_PAD0 + (u32)(i) * TM_PAD_STRIDE + (u32)(o))

/* The shim's state starts where the core's ends, and fits the data with the read-only data after it. */
typedef char mh_tod_layout[(MH_STATE + MH_CORE_SIZE == MH_SHIM && MH_SHIM + SH_SIZE <= MH_DATA_END &&
                            SH_SPRITE + SPR_SIZE <= SH_SIZE && (SH_POS & 7) == 0 && (SH_RECT & 7) == 0 &&
                            (SH_SPRITE & 7) == 0 && MH_MAIN_ROWS <= MH_ROWS && MH_CFG_ROWS <= MH_ROWS &&
                            MH_PICKER == 0 && TM_PAD_BLANK + 4 <= TM_PAD_STRIDE) ? 1 : -1];

/* The entries the stubs reach; file-local in the host tests, which build several renderers. */
MH_API void mh_frame(void);
MH_API void mh_pad(void);

typedef void  (*fn_text)(u32, u32, u32, u32, u32, u32, u32, u32, u32,
                         float, float, float, float, float, float, float);
typedef void  (*fn_v_u)(u32);
typedef void  (*fn_v_uu)(u32, u32);
typedef void  (*fn_v_ii)(int, int);
typedef void  (*fn_v_v)(void);
typedef float (*fn_f_v)(void);

#define text_draw       GAME_FN(fn_text, OPD_TEXT_DRAW,       mock_text_draw)
#define sprite_init     GAME_FN(fn_v_u,  OPD_SPRITE_INIT,     mock_sprite_init)
#define sprite_draw     GAME_FN(fn_v_uu, OPD_SPRITE_DRAW,     mock_sprite_draw)
#define hud_reset_alpha GAME_FN(fn_v_v,  OPD_HUD_RESET_ALPHA, mock_hud_reset_alpha)
#define font_select     GAME_FN(fn_v_ii, OPD_FONT_SELECT,     mock_font_select)
#define font_height     GAME_FN(fn_f_v,  OPD_FONT_HEIGHT,     mock_font_height)

#define P 0

/* ------------------------------------------------------------- open, close */

static int is_up(void)
{
	return r32(SH(SH_UP)) != 0;
}

/*
 * The panel's own words, into the shim's state as the menu opens: the game's
 * Text_Draw takes a game address, and the host tests' memory is the fake
 * console's, so they are copied in rather than pointed at.
 */
static void put(u32 dst, const char *s)
{
	while (*s != 0) w8(dst++, (u8)*s++);
	w8(dst, 0);
}

#define GLYPH_UP    (SH(SH_GLYPHS))
#define GLYPH_DOWN  (SH(SH_GLYPHS) + 2)
#define GLYPH_LINK  (SH(SH_GLYPHS) + 4)

static void open_menu(u32 from_play)
{
	put(SH(SH_HINT), "X Select   Triangle Back   Start Close");
	put(GLYPH_UP, "^");
	put(GLYPH_DOWN, "v");
	put(GLYPH_LINK, ">");
	w32(SH(SH_UP), 1);
	w32(SH(SH_OPENS), r32(SH(SH_OPENS)) + 1);
	w32(SH(SH_PRESSES), 0);
	w8(SH(SH_REPEAT), 0);
	w8(SH(SH_FROZEN), from_play);
	if (from_play) w32(SH(SH_SCALE), r32(TM_CLOCK));
	mh_count(ST_OPENS);
	mh_core_start(P);
	w8(SH(SH_CURSOR), mh_core_cursor(r8(ST(ST_LAST_CURSOR))));
}

/* The menu gone, by the core's close or the game's: play's time scale back, the pad the game's. */
static void finish(void)
{
	mh_core_stop(P);
	w32(SH(SH_UP), 0);
	if (r8(SH(SH_FROZEN)) != 0) {
		w32(TM_CLOCK, r32(SH(SH_SCALE)));
		w8(SH(SH_FROZEN), 0);
	}
}

/* ------------------------------------------------------------ the core's calls */

MH_API void shim_show(int p, u32 how)
{
	(void)p;
	(void)how;
	/* A new page puts the cursor where it says; a refresh keeps it, moved onto the page and off a grey row. */
	w8(SH(SH_CURSOR), mh_core_cursor(r8(SH(SH_CURSOR))));
}

MH_API void shim_switch(int p)
{
	/* One drawing for both types: the screen stops and starts again at once. */
	mh_core_stop(p);
	mh_core_start(p);
	w8(SH(SH_CURSOR), mh_core_cursor(r8(ST(ST_LAST_CURSOR))));
}

MH_API void shim_close(int p, u32 how)
{
	(void)p;
	(void)how;
	/* Back from the root and a close alike: the panel goes, the game goes on as it was. */
	finish();
}

MH_API void shim_title(int p)
{
	/* The title is drawn from mh_title() every frame. */
	(void)p;
}

MH_API u32 shim_cursor(int p)
{
	(void)p;
	return r8(SH(SH_CURSOR));
}

MH_API void shim_sound(u32 snd)
{
	/* No sound yet: the game's are Anark's and the HUD's, not reached (docs/tod-menu.md). */
	(void)snd;
}

/* ------------------------------------------------------------------- input */

/* Up or down a row, past grey ones; at an edge the page goes on past, the core slides the window instead. */
static void move(int down)
{
	u32 n = mh_nrows(), c = r8(SH(SH_CURSOR)), pf = r8(ST(ST_PFLAGS)), i;

	for (i = 0; i < n; i++) {
		if (down) {
			if (c + 1 >= n) {
				if ((pf & MBOX_PAGE_MORE_DOWN) != 0) return;
				c = 0;
			} else {
				c++;
			}
		} else {
			if (c == 0) {
				if ((pf & MBOX_PAGE_MORE_UP) != 0) return;
				c = n - 1;
			} else {
				c--;
			}
		}
		if ((mh_row_flags(c) & MROWF_ENABLED) != 0) {
			w8(SH(SH_CURSOR), c);
			return;
		}
	}
}

/* This frame's presses, a held direction repeating; answers what to press, or 0. */
static u32 presses(void)
{
	u32 p = r32(SH(SH_PRESSES)), held = r32(SH(SH_HELD));
	u32 dirs = held & (PADB_UP | PADB_DOWN | PADB_LEFT | PADB_RIGHT);
	u32 rep = r8(SH(SH_REPEAT));

	w32(SH(SH_PRESSES), 0);
	if (dirs == 0) {
		rep = 0;
	} else {
		if (rep < 0xFF) rep++;
		if (rep >= REPEAT_FIRST && (rep - REPEAT_FIRST) % REPEAT_EVERY == 0) p |= dirs;
		if (rep >= REPEAT_FIRST + 8 * REPEAT_EVERY) rep -= 4 * REPEAT_EVERY;
	}
	w8(SH(SH_REPEAT), rep);
	return p;
}

static void input(void)
{
	u32 p = presses(), cur;

	if ((p & (PADB_START | PADB_SELECT)) != 0) {
		mh_core_press(P, MH_BTN_CLOSE, r8(SH(SH_CURSOR)));
		return;
	}
	if ((p & PADB_UP) != 0) move(0);
	if ((p & PADB_DOWN) != 0) move(1);
	cur = r8(SH(SH_CURSOR));
	if ((p & PADB_CROSS) != 0) mh_core_press(P, MH_BTN_CROSS, cur);
	else if ((p & (PADB_TRIANGLE | PADB_CIRCLE)) != 0) mh_core_press(P, MH_BTN_BACK, cur);
	else if ((p & PADB_LEFT) != 0) mh_core_press(P, MH_BTN_LEFT, cur);
	else if ((p & PADB_RIGHT) != 0) mh_core_press(P, MH_BTN_RIGHT, cur);
	else if ((p & PADB_L1) != 0) mh_core_press(P, MH_BTN_L1, cur);
	else if ((p & PADB_R1) != 0) mh_core_press(P, MH_BTN_R1, cur);
}

/* ------------------------------------------------------------------- drawing */

static void draw_box(float x0, float y0, float x1, float y1, u32 colour)
{
	u32 s = SH(SH_SPRITE), k;

	sprite_init(s);
	for (k = 0; k < 16; k += 4) w32(s + SPR_COLOURS + k, colour);
	wf32(SH(SH_RECT), x0);
	wf32(SH(SH_RECT) + 4, y0);
	wf32(SH(SH_RECT) + 8, x1);
	wf32(SH(SH_RECT) + 12, y1);
	sprite_draw(SH(SH_RECT), s);
}

/* `str` at (x, y), its left edge (align 0), centre (0.5) or right edge (1) there, its top at y. */
static void draw_text(u32 str, float x, float y, float align, float size, u32 colour)
{
	float off = size * 0.06f;

	wf32(SH(SH_POS), x);
	wf32(SH(SH_POS) + 4, y);
	w32(SH(SH_POS) + 8, 0);
	w32(SH(SH_POS) + 12, 0);
	text_draw(str, SH(SH_POS), 0, colour, 0, TM_FONT, 0, 0, COL_SHADOW,
	          size, 0.0f, align, 0.0f, off, off, -1.0f);
}

static void draw(void)
{
	float x0 = rf32(TM_SCREEN_RECT), y0 = rf32(TM_SCREEN_RECT + 4);
	float w = rf32(TM_SCREEN_RECT + 8) - x0, h = rf32(TM_SCREEN_RECT + 12) - y0;
	float line, size, fh, left, right, top, pad, y, ty;
	u32 n = mh_nrows(), cur = r8(SH(SH_CURSOR)), cfg, pf = r8(ST(ST_PFLAGS)), i;

	/* No screen yet (or a nonsense one): nothing to draw on. */
	if (!(w > 64.0f && w < 16384.0f && h > 64.0f && h < 16384.0f)) return;

	cfg = r8(mh_set() + SET_PTYPE) == MBOX_PTYPE_CONFIG;
	line = h * (float)LOOK_LINE / 1000.0f;
	pad = h * (float)LOOK_PAD / 1000.0f;
	left = x0 + w * (float)LOOK_LEFT / 1000.0f;
	right = x0 + w * (float)LOOK_RIGHT / 1000.0f;
	top = y0 + h * (float)LOOK_TOP / 1000.0f;

	/* The HUD's alpha back to whole, as the game's own full-screen draws start; the font, and its size. */
	hud_reset_alpha();
	font_select(TM_FONT, 0);
	fh = font_height();
	if (!(fh > 0.001f && fh < 1000.0f)) fh = 1.0f;
	size = h * (float)LOOK_TEXT / 1000.0f / fh;
	ty = (line - h * (float)LOOK_TEXT / 1000.0f) * 0.5f;

	draw_box(left, top, right, top + pad * 2.0f + line * (float)(n + 3), COL_PANEL);
	draw_text(mh_title(), (left + right) * 0.5f, top + pad + ty, 0.5f, size, COL_TITLE);
	if ((pf & MBOX_PAGE_MORE_UP) != 0) draw_text(GLYPH_UP, right - pad, top + pad + ty, 1.0f, size, COL_HINT);

	for (i = 0; i < n; i++) {
		u32 flags = mh_row_flags(i), kind = mh_row_kind(i);
		u32 col = (flags & MROWF_ENABLED) != 0 ? COL_TEXT : COL_GREY;

		y = top + pad + line * (float)(i + 1) + line * 0.5f;
		if (i == cur) draw_box(left + pad * 0.5f, y, right - pad * 0.5f, y + line, COL_CURSOR);
		if (cfg) {
			u32 vcol = col;

			if ((flags & MROWF_ON) != 0 && (flags & MROWF_ENABLED) != 0 &&
			    (kind == MROW_TOGGLE || kind == MROW_OPTION))
				vcol = COL_ON;
			draw_text(mh_label(i), left + pad, y + ty, 0.0f, size, col);
			if (r8(mh_value(i)) != 0) draw_text(mh_value(i), right - pad, y + ty, 1.0f, size, vcol);
			else if (kind == MROW_LINK) draw_text(GLYPH_LINK, right - pad, y + ty, 1.0f, size, col);
		} else {
			draw_text(mh_text(i), left + pad, y + ty, 0.0f, size,
			     (flags & MROWF_ON) != 0 && kind == MROW_OPTION ? COL_ON : col);
			if (kind == MROW_LINK) draw_text(GLYPH_LINK, right - pad, y + ty, 1.0f, size, col);
		}
	}

	y = top + pad + line * (float)(n + 1) + line * 0.5f;
	if ((pf & MBOX_PAGE_MORE_DOWN) != 0) draw_text(GLYPH_DOWN, right - pad, y + ty, 1.0f, size, COL_HINT);
	draw_text(SH(SH_HINT), (left + right) * 0.5f, y + line * 0.5f + ty, 0.5f, size * 0.8f, COL_HINT);
	w32(SH(SH_FRAMES), r32(SH(SH_FRAMES)) + 1);
}

/* ------------------------------------------------------------- the entries */

MH_API void mh_frame(void)
{
	u32 state = r32(TM_STATE);
	u32 playing = state == TM_STATE_PLAY || state == TM_STATE_PAUSE;

	if (!is_up()) {
		/*
		 * An open request is taken in play or in the game's own pause, with
		 * no change of state queued; anywhere else it is left, and qwark
		 * withdraws it half a second on, so the menu never opens later by
		 * itself. A close request with the menu down has nothing to close.
		 */
		if (r8(BOX(MBOX_OFF_REQUEST)) != MBOX_REQ_OPEN || !playing || r32(TM_PENDING) != 0) return;
		if (!mh_core_open_request()) return;
		open_menu(state == TM_STATE_PLAY);
	} else if (!playing) {
		/* A load, a cinematic or the credits took the game: the menu goes with no word of its own. */
		finish();
	}

	if (is_up()) {
		if (r8(SH(SH_FROZEN)) != 0) w32(TM_CLOCK, 0);
		if (mh_core_frame(P) != MH_GONE && is_up()) input();
		if (is_up()) draw();
	}
	/*
	 * The pads were blanked this frame, the menu up or closing in it: pad 0's
	 * held mask back, so the game's next read takes no press for a button
	 * that was down all along (the Start that closed the menu, the combo),
	 * and qwark's combos see the pad.
	 */
	if (r8(SH(SH_BLANKED)) != 0) {
		w32(PAD(0, TM_PAD_HELD), r32(SH(SH_HELD)));
		w8(SH(SH_BLANKED), 0);
	}
}

MH_API void mh_pad(void)
{
	u32 held = r32(PAD(0, TM_PAD_HELD)), i, k;

	w32(SH(SH_PRESSES), r32(SH(SH_PRESSES)) | (held & ~r32(SH(SH_PREV))));
	w32(SH(SH_PREV), held);
	w32(SH(SH_HELD), held);
	if (!is_up()) return;
	for (i = 0; i < TM_PADS; i++) {
		for (k = 0; k <= TM_PAD_BLANK; k += 4) w32(PAD(i, k), 0);
	}
	w8(SH(SH_BLANKED), 1);
}
