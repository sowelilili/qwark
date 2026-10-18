/*
 * The console menu's renderer core: the half of a renderer that is the same
 * for every game (mh_shim.h has the interface, src/core/menubox.h the mailbox,
 * src/core/menu.c the other end in qwark).
 *
 * It decides nothing about the menu: qwark posts pages, the core copies the
 * one qwark finished writing into buffers that stay put, composes the text the
 * game's widgets will show, and posts back what the player pressed on which
 * row. The game's shim (mh_<game>.c) shows those buffers with the game's own
 * widgets and hands the core the presses. The core names no game address and
 * calls no game function: every access goes through r8..w32 (mh_common.h), on
 * the mailbox at MH_BOX and the state at MH_STATE, so the host tests run it on
 * qwark's fake console against a fake shim and against each game's shim.
 *
 * Built once per game with the game's header included first (build.sh), so
 * the sizes and the casing are that game's.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define BOX(o)   ((u32)MH_BOX + (u32)(o))
#define ST(o)    ((u32)MH_STATE + (u32)(o))
/* A mailbox offset from MBOX_OFF_PAGE_KEY on, in the core's copy of the page. */
#define STAGE(o) (ST(ST_STAGE) + (u32)(o) - MBOX_OFF_PAGE_KEY)
/* Row i's first 0x40 bytes in that copy. */
#define SROW(i)  (STAGE(MBOX_OFF_ROWS) + (u32)(i) * MBOX_ROW_SIZE)

/* ------------------------------------------------------------- the text */

/*
 * Game bytes from `src` (at most `len`, up to a NUL) to `dst`, never at or past
 * `end - 1`, cased `how` (MH_CASE_*). Plain ASCII only: the games' fonts read
 * low bytes as button glyphs and high ones as the first of a double-byte
 * glyph, so anything outside 0x20..0x7E shows as a question mark instead.
 */
static u32 put_mem(u32 dst, u32 end, u32 src, u32 len, u32 how)
{
	u32 i, word = 1;

	for (i = 0; i < len && dst < end - 1; i++) {
		u32 c = r8(src + i);
		if (c == 0) break;
		if (c < 0x20 || c > 0x7E) c = '?';
		if (c >= 'a' && c <= 'z' && (how == MH_CASE_UPPER || (how == MH_CASE_TITLE && word)))
			c -= 0x20;
		word = c == ' ';
		w8(dst, c);
		dst++;
	}
	return dst;
}

/* The same from one of the core's own strings. */
static u32 put_lit(u32 dst, u32 end, const char *s, u32 how)
{
	while (*s != 0 && dst < end - 1) {
		u32 c = (u8)*s;
		if (c >= 'a' && c <= 'z' && how == MH_CASE_UPPER) c -= 0x20;
		w8(dst, c);
		dst++;
		s++;
	}
	return dst;
}

static u32 nrows(void)
{
	return r8(ST(ST_NROWS));
}

/*
 * Row i of the copied page as the shim will show it, by the type it is drawn
 * with: a MAIN row is one string, the label and then the value after
 * MH_MAIN_SEP; a CONFIG row is the label and the value apart. The value has
 * its decorations (MH_STYLE).
 */
static void compose(u32 i)
{
	u32 src = SROW(i);
	u32 kind = r8(src + MROW_OFF_KIND);
	u32 flags = r8(src + MROW_OFF_FLAGS);
	u32 text = ST(ST_TEXT) + i * MH_TEXT_LEN;
	u32 main = r8(ST(ST_PTYPE)) == MBOX_PTYPE_MAIN;
	u32 vcase = main ? MH_CASE_MAIN : MH_CASE_VALUE;
	u32 p, end, v, has = r8(src + MROW_OFF_VALUE) != 0;

	w8(ST(ST_KIND) + i, kind);
	w8(ST(ST_FLAGS) + i, flags);
	w16(ST(ST_ID) + 2 * i, r16(src + MROW_OFF_ID));

	if (main) {
		end = text + MH_TEXT_LEN;
		p = put_mem(text, end, src + MROW_OFF_LABEL, MROW_LABEL_LEN, MH_CASE_MAIN);
		if ((MH_STYLE & MH_STYLE_STAR) != 0 && kind == MROW_OPTION && (flags & MROWF_ON) != 0)
			p = put_lit(p, end, " *", 0);
		if (has || ((MH_STYLE & MH_STYLE_LINKS) != 0 && kind == MROW_LINK))
			p = put_lit(p, end, MH_MAIN_SEP, 0);
		v = p;
	} else {
		w8(put_mem(text, text + MH_LABEL_LEN, src + MROW_OFF_LABEL, MROW_LABEL_LEN, MH_CASE_LABEL), 0);
		v = text + MH_LABEL_LEN;
		end = v + MH_VALUE_LEN;
	}

	p = v;
	if ((MH_STYLE & MH_STYLE_ARROWS) != 0 && kind == MROW_CHOICE && has) p = put_lit(p, end, "< ", 0);
	p = put_mem(p, end, src + MROW_OFF_VALUE, MROW_VALUE_LEN, vcase);
	if ((MH_STYLE & MH_STYLE_ARROWS) != 0 && kind == MROW_CHOICE && has) p = put_lit(p, end, " >", 0);
	if (!has && (MH_STYLE & MH_STYLE_LINKS) != 0 && kind == MROW_LINK) p = put_lit(p, end, ">", 0);
	if (!has && !main && (MH_STYLE & MH_STYLE_DOTS) != 0 && kind == MROW_ACTION) p = put_lit(p, end, "...", 0);
	w8(p, 0);
}

/* The title, from the copied page; or the core's own while qwark is silent. */
static void title(void)
{
	u32 t = ST(ST_TITLE), how = r8(ST(ST_PTYPE)) == MBOX_PTYPE_MAIN ? MH_CASE_MAIN : MH_CASE_HEAD;

	if (r8(ST(ST_WAITING)) != 0)
		w8(put_lit(t, t + MH_TITLE_LEN, "Waiting for qwark...", how), 0);
	else
		w8(put_mem(t, t + MH_TITLE_LEN, STAGE(MBOX_OFF_TITLE), MBOX_TITLE_LEN, how), 0);
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
	u32 n, i, key, type;

	if (s1 == 0 || (s1 & 1) != 0) return 0;
	if (r8(ST(ST_HAVE_PAGE)) != 0 && s1 == r32(ST(ST_SHOWN_SEQ))) return 0;

	mh_sync();
	n = r8(BOX(MBOX_OFF_NROWS));
	if (n > MH_ROWS) n = MH_ROWS;
	for (i = 0; i < MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY; i += 4)
		w32(STAGE(MBOX_OFF_PAGE_KEY) + i, r32(BOX(MBOX_OFF_PAGE_KEY) + i));
	for (i = 0; i < n * MBOX_ROW_SIZE; i += 4)
		w32(SROW(0) + i, r32(BOX(MBOX_OFF_ROWS) + (i / MBOX_ROW_SIZE) * MH_ROW_BYTES + i % MBOX_ROW_SIZE));
	mh_sync();

	if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != s1) {
		mh_count(ST_RETRIES);
		return 0;
	}

	/* A page of no rows is none qwark posts; show one blank row rather than divide by 0. */
	if (n == 0) {
		n = 1;
		for (i = 0; i < MBOX_ROW_SIZE; i += 4) w32(SROW(0) + i, 0);
	}

	/* A shim without the widget a page asks for draws it as a MAIN page. */
	type = r8(STAGE(MBOX_OFF_PTYPE));
	if (type != MBOX_PTYPE_CONFIG || MH_CFG_ROWS == 0) type = MBOX_PTYPE_MAIN;
	w8(ST(ST_PTYPE), type);

	key = r32(STAGE(MBOX_OFF_PAGE_KEY));
	w8(ST(ST_NEW_KEY), r8(ST(ST_HAVE_PAGE)) == 0 || key != r32(ST(ST_SHOWN_KEY)) ||
	                   r8(ST(ST_NEW_KEY)) != 0);
	w32(ST(ST_SHOWN_SEQ), s1);
	w32(ST(ST_SHOWN_KEY), key);
	w8(ST(ST_NROWS), n);
	w8(ST(ST_CURSOR_HINT), r8(STAGE(MBOX_OFF_CURSOR)));
	w8(ST(ST_PFLAGS), r8(STAGE(MBOX_OFF_PFLAGS)));
	w8(ST(ST_DESC_ROW), MH_NONE);

	for (i = 0; i < n; i++) compose(i);
	title();

	w8(ST(ST_HAVE_PAGE), 1);
	mh_count(ST_PAGES);
	return 1;
}

/* Before qwark has posted anything: one grey row, and back leaves. */
static void placeholder(void)
{
	u32 i;

	for (i = 0; i < MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + MBOX_ROW_SIZE; i += 4)
		w32(STAGE(MBOX_OFF_PAGE_KEY) + i, 0);
	put_lit(STAGE(MBOX_OFF_TITLE), STAGE(MBOX_OFF_TITLE) + MBOX_TITLE_LEN, "RaCMAN", 0);
	put_lit(SROW(0) + MROW_OFF_LABEL, SROW(0) + MROW_OFF_LABEL + MROW_LABEL_LEN, "Waiting for qwark", 0);
	w8(ST(ST_PTYPE), MBOX_PTYPE_MAIN);
	w8(ST(ST_NROWS), 1);
	w8(ST(ST_CURSOR_HINT), 0);
	w8(ST(ST_PFLAGS), MBOX_PAGE_ROOT);
	compose(0);
	title();
}

/* `c` moved onto the page, and off a row the cursor cannot land on. */
static u32 pick_cursor(u32 c)
{
	u32 n = nrows();
	u32 i;

	if (c >= n) c = n - 1;
	if ((r8(ST(ST_FLAGS) + c) & MROWF_ENABLED) != 0) return c;
	for (i = 0; i < n; i++) {
		if ((r8(ST(ST_FLAGS) + i) & MROWF_ENABLED) != 0) return i;
	}
	return c;
}

MH_API u32 mh_core_cursor(u32 keep)
{
	u32 c = r8(ST(ST_NEW_KEY)) != 0 ? r8(ST(ST_CURSOR_HINT)) : keep;

	w8(ST(ST_NEW_KEY), 0);
	return pick_cursor(c);
}

/*
 * The description line: the page's hint while it has one (the last answer, as
 * a sentence), else the cursor row's description, read out of the mailbox
 * only while the page shown is still the one there. Answers 1 when it changed.
 */
static int describe(int p)
{
	u32 row = shim_cursor(p), d = ST(ST_DESC), i, seq;

	if (MH_ROW_BYTES < MBOX_ROW_SIZE_DESC || row == r8(ST(ST_DESC_ROW))) return 0;

	if (r8(STAGE(MBOX_OFF_HINT)) != 0) {
		w8(put_mem(d, d + MH_DESC_LEN, STAGE(MBOX_OFF_HINT), MBOX_HINT_LEN, 0), 0);
	} else if (row < nrows()) {
		seq = r32(ST(ST_SHOWN_SEQ));
		if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != seq) return 0;
		mh_sync();
		for (i = 0; i < MROW_DESC_LEN; i += 4)
			w32(ST(ST_DSTAGE) + i, r32(BOX(MBOX_OFF_ROWS) + row * MH_ROW_BYTES + MROW_OFF_DESC + i));
		mh_sync();
		if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != seq) return 0;
		w8(put_mem(d, d + MH_DESC_LEN, ST(ST_DSTAGE), MROW_DESC_LEN, 0), 0);
	} else {
		w8(d, 0);
	}
	w8(ST(ST_DESC_ROW), row);
	return 1;
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
		mh_count(ST_DROPPED);
		return 0;
	}

	w8(BOX(MBOX_OFF_EV_KIND), kind);
	w8(BOX(MBOX_OFF_EV_ROW), row);
	w16(BOX(MBOX_OFF_EV_ID), row < nrows() ? r16(ST(ST_ID) + 2 * row) : 0);
	w32(BOX(MBOX_OFF_EV_KEY), r32(ST(ST_SHOWN_KEY)));
	mh_sync();
	w32(BOX(MBOX_OFF_EV_SEQ), seq + 1);

	w32(ST(ST_BUSY_SINCE), r32(BOX(MBOX_OFF_FRAMES)));
	mh_count(ST_EVENTS);
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

MH_API void mh_core_press(int p, u32 button, u32 row)
{
	u32 flags = row < nrows() ? r8(ST(ST_FLAGS) + row) : 0;

	switch (button) {
	case MH_BTN_CROSS:
		if ((flags & MROWF_ENABLED) != 0 && post(MBOX_EV_ACTIVATE, row)) shim_sound(MH_SND_FORWARD);
		return;
	case MH_BTN_LEFT:
	case MH_BTN_RIGHT:
		if ((flags & MROWF_STEPS) != 0 &&
		    post(button == MH_BTN_LEFT ? MBOX_EV_LEFT : MBOX_EV_RIGHT, row))
			shim_sound(MH_SND_STEP);
		return;
	case MH_BTN_L1:
	case MH_BTN_R1:
		if (has_kind(button == MH_BTN_L1 ? MROW_PREV : MROW_MORE) &&
		    post(button == MH_BTN_L1 ? MBOX_EV_PAGE_PREV : MBOX_EV_PAGE_NEXT, row))
			shim_sound(MH_SND_MOVE);
		return;
	case MH_BTN_BACK:
		/* At the root, and before any page, back is the game's own, which leaves. */
		if (r8(ST(ST_HAVE_PAGE)) == 0 || (r8(ST(ST_PFLAGS)) & MBOX_PAGE_ROOT) != 0)
			shim_close(p, MH_CLOSE_BACK);
		else if (post(MBOX_EV_BACK, row))
			shim_sound(MH_SND_BACK);
		return;
	case MH_BTN_CLOSE:
		shim_close(p, MH_CLOSE_EXIT);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------- the screen's life */

MH_API int mh_core_start(int p)
{
	(void)p;
	if (r32(ST(ST_MAGIC)) != MH_MAGIC) w32(ST(ST_MAGIC), MH_MAGIC);
	mh_count(ST_STARTS);
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	w8(ST(ST_WAITING), 0);
	w8(ST(ST_SWITCHING), 0);

	if (!take_page()) {
		if (r8(ST(ST_HAVE_PAGE)) == 0) placeholder();
		else title();
	}
	w8(ST(ST_DESC_ROW), MH_NONE);
	w8(ST(ST_SHOWN), r8(ST(ST_PTYPE)));
	return (int)r8(ST(ST_PTYPE));
}

MH_API void mh_core_stop(int p)
{
	w8(ST(ST_LAST_CURSOR), shim_cursor(p));
	w8(ST(ST_SHOWN), MH_NONE);
	if (r8(ST(ST_SWITCHING)) != 0) return;
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_CLOSED);
	w8(ST(ST_WAITING), 0);
}

MH_API int mh_core_frame(int p)
{
	u32 req, old_n, frames, ev, ack;

	frames = r32(BOX(MBOX_OFF_FRAMES)) + 1;
	w32(BOX(MBOX_OFF_FRAMES), frames);
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);

	req = r8(BOX(MBOX_OFF_REQUEST));
	if (req != MBOX_REQ_NONE) {
		w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
		if (req == MBOX_REQ_CLOSE) {
			shim_close(p, MH_CLOSE_EXIT);
			return MH_GONE;
		}
	}

	old_n = nrows();
	if (take_page()) {
		w8(ST(ST_WAITING), 0);
		title();
		if (r8(ST(ST_PTYPE)) != r8(ST(ST_SHOWN))) {
			/* The other widget: the shim starts its screen again, through mh_core_start. */
			w8(ST(ST_SWITCHING), 1);
			shim_switch(p);
			return MH_GONE;
		}
		shim_show(p, (r8(ST(ST_NEW_KEY)) != 0 ? MH_SHOW_NEW : 0) | (nrows() != old_n ? MH_SHOW_ROWS : 0));
	} else {
		ev = r32(BOX(MBOX_OFF_EV_SEQ));
		ack = r32(BOX(MBOX_OFF_EV_ACK));
		if (r8(ST(ST_WAITING)) != 0) {
			if (ev == ack) {
				w8(ST(ST_WAITING), 0);
				title();
				shim_title(p);
			}
		} else if (ev != ack && frames - r32(ST(ST_BUSY_SINCE)) >= MH_WAIT_FRAMES) {
			w8(ST(ST_WAITING), 1);
			title();
			shim_title(p);
		}
	}

	if (describe(p)) shim_desc(p);
	return 0;
}

MH_API u32 mh_core_open_request(void)
{
	if (r8(BOX(MBOX_OFF_REQUEST)) != MBOX_REQ_OPEN) return 0;
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	mh_count(ST_OPEN_REQS);
	return 1;
}
