/*
 * The TCP server on 9673, the frame codec and the UDP telemetry fan-out.
 *
 * One accept thread plus at most QWARK_MAX_CLIENTS connection threads on 16 KB
 * stacks. A handler either answers on its own thread (files, config, lists,
 * DESCRIBE) under the core lock, or posts the command into the session ring and
 * waits for the tick thread.
 */
#ifndef QWARK_NET_H
#define QWARK_NET_H

#include "proto.h"

int  net_init(void);

/* The accept loop. Runs until net_stop(). */
void net_accept_thread(void *arg);

/*
 * Breaks every blocking socket call so the threads come back on their own, the
 * way Ratchetron's stop_ratchetron_server does. Call before joining.
 */
void net_stop(void);

/* Called from the tick thread every fourth tick. */
void net_send_telemetry(const u8 *packet, u32 len);

#endif /* QWARK_NET_H */
