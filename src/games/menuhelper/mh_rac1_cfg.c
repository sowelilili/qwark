/*
 * Ratchet & Clank's shim, the CONFIG widget: a page of labels and controls in
 * Goodies > Cheats' layout (mh_rac1.h). The page is one of the shim's two
 * CONFIG pages (mh_rac1.c sets them up on Cheats' poses, with a copy of its
 * title item and its hint); this file is the list item in its large pane: its
 * update, which reads the input as Cheats' list (0x14A460) and Camera Options'
 * (0x14AB64) do and hands the core the presses, and its draw, the geometry of
 * Cheats' list draw (0x14A86C) with Camera Options' 3D slider (0x14AD88), on a
 * window of the page's rows. The text helpers both widgets use (the copy, the
 * width, the cut) are here too.
 *
 * A file of its own to keep the shim's two widgets apart; build.sh spreads
 * both files' functions over RaC1's code areas, as no one run of code nothing
 * reaches holds the whole renderer.
 */
#include "mh_common.h"
#include "mh_shim.h"
#include "mh_rac1_shim.h"

typedef void (*fn_text)(float, float, u32, u32, int, float);
typedef void (*fn_rtext)(int, int, u32, u32, int, float);
typedef void (*fn_box)(int, int, int, int, u32, int);
typedef int  (*fn_i_ii)(int, int);
typedef void (*fn_sprite)(int, int, int, int, int, int, int, int, int, int);
typedef unsigned long long (*fn_blend)(int, int, int, int, int, int, int, int);
typedef void (*fn_gsreg)(int, unsigned long long);

#define draw_text     GAME_FN(fn_text,   OPD_DRAW_TEXT,    mock_draw_text)
#define draw_right    GAME_FN(fn_rtext,  OPD_DRAW_RIGHT,   mock_draw_right)
#define draw_box      GAME_FN(fn_box,    OPD_BOX,          mock_draw_box)
#define sprite_index  GAME_FN(fn_i_ii,   OPD_SPRITE_INDEX, mock_sprite_index)
#define draw_sprite   GAME_FN(fn_sprite, OPD_SPRITE,       mock_draw_sprite)
#define blend         GAME_FN(fn_blend,  OPD_BLEND,        mock_blend)
#define gs_reg        GAME_FN(fn_gsreg,  OPD_GS_REG,       mock_gs_reg)

/* ------------------------------------------- the text, for both widgets */

/* A string of the game's memory into a buffer of the shim's, terminated. */
R1_FN void r1_copy_str(u32 dst, u32 src, u32 cap)
{
	u32 i, c;

	for (i = 0; i + 1 < cap; i++) {
		c = r8(src + i);
		if (c == 0) break;
		w8(dst + i, c);
	}
	w8(dst + i, 0);
}

/* A string's width in one of the game's fonts ({.., .., .., s8 width} a glyph), as its draws measure it. */
R1_FN int r1_text_w(u32 font, u32 s)
{
	int w = 0;
	u32 c;

	while ((c = r8(s)) != 0) {
		w += (int)(signed char)r8(font + 4 * c + 3);
		s++;
	}
	return w;
}

/*
 * Cut the string at `s` (a buffer of the shim's) so that it and ".." take at
 * most `w` at `scale` in `font`. Answers 1 if it cut.
 */
R1_FN int r1_cut(u32 font, u32 s, int w, float scale)
{
	int n = 0, dots = 2 * (int)(signed char)r8(font + 4 * '.' + 3);

	if ((float)r1_text_w(font, s) * scale <= (float)w) return 0;
	while (r8(s + (u32)n) != 0) n++;
	while (n > 0) {
		w8(s + (u32)--n, 0);
		if ((float)(r1_text_w(font, s) + dots) * scale <= (float)w) break;
	}
	w8(s + (u32)n, '.');
	w8(s + (u32)n + 1, '.');
	w8(s + (u32)n + 2, 0);
	w32(SH(SH_CUTS), r32(SH(SH_CUTS)) + 1);
	return 1;
}

/* -------------------------------------------------- the CONFIG list */

/*
 * Rows the list pane holds at R1_CFG_ROW_H or more each, as the pane is now
 * (the render writes its rect every frame), and no more than the page has:
 * Cheats' list pane, which the game fills with Cinematics' 29 rows, should
 * hold well over eight.
 */
static u32 visible(void)
{
	int h = (int)r32(LIST + IT_H);
	u32 v = R1_CFG_VIS, n = mh_nrows();

	if (h > 0) {
		v = (u32)h / R1_CFG_ROW_H;
		v = v > 2 ? v - 1 : 1;
	}
	return v < n ? v : n;
}

/* The window over the page's rows keeps the cursor in it. */
R1_FN void r1_keep_visible(void)
{
	u32 v = visible(), n = mh_nrows(), cur = r8(SH(SH_CUR)), top = r8(SH(SH_TOP));

	if (cur >= n) cur = n - 1;
	if (cur < top) top = cur;
	if (cur >= top + v) top = cur - v + 1;
	if (top + v > n) top = n - v;
	w8(SH(SH_CUR), cur);
	w8(SH(SH_TOP), top);
	w8(SH(SH_VIS), v);
}

/* The CONFIG page the core just started: the cursor, the window, the title. */
R1_FN void r1_conf_start(void)
{
	w8(SH(SH_CUR), mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)));
	w8(SH(SH_TOP), 0);
	r1_keep_visible();
	r1_copy_str(SH(SH_TITLE_TEXT), mh_title(), MH_TITLE_LEN);
	r1_goodies_pane(0);
}

/*
 * Slider rows: a VALUE row with bounds whose span is short enough for a bar
 * (MROWF_RANGE and MROWF_SLIDER), with its place along the span as num / den:
 * (value - lowest) and (highest - lowest), modulo 2^32, whatever the field's
 * sign (mh_shim.h).
 */
static int slider_of(u32 i, u32 *num, u32 *den)
{
	u32 r;

	if ((mh_row_flags(i) & (MROWF_RANGE | MROWF_SLIDER)) != (MROWF_RANGE | MROWF_SLIDER)) return 0;
	r = mh_row_range(i);
	*num = r32(r) - r32(r + 4);
	*den = r32(r + 8) - r32(r + 4);
	return 1;
}

/* Up or Down to the next row the cursor may land on, no wrap, as Cheats' list moves. */
static int conf_move(int down)
{
	u32 n = mh_nrows(), i = r8(SH(SH_CUR));

	for (;;) {
		if (down ? i + 1 >= n : i == 0) return 0;
		i = down ? i + 1 : i - 1;
		if (r1_enabled(i)) break;
	}
	w8(SH(SH_CUR), i);
	r1_keep_visible();
	return 1;
}

/*
 * The CONFIG list, every frame its page is current: the core's frame, then one
 * press as Cheats' list reads them (Up and Down with the menus' auto-repeat,
 * no wrap, a row that is not ENABLED skipped, the cursor's tick),
 * the cross, Left, Right, L1, R1 and Triangle the core's, Start, Select and R3
 * the close (none on a menu the game marks unclosable).
 */
MH_API int mh_conf_update(u32 item)
{
	u32 e, rep, b;

	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC || r1_arrive() != AT_CONF) return 0;
	if (r32(PAGE_CONF(r8(SH(SH_CONF))) + PG_FOCUS) != item) return 0;
	if (r1_closing()) return 1;
	if (r1_phase() != PH_IDLE || r32(R1_MENU_CLOSE) != 0) return 0;
	if (mh_core_frame(0) == MH_GONE) return r1_closing();

	e = r32(R1_PAD_EDGES);
	if ((e & PADB_CLOSE) != 0 && r32(R1_UNCLOSABLE) == 0) {
		b = MH_BTN_CLOSE;
	} else if ((e & PADB_TRIANGLE) != 0) {
		b = MH_BTN_BACK;
	} else {
		rep = e | (r32(R1_PAD_REPEAT) & (PADB_UP | PADB_DOWN));
		if ((rep & (PADB_UP | PADB_DOWN)) != 0) {
			if (conf_move((rep & PADB_DOWN) != 0)) r1_sound(SND_CURSOR, r32(LIST + IT_MOBY));
			return 0;
		}
		b = (e & r32(R1_CONFIRM_MASK)) != 0 ? MH_BTN_CROSS : r1_steps(e);
	}
	if (b != 0) mh_core_press(0, b, r8(SH(SH_CUR)));
	return r1_closing();
}

/*
 * The CONFIG list's draw, into its pane: Cheats' (0x14A86C) on the shim's
 * window of rows. The blend Cheats' and Camera Options' draws set; the medium
 * font at scale 1.0, as Cheats' rows in every language; the rows sharing the
 * pane's height, the first at h / (n + 1) - 8; the label at 12, cut short so
 * that it ends 10 before its value, the value right-aligned at w - 12; the
 * cursor row in the game's cursor colour, a row that is not ENABLED in the
 * generic list's grey; a slider row as Camera Options' 3D depth slider (frame,
 * track, fill sprite), its value before the bar. Answers 2, both draws'
 * answer: the pane takes the drawing as it is.
 */
MH_API int mh_conf_draw(u32 item)
{
	int w = (int)r32(item + IT_W), h = (int)r32(item + IT_H), H, y, x, k, slider;
	u32 vis, top, cur, i, colour, num = 0, den = 1, cb = SH(SH_CUTBUF), v;
	const float scale = 1.0f;

	gs_reg(22, blend(1, 5, 4, 0, 0, 0, 0, 1));
	w16(SH(SH_CONF_W), (u32)w);
	w16(SH(SH_CONF_H), (u32)h);
	r1_keep_visible();
	vis = r8(SH(SH_VIS));
	top = r8(SH(SH_TOP));
	cur = r8(SH(SH_CUR));
	if (vis == 0 || w <= 0 || h <= 0) return 2;

	H = h / (int)(vis + 1);
	for (k = 0, y = H - 8; k < (int)vis; k++, y += H) {
		i = top + (u32)k;
		v = mh_value(i);
		colour = i == cur ? R1_COLOR_CURSOR : r1_enabled(i) ? R1_COLOR_TEXT : R1_COLOR_GREY;
		slider = slider_of(i, &num, &den);
		x = slider ? w / 2 + 80 : w - 12;
		r1_copy_str(cb, mh_label(i), R1_CUTBUF_LEN);
		if (r8(R1_JAPANESE) == 0)
			r1_cut(R1_FONT_MEDIUM, cb, x - (int)((float)r1_text_w(R1_FONT_MEDIUM, v) * scale) - 10 - 12, scale);
		if (!slider) {
			draw_text(12.0f, (float)y, colour, cb, -1, scale);
			draw_right(x, y, colour, v, -1, scale);
			continue;
		}
		draw_text(12.0f, (float)(y + 4), colour, cb, -1, scale);
		draw_right(x, y + 4, colour, v, -1, scale);
		draw_box(w / 2 + 87, y + 4, w - 10, y + 18, R1_COLOR_FRAME, 0);
		draw_box(w / 2 + 89, y + 5, w - 12, y + 15, R1_COLOR_TRACK, 0);
		if (den == 0) den = 1;
		if (num > den) num = den;
		x = (int)((u32)(w - w / 2 - 101) * num / den);
		draw_sprite(sprite_index(R1_SLIDER_SPRITE, 8), w / 2 + 89, y + 5, w / 2 + 88 + x, y + 14, 0, 19, 53, 43, 128);
	}
	return 2;
}
