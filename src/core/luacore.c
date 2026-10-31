/*
 * Revision 1.27: LUA_EXEC, LUA_STATUS and LUA_PATCH (luacore.h).
 *
 * The helper goes into the game two ways, as the savefile helper does:
 *
 *   a console      qwark writes it itself (lh_install: the mailbox's header,
 *                  the cave, the error tap, the VM hook last), on the first
 *                  LUA_EXEC of a process that asks for one in a level, never
 *                  before: a user's click is what puts code into the game
 *                  (DESIGN 3.7)
 *   RPCS3          which recompiles the game's code, an RPCS3 patch the client
 *                  writes from LUA_PATCH puts it in as the game loads; qwark
 *                  looks for it once a session (lh_present), and until it has
 *                  looked LUA_EXEC answers BUSY, after it UNSUPPORTED when the
 *                  patch is not in
 *
 * After that both are the same data writes and reads of the mailbox, which
 * PINE does as well as a console.
 */
#include "luacore.h"

#if QWARK_HAS_TOD

#include "session.h"
#include "mem.h"
#include "util.h"
#include "luamb_client.h"
#include "../games/game.h"
#include "../games/luahelper_patch.h"

#include <string.h>

static struct luamb_client g_lc;
static const struct lh_desc *g_desc;   /* the arrived game's, for the tick that took it */
static s8  g_present;                  /* RPCS3: -1 not looked yet, 0 no, 1 the patch is in */
static u8  g_installed;                /* a console: lh_install done this process */
static u8  g_attached;                 /* luamb_attach done this process */
static u8  g_have_result;
/* Revision 1.29: the menu glue's request, out (its seq) or none (0); its answer is never a LUA_EXEC's. */
static u32 g_glue_seq;
static struct luamb_result g_res;
static char g_out[LUAMB_OUT_CAP];
static u32 g_out_len;

static int io_read(void *ctx, u32 addr, void *buf, u32 len)
{
	(void)ctx;
	return mem_read(addr, buf, len) == ST_OK ? 0 : -1;
}

static int io_write(void *ctx, u32 addr, const void *buf, u32 len)
{
	(void)ctx;
	return mem_write(addr, buf, len) == ST_OK ? 0 : -1;
}

static const struct luamb_io g_io = { io_read, io_write, NULL };

static u32 now_ms(void)
{
	return (u32)(plat_time_us() / 1000u);
}

static void client_init(const struct lh_desc *d)
{
	luamb_client_init(&g_lc, d->mailbox, d->vm + d->vm_state);
	luamb_client_menu(&g_lc, d->menu_ptr, d->menu_off);
}

/* The running game's helper, under the core lock; NULL for none. */
static const struct lh_desc *desc_now(void)
{
	const struct game_api *g = session_game();

	if (g == NULL || g->lua_desc == NULL || !mem_is_ingame()) return NULL;
	return g->lua_desc();
}

void lua_core_enter(void)
{
	const struct lh_desc *d = desc_now();

	g_desc = d;
	g_present = -1;
	g_installed = 0;
	g_attached = 0;
	g_have_result = 0;
	g_glue_seq = 0;
	g_out_len = 0;
	g_out[0] = 0;
	memset(&g_res, 0, sizeof(g_res));
	if (d != NULL) client_init(d);
	else memset(&g_lc, 0, sizeof(g_lc));
}

void lua_core_tick(void)
{
	const struct lh_desc *d = desc_now();
	struct luamb_result res;
	int rc;

	if (d == NULL) return;
	if (d != g_desc) lua_core_enter();

#ifdef QWARK_EMULATOR
	/* The one look a session takes for the helper an RPCS3 patch put in at load. */
	if (g_present < 0 && !plat_can_patch_code()) {
		int found = lh_present(d, &g_io);

		if (found >= 0) {
			g_present = (s8)found;
			plat_log("lua: %s", found ? "the helper is in the game, patched in at load"
			                          : "no Lua helper in the game, or one from another build");
		}
	}
#endif

	/*
	 * Whatever an earlier session left outstanding in the mailbox is withdrawn
	 * before anything is sent: a request made minutes ago must never fire now.
	 * A mailbox with no helper behind it has no header, and nothing is written.
	 */
	if (!g_attached) {
		if (luamb_attach(&g_lc, &g_io, now_ms()) == LUAMB_POLL_IO) return;
		g_attached = 1;
	}

	if (g_lc.phase == LUAMB_C_IDLE) return;
	rc = luamb_poll(&g_lc, &g_io, now_ms(), &res);
	if (rc != LUAMB_POLL_DONE) return;

	rc = luamb_read_out(&g_lc, &g_io, &res, g_out, sizeof(g_out));
	if (g_glue_seq != 0 && res.seq == g_glue_seq) {
		/* Revision 1.29: the menu's glue, the core's own request. */
		g_glue_seq = 0;
		if (res.status != LUAMB_ST_OK)
			plat_log("menu: the pause menu's glue failed (%d): %s", (int)res.status, rc > 0 ? g_out : "");
		g_out_len = 0;
		g_out[0] = 0;
		return;
	}
	g_res = res;
	g_out_len = rc > 0 ? (u32)rc : 0;
	g_have_result = 1;
	if (res.status != LUAMB_ST_OK)
		plat_log("lua: request %u answered %d", (unsigned)res.seq, (int)res.status);
}

/*
 * The helper in the game: on a console written now if this process has not
 * yet (ST_BAD_ARG from lh_install, another executable, is UNSUPPORTED); under
 * RPCS3 the patch's, looked for once a session.
 */
static int ensure_helper(const struct lh_desc *d)
{
	int rc;

	if (plat_can_patch_code()) {
		if (!g_installed) {
			rc = lh_install(d, &g_io);
			if (rc == ST_BAD_ARG) return ST_UNSUPPORTED;
			if (rc != ST_OK) return rc;
			g_installed = 1;
			client_init(d);
			g_attached = 1;
			plat_log("lua: helper installed");
		}
	} else {
		if (g_present < 0) return ST_BUSY;
		if (g_present == 0) return ST_UNSUPPORTED;
	}
	return g_attached ? ST_OK : ST_BUSY;
}

int lua_core_helper_ready(void)
{
	const struct lh_desc *d;

	if (!mem_is_ingame()) return ST_NOT_INGAME;
	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;
	if (d != g_desc) lua_core_enter();
	return ensure_helper(d);
}

int lua_core_menu_glue(void)
{
	const struct lh_desc *d = desc_now();
	int rc;

	if (d == NULL || d->glue == NULL || d->glue_len == 0) return ST_UNSUPPORTED;
	if (d != g_desc) lua_core_enter();
	rc = ensure_helper(d);
	if (rc != ST_OK) return rc;
	if (g_lc.phase != LUAMB_C_IDLE) return ST_BUSY;
	switch (luamb_send_to(&g_lc, &g_io, (const char *)d->glue, d->glue_len, now_ms(), LUAMB_TARGET_MENU)) {
	case LUAMB_SEND_OK:       break;
	case LUAMB_SEND_BUSY:     return ST_BUSY;
	case LUAMB_SEND_NO_STATE: return ST_NOT_FOUND;
	case LUAMB_SEND_IO:       return ST_IO_ERROR;
	default:                  return ST_UNSUPPORTED;
	}
	g_glue_seq = g_lc.seq;
	return ST_OK;
}

int lua_core_exec_start(const u8 *req, u32 reqlen, u32 *seq)
{
	const struct lh_desc *d;
	u32 len, flags, state = 0;
	u8 target;
	int rc;

	*seq = 0;
	if (!mem_is_ingame()) return ST_NOT_INGAME;
	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;
	if (d != g_desc) lua_core_enter();

	if (reqlen < LUA_EXEC_HEAD) return ST_BAD_ARG;
	len = be16_get(req);
	flags = be16_get(req + 2);
	/* Revision 1.29: flags bit0, the pause menu's VM rather than the gameplay VM. */
	if ((flags & ~(u32)LUA_EXEC_MENU) != 0 || len == 0 || len > LUAMB_CHUNK_CAP || reqlen < LUA_EXEC_HEAD + len)
		return ST_BAD_ARG;
	target = (flags & LUA_EXEC_MENU) != 0 ? LUAMB_TARGET_MENU : LUAMB_TARGET_GAMEPLAY;
	if (target == LUAMB_TARGET_MENU && d->menu_ptr == 0) return ST_UNSUPPORTED;

	if (g_lc.phase != LUAMB_C_IDLE) return ST_BUSY;

	/* No level (or no pause menu), no Lua: nothing is written, and nothing is installed either. */
	if (luamb_target_state(&g_lc, &g_io, target, &state) != 0) return ST_IO_ERROR;
	if (state == 0) return ST_NOT_FOUND;

	rc = ensure_helper(d);
	if (rc != ST_OK) return rc;

	switch (luamb_send_to(&g_lc, &g_io, (const char *)(req + LUA_EXEC_HEAD), len, now_ms(), target)) {
	case LUAMB_SEND_OK:        break;
	case LUAMB_SEND_BUSY:      return ST_BUSY;
	case LUAMB_SEND_NO_HELPER: return ST_UNSUPPORTED;
	case LUAMB_SEND_NO_STATE:  return ST_NOT_FOUND;
	case LUAMB_SEND_TOO_LONG:  return ST_BAD_ARG;
	default:                   return ST_IO_ERROR;
	}

	g_have_result = 0;
	*seq = g_lc.seq;
	return ST_OK;
}

int lua_core_exec_result(u32 seq, u8 *reply, u32 cap, u32 *len, int *done)
{
	*len = 0;
	*done = 0;

	if (g_have_result && g_res.seq == seq) {
		u32 n = g_out_len;

		if (cap < LUA_EXEC_REPLY_HEAD) return ST_FULL;
		if (n > cap - LUA_EXEC_REPLY_HEAD) n = cap - LUA_EXEC_REPLY_HEAD;
		be32_put(reply, (u32)g_res.status);
		be32_put(reply + 4, g_res.seq);
		reply[8] = g_res.out_type;
		reply[9] = g_res.out_flags;
		be16_put(reply + 10, (u16)n);
		memcpy(reply + LUA_EXEC_REPLY_HEAD, g_out, n);
		*len = LUA_EXEC_REPLY_HEAD + n;
		*done = 1;
		return ST_OK;
	}
	/* Out still: the tick has not seen the answer, nor the time run out. */
	if (g_lc.phase == LUAMB_C_SENT && g_lc.seq == seq && desc_now() == g_desc) return ST_OK;
	*done = 1;
	return ST_BUSY;
}

int lua_core_status(u8 *reply, u32 cap, u32 *len)
{
	const struct lh_desc *d;
	struct luamb_hdr h;
	u8 *b = qscratch();
	u32 state = 0, n = 0;
	u8 installed, phase;

	*len = 0;
	if (!mem_is_ingame()) return ST_NOT_INGAME;
	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;
	if (d != g_desc) lua_core_enter();
	if (cap < LUA_STATUS_HEAD + LUAMB_ERR_CAP) return ST_FULL;

	memset(reply, 0, LUA_STATUS_HEAD);
	memset(&h, 0, sizeof(h));
	mem_read_u32(g_lc.state_ptr, &state);

	/* The header, the script's name and the error text: one read. */
	if (mem_read(d->mailbox, b, LUAMB_OFF_OUT) == ST_OK) luamb_hdr_decode(b, &h);
	else memset(b, 0, LUAMB_OFF_OUT);

	if (plat_can_patch_code()) {
		/* qwark's own install, or one an earlier qwark made in this process. */
		u32 hook = 0;

		installed = (u8)(g_installed ||
		            (luamb_hdr_valid(&h) && d->nhooks > 0 &&
		             mem_read_u32(d->hooks[0].addr, &hook) == ST_OK && hook == d->hooks[0].value));
	} else {
		installed = g_present < 0 ? LUA_INSTALLED_CHECKING : (u8)g_present;
	}
	phase = g_lc.hung ? LUA_PHASE_HUNG : g_lc.phase;

	reply[0] = installed;
	reply[1] = phase;
	be32_put(reply + 8, state);
	if (installed == LUA_INSTALLED_YES && luamb_hdr_valid(&h)) {
		u32 i;

		be32_put(reply + 4, h.heartbeat);
		be32_put(reply + 12, h.err_seq);
		be32_put(reply + 16, h.err_vm);
		for (i = 0; i < LUAMB_ERR_SCRIPT_CAP; i++) {
			reply[20 + i] = b[LUAMB_OFF_ERR_SCRIPT + i];
			if (b[LUAMB_OFF_ERR_SCRIPT + i] == 0) break;
		}
		reply[20 + LUAMB_ERR_SCRIPT_CAP - 1] = 0;
		while (n < h.err_len && n < LUAMB_ERR_CAP - 1 && b[LUAMB_OFF_ERR + n] != 0) n++;
		memcpy(reply + LUA_STATUS_HEAD, b + LUAMB_OFF_ERR, n);
	}
	be16_put(reply + 84, (u16)n);
	*len = LUA_STATUS_HEAD + n;
	return ST_OK;
}

#ifdef QWARK_EMULATOR
int lua_core_patch(u8 *reply, u32 cap, u32 *len)
{
	const struct game_api *g = session_game();
	const struct lh_desc *d;

	*len = 0;
	if (g == NULL || g->lua_desc == NULL) return ST_UNSUPPORTED;
	d = g->lua_desc();
	if (d == NULL) return ST_UNSUPPORTED;
	return lh_patch_build(d, reply, cap, len);
}
#endif

#endif /* QWARK_HAS_TOD */
