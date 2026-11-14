/*
 * Revision 1.32: the telemetry rate on qwark's fake console.
 *
 *   the key      `telemetry.hz`, 30 out of the box; 30, 60 and 120 taken,
 *                anything else refused and nothing written; a hand edit to
 *                another value, or the key gone, read as 30;
 *   the file     written into config.txt and read back by CONFIG_RELOAD;
 *                console-wide, so with a user's profile active it lands in
 *                config.txt, makes no profile, and stays as users change;
 *   the cadence  packets a second of the fake clock, at the XMB and in a
 *                game, 30, 60 or 120 after the key, from the tick after the
 *                write;
 *   SessionInfo  flags3 bit2 and the rate in bits 3-4, beside the run-start
 *                warnings' bits.
 *
 * It puts back the console's config.txt it found. The op itself, over the
 * wire, is smoke.py's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/net.h"
#include "../src/core/util.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define TR_FILE_MAX 24576

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

static int slurp(const char *path, char *buf, u32 cap)
{
	plat_file_t f;
	u32 got = 0;

	buf[0] = 0;
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return -1;
	if (plat_file_read(f, buf, cap - 1, &got) != 0) got = 0;
	plat_file_close(f);
	buf[got] = 0;
	return (int)got;
}

static int spill(const char *path, const char *text)
{
	plat_file_t f;
	int rc;

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return -1;
	rc = plat_file_write(f, text, qstrlen(text));
	plat_file_close(f);
	return rc;
}

static int file_has(const char *path, const char *text)
{
	static char buf[TR_FILE_MAX];
	if (slurp(path, buf, sizeof(buf)) < 0) return 0;
	return strstr(buf, text) != NULL;
}

/* The packets the tick hands the network over one second of the fake clock. */
static u32 per_second(void)
{
	u32 before = net_telemetry_sends();
	tick(120);
	return net_telemetry_sends() - before;
}

static u8 flags3(void)
{
	u8 info[SESSION_INFO_SIZE];
	session_info_copy(info, sizeof(info));
	return info[31];
}

static u8 packet_flags3(void)
{
	u8 packet[TELEMETRY_MAX];
	if (session_telemetry_copy(packet, sizeof(packet)) < 4 + SESSION_INFO_SIZE) return 0xFF;
	return packet[4 + 31];
}

static u8 rate_bits(u8 code)
{
	return (u8)(SESSION_FLAG3_TELEMETRY_RATE | (code << SESSION_FLAG3_RATE_SHIFT));
}

/* Sets the rate the way TELEMETRY_RATE does, and lets the tick see it. */
static int set_hz(u8 hz)
{
	int rc = config_set_telemetry_hz(hz);
	tick(1);
	return rc;
}

static void cadence(const char *where)
{
	char what[96];

	snprintf(what, sizeof(what), "%s: 30 Hz out of the box, 30 packets a second", where);
	check_eq_u64(per_second(), 30, what);

	check(set_hz(60) == ST_OK, "telemetry.hz = 60");
	snprintf(what, sizeof(what), "%s: 60 packets a second", where);
	check_eq_u64(per_second(), 60, what);
	check_eq_u64(flags3() & (SESSION_FLAG3_TELEMETRY_RATE | SESSION_FLAG3_RATE_MASK),
	             rate_bits(TELEMETRY_RATE_CODE_60), "flags3 says 60 Hz (bits 3-4 = 1)");

	check(set_hz(120) == ST_OK, "telemetry.hz = 120");
	snprintf(what, sizeof(what), "%s: 120 packets a second, one every tick", where);
	check_eq_u64(per_second(), 120, what);
	check_eq_u64(packet_flags3() & (SESSION_FLAG3_TELEMETRY_RATE | SESSION_FLAG3_RATE_MASK),
	             rate_bits(TELEMETRY_RATE_CODE_120), "the packet's own flags3 says 120 Hz (bits 3-4 = 2)");

	check(set_hz(30) == ST_OK, "telemetry.hz = 30");
	snprintf(what, sizeof(what), "%s: back to 30 packets a second", where);
	check_eq_u64(per_second(), 30, what);
	check_eq_u64(flags3() & (SESSION_FLAG3_TELEMETRY_RATE | SESSION_FLAG3_RATE_MASK),
	             rate_bits(TELEMETRY_RATE_CODE_30), "flags3 says 30 Hz (bits 3-4 = 0)");
}

void test_telrate(void)
{
	static char before_cfg[TR_FILE_MAX];
	int had_cfg;

	had_cfg = slurp(QWARK_CONFIG, before_cfg, sizeof(before_cfg)) >= 0;
	if (session_state() != SESSION_XMB) quit();

	group("telemetry rate: the key");
	check(!file_has(QWARK_CONFIG, "telemetry.hz"), "the console's config.txt does not name telemetry.hz yet");
	check_eq_u64(config_telemetry_hz(), 30, "so it reads as 30 Hz");
	tick(1);
	check_eq_u64(session_telemetry_hz(), 30, "and the tick sends at 30 Hz");
	check_eq_u64(flags3(), SESSION_FLAG3_RUN_WARN | rate_bits(TELEMETRY_RATE_CODE_30),
	             "flags3: the run-start warnings on, TELEMETRY_RATE, 30 Hz");

	check(config_set_telemetry_hz(0) == ST_BAD_ARG, "0 Hz is refused");
	check(config_set_telemetry_hz(40) == ST_BAD_ARG, "so is 40");
	check(config_set_telemetry_hz(240) == ST_BAD_ARG, "and 240");
	check(config_set_telemetry_hz(4) == ST_BAD_ARG, "and a tick count, 4");
	check(!file_has(QWARK_CONFIG, "telemetry.hz"), "and a refusal writes nothing");
	check_eq_u64(config_telemetry_hz(), 30, "the rate is still 30");

	group("telemetry rate: the cadence at the XMB");
	check(session_state() == SESSION_XMB, "at the XMB");
	cadence("at the XMB");

	group("telemetry rate: the cadence in a game");
	check(boot("NPEA00385"), "Ratchet & Clank boots");
	cadence("in a game");
	check(set_hz(120) == ST_OK, "120 Hz again");
	check(quit(), "and a quit");
	check_eq_u64(per_second(), 120, "the rate outlives the game: 120 a second back at the XMB");

	group("telemetry rate: the file");
	check(file_has(QWARK_CONFIG, "telemetry.hz = 120"), "config.txt says telemetry.hz = 120");
	check(spill(QWARK_CONFIG, "telemetry.hz = 60\n") >= 0, "a hand edit to 60");
	check(config_load() == ST_OK, "CONFIG_RELOAD");
	tick(1);
	check_eq_u64(per_second(), 60, "is picked up: 60 a second");
	check(spill(QWARK_CONFIG, "telemetry.hz = 45\n") >= 0, "a hand edit to 45, which is no rate");
	check(config_load() == ST_OK, "CONFIG_RELOAD");
	tick(1);
	check_eq_u64(config_telemetry_hz(), 30, "reads as 30");
	check_eq_u64(per_second(), 30, "and sends 30 a second");
	check(spill(QWARK_CONFIG, "telemetry.hz = 120\n") >= 0, "120 by hand");
	check(config_load() == ST_OK, "CONFIG_RELOAD");
	tick(1);
	check(spill(QWARK_CONFIG, "log = 1\n") >= 0, "then a file without the key");
	check(config_load() == ST_OK, "CONFIG_RELOAD");
	tick(1);
	check_eq_u64(per_second(), 30, "is 30 a second again");

#if QWARK_PREVIEW
	group("telemetry rate: console-wide");
	{
		int changed = 0;
		char path[160];

		host_set_has_users(1);
		host_set_user(1);
		check(config_profiles_enable(1, PROFILE_SETUP_FRESH, 1, &changed) == ST_OK && config_profile_active(),
		      "per-user settings on, user 1's profile active");
		check(set_hz(60) == ST_OK, "telemetry.hz = 60 with user 1 signed in");
		snprintf(path, sizeof(path), "%s/user-00000001/config.txt", QWARK_PROFILEDIR);
		check(file_has(QWARK_CONFIG, "telemetry.hz = 60"), "lands in the console's config.txt");
		check(!file_has(path, "telemetry.hz"), "and not in user 1's");

		host_set_user(2);
		tick(130);
		check(config_profile_user() == 2, "user 2 signs in at the XMB");
		check(!config_profile_exists(2), "nothing was made for user 2");
		check_eq_u64(per_second(), 60, "and the rate is the console's still: 60 a second");

		check(config_profiles_enable(0, 0, 2, &changed) == ST_OK && !config_profile_active(),
		      "per-user settings off");
		check_eq_u64(config_telemetry_hz(), 60, "the rate stays 60");

		/* User 1's empty profile goes again, as test_profiles.c takes its own away. */
		{
			static const char * const leaves[] = {
				"positions/rac1.txt", "positions/rac2.txt", "positions/rac3.txt", "positions/rac4.txt",
				"config.txt", "config.txt.new",
			};
			u32 i;
			for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
				snprintf(path, sizeof(path), "%s/user-00000001/%s", QWARK_PROFILEDIR, leaves[i]);
				plat_file_unlink(path);
			}
			snprintf(path, sizeof(path), "%s/user-00000001/positions", QWARK_PROFILEDIR);
			plat_dir_remove(path);
			snprintf(path, sizeof(path), "%s/user-00000001", QWARK_PROFILEDIR);
			plat_dir_remove(path);
		}
		host_set_user(1);
	}
#endif

	if (had_cfg) spill(QWARK_CONFIG, before_cfg); else plat_file_unlink(QWARK_CONFIG);
	config_load();
	tick(1);
	check_eq_u64(session_telemetry_hz(), 30, "the config.txt found is back, and with it 30 Hz");
}
