/*
 * Tools of Destruction's console menu (revision 1.29): the game's
 * own pause menu, on qwark's fake console, against the real core
 * (docs/tod-menu.md).
 *
 *   the install   a few seconds into play: the Lua helper with its two menu
 *                 hooks, then the mailbox's header the core writes itself (no
 *                 ui.pak), the Help byte; under RPCS3 the same data writes
 *                 once the helper's patch is there, and none without it;
 *   the bridge    the Lua helper's pause-menu half (lh_menu.c) compiled for
 *                 the PC (LH_HOST), as the glue drives it through qw(): a
 *                 page qwark posts taken whole, its rows read, presses posted
 *                 back as events and qwark's answer taken as the next page,
 *                 one press in flight at a time, hstate, the requests and the
 *                 combo's press of the pause button, a gameplay frame;
 *   the glue      sent once to each new pause menu VM that asks, as a request
 *                 for the menu's VM (target MENU), and LUA_EXEC's flags bit0;
 *   the switches  menu.help (the Help byte) and menu.enabled (the identity).
 *
 * The glue itself is Lua for the game's own parser and runs only in the game:
 * check.py holds the helper's words to the executable, and the console list in
 * docs/tod-menu.md is what tries it.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/mem.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/core/luacore.h"
#include "../src/core/luamb.h"
#include "../src/games/tod.h"
#include "../src/games/luahelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------- the helper, on the PC */

#define LH_HOST 1

static u8 *lh_host_ptr(u32 addr)
{
	return host_mem_ptr(addr);
}

#include "../src/games/luahelper/lh_menu.c"

/* ---------------------------------------------------------------- helpers */

/* The menu combo's default, L1 + R1 + L2 + R2 + L3 + R3, in ToD's own pad bit order. */
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
static void poke8(u32 a, u8 v) { host_poke(a, &v, 1); }

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

static int wait_ready(void)
{
	int i;
	for (i = 0; i < 1500 && menu_state() != MENU_READY; i++) tick(1);
	return menu_state() == MENU_READY;
}

/* Row i (0-based) of the page the helper took: its label, as a C string. */
static const char *row_label(u32 i, char *buf)
{
	u32 k, a = lm_row(i) + MROW_OFF_LABEL;

	for (k = 0; k < MROW_LABEL_LEN && lm8(a + k) != 0; k++) buf[k] = (char)lm8(a + k);
	buf[k] = 0;
	return buf;
}

static const char *row_value(u32 i, char *buf)
{
	u32 k, a = lm_row(i) + MROW_OFF_VALUE;

	for (k = 0; k < MROW_VALUE_LEN && lm8(a + k) != 0; k++) buf[k] = (char)lm8(a + k);
	buf[k] = 0;
	return buf;
}

/* A row by its label, qwark's spelling: the helper's copy has it in capitals (lm_upper, the fourth console test). */
static int find_row(const char *label)
{
	char b[48], u[48];
	u32 i;

	for (i = 0; label[i] != 0 && i < sizeof(u) - 1; i++) u[i] = (char)toupper((unsigned char)label[i]);
	u[i] = 0;
	for (i = 0; i < lm_nrows(); i++) {
		if (strcmp(row_label(i, b), u) == 0) return (int)i;
	}
	return -1;
}

static void title(char *out)
{
	u32 k, a = LMP(MBOX_OFF_TITLE);

	for (k = 0; k < MBOX_TITLE_LEN && lm8(a + k) != 0; k++) out[k] = (char)lm8(a + k);
	out[k] = 0;
}

/* A press the glue makes, then qwark's ticks, then the page qwark posted taken. */
static int press(u32 kind, int row)
{
	int ok = lm_post(kind, row < 0 ? 0xFFu : (u32)row);
	tick(6);
	lm_take();
	return ok;
}

/*
 * Revision 1.29: Planets' page of planets. Where the galactic map can open
 * (a level's lua_State), the Planets row opens it instead (test_galmap); with
 * none it falls back to the page, which these tests want.
 */
static void planets_list(void)
{
	u32 L = peek32(TOD_LUA_STATE);

	poke32(TOD_LUA_STATE, 0);
	press(MBOX_EV_ACTIVATE, find_row("Planets"));
	poke32(TOD_LUA_STATE, L);
}

/* Whether the n bytes at p hold the string s. */
static int has_bytes(const u8 *p, u32 n, const char *s)
{
	u32 k, len = (u32)strlen(s);

	for (k = 0; k + len <= n; k++) {
		if (memcmp(p + k, s, len) == 0) return 1;
	}
	return 0;
}

static int hooks_in(const struct lh_desc *d, int ours)
{
	u8 i;
	for (i = 0; i < d->nhooks; i++) {
		if (peek32(d->hooks[i].addr) != (ours ? d->hooks[i].value : d->hooks[i].orig)) return 0;
	}
	return 1;
}

/* ----------------------------------------------------------- the install */

static void test_install(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u8 info[256];
	int i;

	group("ToD's console menu: the install (the game's own pause menu)");
	check(d != NULL && d->nhooks == 5 && d->hooks[2].addr == 0x000242B0u && d->hooks[2].orig == 0x4BFED721u &&
	      d->hooks[3].addr == 0x004C5ABCu && d->hooks[3].orig == 0x4BDA1825u &&
	      d->hooks[4].addr == 0x0003633Cu && d->hooks[4].orig == 0x4BFDB695u,
	      "the Lua helper has five hooks: the VM's, the error tap, the pause menu's update, the pause test and the galactic map's update");
	if (d == NULL) return;
	/* Revision 1.29, every planet (docs/tod-menu.md 17): the map's update goes to the pause menu's entry. */
	check(((d->hooks[4].value & 0x03FFFFFCu) + d->hooks[4].addr - ((d->hooks[4].value & 0x02000000u) ? 0x04000000u : 0u)) ==
	      ((d->hooks[2].value & 0x03FFFFFCu) + d->hooks[2].addr - ((d->hooks[2].value & 0x02000000u) ? 0x04000000u : 0u)),
	      "the galactic map's update goes to the same entry as the pause menu's");
	check(d->map_ptr == 0x101CBF80u && d->gmap != NULL && d->gmap_len > 1000 && d->gmap_len <= LUAMB_CHUNK_CAP,
	      "the map's menu system (g_galMapSystem) and its glue, which fits a request");
	check(d->menu_ptr == 0x101CB220u && d->menu_off == 0x106Cu && d->glue != NULL &&
	      d->glue_len > 1000 && d->glue_len <= LUAMB_CHUNK_CAP,
	      "and the pause menu's VM, and its glue, which fits one request");
	check(d->diag != NULL && d->diag_len > 500 && d->diag_len <= LUAMB_CHUNK_CAP &&
	      has_bytes(d->diag, d->diag_len, "qwark_state") && has_bytes(d->glue, d->glue_len, "qwark_state=T"),
	      "revision 1.29, the third console test: the helper's look, which reads the glue's qwark_state");
	check(LM_BOX_SIZE == MBOX_OFF_ROWS + LM_ROWS * MBOX_ROW_SIZE &&
	      LM_STATE_SIZE == LM_PAGE + LM_BOX_SIZE - MBOX_OFF_PAGE_KEY && LM_BOX + LM_BOX_SIZE <= LM_STATE &&
	      LM_STATE + LM_STATE_SIZE <= 0x008D0000u && d->mailbox + d->mailbox_size <= LM_BOX,
	      "the blocks: the Lua mailbox, the menu's, the helper's, in that order inside the page");

	check(boot_tod(), "ToD boots");
	session_info_copy(info, sizeof(info));
	check((info[29] & SESSION_FLAG2_NO_MENU) == 0, "flags2 NO_MENU clear: ToD has a console menu");
	poke32(TOD_STATE_PENDING_OP, 1);
	tick(480);
	check(menu_state() != MENU_READY && hooks_in(d, 0), "a state change queued: nothing goes in yet");
	poke32(TOD_STATE_PENDING_OP, 0);
	check(wait_ready(), "nothing queued: the menu goes in");
	check(hooks_in(d, 1) && peek32(d->mailbox) == LUAMB_MAGIC, "the Lua helper is in, its four words with it");
	check(menu_mailbox() == LM_BOX && peek32(LM_BOX) == MBOX_MAGIC && peek8(LM_BOX + MBOX_OFF_ROW_CAP) == LM_ROWS &&
	      peek8(LM_BOX + MBOX_OFF_ROW_SIZE) == MBOX_ROW_SIZE && peek8(LM_BOX + MBOX_OFF_MAX_ROWS) == LM_MAX_ROWS &&
	      peek8(LM_BOX + MBOX_OFF_CFG_ROWS) == LM_CFG_ROWS && peek8(LM_BOX + MBOX_OFF_CAPS) == LM_CAPS &&
	      peek8(LM_BOX + MBOX_OFF_RENDERER) == GAME_TOD && (peek32(LM_BOX + 4) & 0xFFFF) == LM_BOX_SIZE,
	      "the menu's mailbox, written by the core: 32 row slots, 9 at once in the list, 6 on Game Options, Left/Right, back and requests, game 5");
	check(peek8(LM_STATE + LM_HELP) == 1, "menu.help 1: the help byte says TRAINER (in Inventory's row)");
	check(peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) != 0 && (peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) & 1) == 0,
	      "and the root is posted");

	/* menu.help 0 and back: the byte alone, at once. */
	config_set_u32("menu.help", 0);
	tick(8);
	check(peek8(LM_STATE + LM_HELP) == 0, "menu.help 0: the game's own Inventory row");
	config_set_u32("menu.help", 1);
	tick(8);
	check(peek8(LM_STATE + LM_HELP) == 1, "menu.help 1 again");

	/* menu.enabled 0: the identity goes, so the helper and the glue let go; 1: back. */
	config_set_u32("menu.enabled", 0);
	tick(8);
	check(menu_state() == MENU_IDLE && peek32(LM_BOX) == 0 && hooks_in(d, 1),
	      "menu.enabled 0: the mailbox's identity cleared, the Lua helper left in");
	check(!lm_on(), "the helper's half sees no menu");
	config_set_u32("menu.enabled", 1);
	for (i = 0; i < 1500 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY && peek32(LM_BOX) == MBOX_MAGIC && peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) != 0 &&
	      (peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) & 1) == 0,
	      "menu.enabled 1: a fresh mailbox, the root posted again");
}

/* ------------------------------------------------------------- the bridge */

static void test_bridge(void)
{
	char b[48], t[40];
	int r, k;
	u32 gen, n;

	group("ToD's console menu: the bridge, page and presses (lh_menu.c, on the fake console)");
	check(lm_on(), "the helper's half sees the menu");

	/* The page, taken whole. */
	lm_take();
	title(t);
	check(lm8(LMS(LM_HAVE)) == 1 && lm32(LMS(LM_SEQ)) == peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) &&
	      lm32(LMS(LM_KEY)) == peek32(LM_BOX + MBOX_OFF_PAGE_KEY), "the root taken: its seq and key kept");
	check(strcmp(t, "TRAINER") == 0 && find_row("Game") == 0 && find_row("Cheats") == 1 &&
	      find_row("Manips") == 2 && find_row("Planets") > 2 && find_row("Unlocks") > 2,
	      "the root: Trainer, Game, Cheats, Manips, Planets, Unlocks ...");
	check((lm8(LMP(MBOX_OFF_PFLAGS)) & MBOX_PAGE_ROOT) != 0 && lm8(lm_row(1) + MROW_OFF_KIND) == MROW_LINK &&
	      (lm8(lm_row(1) + MROW_OFF_FLAGS) & MROWF_ENABLED) != 0, "flagged the root; Cheats a link the cursor stands on");
	gen = lm32(LMS(LM_GEN));
	lm_take();
	check(lm32(LMS(LM_GEN)) == gen, "taken once: nothing new without a post");

	/* A page half written is not taken. */
	poke32(LM_BOX + MBOX_OFF_PAGE_SEQ, peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) + 1);
	n = lm32(LMS(LM_VGEN));
	lm_take();
	check(lm32(LMS(LM_VGEN)) == n, "page_seq odd (qwark writing): left for the next frame");
	poke32(LM_BOX + MBOX_OFF_PAGE_SEQ, peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) - 1);

	/* Into Cheats: an event, qwark's answer, a new key. */
	check(press(MBOX_EV_ACTIVATE, find_row("Cheats")), "the cross on Cheats goes out");
	title(t);
	check(lm32(LMS(LM_GEN)) == gen + 1 && strcmp(t, "CHEATS") == 0 && lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG,
	      "qwark answers with Cheats, a CONFIG page, a new key");
	check(find_row("Fast loads") == 0 && find_row("Infinite ammo") >= 0 && find_row("Remove cutscenes") < 0 &&
	      find_row("Max bolts") >= 0, "Cheats: Fast loads, the toggles, Max bolts; Remove cutscenes not here");
	/* 2026-11-12: Remove black screens, a toggle after God Ratchet, in the page's first window. */
	r = find_row("Remove black screens");
	check(r >= 0 && r == find_row("God Ratchet") + 1 && lm8(lm_row((u32)r) + MROW_OFF_KIND) == MROW_TOGGLE &&
	      strcmp(row_value((u32)r, b), "OFF") == 0, "Cheats: Remove black screens, a toggle after God Ratchet, Off");
	check(press(MBOX_EV_ACTIVATE, r) && (features_toggle_state() & (1ull << TF_REMOVE_BLACK)) != 0,
	      "the cross turns Remove black screens on");
	check(press(MBOX_EV_LEFT, r) && (features_toggle_state() & (1ull << TF_REMOVE_BLACK)) == 0, "and Left off");
	r = find_row("Infinite ammo");
	check(r >= 0 && lm8(lm_row((u32)r) + MROW_OFF_KIND) == MROW_TOGGLE && strcmp(row_value((u32)r, b), "OFF") == 0 &&
	      (lm8(lm_row((u32)r) + MROW_OFF_FLAGS) & MROWF_STEPS) != 0, "Infinite ammo: a toggle, Off, Left and Right step it");

	/* One press in flight: the second waits for qwark. */
	n = peek32(LM_BOX + MBOX_OFF_EV_SEQ);
	check(lm_post(MBOX_EV_ACTIVATE, (u32)r) == 1 && lm_post(MBOX_EV_ACTIVATE, (u32)r) == 0 &&
	      peek32(LM_BOX + MBOX_OFF_EV_SEQ) == n + 1, "one press in flight: the second is dropped, not queued");
	check(peek8(LM_BOX + MBOX_OFF_EV_ROW) == (u8)r && peek32(LM_BOX + MBOX_OFF_EV_KEY) == lm32(LMS(LM_KEY)) &&
	      ((u32)peek8(LM_BOX + MBOX_OFF_EV_ID) << 8 | peek8(LM_BOX + MBOX_OFF_EV_ID + 1)) ==
	      ((u32)lm8(lm_row((u32)r) + MROW_OFF_ID) << 8 | lm8(lm_row((u32)r) + MROW_OFF_ID + 1)),
	      "the event: the row, its id echoed, the key of the page it was made on");
	tick(6);
	lm_take();
	check((features_toggle_state() & (1ull << TF_INFINITE_AMMO)) != 0 && strcmp(row_value((u32)r, b), "ON") == 0,
	      "qwark turns Infinite ammo on, and the page it posts says On");
	check(press(MBOX_EV_LEFT, r) && (features_toggle_state() & (1ull << TF_INFINITE_AMMO)) == 0,
	      "Left steps it back off");

	/* Back, and the Manips page with Remove cutscenes. */
	gen = lm32(LMS(LM_GEN));
	check(press(MBOX_EV_BACK, -1) && lm32(LMS(LM_GEN)) == gen + 1 && find_row("Game") == 0, "back: the root again");
	/*
	 * Build 59 (D7, the user's call): Game has no Die row, as the four games'
	 * have had none since round 5: the slot, Save and Load, Challenge mode and
	 * the set-aside pair, six rows, one Game Options window and no second page.
	 */
	press(MBOX_EV_ACTIVATE, find_row("Game"));
	title(t);
	check(strcmp(t, "GAME") == 0 && find_row("Die") < 0 && lm_nrows() == 6 && find_row("Challenge mode") == 3 &&
	      find_row("Load set-aside file") == 5,
	      "build 59: Game is six rows, one window, with no Die (the client's button and the combo keep it)");
	press(MBOX_EV_BACK, -1);
	press(MBOX_EV_ACTIVATE, find_row("Manips"));
	title(t);
	check(strcmp(t, "MANIPS") == 0 && find_row("Remove cutscenes") == 0, "Manips: Remove cutscenes, as the client has it");
	check(find_row("Cache cutscenes") == 2 && find_row("Cutscene cache") == 3 &&
	      find_row("Cache cutscenes on reset") == 4 && find_row("Disable cutscene caching") == 5 && lm_nrows() == 6,
	      "revision 1.29: then Cache cutscenes with its status row under it, and its two toggles");
	/* The fifth console test: a blank row between them, which the cursor passes over (lm_next). */
	check(lm8(lm_row(1) + MROW_OFF_KIND) == MROW_INFO && (lm_flags(1) & MROWF_ENABLED) == 0 &&
	      strcmp(row_label(1, b), "") == 0 && lm_next(0, 1, lm_nrows()) == 2 && lm_next(2, -1, lm_nrows()) == 0,
	      "the fifth console test: a blank row after Remove cutscenes, which the cursor steps over both ways");
	check(lm_press(MBOX_EV_ACTIVATE, 1) == 0, "and the cross there posts nothing");
	press(MBOX_EV_BACK, -1);

	/* An event made on another page is refused, and changes nothing. */
	k = find_row("Cheats");
	poke32(LMS(LM_KEY), lm32(LMS(LM_KEY)) ^ 0x00FF0000u);
	gen = lm32(LMS(LM_GEN));
	press(MBOX_EV_ACTIVATE, k);
	check(find_row("Game") == 0 && lm32(LMS(LM_GEN)) == gen && peek32(LM_BOX + MBOX_OFF_EV_ACK) ==
	      peek32(LM_BOX + MBOX_OFF_EV_SEQ), "a stale key: qwark answers the press and does nothing, the root stays");
	features_set(TF_INFINITE_AMMO, 0);
}

/*
 * Revision 1.29, the fourth console test (docs/tod-menu.md 12): what the
 * helper decides for the glue now. A press goes out only where the row
 * takes it (lm_press), the cursor's next row skips what it cannot stand on
 * (lm_next), a window's edge asks qwark for the next window (lm_edge), and
 * the copy is in capitals (lm_upper), past nothing a RANGE row keeps.
 */
static void test_screens(void)
{
	char b[48], t[40];
	int r, s, i;
	u32 n, gen, ev;

	group("ToD's console menu: presses, windows and capitals for the pause's list and Game Options (lh_menu.c)");
	lm_take();
	poke32(LMS(LM_KEY), peek32(LM_BOX + MBOX_OFF_PAGE_KEY));
	title(t);
	check(strcmp(t, "TRAINER") == 0 && strcmp(row_label(0, b), "GAME") == 0 && lm_nrows() <= LM_MAX_ROWS &&
	      lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_MAIN, "the root: a MAIN page in capitals, nine rows at most (the pause's list)");

	/* Cheats: a toggle takes Left and Right, a button the cross but no step. */
	press(MBOX_EV_ACTIVATE, find_row("Cheats"));
	check(lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG && lm_nrows() <= LM_CFG_ROWS,
	      "Cheats: a CONFIG page, six rows at most (Game Options)");
	ev = peek32(LM_BOX + MBOX_OFF_EV_SEQ);
	r = find_row("Infinite ammo");
	check(r >= 0 && lm_press(MBOX_EV_ACTIVATE, LM_ROWS) == 0 && peek32(LM_BOX + MBOX_OFF_EV_SEQ) == ev,
	      "a press past the page posts nothing");
	check(lm_press(MBOX_EV_RIGHT, (u32)r) == 1 && peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_RIGHT,
	      "Right on Infinite ammo, which steps: posted");
	tick(6);
	lm_take();
	check((features_toggle_state() & (1ull << TF_INFINITE_AMMO)) != 0 && strcmp(row_value((u32)r, b), "ON") == 0,
	      "and qwark turns it on, ON in capitals");
	check(lm_press(MBOX_EV_LEFT, (u32)r) == 1, "Left posted too");
	tick(6);
	lm_take();
	i = find_row("Max bolts");
	ev = peek32(LM_BOX + MBOX_OFF_EV_SEQ);
	check(i >= 0 && (lm_flags((u32)i) & MROWF_STEPS) == 0 && lm_press(MBOX_EV_RIGHT, (u32)i) == 0 &&
	      peek32(LM_BOX + MBOX_OFF_EV_SEQ) == ev, "Right on Max bolts, a button that does not step: nothing (the glue's cross then)");

	/*
	 * Manips, a page that fits one window (Cheats has two since 2026-11-12):
	 * an INFO row takes no cross, and the next row the cursor stands on wraps
	 * inside the window's rows.
	 */
	press(MBOX_EV_BACK, -1);
	press(MBOX_EV_ACTIVATE, find_row("Manips"));
	ev = peek32(LM_BOX + MBOX_OFF_EV_SEQ);
	s = -1;
	for (i = 0; i < (int)lm_nrows(); i++) if ((lm_flags((u32)i) & MROWF_ENABLED) == 0) s = i;
	check(s > 0 && lm_press(MBOX_EV_ACTIVATE, (u32)s) == 0 && peek32(LM_BOX + MBOX_OFF_EV_SEQ) == ev,
	      "the cross on a row the cursor cannot stand on posts nothing");
	n = lm_nrows();
	check(lm_next(0, 1, n) == (lm_flags(1) & MROWF_ENABLED ? 1u : lm_next(1, 1, n)) && lm_next(0, -1, n) < n &&
	      lm_next(n, 1, n) == n && lm_next(0, 1, 0) == 0, "lm_next: down, up wrapping, out of range left alone");
	if (s >= 0 && s + 1 < (int)n && s > 0)
		check(lm_next((u32)s - 1, 1, n) != (u32)s, "lm_next steps over the row the cursor cannot stand on");
	press(MBOX_EV_BACK, -1);

	/* Planets: longer than Game Options' six rows, so a window; its last row asks for the next. */
	planets_list();
	title(t);
	check(strcmp(t, "PLANETS") == 0 && lm_nrows() == LM_CFG_ROWS &&
	      (lm8(LMP(MBOX_OFF_PFLAGS)) & MBOX_PAGE_MORE_DOWN) != 0 && (lm8(LMP(MBOX_OFF_PFLAGS)) & MBOX_PAGE_MORE_UP) == 0,
	      "Planets: six rows, more below");
	gen = lm32(LMS(LM_GEN));
	strcpy(t, row_label(0, b));
	check(lm_edge(2) == 0 && lm_edge(0) == 0, "lm_edge inside the window, and at its top with nothing above: nothing");
	check(lm_edge(LM_CFG_ROWS - 1) == 1 && peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_DOWN,
	      "at its last row: SCROLL_DOWN");
	tick(6);
	lm_take();
	check(lm32(LMS(LM_GEN)) == gen + 1 && (lm8(LMP(MBOX_OFF_PFLAGS)) & MBOX_PAGE_MORE_UP) != 0 &&
	      strcmp(row_label(0, b), t) != 0, "qwark slides the window: more above now, other planets");
	check(lm_edge(0) == 1 && peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_UP, "at its top: SCROLL_UP");
	tick(6);
	lm_take();
	press(MBOX_EV_BACK, -1);
	check(find_row("Game") == 0, "back at the root");

	/* The capitals stop at a string's end: a RANGE row's words past its label are left as they are. */
	poke32(lm_row(0) + MROW_OFF_RANGE, 0x61626364u);
	lm_caps(lm_row(0) + MROW_OFF_LABEL, MROW_LABEL_LEN);
	check(lm32(lm_row(0) + MROW_OFF_RANGE) == 0x61626364u, "lm_caps stops at the label's NUL: a RANGE row's words kept");
	features_set(TF_INFINITE_AMMO, 0);
}

/*
 * Revision 1.29, the polish (docs/tod-menu.md 15): Planets named as the
 * ship's planet selector names them, and a page at a time (MBOX_CAP_PAGED):
 * Down on a window's last row shows the next window with the cursor on its
 * top row, Up on its top row the window before with the cursor on its bottom
 * row, and past either end the page wraps, as the game's lists do. The
 * helper asks (lm_next) as the cursor would leave the window; qwark turns
 * the window (menu.c nav_move).
 */
static void test_paging(void)
{
	char b[48], t[40];
	u32 gen, ev;

	group("ToD's console menu: Planets as the ship names them, a page at a time (lh_menu.c, menu.c)");
	lm_take();
	poke32(LMS(LM_KEY), peek32(LM_BOX + MBOX_OFF_PAGE_KEY));
	check((peek8(LM_BOX + MBOX_OFF_CAPS) & MBOX_CAP_PAGED) != 0 && LM_CAPS == 0x2D,
	      "the mailbox's header says a page at a time");
	planets_list();
	title(t);
	check(strcmp(t, "PLANETS") == 0 && lm_nrows() == LM_CFG_ROWS && strcmp(row_label(0, b), "KERWAN") == 0 &&
	      strcmp(row_label(1, b), "COBALIA") == 0 && strcmp(row_label(2, b), "KORTOG") == 0 &&
	      strcmp(row_label(3, b), "FASTOON") == 0 && strcmp(row_label(4, b), "VORON ASTEROID BELT") == 0 &&
	      strcmp(row_label(5, b), "MUKOW") == 0 && lm8(LMP(MBOX_OFF_PFLAGS)) == MBOX_PAGE_MORE_DOWN,
	      "Planets' first window: KERWAN, COBALIA, KORTOG, FASTOON, VORON ASTEROID BELT, MUKOW, more below");
	gen = lm32(LMS(LM_GEN));
	ev = peek32(LM_BOX + MBOX_OFF_EV_SEQ);
	check(lm_next(4, 1, LM_CFG_ROWS) == 5 && peek32(LM_BOX + MBOX_OFF_EV_SEQ) == ev,
	      "Down inside the window, to its last row: the next row, nothing asked (it used to slide there)");
	check(lm_next(5, 1, LM_CFG_ROWS) == 5 && peek32(LM_BOX + MBOX_OFF_EV_SEQ) == ev + 1 &&
	      peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_DOWN, "Down on its last row: the cursor stays, SCROLL_DOWN");
	tick(6);
	lm_take();
	check(lm32(LMS(LM_GEN)) == gen + 1 && strcmp(row_label(0, b), "NUNDAC ASTEROID RING") == 0 &&
	      strcmp(row_label(5, b), "KREELI COMET") == 0 && lm8(LMP(MBOX_OFF_CURSOR)) == 0 &&
	      lm8(LMP(MBOX_OFF_PFLAGS)) == (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN),
	      "the next window, a whole page on (NUNDAC ASTEROID RING .. KREELI COMET), the cursor on its top row");
	check(lm_next(0, -1, LM_CFG_ROWS) == 0 && peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_UP,
	      "Up on its top row: the cursor stays, SCROLL_UP");
	tick(6);
	lm_take();
	check(strcmp(row_label(0, b), "KERWAN") == 0 && lm8(LMP(MBOX_OFF_CURSOR)) == 5,
	      "the window before, the cursor on its bottom row (MUKOW)");
	check(lm_next(0, -1, LM_CFG_ROWS) == 0 && peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_UP,
	      "Up on the first window's top row: SCROLL_UP too");
	tick(6);
	lm_take();
	check(lm_nrows() == 1 && strcmp(row_label(0, b), "FASTOON II") == 0 && lm8(LMP(MBOX_OFF_CURSOR)) == 0 &&
	      lm8(LMP(MBOX_OFF_PFLAGS)) == MBOX_PAGE_MORE_UP,
	      "it wraps: the last window (18 on, one row, FASTOON II), the cursor on its last row");
	check(lm_next(0, 1, 1) == 0 && peek8(LM_BOX + MBOX_OFF_EV_KIND) == MBOX_EV_SCROLL_DOWN,
	      "Down on the last window's last row: SCROLL_DOWN");
	tick(6);
	lm_take();
	check(strcmp(row_label(0, b), "KERWAN") == 0 && lm8(LMP(MBOX_OFF_CURSOR)) == 0 && lm_nrows() == LM_CFG_ROWS,
	      "it wraps: the first window, the cursor on its first row");
	check(lm_next(5, -1, LM_CFG_ROWS) == 4 && lm_next(0, 1, LM_CFG_ROWS) == 1,
	      "Up and Down inside a window move as before");
	press(MBOX_EV_BACK, -1);
	check(find_row("Game") == 0, "back at the root");
}

/* ---------------------------------------------- open, close, the combo */

static void test_requests(void)
{
	u32 frames;
	int i;

	group("ToD's console menu: open and close, the combo, the pause test");

	/* The glue opening the menu, and closing it. */
	lm_hstate(1);
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_HSTATE) == MBOX_HSTATE_OPEN && peek32(LM_BOX + MBOX_OFF_FRAMES) == 0,
	      "open: hstate OPEN, the frames from 0");
	lm_menu_frame();
	lm_menu_frame();
	check(peek32(LM_BOX + MBOX_OFF_FRAMES) == 2, "a frame of the pause menu counts while it is up");
	lm_hstate(0);
	lm_menu_frame();
	check(peek8(LM_BOX + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && peek32(LM_BOX + MBOX_OFF_FRAMES) == 2,
	      "closed: hstate CLOSED, and no frames counted");

	/* The combo in play: a request, which the game's pause test takes as Start. */
	host_set_pad(0);
	tick(4);
	host_set_pad(TOD_COMBO_MENU);
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "the combo: an open request");
	check(lm_pause_check(0) == 1 && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE &&
	      lm8(LMS(LM_PENDING)) == LM_PENDING_FRAMES, "the pause test answers pressed, the request taken, pending");
	check(lm_pause_check(0) == 0 && lm_pause_check(1) == 1, "and only once: then the pad's own answer");
	for (i = 0; i < 40; i++) tick(1);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "held: the combo fires once, no second request");
	host_set_pad(0);
	tick(4);
	check(lm_take_request() == 1 && lm8(LMS(LM_PENDING)) == 0 && lm_take_request() == 0,
	      "the pause up, the glue takes the pending open, once");

	/* A pause that never came: forgotten after LM_PENDING_FRAMES gameplay frames. */
	lm_pause_check(0);
	poke8(LM_BOX + MBOX_OFF_REQUEST, MBOX_REQ_OPEN);
	lm_pause_check(0);
	for (i = 0; i < LM_PENDING_FRAMES; i++) lm_gameplay_frame();
	check(lm8(LMS(LM_PENDING)) == 0 && lm_take_request() == 0, "a pause that never came up opens nothing later");

	/* The combo with the menu up asks for a close, which the glue takes. */
	lm_hstate(1);
	tick(4);
	host_set_pad(TOD_COMBO_MENU);
	tick(4);
	host_set_pad(0);
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE && lm_take_request() == 2 &&
	      peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "the combo with the menu up: a close, taken");
	check(lm_pause_check(0) == 0, "which the pause test leaves alone");

	/* A gameplay frame: no pause menu, so no menu either, however it went. */
	poke32(LMS(LM_L), 0x1234);
	poke8(LMS(LM_TRIED), 1);
	frames = peek32(LM_BOX + MBOX_OFF_FRAMES);
	lm_gameplay_frame();
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_HSTATE) == MBOX_HSTATE_CLOSED && lm32(LMS(LM_L)) == 0 && lm8(LMS(LM_TRIED)) == 0 &&
	      peek32(LM_BOX + MBOX_OFF_FRAMES) == frames, "a gameplay frame closes it, and the next pause is a new VM");
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_ROOT, 0), "qwark sees the close: its root for the next open");
}

/* ------------------------------------------------- loads from the pause */

/*
 * Revision 1.29, the fifth console test (docs/tod-menu.md 13): a planet
 * picked in the Trainer, or asked for by the client, while the game is in
 * its pause. The glue's part is played here: it takes qwark's close
 * (lm_take_request), leaves the menu (lm_hstate) and calls exit_pause_menu,
 * after which the game's pause goes 4 (closing), then 0 a frame or more
 * before the game itself is back in play (g_gameState 4), as the console's
 * does.
 */
static void pause_up(void)
{
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	poke32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	poke32(TOD_PAUSE_STATE, TOD_PAUSE_UP);
	poke32(LMS(LM_GLUE), LM_GLUE_LOADED);
	tick(4);
}

static void pause_gone(void)
{
	poke32(TOD_PAUSE_STATE, 0);
	poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	poke32(LMS(LM_GLUE), LM_GLUE_NONE);
	lm_gameplay_frame();
	tick(6);
}

static void test_pause_loads(void)
{
	int r, i;
	u32 planet;

	group("ToD's console menu: a load from the pause (the fifth console test)");

	/* The Trainer on Planets in the pause, a planet picked. */
	pause_up();
	lm_hstate(1);
	tick(4);
	lm_take();
	planets_list();
	r = 1;
	planet = lm8(lm_row((u32)r) + MROW_OFF_ID + 1);
	check(lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG && lm8(lm_row((u32)r) + MROW_OFF_ID) == MENU_ROW_PLANET,
	      "the Trainer's Planets in the pause");
	press(MBOX_EV_ACTIVATE, r);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE && peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "a planet: qwark asks the menu (and with it the pause) to close, and loads nothing yet");

	/* The glue takes the close: the menu down, exit_pause_menu; the pause closing, then gone with the game still in it. */
	check(lm_take_request() == 2, "the glue takes the close");
	lm_hstate(0);
	poke32(TOD_PAUSE_STATE, 4);
	tick(6);
	poke32(TOD_PAUSE_STATE, 0);
	for (i = 0; i < 24; i++) tick(1);
	check(peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "the pause gone but the game still in it (g_gameState 5, nothing queued): no load, and no BUSY");
	poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(6);
	check(peek32(TOD_LOAD_REQUEST) == planet, "the game plays: the planet loads");
	pause_gone();
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(30);

	/* The client's PLANET_LOAD in the pause, the Trainer down: the glue closes the pause, then the planet loads. */
	pause_up();
	check(session_planet_load((u8)planet, 0) == ST_OK && peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "the client's planet in the pause: OK, nothing loaded yet");
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE, "with the menu down qwark still asks the glue to close the pause");
	check(lm_take_request() == 2, "which the glue takes");
	poke32(TOD_PAUSE_STATE, 0);
	tick(12);
	check(peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu, "not while the game is still in its pause");
	poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(6);
	check(peek32(TOD_LOAD_REQUEST) == planet, "and loaded once it plays");
	pause_gone();
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(30);

	/* Cancelled: the game leaves its pause another way (an FMV, the title) before it ever plays. */
	pause_up();
	session_planet_load((u8)planet, 0);
	tick(4);
	poke32(TOD_GAME_STATE, TOD_STATE_FMV);
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "the game left its pause another way: the close withdrawn");
	pause_gone();
	tick(30);
	check(peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu, "and the load cancelled: nothing loads when it plays again");

	/* Cancelled: the menu closed and came up again in the same pause (the close never reached the pause). */
	pause_up();
	lm_hstate(1);
	tick(4);
	lm_take();
	planets_list();
	press(MBOX_EV_ACTIVATE, 1);
	lm_hstate(0);
	tick(4);
	lm_hstate(1);
	tick(4);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE, "the menu up again after its close: the load cancelled, no close waiting");
	lm_hstate(0);
	pause_gone();
	tick(30);
	check(peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu, "and nothing loads later");

	/* Not in a pause: PLANET_LOAD is the game's own answer, as before. */
	check(session_planet_load((u8)planet, 0) == ST_OK && peek32(TOD_LOAD_REQUEST) == planet,
	      "in play the client's planet loads at once");
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(30);
}

/* --------------------------------------------- the galactic map (1.29) */

#define LS(level, off) (TOD_LEVEL_SAVE + (u32)(level) * TOD_LEVEL_SAVE_LEN + (off))

static u8 g_ls_before[19][3];

/* A save part way through the story: Metropolis to Fastoon unlocked, a hidden byte, Space Combat II visited. */
static void save_part_way(void)
{
	u32 l;

	for (l = 0; l < 19; l++) {
		poke8(LS(l, TOD_LS_UNLOCKED), l <= 3 ? 1 : 0);
		poke8(LS(l, TOD_LS_VISITED), l <= 2 ? 1 : 0);
		poke8(LS(l, TOD_LS_HIDDEN), 0);
	}
	poke8(LS(5, TOD_LS_HIDDEN), 1);
	poke8(LS(8, TOD_LS_VISITED), 1);
	for (l = 0; l < 19; l++) {
		g_ls_before[l][0] = peek8(LS(l, TOD_LS_UNLOCKED));
		g_ls_before[l][1] = peek8(LS(l, TOD_LS_VISITED));
		g_ls_before[l][2] = peek8(LS(l, TOD_LS_HIDDEN));
	}
}

static int save_as_before(void)
{
	u32 l, k;

	for (l = 0; l < 19; l++) {
		for (k = 0; k < 3; k++) {
			if (peek8(LS(l, TOD_LS_UNLOCKED + k)) != g_ls_before[l][k]) return 0;
		}
	}
	return 1;
}

/* Every level the map lists unlocked and shown, the space combat levels visited; Metropolis and Fastoon II the save's. */
static int save_for_map(void)
{
	u32 l;

	for (l = 1; l <= 17; l++) {
		if (peek8(LS(l, TOD_LS_UNLOCKED)) != 1 || peek8(LS(l, TOD_LS_HIDDEN)) != 0) return 0;
	}
	return peek8(LS(4, TOD_LS_VISITED)) == 1 && peek8(LS(8, TOD_LS_VISITED)) == 1 &&
	       peek8(LS(13, TOD_LS_VISITED)) == 1 && peek8(LS(9, TOD_LS_VISITED)) == g_ls_before[9][1] &&
	       peek8(LS(0, TOD_LS_UNLOCKED)) == g_ls_before[0][0] && peek8(LS(18, TOD_LS_UNLOCKED)) == g_ls_before[18][0];
}

/* The Trainer up in the pause on its root, the page taken. */
static void trainer_up(void)
{
	pause_up();
	lm_hstate(1);
	tick(4);
	lm_take();
}

/* Planets pressed, the glue closing the pause, the game playing: the map asked for. */
static void to_map(int row)
{
	press(MBOX_EV_ACTIVATE, row);
	lm_take_request();
	lm_hstate(0);
	poke32(TOD_PAUSE_STATE, 4);
	tick(4);
	poke32(TOD_PAUSE_STATE, 0);
	poke32(LMS(LM_GLUE), LM_GLUE_NONE);
	poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(6);
}

/*
 * Revision 1.29, every planet (docs/tod-menu.md 17): the map's frames, as the
 * helper's map hook runs them, until one serves a request of target MAP (the
 * map's glue), the core ticking between; 1 when one was served (at most
 * three seconds: the glue goes once the mailbox is free).
 */
static int map_frames(void)
{
	int i;

	for (i = 0; i < 360; i++) {
		if (host_fake_map_frame()) {
			tick(8);
			return 1;
		}
		tick(1);
	}
	return 0;
}

/*
 * Revision 1.29: a pick on qwark's map is qwark's load (tod.h,
 * tod_galmap_up): Fast loads forced and the short fade armed as the map comes
 * up, put back at once when it goes without a pick, and after the load
 * otherwise. The flag byte its pieces read is the console's sign of the patch.
 */
#define GM_FL_FLAG 0x108A8560u   /* SW_TOD_FLAG_FAST_LOADS */

static void gm_flag106(int set)
{
	const u32 at = TOD_LEVEL_FLAGS + TOD_LF_BYTE(TOD_LF_FAST_SHIP_FIXED);
	u8 bit = (u8)(1u << (TOD_LF_FAST_SHIP_FIXED & 7u));

	poke8(at, set ? (u8)(peek8(at) | bit) : (u8)(peek8(at) & ~bit));
}

static int gm_forced(u32 fade)
{
	return peek8(GM_FL_FLAG) == 1 && peek32(TOD_TRANSITION_MIN) == 0 && peek32(TOD_LOAD_FADE_TIME) == fade;
}

static int gm_game(void)
{
	return peek8(GM_FL_FLAG) == 0 && peek32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME &&
	       peek32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME;
}

/* The game's load of the pick, the request already taken: the level's tiers out, then in, the level up again. */
static void gm_load(void)
{
	u32 lua = peek32(TOD_LUA_STATE);

	poke32(TOD_LUA_STATE, 0);
	poke8(TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED, 0);
	poke32(TOD_LOAD_JOB, TOD_LOAD_JOB_LEVEL);
	tick(8);
	poke8(TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED, 1);
	poke32(TOD_LOAD_JOB, 0);
	tick(4);
	poke32(TOD_LUA_STATE, lua);
	tick(8);
}

static void test_galmap(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u32 mb = d->mailbox, req, L, i;
	char b[48], t[40];
	int r;

	group("ToD's console menu: Planets opens the galactic map, every planet on it (revision 1.29, docs/tod-menu.md 16, 17)");
	save_part_way();
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	trainer_up();
	r = find_row("Planets");
	check(r >= 0 && lm8(lm_row((u32)r) + MROW_OFF_KIND) == MROW_LINK,
	      "the root's Planets row, a link as before");

	poke32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	poke32(TOD_LOAD_FADE_TIME, TOD_LOAD_FADE_GAME);
	poke8(GM_FL_FLAG, 0);
	poke8(TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED, 1);
	poke32(TOD_LOAD_JOB, 0);
	gm_flag106(1);
	check(!((features_toggle_state() >> TF_FAST_LOADS) & 1u) && gm_game(), "Fast loads off, the game's fade");
	req = peek32(mb + LUAMB_OFF_REQ);
	press(MBOX_EV_ACTIVATE, r);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_CLOSE && peek32(mb + LUAMB_OFF_REQ) == req && save_as_before(),
	      "the cross: qwark asks the pause to close, and writes nothing yet");
	check(lm_take_request() == 2, "the glue takes the close (exit_pause_menu)");
	lm_hstate(0);
	poke32(TOD_PAUSE_STATE, 4);
	tick(6);
	poke32(TOD_PAUSE_STATE, 0);
	tick(12);
	check(peek32(mb + LUAMB_OFF_REQ) == req && save_as_before(), "the game still in its pause: nothing yet");
	poke32(LMS(LM_GLUE), LM_GLUE_NONE);
	poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(6);
	{
		u8 tg = peek8(mb + LUAMB_OFF_TARGET);
		char chunk[16];

		host_peek(mb + LUAMB_OFF_CHUNK, (u8 *)chunk, 6);
		chunk[6] = 0;
		check(peek32(mb + LUAMB_OFF_REQ) != req && tg == LUAMB_TARGET_GAMEPLAY && peek32(mb + LUAMB_OFF_CHUNK_LEN) == 6 &&
		      strcmp(chunk, "gmap()") == 0, "the game plays: gmap() to the gameplay VM, as Fastoon's ship calls it");
	}
	check(save_for_map(), "and every level the map lists unlocked and shown, the space combat levels visited (Metropolis, Fastoon II the save's)");
	host_fake_lua_frame();
	tick(6);
	check(peek32(TOD_GAME_STATE) == TOD_STATE_GALMAP && save_for_map(), "the map up (g_gameState 6), the unlocks kept while it is");
	check(gm_forced(TOD_LOAD_FADE_QUICK) && !((features_toggle_state() >> TF_FAST_LOADS) & 1u),
	      "and a pick on it qwark's load: Fast loads forced on (the toggle still off), the one-frame fade armed (flag 106 set)");
	/* Revision 1.29, every planet (docs/tod-menu.md 17): the map's glue to the map's own VM. */
	check(peek32(mb + LUAMB_OFF_REQ) != req && peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_MAP &&
	      peek32(mb + LUAMB_OFF_CHUNK_LEN) == d->gmap_len &&
	      memcmp(host_mem_ptr(mb + LUAMB_OFF_CHUNK), d->gmap, d->gmap_len) == 0,
	      "the map up: qwark sends the map's glue (lh_gmap.lua) to the map's VM, a request of target MAP");
	host_log_clear();
	check(map_frames() && host_log_seen("menu: the galactic map lists every planet (23 rows)"),
	      "the map's frame runs it: every planet, said");
	req = peek32(mb + LUAMB_OFF_REQ);
	check(!map_frames() && peek32(mb + LUAMB_OFF_REQ) == req, "and once: nothing more is sent");
	poke8(LS(9, TOD_LS_UNLOCKED), 2);

	/* Back: the save's bytes again, the Trainer again on its root, the cursor on Planets, which opens the map again. */
	host_fake_map(-1);
	tick(12);
	check(gm_game(), "Back, no load: Fast loads and the fade-out the game's again at once");
	check(peek8(LS(9, TOD_LS_UNLOCKED)) == 2, "a byte the game changed while the map was up stays the game's");
	poke8(LS(9, TOD_LS_UNLOCKED), g_ls_before[9][0]);
	check(save_as_before(), "Back: every other byte the save's own again");
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN && peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "and qwark asks the Trainer to open again, nothing loaded");
	pause_up();
	check(lm_take_request() == 1, "the pause test takes the open, the glue opens the Trainer");
	lm_hstate(1);
	tick(4);
	lm_take();
	title(t);
	check(strcmp(t, "TRAINER") == 0 && lm8(LMP(MBOX_OFF_CURSOR)) == (u8)r && strcmp(row_label((u32)r, b), "PLANETS") == 0,
	      "the root, the cursor on the Planets row, which reads PLANETS: one picker, no second row");

	/* A pick (Kerwan, which only the glue lists): the game's own load (the map's exit, LoadLevel), the bytes back, the Trainer stays down. */
	gm_flag106(0);
	to_map(r);
	host_fake_lua_frame();
	tick(6);
	check(peek32(TOD_GAME_STATE) == TOD_STATE_GALMAP, "Planets again: the map again, at once");
	check(gm_forced(TOD_LOAD_FADE_STORY), "flag 106 clear: the story load's fade armed, a pick may be Kerwan or Cobalia");
	host_log_clear();
	check(map_frames() && host_log_seen("lists every planet"), "its glue in again");
	host_fake_map(0);
	tick(6);
	check(save_as_before() && peek32(TOD_LOAD_REQUEST) == 0 && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE,
	      "a pick of Kerwan: the game's own load under way, the save's bytes back, no Trainer");
	check(gm_forced(TOD_LOAD_FADE_STORY), "a story load: its fade, Fast loads forced for it");
	pause_gone();
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(30);
	check(peek32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME && peek8(GM_FL_FLAG) == 1,
	      "the request taken: the game's fade time back, Fast loads kept for the load");
	gm_load();
	check(gm_game() && !((features_toggle_state() >> TF_FAST_LOADS) & 1u), "the level up: Fast loads off again, as the toggle is");

	/* The glue saying "wait" (the map's handler not up yet): sent again until it is in. Fast loads on this time. */
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && peek8(GM_FL_FLAG) == 1, "Fast loads on");
	host_fake_map_glue(1);
	trainer_up();
	to_map(find_row("Planets"));
	host_fake_lua_frame();
	tick(6);
	host_log_clear();
	check(map_frames(), "the glue sent, and it says wait");
	req = peek32(mb + LUAMB_OFF_REQ);
	tick(24);
	check(peek32(mb + LUAMB_OFF_REQ) != req && peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_MAP &&
	      !host_log_seen("lists every planet"), "sent again a moment later");
	host_fake_map_glue(0);
	check(map_frames() && host_log_seen("lists every planet"), "then in");
	check(gm_forced(TOD_LOAD_FADE_STORY), "the map up: the story fade armed, Fast loads on as they were");
	host_fake_map(7);
	tick(6);
	check(save_as_before() && peek32(TOD_LOAD_REQUEST) == 7 && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE,
	      "a pick (Ardolis): the game's own load under way, the save's bytes back, no Trainer");
	check(gm_forced(TOD_LOAD_FADE_QUICK), "Ardolis is no story load: the one frame");
	pause_gone();
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(30);
	gm_load();
	check(peek8(GM_FL_FLAG) == 1 && peek32(TOD_TRANSITION_MIN) == 0 && ((features_toggle_state() >> TF_FAST_LOADS) & 1u) &&
	      peek32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "the level up: Fast loads left on, the toggle's");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && gm_game(), "Fast loads off again");
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE && peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "and nothing more: qwark loads nothing of its own");

	/* The glue failing: said, the map the game's own still; Back then opens the page of planets, the one fallback. */
	host_fake_map_glue(2);
	trainer_up();
	to_map(find_row("Planets"));
	host_fake_lua_frame();
	tick(6);
	host_log_clear();
	i = host_notify_count();
	map_frames();
	check(host_log_seen("menu: the galactic map could not list every planet (qwark:1: x); Back opens the Planets page") &&
	      host_notify_count() == i + 1 && strstr(host_notify_last(), "lists the save's planets only") != NULL,
	      "the glue failing: logged, and said on the console");
	check(peek32(TOD_GAME_STATE) == TOD_STATE_GALMAP && save_for_map(), "the map stays up, the game's own, the unlocks in");
	host_fake_map(-1);
	tick(12);
	check(save_as_before() && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN, "Back: the bytes back, the Trainer asked to open");
	check(gm_game(), "the game's own list's map closed: the force and the fade back too");
	pause_up();
	lm_take_request();
	lm_hstate(1);
	tick(4);
	lm_take();
	title(t);
	check(strcmp(t, "PLANETS") == 0 && lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG && strcmp(row_label(0, b), "KERWAN") == 0,
	      "on the page of planets (Kerwan first): every planet a press away");
	press(MBOX_EV_BACK, -1);
	lm_hstate(0);
	pause_gone();
	tick(30);
	host_fake_map_glue(0);

	/* A gmap() that never brings the map: given up, the bytes back, the Trainer on the page of planets. */
	host_fake_map_off(1);
	trainer_up();
	to_map(find_row("Planets"));
	host_fake_lua_frame();
	tick(6);
	check(peek32(TOD_GAME_STATE) == TOD_STATE_GAMEPLAY && save_for_map() && gm_game(),
	      "a map that does not come: the unlocks in, waiting, nothing forced or armed");
	for (i = 0; i < 520 && peek8(LM_BOX + MBOX_OFF_REQUEST) != MBOX_REQ_OPEN; i++) tick(1);
	check(save_as_before() && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "four seconds on: the bytes back, and the Trainer asked to open again");
	pause_up();
	lm_take_request();
	lm_hstate(1);
	tick(4);
	lm_take();
	title(t);
	check(strcmp(t, "PLANETS") == 0 && lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG,
	      "on the page of planets: the fallback");
	press(MBOX_EV_BACK, -1);
	host_fake_map_off(0);

	/* No level's lua_State (no Lua for gmap()): the page at once, nothing closed or written. */
	L = peek32(TOD_LUA_STATE);
	poke32(TOD_LUA_STATE, 0);
	req = peek32(mb + LUAMB_OFF_REQ);
	press(MBOX_EV_ACTIVATE, find_row("Planets"));
	title(t);
	check(strcmp(t, "PLANETS") == 0 && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE &&
	      peek32(mb + LUAMB_OFF_REQ) == req && save_as_before(),
	      "where the map cannot open: the page of planets at once, nothing closed, nothing written");
	poke32(TOD_LUA_STATE, L);
	press(MBOX_EV_BACK, -1);
	lm_hstate(0);
	pause_gone();
	tick(30);
}

/* ------------------------------------------- the page combos (rev. 1.34) */

/* ToD's own pad bits for the page combos: L1 0x40 and Select 0x100, and a direction. */
#define TOD_PAGE_PAD(dir) (0x40u | 0x100u | (dir))
#define TOD_PAD_UP    0x1000u
#define TOD_PAD_RIGHT 0x2000u
#define TOD_PAD_DOWN  0x4000u
#define TOD_PAD_LEFT  0x8000u

static void tod_press(u32 raw)
{
	host_set_pad(0);
	tick(4);
	host_set_pad(raw);
	tick(4);
	host_set_pad(0);
	tick(4);
}

/* The title of the page qwark last posted, as it wrote it (the helper's copy is in capitals). */
static void posted_title(char *out)
{
	u32 k;

	for (k = 0; k < MBOX_TITLE_LEN && peek8(LM_BOX + MBOX_OFF_TITLE + k) != 0; k++) out[k] = (char)peek8(LM_BOX + MBOX_OFF_TITLE + k);
	out[k] = 0;
}

static void test_page_combos(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	const u32 game = TOD_PAGE_PAD(TOD_PAD_UP), cheats = TOD_PAGE_PAD(TOD_PAD_RIGHT);
	const u32 planets = TOD_PAGE_PAD(TOD_PAD_DOWN), manips = TOD_PAGE_PAD(TOD_PAD_LEFT);
	u32 mb = d->mailbox, req;
	char t[40];
	int i, r;

	group("ToD's console menu: the page combos, the pause first, then the page (revision 1.34)");
	config_set_combo(COMBO_PAGE_GAME, tod_pad_to_og(game));
	config_set_combo(COMBO_PAGE_CHEATS, tod_pad_to_og(cheats));
	config_set_combo(COMBO_PAGE_PLANETS, tod_pad_to_og(planets));
	config_set_combo(COMBO_PAGE_MANIPS, tod_pad_to_og(manips));
	tick(2);

	/* From play: an open, which the game's pause test takes as Start; the glue then opens the Trainer on the page. */
	tod_press(cheats);
	posted_title(t);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN && strcmp(t, "Cheats") == 0 && menu_model_depth() == 2,
	      "Cheats' combo in play: an open request, with Cheats already in the mailbox");
	check(lm_pause_check(0) == 1, "the game's pause test takes it, as a press of Start");
	pause_up();
	check(lm_take_request() == 1, "the pause up, the glue takes the pending open");
	lm_hstate(1);
	tick(4);
	lm_take();
	title(t);
	check(strcmp(t, "CHEATS") == 0 && lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG,
	      "the Trainer opens on Cheats, a CONFIG page (Game Options), not on its root");
	press(MBOX_EV_BACK, -1);
	title(t);
	r = find_row("Cheats");
	check(strcmp(t, "TRAINER") == 0 && r >= 0 && lm8(LMP(MBOX_OFF_CURSOR)) == (u8)r,
	      "Back: the Trainer's root, the cursor on Cheats");

	/* Up already: Manips, a section of ToD's own table, at once, nothing asked. */
	tod_press(manips);
	lm_take();
	title(t);
	check(strcmp(t, "MANIPS") == 0 && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE,
	      "with the Trainer up, Manips' combo shows Manips at once and asks for nothing");
	press(MBOX_EV_BACK, -1);
	lm_hstate(0);
	pause_gone();
	tick(30);

	/* Taken by the pause test, but the Trainer never up: four seconds, then the root for the next open. */
	tod_press(game);
	posted_title(t);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN && strcmp(t, "Game") == 0, "Game's combo: an open, on Game");
	lm_pause_check(0);
	for (i = 0; i < 360; i++) tick(1);
	posted_title(t);
	check(strcmp(t, "Game") == 0, "taken by the pause test: Game is kept a while for the glue to open on");
	for (i = 0; i < 240; i++) tick(1);
	posted_title(t);
	check(strcmp(t, "Trainer") == 0 && menu_model_depth() == 1,
	      "with no Trainer four seconds on, the root again, so the TRAINER row opens on it");
	for (i = 0; i < LM_PENDING_FRAMES; i++) lm_gameplay_frame();
	tick(4);

	/* Planets: the galactic map straight from play, no pause; Back is the Trainer's root, as from its row. */
	save_part_way();
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	req = peek32(mb + LUAMB_OFF_REQ);
	tod_press(planets);
	tick(6);
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE && peek32(mb + LUAMB_OFF_REQ) != req &&
	      peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_GAMEPLAY && save_for_map(),
	      "Planets' combo in play: no pause, the unlocks poked and gmap() to the gameplay VM");
	host_fake_lua_frame();
	tick(6);
	check(peek32(TOD_GAME_STATE) == TOD_STATE_GALMAP, "the galactic map up");
	host_fake_map(-1);
	tick(12);
	check(save_as_before() && peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN && peek32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "Back: the save's bytes again, nothing loaded, the Trainer asked to open");
	pause_up();
	lm_take_request();
	lm_hstate(1);
	tick(4);
	lm_take();
	title(t);
	r = find_row("Planets");
	check(strcmp(t, "TRAINER") == 0 && r >= 0 && lm8(LMP(MBOX_OFF_CURSOR)) == (u8)r,
	      "on the Trainer's root, the cursor on Planets");
	lm_hstate(0);
	pause_gone();
	tick(30);

	config_set_combo(COMBO_PAGE_GAME, 0);
	config_set_combo(COMBO_PAGE_CHEATS, 0);
	config_set_combo(COMBO_PAGE_PLANETS, 0);
	config_set_combo(COMBO_PAGE_MANIPS, 0);
	tick(2);
}

/* ------------------------------------------------------------ the glue */

static void test_glue(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u8 req[16];
	u8 reply[256];
	u32 seq, len, mb = d->mailbox;
	int done, i;

	group("ToD's console menu: the glue for each pause menu VM, and LUA_EXEC's menu target");

	/* The pause up: a menu system with a VM; its first frame asks for the glue. */
	host_set_fake_pause(1);
	seq = peek32(mb + LUAMB_OFF_REQ);
	host_fake_menu_frame();
	check(peek32(LM_STATE + LM_GLUE) == LM_GLUE_WANTED && peek32(LM_STATE + LM_LOADS) == 1,
	      "a new pause menu VM asks for the glue");
	tick(8);
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1 && peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_MENU &&
	      peek32(mb + LUAMB_OFF_CHUNK_LEN) == d->glue_len &&
	      memcmp(host_mem_ptr(mb + LUAMB_OFF_CHUNK), d->glue, d->glue_len) == 0,
	      "qwark sends it: a request for the menu's VM, the glue's bytes");
	check(peek32(mb + LUAMB_OFF_ACK) != seq + 1, "not answered before the pause menu's frame");
	host_fake_menu_frame();
	check(peek32(mb + LUAMB_OFF_ACK) == seq + 1 && peek32(LM_STATE + LM_GLUE) == LM_GLUE_LOADED,
	      "the pause menu's frame runs it: the glue loaded");
	tick(8);
	host_fake_menu_frame();
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1, "and it goes once: nothing more is sent");

	/* LUA_EXEC with flags bit0: the menu's VM. */
	memset(req, 0, sizeof(req));
	be16_put(req, 8);
	be16_put(req + 2, LUA_EXEC_MENU);
	memcpy(req + 4, "_VERSION", 8);
	check(lua_core_exec_start(req, 12, &seq) == ST_OK && peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_MENU,
	      "LUA_EXEC flags 1 in the pause: a request for the menu's VM");
	done = 0;
	for (i = 0; i < 20 && !done; i++) {
		host_fake_menu_frame();
		tick(1);
		lua_core_exec_result(seq, reply, sizeof(reply), &len, &done);
	}
	check(done && be32_get(reply) == LUAMB_ST_OK && memcmp(reply + LUA_EXEC_REPLY_HEAD, "Lua 5.0.2", 9) == 0,
	      "answered by the pause menu's frame");

	/* Out of the pause: the next pause is a new VM, which asks again. */
	host_set_fake_pause(0);
	host_fake_lua_frame();
	host_set_fake_pause(1);
	seq = peek32(mb + LUAMB_OFF_REQ);
	host_fake_menu_frame();
	tick(8);
	check(peek32(LM_STATE + LM_LOADS) == 2 && peek32(mb + LUAMB_OFF_REQ) == seq + 1,
	      "the next pause: asked and sent again");
	host_fake_menu_frame();
	host_set_fake_pause(0);
	host_fake_lua_frame();
	tick(4);
}

/* -------------------------------------------- the gate (the crash fix) */

/*
 * A menu system of the pause, laid out as AnarkMenuSystem_startPresentation
 * leaves it once the loading thread's first update ran (lh_tod.h).
 */
#define GATE_MS      0x30070000u
#define GATE_BRIDGE  0x30072000u
#define GATE_RUNTIME 0x30072100u
#define GATE_PRES    0x30072200u

static void gate_layout(void)
{
	poke32(LH_PAUSE_MS, GATE_MS);
	poke32(LH_PAUSE_STATE, 3);
	poke32(GATE_MS + LH_MS_STATE, 0x30074000u);
	poke32(GATE_MS + LH_MS_BRIDGE, GATE_BRIDGE);
	poke32(GATE_MS + LH_MS_RUNTIME, GATE_RUNTIME);
	poke32(GATE_MS + LH_MS_PRES, GATE_PRES);
	poke8(GATE_MS + LH_MS_FIRST, 0);
	poke8(GATE_MS + LH_MS_STOPPED, 0);
	poke32(GATE_MS + LH_MS_FRAMES, 2);
	poke32(GATE_RUNTIME + LH_RT_PUSHED, 0);
	poke32(GATE_BRIDGE + LH_BR_PRES, 0);
}

static void test_gate(void)
{
	u32 i;

	group("ToD's console menu: the helper enters the pause menu's VM only with its presentation up (the crash fix)");
	check(LH_FN_PRES_PUSH == 0x0079647Cu && LH_FN_PRES_POP == 0x0079558Cu && LH_MS_BRIDGE == 0x1058u &&
	      LH_MS_RUNTIME == 0x1068u && LH_MS_PRES == 0x1090u && LH_MS_FIRST == 0x10ABu && LH_BR_PRES == 8u &&
	      LH_RT_PUSHED == 0xBCu, "the runtime's push and pop, the menu system's words, as AnarkMenuSystem_reset uses them");

	gate_layout();
	check(lm_ready(GATE_MS, 0) == 0, "the pause up, its presentation started and nothing pushed: let in");
	check(lm_ready(0, 0) == LM_WHY_SYSTEM, "no menu system: kept out");
	poke32(LH_PAUSE_STATE, 1);
	check(lm_ready(GATE_MS, 0) == LM_WHY_SYSTEM, "g_pauseMenuState 1 (the loading thread at work): kept out");
	poke32(LH_PAUSE_STATE, 4);
	check(lm_ready(GATE_MS, 0) == LM_WHY_SYSTEM, "g_pauseMenuState 4 (closing): kept out");
	poke32(LH_PAUSE_STATE, 2);
	check(lm_ready(GATE_MS, 0) == 0, "g_pauseMenuState 2: let in");
	poke32(LH_PAUSE_MS, GATE_MS + 0x2000u);
	check(lm_ready(GATE_MS, 0) == LM_WHY_SYSTEM, "another menu system than g_pauseMenuSystem's: kept out");

	gate_layout();
	poke32(GATE_MS + LH_MS_STATE, 0);
	check(lm_ready(GATE_MS, 0) == LM_WHY_PARTS, "no lua_State: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_PRES, 0);
	check(lm_ready(GATE_MS, 0) == LM_WHY_PARTS, "no presentation: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_BRIDGE, 0);
	check(lm_ready(GATE_MS, 0) == LM_WHY_PARTS, "no Lua bridge: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_RUNTIME, 0);
	check(lm_ready(GATE_MS, 0) == LM_WHY_PARTS, "no runtime: kept out");

	gate_layout();
	poke8(GATE_MS + LH_MS_FIRST, 1);
	check(lm_ready(GATE_MS, 0) == LM_WHY_FIRST, "the presentation's first update still to come: kept out");
	gate_layout();
	poke8(GATE_MS + LH_MS_STOPPED, 1);
	check(lm_ready(GATE_MS, 0) == LM_WHY_STOPPED, "the menu system stopping: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_FRAMES, 1);
	check(lm_ready(GATE_MS, 0) == LM_WHY_YOUNG, "one update only: kept out");
	gate_layout();
	poke32(GATE_RUNTIME + LH_RT_PUSHED, 1);
	check(lm_ready(GATE_MS, 0) == LM_WHY_PUSHED, "a presentation pushed already: kept out");
	gate_layout();
	poke32(GATE_BRIDGE + LH_BR_PRES, GATE_PRES);
	check(lm_ready(GATE_MS, 0) == LM_WHY_PUSHED, "the bridge holding a presentation: kept out");
	poke8(GATE_MS + LH_MS_FIRST, 1);
	poke32(GATE_MS + LH_MS_FRAMES, 0);
	check(lm_ready(GATE_MS, 0) == (LM_WHY_FIRST | LM_WHY_YOUNG | LM_WHY_PUSHED), "every reason at once, each said");

	/*
	 * Revision 1.29, the galactic map (docs/tod-menu.md 17): the map's menu
	 * system the same words, its own first test (g_gameState 6), the pause's
	 * words no matter.
	 */
	gate_layout();
	poke32(LH_PAUSE_MS, 0);
	poke32(LH_PAUSE_STATE, 0);
	poke32(LH_GAME_STATE, LH_STATE_MAP);
	check(lm_ready(GATE_MS, 1) == 0, "the map up (g_gameState 6), its presentation started: let in, the pause's words no matter");
	check(lm_ready(GATE_MS, 0) == LM_WHY_SYSTEM, "the same menu system as the pause's: kept out");
	poke32(LH_GAME_STATE, 4);
	check(lm_ready(GATE_MS, 1) == LM_WHY_SYSTEM, "g_gameState 4 (play, the map gone): kept out");
	poke32(LH_GAME_STATE, LH_STATE_MAP);
	poke8(GATE_MS + LH_MS_FIRST, 1);
	check(lm_ready(GATE_MS, 1) == LM_WHY_FIRST, "the map's first update still to come: kept out");
	gate_layout();
	poke32(GATE_RUNTIME + LH_RT_PUSHED, 1);
	poke32(LH_GAME_STATE, LH_STATE_MAP);
	check(lm_ready(GATE_MS, 1) == LM_WHY_PUSHED, "a presentation pushed already: kept out");
	poke32(LH_GAME_STATE, 4);

	/* The map's pick (qw LM_OP_PICK): a level into g_galMap.selectedLevel, nothing else. */
	poke32(LH_MAP_PICK, 0xFFFFFFFFu);
	lm_pick(0);
	check(lm32(LH_MAP_PICK) == 0, "the map's pick of Kerwan (0): selectedLevel 0");
	lm_pick(18);
	check(lm32(LH_MAP_PICK) == 18, "of Fastoon II (18): 18");
	lm_pick(19);
	lm_pick(0xFFFFFFFFu);
	check(lm32(LH_MAP_PICK) == 18, "19 and -1 are no level: left as it was");
	check(LH_MAP_MS == 0x101CBF80u && LH_MAP_PICK == 0x101CBFC0u && LH_GAME_STATE == TOD_GAME_STATE &&
	      LH_STATE_MAP == TOD_STATE_GALMAP && LH_MAP_PICK == TOD_GALMAP_PICK,
	      "the helper's map words are tod.h's (g_galMapSystem, selectedLevel, g_gameState 6)");
	poke32(LH_MAP_PICK, 0xFFFFFFFFu);

	/* The push taking: the bridge holds the presentation (AnarkLuaBridge_onPresentationPush). */
	gate_layout();
	check(!lm_pushed(GATE_MS), "nothing pushed: the bridge holds no presentation");
	poke32(GATE_BRIDGE + LH_BR_PRES, GATE_PRES);
	check(lm_pushed(GATE_MS), "pushed: it holds the menu system's");
	poke32(GATE_BRIDGE + LH_BR_PRES, GATE_PRES + 0x100u);
	check(!lm_pushed(GATE_MS), "another presentation: not taken");

	/* The frames kept out, counted for qwark's log. */
	lm_kept_out(0);
	for (i = 0; i < 5; i++) lm_kept_out(LM_WHY_FIRST);
	check(lm32(LMS(LM_WAITS)) == 5 && lm32(LMS(LM_WHY)) == LM_WHY_FIRST, "five frames kept out: counted, the reason kept");
	lm_kept_out(0);
	check(lm32(LMS(LM_WAITS)) == 0 && lm32(LMS(LM_WHY)) == 0, "a frame let in: the count from 0 again");
	lm_kept_out(LM_WHY_PARTS);
	lm_gameplay_frame();
	check(lm32(LMS(LM_WAITS)) == 0, "a gameplay frame: no pause, no wait");

	/* The core says a long wait once, and the glue's notes. */
	poke32(LH_PAUSE_MS, 0);
	poke32(LH_PAUSE_STATE, 0);
	check(wait_ready() || menu_state() == MENU_READY, "the menu is in");
	host_log_clear();
	poke32(LM_STATE + LM_WHY, LM_WHY_FIRST);
	poke32(LM_STATE + LM_WAITS, 299);
	tick(4);
	check(!host_log_seen("not ready for the Trainer"), "a short wait: nothing said");
	poke32(LM_STATE + LM_WAITS, 300);
	tick(4);
	check(host_log_seen("menu: the pause menu is not ready for the Trainer after 300 frames (why 0x4)"),
	      "a long one: said, with its reasons");
	host_log_clear();
	poke32(LM_STATE + LM_WAITS, 400);
	tick(4);
	check(!host_log_seen("not ready for the Trainer"), "and once");
	poke32(LM_STATE + LM_WAITS, 0);
	poke32(LM_STATE + LM_WHY, 0);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_IN);
	tick(4);
	check(host_log_seen("menu: the pause menu's glue is in: TRAINER in the pause list for Inventory"), "the glue in: said");
	host_log_clear();
	poke32(LM_STATE + LM_NOTE, 0);
	tick(4);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_IN);
	tick(4);
	check(!host_log_seen("glue is in"), "the next pause the same: not said again");
	poke32(LM_STATE + LM_NOTE, 0);
	poke32(LH_PAUSE_MS, 0);
	poke32(LH_PAUSE_STATE, 0);
}

/*
 * Revision 1.29, the third console test (docs/tod-menu.md 11): the glue's
 * finer notes, and the helper's look (lh_diag.lua) qwark sends once after a
 * failure of each kind, its answer logged in pieces a log line holds. The
 * glue's own cases are test/test_glue.py's, which runs it in a real Lua.
 */
static void test_diag(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u32 mb = d->mailbox, seq;
	int i;

	group("ToD's console menu: the glue's notes, and the look qwark sends when it found nothing");
	check(wait_ready() || menu_state() == MENU_READY, "the menu is in");
	host_set_fake_pause(1);
	for (i = 0; i < 4; i++) {
		host_fake_menu_frame();
		tick(8);
	}
	check(peek32(LM_STATE + LM_GLUE) == LM_GLUE_LOADED, "a pause, its glue in");

	host_log_clear();
	poke32(LM_STATE + LM_NOTE, LM_NOTE_IN_LIST);
	tick(4);
	check(host_log_seen("menu: the pause menu's glue is in: TRAINER in the pause list for Inventory, Game Options met when the "
	                    "Trainer shows a page there"), "the list in, Game Options not met yet: said");
	poke32(LM_STATE + LM_NOTE, LM_NOTE_LATE);
	tick(4);
	check(host_log_seen("menu: the Trainer met the game's Game Options screen on its slide; its page is in"),
	      "the Cheats screen met on its slide: said");

	/* No pause list: said, and the look sent, a request for the menu's VM. */
	seq = peek32(mb + LUAMB_OFF_REQ);
	host_log_clear();
	poke32(LM_STATE + LM_NOTE, LM_NOTE_NONE);
	tick(8);
	check(host_log_seen("menu: the pause menu's glue found no pause list; Inventory stays the game's"),
	      "no pause list: said");
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1 && peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_MENU &&
	      peek32(mb + LUAMB_OFF_CHUNK_LEN) == d->diag_len &&
	      memcmp(host_mem_ptr(mb + LUAMB_OFF_CHUNK), d->diag, d->diag_len) == 0,
	      "and the look goes to the pause menu's VM");
	host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_ACK) == seq + 1 &&
	      host_log_seen("menu: what the glue saw (1/2): g=301,301,1,0,-,0 s=uuutffn sl=nil reg=t,23,23,4 ") &&
	      host_log_seen("menu: what the glue saw (2/2): ") && host_log_seen("ListUpdater:mh+ end"),
	      "its answer in qwark.log, in two lines (a log line holds 180 characters of it)");
	check(!host_log_seen("lua: request"), "and not as a LUA_EXEC's answer");

	/* Once: the next pause that finds no list sends nothing more. */
	poke32(LM_STATE + LM_NOTE, 0);
	tick(4);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_NONE);
	tick(8);
	host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1, "once a session: no second look for the same failure");

	/* No Cheats screen: said, and its own look, here into a pool short of a load's room. */
	host_log_clear();
	host_set_fake_pause(4);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_NOCH);
	tick(8);
	check(host_log_seen("menu: the Trainer found no Game Options screen on its slide and went back to the pause's list"),
	      "the Trainer found no Cheats screen: said");
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 2 && peek32(mb + LUAMB_OFF_CHUNK_LEN) == d->diag_len,
	      "and its look sent");
	host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_ACK) == seq + 2 &&
	      host_log_seen("menu: the glue's look was not run: the pause menu's Lua pool is short"),
	      "a pool short of a load's room: NO_ROOM, said");
	check(!host_log_seen("what the glue saw"), "and nothing else");

	/* The tight pool's shortage was counted and said for this pause: the next tests start from none, in another. */
	poke32(LM_STATE + LM_NOTE, 0);
	host_set_fake_pause(0);
	host_fake_lua_frame();
	poke32(LM_STATE + LM_SHORT, 0);
	tick(4);
	poke32(LM_STATE + LM_LOADS, peek32(LM_STATE + LM_LOADS) + 1u);
	tick(4);
}

/* The fake console's pause whose presentation never comes up: the helper never enters it, the core says so. */
static void test_gate_fake(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u32 loads = peek32(LM_STATE + LM_LOADS), seq = peek32(d->mailbox + LUAMB_OFF_REQ);
	int i;

	group("ToD's console menu: a pause that never comes up is never entered");
	host_log_clear();
	host_set_fake_pause(2);
	for (i = 0; i < 310; i++) host_fake_menu_frame();
	tick(8);
	check(peek32(LM_STATE + LM_LOADS) == loads && peek32(d->mailbox + LUAMB_OFF_REQ) == seq &&
	      peek32(LM_STATE + LM_WAITS) >= 300 && peek32(LM_STATE + LM_WHY) == LM_WHY_FIRST,
	      "no glue asked for, none sent; the frames kept out counted");
	check(host_log_seen("not ready for the Trainer"), "and the log says so");
	host_set_fake_pause(1);
	host_fake_menu_frame();
	tick(8);
	check(peek32(LM_STATE + LM_WAITS) == 0 && peek32(LM_STATE + LM_LOADS) == loads + 1,
	      "the presentation up: let in, the glue asked for");
	host_fake_menu_frame();
	host_set_fake_pause(0);
	host_fake_lua_frame();
	tick(4);
}

/* --------------------------------- the Lua pool (the second crash) */

/*
 * A LUA::PoolAllocator as LuaPoolAllocator_setup lays one out (lh_tod.h
 * LH_PA_*, LH_PC_*), four classes of 8, 24, 64 and 640 bytes, whose blocks
 * the tests hand out (used) and free (onto a chunk's list) by hand.
 */
#define POOL        0x30080000u
#define POOL_DESC   0x30080800u
#define POOL_CHUNKS 0x30081000u
#define POOL_BLOCKS 0x30090000u

static const u32 pool_size[4] = { 8, 24, 64, 640 };
static const u32 pool_count[4] = { 16, 10, 6, 4 };

static u32 pool_chunk(u32 k) { return POOL_CHUNKS + k * LH_PC_SIZE; }
static u32 pool_block(u32 k, u32 i) { return POOL_BLOCKS + k * 0x4000u + i * (pool_size[k] + 4u); }

static void pool_layout(void)
{
	u32 k, i;

	poke32(POOL + LH_PA_DESC, POOL_DESC);
	poke32(POOL + LH_PA_CHUNKS, POOL_CHUNKS);
	poke32(POOL_DESC, 4);
	for (k = 0; k < 4; k++) poke32(POOL_DESC + 4u + 4u * k, pool_size[k]);
	for (i = 0; i < 160; i++) {
		u32 size = 4u * i + 4u;
		poke32(POOL + LH_PA_INDEX + 4u * i, size <= 8 ? 0 : size <= 24 ? 1 : size <= 64 ? 2 : 3);
	}
	for (k = 0; k < 4; k++) {
		u32 c = pool_chunk(k);

		poke32(c + LH_PC_BASE, pool_block(k, 0));
		poke32(c + LH_PC_FREE, 0);
		poke32(c + LH_PC_BYTES, pool_count[k] * (pool_size[k] + 4u));
		poke32(c + LH_PC_STRIDE, pool_size[k] + 4u);
		poke32(c + LH_PC_USED, 0);
	}
}

static void pool_use_up(u32 k)
{
	poke32(pool_chunk(k) + LH_PC_USED, peek32(pool_chunk(k) + LH_PC_BYTES));
}

/* Block i of class k freed, as LuaPoolAllocator_realloc frees one: its first word the old head. */
static void pool_free(u32 k, u32 i)
{
	poke32(pool_block(k, i), peek32(pool_chunk(k) + LH_PC_FREE));
	poke32(pool_chunk(k) + LH_PC_FREE, pool_block(k, i));
}

static void test_room(void)
{
	u32 r, i;

	group("ToD's console menu: the Anark Lua pool counted before the menu's Lua runs (the second crash)");
	check(LH_ANARK_POOL == 0x1073DC40u && LH_FN_SETGCTHRESHOLD == 0x00691504u && LH_PC_SIZE == 0x20u &&
	      LH_PC_FREE == 4u && LH_PC_STRIDE == 0xCu && LH_PC_USED == 0x10u && LH_PA_INDEX == 0xCu,
	      "g_anarkLuaAllocator's words, as LuaPoolAllocator_alloc reads them");

	pool_layout();
	r = lm_room(POOL, 1000, 100);
	check(r == ((4u << 16) | 20u) && lm32(LMS(LM_ROOM)) == r,
	      "a fresh pool: 20 blocks for a 20-byte object (24, 64 and 640 bytes), 4 of the largest; kept in LM_ROOM");
	check(lm_room_ok(r, 20, 4) && !lm_room_ok(r, 21, 4) && !lm_room_ok(r, 20, 5), "and judged against what is wanted");

	pool_use_up(1);
	pool_use_up(2);
	pool_use_up(3);
	r = lm_room(POOL, 1000, 100);
	check(r == 0 && !lm_room_ok(r, LM_ROOM_FLOOR_SMALL, LM_ROOM_FLOOR_BIG),
	      "the console's crash: every class from 24 bytes up used up, the 8-byte class not: no room at all");

	pool_free(1, 2);
	pool_free(1, 7);
	pool_free(1, 4);
	pool_free(3, 1);
	pool_free(3, 3);
	check(lm_room(POOL, 1000, 100) == ((2u << 16) | 5u), "blocks freed onto the lists: counted, the largest class's too");
	check(lm_room(POOL, 2, 1) == ((1u << 16) | 2u), "counted only as far as wanted, from the largest class down");
	poke32(pool_block(1, 4), 0x801Bu);
	check(lm_room(POOL, 1000, 100) == ((2u << 16) | 3u), "a link that is no block of its chunk ends the count");
	poke32(pool_block(1, 4), pool_block(1, 7) + 2u);
	check(lm_room(POOL, 1000, 100) == ((2u << 16) | 3u), "nor is one between two blocks");

	pool_layout();
	poke32(pool_chunk(2) + LH_PC_STRIDE, 0);
	check(lm_room(POOL, 1000, 100) == ((4u << 16) | 14u), "a chunk with no stride counts nothing, and no division by it");
	pool_layout();
	poke32(POOL + LH_PA_DESC, 0);
	check(lm_room(POOL, 1000, 100) == 0, "a pool released (Memory_FreeAnarkHeaps clears its table): no room");
	pool_layout();
	poke32(POOL_DESC, LH_PA_CLASSES + 1u);
	check(lm_room(POOL, 1000, 100) == 0, "a table of too many classes: no room");
	pool_layout();
	poke32(POOL + LH_PA_INDEX + 4u * ((LM_ROOM_BLOCK - 1u) / 4u), 0xFFFFFFFFu);
	check(lm_room(POOL, 1000, 100) == 0, "no class for a 20-byte object: no room");
	pool_layout();
	poke32(POOL + LH_PA_CHUNKS, 0);
	check(lm_room(POOL, 1000, 100) == 0, "no chunks: no room");

	/* The collections: never under the floor, held off after one that did not help. */
	poke32(LM_STATE + LM_HOLD, 0);
	poke32(LM_STATE + LM_SHORT, 0);
	check(lm_may_collect((LM_ROOM_FLOOR_BIG << 16) | LM_ROOM_FLOOR_SMALL), "short but over the floor: a collection");
	check(!lm_may_collect((LM_ROOM_FLOOR_BIG << 16) | (LM_ROOM_FLOOR_SMALL - 1u)) &&
	      !lm_may_collect(((LM_ROOM_FLOOR_BIG - 1u) << 16) | 0xFFFFu), "under the floor: none (it allocates too)");
	lm_short();
	check(lm32(LMS(LM_SHORT)) == 1 && lm32(LMS(LM_HOLD)) == LM_ROOM_HOLD, "a collection that left it short: counted, the next held");
	for (i = 0; i < LM_ROOM_HOLD; i++)
		if (lm_may_collect(0xFFFFFFFFu)) break;
	check(i == LM_ROOM_HOLD && lm_may_collect(0xFFFFFFFFu), "held for LM_ROOM_HOLD frames, then one again");
	lm_short();
	lm_gameplay_frame();
	check(lm32(LMS(LM_HOLD)) == 0, "a gameplay frame: the next pause starts unheld");
	check(LM_ROOM_SMALL < LM_ROOM_LOAD_SMALL && LM_ROOM_BIG < LM_ROOM_LOAD_BIG &&
	      LM_ROOM_FLOOR_SMALL < LM_ROOM_SMALL && LM_ROOM_FLOOR_BIG < LM_ROOM_BIG,
	      "a load wants more than a frame, a frame more than a collection");
	poke32(LM_STATE + LM_SHORT, 0);
	poke32(LM_STATE + LM_ROOM, 0);
}

/* The core's lines for the pool, and the fake console's short pools. */
static void test_room_log(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u32 mb = d->mailbox, seq, loads;
	int i;

	group("ToD's console menu: a short Lua pool keeps the Trainer out, and the log says so");
	check(wait_ready() || menu_state() == MENU_READY, "the menu is in");
	host_log_clear();
	loads = peek32(LM_STATE + LM_LOADS);
	poke32(LM_STATE + LM_ROOM, (2u << 16) | 300u);
	poke32(LM_STATE + LM_SHORT, 1);
	tick(4);
	check(host_log_seen("menu: the pause menu's Lua pool is short even after a collection (300 blocks left for small "
	                    "objects, 2 in its largest class); the Trainer stays out of it, Inventory is the game's"),
	      "a collection that left the pool short: said, with what was left");
	host_log_clear();
	poke32(LM_STATE + LM_SHORT, 2);
	tick(4);
	check(!host_log_seen("Lua pool is short"), "again in the same pause: not said again");
	poke32(LM_STATE + LM_LOADS, loads + 1u);
	poke32(LM_STATE + LM_SHORT, 3);
	tick(4);
	check(host_log_seen("Lua pool is short"), "in the next pause: said again");
	poke32(LM_STATE + LM_LOADS, loads);
	poke32(LM_STATE + LM_ROOM, (150u << 16) | 1024u);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_NONE);
	tick(4);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_IN);
	tick(4);
	check(host_log_seen("menu: the pause menu's glue is in: TRAINER in the pause list for Inventory (its Lua pool: 1024 blocks or more "
	                    "left for small objects, 150 in its largest class)"), "the glue in: said with the pool's room");
	poke32(LM_STATE + LM_NOTE, 0);

	/* The fake console: a pool short even after collections, every frame kept out. */
	host_set_fake_pause(0);
	host_fake_lua_frame();
	host_log_clear();
	loads = peek32(LM_STATE + LM_LOADS);
	seq = peek32(mb + LUAMB_OFF_REQ);
	host_set_fake_pause(3);
	for (i = 0; i < 70; i++) host_fake_menu_frame();
	tick(8);
	check(peek32(LM_STATE + LM_WHY) == LM_WHY_MEMORY && peek32(LM_STATE + LM_LOADS) == loads &&
	      peek32(mb + LUAMB_OFF_REQ) == seq, "a pause with a short pool: never entered, no glue asked for or sent");
	check(host_log_seen("Lua pool is short even after a collection (300 blocks left"), "and the log says so");

	/* A pool with room for a frame but not for the glue's load: the request answered NO_ROOM, said once. */
	host_set_fake_pause(0);
	host_fake_lua_frame();
	host_log_clear();
	host_set_fake_pause(4);
	host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1 && peek8(mb + LUAMB_OFF_TARGET) == LUAMB_TARGET_MENU,
	      "a tight pool: the glue asked for and sent");
	host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_ACK) == seq + 1 && (s32)peek32(mb + LUAMB_OFF_STATUS) == LUAMB_ST_NO_ROOM &&
	      peek32(LM_STATE + LM_GLUE) == LM_GLUE_WANTED, "the helper answers NO_ROOM and runs nothing");
	check(host_log_seen("menu: the pause menu's glue was not run: its Lua pool is short even after a collection; "
	                    "Inventory stays the game's"), "the log says so");
	for (i = 0; i < 4; i++) host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1, "and nothing is sent again in that pause");
	host_set_fake_pause(0);
	host_fake_lua_frame();
	tick(4);
}

/* ---------------------------------------------------------------- RPCS3 */

static void test_rpcs3(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u32 n;
	int i;

	group("ToD's console menu under RPCS3: the Lua helper's patch, data writes alone");
	check(quit(), "the game quits");
	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(boot_tod(), "ToD boots under RPCS3 without the patch");
	tick(600);
	check(menu_state() != MENU_READY && peek32(LM_BOX) == 0 && hooks_in(d, 0), "no patch: no menu, nothing written");
	n = host_notify_count();
	host_set_pad(0);
	tick(2);
	host_set_pad(TOD_COMBO_MENU);
	tick(2);
	host_set_pad(0);
	tick(2);
	check(host_notify_count() == n + 1 && strstr(host_notify_last(), "RPCS3 patch") != NULL,
	      "the combo says it needs qwark's RPCS3 patch");
	check(quit(), "the game quits");
	host_set_lua_patch(1);
	check(boot_tod(), "and boots with the patch in");
	for (i = 0; i < 1500 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY && peek32(LM_BOX) == MBOX_MAGIC && hooks_in(d, 1),
	      "the menu goes in: the mailbox written over PINE, the hooks the patch's");
	/* Revision 1.34: the page combos work here as the menu does; Mods, which RPCS3 has not, opens nothing. */
	config_set_combo(COMBO_PAGE_MODS, tod_pad_to_og(TOD_PAGE_PAD(TOD_PAD_UP)));
	config_set_combo(COMBO_PAGE_UNLOCKS, tod_pad_to_og(TOD_PAGE_PAD(TOD_PAD_DOWN)));
	tick(2);
	n = host_notify_count();
	tod_press(TOD_PAGE_PAD(TOD_PAD_UP));
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_NONE && host_notify_count() == n + 1 &&
	      strcmp(host_notify_last(), "Trainer: no Mods page in this game") == 0,
	      "Mods' combo under RPCS3: no Mods page, nothing asked, and the console says so");
	tod_press(TOD_PAGE_PAD(TOD_PAD_DOWN));
	check(peek8(LM_BOX + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN && menu_model_page() == MENU_PAGE_ID(MENU_PAGE_UNLOCKS, 0),
	      "while Unlocks' asks to open on Unlocks, as on a console");
	config_set_combo(COMBO_PAGE_MODS, 0);
	config_set_combo(COMBO_PAGE_UNLOCKS, 0);
	check(quit(), "the game quits");
	host_set_lua_patch(0);
	host_set_can_patch_code(1);
	host_set_emulator(0);
}

void test_tod_menu(void)
{
	test_install();
	test_bridge();
	test_screens();
	test_paging();
	test_requests();
	test_pause_loads();
	test_galmap();
	test_page_combos();
	test_glue();
	test_gate();
	test_diag();
	test_gate_fake();
	test_room();
	test_room_log();
	test_rpcs3();
	check(quit() || session_state() == SESSION_XMB, "ToD quits");
}
