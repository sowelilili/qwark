/*
 * The PINE backend against a fake PINE server.
 *
 * The server below speaks exactly the packet format RPCS3's
 * 3rdparty/pine/pine_server.h speaks, down to the two details that are easy to
 * get wrong and expensive to get wrong:
 *
 *   - a read returns the LOGICAL value of the word, little-endian, because
 *     RPCS3 converts the be_t out of guest memory before it copies it onto the
 *     wire. Its memory here is therefore kept in PS3 byte order and swapped on
 *     the way out, exactly as the emulator does.
 *   - MsgStatus advances the request cursor by four even though it takes no
 *     argument, so anything batched behind it needs four bytes of padding.
 *
 * One failing command fails the whole packet with a bare five-byte FAIL reply,
 * which is what makes a batched read all-or-nothing.
 */
#include "../src/plat/plat.h"
#include "../src/plat/plat_net.h"
#include "../src/plat/host/backend_pine.h"
#include "../src/core/util.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------- the fake server */

#define FAKE_BASE 0x00300000u
#define FAKE_LEN  0x00010000u
#define FAKE_REQ  262144u
#define FAKE_REP  262144u

static int  g_listen = -1;
static int  g_port;
static plat_thread_t g_thread;

static volatile int g_srv_running;
static volatile int g_srv_stop;
static volatile int g_srv_drop;      /* drop the live connection at the next poll */
static volatile int g_conns;         /* connections accepted so far */
static volatile int g_packets;       /* request packets answered so far */

static u32  g_status = PINE_STATUS_RUNNING;
static char g_id[16] = "NPEA00385";
static char g_name[64] = "Ratchet & Clank";

static u8 g_mem[FAKE_LEN];
static u8 g_req[FAKE_REQ];
static u8 g_rep[FAKE_REP];

static void le32_put(u8 *p, u32 v)
{
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 le32_get(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void le16_put(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16 le16_get(const u8 *p) { return (u16)((u16)p[0] | ((u16)p[1] << 8)); }

static void le64_put(u8 *p, u64 v)
{
	le32_put(p, (u32)v);
	le32_put(p + 4, (u32)(v >> 32));
}

static u64 le64_get(const u8 *p)
{
	return (u64)le32_get(p) | ((u64)le32_get(p + 4) << 32);
}

static int fake_valid(u32 addr, u32 size)
{
	if (addr < FAKE_BASE) return 0;
	if ((u64)addr + size > (u64)FAKE_BASE + FAKE_LEN) return 0;
	return 1;
}

static u8 *fake_at(u32 addr) { return g_mem + (addr - FAKE_BASE); }

/* Writes `u32 length including the NUL, bytes, NUL` at g_rep + *ret. */
static void fake_string(u32 *ret, const char *s)
{
	u32 n = (u32)strlen(s);
	le32_put(g_rep + *ret, n + 1u);
	*ret += 4u;
	memcpy(g_rep + *ret, s, n);
	*ret += n;
	g_rep[(*ret)++] = 0;
}

/* Returns the reply length, always at least 5. */
static u32 fake_parse(const u8 *buf, u32 size)
{
	u32 cur = 0;
	u32 ret = 5;

	while (cur < size) {
		u8 op = buf[cur++];
		u32 a;

		switch (op) {
		case PINE_MSG_READ8:
			if (cur + 4 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 1)) goto fail;
			g_rep[ret++] = *fake_at(a);
			cur += 4;
			break;

		case PINE_MSG_READ16:
			if (cur + 4 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 2)) goto fail;
			le16_put(g_rep + ret, be16_get(fake_at(a)));
			ret += 2;
			cur += 4;
			break;

		case PINE_MSG_READ32:
			if (cur + 4 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 4)) goto fail;
			le32_put(g_rep + ret, be32_get(fake_at(a)));
			ret += 4;
			cur += 4;
			break;

		case PINE_MSG_READ64:
			if (cur + 4 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 8)) goto fail;
			le64_put(g_rep + ret, be64_get(fake_at(a)));
			ret += 8;
			cur += 4;
			break;

		case PINE_MSG_WRITE8:
			if (cur + 5 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 1)) goto fail;
			*fake_at(a) = buf[cur + 4];
			cur += 5;
			break;

		case PINE_MSG_WRITE16:
			if (cur + 6 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 2)) goto fail;
			be16_put(fake_at(a), le16_get(buf + cur + 4));
			cur += 6;
			break;

		case PINE_MSG_WRITE32:
			if (cur + 8 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 4)) goto fail;
			be32_put(fake_at(a), le32_get(buf + cur + 4));
			cur += 8;
			break;

		case PINE_MSG_WRITE64:
			if (cur + 12 > size) goto fail;
			a = le32_get(buf + cur);
			if (!fake_valid(a, 8)) goto fail;
			be64_put(fake_at(a), le64_get(buf + cur + 4));
			cur += 12;
			break;

		case PINE_MSG_STATUS:
			le32_put(g_rep + ret, g_status);
			ret += 4;
			cur += 4;          /* the quirk: no argument, cursor moves anyway */
			break;

		case PINE_MSG_ID:
			fake_string(&ret, g_id);
			break;

		case PINE_MSG_TITLE:
			fake_string(&ret, g_name);
			break;

		case PINE_MSG_VERSION:
			fake_string(&ret, "RPCS3 fake-pine");
			break;

		default:
			goto fail;
		}
	}

	le32_put(g_rep, ret);
	g_rep[4] = PINE_OK;
	return ret;

fail:
	le32_put(g_rep, 5);
	g_rep[4] = PINE_FAIL;
	return 5;
}

static int fake_recv_all(int fd, u8 *buf, u32 len)
{
	u32 done = 0;
	while (done < len) {
		int n = (int)recv(fd, (char *)buf + done, (int)(len - done), 0);
		if (n <= 0) return -1;
		done += (u32)n;
	}
	return 0;
}

/* Waits up to `ms` for the descriptor to become readable. 1 ready, 0 timeout. */
static int fake_wait(int fd, int ms)
{
	fd_set r;
	struct timeval tv;

	FD_ZERO(&r);
	FD_SET((unsigned)fd, &r);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;

	return select(fd + 1, &r, NULL, NULL, &tv) > 0 ? 1 : 0;
}

static void fake_serve(int fd)
{
	for (;;) {
		u32 total;
		u32 size;
		u32 replylen;

		if (g_srv_stop || g_srv_drop) return;
		if (!fake_wait(fd, 20)) continue;

		if (fake_recv_all(fd, g_req, 4) != 0) return;
		total = le32_get(g_req);
		if (total < 4 || total > FAKE_REQ) return;

		size = total - 4;
		if (size > 0 && fake_recv_all(fd, g_req, size) != 0) return;

		replylen = fake_parse(g_req, size);
		g_packets++;

		if (send(fd, (const char *)g_rep, (int)replylen, 0) != (int)replylen) return;
	}
}

static void fake_thread(void *arg)
{
	(void)arg;

	g_srv_running = 1;

	while (!g_srv_stop) {
		int fd;

		if (!fake_wait(g_listen, 20)) continue;

		fd = (int)accept(g_listen, NULL, NULL);
		if (fd < 0) continue;

		g_conns++;
		fake_serve(fd);
		g_srv_drop = 0;
		plat_socket_close(fd);
	}

	g_srv_running = 0;
	plat_thread_exit();
}

static int fake_start(void)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);

	g_listen = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listen < 0) return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = 0;                              /* any free port */
	sa.sin_addr.s_addr = htonl(0x7F000001u);      /* 127.0.0.1, as PINE binds */

	if (bind(g_listen, (struct sockaddr *)&sa, sizeof(sa)) < 0) return -1;
	if (listen(g_listen, 4) < 0) return -1;

	memset(&sa, 0, sizeof(sa));
	if (getsockname(g_listen, (struct sockaddr *)&sa, &len) < 0) return -1;
	g_port = (int)ntohs(sa.sin_port);

	g_srv_stop = 0;
	if (plat_thread_create(&g_thread, fake_thread, NULL, 65536, "fake_pine") != 0)
		return -1;

	return 0;
}

static void fake_stop(void)
{
	g_srv_stop = 1;
	plat_thread_join(g_thread);
	if (g_listen >= 0) { plat_socket_close(g_listen); g_listen = -1; }
}

/* ------------------------------------------------------------------ tests */

/*
 * A PPC "li r3, 1" and a 64-bit pattern, in the byte order the PS3 keeps them:
 * this is what the core must see coming back out of pine_read.
 */
static const u8 WORD_BE[4] = { 0x38, 0x60, 0x00, 0x01 };
static const u8 QUAD_BE[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };

static void seed(void)
{
	u32 i;
	for (i = 0; i < FAKE_LEN; i++) g_mem[i] = (u8)(i * 7u + 3u);

	memcpy(fake_at(FAKE_BASE + 0x100), WORD_BE, 4);
	memcpy(fake_at(FAKE_BASE + 0x200), QUAD_BE, 8);
}

void test_pine(void)
{
	u8 buf[64];
	u8 out[300];
	u32 status = 0xFFFFFFFFu;
	char id[16];
	char version[64];
	int before;
	u32 i;

	group("PINE backend");

	seed();

	if (fake_start() != 0) {
		check(0, "the fake PINE server starts");
		return;
	}
	check(1, "the fake PINE server starts");

	pine_set_port(g_port);
	check(pine_ensure() == 1, "the backend connects to it");
	check(pine_connected() == 1, "and reports the connection");

	/* ------------------------------------------------------- byte order */

	memset(buf, 0, sizeof(buf));
	check(pine_read(FAKE_BASE + 0x100, buf, 4) == 0, "a 4-byte read succeeds");
	check(memcmp(buf, WORD_BE, 4) == 0,
	      "and hands the core the PS3's own bytes, not the emulator's");
	check_eq_u64(be32_get(buf), 0x38600001u, "so be32_get sees the instruction word");

	memset(buf, 0, sizeof(buf));
	check(pine_read(FAKE_BASE + 0x200, buf, 8) == 0, "an 8-byte read succeeds");
	check(memcmp(buf, QUAD_BE, 8) == 0, "and round-trips byte-exact");
	check_eq_u64(be64_get(buf), 0x0123456789ABCDEFull, "be64_get sees the value");

	memset(buf, 0, sizeof(buf));
	check(pine_read(FAKE_BASE + 0x100, buf, 1) == 0, "a 1-byte read succeeds");
	check(buf[0] == 0x38, "and returns the first byte, not the last");

	/* --------------------------------------------------- odd-length block */

	memset(out, 0, sizeof(out));
	check(pine_read(FAKE_BASE + 0x40, out, 13) == 0, "a 13-byte read succeeds");
	check(memcmp(out, fake_at(FAKE_BASE + 0x40), 13) == 0,
	      "an odd length round-trips byte-exact (8 + 5)");

	memset(out, 0, sizeof(out));
	check(pine_read(FAKE_BASE + 0x41, out, 7) == 0, "an unaligned 7-byte read succeeds");
	check(memcmp(out, fake_at(FAKE_BASE + 0x41), 7) == 0,
	      "and is byte-exact too");

	memset(out, 0, sizeof(out));
	check(pine_read(FAKE_BASE + 0x1000, out, 257) == 0, "a 257-byte read succeeds");
	check(memcmp(out, fake_at(FAKE_BASE + 0x1000), 257) == 0,
	      "and matches the server's image");

	/* ------------------------------------------------------------ writes */

	for (i = 0; i < sizeof(buf); i++) buf[i] = (u8)(0xA0 + i);

	check(pine_write(FAKE_BASE + 0x2000, buf, 4) == 0, "a 4-byte write succeeds");
	check(memcmp(fake_at(FAKE_BASE + 0x2000), buf, 4) == 0,
	      "and lands in the server's memory in PS3 byte order");

	check(pine_write(FAKE_BASE + 0x2010, buf, 8) == 0, "an 8-byte write succeeds");
	check(memcmp(fake_at(FAKE_BASE + 0x2010), buf, 8) == 0, "and is byte-exact");

	check(pine_write(FAKE_BASE + 0x2020, buf, 1) == 0, "a 1-byte write succeeds");
	check(*fake_at(FAKE_BASE + 0x2020) == buf[0], "and writes that one byte");

	check(pine_write(FAKE_BASE + 0x2030, buf, 13) == 0, "a 13-byte write succeeds");
	check(memcmp(fake_at(FAKE_BASE + 0x2030), buf, 13) == 0,
	      "and an odd length is byte-exact (8 + 5)");

	memset(out, 0, sizeof(out));
	check(pine_read(FAKE_BASE + 0x2030, out, 13) == 0, "reading it back succeeds");
	check(memcmp(out, buf, 13) == 0, "write then read round-trips");

	/* ----------------------------------------------------------- batching */

	before = g_packets;
	check(pine_read(FAKE_BASE + 0x3000, out, 256) == 0, "a 256-byte read succeeds");
	check(g_packets - before == 1,
	      "and cost exactly one packet: 32 MsgRead64 in one request");

	before = g_packets;
	check(pine_read(FAKE_BASE + 0x3000, out, 260) == 0, "a 260-byte read succeeds");
	check(g_packets - before == 1,
	      "and its four-byte tail rides in the same packet");

	before = g_packets;
	check(pine_write(FAKE_BASE + 0x3400, buf, 64) == 0, "a 64-byte write succeeds");
	check(g_packets - before == 1, "and is one packet as well");

	/* ------------------------------------------------------ status and id */

	g_status = PINE_STATUS_RUNNING;
	before = g_packets;
	check(pine_status_and_id(&status, id) == 0, "status and id come back together");
	check(g_packets - before == 1, "in one packet, MsgStatus padded to four bytes");
	check_eq_u64(status, PINE_STATUS_RUNNING, "the status is Running");
	check(qstreq(id, "NPEA00385"), "and the id is the title");

	check(pine_title_id(id) == 0 && qstreq(id, "NPEA00385"),
	      "MsgID on its own answers the same");
	check(pine_version(version, sizeof(version)) == 0 &&
	      memcmp(version, "RPCS3 ", 6) == 0, "MsgVersion answers a string");

	pine_forget_cache();
	check(pine_game_running() == 1, "the backend reports a game running");
	check(pine_game_pid() == 1, "with pid 1");
	id[0] = 0;
	check(pine_game_title(id) == 1 && qstreq(id, "NPEA00385"),
	      "and the title id");

	before = g_packets;
	for (i = 0; i < 100; i++) (void)pine_game_running();
	check(g_packets - before == 0,
	      "a hundred more asks inside 50 ms cost no packets at all");

	g_status = PINE_STATUS_PAUSED;
	pine_forget_cache();
	check(pine_game_running() == 0, "Paused is not running");
	check(pine_game_pid() == 0, "and the pid goes to 0");

	g_status = PINE_STATUS_RUNNING;
	g_id[0] = 0;
	pine_forget_cache();
	check(pine_game_running() == 0, "Running with an empty id is not running either");

	snprintf(g_id, sizeof(g_id), "NPEA00385");
	pine_forget_cache();
	check(pine_game_running() == 1, "and it comes back when the id does");

	/* ------------------------------------------------------- a FAIL reply */

	check(pine_read(FAKE_BASE + FAKE_LEN + 0x1000, buf, 4) < 0,
	      "a read of an unmapped address is an error");
	check(pine_connected() == 1, "and a FAIL reply does not close the socket");
	check(pine_write(FAKE_BASE + FAKE_LEN + 0x1000, buf, 4) < 0,
	      "so is a write to one");
	check(pine_read(FAKE_BASE + 0x100, buf, 4) == 0,
	      "and the next good read still works");

	/* --------------------------------------------------------- reconnect */

	before = g_conns;
	g_srv_drop = 1;
	/* The server drops the connection within one 20 ms poll. */
	plat_sleep_us(120000);

	check(pine_read(FAKE_BASE + 0x100, buf, 4) < 0,
	      "a read over the dropped connection fails");
	check(pine_connected() == 0, "and the backend notices it is down");
	check(pine_game_running() == 0, "so the session is told there is no game");

	/* pine_ensure retries at most once a second; wait it out. */
	plat_sleep_us(1100000);

	check(pine_ensure() == 1, "a second later it reconnects");
	memset(buf, 0, sizeof(buf));
	check(pine_read(FAKE_BASE + 0x100, buf, 4) == 0 && memcmp(buf, WORD_BE, 4) == 0,
	      "and reads are byte-exact again");
	/* Only now: the accept happens on the server thread, so a completed
	 * round trip is the proof that it ran. */
	check(g_conns - before == 1, "the server saw exactly one new connection");

	pine_forget_cache();
	check(pine_game_running() == 1, "the game is reported running again");

	pine_close();
	fake_stop();
	check(g_srv_running == 0, "the fake PINE server stops");
}
