/*
 * RPCS3 over PINE: the game half of the platform for qwark-rpcs3.exe.
 *
 * PINE is RPCS3's IPC server (3rdparty/pine/pine_server.h, enabled from
 * Settings -> I/O -> "Enable IPC server"). One request packet is
 *
 *     u32 total_size (little-endian, counts these four bytes)
 *     { u8 opcode, args... } one or more, back to back
 *
 * and one reply packet is
 *
 *     u32 total_size (little-endian, counts these four bytes)
 *     u8  result      0x00 OK, 0xFF FAIL
 *     data            each command's result, concatenated in request order
 *
 * A command that fails aborts the whole packet: RPCS3 answers a bare five-byte
 * FAIL and drops whatever the earlier commands produced. So a batch is all or
 * nothing, which is what plat_mem_read wants anyway.
 *
 * BYTE ORDER, the thing that is easy to get backwards. RPCS3 decodes the word
 * before it sends it: pine_server.h does `const u32 res = Impl::read32(a);`,
 * where read32 returns a `be_t<u32>` straight out of guest memory, and that
 * conversion applies the byte swap. The u32 then goes onto the wire in the
 * host's own order, little-endian. So the wire carries the LOGICAL value, and
 * the PS3's big-endian bytes only exist inside the emulator. qwark's core wants
 * the bytes, so pine_read rebuilds them with be64_put / be32_put and pine_write
 * takes them apart with be64_get. Get this wrong and every word in the trainer
 * is byte-reversed.
 *
 * Transport: on Windows PINE listens on a TCP socket bound to 127.0.0.1 at the
 * configured port (28012 by default). On Linux and macOS it is a Unix socket at
 * $XDG_RUNTIME_DIR/rpcs3.sock (or $TMPDIR/rpcs3.sock, falling back to
 * /tmp/rpcs3.sock), with ".<slot>" appended when the port is not the default.
 * Only the Windows path is implemented here; pine_open_socket() below is the
 * single place that has to learn about AF_UNIX for the other two.
 */
#include "../plat.h"
#include "../plat_net.h"
#include "plat_host.h"
#include "backend_pine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * How much memory one packet carries. PINE's own limits are MAX_IPC_SIZE
 * 650000 for the request and MAX_IPC_RETURN_SIZE 450000 for the reply; a
 * 16 KB block costs 5 bytes of request per 8 bytes read, so 10245 bytes of
 * request and 16389 of reply. Four packets cover the 64 KB the core can ask
 * for in one call and every buffer here stays small enough to be static.
 */
#define PINE_CHUNK      16384u
/* A write is the fat one: 13 bytes of request per eight bytes written, plus up
 * to seven single-byte writes at 6 bytes each for a tail. */
#define PINE_REQ_MAX    (4u + (PINE_CHUNK / 8u) * 13u + 8u * 6u + 16u)
#define PINE_REP_MAX    (5u + PINE_CHUNK + 256u)

/* Long enough that a stuttering emulator is not a disconnect, short enough
 * that a dead one does not wedge the tick thread. */
#define PINE_TIMEOUT_MS 2000

/* A failed connect is retried at most this often. */
#define PINE_RETRY_US   1000000u

static int  g_port = PINE_DEFAULT_PORT;
static int  g_sock = -1;
static u64  g_last_try_us;
static int  g_ever_connected;

/* g_lock covers the socket and the two packet buffers; g_poll_lock covers the
 * cached console state below, which is read from more than one thread. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_poll_lock = PTHREAD_MUTEX_INITIALIZER;

/* The cached console state, refreshed at most every PINE_POLL_US. */
static u64  g_poll_us;
static int  g_have_poll;
static int  g_running;
static char g_title[16];

static u8 g_req[PINE_REQ_MAX];
static u8 g_rep[PINE_REP_MAX];

/* ------------------------------------------------------- little-endian bytes */

static void le32_put(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static u32 le32_get(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void le64_put(u8 *p, u64 v)
{
	le32_put(p, (u32)v);
	le32_put(p + 4, (u32)(v >> 32));
}

static u64 le64_get(const u8 *p)
{
	return (u64)le32_get(p) | ((u64)le32_get(p + 4) << 32);
}

/* ------------------------------------------------------------- the connection */

void pine_set_port(int port)
{
	if (port > 0 && port < 65536) g_port = port;
}

int pine_port(void)
{
	return g_port;
}

int pine_connected(void)
{
	return g_sock >= 0;
}

/*
 * Called with g_lock held. The cached console state is left alone on purpose:
 * the next poll goes out over a dead socket, fails, and clears it under its own
 * lock, which keeps the two locks from ever having to nest.
 */
static void pine_drop(const char *why)
{
	if (g_sock < 0) return;

	plat_socket_close(g_sock);
	g_sock = -1;

	plat_log("pine: lost (%s)", why != NULL ? why : "closed");
}

void pine_close(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_sock >= 0) {
		plat_socket_close(g_sock);
		g_sock = -1;
	}
	pthread_mutex_unlock(&g_lock);

	pine_forget_cache();
}

/*
 * The one place that knows what kind of socket PINE is. Windows: TCP to
 * 127.0.0.1:<port>. Linux and macOS: an AF_UNIX stream socket at the path in
 * the file header, which is the only thing that has to change here.
 */
static int pine_open_socket(void)
{
	struct sockaddr_in addr;
	int s;
	int one = 1;

	s = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)g_port);
	addr.sin_addr.s_addr = htonl(0x7F000001u);   /* 127.0.0.1 */

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		plat_socket_close(s);
		return -1;
	}

	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

#ifdef _WIN32
	{
		DWORD ms = PINE_TIMEOUT_MS;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = PINE_TIMEOUT_MS / 1000;
		tv.tv_usec = (PINE_TIMEOUT_MS % 1000) * 1000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
#endif

	return s;
}

int pine_ensure(void)
{
	u64 now;

	if (g_sock >= 0) return 1;

	now = plat_time_us();
	if (g_ever_connected || g_last_try_us != 0) {
		if (now - g_last_try_us < PINE_RETRY_US) return 0;
	}
	g_last_try_us = now;

	g_sock = pine_open_socket();
	if (g_sock < 0) return 0;

	g_ever_connected = 1;
	plat_log("pine: connected to 127.0.0.1:%d", g_port);
	return 1;
}

void pine_startup(void)
{
	pthread_mutex_lock(&g_lock);
	if (!pine_ensure())
		plat_log("pine: no server on 127.0.0.1:%d yet, retrying every second",
		         g_port);
	pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------- transactions */

static int pine_send_all(const u8 *buf, u32 len)
{
	u32 done = 0;

	while (done < len) {
		int n = (int)send(g_sock, (const char *)buf + done, (int)(len - done), 0);
		if (n <= 0) {
			if (n < 0 && plat_net_would_retry(plat_net_errno())) continue;
			return -1;
		}
		done += (u32)n;
	}
	return 0;
}

static int pine_recv_all(u8 *buf, u32 len)
{
	u32 done = 0;

	while (done < len) {
		int n = (int)recv(g_sock, (char *)buf + done, (int)(len - done), 0);
		if (n <= 0) {
			if (n < 0 && plat_net_would_retry(plat_net_errno())) continue;
			return -1;
		}
		done += (u32)n;
	}
	return 0;
}

/*
 * Sends `reqlen` bytes of commands (without the size header, which is written
 * here) and returns the reply's data length, or negative. The data itself lands
 * in g_rep + 5. Called with g_lock held.
 */
static int pine_call(u32 reqlen)
{
	u32 total;
	u32 size;

	if (!pine_ensure()) return -1;
	if (reqlen == 0 || reqlen + 4u > sizeof(g_req)) return -1;

	le32_put(g_req, reqlen + 4u);

	if (pine_send_all(g_req, reqlen + 4u) != 0) {
		pine_drop("send failed");
		return -1;
	}

	if (pine_recv_all(g_rep, 5) != 0) {
		pine_drop("no reply");
		return -1;
	}

	total = le32_get(g_rep);
	if (total < 5u || total > sizeof(g_rep)) {
		pine_drop("bad reply size");
		return -1;
	}

	size = total - 5u;
	if (size > 0 && pine_recv_all(g_rep + 5, size) != 0) {
		pine_drop("short reply");
		return -1;
	}

	/*
	 * A FAIL is RPCS3 saying no - an unmapped address, most often - not the
	 * connection going away, so the socket stays open and the caller gets an
	 * error exactly as PS3MAPI would have given one.
	 */
	if (g_rep[4] != PINE_OK) return -1;

	return (int)size;
}

/* ------------------------------------------------------------------ memory */

/*
 * One packet: an MsgRead64 per whole eight bytes and an MsgRead8 for whatever
 * is left. Fewer round trips is the whole point; a 16 KB block is 2048 commands
 * in one packet and one reply.
 */
static int pine_read_chunk(u32 addr, u8 *out, u32 len)
{
	u32 req = 0;
	u32 done = 0;
	u32 got;
	int size;

	while (done + 8u <= len) {
		g_req[4 + req] = PINE_MSG_READ64;
		le32_put(g_req + 4 + req + 1, addr + done);
		req += 5;
		done += 8;
	}
	while (done < len) {
		g_req[4 + req] = PINE_MSG_READ8;
		le32_put(g_req + 4 + req + 1, addr + done);
		req += 5;
		done += 1;
	}

	size = pine_call(req);
	if (size < 0 || (u32)size != len) return -1;

	/* The reply is the values, in order: eight bytes each, then single bytes. */
	done = 0;
	got = 0;
	while (done + 8u <= len) {
		be64_put(out + done, le64_get(g_rep + 5 + got));
		done += 8;
		got += 8;
	}
	while (done < len) {
		out[done] = g_rep[5 + got];
		done += 1;
		got += 1;
	}

	return 0;
}

static int pine_write_chunk(u32 addr, const u8 *in, u32 len)
{
	u32 req = 0;
	u32 done = 0;

	while (done + 8u <= len) {
		g_req[4 + req] = PINE_MSG_WRITE64;
		le32_put(g_req + 4 + req + 1, addr + done);
		le64_put(g_req + 4 + req + 5, be64_get(in + done));
		req += 13;
		done += 8;
	}
	while (done < len) {
		g_req[4 + req] = PINE_MSG_WRITE8;
		le32_put(g_req + 4 + req + 1, addr + done);
		g_req[4 + req + 5] = in[done];
		req += 6;
		done += 1;
	}

	return pine_call(req) < 0 ? -1 : 0;
}

int pine_read(u32 addr, void *buf, u32 len)
{
	u8 *out = (u8 *)buf;
	u32 done = 0;
	int rc = 0;

	if (len == 0 || len > PLAT_MEM_MAX) return -1;

	pthread_mutex_lock(&g_lock);
	while (done < len) {
		u32 chunk = len - done;
		if (chunk > PINE_CHUNK) chunk = PINE_CHUNK;
		if (pine_read_chunk(addr + done, out + done, chunk) != 0) { rc = -1; break; }
		done += chunk;
	}
	pthread_mutex_unlock(&g_lock);

	return rc;
}

int pine_write(u32 addr, const void *buf, u32 len)
{
	const u8 *in = (const u8 *)buf;
	u32 done = 0;
	int rc = 0;

	if (len == 0 || len > PLAT_MEM_MAX) return -1;

	pthread_mutex_lock(&g_lock);
	while (done < len) {
		u32 chunk = len - done;
		if (chunk > PINE_CHUNK) chunk = PINE_CHUNK;
		if (pine_write_chunk(addr + done, in + done, chunk) != 0) { rc = -1; break; }
		done += chunk;
	}
	pthread_mutex_unlock(&g_lock);

	return rc;
}

/* ------------------------------------------------------- status and strings */

/* `u32 length including the NUL, bytes, NUL`, at `g_rep + 5 + off`. */
static int pine_take_string(u32 size, u32 *off, char *out, u32 cap)
{
	u32 n;
	u32 copy;

	if (cap == 0) return -1;
	if (*off + 4u > size) return -1;
	n = le32_get(g_rep + 5 + *off);
	if (n == 0 || *off + 4u + n > size) return -1;

	copy = n - 1u;                 /* the length counts the NUL */
	if (copy > cap - 1u) copy = cap - 1u;
	memcpy(out, g_rep + 5 + *off + 4u, copy);
	out[copy] = 0;

	*off += 4u + n;
	return 0;
}

/*
 * MsgStatus and MsgID in one packet.
 *
 * The four zero bytes after the MsgStatus opcode are not an argument: PINE's
 * parser advances its cursor by four for MsgStatus even though the command
 * takes none (pine_server.h, `buf_cnt += 4`), so anything batched behind it
 * would lose its first four bytes without this padding.
 */
int pine_status_and_id(u32 *status, char id[16])
{
	u32 req = 0;
	u32 off = 0;
	int size;
	int rc = 0;

	pthread_mutex_lock(&g_lock);

	g_req[4 + req++] = PINE_MSG_STATUS;
	le32_put(g_req + 4 + req, 0);
	req += 4;
	g_req[4 + req++] = PINE_MSG_ID;

	size = pine_call(req);
	if (size < 4) {
		rc = -1;
	} else {
		if (status != NULL) *status = le32_get(g_rep + 5);
		off = 4;
		if (id != NULL) {
			if (pine_take_string((u32)size, &off, id, 16) != 0) {
				id[0] = 0;
				rc = -1;
			}
		}
	}

	pthread_mutex_unlock(&g_lock);
	return rc;
}

int pine_status(u32 *status)
{
	int size;
	int rc = 0;

	pthread_mutex_lock(&g_lock);

	g_req[4] = PINE_MSG_STATUS;
	le32_put(g_req + 5, 0);

	size = pine_call(5);
	if (size < 4) rc = -1;
	else if (status != NULL) *status = le32_get(g_rep + 5);

	pthread_mutex_unlock(&g_lock);
	return rc;
}

static int pine_string_cmd(u8 opcode, char *out, u32 cap)
{
	u32 off = 0;
	int size;
	int rc = 0;

	if (cap == 0) return -1;
	out[0] = 0;

	pthread_mutex_lock(&g_lock);

	g_req[4] = opcode;
	size = pine_call(1);
	if (size < 0 || pine_take_string((u32)size, &off, out, cap) != 0) rc = -1;

	pthread_mutex_unlock(&g_lock);
	return rc;
}

int pine_title_id(char id[16])
{
	return pine_string_cmd(PINE_MSG_ID, id, 16);
}

int pine_title(char *out, u32 cap)
{
	return pine_string_cmd(PINE_MSG_TITLE, out, cap);
}

int pine_version(char *out, u32 cap)
{
	return pine_string_cmd(PINE_MSG_VERSION, out, cap);
}

/* --------------------------------------------------------- the cached state */

void pine_forget_cache(void)
{
	pthread_mutex_lock(&g_poll_lock);
	g_have_poll = 0;
	g_poll_us = 0;
	g_running = 0;
	g_title[0] = 0;
	pthread_mutex_unlock(&g_poll_lock);
}

/*
 * The tick thread asks whether a game is running 120 times a second. One packet
 * every 50 ms answers all of them; the real socket traffic is the hot block.
 */
static void pine_poll(void)
{
	u64 now = plat_time_us();
	u32 status = PINE_STATUS_SHUTDOWN;
	char id[16];
	int was;
	int running;

	pthread_mutex_lock(&g_poll_lock);
	if (g_have_poll && now - g_poll_us < PINE_POLL_US) {
		pthread_mutex_unlock(&g_poll_lock);
		return;
	}
	g_poll_us = now;
	g_have_poll = 1;
	was = g_running;
	pthread_mutex_unlock(&g_poll_lock);

	id[0] = 0;
	if (pine_status_and_id(&status, id) != 0) {
		running = 0;
		id[0] = 0;
	} else {
		/*
		 * "Running" alone is not enough: RPCS3 reports Running with no game
		 * booted too, and then MsgID is empty. Both have to hold.
		 */
		running = (status == PINE_STATUS_RUNNING && id[0] != 0) ? 1 : 0;
		if (!running) id[0] = 0;
	}

	pthread_mutex_lock(&g_poll_lock);
	g_running = running;
	snprintf(g_title, sizeof(g_title), "%s", id);
	pthread_mutex_unlock(&g_poll_lock);

	if (running && !was)      plat_log("pine: game %s running", id);
	else if (!running && was) plat_log("pine: game stopped");
}

int pine_game_running(void)
{
	pine_poll();
	return g_running;
}

/*
 * RPCS3 has no PS3MAPI process id and the core only ever compares it with
 * itself, so one constant while a game is up is exactly enough: it changes to 0
 * and back when the game goes away and returns, which is what drives the
 * session's reboot detection.
 */
u32 pine_game_pid(void)
{
	pine_poll();
	return g_running ? 1u : 0u;
}

int pine_game_title(char out[16])
{
	pine_poll();

	pthread_mutex_lock(&g_poll_lock);
	snprintf(out, 16, "%s", g_title);
	pthread_mutex_unlock(&g_poll_lock);

	return out[0] != 0;
}

/* ------------------------------------------------------------- the seam */

#ifndef QWARK_PINE_NO_PLAT

/* Set by rpcs3_main.c before plat_init, from --root. */
static char g_root_arg[1024];

void pine_set_root(const char *dir)
{
	snprintf(g_root_arg, sizeof(g_root_arg), "%s", dir != NULL ? dir : "");
}

int plat_init(void)
{
	if (host_common_init(g_root_arg, "qwark-rpcs3-root") != 0) return -1;
	pine_startup();
	return 0;
}

void plat_shutdown(void)
{
	pine_close();
	host_common_shutdown();
}

int plat_game_running(void) { return pine_game_running(); }
u32 plat_game_pid(void)     { return pine_game_pid(); }

int plat_game_title(char out[16]) { return pine_game_title(out); }

/* RPCS3 recompiles PPU code, so an instruction word written here changes
 * nothing the game executes. See plat.h. */
int plat_can_patch_code(void) { return 0; }
int plat_is_emulator(void)    { return 1; }

int plat_mem_read(u32 pid, u32 addr, void *buf, u32 len)
{
	(void)pid;
	return pine_read(addr, buf, len);
}

int plat_mem_write(u32 pid, u32 addr, const void *buf, u32 len)
{
	(void)pid;
	return pine_write(addr, buf, len);
}

/* PINE has no equivalent, and RPCS3 needs none: nothing here patches code. */
void plat_rsx_pause(int pause)
{
	(void)pause;
}

void plat_notify(const char *msg)
{
	char ts[16];
	host_stamp(ts, sizeof(ts));
	printf("[%s] notify: %s\n", ts, msg);
	fflush(stdout);
}

#endif /* QWARK_PINE_NO_PLAT */
