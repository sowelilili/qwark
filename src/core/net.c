#include "net.h"
#include "session.h"
#include "mem.h"
#include "features.h"
#include "mods.h"
#include "modusb.h"
#include "config.h"
#include "util.h"
#include "autosplit.h"
#include "savefile.h"
#include "switches.h"
#include "uipak.h"
#include "gamemod.h"
#include "manips.h"
#include "luacore.h"
#include "../plat/plat.h"
#include "../plat/plat_net.h"
#include "../games/game.h"

#include <string.h>

/*
 * Revision 1.27. Not on the wire: LUA_EXEC's second half, which only lua_exec
 * posts. op_needs_ring does not know it, so a frame that names it is answered
 * UNKNOWN_OP like any other op qwark has not got.
 */
#define OP_LUA_RESULT 0xFFE0

/*
 * The request arena, revision 1.11.
 *
 * Every non-control request used to allocate its own buffers out of the VSH's
 * small memory pool - sys_memory_allocate, 64 KB pages, up to three of them per
 * request in flight - because a frame could be 65600 bytes and a reply another
 * 65600. A frame is 16384 now, so one request buffer and one reply buffer of
 * that size are small enough to live in the module's own bss and be shared by
 * every connection. The module never calls sys_memory_allocate again.
 *
 * One request owns the arena at a time: it is taken before the payload is read,
 * held across the ring command or the inline handler, and released after the
 * reply has gone out. A second bulk request waits for it, and waits without
 * holding session_activity_lock and without becoming uncancellable, so a game
 * launch still closes it and the tick thread is never held up by it.
 *
 * The control ops (HELLO, HEARTBEAT, SUBSCRIBE, UNSUBSCRIBE, GET_STATE and,
 * since build 54, VSH_MEMORY) keep their own small per-connection buffers and never come near this, so they are
 * answered while another client holds the arena, exactly as they are answered
 * during a boot.
 *
 * Build 55 (SPEC-footprint.md, cut C4): the arena is one frame, not two. No op
 * takes a large request and gives a large reply: the bulk writes (MEM_WRITE,
 * FILE_WRITE, SAVEFILE_WRITE, PATCH_APPLY, LEVELFLAGS_SET, POS_STORE,
 * FILE_RENAME's two paths) answer in a few bytes, and every large reply comes
 * from a small request. So a request that fits the connection's own control
 * reply buffer (CONN_SMALL, 937 bytes) is read into that and the arena takes
 * the reply, a whole frame of it; a larger request is read into the arena and
 * its reply goes into the connection's buffer, FULL past 937 bytes, which every
 * handler checks as it has always checked its cap. Who holds the arena, and
 * for how long, is as it was: one request of one connection at a time.
 */
#define CONN_REPLY_CAP  QWARK_MAX_PAYLOAD
#define CONN_SMALL      TELEMETRY_MAX
#define REQUEST_TIMEOUT_US 2000000u

/* How long a connection waiting for the arena sleeps between tries. */
#define ARENA_WAIT_US   1000u

/*
 * A connection thread's stack, and it stays where it is. Its deepest chain is
 * DIR_DELETE's recursive walk - about 1.4 KB a level, five levels deep, which is
 * what the depth cap in rmdir_recursive is there to bound - at a little over
 * 12 KB, and SAVEFILE_LIST is next at 10 KB because its frame carries a 4 KB
 * buffer for the CRC it computes. Neither can borrow the tick thread's scratch
 * buffer, so neither number moves. See qwark_ps3.h for how they are measured.
 */
#define CLIENT_STACK    16384u

/*
 * A subscriber that has sent nothing for this long is not sent to. The entry stays,
 * and the next frame on its connection starts the packets again: a client that
 * went quiet for a while, or whose requests were held up, is never left polling
 * over TCP for a subscription the console threw away. It ends when the connection
 * closes or the client unsubscribes.
 */
#define SUB_TIMEOUT_US  5000000u

/* How long net_init sleeps between tries while the console's network comes up. */
#define NET_WAIT_STEP_US 500000u

/*
 * How long the accept thread waits after the listener is up before it asks the
 * XMB to show "qwark loaded and listening". See net_accept_thread for why there
 * is a wait at all. The host builds are the simulator and the two test suites,
 * where there is no XMB and no plugin loader to stay out of the way of, and
 * where plat_sleep_us really sleeps, so there the wait is nothing.
 */
#ifdef QWARK_HOST
#define BOOT_NOTIFY_DELAY_US 0u
#else
#define BOOT_NOTIFY_DELAY_US 1500000u
#endif

#define MAX_FILES       16
#define PATH_MAX_LEN    512

/* ------------------------------------------------------------------ state */

/*
 * `used` is the slot's life, and the connection thread clears it as the last
 * thing it does; net_wait_clients watches it on the way out. There is no thread
 * handle here because a connection thread is detached: nobody ever joins one,
 * so a client wedged in the network stack can never hold up a module unload.
 */
struct conn {
	int   sock;
	u8   *req;
	u8   *reply;
	/* 1 while this connection owns the shared request arena. */
	volatile u8 has_arena;
	volatile int cancelled;
	/* Control traffic never waits for the arena, including during launch. */
	u8    control_req[2];
	u8    control_reply[TELEMETRY_MAX];
	u32   remote_ip;      /* network byte order, as it came off the socket */
	u8    used;
};

struct sub {
	u32 ip;
	u16 port;
	u64 last_us;
	int conn_slot;
	u8  used;
};

struct filehandle {
	plat_file_t f;
	int  conn_slot;
	u8   used;
	/* Build 55: opened for writing on ui.pak, which is looked at again once it closes. */
	u8   pak;
};

static struct conn g_conns[QWARK_MAX_CLIENTS];
static struct sub  g_subs[QWARK_MAX_SUBS];
static struct filehandle g_files[MAX_FILES];

/*
 * The arena itself. One frame (two until build 55), and a flag that says
 * whether a connection has it. The flag is taken and given back under
 * g_arena_mutex; a connection that cannot have it sleeps and tries again rather
 * than blocking on the mutex, so it stays cancellable and holds no other lock
 * while it waits.
 */
static u8 g_arena[QWARK_MAX_PAYLOAD];
static plat_mutex_t g_arena_mutex;
static int g_arena_held;
static u32 g_arena_takes;

u32 net_arena_held(void)  { return g_arena_held ? 1u : 0u; }
u32 net_arena_takes(void) { return g_arena_takes; }

static int g_listen = -1;
static int g_udp = -1;
static volatile int g_working = 1;
/* Protected by core_lock, including the allocation itself. Closed at startup. */
static int g_booting = 1;

void net_set_booting(int booting)
{
	int i;
	core_lock();
	g_booting = booting;
	if (booting) {
		for (i = 0; i < QWARK_MAX_CLIENTS; i++) {
			/* Never take the arena back from a handler/ring command that owns it. */
			if (g_conns[i].has_arena) g_conns[i].cancelled = 1;
		}
	}
	core_unlock();
}

static plat_mutex_t g_net_mutex;
static plat_mutex_t g_file_mutex;

/* ------------------------------------------------------------- socket glue */

static void close_tracked(int *sock)
{
	int fd = *sock;
	*sock = -1;

	if (fd >= 0) {
		plat_socket_shutdown(fd);
		plat_socket_close(fd);
	}
}

/* Returns with the activity lock held. Pausing preserves frame offsets and
 * does not consume the payload/reply timeout. Cancelled bulk owners can leave
 * immediately to release their pages, even inside the quiet window. */
static int conn_activity(struct conn *c, u64 *deadline)
{
	for (;;) {
		u64 started;
		session_activity_lock();
		if (!g_working || c->cancelled) {
			session_activity_unlock();
			return -1;
		}
		if (!session_quiet()) return 0;
		started = plat_time_us();
		session_activity_unlock();
		plat_sleep_us(20000);
		if (deadline != NULL && *deadline != 0)
			*deadline += plat_time_us() - started;
	}
}

/* Nonblocking I/O makes cancellation independent of shutdown() wakeups. */
static int conn_io(struct conn *c, void *buf, u32 len, int writing, int idle)
{
	char *p = (char *)buf;
	u32 done = 0;
	u64 deadline = idle ? 0 : plat_time_us() + REQUEST_TIMEOUT_US;

	while (done < len) {
		int ready, res;
		if (conn_activity(c, &deadline) != 0) return -1;
		session_activity_unlock();
		if (deadline && plat_time_us() >= deadline) return -1;
		ready = plat_socket_wait(c->sock, writing, 20);
		if (ready == 0) continue;
		if (ready < 0) {
			if (plat_net_would_retry(plat_net_errno())) continue;
			return -1;
		}
		/* Detection can run while select waits. Recheck before touching a frame. */
		if (conn_activity(c, &deadline) != 0) return -1;
		res = writing ? (int)send(c->sock, p + done, (int)(len - done), 0) :
		                (int)recv(c->sock, p + done, (int)(len - done), 0);
		session_activity_unlock();
		if (res < 0) {
			if (plat_net_would_retry(plat_net_errno())) continue;
			return -1;
		}
		if (res == 0) return -1;
		done += (u32)res;
	}
	return 0;
}

static int send_all(struct conn *c, const void *buf, u32 len)
{
	return conn_io(c, (void *)buf, len, 1, 0);
}

static int recv_all(struct conn *c, void *buf, u32 len)
{
	return conn_io(c, buf, len, 0, 0);
}

/* ------------------------------------------------------------- subscribers */

static void subs_refresh(int conn_slot)
{
	u64 now = plat_time_us();
	int i;

	plat_mutex_lock(&g_net_mutex);
	for (i = 0; i < QWARK_MAX_SUBS; i++) {
		if (g_subs[i].used && g_subs[i].conn_slot == conn_slot)
			g_subs[i].last_us = now;
	}
	plat_mutex_unlock(&g_net_mutex);
}

static void subs_drop_conn(int conn_slot)
{
	int i;

	plat_mutex_lock(&g_net_mutex);
	for (i = 0; i < QWARK_MAX_SUBS; i++) {
		if (g_subs[i].used && g_subs[i].conn_slot == conn_slot)
			memset(&g_subs[i], 0, sizeof(g_subs[i]));
	}
	plat_mutex_unlock(&g_net_mutex);
}

static u16 subs_add(int conn_slot, u32 ip, u16 port)
{
	u64 now = plat_time_us();
	int i;
	u16 rc = ST_FULL;

	plat_mutex_lock(&g_net_mutex);

	/* Keyed by remote address and port, so re-subscribing just refreshes. */
	for (i = 0; i < QWARK_MAX_SUBS; i++) {
		if (g_subs[i].used && g_subs[i].ip == ip && g_subs[i].port == port) {
			g_subs[i].conn_slot = conn_slot;
			g_subs[i].last_us = now;
			plat_mutex_unlock(&g_net_mutex);
			return ST_OK;
		}
	}

	for (i = 0; i < QWARK_MAX_SUBS; i++) {
		if (g_subs[i].used) continue;
		g_subs[i].used = 1;
		g_subs[i].ip = ip;
		g_subs[i].port = port;
		g_subs[i].conn_slot = conn_slot;
		g_subs[i].last_us = now;
		rc = ST_OK;
		plat_log("qwark: subscribed, telemetry starts going out");
		break;
	}

	plat_mutex_unlock(&g_net_mutex);
	return rc;
}

/* Every call, whether or not anyone is subscribed. */
static u32 g_telemetry_sends;

u32 net_telemetry_sends(void)
{
	return g_telemetry_sends;
}

void net_send_telemetry(const u8 *packet, u32 len)
{
	u64 now;
	int i;

	g_telemetry_sends++;

	if (g_udp < 0 || len == 0) return;

	now = plat_time_us();

	plat_mutex_lock(&g_net_mutex);
	for (i = 0; i < QWARK_MAX_SUBS; i++) {
		struct sockaddr_in sa;

		if (!g_subs[i].used) continue;

		if (now - g_subs[i].last_us > SUB_TIMEOUT_US) continue;

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(g_subs[i].port);
		sa.sin_addr.s_addr = g_subs[i].ip;

		sendto(g_udp, (const char *)packet, (int)len, 0,
		       (struct sockaddr *)&sa, sizeof(sa));
	}
	plat_mutex_unlock(&g_net_mutex);
}

/* -------------------------------------------------------------- file paths */

/* Only /dev_hdd0/ and /dev_usb are reachable, and never through "..". */
static int path_ok(const char *path)
{
	u32 i;

	if (path == NULL) return 0;
	if (!(qstrneq(path, "/dev_hdd0/", 10) || qstrneq(path, "/dev_usb", 8))) return 0;

	for (i = 0; path[i] != 0; i++) {
		if (path[i] == '.' && path[i + 1] == '.') return 0;
	}

	return 1;
}

static int payload_path(const u8 *req, u32 reqlen, u32 offset, char *out, u32 cap)
{
	u32 len;

	if (reqlen < offset) return 0;
	len = reqlen - offset;
	if (len == 0 || len >= cap) return 0;

	memcpy(out, req + offset, len);
	out[len] = 0;
	return path_ok(out);
}

static int file_alloc(int conn_slot, plat_file_t f, u32 *handle, u8 pak)
{
	int i;

	plat_mutex_lock(&g_file_mutex);
	for (i = 0; i < MAX_FILES; i++) {
		if (g_files[i].used) continue;
		g_files[i].used = 1;
		g_files[i].f = f;
		g_files[i].conn_slot = conn_slot;
		g_files[i].pak = pak;
		*handle = (u32)(i + 1);
		plat_mutex_unlock(&g_file_mutex);
		return 1;
	}
	plat_mutex_unlock(&g_file_mutex);
	return 0;
}

static int file_get(int conn_slot, u32 handle, plat_file_t *out)
{
	int ok = 0;

	if (handle == 0 || handle > MAX_FILES) return 0;

	plat_mutex_lock(&g_file_mutex);
	if (g_files[handle - 1].used && g_files[handle - 1].conn_slot == conn_slot) {
		*out = g_files[handle - 1].f;
		ok = 1;
	}
	plat_mutex_unlock(&g_file_mutex);
	return ok;
}

static void file_release(int conn_slot, u32 handle)
{
	if (handle == 0 || handle > MAX_FILES) return;

	plat_mutex_lock(&g_file_mutex);
	if (g_files[handle - 1].used && g_files[handle - 1].conn_slot == conn_slot) {
		plat_file_close(g_files[handle - 1].f);
		if (g_files[handle - 1].pak & 1) uipak_touch(UIPAK_PATH);
		if (g_files[handle - 1].pak >> 4) gamemod_touch_game((u8)(g_files[handle - 1].pak >> 4));
		memset(&g_files[handle - 1], 0, sizeof(g_files[0]));
	}
	plat_mutex_unlock(&g_file_mutex);
}

static void file_release_conn(int conn_slot)
{
	int i;

	plat_mutex_lock(&g_file_mutex);
	for (i = 0; i < MAX_FILES; i++) {
		if (!g_files[i].used || g_files[i].conn_slot != conn_slot) continue;
		plat_file_close(g_files[i].f);
		if (g_files[i].pak & 1) uipak_touch(UIPAK_PATH);
		if (g_files[i].pak >> 4) gamemod_touch_game((u8)(g_files[i].pak >> 4));
		memset(&g_files[i], 0, sizeof(g_files[0]));
	}
	plat_mutex_unlock(&g_file_mutex);
}

static int rmdir_recursive(const char *path, int depth)
{
	plat_dir_t dir;
	struct plat_dirent ent;

	/*
	 * A client thread only has a 16 KB stack and each level costs about 1.3 KB
	 * (a plat_dir_t, a dirent and a path), so the tree qwark will walk is capped.
	 */
	if (depth > 5) return ST_BAD_ARG;

	if (plat_dir_open(path, &dir) != 0) return ST_NOT_FOUND;

	while (plat_dir_next(&dir, &ent) == 1) {
		char child[PATH_MAX_LEN];

		if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;

		child[0] = 0;
		qstrcat(child, sizeof(child), path);
		qstrcat(child, sizeof(child), "/");
		qstrcat(child, sizeof(child), ent.name);

		if (ent.is_dir) rmdir_recursive(child, depth + 1);
		else            plat_file_unlink(child);
	}

	plat_dir_close(&dir);
	return plat_dir_remove(path) == 0 ? ST_OK : ST_IO_ERROR;
}

static int mkdir_parents(const char *path)
{
	char work[PATH_MAX_LEN];
	u32 i;

	qstrcpy(work, sizeof(work), path);

	for (i = 1; work[i] != 0; i++) {
		if (work[i] != '/') continue;
		work[i] = 0;
		plat_dir_create(work);
		work[i] = '/';
	}

	plat_dir_create(work);
	return plat_path_exists(path, NULL, NULL) ? ST_OK : ST_IO_ERROR;
}

/* --------------------------------------------------- helpers for encoding */

/*
 * Revision 1.21. The four manip ops answer UNSUPPORTED for a game without
 * groups and outside a game, as DESCRIBE does; MANIP_APPLY, which needs the
 * game's state, answers NOT_INGAME while a game with groups is not INGAME.
 */
static int manip_gate(int needs_state)
{
	const struct game_api *g = session_game();

	if (g == NULL || g->manips == NULL) return ST_UNSUPPORTED;
	if (session_state() != SESSION_INGAME)
		return needs_state ? ST_NOT_INGAME : ST_UNSUPPORTED;
	if (!manips_supported()) return ST_UNSUPPORTED;
	return ST_OK;
}

/* ------------------------------------------------------ tick-thread handlers */

/*
 * Everything in here runs on the tick thread, out of the command ring. It is the
 * only place outside session.c that touches game memory.
 *
 * The caller holds the core lock, so a network thread can never observe a table
 * half-way through a mutation.
 */
static void ring_exec_locked(struct ring_cmd *cmd)
{
	const u8 *req = cmd->req;
	u32 reqlen = cmd->reqlen;
	u8 *reply = cmd->reply;

	cmd->replylen = 0;
	cmd->status = ST_OK;

	switch (cmd->op) {

	case OP_MEM_READ: {
		u32 addr, len;
		if (reqlen < 8) { cmd->status = ST_BAD_ARG; break; }
		addr = be32_get(req);
		len  = be32_get(req + 4);
		if (len == 0 || len > PLAT_MEM_MAX || len > cmd->replycap) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)mem_read(addr, reply, len);
		if (cmd->status == ST_OK) cmd->replylen = len;
		break;
	}

	case OP_MEM_WRITE: {
		u32 addr, len;
		if (reqlen < 5) { cmd->status = ST_BAD_ARG; break; }
		addr = be32_get(req);
		len = reqlen - 4;
		if (len > PLAT_MEM_MAX) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)mem_write(addr, req + 4, len);
		break;
	}

	case OP_WATCH_ADD: {
		u8 id = 0;
		if (reqlen < 5) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)watch_add(be32_get(req), req[4], &id);
		if (cmd->status == ST_OK) { reply[0] = id; cmd->replylen = 1; }
		break;
	}

	case OP_WATCH_REMOVE:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)watch_remove(req[0]);
		break;

	case OP_FREEZE_ADD: {
		u8 id = 0;
		if (reqlen < 16) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)freeze_add(be32_get(req), req[4], be64_get(req + 8), &id);
		if (cmd->status == ST_OK) { reply[0] = id; cmd->replylen = 1; }
		break;
	}

	case OP_FREEZE_REMOVE:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)freeze_remove(req[0]);
		break;

	case OP_PATCH_APPLY: {
		struct patch_word words[QWARK_MAX_PATCH_WORDS];
		u16 n;
		u16 i;

		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		n = be16_get(req);
		if (n == 0 || n > QWARK_MAX_PATCH_WORDS || reqlen < 4u + (u32)n * 8u) {
			cmd->status = ST_BAD_ARG;
			break;
		}

		for (i = 0; i < n; i++) {
			words[i].addr  = be32_get(req + 4 + i * 8);
			words[i].value = be32_get(req + 8 + i * 8);
		}
		cmd->status = (u16)client_patch_apply(words, n);
		break;
	}

	case OP_PATCH_REVERT:
		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)client_patch_revert(be32_get(req));
		break;

	case OP_CLEAR_CLIENT:
		cmd->status = (u16)mem_clear_client();
		break;

	case OP_FEATURE_SET:
		if (reqlen < 5) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)features_set(req[0], be32_get(req + 1));
		break;

	case OP_FEATURE_TRIGGER:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)features_trigger(req[0]);
		break;

	case OP_POS_SAVE:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)session_position_save(req[0]);
		break;

	case OP_POS_LOAD:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)session_position_load(req[0]);
		break;

	case OP_POS_EDIT: {
		f32 xyz[3];
		int i;

		if (reqlen < 16) { cmd->status = ST_BAD_ARG; break; }
		for (i = 0; i < 3; i++) xyz[i] = bef32_get(req + 4 + i * 4);
		cmd->status = (u16)session_position_edit(req[0], xyz);
		break;
	}

	case OP_POS_STORE:
		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		if (req[1] > QWARK_MAX_BLOB || reqlen < 4u + (u32)req[1]) {
			cmd->status = ST_BAD_ARG;
			break;
		}
		cmd->status = (u16)session_position_store(req[0], req + 4, req[1]);
		break;

	case OP_PLANET_LOAD:
		if (reqlen < 2) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)session_planet_load(req[0], req[1]);
		break;

	case OP_DIE:
		cmd->status = (u16)session_die();
		break;

	case OP_PREVIOUS_REAPPLY:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		session_previous_reapply(req[0]);
		break;

	case OP_PREVIOUS_DISMISS:
		session_previous_dismiss();
		break;

	case OP_MOD_LOAD:
	case OP_MOD_UNLOAD:
	case OP_MOD_RESCAN: {
		char dirname[33];

		if (cmd->op == OP_MOD_RESCAN) {
			cmd->status = (u16)mods_rescan();
			/* Build 56: and the USB drives, whatever the last look found. */
			modusb_scan(1);
			break;
		}

		if (reqlen < 32) { cmd->status = ST_BAD_ARG; break; }
		memcpy(dirname, req, 32);
		dirname[32] = 0;

		cmd->status = (u16)(cmd->op == OP_MOD_LOAD ? mods_load(dirname)
		                                           : mods_unload(dirname));
		break;
	}

	case OP_UNLOCK_LIST: {
		const struct game_api *g = session_game();
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		const struct unlock_field_desc *fields = NULL;
		u8 n = 0, ncat = 0;
		u32 off;
		u8 i;

		if (g == NULL || g->unlock_list == NULL || g->unlock_read == NULL) {
			cmd->status = ST_UNSUPPORTED;
			break;
		}
		cmd->status = (u16)g->unlock_list(&list, &n, &cats, &ncat, &fields);
		if (cmd->status != ST_OK) break;
		if (fields == NULL) { cmd->status = ST_UNSUPPORTED; break; }

		if (1u + (u32)ncat * 24u + 4u * UNLOCK_FIELD_WIRE_SIZE
		    + 1u + (u32)n * UNLOCK_WIRE_SIZE > cmd->replycap) {
			cmd->status = ST_FULL;
			break;
		}

		off = 0;
		reply[off++] = ncat;
		for (i = 0; i < ncat; i++) { qput_fixed(reply + off, 24, cats[i]); off += 24; }

		/*
		 * Protocol 1.3: four UnlockFieldDesc, always four, naming and typing
		 * the four value slots so the client draws a checkbox or a number box
		 * per game instead of guessing from RaC1's labels.
		 */
		for (i = 0; i < 4; i++) {
			qput_fixed(reply + off, UNLOCK_FIELD_NAME_LEN, fields[i].name);
			reply[off + UNLOCK_FIELD_NAME_LEN + 0] = fields[i].kind;
			reply[off + UNLOCK_FIELD_NAME_LEN + 1] = fields[i].max;
			reply[off + UNLOCK_FIELD_NAME_LEN + 2] = 0;
			reply[off + UNLOCK_FIELD_NAME_LEN + 3] = 0;
			off += UNLOCK_FIELD_WIRE_SIZE;
		}

		reply[off++] = n;
		for (i = 0; i < n; i++) {
			u32 values[4];
			u8 v;
			memset(values, 0, sizeof(values));
			g->unlock_read(&list[i], values);

			reply[off + 0] = list[i].id;
			reply[off + 1] = list[i].category;
			reply[off + 2] = list[i].fields;
			reply[off + 3] = 0;
			for (v = 0; v < 4; v++) be32_put(reply + off + 4 + v * 4, values[v]);
			qput_fixed(reply + off + 20, 24, list[i].name);
			off += UNLOCK_WIRE_SIZE;
		}
		cmd->replylen = off;
		break;
	}

	case OP_UNLOCK_SET: {
		const struct game_api *g = session_game();
		if (reqlen < 8) { cmd->status = ST_BAD_ARG; break; }
		if (g == NULL || g->unlock_set == NULL) { cmd->status = ST_UNSUPPORTED; break; }
		cmd->status = (u16)g->unlock_set(req[0], req[1], be32_get(req + 4));
		break;
	}

	case OP_LEVELFLAGS_GET: {
		const struct game_api *g = session_game();
		u32 cap;
		u16 len = 0;

		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		if (g == NULL || g->levelflags_get == NULL) { cmd->status = ST_UNSUPPORTED; break; }
		if (cmd->replycap < 2) { cmd->status = ST_FULL; break; }

		/* The hook takes a u16 cap, and the reply buffer is bigger than one. */
		cap = cmd->replycap - 2;
		if (cap > 0xFFFFu) cap = 0xFFFFu;

		cmd->status = (u16)g->levelflags_get(req[0], reply + 2, (u16)cap, &len);
		if (cmd->status != ST_OK) break;
		be16_put(reply, len);
		cmd->replylen = 2u + len;
		break;
	}

	case OP_LEVELFLAGS_RESET: {
		const struct game_api *g = session_game();
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		if (g == NULL || g->levelflags_reset == NULL) { cmd->status = ST_UNSUPPORTED; break; }
		cmd->status = (u16)g->levelflags_reset(req[0]);
		break;
	}

	case OP_LEVELFLAGS_SET: {
		const struct game_api *g = session_game();
		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		if (g == NULL || g->levelflags_set == NULL) { cmd->status = ST_UNSUPPORTED; break; }
		/* u8 planet, u8 value, u16 offset */
		cmd->status = (u16)g->levelflags_set(req[0], be16_get(req + 2), req[1]);
		break;
	}

#if QWARK_HAS_TOD
	/*
	 * Revision 1.28. One bit of LEVELFLAGS_GET's region, set or
	 * cleared on the tick: the byte is read and written back here, a frame
	 * apart at most, rather than by a client a refresh apart. The reply
	 * buffer holds the region while it works.
	 */
	case OP_LEVELFLAGS_BIT: {
		const struct game_api *g = session_game();
		u32 cap;
		u16 len = 0, bit;
		u8 byte, mask, want;

		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		if (g == NULL || g->levelflags_get == NULL || g->levelflags_set == NULL) {
			cmd->status = ST_UNSUPPORTED;
			break;
		}
		/* u8 planet, u8 on, u16 bit */
		bit = be16_get(req + 2);
		cap = cmd->replycap;
		if (cap > 0xFFFFu) cap = 0xFFFFu;
		cmd->status = (u16)g->levelflags_get(req[0], reply, (u16)cap, &len);
		if (cmd->status != ST_OK) break;
		if ((u32)(bit >> 3) >= len) { cmd->status = ST_BAD_ARG; break; }
		byte = reply[bit >> 3];
		mask = (u8)(1u << (bit & 7u));
		want = req[1] ? (u8)(byte | mask) : (u8)(byte & (u8)~mask);
		cmd->status = want == byte ? ST_OK : (u16)g->levelflags_set(req[0], (u16)(bit >> 3), want);
		break;
	}

	/*
	 * Revision 1.28: u8 ngroups, LevelFlagGroup[ngroups] (u8 planet,
	 * char[23] name), u16 n, LevelFlagName[n] (u16 bit, u16 number, u8 group,
	 * u8 len, char name[len]).
	 */
	case OP_LEVELFLAGS_NAMES: {
		const struct game_api *g = session_game();
		const struct levelflag_names *t;
		u32 off = 0, need;
		u16 i;

		if (g == NULL || g->levelflag_names == NULL || (t = g->levelflag_names()) == NULL) {
			cmd->status = ST_UNSUPPORTED;
			break;
		}
		need = 1u + (u32)t->ngroups * LEVELFLAG_GROUP_WIRE_SIZE + 2u;
		for (i = 0; i < t->nflags; i++) {
			u32 n = qstrlen(t->flags[i].name);
			need += LEVELFLAG_NAME_WIRE_HEAD + (n > 255u ? 255u : n);
		}
		if (need > cmd->replycap) { cmd->status = ST_FULL; break; }

		reply[off++] = t->ngroups;
		for (i = 0; i < t->ngroups; i++) {
			reply[off] = t->groups[i].planet;
			qput_fixed(reply + off + 1, LEVELFLAG_GROUP_WIRE_SIZE - 1u, t->groups[i].name);
			off += LEVELFLAG_GROUP_WIRE_SIZE;
		}
		be16_put(reply + off, t->nflags);
		off += 2;
		for (i = 0; i < t->nflags; i++) {
			const struct levelflag_name *f = &t->flags[i];
			u32 n = qstrlen(f->name);
			if (n > 255u) n = 255u;
			be16_put(reply + off, f->bit);
			be16_put(reply + off + 2, f->number);
			reply[off + 4] = f->group;
			reply[off + 5] = (u8)n;
			memcpy(reply + off + LEVELFLAG_NAME_WIRE_HEAD, f->name, n);
			off += LEVELFLAG_NAME_WIRE_HEAD + n;
		}
		cmd->replylen = off;
		break;
	}
#endif

	/*
	 * Protocol 1.8. The hold is tick-thread state that step_combos reads every
	 * tick, so it is set here rather than inline on a network thread. It is not
	 * a game write and answers whatever the session state is.
	 */
	case OP_COMBO_SUSPEND:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		session_combo_suspend(req[0]);
		break;

	/*
	 * Protocol 1.9. The savefile block. All three run here rather than on the
	 * network thread because all three touch game memory, and all three install
	 * the game's helper the first time they are asked.
	 */
	case OP_SAVEFILE_INFO: {
		u8 supported = 0, installed = 0, running = 0, pending = 0, error = 0;
		u32 size = 0, done = 0, total = 0;

		if (cmd->replycap < SAVEFILE_INFO_SIZE) { cmd->status = ST_FULL; break; }

		cmd->status = (u16)savefile_info(&supported, &installed, &running,
		                                 &pending, &size);
		if (cmd->status != ST_OK) break;

		savefile_transfer(&done, &total, &error);

		reply[0] = supported;
		reply[1] = installed;
		reply[2] = running;
		reply[3] = pending;
		be32_put(reply + 4, size);
		/*
		 * Revision 1.10. The three transfer fields sit past the eight bytes 1.9
		 * defined, so a client built against 1.9 reads the first half and is
		 * right about every byte of it.
		 */
		be32_put(reply + 8, done);
		be32_put(reply + 12, total);
		reply[16] = error;
		/* Revision 1.29 (pending): which of the game's checks `error` came from. */
		reply[17] = savefile_why_now();
		reply[18] = 0;
		reply[19] = 0;
		cmd->replylen = SAVEFILE_INFO_SIZE;
		break;
	}

	/*
	 * Protocol 1.10. The two transfers. Both start here on the tick thread,
	 * because both write the game's request byte, and both then run as a chunked
	 * state machine in savefile_tick so nothing else on this thread is held up.
	 */
	case OP_SAVEFILE_STORE:
	case OP_SAVEFILE_RESTORE: {
		char category[SAVEFILE_NAME_LEN + 1];
		char name[SAVEFILE_NAME_LEN + 1];

		if (reqlen < 2u * SAVEFILE_NAME_LEN) { cmd->status = ST_BAD_ARG; break; }

		memcpy(category, req, SAVEFILE_NAME_LEN);
		category[SAVEFILE_NAME_LEN] = 0;
		memcpy(name, req + SAVEFILE_NAME_LEN, SAVEFILE_NAME_LEN);
		name[SAVEFILE_NAME_LEN] = 0;

		cmd->status = (u16)(cmd->op == OP_SAVEFILE_STORE
		                    ? savefile_store(category, name)
		                    : savefile_restore(category, name));
		break;
	}

	case OP_SAVEFILE_READ: {
		u32 offset, len, got = 0;

		if (reqlen < 8) { cmd->status = ST_BAD_ARG; break; }
		offset = be32_get(req);
		len    = be32_get(req + 4);
		if (len == 0 || len > SAVEFILE_CHUNK_MAX || len > cmd->replycap) {
			cmd->status = ST_BAD_ARG;
			break;
		}

		cmd->status = (u16)savefile_read(offset, len, reply, &got);
		if (cmd->status == ST_OK) cmd->replylen = got;
		break;
	}

	case OP_SAVEFILE_WRITE: {
		u32 offset, len;

		if (reqlen < 5) { cmd->status = ST_BAD_ARG; break; }
		offset = be32_get(req);
		len = reqlen - 4;
		if (len > SAVEFILE_CHUNK_MAX) { cmd->status = ST_BAD_ARG; break; }

		cmd->status = (u16)savefile_write(offset, req + 4, len);
		break;
	}

	/*
	 * Revision 1.21, manip categories. All three run here: a selection can put
	 * a setup into the game straight away, Apply now writes, and the options sit
	 * beside state the tick thread's trigger reads.
	 */
	case OP_MANIP_SELECT:
		if (reqlen < 1) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)manip_gate(0);
		if (cmd->status == ST_OK) cmd->status = (u16)manips_select(req[0]);
		break;

	case OP_MANIP_OPTIONS:
		if (reqlen < 2) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)manip_gate(0);
		if (cmd->status == ST_OK) cmd->status = (u16)manips_options(req[0], req[1]);
		break;

	case OP_MANIP_APPLY:
		cmd->status = (u16)manip_gate(1);
		if (cmd->status == ST_OK) cmd->status = (u16)manips_apply();
		break;

#if QWARK_HAS_TOD
	/*
	 * Revision 1.27, the game's own Lua (luacore.h). LUA_EXEC runs here in two
	 * halves, which lua_exec below asks for one after the other: the send, and
	 * then, as often as it takes, whether the answer the tick watches for is in.
	 */
	case OP_LUA_EXEC: {
		u32 seq = 0;

		cmd->status = (u16)lua_core_exec_start(req, reqlen, &seq);
		if (cmd->status != ST_OK) break;
		if (cmd->replycap < 4) { cmd->status = ST_FULL; break; }
		be32_put(reply, seq);
		cmd->replylen = 4;
		break;
	}

	case OP_LUA_RESULT: {
		u32 len = 0;
		int done = 0;

		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		cmd->status = (u16)lua_core_exec_result(be32_get(req), reply, cmd->replycap, &len, &done);
		cmd->replylen = done ? len : 0;
		break;
	}

	case OP_LUA_STATUS: {
		u32 len = 0;

		cmd->status = (u16)lua_core_status(reply, cmd->replycap, &len);
		if (cmd->status == ST_OK) cmd->replylen = len;
		break;
	}
#endif

	default:
		cmd->status = ST_UNKNOWN_OP;
		break;
	}
}

static void net_ring_exec(struct ring_cmd *cmd)
{
	core_lock();
	if (g_booting) {
		cmd->status = ST_BUSY;
		cmd->replylen = 0;
	} else ring_exec_locked(cmd);
	core_unlock();
}

static int op_needs_ring(u16 op)
{
	switch (op) {
	case OP_MEM_READ:
	case OP_MEM_WRITE:
	case OP_WATCH_ADD:
	case OP_WATCH_REMOVE:
	case OP_FREEZE_ADD:
	case OP_FREEZE_REMOVE:
	case OP_PATCH_APPLY:
	case OP_PATCH_REVERT:
	case OP_CLEAR_CLIENT:
	case OP_FEATURE_SET:
	case OP_FEATURE_TRIGGER:
	case OP_POS_SAVE:
	case OP_POS_LOAD:
	case OP_POS_EDIT:
	case OP_POS_STORE:
	case OP_PLANET_LOAD:
	case OP_DIE:
	case OP_PREVIOUS_REAPPLY:
	case OP_PREVIOUS_DISMISS:
	case OP_MOD_LOAD:
	case OP_MOD_UNLOAD:
	case OP_MOD_RESCAN:
	case OP_UNLOCK_LIST:
	case OP_UNLOCK_SET:
	case OP_LEVELFLAGS_GET:
	case OP_LEVELFLAGS_RESET:
	case OP_LEVELFLAGS_SET:
#if QWARK_HAS_TOD
	case OP_LEVELFLAGS_BIT:
	case OP_LEVELFLAGS_NAMES:
#endif
	case OP_COMBO_SUSPEND:
	case OP_SAVEFILE_INFO:
	case OP_SAVEFILE_READ:
	case OP_SAVEFILE_WRITE:
	case OP_SAVEFILE_STORE:
	case OP_SAVEFILE_RESTORE:
	case OP_MANIP_SELECT:
	case OP_MANIP_OPTIONS:
	case OP_MANIP_APPLY:
#if QWARK_HAS_TOD
	case OP_LUA_EXEC:
	case OP_LUA_STATUS:
#endif
		return 1;
	default:
		return 0;
	}
}

#if QWARK_HAS_TOD
/*
 * LUA_EXEC on the connection's thread: the send through the ring, then a look
 * through the ring every 10 ms until the tick has the game's answer, which the
 * mailbox's own deadline (LUAMB_T_ANSWER_MS, src/core/luamb_client.h) bounds
 * at a second and a half: by then the tick has withdrawn the request and the
 * answer says TIMEOUT or NOT_RUNNING. LUA_EXEC_POLLS is only a backstop for a
 * tick thread that stops. The thread holds its buffers for as long as the
 * game takes, which is a frame or two while the game is playing.
 */
#define LUA_EXEC_POLLS   400
#define LUA_EXEC_POLL_US 10000u

static u16 lua_exec(const u8 *req, u32 reqlen, u8 *reply, u32 replycap, u32 *replylen,
                    volatile int *cancelled)
{
	struct ring_cmd cmd;
	u8 seq[4];
	int i;

	*replylen = 0;
	memset(&cmd, 0, sizeof(cmd));
	cmd.op = OP_LUA_EXEC;
	cmd.req = req;
	cmd.reqlen = reqlen;
	cmd.reply = reply;
	cmd.replycap = replycap;
	session_submit(&cmd);
	if (cmd.status != ST_OK) return cmd.status;
	if (cmd.replylen < 4) return ST_IO_ERROR;
	memcpy(seq, reply, 4);

	for (i = 0; i < LUA_EXEC_POLLS; i++) {
		plat_sleep_us(LUA_EXEC_POLL_US);
		if (*cancelled) return ST_BUSY;

		memset(&cmd, 0, sizeof(cmd));
		cmd.op = OP_LUA_RESULT;
		cmd.req = seq;
		cmd.reqlen = 4;
		cmd.reply = reply;
		cmd.replycap = replycap;
		session_submit(&cmd);
		if (cmd.status != ST_OK || cmd.replylen != 0) {
			*replylen = cmd.replylen;
			return cmd.status;
		}
	}
	return ST_BUSY;
}
#endif

/* --------------------------------------------------- network-thread handlers */

static u16 handle_inline(struct conn *c, int slot, u16 op,
                         const u8 *req, u32 reqlen,
                         u8 *reply, u32 replycap, u32 *replylen)
{
	*replylen = 0;

	switch (op) {

	case OP_HELLO:
		if (reqlen < 1) return ST_BAD_ARG;
		/* Revision 1.24: SessionInfo, then the build ID (PROTOCOL.md 3.1.1). */
		if (replycap < SESSION_INFO_SIZE + HELLO_BUILD_ID_SIZE) return ST_FULL;
		session_info_copy(reply, replycap);
		be32_put(reply + SESSION_INFO_SIZE, QWARK_BUILD_ID);
		*replylen = SESSION_INFO_SIZE + HELLO_BUILD_ID_SIZE;
		return ST_OK;

	case OP_HEARTBEAT:
		return ST_OK;

	case OP_NOTIFY: {
		char msg[256];
		u32 n = reqlen;
		if (n > 255) n = 255;
		memcpy(msg, req, n);
		msg[n] = 0;
		plat_notify(msg);
		return ST_OK;
	}

	case OP_PREVIOUS_LIST: {
		const struct previous_record *p;
		u32 off;
		u8 i;

		core_lock();
		p = session_previous();

		if (13u + (u32)p->nfreeze * 16u + 1u + (u32)p->npatch * 8u > replycap) {
			core_unlock();
			return ST_FULL;
		}

		be64_put(reply, p->toggles);
		be32_put(reply + 8, p->mods);
		reply[12] = p->nfreeze;
		off = 13;
		for (i = 0; i < p->nfreeze; i++) {
			reply[off + 0] = p->freezes[i].size;
			reply[off + 1] = 0;
			reply[off + 2] = 0;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, p->freezes[i].addr);
			be64_put(reply + off + 8, p->freezes[i].value);
			off += 16;
		}
		reply[off++] = p->npatch;
		for (i = 0; i < p->npatch; i++) {
			be32_put(reply + off, p->patches[i].first_addr);
			be16_put(reply + off + 4, p->patches[i].nwords);
			be16_put(reply + off + 6, 0);
			off += 8;
		}
		core_unlock();

		*replylen = off;
		return ST_OK;
	}

	case OP_SUBSCRIBE:
		if (reqlen < 2) return ST_BAD_ARG;
		return subs_add(slot, c->remote_ip, be16_get(req));

	case OP_UNSUBSCRIBE:
		subs_drop_conn(slot);
		return ST_OK;

	case OP_GET_STATE:
		*replylen = session_telemetry_copy(reply, replycap);
		return *replylen ? ST_OK : ST_FULL;

	/* Build 54. The session's last reading of the VSH's memory (PROTOCOL.md 5.1.1). */
	case OP_VSH_MEMORY:
		*replylen = session_vsh_memory(reply, replycap);
		return *replylen ? ST_OK : ST_UNSUPPORTED;

	/* Revision 1.26: the running game's module and the memory gate (PROTOCOL.md 5.1.2). */
	case OP_GAME_MODULE:
		*replylen = gamemod_info(reply, replycap);
		return *replylen ? ST_OK : ST_FULL;

	case OP_DESCRIBE: {
		u16 rc;
		/*
		 * The descriptor registry only becomes the running game's table at INGAME
		 * (features_set_game runs in enter_ingame). During BOOTING it still holds
		 * the previous game's table, so answer UNSUPPORTED until INGAME rather than
		 * hand a client a table for a game that is not the one now starting.
		 */
		/*
		 * Revision 1.26, the lock audit (PROTOCOL.md 9.11): the game is asked
		 * for under the core lock, which its going takes too, and the table is
		 * copied into the reply before the lock is let go.
		 */
		core_lock();
		if (session_game() == NULL || session_state() != SESSION_INGAME) rc = ST_UNSUPPORTED;
		else rc = (u16)features_describe(reply, replycap, replylen);
		core_unlock();
		return rc;
	}

	/*
	 * Revision 1.21. The list reads qwark's own state and config, never game
	 * memory, so it answers here under the core lock, as DESCRIBE does.
	 */
	case OP_MANIP_LIST: {
		u16 rc;

		core_lock();
		rc = (u16)manip_gate(0);
		if (rc == ST_OK) rc = (u16)manips_list(reply, replycap, replylen);
		core_unlock();
		return rc;
	}

	case OP_FEATURE_SET_AUTO: {
		u16 rc;
		if (reqlen < 2) return ST_BAD_ARG;
		core_lock();
		/* Revision 1.24: a COLOR's request carries the value to re-apply, u32 at +2. */
		if (session_game() == NULL) rc = ST_UNSUPPORTED;
		else rc = (u16)features_set_auto_value(req[0], req[1] != 0, reqlen >= 6 ? be32_get(req + 2) : 0);
		core_unlock();
		return rc;
	}

	case OP_FEATURE_OPTIONS: {
		const char * const *options = NULL;
		u8 count = 0;
		u16 rc;
		u8 i;
		u32 off;

		if (reqlen < 1) return ST_BAD_ARG;

		core_lock();
		rc = session_game() == NULL ? ST_UNSUPPORTED : (u16)features_options(req[0], &options, &count);
		if (rc != ST_OK) { core_unlock(); return rc; }
		if (1u + (u32)count * 24u > replycap) { core_unlock(); return ST_FULL; }

		reply[0] = count;
		off = 1;
		for (i = 0; i < count; i++) { qput_fixed(reply + off, 24, options[i]); off += 24; }
		core_unlock();

		*replylen = off;
		return ST_OK;
	}

#ifdef QWARK_EMULATOR
	/*
	 * Revision 1.17. The running game's code switches as the words an RPCS3
	 * patch writes. Like SAVEFILE_PATCH it reads the generated table and the
	 * session's game and nothing else, so it answers here, under the core lock.
	 * The emulator builds only; see the end of this switch for the PS3.
	 */
	case OP_SWITCH_PATCH: {
		u16 rc;

		core_lock();
		rc = (u16)switches_patch(reply, replycap, replylen);
		core_unlock();
		return rc;
	}
#endif

	/*
	 * Protocol 1.4. The catch-up read for the UDP push: everything the 64-entry
	 * ring still holds past `since_seq`. It answers whatever the session state
	 * is, because a client that reconnects after a crash still wants the splits
	 * that happened while it was away.
	 */
	case OP_AUTOSPLIT_EVENTS: {
		u32 n;

		if (reqlen < 4) return ST_BAD_ARG;

		n = autosplit_encode_events(be32_get(req), reply, replycap);
		if (n == 0) return ST_FULL;

		*replylen = n;
		return ST_OK;
	}

	case OP_AUTOSPLIT_DESCRIBE: {
		const struct game_api *g;
		const struct autosplit_desc *rows = NULL;
		u8 count = 0;
		u8 i;
		u32 off;

		/*
		 * Same gate as DESCRIBE: during BOOTING under BCES01503 session_game()
		 * is still only a guess, and handing a client the wrong game's split
		 * list is worse than making it wait.
		 */
		core_lock();   /* revision 1.26: the lock audit (PROTOCOL.md 9.11) */
		g = session_game();
		if (g == NULL || session_state() != SESSION_INGAME || g->autosplit_describe == NULL) {
			core_unlock();
			return ST_UNSUPPORTED;
		}

		rows = g->autosplit_describe(&count);
		if (rows == NULL || count == 0) { core_unlock(); return ST_UNSUPPORTED; }
		if (1u + (u32)count * AUTOSPLIT_DESC_SIZE > replycap) { core_unlock(); return ST_FULL; }

		reply[0] = count;
		off = 1;
		for (i = 0; i < count; i++) {
			reply[off + 0] = rows[i].code;
			reply[off + 1] = rows[i].kind;
			reply[off + 2] = rows[i].flags;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, rows[i].param_us);
			qput_fixed(reply + off + 8, AUTOSPLIT_LABEL_LEN, rows[i].label);
			off += AUTOSPLIT_DESC_SIZE;
		}

		core_unlock();
		*replylen = off;
		return ST_OK;
	}

	case OP_WATCH_LIST: {
		u32 off = 1;
		u8 n = 0;
		u32 i;

		core_lock();
		for (i = 0; i < QWARK_MAX_WATCHES; i++) {
			const struct watch_entry *w = watch_slot((u8)i);
			if (w == NULL) continue;
			if (off + 8 > replycap) break;
			reply[off + 0] = (u8)i;
			reply[off + 1] = w->size;
			reply[off + 2] = 0;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, w->addr);
			off += 8;
			n++;
		}
		core_unlock();

		reply[0] = n;
		*replylen = off;
		return ST_OK;
	}

	case OP_FREEZE_LIST: {
		u32 off = 1;
		u8 n = 0;
		u32 i;

		core_lock();
		for (i = 0; i < QWARK_MAX_FREEZES; i++) {
			const struct freeze_entry *f = freeze_slot((u8)i);
			if (f == NULL) continue;
			if (off + 16 > replycap) break;
			reply[off + 0] = (u8)i;
			reply[off + 1] = f->size;
			reply[off + 2] = 0;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, f->addr);
			be64_put(reply + off + 8, f->value);
			off += 16;
			n++;
		}
		core_unlock();

		reply[0] = n;
		*replylen = off;
		return ST_OK;
	}

	case OP_PATCH_LIST: {
		u32 off = 1;
		u8 n = 0;
		u32 i;
		u32 total;

		core_lock();
		total = patch_count();
		for (i = 0; i < total; i++) {
			const struct patch_def *d = patch_at(i);
			if (d == NULL) continue;
			if (off + 40 > replycap) break;
			be32_put(reply + off, d->count ? d->words[0].addr : 0);
			be16_put(reply + off + 4, d->count);
			reply[off + 6] = d->kind;
			reply[off + 7] = 0;
			qput_fixed(reply + off + 8, 32, d->name);
			off += 40;
			n++;
		}
		core_unlock();

		reply[0] = n;
		*replylen = off;
		return ST_OK;
	}

	case OP_POS_SELECT:
		if (reqlen < 1) return ST_BAD_ARG;
		if (req[0] >= QWARK_POS_SLOTS) return ST_BAD_ARG;
		/* Revision 1.24: under the core lock, as a profile switch swaps the store under it. */
		core_lock();
		config_set_selected_slot(req[0]);
		core_unlock();
		return ST_OK;

	case OP_POS_LIST: {
		const struct game_api *g;
		u8 planet;
		u8 s;
		u32 off;

		if (2u + QWARK_POS_SLOTS * 16u > replycap) return ST_FULL;

		core_lock();
		g = session_game();   /* revision 1.26: under the lock (PROTOCOL.md 9.11) */
		planet = session_current_planet();
		reply[0] = planet;
		reply[1] = QWARK_POS_SLOTS;
		off = 2;
		for (s = 0; s < QWARK_POS_SLOTS; s++) {
			u8 blob[QWARK_MAX_BLOB];
			u8 len = 0;
			f32 xyz[3];

			xyz[0] = 0.0f; xyz[1] = 0.0f; xyz[2] = 0.0f;

			if (pos_fetch(planet, s, blob, &len) == ST_OK) {
				reply[off] = 1;
				if (g != NULL && g->blob_xyz != NULL) g->blob_xyz(blob, len, xyz);
			} else {
				reply[off] = 0;
			}
			reply[off + 1] = 0;
			reply[off + 2] = 0;
			reply[off + 3] = 0;
			bef32_put(reply + off + 4, xyz[0]);
			bef32_put(reply + off + 8, xyz[1]);
			bef32_put(reply + off + 12, xyz[2]);
			off += 16;
		}
		core_unlock();

		*replylen = off;
		return ST_OK;
	}

	case OP_POS_CLEAR: {
		u16 rc;
		if (reqlen < 1) return ST_BAD_ARG;
		core_lock();
		rc = (u16)pos_clear(session_current_planet(), req[0]);
		core_unlock();
		return rc;
	}

	case OP_PLANET_LIST: {
		const struct game_api *g;
		const char * const *names;
		u8 count = 0;
		u8 i;
		u32 off;

		core_lock();   /* revision 1.26: the lock audit (PROTOCOL.md 9.11) */
		g = session_game();
		if (g == NULL || g->planet_names == NULL) { core_unlock(); return ST_UNSUPPORTED; }

		names = g->planet_names(&count);
		if (names == NULL) { core_unlock(); return ST_UNSUPPORTED; }
		if (1u + (u32)count * 25u > replycap) { core_unlock(); return ST_FULL; }

		reply[0] = count;
		off = 1;
		for (i = 0; i < count; i++) { qput_fixed(reply + off, 24, names[i]); off += 24; }

		/*
		 * Build 53, revision 1.21: then the order a menu lists them in, one
		 * index per planet. A client from before reads the names and stops,
		 * so the bytes after them are new to nobody.
		 */
		for (i = 0; i < count; i++)
			reply[off++] = g->planet_order != NULL ? g->planet_order[i] : i;

		core_unlock();
		*replylen = off;
		return ST_OK;
	}

	case OP_PLANET_SELECT: {
		u16 rc;
		if (reqlen < 2) return ST_BAD_ARG;
		/*
		 * Build 53: one game's selection, the running game's or the last
		 * one's, rather than the console's. Under the core lock, which the
		 * tick thread holds to read it into the info block.
		 */
		core_lock();
		rc = (u16)session_planet_select(req[0], req[1]);
		core_unlock();
		return rc;
	}

	case OP_MOBY_TABLE: {
		const struct game_api *g;
		u32 tbl = 0, end = 0;
		u16 stride = 0;
		u16 rc;

		if (replycap < 12) return ST_FULL;
		core_lock();   /* revision 1.26: the lock audit (PROTOCOL.md 9.11) */
		g = session_game();
		rc = (g == NULL || g->moby_table == NULL) ? ST_UNSUPPORTED : (u16)g->moby_table(&tbl, &end, &stride);
		core_unlock();
		if (rc != ST_OK) return rc;

		be32_put(reply, tbl);
		be32_put(reply + 4, end);
		be16_put(reply + 8, stride);
		be16_put(reply + 10, 0);
		*replylen = 12;
		return ST_OK;
	}

	case OP_MOD_LIST: {
		u16 rc;
		core_lock();
		rc = (u16)mods_list_encode(reply, replycap, replylen);
		core_unlock();
		return rc;
	}

	/*
	 * Revision 1.24. The mods on the USB drives, read again when the last look
	 * is over a second old: file reads on this thread, under the core lock, as
	 * MOD_INFO's are, and only when a client asks. UNSUPPORTED in a release,
	 * which leaves them out (modusb.h, QWARK_PREVIEW), as where code cannot be
	 * patched.
	 */
	case OP_MOD_USB_LIST: {
		u16 rc;

		if (!modusb_enabled()) return ST_UNSUPPORTED;
		core_lock();
		rc = (u16)modusb_list_encode(reply, replycap, replylen);
		core_unlock();
		return rc;
	}

	case OP_MOD_SET_AUTO: {
		char dirname[33];
		u16 rc;

		if (reqlen < 33) return ST_BAD_ARG;
		memcpy(dirname, req, 32);
		dirname[32] = 0;

		core_lock();
		rc = (u16)mods_set_auto(dirname, req[32] != 0);
		core_unlock();
		return rc;
	}

	case OP_MOD_INFO: {
		char dirname[33];
		u16 rc;
		u32 n = 0;

		if (reqlen < 32) return ST_BAD_ARG;
		memcpy(dirname, req, 32);
		dirname[32] = 0;

		core_lock();
		rc = (u16)mods_info(dirname, (char *)reply,
		                    replycap > 1024 ? 1024 : replycap, &n);
		core_unlock();

		*replylen = n;
		return rc;
	}

#ifdef QWARK_EMULATOR
	/*
	 * Revision 1.16. One mod as the words and bytes an RPCS3 patch writes. It
	 * reads the mod table and the mod's own files and never the game, so it
	 * answers here, under the core lock, the way MOD_LIST and MOD_INFO do, and
	 * in the same session states they do. The emulator builds only.
	 *
	 * Revision 1.19: a page at a time, `u8 index, u8 pad, u16 page`, so a mod
	 * bigger than one frame still goes out (mods.h).
	 */
	case OP_MOD_PATCH: {
		u16 rc;

		if (reqlen < 4) return ST_BAD_ARG;

		core_lock();
		rc = (u16)mods_patch_request(req, reqlen, reply, replycap, replylen);
		core_unlock();
		return rc;
	}
#endif

	case OP_FILE_OPEN: {
		char path[PATH_MAX_LEN];
		plat_file_t f;
		u32 handle = 0;

		if (reqlen < 2) return ST_BAD_ARG;
		if (req[0] > 1) return ST_BAD_ARG;
		if (!payload_path(req, reqlen, 1, path, sizeof(path))) return ST_BAD_ARG;
		if (replycap < 4) return ST_FULL;

		if (plat_file_open(path, req[0] == 0 ? PLAT_OPEN_READ : PLAT_OPEN_WRITE, &f) != 0)
			return ST_IO_ERROR;

		/*
		 * Build 55: a write of ui.pak has the pak looked at again as it closes
		 * (bit0); revision 1.26: and one of a game module (the game's id in the
		 * high nibble), the session's game's module.
		 */
		if (!file_alloc(slot, f, &handle,
		                (u8)(req[0] == 1 ? (uipak_names(path) ? 1 : 0) | (gamemod_names(path) << 4) : 0))) {
			plat_file_close(f);
			return ST_FULL;
		}

		be32_put(reply, handle);
		*replylen = 4;
		return ST_OK;
	}

	case OP_FILE_WRITE: {
		plat_file_t f;
		u32 len;

		if (reqlen < 4) return ST_BAD_ARG;
		if (!file_get(slot, be32_get(req), &f)) return ST_NOT_FOUND;

		len = reqlen - 4;
		if (len > QWARK_MAX_PAYLOAD) return ST_BAD_ARG;
		if (len == 0) return ST_OK;

		return plat_file_write(f, req + 4, len) == 0 ? ST_OK : ST_IO_ERROR;
	}

	case OP_FILE_READ: {
		plat_file_t f;
		u32 want;
		u32 got = 0;

		if (reqlen < 8) return ST_BAD_ARG;
		if (!file_get(slot, be32_get(req), &f)) return ST_NOT_FOUND;

		want = be32_get(req + 4);
		if (want > QWARK_MAX_PAYLOAD) want = QWARK_MAX_PAYLOAD;
		if (want > replycap) want = replycap;

		if (plat_file_read(f, reply, want, &got) != 0) return ST_IO_ERROR;

		*replylen = got;
		return ST_OK;
	}

	case OP_FILE_CLOSE:
		if (reqlen < 4) return ST_BAD_ARG;
		file_release(slot, be32_get(req));
		return ST_OK;

	case OP_FILE_DELETE: {
		char path[PATH_MAX_LEN];
		int rc;

		if (!payload_path(req, reqlen, 0, path, sizeof(path))) return ST_BAD_ARG;
		rc = plat_file_unlink(path) == 0 ? ST_OK : ST_NOT_FOUND;
		/* Build 55: after the op, so the look the tick takes sees what it left. */
		uipak_touch(path);
		gamemod_touch_game(gamemod_names(path));
		return rc;
	}

	case OP_DIR_LIST: {
		char path[PATH_MAX_LEN];
		plat_dir_t dir;
		struct plat_dirent ent;
		u32 off = 2;
		u16 n = 0;

		if (!payload_path(req, reqlen, 0, path, sizeof(path))) return ST_BAD_ARG;
		if (plat_dir_open(path, &dir) != 0) return ST_NOT_FOUND;

		while (plat_dir_next(&dir, &ent) == 1) {
			u32 namelen = qstrlen(ent.name);
			if (namelen > 255) namelen = 255;
			if (off + 6 + namelen > replycap) break;

			reply[off] = ent.is_dir ? 1 : 0;
			reply[off + 1] = (u8)namelen;
			be32_put(reply + off + 2, (u32)ent.size);
			memcpy(reply + off + 6, ent.name, namelen);
			off += 6 + namelen;
			n++;
		}
		plat_dir_close(&dir);

		be16_put(reply, n);
		*replylen = off;
		return ST_OK;
	}

	case OP_DIR_CREATE: {
		char path[PATH_MAX_LEN];
		if (!payload_path(req, reqlen, 0, path, sizeof(path))) return ST_BAD_ARG;
		return (u16)mkdir_parents(path);
	}

	case OP_DIR_DELETE: {
		char path[PATH_MAX_LEN];
		u16 rc;

		if (!payload_path(req, reqlen, 0, path, sizeof(path))) return ST_BAD_ARG;
		rc = (u16)rmdir_recursive(path, 0);
		uipak_touch(path);
		return rc;
	}

	case OP_USER_ID:
		if (replycap < 4) return ST_FULL;
		be32_put(reply, plat_user_id());
		*replylen = 4;
		return ST_OK;

	/*
	 * Protocol 1.10. `u16 from_len`, the old path, then the new one as the rest
	 * of the payload: every other op in this block ends with a path, and one
	 * length is all it takes to fit two of them into that shape.
	 *
	 * A destination that already exists is refused rather than replaced. cellFs
	 * and Windows refuse one and POSIX replaces it silently, and a rename that
	 * means different things on the console and in the simulator is worse than
	 * one that overwrites nothing anywhere.
	 */
	case OP_FILE_RENAME: {
		char from[PATH_MAX_LEN];
		char to[PATH_MAX_LEN];
		u32 nfrom, nto;

		if (reqlen < 3) return ST_BAD_ARG;

		nfrom = be16_get(req);
		if (nfrom == 0 || nfrom >= sizeof(from) || reqlen <= 2u + nfrom) return ST_BAD_ARG;

		nto = reqlen - 2u - nfrom;
		if (nto == 0 || nto >= sizeof(to)) return ST_BAD_ARG;

		memcpy(from, req + 2, nfrom);
		from[nfrom] = 0;
		memcpy(to, req + 2 + nfrom, nto);
		to[nto] = 0;

		if (!path_ok(from) || !path_ok(to)) return ST_BAD_ARG;
		if (!plat_path_exists(from, NULL, NULL)) return ST_NOT_FOUND;
		if (plat_path_exists(to, NULL, NULL)) return ST_BAD_ARG;

		{
			u16 rc = plat_file_rename(from, to) == 0 ? ST_OK : ST_IO_ERROR;

			/* Build 55: a client putting ui.pak in place, or taking it away. */
			uipak_touch(from);
			uipak_touch(to);
			/* Revision 1.26: and a game module. */
			gamemod_touch_game(gamemod_names(from));
			gamemod_touch_game(gamemod_names(to));
			return rc;
		}
	}

	/*
	 * Protocol 1.10, the savefile library's three read-and-arrange ops. They
	 * touch files and never game memory, so they answer here rather than on the
	 * tick thread: a directory walk, and the one CRC over a 2 MB file a client's
	 * own upload leaves to be summed, would otherwise be a stall of the 120 Hz
	 * loop. The gate is taken under the core lock, the work is not.
	 */
	case OP_SAVEFILE_CATEGORIES: {
		u16 rc;

		core_lock();
		rc = (u16)savefile_library_gate();
		core_unlock();
		if (rc != ST_OK) return rc;

		return (u16)savefile_categories(reply, replycap, replylen);
	}

	case OP_SAVEFILE_LIST: {
		char category[SAVEFILE_NAME_LEN + 1];
		u16 rc;

		if (reqlen < SAVEFILE_NAME_LEN) return ST_BAD_ARG;
		memcpy(category, req, SAVEFILE_NAME_LEN);
		category[SAVEFILE_NAME_LEN] = 0;

		core_lock();
		rc = (u16)savefile_library_gate();
		core_unlock();
		if (rc != ST_OK) return rc;

		return (u16)savefile_list(category, reply, replycap, replylen);
	}

	case OP_SAVEFILE_CATEGORY: {
		char name[SAVEFILE_NAME_LEN + 1];
		u16 rc;

		if (reqlen < 1u + SAVEFILE_NAME_LEN) return ST_BAD_ARG;
		memcpy(name, req + 1, SAVEFILE_NAME_LEN);
		name[SAVEFILE_NAME_LEN] = 0;

		core_lock();
		rc = (u16)savefile_library_gate();
		core_unlock();
		if (rc != ST_OK) return rc;

		return (u16)savefile_category(req[0], name);
	}

#ifdef QWARK_EMULATOR
	/*
	 * Revision 1.15. The helper as the words an RPCS3 patch writes. It reads the
	 * generated table and the session's game and nothing else - no game memory,
	 * no file - so it answers here the way DESCRIBE does, under the core lock the
	 * library gate above takes for the same session state. The emulator builds
	 * only.
	 */
	case OP_SAVEFILE_PATCH: {
		u16 rc;

		core_lock();
		rc = (u16)savefile_patch(reply, replycap, replylen);
		core_unlock();
		return rc;
	}
#endif

	/*
	 * The console's mask, always: revision 1.24 keeps a profile's own in an
	 * override (COMBO_OVERRIDE, below), so a profile's override hides this
	 * write from its user until the override goes. Every config op from here
	 * to MENU_HELP takes the core lock, which a profile switch holds while it
	 * swaps the profile's keys.
	 */
	case OP_COMBO_SET: {
		u16 rc;
		if (reqlen < 8) return ST_BAD_ARG;
		if (req[0] >= COMBO_COUNT) return ST_BAD_ARG;
		core_lock();
		rc = (u16)config_set_combo(req[0], be32_get(req + 4));
		core_unlock();
		return rc;
	}

	/*
	 * The mask in force, the active profile's override where it has one.
	 * Revision 1.24: the byte after the action, a pad until then, is the row's
	 * flags, bit0 OVERRIDDEN.
	 */
	case OP_COMBO_LIST: {
		u8 a;
		u32 off = 1;

		if (1u + COMBO_COUNT * 8u > replycap) return ST_FULL;

		core_lock();
		reply[0] = COMBO_COUNT;
		for (a = 0; a < COMBO_COUNT; a++) {
			reply[off] = a;
			reply[off + 1] = config_combo_overridden(a) ? COMBO_LIST_OVERRIDDEN : 0;
			reply[off + 2] = 0;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, config_combo(a));
			off += 8;
		}
		core_unlock();
		*replylen = off;
		return ST_OK;
	}

#if QWARK_PREVIEW
	/*
	 * Revision 1.24. One combo's override in the active profile: u8 action,
	 * u8 set, u8 pad[2], u32 mask. A config write like COMBO_SET, answered in
	 * any session state; the tick thread's cached masks follow the config
	 * version.
	 */
	case OP_COMBO_OVERRIDE: {
		u16 rc;
		if (reqlen < 8 || req[0] >= COMBO_COUNT || req[1] > 1) return ST_BAD_ARG;
		if (!plat_has_users()) return ST_UNSUPPORTED;
		core_lock();
		rc = (u16)config_set_combo_override(req[0], req[1], be32_get(req + 4));
		core_unlock();
		return rc;
	}

	/*
	 * Revision 1.24. The active profile and the signed-in user, 64 bytes
	 * (PROTOCOL.md 5.10.1). The user is asked for here, on demand: one VSH
	 * call. The name field stays empty until named profiles come.
	 */
	case OP_PROFILE_INFO: {
		u32 user;
		u8 flags;

		if (replycap < PROFILE_INFO_SIZE) return ST_FULL;
		memset(reply, 0, PROFILE_INFO_SIZE);
		*replylen = PROFILE_INFO_SIZE;
		if (!plat_has_users()) return ST_OK;

		user = plat_user_id();
		if (user > 99999999u) user = 0;

		core_lock();
		flags = config_profile_flags();
		if (user != 0 && config_profile_exists(user)) flags |= PROFILE_INFO_HAS_PROFILE;
		reply[0] = flags;
		reply[1] = config_profile_active() ? 1 : 0;
		reply[2] = config_profile_seq();
		core_unlock();

		be32_put(reply + 4, user);
		{
			char folder[PROFILE_FOLDER_MAX];
			config_profile_folder_for(user, folder, sizeof(folder));
			memcpy(reply + 8, folder, qstrlen(folder));
		}
		return ST_OK;
	}

	/*
	 * Revision 1.24. The console's switch, and the signed-in user's profile
	 * made as it goes on: a copy of the console's per-user settings, or empty.
	 * It acts at once, in a game too: it is the user's own request. Only the
	 * session's look for a new user at the XMB waits for the XMB (session.c).
	 */
	case OP_PROFILE_ENABLE: {
		u16 rc;
		int changed = 0;
		if (reqlen < 2 || req[0] > 1 || req[1] > PROFILE_SETUP_FRESH) return ST_BAD_ARG;
		if (!plat_has_users()) return ST_UNSUPPORTED;
		core_lock();
		/* Build 56: all or nothing, the switch and the setup (config.h). */
		rc = (u16)config_profiles_enable(req[0], req[1], plat_user_id(), &changed);
		if (changed) session_profile_changed();
		core_unlock();
		return rc;
	}
#else
	/*
	 * A release has no settings profiles (proto.h, QWARK_PREVIEW): their ops
	 * are known and reserved, so UNSUPPORTED rather than UNKNOWN_OP, as the
	 * RPCS3 patch ops are on a console.
	 */
	case OP_COMBO_OVERRIDE:
	case OP_PROFILE_INFO:
	case OP_PROFILE_ENABLE:
		return ST_UNSUPPORTED;
#endif

	/*
	 * Protocol 1.12. The persistent switch. It is a config write and nothing
	 * else, exactly like COMBO_SET beside it: no game memory is touched, so it
	 * does not go to the tick thread and it answers whatever the session state
	 * is. The tick thread picks the new value up on its next tick, through the
	 * same config version the masks are cached against.
	 */
	case OP_COMBO_ENABLE:
		if (reqlen < 1) return ST_BAD_ARG;
		if (req[0] > 1) return ST_BAD_ARG;
		{
			u16 rc;
			core_lock();
			rc = (u16)config_set_combo_enabled(req[0]);
			core_unlock();
			return rc;
		}

	case OP_CONFIG_RELOAD: {
		u16 rc;
		core_lock();
		rc = (u16)config_load();
#if QWARK_PREVIEW
		/* Revision 1.24: the profile was read again with it; its auto flags follow. */
		if (config_profile_active()) session_profile_changed();
#endif
		core_unlock();
		return rc == ST_NOT_FOUND ? ST_OK : rc;
	}

	case OP_CONFIG_SAVE: {
		u16 rc;
		core_lock();
		rc = (u16)config_save();
		core_unlock();
		return rc;
	}

	/*
	 * Revision 1.23. `menu.help`, a config write and nothing else, exactly like
	 * COMBO_ENABLE: it answers whatever the session state is, and the tick
	 * thread puts the Help row's words in or back at the next moment it is safe
	 * to (menu.c). Where code cannot be patched there is no menu to set.
	 */
	case OP_MENU_HELP:
		if (reqlen < 1 || req[0] > 1) return ST_BAD_ARG;
		if (!plat_can_patch_code()) return ST_UNSUPPORTED;
		{
			u16 rc;
			core_lock();
			rc = (u16)config_set_menu_help(req[0]);
			core_unlock();
			return rc;
		}

#ifndef QWARK_EMULATOR
	/*
	 * Revision 1.18. The three RPCS3 patch ops exist only in the emulator
	 * builds. A console patches code itself, so there is nothing for a client
	 * to write into a patch file here, and a client only ever asks under its
	 * RPCS3 target; they are known ops all the same, so UNSUPPORTED rather than
	 * UNKNOWN_OP.
	 */
	case OP_SWITCH_PATCH:
	case OP_MOD_PATCH:
	case OP_SAVEFILE_PATCH:
		return ST_UNSUPPORTED;
#endif

	/*
	 * Revision 1.27. The Lua helper as an RPCS3 patch, in the emulator builds
	 * alone, like the three above; and nowhere in a release, which has no Lua
	 * game: there LUA_EXEC and LUA_STATUS are UNSUPPORTED here too.
	 */
#if QWARK_HAS_TOD && defined(QWARK_EMULATOR)
	case OP_LUA_PATCH: {
		u16 rc;

		core_lock();
		rc = (u16)lua_core_patch(reply, replycap, replylen);
		core_unlock();
		return rc;
	}
#else
	case OP_LUA_PATCH:
#endif
#if !QWARK_HAS_TOD
	case OP_LUA_EXEC:
	case OP_LUA_STATUS:
#endif
#if !(QWARK_HAS_TOD && defined(QWARK_EMULATOR))
		return ST_UNSUPPORTED;
#endif

	default:
		return ST_UNKNOWN_OP;
	}
}

/* ---------------------------------------------------------- the connection */

/* The arena, back to whoever is waiting. Safe to call when this connection has none. */
static void release_buffers(struct conn *c)
{
	plat_mutex_lock(&g_arena_mutex);
	if (c->has_arena) {
		c->has_arena = 0;
		g_arena_held = 0;
	}
	c->req = NULL;
	c->reply = NULL;
	plat_mutex_unlock(&g_arena_mutex);
}

/* 1 when this connection now owns the arena, 0 when somebody else still does. */
static int take_arena(struct conn *c)
{
	int got = 0;

	plat_mutex_lock(&g_arena_mutex);
	if (!g_arena_held) {
		g_arena_held = 1;
		g_arena_takes++;
		c->has_arena = 1;
		c->req = g_arena;
		c->reply = g_arena;
		got = 1;
	}
	plat_mutex_unlock(&g_arena_mutex);
	return got;
}

static int control_request(u16 op, u32 len)
{
	if (len > 2) return 0;
	switch (op) {
	case OP_HELLO:
	case OP_HEARTBEAT:
	case OP_SUBSCRIBE:
	case OP_UNSUBSCRIBE:
	case OP_GET_STATE:
	case OP_VSH_MEMORY:   /* build 54: launches are when it matters most */
	case OP_GAME_MODULE:  /* revision 1.26: and so is a game module's gate */
		return 1;
	default: return 0;
	}
}

/*
 * Reads and discards a payload there was nowhere to put, so the next header is
 * read from where the next header actually starts.
 */
static int drain(struct conn *c, u32 len)
{
	char scratch[256];

	while (len > 0) {
		u32 chunk = len > sizeof(scratch) ? (u32)sizeof(scratch) : len;
		if (recv_all(c, scratch, chunk) != 0) return -1;
		len -= chunk;
	}
	return 0;
}

static void conn_thread(void *arg)
{
	int slot = (int)(size_t)arg;
	struct conn *c = &g_conns[slot];
	int sock = c->sock;
	int flag = 1;

	if (sock < 0) {
		plat_thread_exit();
		return;
	}

	if (conn_activity(c, NULL) == 0) {
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
		if (plat_socket_nonblocking(sock) != 0) c->cancelled = 1;
		session_activity_unlock();
		plat_log("qwark: client connected on slot %d", slot);
	}

	while (g_working && !c->cancelled && c->sock >= 0) {
		u8 header[QWARK_FRAME_HEADER];
		u32 length;
		u16 seq;
		u16 op;
		u16 status;
		u32 replylen = 0;
		u32 replycap;

		/* An idle header holds no arena. Payloads and replies have a deadline. */
		if (conn_io(c, header, QWARK_FRAME_HEADER, 0, 1) != 0) break;

		length = be32_get(header);
		seq    = be16_get(header + 4);
		op     = be16_get(header + 6);

		if (length > QWARK_MAX_PAYLOAD) {
			/* Unrecoverable: we cannot drain what we cannot buffer. */
			plat_log("qwark: oversize frame (%d), closing", (int)length);
			break;
		}

		status = ST_OK;
		if (control_request(op, length)) {
			c->req = c->control_req;
			c->reply = c->control_reply;
			replycap = sizeof(c->control_reply);
		} else {
			/*
			 * The gate and the arena. The boot check is taken under the
			 * transition's lock, exactly as the allocation used to be; the wait
			 * for the arena is not, because another connection may hold it for
			 * as long as a ring command takes and the tick thread wants that
			 * lock every tick. A connection waiting here holds nothing and is
			 * still cancelled by a launch, which is what closes it.
			 */
			for (;;) {
				int booting;

				if (conn_activity(c, NULL) != 0) break;
				core_lock();
				booting = g_booting;
				core_unlock();
				session_activity_unlock();

				if (booting) { status = ST_BUSY; break; }
				if (take_arena(c)) break;
				plat_sleep_us(ARENA_WAIT_US);
			}
			if (!c->has_arena && status == ST_OK) break;   /* cancelled while waiting */
			if (status != ST_OK) {
				/* Consume the frame without the arena; keep framing and liveness. */
				if (drain(c, length) != 0) break;
				subs_refresh(slot);
				be32_put(header, 0);
				be16_put(header + 4, seq);
				be16_put(header + 6, status);
				if (send_all(c, header, QWARK_FRAME_HEADER) != 0) break;
				continue;
			}
			/*
			 * Build 55: one arena. A request that fits the connection's own
			 * buffer is read into it and the arena takes the reply; a larger
			 * one is read into the arena and answered from the connection's
			 * buffer. The two never overlap, whatever a handler does.
			 */
			if (length <= CONN_SMALL) {
				c->req = c->control_reply;
				c->reply = g_arena;
				replycap = CONN_REPLY_CAP;
			} else {
				c->req = g_arena;
				c->reply = c->control_reply;
				replycap = CONN_SMALL;
			}
		}

		if (length > 0 && recv_all(c, c->req, length) != 0) {
			release_buffers(c);
			break;
		}

		/*
		 * The gate. A frame is dispatched only outside the quiet second and
		 * only while no transition has cancelled this connection, and the
		 * activity lock covers exactly that decision. It is released before
		 * anything slow happens on this thread, the handler above all. The tick
		 * thread takes the same lock every tick, so anything held under it is
		 * a stall of the freezes, the combos and the telemetry. A quiet second
		 * that begins while a handler is running still holds its reply, since
		 * the send goes through the same gate.
		 */
		if (conn_activity(c, NULL) != 0) {
			release_buffers(c);
			break;
		}
		subs_refresh(slot);
		core_lock();
		status = c->cancelled || (c->has_arena && g_booting) ? ST_BUSY : ST_OK;
		core_unlock();
		session_activity_unlock();
		if (status != ST_OK) {
			/* The transition has cancelled this connection. */
			release_buffers(c);
			break;
		}

#if QWARK_HAS_TOD
		if (op == OP_LUA_EXEC) {
			status = lua_exec(c->req, length, c->reply, replycap, &replylen, &c->cancelled);
			if (c->cancelled) {
				release_buffers(c);
				break;
			}
		} else
#endif
		if (op_needs_ring(op)) {
			struct ring_cmd cmd;

			memset(&cmd, 0, sizeof(cmd));
			cmd.op = op;
			cmd.req = c->req;
			cmd.reqlen = length;
			cmd.reply = c->reply;
			cmd.replycap = replycap;
			cmd.user = c;

			session_submit(&cmd);
			status = cmd.status;
			replylen = cmd.replylen;
			if (c->cancelled) {
				release_buffers(c);
				break;
			}
		} else {
			status = handle_inline(c, slot, op, c->req, length,
			                       c->reply, replycap, &replylen);
		}

		if (status != ST_OK) replylen = 0;

		be32_put(header, replylen);
		be16_put(header + 4, seq);
		be16_put(header + 6, status);

		if (send_all(c, header, QWARK_FRAME_HEADER) != 0) {
			release_buffers(c);
			break;
		}
		if (replylen > 0 && send_all(c, c->reply, replylen) != 0) {
			release_buffers(c);
			break;
		}

		release_buffers(c);
	}

	subs_drop_conn(slot);
	file_release_conn(slot);

	close_tracked(&c->sock);

	release_buffers(c);
	c->used = 0;

	plat_log("qwark: client on slot %d gone", slot);
	plat_thread_exit();
}

/* -------------------------------------------------------------- the server */

/*
 * The command port. QWARK_PORT on the console and in the simulator, and only a
 * PC build ever moves it: two emulator sessions on one PC need two ports, the
 * test suites want one nobody else holds, and 9673 is the one the client looks
 * for first.
 */
static u16 g_port = QWARK_PORT;

void net_set_port(u16 port)
{
	if (port != 0) g_port = port;
}

u16 net_port(void)
{
	return g_port;
}

#ifdef QWARK_HOST
/*
 * Where the listener binds, on the PC only. The console binds every interface
 * because the client is on another machine; a PC build that did the same raised
 * a Windows Firewall prompt the first time each executable ran from each path,
 * and left another rule behind for every Allow - and the simulator, the RPCS3
 * helper and the test suite are all rebuilt in throwaway directories. Loopback
 * is never asked about, so that is the default, and --bind moves it for somebody
 * who really wants the simulator reachable from another machine.
 */
static u32 g_bind = 0x7F000001u;   /* 127.0.0.1, network order below */

int net_set_bind(const char *addr)
{
	unsigned long v;

	if (addr == NULL || addr[0] == 0) return 0;

	v = (unsigned long)inet_addr(addr);
	if (v == INADDR_NONE) return 0;   /* not a dotted quad */

	g_bind = ntohl((u32)v);
	return 1;
}
#endif

static int open_listener(void)
{
	int s;
	struct sockaddr_in sa;
#ifndef QWARK_HOST
	int reuse = 1;
#endif

	s = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) return -1;

#ifndef QWARK_HOST
	/*
	 * The console restarts fast after a crash and must not be told its own
	 * port is still in use. The PC builds deliberately go without: two qwarks
	 * on one port there is the bug (the tests silently talked to whichever
	 * qwark-rpcs3.exe was already running), so let the second bind fail and say so.
	 */
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#endif

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(g_port);
#ifdef QWARK_HOST
	sa.sin_addr.s_addr = htonl(g_bind);
#else
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
#endif

	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		plat_socket_close(s);
		return -1;
	}

	if (listen(s, 8) < 0) {
		plat_socket_close(s);
		return -1;
	}

	return s;
}

int net_init(void)
{
	plat_mutex_init(&g_net_mutex);
	plat_mutex_init(&g_file_mutex);
	plat_mutex_init(&g_arena_mutex);
	g_arena_held = 0;

	memset(g_conns, 0, sizeof(g_conns));
	memset(g_subs, 0, sizeof(g_subs));
	memset(g_files, 0, sizeof(g_files));

	{
		int i;
		for (i = 0; i < QWARK_MAX_CLIENTS; i++) g_conns[i].sock = -1;
	}

	/*
	 * A plugin named in boot_plugins.txt starts before the VSH has brought its
	 * network stack up, and every socket() fails until it has. This used to
	 * return on the first failure, which left the module resident with its
	 * boot thread gone: a qwark.sprx installed for boot was loaded and dead.
	 * So wait for the stack the way the accept loop below waits to listen,
	 * half a second at a time, for as long as the module is alive. A load
	 * through webMAN comes long after the stack is up and never waits.
	 */
	{
		u32 waited_ms = 0;

		for (;;) {
			g_udp = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (g_udp >= 0) break;
			if (!g_working) {
				plat_trace("qwark:   udp socket create FAILED and the module is stopping");
				return ST_IO_ERROR;
			}
			if (waited_ms == 0) {
				plat_trace("qwark:   no network yet, waiting for it");
			}
			plat_sleep_us(NET_WAIT_STEP_US);
			waited_ms += NET_WAIT_STEP_US / 1000u;
		}

		/*
		 * To the log file as well as the TTY, since a console that boots the
		 * module has no TTY attached: this line is the proof that a boot load
		 * came up, and how long the network kept it waiting.
		 */
		if (waited_ms != 0) plat_log("qwark: the network came up after %u ms", waited_ms);
	}
	plat_trace("qwark:   udp socket ok");

	session_set_ring_exec(net_ring_exec);
	return ST_OK;
}

void net_stop(void)
{
	int i;

	plat_trace("qwark:   net_stop: breaking sockets");
	g_working = 0;

	/*
	 * A client socket is only shut down here, never closed: that is enough to
	 * bring its thread back out of recv(), and leaving the close to the thread
	 * that owns the descriptor means two threads can never close the same fd
	 * (and take out an unrelated descriptor that got the same number in
	 * between). net_wait_clients closes anything still open at the end.
	 */
	for (i = 0; i < QWARK_MAX_CLIENTS; i++) {
		int fd = g_conns[i].sock;
		if (fd >= 0) plat_socket_shutdown(fd);
	}

	/* The listener has to go, though: closing it is what wakes accept(). */
	close_tracked(&g_listen);
	close_tracked(&g_udp);
	plat_trace("qwark:   net_stop: sockets broken");
}

int net_wait_clients(u32 timeout_us)
{
	u32 waited = 0;
	int i;

	for (;;) {
		int busy = 0;

		for (i = 0; i < QWARK_MAX_CLIENTS; i++) {
			if (g_conns[i].used) { busy = 1; break; }
		}
		if (!busy) {
			plat_trace("qwark:   net_wait_clients: all clients gone");
			return 1;
		}
		if (waited >= timeout_us) break;

		plat_sleep_us(10000);
		waited += 10000;
	}

	/*
	 * A thread still in there is wedged in the network stack. The module is
	 * going away regardless, so take its descriptor back and carry on.
	 */
	plat_trace("qwark:   net_wait_clients: TIMED OUT, forcing sockets closed");
	for (i = 0; i < QWARK_MAX_CLIENTS; i++) close_tracked(&g_conns[i].sock);
	return 0;
}

void net_shutdown(void)
{
	plat_mutex_destroy(&g_net_mutex);
	plat_mutex_destroy(&g_file_mutex);
	plat_mutex_destroy(&g_arena_mutex);
	plat_trace("qwark:   net mutexes destroyed");
}

void net_accept_thread(void *arg)
{
#ifdef QWARK_HOST
	int complained = 0;
#endif

	(void)arg;

	while (g_working) {
		int listener = open_listener();
		if (listener < 0) {
			if (!g_working) break;
#ifdef QWARK_HOST
			/*
			 * On the PC a refused bind means somebody else holds the port, not
			 * that the network is still coming up, and with no SO_REUSEADDR the
			 * retry below will never win. Say it once instead of spinning in
			 * silence: this is exactly the case that used to end with two
			 * qwarks and a client talking to the wrong one.
			 */
			if (!complained) {
				complained = 1;
				plat_log("qwark: cannot listen on %d, something else has it",
				         (int)g_port);
			}
#endif
			plat_yield();
			plat_sleep_us(250000);
			continue;
		}

		g_listen = listener;
		plat_log("qwark: listening on %d", (int)g_port);

		/*
		 * A breath before the XMB is spoken to. PS3HEN reads boot_plugins.txt
		 * with the XMB already up and starts the plugins one after another,
		 * waiting on each start entry, so a module's first moments are spent
		 * inside the loader's own sequence. A user found the XMB hung when
		 * qwark.sprx was listed after webMAN and not when it was listed before,
		 * which points at exactly that window; the pause keeps our notification
		 * call off the loader's heels rather than racing it. It costs one
		 * delayed message per listener open and nothing else: accepting
		 * connections is what happens next, and is unaffected.
		 *
		 * The host backend really sleeps, so BOOT_NOTIFY_DELAY_US is 0 there
		 * and the unit and smoke suites do not wait on this.
		 */
		if (BOOT_NOTIFY_DELAY_US != 0) plat_sleep_us(BOOT_NOTIFY_DELAY_US);
		if (g_working) plat_notify("qwark loaded and listening");

		while (g_working) {
			struct sockaddr_in peer;
			socklen_t peerlen = sizeof(peer);
			int fd;
			int slot = -1;
			int i;

			memset(&peer, 0, sizeof(peer));
			fd = (int)accept(g_listen, (struct sockaddr *)&peer, &peerlen);
			if (fd < 0) {
				if (!g_working) break;
				if (plat_net_would_retry(plat_net_errno())) continue;
				break;
			}

			if (!g_working) { plat_socket_close(fd); break; }

			session_activity_lock();
			core_lock();
			if (g_booting) {
				core_unlock();
				session_activity_unlock();
				plat_socket_close(fd);
				continue;
			}
			plat_mutex_lock(&g_net_mutex);
			for (i = 0; i < QWARK_MAX_CLIENTS; i++) {
				if (!g_conns[i].used) { slot = i; g_conns[i].used = 1; break; }
			}
			plat_mutex_unlock(&g_net_mutex);

			if (slot < 0) {
				core_unlock();
				session_activity_unlock();
				plat_log("qwark: too many clients");
				plat_socket_close(fd);
				continue;
			}

			/*
			 * No buffers yet: a connection holds none while it waits. See the
			 * request loop in conn_thread.
			 */
			g_conns[slot].cancelled = 0;
			g_conns[slot].has_arena = 0;
			g_conns[slot].req = NULL;
			g_conns[slot].reply = NULL;
			g_conns[slot].remote_ip = peer.sin_addr.s_addr;
			g_conns[slot].sock = fd;

			/*
			 * Detached, like Ratchetron's client threads: nothing joins one,
			 * so a client that never comes back cannot hold up the unload.
			 */
			if (plat_thread_create_detached(conn_thread, (void *)(size_t)slot,
			                                CLIENT_STACK, "qwark_cli") != 0) {
				close_tracked(&g_conns[slot].sock);
				g_conns[slot].used = 0;
			}
			core_unlock();
			session_activity_unlock();
		}

		close_tracked(&g_listen);

		if (!g_working) break;

		plat_yield();
		plat_sleep_us(250000);
	}

	close_tracked(&g_listen);
	plat_log("qwark: accept thread down");
	plat_trace("qwark:   accept loop returning");
	plat_thread_exit();
}
