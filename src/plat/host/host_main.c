/*
 * qwark-host: the same core, the same protocol, a fake console.
 *
 * Runs the tick thread and the accept thread exactly as the SPRX does, and takes
 * console events on stdin:
 *
 *   boot <TITLEID> [game] a game process appears, its fingerprint is seeded.
 *                         BCES01503 hosts RaC1, RaC2 and RaC3, so the optional
 *                         second word ("rac1".."rac4") says which one to seed
 *   quit                  the game process goes away
 *   pad <hex mask>        write the running game's pad mask (drives combos)
 *   poke <addr> <hex>     write bytes into the fake process memory
 *   peek <addr> <len>     print bytes from the fake process memory
 *   status                one line of session state
 *   exit                  shut down
 */
#include "../plat.h"
#include "plat_host.h"
#include "../../core/session.h"
#include "../../core/net.h"
#include "../../core/config.h"
#include "../../core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static plat_thread_t g_tick;
static plat_thread_t g_net;

static const char *state_name(u8 state)
{
	switch (state) {
	case SESSION_XMB:      return "XMB";
	case SESSION_BOOTING:  return "BOOTING";
	case SESSION_INGAME:   return "INGAME";
	case SESSION_QUITTING: return "QUITTING";
	default:               return "?";
	}
}

static void print_status(void)
{
	printf("state=%s game=%d title=%s pid=%u gen=%u tick=%u root=%s\n",
	       state_name(session_state()),
	       session_game() ? session_game()->game_id : 0,
	       session_title(),
	       (unsigned)plat_game_pid(),
	       (unsigned)session_generation(),
	       (unsigned)session_tick_count(),
	       host_root());
	fflush(stdout);
}

static char *next_token(char **cursor)
{
	char *s = *cursor;
	char *start;

	while (*s == ' ' || *s == '\t') s++;
	if (*s == 0) { *cursor = s; return NULL; }

	start = s;
	while (*s != 0 && *s != ' ' && *s != '\t') s++;
	if (*s != 0) { *s = 0; s++; }

	*cursor = s;
	return start;
}

static void handle_line(char *line, int *keep_running)
{
	char *cursor = line;
	char *cmd = next_token(&cursor);

	if (cmd == NULL) return;

	if (qstreq(cmd, "boot")) {
		char *title = next_token(&cursor);
		char *which = (title != NULL) ? next_token(&cursor) : NULL;

		if (title == NULL) {
			printf("usage: boot <TITLEID> [rac1|rac2|rac3|rac4]\n");
			fflush(stdout);
			return;
		}

		/* BCES01503 hosts three games; `which` says whose fingerprint to seed. */
		host_boot_as(title, which);
		printf("ok boot %s%s%s\n", title, which ? " " : "", which ? which : "");

	} else if (qstreq(cmd, "quit")) {
		host_quit();
		printf("ok quit\n");

	} else if (qstreq(cmd, "pad")) {
		char *hex = next_token(&cursor);
		int ok = 0;
		u32 mask;
		if (hex == NULL) { printf("usage: pad <hex mask>\n"); fflush(stdout); return; }
		mask = qparse_u32(hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X') ? hex : hex, &ok);
		if (!ok) { printf("err bad mask\n"); fflush(stdout); return; }
		printf(host_set_pad(mask) == 0 ? "ok pad\n" : "err pad\n");

	} else if (qstreq(cmd, "poke")) {
		char *addr_s = next_token(&cursor);
		char *hex = next_token(&cursor);
		u8 bytes[256];
		u32 n;
		int ok = 0;
		u32 addr;

		if (addr_s == NULL || hex == NULL) { printf("usage: poke <addr> <hex>\n"); fflush(stdout); return; }
		addr = qparse_u32(addr_s, &ok);
		if (!ok) { printf("err bad addr\n"); fflush(stdout); return; }

		n = qhex_to_bytes(hex, bytes, sizeof(bytes));
		if (n == 0) { printf("err bad hex\n"); fflush(stdout); return; }

		host_poke(addr, bytes, n);
		printf("ok poke %u\n", (unsigned)n);

	} else if (qstreq(cmd, "peek")) {
		char *addr_s = next_token(&cursor);
		char *len_s = next_token(&cursor);
		u8 bytes[256];
		char hex[2 * 256 + 1];
		int ok = 0;
		u32 addr, len;

		if (addr_s == NULL || len_s == NULL) { printf("usage: peek <addr> <len>\n"); fflush(stdout); return; }
		addr = qparse_u32(addr_s, &ok);
		if (!ok) { printf("err bad addr\n"); fflush(stdout); return; }
		len = qparse_u32(len_s, &ok);
		if (!ok || len == 0 || len > sizeof(bytes)) { printf("err bad len\n"); fflush(stdout); return; }

		host_peek(addr, bytes, len);
		qbytes_to_hex(bytes, len, hex, sizeof(hex));
		printf("ok peek %s\n", hex);

	} else if (qstreq(cmd, "status")) {
		print_status();
		return;

	} else if (qstreq(cmd, "exit")) {
		printf("ok exit\n");
		*keep_running = 0;

	} else {
		printf("err unknown command\n");
	}

	fflush(stdout);
}

int main(int argc, char **argv)
{
	char line[1024];
	int keep_running = 1;

	(void)argc;
	(void)argv;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (plat_init() != 0) {
		fprintf(stderr, "qwark-host: plat_init failed\n");
		return 1;
	}

	if (session_init() != ST_OK) {
		fprintf(stderr, "qwark-host: session_init failed\n");
		return 1;
	}

	if (net_init() != ST_OK) {
		fprintf(stderr, "qwark-host: net_init failed\n");
		return 1;
	}

	if (plat_thread_create(&g_tick, session_tick_thread, NULL, 49152, "qwark_tick") != 0) {
		fprintf(stderr, "qwark-host: cannot start the tick thread\n");
		return 1;
	}

	if (plat_thread_create(&g_net, net_accept_thread, NULL, 16384, "qwark_net") != 0) {
		fprintf(stderr, "qwark-host: cannot start the accept thread\n");
		return 1;
	}

	printf("qwark-host ready on %d, root %s\n", QWARK_PORT, host_root());
	fflush(stdout);

	while (keep_running && fgets(line, sizeof(line), stdin) != NULL) {
		char *trimmed = qtrim(line);
		handle_line(trimmed, &keep_running);
	}

	net_stop();
	session_stop();

	plat_thread_join(g_net);
	plat_thread_join(g_tick);

	plat_shutdown();
	return 0;
}
