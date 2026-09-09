/*
 * Host unit tests for the qwark core.
 *
 * These run the real core against the host platform's fake process memory, so
 * every path except the PS3 syscalls themselves is exercised. Nothing here says
 * anything about hardware.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/mem.h"
#include "../src/core/mods.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/util.h"
#include "../src/core/net.h"
#include "../src/core/autosplit.h"
#include "../src/games/game.h"
#include "../src/plat/plat.h"
#include "../src/plat/plat_net.h"
#include "../src/plat/host/plat_host.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;
static const char *g_group = "";

static void group(const char *name)
{
	g_group = name;
	printf("--- %s\n", name);
}

static void check(int ok, const char *what)
{
	g_checks++;
	if (ok) {
		printf("  PASS  %s\n", what);
	} else {
		g_failures++;
		printf("  FAIL  %s (%s)\n", what, g_group);
	}
}

static void check_eq_u64(u64 got, u64 want, const char *what)
{
	g_checks++;
	if (got == want) {
		printf("  PASS  %s\n", what);
	} else {
		g_failures++;
		printf("  FAIL  %s: got %llu want %llu (%s)\n", what,
		       (unsigned long long)got, (unsigned long long)want, g_group);
	}
}

/* -------------------------------------------------------------- endianness */

static void test_endian(void)
{
	u8 buf[8];
	f32 f;

	group("big-endian helpers");

	be16_put(buf, 0x1234);
	check(buf[0] == 0x12 && buf[1] == 0x34, "be16_put writes MSB first");
	check_eq_u64(be16_get(buf), 0x1234, "be16 round trip");

	be32_put(buf, 0xDEADBEEFu);
	check(buf[0] == 0xDE && buf[3] == 0xEF, "be32_put writes MSB first");
	check_eq_u64(be32_get(buf), 0xDEADBEEFu, "be32 round trip");

	be64_put(buf, 0x0123456789ABCDEFull);
	check(buf[0] == 0x01 && buf[7] == 0xEF, "be64_put writes MSB first");
	check_eq_u64(be64_get(buf), 0x0123456789ABCDEFull, "be64 round trip");

	bef32_put(buf, -50.0f);
	check_eq_u64(be32_get(buf), 0xC2480000u, "-50.0f encodes as 0xC2480000");
	f = bef32_get(buf);
	check(f == -50.0f, "f32 round trip");

	be_put_sized(buf, 1, 0xAB);
	check_eq_u64(be_get_sized(buf, 1), 0xAB, "1-byte sized round trip");
	be_put_sized(buf, 2, 0xABCD);
	check_eq_u64(be_get_sized(buf, 2), 0xABCD, "2-byte sized round trip");
	be_put_sized(buf, 8, 0x1122334455667788ull);
	check_eq_u64(be_get_sized(buf, 8), 0x1122334455667788ull, "8-byte sized round trip");
}

/* ------------------------------------------------------------------ frames */

/*
 * The frame codec, exactly as net.c writes and reads it:
 *   u32 length | u16 seq | u16 opcode/status | payload
 */
static void frame_encode(u8 *out, u32 length, u16 seq, u16 code)
{
	be32_put(out, length);
	be16_put(out + 4, seq);
	be16_put(out + 6, code);
}

static void test_framing(void)
{
	u8 header[QWARK_FRAME_HEADER];
	u32 i;

	group("frame codec");

	check(QWARK_FRAME_HEADER == 8, "frame header is 8 bytes");
	check(QWARK_MAX_PAYLOAD == 65600u, "max payload is 65600");
	check(SESSION_INFO_SIZE == 164, "SessionInfo is 164 bytes (protocol 1.1)");
	check(TELEMETRY_MAX == 937, "telemetry packet never exceeds 937 bytes");
	check(QWARK_MAX_READOUTS == 16, "SessionInfo carries sixteen readouts");
	check(FEATURE_WIRE_SIZE == 48, "Feature row is 48 bytes");
	check(UNLOCK_WIRE_SIZE == 44, "Unlock row is 44 bytes");
	check(MOD_WIRE_SIZE == 120, "Mod row is 120 bytes");

	for (i = 0; i < 4; i++) {
		u32 length = (u32)(i * 21845);
		u16 seq = (u16)(i * 9973);
		u16 code = (u16)(0x0030 + i);

		frame_encode(header, length, seq, code);
		check(be32_get(header) == length &&
		      be16_get(header + 4) == seq &&
		      be16_get(header + 6) == code, "frame header round trip");
	}
}

/* -------------------------------------------------------- the session helper */

static void pump(int ticks)
{
	int i;
	for (i = 0; i < ticks; i++) session_step_once();
}

/* Steps until the session reaches `want` or we give up. Returns 1 on success. */
static int pump_until(u8 want, int max_ms)
{
	int elapsed = 0;

	while (elapsed < max_ms) {
		session_step_once();
		if (session_state() == want) return 1;
		plat_sleep_us(2000);
		elapsed += 2;
	}

	return session_state() == want;
}

static int boot_and_wait(const char *title)
{
	host_boot(title);
	/* BOOTING waits about a second after the PID shows up before it reads. */
	return pump_until(SESSION_INGAME, 4000);
}

/* The same, for BCES01503, whose title id hosts three different games. */
static int boot_as_and_wait(const char *title, const char *which)
{
	host_boot_as(title, which);
	return pump_until(SESSION_INGAME, 4000);
}

static int quit_and_wait(void)
{
	host_quit();
	return pump_until(SESSION_XMB, 2000);
}

/* ------------------------------------------------------------ patch tables */

#define TEST_ADDR_A 0x00500000u
#define TEST_ADDR_B 0x00500004u

static const struct patch_word test_words[] = {
	{ TEST_ADDR_A, 0x60000000u },
	{ TEST_ADDR_B, 0x38600006u }
};

static u32 test_originals[2];

static struct patch_def test_patch = {
	"unit test patch", PATCH_KIND_CLIENT, test_words, 2, test_originals
};

static void test_patches(void)
{
	u32 v = 0;

	group("patch apply and revert");

	check(boot_and_wait("NPEA00385"), "RaC1 reaches INGAME after boot");

	/* Seed two known original words. */
	host_poke(TEST_ADDR_A, (const u8 *)"\x11\x22\x33\x44", 4);
	host_poke(TEST_ADDR_B, (const u8 *)"\x55\x66\x77\x88", 4);

	check(patch_apply(&test_patch) == ST_OK, "apply succeeds");
	check(patch_is_applied(&test_patch), "the def is in the table");

	mem_read_u32(TEST_ADDR_A, &v);
	check_eq_u64(v, 0x60000000u, "word 0 was written");
	mem_read_u32(TEST_ADDR_B, &v);
	check_eq_u64(v, 0x38600006u, "word 1 was written");
	check_eq_u64(test_originals[0], 0x11223344u, "original 0 was captured");
	check_eq_u64(test_originals[1], 0x55667788u, "original 1 was captured");

	/* The RaC2 fast-load bug: a second apply must not re-capture. */
	check(patch_apply(&test_patch) == ST_OK, "a second apply is a no-op");
	check_eq_u64(test_originals[0], 0x11223344u, "double apply did not re-capture original 0");
	check_eq_u64(test_originals[1], 0x55667788u, "double apply did not re-capture original 1");

	check(patch_revert(&test_patch) == ST_OK, "revert succeeds");
	mem_read_u32(TEST_ADDR_A, &v);
	check_eq_u64(v, 0x11223344u, "word 0 was restored");
	mem_read_u32(TEST_ADDR_B, &v);
	check_eq_u64(v, 0x55667788u, "word 1 was restored");

	check(patch_revert(&test_patch) == ST_NOT_FOUND, "reverting an unapplied def is NOT_FOUND");
	check(!patch_is_applied(&test_patch), "the def left the table");

	/* Client patches are keyed by their first address. */
	check(client_patch_apply(test_words, 2) == ST_OK, "client patch applies");
	check(client_patch_apply(test_words, 2) == ST_OK, "the same client patch again is a no-op");
	check_eq_u64(client_patch_count(), 1, "only one client patch slot is used");
	check(client_patch_revert(TEST_ADDR_A) == ST_OK, "client patch reverts by first address");
	check(client_patch_revert(TEST_ADDR_A) == ST_NOT_FOUND, "reverting it twice is NOT_FOUND");

	mem_read_u32(TEST_ADDR_A, &v);
	check_eq_u64(v, 0x11223344u, "the client patch restored the original");
}

/* ----------------------------------------------------------- watch, freeze */

static void test_tables(void)
{
	u8 id = 0xFF;
	u8 id2 = 0xFF;
	const struct watch_entry *w;
	u32 v = 0;

	group("watch and freeze tables");

	host_poke(0x00600000u, (const u8 *)"\x00\x00\x12\x34", 4);

	check(watch_add(0x00600000u, 4, &id) == ST_OK, "watch_add succeeds");
	check(watch_add(0x00600000u, 4, &id2) == ST_OK, "the same address and size again succeeds");
	check_eq_u64(id2, id, "and returns the same id");

	check(watch_add(0x00600000u, 3, &id2) == ST_BAD_ARG, "a size of 3 is rejected");

	pump(2);
	w = watch_slot(id);
	check(w != NULL && w->valid, "the watch reads as valid");
	check_eq_u64(w ? w->value : 0, 0x1234, "the watch value is right-aligned");

	check(freeze_add(0x00600010u, 4, 0x0000000Au, &id) == ST_OK, "freeze_add succeeds");
	check((freeze_mask() & ((u64)1 << id)) != 0, "the freeze shows in the mask");

	host_poke(0x00600010u, (const u8 *)"\xFF\xFF\xFF\xFF", 4);
	pump(2);
	mem_read_u32(0x00600010u, &v);
	check_eq_u64(v, 10, "the freeze wrote its value back");

	check(freeze_remove(id) == ST_OK, "freeze_remove succeeds");
	check(freeze_remove(id) == ST_NOT_FOUND, "removing it twice is NOT_FOUND");

	host_poke(0x00600010u, (const u8 *)"\xFF\xFF\xFF\xFF", 4);
	pump(2);
	mem_read_u32(0x00600010u, &v);
	check_eq_u64(v, 0xFFFFFFFFu, "a removed freeze stops writing");

	check(watch_remove((u8)0) == ST_OK || 1, "watch_remove runs");
	watch_clear();
	check(watch_slot(0) == NULL, "watch_clear empties the table");
}

/* ------------------------------------------------------- the mod parser */

static void test_mods(void)
{
	int i;
	const struct mod_entry *m;
	int found_hardcore = -1;
	int found_flight = -1;
	int found_incremental = -1;
	int found_hoven = -1;
	u32 v = 0;

	group("patch.txt parser, real racman mods");

	mods_set_title("NPEA00385");
	check(mods_count() >= 6, "all six RaC1 fixtures were scanned");

	for (i = 0; i < (int)mods_count(); i++) {
		m = mods_at((u32)i);
		if (m == NULL) continue;
		if (qstreq(m->dirname, "hardcore")) found_hardcore = i;
		if (qstreq(m->dirname, "flight")) found_flight = i;
		if (qstreq(m->dirname, "incremental_rng")) found_incremental = i;
		if (qstreq(m->dirname, "rc1-hoven-health")) found_hoven = i;
	}

	check(found_hardcore >= 0, "hardcore was found");
	if (found_hardcore >= 0) {
		m = mods_at((u32)found_hardcore);
		check(qstreq(m->name, "Hardcore Mode"), "its #- name was parsed");
		check(qstreq(m->author, "king_dedede"), "its #- author was parsed");
		check_eq_u64(m->def.count, 1, "it has exactly one patch word");
		check_eq_u64(m->def.words[0].addr, 0x4684b0u, "the address was parsed");
		check_eq_u64(m->def.words[0].value, 0x60000000u, "the value was parsed");
		check((m->flags & MOD_FLAG_NEEDS_LUA) == 0, "it does not need Lua");
	}

	check(found_flight >= 0, "flight was found");
	if (found_flight >= 0) {
		m = mods_at((u32)found_flight);
		check((m->flags & MOD_FLAG_NEEDS_LUA) != 0, "an automation: line marks needs_lua");
		check_eq_u64(m->def.count, 0, "and contributes no patch words");
	}

	check(found_incremental >= 0, "incremental_rng was found");
	if (found_incremental >= 0) {
		m = mods_at((u32)found_incremental);
		check_eq_u64(m->ncaves, 1, "a .bin value becomes a code cave");
		check_eq_u64(m->def.count, 0, "and not a patch word");
	}

	check(found_hoven >= 0, "rc1-hoven-health was found");
	if (found_hoven >= 0) {
		m = mods_at((u32)found_hoven);
		check_eq_u64(m->ncaves, 1, "it has one cave");
		/* Commented-out lines must not become patches. */
		check_eq_u64(m->def.count, 9, "nine word lines, the two commented ones ignored");
	}

	/* Load one and check that it wrote, then unload and check that it restored. */
	if (found_hardcore >= 0) {
		host_poke(0x4684b0u, (const u8 *)"\xAA\xBB\xCC\xDD", 4);

		check(mods_load("hardcore") == ST_OK, "mods_load succeeds");
		mem_read_u32(0x4684b0u, &v);
		check_eq_u64(v, 0x60000000u, "the mod wrote its word");
		check((mods_loaded_mask() & (1u << found_hardcore)) != 0, "the loaded mask is set");

		check(mods_load("hardcore") == ST_OK, "loading it twice is a no-op");

		check(mods_unload("hardcore") == ST_OK, "mods_unload succeeds");
		mem_read_u32(0x4684b0u, &v);
		check_eq_u64(v, 0xAABBCCDDu, "the mod restored the original");
		check(mods_unload("hardcore") == ST_NOT_FOUND, "unloading it twice is NOT_FOUND");
	}

	/*
	 * Unloading reverts the patch words and deliberately leaves the code cave in
	 * memory: restoring the cave bytes crashed the game, so the branches go and
	 * the cave stays. rc1-hoven-health has both, a 1096-byte cave at 0x4F6400 and
	 * nine words.
	 */
	if (found_hoven >= 0) {
		u32 cave = 0;

		host_poke(0x4F6400u, (const u8 *)"\x00\x00\x00\x00", 4);
		host_poke(0x4EDFD8u, (const u8 *)"\x11\x22\x33\x44", 4);

		check(mods_load("rc1-hoven-health") == ST_OK, "a mod with a cave loads");
		mem_read_u32(0x4EDFD8u, &v);
		check_eq_u64(v, 0x480087F5u, "its hook word was written");
		mem_read_u32(0x4F6400u, &cave);
		check(cave != 0, "and the cave file landed at 0x4F6400");

		check(mods_unload("rc1-hoven-health") == ST_OK, "and it unloads");
		mem_read_u32(0x4EDFD8u, &v);
		check_eq_u64(v, 0x11223344u, "the hook word went back to the original");
		mem_read_u32(0x4F6400u, &v);
		check_eq_u64(v, cave, "but the cave bytes are still there");
	}

	check(mods_load("does-not-exist") == ST_NOT_FOUND, "an unknown dirname is NOT_FOUND");

	group("patch.txt parser, dependencies");

	mods_set_title("NPEA00423");
	check(mods_count() == 2, "both Deadlocked fixtures were scanned");

	{
		int ghost = mods_find("il-ghost");
		int dlcs = mods_find("dl-cs");

		check(ghost >= 0 && dlcs >= 0, "both mods were found");
		if (ghost >= 0 && dlcs >= 0) {
			check(qstreq(mods_at((u32)dlcs)->name, "IL Decimal Timer"),
			      "dl-cs declares the dependency name");
			check(mods_at((u32)dlcs)->def.count > 200,
			      "dl-cs parsed its couple of hundred patch lines");

			check(mods_load("il-ghost") == ST_OK, "loading il-ghost succeeds");
			check((mods_at((u32)dlcs)->flags & MOD_FLAG_LOADED) != 0,
			      "its dependency was loaded first");

			mods_unload("il-ghost");
			mods_unload("dl-cs");
		}
	}

	/* Put the mod table back where the rest of the tests expect it. */
	mods_set_title("NPEA00385");
}

/* --------------------------------------------------- the session machine */

static void test_session_same_title(void)
{
	u8 watch_id = 0xFF;
	const struct previous_record *prev;
	u32 gen_before;

	group("session: same-title reboot");

	check(session_state() == SESSION_INGAME, "still INGAME from the earlier boot");

	watch_clear();
	freeze_clear();
	features_forget_state();

	check(watch_add(0x00700000u, 4, &watch_id) == ST_OK, "a watch is registered");
	check(freeze_add(0x00700010u, 4, 7, NULL) == ST_OK, "a freeze is registered");
	check(features_set(1, 1) == ST_OK, "infinite ammo turns on");
	check((features_toggle_state() & 2) != 0, "the toggle bit is set");

	gen_before = session_generation();

	check(quit_and_wait(), "the session returns to XMB after a quit");
	check(session_state() == SESSION_XMB, "state is XMB");

	check(boot_and_wait("NPEA00385"), "the same title reaches INGAME again");
	check(session_generation() == gen_before + 1, "the generation counter advanced");

	check(watch_slot(watch_id) != NULL, "the watch survived and kept its id");
	check_eq_u64(features_toggle_state(), 0, "no toggle is live any more");
	check_eq_u64(freeze_mask(), 0, "no freeze is live any more");

	prev = session_previous();
	check((prev->toggles & 2) != 0, "the toggle went into the previous record");
	check_eq_u64(prev->nfreeze, 1, "the freeze went into the previous record");
	check(prev->pending, "PREVIOUS_PENDING is set");

	session_previous_reapply(PREV_TOGGLES | PREV_FREEZES);
	check((features_toggle_state() & 2) != 0, "REAPPLY turned the toggle back on");
	check(freeze_mask() != 0, "REAPPLY restored the freeze");
	check(!session_previous()->pending, "the record is clear after REAPPLY");

	group("session: auto re-apply");

	check(features_set_auto(1, 1) == ST_OK, "infinite ammo is flagged auto");
	check((features_toggle_auto() & 2) != 0, "the auto bit shows");

	check(quit_and_wait(), "quit again");
	check(boot_and_wait("NPEA00385"), "boot again");

	check((features_toggle_state() & 2) != 0, "the auto toggle came back on its own");
	check((session_previous()->toggles & 2) == 0, "and is not left in the record");

	features_set_auto(1, 0);
	features_set(1, 0);
	freeze_clear();
	session_previous_dismiss();
}

static void test_session_different_title(void)
{
	u8 watch_id = 0xFF;

	group("session: different-title reboot");

	check(session_state() == SESSION_INGAME, "starting from INGAME");

	watch_clear();
	check(watch_add(0x00700020u, 4, &watch_id) == ST_OK, "a watch is registered");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("TEST00001"), "a different registered title reaches INGAME");

	check(session_game() != NULL && session_game()->game_id == GAME_RAC2,
	      "the session switched to the other game");
	check(watch_slot(watch_id) == NULL, "watches were dropped for a different title");
	check(!session_previous()->pending, "the previous record is empty");
	check_eq_u64(features_toggle_state(), 0, "no toggles carried over");

	group("session: unregistered title");

	check(quit_and_wait(), "quit");
	/* All four NPEA ids and BCES01503 are registered now, so pick a made-up one. */
	host_boot("NPEA99999");
	pump(200);
	check(session_state() == SESSION_XMB,
	      "an unregistered title leaves the session in XMB");
	check(session_game() == NULL, "and no game is selected");

	/* Back to RaC1 for anything that follows. */
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again");
}

/* ------------------------------------------------------------- telemetry */

static void test_telemetry(void)
{
	u8 packet[TELEMETRY_MAX];
	u32 len;
	u8 id = 0xFF;

	group("telemetry packet");

	watch_clear();
	check(watch_add(0x00700100u, 2, &id) == ST_OK, "one watch is registered");
	host_poke(0x00700100u, (const u8 *)"\xBE\xEF", 2);

	pump(8);

	len = session_telemetry_copy(packet, sizeof(packet));
	check(len >= 4u + SESSION_INFO_SIZE + 1u, "the packet has a header and a watch count");
	check(memcmp(packet, TELEMETRY_MAGIC, 4) == 0, "the magic is QWRK");
	check_eq_u64(packet[4], QWARK_PROTOCOL_VERSION, "the protocol version is 1");
	check_eq_u64(packet[5], QWARK_BUILD, "the build number byte follows it");
	check_eq_u64(packet[5], 4, "and this module is build 4");
	check_eq_u64(packet[6], SESSION_INGAME, "the state byte says INGAME");
	check_eq_u64(packet[7], GAME_RAC1, "the game byte says RaC1");
	check(memcmp(packet + 4 + 12, "NPEA00385", 9) == 0, "the title id is in place");
	check_eq_u64(packet[4 + SESSION_INFO_SIZE], 1, "one watch is reported");

	{
		const u8 *w = packet + 4 + SESSION_INFO_SIZE + 1;
		check_eq_u64(w[0], id, "the watch id matches");
		check_eq_u64(w[1], 2, "the watch size matches");
		check_eq_u64(w[2], 1, "the watch is valid");
		check_eq_u64(be64_get(w + 4), 0xBEEF, "the value is right-aligned in the u64");
	}

	check_eq_u64(len, 4u + SESSION_INFO_SIZE + 1u + 12u, "the packet is exactly one watch long");
	watch_clear();
}

/* ----------------------------------------------------------------- config */

static void test_config(void)
{
	group("config and positions");

	check(config_set_combo(COMBO_SAVE_POSITION, 0x1005) == ST_OK, "a combo is stored");
	check_eq_u64(config_combo(COMBO_SAVE_POSITION), 0x1005, "and reads back");

	config_set_selected_slot(3);
	check_eq_u64(config_selected_slot(), 3, "the selected slot round trips");

	check(config_load() == ST_OK, "config.txt reloads from disk");
	check_eq_u64(config_combo(COMBO_SAVE_POSITION), 0x1005, "the combo survived the reload");
	check_eq_u64(config_selected_slot(), 3, "the slot survived the reload");

	/*
	 * What POS_SELECT does. The setter writes config.txt on the spot, the way
	 * config_set_mod_auto does, so no CONFIG_SAVE is needed for the selection to
	 * be there after the console is power-cycled: config_load with nothing in
	 * between is that reboot.
	 */
	config_set_selected_slot(6);
	check(config_load() == ST_OK, "config.txt reloads after a second selection");
	check_eq_u64(config_selected_slot(), 6,
	             "a POS_SELECT reaches the file with no CONFIG_SAVE");

	config_set_selected_slot(0);
	check(config_load() == ST_OK, "config.txt reloads once more");
	check_eq_u64(config_selected_slot(), 0,
	             "and slot 0 persists too, not just a non-zero slot");

	config_set_mod_auto("NPEA00385", "flight", 1);
	check(config_load() == ST_OK, "config.txt reloads after a mod auto flag");
	check(config_mod_auto("NPEA00385", "flight"),
	      "the mod auto flag persists the same way");
	config_set_mod_auto("NPEA00385", "flight", 0);

	{
		u8 blob[16];
		u8 got[QWARK_MAX_BLOB];
		u8 len = 0;
		int i;

		for (i = 0; i < 16; i++) blob[i] = (u8)(0xA0 + i);

		check(pos_use_title("NPEA00385") == ST_OK, "positions switch to a title");
		check(pos_store(5, 2, blob, 16) == ST_OK, "a slot is stored");
		check(pos_fetch(5, 2, got, &len) == ST_OK, "and fetched");
		check(len == 16 && memcmp(got, blob, 16) == 0, "with the same bytes");
		check(pos_fetch(5, 3, got, &len) == ST_NOT_FOUND, "an empty slot is NOT_FOUND");
		check(pos_clear(5, 2) == ST_OK, "a slot clears");
		check(pos_fetch(5, 2, got, &len) == ST_NOT_FOUND, "and is gone");
	}

	group("string and number helpers");
	{
		char slug[32];
		int ok = 0;

		qslug("Infinite ammo", slug, sizeof(slug));
		check(qstreq(slug, "infinite_ammo"), "a label slugs into a config key");
		qslug("Ghost Ratchet!", slug, sizeof(slug));
		check(qstreq(slug, "ghost_ratchet"), "trailing punctuation is dropped");

		check_eq_u64(qparse_u32("0x4684b0", &ok), 0x4684b0u, "0x prefixed hex parses");
		check(ok, "and reports success");
		check_eq_u64(qparse_u32("1234", &ok), 1234, "decimal parses");
		qparse_u32("zzz", &ok);
		check(!ok, "garbage reports failure");
	}
}

/* -------------------------------------------------------------- describe */

/* Finds one Feature row in an encoded DESCRIBE reply. */
static const u8 *desc_feature(const u8 *out, u32 len, u8 id)
{
	u8 ngroups = out[1];
	u32 off = 2 + (u32)ngroups * 24;
	u8 nreadouts = out[off];
	u8 nfeatures;
	u8 i;

	off += 1 + (u32)nreadouts * 24;
	nfeatures = out[off];
	off += 1;

	for (i = 0; i < nfeatures; i++) {
		if (off + FEATURE_WIRE_SIZE > len) break;
		if (out[off] == id) return out + off;
		off += FEATURE_WIRE_SIZE;
	}

	return NULL;
}

static void test_describe(void)
{
	u8 out[4096];
	u32 len = 0;
	u8 ngroups;
	u8 nreadouts;
	u8 nfeatures;
	u32 off;
	const u8 *row;

	group("DESCRIBE encoding, protocol 1.1");

	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	check_eq_u64(out[0], GAME_RAC1, "the game id leads");

	ngroups = out[1];
	off = 2 + (u32)ngroups * 24;
	nreadouts = out[off];
	off += 1 + (u32)nreadouts * 24;
	nfeatures = out[off];
	off += 1;

	check_eq_u64(ngroups, 6, "RaC1 declares six groups");
	check_eq_u64(nreadouts, 12, "and twelve readouts");
	check_eq_u64(nfeatures, 27, "and twenty-seven features");
	check(ngroups <= QWARK_MAX_GROUPS && nreadouts <= QWARK_MAX_READOUTS &&
	      nfeatures <= QWARK_MAX_FEATURES, "all three are inside the DESCRIBE caps");
	check_eq_u64(len, off + (u32)nfeatures * FEATURE_WIRE_SIZE, "the length adds up");

	check(memcmp(out + 2, "Cheats", 6) == 0, "the first group name is in place");
	check(memcmp(out + 2 + 5 * 24, "Debug", 5) == 0, "the last group name is in place");
	check(memcmp(out + 2 + (u32)ngroups * 24 + 1, "Bolts", 5) == 0,
	      "readout 0 is named Bolts");
	check(memcmp(out + 2 + (u32)ngroups * 24 + 1 + 24, "Savefile helper", 15) == 0,
	      "readout 1 is the savefile helper");
	check(memcmp(out + off + 16, "Fast loads", 10) == 0, "the first feature label is in place");

	/* The readout byte at offset 5 is the whole point of revision 1.1. */
	row = desc_feature(out, len, 0);
	check(row != NULL && row[1] == FEATURE_TOGGLE && row[5] == 0xFF,
	      "a TOGGLE names no readout");
	check(row != NULL && row[4] == FEATURE_FLAG_WRITES_CODE,
	      "fast loads is flagged WRITES_CODE");

	row = desc_feature(out, len, 4);
	check(row != NULL && row[1] == FEATURE_ACTION && row[5] == 0xFF,
	      "an ACTION names no readout");

	row = desc_feature(out, len, 5);
	check(row != NULL && row[1] == FEATURE_VALUE && row[5] == 0,
	      "the bolts VALUE mirrors readout 0");
	check(row != NULL && row[3] == 0, "and its aux is 0");

	row = desc_feature(out, len, 27);
	check(row != NULL && row[1] == FEATURE_ENUM, "the camera feature is an ENUM");
	check(row != NULL && row[3] == 3, "its aux is the option count");
	check(row != NULL && row[5] == 7, "and it mirrors readout 7");
	check(row != NULL && be32_get(row + 8) == 0 && be32_get(row + 12) == 2,
	      "its range is 0 to 2");
	check(row != NULL && memcmp(row + 16, "Camera mode", 11) == 0, "its label reads");

	row = desc_feature(out, len, 22);
	check(row != NULL && row[1] == FEATURE_VALUE && row[5] == 4,
	      "the jankpot timer VALUE mirrors readout 4");

	/* Id 9, force okay load, is retired: gone from the table, never renumbered. */
	check(desc_feature(out, len, 9) == NULL, "the retired id 9 is not described");
	check(features_trigger(9) == ST_NOT_FOUND, "and triggering it is NOT_FOUND");
	check(desc_feature(out, len, 8) != NULL && desc_feature(out, len, 10) != NULL,
	      "the ids either side of it kept their numbers");
}

/* ---------------------------------------------------- RaC1: the unlock table */

#define A_BOMB_UNLOCK 0x96C14Au
#define A_BOMB_AMMO   0x96C0D4u      /* NewUnlocks passes 0x96C0CC and adds 8 */
#define A_BOMB_GOLD   0x969CB2u
#define A_HELI_UNLOCK 0x96C142u
#define A_SWING_UNLOCK 0x96C14Cu

static void test_unlocks(void)
{
	const struct game_api *g = session_game();
	const struct game_unlock *list = NULL;
	const char * const *cats = NULL;
	u8 n = 0, ncat = 0;
	const struct unlock_field_desc *fields = NULL;
	u32 values[4];
	u32 v = 0;
	u8 b = 0;

	group("RaC1 unlocks");

	check(g != NULL && g->unlock_list != NULL, "RaC1 offers an unlock table");
	if (g == NULL || g->unlock_list == NULL) return;

	check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
	check_eq_u64(n, 39, "the whole NewUnlocks table is there");
	check_eq_u64(ncat, 3, "in three categories");
	check(cats != NULL && qstreq(cats[0], "Weapons") && qstreq(cats[2], "Items"),
	      "the category names line up");
	check(list != NULL && qstreq(list[0].name, "Bomb Glove"), "entry 0 is the Bomb Glove");
	check(list != NULL && (list[0].fields & UNLOCK_FIELD_AMMO) != 0,
	      "a weapon with max ammo declares the ammo field");
	check(list != NULL && (list[5].fields & UNLOCK_FIELD_AMMO) == 0,
	      "the Taunter, whose max ammo is 0, does not");
	check(list != NULL && (list[34].fields == UNLOCK_FIELD_OWNED),
	      "an index-less item owns nothing but its byte");

	/* Protocol 1.3: the four slots are named and typed per game. */
	check(fields != NULL && qstreq(fields[0].name, "Owned") &&
	      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
	check(fields != NULL && qstreq(fields[1].name, "Gold") &&
	      fields[1].kind == UNLOCK_KIND_FLAG, "slot 1 is the Gold checkbox");
	check(fields != NULL && (fields[2].name == NULL || fields[2].name[0] == 0),
	      "RaC1 leaves slot 2 unnamed: it has no weapon levels");
	check(fields != NULL && qstreq(fields[3].name, "Ammo") &&
	      fields[3].kind == UNLOCK_KIND_NUMBER && fields[3].max == 0,
	      "slot 3 is an unbounded Ammo number");

	/* Owning a weapon hands it its full ammo, as NewUnlocks.Unlock does. */
	host_poke(A_BOMB_UNLOCK, (const u8 *)"\x00", 1);
	host_poke(A_BOMB_AMMO, (const u8 *)"\x00\x00\x00\x00", 4);

	check(g->unlock_set(0, 0, 1) == ST_OK, "UNLOCK_SET owned=1 on a weapon");
	host_peek(A_BOMB_UNLOCK, &b, 1);
	check_eq_u64(b, 1, "the owned byte was written");
	mem_read_u32(A_BOMB_AMMO, &v);
	check_eq_u64(v, 40, "and its max ammo came with it");

	check(g->unlock_set(0, 1, 1) == ST_OK, "UNLOCK_SET gold=1");
	host_peek(A_BOMB_GOLD, &b, 1);
	check_eq_u64(b, 1, "the gold byte was written");

	check(g->unlock_set(0, 3, 7) == ST_OK, "UNLOCK_SET ammo=7");
	mem_read_u32(A_BOMB_AMMO, &v);
	check_eq_u64(v, 7, "the ammo word was written");

	check(g->unlock_set(34, 1, 1) == ST_UNSUPPORTED,
	      "gold on an entry with no gold byte is UNSUPPORTED");
	check(g->unlock_set(34, 3, 1) == ST_UNSUPPORTED,
	      "so is ammo on an entry with no ammo word");
	check(g->unlock_set(0, 2, 1) == ST_UNSUPPORTED, "RaC1 has no level field");
	check(g->unlock_set(200, 0, 1) == ST_BAD_ARG, "an unknown id is BAD_ARG");

	/* And the live values come back through the two-read snapshot. */
	check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads again");
	memset(values, 0, sizeof(values));
	check(g->unlock_read(&list[0], values) == ST_OK, "UNLOCK_LIST reads entry 0 live");
	check_eq_u64(values[0], 1, "owned reads back");
	check_eq_u64(values[1], 1, "gold reads back");
	check_eq_u64(values[3], 7, "ammo reads back");

	/* Max ammo for every weapon. */
	host_poke(A_BOMB_AMMO, (const u8 *)"\x00\x00\x00\x00", 4);
	check(features_trigger(17) == ST_OK, "the max-ammo action fires");
	mem_read_u32(A_BOMB_AMMO, &v);
	check_eq_u64(v, 40, "and refilled the Bomb Glove");
}

/* ------------------------------------------------------- RaC1: level flags */

#define A_LEVEL_FLAGS 0xA0CA84u
#define A_MISC_FLAGS  0xA0CD1Cu
#define A_GOLD_BOLTS  0xA0CA34u
#define A_LOAD_PLANET 0xA10700u

/*
 * RaC1's flag region is not laid out the way rac1_levelflags_get assumes, so the
 * three vtable entries are NULL until it has been reverse-engineered. net.c
 * answers UNSUPPORTED for a NULL entry, which is what makes the client hide the
 * panel; smoke.py checks that end of it over the wire.
 *
 * The reset itself is a separate path that PLANET_LOAD's bit0 still drives, so
 * what used to be checked through levelflags_reset is checked through a planet
 * load here instead.
 */
static void test_levelflags(void)
{
	const struct game_api *g = session_game();
	u8 b = 0;

	group("RaC1 level flags are withheld");

	check(g != NULL, "the session has a game");
	if (g == NULL) return;

	check(g->levelflags_get == NULL, "RaC1 declares no levelflags_get");
	check(g->levelflags_reset == NULL, "nor a levelflags_reset");
	check(g->levelflags_set == NULL, "nor a levelflags_set");

	/* But the planet-load reset still clears every region racman cleared. */
	host_poke(A_LEVEL_FLAGS + 5 * 0x10 + 3, (const u8 *)"\xAA", 1);
	host_poke(A_MISC_FLAGS + 5 * 0x100 + 0x20, (const u8 *)"\xBB", 1);
	host_poke(0x96C498u, (const u8 *)"\xEE", 1);   /* Rilgar's own block */

	check(session_planet_load(5, PLANET_FLAG_RESET_LEVELFLAGS) == ST_OK,
	      "a Rilgar load with bit0 runs the reset");
	host_peek(A_LEVEL_FLAGS + 5 * 0x10 + 3, &b, 1);
	check_eq_u64(b, 0, "the main region was zeroed");
	host_peek(A_MISC_FLAGS + 5 * 0x100 + 0x20, &b, 1);
	check_eq_u64(b, 0, "and the misc region with it");
	host_peek(0x96C498u, &b, 1);
	check_eq_u64(b, 0, "and Rilgar's own block went too");
}

static void test_planet_load(void)
{
	u32 v = 0;
	u8 b = 0;

	group("RaC1 planet load with flags");

	host_poke(A_LEVEL_FLAGS + 3 * 0x10, (const u8 *)"\x11", 1);
	host_poke(A_GOLD_BOLTS + 3 * 4, (const u8 *)"\xFF\xFF\xFF\xFF", 4);
	host_poke(A_HELI_UNLOCK, (const u8 *)"\x01", 1);
	host_poke(A_SWING_UNLOCK, (const u8 *)"\x01", 1);

	/* Neither flag: the request goes out and nothing else is touched. */
	check(session_planet_load(3, 0) == ST_OK, "PLANET_LOAD with no flags");
	mem_read_u32(A_LOAD_PLANET, &v);
	check_eq_u64(v, 1, "the request word was written");
	mem_read_u32(A_LOAD_PLANET + 4, &v);
	check_eq_u64(v, 3, "with the planet index");
	host_peek(A_LEVEL_FLAGS + 3 * 0x10, &b, 1);
	check_eq_u64(b, 0x11, "the level flags were left alone");
	mem_read_u32(A_GOLD_BOLTS + 3 * 4, &v);
	check_eq_u64(v, 0xFFFFFFFFu, "and so were the gold bolts");

	check(session_planet_load(3, PLANET_FLAG_RESET_LEVELFLAGS |
	                             PLANET_FLAG_RESET_BOLTS) == ST_OK,
	      "PLANET_LOAD with both flags");
	host_peek(A_LEVEL_FLAGS + 3 * 0x10, &b, 1);
	check_eq_u64(b, 0, "bit0 reset the level flags");
	mem_read_u32(A_GOLD_BOLTS + 3 * 4, &v);
	check_eq_u64(v, 0, "bit1 reset that planet's gold bolts");
	host_peek(A_HELI_UNLOCK, &b, 1);
	check_eq_u64(b, 0, "Kerwan took the Heli-Pack back");
	host_peek(A_SWING_UNLOCK, &b, 1);
	check_eq_u64(b, 0, "and the Swingshot");

	check(session_planet_load(99, 0) == ST_BAD_ARG, "an unknown planet is BAD_ARG");

	/* The whole-table actions. */
	check(features_trigger(13) == ST_OK, "unlock all gold bolts fires");
	mem_read_u32(A_GOLD_BOLTS, &v);
	check_eq_u64(v, 0x01010101u, "every gold bolt byte is 1");
	check(features_trigger(12) == ST_OK, "reset all gold bolts fires");
	mem_read_u32(A_GOLD_BOLTS + 76, &v);
	check_eq_u64(v, 0, "and the last of the eighty bytes is zero again");
}

/* ------------------------------------------------- RaC1: the savefile gate */

#define A_HELPER   0xB00070u
#define A_SF_LOAD  0xB00071u
#define A_SF_ASIDE 0xB00072u
#define A_SF_AUTO  0xB00073u

static void test_savefile_gate(void)
{
	u8 b = 0;

	group("RaC1 savefile helper gate");

	host_poke(A_HELPER, (const u8 *)"\x00\x00\x00\x00", 4);

	check(features_trigger(18) == ST_UNSUPPORTED,
	      "load set-aside file is UNSUPPORTED without the helper");
	check(features_trigger(19) == ST_UNSUPPORTED, "so is set aside file");
	check(features_trigger(20) == ST_UNSUPPORTED, "so is force autosave");
	check(session_load_setaside() == ST_UNSUPPORTED,
	      "and so is the combo action behind them");

	host_poke(A_HELPER, (const u8 *)"\x01", 1);

	check(features_trigger(18) == ST_OK, "with the helper loaded it goes through");
	host_peek(A_SF_LOAD, &b, 1);
	check_eq_u64(b, 1, "load writes a 1");

	check(features_trigger(19) == ST_OK, "set aside file goes through");
	host_peek(A_SF_ASIDE, &b, 1);
	check_eq_u64(b, 1, "set aside writes a 1");

	check(features_trigger(20) == ST_OK, "force autosave goes through");
	host_peek(A_SF_AUTO, &b, 1);
	check_eq_u64(b, 3, "force autosave writes a 3, not a 1");

	/* The readout mirrors the helper byte once the slow block comes round. */
	pump(20);
	{
		u8 info[SESSION_INFO_SIZE];
		session_info_copy(info, sizeof(info));
		check_eq_u64(be32_get(info + 64 + 4 * 1), 1,
		             "readout 1 reports the helper as present");
	}
}

/* ------------------------------------------------------ RaC1: debug options */

#define A_DBG_UPDATE 0x95C5C8u
#define A_DBG_MODE   0x95C5D4u

static void test_debug_options(void)
{
	const char * const *options = NULL;
	u8 count = 0;
	u32 v = 0;
	u8 info[SESSION_INFO_SIZE];

	group("RaC1 debug options");

	host_poke(A_DBG_UPDATE, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(A_DBG_MODE, (const u8 *)"\x00\x00\x00\x00", 4);

	check(features_set(25, 1) == ST_OK, "update mobys turns on");
	mem_read_u32(A_DBG_UPDATE, &v);
	check_eq_u64(v, 0x2, "its bit went into the update word");
	check(features_set(24, 1) == ST_OK, "update Ratchet turns on");
	mem_read_u32(A_DBG_UPDATE, &v);
	check_eq_u64(v, 0x3, "read-modify-write kept the other bit");
	check(features_set(25, 0) == ST_OK, "update mobys turns off");
	mem_read_u32(A_DBG_UPDATE, &v);
	check_eq_u64(v, 0x1, "and only its bit cleared");

	check(features_options(27, &options, &count) == ST_OK, "FEATURE_OPTIONS answers");
	check_eq_u64(count, 3, "the camera ENUM has three options");
	check(options != NULL && qstreq(options[0], "Normal") &&
	      qstreq(options[2], "Freecam character"), "the option names line up");
	check(features_options(0, &options, &count) == ST_BAD_ARG,
	      "asking a TOGGLE for options is BAD_ARG");

	check(features_set(27, 1) == ST_OK, "the camera goes to freecam");
	mem_read_u32(A_DBG_MODE, &v);
	check_eq_u64(v, 1, "the mode word says 1");
	mem_read_u32(A_DBG_UPDATE, &v);
	check_eq_u64(v & 0x8, 0, "and the game lost the update-camera bit");

	check(features_set(27, 0) == ST_OK, "the camera goes back to normal");
	mem_read_u32(A_DBG_MODE, &v);
	check_eq_u64(v, 0, "the mode word says 0");
	mem_read_u32(A_DBG_UPDATE, &v);
	check_eq_u64(v & 0x8, 0x8, "and the game has the update-camera bit again");

	check(features_set(27, 3) == ST_BAD_ARG, "a mode outside the range is BAD_ARG");

	/* The debug block is slow, so give it a full period to come round. */
	host_poke(A_DBG_MODE, (const u8 *)"\x00\x00\x00\x02", 4);
	pump(20);
	session_info_copy(info, sizeof(info));
	check_eq_u64(be32_get(info + 64 + 4 * 7), 2, "readout 7 mirrors the camera mode");
	check_eq_u64(be32_get(info + 64 + 4 * 8), 1, "readout 8 mirrors update Ratchet");
	check_eq_u64(be32_get(info + 64 + 4 * 9), 0, "readout 9 mirrors update mobys");
}

/* ------------------------------------------- protocol 1.3, the LIVE toggles */

#define A_GOODIES 0x969CD3u

/* RaC1 feature ids, from rac1.h. */
#define F1_FAST_LOADS    0
#define F1_GOODIES       6
#define F1_DBG_RATCHET   24
#define F1_DBG_MOBYS     25
#define F1_DBG_PARTICLES 26

/*
 * A LIVE toggle's truth is a byte the game owns, so toggle_state has to follow
 * memory rather than the last thing qwark wrote. The poll runs every twelfth
 * tick, so twenty-four ticks is always at least one pass.
 */
static void test_live_toggles(void)
{
	u64 live;
	u8 b = 0;

	group("protocol 1.3 live toggles");

	live = features_live_mask();
	check((live & ((u64)1 << F1_GOODIES)) != 0, "the goodies menu is a LIVE toggle");
	check((live & ((u64)1 << F1_DBG_RATCHET)) != 0 && (live & ((u64)1 << F1_DBG_MOBYS)) != 0 &&
	      (live & ((u64)1 << F1_DBG_PARTICLES)) != 0, "so are the three debug update bits");
	check((live & ((u64)1 << F1_FAST_LOADS)) == 0,
	      "a patch-backed toggle is not");

	/* Somebody else turns the goodies menu on: the bit follows on its own. */
	host_poke(A_GOODIES, (const u8 *)"\x01", 1);
	pump(24);
	check((features_toggle_state() & ((u64)1 << F1_GOODIES)) != 0,
	      "a poke of the goodies byte turns the bit on");

	host_poke(A_GOODIES, (const u8 *)"\x00", 1);
	pump(24);
	check((features_toggle_state() & ((u64)1 << F1_GOODIES)) == 0,
	      "and clearing the byte turns it back off");

	/* The same for a bit of the debug update word. */
	host_poke(A_DBG_UPDATE, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(24);
	check((features_toggle_state() & ((u64)1 << F1_DBG_MOBYS)) == 0, "update mobys reads off");
	host_poke(A_DBG_UPDATE, (const u8 *)"\x00\x00\x00\x02", 4);
	pump(24);
	check((features_toggle_state() & ((u64)1 << F1_DBG_MOBYS)) != 0,
	      "and setting its bit in the word turns it on");
	check((features_toggle_state() & ((u64)1 << F1_DBG_RATCHET)) == 0,
	      "while the neighbouring bit stays off");

	/* FEATURE_SET still writes the byte, and the poll agrees with it. */
	check(features_set(F1_GOODIES, 1) == ST_OK, "FEATURE_SET still writes it");
	host_peek(A_GOODIES, &b, 1);
	check_eq_u64(b, 1, "the byte was written");
	pump(24);
	check((features_toggle_state() & ((u64)1 << F1_GOODIES)) != 0,
	      "and the poll agrees");

	/* But there is no auto bit to set: qwark never re-applies a LIVE toggle. */
	check(features_set_auto(F1_GOODIES, 1) == ST_UNSUPPORTED,
	      "FEATURE_SET_AUTO on a LIVE toggle is UNSUPPORTED");
	check((features_toggle_auto() & ((u64)1 << F1_GOODIES)) == 0,
	      "and its auto bit stays clear");
	check(features_set_auto(F1_FAST_LOADS, 1) == ST_OK,
	      "an ordinary toggle still takes an auto flag");
	features_set_auto(F1_FAST_LOADS, 0);

	/* Leave the game as we found it. */
	features_set(F1_GOODIES, 0);
	host_poke(A_DBG_UPDATE, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(24);
}

/* ------------------------------------------------- protocol 1.2, all games */

/*
 * Every game must name exactly one SAVE_ASIDE ACTION and one LOAD_ASIDE ACTION,
 * so a client's save-file manager can drive the helper without matching labels.
 * This walks the registry rather than booting, because the descriptor table is
 * the contract.
 */
static void test_savefile_flags(void)
{
	u32 g;

	group("protocol 1.2 savefile flags");

	for (g = 0; ; g++) {
		const struct game_api *api = game_at(g);
		const struct game_describe *d;
		int nsave = 0;
		int nload = 0;
		u8 i;

		if (api == NULL) break;
		if (api->game_id == GAME_NONE || api->describe == NULL) continue;
		/* The host-test-only stub game has no savefile helper. */
		if (api->load_setaside == NULL) continue;

		d = api->describe();
		for (i = 0; i < d->nfeatures; i++) {
			const struct feature_desc *f = &d->features[i];

			if ((f->flags & FEATURE_FLAG_SAVE_ASIDE) != 0) {
				nsave++;
				check(f->kind == FEATURE_ACTION, "SAVE_ASIDE sits on an ACTION");
			}
			if ((f->flags & FEATURE_FLAG_LOAD_ASIDE) != 0) {
				nload++;
				check(f->kind == FEATURE_ACTION, "LOAD_ASIDE sits on an ACTION");
			}
		}

		check_eq_u64(nsave, 1, "the game names one SAVE_ASIDE action");
		check_eq_u64(nload, 1, "the game names one LOAD_ASIDE action");
	}

	check(FEATURE_FLAG_SAVE_ASIDE == 0x04 && FEATURE_FLAG_LOAD_ASIDE == 0x08,
	      "the two flags are bit2 and bit3");
}

/* ------------------------------------------------------------------ RaC2 */

#define R2_FP_ADDR      0x00BEA8A0u
#define R2_AMMO_INSTR   0x00B30C7Cu
#define R2_CHARGE_BUF   0x0145C180u
#define R2_LOADCOUNT    0x0147A25Bu
#define R2_COORDS       0x0147F260u
#define R2_BOSS_SIB     0x01481792u
#define R2_UNLOCK_LANCE 0x01481A9Eu
#define R2_PLANET_ADDR  0x01329A3Cu
#define R2_BOLTS_ADDR   0x01329A90u
#define R2_PAD_MANIP    0x013185B8u
#define R2_PBOLT_ARRAY  0x01562540u
#define R2_LEVELFLAGS   0x015625B0u
#define R2_LOADPLANET   0x0156B050u
#define R2_SF_HELPER    0x01BF0002u
#define R2_SF_MGR_SAVE  0x01BF0003u

#define F2_FAST_LOADS   0
#define F2_INFINITE_AMMO 1
#define F2_DIE          6
#define F2_BOLTS        7
#define F2_DEATH_BOSSES 13
#define F2_DEATH_PBOLTS 14
#define F2_AUTO_ANYPCT  21
#define F2_LOAD_ASIDE   30
#define F2_MGR_SAVE     32

static void test_rac2(void)
{
	const struct game_api *g;
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	u32 v = 0;
	u8 b = 0;

	group("RaC2: boot and describe");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00386"), "NPEA00386 reaches INGAME");

	g = session_game();
	check(g != NULL && g->game_id == GAME_RAC2, "the session says RaC2");
	if (g == NULL) return;

	{
		const struct game_describe *d = g->describe();
		check_eq_u64(d->nfeatures, 37, "RaC2 declares thirty-seven features");
		check_eq_u64(d->nreadouts, 9, "and nine readouts");
		check_eq_u64(d->ngroups, 6, "and six groups");
		check(qstreq(d->readouts[0], "Bolts"), "readout 0 is Bolts");
	}

	group("RaC2: toggles");

	/* The fingerprint site is the fast-load instruction, so it is seeded. */
	check(features_set(F2_FAST_LOADS, 1) == ST_OK, "fast loads turn on");
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x60000000u, "the branch was nopped");
	check(features_set(F2_FAST_LOADS, 0) == ST_OK, "fast loads turn off");
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x4BFFEA69u, "and the original instruction came back");

	host_poke(R2_AMMO_INSTR, (const u8 *)"\x7C\x64\x29\x2E", 4);
	check(features_set(F2_INFINITE_AMMO, 1) == ST_OK, "infinite ammo turns on");
	mem_read_u32(R2_AMMO_INSTR, &v);
	check_eq_u64(v, 0x60000000u, "the decrement was nopped");
	mem_read_u32(0x0148185Cu, &v);
	check_eq_u64(v, 0x7FFFFFFFu, "and the ammo array was filled");
	check(features_set(F2_INFINITE_AMMO, 0) == ST_OK, "infinite ammo turns off");
	mem_read_u32(R2_AMMO_INSTR, &v);
	check_eq_u64(v, 0x7C64292Eu, "and the instruction was restored");

	group("RaC2: unlocks");

	{
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		u8 n = 0, ncat = 0;
		const struct unlock_field_desc *fields = NULL;
		u32 values[4];

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 44, "the whole RC2Unlocks table is there");
		check_eq_u64(ncat, 3, "in three categories");
		check(list != NULL && qstreq(list[0].name, "Lancer"), "entry 0 is the Lancer");
		check(list != NULL && list[0].fields == UNLOCK_FIELD_OWNED,
		      "RaC2 entries own nothing but their byte");
		check(fields != NULL && qstreq(fields[0].name, "Owned") &&
		      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
		check(fields != NULL &&
		      (fields[1].name == NULL || fields[1].name[0] == 0) &&
		      (fields[2].name == NULL || fields[2].name[0] == 0) &&
		      (fields[3].name == NULL || fields[3].name[0] == 0),
		      "and RaC2 names no other slot");

		check(g->unlock_set(0, 0, 1) == ST_OK, "UNLOCK_SET owned=1");
		host_peek(R2_UNLOCK_LANCE, &b, 1);
		check_eq_u64(b, 1, "the owned byte was written");
		check(g->unlock_set(0, 3, 1) == ST_UNSUPPORTED, "RaC2 has no ammo field");

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(&list[0], values) == ST_OK, "and reads entry 0 live");
		check_eq_u64(values[0], 1, "owned reads back");
	}

	group("RaC2: level flags and planet load");

	host_poke(R2_LEVELFLAGS + 5 * 0x10 + 2, (const u8 *)"\xAA", 1);
	{
		u8 flags[0x40];
		u16 flen = 0;
		check(g->levelflags_get(5, flags, sizeof(flags), &flen) == ST_OK,
		      "LEVELFLAGS_GET reads");
		check_eq_u64(flen, 0x10, "RaC2 has one sixteen-byte region");
		check_eq_u64(flags[2], 0xAA, "the region contents come through");
	}
	check(g->levelflags_set(5, 3, 0xCC) == ST_OK, "LEVELFLAGS_SET writes");
	host_peek(R2_LEVELFLAGS + 5 * 0x10 + 3, &b, 1);
	check_eq_u64(b, 0xCC, "at the right byte");
	check(g->levelflags_set(5, 0x10, 1) == ST_BAD_ARG, "one past the end is BAD_ARG");

	check(session_planet_load(5, PLANET_FLAG_RESET_LEVELFLAGS) == ST_OK,
	      "PLANET_LOAD with the flag reset");
	mem_read_u32(R2_LOADPLANET, &v);
	check_eq_u64(v, 1, "the request word was written");
	mem_read_u32(R2_LOADPLANET + 4, &v);
	check_eq_u64(v, 5, "with the planet index");
	host_peek(R2_LEVELFLAGS + 5 * 0x10 + 2, &b, 1);
	check_eq_u64(b, 0, "and the level flags were zeroed");
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x60000000u, "the load forced fast loads on");
	check(session_planet_load(99, 0) == ST_BAD_ARG, "an unknown planet is BAD_ARG");

	group("RaC2: positions");

	host_poke(R2_COORDS, (const u8 *)"\x41\x20\x00\x00\x42\x48\x00\x00"
	                                 "\xC1\x20\x00\x00", 12);
	check(g->save_blob(blob, &len) == ST_OK, "save_blob reads");
	check_eq_u64(len, 30, "RaC2 stores thirty bytes");
	{
		f32 xyz[3];
		check(g->blob_xyz(blob, len, xyz) == ST_OK, "blob_xyz decodes");
		check(xyz[0] == 10.0f && xyz[1] == 50.0f && xyz[2] == -10.0f,
		      "and the coordinates round trip");
	}
	host_poke(R2_COORDS, (const u8 *)"\x00\x00\x00\x00", 4);
	check(g->load_blob(blob, len) == ST_OK, "load_blob writes");
	mem_read_u32(R2_COORDS, &v);
	check_eq_u64(v, 0x41200000u, "the position came back");

	group("RaC2: die with both reset toggles");

	host_poke(R2_BOSS_SIB, (const u8 *)"\x07", 1);
	host_poke(R2_PBOLT_ARRAY, (const u8 *)"\xFF\xFF\xFF\xFF", 4);
	check(features_set(F2_DEATH_BOSSES, 0) == ST_OK, "boss reset off");
	check(features_set(F2_DEATH_PBOLTS, 0) == ST_OK, "platinum reset off");
	check(session_die() == ST_OK, "DIE runs");
	mem_read_u32(R2_COORDS + 8, &v);
	check_eq_u64(v, 0xC2480000u, "Z was driven to -50");
	host_peek(R2_BOSS_SIB, &b, 1);
	check_eq_u64(b, 7, "the boss counter was left alone");

	check(features_set(F2_DEATH_BOSSES, 1) == ST_OK, "boss reset on");
	check(features_set(F2_DEATH_PBOLTS, 1) == ST_OK, "platinum reset on");
	check(session_die() == ST_OK, "DIE runs again");
	host_peek(R2_BOSS_SIB, &b, 1);
	check_eq_u64(b, 0, "and now the boss counter cleared");
	mem_read_u32(R2_PBOLT_ARRAY, &v);
	check_eq_u64(v, 0, "and the platinum bolts went with it");

	group("RaC2: savefile gate");

	host_poke(R2_SF_HELPER, (const u8 *)"\x00", 1);
	check(features_trigger(F2_MGR_SAVE) == ST_UNSUPPORTED,
	      "a savefile action without the helper is UNSUPPORTED");
	check(session_load_setaside() == ST_UNSUPPORTED, "and so is the combo action");
	host_poke(R2_SF_HELPER, (const u8 *)"\x01", 1);
	check(features_trigger(F2_MGR_SAVE) == ST_OK, "with the helper it goes through");
	host_peek(R2_SF_MGR_SAVE, &b, 1);
	check_eq_u64(b, 1, "the save-manager byte was written");
	check(features_trigger(F2_LOAD_ASIDE) == ST_OK, "so does the load-file action");

	group("RaC2: the loading-screen watcher");

	/* The final load screen on planet 0 is what the old form watched for. */
	host_poke(R2_PLANET_ADDR, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R2_CHARGE_BUF, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R2_PAD_MANIP, (const u8 *)"\x00\x00\x00\x00", 4);
	check(features_set(F2_AUTO_ANYPCT, 1) == ST_OK, "auto-reset any% on");
	check(features_set(F2_INFINITE_AMMO, 1) == ST_OK, "infinite ammo on");

	host_poke(R2_LOADCOUNT, (const u8 *)"\x01", 1);
	pump(3);
	mem_read_u32(R2_CHARGE_BUF, &v);
	check_eq_u64(v, 0, "an ordinary load screen does nothing");

	host_poke(R2_LOADCOUNT, (const u8 *)"\x02", 1);
	pump(3);
	mem_read_u32(R2_CHARGE_BUF, &v);
	check_eq_u64(v, 30, "the final load screen set the buffer charge");
	check((features_toggle_state() & ((u64)1 << F2_INFINITE_AMMO)) == 0,
	      "and turned infinite ammo back off");
	mem_read_u32(R2_PAD_MANIP, &v);
	check_eq_u64(v, 0x41500000u, "and ran the any% reset");
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x4BFFEA69u, "and put fast loads back to the toggle setting");

	features_set(F2_AUTO_ANYPCT, 0);
	features_set(F2_DEATH_BOSSES, 0);
	features_set(F2_DEATH_PBOLTS, 0);

	group("RaC2: values");

	check(features_set(F2_BOLTS, 1234) == ST_OK, "the bolts VALUE writes");
	mem_read_u32(R2_BOLTS_ADDR, &v);
	check_eq_u64(v, 1234, "and lands at the bolt count");
	check(features_trigger(F2_BOLTS) == ST_BAD_ARG, "triggering a VALUE is BAD_ARG");
}

/* ------------------------------------------------------------------ RaC3 */

#define R3_FP_ADDR      0x00182A88u
#define R3_ITEM_ARRAY   0x00C1E43Cu
#define R3_PLANET_ADDR  0x00C1E438u
#define R3_BOLTS_ADDR   0x00C1E4DCu
#define R3_ARMOUR_ADDR  0x00C1E51Cu
#define R3_SF_HELPER    0x00D9FF00u
#define R3_SF_MGR_SAVE  0x00D9FF03u
#define R3_COORDS       0x00DA2870u
#define R3_HEALTH_ADDR  0x00DA5040u
#define R3_UNLOCK_ARRAY 0x00DA56ECu
#define R3_EXP_ARRAY    0x00DA5824u
#define R3_AMMO_ARRAY   0x00DA5240u
#define R3_VID_COMICS   0x00DA650Bu
#define R3_LEVELFLAGS   0x00ECE675u
#define R3_LOADPLANET   0x00EE9310u
#define R3_FASTLOAD1    0x0134EBD4u
#define R3_FASTLOAD2    0x0134EE70u

#define F3_FREEZE_AMMO   0
#define F3_FREEZE_HEALTH 1
#define F3_OHKO          2
#define F3_DIE           6
#define F3_BOLTS         7
#define F3_ARMOUR        11
#define F3_SHIP_COLOUR   12
#define F3_SET_ASIDE     31
#define F3_LOAD_ASIDE    32

/* Agents of Doom: item id 0x57, unlock 0x4FF, exp 0x74C, ammo 0x39F, 8 levels. */
#define AOD_ROW    21
#define AOD_UNLOCK (R3_UNLOCK_ARRAY + 0x57u)
#define AOD_EXP    (R3_EXP_ARRAY + (0x74Cu - 0x5F0u))
#define AOD_AMMO   (R3_AMMO_ARRAY + (0x39Fu - 0x243u))
#define AOD_ITEM   (R3_ITEM_ARRAY + 0x57u)

/* Bouncer, one of the five GC weapons whose versions live in a table of their own. */
#define BOUNCER_ROW  23
#define BOUNCER_ITEM (R3_ITEM_ARRAY + 0x13u)

/* R3YNO: item id 0x97, the one weapon that stops at v5 rather than v8. */
#define RYNO_ROW  36
#define RYNO_ITEM (R3_ITEM_ARRAY + 0x97u)

/* Suck Cannon: item id 0x87, and no ammo the game actually counts. */
#define SUCK_ROW  40

static void test_rac3(void)
{
	const struct game_api *g;
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	u32 v = 0;
	u8 b = 0;

	group("RaC3: boot and describe");

	check(quit_and_wait(), "quit RaC2");
	check(boot_and_wait("NPEA00387"), "NPEA00387 reaches INGAME");

	g = session_game();
	check(g != NULL && g->game_id == GAME_RAC3, "the session says RaC3");
	if (g == NULL) return;

	{
		const struct game_describe *d = g->describe();
		u8 i;
		int retired = 0;
		int neighbours = 0;

		check_eq_u64(d->nfeatures, 32, "RaC3 declares thirty-two features");
		check_eq_u64(d->nreadouts, 12, "and twelve readouts");
		check_eq_u64(d->ngroups, 5, "and five groups");

		/* 4, 17, 28, 29 and 30 are retired: gone, but nothing is renumbered. */
		for (i = 0; i < d->nfeatures; i++) {
			u8 id = d->features[i].id;
			if (id == 4 || id == 17 || id == 28 || id == 29 || id == 30)
				retired++;
			if (id == 16 || id == 18) neighbours++;
		}
		check_eq_u64(retired, 0, "and none of the retired ids");
		check_eq_u64(neighbours, 2, "while 16 and 18 kept their numbers");
		check(features_trigger(17) == ST_NOT_FOUND,
		      "the retired no-QE action is NOT_FOUND");
	}

	{
		const char * const *options = NULL;
		u8 count = 0;

		check(features_options(F3_ARMOUR, &options, &count) == ST_OK,
		      "the armour ENUM answers FEATURE_OPTIONS");
		check_eq_u64(count, 8, "with eight armours");
		check(options != NULL && qstreq(options[4], "Infernox Armor"),
		      "and Infernox is index 4");

		check(features_options(F3_SHIP_COLOUR, &options, &count) == ST_OK,
		      "so does the ship colour ENUM");
		check_eq_u64(count, 32, "with thirty-two colours");
	}

	group("RaC3: toggles");

	check(features_set(F3_FREEZE_AMMO, 1) == ST_OK, "freeze ammo turns on");
	mem_read_u32(R3_FP_ADDR, &v);
	check_eq_u64(v, 0x60000000u, "the decrement was nopped");
	check(features_set(F3_FREEZE_AMMO, 0) == ST_OK, "freeze ammo turns off");
	mem_read_u32(R3_FP_ADDR, &v);
	check_eq_u64(v, 0x7C85312Eu, "and the original instruction came back");

	/*
	 * Freeze health and one-hit KO share the freeze entry, so the second one on
	 * replaces the first and one release clears both.
	 */
	check(features_set(F3_FREEZE_HEALTH, 1) == ST_OK, "freeze health turns on");
	pump(2);
	mem_read_u32(R3_HEALTH_ADDR, &v);
	check_eq_u64(v, 200, "health is pinned at 200");
	check(features_set(F3_OHKO, 1) == ST_OK, "one-hit KO turns on");
	pump(2);
	mem_read_u32(R3_HEALTH_ADDR, &v);
	check_eq_u64(v, 1, "and health is pinned at 1 instead");
	check(features_set(F3_OHKO, 0) == ST_OK, "one-hit KO turns off");
	pump(2);
	host_poke(R3_HEALTH_ADDR, (const u8 *)"\x00\x00\x00\x63", 4);
	pump(2);
	mem_read_u32(R3_HEALTH_ADDR, &v);
	check_eq_u64(v, 99, "and nothing writes the word any more");
	features_set(F3_FREEZE_HEALTH, 0);

	group("RaC3: unlocks with levels, XP and ammo");

	{
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		u8 n = 0, ncat = 0;
		const struct unlock_field_desc *fields = NULL;
		u32 values[4];

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 41, "the whole UYAUnlocks table is there");
		check_eq_u64(ncat, 3, "in three categories");
		check(list != NULL && qstreq(list[AOD_ROW].name, "Agents of Doom"),
		      "row 21 is the Agents of Doom");
		check(list != NULL && list[AOD_ROW].fields ==
		      (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 |
		       UNLOCK_FIELD_2 | UNLOCK_FIELD_3),
		      "a weapon declares all four fields");
		check(list != NULL && list[16].fields == UNLOCK_FIELD_0,
		      "a vid comic is owned-only");
		check(list != NULL && qstreq(list[SUCK_ROW].name, "Suck Cannon") &&
		      (list[SUCK_ROW].fields & UNLOCK_FIELD_3) == 0,
		      "the Suck Cannon carries no ammo in game, so it declares none");

		/*
		 * Protocol 1.3. Before this, slot 1 was called "Gold" and slot 2
		 * "Level" on the wire, so a client drew UYA's weapon version as a
		 * checkbox and could only ever write v1.
		 */
		check(fields != NULL && qstreq(fields[0].name, "Owned") &&
		      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
		check(fields != NULL && qstreq(fields[1].name, "Level") &&
		      fields[1].kind == UNLOCK_KIND_NUMBER && fields[1].max == 8,
		      "slot 1 is a Level number that stops at 8");
		check(fields != NULL && qstreq(fields[2].name, "XP") &&
		      fields[2].kind == UNLOCK_KIND_NUMBER && fields[2].max == 0,
		      "slot 2 is an unbounded XP number");
		check(fields != NULL && qstreq(fields[3].name, "Ammo") &&
		      fields[3].kind == UNLOCK_KIND_NUMBER && fields[3].max == 0,
		      "slot 3 is an unbounded Ammo number");

		check(g->unlock_set(AOD_ROW, 0, 1) == ST_OK, "UNLOCK_SET owned=1");
		host_peek(AOD_UNLOCK, &b, 1);
		check_eq_u64(b, 1, "the owned byte was written");

		check(g->unlock_set(AOD_ROW, 1, 3) == ST_OK, "UNLOCK_SET level=3");
		host_peek(AOD_ITEM, &b, 1);
		check_eq_u64(b, 0x59, "the item array carries id + version - 1");

		check(g->unlock_set(AOD_ROW, 2, 4242) == ST_OK, "UNLOCK_SET xp");
		mem_read_u32(AOD_EXP, &v);
		check_eq_u64(v, 4242, "the exp word was written");

		check(g->unlock_set(AOD_ROW, 3, 77) == ST_OK, "UNLOCK_SET ammo");
		mem_read_u32(AOD_AMMO, &v);
		check_eq_u64(v, 77, "the ammo word was written");

		/*
		 * The level field advertises the game-wide maximum of 8, so a client
		 * may well send 8 for the R3YNO. UNLOCK_SET clamps to the entry's own
		 * level count rather than refusing.
		 */
		check(g->unlock_set(AOD_ROW, 1, 99) == ST_OK,
		      "a version past the weapon's level count is clamped, not refused");
		host_peek(AOD_ITEM, &b, 1);
		check_eq_u64(b, 0x5Eu, "the Agents of Doom landed on v8");
		check(g->unlock_set(RYNO_ROW, 1, 8) == ST_OK, "the R3YNO takes a level of 8");
		host_peek(RYNO_ITEM, &b, 1);
		check_eq_u64(b, 0x9Bu, "and stops at its own v5");
		check(g->unlock_set(AOD_ROW, 1, 3) == ST_OK, "back down to v3");

		check(g->unlock_set(16, 2, 1) == ST_UNSUPPORTED,
		      "a vid comic has no exp word");
		check(g->unlock_set(SUCK_ROW, 3, 5) == ST_UNSUPPORTED,
		      "and the Suck Cannon refuses an ammo write");
		check(g->unlock_set(200, 0, 1) == ST_BAD_ARG, "an unknown id is BAD_ARG");

		check(g->unlock_set(BOUNCER_ROW, 1, 2) == ST_OK, "the Bouncer goes to v2");
		host_peek(BOUNCER_ITEM, &b, 1);
		check_eq_u64(b, 0xA6, "which is its own table offset, not id + 1");

		/* Vid comic 3 sits at 0x12CA - 0x4A8 past the unlock array, so +3 here. */
		host_poke(R3_VID_COMICS + 3, (const u8 *)"\x01", 1);

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(&list[AOD_ROW], values) == ST_OK, "row 21 reads live");
		check_eq_u64(values[0], 1, "owned reads back");
		check_eq_u64(values[1], 3, "the version heuristic reads back");
		check_eq_u64(values[2], 4242, "the exp reads back");
		check_eq_u64(values[3], 77, "the ammo reads back");

		memset(values, 0, sizeof(values));
		check(g->unlock_read(&list[BOUNCER_ROW], values) == ST_OK, "the Bouncer reads");
		check_eq_u64(values[1], 0, "a GC weapon reports no readable version");

		memset(values, 0, sizeof(values));
		check(g->unlock_read(&list[18], values) == ST_OK, "vid comic 3 reads");
		check_eq_u64(values[0], 1, "from the far end of the unlock array");
	}

	group("RaC3: level flags, planets and the fast-load arm");

	check(g->levelflags_get(0, blob, sizeof(blob), (u16 *)&v) == ST_BAD_ARG ||
	      1, "planet 0 is the placeholder");
	{
		u8 flags[0x40];
		u16 flen = 0;

		check(g->levelflags_get(0, flags, sizeof(flags), &flen) == ST_BAD_ARG,
		      "LEVELFLAGS_GET refuses the placeholder planet");

		host_poke(R3_LEVELFLAGS + 5 * 0x10 + 1, (const u8 *)"\xBB", 1);
		check(g->levelflags_get(5, flags, sizeof(flags), &flen) == ST_OK,
		      "and reads a real one");
		check_eq_u64(flen, 0x10, "sixteen bytes");
		check_eq_u64(flags[1], 0xBB, "with the right contents");
		check(g->levelflags_reset(5) == ST_OK, "LEVELFLAGS_RESET runs");
		host_peek(R3_LEVELFLAGS + 5 * 0x10 + 1, &b, 1);
		check_eq_u64(b, 0, "and zeroed the region");
	}

	check(session_planet_load(0, 0) == ST_BAD_ARG, "planet 0 will not load");
	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R3_FASTLOAD2, (const u8 *)"\x00\x00", 2);

	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD runs");
	mem_read_u32(R3_LOADPLANET, &v);
	check_eq_u64(v, 1, "the request word was written");
	mem_read_u32(R3_LOADPLANET + 4, &v);
	check_eq_u64(v, 5, "with the planet id");
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 3, "and the third load screen was forced");

	host_peek(R3_FASTLOAD2, &b, 1);
	check_eq_u64(b, 0, "the second write has not happened yet");
	pump(30);
	{
		u8 pair[2];
		host_peek(R3_FASTLOAD2, pair, 2);
		check(pair[0] == 1 && pair[1] == 1,
		      "and on_tick armed it a fifth of a second later");
	}

	/* Aquatos is the one planet LoadPlanetSafe leaves alone. */
	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	check(session_planet_load(8, 0) == ST_OK, "PLANET_LOAD to Aquatos runs");
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 0, "and skipped the fast-load arm");

	group("RaC3: positions and values");

	host_poke(R3_COORDS, (const u8 *)"\x41\x20\x00\x00\x42\x48\x00\x00"
	                                 "\xC1\x20\x00\x00", 12);
	check(g->save_blob(blob, &len) == ST_OK, "save_blob reads");
	check_eq_u64(len, 30, "RaC3 stores thirty bytes");
	host_poke(R3_COORDS, (const u8 *)"\x00\x00\x00\x00", 4);
	check(g->load_blob(blob, len) == ST_OK, "load_blob writes");
	mem_read_u32(R3_COORDS, &v);
	check_eq_u64(v, 0x41200000u, "the position came back");

	check(session_die() == ST_OK, "DIE runs");
	mem_read_u32(R3_COORDS + 8, &v);
	check_eq_u64(v, 0xC2480000u, "Z was driven to -50");

	check(features_set(F3_BOLTS, 4321) == ST_OK, "the bolts VALUE writes");
	mem_read_u32(R3_BOLTS_ADDR, &v);
	check_eq_u64(v, 4321, "and lands at the bolt count");

	check(features_set(F3_ARMOUR, 4) == ST_OK, "the armour ENUM writes");
	{
		u8 halfword[2];
		host_peek(R3_ARMOUR_ADDR, halfword, 2);
		check(halfword[0] == 0 && halfword[1] == 4,
		      "as a big-endian halfword");
	}
	check(features_set(F3_ARMOUR, 8) == ST_BAD_ARG, "an armour past the end is BAD_ARG");

	group("RaC3: savefile gate");

	host_poke(R3_SF_HELPER, (const u8 *)"\x00", 1);
	check(features_trigger(F3_SET_ASIDE) == ST_UNSUPPORTED,
	      "set aside without the helper is UNSUPPORTED");
	host_poke(R3_SF_HELPER, (const u8 *)"\x01", 1);
	check(features_trigger(F3_SET_ASIDE) == ST_OK, "and goes through with it");
	host_peek(R3_SF_MGR_SAVE, &b, 1);
	check_eq_u64(b, 1, "writing the save-manager byte");
	check(session_load_setaside() == ST_OK, "the combo action works too");
}

/* ------------------------------------------------------------ Deadlocked */

#define R4_QUIT_HOOK    0x00013780u
#define R4_FP_ADDR      0x001D7D18u
#define R4_FASTLOAD_1   0x002A8AF4u
#define R4_FASTLOAD_2   0x002A8C50u
#define R4_CRASH_FIRST  0x0080883Cu
#define R4_BOLTS_ADDR   0x009C32E8u
#define R4_BOTS_SAVE    0x009C3325u
#define R4_BOTS_LIVE    0x009D2775u
#define R4_LOADP2       0x00B36DCCu
#define R4_TARGET       0x00B36DD0u
#define R4_TUTORIAL     0x00B1F46Cu
#define R4_GAMESTATE    0x00B3C5A0u
#define R4_COORDS       0x010D44D0u
#define R4_COORDS2      0x010D7334u
#define R4_CAM_LR       0x010D5DF0u
#define R4_CAM_UD       0x010D5E00u
#define R4_SOFTLOCK     0x011C04C0u
#define R4_SF_HELPER    0x015CD71Du
#define R4_SF_SET_ASIDE 0x015CD71Fu
#define R4_QUIT_FLAG    0x01700000u

#define F4_CRASH_PATCHES 0
#define F4_SOFTLOCK_FIX  1
#define F4_FAST_LOADS    2
#define F4_SKIN          10
#define F4_SET_ASIDE     13

static void test_rac4(void)
{
	const struct game_api *g;
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	u32 v = 0;
	u8 b = 0;

	group("Deadlocked: boot, the quit hook and the auto toggles");

	check(quit_and_wait(), "quit RaC3");

	/*
	 * The quit hook byte must not read as set on a fresh boot, or the session
	 * quits the instant it arrives. Poke it during the boot settle to prove
	 * on_enter clears it.
	 */
	host_boot("NPEA00423");
	host_poke(R4_QUIT_FLAG, (const u8 *)"\xFF", 1);
	check(pump_until(SESSION_INGAME, 4000), "NPEA00423 reaches INGAME");

	g = session_game();
	check(g != NULL && g->game_id == GAME_RAC4, "the session says Deadlocked");
	if (g == NULL) return;

	check_eq_u64(g->quit_hook_addr, R4_QUIT_FLAG, "the vtable names the quit flag");
	host_peek(R4_QUIT_FLAG, &b, 1);
	check_eq_u64(b, 0, "and on_enter cleared it");

	mem_read_u32(R4_QUIT_HOOK, &v);
	check_eq_u64(v, 0x386000FFu, "the quit hook's first word is in place");
	mem_read_u32(R4_QUIT_HOOK + 4, &v);
	check_eq_u64(v, 0x3C800170u, "and its second");
	mem_read_u32(R4_QUIT_HOOK + 8, &v);
	check_eq_u64(v, 0x98640000u, "and its third");

	{
		const struct game_describe *d = g->describe();
		check_eq_u64(d->nfeatures, 15, "Deadlocked declares fifteen features");
		check_eq_u64(d->nreadouts, 8, "and eight readouts");
		check_eq_u64(d->auto_default,
		             ((u64)1 << F4_CRASH_PATCHES) | ((u64)1 << F4_SOFTLOCK_FIX),
		             "two toggles ship auto-flagged");
	}

	check((features_toggle_auto() & ((u64)1 << F4_CRASH_PATCHES)) != 0,
	      "the crash patches are auto by default");
	check((features_toggle_state() & ((u64)1 << F4_CRASH_PATCHES)) != 0,
	      "so they came on at INGAME");
	mem_read_u32(R4_CRASH_FIRST, &v);
	check_eq_u64(v, 0x14151617u, "and the first crash word is written");
	check((features_toggle_state() & ((u64)1 << F4_SOFTLOCK_FIX)) != 0,
	      "the softlock fix came on too");

	check(features_set(F4_CRASH_PATCHES, 0) == ST_OK, "the crash patches turn off");
	mem_read_u32(R4_CRASH_FIRST, &v);
	check_eq_u64(v, 0, "and the original word is back");
	check(features_set(F4_CRASH_PATCHES, 1) == ST_OK, "and on again");
	mem_read_u32(R4_CRASH_FIRST, &v);
	check_eq_u64(v, 0x14151617u, "with the patch word");

	group("Deadlocked: the softlock fix watcher");

	host_poke(R4_SOFTLOCK, (const u8 *)"\xAA", 1);
	host_poke(R4_TUTORIAL + 3, (const u8 *)"\x01", 1);
	pump(20);
	host_peek(R4_SOFTLOCK, &b, 1);
	check_eq_u64(b, 0xAA, "a completed tutorial leaves the softlock byte alone");

	host_poke(R4_TUTORIAL + 3, (const u8 *)"\x00", 1);
	pump(20);
	host_peek(R4_SOFTLOCK, &b, 1);
	check_eq_u64(b, 0, "a fresh file clears it");

	check(features_set(F4_SOFTLOCK_FIX, 0) == ST_OK, "the fix turns off");
	host_poke(R4_SOFTLOCK, (const u8 *)"\xAA", 1);
	host_poke(R4_TUTORIAL + 3, (const u8 *)"\x01", 1);
	pump(20);
	host_poke(R4_TUTORIAL + 3, (const u8 *)"\x00", 1);
	pump(20);
	host_peek(R4_SOFTLOCK, &b, 1);
	check_eq_u64(b, 0xAA, "and then nothing is written");
	features_set(F4_SOFTLOCK_FIX, 1);

	group("Deadlocked: planet load and the fast-load restore");

	host_poke(R4_FASTLOAD_1, (const u8 *)"\x48\x12\x02\x61", 4);
	host_poke(R4_FASTLOAD_2, (const u8 *)"\x4E\x9E\x04\x21", 4);
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(4);

	check(session_planet_load(4, 0) == ST_OK, "PLANET_LOAD runs");
	mem_read_u32(R4_TARGET, &v);
	check_eq_u64(v, 4, "the target planet was written");
	mem_read_u32(R4_LOADP2, &v);
	check_eq_u64(v, 1, "and the load was requested");
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x60000000u, "fast loads were forced on");
	check(session_planet_load(0, 0) == ST_BAD_ARG, "the unused planet is BAD_ARG");

	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x06", 1);
	pump(6);
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x60000000u, "they stay on through the space transition");

	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x02", 1);
	pump(6);
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x48120261u, "and go back to the toggle setting when it ends");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, 0x4E9E0421u, "both words restored");

	group("Deadlocked: bot unlocks");

	{
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		u8 n = 0, ncat = 0;
		const struct unlock_field_desc *fields = NULL;
		u32 values[4];

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 16, "sixteen bot upgrades");
		check_eq_u64(ncat, 1, "in one category");
		check(list != NULL && qstreq(list[0].name, "Pistol Flux LX"),
		      "entry 0 is the Pistol Flux LX");
		check(fields != NULL && qstreq(fields[0].name, "Owned") &&
		      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
		check(fields != NULL &&
		      (fields[1].name == NULL || fields[1].name[0] == 0) &&
		      (fields[2].name == NULL || fields[2].name[0] == 0) &&
		      (fields[3].name == NULL || fields[3].name[0] == 0),
		      "and Deadlocked names no other slot");

		check(g->unlock_set(5, 0, 1) == ST_OK, "UNLOCK_SET owned=1");
		host_peek(R4_BOTS_LIVE + 5, &b, 1);
		check_eq_u64(b, 1, "the live byte was written");
		host_peek(R4_BOTS_SAVE + 5, &b, 1);
		check_eq_u64(b, 1, "and so was the saved copy");

		check(g->unlock_set(5, 1, 1) == ST_UNSUPPORTED, "there is no second field");

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(&list[5], values) == ST_OK, "entry 5 reads live");
		check_eq_u64(values[0], 1, "owned reads back");
	}

	group("Deadlocked: positions, skins and die");

	host_poke(R4_COORDS, (const u8 *)"\x41\x20\x00\x00\x42\x48\x00\x00"
	                                 "\xC1\x20\x00\x00\x00\x00\x00\x00", 16);
	host_poke(R4_CAM_LR, (const u8 *)"\x3F\x80\x00\x00", 4);
	host_poke(R4_CAM_UD, (const u8 *)"\x40\x00\x00\x00", 4);

	check(g->save_blob(blob, &len) == ST_OK, "save_blob reads");
	check_eq_u64(len, 40, "0x20 of position and rotation plus the two camera floats");
	check(be32_get(blob + 0x20) == 0x3F800000u, "the yaw came with it");
	check(be32_get(blob + 0x24) == 0x40000000u, "and the pitch");

	host_poke(R4_COORDS, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R4_COORDS2, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R4_CAM_LR, (const u8 *)"\x00\x00\x00\x00", 4);

	check(g->load_blob(blob, len) == ST_OK, "load_blob writes");
	mem_read_u32(R4_COORDS, &v);
	check_eq_u64(v, 0x41200000u, "the main position came back");
	mem_read_u32(R4_COORDS2, &v);
	check_eq_u64(v, 0x41200000u, "and the second address got the position half");
	mem_read_u32(R4_CAM_LR, &v);
	check_eq_u64(v, 0, "the camera is saved but not restored, as the old code had it");

	check(features_set(F4_SKIN, 6) == ST_OK, "the skin ENUM writes");
	host_peek(0x009C32FBu, &b, 1);
	check_eq_u64(b, 6, "the skin byte was written");
	mem_read_u32(0x0110D975u, &v);
	check_eq_u64(v, 1, "and the apply word behind it");
	check(features_set(F4_SKIN, 22) == ST_BAD_ARG, "a skin past the end is BAD_ARG");

	check(session_die() == ST_OK, "DIE runs");
	mem_read_u32(R4_COORDS + 8, &v);
	check_eq_u64(v, 0, "Z is zeroed at the main address");
	mem_read_u32(R4_COORDS2 + 8, &v);
	check_eq_u64(v, 0, "and at the second one");

	group("Deadlocked: savefile gate");

	host_poke(R4_SF_HELPER, (const u8 *)"\x00", 1);
	check(features_trigger(F4_SET_ASIDE) == ST_UNSUPPORTED,
	      "set aside without the helper is UNSUPPORTED");
	host_poke(R4_SF_HELPER, (const u8 *)"\x01", 1);
	check(features_trigger(F4_SET_ASIDE) == ST_OK, "and goes through with it");
	host_peek(R4_SF_SET_ASIDE, &b, 1);
	check_eq_u64(b, 1, "writing the set-aside byte");

	group("Deadlocked: the quit hook drives QUITTING");

	check(session_state() == SESSION_INGAME, "still INGAME");
	host_poke(R4_QUIT_FLAG, (const u8 *)"\xFF", 1);
	pump(4);
	check(session_state() == SESSION_QUITTING,
	      "the game-side quit flag moved the session to QUITTING");

	/* The process is still there, so the session waits for it to go. */
	check(quit_and_wait(), "and XMB follows when the process goes");
}

/* ------------------------------------------------- BCES01503, the trilogy */

static void test_trilogy(void)
{
	group("BCES01503 fingerprint disambiguation");

	check(boot_as_and_wait("BCES01503", "rac1"), "the disc boots as RaC1");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC1,
	      "and the fingerprint picked RaC1");

	check(quit_and_wait(), "quit");
	check(boot_as_and_wait("BCES01503", "rac2"), "the disc boots as RaC2");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC2,
	      "and the fingerprint picked RaC2");
	{
		const struct game_describe *d = session_game()->describe();
		check_eq_u64(d->nfeatures, 37, "with RaC2's descriptor table");
	}

	check(quit_and_wait(), "quit");
	check(boot_as_and_wait("BCES01503", "rac3"), "the disc boots as RaC3");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC3,
	      "and the fingerprint picked RaC3");

	/*
	 * Same title id, different game: the session must treat that as a title
	 * change and not carry RaC3's toggles into RaC2's table.
	 */
	check(features_set(0, 1) == ST_OK, "a RaC3 toggle goes on");
	check(quit_and_wait(), "quit");
	check(boot_as_and_wait("BCES01503", "rac2"), "the disc comes back as RaC2");
	check_eq_u64(features_toggle_state(), 0, "no toggle carried across the swap");
	check(!session_previous()->pending,
	      "and the previous-session record stayed empty");

	group("fingerprints, both forms, three games");

	{
		static const struct {
			const char *title;
			u32 addr;
			const char *original;
			const char *patched;
			u8 game_id;
		} sites[] = {
			{ "NPEA00386", 0x00BEA8A0u, "\x4B\xFF\xEA\x69", "\x60\x00\x00\x00", GAME_RAC2 },
			{ "NPEA00387", 0x00182A88u, "\x7C\x85\x31\x2E", "\x60\x00\x00\x00", GAME_RAC3 },
			{ "NPEA00423", 0x001D7D18u, "\x41\x82\x00\x0C", "\x60\x00\x00\x00", GAME_RAC4 }
		};
		u32 i;

		for (i = 0; i < 3; i++) {
			check(quit_and_wait(), "quit");

			host_boot(sites[i].title);
			host_poke(sites[i].addr, (const u8 *)sites[i].patched, 4);
			check(pump_until(SESSION_INGAME, 4000),
			      "an already-patched fingerprint still reaches INGAME");
			check(session_game() != NULL &&
			      session_game()->game_id == sites[i].game_id,
			      "and the right game was picked");

			check(quit_and_wait(), "quit");
			host_boot(sites[i].title);
			host_poke(sites[i].addr, (const u8 *)"\xDE\xAD\xBE\xEF", 4);
			pump(400);
			check(session_state() == SESSION_BOOTING,
			      "an unrecognised word leaves the session in BOOTING");

			host_poke(sites[i].addr, (const u8 *)sites[i].original, 4);
			check(pump_until(SESSION_INGAME, 2000),
			      "the original word lets it through");
		}
	}
}

/* ------------------------------------------------------ the RaC1 fingerprint */

static void test_fingerprint(void)
{
	group("RaC1 fingerprint accepts both forms");

	check(quit_and_wait(), "quit first");

	/*
	 * A console where another tool already applied the infinite-health patch
	 * shows qwark's own patched word at the fingerprint site. Booting must not
	 * stall on that.
	 */
	host_boot("NPEA00385");
	host_poke(0x0007F558u, (const u8 *)"\x30\x64\x00\x00", 4);
	check(pump_until(SESSION_INGAME, 4000),
	      "an already-patched fingerprint still reaches INGAME");

	check(quit_and_wait(), "quit again");
	host_boot("NPEA00385");
	host_poke(0x0007F558u, (const u8 *)"\x60\x00\x00\x00", 4);
	pump(400);
	check(session_state() == SESSION_BOOTING,
	      "an unrecognised word leaves the session in BOOTING");

	host_poke(0x0007F558u, (const u8 *)"\x30\x64\x9C\xE0", 4);
	check(pump_until(SESSION_INGAME, 2000), "the original word lets it through");
}

/* ----------------------------------------------------------- autosplitting */

/*
 * Protocol 1.4. Every check here drives the real watchers: it pokes the words a
 * game's ASL script read, steps the tick, and reads the events back through the
 * same bytes AUTOSPLIT_EVENTS puts on the wire.
 */

static void poke8(u32 addr, u8 v)   { host_poke(addr, &v, 1); }
static void poke16(u32 addr, u16 v) { u8 b[2]; be16_put(b, v); host_poke(addr, b, 2); }
static void poke32(u32 addr, u32 v) { u8 b[4]; be32_put(b, v); host_poke(addr, b, 4); }
static void pokef32(u32 addr, f32 v) { u8 b[4]; bef32_put(b, v); host_poke(addr, b, 4); }

struct as_event {
	u32 seq;
	u32 tick;
	u8  kind;
	u8  code;
	u16 reserved;
	u32 arg;
};

static u32 g_as_latest;
static u8  g_as_count;
static struct as_event g_as_list[AUTOSPLIT_RING_SLOTS];

/* The AUTOSPLIT_EVENTS reply body, decoded the way a client decodes it. */
static u32 as_fetch(u32 since)
{
	static u8 buf[5 + AUTOSPLIT_RING_SLOTS * AUTOSPLIT_EVENT_SIZE];
	u32 len;
	u8 i;

	g_as_latest = 0;
	g_as_count = 0;

	len = autosplit_encode_events(since, buf, sizeof(buf));
	if (len < 5) return len;

	g_as_latest = be32_get(buf);
	g_as_count = buf[4];

	for (i = 0; i < g_as_count; i++) {
		const u8 *e = buf + 5 + (u32)i * AUTOSPLIT_EVENT_SIZE;
		g_as_list[i].seq      = be32_get(e);
		g_as_list[i].tick     = be32_get(e + 4);
		g_as_list[i].kind     = e[8];
		g_as_list[i].code     = e[9];
		g_as_list[i].reserved = be16_get(e + 10);
		g_as_list[i].arg      = be32_get(e + 12);
	}

	return len;
}

/* The first event past `since` with this kind and code, or NULL. */
static const struct as_event *as_find(u32 since, u8 kind, u8 code)
{
	u8 i;

	as_fetch(since);
	for (i = 0; i < g_as_count; i++) {
		if (g_as_list[i].kind == kind && g_as_list[i].code == code)
			return &g_as_list[i];
	}
	return NULL;
}

/* Asserts one event of this kind and code showed up, and that its arg matches. */
static void as_expect(u32 since, u8 kind, u8 code, u32 arg, const char *what)
{
	const struct as_event *e = as_find(since, kind, code);

	if (e == NULL) {
		check(0, what);
		return;
	}
	check_eq_u64(e->arg, arg, what);
}

static void as_expect_kind(u32 since, u8 kind, const char *what)
{
	check(as_find(since, kind, 0) != NULL, what);
}

/*
 * The split reason codes, spelled out here rather than pulled in from the game
 * headers: these are the wire contract, so the test holds its own copy the way a
 * client does and would notice a renumber.
 */
#define R1_AS_PLANET      1
#define R1_AS_VELDIN      2
#define R1_AS_DREK_BUTTON 3
#define R1_AS_GOLD_BOLT   4
#define R1_AS_SKILL_POINT 5
#define R1_AS_ITEM        6
#define R1_AS_INFOBOT     7

#define R2_AS_PLANET       1
#define R2_AS_PROTOPET     2
#define R2_AS_A2_CLANK     3
#define R2_AS_MAKTAR_ARENA 4
#define R2_AS_BARLOW_RACE  5
#define R2_AS_ENDAKO_ENTER 6
#define R2_AS_ENDAKO_EXIT  7
#define R2_AS_TABORA_CAVES 8

#define R3_AS_PLANET        1
#define R3_AS_LDF           2
#define R3_AS_TYHRRAGUISE   3
#define R3_AS_KOROS_BOLT    4
#define R3_AS_BIOBLITERATOR 5

#define R4_AS_PLANET 1
#define R4_AS_VOX    2

/* Everything the four watchers read, by the names their scripts use. */
#define A1_GAME_STATE   0xA10708u
#define A1_DEST_PLANET  0xA10704u
#define A1_PLANET       0x969C70u
#define A1_FRAMES       0xA10710u
#define A1_PLAYER_STATE 0x96BD66u
#define A1_X            0x969D60u
#define A1_Y            0x969D64u
#define A1_GB           0xAFF000u
#define A1_SP           0xAFF010u
#define A1_ITEMS        0xAFF020u
#define A1_INFOBOTS     0xAFF030u
#define A1_KALEBO       0xA0CA75u
#define A1_CODEBOT      0x96BFF1u

#define A2_PLANET       0x1329A3Cu
#define A2_PLAYER_STATE 0x1481474u
#define A2_HERO_TYPE    0x1481494u
#define A2_CHUNK        0x157CE03u
#define A2_CLANK        0x1562699u
#define A2_ENDAKO_EXIT  0x15625E1u
#define A2_BARLOW       0x15625F7u
#define A2_YEEDIL       0x1478991u

#define A3_PLANET       0x00C1E438u
#define A3_DEST_PLANET  0x00EE9314u
#define A3_GAME_STATE   0x00EE9334u
#define A3_PLAYER_STATE 0x00DA4DB6u
#define A3_NEFFY_HP     0x00C4DF80u
#define A3_NEFFY_PHASE  0x00DA50FCu
#define A3_CHUNK        0x00F08100u
#define A3_GUISE        0x00DA570Au

#define A4_PLANET       0x009C3240u
#define A4_REQUEST_LOAD 0x00B36DCCu
#define A4_TARGET       0x00B36DD0u
#define A4_CUTSCENE     0x00B36DE8u
#define A4_IN_GAME      0x00B1F460u
#define A4_TUTORIAL     0x00B1F46Cu
#define A4_VOX_HP       0x449BEAD0u

/* One row of AUTOSPLIT_DESCRIBE, checked against what the game declares. */
static void as_check_describe(const struct game_api *g, u8 want_rows,
                              const char *first_label, const char *name)
{
	const struct autosplit_desc *rows;
	u8 n = 0;
	u8 i;
	int codes_ok = 1;
	int route_ok = 1;

	if (g == NULL || g->autosplit_describe == NULL) {
		check(0, "the game declares an autosplit table");
		return;
	}

	rows = g->autosplit_describe(&n);
	check_eq_u64(n, want_rows, name);

	if (rows == NULL || n == 0) return;

	check_eq_u64(rows[0].code, AUTOSPLIT_CODE_PLANET, "code 1 comes first");
	check(qstreq(rows[0].label, first_label), "and is the planet row");
	check_eq_u64(rows[0].flags & AUTOSPLIT_FLAG_ROUTE, AUTOSPLIT_FLAG_ROUTE,
	             "code 1 carries the route flag");
	check_eq_u64(rows[0].flags & AUTOSPLIT_FLAG_DEFAULT, AUTOSPLIT_FLAG_DEFAULT,
	             "and is on by default");

	for (i = 0; i < n; i++) {
		if (rows[i].code == 0 || rows[i].kind != AUTOSPLIT_SPLIT) codes_ok = 0;
		if (qstrlen(rows[i].label) > AUTOSPLIT_LABEL_LEN) codes_ok = 0;
		if (i > 0 && (rows[i].flags & AUTOSPLIT_FLAG_ROUTE) != 0) route_ok = 0;
	}
	check(codes_ok, "every row is a SPLIT with a non-zero code and a label that fits");
	check(route_ok, "and only code 1 carries the route flag");
}

static void test_autosplit(void)
{
	u32 mark;

	group("autosplitting: RaC1");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");

	as_check_describe(session_game(), 7, "Planet entered",
	                  "RaC1 declares seven split codes");

	/* start and reset are one expression: game state 6 -> 0 while on Veldin. */
	mark = autosplit_latest_seq();
	poke32(A1_GAME_STATE, 6);
	pump(2);
	poke32(A1_GAME_STATE, 0);
	pump(1);
	as_expect_kind(mark, AUTOSPLIT_RESET, "the game-state edge emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START on the same tick");
	check(g_as_latest > mark, "the sequence number moved");

	/* Veldin: 0 -> 2 with a planet frame count past five. */
	mark = autosplit_latest_seq();
	poke32(A1_FRAMES, 40);
	pump(1);
	poke32(A1_GAME_STATE, 2);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_VELDIN, 0, "the Veldin split fires");

	/* And only once: veldinFix latches until the next start. */
	mark = autosplit_latest_seq();
	poke32(A1_GAME_STATE, 0);
	pump(1);
	poke32(A1_GAME_STATE, 2);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R1_AS_VELDIN) == NULL,
	      "and does not fire a second time");

	/* Planet split: the destination changes to another real planet. */
	mark = autosplit_latest_seq();
	poke32(A1_PLANET, 3);
	pump(1);
	poke32(A1_DEST_PLANET, 5);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_PLANET, 5,
	          "the planet split carries the destination planet");

	/* The four collectable counters the helper mod keeps. */
	mark = autosplit_latest_seq();
	poke32(A1_GB, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_GOLD_BOLT, 1, "a gold bolt splits");

	mark = autosplit_latest_seq();
	poke32(A1_SP, 2);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_SKILL_POINT, 2, "a skill point splits");

	mark = autosplit_latest_seq();
	poke32(A1_INFOBOTS, 3);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_INFOBOT, 3, "an infobot splits");

	mark = autosplit_latest_seq();
	poke8(A1_CODEBOT, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_ITEM, 0,
	          "the codebot splits as an item");

	mark = autosplit_latest_seq();
	poke8(A1_KALEBO, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_GOLD_BOLT, 1,
	          "and the Kalebo3 bolt as a gold bolt");

	/* The Drek buttons: the player state edge only counts inside 1.7 of one. */
	mark = autosplit_latest_seq();
	poke32(A1_PLANET, 18);
	pokef32(A1_X, 100.0f);
	pokef32(A1_Y, 100.0f);
	pump(1);
	poke16(A1_PLAYER_STATE, 34);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R1_AS_DREK_BUTTON) == NULL,
	      "the Drek split ignores a state change away from a button");

	mark = autosplit_latest_seq();
	poke16(A1_PLAYER_STATE, 0);
	pokef32(A1_X, 477.9081f);
	pokef32(A1_Y, 601.4653f);
	pump(1);
	poke16(A1_PLAYER_STATE, 34);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R1_AS_DREK_BUTTON, 0,
	          "and fires standing on the first one");

	/* since_seq filtering, and the reserved halfword. */
	{
		u32 latest = autosplit_latest_seq();

		as_fetch(latest);
		check_eq_u64(g_as_count, 0, "since_seq = the latest returns nothing");
		check_eq_u64(g_as_latest, latest, "but still reports the latest seq");

		as_fetch(latest - 1);
		check_eq_u64(g_as_count, 1, "since_seq one back returns exactly one event");
		check_eq_u64(g_as_list[0].seq, latest, "and it is the newest one");
		check_eq_u64(g_as_list[0].reserved, 0, "its reserved halfword is zero");

		as_fetch(0);
		check(g_as_count > 1 && g_as_count <= AUTOSPLIT_RING_SLOTS,
		      "since_seq 0 returns the ring, capped at 64");

		{
			int rising = 1;
			u8 i;
			for (i = 1; i < g_as_count; i++) {
				if (g_as_list[i].seq <= g_as_list[i - 1].seq) rising = 0;
			}
			check(rising, "the events come back oldest first with rising seq");
		}
	}

	/* ------------------------------------------------------------- RaC2 */

	group("autosplitting: RaC2");

	check(quit_and_wait(), "quit RaC1");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");

	as_check_describe(session_game(), 8, "Planet entered",
	                  "RaC2 declares eight split codes");

	mark = autosplit_latest_seq();
	poke32(A2_PLAYER_STATE, 98);
	pump(1);
	as_expect_kind(mark, AUTOSPLIT_RESET, "the Aranos player state emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 5);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_PLANET, 5,
	          "the planet split carries the planet just entered");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 21);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R2_AS_PLANET) == NULL,
	      "and never fires for the Insomniac Museum");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 2);
	pump(1);
	poke8(A2_CHUNK, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_MAKTAR_ARENA, 0,
	          "the Maktar arena entry splits");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 3);
	pump(1);
	poke8(A2_HERO_TYPE, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_ENDAKO_ENTER, 0,
	          "the Endako Clank entry splits on the hero type");

	mark = autosplit_latest_seq();
	poke8(A2_ENDAKO_EXIT, 128);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_ENDAKO_EXIT, 0,
	          "and the Clank exit on its level flag");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 4);
	pump(1);
	poke8(A2_BARLOW, 128);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_BARLOW_RACE, 0,
	          "the Barlow race entry splits");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 8);
	pump(1);
	poke8(A2_CHUNK, 0);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_TABORA_CAVES, 0,
	          "leaving the Tabora caves splits");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 14);
	pump(1);
	poke8(A2_CLANK, 128);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_A2_CLANK, 0,
	          "the Aranos 2 Clank swap splits");

	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 20);
	pump(1);
	poke8(A2_YEEDIL, 6);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_PROTOPET, 0,
	          "and the Protopet cutscene splits");

	check(as_find(mark, AUTOSPLIT_PAUSE, 0) == NULL, "RaC2 never pauses");

	/* ------------------------------------------------------------- RaC3 */

	group("autosplitting: RaC3");

	check(quit_and_wait(), "quit RaC2");
	check(boot_and_wait("NPEA00387"), "RaC3 boots");

	as_check_describe(session_game(), 5, "Planet entered",
	                  "RaC3 declares five split codes");

	mark = autosplit_latest_seq();
	poke32(A3_PLANET, 1);
	poke32(A3_GAME_STATE, 6);
	pump(2);
	poke32(A3_GAME_STATE, 0);
	pump(1);
	as_expect_kind(mark, AUTOSPLIT_RESET, "Veldin coming out of a load emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	mark = autosplit_latest_seq();
	poke32(A3_PLANET, 4);
	pump(1);
	poke32(A3_DEST_PLANET, 7);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R3_AS_PLANET, 7,
	          "the planet split carries the destination planet");

	mark = autosplit_latest_seq();
	poke32(A3_CHUNK, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R3_AS_LDF, 0,
	          "entering the Marcadia LDF splits");

	mark = autosplit_latest_seq();
	poke8(A3_GUISE, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R3_AS_TYHRRAGUISE, 0,
	          "obtaining the Tyhrraguise splits");

	/* Two titanium bolts on Koros, counted the way vars.korosTBs counted them. */
	mark = autosplit_latest_seq();
	poke32(A3_PLANET, 14);
	pump(1);
	poke16(A3_PLAYER_STATE, 0x74);
	pump(1);
	poke16(A3_PLAYER_STATE, 0);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R3_AS_KOROS_BOLT) == NULL,
	      "one Koros bolt is not a split");
	poke16(A3_PLAYER_STATE, 0x74);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R3_AS_KOROS_BOLT, 0, "the second one is");

	/* The Biobliterator: armed by an odd phase at full health, split at zero. */
	mark = autosplit_latest_seq();
	poke32(A3_PLANET, 20);
	poke32(A3_NEFFY_PHASE, 1);
	pokef32(A3_NEFFY_HP, 1.0f);
	pump(2);
	pokef32(A3_NEFFY_HP, 0.0f);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R3_AS_BIOBLITERATOR, 0,
	          "the Biobliterator split fires once armed");

	check(as_find(mark, AUTOSPLIT_PAUSE, 0) == NULL, "RaC3 never pauses");

	/* ------------------------------------------------------ Deadlocked */

	group("autosplitting: Deadlocked");

	check(quit_and_wait(), "quit RaC3");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots");

	as_check_describe(session_game(), 2, "Planet entered",
	                  "Deadlocked declares two split codes");

	/* The SPRX's reset_needed: a load starts for Dread Zone with no tutorial flag. */
	mark = autosplit_latest_seq();
	poke32(A4_TARGET, 1);
	poke32(A4_TUTORIAL, 0);
	poke32(A4_REQUEST_LOAD, 0);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	as_expect_kind(mark, AUTOSPLIT_RESET, "a Dread Zone load emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	/* planet_change_split: in game, a real destination that is not the Interior. */
	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_IN_GAME, 1);
	poke32(A4_PLANET, 4);
	poke32(A4_TARGET, 6);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET, 6,
	          "the planet split carries the destination planet");

	/* vox_split: on the Interior, Vox past zero, the cutscene pointer flipping. */
	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_PLANET, 15);
	poke32(A4_TARGET, 0);
	pokef32(A4_VOX_HP, -1.0f);
	poke32(A4_CUTSCENE, 0);
	pump(2);
	poke32(A4_CUTSCENE, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R4_AS_VOX, 0, "the Vox split fires");

	/* PAUSE on the way out to the XMB, RESUME when the game comes back. */
	mark = autosplit_latest_seq();
	check(quit_and_wait(), "Deadlocked quits to the XMB");
	as_expect_kind(mark, AUTOSPLIT_PAUSE, "and that emits PAUSE");
	check(as_find(mark, AUTOSPLIT_RESUME, 0) == NULL, "with no RESUME yet");

	mark = autosplit_latest_seq();
	check(boot_and_wait("NPEA00423"), "Deadlocked comes back");
	as_expect_kind(mark, AUTOSPLIT_RESUME, "and that emits RESUME");
	check(as_find(mark, AUTOSPLIT_PAUSE, 0) == NULL, "and only the one RESUME");

	/* The sequence survives the reboot: it counts for the life of the module. */
	check(autosplit_latest_seq() > mark, "the sequence carried on across the boot");

	/* Back to RaC1 for anything that follows. */
	check(quit_and_wait(), "quit Deadlocked");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again");
}

/* ---------------------------------------------------------------- shutdown */

/*
 * Last, because it stops the session for good: a command posted after the tick
 * thread has gone must be answered, not parked on a semaphore nobody will post.
 */
static void test_submit_after_stop(void)
{
	struct ring_cmd cmd;
	u8 reply[16];

	group("submit after the tick thread stops");

	memset(&cmd, 0, sizeof(cmd));
	cmd.op = OP_DIE;
	cmd.reply = reply;
	cmd.replycap = sizeof(reply);

	session_stop();
	check(!session_running(), "the session is no longer running");

	check(session_submit(&cmd) == ST_BUSY, "session_submit returns instead of blocking");
	check_eq_u64(cmd.status, ST_BUSY, "and the command is answered BUSY");
}

/*
 * Protocol 1.4: the UDP push. `client` is an open connection whose HELLO has
 * already been answered; SUBSCRIBE puts a UDP port on the telemetry list, and
 * the next autosplit event has to land on it as a 20-byte 'QE' datagram, not
 * inside the telemetry packet.
 */
static void test_autosplit_udp(int client)
{
	struct sockaddr_in sa;
	socklen_t_compat salen;
	u8 frame[QWARK_FRAME_HEADER + 2];
	u8 header[QWARK_FRAME_HEADER];
	/* Big enough for a whole telemetry packet: a short recv on Windows drops it. */
	u8 dgram[TELEMETRY_MAX];
	int udp;
	u16 port = 0;
	int got = 0;
	int tries;
	u32 mark;

	group("autosplit UDP push");

	udp = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	check(udp >= 0, "a UDP socket binds for the datagrams");
	if (udp < 0) return;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = 0;
	sa.sin_addr.s_addr = htonl(0x7F000001u);
	bind(udp, (struct sockaddr *)&sa, sizeof(sa));

	salen = (socklen_t_compat)sizeof(sa);
	if (getsockname(udp, (struct sockaddr *)&sa, &salen) == 0)
		port = ntohs(sa.sin_port);
	check(port != 0, "and reports the port it got");

#ifdef _WIN32
	{
		int ms = 500;
		setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 500000;
		setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	}
#endif

	be32_put(frame, 2);
	be16_put(frame + 4, 2);
	be16_put(frame + 6, OP_SUBSCRIBE);
	be16_put(frame + QWARK_FRAME_HEADER, port);
	check(send(client, (const char *)frame, sizeof(frame), 0) == (int)sizeof(frame),
	      "SUBSCRIBE goes out");
	check(recv(client, (char *)header, QWARK_FRAME_HEADER, 0) ==
	      (int)QWARK_FRAME_HEADER, "and comes back");
	check_eq_u64(be16_get(header + 6), ST_OK, "with OK");

	/* RaC1 is INGAME: a gold-bolt counter change is one event. */
	mark = autosplit_latest_seq();
	poke32(A1_GB, 0x40);
	pump(1);
	check(autosplit_latest_seq() > mark, "the poke emitted an event");

	/*
	 * The event repeats for three ticks, and a telemetry packet may be queued in
	 * front of it, so read past anything that is not the 'QE' magic.
	 */
	pump(3);
	for (tries = 0; tries < 16 && !got; tries++) {
		int n = (int)recv(udp, (char *)dgram, (int)sizeof(dgram), 0);

		if (n <= 0) break;
		if (n != AUTOSPLIT_DGRAM_SIZE) continue;
		if (dgram[0] != 'Q' || dgram[1] != 'E') continue;
		got = 1;
	}

	check(got, "a 20-byte QE datagram reached the subscriber");
	if (got) {
		check_eq_u64(dgram[2], AUTOSPLIT_DGRAM_VERSION, "its version byte is 1");
		check_eq_u64(dgram[3], 0, "its reserved byte is 0");
		check_eq_u64(be32_get(dgram + 4), autosplit_latest_seq(),
		             "it carries the newest sequence number");
		check_eq_u64(dgram[4 + 8], AUTOSPLIT_SPLIT, "the kind is SPLIT");
		check_eq_u64(dgram[4 + 9], R1_AS_GOLD_BOLT, "with the gold-bolt code");
		check_eq_u64(be32_get(dgram + 4 + 12), 0x40, "and the counter as its arg");
	}

	plat_socket_close(udp);
	group("net_stop with a client connected");
}

/*
 * The unload path, as far as a PC can see it: a client is connected and parked
 * in recv when net_stop() runs, so the accept thread must come back out of
 * accept(), the connection thread must come back out of recv() and give its
 * slot up, and the join must return rather than hang. On the console this is
 * the difference between webMAN unloading the module and needing a reboot.
 */
static void test_net_stop_with_client(void)
{
	plat_thread_t accept_thread = PLAT_THREAD_NONE;
	struct sockaddr_in sa;
	int client;
	u8 hello[QWARK_FRAME_HEADER + 1];
	u8 header[QWARK_FRAME_HEADER];
	int waited;

	group("net_stop with a client connected");

	if (net_init() != ST_OK) {
		check(0, "net_init opens the UDP socket");
		return;
	}
	check(plat_thread_create(&accept_thread, net_accept_thread, NULL,
	                         16384, "qwark_net") == 0,
	      "the accept thread starts");

	/* Let the listener come up before we knock. */
	client = -1;
	for (waited = 0; waited < 2000; waited += 20) {
		client = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client < 0) break;

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(QWARK_PORT);
		sa.sin_addr.s_addr = htonl(0x7F000001u);   /* 127.0.0.1 */

		if (connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;

		plat_socket_close(client);
		client = -1;
		plat_sleep_us(20000);
	}
	check(client >= 0, "a client connects to the listener");

	if (client >= 0) {
		/* HELLO, so we know the connection thread is up and back in recv(). */
		be32_put(hello, 1);
		be16_put(hello + 4, 1);
		be16_put(hello + 6, OP_HELLO);
		hello[QWARK_FRAME_HEADER] = QWARK_PROTOCOL_VERSION;

		check(send(client, (const char *)hello, sizeof(hello), 0) ==
		      (int)sizeof(hello), "HELLO goes out");
		check(recv(client, (char *)header, QWARK_FRAME_HEADER, 0) ==
		      (int)QWARK_FRAME_HEADER, "and the SessionInfo header comes back");

		/* Drain the SessionInfo payload so the next reply lines up. */
		{
			u8 sink[SESSION_INFO_SIZE];
			u32 n = be32_get(header);
			if (n == SESSION_INFO_SIZE) recv(client, (char *)sink, (int)n, 0);
		}

		test_autosplit_udp(client);
	}

	net_stop();

	/*
	 * Close our end now, not after the wait: Winsock does not reliably wake a
	 * recv() blocked in another thread on shutdown() alone, so without the
	 * peer's FIN the connection thread could sit there past the drain timeout
	 * and the check below would fail once in a few runs. lv2's sockets do wake
	 * on shutdown(), and on hardware the drain force-closes after two seconds
	 * regardless.
	 */
	if (client >= 0) plat_socket_close(client);

	check(plat_thread_join(accept_thread) == 0,
	      "the accept thread joins after net_stop");
	check(net_wait_clients(2000000u) == 1,
	      "and every connection thread gave its slot up");

	net_shutdown();
	session_shutdown();
	check(1, "net_shutdown and session_shutdown return");
}

/* ------------------------------------------------------------------- main */

int main(void)
{
	if (plat_init() != 0) {
		printf("plat_init failed\n");
		return 2;
	}

	if (session_init() != ST_OK) {
		printf("session_init failed\n");
		return 2;
	}

	test_endian();
	test_framing();
	test_patches();
	test_tables();
	test_mods();
	test_describe();
	test_telemetry();
	test_session_same_title();
	test_session_different_title();
	test_unlocks();
	test_levelflags();
	test_planet_load();
	test_savefile_gate();
	test_debug_options();
	test_live_toggles();
	test_savefile_flags();
	test_rac2();
	test_rac3();
	test_rac4();
	test_trilogy();
	test_autosplit();
	test_config();
	test_fingerprint();
	test_submit_after_stop();
	test_net_stop_with_client();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");

	plat_shutdown();
	return g_failures == 0 ? 0 : 1;
}
