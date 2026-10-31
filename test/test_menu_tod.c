/*
 * The console menu in Tools of Destruction (2026-10-31, a prototype), on
 * qwark's fake console, against the real core: as test_menu_rac2.c has it for
 * RaC2.
 *
 *   the model     ToD's pages, built from its menu table (src/games/tod_game.c):
 *                 Game, Cheats with its two buttons, Unlocks with the weapons'
 *                 Locked, V1 and V10 and its Collectables;
 *   the install   only with config.txt's menu.tod = 1: ui.pak's kind 3 entry,
 *                 the mailbox, the state and the code, then the two words, a
 *                 few seconds into the session; none with the key at 0, and
 *                 the combo then says why;
 *   the renderer  the shared core and ToD's shim (mh_tod.c) compiled for the
 *                 PC (MH_HOST), against mocks of the HUD's Text_Draw and
 *                 Sprite_Draw. The stubs are assembler and do not run here:
 *                 check.py looks at them, and frame() below does what the two
 *                 do (the pads' read, then the frame's end).
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/mem.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/games/tod.h"
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

#include "../src/games/menuhelper/mh_tod.h"
#include "../src/games/menuhelper/mh_common.h"
#include "../src/games/menuhelper/mh_shim.h"

/* What the HUD drew in the last frame. */
#define MAX_TEXTS 40
static struct {
	int texts, boxes, resets, font, inits;
	char text[MAX_TEXTS][80];
	u32 colour[MAX_TEXTS];
	float x[MAX_TEXTS], y[MAX_TEXTS], align[MAX_TEXTS], size[MAX_TEXTS];
	u32 box_colour[8];
	float box[8][4];
} D;

static void read_str(u32 a, char *out, u32 cap)
{
	u32 n = 0;
	while (n + 1 < cap) {
		u8 c = (u8)r8(a + n);
		if (c == 0) break;
		out[n++] = (char)c;
	}
	out[n] = 0;
}

static void mock_text_draw(u32 str, u32 pos, u32 r5, u32 colour, u32 r7, u32 font, u32 r9, u32 r10, u32 shadow,
                           float size, float wrap, float ax, float ay, float sdx, float sdy, float lsp)
{
	(void)r5; (void)r7; (void)r9; (void)r10; (void)shadow; (void)wrap; (void)ay; (void)sdx; (void)sdy; (void)lsp;
	if (D.texts >= MAX_TEXTS) return;
	read_str(str, D.text[D.texts], sizeof(D.text[0]));
	D.colour[D.texts] = colour;
	D.x[D.texts] = rf32(pos);
	D.y[D.texts] = rf32(pos + 4);
	D.align[D.texts] = ax;
	D.size[D.texts] = size;
	D.font = (int)font;
	D.texts++;
}

/* Sprite_Init as the game's (0x682D30): white corners, alpha 1, blend 0x400. */
static void mock_sprite_init(u32 s)
{
	u32 k;
	for (k = 0; k < SPR_SIZE; k += 4) w32(s + k, 0);
	for (k = 0; k < 16; k += 4) w32(s + SPR_COLOURS + k, 0xFFFFFFFFu);
	wf32(s + 0x34, 1.0f);
	w32(s + 0x38, 0x400);
	D.inits++;
}

static void mock_sprite_draw(u32 rect, u32 s)
{
	if (D.boxes < 8) {
		u32 k;
		D.box_colour[D.boxes] = r32(s + SPR_COLOURS);
		for (k = 0; k < 4; k++) D.box[D.boxes][k] = rf32(rect + 4 * k);
	}
	D.boxes++;
}

static void mock_hud_reset_alpha(void) { D.resets++; }
static void mock_font_select(int font, int b) { (void)b; D.font = font; }
static float mock_font_height(void) { return 1.25f; }

#include "../src/games/menuhelper/mh_core.c"
#include "../src/games/menuhelper/mh_tod.c"

/* ---------------------------------------------------------------- helpers */

/*
 * The menu combo's default, L1 + R1 + L2 + R2 + L3 + R3, as the fake pad
 * holds it: in ToD's own bit order (REPORT-tod-lua section 7), which the
 * session remaps to the OG layout the combos are kept in.
 */
#define TOD_COMBO_MENU (0x40u | 0x80u | 0x10u | 0x20u | 0x400u | 0x800u)

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static u32 peek32(u32 a) { u8 b[4]; host_peek(a, b, 4); return be32_get(b); }
static u8 peek8(u32 a) { u8 b = 0; host_peek(a, &b, 1); return b; }
static void poke32(u32 a, u32 v) { u8 b[4]; be32_put(b, v); host_poke(a, b, 4); }
static void pokef(u32 a, float f) { union { float f; u32 u; } x; x.f = f; poke32(a, x.u); }
static float peekf(u32 a) { union { float f; u32 u; } x; x.u = peek32(a); return x.f; }

static int boot_tod(void)
{
	int i;

	host_boot_as(TOD_TITLE, "tod");
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME && session_game() != NULL) return 1;
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

/*
 * A frame of the game: Pad_Update with `held` on pad 0 (its tail, mh_pad),
 * then the frame's end (mh_frame, when the stub would call it: the menu up or
 * a request waiting). The pad's block as Pad_Update leaves it: the held mask
 * and its edges; a byte of pad 1 to see it blanked too.
 */
static void frame(u32 held)
{
	u32 prev = peek32(TM_PAD0 + TM_PAD_HELD);

	poke32(TM_PAD0 + TM_PAD_HELD + 4, prev);
	poke32(TM_PAD0 + TM_PAD_HELD, held);
	poke32(TM_PAD0 + TM_PAD_PRESSED, (prev ^ held) & held);
	poke32(TM_PAD0 + TM_PAD_RELEASED, (prev ^ held) & ~held);
	pokef(TM_PAD0, 0.5f);
	poke32(TM_PAD0 + TM_PAD_STRIDE + TM_PAD_HELD, held);
	mh_pad();
	memset(&D, 0, sizeof(D));
	if (r32(MH_SHIM + SH_UP) != 0 || r8(MH_BOX + MBOX_OFF_REQUEST) != 0) mh_frame();
}

/* A press: down one frame, up the next, qwark's ticks between. */
static void press(u32 b)
{
	frame(b);
	tick(2);
	frame(0);
	tick(2);
	frame(0);
}

static int drew(const char *s)
{
	int i;
	for (i = 0; i < D.texts; i++) {
		if (strcmp(D.text[i], s) == 0) return i;
	}
	return -1;
}

static void tod_playing(void)
{
	poke32(TM_STATE, TM_STATE_PLAY);
	poke32(TM_PENDING, 0);
	pokef(TM_SCREEN_RECT, 0.0f);
	pokef(TM_SCREEN_RECT + 4, 0.0f);
	pokef(TM_SCREEN_RECT + 8, 1280.0f);
	pokef(TM_SCREEN_RECT + 12, 720.0f);
	pokef(TM_CLOCK, 1.0f);
}

static const struct menu_row *view_row(const struct menu_view *v, const char *label)
{
	u8 i;
	for (i = 0; i < v->nrows; i++) {
		if (strcmp(v->rows[i].label, label) == 0) return &v->rows[i];
	}
	return NULL;
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

/* -------------------------------------------------------------- the model */

static void test_model_tod(void)
{
	static const char * const game[] = { "Position slot", "Save position", "Load position", "Die",
	                                     "Challenge mode", "Set aside file", "Load set-aside file" };
	static const char * const cheats[] = { "Fast loads", "Remove cutscenes", "Infinite ammo", "Infinite health",
	                                       "God Ratchet", "Max bolts", "Max raritanium" };
	struct menu_view v;
	const struct menu_row *r;

	group("console menu: ToD's pages");

	check(boot_tod(), "ToD boots");
	check(session_game() != NULL && session_game()->menu != NULL, "ToD has a menu table");
	menu_model_reset(MH_MAIN_ROWS, MH_CFG_ROWS, MH_CAPS);
	menu_model_view(&v);
	check(view_row(&v, "Game") != NULL && view_row(&v, "Cheats") != NULL && view_row(&v, "Planets") != NULL &&
	      view_row(&v, "Unlocks") != NULL, "the root: Game, Cheats, Planets, Unlocks among its rows");
	check(menu_model_event(MBOX_EV_ACTIVATE, 0, v.rows[0].id, v.key) == ST_OK, "Game opens");
	menu_model_view(&v);
	check(v.ptype == MBOX_PTYPE_CONFIG && labels_are(&v, game, 7), "Game: the slot, the positions, Die, "
	      "Challenge mode, the set-aside pair, a CONFIG page");
	menu_model_event(MBOX_EV_BACK, 0, v.rows[0].id, v.key);
	menu_model_view(&v);
	menu_model_event(MBOX_EV_ACTIVATE, 1, v.rows[1].id, v.key);
	menu_model_view(&v);
	check(v.ptype == MBOX_PTYPE_CONFIG && labels_are(&v, cheats, 7), "Cheats: the five toggles, then the two buttons");
	r = view_row(&v, "Infinite ammo");
	check(r != NULL && r->kind == MROW_TOGGLE && strcmp(r->value, "Off") == 0, "Infinite ammo, a toggle, Off");
	check(quit(), "quit ToD");
}

/* ----------------------------------------------------- the install, the key */

static void test_install_tod(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_TOD);
	u32 mb;
	int i;

	group("console menu: ToD's install (menu.tod)");

	check(d != NULL && d->mailbox == MH_BOX && d->state == MH_STATE && d->nhooks == 2 && d->nhelp == 0 &&
	      d->ncaves == 4, "ToD has a renderer: the mailbox, the state, the read-only data, the code, two words, "
	      "no Help row");
	if (d == NULL) return;
	check(d->hooks[0].addr == MH_SITE_FRAME && d->hooks[0].original == MH_ORIG_FRAME &&
	      d->hooks[1].addr == MH_SITE_PAD && d->hooks[1].original == MH_ORIG_PAD,
	      "the frame's end first, then the pads' read, each the game's own bl");
	check(d->mode_addr == MH_SHIM + SH_UP && d->mode_paused == 1 && d->pending_addr == TM_PENDING &&
	      d->pending_none == 0 && d->screen_addr == MH_SHIM + SH_UP && d->screen_id == 1,
	      "down when the shim's word says so and the game's state stack has nothing queued");

	/* menu.tod unset: nothing goes in, and the combo says why. */
	config_set("menu.tod", "0");
	check(boot_tod(), "ToD boots");
	tod_playing();
	tick(600);
	check(menu_state() == MENU_OFF && hooks_are(d, 0) && peek32(MH_BOX) == 0,
	      "menu.tod = 0: five seconds in, nothing of the menu");
	{
		u32 n = host_notify_count();
		combo_press(TOD_COMBO_MENU);
		check(host_notify_count() == n + 1 && strstr(host_notify_last(), "menu.tod") != NULL,
		      "the combo says the menu is a prototype that menu.tod turns on");
	}
	check(quit(), "quit ToD");

	/* menu.tod = 1: in a few seconds into play, the caves before the words. */
	config_set("menu.tod", "1");
	check(boot_tod(), "ToD boots again");
	tod_playing();
	poke32(TM_PENDING, 1);
	tick(480);
	check(menu_state() != MENU_READY && hooks_are(d, 0), "a state change queued: not yet");
	poke32(TM_PENDING, 0);
	for (i = 0; i < 100 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "nothing queued: the renderer goes in, both words");
	mb = menu_mailbox();
	{
		u8 info[256];
		session_info_copy(info, sizeof(info));
		check((info[29] & SESSION_FLAG2_NO_MENU) == 0, "menu.tod = 1: flags2 NO_MENU clear, ToD has a menu now");
	}
	check(mb == MH_BOX && peek32(mb) == MBOX_MAGIC && peek8(mb + MBOX_OFF_ROW_CAP) == 14 &&
	      peek8(mb + MBOX_OFF_MAX_ROWS) == 14 && peek8(mb + MBOX_OFF_CFG_ROWS) == 14 &&
	      peek8(mb + MBOX_OFF_RENDERER) == GAME_TOD && peek32(MH_STATE) == MH_MAGIC,
	      "the mailbox: fourteen rows a page of either type, renderer 5; the state's magic");
	check(peek32(mb + MBOX_OFF_PAGE_SEQ) != 0 && (peek32(mb + MBOX_OFF_PAGE_SEQ) & 1) == 0,
	      "the root already posted");

	/* menu.enabled = 0 takes the words out while the menu is down. */
	config_set_u32("menu.enabled", 0);
	tick(8);
	check(menu_state() == MENU_IDLE && hooks_are(d, 0), "menu.enabled = 0: both words back");
	config_set_u32("menu.enabled", 1);
	for (i = 0; i < 1500 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY && hooks_are(d, 1), "menu.enabled = 1: back in");
	check(quit(), "quit ToD");
	config_set("menu.tod", "0");
}

/* ------------------------------------------------------------ the renderer */

static void test_renderer_tod(void)
{
	static const char * const cheats[] = { "Fast Loads", "Remove Cutscenes", "Infinite Ammo" };
	u32 mb;
	int i, k;
	char a[80];

	group("console menu: ToD's renderer, core and shim, on the fake console");

	config_set("menu.tod", "1");
	check(boot_tod(), "ToD boots");
	tod_playing();
	for (i = 0; i < 1500 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY, "the renderer is in");
	mb = menu_mailbox();

	/* Frames with the menu down: nothing drawn, the pad the game's. */
	frame(PADB_CROSS);
	check(D.texts == 0 && D.boxes == 0 && peek32(TM_PAD0 + TM_PAD_HELD) == PADB_CROSS,
	      "the menu down: the stub calls nothing, the pad goes to the game as it is");
	frame(0);

	/* The combo in a cinematic: the request is left, never taken, and qwark withdraws it. */
	poke32(TM_STATE, 9);
	combo_press(TOD_COMBO_MENU);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "the combo: an open request");
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "in a cinematic (state 9) the frame leaves it");
	tick(80);
	check(peek8(mb + MBOX_OFF_REQUEST) == 0, "and qwark withdraws it half a second on");
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 0, "so the menu never opens later by itself");

	/* In play: the combo opens the root, play freezes, the pad is the menu's. */
	poke32(TM_STATE, TM_STATE_PLAY);
	pokef(TM_CLOCK, 0.75f);
	combo_press(TOD_COMBO_MENU);
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 1 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN &&
	      peek8(mb + MBOX_OFF_REQUEST) == 0 && r32(MH_STATE + ST_OPENS) == 1,
	      "in play the next frame opens the menu, the request taken");
	check(peekf(TM_CLOCK) == 0.0f && r8(MH_SHIM + SH_FROZEN) == 1 && rf32(MH_SHIM + SH_SCALE) == 0.75f,
	      "play frozen: the game clock's time scale 0, the game's 0.75 kept");
	check(drew("Trainer") >= 0 && D.colour[drew("Trainer")] == COL_TITLE && D.align[drew("Trainer")] == 0.5f &&
	      drew("Game") >= 0 && drew("Cheats") >= 0 && drew(">") >= 0 && drew("X Select   Triangle Back   Start Close") >= 0,
	      "drawn: the title Trainer, centred, in gold; the root's links with \">\"; the controls' line");
	check(D.boxes == 2 && D.box_colour[0] == COL_PANEL && D.box_colour[1] == COL_CURSOR && D.resets == 1 &&
	      D.font == TM_FONT && D.box[0][0] == 256.0f && D.box[0][2] == 1024.0f && D.box[0][1] == 108.0f,
	      "the panel from 20% to 80% of 1280 at 15% of 720, then the cursor's bar; the HUD's alpha reset; font 1");
	k = drew("Game");
	check(k >= 0 && D.size[k] > 16.1f && D.size[k] < 16.2f && D.colour[k] == COL_TEXT && D.align[k] == 0.0f,
	      "a row: 2.8% of the height over the font's height (1.25), left aligned, white");
	frame(PADB_SQUARE);
	check(peek32(TM_PAD0 + TM_PAD_HELD) == PADB_SQUARE && peek32(TM_PAD0 + TM_PAD_PRESSED) == 0 &&
	      peekf(TM_PAD0) == 0.0f && peek32(TM_PAD0 + TM_PAD_STRIDE + TM_PAD_HELD) == 0,
	      "the pad's read with the menu up: the edges and the sticks blanked for the game, every pad; the held "
	      "mask back at the frame's end, as the next read and qwark's combos want it");
	check(r32(MH_SHIM + SH_UP) == 1, "Square does nothing in the menu: still up");
	frame(0);

	/* Down to Cheats and in. */
	press(PADB_DOWN);
	check(r8(MH_SHIM + SH_CURSOR) == 1, "Down: the cursor on Cheats");
	press(PADB_CROSS);
	tick(4);
	frame(0);
	check(drew("Cheats") >= 0 && D.colour[drew("Cheats")] == COL_TITLE, "the cross: Cheats, a CONFIG page, its title");
	for (i = 0; i < 3; i++) {
		k = drew(cheats[i]);
		check(k >= 0 && D.align[k] == 0.0f, cheats[i]);
	}
	k = drew("Off");
	check(k >= 0 && D.align[k] == 1.0f && D.x[k] < 1024.0f, "the values right aligned inside the panel: Off");
	check(r8(MH_SHIM + SH_CURSOR) == 0, "the cursor on the page's first row");

	/* Infinite ammo on. */
	press(PADB_DOWN);
	press(PADB_DOWN);
	press(PADB_CROSS);
	tick(4);
	frame(0);
	k = drew("On");
	check(r8(MH_SHIM + SH_CURSOR) == 2 && k >= 0 && D.colour[k] == COL_ON &&
	      (features_toggle_state() & (1ull << TF_INFINITE_AMMO)) != 0,
	      "Infinite ammo: the cross turns it on, its value On in green");

	/* A held direction repeats. */
	frame(PADB_DOWN);
	for (i = 0; i < REPEAT_FIRST + 2 * REPEAT_EVERY; i++) frame(PADB_DOWN);
	frame(0);
	check(r8(MH_SHIM + SH_CURSOR) == 6, "Down held: one step, then a step every few frames (to Max raritanium)");
	press(PADB_DOWN);
	check(r8(MH_SHIM + SH_CURSOR) == 0, "past the last row it wraps to the first");

	/* Triangle back to the root, on Cheats; Triangle again closes; play goes on as it was. */
	press(PADB_TRIANGLE);
	tick(4);
	frame(0);
	check(drew("Trainer") >= 0 && r8(MH_SHIM + SH_CURSOR) == 1, "Triangle: back on the root, the cursor on Cheats");
	press(PADB_TRIANGLE);
	check(r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peekf(TM_CLOCK) == 0.75f && r8(MH_SHIM + SH_FROZEN) == 0,
	      "Triangle on the root: the menu closes, the time scale back to the game's 0.75");
	frame(PADB_CROSS);
	check(peek32(TM_PAD0 + TM_PAD_PRESSED) == PADB_CROSS && D.texts == 0, "the pad the game's again, nothing drawn");
	frame(0);
	tick(4);

	/* From the game's pause: no freeze; Start closes from deep in. */
	poke32(TM_STATE, TM_STATE_PAUSE);
	combo_press(TOD_COMBO_MENU);
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 1 && r8(MH_SHIM + SH_FROZEN) == 0 && peekf(TM_CLOCK) == 0.75f &&
	      drew("Trainer") >= 0 && r8(MH_SHIM + SH_CURSOR) == 0,
	      "in the game's pause the combo opens the root over it, the clock left alone");
	press(PADB_CROSS);
	tick(4);
	frame(0);
	frame(PADB_START);
	check(r32(MH_SHIM + SH_UP) == 0 && peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED &&
	      peek32(TM_PAD0 + TM_PAD_HELD) == PADB_START && r8(MH_SHIM + SH_BLANKED) == 0,
	      "Start closes it from Game, and the held mask goes back in that frame: the game's next read sees Start "
	      "down all along, no press");
	frame(PADB_START);
	check(peek32(TM_PAD0 + TM_PAD_PRESSED) == 0 && peek32(TM_PAD0 + TM_PAD_HELD) == PADB_START,
	      "the next read: Start held, not pressed, so the game's pause does not react to it");
	frame(0);
	tick(4);
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_ROOT, 0), "qwark back on its root for the next opening");

	/* A load taking the game while the menu is up closes it, the clock back. */
	poke32(TM_STATE, TM_STATE_PLAY);
	combo_press(TOD_COMBO_MENU);
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 1 && peekf(TM_CLOCK) == 0.0f, "open again from play, frozen");
	poke32(TM_STATE, 3);
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 0 && peekf(TM_CLOCK) == 0.75f &&
	      peek8(mb + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && D.texts == 0,
	      "a level load (state 3): the menu goes with nothing drawn, the clock back");
	poke32(TM_STATE, TM_STATE_PLAY);
	tick(4);

	/* The combo again while up closes it. */
	combo_press(TOD_COMBO_MENU);
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 1, "open");
	tick(4);
	combo_press(TOD_COMBO_MENU);
	check(peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "the combo with the menu up asks for the close");
	frame(0);
	check(r32(MH_SHIM + SH_UP) == 0 && peekf(TM_CLOCK) == 0.75f, "which the next frame takes");
	read_str(MH_SHIM + SH_HINT, a, sizeof(a));
	check(strcmp(a, "X Select   Triangle Back   Start Close") == 0, "the controls' line in the shim's state");

	features_set(TF_INFINITE_AMMO, 0);
	check(quit(), "quit ToD");
	config_set("menu.tod", "0");
}

void test_menu_tod(void)
{
	test_model_tod();
	test_install_tod();
	test_renderer_tod();
}
