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
#if QWARK_HAS_TOD
#include "luacore.h"
#endif
#if QWARK_PREVIEW
#include "mods.h"
#include "modusb.h"
#endif

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
 * Revision 1.29, the polish (MBOX_CAP_PAGED): the way a window just turned,
 * 1 down, -1 up (0 none), so the next view puts a cursor that landed on a row
 * it cannot stand on (a spacer, a status row) on the nearest one it can, that
 * way first.
 */
static s8 g_turn;
/*
 * Bumped on every move between pages or windows, so a page's key changes with
 * every visit and an event made on an earlier visit is never taken for this one.
 */
static u8 g_visit;

#ifdef QWARK_TEST
/* The step a VALUE row moves by, as a power of ten, per feature id (the steppers, below). */
static u8 g_step[QWARK_MAX_FEATURES];
#endif

/* The last view built: what an event has to be about. */
static u32 g_view_key;
static u8  g_view_n;
static u16 g_view_id[MENU_VIEW_MAX];

/*
 * A VALUE just written from the menu (round 5: a menu value's button or
 * switch). Its readout lags the write by up to a slow block's period
 * (session.c), so the row shows what was written until the readout agrees or
 * a quarter second has gone by.
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
#if QWARK_HAS_TOD
#define DEFER_PICKER  4         /* revision 1.29: the game's own planet picker (struct menu_picker) */
#endif
static struct {
	u8   kind;
	u8   arg;
	u8   waiting;           /* the driver has asked for the close */
#if QWARK_HAS_TOD
	u8   flags;             /* revision 1.29: a planet's load flags, 0xFF the selected ones */
	u8   closed;            /* revision 1.29: the menu was seen closed since */
#endif
	u64  until;
	char cat[SAVEFILE_NAME_LEN + 1];
	char name[SAVEFILE_NAME_LEN + 1];
	char label[32];
} g_defer;

#if QWARK_HAS_TOD
/* Revision 1.29: the game's own planet picker, the driver's (native_picker_*). */
static int native_picker_act(u8 row);
static void native_picker_drop(void);
#endif

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
 * The tests' own menu table in place of the game's, NULL for the game's again
 * (a root of seven rows, and the steppers below).
 */
static const struct game_menu *g_test_menu;
void menu_test_menu(const struct game_menu *gm) { g_test_menu = gm; }
#if QWARK_PREVIEW
static int g_test_no_mods;
void menu_test_mods_row(int on) { g_test_no_mods = !on; }
#endif
#endif

static __attribute__((noinline)) const struct game_menu *gmenu(void)
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

/*
 * Out of line (build 56): -Os put a copy of the switch's table in each of its
 * four callers.
 */
static __attribute__((noinline)) const char *status_text(u8 rc)
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

/*
 * A console notification, "Trainer: " and three pieces: a load the menu ran
 * once it closed that failed or was dropped, and (build 56) a refusal of the
 * Mods page's.
 */
static void tell(const char *a, const char *b, const char *c)
{
	char msg[128];

	qstrcpy(msg, sizeof(msg), "Trainer: ");
	qstrcat(msg, sizeof(msg), a);
	qstrcat(msg, sizeof(msg), b);
	qstrcat(msg, sizeof(msg), c);
	plat_notify(msg);
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

/* The current value of a VALUE: the readout, or what was just written. */
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

#ifdef QWARK_TEST
/*
 * The steppers: a VALUE listed in a section as itself, whose Left and Right
 * step the number and whose cross changes the step (round 1), or which a
 * renderer draws as a native slider (round 3: MROWF_RANGE, its four words at
 * MROW_OFF_RANGE; round 4, a step of a tenth). Since round 5 no game's table
 * lists a VALUE so (its values are buttons and switches, MENU_ITEM_VALUE), so
 * the module carries none of it: it is the tests' own, on a table of theirs
 * (menu_test_menu), for the renderers' sliders, which ui.pak still carries.
 * test_menu.c holds every game's table to the rows the module serves.
 */
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
#endif /* QWARK_TEST: the steppers */

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
	return (k == MENU_PAGE_SECTION || k == MENU_PAGE_MANIPS ||
	        k == MENU_PAGE_UNLOCK_CAT || k == MENU_PAGE_UNLOCK_ACTIONS || k == MENU_PAGE_PLANETS ||
	        k == MENU_PAGE_SAVES || k == MENU_PAGE_SAVE_CAT || k == MENU_PAGE_MODS) ? MBOX_PTYPE_CONFIG : MBOX_PTYPE_MAIN;
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
	return k == MENU_PAGE_SECTION || k == MENU_PAGE_MANIPS || k == MENU_PAGE_UNLOCK_CAT || k == MENU_PAGE_MODS;
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
	case MENU_PAGE_MODS:    return "Mods";
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
 * Round 5: a blank row between a page's parts (the Manips page's category and
 * its headers' features), never opening the page or following another.
 */
static u8 push_spacer(u16 *ids, u8 n)
{
	if (n == 0 || ids[n - 1] == SPACER_ID) return n;
	return push_id(ids, n, SPACER_ID);
}

#if QWARK_PREVIEW
/*
 * Build 56, a preview feature: the Mods page (PROTOCOL.md 9.1). One row a mod
 * of the running game, as MOD_LIST and MOD_USB_LIST have them: the installed
 * ones (row argument the mod's index, which a rescan keeps) and those only a
 * USB drive has (MENU_MOD_USB + their place in the USB list), in the order
 * MOD_LIST and MOD_USB_LIST give them. Each is Off, On or On boot, the Manips page's Auto-reset words:
 * not loaded, loaded, loaded and auto-loaded at every boot (MOD_SET_AUTO's
 * flag, which config.txt keeps as the client's does). A press is checked
 * against the row id the last build put there, as on every page, so a USB
 * list read again in between, which may order it otherwise, changes nothing.
 */
struct menu_mod {
	const char *dir;
	u8 flags;              /* MOD_FLAG_*: 0 for one only USB has */
	u8 usb;                /* 1 for one only USB has */
};

/* Row argument `arg`'s mod: its name, NULL for none. */
static const char *mod_of(u8 arg, struct menu_mod *o)
{
	const struct modusb_entry *u;
	const struct mod_entry *m;

	o->usb = arg >> 7;
	o->flags = 0;
	if (o->usb) {
		u = modusb_at(arg & (MENU_MOD_USB - 1));
		if (u == NULL) return NULL;
		o->dir = u->dirname;
		return u->name;
	}
	m = mods_at(arg);
	if (m == NULL) return NULL;
	o->dir = m->dirname;
	o->flags = m->flags;
	return m->name;
}

/* 0 Off, 1 On, 2 On boot. A mod that is not loaded reads Off, its auto flag or not, as a toggle does. */
static u8 mod_now(u8 flags)
{
	if ((flags & MOD_FLAG_LOADED) == 0) return 0;
	return (flags & MOD_FLAG_AUTO) != 0 ? 2 : 1;
}

#endif

/* Every row of a page, as row ids, in order; how many. */
static u8 page_items(u16 page, u16 *ids)
{
	const struct game_menu *gm = gmenu();
	const struct feature_desc *f;
	char cat[SAVEFILE_NAME_LEN + 1];
	u8 arg = page_arg(page);
	u8 n = 0, i, j, count;

	switch (page_kind(page)) {
	case MENU_PAGE_ROOT:
		/*
		 * The game's sections first. (Round 1's page per DESCRIBE group, for
		 * a game with no menu table, went with build 56: all four have one.)
		 */
		if (gm != NULL) {
			for (i = 0; i < gm->nsections; i++) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_SECTION, i));
		}
		if (g_game != NULL && g_game->planet_names != NULL && g_game->planet_load != NULL)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_PLANETS));
		if (g_game != NULL && g_game->manips != NULL)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_MANIPS));
		if (g_game != NULL && g_game->unlock_list != NULL && g_game->unlock_set != NULL)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_UNLOCKS));
#if QWARK_PREVIEW
		/* Build 56, a preview feature: the Mods page, where a mod can go into the game. */
		if (plat_can_patch_code()
#ifdef QWARK_TEST
		    && !g_test_no_mods
#endif
		   ) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_MODS));
#endif
		if (savefile_library_gate() != ST_UNSUPPORTED)
			n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_SAVES));
		return n;

	case MENU_PAGE_SECTION:
		if (gm == NULL || arg >= gm->nsections) return 0;
		for (i = 0; i < gm->sections[arg].nitems; i++) {
			u8 it = gm->sections[arg].items[i];
			if (it >= MENU_ITEM_SLOT && it <= MENU_ITEM_POS_LOAD) {
				if (g_game->save_blob != NULL) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_POS, it));
#if QWARK_HAS_TOD
			} else if (it == MENU_ITEM_SPACER) {
				/*
				 * Revision 1.29: a blank row between a section's parts again
				 * (ToD's Manips: Remove cutscenes, then the cache's three), as
				 * the Manips page makes its own: never the page's first row
				 * nor after another, and none left at the end (below). A
				 * release build has no ToD and no section that lists one.
				 */
				n = push_spacer(ids, n);
#endif
			} else if (it >= MENU_ITEM_VALUE(0)) {
				/*
				 * Round 5: a VALUE as a button or a switch (struct menu_value).
				 * An item past the table's values is no row, and in a release
				 * build nor is MENU_ITEM_SPACER, which comes here (build 56).
				 */
				j = (u8)(it - MENU_ITEM_VALUE(0));
				if (j < gm->nvalues && feature(gm->values[j].id) != NULL)
					n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_VALUE, j));
			} else if ((f = feature(it)) != NULL) {
				n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_FEATURE, it));
#if QWARK_HAS_TOD
				/* Revision 1.29: an ACTION's status row after it, as on the Manips page (ToD's Cache cutscenes). */
				if (f->kind == FEATURE_ACTION && f->readout < QWARK_MAX_READOUTS && gm->action_status != NULL)
					n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_STATUS, it));
#endif
			}
		}
#if QWARK_HAS_TOD
		if (n > 0 && ids[n - 1] == SPACER_ID) n--;
#endif
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

#if QWARK_PREVIEW
	case MENU_PAGE_MODS: {
		/* The installed ones by index, then each the USB list has that the console does not. */
		struct menu_mod o;
		for (j = 0; j < 2 * QWARK_MAX_MODS; j++) {
			arg = j < QWARK_MAX_MODS ? j : (u8)(MENU_MOD_USB | (j - QWARK_MAX_MODS));
			if (mod_of(arg, &o) != NULL && (!o.usb || mods_find(o.dir) < 0)) n = push_id(ids, n, MENU_ROW_ID(MENU_ROW_MOD, arg));
		}
		return n;
	}
#endif

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
	const struct game_menu *gm = gmenu();
	u8 arg = page_arg(page);

	memset(out, 0, cap);
	switch (page_kind(page)) {
	case MENU_PAGE_SECTION:
		if (gm != NULL && arg < gm->nsections) qstrcpy(out, cap, gm->sections[arg].title);
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

static __attribute__((noinline)) u8 steps(void)
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

#ifdef QWARK_TEST
/*
 * Round 3, what a native slider needs (the steppers, the tests' alone): a
 * VALUE whose range is narrower than its 32-bit field (explicit bounds, or an
 * 8- or 16-bit field) is flagged MROWF_RANGE and carries its value, its bounds
 * and its step as numbers, its label held to the 23 characters such a row
 * leaves room for; one whose range spans MENU_SLIDER_SPAN or fewer is
 * MROWF_SLIDER as well.
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
#endif

/*
 * A feature the game's table lists: a toggle or an action. (Build 56: no
 * table lists an ENUM, a COLOR or, since round 5, a VALUE, and test_menu.c
 * holds them to it, so their rows went; a VALUE is still the tests' stepper.)
 */
static void feature_row(const struct feature_desc *f, struct menu_row *r)
{
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
#ifdef QWARK_TEST
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
#endif
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
	char text[LIB_NAME];
	u8 arg = (u8)id, count;
	const char *n;

	memset(r, 0, sizeof(*r));
	r->id = id;

	switch ((u8)(id >> 8)) {
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

#if QWARK_PREVIEW
	case MENU_ROW_MOD: {
		struct menu_mod o;
		n = mod_of(arg, &o);
		if (n == NULL) break;
		count = mod_now(o.flags);
		row(r, MROW_TOGGLE, MROWF_ENABLED | steps() | (count ? MROWF_ON : 0), n, g_reset_text[count]);
		break;
	}
#endif

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

/* Revision 1.29, the polish: windows a whole screen apart (MBOX_CAP_PAGED, Tools of Destruction's pause menu). */
static int paged(void)
{
	return (g_caps & MBOX_CAP_PAGED) != 0;
}

/* The last window's start: 0 for a page that fits; paged, the last whole screen's (that window short). */
static u8 last_start(u8 n, u8 m)
{
	if (n <= m) return 0;
	return paged() ? (u8)((n - 1) / m * m) : (u8)(n - m);
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
	g_turn = 0;
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
	g_turn = 0;
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

	/*
	 * Revision 1.29, the polish (MBOX_CAP_PAGED): a whole window on. A
	 * slide (the cursor leaving the window) puts the cursor on the next
	 * window's first row going down, the window before's last going up, and
	 * wraps past the page's ends as the game's lists do; a jump keeps the
	 * cursor's row and stops at the ends. menu_model_view then moves a
	 * cursor on a row it cannot stand on to the nearest one (g_turn).
	 */
	if (paged()) {
		u8 last = last_start(n, m);

		if (n <= m || (jump && (down ? start >= last : start == 0))) return ST_BAD_ARG;
		start = (u8)(start / m * m);
		if (down) next = start >= last ? 0 : (u8)(start + m);
		else next = start == 0 ? last : (u8)(start - m);
		if (jump) a = row;
		else if (down) a = 0;
		else a = (n - next < m ? n - next : m) - 1;
		if (a >= n - next) a = n - next - 1;
		cur->start = next;
		cur->cursor = (u8)a;
		g_turn = (s8)(down ? 1 : -1);
		g_visit++;
		return ST_OK;
	}

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
	g_turn = 0;
#ifdef QWARK_TEST
	memset(g_step, 0, sizeof(g_step));
#endif
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

/*
 * The view's next row, built where it goes (build 56: not on the stack and
 * copied in, a copy -Os spelt out word by word once a row was 64 bytes).
 */
static void push_row(struct menu_view *v, u16 page, u16 id)
{
	if (v->nrows >= MENU_VIEW_MAX) return;
	build_row(page, id, &v->rows[v->nrows]);
	v->nrows++;
}

static void nav_row(struct menu_view *v, u8 which, const char *label)
{
	struct menu_row *r = &v->rows[v->nrows];

	if (v->nrows >= MENU_VIEW_MAX) return;
	r->id = MENU_ROW_ID(MENU_ROW_NAV, which);
	row(r, MROW_BACK, MROWF_ENABLED, label, "");
	v->nrows++;
}

void menu_model_view(struct menu_view *v)
{
	struct menu_nav *cur;
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
	/* Revision 1.29, the polish: paged, a window starts a whole screen in (the cursor kept on its row of the page). */
	if (paged() && m != 0 && cur->start % m != 0) {
		u16 a = (u16)cur->start + cur->cursor;

		cur->start = (u8)(cur->start / m * m);
		cur->cursor = (u8)(a - cur->start < m ? a - cur->start : m - 1);
		if (g_lib.on) lib_read(cur->page, cur->start, m);
	}
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

	for (i = 0; i < content; i++) push_row(v, cur->page, ids[cur->start + i]);
	if ((g_caps & MBOX_CAP_BACK) == 0 && g_depth > 1) nav_row(v, MENU_NAV_BACK, "Back");

	/* A page with nothing on it still has a row, which says so. */
	if (v->nrows == 0) push_row(v, cur->page, MENU_ROW_ID(MENU_ROW_TEXT, MENU_TEXT_EMPTY));

	v->cursor = cur->cursor < v->nrows ? cur->cursor : (u8)(v->nrows - 1);
	/*
	 * Revision 1.29, the polish: a window just turned (paged) with the
	 * cursor on a row it cannot stand on: the nearest one it can, the way
	 * the window turned first, then the other way; kept for the next view.
	 */
	if (g_turn != 0 && (v->rows[v->cursor].flags & MROWF_ENABLED) == 0) {
		int d, k;

		for (d = g_turn, k = 0; k < 2; k++, d = -d) {
			int j;

			for (j = (int)v->cursor + d; j >= 0 && j < (int)v->nrows; j += d) {
				if ((v->rows[j].flags & MROWF_ENABLED) != 0) break;
			}
			if (j >= 0 && j < (int)v->nrows) {
				v->cursor = (u8)j;
				cur->cursor = (u8)j;
				break;
			}
		}
	}
	g_turn = 0;

	/* The names in the scratch are this build's; `sig` stays, for a press on this view. */
	g_lib.on = 0;
	g_view_key = v->key;
	g_view_n = v->nrows;
	for (i = 0; i < v->nrows; i++) g_view_id[i] = v->rows[i].id;
}

/* ------------------------------------------------------------ the events */

#ifdef QWARK_TEST
/* Left or Right (dir -1, +1) on a stepper (the steppers, the tests' alone). */
static int step_value(const struct feature_desc *f, int dir)
{
	s64 v, lo, hi, step;
	u32 raw;
	int rc;

	value_range(f, &lo, &hi);
	step = value_step(f);
	v = value_number(f, feature_value(f)) + (dir > 0 ? step : -step);
	if (v < lo) v = lo;
	if (v > hi) v = hi;
	raw = (u32)((u64)v & value_mask(f));
	rc = features_set(f->id, raw);
	if (rc == ST_OK) remember_value(f->id, raw);
	return rc;
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
#endif

/* A CLOSES action, set up for the driver (round 5: its row says nothing meanwhile). */
static int defer(u8 kind, u8 arg, const char *label)
{
	g_defer.kind = kind;
	g_defer.arg = arg;
	g_defer.waiting = 0;
#if QWARK_HAS_TOD
	g_defer.flags = 0xFF;
	g_defer.closed = 0;
#endif
	set_text(g_defer.label, sizeof(g_defer.label), label);
	return ST_OK;
}

/*
 * A feature row: the cross (dir 0) or Left and Right (-1, +1). A toggle's
 * cross moves it on as Right does, as the game's Cheats screen does. (Build
 * 56: an ENUM's page of options and its steps went with its rows.)
 */
static int act_feature(u16 id, const struct feature_desc *f, int dir)
{
	switch (f->kind) {
	case FEATURE_ACTION:
		if (dir != 0) return ST_OK;
		if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0) return defer(DEFER_FEATURE, f->id, f->label);
		return answer(id, features_trigger(f->id), f->label);
	case FEATURE_TOGGLE:
		return answer(id, toggle_set(f, cycle((u8)toggle_now(f), toggle_auto(f) ? 3 : 2, dir)), f->label);
#ifdef QWARK_TEST
	case FEATURE_VALUE:
		if (dir != 0) return answer(id, step_value(f, dir), f->label);
		if ((g_caps & MBOX_CAP_STEPS) != 0) cycle_step(f);
		return ST_OK;
#endif
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

#if QWARK_PREVIEW
/*
 * The Mods page's rules (build 56, a preview feature). MOD_LOAD's own do most
 * of it: a mod whose patch.txt qwark could not parse does not load
 * (IO_ERROR), nor one that needs (`#- depends`, by `#- name`) a mod the
 * console does not have (NOT_FOUND), and a mod only a USB drive has is copied
 * in first, a copy that cannot be taken leaving everything as it was
 * (modusb.c). The page loads through mods_load_guarded, which adds two: no
 * part of a mod goes into the game over a loaded mod or over qwark's own code
 * there (mem_owner, mem.h), the rule the client kept under RPCS3 alone; and a
 * load that fails half way takes back every mod it put in.
 * Unloading is MOD_UNLOAD's, as the client's is. A refusal shows in the row
 * for two seconds, as any row's does, and as a console notification that
 * names the mod and the reason.
 */
static const char g_menu_who[] = "the trainer menu";

/* A mod's row: the cross and Right forward through Off, On, On boot, Left back, wrapping. */
static int act_mod(u16 id, u8 arg, int dir)
{
	struct menu_mod o;
	char d[32], name[32];
	const char *n = mod_of(arg, &o), *who = NULL;
	u8 now, want;
	int rc = ST_OK;

	if (n == NULL) return ST_NOT_FOUND;
	qstrcpy(d, sizeof(d), o.dir);
	qstrcpy(name, sizeof(name), n);
	now = mod_now(o.flags);
	want = cycle(now, 3, dir);
	if (want == 0) rc = mods_unload(d);
	else if (now == 0) rc = mods_load_guarded(d, &who);
	/* A mod only USB had is installed now, with no auto flag (modusb.c clears a stale one). */
	if (rc == ST_OK && ((o.flags & MOD_FLAG_AUTO) != 0) != (want == 2)) rc = mods_set_auto(d, want == 2);
	if (rc != ST_OK)
		tell(name, who != NULL ? " would write over " : ": ",
		     who != NULL ? who : o.usb ? "its USB copy can't be used" :
		     rc == ST_NOT_FOUND ? "a mod it needs is missing" : status_text((u8)rc));
	return answer(id, who != NULL ? ST_BAD_ARG : rc, name);
}
#endif

static int act(u8 row, u16 id, int dir)
{
	struct menu_nav *cur = &g_nav[g_depth - 1];
	const struct feature_desc *f;
	const char * const *names;
	u8 arg = (u8)id, count;

	switch ((u8)(id >> 8)) {
	case MENU_ROW_SECTION: return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_SECTION, arg), row, 0, 0);
	case MENU_ROW_PAGE:
		if (dir != 0) return ST_OK;
#if QWARK_HAS_TOD
		/* Revision 1.29: where the game's own planet picker can open, Planets opens it. */
		if (arg == MENU_PAGE_PLANETS) {
			int rc = native_picker_act(row);

			if (rc >= 0) return rc;
		}
#endif
#if QWARK_PREVIEW
		/*
		 * The Mods page reads the console's library and its USB drives again
		 * as it opens, as MOD_RESCAN does, so a mod copied in by FTP or a
		 * drive put in since shows.
		 */
		if (arg == MENU_PAGE_MODS) {
			mods_rescan();
			modusb_scan(0);
		}
#endif
		return nav_push(MENU_PAGE_ID(arg, 0), row, 0, 0);
	case MENU_ROW_UCAT:    return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_UNLOCK_CAT, arg), row, 0, 0);
	case MENU_ROW_SCAT:    return dir != 0 ? ST_OK : nav_push(MENU_PAGE_ID(MENU_PAGE_SAVE_CAT, arg), row, 0, 0);

	case MENU_ROW_FEATURE:
		f = feature(arg);
		if (f == NULL) return ST_NOT_FOUND;
		return act_feature(id, f, dir);

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

#if QWARK_PREVIEW
	case MENU_ROW_MOD:
		return act_mod(id, arg, dir);
#endif

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
#define WHY_NO_HELPER    0x10

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

#if QWARK_HAS_TOD
/*
 * Revision 1.29 (pending): a game whose menu is its own UI (struct
 * menu_native, Tools of Destruction's pause menu), which has no renderer in
 * ui.pak: the session's g_mh is this one, made from the game's description,
 * with no cave and no hook. Its helper is the game's Lua helper
 * (lua_core_helper_ready), its mailbox header the core's to write
 * (native_install), its Help row a byte (help_sync), and its glue the
 * core's to send when the game asks (native_glue).
 */
static struct mh_desc g_native_mh;
static const struct menu_native *g_native;
static u32  g_native_loads;      /* the helper's loads word when the glue last went */
static u32  g_native_stopped;    /* the same, when a stop was last logged */
static u8   g_native_why;        /* 1: no helper (RPCS3 without the patch) */
static u32  g_native_note;       /* the glue's note last logged */
static u8   g_native_waited;     /* a long wait of the helper's logged, until it ends */
static u32  g_native_short;      /* the helper's shortage count last seen (revision 1.29, the second crash) */
static u32  g_native_short_loads; /* the loads word when a shortage was last logged, once a pause */
static u8   g_native_short_said; /* and whether one ever was */
static u8   g_native_diag;       /* the third console test: the helper's look to send (1) */
static u8   g_native_diag_done;  /* and the failures it went for once: bit0 no list, bit1 no Cheats screen */

/* Pause menu frames the helper waits for the presentation before the core says so (about five seconds). */
#define NATIVE_WAIT_LOG 300

/* Whether `game_id` has a menu of its own UI, before its module is in. */
static int native_game(u8 game_id)
{
	return game_id == GAME_TOD;
}
#endif

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
#if QWARK_HAS_TOD
	native_picker_drop();
	g_native = NULL;
	g_native_why = 0;
#endif
	mh_pak_close();
	g_posted = 0;
	g_hstate = MBOX_HSTATE_CLOSED;
	g_req = 0;
	g_help.in = -1;
	g_help.retry = 0;
	memset(&g_defer, 0, sizeof(g_defer));
}

int menu_none(const struct game_api *game)
{
	if (game == NULL || game->menu == NULL) return 1;
	return 0;
}

void menu_enter(const struct game_api *game)
{
	menu_enter_id(game != NULL ? game->game_id : GAME_NONE);
}

/*
 * Revision 1.26: the session's game going (game modules, PROTOCOL.md 9.11):
 * the model lets go of the game's tables and its unlock list, which a module
 * takes with it. The next view takes session_game() afresh, as every one does.
 */
void menu_detach(void)
{
#if QWARK_HAS_TOD
	/* Revision 1.29: the planet picker's bytes back while the game is still there. */
	native_picker_drop();
#endif
	g_game = NULL;
	g_ul.ok = 0;
	g_ul.list = NULL;
	g_ul.cats = NULL;
	g_ul.fields = NULL;
	g_ul.n = 0;
	g_ul.ncats = 0;
}

/* The game arrived after the session started: the page on show is built again. */
static int g_game_changed;

void menu_game_changed(void)
{
	g_game = session_game();
	g_ul.ok = 0;
	g_game_changed = 1;
}

void menu_enter_id(u8 game_id)
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
	if (game_id == GAME_NONE) return;
#if QWARK_HAS_TOD
	/*
	 * Revision 1.29: a menu that is the game's own UI needs no code of the
	 * menu's own, only the game's Lua helper, which RPCS3 takes as a patch,
	 * so it has a menu there too.
	 */
	if (!plat_can_patch_code() && !native_game(game_id)) return;
#else
	if (!plat_can_patch_code()) return;
#endif
	g_game_id = game_id;

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
#ifdef QWARK_TEST
		/* A slider's four words (the steppers, the tests' alone: no row the module builds has them). */
		if ((v->rows[i].flags & MROWF_RANGE) != 0) {
			u8 k;
			for (k = 0; k < 4; k++) be32_put(r + MROW_OFF_RANGE + 4u * k, v->rows[i].range[k]);
		}
#endif
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

#if QWARK_HAS_TOD
/*
 * Revision 1.29: a menu of the game's own UI goes in. Its helper first (a
 * console writes the game's Lua helper now if it has not; RPCS3 has it as a
 * patch or not at all), then the mailbox: taken over as it stands when it is
 * already this layout (the module loaded again into a running game), else the
 * helper's block zeroed and a header written, the identity last, so the helper
 * never reads half of one; then what install() does with any mailbox.
 */
/* Revision 1.29, the fourth console test: a screen's rows at once, row_cap for 0 or more. */
static u8 native_rows(u8 rows)
{
	return rows == 0 || rows > g_native->row_cap ? g_native->row_cap : rows;
}

static int native_install(void)
{
	const struct menu_native *n = g_native;
	u8 head[MBOX_HEAD_SIZE];
	u8 z[64];
	u32 off;
	int rc = lua_core_helper_ready();

	if (rc == ST_UNSUPPORTED) {
		if (!g_logged_refusal) {
			plat_log(plat_can_patch_code() ? "menu: this game's Lua helper cannot go in; no menu"
			                               : "menu: no Lua helper in the game (qwark's RPCS3 patch); no menu");
			g_logged_refusal = 1;
		}
		g_native_why = 1;
		return INSTALL_REFUSED;
	}
	if (rc != ST_OK) return rc;
	g_native_why = 0;

	if (mem_read(n->mailbox, head, MBOX_READ_HEAD) != ST_OK) return ST_IO_ERROR;
	if (be32_get(head + MBOX_OFF_MAGIC) != MBOX_MAGIC || be16_get(head + MBOX_OFF_VERSION) != MBOX_VERSION ||
	    be16_get(head + MBOX_OFF_SIZE) != n->mailbox_size || head[MBOX_OFF_ROW_CAP] != n->row_cap ||
	    head[MBOX_OFF_MAX_ROWS] != native_rows(n->max_rows) || head[MBOX_OFF_CFG_ROWS] != native_rows(n->cfg_rows) ||
	    head[MBOX_OFF_CAPS] != n->caps) {
		memset(z, 0, sizeof(z));
		for (off = 0; off < n->state_size; off += sizeof(z)) {
			u32 len = n->state_size - off < sizeof(z) ? n->state_size - off : (u32)sizeof(z);
			if (mem_write(n->state_addr + off, z, len) != ST_OK) return ST_IO_ERROR;
		}
		memset(head, 0, sizeof(head));
		be16_put(head + MBOX_OFF_VERSION, MBOX_VERSION);
		be16_put(head + MBOX_OFF_SIZE, n->mailbox_size);
		head[MBOX_OFF_ROW_CAP] = n->row_cap;
		head[MBOX_OFF_ROW_SIZE] = MBOX_ROW_SIZE;
		head[MBOX_OFF_MAX_ROWS] = native_rows(n->max_rows);
		head[MBOX_OFF_CAPS] = n->caps;
		head[MBOX_OFF_RENDERER] = g_game_id;
		head[MBOX_OFF_CFG_ROWS] = native_rows(n->cfg_rows);
		if (mem_write(n->mailbox + 4, head + 4, sizeof(head) - 4) != ST_OK) return ST_IO_ERROR;
		be32_put(head, MBOX_MAGIC);
		if (mem_write(n->mailbox, head, 4) != ST_OK) return ST_IO_ERROR;
		g_native_loads = 0;
		g_native_stopped = 0;
		g_native_note = 0;
		g_native_waited = 0;
		g_native_short = 0;
		g_native_short_loads = 0;
		g_native_short_said = 0;
		g_native_diag = 0;
		g_native_diag_done = 0;
	}

	/* Taken as install() takes any renderer's mailbox (its tail, the same words). */
	g_page_seq = (be32_get(head + MBOX_OFF_PAGE_SEQ) + 1u) & ~1u;
	g_last_ack = be32_get(head + MBOX_OFF_EV_SEQ);
	g_pick_seq = be32_get(head + MBOX_OFF_PICK_SEQ);
	if (be32_get(head + MBOX_OFF_EV_ACK) != g_last_ack &&
	    mem_write_u32(box(MBOX_OFF_EV_ACK), g_last_ack) != ST_OK)
		return ST_IO_ERROR;
	g_hstate = head[MBOX_OFF_HSTATE];
	g_frames = be32_get(head + MBOX_OFF_FRAMES);
	g_row_cap = head[MBOX_OFF_ROW_CAP] > MENU_VIEW_MAX ? MENU_VIEW_MAX : head[MBOX_OFF_ROW_CAP];
	g_row_size = head[MBOX_OFF_ROW_SIZE];
	menu_model_reset(head[MBOX_OFF_MAX_ROWS], head[MBOX_OFF_CFG_ROWS], head[MBOX_OFF_CAPS]);
	g_posted = 0;
	post(1);
	if (!g_posted) return ST_IO_ERROR;
	g_last_poll = now_tick();
	plat_log("menu: the game's own menu served for game %d, mailbox 0x%08X", (int)g_game_id, (int)n->mailbox);
	return ST_OK;
}

/*
 * Revision 1.29, the crash fix: what the helper and the glue say of a pause
 * menu, logged once each. The helper waits, the glue never entered, while
 * the game's own words say the pause's presentation is not up (lh_menu.c,
 * lm_ready): a wait of NATIVE_WAIT_LOG frames is said once, with its reasons.
 * The glue's note, each time it differs from the last one said: the Trainer
 * row is in, or it found no pause list or Cheats screen and leaves the
 * pause menu the game's. Since docs/tod-menu.md 14 the TRAINER row takes the
 * pause list's Inventory row, not Options' Help.
 *
 * Revision 1.29, the second crash (docs/tod-menu.md 10): the pause menu's Lua
 * pool short even after a full collection, which keeps the helper and the
 * glue out of the VM (and takes a glue that was in back out), said once a
 * pause with what was left; and what was left beside the glue's "in".
 *
 * Revision 1.29, the third console test (docs/tod-menu.md 11): the glue
 * finds the pause list and the Cheats screen apart, so it says more: the
 * list in with the Cheats screen not met yet, the Cheats screen met late,
 * and no Cheats screen when the Trainer went to its slide. After either
 * failure (no list, no Cheats screen) the helper's look (lh_diag.lua) is
 * sent once a session for each, and what it saw is logged.
 */
static void native_diag_want(u8 bit)
{
	if ((g_native_diag_done & bit) != 0) return;
	g_native_diag_done |= bit;
	g_native_diag = 1;
}

static void native_notes(void)
{
	const struct menu_native *n = g_native;
	u32 waits = 0, why = 0, note = 0, shortage = 0, room = 0, loads = 0;

	mem_read_u32(n->room_addr, &room);
	if (mem_read_u32(n->short_addr, &shortage) == ST_OK && shortage != g_native_short) {
		g_native_short = shortage;
		mem_read_u32(n->loads_addr, &loads);
		if (!g_native_short_said || loads != g_native_short_loads) {
			g_native_short_said = 1;
			g_native_short_loads = loads;
			plat_log("menu: the pause menu's Lua pool is short even after a collection (%u blocks left for small "
			         "objects, %u in its largest class); the Trainer stays out of it, Inventory is the game's",
			         (unsigned)(room & 0xFFFFu), (unsigned)(room >> 16));
		}
	}

	if (mem_read_u32(n->waits_addr, &waits) == ST_OK) {
		if (waits < NATIVE_WAIT_LOG) {
			g_native_waited = 0;
		} else if (!g_native_waited) {
			g_native_waited = 1;
			mem_read_u32(n->why_addr, &why);
			plat_log("menu: the pause menu is not ready for the Trainer after %u frames (why 0x%X); Inventory stays the game's",
			         (unsigned)waits, (unsigned)why);
		}
	}
	if (mem_read_u32(n->note_addr, &note) != ST_OK || note == 0) return;
	if (note == g_native_note) return;
	g_native_note = note;
	if (note == n->note_in) {
		plat_log("menu: the pause menu's glue is in: TRAINER in the pause list for Inventory (its Lua pool: %u blocks or more left "
		         "for small objects, %u in its largest class)", (unsigned)(room & 0xFFFFu), (unsigned)(room >> 16));
	} else if (note == n->note_in_list) {
		plat_log("menu: the pause menu's glue is in: TRAINER in the pause list for Inventory, Game Options met when the Trainer "
		         "shows a page there (its Lua pool: %u blocks or more left for small objects, %u in its largest class)",
		         (unsigned)(room & 0xFFFFu), (unsigned)(room >> 16));
	} else if (note == n->note_late) {
		plat_log("menu: the Trainer met the game's Game Options screen on its slide; its page is in");
	} else if (note == n->note_none) {
		plat_log("menu: the pause menu's glue found no pause list; Inventory stays the game's");
		native_diag_want(1);
	} else if (note == n->note_noch) {
		plat_log("menu: the Trainer found no Game Options screen on its slide and went back to the pause's list");
		native_diag_want(2);
	}
}

/*
 * The helper asks for the glue once per new pause menu VM (its loads word
 * moves and its glue word says wanted): sent once for that value, when the
 * Lua mailbox is free, and a glue that stopped in a VM is logged once.
 */
static void native_glue(void)
{
	const struct menu_native *n = g_native;
	u32 glue = 0, loads = 0;

	if (mem_read_u32(n->glue_addr, &glue) != ST_OK || mem_read_u32(n->loads_addr, &loads) != ST_OK) return;
	if (glue == n->glue_wanted && loads != g_native_loads) {
		int rc = lua_core_menu_glue();

		if (rc == ST_OK) {
			g_native_loads = loads;
		} else if (rc != ST_BUSY) {
			g_native_loads = loads;
			plat_log("menu: the pause menu's glue could not be sent (%d)", rc);
		}
	} else if (glue == n->glue_stopped && loads != g_native_stopped) {
		g_native_stopped = loads;
		plat_log("menu: the pause menu's glue stopped on an error and put Help back (LUA_STATUS has it)");
	} else if (g_native_diag) {
		int rc = lua_core_menu_diag();

		if (rc != ST_BUSY) g_native_diag = 0;
		if (rc != ST_OK && rc != ST_BUSY) plat_log("menu: the glue's look could not be sent (%d)", rc);
	}
	native_notes();
}
#endif

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

#if QWARK_HAS_TOD
	if (g_native != NULL) return native_install();
#endif

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

#if QWARK_PREVIEW
	/* Build 56: the renderer is qwark's own, for the Mods page's guarded load (mem.h), once however often it goes in. */
	mem_own_drop(g_menu_who);
	for (i = 0; i < d->ncaves; i++) mem_own(d->caves[i].addr, d->caves[i].len + d->caves[i].zeros, g_menu_who);
	for (i = 0; i < d->nhooks; i++) mem_own(d->hooks[i].addr, 4, g_menu_who);
#endif
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

#if QWARK_HAS_TOD
	/* A menu of the game's own UI has no hook: without the mailbox's identity the helper and the glue let it go. */
	if (g_native != NULL) {
		if (mem_write_u32(d->mailbox + MBOX_OFF_MAGIC, 0) != ST_OK) return ST_IO_ERROR;
		plat_log("menu: switched off, the game's own menu let go");
		return ST_OK;
	}
#endif

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
#if QWARK_HAS_TOD
	if (native_game(g_game_id)) {
		/* Its description is the game module's, there once the module is (revision 1.26). */
		const struct game_api *g = session_game();
		const struct menu_native *n = g != NULL && g->game_id == g_game_id ? g->menu_native : NULL;

		if (n == NULL) return 0;
		memset(&g_native_mh, 0, sizeof(g_native_mh));
		g_native_mh.game_id = g_game_id;
		g_native_mh.mailbox = n->mailbox;
		g_native_mh.mailbox_size = n->mailbox_size;
		g_native_mh.state = n->state_addr;
		g_native_mh.mode_addr = n->mode_addr;
		g_native_mh.mode_paused = n->mode_paused;
		g_native_mh.pending_addr = n->pending_addr;
		g_native_mh.pending_none = n->pending_none;
		g_native_mh.screen_addr = n->screen_addr;
		g_native_mh.screen_id = n->screen_id;
		g_native = n;
		g_mh = &g_native_mh;
		return 1;
	}
#endif
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
#if QWARK_HAS_TOD
	} else if (rc == ST_BUSY) {
		/* Revision 1.29: the game's Lua helper not looked for yet (RPCS3), a moment from now. */
		g_state = MENU_IDLE;
		g_install_at = plat_time_us() + 250000ull;
		return 0;
#endif
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
#if QWARK_HAS_TOD
	/*
	 * Revision 1.29, the fifth console test (docs/tod-menu.md 13): and for
	 * the game's own pause menu, the game back in play. Tools of
	 * Destruction's pause says it is gone (its state 0) at the end of the
	 * frame that tore it down, and only asks for play the frame after
	 * (GameState_requestGameplay), so for a frame or more nothing is queued
	 * and the game is still in its pause: a load run then was refused BUSY.
	 */
	if (g_native != NULL) {
		u32 screen = 0;

		if (mem_read_u32(d->screen_addr, &screen) != ST_OK) return 0;
		if (screen != g_native->play_state) return 0;
	}
#endif
	return mode == 0 && pending == d->pending_none;
}

#if QWARK_HAS_TOD
/*
 * Revision 1.29, the fifth console test: the game's own pause menu up, and
 * in it the glue that takes qwark's close (the pause's state its "up" one,
 * the game in its pause, the glue loaded in this pause's VM).
 */
static int native_paused(void)
{
	u32 mode = 0, screen = 0, glue = 0;

	if (g_native == NULL) return 0;
	if (mem_read_u32(g_native->mode_addr, &mode) != ST_OK ||
	    mem_read_u32(g_native->screen_addr, &screen) != ST_OK ||
	    mem_read_u32(g_native->glue_addr, &glue) != ST_OK)
		return 0;
	return mode == g_native->mode_paused && screen == g_native->screen_id && glue == g_native->glue_loaded;
}
#endif

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
#if QWARK_HAS_TOD
	/*
	 * A menu of the game's own UI: a byte, which its glue reads every frame
	 * and puts into the Help row the next time the pause menu's list shows,
	 * so it may change at any time.
	 */
	if (g_native != NULL) {
		if (mem_write_u8(g_native->help_addr, (u8)want) != ST_OK) return;
		if (g_help.in >= 0) plat_log("menu: the Help row is %s", want ? "the menu's again" : "the game's own");
		g_help.in = want;
		return;
	}
#endif
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

#if QWARK_HAS_TOD
/*
 * Revision 1.29 (pending): the game's own planet picker (struct menu_picker,
 * Tools of Destruction's galactic map; docs/tod-menu.md 16). The cross on the
 * root's Planets row is a CLOSES action (DEFER_PICKER): the game's menu
 * closes as for a load, and once the game plays the picker starts here:
 *
 *   SEND    the pokes written, each byte's own value kept first (the picker's
 *           table is copied, so they go back even if the game's module has
 *           gone), and the picker's chunk sent to the gameplay VM; a busy
 *           mailbox is tried again for a second
 *   WAIT    for the game to say the picker is up (screen_addr its state); its
 *           Lua failing or a few seconds without it is a failure
 *   UP      until it has gone; then every poked byte that still holds what
 *           was written is put back, and the pick read: a planet is the
 *           game's own load, already under way; none (Back) opens the menu
 *           again on its root, the cursor on Planets
 *   REOPEN  the open asked for until the menu is up, while the game plays
 *
 * Revision 1.29, every planet on the map (docs/tod-menu.md 17): while it is
 * UP, a picker with `glue` has the helper's map glue sent to the picker's
 * own VM (lua_core_map_send), again while it answers "wait", for a few
 * seconds: it lists the planets the game's picker leaves out and makes every
 * pick the core's. Should it not go in, the picker is the game's own still,
 * the console says so, and Back opens the page of planets instead of the
 * root, so every planet is a press away; that is the one time the page is
 * where Planets leads.
 *
 * A failure puts the bytes back and opens the menu again on the page of
 * planets, saying so on the console; so does a Planets row pressed where the
 * picker cannot open (no Lua helper, no level), without closing anything.
 */
#define GM_NONE    0
#define GM_SEND    1
#define GM_WAIT    2
#define GM_UP      3
#define GM_REOPEN  4
#define GM_BYTES   48
#define GM_SEND_US   1000000ull
#define GM_WAIT_US   4000000ull
#define GM_REOPEN_US 4000000ull
#define GM_ASK_US    750000ull
#define GM_GLUE_US   4000000ull    /* the map's glue: how long it may say "wait" */
#define GM_RESEND_US 100000ull     /* and how soon it is sent again after it did */
/* g_gm.glue */
#define GM_GLUE_NONE 0             /* none to send, or not sent yet */
#define GM_GLUE_OUT  1             /* out, its answer awaited */
#define GM_GLUE_IN   2
#define GM_GLUE_FAIL 3

static struct {
	u8   phase;
	u8   row;               /* the root's Planets row, for the cursor */
	u8   n;                 /* bytes poked, to put back */
	u8   want_glue;         /* the picker's glue is to go in once it is up (struct menu_picker's glue) */
	u8   glue;              /* GM_GLUE_* */
	u32  seq;               /* the chunk's request, then the glue's */
	u64  glue_until;        /* the glue may say "wait" until then */
	u32  state;             /* the picker's, copied: screen_addr's value while it is up */
	u32  pick_addr;
	u64  until;
	u64  ask;               /* REOPEN: the next open asked for no sooner */
	char name[24];
	u32  addr[GM_BYTES];
	u8   saved[GM_BYTES];
	u8   value[GM_BYTES];
} g_gm;

static const struct menu_picker *picker(void)
{
	return g_native != NULL ? g_native->picker : NULL;
}

/* Every poked byte that still holds what was written, back to what it was. */
static void native_picker_restore(void)
{
	u8 i, b;

	for (i = 0; i < g_gm.n; i++) {
		if (mem_read_u8(g_gm.addr[i], &b) == ST_OK && b == g_gm.value[i]) mem_write_u8(g_gm.addr[i], g_gm.saved[i]);
	}
	if (g_gm.n != 0) plat_log("menu: %s gone; the planets' unlocks are the save's again", g_gm.name);
	g_gm.n = 0;
}

/* The session or the game's module going: the bytes back, nothing waiting. */
static void native_picker_drop(void)
{
	native_picker_restore();
	memset(&g_gm, 0, sizeof(g_gm));
}

/* The menu opened again (REOPEN): on the root with the cursor on Planets, or on the page of planets. */
static void native_picker_reopen(int list)
{
	menu_model_home();
	if (list) nav_push(MENU_PAGE_ID(MENU_PAGE_PLANETS, 0), g_gm.row, 0, 0);
	else g_nav[0].cursor = g_gm.row;
	g_gm.phase = GM_REOPEN;
	g_gm.until = plat_time_us() + GM_REOPEN_US;
	g_gm.ask = 0;
}

static void native_picker_fail(const char *why)
{
	native_picker_restore();
	plat_log("menu: %s did not open (%s); the Planets page instead", g_gm.name, why);
	tell(g_gm.name, " did not open", "; the planets instead");
	native_picker_reopen(1);
}

/*
 * The Planets row's cross: -1 for the page of planets as for every game, or
 * the picker's CLOSES action.
 */
static int native_picker_act(u8 row)
{
	const struct menu_picker *p = picker();
	int rc;

	if (p == NULL) return -1;
	rc = g_gm.phase != GM_NONE ? ST_BUSY : lua_core_gameplay_ready();
	if (rc != ST_OK) {
		plat_log("menu: %s cannot open here (%s); the Planets page instead", p->name, status_text((u8)rc));
		tell(p->name, " cannot open here", "; the planets instead");
		return -1;
	}
	g_gm.row = row;
	return defer(DEFER_PICKER, row, p->name);
}

/* DEFER_PICKER, once the menu has closed and the game plays: the picker's table taken, the bytes poked. */
static int native_picker_start(void)
{
	const struct menu_picker *p = picker();
	u8 i, k, row = g_defer.arg;

	if (p == NULL) return ST_UNSUPPORTED;
	native_picker_restore();
	memset(&g_gm, 0, sizeof(g_gm));
	qstrcpy(g_gm.name, sizeof(g_gm.name), p->name);
	g_gm.row = row;
	g_gm.state = p->state;
	g_gm.pick_addr = p->pick_addr;
	g_gm.want_glue = p->glue;
	for (i = 0; i < p->npokes; i++) {
		for (k = 0; k < p->pokes[i].count && g_gm.n < GM_BYTES; k++, g_gm.n++) {
			g_gm.addr[g_gm.n] = p->pokes[i].addr + (u32)k * p->pokes[i].stride;
			g_gm.value[g_gm.n] = p->pokes[i].value;
		}
	}
	/* All read before any is written; from here on n counts the bytes that may need putting back. */
	k = g_gm.n;
	g_gm.n = 0;
	for (i = 0; i < k; i++) {
		if (mem_read_u8(g_gm.addr[i], &g_gm.saved[i]) != ST_OK) return ST_IO_ERROR;
	}
	for (i = 0; i < k; i++) {
		g_gm.n = (u8)(i + 1);
		if (g_gm.saved[i] != g_gm.value[i] && mem_write_u8(g_gm.addr[i], g_gm.value[i]) != ST_OK) {
			native_picker_restore();
			return ST_IO_ERROR;
		}
	}
	g_gm.phase = GM_SEND;
	g_gm.until = plat_time_us() + GM_SEND_US;
	return ST_OK;
}

/*
 * Revision 1.29, every planet on the map (docs/tod-menu.md 17): while the
 * picker is up, its glue into its own VM (the helper's lh_gmap.lua): sent,
 * its answer awaited, sent again while it says "wait" (the map's handler and
 * list not up yet), for GM_GLUE_US. "in<rows>" is in; anything else, or
 * the time up, is a failure: logged and said on the console once, and Back
 * then opens the page of planets (GM_UP).
 */
static void native_picker_glue_fail(const char *why)
{
	g_gm.glue = GM_GLUE_FAIL;
	plat_log("menu: %s could not list every planet (%s); Back opens the Planets page", g_gm.name, why);
	tell(g_gm.name, " lists the save's planets only", "; Back for every planet");
}

static void native_picker_glue(u64 now)
{
	const char *out;
	int rc;

	if (!g_gm.want_glue || g_gm.glue == GM_GLUE_IN || g_gm.glue == GM_GLUE_FAIL) return;
	if (g_gm.glue == GM_GLUE_OUT) {
		rc = lua_core_gameplay_result(g_gm.seq);
		if (rc == 0) return;
		out = lua_core_own_out(g_gm.seq);
		if (rc == 1 && out[0] == 'i' && out[1] == 'n') {
			g_gm.glue = GM_GLUE_IN;
			plat_log("menu: %s lists every planet (%s rows)", g_gm.name, out[2] != 0 ? out + 2 : "its");
			return;
		}
		if (rc != 1 || strcmp(out, "wait") != 0) {
			native_picker_glue_fail(out[0] != 0 ? out : "its glue did not run");
			return;
		}
		g_gm.glue = GM_GLUE_NONE;
		g_gm.ask = now + GM_RESEND_US;
	}
	if (now >= g_gm.glue_until) {
		native_picker_glue_fail("its list never came up");
		return;
	}
	if (now < g_gm.ask) return;
	rc = lua_core_map_send(&g_gm.seq);
	if (rc == ST_OK) {
		g_gm.glue = GM_GLUE_OUT;
	} else if (rc != ST_BUSY && rc != ST_NOT_FOUND) {
		native_picker_glue_fail(status_text((u8)rc));
	}
}

static void native_picker_tick(void)
{
	const struct menu_picker *p;
	u32 screen = 0, pick = 0xFFFFFFFFu;
	u64 now = plat_time_us();
	int rc;

	switch (g_gm.phase) {
	case GM_SEND:
		p = picker();
		if (p == NULL) {
			native_picker_drop();
			return;
		}
		rc = lua_core_gameplay_send(p->chunk, (u32)strlen(p->chunk), &g_gm.seq);
		if (rc == ST_OK) {
			g_gm.phase = GM_WAIT;
			g_gm.until = now + GM_WAIT_US;
		} else if (rc != ST_BUSY || now >= g_gm.until) {
			native_picker_fail(rc == ST_BUSY ? "the game's Lua was busy" : status_text((u8)rc));
		}
		return;

	case GM_WAIT:
		if (mem_read_u32(g_native->screen_addr, &screen) != ST_OK) return;
		if (screen == g_gm.state) {
			g_gm.phase = GM_UP;
			g_gm.glue = GM_GLUE_NONE;
			g_gm.seq = 0;
			g_gm.ask = 0;
			g_gm.glue_until = now + GM_GLUE_US;
			plat_log("menu: %s is up", g_gm.name);
			/* Revision 1.29 (pending): a pick on it is qwark's load (struct menu_picker's up). */
			p = picker();
			if (p != NULL && p->up != NULL) p->up();
		} else if (lua_core_gameplay_result(g_gm.seq) == 2) {
			native_picker_fail("its Lua did not run");
		} else if (now >= g_gm.until) {
			native_picker_fail("the game never showed it");
		}
		return;

	case GM_UP:
		if (mem_read_u32(g_native->screen_addr, &screen) != ST_OK) return;
		if (screen == g_gm.state) {
			native_picker_glue(now);
			return;
		}
		native_picker_restore();
		if (mem_read_u32(g_gm.pick_addr, &pick) == ST_OK && (s32)pick >= 0) {
			const char * const *names = NULL;
			u8 count = 0;

			if (g_game != NULL && g_game->planet_names != NULL) names = g_game->planet_names(&count);
			plat_log("menu: %s picked %s; the game loads it", g_gm.name,
			         names != NULL && pick < count && names[pick] != NULL ? names[pick] : "a planet");
			g_gm.phase = GM_NONE;
			return;
		}
		if (screen != g_native->play_state) {
			/* Gone some other way than back to play: nothing to open again. */
			g_gm.phase = GM_NONE;
			return;
		}
		plat_log("menu: %s closed without a pick; the Trainer again%s", g_gm.name,
		         g_gm.glue == GM_GLUE_FAIL ? ", on the Planets page" : "");
		native_picker_reopen(g_gm.glue == GM_GLUE_FAIL);
		return;

	case GM_REOPEN:
		if (g_hstate != MBOX_HSTATE_CLOSED) {
			g_gm.phase = GM_NONE;
			return;
		}
		if (now >= g_gm.until) {
			plat_log("menu: the Trainer did not open again after %s", g_gm.name);
			g_gm.phase = GM_NONE;
			return;
		}
		/* Asked again only while the game plays with no pause coming (one nobody took is withdrawn). */
		if (g_req == 0 && now >= g_gm.ask && game_playing()) {
			post(1);
			request(MBOX_REQ_OPEN);
			g_gm.ask = now + GM_ASK_US;
		}
		return;

	default:
		return;
	}
}
#endif

/* A CLOSES action, now. */
static int run_deferred(void)
{
	switch (g_defer.kind) {
	case DEFER_FEATURE: return features_trigger(g_defer.arg);
#if QWARK_HAS_TOD
	case DEFER_PLANET:  return session_planet_load(g_defer.arg, g_defer.flags);
#else
	case DEFER_PLANET:  return session_planet_load(g_defer.arg, 0xFF);
#endif
	case DEFER_RESTORE: return savefile_restore(g_defer.cat, g_defer.name);
#if QWARK_HAS_TOD
	case DEFER_PICKER:  return native_picker_start();
#endif
	default:            return ST_OK;
	}
}

#if QWARK_HAS_TOD
/* Revision 1.29: a waiting CLOSES action given up, and why, on the console and in the log. */
static void defer_cancel(const char *why)
{
	plat_log("menu: %s was cancelled: %s", g_defer.label, why);
	tell(g_defer.label, " was cancelled: ", why);
	g_defer.kind = DEFER_NONE;
	if (g_req == MBOX_REQ_CLOSE) {
		mem_write_u8(box(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
		g_req = 0;
	}
}

/*
 * Revision 1.29, the fifth console test (docs/tod-menu.md 13): a CLOSES
 * action waiting on the game's own pause menu. The glue closes the pause as
 * the game's own Quit does (exit_pause_menu) when it takes qwark's close;
 * the action runs once the game is back in play (game_playing), never while
 * the pause is still being torn down. Meanwhile:
 *
 *   - the menu seen closed and then up again: the player went on in the
 *     pause (a pause the close did not reach), so the action is cancelled;
 *   - the game neither in its pause nor in play (quit to the title, a load
 *     of the game's own): cancelled;
 *   - the pause still up with no close waiting (one nobody took is withdrawn
 *     after half a second, and one the client's load asked for comes with
 *     the menu down): asked again;
 *   - the pause gone with a close still waiting: withdrawn, so it never
 *     closes the next pause.
 *
 * Answers 1 when it cancelled.
 */
static int native_defer_tick(void)
{
	u32 screen = 0;

	if (g_hstate == MBOX_HSTATE_CLOSED) {
		g_defer.closed = 1;
	} else if (g_defer.closed) {
		defer_cancel("the menu came up again");
		return 1;
	}
	if (mem_read_u32(g_native->screen_addr, &screen) == ST_OK &&
	    screen != g_native->screen_id && screen != g_native->play_state) {
		defer_cancel("the game left its pause another way");
		return 1;
	}
	if (native_paused()) {
		if (g_req == 0) request(MBOX_REQ_CLOSE);
	} else if (g_req == MBOX_REQ_CLOSE) {
		mem_write_u8(box(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
		g_req = 0;
	}
	return 0;
}

/*
 * Revision 1.29, the fifth console test: a planet the client asked for while
 * Tools of Destruction is in its pause, which no load can start from. Taken
 * as the menu's own Planets rows take one: the pause closed by the glue, the
 * planet loaded once the game plays (deferred_tick). ST_OK when it is taken
 * so; ST_UNSUPPORTED when it is not for this (no game pause menu of the
 * glue's up), and the game answers as before. Tick thread.
 */
int menu_pause_defer(u8 planet, u8 flags)
{
	const char * const *names;
	u8 count = 0;

	if (g_native == NULL || g_state != MENU_READY || (g_caps & MBOX_CAP_REQUEST) == 0 || !native_paused())
		return ST_UNSUPPORTED;
	if (g_game == NULL || g_game->planet_names == NULL || g_game->planet_load == NULL) return ST_UNSUPPORTED;
	names = g_game->planet_names(&count);
	if (planet >= count) return ST_UNSUPPORTED;
	defer(DEFER_PLANET, planet, names[planet]);
	g_defer.flags = flags;
	plat_log("menu: the pause is up; %s loads once it has closed", names[planet] != NULL ? names[planet] : "the planet");
	return ST_OK;
}
#endif

/*
 * A CLOSES action (a load: the set-aside file, a planet, a save of the
 * library): asked for, the menu asked to close; run once it has and the game
 * plays again, or dropped five seconds on. Revision 1.29: in the game's own
 * pause menu, native_defer_tick too.
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
#if QWARK_HAS_TOD
		else if (native_paused()) request(MBOX_REQ_CLOSE);
#endif
		return;
	}

	if (g_hstate == MBOX_HSTATE_CLOSED && game_playing()) {
		rc = run_deferred();
		if (rc != ST_OK) tell(g_defer.label, ": ", status_text((u8)rc));
		g_defer.kind = DEFER_NONE;
		return;
	}

#if QWARK_HAS_TOD
	if (g_native != NULL && native_defer_tick()) return;
#endif

	if (plat_time_us() >= g_defer.until) {
		tell(g_defer.label, " was dropped, the game did not go back to play", "");
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
#if QWARK_HAS_TOD
	g_defer.flags = 0xFF;
	g_defer.closed = 0;
#endif
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

	/* Revision 1.26: a game module arrived, with the game's own pages. */
	if (g_game_changed) {
		g_game_changed = 0;
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

#if QWARK_HAS_TOD
	if (g_native != NULL) {
		native_glue();
		native_picker_tick();
	}
#endif

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
#if QWARK_HAS_TOD
		if (g_native != NULL && g_native_why == 1)
			unavailable(WHY_NO_HELPER, plat_can_patch_code() ? "Trainer menu: the game's Lua helper cannot go in"
			                                                 : "Trainer menu: needs qwark's RPCS3 patch (Lua helper)");
		else
#endif
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
