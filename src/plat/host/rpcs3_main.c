/*
 * qwark-rpcs3: the same core, the same protocol, RPCS3 instead of a console.
 *
 * Runs the tick thread and the accept thread exactly as the SPRX does. There is
 * no fake console to drive: the game state comes from RPCS3 over PINE, so the
 * only things stdin takes are
 *
 *   status   one line of session state
 *   version  what RPCS3 reports for MsgVersion, MsgTitle and MsgID
 *   exit     shut down
 *
 * Usage: qwark-rpcs3.exe [--pine-port N] [--port 9673] [--root DIR]
 *
 *   --pine-port  RPCS3's IPC port, 28012 unless it was changed in
 *                Settings -> I/O -> IPC
 *   --port       the port qwark itself listens on for the client, 9673
 *   --root       where /dev_hdd0 is mapped; by default a qwark-rpcs3-root
 *                folder beside the executable, with the same layout the
 *                console uses (dev_hdd0/qwark/config.txt, positions, mods)
 *
 * Code patches do not work under RPCS3 - the PPU code is recompiled, so writing
 * an instruction word changes memory and nothing else - so everything that
 * needs one is refused and the client is told through SessionInfo flags bit2.
 */
#include "../plat.h"
#include "plat_host.h"
#include "backend_pine.h"
#include "../../core/session.h"
#include "../../core/net.h"
#include "../../core/config.h"
#include "../../core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static plat_thread_t g_tick;
static plat_thread_t g_net;

static volatile int g_keep_running = 1;

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
	char title[16];

	title[0] = 0;
	plat_game_title(title);

	printf("state=%s game=%d title=%s pine=%s gen=%u tick=%u root=%s\n",
	       state_name(session_state()),
	       session_game() ? session_game()->game_id : 0,
	       title[0] ? title : "-",
	       pine_connected() ? "connected" : "down",
	       (unsigned)session_generation(),
	       (unsigned)session_tick_count(),
	       host_root());
	fflush(stdout);
}

static void print_version(void)
{
	char version[128];
	char title[128];
	char id[16];

	version[0] = title[0] = id[0] = 0;
	pine_version(version, sizeof(version));
	pine_title(title, sizeof(title));
	pine_title_id(id);

	printf("rpcs3=%s title=%s id=%s\n",
	       version[0] ? version : "-",
	       title[0] ? title : "-",
	       id[0] ? id : "-");
	fflush(stdout);
}

/*
 * The same order the SPRX stops in, so this path is exercised too. Runs once:
 * either the stdin loop ended or Ctrl-C got here first, never both.
 */
static long g_stopping;

static void shutdown_all(void)
{
#ifdef _WIN32
	if (InterlockedExchange(&g_stopping, 1) != 0) return;
#else
	if (g_stopping) return;
	g_stopping = 1;
#endif

	net_stop();
	session_stop();

	plat_thread_join(g_net);
	plat_thread_join(g_tick);

	net_wait_clients(2000000u);

	net_shutdown();
	session_shutdown();

	plat_shutdown();
}

#ifdef _WIN32
/*
 * Ctrl-C. main is parked in fgets and will not come back on its own, because a
 * console control event does not close stdin, so the whole shutdown happens
 * here and the process leaves from here too.
 */
static BOOL WINAPI console_handler(DWORD type)
{
	(void)type;

	g_keep_running = 0;
	printf("\nqwark-rpcs3: stopping\n");
	fflush(stdout);

	shutdown_all();
	ExitProcess(0);
	return TRUE;
}
#endif

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
	printf("usage: qwark-rpcs3 [--pine-port N] [--port N] [--root DIR]\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	char line[1024];
	int qwark_port = QWARK_PORT;
	int pine = PINE_DEFAULT_PORT;
	const char *root = NULL;
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 1; i < argc; i++) {
		if (qstreq(argv[i], "--pine-port") && i + 1 < argc) {
			if (!parse_int(argv[++i], &pine)) { usage(); return 2; }
		} else if (qstreq(argv[i], "--port") && i + 1 < argc) {
			if (!parse_int(argv[++i], &qwark_port)) { usage(); return 2; }
		} else if (qstreq(argv[i], "--root") && i + 1 < argc) {
			root = argv[++i];
		} else {
			usage();
			return 2;
		}
	}

	pine_set_port(pine);
	pine_set_root(root);
	net_set_port((u16)qwark_port);

	if (plat_init() != 0) {
		fprintf(stderr, "qwark-rpcs3: plat_init failed\n");
		return 1;
	}

	if (session_init() != ST_OK) {
		fprintf(stderr, "qwark-rpcs3: session_init failed\n");
		return 1;
	}

	if (net_init() != ST_OK) {
		fprintf(stderr, "qwark-rpcs3: net_init failed\n");
		return 1;
	}

	if (plat_thread_create(&g_tick, session_tick_thread, NULL, 49152, "qwark_tick") != 0) {
		fprintf(stderr, "qwark-rpcs3: cannot start the tick thread\n");
		return 1;
	}

	if (plat_thread_create(&g_net, net_accept_thread, NULL, 16384, "qwark_net") != 0) {
		fprintf(stderr, "qwark-rpcs3: cannot start the accept thread\n");
		return 1;
	}

#ifdef _WIN32
	SetConsoleCtrlHandler(console_handler, TRUE);
#endif

	printf("qwark-rpcs3 protocol %d build %d: client port %d, PINE 127.0.0.1:%d, root %s\n",
	       QWARK_PROTOCOL_VERSION, QWARK_BUILD, (int)net_port(), pine_port(), host_root());
	printf("code patches are refused on RPCS3; type 'status', 'version' or 'exit'\n");
	fflush(stdout);

	while (g_keep_running && fgets(line, sizeof(line), stdin) != NULL) {
		char *cmd = qtrim(line);

		if (qstreq(cmd, "status")) {
			print_status();
		} else if (qstreq(cmd, "version")) {
			print_version();
		} else if (qstreq(cmd, "exit") || qstreq(cmd, "quit")) {
			printf("ok exit\n");
			fflush(stdout);
			break;
		} else if (cmd[0] != 0) {
			printf("err unknown command\n");
			fflush(stdout);
		}
	}

	shutdown_all();
	return 0;
}
