/*
 * Revision 1.31: the VSH pad probe (vshpad.h). Tick thread only.
 */
#include "vshpad.h"
#include "proto.h"

#include <string.h>

/* What a change line compares: one is written when any of it differs from the last line's. */
struct vp_key {
	u32 vsh;           /* the VSH's buttons, OG layout, 0 when it has no view */
	u8  game_on;       /* the game's pad_mask is not 0 */
	u8  game_running;
	u8  buttons_on;    /* SessionInfo's buttons are the VSH's */
	u8  sticks_on;     /* and its sticks */
	u8  state;
	u8  err;           /* the last read failed */
};

static struct {
	u8  watching;      /* the last poll had a client to read for */
	u8  none;          /* the last read answered PLAT_PAD_NONE */
	u8  none_said;
	u8  started;       /* the first read's answer is logged */
	u8  game_running;  /* the last poll's, to blank the view when a game comes or goes */
	u8  port;
	u8  have;          /* a fresh read since the view was last blanked */
	u8  digital1, digital2;
	u8  stick[4];
	u8  buttons_on, sticks_on;   /* what vshpad_merge takes, as of the last poll */
	int read_rc;
	/* GetInfo2, once a second */
	u8  info_done, info_said;
	int info_rc;
	u32 now_connect, system_info, port_status;
	u64 info_us;
	u64 fresh_us;      /* when libpad last handed over new data */
	/* the change lines */
	struct vp_key logged;
	u8  logged_any;
	u8  game_seen;     /* the game's pad was not 0 at some poll since the last line */
	u32 vsh_seen;      /* every VSH button seen since the last line */
	u64 log_us;
	u32 lines;
	u32 reads, fresh;  /* since the last line */
} g_vp;

/*
 * libpad's DIGITAL1 byte is select 0x01, L3 0x02, R3 0x04, start 0x08, up
 * 0x10, right 0x20, down 0x40, left 0x80, and DIGITAL2 L2 0x01, R2 0x02, L1
 * 0x04, R1 0x08, triangle 0x10, circle 0x20, cross 0x40, square 0x80
 * (pad_codes.h's CELL_PAD_CTRL_*). The OG layout is DIGITAL2 in its low byte
 * and DIGITAL1 in the next, bit for bit, which is what the four original
 * games keep in memory and what ToD's pad is remapped to.
 */
u32 vshpad_og_mask(u8 digital1, u8 digital2)
{
	return ((u32)digital1 << 8) | (u32)digital2;
}

f32 vshpad_axis(u8 v)
{
	return ((f32)v - 128.0f) / 128.0f;
}

static int stick_moved(u8 v)
{
	int d = (int)v - 0x80;
	if (d < 0) d = -d;
	return d > VSHPAD_DEADZONE;
}

static void view_blank(void)
{
	g_vp.have = 0;
	g_vp.digital1 = g_vp.digital2 = 0;
	g_vp.stick[0] = g_vp.stick[1] = g_vp.stick[2] = g_vp.stick[3] = 0x80;
	g_vp.buttons_on = g_vp.sticks_on = 0;
}

/* Every probe line goes through here, so the cap holds for all of them. */
static int line_ok(void)
{
	if (g_vp.lines < VSHPAD_LOG_MAX) { g_vp.lines++; return 1; }
	if (g_vp.lines == VSHPAD_LOG_MAX) {
		g_vp.lines++;
		plat_log("vshpad: %d lines logged, the probe says no more until the module loads again",
		         (int)VSHPAD_LOG_MAX);
	}
	return 0;
}

static void info_poll(u64 now)
{
	struct plat_pad_info info;
	u32 port = 0, status = 0;
	int rc, i, changed;

	g_vp.info_done = 1;
	g_vp.info_us = now;
	rc = plat_vsh_pad_info(&info);
	if (rc == PLAT_PAD_NONE) return;   /* the read says so */

	if (rc == 0) {
		/* Pad 1 is the lowest port with a controller on it, port 0 when there is none. */
		for (i = 0; i < PLAT_PAD_PORTS; i++) {
			if (info.port_status[i] & 1u) { port = (u32)i; break; }
		}
		status = info.port_status[port];
	} else {
		port = g_vp.port;
		memset(&info, 0, sizeof(info));
	}

	changed = !g_vp.info_said || rc != g_vp.info_rc ||
	          (rc == 0 && (port != g_vp.port || info.system_info != g_vp.system_info ||
	                       info.now_connect != g_vp.now_connect || status != g_vp.port_status));
	if (changed && line_ok()) {
		plat_log("vshpad: GetInfo2 0x%08X: %d of %d connected, system_info 0x%X, port %d status 0x%X type %d",
		         (unsigned)rc, (int)info.now_connect, (int)info.max_connect, (unsigned)info.system_info,
		         (int)port, (unsigned)status, (int)(rc == 0 ? info.device_type[port] : 0));
		g_vp.info_said = 1;
	}
	if (port != g_vp.port) view_blank();   /* another controller */
	g_vp.info_rc = rc;
	g_vp.port = (u8)port;
	g_vp.now_connect = info.now_connect;
	g_vp.system_info = info.system_info;
	g_vp.port_status = status;
}

static const char *source(int vsh, int game_running)
{
	return vsh ? "vsh" : (game_running ? "game" : "none");
}

static void change_line(int game_running, u32 game_mask, u8 state, u8 game_id, u64 now)
{
	struct vp_key k;
	u32 vsh = g_vp.have ? vshpad_og_mask(g_vp.digital1, g_vp.digital2) : 0;

	if (!game_running) game_mask = 0;   /* what g_hot keeps of the last game is nobody's pad */

	memset(&k, 0, sizeof(k));
	k.vsh = vsh;
	k.game_on = game_mask != 0;
	k.game_running = (u8)(game_running != 0);
	k.buttons_on = g_vp.buttons_on;
	k.sticks_on = g_vp.sticks_on;
	k.state = state;
	k.err = g_vp.read_rc != 0;

	g_vp.vsh_seen |= vsh;
	if (k.game_on) g_vp.game_seen = 1;

	/* A press shorter than the gap is still said, by what was seen since the last line. */
	if (g_vp.logged_any && memcmp(&k, &g_vp.logged, sizeof(k)) == 0 &&
	    (g_vp.vsh_seen & ~g_vp.logged.vsh) == 0 && (!g_vp.game_seen || g_vp.logged.game_on))
		return;
	if (g_vp.logged_any && now - g_vp.log_us < VSHPAD_LOG_GAP_US) return;
	if (!line_ok()) return;

	plat_log("vshpad: vsh 0x%04X seen 0x%04X sticks %d,%d,%d,%d | game 0x%04X%s | sent %s, sticks %s"
	         " | state %d game %d port %d sysinfo 0x%X rc 0x%X | reads %d new %d",
	         (unsigned)vsh, (unsigned)g_vp.vsh_seen,
	         (int)g_vp.stick[0], (int)g_vp.stick[1], (int)g_vp.stick[2], (int)g_vp.stick[3],
	         (unsigned)game_mask, game_running ? "" : " (no game)",
	         source(g_vp.buttons_on, game_running), source(g_vp.sticks_on, game_running),
	         (int)state, (int)game_id, (int)g_vp.port, (unsigned)g_vp.system_info,
	         (unsigned)g_vp.read_rc, (int)g_vp.reads, (int)g_vp.fresh);

	memcpy(&g_vp.logged, &k, sizeof(k));   /* padding and all, for the memcmp above */
	g_vp.logged_any = 1;
	g_vp.log_us = now;
	g_vp.vsh_seen = vsh;
	g_vp.game_seen = k.game_on;
	g_vp.reads = g_vp.fresh = 0;
}

void vshpad_poll(int wanted, int game_running, u32 game_mask, u8 state, u8 game_id, u64 now)
{
	struct plat_pad pad;
	int rc, i, live;

	game_running = game_running != 0;

	if (!wanted) {
		if (g_vp.watching && !g_vp.none && line_ok())
			plat_log("vshpad: no client connected, the VSH's pad is not read");
		g_vp.watching = 0;
		view_blank();
		return;
	}
	if (!g_vp.watching) {
		g_vp.watching = 1;
		g_vp.info_done = 0;   /* GetInfo2 at once: the pads may have changed meanwhile */
		view_blank();
		if (!g_vp.none && line_ok())
			plat_log("vshpad: a client is connected, reading the VSH's pad once per telemetry frame");
	}

	if (game_running != g_vp.game_running) {
		g_vp.game_running = (u8)game_running;
		view_blank();
	}

	if (!g_vp.info_done || now - g_vp.info_us >= VSHPAD_INFO_EVERY_US) info_poll(now);

	rc = plat_vsh_pad_read(g_vp.port, &pad);
	if (rc == PLAT_PAD_NONE) {
		if (!g_vp.none_said && line_ok())
			plat_log("vshpad: no VSH pad to read here, the input display shows the game's pad only");
		g_vp.none = g_vp.none_said = 1;
		view_blank();
		return;
	}
	g_vp.none = 0;

	if (!g_vp.started) {
		g_vp.started = 1;
		if (line_ok())
			plat_log("vshpad: first read, port %d: 0x%08X, len %d", (int)g_vp.port, (unsigned)rc, (int)pad.len);
	} else if (rc != g_vp.read_rc && line_ok()) {
		if (rc != 0)
			plat_log("vshpad: port %d read 0x%08X, the game's pad until it reads again", (int)g_vp.port, (unsigned)rc);
		else
			plat_log("vshpad: port %d reads again", (int)g_vp.port);
	}
	g_vp.read_rc = rc;

	if (rc != 0) {
		view_blank();
	} else {
		g_vp.reads++;
		if (pad.fresh) {
			g_vp.have = 1;
			g_vp.fresh++;
			g_vp.fresh_us = now;
			g_vp.digital1 = pad.digital1;
			g_vp.digital2 = pad.digital2;
			for (i = 0; i < 4; i++) g_vp.stick[i] = pad.stick[i];
		}
		live = g_vp.have && now - g_vp.fresh_us < VSHPAD_STALE_US;
		g_vp.buttons_on = (u8)(live && vshpad_og_mask(g_vp.digital1, g_vp.digital2) != 0);
		g_vp.sticks_on = 0;
		for (i = 0; i < 4; i++) {
			if (live && stick_moved(g_vp.stick[i])) g_vp.sticks_on = 1;
		}
	}

	change_line(game_running, game_mask, state, game_id, now);
}

void vshpad_merge(int game_running, u32 game_mask, const f32 game_analog[4],
                  u32 *mask, f32 analog[4])
{
	int i;

	if (g_vp.buttons_on)  *mask = vshpad_og_mask(g_vp.digital1, g_vp.digital2);
	else if (game_running) *mask = game_mask;
	else                  *mask = 0;

	for (i = 0; i < 4; i++) {
		if (g_vp.sticks_on)    analog[i] = vshpad_axis(g_vp.stick[i]);
		else if (game_running) analog[i] = game_analog[i];
		else                   analog[i] = 0.0f;
	}
}

#ifdef QWARK_TEST
void vshpad_reset(void)
{
	memset(&g_vp, 0, sizeof(g_vp));
	view_blank();
}
#endif
