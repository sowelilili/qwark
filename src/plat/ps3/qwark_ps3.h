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

#define QWARK_STACK_TICK    THREAD_STACK_SIZE_48KB
#define QWARK_STACK_NET     THREAD_STACK_SIZE_16KB
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
