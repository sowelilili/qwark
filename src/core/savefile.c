#include "savefile.h"
#include "mem.h"
#include "session.h"
#include "config.h"
#include "util.h"
#include "../games/sfhelper_bins.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * Whether this process has the helper in it. It is a plain flag rather than a
 * read-back, because the read-back is api_mod and that only goes to 1 once the
 * game has reached the hook: between the write and the next frame the helper is
 * installed and not yet running, and a second install in that window would write
 * the caves out from under code that is about to execute them.
 */
static int g_installed;

/*
 * Build 47. Whether the running game already had the helper in it when it
 * loaded, on a platform that refuses code patches: -1 until the tick thread has
 * looked, then 0 or 1 for the rest of the session. savefile_forget puts it back
 * to -1 wherever it puts g_installed back to 0.
 *
 * That platform is RPCS3, and the helper gets there through an RPCS3 patch the
 * client writes from SAVEFILE_PATCH: RPCS3 applies it to the executable as it
 * loads it, before it recompiles a single block, so the caves are code the game
 * runs and the hook is live from the first frame. Nothing about that can change
 * while the game runs - a patch goes in at load or not at all, and nothing ever
 * takes one out - so one look a session is the whole of the question. The look
 * is one read per cave and one per hook word, a kilobyte or two over PINE,
 * which is cheap once and not something to repeat on every INFO a client polls.
 *
 * It is its own flag rather than g_installed because the two answer different
 * questions: g_installed is "qwark wrote it", this is "it was there already",
 * and only the second means anything on this platform.
 *
 * Build 51: the emulator builds' alone (QWARK_EMULATOR, plat.h). A console
 * installs its own helper and never looks for one, so on the PS3 this is a
 * constant nothing asks: every read of it sits behind !plat_can_patch_code(),
 * the constant 0 there, and folds away with the look (present_look, below).
 */
#ifdef QWARK_EMULATOR
static int g_present = -1;

static void present_forget(void)
{
	g_present = -1;
}
#else
static const int g_present = -1;

static __inline void present_forget(void) { }
#endif

/*
 * config.txt `savefile_helper = 0`: never write the helper into a game. Off, the
 * game reports as having none and nothing is ever written.
 */
static int g_enabled = 1;

void savefile_set_enabled(int on)
{
	g_enabled = on ? 1 : 0;
}

/*
 * One outstanding request, watched from the tick thread.
 *
 * `armed` is set the moment qwark writes the request byte and is what
 * SAVEFILE_INFO reports; `settle` counts the ticks the byte has read 0 in a row
 * since. One zero on its own is thinner evidence than it looks. The helper
 * clears the byte with a plain store after a plain memcpy, with no barrier
 * between the two, so nothing promises that a reader sees the last bytes of the
 * copy before it sees the cleared byte; qwark's read is a single unsynchronised
 * sample of one byte of a running game; and before `armed` existed, a memory
 * read that simply failed also left the bit clear, which read as "the save is
 * ready". A quarter of a second of zeroes, on every tick of it, is proof enough
 * of all three, and the client is not told the save is ready until then.
 */
struct sf_request {
	u8  armed;
	/*
	 * Build 43. The helper answered this request with SAVEFILE_HELPER_REFUSED
	 * instead of doing the work: the game was not idle on the frame it saw it.
	 * Set when the byte is read back, cleared when the next request goes out,
	 * and what tells a transfer waiting on this request to stop rather than
	 * copy a buffer nothing has filled.
	 *
	 * Build 53: it holds the reason, the SAVEFILE_ERR_* code the refusal is
	 * reported with - SAVEFILE_ERR_NOT_IDLE, or SAVEFILE_ERR_NO_SAVE for
	 * SAVEFILE_HELPER_NO_SAVE - and 0 when the request was not refused.
	 */
	u8  refused;
	u16 settle;
};

static struct sf_request g_set_aside;
static struct sf_request g_load;

/* Build 53: see savefile_load_refusals. */
static u32 g_load_refusals;

/*
 * The one transfer, revision 1.10.
 *
 * A save is up to 2 MB and the tick thread owes the rest of the module a tick
 * every 8.3 ms, so the copy between a file and the aside buffer is a state
 * machine that moves SAVEFILE_CHUNKS_PER_TICK chunks and gets out of the way.
 * There is exactly one of these because there is exactly one aside buffer: a
 * second STORE or RESTORE while this is running is refused rather than queued.
 */
#define SF_IDLE         0
#define SF_STORE_SETTLE 1   /* the set-aside is out; waiting for the helper */
#define SF_STORE_COPY   2   /* buffer -> file */
#define SF_RESTORE_COPY 3   /* file -> buffer */
#define SF_RESTORE_LOAD 4   /* the load is out; waiting for the helper */

struct sf_transfer {
	u8   state;
	u8   error;
	/*
	 * Whether `file` is an open handle. A flag rather than a sentinel in `file`,
	 * because the table starts out all zeroes and zero is a handle a platform
	 * may well hand out: closing it on the strength of a sentinel that was
	 * never written would take out somebody else's file.
	 */
	u8   have_file;
	u32  done;
	u32  total;
	u32  crc;
	plat_file_t file;
	char path[SAVEFILE_PATH_MAX];
};

static struct sf_transfer g_xfer;

/*
 * gnu99, so no _Static_assert: a copy chunk bigger than the shared scratch
 * buffer fails the build here rather than running off the end of it.
 */
typedef char savefile_copy_chunk_fits[SAVEFILE_COPY_CHUNK <= QSCRATCH_BYTES ? 1 : -1];

static void request_clear(struct sf_request *r)
{
	r->armed = 0;
	r->settle = 0;
	r->refused = 0;
}

/*
 * Build 43. Whether the running game is somewhere a set-aside or a load may
 * happen at all. A game that does not answer is always ready; since build 53
 * every game answers.
 * Tick thread, like everything else that reads the game.
 *
 * Build 53: `what` is one of GAME_SF_SET_ASIDE, GAME_SF_LOAD and
 * GAME_SF_RESTORE (src/games/game.h), and the answer is the SAVEFILE_ERR_* code
 * a refusal is reported with, SAVEFILE_ERR_NONE when the work may start.
 */
static u8 game_ready(u8 what)
{
	const struct game_api *g = session_game();

	if (g == NULL || g->savefile_ready == NULL) return SAVEFILE_ERR_NONE;
	return g->savefile_ready(what);
}

/* The error a refused request left behind, or NOT_IDLE when it left none. */
static u8 refusal_error(void)
{
	return g_xfer.error != SAVEFILE_ERR_NONE ? g_xfer.error : SAVEFILE_ERR_NOT_IDLE;
}

/* Drops whatever the transfer was doing, keeping `error` for the client. */
static void xfer_stop(u8 error)
{
	if (g_xfer.have_file) {
		plat_file_close(g_xfer.file);
		g_xfer.have_file = 0;
	}

	/*
	 * A store that stopped part way through leaves a file that is the right
	 * name and the wrong length. A restore would refuse it later, on its size,
	 * but a listing would still show it and a user would still think it was
	 * their save, so it goes. Only from SF_STORE_COPY: that is the state that
	 * opened the file, and by opening it truncated whatever was there, so there
	 * is nothing left to lose. A store that fails before then has written
	 * nothing, and unlinking would take out the file it was going to replace.
	 */
	if (error != SAVEFILE_ERR_NONE && g_xfer.state == SF_STORE_COPY) {
		plat_file_unlink(g_xfer.path);
	}

	g_xfer.state = SF_IDLE;
	g_xfer.crc   = 0;
	g_xfer.error = error;

	/*
	 * `done` and `total` are left where they stopped, exactly as `error` is:
	 * between transfers they describe the last one, which is what a client that
	 * polls once more after the pending bit clears wants to read. The next STORE
	 * or RESTORE puts all three back.
	 */
	if (error != SAVEFILE_ERR_NONE) plat_log("savefile: transfer failed, error %d", (int)error);
}

void savefile_forget(void)
{
	g_installed = 0;
	/*
	 * Build 47. The next process may have been loaded with the RPCS3 patch or
	 * without it - a user who has just written one restarts the game for it to
	 * take - so it is looked for again rather than taken from the last one.
	 */
	present_forget();
	request_clear(&g_set_aside);
	request_clear(&g_load);
	g_load_refusals = 0;

	/*
	 * A transfer that was in flight belonged to a process that is gone. Say so
	 * rather than leave a client polling a `done` that will never move.
	 */
	if (g_xfer.state != SF_IDLE) xfer_stop(SAVEFILE_ERR_NO_HELPER);
}

static const struct sf_desc *desc_now(void)
{
	const struct game_api *g = session_game();

	if (g == NULL) return NULL;
	return sf_desc_for_game(g->game_id);
}

/*
 * Build 47. Whether every byte of every cave and every hook word in the table is
 * in the game exactly as savefile_install would have written it: 1 yes, 0 no,
 * -1 when a read failed and nothing can be said yet. Tick thread, because it
 * reads the game.
 *
 * All of it or nothing. A cave from an older build of the helper differs
 * somewhere, and so does a hook word that branches to where an older helper
 * started, and code this module did not build may not keep the request bytes
 * the way this module reads them: it reads as not present, and the client
 * writes the new patch. The request bytes and the aside buffer are not part of
 * the look - they are data the helper and the game change, and the patch only
 * sets the request bytes to 0 at load - only the code the patch wrote.
 *
 * Build 51: the look is the emulator builds' alone. On the PS3 every caller of
 * present_look sits behind !plat_can_patch_code(), which is the constant 0
 * there, so the stub below is only ever compiled, never called.
 */
#ifdef QWARK_EMULATOR
static int helper_matches(const struct sf_desc *d)
{
	/*
	 * The tick thread's scratch buffer, filled and compared inside this call and
	 * never held across a return: see util.h. A cave is a few hundred bytes, so
	 * it is one read each, in chunks only if one ever outgrows the buffer.
	 */
	u8 *buf = qscratch();
	u8 i;

	for (i = 0; i < d->ncaves; i++) {
		u32 off = 0;

		while (off < d->caves[i].len) {
			u32 n = d->caves[i].len - off;
			if (n > QSCRATCH_BYTES) n = QSCRATCH_BYTES;

			if (mem_read(d->caves[i].addr + off, buf, n) != ST_OK) return -1;
			if (memcmp(buf, d->caves[i].bytes + off, n) != 0) return 0;
			off += n;
		}
	}

	for (i = 0; i < d->nhooks; i++) {
		u32 word = 0;

		if (mem_read_u32(d->hooks[i].addr, &word) != ST_OK) return -1;
		if (word != d->hooks[i].value) return 0;
	}

	return 1;
}

/*
 * Build 47. The one look a session takes, on a platform that refuses code
 * patches, for the helper an RPCS3 patch put in at load. It does nothing once
 * the answer is known, on a platform that patches code (the console installs
 * its own and never asks), with the helper switched off, and outside INGAME,
 * where there is no process to look at. A read that fails leaves the answer
 * unknown and the next caller looks again: a stalled emulator is not an absent
 * helper, and the session would otherwise carry that wrong answer to its end.
 */
static void present_look(const struct sf_desc *d)
{
	int found;

	if (g_present >= 0 || d == NULL) return;
	if (plat_can_patch_code() || !g_enabled || !mem_is_ingame()) return;

	found = helper_matches(d);
	if (found < 0) return;

	g_present = found;
	if (found) plat_log("savefile: the helper is already in game %d, patched in at load", (int)d->game_id);
	else       plat_log("savefile: no helper in game %d, or one from another build", (int)d->game_id);
}
#else
static __inline void present_look(const struct sf_desc *d) { (void)d; }
#endif /* QWARK_EMULATOR */

/*
 * The gate every entry point starts with. NOT_INGAME outside INGAME, because
 * there is no process to write into; UNSUPPORTED for a game with no helper.
 *
 * On a platform that refuses code patches the helper cannot be qwark's own, so
 * it is UNSUPPORTED there unless an RPCS3 patch put it in (build 47). Before
 * build 47 that refusal came ahead of the INGAME test, so such a platform said
 * UNSUPPORTED at the XMB too; whether the helper is there is a question about a
 * process now, and outside INGAME there is none to ask about. On a console the
 * order changes nothing, since that test never refused.
 *
 * `look` is 0 for the one caller on a network thread, which must never read the
 * game: it takes the answer the tick thread has already found, and BUSY in the
 * tick or so before it has found one.
 */
static int gate_as(const struct sf_desc **out, int look)
{
	const struct sf_desc *d;

	*out = NULL;

	if (!g_enabled) return ST_UNSUPPORTED;
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;

	if (!plat_can_patch_code()) {
		if (look) present_look(d);
		if (g_present < 0) return ST_BUSY;
		if (!g_present) return ST_UNSUPPORTED;
	}

	*out = d;
	return ST_OK;
}

static int gate(const struct sf_desc **out)
{
	return gate_as(out, 1);
}

/*
 * Whether this process has the helper in it, by either road: qwark's own
 * install on a console, the RPCS3 patch where code cannot be patched.
 */
static int helper_in(void)
{
	if (plat_can_patch_code()) return g_installed != 0;
	return g_present == 1;
}

int savefile_install(void)
{
	const struct sf_desc *d;
	int rc = gate(&d);
	u8 i;

	if (rc != ST_OK) return rc;
	if (g_installed) return ST_OK;

	/*
	 * Build 47. Where code cannot be patched the gate has already found the
	 * helper in the game, put there by an RPCS3 patch when it loaded, and there
	 * is nothing left to write. Not the caves or the hook, which are there and
	 * which RPCS3 has already compiled, and not the three request bytes either:
	 * the console zeroes them below because nothing is reading them yet, and
	 * here the patch zeroed them at load (SAVEFILE_PATCH carries them) and the
	 * hook has been live since the first frame, so they are the helper's own
	 * and a zero written now could land on top of its work.
	 */
	if (!plat_can_patch_code()) return ST_OK;

	/*
	 * A request byte left over from whatever used to live at these addresses
	 * would fire the moment the hook goes in, so the three go to zero while
	 * nothing is reading them yet.
	 */
	if (mem_write_u8(d->api_mod, 0) != ST_OK ||
	    mem_write_u8(d->api_load, 0) != ST_OK ||
	    mem_write_u8(d->api_setaside, 0) != ST_OK) return ST_IO_ERROR;

	/* Build 53: and RaC3's trophy reset byte, the fourth, where there is one. */
	if (d->api_trophy != 0 && mem_write_u8(d->api_trophy, 0) != ST_OK) return ST_IO_ERROR;

	/*
	 * Caves first and hook words second, the order mods.c uses: the words branch
	 * into the caves, so the target exists before anything can jump to it.
	 * Nothing pauses the game; that order is what keeps it off a half-written cave.
	 */
	for (i = 0; i < d->ncaves; i++) {
		rc = mem_write(d->caves[i].addr, d->caves[i].bytes, d->caves[i].len);
		if (rc != ST_OK) return rc;
	}

	for (i = 0; i < d->nhooks; i++) {
		rc = mem_write_u32(d->hooks[i].addr, d->hooks[i].value);
		if (rc != ST_OK) return rc;
	}

	/*
	 * Nothing reverts it. The hook is a branch the game may be executing at any
	 * moment, and taking it back out from under running code is the crash
	 * mods.c documents; with no request outstanding the helper is a byte write
	 * and three comparisons a frame.
	 */
	g_installed = 1;
	plat_log("savefile: helper installed for game %d", (int)d->game_id);
	return ST_OK;
}

int savefile_info(u8 *supported, u8 *installed, u8 *running, u8 *pending, u32 *size)
{
	const struct sf_desc *d;
	u8 byte = 0;

	*supported = 0;
	*installed = 0;
	*running   = 0;
	*pending   = 0;
	*size      = 0;

	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	/*
	 * A game qwark has no helper for is not an error: the client asks every
	 * game and hides its save-file panel for the ones that answer 0 here, and a
	 * helper switched off in config.txt answers the same way.
	 */
	if (d == NULL || !g_enabled) return ST_OK;

	*supported = 1;
	*size = d->aside_size;

	/*
	 * Build 47. Where code cannot be patched this used to be UNSUPPORTED before
	 * anything else was asked. It answers now, and `installed` is whether an
	 * RPCS3 patch put the helper in when the game loaded - which is how a client
	 * knows whether to offer to write one. A client tells the two platforms
	 * apart by SessionInfo flags bit2, which it already reads. The look is the
	 * session's one, usually taken already by the first INGAME tick; a read that
	 * fails leaves it unknown, and BUSY says to ask again rather than report a
	 * helper absent that may well be there.
	 */
	if (!plat_can_patch_code()) {
		present_look(d);
		if (g_present < 0) return ST_BUSY;
	}

	/* Discovery must not patch a newly booted game. Until an action installs
	 * the helper, its request bytes may contain unrelated data. */
	*installed = (u8)helper_in();
	if (!*installed) return ST_OK;

	if (mem_read_u8(d->api_mod, &byte) == ST_OK && byte == 1) *running = 1;

	/*
	 * The bytes are read here as well as on the tick, so a request something
	 * other than qwark started - RaC1's Force autosave drives a byte of its own,
	 * and Deadlocked's load parks a 2 in api_load for the length of its menu and
	 * a 3 while a cancel settles - is reported outstanding too. What the settle window adds is the other
	 * direction: a byte that reads 0 is not enough to say the work is done.
	 */
	if (g_set_aside.armed) {
		*pending |= SAVEFILE_PENDING_SET_ASIDE;
	} else if (mem_read_u8(d->api_setaside, &byte) == ST_OK && byte != 0) {
		*pending |= SAVEFILE_PENDING_SET_ASIDE;
	}

	if (g_load.armed) {
		*pending |= SAVEFILE_PENDING_LOAD;
	} else if (mem_read_u8(d->api_load, &byte) == ST_OK && byte != 0) {
		*pending |= SAVEFILE_PENDING_LOAD;
	}

	/*
	 * Revision 1.10. The copy qwark is running itself. It is the same promise as
	 * the other two bits: set from the moment STORE or RESTORE is accepted,
	 * clear only once the copy and the request that goes with it are both over.
	 */
	if (g_xfer.state != SF_IDLE) *pending |= SAVEFILE_PENDING_TRANSFER;

	return ST_OK;
}

void savefile_transfer(u32 *done, u32 *total, u8 *error)
{
	*done  = g_xfer.done;
	*total = g_xfer.total;
	*error = g_xfer.error;
}

/*
 * One request's settle window, a tick's worth. The byte reading anything but 0
 * puts the count back to the start, so the window is the *last* stretch of
 * zeroes rather than any stretch of them.
 */
static void request_tick(struct sf_request *r, u32 addr)
{
	u8 byte = 0;

	if (!r->armed) return;

	if (mem_read_u8(addr, &byte) != ST_OK) return;

	/*
	 * Build 43. The helper dropped the request: the game was not idle on the
	 * frame it saw it, so it did nothing and left this code in the byte instead
	 * of clearing it. The request is over - it is never retried, because a load
	 * that fires at some later moment the user did not ask for is worse than
	 * one that plainly did not happen - so put the byte back to 0, since the
	 * helper leaves the code there for exactly one reader, and let the error
	 * say why nothing came of it.
	 */
	if (byte == SAVEFILE_HELPER_REFUSED || byte == SAVEFILE_HELPER_NO_SAVE) {
		u8 why = byte == SAVEFILE_HELPER_REFUSED ? SAVEFILE_ERR_NOT_IDLE : SAVEFILE_ERR_NO_SAVE;

		mem_write_u8(addr, 0);
		request_clear(r);
		r->refused = why;
		g_xfer.error = why;
		if (r == &g_load) g_load_refusals++;
		if (why == SAVEFILE_ERR_NOT_IDLE)
			plat_log("savefile: the helper refused the request, the game was not idle");
		else
			plat_log("savefile: the helper refused the request, there was no save");
		return;
	}

	if (byte != 0) {
		r->settle = 0;
		return;
	}

	r->settle++;
	if (r->settle >= SAVEFILE_SETTLE_TICKS) request_clear(r);
}

/* Writes <path>.sum, the eight lowercase hex digits mods.c reads qwark.sum as. */
static void sum_write(const char *path, u32 crc)
{
	char sum_path[SAVEFILE_PATH_MAX];
	char text[16];
	plat_file_t f;

	qstrcpy(sum_path, sizeof(sum_path), path);
	qstrcat(sum_path, sizeof(sum_path), SAVEFILE_SUM_EXT);

	qfmt_hex(text, sizeof(text), crc, 8);

	if (plat_file_open(sum_path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, 8);
	plat_file_close(f);
}

/*
 * One tick of the copy. Called from savefile_tick with the running game's
 * descriptor, so every branch here already knows the game is there.
 */
static void xfer_tick(const struct sf_desc *d)
{
	/*
	 * The tick thread's shared scratch buffer, filled and emptied inside this
	 * call and never held across a return to the tick loop: see util.h.
	 */
	u8 *copy = qscratch();
	u32 chunks;
	int rc;

	switch (g_xfer.state) {

	case SF_STORE_SETTLE:
		/*
		 * The helper dropped it, so the aside buffer still holds whatever was
		 * in it before. Storing that would write a file of the last save under
		 * this one's name, which is worse than storing nothing.
		 */
		if (g_set_aside.refused) {
			xfer_stop(g_set_aside.refused);
			return;
		}

		/*
		 * The helper has the request. Nothing may be read out of the buffer
		 * until the settle window says the copy inside the game is over, which
		 * is the guarantee SAVEFILE_PENDING_SET_ASIDE carries; this waits on
		 * exactly that rather than on a delay of its own.
		 */
		if (g_set_aside.armed) return;

		if (plat_file_open(g_xfer.path, PLAT_OPEN_WRITE, &g_xfer.file) != 0) {
			xfer_stop(SAVEFILE_ERR_IO);
			return;
		}
		g_xfer.have_file = 1;
		g_xfer.state = SF_STORE_COPY;
		return;

	case SF_STORE_COPY:
		for (chunks = 0; chunks < SAVEFILE_CHUNKS_PER_TICK && g_xfer.done < g_xfer.total; chunks++) {
			u32 n = g_xfer.total - g_xfer.done;
			if (n > SAVEFILE_COPY_CHUNK) n = SAVEFILE_COPY_CHUNK;

			if (mem_read(d->aside_addr + g_xfer.done, copy, n) != ST_OK) {
				xfer_stop(SAVEFILE_ERR_IO);
				return;
			}
			if (plat_file_write(g_xfer.file, copy, n) != 0) {
				xfer_stop(SAVEFILE_ERR_IO);
				return;
			}

			g_xfer.crc = qcrc32_update(g_xfer.crc, copy, n);
			g_xfer.done += n;
		}

		if (g_xfer.done < g_xfer.total) return;

		plat_file_close(g_xfer.file);
		g_xfer.have_file = 0;
		sum_write(g_xfer.path, qcrc32_finish(g_xfer.crc));
		plat_log("savefile: stored %d bytes", (int)g_xfer.total);
		xfer_stop(SAVEFILE_ERR_NONE);
		return;

	case SF_RESTORE_COPY:
		for (chunks = 0; chunks < SAVEFILE_CHUNKS_PER_TICK && g_xfer.done < g_xfer.total; chunks++) {
			u32 n = g_xfer.total - g_xfer.done;
			u32 got = 0;
			if (n > SAVEFILE_COPY_CHUNK) n = SAVEFILE_COPY_CHUNK;

			if (plat_file_read(g_xfer.file, copy, n, &got) != 0) {
				xfer_stop(SAVEFILE_ERR_IO);
				return;
			}
			/*
			 * The size was checked before the transfer started, so a file that
			 * runs out here shrank underneath us. Stop rather than raise a load
			 * over a buffer that is part this save and part the last one.
			 */
			if (got != n) {
				xfer_stop(SAVEFILE_ERR_SHORT);
				return;
			}
			if (mem_write(d->aside_addr + g_xfer.done, copy, got) != ST_OK) {
				xfer_stop(SAVEFILE_ERR_IO);
				return;
			}

			g_xfer.done += got;
		}

		if (g_xfer.done < g_xfer.total) return;

		plat_file_close(g_xfer.file);
		g_xfer.have_file = 0;

		/*
		 * The whole file is in the buffer, so now the game may have it - if the
		 * game is still somewhere it may take it. The copy took ticks, and the
		 * game had frames of its own in them, so this is asked again here
		 * rather than taken on trust from when the RESTORE was accepted.
		 */
		rc = savefile_load_aside();
		if (rc != ST_OK) {
			xfer_stop(rc == ST_BUSY ? refusal_error() : SAVEFILE_ERR_IO);
			return;
		}
		plat_log("savefile: restored %d bytes, load requested", (int)g_xfer.total);
		g_xfer.state = SF_RESTORE_LOAD;
		return;

	case SF_RESTORE_LOAD:
		/* The helper dropped it, so the game never took the buffer. */
		if (g_load.refused) {
			xfer_stop(g_load.refused);
			return;
		}

		/* Done when the load has settled, the same window a plain load waits. */
		if (g_load.armed) return;
		xfer_stop(SAVEFILE_ERR_NONE);
		return;

	default:
		return;
	}
}

void savefile_tick(void)
{
	const struct sf_desc *d;

	/*
	 * Build 47. The session's one look for a patched-in helper is taken here, on
	 * the first INGAME tick, rather than left for the first op that needs it: the
	 * three library ops gate on a network thread that never reads the game, and
	 * they need the answer to be there already. Once it has one this is a
	 * comparison a tick, and on a console a comparison and a call.
	 */
	if (g_present < 0 && !plat_can_patch_code()) present_look(desc_now());

	if (!g_set_aside.armed && !g_load.armed && g_xfer.state == SF_IDLE) return;

	/*
	 * Nothing can be armed without the gate having passed, which on a platform
	 * that refuses code patches means the RPCS3 patch was found (build 47), so
	 * the platform is not asked again here: a request there is watched exactly
	 * as one on a console is.
	 */
	if (!mem_is_ingame()) {
		if (g_xfer.state != SF_IDLE) xfer_stop(SAVEFILE_ERR_NO_HELPER);
		return;
	}

	d = desc_now();
	if (d == NULL) {
		if (g_xfer.state != SF_IDLE) xfer_stop(SAVEFILE_ERR_NO_HELPER);
		return;
	}

	request_tick(&g_set_aside, d->api_setaside);
	request_tick(&g_load, d->api_load);
	xfer_tick(d);
}

int savefile_read(u32 offset, u32 len, u8 *out, u32 *outlen)
{
	const struct sf_desc *d;
	int rc = gate(&d);

	*outlen = 0;
	if (rc != ST_OK) return rc;
	if (len == 0 || len > PLAT_MEM_MAX) return ST_BAD_ARG;
	if (offset >= d->aside_size) return ST_BAD_ARG;

	/* A read that runs off the end is trimmed, so a client can ask for a round
	 * chunk at every offset and let the last one come back short. */
	if (len > d->aside_size - offset) len = d->aside_size - offset;

	rc = mem_read(d->aside_addr + offset, out, len);
	if (rc != ST_OK) return rc;

	*outlen = len;
	return ST_OK;
}

int savefile_write(u32 offset, const u8 *data, u32 len)
{
	const struct sf_desc *d;
	int rc = gate(&d);

	if (rc != ST_OK) return rc;
	if (len == 0 || len > PLAT_MEM_MAX) return ST_BAD_ARG;
	if (offset >= d->aside_size) return ST_BAD_ARG;

	/* Unlike a read, a write past the end is refused rather than trimmed: a
	 * client sending more than the buffer holds has the wrong file. */
	if (len > d->aside_size - offset) return ST_BAD_ARG;

	return mem_write(d->aside_addr + offset, data, len);
}

static int request(int load)
{
	const struct sf_desc *d;
	struct sf_request *r;
	int rc = gate(&d);
	u8 why;

	if (rc != ST_OK) return rc;

	/*
	 * Build 43. Nothing is raised at a game that is not sitting still. The
	 * helper makes the same test on the frame it would act, because the game
	 * gets frames of its own between this tick and that one, but asking here
	 * as well is what lets a client say why: a BUSY now, at the moment the user
	 * pressed the button, instead of a request that quietly comes to nothing.
	 * Build 53: or that has no save to act on (SAVEFILE_ERR_NO_SAVE).
	 */
	why = game_ready(load ? GAME_SF_LOAD : GAME_SF_SET_ASIDE);
	if (why != SAVEFILE_ERR_NONE) {
		g_xfer.error = why;
		return ST_BUSY;
	}

	rc = savefile_install();
	if (rc != ST_OK) return rc;

	rc = mem_write_u8(load ? d->api_load : d->api_setaside, 1);
	if (rc != ST_OK) return rc;

	/*
	 * Pending from here, not from the first tick that reads the byte back. The
	 * bit is then qwark's own knowledge that it asked for something, which no
	 * bad read of the game's memory can talk it out of.
	 */
	r = load ? &g_load : &g_set_aside;
	r->armed = 1;
	r->settle = 0;
	r->refused = 0;
	return ST_OK;
}

int savefile_set_aside(void) { return request(0); }
int savefile_load_aside(void) { return request(1); }

int savefile_load_ready(void)
{
	const struct sf_desc *d;
	int rc = gate(&d);
	u8 why;

	if (rc != ST_OK) return rc;

	why = game_ready(GAME_SF_LOAD);
	if (why != SAVEFILE_ERR_NONE) {
		g_xfer.error = why;
		return ST_BUSY;
	}
	return ST_OK;
}

u32 savefile_load_refusals(void)
{
	return g_load_refusals;
}

/* ------------------------------------------ the RPCS3 patch, revision 1.15 */

#ifdef QWARK_EMULATOR   /* build 51: the PS3 answers SAVEFILE_PATCH UNSUPPORTED */

/*
 * How many words a cave takes on the wire. Every cave is whole words today:
 * build.sh cuts .text and .text.stub, which hold nothing but instructions, so
 * RaC1's 116 and 920 bytes, RaC2's 964, RaC3's 976 and Deadlocked's 40 and
 * 1004 (build 53) are all multiples of four and none of them is padded. The
 * rounding is there for a cave that one day is not, whose last word then
 * carries its tail and zeroes after it rather than losing the tail.
 */
static u32 cave_words(const struct sf_cave *c)
{
	return (c->len + 3u) / 4u;
}

int savefile_patch(u8 *out, u32 cap, u32 *len)
{
	const struct sf_desc *d;
	u32 n = 0;
	u32 off = SAVEFILE_PATCH_HEAD;
	u32 need;
	u32 bytes[SAVEFILE_PATCH_NBYTES];
	u8 i;

	*len = 0;

	/*
	 * The gate the other savefile ops use, bar the one test: the platform is not
	 * asked and neither is the game. This describes the helper; it does not need
	 * one to be there, and on a console it is the same words the install writes.
	 */
	if (!g_enabled) return ST_UNSUPPORTED;
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;

	for (i = 0; i < d->ncaves; i++) n += cave_words(&d->caves[i]);
	n += d->nhooks;

	need = SAVEFILE_PATCH_HEAD + n * SAVEFILE_PATCH_ROW
	     + SAVEFILE_PATCH_BYTES_HEAD + SAVEFILE_PATCH_NBYTES * SAVEFILE_PATCH_BYTE_ROW;
	if (n > 0xFFFFu || need > cap) return ST_FULL;

	/*
	 * Caves first and hook words last, the order the install writes them in and
	 * so the order a patch file lists them in: a reader applying the words one
	 * at a time never has a hook in place before the cave it branches to.
	 */
	for (i = 0; i < d->ncaves; i++) {
		const struct sf_cave *c = &d->caves[i];
		u32 at;

		for (at = 0; at < c->len; at += 4u) {
			u32 take = c->len - at;
			u32 k;

			if (take > 4u) take = 4u;

			be32_put(out + off, c->addr + at);
			/*
			 * The cave's bytes go on the wire as they are, which is the word the
			 * game reads there: both are the PS3's big-endian order.
			 */
			for (k = 0; k < 4u; k++) out[off + 4u + k] = k < take ? c->bytes[at + k] : 0;
			off += SAVEFILE_PATCH_ROW;
		}
	}

	for (i = 0; i < d->nhooks; i++) {
		be32_put(out + off, d->hooks[i].addr);
		be32_put(out + off + 4u, d->hooks[i].value);
		off += SAVEFILE_PATCH_ROW;
	}

	/*
	 * Then the three request bytes, at 0, in the order a console install zeroes
	 * them before it hooks anything. A console clears them because whatever the
	 * executable holds there would fire the moment the hook goes in, and under
	 * RPCS3 the hook is in from the first frame with no install to clear them,
	 * so the patch has to. Until build 50 RaC2's three sat in the executable's
	 * code segment and loaded as 0x80 0xFE 0xA4, which INFO reported from boot
	 * as a load and a set-aside still outstanding, two pending bits that never
	 * cleared, with the helper's own refusal code (SAVEFILE_HELPER_REFUSED) in
	 * the load byte. Every game's load as zero now - RaC2's moved out of the
	 * code segment, which RPCS3 makes read-only (sf_rac2.h), and the other
	 * three are in zero-filled BSS - and all four still carry them, for the
	 * next executable that does not.
	 *
	 * They are no part of the presence look: the helper and qwark change them
	 * from the first frame on, so what they read later says nothing about
	 * whether the patch went in.
	 */
	bytes[0] = d->api_mod;
	bytes[1] = d->api_load;
	bytes[2] = d->api_setaside;

	be16_put(out + off, SAVEFILE_PATCH_NBYTES);
	be16_put(out + off + 2u, 0);
	off += SAVEFILE_PATCH_BYTES_HEAD;

	for (i = 0; i < SAVEFILE_PATCH_NBYTES; i++) {
		be32_put(out + off, bytes[i]);
		out[off + 4u] = 0;
		out[off + 5u] = 0;
		out[off + 6u] = 0;
		out[off + 7u] = 0;
		off += SAVEFILE_PATCH_BYTE_ROW;
	}

	/*
	 * The stamp is the CRC of everything after it exactly as it went out, words
	 * and bytes alike, so it moves when anything the patch writes does and at no
	 * other time. A client keeps it beside the patch it wrote and knows from it
	 * alone whether that patch is still this build's helper, without comparing a
	 * kilobyte of words.
	 */
	be16_put(out, (u16)n);
	be16_put(out + 2, 0);
	be32_put(out + 4, qcrc32(out + SAVEFILE_PATCH_HEAD, off - SAVEFILE_PATCH_HEAD));

	*len = off;
	return ST_OK;
}

#endif /* QWARK_EMULATOR */

/* ------------------------------------------------------ the console library
 *
 * /dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav, with <name>.sav.sum
 * beside each file holding eight hex digits of CRC32. The layout is the mod
 * library's, and for the same reason: the client uploads a file once and the
 * console owns it from then on.
 */

int savefile_name_ok(const char *name)
{
	u32 n;
	u32 i;

	if (name == NULL) return 0;

	n = qstrlen(name);
	if (n == 0 || n > SAVEFILE_NAME_LEN) return 0;

	/*
	 * A leading dot is "." and ".." and every hidden name at once, and a
	 * separator or a colon is a path the caller did not mean to build. Anything
	 * outside printable ASCII is refused rather than passed to cellFs.
	 */
	if (name[0] == '.') return 0;

	for (i = 0; i < n; i++) {
		char c = name[i];
		if (c == '/' || c == '\\' || c == ':') return 0;
		if (c < 0x20 || (u8)c > 0x7E) return 0;
	}

	return 1;
}

/* A save is a <name>.sav; anything else in the folder is not one. */
static int has_sav_ext(const char *name)
{
	u32 n = qstrlen(name);
	u32 e = qstrlen(SAVEFILE_EXT);

	if (n <= e) return 0;
	return qstreq(name + (n - e), SAVEFILE_EXT);
}

static int has_sum_ext(const char *name)
{
	u32 n = qstrlen(name);
	u32 e = qstrlen(SAVEFILE_SUM_EXT);

	if (n <= e) return 0;
	return qstreq(name + (n - e), SAVEFILE_SUM_EXT);
}

/*
 * <QWARK_SAVEDIR>/<title>[/<category>[/<name>]]. The title is the running
 * game's, so a category or a file is only ever reachable while that game is up,
 * which is the same rule the whole savefile block already answers to.
 */
static void save_path(char *out, u32 cap, const char *category, const char *name)
{
	out[0] = 0;
	qstrcat(out, cap, QWARK_SAVEDIR);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, session_title());

	if (category == NULL) return;
	qstrcat(out, cap, "/");
	qstrcat(out, cap, category);

	if (name == NULL) return;
	qstrcat(out, cap, "/");
	qstrcat(out, cap, name);
}

/* One NUL-padded 32-byte name field, which is never terminated when it is full. */
static void put_name(u8 *dst, const char *src)
{
	u32 i = 0;

	memset(dst, 0, SAVEFILE_NAME_LEN);
	while (i < SAVEFILE_NAME_LEN && src[i] != 0) { dst[i] = (u8)src[i]; i++; }
}

/*
 * Whether anything already owns the aside buffer: a transfer of our own, or a
 * request the game has not answered yet, which is what a Force autosave or a
 * combo-driven load leaves outstanding.
 */
static int buffer_busy(void)
{
	return g_xfer.state != SF_IDLE || g_set_aside.armed || g_load.armed;
}

/*
 * The shared front half of STORE and RESTORE. `what` is GAME_SF_SET_ASIDE for a
 * STORE and GAME_SF_RESTORE for a RESTORE (build 53).
 */
static int transfer_gate(const struct sf_desc **d, const char *category, const char *name, u8 what)
{
	int rc = gate(d);
	u8 why;

	if (rc != ST_OK) return rc;
	if (!savefile_name_ok(category) || !savefile_name_ok(name)) return ST_BAD_ARG;
	/*
	 * The listing only reports <name>.sav, so a file stored under any other name
	 * would be one the client could never see again. Refuse it here instead.
	 */
	if (!has_sav_ext(name)) return ST_BAD_ARG;

	if (buffer_busy()) {
		g_xfer.error = SAVEFILE_ERR_BUSY;
		return ST_BUSY;
	}

	/*
	 * Build 43. Refused here rather than part way in: a STORE that got as far
	 * as the request and then found the game busy would have created the file
	 * already, and a RESTORE would have read two megabytes off the disc for a
	 * load nothing was going to take. Build 53: a STORE of a game with no save
	 * yet is refused here too, with SAVEFILE_ERR_NO_SAVE.
	 */
	why = game_ready(what);
	if (why != SAVEFILE_ERR_NONE) {
		g_xfer.error = why;
		return ST_BUSY;
	}

	return ST_OK;
}

static void xfer_begin(u8 state, u32 total, const char *path)
{
	g_xfer.state = state;
	g_xfer.error = SAVEFILE_ERR_NONE;
	g_xfer.done  = 0;
	g_xfer.total = total;
	g_xfer.crc   = qcrc32_start();
	qstrcpy(g_xfer.path, sizeof(g_xfer.path), path);
}

int savefile_store(const char *category, const char *name)
{
	const struct sf_desc *d;
	char path[SAVEFILE_PATH_MAX];
	int rc = transfer_gate(&d, category, name, GAME_SF_SET_ASIDE);

	if (rc != ST_OK) return rc;

	/* The three levels of the library, since the first save makes all of them. */
	plat_dir_create(QWARK_SAVEDIR);
	save_path(path, sizeof(path), NULL, NULL);
	plat_dir_create(path);
	save_path(path, sizeof(path), category, NULL);
	plat_dir_create(path);

	save_path(path, sizeof(path), category, name);
	xfer_begin(SF_STORE_SETTLE, d->aside_size, path);

	/*
	 * The request goes out last, so a failure above leaves nothing outstanding
	 * at the game. The file is not opened until the settle window says the copy
	 * inside the game is over: a file created now and filled later is a file
	 * that exists and is empty for the length of the wait.
	 */
	rc = savefile_set_aside();
	if (rc != ST_OK) {
		xfer_stop(rc == ST_BUSY ? refusal_error() : SAVEFILE_ERR_IO);
		return rc;
	}

	plat_log("savefile: store %s", g_xfer.path);
	return ST_OK;
}

int savefile_restore(const char *category, const char *name)
{
	const struct sf_desc *d;
	char path[SAVEFILE_PATH_MAX];
	plat_file_t f;
	u64 size = 0;
	int is_dir = 0;
	int rc = transfer_gate(&d, category, name, GAME_SF_RESTORE);

	if (rc != ST_OK) return rc;

	save_path(path, sizeof(path), category, name);

	if (!plat_path_exists(path, &is_dir, &size) || is_dir) {
		g_xfer.error = SAVEFILE_ERR_MISSING;
		return ST_NOT_FOUND;
	}

	/*
	 * A save for a game is one fixed length. Anything else is a file for another
	 * game or one that was truncated on the way in, and the game would take the
	 * buffer either way, so nothing is raised at it: the error says why.
	 */
	if (size != (u64)d->aside_size) {
		g_xfer.error = SAVEFILE_ERR_SHORT;
		return ST_BAD_ARG;
	}

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) {
		g_xfer.error = SAVEFILE_ERR_IO;
		return ST_IO_ERROR;
	}

	/* A valid restore needs the game-side loader. Metadata queries and missing
	 * or invalid files must not install code as a side effect. */
	rc = savefile_install();
	if (rc != ST_OK) {
		plat_file_close(f);
		return rc;
	}

	xfer_begin(SF_RESTORE_COPY, d->aside_size, path);
	g_xfer.file = f;
	g_xfer.have_file = 1;

	plat_log("savefile: restore %s", g_xfer.path);
	return ST_OK;
}

/* ------------------------------------------- the library ops, network thread */

int savefile_library_gate(void)
{
	const struct sf_desc *d;
	/*
	 * Network thread: the answer the tick thread's look found, never a look of
	 * its own. The look is taken on the first INGAME tick, so the BUSY it gives
	 * before then lasts a tick, or for as long as the emulator will not answer.
	 */
	int rc = gate_as(&d, 0);

	if (rc != ST_OK) return rc;

	/*
	 * A listing taken while qwark is writing a file would report a size and a
	 * sum for half a save. There is one transfer and the client that started it
	 * is the one asking, so refusing is both correct and cheap; it is also the
	 * whole of the arbitration between this thread and the tick thread.
	 */
	if (g_xfer.state != SF_IDLE) return ST_BUSY;

	return ST_OK;
}

int savefile_categories(u8 *out, u32 cap, u32 *len)
{
	char path[SAVEFILE_PATH_MAX];
	plat_dir_t dir;
	struct plat_dirent ent;
	u32 off = 1;
	u8 n = 0;

	*len = 0;
	if (cap < 1) return ST_FULL;

	save_path(path, sizeof(path), NULL, NULL);

	/* No folder yet is an empty library, not an error: nothing has been saved. */
	if (plat_dir_open(path, &dir) != 0) {
		out[0] = 0;
		*len = 1;
		return ST_OK;
	}

	while (plat_dir_next(&dir, &ent) == 1) {
		if (!ent.is_dir) continue;
		if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;
		if (!savefile_name_ok(ent.name)) continue;
		if (off + SAVEFILE_NAME_LEN > cap || n == 0xFF) break;

		put_name(out + off, ent.name);
		off += SAVEFILE_NAME_LEN;
		n++;
	}

	plat_dir_close(&dir);

	out[0] = n;
	*len = off;
	return ST_OK;
}

/*
 * The CRC of a file that has no sidecar yet, which is every file the client
 * uploaded itself. Computed once and written out, so the next listing reads it.
 *
 * The buffer is on the stack on purpose: a client thread has 16 KB of it and
 * this is one of two frames deep, while the shared scratch buffer belongs to the
 * tick thread and must never be borrowed from another one.
 */
static int crc_of_file(const char *path, u32 *out)
{
	u8 chunk[4096];
	plat_file_t f;
	u32 state = qcrc32_start();

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return ST_NOT_FOUND;

	for (;;) {
		u32 got = 0;
		if (plat_file_read(f, chunk, sizeof(chunk), &got) != 0) {
			plat_file_close(f);
			return ST_IO_ERROR;
		}
		if (got == 0) break;
		state = qcrc32_update(state, chunk, got);
	}

	plat_file_close(f);
	*out = qcrc32_finish(state);
	return ST_OK;
}

/* The sum beside a file, or 0 when there is not one (or it is unreadable). */
static u32 sum_read(const char *path)
{
	char sum_path[SAVEFILE_PATH_MAX];
	char text[32];
	u8 raw[4];

	qstrcpy(sum_path, sizeof(sum_path), path);
	qstrcat(sum_path, sizeof(sum_path), SAVEFILE_SUM_EXT);

	if (qread_file(sum_path, text, sizeof(text), NULL) != ST_OK) return 0;
	if (qhex_to_bytes(qtrim(text), raw, 4) != 4) return 0;

	return be32_get(raw);
}

int savefile_list(const char *category, u8 *out, u32 cap, u32 *len)
{
	char folder[SAVEFILE_PATH_MAX];
	plat_dir_t dir;
	struct plat_dirent ent;
	u32 off = 1;
	u8 n = 0;

	*len = 0;
	if (cap < 1) return ST_FULL;
	if (!savefile_name_ok(category)) return ST_BAD_ARG;

	save_path(folder, sizeof(folder), category, NULL);
	if (plat_dir_open(folder, &dir) != 0) return ST_NOT_FOUND;

	while (plat_dir_next(&dir, &ent) == 1) {
		char path[SAVEFILE_PATH_MAX];
		u32 crc;

		if (ent.is_dir) continue;
		/* Only saves are listed; the .sum sidecars are qwark's own bookkeeping. */
		if (!has_sav_ext(ent.name)) continue;
		if (!savefile_name_ok(ent.name)) continue;
		if (off + SAVEFILE_ROW_SIZE > cap || n == 0xFF) break;

		save_path(path, sizeof(path), category, ent.name);

		crc = sum_read(path);
		if (crc == 0) {
			/*
			 * No sidecar: a file the client uploaded. Sum it once and write the
			 * sum out, so this costs a read of the file the first time it is
			 * listed and nothing every time after that.
			 */
			if (crc_of_file(path, &crc) != ST_OK) continue;
			sum_write(path, crc);
		}

		put_name(out + off, ent.name);
		be32_put(out + off + SAVEFILE_NAME_LEN, (u32)ent.size);
		be32_put(out + off + SAVEFILE_NAME_LEN + 4, crc);
		off += SAVEFILE_ROW_SIZE;
		n++;
	}

	plat_dir_close(&dir);

	out[0] = n;
	*len = off;
	return ST_OK;
}

int savefile_category(u8 op, const char *name)
{
	char path[SAVEFILE_PATH_MAX];

	if (!savefile_name_ok(name)) return ST_BAD_ARG;

	if (op == SAVEFILE_CATEGORY_CREATE) {
		plat_dir_create(QWARK_SAVEDIR);
		save_path(path, sizeof(path), NULL, NULL);
		plat_dir_create(path);
		save_path(path, sizeof(path), name, NULL);
		plat_dir_create(path);
		return plat_path_exists(path, NULL, NULL) ? ST_OK : ST_IO_ERROR;
	}

	if (op != SAVEFILE_CATEGORY_DELETE) return ST_BAD_ARG;

	save_path(path, sizeof(path), name, NULL);
	if (!plat_path_exists(path, NULL, NULL)) return ST_NOT_FOUND;

	/*
	 * The sidecars first. A client deletes a save with FILE_DELETE and may or
	 * may not remember its .sum, and a folder that cannot be removed because of
	 * a file the user never made is a bad answer. A save that is still there is
	 * a different matter: the rmdir below fails and the category stays.
	 */
	{
		plat_dir_t dir;
		struct plat_dirent ent;

		if (plat_dir_open(path, &dir) == 0) {
			while (plat_dir_next(&dir, &ent) == 1) {
				char child[SAVEFILE_PATH_MAX];
				if (ent.is_dir || !has_sum_ext(ent.name)) continue;
				save_path(child, sizeof(child), name, ent.name);
				plat_file_unlink(child);
			}
			plat_dir_close(&dir);
		}
	}

	return plat_dir_remove(path) == 0 ? ST_OK : ST_IO_ERROR;
}
