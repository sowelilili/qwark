/*
 * The Tools of Destruction groundwork (docs/tod-poc.md): the Lua
 * mailbox's codec and request state machine (src/core/luamb.c), the helper's
 * RPCS3 patch, presence rule and console install (src/games/luahelper_patch.c),
 * and the fact sheet (src/games/tod.c).
 *
 * A program of its own, as the installer's tests are: nothing here is linked
 * into qwark yet, so it does not go into qwark-test.exe. test/run.sh builds
 * and runs it.
 *
 * The game side is a fake memory and a C model of the helper's frame
 * (helper_frame below), written from src/games/luahelper/luahelper.c line for
 * line, with a stand-in for Lua that knows a handful of chunks.
 */
#include <stdio.h>
#include <string.h>

#include "../src/core/luamb_client.h"
#include "../src/core/proto.h"
#include "../src/core/util.h"
#include "../src/games/luahelper_patch.h"
#include "../src/games/tod.h"

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
	if (ok) g_pass++;
	else { g_fail++; printf("FAIL: %s\n", what); }
}

/* util.c's file helpers, which nothing here calls. */
int plat_file_open(const char *path, int mode, plat_file_t *out) { (void)path; (void)mode; (void)out; return -1; }
int plat_file_read(plat_file_t f, void *buf, u32 len, u32 *nread) { (void)f; (void)buf; (void)len; *nread = 0; return -1; }
int plat_file_close(plat_file_t f) { (void)f; return 0; }

/* ------------------------------------------------------------ fake memory */

/*
 * Three windows of the game: the code around both hook sites and the cave,
 * the RW page with the mailbox, and the .bss word holding the lua_State*.
 */
#define CODE_LO 0x00020000u
#define CODE_HI 0x00870000u
#define RW_LO   0x008CA000u
#define RW_HI   0x008D0000u
#define VM_LO   0x10204000u
#define VM_HI   0x10205000u
/* Revision 1.29: the pause menu's system pointer, and a fake menu system past it. */
#define MS_LO   0x101CB000u
#define MS_HI   0x101CE000u
#define FAKE_MS 0x101CB400u

static u8 g_code[CODE_HI - CODE_LO];
static u8 g_rw[RW_HI - RW_LO];
static u8 g_vm[VM_HI - VM_LO];
static u8 g_ms[MS_HI - MS_LO];
static int g_fail_reads, g_fail_writes;

static u8 *at(u32 addr, u32 len)
{
	if (addr >= CODE_LO && addr + len <= CODE_HI) return g_code + (addr - CODE_LO);
	if (addr >= RW_LO && addr + len <= RW_HI) return g_rw + (addr - RW_LO);
	if (addr >= VM_LO && addr + len <= VM_HI) return g_vm + (addr - VM_LO);
	if (addr >= MS_LO && addr + len <= MS_HI) return g_ms + (addr - MS_LO);
	return NULL;
}

static int fake_read(void *ctx, u32 addr, void *buf, u32 len)
{
	u8 *p = at(addr, len);

	(void)ctx;
	if (g_fail_reads || p == NULL) return -1;
	memcpy(buf, p, len);
	return 0;
}

static int fake_write(void *ctx, u32 addr, const void *buf, u32 len)
{
	u8 *p = at(addr, len);

	(void)ctx;
	if (g_fail_writes || p == NULL) return -1;
	memcpy(p, buf, len);
	return 0;
}

static const struct luamb_io g_io = { fake_read, fake_write, NULL };

static u32 rd32(u32 a) { return be32_get(at(a, 4)); }
static void wr32(u32 a, u32 v) { be32_put(at(a, 4), v); }

static const struct lh_desc *g_d;
static u32 MB;

/* Revision 1.29: a stand-in for ui.pak's entry, the cave's bytes out of the table, failing at read g_src_fail_at. */
static u32 g_src_reads;
static int g_src_fail_at = -1;

static int src_read(void *ctx, u8 cave, u32 off, void *buf, u32 len)
{
	const struct lh_desc *d = (const struct lh_desc *)ctx;

	if ((int)g_src_reads++ == g_src_fail_at) return -1;
	if (cave >= d->ncaves || off > d->caves[cave].len || len > d->caves[cave].len - off) return -1;
	memcpy(buf, d->caves[cave].bytes + off, len);
	return 0;
}

static void reset_game(u32 L)
{
	memset(g_code, 0, sizeof(g_code));
	memset(g_rw, 0, sizeof(g_rw));
	memset(g_vm, 0, sizeof(g_vm));
	memset(g_ms, 0, sizeof(g_ms));
	wr32(0x004C461Cu, 0x4BDCC909u);
	wr32(0x0068F8F8u, 0x4E800020u);
	wr32(0x000242B0u, 0x4BFED721u);       /* revision 1.29: the pause menu's update */
	wr32(0x004C5ABCu, 0x4BDA1825u);       /* and the pause test */
	wr32(0x0003633Cu, 0x4BFDB695u);       /* revision 1.29, every planet: the galactic map's update */
	wr32(g_d->vm + g_d->vm_state, L);
}

static void put_ident(void)
{
	u8 b[8];

	luamb_ident_bytes(b);
	memcpy(at(MB, 8), b, 8);
}

/* --------------------------------------------- the helper's frame, modelled */

static int g_ran;            /* chunks that reached "Lua" */
static char g_last_chunk[LUAMB_CHUNK_CAP + 1];
static int g_claim_then_stop; /* stop after raising busy: the helper is inside a request */

static void put_out(const char *s, u32 *len, u8 *flags)
{
	u32 n = 0;

	while (s[n] != 0 && n < LUAMB_OUT_CAP - 1) {
		*at(MB + LUAMB_OFF_OUT + n, 1) = (u8)s[n];
		n++;
	}
	if (s[n] != 0) *flags |= LUAMB_OUT_TRUNCATED;
	*at(MB + LUAMB_OFF_OUT + n, 1) = 0;
	*len = n;
}

/* A Lua that knows five chunks. */
static int fake_lua(const char *src, u32 *len, u8 *type, u8 *flags)
{
	static char big[3000];

	g_ran++;
	if (strcmp(src, "time_set_scale(0.5) return _VERSION") == 0 || strcmp(src, "return _VERSION") == 0) {
		*type = LUAMB_T_STRING;
		put_out("Lua 5.0.2", len, flags);
		return LUAMB_ST_OK;
	}
	if (strcmp(src, "return 1 + 1") == 0) {
		*type = LUAMB_T_NUMBER;
		put_out("2", len, flags);
		return LUAMB_ST_OK;
	}
	if (strcmp(src, "return true") == 0) {
		*type = LUAMB_T_BOOLEAN;
		*flags |= LUAMB_OUT_TRUE;
		return LUAMB_ST_OK;
	}
	if (strcmp(src, "return big") == 0) {
		memset(big, 'x', sizeof(big) - 1);
		*type = LUAMB_T_STRING;
		put_out(big, len, flags);
		return LUAMB_ST_OK;
	}
	if (strncmp(src, "error(", 6) == 0) {
		*type = LUAMB_T_STRING;
		put_out("qwark:1: boom", len, flags);
		return LUAMB_ST_ERRRUN;
	}
	*type = LUAMB_T_STRING;
	put_out("qwark:1: syntax", len, flags);
	return LUAMB_ST_ERRSYNTAX;
}

/* luahelper.c's lh_tick, on the fake memory. */
static void helper_frame(void)
{
	u32 r, L, len, out_len = 0;
	u8 out_type = LUAMB_T_NONE, out_flags = 0;
	int st;

	if (rd32(MB + LUAMB_OFF_MAGIC) != LUAMB_MAGIC) return;
	wr32(MB + LUAMB_OFF_HEARTBEAT, rd32(MB + LUAMB_OFF_HEARTBEAT) + 1);
	L = rd32(g_d->vm + g_d->vm_state);
	wr32(MB + LUAMB_OFF_LAST_STATE, L);
	r = rd32(MB + LUAMB_OFF_REQ);
	if (r == rd32(MB + LUAMB_OFF_ACK)) return;
	/* Version 2: a request for the pause menu's VM is not the gameplay frame's. */
	if (*at(MB + LUAMB_OFF_TARGET, 1) != LUAMB_TARGET_GAMEPLAY) return;
	wr32(MB + LUAMB_OFF_BUSY, r);
	if (g_claim_then_stop) return;

	*at(MB + LUAMB_OFF_OUT, 1) = 0;
	len = rd32(MB + LUAMB_OFF_CHUNK_LEN);
	if (rd32(MB + LUAMB_OFF_CANCEL) == r) st = LUAMB_ST_CANCELLED;
	else if (L == 0) st = LUAMB_ST_NO_STATE;
	else if (len == 0 || len > LUAMB_CHUNK_CAP) st = LUAMB_ST_BAD_LEN;
	else {
		memcpy(g_last_chunk, at(MB + LUAMB_OFF_CHUNK, len), len);
		g_last_chunk[len] = 0;
		st = fake_lua(g_last_chunk, &out_len, &out_type, &out_flags);
	}
	wr32(MB + LUAMB_OFF_OUT_LEN, out_len);
	*at(MB + LUAMB_OFF_OUT_TYPE, 1) = out_type;
	*at(MB + LUAMB_OFF_OUT_FLAGS, 1) = out_flags;
	wr32(MB + LUAMB_OFF_STATUS, (u32)st);
	wr32(MB + LUAMB_OFF_ACK, r);
	wr32(MB + LUAMB_OFF_BUSY, 0);
}

/* The client and a poll loop: one helper frame between polls when `frames`. */
static struct luamb_client g_c;

static int run(const char *src, int frames, u32 *now, struct luamb_result *res)
{
	int rc = luamb_send(&g_c, &g_io, src, (u32)strlen(src), *now);
	int i;

	if (rc != LUAMB_SEND_OK) return -100 - rc;
	for (i = 0; i < 400; i++) {
		if (frames) helper_frame();
		*now += 16;
		rc = luamb_poll(&g_c, &g_io, *now, res);
		if (rc == LUAMB_POLL_DONE) return 0;
		if (rc != LUAMB_POLL_PENDING) return -200 - rc;
	}
	return -1;
}

/* ----------------------------------------------------------------- tests */

static void test_layout(void)
{
	check(LUAMB_OFF_CHUNK + LUAMB_CHUNK_CAP == LUAMB_SIZE, "layout: the chunk ends the block");
	check(LUAMB_OFF_OUT + LUAMB_OUT_CAP == LUAMB_OFF_CHUNK, "layout: out runs into chunk");
	check(LUAMB_OFF_ERR + LUAMB_ERR_CAP == LUAMB_OFF_OUT, "layout: err runs into out");
	check(LUAMB_OFF_ERR_SCRIPT + LUAMB_ERR_SCRIPT_CAP <= LUAMB_OFF_ERR, "layout: the script name fits");
	check(LUAMB_OFF_ERR_LEN + 4 <= LUAMB_HDR_SIZE, "layout: every polled word is in the header read");
	check(LUAMB_SIZE <= 0xFFFF, "layout: the size fits its u16");
	check((LUAMB_OFF_REQ & 3) == 0 && (LUAMB_OFF_ACK & 3) == 0 && (LUAMB_OFF_CANCEL & 3) == 0 &&
	      (LUAMB_OFF_BUSY & 3) == 0, "layout: the protocol words are aligned, so each write is atomic");
}

static void test_codec(void)
{
	struct luamb_hdr h, g;
	u8 raw[LUAMB_HDR_SIZE];
	u8 out[LUAMB_OUT_CAP];
	char txt[16];
	u8 id[8];

	memset(&h, 0, sizeof(h));
	memset(&g, 0, sizeof(g));
	h.magic = LUAMB_MAGIC; h.version = LUAMB_VERSION; h.size = LUAMB_SIZE;
	h.req = 7; h.ack = 6; h.cancel = 3; h.busy = 7; h.status = LUAMB_ST_CANCELLED;
	h.chunk_len = 100; h.out_len = 5; h.out_type = LUAMB_T_TABLE; h.out_flags = 3;
	h.heartbeat = 0xA1B2C3D4u; h.last_state = 0x30001000u; h.err_seq = 9; h.err_vm = 0x102043CCu; h.err_len = 12;
	h.target = LUAMB_TARGET_MENU; h.menu_beat = 0x55AA0001u;
	luamb_hdr_encode(&h, raw);
	luamb_hdr_decode(raw, &g);
	check(memcmp(&h, &g, sizeof(h)) == 0, "codec: a header survives encode and decode");
	check(raw[0] == 'Q' && raw[1] == 'L' && raw[2] == 'U' && raw[3] == 'A', "codec: the magic reads QLUA");
	check(raw[LUAMB_OFF_REQ + 3] == 7 && raw[LUAMB_OFF_REQ] == 0, "codec: big-endian");
	check(luamb_hdr_valid(&g), "codec: a good header is valid");
	g.version = LUAMB_VERSION - 1;
	check(!luamb_hdr_valid(&g), "codec: another version is not (version 1, before the menu target)");

	luamb_ident_bytes(id);
	check(be32_get(id) == LUAMB_MAGIC && be16_get(id + 4) == LUAMB_VERSION && be16_get(id + 6) == LUAMB_SIZE,
	      "codec: the identity words");

	memset(out, 'a', sizeof(out));
	check(luamb_out_text(out, 9999, txt, sizeof(txt)) == sizeof(txt) - 1 && txt[sizeof(txt) - 1] == 0,
	      "codec: an out_len past the cap and no NUL still give a bounded string");
	memcpy(out, "ab\0cd", 5);
	check(luamb_out_text(out, 5, txt, sizeof(txt)) == 2 && strcmp(txt, "ab") == 0, "codec: the text stops at a NUL");
}

static void test_requests(void)
{
	struct luamb_result res;
	char txt[LUAMB_OUT_CAP];
	u32 now = 1000;
	int rc;

	/* No helper: no magic. */
	reset_game(0x30001000u);
	luamb_client_init(&g_c, MB, g_d->vm + g_d->vm_state);
	check(luamb_attach(&g_c, &g_io, now) == LUAMB_POLL_IDLE, "attach: nothing to take up");
	check(luamb_send(&g_c, &g_io, "return 1", 8, now) == LUAMB_SEND_NO_HELPER, "send: no helper without the magic");

	/* The round trip. */
	put_ident();
	g_ran = 0;
	rc = run("time_set_scale(0.5) return _VERSION", 1, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_OK && res.out_type == LUAMB_T_STRING, "round trip: the PoC chunk answers OK");
	check(luamb_read_out(&g_c, &g_io, &res, txt, sizeof(txt)) == 9 && strcmp(txt, "Lua 5.0.2") == 0,
	      "round trip: the text is Lua 5.0.2");
	check(rd32(MB + LUAMB_OFF_REQ) == 1 && rd32(MB + LUAMB_OFF_ACK) == 1 && g_ran == 1, "round trip: seq 1, run once");
	check(rd32(MB + LUAMB_OFF_BUSY) == 0, "round trip: busy back to 0");

	rc = run("return 1 + 1", 1, &now, &res);
	check(rc == 0 && res.seq == 2 && res.out_type == LUAMB_T_NUMBER, "round trip: a second request is seq 2");
	rc = run("return true", 1, &now, &res);
	check(rc == 0 && res.out_type == LUAMB_T_BOOLEAN && (res.out_flags & LUAMB_OUT_TRUE), "round trip: a boolean");
	rc = run("error('boom')", 1, &now, &res);
	luamb_read_out(&g_c, &g_io, &res, txt, sizeof(txt));
	check(rc == 0 && res.status == LUAMB_ST_ERRRUN && strcmp(txt, "qwark:1: boom") == 0, "round trip: an error's message");
	rc = run("this is not lua", 1, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_ERRSYNTAX, "round trip: a syntax error");
	rc = run("return big", 1, &now, &res);
	luamb_read_out(&g_c, &g_io, &res, txt, sizeof(txt));
	check(rc == 0 && (res.out_flags & LUAMB_OUT_TRUNCATED) && strlen(txt) == LUAMB_OUT_CAP - 1,
	      "round trip: a long result is cut at the cap and flagged");

	/* Refusals before anything is written. */
	check(luamb_send(&g_c, &g_io, "", 0, now) == LUAMB_SEND_TOO_LONG, "send: an empty chunk");
	{
		static char huge[LUAMB_CHUNK_CAP + 2];

		memset(huge, ' ', sizeof(huge) - 1);
		check(luamb_send(&g_c, &g_io, huge, LUAMB_CHUNK_CAP + 1, now) == LUAMB_SEND_TOO_LONG, "send: one byte over the cap");
		check(luamb_send(&g_c, &g_io, huge, LUAMB_CHUNK_CAP, now) == LUAMB_SEND_OK, "send: exactly the cap");
		helper_frame();
		now += 16;
		check(luamb_poll(&g_c, &g_io, now, &res) == LUAMB_POLL_DONE, "send: the cap's answer comes back");
	}
	wr32(g_d->vm + g_d->vm_state, 0);
	check(luamb_send(&g_c, &g_io, "return 1", 8, now) == LUAMB_SEND_NO_STATE, "send: no lua_State between levels");
	wr32(g_d->vm + g_d->vm_state, 0x30002000u);

	/* One at a time. */
	check(luamb_send(&g_c, &g_io, "return 1 + 1", 12, now) == LUAMB_SEND_OK, "busy: the first goes out");
	check(luamb_send(&g_c, &g_io, "return 1 + 1", 12, now) == LUAMB_SEND_BUSY, "busy: the second waits");
	helper_frame();
	check(luamb_poll(&g_c, &g_io, now + 16, &res) == LUAMB_POLL_DONE, "busy: and then it is free");
	check(luamb_poll(&g_c, &g_io, now + 32, &res) == LUAMB_POLL_IDLE, "busy: nothing more to poll");

	/* A read that fails is retried, not taken for an answer. */
	check(luamb_send(&g_c, &g_io, "return 1 + 1", 12, now) == LUAMB_SEND_OK, "io: sent");
	g_fail_reads = 1;
	check(luamb_poll(&g_c, &g_io, now + 16, &res) == LUAMB_POLL_IO, "io: a failed read says so");
	g_fail_reads = 0;
	helper_frame();
	check(luamb_poll(&g_c, &g_io, now + 32, &res) == LUAMB_POLL_DONE, "io: the answer still comes");
	g_fail_writes = 1;
	check(luamb_send(&g_c, &g_io, "return 1", 8, now) == LUAMB_SEND_IO, "io: a failed write is refused");
	g_fail_writes = 0;
	/* A failed write may have left the chunk half written but never raised req. */
	check(rd32(MB + LUAMB_OFF_REQ) == rd32(MB + LUAMB_OFF_ACK), "io: no request raised by a failed send");
}

/*
 * Version 2 (revision 1.29): a request for the pause menu's VM, which only
 * the pause menu's frame takes; the gameplay frame leaves it, and its
 * timeout reads the menu's heartbeat.
 */
static void menu_frame(void)
{
	u32 r = rd32(MB + LUAMB_OFF_REQ);

	wr32(MB + LUAMB_OFF_MENU_BEAT, rd32(MB + LUAMB_OFF_MENU_BEAT) + 1);
	if (r == rd32(MB + LUAMB_OFF_ACK) || *at(MB + LUAMB_OFF_TARGET, 1) != LUAMB_TARGET_MENU) return;
	wr32(MB + LUAMB_OFF_STATUS, LUAMB_ST_OK);
	wr32(MB + LUAMB_OFF_OUT_LEN, 0);
	wr32(MB + LUAMB_OFF_ACK, r);
}

static void test_target(void)
{
	struct luamb_result res;
	u32 now = 90000, st = 0;
	int rc, i;

	reset_game(0x30001000u);
	put_ident();
	luamb_client_init(&g_c, MB, g_d->vm + g_d->vm_state);
	check(luamb_target_state(&g_c, &g_io, LUAMB_TARGET_MENU, &st) == 0 && st == 0,
	      "target: a client with no menu VM has no menu lua_State");
	luamb_client_menu(&g_c, g_d->menu_ptr, g_d->menu_off);
	luamb_attach(&g_c, &g_io, now);
	check(g_d->menu_ptr >= MS_LO && g_d->menu_ptr < MS_HI, "target: the pause menu's pointer is in the fake memory");
	check(luamb_send_to(&g_c, &g_io, "return 1", 8, now, LUAMB_TARGET_MENU) == LUAMB_SEND_NO_STATE,
	      "target: no pause up, no menu lua_State: NO_STATE");
	wr32(g_d->menu_ptr, FAKE_MS);
	check(luamb_send_to(&g_c, &g_io, "return 1", 8, now, LUAMB_TARGET_MENU) == LUAMB_SEND_NO_STATE,
	      "target: a menu system with no VM yet: NO_STATE");
	wr32(FAKE_MS + g_d->menu_off, 0x30060000u);
	check(luamb_target_state(&g_c, &g_io, LUAMB_TARGET_MENU, &st) == 0 && st == 0x30060000u,
	      "target: the menu's lua_State, two reads away");
	check(luamb_send_to(&g_c, &g_io, "return 1", 8, now, LUAMB_TARGET_MENU) == LUAMB_SEND_OK &&
	      *at(MB + LUAMB_OFF_TARGET, 1) == LUAMB_TARGET_MENU, "target: sent, the target byte written");
	g_ran = 0;
	helper_frame();
	check(g_ran == 0 && rd32(MB + LUAMB_OFF_ACK) != rd32(MB + LUAMB_OFF_REQ) && rd32(MB + LUAMB_OFF_BUSY) == 0,
	      "target: the gameplay frame leaves a menu request alone");
	menu_frame();
	now += 16;
	rc = luamb_poll(&g_c, &g_io, now, &res);
	check(rc == LUAMB_POLL_DONE && res.status == LUAMB_ST_OK, "target: the pause menu's frame answers it");

	/* Gameplay frames only: the menu's heartbeat stands still, so NOT_RUNNING. */
	luamb_send_to(&g_c, &g_io, "return 1", 8, now, LUAMB_TARGET_MENU);
	for (i = 0, rc = LUAMB_POLL_PENDING; i < 400 && rc == LUAMB_POLL_PENDING; i++) {
		helper_frame();
		now += 16;
		rc = luamb_poll(&g_c, &g_io, now, &res);
	}
	check(rc == LUAMB_POLL_DONE && res.status == LUAMB_ST_NOT_RUNNING,
	      "target: no pause menu frame in time is NOT_RUNNING, the gameplay frames notwithstanding");

	/* And a gameplay request goes back to target 0. */
	rc = run("return 1 + 1", 1, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_OK && *at(MB + LUAMB_OFF_TARGET, 1) == LUAMB_TARGET_GAMEPLAY,
	      "target: the next gameplay request says so, and runs");
	wr32(g_d->menu_ptr, 0);
}

static void test_timeouts(void)
{
	struct luamb_result res;
	u32 now = 50000;
	u32 seq;
	int rc;

	reset_game(0x30001000u);
	put_ident();
	luamb_client_init(&g_c, MB, g_d->vm + g_d->vm_state);
	luamb_attach(&g_c, &g_io, now);
	g_ran = 0;

	/* Paused: no gameplay frame runs, so no heartbeat. */
	rc = run("return 1 + 1", 0, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_NOT_RUNNING, "paused: NOT_RUNNING after the answer time");
	check(g_c.phase == LUAMB_C_IDLE, "paused: the helper never held it, so the slot is free at once");
	seq = res.seq;
	check(rd32(MB + LUAMB_OFF_CANCEL) == seq, "paused: the request is withdrawn");
	/* The game unpauses: the withdrawn request must not run late. */
	helper_frame();
	check(g_ran == 0 && rd32(MB + LUAMB_OFF_ACK) == seq && (s32)rd32(MB + LUAMB_OFF_STATUS) == LUAMB_ST_CANCELLED,
	      "paused: the next frame answers CANCELLED without running it");
	rc = run("return 1 + 1", 1, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_OK && g_ran == 1, "paused: the next request runs normally");

	/* Withdrawn, and a new request sent before the helper ever saw the old one. */
	rc = run("return 1 + 1", 0, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_NOT_RUNNING, "overtake: the first times out");
	g_ran = 0;
	rc = run("return true", 1, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_OK && res.out_type == LUAMB_T_BOOLEAN && g_ran == 1 &&
	      strcmp(g_last_chunk, "return true") == 0, "overtake: only the new chunk runs");

	/* Frames run but nothing answers (the helper sits inside the chunk). */
	g_claim_then_stop = 1;
	luamb_send(&g_c, &g_io, "return 1 + 1", 12, now);
	seq = g_c.seq;
	helper_frame();          /* claims: busy = seq */
	now += LUAMB_T_ANSWER_MS + 1;
	rc = luamb_poll(&g_c, &g_io, now, &res);
	check(rc == LUAMB_POLL_DONE && res.status == LUAMB_ST_TIMEOUT, "inside: TIMEOUT, since the heartbeat moved");
	check(g_c.phase == LUAMB_C_CANCELLING, "inside: the helper holds it, so qwark waits for its ack");
	check(luamb_send(&g_c, &g_io, "return 1", 8, now) == LUAMB_SEND_BUSY, "inside: nothing new meanwhile");
	now += LUAMB_T_HUNG_MS;
	check(luamb_poll(&g_c, &g_io, now, &res) == LUAMB_POLL_PENDING && g_c.hung, "inside: HUNG after the hang limit");
	/* The helper comes out: its late answer is dropped. */
	wr32(MB + LUAMB_OFF_STATUS, LUAMB_ST_OK);
	wr32(MB + LUAMB_OFF_ACK, seq);
	wr32(MB + LUAMB_OFF_BUSY, 0);
	g_claim_then_stop = 0;
	check(luamb_poll(&g_c, &g_io, now + 16, &res) == LUAMB_POLL_IDLE && !g_c.hung, "inside: the late ack frees the slot");
	rc = run("return 1 + 1", 1, &now, &res);
	check(rc == 0 && res.status == LUAMB_ST_OK, "inside: and the next request works");

	/* The race: the helper claimed it after its cancel check would have missed. */
	luamb_send(&g_c, &g_io, "return 1 + 1", 12, now);
	seq = g_c.seq;
	g_claim_then_stop = 1;
	helper_frame();
	g_claim_then_stop = 0;
	now += LUAMB_T_ANSWER_MS + 1;
	rc = luamb_poll(&g_c, &g_io, now, &res);
	check(rc == LUAMB_POLL_DONE && g_c.phase == LUAMB_C_CANCELLING, "race: busy seen after the cancel: wait");
	/* The helper's re-check finds the cancel: CANCELLED, never run. */
	g_ran = 0;
	wr32(MB + LUAMB_OFF_BUSY, 0);
	wr32(MB + LUAMB_OFF_ACK, seq - 1);     /* as it was before the frame */
	helper_frame();
	check(g_ran == 0 && rd32(MB + LUAMB_OFF_ACK) == seq, "race: the helper cancels it");
	check(luamb_poll(&g_c, &g_io, now + 16, &res) == LUAMB_POLL_IDLE, "race: and qwark is free");

	/* Answered after the answer time but before qwark looked: the answer wins. */
	luamb_send(&g_c, &g_io, "return true", 11, now);
	now += LUAMB_T_ANSWER_MS + 1;
	helper_frame();
	rc = luamb_poll(&g_c, &g_io, now, &res);
	check(rc == LUAMB_POLL_DONE && res.status == LUAMB_ST_OK, "late: an ack before the timeout's look is the answer");
}

static void test_attach(void)
{
	u32 now = 9000;

	reset_game(0x30001000u);
	put_ident();
	/* An earlier session left request 5 outstanding. */
	wr32(MB + LUAMB_OFF_REQ, 5);
	wr32(MB + LUAMB_OFF_ACK, 4);
	wr32(MB + LUAMB_OFF_CHUNK_LEN, 12);
	memcpy(at(MB + LUAMB_OFF_CHUNK, 12), "return 1 + 1", 12);
	luamb_client_init(&g_c, MB, g_d->vm + g_d->vm_state);
	check(luamb_attach(&g_c, &g_io, now) == LUAMB_POLL_IDLE, "attach: a stale request is taken up");
	check(rd32(MB + LUAMB_OFF_CANCEL) == 5, "attach: and withdrawn");
	g_ran = 0;
	helper_frame();
	check(g_ran == 0 && (s32)rd32(MB + LUAMB_OFF_STATUS) == LUAMB_ST_CANCELLED, "attach: it never runs");
	check(luamb_send(&g_c, &g_io, "return 1", 8, now) == LUAMB_SEND_OK && g_c.seq == 6, "attach: the next is seq 6");
}

static void test_patch(void)
{
	static u8 reply[16384];
	u32 len, n, i, off, words = 0;
	int rc;

	n = lh_patch_count(g_d);
	for (i = 0; i < g_d->ncaves; i++) words += (g_d->caves[i].len + 3) / 4;
	check(n == words + LH_PATCH_IDENT_WORDS + g_d->nhooks, "patch: caves, identity, hooks");

	rc = lh_patch_build(g_d, reply, sizeof(reply), &len);
	check(rc == ST_OK && be16_get(reply) == n, "patch: built, n words");
	check(len == LH_PATCH_HEAD + n * LH_PATCH_ROW + LH_PATCH_BYTES_HEAD, "patch: the length");
	check(be32_get(reply + 4) == qcrc32(reply + LH_PATCH_HEAD, len - LH_PATCH_HEAD), "patch: the stamp");
	check(be32_get(reply + LH_PATCH_HEAD) == g_d->caves[0].addr, "patch: the cave comes first");
	off = LH_PATCH_HEAD + words * LH_PATCH_ROW;
	check(be32_get(reply + off) == g_d->mailbox && be32_get(reply + off + 4) == LUAMB_MAGIC, "patch: then the magic");
	check(be32_get(reply + off + 8) == g_d->mailbox + 4 &&
	      be32_get(reply + off + 12) == ((LUAMB_VERSION << 16) | LUAMB_SIZE), "patch: then version and size");
	off += 2 * LH_PATCH_ROW;
	check(g_d->nhooks == 5 && be32_get(reply + off) == g_d->hooks[4].addr &&
	      be32_get(reply + off + LH_PATCH_ROW) == g_d->hooks[3].addr &&
	      be32_get(reply + off + 2 * LH_PATCH_ROW) == g_d->hooks[2].addr,
	      "patch: the galactic map's update first (revision 1.29, every planet), then the menu's two: the pause test, the menu's update");
	off += 3 * LH_PATCH_ROW;
	check(be32_get(reply + off) == g_d->hooks[1].addr, "patch: the error tap before the VM hook");
	check(be32_get(reply + off + LH_PATCH_ROW) == g_d->hooks[0].addr &&
	      be32_get(reply + off + LH_PATCH_ROW + 4) == g_d->hooks[0].value, "patch: the VM hook last");
	check(len <= QWARK_MAX_PAYLOAD, "patch: one reply holds it");
	check(be16_get(reply + len - 4) == 0, "patch: no single bytes");
	check(lh_patch_build(g_d, reply, len - 1, &len) == ST_FULL, "patch: refuses a short buffer");

	/* Presence: apply the words as RPCS3 would, then look. */
	reset_game(0);
	lh_patch_build(g_d, reply, sizeof(reply), &len);
	check(lh_present(g_d, &g_io) == 0, "present: not before the patch");
	g_fail_reads = 1;
	check(lh_present(g_d, &g_io) == -1, "present: unknown when the game cannot be read");
	g_fail_reads = 0;
	for (i = 0; i < n; i++) {
		u32 a = be32_get(reply + LH_PATCH_HEAD + i * LH_PATCH_ROW);

		if (at(a, 4) != NULL) memcpy(at(a, 4), reply + LH_PATCH_HEAD + i * LH_PATCH_ROW + 4, 4);
	}
	check(lh_present(g_d, &g_io) == 1, "present: every word reads back");
	*at(g_d->caves[0].addr + 0x40, 1) ^= 0xFF;
	check(lh_present(g_d, &g_io) == 0, "present: one cave byte off is not present");
	*at(g_d->caves[0].addr + 0x40, 1) ^= 0xFF;
	wr32(g_d->mailbox, 0);
	check(lh_present(g_d, &g_io) == 0, "present: no magic is not present");

	/* A console install, then out again. */
	reset_game(0x30001000u);
	wr32(g_d->hooks[1].addr, g_d->hooks[1].orig);
	wr32(g_d->mailbox + LUAMB_OFF_REQ, 77);       /* whatever was there */
	check(lh_install(g_d, &g_io, NULL) == ST_OK, "install: accepted on the right executable");
	check(lh_present(g_d, &g_io) == 1, "install: present afterwards");
	check(rd32(g_d->mailbox + LUAMB_OFF_REQ) == 0, "install: the header is zeroed, so nothing is outstanding");
	check(lh_install(g_d, &g_io, NULL) == ST_OK, "install: a second install over the first is fine");

	/* Revision 1.29 (pending): a console's install takes the cave from a source (ui.pak's), a piece at a time. */
	{
		static u8 piece[1000];
		struct lh_source src;

		reset_game(0x30001000u);
		wr32(g_d->hooks[1].addr, g_d->hooks[1].orig);
		src.read = src_read;
		src.ctx = (void *)g_d;
		src.buf = piece;
		src.cap = sizeof(piece);
		g_src_reads = 0;
		g_src_fail_at = -1;
		check(lh_install(g_d, &g_io, &src) == ST_OK && lh_present(g_d, &g_io) == 1 &&
		      g_src_reads == (g_d->caves[0].len + sizeof(piece) - 1) / sizeof(piece),
		      "install from a source: every cave byte through a 1000-byte buffer, present afterwards");
		reset_game(0x30001000u);
		wr32(g_d->hooks[1].addr, g_d->hooks[1].orig);
		g_src_reads = 0;
		g_src_fail_at = 3;
		check(lh_install(g_d, &g_io, &src) == ST_IO_ERROR && rd32(g_d->hooks[0].addr) == g_d->hooks[0].orig &&
		      rd32(g_d->hooks[1].addr) == g_d->hooks[1].orig,
		      "install from a source: a failed read is IO_ERROR with no hook word written");
		g_src_fail_at = -1;
		check(lh_install(g_d, &g_io, &src) == ST_OK, "install from a source: and goes in once it reads");
	}
	check(lh_uninstall(g_d, &g_io) == ST_OK && rd32(g_d->hooks[0].addr) == g_d->hooks[0].orig &&
	      rd32(g_d->hooks[1].addr) == g_d->hooks[1].orig, "uninstall: the game's words are back");
	wr32(g_d->hooks[0].addr, 0x60000000u);
	check(lh_install(g_d, &g_io, NULL) == ST_BAD_ARG, "install: refused on another executable");
}

static const struct tod_mark *mark(const char *name)
{
	u32 i;

	for (i = 0; i < tod_facts.nmarks; i++)
		if (strcmp(tod_facts.marks[i].name, name) == 0) return &tod_facts.marks[i];
	return NULL;
}

static void test_facts(void)
{
	const struct tod_facts *f = &tod_facts;
	u32 seen = 0, i, j;
	char chunk[128];
	int ok;

	check(strcmp(f->title, TOD_TITLE) == 0 && strcmp(g_d->title, TOD_TITLE) == 0, "facts: title");
	check(f->nplanets == 19, "facts: nineteen LEVEL_ ids");
	ok = 1;
	for (i = 0; i < f->nplanets; i++) ok &= f->planet_names[i] != NULL && f->planet_names[i][0] != 0;
	check(ok, "facts: every planet has a name");
	check(strcmp(f->planet_names[0], "Kerwan") == 0 && strcmp(f->planet_names[2], "Kortog") == 0 &&
	      strcmp(f->planet_names[4], "Voron Asteroid Belt") == 0 && strcmp(f->planet_names[18], "Fastoon II") == 0,
	      "facts: the planets as the ship names them, in the enum's order: Kerwan (Metropolis) = 0");

	for (i = 0; i < 16; i++) seen |= 1u << f->pad_remap[i];
	check(seen == 0xFFFF, "facts: the pad remap is a permutation");
	check(tod_pad_to_og(0x200) == 0x800, "pad: Start");
	check(tod_pad_to_og(0x400) == 0x200 && tod_pad_to_og(0x800) == 0x400, "pad: L3, R3");
	check(tod_pad_to_og(0x1) == 0x10 && tod_pad_to_og(0x4) == 0x40, "pad: Triangle, Cross");
	check(tod_pad_to_og(0x10) == 0x1 && tod_pad_to_og(0x80) == 0x8, "pad: L2, R1");
	check(tod_pad_to_og(0xF000) == 0xF000 && tod_pad_to_og(0x100) == 0x100, "pad: d-pad and Select unchanged");
	check(tod_pad_to_og(0x10 | 0x20 | 0x40 | 0x80 | 0x400 | 0x800) == 0x060F, "pad: the menu combo comes out as OG 0x060F");

	check(f->hot_mask + 4 <= f->hot_len && f->hot_released + 4 <= f->hot_len && f->hot_analog + 16 <= f->hot_len,
	      "facts: every hot field is inside the block");
	check(f->hot_addr + f->hot_mask == 0x10691970u, "facts: the mask is at 0x10691970");
	check(f->lua_state == g_d->vm + g_d->vm_state && f->lua_vm == g_d->vm, "facts: the VM agrees with the helper's");
	check(f->bolts == f->save_data + 0x41C && f->raritanium == f->save_data + 0x420, "facts: bolts and raritanium in the save");
	check(f->weapon_stride * f->nweapons == 0x280, "facts: 32 slots of 0x14");
	check(f->weapon_unlocked == f->save_data + 0x5754 && f->cheats == f->level_flags + 0x31D0, "facts: unlocks and cheats");
	check(f->cur_level == f->save_data + 0x906E8, "facts: the save's level");
	check(f->planet_request != f->cur_level, "facts: the load request is not the save's level");

	ok = 1;
	for (i = 0; i < f->nmarks; i++) ok &= f->marks[i].how >= TOD_DEC && f->marks[i].how <= TOD_INFER && f->marks[i].note;
	check(ok, "marks: every fact says how it is known");
	check(mark("fingerprint") && mark("fingerprint")->addr == f->fp_addr, "marks: fingerprint");
	check(mark("hot block") && mark("hot block")->addr == f->hot_addr, "marks: hot block");
	check(mark("current level") && mark("current level")->addr == f->cur_level, "marks: current level");
	check(mark("planet request") && mark("planet request")->addr == f->planet_request &&
	      mark("planet request")->how == TOD_DEC, "marks: planet request, from the decompile");
	check(mark("quit byte") && mark("quit byte")->addr == f->quit_byte, "marks: quit byte");
	check(mark("position root") && mark("position root")->addr == f->pos_root &&
	      mark("position root")->how == TOD_DEC, "marks: positions, from the decompile");
	check(mark("bolts") && mark("bolts")->addr == f->bolts, "marks: bolts");
	check(mark("raritanium") && mark("raritanium")->addr == f->raritanium, "marks: raritanium");
	check(mark("weapon unlocked") && mark("weapon unlocked")->addr == f->weapon_unlocked, "marks: weapon unlocks");
	check(mark("cheats") && mark("cheats")->addr == f->cheats, "marks: cheats");
	check(mark("lua state") && mark("lua state")->addr == f->lua_state, "marks: lua state");
	check(mark("game clock") && mark("game clock")->addr == f->game_clock, "marks: game clock");

	ok = 1;
	for (i = 0; i < f->nrows; i++) {
		const struct tod_row *r = &f->rows[i];

		for (j = 0; j < i; j++) ok &= f->rows[j].id != r->id;
		if (r->lua) ok &= r->write_addr == 0 && (r->flags & TOD_ROW_NEEDS_LUA) && strlen(r->lua) < LUAMB_CHUNK_CAP;
		else ok &= r->write_addr == f->planet_request && r->write_size == 4 && !(r->flags & TOD_ROW_NEEDS_LUA);
		if (r->kind == FEATURE_ENUM) ok &= r->options == f->nplanets;
	}
	check(ok, "rows: unique ids; a row is Lua or a write, never both");
	check(f->nrows >= 1 && strstr(f->rows[0].lua, "_VERSION") != NULL, "rows: the first is the Lua test");

	check(tod_row_chunk(&f->rows[3], 18, chunk, sizeof(chunk)) > 0 &&
	      strcmp(chunk, "set_level(18, true) return get_level()") == 0, "rows: %v becomes the option");
	check(tod_row_chunk(&f->rows[3], 0, chunk, sizeof(chunk)) > 0 && strncmp(chunk, "set_level(0,", 12) == 0,
	      "rows: option 0");
	check(tod_row_chunk(&f->rows[3], 18, chunk, 10) == -1, "rows: too small a buffer is refused");
	check(tod_row_chunk(&f->rows[2], 1, chunk, sizeof(chunk)) == -1, "rows: a write row has no chunk");
}

int main(void)
{
	g_d = lh_desc_for_title(TOD_TITLE);
	check(g_d != NULL && g_d->caves != NULL, "helper: ToD's descriptor, with its bytes in an emulator build");
	if (g_d == NULL || g_d->caves == NULL) {
		printf("test_tod: no descriptor\n");
		return 1;
	}
	check(lh_desc_for_title("NPEA00453") == NULL && lh_desc_for_title("NPEA0045") == NULL &&
	      lh_desc_for_title(NULL) == NULL, "helper: no other title");
	MB = g_d->mailbox;
	check(g_d->mailbox_size == LUAMB_SIZE && MB + LUAMB_SIZE <= RW_HI, "helper: the mailbox fits its page");

	test_layout();
	test_codec();
	test_requests();
	test_timeouts();
	test_target();
	test_attach();
	test_patch();
	test_facts();

	printf("test_tod: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
