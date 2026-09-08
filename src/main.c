/*
 * qwark module entry.
 *
 * The module entry point runs on webMAN's VSH-plugin loader thread, in a
 * restricted context with a small stack. Every webMAN-derived module that loads
 * cleanly (Ratchetron, both autosplitters) does nothing here but spawn one
 * worker and return; the heavy setup (kernel objects, sockets, filesystem) runs
 * on that worker. qwark now does the same: qwark_start creates the boot thread
 * and exits, and qwark_boot_thread does the init and then becomes the accept
 * loop.
 *
 * A plat_trace() line goes to the PS3 TTY (Target Manager / ProDG) at every
 * step, so a fault in bring-up names the step it died on. plat_trace is a bare
 * sys_tty_write with no file, mutex or allocation, so it is safe this early.
 *
 * Stop follows Ratchetron's pattern: break every socket first so the threads
 * come back out of accept()/recv() on their own, then join through a stop
 * thread that gives them time to wind down, then unload.
 */
#include "plat/ps3/types.h"
#define QWARK_PLAT_TYPES_PROVIDED
#include "plat/plat.h"

#include "plat/ps3/qwark_ps3.h"
#include "plat/ps3/vsh.h"
#include "plat/ps3/process.h"
#include "plat/ps3/timer.h"

#include "core/session.h"
#include "core/net.h"

int qwark_start(size_t args, void *argp);
int qwark_stop(void);

SYS_MODULE_INFO(qwark, 0, 1, 1);
SYS_MODULE_START(qwark_start);
SYS_MODULE_STOP(qwark_stop);
SYS_MODULE_EXIT(qwark_stop);

sys_ppu_thread_t qwark_thread_tick = SYS_PPU_THREAD_NONE;
sys_ppu_thread_t qwark_thread_net  = SYS_PPU_THREAD_NONE;

static plat_thread_t g_boot = PLAT_THREAD_NONE;
static plat_thread_t g_tick = PLAT_THREAD_NONE;

/*
 * The worker. Runs off the loader thread, so it may create kernel objects,
 * sockets and touch the filesystem. It does the init, starts the tick thread,
 * then runs the accept loop itself until stop closes the sockets.
 */
static void qwark_boot_thread(void *arg)
{
	(void)arg;

	plat_trace("qwark: boot thread up");

	if (plat_init() != 0) {
		plat_trace("qwark: plat_init FAILED");
		show_msg("qwark: platform init failed");
		plat_thread_exit();
		return;
	}
	plat_trace("qwark: plat_init ok");

	if (session_init() != 0) {
		plat_trace("qwark: session_init FAILED");
		show_msg("qwark: session init failed");
		plat_thread_exit();
		return;
	}
	plat_trace("qwark: session_init ok");

	if (net_init() != 0) {
		plat_trace("qwark: net_init FAILED");
		show_msg("qwark: network init failed");
		plat_thread_exit();
		return;
	}
	plat_trace("qwark: net_init ok");

	if (plat_thread_create(&g_tick, session_tick_thread, NULL,
	                       QWARK_STACK_TICK, THREAD_NAME_TICK) != 0) {
		plat_trace("qwark: tick thread create FAILED");
		show_msg("qwark: cannot start the tick thread");
	} else {
		qwark_thread_tick = (sys_ppu_thread_t)g_tick;
		plat_trace("qwark: tick thread ok");
	}

	plat_trace("qwark: entering accept loop");
	show_msg("qwark loaded and listening.");

	/* Runs until net_stop() closes the listener; then we fall through. */
	net_accept_thread(NULL);

	plat_trace("qwark: accept loop exited");
	plat_thread_exit();
}

int qwark_start(size_t args, void *argp)
{
	(void)args;
	(void)argp;

	plat_trace("qwark: module entry");

	if (plat_thread_create(&g_boot, qwark_boot_thread, NULL,
	                       QWARK_STACK_NET, THREAD_NAME_NET) != 0) {
		plat_trace("qwark: boot thread create FAILED");
		show_msg("qwark: cannot start");
	} else {
		qwark_thread_net = (sys_ppu_thread_t)g_boot;
	}

	_sys_ppu_thread_exit(0);

	return SYS_PRX_RESIDENT;
}

void qwark_stop_server(void)
{
	qwark_working = 0;
	net_stop();
	session_stop();
}

static void qwark_stop_thread(u64 arg)
{
	(void)arg;

	/* Give the tick and client threads a moment before we join. */
	sys_ppu_thread_sleep(2);

	if (g_boot != PLAT_THREAD_NONE) plat_thread_join(g_boot);
	if (g_tick != PLAT_THREAD_NONE) plat_thread_join(g_tick);

	sys_ppu_thread_exit(0);
}

int qwark_stop(void)
{
	sys_ppu_thread_t t_id;
	u64 exit_code;

	qwark_stop_server();

	if (sys_ppu_thread_create(&t_id, qwark_stop_thread, 0, THREAD_PRIO_STOP,
	                          QWARK_STACK_STOP, SYS_PPU_THREAD_CREATE_JOINABLE,
	                          THREAD_NAME_STOP) == CELL_OK) {
		sys_ppu_thread_join(t_id, &exit_code);
	}

	sys_ppu_thread_usleep(500000);

	plat_shutdown();

	unload_prx_module();

	_sys_ppu_thread_exit(0);

	show_msg("qwark unloaded!");

	return SYS_PRX_STOP_OK;
}
