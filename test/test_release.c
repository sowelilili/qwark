/*
 * The release check: what a release module (QWARK_DEV 0, src/core/proto.h)
 * carries, on qwark's fake console. test/run.sh builds this file on its own,
 * with the core, into test/qwark-test-release.exe, with -DQWARK_DEV=0 and
 * every other switch at its default, as the makefile builds `make release`;
 * it has its own main and its own count.
 *
 * Build 56's release left its preview features out. Build 57's ships
 * everything its test builds had (makefile, `release`): a release differs from
 * a test build by QWARK_DEV alone, the DEV bit and nothing else. With the
 * settings profiles and the USB mods set up on the fake HDD and a fake USB
 * drive - the switch on, a profile for the signed-in user, a mod only the
 * drive has and a newer copy of an installed one - the module must:
 *   - be built with the preview features, its games as modules and Tools of
 *     Destruction (QWARK_PREVIEW, QWARK_GAME_MODULES, QWARK_HAS_TOD);
 *   - set no DEV bit in SessionInfo's flags2, and still carry its build and
 *     its build ID in HELLO;
 *   - set the USB mods' and the profiles' bits, and answer MOD_USB_LIST,
 *     PROFILE_INFO, COMBO_OVERRIDE and GAME_MODULE;
 *   - make the signed-in user's profile active at the XMB, and take a newer
 *     USB copy and a mod only the drive has;
 *   - show the console menu's Mods page on its root;
 *   - know Tools of Destruction (the title index, tod.sprx, the Lua ops);
 *   - take only the ui.pak with ToD's Lua helper (pak.py --tod, what `make
 *     release` packs) and refuse the one without as another build's.
 *
 * Then test_config_files.c's checks, as a release reads and writes config.txt
 * and the profiles' files.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/mods.h"
#include "../src/core/modusb.h"
#include "../src/core/menu.h"
#include "../src/core/net.h"
#include "../src/core/uipak.h"
#include "../src/core/luacore.h"
#include "../src/core/util.h"
#include "../src/games/titles.h"
#include "../src/plat/plat.h"
#include "../src/plat/plat_net.h"
#include "../src/plat/host/plat_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if QWARK_DEV || !QWARK_PREVIEW || !QWARK_GAME_MODULES || !QWARK_HAS_TOD
#error "test_release.c is the release configuration: build it with -DQWARK_DEV=0 and the other switches at their defaults"
#endif

static int g_checks;
static int g_failures;

/* Not static: test_config_files.c, built into this suite too, counts in it (test_common.h). */
void check(int ok, const char *what)
{
	g_checks++;
	if (ok) {
		printf("  PASS  %s\n", what);
	} else {
		g_failures++;
		printf("  FAIL  %s\n", what);
	}
}

static void check_eq(u64 got, u64 want, const char *what)
{
	g_checks++;
	if (got == want) {
		printf("  PASS  %s\n", what);
	} else {
		g_failures++;
		printf("  FAIL  %s: got %llu want %llu\n", what, (unsigned long long)got, (unsigned long long)want);
	}
}

/* test_config_files.c, which both suites build. */
void test_config_files(void);

void check_eq_u64(u64 got, u64 want, const char *what)
{
	check_eq(got, want, what);
}

void group(const char *name)
{
	printf("\n%s\n", name);
}

/* ------------------------------------------------------------ the console */

#define T        "NPEA00385"
#define HDD      "/dev_hdd0/qwark/mods/" T
#define USB0     "/dev_usb000/qwark/mods/" T
#define USB1     "/dev_usb001/mods/" T
#define PROFILE  QWARK_PROFILEDIR "/user-00000001"
#define WORD_AT  0x00A20000u
#define USB_AT   0x00A20100u
/* test/run.sh packs both beside each other: the --tod pak as ui.pak, and this one without ToD's entries. */
#define PAK_NOTOD "/dev_hdd0/qwark/ui-notod.pak"

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int tick_until(u8 state, int max)
{
	int i;
	for (i = 0; i < max; i++) {
		tick(1);
		if (session_state() == state) return 1;
	}
	return 0;
}

static void put(const char *path, const char *text)
{
	plat_file_t f;
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, (u32)strlen(text));
	plat_file_close(f);
}

/* A whole file into `buf`, its length, or -1. */
static long slurp(const char *path, u8 *buf, u32 cap)
{
	plat_file_t f;
	u32 got = 0, n = 0;

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return -1;
	while (n < cap && plat_file_read(f, buf + n, cap - n, &got) == 0 && got > 0) n += got;
	plat_file_close(f);
	return (long)n;
}

static int spill(const char *path, const u8 *buf, u32 len)
{
	plat_file_t f;
	int ok;

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return 0;
	ok = plat_file_write(f, buf, len) == 0;
	plat_file_close(f);
	return ok;
}

static u32 peek32(u32 a)
{
	u8 b[4] = { 0, 0, 0, 0 };
	host_peek(a, b, 4);
	return be32_get(b);
}

/* Whether `len` bytes at `p` hold `s` anywhere. */
static int holds(const u8 *p, u32 len, const char *s)
{
	u32 n = (u32)strlen(s), i;

	for (i = 0; i + n <= len; i++)
		if (memcmp(p + i, s, n) == 0) return 1;
	return 0;
}

/* ------------------------------------------------------------- the wire */

static int g_sock = -1;
static u16 g_seq;

static int recv_all(u8 *buf, u32 n)
{
	u32 got = 0;
	while (got < n) {
		int r = recv(g_sock, (char *)buf + got, (int)(n - got), 0);
		if (r <= 0) return 0;
		got += (u32)r;
	}
	return 1;
}

/* One request; the status, or 0xFFFF when the connection failed. */
static u16 call(u16 op, const u8 *payload, u32 len, u8 *reply, u32 cap, u32 *rlen)
{
	static u8 frame[QWARK_FRAME_HEADER + 64];
	u8 header[QWARK_FRAME_HEADER];
	u32 n;

	if (g_sock < 0 || len > 64) return 0xFFFF;
	g_seq++;
	be32_put(frame, len);
	be16_put(frame + 4, g_seq);
	be16_put(frame + 6, op);
	if (len) memcpy(frame + QWARK_FRAME_HEADER, payload, len);
	if (send(g_sock, (const char *)frame, (int)(QWARK_FRAME_HEADER + len), 0) != (int)(QWARK_FRAME_HEADER + len))
		return 0xFFFF;
	if (!recv_all(header, QWARK_FRAME_HEADER)) return 0xFFFF;
	n = be32_get(header);
	if (be16_get(header + 4) != g_seq || n > cap) return 0xFFFF;
	if (n && !recv_all(reply, n)) return 0xFFFF;
	if (rlen) *rlen = n;
	return be16_get(header + 6);
}

static int connect_local(void)
{
	struct sockaddr_in sa;
	int waited;

	for (waited = 0; waited < 2000; waited += 20) {
		int s = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s < 0) return -1;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(net_port());
		sa.sin_addr.s_addr = htonl(0x7F000001u);
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) return s;
		plat_socket_close(s);
		plat_sleep_us(20000);
	}
	return -1;
}

/* A MOD_LIST row by folder name, 1 when the list has it. */
static int mod_listed(const u8 *reply, u32 len, const char *dir)
{
	u32 i;
	for (i = 0; len > 0 && i < reply[0]; i++) {
		const u8 *row = reply + 1 + i * MOD_WIRE_SIZE;
		if (1 + (i + 1) * MOD_WIRE_SIZE > len) break;
		if (strncmp((const char *)row + 8, dir, 32) == 0) return 1;
	}
	return 0;
}

/* ------------------------------------------------------------ the check */

static void set_up_both_features(void)
{
	/* The profiles: the switch on, and user 1's profile with a slot of its own. */
	put(QWARK_CONFIG, "log = 1\nprofiles.enabled = 1\nselected.slot = 2\n" T ".mod.both.auto = 1\n");
	put(PROFILE "/config.txt", "selected.slot = 5\n" T ".mod.both.auto = 1\n");
	put(PROFILE "/positions/rac1.txt", "");
	host_set_has_users(1);
	host_set_user(1);

	/*
	 * The USB mods: one only a drive has, on each of the two places a drive
	 * keeps them, and a newer copy of a mod the console has.
	 */
	put(USB0 "/usbonly/patch.txt", "#- name: USB only\n#- version: 1.0\n0x00A20100: 0x60000000\n");
	put(USB1 "/usbonly2/patch.txt", "#- name: USB only 2\n#- version: 1.0\n0x00A20200: 0x60000000\n");
	put(HDD "/both/patch.txt", "#- name: Both\n#- version: 1.0\n0x00A20000: 0x10000000\n");
	put(USB0 "/both/patch.txt", "#- name: Both\n#- version: 2.0\n0x00A20000: 0x20000000\n");
}

/* The ui.pak a release takes, and the one without ToD's Lua helper it refuses. */
static void test_release_pak(void)
{
	static u8 good[UIPAK_MAX_BODY + 64], notod[UIPAK_MAX_BODY + 64];
	long ng = slurp(UIPAK_PATH, good, sizeof(good));
	long nn = slurp(PAK_NOTOD, notod, sizeof(notod));

	group("--- release build: ui.pak with Tools of Destruction's Lua helper");
	check(ng > 32 && nn > 32 && nn < ng && good[20] == notod[20] + 4,
	      "run.sh packed both paks: the release's has four entries more, ToD's Lua helper and its three chunks");
	tick(4);
	check_eq(uipak_status(), UIPAK_OK, "the pak `make release` packs (pak.py --tod) is this module's");

	if (nn > 0 && spill(UIPAK_PATH, notod, (u32)nn)) {
		uipak_touch(UIPAK_PATH);
		tick(4);
		check_eq(uipak_status(), UIPAK_OTHER, "a pak without ToD's entries is another build's: a release refuses it");
	}
	if (ng > 0 && spill(UIPAK_PATH, good, (u32)ng)) {
		uipak_touch(UIPAK_PATH);
		tick(4);
		check_eq(uipak_status(), UIPAK_OK, "the release's pak back: good again");
	}
}

/* Tools of Destruction and the game modules, in a release since build 57. */
static void test_release_tod(void)
{
	u8 reply[64];
	u32 len = 0;
	u16 st;
	u8 g;
	static const char * const names[6] = { NULL, "rac1.sprx", "rac2.sprx", "rac3.sprx", "rac4.sprx", "tod.sprx" };

	group("--- release build: Tools of Destruction and the game modules");
	{
		const struct game_title *t[4];
		check(title_for_game(GAME_TOD) != NULL && title_candidates("NPEA00452", t, 4) > 0,
		      "the title index knows Tools of Destruction (NPEA00452)");
	}
	for (g = GAME_RAC1; g <= GAME_TOD; g++) {
		const char *n = title_module_name(g);
		char what[96];
		snprintf(what, sizeof(what), "game %d's module is %s", (int)g, names[g]);
		check(n != NULL && strcmp(n, names[g]) == 0, what);
	}

	core_lock();
	st = (u16)lua_core_status(reply, sizeof(reply), &len);
	core_unlock();
	check_eq(st, ST_NOT_INGAME, "the Lua ops are in: LUA_STATUS at the XMB is NOT_INGAME, not UNSUPPORTED");

	st = call(OP_GAME_MODULE, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && len == GAME_MODULE_SIZE, "GAME_MODULE answers, the memory gate's figures");
}

static void test_release(void)
{
	static const char * const watched[1] = { "/dev_usb" };
	static u8 reply[QWARK_MAX_PAYLOAD];
	plat_thread_t accept_thread = PLAT_THREAD_NONE;
	u32 len = 0;
	u16 st;
	u8 req[8];

	printf("--- release build: everything the test builds have, but the DEV bit\n");

	set_up_both_features();

	core_lock();
	config_load();
	core_unlock();

	/* Three seconds at the XMB: the signed-in user's profile is made active. */
	tick(360);
	check(config_profile_active(), "the signed-in user's profile is active");
	check_eq(config_selected_slot(), 5, "and its slot is the profile's, not config.txt's");

	test_release_pak();

	check(net_init() == ST_OK, "net_init");
	check(plat_thread_create(&accept_thread, net_accept_thread, NULL, 16384, "qwark_net") == 0,
	      "the accept thread starts");
	g_sock = connect_local();
	check(g_sock >= 0, "a client connects");

	test_release_tod();

	/* The boot: the USB look and the auto-load. */
	host_watch_paths(watched, 1);
	host_boot(T);
	check(tick_until(SESSION_INGAME, 4000), T " boots");
	check_eq(peek32(WORD_AT), 0x20000000u, "the auto-load takes the drive's newer copy (2.0)");
	check(host_watch_hits(0) > 0, "the boot looked at the USB drives");
	host_watch_paths(NULL, 0);

	req[0] = QWARK_PROTOCOL_VERSION;
	st = call(OP_HELLO, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && len == SESSION_INFO_SIZE + HELLO_BUILD_ID_SIZE, "HELLO answers, with the build ID after SessionInfo");
	if (st == ST_OK && len == SESSION_INFO_SIZE + HELLO_BUILD_ID_SIZE) {
		check_eq(reply[1], QWARK_BUILD, "qwark_version is this build");
		check_eq(QWARK_BUILD, 59, "build 59, the release after build 58");
		check_eq(reply[29] & SESSION_FLAG2_DEV, 0, "flags2 bit4 DEV is clear: a release");
		check_eq(be32_get(reply + SESSION_INFO_SIZE), QWARK_BUILD_ID, "the build ID is carried all the same");
		check_eq(reply[29] & SESSION_FLAG2_USB_MODS, SESSION_FLAG2_USB_MODS, "flags2 bit0: the USB mods");
		check_eq(reply[29] & (SESSION_PROFILE_SUPPORTED | SESSION_PROFILE_ON | SESSION_PROFILE_ACTIVE),
		         SESSION_PROFILE_SUPPORTED | SESSION_PROFILE_ON | SESSION_PROFILE_ACTIVE,
		         "flags2 bits 1 to 3: profiles supported, switched on, one active");
		check_eq(reply[29] & (SESSION_FLAG2_GAME_MODULE_BAD | SESSION_FLAG2_GAME_WAITING | SESSION_FLAG2_NO_MENU), 0,
		         "and no game-module or no-menu bit with RaC1's code in");
		check(reply[30] != 0, "byte 30, the profile sequence, has moved");
		check_eq(reply[25], 5, "SessionInfo's slot is the profile's");
		check_eq(reply[31], SESSION_FLAG3_RUN_WARN | SESSION_FLAG3_SETTINGS_133 | SESSION_FLAG3_TELEMETRY_RATE
		         | (TELEMETRY_RATE_CODE_60 << SESSION_FLAG3_RATE_SHIFT),
		         "flags3 bit0: the run-start warnings, on; bit5: cheats only to be had; bit2: the telemetry rate, at 60 Hz");
		check_eq(reply[24] & SESSION_FLAG_UI_PAK_BAD, 0, "flags bit7 clear: the pak is good");
	}

	/* Revision 1.32: a release has the telemetry rate too. */
	req[0] = 120;
	st = call(OP_TELEMETRY_RATE, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && len == 0, "TELEMETRY_RATE 120 answers OK, with nothing");
	req[0] = 90;
	check_eq(call(OP_TELEMETRY_RATE, req, 1, reply, sizeof(reply), &len), ST_BAD_ARG, "TELEMETRY_RATE 90 is BAD_ARG");
	tick(1);
	req[0] = QWARK_PROTOCOL_VERSION;
	st = call(OP_HELLO, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && len >= SESSION_INFO_SIZE
	      && (reply[31] & SESSION_FLAG3_RATE_MASK) == (TELEMETRY_RATE_CODE_120 << SESSION_FLAG3_RATE_SHIFT),
	      "and the next SessionInfo says 120 Hz");
	req[0] = 60;
	check_eq(call(OP_TELEMETRY_RATE, req, 1, reply, sizeof(reply), &len), ST_OK, "back to 60 Hz");

	/* Revision 1.33: a release takes RUN_WARN 2, cheats only, and refuses 3. */
	req[0] = RUN_WARN_CHEATS_ONLY;
	check_eq(call(OP_RUN_WARN, req, 1, reply, sizeof(reply), &len), ST_OK, "RUN_WARN 2 answers OK");
	req[0] = 3;
	check_eq(call(OP_RUN_WARN, req, 1, reply, sizeof(reply), &len), ST_BAD_ARG, "RUN_WARN 3 is BAD_ARG");
	/* SessionInfo is the published packet's, which goes out every second tick at 60 Hz. */
	tick(4);
	req[0] = QWARK_PROTOCOL_VERSION;
	st = call(OP_HELLO, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && len >= SESSION_INFO_SIZE
	      && (reply[31] & (SESSION_FLAG3_RUN_WARN_OFF | SESSION_FLAG3_RUN_WARN_CHEATS)) == SESSION_FLAG3_RUN_WARN_CHEATS,
	      "and the next SessionInfo says cheats only, bit6, and not off");
	req[0] = RUN_WARN_ALL;
	check_eq(call(OP_RUN_WARN, req, 1, reply, sizeof(reply), &len), ST_OK, "back to mods and cheats");

	/* Revision 1.33: and REBOOT_APPLY, written and read back; 0x0099 is no op. */
	st = call(OP_REBOOT_APPLY, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && len == 1 && reply[0] == REBOOT_APPLY_ASK, "REBOOT_APPLY with no byte reads Ask");
	req[0] = REBOOT_APPLY_ALWAYS;
	check_eq(call(OP_REBOOT_APPLY, req, 1, reply, sizeof(reply), &len), ST_OK, "REBOOT_APPLY 0 answers OK");
	req[0] = 3;
	check_eq(call(OP_REBOOT_APPLY, req, 1, reply, sizeof(reply), &len), ST_BAD_ARG, "REBOOT_APPLY 3 is BAD_ARG");
	st = call(OP_REBOOT_APPLY, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && len == 1 && reply[0] == REBOOT_APPLY_ALWAYS, "and reads back Always");
	req[0] = REBOOT_APPLY_ASK;
	check_eq(call(OP_REBOOT_APPLY, req, 1, reply, sizeof(reply), &len), ST_OK, "back to Ask");
	req[0] = 1;
	check_eq(call(0x0099, req, 1, reply, sizeof(reply), &len), ST_UNKNOWN_OP,
	         "0x0099, the test build's BOOT_ASK, is UNKNOWN_OP");
	tick(1);

	st = call(OP_MOD_USB_LIST, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && holds(reply, len, "usbonly") && holds(reply, len, "usbonly2"),
	      "MOD_USB_LIST answers, with both mods only a drive has");
	st = call(OP_PROFILE_INFO, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && len == PROFILE_INFO_SIZE && (reply[0] & PROFILE_INFO_HAS_PROFILE) && reply[1] == 1 &&
	      be32_get(reply + 4) == 1, "PROFILE_INFO answers: user 1, a profile made and active");

	memset(req, 0, sizeof(req));
	req[1] = 1;
	be32_put(req + 4, 0x1005u);
	st = call(OP_COMBO_OVERRIDE, req, 8, reply, sizeof(reply), &len);
	check_eq(st, ST_OK, "COMBO_OVERRIDE answers OK");
	st = call(OP_COMBO_LIST, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && len == 1u + COMBO_COUNT * 8u && (reply[2] & COMBO_LIST_OVERRIDDEN),
	      "and COMBO_LIST flags the first action OVERRIDDEN");
	memset(req, 0, sizeof(req));
	st = call(OP_COMBO_OVERRIDE, req, 8, reply, sizeof(reply), &len);
	check_eq(st, ST_OK, "the override cleared again");

	/* A rescan and a load, as the tick thread runs them for MOD_RESCAN and MOD_LOAD. */
	core_lock();
	mods_rescan();
	check_eq(mods_load("usbonly"), ST_OK, "MOD_LOAD of a mod only a drive has copies it in and loads it");
	core_unlock();
	check_eq(peek32(USB_AT), 0x60000000u, "its word is in the game");

	st = call(OP_MOD_LIST, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && mod_listed(reply, len, "both") && mod_listed(reply, len, "usbonly"),
	      "MOD_LIST lists the installed mod and the one copied off the drive");

	/* Build 56's preview feature, in a release since build 57: the console menu's Mods page. */
	{
		struct menu_view v;
		u8 i, mods = 0xFF;

		core_lock();
		menu_model_reset(7, 24, MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST);
		menu_model_view(&v);
		for (i = 0; i < v.nrows; i++)
			if (strcmp(v.rows[i].label, "Mods") == 0) mods = i;
		check(mods == 5 && v.nrows == 7 && v.rows[mods].id == MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_MODS) &&
		      strcmp(v.rows[6].label, "Save files") == 0,
		      "the console menu's root has its Mods row, between Unlocks and Save files, seven rows");
		core_unlock();
	}

	host_quit();
	check(tick_until(SESSION_XMB, 2000), "back at the XMB");

	net_stop();
	if (g_sock >= 0) plat_socket_close(g_sock);
	g_sock = -1;
	check(plat_thread_join(accept_thread) == 0, "the accept thread joins");
	net_wait_clients(2000000u);
	net_shutdown();

	/* What the config files' tests that follow expect to start from: the profiles off, no profile, no users. */
	{
		int changed = 0;

		core_lock();
		config_profiles_enable(0, PROFILE_SETUP_NONE, 1, &changed);
		core_unlock();
	}
	plat_file_unlink(PROFILE "/config.txt");
	plat_file_unlink(PROFILE "/positions/rac1.txt");
	host_set_has_users(0);
	put(QWARK_CONFIG, "log = 1\n");
	core_lock();
	config_load();
	core_unlock();
	tick(360);
	check(!config_profile_active(), "the profile let go again, for the config files' tests");
}

int main(void)
{
	const char *port_env = getenv("QWARK_TEST_PORT");

	if (port_env != NULL && port_env[0] != 0) net_set_port((u16)atoi(port_env));

	if (plat_init() != 0) {
		printf("plat_init failed\n");
		return 2;
	}
	if (session_init() != ST_OK) {
		printf("session_init failed\n");
		return 2;
	}

	test_release();
	/* config.txt and the profiles' files on a console's HDD and after a hand edit, as a release reads and writes them. */
	test_config_files();

	session_shutdown();
	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
	plat_shutdown();
	return g_failures == 0 ? 0 : 1;
}
