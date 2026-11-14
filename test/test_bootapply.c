/*
 * Revision 1.33: apply toggles on game boot, `boot.apply` (BOOT_APPLY), on
 * qwark's fake console. RaC1 boots with Ghost Ratchet and a mod flagged on
 * boot, under each mode:
 *
 *   Always   both applied as the game arrives, as every earlier build did;
 *   Never    neither applied, both flags kept (SessionInfo's toggle_auto and
 *            mod_auto, config.txt's keys), and no question;
 *   Ask      with no client connected, Never; with one, neither applied and
 *            flags3 bit7 BOOT_ASK up until BOOT_ASK answers: 1 applies both,
 *            0 leaves them off, a second answer is NOT_FOUND, and a quit
 *            takes the question away.
 *
 * The key's default, its refusals and a hand edit, and flags3 bit5. It puts
 * the flags and the mode back as it found them. The ops over the wire are
 * smoke.py's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/mods.h"
#include "../src/core/util.h"
#include "../src/games/rac1.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define BA_TITLE "NPEA00385"
#define BA_MOD   "ba-onboot"
#define BA_DIR   "/dev_hdd0/qwark/mods/" BA_TITLE "/" BA_MOD

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

	if (session_state() == SESSION_XMB) return 1;
	host_quit();
	for (i = 0; i < 2000; i++) {
		tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

static u8 info_byte(u32 at)
{
	u8 info[SESSION_INFO_SIZE];
	tick(16);   /* the block is published every few ticks */
	session_info_copy(info, sizeof(info));
	return info[at];
}

static int ghost_on(void)   { return (features_toggle_state() & ((u64)1 << F_GHOST)) != 0; }
static int ghost_auto(void) { return (features_toggle_auto() & ((u64)1 << F_GHOST)) != 0; }

static int mod_loaded(void)
{
	int i = mods_find(BA_MOD);
	return i >= 0 && (mods_loaded_mask() & (1u << i)) != 0;
}

static int mod_auto(void)
{
	int i = mods_find(BA_MOD);
	return i >= 0 && (mods_auto_mask() & (1u << i)) != 0;
}

/* A boot under `mode`, with or without a client connected. */
static int boot_under(u8 mode, int client)
{
	if (!quit()) return 0;
	if (config_set_boot_apply(mode) != ST_OK) return 0;
	session_test_client(client);
	return boot(BA_TITLE);
}

static void put_mod(int make)
{
	plat_file_t f;
	static const char text[] = "#- name: BA On Boot\n0x00880080: 0x60000000\n";

	if (!make) {
		plat_file_unlink(BA_DIR "/patch.txt");
		plat_dir_remove(BA_DIR);
		return;
	}
	if (plat_file_open(BA_DIR "/patch.txt", PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, (u32)strlen(text));
	plat_file_close(f);
}

static void test_key(void)
{
	group("boot apply: the key");

	check_eq_u64(config_boot_apply(), BOOT_APPLY_ALWAYS, "boot.apply reads as Always when the file does not name it");
	check(config_set_boot_apply(3) == ST_BAD_ARG && config_set_boot_apply(255) == ST_BAD_ARG,
	      "3 and 255 are refused");
	check_eq_u64(config_boot_apply(), BOOT_APPLY_ALWAYS, "and a refusal writes nothing");
	check(config_set_boot_apply(BOOT_APPLY_NEVER) == ST_OK && config_boot_apply() == BOOT_APPLY_NEVER, "2 is Never");
	check(config_set_boot_apply(BOOT_APPLY_ASK) == ST_OK && config_boot_apply() == BOOT_APPLY_ASK, "1 is Ask");
	check(config_set_u32("boot.apply", 9) == ST_OK && config_boot_apply() == BOOT_APPLY_ALWAYS,
	      "a 9 by hand reads as Always");
	check_eq_u64(config_key_scope("boot.apply"), CONFIG_SCOPE_USER, "a per-user key, as the auto flags are");
	config_set_boot_apply(BOOT_APPLY_ALWAYS);
	check((info_byte(31) & (SESSION_FLAG3_SETTINGS_133 | SESSION_FLAG3_BOOT_ASK)) == SESSION_FLAG3_SETTINGS_133,
	      "flags3 bit5: the module has it; bit7 clear, nothing asked");
}

void test_bootapply(void)
{
	test_key();

	group("boot apply: the flags");
	check(quit() && boot(BA_TITLE), "RaC1 boots");
	put_mod(1);
	check(mods_rescan() == ST_OK && mods_find(BA_MOD) >= 0, "a mod for RaC1");
	check(features_set_auto(F_GHOST, 1) == ST_OK && mods_set_auto(BA_MOD, 1) == ST_OK && ghost_auto() && mod_auto(),
	      "Ghost Ratchet and the mod flagged on boot");

	group("boot apply: Always");
	check(boot_under(BOOT_APPLY_ALWAYS, 0), "a boot under Always");
	check(ghost_on() && mod_loaded(), "Ghost Ratchet is on and the mod is in, as every build before did");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) == 0 && session_boot_ask(1) == ST_NOT_FOUND,
	      "no question: bit7 clear, BOOT_ASK NOT_FOUND");

	group("boot apply: Never");
	check(boot_under(BOOT_APPLY_NEVER, 1), "a boot under Never, a client connected");
	check(!ghost_on() && !mod_loaded(), "nothing applied");
	check(ghost_auto() && mod_auto() && config_feature_auto(BA_TITLE, "Ghost Ratchet") != 0 &&
	      config_mod_auto(BA_TITLE, BA_MOD), "the flags are kept, in the session and in config.txt");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) == 0, "and nothing is asked");
	check(session_boot_ask(1) == ST_NOT_FOUND && !ghost_on(), "BOOT_ASK finds no question and applies nothing");

	group("boot apply: Ask with no client");
	check(boot_under(BOOT_APPLY_ASK, 0), "a boot under Ask, no client connected");
	check(!ghost_on() && !mod_loaded(), "Never: nothing applied");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) == 0 && session_boot_ask(1) == ST_NOT_FOUND,
	      "and no question waits for a client that comes later");

	group("boot apply: Ask with a client");
	check(boot_under(BOOT_APPLY_ASK, 1), "a boot under Ask, a client connected");
	check(!ghost_on() && !mod_loaded(), "nothing applied yet");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) != 0, "flags3 bit7: the question waits");
	check(session_boot_ask(0) == ST_OK && !ghost_on() && !mod_loaded(), "BOOT_ASK 0: left off");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) == 0 && session_boot_ask(1) == ST_NOT_FOUND,
	      "the question is answered: bit7 clear, a second answer NOT_FOUND");
	check(ghost_auto() && mod_auto(), "and the flags are kept");

	check(boot_under(BOOT_APPLY_ASK, 1), "Ask again, a client connected");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) != 0, "bit7 up");
	check(session_boot_ask(1) == ST_OK && ghost_on() && mod_loaded(), "BOOT_ASK 1: Ghost Ratchet on and the mod in");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) == 0, "bit7 clear");

	check(boot_under(BOOT_APPLY_ASK, 1) && (info_byte(31) & SESSION_FLAG3_BOOT_ASK) != 0, "Ask once more");
	check(quit() && session_boot_ask(1) == ST_NOT_INGAME, "a quit: BOOT_ASK is NOT_INGAME at the XMB");
	check((info_byte(31) & SESSION_FLAG3_BOOT_ASK) == 0, "and the question went with the game");

	group("boot apply: put back");
	session_test_client(0);
	config_set_boot_apply(BOOT_APPLY_ALWAYS);
	check(boot(BA_TITLE), "RaC1 boots under Always");
	features_set_auto(F_GHOST, 0);
	mods_set_auto(BA_MOD, 0);
	features_set(F_GHOST, 0);
	mods_unload(BA_MOD);
	put_mod(0);
	mods_rescan();
	check(!ghost_auto() && mods_find(BA_MOD) < 0, "the flags and the mod are gone again");
	check(quit(), "quit");
}
