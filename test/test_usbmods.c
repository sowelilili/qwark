/*
 * Revision 1.24, build 56: mods on a USB drive (src/core/modusb.c, PROTOCOL.md
 * 5.7.2), on the fake console with fake drives in the host root:
 *
 *   the version rule, the scan of both places on every port and the choice
 *   between copies of one mod, the states against the console's library, the
 *   wire row, a load taking a USB-only mod (into a temporary folder, checked,
 *   swapped in, qwark.sum written), an update taken at load, the deferral while
 *   the mod is loaded, an older copy, an equal version with other contents, a
 *   missing version, a pulled drive (a USB-only mod and an update), a mod
 *   deleted from the console, auto-load on, a dependency only USB has, the
 *   rescan keeping slots, the rate limit, the bounds, and RPCS3.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/util.h"
#include "../src/core/mods.h"
#include "../src/core/modusb.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define T        "NPEA00385"
#define HDD      "/dev_hdd0/qwark/mods/" T
#define USB0     "/dev_usb000/qwark/mods/" T
#define USB0M    "/dev_usb000/mods/" T
#define USB1     "/dev_usb001/qwark/mods/" T
#define USB6M    "/dev_usb006/mods/" T

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

static void put(const char *path, const void *data, u32 len)
{
	plat_file_t f;
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, data, len);
	plat_file_close(f);
}

static void put_text(const char *folder, const char *leaf, const char *text)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", folder, leaf);
	put(path, text, (u32)strlen(text));
}

static int read_text(const char *path, char *out, u32 cap)
{
	return qread_file(path, out, cap, NULL) == ST_OK;
}

static int exists(const char *path)
{
	return plat_path_exists(path, NULL, NULL);
}

/* Test code, so plain recursion: the trees here are two levels deep. */
static void rm_tree(const char *path)
{
	plat_dir_t d;
	struct plat_dirent e;
	int is_dir = 0;

	if (!plat_path_exists(path, &is_dir, NULL)) return;
	if (!is_dir) { plat_file_unlink(path); return; }
	if (plat_dir_open(path, &d) == 0) {
		while (plat_dir_next(&d, &e) == 1) {
			char child[512];
			if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
			snprintf(child, sizeof(child), "%s/%s", path, e.name);
			rm_tree(child);
		}
		plat_dir_close(&d);
	}
	plat_dir_remove(path);
}

/*
 * A mod folder: name, version (none when NULL), one word at `addr`, and a cave
 * file named `bin` at addr + 0x100 holding `cave` when bin is not NULL.
 */
static void make_mod(const char *root, const char *dir, const char *name, const char *version,
                     u32 addr, u32 value, const char *bin, const char *cave, const char *depends)
{
	char folder[256];
	char text[512];
	int n;

	snprintf(folder, sizeof(folder), "%s/%s", root, dir);
	n = snprintf(text, sizeof(text), "#- name: %s\n", name);
	if (version) n += snprintf(text + n, sizeof(text) - n, "#- version: %s\n", version);
	n += snprintf(text + n, sizeof(text) - n, "#- author: tester\n");
	if (depends) n += snprintf(text + n, sizeof(text) - n, "#- depends: %s\n", depends);
	n += snprintf(text + n, sizeof(text) - n, "0x%08X: 0x%08X\n", addr, value);
	if (bin) {
		char path[300];
		snprintf(text + n, sizeof(text) - n, "0x%08X: %s\n", addr + 0x100u, bin);
		snprintf(path, sizeof(path), "%s/%s", folder, bin);
		put(path, cave, (u32)strlen(cave));
	}
	put_text(folder, "patch.txt", text);
}

/* The client's hash over a folder: patch.txt, then the one .bin. */
static u32 folder_hash(const char *folder, const char *bin)
{
	static char text[8192];
	char path[300];
	u32 len = 0;
	u32 crc;

	snprintf(path, sizeof(path), "%s/patch.txt", folder);
	if (qread_file(path, text, sizeof(text), &len) != ST_OK) return 0;
	crc = qcrc32_update(qcrc32_start(), (const u8 *)text, len);
	if (bin) {
		snprintf(path, sizeof(path), "%s/%s", folder, bin);
		if (qread_file(path, text, sizeof(text), &len) == ST_OK)
			crc = qcrc32_update(crc, (const u8 *)text, len);
	}
	return qcrc32_finish(crc);
}

/* An installed copy the way the client uploads one: the files and qwark.sum. */
static void install_hdd(const char *dir, const char *name, const char *version, u32 addr, u32 value,
                        const char *bin, const char *cave)
{
	char folder[256];
	char sum[12];

	make_mod(HDD, dir, name, version, addr, value, bin, cave, NULL);
	snprintf(folder, sizeof(folder), "%s/%s", HDD, dir);
	qfmt_hex(sum, sizeof(sum), folder_hash(folder, bin), 8);
	put_text(folder, "qwark.sum", sum);
}

static u32 peek32(u32 a)
{
	u8 b[4] = { 0, 0, 0, 0 };
	host_peek(a, b, 4);
	return be32_get(b);
}

static void poke32(u32 a, u32 v)
{
	u8 b[4];
	be32_put(b, v);
	host_poke(a, b, 4);
}

static const struct modusb_entry *usb(const char *dir)
{
	int i = modusb_find(dir);
	return i >= 0 ? modusb_at((u32)i) : NULL;
}

static u8 state_of(const char *dir, u8 *flags)
{
	const struct modusb_entry *u = usb(dir);
	u8 f = 0;
	u8 s = u ? modusb_state(u, &f) : 0xFF;
	if (flags) *flags = f;
	return s;
}

static const struct mod_entry *hdd(const char *dir)
{
	int i = mods_find(dir);
	return i >= 0 ? mods_at((u32)i) : NULL;
}

static void rescan_all(void)
{
	mods_rescan();
	modusb_scan(1);
}

static void clean(void)
{
	static const char *const dirs[] = {
		"usbonly", "upd", "olddrv", "same", "conflict", "nover", "nover2", "pulled", "pullupd",
		"autoon", "deluser", "depuser", "depmod", "broken", "toolong", "evil", "dup", "dup2",
		"dupeq", "late", "first", "aaa-new", "sizey", "uponly",
	};
	u32 i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char p[256];
		snprintf(p, sizeof(p), "%s/%s", HDD, dirs[i]);
		rm_tree(p);
		config_set_mod_auto(T, dirs[i], 0);
	}
	rm_tree("/dev_usb000");
	rm_tree("/dev_usb001");
	rm_tree("/dev_usb006");
	rm_tree("/dev_hdd0/qwark/usbtmp");
}

static void test_versions(void)
{
	int c = 9;

	group("USB mods: the version rule");

	check(modusb_version_cmp("1.0", "1.00", &c) && c == 0, "1.0 and 1.00 are the same version");
	check(modusb_version_cmp("1.10", "1.9", &c) && c > 0, "1.10 is newer than 1.9, as numbers");
	check(modusb_version_cmp("v2", "1.9.9", &c) && c > 0, "a leading v is allowed, and 2 beats 1.9.9");
	check(modusb_version_cmp("1.0", "1.0.0", &c) && c == 0, "a missing part is 0");
	check(modusb_version_cmp("0.6", "0.10", &c) && c < 0, "0.6 is older than 0.10");
	check(modusb_version_cmp(" 3.8 ", "3.8", &c) && c == 0, "spaces around are ignored");
	check(!modusb_version_cmp("1.0-beta", "1.0", &c), "a suffix is no version to compare");
	check(!modusb_version_cmp("", "1.0", &c), "an empty version is no version to compare");
	check(!modusb_version_cmp("1.0", "final", &c), "a word is no version to compare");
	check(!modusb_version_cmp("1.2.3.4.5", "1.0", &c), "five parts is no version to compare");
	check(!modusb_version_cmp("1..2", "1.0", &c), "an empty part is no version to compare");
}

static void test_scan(void)
{
	const struct modusb_entry *u;
	u8 buf[1 + MODUSB_MAX * MODUSB_WIRE_SIZE];
	u32 len = 0;
	u8 flags = 0;
	u8 info[SESSION_INFO_SIZE];
	char big[300];
	int found;
	u32 i;

	group("USB mods: the scan, both places on every port");

	make_mod(USB0, "usbonly", "USB Only", "1.0", 0x00850000u, 0x60000000u, "cave.bin", "ABCDEFGHIJ", NULL);
	/* One mod in three places: the newest, on the second drive's short path, wins. */
	make_mod(USB0, "dup", "Dup", "1.0", 0x00850100u, 0x11111111u, NULL, NULL, NULL);
	make_mod(USB0M, "dup", "Dup", "1.2", 0x00850100u, 0x22222222u, NULL, NULL, NULL);
	make_mod(USB6M, "dup", "Dup", "1.1", 0x00850100u, 0x33333333u, NULL, NULL, NULL);
	/* Equal versions, other contents: the first found, port 0's qwark/mods, and no conflict. */
	make_mod(USB1, "dupeq", "Dup eq", "2.0", 0x00850200u, 0x44444444u, NULL, NULL, NULL);
	make_mod(USB0M, "dupeq", "Dup eq", "2.0", 0x00850200u, 0x55555555u, NULL, NULL, NULL);
	/* A copy that cannot be taken loses to one that can, whatever its version. */
	make_mod(USB0, "dup2", "Dup two", "9.0", 0x00850300u, 0x1u, "gone.bin", "x", NULL);
	plat_file_unlink(USB0 "/dup2/gone.bin");
	make_mod(USB1, "dup2", "Dup two", "1.0", 0x00850300u, 0x2u, NULL, NULL, NULL);
	/* Problems. */
	make_mod(USB0, "broken", "Broken", "1.0", 0x00850400u, 0x1u, "missing.bin", "x", NULL);
	plat_file_unlink(USB0 "/broken/missing.bin");
	put_text(USB0 "/evil", "patch.txt", "#- name: Evil\n#- version: 1.0\n0x00850500: ../../../escape.bin\n");
	make_mod(USB0, "sizey", "Sizey", "1.0", 0x00850600u, 0x1u, NULL, NULL, NULL);
	put_text(USB0 "/sizey", "patch.txt", "#- name: Sizey\n#- version: 1.0\n0x00850600: huge.bin\n");
	{
		static u8 huge[MODUSB_FILE_MAX + 16];
		memset(huge, 0x42, sizeof(huge));
		put(USB0 "/sizey/huge.bin", huge, sizeof(huge));
	}
	/* A folder name longer than 31 bytes is skipped, and a folder with no patch.txt is no mod. */
	snprintf(big, sizeof(big), "%s/%s", USB0, "this-folder-name-is-much-too-long-for-qwark");
	make_mod(USB0, "this-folder-name-is-much-too-long-for-qwark", "Long", "1.0", 0x00850700u, 1, NULL, NULL, NULL);
	put_text(USB0 "/notamod", "readme.txt", "hello");

	check(quit(), "back to the XMB");
	check(boot(T), "RaC1 boots, which reads the drives");

	u = usb("usbonly");
	check(u != NULL, "the USB-only mod is listed");
	if (u != NULL) {
		check(qstreq(u->name, "USB Only") && qstreq(u->version, "1.0") && qstreq(u->author, "tester"),
		      "with its name, version and author");
		check(u->port == 0 && u->root == MODUSB_ROOT_QWARK, "on /dev_usb000/qwark/mods");
		check_eq_u64(u->hash, folder_hash(USB0 "/usbonly", "cave.bin"), "its hash is the client's rule");
		check(state_of("usbonly", NULL) == MODUSB_AVAILABLE, "it is AVAILABLE");
	}
	check(hdd("usbonly") == NULL, "and not installed: MOD_LIST does not have it");

	u = usb("dup");
	check(u != NULL && qstreq(u->version, "1.2") && u->port == 0 && u->root == MODUSB_ROOT_MODS,
	      "of three copies the newest is kept, from /dev_usb000/mods");
	state_of("dup", &flags);
	check((flags & MODUSB_FLAG_ROOT_MODS) != 0, "and its row says /dev_usb00N/mods");

	u = usb("dupeq");
	check(u != NULL && u->port == 0 && u->root == MODUSB_ROOT_MODS,
	      "equal versions: the first found, the lower port, is kept");
	check(state_of("dupeq", NULL) == MODUSB_AVAILABLE, "and it is AVAILABLE, not a conflict");

	u = usb("dup2");
	check(u != NULL && u->port == 1 && u->problem == MODUSB_PROBLEM_NONE,
	      "a copy that can be taken beats a newer one that cannot");

	check(state_of("broken", NULL) == MODUSB_BROKEN && usb("broken")->problem == MODUSB_PROBLEM_MISSING,
	      "a missing .bin is BROKEN, MISSING");
	check(state_of("evil", NULL) == MODUSB_BROKEN && usb("evil")->problem == MODUSB_PROBLEM_NAME,
	      "a .bin name that climbs out is BROKEN, NAME");
	check(state_of("sizey", NULL) == MODUSB_BROKEN && usb("sizey")->problem == MODUSB_PROBLEM_SIZE,
	      "a .bin over 256 KB is BROKEN, SIZE");
	found = 0;
	for (i = 0; i < modusb_count(); i++) {
		const struct modusb_entry *e = modusb_at(i);
		if (strncmp(e->dirname, "this-folder", 11) == 0 || qstreq(e->dirname, "notamod")) found = 1;
	}
	check(!found, "a long folder name and a folder with no patch.txt are not listed");

	group("USB mods: MOD_USB_LIST and flags2");

	check(modusb_list_encode(buf, sizeof(buf), &len) == ST_OK, "MOD_USB_LIST encodes");
	check_eq_u64(len, 1u + buf[0] * MODUSB_WIRE_SIZE, "n rows of 120 bytes");
	check_eq_u64(buf[0], modusb_count(), "one row per mod");
	found = 0;
	for (i = 0; i < buf[0]; i++) {
		const u8 *r = buf + 1 + i * MODUSB_WIRE_SIZE;
		if (memcmp(r + 8, "usbonly", 8) != 0) continue;
		found = 1;
		check(r[0] == 0 && r[1] == MODUSB_AVAILABLE && r[2] == 0 && r[3] == 0,
		      "the row: port 0, AVAILABLE, no flags, no problem");
		check_eq_u64(be32_get(r + 4), usb("usbonly")->hash, "the hash, big-endian");
		check(memcmp(r + 40, "USB Only", 9) == 0 && memcmp(r + 72, "1.0", 4) == 0 &&
		      memcmp(r + 88, "tester", 7) == 0, "name, version, author at 40, 72, 88");
	}
	check(found, "the USB-only mod is a row");
	check(modusb_list_encode(buf, 10, &len) == ST_FULL, "a buffer too small is FULL");

	tick(1);
	session_info_copy(info, sizeof(info));
	check((info[29] & SESSION_FLAG2_USB_MODS) != 0, "SessionInfo flags2 bit0 says USB mods are read");

	group("USB mods: when the drives are read");

	make_mod(USB0, "late", "Late", "1.0", 0x00850800u, 1, NULL, NULL, NULL);
	modusb_scan(0);
	check(usb("late") == NULL, "a list asked for within a second of the last look does not look again");
	host_advance_time(1100000u);
	modusb_scan(0);
	check(usb("late") != NULL, "a second later it does");
	make_mod(USB0, "first", "First", "1.0", 0x00850900u, 1, NULL, NULL, NULL);
	mods_rescan();
	modusb_scan(1);
	check(usb("first") != NULL, "MOD_RESCAN's scan looks at once");
}

static void test_take(void)
{
	char text[256];
	char sum[16];
	const struct mod_entry *m;
	u8 flags = 0;
	int slot;

	group("USB mods: a load takes a USB-only mod");

	/* A stale auto key, as a deleted copy of the same folder would leave. */
	config_set_mod_auto(T, "usbonly", 1);
	poke32(0x00850000u, 0xDEADBEEFu);
	check(mods_load("usbonly") == ST_OK, "MOD_LOAD of a USB-only mod answers OK");
	m = hdd("usbonly");
	check(m != NULL && (m->flags & MOD_FLAG_LOADED) != 0, "it is installed and loaded");
	check_eq_u64(peek32(0x00850000u), 0x60000000u, "its word is in the game");
	check(read_text(HDD "/usbonly/cave.bin", text, sizeof(text)) && qstreq(text, "ABCDEFGHIJ"),
	      "its .bin was copied");
	check(read_text(HDD "/usbonly/qwark.sum", sum, sizeof(sum)), "qwark.sum was written");
	{
		char want[12];
		qfmt_hex(want, sizeof(want), usb("usbonly")->hash, 8);
		check(qstreq(sum, want), "holding the hash, lowercase hex, as the client writes it");
	}
	check(m != NULL && m->hash == usb("usbonly")->hash, "so MOD_LIST's hash is the client's");
	check(!exists("/dev_hdd0/qwark/usbtmp/new") && !exists("/dev_hdd0/qwark/usbtmp/old"),
	      "no temporary folder is left");
	check(!config_mod_auto(T, "usbonly") && (m == NULL || (m->flags & MOD_FLAG_AUTO) == 0),
	      "a stale auto key is cleared: it does not come back auto-loading");
	check(state_of("usbonly", NULL) == MODUSB_SAME, "the USB copy is SAME now");
	mods_unload("usbonly");

	group("USB mods: an update, the deferral");

	install_hdd("upd", "Upd", "1.0", 0x00851000u, 0x10000000u, NULL, NULL);
	make_mod(USB0, "upd", "Upd", "1.1", 0x00851000u, 0x11000000u, NULL, NULL, NULL);
	rescan_all();
	slot = mods_find("upd");
	check(state_of("upd", &flags) == MODUSB_NEWER && (flags & MODUSB_FLAG_WAITING) == 0,
	      "a newer USB copy is NEWER");
	check(mods_load("upd") == ST_OK, "the load");
	m = hdd("upd");
	check(m != NULL && qstreq(m->version, "1.1"), "took the USB copy: the console has 1.1");
	check_eq_u64(peek32(0x00851000u), 0x11000000u, "and 1.1's word is in the game");
	check(mods_find("upd") == slot, "the mod kept its slot through the rescan");
	check(state_of("upd", NULL) == MODUSB_SAME, "the USB copy is SAME now");

	make_mod(USB0, "upd", "Upd", "1.2", 0x00851000u, 0x12000000u, NULL, NULL, NULL);
	host_advance_time(1100000u);
	modusb_scan(1);
	check(state_of("upd", &flags) == MODUSB_NEWER && (flags & MODUSB_FLAG_WAITING) != 0,
	      "1.2 on USB while 1.1 is loaded: NEWER, WAITING");
	check(mods_load("upd") == ST_OK, "a load of the loaded mod answers OK");
	check(qstreq(hdd("upd")->version, "1.1"), "and does not touch the loaded mod's files");
	check(read_text(HDD "/upd/patch.txt", text, sizeof(text)) && strstr(text, "1.1") != NULL,
	      "1.1 is still on the console");
	check_eq_u64(peek32(0x00851000u), 0x11000000u, "the game still has 1.1's word");
	check(mods_unload("upd") == ST_OK, "unloaded");
	check(mods_load("upd") == ST_OK, "loaded again");
	check(qstreq(hdd("upd")->version, "1.2"), "the next load takes 1.2");
	check_eq_u64(peek32(0x00851000u), 0x12000000u, "and 1.2's word is in the game");
	mods_unload("upd");

	group("USB mods: the console's copy stays");

	install_hdd("olddrv", "Old drive", "1.0", 0x00851100u, 0xA0000000u, NULL, NULL);
	make_mod(USB0, "olddrv", "Old drive", "0.9", 0x00851100u, 0xA9000000u, NULL, NULL, NULL);
	install_hdd("conflict", "Conflict", "1.0", 0x00851200u, 0xB0000000u, NULL, NULL);
	make_mod(USB0, "conflict", "Conflict", "1.0", 0x00851200u, 0xB1000000u, NULL, NULL, NULL);
	install_hdd("nover", "No version", NULL, 0x00851300u, 0xC0000000u, NULL, NULL);
	make_mod(USB0, "nover", "No version", "2.0", 0x00851300u, 0xC2000000u, NULL, NULL, NULL);
	install_hdd("nover2", "No version 2", NULL, 0x00851400u, 0xD0000000u, NULL, NULL);
	make_mod(USB0, "nover2", "No version 2", NULL, 0x00851400u, 0xD0000000u, NULL, NULL, NULL);
	install_hdd("same", "Same", "1.0", 0x00851500u, 0xE0000000u, NULL, NULL);
	make_mod(USB1, "same", "Same", "1.0", 0x00851500u, 0xE0000000u, NULL, NULL, NULL);
	rescan_all();

	check(state_of("olddrv", NULL) == MODUSB_OLDER, "an older USB copy is OLDER");
	check(state_of("conflict", NULL) == MODUSB_CONFLICT, "an equal version with other contents is CONFLICT");
	check(state_of("nover", NULL) == MODUSB_CONFLICT, "no version on the console's side: CONFLICT, never NEWER");
	check(state_of("nover2", NULL) == MODUSB_SAME, "no version either side, the same contents: SAME");
	check(state_of("same", NULL) == MODUSB_SAME, "the same copy is SAME");

	check(mods_load("olddrv") == ST_OK && peek32(0x00851100u) == 0xA0000000u, "an older copy: the console's loads");
	check(mods_load("conflict") == ST_OK && peek32(0x00851200u) == 0xB0000000u, "a conflict: the console's loads");
	check(mods_load("nover") == ST_OK && peek32(0x00851300u) == 0xC0000000u, "no version: the console's loads");
	check(read_text(HDD "/conflict/patch.txt", text, sizeof(text)) && strstr(text, "0xB0000000") != NULL,
	      "and its files are untouched");
	mods_unload("olddrv");
	mods_unload("conflict");
	mods_unload("nover");

	/* A console copy with no qwark.sum is compared by its contents all the same. */
	plat_file_unlink(HDD "/same/qwark.sum");
	rescan_all();
	check(hdd("same") != NULL && hdd("same")->hash == 0, "a console copy without qwark.sum has hash 0");
	check(state_of("same", NULL) == MODUSB_SAME, "and is still SAME by its contents");
}

static void test_pulled(void)
{
	char text[256];

	group("USB mods: a pulled drive");

	make_mod(USB0, "pulled", "Pulled", "1.0", 0x00852000u, 0x1u, "p.bin", "0123456789abcdef0123", NULL);
	install_hdd("pullupd", "Pull upd", "1.0", 0x00852100u, 0x20000000u, NULL, NULL);
	make_mod(USB0, "pullupd", "Pull upd", "1.5", 0x00852100u, 0x25000000u, "u.bin", "zzzz", NULL);
	rescan_all();
	check(state_of("pulled", NULL) == MODUSB_AVAILABLE, "a USB-only mod is AVAILABLE");

	/* patch.txt's two reads (its bytes, then the end), then the .bin's first fails as the drive goes. */
	host_pull_drive("/dev_usb000", 3);
	check(mods_load("pulled") == ST_IO_ERROR, "the load of a USB-only mod answers IO_ERROR");
	check(!exists(HDD "/pulled"), "nothing went into the library");
	check(!exists("/dev_hdd0/qwark/usbtmp/new"), "the temporary folder is gone");
	host_pull_drive(NULL, 0);

	host_advance_time(1100000u);
	modusb_scan(1);
	host_pull_drive("/dev_usb000", 3);
	check(mods_load("pullupd") == ST_OK, "an update cut short: the load still answers OK");
	check(read_text(HDD "/pullupd/patch.txt", text, sizeof(text)) && strstr(text, "1.0") != NULL,
	      "the console's 1.0 is untouched");
	check_eq_u64(peek32(0x00852100u), 0x20000000u, "and it is the copy that loaded");
	mods_unload("pullupd");

	host_pull_drive("/dev_usb000", 0);
	host_advance_time(1100000u);
	modusb_scan(0);
	check(usb("pulled") == NULL && usb("pullupd") == NULL, "with the drive gone, its mods are not listed");
	check(hdd("pullupd") != NULL, "and the console's own copy is still listed");
	host_pull_drive(NULL, 0);
	modusb_scan(1);
}

static void test_deleted_auto_deps(void)
{
	const struct mod_entry *m;

	group("USB mods: deleted on the console, still on USB");

	make_mod(USB0, "deluser", "Del user", "1.0", 0x00853000u, 0x30000000u, NULL, NULL, NULL);
	rescan_all();
	check(mods_load("deluser") == ST_OK, "loaded once from USB");
	check(mods_set_auto("deluser", 1) == ST_OK, "and set to auto-load");
	mods_unload("deluser");
	rm_tree(HDD "/deluser");
	rescan_all();
	check(hdd("deluser") == NULL && state_of("deluser", NULL) == MODUSB_AVAILABLE,
	      "deleted from the console, it is AVAILABLE again");

	poke32(0x00853000u, 0);
	check(quit() && boot(T), "a reboot");
	check(hdd("deluser") == NULL, "is not copied by the auto-load the old key asks for");
	check_eq_u64(peek32(0x00853000u), 0, "and is not in the game");

	group("USB mods: auto-load on, a dependency on USB");

	make_mod(USB0, "autoon", "Auto on", "1.0", 0x00853100u, 0x31000000u, NULL, NULL, NULL);
	modusb_scan(1);
	check(mods_set_auto("autoon", 1) == ST_OK, "MOD_SET_AUTO on for a USB-only mod answers OK");
	m = hdd("autoon");
	check(m != NULL && (m->flags & MOD_FLAG_AUTO) != 0 && (m->flags & MOD_FLAG_LOADED) == 0,
	      "it is copied in with auto on, and not loaded");
	check(mods_set_auto("nobody-anywhere", 1) == ST_NOT_FOUND, "a name nowhere is NOT_FOUND");

	/* A newer copy of an auto-load mod is taken by the next boot's auto-load. */
	make_mod(USB0, "autoon", "Auto on", "1.1", 0x00853100u, 0x31100000u, NULL, NULL, NULL);
	check(quit() && boot(T), "a reboot");
	m = hdd("autoon");
	check(m != NULL && qstreq(m->version, "1.1") && (m->flags & MOD_FLAG_LOADED) != 0,
	      "the auto-load took 1.1 and loaded it");
	check_eq_u64(peek32(0x00853100u), 0x31100000u, "1.1's word is in the game");
	mods_set_auto("autoon", 0);
	mods_unload("autoon");

	install_hdd("depuser", "Dep user", "1.0", 0x00853200u, 0x32000000u, NULL, NULL);
	put_text(HDD "/depuser", "patch.txt",
	         "#- name: Dep user\n#- version: 1.0\n#- depends: Dep Mod\n0x00853200: 0x32000000\n");
	plat_file_unlink(HDD "/depuser/qwark.sum");
	make_mod(USB1, "depmod", "Dep Mod", "1.0", 0x00853300u, 0x33000000u, NULL, NULL, NULL);
	rescan_all();
	check(hdd("depmod") == NULL, "the dependency is only on USB");
	check(mods_load("depuser") == ST_OK, "loading the mod that names it answers OK");
	check(hdd("depmod") != NULL && (hdd("depmod")->flags & MOD_FLAG_LOADED) != 0,
	      "the dependency was taken from USB and loaded first");
	check_eq_u64(peek32(0x00853300u), 0x33000000u, "its word is in the game");
	mods_unload("depuser");
	mods_unload("depmod");
}

static void test_rpcs3(void)
{
	u8 buf[64];
	u8 info[SESSION_INFO_SIZE];
	u32 len = 0;

	group("USB mods: nothing under RPCS3");

	host_set_can_patch_code(0);
	modusb_scan(1);
	check_eq_u64(modusb_count(), 0, "no scan: the table is empty");
	check(modusb_list_encode(buf, sizeof(buf), &len) == ST_UNSUPPORTED, "MOD_USB_LIST is UNSUPPORTED");
	tick(1);
	session_info_copy(info, sizeof(info));
	check((info[29] & SESSION_FLAG2_USB_MODS) == 0, "flags2 bit0 is clear");
	check_eq_u64(modusb_prepare("usbonly"), ST_OK, "a load's preparation does nothing");
	host_set_can_patch_code(1);
	modusb_scan(1);
}

void test_usbmods(void)
{
	/* The clock moves only with the ticks, so "within a second of the last look" is exact. */
	host_freeze_time(1);
	clean();
	mods_rescan();
	test_versions();
	test_scan();
	test_take();
	test_pulled();
	test_deleted_auto_deps();
	test_rpcs3();
	clean();
	mods_rescan();
	modusb_scan(1);
	host_freeze_time(0);
}
