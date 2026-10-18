#include "menu.h"
#include "features.h"
#include "session.h"
#include "config.h"
#include "mem.h"
#include "util.h"
#include "../games/menuhelper_bins.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * The console menu, both halves (menu.h). The model first: pages, rows, the
 * navigation stack and what an event does. Then the driver: the renderer's
 * install, the mailbox and the combo.
 */

/* --------------------------------------------------------------- the model */

static const struct game_api *g_game;
static u8 g_max_rows = 8;
static u8 g_caps;

struct menu_nav {
	u16 page;
	u8  start;              /* the first of the page's rows this window shows */
	u8  cursor;             /* the row of the window the cursor goes to (or came from) */
};

static struct menu_nav g_nav[MENU_NAV_DEPTH];
static u8 g_depth;
/*
 * Bumped on every move between pages or windows, so a page's key changes with
 * every visit and an event made on an earlier visit is never taken for this one.
 */
static u8 g_visit;

/* The step a VALUE row moves by, as a power of ten, per feature id. */
static u8 g_step[QWARK_MAX_FEATURES];

/* The last view built: what an event has to be about. */
static u32 g_view_key;
static u8  g_view_n;
static u16 g_view_id[MENU_VIEW_MAX];

/*
 * A VALUE or ENUM just written from the menu. Its readout lags the write by up
 * to a slow block's period (session.c), so the row shows, and the next step
 * starts from, what was written until the readout agrees or a quarter second
 * has gone by.
 */
#define VCACHE_TICKS 30
static u8  g_vc_id = 0xFF;
static u32 g_vc_value;
static u32 g_vc_until;

/*
 * A row's answer, shown in place of its value for two seconds: "done" for an
 * action, the reason for a refusal.
 */
#define NOTE_TICKS 240
#define NOTE_DEFERRED 0xFF
static u16 g_note_page;
static u16 g_note_id = 0;
static u8  g_note_rc;
static u32 g_note_until;
static char g_note_label[32];

/* An action that runs once the menu has closed (MROWF_CLOSES), 0xFF for none. */
static u8 g_defer_id = 0xFF;

static u32 now_tick(void)
{
	return session_tick_count();
}

static const struct game_describe *desc(void)
{
	if (g_game == NULL || g_game->describe == NULL) return NULL;
	return g_game->describe();
}

static const struct feature_desc *feature(u8 id)
{
	const struct game_describe *d = desc();
	u8 i;

	if (d == NULL) return NULL;
	for (i = 0; i < d->nfeatures; i++) {
		if (d->features[i].id == id) return &d->features[i];
	}
	return NULL;
}

/* How many of the game's features are in group `g`, and the i-th of them. */
static u8 group_count(u8 g)
{
	const struct game_describe *d = desc();
	u8 i, n = 0;

	if (d == NULL) return 0;
	for (i = 0; i < d->nfeatures; i++) {
		if (d->features[i].group == g) n++;
	}
	return n;
}

static const struct feature_desc *group_feature(u8 g, u8 index)
{
	const struct game_describe *d = desc();
	u8 i, n = 0;

	if (d == NULL) return NULL;
	for (i = 0; i < d->nfeatures; i++) {
		if (d->features[i].group != g) continue;
		if (n == index) return &d->features[i];
		n++;
	}
	return NULL;
}

/* The root's links: the groups that have a feature, in DESCRIBE order. */
static u8 root_groups(u8 *out, u8 cap)
{
	const struct game_describe *d = desc();
	u8 g, n = 0;

	if (d == NULL) return 0;
	for (g = 0; g < d->ngroups && n < cap; g++) {
		if (group_count(g) != 0) out[n++] = g;
	}
	return n;
}

static void set_text(char *dst, u32 cap, const char *src)
{
	memset(dst, 0, cap);
	if (src != NULL) qstrcpy(dst, cap, src);
}

static const char *status_text(u8 rc)
{
	switch (rc) {
	case ST_OK:          return "done";
	case ST_NOT_INGAME:  return "not in game";
	case ST_UNSUPPORTED: return "not available";
	case ST_BAD_ARG:     return "refused";
	case ST_IO_ERROR:    return "failed";
	case ST_FULL:        return "full";
	case ST_NOT_FOUND:   return "not found";
	case ST_BUSY:        return "busy";
	case NOTE_DEFERRED:  return "after closing";
	default:             return "error";
	}
}

static void note(u16 id, u8 rc, const char *label)
{
	g_note_page = g_nav[g_depth - 1].page;
	g_note_id = id;
	g_note_rc = rc;
	g_note_until = now_tick() + NOTE_TICKS;
	set_text(g_note_label, sizeof(g_note_label), label);
}

/* The page's hint: the last answer as "<row>: <answer>", while it lasts. */
static void note_hint(char *out, u32 cap)
{
	memset(out, 0, cap);
	if (g_note_id == 0 || (s32)(now_tick() - g_note_until) >= 0) return;
	qstrcpy(out, cap, g_note_label);
	qstrcat(out, cap, ": ");
	qstrcat(out, cap, status_text(g_note_rc));
}

/* The row's note, if it has one now. */
static const char *note_for(u16 page, u16 id)
{
	if (g_note_id == 0 || id != g_note_id || page != g_note_page) return NULL;
	if ((s32)(now_tick() - g_note_until) >= 0) {
		g_note_id = 0;
		return NULL;
	}
	return status_text(g_note_rc);
}

/* ------------------------------------------------------------ the values */

static u8 value_bits(const struct feature_desc *f)
{
	return (f->aux == 8 || f->aux == 16) ? f->aux : 32;
}

static u32 value_mask(const struct feature_desc *f)
{
	u8 bits = value_bits(f);
	return bits == 32 ? 0xFFFFFFFFu : (((u32)1 << bits) - 1u);
}

/* The current value of a VALUE, ENUM or COLOR row: the readout, or what was just written. */
static u32 feature_value(const struct feature_desc *f)
{
	u32 live = 0;

	if (f->readout < QWARK_MAX_READOUTS) live = session_readout(f->readout);
	if (g_vc_id == f->id) {
		if (live == g_vc_value || (s32)(now_tick() - g_vc_until) >= 0) g_vc_id = 0xFF;
		else return g_vc_value;
	}
	return live;
}

static void remember_value(u8 id, u32 value)
{
	g_vc_id = id;
	g_vc_value = value;
	g_vc_until = now_tick() + VCACHE_TICKS;
}

/* The field as a number, two's complement in its width for a SIGNED row. */
static s64 value_number(const struct feature_desc *f, u32 raw)
{
	u8 bits = value_bits(f);

	raw &= value_mask(f);
	if ((f->flags & FEATURE_FLAG_SIGNED) != 0 && bits < 64 &&
	    (raw & ((u32)1 << (bits - 1))) != 0)
		return (s64)raw - ((s64)1 << bits);
	return (s64)raw;
}

static void value_range(const struct feature_desc *f, s64 *lo, s64 *hi)
{
	u8 bits = value_bits(f);

	if ((f->flags & FEATURE_FLAG_SIGNED) != 0) {
		*lo = -((s64)1 << (bits - 1));
		*hi = ((s64)1 << (bits - 1)) - 1;
	} else if (f->min == 0 && f->max == 0) {
		*lo = 0;
		*hi = (s64)value_mask(f);
	} else {
		*lo = f->min;
		*hi = f->max;
	}
}

static const u32 g_pow10[] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u };
#define STEP_LEVELS 7

static void fmt_number(char *out, u32 cap, s64 v)
{
	u32 n = 0;

	if (cap < 2) return;
	if (v < 0) {
		out[n++] = '-';
		v = -v;
	}
	qfmt_u32(out + n, cap - n, (u32)v);
}

static void value_text(const struct feature_desc *f, char *out, u32 cap)
{
	char step[16];

	fmt_number(out, cap, value_number(f, feature_value(f)));
	if (f->id < QWARK_MAX_FEATURES && g_step[f->id] != 0 && (g_caps & MBOX_CAP_STEPS) != 0) {
		step[0] = ' ';
		step[1] = '[';
		qfmt_u32(step + 2, sizeof(step) - 3, g_pow10[g_step[f->id]]);
		qstrcat(step, sizeof(step), "]");
		qstrcat(out, cap, step);
	}
}

/* The ENUM's options, or none. */
static u8 enum_options(const struct feature_desc *f, const char * const **opts)
{
	u8 count = 0;

	*opts = NULL;
	if (features_options(f->id, opts, &count) != ST_OK || *opts == NULL) return 0;
	return count;
}

static void enum_text(const struct feature_desc *f, char *out, u32 cap)
{
	const char * const *opts;
	u8 count = enum_options(f, &opts);
	u32 v = feature_value(f);

	if (v >= f->min && v - f->min < count && opts[v - f->min] != NULL) {
		qstrcpy(out, cap, opts[v - f->min]);
		return;
	}
	qfmt_u32(out, cap, v);
}

/* ------------------------------------------------------------- the rows */

static void feature_row(u16 page, const struct feature_desc *f, struct menu_row *r)
{
	const char *n;
	u8 steps = (g_caps & MBOX_CAP_STEPS) != 0 ? MROWF_STEPS : 0;

	memset(r, 0, sizeof(*r));
	r->id = MENU_ROW_ID(MENU_ROW_FEATURE, f->id);
	set_text(r->label, sizeof(r->label), f->label);

	switch (f->kind) {
	case FEATURE_TOGGLE: {
		int on = f->id < QWARK_MAX_FEATURES &&
		         (features_toggle_state() & ((u64)1 << f->id)) != 0;
		r->kind = MROW_TOGGLE;
		r->flags = MROWF_ENABLED | steps | (on ? MROWF_ON : 0);
		qstrcpy(r->value, sizeof(r->value), on ? "ON" : "OFF");
		set_text(r->desc, sizeof(r->desc), "On or off.");
		break;
	}
	case FEATURE_ACTION:
		r->kind = MROW_ACTION;
		r->flags = MROWF_ENABLED;
		/*
		 * Revision 1.22. Loading the set-aside file runs the game's own load,
		 * which Deadlocked's helper does from a pause of its own, so it waits
		 * for the menu to close and the game to be back in play.
		 */
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0) r->flags |= MROWF_CLOSES;
		set_text(r->desc, sizeof(r->desc), (r->flags & MROWF_CLOSES) != 0
		         ? "Runs once the menu has closed." : "Runs once.");
		break;
	case FEATURE_VALUE:
		/*
		 * Left and Right step the number, the cross changes the step: a
		 * stepper wants both, so a renderer that cannot send Left and Right
		 * shows the number and nothing more.
		 */
		if (steps != 0) {
			r->kind = MROW_VALUE;
			r->flags = MROWF_ENABLED | MROWF_STEPS;
			set_text(r->desc, sizeof(r->desc), "Left and Right step it; select changes the step.");
		} else {
			r->kind = MROW_INFO;
		}
		value_text(f, r->value, sizeof(r->value));
		break;
	case FEATURE_ENUM:
		r->kind = MROW_CHOICE;
		r->flags = MROWF_ENABLED | steps;
		enum_text(f, r->value, sizeof(r->value));
		set_text(r->desc, sizeof(r->desc), steps != 0
		         ? "Left and Right pick; select lists them all." : "Select lists them all.");
		break;
	case FEATURE_COLOR: {
		u32 c = feature_value(f) & 0x00FFFFFFu;
		r->kind = MROW_INFO;
		r->value[0] = '#';
		qfmt_hex(r->value + 1, sizeof(r->value) - 1, c, 6);
		break;
	}
	default:
		r->kind = MROW_INFO;
		break;
	}

	n = note_for(page, r->id);
	if (n != NULL) set_text(r->value, sizeof(r->value), n);
}

/* ------------------------------------------------------------- the pages */

static u8 page_kind(u16 page) { return (u8)(page >> 8); }
static u8 page_arg(u16 page)  { return (u8)page; }

/* How many rows the page has, before it is cut into windows. */
static u8 page_count(u16 page)
{
	const struct feature_desc *f;
	const char * const *opts;
	u8 groups[QWARK_MAX_GROUPS];

	switch (page_kind(page)) {
	case MENU_PAGE_ROOT:
		return (u8)(root_groups(groups, QWARK_MAX_GROUPS) + 1);
	case MENU_PAGE_GROUP:
		return group_count(page_arg(page));
	case MENU_PAGE_CHOICE:
		f = feature(page_arg(page));
		return f != NULL ? enum_options(f, &opts) : 0;
	default:
		return 0;
	}
}

static void page_row(u16 page, u8 index, struct menu_row *r)
{
	const struct game_describe *d = desc();
	const struct feature_desc *f;
	const char * const *opts;
	u8 groups[QWARK_MAX_GROUPS];
	u8 ng, count;
	char build[8];

	memset(r, 0, sizeof(*r));

	switch (page_kind(page)) {
	case MENU_PAGE_ROOT:
		ng = root_groups(groups, QWARK_MAX_GROUPS);
		if (index < ng && d != NULL) {
			r->id = MENU_ROW_ID(MENU_ROW_GROUP, groups[index]);
			r->kind = MROW_LINK;
			r->flags = MROWF_ENABLED;
			set_text(r->label, sizeof(r->label), d->groups[groups[index]]);
		} else {
			r->id = MENU_ROW_ID(MENU_ROW_TEXT, 0);
			r->kind = MROW_INFO;
			set_text(r->label, sizeof(r->label), "qwark build ");
			qfmt_u32(build, sizeof(build), QWARK_BUILD);
			qstrcat(r->label, sizeof(r->label), build);
		}
		return;

	case MENU_PAGE_GROUP:
		f = group_feature(page_arg(page), index);
		if (f != NULL) feature_row(page, f, r);
		return;

	case MENU_PAGE_CHOICE:
		f = feature(page_arg(page));
		if (f == NULL) return;
		count = enum_options(f, &opts);
		if (index >= count) return;
		r->id = MENU_ROW_ID(MENU_ROW_OPTION, index);
		r->kind = MROW_OPTION;
		r->flags = MROWF_ENABLED;
		set_text(r->desc, sizeof(r->desc), "Select picks it.");
		if (feature_value(f) == f->min + index) r->flags |= MROWF_ON;
		if (opts[index] != NULL) set_text(r->label, sizeof(r->label), opts[index]);
		else qfmt_u32(r->label, sizeof(r->label), index);
		{
			const char *n = note_for(page, r->id);
			if (n != NULL) set_text(r->value, sizeof(r->value), n);
		}
		return;

	default:
		return;
	}
}

static void page_title(u16 page, char *out, u32 cap)
{
	const struct game_describe *d = desc();
	const struct feature_desc *f;

	memset(out, 0, cap);
	switch (page_kind(page)) {
	case MENU_PAGE_GROUP:
		if (d != NULL && page_arg(page) < d->ngroups)
			qstrcpy(out, cap, d->groups[page_arg(page)]);
		return;
	case MENU_PAGE_CHOICE:
		f = feature(page_arg(page));
		if (f != NULL) qstrcpy(out, cap, f->label);
		return;
	default:
		qstrcpy(out, cap, "RaCMAN");
		return;
	}
}

/* ----------------------------------------------------------- the windows */

/*
 * A page longer than the renderer's screen is cut into windows. The first has
 * as many of the page's rows as fit and a MORE row last; a middle one a PREV
 * row first and a MORE row last; the last a PREV row and the rest. With a
 * renderer that has no back control every page but the root ends in a BACK row
 * as well, and the screen is one row shorter for the page's own.
 */
static u8 screen_rows(void)
{
	u8 m = g_max_rows;

	if ((g_caps & MBOX_CAP_BACK) == 0 && g_depth > 1) m--;
	return m;
}

static void window(u8 n, u8 start, u8 *content, int *prev, int *more)
{
	u8 m = screen_rows();
	u8 avail;

	*prev = 0;
	*more = 0;
	if (n <= m) {
		*content = n;
		return;
	}
	*prev = start > 0;
	avail = (u8)(m - (*prev ? 1 : 0));
	if (n - start <= avail) {
		*content = (u8)(n - start);
	} else {
		*content = (u8)(avail - 1);
		*more = 1;
	}
}

/* Where the window before `start` begins. */
static u8 window_prev(u8 start)
{
	u8 m = screen_rows();

	if (start <= (u8)(m - 1)) return 0;
	return (u8)(start - (m - 2));
}

/* The window that shows row `index` of a page of n rows, and that row's place in it. */
static u8 window_of(u8 n, u8 index, u8 *pos)
{
	u8 start = 0, content;
	int prev, more;

	for (;;) {
		window(n, start, &content, &prev, &more);
		if (index < start + content || !more) break;
		start = (u8)(start + content);
	}
	*pos = (u8)((prev ? 1 : 0) + (index - start));
	return start;
}

/* Which window `start` is, and how many there are, counted from 1. */
static void window_number(u8 n, u8 start, u8 *k, u8 *total)
{
	u8 s = 0, content;
	int prev, more;

	*k = 1;
	*total = 1;
	for (;;) {
		window(n, s, &content, &prev, &more);
		if (s < start) (*k)++;
		if (!more) break;
		s = (u8)(s + content);
		(*total)++;
	}
	if (*k > *total) *k = *total;
}

/* A start that is no window's (the page got shorter) goes back to the first. */
static u8 valid_start(u8 n, u8 start)
{
	u8 s = 0, content;
	int prev, more;

	for (;;) {
		if (s == start) return start;
		window(n, s, &content, &prev, &more);
		if (!more || s > start) return 0;
		s = (u8)(s + content);
	}
}

/* ------------------------------------------------------------ navigation */

static void nav_reset(void)
{
	memset(g_nav, 0, sizeof(g_nav));
	g_nav[0].page = MENU_PAGE_ID(MENU_PAGE_ROOT, 0);
	g_depth = 1;
	g_visit++;
}

/* Opens `page` from row `from` of the current view, with the cursor on its row `cursor`. */
static int nav_push(u16 page, u8 from, u8 start, u8 cursor)
{
	if (g_depth >= MENU_NAV_DEPTH) return ST_FULL;
	g_nav[g_depth - 1].cursor = from;
	g_nav[g_depth].page = page;
	g_nav[g_depth].start = start;
	g_nav[g_depth].cursor = cursor;
	g_depth++;
	g_visit++;
	return ST_OK;
}

/* Back a page, the cursor on the row the page was opened from. */
static int nav_pop(void)
{
	if (g_depth <= 1) return ST_BAD_ARG;
	g_depth--;
	g_visit++;
	return ST_OK;
}

/* The next or the previous window of the current page; `cursor_end` puts the cursor last. */
static int nav_turn(int forward, int cursor_end)
{
	struct menu_nav *cur = &g_nav[g_depth - 1];
	u8 n = page_count(cur->page);
	u8 content;
	int prev, more;

	window(n, cur->start, &content, &prev, &more);
	if (forward) {
		if (!more) return ST_BAD_ARG;
		cur->start = (u8)(cur->start + content);
	} else {
		if (!prev) return ST_BAD_ARG;
		cur->start = window_prev(cur->start);
	}

	window(n, cur->start, &content, &prev, &more);
	cur->cursor = (u8)(prev ? 1 : 0);
	if (cursor_end && content != 0) cur->cursor = (u8)(cur->cursor + content - 1);
	g_visit++;
	return ST_OK;
}

/* ------------------------------------------------------------- the view */

void menu_model_reset(u8 max_rows, u8 caps)
{
	g_game = session_game();
	if (max_rows < 3) max_rows = 3;
	if (max_rows > MENU_VIEW_MAX) max_rows = MENU_VIEW_MAX;
	g_max_rows = max_rows;
	g_caps = caps;
	nav_reset();
	memset(g_step, 0, sizeof(g_step));
	g_vc_id = 0xFF;
	g_note_id = 0;
	g_defer_id = 0xFF;
	g_view_key = 0;
	g_view_n = 0;
}

u16 menu_model_page(void)
{
	return g_nav[g_depth - 1].page;
}

u8 menu_model_depth(void)
{
	return g_depth;
}

static void push_row(struct menu_view *v, const struct menu_row *r)
{
	if (v->nrows >= MENU_VIEW_MAX) return;
	v->rows[v->nrows] = *r;
	v->nrows++;
}

static void nav_row(struct menu_view *v, u8 which, const char *label, const char *value)
{
	struct menu_row r;

	memset(&r, 0, sizeof(r));
	r.id = MENU_ROW_ID(MENU_ROW_NAV, which);
	r.kind = which == MENU_NAV_MORE ? MROW_MORE : which == MENU_NAV_PREV ? MROW_PREV : MROW_BACK;
	r.flags = MROWF_ENABLED;
	set_text(r.label, sizeof(r.label), label);
	set_text(r.value, sizeof(r.value), value);
	push_row(v, &r);
}

void menu_model_view(struct menu_view *v)
{
	struct menu_nav *cur = &g_nav[g_depth - 1];
	struct menu_row r;
	u8 n, content, i, k, total;
	int prev, more;
	char of[12];

	memset(v, 0, sizeof(*v));
	g_game = session_game();

	n = page_count(cur->page);
	cur->start = valid_start(n, cur->start);
	window(n, cur->start, &content, &prev, &more);
	window_number(n, cur->start, &k, &total);

	v->key = ((u32)cur->page << 16) | ((u32)cur->start << 8) | g_visit;
	v->pflags = g_depth == 1 ? MBOX_PAGE_ROOT : 0;
	page_title(cur->page, v->title, sizeof(v->title));
	note_hint(v->hint, sizeof(v->hint));

	of[0] = 0;
	if (total > 1) {
		char num[4];
		qfmt_u32(num, sizeof(num), k);
		qstrcpy(of, sizeof(of), " ");
		qstrcat(of, sizeof(of), num);
		qstrcat(of, sizeof(of), "/");
		qfmt_u32(num, sizeof(num), total);
		qstrcat(of, sizeof(of), num);
		if (qstrlen(v->title) + qstrlen(of) < sizeof(v->title))
			qstrcat(v->title, sizeof(v->title), of);
	}

	if (prev) nav_row(v, MENU_NAV_PREV, "Previous", "");
	for (i = 0; i < content; i++) {
		page_row(cur->page, (u8)(cur->start + i), &r);
		push_row(v, &r);
	}
	if (more) nav_row(v, MENU_NAV_MORE, "More", "");
	if ((g_caps & MBOX_CAP_BACK) == 0 && g_depth > 1) nav_row(v, MENU_NAV_BACK, "Back", "");

	/* A page with nothing on it still has a row, which says so. */
	if (v->nrows == 0) {
		memset(&r, 0, sizeof(r));
		r.id = MENU_ROW_ID(MENU_ROW_TEXT, 1);
		r.kind = MROW_INFO;
		set_text(r.label, sizeof(r.label), "Nothing here");
		push_row(v, &r);
	}

	v->cursor = cur->cursor < v->nrows ? cur->cursor : (u8)(v->nrows - 1);

	g_view_key = v->key;
	g_view_n = v->nrows;
	for (i = 0; i < v->nrows; i++) g_view_id[i] = v->rows[i].id;
}

/* ------------------------------------------------------------ the events */

/* Left or Right on a feature row: dir is -1 or +1. */
static int step_feature(const struct feature_desc *f, int dir)
{
	const char * const *opts;
	s64 v, lo, hi, step;
	u32 raw;
	u8 count;
	int rc;

	switch (f->kind) {
	case FEATURE_TOGGLE: {
		int on = f->id < QWARK_MAX_FEATURES &&
		         (features_toggle_state() & ((u64)1 << f->id)) != 0;
		int want = dir > 0;
		if (on == want) return ST_OK;
		return features_set(f->id, (u32)want);
	}
	case FEATURE_ENUM:
		count = enum_options(f, &opts);
		if (count == 0) return ST_UNSUPPORTED;
		raw = feature_value(f) - f->min;
		if (raw >= count) raw = 0;
		else raw = (raw + count + (dir > 0 ? 1u : count - 1u)) % count;
		rc = features_set(f->id, f->min + raw);
		if (rc == ST_OK) remember_value(f->id, f->min + raw);
		return rc;
	case FEATURE_VALUE:
		value_range(f, &lo, &hi);
		step = g_pow10[f->id < QWARK_MAX_FEATURES ? g_step[f->id] : 0];
		v = value_number(f, feature_value(f)) + (dir > 0 ? step : -step);
		if (v < lo) v = lo;
		if (v > hi) v = hi;
		raw = (u32)((u64)v & value_mask(f));
		rc = features_set(f->id, raw);
		if (rc == ST_OK) remember_value(f->id, raw);
		return rc;
	default:
		return ST_OK;
	}
}

/* The cross on a VALUE row: the next step, back to 1 past the field's range. */
static void cycle_step(const struct feature_desc *f)
{
	s64 lo, hi;
	u8 next;

	if (f->id >= QWARK_MAX_FEATURES) return;
	value_range(f, &lo, &hi);
	next = (u8)(g_step[f->id] + 1);
	if (next >= STEP_LEVELS || (s64)g_pow10[next] > hi - lo) next = 0;
	g_step[f->id] = next;
}

static int activate_feature(u8 row, u16 id, const struct feature_desc *f)
{
	const char * const *opts;
	u8 count, start, pos;
	u32 v;
	int rc;

	switch (f->kind) {
	case FEATURE_TOGGLE:
		rc = step_feature(f, (features_toggle_state() & ((u64)1 << f->id)) != 0 ? -1 : 1);
		if (rc != ST_OK) note(id, (u8)rc, f->label);
		return rc;

	case FEATURE_ACTION:
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0) {
			g_defer_id = f->id;
			note(id, NOTE_DEFERRED, f->label);
			return ST_OK;
		}
		rc = features_trigger(f->id);
		note(id, (u8)rc, f->label);
		return rc;

	case FEATURE_VALUE:
		if ((g_caps & MBOX_CAP_STEPS) != 0) cycle_step(f);
		return ST_OK;

	case FEATURE_ENUM:
		count = enum_options(f, &opts);
		if (count == 0) return ST_UNSUPPORTED;
		v = feature_value(f) - f->min;
		if (v >= count) v = 0;
		start = window_of(count, (u8)v, &pos);
		return nav_push(MENU_PAGE_ID(MENU_PAGE_CHOICE, f->id), row, start, pos);

	default:
		return ST_OK;
	}
}

static int activate(u8 row, u16 id)
{
	struct menu_nav *cur = &g_nav[g_depth - 1];
	const struct feature_desc *f;
	u8 arg = (u8)id;
	int rc;

	switch ((u8)(id >> 8)) {
	case MENU_ROW_GROUP:
		return nav_push(MENU_PAGE_ID(MENU_PAGE_GROUP, arg), row, 0, 0);

	case MENU_ROW_FEATURE:
		f = feature(arg);
		if (f == NULL) return ST_NOT_FOUND;
		return activate_feature(row, id, f);

	case MENU_ROW_OPTION:
		if (page_kind(cur->page) != MENU_PAGE_CHOICE) return ST_BAD_ARG;
		f = feature(page_arg(cur->page));
		if (f == NULL) return ST_NOT_FOUND;
		rc = features_set(f->id, f->min + arg);
		if (rc != ST_OK) {
			note(id, (u8)rc, f->label);
			return rc;
		}
		remember_value(f->id, f->min + arg);
		return nav_pop();

	case MENU_ROW_NAV:
		if (arg == MENU_NAV_MORE) return nav_turn(1, 0);
		if (arg == MENU_NAV_PREV) return nav_turn(0, 1);
		if (arg == MENU_NAV_BACK) return nav_pop();
		return ST_BAD_ARG;

	default:
		return ST_OK;
	}
}

int menu_model_event(u8 kind, u8 row, u16 id, u32 key)
{
	const struct feature_desc *f;
	int rc;

	if (key != g_view_key) return ST_BAD_ARG;

	switch (kind) {
	case MBOX_EV_BACK:
		return nav_pop();
	case MBOX_EV_PAGE_PREV:
		return nav_turn(0, 0);
	case MBOX_EV_PAGE_NEXT:
		return nav_turn(1, 0);
	default:
		break;
	}

	if (row >= g_view_n || g_view_id[row] != id) return ST_BAD_ARG;

	switch (kind) {
	case MBOX_EV_ACTIVATE:
		return activate(row, id);

	case MBOX_EV_LEFT:
	case MBOX_EV_RIGHT:
		if ((u8)(id >> 8) != MENU_ROW_FEATURE) return ST_OK;
		f = feature((u8)id);
		if (f == NULL) return ST_NOT_FOUND;
		rc = step_feature(f, kind == MBOX_EV_RIGHT ? 1 : -1);
		if (rc != ST_OK) note(id, (u8)rc, f->label);
		return rc;

	default:
		return ST_BAD_ARG;
	}
}

/* The CLOSES action waiting for the menu to close, taken by the driver. */
static u8 take_deferred(void)
{
	u8 id = g_defer_id;
	g_defer_id = 0xFF;
	return id;
}

/* -------------------------------------------------------------- the driver */

/* How often the mailbox is read: every tick while the menu is up, else every 4th. */
#define POLL_OPEN        1
#define POLL_CLOSED      4
/* How often the page is rebuilt to catch live values: 10 Hz up, 1 Hz down. */
#define BUILD_OPEN       12
#define BUILD_CLOSED     120
/* An open or close request nobody took is withdrawn after half a second. */
#define REQUEST_US       500000ull
/* A CLOSES action waits this long for the menu to close and the game to play. */
#define DEFER_US         5000000ull

/* Notified once a session, per reason. */
#define WHY_NO_RENDERER  0x01
#define WHY_REFUSED      0x02
#define WHY_OFF          0x04

static int  g_state = MENU_OFF;
static const struct mh_desc *g_mh;
/* The block as the renderer laid it out: row slots and the size of one. */
static u8   g_row_cap;
static u8   g_row_size;
static int  g_logged_refusal;

static u32  g_page_seq;
static u32  g_posted_crc;
static int  g_posted;
static u32  g_last_ack;
static u8   g_hstate;
static u32  g_last_poll;
static u32  g_last_build;
static u8   g_req;
static u64  g_req_until;
static u8   g_deferred = 0xFF;
static u64  g_deferred_until;
static u8   g_why;

static int  g_enabled = 1;
static u32  g_enabled_version = 0xFFFFFFFFu;

int menu_state(void) { return g_state; }

u32 menu_mailbox(void)
{
	if (g_mh == NULL || (g_state != MENU_READY && g_state != MENU_REMOVING)) return 0;
	return g_mh->mailbox;
}

/* config.txt `menu.enabled`, 1 unless it says 0 (PROTOCOL.md 5.10). */
static int enabled(void)
{
	u32 v = config_version();

	if (v != g_enabled_version) {
		g_enabled_version = v;
		g_enabled = config_menu_enabled();
	}
	return g_enabled;
}

static u32 box(u32 off)
{
	return g_mh->mailbox + off;
}

void menu_forget(void)
{
	g_state = MENU_OFF;
	g_mh = NULL;
	g_posted = 0;
	g_hstate = MBOX_HSTATE_CLOSED;
	g_req = 0;
	g_deferred = 0xFF;
}

void menu_enter(const struct game_api *game)
{
	menu_forget();
	g_why = 0;
	g_logged_refusal = 0;
	menu_model_reset(8, 0);

	/*
	 * A renderer is code, so where code cannot be patched (RPCS3) there is no
	 * menu at all: nothing is installed and the combo says so.
	 */
	g_mh = game != NULL ? mh_desc_for_game(game->game_id) : NULL;
	if (g_mh == NULL || !plat_can_patch_code()) {
		g_mh = NULL;
		return;
	}

	/*
	 * Nothing goes into the game yet. The combo is the menu's only way in, so
	 * qwark sees every open coming, and the first press installs the renderer
	 * (menu_combo): a session in which nobody opens the menu has none of it.
	 */
	g_state = MENU_IDLE;
}

/*
 * Posts the current view into the mailbox: page_seq odd, the page, page_seq
 * even, three writes. Nothing is written when the page is the one already
 * there, unless `force`.
 */
static void post(int force)
{
	/* The view and the page it becomes, in the tick's scratch: 2.2 KB and 2.1 KB at most. */
	struct menu_view *v = (struct menu_view *)qscratch();
	u8 *img = qscratch() + 4096;
	u8 cap = g_row_cap;
	u32 len, crc, i;

	menu_model_view(v);
	g_last_build = now_tick();

	if (v->nrows > cap) v->nrows = cap;
	len = MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + (u32)v->nrows * g_row_size;
	memset(img, 0, len);
	be32_put(img, v->key);
	img[MBOX_OFF_NROWS - MBOX_OFF_PAGE_KEY] = v->nrows;
	img[MBOX_OFF_CURSOR - MBOX_OFF_PAGE_KEY] = v->cursor;
	img[MBOX_OFF_PFLAGS - MBOX_OFF_PAGE_KEY] = v->pflags;
	memcpy(img + MBOX_OFF_TITLE - MBOX_OFF_PAGE_KEY, v->title, MBOX_TITLE_LEN);
	memcpy(img + MBOX_OFF_HINT - MBOX_OFF_PAGE_KEY, v->hint, MBOX_HINT_LEN);
	for (i = 0; i < v->nrows; i++) {
		u8 *r = img + MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + i * g_row_size;
		r[MROW_OFF_KIND] = v->rows[i].kind;
		r[MROW_OFF_FLAGS] = v->rows[i].flags;
		be16_put(r + MROW_OFF_ID, v->rows[i].id);
		memcpy(r + MROW_OFF_LABEL, v->rows[i].label, MROW_LABEL_LEN);
		memcpy(r + MROW_OFF_VALUE, v->rows[i].value, MROW_VALUE_LEN);
		if (g_row_size >= MBOX_ROW_SIZE_DESC)
			memcpy(r + MROW_OFF_DESC, v->rows[i].desc, MROW_DESC_LEN);
	}

	crc = qcrc32(img, len);
	if (!force && g_posted && crc == g_posted_crc) return;

	if (mem_write_u32(box(MBOX_OFF_PAGE_SEQ), g_page_seq + 1) != ST_OK) return;
	if (mem_write(box(MBOX_OFF_PAGE_KEY), img, len) != ST_OK) return;
	if (mem_write_u32(box(MBOX_OFF_PAGE_SEQ), g_page_seq + 2) != ST_OK) return;
	g_page_seq += 2;
	g_posted_crc = crc;
	g_posted = 1;
}

/* Whether the code cave holds this build's renderer, byte for byte. */
static int code_matches(const struct mh_cave *c)
{
	u8 *buf = qscratch();
	u32 off = 0;

	while (off < c->len) {
		u32 n = c->len - off;
		if (n > QSCRATCH_BYTES) n = QSCRATCH_BYTES;
		if (mem_read(c->addr + off, buf, n) != ST_OK) return 0;
		if (memcmp(buf, c->bytes + off, n) != 0) return 0;
		off += n;
	}
	return 1;
}

#define INSTALL_REFUSED 100

/*
 * Puts the renderer into the game: every hook site must hold the game's own
 * word or the renderer's. All the game's: the data image, the first page, the
 * code, then the hook words in order. Some already the renderer's - this module
 * was loaded again into a running game, or an install was cut short - and the
 * code must be this build's to the byte; the rest goes in around it and the
 * mailbox is taken over as it stands. Anything else at a site is another mod's,
 * and nothing is written.
 *
 * It runs in the tick that saw the menu combo, the first in a session: some
 * twenty reads and eleven writes, the data image and the code one write each,
 * 7.5 KB in all for Deadlocked. The combo's request goes out right after it,
 * as the savefile helper's first request follows its install, so the first
 * open waits for nothing the others do not. Nothing can branch into a cave
 * before its hook word, every hook word goes in after the caves, and the
 * request byte goes in last of all.
 */
static int install(void)
{
	const struct mh_desc *d = g_mh;
	u8 head[MBOX_READ_HEAD];
	u32 w;
	int ours = 0;
	u8 i;
	int rc;

	for (i = 0; i < d->nhooks; i++) {
		if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return ST_IO_ERROR;
		if (w == d->hooks[i].original) continue;
		if (w == d->hooks[i].value) { ours++; continue; }
		if (!g_logged_refusal) {
			plat_log("menu: 0x%08X holds 0x%08X, neither the game's word nor the menu's; "
			         "another mod has it, so no menu", (int)d->hooks[i].addr, (int)w);
			g_logged_refusal = 1;
		}
		return INSTALL_REFUSED;
	}

	if (ours != 0) {
		for (i = 0; i < d->ncaves; i++) {
			if (d->caves[i].code && !code_matches(&d->caves[i])) {
				if (!g_logged_refusal) {
					plat_log("menu: a menu from another qwark build is in this game; "
					         "it comes back with a restart of the game");
					g_logged_refusal = 1;
				}
				return INSTALL_REFUSED;
			}
		}
		if (mem_read(d->mailbox, head, sizeof(head)) != ST_OK) return ST_IO_ERROR;
	}

	if (ours == 0 || be32_get(head + MBOX_OFF_MAGIC) != MBOX_MAGIC ||
	    be16_get(head + MBOX_OFF_VERSION) != MBOX_VERSION) {
		/* The data image: a mailbox with no event and no page yet. */
		for (i = 0; i < d->ncaves; i++) {
			if (d->caves[i].code) continue;
			rc = mem_write(d->caves[i].addr, d->caves[i].bytes, d->caves[i].len);
			if (rc != ST_OK) return rc;
		}
		if (mem_read(d->mailbox, head, sizeof(head)) != ST_OK) return ST_IO_ERROR;
	}

	/*
	 * The block's shape, as the renderer declared it: rows of 0x40 or 0x80
	 * bytes, at least three of them, all inside the block. A renderer that
	 * says otherwise is one this build cannot serve.
	 */
	if ((head[MBOX_OFF_ROW_SIZE] != MBOX_ROW_SIZE && head[MBOX_OFF_ROW_SIZE] != MBOX_ROW_SIZE_DESC) ||
	    head[MBOX_OFF_ROW_CAP] < 3 ||
	    MBOX_OFF_ROWS + (u32)head[MBOX_OFF_ROW_CAP] * head[MBOX_OFF_ROW_SIZE] >
	    be16_get(head + MBOX_OFF_SIZE)) {
		if (!g_logged_refusal) {
			plat_log("menu: the mailbox at 0x%08X has rows of %d bytes, %d of them, in %d bytes; "
			         "no menu", (int)d->mailbox, (int)head[MBOX_OFF_ROW_SIZE],
			         (int)head[MBOX_OFF_ROW_CAP], (int)be16_get(head + MBOX_OFF_SIZE));
			g_logged_refusal = 1;
		}
		return INSTALL_REFUSED;
	}

	/* Take the mailbox as it is: an event left from before is answered unread. */
	g_page_seq = (be32_get(head + MBOX_OFF_PAGE_SEQ) + 1u) & ~1u;
	g_last_ack = be32_get(head + MBOX_OFF_EV_SEQ);
	if (be32_get(head + MBOX_OFF_EV_ACK) != g_last_ack &&
	    mem_write_u32(box(MBOX_OFF_EV_ACK), g_last_ack) != ST_OK)
		return ST_IO_ERROR;
	g_hstate = head[MBOX_OFF_HSTATE];

	{
		u8 rows = head[MBOX_OFF_MAX_ROWS];

		g_row_cap = head[MBOX_OFF_ROW_CAP];
		g_row_size = head[MBOX_OFF_ROW_SIZE];
		if (g_row_cap > MENU_VIEW_MAX) g_row_cap = MENU_VIEW_MAX;
		if (rows > g_row_cap) rows = g_row_cap;
		menu_model_reset(rows, head[MBOX_OFF_CAPS]);
	}

	g_posted = 0;
	post(1);
	if (!g_posted) return ST_IO_ERROR;

	if (ours == 0) {
		for (i = 0; i < d->ncaves; i++) {
			if (!d->caves[i].code) continue;
			rc = mem_write(d->caves[i].addr, d->caves[i].bytes, d->caves[i].len);
			if (rc != ST_OK) return rc;
		}
	}

	for (i = 0; i < d->nhooks; i++) {
		if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return ST_IO_ERROR;
		if (w == d->hooks[i].value) continue;
		rc = mem_write_u32(d->hooks[i].addr, d->hooks[i].value);
		if (rc != ST_OK) return rc;
	}

	g_last_poll = now_tick();
	plat_log("menu: %s for game %d, mailbox 0x%08X",
	         ours != 0 ? "renderer found and taken over" : "renderer installed",
	         (int)d->game_id, (int)d->mailbox);
	return ST_OK;
}

/* The hooks back to the game's own words, last in first out. The caves stay. */
static int uninstall(void)
{
	const struct mh_desc *d = g_mh;
	u32 w;
	int i;

	for (i = (int)d->nhooks - 1; i >= 0; i--) {
		if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return ST_IO_ERROR;
		if (w != d->hooks[i].value) continue;
		if (mem_write_u32(d->hooks[i].addr, d->hooks[i].original) != ST_OK) return ST_IO_ERROR;
	}
	plat_log("menu: switched off, the renderer's hooks are out");
	return ST_OK;
}

/*
 * install(), and the state it leaves: READY, REFUSED for another mod's word or
 * another build's renderer, IDLE for a write that failed. Nothing retries on
 * its own; the next combo press tries again. Answers 1 when the menu serves.
 */
static int try_install(void)
{
	int rc = install();

	if (rc == ST_OK) {
		g_state = MENU_READY;
		return 1;
	}
	if (rc == INSTALL_REFUSED) {
		g_state = MENU_REFUSED;
	} else {
		plat_log("menu: install failed (%d); the next press tries again", rc);
		g_state = MENU_IDLE;
	}
	return 0;
}

/*
 * Whether the hooks may come out: never while the menu is up or the game is
 * paused, since the menu's code is running then, and not while a mode change
 * is queued either.
 */
static int safe_to_remove(void)
{
	const struct mh_desc *d = g_mh;
	u32 mode = 0, pending = 0, screen = 0;

	if (g_hstate != MBOX_HSTATE_CLOSED) return 0;
	if (mem_read_u32(d->mode_addr, &mode) != ST_OK) return 0;
	if (mem_read_u32(d->pending_addr, &pending) != ST_OK) return 0;
	if (mem_read_u32(d->screen_addr, &screen) != ST_OK) return 0;
	return mode != d->mode_paused && pending == d->pending_none && screen != d->screen_id;
}

/* Whether the game is back in play: the mode word 0 with no change queued. */
static int game_playing(void)
{
	const struct mh_desc *d = g_mh;
	u32 mode = 1, pending = 0;

	if (mem_read_u32(d->mode_addr, &mode) != ST_OK) return 0;
	if (mem_read_u32(d->pending_addr, &pending) != ST_OK) return 0;
	return mode == 0 && pending == d->pending_none;
}

static void request(u8 what)
{
	if (mem_write_u8(box(MBOX_OFF_REQUEST), what) != ST_OK) return;
	g_req = what;
	g_req_until = plat_time_us() + REQUEST_US;
}

/* A CLOSES action: run it once the menu is closed and the game plays again. */
static void deferred_tick(void)
{
	const struct feature_desc *f;
	char msg[96];
	int rc;

	if (g_deferred == 0xFF) return;
	f = feature(g_deferred);

	if (g_hstate == MBOX_HSTATE_CLOSED && game_playing()) {
		rc = features_trigger(g_deferred);
		g_deferred = 0xFF;
		if (rc != ST_OK && f != NULL) {
			qstrcpy(msg, sizeof(msg), "RaCMAN: ");
			qstrcat(msg, sizeof(msg), f->label);
			qstrcat(msg, sizeof(msg), ": ");
			qstrcat(msg, sizeof(msg), status_text((u8)rc));
			plat_notify(msg);
		}
		return;
	}

	if (plat_time_us() >= g_deferred_until) {
		g_deferred = 0xFF;
		if (f != NULL) {
			qstrcpy(msg, sizeof(msg), "RaCMAN: ");
			qstrcat(msg, sizeof(msg), f->label);
			qstrcat(msg, sizeof(msg), " was dropped, the game did not go back to play");
			plat_notify(msg);
		}
	}
}

/* The mailbox: requests, events, live values. Every POLL_* ticks. */
static void serve(void)
{
	u8 head[MBOX_READ_HEAD];
	u32 tick = now_tick();
	u32 ev;
	int dirty = 0;
	u8 hstate;

	if (tick - g_last_poll < (g_hstate == MBOX_HSTATE_OPEN ? POLL_OPEN : POLL_CLOSED)) return;
	g_last_poll = tick;

	if (mem_read(g_mh->mailbox, head, sizeof(head)) != ST_OK) return;
	if (be32_get(head + MBOX_OFF_MAGIC) != MBOX_MAGIC ||
	    be16_get(head + MBOX_OFF_VERSION) != MBOX_VERSION) {
		/*
		 * Something wrote over the mailbox, and with it maybe the renderer's
		 * TOC and descriptors, under hook words that are still in. Install
		 * again at once, which looks first and puts the data image back.
		 */
		plat_log("menu: the mailbox at 0x%08X is gone; installing again", (int)g_mh->mailbox);
		try_install();
		return;
	}

	hstate = head[MBOX_OFF_HSTATE];
	if (hstate != g_hstate) {
		g_hstate = hstate;
		if (hstate == MBOX_HSTATE_OPEN) dirty = 1;
	}

	if (g_req != 0) {
		if (head[MBOX_OFF_REQUEST] == MBOX_REQ_NONE) {
			g_req = 0;
		} else if (plat_time_us() >= g_req_until) {
			if (head[MBOX_OFF_REQUEST] == g_req) mem_write_u8(box(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
			g_req = 0;
		}
	}

	ev = be32_get(head + MBOX_OFF_EV_SEQ);
	if (ev != g_last_ack) {
		u8 id;

		menu_model_event(head[MBOX_OFF_EV_KIND], head[MBOX_OFF_EV_ROW],
		                 be16_get(head + MBOX_OFF_EV_ID), be32_get(head + MBOX_OFF_EV_KEY));
		g_last_ack = ev;
		mem_write_u32(box(MBOX_OFF_EV_ACK), ev);
		dirty = 1;

		id = take_deferred();
		if (id != 0xFF) {
			if ((g_caps & MBOX_CAP_REQUEST) == 0) {
				features_trigger(id);
			} else {
				g_deferred = id;
				g_deferred_until = plat_time_us() + DEFER_US;
				if (g_hstate == MBOX_HSTATE_OPEN) request(MBOX_REQ_CLOSE);
			}
		}
	}

	if (dirty || tick - g_last_build >= (g_hstate == MBOX_HSTATE_OPEN ? BUILD_OPEN : BUILD_CLOSED))
		post(0);

	deferred_tick();
}

void menu_tick(void)
{
	if (g_mh == NULL) return;

	switch (g_state) {
	case MENU_READY:
		if (enabled()) { serve(); return; }
		g_state = MENU_REMOVING;
		/* fall through */
	case MENU_REMOVING:
		if (enabled()) { g_state = MENU_READY; serve(); return; }
		/* The menu keeps working until it is closed; then the hooks come out. */
		serve();
		if (g_state == MENU_REMOVING && safe_to_remove() && uninstall() == ST_OK) {
			g_state = MENU_IDLE;
			g_deferred = 0xFF;
		}
		return;

	default:
		/* Nothing of the menu is in the game: there is nothing to do until the combo. */
		return;
	}
}

static void unavailable(u8 why, const char *msg)
{
	if ((g_why & why) != 0) return;
	g_why |= why;
	plat_notify(msg);
}

void menu_combo(void)
{
	if (g_state == MENU_OFF || g_mh == NULL) {
		unavailable(WHY_NO_RENDERER, plat_can_patch_code()
		            ? "RaCMAN menu: not available in this game yet"
		            : "RaCMAN menu: not available under RPCS3");
		return;
	}
	if (!enabled()) {
		unavailable(WHY_OFF, "RaCMAN menu: switched off (menu.enabled = 0 in config.txt)");
		return;
	}
	if (g_state == MENU_REMOVING) g_state = MENU_READY;

	/*
	 * The first press of a session puts the renderer in, here in the tick that
	 * saw it, and asks it to open straight after (install() has the cost). So
	 * does the first press after a refusal, or after menu.enabled went to 0
	 * and back: no timer retries in between.
	 */
	if (g_state != MENU_READY && !try_install()) {
		if (g_state == MENU_REFUSED)
			unavailable(WHY_REFUSED, "RaCMAN menu: another mod holds the menu's hooks");
		return;
	}

	if ((g_caps & MBOX_CAP_REQUEST) == 0) return;
	if (g_hstate == MBOX_HSTATE_OPEN) {
		request(MBOX_REQ_CLOSE);
	} else {
		/* The page it opens on is the current one, values and all. */
		post(0);
		request(MBOX_REQ_OPEN);
	}
}
