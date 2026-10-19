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
 *                 below do in C what they do;
 *   the picker    (round 4) the root's Planets row opening the renderer's
 *                 copy of the ship's planet select, against mocks of the
 *                 select's list update and its galaxy and picture items, the
 *                 pick to qwark and qwark's planet load of it.
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
#define PICK_SCREEN (MH_SCREENS + MS_PICK)
#define PICK_ITEM   (MH_SCREENS + MS_PICK_ITEM)
#define PICK_ROWS   (MH_SCREENS + MS_PICK_ROWS)

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
 * the real one), else a game text. The pickers' round 2: our ids sign-extended
 * as well (0xFFFFF0xx), as the planet list's draw passes a row's.
 */
static u32 game_msg(u32 id)
{
	if ((id & ~0x3Fu) == MH_MSG_BASE || (id & ~0x3Fu) == (0xFFFF0000u | MH_MSG_BASE)) {
		u32 i = id & 0x3F;
		if (i == 0) return MH_SCREENS + MS_TRAINER;
		if (i <= MH_MAIN_ROWS) return mh_text(i - 1);      /* the text set on show */
		if (i >= MH_MSG_PLANET - MH_MSG_BASE && i - (MH_MSG_PLANET - MH_MSG_BASE) < MH_PLANETS)
			return r32(MH_SCREENS + MS_PICK_NAMES + 4 * (i - (MH_MSG_PLANET - MH_MSG_BASE)));
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

/*
 * The planet select's pieces the picker reuses. 0xB4DEF8 shows a planet in
 * the galaxy and the picture (*0x157D110 = it, 27 at most); the PS2 list's
 * update 0xBBE528 as it runs for the select's list (flags 0x538): only for the
 * screen's focus; Start and Select show the current planet and close (-1);
 * Triangle shows it and goes to the parent, or closes with none; the cross does
 * the row's action (kind 3: the next screen); Up and Down move the cursor over
 * rows of a kind, without wrapping, with the tick, and show the planet of the
 * game's discovered list (0x1345AB4) at the cursor, which the test fills with
 * planets not in the picker's order.
 */
static struct {
	int selects, updates, confirms, galaxy[4], picture[4], list_draws;
	u32 discovered[28];
} P;

static int mock_select_planet(u32 p)
{
	w32(R2_SELECTED, p >= 28 ? 27 : p);
	P.selects++;
	return 1;
}

static int mock_list_update(u32 item)
{
	u32 e = r32(R2_PAD_EDGES), cur = r32(R2_PAUSE_MGR + R2_MGR_CURRENT), rows = r32(item + R2_LI_ROWS);
	u32 c = r32(item + R2_LI_CURSOR), n = 0;

	P.updates++;
	if (r32(cur + R2_SCR_FOCUS) != item) return 0;
	while (r16(rows + n * R2_ROW_SIZE) != 0) n++;
	if ((e & (PADB_START | PADB_SELECT)) != 0) {
		mock_select_planet(r32(R2_CURRENT_PLANET));
		return -1;
	}
	if ((e & PADB_TRIANGLE) != 0) {
		mock_select_planet(r32(R2_CURRENT_PLANET));
		if (r32(cur + R2_SCR_PARENT) == 0) return -1;
		w32(R2_PAUSE_MGR + R2_MGR_NEXT, r32(cur + R2_SCR_PARENT));
		return 0;
	}
	if ((e & r32(R2_CONFIRM_MASK)) != 0) {
		P.confirms++;
		if (r16(rows + c * R2_ROW_SIZE + 2) == 3) w32(R2_PAUSE_MGR + R2_MGR_NEXT, r32(rows + c * R2_ROW_SIZE + 4));
		return 0;
	}
	if ((e & PADB_UP) != 0 && c > 0) c--;
	else if ((e & PADB_DOWN) != 0 && c + 1 < n && r16(rows + (c + 1) * R2_ROW_SIZE + 2) != 0) c++;
	else return 0;
	w32(item + R2_LI_CURSOR, c);
	mock_play_sound(SND_MOVE, 0, 0);
	mock_select_planet(P.discovered[c]);
	return 0;
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

/*
 * The picker's items, as the manager calls each item of the current screen in
 * turn: the game's galaxy and picture (counted), and the shim's list, whose
 * draw is the game's (counted).
 */
static int call_pick_item(u32 item, int which)
{
	if (item == R2_PICK_GALAXY || item == R2_PICK_PICTURE) {
		(item == R2_PICK_GALAXY ? P.galaxy : P.picture)[which]++;
		return 0;
	}
	if (item != PICK_ITEM) return 0;
	switch (which) {
	case CALL_UPDATE: return mh_pick_update(item);
	case CALL_DRAW:   P.list_draws++; return 0;
	case CALL_ENTER:  return mh_pick_enter(item, 0);
	default:          return mh_pick_exit(item, 0);
	}
}

/* An item call through the screen's first item: the root's, or the shim's (by the item's address). */
static int call(u32 screen, int which)
{
	u32 item, k;
	int rc = 0;

	if (screen == R2_ROOT_SCREEN) {
		if (which == CALL_UPDATE) return mock_root_update();
		if (which == CALL_DRAW) G.root_draws++;
		return 0;
	}
	if (screen == PICK_SCREEN) {
		for (k = 0; k < 14; k++) {
			item = r32(screen + R2_SCR_ITEMS + 4 * k);
			if (item != 0 && call_pick_item(item, which) != 0) rc = 1;
		}
		return rc;
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
	w32(R2_SELECTED, r32(R2_CURRENT_PLANET));       /* PauseInit shows the current planet (0xBB7290) */
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
	/* Round 5: no Die, Health XP, Set respawn point or Store Swingshot; a spacer (""); the values on Cheats. */
	static const char * const game[] = { "Position slot", "Save position", "Load position", "",
	                                     "Set aside file", "Load set-aside file" };
	static const char * const cheats[] = { "Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health",
	                                       "Reset bosses on death", "Reset platinum bolts on death",
	                                       "Challenge mode", "Max bolts", "Max raritanium" };
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
	menu_model_reset(7, 32, CAPS_ALL);
	menu_model_view(&v);
	check(labels_are(&v, root, 7) && v.ptype == MBOX_PTYPE_MAIN && v.rows[6].kind == MROW_INFO,
	      "the root: Game, Cheats, Planets, Manips, Unlocks, Save files, and the build, grey: the design's, "
	      "seven rows, the root's own seven bars");

	act(&v, MBOX_EV_ACTIVATE, "Game");
	check(labels_are(&v, game, 6) && v.ptype == MBOX_PTYPE_CONFIG && v.rows[3].kind == MROW_INFO &&
	      v.rows[3].flags == 0,
	      "round 5, Game: the slot and the positions, a blank grey row, the save-file buttons");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	act(&v, MBOX_EV_ACTIVATE, "Cheats");
	check(labels_are(&v, cheats, 9) && v.ptype == MBOX_PTYPE_CONFIG,
	      "Cheats: the four cheats, the two resets on death, then (round 5) Challenge mode, Max bolts and Max "
	      "raritanium; no Set respawn point or Store Swingshot");
	check(act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet") == ST_OK && strcmp(value_of(&v, "Ghost Ratchet"), "On") == 0,
	      "Ghost Ratchet: the cross turns it On");
	poke8(RAC2_CHALLENGE_MODE, 0);
	tick(2);
	menu_model_view(&v);
	r = view_row(&v, "Challenge mode");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0 &&
	      act(&v, MBOX_EV_ACTIVATE, "Challenge mode") == ST_OK && peek8(RAC2_CHALLENGE_MODE) == 1 &&
	      strcmp(value_of(&v, "Challenge mode"), "On") == 0, "Challenge mode: a switch, Off at 0, the cross On at 1");
	act(&v, MBOX_EV_ACTIVATE, "Challenge mode");
	check(peek8(RAC2_CHALLENGE_MODE) == 0, "and Off again at 0");
	check(act(&v, MBOX_EV_ACTIVATE, "Max bolts") == ST_OK && peek32(RAC2_BOLTS) == 9999999u &&
	      act(&v, MBOX_EV_ACTIVATE, "Max raritanium") == ST_OK && peek32(RAC2_RARITANIUM) == 9999999u &&
	      value_of(&v, "Max raritanium")[0] == 0,
	      "Max bolts and Max raritanium: 9,999,999 each, the HUD's seven digits, the rows blank");
	r = view_row(&v, "Reset bosses on death");
	check(r != NULL && r->kind == MROW_TOGGLE, "the resets on death are checkboxes");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	act(&v, MBOX_EV_ACTIVATE, "Ghost Ratchet");
	check(view_row(&v, "Enable debug mode") == NULL, "debug mode, the client's Debug page, is left out");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Planets: the plain list in the ids' order, which is the game's, on a CONFIG page (round 4). */
	act(&v, MBOX_EV_ACTIVATE, "Planets");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 27 && strcmp(v.rows[0].label, "Aranos") == 0 &&
	      strcmp(v.rows[5].label, "Feltzin") == 0 && strcmp(v.rows[26].label, "Jamming Array") == 0 &&
	      (v.pflags & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) == 0,
	      "Planets: a CONFIG page of all 27, Aranos .. the Jamming Array, no More row");
	check((v.rows[0].flags & MROWF_CLOSES) != 0 && v.rows[0].kind == MROW_ACTION, "each a button that closes the menu first");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);

	/* Manips: no header in RaC2, so the category and its options. */
	act(&v, MBOX_EV_ACTIVATE, "Manips");
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 3 && strcmp(v.rows[0].label, "Category") == 0 &&
	      strcmp(value_of(&v, "Category"), "None") == 0 && strcmp(v.rows[1].label, "Auto-reset") == 0 &&
	      strcmp(v.rows[2].label, "Apply now") == 0,
	      "Manips: Category (None), Auto-reset, Apply now (round 5: no Remember row, and no spacer with nothing after)");
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
	check(v.ptype == MBOX_PTYPE_CONFIG && v.nrows == 24 && v.rows[23].kind == MROW_CHOICE &&
	      strcmp(v.title, "Weapons") == 0, "Weapons: all twenty-four on one page (round 4: no More, no \"1/2\")");
	poke8(RAC2_OWNED_ARRAY + R2_LANCER_ITEM, 0);
	poke8(RAC2_ITEM_ARRAY + R2_LANCER_ITEM, R2_LANCER_ITEM);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Lancer"), "Locked") == 0, "a locked weapon: Locked, in the game's title case");
	check(act(&v, MBOX_EV_RIGHT, "Lancer") == ST_OK && strcmp(value_of(&v, "Lancer"), "V1") == 0 &&
	      peek8(RAC2_OWNED_ARRAY + R2_LANCER_ITEM) == 1, "Right: V1, owned at v1 (round 5: it said Unlocked)");
	check(act(&v, MBOX_EV_RIGHT, "Lancer") == ST_OK && strcmp(value_of(&v, "Lancer"), "V4") == 0 &&
	      peek8(RAC2_ITEM_ARRAY + R2_LANCER_ITEM) == 80, "Right: V4, its last (its version item 80; it said Max)");
	check(act(&v, MBOX_EV_RIGHT, "Lancer") == ST_OK && strcmp(value_of(&v, "Lancer"), "Locked") == 0,
	      "Right wraps to Locked");
	poke8(RAC2_OWNED_ARRAY + R2_LANCER_ITEM, 1);
	poke8(RAC2_ITEM_ARRAY + R2_LANCER_ITEM, 60);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Lancer"), "V2") == 0, "a version between shows as itself, V2");
	/* Round 5: the Tesla Claw, item 18, stops at v2 (item 114): its own last (rac2_unlock_levels). */
	poke8(RAC2_OWNED_ARRAY + 18, 1);
	poke8(RAC2_ITEM_ARRAY + 18, 114);
	menu_model_view(&v);
	check(strcmp(value_of(&v, "Tesla Claw"), "V2") == 0, "the Tesla Claw at v2, its own last: V2");
	check(act(&v, MBOX_EV_RIGHT, "Tesla Claw") == ST_OK && strcmp(value_of(&v, "Tesla Claw"), "Locked") == 0 &&
	      peek8(RAC2_OWNED_ARRAY + 18) == 0,
	      "Right from its last wraps to Locked (round 4 asked for v4, held to v2, and never left it)");
	act(&v, MBOX_EV_LEFT, "Tesla Claw");
	check(strcmp(value_of(&v, "Tesla Claw"), "V2") == 0 && peek8(RAC2_ITEM_ARRAY + 18) == 114,
	      "Left from Locked: V2, its last");
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
		check(at_code < first_hook && n <= 16 && bytes <= 23u * 1024u,
		      "the caves before the words: fifteen writes, under 23 KB (round 4: 32 rows; the picker's screen, "
		      "rows and names)");
		printf("    (the install wrote %u times, %u bytes)\n", (unsigned)n, (unsigned)bytes);
	}
	mb = menu_mailbox();
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 32 &&
	      peek8(mb + MBOX_OFF_ROW_SIZE) == 0x40 && peek8(mb + MBOX_OFF_MAX_ROWS) == 7 &&
	      peek8(mb + MBOX_OFF_CFG_ROWS) == 32 && peek8(mb + MBOX_OFF_RENDERER) == 2,
	      "the mailbox (round 4): 32 rows of 0x40, seven a MAIN page, 32 a CONFIG page, renderer 2");
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
	check(G.rows_drawn == 9 && G.first_y == CFG_Y_ROW0 && strcmp(G.lab[0], "Fast Loads") == 0 &&
	      strcmp(G.val[0], "Off") == 0 && strcmp(G.lab[5], "Reset Platinum Bolts On Death") == 0 &&
	      strcmp(G.lab[6], "Challenge Mode") == 0 && strcmp(G.lab[7], "Max Bolts") == 0 &&
	      strcmp(G.lab[8], "Max Raritanium") == 0 && strcmp(G.val[8], "") == 0 &&
	      G.colour[0] == CFG_COLOR_CURSOR && G.colour[1] == CFG_COLOR_TEXT && !G.more_top && !G.more_bottom,
	      "nine rows from y 112, all in the frame, labels and values in title case, the cursor's row in Cheats' "
	      "highlight; round 5: Challenge Mode, Max Bolts and Max Raritanium last");
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
	for (i = 0; i < 6; i++) pframe(PADB_DOWN);
	seq = peek32(mb + MBOX_OFF_EV_SEQ);
	pframe(PADB_LEFT);
	check(r8(MH_SHIM + SH_CCUR) == 7 && strcmp(G.lab[7], "Max Bolts") == 0 && G.sound == SND_NO &&
	      peek32(mb + MBOX_OFF_EV_SEQ) == seq, "Left on Max Bolts, a button: Cheats' refusal, no event");

	/* Round 5: the cross on Max Bolts writes the bolts, and the row says nothing. */
	poke32(RAC2_BOLTS, 3);
	pframe(0x40);
	tick(2);
	pframe(0);
	check(peek32(RAC2_BOLTS) == 9999999u && strcmp(G.val[7], "") == 0, "the cross on Max Bolts: 9,999,999, no DONE");

	/* A label too long for its column is cut short before its value: the page refreshed after a cross. */
	G.char_w = 12;
	poke8(RAC2_CHALLENGE_MODE, 0);
	pframe(PADB_UP);
	pframe(0x40);
	tick(2);
	pframe(0);
	pframe(0);
	check(r32(MH_SHIM + SH_CUTS) >= 1 && strlen(G.lab[5]) < strlen("Reset Platinum Bolts On Death") &&
	      strcmp(G.lab[5] + strlen(G.lab[5]) - 2, "..") == 0 &&
	      (int)strlen(G.lab[5]) * 12 <= CFG_X_VALUE - 3 * 12 / 2 - CFG_GAP - CFG_X_LABEL &&
	      peek8(RAC2_CHALLENGE_MODE) == 1 && strcmp(G.val[6], "On") == 0,
	      "wider glyphs: Reset Platinum Bolts On Death cut short with \"..\", ending before its value (the page "
	      "refreshed by Challenge Mode On)");
	G.char_w = 8;

	/* Back: the root on the bars again, on the next MAIN screen, the cursor on Cheats. */
	pframe(PADB_TRIANGLE);
	tick(2);
	pframe(0);
	pframe(0);
	pframe(0);
	check(current() == SCREEN(MS_MAIN_A) && bars_are(root, 7) && r32(BARS + R2_R_CURSOR) == 1,
	      "Triangle: MAIN A, the root's bars, the cursor on Cheats");

	/*
	 * Game (round 5): six rows, all on screen; the spacer a blank line in the
	 * grey of a row the cursor skips, which Up and Down pass over (cfg_step).
	 */
	w32(BARS + R2_R_CURSOR, 0);
	pframe(0x40);
	settle();
	check(current() == SCREEN(MS_CFG_A) && G.rows_drawn == 6 && strcmp(G.lab[2], "Load Position") == 0 &&
	      strcmp(G.lab[3], "") == 0 && strcmp(G.val[3], "") == 0 && G.colour[3] == CFG_COLOR_GREY &&
	      strcmp(G.lab[4], "Set Aside File") == 0 && !G.more_top && !G.more_bottom,
	      "Game: six rows, the fourth blank and grey, nothing scrolled");
	pframe(PADB_DOWN);
	pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 2, "Down twice: Load Position");
	pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 4 && G.sound == SND_MOVE && G.colour[4] == CFG_COLOR_CURSOR,
	      "Down: past the spacer to Set Aside File, the tick");
	pframe(PADB_UP);
	check(r8(MH_SHIM + SH_CCUR) == 2, "Up: past it back to Load Position");
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
	check(current() == SCREEN(MS_CFG_A) && G.rows_drawn == 8 && !G.more_top &&
	      G.more_bottom == CFG_Y_ROW0 + 8 * CFG_DY && strcmp(G.title, "Weapons") == 0 && mh_nrows() == 24,
	      "Weapons: all twenty-four on the page, eight on screen, \"...\" under the last (round 4: no More)");
	for (i = 0; i < 12; i++) pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 12 && r8(MH_SHIM + SH_CTOP) == 5 && G.more_top && G.more_bottom &&
	      G.colour[7] == CFG_COLOR_CURSOR, "Down to the thirteenth: the page scrolled by five, \"...\" over and under");
	for (i = 0; i < 11; i++) pframe(PADB_DOWN);
	check(r8(MH_SHIM + SH_CCUR) == 23 && r8(MH_SHIM + SH_CTOP) == 16 && G.more_top && !G.more_bottom &&
	      strcmp(G.lab[7], "Clank Zapper") == 0, "to the last weapon, the Clank Zapper: the last eight on screen");
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

/* ------------------------------------------------------------ the picker */

extern const struct game_api rac2_game;

static u16 peek16(u32 a) { u8 b[2]; host_peek(a, b, 2); return (u16)((b[0] << 8) | b[1]); }

/* The picker's name of planet row i: the line under the game's location, or the row's only line. */
/*
 * Row i's name line as the list's draw (0xBCBC58) asks msg_string for it: the
 * halfword sign-extended (lhzx and extsh), so our 0xF010 + p comes as
 * 0xFFFFF010 + p, which the label hook takes since the pickers' round 2
 * (round 1 sent it on into msg_string, whose answer was "(unavailable)").
 */
static const char *pick_name(u32 i, char *out)
{
	u32 row = PICK_ROWS + i * R2_ROW_SIZE, text = r16(row), sub = r16(row + 8);
	return read_str(game_msg((u32)(s32)(s16)(u16)(sub != 0 ? sub : text)), out, 80);
}

/* Frames of the pause with nothing pressed. */
static void pframes(int n)
{
	int i;
	for (i = 0; i < n; i++) pframe(0);
}

/* Start, then Trainer: the menu's root from the Help bar, ten frames old. */
static void open_from_help(void)
{
	play_frame(1);
	pframe(0);
	w32(BARS + R2_R_CURSOR, 4);
	pframe(0x40);
	pframes(12);
}

/* The combo: the menu's root, ten frames old. */
static void open_from_combo(void)
{
	combo_press(COMBO_MENU_DEFAULT);
	play_frame(0);
	pframes(12);
}

/* The cross on the root's Planets bar, and the frames until the picker has entered. */
static void open_picker(void)
{
	w32(BARS + R2_R_CURSOR, 2);
	pframe(0x40);
	pframes(2);
}

static void test_picker_rac2(void)
{
	static const char * const root[] = { "Game", "Cheats", "Planets", "Manips", "Unlocks", "Save Files",
	                                     "Qwark Build 54" };
	static const char * const game_bars[] = { "Weapons", "Gadgets", "Map", "Goodies", "Trainer", "Options",
	                                          "Special" };
	const char * const *names;
	struct menu_view v;
	char a[80];
	u32 mb, seq, frames, i, good;
	u8 count = 0;
	int n;

	group("console menu: RaC2's planet picker, the ship's planet select from the Planets row");

	memset(&G, 0, sizeof(G));
	memset(&P, 0, sizeof(P));
	G.char_w = 8;
	for (i = 0; i < 28; i++) P.discovered[i] = (i * 7 + 5) % 27;   /* the game's own list: another order */
	check(boot("NPEA00386"), "RaC2 boots");
	rac2_playing();
	seed_game();
	poke32(R2_CURRENT_PLANET, 3);                                     /* Endako */
	/* Words of the game's own planet select, which nothing of the picker may change. */
	poke32(R2_PICK_SCREEN + R2_SCR_PARENT, 0);
	poke32(R2_PICK_LIST + R2_LI_ROWS, 0x015D167Cu);
	poke32(R2_PICK_LIST + R2_LI_CURSOR, 5);
	poke32(0x01345AB4u, 0x01481BD0u);
	for (n = 0; n < 1000 && menu_state() != MENU_READY; n++) tick(1);
	tick(2);
	check(menu_state() == MENU_READY, "the renderer is in");
	mb = menu_mailbox();

	/* The model: the root's Planets row opens the renderer's picker. */
	check((peek8(mb + MBOX_OFF_CAPS) & MBOX_CAP_PICKER) != 0 && peek32(mb + MBOX_OFF_PICK_SEQ) == 0 &&
	      peek16(mb + MBOX_OFF_SIZE) == MH_MBOX_SIZE,
	      "the mailbox says the renderer has a picker (the pick in the header, the block as before), no pick yet");
	menu_model_view(&v);
	check(view_row(&v, "Planets") != NULL && (view_row(&v, "Planets")->flags & MROWF_PICKER) != 0 &&
	      (view_row(&v, "Manips")->flags & MROWF_PICKER) == 0 &&
	      (peek8(mb + MBOX_OFF_ROWS + 2 * 0x40 + MROW_OFF_FLAGS) & MROWF_PICKER) != 0,
	      "the root's Planets row, and only it, flagged MROWF_PICKER in the page posted");

	/* The picker's data: every planet of PLANET_LIST, in its order, by its names. */
	names = rac2_game.planet_names(&count);
	good = count == MH_PLANETS && r16(PICK_ROWS + MH_PLANETS * R2_ROW_SIZE) == 0;
	for (i = 0; good && i < count; i++)
		good = strcmp(pick_name(i, a), names[i]) == 0 && r16(PICK_ROWS + i * R2_ROW_SIZE + 2) == R2_ROW_INERT &&
		       r32(PICK_ROWS + i * R2_ROW_SIZE + 4) == 0;
	check(good, "the picker's rows: the 27 planets of PLANET_LIST in qwark's order, by qwark's names (asked for "
	      "sign-extended, as the list's draw asks), a kind the game's row actions ignore, then a zero row");
	good = 1;
	for (i = 0; i < count; i++) {
		u32 row = PICK_ROWS + i * R2_ROW_SIZE, id = r16(row + 8) != 0 ? r16(row + 8) : r16(row);
		good = good && id >= 0x8000 && game_msg(id) == game_msg(0xFFFF0000u | id) &&
		       strcmp(read_str(game_msg(0xFFFF0000u | id), a, 80), names[i]) == 0;
	}
	check(good, "every row's name id is a halfword past 0x7FFF, which the draw makes negative: the hook answers "
	      "both forms alike, so no row says \"(unavailable)\" (the user's photo, round 1)");
	check(r16(PICK_ROWS + 1 * R2_ROW_SIZE) == 0xB2C && r16(PICK_ROWS + 1 * R2_ROW_SIZE + 8) == MH_MSG_PLANET + 1 &&
	      r16(PICK_ROWS + 21 * R2_ROW_SIZE) == MH_MSG_PLANET + 21 && r16(PICK_ROWS + 21 * R2_ROW_SIZE + 8) == 0 &&
	      r16(PICK_ROWS + 25 * R2_ROW_SIZE) == MH_MSG_PLANET + 25,
	      "Oozla's row the game's location (Megacorp Outlet) over \"Oozla\"; the Museum and Wupash their name alone");
	check(r32(PICK_SCREEN + R2_SCR_FOCUS) == PICK_ITEM && r32(PICK_SCREEN + R2_SCR_ITEMS) == R2_PICK_GALAXY &&
	      r32(PICK_SCREEN + R2_SCR_ITEMS + 4) == PICK_ITEM && r32(PICK_SCREEN + R2_SCR_ITEMS + 16) == R2_PICK_PICTURE &&
	      r32(PICK_SCREEN + 0xE4) == R2_PICK_ID && r32(PICK_ITEM + 4) == R2_LIST_DRAW &&
	      r32(PICK_ITEM + R2_LI_FLAGS) == R2_LIST_FLAGS && r32(PICK_ITEM + R2_LI_ROWS) == PICK_ROWS,
	      "the picker's screen: id 15, the game's galaxy and picture items, the shim's list (the game's draw, its flags)");

	/* Start, Trainer: the menu's root. */
	open_from_help();
	check(current() == SCREEN(MS_HELP) && bars_are(root, 7), "Start, Trainer: the menu's root on the bars");

	/* The cross on Planets: the picker, with the game's screen sound; no event for qwark. */
	w32(BARS + R2_R_CURSOR, 2);
	seq = peek32(mb + MBOX_OFF_EV_SEQ);
	n = G.screen_sounds;
	pframe(0x40);
	check(G.state == 3 && r32(PM + R2_MGR_NEXT) == PICK_SCREEN && G.screen_sounds == n + 1 &&
	      peek32(mb + MBOX_OFF_EV_SEQ) == seq && r8(MH_STATE + ST_PICKER) == 1 && r8(MH_STATE + ST_SWITCHING) == 0 &&
	      r32(PICK_SCREEN + R2_SCR_PARENT) == SCREEN(MS_HELP) && r32(MH_SHIM + SH_PICK_OPENS) == 1,
	      "the cross on Planets: the picker next, its parent the menu's screen, the screen sound, no event");
	check(r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER &&
	      r8(MH_STATE + ST_LAST_CURSOR) == 2,
	      "the menu's screen stopped, but no close: hstate PICKER for qwark, the cursor kept");
	n = G.main_draws;
	pframes(2);
	check(current() == PICK_SCREEN && P.galaxy[CALL_ENTER] == 1 && P.picture[CALL_ENTER] == 1 &&
	      r32(PICK_ITEM + R2_LI_CURSOR) == 3 && (r32(PICK_ITEM + R2_LI_FLAGS) & R2_LI_FRESH) != 0 &&
	      P.list_draws >= 1 && P.galaxy[CALL_DRAW] >= 1 && P.picture[CALL_UPDATE] >= 1 && G.main_draws == n,
	      "the picker: the game's galaxy and picture enter, the list's cursor on Endako, the planet shown");
	tick(4);
	check(peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_PICKER && menu_model_page() == MENU_PAGE_ID(MENU_PAGE_ROOT, 0),
	      "qwark keeps the menu where it was under the picker");

	/* Down: Barlow, the game's tick, and Barlow shown, not the discovered list's planet. */
	n = P.selects;
	pframe(PADB_DOWN);
	check(r32(PICK_ITEM + R2_LI_CURSOR) == 4 && G.sound == SND_MOVE && r32(R2_SELECTED) == 4 && P.selects == n + 2,
	      "Down: the cursor on Barlow with the tick; the galaxy and picture show Barlow");
	pframe(PADB_UP);
	check(r32(PICK_ITEM + R2_LI_CURSOR) == 3 && r32(R2_SELECTED) == 3, "Up: Endako again");

	/* Triangle: the menu's root again, on Planets, the same opening. */
	frames = peek32(mb + MBOX_OFF_FRAMES);
	pframe(PADB_DOWN);
	pframe(PADB_TRIANGLE);
	check(r32(PM + R2_MGR_NEXT) == SCREEN(MS_HELP) && r32(R2_SELECTED) == 3 && P.picture[CALL_EXIT] == 1,
	      "Triangle: back to the menu's screen, the current planet shown again, the picture's exit");
	pframes(2);
	check(current() == SCREEN(MS_HELP) && bars_are(root, 7) && r32(BARS + R2_R_CURSOR) == 2 &&
	      r8(MH_STATE + ST_PICKER) == 0 && r8(MH_SHIM + SH_FROM_HELP) == 1 &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && peek32(mb + MBOX_OFF_FRAMES) > frames,
	      "the menu's root again, the cursor on Planets, still from Help, the frames going on (no new opening)");
	tick(4);
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_ROOT, 0) && r32(BARS + R2_R_CURSOR) == 2,
	      "qwark's root, the cursor where it was");

	/* The cross on Planets again, then on Aranos 2: the pick. */
	open_picker();
	check(current() == PICK_SCREEN && r32(PICK_ITEM + R2_LI_CURSOR) == 3, "the picker again, on Endako");
	for (i = 0; i < 11; i++) pframe(PADB_DOWN);
	n = P.confirms;
	pframe(0x40);
	check(G.sound == SND_CHANGE && P.confirms == n && peek32(mb + MBOX_OFF_PICK_PLANET) == 14 &&
	      peek32(mb + MBOX_OFF_PICK_SEQ) == 1 && r32(MH_SHIM + SH_PICKS) == 1 && r32(MH_SHIM + SH_PICK_LAST) == 14,
	      "the cross on Aranos 2: planet 14 to qwark, seq 1, the planet page's sound; the game's list never sees it");
	check(!G.paused && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && r32(MH_SHIM + SH_UP) == 0 &&
	      r8(MH_STATE + ST_PICKER) == 0 && peek32(R2_GAME_MODE) == 0,
	      "the pause closes: the menu down, its words free to come out");
	poke32(RAC2_LOAD_PLANET, 0);
	poke32(RAC2_LOAD_PLANET + 4, 0);
	tick(4);
	check(peek32(RAC2_LOAD_PLANET) == 1 && peek32(RAC2_LOAD_PLANET + 4) == 14,
	      "qwark loads Aranos 2 through its planet load once the game plays");
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_ROOT, 0), "and the menu's next opening is its root");
	poke32(RAC2_LOAD_PLANET, 0);

	/* From the combo: the picker, Start: everything closes, nothing picked. */
	open_from_combo();
	open_picker();
	check(current() == PICK_SCREEN && r32(PICK_SCREEN + R2_SCR_PARENT) == SCREEN(MS_COMBO),
	      "the combo's root, Planets: the picker, its parent the combo's screen");
	pframe(PADB_START);
	check(!G.paused && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && r32(MH_SHIM + SH_UP) == 0 &&
	      peek32(mb + MBOX_OFF_PICK_SEQ) == 1 && r32(R2_SELECTED) == 3,
	      "Start in the picker closes the pause, nothing picked");
	tick(4);
	check(peek32(RAC2_LOAD_PLANET) == 0, "and nothing loads");

	/* The combo while the picker is up closes it, as the menu's own screens do. */
	open_from_combo();
	open_picker();
	check(current() == PICK_SCREEN, "the picker again from the combo's root");
	tick(4);
	combo_press(COMBO_MENU_DEFAULT);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with the picker up asks for the close");
	pframe(0);
	check(!G.paused && peek8(mb + MBOX_OFF_REQUEST) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peek32(mb + MBOX_OFF_PICK_SEQ) == 1,
	      "which the picker's next frame takes: the pause closes, nothing picked");
	tick(4);

	/* Back from a picker the combo opened, then Triangle at the root: out of the pause, as from the combo. */
	open_from_combo();
	open_picker();
	pframe(PADB_TRIANGLE);
	pframes(2);
	check(current() == SCREEN(MS_COMBO) && r8(MH_SHIM + SH_FROM_HELP) == 0 && r32(BARS + R2_R_CURSOR) == 2,
	      "back from the picker to the combo's root, on Planets");
	pframe(PADB_TRIANGLE);
	for (i = 0; i < 12 && G.paused; i++) pframe(0);
	check(!G.paused && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED, "Triangle there leaves the pause");
	tick(4);

	/* Back from a picker the Help bar opened, then Triangle at the root: the game's root, on Trainer. */
	open_from_help();
	open_picker();
	pframe(PADB_TRIANGLE);
	pframes(2);
	pframe(PADB_TRIANGLE);
	pframe(0);
	check(current() == R2_ROOT_SCREEN && bars_are(game_bars, 7) && r32(BARS + R2_R_CURSOR) == 4,
	      "Help, Planets, Triangle, Triangle: the game's own root, on Trainer");
	pframes(10);
	pframe(PADB_START);
	check(!G.paused, "closed");
	tick(4);

	/* Nothing of the game's own planet select changed. */
	check(peek32(R2_PICK_SCREEN + R2_SCR_PARENT) == 0 && peek32(R2_PICK_LIST + R2_LI_ROWS) == 0x015D167Cu &&
	      peek32(R2_PICK_LIST + R2_LI_CURSOR) == 5 && peek32(0x01345AB4u) == 0x01481BD0u,
	      "the game's own select untouched: screen 15's parent, its list's rows and cursor, the discovered list's word");

	/* Without the cap the Planets row is a plain link. */
	menu_model_reset(7, 16, MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST);
	menu_model_view(&v);
	check(view_row(&v, "Planets") != NULL && (view_row(&v, "Planets")->flags & MROWF_PICKER) == 0,
	      "a renderer without MBOX_CAP_PICKER: Planets is qwark's own page, no flag");

	check(quit(), "quit RaC2");
}

void test_menu_rac2(void)
{
	test_model_rac2();
	test_install_rac2();
	test_renderer_rac2();
	test_picker_rac2();
}
