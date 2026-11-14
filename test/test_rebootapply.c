/*
 * Revision 1.33: Apply toggles on game reboot, `reboot.apply` (REBOOT_APPLY),
 * on qwark's fake console. RaC1 boots with Ghost Ratchet and a mod flagged on
 * boot, and before each reboot Infinite ammo, a second mod and a freeze are
 * turned on by hand, so the reboot has a previous-session record (section
 * 4.1). Under each mode:
 *
 *   every mode  Ghost Ratchet and the flagged mod are applied at the boot and
 *               are not in the record: `reboot.apply` never touches them;
 *   Always      the record is re-applied as the game arrives, with or without
 *               a client, and nothing is left pending;
 *   Ask         with a client connected the record waits (PREVIOUS_PENDING)
 *               with nothing re-applied, as every build before did; with none
 *               connected it is dropped, so a client that comes later is not
 *               asked;
 *   Never       the record is dropped, with a client connected too.
 *
 * And a boot that is not a reboot (another title in between) carries nothing
 * over in any mode. The key's default, its refusals, a hand edit and the old
 * test build's boot.apply. It puts the flags and the mode back as it found
 * them. The op over the wire is smoke.py's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/mods.h"
#include "../src/core/mem.h"
#include "../src/core/util.h"
#include "../src/games/rac1.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define RA_TITLE  "NPEA00385"
#define RA_OTHER  "NPEA00386"
#define RA_AUTO   "ra-onboot"
#define RA_HAND   "ra-byhand"
#define RA_ROOT   "/dev_hdd0/qwark/mods/" RA_TITLE "/"
#define RA_FREEZE 0x00700040u

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

static int on(u8 id) { return (features_toggle_state() & ((u64)1 << id)) != 0; }

static int loaded(const char *dir)
{
	int i = mods_find(dir);
	return i >= 0 && (mods_loaded_mask() & (1u << i)) != 0;
}

static int in_record(const char *dir)
{
	int i = mods_find(dir);
	return i >= 0 && (session_previous()->mods & (1u << i)) != 0;
}

static int frozen(void) { return freeze_find(RA_FREEZE, 4) >= 0; }

static int pending(void)
{
	return session_previous()->pending && (info_byte(24) & SESSION_FLAG_PREVIOUS_PENDING) != 0;
}

static int nothing_pending(void)
{
	return !session_previous()->pending && (info_byte(24) & SESSION_FLAG_PREVIOUS_PENDING) == 0;
}

/* What the user had on by hand before the reboot: what the record is made of. */
static int by_hand(void)
{
	return features_set(F_INFINITE_AMMO, 1) == ST_OK && mods_load(RA_HAND) == ST_OK &&
	       (frozen() || freeze_add(RA_FREEZE, 4, 9, NULL) == ST_OK);
}

/* A same-game reboot under `mode`, with or without a client connected. */
static int reboot_under(u8 mode, int client)
{
	if (!by_hand()) return 0;
	if (!quit()) return 0;
	if (config_set_reboot_apply(mode) != ST_OK) return 0;
	session_test_client(client);
	return boot(RA_TITLE);
}

static void put_mod(const char *dir, int make)
{
	char path[96];
	char text[64];
	plat_file_t f;

	snprintf(path, sizeof(path), RA_ROOT "%s/patch.txt", dir);
	if (!make) {
		plat_file_unlink(path);
		snprintf(path, sizeof(path), RA_ROOT "%s", dir);
		plat_dir_remove(path);
		return;
	}
	/* Each mod its own word, so the two never overlap. */
	snprintf(text, sizeof(text), "#- name: %s\n0x%08X: 0x60000000\n", dir,
	         strcmp(dir, RA_AUTO) == 0 ? 0x00880080u : 0x00880090u);
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, (u32)strlen(text));
	plat_file_close(f);
}

/* Every mode: the on-boot flags were applied at the boot, and the record never held them. */
static void check_on_boot(const char *mode)
{
	char what[112];

	snprintf(what, sizeof(what), "%s: Ghost Ratchet and the flagged mod applied at the boot", mode);
	check(on(F_GHOST) && loaded(RA_AUTO), what);
	snprintf(what, sizeof(what), "%s: and neither is in the record", mode);
	check((session_previous()->toggles & ((u64)1 << F_GHOST)) == 0 && !in_record(RA_AUTO), what);
}

static void test_key(void)
{
	group("reboot apply: the key");

	check_eq_u64(config_reboot_apply(), REBOOT_APPLY_ASK, "reboot.apply reads as Ask when the file does not name it");
	check(config_set_reboot_apply(3) == ST_BAD_ARG && config_set_reboot_apply(255) == ST_BAD_ARG,
	      "3 and 255 are refused");
	check_eq_u64(config_reboot_apply(), REBOOT_APPLY_ASK, "and a refusal writes nothing");
	check(config_set_reboot_apply(REBOOT_APPLY_NEVER) == ST_OK && config_reboot_apply() == REBOOT_APPLY_NEVER, "2 is Never");
	check(config_set_reboot_apply(REBOOT_APPLY_ALWAYS) == ST_OK && config_reboot_apply() == REBOOT_APPLY_ALWAYS, "0 is Always");
	check(config_set_u32("reboot.apply", 9) == ST_OK && config_reboot_apply() == REBOOT_APPLY_ASK,
	      "a 9 by hand reads as Ask");
	check(config_set_u32("boot.apply", REBOOT_APPLY_NEVER) == ST_OK && config_reboot_apply() == REBOOT_APPLY_ASK,
	      "the test build's boot.apply is not read");
	check_eq_u64(config_key_scope("reboot.apply"), CONFIG_SCOPE_USER, "a per-user key");
	config_set_reboot_apply(REBOOT_APPLY_ASK);
	check((info_byte(31) & (SESSION_FLAG3_SETTINGS_133 | 0x80)) == SESSION_FLAG3_SETTINGS_133,
	      "flags3 bit5: the module has it; bit7 is 0");
}

void test_rebootapply(void)
{
	test_key();

	group("reboot apply: the flags");
	check(quit() && boot(RA_TITLE), "RaC1 boots");
	put_mod(RA_AUTO, 1);
	put_mod(RA_HAND, 1);
	check(mods_rescan() == ST_OK && mods_find(RA_AUTO) >= 0 && mods_find(RA_HAND) >= 0, "two mods for RaC1");
	check(features_set_auto(F_GHOST, 1) == ST_OK && mods_set_auto(RA_AUTO, 1) == ST_OK,
	      "Ghost Ratchet and one mod flagged on boot");

	group("reboot apply: Ask with a client");
	check(reboot_under(REBOOT_APPLY_ASK, 1), "a reboot under Ask, a client connected");
	check_on_boot("Ask");
	check(!on(F_INFINITE_AMMO) && !loaded(RA_HAND) && !frozen(), "nothing of the last session re-applied");
	check(pending(), "the record waits: PREVIOUS_PENDING");
	check((session_previous()->toggles & ((u64)1 << F_INFINITE_AMMO)) != 0 && in_record(RA_HAND) &&
	      session_previous()->nfreeze == 1, "with Infinite ammo, the mod and the freeze in it");
	session_previous_reapply(PREV_TOGGLES | PREV_MODS | PREV_FREEZES);
	check(on(F_INFINITE_AMMO) && loaded(RA_HAND) && frozen() && nothing_pending(),
	      "and the client's Reapply puts them back");

	group("reboot apply: Ask with no client");
	check(reboot_under(REBOOT_APPLY_ASK, 0), "a reboot under Ask, no client connected");
	check_on_boot("Ask, no client");
	check(!on(F_INFINITE_AMMO) && !loaded(RA_HAND) && !frozen(), "nothing of the last session re-applied");
	check(nothing_pending() && session_previous()->toggles == 0 && session_previous()->nfreeze == 0 &&
	      !in_record(RA_HAND), "and the record is dropped: nothing waits for a client that comes later");

	group("reboot apply: Always");
	check(reboot_under(REBOOT_APPLY_ALWAYS, 0), "a reboot under Always, no client connected");
	check_on_boot("Always");
	check(on(F_INFINITE_AMMO) && loaded(RA_HAND) && frozen(), "Infinite ammo, the mod and the freeze are back");
	check(nothing_pending(), "and nothing is pending");
	check(reboot_under(REBOOT_APPLY_ALWAYS, 1), "a reboot under Always, a client connected");
	check(on(F_INFINITE_AMMO) && loaded(RA_HAND) && frozen() && nothing_pending(),
	      "re-applied the same, and nothing to ask");

	group("reboot apply: Never");
	check(reboot_under(REBOOT_APPLY_NEVER, 1), "a reboot under Never, a client connected");
	check_on_boot("Never");
	check(!on(F_INFINITE_AMMO) && !loaded(RA_HAND) && !frozen(), "nothing of the last session re-applied");
	check(nothing_pending(), "and nothing is offered");
	check((features_toggle_auto() & ((u64)1 << F_GHOST)) != 0 && config_feature_auto(RA_TITLE, "Ghost Ratchet") != 0 &&
	      config_mod_auto(RA_TITLE, RA_AUTO), "the on-boot flags are kept, in the session and in config.txt");

	group("reboot apply: not a reboot");
	check(by_hand() && quit() && boot(RA_OTHER) && quit(), "Infinite ammo on, then RaC2 in between");
	check(config_set_reboot_apply(REBOOT_APPLY_ALWAYS) == ST_OK && boot(RA_TITLE), "RaC1 again under Always");
	check_on_boot("Always, after another game");
	check(!on(F_INFINITE_AMMO) && !loaded(RA_HAND) && !frozen() && nothing_pending(),
	      "a fresh boot of the game: nothing carried over, nothing pending");

	group("reboot apply: put back");
	session_test_client(1);
	config_set_reboot_apply(REBOOT_APPLY_ASK);
	features_set_auto(F_GHOST, 0);
	mods_set_auto(RA_AUTO, 0);
	features_set(F_GHOST, 0);
	features_set(F_INFINITE_AMMO, 0);
	mods_unload(RA_AUTO);
	mods_unload(RA_HAND);
	freeze_clear();
	session_previous_dismiss();
	put_mod(RA_AUTO, 0);
	put_mod(RA_HAND, 0);
	mods_rescan();
	check((features_toggle_auto() & ((u64)1 << F_GHOST)) == 0 && mods_find(RA_AUTO) < 0 && mods_find(RA_HAND) < 0,
	      "the flags and the mods are gone again");
	check(quit(), "quit");
}
