/*
 * Build 55: ui.pak, the console menu's renderers on the console's HDD
 * (src/core/uipak.c), on qwark's fake console. test/run.sh packs the pak into
 * the fake HDD with the same pak.py `make` runs. Build 56: and the savefile
 * helpers, which a console installs out of the pak too.
 *
 *   the reader    the pak's header, and every game's renderer out of it equal
 *                 to the table the host builds link (menuhelper_bins.c): the
 *                 descriptor, the caves, every byte, the hook words;
 *   the install   every game's renderer goes into the game out of the pak, its
 *                 code and data byte for byte;
 *   the failures  a pak missing, of another build, of other renderers, damaged,
 *                 cut short or not a pak at all: no menu and nothing written,
 *                 the log, one notification, SessionInfo flags bit7, and the
 *                 menu once the pak is right again;
 *   the update    ui.pak.new taken at the module's start when it is this
 *                 build's, and left where it is when not;
 *   RPCS3         never looks, never sets the flag;
 *   the helpers   every game's savefile helper installed out of the pak, byte
 *                 for byte the table the module carried up to build 55
 *                 (sfhelper_bins.c); refused with SAVEFILE_ERR_NO_PAK and
 *                 nothing written for a pak missing, another build's, with
 *                 other helpers or damaged, and installed once it is right.
 *   ToD's Lua     (revision 1.29) the Lua helper's code and its three chunks
 *                 out of the pak, the table's byte for byte; the helper in
 *                 with the console menu; and for a pak missing, without the
 *                 Lua entries, with another build's, or with an entry that
 *                 fails its CRC: no menu, nothing written, Inventory the
 *                 game's, LUA_EXEC UNSUPPORTED, the log; and a stale pak
 *                 mid-session sends no glue.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/core/uipak.h"
#include "../src/core/savefile.h"
#include "../src/games/menuhelper_bins.h"
#include "../src/games/sfhelper_bins.h"
#include "../src/core/luacore.h"
#include "../src/core/luamb.h"
#include "../src/games/tod.h"
#include "../src/games/luahelper_bins.h"
#include "../src/games/luahelper/lh_menu.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define PAK_BACKUP "/dev_hdd0/qwark/ui.pak.test-backup"

/* The pak run.sh packed, and room for a copy to spoil. */
static u8  g_good[128 * 1024];
static u32 g_good_len;
static u8  g_bad[128 * 1024];

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot(const char *title)
{
	int i;

	host_boot(title);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME) return 1;
	}
	return 0;
}

static int quit(void)
{
	int i;

	host_quit();
	for (i = 0; i < 2000; i++) {
		tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

static u32 peek32(u32 a)
{
	u8 b[4];
	host_peek(a, b, 4);
	return be32_get(b);
}

static void poke32(u32 a, u32 v)
{
	u8 b[4];
	be32_put(b, v);
	host_poke(a, b, 4);
}

static void press(u32 mask)
{
	host_set_pad(0);
	tick(2);
	host_set_pad(mask);
	tick(2);
	host_set_pad(0);
	tick(2);
}

static u8 info_flags(void)
{
	u8 info[SESSION_INFO_SIZE];
	session_info_copy(info, sizeof(info));
	return info[24];
}

static int flag_bad(void)
{
	return (info_flags() & SESSION_FLAG_UI_PAK_BAD) != 0;
}

static int read_file(const char *path, u8 *buf, u32 cap, u32 *len)
{
	plat_file_t f;
	u32 got = 0;

	*len = 0;
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return 0;
	while (*len < cap && plat_file_read(f, buf + *len, cap - *len, &got) == 0 && got != 0) *len += got;
	plat_file_close(f);
	return 1;
}

static int write_file(const char *path, const u8 *buf, u32 len)
{
	plat_file_t f;
	int ok;

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return 0;
	ok = plat_file_write(f, buf, len) == 0;
	plat_file_close(f);
	return ok;
}

/* What a client's write of the pak does: the file, then net.c's touch, then the tick's look. */
static void put_pak(const u8 *buf, u32 len)
{
	write_file(UIPAK_PATH, buf, len);
	uipak_touch(UIPAK_PATH);
	tick(4);
}

static void put_good(void)
{
	put_pak(g_good, g_good_len);
}

/* The good pak with `n` bytes from `at` overwritten, its header's CRC made to fit again when `refit`. */
static u32 spoiled(u32 at, const u8 *with, u32 n, int refit)
{
	memcpy(g_bad, g_good, g_good_len);
	memcpy(g_bad + at, with, n);
	if (refit) be32_put(g_bad + 12, qcrc32(g_bad + UIPAK_HEADER_SIZE, g_good_len - UIPAK_HEADER_SIZE));
	return g_good_len;
}

/* The game in ordinary play, as the renderer's desc reads it. */
static void playing(const struct mh_desc *d)
{
	poke32(d->mode_addr, 0);
	poke32(d->pending_addr, d->pending_none);
	poke32(d->screen_addr, 0);
}

static int hooks_are(const struct mh_desc *d, int ours)
{
	u8 i;
	for (i = 0; i < d->nhooks; i++) {
		if (peek32(d->hooks[i].addr) != (ours ? d->hooks[i].value : d->hooks[i].original)) return 0;
	}
	return 1;
}

/* Whether any byte of the renderer's caves is in the game (it was all 0 before). */
static int nothing_written(const struct mh_desc *d)
{
	u8 i;
	for (i = 0; i < d->ncaves; i++) {
		if (peek32(d->caves[i].addr) != 0) return 0;
	}
	return hooks_are(d, 0);
}

/* Ten seconds of play past a session's install; the state it leaves. */
static int after_install(const struct mh_desc *d)
{
	int i;

	for (i = 0; i < 1200 && menu_state() != MENU_READY; i++) {
		playing(d);
		tick(1);
	}
	return menu_state();
}

/* ------------------------------------------------------------- the reader */

static void test_reader(void)
{
	static u8 piece[4096];
	const struct mh_desc *d, *t;
	u8 game, i;
	int st, same;

	group("ui.pak: the reader");

	check(read_file(UIPAK_PATH, g_good, sizeof(g_good), &g_good_len) && g_good_len > 60000u &&
	      g_good_len < sizeof(g_good), "run.sh packed ui.pak into the fake HDD");
	check(be32_get(g_good) == UIPAK_MAGIC && be16_get(g_good + 4) == 2 && UIPAK_FORMAT == 2 &&
	      be16_get(g_good + 6) == UIPAK_HEADER_SIZE && be32_get(g_good + 8) == QWARK_BUILD &&
	      be32_get(g_good + 16) == g_good_len - UIPAK_HEADER_SIZE && g_good[20] == 12,
	      "its header: QPAK, format 2, this build, the length of the rest, four renderers and four helpers, "
	      "and (revision 1.29, a build with ToD) the Lua helper's four entries");
	check(be32_get(g_good + 12) == qcrc32(g_good + UIPAK_HEADER_SIZE, g_good_len - UIPAK_HEADER_SIZE),
	      "and the CRC of the rest");
	check(be32_get(g_good + 24) == MH_PAK_ID && be32_get(g_good + 28) == SF_PAK_ID,
	      "and the identities this module was built with, the renderers' (MH_PAK_ID) and the helpers' (SF_PAK_ID)");
	check(uipak_status() == UIPAK_OK && !uipak_bad() && !flag_bad(),
	      "the module's start looked at it and found it good: flags bit7 clear");

	for (game = GAME_RAC1; game <= GAME_RAC4; game++) {
		char what[160];

		t = mh_desc_for_game(game);
		d = mh_pak_desc(game, &st);
		same = d != NULL && t != NULL && st == UIPAK_OK && d->game_id == t->game_id &&
		       d->ncaves == t->ncaves && d->nhooks == t->nhooks && d->nhelp == t->nhelp &&
		       d->mailbox == t->mailbox && d->mailbox_size == t->mailbox_size && d->state == t->state &&
		       d->mode_addr == t->mode_addr && d->mode_paused == t->mode_paused &&
		       d->pending_addr == t->pending_addr && d->pending_none == t->pending_none &&
		       d->screen_addr == t->screen_addr && d->screen_id == t->screen_id;
		for (i = 0; same && i < d->nhooks; i++) {
			same = d->hooks[i].addr == t->hooks[i].addr && d->hooks[i].value == t->hooks[i].value &&
			       d->hooks[i].original == t->hooks[i].original;
		}
		for (i = 0; same && i < d->ncaves; i++) {
			same = d->caves[i].addr == t->caves[i].addr && d->caves[i].len == t->caves[i].len &&
			       d->caves[i].code == t->caves[i].code && d->caves[i].zeros == t->caves[i].zeros &&
			       d->caves[i].bytes == NULL;
		}
		snprintf(what, sizeof(what), "game %u: the pak's descriptor, caves and hook words are the table's",
		         (unsigned)game);
		check(same, what);
		if (!same) continue;

		check(mh_pak_read(&d->caves[0], 0, piece, 4) == ST_IO_ERROR, "no byte is read before mh_pak_open");
		check(mh_pak_open() == UIPAK_OK, "mh_pak_open finds the game's part whole");
		for (i = 0; same && i < d->ncaves; i++) {
			u32 off, n;
			for (off = 0; same && off < d->caves[i].len; off += n) {
				n = d->caves[i].len - off;
				if (n > sizeof(piece)) n = sizeof(piece);
				same = mh_pak_read(&d->caves[i], off, piece, n) == ST_OK &&
				       memcmp(piece, t->caves[i].bytes + off, n) == 0;
			}
		}
		snprintf(what, sizeof(what), "game %u: every byte of every cave, read in pieces, is the table's",
		         (unsigned)game);
		check(same, what);
		check(mh_pak_read(&d->caves[0], d->caves[0].len - 2, piece, 4) == ST_IO_ERROR &&
		      mh_pak_read(&t->caves[0], 0, piece, 4) == ST_IO_ERROR,
		      "nothing past a cave's end, and nothing of a cave that is not the pak's");
		mh_pak_close();
	}

	d = mh_pak_desc(GAME_NONE, &st);
	check(d == NULL && st == UIPAK_OK, "no renderer for no game, and the pak is still good");
	check(uipak_names(UIPAK_PATH) && uipak_names("/dev_hdd0/qwark") && uipak_names("/dev_hdd0/qwark/") &&
	      uipak_names("/dev_hdd0") && !uipak_names("/dev_hdd0/qwark/ui.pak.new") &&
	      !uipak_names("/dev_hdd0/qwark/ui") && !uipak_names("/dev_hdd0/qwarkx") &&
	      !uipak_names("/dev_hdd0/qwark/savefiles") && !uipak_names(NULL),
	      "a file op names the pak by its path or a folder above it, and nothing else");
}

/* ------------------------------------------------------------ the install */

static void install_one(u8 game, const char *title)
{
	static u8 buf[20 * 1024];
	const struct mh_desc *t = mh_desc_for_game(game);
	char what[160];
	int ok = 1;
	u8 i;

	if (!boot(title)) {
		check(0, "the game boots");
		return;
	}
	ok = after_install(t) == MENU_READY && hooks_are(t, 1);
	for (i = 0; ok && i < t->ncaves; i++) {
		/* The mailbox carries the first page by now; its header and the rest are the image's. */
		u32 n = t->caves[i].addr == t->mailbox ? 8u : t->caves[i].len;
		if (n > sizeof(buf)) n = sizeof(buf);
		host_peek(t->caves[i].addr, buf, n);
		ok = memcmp(buf, t->caves[i].bytes, n) == 0;
	}
	snprintf(what, sizeof(what), "%s: the renderer out of the pak is in the game, its code and data the table's "
	         "byte for byte, every hook word in", title);
	check(ok, what);
	check(quit(), "quit");
}

static void test_install(void)
{
	group("ui.pak: every game's renderer goes in out of the pak");
	install_one(GAME_RAC4, "NPEA00423");
	install_one(GAME_RAC3, "NPEA00387");
	install_one(GAME_RAC2, "NPEA00386");
	install_one(GAME_RAC1, "NPEA00385");
}

/* ------------------------------------------------------------ the failures */

/* A Deadlocked session over a pak that cannot serve: nothing goes in, and it says so. */
static void refused(const char *what, int status)
{
	const struct mh_desc *t = mh_desc_for_game(GAME_RAC4);
	char line[200];

	check(boot("NPEA00423"), "Deadlocked boots");
	snprintf(line, sizeof(line), "%s: no menu, nothing of it written, flags bit7 set", what);
	check(after_install(t) == MENU_NOPAK && nothing_written(t) && uipak_status() == status && flag_bad(), line);
	check(quit(), "quit");
}

static void test_missing(void)
{
	const struct mh_desc *t = mh_desc_for_game(GAME_RAC4);
	u32 n0;

	group("ui.pak: missing");

	check(plat_file_rename(UIPAK_PATH, PAK_BACKUP) == 0, "ui.pak taken away");
	uipak_touch("/dev_hdd0/qwark");
	tick(4);
	check(uipak_status() == UIPAK_MISSING && flag_bad(), "the next tick finds it gone: flags bit7 set, at the XMB too");

	n0 = host_notify_count();
	check(boot("NPEA00423"), "Deadlocked boots");
	check(after_install(t) == MENU_NOPAK && nothing_written(t), "its install is due: no menu, nothing written");
	check(host_notify_count() == n0 + 1 && strstr(host_notify_last(), "ui.pak missing or from another build") != NULL,
	      "and the console says so once");
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_NOPAK && host_notify_count() == n0 + 1 && nothing_written(t),
	      "the combo looks again, finds none, and says nothing more this session");
	check(quit(), "quit");

	check(boot("NPEA00423"), "Deadlocked boots again");
	check(after_install(t) == MENU_NOPAK && host_notify_count() == n0 + 1,
	      "the next session has no menu either, and the console is not told again");
	press(COMBO_MENU_DEFAULT);
	check(host_notify_count() == n0 + 2 && strstr(host_notify_last(), "ui.pak") != NULL,
	      "but a press of the combo says why, once a session");

	/* A client writes it back: the touch, the look, and the menu goes in in the same session. */
	check(plat_file_rename(PAK_BACKUP, UIPAK_PATH) == 0, "ui.pak put back");
	uipak_touch(UIPAK_PATH);
	tick(4);
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the next tick finds it good: flags bit7 clear");
	check(after_install(t) == MENU_READY && hooks_are(t, 1), "and the menu goes in without a restart of the game");
	check(quit(), "quit");
}

static void test_other_build(void)
{
	u8 b[4];

	group("ui.pak: another build's, or other renderers");

	be32_put(b, QWARK_BUILD - 1);
	put_pak(g_bad, spoiled(8, b, 4, 0));
	check(uipak_status() == UIPAK_OTHER && flag_bad(), "a pak that names build 54: another build's, flags bit7 set");
	refused("another build's", UIPAK_OTHER);

	be32_put(b, MH_PAK_ID ^ 0x00010000u);
	put_pak(g_bad, spoiled(24, b, 4, 0));
	check(uipak_status() == UIPAK_OTHER && flag_bad(),
	      "this build's number over other renderers (the CRC fits, but the identity is not MH_PAK_ID): not this module's");
	refused("other renderers", UIPAK_OTHER);

	/* Build 56: the helpers are part of what makes a pak this module's. */
	be32_put(b, SF_PAK_ID ^ 0x00000100u);
	put_pak(g_bad, spoiled(28, b, 4, 0));
	check(uipak_status() == UIPAK_OTHER && flag_bad(),
	      "this build's renderers over other savefile helpers (not SF_PAK_ID): not this module's either");
	refused("other helpers", UIPAK_OTHER);

	/* Build 55's format, or a build 56 test's from before the helpers moved in, naming build 56. */
	b[0] = 0;
	b[1] = 1;
	put_pak(g_bad, spoiled(4, b, 2, 0));
	check(uipak_status() == UIPAK_OTHER && flag_bad(), "a format 1 pak naming this build: another build's");

	put_good();
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the right one back: good");
}

static void test_damaged(void)
{
	const struct mh_desc *t = mh_desc_for_game(GAME_RAC4);
	const struct mh_desc *d;
	u32 at, n0;
	int st;
	u8 b[1];

	group("ui.pak: damaged, cut short, not a pak");

	/* A byte of Deadlocked's code: the whole file's CRC fails, and so does Deadlocked's part. */
	d = mh_pak_desc(GAME_RAC4, &st);
	at = be32_get(g_good + UIPAK_HEADER_SIZE + 16) + UIPAK_DESC_SIZE + d->ncaves * UIPAK_CAVE_SIZE +
	     d->nhooks * UIPAK_HOOK_SIZE + 600;
	b[0] = (u8)(g_good[at] ^ 0x01);
	put_pak(g_bad, spoiled(at, b, 1, 0));
	check(uipak_status() == UIPAK_DAMAGED && flag_bad(), "one byte changed: damaged, flags bit7 set");
	refused("a byte of Deadlocked's renderer changed", UIPAK_DAMAGED);

	/* UYA's part is whole, so its menu still goes in; the file is still damaged. */
	check(boot("NPEA00387"), "UYA boots");
	check(after_install(mh_desc_for_game(GAME_RAC3)) == MENU_READY && uipak_status() == UIPAK_DAMAGED && flag_bad(),
	      "UYA's part is whole and its menu goes in, the flag still saying the file is damaged");
	check(quit(), "quit");

	/* Build 56: RaC1's renderer is the last renderer, with the four helpers after it. */
	put_pak(g_good, be32_get(g_good + UIPAK_HEADER_SIZE + 4 * UIPAK_TOC_SIZE + 16) - 1000);
	check(uipak_status() == UIPAK_DAMAGED && flag_bad(), "cut 1000 bytes into RaC1's renderer: damaged");
	check(boot("NPEA00385"), "RaC1 boots");
	check(after_install(mh_desc_for_game(GAME_RAC1)) == MENU_NOPAK &&
	      nothing_written(mh_desc_for_game(GAME_RAC1)), "RaC1's part, the last renderer, is cut: no menu, nothing written");
	check(quit(), "quit");

	put_pak(g_good, 10);
	check(uipak_status() == UIPAK_DAMAGED && flag_bad(), "ten bytes, shorter than the header: damaged");
	refused("shorter than the header", UIPAK_DAMAGED);

	put_pak(g_good, 0);
	check(uipak_status() == UIPAK_DAMAGED && flag_bad(), "empty: damaged");

	{
		const char *text = "not a pak, just some text that is long enough to be read as a header";
		put_pak((const u8 *)text, qstrlen(text));
	}
	check(uipak_status() == UIPAK_DAMAGED && flag_bad(), "text: not a pak");

	memcpy(g_bad, g_good, g_good_len);
	memcpy(g_bad + g_good_len, "x", 1);
	put_pak(g_bad, g_good_len + 1);
	check(uipak_status() == UIPAK_DAMAGED && flag_bad(), "a byte past the length the header gives: damaged");

	n0 = host_notify_count();
	put_good();
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the right one back: good");
	check(boot("NPEA00423"), "Deadlocked boots");
	check(after_install(t) == MENU_READY && hooks_are(t, 1) && host_notify_count() == n0,
	      "and the menu goes in, nothing said");
	check(quit(), "quit");
}

/* ------------------------------------------------------------- the update */

static void test_update(void)
{
	static u8 back[128 * 1024];
	u32 len = 0;
	u8 b[4];
	int exists;

	group("ui.pak: an update's ui.pak.new");

	/* The standalone update leaves the next module's pak as ui.pak.new; the old one stays. */
	be32_put(b, QWARK_BUILD - 1);
	spoiled(8, b, 4, 0);
	write_file(UIPAK_PATH, g_bad, g_good_len);
	write_file(UIPAK_NEW_PATH, g_good, g_good_len);
	uipak_test_start();
	tick(4);
	exists = plat_path_exists(UIPAK_NEW_PATH, NULL, NULL);
	check(read_file(UIPAK_PATH, back, sizeof(back), &len) && len == g_good_len &&
	      memcmp(back, g_good, len) == 0 && !exists && uipak_status() == UIPAK_OK && !flag_bad(),
	      "the module's start takes a ui.pak.new of its own build in place of ui.pak, which it then finds good");

	write_file(UIPAK_NEW_PATH, g_bad, g_good_len);
	uipak_test_start();
	tick(4);
	exists = plat_path_exists(UIPAK_NEW_PATH, NULL, NULL);
	check(read_file(UIPAK_PATH, back, sizeof(back), &len) && len == g_good_len &&
	      memcmp(back, g_good, len) == 0 && exists && uipak_status() == UIPAK_OK,
	      "a ui.pak.new of another build is left where it is, ui.pak untouched");
	plat_file_unlink(UIPAK_NEW_PATH);

	plat_file_unlink(UIPAK_PATH);
	write_file(UIPAK_NEW_PATH, g_good, g_good_len);
	uipak_test_start();
	tick(4);
	check(plat_path_exists(UIPAK_PATH, NULL, NULL) && !plat_path_exists(UIPAK_NEW_PATH, NULL, NULL) &&
	      uipak_status() == UIPAK_OK, "and taken with no ui.pak there at all");

	/* A later write of ui.pak.new, with the module running, is the next module's: not looked at. */
	write_file(UIPAK_NEW_PATH, g_good, g_good_len);
	uipak_touch(UIPAK_NEW_PATH);
	tick(4);
	check(plat_path_exists(UIPAK_NEW_PATH, NULL, NULL), "a ui.pak.new written while the module runs waits for the next start");
	plat_file_unlink(UIPAK_NEW_PATH);
}

/* ---------------------------------------------------------------- RPCS3 */

static void test_rpcs3(void)
{
	group("ui.pak: RPCS3 never needs it");

	host_set_can_patch_code(0);
	plat_file_rename(UIPAK_PATH, PAK_BACKUP);
	uipak_test_start();
	tick(4);
	check(uipak_status() == UIPAK_UNCHECKED && !flag_bad(), "no pak and RPCS3: never looked at, flags bit7 clear");
	host_set_can_patch_code(1);
	plat_file_rename(PAK_BACKUP, UIPAK_PATH);
	uipak_test_start();
	tick(4);
	check(uipak_status() == UIPAK_OK && !flag_bad(), "a console again: good");
}

/* ------------------------------------------------------- the helpers, build 56 */

static const char *const g_sf_titles[5] = { "", "NPEA00385", "NPEA00386", "NPEA00387", "NPEA00423" };

/* Whether every cave byte and hook word of the game's helper is in the game as the table has it. */
static int sf_in_game(const struct sf_desc *t)
{
	static u8 buf[4096];
	u8 i;

	for (i = 0; i < t->ncaves; i++) {
		if (t->caves[i].len > sizeof(buf)) return 0;
		host_peek(t->caves[i].addr, buf, t->caves[i].len);
		if (memcmp(buf, t->caves[i].bytes, t->caves[i].len) != 0) return 0;
	}
	for (i = 0; i < t->nhooks; i++) {
		if (peek32(t->hooks[i].addr) != t->hooks[i].value) return 0;
	}
	return 1;
}

/* What the game holds where the helper goes: every cave and hook word, and the request bytes. */
static u32 sf_snapshot(const struct sf_desc *t)
{
	static u8 buf[4096];
	u32 crc = qcrc32_start();
	u8 i, b;

	for (i = 0; i < t->ncaves; i++) {
		host_peek(t->caves[i].addr, buf, t->caves[i].len);
		crc = qcrc32_update(crc, buf, t->caves[i].len);
	}
	for (i = 0; i < t->nhooks; i++) {
		be32_put(buf, peek32(t->hooks[i].addr));
		crc = qcrc32_update(crc, buf, 4);
	}
	host_peek(t->api_mod, &b, 1);
	crc = qcrc32_update(crc, &b, 1);
	host_peek(t->api_load, &b, 1);
	crc = qcrc32_update(crc, &b, 1);
	host_peek(t->api_setaside, &b, 1);
	crc = qcrc32_update(crc, &b, 1);
	return qcrc32_finish(crc);
}

static u8 sf_error(void)
{
	u32 done, total;
	u8 error;

	savefile_transfer(&done, &total, &error);
	return error;
}

/* Request bytes the game left non-zero, which the install must zero. */
static void sf_dirty(const struct sf_desc *t)
{
	u8 b = 0x5A;

	host_poke(t->api_mod, &b, 1);
	host_poke(t->api_load, &b, 1);
	host_poke(t->api_setaside, &b, 1);
}

static int sf_clean(const struct sf_desc *t)
{
	u8 a = 1, l = 1, s = 1;

	host_peek(t->api_mod, &a, 1);
	host_peek(t->api_load, &l, 1);
	host_peek(t->api_setaside, &s, 1);
	return a == 0 && l == 0 && s == 0;
}

/*
 * Every game's helper installed out of the pak is byte for byte the table the
 * module linked up to build 55, which the host builds still link
 * (sfhelper_bins.c), and its entry in the pak is that table.
 */
static void test_sf_install(void)
{
	const u8 *e;
	u32 len;
	u8 game, i;

	group("ui.pak: every game's savefile helper goes in out of the pak");

	for (game = GAME_RAC1; game <= GAME_RAC4; game++) {
		const struct sf_desc *t = sf_desc_for_game(game);
		char what[200];
		int same;

		check(t != NULL && t->caves != NULL && t->hooks != NULL, "the host builds link the helper's bytes");
		if (t == NULL || t->caves == NULL) continue;

		same = uipak_sf_entry(game, &e, &len) == UIPAK_OK && len <= UIPAK_MAX_SF_ENTRY &&
		       e[36] == t->ncaves && e[37] == t->nhooks && e[38] == 0 &&
		       be32_get(e) == t->api_mod && be32_get(e + 4) == t->api_load &&
		       be32_get(e + 8) == t->api_setaside && be32_get(e + 12) == t->aside_addr &&
		       be32_get(e + 16) == t->aside_size && be32_get(e + 20) == t->api_trophy;
		for (i = 0; same && i < t->ncaves; i++) {
			const u8 *c = e + UIPAK_DESC_SIZE + i * UIPAK_CAVE_SIZE;
			same = be32_get(c) == t->caves[i].addr && be32_get(c + 8) == t->caves[i].len &&
			       be32_get(c + 12) == 0 && c[16] == 1 &&
			       memcmp(e + be32_get(c + 4), t->caves[i].bytes, t->caves[i].len) == 0;
		}
		for (i = 0; same && i < t->nhooks; i++) {
			const u8 *w = e + UIPAK_DESC_SIZE + t->ncaves * UIPAK_CAVE_SIZE + i * UIPAK_HOOK_SIZE;
			same = be32_get(w) == t->hooks[i].addr && be32_get(w + 4) == t->hooks[i].value;
		}
		snprintf(what, sizeof(what), "game %u: the pak's helper entry is the table's: numbers, %u caves byte for "
		         "byte, %u hook word", (unsigned)game, (unsigned)t->ncaves, (unsigned)t->nhooks);
		check(same, what);

		if (!boot(g_sf_titles[game])) {
			check(0, "the game boots");
			continue;
		}
		sf_dirty(t);
		check(!sf_in_game(t), "the helper is not in the game yet");
		check(savefile_install() == ST_OK, "savefile_install answers OK");
		snprintf(what, sizeof(what), "%s: the helper out of the pak is in the game, every cave byte and the hook "
		         "word the table's, the request bytes zeroed", g_sf_titles[game]);
		check(sf_in_game(t) && sf_clean(t), what);
		check(quit(), "quit");
	}

	/* A pak of this build with no helper for a game this build has one for is a pak gone wrong. */
	check(uipak_sf_entry(GAME_NONE, &e, &len) == UIPAK_DAMAGED && e == NULL && len == 0 &&
	      uipak_status() == UIPAK_DAMAGED, "no entry for the game: damaged");
	put_good();
}

/* A Deadlocked session over a pak that cannot give the helper: nothing written, the reason, the flag. */
static void sf_refused(const char *what, int status)
{
	const struct sf_desc *t = sf_desc_for_game(GAME_RAC4);
	char line[200];
	u32 before;
	u8 sup = 0, inst = 1, run = 1, pend = 1;
	u32 size = 0;
	int ok;

	check(boot("NPEA00423"), "Deadlocked boots");
	before = sf_snapshot(t);
	snprintf(line, sizeof(line), "%s: savefile_install is BUSY with SAVEFILE_ERR_NO_PAK, nothing written, flags bit7",
	         what);
	ok = savefile_install() == ST_BUSY && sf_error() == SAVEFILE_ERR_NO_PAK && sf_snapshot(t) == before &&
	     uipak_status() == status;
	tick(2);
	check(ok && flag_bad(), line);
	check(savefile_info(&sup, &inst, &run, &pend, &size) == ST_OK && sup == 1 && inst == 0 && pend == 0,
	      "SAVEFILE_INFO: supported, not installed, nothing pending");
	check(quit(), "quit");
}

static void test_sf_failures(void)
{
	const struct sf_desc *t = sf_desc_for_game(GAME_RAC4);
	u8 b[4];
	u32 at;

	group("ui.pak: the savefile helpers when the pak cannot give them");

	check(plat_file_rename(UIPAK_PATH, PAK_BACKUP) == 0, "ui.pak taken away");
	uipak_touch(UIPAK_PATH);
	tick(4);
	sf_refused("missing", UIPAK_MISSING);

	/* A client writes it back: the next try installs, in the same session. */
	check(boot("NPEA00423"), "Deadlocked boots");
	check(savefile_install() == ST_BUSY && sf_error() == SAVEFILE_ERR_NO_PAK, "still missing: refused");
	check(plat_file_rename(PAK_BACKUP, UIPAK_PATH) == 0, "ui.pak put back");
	uipak_touch(UIPAK_PATH);
	tick(4);
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the next tick finds it good: flags bit7 clear");
	check(savefile_install() == ST_OK && sf_in_game(t), "and the helper goes in without a restart of the game");

	/* Installed is installed: the pak going away later takes nothing from this process. */
	check(plat_file_rename(UIPAK_PATH, PAK_BACKUP) == 0, "ui.pak taken away again");
	uipak_touch(UIPAK_PATH);
	tick(4);
	check(savefile_install() == ST_OK && sf_in_game(t),
	      "a helper already in this process needs no pak: the next install answers OK");
	check(plat_file_rename(PAK_BACKUP, UIPAK_PATH) == 0, "ui.pak put back");
	uipak_touch(UIPAK_PATH);
	tick(4);
	check(quit(), "quit");

	be32_put(b, QWARK_BUILD - 1);
	put_pak(g_bad, spoiled(8, b, 4, 0));
	sf_refused("another build's", UIPAK_OTHER);

	be32_put(b, SF_PAK_ID ^ 0x00000100u);
	put_pak(g_bad, spoiled(28, b, 4, 0));
	sf_refused("other helpers", UIPAK_OTHER);

	/* A byte of Deadlocked's helper code, the pak's last entry: its CRC fails, and the file's. */
	at = be32_get(g_good + UIPAK_HEADER_SIZE + 7 * UIPAK_TOC_SIZE + 16);
	at += be32_get(g_good + at + UIPAK_DESC_SIZE + UIPAK_CAVE_SIZE + 4) + 100;
	b[0] = (u8)(g_good[at] ^ 0x01);
	put_pak(g_bad, spoiled(at, b, 1, 0));
	sf_refused("a byte of Deadlocked's helper changed", UIPAK_DAMAGED);

	/*
	 * The same byte with the file's CRC made to fit but not the entry's, which
	 * pak.py never writes: the install still checks the entry and writes
	 * nothing. (The flag then follows the next whole-file look, which finds the
	 * file good again once the menu's install has read its own entry.)
	 */
	put_pak(g_bad, spoiled(at, b, 1, 1));
	check(uipak_status() == UIPAK_OK, "the file's CRC refitted: the look finds the file good");
	check(boot("NPEA00423"), "Deadlocked boots");
	{
		u32 before = sf_snapshot(t);
		check(savefile_install() == ST_BUSY && sf_error() == SAVEFILE_ERR_NO_PAK && sf_snapshot(t) == before &&
		      uipak_status() == UIPAK_DAMAGED,
		      "its entry's CRC still fails: BUSY with SAVEFILE_ERR_NO_PAK, nothing written, the pak damaged");
	}
	check(quit(), "quit");

	/* Revision 1.29: Deadlocked's helper is the eighth entry, with the Lua helper's four after it. */
	put_pak(g_good, be32_get(g_good + UIPAK_HEADER_SIZE + 7 * UIPAK_TOC_SIZE + 16) +
	                be32_get(g_good + UIPAK_HEADER_SIZE + 7 * UIPAK_TOC_SIZE + 20) - 200);
	sf_refused("cut into Deadlocked's helper", UIPAK_DAMAGED);

	put_good();
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the right one back: good");
	check(boot("NPEA00423"), "Deadlocked boots");
	check(savefile_install() == ST_OK && sf_in_game(t), "and the helper goes in");
	check(quit(), "quit");
}

/* ------------------------------- ToD's Lua helper, revision 1.29 */

/*
 * The pak's entries by their place in run.sh's pak (pak.py --tod): the four
 * renderers, the four savefile helpers, then the Lua helper's four, its code
 * (kind 3), the pause menu's glue (4), the look (5) and the map's glue (6).
 */
#define LH_FIRST 8u

static u32 toc_word(const u8 *pak, u32 i, u32 at)
{
	return be32_get(pak + UIPAK_HEADER_SIZE + i * UIPAK_TOC_SIZE + at);
}

static int lh_boot(void)
{
	int i;

	host_boot_as(TOD_TITLE, "tod");
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME && session_game() != NULL) {
			poke32(TOD_STATE_PENDING_OP, 0);
			poke32(TOD_LUA_STATE, 0x30030000u);
			return 1;
		}
	}
	return 0;
}

/* Twelve seconds of play, or until the menu has gone in or found no pak. */
static int lh_settle(void)
{
	int i;

	for (i = 0; i < 1500 && menu_state() != MENU_READY && menu_state() != MENU_NOPAK; i++) tick(1);
	return menu_state();
}

/* Whether the helper's cave, its five hook words and the mailbox's identity are in the game, the table's. */
static int lh_in_game(const struct lh_desc *t)
{
	u8 i;

	if (memcmp(host_mem_ptr(t->caves[0].addr), t->caves[0].bytes, t->caves[0].len) != 0) return 0;
	for (i = 0; i < t->nhooks; i++) {
		if (peek32(t->hooks[i].addr) != t->hooks[i].value) return 0;
	}
	return peek32(t->mailbox) == LUAMB_MAGIC;
}

/* Nothing of it: no cave byte, the game's own five words, no identity, no menu mailbox, Inventory the game's. */
static int lh_nothing(const struct lh_desc *t)
{
	u8 i;

	if (peek32(t->caves[0].addr) != 0 || peek32(t->mailbox) != 0 || peek32(LM_BOX) != 0 ||
	    host_mem_ptr(LM_STATE + LM_HELP)[0] != 0)
		return 0;
	for (i = 0; i < t->nhooks; i++) {
		if (peek32(t->hooks[i].addr) != t->hooks[i].orig) return 0;
	}
	return 1;
}

/* LUA_EXEC's first half for a little chunk, its frame status. */
static int lh_exec(void)
{
	u8 req[16];
	u32 seq = 0;

	be16_put(req, 8);
	be16_put(req + 2, 0);
	memcpy(req + 4, "_VERSION", 8);
	return lua_core_exec_start(req, 12, &seq);
}

static void test_lh_reader(void)
{
	static u8 piece[3000];
	const struct lh_desc *t = lh_desc_for_title(TOD_TITLE);
	const u8 *e, *b;
	u32 len, n, off, crc, i;
	int same;

	group("ui.pak: Tools of Destruction's Lua helper (revision 1.29)");

	check(t != NULL && t->game_id == GAME_TOD && t->ncaves == 1 && t->caves[0].bytes != NULL && t->glue != NULL &&
	      t->diag != NULL && t->gmap != NULL, "the host builds link the helper's bytes and its three chunks");
	if (t == NULL || t->caves[0].bytes == NULL) return;

	same = 1;
	crc = qcrc32_start();
	for (i = 0; i < 4; i++) {
		const u8 *toc = g_good + UIPAK_HEADER_SIZE + (LH_FIRST + i) * UIPAK_TOC_SIZE;

		same = same && toc[0] == GAME_TOD && toc[1] == UIPAK_KIND_LUAHELPER + i && memcmp(toc + 4, TOD_TITLE, 10) == 0;
		crc = qcrc32_update(crc, toc + 24, 4);
	}
	check(same && qcrc32_finish(crc) == LH_PAK_ID,
	      "the pak's last four entries are ToD's helper, glue, look and map glue, and the CRC of their CRCs is LH_PAK_ID");

	same = uipak_lh_open(GAME_TOD, &e, &len) == UIPAK_OK && len == toc_word(g_good, LH_FIRST, 20) &&
	       e[36] == 1 && e[37] == 5 && e[38] == 0 && be32_get(e) == t->mailbox &&
	       be32_get(e + 4) == t->mailbox_size && be32_get(e + 8) == t->vm && be32_get(e + 12) == t->vm_state &&
	       be32_get(e + 16) == t->menu_ptr && be32_get(e + 20) == t->menu_off && be32_get(e + 24) == t->map_ptr &&
	       be32_get(e + UIPAK_DESC_SIZE) == t->caves[0].addr && be32_get(e + UIPAK_DESC_SIZE + 8) == t->caves[0].len;
	for (i = 0; same && i < t->nhooks; i++) {
		const u8 *w = e + UIPAK_DESC_SIZE + UIPAK_CAVE_SIZE + i * UIPAK_HOOK_SIZE;
		same = be32_get(w) == t->hooks[i].addr && be32_get(w + 4) == t->hooks[i].value &&
		       be32_get(w + 8) == t->hooks[i].orig;
	}
	check(same, "uipak_lh_open: the helper's entry whole by its CRC, its numbers, its cave and its five hook words the table's");
	off = same ? be32_get(e + UIPAK_DESC_SIZE + 4) : 0;
	for (i = 0; same && i < t->caves[0].len; i += n) {
		n = t->caves[0].len - i < sizeof(piece) ? t->caves[0].len - i : (u32)sizeof(piece);
		same = uipak_lh_read(off + i, piece, n) == ST_OK && memcmp(piece, t->caves[0].bytes + i, n) == 0;
	}
	check(same, "uipak_lh_read: the cave, read in pieces, is the table's byte for byte");
	check(uipak_lh_read(len - 2, piece, 4) == ST_IO_ERROR, "nothing past the entry");
	uipak_lh_close();
	check(uipak_lh_read(0, piece, 4) == ST_IO_ERROR, "and nothing once it is closed");

	check(uipak_lh_chunk(GAME_TOD, UIPAK_KIND_LUA_GLUE, &b, &n) == UIPAK_OK && n == t->glue_len &&
	      memcmp(b, t->glue, n) == 0, "uipak_lh_chunk: the pause menu's glue, the bytes the emulator builds send");
	check(uipak_lh_chunk(GAME_TOD, UIPAK_KIND_LUA_DIAG, &b, &n) == UIPAK_OK && n == t->diag_len &&
	      memcmp(b, t->diag, n) == 0, "the look at the pause");
	check(uipak_lh_chunk(GAME_TOD, UIPAK_KIND_LUA_GMAP, &b, &n) == UIPAK_OK && n == t->gmap_len &&
	      memcmp(b, t->gmap, n) == 0, "and the galactic map's glue");
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the pak still good");
	check(uipak_lh_chunk(GAME_RAC4, UIPAK_KIND_LUA_GLUE, &b, &n) == UIPAK_DAMAGED && b == NULL && n == 0,
	      "no chunk for a game with no Lua helper: a pak gone wrong");
	put_good();
}

static void test_lh_install(void)
{
	const struct lh_desc *t = lh_desc_for_title(TOD_TITLE);

	group("ui.pak: ToD's Lua helper goes in out of the pak, with the console menu");
	if (t == NULL || t->caves[0].bytes == NULL) return;
	check(lh_boot(), "ToD boots");
	check(lh_nothing(t), "nothing of the helper in the game yet");
	check(lh_settle() == MENU_READY && lh_in_game(t),
	      "the menu goes in, and the helper with it out of the pak: the cave the table's byte for byte, its five "
	      "hook words, the mailbox's identity");
	check(host_mem_ptr(LM_STATE + LM_HELP)[0] == 1, "TRAINER in Inventory's row");
	check(quit(), "quit");
}

/* run.sh's pak without the Lua helper's entries, laid out as one a build without ToD packs: into g_bad. */
static u32 pak_without_lua(void)
{
	u32 len = toc_word(g_good, LH_FIRST - 1, 16) + toc_word(g_good, LH_FIRST - 1, 20);

	memcpy(g_bad, g_good, len);
	g_bad[20] = (u8)LH_FIRST;
	be32_put(g_bad + 16, len - UIPAK_HEADER_SIZE);
	be32_put(g_bad + 12, qcrc32(g_bad + UIPAK_HEADER_SIZE, len - UIPAK_HEADER_SIZE));
	return len;
}

/* The good pak with a byte of entry `i` changed at `at` into it, the entry's CRC refitted when `entry`, the file's always. */
static u32 lh_spoiled(u32 i, u32 at, int entry)
{
	u32 e = toc_word(g_good, i, 16), n = toc_word(g_good, i, 20);

	memcpy(g_bad, g_good, g_good_len);
	g_bad[e + at] ^= 0x20;
	if (entry) be32_put(g_bad + UIPAK_HEADER_SIZE + i * UIPAK_TOC_SIZE + 24, qcrc32(g_bad + e, n));
	be32_put(g_bad + 12, qcrc32(g_bad + UIPAK_HEADER_SIZE, g_good_len - UIPAK_HEADER_SIZE));
	return g_good_len;
}

/* A ToD session over a pak that cannot give the helper: nothing goes in, and it says why. */
static void lh_refused(const char *what, int status, const char *log)
{
	const struct lh_desc *t = lh_desc_for_title(TOD_TITLE);
	char line[240];
	u32 n0;

	n0 = host_notify_count();
	check(lh_boot(), "ToD boots");
	snprintf(line, sizeof(line), "%s: no menu, nothing of the helper written, Inventory the game's, flags bit7 set", what);
	check(lh_settle() == MENU_NOPAK && lh_nothing(t) && uipak_status() == status && flag_bad(), line);
	snprintf(line, sizeof(line), "%s: qwark.log says why (\"%s\")", what, log);
	check(host_log_seen("lua: no usable Lua helper for game 5 in ui.pak; nothing written") &&
	      (log == NULL || host_log_seen(log)), line);
	check(host_notify_count() <= n0 + 1, "the console says so at most once");
	snprintf(line, sizeof(line), "%s: LUA_EXEC answers UNSUPPORTED and writes nothing", what);
	check(lh_exec() == ST_UNSUPPORTED && lh_nothing(t), line);
	check(lua_core_menu_glue() == ST_UNSUPPORTED && lh_nothing(t), "and the glue is not sent either");
	check(quit(), "quit");
}

static void test_lh_failures(void)
{
	const struct lh_desc *t = lh_desc_for_title(TOD_TITLE);
	u32 mb, seq;

	group("ui.pak: ToD's Lua helper when the pak cannot give it");
	if (t == NULL || t->caves[0].bytes == NULL) return;
	mb = t->mailbox;

	host_log_clear();
	check(plat_file_rename(UIPAK_PATH, PAK_BACKUP) == 0, "ui.pak taken away");
	uipak_touch(UIPAK_PATH);
	tick(4);
	lh_refused("missing", UIPAK_MISSING, "ui.pak: none at");

	/* A client writes it back: the menu and the helper go in, in the same session. */
	check(lh_boot(), "ToD boots");
	check(lh_settle() == MENU_NOPAK && lh_nothing(t), "still missing: no menu");
	check(plat_file_rename(PAK_BACKUP, UIPAK_PATH) == 0, "ui.pak put back");
	uipak_touch(UIPAK_PATH);
	tick(4);
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the next tick finds it good: flags bit7 clear");
	check(lh_settle() == MENU_READY && lh_in_game(t), "and the menu and the helper go in without a restart of the game");
	{
		u8 reply[64];
		u32 rlen = 0, rseq = 0;
		u8 req[16];
		int done = 0, k;

		be16_put(req, 8);
		be16_put(req + 2, 0);
		memcpy(req + 4, "_VERSION", 8);
		check(lua_core_exec_start(req, 12, &rseq) == ST_OK, "LUA_EXEC is sent");
		for (k = 0; k < 20 && !done; k++) {
			host_fake_lua_frame();
			tick(1);
			lua_core_exec_result(rseq, reply, sizeof(reply), &rlen, &done);
		}
		check(done && be32_get(reply) == LUAMB_ST_OK, "and answered");
	}

	/*
	 * The pak goes stale while the helper is in: a pause's glue comes out of
	 * the pak, so none is sent, and the Trainer row never comes (the glue
	 * makes it): Inventory stays the game's.
	 */
	put_pak(g_bad, lh_spoiled(LH_FIRST + 1, 300, 1));
	check(uipak_status() == UIPAK_OTHER && flag_bad(), "another build's glue under this build's number: not this module's");
	host_log_clear();
	host_set_fake_pause(1);
	seq = peek32(mb + LUAMB_OFF_REQ);
	host_fake_menu_frame();
	tick(8);
	check(peek32(LM_STATE + LM_GLUE) == LM_GLUE_WANTED && peek32(mb + LUAMB_OFF_REQ) == seq,
	      "a pause asks for the glue, and none is sent: the Trainer row stays out, Inventory the game's");
	check(host_log_seen("lua: no usable Lua chunk 4 for game 5 in ui.pak; not sent") &&
	      host_log_seen("menu: the pause menu's glue could not be sent"), "qwark.log says why");
	host_set_fake_pause(0);
	host_fake_lua_frame();
	put_good();
	host_set_fake_pause(1);
	seq = peek32(mb + LUAMB_OFF_REQ);
	host_fake_menu_frame();
	tick(8);
	check(peek32(mb + LUAMB_OFF_REQ) == seq + 1 && peek32(mb + LUAMB_OFF_CHUNK_LEN) == t->glue_len &&
	      memcmp(host_mem_ptr(mb + LUAMB_OFF_CHUNK), t->glue, t->glue_len) == 0,
	      "the right pak back: the next pause's glue goes, the pak's bytes");
	host_fake_menu_frame();
	host_set_fake_pause(0);
	host_fake_lua_frame();
	tick(4);
	check(quit(), "quit");

	host_log_clear();
	put_pak(g_bad, pak_without_lua());
	check(uipak_status() == UIPAK_OTHER && flag_bad(),
	      "a pak without the Lua helper, as a build without ToD packs it: not this module's, flags bit7 set");
	lh_refused("no Lua helper in the pak", UIPAK_OTHER, "Lua helper 0x00000000, not this module's");

	put_good();
	host_log_clear();
	put_pak(g_bad, lh_spoiled(LH_FIRST, 2000, 1));
	lh_refused("another build's helper code", UIPAK_OTHER, "not this module's");

	put_good();
	host_log_clear();
	put_pak(g_bad, lh_spoiled(LH_FIRST + 3, 100, 1));
	lh_refused("another build's map glue", UIPAK_OTHER, "not this module's");

	/* A byte of the helper's code with the file's CRC refitted but not the entry's, which pak.py never writes. */
	put_good();
	host_log_clear();
	put_pak(g_bad, lh_spoiled(LH_FIRST, 2000, 0));
	check(uipak_status() == UIPAK_OK, "the file's CRC refitted: the look finds the file good");
	lh_refused("its entry's CRC fails", UIPAK_DAMAGED, NULL);

	put_good();
	check(uipak_status() == UIPAK_OK && !flag_bad(), "the right one back: good");
	check(lh_boot(), "ToD boots");
	check(lh_settle() == MENU_READY && lh_in_game(t), "and the helper goes in");
	check(quit(), "quit");
}

void test_uipak(void)
{
	test_reader();
	if (g_good_len == 0) return;
	test_install();
	test_missing();
	test_other_build();
	test_damaged();
	test_update();
	test_rpcs3();
	test_sf_install();
	test_sf_failures();
	test_lh_reader();
	test_lh_install();
	test_lh_failures();
	put_good();
}
