/*
 * ui.pak's reader (build 55). uipak.h says what the file is for; PROTOCOL.md
 * section 9.10 and src/games/menuhelper/pak.py say how it is laid out.
 *
 * Everything here runs on the tick thread but uipak_touch, uipak_names,
 * uipak_status and uipak_bad, which a network thread calls: the first sets a
 * flag the next tick reads, the others read one int. The pak is read through
 * the tick's scratch buffer and is open only inside one call, or between
 * mh_pak_open and mh_pak_close inside one install.
 */
#include "uipak.h"
#include "proto.h"
#include "util.h"

#include <string.h>

static volatile int g_status = UIPAK_UNCHECKED;
/* Set by the module's start and by uipak_touch; the tick looks and clears it. */
static volatile int g_look = 1;
static int g_started;
static u32 g_looks;

/* The running game's renderer, as mh_pak_desc last read it. */
static struct mh_desc g_desc;
static struct mh_cave g_caves[UIPAK_MAX_CAVES];
static struct mh_word g_hooks[UIPAK_MAX_HOOKS];
/* Where each cave's bytes start in the file, and the game's whole part. */
static u32 g_cave_at[UIPAK_MAX_CAVES];
static u32 g_entry_at;
static u32 g_entry_len;
static u32 g_entry_crc;

static plat_file_t g_file = PLAT_FILE_INVALID;

int uipak_status(void) { return g_status; }
u32 uipak_looks(void)  { return g_looks; }

int uipak_bad(void)
{
	int s = g_status;
	return s == UIPAK_MISSING || s == UIPAK_OTHER || s == UIPAK_DAMAGED;
}

/* The pak itself, or a folder above it, which a DIR_DELETE takes it with. */
int uipak_names(const char *path)
{
	u32 n;

	if (path == NULL) return 0;
	if (qstreq(path, UIPAK_PATH)) return 1;
	n = qstrlen(path);
	while (n > 1 && path[n - 1] == '/') n--;
	return n < sizeof(UIPAK_PATH) - 1 && qstrneq(path, UIPAK_PATH, n) && UIPAK_PATH[n] == '/';
}

void uipak_touch(const char *path)
{
	if (uipak_names(path)) g_look = 1;
}

/*
 * The header's verdict: DAMAGED for something that is not a format 1 pak,
 * OTHER for a pak of another build or other renderers, else OK with the body's
 * length and the entries.
 */
static int header_verdict(const u8 *h, u32 *body, u32 *entries)
{
	u32 n = h[20];

	if (be32_get(h) != UIPAK_MAGIC || be16_get(h + 4) != UIPAK_FORMAT ||
	    be16_get(h + 6) != UIPAK_HEADER_SIZE)
		return UIPAK_DAMAGED;
	if (be32_get(h + 8) != QWARK_BUILD || be32_get(h + 12) != MH_PAK_ID) return UIPAK_OTHER;
	*body = be32_get(h + 16);
	*entries = n;
	if (n == 0 || n > UIPAK_MAX_ENTRIES || *body < n * UIPAK_TOC_SIZE || *body > UIPAK_MAX_BODY)
		return UIPAK_DAMAGED;
	return UIPAK_OK;
}

/* Every entry of the table of contents inside the body and after the table. */
static int toc_ok(const u8 *toc, u32 body, u32 n)
{
	u32 i, at, len, end = UIPAK_HEADER_SIZE + body;

	for (i = 0; i < n; i++) {
		at = be32_get(toc + i * UIPAK_TOC_SIZE + 16);
		len = be32_get(toc + i * UIPAK_TOC_SIZE + 20);
		if (at < UIPAK_HEADER_SIZE + n * UIPAK_TOC_SIZE || len < UIPAK_DESC_SIZE ||
		    at > end || len > end - at)
			return 0;
	}
	return 1;
}

/*
 * The whole file: its header, then every byte after it against the header's
 * CRC and length, through the scratch. `h` gets the header for the log.
 */
static int look_at(const char *path, u8 *h)
{
	u8 *buf = qscratch();
	plat_file_t f;
	u32 got, off = 0, body = 0, n = 0, crc;
	int st;

	memset(h, 0, UIPAK_HEADER_SIZE);
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return UIPAK_MISSING;

	st = UIPAK_DAMAGED;
	if (plat_file_read(f, h, UIPAK_HEADER_SIZE, &got) != 0 || got != UIPAK_HEADER_SIZE) goto out;
	st = header_verdict(h, &body, &n);
	if (st != UIPAK_OK) goto out;

	st = UIPAK_DAMAGED;
	crc = qcrc32_start();
	for (;;) {
		if (plat_file_read(f, buf, QSCRATCH_BYTES, &got) != 0) goto out;
		if (got == 0) break;
		if (off == 0 && (got < n * UIPAK_TOC_SIZE || !toc_ok(buf, body, n))) goto out;
		off += got;
		if (off > body) goto out;
		crc = qcrc32_update(crc, buf, got);
	}
	if (off == body && qcrc32_finish(crc) == be32_get(h + 12)) st = UIPAK_OK;
out:
	plat_file_close(f);
	return st;
}

/* A new verdict, logged when it differs from the last. */
static void verdict(int st, const u8 *h)
{
	if (st == g_status) return;
	g_status = st;
	switch (st) {
	case UIPAK_OK:
		plat_log("ui.pak: build %d, %d renderers", (int)QWARK_BUILD, (int)(h != NULL ? h[20] : 0));
		break;
	case UIPAK_MISSING:
		plat_log("ui.pak: none at " UIPAK_PATH "; no Trainer menu in a game");
		break;
	case UIPAK_OTHER:
		plat_log("ui.pak: build %d, renderers 0x%08X, not this module's (build %d, 0x%08X); no Trainer menu",
		         (int)(h != NULL ? be32_get(h + 8) : 0), (int)(h != NULL ? be32_get(h + 12) : 0),
		         (int)QWARK_BUILD, (int)MH_PAK_ID);
		break;
	default:
		plat_log("ui.pak: damaged or cut short; no Trainer menu");
		break;
	}
}

/* The module's start: a pak an update left as ui.pak.new, when it is this build's, becomes ui.pak. */
static void take_new(void)
{
	u8 h[UIPAK_HEADER_SIZE];

	if (!plat_path_exists(UIPAK_NEW_PATH, NULL, NULL)) return;
	if (look_at(UIPAK_NEW_PATH, h) != UIPAK_OK) {
		plat_log("ui.pak: ui.pak.new is not this build's; left where it is");
		return;
	}
	plat_file_unlink(UIPAK_PATH);
	if (plat_file_rename(UIPAK_NEW_PATH, UIPAK_PATH) == 0) plat_log("ui.pak: took ui.pak.new, which an update left");
	else plat_log("ui.pak: could not put ui.pak.new in place of ui.pak");
}

#ifdef QWARK_TEST
/* The module's start again, for test_uipak.c: ui.pak.new taken, the pak looked at. */
void uipak_test_start(void)
{
	g_started = 0;
	g_look = 1;
	g_status = UIPAK_UNCHECKED;
}
#endif

void uipak_tick(void)
{
	u8 h[UIPAK_HEADER_SIZE];
	int st;

	if (!g_look) return;
	g_look = 0;
	/* RPCS3 never has a menu, so it never needs the pak and never says it lacks one. */
	if (!plat_can_patch_code()) return;

	if (!g_started) {
		g_started = 1;
		take_new();
	}
	st = look_at(UIPAK_PATH, h);
	verdict(st, h);
	g_looks++;
}

/* ------------------------------------------------------------- the seam */

/*
 * What the seam found, for the flag. A pak that cannot serve is the pak's
 * state at once; one that served while the last look said otherwise (copied
 * over by hand since) is looked at whole at the next tick, which decides.
 */
static void settle(int st, const u8 *h)
{
	if (st != UIPAK_OK) verdict(st, h);
	else if (g_status != UIPAK_OK) g_look = 1;
}

void mh_pak_close(void)
{
	if (g_file == PLAT_FILE_INVALID) return;
	plat_file_close(g_file);
	g_file = PLAT_FILE_INVALID;
}

const struct mh_desc *mh_pak_desc(u8 game_id, int *status)
{
	u8 *buf = qscratch();
	u8 h[UIPAK_HEADER_SIZE];
	const struct mh_desc *d = NULL;
	plat_file_t f;
	const u8 *e, *t;
	u32 got, body = 0, n = 0, i, at = 0, len = 0, crc = 0, want, rec;
	int st;

	mh_pak_close();
	memset(h, 0, sizeof(h));
	if (plat_file_open(UIPAK_PATH, PLAT_OPEN_READ, &f) != 0) {
		st = UIPAK_MISSING;
		goto done;
	}

	st = UIPAK_DAMAGED;
	if (plat_file_read_at(f, 0, buf, UIPAK_HEADER_SIZE + UIPAK_MAX_ENTRIES * UIPAK_TOC_SIZE, &got) != 0 ||
	    got < UIPAK_HEADER_SIZE)
		goto out;
	memcpy(h, buf, sizeof(h));
	st = header_verdict(h, &body, &n);
	if (st != UIPAK_OK) goto out;
	st = UIPAK_DAMAGED;
	if (got < UIPAK_HEADER_SIZE + n * UIPAK_TOC_SIZE || !toc_ok(buf + UIPAK_HEADER_SIZE, body, n)) goto out;

	for (i = 0; i < n; i++) {
		t = buf + UIPAK_HEADER_SIZE + i * UIPAK_TOC_SIZE;
		if (t[0] != game_id || t[1] != UIPAK_KIND_RENDERER) continue;
		at = be32_get(t + 16);
		len = be32_get(t + 20);
		crc = be32_get(t + 24);
		break;
	}
	if (i == n) {
		/* A pak of this build with no renderer for the game: no menu, as before the pak. */
		st = UIPAK_OK;
		goto out;
	}

	/* The descriptor and the records, which the reader has room for or the entry is not one. */
	want = UIPAK_DESC_SIZE + UIPAK_MAX_CAVES * UIPAK_CAVE_SIZE + UIPAK_MAX_HOOKS * UIPAK_HOOK_SIZE;
	if (want > len) want = len;
	if (plat_file_read_at(f, at, buf, want, &got) != 0 || got != want) goto out;
	e = buf;
	memset(&g_desc, 0, sizeof(g_desc));
	g_desc.ncaves = e[36];
	g_desc.nhooks = e[37];
	g_desc.nhelp = e[38];
	rec = UIPAK_DESC_SIZE + (u32)g_desc.ncaves * UIPAK_CAVE_SIZE + (u32)g_desc.nhooks * UIPAK_HOOK_SIZE;
	if (g_desc.ncaves == 0 || g_desc.ncaves > UIPAK_MAX_CAVES || g_desc.nhooks == 0 ||
	    g_desc.nhooks > UIPAK_MAX_HOOKS || g_desc.nhelp > g_desc.nhooks || rec > want)
		goto out;

	for (i = 0; i < g_desc.ncaves; i++) {
		const u8 *c = e + UIPAK_DESC_SIZE + i * UIPAK_CAVE_SIZE;
		u32 off = be32_get(c + 4), clen = be32_get(c + 8);

		if (off < rec || off > len || clen > len - off) goto out;
		g_caves[i].addr = be32_get(c);
		g_caves[i].bytes = NULL;
		g_caves[i].len = clen;
		g_caves[i].zeros = be32_get(c + 12);
		g_caves[i].code = c[16];
		g_cave_at[i] = at + off;
	}
	for (i = 0; i < g_desc.nhooks; i++) {
		const u8 *w = e + UIPAK_DESC_SIZE + (u32)g_desc.ncaves * UIPAK_CAVE_SIZE + i * UIPAK_HOOK_SIZE;

		g_hooks[i].addr = be32_get(w);
		g_hooks[i].value = be32_get(w + 4);
		g_hooks[i].original = be32_get(w + 8);
	}
	g_desc.game_id = game_id;
	g_desc.caves = g_caves;
	g_desc.hooks = g_hooks;
	g_desc.mailbox = be32_get(e);
	g_desc.mailbox_size = be32_get(e + 4);
	g_desc.state = be32_get(e + 8);
	g_desc.mode_addr = be32_get(e + 12);
	g_desc.mode_paused = be32_get(e + 16);
	g_desc.pending_addr = be32_get(e + 20);
	g_desc.pending_none = be32_get(e + 24);
	g_desc.screen_addr = be32_get(e + 28);
	g_desc.screen_id = be32_get(e + 32);
	g_entry_at = at;
	g_entry_len = len;
	g_entry_crc = crc;
	d = &g_desc;
	st = UIPAK_OK;
out:
	plat_file_close(f);
done:
	settle(st, h);
	*status = st;
	return d;
}

int mh_pak_open(void)
{
	u8 *buf = qscratch();
	u32 got, off, n, body, entries, crc;
	int st;

	mh_pak_close();
	if (g_desc.caves == NULL) return UIPAK_MISSING;
	if (plat_file_open(UIPAK_PATH, PLAT_OPEN_READ, &g_file) != 0) {
		g_file = PLAT_FILE_INVALID;
		verdict(UIPAK_MISSING, NULL);
		return UIPAK_MISSING;
	}

	st = UIPAK_DAMAGED;
	if (plat_file_read_at(g_file, 0, buf, UIPAK_HEADER_SIZE, &got) == 0 && got == UIPAK_HEADER_SIZE) {
		st = header_verdict(buf, &body, &entries);
		if (st == UIPAK_OK && g_entry_at + g_entry_len > UIPAK_HEADER_SIZE + body) st = UIPAK_DAMAGED;
	}
	if (st == UIPAK_OK) {
		/* The game's part, whole: what the install writes is exactly what pak.py packed. */
		crc = qcrc32_start();
		for (off = 0; off < g_entry_len && st == UIPAK_OK; off += n) {
			n = g_entry_len - off;
			if (n > QSCRATCH_BYTES) n = QSCRATCH_BYTES;
			if (plat_file_read_at(g_file, g_entry_at + off, buf, n, &got) != 0 || got != n) st = UIPAK_DAMAGED;
			else crc = qcrc32_update(crc, buf, n);
		}
		if (st == UIPAK_OK && qcrc32_finish(crc) != g_entry_crc) st = UIPAK_DAMAGED;
	}
	settle(st, buf);
	if (st != UIPAK_OK) mh_pak_close();
	return st;
}

int mh_pak_read(const struct mh_cave *c, u32 off, void *buf, u32 len)
{
	u32 i, got;

	if (g_file == PLAT_FILE_INVALID || c < g_caves || c >= g_caves + g_desc.ncaves) return ST_IO_ERROR;
	i = (u32)(c - g_caves);
	if (off > c->len || len > c->len - off) return ST_IO_ERROR;
	if (len == 0) return ST_OK;
	if (plat_file_read_at(g_file, g_cave_at[i] + off, buf, len, &got) != 0 || got != len) return ST_IO_ERROR;
	return ST_OK;
}
