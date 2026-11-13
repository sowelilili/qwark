/*
 * Revision 1.31, the VSH pad probe (src/core/vshpad.c): the input display's
 * pad at the XMB, on the fake console, whose VSH libpad answers whatever
 * host_set_vsh_pad says, hands a change over once and then "same as before"
 * (len 0), as libpad does.
 *
 * libpad's digital bytes to the OG layout, bit by bit and against ToD's own
 * remap; the defer rule as SessionInfo carries it (the VSH's view where it has
 * something held, else the running game's pad, else nothing), buttons and
 * sticks decided apart, the dead zone, a view gone stale, a view left over
 * from the XMB as a game starts, the last game's pad at the XMB, an error and
 * no pad API at all falling back to the game's pad, no read without a client,
 * and the combos still on the game's own pad. The wire itself is smoke's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/vshpad.h"
#include "../src/games/game.h"
#include "../src/games/tod.h"
#include "../src/games/rac1.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define ERR_NO_DEVICE 0x80121107u    /* CELL_PAD_ERROR_NO_DEVICE */

static const u8 CENTRE[4] = { 0x80, 0x80, 0x80, 0x80 };

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
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	return session_state() == SESSION_INGAME && session_game() != NULL;
}

static int quit(void)
{
	int i;

	host_quit();
	for (i = 0; i < 2000 && session_state() != SESSION_XMB; i++) tick(1);
	return session_state() == SESSION_XMB;
}

/* SessionInfo's pad: the mask at +44 and rx, ry, lx, ly at +48. */
static u32 info_pad(f32 analog[4])
{
	u8 b[SESSION_INFO_SIZE];
	int i;

	memset(b, 0, sizeof(b));
	session_info_copy(b, sizeof(b));
	for (i = 0; i < 4 && analog != NULL; i++) analog[i] = bef32_get(b + 48 + i * 4);
	return be32_get(b + 44);
}

/* The VSH's libpad holds the OG mask's two bytes and these sticks; a telemetry frame or two later. */
static void vsh(u32 og, const u8 stick[4])
{
	host_set_vsh_pad(0, (u8)(og >> 8), (u8)og, stick != NULL ? stick : CENTRE);
	tick(8);
}

static int near(f32 a, f32 b)
{
	f32 d = a - b;
	return d < 0.001f && d > -0.001f;
}

/* ------------------------------------------------------------ the layout */

static void mapping(void)
{
	/*
	 * libpad's CELL_PAD_CTRL_* (pad_codes.h): DIGITAL1 select 0x01, L3 0x02,
	 * R3 0x04, start 0x08, up 0x10, right 0x20, down 0x40, left 0x80;
	 * DIGITAL2 L2 0x01, R2 0x02, L1 0x04, R1 0x08, triangle 0x10, circle
	 * 0x20, cross 0x40, square 0x80. ToD's own bits for the same buttons
	 * (tod.c's remap): triangle 0x1, circle 0x2, cross 0x4, square 0x8, L2
	 * 0x10, R2 0x20, L1 0x40, R1 0x80, select 0x100, start 0x200, L3 0x400,
	 * R3 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000.
	 */
	static const struct { const char *name; u8 d1, d2; u32 og; u32 tod; } b[16] = {
		{ "select",   0x01, 0, PAD_SELECT,   0x0100 },
		{ "L3",       0x02, 0, PAD_L3,       0x0400 },
		{ "R3",       0x04, 0, PAD_R3,       0x0800 },
		{ "start",    0x08, 0, PAD_START,    0x0200 },
		{ "up",       0x10, 0, PAD_UP,       0x1000 },
		{ "right",    0x20, 0, PAD_RIGHT,    0x2000 },
		{ "down",     0x40, 0, PAD_DOWN,     0x4000 },
		{ "left",     0x80, 0, PAD_LEFT,     0x8000 },
		{ "L2",       0, 0x01, PAD_L2,       0x0010 },
		{ "R2",       0, 0x02, PAD_R2,       0x0020 },
		{ "L1",       0, 0x04, PAD_L1,       0x0040 },
		{ "R1",       0, 0x08, PAD_R1,       0x0080 },
		{ "triangle", 0, 0x10, PAD_TRIANGLE, 0x0001 },
		{ "circle",   0, 0x20, PAD_CIRCLE,   0x0002 },
		{ "cross",    0, 0x40, PAD_CROSS,    0x0004 },
		{ "square",   0, 0x80, PAD_SQUARE,   0x0008 },
	};
	char what[128];
	u32 all = 0;
	int i;

	group("VSH pad probe: libpad's buttons in the OG layout");
	for (i = 0; i < 16; i++) {
		u32 got = vshpad_og_mask(b[i].d1, b[i].d2);
		snprintf(what, sizeof(what), "%s: libpad's bit is the OG layout's 0x%04X, which ToD's remap also gives",
		         b[i].name, (unsigned)b[i].og);
		check(got == b[i].og && got == tod_pad_to_og(b[i].tod), what);
		all |= got;
	}
	check(all == 0xFFFFu && vshpad_og_mask(0xFF, 0xFF) == 0xFFFFu, "and all sixteen together fill the mask");
}

/* -------------------------------------------------------- the defer rule */

static void defer_rule(void)
{
	static const u8 moved[4] = { 0xFF, 0x80, 0x00, 0x40 };   /* rx right, lx left, ly up */
	static const u8 wobble[4] = { 0x80 + VSHPAD_DEADZONE, 0x80 - VSHPAD_DEADZONE, 0x80, 0x80 };
	f32 a[4];
	u32 m, reads;
	u64 t0;
	u8 sticks[16];
	int i;

	group("VSH pad probe: the defer rule at the XMB");
	host_log_clear();
	session_vshpad_always(1);
	host_set_vsh_pad(PLAT_PAD_NONE, 0, 0, NULL);
	tick(8);
	m = info_pad(a);
	check(m == 0 && a[0] == 0.0f && a[3] == 0.0f, "no VSH pad and no game: nothing");
	check(host_log_seen("vshpad: no VSH pad to read here"), "and the log says there is no VSH pad, once");

	vsh(PAD_CROSS | PAD_L1, moved);
	m = info_pad(a);
	check(m == (PAD_CROSS | PAD_L1), "at the XMB the VSH's buttons are what SessionInfo carries");
	check(near(a[0], 127.0f / 128.0f) && near(a[1], 0.0f) && near(a[2], -1.0f) && near(a[3], -0.5f),
	      "and its sticks, rx ry lx ly, as (v - 128) / 128");
	check(host_log_seen("vshpad: first read, port 0: 0x00000000, len 24"), "the first read's answer is logged");
	check(host_log_seen("vshpad: vsh 0x0044"), "and a change line with the VSH's mask");

	tick(40);   /* libpad says "same as before" now, for a third of a second */
	check(info_pad(NULL) == (PAD_CROSS | PAD_L1), "a button still held reads as held through libpad's len 0");

	vsh(0, NULL);
	check(info_pad(a) == 0 && a[2] == 0.0f, "released: nothing, with no game to fall back on");

	vsh(0, wobble);
	info_pad(a);
	check(a[0] == 0.0f && a[1] == 0.0f, "a stick no further out than the dead zone counts as centred");

	group("VSH pad probe: the defer rule in a game");
	vsh(PAD_CIRCLE, NULL);
	t0 = plat_time_us();
	check(boot("NPEA00385"), "RaC1 boots with circle still held on the VSH's pad");
	host_set_pad(PAD_SQUARE);
	/* The game's sticks, rx ry lx ly, at its inputs block: 0.25 each, which no VSH stick here reads as. */
	for (i = 0; i < 4; i++) bef32_put(sticks + i * 4, 0.25f);
	host_poke(RAC1_HOT_INPUTS_ADDR, sticks, sizeof(sticks));
	tick(8);
	m = info_pad(a);
	check(plat_time_us() - t0 < VSHPAD_STALE_US && m == PAD_SQUARE && near(a[0], 0.25f),
	      "the XMB's view, not yet stale, does not follow into the game: the game's own pad and sticks");

	vsh(0, NULL);
	host_set_pad(PAD_TRIANGLE);
	tick(8);
	check(info_pad(NULL) == PAD_TRIANGLE, "the VSH's view blank: the game's pad");

	vsh(PAD_START, NULL);
	check(info_pad(NULL) == PAD_START, "the VSH's view with a button held wins over the game's");

	tick(120);   /* one second of len 0 */
	check(info_pad(NULL) == PAD_START, "and holds through a second of libpad's \"same as before\"");
	tick(200);   /* past VSHPAD_STALE_US with nothing new */
	check(info_pad(NULL) == PAD_TRIANGLE, "a view with nothing new for two seconds is stale: the game's pad");

	host_set_pad(PAD_R1);
	vsh(0, moved);
	m = info_pad(a);
	check(m == PAD_R1 && near(a[2], -1.0f), "sticks moved and no button on the VSH: the game's buttons, the VSH's sticks");

	vsh(PAD_L2, NULL);
	m = info_pad(a);
	check(m == PAD_L2 && near(a[0], 0.25f) && near(a[2], 0.25f),
	      "a VSH button with its sticks centred: the VSH's buttons and the game's sticks");

	host_log_clear();
	host_set_vsh_pad((int)ERR_NO_DEVICE, 0, 0, NULL);
	tick(8);
	check(info_pad(NULL) == PAD_R1, "a read that fails falls back to the game's pad");
	check(host_log_seen("vshpad: port 0 read 0x80121107"), "and the error is logged with its code");
	vsh(PAD_UP, NULL);
	check(info_pad(NULL) == PAD_UP && host_log_seen("vshpad: port 0 reads again"),
	      "and once it reads again the VSH's view is back");

	host_set_vsh_pad_port(1, 1);
	vsh(PAD_DOWN, NULL);
	tick(130);   /* GetInfo2 moves to port 1 within a second; then a press there */
	vsh(PAD_DOWN, NULL);
	check(info_pad(NULL) == PAD_DOWN, "pad 1 on port 1: GetInfo2 finds it and the read follows it there");
	check(host_log_seen("port 1 status 0x1"), "and GetInfo2's answer, system_info 1 included, is logged");
	host_set_vsh_pad_port(0, 0);
	tick(130);

	/* The combos keep the game's own pad: a combo held on the VSH's view fires nothing. */
	{
		u32 n0;

		config_set_combo(COMBO_TOGGLE_FAST_LOADS, PAD_SELECT | PAD_TRIANGLE | PAD_LEFT);
		host_set_pad(0);
		vsh(0, NULL);
		tick(4);
		n0 = host_notify_count();
		vsh(PAD_SELECT | PAD_TRIANGLE | PAD_LEFT, NULL);
		check(info_pad(NULL) == (PAD_SELECT | PAD_TRIANGLE | PAD_LEFT) && host_notify_count() == n0,
		      "a combo held on the VSH's pad shows in SessionInfo and fires nothing");
		vsh(0, NULL);
		host_set_pad(PAD_SELECT | PAD_TRIANGLE | PAD_LEFT);
		tick(4);
		host_set_pad(0);
		tick(4);
		check(host_notify_count() == n0 + 1, "while the same combo on the game's pad fires");
		host_set_pad(PAD_SELECT | PAD_TRIANGLE | PAD_LEFT);   /* and back off */
		tick(4);
		host_set_pad(0);
		tick(4);
		config_set_combo(COMBO_TOGGLE_FAST_LOADS, 0);
	}

	group("VSH pad probe: back at the XMB, and with no client");
	host_set_pad(PAD_CIRCLE);
	tick(8);
	check(quit(), "the game quits with circle held on its pad");
	tick(8);
	check(info_pad(NULL) == 0, "at the XMB the last game's pad is not reported");

	session_vshpad_always(0);
	reads = host_vsh_pad_reads();
	host_set_vsh_pad(0, 0, 0x40, CENTRE);
	tick(40);
	check(host_vsh_pad_reads() == reads && info_pad(NULL) == 0,
	      "with no client connected the VSH's pad is never read");

	host_set_vsh_pad(PLAT_PAD_NONE, 0, 0, NULL);
	vshpad_reset();
	tick(8);
}

void test_vshpad(void)
{
	vshpad_reset();
	mapping();
	defer_rule();
}
