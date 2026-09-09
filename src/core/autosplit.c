/*
 * The autosplit ring and the UDP push. See autosplit.h for the contract and
 * docs/PROTOCOL.md section 8 for the wire format.
 *
 * The ring is written by the tick thread and read by connection threads, so it
 * has a mutex of its own rather than borrowing the core lock: emit() is called
 * from inside on_tick, which already runs under the core lock, and from
 * on_quit, which does not.
 */
#include "autosplit.h"
#include "session.h"
#include "net.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * How many events may be waiting to be repeated at once. Three ticks of overlap
 * at 120 Hz is 25 ms, and no game emits anything like eight events in 25 ms;
 * the slot is only ever reused when something has gone very strange.
 */
#define AUTOSPLIT_PENDING 8

struct autosplit_slot {
	u32 seq;
	u32 time_ms;
	u8  kind;
	u8  code;
	u32 arg;
};

struct autosplit_pending {
	u8 dgram[AUTOSPLIT_DGRAM_SIZE];
	u8 left;
};

static struct autosplit_slot    g_ring[AUTOSPLIT_RING_SLOTS];
static struct autosplit_pending g_pending[AUTOSPLIT_PENDING];

static u32 g_seq;      /* the last sequence number handed out, 0 before the first */
static u32 g_next;     /* where the next event goes in the ring */
static u32 g_count;    /* how many slots hold an event, at most the ring size */

static plat_mutex_t g_mutex;
static int g_ready;

int autosplit_init(void)
{
	if (g_ready) return ST_OK;

	if (plat_mutex_init(&g_mutex) != 0) return ST_IO_ERROR;

	memset(g_ring, 0, sizeof(g_ring));
	memset(g_pending, 0, sizeof(g_pending));
	g_seq = 0;
	g_next = 0;
	g_count = 0;
	g_ready = 1;
	return ST_OK;
}

void autosplit_shutdown(void)
{
	if (!g_ready) return;
	g_ready = 0;
	plat_mutex_destroy(&g_mutex);
}

/* Lays one event out the way the wire wants it: the same 16 bytes everywhere. */
static void encode_event(u8 *out, const struct autosplit_slot *e)
{
	be32_put(out + 0, e->seq);
	be32_put(out + 4, e->time_ms);
	out[8]  = e->kind;
	out[9]  = e->code;
	be16_put(out + 10, 0);
	be32_put(out + 12, e->arg);
}

static void queue_dgram(const struct autosplit_slot *e)
{
	int slot = -1;
	int i;

	for (i = 0; i < AUTOSPLIT_PENDING; i++) {
		if (g_pending[i].left == 0) { slot = i; break; }
	}

	/*
	 * Nothing free: take the entry with the fewest sends left, which is the one
	 * whose datagram has already gone out the most times.
	 */
	if (slot < 0) {
		slot = 0;
		for (i = 1; i < AUTOSPLIT_PENDING; i++) {
			if (g_pending[i].left < g_pending[slot].left) slot = i;
		}
	}

	memcpy(g_pending[slot].dgram, AUTOSPLIT_MAGIC, 2);
	g_pending[slot].dgram[2] = AUTOSPLIT_DGRAM_VERSION;
	g_pending[slot].dgram[3] = 0;
	encode_event(g_pending[slot].dgram + 4, e);
	g_pending[slot].left = AUTOSPLIT_REPEATS;
}

void autosplit_emit(u8 kind, u8 code, u32 arg)
{
	struct autosplit_slot e;

	if (!g_ready) return;

	/*
	 * Only a running game produces run events. enter_quitting calls on_quit
	 * before it moves the state, so Deadlocked's PAUSE still counts as INGAME.
	 */
	if (session_state() != SESSION_INGAME) return;

	/*
	 * Revision 1.5: only the two kinds that describe the run as a whole are
	 * codeless. A pause, a resume and either end of a load all name the row they
	 * belong to, so the client knows which timing rule to apply.
	 */
	if (kind == AUTOSPLIT_START || kind == AUTOSPLIT_RESET) { code = 0; arg = 0; }

	plat_mutex_lock(&g_mutex);

	g_seq++;
	e.seq  = g_seq;
	/*
	 * Milliseconds since the module started rather than a tick count: the client
	 * measures a load or a pause from these, and must not have to know the tick
	 * rate to do it. It wraps every 49 days, and a client subtracting two of them
	 * in u32 arithmetic gets the right answer across the wrap.
	 */
	e.time_ms = (u32)(plat_time_us() / 1000u);
	e.kind = kind;
	e.code = code;
	e.arg  = arg;

	g_ring[g_next] = e;
	g_next = (g_next + 1) % AUTOSPLIT_RING_SLOTS;
	if (g_count < AUTOSPLIT_RING_SLOTS) g_count++;

	queue_dgram(&e);

	plat_mutex_unlock(&g_mutex);
}

void autosplit_push(void)
{
	u8  out[AUTOSPLIT_PENDING][AUTOSPLIT_DGRAM_SIZE];
	int n = 0;
	int i;

	if (!g_ready) return;

	plat_mutex_lock(&g_mutex);
	for (i = 0; i < AUTOSPLIT_PENDING; i++) {
		if (g_pending[i].left == 0) continue;
		memcpy(out[n], g_pending[i].dgram, AUTOSPLIT_DGRAM_SIZE);
		n++;
		g_pending[i].left--;
	}
	plat_mutex_unlock(&g_mutex);

	/* The sends happen outside the lock: sendto takes the network mutex. */
	for (i = 0; i < n; i++) net_send_telemetry(out[i], AUTOSPLIT_DGRAM_SIZE);
}

u32 autosplit_latest_seq(void)
{
	u32 seq;

	if (!g_ready) return 0;

	plat_mutex_lock(&g_mutex);
	seq = g_seq;
	plat_mutex_unlock(&g_mutex);
	return seq;
}

u32 autosplit_encode_events(u32 since_seq, u8 *out, u32 cap)
{
	u32 off = 5;
	u32 i;
	u32 first;
	u8  n = 0;

	if (out == NULL) return 0;
	if (cap < 5u + (u32)AUTOSPLIT_RING_SLOTS * AUTOSPLIT_EVENT_SIZE) return 0;

	if (!g_ready) {
		be32_put(out, 0);
		out[4] = 0;
		return 5;
	}

	plat_mutex_lock(&g_mutex);

	be32_put(out, g_seq);

	/* Oldest first: the ring is a plain wrap, so start `g_count` back from the head. */
	first = (g_next + AUTOSPLIT_RING_SLOTS - g_count) % AUTOSPLIT_RING_SLOTS;
	for (i = 0; i < g_count; i++) {
		const struct autosplit_slot *e =
			&g_ring[(first + i) % AUTOSPLIT_RING_SLOTS];

		if (e->seq <= since_seq) continue;

		encode_event(out + off, e);
		off += AUTOSPLIT_EVENT_SIZE;
		n++;
	}

	plat_mutex_unlock(&g_mutex);

	out[4] = n;
	return off;
}
