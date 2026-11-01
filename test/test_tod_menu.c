/*
 * Tools of Destruction's console menu (revision 1.29, pending): the game's
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

static int find_row(const char *label)
{
	char b[48];
	u32 i;

	for (i = 0; i < lm_nrows(); i++) {
		if (strcmp(row_label(i, b), label) == 0) return (int)i;
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
	u32 gen;
	int i;

	group("ToD's console menu: the install (the game's own pause menu)");
	check(d != NULL && d->nhooks == 4 && d->hooks[2].addr == 0x000242B0u && d->hooks[2].orig == 0x4BFED721u &&
	      d->hooks[3].addr == 0x004C5ABCu && d->hooks[3].orig == 0x4BDA1825u,
	      "the Lua helper has four hooks: the VM's, the error tap, the pause menu's update and the pause test");
	if (d == NULL) return;
	check(d->menu_ptr == 0x101CB220u && d->menu_off == 0x106Cu && d->glue != NULL &&
	      d->glue_len > 1000 && d->glue_len <= LUAMB_CHUNK_CAP,
	      "and the pause menu's VM, and its glue, which fits one request");
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
	      peek8(LM_BOX + MBOX_OFF_ROW_SIZE) == MBOX_ROW_SIZE && peek8(LM_BOX + MBOX_OFF_MAX_ROWS) == LM_ROWS &&
	      peek8(LM_BOX + MBOX_OFF_CFG_ROWS) == LM_ROWS && peek8(LM_BOX + MBOX_OFF_CAPS) == LM_CAPS &&
	      peek8(LM_BOX + MBOX_OFF_RENDERER) == GAME_TOD && (peek32(LM_BOX + 4) & 0xFFFF) == LM_BOX_SIZE,
	      "the menu's mailbox, written by the core: 32 rows of either type, Left/Right, back and requests, game 5");
	check(peek8(LM_STATE + LM_HELP) == 1, "menu.help 1: the Help byte says Trainer");
	check(peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) != 0 && (peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) & 1) == 0,
	      "and the root is posted");

	/* menu.help 0 and back: the byte alone, at once. */
	config_set_u32("menu.help", 0);
	tick(8);
	check(peek8(LM_STATE + LM_HELP) == 0, "menu.help 0: the game's own Help");
	config_set_u32("menu.help", 1);
	tick(8);
	check(peek8(LM_STATE + LM_HELP) == 1, "menu.help 1 again");

	/* menu.enabled 0: the identity goes, so the helper and the glue let go; 1: back. */
	gen = peek32(LM_BOX + MBOX_OFF_PAGE_SEQ);
	config_set_u32("menu.enabled", 0);
	tick(8);
	check(menu_state() == MENU_IDLE && peek32(LM_BOX) == 0 && hooks_in(d, 1),
	      "menu.enabled 0: the mailbox's identity cleared, the Lua helper left in");
	check(!lm_on(), "the helper's half sees no menu");
	config_set_u32("menu.enabled", 1);
	for (i = 0; i < 1500 && menu_state() != MENU_READY; i++) tick(1);
	check(menu_state() == MENU_READY && peek32(LM_BOX) == MBOX_MAGIC && peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) != 0 &&
	      (peek32(LM_BOX + MBOX_OFF_PAGE_SEQ) & 1) == 0 && gen != 0,
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
	check(strcmp(t, "Trainer") == 0 && find_row("Game") == 0 && find_row("Cheats") == 1 &&
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
	check(lm32(LMS(LM_GEN)) == gen + 1 && strcmp(t, "Cheats") == 0 && lm8(LMP(MBOX_OFF_PTYPE)) == MBOX_PTYPE_CONFIG,
	      "qwark answers with Cheats, a CONFIG page, a new key");
	check(find_row("Fast loads") == 0 && find_row("Infinite ammo") >= 0 && find_row("Remove cutscenes") < 0 &&
	      find_row("Max bolts") >= 0, "Cheats: Fast loads, the toggles, Max bolts; Remove cutscenes not here");
	r = find_row("Infinite ammo");
	check(r >= 0 && lm8(lm_row((u32)r) + MROW_OFF_KIND) == MROW_TOGGLE && strcmp(row_value((u32)r, b), "Off") == 0 &&
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
	check((features_toggle_state() & (1ull << TF_INFINITE_AMMO)) != 0 && strcmp(row_value((u32)r, b), "On") == 0,
	      "qwark turns Infinite ammo on, and the page it posts says On");
	check(press(MBOX_EV_LEFT, r) && (features_toggle_state() & (1ull << TF_INFINITE_AMMO)) == 0,
	      "Left steps it back off");

	/* Back, and the Manips page with Remove cutscenes. */
	gen = lm32(LMS(LM_GEN));
	check(press(MBOX_EV_BACK, -1) && lm32(LMS(LM_GEN)) == gen + 1 && find_row("Game") == 0, "back: the root again");
	press(MBOX_EV_ACTIVATE, find_row("Manips"));
	title(t);
	check(strcmp(t, "Manips") == 0 && find_row("Remove cutscenes") == 0, "Manips: Remove cutscenes, as the client has it");
	check(find_row("Cache cutscenes") == 1 && find_row("Cutscene cache") == 2 &&
	      find_row("Cache cutscenes on reset") == 3 && find_row("Disable cutscene caching") == 4,
	      "revision 1.29: then Cache cutscenes with its status row under it, and its two toggles");
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
	check(lm_ready(GATE_MS) == 0, "the pause up, its presentation started and nothing pushed: let in");
	check(lm_ready(0) == LM_WHY_SYSTEM, "no menu system: kept out");
	poke32(LH_PAUSE_STATE, 1);
	check(lm_ready(GATE_MS) == LM_WHY_SYSTEM, "g_pauseMenuState 1 (the loading thread at work): kept out");
	poke32(LH_PAUSE_STATE, 4);
	check(lm_ready(GATE_MS) == LM_WHY_SYSTEM, "g_pauseMenuState 4 (closing): kept out");
	poke32(LH_PAUSE_STATE, 2);
	check(lm_ready(GATE_MS) == 0, "g_pauseMenuState 2: let in");
	poke32(LH_PAUSE_MS, GATE_MS + 0x2000u);
	check(lm_ready(GATE_MS) == LM_WHY_SYSTEM, "another menu system than g_pauseMenuSystem's: kept out");

	gate_layout();
	poke32(GATE_MS + LH_MS_STATE, 0);
	check(lm_ready(GATE_MS) == LM_WHY_PARTS, "no lua_State: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_PRES, 0);
	check(lm_ready(GATE_MS) == LM_WHY_PARTS, "no presentation: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_BRIDGE, 0);
	check(lm_ready(GATE_MS) == LM_WHY_PARTS, "no Lua bridge: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_RUNTIME, 0);
	check(lm_ready(GATE_MS) == LM_WHY_PARTS, "no runtime: kept out");

	gate_layout();
	poke8(GATE_MS + LH_MS_FIRST, 1);
	check(lm_ready(GATE_MS) == LM_WHY_FIRST, "the presentation's first update still to come: kept out");
	gate_layout();
	poke8(GATE_MS + LH_MS_STOPPED, 1);
	check(lm_ready(GATE_MS) == LM_WHY_STOPPED, "the menu system stopping: kept out");
	gate_layout();
	poke32(GATE_MS + LH_MS_FRAMES, 1);
	check(lm_ready(GATE_MS) == LM_WHY_YOUNG, "one update only: kept out");
	gate_layout();
	poke32(GATE_RUNTIME + LH_RT_PUSHED, 1);
	check(lm_ready(GATE_MS) == LM_WHY_PUSHED, "a presentation pushed already: kept out");
	gate_layout();
	poke32(GATE_BRIDGE + LH_BR_PRES, GATE_PRES);
	check(lm_ready(GATE_MS) == LM_WHY_PUSHED, "the bridge holding a presentation: kept out");
	poke8(GATE_MS + LH_MS_FIRST, 1);
	poke32(GATE_MS + LH_MS_FRAMES, 0);
	check(lm_ready(GATE_MS) == (LM_WHY_FIRST | LM_WHY_YOUNG | LM_WHY_PUSHED), "every reason at once, each said");

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
	check(host_log_seen("menu: the pause menu's glue is in: TRAINER in Options"), "the glue in: said");
	host_log_clear();
	poke32(LM_STATE + LM_NOTE, 0);
	tick(4);
	poke32(LM_STATE + LM_NOTE, LM_NOTE_IN);
	tick(4);
	check(!host_log_seen("glue is in"), "the next pause the same: not said again");
	poke32(LM_STATE + LM_NOTE, LM_NOTE_NONE);
	tick(4);
	check(host_log_seen("menu: the pause menu's glue found no Options list or Cheats screen; Help stays the game's"),
	      "a pause where it found nothing: said");
	poke32(LM_STATE + LM_NOTE, 0);
	poke32(LH_PAUSE_MS, 0);
	poke32(LH_PAUSE_STATE, 0);
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
	                    "objects, 2 in its largest class); the Trainer stays out of it, Help is the game's"),
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
	check(host_log_seen("menu: the pause menu's glue is in: TRAINER in Options (its Lua pool: 1024 blocks or more "
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
	                    "Help stays the game's"), "the log says so");
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
	check(quit(), "the game quits");
	host_set_lua_patch(0);
	host_set_can_patch_code(1);
	host_set_emulator(0);
}

void test_tod_menu(void)
{
	test_install();
	test_bridge();
	test_requests();
	test_glue();
	test_gate();
	test_gate_fake();
	test_room();
	test_room_log();
	test_rpcs3();
	check(quit() || session_state() == SESSION_XMB, "ToD quits");
}
