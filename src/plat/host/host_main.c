/*
 * qwark-host: the same core, the same protocol, a fake console.
 *
 * Runs the tick thread and the accept thread exactly as the SPRX does, and takes
 * console events on stdin:
 *
 *   boot <TITLEID> [game] a game process appears, its fingerprint is seeded.
 *                         BCES01503 hosts RaC1, RaC2 and RaC3, so the optional
 *                         second word ("rac1".."rac4", "tod") says which one to seed
 *   quit                  the game process goes away
 *   pad <hex mask>        write the running game's pad mask (drives combos)
 *   poke <addr> <hex>     write bytes into the fake process memory
 *   peek <addr> <len>     print bytes from the fake process memory
 *   vshmem <free> <total> the fake VSH's memory, in KB, and a reading of it
 *                         now; `vshmem off` makes it unreadable again
 *   user <n>              the fake XMB's signed-in user number (1 at start, 0
 *                         nobody); the session sees it at the XMB within a
 *                         second, never in a game (revision 1.24)
 *   fakelua on|off        the simulated ToD's Lua helper runs a frame every
 *                         16 ms (revision 1.27)
 *   fakepause on|off      the simulated ToD in its pause menu, whose frame the
 *                         fakelua thread runs instead (revision 1.29); `wait`,
 *                         one whose presentation never comes up; `short`, one
 *                         whose Lua pool is short even after a collection,
 *                         `tight`, one too short for the glue's load
 *   luapatch on|off       ToD boots with the Lua helper's RPCS3 patch in
 *   fakesave on|off       the simulated ToD's save system runs a frame every
 *                         16 ms and takes qwark's loads (revision 1.28)
 *   fakesave slot <n>|-1  the simulated ToD saved into slot n (or has none)
 *   status                one line of session state
 *   exit                  shut down
 *
 * Usage: qwark-host.exe [--port 9673] [--bind 127.0.0.1] [--vshmem FREE,TOTAL]
 *
 *   --port    the port qwark itself listens on for the client, 9673
 *   --bind    the address the listener binds. Loopback by default, so Windows
 *             Firewall is never asked about the simulator; pass 0.0.0.0 to make
 *             it reachable from another machine, and answer the prompt once.
 *   --vshmem  the fake VSH's memory from the start, in KB, so the module start's
 *             readings have something to read. Without it, and until the
 *             `vshmem` command, the VSH's memory cannot be read, as under RPCS3
 */
#include "../plat.h"
#include "plat_host.h"
#include "../../core/session.h"
#include "../../core/net.h"
#include "../../core/config.h"
#include "../../core/util.h"
#include "../../core/gamemod.h"
#include "../../games/titles.h"

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

	} else if (qstreq(cmd, "arena")) {
		/* 1 while a connection owns the request arena: an idle one must not. */
		printf("ok arena %u\n", (unsigned)net_arena_held());

	} else if (qstreq(cmd, "arena_takes")) {
		printf("ok arena_takes %u\n", (unsigned)net_arena_takes());

	} else if (qstreq(cmd, "activity")) {
		/* Observe the quiet window without asking the backend for a PID/title. */
		session_activity_lock();
		printf("ok activity %d %u %u %u %u %u\n", session_quiet(),
		       (unsigned)host_pid_calls(), (unsigned)host_title_calls(),
		       (unsigned)host_mem_reads(), (unsigned)host_write_log_count(),
		       (unsigned)net_telemetry_sends());
		session_activity_unlock();

	} else if (qstreq(cmd, "modules")) {
		/*
		 * Revision 1.26. `modules on` stands game modules in for the linked
		 * games from the next boot on (the simulator's loader, backend_fake.c),
		 * with the memory gate in front of them; `modules off` is linked again.
		 */
		char *on = next_token(&cursor);
		if (on == NULL || (!qstreq(on, "on") && !qstreq(on, "off"))) {
			printf("usage: modules on|off\n");
			fflush(stdout);
			return;
		}
		session_activity_lock();
		gamemod_set_modules(qstreq(on, "on"));
		session_activity_unlock();
		printf("ok modules %s\n", on);

	} else if (qstreq(cmd, "fakemodule")) {
		/*
		 * Revision 1.26. Writes game n's module (/dev_hdd0/qwark/rac<n>.sprx for
		 * the four, title_module_name) as the simulator's
		 * loader takes it: `fakemodule <n>` this build's, `fakemodule <n> other`
		 * another build's, `fakemodule <n> broken` no module at all,
		 * `fakemodule <n> refused` one lv2 will not load, `fakemodule <n> pages
		 * <p>` one of p pages, `fakemodule <n> gone` deletes it.
		 */
		char *n_s = next_token(&cursor);
		char *how = n_s != NULL ? next_token(&cursor) : NULL;
		char *arg = how != NULL ? next_token(&cursor) : NULL;
		char path[48];
		int ok = 0, rc;
		u32 n = n_s != NULL ? qparse_u32(n_s, &ok) : 0;
		u32 pages = 1;

		if (!ok || n < 1 || n > 4) {
			printf("usage: fakemodule <1-4> [other|broken|refused|gone|pages <n>]\n");
			fflush(stdout);
			return;
		}
		title_module_path((u8)n, path, sizeof(path), 0);
		if (how != NULL && qstreq(how, "pages") && arg != NULL) pages = qparse_u32(arg, &ok);
		if (how != NULL && qstreq(how, "gone")) rc = plat_file_unlink(path);
		else if (how != NULL && qstreq(how, "other"))
			rc = host_write_fake_module(path, (u8)n, QWARK_BUILD, QWARK_BUILD_ID ^ 0x5A5A5A5Au, 1, 0);
		else if (how != NULL && qstreq(how, "broken")) rc = host_write_fake_module(path, (u8)n, QWARK_BUILD, QWARK_BUILD_ID, 0, 0);
		else if (how != NULL && qstreq(how, "refused"))
			rc = host_write_fake_module(path, (u8)n, QWARK_BUILD, QWARK_BUILD_ID, 1, 0x80010004u);
		else rc = host_write_fake_module(path, (u8)n, QWARK_BUILD, QWARK_BUILD_ID, (u8)pages, 0);
		printf(rc == 0 ? "ok fakemodule %u\n" : "err fakemodule %u\n", (unsigned)n);

	} else if (qstreq(cmd, "vshmem")) {
		/*
		 * Build 54. The figure, and a reading at once rather than at the next
		 * ten-second one, under the lock the tick thread changes the session
		 * state under, so the reading sees one state.
		 */
		char *free_s = next_token(&cursor);
		char *total_s = (free_s != NULL) ? next_token(&cursor) : NULL;
		int ok1 = 0, ok2 = 0;
		u32 free_kb = 0, total_kb = 0;

		if (free_s != NULL && qstreq(free_s, "off")) {
			host_set_vsh_memory(0, 0);
			printf("ok vshmem off\n");
			fflush(stdout);
			return;
		}
		if (free_s != NULL) free_kb = qparse_u32(free_s, &ok1);
		if (total_s != NULL) total_kb = qparse_u32(total_s, &ok2);
		if (!ok1 || !ok2 || total_kb == 0 || free_kb > total_kb || total_kb > 4194303u) {
			printf("usage: vshmem <free KB> <total KB> | vshmem off\n");
			fflush(stdout);
			return;
		}
		host_set_vsh_memory(free_kb * 1024u, total_kb * 1024u);
		session_activity_lock();
		session_vsh_memory_read(VSHMEM_AT_PERIODIC);
		session_activity_unlock();
		printf("ok vshmem %u %u\n", (unsigned)free_kb, (unsigned)total_kb);

	} else if (qstreq(cmd, "user")) {
		char *n_s = next_token(&cursor);
		int ok = 0;
		u32 n = (n_s != NULL) ? qparse_u32(n_s, &ok) : 0;

		if (!ok) {
			printf("usage: user <n>\n");
			fflush(stdout);
			return;
		}
		host_set_user(n);
		printf("ok user %u\n", (unsigned)n);

	} else if (qstreq(cmd, "fakepause")) {
		/*
		 * Revision 1.29: `fakepause on` puts the simulated ToD in its pause
		 * menu, whose frame the fakelua thread then runs instead of a
		 * gameplay frame; off, back to play.
		 */
		char *on = next_token(&cursor);

		if (on == NULL || (!qstreq(on, "on") && !qstreq(on, "off") && !qstreq(on, "wait") &&
		                   !qstreq(on, "short") && !qstreq(on, "tight"))) {
			printf("usage: fakepause on|off|wait|short|tight\n");
		} else {
			/*
			 * wait (the crash fix): a pause whose presentation never comes up,
			 * which the helper never enters. short and tight (the second
			 * crash): one whose Lua pool is short, of every frame or of the
			 * glue's load.
			 */
			host_set_fake_pause(qstreq(on, "on") ? 1 : qstreq(on, "wait") ? 2 :
			                    qstreq(on, "short") ? 3 : qstreq(on, "tight") ? 4 : 0);
			printf("ok fakepause %s\n", on);
		}

	} else if (qstreq(cmd, "fakelua") || qstreq(cmd, "luapatch")) {
		/*
		 * Revision 1.27: `fakelua on` runs the simulated ToD's Lua helper a
		 * frame every 16 ms; `luapatch on` boots ToD with the helper's RPCS3
		 * patch in, as RPCS3 does once the client has written it.
		 */
		char *on = next_token(&cursor);

		if (on == NULL || (!qstreq(on, "on") && !qstreq(on, "off"))) {
			printf("usage: %s on|off\n", cmd);
		} else if (qstreq(cmd, "fakelua")) {
			host_set_fake_lua(qstreq(on, "on"));
			printf("ok fakelua %s\n", on);
		} else {
			host_set_lua_patch(qstreq(on, "on"));
			printf("ok luapatch %s\n", on);
		}

	} else if (qstreq(cmd, "fakesave")) {
		/*
		 * Revision 1.28: `fakesave on` runs the simulated ToD's save
		 * system a frame every 16 ms (it takes qwark's loads), and `fakesave
		 * slot <n>` is the game's own save into slot n (-1 for none).
		 */
		char *on = next_token(&cursor);

		if (on != NULL && qstreq(on, "slot")) {
			char *n_s = next_token(&cursor);
			int ok = 0;
			u32 n = 0xFFFFFFFFu;

			if (n_s != NULL && qstreq(n_s, "-1")) ok = 1;
			else if (n_s != NULL) n = qparse_u32(n_s, &ok);
			if (!ok) {
				printf("usage: fakesave slot <n>|-1\n");
			} else {
				host_fake_tod_saved(n);
				printf("ok fakesave slot %s\n", n_s);
			}
		} else if (on == NULL || (!qstreq(on, "on") && !qstreq(on, "off"))) {
			printf("usage: fakesave on|off|slot <n>\n");
		} else {
			host_set_fake_tod_save(qstreq(on, "on"));
			printf("ok fakesave %s\n", on);
		}

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

static int parse_int(const char *s, int *out)
{
	long v;
	char *end = NULL;

	if (s == NULL || s[0] == 0) return 0;
	v = strtol(s, &end, 0);
	if (end == NULL || *end != 0) return 0;
	if (v <= 0 || v > 65535) return 0;

	*out = (int)v;
	return 1;
}

static void usage(void)
{
	printf("usage: qwark-host [--port N] [--bind ADDR] [--vshmem FREE,TOTAL]\n");
	printf("  --port N     the port to listen on, %d by default\n", QWARK_PORT);
	printf("  --bind ADDR  the address to listen on, 127.0.0.1 by default;\n");
	printf("               0.0.0.0 to be reachable from another machine\n");
	printf("  --vshmem F,T the fake VSH's free and total memory, KB, from the start\n");
	fflush(stdout);
}

/* "FREE,TOTAL" in KB, as --vshmem takes it. */
static int parse_vshmem(const char *s, u32 *free_kb, u32 *total_kb)
{
	char *end = NULL;
	unsigned long f, t;

	if (s == NULL) return 0;
	f = strtoul(s, &end, 10);
	if (end == s || *end != ',') return 0;
	s = end + 1;
	t = strtoul(s, &end, 10);
	if (end == s || *end != 0 || t == 0 || f > t || t > 4194303ul) return 0;
	*free_kb = (u32)f;
	*total_kb = (u32)t;
	return 1;
}

int main(int argc, char **argv)
{
	char line[1024];
	int keep_running = 1;
	int port = QWARK_PORT;
	const char *bind_addr = "127.0.0.1";
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 1; i < argc; i++) {
		if (qstreq(argv[i], "--port") && i + 1 < argc) {
			if (!parse_int(argv[++i], &port)) { usage(); return 2; }
		} else if (qstreq(argv[i], "--bind") && i + 1 < argc) {
			bind_addr = argv[++i];
		} else if (qstreq(argv[i], "--vshmem") && i + 1 < argc) {
			u32 free_kb, total_kb;
			if (!parse_vshmem(argv[++i], &free_kb, &total_kb)) { usage(); return 2; }
			host_set_vsh_memory(free_kb * 1024u, total_kb * 1024u);
		} else {
			usage();
			return 2;
		}
	}

	net_set_port((u16)port);
	if (!net_set_bind(bind_addr)) {
		fprintf(stderr, "qwark-host: --bind wants a dotted quad, not '%s'\n", bind_addr);
		return 2;
	}

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

	/* The two readings the module entry takes, either side of the tick thread. */
	session_vsh_memory_read(VSHMEM_AT_START);

	if (plat_thread_create(&g_tick, session_tick_thread, NULL, 49152, "qwark_tick") != 0) {
		fprintf(stderr, "qwark-host: cannot start the tick thread\n");
		return 1;
	}

	session_vsh_memory_read(VSHMEM_AT_TICK_THREAD);

	if (plat_thread_create(&g_net, net_accept_thread, NULL, 16384, "qwark_net") != 0) {
		fprintf(stderr, "qwark-host: cannot start the accept thread\n");
		return 1;
	}

	printf("qwark-host ready on %s:%d, root %s\n",
	       bind_addr, (int)net_port(), host_root());
	fflush(stdout);

	while (keep_running && fgets(line, sizeof(line), stdin) != NULL) {
		char *trimmed = qtrim(line);
		handle_line(trimmed, &keep_running);
	}

	/* The same order the SPRX stops in, so the simulator exercises that path. */
	net_stop();
	session_stop();

	plat_thread_join(g_net);
	plat_thread_join(g_tick);

	net_wait_clients(2000000u);

	net_shutdown();
	session_shutdown();

	plat_shutdown();
	return 0;
}
