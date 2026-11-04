/*
 * The release check: what a release module (QWARK_DEV 0, QWARK_PREVIEW 0,
 * src/core/proto.h) leaves out, on qwark's fake console. test/run.sh builds
 * this file on its own, with the core, into test/qwark-test-release.exe, with
 * -DQWARK_DEV=0 -DQWARK_PREVIEW=0 and without src/core/modusb.c, as the
 * makefile builds `make release`; it has its own main and its own count.
 *
 * Build 56 left two features out of the release: the settings profiles and
 * the USB mods. With both set up on the fake HDD and a fake USB drive - the
 * switch on, a profile for the signed-in user, a mod only the drive has and a
 * newer copy of an installed one - the module must:
 *   - set none of their bits in SessionInfo's flags2, and no DEV bit;
 *   - answer MOD_USB_LIST, PROFILE_INFO, PROFILE_ENABLE and COMBO_OVERRIDE
 *     UNSUPPORTED, and flag no COMBO_LIST row as overridden;
 *   - read config.txt alone and load the console's own copy of a mod, as
 *     1.4.1 did, and not know a mod only the drive has;
 *   - never use a path under /dev_usb or /dev_hdd0/qwark/profiles, at the XMB
 *     (where the profile look would run), through a boot (the USB look and the
 *     auto-load), over the wire, at a rescan and a load.
 *
 * Build 57: then test_config_files.c's config.txt checks, as a release reads
 * and writes the file (short reads, an open that fails, a write cut short, a
 * hand edit from Windows).
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/mods.h"
#include "../src/core/menu.h"
#include "../src/core/net.h"
#include "../src/core/util.h"
#include "../src/plat/plat.h"
#include "../src/plat/plat_net.h"
#include "../src/plat/host/plat_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if QWARK_PREVIEW || QWARK_DEV
#error "test_release.c is the release configuration: build it with -DQWARK_DEV=0 -DQWARK_PREVIEW=0"
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

static int exists(const char *path)
{
	return plat_path_exists(path, NULL, NULL);
}

static u32 peek32(u32 a)
{
	u8 b[4] = { 0, 0, 0, 0 };
	host_peek(a, b, 4);
	return be32_get(b);
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
	put(PROFILE "/config.txt", "selected.slot = 5\n");
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

static void test_release(void)
{
	static const char * const watched[2] = { "/dev_usb", QWARK_PROFILEDIR };
	static u8 reply[QWARK_MAX_PAYLOAD];
	plat_thread_t accept_thread = PLAT_THREAD_NONE;
	u32 len = 0;
	u16 st;
	u8 req[8];

	printf("--- release build: the settings profiles and the USB mods are left out\n");

	check(QWARK_PREVIEW == 0 && QWARK_DEV == 0, "this is the release configuration");

	set_up_both_features();
	host_watch_paths(watched, 2);

	core_lock();
	config_load();
	core_unlock();

	/* Three seconds at the XMB: a test build would have made user 1's profile active. */
	tick(360);
	check_eq(config_selected_slot(), 2, "config.txt's slot, not the profile's: the switch is not obeyed");

	check(net_init() == ST_OK, "net_init");
	check(plat_thread_create(&accept_thread, net_accept_thread, NULL, 16384, "qwark_net") == 0,
	      "the accept thread starts");
	g_sock = connect_local();
	check(g_sock >= 0, "a client connects");

	/* The boot: the USB look and the auto-load. */
	host_boot(T);
	check(tick_until(SESSION_INGAME, 4000), T " boots");
	check_eq(peek32(WORD_AT), 0x10000000u, "the auto-load loads the console's copy, not the drive's newer one");

	req[0] = QWARK_PROTOCOL_VERSION;
	st = call(OP_HELLO, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && len == SESSION_INFO_SIZE + HELLO_BUILD_ID_SIZE, "HELLO answers");
	if (st == ST_OK) {
		check_eq(reply[29], 0, "flags2 is 0: no USB mods, no profiles, no DEV");
		check_eq(reply[30], 0, "and byte 30, the profile sequence, is 0");
		check_eq(reply[25], 2, "SessionInfo's slot is config.txt's");
		check_eq(reply[31], SESSION_FLAG3_RUN_WARN,
		         "flags3 bit0: the run-start warnings are in a release too (revision 1.30), on");
	}

	/* Revision 1.30 (pending): RUN_WARN, not a preview feature. */
	req[0] = 2;
	st = call(OP_RUN_WARN, req, 1, reply, sizeof(reply), &len);
	check_eq(st, ST_BAD_ARG, "RUN_WARN 2 is BAD_ARG");
	req[0] = 0;
	st = call(OP_RUN_WARN, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && !config_run_warnings(), "RUN_WARN 0 answers OK and writes run.warnings = 0");
	req[0] = 1;
	st = call(OP_RUN_WARN, req, 1, reply, sizeof(reply), &len);
	check(st == ST_OK && config_run_warnings(), "RUN_WARN 1 puts it back");

	st = call(OP_MOD_USB_LIST, NULL, 0, reply, sizeof(reply), &len);
	check_eq(st, ST_UNSUPPORTED, "MOD_USB_LIST is UNSUPPORTED");
	st = call(OP_PROFILE_INFO, NULL, 0, reply, sizeof(reply), &len);
	check_eq(st, ST_UNSUPPORTED, "PROFILE_INFO is UNSUPPORTED");
	req[0] = 1;
	req[1] = PROFILE_SETUP_COPY;
	st = call(OP_PROFILE_ENABLE, req, 2, reply, sizeof(reply), &len);
	check_eq(st, ST_UNSUPPORTED, "PROFILE_ENABLE is UNSUPPORTED");
	memset(req, 0, sizeof(req));
	req[1] = 1;
	be32_put(req + 4, 0x1005u);
	st = call(OP_COMBO_OVERRIDE, req, 8, reply, sizeof(reply), &len);
	check_eq(st, ST_UNSUPPORTED, "COMBO_OVERRIDE is UNSUPPORTED");

	st = call(OP_COMBO_LIST, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && len == 1u + COMBO_COUNT * 8u, "COMBO_LIST answers seven rows");
	if (st == ST_OK) {
		u32 i;
		int clear = 1;
		for (i = 0; i < reply[0] && 1 + (i + 1) * 8 <= len; i++) {
			if (reply[1 + i * 8 + 1] != 0) clear = 0;
		}
		check(clear, "and no row is flagged OVERRIDDEN");
	}

	st = call(OP_CONFIG_RELOAD, NULL, 0, reply, sizeof(reply), &len);
	check_eq(st, ST_OK, "CONFIG_RELOAD answers OK");
	check_eq(config_selected_slot(), 2, "and reads config.txt alone");

	{
		u8 setauto[33];
		memset(setauto, 0, sizeof(setauto));
		memcpy(setauto, "usbonly", 7);
		setauto[32] = 1;
		st = call(OP_MOD_SET_AUTO, setauto, sizeof(setauto), reply, sizeof(reply), &len);
	}
	check_eq(st, ST_NOT_FOUND, "MOD_SET_AUTO on for a mod only a drive has is NOT_FOUND, as in 1.4.1");

	/* A rescan and a load, as the tick thread runs them for MOD_RESCAN and MOD_LOAD. */
	core_lock();
	mods_rescan();
	check_eq(mods_load("usbonly"), ST_NOT_FOUND, "MOD_LOAD of a mod only a drive has is NOT_FOUND");
	mods_unload("both");
	check_eq(mods_load("both"), ST_OK, "MOD_LOAD of an installed mod with a newer USB copy loads");
	core_unlock();
	check_eq(peek32(WORD_AT), 0x10000000u, "the console's own copy");

	st = call(OP_MOD_LIST, NULL, 0, reply, sizeof(reply), &len);
	check(st == ST_OK && mod_listed(reply, len, "both"), "MOD_LIST lists the installed mod");
	check(st == ST_OK && !mod_listed(reply, len, "usbonly") && !mod_listed(reply, len, "usbonly2"),
	      "and neither mod only a drive has");

	/* Build 56: the console menu's Mods page is a preview feature too. */
	{
		struct menu_view v;
		u8 i, mods = 0;

		core_lock();
		menu_model_reset(7, 24, MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST);
		menu_model_view(&v);
		for (i = 0; i < v.nrows; i++) {
			if (strcmp(v.rows[i].label, "Mods") == 0 || v.rows[i].id == MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_MODS))
				mods = 1;
		}
		check(!mods && v.nrows == 6 && strcmp(v.rows[4].label, "Unlocks") == 0 &&
		      strcmp(v.rows[5].label, "Save files") == 0,
		      "the console menu's root has no Mods row: Game .. Unlocks, Save files, six rows, as in 1.4.1");
		check_eq(menu_model_event(MBOX_EV_ACTIVATE, 5, MENU_ROW_ID(MENU_ROW_PAGE, MENU_PAGE_MODS), v.key), ST_BAD_ARG,
		         "an event for a Mods row is refused: no row of the root's is one");
		check_eq(menu_model_depth(), 1, "and the menu stays on its root");
		core_unlock();
	}

	host_quit();
	check(tick_until(SESSION_XMB, 2000), "back at the XMB");
	host_set_user(2);
	tick(360);
	check_eq(config_selected_slot(), 2, "another user signing in changes nothing");

	check_eq(host_watch_hits(0), 0, "no path under /dev_usb was used");
	check_eq(host_watch_hits(1), 0, "no path under /dev_hdd0/qwark/profiles was used");
	host_watch_paths(NULL, 0);

	check(!exists(HDD "/usbonly") && !exists("/dev_hdd0/qwark/usbtmp"), "nothing was copied off the drive");
	check(!exists(QWARK_PROFILEDIR "/user-00000002"), "no profile was made");
	{
		char text[256];
		u32 n = 0;
		check(qread_file(QWARK_CONFIG, text, sizeof(text) - 1, &n) == ST_OK &&
		      (text[n] = 0, strstr(text, "profiles.enabled = 1") != NULL),
		      "config.txt keeps profiles.enabled, for a test build to find again");
	}

	net_stop();
	if (g_sock >= 0) plat_socket_close(g_sock);
	g_sock = -1;
	check(plat_thread_join(accept_thread) == 0, "the accept thread joins");
	net_wait_clients(2000000u);
	net_shutdown();
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
	/* Build 57: config.txt on a console's HDD and after a hand edit, as a release reads and writes it. */
	test_config_files();

	session_shutdown();
	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
	plat_shutdown();
	return g_failures == 0 ? 0 : 1;
}
