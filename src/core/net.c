#include "net.h"
#include "session.h"
#include "mem.h"
#include "features.h"
#include "mods.h"
#include "config.h"
#include "util.h"
#include "autosplit.h"
#include "../plat/plat.h"
#include "../plat/plat_net.h"
#include "../games/game.h"

#include <string.h>

#define CONN_REQ_CAP    QWARK_MAX_PAYLOAD
#define CONN_REPLY_CAP  65600u
#define CONN_ALLOC      (3u * 65536u)      /* 64 KB pages, holds both buffers */

#define CLIENT_STACK    16384u
#define ACCEPT_STACK    16384u

#define SUB_TIMEOUT_US  5000000u

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
	void *block;
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
};

static struct conn g_conns[QWARK_MAX_CLIENTS];
static struct sub  g_subs[QWARK_MAX_SUBS];
static struct filehandle g_files[MAX_FILES];

static int g_listen = -1;
static int g_udp = -1;
static volatile int g_working = 1;

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

static int send_all(int s, const void *buf, u32 len)
{
	const char *p = (const char *)buf;
	u32 sent = 0;

	while (sent < len) {
		int res = (int)send(s, p + sent, (int)(len - sent), 0);
		if (res < 0) {
			if (plat_net_would_retry(plat_net_errno())) continue;
			return -1;
		}
		if (res == 0) return -1;
		sent += (u32)res;
	}

	return 0;
}

static int recv_all(int s, void *buf, u32 len)
{
	char *p = (char *)buf;
	u32 got = 0;

	while (got < len) {
		int res = (int)recv(s, p + got, (int)(len - got), 0);
		if (res < 0) {
			if (plat_net_would_retry(plat_net_errno())) continue;
			return -1;
		}
		if (res == 0) return -1;
		got += (u32)res;
	}

	return 0;
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
		break;
	}

	plat_mutex_unlock(&g_net_mutex);
	return rc;
}

void net_send_telemetry(const u8 *packet, u32 len)
{
	u64 now;
	int i;

	if (g_udp < 0 || len == 0) return;

	now = plat_time_us();

	plat_mutex_lock(&g_net_mutex);
	for (i = 0; i < QWARK_MAX_SUBS; i++) {
		struct sockaddr_in sa;

		if (!g_subs[i].used) continue;

		if (now - g_subs[i].last_us > SUB_TIMEOUT_US) {
			memset(&g_subs[i], 0, sizeof(g_subs[i]));
			continue;
		}

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

static int file_alloc(int conn_slot, plat_file_t f, u32 *handle)
{
	int i;

	plat_mutex_lock(&g_file_mutex);
	for (i = 0; i < MAX_FILES; i++) {
		if (g_files[i].used) continue;
		g_files[i].used = 1;
		g_files[i].f = f;
		g_files[i].conn_slot = conn_slot;
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

static void put_fixed(u8 *dst, u32 cap, const char *src)
{
	u32 i = 0;
	memset(dst, 0, cap);
	if (src == NULL) return;
	while (i < cap && src[i] != 0) { dst[i] = (u8)src[i]; i++; }
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
		struct patch_word words[64];
		u16 n;
		u16 i;

		if (reqlen < 4) { cmd->status = ST_BAD_ARG; break; }
		n = be16_get(req);
		if (n == 0 || n > 64 || reqlen < 4u + (u32)n * 8u) { cmd->status = ST_BAD_ARG; break; }

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
		mem_clear_client();
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
		for (i = 0; i < ncat; i++) { put_fixed(reply + off, 24, cats[i]); off += 24; }

		/*
		 * Protocol 1.3: four UnlockFieldDesc, always four, naming and typing
		 * the four value slots so the client draws a checkbox or a number box
		 * per game instead of guessing from RaC1's labels.
		 */
		for (i = 0; i < 4; i++) {
			put_fixed(reply + off, UNLOCK_FIELD_NAME_LEN, fields[i].name);
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
			put_fixed(reply + off + 20, 24, list[i].name);
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

	default:
		cmd->status = ST_UNKNOWN_OP;
		break;
	}
}

static void net_ring_exec(struct ring_cmd *cmd)
{
	core_lock();
	ring_exec_locked(cmd);
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
		return 1;
	default:
		return 0;
	}
}

/* --------------------------------------------------- network-thread handlers */

static u16 handle_inline(struct conn *c, int slot, u16 op,
                         const u8 *req, u32 reqlen,
                         u8 *reply, u32 replycap, u32 *replylen)
{
	*replylen = 0;

	switch (op) {

	case OP_HELLO:
		if (reqlen < 1) return ST_BAD_ARG;
		*replylen = session_info_copy(reply, replycap);
		return *replylen ? ST_OK : ST_FULL;

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

	case OP_DESCRIBE: {
		u16 rc;
		/*
		 * The descriptor registry only becomes the running game's table at INGAME
		 * (features_set_game runs in enter_ingame). During BOOTING it still holds
		 * the previous game's table, so answer UNSUPPORTED until INGAME rather than
		 * hand a client a table for a game that is not the one now starting.
		 */
		if (session_game() == NULL || session_state() != SESSION_INGAME) return ST_UNSUPPORTED;
		core_lock();
		rc = (u16)features_describe(reply, replycap, replylen);
		core_unlock();
		return rc;
	}

	case OP_FEATURE_SET_AUTO: {
		u16 rc;
		if (reqlen < 2) return ST_BAD_ARG;
		if (session_game() == NULL) return ST_UNSUPPORTED;
		core_lock();
		rc = (u16)features_set_auto(req[0], req[1] != 0);
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
		if (session_game() == NULL) return ST_UNSUPPORTED;

		core_lock();
		rc = (u16)features_options(req[0], &options, &count);
		if (rc != ST_OK) { core_unlock(); return rc; }
		if (1u + (u32)count * 24u > replycap) { core_unlock(); return ST_FULL; }

		reply[0] = count;
		off = 1;
		for (i = 0; i < count; i++) { put_fixed(reply + off, 24, options[i]); off += 24; }
		core_unlock();

		*replylen = off;
		return ST_OK;
	}

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
		const struct game_api *g = session_game();
		const struct autosplit_desc *rows = NULL;
		u8 count = 0;
		u8 i;
		u32 off;

		/*
		 * Same gate as DESCRIBE: during BOOTING under BCES01503 session_game()
		 * is still only a guess, and handing a client the wrong game's split
		 * list is worse than making it wait.
		 */
		if (g == NULL || session_state() != SESSION_INGAME) return ST_UNSUPPORTED;
		if (g->autosplit_describe == NULL) return ST_UNSUPPORTED;

		rows = g->autosplit_describe(&count);
		if (rows == NULL || count == 0) return ST_UNSUPPORTED;
		if (1u + (u32)count * AUTOSPLIT_DESC_SIZE > replycap) return ST_FULL;

		reply[0] = count;
		off = 1;
		for (i = 0; i < count; i++) {
			reply[off + 0] = rows[i].code;
			reply[off + 1] = rows[i].kind;
			reply[off + 2] = rows[i].flags;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, rows[i].param_us);
			put_fixed(reply + off + 8, AUTOSPLIT_LABEL_LEN, rows[i].label);
			off += AUTOSPLIT_DESC_SIZE;
		}

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
			put_fixed(reply + off + 8, 32, d->name);
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
		config_set_selected_slot(req[0]);
		return ST_OK;

	case OP_POS_LIST: {
		const struct game_api *g = session_game();
		u8 planet;
		u8 s;
		u32 off;

		if (2u + QWARK_POS_SLOTS * 16u > replycap) return ST_FULL;

		core_lock();
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
		const struct game_api *g = session_game();
		const char * const *names;
		u8 count = 0;
		u8 i;
		u32 off;

		if (g == NULL || g->planet_names == NULL) return ST_UNSUPPORTED;

		names = g->planet_names(&count);
		if (names == NULL) return ST_UNSUPPORTED;
		if (1u + (u32)count * 24u > replycap) return ST_FULL;

		reply[0] = count;
		off = 1;
		for (i = 0; i < count; i++) { put_fixed(reply + off, 24, names[i]); off += 24; }
		*replylen = off;
		return ST_OK;
	}

	case OP_PLANET_SELECT:
		if (reqlen < 2) return ST_BAD_ARG;
		config_set_selected_planet(req[0], req[1]);
		return ST_OK;

	case OP_MOBY_TABLE: {
		const struct game_api *g = session_game();
		u32 tbl = 0, end = 0;
		u16 stride = 0;
		u16 rc;

		if (g == NULL || g->moby_table == NULL) return ST_UNSUPPORTED;
		if (replycap < 12) return ST_FULL;

		rc = (u16)g->moby_table(&tbl, &end, &stride);
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

		if (!file_alloc(slot, f, &handle)) {
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
		if (len > 65536) return ST_BAD_ARG;
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
		if (want > 65536) want = 65536;
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
		if (!payload_path(req, reqlen, 0, path, sizeof(path))) return ST_BAD_ARG;
		return plat_file_unlink(path) == 0 ? ST_OK : ST_NOT_FOUND;
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
		if (!payload_path(req, reqlen, 0, path, sizeof(path))) return ST_BAD_ARG;
		return (u16)rmdir_recursive(path, 0);
	}

	case OP_USER_ID:
		if (replycap < 4) return ST_FULL;
		be32_put(reply, plat_user_id());
		*replylen = 4;
		return ST_OK;

	case OP_COMBO_SET:
		if (reqlen < 8) return ST_BAD_ARG;
		if (req[0] >= COMBO_COUNT) return ST_BAD_ARG;
		return (u16)config_set_combo(req[0], be32_get(req + 4));

	case OP_COMBO_LIST: {
		u8 a;
		u32 off = 1;

		if (1u + COMBO_COUNT * 8u > replycap) return ST_FULL;

		reply[0] = COMBO_COUNT;
		for (a = 0; a < COMBO_COUNT; a++) {
			reply[off] = a;
			reply[off + 1] = 0;
			reply[off + 2] = 0;
			reply[off + 3] = 0;
			be32_put(reply + off + 4, config_combo(a));
			off += 8;
		}
		*replylen = off;
		return ST_OK;
	}

	case OP_CONFIG_RELOAD: {
		u16 rc;
		core_lock();
		rc = (u16)config_load();
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

	default:
		return ST_UNKNOWN_OP;
	}
}

/* ---------------------------------------------------------- the connection */

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

	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

	plat_log("qwark: client connected on slot %d", slot);

	while (g_working && c->sock >= 0) {
		u8 header[QWARK_FRAME_HEADER];
		u32 length;
		u16 seq;
		u16 op;
		u16 status;
		u32 replylen = 0;

		if (recv_all(c->sock, header, QWARK_FRAME_HEADER) != 0) break;

		length = be32_get(header);
		seq    = be16_get(header + 4);
		op     = be16_get(header + 6);

		if (length > QWARK_MAX_PAYLOAD) {
			/* Unrecoverable: we cannot drain what we cannot buffer. */
			plat_log("qwark: oversize frame (%d), closing", (int)length);
			break;
		}

		if (length > 0 && recv_all(c->sock, c->req, length) != 0) break;

		subs_refresh(slot);

		if (op_needs_ring(op)) {
			struct ring_cmd cmd;

			memset(&cmd, 0, sizeof(cmd));
			cmd.op = op;
			cmd.req = c->req;
			cmd.reqlen = length;
			cmd.reply = c->reply;
			cmd.replycap = CONN_REPLY_CAP;
			cmd.user = c;

			session_submit(&cmd);
			status = cmd.status;
			replylen = cmd.replylen;
		} else {
			status = handle_inline(c, slot, op, c->req, length,
			                       c->reply, CONN_REPLY_CAP, &replylen);
		}

		if (status != ST_OK) replylen = 0;

		be32_put(header, replylen);
		be16_put(header + 4, seq);
		be16_put(header + 6, status);

		if (send_all(c->sock, header, QWARK_FRAME_HEADER) != 0) break;
		if (replylen > 0 && send_all(c->sock, c->reply, replylen) != 0) break;
	}

	subs_drop_conn(slot);
	file_release_conn(slot);

	close_tracked(&c->sock);

	if (c->block != NULL) {
		plat_free_pages(c->block);
		c->block = NULL;
	}
	c->req = NULL;
	c->reply = NULL;
	c->used = 0;

	plat_log("qwark: client on slot %d gone", slot);
	plat_thread_exit();
}

/* -------------------------------------------------------------- the server */

static int open_listener(void)
{
	int s;
	int reuse = 1;
	struct sockaddr_in sa;

	s = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) return -1;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(QWARK_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);

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

	memset(g_conns, 0, sizeof(g_conns));
	memset(g_subs, 0, sizeof(g_subs));
	memset(g_files, 0, sizeof(g_files));

	{
		int i;
		for (i = 0; i < QWARK_MAX_CLIENTS; i++) g_conns[i].sock = -1;
	}

	g_udp = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_udp < 0) {
		plat_trace("qwark:   udp socket create FAILED");
		return ST_IO_ERROR;
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
	plat_trace("qwark:   net mutexes destroyed");
}

void net_accept_thread(void *arg)
{
	(void)arg;

	while (g_working) {
		int listener = open_listener();
		if (listener < 0) {
			if (!g_working) break;
			plat_yield();
			plat_sleep_us(250000);
			continue;
		}

		g_listen = listener;
		plat_log("qwark: listening on %d", QWARK_PORT);
		plat_notify("qwark loaded and listening");

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

			plat_mutex_lock(&g_net_mutex);
			for (i = 0; i < QWARK_MAX_CLIENTS; i++) {
				if (!g_conns[i].used) { slot = i; g_conns[i].used = 1; break; }
			}
			plat_mutex_unlock(&g_net_mutex);

			if (slot < 0) {
				plat_log("qwark: too many clients");
				plat_socket_close(fd);
				continue;
			}

			g_conns[slot].block = plat_alloc_pages(CONN_ALLOC);
			if (g_conns[slot].block == NULL) {
				plat_log("qwark: out of memory for a client buffer");
				plat_socket_close(fd);
				g_conns[slot].used = 0;
				continue;
			}

			g_conns[slot].req = (u8 *)g_conns[slot].block;
			g_conns[slot].reply = g_conns[slot].req + CONN_REQ_CAP;
			g_conns[slot].remote_ip = peer.sin_addr.s_addr;
			g_conns[slot].sock = fd;

			/*
			 * Detached, like Ratchetron's client threads: nothing joins one,
			 * so a client that never comes back cannot hold up the unload.
			 */
			if (plat_thread_create_detached(conn_thread, (void *)(size_t)slot,
			                                CLIENT_STACK, "qwark_cli") != 0) {
				close_tracked(&g_conns[slot].sock);
				plat_free_pages(g_conns[slot].block);
				g_conns[slot].block = NULL;
				g_conns[slot].used = 0;
			}
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
