/*
 * Revision 1.31, the VSH pad probe (src/core/vshpad.c): the input display's
 * pad at the XMB, on the fake console, whose VSH libpad answers whatever
 * host_set_vsh_pad says, hands a change over once and then "same as before"
 * (len 0), as libpad does, and whose GetInfo2 answers host_set_vsh_pad_port's
 * system_info.
 *
 * libpad's digital bytes to the OG layout, bit by bit and against ToD's own
 * remap; the read cadence (every tick, so a change is qwark's before another
 * reader in the VSH can take it); the defer rule as SessionInfo carries it: at
 * the XMB the VSH's view, held for as long as libpad says nothing changed; in
 * a game the game's pad, but for the overlay, told first by who took input and
 * then by system_info's bit once it has shown it follows the overlay, in
 * either sense; the overlay closing with the game's pad showing at once and
 * nothing of the VSH's left lit; buttons and sticks decided apart, the dead
 * zone, a view left over from the XMB as a game starts, the last game's pad at
 * the XMB, an error and no pad API at all falling back to the game's pad, no
 * read without a client, and the combos still on the game's own pad. The wire
 * itself is smoke's.
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
#define TICK_US       8334
#define TICKS(us)     ((int)((us) / TICK_US) + 1)

static const u8 CENTRE[4] = { 0x80, 0x80, 0x80, 0x80 };

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(TICK_US);
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

/* RaC1's own sticks, rx ry lx ly, at its inputs block. */
static void game_sticks(f32 v)
{
	u8 sticks[16];
	int i;

	for (i = 0; i < 4; i++) bef32_put(sticks + i * 4, v);
	host_poke(RAC1_HOT_INPUTS_ADDR, sticks, sizeof(sticks));
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

/* ------------------------------------------------------------ the XMB */

static void at_xmb(void)
{
	static const u8 moved[4] = { 0xFF, 0x80, 0x00, 0x40 };   /* rx right, lx left, ly up */
	static const u8 wobble[4] = { 0x80 + VSHPAD_DEADZONE, 0x80 - VSHPAD_DEADZONE, 0x80, 0x80 };
	f32 a[4];
	u32 m, reads;
	int phase, missed;

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

	tick(TICKS(3000000));   /* three seconds of libpad's "same as before" */
	m = info_pad(a);
	check(m == (PAD_CROSS | PAD_L1) && near(a[2], -1.0f),
	      "a button and a stick held still stay lit through three seconds of len 0: at the XMB nothing goes stale");

	vsh(0, NULL);
	check(info_pad(a) == 0 && a[2] == 0.0f, "released: nothing, with no game to fall back on");

	vsh(0, wobble);
	info_pad(a);
	check(a[0] == 0.0f && a[1] == 0.0f, "a stick no further out than the dead zone counts as centred");

	group("VSH pad probe: read every tick, as the game's pad is");
	reads = host_vsh_pad_reads();
	tick(40);
	check(host_vsh_pad_reads() - reads == 40, "40 ticks, 40 reads of the VSH's pad: 120 a second, not 30");

	/*
	 * Another reader in the VSH (webMAN's combo poll) takes whatever change
	 * is still waiting when it asks, one tick after the press here, whatever
	 * the press's place in the telemetry frame: reading every tick, qwark has
	 * always had it by then, and the packet carries it.
	 */
	missed = 0;
	for (phase = 0; phase < 4; phase++) {
		static const u32 press[4] = { PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT };
		tick(phase + 1);
		host_set_vsh_pad(0, (u8)(press[phase] >> 8), (u8)press[phase], CENTRE);
		tick(1);
		if (host_vsh_pad_take()) missed++;
		tick(4);
		if (info_pad(NULL) != press[phase]) missed++;
	}
	check(missed == 0, "a press at each tick of a telemetry frame is read before another reader can take it");

	host_set_vsh_pad(0, 0, 0, CENTRE);
	tick(1);
	host_set_vsh_pad(0, (u8)(PAD_START >> 8), (u8)PAD_START, CENTRE);
	check(host_vsh_pad_take() == 1, "(the fake's other reader does take a change nobody has read yet)");
	tick(8);
	check(info_pad(NULL) == 0, "(and then qwark never sees it, as on the console)");

	group("VSH pad probe: errors and ports at the XMB");
	vsh(PAD_R2, NULL);
	host_log_clear();
	host_set_vsh_pad((int)ERR_NO_DEVICE, 0, 0, NULL);
	tick(8);
	check(info_pad(NULL) == 0, "a read that fails drops the VSH's view: R2 is not left lit");
	check(host_log_seen("vshpad: port 0 read 0x80121107"), "and the error is logged with its code");
	vsh(PAD_UP, NULL);
	check(info_pad(NULL) == PAD_UP && host_log_seen("vshpad: port 0 reads again"),
	      "and once it reads again the VSH's view is back");

	host_set_vsh_pad_port(1, 0);
	vsh(PAD_DOWN, NULL);
	tick(8);   /* GetInfo2 moves to port 1 within a telemetry frame; then a press there */
	vsh(PAD_DOWN, NULL);
	check(info_pad(NULL) == PAD_DOWN, "pad 1 on port 1: GetInfo2 finds it and the read follows it there");
	check(host_log_seen("port 1 status 0x1"), "and GetInfo2's answer is logged");
	host_set_vsh_pad_port(0, 0);
	tick(8);
	vsh(0, NULL);
}

/* ------------------------------------------------------------ in a game */

/*
 * The overlay as the fake console shows it: the game's pad empty while it is
 * open (the game gets no input), the VSH's libpad handing over what is pressed
 * in it, and once it closes no more from the VSH (its last report stays its
 * view, len 0) while the game's pad shows what is pressed.
 */
static void in_game_by_input(void)
{
	static const u8 moved[4] = { 0xFF, 0x80, 0x00, 0x40 };
	f32 a[4];
	u32 m;

	group("VSH pad probe: in a game, by who took input (system_info steady)");
	vsh(PAD_CIRCLE, NULL);
	host_log_clear();
	check(boot("NPEA00385"), "RaC1 boots with circle still held on the VSH's pad");
	host_set_pad(PAD_SQUARE);
	game_sticks(0.25f);   /* inside the dead zone: the game's sticks, which no VSH stick here reads as */
	tick(8);
	m = info_pad(a);
	check(m == PAD_SQUARE && near(a[0], 0.25f), "the XMB's view does not follow into the game: the game's own pad");
	check(host_log_seen("vshpad: source game: a game is running"), "and the log says the game has the pad");

	host_set_pad(PAD_TRIANGLE);
	tick(4);
	vsh(PAD_START, moved);
	m = info_pad(a);
	check(m == PAD_TRIANGLE && near(a[2], 0.25f),
	      "the game has the pad: a change on the VSH's view does not show over the game's buttons or sticks");

	/* The overlay opens. */
	host_log_clear();
	host_set_pad(0);
	tick(4);
	vsh(PAD_CROSS, moved);
	m = info_pad(a);
	check(m == PAD_CROSS && near(a[2], -1.0f), "the overlay: the game's pad empty, the VSH's press and sticks show");
	check(host_log_seen("vshpad: source vsh: the VSH's pad took input with the game's empty"),
	      "and the log says the VSH took the pad, and why");
	tick(TICKS(500000));
	check(info_pad(NULL) == PAD_CROSS, "a press held still in the overlay stays lit through half a second of len 0");
	check(host_log_seen("vshpad: system_info bit0 is 0 while the game takes input"),
	      "meanwhile system_info's bit was learnt from the game's input before the overlay: 0");

	/* It closes with cross on the VSH's last report; the game takes input. */
	host_log_clear();
	host_set_pad(PAD_SQUARE);
	tick(4);
	m = info_pad(a);
	check(m == PAD_SQUARE && near(a[2], 0.25f),
	      "the overlay closes: the game's first input shows in the next packet, cross and the VSH's sticks gone");
	check(host_log_seen("vshpad: source game: the game's pad took input"), "and the log says the game took it back");
	host_set_pad(0);
	tick(4);
	check(info_pad(NULL) == 0, "and the VSH's last press does not come back when the game's pad empties");

	/* It closes with the game left alone: the VSH's last press is not left lit. */
	vsh(PAD_CIRCLE, NULL);
	check(info_pad(NULL) == PAD_CIRCLE, "the overlay again: circle");
	host_log_clear();
	tick(TICKS(VSHPAD_QUIET_US) - 16);
	check(info_pad(NULL) == PAD_CIRCLE, "held still short of a second, still lit");
	tick(16);
	check(info_pad(NULL) == 0, "closed with the game's pad left empty: after a second with nothing new, circle goes");
	check(host_log_seen("vshpad: source game: nothing new from the VSH for a second"), "and the log says why");
}

static void in_game_by_sysinfo(void)
{
	static const u8 moved[4] = { 0xFF, 0x80, 0x00, 0x40 };
	f32 a[4];
	u32 m;

	group("VSH pad probe: in a game, by system_info once it follows the overlay");
	host_log_clear();
	host_set_vsh_pad_port(0, 1);   /* the overlay opens: system_info's bit0 goes to 1 */
	tick(8);
	vsh(PAD_TRIANGLE, NULL);
	check(info_pad(NULL) == PAD_TRIANGLE, "the overlay opens and system_info's bit goes: the VSH's press shows");
	check(host_log_seen("vshpad: system_info bit0 went to 1 in a game: it says who has the pad from now on") &&
	      host_log_seen("vshpad: source vsh: system_info says the VSH has it"),
	      "and the log says the bit decides from now on, and that the VSH has the pad");
	tick(TICKS(2 * VSHPAD_QUIET_US));
	check(info_pad(NULL) == PAD_TRIANGLE, "a press held still in the overlay for two seconds stays lit: no quiet rule now");

	host_log_clear();
	host_set_vsh_pad_port(0, 0);   /* it closes, the game's pad still empty */
	tick(8);
	check(info_pad(NULL) == 0, "the overlay closes: the VSH's last press goes at once, though the game's pad is empty");
	check(host_log_seen("vshpad: source game: system_info says the game has it"), "and the log says so");

	/* Open again, and close with the game's pad held across it: no blank between the two. */
	host_set_vsh_pad_port(0, 1);
	tick(8);
	vsh(PAD_CIRCLE, moved);
	tick(TICKS(VSHPAD_BIT_STEADY_US));   /* the overlay up long enough for the bit to count as still */
	host_log_clear();
	host_set_vsh_pad_port(0, 0);
	host_set_pad(PAD_R1);
	tick(4);
	m = info_pad(a);
	check(m == PAD_R1 && near(a[2], 0.25f), "closing with R1 held in the game: R1 and the game's sticks in the next packet");
	tick(TICKS(2 * VSHPAD_BIT_STEADY_US));
	check(info_pad(NULL) == PAD_R1 && !host_log_seen("while the game takes input"),
	      "and the game's input as the bit went taught nothing wrong: still R1, nothing relearnt");
	host_set_pad(0);
	tick(8);

	/* Buttons and sticks apart, in the overlay. */
	host_set_vsh_pad_port(0, 1);
	tick(8);
	vsh(PAD_L2, NULL);
	m = info_pad(a);
	check(m == PAD_L2 && near(a[0], 0.25f) && near(a[2], 0.25f),
	      "a VSH button with its sticks centred: the VSH's buttons and the game's sticks");
	vsh(0, moved);
	m = info_pad(a);
	check(m == 0 && near(a[2], -1.0f), "the VSH's sticks out and no button: its sticks");

	/* An error in the overlay. */
	vsh(PAD_TRIANGLE, NULL);
	host_log_clear();
	host_set_vsh_pad((int)ERR_NO_DEVICE, 0, 0, NULL);
	tick(8);
	m = info_pad(a);
	check(m == 0 && near(a[2], 0.25f), "a read that fails falls back to the game's pad: triangle not left lit");
	check(host_log_seen("vshpad: port 0 read 0x80121107"), "and the error is logged");
	vsh(PAD_UP, NULL);
	check(info_pad(NULL) == PAD_UP, "and once it reads again the VSH's view is back");

	/* The combos keep the game's own pad: a combo held on the VSH's view fires nothing. */
	{
		u32 n0;

		config_set_combo(COMBO_TOGGLE_FAST_LOADS, PAD_SELECT | PAD_TRIANGLE | PAD_LEFT);
		vsh(0, NULL);
		n0 = host_notify_count();
		vsh(PAD_SELECT | PAD_TRIANGLE | PAD_LEFT, NULL);
		check(info_pad(NULL) == (PAD_SELECT | PAD_TRIANGLE | PAD_LEFT) && host_notify_count() == n0,
		      "a combo held on the VSH's pad shows in SessionInfo and fires nothing");
		vsh(0, NULL);
		host_set_vsh_pad_port(0, 0);
		host_set_pad(PAD_SELECT | PAD_TRIANGLE | PAD_LEFT);
		tick(8);
		host_set_pad(0);
		tick(4);
		check(host_notify_count() == n0 + 1, "while the same combo on the game's pad fires");
		host_set_pad(PAD_SELECT | PAD_TRIANGLE | PAD_LEFT);   /* and back off */
		tick(4);
		host_set_pad(0);
		tick(4);
		config_set_combo(COMBO_TOGGLE_FAST_LOADS, 0);
	}

	/*
	 * The other sense: a VSH whose libpad sets the bit while the game has the
	 * pad (it is the VSH that is intercepted) and clears it in the overlay.
	 * A module that has just loaded learns it from the game's input.
	 */
	group("VSH pad probe: system_info in the other sense");
	host_set_vsh_pad_port(0, 1);
	vshpad_reset();
	host_log_clear();
	tick(TICKS(VSHPAD_BIT_STEADY_US) + 4);
	host_set_pad(PAD_CROSS);
	tick(TICKS(VSHPAD_BIT_STEADY_US) - 4);
	check(!host_log_seen("while the game takes input"), "the bit is not learnt from the game's input at once");
	tick(8);
	check(host_log_seen("vshpad: system_info bit0 is 1 while the game takes input"),
	      "but once it has held still as long after: 1");
	host_set_pad(0);
	tick(4);
	host_set_vsh_pad_port(0, 0);   /* the overlay opens */
	tick(8);
	vsh(PAD_SQUARE, NULL);
	check(info_pad(NULL) == PAD_SQUARE, "the overlay opens, the bit going to 0: the VSH's press shows");
	host_set_vsh_pad_port(0, 1);   /* and closes */
	host_set_pad(PAD_L1);
	tick(8);
	check(info_pad(NULL) == PAD_L1, "it closes, the bit back at 1: the game's L1 at once, square gone");
	host_set_pad(0);
	tick(4);
	host_set_vsh_pad_port(0, 0);
	tick(8);
	check(info_pad(NULL) == 0, "(the bit at 0 again with nothing pressed: nothing stuck)");
	host_set_vsh_pad_port(0, 1);
	tick(8);
}

static void back_at_xmb(void)
{
	u32 reads;

	group("VSH pad probe: back at the XMB, and with no client");
	host_set_pad(PAD_CIRCLE);
	tick(8);
	host_log_clear();
	check(quit(), "the game quits with circle held on its pad");
	tick(8);
	check(info_pad(NULL) == 0, "at the XMB the last game's pad is not reported");
	check(host_log_seen("vshpad: source vsh: no game: the XMB has the pad"), "and the log says the VSH has the pad");
	host_set_vsh_pad_port(0, 0);
	vsh(PAD_L2, NULL);
	check(info_pad(NULL) == PAD_L2, "and the VSH's view is what goes out");

	session_vshpad_always(0);
	tick(4);
	check(info_pad(NULL) == 0, "no client: the VSH's view is dropped");
	reads = host_vsh_pad_reads();
	host_set_vsh_pad(0, 0, 0x40, CENTRE);
	tick(40);
	check(host_vsh_pad_reads() == reads && info_pad(NULL) == 0,
	      "with no client connected the VSH's pad is never read");

	host_set_vsh_pad(PLAT_PAD_NONE, 0, 0, NULL);
	host_set_pad(0);
	game_sticks(0.0f);
	vshpad_reset();
	tick(8);
}

void test_vshpad(void)
{
	vshpad_reset();
	mapping();
	at_xmb();
	in_game_by_input();
	in_game_by_sysinfo();
	back_at_xmb();
}
