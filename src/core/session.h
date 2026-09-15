/*
 * The session state machine, the 120 Hz tick loop and the command ring.
 *
 * The tick thread is the only thread that ever calls plat_mem_read /
 * plat_mem_write. Network threads post a command into the ring and wait on a
 * per-slot semaphore; the tick thread drains the ring every tick, between the
 * state check and the memory work.
 */
#ifndef QWARK_SESSION_H
#define QWARK_SESSION_H

#include "proto.h"
#include "../games/game.h"

/* --------------------------------------------------------- the command ring */

struct ring_cmd {
	u16 op;
	const u8 *req;
	u32 reqlen;
	u8 *reply;
	u32 replycap;
	u32 replylen;   /* out */
	u16 status;     /* out */
	void *user;     /* the connection, for handlers that need it */

	/*
	 * Out, and only for the op trace: what running this command cost the game.
	 * Every one of these is a PS3MAPI call against the running process, made
	 * inside a single tick, so a command with hundreds of them is a tick the
	 * game can feel.
	 */
	u32 mem_reads;
	u32 mem_writes;
	u32 exec_us;
};

typedef void (*ring_exec_fn)(struct ring_cmd *cmd);

/* net.c registers the dispatcher the tick thread runs commands through. */
void session_set_ring_exec(ring_exec_fn fn);

/*
 * Posts a command and blocks until the tick thread has run it. Returns ST_OK
 * once cmd->status is filled in, or ST_BUSY straight away when the ring is full.
 */
int  session_submit(struct ring_cmd *cmd);

/* ------------------------------------------------------------- the session */

int  session_init(void);
void session_tick_thread(void *arg);

/* One iteration of the tick loop, so the host tests can drive it deterministically. */
void session_step_once(void);
void session_stop(void);
int  session_running(void);

/*
 * Lock before core_lock; never hold it while waiting for a ring command, and
 * never across anything slow. It serializes boot detection with the moments a
 * network thread touches its socket or decides to dispatch a frame, and with
 * nothing else: a handler, a file operation or a log line runs without it,
 * because the tick thread takes it every tick and whatever is held under it is
 * a stall of the freezes, the combos and the telemetry. session_quiet() is read
 * under this lock by network threads, or directly by the owning tick.
 */
void session_activity_lock(void);
void session_activity_unlock(void);
int  session_quiet(void);

/*
 * Destroys the mutexes and the ring semaphores session_init created. Call only
 * after the tick thread has been joined; nothing may enter the session code
 * afterwards.
 */
void session_shutdown(void);

u8   session_state(void);
u32  session_generation(void);
u32  session_tick_count(void);
const char *session_title(void);
const struct game_api *session_game(void);

/*
 * Whether every block of a game's hot table gets a slice of the shared pool the
 * tick thread reads them into. Nothing in the running module calls it: it is
 * there so the unit tests can assert it of every registered game, because a
 * table that outgrows the pool loses its last blocks and a game that quietly
 * stops decoding half its state is a bad way to find that out.
 */
int  session_hot_fits(const struct game_api *g);

/* Copies the published SessionInfo (164 bytes) or the full telemetry packet. */
u32  session_info_copy(u8 *out, u32 cap);
u32  session_telemetry_copy(u8 *out, u32 cap);

/* ------------------------------------------------- the previous-session record */

struct prev_freeze {
	u32 addr;
	u64 value;
	u8  size;
};

struct prev_patch {
	u32 first_addr;
	u16 nwords;
};

#define PREV_MAX_PATCHES 16

struct previous_record {
	u64 toggles;
	u32 mods;
	u8  nfreeze;
	struct prev_freeze freezes[QWARK_MAX_FREEZES];
	u8  npatch;
	struct prev_patch patches[PREV_MAX_PATCHES];
	int pending;
};

const struct previous_record *session_previous(void);
void session_previous_reapply(u8 flags);   /* tick thread only */
void session_previous_dismiss(void);

/* ------------------------------------------------------------- the core lock */

/*
 * Guards everything that is not game memory: the descriptor registry, the mod
 * table, config, the published telemetry buffers. Held for microseconds at a
 * time by the tick thread, and by network threads answering list ops.
 */
void core_lock(void);
void core_unlock(void);

/* Position and planet helpers the combo actions and the net handlers share. */
int  session_position_save(u8 slot);
int  session_position_load(u8 slot);
int  session_planet_load(u8 planet, u8 flags);
int  session_die(void);
int  session_load_setaside(void);
u8   session_current_planet(void);

/* ----------------------------------------------------------------- combos */

/*
 * Protocol 1.8. COMBO_SUSPEND: holds every stored combo off (`on` non-zero) or
 * lets them go again (0). A client capturing a new combo reads the pad out of
 * telemetry while the console is watching the same pad, so without this the
 * buttons being recorded also fire whatever is already stored.
 *
 * The hold carries a deadline rather than lasting until a client says otherwise,
 * so a client that dies mid-capture cannot leave the combos off for good, and it
 * is dropped when the session leaves the game. Tick thread only: the net layer
 * posts it through the ring.
 */
void session_combo_suspend(u8 on);

/* How long a hold lasts before the console takes the combos back, microseconds. */
u64  session_combo_suspend_window_us(void);

/* Test hook: shortens that window so the expiry is reachable in a unit test. */
void session_set_combo_suspend_window_us(u64 us);

#endif /* QWARK_SESSION_H */
