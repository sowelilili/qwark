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
#include "../src/core/savefile.h"
#include "../src/games/game.h"
#include "../src/games/sfhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/plat_net.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;
static const char *g_group = "";

void group(const char *name)
{
	g_group = name;
	printf("--- %s\n", name);
}

void check(int ok, const char *what)
{
	g_checks++;
	if (ok) {
		printf("  PASS  %s\n", what);
	} else {
		g_failures++;
		printf("  FAIL  %s (%s)\n", what, g_group);
	}
}

void check_eq_u64(u64 got, u64 want, const char *what)
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
	for (i = 0; i < ticks; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

/*
 * Steps until the session reaches `want` or we give up. Returns 1 on success.
 *
 * Advance the host clock as well as the tick, without sleeping through boots.
 */
static int pump_until(u8 want, int max_ticks)
{
	int i;

	for (i = 0; i < max_ticks; i++) {
		pump(1);
		if (session_state() == want) return 1;
	}

	return session_state() == want;
}

static int boot_and_wait(const char *title)
{
	host_boot(title);
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

/* ------------------------------------------------------ patch write order */

/* The logged write that covers addr, or -1. */
static int write_covering(u32 addr)
{
	u32 i, a, len;

	for (i = 0; i < host_write_log_count(); i++) {
		if (host_write_log_at(i, &a, &len) != 0) break;
		if (addr - a < len) return (int)i;
	}
	return -1;
}

static u32 write_len_at(int index)
{
	u32 a = 0, len = 0;
	if (index < 0 || host_write_log_at((u32)index, &a, &len) != 0) return 0;
	return len;
}

/* A hook listed ahead of the three-word trampoline it branches into. */
#define ORDER_HOOK  0x00510000u
#define ORDER_TRAMP 0x00520000u

static const struct patch_word order_hook_words[] = {
	{ ORDER_HOOK,        0x48010000u },     /* b ORDER_TRAMP */
	{ ORDER_TRAMP + 0,   0x38600001u },     /* li r3, 1 */
	{ ORDER_TRAMP + 4,   0x60000000u },     /* nop */
	{ ORDER_TRAMP + 8,   0x4BFEFFFCu }      /* b ORDER_HOOK + 4, outside the patch */
};
static u32 order_hook_originals[4];
static struct patch_def order_hook = {
	"order: hook first", PATCH_KIND_CLIENT, order_hook_words, 4, order_hook_originals
};

/* A ba into B, whose beq lands in C: listed A, B, C, it can only go in C, B, A. */
#define ORDER_A 0x00530000u
#define ORDER_B 0x00530100u
#define ORDER_C 0x00530200u

static const struct patch_word order_chain_words[] = {
	{ ORDER_A,     0x48530102u },           /* ba ORDER_B */
	{ ORDER_B,     0x60000000u },
	{ ORDER_B + 4, 0x41820100u },           /* beq ORDER_C + 4 */
	{ ORDER_C,     0x60000000u },
	{ ORDER_C + 4, 0x38600002u }
};
static u32 order_chain_originals[5];
static struct patch_def order_chain = {
	"order: chain", PATCH_KIND_CLIENT, order_chain_words, 5, order_chain_originals
};

/* Two runs that branch into each other. */
#define ORDER_D 0x00540000u
#define ORDER_E 0x00540100u

static const struct patch_word order_cycle_words[] = {
	{ ORDER_D, 0x48000100u },               /* b ORDER_E */
	{ ORDER_E, 0x4BFFFF00u }                /* b ORDER_D */
};
static u32 order_cycle_originals[2];
static struct patch_def order_cycle = {
	"order: cycle", PATCH_KIND_CLIENT, order_cycle_words, 2, order_cycle_originals
};

static void test_patch_order(void)
{
	u32 v = 0;
	int hook, tramp, a, b, c;

	group("patch write order, with nothing pausing the game");

	host_poke(ORDER_HOOK, (const u8 *)"\x7C\x08\x02\xA6", 4);
	host_poke(ORDER_TRAMP, (const u8 *)"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12);

	host_write_log_reset();
	check(patch_apply(&order_hook) == ST_OK, "a hook listed before its trampoline applies");
	hook = write_covering(ORDER_HOOK);
	tramp = write_covering(ORDER_TRAMP);
	check_eq_u64(host_write_log_count(), 2, "in two writes, one per run of adjacent words");
	check(tramp >= 0 && hook > tramp, "the trampoline went in before the branch into it");
	check_eq_u64(write_len_at(tramp), 12, "and all three trampoline words went in together");
	mem_read_u32(ORDER_HOOK, &v);
	check_eq_u64(v, 0x48010000u, "the hook word is in");
	mem_read_u32(ORDER_TRAMP + 8, &v);
	check_eq_u64(v, 0x4BFEFFFCu, "the trampoline's last word is in");

	host_write_log_reset();
	check(patch_revert(&order_hook) == ST_OK, "it reverts");
	hook = write_covering(ORDER_HOOK);
	tramp = write_covering(ORDER_TRAMP);
	check(hook >= 0 && tramp > hook, "the branch came out before the trampoline did");
	mem_read_u32(ORDER_HOOK, &v);
	check_eq_u64(v, 0x7C0802A6u, "the hook site has its original word back");

	host_write_log_reset();
	check(patch_apply(&order_chain) == ST_OK, "a chain listed A, B, C applies");
	a = write_covering(ORDER_A);
	b = write_covering(ORDER_B);
	c = write_covering(ORDER_C);
	check(c >= 0 && b > c && a > b, "and went in C, B, A: ba and beq targets both followed");

	host_write_log_reset();
	check(patch_revert(&order_chain) == ST_OK, "the chain reverts");
	a = write_covering(ORDER_A);
	b = write_covering(ORDER_B);
	c = write_covering(ORDER_C);
	check(a >= 0 && b > a && c > b, "in A, B, C, the reverse");

	host_write_log_reset();
	check(patch_apply(&order_cycle) == ST_OK, "two runs that branch into each other still apply");
	check_eq_u64(host_write_log_count(), 2, "each written once");
	check(write_covering(ORDER_D) == 0, "in the order they were listed");
	check(patch_revert(&order_cycle) == ST_OK, "and revert");
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

	/*
	 * quartu_patch lists its two hooks, ba 0x224860 and ba 0x224870, ahead of the
	 * ten words at 0x22485C they land in. Nothing pauses the game while a mod
	 * loads, so the order is all that keeps the hooks off missing code.
	 */
	{
		int tramp, hook1, hook2;

		host_write_log_reset();
		check(mods_load("quartu_patch") == ST_OK, "quartu_patch loads");
		tramp = write_covering(0x224860u);
		hook1 = write_covering(0xE03F4u);
		hook2 = write_covering(0x168B3Cu);
		check(tramp >= 0 && hook1 > tramp && hook2 > tramp,
		      "its trampoline went in before either hook, though listed after them");
		check_eq_u64(write_len_at(tramp), 40, "all ten trampoline words in one write");
		check(mods_unload("quartu_patch") == ST_OK, "and it unloads");
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

			host_write_log_reset();
			check(mods_load("il-ghost") == ST_OK, "loading il-ghost succeeds");
			check((mods_at((u32)dlcs)->flags & MOD_FLAG_LOADED) != 0,
			      "its dependency was loaded first");

			/* dl-cs lists b 0x3d84c8 at 0x37b9b0 ahead of the 248 words it lands in. */
			{
				int tramp = write_covering(0x3D84C8u);
				int hook = write_covering(0x37B9B0u);
				check(tramp >= 0 && hook > tramp,
				      "dl-cs's trampoline went in before the branch into it");
				check_eq_u64(write_len_at(tramp), 248 * 4, "as one write of all 248 words");
			}

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

/*
 * Failures while staging code and while changing titles must not install hooks
 * into missing code or carry the previous game's tables into a new process.
 */
static void test_launch_failures(void)
{
	u8 zero[64] = {0}, description[8192];
	u32 len, value;
	const struct game_api *candidates[GAME_MAX_CANDIDATES];
	const struct patch_word words[] = {{0x1000u, 0x48001000u}, {0x2000u, 0x4E800020u}};
	u32 originals[2];
	struct patch_def def = {"failure test", PATCH_KIND_CLIENT, words, 2, originals};
	plat_file_t f;
	const char *path = "/dev_hdd0/qwark/mods/NPEA00385/failure-test/patch.txt";
	const char patch[] = "0x3000: missing.bin\n0x4000: 0x4bfff000\n";

	group("failed code writes and aborted launches");
	check(quit_and_wait() && boot_and_wait("NPEA00385"), "RaC1 starts for failure checks");
	host_poke(0x1000, zero, 4);
	host_poke(0x2000, zero, 4);
	host_fail_writes(0x2000, 1);
	check(patch_apply(&def) == ST_IO_ERROR, "failed trampoline reports IO_ERROR");
	mem_read_u32(0x1000, &value);
	check_eq_u64(value, 0, "failed trampoline never enables its hook");
	check(!patch_is_applied(&def), "successful rollback drops the patch record");

	check(patch_apply(&def) == ST_OK, "patch can be applied after successful cleanup");
	host_fail_writes(0x1000, 2);
	check(patch_revert(&def) == ST_IO_ERROR, "failed hook removal reports IO_ERROR");
	mem_read_u32(0x2000, &value);
	check_eq_u64(value, 0x4E800020u, "failed hook removal leaves the trampoline intact");
	check(patch_is_applied(&def), "failed revert retains the original words and record");
	check(patch_apply(&def) == ST_IO_ERROR, "partial revert is not reported as an applied patch");
	check(patch_revert(&def) == ST_IO_ERROR, "cleanup can report another failure");
	check(patch_revert(&def) == ST_OK, "cleanup can retry once writes recover");
	mem_read_u32(0x2000, &value);
	check_eq_u64(value, 0, "retry restores the original trampoline bytes");

	host_fail_writes(0x2000, 2);
	check(client_patch_apply(words, 2) == ST_IO_ERROR, "client patch reports failed staging and cleanup");
	check(client_patch_count() == 1, "client retains storage for failed cleanup");
	check(client_patch_apply(words, 2) == ST_IO_ERROR, "client retry does not falsely succeed");
	host_fail_writes(0x2000, 1);
	check(mem_clear_client() == ST_IO_ERROR && client_patch_count() == 1,
	      "CLEAR_CLIENT preserves and reports a patch it could not remove");
	check(mem_clear_client() == ST_OK && client_patch_count() == 0, "CLEAR_CLIENT retry finishes cleanup");

	plat_dir_create("/dev_hdd0/qwark/mods/NPEA00385/failure-test");
	check(plat_file_open(path, PLAT_OPEN_WRITE, &f) == 0, "missing-cave fixture opens");
	plat_file_write(f, patch, sizeof(patch)-1);
	plat_file_close(f);
	mods_set_title("NPEA00385");
	check(mods_load("failure-test") == ST_NOT_FOUND, "missing cave refuses mod load");
	mem_read_u32(0x4000, &value);
	check_eq_u64(value, 0, "missing cave never enables its hook");
	check(!(mods_at((u32)mods_find("failure-test"))->flags & MOD_FLAG_LOADED), "missing-cave mod is not marked loaded");
	plat_file_unlink(path);
	plat_dir_remove("/dev_hdd0/qwark/mods/NPEA00385/failure-test");
	mods_set_title("NPEA00385");

	check(quit_and_wait(), "RaC1 quits");
	host_boot("NPEA00386");
	game_candidates_for_title("NPEA00386", candidates, GAME_MAX_CANDIDATES);
	host_poke(candidates[0]->fp_addr, zero, candidates[0]->fp_len);
	pump(122);
	check(session_state() == SESSION_BOOTING && qstreq(session_title(), "NPEA00386"), "RaC2 title is known but fingerprint is not ready");
	check(quit_and_wait() && boot_and_wait("NPEA00386"), "aborted RaC2 launch retries successfully");
	check(features_describe(description, sizeof(description), &len) == ST_OK && description[0] == GAME_RAC2,
	      "retried RaC2 launch replaces the feature table with RaC2's");
	check(mods_find("quartu_patch") < 0, "retried RaC2 launch drops RaC1's mod table");
	check(quit_and_wait() && boot_and_wait("NPEA00385"), "restore RaC1 for later tests");
}

static void test_boot(void)
{
	u32 reads_at_boot, writes_at_boot, titles_at_boot, sends_at_boot, pids_at_boot;
	u32 presence_at_boot;

	group("session: a boot");

	check(quit_and_wait(), "quit whatever was running");

	host_freeze_time(1);
	pids_at_boot = host_pid_calls();
	host_boot("NPEA00385");
	pump(1);
	check(session_state() == SESSION_BOOTING, "IS_INGAME puts the session in BOOTING at once");
	check_eq_u64(host_pid_calls() - pids_at_boot, 0, "even the first PID query is deferred");

	reads_at_boot = host_mem_reads();
	writes_at_boot = host_write_log_count();
	titles_at_boot = host_title_calls();
	sends_at_boot = net_telemetry_sends();
	presence_at_boot = host_presence_calls();

	/* Lots of ticks cannot shorten the measured second. */
	{
		int i;
		for (i = 0; i < 250; i++) session_step_once();
	}
	host_advance_time(999999);
	session_step_once();
	check(session_state() == SESSION_BOOTING, "still BOOTING one microsecond short of a second");
	check_eq_u64(host_mem_reads() - reads_at_boot, 0, "and the process has not been read");
	check_eq_u64(host_write_log_count() - writes_at_boot, 0, "nor written");
	check_eq_u64(host_title_calls() - titles_at_boot, 0, "nor the XMB asked for the title");
	check_eq_u64(host_pid_calls() - pids_at_boot, 0, "nor the process ID queried");
	check_eq_u64(host_presence_calls() - presence_at_boot, 0, "nor IS_INGAME polled again");
	check_eq_u64(net_telemetry_sends() - sends_at_boot, 0, "UDP telemetry is silent too");

	host_advance_time(1);
	session_step_once();
	check(session_state() == SESSION_INGAME, "the game is up when the full second is over");
	check_eq_u64(host_title_calls() - titles_at_boot, 1, "after one question to the XMB");
	check_eq_u64(host_pid_calls() - pids_at_boot, 1, "and the first PID query");
	host_freeze_time(0);
	check(host_write_log_count() == writes_at_boot, "RaC1 boot injects no helper code");

	/*
	 * A process that comes and goes inside the quiet second: a launch that
	 * failed, or a launcher handing over to the game proper. No pid was ever
	 * read, so QUITTING has no process to wait for and must not wait for one:
	 * with the gate shut, every bulk request was BUSY and every new connection
	 * was refused until the next launch happened to come along.
	 */
	check(quit_and_wait(), "quit again");
	host_freeze_time(1);
	host_boot("NPEA00385");
	pump(1);
	check(session_state() == SESSION_BOOTING, "a second boot starts its quiet second");
	host_quit();
	host_advance_time(500000);
	session_step_once();
	check(session_state() == SESSION_BOOTING, "and does not look at the process inside it");
	host_advance_time(500000);
	pump(3);
	check(session_state() == SESSION_XMB, "a process gone before its pid was read leaves the session in the XMB");
	host_freeze_time(0);
	check(boot_and_wait("NPEA00385"), "and the next launch still comes up");
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
	check_eq_u64(packet[5], 32, "and this module is build 32");
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

	/* trace_ops is on for now, for any config.txt that does not say otherwise. */
	net_set_trace_ops(0);
	check(config_load() == ST_OK, "config.txt reloads with no trace_ops key in it");
	check(net_trace_ops(), "and every request is traced by default");
	check(config_set_u32("trace_ops", 0) == ST_OK && config_load() == ST_OK && !net_trace_ops(),
	      "trace_ops = 0 turns it off");
	check(config_set_u32("trace_ops", 1) == ST_OK && config_load() == ST_OK && net_trace_ops(),
	      "and 1 turns it back on");

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

	group("CRC32");
	{
		/*
		 * The one number every CRC-32 implementation is checked against, and the
		 * one the PC client's Crc32.cs and Python's zlib.crc32 both produce. The
		 * split call is what a 2 MB save gets: summed 64 KB at a time as it goes
		 * past, never held whole.
		 */
		u32 state;

		check_eq_u64(qcrc32((const u8 *)"123456789", 9), 0xCBF43926u,
		             "the check vector is 0xcbf43926");
		check_eq_u64(qcrc32((const u8 *)"", 0), 0, "an empty buffer sums to zero");

		state = qcrc32_update(qcrc32_start(), (const u8 *)"12345", 5);
		state = qcrc32_update(state, (const u8 *)"6789", 4);
		check_eq_u64(qcrc32_finish(state), 0xCBF43926u,
		             "and summing it in two pieces gives the same answer");
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

/* ------------------------------------------- RaC1: the savefile requests */

#define A_HELPER   0xB00070u
#define A_SF_LOAD  0xB00071u
#define A_SF_ASIDE 0xB00072u
#define A_SF_AUTO  0xB00073u

/*
 * Protocol 1.9. Nobody loads a mod for the helper any more: the first action needing it in
 * a session installs it, so the actions work from a cold process and the helper
 * byte is qwark's own write rather than a gate.
 */
static void test_savefile_requests(void)
{
	u8 b = 0;

	group("RaC1 savefile requests");

	host_poke(A_HELPER, (const u8 *)"\x00\x00\x00\x00", 4);

	check(features_trigger(18) == ST_OK,
	      "load set-aside file works with nothing loaded first");
	host_peek(A_SF_LOAD, &b, 1);
	check_eq_u64(b, 1, "load writes a 1");

	check(features_trigger(19) == ST_OK, "set aside file goes through");
	host_peek(A_SF_ASIDE, &b, 1);
	check_eq_u64(b, 1, "set aside writes a 1");

	check(features_trigger(20) == ST_OK, "force autosave goes through");
	host_peek(A_SF_AUTO, &b, 1);
	check_eq_u64(b, 3, "force autosave writes a 3, not a 1");

	check(session_load_setaside() == ST_OK, "and so does the combo action");

	/*
	 * The readout mirrors the helper's own byte, which only the helper writes.
	 * The fake console runs no PowerPC, so the byte is poked here instead: what
	 * is being checked is that the readout follows it.
	 */
	host_poke(A_HELPER, (const u8 *)"\x01", 1);
	pump(20);
	{
		u8 info[SESSION_INFO_SIZE];
		session_info_copy(info, sizeof(info));
		check_eq_u64(be32_get(info + 64 + 4 * 1), 1,
		             "readout 1 reports the helper as running");
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

/* ------------------------------------- protocol 1.9, the savefile helper */

/*
 * What can and cannot be checked here.
 *
 * The helper is PowerPC code that runs inside the game, and the fake console
 * runs no PowerPC: nothing in this file executes a single instruction of it, so
 * whether it copies the right save buffer, and whether the hook site is the
 * right one, are questions only a console can answer. What is checked is
 * everything on qwark's side of that line - that installing writes the cave
 * bytes and the hook words at the addresses the generated table names, that the
 * three ops answer the way the protocol says, that a request writes the request
 * byte the helper would read, and that the aside buffer round-trips.
 */
static void savefile_one_game(const char *title, u8 game_id, u32 set_aside_id)
{
	const struct sf_desc *d = sf_desc_for_game(game_id);
	u8 supported = 0, installed = 0, running = 0, pending = 0;
	u32 size = 0;
	u8 pattern[64];
	u8 buf[64];
	u32 got = 0;
	u8 b = 0;
	u8 i;

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait(title), "the game reaches INGAME");

	check(d != NULL, "the game has an entry in the generated helper table");
	if (d == NULL) return;
	check(d->ncaves >= 1 && d->nhooks >= 1, "which names a cave and a hook word");

	/* Dirty request bytes must not masquerade as an installed helper. */
	host_poke(d->api_mod, (const u8 *)"\x01", 1);
	host_poke(d->api_load, (const u8 *)"\x01", 1);
	host_poke(d->api_setaside, (const u8 *)"\x01", 1);
	host_write_log_reset();
	check(savefile_info(&supported, &installed, &running, &pending, &size) == ST_OK,
	      "SAVEFILE_INFO answers");
	check_eq_u64(supported, 1, "the game is supported");
	check_eq_u64(installed, 0, "status queries leave the helper uninstalled");
	check_eq_u64(size, d->aside_size, "the size is the aside buffer's");
	check_eq_u64(running, 0, "nothing has executed it, so it is not running");
	check_eq_u64(pending, 0, "and no request is outstanding");

	check(savefile_info(&supported, &installed, &running, &pending, &size) == ST_OK,
	      "repeated status polling succeeds");
	check_eq_u64(host_write_log_count(), 0, "status polling writes no game memory");
	check(savefile_write(0, (const u8 *)"test", 4) == ST_OK, "a cold buffer upload succeeds");
	check(savefile_read(0, 4, buf, &got) == ST_OK && memcmp(buf, "test", 4) == 0,
	      "a cold buffer read round-trips without installing code");
	check_eq_u64(host_write_log_count(), 1, "only the uploaded data was written");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(installed, 0, "buffer I/O leaves the helper uninstalled");
	check(savefile_set_aside() == ST_OK, "the first set-aside action installs the helper");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(installed, 1, "the action reports installed");
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);

	for (i = 0; i < d->ncaves; i++) {
		u8 head[16];
		host_peek(d->caves[i].addr, head, sizeof(head));
		check(memcmp(head, d->caves[i].bytes, sizeof(head)) == 0,
		      "the cave's bytes are at the cave address");
	}

	for (i = 0; i < d->nhooks; i++) {
		u8 word[4];
		host_peek(d->hooks[i].addr, word, sizeof(word));
		check_eq_u64(be32_get(word), d->hooks[i].value,
		             "and the hook word is at the hook site");
	}

	/*
	 * api_mod is the helper's own write, once a frame. Poking it here stands in
	 * for the game having reached the hook.
	 */
	host_poke(d->api_mod, (const u8 *)"\x01", 1);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(running, 1, "api_mod reading 1 is reported as running");

	/*
	 * The two requests, the pending bits that follow them, and the settle window
	 * that keeps a bit set for a quarter of a second after the byte goes to zero.
	 * The window is counted on the tick, so the pumps below are what moves it.
	 */
	check(savefile_set_aside() == ST_OK, "the set-aside request goes out");
	host_peek(d->api_setaside, &b, 1);
	check_eq_u64(b, 1, "as a 1 in the set-aside byte");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_SET_ASIDE, "INFO says it is outstanding");

	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_SET_ASIDE,
	             "a byte that has just read zero is not the work being done");

	pump(SAVEFILE_SETTLE_TICKS - 1);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_SET_ASIDE,
	             "and neither is one tick short of the settle window");

	pump(1);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "the whole window of zeroes is what clears the bit");

	/* A byte that goes back to 1 inside the window starts the count again. */
	check(savefile_set_aside() == ST_OK, "another set-aside request");
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS - 2);
	host_poke(d->api_setaside, (const u8 *)"\x01", 1);
	pump(1);
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS - 1);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_SET_ASIDE,
	             "a byte that comes back inside the window restarts it");
	pump(1);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "and the new window is what clears the bit");

	check(savefile_load_aside() == ST_OK, "the load request goes out");
	host_peek(d->api_load, &b, 1);
	check_eq_u64(b, 1, "as a 1 in the load byte");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_LOAD, "INFO says the load is outstanding");
	host_poke(d->api_load, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "and it settles the same way");

	/* The flagged ACTION is the same request by another road. */
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	check(features_trigger((u8)set_aside_id) == ST_OK,
	      "the SAVE_ASIDE action fires");
	host_peek(d->api_setaside, &b, 1);
	check_eq_u64(b, 1, "and writes the same byte");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_SET_ASIDE,
	             "and is outstanding from the moment it is issued");
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);

	/* WRITE then READ, through the buffer the helper parks a save in. */
	for (i = 0; i < (u8)sizeof(pattern); i++) pattern[i] = (u8)(i * 7 + 1);

	check(savefile_write(0, pattern, sizeof(pattern)) == ST_OK, "a write at offset 0");
	check(savefile_read(0, sizeof(buf), buf, &got) == ST_OK, "and a read back");
	check_eq_u64(got, sizeof(buf), "of the length that was asked for");
	check(memcmp(buf, pattern, sizeof(pattern)) == 0, "the bytes round-tripped");

	host_peek(d->aside_addr, buf, sizeof(buf));
	check(memcmp(buf, pattern, sizeof(pattern)) == 0,
	      "and they are in the aside buffer, at the address the helper reads");

	check(savefile_write(d->aside_size - 4, pattern, 4) == ST_OK,
	      "a write that ends exactly at the end of the buffer is fine");
	check(savefile_write(d->aside_size - 4, pattern, 8) == ST_BAD_ARG,
	      "one that would run past it is BAD_ARG");
	check(savefile_write(d->aside_size, pattern, 4) == ST_BAD_ARG,
	      "and so is an offset at the end");
	check(savefile_write(0, pattern, 0) == ST_BAD_ARG, "an empty write is BAD_ARG");

	check(savefile_read(d->aside_size - 4, sizeof(buf), buf, &got) == ST_OK,
	      "a read that runs past the end is trimmed rather than refused");
	check_eq_u64(got, 4, "to what is left of the buffer");
	check(memcmp(buf, pattern, 4) == 0, "and it is the tail that was written");
	check(savefile_read(d->aside_size, 4, buf, &got) == ST_BAD_ARG,
	      "an offset at the end is BAD_ARG");
}

static void test_savefile_helper(void)
{
	u8 supported = 9, installed = 9, running = 9, pending = 9;
	u32 size = 9;
	u8 buf[4] = { 0 };
	u32 got = 0;

	group("protocol 1.9: RaC1's savefile helper");
	savefile_one_game("NPEA00385", GAME_RAC1, 19);

	group("protocol 1.9: RaC2's savefile helper");
	savefile_one_game("NPEA00386", GAME_RAC2, 31);

	group("protocol 1.9: RaC3's savefile helper");
	savefile_one_game("NPEA00387", GAME_RAC3, 31);

	group("protocol 1.9: Deadlocked's savefile helper");
	savefile_one_game("NPEA00423", GAME_RAC4, 13);

	/*
	 * Build 14 moved Deadlocked's helper: Bot Info and IL HUD Display fill the
	 * mod's old cave at 0x661F9C and branch out of its old hook word at
	 * 0x70719C, so the helper now has a cave of its own in dead lobby code and
	 * hooks the last instruction of the pad routine, through a stub that runs
	 * the displaced store first (src/games/sfhelper/sf_rac4_stub.s). Pinned so
	 * that a regenerated table drifting back onto the mods' addresses fails here.
	 */
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC4);

		check_eq_u64(d->ncaves, 2, "Deadlocked has a stub cave and a helper cave");
		check_eq_u64(d->caves[0].addr, 0x00667F70u, "the stub is at 0x667F70");
		check_eq_u64(d->caves[1].addr, 0x00667FB0u, "the helper follows at 0x667FB0");
		check(d->caves[0].addr + d->caves[0].len <= d->caves[1].addr,
		      "and the stub fits in front of it");
		check(d->caves[1].addr + d->caves[1].len <= 0x0066840Cu,
		      "and the helper ends inside the dead function");
		check_eq_u64(be32_get(d->caves[0].bytes), 0x9BDD0457u,
		             "the stub opens with the displaced store, stb r30,0x457(r29)");
		check_eq_u64(d->hooks[0].addr, 0x00707430u,
		             "the hook word replaces that store");
		check_eq_u64(d->hooks[0].value, 0x4BF60B41u,
		             "and is the relative bl from there to the stub");
	}

	group("protocol 1.9: the savefile ops outside INGAME");

	check(sf_desc_for_game(GAME_NONE) == NULL,
	      "no game means no helper table entry");

	check(quit_and_wait(), "the game goes away");
	check(savefile_info(&supported, &installed, &running, &pending, &size)
	      == ST_NOT_INGAME, "SAVEFILE_INFO is NOT_INGAME");
	check(savefile_read(0, 4, buf, &got) == ST_NOT_INGAME,
	      "and so is SAVEFILE_READ");
	check(savefile_write(0, buf, 4) == ST_NOT_INGAME, "and SAVEFILE_WRITE");
	check(savefile_install() == ST_NOT_INGAME, "and installing the helper");

	/*
	 * The helper is not carried across a reboot: a new process has none of it,
	 * so the next request writes it again.
	 */
	check(boot_and_wait("NPEA00385"), "RaC1 comes back");
	{
		u8 word[4];
		host_peek(sf_desc_for_game(GAME_RAC1)->hooks[0].addr, word, 4);
		check(be32_get(word) != sf_desc_for_game(GAME_RAC1)->hooks[0].value,
		      "the fresh process has no hook word in it");
		check(savefile_load_aside() == ST_OK, "a cold load action installs it again");
		host_peek(sf_desc_for_game(GAME_RAC1)->hooks[0].addr, word, 4);
		check_eq_u64(be32_get(word), sf_desc_for_game(GAME_RAC1)->hooks[0].value,
		             "and the hook word is back");
	}
}

/* ------------------------------ protocol 1.10, the console savefile library */

/*
 * What can be checked here is everything but the game: the copies run on qwark's
 * own tick thread between a file on the fake console's disk and the fake
 * process's memory, so the state machine, the chunking, the CRC, the sidecar,
 * the listing and every refusal are all exercised. What the *game* does with the
 * buffer afterwards is still a question only hardware answers.
 */

/* One 64 KB chunk of a pattern, poked straight into the aside buffer. */
static u8 g_pattern[SAVEFILE_COPY_CHUNK];

/* Fills the aside buffer with a seeded pattern and reports its CRC32. */
static u32 fill_aside(const struct sf_desc *d, u8 seed)
{
	u32 off = 0;
	u32 crc = qcrc32_start();

	while (off < d->aside_size) {
		u32 n = d->aside_size - off;
		u32 i;

		if (n > sizeof(g_pattern)) n = sizeof(g_pattern);
		for (i = 0; i < n; i++) g_pattern[i] = (u8)((off + i) * 7u + seed);

		host_poke(d->aside_addr + off, g_pattern, n);
		crc = qcrc32_update(crc, g_pattern, n);
		off += n;
	}

	return qcrc32_finish(crc);
}

/* The CRC32 and the size of a file on the fake console, 0 and 0 when missing. */
static u32 file_crc(const char *path, u32 *size_out)
{
	plat_file_t f;
	u32 crc = qcrc32_start();
	u32 total = 0;

	if (size_out != NULL) *size_out = 0;
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return 0;

	for (;;) {
		u32 got = 0;
		if (plat_file_read(f, g_pattern, sizeof(g_pattern), &got) != 0) break;
		if (got == 0) break;
		crc = qcrc32_update(crc, g_pattern, got);
		total += got;
	}

	plat_file_close(f);
	if (size_out != NULL) *size_out = total;
	return qcrc32_finish(crc);
}

/* Pumps until the transfer bit clears, or gives up so a failure is not a hang. */
static int pump_until_transfer_done(int max_ticks)
{
	u8 supported, installed, running, pending;
	u32 size;
	int i;

	for (i = 0; i < max_ticks; i++) {
		savefile_info(&supported, &installed, &running, &pending, &size);
		if ((pending & SAVEFILE_PENDING_TRANSFER) == 0) return 1;
		pump(1);
	}

	savefile_info(&supported, &installed, &running, &pending, &size);
	return (pending & SAVEFILE_PENDING_TRANSFER) == 0;
}

#define SAVE_ROOT "/dev_hdd0/qwark/savefiles/NPEA00385"

static void test_savefile_library(void)
{
	const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);
	u8 supported = 0, installed = 0, running = 0, pending = 0, error = 9;
	u32 size = 0, done = 0, total = 0;
	u8 out[2048];
	u32 len = 0;
	u32 crc_saved;
	u32 file_size = 0;
	u8 byte = 9;

	group("protocol 1.10: the console savefile library");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00385"), "RaC1 reaches INGAME");
	if (d == NULL) { check(0, "RaC1 has a helper table entry"); return; }
	host_write_log_reset();

	/* ------------------------------------------------------- categories */

	check(savefile_categories(out, sizeof(out), &len) == ST_OK,
	      "SAVEFILE_CATEGORIES answers before anything has been saved");
	check_eq_u64(out[0], 0, "with an empty library");

	check(savefile_category(SAVEFILE_CATEGORY_CREATE, "runs") == ST_OK,
	      "a category is created");
	check(savefile_categories(out, sizeof(out), &len) == ST_OK,
	      "SAVEFILE_CATEGORIES answers again");
	check_eq_u64(out[0], 1, "and reports the one category");
	check(qstreq((const char *)(out + 1), "runs"), "by name, NUL-padded to 32");
	check_eq_u64(len, 1 + SAVEFILE_NAME_LEN, "the reply is one 32-byte row");

	check(savefile_category(SAVEFILE_CATEGORY_CREATE, "../escape") == ST_BAD_ARG,
	      "a category name with a separator in it is refused");
	check(savefile_category(9, "runs") == ST_BAD_ARG, "and so is an unknown op");
	check(savefile_restore("runs", "missing.sav") == ST_NOT_FOUND,
	      "a missing restore file is refused before installation");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(installed, 0, "metadata and a refused restore leave the helper uninstalled");
	check_eq_u64(host_write_log_count(), 0, "metadata and refused restore write no game memory");

	/* ------------------------------------------------------------ STORE */

	crc_saved = fill_aside(d, 1);

	check(savefile_store("runs", "one.txt") == ST_BAD_ARG,
	      "a name that is not a .sav is refused");
	check(savefile_store("runs", "one.sav") == ST_OK, "STORE is accepted");

	savefile_info(&supported, &installed, &running, &pending, &size);
	savefile_transfer(&done, &total, &error);
	check((pending & SAVEFILE_PENDING_TRANSFER) != 0, "a transfer is in flight");
	check((pending & SAVEFILE_PENDING_SET_ASIDE) != 0,
	      "and the set-aside it raised is outstanding");
	check_eq_u64(total, d->aside_size, "total is the size of the aside buffer");
	check_eq_u64(done, 0, "and nothing has been copied yet");
	check_eq_u64(error, SAVEFILE_ERR_NONE, "the last error was cleared");

	/* Only one transfer at a time, and the library ops step aside for it. */
	check(savefile_store("runs", "two.sav") == ST_BUSY,
	      "a second STORE during one is refused");
	check(savefile_restore("runs", "one.sav") == ST_BUSY, "and so is a RESTORE");
	check(savefile_library_gate() == ST_BUSY,
	      "the listing ops answer BUSY while it runs");

	/*
	 * Nothing may be read out of the buffer until the helper has answered, so
	 * the copy does not start until the settle window is over. The helper's
	 * clearing of the byte is what host_poke stands in for.
	 */
	pump(2);
	savefile_transfer(&done, &total, &error);
	check_eq_u64(done, 0, "the copy waits for the set-aside to settle");

	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	pump(1);
	savefile_transfer(&done, &total, &error);
	check_eq_u64(done, 2 * SAVEFILE_COPY_CHUNK,
	             "then two 64 KB chunks go per tick");

	check(pump_until_transfer_done(64), "the transfer finishes");
	savefile_info(&supported, &installed, &running, &pending, &size);
	savefile_transfer(&done, &total, &error);
	check_eq_u64(pending, 0, "with nothing left outstanding");
	check_eq_u64(done, d->aside_size, "done is the whole buffer");
	check_eq_u64(error, SAVEFILE_ERR_NONE, "and no error");

	check_eq_u64(file_crc(SAVE_ROOT "/runs/one.sav", &file_size), crc_saved,
	             "the file holds the bytes the buffer held");
	check_eq_u64(file_size, d->aside_size, "and is exactly the buffer's size");

	{
		char text[32];
		char want[16];
		check(qread_file(SAVE_ROOT "/runs/one.sav.sum", text, sizeof(text), NULL) == ST_OK,
		      "the CRC sidecar is beside it");
		qfmt_hex(want, sizeof(want), crc_saved, 8);
		check(qstreq(qtrim(text), want), "holding the same eight hex digits");
	}

	/* ------------------------------------------------------------- LIST */

	check(savefile_list("runs", out, sizeof(out), &len) == ST_OK, "SAVEFILE_LIST answers");
	check_eq_u64(out[0], 1, "with the one save");
	check(qstreq((const char *)(out + 1), "one.sav"), "named as it was stored");
	check_eq_u64(be32_get(out + 1 + SAVEFILE_NAME_LEN), d->aside_size, "with its size");
	check_eq_u64(be32_get(out + 1 + SAVEFILE_NAME_LEN + 4), crc_saved, "and its CRC");
	check_eq_u64(len, 1 + SAVEFILE_ROW_SIZE, "the row is 40 bytes");

	check(savefile_list("nosuch", out, sizeof(out), &len) == ST_NOT_FOUND,
	      "a category that does not exist is NOT_FOUND");

	/* ---------------------------------------------------------- RESTORE */

	/* A different pattern first, so a restore that did nothing would show. */
	check(fill_aside(d, 9) != crc_saved, "the buffer is filled with something else");
	host_poke(d->api_load, (const u8 *)"\x00", 1);

	check(savefile_restore("runs", "one.sav") == ST_OK, "RESTORE is accepted");
	host_peek(d->api_load, &byte, 1);
	check_eq_u64(byte, 0, "and raises nothing at the game yet");

	pump(1);
	savefile_transfer(&done, &total, &error);
	check_eq_u64(done, 2 * SAVEFILE_COPY_CHUNK, "the copy starts on the next tick");

	/* It runs to the end of the file and only then asks the game to take it. */
	{
		int ticks;
		for (ticks = 0; ticks < 64; ticks++) {
			host_peek(d->api_load, &byte, 1);
			if (byte != 0) break;
			pump(1);
		}
	}
	check_eq_u64(byte, 1, "the load request goes out once the whole file is in");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(done, d->aside_size, "with every byte of it copied");

	{
		u32 crc = qcrc32_start();
		u32 off = 0;
		while (off < d->aside_size) {
			u32 n = d->aside_size - off;
			if (n > sizeof(g_pattern)) n = sizeof(g_pattern);
			host_peek(d->aside_addr + off, g_pattern, n);
			crc = qcrc32_update(crc, g_pattern, n);
			off += n;
		}
		check_eq_u64(qcrc32_finish(crc), crc_saved,
		             "and the buffer holds what the file holds");
	}

	savefile_info(&supported, &installed, &running, &pending, &size);
	check((pending & SAVEFILE_PENDING_TRANSFER) != 0,
	      "the transfer is still in flight while the load settles");

	host_poke(d->api_load, (const u8 *)"\x00", 1);
	check(pump_until_transfer_done(SAVEFILE_SETTLE_TICKS + 4),
	      "and it is over once the load has settled");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NONE, "with no error");

	/* ------------------------------------- a store the game walks out on */

	/*
	 * The game going away mid-copy is the one failure a PC can stage: the file
	 * has been opened, so whatever was under that name is already gone, and
	 * what must not be left is a file of the right name and the wrong length.
	 */
	crc_saved = fill_aside(d, 3);
	check(savefile_store("runs", "torn.sav") == ST_OK, "another STORE starts");
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS + 2);
	savefile_transfer(&done, &total, &error);
	check(done > 0 && done < total, "and is part way through the copy");
	check(plat_path_exists(SAVE_ROOT "/runs/torn.sav", NULL, NULL),
	      "with a file open on the console");

	check(quit_and_wait(), "the game quits underneath it");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NO_HELPER, "the transfer says the helper went away");
	check(!plat_path_exists(SAVE_ROOT "/runs/torn.sav", NULL, NULL),
	      "and the half-written file was not left behind");

	check(boot_and_wait("NPEA00385"), "RaC1 comes back");
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	host_poke(d->api_load, (const u8 *)"\x00", 1);

	/* --------------------------------------------------- a missing file */

	host_poke(d->api_load, (const u8 *)"\x00", 1);
	check(savefile_restore("runs", "gone.sav") == ST_NOT_FOUND,
	      "restoring a file that is not there is NOT_FOUND");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_MISSING, "the error says which");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "nothing is outstanding");
	host_peek(d->api_load, &byte, 1);
	check_eq_u64(byte, 0, "and nothing was raised at the game");

	/* A file that is not the size of the buffer is refused the same way. */
	{
		plat_file_t f;
		check(plat_file_open(SAVE_ROOT "/runs/short.sav", PLAT_OPEN_WRITE, &f) == 0,
		      "a short file is written into the category");
		plat_file_write(f, "not a save", 10);
		plat_file_close(f);

		check(savefile_restore("runs", "short.sav") == ST_BAD_ARG,
		      "restoring it is BAD_ARG");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_SHORT, "with the short-file error");
		host_peek(d->api_load, &byte, 1);
		check_eq_u64(byte, 0, "and again nothing was raised");
	}

	/* ---------------------------- a file the client uploaded itself */

	/*
	 * No sidecar, because the client wrote it with the file ops. The listing
	 * sums it once and writes the sum out, so the next listing is free.
	 */
	check(savefile_list("runs", out, sizeof(out), &len) == ST_OK, "the listing runs again");
	check_eq_u64(out[0], 2, "and reports the uploaded file too");
	check_eq_u64(be32_get(out + 1 + SAVEFILE_ROW_SIZE + SAVEFILE_NAME_LEN), 10,
	             "with its real size");
	check_eq_u64(be32_get(out + 1 + SAVEFILE_ROW_SIZE + SAVEFILE_NAME_LEN + 4),
	             qcrc32((const u8 *)"not a save", 10),
	             "and a CRC computed from the file itself");
	check(plat_path_exists(SAVE_ROOT "/runs/short.sav.sum", NULL, NULL),
	      "the sum it computed was written beside the file");

	/* ------------------------------------------------------- FILE_RENAME */

	check(plat_file_rename(SAVE_ROOT "/runs/short.sav",
	                       SAVE_ROOT "/runs/renamed.sav") == 0,
	      "plat_file_rename moves a file");
	check(!plat_path_exists(SAVE_ROOT "/runs/short.sav", NULL, NULL),
	      "the old name is gone");
	check(plat_path_exists(SAVE_ROOT "/runs/renamed.sav", NULL, NULL),
	      "and the new one is there");

	/* ------------------------------------------------- deleting a category */

	check(savefile_category(SAVEFILE_CATEGORY_DELETE, "runs") != ST_OK,
	      "a category with saves in it is not deleted");

	plat_file_unlink(SAVE_ROOT "/runs/one.sav");
	plat_file_unlink(SAVE_ROOT "/runs/renamed.sav");
	/*
	 * one.sav.sum and short.sav.sum are left behind on purpose: a client that
	 * forgets a sidecar must not end up with a category it cannot remove.
	 */
	check(savefile_category(SAVEFILE_CATEGORY_DELETE, "runs") == ST_OK,
	      "with the saves gone the category goes, sidecars and all");
	check(savefile_category(SAVEFILE_CATEGORY_DELETE, "runs") == ST_NOT_FOUND,
	      "and deleting it twice is NOT_FOUND");

	check(savefile_categories(out, sizeof(out), &len) == ST_OK, "the categories are read again");
	check_eq_u64(out[0], 0, "and the library is empty");

	/* ------------------------------------------- outside a running game */

	check(quit_and_wait(), "the game goes away");
	check(savefile_store("runs", "one.sav") == ST_NOT_INGAME, "STORE is NOT_INGAME");
	check(savefile_restore("runs", "one.sav") == ST_NOT_INGAME, "and RESTORE");
	check(savefile_library_gate() == ST_NOT_INGAME, "and the library ops");
}

/*
 * Protocol 1.7. A VALUE whose field the game reads as two's complement is
 * flagged SIGNED and says how wide that field is, so a client can sign-extend
 * the readout instead of showing 65535 where the game means -1. The width goes
 * out in the `bits` byte the revision took from Feature's padding; the games'
 * tables keep it in the kind-dependent `aux`, which a VALUE has no other use for.
 */
#define SIGNED_QE_R2   23
#define SIGNED_XP_R2   10
#define SIGNED_QE_R3   14
#define SIGNED_XP_R3    9
#define UNSIGNED_R2     7   /* the bolt count, the control in this group */
#define R2_QE_ADDR     0x013298CCu
#define R3_QE_ADDR     0x00C1E2C0u

/* One encoded row: the flag, the width byte and the range the width implies. */
static void check_signed_row(const u8 *out, u32 len, u8 id, u8 want_bits,
                             const char *what)
{
	const u8 *row = desc_feature(out, len, id);

	check(row != NULL, what);
	if (row == NULL) return;

	check(row[1] == FEATURE_VALUE, "it is a VALUE");
	check((row[4] & FEATURE_FLAG_SIGNED) != 0, "it is flagged SIGNED");
	check_eq_u64(row[6], want_bits, "and carries its field width in `bits`");
	check(row[3] == 0, "the wire's aux byte stays 0 for a VALUE");
	check(row[7] == 0, "the second pad byte is still zero");
	check(be32_get(row + 8) == 0 && be32_get(row + 12) == 0,
	      "min and max stay 0: the width is the range");
}

static void test_signed_values(void)
{
	u8 out[4096];
	u32 len = 0;
	const u8 *row;
	u8 halfword[2];
	u32 g;

	group("protocol 1.7 signed VALUEs");

	check(FEATURE_FLAG_SIGNED == 0x20, "SIGNED is flags bit5");
	check(FEATURE_BITS_DEFAULT == 32, "and a `bits` of 0 stands for 32");

	/* Whatever a game marks SIGNED has to be a VALUE that says how wide it is. */
	for (g = 0; ; g++) {
		const struct game_api *api = game_at(g);
		const struct game_describe *d;
		u8 i;

		if (api == NULL) break;
		if (api->game_id == GAME_NONE || api->describe == NULL) continue;

		d = api->describe();
		for (i = 0; i < d->nfeatures; i++) {
			const struct feature_desc *f = &d->features[i];

			if ((f->flags & FEATURE_FLAG_SIGNED) == 0) continue;

			check(f->kind == FEATURE_VALUE, "SIGNED sits on a VALUE");
			check(f->aux == 8 || f->aux == 16 || f->aux == 32,
			      "and the row names a field width of 8, 16 or 32");
			check(f->min == 0 && f->max == 0,
			      "and leaves min and max unbounded");
		}
	}

	/* RaC2: the QE write-offset halfword and the health XP word. */
	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00386"), "NPEA00386 reaches INGAME");
	check(features_describe(out, sizeof(out), &len) == ST_OK, "RaC2 DESCRIBE encodes");

	check_signed_row(out, len, SIGNED_QE_R2, 16, "RaC2 describes the QE write-offset");
	check_signed_row(out, len, SIGNED_XP_R2, 32, "RaC2 describes health XP");

	row = desc_feature(out, len, UNSIGNED_R2);
	check(row != NULL && (row[4] & FEATURE_FLAG_SIGNED) == 0 && row[6] == 0,
	      "the bolt count is neither signed nor width-tagged");

	/*
	 * The whole point: -1 travels as its low sixteen bits and has to reach the
	 * game as a halfword, or a negative offset never lands.
	 */
	check(features_set(SIGNED_QE_R2, 0xFFFFu) == ST_OK, "FEATURE_SET sends -1 as 0xFFFF");
	host_peek(R2_QE_ADDR, halfword, 2);
	check(halfword[0] == 0xFF && halfword[1] == 0xFF,
	      "and RaC2 wrote both bytes of the halfword");
	check(features_set(SIGNED_QE_R2, 0x8000u) == ST_OK, "the most negative offset is in range");
	host_peek(R2_QE_ADDR, halfword, 2);
	check(halfword[0] == 0x80 && halfword[1] == 0x00, "and lands as 0x8000");

	/* RaC3: the same pair, and the same halfword write behind its QE offset. */
	check(quit_and_wait(), "quit RaC2");
	check(boot_and_wait("NPEA00387"), "NPEA00387 reaches INGAME");
	check(features_describe(out, sizeof(out), &len) == ST_OK, "RaC3 DESCRIBE encodes");

	check_signed_row(out, len, SIGNED_QE_R3, 16, "RaC3 describes the QE offset");
	check_signed_row(out, len, SIGNED_XP_R3, 32, "RaC3 describes health XP");

	check(features_set(SIGNED_QE_R3, 0xFFFFu) == ST_OK, "RaC3 takes -1 as 0xFFFF");
	host_peek(R3_QE_ADDR, halfword, 2);
	check(halfword[0] == 0xFF && halfword[1] == 0xFF,
	      "and wrote both bytes of the halfword");

	check(quit_and_wait(), "quit RaC3");
}

/* --------------------------------------------- combos and COMBO_SUSPEND (1.8) */

/*
 * The save-position combo is the one the capture problem was reported against:
 * recording L2+R2+Right over it also saved a position. Firing is observed
 * through the position slot the action writes, which is empty until it fires.
 */
#define COMBO_TEST_MASK 0x1005u   /* l2 + l1 + up */
#define COMBO_TEST_SLOT 2

static void test_combo_suspend(void)
{
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	u8 planet;
	u64 window;

	group("combo suspend");

	check(boot_and_wait("NPEA00385"), "RaC1 boots for the combo checks");

	window = session_combo_suspend_window_us();
	check_eq_u64(window, 120000000ull, "a hold lasts two minutes by default");

	config_set_selected_slot(COMBO_TEST_SLOT);
	check(config_set_combo(COMBO_SAVE_POSITION, COMBO_TEST_MASK) == ST_OK,
	      "the save-position combo is stored");

	planet = session_current_planet();
	pos_clear(planet, COMBO_TEST_SLOT);

	/* The plain case first: held mask equals the stored mask, the combo fires. */
	host_set_pad(0);
	pump(2);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_OK,
	      "the matching pad mask fires the combo");

	host_set_pad(0);
	pump(2);
	pos_clear(planet, COMBO_TEST_SLOT);

	/* What the client does while it captures. */
	session_combo_suspend(1);
	host_set_pad(COMBO_TEST_MASK);
	pump(4);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "with the hold on the same mask does nothing");

	/*
	 * Lifting the hold must not fire the combo the user is still pressing: the
	 * pad has to come back to empty first, exactly as it does after any combo.
	 */
	session_combo_suspend(0);
	pump(4);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "resuming does not fire the combo under the buttons still held");

	host_set_pad(0);
	pump(2);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_OK,
	      "and once the pad is released the combo fires again");

	/*
	 * The deadline is the whole point of a hold that expires: a client that dies
	 * mid-capture never sends the 0, and the console has to hand the combos back
	 * on its own. Shortened here so the test does not wait two minutes for it.
	 */
	host_set_pad(0);
	pump(2);
	pos_clear(planet, COMBO_TEST_SLOT);

	session_set_combo_suspend_window_us(30000);   /* 30 ms */
	session_combo_suspend(1);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "the hold holds inside its window");

	host_set_pad(0);
	pump(2);
	plat_sleep_us(60000);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_OK,
	      "and the window expiring hands the combos back with no client");

	session_set_combo_suspend_window_us(window);
	check_eq_u64(session_combo_suspend_window_us(), 120000000ull,
	             "the window is back to two minutes");

	/* Leave nothing behind for the tests that follow. */
	host_set_pad(0);
	pump(2);
	pos_clear(planet, COMBO_TEST_SLOT);
	config_set_combo(COMBO_SAVE_POSITION, 0);
	config_set_selected_slot(0);

	check(quit_and_wait(), "quit RaC1");
}

/* ------------------------------------------------------------------ RaC2 */

#define R2_FP_ADDR      0x00BEA8A0u
#define R2_AMMO_INSTR   0x00B30C7Cu
#define R2_CHARGE_BUF   0x0145C180u
#define R2_LOADCOUNT    0x0147A25Bu
#define R2_COORDS       0x0147F260u
#define R2_BOSS_SIB     0x01481792u
#define R2_PLANET_ADDR  0x01329A3Cu
#define R2_BOLTS_ADDR   0x01329A90u
#define R2_PAD_MANIP    0x013185B8u
#define R2_PBOLT_ARRAY  0x01562540u
#define R2_LEVELFLAGS   0x015625B0u
#define R2_LOADPLANET   0x0156B050u
#define R2_SF_HELPER    0x010CD71Du
#define R2_SF_LOAD      0x010CD71Eu
#define R2_SF_SET_ASIDE 0x010CD71Fu

#define F2_FAST_LOADS   0
#define F2_INFINITE_AMMO 1
#define F2_DIE          6
#define F2_BOLTS        7
#define F2_DEATH_BOSSES 13
#define F2_DEATH_PBOLTS 14
#define F2_AUTO_ANYPCT  21
#define F2_LOAD_ASIDE   30
#define F2_SET_ASIDE    31
#define F2_MAX_LEVELS   38
#define F2_MAX_AMMO     39

/*
 * The item system: every array is base plus item id times stride, and the stats
 * table is 208-byte records indexed by the item id of a weapon VERSION.
 */
#define R2_AMMO_ARRAY   0x0148182Cu
#define R2_OWNED_ARRAY  0x01481A80u
#define R2_EXP_ARRAY    0x01481AF0u
#define R2_ITEM_ARRAY   0x01329A40u
#define R2_STATS_TABLE  0x01322A90u
#define R2_STATS_STRIDE 208u
#define R2_STATS_CAP    0x8Au

#define R2_OWNED(item)  (R2_OWNED_ARRAY + (u32)(item))
#define R2_AMMO(item)   (R2_AMMO_ARRAY + (u32)(item) * 4u)
#define R2_EXP(item)    (R2_EXP_ARRAY + (u32)(item) * 4u)
#define R2_ITEM(item)   (R2_ITEM_ARRAY + (u32)(item))
#define R2_CAPACITY(v)  (R2_STATS_TABLE + (u32)(v) * R2_STATS_STRIDE + R2_STATS_CAP)

/* The unlock categories, in the order rac2_panel.c declares them. */
#define R2_CAT_WEAPONS 0
#define R2_CAT_GADGETS 1
#define R2_CAT_ITEMS   2

/* Lancer: unlock id 0, item id 30, versions 60, 79 and 80. */
#define LANCER_ID       0
#define LANCER_ITEM     30
#define LANCER_V2       60
#define LANCER_V3       79
#define LANCER_V4       80
#define R2_UNLOCK_LANCE R2_OWNED(LANCER_ITEM)   /* 0x1481A9E, the old address */

/* Bouncer: unlock id 10, item id 37, top version 98. */
#define BOUNCER_ID      10
#define BOUNCER_ITEM    37
#define BOUNCER_V4      98

/* Clank Zapper: unlock id 23, item id 9, and the one weapon that stops at V2. */
#define ZAPPER_ID       23
#define ZAPPER_ITEM     9
#define ZAPPER_V2       73

/* Zodiac: a weapon with no second version, so no Level slot. */
#define ZODIAC_ID       21
#define ZODIAC_ITEM     43

/* Heli-Pack: a gadget, so an owned byte and nothing else. */
#define HELI2_ID        29
#define HELI2_ITEM      2

/* The seven weapons with one version: Tesla Claw through the RYNO II. */
#define R2_NO_LEVEL(id) ((id) >= 16 && (id) <= 22)

/*
 * A row by its unlock id. RaC2 has no retired unlock id and RaC3 has one, so
 * neither list can be assumed to run id by id: a check that wants a particular
 * item asks for it by id.
 */
static const struct game_unlock *unlock_row(const struct game_unlock *list,
                                            u8 n, u8 id)
{
	u8 i;

	for (i = 0; i < n; i++)
		if (list[i].id == id) return &list[i];

	return NULL;
}

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

	group("RaC2: unlocks with levels, XP and ammo");

	{
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		u8 n = 0, ncat = 0;
		const struct unlock_field_desc *fields = NULL;
		const struct game_unlock *row = NULL;
		u32 values[4];

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 44, "the whole RC2Unlocks table is there");
		check_eq_u64(ncat, 3, "in three categories");
		check(list != NULL && qstreq(list[0].name, "Lancer"), "entry 0 is the Lancer");

		/*
		 * RaC2's unlocks moved onto the item arrays, so slot 1 is the weapon
		 * version the item array holds and slot 2 its experience, the way RaC3
		 * has had them since protocol 1.3.
		 */
		check(fields != NULL && qstreq(fields[0].name, "Owned") &&
		      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
		check(fields != NULL && qstreq(fields[1].name, "Level") &&
		      fields[1].kind == UNLOCK_KIND_NUMBER && fields[1].max == 4,
		      "slot 1 is a Level number that stops at 4");
		check(fields != NULL && qstreq(fields[2].name, "XP") &&
		      fields[2].kind == UNLOCK_KIND_NUMBER && fields[2].max == 0,
		      "slot 2 is an unbounded XP number");
		check(fields != NULL && qstreq(fields[3].name, "Ammo") &&
		      fields[3].kind == UNLOCK_KIND_NUMBER && fields[3].max == 0,
		      "slot 3 is an unbounded Ammo number");

		row = unlock_row(list, n, LANCER_ID);
		check(row != NULL && row->fields ==
		      (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 |
		       UNLOCK_FIELD_2 | UNLOCK_FIELD_3),
		      "a weapon with versions declares all four fields");

		row = unlock_row(list, n, ZODIAC_ID);
		check(row != NULL && qstreq(row->name, "Zodiac") &&
		      row->fields == (UNLOCK_FIELD_0 | UNLOCK_FIELD_2 | UNLOCK_FIELD_3),
		      "a weapon with no second version declares no Level");

		row = unlock_row(list, n, HELI2_ID);
		check(row != NULL && qstreq(row->name, "Heli-Pack") &&
		      row->fields == UNLOCK_FIELD_0,
		      "a gadget has no level, no XP and no ammo, so it is owned-only");

		/*
		 * The category decides the mask, bar the seven weapons the game gives
		 * no second version: those declare everything but the level.
		 */
		{
			u8 i, wrong = 0;

			for (i = 0; i < n; i++) {
				u8 want = UNLOCK_FIELD_0;

				if (list[i].category == R2_CAT_WEAPONS)
					want = (u8)(UNLOCK_FIELD_0 | UNLOCK_FIELD_1 |
					            UNLOCK_FIELD_2 | UNLOCK_FIELD_3);
				if (R2_NO_LEVEL(list[i].id))
					want = (u8)(want & ~(u8)UNLOCK_FIELD_1);

				if (list[i].fields != want) wrong++;
			}
			check_eq_u64(wrong, 0, "every row declares the slots its category has");
		}

		/* A weapon on V2 with experience and rounds in the magazine. */
		host_poke(R2_UNLOCK_LANCE, (const u8 *)"\x01", 1);
		host_poke(R2_ITEM(LANCER_ITEM), (const u8 *)"\x3C", 1);   /* V2 */
		host_poke(R2_EXP(LANCER_ITEM), (const u8 *)"\x00\x00\x10\x92", 4);
		host_poke(R2_AMMO(LANCER_ITEM), (const u8 *)"\x00\x00\x00\x4D", 4);

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, LANCER_ID), values) == ST_OK,
		      "the Lancer reads live");
		check_eq_u64(values[0], 1, "owned reads back");
		check_eq_u64(values[1], 2, "the item array byte reads back as its version");
		check_eq_u64(values[2], 4242, "the exp reads back");
		check_eq_u64(values[3], 77, "the ammo reads back");

		/* The level is the step in the chain, not the distance from the id. */
		host_poke(R2_ITEM(LANCER_ITEM), (const u8 *)"\x50", 1);   /* V4 */
		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, LANCER_ID), values) == ST_OK,
		      "the Lancer reads again");
		check_eq_u64(values[1], 4, "the top version reads as 4");

		host_poke(R2_ITEM(LANCER_ITEM), (const u8 *)"\x07", 1);   /* nothing of ours */
		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, LANCER_ID), values) == ST_OK,
		      "and once more");
		check_eq_u64(values[1], 0, "a byte that names no version of it reads as 0");

		/* Writing a level writes that version's item id and nothing else. */
		host_poke(R2_ITEM(LANCER_ITEM - 1), (const u8 *)"\x55", 1);
		host_poke(R2_ITEM(LANCER_ITEM + 1), (const u8 *)"\x66", 1);
		check(g->unlock_set(LANCER_ID, 1, 3) == ST_OK, "UNLOCK_SET level=3");
		host_peek(R2_ITEM(LANCER_ITEM), &b, 1);
		check_eq_u64(b, LANCER_V3, "the item array carries the V3 item id");
		host_peek(R2_ITEM(LANCER_ITEM - 1), &b, 1);
		check_eq_u64(b, 0x55, "the item on either side of it was left alone");
		host_peek(R2_ITEM(LANCER_ITEM + 1), &b, 1);
		check_eq_u64(b, 0x66, "both of them");
		host_peek(R2_OWNED(LANCER_V3), &b, 1);
		check_eq_u64(b, 0, "and a version has no owned byte of its own to write");

		/*
		 * The level field advertises the game-wide maximum of 4, so a client may
		 * well send 4 for the Clank Zapper. UNLOCK_SET clamps to the entry's own
		 * version count rather than refusing.
		 */
		check(g->unlock_set(LANCER_ID, 1, 99) == ST_OK,
		      "a version past the weapon's count is clamped, not refused");
		host_peek(R2_ITEM(LANCER_ITEM), &b, 1);
		check_eq_u64(b, LANCER_V4, "the Lancer landed on V4");
		check(g->unlock_set(ZAPPER_ID, 1, 4) == ST_OK, "the Clank Zapper takes a level of 4");
		host_peek(R2_ITEM(ZAPPER_ITEM), &b, 1);
		check_eq_u64(b, ZAPPER_V2, "and stops at its own V2");
		check(g->unlock_set(LANCER_ID, 1, 1) == ST_OK, "back down to V1");
		host_peek(R2_ITEM(LANCER_ITEM), &b, 1);
		check_eq_u64(b, LANCER_ITEM, "which is the weapon's own item id");

		/* Owned, XP and ammo land in their own array cell and nowhere else. */
		check(g->unlock_set(LANCER_ID, 0, 1) == ST_OK, "UNLOCK_SET owned=1");
		host_peek(R2_UNLOCK_LANCE, &b, 1);
		check_eq_u64(b, 1, "the owned byte was written");

		host_poke(R2_EXP(LANCER_ITEM - 1), (const u8 *)"\xAA\xAA\xAA\xAA", 4);
		host_poke(R2_EXP(LANCER_ITEM + 1), (const u8 *)"\xBB\xBB\xBB\xBB", 4);
		check(g->unlock_set(LANCER_ID, 2, 1234) == ST_OK, "UNLOCK_SET xp");
		mem_read_u32(R2_EXP(LANCER_ITEM), &v);
		check_eq_u64(v, 1234, "the exp word was written");
		mem_read_u32(R2_EXP(LANCER_ITEM - 1), &v);
		check_eq_u64(v, 0xAAAAAAAAu, "the exp beside it was left alone");
		mem_read_u32(R2_EXP(LANCER_ITEM + 1), &v);
		check_eq_u64(v, 0xBBBBBBBBu, "on both sides");

		host_poke(R2_AMMO(LANCER_ITEM - 1), (const u8 *)"\xAA\xAA\xAA\xAA", 4);
		host_poke(R2_AMMO(LANCER_ITEM + 1), (const u8 *)"\xBB\xBB\xBB\xBB", 4);
		check(g->unlock_set(LANCER_ID, 3, 55) == ST_OK, "UNLOCK_SET ammo");
		mem_read_u32(R2_AMMO(LANCER_ITEM), &v);
		check_eq_u64(v, 55, "the ammo word was written");
		mem_read_u32(R2_AMMO(LANCER_ITEM - 1), &v);
		check_eq_u64(v, 0xAAAAAAAAu, "and the magazines beside it were left alone");
		mem_read_u32(R2_AMMO(LANCER_ITEM + 1), &v);
		check_eq_u64(v, 0xBBBBBBBBu, "on both sides");

		/*
		 * A gadget takes its owned byte and refuses the other three slots, which
		 * is what stops a client writing an exp or ammo word the game does not
		 * count for it.
		 */
		host_poke(R2_EXP(HELI2_ITEM), (const u8 *)"\x00\x00\x00\x09", 4);
		host_poke(R2_AMMO(HELI2_ITEM), (const u8 *)"\x00\x00\x00\x09", 4);
		check(g->unlock_set(HELI2_ID, 0, 1) == ST_OK, "a gadget takes owned=1");
		host_peek(R2_OWNED(HELI2_ITEM), &b, 1);
		check_eq_u64(b, 1, "and its owned byte was written");
		check(g->unlock_set(HELI2_ID, 1, 2) == ST_UNSUPPORTED, "a gadget has no level");
		check(g->unlock_set(HELI2_ID, 2, 1) == ST_UNSUPPORTED, "a gadget has no XP");
		check(g->unlock_set(HELI2_ID, 3, 1) == ST_UNSUPPORTED, "a gadget has no ammo");
		mem_read_u32(R2_EXP(HELI2_ITEM), &v);
		check_eq_u64(v, 9, "the refused XP write left the word alone");
		mem_read_u32(R2_AMMO(HELI2_ITEM), &v);
		check_eq_u64(v, 9, "and so did the refused ammo write");

		check(g->unlock_set(ZODIAC_ID, 1, 2) == ST_UNSUPPORTED,
		      "a weapon with one version has no level either");
		host_peek(R2_ITEM(ZODIAC_ITEM), &b, 1);
		check_eq_u64(b, 0, "and nothing was written for it");
		check(g->unlock_set(200, 0, 1) == ST_BAD_ARG, "an unknown id is BAD_ARG");
	}

	group("RaC2: the two weapon actions");

	{
		/* The Lancer is held at V2, the Bouncer is not held at all. */
		host_poke(R2_OWNED(LANCER_ITEM), (const u8 *)"\x01", 1);
		host_poke(R2_ITEM(LANCER_ITEM), (const u8 *)"\x3C", 1);
		host_poke(R2_OWNED(BOUNCER_ITEM), (const u8 *)"\x00", 1);
		host_poke(R2_ITEM(BOUNCER_ITEM), (const u8 *)"\x25", 1);

		check(features_trigger(F2_MAX_LEVELS) == ST_OK, "Max all weapon levels runs");
		host_peek(R2_ITEM(LANCER_ITEM), &b, 1);
		check_eq_u64(b, LANCER_V4, "the Lancer went to its top version");
		host_peek(R2_ITEM(BOUNCER_ITEM), &b, 1);
		check_eq_u64(b, BOUNCER_ITEM,
		             "and the Bouncer, which the player has not got, was passed over");

		/*
		 * The magazine comes from the stats record of the version the weapon is
		 * on, so a V2 Lancer is refilled to the V2 capacity and not the V1 one.
		 */
		host_poke(R2_ITEM(LANCER_ITEM), (const u8 *)"\x3C", 1);
		host_poke(R2_CAPACITY(LANCER_ITEM), (const u8 *)"\x00\x0A", 2);
		host_poke(R2_CAPACITY(LANCER_V2), (const u8 *)"\x00\x64", 2);
		host_poke(R2_CAPACITY(BOUNCER_ITEM), (const u8 *)"\x00\x0F", 2);
		host_poke(R2_AMMO(LANCER_ITEM), (const u8 *)"\x00\x00\x00\x00", 4);
		host_poke(R2_AMMO(BOUNCER_ITEM), (const u8 *)"\x00\x00\x00\x07", 4);

		check(features_trigger(F2_MAX_AMMO) == ST_OK, "Max all weapon ammo runs");
		mem_read_u32(R2_AMMO(LANCER_ITEM), &v);
		check_eq_u64(v, 100, "the V2 magazine, not the V1 one");
		mem_read_u32(R2_AMMO(BOUNCER_ITEM), &v);
		check_eq_u64(v, 7, "and the unowned Bouncer was passed over again");
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

	group("RaC2: savefile requests");

	host_poke(R2_SF_HELPER, (const u8 *)"\x00\x00\x00", 3);
	check(features_trigger(F2_SET_ASIDE) == ST_OK,
	      "set aside works from a cold process: the helper goes in first");
	host_peek(R2_SF_SET_ASIDE, &b, 1);
	check_eq_u64(b, 1, "the set-aside byte was written");
	check(features_trigger(F2_LOAD_ASIDE) == ST_OK, "so does the load action");
	host_peek(R2_SF_LOAD, &b, 1);
	check_eq_u64(b, 1, "and it wrote the load byte");
	check(session_load_setaside() == ST_OK, "and so does the combo action");

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
#define R3_SF_LOAD      0x00D9FF01u
#define R3_SF_SET_ASIDE 0x00D9FF02u
#define R3_COORDS       0x00DA2870u
#define R3_HEALTH_ADDR  0x00DA5040u
#define R3_UNLOCK_ARRAY 0x00DA56ECu
#define R3_EXP_ARRAY    0x00DA5824u
#define R3_AMMO_ARRAY   0x00DA5240u
#define R3_VID_COMICS   0x00DA650Bu
#define R3_LEVELFLAGS   0x00ECE675u
#define R3_LOADPLANET   0x00EE9310u
#define R3_DEST_PLANET  0x00EE9314u
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
#define F3_FAST_LOADS    38

/* The unlock categories, in the order rac3_panel.c declares them. */
#define R3_CAT_WEAPONS 0
#define R3_CAT_GADGETS 1
#define R3_CAT_COMICS  2

/* Agents of Doom: item id 0x57, unlock 0x4FF, exp 0x74C, ammo 0x39F, 8 levels. */
#define AOD_ID     21
#define AOD_UNLOCK (R3_UNLOCK_ARRAY + 0x57u)
#define AOD_EXP    (R3_EXP_ARRAY + (0x74Cu - 0x5F0u))
#define AOD_AMMO   (R3_AMMO_ARRAY + (0x39Fu - 0x243u))
#define AOD_ITEM   (R3_ITEM_ARRAY + 0x57u)

/* Bouncer, one of the five GC weapons whose versions live in a table of their own. */
#define BOUNCER_ID   23
#define BOUNCER_ITEM (R3_ITEM_ARRAY + 0x13u)

/* R3YNO: item id 0x97, the one weapon that stops at v5 rather than v8. */
#define RYNO_ID   36
#define RYNO_ITEM (R3_ITEM_ARRAY + 0x97u)

/* Suck Cannon: item id 0x87, and no ammo the game actually counts. */
#define SUCK_ID   40
#define SUCK_ITEM (R3_ITEM_ARRAY + 0x87u)

/* Heli Pack: a gadget, so an owned byte and nothing else. */
#define HELI_ID     1
#define HELI_UNLOCK (R3_UNLOCK_ARRAY + (0x4AAu - 0x4A8u))
#define HELI_EXP    (R3_EXP_ARRAY + (0x5F8u - 0x5F0u))
#define HELI_AMMO   (R3_AMMO_ARRAY + (0x24Bu - 0x243u))

/* Vid comic 2, and vid comic 3 three bytes into the comic run. */
#define COMIC2_ID 17
#define COMIC3_ID 18

/* The Bomb Glove's id, retired because UYA cannot reach the item in game. */
#define BOMB_GLOVE_ID 0

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
		const struct game_unlock *row = NULL;
		u32 values[4];

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 40, "the whole UYAUnlocks table is there, less the Bomb Glove");
		check_eq_u64(ncat, 3, "in three categories");

		row = unlock_row(list, n, AOD_ID);
		check(row != NULL && qstreq(row->name, "Agents of Doom"),
		      "id 21 is the Agents of Doom");
		check(row != NULL && row->fields ==
		      (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 |
		       UNLOCK_FIELD_2 | UNLOCK_FIELD_3),
		      "a weapon declares all four fields");

		row = unlock_row(list, n, COMIC2_ID);
		check(row != NULL && row->fields == UNLOCK_FIELD_0,
		      "a vid comic is owned-only");

		row = unlock_row(list, n, HELI_ID);
		check(row != NULL && qstreq(row->name, "Heli Pack") &&
		      row->fields == UNLOCK_FIELD_0,
		      "a gadget has no level, no XP and no ammo, so it is owned-only too");

		row = unlock_row(list, n, SUCK_ID);
		check(row != NULL && qstreq(row->name, "Suck Cannon") &&
		      (row->fields & UNLOCK_FIELD_3) == 0,
		      "the Suck Cannon carries no ammo in game, so it declares none");

		/*
		 * The categories are what decide the mask: the weapons carry all four
		 * slots, bar the Suck Cannon's ammo, and everything else carries the
		 * owned flag alone.
		 */
		{
			u8 i, wrong = 0;

			for (i = 0; i < n; i++) {
				u8 want = UNLOCK_FIELD_0;

				if (list[i].category == R3_CAT_WEAPONS)
					want = (u8)(UNLOCK_FIELD_0 | UNLOCK_FIELD_1 |
					            UNLOCK_FIELD_2 | UNLOCK_FIELD_3);
				if (list[i].id == SUCK_ID)
					want = (u8)(want & ~(u8)UNLOCK_FIELD_3);

				if (list[i].fields != want) wrong++;
			}
			check_eq_u64(wrong, 0, "every row declares the slots its category has");
		}

		/* Id 0 is retired, not renumbered: the ids around it did not move. */
		check(unlock_row(list, n, BOMB_GLOVE_ID) == NULL,
		      "the Bomb Glove is gone from the table");
		{
			u8 i, named = 0;

			for (i = 0; i < n; i++)
				if (qstreq(list[i].name, "Bomb Glove")) named++;
			check_eq_u64(named, 0, "and no row carries its name");
		}
		check(unlock_row(list, n, 1) != NULL && unlock_row(list, n, 40) != NULL,
		      "the ids on either side of it kept their places");

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

		check(g->unlock_set(AOD_ID, 0, 1) == ST_OK, "UNLOCK_SET owned=1");
		host_peek(AOD_UNLOCK, &b, 1);
		check_eq_u64(b, 1, "the owned byte was written");

		check(g->unlock_set(AOD_ID, 1, 3) == ST_OK, "UNLOCK_SET level=3");
		host_peek(AOD_ITEM, &b, 1);
		check_eq_u64(b, 0x59, "the item array carries id + version - 1");

		check(g->unlock_set(AOD_ID, 2, 4242) == ST_OK, "UNLOCK_SET xp");
		mem_read_u32(AOD_EXP, &v);
		check_eq_u64(v, 4242, "the exp word was written");

		check(g->unlock_set(AOD_ID, 3, 77) == ST_OK, "UNLOCK_SET ammo");
		mem_read_u32(AOD_AMMO, &v);
		check_eq_u64(v, 77, "the ammo word was written");

		/*
		 * The level field advertises the game-wide maximum of 8, so a client
		 * may well send 8 for the R3YNO. UNLOCK_SET clamps to the entry's own
		 * level count rather than refusing.
		 */
		check(g->unlock_set(AOD_ID, 1, 99) == ST_OK,
		      "a version past the weapon's level count is clamped, not refused");
		host_peek(AOD_ITEM, &b, 1);
		check_eq_u64(b, 0x5Eu, "the Agents of Doom landed on v8");
		check(g->unlock_set(RYNO_ID, 1, 8) == ST_OK, "the R3YNO takes a level of 8");
		host_peek(RYNO_ITEM, &b, 1);
		check_eq_u64(b, 0x9Bu, "and stops at its own v5");
		check(g->unlock_set(AOD_ID, 1, 3) == ST_OK, "back down to v3");

		check(g->unlock_set(COMIC2_ID, 2, 1) == ST_UNSUPPORTED,
		      "a vid comic has no exp word");
		check(g->unlock_set(SUCK_ID, 3, 5) == ST_UNSUPPORTED,
		      "and the Suck Cannon refuses an ammo write");
		check(g->unlock_set(200, 0, 1) == ST_BAD_ARG, "an unknown id is BAD_ARG");
		check(g->unlock_set(BOMB_GLOVE_ID, 0, 1) == ST_BAD_ARG,
		      "and so is the retired Bomb Glove id");

		/*
		 * A gadget takes its owned byte and refuses the other three slots,
		 * which is what stops a client writing an exp or ammo word the game
		 * does not count for it.
		 */
		host_poke(HELI_EXP, (const u8 *)"\x00\x00\x00\x09", 4);
		host_poke(HELI_AMMO, (const u8 *)"\x00\x00\x00\x09", 4);
		check(g->unlock_set(HELI_ID, 0, 1) == ST_OK, "a gadget takes owned=1");
		host_peek(HELI_UNLOCK, &b, 1);
		check_eq_u64(b, 1, "and its owned byte was written");
		check(g->unlock_set(HELI_ID, 1, 2) == ST_UNSUPPORTED, "a gadget has no level");
		check(g->unlock_set(HELI_ID, 2, 1) == ST_UNSUPPORTED, "a gadget has no XP");
		check(g->unlock_set(HELI_ID, 3, 1) == ST_UNSUPPORTED, "a gadget has no ammo");
		mem_read_u32(HELI_EXP, &v);
		check_eq_u64(v, 9, "the refused XP write left the word alone");
		mem_read_u32(HELI_AMMO, &v);
		check_eq_u64(v, 9, "and so did the refused ammo write");

		check(g->unlock_set(BOUNCER_ID, 1, 2) == ST_OK, "the Bouncer goes to v2");
		host_peek(BOUNCER_ITEM, &b, 1);
		check_eq_u64(b, 0xA6, "which is its own table offset, not id + 1");

		/*
		 * The last row is where a slip between the unlock table and the item
		 * table would show, so the Suck Cannon's version has to land on the
		 * Suck Cannon's own item byte.
		 */
		check(g->unlock_set(SUCK_ID, 1, 3) == ST_OK, "the Suck Cannon takes a level");
		host_peek(SUCK_ITEM, &b, 1);
		check_eq_u64(b, 0x89u, "on its own item byte: the two tables still line up");

		/* Vid comic 3 sits at 0x12CA - 0x4A8 past the unlock array, so +3 here. */
		host_poke(R3_VID_COMICS + 3, (const u8 *)"\x01", 1);

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, AOD_ID), values) == ST_OK,
		      "id 21 reads live");
		check_eq_u64(values[0], 1, "owned reads back");
		check_eq_u64(values[1], 3, "the version heuristic reads back");
		check_eq_u64(values[2], 4242, "the exp reads back");
		check_eq_u64(values[3], 77, "the ammo reads back");

		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, BOUNCER_ID), values) == ST_OK,
		      "the Bouncer reads");
		check_eq_u64(values[1], 0, "a GC weapon reports no readable version");

		/* The gadget's owned byte is live; the words it withholds stay zero. */
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, HELI_ID), values) == ST_OK,
		      "the Heli Pack reads");
		check_eq_u64(values[0], 1, "its owned byte reads back");
		check_eq_u64(values[2], 0, "and the XP it does not declare reads as zero");
		check_eq_u64(values[3], 0, "as does the ammo");

		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, COMIC3_ID), values) == ST_OK,
		      "vid comic 3 reads");
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

	/*
	 * Build 11. The two fast-load values are game data and the game writes over
	 * them every time it loads a planet, so the toggle has to arm them again on
	 * every load. A load the client asks for is the case above; this is the game
	 * starting one on its own, which qwark sees only as the destination planet
	 * and then the current planet moving in the hot block.
	 */
	group("RaC3: the Fast loads toggle survives a planet load");

	host_poke(R3_PLANET_ADDR, (const u8 *)"\x00\x00\x00\x05", 4);
	host_poke(R3_DEST_PLANET, (const u8 *)"\x00\x00\x00\x05", 4);
	pump(3);

	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	check(features_set(F3_FAST_LOADS, 1) == ST_OK, "the toggle turns on");
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 3, "and arms the values there and then");

	/* The game is off to Tyhrranosis, and clears them on the way. */
	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R3_FASTLOAD2, (const u8 *)"\x00\x00", 2);
	host_poke(R3_DEST_PLANET, (const u8 *)"\x00\x00\x00\x09", 4);
	pump(1);
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 3, "a load the game started of its own accord re-arms them");

	pump(30);   /* the same fifth of a second the arm above waits */
	{
		u8 pair[2];
		host_peek(R3_FASTLOAD2, pair, 2);
		check(pair[0] == 1 && pair[1] == 1, "second value and all");
	}

	/* And again when it arrives, whatever the load wrote over them on its way. */
	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R3_PLANET_ADDR, (const u8 *)"\x00\x00\x00\x09", 4);
	pump(1);
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 3, "and so does arriving there");

	/* Aquatos is left alone here too. */
	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R3_DEST_PLANET, (const u8 *)"\x00\x00\x00\x08", 4);
	pump(2);
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 0, "a load to Aquatos is not armed");

	/* Off, and a planet load is a planet load again. */
	host_poke(R3_PLANET_ADDR, (const u8 *)"\x00\x00\x00\x08", 4);
	pump(2);
	check(features_set(F3_FAST_LOADS, 0) == ST_OK, "the toggle turns off");
	host_poke(R3_FASTLOAD1, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R3_DEST_PLANET, (const u8 *)"\x00\x00\x00\x05", 4);
	pump(1);
	host_poke(R3_PLANET_ADDR, (const u8 *)"\x00\x00\x00\x05", 4);
	pump(1);
	mem_read_u32(R3_FASTLOAD1, &v);
	check_eq_u64(v, 0, "and nothing arms them any more");

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

	group("RaC3: savefile requests");

	host_poke(R3_SF_HELPER, (const u8 *)"\x00\x00\x00", 3);
	check(features_trigger(F3_SET_ASIDE) == ST_OK,
	      "set aside works from a cold process");
	host_peek(R3_SF_SET_ASIDE, &b, 1);
	check_eq_u64(b, 1, "writing the set-aside byte");
	check(session_load_setaside() == ST_OK, "the combo action works too");
	host_peek(R3_SF_LOAD, &b, 1);
	check_eq_u64(b, 1, "and it wrote the load byte");
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
#define R4_GADGETS      0x00B2B760u
#define R4_GADGET_STEP  68u
#define R4_PER_MOD      0x009DD434u
#define R4_STATS        0x009DE970u
#define R4_STATS_STEP   176u
#define R4_GAME_TYPE    0x00B36DF4u
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
#define F4_MAX_LEVELS    15
#define F4_RESET_LEVELS  16   /* retired in build 32, never reused */
#define F4_MAX_AMMO      17

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
	 * quits the instant it arrives. Poke it during the boot to prove on_enter
	 * clears it.
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
		check_eq_u64(d->nfeatures, 17, "Deadlocked declares seventeen features");
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

	group("Deadlocked: bot and weapon unlocks");

	{
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		u8 n = 0, ncat = 0;
		const struct unlock_field_desc *fields = NULL;
		const struct game_unlock *vipers = NULL;
		const struct game_unlock *flail = NULL;
		u32 values[4];
		u32 vaddr = 0, faddr = 0;
		u8 entry[4];
		u8 around[8];
		u8 i, weapons = 0;
		int rows_ok = 1;

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 26, "sixteen bot upgrades and ten weapons");
		check_eq_u64(ncat, 2, "in two categories");
		check(list != NULL && qstreq(list[0].name, "Pistol Flux LX"),
		      "entry 0 is the Pistol Flux LX");
		check(cats != NULL && qstreq(cats[0], "Bot upgrades") &&
		      qstreq(cats[1], "Weapons"), "the bots first and the weapons after them");

		check(fields != NULL && qstreq(fields[0].name, "Owned") &&
		      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
		check(fields != NULL && qstreq(fields[1].name, "Level") &&
		      fields[1].kind == UNLOCK_KIND_NUMBER && fields[1].max == 99,
		      "slot 1 is the level, up to the challenge-mode 99");
		check(fields != NULL && qstreq(fields[2].name, "Ammo") &&
		      fields[2].kind == UNLOCK_KIND_NUMBER && fields[2].max == 0,
		      "slot 2 is the ammo count, with no bound");
		check(fields != NULL && (fields[3].name == NULL || fields[3].name[0] == 0),
		      "and Deadlocked names no fourth slot");

		/*
		 * A weapon's id is 32 plus its index into g_GadgetData, which is what
		 * keeps the two halves of the table apart for good; the addresses below
		 * are derived from the ids, so the real indices are only stated once.
		 * An entry is 68 bytes, of which qwark touches the first four.
		 */
		for (i = 0; i < n; i++) {
			if (list[i].category == 0) {
				if (list[i].fields != UNLOCK_FIELD_0) rows_ok = 0;
				continue;
			}
			weapons++;
			if (list[i].id < 32) rows_ok = 0;
			if (list[i].fields != (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 | UNLOCK_FIELD_2))
				rows_ok = 0;
			if (qstreq(list[i].name, "Dual Vipers")) vipers = &list[i];
			if (qstreq(list[i].name, "Scorpion Flail")) flail = &list[i];
		}
		check_eq_u64(weapons, 10, "ten weapon rows");
		check(rows_ok, "a bot declares owned alone and a weapon owned, level and ammo "
		               "with an id past the bots");

		check(vipers != NULL && flail != NULL,
		      "the Dual Vipers and the Scorpion Flail are in the table");

		if (vipers != NULL && flail != NULL) {
			vaddr = R4_GADGETS + ((u32)vipers->id - 32) * R4_GADGET_STEP;
			faddr = R4_GADGETS + ((u32)flail->id - 32) * R4_GADGET_STEP;

			/* The indices behind the two rows, stated the long way once. */
			check_eq_u64(vaddr, 0x00B2B760u + 2 * 68, "the Dual Vipers are gadget 2");
			check_eq_u64(faddr, 0x00B2B760u + 15 * 68,
			             "and the Scorpion Flail gadget 15, 68 bytes apiece");

			/*
			 * The halfword is one below the level the game shows, and a locked
			 * weapon holds -1: V5 is a 4 in memory, and 0xFFFF is no weapon.
			 */
			host_poke(vaddr, (const u8 *)"\x00\x04\x01\x2C", 4);  /* V5, 300 rounds */
			host_poke(faddr, (const u8 *)"\xFF\xFF\x00\x07", 4);  /* locked, 7 rounds */

			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST snapshots the gadget table");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(vipers, values) == ST_OK, "a weapon row reads live");
			check_eq_u64(values[0], 1, "a weapon with a level reads owned");
			check_eq_u64(values[1], 5, "the level is the first halfword plus one");
			check_eq_u64(values[2], 300, "and the ammo the second");

			memset(values, 0, sizeof(values));
			check(g->unlock_read(flail, values) == ST_OK, "and so does a locked one");
			check_eq_u64(values[0], 0, "a level of -1 reads as not owned");
			check_eq_u64(values[1], 0, "with level 0, which no owned weapon shows");
			check_eq_u64(values[2], 7, "though its ammo still comes back");

			/*
			 * Both ends of the range. A 0 in memory is V1, the lowest level an
			 * owned weapon has, and must never pass for locked; 98 is V99, the
			 * top of the field.
			 */
			host_poke(faddr, (const u8 *)"\x00\x00", 2);
			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST with a V1 weapon");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(flail, values) == ST_OK, "the V1 weapon reads");
			check_eq_u64(values[0], 1, "a 0 in memory is owned");
			check_eq_u64(values[1], 1, "and shows as V1");

			host_poke(faddr, (const u8 *)"\x00\x62", 2);
			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST with a V99 weapon");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(flail, values) == ST_OK, "the V99 weapon reads");
			check_eq_u64(values[1], 99, "and a 98 in memory shows as V99");

			/*
			 * The last row's entry is 15 strides in, at the far end of the
			 * snapshot: a level poked there has to come back on that row and
			 * on no other, which is what the stride buys.
			 */
			host_poke(faddr, (const u8 *)"\x00\x08", 2);
			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST covers the whole table");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(flail, values) == ST_OK, "the last weapon row reads");
			check(values[0] == 1 && values[1] == 9 && values[2] == 7,
			      "a level 15 strides in is the Scorpion Flail's own");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(vipers, values) == ST_OK, "and the first row again");
			check_eq_u64(values[1], 5, "which still reads its own level");
			host_poke(faddr, (const u8 *)"\xFF\xFF", 2);

			check(g->unlock_set(flail->id, 0, 1) == ST_OK,
			      "UNLOCK_SET owned=1 on a locked weapon");
			host_peek(faddr, entry, 4);
			check(be16_get(entry) == 0 && be16_get(entry + 2) == 7,
			      "hands it V1, a 0 in memory, and leaves the ammo alone");

			check(g->unlock_set(vipers->id, 0, 1) == ST_OK,
			      "UNLOCK_SET owned=1 on a V5 weapon");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry) == 4, "keeps the level it had");

			check(g->unlock_set(vipers->id, 0, 0) == ST_OK, "UNLOCK_SET owned=0");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry) == 0xFFFF && be16_get(entry + 2) == 300,
			      "writes the locked -1 back and nothing else");

			/* A level write moves exactly two bytes of the 68-byte entry. */
			host_poke(vaddr - 2, (const u8 *)"\xA1\xA2", 2);
			host_poke(vaddr + 4, (const u8 *)"\xB1\xB2", 2);

			check(g->unlock_set(vipers->id, 1, 42) == ST_OK, "UNLOCK_SET level=42");
			host_peek(vaddr - 2, around, 8);
			check(be16_get(around + 2) == 41 && be16_get(around + 4) == 300,
			      "writes the level halfword big-endian, one below, and no further");
			check(around[0] == 0xA1 && around[1] == 0xA2,
			      "the bytes before the entry are left alone");
			check(around[6] == 0xB1 && around[7] == 0xB2,
			      "and so is the rest of the entry behind the ammo");

			check(g->unlock_set(vipers->id, 2, 999) == ST_OK, "UNLOCK_SET ammo=999");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry) == 41 && be16_get(entry + 2) == 999,
			      "writes the ammo halfword after it");

			check(g->unlock_set(vipers->id, 1, 1) == ST_OK, "UNLOCK_SET level=1");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry) == 0, "V1 is stored as 0");
			check(g->unlock_set(vipers->id, 1, 99) == ST_OK, "UNLOCK_SET level=99");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry) == 98, "and V99 as 98, the most the halfword may hold");
			check(g->unlock_set(vipers->id, 1, 100) == ST_BAD_ARG,
			      "a level past 99 is BAD_ARG, not a clamp: the game breaks above it");
			check(g->unlock_set(vipers->id, 1, 0) == ST_BAD_ARG,
			      "and so is 0: the field counts from V1, and Owned is what takes a weapon away");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry) == 98, "and nothing was written for either");

			check(g->unlock_set(vipers->id, 2, 0x12345) == ST_OK,
			      "an ammo past a halfword too");
			host_peek(vaddr, entry, 4);
			check(be16_get(entry + 2) == 0xFFFF, "and clamped to what the entry holds");

			check(g->unlock_set(vipers->id, 3, 1) == ST_UNSUPPORTED,
			      "no weapon has a fourth field");

			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST after the writes");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(vipers, values) == ST_OK, "the row reads back");
			check(values[0] == 1 && values[1] == 99 && values[2] == 0xFFFF,
			      "with the level and ammo that were written");
		}

		check(g->unlock_set(5, 0, 1) == ST_OK, "UNLOCK_SET owned=1 on a bot");
		host_peek(R4_BOTS_LIVE + 5, &b, 1);
		check_eq_u64(b, 1, "the live byte was written");
		host_peek(R4_BOTS_SAVE + 5, &b, 1);
		check_eq_u64(b, 1, "and so was the saved copy");

		check(g->unlock_set(5, 1, 1) == ST_UNSUPPORTED, "a bot has no level");
		check(g->unlock_set(16, 0, 1) == ST_BAD_ARG, "an id no row carries is BAD_ARG");

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(&list[5], values) == ST_OK, "entry 5 reads live");
		check_eq_u64(values[0], 1, "owned reads back");
	}

	group("Deadlocked: the weapon actions");

	{
		u32 g2  = R4_GADGETS + 2 * R4_GADGET_STEP;    /* Dual Vipers */
		u32 g3  = R4_GADGETS + 3 * R4_GADGET_STEP;    /* Magma Cannon */
		u32 g15 = R4_GADGETS + 15 * R4_GADGET_STEP;   /* Scorpion Flail */
		u8 got[4];
		u8 desc[4096];
		u32 dlen = 0;

		/*
		 * Two weapons the player has, one of them at V1 — a 0 in memory, the
		 * value that must never pass for locked — and one still locked at -1.
		 */
		host_poke(g2,  (const u8 *)"\x00\x02\x00\x00", 4);   /* V3 */
		host_poke(g3,  (const u8 *)"\x00\x00\x00\x00", 4);   /* V1 */
		host_poke(g15, (const u8 *)"\xFF\xFF\x00\x05", 4);   /* locked, 5 rounds */

		/* Two ammo mods on the Dual Vipers, one mod of another type, none elsewhere. */
		host_poke(g2 + 0x14, (const u8 *)"\x02", 1);
		host_poke(g2 + 0x18, (const u8 *)"\x02", 1);
		host_poke(g2 + 0x1C, (const u8 *)"\x01", 1);
		host_poke(g3 + 0x14, (const u8 *)"\x00\x00\x00\x00", 4);

		/* The stats records, a single-player base and a multiplayer one. */
		host_poke(R4_STATS + 2 * R4_STATS_STEP + 0x42, (const u8 *)"\x00\x64", 2);
		host_poke(R4_STATS + 2 * R4_STATS_STEP + 0x44, (const u8 *)"\x00\xC8", 2);
		host_poke(R4_STATS + 3 * R4_STATS_STEP + 0x42, (const u8 *)"\x00\x32", 2);
		host_poke(R4_STATS + 15 * R4_STATS_STEP + 0x42, (const u8 *)"\x03\xE7", 2);
		host_poke(R4_PER_MOD + 2 * 4, (const u8 *)"\x00\x00\x00\x19", 4);
		host_poke(R4_PER_MOD + 3 * 4, (const u8 *)"\x00\x00\x00\x0A", 4);
		host_poke(R4_GAME_TYPE, (const u8 *)"\x00\x00\x00\x00", 4);

		check(features_trigger(F4_MAX_AMMO) == ST_OK, "the max-ammo action runs");
		host_peek(g2, got, 4);
		check(be16_get(got + 2) == 150,
		      "a base of 100 and two ammo mods at 25 apiece make 150");
		host_peek(g3, got, 4);
		check(be16_get(got + 2) == 50,
		      "a V1 weapon with no mods, a 0 in memory, is owned and gets its base");
		host_peek(g15, got, 4);
		check(be16_get(got) == 0xFFFF && be16_get(got + 2) == 5,
		      "and a locked weapon is left alone");

		host_poke(R4_GAME_TYPE, (const u8 *)"\x00\x00\x00\x01", 4);
		check(features_trigger(F4_MAX_AMMO) == ST_OK, "it runs again in multiplayer");
		host_peek(g2, got, 4);
		check(be16_get(got + 2) == 250, "which takes the other base out of the record");
		host_poke(R4_GAME_TYPE, (const u8 *)"\x00\x00\x00\x00", 4);

		check(features_trigger(F4_MAX_LEVELS) == ST_OK, "the max-levels action runs");
		host_peek(g2, got, 4);
		check(be16_get(got) == 98, "an owned weapon goes to V99, a 98 in memory");
		host_peek(g3, got, 4);
		check(be16_get(got) == 98, "and so does the V1 weapon beside it");
		host_peek(g15, got, 4);
		check(be16_get(got) == 0xFFFF, "while a locked weapon is not handed out");

		/*
		 * Id 16, reset all weapon levels, is retired in build 32: gone from
		 * DESCRIBE, never renumbered, and a trigger on it writes nothing.
		 */
		check(features_describe(desc, sizeof(desc), &dlen) == ST_OK,
		      "Deadlocked DESCRIBE encodes");
		check(desc_feature(desc, dlen, F4_RESET_LEVELS) == NULL,
		      "the retired id 16 is not described");
		check(desc_feature(desc, dlen, F4_MAX_LEVELS) != NULL &&
		      desc_feature(desc, dlen, F4_MAX_AMMO) != NULL,
		      "the ids either side of it kept their numbers");
		check(features_trigger(F4_RESET_LEVELS) == ST_NOT_FOUND,
		      "and triggering it is NOT_FOUND");
		host_peek(g2, got, 4);
		check(be16_get(got) == 98 && be16_get(got + 2) == 250,
		      "which wrote nothing");
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

	group("Deadlocked: savefile requests");

	host_poke(R4_SF_HELPER, (const u8 *)"\x00\x00\x00", 3);
	check(features_trigger(F4_SET_ASIDE) == ST_OK,
	      "set aside works from a cold process");
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
 * Protocol 1.5. Every check here drives the real watchers: it pokes the words a
 * game's ASL script read, steps the tick, and reads the events back through the
 * same bytes AUTOSPLIT_EVENTS puts on the wire.
 */

static void poke8(u32 addr, u8 v)   { host_poke(addr, &v, 1); }
static void poke16(u32 addr, u16 v) { u8 b[2]; be16_put(b, v); host_poke(addr, b, 2); }
static void poke32(u32 addr, u32 v) { u8 b[4]; be32_put(b, v); host_poke(addr, b, 4); }
static void pokef32(u32 addr, f32 v) { u8 b[4]; bef32_put(b, v); host_poke(addr, b, 4); }

struct as_event {
	u32 seq;
	u32 time_ms;    /* revision 1.5: milliseconds, not the tick count */
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
		g_as_list[i].time_ms  = be32_get(e + 4);
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

/* The same, for the kinds that carry a reason code of their own. */
static void as_expect_coded(u32 since, u8 kind, u8 code, const char *what)
{
	check(as_find(since, kind, code) != NULL, what);
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
#define R1_AS_LOADING     8

#define R2_AS_PLANET       1
#define R2_AS_PROTOPET     2
#define R2_AS_A2_CLANK     3
#define R2_AS_MAKTAR_ARENA 4
#define R2_AS_BARLOW_RACE  5
#define R2_AS_ENDAKO_ENTER 6
#define R2_AS_ENDAKO_EXIT  7
#define R2_AS_TABORA_CAVES 8
#define R2_AS_LOAD_SLIDE   9
#define R2_AS_LOAD_CURVED  10
#define R2_AS_LOAD_WIPE    11

#define R3_AS_PLANET        1
#define R3_AS_LDF           2
#define R3_AS_TYHRRAGUISE   3
#define R3_AS_KOROS_BOLT    4
#define R3_AS_BIOBLITERATOR 5
#define R3_AS_LONG_LOAD     6

#define R4_AS_PLANET 1
#define R4_AS_VOX    2
#define R4_AS_QUIT   3

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
#define A1_LOADSCR      0x9645CBu   /* low byte of the loading-screen word */

/* The four gb_sp_as_helper caves and the four words that branch into them. */
#define A1_CAVE_GB      0x4F5BE4u
#define A1_CAVE_SP      0x4F5CACu
#define A1_CAVE_ITEM    0x4F5D10u
#define A1_CAVE_IB      0x4F5D74u
#define A1_HOOK_GB      0x708EC8u
#define A1_HOOK_SP      0x11B7C0u
#define A1_HOOK_ITEM    0x112F08u
#define A1_HOOK_IB      0x112CD0u

#define A2_PLANET       0x1329A3Cu
#define A2_PLAYER_STATE 0x1481474u
#define A2_HERO_TYPE    0x1481494u
#define A2_CHUNK        0x157CE03u
#define A2_CLANK        0x1562699u
#define A2_ENDAKO_EXIT  0x15625E1u
#define A2_BARLOW       0x15625F7u
#define A2_YEEDIL       0x1478991u
#define A2_LOADSCR      0x147A257u

#define A3_PLANET       0x00C1E438u
#define A3_DEST_PLANET  0x00EE9314u
#define A3_GAME_STATE   0x00EE9334u
#define A3_PLAYER_STATE 0x00DA4DB6u
#define A3_NEFFY_HP     0x00C4DF80u
#define A3_NEFFY_PHASE  0x00DA50FCu
#define A3_CHUNK        0x00F08100u
#define A3_GUISE        0x00DA570Au
#define A3_LOADSCR      0x00D99117u   /* low byte of the loading-screen word */

#define A4_PLANET       0x009C3240u
#define A4_REQUEST_LOAD 0x00B36DCCu
#define A4_TARGET       0x00B36DD0u
#define A4_CUTSCENE     0x00B36DE8u
#define A4_IN_GAME      0x00B1F460u
#define A4_TUTORIAL     0x00B1F46Cu
#define A4_VOX_HP       0x449BEAD0u
#define A4_LOADING_VAL  0x01710000u   /* the loading hook's byte */
#define A4_LOADING_H1   0x00011884u   /* the branch into the trampoline */
#define A4_LOADING_H2   0x00011904u   /* the trampoline */
#define A4_MAINMENU     0u            /* planet 0, which is the main menu */

/*
 * What a game's AUTOSPLIT_DESCRIBE table has to say, row for row. Held here
 * rather than pulled in from the game headers: this is the wire contract, so the
 * test keeps its own copy the way a client does and notices a renumber, a
 * relabel or a timing parameter that moved.
 */
struct as_want {
	u8  code;
	u8  kind;
	u8  flags;
	u32 param_us;
	const char *label;
};

#define WANT_DF AUTOSPLIT_FLAG_DEFAULT
#define WANT_RT AUTOSPLIT_FLAG_ROUTE
#define WANT_FL AUTOSPLIT_FLAG_FLAT
#define WANT_NM AUTOSPLIT_FLAG_NORMALISE

static void as_check_describe(const struct game_api *g,
                              const struct as_want *want, u8 want_rows,
                              const char *name)
{
	const struct autosplit_desc *rows;
	u8 n = 0;
	u8 i;
	int rows_ok = 1;
	int route_ok = 1;
	int labels_ok = 1;
	int timing_ok = 1;

	if (g == NULL || g->autosplit_describe == NULL) {
		check(0, "the game declares an autosplit table");
		return;
	}

	rows = g->autosplit_describe(&n);
	check_eq_u64(n, want_rows, name);

	if (rows == NULL || n == 0) return;

	check_eq_u64(rows[0].code, AUTOSPLIT_CODE_PLANET, "code 1 comes first");
	check_eq_u64(rows[0].flags & AUTOSPLIT_FLAG_ROUTE, AUTOSPLIT_FLAG_ROUTE,
	             "code 1 carries the route flag");
	check_eq_u64(rows[0].flags & AUTOSPLIT_FLAG_DEFAULT, AUTOSPLIT_FLAG_DEFAULT,
	             "and is on by default");

	for (i = 0; i < n && i < want_rows; i++) {
		if (rows[i].code != want[i].code) rows_ok = 0;
		if (rows[i].kind != want[i].kind) rows_ok = 0;
		if (rows[i].flags != want[i].flags) rows_ok = 0;
		if (rows[i].param_us != want[i].param_us) rows_ok = 0;
		if (!qstreq(rows[i].label, want[i].label)) rows_ok = 0;
	}
	check(rows_ok, "every row matches the code, kind, flags, param and label");

	for (i = 0; i < n; i++) {
		u8 timing = rows[i].flags & (AUTOSPLIT_FLAG_FLAT | AUTOSPLIT_FLAG_NORMALISE);

		if (rows[i].code == 0) rows_ok = 0;
		if (qstrlen(rows[i].label) > AUTOSPLIT_LABEL_LEN) labels_ok = 0;
		if (i > 0 && (rows[i].flags & AUTOSPLIT_FLAG_ROUTE) != 0) route_ok = 0;

		/* At most one timing flag, and a timing flag always names a parameter. */
		if (timing == (AUTOSPLIT_FLAG_FLAT | AUTOSPLIT_FLAG_NORMALISE)) timing_ok = 0;
		if (timing != 0 && rows[i].param_us == 0) timing_ok = 0;
		if (timing == 0 && rows[i].param_us != 0) timing_ok = 0;
	}
	check(labels_ok, "every label fits the wire field");
	check(route_ok, "and only code 1 carries the route flag");
	check(timing_ok, "a row carries at most one timing flag, with a parameter");
}

/* The four tables, exactly as a client would ship them. */
static const struct as_want rac1_want[] = {
	{ 1, AUTOSPLIT_SPLIT,      WANT_DF | WANT_RT, 0,       "Planet entered" },
	{ 2, AUTOSPLIT_SPLIT,      WANT_DF,           0,       "Veldin" },
	{ 3, AUTOSPLIT_SPLIT,      WANT_DF,           0,       "Drek button" },
	{ 8, AUTOSPLIT_LOAD_START, WANT_DF | WANT_NM, 7560000, "Loading screen" }
};

static const struct as_want rac2_want[] = {
	{ 1,  AUTOSPLIT_SPLIT,      WANT_DF | WANT_RT, 0,      "Planet entered" },
	{ 2,  AUTOSPLIT_SPLIT,      WANT_DF | WANT_FL, 116667, "Protopet defeated" },
	{ 3,  AUTOSPLIT_SPLIT,      WANT_DF,           0,      "Aranos 2 Clank swap" },
	{ 4,  AUTOSPLIT_SPLIT,      0,                 0,      "Maktar arena entry" },
	{ 5,  AUTOSPLIT_SPLIT,      0,                 0,      "Barlow race entry" },
	{ 6,  AUTOSPLIT_SPLIT,      0,                 0,      "Endako Clank entry" },
	{ 7,  AUTOSPLIT_SPLIT,      0,                 0,      "Endako Clank exit" },
	{ 8,  AUTOSPLIT_SPLIT,      0,                 0,      "Tabora caves" },
	{ 9,  AUTOSPLIT_LOAD_START, WANT_DF | WANT_FL, 16667,  "Load transition: slide" },
	{ 10, AUTOSPLIT_LOAD_START, WANT_DF | WANT_FL, 150000, "Load transition: curved" },
	{ 11, AUTOSPLIT_LOAD_START, WANT_DF | WANT_FL, 350000, "Load transition: wipe" }
};

static const struct as_want rac3_want[] = {
	{ 1, AUTOSPLIT_SPLIT,      WANT_DF | WANT_RT, 0,       "Planet entered" },
	{ 5, AUTOSPLIT_SPLIT,      WANT_DF,           0,       "Biobliterator defeated" },
	{ 2, AUTOSPLIT_SPLIT,      0,                 0,       "LDF entered" },
	{ 4, AUTOSPLIT_SPLIT,      0,                 0,       "Koros bolt 2" },
	{ 3, AUTOSPLIT_SPLIT,      0,                 0,       "Tyhrraguise obtained" },
	{ 6, AUTOSPLIT_LOAD_START, WANT_DF | WANT_FL, 1000000, "Long load" }
};

static const struct as_want rac4_want[] = {
	{ 1, AUTOSPLIT_SPLIT, WANT_DF | WANT_RT, 0,        "Planet entered" },
	{ 2, AUTOSPLIT_SPLIT, WANT_DF,           0,        "Vox defeated" },
	{ 3, AUTOSPLIT_PAUSE, WANT_DF | WANT_NM, 14800000, "Quit to XMB" }
};

#define WANT_ROWS(t) (u8)(sizeof(t) / sizeof((t)[0]))

static void test_autosplit(void)
{
	u32 mark;

	group("autosplitting: RaC1");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");

	as_check_describe(session_game(), rac1_want, WANT_ROWS(rac1_want),
	                  "RaC1 advertises only its four supported reason codes");

	/* Disabling injection must leave every former cave and hook untouched. */
	{
		const u32 sites[] = {A1_CAVE_GB, A1_CAVE_SP, A1_CAVE_ITEM, A1_CAVE_IB,
		                     A1_HOOK_GB, A1_HOOK_SP, A1_HOOK_ITEM, A1_HOOK_IB};
		u32 i;
		for (i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
			u8 word[4];
			host_peek(sites[i], word, 4);
			check_eq_u64(be32_get(word), 0, "boot leaves the autosplit cave/hook untouched");
		}
	}

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

	/* Retired menu options must not generate hidden splits, even if the old
	 * counters or the fallback collectible bytes change. */
	mark = autosplit_latest_seq();
	poke32(A1_GB, 1);
	poke32(A1_SP, 2);
	poke32(A1_ITEMS, 3);
	poke32(A1_INFOBOTS, 4);
	poke8(A1_CODEBOT, 1);
	poke8(A1_CODEBOT + 1, 1);
	poke8(A1_KALEBO, 1);
	pump(2);
	check(autosplit_latest_seq() == mark, "unsupported collectable changes emit no hidden split");

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

	/*
	 * The loading screen, revision 1.5: anything but 4 is a load. The pair is
	 * what the client normalises to 7.56 s, so both ends have to show up and
	 * neither may fire on a change that stays on the same side of 4.
	 */
	mark = autosplit_latest_seq();
	poke8(A1_LOADSCR, 4);
	pump(2);
	check(as_find(mark, AUTOSPLIT_LOAD_START, R1_AS_LOADING) == NULL,
	      "settling on the idle loading-screen id starts no load");

	mark = autosplit_latest_seq();
	poke8(A1_LOADSCR, 0);
	pump(1);
	as_expect(mark, AUTOSPLIT_LOAD_START, R1_AS_LOADING, 0,
	          "leaving id 4 emits LOAD_START");
	check(as_find(mark, AUTOSPLIT_LOAD_END, R1_AS_LOADING) == NULL,
	      "and nothing closes it yet");

	mark = autosplit_latest_seq();
	poke8(A1_LOADSCR, 2);
	pump(1);
	check(as_find(mark, AUTOSPLIT_LOAD_START, R1_AS_LOADING) == NULL &&
	      as_find(mark, AUTOSPLIT_LOAD_END, R1_AS_LOADING) == NULL,
	      "a change between two loading ids emits nothing");

	mark = autosplit_latest_seq();
	poke8(A1_LOADSCR, 4);
	pump(1);
	as_expect(mark, AUTOSPLIT_LOAD_END, R1_AS_LOADING, 4,
	          "and coming back to 4 emits LOAD_END");

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

	as_check_describe(session_game(), rac2_want, WANT_ROWS(rac2_want),
	                  "RaC2 declares eleven reason codes");

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

	/*
	 * The three load transitions the script paid a fixed toll for, keyed on the
	 * value the byte changed *to*. Every other value costs nothing, exactly as
	 * its `norm == 0.0` case did.
	 */
	mark = autosplit_latest_seq();
	poke8(A2_LOADSCR, 2);
	pump(2);
	check(as_find(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_SLIDE) == NULL &&
	      as_find(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_CURVED) == NULL &&
	      as_find(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_WIPE) == NULL,
	      "a change to an unpriced load screen costs nothing");

	mark = autosplit_latest_seq();
	poke8(A2_LOADSCR, 0);
	pump(1);
	as_expect_coded(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_SLIDE,
	                "load screen 0 emits the slide transition");

	mark = autosplit_latest_seq();
	poke8(A2_LOADSCR, 1);
	pump(1);
	as_expect_coded(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_CURVED,
	                "load screen 1 emits the curved transition");

	mark = autosplit_latest_seq();
	poke8(A2_LOADSCR, 3);
	pump(1);
	as_expect_coded(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_WIPE,
	                "load screen 3 emits the wipe transition");

	mark = autosplit_latest_seq();
	pump(2);
	check(as_find(mark, AUTOSPLIT_LOAD_START, R2_AS_LOAD_WIPE) == NULL,
	      "and a load screen that holds still emits nothing");

	check(as_find(mark, AUTOSPLIT_PAUSE, 0) == NULL, "RaC2 never pauses");

	/* ------------------------------------------------------------- RaC3 */

	group("autosplitting: RaC3");

	check(quit_and_wait(), "quit RaC2");
	check(boot_and_wait("NPEA00387"), "RaC3 boots");

	as_check_describe(session_game(), rac3_want, WANT_ROWS(rac3_want),
	                  "RaC3 declares six reason codes");

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

	/*
	 * The long load: a second off whenever the loading screen becomes 1, unless
	 * either end of the trip is one of the five planets whose screen only looks
	 * long. Park somewhere ordinary first, so vars.originPlanet is not one of
	 * them, then run one that counts and one that must not.
	 */
	mark = autosplit_latest_seq();
	poke32(A3_PLANET, 4);
	poke32(A3_DEST_PLANET, 0);
	poke8(A3_LOADSCR, 0);
	pump(3);

	mark = autosplit_latest_seq();
	poke32(A3_DEST_PLANET, 7);
	pump(1);
	poke8(A3_LOADSCR, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_LOAD_START, R3_AS_LONG_LOAD, 7,
	          "a long load between two ordinary planets counts");

	mark = autosplit_latest_seq();
	poke8(A3_LOADSCR, 0);
	pump(1);
	as_expect(mark, AUTOSPLIT_LOAD_END, R3_AS_LONG_LOAD, 7,
	          "and leaving the loading screen closes it");

	/* Now one bound for the Launch site, which vars.llIgnorePlanets excludes. */
	mark = autosplit_latest_seq();
	poke32(A3_DEST_PLANET, 20);
	pump(1);
	poke8(A3_LOADSCR, 1);
	pump(1);
	check(as_find(mark, AUTOSPLIT_LOAD_START, R3_AS_LONG_LOAD) == NULL,
	      "a load to an ignored planet does not");

	mark = autosplit_latest_seq();
	poke8(A3_LOADSCR, 0);
	pump(1);
	check(as_find(mark, AUTOSPLIT_LOAD_END, R3_AS_LONG_LOAD) == NULL,
	      "and it is not closed either, because it never opened");

	/* The origin end of the same test: leaving an ignored planet is ignored. */
	mark = autosplit_latest_seq();
	poke32(A3_DEST_PLANET, 0);
	poke32(A3_PLANET, 26);
	pump(2);
	poke32(A3_PLANET, 5);
	poke32(A3_DEST_PLANET, 5);
	pump(1);
	poke8(A3_LOADSCR, 1);
	pump(1);
	check(as_find(mark, AUTOSPLIT_LOAD_START, R3_AS_LONG_LOAD) == NULL,
	      "nor does one leaving an ignored planet");
	poke8(A3_LOADSCR, 0);
	pump(1);

	check(as_find(mark, AUTOSPLIT_PAUSE, 0) == NULL, "RaC3 never pauses");

	/* ------------------------------------------------------ Deadlocked */

	group("autosplitting: Deadlocked");

	check(quit_and_wait(), "quit RaC3");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots");

	as_check_describe(session_game(), rac4_want, WANT_ROWS(rac4_want),
	                  "Deadlocked declares three reason codes");

	/* The loading hook goes in on entry, beside the quit hook. */
	{
		u8 word[4];

		host_peek(A4_LOADING_H1, word, 4);
		check_eq_u64(be32_get(word), 0x48000080u,
		             "the branch into the loading trampoline is written");
		host_peek(A4_LOADING_H2, word, 4);
		check_eq_u64(be32_get(word), 0x9421FFF0u, "the trampoline's first word");
		host_peek(A4_LOADING_H2 + 0x24, word, 4);
		check_eq_u64(be32_get(word), 0x4BFFFF60u, "and its branch home");
		host_peek(A4_LOADING_VAL, word, 1);
		check_eq_u64(word[0], 0, "and its byte starts clear");
	}

	/*
	 * The quit-hook group quit Deadlocked earlier in this run, so a RESUME is
	 * still outstanding and revision 1.5 will not answer it until the game says
	 * the logo is up. Say so, the way the console would, before the PAUSE and
	 * RESUME pair is tested for real below.
	 */
	mark = autosplit_latest_seq();
	poke8(A4_LOADING_VAL, 0xFF);
	pump(2);
	as_expect_coded(mark, AUTOSPLIT_RESUME, R4_AS_QUIT,
	                "the outstanding RESUME from the earlier quit lands");

	/*
	 * The origin rule. RAC4_AS_PLANET is a saved word and still holds the save's
	 * planet after a boot, so the watcher keeps its own origin instead: a fresh
	 * session starts in the main menu, and the first load out of it is where the
	 * run starts rather than a planet change.
	 */
	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_IN_GAME, 1);
	poke32(A4_TUTORIAL, 1);
	poke32(A4_PLANET, 5);      /* what the save left in the word */
	poke32(A4_TARGET, 5);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET) == NULL,
	      "the first load out of the main menu does not split");

	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_TARGET, 6);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	as_expect(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET, 6,
	          "and the load after it does, because the origin is a real planet now");

	/* A load to the main menu is not a split, and it puts the origin back to 0. */
	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_TARGET, A4_MAINMENU);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET) == NULL,
	      "a load to the main menu is not a split");

	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_TARGET, 4);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	check(as_find(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET) == NULL,
	      "and the load out of the menu after it is not either");

	/* The SPRX's reset_needed: a load starts for DreadZone with no tutorial flag. */
	mark = autosplit_latest_seq();
	poke32(A4_TARGET, 1);
	poke32(A4_TUTORIAL, 0);
	poke32(A4_REQUEST_LOAD, 0);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(1);
	as_expect_kind(mark, AUTOSPLIT_RESET, "a DreadZone load emits RESET");
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

	/*
	 * PAUSE on the way out to the XMB, and RESUME only once the game says it is
	 * past the SCE logo. Revision 1.5 moved the RESUME onto the old SPRX's
	 * loading hook: qwark reaches INGAME while Deadlocked is still behind its
	 * loading and warning screens, so resuming there hands the runner seconds
	 * the old autosplitter never gave away. Both carry code 3.
	 */
	mark = autosplit_latest_seq();
	check(quit_and_wait(), "Deadlocked quits to the XMB");
	as_expect_coded(mark, AUTOSPLIT_PAUSE, R4_AS_QUIT, "and that emits PAUSE");
	check(as_find(mark, AUTOSPLIT_RESUME, R4_AS_QUIT) == NULL, "with no RESUME yet");

	mark = autosplit_latest_seq();
	check(boot_and_wait("NPEA00423"), "Deadlocked comes back");
	pump(4);
	check(as_find(mark, AUTOSPLIT_RESUME, R4_AS_QUIT) == NULL,
	      "the process coming back is not yet a RESUME");

	poke8(A4_LOADING_VAL, 0xFF);
	pump(1);
	as_expect_coded(mark, AUTOSPLIT_RESUME, R4_AS_QUIT,
	                "the loading hook's 0xFF emits RESUME");
	check(as_find(mark, AUTOSPLIT_PAUSE, R4_AS_QUIT) == NULL,
	      "and only the one RESUME");

	{
		u8 b = 0xFF;
		host_peek(A4_LOADING_VAL, &b, 1);
		check_eq_u64(b, 0, "and the byte is cleared for the next quit");
	}

	/* The sequence survives the reboot: it counts for the life of the module. */
	check(autosplit_latest_seq() > mark, "the sequence carried on across the boot");

	/* Back to RaC1 for anything that follows. */
	check(quit_and_wait(), "quit Deadlocked");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again");
}

/* ------------------------------------------- the platform that cannot patch */

/* Feature ids and one address, held here the way a client holds them. */
#define F_INFINITE_AMMO   1
#define F_GHOST           3
#define F_BOLTS           5
#define RAC1_BOLTS_ADDR   0x969CA0u
#define R4_CRASH_PATCHES  0
#define R4_SOFTLOCK_FIX   1

/*
 * Everything the core refuses when plat_can_patch_code() says no, which is what
 * qwark-rpcs3.exe answers because RPCS3 recompiles PPU code. The fake backend
 * lets the tests force that answer, so the whole gate is exercised here rather
 * than only against a running emulator.
 */
static void test_no_code_patches(void)
{
	u8 info[SESSION_INFO_SIZE];
	u32 mark;
	u32 v = 0;
	u8 word[4];

	group("no code patches: the platform gate");

	host_set_can_patch_code(0);
	host_set_emulator(1);

	/* A fresh entry, so on_enter runs with the gate closed. */
	check(quit_and_wait(), "quit RaC1");
	check(boot_and_wait("NPEA00385"), "RaC1 boots with the gate closed");

	/* ------------------------------------------------- SessionInfo flags */

	pump(8);   /* telemetry is published every fourth tick */
	check_eq_u64(session_info_copy(info, sizeof(info)), SESSION_INFO_SIZE,
	             "a SessionInfo snapshot is published");
	check((info[24] & SESSION_FLAG_EMULATOR) != 0,
	      "SessionInfo flags bit1 EMULATOR is set");
	check((info[24] & SESSION_FLAG_NO_CODE_PATCHES) != 0,
	      "SessionInfo flags bit2 NO_CODE_PATCHES is set");

	/* ------------------------------------------------------ the helpers */

	host_peek(A1_HOOK_GB, word, 4);
	check(be32_get(word) != 0x004F5BE4u,
	      "RaC1's autosplit helper was not installed");

	/* ------------------------------------------------- the savefile helper */

	/*
	 * The savefile helper is a code cave and a branch into it, so on a platform
	 * that cannot patch code there is nothing to install and nothing to talk to:
	 * all three ops and both request actions say so rather than half-working.
	 */
	{
		u8 sup = 9, ins = 9, run = 9, pend = 9;
		u32 sz = 9;
		u32 got = 0;

		check(savefile_info(&sup, &ins, &run, &pend, &sz) == ST_UNSUPPORTED,
		      "SAVEFILE_INFO is UNSUPPORTED");
		check(savefile_read(0, 4, word, &got) == ST_UNSUPPORTED,
		      "and so is SAVEFILE_READ");
		check(savefile_write(0, word, 4) == ST_UNSUPPORTED, "and SAVEFILE_WRITE");
		check(savefile_install() == ST_UNSUPPORTED, "and installing the helper");
		check(features_trigger(19) == ST_UNSUPPORTED,
		      "the SAVE_ASIDE action is UNSUPPORTED");
		check(features_trigger(18) == ST_UNSUPPORTED, "and the LOAD_ASIDE one");
		check(session_load_setaside() == ST_UNSUPPORTED, "and the combo action");

		/*
		 * Protocol 1.10. The library on the console is the same code cave by
		 * another road: without the helper there is nothing to copy out of and
		 * nothing to hand a copied file to, so the five new ops go the same way.
		 */
		check(savefile_store("runs", "one.sav") == ST_UNSUPPORTED,
		      "SAVEFILE_STORE is UNSUPPORTED");
		check(savefile_restore("runs", "one.sav") == ST_UNSUPPORTED,
		      "and SAVEFILE_RESTORE");
		check(savefile_library_gate() == ST_UNSUPPORTED,
		      "and the gate the three library ops share");

		host_peek(sf_desc_for_game(GAME_RAC1)->hooks[0].addr, word, 4);
		check(be32_get(word) != sf_desc_for_game(GAME_RAC1)->hooks[0].value,
		      "and no hook word was written");
	}

	/* -------------------------------------------------------- FEATURE_SET */

	check(features_set(F_INFINITE_AMMO, 1) == ST_UNSUPPORTED,
	      "FEATURE_SET on a WRITES_CODE toggle is UNSUPPORTED");
	check((features_toggle_state() & ((u64)1 << F_INFINITE_AMMO)) == 0,
	      "and its bit stays clear");

	check(features_set(F_GHOST, 1) == ST_OK,
	      "a plain data toggle still works");
	check((features_toggle_state() & ((u64)1 << F_GHOST)) != 0,
	      "and its bit is set");
	features_set(F_GHOST, 0);

	check(features_set(F_BOLTS, 1234) == ST_OK, "a VALUE feature still writes");
	check(mem_read_u32(RAC1_BOLTS_ADDR, &v) == ST_OK && v == 1234,
	      "and the bolt count landed in memory");

	check(freeze_add(0x00700100u, 4, 99, NULL) == ST_OK, "a freeze still registers");
	freeze_clear();

	/* ---------------------------------------------------------- PATCH_ADD */

	check(patch_apply(&test_patch) == ST_UNSUPPORTED,
	      "patch_apply is UNSUPPORTED");
	check(client_patch_apply(test_words, 2) == ST_UNSUPPORTED,
	      "and so is a client PATCH_APPLY");
	client_patch_drop_all();

	/* ----------------------------------------------------------- MOD_LOAD */

	check(mods_load("incremental_rng") == ST_UNSUPPORTED,
	      "MOD_LOAD of a mod with a code cave is UNSUPPORTED");
	check(mods_load("hardcore") == ST_UNSUPPORTED,
	      "MOD_LOAD of a mod with patch words is UNSUPPORTED");
	check_eq_u64(mods_loaded_mask(), 0, "and nothing is marked loaded");

	/* ------------------------------------ auto-flagged toggles at boot */

	check(quit_and_wait(), "quit RaC1");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots with the gate closed");

	check((features_toggle_state() & ((u64)1 << R4_CRASH_PATCHES)) == 0,
	      "the auto-flagged crash patches were skipped at boot");
	check((features_toggle_state() & ((u64)1 << R4_SOFTLOCK_FIX)) != 0,
	      "while the auto-flagged data toggle came back as usual");

	host_peek(A4_LOADING_H1, word, 4);
	check(be32_get(word) != 0x48000080u,
	      "Deadlocked's loading hook was not installed");

	/* ----------------------------- the process-vanished PAUSE and RESUME */

	mark = autosplit_latest_seq();
	check(quit_and_wait(), "the game goes away");
	as_expect_coded(mark, AUTOSPLIT_PAUSE, R4_AS_QUIT,
	                "PAUSE still fires on the process-vanished path");

	mark = autosplit_latest_seq();
	check(boot_and_wait("NPEA00423"), "Deadlocked comes back");
	pump(4);
	as_expect_coded(mark, AUTOSPLIT_RESUME, R4_AS_QUIT,
	                "and RESUME fires on entry, with no loading hook to wait for");

	/* ------------------------------------------------- put it all back */

	host_set_can_patch_code(1);
	host_set_emulator(0);

	check(quit_and_wait(), "quit Deadlocked");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again with the gate open");

	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & (SESSION_FLAG_EMULATOR | SESSION_FLAG_NO_CODE_PATCHES)) == 0,
	      "and neither flag is set any more");
	check(features_set(F_INFINITE_AMMO, 1) == ST_OK,
	      "a WRITES_CODE toggle works again");
	features_set(F_INFINITE_AMMO, 0);
	features_forget_state();
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

	/* RaC1 is INGAME: use its supported planet split for UDP coverage. */
	poke32(A1_PLANET, 3);
	poke32(A1_DEST_PLANET, 0);
	pump(1);
	mark = autosplit_latest_seq();
	poke32(A1_DEST_PLANET, 5);
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
		check_eq_u64(dgram[4 + 9], R1_AS_PLANET, "with the planet code");
		check_eq_u64(be32_get(dgram + 4 + 12), 5, "and the destination as its arg");
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
	test_patch_order();
	test_tables();
	test_mods();
	test_describe();
	test_telemetry();
	test_session_same_title();
	test_session_different_title();
	test_boot();
	test_launch_failures();
	test_unlocks();
	test_levelflags();
	test_planet_load();
	test_savefile_requests();
	test_debug_options();
	test_live_toggles();
	test_savefile_flags();
	test_savefile_helper();
	test_savefile_library();
	test_signed_values();
	test_combo_suspend();
	test_rac2();
	test_rac3();
	test_rac4();
	test_trilogy();
	test_autosplit();
	test_no_code_patches();
	test_config();
	test_fingerprint();
	test_submit_after_stop();
	test_net_stop_with_client();
	test_pine();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");

	plat_shutdown();
	return g_failures == 0 ? 0 : 1;
}
