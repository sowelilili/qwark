/*
 * The console menu in Ratchet & Clank (revision 1.22, mailbox version 2), on
 * qwark's fake console, against the real core: three layers, as
 * test_menu_uya.c has them for UYA.
 *
 *   the model     RaC1's pages, built from its menu table (src/games/rac1.c):
 *                 the root, Game, Cheats, Planets in the game's order, Manips,
 *                 Unlocks with the gold bolt and skill point buttons, and the
 *                 unlock values Locked, Unlocked and Gold in the game's title
 *                 case;
 *   the install   the four words a few seconds into the session in play, the
 *                 seven caves, the refusals, menu.enabled = 0 and the word that
 *                 says one of the menu's pages is up;
 *   the renderer  the shared core and RaC1's shim (mh_rac1.c, mh_rac1_cfg.c)
 *                 compiled for the PC (MH_HOST), against a model of the game's
 *                 pause-menu framework: pages of fourteen items, the current
 *                 and the next page, a transition of twelve frames, the close
 *                 of ten, the panes and their poses, the generic list's update,
 *                 the title and list draws, the main page with its Help entry,
 *                 the opener and its reopen fields. The hook stubs are
 *                 assembler and do not run here: src/games/menuhelper/check.py
 *                 steps them, and the model does in C what they do.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/mem.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/manips.h"
#include "../src/core/savefile.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/games/rac1.h"
#include "../src/games/menuhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- the renderer */

#define MH_HOST 1

static u8 *mh_host_ptr(u32 addr)
{
	return host_mem_ptr(addr);
}

#include "../src/games/menuhelper/mh_rac1.h"
#include "../src/games/menuhelper/mh_common.h"
#include "../src/games/menuhelper/mh_shim.h"

/* Where the fake game keeps what the model needs. */
#define PANE0        0x00E00000u      /* the fourteen pane mobys, 0x100 apart */
#define PANE_CLASS   0x00E10000u      /* their class: +0x48 + 4 * pose -> the pose */
#define POSE         0x00E11000u      /* every pose: 6 frames */
#define MSG_AT       0x00A80000u      /* the strings get_ui_string answers for the game's ids */
#define MAIN_ITEM0   0x00729BB0u      /* the main page's seven one-row items */
#define MAIN_ENTRY0  0x00729B08u      /* and their entries */
#define HELP_ITEM    (MAIN_ITEM0 + 4 * 0x50)

/*
 * The game's pause as far as the renderer can see it: the frames a transition
 * and a close have left, the sounds, the poses set, and what the draws drew.
 */
#define NDRAW 24
static struct {
	int trans;
	u32 trans_to;
	int closing;
	int sounds, sound;
	int anims;
	int help_opened;            /* the game's own Help page was reached */
	/* the last frame's draws */
	char pane_text[14][80];     /* what each pane's generic list or title drew */
	u32 pane_flags[14];
	int ndraw;
	char drawn[NDRAW][80];    /* the CONFIG list's: label, value, label, ... */
	float draw_x[NDRAW], draw_y[NDRAW];
	u32 draw_colour[NDRAW];
	int boxes, sprites, blends;
	int list_h;                 /* the CONFIG list pane's height */
} G;

static void put_str(u32 a, const char *s)
{
	u32 i;
	for (i = 0; i <= strlen(s); i++) w8(a + i, (u8)s[i]);
}

static const char *read_str(u32 a, char *out, u32 cap)
{
	u32 n = 0;
	while (a != 0 && n + 1 < cap) {
		u8 c = (u8)r8(a + n);
		if (c == 0) break;
		out[n++] = (char)c;
	}
	out[n] = 0;
	return out;
}

/* get_ui_string with the text hook in: what mh_text_stub answers (check.py steps the real one). */
static u32 ui_string(u32 id)
{
	if (id - R1_ID_BASE < R1_ID_COUNT && r32(MH_SHIM + SH_STRTAB + 4 * (id - R1_ID_BASE)) != 0)
		return r32(MH_SHIM + SH_STRTAB + 4 * (id - R1_ID_BASE));
	if (id == 0x4EE6) return MSG_AT + 0x10;          /* "Help" */
	if (id == 0x4F42) return MSG_AT + 0x20;          /* "Camera Options" */
	return MSG_AT;                                   /* "Weapons" etc.: one string will do */
}

static void mock_play_sound(int id, int a, u32 moby) { (void)a; (void)moby; G.sound = id; G.sounds++; }
static void mock_anim_set(u32 moby, int pose, int frame) { (void)moby; (void)pose; (void)frame; G.anims++; }
static unsigned long long mock_blend(int a, int b, int c, int d, int e, int f, int g, int h)
{
	(void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
	G.blends++;
	return 0x44ull;
}
static void mock_gs_reg(int n, unsigned long long v) { (void)n; (void)v; }
static void mock_draw_box(int x0, int y0, int x1, int y1, u32 c, int z) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; (void)z; G.boxes++; }
static int mock_sprite_index(int g, int i) { (void)g; return 100 + i; }
static void mock_draw_sprite(int t, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, int a)
{
	(void)t; (void)x0; (void)y0; (void)x1; (void)y1; (void)u0; (void)v0; (void)u1; (void)v1; (void)a;
	G.sprites++;
}

static void drew(u32 s, float x, float y, u32 colour)
{
	if (G.ndraw >= NDRAW) return;
	read_str(s, G.drawn[G.ndraw], 80);
	G.draw_x[G.ndraw] = x;
	G.draw_y[G.ndraw] = y;
	G.draw_colour[G.ndraw] = colour;
	G.ndraw++;
}

static void mock_draw_text(float x, float y, u32 colour, u32 s, int len, float scale)
{
	(void)len; (void)scale;
	drew(s, x, y, colour);
}

static void mock_draw_right(int x, int y, u32 colour, u32 s, int len, float scale)
{
	(void)len; (void)scale;
	drew(s, (float)x, (float)y, colour);
}

/*
 * The generic list's update (0x141408) for a one-row item: only the focused
 * item reacts; Start, Select and R3 close (-1); Triangle goes to the page's
 * parent; the cross follows an entry of kind 3 to its page; Up and Down at the
 * row's ends ask for the focus to move to the items the item's up and down
 * words name (page +0x80, which the pause applies after every item's update).
 */
static int mock_list_update(u32 item)
{
	u32 page = r32(R1_MENU_PAGE), e = r32(R1_PAD_EDGES), ent = r32(item + 0x34), to;

	if (page == 0 || r32(page + 0x40) != item) return 0;
	if ((e & PADB_CLOSE) != 0) return -1;
	if ((e & PADB_TRIANGLE) != 0) {
		if (r32(page + 0x38) == 0) return -1;
		w32(R1_MENU_NEXT, r32(page + 0x38));
		return 0;
	}
	if ((e & 0x40) != 0) {
		if (r16(ent + 2) == 3) {
			to = r32(ent + 4);
			if (to == MH_ORIG_HELP_PAGE) G.help_opened++;
			else w32(R1_MENU_NEXT, to);
		}
		return 0;
	}
	if ((e & PADB_UP) != 0 && r32(item + 0x38) != 0) {
		w32(page + 0x80, r32(item + 0x38));
		mock_play_sound(1, 17, 0);
	}
	if ((e & PADB_DOWN) != 0 && r32(item + 0x3C) != 0) {
		w32(page + 0x80, r32(item + 0x3C));
		mock_play_sound(1, 17, 0);
	}
	return 0;
}

#include "../src/games/menuhelper/mh_core.c"
#include "../src/games/menuhelper/mh_rac1.c"
#include "../src/games/menuhelper/mh_rac1_cfg.c"

/* ----------------------------------------------------- the game's pause */

/* The game's own main page, as the executable lays it out (0x729A80), and the panes. */
static void seed_game(void)
{
	static const u16 texts[7] = { 0x4EE2, 0x4EE3, 0x4EE4, 0x4EE5, 0x4EE6, 0x4EE7, 0x4FC2 };
	u32 k, item, ent;

	for (k = 0; k < 14; k++) {
		w32(R1_MAIN_PAGE + 4 * k, k < 7 ? 0x0E + k : k);
		w32(PANE0 + 0x100 * k + R1_MOBY_CLASS, PANE_CLASS);
	}
	for (k = 0; k < 0x200; k += 4) w32(PANE_CLASS + 0x48 + k, POSE);
	w8(POSE + R1_POSE_FRAMES, 6);
	w32(R1_MAIN_PAGE + 0x38, 0);
	w32(R1_MAIN_PAGE + 0x3C, 2);
	w32(R1_MAIN_PAGE + 0x40, MAIN_ITEM0);
	for (k = 0; k < 7; k++) {
		item = MAIN_ITEM0 + 0x50 * k;
		ent = MAIN_ENTRY0 + 0x18 * k;
		w32(R1_MAIN_PAGE + 0x44 + 4 * k, item);
		w32(item, OPD_LIST_UPDATE);
		w32(item + 4, OPD_LIST_DRAW);
		w32(item + 0x30, 0x84);
		w32(item + 0x34, ent);
		w32(item + 0x38, k > 0 ? item - 0x50 : 0);
		w32(item + 0x3C, k < 6 ? item + 0x50 : 0);
		if (k != 4) {             /* the Help entry's two words are the install's */
			w16(ent, texts[k]);
			w16(ent + 2, 3);
			w32(ent + 4, 0x00720000u + k);
		}
	}
	put_str(MSG_AT, "Weapons");
	put_str(MSG_AT + 0x10, "Help");
	put_str(MSG_AT + 0x20, "Camera Options");
	w32(R1_CONFIRM_MASK, 0x40);
	w32(R1_FRAMES, 100);
	w32(R1_HP, 8);
	w32(R1_LANGUAGE, 0);
}

/* A page arrived on: current, its screen, each item's pane. */
static void arrive_on(u32 page)
{
	u32 k, item;

	w32(R1_MENU_PAGE, page);
	w32(R1_MENU_NEXT, 0);
	w32(0x00A52BB0, r32(page + 0x3C));
	for (k = 0; k < 14; k++) {
		item = r32(page + 0x44 + 4 * k);
		if (item != 0) w32(item + 0x14, PANE0 + 0x100 * k);
	}
}

/* The opener (0x13B108): the pause on `page`, its fourteen panes spawned. */
static void open_pause(u32 page)
{
	u32 k;

	w32(R1_STATE, R1_STATE_PAUSE);
	for (k = 0; k < 14; k++) w32(R1_PANES + 4 * k, PANE0 + 0x100 * k);
	G.trans = 0;
	G.closing = 0;
	w32(R1_MENU_CLOSE, 0);
	arrive_on(page);
}

static int call_update(u32 tok, u32 item)
{
	if (tok == MH_FN_ROW) return mh_row_update(item);
	if (tok == MH_FN_CONF) return mh_conf_update(item);
	if (tok == OPD_LIST_UPDATE) return mock_list_update(item);
	return 0;
}

/* The render: each item's rect from its pane, then its draw. */
static void render(u32 page)
{
	u32 k, item, tok;

	G.ndraw = 0;
	for (k = 0; k < 14; k++) {
		G.pane_text[k][0] = 0;
		item = r32(page + 0x44 + 4 * k);
		if (item == 0 || item == R1_EMPTY_ITEM || item == R1_TOGGLE_HINT) continue;
		/* one-row panes 180 x 36; the CONFIG list's large pane 440 x 250 */
		w32(item + 0x20, k == 3 && page != PAGE_LIST ? 440 : 180);
		w32(item + 0x24, k == 3 && page != PAGE_LIST ? (u32)G.list_h : 36);
		tok = r32(item + 4);
		if (tok == MH_FN_DRAW) {
			mh_conf_draw(item);
		} else if (tok == OPD_LIST_DRAW) {
			read_str(ui_string(r16(r32(item + 0x34))), G.pane_text[k], 80);
			G.pane_flags[k] = r32(item + 0x30);
		} else if (tok == OPD_LABEL_DRAW) {
			read_str(ui_string(r32(item + 0x34)), G.pane_text[k], 80);
		}
	}
}

/*
 * One frame of the pause: every item of the current page updated (any non-zero
 * answer closes, ten frames on), then the next page if one was asked for (the
 * transition: twelve frames with no current page), then the draws.
 */
static void pause_frame(void)
{
	u32 page = r32(R1_MENU_PAGE), k, item, rc;

	if (G.closing) {
		if (--G.closing == 0) {
			w32(R1_STATE, 0);
			w32(R1_MENU_PAGE, 0);
			w32(R1_MENU_CLOSE, 0);
			w32(0x00A52BB0, 20);
		}
		return;
	}
	if (G.trans) {
		if (--G.trans == 0) arrive_on(G.trans_to);
		return;
	}
	for (k = 0; k < 14 && page != 0; k++) {
		item = r32(page + 0x44 + 4 * k);
		if (item == 0) continue;
		rc = (u32)call_update(r32(item), item);
		if (rc != 0 && r32(R1_MENU_CLOSE) == 0) w32(R1_MENU_CLOSE, 1);
	}
	if (page != 0 && r32(page + 0x80) != 0) {
		w32(page + 0x40, r32(page + 0x80));
		w32(page + 0x80, 0);
	}
	if (r32(R1_MENU_CLOSE) != 0) {
		G.closing = 10;
		return;
	}
	if (r32(R1_MENU_NEXT) != 0) {
		G.trans = 12;
		G.trans_to = r32(R1_MENU_NEXT);
		w32(R1_MENU_PAGE, 0);
		return;
	}
	render(page);
}

/*
 * One frame of the game: in play, the play update (the reopen fields, or the
 * player's Start, which opens the main page) and then the play hook; in the
 * pause, the pause's frame. `edges` are this frame's presses.
 */
static void frame(u32 edges)
{
	w32(R1_PAD_EDGES, edges);
	if (r32(R1_STATE) == 0) {
		if (r32(R1_RESUME_PAGE) != 0) {
			open_pause(r32(R1_RESUME_PAGE));
			w32(R1_RESUME_PAGE, 0);
		} else if ((edges & 0x800) != 0) {
			open_pause(R1_MAIN_PAGE);
		}
		mh_play();
	} else {
		pause_frame();
	}
	w32(R1_PAD_EDGES, 0);
}

static void frames(int n)
{
	while (n-- > 0) frame(0);
}

/* ------------------------------------------------------------- helpers */

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot(const char *title)
{
	int i;

	host_boot(title);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME) return 1;
	}
	return 0;
}

static int quit(void)
{
	int i;

	host_quit();
	for (i = 0; i < 2000; i++) {
		tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

static u32 peek32(u32 a) { u8 b[4]; host_peek(a, b, 4); return be32_get(b); }
static u8 peek8(u32 a) { u8 b = 0; host_peek(a, &b, 1); return b; }
static void poke32(u32 a, u32 v) { u8 b[4]; be32_put(b, v); host_poke(a, b, 4); }
static void poke8(u32 a, u8 v) { host_poke(a, &v, 1); }

static void press(u32 mask)
{
	host_set_pad(0);
	tick(2);
	host_set_pad(mask);
	tick(2);
	host_set_pad(0);
	tick(2);
}

static int hooks_are(const struct mh_desc *d, int ours)
{
	u8 i;
	for (i = 0; i < d->nhooks; i++) {
		u32 want = ours ? d->hooks[i].value : d->hooks[i].original;
		if (peek32(d->hooks[i].addr) != want) return 0;
	}
	return 1;
}

static const struct menu_row *view_row(const struct menu_view *v, const char *label)
{
	u8 i;
	for (i = 0; i < v->nrows; i++) {
		if (strcmp(v->rows[i].label, label) == 0) return &v->rows[i];
	}
	return NULL;
}

static int view_index(const struct menu_view *v, const char *label)
{
	u8 i;
	for (i = 0; i < v->nrows; i++) {
		if (strcmp(v->rows[i].label, label) == 0) return i;
	}
	return -1;
}

static int act(struct menu_view *v, u8 kind, const char *label)
{
	int i = view_index(v, label), rc;

	if (i < 0) return -1;
	rc = menu_model_event(kind, (u8)i, v->rows[i].id, v->key);
	menu_model_view(v);
	return rc;
}

static const char *value_of(const struct menu_view *v, const char *label)
{
	const struct menu_row *r = view_row(v, label);
	return r != NULL ? r->value : "(no row)";
}

static int labels_are(const struct menu_view *v, const char * const *want, u8 n)
{
	u8 i;
	if (v->nrows != n) return 0;
	for (i = 0; i < n; i++) {
		if (strcmp(v->rows[i].label, want[i]) != 0) return 0;
	}
	return 1;
}

#define CAPS_ALL (MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST)

/* ------------------------------------------------------------- the model */

static void test_model_rac1(void)
{
	static const char * const root[] = { "Game", "Cheats", "Planets", "Manips", "Unlocks", "Save files",
	                                     "qwark build 54" };
	static const char * const game[] = { "Die", "Position slot", "Save position", "Load position",
	                                     "Set aside file", "Load set-aside file", "Force autosave", "Bolts" };
	static const char * const cheats[] = { "Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health",
	                                       "Goodies menu" };
	static const char * const manips[] = { "Category", "Auto-reset", "Remember", "Apply now" };
	static const char * const unlocks[] = { "Weapons", "Gadgets", "Items", "Collectables" };
	static const char * const collectables[] = { "Unlock all gold bolts", "Reset all gold bolts",
	                                             "Unlock all skill points", "Reset skill points" };
	struct menu_view v;
	const struct menu_row *r;
	u8 i;

	group("console menu: the model, RaC1's pages");

	check(boot("NPEA00385"), "RaC1 boots");
	menu_model_reset(7, 16, CAPS_ALL);
	menu_model_view(&v);
	check(labels_are(&v, root, 7) && v.ptype == MBOX_PTYPE_MAIN && v.rows[6].kind == MROW_INFO &&
	      (v.rows[6].flags & MROWF_ENABLED) == 0 && strcmp(v.title, "Trainer") == 0,
	      "the root, Trainer: Game, Cheats, Planets, Manips, Unlocks, Save files, and the build, grey, in one "
	      "window of seven");

	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(labels_are(&v, game, 8) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Game: a CONFIG page, the quick block, Force autosave and Bolts (the jankpot and debug rows left out)");
	r = view_row(&v, "Bolts");
	check(r != NULL && r->kind == MROW_VALUE, "Bolts is a stepper");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC1_BOLTS) == 1, "and Right adds one");
	r = view_row(&v, "Load set-aside file");
	check(r != NULL && (r->flags & MROWF_CLOSES) != 0, "Load set-aside file closes the menu first");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Cheats");
	check(labels_are(&v, cheats, 5) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Cheats: the four cheats every game has, then the Goodies menu");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0,
	      "Ghost Ratchet: the cross turns it On");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	r = view_row(&v, "Goodies menu");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0,
	      "the Goodies menu, a LIVE toggle, is Off or On");
	check(act(&v, MBOX_EV_ACTIVATE, "Goodies menu") == ST_OK && peek8(RAC1_GOODIES_MENU) == 1,
	      "and the cross writes the game's byte");
	act(&v, MBOX_EV_ACTIVATE, "Goodies menu");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: the plain list, in the game's order, seven a window. */
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.ptype == MBOX_PTYPE_MAIN && v.nrows == 7 && strcmp(v.rows[0].label, "Veldin") == 0 &&
	      strcmp(v.rows[5].label, "Rilgar") == 0 && v.rows[6].kind == MROW_MORE,
	      "Planets: Veldin, Novalis, Aridia, Kerwan, Eudora, Rilgar and More");
	check((v.rows[0].flags & MROWF_CLOSES) != 0 && v.rows[0].kind == MROW_ACTION, "each a button that closes the menu first");
	for (i = 0; i < 6 && v.rows[v.nrows - 1].kind == MROW_MORE; i++) act(&v, MBOX_EV_ACTIVATE, "More");
	check(view_row(&v, "Veldin2") != NULL && v.rows[v.nrows - 1].kind != MROW_MORE,
	      "the last window ends with Veldin2: all nineteen planets");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Manips");
	check(labels_are(&v, manips, 4) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Manips: the category, Auto-reset, Remember, Apply now (RaC1 has no headers)");
	check(strcmp(value_of(&v, "Category"), "None") == 0 && act(&v, MBOX_EV_RIGHT, "Category") == ST_OK &&
	      strcmp(value_of(&v, "Category"), "NG+ (poki skip)") == 0, "Category: Right goes from None to NG+ (poki skip)");
	act(&v, MBOX_EV_LEFT, "Category");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Unlocks: the categories and Collectables; the values Locked, Unlocked, Gold. */
	act(&v, MBOX_EV_ACTIVATE, "Unlocks");
	check(labels_are(&v, unlocks, 4) && v.ptype == MBOX_PTYPE_MAIN,
	      "Unlocks: Weapons, Gadgets, Items, then Collectables");
	act(&v, MBOX_EV_ACTIVATE, "Collectables");
	check(labels_are(&v, collectables, 4) && v.ptype == MBOX_PTYPE_CONFIG && v.rows[0].kind == MROW_ACTION &&
	      strcmp(v.title, "Collectables") == 0,
	      "Collectables: the gold bolt and skill point buttons, a CONFIG page of buttons");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Weapons");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 16 && v.rows[15].kind != MROW_MORE &&
	      strcmp(v.title, "Weapons") == 0, "Weapons: all sixteen in one CONFIG page");
	poke8(0x96C14Au, 0);
	poke8(0x969CB2u, 0);
	tick(2);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Bomb Glove"), "Locked") == 0, "a locked weapon: Locked, in the game's title case");
	check(act(&v, MBOX_EV_RIGHT, "Bomb Glove") == ST_OK && peek8(0x96C14Au) == 1 && peek8(0x969CB2u) == 0,
	      "Right: owned, not gold");
	tick(2);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Bomb Glove"), "Unlocked") == 0, "Unlocked");
	act(&v, MBOX_EV_RIGHT, "Bomb Glove");
	tick(2);
	menu_model_view(&v);
	check(peek8(0x969CB2u) == 1 && strcmp(value_of(&v, "Bomb Glove"), "Gold") == 0, "Right: Gold");
	act(&v, MBOX_EV_RIGHT, "Bomb Glove");
	tick(2);
	menu_model_view(&v);
	check(peek8(0x96C14Au) == 0 && strcmp(value_of(&v, "Bomb Glove"), "Locked") == 0, "and Right wraps to Locked");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Items");
	r = view_row(&v, "Zoomerator");
	check(r != NULL && (strcmp(r->value, "Locked") == 0 || strcmp(r->value, "Unlocked") == 0),
	      "Items: an item with no gold version is Locked or Unlocked");
	check(quit(), "quit RaC1");
}

/* ------------------------------------------------------------ the install */

static void rac1_playing(void)
{
	poke32(RAC1_GAME_STATE, 0);
	poke32(R1_RESUME_PAGE, 0);
}

static void test_install_rac1(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC1);
	u32 mb;
	int i;

	group("console menu: RaC1's install and mailbox");

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 4 && d->ncaves == 7,
	      "RaC1 has a renderer: the mailbox, the state, the read-only data, four code areas, four words");
	if (d == NULL) return;
	check(d->caves[3].addr == MH_CAVE && d->caves[4].addr == MH_CAVE2 && d->caves[5].addr == MH_CAVE3 &&
	      d->caves[6].addr == MH_CAVE4 && d->caves[3].code && d->caves[4].code && d->caves[5].code &&
	      d->caves[6].code && !d->caves[0].code && !d->caves[2].code,
	      "the stubs, the core and the shim's functions in four code caves, after the data");
	check(d->hooks[0].addr == MH_SITE_TEXT && d->hooks[1].addr == MH_SITE_PLAY &&
	      d->hooks[2].addr == MH_SITE_HELP_TEXT && d->hooks[2].value == MH_HELP_WORD &&
	      d->hooks[3].addr == MH_SITE_HELP_PAGE && d->hooks[3].value == MH_SHIM + SH_PAGE_LIST,
	      "the text hook first, the play hook, then the Help entry's text and page last");
	check(d->mode_addr == RAC1_GAME_STATE && d->mode_paused == 3 && d->pending_addr == R1_RESUME_PAGE &&
	      d->pending_none == 0 && d->screen_addr == MH_SHIM + SH_UP && d->screen_id == R1_SCREEN_UP,
	      "the menu is down when the state is not the pause, no reopen is queued and the shim's word says "
	      "none of its pages is up");

	check(boot("NPEA00385"), "RaC1 boots");
	rac1_playing();
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_BOX) == 0, "two seconds in: nothing yet");
	poke32(RAC1_GAME_STATE, 3);
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "four, paused: still nothing");
	poke32(RAC1_GAME_STATE, 0);
	poke32(R1_RESUME_PAGE, 0x0072E0E0u);
	tick(60);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "in play with a reopen queued (a Goodies movie): nothing");
	poke32(R1_RESUME_PAGE, 0);
	for (i = 0; i < 40 && menu_state() != MENU_READY; i++) {
		host_write_log_reset();
		tick(1);
	}
	check(menu_state() == MENU_READY && hooks_are(d, 1), "in play, nothing queued: the renderer goes in, all four words");
	{
		u32 k, a = 0, l = 0, n = host_write_log_count(), bytes = 0, first_code = 999, first_hook = 999;
		for (k = 0; k < n && k < 64; k++) {
			host_write_log_at(k, &a, &l);
			bytes += l;
			if ((a == MH_CAVE || a == MH_CAVE2 || a == MH_CAVE3 || a == MH_CAVE4) && first_code == 999) first_code = k;
			if (a == MH_SITE_TEXT && first_hook == 999) first_hook = k;
		}
		check(first_code < first_hook && n <= 20 && bytes <= 22u * 1024u,
		      "the caves before the words: under twenty writes, under 22 KB");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 16 &&
	      peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 && peek8(mb + MBOX_OFF_MAX_ROWS) == 7 &&
	      peek8(mb + MBOX_OFF_CFG_ROWS) == 16 && peek8(mb + MBOX_OFF_RENDERER) == 1,
	      "the mailbox: sixteen rows of 0x40, seven a MAIN page, sixteen a CONFIG page, renderer 1");
	check(peek32(MH_SITE_HELP_TEXT) == 0x7E000003u && peek32(MH_SITE_HELP_PAGE) == MH_SHIM + SH_PAGE_LIST,
	      "the main page's Help entry: text 0x7E00, kind 3, the menu's MAIN page");

	/* menu.enabled = 0: every word back once the menu is down and the pause closed. */
	config_set_u32("menu.enabled", 0);
	poke32(MH_SHIM + SH_UP, 1);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "menu.enabled = 0 while one of its pages is up: nothing yet");
	poke32(MH_SHIM + SH_UP, 0);
	poke32(RAC1_GAME_STATE, 3);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "nor in the pause");
	poke32(RAC1_GAME_STATE, 0);
	poke32(R1_RESUME_PAGE, MH_SHIM + SH_PAGE_LIST);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "nor with a reopen on its page queued");
	poke32(R1_RESUME_PAGE, 0);
	tick(8);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_SITE_HELP_TEXT) == MH_ORIG_HELP_TEXT &&
	      peek32(MH_SITE_HELP_PAGE) == MH_ORIG_HELP_PAGE && peek32(MH_SITE_TEXT) == MH_ORIG_TEXT &&
	      peek32(MH_SITE_PLAY) == MH_ORIG_PLAY,
	      "in play: every word the game's again, Help its own row");
	config_set_u32("menu.enabled", 1);
	tick(4);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "menu.enabled = 1: back in at once");
	check(quit(), "quit RaC1");

	/* Refused: a probe's word at a site. */
	host_boot("NPEA00385");
	poke32(MH_SITE_TEXT, 0x484C1038u);          /* probe N1c's b n1c_text_tail */
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	rac1_playing();
	tick(4 * 120);
	check(menu_state() == MENU_REFUSED && peek32(MH_BOX) == 0 && peek32(MH_SITE_HELP_TEXT) == MH_ORIG_HELP_TEXT &&
	      peek32(MH_CAVE) == 0 && peek32(MH_CAVE2) == 0 && peek32(MH_CAVE3) == 0 && peek32(MH_CAVE4) == 0,
	      "probe N1c's text hook in the game: the install writes nothing");
	poke32(MH_SITE_TEXT, MH_ORIG_TEXT);
	tick(11 * 120);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "unloaded, the renderer goes in ten seconds on");
	check(quit(), "quit RaC1");

	/* The combo before the install is due puts it in at once. */
	menu_set_install_delay_us(60000000ull);
	check(boot("NPEA00385"), "RaC1 boots");
	rac1_playing();
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_READY && hooks_are(d, 1) && peek8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "a press of the combo installs at once and asks the renderer to open");
	menu_set_install_delay_us(3000000ull);
	check(quit(), "quit RaC1");

	/* RPCS3: nothing at all. */
	host_set_can_patch_code(0);
	check(boot("NPEA00385"), "RaC1 boots under RPCS3");
	rac1_playing();
	tick(4 * 120);
	check(menu_state() == MENU_OFF && peek32(MH_BOX) == 0 && hooks_are(d, 0), "no menu under RPCS3");
	check(quit(), "quit");
	host_set_can_patch_code(1);
}

/* ----------------------------------------------------------- the renderer */

/* The MAIN page's rows as the panes drew them, "|"-joined, and the CONFIG rows as label=value pairs. */
static const char *main_texts(char *out, u32 cap)
{
	u32 k;
	out[0] = 0;
	for (k = 0; k < 7; k++) {
		if (G.pane_text[k][0] == 0) continue;
		if (out[0] != 0) qstrcat(out, cap, "|");
		qstrcat(out, cap, G.pane_text[k]);
	}
	return out;
}

/* Draws come in pairs, label then value; the pair `i` as "label=value". */
static const char *conf_row(int i, char *out, u32 cap)
{
	out[0] = 0;
	if (2 * i + 1 >= G.ndraw) return out;
	qstrcpy(out, cap, G.drawn[2 * i]);
	qstrcat(out, cap, "=");
	qstrcat(out, cap, G.drawn[2 * i + 1]);
	return out;
}

/* A frame of the pause, then qwark's ticks to answer an event and post the page. */
static void step(u32 edges)
{
	frame(edges);
	tick(2);
}

static void test_renderer_rac1(void)
{
	char a[160], b[160];
	u32 mb;
	int i;

	group("console menu: RaC1's renderer, core and shim, on the fake console");

	memset(&G, 0, sizeof(G));
	G.list_h = 250;
	check(boot("NPEA00385"), "RaC1 boots");
	rac1_playing();
	seed_game();
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	check(menu_state() == MENU_READY, "the renderer is in");
	mb = menu_mailbox();
	frame(0);
	check(r32(MH_SHIM + SH_READY) == MH_MAGIC && r32(PAGE_LIST + 0x3C) == R1_SCREEN &&
	      r32(PAGE_LIST + 0x00) == 0x0E && r32(PAGE_CONF(0) + 0x00) == 0x76 && r32(PAGE_CONF(1) + 0x4C) == R1_EMPTY_ITEM &&
	      r32(PAGE_CONF(0) + 0x50) == LIST && r32(PAGE_CONF(0) + 0x54) == R1_TOGGLE_HINT,
	      "the first play frame sets the pages up: the main page's poses, Camera Options' poses and panes");

	/* The Help row: Trainer in its place, opening the menu's root on the main page's panes. */
	frame(0x800);
	check(r32(R1_STATE) == 3 && r32(R1_MENU_PAGE) == R1_MAIN_PAGE, "Start: the game's own main page");
	frame(0);
	check(strcmp(G.pane_text[4], "Trainer") == 0 && strcmp(G.pane_text[0], "Weapons") == 0,
	      "the fifth pane, Help's, says Trainer");
	w32(R1_MAIN_PAGE + 0x40, HELP_ITEM);
	frame(0x40);
	check(G.trans == 12 && G.trans_to == PAGE_LIST && G.help_opened == 0,
	      "the cross on Trainer: the game's transition to the menu's MAIN page");
	frames(12);
	check(r32(R1_MENU_PAGE) == PAGE_LIST, "arrived");
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN &&
	      r8(MH_SHIM + SH_NROWS) == 7 && r8(MH_SHIM + SH_PHASE) == PH_IN && r8(R1_GOODIES_PANE) == 1,
	      "the root's seven rows fly in on the main page's panes, pane 6 drawn for the seventh");
	frames(7);
	check(r8(MH_SHIM + SH_PHASE) == PH_IDLE, "and settle");
	check(strcmp(main_texts(a, sizeof(a)), "Game|Cheats|Planets|Manips|Unlocks|Save Files|Qwark Build 54") == 0,
	      "Game .. Save Files, then the build, in title case");
	printf("    (%s)\n", a);
	check(r16(ENTRY(6) + 2) == 0 && r16(ENTRY(0) + 2) == 1 && r32(ROW(5) + 0x3C) == 0 && r32(ROW(0) + 0x38) == 0,
	      "the build row grey (kind 0), left out of the links; no wrap at the ends");
	check(r32(PAGE_LIST + 0x40) == ROW(0), "the focus on Game");
	frame(PADB_DOWN);
	check(r32(PAGE_LIST + 0x40) == ROW(1) && G.sound == 1, "Down: Cheats, with the cursor's tick (the game's list)");
	frame(PADB_UP);

	/* Triangle at the root (from Help): the panes fly out, the main page comes back on Help. */
	frame(PADB_TRIANGLE);
	check(r8(MH_SHIM + SH_PHASE) == PH_OUT && G.sound == SND_PAGE, "Triangle at the root: the panes fly out");
	frames(6);
	check(G.trans == 12 && G.trans_to == R1_MAIN_PAGE && r32(PAGE_LIST + 0x38) == R1_MAIN_PAGE &&
	      r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && r8(R1_GOODIES_PANE) == 0,
	      "then the game's transition back to the main page (the MAIN page's poses backwards), the menu down");
	frames(12);
	check(r32(R1_MENU_PAGE) == R1_MAIN_PAGE && r32(R1_MAIN_PAGE + 0x40) == HELP_ITEM,
	      "the main page, its focus on Trainer");
	frame(0x40);
	frames(12);
	frame(0);
	check(r32(R1_MENU_PAGE) == PAGE_LIST && r32(MH_SHIM + SH_UP) == 1 && r8(MH_SHIM + SH_PHASE) == PH_IN,
	      "the cross again: back on the root, its panes flying in");
	frames(8);
	frame(0x800);
	frames(10);
	frame(0);
	check(r32(R1_STATE) == 0 && r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "Start: the game's list closes the pause; in play the menu is down");
	tick(4);

	/* The combo: the reopen fields, straight onto the root. */
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "the combo asks the renderer to open");
	w32(R1_FRAMES, 4);
	frame(0);
	check(r32(R1_RESUME_PAGE) == 0 && peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "not before Start itself could open the pause (eight frames in the state)");
	w32(R1_FRAMES, 100);
	frame(0);
	check(r32(R1_RESUME_PAGE) == PAGE_LIST && r32(R1_RESUME_SCREEN) == R1_SCREEN &&
	      peek8(mb + MBOX_OFF_REQUEST) == 0 && r8(MH_SHIM + SH_COMBO) == 1,
	      "then the play hook takes it and writes the reopen fields");
	frame(0);
	frame(0);
	check(r32(R1_STATE) == 3 && r32(R1_MENU_PAGE) == PAGE_LIST && r32(MH_STATE + ST_OPENS) == 1 &&
	      r8(MH_SHIM + SH_PHASE) == PH_IDLE && r8(MH_SHIM + SH_NROWS) == 7,
	      "the next play frame opens the pause on the root, counted (seven rows: no spare pane to send away)");
	frames(8);
	tick(4);

	/* CHEATS: the CONFIG page, the panes out, then Camera Options' transition. */
	w32(PAGE_LIST + 0x40, ROW(1));
	frame(0x40);
	check(G.sound == SND_SELECT && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "the cross on Cheats posts an event, with the select sound");
	tick(2);
	frame(0);
	check(r8(MH_SHIM + SH_PHASE) == PH_OUT, "the CONFIG page: the root's panes fly out");
	check(strcmp(main_texts(a, sizeof(a)), "Game|Cheats|Planets|Manips|Unlocks|Save Files|Qwark Build 54") == 0,
	      "showing the root's rows as they go");
	frames(6);
	check(G.trans == 12 && (G.trans_to == PAGE_CONF(0) || G.trans_to == PAGE_CONF(1)),
	      "then the game's transition to a CONFIG page");
	frames(12);
	frame(0);
	check(r1_at() == AT_CONF && strcmp(G.pane_text[0], "Cheats") == 0 && G.ndraw == 10 && G.blends >= 1,
	      "titled Cheats in the title pane, five rows in the list pane");
	check(strcmp(conf_row(0, a, sizeof(a)), "Fast Loads=Off") == 0 &&
	      strcmp(conf_row(4, b, sizeof(b)), "Goodies Menu=Off") == 0,
	      "labels and values in title case");
	printf("    (%s, %s)\n", a, b);
	check(G.draw_x[0] == 12.0f && G.draw_x[1] == 440.0f - 12.0f && G.draw_colour[0] == R1_COLOR_CURSOR &&
	      G.draw_colour[2] == R1_COLOR_TEXT && G.draw_y[0] == (float)(250 / 6 - 8) && G.draw_y[2] == (float)(2 * (250 / 6) - 8),
	      "Camera Options' geometry: the label at 12, the value right-aligned at w - 12, the cursor row lit");
	step(PADB_DOWN);
	check(r8(MH_SHIM + SH_CUR) == 1 && G.sound == SND_CURSOR, "Down: Ghost Ratchet, the cursor's tick");
	step(PADB_RIGHT);
	tick(2);
	frame(0);
	check(strcmp(conf_row(1, a, sizeof(a)), "Ghost Ratchet=On") == 0 && (features_toggle_state() & ((u64)1 << F_GHOST)) != 0,
	      "Right: On, refreshed in place");
	step(0x40);
	tick(2);
	frame(0);
	check(strcmp(conf_row(1, a, sizeof(a)), "Ghost Ratchet=Auto") == 0, "the cross: Auto");
	step(PADB_LEFT);
	tick(2);
	step(PADB_LEFT);
	tick(2);
	frame(0);
	check(strcmp(conf_row(1, a, sizeof(a)), "Ghost Ratchet=Off") == 0 && (features_toggle_state() & ((u64)1 << F_GHOST)) == 0,
	      "Left twice: On, then Off");

	/*
	 * A VALUE row with bounds short enough for a bar (MROWF_RANGE | SLIDER,
	 * as qwark sends Challenge mode in the games that have one), refreshed in
	 * place on the same page: drawn as Camera Options' 3D slider, the frame,
	 * the track and the fill sprite, its value before the bar.
	 */
	{
		u32 seq = peek32(mb + MBOX_OFF_PAGE_SEQ), r = mb + MBOX_OFF_ROWS + 3 * MBOX_ROW_SIZE;
		u8 words[16], keep[MBOX_ROW_SIZE];
		int boxes, sprites;

		host_peek(r, keep, sizeof(keep));
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 1);
		poke8(r + MROW_OFF_KIND, MROW_VALUE);
		poke8(r + MROW_OFF_FLAGS, MROWF_ENABLED | MROWF_STEPS | MROWF_RANGE | MROWF_SLIDER);
		memset(words, 0, sizeof(words));
		host_poke(r + MROW_OFF_LABEL, (const u8 *)"Challenge mode", 15);
		be32_put(words, 5);
		be32_put(words + 8, 10);
		be32_put(words + 12, 1);
		host_poke(r + MROW_OFF_RANGE, words, 16);
		host_poke(r + MROW_OFF_VALUE, (const u8 *)"5", 2);
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 2);
		frame(0);
		boxes = G.boxes;
		sprites = G.sprites;
		frame(0);
		check(G.boxes == boxes + 2 && G.sprites == sprites + 1 && strcmp(G.drawn[6], "Challenge Mode") == 0 &&
		      strcmp(G.drawn[7], "5") == 0 && G.draw_x[7] == 440.0f / 2 + 80 &&
		      G.draw_y[6] == G.draw_y[4] + (float)(250 / 6) + 4.0f,
		      "a slider row: its label 4 lower, its value before the bar, the frame and track boxes and the fill");
		/* qwark's own page back, as qwark posted it. */
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 3);
		host_poke(r, keep, sizeof(keep));
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 4);
		frame(0);
		frame(0);
		check(strcmp(conf_row(3, a, sizeof(a)), "Freeze Health=Off") == 0 && G.boxes == boxes + 2,
		      "and the row as it was: no bar");
	}

	/* Back: the CONFIG page's poses backwards, the root's panes fly in, the cursor on Cheats. */
	step(PADB_TRIANGLE);
	frame(0);
	check(G.trans == 12 && G.trans_to == PAGE_LIST, "Triangle: the game's transition back to the MAIN page");
	frames(12);
	frame(0);
	frames(8);
	check(r32(R1_MENU_PAGE) == PAGE_LIST && r32(PAGE_LIST + 0x40) == ROW(1) && r8(MH_SHIM + SH_NROWS) == 7,
	      "the root again, the focus on Cheats");

	/* GAME: eight rows; a long page scrolls in the list pane. */
	w32(PAGE_LIST + 0x40, ROW(0));
	step(0x40);
	frame(0);
	frames(7);
	frames(12);
	frame(0);
	check(r1_at() == AT_CONF && r8(MH_SHIM + SH_VIS) == 8 && G.ndraw == 16, "Game: eight rows, all on screen in a pane of 250");
	G.list_h = 120;
	frame(0);
	check(r8(MH_SHIM + SH_VIS) == 5 && G.ndraw == 10, "in a pane of 120, five on screen (rows of 20 at least)");
	for (i = 0; i < 7; i++) step(PADB_DOWN);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 7 && r8(MH_SHIM + SH_TOP) == 3 && strncmp(G.drawn[8], "Bolts", 5) == 0,
	      "Down to Bolts scrolls the window: rows 3 to 7");
	step(PADB_RIGHT);
	tick(2);
	frame(0);
	check(peek32(RAC1_BOLTS) == 1 && strcmp(G.drawn[9], "1") == 0, "Right on Bolts steps it");
	for (i = 0; i < 10; i++) step(PADB_UP);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 0 && r8(MH_SHIM + SH_TOP) == 0, "Up to the top: no wrap, the window back");

	/* Start on the CONFIG page closes the pause, as Camera Options' list does. */
	step(0x800);
	check(G.closing != 0, "Start: the pause closes");
	frames(12);
	frame(0);
	check(r32(R1_STATE) == 0 && r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "in play the menu is down");
	tick(4);

	/* The combo again opens the menu, on the root (qwark homes it at a close); the combo again closes it. */
	press(COMBO_MENU_DEFAULT);
	frame(0);
	frame(0);
	frame(0);
	check(r32(R1_STATE) == 3 && r1_at() == AT_LIST && r32(MH_SHIM + SH_UP) == 1 && r32(PAGE_LIST + 0x40) == ROW(0) &&
	      strcmp(main_texts(a, sizeof(a)), "Game|Cheats|Planets|Manips|Unlocks|Save Files|Qwark Build 54") == 0,
	      "the combo opens the menu again, on the root, the cursor on Game");
	frames(8);
	tick(4);
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with it up asks it to close");
	frame(0);
	frame(0);
	check(G.closing != 0 && peek8(mb + MBOX_OFF_REQUEST) == 0, "which the next frame takes, closing the pause");
	frames(12);
	frame(0);
	check(r32(R1_STATE) == 0 && r32(MH_SHIM + SH_UP) == 0, "closed");
	tick(4);

	/* A written open the game never takes (a movie starts first) is taken back. */
	press(COMBO_MENU_DEFAULT);
	mh_play();
	check(r32(R1_RESUME_PAGE) == PAGE_LIST && r8(MH_SHIM + SH_PENDING) == 30, "the combo writes the reopen fields");
	w32(R1_STATE, 7);
	mh_play();
	check(r32(R1_RESUME_PAGE) == 0 && r32(MH_SHIM + SH_TIMEOUTS) == 1 && r8(MH_SHIM + SH_PENDING) == 0,
	      "the state leaves play before the pause opened: the fields are taken back");
	w32(R1_STATE, 0);
	frame(0);

	check(quit(), "quit RaC1");
}

void test_menu_rac1(void)
{
	test_model_rac1();
	test_install_rac1();
	test_renderer_rac1();
}
