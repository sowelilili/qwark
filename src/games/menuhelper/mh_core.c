/*
 * The console menu's renderer core: the half of a renderer that is the same
 * for every game (mh_shim.h has the interface, src/core/menubox.h the mailbox,
 * src/core/menu.c the other end in qwark).
 *
 * It decides nothing about the menu: qwark posts pages, the core composes the
 * one qwark finished writing into a text set of its own, which the game's
 * widgets show, and posts back what the player pressed on which row. The
 * game's shim (mh_<game>.c) shows those sets with the game's own widgets and
 * hands the core the presses. The core names no game address and calls no
 * game function: every access goes through r8..w32 (mh_common.h), on the
 * mailbox at MH_BOX and the state at MH_STATE, so the host tests run it on
 * qwark's fake console against a fake shim and against each game's shim.
 *
 * Round 3: two text sets (mh_shim.h says the contract). A page the screen on
 * show takes in place is written into its set, so the new text shows at once;
 * a page that needs the screen started again goes into the other set, and the
 * screen going keeps the old page's text until the new screen has replaced it.
 *
 * Built once per game with the game's header included first (build.sh), so
 * the sizes and the casing are that game's.
 */
#include "mh_common.h"
#include "mh_shim.h"

#define BOX(o)   ((u32)MH_BOX + (u32)(o))
#define ST(o)    ((u32)MH_STATE + (u32)(o))
/* Text set k, and row i of the mailbox's page. */
#define SET(k)   (ST(ST_SET0) + (u32)(k) * (u32)MH_SET_SIZE)
#define MROW(i)  (BOX(MBOX_OFF_ROWS) + (u32)(i) * MH_ROW_BYTES)

/* take_page's answers, with shim_show's MH_SHOW_* bits under them. */
#define TAKE_NONE  0x00
#define TAKE_HERE  0x10     /* into the set on show, which the screen takes in place */
#define TAKE_OTHER 0x20     /* into the other set: the screen starts again for it */

/* ------------------------------------------------------------- the sets */

static u32 front(void)
{
	return SET(r8(ST(ST_FRONT)));
}

static u32 other(void)
{
	return SET(r8(ST(ST_FRONT)) ^ 1);
}

static u32 nrows(void)
{
	return r8(front() + SET_NROWS);
}

/* ------------------------------------------------------------- the text */

/* `c` cased `how` (MH_CASE_*), the first letter of a word being one after a space or the start. */
static u32 cased(u32 c, u32 how, u32 word)
{
	if (c >= 'a' && c <= 'z' && (how == MH_CASE_UPPER || (how == MH_CASE_TITLE && word))) c -= 0x20;
	return c;
}

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
		c = cased(c, how, word);
		word = c == ' ';
		w8(dst, c);
		dst++;
	}
	return dst;
}

/* The same from one of the core's own strings. */
static u32 put_lit(u32 dst, u32 end, const char *s, u32 how)
{
	u32 word = 1;

	while (*s != 0 && dst < end - 1) {
		u32 c = cased((u8)*s, how, word);
		word = c == ' ';
		w8(dst, c);
		dst++;
		s++;
	}
	return dst;
}

/*
 * Row i of the mailbox's page into text set `set`, as the shim will show it by
 * the set's type: a MAIN row is one string, the label and then the value after
 * MH_MAIN_SEP; a CONFIG row is the label and the value apart. The value has
 * its decorations (MH_STYLE). A VALUE row with a range keeps its four words in
 * its text, past the label, which qwark holds to 23 characters on such a row.
 */
static void compose(u32 set, u32 i)
{
	u32 src = MROW(i);
	u32 kind = r8(src + MROW_OFF_KIND);
	u32 flags = r8(src + MROW_OFF_FLAGS);
	u32 text = set + SET_TEXT + i * MH_TEXT_LEN;
	u32 main = r8(set + SET_PTYPE) == MBOX_PTYPE_MAIN;
	u32 range = kind == MROW_VALUE && (flags & MROWF_RANGE) != 0 ? (main ? MH_RANGE_MAIN : MH_RANGE_CFG) : 0;
	u32 lim = text + (range != 0 ? range : MH_LABEL_LEN);
	u32 p, end, v, k, lend = 0, has = r8(src + MROW_OFF_VALUE) != 0;

	w8(set + SET_KIND + i, kind);
	w8(set + SET_FLAGS + i, flags);
	w16(set + SET_ID + 2 * i, r16(src + MROW_OFF_ID));

	if (main) {
		end = text + (range != 0 ? range : MH_TEXT_LEN);
		p = put_mem(text, end, src + MROW_OFF_LABEL, MROW_LABEL_LEN, MH_CASE_MAIN);
		if ((MH_STYLE & MH_STYLE_STAR) != 0 && kind == MROW_OPTION && (flags & MROWF_ON) != 0)
			p = put_lit(p, end, " *", 0);
		if (has || ((MH_STYLE & MH_STYLE_LINKS) != 0 && kind == MROW_LINK))
			p = put_lit(p, end, MH_MAIN_SEP, 0);
		v = p;
	} else {
		/* One byte short of the slot: room for MH_STYLE_COLON's colon. */
		lend = put_mem(text, lim - 1, src + MROW_OFF_LABEL, MROW_LABEL_LEN, MH_CASE_LABEL);
		w8(lend, 0);
		v = text + MH_LABEL_LEN;
		end = v + MH_VALUE_LEN;
	}

	p = put_mem(v, end, src + MROW_OFF_VALUE, MROW_VALUE_LEN, main ? MH_CASE_MAIN : MH_CASE_VALUE);
	if (!has && (MH_STYLE & MH_STYLE_LINKS) != 0 && kind == MROW_LINK) p = put_lit(p, end, ">", 0);
	if (!has && !main && (MH_STYLE & MH_STYLE_DOTS) != 0 && kind == MROW_ACTION) p = put_lit(p, end, "...", 0);
	w8(p, 0);
	/* "MUSIC VOLUME:": a CONFIG label with a value beside it (Deadlocked's GAME SETTINGS). */
	if ((MH_STYLE & MH_STYLE_COLON) != 0 && !main && p != v && lend != text)
		w8(put_lit(lend, lim, ":", 0), 0);

	if (range != 0) {
		for (k = 0; k < 16; k += 4) w32(text + range + k, r32(src + MROW_OFF_RANGE + k));
	}
}

/* How a title is cased in `set`. */
static u32 title_case(u32 set)
{
	return r8(set + SET_PTYPE) == MBOX_PTYPE_MAIN ? MH_CASE_MAIN : MH_CASE_HEAD;
}

/* The title of the mailbox's page into `set`. */
static void title(u32 set)
{
	u32 t = set + SET_TITLE;

	w8(put_mem(t, t + MH_TITLE_LEN, BOX(MBOX_OFF_TITLE), MBOX_TITLE_LEN, title_case(set)), 0);
}

/* The core's own title on the set on show, while qwark is silent. */
static void waiting(void)
{
	u32 t = front() + SET_TITLE;

	w8(ST(ST_WAITING), 1);
	w8(put_lit(t, t + MH_TITLE_LEN, "Waiting for qwark...", title_case(front())), 0);
}

/*
 * The page's own title back on the set on show, while the mailbox still holds
 * that page; answers 0, nothing written, when it holds another by now (which
 * the next take brings).
 */
static int retitle(void)
{
	u32 seq = r32(ST(ST_SHOWN_SEQ));

	if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != seq) return 0;
	mh_sync();
	title(front());
	mh_sync();
	return r32(BOX(MBOX_OFF_PAGE_SEQ)) == seq;
}

/*
 * Takes the page qwark last posted, if there is one this renderer has not
 * taken. page_seq is a seqlock, odd while qwark is writing: the page is
 * composed straight out of the mailbox and kept only if page_seq read the same
 * even number before and after; a page caught half written is left for the
 * next frame.
 *
 * Where it goes. While the menu's screen starts (`starting`), into the set on
 * show, which nothing shows yet. Otherwise by what the screen can do with it:
 * a page of the other type, or one the widget cannot take in place
 * (MH_RESTART_*), into the other set (TAKE_OTHER), unless that set is still on
 * show by a screen going (ST_HOLD), when it waits in the mailbox; a page the
 * screen takes in place into the other set and then over the set on show, so
 * the widgets' pointers stay put and nothing half written ever shows, or
 * straight into the set on show while the other is held (TAKE_HERE). The
 * answer carries shim_show's MH_SHOW_* bits.
 */
static u32 take_page(u32 starting)
{
	u32 s1 = r32(BOX(MBOX_OFF_PAGE_SEQ));
	u32 n, i, key, type, cursor, pflags, how, set, there, newkey, f;

	if (s1 == 0 || (s1 & 1) != 0) return TAKE_NONE;
	if (r8(ST(ST_HAVE_PAGE)) != 0 && s1 == r32(ST(ST_SHOWN_SEQ))) return TAKE_NONE;

	mh_sync();
	key = r32(BOX(MBOX_OFF_PAGE_KEY));
	n = r8(BOX(MBOX_OFF_NROWS));
	cursor = r8(BOX(MBOX_OFF_CURSOR));
	pflags = r8(BOX(MBOX_OFF_PFLAGS));
	type = r8(BOX(MBOX_OFF_PTYPE));
	if (n > MH_ROWS) n = MH_ROWS;
	/* A shim without the widget a page asks for draws it as a MAIN page. */
	if (type != MBOX_PTYPE_CONFIG || MH_CFG_ROWS == 0) type = MBOX_PTYPE_MAIN;

	newkey = r8(ST(ST_HAVE_PAGE)) == 0 || key != r32(ST(ST_SHOWN_KEY)) || r8(ST(ST_NEW_KEY)) != 0;
	how = (newkey ? MH_SHOW_NEW : 0) | ((n != 0 ? n : 1) != nrows() ? MH_SHOW_ROWS : 0);
	there = !starting && (type != r8(ST(ST_SHOWN)) ||
	                      (how & (type == MBOX_PTYPE_MAIN ? MH_RESTART_MAIN : MH_RESTART_CFG)) != 0);
	if (there && r8(ST(ST_HOLD)) != 0) return TAKE_NONE;
	set = starting || (!there && r8(ST(ST_HOLD)) != 0) ? front() : other();

	w8(set + SET_PTYPE, type);
	w8(set + SET_NROWS, n != 0 ? n : 1);
	if (n == 0) {
		/* A page of no rows is none qwark posts; show one blank row rather than divide by 0. */
		w8(set + SET_KIND, MROW_INFO);
		w8(set + SET_FLAGS, 0);
		w16(set + SET_ID, 0);
		w8(set + SET_TEXT, 0);
		w8(set + SET_TEXT + MH_LABEL_LEN, 0);
	}
	for (i = 0; i < n; i++) compose(set, i);
	title(set);
	mh_sync();

	if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != s1) {
		mh_count(ST_RETRIES);
		return TAKE_NONE;
	}

	f = front();
	if (set != f && !there) {
		for (i = 0; i < MH_SET_SIZE; i += 4) w32(f + i, r32(set + i));
	}
	w8(ST(ST_WAITING), 0);
	w8(ST(ST_PTYPE), type);
	w8(ST(ST_NEW_KEY), newkey);
	w32(ST(ST_SHOWN_SEQ), s1);
	w32(ST(ST_SHOWN_KEY), key);
	w8(ST(ST_CURSOR_HINT), cursor);
	w8(ST(ST_PFLAGS), pflags);
	w8(ST(ST_DESC_ROW), MH_NONE);
	w8(ST(ST_HAVE_PAGE), 1);
	mh_count(ST_PAGES);
	return (there ? TAKE_OTHER : TAKE_HERE) | how;
}

/* Before qwark has posted anything: one grey row, and back leaves. */
static void placeholder(void)
{
	u32 set = front(), t = set + SET_TEXT;

	w8(set + SET_PTYPE, MBOX_PTYPE_MAIN);
	w8(set + SET_NROWS, 1);
	w8(set + SET_KIND, MROW_INFO);
	w8(set + SET_FLAGS, 0);
	w16(set + SET_ID, 0);
	w8(put_lit(t, t + MH_TEXT_LEN, "Waiting for qwark", MH_CASE_MAIN), 0);
	w8(put_lit(set + SET_TITLE, set + SET_TITLE + MH_TITLE_LEN, MH_ENTRY_LABEL, MH_CASE_MAIN), 0);
	w8(ST(ST_PTYPE), MBOX_PTYPE_MAIN);
	w8(ST(ST_CURSOR_HINT), 0);
	w8(ST(ST_PFLAGS), MBOX_PAGE_ROOT);
}

/* `c` moved onto the page, and off a row the cursor cannot land on. */
static u32 pick_cursor(u32 c)
{
	u32 n = nrows(), flags = front() + SET_FLAGS;
	u32 i;

	if (c >= n) c = n - 1;
	if ((r8(flags + c) & MROWF_ENABLED) != 0) return c;
	for (i = 0; i < n; i++) {
		if ((r8(flags + i) & MROWF_ENABLED) != 0) return i;
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
	u32 row = shim_cursor(p), d = ST(ST_DESC), seq;

	if (MH_ROW_BYTES < MBOX_ROW_SIZE_DESC || row == r8(ST(ST_DESC_ROW))) return 0;

	seq = r32(ST(ST_SHOWN_SEQ));
	if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != seq) return 0;
	mh_sync();
	if (r8(BOX(MBOX_OFF_HINT)) != 0)
		w8(put_mem(d, d + MH_DESC_LEN, BOX(MBOX_OFF_HINT), MBOX_HINT_LEN, 0), 0);
	else if (row < nrows())
		w8(put_mem(d, d + MH_DESC_LEN, MROW(row) + MROW_OFF_DESC, MROW_DESC_LEN, 0), 0);
	else
		w8(d, 0);
	mh_sync();
	/* Caught half written: the line is read again next frame, or with the next page. */
	if (r32(BOX(MBOX_OFF_PAGE_SEQ)) != seq) return 0;
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
	w16(BOX(MBOX_OFF_EV_ID), row < nrows() ? r16(front() + SET_ID + 2 * row) : 0);
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
	u32 n = nrows(), kinds = front() + SET_KIND;
	u32 i;

	for (i = 0; i < n; i++) {
		if (r8(kinds + i) == kind) return 1;
	}
	return 0;
}

/*
 * Start, Select or a close request: the whole menu closes, also halfway
 * through a switch, which is then no switch: its stop is the close. The page
 * the switch was for is the last taken, so its set is the one on show from
 * here, for whatever opens next; the screen closing keeps the other.
 */
static void close_all(int p)
{
	if (r8(ST(ST_SWITCHING)) != 0) {
		w8(ST(ST_FRONT), r8(ST(ST_FRONT)) ^ 1);
		w8(ST(ST_SWITCHING), 0);
	}
	shim_close(p, MH_CLOSE_EXIT);
}

MH_API void mh_core_press(int p, u32 button, u32 row)
{
	u32 flags = row < nrows() ? r8(front() + SET_FLAGS + row) : 0;

	/* A screen on its way out acts on nothing but a close. */
	if (r8(ST(ST_SWITCHING)) != 0 && button != MH_BTN_CLOSE) return;

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
		close_all(p);
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
	w8(ST(ST_WAITING), 0);

	if (r8(ST(ST_SWITCHING)) != 0) {
		/* The page the switch was for takes over: its set is the one on show now. */
		w8(ST(ST_FRONT), r8(ST(ST_FRONT)) ^ 1);
		w8(ST(ST_SWITCHING), 0);
	} else {
		/*
		 * A fresh open: the frames count from it, which tells qwark the menu
		 * opened again even if it never saw it closed (written before the
		 * menu says it is open).
		 */
		w32(BOX(MBOX_OFF_FRAMES), 0);
		w32(ST(ST_BUSY_SINCE), 0);
		w8(ST(ST_HOLD), 0);
		mh_sync();
	}
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);

	if (take_page(1) == TAKE_NONE) {
		if (r8(ST(ST_HAVE_PAGE)) == 0) placeholder();
		else retitle();
	}
	w8(ST(ST_DESC_ROW), MH_NONE);
	w8(ST(ST_SHOWN), r8(front() + SET_PTYPE));
	return (int)r8(front() + SET_PTYPE);
}

MH_API void mh_core_stop(int p)
{
	w8(ST(ST_LAST_CURSOR), shim_cursor(p));
	w8(ST(ST_SHOWN), MH_NONE);
	if (r8(ST(ST_SWITCHING)) != 0) return;
	/* A close: the next open starts over, on the root qwark puts back, at its first row. */
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_CLOSED);
	w8(ST(ST_WAITING), 0);
	w8(ST(ST_LAST_CURSOR), 0);
	w8(ST(ST_HOLD), 0);
}

MH_API int mh_core_frame(int p)
{
	u32 req, frames, ev, ack, t;

	frames = r32(BOX(MBOX_OFF_FRAMES)) + 1;
	w32(BOX(MBOX_OFF_FRAMES), frames);
	w8(BOX(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);
	if (r8(ST(ST_HOLD)) != 0) w8(ST(ST_HOLD), r8(ST(ST_HOLD)) - 1);

	req = r8(BOX(MBOX_OFF_REQUEST));
	if (req != MBOX_REQ_NONE) {
		w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
		if (req == MBOX_REQ_CLOSE) {
			close_all(p);
			return MH_GONE;
		}
	}

	/* The screen is on its way out: the next one takes whatever comes. */
	if (r8(ST(ST_SWITCHING)) != 0) return 0;

	t = take_page(0);
	if ((t & TAKE_OTHER) != 0) {
		/*
		 * The other widget, or a page this one cannot take in place: the
		 * shim starts its screen again, through mh_core_start, which makes
		 * the other set the one on show. The screen going keeps its own
		 * until then, and MH_HOLD_* frames more where it fades out.
		 */
		w8(ST(ST_HOLD), r8(ST(ST_SHOWN)) == MBOX_PTYPE_MAIN ? MH_HOLD_MAIN : MH_HOLD_CFG);
		w8(ST(ST_SWITCHING), 1);
		shim_switch(p);
		return MH_GONE;
	}
	if ((t & TAKE_HERE) != 0) {
		shim_show(p, t & (MH_SHOW_NEW | MH_SHOW_ROWS));
	} else {
		ev = r32(BOX(MBOX_OFF_EV_SEQ));
		ack = r32(BOX(MBOX_OFF_EV_ACK));
		if (r8(ST(ST_WAITING)) != 0) {
			/* Answered: the page's own title back, once the mailbox has nothing newer. */
			if (ev == ack && retitle()) {
				w8(ST(ST_WAITING), 0);
				shim_title(p);
			}
		} else if (ev != ack && frames - r32(ST(ST_BUSY_SINCE)) >= MH_WAIT_FRAMES) {
			waiting();
			shim_title(p);
		}
	}

	if (describe(p)) shim_desc(p);
	return 0;
}

MH_API void mh_core_hold(u32 frames)
{
	w8(ST(ST_HOLD), frames > 0xFF ? 0xFF : frames);
}

MH_API u32 mh_core_open_request(void)
{
	if (r8(BOX(MBOX_OFF_REQUEST)) != MBOX_REQ_OPEN) return 0;
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	mh_count(ST_OPEN_REQS);
	return 1;
}
