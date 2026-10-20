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
 *                 the opener and its reopen fields; the planet picker on the
 *                 Galactic Map's page (mh_rac1_pick.c) and the transitions'
 *                 direction and poses. The hook stubs are assembler and do
 *                 not run here: src/games/menuhelper/check.py steps them, and
 *                 the model does in C what they do.
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
#define NDRAW 40
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
	/* the transitions: back to the current page's parent, and the pose pane 0 then plays */
	int trans_back;
	u32 trans_pose0;
	/* the planet picker's page, as the last frame drew it */
	int map_draws, galaxy_draws;
	u32 map_cursor, galaxy_sel, map_flags;
	char map_rows[20][40];
	int map_nrows;
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
	if (id == 0x4FC6) return MSG_AT + 0x20;          /* "Cheats" */
	if (id == 0x4EF0) return MSG_AT + 0x30;          /* "Galactic Map" */
	if (id >= 0x4EB9 && id <= 0x4ECB) return MSG_AT + 0x40 + 0x10 * (id - 0x4EB9);   /* "Planet 0" .. */
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
#include "../src/games/menuhelper/mh_rac1_pick.c"

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
	put_str(MSG_AT + 0x20, "Cheats");
	put_str(MSG_AT + 0x30, "Galactic Map");
	w32(R1_MAP_TITLE + 0x00, OPD_LABEL_UPDATE);   /* the map's title item, as the executable has it */
	w32(R1_MAP_TITLE + 0x04, OPD_LABEL_DRAW);
	w32(R1_MAP_TITLE + 0x30, 0x0F);
	w32(R1_MAP_TITLE + 0x34, 0x4EF0);
	/* The planet table (0x7352B0): the names 0x4EB9 + p, the subtitles 0x4ECD + p, Veldin's 0x4EDE. */
	for (k = 0; k < 19; k++) {
		char name[16];
		snprintf(name, sizeof(name), "Planet %u", (unsigned)k);
		put_str(MSG_AT + 0x40 + 0x10 * k, name);
		w32(R1_PLANET_TABLE + 12 * k, 0x4EB9 + k);
		w32(R1_PLANET_TABLE + 12 * k + 4, k == 0 ? 0x4EDE : 0x4ECD + k - 1);
	}
	w32(R1_CONFIRM_MASK, 0x40);
	w32(R1_FRAMES, 100);
	w32(R1_HP, 8);
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
	if (tok == MH_FN_PICK) return mh_pick_update(item);
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
		if (item == 0 || item == R1_CHEATS_HINT) continue;
		/* one-row panes 180 x 36; the CONFIG list's large pane, Cheats', 440 x G.list_h */
		w32(item + 0x20, k == R1_CONF_LIST_PANE && page != PAGE_LIST ? 440 : 180);
		w32(item + 0x24, k == R1_CONF_LIST_PANE && page != PAGE_LIST ? (u32)G.list_h : 36);
		tok = r32(item + 4);
		if (tok == MH_FN_DRAW) {
			mh_conf_draw(item);
		} else if (tok == OPD_LIST_DRAW) {
			/* an entry of text 0 is the list's end: the generic draw shows nothing */
			if (r16(r32(item + 0x34)) != 0) read_str(ui_string(r16(r32(item + 0x34))), G.pane_text[k], 80);
			G.pane_flags[k] = r32(item + 0x30);
		} else if (tok == OPD_SCROLL_DRAW) {
			/* the map list: the scroll list's draw of its entries, the cursor's row lit */
			u32 ent = r32(item + 0x34), n = 0;
			while (n < 20 && r16(ent + 12 * n) != 0) {
				read_str(ui_string(r16(ent + 12 * n)), G.map_rows[n], 40);
				n++;
			}
			G.map_nrows = (int)n;
			G.map_cursor = r32(item + 0x40);
			G.map_flags = r32(item + 0x30);
			w32(item + 0x30, r32(item + 0x30) & ~0x8000u);
			G.map_draws++;
		} else if (tok == OPD_GALAXY_DRAW) {
			G.galaxy_sel = r32(R1_SELECTED);
			G.galaxy_draws++;
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
		G.trans_back = r32(R1_MENU_NEXT) == r32(page + 0x38);
		G.trans_pose0 = G.trans_back ? r32(page + 0x00) : r32(r32(R1_MENU_NEXT) + 0x00);
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
	/*
	 * Round 5: no Die, Bolts a button on Cheats; Auto-reset alone, no
	 * Remember. Round 6: no spacer on Game, and the Goodies menu in the place
	 * the other games give Challenge mode (RaC1 has none; the user: the
	 * goodies menu is its equivalent), off Cheats.
	 */
	static const char * const game[] = { "Position slot", "Save position", "Load position", "Goodies menu",
	                                     "Set aside file", "Load set-aside file", "Force autosave" };
	static const char * const cheats[] = { "Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health",
	                                       "Max bolts" };
	static const char * const manips[] = { "Category", "Auto-reset", "Apply now" };
	static const char * const unlocks[] = { "Weapons", "Gadgets", "Items", "Collectables" };
	static const char * const collectables[] = { "Unlock all gold bolts", "Reset all gold bolts",
	                                             "Unlock all skill points", "Reset skill points" };
	struct menu_view v;
	const struct menu_row *r;
	u8 i;

	group("console menu: the model, RaC1's pages");

	check(boot("NPEA00385"), "RaC1 boots");
	menu_model_reset(7, 24, CAPS_ALL);
	menu_model_view(&v);
	check(labels_are(&v, root, 7) && v.ptype == MBOX_PTYPE_MAIN && v.rows[6].kind == MROW_INFO &&
	      (v.rows[6].flags & MROWF_ENABLED) == 0 && strcmp(v.title, "Trainer") == 0,
	      "the root, Trainer: Game, Cheats, Planets, Manips, Unlocks, Save files, and the build, grey, in one "
	      "window of seven");

	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(labels_are(&v, game, 7) && v.ptype == MBOX_PTYPE_CONFIG && view_row(&v, "") == NULL &&
	      view_row(&v, "Challenge mode") == NULL,
	      "round 6, Game: the slot and the positions, the Goodies menu (in Challenge mode's place), the "
	      "save-file buttons and Force autosave, no blank row (the jankpot and debug rows left out)");
	r = view_row(&v, "Goodies menu");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0,
	      "the Goodies menu, a LIVE toggle, is Off or On");
	check(act(&v, MBOX_EV_ACTIVATE, "Goodies menu") == ST_OK && peek8(RAC1_GOODIES_MENU) == 1 &&
	      strcmp(value_of(&v, "Goodies menu"), "On") == 0,
	      "and the cross writes the game's byte");
	check(act(&v, MBOX_EV_ACTIVATE, "Goodies menu") == ST_OK && peek8(RAC1_GOODIES_MENU) == 0 &&
	      strcmp(value_of(&v, "Goodies menu"), "Off") == 0,
	      "and again takes it back (a LIVE toggle has no Auto)");
	r = view_row(&v, "Load set-aside file");
	check(r != NULL && (r->flags & MROWF_CLOSES) != 0, "Load set-aside file closes the menu first");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Cheats");
	check(labels_are(&v, cheats, 5) && v.ptype == MBOX_PTYPE_CONFIG && view_row(&v, "Goodies menu") == NULL,
	      "Cheats: the four cheats every game has, then (round 5) Max bolts; the Goodies menu is on Game");
	r = view_row(&v, "Max bolts");
	check(r != NULL && r->kind == MROW_ACTION && act(&v, MBOX_EV_ACTIVATE, "Max bolts") == ST_OK &&
	      peek32(RAC1_BOLTS) == 9999999u && value_of(&v, "Max bolts")[0] == 0,
	      "Max bolts: 9,999,999, the HUD counter's limit, the row blank");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0,
	      "Ghost Ratchet: the cross turns it On");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: the plain list, in the game's order, on a CONFIG page (round 4: the panes do not scroll). */
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 19 && strcmp(v.rows[0].label, "Veldin") == 0 &&
	      strcmp(v.rows[5].label, "Rilgar") == 0 && strcmp(v.rows[18].label, "Veldin2") == 0 &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == 0 && strcmp(v.title, "Planets") == 0,
	      "Planets: a CONFIG page of all nineteen, Veldin .. Veldin2, no More row");
	check((v.rows[0].flags & MROWF_CLOSES) != 0 && v.rows[0].kind == MROW_ACTION && v.rows[0].value[0] == 0,
	      "each a button, blank, that closes the menu first");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Manips");
	check(labels_are(&v, manips, 3) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Manips: the category, Auto-reset, Apply now (RaC1 has no headers, so no spacer either)");
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
		check(first_code < first_hook && n <= 20 && bytes <= 27u * 1024u,
		      "the caves before the words: under twenty writes, under 27 KB (round 4's 24 rows, the planet picker)");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 24 &&
	      peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 && peek8(mb + MBOX_OFF_MAX_ROWS) == 7 &&
	      peek8(mb + MBOX_OFF_CFG_ROWS) == 24 && peek8(mb + MBOX_OFF_RENDERER) == 1,
	      "the mailbox (round 4): 24 rows of 0x40, seven a MAIN page, 24 a CONFIG page, renderer 1");
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

static void test_picker_rac1(u32 mb);

static void test_renderer_rac1(void)
{
	char a[160], b[160];
	u32 mb;
	int i;

	group("console menu: RaC1's renderer, core and shim, on the fake console");

	memset(&G, 0, sizeof(G));
	G.list_h = 300;
	check(boot("NPEA00385"), "RaC1 boots");
	rac1_playing();
	seed_game();
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	check(menu_state() == MENU_READY, "the renderer is in");
	mb = menu_mailbox();
	frame(0);
	check(r32(MH_SHIM + SH_READY) == MH_MAGIC && r32(PAGE_LIST + 0x3C) == R1_SCREEN && r32(PAGE_LIST + 0x00) == 0x0E,
	      "the first play frame sets the pages up: the MAIN page on the main page's poses");
	for (i = 0; i < 2; i++) {
		u32 c = PAGE_CONF(i);
		check(r32(c + 0x00) == 0xA4 && r32(c + 0x04) == 0x9A && r32(c + 0x08) == 0x9B && r32(c + 0x0C) == 0x9C &&
		      r32(c + 0x10) == 0x9D && r32(c + 0x14) == 5 && r32(c + 0x34) == 13 && r32(c + 0x38) == PAGE_LIST &&
		      r32(c + 0x3C) == R1_SCREEN && r32(c + 0x40) == LIST && r32(c + 0x44) == TITLE && r32(c + 0x48) == LIST &&
		      r32(c + 0x4C) == R1_CHEATS_HINT && r32(c + 0x50) == 0 && r32(c + 0x54) == 0,
		      "a CONFIG page on Cheats' poses with In-Level Movies' title pose: the title, the list and Cheats' "
		      "hint on panes 0 to 2, nothing on the two panes Cheats leaves empty");
	}
	check(r32(TITLE + 0x00) == OPD_LABEL_UPDATE && r32(TITLE + 0x04) == OPD_LABEL_DRAW && r32(TITLE + 0x08) == 0 &&
	      r32(TITLE + 0x30) == 0x0F && r32(TITLE + 0x34) == R1_ID_TITLE && r32(LIST + 0x00) == MH_FN_CONF &&
	      r32(LIST + 0x04) == MH_FN_DRAW,
	      "the title a copy of Cheats' (no enter, flags 0x0F) with the shim's text id; the list the shim's");

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

	/* CHEATS: the CONFIG page, the panes out, then the game's transition to Cheats' layout. */
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
	      strcmp(conf_row(3, b, sizeof(b)), "Freeze Health=Off") == 0 &&
	      strcmp(conf_row(4, b + 80, 80), "Max Bolts=") == 0,
	      "labels and values in title case; round 5: Max Bolts, a blank button, last; round 6: no Goodies Menu");
	printf("    (%s, %s)\n", a, b);
	check(G.draw_x[0] == 12.0f && G.draw_x[1] == 440.0f - 12.0f && G.draw_colour[0] == R1_COLOR_CURSOR &&
	      G.draw_colour[2] == R1_COLOR_TEXT && G.draw_y[0] == (float)(300 / 6 - 8) && G.draw_y[2] == (float)(2 * (300 / 6) - 8),
	      "Cheats' list geometry: the label at 12, the value right-aligned at w - 12, the rows sharing the pane, "
	      "the cursor row lit");
	check(r16(MH_SHIM + SH_CONF_W) == 440 && r16(MH_SHIM + SH_CONF_H) == 300 && r32(LIST + 0x14) == PANE0 + 0x100,
	      "the list pane's size kept for the Memory panel; the list on pane 1, Cheats' list pane");
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
		      G.draw_y[6] == G.draw_y[4] + (float)(300 / 6) + 4.0f,
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

	/*
	 * A grey row and a label too long for its column, refreshed in place: the
	 * grey row in the generic list's grey and passed over by Down; the label
	 * cut with ".." so that it ends 10 before its value (the medium font's
	 * glyphs 10 wide here).
	 */
	{
		u32 seq = peek32(mb + MBOX_OFF_PAGE_SEQ), r2 = mb + MBOX_OFF_ROWS + 2 * MBOX_ROW_SIZE;
		u32 r4 = mb + MBOX_OFF_ROWS + 3 * MBOX_ROW_SIZE, c;
		u8 keep2[MBOX_ROW_SIZE], keep4[MBOX_ROW_SIZE];
		size_t n;

		for (c = 0; c < 128; c++) w8(R1_FONT_MEDIUM + 4 * c + 3, 10);
		host_peek(r2, keep2, sizeof(keep2));
		host_peek(r4, keep4, sizeof(keep4));
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 1);
		poke8(r2 + MROW_OFF_KIND, MROW_INFO);
		poke8(r2 + MROW_OFF_FLAGS, 0);
		host_poke(r4 + MROW_OFF_LABEL, (const u8 *)"abcdefghij abcdefghij abcdefghij abcdefg", MROW_LABEL_LEN);
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 2);
		frame(0);
		frame(0);
		n = strlen(G.drawn[6]);
		check(G.draw_colour[4] == R1_COLOR_GREY && G.draw_colour[2] == R1_COLOR_CURSOR && n >= 3 && n < 40 &&
		      strcmp(G.drawn[6] + n - 2, "..") == 0 && strcmp(G.drawn[7], "Off") == 0 &&
		      (int)n * 10 <= (440 - 12) - 3 * 10 - 10 - 12 && (int)(n + 1) * 10 > (440 - 12) - 3 * 10 - 10 - 12,
		      "a grey row in the generic list's grey; a label too long cut with \"..\", ending 10 before its value");
		printf("    (%s)\n", G.drawn[6]);
		frame(PADB_DOWN);
		frame(0);
		check(r8(MH_SHIM + SH_CUR) == 3 && G.draw_colour[6] == R1_COLOR_CURSOR, "Down passes over the grey row");
		frame(PADB_UP);
		check(r8(MH_SHIM + SH_CUR) == 1, "and Up too");
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 3);
		host_poke(r2, keep2, sizeof(keep2));
		host_poke(r4, keep4, sizeof(keep4));
		poke32(mb + MBOX_OFF_PAGE_SEQ, seq + 4);
		frame(0);
		frame(0);
		check(strcmp(conf_row(2, a, sizeof(a)), "Infinite Ammo=Off") == 0 &&
		      strcmp(conf_row(3, b, sizeof(b)), "Freeze Health=Off") == 0 && G.draw_colour[4] == R1_COLOR_TEXT,
		      "and the rows as they were");
		for (c = 0; c < 128; c++) w8(R1_FONT_MEDIUM + 4 * c + 3, 0);
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

	/*
	 * GAME (round 6): seven rows, no spacer among them, the Goodies menu in
	 * Challenge mode's place; a long page scrolls in the list pane.
	 */
	w32(PAGE_LIST + 0x40, ROW(0));
	step(0x40);
	frame(0);
	frames(7);
	frames(12);
	frame(0);
	check(r1_at() == AT_CONF && r8(MH_SHIM + SH_VIS) == 7 && G.ndraw == 14, "Game: seven rows, all on screen in a pane of 300");
	check(strcmp(conf_row(2, a, sizeof(a)), "Load Position=") == 0 &&
	      strcmp(conf_row(3, b, sizeof(b)), "Goodies Menu=Off") == 0 &&
	      strcmp(conf_row(4, b + 80, 80), "Set Aside File=") == 0,
	      "round 6: Goodies Menu straight after Load Position, Set Aside File after it, no blank row");
	step(PADB_DOWN);
	step(PADB_DOWN);
	check(r8(MH_SHIM + SH_CUR) == 2, "Down twice: Load Position");
	step(PADB_DOWN);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 3 && G.draw_colour[6] == R1_COLOR_CURSOR, "Down: Goodies Menu");
	step(0x40);
	tick(2);
	frame(0);
	check(strcmp(conf_row(3, a, sizeof(a)), "Goodies Menu=On") == 0 && peek8(RAC1_GOODIES_MENU) == 1 &&
	      r8(MH_SHIM + SH_CUR) == 3,
	      "the cross: On, the game's byte written, refreshed in place");
	step(0x40);
	tick(2);
	frame(0);
	check(strcmp(conf_row(3, a, sizeof(a)), "Goodies Menu=Off") == 0 && peek8(RAC1_GOODIES_MENU) == 0,
	      "and again: Off");
	step(PADB_DOWN);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 4 && G.draw_colour[8] == R1_COLOR_CURSOR, "Down: Set Aside File");
	step(PADB_UP);
	step(PADB_UP);
	check(r8(MH_SHIM + SH_CUR) == 2, "and Up twice: Load Position");
	G.list_h = 120;
	frame(0);
	check(r8(MH_SHIM + SH_VIS) == 5 && G.ndraw == 10, "in a pane of 120, five on screen (rows of 20 at least)");
	for (i = 0; i < 4; i++) step(PADB_DOWN);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 6 && r8(MH_SHIM + SH_TOP) == 2 && strncmp(G.drawn[8], "Force Autosave", 14) == 0,
	      "Down to Force Autosave, the last, scrolls the window: rows 2 to 6");
	for (i = 0; i < 10; i++) step(PADB_UP);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 0 && r8(MH_SHIM + SH_TOP) == 0, "Up to the top: no wrap, the window back");

	/* Back to the root, then Unlocks > Weapons: sixteen rows in Cheats' pane, fourteen on screen. */
	G.list_h = 300;
	step(PADB_TRIANGLE);
	frame(0);
	check(G.trans == 12 && G.trans_to == PAGE_LIST, "Triangle on Game: the game's transition back to the root");
	frames(12);
	frame(0);
	frames(8);
	w32(PAGE_LIST + 0x40, ROW(4));
	step(0x40);
	frame(0);
	frames(7);
	frames(7);
	check(r1_at() == AT_LIST && r8(MH_SHIM + SH_PHASE) == PH_IDLE &&
	      strcmp(main_texts(a, sizeof(a)), "Weapons|Gadgets|Items|Collectables") == 0,
	      "Unlocks: the root's panes out, four in");
	step(0x40);
	frame(0);
	frames(7);
	frames(12);
	frame(0);
	check(r1_at() == AT_CONF && mh_nrows() == 16 && r8(MH_SHIM + SH_VIS) == 14 && G.ndraw == 28 &&
	      strcmp(G.pane_text[0], "Weapons") == 0,
	      "Weapons: sixteen rows, fourteen on screen in a pane of 300 (rows of 20 at least)");
	check(G.draw_y[0] == (float)(300 / 15 - 8) && G.draw_y[26] == (float)(14 * (300 / 15) - 8),
	      "sharing the pane, 20 apart");
	read_str(mh_label(15), b, sizeof(b));
	for (i = 0; i < 15; i++) step(PADB_DOWN);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 15 && r8(MH_SHIM + SH_TOP) == 2 && strcmp(G.drawn[26], b) == 0 &&
	      G.draw_colour[26] == R1_COLOR_CURSOR,
	      "Down to the last weapon scrolls the window by two: rows 2 to 15, the last one lit");
	printf("    (%s)\n", b);
	step(PADB_UP);
	frame(0);
	check(r8(MH_SHIM + SH_CUR) == 14 && r8(MH_SHIM + SH_TOP) == 2, "Up: the window stays");

	/* Start on the CONFIG page closes the pause, as Cheats' list does. */
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

	test_picker_rac1(mb);

	check(quit(), "quit RaC1");
}

void test_menu_rac1(void)
{
	test_model_rac1();
	test_install_rac1();
	test_renderer_rac1();
}

/*
 * The planet picker, on the root the combo opened: the Galactic Map's page in
 * the same pause, every planet in qwark's order, the cursor moving the map's
 * selected planet, Triangle back to the root on the Planets row, the cross a
 * pick through the mailbox's pick fields and a plain close, Start and the
 * combo a close; the core's screen never stopped while it is up.
 */
static void test_picker_rac1(u32 mb)
{
	char a[160];
	u32 starts, seq, k, key, fr;
	int i, good;

	group("console menu: RaC1's planet picker, the Galactic Map's page");

	w32(R1_SELECTED, 0x77);
	poke32(RAC1_CURRENT_PLANET, 3);
	press(COMBO_MENU_DEFAULT);
	frame(0);
	frame(0);
	frame(0);
	frames(8);
	tick(4);
	check(r1_at() == AT_LIST && r8(MH_SHIM + SH_NROWS) == 7 && r8(MH_SHIM + SH_PHASE) == PH_IDLE,
	      "the root, from the combo");
	check(r32(PAGE_MAP + 0x00) == 0x63 && r32(PAGE_MAP + 0x10) == 0x67 && r32(PAGE_MAP + 0x14) == 5 &&
	      r32(PAGE_MAP + 0x3C) == R1_SCREEN && r32(PAGE_MAP + 0x40) == MAP_LIST &&
	      r32(PAGE_MAP + 0x44) == R1_MAP_TITLE && r32(PAGE_MAP + 0x48) == MAP_LIST &&
	      r32(PAGE_MAP + 0x4C) == MAP_GALAXY && r32(PAGE_MAP + 0x50) == R1_MAP_HINT &&
	      r32(PAGE_MAP + 0x54) == R1_MAP_SHOT && r32(PAGE_MAP + 0x58) == 0,
	      "the picker's page: the map's poses 0x63 .. 0x67, screen 50, the map's title, the shim's list and "
	      "galaxy, the map's hint and picture, no loader of planet maps");
	check(r32(MAP_LIST + 0x00) == MH_FN_PICK && r32(MAP_LIST + 0x04) == OPD_SCROLL_DRAW &&
	      r32(MAP_LIST + 0x30) == R1_MAP_LIST_FLAGS && r32(MAP_GALAXY + 0x00) == 0 &&
	      r32(MAP_GALAXY + 0x04) == OPD_GALAXY_DRAW,
	      "the list the map list's look with the shim's update, the galaxy the map's draw and no update");
	good = 1;
	for (k = 0; k < 19; k++) {
		u32 e = MAP_ENTRY(k);
		good = good && r16(e) == (k == 18 ? R1_ID_VELDIN2 : 0x4EB9 + k) && r16(e + 2) == 1 &&
		       r16(e + 8) == (k == 0 ? 0x4EDE : 0x4ECD + k - 1);
	}
	check(good && r16(MAP_ENTRY(19)) == 0, "nineteen entries, Veldin to Veldin2, each its name and subtitle, then the end");

	/* Open: the root's panes out, then the game's transition back to "the parent", on the map's poses. */
	starts = r32(MH_STATE + ST_STARTS);
	check(peek8(mb + MBOX_OFF_CAPS) == (CAPS_ALL | MBOX_CAP_PICKER) && (r8(mh_set() + SET_FLAGS + 2) & MROWF_PICKER) != 0,
	      "the renderer has the picker's cap, and qwark flags the root's Planets row");
	w32(PAGE_LIST + 0x40, ROW(2));
	seq = peek32(mb + MBOX_OFF_EV_SEQ);
	key = peek32(mb + MBOX_OFF_PAGE_KEY);
	fr = peek32(mb + MBOX_OFF_FRAMES);
	frame(0x40);
	check(mh_picker_up() && r8(MH_SHIM + SH_PICK) == PK_OPENING && r8(MH_SHIM + SH_PHASE) == PH_OUT &&
	      r8(MH_SHIM + SH_THEN) == T_PICK && r32(MH_SHIM + SH_PICK_OPENS) == 1 && G.sound == SND_SELECT &&
	      peek32(mb + MBOX_OFF_EV_SEQ) == seq,
	      "the cross on Planets: the core marks the picker up and asks the shim, the forward sound, no event; "
	      "the root's panes fly out");
	check(shim_picker(0) == 0, "and a second call while they move is refused");
	frames(6);
	check(G.trans == 12 && G.trans_to == PAGE_MAP && G.trans_back && G.trans_pose0 == 0x63 &&
	      r32(PAGE_LIST + 0x38) == PAGE_MAP && r8(R1_GOODIES_PANE) == 0,
	      "then the game's transition to the map page as the MAIN page's parent: the map's poses backwards, "
	      "its panes flying in");
	frames(12);
	frame(0);
	check(r1_at() == AT_MAP && r8(MH_SHIM + SH_PICK) == PK_UP && r32(PAGE_LIST + 0x00) == 0x0E &&
	      r32(PAGE_LIST + 0x18) == 0x14 && r32(PAGE_LIST + 0x38) == 0,
	      "arrived: the MAIN page's own poses and parent back");
	check(G.map_draws == 1 && G.map_nrows == 19 && G.map_cursor == 3 && (G.map_flags & 0x8000) != 0 &&
	      r32(R1_SELECTED) == 3 && G.galaxy_draws == 1 && G.galaxy_sel == 3 &&
	      strcmp(G.pane_text[0], "Galactic Map") == 0,
	      "the list of nineteen on the planet the player is on, the galaxy on it too, the map's title");
	check(strcmp(G.map_rows[0], "Planet 0") == 0 && strcmp(G.map_rows[17], "Planet 17") == 0 &&
	      strcmp(G.map_rows[18], "Veldin2") == 0, "the game's names, and Veldin2 for planet 18");
	check(r8(MH_SHIM + SH_STARTED) == 1 && r32(MH_STATE + ST_STARTS) == starts && mh_picker_up() &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && r32(MH_SHIM + SH_UP) == 1,
	      "the core's screen never stopped: the push is no close, hstate open, the picker up");

	/* The cursor: Up and Down, L1 and R1, no wrap, the tick, the selected planet following. */
	G.sounds = 0;
	frame(PADB_DOWN);
	frame(0);
	check(G.map_cursor == 4 && r32(R1_SELECTED) == 4 && G.galaxy_sel == 4 && G.sound == SND_CURSOR && G.sounds == 1,
	      "Down: Eudora, the galaxy and the picture follow, the cursor's tick");
	for (i = 0; i < 6; i++) frame(PADB_UP);
	frame(0);
	check(G.map_cursor == 0 && r32(R1_SELECTED) == 0 && G.sounds == 5, "Up to Veldin, and no further (no wrap)");
	frame(PADB_R1);
	frame(PADB_R1);
	frame(PADB_L1);
	frame(0);
	check(G.map_cursor == 1 && r32(R1_SELECTED) == 1, "R1 and L1 move it too");
	for (i = 0; i < 20; i++) frame(PADB_DOWN);
	frame(0);
	check(G.map_cursor == 18 && r32(R1_SELECTED) == 18, "Down to Veldin2, and no further");

	/* Triangle: the map's panes fly out, then the game's transition back to the MAIN page as the map's parent. */
	frame(PADB_TRIANGLE);
	check(r8(MH_SHIM + SH_PICK) == PK_RETURN && r8(MH_SHIM + SH_PHASE) == PH_OUT && G.sound == SND_PAGE,
	      "Triangle: the map's panes fly out");
	frames(6);
	check(G.trans == 12 && G.trans_to == PAGE_LIST && G.trans_back && G.trans_pose0 == 0x0E &&
	      r32(R1_SELECTED) == 0x77 && r8(R1_GOODIES_PANE) == 1,
	      "then the transition back on the main page's poses: the root's seven panes fly in; the map's "
	      "selected planet as the game had it");
	frames(12);
	frame(0);
	check(r1_at() == AT_LIST && r32(PAGE_LIST + 0x40) == ROW(2) && r8(MH_SHIM + SH_PICK) == PK_NONE &&
	      !mh_picker_up() && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN &&
	      r32(PAGE_MAP + 0x00) == 0x63 && r32(PAGE_MAP + 0x38) == 0 && r32(MH_STATE + ST_STARTS) == starts &&
	      strcmp(main_texts(a, sizeof(a)), "Game|Cheats|Planets|Manips|Unlocks|Save Files|Qwark Build 54") == 0,
	      "the root again, as it was, the focus on Planets, the picker down (mh_core_picker_back); the map "
	      "page's poses back");
	tick(8);
	frame(0);
	check(peek32(mb + MBOX_OFF_PAGE_KEY) == key && peek32(mb + MBOX_OFF_FRAMES) > fr && r32(PAGE_LIST + 0x40) == ROW(2) &&
	      r32(MH_STATE + ST_STARTS) == starts,
	      "and qwark resets nothing: the same root page, the frames counting on from before, still on Planets");
	frames(8);
	frame(PADB_DOWN);
	check(r32(PAGE_LIST + 0x40) == ROW(3), "and the root answers the pad again");
	frame(PADB_UP);

	/* The cross: the pick to qwark (mh_core_pick), then the plain close; qwark loads it in play. */
	frame(0x40);
	frames(6);
	frames(12);
	frame(0);
	frame(PADB_DOWN);
	seq = r32(mb + MBOX_OFF_PICK_SEQ);
	frame(0x40);
	check(G.closing != 0 && r32(mb + MBOX_OFF_PICK_PLANET) == 4 && r32(mb + MBOX_OFF_PICK_SEQ) == seq + 1 &&
	      r32(MH_SHIM + SH_PICK_LAST) == 4 && r32(MH_SHIM + SH_PICK_POSTS) == 1 && G.sound == SND_SELECT &&
	      r32(0x00A52BB0) == R1_SCREEN && !mh_picker_up() && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "the cross on Eudora: mh_core_pick posts it, the planet first, then its number, and the menu counts "
	      "as closed; the pause closes on screen 50, a plain close");
	frames(12);
	frame(0);
	check(r32(R1_STATE) == 0 && r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      r8(MH_SHIM + SH_PICK) == PK_NONE && r32(R1_SELECTED) == 0x77 && r32(PAGE_LIST + 0x00) == 0x0E &&
	      r32(PAGE_MAP + 0x00) == 0x63,
	      "in play: the menu down; the pages and the selected planet as before");
	tick(8);
	check(peek32(RAC1_LOAD_PLANET) != 0 && peek8(RAC1_DEST_PLANET + 3) == 4,
	      "and qwark loads Eudora, the game back in play");
	poke32(RAC1_LOAD_PLANET, 0);
	tick(4);

	/* Start from the picker closes, and so does the combo. */
	press(COMBO_MENU_DEFAULT);
	frame(0);
	frame(0);
	frame(0);
	frames(8);
	tick(4);
	check(r1_at() == AT_LIST && r32(PAGE_LIST + 0x40) == ROW(0), "the combo again: the root, on Game");
	w32(PAGE_LIST + 0x40, ROW(2));
	frame(0x40);
	check(mh_picker_up() && r8(MH_SHIM + SH_PICK) == PK_OPENING, "the picker again");
	frames(6);
	frames(12);
	frame(0);
	seq = peek32(mb + MBOX_OFF_PICK_SEQ);
	frame(0x800);
	check(G.closing != 0 && r32(MH_SHIM + SH_PICK_POSTS) == 1 && !mh_picker_up() &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && peek32(mb + MBOX_OFF_PICK_SEQ) == seq,
	      "Start: mh_core_picker_gone, a close with no pick; the pause closes");
	frames(12);
	frame(0);
	check(r32(R1_STATE) == 0 && r8(MH_SHIM + SH_PICK) == PK_NONE && r32(R1_SELECTED) == 0x77, "closed");
	tick(4);
	press(COMBO_MENU_DEFAULT);
	frame(0);
	frame(0);
	frame(0);
	frames(8);
	tick(4);
	w32(PAGE_LIST + 0x40, ROW(2));
	frame(0x40);
	frames(6);
	frames(12);
	frame(0);
	check(r1_at() == AT_MAP && mh_picker_up(), "the picker once more");
	press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo while it is up asks to close");
	frame(0);
	check(G.closing != 0 && peek8(mb + MBOX_OFF_REQUEST) == 0 && !mh_picker_up() &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "which the picker takes (mh_core_close_request), a close with no pick");
	frames(12);
	frame(0);
	check(r32(R1_STATE) == 0 && r8(MH_SHIM + SH_PICK) == PK_NONE, "closed");
	tick(4);

	/* Nowhere but on a still MAIN page: on a CONFIG page the core shows its list. */
	press(COMBO_MENU_DEFAULT);
	frame(0);
	frame(0);
	frame(0);
	frames(8);
	tick(4);
	w32(PAGE_LIST + 0x40, ROW(1));
	step(0x40);
	frame(0);
	frames(6);
	frames(12);
	frame(0);
	check(r1_at() == AT_CONF && shim_picker(0) == 0, "on a CONFIG page the picker is refused");
	step(0x800);
	frames(12);
	frame(0);
	tick(4);
	check(r32(0x00729970) == 0 && r32(R1_MAP_PAGE + 0x00) == 0 && r32(R1_MAP_LIST + 0x00) == 0,
	      "the game's own map, its page, its list and the map list's pointer, never written");
}
