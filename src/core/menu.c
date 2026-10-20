#include "menu.h"
#include "features.h"
#include "session.h"
#include "config.h"
#include "mem.h"
#include "util.h"
#include "manips.h"
#include "savefile.h"
#include "uipak.h"
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
 * A row's answer, shown in place of its value for two seconds: the reason for
 * a refusal. Round 5 (the user's call): nothing for an answer that went
 * through, the game's own sound being the cue; round 4 showed "done", and
 * "after closing" for a load. 1.4.1: it goes when its two seconds are up,
 * whatever is built, and with its page (note_up, nav_*); it used to wait for
 * its own row to be built again, and kept every page live meanwhile.
 */
#define NOTE_TICKS 240
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

/*
 * 1.4.1 (finding 8): the save library as one page build reads it. Save files
 * walks the library's folder once a build, and a category's page walks it
 * once for the category's name and the category's folder once for its saves,
 * where every row used to walk both again. The names of the rows the window
 * shows go in the tick's scratch, past what post() puts there (LIB_AT, the
 * category's name first); they are good for that build only, so an event
 * reads the folders afresh. `sig` is the listing the last build showed
 * (savefile_menu_list), which a save row's press checks (finding 10).
 */
#define LIB_NAME (SAVEFILE_NAME_LEN + 1)
#define LIB_AT   (QSCRATCH_BYTES - (MENU_VIEW_MAX + 1) * LIB_NAME)
static struct {
	u8  on;                 /* a build is reading from it */
	u8  n;                  /* the page's rows */
	u8  first;              /* the row the first name kept is */
	u32 sig;
} g_lib;

static u32 now_tick(void)
{
	return session_tick_count();
}

#ifdef QWARK_TEST
/*
 * The tests' own menu table in place of the game's, NULL for the game's again:
 * since round 5 no game lists a VALUE as a stepper, and the steppers and
 * sliders are still a page's controls (round 1's DESCRIBE groups).
 */
static const struct game_menu *g_test_menu;
void menu_test_menu(const struct game_menu *gm) { g_test_menu = gm; }
#endif

static const struct game_menu *gmenu(void)
{
#ifdef QWARK_TEST
	if (g_test_menu != NULL) return g_test_menu;
#endif
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
	case ST_NOT_INGAME:  return "not in game";
	case ST_UNSUPPORTED: return "not available";
	case ST_BAD_ARG:     return "refused";
	case ST_IO_ERROR:    return "failed";
	case ST_FULL:        return "full";
	case ST_NOT_FOUND:   return "not found";
	case ST_BUSY:        return "busy";
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

/* Whether an answer is showing; one whose two seconds are up is forgotten here. */
static int note_up(void)
{
	if (g_note_id != 0 && (s32)(now_tick() - g_note_until) >= 0) g_note_id = 0;
	return g_note_id != 0;
}

/* The page's hint: the last answer as "<row>: <answer>", while it lasts. Every build asks. */
static void note_hint(char *out, u32 cap)
{
	memset(out, 0, cap);
	if (!note_up()) return;
	qstrcpy(out, cap, g_note_label);
	qstrcat(out, cap, ": ");
	qstrcat(out, cap, status_text(g_note_rc));
}

/* The row's note, if it has one now. */
static const char *note_for(u16 page, u16 id)
{
	if (!note_up() || id != g_note_id || page != g_note_page) return NULL;
	return status_text(g_note_rc);
}

/*
 * Notes a failure, and answers what it was given. Round 5: an action's success
 * too is no note (round 4's done() noted "done"); 1.4.1: and it takes the
 * row's own refusal off, which went on showing for its two seconds.
 */
static int answer(u16 id, int rc, const char *label)
{
	if (rc != ST_OK) note(id, (u8)rc, label);
	else if (id == g_note_id) g_note_id = 0;
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

/*
 * Whether a VALUE suits a slider: a range of MENU_SLIDER_SPAN steps of 1 or
 * fewer (Challenge mode's 0..255, not Bolts'), on a renderer that steps it.
 */
static int value_slider(const struct feature_desc *f)
{
	s64 lo, hi;

	value_range(f, &lo, &hi);
	return hi - lo <= MENU_SLIDER_SPAN && (g_caps & MBOX_CAP_STEPS) != 0;
}

/*
 * What Left and Right move a VALUE by. A stepper: 1, 10, 100 ..., which the
 * cross cycles through. Round 4, a slider: a tenth of its range, rounded up,
 * so ten presses cross it, or 1, the fine step the cross switches to and back.
 */
static u32 value_step(const struct feature_desc *f)
{
	s64 lo, hi;
	u8 k = f->id < QWARK_MAX_FEATURES ? g_step[f->id] : 0;

	if (!value_slider(f)) return g_pow10[k];
	if (k != 0) return 1;
	value_range(f, &lo, &hi);
	if (hi - lo < MENU_SLIDER_STEPS) return 1;
	return (u32)((hi - lo + MENU_SLIDER_STEPS - 1) / MENU_SLIDER_STEPS);
}

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
	/* A step other than the first shows: "[10]" on a stepper, "[1]" on a slider's fine step. */
	if (f->id < QWARK_MAX_FEATURES && g_step[f->id] != 0 && (g_caps & MBOX_CAP_STEPS) != 0) {
		step[0] = ' ';
		step[1] = '[';
		qfmt_u32(step + 2, sizeof(step) - 3, value_step(f));
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

/*
 * The widget a page is drawn with: a list of features is a CONFIG page where
 * the renderer has one. Round 4: so is any list that can be longer than a
 * MAIN look holds, since no game's MAIN look scrolls (UYA's main pause holds
 * nine, RaC1's panes and RaC2's bars seven) and every CONFIG look does: the
 * planets (until the native picker) and the save library's categories and
 * saves, as lists of buttons. Only the root and Unlocks are MAIN, and each
 * fits its game's MAIN look.
 */
static u8 page_type(u16 page)
{
	u8 k = page_kind(page);

	if (g_cfg_rows == 0) return MBOX_PTYPE_MAIN;
	return (k == MENU_PAGE_GROUP || k == MENU_PAGE_SECTION || k == MENU_PAGE_MANIPS ||
	        k == MENU_PAGE_UNLOCK_CAT || k == MENU_PAGE_UNLOCK_ACTIONS || k == MENU_PAGE_PLANETS ||
	        k == MENU_PAGE_SAVES || k == MENU_PAGE_SAVE_CAT) ? MBOX_PTYPE_CONFIG : MBOX_PTYPE_MAIN;
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

/* The save library's category with this place, into `out` (LIB_NAME bytes); 0 when there is none. */
static int save_category(u8 index, char *out)
{
	out[0] = 0;
	return index < savefile_menu_list(NULL, index, 1, out, NULL) && out[0] != 0;
}

static char *lib_slot(u8 k)
{
	return (char *)qscratch() + LIB_AT + (u32)k * LIB_NAME;
}

/* The library for a build of `page`, its window from row `first`, `m` rows: one walk of each folder. */
static void lib_read(u16 page, u8 first, u8 m)
{
	u8 k = page_kind(page);

	g_lib.on = 0;
	g_lib.n = 0;
	g_lib.sig = 0;
	if (k != MENU_PAGE_SAVES && k != MENU_PAGE_SAVE_CAT) return;
	if (m > MENU_VIEW_MAX) m = MENU_VIEW_MAX;
	memset(lib_slot(0), 0, (MENU_VIEW_MAX + 1) * LIB_NAME);
	g_lib.on = 1;
	g_lib.first = first;
	if (k == MENU_PAGE_SAVES)
		g_lib.n = savefile_menu_list(NULL, first, m, lib_slot(1), NULL);
	else if (save_category(page_arg(page), lib_slot(0)))
		g_lib.n = savefile_menu_list(lib_slot(0), first, m, lib_slot(1), &g_lib.sig);
}

/* The name of the page's row `i` as the build read it, "" outside its window. */
static const char *lib_name(u8 i)
{
	if (!g_lib.on || i < g_lib.first || i - g_lib.first >= MENU_VIEW_MAX) return "";
	return lib_slot((u8)(1 + i - g_lib.first));
}

static u8 push_id(u16 *ids, u8 n, u16 id)
{
	if (n < MENU_ITEMS_MAX) ids[n++] = id;
	return n;
}

#define SPACER_ID MENU_ROW_ID(MENU_ROW_TEXT, MENU_TEXT_SPACER)

/*
 * Round 5: a blank row between a page's parts, never opening the page or
 * following another (Game without its position rows, where the game has no
 * positions, starts with its save-file part).
 */
static u8 push_spacer(u16 *ids, u8 n)
{
	if (n == 0 || ids[n - 1] == SPACER_ID) return n;
	return push_id(ids, n, SPACER_ID);
}

/* Nor ending it. */
static u8 drop_spacer(const u16 *ids, u8 n)
{
	return n != 0 && ids[n - 1] == SPACER_ID ? (u8)(n - 1) : n;
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
		return n;

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
			if (it == MENU_ITEM_SPACER) {
				n = push_spacer(ids, n);
			} else if (it >= MENU_ITEM_SLOT) {
				if (g_game->save_blob != NULL) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_POS, it));
			} else if (it >= MENU_ITEM_VALUE(0)) {
				/* Round 5: a VALUE as a button or a switch (struct menu_value). */
				j = (u8)(it - MENU_ITEM_VALUE(0));
				if (j < gm->nvalues && feature(gm->values[j].id) != NULL)
					n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_VALUE, j));
			} else if (feature(it) != NULL) {
				n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_FEATURE, it));
			}
		}
		return drop_spacer(ids, n);

	case MENU_PAGE_CHOICE:
		f = feature(arg);
		count = f != NULL ? enum_options(f, &opts) : 0;
		for (i = 0; i < count; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_OPTION, i));
		return n;

	case MENU_PAGE_PLANETS: {
		const char * const *names;
		/* The pickers' round 2: the planets the game's picker cannot show, where it names them. */
		const struct menu_section *o = arg == MENU_PLANETS_OTHERS && gm != NULL ? gm->picker_others : NULL;
		if (g_game == NULL || g_game->planet_names == NULL) return 0;
		names = g_game->planet_names(&count);
		for (i = 0; i < (o != NULL ? o->nitems : count); i++) {
			u8 p = o != NULL ? o->items[i] : g_game->planet_order != NULL ? g_game->planet_order[i] : i;
			if (p >= count || names[p] == NULL || names[p][0] == '(' || names[p][0] == 0) continue;
			if (gm != NULL && gm->planet_ok != NULL && !gm->planet_ok(p)) continue;
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PLANET, p));
		}
		return n;
	}

	case MENU_PAGE_MANIPS:
		if (g_game == NULL || g_game->manips == NULL) return 0;
		/* Round 5: one Auto-reset row, Off, On or On boot, where Remember had its own. */
		n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_MANIP, MENU_MANIP_CATEGORY));
		n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_MANIP, MENU_MANIP_AUTO));
		n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_MANIP, MENU_MANIP_APPLY));
		count = n;
		/*
		 * MANIP_LIST's headers' features, those DESCRIBE has (UYA's trophy
		 * rows are not where the reset cannot work), each ACTION that names a
		 * status readout followed by a row of that status (UYA's Reset
		 * trophies, PROTOCOL.md 5.14). Round 5: a spacer between the category's
		 * part and theirs (Deadlocked's Fixes, UYA's Trophies).
		 */
		for (i = 0; i < g_game->manips->nheaders; i++) {
			for (j = 0; j < g_game->manips->headers[i].nfeatures; j++) {
				u8 id = g_game->manips->headers[i].features[j];
				f = feature(id);
				if (f == NULL) continue;
				if (n == count) n = push_spacer(ids, n);
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

	/* Within a build, the count lib_read found; outside one (a slide), a walk of its own. */
	case MENU_PAGE_SAVES:
		count = g_lib.on ? g_lib.n : savefile_menu_list(NULL, 0, 0, NULL, NULL);
		for (i = 0; i < count; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_SCAT, i));
		return n;

	case MENU_PAGE_SAVE_CAT:
		if (g_lib.on) count = g_lib.n;
		else count = save_category(arg, cat) ? savefile_menu_list(cat, 0, 0, NULL, NULL) : 0;
		for (i = 0; i < count; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_SAVE, i));
		return n;

	default:
		return 0;
	}
}

static void page_title(u16 page, char *out, u32 cap)
{
	const struct game_describe *d = desc();
	const struct game_menu *gm = gmenu();
	const struct feature_desc *f;
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
		/* The category's name as the build read it (page_title runs only in one). */
		if (g_lib.on) qstrcpy(out, cap, lib_slot(0));
		return;
	case MENU_PAGE_PLANETS:
		if (arg == MENU_PLANETS_OTHERS && gm != NULL && gm->picker_others != NULL) {
			qstrcpy(out, cap, gm->picker_others->title);
			return;
		}
		/* fall through */
	default:
		qstrcpy(out, cap, page_name(page_kind(page)));
		return;
	}
}

/* ---------------------------------------------------------------- rows */

static void row(struct menu_row *r, u8 kind, u8 flags, const char *label, const char *value)
{
	r->kind = kind;
	r->flags = flags;
	set_text(r->label, sizeof(r->label), label);
	if (value != NULL) set_text(r->value, sizeof(r->value), value);
}

static u8 steps(void)
{
	return (g_caps & MBOX_CAP_STEPS) != 0 ? MROWF_STEPS : 0;
}

static const char *const g_toggle_text[3] = { "Off", "On", "Auto" };

/*
 * Round 5: the Manips page's Auto-reset, MANIP_OPTIONS' two flags as one row
 * (manips_options_now: bit0 auto-reset, bit1 remember). Off is no auto-reset
 * and no remembering, On is auto-reset alone, On boot both: the category
 * comes back at the next boot and is put in on every file load. Remember
 * without auto-reset, which only the client can set, reads Off, since nothing
 * is put in on a file load then; a step from it writes one of the three.
 */
static const char *const g_reset_text[3] = { "Off", "On", "On boot" };

static u8 manip_reset(u8 opts)
{
	if ((opts & 1u) == 0) return 0;
	return (opts & 2u) != 0 ? 2 : 1;
}

/* Round 5: a VALUE as a switch (struct menu_value) is on while the field reads above 0. */
static u8 value_on(const struct feature_desc *f)
{
	return value_number(f, feature_value(f)) > 0;
}

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
	r->range[3] = value_step(f);
	memset(r->label + MROW_RANGE_LABEL - 1, 0, sizeof(r->label) - (MROW_RANGE_LABEL - 1));
}

static void feature_row(const struct feature_desc *f, struct menu_row *r)
{
	const char * const *opts;
	u8 count;

	switch (f->kind) {
	case FEATURE_TOGGLE: {
		int st = toggle_now(f);
		row(r, MROW_TOGGLE, MROWF_ENABLED | steps() | (st != 0 ? MROWF_ON : 0), f->label, g_toggle_text[st]);
		break;
	}
	case FEATURE_ACTION:
		/*
		 * Loading the set-aside file runs the game's own load, which
		 * Deadlocked's helper does from a pause of its own, so it waits for
		 * the menu to close and the game to be back in play.
		 */
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0)
			row(r, MROW_ACTION, MROWF_ENABLED | MROWF_CLOSES, f->label, "");
		else
			row(r, MROW_ACTION, MROWF_ENABLED, f->label, "");
		break;
	case FEATURE_VALUE:
		/*
		 * Left and Right step the number, the cross changes the step: a
		 * stepper wants both, so a renderer that cannot send Left and Right
		 * shows the number and nothing more.
		 */
		if (steps() != 0)
			row(r, MROW_VALUE, MROWF_ENABLED | MROWF_STEPS, f->label, NULL);
		else
			row(r, MROW_INFO, 0, f->label, NULL);
		value_text(f, r->value, sizeof(r->value));
		if (r->kind == MROW_VALUE) value_range_row(f, r);
		break;
	case FEATURE_ENUM:
		count = enum_options(f, &opts);
		row(r, MROW_CHOICE, MROWF_ENABLED | steps(), f->label,
		    count != 0 && opts[enum_index(f, count)] != NULL ? opts[enum_index(f, count)] : "");
		break;
	case FEATURE_COLOR:
		row(r, MROW_INFO, 0, f->label, "#");
		qfmt_hex(r->value + 1, sizeof(r->value) - 1, feature_value(f) & 0x00FFFFFFu, 6);
		break;
	default:
		row(r, MROW_INFO, 0, f->label, NULL);
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
			below = i;
			break;
		}
		if (v[gm->unlock_field] > unlock_value(gm, u, i)) below = i;
	}
	/*
	 * Round 5 (the user's call): where the game has a prefix, the version as
	 * the game numbers it, on the list or off it: V1, a weapon's own last
	 * version ("V4", "V8", the R3YNO's "V5") and any between ("V3"), where
	 * RaC2 and UYA said Unlocked and Max. Without one, the name of the value,
	 * or of the value under it off the list.
	 */
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
	char text[LIB_NAME];
	u8 arg = (u8)id, count;
	const char *n;

	memset(r, 0, sizeof(*r));
	r->id = id;

	switch ((u8)(id >> 8)) {
	case MENU_ROW_GROUP:
		row(r, MROW_LINK, MROWF_ENABLED, d != NULL && arg < d->ngroups ? d->groups[arg] : "", "");
		break;
	case MENU_ROW_SECTION:
		row(r, MROW_LINK, MROWF_ENABLED, gm->sections[arg].title, "");
		break;
	case MENU_ROW_PAGE:
		row(r, MROW_LINK, MROWF_ENABLED, page_name(arg), "");
		/* Mailbox version 3: a renderer that opens the game's own planet picker opens it here. */
		if (arg == MENU_PAGE_PLANETS && (g_caps & MBOX_CAP_PICKER) != 0) r->flags |= MROWF_PICKER;
		break;
	case MENU_ROW_UCAT:
		row(r, MROW_LINK, MROWF_ENABLED, unlocks() && arg < g_ul.ncats ? g_ul.cats[arg] : "", "");
		break;
	case MENU_ROW_SCAT:
		row(r, MROW_LINK, MROWF_ENABLED, lib_name(arg), "");
		break;
	case MENU_ROW_TEXT:
		/* Grey: no row of text is ENABLED, so every renderer's cursor passes over it. */
		row(r, MROW_INFO, 0, arg == MENU_TEXT_EMPTY ? "Nothing here" : "", "");
		break;

	case MENU_ROW_VALUE: {
		/* Round 5: a VALUE as a button ("Max bolts") or a switch (Challenge mode). */
		const struct menu_value *mv = gm != NULL && arg < gm->nvalues ? &gm->values[arg] : NULL;
		f = mv != NULL ? feature(mv->id) : NULL;
		if (f == NULL) break;
		n = mv->label != NULL ? mv->label : f->label;
		if (mv->kind == MENU_VALUE_SWITCH) {
			count = value_on(f);
			row(r, MROW_TOGGLE, MROWF_ENABLED | steps() | (count ? MROWF_ON : 0), n, g_toggle_text[count]);
		} else {
			row(r, MROW_ACTION, MROWF_ENABLED, n, "");
		}
		break;
	}

	case MENU_ROW_FEATURE:
		f = feature(arg);
		if (f != NULL) feature_row(f, r);
		break;

	case MENU_ROW_OPTION:
		f = feature(page_arg(page));
		if (f == NULL) break;
		count = enum_options(f, &opts);
		row(r, MROW_OPTION, MROWF_ENABLED | (enum_index(f, count) == arg ? MROWF_ON : 0),
		    arg < count && opts[arg] != NULL ? opts[arg] : "", "");
		break;

	case MENU_ROW_POS:
		if (arg == MENU_ITEM_SLOT) {
			row(r, MROW_CHOICE, MROWF_ENABLED | steps(), "Position slot", "");
			qfmt_u32(r->value, sizeof(r->value), (u32)config_selected_slot() + 1u);
		} else {
			row(r, MROW_ACTION, MROWF_ENABLED, arg == MENU_ITEM_POS_SAVE ? "Save position" : "Load position", "");
		}
		break;

	case MENU_ROW_PLANET: {
		const char * const *names = g_game->planet_names(&count);
		row(r, MROW_ACTION, MROWF_ENABLED | MROWF_CLOSES, arg < count ? names[arg] : "", "");
		break;
	}

	case MENU_ROW_MANIP: {
		const struct game_manips *m = g_game->manips;
		u8 sel = manips_selected(), opts_now = manips_options_now(), on;
		switch (arg) {
		case MENU_MANIP_CATEGORY:
			row(r, MROW_CHOICE, MROWF_ENABLED | steps(), "Category", "None");
			for (count = 0; count < m->ngroups; count++) {
				if (m->groups[count].id == sel) set_text(r->value, sizeof(r->value), m->groups[count].name);
			}
			break;
		case MENU_MANIP_AUTO:
			on = manip_reset(opts_now);
			row(r, MROW_TOGGLE, MROWF_ENABLED | steps() | (on ? MROWF_ON : 0), "Auto-reset", g_reset_text[on]);
			break;
		default:
			row(r, MROW_ACTION, MROWF_ENABLED, "Apply now", "");
			break;
		}
		break;
	}

	case MENU_ROW_STATUS:
		/* An ACTION's status readout, as the game words it (UYA's trophy reset). */
		f = feature(arg);
		if (f == NULL || gm == NULL || gm->action_status == NULL || f->readout >= QWARK_MAX_READOUTS) break;
		row(r, MROW_INFO, 0, f->readout < d->nreadouts ? d->readouts[f->readout] : f->label, "");
		gm->action_status(arg, session_readout(f->readout), r->value, sizeof(r->value));
		r->value[sizeof(r->value) - 1] = 0;
		break;

	case MENU_ROW_UNLOCK:
		u = unlock_row(arg);
		if (u == NULL) break;
		row(r, MROW_CHOICE, MROWF_ENABLED | steps(), u->name, "");
		if (unlock_now(u, r->value, sizeof(r->value)) != 0) r->flags |= MROWF_ON;
		break;

	case MENU_ROW_SAVE:
		qstrcpy(text, sizeof(text), lib_name(arg));
		/* The name without its ".sav". */
		count = (u8)qstrlen(text);
		if (count > 4) text[count - 4] = 0;
		row(r, MROW_ACTION, MROWF_ENABLED | MROWF_CLOSES, text, "");
		break;

	default:
		break;
	}

	n = note_for(page, id);
	if (n != NULL) set_text(r->value, sizeof(r->value), n);
}

/* ----------------------------------------------------------- the windows */

/*
 * Round 4: a page longer than the renderer takes at once is posted a window
 * of `m` of its rows, with no More and no Previous row: the page's flags say
 * it goes on above or below (MBOX_PAGE_MORE_*), and when the cursor comes to
 * the window's edge the renderer asks for the window that continues it
 * (MBOX_EV_SCROLL_*), with the cursor on the same row. Every game's CONFIG
 * look scrolls its rows natively, so a window is as many rows as the
 * renderer's mailbox holds, and it only slides on a page longer than that (a
 * save category of the console's library, RaC1's and UYA's planets). With a
 * renderer that has no back control every page but the root ends in a BACK
 * row as well, and the window is one row shorter for the page's own.
 */
static u8 screen_rows(u16 page, u8 depth)
{
	u8 m = page_type(page) == MBOX_PTYPE_CONFIG ? g_cfg_rows : g_max_rows;

	if ((g_caps & MBOX_CAP_BACK) == 0 && depth > 1) m--;
	return m;
}

/* The last window's start: 0 for a page that fits. */
static u8 last_start(u8 n, u8 m)
{
	return n > m ? (u8)(n - m) : 0;
}

/*
 * The window that shows the page's row `a`, never with the cursor on an edge
 * the page goes on past: going down, `a` second from the window's top; going
 * up, second from its bottom (as far as the page's own ends allow).
 */
static u8 window_at(u8 n, u8 m, u8 a, int down)
{
	int s;

	if (n <= m) return 0;
	s = down ? (int)a - 1 : (int)a - (int)m + 2;
	if (s < 0) s = 0;
	if (s > (int)last_start(n, m)) s = last_start(n, m);
	return (u8)s;
}

/* ------------------------------------------------------------ navigation */

/* 1.4.1: a row's answer goes with its page, so each move to another page drops it. */
static void nav_reset(void)
{
	memset(g_nav, 0, sizeof(g_nav));
	g_nav[0].page = MENU_PAGE_ID(MENU_PAGE_ROOT, 0);
	g_depth = 1;
	g_visit++;
	g_note_id = 0;
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
	g_note_id = 0;
	return ST_OK;
}

/* Back a page, the cursor on the row the page was opened from. */
static int nav_pop(void)
{
	if (g_depth <= 1) return ST_BAD_ARG;
	g_depth--;
	g_visit++;
	g_note_id = 0;
	return ST_OK;
}

/*
 * The window moved on the current page from the cursor's `row` of it: a slide
 * (the cursor at the window's edge, SCROLL_UP and SCROLL_DOWN), the cursor
 * staying on its row of the page, or a jump (L1 and R1, PAGE_PREV and
 * PAGE_NEXT), the cursor going m - 2 rows up or down. Refused on a page that
 * does not go on that way.
 */
static int nav_move(u8 row, int down, int jump)
{
	struct menu_nav *cur = &g_nav[g_depth - 1];
	u16 ids[MENU_ITEMS_MAX];
	u8 n = page_items(cur->page, ids);
	u8 m = screen_rows(cur->page, g_depth);
	u8 start = cur->start;
	int a = (int)start + (int)row, step = m > 2 ? m - 2 : 1;
	u8 next;

	if (n <= m || (down ? start >= last_start(n, m) : start == 0)) return ST_BAD_ARG;
	if (a >= n) a = n - 1;
	if (jump) a = down ? (a + step < n ? a + step : n - 1) : (a > step ? a - step : 0);
	next = window_at(n, m, (u8)a, down);
	/* A slide always moves: the cursor's row may be the window's only one it can stand on. */
	if (down && next <= start) next = (u8)(start + 1);
	if (!down && next >= start) next = (u8)(start - 1);
	cur->start = next;
	a -= next;
	cur->cursor = (u8)(a < 0 ? 0 : a >= m ? m - 1 : a);
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

/*
 * Whether the current page wants building again ten times a second (driver):
 * a live page, or any page while a row's answer is on it, until the build
 * after its two seconds, which takes it off (note_hint).
 */
static int menu_model_live(void)
{
	return page_live(g_nav[g_depth - 1].page) || g_note_id != 0;
}

#ifdef QWARK_TEST
static u32 g_test_builds;
u32 menu_test_builds(void) { return g_test_builds; }
#endif

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
	row(&r, MROW_BACK, MROWF_ENABLED, label, "");
	push_row(v, &r);
}

void menu_model_view(struct menu_view *v)
{
	struct menu_nav *cur;
	struct menu_row r;
	u16 ids[MENU_ITEMS_MAX];
	u8 n, m, content, i;

	memset(v, 0, sizeof(*v));
	g_game = session_game();
	g_ul.ok = 0;
	cur = &g_nav[g_depth - 1];
#ifdef QWARK_TEST
	g_test_builds++;
#endif

	m = screen_rows(cur->page, g_depth);
	lib_read(cur->page, cur->start, m);
	n = page_items(cur->page, ids);
	/* A start past the last window (the page got shorter) comes back to it, its names read again. */
	if (cur->start > last_start(n, m)) {
		cur->start = last_start(n, m);
		if (g_lib.on) lib_read(cur->page, cur->start, m);
	}
	content = n - cur->start < m ? (u8)(n - cur->start) : m;

	v->key = ((u32)cur->page << 16) | ((u32)cur->start << 8) | g_visit;
	v->pflags = (g_depth == 1 ? MBOX_PAGE_ROOT : 0) | (cur->start > 0 ? MBOX_PAGE_MORE_UP : 0) |
	            (cur->start + content < n ? MBOX_PAGE_MORE_DOWN : 0);
	v->ptype = page_type(cur->page);
	page_title(cur->page, v->title, sizeof(v->title));
	note_hint(v->hint, sizeof(v->hint));

	for (i = 0; i < content; i++) {
		build_row(cur->page, ids[cur->start + i], &r);
		push_row(v, &r);
	}
	if ((g_caps & MBOX_CAP_BACK) == 0 && g_depth > 1) nav_row(v, MENU_NAV_BACK, "Back");

	/* A page with nothing on it still has a row, which says so. */
	if (v->nrows == 0) {
		build_row(cur->page, MENU_ROW_ID(MENU_ROW_TEXT, MENU_TEXT_EMPTY), &r);
		push_row(v, &r);
	}

	v->cursor = cur->cursor < v->nrows ? cur->cursor : (u8)(v->nrows - 1);

	/* The names in the scratch are this build's; `sig` stays, for a press on this view. */
	g_lib.on = 0;
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
		step = value_step(f);
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

/*
 * The cross on a VALUE row: the next step, back to 1 past the field's range;
 * on a slider (round 4) the fine step of 1 and back to the tenth.
 */
static void cycle_step(const struct feature_desc *f)
{
	s64 lo, hi;
	u8 next;

	if (f->id >= QWARK_MAX_FEATURES) return;
	if (value_slider(f)) {
		g_step[f->id] = g_step[f->id] == 0;
		return;
	}
	value_range(f, &lo, &hi);
	next = (u8)(g_step[f->id] + 1);
	if (next >= STEP_LEVELS || (s64)g_pow10[next] > hi - lo) next = 0;
	g_step[f->id] = next;
}

/* A CLOSES action, set up for the driver (round 5: its row says nothing meanwhile). */
static int defer(u8 kind, u8 arg, const char *label)
{
	g_defer.kind = kind;
	g_defer.arg = arg;
	g_defer.waiting = 0;
	set_text(g_defer.label, sizeof(g_defer.label), label);
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
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0) return defer(DEFER_FEATURE, f->id, f->label);
		return answer(id, features_trigger(f->id), f->label);
	case FEATURE_VALUE:
		if (dir != 0) return answer(id, step_feature(f, dir), f->label);
		if ((g_caps & MBOX_CAP_STEPS) != 0) cycle_step(f);
		return ST_OK;
	case FEATURE_ENUM:
		if (dir == 0 && page_type(g_nav[g_depth - 1].page) == MBOX_PTYPE_MAIN) {
			count = enum_options(f, &opts);
			if (count == 0) return ST_UNSUPPORTED;
			pos = enum_index(f, count);
			start = window_at(count, screen_rows(MENU_PAGE_ID(MENU_PAGE_CHOICE, f->id), (u8)(g_depth + 1)),
			                  pos, 1);
			return nav_push(MENU_PAGE_ID(MENU_PAGE_CHOICE, f->id), row, start, (u8)(pos - start));
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
		/* Round 5: Off, On, On boot (manip_reset), the cross and Right forward, wrapping. */
		cur = cycle(manip_reset(opts), 3, dir);
		return answer(id, manips_options(cur != 0, cur == 2), "Auto-reset");
	case MENU_MANIP_APPLY:
		if (dir != 0) return ST_OK;
		return answer(id, manips_apply(), "Apply now");
	default:
		return ST_OK;
	}
}

/*
 * Round 5: a VALUE as a button, which writes the game's value (Max bolts), or
 * as a switch (Challenge mode): Off writes 0, On the menu value's (1), since
 * a switch that reads Off reads 0. The row then shows what was written until
 * the readout agrees, as a stepped value did.
 */
static int act_value(u16 id, u8 arg, int dir)
{
	const struct game_menu *gm = gmenu();
	const struct menu_value *mv = gm != NULL && arg < gm->nvalues ? &gm->values[arg] : NULL;
	const struct feature_desc *f = mv != NULL ? feature(mv->id) : NULL;
	u32 raw;
	int rc;

	if (f == NULL) return ST_NOT_FOUND;
	if (mv->kind == MENU_VALUE_SWITCH) raw = value_on(f) ? 0 : mv->value;
	else if (dir != 0) return ST_OK;
	else raw = mv->value;
	rc = features_set(f->id, raw);
	if (rc == ST_OK) remember_value(f->id, raw);
	return answer(id, rc, mv->label != NULL ? mv->label : f->label);
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
		if (arg == MENU_ITEM_POS_SAVE) return answer(id, session_position_save(0xFF), "Save position");
		return answer(id, session_position_load(0xFF), "Load position");

	case MENU_ROW_PLANET:
		if (dir != 0) return ST_OK;
		names = g_game->planet_names(&count);
		return defer(DEFER_PLANET, arg, arg < count ? names[arg] : NULL);

	case MENU_ROW_VALUE:
		return act_value(id, arg, dir);

	case MENU_ROW_MANIP:
		return act_manip(id, arg, dir);

	case MENU_ROW_UNLOCK:
		return act_unlock(id, arg, dir);

	case MENU_ROW_SAVE: {
		char cat[LIB_NAME], name[LIB_NAME];
		u32 sig = 0;

		if (dir != 0) return ST_OK;
		/*
		 * 1.4.1 (finding 10): the row is the save in its place as the page
		 * was built. An upload, a rename or a delete from a PC since then can
		 * put another save there, so the press stands only while the category
		 * lists what it listed then; otherwise it is stale, refused as an
		 * event on an old page is, and the page built again shows the change.
		 */
		name[0] = 0;
		if (save_category(page_arg(cur->page), cat)) savefile_menu_list(cat, arg, 1, name, &sig);
		if (sig != g_lib.sig) return ST_BAD_ARG;
		if (name[0] == 0) return answer(id, ST_NOT_FOUND, NULL);
		qstrcpy(g_defer.cat, sizeof(g_defer.cat), cat);
		qstrcpy(g_defer.name, sizeof(g_defer.name), name);
		return defer(DEFER_RESTORE, arg, name);
	}

	case MENU_ROW_NAV:
		if (dir != 0) return ST_OK;
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
		return nav_move(row, 0, 1);
	case MBOX_EV_PAGE_NEXT:
		return nav_move(row, 1, 1);
	case MBOX_EV_SCROLL_UP:
		return nav_move(row, 0, 0);
	case MBOX_EV_SCROLL_DOWN:
		return nav_move(row, 1, 0);
	default:
		break;
	}

	if (row >= g_view_n || g_view_id[row] != id) return ST_BAD_ARG;

	switch (kind) {
	case MBOX_EV_ACTIVATE: return act(row, id, 0);
	case MBOX_EV_LEFT:     return act(row, id, -1);
	case MBOX_EV_RIGHT:    return act(row, id, 1);
	case MBOX_EV_PICKER_OTHERS:
		/*
		 * The pickers' round 2: on the Planets row, the game's picker asked for
		 * the planets it cannot show (UYA's map, the vid comics): the game's
		 * page of them, or the whole Planets page where it names none.
		 */
		if (id != MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_PLANETS)) return ST_BAD_ARG;
		return nav_push(MENU_PAGE_ID(MENU_PAGE_PLANETS,
		                             gmenu() != NULL && gmenu()->picker_others != NULL ? MENU_PLANETS_OTHERS : 0),
		                row, 0, 0);
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
#define WHY_NO_PAK       0x08

#define NO_PAK_TEXT      "Trainer menu: ui.pak missing or from another build"

static int  g_state = MENU_OFF;
/*
 * Build 55: the session's renderer, read out of ui.pak the first time it is
 * wanted (load), and the game it is for. NULL until then.
 */
static const struct mh_desc *g_mh;
static u8   g_game_id;
/* The pak's looks (uipak_looks) when MENU_NOPAK began; a new one is a try again. */
static u32  g_pak_looks;
/* Said once on the console in the module's life, not every session; a good pak resets it. */
static int  g_pak_told;
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
/* Mailbox version 3: the renderer's pick_seq when last read; a new one is a pick to load. */
static u32  g_pick_seq;
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

/*
 * Revision 1.23, build 55: config.txt's `menu.help`, read beside menu.enabled,
 * and what the Help row's words in the game are: -1 not known (nothing of the
 * menu in, or an install since they were last put right), 0 the game's, 1 the
 * menu's. While the two differ a swap waits for a safe tick (help_sync).
 */
static struct {
	int want;          /* menu.help */
	int in;            /* the words in the game: -1, 0 or 1 */
	u64 retry;         /* another mod's word stopped them: not before then */
	int logged;        /* and the log has said so, once a session */
} g_help = { 1, -1, 0, 0 };

int menu_state(void) { return g_state; }
u64 menu_install_delay_us(void) { return g_install_delay; }
void menu_set_install_delay_us(u64 us) { g_install_delay = us; }

u32 menu_mailbox(void)
{
	if (g_mh == NULL || (g_state != MENU_READY && g_state != MENU_REMOVING)) return 0;
	return g_mh->mailbox;
}

/* config.txt `menu.enabled` and (revision 1.23) `menu.help`, each 1 unless it says 0 (PROTOCOL.md 5.10). */
static void read_switches(void)
{
	u32 v = config_version();

	if (v != g_enabled_version) {
		g_enabled_version = v;
		g_enabled = config_menu_enabled();
		g_help.want = config_menu_help();
	}
}

static int enabled(void)
{
	read_switches();
	return g_enabled;
}

static int help_wanted(void)
{
	read_switches();
	return g_help.want;
}

static u32 box(u32 off)
{
	return g_mh->mailbox + off;
}

void menu_forget(void)
{
	g_state = MENU_OFF;
	g_mh = NULL;
	mh_pak_close();
	g_posted = 0;
	g_hstate = MBOX_HSTATE_CLOSED;
	g_req = 0;
	g_help.in = -1;
	g_help.retry = 0;
	memset(&g_defer, 0, sizeof(g_defer));
}

void menu_enter(const struct game_api *game)
{
	menu_forget();
	g_why = 0;
	g_logged_refusal = 0;
	g_help.logged = 0;
	menu_model_reset(8, 0, 0);

	/*
	 * A renderer is code, so where code cannot be patched (RPCS3) there is no
	 * menu at all: nothing is installed and the combo says so.
	 *
	 * Build 55: the renderer is read out of ui.pak when it is first wanted
	 * (load), not here, so a session's entry reads no file for it; a game
	 * the pak has no renderer for is MENU_OFF from then on.
	 */
	if (game == NULL || !plat_can_patch_code()) return;
	g_game_id = game->game_id;

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
	/*
	 * The view and the page it becomes, in the tick's scratch: 2.7 KB and
	 * 2.1 KB at most (32 rows), and the build's save names past them (LIB_AT).
	 */
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

/*
 * The view and the page image fit the tick's scratch, as post() lays them out,
 * clear of the save library's names that the build keeps at its end (LIB_AT).
 */
typedef char menu_scratch_fits[(sizeof(struct menu_view) <= 4096 &&
                                4096 + MBOX_OFF_ROWS + MENU_VIEW_MAX * MBOX_ROW_SIZE <= LIB_AT) ? 1 : -1];

/*
 * Whether the code cave holds this build's renderer, byte for byte: the game's
 * memory in the scratch's first half, ui.pak's bytes (build 55) in its second.
 */
static int code_matches(const struct mh_cave *c)
{
	u8 *game = qscratch();
	u8 *ours = qscratch() + QSCRATCH_BYTES / 2;
	u32 off = 0;

	while (off < c->len) {
		u32 n = c->len - off;
		if (n > QSCRATCH_BYTES / 2) n = QSCRATCH_BYTES / 2;
		if (mem_read(c->addr + off, game, n) != ST_OK) return 0;
		if (mh_pak_read(c, off, ours, n) != ST_OK) return 0;
		if (memcmp(game, ours, n) != 0) return 0;
		off += n;
	}
	return 1;
}

#define INSTALL_REFUSED 100
#define INSTALL_NO_PAK  101

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
 * Build 55: a cave out of ui.pak into the game, a scratch at a time, then its
 * zeros. Nothing branches into a cave until its hook words are in, and they go
 * in last, so a cave may arrive in pieces, as a mod's caves do.
 */
static int write_cave(const struct mh_cave *c)
{
	u8 *buf = qscratch();
	u32 off = 0, n;
	int rc;

	while (off < c->len) {
		n = c->len - off;
		if (n > QSCRATCH_BYTES) n = QSCRATCH_BYTES;
		rc = mh_pak_read(c, off, buf, n);
		if (rc == ST_OK) rc = mem_write(c->addr + off, buf, n);
		if (rc != ST_OK) return rc;
		off += n;
	}
	return c->zeros != 0 ? write_zeros(c->addr + c->len, c->zeros) : ST_OK;
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
 * twenty-six writes, about 20 KB in all for Deadlocked (the zeros of the
 * mailbox and the state written out of the tick's scratch). Nothing can branch
 * into a cave before its hook word, every hook word goes in after the caves,
 * and a combo's request byte goes in last of all.
 *
 * Build 55: the caves' bytes come out of ui.pak, a scratch at a time, once
 * the hook words have been read and the pak checked (mh_pak_open): one file
 * open and the game's part read twice, about 16 to 18 KB, the first time to
 * check its CRC. A pak that cannot serve writes nothing (INSTALL_NO_PAK).
 *
 * Revision 1.23: the Help row's words, the last `nhelp` of the table, are not
 * written here. help_sync puts them in, or leaves them out for menu.help = 0,
 * straight after a good install, in the same tick when it is safe to (the
 * session's install always is: it runs in ordinary play), so they still go in
 * last. Each must hold the game's word or the menu's as the others must, both
 * being fine either way; with menu.help = 0 a third mod's word there is that
 * mod's business, and refuses nothing.
 */
static int install(void)
{
	const struct mh_desc *d = g_mh;
	u8 head[MBOX_READ_HEAD];
	u8 first_help = (u8)(d->nhooks - d->nhelp);
	int help = help_wanted();
	u32 w;
	int ours = 0;
	u8 i;
	int rc;

	for (i = 0; i < d->nhooks; i++) {
		if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return ST_IO_ERROR;
		if (w == d->hooks[i].original) continue;
		if (w == d->hooks[i].value) { ours++; continue; }
		if (i >= first_help && !help) continue;
		if (!g_logged_refusal) {
			plat_log("menu: 0x%08X holds 0x%08X, neither the game's word nor the menu's; "
			         "another mod has it, so no menu", (int)d->hooks[i].addr, (int)w);
			g_logged_refusal = 1;
		}
		return INSTALL_REFUSED;
	}

	/*
	 * Build 55: the renderer's bytes are in ui.pak. It must still be this
	 * build's and the game's part of it whole before a byte of it is compared
	 * or written; mh_pak_close (try_install) closes it again.
	 */
	if (mh_pak_open() != UIPAK_OK) return INSTALL_NO_PAK;

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
			rc = write_cave(&d->caves[i]);
			if (rc != ST_OK) return rc;
		}
		if (mem_read(d->mailbox, head, sizeof(head)) != ST_OK) return ST_IO_ERROR;
	}

	/*
	 * The block's shape, as the renderer declared it: rows of 0x40 bytes (round
	 * 4: no longer 0x80, with descriptions), at least three of them, all inside
	 * the block. A renderer that says otherwise is one this build cannot serve.
	 */
	if (head[MBOX_OFF_ROW_SIZE] != MBOX_ROW_SIZE ||
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

	/* Take the mailbox as it is: an event left from before is answered unread, a pick dropped. */
	g_page_seq = (be32_get(head + MBOX_OFF_PAGE_SEQ) + 1u) & ~1u;
	g_last_ack = be32_get(head + MBOX_OFF_EV_SEQ);
	g_pick_seq = be32_get(head + MBOX_OFF_PICK_SEQ);
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
			rc = write_cave(&d->caves[i]);
			if (rc != ST_OK) return rc;
		}
	}

	for (i = 0; i < first_help; i++) {
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

static void help_sync(void);

static void unavailable(u8 why, const char *msg)
{
	if ((g_why & why) != 0) return;
	g_why |= why;
	plat_notify(msg);
}

/*
 * Build 55: ui.pak is missing, damaged or another build's (uipak.c has logged
 * which). Nothing of the menu goes in, and the rest of qwark carries on. The
 * console says so once in the module's life, at the first session it stops,
 * not in every session; a press of the combo says it again, once a session.
 * MENU_NOPAK is left once the pak has been looked at again, which a client's
 * write of it does (uipak_touch), and at a press of the combo.
 */
static void no_pak(void)
{
	g_state = MENU_NOPAK;
	g_pak_looks = uipak_looks();
	if (!g_pak_told) {
		g_pak_told = 1;
		unavailable(WHY_NO_PAK, NO_PAK_TEXT);
	}
}

/*
 * Build 55: the session's renderer out of ui.pak, when it is first wanted.
 * 1 with g_mh set. A game the pak has no renderer for has no menu (MENU_OFF),
 * as a game had with no renderer in the module; a pak that cannot serve leaves
 * the session at MENU_NOPAK.
 */
static int load(void)
{
	int st;

	if (g_mh != NULL) return 1;
	g_mh = mh_pak_desc(g_game_id, &st);
	if (g_mh != NULL) return 1;
	if (st == UIPAK_OK) g_state = MENU_OFF;
	else no_pak();
	return 0;
}

/*
 * install(), and the state it leaves: READY, REFUSED for another mod's word or
 * another build's renderer, IDLE for a write that failed. Either is tried
 * again ten seconds on (menu_tick), or sooner by a press of the combo.
 * Answers 1 when the menu serves. Revision 1.23: READY puts the Help row's
 * words right at once if it is safe, and at the first safe tick otherwise.
 */
static int try_install(void)
{
	int rc = install();

	/* Build 55: ui.pak is open only inside the install. */
	mh_pak_close();
	if (rc == INSTALL_NO_PAK) {
		no_pak();
		return 0;
	}
	if (rc == ST_OK) {
		g_state = MENU_READY;
		g_pak_told = 0;
		g_help.in = -1;
		help_sync();
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

/*
 * Revision 1.23: whether the Help row's words may change now, which is
 * whenever nothing can be reading the rows they sit in. The menu down; the
 * game in ordinary play, its mode 0 with no mode change queued (RaC1: its
 * state 0 and no reopen of the pause queued), so no pause menu is up or being
 * built and no move between its screens is under way; and none of the
 * renderer's screens up. The install's moment, with uninstall()'s screen test.
 */
static int help_safe(void)
{
	const struct mh_desc *d = g_mh;
	u32 screen = 0;

	if (g_hstate != MBOX_HSTATE_CLOSED || !game_playing()) return 0;
	if (mem_read_u32(d->screen_addr, &screen) != ST_OK) return 0;
	return screen != d->screen_id;
}

/*
 * Revision 1.23, build 55: the Help row's words (the table's last `nhelp`)
 * made what `menu.help` asks for while the rest of the renderer stays in: 1,
 * the menu's Trainer row where the game's Help was; 0, the game's own Help.
 * Only while help_safe() says so; otherwise the swap waits for a later tick,
 * so a pause menu on screen when the setting changes keeps its rows until it
 * has closed. In they go in the table's order and out they come in the
 * reverse, as install() and uninstall() put them. A word another mod holds
 * stops them going in, as it stops an install, and the swap is tried again ten
 * seconds on; going out, such a word is that mod's and is left alone.
 */
static void help_sync(void)
{
	const struct mh_desc *d = g_mh;
	int want = help_wanted();
	int first = (int)d->nhooks - (int)d->nhelp;
	int wrote = 0;
	u32 w;
	int i;

	if (g_help.in == want) return;
	if (plat_time_us() < g_help.retry || !help_safe()) return;

	for (i = first; want && i < (int)d->nhooks; i++) {
		if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return;
		if (w == d->hooks[i].original || w == d->hooks[i].value) continue;
		if (!g_help.logged) {
			plat_log("menu: 0x%08X holds 0x%08X, another mod's; the Help row stays the game's",
			         (int)d->hooks[i].addr, (int)w);
			g_help.logged = 1;
		}
		g_help.retry = plat_time_us() + RETRY_US;
		return;
	}

	if (want) {
		for (i = first; i < (int)d->nhooks; i++) {
			if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return;
			if (w != d->hooks[i].original) continue;
			if (mem_write_u32(d->hooks[i].addr, d->hooks[i].value) != ST_OK) return;
			wrote = 1;
		}
	} else {
		for (i = (int)d->nhooks - 1; i >= first; i--) {
			if (mem_read_u32(d->hooks[i].addr, &w) != ST_OK) return;
			if (w != d->hooks[i].value) continue;
			if (mem_write_u32(d->hooks[i].addr, d->hooks[i].original) != ST_OK) return;
			wrote = 1;
		}
	}
	/* An install says so itself; a swap in a running game is worth its own line. */
	if (wrote && g_help.in >= 0)
		plat_log("menu: the Help row is %s", want ? "the menu's again" : "the game's own");
	g_help.in = want;
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

/*
 * Mailbox version 3: what the player picked in the game's own planet picker,
 * which the menu's Planets row opened. The renderer has closed the game's menu
 * on the pick; the planet loads once the game plays again, as a planet row of
 * the menu's own Planets page loads it (session_planet_load, the selected
 * flags), whatever the request cap.
 */
static void take_pick(s32 planet)
{
	const struct game_api *game = session_game();
	const char * const *names;
	u8 count = 0;

	if (game == NULL || game->planet_names == NULL || game->planet_load == NULL) return;
	names = game->planet_names(&count);
	if (planet < 0 || planet >= (s32)count) {
		plat_log("menu: a pick of planet %d, which the game has not; nothing loads", (int)planet);
		return;
	}
	g_defer.kind = DEFER_PLANET;
	g_defer.arg = (u8)planet;
	g_defer.waiting = 1;
	g_defer.until = plat_time_us() + DEFER_US;
	set_text(g_defer.label, sizeof(g_defer.label), names[planet]);
}

/* The mailbox: requests, events, picks, live values. Every POLL_* ticks. */
static void serve(void)
{
	u8 head[MBOX_READ_HEAD];
	u32 tick = now_tick();
	u32 ev, frames, pick;
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

	pick = be32_get(head + MBOX_OFF_PICK_SEQ);
	if (pick != g_pick_seq) {
		g_pick_seq = pick;
		take_pick((s32)be32_get(head + MBOX_OFF_PICK_PLANET));
	}

	deferred_tick();

	/* Revision 1.23: a menu.help change, or a swap still waiting, once it is safe. */
	if (g_state == MENU_READY) help_sync();
}

void menu_tick(void)
{
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
			g_help.in = -1;
		}
		return;

	case MENU_NOPAK:
		/*
		 * Build 55: no usable ui.pak. Looked at again only once the pak has
		 * been (a client wrote it, uipak_touch), not every ten seconds.
		 */
		if (uipak_looks() == g_pak_looks) return;
		g_state = MENU_IDLE;
		return;

	case MENU_IDLE:
	case MENU_REFUSED:
		/*
		 * Nothing of the menu is in the game yet. It goes in once it is due
		 * and the game is in ordinary play, so nothing is written while a
		 * menu of the game's is up; a refusal waits ten seconds for the other
		 * mod to go, and says so once in the log. Build 55: the renderer is
		 * read out of ui.pak first, the first time it is due.
		 */
		if (!enabled() || plat_time_us() < g_install_at) return;
		if (!load()) return;
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

void menu_combo(void)
{
	if (g_state == MENU_OFF) {
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
	 * cost). Build 55: and after ui.pak was found wanting, it is read again.
	 */
	if (g_state != MENU_READY && !(load() && try_install())) {
		if (g_state == MENU_REFUSED)
			unavailable(WHY_REFUSED, "Trainer menu: another mod holds the menu's hooks");
		else if (g_state == MENU_NOPAK)
			unavailable(WHY_NO_PAK, NO_PAK_TEXT);
		else if (g_state == MENU_OFF)
			unavailable(WHY_NO_RENDERER, "Trainer menu: not available in this game yet");
		return;
	}

	if ((g_caps & MBOX_CAP_REQUEST) == 0) return;
	/* Up, or under the game's planet picker it opened (which takes no request, so nothing happens). */
	if (g_hstate != MBOX_HSTATE_CLOSED) {
		request(MBOX_REQ_CLOSE);
	} else {
		/* The page it opens on is the current one, values and all. */
		post(0);
		request(MBOX_REQ_OPEN);
	}
}
