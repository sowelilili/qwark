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
	u16 settle;
};

static struct sf_request g_set_aside;
static struct sf_request g_load;

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
	request_clear(&g_set_aside);
	request_clear(&g_load);

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
 * The gate every entry point starts with. UNSUPPORTED for a platform that
 * refuses code patches, because the helper *is* a code patch; NOT_INGAME
 * outside INGAME, because there is no process to write into.
 */
static int gate(const struct sf_desc **out)
{
	const struct sf_desc *d;

	*out = NULL;

	if (!g_enabled) return ST_UNSUPPORTED;
	if (!plat_can_patch_code()) return ST_UNSUPPORTED;
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;

	*out = d;
	return ST_OK;
}

int savefile_install(void)
{
	const struct sf_desc *d;
	int rc = gate(&d);
	u8 i;

	if (rc != ST_OK) return rc;
	if (g_installed) return ST_OK;

	/*
	 * A request byte left over from whatever used to live at these addresses
	 * would fire the moment the hook goes in, so the three go to zero while
	 * nothing is reading them yet.
	 */
	if (mem_write_u8(d->api_mod, 0) != ST_OK ||
	    mem_write_u8(d->api_load, 0) != ST_OK ||
	    mem_write_u8(d->api_setaside, 0) != ST_OK) return ST_IO_ERROR;

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

	if (!plat_can_patch_code()) return ST_UNSUPPORTED;
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

	/* Discovery must not patch a newly booted game. Until an action installs
	 * the helper, its request bytes may contain unrelated data. */
	*installed = (u8)(g_installed != 0);
	if (!g_installed) return ST_OK;

	if (mem_read_u8(d->api_mod, &byte) == ST_OK && byte == 1) *running = 1;

	/*
	 * The bytes are read here as well as on the tick, so a request something
	 * other than qwark started - RaC1's Force autosave drives a byte of its own,
	 * and Deadlocked's load parks a 2 in api_load for the length of its menu - is
	 * reported outstanding too. What the settle window adds is the other
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

	switch (g_xfer.state) {

	case SF_STORE_SETTLE:
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

		/* The whole file is in the buffer, so now the game may have it. */
		if (savefile_load_aside() != ST_OK) {
			xfer_stop(SAVEFILE_ERR_IO);
			return;
		}
		plat_log("savefile: restored %d bytes, load requested", (int)g_xfer.total);
		g_xfer.state = SF_RESTORE_LOAD;
		return;

	case SF_RESTORE_LOAD:
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

	if (!g_set_aside.armed && !g_load.armed && g_xfer.state == SF_IDLE) return;

	if (!plat_can_patch_code() || !mem_is_ingame()) {
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

	if (rc != ST_OK) return rc;

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
	return ST_OK;
}

int savefile_set_aside(void) { return request(0); }
int savefile_load_aside(void) { return request(1); }

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

/* The shared front half of STORE and RESTORE. */
static int transfer_gate(const struct sf_desc **d, const char *category, const char *name)
{
	int rc = gate(d);

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
	int rc = transfer_gate(&d, category, name);

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
		xfer_stop(SAVEFILE_ERR_IO);
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
	int rc = transfer_gate(&d, category, name);

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
	int rc = gate(&d);

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
