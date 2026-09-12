#include "session.h"
#include "mem.h"
#include "features.h"
#include "mods.h"
#include "config.h"
#include "net.h"
#include "util.h"
#include "autosplit.h"
#include "savefile.h"
#include "../plat/plat.h"

#include <string.h>

#define TICK_PERIOD_US   8333u     /* 120 Hz */
#define TELEMETRY_EVERY  4         /* 30 Hz */

/*
 * Outside a game there is nothing moving to report, and one of the times there
 * is nothing moving is while a game is starting, which is the worst moment to
 * be busy on the network: this module lives in the VSH, and a VSH plugin
 * working the network stack while the console hands over to a game is the other
 * half of the crash the boot window below is about. So the packet drops to
 * 10 Hz whenever the session is not INGAME.
 *
 * It does not stop. A client that has heard nothing for 400 ms starts asking
 * for the same snapshot over TCP instead, which is more traffic at a worse
 * moment, so this has to stay comfortably inside that.
 */
#define TELEMETRY_EVERY_QUIET 12   /* 10 Hz */

/*
 * Protocol 1.3. How often the FEATURE_FLAG_LIVE toggles are re-read out of game
 * memory, in ticks: 12 is 10 Hz, which is three telemetry frames apart and
 * costs one small read per live toggle. A checkbox that follows the save file
 * does not need to be any quicker than the eye.
 */
#define LIVE_POLL_EVERY  12        /* 10 Hz */

/*
 * How long qwark keeps away from a process that has just appeared.
 *
 * A second was not enough. IS_INGAME goes true when the VSH has handed over to
 * the game, which is before the game has finished building itself, and reading
 * a process in that state is how a console panics. It was survivable on a fast
 * console over Ethernet and it crashed reliably on a slower one over WiFi.
 *
 * The number is the platform's, since it is the platform that knows what it
 * costs to be wrong: see plat_boot_settle_ticks. It is counted in ticks rather
 * than microseconds so that a console too busy to hold 120 Hz waits longer
 * rather than less, and so the host tests can step through the window instead
 * of sleeping through it. config.txt's `boot_delay_ms` overrides it, in the
 * milliseconds somebody tuning it would think in.
 */
#define BOOT_SETTLE_MS_DEFAULT    8300u

/*
 * Then the fingerprint, four times a second rather than at the full tick rate.
 * Under BCES01503 there are three candidates to try, so a tick used to cost the
 * booting process three reads; at 120 Hz that is 360 reads a second thrown at
 * something that may still be mapping itself. Nothing is waiting on the answer
 * to the millisecond: the game is still on its own loading screen.
 */
#define BOOT_FINGERPRINT_EVERY 30u   /* 4 Hz */

/*
 * And the fingerprint has to agree with itself. One read that happens to land
 * while the image is being mapped could match on a page that is not finished;
 * three quarters of a second of the same answer is cheap proof that the process
 * has settled at the address the game is meant to live at.
 */
#define BOOT_FINGERPRINT_MATCHES 3u

/*
 * BOOTING sends nothing at all. Build 17 spent half a second announcing the
 * silence first, on the grounds that a client cannot honour a window it never
 * heard about; a console died inside those packets, so the announcement is
 * gone. A client works the silence out from the state it last saw: XMB, and
 * then nothing, is a game starting.
 *
 * Except for a client that does not know that and polls GET_STATE to fill the
 * gap, which is what every client before revision 1.11 does after 400 ms. That
 * costs a TCP round trip each time, several times worse than the packet it is
 * replacing, so if one arrives while BOOTING the silence is called off for the
 * rest of this boot and the cheaper thing happens instead. Nothing is asked of
 * the client and nothing is taken on trust: it is answered by what it does.
 */
static volatile int g_quiet_broken;

/* ------------------------------------------------------- the settle after that */

/*
 * INGAME says the game is mapped and its fingerprint is where it belongs. It
 * does not say the game has finished starting: it is still opening its PRXs,
 * setting up the RSX and reading the disc. The one thing qwark can ask about
 * that, without reading the process, is how many modules the process has, and
 * a count that has stopped moving is a game that has stopped loading.
 *
 * So the writes that are big enough to matter (a mod's code cave, the savefile
 * helper's) wait for that count to hold still, floor and ceiling either side:
 * never sooner than the platform's floor, never later than SETTLE_MAX_TICKS, and on
 * a platform with no module list the floor is the whole of it.
 */
#define SETTLE_MAX_TICKS  3600u   /* 30 s, a ceiling so nothing waits forever */
#define SETTLE_POLL_EVERY   30u   /* 4 Hz */
#define SETTLE_SAME_POLLS    4u   /* a second of the same answer */

/*
 * Sixteen blocks, which is more than any game needs: RaC3 uses twelve, eight of
 * them per-tick reads once its autosplit watcher is counted, and four slow ones
 * for ship colour, file time, the savefile helper and the chargeboot colours.
 * The slow blocks carry a period and a phase, so a tick pays for the per-tick
 * reads plus at most one of them.
 */
#define HOT_MAX_BLOCKS   16
#define HOT_MAX_LEN      1024

/* ------------------------------------------------------------------- state */

static volatile int g_running = 1;

static u8   g_state = SESSION_XMB;
static u32  g_generation;
static u32  g_tick;
static u32  g_pid;

/* The boot window: how many ticks BOOTING has had, how many it owes, and how
 * many times running the fingerprint has given the same answer. */
/*
 * Before any of that: the process id qwark has seen but not yet asked the XMB
 * about. A game appearing is the XMB tearing itself down and building the game
 * up, and the title has to come from a plugin inside that XMB, so the question
 * waits until the same id has been there a while. See the XMB case below.
 */
static u32  g_pre_pid;
static u32  g_pre_tick;

static u32  g_boot_ticks;       /* ticks since BOOTING began, for the packet */
static u32  g_boot_start_tick;  /* the tick it began on */
static u32  g_fp_last_tick;     /* the tick the fingerprint last ran on */
static u32  g_boot_settle;
static u32  g_fp_matches;
static const struct game_api *g_fp_game;


/*
 * And the window after that one. INGAME means the game is mapped and running;
 * it does not mean the game has finished starting, and the biggest writes qwark
 * makes are the worst thing to do while it has not. See settled_tick.
 */
static u8   g_settled;
static u32  g_settled_ticks;
static int  g_module_count;
static u32  g_module_same;

static char g_title[16];
static char g_last_title[16];
static u8   g_last_game_id;
static const struct game_api *g_game;

/*
 * BCES01503 hosts RaC1, RaC2 and RaC3, so a title can hand back more than one
 * candidate and BOOTING fingerprints each in turn. For every other title the
 * list is one long and this costs one extra pointer.
 */
static const struct game_api *g_candidates[GAME_MAX_CANDIDATES];
static u32 g_ncandidates;

static struct game_hot g_hot;
static u8   g_hotbuf[HOT_MAX_BLOCKS][HOT_MAX_LEN];
static int  g_hot_primed;

static struct previous_record g_prev;

static int  g_combo_armed = 1;

/*
 * Protocol 1.8. While the client captures a combo it holds the console's own
 * combos off, because the buttons the user is recording are the same pad the
 * tick thread watches. The hold is a deadline, not a flag: a client that
 * crashes with the hold on is gone, and nobody would be left to lift it.
 * Two minutes is far longer than a capture and far shorter than a session.
 */
#define COMBO_SUSPEND_WINDOW_US 120000000ull   /* 120 seconds */

static u64 g_combo_suspend_until;
static u64 g_combo_suspend_window = COMBO_SUSPEND_WINDOW_US;

static plat_mutex_t g_core_mutex;
static plat_mutex_t g_ring_mutex;

static u8  g_info_pub[SESSION_INFO_SIZE];
static u8  g_tele_pub[TELEMETRY_MAX];
static u32 g_tele_pub_len;

void core_lock(void)   { plat_mutex_lock(&g_core_mutex); }
void core_unlock(void) { plat_mutex_unlock(&g_core_mutex); }

u8   session_state(void)      { return g_state; }

/*
 * A client asked for the snapshot over TCP. Harmless in itself, and during a
 * boot it is the thing the silence exists to avoid, so the silence gives way:
 * a packet costs this console less than answering the poll that replaces it.
 */
void session_note_state_poll(void)
{
	if (g_state == SESSION_BOOTING && !g_quiet_broken) {
		g_quiet_broken = 1;
		plat_log("qwark: a client polled during the boot, so telemetry resumes");
	}
}
int  session_settled(void)    { return (g_state == SESSION_INGAME) && g_settled; }
u32  session_generation(void) { return g_generation; }
u32  session_tick_count(void) { return g_tick; }
const char *session_title(void) { return g_title; }
const struct game_api *session_game(void) { return g_game; }
int  session_running(void)    { return g_running; }
u8   session_current_planet(void) { return g_hot.current_planet; }

const struct previous_record *session_previous(void) { return &g_prev; }

/* -------------------------------------------------------- the command ring */

#define RING_FREE    0
#define RING_PENDING 1
#define RING_DONE    2

struct ring_slot {
	struct ring_cmd *cmd;
	plat_sem_t sem;
	volatile u8 state;
};

static struct ring_slot g_ring[QWARK_RING_SLOTS];
static ring_exec_fn g_ring_exec;

/*
 * Set under g_ring_mutex by the tick thread on its way out, after it has
 * released everything still parked. A submit that sees it answers BUSY instead
 * of waiting on a semaphore nobody will ever post.
 */
static volatile int g_ring_closed;

void session_set_ring_exec(ring_exec_fn fn)
{
	g_ring_exec = fn;
}

int session_submit(struct ring_cmd *cmd)
{
	int slot = -1;
	int i;

	plat_mutex_lock(&g_ring_mutex);
	if (!g_ring_closed && g_running) {
		for (i = 0; i < QWARK_RING_SLOTS; i++) {
			if (g_ring[i].state == RING_FREE) { slot = i; break; }
		}
		if (slot >= 0) {
			g_ring[slot].cmd = cmd;
			g_ring[slot].state = RING_PENDING;
		}
	}
	plat_mutex_unlock(&g_ring_mutex);

	if (slot < 0) {
		/*
		 * A full ring never blocks a network thread, and neither does a tick
		 * thread that has stopped: both answer BUSY and the client retries.
		 */
		cmd->status = ST_BUSY;
		cmd->replylen = 0;
		return ST_BUSY;
	}

	plat_sem_wait(&g_ring[slot].sem);

	g_ring[slot].cmd = NULL;
	g_ring[slot].state = RING_FREE;
	return ST_OK;
}

static void ring_drain(void)
{
	int i;

	for (i = 0; i < QWARK_RING_SLOTS; i++) {
		struct ring_cmd *cmd;

		if (g_ring[i].state != RING_PENDING) continue;

		cmd = g_ring[i].cmd;
		cmd->status = ST_UNKNOWN_OP;
		cmd->replylen = 0;

		if (g_ring_exec != NULL) g_ring_exec(cmd);

		g_ring[i].state = RING_DONE;
		plat_sem_post(&g_ring[i].sem);
	}
}

/* --------------------------------------------------- previous-session record */

static void prev_clear(void)
{
	memset(&g_prev, 0, sizeof(g_prev));
}

static void prev_build(void)
{
	u32 i;
	u8 n;

	prev_clear();

	/*
	 * A LIVE toggle is a byte in the game and in the save file, not something
	 * qwark applied, so there is nothing to offer back: it comes into the new
	 * session already carrying whatever the save says.
	 */
	g_prev.toggles = features_toggle_state() & ~features_live_mask();
	g_prev.mods    = mods_loaded_mask();

	n = 0;
	for (i = 0; i < QWARK_MAX_FREEZES; i++) {
		const struct freeze_entry *f = freeze_slot((u8)i);
		if (f == NULL) continue;
		g_prev.freezes[n].addr  = f->addr;
		g_prev.freezes[n].size  = f->size;
		g_prev.freezes[n].value = f->value;
		n++;
	}
	g_prev.nfreeze = n;

	n = 0;
	for (i = 0; i < client_patch_count() && n < PREV_MAX_PATCHES; i++) {
		const struct patch_def *d = client_patch_at(i);
		if (d == NULL) continue;
		g_prev.patches[n].first_addr = d->words[0].addr;
		g_prev.patches[n].nwords     = d->count;
		n++;
	}
	g_prev.npatch = n;
}

static void prev_update_pending(void)
{
	g_prev.pending = (g_prev.toggles != 0 || g_prev.mods != 0 ||
	                  g_prev.nfreeze != 0 || g_prev.npatch != 0);
}

void session_previous_reapply(u8 flags)
{
	u32 i;

	if (flags & PREV_TOGGLES) features_apply_mask(g_prev.toggles);
	if (flags & PREV_MODS)    mods_apply_mask(g_prev.mods);

	if (flags & PREV_FREEZES) {
		for (i = 0; i < g_prev.nfreeze; i++) {
			freeze_add(g_prev.freezes[i].addr, g_prev.freezes[i].size,
			           g_prev.freezes[i].value, NULL);
		}
	}

	if (flags & PREV_PATCHES) {
		u32 n = client_patch_count();
		for (i = 0; i < n; i++) {
			const struct patch_def *d = client_patch_at(i);
			if (d != NULL) patch_apply(d);
		}
	}

	prev_clear();
	client_patch_gc();
}

void session_previous_dismiss(void)
{
	prev_clear();
	client_patch_gc();
}

/* ------------------------------------------------------- the state machine */

/*
 * The boot window in ticks. config.txt's `boot_delay_ms` is in milliseconds,
 * which is what somebody tuning it thinks in, and a tick is what the loop
 * counts. Zero is allowed and means "as soon as the fingerprint agrees", for a
 * console whose owner would rather have the seconds back than the margin.
 */
static u32 boot_settle_ticks(void)
{
	if (config_get("boot_delay_ms") != NULL) {
		u32 ms = config_get_u32("boot_delay_ms", BOOT_SETTLE_MS_DEFAULT);
		return (u32)(((u64)ms * 1000u) / TICK_PERIOD_US);
	}

	return plat_boot_settle_ticks();
}

static int fingerprint_matches(const struct game_api *g)
{
	u8 buf[64];
	u16 len;

	if (g == NULL) return 0;
	if (g->fp_len == 0 || g->fp_bytes == NULL) return 1;

	len = g->fp_len;
	if (len > sizeof(buf)) len = sizeof(buf);

	/* Before INGAME the gate refuses, so this goes straight at the platform. */
	if (plat_mem_read(g_pid, g->fp_addr, buf, len) != 0) return 0;

	if (memcmp(buf, g->fp_bytes, len) == 0) return 1;

	/* The site may already carry a known patched form; that still proves it. */
	return g->fp_alt != NULL && memcmp(buf, g->fp_alt, len) == 0;
}

/*
 * The candidate whose fingerprint is in memory, or NULL while none is. With one
 * candidate this is the old check; with three it is also the answer to "which of
 * the trilogy is running".
 */
static const struct game_api *fingerprinted_game(void)
{
	u32 i;

	for (i = 0; i < g_ncandidates; i++) {
		if (fingerprint_matches(g_candidates[i])) return g_candidates[i];
	}
	return NULL;
}

static void enter_quitting(void)
{
	if (g_state == SESSION_QUITTING) return;

	/* Memory access stops immediately, before anything else happens. */
	mem_set_context(0, 0);
	watch_invalidate();

	/*
	 * The savefile helper lived in that process and a copy may have been half
	 * way through it. savefile_tick only runs while INGAME, so this is the last
	 * moment anything can tidy up after it: the transfer is stopped here rather
	 * than on the way back in, so a client polling SAVEFILE_INFO is told the
	 * game went away instead of watching a byte count that will never move.
	 */
	savefile_forget();

	if (g_game != NULL && g_game->on_quit != NULL) g_game->on_quit();

	qstrcpy(g_last_title, sizeof(g_last_title), g_title);
	/*
	 * Under BCES01503 the same title id can come back as a different game, so
	 * "same title" is not enough to decide that the feature and position tables
	 * still apply: remember which game it actually was.
	 */
	g_last_game_id = (g_game != NULL) ? g_game->game_id : GAME_NONE;

	/*
	 * A combo hold belongs to the client's capture, and the game it was holding
	 * combos off in has gone. Whatever the client does next starts a new one.
	 */
	g_combo_suspend_until = 0;

	g_state = SESSION_QUITTING;
	net_subs_touch_all();
	plat_log("qwark: session QUITTING (%s)", g_title);
}

/*
 * Position slots are keyed on the game, not the title: BCES01503 hosts three
 * games with different coordinate addresses, and the disc and PSN releases of
 * one game share addresses, so the file is positions/<game>.txt.
 */
static const char *pos_key_for(u8 game_id)
{
	switch (game_id) {
	case GAME_RAC1: return "rac1";
	case GAME_RAC2: return "rac2";
	case GAME_RAC3: return "rac3";
	case GAME_RAC4: return "rac4";
	default:        return "unknown";
	}
}

static void enter_ingame(void)
{
	int same = (g_last_title[0] != 0 && qstreq(g_last_title, g_title) &&
	            g_last_game_id == g_game->game_id);

	core_lock();

	if (same) {
		/* Watches survive and resume; everything that writes goes on the record. */
		prev_build();
	} else {
		prev_clear();
		watch_clear();
		client_patch_drop_all();
	}

	/*
	 * Drop the live write-state without touching memory. We are not INGAME yet,
	 * so patch_revert would be refused anyway, but this makes it explicit: the
	 * old process is gone and its patches went with it.
	 */
	features_forget_state();
	freeze_clear();
	patch_forget_all();
	mods_forget_loaded();
	/* A fresh process has none of the savefile helper in it. */
	savefile_forget();

	if (!same) {
		features_set_game(g_game, g_title);
		mods_set_title(g_title);
		pos_use_title(pos_key_for(g_game->game_id));
	}

	mem_set_context(g_pid, 1);
	g_state = SESSION_INGAME;

	/*
	 * The boot's silence is over. Nothing came from this module for the whole of
	 * it and a client that understood that sent nothing either, so every
	 * subscription's idle clock reads as expired; start them again here rather
	 * than let the first packet after a boot drop the client it is meant for.
	 */
	net_subs_touch_all();

	/* The hot buffers still hold the dead process; read every block once. */
	memset(g_hotbuf, 0, sizeof(g_hotbuf));
	memset(&g_hot, 0, sizeof(g_hot));
	g_hot_primed = 0;

	if (g_game->on_enter != NULL) g_game->on_enter();

	/*
	 * The auto-flagged toggles and mods used to go in here. They do not any
	 * more: a mod is a code cave, the biggest write qwark makes, and a game that
	 * has only just reached INGAME is still loading its own modules. They go in
	 * from the tick, once settled() says the process has stopped changing shape.
	 * on_enter above stays where it is: those are three instruction words with a
	 * proven record, and the Deadlocked autosplitter needs its quit hook in
	 * place before the player can possibly quit.
	 */
	g_settled = 0;
	g_settled_ticks = 0;
	g_module_count = -1;
	g_module_same = 0;

	g_prev.toggles &= ~features_toggle_auto();
	g_prev.mods    &= ~mods_auto_mask();
	prev_update_pending();

	g_combo_armed = 1;

	core_unlock();

	plat_log("qwark: session INGAME (%s) gen %d", g_title, (int)g_generation);
}

/*
 * How often the VSH is asked what it is running, while it is not running a
 * game. plat_game_running and plat_game_pid are calls into vsh.self, and
 * plat_game_title reaches further still: it looks the game_plugin view up by
 * name and calls into it. Asking 120 times a second is free while a game is up
 * and the XMB is idle; it is 120 calls a second into the XMB *while the XMB is
 * handing the machine over*, which is the one moment none of it is free.
 *
 * INGAME keeps the full rate, because that is where the quit has to be noticed
 * within a frame or two.
 */
#define STATE_POLL_QUIET 12u   /* 10 Hz */

static void step_state(void)
{
	int running;
	u32 pid;

	if (g_state != SESSION_INGAME && (g_tick % STATE_POLL_QUIET) != 0) return;

	running = plat_game_running();
	pid = running ? plat_game_pid() : 0;

	switch (g_state) {
	case SESSION_XMB: {
		char title[16];

		if (!running || pid == 0) { g_pre_pid = 0; break; }

		/*
		 * The title comes from game_plugin, a plugin inside the XMB, and this is
		 * the one thing qwark asks of another VSH plugin. Asking it the moment a
		 * process id appears means asking while the XMB is mid-handover, which is
		 * when that plugin is least likely to be a thing worth calling into: a
		 * console that reboots here reboots before BOOTING is ever reached, which
		 * is why none of the windows below covered it.
		 *
		 * So the id has to hold still first. A quarter of the boot window, which
		 * is two seconds on a console and nothing on a platform that has no XMB,
		 * and which follows boot_delay_ms for anyone who tunes it.
		 */
		if (pid != g_pre_pid) {
			g_pre_pid = pid;
			g_pre_tick = g_tick;
			plat_log("qwark: a game process appeared (pid %d), letting it settle", (int)pid);
			break;
		}
		if (g_tick - g_pre_tick < boot_settle_ticks() / 4u) break;

		if (!plat_game_title(title)) break;

		g_ncandidates = game_candidates_for_title(title, g_candidates,
		                                          GAME_MAX_CANDIDATES);
		if (g_ncandidates == 0) {
			/*
			 * A title qwark does not know keeps the session in XMB: there is no
			 * fingerprint to wait for and nothing we could safely write.
			 */
			break;
		}

		/*
		 * Provisional until a fingerprint answers. For a single-game title it is
		 * already the right one; for BCES01503 it only fills the telemetry game
		 * byte during BOOTING, which the client shows as "booting".
		 */
		g_game = g_candidates[0];
		qstrcpy(g_title, sizeof(g_title), title);
		g_pid = pid;
		g_boot_ticks = 0;
		g_boot_start_tick = g_tick;
		g_fp_last_tick = g_tick;
		g_boot_settle = boot_settle_ticks();
		g_fp_matches = 0;
		g_fp_game = NULL;
		g_quiet_broken = 0;
		g_generation++;
		g_state = SESSION_BOOTING;
		plat_log("qwark: session BOOTING (%s) pid %d, leaving it alone for %d ticks",
		         g_title, (int)g_pid, (int)g_boot_settle);
		break;
	}

	case SESSION_BOOTING: {
		const struct game_api *found;

		if (!running || pid != g_pid) { enter_quitting(); break; }

		/*
		 * Nothing here touches the process until the window is over, and then
		 * only every BOOT_FINGERPRINT_EVERY ticks. This is the one path in
		 * qwark that reads a process before mem_set_context has let the gate
		 * open, so it is the one that has to hold itself back.
		 *
		 * Both intervals are measured against the tick counter rather than
		 * counted here, because this function does not run every tick any more:
		 * outside a game it runs at 10 Hz, and a window counted in visits would
		 * silently become twelve times what it says it is.
		 */
		g_boot_ticks = g_tick - g_boot_start_tick;
		if (g_boot_ticks < g_boot_settle) break;
		if (g_tick - g_fp_last_tick < BOOT_FINGERPRINT_EVERY) break;
		g_fp_last_tick = g_tick;

		found = fingerprinted_game();
		if (found == NULL) {
			g_fp_matches = 0;
			g_fp_game = NULL;
			break;
		}

		/* The same game, repeatedly, or the count starts again. */
		if (found != g_fp_game) {
			g_fp_game = found;
			g_fp_matches = 0;
		}

		g_fp_matches++;
		if (g_fp_matches < BOOT_FINGERPRINT_MATCHES) break;

		g_game = found;
		enter_ingame();
		break;
	}

	case SESSION_INGAME:
		if (!running || pid != g_pid) { enter_quitting(); break; }
		if (g_game != NULL && g_game->quit_hook_addr != 0) {
			u8 quit = 0;
			if (mem_read(g_game->quit_hook_addr, &quit, 1) == ST_OK && quit != 0) {
				enter_quitting();
			}
		}
		break;

	case SESSION_QUITTING:
		if (!running || pid == 0 || pid != g_pid) {
			g_state = SESSION_XMB;
			g_pid = 0;
			g_title[0] = 0;
			/*
			 * Telemetry must not keep naming a game that is not running: the
			 * client watches the `game` byte to know when to re-DESCRIBE. The
			 * feature and mod tables keep their own pointer, so the previous
			 * record still re-applies correctly on the way back in.
			 */
			g_game = NULL;
			g_ncandidates = 0;
			mem_set_context(0, 0);
			plat_log("qwark: session XMB");
		}
		break;

	default:
		g_state = SESSION_XMB;
		break;
	}
}

/* -------------------------------------------------------------- hot blocks */

static void read_hot(void)
{
	const u8 *ptrs[HOT_MAX_BLOCKS];
	u8 i;
	u8 n;

	if (g_state != SESSION_INGAME || g_game == NULL ||
	    g_game->hot == NULL || g_game->hot_decode == NULL) {
		return;
	}

	n = g_game->nhot;
	if (n > HOT_MAX_BLOCKS) n = HOT_MAX_BLOCKS;

	for (i = 0; i < n; i++) {
		u16 len = g_game->hot[i].len;
		u8 period = g_game->hot[i].period;

		if (len > HOT_MAX_LEN) len = HOT_MAX_LEN;
		ptrs[i] = g_hotbuf[i];

		/*
		 * A slow block keeps the bytes from its last read in between, so the
		 * decode always sees a complete set. The first pass of a new session
		 * reads every block, because the buffers still hold the old process.
		 */
		if (period > 1 && g_hot_primed &&
		    (g_tick % period) != (u32)(g_game->hot[i].phase % period))
			continue;

		if (mem_read(g_game->hot[i].addr, g_hotbuf[i], len) != ST_OK)
			memset(g_hotbuf[i], 0, len);
	}
	for (; i < HOT_MAX_BLOCKS; i++) ptrs[i] = NULL;

	g_hot_primed = 1;
	g_game->hot_decode(ptrs, &g_hot);
}

/* ------------------------------------------------- positions, planets, die */

int session_position_save(u8 slot)
{
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	int rc;

	if (g_game == NULL || g_game->save_blob == NULL) return ST_UNSUPPORTED;
	if (g_state != SESSION_INGAME) return ST_NOT_INGAME;
	if (slot == 0xFF) slot = config_selected_slot();
	if (slot >= QWARK_POS_SLOTS) return ST_BAD_ARG;

	memset(blob, 0, sizeof(blob));
	rc = g_game->save_blob(blob, &len);
	if (rc != ST_OK) return rc;
	if (len == 0 || len > QWARK_MAX_BLOB) return ST_BAD_ARG;

	return pos_store(g_hot.current_planet, slot, blob, len);
}

int session_position_load(u8 slot)
{
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	int rc;

	if (g_game == NULL || g_game->load_blob == NULL) return ST_UNSUPPORTED;
	if (g_state != SESSION_INGAME) return ST_NOT_INGAME;
	if (slot == 0xFF) slot = config_selected_slot();
	if (slot >= QWARK_POS_SLOTS) return ST_BAD_ARG;

	rc = pos_fetch(g_hot.current_planet, slot, blob, &len);
	if (rc != ST_OK) return rc;

	return g_game->load_blob(blob, len);
}

int session_planet_load(u8 planet, u8 flags)
{
	if (g_game == NULL || g_game->planet_load == NULL) return ST_UNSUPPORTED;
	if (g_state != SESSION_INGAME) return ST_NOT_INGAME;

	if (planet == 0xFF) planet = config_selected_planet();
	if (flags == 0xFF)  flags = config_selected_planet_flags();

	return g_game->planet_load(planet, flags);
}

int session_die(void)
{
	if (g_game == NULL || g_game->die == NULL) return ST_UNSUPPORTED;
	if (g_state != SESSION_INGAME) return ST_NOT_INGAME;
	return g_game->die();
}

int session_load_setaside(void)
{
	if (g_game == NULL || g_game->load_setaside == NULL) return ST_UNSUPPORTED;
	if (g_state != SESSION_INGAME) return ST_NOT_INGAME;
	return g_game->load_setaside();
}

/* ----------------------------------------------------------------- combos */

static void run_combo(u8 action)
{
	switch (action) {
	case COMBO_SAVE_POSITION: session_position_save(0xFF); break;
	case COMBO_LOAD_POSITION: session_position_load(0xFF); break;
	case COMBO_DIE:           session_die(); break;
	case COMBO_LOAD_PLANET:   session_planet_load(0xFF, 0xFF); break;
	case COMBO_LOAD_SETASIDE: session_load_setaside(); break;
	default: break;
	}
}

/* The config store is a linear scan, so cache the masks and refresh on change. */
static u32 g_combo_mask[COMBO_COUNT];
static u32 g_combo_cfg_version = 0xFFFFFFFFu;

static void refresh_combos(void)
{
	u8 a;
	u32 v = config_version();

	if (v == g_combo_cfg_version) return;
	g_combo_cfg_version = v;

	for (a = 0; a < COMBO_COUNT; a++) g_combo_mask[a] = config_combo(a);
}

void session_combo_suspend(u8 on)
{
	g_combo_suspend_until = on ? plat_time_us() + g_combo_suspend_window : 0;
}

u64 session_combo_suspend_window_us(void)
{
	return g_combo_suspend_window;
}

void session_set_combo_suspend_window_us(u64 us)
{
	g_combo_suspend_window = us;
}

static int combos_suspended(void)
{
	return g_combo_suspend_until != 0 && plat_time_us() < g_combo_suspend_until;
}

static void step_combos(void)
{
	u8 a;

	refresh_combos();

	if (g_state != SESSION_INGAME) { g_combo_armed = 1; return; }

	if (g_hot.pad_mask == 0) {
		g_combo_armed = 1;
		return;
	}

	/*
	 * Held off while the client captures. The arming rule above is untouched, so
	 * a pad still full when the hold ends stays disarmed: the buttons the user
	 * pressed to record a combo do not fire one the instant the combos come
	 * back, they wait for the pad to return to empty like any other press.
	 */
	if (combos_suspended()) {
		g_combo_armed = 0;
		return;
	}

	if (!g_combo_armed) return;

	for (a = 0; a < COMBO_COUNT; a++) {
		u32 mask = g_combo_mask[a];
		if (mask == 0) continue;
		if (g_hot.pad_mask != mask) continue;

		run_combo(a);
		g_combo_armed = 0;
		break;
	}
}

/* -------------------------------------------------------------- telemetry */

static u32 encode_info(u8 *out, u32 cap)
{
	u8 flags = 0;
	int i;

	if (cap < SESSION_INFO_SIZE) return 0;
	memset(out, 0, SESSION_INFO_SIZE);

	if (g_prev.pending) flags |= SESSION_FLAG_PREVIOUS_PENDING;
	/*
	 * Revision 1.6. The platform, not the session, decides these two: a client
	 * greys every WRITES_CODE row from bit2 rather than discovering one refusal
	 * at a time, and says "RPCS3" rather than "console" from bit1.
	 */
	if (plat_is_emulator())      flags |= SESSION_FLAG_EMULATOR;
	if (!plat_can_patch_code())  flags |= SESSION_FLAG_NO_CODE_PATCHES;
	if (g_state == SESSION_BOOTING) flags |= SESSION_FLAG_TELEMETRY_QUIET;

	out[0] = QWARK_PROTOCOL_VERSION;
	out[1] = QWARK_BUILD;
	out[2] = g_state;
	out[3] = (g_game != NULL) ? g_game->game_id : GAME_NONE;
	be32_put(out + 4, g_generation);
	be32_put(out + 8, g_tick);

	{
		u32 n = qstrlen(g_title);
		if (n > 12) n = 12;
		memcpy(out + 12, g_title, n);
	}

	out[24] = flags;
	out[25] = config_selected_slot();
	out[26] = config_selected_planet();
	out[27] = config_selected_planet_flags();
	out[28] = g_hot.current_planet;

	/*
	 * Revision 1.11, out of the first two of the three pad bytes: how long the
	 * silence announced by flags bit3 has left to run, in milliseconds, capped
	 * at what the field holds. Zero whenever the flag is clear.
	 */
	if ((flags & SESSION_FLAG_TELEMETRY_QUIET) != 0) {
		u32 left = (g_boot_settle > g_boot_ticks) ? (g_boot_settle - g_boot_ticks) : 0;
		u32 ms = (u32)(((u64)left * TICK_PERIOD_US) / 1000u);
		if (ms > 0xFFFFu) ms = 0xFFFFu;
		be16_put(out + 29, (u16)ms);
	} else {
		be16_put(out + 29, 0);
	}
	/* out[31] pad */

	for (i = 0; i < 3; i++) bef32_put(out + 32 + i * 4, g_hot.pos[i]);
	be32_put(out + 44, g_hot.pad_mask);
	for (i = 0; i < 4; i++) bef32_put(out + 48 + i * 4, g_hot.analog[i]);
	for (i = 0; i < QWARK_MAX_READOUTS; i++)
		be32_put(out + 64 + i * 4, g_hot.readout[i]);

	be64_put(out + 128, features_toggle_state());
	be64_put(out + 136, features_toggle_auto());
	be64_put(out + 144, freeze_mask());
	be32_put(out + 152, mods_loaded_mask());
	be32_put(out + 156, mods_auto_mask());
	be32_put(out + 160, g_prev.mods);

	return SESSION_INFO_SIZE;
}

static u32 encode_telemetry(u8 *out, u32 cap)
{
	u32 off;
	u32 i;
	u8 n = 0;

	if (cap < TELEMETRY_MAX) return 0;

	memcpy(out, TELEMETRY_MAGIC, 4);
	encode_info(out + 4, cap - 4);
	off = 4 + SESSION_INFO_SIZE;

	off++;   /* nwatch goes back in once we know it */

	for (i = 0; i < QWARK_MAX_WATCHES; i++) {
		const struct watch_entry *w = watch_slot((u8)i);
		if (w == NULL) continue;

		out[off + 0] = (u8)i;
		out[off + 1] = w->size;
		out[off + 2] = w->valid;
		out[off + 3] = 0;
		be64_put(out + off + 4, w->value);
		off += 12;
		n++;
	}

	out[4 + SESSION_INFO_SIZE] = n;
	return off;
}

static void publish(void)
{
	core_lock();
	encode_info(g_info_pub, sizeof(g_info_pub));
	g_tele_pub_len = encode_telemetry(g_tele_pub, sizeof(g_tele_pub));
	core_unlock();
}

u32 session_info_copy(u8 *out, u32 cap)
{
	u32 n = SESSION_INFO_SIZE;

	if (cap < n) return 0;

	core_lock();
	memcpy(out, g_info_pub, n);
	core_unlock();
	return n;
}

u32 session_telemetry_copy(u8 *out, u32 cap)
{
	u32 n;

	core_lock();
	n = g_tele_pub_len;
	if (n > cap) n = 0;
	else memcpy(out, g_tele_pub, n);
	core_unlock();
	return n;
}

/* ---------------------------------------------------------------- the loop */

int session_init(void)
{
	int i;

	plat_mutex_init(&g_core_mutex);
	plat_mutex_init(&g_ring_mutex);
	plat_trace("qwark:   session mutexes ok");

	for (i = 0; i < QWARK_RING_SLOTS; i++) {
		g_ring[i].state = RING_FREE;
		g_ring[i].cmd = NULL;
		if (plat_sem_init(&g_ring[i].sem, 0) != 0) {
			plat_trace("qwark:   ring semaphore create FAILED");
			return ST_IO_ERROR;
		}
	}
	plat_trace("qwark:   ring semaphores ok");

	if (autosplit_init() != ST_OK) {
		plat_trace("qwark:   autosplit mutex create FAILED");
		return ST_IO_ERROR;
	}
	plat_trace("qwark:   autosplit ring ok");

	config_init();
	plat_trace("qwark:   config_init ok");
	prev_clear();

	/* Nothing is published yet; give clients a valid empty snapshot to read. */
	publish();
	return ST_OK;
}

void session_stop(void)
{
	plat_trace("qwark:   session_stop: tick loop asked to stop");
	g_running = 0;
}

void session_shutdown(void)
{
	int i;

	/*
	 * Every kernel object session_init created. The tick thread has been joined
	 * by the time this runs and the ring is closed, so nothing is parked on a
	 * semaphore and nothing is holding the core lock.
	 */
	for (i = 0; i < QWARK_RING_SLOTS; i++) plat_sem_destroy(&g_ring[i].sem);
	plat_trace("qwark:   ring semaphores destroyed");

	autosplit_shutdown();
	plat_mutex_destroy(&g_ring_mutex);
	plat_mutex_destroy(&g_core_mutex);
	plat_trace("qwark:   session mutexes destroyed");
}

/*
 * The moment the big writes are allowed. The auto-flagged toggles and mods were
 * held back from enter_ingame for this, so they go in here instead, in the same
 * order and with the same bookkeeping they had there.
 */
static void settled_reached(void)
{
	g_settled = 1;

	features_apply_mask(features_auto_mask());
	mods_apply_mask(mods_auto_mask());

	g_prev.toggles &= ~features_toggle_auto();
	g_prev.mods    &= ~mods_auto_mask();
	prev_update_pending();
}

/*
 * Watches the process stop changing shape, and lets the big writes through when
 * it has. Runs on the tick, INGAME only, and asks the kernel rather than the
 * process: see plat_module_count.
 */
static void settled_tick(void)
{
	int count;

	if (g_settled) return;

	g_settled_ticks++;
	if (g_settled_ticks < plat_settle_min_ticks()) return;

	if (g_settled_ticks >= SETTLE_MAX_TICKS) {
		plat_log("qwark: settling gave up after %d ticks, letting the writes through",
		         (int)g_settled_ticks);
		settled_reached();
		return;
	}

	if ((g_settled_ticks % SETTLE_POLL_EVERY) != 0) return;

	count = plat_module_count(g_pid);
	if (count < 0) {
		/* No module list here. The floor was the whole of the wait. */
		settled_reached();
		return;
	}

	if (count == g_module_count) {
		g_module_same++;
		if (g_module_same >= SETTLE_SAME_POLLS) {
			plat_log("qwark: %d modules, steady, %d ticks in", count, (int)g_settled_ticks);
			settled_reached();
		}
		return;
	}

	g_module_count = count;
	g_module_same = 1;
}

/* One iteration of the loop. Exposed as session_step_once() for the host tests. */
static void session_step(void)
{
	step_state();

	ring_drain();

	/*
	 * The core lock covers every table a network thread can list, so a WATCH_LIST
	 * or a POS_LIST never sees one half-updated. It is held for the two hot-block
	 * reads plus the freeze and watch passes, which is microseconds.
	 */
	core_lock();
	if (g_state == SESSION_INGAME) {
		read_hot();
		/*
		 * The LIVE toggles are game-owned bytes; make toggle_state agree with
		 * what memory says rather than with the last thing qwark wrote. No
		 * hook fires and nothing is written back.
		 */
		if ((g_tick % LIVE_POLL_EVERY) == 0) features_poll_live();
		/*
		 * Game-side watchers run on the decoded block, before the freeze and
		 * watch passes, so anything they write this tick is already in place
		 * when the rest of the tick reads memory back.
		 */
		if (g_game != NULL && g_game->on_tick != NULL) g_game->on_tick(&g_hot);
		freeze_tick();
		watch_tick();
		step_combos();
		/*
		 * Protocol 1.9. An outstanding savefile request has its byte watched here
		 * rather than when SAVEFILE_INFO is asked, because the settle window is
		 * counted in ticks: a client that polls slowly must not shorten it, and
		 * one that polls in a tight loop must not lengthen it either.
		 */
		savefile_tick();
		settled_tick();
	} else {
		watch_invalidate();
	}
	core_unlock();

	/*
	 * Protocol 1.4. Autosplit datagrams go out every tick, not every fourth:
	 * a split has to reach the PC in single-digit milliseconds, and each event
	 * repeats for three ticks so one lost datagram costs nothing.
	 *
	 * Not while a game is starting, though. Nothing produces an event then - the
	 * watchers run INGAME - so in practice this holds back nothing at all, and
	 * "the network is completely quiet during a boot" is worth more as a rule
	 * with no exceptions in it than as one with a harmless-looking exception.
	 */
	if (g_state != SESSION_BOOTING || g_quiet_broken) autosplit_push();

	g_tick++;

	if ((g_tick % ((g_state == SESSION_INGAME) ? TELEMETRY_EVERY : TELEMETRY_EVERY_QUIET)) == 0) {
		/*
		 * Revision 1.11. A game is starting, this module lives in the VSH, and
		 * the network is the half of the crash the boot window does not cover:
		 * BOOTING sends nothing, unless a client has already shown that its
		 * idea of nothing is to ask over TCP instead.
		 */
		/*
		 * Published either way: HELLO and GET_STATE hand back the last published
		 * block, and a client that connects or asks during a boot deserves the
		 * truth about it. Filling that buffer costs nothing and touches nothing.
		 * It is the sending that stops.
		 */
		publish();
		if (g_state != SESSION_BOOTING || g_quiet_broken) {
			net_send_telemetry(g_tele_pub, g_tele_pub_len);
		}
	}
}

void session_step_once(void)
{
	session_step();
}

void session_tick_thread(void *arg)
{
	(void)arg;

	plat_log("qwark: tick thread up");

	while (g_running) {
		session_step();

		plat_yield();
		plat_sleep_us(TICK_PERIOD_US);
	}

	/*
	 * Let anyone still parked on the ring go, or the module never unloads, and
	 * close it under the mutex so a submit racing us here answers BUSY rather
	 * than parking on a semaphore that will never be posted.
	 */
	{
		int i;

		plat_mutex_lock(&g_ring_mutex);
		g_ring_closed = 1;
		for (i = 0; i < QWARK_RING_SLOTS; i++) {
			if (g_ring[i].state == RING_PENDING) {
				g_ring[i].cmd->status = ST_BUSY;
				g_ring[i].cmd->replylen = 0;
				g_ring[i].state = RING_DONE;
				plat_sem_post(&g_ring[i].sem);
			}
		}
		plat_mutex_unlock(&g_ring_mutex);
	}

	plat_log("qwark: tick thread down");
	plat_trace("qwark:   tick thread returning");
	plat_thread_exit();
}
