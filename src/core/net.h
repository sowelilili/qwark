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

/* Tick thread: serialize the launch allocation gate with buffer acquisition.
 * Cancels connections holding bulk buffers; their owners release them safely. */
void net_set_booting(int booting);

/*
 * Moves the command port off QWARK_PORT. Call before net_accept_thread starts.
 * The SPRX never does: the console always listens on 9673. qwark-rpcs3.exe
 * offers --port so two emulator sessions can run side by side on one PC.
 */
void net_set_port(u16 port);
u16  net_port(void);

/* The accept loop. Runs until net_stop(). */
void net_accept_thread(void *arg);

/*
 * Breaks every blocking socket call so the threads come back on their own, the
 * way Ratchetron's stop_ratchetron_server does. Call before joining.
 */
void net_stop(void);

/*
 * Waits for the connection threads to leave their slots, at most `timeout_us`.
 * Returns 1 when every slot is free, 0 on timeout, in which case any socket
 * still open is closed outright. Call after net_stop and before the module is
 * unloaded, so a detached client thread is not still inside module code.
 */
int  net_wait_clients(u32 timeout_us);

/* Destroys what net_init created. Nothing may be in the network code after this. */
void net_shutdown(void);

/* Called from the tick thread every fourth tick. */
void net_send_telemetry(const u8 *packet, u32 len);

/* How many times the tick has handed it a packet, subscribers or not. For the tests. */
u32  net_telemetry_sends(void);

/* config.txt `trace_ops`: log every request as it arrives and as it is answered. */
void net_set_trace_ops(int on);
int  net_trace_ops(void);

#endif /* QWARK_NET_H */
