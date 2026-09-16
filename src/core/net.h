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

/* Tick thread: serialize the launch gate with the request arena being taken.
 * Cancels the connection holding the arena; its owner releases it safely. */
void net_set_booting(int booting);

/*
 * The shared request arena, revision 1.11. One 16 KB request buffer and one
 * 16 KB reply buffer in bss, held by one connection at a time for the life of
 * one request; the control ops never touch it. These two are for the tests and
 * the simulator's console: `held` is 1 while somebody owns it and `takes` counts
 * how many times it has ever been taken. Nothing in the module allocates memory
 * from the console any more, so both replace the old page counters.
 */
u32  net_arena_held(void);
u32  net_arena_takes(void);

/*
 * Moves the command port off QWARK_PORT. Call before net_accept_thread starts.
 * The SPRX never does: the console always listens on 9673. Both PC builds offer
 * --port, so two emulator sessions can run side by side on one PC, and both test
 * suites pick a free port with it so a run never lands on a helper that is
 * already serving the user's client.
 */
void net_set_port(u16 port);
u16  net_port(void);

#ifdef QWARK_HOST
/*
 * Moves the listener off 127.0.0.1, on the PC builds only. `addr` is a dotted
 * quad; returns 1 when it parsed and 0 when it did not, in which case the bind
 * address is unchanged. Call before net_accept_thread starts.
 *
 * The console listens on every interface and has no such call: only a PC build
 * binds loopback, to keep Windows Firewall out of every rebuild, and only
 * --bind 0.0.0.0 puts a PC build back on the network.
 */
int  net_set_bind(const char *addr);
#endif

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

#endif /* QWARK_NET_H */
