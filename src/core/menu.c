#include "menu.h"
#include "features.h"
#include "session.h"
#include "config.h"
#include "mem.h"
#include "util.h"
#include "manips.h"
#include "savefile.h"
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
static u8 g_max_rows = 8;       /* rows a MAIN page takes at once */
static u8 g_cfg_rows;           /* the same for a CONFIG page, 0: the renderer has none */
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

/*
 * An action that runs once the menu has closed (MROWF_CLOSES): a feature, a
 * planet load or a save of the console's library. The model sets it, the
 * driver asks the renderer to close and runs it once the game plays again.
 */
#define DEFER_NONE    0
#define DEFER_FEATURE 1
#define DEFER_PLANET  2
#define DEFER_RESTORE 3
static struct {
	u8   kind;
	u8   arg;
	u8   waiting;           /* the driver has asked for the close */
	u64  until;
	char cat[SAVEFILE_NAME_LEN + 1];
	char name[SAVEFILE_NAME_LEN + 1];
	char label[32];
} g_defer;

/*
 * The unlock table as the game last read it: one read a page build, again
 * after a write (unlocks()).
 */
static struct {
	const struct game_unlock *list;
	const char * const *cats;
	const struct unlock_field_desc *fields;
	u8  n;
	u8  ncats;
	u8  ok;
	u32 tick;
} g_ul;

static u32 now_tick(void)
{
	return session_tick_count();
}

static const struct game_menu *gmenu(void)
{
	return g_game != NULL ? g_game->menu : NULL;
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

/* Notes a failure, and answers what it was given. */
static int answer(u16 id, int rc, const char *label)
{
	if (rc != ST_OK) note(id, (u8)rc, label);
	return rc;
}

/* An action's answer, "done" included, and answers what it was given. */
static int done(u16 id, int rc, const char *label)
{
	note(id, (u8)rc, label);
	return rc;
}

static int unlocks(void)
{
	u32 t = now_tick();

	if (g_game == NULL || g_game->unlock_list == NULL || g_game->unlock_read == NULL) return 0;
	if (g_ul.ok && g_ul.tick == t) return 1;
	g_ul.ok = g_game->unlock_list(&g_ul.list, &g_ul.n, &g_ul.cats, &g_ul.ncats,
	                              &g_ul.fields) == ST_OK;
	g_ul.tick = t;
	return g_ul.ok;
}

static const struct game_unlock *unlock_row(u8 id)
{
	u8 i;

	if (!unlocks()) return NULL;
	for (i = 0; i < g_ul.n; i++) {
		if (g_ul.list[i].id == id) return &g_ul.list[i];
	}
	return NULL;
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

static void value_text(const struct feature_desc *f, char *out, u32 cap)
{
	char step[16];
	s64 v = value_number(f, feature_value(f));
	u32 n = 0;

	if (v < 0) {
		out[n++] = '-';
		v = -v;
	}
	qfmt_u32(out + n, cap - n, (u32)v);
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

/* The option an ENUM is on, 0 for a value off the list. */
static u8 enum_index(const struct feature_desc *f, u8 count)
{
	u32 v = feature_value(f) - f->min;
	return v < count ? (u8)v : 0;
}

/*
 * A toggle with an auto flag is a three-way choice: Off, On, and Auto, which
 * is on and on again at every boot (the client's "A" box). A LIVE toggle has
 * no auto flag (features_set_auto refuses it), so it is Off or On.
 */
static int toggle_auto(const struct feature_desc *f)
{
	return (f->flags & FEATURE_FLAG_LIVE) == 0;
}

static int toggle_now(const struct feature_desc *f)
{
	u64 bit = (u64)1 << f->id;

	if (f->id >= QWARK_MAX_FEATURES || (features_toggle_state() & bit) == 0) return 0;
	return (toggle_auto(f) && (features_toggle_auto() & bit) != 0) ? 2 : 1;
}

static int toggle_set(const struct feature_desc *f, int want)
{
	int rc = ST_OK;

	if ((toggle_now(f) != 0) != (want != 0)) rc = features_set(f->id, (u32)(want != 0));
	if (rc == ST_OK && toggle_auto(f)) rc = features_set_auto(f->id, want == 2);
	return rc;
}

/* `cur` moved by `dir` (0 counts as forward) among `n` choices, wrapping. */
static u8 cycle(u8 cur, u8 n, int dir)
{
	if (n == 0) return 0;
	if (cur >= n) cur = 0;
	return (u8)((cur + n + (dir < 0 ? n - 1 : 1)) % n);
}

/* ---------------------------------------------------------------- pages */

static u8 page_kind(u16 page) { return (u8)(page >> 8); }
static u8 page_arg(u16 page)  { return (u8)page; }

/* The widget a page is drawn with: a list of features is a CONFIG page where the renderer has one. */
static u8 page_type(u16 page)
{
	u8 k = page_kind(page);

	if (g_cfg_rows == 0) return MBOX_PTYPE_MAIN;
	return (k == MENU_PAGE_GROUP || k == MENU_PAGE_SECTION || k == MENU_PAGE_MANIPS ||
	        k == MENU_PAGE_UNLOCK_CAT || k == MENU_PAGE_UNLOCK_ACTIONS) ? MBOX_PTYPE_CONFIG : MBOX_PTYPE_MAIN;
}

/*
 * Whether a page's rows follow the game while the menu is up: the pages of
 * features, manips and unlocks. The others list links, planets and the save
 * library, none of which moves while the game is paused, and are built again
 * only when the player does something (or a row's answer runs out).
 */
static int page_live(u16 page)
{
	u8 k = page_kind(page);
	return k == MENU_PAGE_GROUP || k == MENU_PAGE_SECTION || k == MENU_PAGE_CHOICE ||
	       k == MENU_PAGE_MANIPS || k == MENU_PAGE_UNLOCK_CAT;
}

/*
 * Round 3: the root is the trainer, "Trainer" as qwark writes it, which each
 * renderer cases as its game's lists ("TRAINER" in Deadlocked and UYA), the
 * name of the pause-menu row that opens it.
 */
static const char *page_name(u8 kind)
{
	switch (kind) {
	case MENU_PAGE_PLANETS: return "Planets";
	case MENU_PAGE_MANIPS:  return "Manips";
	case MENU_PAGE_UNLOCKS: return "Unlocks";
	case MENU_PAGE_SAVES:   return "Save files";
	case MENU_PAGE_UNLOCK_ACTIONS:
		return gmenu() != NULL && gmenu()->unlock_actions_title != NULL ? gmenu()->unlock_actions_title : "Unlocks";
	default:                return "Trainer";
	}
}

/* Whether the game puts the Unlocks page's actions on a page of their own (struct game_menu). */
static int unlock_actions_page(void)
{
	const struct game_menu *gm = gmenu();
	u8 i;

	if (gm == NULL || gm->unlock_actions_title == NULL) return 0;
	for (i = 0; i < gm->nunlock_actions; i++) {
		if (feature(gm->unlock_actions[i]) != NULL) return 1;
	}
	return 0;
}

/* The save library's category with this place, into `out`; 0 when there is none. */
static int save_category(u8 index, char *out)
{
	return index < savefile_menu_entry(NULL, index, out, SAVEFILE_NAME_LEN + 1) && out[0] != 0;
}

static u8 push_id(u16 *ids, u8 n, u16 id)
{
	if (n < MENU_ITEMS_MAX) ids[n++] = id;
	return n;
}

/* Every row of a page, as row ids, in order; how many. */
static u8 page_items(u16 page, u16 *ids)
{
	const struct game_describe *d = desc();
	const struct game_menu *gm = gmenu();
	const struct feature_desc *f;
	const char * const *opts;
	char cat[SAVEFILE_NAME_LEN + 1];
	u8 arg = page_arg(page);
	u8 n = 0, i, j, count;

	switch (page_kind(page)) {
	case MENU_PAGE_ROOT:
		if (gm != NULL && gm->nsections != 0) {
			for (i = 0; i < gm->nsections; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_SECTION, i));
		} else if (d != NULL) {
			/* Round 1's menu: the DESCRIBE groups that have a feature. */
			for (i = 0; i < d->ngroups; i++) {
				for (j = 0; j < d->nfeatures; j++) {
					if (d->features[j].group == i) break;
				}
				if (j < d->nfeatures) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_GROUP, i));
			}
		}
		if (g_game != NULL && g_game->planet_names != NULL && g_game->planet_load != NULL)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_PLANETS));
		if (g_game != NULL && g_game->manips != NULL)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_MANIPS));
		if (g_game != NULL && g_game->unlock_list != NULL && g_game->unlock_set != NULL)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_UNLOCKS));
		if (savefile_library_gate() != ST_UNSUPPORTED)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_SAVES));
		return push_id(ids, n, MENU_ROW_ID(MENU_ROW_TEXT, 0));

	case MENU_PAGE_GROUP:
		if (d == NULL) return 0;
		for (i = 0; i < d->nfeatures; i++) {
			if (d->features[i].group == arg)
				n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_FEATURE, d->features[i].id));
		}
		return n;

	case MENU_PAGE_SECTION:
		if (gm == NULL || arg >= gm->nsections) return 0;
		for (i = 0; i < gm->sections[arg].nitems; i++) {
			u8 it = gm->sections[arg].items[i];
			if (it >= MENU_ITEM_SLOT) {
				if (g_game->save_blob != NULL) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_POS, it));
			} else if (feature(it) != NULL) {
				n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_FEATURE, it));
			}
		}
		return n;

	case MENU_PAGE_CHOICE:
		f = feature(arg);
		count = f != NULL ? enum_options(f, &opts) : 0;
		for (i = 0; i < count; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_OPTION, i));
		return n;

	case MENU_PAGE_PLANETS: {
		const char * const *names;
		if (g_game == NULL || g_game->planet_names == NULL) return 0;
		names = g_game->planet_names(&count);
		for (i = 0; i < count; i++) {
			u8 p = g_game->planet_order != NULL ? g_game->planet_order[i] : i;
			if (p >= count || names[p] == NULL || names[p][0] == '(' || names[p][0] == 0) continue;
			if (gm != NULL && gm->planet_ok != NULL && !gm->planet_ok(p)) continue;
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PLANET, p));
		}
		return n;
	}

	case MENU_PAGE_MANIPS:
		if (g_game == NULL || g_game->manips == NULL) return 0;
		for (i = MENU_MANIP_CATEGORY; i <= MENU_MANIP_APPLY; i++)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_MANIP, i));
		/*
		 * MANIP_LIST's headers' features, those DESCRIBE has (UYA's trophy
		 * rows are not where the reset cannot work), each ACTION that names a
		 * status readout followed by a row of that status (UYA's Reset
		 * trophies, PROTOCOL.md 5.14).
		 */
		for (i = 0; i < g_game->manips->nheaders; i++) {
			for (j = 0; j < g_game->manips->headers[i].nfeatures; j++) {
				u8 id = g_game->manips->headers[i].features[j];
				f = feature(id);
				if (f == NULL) continue;
				n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_FEATURE, id));
				if (f->kind == FEATURE_ACTION && f->readout < QWARK_MAX_READOUTS &&
				    gm != NULL && gm->action_status != NULL)
					n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_STATUS, id));
			}
		}
		return n;

	case MENU_PAGE_UNLOCKS:
		if (unlocks()) {
			for (i = 0; i < g_ul.ncats; i++) {
				for (j = 0; j < g_ul.n; j++) {
					if (g_ul.list[j].category == i) break;
				}
				if (j < g_ul.n) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_UCAT, i));
			}
		}
		/* Round 3: the actions on a page of their own where the game names one, one row for it here. */
		if (unlock_actions_page())
			return push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_UNLOCK_ACTIONS));
		/* fall through */
	case MENU_PAGE_UNLOCK_ACTIONS:
		if (page_kind(page) == MENU_PAGE_UNLOCK_ACTIONS && !unlock_actions_page()) return n;
		if (gm != NULL) {
			for (i = 0; i < gm->nunlock_actions; i++) {
				if (feature(gm->unlock_actions[i]) != NULL)
					n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_FEATURE, gm->unlock_actions[i]));
			}
		}
		return n;

	case MENU_PAGE_UNLOCK_CAT:
		if (!unlocks()) return 0;
		for (i = 0; i < g_ul.n; i++) {
			if (g_ul.list[i].category == arg) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_UNLOCK, g_ul.list[i].id));
		}
		return n;

	case MENU_PAGE_SAVES:
		count = savefile_menu_entry(NULL, 0, cat, sizeof(cat));
		for (i = 0; i < count; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_SCAT, i));
		return n;

	case MENU_PAGE_SAVE_CAT: {
		char name[SAVEFILE_NAME_LEN + 1];
		if (!save_category(arg, cat)) return 0;
		count = savefile_menu_entry(cat, 0, name, sizeof(name));
		for (i = 0; i < count; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_SAVE, i));
		return n;
	}

	default:
		return 0;
	}
}

static void page_title(u16 page, char *out, u32 cap)
{
	const struct game_describe *d = desc();
	const struct game_menu *gm = gmenu();
	const struct feature_desc *f;
	char cat[SAVEFILE_NAME_LEN + 1];
	u8 arg = page_arg(page);

	memset(out, 0, cap);
	switch (page_kind(page)) {
	case MENU_PAGE_GROUP:
		if (d != NULL && arg < d->ngroups) qstrcpy(out, cap, d->groups[arg]);
		return;
	case MENU_PAGE_SECTION:
		if (gm != NULL && arg < gm->nsections) qstrcpy(out, cap, gm->sections[arg].title);
		return;
	case MENU_PAGE_CHOICE:
		f = feature(arg);
		if (f != NULL) qstrcpy(out, cap, f->label);
		return;
	case MENU_PAGE_UNLOCK_CAT:
		if (unlocks() && arg < g_ul.ncats) qstrcpy(out, cap, g_ul.cats[arg]);
		return;
	case MENU_PAGE_SAVE_CAT:
		if (save_category(arg, cat)) qstrcpy(out, cap, cat);
		return;
	default:
		qstrcpy(out, cap, page_name(page_kind(page)));
		return;
	}
}

/* ---------------------------------------------------------------- rows */

static void row(struct menu_row *r, u8 kind, u8 flags, const char *label, const char *value,
                const char *text)
{
	r->kind = kind;
	r->flags = flags;
	set_text(r->label, sizeof(r->label), label);
	if (value != NULL) set_text(r->value, sizeof(r->value), value);
	set_text(r->desc, sizeof(r->desc), text);
}

static u8 steps(void)
{
	return (g_caps & MBOX_CAP_STEPS) != 0 ? MROWF_STEPS : 0;
}

static const char *const g_toggle_text[3] = { "Off", "On", "Auto" };

/*
 * Round 3, what a native slider needs: a VALUE whose range is narrower than
 * its 32-bit field (explicit bounds, or an 8- or 16-bit field) is flagged
 * MROWF_RANGE and carries its value, its bounds and its step as numbers, its
 * label held to the 23 characters such a row leaves room for; one whose range
 * spans MENU_SLIDER_SPAN or fewer is MROWF_SLIDER as well.
 */
static void value_range_row(const struct feature_desc *f, struct menu_row *r)
{
	s64 lo, hi;

	value_range(f, &lo, &hi);
	if (hi - lo >= (s64)0xFFFFFFFFll) return;
	r->flags |= MROWF_RANGE;
	if (hi - lo <= MENU_SLIDER_SPAN) r->flags |= MROWF_SLIDER;
	r->range[0] = (u32)value_number(f, feature_value(f));
	r->range[1] = (u32)lo;
	r->range[2] = (u32)hi;
	r->range[3] = g_pow10[f->id < QWARK_MAX_FEATURES ? g_step[f->id] : 0];
	memset(r->label + MROW_RANGE_LABEL - 1, 0, sizeof(r->label) - (MROW_RANGE_LABEL - 1));
}

static void feature_row(const struct feature_desc *f, struct menu_row *r)
{
	const char * const *opts;
	u8 count;

	switch (f->kind) {
	case FEATURE_TOGGLE: {
		int st = toggle_now(f);
		row(r, MROW_TOGGLE, MROWF_ENABLED | steps() | (st != 0 ? MROWF_ON : 0), f->label,
		    g_toggle_text[st], toggle_auto(f) ? "Auto also turns it on whenever the game starts."
		                                      : "On or off.");
		break;
	}
	case FEATURE_ACTION:
		/*
		 * Loading the set-aside file runs the game's own load, which
		 * Deadlocked's helper does from a pause of its own, so it waits for
		 * the menu to close and the game to be back in play.
		 */
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0)
			row(r, MROW_ACTION, MROWF_ENABLED | MROWF_CLOSES, f->label, "", "Closes the menu, then runs.");
		else
			row(r, MROW_ACTION, MROWF_ENABLED, f->label, "", "Runs it.");
		break;
	case FEATURE_VALUE:
		/*
		 * Left and Right step the number, the cross changes the step: a
		 * stepper wants both, so a renderer that cannot send Left and Right
		 * shows the number and nothing more.
		 */
		if (steps() != 0)
			row(r, MROW_VALUE, MROWF_ENABLED | MROWF_STEPS, f->label, NULL,
			    "Left and Right step it; select changes the step.");
		else
			row(r, MROW_INFO, 0, f->label, NULL, NULL);
		value_text(f, r->value, sizeof(r->value));
		if (r->kind == MROW_VALUE) value_range_row(f, r);
		break;
	case FEATURE_ENUM:
		count = enum_options(f, &opts);
		row(r, MROW_CHOICE, MROWF_ENABLED | steps(), f->label,
		    count != 0 && opts[enum_index(f, count)] != NULL ? opts[enum_index(f, count)] : "",
		    "Left and Right change it.");
		break;
	case FEATURE_COLOR:
		row(r, MROW_INFO, 0, f->label, "#", NULL);
		qfmt_hex(r->value + 1, sizeof(r->value) - 1, feature_value(f) & 0x00FFFFFFu, 6);
		break;
	default:
		row(r, MROW_INFO, 0, f->label, NULL, NULL);
		break;
	}
}

/*
 * The value of the set's i-th name for this item: the set's own, but for the
 * last, which is the item's own last value where the game says it stops short
 * (struct game_menu, unlock_top), so MAX is each weapon's maximum.
 */
static u32 unlock_value(const struct game_menu *gm, const struct game_unlock *u, u8 i)
{
	u32 top;

	if (i + 1 == gm->nunlock && gm->unlock_top != NULL) {
		top = gm->unlock_top(u->id);
		if (top != 0) return top;
	}
	return gm->unlock_values[i];
}

/*
 * What an owned item with no value field shows: the set's own name for the
 * first owned value (RaC2's "Unlocked", in its title case), but where that is
 * a version (Deadlocked's "V1"), UNLOCKED. A locked one shows the set's first.
 */
static const char *owned_name(const struct game_menu *gm)
{
	const char *p;

	if (gm == NULL || gm->nunlock < 2) return "UNLOCKED";
	p = gm->unlock_prefix;
	if (p != NULL && p[0] != 0 && qstrneq(gm->unlock_names[1], p, qstrlen(p))) return "UNLOCKED";
	return gm->unlock_names[1];
}

/* An unlock row's value: its place in the game's value set, and that value's name. */
static u8 unlock_now(const struct game_unlock *u, char *out, u32 cap)
{
	const struct game_menu *gm = gmenu();
	u32 v[4] = { 0, 0, 0, 0 };
	u8 i, below = 1;

	g_game->unlock_read(u, v);
	if (v[0] == 0) {
		qstrcpy(out, cap, gm != NULL && gm->nunlock != 0 ? gm->unlock_names[0] : "LOCKED");
		return 0;
	}
	if (gm == NULL || gm->nunlock < 2 || (u->fields & (1u << gm->unlock_field)) == 0) {
		qstrcpy(out, cap, owned_name(gm));
		return 1;
	}
	/*
	 * Owned, and the field reads 0: a value the game cannot report (UYA's
	 * GameCube weapons keep theirs where it cannot be read back), shown as the
	 * set's first owned name rather than as a number.
	 */
	if (v[gm->unlock_field] == 0) {
		qstrcpy(out, cap, gm->unlock_names[1]);
		return 1;
	}
	for (i = 1; i < gm->nunlock; i++) {
		if (v[gm->unlock_field] == unlock_value(gm, u, i)) {
			qstrcpy(out, cap, gm->unlock_names[i]);
			return i;
		}
		if (v[gm->unlock_field] > unlock_value(gm, u, i)) below = i;
	}
	/* Off the list: the number, or the name of the value under it. */
	if (gm->unlock_prefix != NULL) {
		qstrcpy(out, cap, gm->unlock_prefix);
		qfmt_u32(out + qstrlen(out), cap - qstrlen(out), v[gm->unlock_field]);
	} else {
		qstrcpy(out, cap, gm->unlock_names[below]);
	}
	return below;
}

/* How many values an unlock row cycles through. */
static u8 unlock_count(const struct game_unlock *u)
{
	const struct game_menu *gm = gmenu();

	if (gm == NULL || gm->nunlock < 2 || (u->fields & (1u << gm->unlock_field)) == 0) return 2;
	return gm->nunlock;
}

static void build_row(u16 page, u16 id, struct menu_row *r)
{
	const struct game_describe *d = desc();
	const struct game_menu *gm = gmenu();
	const struct feature_desc *f;
	const struct game_unlock *u;
	const char * const *opts;
	char text[SAVEFILE_NAME_LEN + 1], cat[SAVEFILE_NAME_LEN + 1];
	u8 arg = (u8)id, count;
	const char *n;

	memset(r, 0, sizeof(*r));
	r->id = id;

	switch ((u8)(id >> 8)) {
	case MENU_ROW_GROUP:
		row(r, MROW_LINK, MROWF_ENABLED, d != NULL && arg < d->ngroups ? d->groups[arg] : "", "", NULL);
		break;
	case MENU_ROW_SECTION:
		row(r, MROW_LINK, MROWF_ENABLED, gm->sections[arg].title, "", NULL);
		break;
	case MENU_ROW_PAGE:
		row(r, MROW_LINK, MROWF_ENABLED, page_name(arg), "", NULL);
		break;
	case MENU_ROW_UCAT:
		row(r, MROW_LINK, MROWF_ENABLED, unlocks() && arg < g_ul.ncats ? g_ul.cats[arg] : "", "", NULL);
		break;
	case MENU_ROW_SCAT:
		save_category(arg, text);
		row(r, MROW_LINK, MROWF_ENABLED, text, "", NULL);
		break;
	case MENU_ROW_TEXT:
		row(r, MROW_INFO, 0, arg == 0 ? "qwark build " : "Nothing here", "", NULL);
		if (arg == 0) qfmt_u32(r->label + qstrlen(r->label), 8, QWARK_BUILD);
		break;

	case MENU_ROW_FEATURE:
		f = feature(arg);
		if (f != NULL) feature_row(f, r);
		break;

	case MENU_ROW_OPTION:
		f = feature(page_arg(page));
		if (f == NULL) break;
		count = enum_options(f, &opts);
		row(r, MROW_OPTION, MROWF_ENABLED | (enum_index(f, count) == arg ? MROWF_ON : 0),
		    arg < count && opts[arg] != NULL ? opts[arg] : "", "", "Select picks it.");
		break;

	case MENU_ROW_POS:
		if (arg == MENU_ITEM_SLOT) {
			row(r, MROW_CHOICE, MROWF_ENABLED | steps(), "Position slot", "",
			    "The slot Save position and Load position use.");
			qfmt_u32(r->value, sizeof(r->value), (u32)config_selected_slot() + 1u);
		} else {
			row(r, MROW_ACTION, MROWF_ENABLED, arg == MENU_ITEM_POS_SAVE ? "Save position" : "Load position",
			    "", arg == MENU_ITEM_POS_SAVE ? "Keeps where you stand in the slot."
			                                  : "Puts you back where the slot says.");
		}
		break;

	case MENU_ROW_PLANET: {
		const char * const *names = g_game->planet_names(&count);
		row(r, MROW_ACTION, MROWF_ENABLED | MROWF_CLOSES, arg < count ? names[arg] : "", "", NULL);
		break;
	}

	case MENU_ROW_MANIP: {
		const struct game_manips *m = g_game->manips;
		u8 sel = manips_selected(), opts_now = manips_options_now(), on;
		switch (arg) {
		case MENU_MANIP_CATEGORY:
			row(r, MROW_CHOICE, MROWF_ENABLED | steps(), "Category", "None",
			    "Whose manips are applied.");
			for (count = 0; count < m->ngroups; count++) {
				if (m->groups[count].id == sel) set_text(r->value, sizeof(r->value), m->groups[count].name);
			}
			break;
		case MENU_MANIP_AUTO:
		case MENU_MANIP_REMEMBER:
			on = (opts_now >> (arg - MENU_MANIP_AUTO)) & 1u;
			row(r, MROW_TOGGLE, MROWF_ENABLED | steps() | (on ? MROWF_ON : 0),
			    arg == MENU_MANIP_AUTO ? "Auto-reset" : "Remember", g_toggle_text[on],
			    arg == MENU_MANIP_AUTO ? "Applies them again at every file load."
			                           : "Keeps the category when the game starts again.");
			break;
		default:
			row(r, MROW_ACTION, MROWF_ENABLED, "Apply now", "", "Applies the category's manips now.");
			break;
		}
		break;
	}

	case MENU_ROW_STATUS:
		/* An ACTION's status readout, as the game words it (UYA's trophy reset). */
		f = feature(arg);
		if (f == NULL || gm == NULL || gm->action_status == NULL || f->readout >= QWARK_MAX_READOUTS) break;
		row(r, MROW_INFO, 0, f->readout < d->nreadouts ? d->readouts[f->readout] : f->label, "", NULL);
		gm->action_status(arg, session_readout(f->readout), r->value, sizeof(r->value));
		r->value[sizeof(r->value) - 1] = 0;
		break;

	case MENU_ROW_UNLOCK:
		u = unlock_row(arg);
		if (u == NULL) break;
		row(r, MROW_CHOICE, MROWF_ENABLED | steps(), u->name, "",
		    unlock_count(u) == 2 ? "Locked or unlocked." : "Locked, or which version it is.");
		if (unlock_now(u, r->value, sizeof(r->value)) != 0) r->flags |= MROWF_ON;
		break;

	case MENU_ROW_SAVE:
		text[0] = 0;
		if (save_category(page_arg(page), cat)) savefile_menu_entry(cat, arg, text, sizeof(text));
		/* The name without its ".sav". */
		count = (u8)qstrlen(text);
		if (count > 4) text[count - 4] = 0;
		row(r, MROW_ACTION, MROWF_ENABLED | MROWF_CLOSES, text, "", NULL);
		break;

	default:
		break;
	}

	n = note_for(page, id);
	if (n != NULL) set_text(r->value, sizeof(r->value), n);
}

/* ----------------------------------------------------------- the windows */

/*
 * A page longer than the renderer's screen is cut into windows of `m` rows.
 * The first has as many of the page's rows as fit and a MORE row last; a
 * middle one a PREV row first and a MORE row last; the last a PREV row and the
 * rest. With a renderer that has no back control every page but the root ends
 * in a BACK row as well, and the screen is one row shorter for the page's own.
 */
static u8 screen_rows(u16 page, u8 depth)
{
	u8 m = page_type(page) == MBOX_PTYPE_CONFIG ? g_cfg_rows : g_max_rows;

	if ((g_caps & MBOX_CAP_BACK) == 0 && depth > 1) m--;
	return m;
}

static void window(u8 n, u8 start, u8 m, u8 *content, int *prev, int *more)
{
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
static u8 window_prev(u8 start, u8 m)
{
	if (start <= (u8)(m - 1)) return 0;
	return (u8)(start - (m - 2));
}

/* The window that shows row `index` of a page of n rows, and that row's place in it. */
static u8 window_of(u8 n, u8 index, u8 m, u8 *pos)
{
	u8 start = 0, content;
	int prev, more;

	for (;;) {
		window(n, start, m, &content, &prev, &more);
		if (index < start + content || !more) break;
		start = (u8)(start + content);
	}
	*pos = (u8)((prev ? 1 : 0) + (index - start));
	return start;
}

/* Which window `start` is, and how many there are, counted from 1. */
static void window_number(u8 n, u8 start, u8 m, u8 *k, u8 *total)
{
	u8 s = 0, content;
	int prev, more;

	*k = 1;
	*total = 1;
	for (;;) {
		window(n, s, m, &content, &prev, &more);
		if (s < start) (*k)++;
		if (!more) break;
		s = (u8)(s + content);
		(*total)++;
	}
	if (*k > *total) *k = *total;
}

/* A start that is no window's (the page got shorter) goes back to the first. */
static u8 valid_start(u8 n, u8 start, u8 m)
{
	u8 s = 0, content;
	int prev, more;

	for (;;) {
		if (s == start) return start;
		window(n, s, m, &content, &prev, &more);
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
	u16 ids[MENU_ITEMS_MAX];
	u8 n = page_items(cur->page, ids);
	u8 m = screen_rows(cur->page, g_depth);
	u8 content;
	int prev, more;

	window(n, cur->start, m, &content, &prev, &more);
	if (forward) {
		if (!more) return ST_BAD_ARG;
		cur->start = (u8)(cur->start + content);
	} else {
		if (!prev) return ST_BAD_ARG;
		cur->start = window_prev(cur->start, m);
	}

	window(n, cur->start, m, &content, &prev, &more);
	cur->cursor = (u8)(prev ? 1 : 0);
	if (cursor_end && content != 0) cur->cursor = (u8)(cur->cursor + content - 1);
	g_visit++;
	return ST_OK;
}

/* ------------------------------------------------------------- the view */

void menu_model_reset(u8 max_rows, u8 cfg_rows, u8 caps)
{
	g_game = session_game();
	if (max_rows < 3) max_rows = 3;
	if (max_rows > MENU_VIEW_MAX) max_rows = MENU_VIEW_MAX;
	if (cfg_rows != 0 && cfg_rows < 3) cfg_rows = 3;
	if (cfg_rows > MENU_VIEW_MAX) cfg_rows = MENU_VIEW_MAX;
	g_max_rows = max_rows;
	g_cfg_rows = cfg_rows;
	g_caps = caps;
	nav_reset();
	memset(g_step, 0, sizeof(g_step));
	memset(&g_defer, 0, sizeof(g_defer));
	g_vc_id = 0xFF;
	g_note_id = 0;
	g_view_key = 0;
	g_view_n = 0;
	g_ul.ok = 0;
}

void menu_model_home(void)
{
	nav_reset();
}

u16 menu_model_page(void)
{
	return g_nav[g_depth - 1].page;
}

u8 menu_model_depth(void)
{
	return g_depth;
}

/* Whether the current page wants building again ten times a second (driver). */
static int menu_model_live(void)
{
	return page_live(g_nav[g_depth - 1].page) || g_note_id != 0;
}

static void push_row(struct menu_view *v, const struct menu_row *r)
{
	if (v->nrows >= MENU_VIEW_MAX) return;
	v->rows[v->nrows] = *r;
	v->nrows++;
}

static void nav_row(struct menu_view *v, u8 which, const char *label)
{
	struct menu_row r;

	memset(&r, 0, sizeof(r));
	r.id = MENU_ROW_ID(MENU_ROW_NAV, which);
	row(&r, which == MENU_NAV_MORE ? MROW_MORE : which == MENU_NAV_PREV ? MROW_PREV : MROW_BACK,
	    MROWF_ENABLED, label, "", NULL);
	push_row(v, &r);
}

void menu_model_view(struct menu_view *v)
{
	struct menu_nav *cur;
	struct menu_row r;
	u16 ids[MENU_ITEMS_MAX];
	u8 n, m, content, i, k, total;
	int prev, more;
	char of[12];

	memset(v, 0, sizeof(*v));
	g_game = session_game();
	g_ul.ok = 0;
	cur = &g_nav[g_depth - 1];

	n = page_items(cur->page, ids);
	m = screen_rows(cur->page, g_depth);
	cur->start = valid_start(n, cur->start, m);
	window(n, cur->start, m, &content, &prev, &more);
	window_number(n, cur->start, m, &k, &total);

	v->key = ((u32)cur->page << 16) | ((u32)cur->start << 8) | g_visit;
	v->pflags = g_depth == 1 ? MBOX_PAGE_ROOT : 0;
	v->ptype = page_type(cur->page);
	page_title(cur->page, v->title, sizeof(v->title));
	note_hint(v->hint, sizeof(v->hint));

	if (total > 1) {
		of[0] = ' ';
		qfmt_u32(of + 1, 4, k);
		qstrcat(of, sizeof(of), "/");
		qfmt_u32(of + qstrlen(of), 4, total);
		if (qstrlen(v->title) + qstrlen(of) < sizeof(v->title))
			qstrcat(v->title, sizeof(v->title), of);
	}

	if (prev) nav_row(v, MENU_NAV_PREV, "Previous");
	for (i = 0; i < content; i++) {
		build_row(cur->page, ids[cur->start + i], &r);
		push_row(v, &r);
	}
	if (more) nav_row(v, MENU_NAV_MORE, "More");
	if ((g_caps & MBOX_CAP_BACK) == 0 && g_depth > 1) nav_row(v, MENU_NAV_BACK, "Back");

	/* A page with nothing on it still has a row, which says so. */
	if (v->nrows == 0) {
		build_row(cur->page, MENU_ROW_ID(MENU_ROW_TEXT, 1), &r);
		push_row(v, &r);
	}

	v->cursor = cur->cursor < v->nrows ? cur->cursor : (u8)(v->nrows - 1);

	g_view_key = v->key;
	g_view_n = v->nrows;
	for (i = 0; i < v->nrows; i++) g_view_id[i] = v->rows[i].id;
}

/* ------------------------------------------------------------ the events */

/* Left or Right (dir -1, +1), or the cross on a CONFIG page (0), on a feature row. */
static int step_feature(const struct feature_desc *f, int dir)
{
	const char * const *opts;
	s64 v, lo, hi, step;
	u32 raw;
	u8 count;
	int rc;

	switch (f->kind) {
	case FEATURE_TOGGLE:
		return toggle_set(f, cycle((u8)toggle_now(f), toggle_auto(f) ? 3 : 2, dir));
	case FEATURE_ENUM:
		count = enum_options(f, &opts);
		if (count == 0) return ST_UNSUPPORTED;
		raw = f->min + cycle(enum_index(f, count), count, dir);
		rc = features_set(f->id, raw);
		if (rc == ST_OK) remember_value(f->id, raw);
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

static int defer(u16 id, u8 kind, u8 arg, const char *label)
{
	g_defer.kind = kind;
	g_defer.arg = arg;
	g_defer.waiting = 0;
	set_text(g_defer.label, sizeof(g_defer.label), label);
	note(id, NOTE_DEFERRED, label);
	return ST_OK;
}

/*
 * A feature row: the cross (dir 0) or Left and Right (-1, +1). On a CONFIG
 * page the cross moves a choice on, as the game's Cheats screen does; on a
 * MAIN page it opens the choice's options as a page of their own.
 */
static int act_feature(u8 row, u16 id, const struct feature_desc *f, int dir)
{
	const char * const *opts;
	u8 count, start, pos;

	switch (f->kind) {
	case FEATURE_ACTION:
		if (dir != 0) return ST_OK;
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0) return defer(id, DEFER_FEATURE, f->id, f->label);
		return done(id, features_trigger(f->id), f->label);
	case FEATURE_VALUE:
		if (dir != 0) return answer(id, step_feature(f, dir), f->label);
		if ((g_caps & MBOX_CAP_STEPS) != 0) cycle_step(f);
		return ST_OK;
	case FEATURE_ENUM:
		if (dir == 0 && page_type(g_nav[g_depth - 1].page) == MBOX_PTYPE_MAIN) {
			count = enum_options(f, &opts);
			if (count == 0) return ST_UNSUPPORTED;
			start = window_of(count, enum_index(f, count),
			                  screen_rows(MENU_PAGE_ID(MENU_PAGE_CHOICE, f->id), (u8)(g_depth + 1)), &pos);
			return nav_push(MENU_PAGE_ID(MENU_PAGE_CHOICE, f->id), row, start, pos);
		}
		return answer(id, step_feature(f, dir), f->label);
	case FEATURE_TOGGLE:
		return answer(id, step_feature(f, dir), f->label);
	default:
		return ST_OK;
	}
}

static int act_manip(u16 id, u8 arg, int dir)
{
	const struct game_manips *m = g_game->manips;
	u8 opts = manips_options_now(), i, cur = 0;

	switch (arg) {
	case MENU_MANIP_CATEGORY:
		/* None, then the groups in their order. */
		for (i = 0; i < m->ngroups; i++) {
			if (m->groups[i].id == manips_selected()) cur = (u8)(i + 1);
		}
		cur = cycle(cur, (u8)(m->ngroups + 1), dir);
		return answer(id, manips_select(cur == 0 ? 0 : m->groups[cur - 1].id), "Category");
	case MENU_MANIP_AUTO:
	case MENU_MANIP_REMEMBER:
		opts ^= (u8)(1u << (arg - MENU_MANIP_AUTO));
		return answer(id, manips_options(opts & 1u, (opts >> 1) & 1u), NULL);
	default:
		if (dir != 0) return ST_OK;
		return done(id, manips_apply(), "Apply now");
	}
}

static int act_unlock(u16 id, u8 arg, int dir)
{
	const struct game_menu *gm = gmenu();
	const struct game_unlock *u = unlock_row(arg);
	char text[MROW_VALUE_LEN];
	u8 next;
	int rc;

	if (u == NULL) return ST_NOT_FOUND;
	next = cycle(unlock_now(u, text, sizeof(text)), unlock_count(u), dir);
	g_ul.ok = 0;
	rc = g_game->unlock_set(u->id, 0, next != 0);
	if (rc == ST_OK && next != 0 && unlock_count(u) > 2)
		rc = g_game->unlock_set(u->id, gm->unlock_field, unlock_value(gm, u, next));
	return answer(id, rc, u->name);
}

static int act(u8 row, u16 id, int dir)
{
	struct menu_nav *cur = &g_nav[g_depth - 1];
	const struct feature_desc *f;
	const char * const *names;
	u8 arg = (u8)id, count;
	int rc;

	switch ((u8)(id >> 8)) {
	case MENU_ROW_GROUP:   return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_GROUP, arg), row, 0, 0);
	case MENU_ROW_SECTION: return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_SECTION, arg), row, 0, 0);
	case MENU_ROW_PAGE:    return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(arg, 0), row, 0, 0);
	case MENU_ROW_UCAT:    return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_UNLOCK_CAT, arg), row, 0, 0);
	case MENU_ROW_SCAT:    return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_SAVE_CAT, arg), row, 0, 0);

	case MENU_ROW_FEATURE:
		f = feature(arg);
		if (f == NULL) return ST_NOT_FOUND;
		return act_feature(row, id, f, dir);

	case MENU_ROW_OPTION:
		if (dir != 0) return ST_OK;
		if (page_kind(cur->page) != MENU_PAGE_CHOICE) return ST_BAD_ARG;
		f = feature(page_arg(cur->page));
		if (f == NULL) return ST_NOT_FOUND;
		rc = features_set(f->id, f->min + arg);
		if (rc != ST_OK) return answer(id, rc, f->label);
		remember_value(f->id, f->min + arg);
		return nav_pop();

	case MENU_ROW_POS:
		if (arg == MENU_ITEM_SLOT) {
			config_set_selected_slot(cycle(config_selected_slot(), QWARK_POS_SLOTS, dir));
			return ST_OK;
		}
		if (dir != 0) return ST_OK;
		if (arg == MENU_ITEM_POS_SAVE) return done(id, session_position_save(0xFF), "Save position");
		return done(id, session_position_load(0xFF), "Load position");

	case MENU_ROW_PLANET:
		if (dir != 0) return ST_OK;
		names = g_game->planet_names(&count);
		return defer(id, DEFER_PLANET, arg, arg < count ? names[arg] : NULL);

	case MENU_ROW_MANIP:
		return act_manip(id, arg, dir);

	case MENU_ROW_UNLOCK:
		return act_unlock(id, arg, dir);

	case MENU_ROW_SAVE:
		if (dir != 0) return ST_OK;
		if (!save_category(page_arg(cur->page), g_defer.cat) ||
		    arg >= savefile_menu_entry(g_defer.cat, arg, g_defer.name, sizeof(g_defer.name)))
			return answer(id, ST_NOT_FOUND, NULL);
		return defer(id, DEFER_RESTORE, arg, g_defer.name);

	case MENU_ROW_NAV:
		if (dir != 0) return ST_OK;
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
	case MBOX_EV_ACTIVATE: return act(row, id, 0);
	case MBOX_EV_LEFT:     return act(row, id, -1);
	case MBOX_EV_RIGHT:    return act(row, id, 1);
	default:               return ST_BAD_ARG;
	}
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
/* An install that was refused or failed is tried again ten seconds on. */
#define RETRY_US         10000000ull
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
/* When the session's install is due: a few seconds in, and again after a refusal. */
static u64  g_install_delay = 3000000ull;
static u64  g_install_at;

static u32  g_page_seq;
static u32  g_posted_crc;
static int  g_posted;
static u32  g_last_ack;
static u8   g_hstate;
/* The renderer's frame count when last read: it starts again from 0 at each opening. */
static u32  g_frames;
static u32  g_last_poll;
static u32  g_last_build;
static u8   g_req;
static u64  g_req_until;
static u8   g_why;

static int  g_enabled = 1;
static u32  g_enabled_version = 0xFFFFFFFFu;

int menu_state(void) { return g_state; }
u64 menu_install_delay_us(void) { return g_install_delay; }
void menu_set_install_delay_us(u64 us) { g_install_delay = us; }

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
	memset(&g_defer, 0, sizeof(g_defer));
}

void menu_enter(const struct game_api *game)
{
	menu_forget();
	g_why = 0;
	g_logged_refusal = 0;
	menu_model_reset(8, 0, 0);

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
	 * Round 2: the renderer goes in a few seconds into the session, once the
	 * game is in ordinary play, because the pause menu's Help row is the
	 * menu's way in now and has to be there before the player opens the
	 * pause. The combo can still put it in sooner (menu_combo).
	 */
	g_state = MENU_IDLE;
	g_install_at = plat_time_us() + g_install_delay;
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
	img[MBOX_OFF_PTYPE - MBOX_OFF_PAGE_KEY] = v->ptype;
	memcpy(img + MBOX_OFF_TITLE - MBOX_OFF_PAGE_KEY, v->title, MBOX_TITLE_LEN);
	memcpy(img + MBOX_OFF_HINT - MBOX_OFF_PAGE_KEY, v->hint, MBOX_HINT_LEN);
	for (i = 0; i < v->nrows; i++) {
		u8 *r = img + MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + i * g_row_size;
		r[MROW_OFF_KIND] = v->rows[i].kind;
		r[MROW_OFF_FLAGS] = v->rows[i].flags;
		be16_put(r + MROW_OFF_ID, v->rows[i].id);
		memcpy(r + MROW_OFF_LABEL, v->rows[i].label, MROW_LABEL_LEN);
		if ((v->rows[i].flags & MROWF_RANGE) != 0) {
			u8 k;
			for (k = 0; k < 4; k++) be32_put(r + MROW_OFF_RANGE + 4u * k, v->rows[i].range[k]);
		}
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

/* A cave's zeros, out of the tick's scratch: one write per 8 KB rather than per 256 bytes. */
static int write_zeros(u32 addr, u32 len)
{
	u8 *buf = qscratch();
	u32 n;
	int rc;

	memset(buf, 0, len < QSCRATCH_BYTES ? len : QSCRATCH_BYTES);
	while (len != 0) {
		n = len < QSCRATCH_BYTES ? len : QSCRATCH_BYTES;
		rc = mem_write(addr, buf, n);
		if (rc != ST_OK) return rc;
		addr += n;
		len -= n;
	}
	return ST_OK;
}

/*
 * Puts the renderer into the game: every hook site must hold the game's own
 * word or the renderer's. All the game's: the data images (the mailbox, the
 * state, the read-only data), the first page, the code, then the hook words in
 * order. Some already the renderer's - this module was loaded again into a
 * running game, or an install was cut short - and the code must be this
 * build's to the byte; the rest goes in around it and the mailbox is taken over
 * as it stands. Anything else at a site is another mod's, and nothing is
 * written.
 *
 * It runs a few seconds into the session with the game in ordinary play, or in
 * the tick that saw an earlier press of the menu combo: some twenty reads and
 * sixteen writes, 17 KB in all for Deadlocked (the zeros of the mailbox and the
 * state written out of the tick's scratch). Nothing can branch into a cave
 * before its hook word, every hook word goes in after the caves, and a combo's
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
			if (rc == ST_OK && d->caves[i].zeros != 0)
				rc = write_zeros(d->caves[i].addr + d->caves[i].len, d->caves[i].zeros);
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
	    head[MBOX_OFF_ROW_CAP] < 3 || be16_get(head + MBOX_OFF_VERSION) != MBOX_VERSION ||
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
	g_frames = be32_get(head + MBOX_OFF_FRAMES);

	{
		u8 rows = head[MBOX_OFF_MAX_ROWS], cfg = head[MBOX_OFF_CFG_ROWS];

		g_row_cap = head[MBOX_OFF_ROW_CAP];
		g_row_size = head[MBOX_OFF_ROW_SIZE];
		if (g_row_cap > MENU_VIEW_MAX) g_row_cap = MENU_VIEW_MAX;
		if (rows > g_row_cap) rows = g_row_cap;
		if (cfg > g_row_cap) cfg = g_row_cap;
		menu_model_reset(rows, cfg, head[MBOX_OFF_CAPS]);
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
		plat_log("menu: install failed (%d); trying again in ten seconds", rc);
		g_state = MENU_IDLE;
	}
	g_install_at = plat_time_us() + RETRY_US;
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

/* A CLOSES action, now. */
static int run_deferred(void)
{
	switch (g_defer.kind) {
	case DEFER_FEATURE: return features_trigger(g_defer.arg);
	case DEFER_PLANET:  return session_planet_load(g_defer.arg, 0xFF);
	case DEFER_RESTORE: return savefile_restore(g_defer.cat, g_defer.name);
	default:            return ST_OK;
	}
}

static void defer_notify(const char *what, const char *why)
{
	char msg[96];

	qstrcpy(msg, sizeof(msg), "Trainer: ");
	qstrcat(msg, sizeof(msg), g_defer.label);
	qstrcat(msg, sizeof(msg), what);
	qstrcat(msg, sizeof(msg), why);
	plat_notify(msg);
}

/*
 * A CLOSES action (a load: the set-aside file, a planet, a save of the
 * library): asked for, the menu asked to close; run once it has and the game
 * plays again, or dropped five seconds on.
 */
static void deferred_tick(void)
{
	int rc;

	if (g_defer.kind == DEFER_NONE) return;

	if (!g_defer.waiting) {
		if ((g_caps & MBOX_CAP_REQUEST) == 0) {
			run_deferred();
			g_defer.kind = DEFER_NONE;
			return;
		}
		g_defer.waiting = 1;
		g_defer.until = plat_time_us() + DEFER_US;
		if (g_hstate == MBOX_HSTATE_OPEN) request(MBOX_REQ_CLOSE);
		return;
	}

	if (g_hstate == MBOX_HSTATE_CLOSED && game_playing()) {
		rc = run_deferred();
		if (rc != ST_OK) defer_notify(": ", status_text((u8)rc));
		g_defer.kind = DEFER_NONE;
		return;
	}

	if (plat_time_us() >= g_defer.until) {
		defer_notify(" was dropped, the game did not go back to play", "");
		g_defer.kind = DEFER_NONE;
	}
}

/* The mailbox: requests, events, live values. Every POLL_* ticks. */
static void serve(void)
{
	u8 head[MBOX_READ_HEAD];
	u32 tick = now_tick();
	u32 ev, frames;
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

	/*
	 * Round 3: whenever the menu closes, the next open starts at the root, the
	 * cursor on its first row, from the game's row and the combo alike: the
	 * stack goes back to the root as qwark sees it close, and the root is in
	 * the mailbox before it can open again. An opening qwark did not see the
	 * close before (the renderer's frame count back near 0 with the menu up
	 * all along) goes back to the root too, a moment late.
	 */
	hstate = head[MBOX_OFF_HSTATE];
	frames = be32_get(head + MBOX_OFF_FRAMES);
	if (hstate != g_hstate) {
		g_hstate = hstate;
		if (hstate == MBOX_HSTATE_CLOSED) menu_model_home();
		dirty = 1;
	} else if (hstate == MBOX_HSTATE_OPEN && frames < g_frames) {
		menu_model_home();
		dirty = 1;
	}
	g_frames = frames;

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
		menu_model_event(head[MBOX_OFF_EV_KIND], head[MBOX_OFF_EV_ROW],
		                 be16_get(head + MBOX_OFF_EV_ID), be32_get(head + MBOX_OFF_EV_KEY));
		g_last_ack = ev;
		mem_write_u32(box(MBOX_OFF_EV_ACK), ev);
		dirty = 1;
	}

	/* A page whose rows do not move is built again only when something happened. */
	if (dirty || (menu_model_live() &&
	              tick - g_last_build >= (g_hstate == MBOX_HSTATE_OPEN ? BUILD_OPEN : BUILD_CLOSED)))
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
			g_defer.kind = DEFER_NONE;
		}
		return;

	case MENU_IDLE:
	case MENU_REFUSED:
		/*
		 * Nothing of the menu is in the game yet. It goes in once it is due
		 * and the game is in ordinary play, so nothing is written while a
		 * menu of the game's is up; a refusal waits ten seconds for the other
		 * mod to go, and says so once in the log.
		 */
		if (!enabled() || plat_time_us() < g_install_at) return;
		if (!game_playing()) {
			/* Looked at four times a second, not every tick, until the game plays. */
			g_install_at = plat_time_us() + 250000ull;
			return;
		}
		try_install();
		return;

	default:
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
		            ? "Trainer menu: not available in this game yet"
		            : "Trainer menu: not available under RPCS3");
		return;
	}
	if (!enabled()) {
		unavailable(WHY_OFF, "Trainer menu: switched off (menu.enabled = 0 in config.txt)");
		return;
	}
	if (g_state == MENU_REMOVING) g_state = MENU_READY;

	/*
	 * A press before the session's install, after a refusal, or after
	 * menu.enabled went to 0 and back puts the renderer in now, here in the
	 * tick that saw it, and asks it to open straight after (install() has the
	 * cost).
	 */
	if (g_state != MENU_READY && !try_install()) {
		if (g_state == MENU_REFUSED)
			unavailable(WHY_REFUSED, "Trainer menu: another mod holds the menu's hooks");
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
