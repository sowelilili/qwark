/*
 * The autosplit event stream, protocol 1.5.
 *
 * A game's watcher runs inside its on_tick and calls autosplit_emit() whenever
 * it sees a run start, a split candidate, a reset, a loading screen coming or
 * going, or (Deadlocked only) a pause around a quit to the XMB. qwark keeps no
 * timer and applies no user setting: every candidate is emitted and the PC
 * decides what reaches LiveSplit. Each event is stamped with the module's
 * millisecond clock, so the client can measure a load or a pause without knowing
 * anything about the tick rate.
 *
 * Each event is stamped with a sequence number that counts from 1 for the life
 * of the module and never resets, not even when the game reboots, so a client
 * can ask for everything it missed with AUTOSPLIT_EVENTS. The last 64 events are
 * kept; the same event also goes out as a 20-byte UDP datagram on the tick it
 * happened and on the two ticks after it, so a dropped datagram costs latency
 * rather than a split.
 */
#ifndef QWARK_AUTOSPLIT_H
#define QWARK_AUTOSPLIT_H

#include "proto.h"

int  autosplit_init(void);
void autosplit_shutdown(void);

/*
 * Tick thread only, and only while the session is INGAME: anything emitted from
 * another state is dropped. `code` and `arg` are forced to 0 for START and
 * RESET; every other kind carries the reason code of the row it belongs to.
 * Records the event and queues its datagram; the sends happen in autosplit_push.
 */
void autosplit_emit(u8 kind, u8 code, u32 arg);

/* Tick thread only. Sends whatever datagrams are still due, once per tick. */
void autosplit_push(void);

/*
 * Encodes the AUTOSPLIT_EVENTS reply body into `out`:
 *
 *     u32 latest_seq, u8 n, Event[n]
 *
 * where Event is 16 bytes and the n events are those with seq > since_seq that
 * the ring still holds, oldest first. since_seq 0 asks for everything. Returns
 * the number of bytes written, or 0 when `cap` cannot hold the whole ring.
 * Safe from any thread.
 */
u32  autosplit_encode_events(u32 since_seq, u8 *out, u32 cap);

/* The sequence number of the last event emitted, 0 before the first one. */
u32  autosplit_latest_seq(void);

#endif /* QWARK_AUTOSPLIT_H */
