/*
 * The PS3 side of the platform seam: every system header the VSH glue needs,
 * plus the thread names and stack sizes qwark uses.
 *
 * Adapted from Ratchetron (webMAN MOD glue, GPL v3). Thread names are deliberately
 * different from Ratchetron's so both can be loaded at once during migration.
 */
#ifndef QWARK_PS3_H
#define QWARK_PS3_H

#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/event.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/memory.h>
#include <sys/timer.h>
#include <sys/process.h>
#include <sys/synchronization.h>
#include <sys/sys_time.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netex/net.h>
#include <netex/errno.h>
#include <netex/libnetctl.h>
#include <netex/sockinfo.h>
#include <netinet/tcp.h>

#include "types.h"
#include "ps3mapi.h"
#include "socket.h"
#include "thread.h"
#include "vsh_notify.h"

#define THREAD_NAME_TICK    "qwark_tick"
#define THREAD_NAME_NET     "qwark_net"
#define THREAD_NAME_CLIENT  "qwark_cli"
#define THREAD_NAME_STOP    "qwark_stop"

/*
 * Thread stacks, measured rather than guessed. The deepest chain each thread can
 * reach is read off the linked module with
 *
 *   ppu-lv2-objdump -d qwark.prx   and the stdu r1,-N(r1) in every prologue
 *
 * following the call graph from the thread's entry point, including the two
 * indirect hops the graph does not show: the command ring's dispatcher and the
 * game vtable.
 *
 *   tick    4864 bytes   session_step -> net_ring_exec -> mods_rescan ->
 *                        plat_dir_next -> snprintf -> vsnprintf
 *   accept   784 bytes   net_accept_thread -> plat_log -> vsnprintf
 *
 * The tick thread wanted 48 KB because loading a mod recursed: mods_load_index
 * and load_dependencies called each other at about 700 bytes a level for as many
 * as 33 levels. Build 37 walks the dependency graph with an explicit work list
 * in static storage, the recursion is gone, and the deepest chain is under
 * 5 KB. 16 KB is 3.4 times it and the smallest size the SDK's ladder offers
 * above two and a half times it; 8 KB is ten times what the accept thread uses.
 *
 * The connection threads keep 16 KB (CLIENT_STACK in net.c): their deepest chain
 * is a recursive directory delete at about 12 KB, and savefile_list, which
 * inlines a 4 KB buffer of its own, is close behind it. Neither can move to the
 * shared scratch buffer, which belongs to the tick thread.
 */
#define QWARK_STACK_TICK    THREAD_STACK_SIZE_16KB
#define QWARK_STACK_NET     THREAD_STACK_SIZE_8KB
#define QWARK_STACK_STOP    THREAD_STACK_SIZE_6KB

#define CELL_FS_O_CREAT         000100
#define CELL_FS_O_EXCL          000200
#define CELL_FS_O_TRUNC         001000
#define CELL_FS_O_APPEND        002000
#define CELL_FS_O_ACCMODE       000003
#define CELL_FS_O_RDONLY        000000
#define CELL_FS_O_RDWR          000002
#define CELL_FS_O_WRONLY        000001

static volatile u8 qwark_working __attribute__((unused)) = 1;

/* The tick thread and the accept thread, created by SYS_MODULE_START. */
extern sys_ppu_thread_t qwark_thread_tick;
extern sys_ppu_thread_t qwark_thread_net;

/* Breaks every blocking socket call, then stops the tick loop. */
void qwark_stop_server(void);

#endif /* QWARK_PS3_H */
