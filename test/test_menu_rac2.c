/*
 * The console menu in Going Commando (revision 1.22, mailbox version 2), on
 * qwark's fake console, against the real core: three layers, as test_menu.c
 * and test_menu_uya.c have them for Deadlocked and UYA.
 *
 *   the model     RaC2's pages, built from its menu table (src/games/rac2.c):
 *                 the root, Game, Cheats, Planets in the game's order, Manips,
 *                 Unlocks with its buttons, and the unlock values Locked,
 *                 Unlocked and Max in the game's title case;
 *   the install   the six words a few seconds into the session in play, the
 *                 refusals, menu.enabled = 0 and the word that says one of
 *                 the menu's screens is the pause manager's;
 *   the renderer  the shared core and RaC2's shim (mh_rac2.c) compiled for the
 *                 PC (MH_HOST), against mocks of the game's pause: the pause
 *                 manager's screens and items, the root's bars and its own
 *                 update and draw, the setup of its bars, the calls Cheats'
 *                 draw makes, the Start path and the root it opens on. The
 *                 hook stubs are assembler and do not run here:
 *                 src/games/menuhelper/check.py steps them, and the mocks
 *                 below do in C what they do.
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
#include "../src/games/rac2.h"
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

#include "../src/games/menuhelper/mh_rac2.h"
#include "../src/games/menuhelper/mh_common.h"
#include "../src/games/menuhelper/mh_shim.h"

/* Where the fake game keeps what the mocks need. */
#define UI          0x00E00000u      /* what *R2_UI_MANAGER points at */
#define BARS        (UI + R2_BARS_OFF)
#define MSG_AT      0x00A80000u      /* the strings msg_string answers */
#define ROOT_ITEM   0x013468A4u
#define OUR_ITEM    (MH_SCREENS + MS_ITEM)
#define PM          R2_PAUSE_MGR     /* the pause manager */
#define SCREEN(k)   (MH_SCREENS + (u32)(k) * R2_SCREEN_SIZE)
#define LABELS      0x0135211Cu      /* the root's seven labels, Help's the fifth */

/* The bars' object as the root's code has it (0xCAA71C, 0xCAA70C, 0xCA9718). */
#define BAR_TEXT(i) (BARS + 0x1A8u + 0x50u * (u32)(i) + 0x38u)
#define BAR_ON(i)   (BARS + 0x3E0u + (u32)(i))
#define BAR_COUNT   (BARS + 0x3F8u)
#define BAR_EIGHTH  (BARS + 0x3FCu)

/*
 * The game's pause as far as the renderer can see it: the manager's state, the
 * root's draws, what the CONFIG look drew, the sounds, and the level the
 * questions of the root's setup describe.
 */
static struct {
	int paused, state, exit_action, opened;
	int gate_closed;
	int sound, sounds, screen_sounds;
	int root_draws, main_draws, setups;
	int begins, ends, sprites;
	int char_w;
	int q_race, q_ship, q_ship_on, q_orbit, q_other, pstate;
	char title[64], hint1[32], hint2[32];
	char lab[16][64], val[16][64];
	u32 colour[16];
	int rows_drawn, first_y, more_top, more_bottom;
} G;

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

static void put_str(u32 a, const char *s)
{
	u32 i;
	for (i = 0; i <= strlen(s); i++) w8(a + i, (u8)s[i]);
}

/*
 * msg_string with the label hook in: what mh_msg_stub answers (check.py steps
 * the real one), else a game text.
 */
static u32 game_msg(u32 id)
{
	if ((id & ~0xFu) == MH_MSG_BASE) {
		u32 i = id & 0xF;
		if (i == 0) return MH_SCREENS + MS_TRAINER;
		if (i <= MH_MAIN_ROWS) return mh_text(i - 1);      /* the text set on show */
	}
	return MSG_AT + (id & 0xFFu) * 0x20u;
}

static void mock_play_sound(int id, int a, int b)
{
	(void)a;
	(void)b;
	G.sound = id;
	G.sounds++;
	if (id == SND_SCREEN) G.screen_sounds++;
}

static u32 mock_msg_string(int id) { return game_msg((u32)id); }

/* The bars: SetText keeps the pointer msg_string gave it. */
static void mock_bars_labels(u32 r, int n, u32 ids)
{
	int i;
	w32(r + 0x3F8, (u32)n);
	for (i = 0; i < n; i++) w32(r + 0x1A8 + 0x50 * (u32)i + 0x38, game_msg(r32(ids + 4 * (u32)i)));
}

static void mock_bars_enable(u32 r, int i, int on) { w8(r + 0x3E0 + (u32)i, on ? 1 : 0); }
static void mock_bars_eighth(u32 r, int on) { w8(r + 0x3FC, on ? 1 : 0); }

/* Up and Down move the cursor past a bar that is not on, wrapping, back to the start at most. */
static int mock_bars_update(u32 r, u32 e)
{
	int n = (int)r32(r + 0x3F8), c = (int)r32(r + 0x3F0), start = c, d;

	if ((e & PADB_UP) != 0) d = -1;
	else if ((e & PADB_DOWN) != 0) d = 1;
	else return c;
	mock_play_sound(SND_MOVE, 0, 0);
	do {
		c = (c + d + n) % n;
		if (r8(r + 0x3E0 + (u32)c) != 0) break;
	} while (c != start);
	w32(r + 0x3F0, (u32)c);
	return c;
}

/* 0xBB6970 on an ordinary planet: the root's seven bars, the eighth on, the table, the cursor on the first. */
static void mock_bars_setup(void)
{
	int i;

	mock_bars_labels(BARS, 7, LABELS);
	for (i = 0; i < 7; i++) mock_bars_enable(BARS, i, 1);
	mock_bars_eighth(BARS, 1);
	w32(BARS + R2_R_POS, R2_POS_NORMAL);
	w32(BARS + R2_R_CURSOR, 0);
	G.setups++;
}

static int mock_root_draw(u32 item) { (void)item; G.main_draws++; return 0; }

/* What Cheats' draw draws, by where: the title, the hints, the rows and the marks of more. */
static void mock_draw_begin(int a) { (void)a; G.begins++; G.rows_drawn = 0; G.more_top = G.more_bottom = 0; G.sprites = 0; G.first_y = 0; }
static void mock_draw_end(void) { G.ends++; }
static u32 mock_sprite(u32 tex, int id) { return tex + (u32)id; }

static void mock_sprite_draw(u32 s, u32 a, u32 b, u32 colour, float x, float y, float f1, float f2, float f3)
{
	(void)s; (void)a; (void)b; (void)colour; (void)x; (void)y; (void)f1; (void)f2; (void)f3;
	G.sprites++;
}

static void mock_text_centre(int x, int y, u32 colour, u32 s, int len)
{
	(void)x; (void)colour; (void)len;
	if (y == CFG_Y_TITLE) read_str(s, G.title, sizeof(G.title));
	else if (y == CFG_Y_HINT1) read_str(s, G.hint1, sizeof(G.hint1));
	else if (y == CFG_Y_HINT2) read_str(s, G.hint2, sizeof(G.hint2));
	else if (y == CFG_Y_ROW0 - CFG_DY) G.more_top = 1;
	else G.more_bottom = y;
}

static void mock_text_left(int x, int y, u32 colour, u32 s, int len)
{
	int k = (y - CFG_Y_ROW0) / CFG_DY;
	(void)x; (void)len;
	if (k < 0 || k >= 16) return;
	if (G.rows_drawn == 0) G.first_y = y;
	read_str(s, G.lab[k], sizeof(G.lab[k]));
	G.colour[k] = colour;
	G.rows_drawn++;
}

static void mock_text_mid(int x, int y, u32 colour, u32 s, int len)
{
	int k = (y - CFG_Y_ROW0) / CFG_DY;
	(void)x; (void)colour; (void)len;
	if (k >= 0 && k < 16) read_str(s, G.val[k], sizeof(G.val[k]));
}

static int mock_text_width(u32 s, int len)
{
	char t[80];
	(void)len;
	return (int)strlen(read_str(s, t, sizeof(t))) * G.char_w;
}

static int mock_lv_race(void) { return G.q_race; }
static int mock_lv_ship(void) { return G.q_ship; }
static int mock_lv_ship_on(void) { return G.q_ship_on; }
static int mock_lv_orbit(void) { return G.q_orbit; }
static int mock_lv_other(void) { return G.q_other; }
static int mock_lv_pstate(void) { return G.pstate; }

#include "../src/games/menuhelper/mh_core.c"
#include "../src/games/menuhelper/mh_rac2.c"

/* ------------------------------------------------------- the pause manager */

#define CALL_UPDATE 0
#define CALL_DRAW   1
#define CALL_ENTER  2
#define CALL_EXIT   3

/* The root's own update (0xBBEC10): Start, Select and Triangle close; the cross on bar 4 opens entry 4's screen. */
static int mock_root_update(void)
{
	u32 e = r32(R2_PAD_EDGES);
	int c;

	if ((e & 0x910u) != 0) {
		mock_play_sound(SND_SCREEN, 0, 0);
		return 1;
	}
	c = mock_bars_update(BARS, e);
	if ((e & r32(R2_CONFIRM_MASK)) != 0 && c == 4) w32(PM + R2_MGR_NEXT, r32(MH_SITE_HELP_SCREEN));
	return 0;
}

/* An item call through the screen's first item: the root's, or the shim's (by the item's address). */
static int call(u32 screen, int which)
{
	u32 item;

	if (screen == R2_ROOT_SCREEN) {
		if (which == CALL_UPDATE) return mock_root_update();
		if (which == CALL_DRAW) G.root_draws++;
		return 0;
	}
	item = r32(screen + 0xEC);
	if (item != OUR_ITEM) return 0;
	switch (which) {
	case CALL_UPDATE: return mh_update(item);
	case CALL_DRAW:   return mh_draw(item);
	case CALL_ENTER:  return mh_enter(item, 0);
	default:          return mh_exit(item, 0);
	}
}

static u32 current(void) { return r32(PM + R2_MGR_CURRENT); }

/*
 * A frame of the pause with `edges` pressed this frame, as 0xBB93D0 runs the
 * manager: state 3 makes the next screen the current one; state 4 calls its
 * enters on its first frame, its update every frame (non-zero: close), and on a
 * next screen plays the screen sound (17 for the same one), calls the exits and
 * goes to state 3; a close waits for the pause's tenth frame and calls the
 * exits. Then the draw of the current screen, whatever state it is in.
 */
static void pframe(u32 edges)
{
	u32 cur, next;

	w32(R2_PAD_EDGES, edges);
	G.opened++;
	if (G.state == 3) {
		w32(PM + R2_MGR_CURRENT, r32(PM + R2_MGR_NEXT));
		w32(PM + R2_MGR_NEXT, 0);
		w32(PM + 4, 0);
		G.state = 4;
	} else if (G.state == 4) {
		cur = current();
		if (r32(PM + 4) == 0) call(cur, CALL_ENTER);
		w32(PM + 4, r32(PM + 4) + 1);
		if (call(cur, CALL_UPDATE) != 0) G.exit_action = 1;
		next = r32(PM + R2_MGR_NEXT);
		if (next != 0) {
			mock_play_sound(next == cur ? 17 : SND_SCREEN, 0, 0);
			call(cur, CALL_EXIT);
			w32(PM + R2_MGR_PREVIOUS, cur);
			w32(PM + R2_MGR_CURRENT, 0);
			G.state = 3;
		}
	}
	if (G.exit_action && G.opened >= 10) {
		cur = current();
		if (cur != 0) call(cur, CALL_EXIT);
		w32(PM + R2_MGR_CURRENT, 0);
		w32(PM + R2_MGR_NEXT, 0);
		w32(R2_GAME_MODE, 0);
		G.paused = 0;
		G.state = 0;
		G.exit_action = 0;
	}
	if (G.paused && current() != 0) call(current(), CALL_DRAW);
	w32(R2_PAD_EDGES, 0);
}

/*
 * A frame of play: the mode 0 update's Start test with the inject stub's logic
 * (check.py steps the real one), the gate, the combo stub, PauseInit's setup
 * of the bars and the root stub (stepped there too), then the manager's states
 * 1 and 2, which leave the screen as the next. `start` is the player's own.
 */
static void play_frame(int start)
{
	u32 test = start ? PADB_START : 0, screen = R2_ROOT_SCREEN;

	w8(MH_SHIM + SH_PRESSED, 0);
	if (r8(MH_BOX + MBOX_OFF_REQUEST) != 0) test = mh_inject(test);
	if (test == 0 || G.gate_closed || G.paused) return;
	w8(MH_SHIM + SH_FORCE, r8(MH_SHIM + SH_PRESSED));
	mock_bars_setup();
	if (r8(MH_SHIM + SH_FORCE) != 0) {
		w8(MH_SHIM + SH_FORCE, 0);
		if (r32(R2_UI_MANAGER) != 0 && r32(BARS + R2_R_POS) == R2_POS_NORMAL) {
			mh_count(ST_OPENS);
			screen = SCREEN(MS_COMBO);
		}
	}
	w32(R2_GAME_MODE, R2_MODE_PAUSE);
	w32(PM + R2_MGR_PREVIOUS, 0);
	w32(PM + R2_MGR_CURRENT, screen);
	w32(PM + R2_MGR_NEXT, screen);
	G.paused = 1;
	G.opened = 0;
	G.exit_action = 0;
	G.state = 3;
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

static void combo_press(u32 mask)
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

/* RaC2 in play: gameMode 0, no mode change queued. */
static void rac2_playing(void)
{
	poke32(R2_GAME_MODE, 0);
	poke32(R2_PENDING_MODE, R2_MODE_NONE);
}

#define CAPS_ALL (MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST)

/* The Lancer: unlock 0, item 30, its versions' items 60, 79 and 80 (rac2_panel.c). */
#define R2_LANCER_ITEM 30

/* ------------------------------------------------------------- the model */

static void test_model_rac2(void)
{
	static const char * const root[] = { "Game", "Cheats", "Planets", "Manips", "Unlocks", "Save files",
	                                     "qwark build 54" };
	static const char * const game[] = { "Die", "Position slot", "Save position", "Load position",
	                                     "Set aside file", "Load set-aside file", "Bolts", "Raritanium",
	                                     "Challenge mode", "Health XP" };
	static const char * const cheats[] = { "Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health",
	                                       "Set respawn point", "Store Swingshot", "Reset bosses on death",
	                                       "Reset platinum bolts on death" };
	static const char * const unlocks[] = { "Weapons", "Gadgets", "Items", "Collectables" };
	static const char * const collectables[] = { "Max all weapon levels", "Max all weapon ammo",
	                                             "Unlock all platinum bolts", "Reset platinum bolts",
	                                             "Unlock all nanotech boosts", "Reset nanotech boosts",
	                                             "Unlock all skill points", "Reset skill points" };
	struct menu_view v;
	const struct menu_row *r;
	u8 i;

	group("console menu: the model, RaC2's pages");

	check(boot("NPEA00386"), "RaC2 boots");
	menu_model_reset(7, 16, CAPS_ALL);
	menu_model_view(&v);
	check(labels_are(&v, root, 7) && v.ptype == MBOX_PTYPE_MAIN && v.rows[6].kind == MROW_INFO,
	      "the root: Game, Cheats, Planets, Manips, Unlocks, Save files, and the build, grey: the design's, "
	      "seven rows, the root's own seven bars");

	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(labels_are(&v, game, 10) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Game: a CONFIG page, the quick block and the client's Values (no QE offset, no cosmetics)");
	r = view_row(&v, "Raritanium");
	check(r != NULL && r->kind == MROW_VALUE, "Raritanium is a stepper");
	check(act(&v, MBOX_EV_RIGHT, "Bolts") == ST_OK && peek32(RAC2_BOLTS) == 1, "and Right on Bolts adds one");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Cheats");
	check(labels_are(&v, cheats, 8) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Cheats: the four cheats every game has, then the Game panel's Player section");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0,
	      "Ghost Ratchet: the cross turns it On");
	r = view_row(&v, "Store Swingshot");
	check(r != NULL && r->kind == MROW_ACTION && (r->flags & MROWF_STEPS) == 0,
	      "Store Swingshot is a button: Left and Right do nothing on it");
	r = view_row(&v, "Reset bosses on death");
	check(r != NULL && r->kind == MROW_TOGGLE, "the resets on death are checkboxes");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	check(view_row(&v, "Enable debug mode") == NULL, "debug mode, the client's Debug page, is left out");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: the plain list in the ids' order, which is the game's, seven a window. */
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.ptype == MBOX_PTYPE_MAIN && v.nrows == 7 && strcmp(v.rows[0].label, "Aranos") == 0 &&
	      strcmp(v.rows[5].label, "Feltzin") == 0 && v.rows[6].kind == MROW_MORE,
	      "Planets: Aranos, Oozla, Maktar, Endako, Barlow, Feltzin and More");
	check((v.rows[0].flags & MROWF_CLOSES) != 0 && v.rows[0].kind == MROW_ACTION, "each a button that closes the menu first");
	for (i = 0; i < 8 && v.rows[v.nrows - 1].kind == MROW_MORE; i++) act(&v, MBOX_EV_ACTIVATE, "More");
	check(view_row(&v, "Jamming Array") != NULL && v.rows[v.nrows - 1].kind != MROW_MORE,
	      "the last window ends with the Jamming Array");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Manips: no header in RaC2, so the category and its options. */
	act(&v, MBOX_EV_ACTIVATE, "Manips");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 4 && strcmp(v.rows[0].label, "Category") == 0 &&
	      strcmp(value_of(&v, "Category"), "None") == 0, "Manips: Category (None), Auto-reset, Remember, Apply now");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Unlocks: the categories and Collectables, its eight buttons a page of their own; Locked, Unlocked, Max. */
	act(&v, MBOX_EV_ACTIVATE, "Unlocks");
	check(labels_are(&v, unlocks, 4) && v.ptype == MBOX_PTYPE_MAIN,
	      "Unlocks: Weapons, Gadgets, Items and Collectables (round 3's page for the buttons)");
	act(&v, MBOX_EV_ACTIVATE, "Collectables");
	check(labels_are(&v, collectables, 8) && v.ptype == MBOX_PTYPE_CONFIG && strcmp(v.title, "Collectables") == 0 &&
	      v.rows[0].kind == MROW_ACTION, "Collectables: the two weapon buttons and the client's six Collectables, "
	      "buttons on a CONFIG page");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Weapons");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 16 && v.rows[15].kind == MROW_MORE &&
	      strcmp(v.title, "Weapons 1/2") == 0, "Weapons: twenty-four, fifteen and More in a page of sixteen");
	poke8(RAC2_OWNED_ARRAY + R2_LANCER_ITEM, 0);
	poke8(RAC2_ITEM_ARRAY + R2_LANCER_ITEM, R2_LANCER_ITEM);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Lancer"), "Locked") == 0, "a locked weapon: Locked, in the game's title case");
	check(act(&v, MBOX_EV_RIGHT, "Lancer") == ST_OK && strcmp(value_of(&v, "Lancer"), "Unlocked") == 0 &&
	      peek8(RAC2_OWNED_ARRAY + R2_LANCER_ITEM) == 1, "Right: Unlocked, owned at v1");
	check(act(&v, MBOX_EV_RIGHT, "Lancer") == ST_OK && strcmp(value_of(&v, "Lancer"), "Max") == 0 &&
	      peek8(RAC2_ITEM_ARRAY + R2_LANCER_ITEM) == 80, "Right: Max, v4 (its version item 80)");
	check(act(&v, MBOX_EV_RIGHT, "Lancer") == ST_OK && strcmp(value_of(&v, "Lancer"), "Locked") == 0,
	      "Right wraps to Locked");
	poke8(RAC2_OWNED_ARRAY + R2_LANCER_ITEM, 1);
	poke8(RAC2_ITEM_ARRAY + R2_LANCER_ITEM, 60);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Lancer"), "V2") == 0, "a version between shows as itself, V2");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Gadgets");
	r = view_row(&v, "Swingshot");
	check(v.nrows == 10 && r != NULL && (strcmp(r->value, "Locked") == 0 || strcmp(r->value, "Unlocked") == 0),
	      "Gadgets: ten, Locked or Unlocked");
	check(act(&v, MBOX_EV_RIGHT, "Swingshot") == ST_OK &&
	      (strcmp(value_of(&v, "Swingshot"), "Locked") == 0 || strcmp(value_of(&v, "Swingshot"), "Unlocked") == 0),
	      "and Right flips it, in the game's words both ways");

	check(quit(), "quit RaC2");
}

/* ------------------------------------------------------------ the install */

static void test_install_rac2(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC2);
	u32 mb;
	int i;

	group("console menu: RaC2's install and mailbox");

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 6 && d->ncaves == 4,
	      "RaC2 has a renderer: the mailbox, the state, the screens and read-only data, the code, six words");
	if (d == NULL) return;
	check(d->hooks[0].addr == MH_SITE_MSG && d->hooks[1].addr == MH_SITE_INJECT &&
	      d->hooks[2].addr == MH_SITE_COMBO && d->hooks[3].addr == MH_SITE_ROOT &&
	      d->hooks[4].addr == MH_SITE_HELP_SCREEN && d->hooks[4].value == SCREEN(MS_HELP) &&
	      d->hooks[5].addr == MH_SITE_HELP_LABEL && d->hooks[5].value == MH_MSG_HELP,
	      "the label hook first, the Start path's three, then Help's screen and label, last");
	check(d->mode_addr == R2_GAME_MODE && d->mode_paused == 3 && d->pending_addr == R2_PENDING_MODE &&
	      d->pending_none == R2_MODE_NONE && d->screen_addr == MH_SHIM + SH_UP && d->screen_id == R2_SCREEN_UP,
	      "the menu is down when gameMode is not 3, nothing is queued and the shim's word says none of its "
	      "screens is the manager's");

	check(boot("NPEA00386"), "RaC2 boots");
	rac2_playing();
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_BOX) == 0, "two seconds in: nothing yet");
	poke32(R2_GAME_MODE, 3);
	tick(240);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "four, paused: still nothing");
	poke32(R2_GAME_MODE, 0);
	for (i = 0; i < 40 && menu_state() != MENU_READY; i++) {
		host_write_log_reset();
		tick(1);
	}
	check(menu_state() == MENU_READY && hooks_are(d, 1), "in play again, the renderer goes in, all six words");
	{
		u32 k, a = 0, l = 0, n = host_write_log_count(), bytes = 0, at_code = 999, first_hook = 999;
		for (k = 0; k < n && k < 64; k++) {
			host_write_log_at(k, &a, &l);
			bytes += l;
			if (a == MH_CAVE) at_code = k;
			if (a == MH_SITE_MSG && first_hook == 999) first_hook = k;
		}
		check(at_code < first_hook && n <= 16 && bytes <= 18u * 1024u,
		      "the caves before the words: fifteen writes, under 18 KB");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 16 &&
	      peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 && peek8(mb + MBOX_OFF_MAX_ROWS) == 7 &&
	      peek8(mb + MBOX_OFF_CFG_ROWS) == 16 && peek8(mb + MBOX_OFF_RENDERER) == 2,
	      "the mailbox: sixteen rows of 0x40, seven a MAIN page, sixteen a CONFIG page, renderer 2");
	check(peek32(MH_SITE_HELP_LABEL) == 0xF000 && peek32(MH_SITE_HELP_SCREEN) == SCREEN(MS_HELP) &&
	      peek32(SCREEN(MS_HELP) + 0xEC) == OUR_ITEM && peek32(SCREEN(MS_HELP) + 0xE0) == R2_ROOT_SCREEN,
	      "the root's Help entry: label 0xF000 (Trainer), its screen the menu's, which the data image brought");

	/* menu.enabled = 0: every word back once the menu is down and the pause closed. */
	config_set_u32("menu.enabled", 0);
	poke32(MH_SHIM + SH_UP, 1);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "menu.enabled = 0 while one of its screens is up: nothing yet");
	poke32(MH_SHIM + SH_UP, 0);
	poke32(R2_GAME_MODE, 3);
	tick(8);
	check(menu_state() == MENU_REMOVING && hooks_are(d, 1), "nor in the pause");
	poke32(R2_GAME_MODE, 0);
	tick(8);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0) && peek32(MH_SITE_HELP_LABEL) == MH_ORIG_HELP_LABEL &&
	      peek32(MH_SITE_HELP_SCREEN) == MH_ORIG_HELP_SCREEN && peek32(MH_SITE_MSG) == MH_ORIG_MSG,
	      "in play: every word the game's again, Help its own entry");
	config_set_u32("menu.enabled", 1);
	tick(4);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "menu.enabled = 1: back in at once");
	check(quit(), "quit RaC2");

	/* Refused: a prototype's word at a site. */
	host_boot("NPEA00386");
	poke32(MH_SITE_MSG, 0x487B40C8u);           /* probe N2c's b n2c_msg_stub */
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	rac2_playing();
	tick(4 * 120);
	check(menu_state() == MENU_REFUSED && peek32(MH_BOX) == 0 && peek32(MH_SITE_HELP_LABEL) == MH_ORIG_HELP_LABEL &&
	      peek32(MH_CAVE) == 0, "probe N2c's label hook in the game: the install writes nothing");
	poke32(MH_SITE_MSG, MH_ORIG_MSG);
	tick(11 * 120);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "unloaded, the renderer goes in ten seconds on");
	check(quit(), "quit RaC2");

	/* The combo before the install is due puts it in at once. */
	menu_set_install_delay_us(60000000ull);
	check(boot("NPEA00386"), "RaC2 boots");
	rac2_playing();
	combo_press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_READY && hooks_are(d, 1) && peek8(MH_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "a press of the combo installs at once and asks the renderer to open");
	menu_set_install_delay_us(3000000ull);
	check(quit(), "quit RaC2");

	/* RPCS3: nothing at all. */
	host_set_can_patch_code(0);
	check(boot("NPEA00386"), "RaC2 boots under RPCS3");
	rac2_playing();
	tick(4 * 120);
	check(menu_state() == MENU_OFF && peek32(MH_BOX) == 0 && hooks_are(d, 0), "no menu under RPCS3");
	check(quit(), "quit");
	host_set_can_patch_code(1);
}

/* ----------------------------------------------------------- the renderer */

static const char *bar_text(int i, char *out) { return read_str(r32(BAR_TEXT(i)), out, 80); }

/* The bars' texts are the page's rows, in order. */
static int bars_are(const char * const *want, int n)
{
	char t[80];
	int i;
	if ((int)r32(BAR_COUNT) != n) return 0;
	for (i = 0; i < n; i++) {
		if (strcmp(bar_text(i, t), want[i]) != 0) return 0;
	}
	return 1;
}

/* The game's root's labels and strings, which the fake console does not hold (bar the hooked word). */
static void seed_game(void)
{
	static const u32 ids[7] = { 0x2D51, 0x2D52, 0x2D53, 0x2D54, 0, 0x2D56, 0x2DB3 };
	static const char * const names[7] = { "Weapons", "Gadgets", "Map", "Goodies", "", "Options", "Special" };
	u32 i;

	for (i = 0; i < 7; i++) {
		if (i == 4) continue;                     /* Help's, the hooked word */
		poke32(LABELS + 4 * i, ids[i]);
		put_str(game_msg(ids[i]), names[i]);
	}
	put_str(game_msg(0x2D55), "Help");
	put_str(game_msg(R2_ID_TOGGLE), "\x10 Toggle");
	put_str(game_msg(R2_ID_EXIT), "\x12 Exit");
	poke32(R2_UI_MANAGER, UI);
	poke32(R2_CONFIRM_MASK, 0x40);
	poke32(R2_SCREEN_W, 512);
	poke32(R2_ROOT_SCREEN + 0xEC, ROOT_ITEM);
}

/* Frames of the pause until its screen has entered, and qwark's ticks between. */
static void settle(void)
{
	tick(2);
	pframe(0);
	pframe(0);
	pframe(0);
}

static void test_renderer_rac2(void)
{
	static const char * const root[] = { "Game", "Cheats", "Planets", "Manips", "Unlocks", "Save Files",
	                                     "Qwark Build 54" };
	static const char * const game_bars[] = { "Weapons", "Gadgets", "Map", "Goodies", "Trainer", "Options",
	                                          "Special" };
	char a[80];
	u32 mb, seq;
	int i, n;

	group("console menu: RaC2's renderer, core and shim, on the fake console");

	memset(&G, 0, sizeof(G));
	G.char_w = 8;
	check(boot("NPEA00386"), "RaC2 boots");
	rac2_playing();
	seed_game();
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	check(menu_state() == MENU_READY, "the renderer is in");
	mb = menu_mailbox();

	/* Start: the game's root, Trainer in Help's place. */
	play_frame(1);
	pframe(0);
	check(G.paused && current() == R2_ROOT_SCREEN && G.root_draws == 1 && bars_are(game_bars, 7) &&
	      r8(BAR_EIGHTH) == 1, "Start: the game's root, seven bars, the fifth \"Trainer\" where Help was");

	/* Trainer: the menu's root on the bars. */
	w32(BARS + R2_R_CURSOR, 4);
	pframe(0x40);
	check(G.state == 3 && r32(PM + R2_MGR_NEXT) == SCREEN(MS_HELP) && G.sound == SND_SCREEN && current() == 0,
	      "the cross on Trainer: the menu's Help screen next, with the game's screen sound");
	n = G.main_draws;
	pframe(0);
	check(current() == SCREEN(MS_HELP) && G.main_draws == n && r8(MH_SHIM + SH_READY) == 0,
	      "the frame it becomes the current screen, before its enter: nothing of the menu drawn");
	pframe(0);
	check(G.main_draws == n + 1 && bars_are(root, 7) && r8(BAR_ON(6)) == 0 && r8(BAR_ON(0)) == 1 &&
	      r8(BAR_EIGHTH) == 0 && r32(BARS + R2_R_CURSOR) == 0 && r8(MH_SHIM + SH_FROM_HELP) == 1 &&
	      r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN,
	      "its enter: Game .. Save Files and a grey Qwark Build 54 on the bars, in title case, the cursor on Game");
	pframe(PADB_UP);
	check(r32(BARS + R2_R_CURSOR) == 5 && G.sound == SND_MOVE, "Up from Game wraps past the grey bar to Save Files");
	pframe(PADB_DOWN);

	/* Triangle at the root: the game's root again, on Trainer. */
	n = G.setups;
	pframe(PADB_TRIANGLE);
	check(G.setups == n + 1 && r32(PM + R2_MGR_PREVIOUS) == SCREEN(MS_HELP) && G.sound == SND_SCREEN &&
	      r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED,
	      "Triangle at the menu's root: the game's bars set up again, the root next, the menu down");
	pframe(0);
	check(current() == R2_ROOT_SCREEN && bars_are(game_bars, 7) && r32(BARS + R2_R_CURSOR) == 4,
	      "the game's root again, its cursor on Trainer");
	for (i = 0; i < 10; i++) pframe(0);
	pframe(PADB_START);
	check(!G.paused && peek32(R2_GAME_MODE) == 0, "Start closes the pause");
	tick(4);

	/* The combo: through the Start path and the root stub, straight onto the menu's root. */
	combo_press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "the combo asks the renderer to open");
	play_frame(0);
	check(G.paused && r32(PM + R2_MGR_NEXT) == SCREEN(MS_COMBO) && peek8(mb + MBOX_OFF_REQUEST) == 0 &&
	      r32(MH_STATE + ST_OPENS) == 1 && r32(MH_STATE + ST_OPEN_REQS) == 1,
	      "the next frame of play opens the pause on the combo's screen, counted");
	pframe(0);
	pframe(0);
	check(current() == SCREEN(MS_COMBO) && bars_are(root, 7) && r8(MH_SHIM + SH_FROM_HELP) == 0,
	      "its enter: the menu's root, not from Help");
	tick(4);

	/* Cheats: the two-column page, on the next screen, in Cheats' look. */
	w32(BARS + R2_R_CURSOR, 1);
	n = G.sounds;
	pframe(0x40);
	check(G.sounds == n && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "the cross on Cheats posts an event, with no sound of its own: the screen change has it");
	tick(2);
	pframe(0);
	check(G.state == 3 && r32(PM + R2_MGR_PREVIOUS) == SCREEN(MS_COMBO) && G.sound == SND_SCREEN &&
	      r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN,
	      "the CONFIG page: the screen changes with the game's sound, the menu still up");
	n = G.begins;
	pframe(0);
	check(current() == SCREEN(MS_CFG_A) && G.begins == n, "the next screen, CONFIG A, drawn nothing before its enter");
	pframe(0);
	check(G.begins == n + 1 && G.ends == n + 1 && G.sprites == 3 && strcmp(G.title, "Cheats") == 0 &&
	      strcmp(G.hint1, "\x10 Toggle") == 0 && strcmp(G.hint2, "\x12 Exit") == 0,
	      "Cheats' look: the frame's three sprites, the title Cheats, the Toggle and Exit hints");
	check(G.rows_drawn == 8 && G.first_y == CFG_Y_ROW0 && strcmp(G.lab[0], "Fast Loads") == 0 &&
	      strcmp(G.val[0], "Off") == 0 && strcmp(G.lab[7], "Reset Platinum Bolts On Death") == 0 &&
	      G.colour[0] == CFG_COLOR_CURSOR && G.colour[1] == CFG_COLOR_TEXT && !G.more_top && !G.more_bottom,
	      "eight rows from y 112, labels and values in title case, the cursor's row in Cheats' highlight");
	pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 1 && G.sound == SND_MOVE && G.colour[1] == CFG_COLOR_CURSOR,
	      "Down: the cursor on Ghost Ratchet, the tick");
	pframe(PADB_RIGHT);
	check(G.sound == SND_CHANGE && peek32(mb + MBOX_OFF_EV_SEQ) != peek32(mb + MBOX_OFF_EV_ACK),
	      "Right on Ghost Ratchet posts an event, with Cheats' change sound");
	tick(2);
	n = G.screen_sounds;
	pframe(0);
	check(strcmp(G.val[1], "On") == 0 && G.screen_sounds == n && current() == SCREEN(MS_CFG_A) &&
	      r8(MH_SHIM + SH_CCUR) == 1 && (features_toggle_state() & ((u64)1 << R2_GHOST)) != 0,
	      "the same page refreshed in place: On, the cursor where it was, no screen change");
	for (i = 0; i < 3; i++) pframe(PADB_DOWN);
	seq = peek32(mb + MBOX_OFF_EV_SEQ);
	pframe(PADB_LEFT);
	check(r8(MH_SHIM + SH_CCUR) == 4 && strcmp(G.lab[4], "Set Respawn Point") == 0 && G.sound == SND_NO &&
	      peek32(mb + MBOX_OFF_EV_SEQ) == seq, "Left on Set Respawn Point, a button: Cheats' refusal, no event");

	/* A label too long for its column is cut short before its value: the page refreshed after a cross. */
	G.char_w = 12;
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(r32(MH_SHIM + SH_CUTS) >= 1 && strlen(G.lab[7]) < strlen("Reset Platinum Bolts On Death") &&
	      strcmp(G.lab[7] + strlen(G.lab[7]) - 2, "..") == 0 &&
	      (int)strlen(G.lab[7]) * 12 <= CFG_X_VALUE - 3 * 12 / 2 - CFG_GAP - CFG_X_LABEL,
	      "wider glyphs: Reset Platinum Bolts On Death cut short with \"..\", ending before its value");
	G.char_w = 8;

	/* Back: the root on the bars again, on the next MAIN screen, the cursor on Cheats. */
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	pframe(0);
	check(current() == SCREEN(MS_MAIN_A) && bars_are(root, 7) && r32(BARS + R2_R_CURSOR) == 1,
	      "Triangle: MAIN A, the root's bars, the cursor on Cheats");

	/* Game: ten rows, all on screen. */
	w32(BARS + R2_R_CURSOR, 0);
	pframe(0x40);
	settle();
	check(current() == SCREEN(MS_CFG_A) && G.rows_drawn == 10 && strcmp(G.lab[9], "Health XP") == 0 &&
	      !G.more_top && !G.more_bottom, "Game: ten rows, all in the frame");
	pframe(PADB_TRIANGLE);
	settle();

	/* Unlocks, then Weapons: sixteen rows, ten on screen, scrolled. */
	w32(BARS + R2_R_CURSOR, 4);
	pframe(0x40);
	settle();
	check(current() == SCREEN(MS_MAIN_B) && r32(BAR_COUNT) == 4 && strcmp(bar_text(0, a), "Weapons") == 0 &&
	      strcmp(bar_text(3, a), "Collectables") == 0, "Unlocks: four bars, Weapons .. Collectables, on MAIN B");
	pframe(0x40);
	settle();
	check(current() == SCREEN(MS_CFG_A) && G.rows_drawn == 10 && !G.more_top &&
	      G.more_bottom == CFG_Y_ROW0 + 10 * CFG_DY && strcmp(G.title, "Weapons 1/2") == 0,
	      "Weapons: ten of sixteen rows, \"...\" under the last");
	for (i = 0; i < 12; i++) pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 12 && r8(MH_SHIM + SH_CTOP) == 3 && G.more_top && G.more_bottom &&
	      G.colour[9] == CFG_COLOR_CURSOR, "Down to the thirteenth: the page scrolled by three, \"...\" over and under");
	for (i = 0; i < 3; i++) pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 15 && r8(MH_SHIM + SH_CTOP) == 6 && G.more_top && !G.more_bottom &&
	      strcmp(G.lab[9], "More") == 0, "to the last, More: the last ten on screen");
	pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 0 && r8(MH_SHIM + SH_CTOP) == 0, "Down wraps to the first");

	/* Start on the CONFIG page closes the pause, once it is ten frames old. */
	n = G.screen_sounds;
	pframe(PADB_START);
	check(G.screen_sounds == n + 1 && r8(MH_SHIM + SH_RET) == 1, "Start: the close, with the root's closing sound");
	for (i = 0; i < 12 && G.paused; i++) pframe(0);
	check(!G.paused && r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peek32(R2_GAME_MODE) == 0, "the manager closes the pause, the menu down");
	tick(4);

	/* The combo again opens on the root (round 3: a close puts qwark's stack back); again closes it. */
	combo_press(COMBO_MENU_DEFAULT);
	play_frame(0);
	pframe(0);
	pframe(0);
	check(current() == SCREEN(MS_COMBO) && r8(MH_SHIM + SH_TYPE) == MBOX_PTYPE_MAIN && bars_are(root, 7) &&
	      r32(BARS + R2_R_CURSOR) == 0, "the combo opens the menu on its root, the cursor on Game");
	tick(4);
	for (i = 0; i < 10; i++) pframe(0);
	combo_press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with it up asks it to close");
	pframe(0);
	pframe(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0, "which its next frame does");
	tick(4);

	/* Where the root has no Help: a ship level, a hoverbike, a race, an orbit. */
	G.q_ship = 1;
	G.q_ship_on = 1;
	n = (int)r32(MH_STATE + ST_OPEN_REQS);
	combo_press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0 && (int)r32(MH_STATE + ST_OPEN_REQS) == n,
	      "in a ship level: the request taken and dropped, no pause");
	G.q_ship_on = 0;
	G.pstate = R2_PS_HOVERBIKE;
	combo_press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0, "on the hoverbike: the same");
	G.pstate = 0;
	G.q_orbit = 1;
	combo_press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(!G.paused, "in an orbit: the same");
	G.q_orbit = 0;
	G.q_ship = 0;
	G.gate_closed = 1;
	combo_press(COMBO_MENU_DEFAULT);
	play_frame(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0 && r8(MH_SHIM + SH_FORCE) == 0,
	      "where the game's gate refuses the pause: the request taken, nothing opens");
	G.gate_closed = 0;
	play_frame(1);
	pframe(0);
	check(G.paused && current() == R2_ROOT_SCREEN, "and the player's next Start is the game's own root");
	for (i = 0; i < 10; i++) pframe(0);
	pframe(PADB_START);
	check(!G.paused, "closed");

	check(quit(), "quit RaC2");
}

void test_menu_rac2(void)
{
	test_model_rac2();
	test_install_rac2();
	test_renderer_rac2();
}
