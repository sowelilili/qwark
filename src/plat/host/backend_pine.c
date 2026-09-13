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
 * ONE CLIENT AT A TIME. pine_server.h accepts a connection and serves it until
 * it goes away; anyone else who connects meanwhile completes the TCP handshake
 * (the listen backlog is 4096) and then hears nothing at all until the first
 * client leaves. So a connect that succeeds proves nothing, and a request that
 * is never answered is the normal symptom of another program - a second copy
 * of this helper, say - sitting on RPCS3's IPC port.
 *
 * THREADS. The tick thread runs 120 times a second and is the only caller of
 * plat_mem_read and friends, so it must never sit in a blocking recv: a silent
 * or stalled emulator would otherwise starve the whole helper, and the PC client
 * with it. The socket therefore belongs to a worker thread. A caller hands it
 * one packet through the mailbox below and waits at most PINE_WAIT_MS for the
 * reply; past that the call returns PINE_BUSY and the caller goes on with its
 * life, while the worker keeps waiting for up to PINE_LINK_MS before it calls
 * the link dead. A reply that arrives after its caller gave up is thrown away,
 * never handed to the next request. Everything above this file sees one plain
 * synchronous API and, on a stall, a failed read.
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
#include <errno.h>
#include <time.h>
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

/*
 * Timing.
 *
 *   PINE_WAIT_MS          how long a caller waits for its reply before it gets
 *                         PINE_BUSY. A live RPCS3 on loopback answers a 16 KB
 *                         read in well under a millisecond, so this is only
 *                         ever reached when the emulator is stalled or silent.
 *   PINE_LINK_MS          how long the worker waits before a silent link is a
 *                         dead one. Long enough that a stuttering emulator is
 *                         not a disconnect.
 *   PINE_RETRY_MS         how often a refused connect is retried.
 *   PINE_SILENT_RETRY_MS  how long RPCS3 is left alone after it accepted a
 *                         connection and then never answered on it: that is
 *                         another client holding the server, and queueing up
 *                         behind it again at once only wastes a socket.
 *   PINE_CONNECT_MS       the cap on a connect. A closed local port takes
 *                         Windows a second or two of SYN retries otherwise.
 *
 * The tests shorten the first four through pine_set_timeouts().
 */
#define PINE_WAIT_MS          100
#define PINE_LINK_MS          5000
#define PINE_RETRY_MS         1000
#define PINE_SILENT_RETRY_MS  5000
#define PINE_CONNECT_MS       40

static int  g_port = PINE_DEFAULT_PORT;
static int  g_wait_ms = PINE_WAIT_MS;
static int  g_link_ms = PINE_LINK_MS;
static int  g_retry_ms = PINE_RETRY_MS;
static int  g_silent_retry_ms = PINE_SILENT_RETRY_MS;

/*
 * The socket and its bookkeeping belong to the worker thread. g_sock is read
 * by pine_connected() from other threads, which is fine for a status line;
 * the only cross-thread write is pine_close() taking it away, under g_mb_lock.
 */
static volatile int g_sock = -1;
static u64  g_last_try_us;
static u64  g_retry_us;              /* wait this long before the next connect */
static int  g_ever_connected;
static int  g_announced;             /* "connected" logged for this socket */

/* g_lock serialises the callers and covers their packet buffers; g_poll_lock
 * covers the cached console state, which is read from more than one thread. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_poll_lock = PTHREAD_MUTEX_INITIALIZER;

/* The cached console state, refreshed at most every PINE_POLL_US. */
static u64  g_poll_us;
static int  g_have_poll;
static int  g_running;
static char g_title[16];

/* The callers' packet buffers, under g_lock. */
static u8 g_req[PINE_REQ_MAX];
static u8 g_rep[PINE_REP_MAX];

/*
 * The mailbox. One packet at a time goes from a caller to the worker and its
 * reply comes back the same way. The states:
 *
 *   IDLE       nothing posted
 *   PENDING    a caller posted g_wreq and is (or was) waiting
 *   DONE       the worker finished; the caller collects g_mb_result / g_wrep
 *   ABANDONED  the caller stopped waiting; the worker discards the reply and
 *              returns the mailbox to IDLE when it is done
 *
 * The worker reads nothing out of the callers' buffers: the caller copies its
 * packet into g_wreq under the lock before it posts, so a later caller that is
 * told PINE_BUSY can rebuild g_req freely without touching a packet the worker
 * may still be sending.
 */
enum { MB_IDLE, MB_PENDING, MB_DONE, MB_ABANDONED };

static pthread_mutex_t g_mb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_mb_posted = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_mb_finished = PTHREAD_COND_INITIALIZER;
static int  g_mb_state = MB_IDLE;
static u32  g_mb_len;                /* bytes in g_wreq; 0 means "just connect" */
static int  g_mb_result;
static u8   g_wreq[PINE_REQ_MAX];
static u8   g_wrep[PINE_REP_MAX];

static int       g_worker_up;
static int       g_worker_stop;
static pthread_t g_worker;

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

/* ------------------------------------------------------------- the settings */

void pine_set_port(int port)
{
	if (port > 0 && port < 65536) g_port = port;
}

int pine_port(void)
{
	return g_port;
}

void pine_set_timeouts(int wait_ms, int link_ms, int silent_retry_ms)
{
	g_wait_ms = wait_ms > 0 ? wait_ms : PINE_WAIT_MS;
	g_link_ms = link_ms > 0 ? link_ms : PINE_LINK_MS;
	g_silent_retry_ms = silent_retry_ms > 0 ? silent_retry_ms : PINE_SILENT_RETRY_MS;
	/* A refused connect is retried on the same clock as a silent one, scaled. */
	g_retry_ms = silent_retry_ms > 0 ? (silent_retry_ms + 4) / 5 : PINE_RETRY_MS;
	if (g_retry_ms < 1) g_retry_ms = 1;
}

int pine_connected(void)
{
	return g_sock >= 0;
}

/* --------------------------------------------------- the worker: the socket */

/*
 * connect() with a deadline: non-blocking connect, wait up to `ms` for it to
 * complete, then put the socket back into blocking mode for the transactions.
 * Returns 1 when connected.
 */
static int pine_connect_bounded(int s, const struct sockaddr *addr, int addrlen, int ms)
{
	fd_set wfds, efds;
	struct timeval tv;
	int rc;
#ifdef _WIN32
	u_long on = 1, off = 0;
	int err = 0;
	int errlen = (int)sizeof(err);

	ioctlsocket(s, FIONBIO, &on);
	rc = connect(s, addr, addrlen);
	if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
		ioctlsocket(s, FIONBIO, &off);
		return 0;
	}
#else
	int flags = fcntl(s, F_GETFL, 0);
	int err = 0;
	socklen_t errlen = sizeof(err);

	fcntl(s, F_SETFL, flags | O_NONBLOCK);
	rc = connect(s, addr, (socklen_t)addrlen);
	if (rc != 0 && errno != EINPROGRESS) {
		fcntl(s, F_SETFL, flags);
		return 0;
	}
#endif

	if (rc != 0) {
		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		FD_SET(s, &wfds);
		FD_SET(s, &efds);
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		rc = select(s + 1, NULL, &wfds, &efds, &tv);
		if (rc <= 0 || FD_ISSET(s, &efds)) rc = -1;
		else {
			getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
			rc = err == 0 ? 0 : -1;
		}
	}

#ifdef _WIN32
	ioctlsocket(s, FIONBIO, &off);
#else
	fcntl(s, F_SETFL, flags);
#endif
	return rc == 0;
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

	if (!pine_connect_bounded(s, (struct sockaddr *)&addr, sizeof(addr), PINE_CONNECT_MS)) {
		plat_socket_close(s);
		return -1;
	}

	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

	/* The link timeout is the socket's own: a recv that returns nothing for
	 * this long is the worker's signal to call the link dead. */
#ifdef _WIN32
	{
		DWORD ms = (DWORD)g_link_ms;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = g_link_ms / 1000;
		tv.tv_usec = (g_link_ms % 1000) * 1000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
#endif

	return s;
}

/* Worker thread. Opens the socket if the retry clock allows. 1 when connected. */
static int worker_connect(void)
{
	u64 now;

	if (g_sock >= 0) return 1;

	now = plat_time_us();
	if (g_last_try_us != 0 && now - g_last_try_us < g_retry_us) return 0;
	g_last_try_us = now;
	g_retry_us = (u64)g_retry_ms * 1000u;

	g_sock = pine_open_socket();
	g_announced = 0;
	if (g_sock < 0) return 0;

	g_ever_connected = 1;
	return 1;
}

/* Worker thread. Closes the socket; the next worker_connect opens a new one. */
static void worker_drop(void)
{
	int s;

	pthread_mutex_lock(&g_mb_lock);
	s = g_sock;
	g_sock = -1;
	pthread_mutex_unlock(&g_mb_lock);

	if (s >= 0) plat_socket_close(s);
}

/* recv() outcomes, so the drop can say what happened. */
#define RECV_OK       0
#define RECV_CLOSED  -1    /* orderly close: RPCS3 went away or dropped us */
#define RECV_SILENT  -2    /* nothing at all for g_link_ms */
#define RECV_ERROR   -3

static int errno_is_timeout(int err)
{
#ifdef _WIN32
	return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
	return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

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
		if (n == 0) return RECV_CLOSED;
		if (n < 0) {
			int err = plat_net_errno();
			if (plat_net_would_retry(err)) continue;
			return errno_is_timeout(err) ? RECV_SILENT : RECV_ERROR;
		}
		done += (u32)n;
	}
	return RECV_OK;
}

/*
 * Worker thread. The link died mid-transaction: say why, in the words the
 * Connection panel will show, and decide how soon to try again.
 */
static void worker_lost(int how)
{
	int err = plat_net_errno();

	worker_drop();

	if (how == RECV_SILENT && !g_announced) {
		/*
		 * Accepted, then never a word: that is the one-client-at-a-time server
		 * with somebody else in the chair. Stay out of its backlog for a while,
		 * counted from now, and say so once per attempt rather than once a
		 * second.
		 */
		g_last_try_us = plat_time_us();
		g_retry_us = (u64)g_silent_retry_ms * 1000u;
		plat_log("pine: RPCS3 accepted the connection but has not answered in %d s; "
		         "another program is probably connected to its IPC server (RPCS3 "
		         "serves one client at a time), close that and this will recover",
		         (g_link_ms + 999) / 1000);
		return;
	}

	if (how == RECV_SILENT)
		plat_log("pine: lost (RPCS3 stopped answering for %d s)", (g_link_ms + 999) / 1000);
	else if (how == RECV_CLOSED)
		plat_log("pine: lost (RPCS3 closed the connection)");
	else
		plat_log("pine: lost (socket error %d)", err);
}

/*
 * Worker thread. One whole transaction on g_wreq / g_wrep: connect if need be,
 * send, receive. Returns the reply's data length, or negative. `len` of zero
 * means "just make sure we are connected".
 */
static int worker_transact(u32 len)
{
	u32 total;
	u32 size;
	int rc;

	if (!worker_connect()) return -1;
	if (len == 0) return 0;

	if (pine_send_all(g_wreq, len) != 0) {
		worker_drop();
		plat_log("pine: lost (send failed)");
		return -1;
	}

	rc = pine_recv_all(g_wrep, 5);
	if (rc != RECV_OK) { worker_lost(rc); return -1; }

	total = le32_get(g_wrep);
	if (total < 5u || total > sizeof(g_wrep)) {
		worker_drop();
		plat_log("pine: lost (bad reply size %u)", (unsigned)total);
		return -1;
	}

	size = total - 5u;
	if (size > 0) {
		rc = pine_recv_all(g_wrep + 5, size);
		if (rc != RECV_OK) { worker_lost(rc); return -1; }
	}

	/* The first answer is the proof of a connection; a completed handshake
	 * alone is not, see the file header. */
	if (!g_announced) {
		g_announced = 1;
		plat_log("pine: connected to 127.0.0.1:%d", g_port);
	}

	/*
	 * A FAIL is RPCS3 saying no - an unmapped address, most often - not the
	 * connection going away, so the socket stays open and the caller gets an
	 * error exactly as PS3MAPI would have given one.
	 */
	if (g_wrep[4] != PINE_OK) return -1;

	return (int)size;
}

static void *worker_main(void *arg)
{
	(void)arg;

	for (;;) {
		u32 len;
		int result;

		pthread_mutex_lock(&g_mb_lock);
		while (g_mb_state != MB_PENDING && !g_worker_stop)
			pthread_cond_wait(&g_mb_posted, &g_mb_lock);
		if (g_worker_stop) {
			pthread_mutex_unlock(&g_mb_lock);
			break;
		}
		len = g_mb_len;
		pthread_mutex_unlock(&g_mb_lock);

		/* The state stays PENDING while this runs; the caller may turn it into
		 * ABANDONED meanwhile, which is why it is re-read below. */
		result = worker_transact(len);

		pthread_mutex_lock(&g_mb_lock);
		if (g_mb_state == MB_PENDING) {
			g_mb_result = result;
			g_mb_state = MB_DONE;
			pthread_cond_broadcast(&g_mb_finished);
		} else {
			/* Nobody is waiting for this answer any more. */
			g_mb_state = MB_IDLE;
		}
		pthread_mutex_unlock(&g_mb_lock);
	}

	return NULL;
}

/* Called with g_mb_lock held. */
static int worker_start_locked(void)
{
	if (g_worker_up) return 1;

	g_worker_stop = 0;
	g_mb_state = MB_IDLE;
	if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0) return 0;
	g_worker_up = 1;
	return 1;
}

static void mb_deadline(struct timespec *ts, int ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

/*
 * Hands `len` bytes of packet to the worker and waits up to g_wait_ms for the
 * reply, which lands in `reply` (5 + result bytes). Returns the reply's data
 * length, negative on failure, PINE_BUSY when there is no answer yet - either
 * this packet's, or an earlier caller's that the worker is still waiting on.
 */
static int mb_post(const u8 *packet, u32 len, u8 *reply)
{
	struct timespec deadline;
	int rc;

	pthread_mutex_lock(&g_mb_lock);

	if (!worker_start_locked()) {
		pthread_mutex_unlock(&g_mb_lock);
		return -1;
	}

	if (g_mb_state != MB_IDLE) {
		pthread_mutex_unlock(&g_mb_lock);
		return PINE_BUSY;
	}

	if (len > 0) memcpy(g_wreq, packet, len);
	g_mb_len = len;
	g_mb_state = MB_PENDING;
	pthread_cond_signal(&g_mb_posted);

	mb_deadline(&deadline, g_wait_ms);
	while (g_mb_state == MB_PENDING) {
		if (pthread_cond_timedwait(&g_mb_finished, &g_mb_lock, &deadline) == ETIMEDOUT)
			break;
	}

	if (g_mb_state == MB_DONE) {
		rc = g_mb_result;
		if (rc >= 0 && len > 0 && reply != NULL) memcpy(reply, g_wrep, 5u + (u32)rc);
		g_mb_state = MB_IDLE;
	} else {
		/* Still pending after the wait: the worker will find nobody listening. */
		g_mb_state = MB_ABANDONED;
		rc = PINE_BUSY;
	}

	pthread_mutex_unlock(&g_mb_lock);
	return rc;
}

/* --------------------------------------------------------- the connection */

int pine_ensure(void)
{
	pthread_mutex_lock(&g_lock);
	(void)mb_post(NULL, 0, NULL);
	pthread_mutex_unlock(&g_lock);

	return pine_connected();
}

void pine_startup(void)
{
	if (!pine_ensure())
		plat_log("pine: no server on 127.0.0.1:%d yet, retrying every second", g_port);
}

void pine_close(void)
{
	pthread_t worker;
	int had_worker = 0;
	int s;

	pthread_mutex_lock(&g_mb_lock);
	if (g_worker_up) {
		g_worker_stop = 1;
		had_worker = 1;
		worker = g_worker;
		pthread_cond_broadcast(&g_mb_posted);
	}
	/* Taking the socket away is what gets a worker out of a blocked recv. */
	s = g_sock;
	g_sock = -1;
	if (s >= 0) {
		plat_socket_shutdown(s);
		plat_socket_close(s);
	}
	pthread_mutex_unlock(&g_mb_lock);

	if (had_worker) {
		pthread_join(worker, NULL);
		pthread_mutex_lock(&g_mb_lock);
		g_worker_up = 0;
		g_mb_state = MB_IDLE;
		pthread_mutex_unlock(&g_mb_lock);
	}

	/* The worker is gone, so its bookkeeping can be reset from here. */
	g_last_try_us = 0;
	g_retry_us = 0;
	g_announced = 0;

	pine_forget_cache();
}

/*
 * Sends `reqlen` bytes of commands (without the size header, which is written
 * here) and returns the reply's data length, negative on failure, PINE_BUSY
 * when the answer is not in yet. The data itself lands in g_rep + 5. Called
 * with g_lock held.
 */
static int pine_call(u32 reqlen)
{
	if (reqlen == 0 || reqlen + 4u > sizeof(g_req)) return -1;

	le32_put(g_req, reqlen + 4u);
	return mb_post(g_req, reqlen + 4u, g_rep);
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
 *
 * Returns 0, negative, or PINE_BUSY when RPCS3 has not answered yet.
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
	if (size == PINE_BUSY) {
		rc = PINE_BUSY;
	} else if (size < 4) {
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
 *
 * A stalled RPCS3 (PINE_BUSY) changes nothing: the last answer stands until a
 * real one replaces it or the link itself is given up on. Only a failed
 * transaction - the link down - clears it, and that is what tells the session
 * the game has gone.
 */
static void pine_poll(void)
{
	u64 now = plat_time_us();
	u32 status = PINE_STATUS_SHUTDOWN;
	char id[16];
	int was;
	int running;
	int rc;

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
	rc = pine_status_and_id(&status, id);
	if (rc == PINE_BUSY) {
		/* Ask again next tick; the cache stays as it was. */
		pthread_mutex_lock(&g_poll_lock);
		g_poll_us = 0;
		pthread_mutex_unlock(&g_poll_lock);
		return;
	}

	if (rc != 0) {
		running = 0;
		id[0] = 0;
	} else {
		/*
		 * "Running" alone is not enough: RPCS3 reports Running with no game
		 * booted too, and then MsgID is empty. Both have to hold. Paused is a
		 * game that is still there - its memory, its title id, the lot - so it
		 * counts as up: pausing the emulator must not look like a quit.
		 */
		int up = status == PINE_STATUS_RUNNING || status == PINE_STATUS_PAUSED;
		running = (up && id[0] != 0) ? 1 : 0;
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
u32 plat_game_pid(void) { return pine_game_pid(); }

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

void plat_notify(const char *msg)
{
	char ts[16];
	host_stamp(ts, sizeof(ts));
	printf("[%s] notify: %s\n", ts, msg);
	fflush(stdout);
}

#endif /* QWARK_PINE_NO_PLAT */
