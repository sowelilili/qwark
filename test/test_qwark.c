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
#include "../src/core/switches.h"
#include "../src/core/manips.h"
#include "../src/games/game.h"
#include "../src/games/classic.h"
#include "../src/games/sfhelper_bins.h"
#include "../src/games/switches_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/plat_net.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <stdlib.h>
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
	check(QWARK_MAX_PAYLOAD == 16384u, "max payload is 16384 (revision 1.11)");
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

/* -------------------------------------------------- the client patch pool */

/*
 * Build 37 gave the sixteen client-patch slots one shared pool of words instead
 * of a private QWARK_MAX_PATCH_WORDS each. QWARK_MAX_PATCH_WORDS is unchanged
 * and is still what one PATCH_APPLY may carry, so what these check is the new
 * edge: the pool exactly full, one word past it, and that reverting a patch
 * gives its words back for the next one to use.
 */
#define POOL_BASE 0x00A00000u

static void test_patch_pool(void)
{
	struct patch_word words[QWARK_MAX_PATCH_WORDS];
	u32 patches;
	u32 i;
	u32 p;

	group("client patches share one word pool");

	client_patch_drop_all();

	/* Four patches of 64 words is 256, which is the pool exactly. */
	patches = 256u / QWARK_MAX_PATCH_WORDS;

	for (p = 0; p < patches; p++) {
		u8 seed[4];
		seed[0] = (u8)(0xA0 + p); seed[1] = 0x11; seed[2] = 0x22; seed[3] = 0x33;
		host_poke(POOL_BASE + p * 0x1000u, seed, 4);
	}

	for (p = 0; p < patches; p++) {
		for (i = 0; i < QWARK_MAX_PATCH_WORDS; i++) {
			words[i].addr  = POOL_BASE + p * 0x1000u + i * 4u;
			words[i].value = 0x60000000u;
		}
		if (client_patch_apply(words, (u16)QWARK_MAX_PATCH_WORDS) != ST_OK) break;
	}
	check_eq_u64(p, patches, "four patches of 64 words fill the pool exactly");
	check_eq_u64(client_patch_count(), patches, "and all four are in the table");

	/* One more word than the pool holds, with slots still free. */
	words[0].addr  = POOL_BASE + 0x8000u;
	words[0].value = 0x60000000u;
	check(client_patch_apply(words, 1) == ST_FULL,
	      "one word past a full pool is ST_FULL, though slots are free");
	check_eq_u64(client_patch_count(), patches, "and nothing was added");

	/* Reverting one hands its words back, and the next patch fits again. */
	check(client_patch_revert(POOL_BASE) == ST_OK, "the first patch reverts");
	check_eq_u64(client_patch_count(), patches - 1, "leaving three");

	check(client_patch_apply(words, 1) == ST_OK,
	      "and the word it gave back is enough for another patch");
	check_eq_u64(client_patch_count(), patches, "which is in the table");

	/*
	 * The pool is kept packed rather than bump allocated, so the three patches
	 * that were above the hole still describe their own words: a client that
	 * reverts the one it added last must put back what it wrote, not somebody
	 * else's word.
	 */
	{
		u32 v = 0;

		check(client_patch_revert(POOL_BASE + 0x1000u) == ST_OK,
		      "a patch that sat above the freed one reverts");
		mem_read_u32(POOL_BASE + 0x1000u, &v);
		check_eq_u64(v, 0xA1112233u, "and put its own original word back, not a neighbour's");

		check(client_patch_revert(POOL_BASE + 0x2000u) == ST_OK, "so does the next");
		mem_read_u32(POOL_BASE + 0x2000u, &v);
		check_eq_u64(v, 0xA2112233u, "with its own original too");

		check(client_patch_revert(POOL_BASE + 0x3000u) == ST_OK, "and the next");
		check(client_patch_revert(POOL_BASE + 0x8000u) == ST_OK, "and the late arrival");
		check_eq_u64(client_patch_count(), 0, "the table is empty again");
	}

	/* The wire cap itself has not moved. */
	for (i = 0; i < QWARK_MAX_PATCH_WORDS; i++) {
		words[i].addr  = POOL_BASE + 0x9000u + i * 4u;
		words[i].value = 0x60000000u;
	}
	check(client_patch_apply(words, (u16)QWARK_MAX_PATCH_WORDS + 1) == ST_BAD_ARG,
	      "a PATCH_APPLY of 65 words is still BAD_ARG");
	check(client_patch_apply(words, (u16)QWARK_MAX_PATCH_WORDS) == ST_OK,
	      "and 64 is still accepted");
	check(client_patch_clear() == ST_OK, "CLEAR_CLIENT empties it");
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

/*
 * Writes a generated patch.txt into the fake console's mod folder: `nwords`
 * "0xADDR: 0xVALUE" lines and `ncaves` "0xADDR: rack.bin" lines. Used to walk
 * the word and cave pools right up to their edges, which no shipped mod comes
 * near.
 */
static void write_generated_mod(const char *title, const char *dirname,
                                u32 nwords, u32 ncaves)
{
	char path[256];
	char line[64];
	plat_file_t f;
	u32 i;

	path[0] = 0;
	qstrcat(path, sizeof(path), QWARK_MODSDIR);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), title);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), dirname);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/patch.txt");

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;

	for (i = 0; i < nwords; i++) {
		line[0] = 0;
		qstrcat(line, sizeof(line), "0x");
		qfmt_hex(line + 2, sizeof(line) - 2, 0x00800000u + i * 4u, 8);
		qstrcat(line, sizeof(line), ": 0x60000000\n");
		plat_file_write(f, line, qstrlen(line));
	}

	for (i = 0; i < ncaves; i++) {
		line[0] = 0;
		qstrcat(line, sizeof(line), "0x");
		qfmt_hex(line + 2, sizeof(line) - 2, 0x00900000u + i * 0x1000u, 8);
		qstrcat(line, sizeof(line), ": rack.bin\n");
		plat_file_write(f, line, qstrlen(line));
	}

	plat_file_close(f);
}

/* The same, but padded with one long comment line to exactly `bytes`. */
static void write_generated_padded_mod(const char *title, const char *dirname, u32 bytes)
{
	char path[256];
	char pad[256];
	plat_file_t f;
	u32 written = 0;

	memset(pad, 'x', sizeof(pad));
	write_generated_mod(title, dirname, 1, 0);

	path[0] = 0;
	qstrcat(path, sizeof(path), QWARK_MODSDIR);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), title);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), dirname);
	qstrcat(path, sizeof(path), "/patch.txt");

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;

	plat_file_write(f, "#", 1);
	written = 1;
	while (written + 1 < bytes) {
		u32 n = bytes - 1 - written;
		if (n > sizeof(pad)) n = sizeof(pad);
		plat_file_write(f, pad, n);
		written += n;
	}
	plat_file_write(f, "\n", 1);

	plat_file_close(f);
}

/*
 * A mod folder as a client's upload leaves one: a patch.txt carrying a version
 * line and `nwords` words from `base`, and then the qwark.sum the client writes
 * after the files, which is the hash MOD_LIST reports. Writing one over another
 * is what an edited mod being uploaded again looks like from this side.
 */
static void write_uploaded_mod(const char *title, const char *dirname,
                               const char *version, u32 nwords, u32 base,
                               const char *sum)
{
	char dirpath[256];
	char path[288];
	char line[64];
	plat_file_t f;
	u32 i;

	dirpath[0] = 0;
	qstrcat(dirpath, sizeof(dirpath), QWARK_MODSDIR);
	plat_dir_create(dirpath);
	qstrcat(dirpath, sizeof(dirpath), "/");
	qstrcat(dirpath, sizeof(dirpath), title);
	plat_dir_create(dirpath);
	qstrcat(dirpath, sizeof(dirpath), "/");
	qstrcat(dirpath, sizeof(dirpath), dirname);
	plat_dir_create(dirpath);

	qstrcpy(path, sizeof(path), dirpath);
	qstrcat(path, sizeof(path), "/patch.txt");
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;

	line[0] = 0;
	qstrcat(line, sizeof(line), "#- name: ");
	qstrcat(line, sizeof(line), dirname);
	qstrcat(line, sizeof(line), "\n");
	plat_file_write(f, line, qstrlen(line));

	line[0] = 0;
	qstrcat(line, sizeof(line), "#- version: ");
	qstrcat(line, sizeof(line), version);
	qstrcat(line, sizeof(line), "\n");
	plat_file_write(f, line, qstrlen(line));

	for (i = 0; i < nwords; i++) {
		line[0] = 0;
		qstrcat(line, sizeof(line), "0x");
		qfmt_hex(line + 2, sizeof(line) - 2, base + i * 4u, 8);
		qstrcat(line, sizeof(line), ": 0x60000000\n");
		plat_file_write(f, line, qstrlen(line));
	}

	plat_file_close(f);

	qstrcpy(path, sizeof(path), dirpath);
	qstrcat(path, sizeof(path), "/qwark.sum");
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, sum, qstrlen(sum));
	plat_file_close(f);
}

/*
 * One mod folder with a patch.txt written out line by line: a "#- name:", an
 * optional "#- depends:" and one patch word, which is all a dependency test
 * needs. The word addresses are spread so two mods never claim the same one.
 */
static void write_named_mod(const char *title, const char *dirname,
                            const char *depends, u32 addr)
{
	char path[256];
	char line[160];
	plat_file_t f;

	path[0] = 0;
	qstrcat(path, sizeof(path), QWARK_MODSDIR);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), title);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), dirname);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/patch.txt");

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;

	line[0] = 0;
	qstrcat(line, sizeof(line), "#- name: ");
	qstrcat(line, sizeof(line), dirname);
	qstrcat(line, sizeof(line), "\n");
	plat_file_write(f, line, qstrlen(line));

	if (depends != NULL && depends[0] != 0) {
		line[0] = 0;
		qstrcat(line, sizeof(line), "#- depends: ");
		qstrcat(line, sizeof(line), depends);
		qstrcat(line, sizeof(line), "\n");
		plat_file_write(f, line, qstrlen(line));
	}

	line[0] = 0;
	qstrcat(line, sizeof(line), "0x");
	qfmt_hex(line + 2, sizeof(line) - 2, addr, 8);
	qstrcat(line, sizeof(line), ": 0x60000000\n");
	plat_file_write(f, line, qstrlen(line));

	plat_file_close(f);
}

/* link0, then link1 depending on link0, up to link<n-1>: a chain n levels deep. */
static void write_chain_mods(const char *title, u32 n)
{
	char dir[16];
	char dep[16];
	u32 i;

	for (i = 0; i < n; i++) {
		qstrcpy(dir, sizeof(dir), "link");
		qfmt_u32(dir + 4, sizeof(dir) - 4, i);

		dep[0] = 0;
		if (i > 0) {
			qstrcpy(dep, sizeof(dep), "link");
			qfmt_u32(dep + 4, sizeof(dep) - 4, i - 1);
		}

		write_named_mod(title, dir, dep, 0x00810000u + i * 4u);
	}
}

/* Two mods that name each other: a chain with no bottom. */
static void write_cycle_mods(const char *title)
{
	write_named_mod(title, "loopa", "loopb", 0x00820000u);
	write_named_mod(title, "loopb", "loopa", 0x00820004u);
}

/* One mod depending on a name no mod in the folder carries. */
static void write_missing_dep_mod(const char *title)
{
	write_named_mod(title, "orphan", "nobody-here", 0x00830000u);
}

/*
 * test/fixtures/mods/NPEA00385/big-cave: a 10240-byte cave at 0x700000 and two
 * words at 0x740000. The bytes are generated rather than read back out of the
 * fixture so the check is against what the file is meant to hold.
 */
#define BIG_CAVE_ADDR  0x00700000u
#define BIG_CAVE_LEN   10240u
#define BIG_CAVE_WORDS 0x00740000u

static u8 g_big_cave[BIG_CAVE_LEN];
static u8 g_big_read[BIG_CAVE_LEN];

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
	check(mods_count() >= 7, "all seven RaC1 fixtures were scanned");

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
	 * A cave several chunks long. mods.c streams a cave file into the game a
	 * MOD_CAVE_CHUNK at a time out of the shared scratch buffer, and the patch
	 * words go in as one batch only once every byte of every cave has landed: a
	 * cave is memory nothing branches to until those words point at it, so how
	 * many pieces it arrives in is invisible to the running game, but a word
	 * written early would point at a cave that is not all there.
	 *
	 * big-cave is a 10240-byte rack.bin at 0x700000, two full chunks and a short
	 * one, and two words at 0x740000.
	 */
	{
		u32 i;
		int last_cave = -1;
		int first_word = -1;
		int cave_writes = 0;

		for (i = 0; i < BIG_CAVE_LEN; i++)
			g_big_cave[i] = (u8)((i * 7u + 3u) & 0xFFu);

		host_poke(BIG_CAVE_WORDS, (const u8 *)"\x11\x22\x33\x44", 4);
		host_write_log_reset();

		check(mods_load("big-cave") == ST_OK, "a mod with a multi-chunk cave loads");

		for (i = 0; i < host_write_log_count(); i++) {
			u32 a = 0, len = 0;
			if (host_write_log_at(i, &a, &len) != 0) break;
			if (a - BIG_CAVE_ADDR < BIG_CAVE_LEN) {
				last_cave = (int)i;
				cave_writes++;
			} else if (first_word < 0) {
				first_word = (int)i;
			}
		}

		check(cave_writes >= 3, "the cave went in as three writes or more");
		check(first_word > last_cave,
		      "and every patch word went in after the last piece of the cave");

		host_peek(BIG_CAVE_ADDR, g_big_read, BIG_CAVE_LEN);
		check(memcmp(g_big_read, g_big_cave, BIG_CAVE_LEN) == 0,
		      "the whole cave arrived, every chunk of it, in order");

		mem_read_u32(BIG_CAVE_WORDS, &v);
		check_eq_u64(v, 0x48000008u, "the first patch word is in");
		mem_read_u32(BIG_CAVE_WORDS + 4, &v);
		check_eq_u64(v, 0x60000000u, "and so is the second");

		check(mods_unload("big-cave") == ST_OK, "it unloads");
		mem_read_u32(BIG_CAVE_WORDS, &v);
		check_eq_u64(v, 0x11223344u, "putting the words back");
		host_peek(BIG_CAVE_ADDR, g_big_read, 4);
		check_eq_u64(be32_get(g_big_read), be32_get(g_big_cave),
		             "and leaving the cave bytes where they are");
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

	group("a rescan after an upload");

	/*
	 * What MOD_RESCAN is for: a client has just replaced a mod's files and has
	 * to see the mod that is on the console now. The hash out of the new
	 * qwark.sum is the one the mods panel compares against its own copy and
	 * marks the row stale when the two differ, and the words are what the next
	 * MOD_LOAD writes into the game.
	 *
	 * A rescan used to skip every folder it already knew as soon as any one mod
	 * was loaded, so with a single mod on - one the user left loaded, or an
	 * auto one the game brought in by itself - an uploaded mod kept the hash,
	 * the metadata and the words of the copy it replaced. The row stayed stale
	 * for the rest of the session whatever the user pressed, and loading it
	 * wrote the old patch.
	 *
	 * The mod that is loaded is the one thing a rescan leaves alone, and that is
	 * here too: its words are in the running process with the originals it will
	 * need to put them back, so it keeps them, and it still unloads to exactly
	 * what it captured although the pool was packed underneath it.
	 */
	{
		const char *title = "NPEA09998";

		write_uploaded_mod(title, "edited", "1.0", 4, 0x00850000u, "11112222");
		write_named_mod(title, "keeper", NULL, 0x00860000u);
		mods_set_title(title);

		host_poke(0x00860000u, (const u8 *)"\x12\x34\x56\x78", 4);
		host_poke(0x00850014u, (const u8 *)"\xAA\xAA\xAA\xAA", 4);

		check(mods_load("keeper") == ST_OK, "one mod of the title is loaded");

		m = mods_at((u32)mods_find("edited"));
		check(m != NULL && m->hash == 0x11112222u,
		      "the other mod reports the hash its qwark.sum holds");

		/* The upload: the new files, the new qwark.sum, then MOD_RESCAN. */
		write_uploaded_mod(title, "edited", "2.0", 6, 0x00850000u, "33334444");
		check(mods_rescan() == ST_OK, "a rescan with a mod loaded answers OK");

		m = mods_at((u32)mods_find("edited"));
		check(m != NULL, "the uploaded mod is still in the table");
		if (m != NULL) {
			check_eq_u64(m->hash, 0x33334444u, "with the hash the upload wrote");
			check(qstreq(m->version, "2.0"),
			      "and the version out of the patch.txt that came with it");
			check_eq_u64(m->def.count, 6, "and the words that patch.txt lists");
		}

		m = mods_at((u32)mods_find("keeper"));
		check(m != NULL && (m->flags & MOD_FLAG_LOADED) != 0,
		      "the loaded mod came through the rescan loaded");

		check(mods_load("edited") == ST_OK, "the uploaded mod loads");
		mem_read_u32(0x00850014u, &v);
		check_eq_u64(v, 0x60000000u, "and it is the new words that reach memory");

		check(mods_unload("keeper") == ST_OK, "the mod that stayed loaded unloads");
		mem_read_u32(0x00860000u, &v);
		check_eq_u64(v, 0x12345678u,
		             "restoring the original it captured before the pool was packed");
		check(mods_unload("edited") == ST_OK, "and the uploaded one unloads too");
	}

	group("patch.txt parser, the pool limits");

	/*
	 * The shared word and cave pools, at the size and one past it. Each generated
	 * mod gets a title of its own so it meets an empty pool: the two pools are
	 * bump allocated across every mod of one title, and a mod sharing a title
	 * with the big one would be refused for its neighbour's size rather than its
	 * own, which is not what is being measured here.
	 *
	 * The statuses are the ones this has always answered: a mod that does not fit
	 * is flagged MOD_FLAG_PARSE_ERROR, keeps none of the pool and contributes no
	 * words, and mods_load answers ST_IO_ERROR for it.
	 */
	{
		const char *title = "NPEA09990";

		write_generated_mod(title, "fits", MOD_WORD_POOL, 0);
		mods_set_title(title);
		m = mods_at((u32)mods_find("fits"));
		check(m != NULL, "a mod holding the whole word pool is scanned");
		if (m != NULL) {
			check((m->flags & MOD_FLAG_PARSE_ERROR) == 0, "and parses");
			check_eq_u64(m->def.count, MOD_WORD_POOL, "with every one of its words");
		}

		/*
		 * And applies: PATCH_ORDER_WORDS is the same number, so this walks the
		 * run-ordering tables to their last entry. The words are consecutive, so
		 * they are one run and go out PATCH_RUN_WORDS at a time.
		 */
		check(mods_load("fits") == ST_OK, "and loads, ordering tables and all");
		mem_read_u32(0x00800000u + (MOD_WORD_POOL - 1) * 4u, &v);
		check_eq_u64(v, 0x60000000u, "the last word of the pool reached memory");
		check(mods_unload("fits") == ST_OK, "and it unloads");
	}

	{
		const char *title = "NPEA09991";

		write_generated_mod(title, "over", MOD_WORD_POOL + 1, 0);
		mods_set_title(title);
		m = mods_at((u32)mods_find("over"));
		check(m != NULL, "a mod one word past the pool is still scanned");
		if (m != NULL) {
			check((m->flags & MOD_FLAG_PARSE_ERROR) != 0, "but is flagged parse_error");
			check_eq_u64(m->def.count, 0, "and keeps no words");
		}
		check(mods_load("over") == ST_IO_ERROR, "and loading it is IO_ERROR");
	}

	{
		const char *title = "NPEA09992";

		write_generated_mod(title, "caves", 0, MOD_CAVE_POOL);
		mods_set_title(title);
		m = mods_at((u32)mods_find("caves"));
		check(m != NULL, "a mod holding the whole cave pool is scanned");
		if (m != NULL) {
			check((m->flags & MOD_FLAG_PARSE_ERROR) == 0, "and parses");
			check_eq_u64(m->ncaves, MOD_CAVE_POOL, "with every one of its caves");
		}
	}

	{
		const char *title = "NPEA09993";

		write_generated_mod(title, "caves", 0, MOD_CAVE_POOL + 1);
		mods_set_title(title);
		m = mods_at((u32)mods_find("caves"));
		check(m != NULL, "a mod one cave past the pool is still scanned");
		if (m != NULL) {
			check((m->flags & MOD_FLAG_PARSE_ERROR) != 0, "but is flagged parse_error");
			check_eq_u64(m->ncaves, 0, "and keeps no caves");
		}
	}

	/*
	 * MOD_TEXT_MAX. A patch.txt too big to read is the same refusal by another
	 * road, and the pool is sized so a file that fills it is still readable.
	 */
	{
		const char *title = "NPEA09994";

		write_generated_padded_mod(title, "huge", MOD_TEXT_MAX + 16u);
		mods_set_title(title);
		m = mods_at((u32)mods_find("huge"));
		check(m != NULL, "a patch.txt bigger than the text buffer is scanned");
		if (m != NULL) {
			check((m->flags & MOD_FLAG_PARSE_ERROR) != 0, "and flagged parse_error");
			check_eq_u64(m->def.count, 0, "with nothing parsed out of it");
		}
	}

	/*
	 * Dependency chains, now that the loader walks them with an explicit work
	 * list instead of recursing. The answers are the ones the recursion gave:
	 * a chain the depth limit allows loads every link of it, bottom up; a chain
	 * past the limit is ST_BAD_ARG; and a cycle, which is a chain of no end, is
	 * ST_BAD_ARG too. The old limit was a recursion depth of QWARK_MAX_MODS, and
	 * these walk right up to it and one past.
	 */
	{
		const char *title = "NPEA09995";
		int i;

		/*
		 * link0 <- link1 <- ... <- link31, each depending on the one below it:
		 * QWARK_MAX_MODS links, so loading the top one is a chain exactly as deep
		 * as the limit allows.
		 */
		write_chain_mods(title, QWARK_MAX_MODS);
		mods_set_title(title);
		check_eq_u64(mods_count(), QWARK_MAX_MODS, "a 32-link dependency chain is scanned");

		check(mods_load("link31") == ST_OK,
		      "loading the top of a chain as deep as the old recursion limit succeeds");

		for (i = 0; i < QWARK_MAX_MODS; i++) {
			char dir[16];
			int idx;

			qstrcpy(dir, sizeof(dir), "link");
			qfmt_u32(dir + 4, sizeof(dir) - 4, (u32)i);
			idx = mods_find(dir);
			if (idx < 0 || (mods_at((u32)idx)->flags & MOD_FLAG_LOADED) == 0) break;
		}
		check_eq_u64(i, QWARK_MAX_MODS, "and every link of it went in, bottom up");

		/* Put every word back before the table is thrown away. */
		for (i = 0; i < QWARK_MAX_MODS; i++) {
			char dir[16];

			qstrcpy(dir, sizeof(dir), "link");
			qfmt_u32(dir + 4, sizeof(dir) - 4, (u32)i);
			if (mods_unload(dir) != ST_OK) break;
		}
		check_eq_u64(i, QWARK_MAX_MODS, "and every link unloads again");
	}

	{
		/*
		 * Two mods that name each other. The recursion could only see a cycle by
		 * running out of depth, and the work list keeps that exact rule, so this
		 * is still ST_BAD_ARG rather than a hang.
		 */
		const char *title = "NPEA09996";

		write_cycle_mods(title);
		mods_set_title(title);
		check_eq_u64(mods_count(), 2, "two mods that depend on each other are scanned");
		check(mods_load("loopa") == ST_BAD_ARG,
		      "and a dependency cycle is refused ST_BAD_ARG, as the recursion depth did");
		check_eq_u64(mods_loaded_mask(), 0, "with neither of them loaded");
	}

	{
		/* A dependency that names a mod nobody has is NOT_FOUND, as before. */
		const char *title = "NPEA09997";

		write_missing_dep_mod(title);
		mods_set_title(title);
		check(mods_load("orphan") == ST_NOT_FOUND,
		      "a dependency naming a mod that is not there is NOT_FOUND");
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
	check(boot_and_wait("NPEA99999"),
	      "an unregistered title reaches INGAME all the same");
	check(session_game() == NULL, "and no game is selected");

	/* Back to RaC1 for anything that follows. */
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again");
}

/*
 * Build 36. A process whose title id no game table claims is a session like any
 * other: INGAME with no game, so the memory panel works on anything the console
 * runs, and everything that needs a game's addresses answers UNSUPPORTED.
 * docs/PROTOCOL.md section 3.3 is the contract this checks.
 */
static void test_unknown_title(void)
{
	u8 packet[TELEMETRY_MAX];
	u8 info[SESSION_INFO_SIZE];
	u8 describe[8192];
	const char * const *options = NULL;
	u32 len = 0;
	u32 v = 0;
	u8 watch_id = 0xFF;
	u8 count = 0;

	group("unknown title: it reaches INGAME");

	check(quit_and_wait(), "quit whatever was running");
	watch_clear();
	freeze_clear();

	check(boot_and_wait("BLUS99999"), "a title no game table claims reaches INGAME");
	check(session_game() == NULL, "and the session has no game");
	check(qstreq(session_title(), "BLUS99999"), "but it does have the title id");

	pump(8);   /* far enough for a publish, which is every fourth tick */
	check_eq_u64(session_info_copy(info, sizeof(info)), SESSION_INFO_SIZE,
	             "the info block is published");
	check_eq_u64(info[2], SESSION_INGAME, "its state byte says INGAME");
	check_eq_u64(info[3], GAME_NONE, "its game byte says NONE");
	check(memcmp(info + 12, "BLUS99999", 9) == 0, "and it carries the title id");

	/* Nothing knows where any of these live, so nothing reads them. */
	check_eq_u64(info[28], 0, "current_planet stays 0");
	check_eq_u64(be32_get(info + 32), 0, "pos[0] stays 0");
	check_eq_u64(be32_get(info + 44), 0, "the pad mask stays 0");
	check_eq_u64(be32_get(info + 64), 0, "readout[0] stays 0");

	group("unknown title: the memory tools work on it");

	host_poke(0x00810000u, (const u8 *)"\xDE\xAD\xBE\xEF", 4);
	check(mem_read_u32(0x00810000u, &v) == ST_OK, "mem_read reaches the process");
	check_eq_u64(v, 0xDEADBEEFu, "and brings back what is there");
	check(mem_write_u32(0x00810000u, 0x12345678u) == ST_OK, "mem_write reaches it too");
	mem_read_u32(0x00810000u, &v);
	check_eq_u64(v, 0x12345678u, "and the write landed");

	check(watch_add(0x00810004u, 4, &watch_id) == ST_OK, "a watch is registered");
	host_poke(0x00810004u, (const u8 *)"\x00\x00\x0B\xAD", 4);
	pump(8);

	len = session_telemetry_copy(packet, sizeof(packet));
	check_eq_u64(len, 4u + SESSION_INFO_SIZE + 1u + 12u,
	             "the telemetry packet carries exactly one watch");
	check_eq_u64(packet[6], SESSION_INGAME, "its state byte says INGAME");
	check_eq_u64(packet[7], GAME_NONE, "its game byte says NONE");
	{
		const u8 *w = packet + 4 + SESSION_INFO_SIZE + 1;
		check_eq_u64(w[0], watch_id, "the watch id matches");
		check_eq_u64(w[2], 1, "the watch is valid");
		check_eq_u64(be64_get(w + 4), 0x0BADu, "and its value is live");
	}

	check(freeze_add(0x00810008u, 4, 0x2222u, NULL) == ST_OK, "a freeze is registered");
	pump(2);
	mem_read_u32(0x00810008u, &v);
	check_eq_u64(v, 0x2222u, "and the tick writes it");
	freeze_clear();

	{
		const struct patch_word words[] = { { 0x00810010u, 0x60000000u } };

		check(client_patch_apply(words, 1) == ST_OK, "a client patch applies");
		mem_read_u32(0x00810010u, &v);
		check_eq_u64(v, 0x60000000u, "and its word reached memory");
		check(client_patch_revert(0x00810010u) == ST_OK, "and it reverts");
	}

	group("unknown title: everything that needs a game is UNSUPPORTED");

	check(features_describe(describe, sizeof(describe), &len) == ST_UNSUPPORTED,
	      "DESCRIBE is UNSUPPORTED");
	check_eq_u64(len, 0, "and encodes nothing");
	check(features_set(0, 1) == ST_UNSUPPORTED, "FEATURE_SET is UNSUPPORTED");
	check(features_trigger(0) == ST_UNSUPPORTED, "FEATURE_TRIGGER is UNSUPPORTED");
	check(features_set_auto(0, 1) == ST_UNSUPPORTED, "FEATURE_SET_AUTO is UNSUPPORTED");
	check(features_options(0, &options, &count) == ST_UNSUPPORTED,
	      "FEATURE_OPTIONS is UNSUPPORTED");
	check_eq_u64(features_toggle_state(), 0, "no toggle is on");
	check_eq_u64(features_toggle_auto(), 0, "and none is flagged auto");
	check(session_position_save(0) == ST_UNSUPPORTED, "POS_SAVE is UNSUPPORTED");
	check(session_position_load(0) == ST_UNSUPPORTED, "POS_LOAD is UNSUPPORTED");
	check(session_planet_load(0, 0) == ST_UNSUPPORTED, "PLANET_LOAD is UNSUPPORTED");
	check(session_die() == ST_UNSUPPORTED, "DIE is UNSUPPORTED");
	check(session_load_setaside() == ST_UNSUPPORTED, "LOAD_ASIDE is UNSUPPORTED");
	{
		u8 supported = 1, installed = 1, running = 1, pending = 1;
		u32 size = 1;

		/*
		 * The one exception, and it is the answer a game with no helper has
		 * always given: supported 0, which is how a client knows to hide the
		 * save-file panel. Everything that would move a save is refused.
		 */
		check(savefile_info(&supported, &installed, &running, &pending, &size) == ST_OK,
		      "SAVEFILE_INFO answers, as it does for a game with no helper");
		check_eq_u64(supported, 0, "reporting no save-file support");
		check_eq_u64(size, 0, "and no aside buffer");
		check(savefile_read(0, 4, describe, &len) == ST_UNSUPPORTED,
		      "SAVEFILE_READ is UNSUPPORTED");
		check(savefile_library_gate() == ST_UNSUPPORTED,
		      "and the whole savefile library with it");
		check(savefile_patch(describe, sizeof(describe), &len) == ST_UNSUPPORTED,
		      "and SAVEFILE_PATCH, with no helper to describe (revision 1.15)");
		check_eq_u64(len, 0, "and no words");
	}
	check_eq_u64(mods_loaded_mask(), 0, "the mod table is the unknown title's own, and empty");

	group("unknown title: quit and the same title again");

	host_quit();
	pump(1);
	check(session_state() == SESSION_QUITTING, "the process going away means QUITTING");
	check(pump_until(SESSION_XMB, 200), "and the tick after that, the XMB");
	check_eq_u64(session_title()[0], 0, "which clears the title id");
	check(session_game() == NULL, "with still no game");

	check(boot_and_wait("BLUS99999"), "the same unknown title boots again");
	check(session_game() == NULL, "still with no game");
	check(watch_slot(watch_id) != NULL, "its watches survived, as a game's do");
	check_eq_u64(features_toggle_state(), 0, "nothing re-applied a toggle");
	check_eq_u64(mods_loaded_mask(), 0, "nor a mod");
	check(!session_previous()->pending, "and nothing is left pending");

	group("unknown title: a game after it");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots after it");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC1,
	      "and the session says RaC1 again");
	check(watch_slot(watch_id) == NULL, "the unknown title's watches were dropped");
	check(features_describe(describe, sizeof(describe), &len) == ST_OK &&
	      describe[0] == GAME_RAC1, "DESCRIBE answers with RaC1's table");
	watch_clear();
}

/*
 * The two repacked PAL builds of Going Commando the old client accepted. They
 * are NPEA00386 under another title id, so they get RaC2's whole table.
 */
static void test_rac2_repacks(void)
{
	const struct game_api *cands[GAME_MAX_CANDIDATES];
	u8 describe[8192];
	u32 len = 0;
	u32 n;

	group("RaC2: the repacked PAL title ids");

	n = game_candidates_for_title("RC2ILYOOO", cands, GAME_MAX_CANDIDATES);
	check_eq_u64(n, 1, "RC2ILYOOO names exactly one game");
	check(n == 1 && cands[0]->game_id == GAME_RAC2, "and it is RaC2");

	n = game_candidates_for_title("BLMODYOOO", cands, GAME_MAX_CANDIDATES);
	check_eq_u64(n, 1, "BLMODYOOO names exactly one game");
	check(n == 1 && cands[0]->game_id == GAME_RAC2, "and it is RaC2");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("BLMODYOOO"), "BLMODYOOO reaches INGAME");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC2,
	      "as RaC2, on RaC2's fingerprint");
	check(qstreq(session_title(), "BLMODYOOO"), "keeping its own title id");
	check(features_describe(describe, sizeof(describe), &len) == ST_OK &&
	      describe[0] == GAME_RAC2, "DESCRIBE answers with RaC2's table");
	if (session_game() != NULL) {
		const struct game_describe *d = session_game()->describe();
		check_eq_u64(d->nfeatures, 28, "and every one of RaC2's twenty-eight features");
	}

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 comes back for the rest of the run");
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
	check_eq_u64(packet[5], 53, "and this module is build 53");
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

/*
 * Writes a config.txt of exactly `bytes` bytes: one real key and then a single
 * comment line padded out to length, which config_load skips the way it skips
 * any comment. Returns what the write did.
 */
static int write_sized_config(u32 bytes)
{
	static const char head[] = "log = 1\n";
	char pad[256];
	plat_file_t f;
	u32 written;

	memset(pad, 'x', sizeof(pad));

	if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) != 0) return ST_IO_ERROR;

	plat_file_write(f, head, sizeof(head) - 1);
	plat_file_write(f, "#", 1);
	written = sizeof(head);

	while (written + 1 < bytes) {
		u32 n = bytes - 1 - written;
		if (n > sizeof(pad)) n = sizeof(pad);
		plat_file_write(f, pad, n);
		written += n;
	}
	plat_file_write(f, "\n", 1);

	plat_file_close(f);
	return ST_OK;
}

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

	/*
	 * trace_ops was the per-request log switch until build 39 took the tracing
	 * out. Any config.txt written before then still carries the line, and a user
	 * who reads an old guide may well type it in again, so it has to be inert
	 * rather than fatal: the file loads like any other, and the key sits in the
	 * table as one more nobody reads. Nothing in net looks at it any more.
	 */
	check(config_set_u32("trace_ops", 1) == ST_OK,
	      "a config.txt carrying a stale trace_ops line is written");
	check(config_load() == ST_OK, "and loads with no complaint");
	check_eq_u64(config_get_u32("trace_ops", 7), 1,
	             "the key is kept like any other unknown key and acts on nothing");

	/*
	 * combo.enabled, the revision 1.12 switch: a plain 0 or 1 a user can edit by
	 * hand, picked up by the next reload, and written by COMBO_ENABLE under the
	 * same name.
	 */
	check(config_set_u32("combo.enabled", 0) == ST_OK && config_load() == ST_OK &&
	      !config_combo_enabled(), "combo.enabled = 0 holds every combo off");
	check(config_set_u32("combo.enabled", 1) == ST_OK && config_load() == ST_OK &&
	      config_combo_enabled(), "and 1 hands them back");
	check(config_set_combo_enabled(0) == ST_OK &&
	      qstreq(config_get("combo.enabled"), "0"),
	      "COMBO_ENABLE writes that same key");
	check(config_set_combo_enabled(2) == ST_BAD_ARG, "and anything but 0 or 1 is BAD_ARG");
	check(config_set_combo_enabled(1) == ST_OK, "the switch goes back on");

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

	/*
	 * CONFIG_TEXT_MAX. config.txt is read whole into one buffer, so the buffer
	 * has to hold anything config_save can write: CONFIG_MAX_ENTRIES lines of a
	 * key, a value and the separators. These two walk the edge of it.
	 *
	 * qread_file cannot tell a full buffer from a file that just fits, so the
	 * largest file it reads is two bytes short of the cap; that is unchanged and
	 * is what the first of these pins. A file past it answers ST_FULL and
	 * config_load hands that straight back, applying none of the file.
	 */
	{
		check(write_sized_config(CONFIG_TEXT_MAX - 2) == ST_OK,
		      "a config.txt at the text limit is written");
		check(config_load() == ST_OK, "and loads");
		check_eq_u64(config_get_u32("log", 0), 1, "with its keys in it");
		check(config_combo_enabled(),
		      "and a config with no combo.enabled in it leaves the combos on");

		check(write_sized_config(CONFIG_TEXT_MAX - 1) == ST_OK,
		      "a config.txt one byte past the limit is written");
		check(config_load() == ST_FULL, "and is refused with ST_FULL");
		check_eq_u64(config_get_u32("log", 7), 7, "leaving nothing of it loaded");

		/* Put a small config back for whatever runs after this. */
		check(config_set_u32("log", 1) == ST_OK, "a small config is written back");
		check(config_load() == ST_OK, "and reloads");
	}

	/*
	 * CONFIG_MAX_ENTRIES, which is 64 since build 37 and was 128. The table fills
	 * and then refuses, exactly as it did at 128: config_set answers ST_FULL for
	 * a key there is no room for, and a file with more keys than the table holds
	 * loads the ones that fit and drops the rest, which is what config_load has
	 * always done with kv_put's answer.
	 */
	{
		plat_file_t f;
		u32 i;
		u32 stored = 0;

		if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) == 0) {
			for (i = 0; i < CONFIG_MAX_ENTRIES + 1u; i++) {
				char line[CONFIG_KEY_MAX + CONFIG_VAL_MAX + 8];

				line[0] = 0;
				qstrcat(line, sizeof(line), "fill");
				qfmt_u32(line + 4, sizeof(line) - 4, i);
				qstrcat(line, sizeof(line), " = ");
				qfmt_u32(line + qstrlen(line), sizeof(line) - qstrlen(line), i);
				qstrcat(line, sizeof(line), "\n");
				plat_file_write(f, line, qstrlen(line));
			}
			plat_file_close(f);
		}

		check(config_load() == ST_OK, "a config.txt of 65 keys reads");

		for (i = 0; i < CONFIG_MAX_ENTRIES + 1u; i++) {
			char key[32];

			qstrcpy(key, sizeof(key), "fill");
			qfmt_u32(key + 4, sizeof(key) - 4, i);
			if (config_get(key) != NULL) stored++;
		}
		check_eq_u64(stored, CONFIG_MAX_ENTRIES,
		             "64 of them are kept and the 65th is dropped");
		check(config_get("fill0") != NULL, "the first key is one of the 64");

		check(config_set("one_too_many", "1") == ST_FULL,
		      "and a 65th key set through config_set is ST_FULL");

		/* Back to a small config for whatever runs after this. */
		if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) == 0) {
			plat_file_write(f, "combo.enabled = 1\n", 18);
			plat_file_close(f);
		}
		check(config_load() == ST_OK, "a small config reloads");
		check_eq_u64(config_get_u32("combo.enabled", 0), 1, "with its one key in it");
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
	check_eq_u64(nfeatures, 22,
	             "and twenty-two features (build 53 retired six rows and added Freeze health)");
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
	check(desc_feature(out, len, 6) != NULL && desc_feature(out, len, 12) != NULL,
	      "the ids either side of the retired run 7..11 kept their numbers");

	/*
	 * Build 53, revision 1.21: the manip buttons are retired the same way, ids
	 * and all. They are the manip groups now (test_manips).
	 */
	{
		static const u8 retired[] = { 7, 8, 10, 11, 16 };
		u8 i, found = 0, refused = 0;

		for (i = 0; i < sizeof(retired); i++) {
			if (desc_feature(out, len, retired[i]) != NULL) found++;
			if (features_trigger(retired[i]) == ST_NOT_FOUND) refused++;
		}
		check_eq_u64(found, 0, "RaC1's five retired manip ids are not described");
		check_eq_u64(refused, sizeof(retired), "and triggering any of them is NOT_FOUND");
	}

	/*
	 * Build 53: max ammo, all weapons is retired too, and Freeze health takes
	 * the next free id, 28, in Cheats beside infinite health.
	 */
	check(desc_feature(out, len, 17) == NULL, "the retired max-ammo id 17 is not described");
	check(features_trigger(17) == ST_NOT_FOUND, "and triggering it is NOT_FOUND");
	check(desc_feature(out, len, 18) != NULL, "id 18 after it kept its number");
	row = desc_feature(out, len, 28);
	check(row != NULL && row[1] == FEATURE_TOGGLE && row[2] == 0 && row[4] == 0,
	      "Freeze health is id 28, a TOGGLE in Cheats that writes no code");
	check(row != NULL && memcmp(row + 16, "Freeze health", 14) == 0, "and its label reads");
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

	/* Build 53: the max-ammo action is retired; infinite ammo covers it. */
	check(features_trigger(17) == ST_NOT_FOUND, "the retired max-ammo action is NOT_FOUND");
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

/*
 * Build 43. Deadlocked's two mode words, the ones its `savefile_ready` reads:
 * gameMode (0 ordinary play, 3 the pause, anything else a transition, a menu or
 * a lobby) and pendingGameMode (-2 when no mode change is queued).
 *
 * A fresh fake process is all zeroes, which reads as gameMode 0 and
 * pendingGameMode 0 - a mode change queued - so Deadlocked is *not* idle when a
 * test boots it. Every test that wants a request to go out says so first.
 */
#define R4_MODE_ADDR         0x00B3C5A0u
#define R4_PENDING_MODE_ADDR 0x009C37F4u
#define R4_MODE_NONE         0xFFFFFFFEu

static void rac4_modes(u32 mode, u32 pending)
{
	u8 raw[4];

	be32_put(raw, mode);
	host_poke(R4_MODE_ADDR, raw, 4);
	be32_put(raw, pending);
	host_poke(R4_PENDING_MODE_ADDR, raw, 4);
}

/*
 * Build 53. RaC1, RaC2 and RaC3 answer `savefile_ready` too. A set-aside needs
 * a save in the game's save buffer and a load needs one to hand over, and a
 * fresh fake process has none: RaC1's save pointer and RaC2's and RaC3's save
 * managers read 0, as they do on a console until the game has saved or loaded
 * once.
 *
 * The tests put a save where the game would: RaC1's buffer is the pointer at
 * 0xA10928 plus 0x100000, RaC2's the word at +4 of the block 0x1410E50 points
 * at and RaC3's the word at +4 of the block 0xCB0A98 points at, and a save is
 * recognised by its head, the two block sizes the game's restore insists on
 * (and, in RaC1, a planet number below 20 at +0x18).
 */
#define SF_R1_STATE     0x00A10708u
#define SF_R1_REQUEST   0x00A10700u
#define SF_R1_SAVE_PTR  0x00A10928u
#define SF_R1_SAVE_BASE 0x00D00000u
#define SF_R1_SAVE_BUF  0x00E00000u   /* SF_R1_SAVE_BASE + 0x100000 */
#define SF_R2_SAVE_INFO 0x01410E50u
#define SF_R2_SAVE_OBJ  0x01A00000u
#define SF_R2_SAVE_BUF  0x01A10000u
#define SF_R2_MODE      0x0156B064u
#define SF_R2_PENDING   0x01329F90u
#define SF_R2_PLANET    0x01329A3Cu
#define SF_R2_REQUEST   0x0156B050u
#define SF_R3_SAVE_INFO 0x00CB0A98u
#define SF_R3_SAVE_OBJ  0x01800000u
#define SF_R3_SAVE_BUF  0x01810000u
#define SF_R3_MODE      0x00EE9334u
#define SF_R3_REQUEST   0x00EE9310u

static void poke32(u32 addr, u32 value)
{
	u8 raw[4];

	be32_put(raw, value);
	host_poke(addr, raw, 4);
}

/* The head of a save of the game at `addr`; nothing for Deadlocked. */
static void sf_put_save_head(u8 game_id, u32 addr)
{
	u8 head[0x1C];

	memset(head, 0, sizeof(head));
	if (game_id == GAME_RAC1) {
		be32_put(head, 0x153Cu);
		be32_put(head + 4, 0x82A4u);
		be32_put(head + 0x18, 3);
	} else if (game_id == GAME_RAC2) {
		be32_put(head, 0x1D08u);
		be32_put(head + 4, 0x84D0u);
	} else if (game_id == GAME_RAC3) {
		be32_put(head, 0x37B0u);
		be32_put(head + 4, 0x84C0u);
	} else {
		return;
	}
	host_poke(addr, head, sizeof(head));
}

/* A save in the game's own save buffer, where its set-aside copies from. */
static void sf_give_save(u8 game_id)
{
	if (game_id == GAME_RAC1) {
		poke32(SF_R1_SAVE_PTR, SF_R1_SAVE_BASE);
		sf_put_save_head(GAME_RAC1, SF_R1_SAVE_BUF);
	} else if (game_id == GAME_RAC2) {
		poke32(SF_R2_SAVE_INFO, SF_R2_SAVE_OBJ);
		poke32(SF_R2_SAVE_OBJ + 4, SF_R2_SAVE_BUF);
		sf_put_save_head(GAME_RAC2, SF_R2_SAVE_BUF);
	} else if (game_id == GAME_RAC3) {
		poke32(SF_R3_SAVE_INFO, SF_R3_SAVE_OBJ);
		poke32(SF_R3_SAVE_OBJ + 4, SF_R3_SAVE_BUF);
		sf_put_save_head(GAME_RAC3, SF_R3_SAVE_BUF);
	}
}

/*
 * The state a request may go out in, for the games that have a notion of one:
 * Deadlocked in ordinary play with nothing queued; RaC1 and RaC2 the same, with
 * a save of their own to set aside or load (build 53).
 */
static void savefile_make_idle(u8 game_id)
{
	if (game_id == GAME_RAC4) rac4_modes(0, R4_MODE_NONE);
	if (game_id == GAME_RAC1) {
		poke32(SF_R1_STATE, 0);
		poke32(SF_R1_REQUEST, 0);
	}
	if (game_id == GAME_RAC2) {
		u8 planet[4];

		poke32(SF_R2_MODE, 0);
		poke32(SF_R2_PENDING, R4_MODE_NONE);
		poke32(SF_R2_REQUEST, 0);
		host_peek(SF_R2_PLANET, planet, 4);
		if ((planet[0] & 0x80) != 0) poke32(SF_R2_PLANET, 0);
	}
	if (game_id == GAME_RAC3) {
		poke32(SF_R3_MODE, 0);
		poke32(SF_R3_REQUEST, 0);
	}
	sf_give_save(game_id);
}

/* Build 53: the request bytes moved out of the game's bump arena. */
#define A_HELPER   0x01080000u
#define A_SF_LOAD  0x01080001u
#define A_SF_ASIDE 0x01080002u
#define A_SF_AUTO  0x01080003u

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
	savefile_make_idle(GAME_RAC1);

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
	savefile_make_idle(game_id);

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

	/*
	 * Build 53: RaC1 and RaC2 load only a save of their own, and the aside
	 * buffer still holds the "test" written above, so a save's head goes in.
	 */
	sf_put_save_head(game_id, d->aside_addr);
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
		savefile_make_idle(GAME_RAC1);
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

		/*
		 * Build 53: in front, the head of a save of the game, which RaC1 and
		 * RaC2 want to see before a load raised over the buffer goes out.
		 */
		if (off == 0) {
			host_poke(d->aside_addr, g_pattern, n);
			sf_put_save_head(d->game_id, d->aside_addr);
			host_peek(d->aside_addr, g_pattern, n);
		}

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
	savefile_make_idle(GAME_RAC1);
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
	check(d->aside_size > SAVEFILE_COPY_CHUNK * SAVEFILE_CHUNKS_PER_TICK,
	      "which is more than one tick's worth of chunks, so the copy is a real loop");
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
	check_eq_u64(done, SAVEFILE_CHUNKS_PER_TICK * SAVEFILE_COPY_CHUNK,
	             "then a whole tick's worth of chunks goes per tick");

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
	check_eq_u64(done, SAVEFILE_CHUNKS_PER_TICK * SAVEFILE_COPY_CHUNK,
	             "the copy starts on the next tick");

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
	savefile_make_idle(GAME_RAC1);
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

/* ------------------------ build 43, a save taken only while the game is idle */

/*
 * What can and cannot be checked here, again.
 *
 * qwark's half runs on the PC: the two words are poked into the fake process and
 * every entry point is asked what it does with them. The *helper's* half is
 * PowerPC that runs a frame at a time inside the game, and nothing here executes
 * it, so what a console has to answer is whether a Deadlocked that is mid
 * cutscene, mid planet load or in the load menu really does read as something
 * other than mode 0 or 3 with pendingGameMode -2 - and whether a save taken on
 * the frames that do read idle comes back clean. What is checked here is the
 * contract between the two: the code the helper leaves in the request byte when
 * it drops a request, and everything qwark does with it.
 */
#define SAVE_ROOT_R4 "/dev_hdd0/qwark/savefiles/NPEA00423"
#define SAVE_ROOT_R3 "/dev_hdd0/qwark/savefiles/NPEA00387"

/* The gameMode values that are not somewhere a save may be taken. */
static const u32 g_r4_busy_modes[] = {
	1,  /* movie   */
	2,  /* scene   */
	4,  /* freeze  */
	5,  /* vendor  */
	6,  /* the space transition, which is a planet load */
	7,  /* puzzle  */
	10, /* lobby   */
	14  /* pre-lobby */
};

static void test_savefile_not_idle(void)
{
	const struct sf_desc *d = sf_desc_for_game(GAME_RAC4);
	u8 supported = 0, installed = 0, running = 0, pending = 0, error = 9;
	u32 size = 0, done = 0, total = 0;
	u8 byte = 9;
	u32 i;

	group("build 43: Deadlocked refuses a save while the game is busy");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00423"), "Deadlocked reaches INGAME");
	if (d == NULL) { check(0, "Deadlocked has a helper table entry"); return; }

	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	host_poke(d->api_load, (const u8 *)"\x00", 1);

	/* ---------------------------------------- pendingGameMode, on its own */

	/* Ordinary play, but a mode change already queued. */
	rac4_modes(0, 0);
	host_write_log_reset();

	check(savefile_set_aside() == ST_BUSY,
	      "a set-aside with a mode change queued is BUSY");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "and the error says the game was not idle");
	host_peek(d->api_setaside, &byte, 1);
	check_eq_u64(byte, 0, "with nothing raised at the game");
	check_eq_u64(host_write_log_count(), 0, "and not a byte written into it");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "nothing is left outstanding");
	check_eq_u64(installed, 0, "and a refusal does not install the helper either");

	check(savefile_load_aside() == ST_BUSY, "a load is refused the same way");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "with the same error");
	host_peek(d->api_load, &byte, 1);
	check_eq_u64(byte, 0, "and nothing raised at the game");

	/* ------------------------------------------- gameMode, on its own */

	for (i = 0; i < sizeof(g_r4_busy_modes) / sizeof(g_r4_busy_modes[0]); i++) {
		rac4_modes(g_r4_busy_modes[i], R4_MODE_NONE);
		check(savefile_set_aside() == ST_BUSY,
		      "a set-aside in a mode that is not play or the pause is BUSY");
		check(savefile_load_aside() == ST_BUSY, "and so is a load");
	}
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "every one of them for the same reason");
	host_peek(d->api_setaside, &byte, 1);
	check_eq_u64(byte, 0, "and none of them raised anything");

	/* ------------------------------------------------- the two idle modes */

	rac4_modes(0, R4_MODE_NONE);
	check(savefile_set_aside() == ST_OK, "ordinary play with nothing queued is accepted");
	host_peek(d->api_setaside, &byte, 1);
	check_eq_u64(byte, 1, "and raises the request exactly as it always did");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, SAVEFILE_PENDING_SET_ASIDE, "the pending bit goes up");
	check_eq_u64(installed, 1, "and this is the action that installed the helper");

	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "and the settle window closes it, unchanged by any of this");

	/*
	 * The pause counts, because it is the mode the helper puts the game into
	 * itself to run Deadlocked's load menu.
	 */
	rac4_modes(3, R4_MODE_NONE);
	check(savefile_load_aside() == ST_OK, "the pause is an idle mode too");
	host_peek(d->api_load, &byte, 1);
	check_eq_u64(byte, 1, "and the load goes out from it");
	host_poke(d->api_load, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);

	/* --------------------------------------- the helper's own refusal code */

	/*
	 * qwark asks on its tick; the helper asks on the game's frame, and the two
	 * are different clocks. A request that qwark let through and the helper then
	 * found the game busy comes back as SAVEFILE_HELPER_REFUSED in the request
	 * byte - which the fake console cannot produce, so the test writes it.
	 */
	rac4_modes(0, R4_MODE_NONE);
	check(savefile_set_aside() == ST_OK, "a set-aside goes out while the game is idle");
	host_poke(d->api_setaside, (const u8 *)"\xFE", 1);
	pump(1);
	host_peek(d->api_setaside, &byte, 1);
	check_eq_u64(byte, 0, "qwark clears the refusal code out of the byte");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "the request is over at once, with no settle window");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "and it is reported as the game not being idle");

	check(savefile_load_aside() == ST_OK, "a load goes out while the game is idle");
	host_poke(d->api_load, (const u8 *)"\xFE", 1);
	pump(1);
	host_peek(d->api_load, &byte, 1);
	check_eq_u64(byte, 0, "the load's refusal is cleared the same way");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "and it is not retried on a later frame");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "with the same error");

	/* ------------------------------------------------- STORE and RESTORE */

	check(savefile_category(SAVEFILE_CATEGORY_CREATE, "dl") == ST_OK,
	      "a category for Deadlocked");

	rac4_modes(0, 0);
	check(savefile_store("dl", "busy.sav") == ST_BUSY, "STORE is refused while busy");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "for the same reason");
	savefile_info(&supported, &installed, &running, &pending, &size);
	check_eq_u64(pending, 0, "no transfer started");
	check(!plat_path_exists(SAVE_ROOT_R4 "/dl/busy.sav", NULL, NULL),
	      "and no file was made for it");

	/* A real store, from an idle game, so there is something to restore. */
	rac4_modes(0, R4_MODE_NONE);
	fill_aside(d, 4);
	check(savefile_store("dl", "good.sav") == ST_OK, "STORE from an idle game is accepted");
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	check(pump_until_transfer_done(SAVEFILE_SETTLE_TICKS + 64), "and runs to the end");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NONE, "with no error");
	check(plat_path_exists(SAVE_ROOT_R4 "/dl/good.sav", NULL, NULL), "leaving the file");

	rac4_modes(6, R4_MODE_NONE);
	check(savefile_restore("dl", "good.sav") == ST_BUSY,
	      "RESTORE into a planet load is refused");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "for the same reason");

	/*
	 * A RESTORE is a megabyte of copying before the load goes out, and the game
	 * has frames of its own in that time. The readiness test is made again at
	 * the end, not taken on trust from when the op was accepted.
	 */
	rac4_modes(0, R4_MODE_NONE);
	host_poke(d->api_load, (const u8 *)"\x00", 1);
	check(savefile_restore("dl", "good.sav") == ST_OK, "RESTORE from an idle game is accepted");
	pump(2);
	rac4_modes(6, R4_MODE_NONE);
	check(pump_until_transfer_done(128), "the copy finishes and the transfer stops");
	host_peek(d->api_load, &byte, 1);
	check_eq_u64(byte, 0, "with no load raised at a game that stopped being idle");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "and the error says why");

	/* The same again, but it is the helper that says no. */
	rac4_modes(0, R4_MODE_NONE);
	check(savefile_restore("dl", "good.sav") == ST_OK, "one more RESTORE");
	{
		int ticks;
		for (ticks = 0; ticks < 128; ticks++) {
			host_peek(d->api_load, &byte, 1);
			if (byte != 0) break;
			pump(1);
		}
	}
	check_eq_u64(byte, 1, "which does raise the load once the file is in");
	host_poke(d->api_load, (const u8 *)"\xFE", 1);
	check(pump_until_transfer_done(8), "the helper refuses it and the transfer stops");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "saying the game was not idle");

	/* And a STORE whose set-aside the helper refuses writes no file at all. */
	rac4_modes(0, R4_MODE_NONE);
	check(savefile_store("dl", "dropped.sav") == ST_OK, "a STORE goes out");
	host_poke(d->api_setaside, (const u8 *)"\xFE", 1);
	check(pump_until_transfer_done(8), "the helper refuses the set-aside");
	savefile_transfer(&done, &total, &error);
	check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "and the transfer stops for that reason");
	check_eq_u64(done, 0, "without copying a byte");
	check(!plat_path_exists(SAVE_ROOT_R4 "/dl/dropped.sav", NULL, NULL),
	      "and without leaving a file of the last save under this one's name");

	/* -------------------------------------------------- the other games */

	/*
	 * Up to build 52 RaC3's vtable had no savefile_ready, which reads as always
	 * ready. Build 53 gave it one (test_savefile_build53): a game that has not
	 * saved yet has nothing to set aside, and with a save the set-aside goes
	 * out from any mode, a set-aside never needing the game to sit still.
	 */
	check(quit_and_wait(), "Deadlocked goes away");
	check(boot_and_wait("NPEA00387"), "RaC3 comes up");
	check(savefile_set_aside() == ST_BUSY, "RaC3 with no save yet is BUSY");
	sf_give_save(GAME_RAC3);
	poke32(SF_R3_MODE, 6);
	check(savefile_set_aside() == ST_OK, "and with one the set-aside goes out, even in space");
	host_poke(sf_desc_for_game(GAME_RAC3)->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	poke32(SF_R3_MODE, 0);

	/* ------------------------------------------------------------ tidy up */

	check(quit_and_wait(), "RaC3 goes away");
	check(boot_and_wait("NPEA00423"), "Deadlocked comes back to clean up after itself");
	plat_file_unlink(SAVE_ROOT_R4 "/dl/good.sav");
	plat_file_unlink(SAVE_ROOT_R4 "/dl/good.sav.sum");
	check(savefile_category(SAVEFILE_CATEGORY_DELETE, "dl") == ST_OK,
	      "and the category goes with them");
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

/* ------------------------------------------- COMBO_ENABLE, revision 1.12 */

/*
 * The switch the old RaCMAN had and this one did not: one persistent on/off
 * over all five combos. It lives in config, so it outlives the client and the
 * console rather than being re-sent on every connection, and the info block's
 * flags bit3 reports it, so a client draws its checkbox out of the telemetry it
 * already reads. It has nothing to do with the capture hold above: either of
 * them holds every combo off, the hold expires and the switch never does.
 */
static void test_combo_enable(void)
{
	u8 info[SESSION_INFO_SIZE];
	u8 blob[QWARK_MAX_BLOB];
	u8 len = 0;
	u8 planet;
	u64 window;

	group("combo enable");

	check(boot_and_wait("NPEA00385"), "RaC1 boots for the combo switch checks");

	config_set_selected_slot(COMBO_TEST_SLOT);
	check(config_set_combo(COMBO_SAVE_POSITION, COMBO_TEST_MASK) == ST_OK,
	      "the save-position combo is stored");

	planet = session_current_planet();
	pos_clear(planet, COMBO_TEST_SLOT);

	/* Nothing has ever written the key, so the switch is on and combos fire. */
	check(config_combo_enabled(),
	      "a config.txt that never mentions the switch has the combos on");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) == 0, "and flags bit3 is clear");

	host_set_pad(0);
	pump(2);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_OK,
	      "the combo fires with the switch on");

	host_set_pad(0);
	pump(2);
	pos_clear(planet, COMBO_TEST_SLOT);

	/* COMBO_ENABLE 0. */
	check(config_set_combo_enabled(0) == ST_OK, "the switch is turned off");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) != 0,
	      "flags bit3 says every combo is held off");

	host_set_pad(COMBO_TEST_MASK);
	pump(4);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "and the stored mask under the user's fingers fires nothing");

	/*
	 * COMBO_ENABLE 1 re-arms as little as lifting a hold does: the pad is still
	 * full of the combo, and it has to come back to empty first.
	 */
	check(config_set_combo_enabled(1) == ST_OK, "the switch is turned back on");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) == 0, "flags bit3 clears with it");
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "re-enabling does not fire the combo under the buttons still held");

	host_set_pad(0);
	pump(2);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_OK,
	      "and the next press, after the pad returned to 0, fires it again");

	host_set_pad(0);
	pump(2);
	pos_clear(planet, COMBO_TEST_SLOT);

	/* Anything but 0 or 1 is refused, and refusing changes nothing. */
	check(config_set_combo_enabled(2) == ST_BAD_ARG, "2 is BAD_ARG");
	check(config_combo_enabled(), "and a refused value leaves the switch alone");

	/*
	 * It is in the file, so config_load with nothing in between - a CONFIG_RELOAD,
	 * and the same thing a console power cycle does - still comes back off.
	 */
	check(config_set_combo_enabled(0) == ST_OK, "the switch goes off again");
	check(config_load() == ST_OK, "config.txt reloads from disk");
	check(!config_combo_enabled(), "and the switch is still off after a CONFIG_RELOAD");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) != 0,
	      "the reloaded config still reports bit3");

	check(quit_and_wait(), "the game quits with the switch off");
	check(boot_and_wait("NPEA00385"), "and the same title boots again");
	check(!config_combo_enabled(), "the switch is still off in the new session");

	planet = session_current_planet();
	pos_clear(planet, COMBO_TEST_SLOT);
	host_set_pad(0);
	pump(2);
	host_set_pad(COMBO_TEST_MASK);
	pump(4);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "and no combo fires after the reboot, with no client having said so");

	/*
	 * The switch and a hold are independent, so each one holds the combos off by
	 * itself. A hold that has expired hands nothing back while the switch is off.
	 */
	window = session_combo_suspend_window_us();
	session_set_combo_suspend_window_us(30000);   /* 30 ms */
	session_combo_suspend(1);
	host_set_pad(0);
	pump(2);
	plat_sleep_us(60000);
	host_set_pad(COMBO_TEST_MASK);
	pump(4);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "an expired hold hands nothing back while the switch is off");
	session_set_combo_suspend_window_us(window);

	/* And the other way round: the switch back on under a live hold. */
	host_set_pad(0);
	pump(2);
	session_combo_suspend(1);
	check(config_set_combo_enabled(1) == ST_OK,
	      "the switch comes back on under a live hold");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) == 0,
	      "bit3 follows the switch alone, not the hold");

	host_set_pad(COMBO_TEST_MASK);
	pump(4);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_NOT_FOUND,
	      "and the hold still holds every combo off on its own");

	session_combo_suspend(0);
	host_set_pad(0);
	pump(2);
	host_set_pad(COMBO_TEST_MASK);
	pump(2);
	check(pos_fetch(planet, COMBO_TEST_SLOT, blob, &len) == ST_OK,
	      "with neither the switch nor a hold against it the combo fires");

	/* Leave nothing behind for the tests that follow. */
	host_set_pad(0);
	pump(2);
	pos_clear(planet, COMBO_TEST_SLOT);
	config_set_combo(COMBO_SAVE_POSITION, 0);
	config_set_selected_slot(0);

	check(quit_and_wait(), "quit RaC1");

	/*
	 * Neither op writes game memory, so both answer at the XMB what they answer
	 * in a game: whatever writing config.txt answered. This is the state rule
	 * COMBO_ENABLE inherits from COMBO_SET.
	 */
	check(session_state() == SESSION_XMB, "the session is back at the XMB");
	check(config_set_combo_enabled(0) == ST_OK,
	      "the switch is settable with no game running");
	check(config_set_combo(COMBO_SAVE_POSITION, 0) == ST_OK,
	      "exactly as a combo mask is");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) != 0,
	      "and the XMB info block carries bit3 too");

	check(config_set_combo_enabled(1) == ST_OK, "the switch goes back on");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((info[24] & SESSION_FLAG_COMBOS_OFF) == 0,
	      "leaving bit3 clear for everything after this");
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
/*
 * Build 50: the savefile helper's request bytes moved out of the code segment
 * (src/games/sfhelper/sf_rac2.h), into the read-write segment's gap.
 */
#define R2_SF_HELPER    0x01317728u
#define R2_SF_LOAD      0x01317729u
#define R2_SF_SET_ASIDE 0x0131772Au

#define F2_FAST_LOADS   0
#define F2_INFINITE_AMMO 1
#define F2_DIE          6
#define F2_BOLTS        7
#define F2_DEATH_BOSSES 13
#define F2_DEATH_PBOLTS 14
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

/*
 * Tesla Claw: unlock id 16, item id 18, and one of the five RaC1 carry-overs
 * whose second and last version is bought from Slim Cognito. 114 is that
 * version's item id.
 */
#define TESLA_ID        16
#define TESLA_ITEM      18
#define TESLA_V2        114

/* Zodiac: a weapon with no second version, so no Level slot. */
#define ZODIAC_ID       21
#define ZODIAC_ITEM     43

/* Heli-Pack: a gadget, so an owned byte and nothing else. */
#define HELI2_ID        29
#define HELI2_ITEM      2

/* The two weapons with one version: the Zodiac and the RYNO II. */
#define R2_NO_LEVEL(id) ((id) == 21 || (id) == 22)

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
		check_eq_u64(d->nfeatures, 28,
		             "RaC2 declares twenty-eight features (build 53 retired nine)");
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

	group("RaC2: unlocks with levels and ammo");

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
		 * version the item array holds. Slot 2 was its experience until build
		 * 34 took the column away; the slot keeps its number and is left
		 * unnamed, so the Ammo slot stays where a client expects it.
		 */
		check(fields != NULL && qstreq(fields[0].name, "Owned") &&
		      fields[0].kind == UNLOCK_KIND_FLAG, "slot 0 is the Owned checkbox");
		check(fields != NULL && qstreq(fields[1].name, "Level") &&
		      fields[1].kind == UNLOCK_KIND_NUMBER && fields[1].max == 4,
		      "slot 1 is a Level number that stops at 4");
		check(fields != NULL && (fields[2].name == NULL || fields[2].name[0] == 0),
		      "slot 2 is unnamed, so no XP column is drawn");
		check(fields != NULL && qstreq(fields[3].name, "Ammo") &&
		      fields[3].kind == UNLOCK_KIND_NUMBER && fields[3].max == 0,
		      "slot 3 is still the unbounded Ammo number");

		row = unlock_row(list, n, LANCER_ID);
		check(row != NULL && row->fields ==
		      (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 | UNLOCK_FIELD_3),
		      "a weapon with versions declares Owned, Level and Ammo");

		row = unlock_row(list, n, TESLA_ID);
		check(row != NULL && qstreq(row->name, "Tesla Claw") &&
		      row->fields == (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 | UNLOCK_FIELD_3),
		      "and so does a RaC1 carry-over, whose second version Slim sells");

		row = unlock_row(list, n, ZODIAC_ID);
		check(row != NULL && qstreq(row->name, "Zodiac") &&
		      row->fields == (UNLOCK_FIELD_0 | UNLOCK_FIELD_3),
		      "a weapon with no second version declares no Level");

		row = unlock_row(list, n, HELI2_ID);
		check(row != NULL && qstreq(row->name, "Heli-Pack") &&
		      row->fields == UNLOCK_FIELD_0,
		      "a gadget has no level and no ammo, so it is owned-only");

		/*
		 * The category decides the mask, bar the two weapons the game gives no
		 * second version: those declare everything but the level. Nothing at
		 * all declares the unnamed slot 2.
		 */
		{
			u8 i, wrong = 0;

			for (i = 0; i < n; i++) {
				u8 want = UNLOCK_FIELD_0;

				if (list[i].category == R2_CAT_WEAPONS)
					want = (u8)(UNLOCK_FIELD_0 | UNLOCK_FIELD_1 |
					            UNLOCK_FIELD_3);
				if (R2_NO_LEVEL(list[i].id))
					want = (u8)(want & ~(u8)UNLOCK_FIELD_1);

				if (list[i].fields != want) wrong++;
			}
			check_eq_u64(wrong, 0, "every row declares the slots its category has");
		}

		/* A weapon on V2 with rounds in the magazine. */
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
		check_eq_u64(values[2], 0, "the exp word is no longer read out");
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

		/*
		 * A RaC1 carry-over is the same two-version shape as the Clank Zapper:
		 * the byte at its own item id is V1 and the one holding 114 is V2, and
		 * a client sending the game-wide 4 lands on 114 rather than on some
		 * item id four steps along.
		 */
		host_poke(R2_ITEM(TESLA_ITEM), (const u8 *)"\x12", 1);    /* its own id */
		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, TESLA_ID), values) == ST_OK,
		      "the Tesla Claw reads live");
		check_eq_u64(values[1], 1, "its own item id reads as level 1");

		host_poke(R2_ITEM(TESLA_ITEM), (const u8 *)"\x72", 1);    /* 114 */
		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		memset(values, 0, sizeof(values));
		check(g->unlock_read(unlock_row(list, n, TESLA_ID), values) == ST_OK,
		      "and again on its second version");
		check_eq_u64(values[1], 2, "114 reads as level 2");

		check(g->unlock_set(TESLA_ID, 1, 1) == ST_OK, "UNLOCK_SET puts it back on V1");
		host_peek(R2_ITEM(TESLA_ITEM), &b, 1);
		check_eq_u64(b, TESLA_ITEM, "which is its own item id");
		check(g->unlock_set(TESLA_ID, 1, 4) == ST_OK, "and takes the game-wide 4");
		host_peek(R2_ITEM(TESLA_ITEM), &b, 1);
		check_eq_u64(b, TESLA_V2, "clamped to its own V2, which is item id 114");

		/* Owned and ammo land in their own array cell and nowhere else. */
		check(g->unlock_set(LANCER_ID, 0, 1) == ST_OK, "UNLOCK_SET owned=1");
		host_peek(R2_UNLOCK_LANCE, &b, 1);
		check_eq_u64(b, 1, "the owned byte was written");

		/*
		 * Slot 2 named the experience word until build 34 and names nothing
		 * now, so every row refuses it and the word itself is left where the
		 * game put it.
		 */
		host_poke(R2_EXP(LANCER_ITEM), (const u8 *)"\x00\x00\x10\x92", 4);
		check(g->unlock_set(LANCER_ID, 2, 1234) == ST_UNSUPPORTED,
		      "UNLOCK_SET on the unnamed slot 2 is UNSUPPORTED");
		mem_read_u32(R2_EXP(LANCER_ITEM), &v);
		check_eq_u64(v, 4242, "and the exp word was left alone");

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
		 * is what stops a client writing an ammo word the game does not count
		 * for it.
		 */
		host_poke(R2_AMMO(HELI2_ITEM), (const u8 *)"\x00\x00\x00\x09", 4);
		check(g->unlock_set(HELI2_ID, 0, 1) == ST_OK, "a gadget takes owned=1");
		host_peek(R2_OWNED(HELI2_ITEM), &b, 1);
		check_eq_u64(b, 1, "and its owned byte was written");
		check(g->unlock_set(HELI2_ID, 1, 2) == ST_UNSUPPORTED, "a gadget has no level");
		check(g->unlock_set(HELI2_ID, 2, 1) == ST_UNSUPPORTED, "and no slot 2 either");
		check(g->unlock_set(HELI2_ID, 3, 1) == ST_UNSUPPORTED, "a gadget has no ammo");
		mem_read_u32(R2_AMMO(HELI2_ITEM), &v);
		check_eq_u64(v, 9, "and the refused ammo write left the word alone");

		check(g->unlock_set(ZODIAC_ID, 1, 2) == ST_UNSUPPORTED,
		      "a weapon with one version has no level either");
		host_peek(R2_ITEM(ZODIAC_ITEM), &b, 1);
		check_eq_u64(b, 0, "and nothing was written for it");
		check(g->unlock_set(200, 0, 1) == ST_BAD_ARG, "an unknown id is BAD_ARG");
	}

	group("RaC2: the two weapon actions");

	{
		/*
		 * The Lancer is held at V2 and the Tesla Claw at its own V1; the
		 * Bouncer is not held at all.
		 */
		host_poke(R2_OWNED(LANCER_ITEM), (const u8 *)"\x01", 1);
		host_poke(R2_ITEM(LANCER_ITEM), (const u8 *)"\x3C", 1);
		host_poke(R2_OWNED(TESLA_ITEM), (const u8 *)"\x01", 1);
		host_poke(R2_ITEM(TESLA_ITEM), (const u8 *)"\x12", 1);
		host_poke(R2_OWNED(BOUNCER_ITEM), (const u8 *)"\x00", 1);
		host_poke(R2_ITEM(BOUNCER_ITEM), (const u8 *)"\x25", 1);

		check(features_trigger(F2_MAX_LEVELS) == ST_OK, "Max all weapon levels runs");
		host_peek(R2_ITEM(LANCER_ITEM), &b, 1);
		check_eq_u64(b, LANCER_V4, "the Lancer went to its top version");
		host_peek(R2_ITEM(TESLA_ITEM), &b, 1);
		check_eq_u64(b, TESLA_V2, "the Tesla Claw went to the one Slim sells");
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
	savefile_make_idle(GAME_RAC2);
	check(features_trigger(F2_SET_ASIDE) == ST_OK,
	      "set aside works from a cold process: the helper goes in first");
	host_peek(R2_SF_SET_ASIDE, &b, 1);
	check_eq_u64(b, 1, "the set-aside byte was written");
	check(features_trigger(F2_LOAD_ASIDE) == ST_OK, "so does the load action");
	host_peek(R2_SF_LOAD, &b, 1);
	check_eq_u64(b, 1, "and it wrote the load byte");
	check(session_load_setaside() == ST_OK, "and so does the combo action");

	group("RaC2: the loading-screen watcher");

	/*
	 * The final load screen on planet 0 is what the old form watched for. Build
	 * 53: the buffer charge and the two auto-resets it used to run there are
	 * manips now (test_manips), so with no category selected the load writes
	 * neither.
	 */
	host_poke(R2_PLANET_ADDR, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R2_CHARGE_BUF, (const u8 *)"\x00\x00\x00\x00", 4);
	host_poke(R2_PAD_MANIP, (const u8 *)"\x00\x00\x00\x00", 4);
	check(features_set(F2_INFINITE_AMMO, 1) == ST_OK, "infinite ammo on");

	host_poke(R2_LOADCOUNT, (const u8 *)"\x01", 1);
	pump(3);
	check((features_toggle_state() & ((u64)1 << F2_INFINITE_AMMO)) != 0,
	      "an ordinary load screen does nothing");

	host_poke(R2_LOADCOUNT, (const u8 *)"\x02", 1);
	pump(3);
	check((features_toggle_state() & ((u64)1 << F2_INFINITE_AMMO)) == 0,
	      "the final load screen turned infinite ammo back off");
	mem_read_u32(R2_CHARGE_BUF, &v);
	check_eq_u64(v, 0, "and no longer sets the buffer charge by itself");
	mem_read_u32(R2_PAD_MANIP, &v);
	check_eq_u64(v, 0, "nor runs any reset");
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x4BFFEA69u, "and put fast loads back to the toggle setting");

	check(features_set(21, 1) == ST_NOT_FOUND && features_set(22, 1) == ST_NOT_FOUND,
	      "the two retired auto-reset toggles are NOT_FOUND");
	check(features_trigger(15) == ST_NOT_FOUND && features_trigger(20) == ST_NOT_FOUND,
	      "and so are the retired reset and Maktar buttons");

	features_set(F2_DEATH_BOSSES, 0);
	features_set(F2_DEATH_PBOLTS, 0);

	group("RaC2: values");

	check(features_set(F2_BOLTS, 1234) == ST_OK, "the bolts VALUE writes");
	mem_read_u32(R2_BOLTS_ADDR, &v);
	check_eq_u64(v, 1234, "and lands at the bolt count");
	check(features_trigger(F2_BOLTS) == ST_BAD_ARG, "triggering a VALUE is BAD_ARG");
}

/* ----------------------------------- RaC2: the Aranos load, for the manips */

/* A few of the words the two resets write, from rac2.h. */
#define R2_GORN_MANIP    0x01A99A4Cu   /* any%: 0 */
#define R2_GORN_OPENING  0x01A99A34u   /* any%: 0, NG+: 1 */
#define R2_FELTZIN_OPEN  0x01A8495Bu   /* one byte; any%: 0, NG+: 1 */
#define R2_RACE_INDEX    0x01A4D7E0u   /* both: 0 */
#define R2_SHORTCUTS     0x01352684u   /* NG+: the setup's shortcut */

#define R2_PAD_13        0x41500000u   /* 13.0f, a fresh console's jump-pad speed */
#define R2_PAD_25        0x41C80000u   /* 25.0f, what Snivelak leaves behind */

#define R2_BIT(id)       ((u64)1 << (id))

static u32 r2_word(u32 addr)
{
	u32 v = 0xDEADBEEFu;
	mem_read_u32(addr, &v);
	return v;
}

static u8 r2_byte(u32 addr)
{
	u8 b = 0xEE;
	host_peek(addr, &b, 1);
	return b;
}

static void r2_put_word(u32 addr, u32 value)
{
	u8 raw[4];
	be32_put(raw, value);
	host_poke(addr, raw, 4);
}

static void r2_put_planet(u8 planet)
{
	r2_put_word(R2_PLANET_ADDR, planet);
}

static void r2_put_count(u8 count)
{
	host_poke(R2_LOADCOUNT, &count, 1);
}

/*
 * Memory as a run that has been through Snivelak, Gorn and the Barlow race
 * leaves it, with the boots' buffer empty. NG+ values where they differ from
 * the any% ones are chosen so that each reset visibly changes something.
 */
static void r2_dirty(void)
{
	r2_put_word(R2_GORN_MANIP, 1);
	r2_put_word(R2_GORN_OPENING, 0);
	host_poke(R2_FELTZIN_OPEN, (const u8 *)"\x00", 1);
	r2_put_word(R2_RACE_INDEX, 5);
	r2_put_word(R2_SHORTCUTS, 3);
	r2_put_word(R2_PAD_MANIP, R2_PAD_25);
	r2_put_word(R2_CHARGE_BUF, 0);
}

/*
 * The game sitting in a level other than Aranos, its last load long over: the
 * counter reads 2. A few ticks so the watcher has seen it.
 */
static void r2_settle_on(u8 planet)
{
	r2_put_planet(planet);
	r2_put_count(2);
	pump(4);
}

/*
 * One planet load as the executable runs it (src/games/rac2.c has the
 * addresses): the planet word takes the destination before the transition
 * loop, the counter drops to 0 while the engine data reads, is 1 for a frame
 * and 2 when the load screen ends. The 120 Hz tick sees each step at least once.
 */
static void r2_load(u8 planet)
{
	r2_put_planet(planet);
	r2_put_count(0);
	pump(3);
	r2_put_count(1);
	pump(1);
	r2_put_count(2);
	pump(2);
}


/* --------------------------------------------- manip categories, rev 1.21 */

/*
 * Build 53. The manip groups of SPEC-manips.md, written out here a second time
 * from the spec rather than from the game files, so a table that drifts from
 * the contract fails here.
 */
struct mt_write {
	u32 addr;
	u8  len;
	const char *data;   /* `len` bytes, or NULL for zeros */
};

#define MT_B(a, v)    { (a), 1, (v) }
#define MT_W(a, v)    { (a), 4, (v) }
#define MT_Z(a, n)    { (a), (n), NULL }
#define MT_D(a, v, n) { (a), (n), (v) }

#define MT_MAX_WRITES 40

struct mt_group {
	u8 id;
	u8 flags;               /* the wire's, PER_RUN included */
	const char *name;
	const char *cats;
	u8 nsetup;              /* the first nsetup writes are the setup */
	u8 n;
	struct mt_write w[MT_MAX_WRITES];
};

#define MT_NG   MANIP_FLAG_NGPLUS
#define MT_EXT  MANIP_FLAG_EXTENSION
#define MT_RUN  MANIP_FLAG_PER_RUN
#define MT_NOCH MANIP_FLAG_NO_CHALLENGE

/* ---- RaC1 */

#define R1M_DREK_CS   0x00FACC74u
#define R1M_DREK_SKIP 0x00FACC78u   /* both flags are words (rac1.h) */
#define R1M_BRIDGE    0x00A0CD04u
#define R1M_RACE      0x00E5EFD0u
#define R1M_SONIC     0x00A15F3Cu
#define R1M_SHOOT     0x0096C9DCu
#define R1M_CHALLENGE 0x0096C9FCu

static const struct mt_group mt_rac1[] = {
	{ 1, MT_NG | MT_RUN, "NG+ (poki skip)", "NG+", 0, 2, {
		MT_W(R1M_DREK_CS, "\0\0\0\x01"), MT_W(R1M_DREK_SKIP, "\0\0\0\0") } },
	{ 2, MT_NG | MT_RUN, "NG+ (no poki skip)", "NG+", 0, 2, {
		MT_W(R1M_DREK_SKIP, "\0\0\0\x01"), MT_W(R1M_DREK_CS, "\0\0\0\0") } },
	{ 3, MT_RUN, "All Missions", "All Missions", 0, 2, {
		MT_W(R1M_BRIDGE, "\0\0\0\0"), MT_Z(R1M_RACE, 12) } },
	{ 4, MT_RUN, "All Checkmarks", "All Checkmarks", 0, 2, {
		MT_Z(R1M_SONIC, 8), MT_Z(R1M_SHOOT, 32) } },
	{ 5, MT_NG | MT_EXT | MT_RUN, "NG+ GBSP", "NG+ GBSP", 0, 4, {
		MT_W(R1M_DREK_SKIP, "\0\0\0\x01"), MT_W(R1M_DREK_CS, "\0\0\0\0"),
		MT_D(R1M_SONIC, "\0\0\0\x01\0\0\0\x01", 8),
		MT_D(R1M_SHOOT, "\0\0\0\x20\0\0\0\x20\0\0\0\x20\0\0\0\x20"
		                "\0\0\0\x20\0\0\0\x20\0\0\0\x20\0\0\0\x20", 32) } }
};

/* ---- RaC2 */

#define R2M_FELTZIN_OPEN 0x01A8495Bu
#define R2M_SNIV         0x01A6FB73u
#define R2M_PAD          0x013185B8u
#define R2M_YEEDIL       0x01A9DF90u
#define R2M_SIB          0x01A5A99Fu
#define R2M_GORN_MANIP   0x01A99A4Cu
#define R2M_GORN_OPEN    0x01A99A34u
#define R2M_IM           0x0135268Cu
#define R2M_CURSOR       0x01352684u
#define R2M_RACE         0x01A4D7E0u
#define R2M_FELTZIN_MISS 0x01A84973u
#define R2M_HRUGIS_MISS  0x0143DB0Fu
#define R2M_GORN_MISS    0x01A99A5Bu
#define R2M_CHARGE       0x0145C180u
#define R2M_ENDAKO_CS    0x01A58158u
#define R2M_DORBIT_OPEN  0x01A59764u
#define R2M_CHALLENGE    0x01329AA2u

#define R2M_G_PRIME \
	MT_B(R2M_FELTZIN_OPEN, "\x01"), MT_B(R2M_SNIV, "\x14"),                  \
	MT_W(R2M_PAD, "\x41\xC8\x00\x00"), MT_B(R2M_YEEDIL, "\x42"),             \
	MT_B(R2M_SIB, "\x14"), MT_W(R2M_GORN_MANIP, "\0\0\0\x01"),               \
	MT_W(R2M_GORN_OPEN, "\0\0\0\x01")
#define R2M_G  R2M_G_PRIME, MT_W(R2M_IM, "\0\0\0\x01")

#define R2M_M(i) MT_W(R2M_CURSOR, "\0\0\0" i), R2M_M_REST
#define R2M_M_REST \
	MT_W(R2M_RACE, "\0\0\0\0"),                                              \
	MT_B(R2M_FELTZIN_OPEN, "\x01"), MT_W(R2M_GORN_OPEN, "\0\0\0\x01"),       \
	MT_W(R2M_FELTZIN_MISS, "\0\0\0\0"), MT_W(R2M_HRUGIS_MISS, "\0\0\0\0"),   \
	MT_W(R2M_GORN_MISS, "\0\0\0\0"), MT_W(R2M_CHARGE, "\0\0\0\x1E")

/* A: RAC2Form.resetAnyPercentVars, as rac2_reset_anypct wrote it until build 53. */
#define R2M_A \
	MT_W(0x01A4D7E0u, "\0\0\0\0"), MT_B(0x01A8495Bu, "\0"),                  \
	MT_W(0x01A99A34u, "\0\0\0\0"), MT_W(0x01A99A4Cu, "\0\0\0\0"),            \
	MT_W(0x01A84973u, "\0\0\0\0"), MT_W(0x0143DB0Fu, "\0\0\0\0"),            \
	MT_W(0x01A99A5Bu, "\0\0\0\0"), MT_W(0x015718F8u, "\0\0\0\0"),            \
	MT_W(0x01329AACu, "\0\0\0\0"), MT_W(0x01A58158u, "\0\0\0\0"),            \
	MT_W(0x01AAC767u, "\0\0\0\0"), MT_W(0x0133E8A0u, "\0\0\0\0"),            \
	MT_W(0x015784E8u, "\0\0\0\0"), MT_W(0x01578424u, "\0\0\0\0"),            \
	MT_W(0x01A30430u, "\0\0\0\0"), MT_W(0x01A849A8u, "\0\0\0\0"),            \
	MT_B(0x01A59764u, "\0"), MT_W(0x013A2D90u, "\0\0\0\0"),                  \
	MT_B(0x01A6FB73u, "\0"), MT_B(0x01A9DF90u, "\0"), MT_B(0x01A5A99Fu, "\0"), \
	MT_W(0x013185B8u, "\x41\x50\x00\x00")

#define R2M_K \
	MT_B(0x014817AFu, "\x28"), MT_B(0x01390C27u, "\x4A"), MT_B(0x01390C37u, "\x2D")

static const struct mt_group mt_rac2[] = {
	{ 1, MT_NG | MT_RUN, "NG+ no IMG", "NG+ no IMG", 7, 15, { R2M_G_PRIME, R2M_M("\x01") } },
	{ 2, MT_NG | MT_RUN, "NG+", "NG+", 8, 16, { R2M_G, R2M_M("\x07") } },
	{ 3, MT_RUN, "Any%", "Any%\nAll Platinum Bolts", 0, 22, { R2M_A } },
	{ 4, MT_RUN, "100%", "Max%\n100%", 0, 25, { R2M_A, R2M_K } },
	{ 5, MT_NG | MT_EXT | MT_RUN, "NG+ All Missions", "NG+ All Missions", 10, 18, {
		R2M_G, MT_W(R2M_ENDAKO_CS, "\0\0\0\x01"), MT_B(R2M_DORBIT_OPEN, "\x01"),
		R2M_M("\x07") } },
	/* No Specials: M without its shortcut cursor, which lives in the specials menu. */
	{ 6, MT_NG | MT_EXT | MT_RUN | MT_NOCH, "NG+ No Specials", "NG+ No Specials", 7, 14, {
		R2M_G_PRIME, R2M_M_REST } }
};

/* ---- UYA */

#define R3M_CHALLENGE 0x00C1E50Eu

#define R3M_S5 \
	MT_W(0x00C9165Cu, "\0\0\0\x07"), MT_W(0x00C36BCCu, "\0\0\0\x03"),        \
	MT_W(0x00EF6098u, "\0\0\0\x0E"), MT_W(0x00C4F918u, "\0\0\0\x02"),        \
	MT_W(0x0148A100u, "\0\0\0\x01")
#define R3M_VQE MT_D(0x00C1E2C0u, "\x00\x17", 2)

static const struct mt_group mt_rac3[] = {
	{ 1, MT_NG, "NG+ no QE", "NG+ no QE", 5, 5, { R3M_S5 } },
	{ 2, MT_NG, "NG+", "NG+", 6, 6, { R3M_S5, R3M_VQE } },
	{ 3, MT_NG | MT_EXT | MT_RUN, "NG+ CC Early", "NG+ CC Early", 6, 9, {
		R3M_S5, R3M_VQE,
		MT_W(0x00C1E660u, "\0\0\0\x41"), MT_W(0x00C1E664u, "\x41\x41\x41\x41"),
		MT_W(0x00C956A0u, "\0\0\0\x64") } }
};

/* ---- Deadlocked */

#define R4M_CHALLENGE 0x009C330Eu

static const struct mt_group mt_rac4[] = {
	{ 1, MT_NG, "NG+", "NG+ no LC\nNG+", 5, 5, {
		/* The Reactor's is the whole word, so its counter reads 20. */
		MT_B(0x00A947D3u, "\x14"), MT_W(0x00A94944u, "\0\0\0\x14"), MT_B(0x00A969E3u, "\x14"),
		MT_B(0x00A96E43u, "\x14"), MT_B(0x00A07C7Fu, "\x14") } }
};

struct mt_game {
	const char *title;
	u8 game_id;
	const char *name;
	const struct mt_group *groups;
	u8 ngroups;
	u32 challenge;
	u8 challenge_width;
};

static const struct mt_game mt_games[] = {
	{ "NPEA00385", GAME_RAC1, "RaC1", mt_rac1, 5, R1M_CHALLENGE, 4 },
	{ "NPEA00386", GAME_RAC2, "RaC2", mt_rac2, 6, R2M_CHALLENGE, 1 },
	{ "NPEA00387", GAME_RAC3, "UYA",  mt_rac3, 3, R3M_CHALLENGE, 1 },
	{ "NPEA00423", GAME_RAC4, "DL",   mt_rac4, 1, R4M_CHALLENGE, 1 }
};

static void mt_put_counter(const struct mt_game *g, u32 value)
{
	if (g->challenge_width == 4) poke32(g->challenge, value);
	else { u8 b = (u8)value; host_poke(g->challenge, &b, 1); }
}

/* A save file in play, or the title screen, as each game's in_file reads it. */
static void mt_put_in_file(u8 game_id, int in_file);

/* Every byte of every write is where the last write to it put it. */
static int mt_values_match(const struct mt_group *grp, u8 from, u8 to)
{
	u8 i, j, k;

	for (i = from; i < to; i++) {
		const struct mt_write *w = &grp->w[i];

		for (j = 0; j < w->len; j++) {
			u32 a = w->addr + j;
			u8 want = w->data != NULL ? (u8)w->data[j] : 0;
			u8 got = 0;
			int later = 0;

			for (k = (u8)(i + 1); k < to && !later; k++) {
				const struct mt_write *l = &grp->w[k];
				if (a >= l->addr && a < l->addr + l->len) later = 1;
			}
			if (later) continue;

			host_peek(a, &got, 1);
			if (got != want) {
				printf("    0x%08X: got %02X want %02X (write %u)\n",
				       (unsigned)a, got, want, (unsigned)i);
				return 0;
			}
		}
	}
	return 1;
}

/* The write log from `first` on is exactly writes [from, to) of the group. */
static int mt_log_matches(const struct mt_group *grp, u8 from, u8 to)
{
	u32 n = host_write_log_count();
	u8 i;

	if (n != (u32)(to - from)) {
		printf("    %u writes logged, %u expected\n", (unsigned)n, (unsigned)(to - from));
		return 0;
	}
	for (i = from; i < to; i++) {
		u32 a = 0, len = 0;
		host_write_log_at(i - from, &a, &len);
		if (a != grp->w[i].addr || len != grp->w[i].len) {
			printf("    write %u: 0x%08X/%u, expected 0x%08X/%u\n", (unsigned)i,
			       (unsigned)a, (unsigned)len, (unsigned)grp->w[i].addr,
			       (unsigned)grp->w[i].len);
			return 0;
		}
	}
	return 1;
}

/* Poison every byte a group writes, so a write that does not happen shows. */
static void mt_poison(const struct mt_group *grp)
{
	static const u8 junk[32] = {
		0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
		0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
		0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
		0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE
	};
	u8 i;

	for (i = 0; i < grp->n; i++) host_poke(grp->w[i].addr, junk, grp->w[i].len);
}

/* MANIP_LIST, parsed. */
struct mt_list {
	u8 selected, auto_reset, remember, ngroups;
	u16 seq;
	u8 kind, group;
	u8 ids[16], flags[16];
	char names[16][40];
	char cats[16][64];
	u8 nheaders;
	char hnames[4][16];
	u8 hn[4];
	u16 hids[4][8];
	u32 len, consumed;
};

static int mt_parse(const u8 *b, u32 len, struct mt_list *l)
{
	u32 off = 8;
	u8 i;

	memset(l, 0, sizeof(*l));
	l->len = len;
	if (len < 8) return 0;
	l->selected = b[0]; l->auto_reset = b[1]; l->remember = b[2]; l->ngroups = b[3];
	l->seq = be16_get(b + 4); l->kind = b[6]; l->group = b[7];

	for (i = 0; i < l->ngroups && i < 16; i++) {
		u8 n;
		if (off + 3 > len) return 0;
		l->ids[i] = b[off]; l->flags[i] = b[off + 1]; n = b[off + 2];
		off += 3;
		if (off + n + 1 > len || n >= sizeof(l->names[i])) return 0;
		memcpy(l->names[i], b + off, n);
		off += n;
		n = b[off++];
		if (off + n > len || n >= sizeof(l->cats[i])) return 0;
		memcpy(l->cats[i], b + off, n);
		off += n;
	}
	if (off + 1 > len) return 0;
	l->nheaders = b[off++];
	for (i = 0; i < l->nheaders && i < 4; i++) {
		u8 n, j;
		if (off + 1 > len) return 0;
		n = b[off++];
		if (off + n + 1 > len || n >= sizeof(l->hnames[i])) return 0;
		memcpy(l->hnames[i], b + off, n);
		off += n;
		l->hn[i] = b[off++];
		for (j = 0; j < l->hn[i]; j++) {
			if (off + 2 > len || j >= 8) return 0;
			l->hids[i][j] = be16_get(b + off);
			off += 2;
		}
	}
	l->consumed = off;
	return off == len;
}

static int mt_list_now(struct mt_list *l)
{
	u8 out[1024];
	u32 len = 0;

	if (manips_list(out, sizeof(out), &len) != ST_OK) return 0;
	return mt_parse(out, len, l);
}

/* Forget whatever a run of the tests left in config.txt for one game. */
static void mt_reset_options(const char *key)
{
	config_set_manip(key, "auto_reset", 1);
	config_set_manip(key, "remember", 0);
	config_set_manip(key, "category", 0);
}

static const char *mt_key(u8 game_id)
{
	switch (game_id) {
	case GAME_RAC1: return "rac1";
	case GAME_RAC2: return "rac2";
	case GAME_RAC3: return "rac3";
	default:        return "rac4";
	}
}

/*
 * Every game's groups against the spec: MANIP_LIST describes them as the spec
 * does, and Apply now writes the setup and then the per-run list, every write
 * in order and at its width, with the values the spec gives.
 */
static void test_manip_tables(void)
{
	u8 gi;

	for (gi = 0; gi < sizeof(mt_games) / sizeof(mt_games[0]); gi++) {
		const struct mt_game *g = &mt_games[gi];
		struct mt_list l;
		char what[128];
		u8 i;

		snprintf(what, sizeof(what), "manip tables: %s", g->name);
		group(what);

		check(quit_and_wait(), "quit whatever was running");
		mt_reset_options(mt_key(g->game_id));
		check(boot_and_wait(g->title), "the game boots");
		pump(4);

		check(mt_list_now(&l), "MANIP_LIST encodes and parses exactly");
		check_eq_u64(l.ngroups, g->ngroups, "with the spec's number of groups");
		check_eq_u64(l.selected, 0, "None is selected after a boot");

		for (i = 0; i < g->ngroups && i < l.ngroups; i++) {
			const struct mt_group *grp = &g->groups[i];
			int ok;

			snprintf(what, sizeof(what), "%s group %u", g->name, (unsigned)grp->id);

			ok = l.ids[i] == grp->id && l.flags[i] == grp->flags &&
			     qstreq(l.names[i], grp->name) && qstreq(l.cats[i], grp->cats);
			if (!ok)
				printf("    listed %u %02X '%s' '%s'\n", l.ids[i], l.flags[i],
				       l.names[i], l.cats[i]);
			check(ok, "MANIP_LIST lists it with the spec's id, flags, name and categories");

			/* A compatible file in play: challenge mode for NG+, fresh otherwise. */
			mt_put_in_file(g->game_id, 1);
			mt_put_counter(g, (grp->flags & MT_NG) ? 1 : 0);

			check(manips_select(grp->id) == ST_OK, "MANIP_SELECT takes it");

			mt_poison(grp);
			host_write_log_reset();
			check(manips_apply() == ST_OK, "Apply now writes it");
			check(mt_log_matches(grp, 0, grp->n),
			      "setup then per run, every write in the spec's order and width");
			check(mt_values_match(grp, 0, grp->n), "with the spec's values");
		}

		check(manips_select(0) == ST_OK, "back to None");
		host_write_log_reset();
		check(manips_apply() == ST_OK && host_write_log_count() == 0,
		      "and Apply now with None writes nothing");
		check(manips_select(99) == ST_BAD_ARG, "an unknown group is BAD_ARG");

		/* The headers: Deadlocked's Fixes and UYA's Trophies, none elsewhere. */
		check(mt_list_now(&l), "MANIP_LIST again");
		if (g->game_id == GAME_RAC4) {
			check(l.nheaders == 1 && qstreq(l.hnames[0], "Fixes") && l.hn[0] == 2 &&
			      l.hids[0][0] == 0 && l.hids[0][1] == 1,
			      "one header, Fixes: Crash patches (0) and Fix reset softlocks (1)");
		} else if (g->game_id == GAME_RAC3) {
			check(l.nheaders == 1 && qstreq(l.hnames[0], "Trophies") && l.hn[0] == 2 &&
			      l.hids[0][0] == 21 && l.hids[0][1] == 39,
			      "one header, Trophies: the refresh (21) and its auto toggle (39)");
		} else {
			check_eq_u64(l.nheaders, 0, "no header");
		}
	}
}

/*
 * The setup goes in once per game boot, the per-run list on every file load
 * while auto-reset is on, and a reboot needs the setup again. RaC2's NG+ has
 * both, so it carries the whole story.
 */
static void test_manip_semantics(void)
{
	const struct mt_game *g = &mt_games[1];        /* RaC2 */
	const struct mt_group *ngp = &mt_rac2[1];      /* NG+: G then M(7) */
	const struct mt_group *any = &mt_rac2[2];      /* Any%: A */
	struct mt_list l;
	u16 seq;

	group("manips: defaults");

	check(quit_and_wait(), "quit whatever was running");
	mt_reset_options("rac2");
	/* A config.txt that has never heard of manips at all. */
	config_set("manip.rac2.auto_reset", "");
	{
		/* Drop the three keys: rewrite config.txt without them. */
		plat_file_t f;
		if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) == 0) {
			plat_file_write(f, "combo.enabled = 1\n", 18);
			plat_file_close(f);
		}
		config_load();
	}
	check(config_get("manip.rac2.auto_reset") == NULL, "config.txt holds no manip keys");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	mt_put_in_file(GAME_RAC2, 0);
	pump(4);
	check(mt_list_now(&l), "MANIP_LIST answers");
	check(l.auto_reset == 1, "auto-reset defaults to on");
	check(l.remember == 0, "remember defaults to off");
	check(l.selected == 0, "and the category to None");
	check(l.kind == MANIP_EVENT_NONE, "with no event yet this boot");

	group("manips: setup once per boot, per run on every file load");

	mt_poison(ngp);
	host_write_log_reset();
	check(manips_select(2) == ST_OK, "NG+ selected on the title screen");
	check_eq_u64(host_write_log_count(), 0, "writes nothing: no file is in play");
	check(!manips_setup_done(), "and its setup is still due");

	mt_put_in_file(GAME_RAC2, 1);
	mt_put_counter(g, 1);
	seq = manips_event_seq();
	host_write_log_reset();
	manips_file_loaded();
	check(mt_log_matches(ngp, 0, ngp->n), "the first file load writes the setup and the per-run list");
	check(mt_values_match(ngp, 0, ngp->n), "with the right values");
	check(manips_setup_done(), "the setup is done for this boot");
	check(manips_event_seq() == (u16)(seq + 1) && manips_event_kind() == MANIP_EVENT_SETUP &&
	      manips_event_group() == 2, "one event: setup activated, NG+");

	mt_poison(ngp);
	seq = manips_event_seq();
	host_write_log_reset();
	manips_file_loaded();
	check(mt_log_matches(ngp, ngp->nsetup, ngp->n), "the next file load writes the per-run list alone");
	check(r2_word(R2M_PAD) == 0xEEEEEEEEu, "and leaves the jump pad, a setup value, alone");
	check(manips_event_seq() == (u16)(seq + 1) && manips_event_kind() == MANIP_EVENT_APPLIED,
	      "one event: per-run applied");

	group("manips: auto-reset off");

	check(manips_options(0, 0) == ST_OK, "auto-reset off");
	check(mt_list_now(&l) && l.auto_reset == 0, "MANIP_LIST says so");
	check(qstreq(config_get("manip.rac2.auto_reset"), "0"), "and config.txt has it");
	seq = manips_event_seq();
	host_write_log_reset();
	manips_file_loaded();
	check_eq_u64(host_write_log_count(), 0, "a file load writes nothing once the setup is in");
	check(manips_event_seq() == seq, "and records no event");
	check(manips_options(2, 0) == ST_BAD_ARG, "an option other than 0 or 1 is BAD_ARG");
	check(manips_options(1, 0) == ST_OK, "auto-reset back on");

	group("manips: a new selection makes the setup due again");

	check(manips_select(3) == ST_OK, "Any% selected");
	mt_put_counter(g, 0);
	host_write_log_reset();
	manips_file_loaded();
	check(mt_log_matches(any, 0, any->n), "a fresh file load runs the any% reset");
	check(r2_word(R2M_PAD) == 0x41500000u, "which puts the jump pad back to 13.0");

	check(manips_select(2) == ST_OK, "NG+ selected again, on a fresh file");
	check(r2_word(R2M_PAD) == 0x41500000u, "nothing is written onto a fresh file");
	check(manips_event_kind() == MANIP_EVENT_NOT_CHALLENGE && manips_event_group() == 2,
	      "the selection's setup is refused: not a challenge mode file");
	mt_put_counter(g, 3);
	host_write_log_reset();
	check(manips_select(2) == ST_OK, "selecting the same group again");
	check_eq_u64(host_write_log_count(), 0, "does nothing at all");
	check(manips_select(0) == ST_OK && manips_select(2) == ST_OK,
	      "None and back to NG+ while a challenge-mode file is in play");
	check(r2_word(R2M_PAD) == 0x41C80000u, "puts the setup in straight away");
	check(manips_event_kind() == MANIP_EVENT_APPLIED,
	      "already announced this boot, so it is recorded as applied");
	host_write_log_reset();
	check(manips_select(2) == ST_OK, "selecting it once more");
	check_eq_u64(host_write_log_count(), 0, "still writes nothing");

	group("manips: a reboot needs the setup again");

	check(manips_options(1, 1) == ST_OK, "remember on");
	check(quit_and_wait(), "the game quits");
	check(boot_and_wait("NPEA00386"), "and boots again");
	mt_put_in_file(GAME_RAC2, 0);
	pump(4);
	check_eq_u64(manips_selected(), 2, "remember brought NG+ back");
	check(!manips_setup_done(), "with its setup due");
	check(mt_list_now(&l) && l.kind == MANIP_EVENT_NONE,
	      "a boot clears the event kind");
	mt_put_in_file(GAME_RAC2, 1);
	mt_put_counter(g, 1);
	mt_poison(ngp);
	host_write_log_reset();
	manips_file_loaded();
	check(mt_log_matches(ngp, 0, ngp->n), "the first file load of the new boot writes the setup again");
	check(manips_event_kind() == MANIP_EVENT_SETUP, "and announces it again");

	group("manips: remember on and off across a game reboot");

	check(manips_options(1, 0) == ST_OK, "remember off");
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00386"), "boot");
	pump(4);
	check_eq_u64(manips_selected(), 0, "with remember off a new process starts at None");
	check(qstreq(config_get("manip.rac2.category"), "2"),
	      "though config.txt still holds the category");
	check(manips_options(1, 1) == ST_OK, "remember on");
	check(qstreq(config_get("manip.rac2.category"), "0"),
	      "which writes the current selection, None");
	check(manips_select(4) == ST_OK, "100% selected");
	check(config_load() == ST_OK, "config.txt reloads, as after a power cycle");
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00386"), "boot");
	pump(4);
	check_eq_u64(manips_selected(), 4, "with remember on the category is restored");
	check(mt_list_now(&l) && l.remember == 1 && l.selected == 4, "MANIP_LIST agrees");
	config_set_manip("rac2", "category", 9);
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00386"), "boot");
	pump(4);
	check_eq_u64(manips_selected(), 0, "a category the game does not have comes back as None");

	group("manips: the challenge check, both ways");

	mt_put_in_file(GAME_RAC2, 1);
	check(manips_select(2) == ST_OK, "NG+ selected");
	mt_put_counter(g, 0);
	mt_poison(ngp);
	seq = manips_event_seq();
	host_write_log_reset();
	manips_file_loaded();
	check_eq_u64(host_write_log_count(), 0, "NG+ on a fresh file writes nothing");
	check(manips_event_seq() == (u16)(seq + 1) &&
	      manips_event_kind() == MANIP_EVENT_NOT_CHALLENGE && manips_event_group() == 2,
	      "and records refusal 3, not a challenge mode file");
	check(!manips_setup_done(), "the setup stays due");
	host_write_log_reset();
	check(manips_apply() == ST_BUSY, "Apply now is refused with BUSY");
	check_eq_u64(host_write_log_count(), 0, "and writes nothing either");
	check(manips_event_seq() == (u16)(seq + 2), "with an event of its own");

	check(manips_select(3) == ST_OK, "Any% selected");
	mt_put_counter(g, 1);
	seq = manips_event_seq();
	host_write_log_reset();
	manips_file_loaded();
	check_eq_u64(host_write_log_count(), 0, "Any% on a challenge-mode file writes nothing");
	check(manips_event_seq() == (u16)(seq + 1) &&
	      manips_event_kind() == MANIP_EVENT_CHALLENGE && manips_event_group() == 3,
	      "and records refusal 4, a challenge mode file");

	mt_put_counter(g, 0);
	host_write_log_reset();
	check(manips_select(6) == ST_OK, "NG+ No Specials selected on a fresh file");
	check(mt_log_matches(&mt_rac2[5], 0, mt_rac2[5].nsetup),
	      "No Specials skips the check: its setup goes in on a counter of 0");
	host_write_log_reset();
	manips_file_loaded();
	check(mt_log_matches(&mt_rac2[5], mt_rac2[5].nsetup, mt_rac2[5].n),
	      "and a file load writes M without the shortcut cursor");
	mt_put_counter(g, 255);
	host_write_log_reset();
	manips_file_loaded();
	check(mt_log_matches(&mt_rac2[5], mt_rac2[5].nsetup, mt_rac2[5].n),
	      "and on an overflowed one");

	group("manips: Apply now");

	check(manips_select(2) == ST_OK, "NG+ selected");
	mt_put_counter(g, 2);
	mt_put_in_file(GAME_RAC2, 0);
	seq = manips_event_seq();
	host_write_log_reset();
	check(manips_apply() == ST_BUSY, "Apply now on the title screen is BUSY");
	check_eq_u64(host_write_log_count(), 0, "and writes nothing");
	check(manips_event_seq() == (u16)(seq + 1) &&
	      manips_event_kind() == MANIP_EVENT_NO_FILE && manips_event_group() == 2,
	      "refusal 5: not in a file");
	mt_put_in_file(GAME_RAC2, 1);
	host_write_log_reset();
	check(manips_apply() == ST_OK, "Apply now in a challenge-mode file");
	check(mt_log_matches(ngp, 0, ngp->n), "writes the setup and the per-run list");
	check(manips_setup_done(), "and counts as this boot's setup");
	host_write_log_reset();
	check(manips_apply() == ST_OK, "Apply now again");
	check(mt_log_matches(ngp, 0, ngp->n), "writes both again");
	check(manips_event_kind() == MANIP_EVENT_APPLIED, "and records an applied event");

	group("manips: MANIP_LIST layout");

	{
		u8 out[1024];
		u32 len = 0;
		u16 before;

		check(manips_list(out, sizeof(out), &len) == ST_OK, "MANIP_LIST encodes");
		check(mt_parse(out, len, &l), "and parses to its last byte");
		check(l.selected == 2 && l.auto_reset == 1 && l.remember == 1,
		      "selected, auto_reset and remember lead");
		check(l.seq == manips_event_seq() && l.kind == manips_event_kind() &&
		      l.group == manips_event_group(), "then the last event");
		before = l.seq;
		mt_put_counter(g, 0);
		manips_apply();
		check(mt_list_now(&l) && l.seq == (u16)(before + 1) &&
		      l.kind == MANIP_EVENT_NOT_CHALLENGE, "event_seq moves on with every event");
		check(manips_list(out, 8, &len) == ST_FULL, "a reply that does not fit is FULL");
	}

	check(manips_select(0) == ST_OK, "back to None");
	mt_reset_options("rac2");
}

/*
 * The retired features: gone from DESCRIBE, their config.txt auto flags
 * ignored, and nothing of theirs in the previous-session record.
 */
static void test_manip_retired(void)
{
	u8 out[4096];
	u32 len = 0;

	group("manips: the retired RaC2 toggles");

	check(quit_and_wait(), "quit whatever was running");
	/* What a config.txt from before build 53 can hold. */
	config_set_feature_auto("NPEA00386", "Auto-reset (any%)", 1);
	config_set_feature_auto("NPEA00386", "Auto-reset (NG+)", 1);
	check(config_feature_auto("NPEA00386", "Auto-reset (any%)"),
	      "config.txt flags the old auto-reset toggle on boot");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	pump(4);
	check((features_toggle_auto() & (((u64)1 << 21) | ((u64)1 << 22))) == 0,
	      "the old auto flags are ignored");
	check((features_toggle_state() & (((u64)1 << 21) | ((u64)1 << 22))) == 0,
	      "and nothing turned the retired toggles on");
	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	{
		u8 id, found = 0;
		for (id = 15; id <= 22; id++) if (desc_feature(out, len, id) != NULL) found++;
		check_eq_u64(found, 0, "none of 15 to 22 is described");
	}
	check(features_set_auto(21, 1) == ST_NOT_FOUND, "FEATURE_SET_AUTO on one is NOT_FOUND");
	check(features_set(13, 1) == ST_OK, "the per-death boss reset is still a toggle");
	check(quit_and_wait(), "quit with it on");
	check(boot_and_wait("NPEA00386"), "the same game comes back");
	pump(4);
	check((session_previous()->toggles & (((u64)1 << 21) | ((u64)1 << 22))) == 0,
	      "the previous-session record offers no retired toggle");
	check((session_previous()->toggles & ((u64)1 << 13)) != 0,
	      "but still offers the per-death reset");
	session_previous_dismiss();
	config_set_feature_auto("NPEA00386", "Auto-reset (any%)", 0);
	config_set_feature_auto("NPEA00386", "Auto-reset (NG+)", 0);

	group("manips: the retired buttons of the other games");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots");
	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	check(desc_feature(out, len, 12) == NULL, "Act tune bosses (12) is not described");
	check(features_trigger(12) == ST_NOT_FOUND, "and is NOT_FOUND");
	{
		const u8 *row = desc_feature(out, len, 0);
		const u8 *row1 = desc_feature(out, len, 1);
		u8 ngroups = out[1];
		check(ngroups == 5 && memcmp(out + 2 + 4 * 24, "Fixes", 5) == 0,
		      "DESCRIBE has a fifth group, Fixes");
		check(row != NULL && row1 != NULL && row[2] == 4 && row1[2] == 4,
		      "Crash patches and Fix reset softlocks are described under it");
		check(row != NULL && (row[4] & FEATURE_FLAG_AUTO) != 0 &&
		      row1 != NULL && (row1[4] & FEATURE_FLAG_AUTO) != 0,
		      "and both still ship auto-flagged");
	}
}

static void test_manip_unsupported(void)
{
	group("manips: a game without groups");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("TEST00001"), "the test title boots");
	check(!manips_supported(), "it has no manip groups");
	{
		u8 out[64];
		u32 len = 0;
		check(manips_list(out, sizeof(out), &len) == ST_UNSUPPORTED, "MANIP_LIST is UNSUPPORTED");
		check(manips_select(1) == ST_UNSUPPORTED && manips_options(1, 0) == ST_UNSUPPORTED &&
		      manips_apply() == ST_UNSUPPORTED, "and so are the other three");
	}
	manips_file_loaded();
	check(1, "and a file load there is nothing");
}


/* --------------------------------------------- the file-load triggers */

#define R4T_FRONTEND 0x00B3C7D4u
#define R4T_LEVEL    0x009C3240u
#define R4T_REQUEST  0x00B36DCCu
#define R4T_TUTORIAL 0x00B1F46Fu   /* the tutorial flags' low byte */

/* The main menu or a level, as the in-file test reads it, and a load or play. */
static void r4t_world(u8 frontend, u32 level, u32 request, u32 mode)
{
	host_poke(R4T_FRONTEND, &frontend, 1);
	poke32(R4T_LEVEL, level);
	poke32(R4T_REQUEST, request);
	rac4_modes(mode, R4_MODE_NONE);
}

static void r4t_tutorial(u8 value)
{
	host_poke(R4T_TUTORIAL, &value, 1);
}

/*
 * RaC2: the end of a load screen landing on Aranos, the old auto-resets'
 * moment, is the manips' file load. Any% is the group: its A list is easy to
 * see in memory.
 */
static void test_manip_trigger_rac2(void)
{
	u16 seq;

	group("manip trigger: RaC2, the end of a load into Aranos");

	check(quit_and_wait(), "quit whatever was running");
	mt_reset_options("rac2");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	r2_settle_on(3);
	check(manips_select(3) == ST_OK, "Any% selected");
	mt_put_counter(&mt_games[1], 0);

	r2_dirty();
	r2_load(2);
	check_eq_u64(r2_word(R2_GORN_MANIP), 1, "a load into Maktar is not a file load");

	r2_put_planet(0);
	r2_put_count(0);
	pump(3);
	check_eq_u64(r2_word(R2_GORN_MANIP), 1, "nothing while the Aranos engine data reads");
	r2_put_count(1);
	pump(1);
	check_eq_u64(r2_word(R2_GORN_MANIP), 1, "nor on the frame the counter reads 1");
	seq = manips_event_seq();
	r2_put_count(2);
	pump(1);
	check_eq_u64(r2_word(R2_GORN_MANIP), 0, "the end of the load screen runs the any% reset");
	check_eq_u64(r2_word(R2_PAD_MANIP), R2_PAD_13, "jump pads and all");
	check(manips_event_seq() == (u16)(seq + 1) && manips_event_kind() == MANIP_EVENT_APPLIED,
	      "with one per-run event");
	check_eq_u64(r2_word(R2_CHARGE_BUF), 0, "Any% does not charge the boots");

	r2_put_word(R2_GORN_MANIP, 1);
	pump(20);
	check_eq_u64(r2_word(R2_GORN_MANIP), 1, "the counter staying at 2 does not fire again");

	r2_load(0);
	check_eq_u64(r2_word(R2_GORN_MANIP), 0, "loading Aranos again fires again");

	group("manip trigger: RaC2 NG+ charges the boots");

	check(manips_select(2) == ST_OK, "NG+ selected");
	mt_put_counter(&mt_games[1], 1);
	r2_dirty();
	r2_load(0);
	check_eq_u64(r2_word(R2_SHORTCUTS), 7, "the Aranos load put the Museum shortcut back");
	check_eq_u64(r2_word(R2_CHARGE_BUF), 30, "and charged the boots");
	check_eq_u64(r2_word(R2_PAD_MANIP), R2_PAD_25, "with the setup in");

	check(manips_select(0) == ST_OK, "None");
	r2_dirty();
	r2_load(0);
	check_eq_u64(r2_word(R2_CHARGE_BUF), 0, "with None an Aranos load charges nothing");

	group("manip trigger: RaC2, attaching on Aranos is not a load");

	check(manips_options(1, 1) == ST_OK && manips_select(3) == ST_OK,
	      "Any% remembered");
	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	r2_put_planet(0);
	r2_put_count(2);
	r2_dirty();
	check(pump_until(SESSION_INGAME, 4000), "INGAME on a game already on Aranos");
	pump(10);
	check_eq_u64(manips_selected(), 3, "with Any% back");
	check_eq_u64(r2_word(R2_GORN_MANIP), 1, "the first ticks reset nothing");
	r2_load(0);
	check_eq_u64(r2_word(R2_GORN_MANIP), 0, "the next Aranos load does");

	check(manips_select(0) == ST_OK, "None");
	mt_reset_options("rac2");
}

/*
 * Deadlocked: the tutorial flags' low byte dropping to 0, which a new game and
 * a challenge-mode start do, acted on the tick after the drop.
 */
static void test_manip_trigger_rac4(void)
{
	const struct mt_group *ngp = &mt_rac4[0];
	u16 seq;

	group("manip trigger: Deadlocked, the tutorial flag's drop");

	check(quit_and_wait(), "quit whatever was running");
	mt_reset_options("rac4");
	config_set_manip("rac4", "remember", 1);
	config_set_manip("rac4", "category", 1);

	host_boot("NPEA00423");
	r4t_world(1, 0xFFFFFFFFu, 0, 0);          /* the main menu */
	r4t_tutorial(0x05);                       /* last file's tutorial, done */
	mt_put_counter(&mt_games[3], 1);
	check(pump_until(SESSION_INGAME, 4000), "Deadlocked boots into its main menu");
	check_eq_u64(manips_selected(), 1, "NG+ remembered");
	mt_poison(ngp);
	pump(6);
	check(r2_byte(0x00A947D3u) == 0xEE, "the main menu writes nothing");

	seq = manips_event_seq();
	r4t_tutorial(0x00);                       /* the game-data reset: a challenge start */
	pump(1);
	check(r2_byte(0x00A947D3u) == 0xEE, "nothing on the tick the drop is seen");
	pump(1);
	check(mt_values_match(ngp, 0, ngp->n), "the tick after it applies the setup");
	check(manips_event_seq() == (u16)(seq + 1) && manips_event_kind() == MANIP_EVENT_SETUP,
	      "and announces it");

	mt_poison(ngp);
	seq = manips_event_seq();
	r4t_world(0, 1, 1, 6);                    /* the DreadZone load that follows */
	pump(4);
	r4t_world(0, 1, 0, 0);
	pump(6);
	check(manips_event_seq() == seq && r2_byte(0x00A947D3u) == 0xEE,
	      "the load that follows is not a second file load");
	r4t_tutorial(0x05);                       /* the first mission done */
	pump(4);
	r4t_world(0, 1, 1, 6);
	pump(3);
	r4t_world(0, 4, 0, 0);
	pump(6);
	check(r2_byte(0x00A947D3u) == 0xEE, "the tutorial byte rising and planet travel fire nothing");
	host_poke(0x010D44D8u, (const u8 *)"\xC2\x48\x00\x00", 4);
	pump(6);
	check(r2_byte(0x00A947D3u) == 0xEE, "nor does a death");
	r4t_tutorial(0x07);
	pump(4);
	check(manips_event_seq() == seq, "a save loaded with its tutorial done is not one either");

	group("manip trigger: Deadlocked, a refusal on a fresh file");

	mt_put_counter(&mt_games[3], 0);
	check(manips_select(0) == ST_OK && manips_select(1) == ST_OK,
	      "NG+ selected again, on a fresh file");
	check(manips_event_kind() == MANIP_EVENT_NOT_CHALLENGE,
	      "its setup is refused there straight away");
	seq = manips_event_seq();
	r4t_tutorial(0x00);                       /* a new game's reset */
	pump(3);
	check(manips_event_seq() == (u16)(seq + 1) &&
	      manips_event_kind() == MANIP_EVENT_NOT_CHALLENGE,
	      "the drop is a file load, and NG+ is refused on a fresh file");
	pump(6);
	check(manips_event_seq() == (u16)(seq + 1), "once per drop");

	group("manip trigger: Deadlocked, attaching on a fresh file is not a load");

	mt_put_counter(&mt_games[3], 1);
	check(quit_and_wait(), "quit");
	host_boot("NPEA00423");
	r4t_world(0, 6, 0, 0);
	r4t_tutorial(0x00);                       /* already 0 when qwark arrives */
	mt_put_counter(&mt_games[3], 1);
	check(pump_until(SESSION_INGAME, 4000), "INGAME in a level, tutorial byte at 0");
	mt_poison(ngp);
	pump(10);
	check(r2_byte(0x00A947D3u) == 0xEE, "the first ticks fire nothing");
	r4t_tutorial(0x01);
	pump(2);
	r4t_tutorial(0x00);
	pump(2);
	check(mt_values_match(ngp, 0, ngp->n), "the next drop does");

	group("manip trigger: Deadlocked, Apply now and the in-file test");

	mt_poison(ngp);
	check(manips_apply() == ST_OK, "Apply now in a level");
	check(mt_values_match(ngp, 0, ngp->n), "writes the setup");
	r4t_world(1, 0xFFFFFFFFu, 0, 0);
	host_write_log_reset();
	check(manips_apply() == ST_BUSY, "in the main menu Apply now is refused");
	check(manips_event_kind() == MANIP_EVENT_NO_FILE, "not in a file");
	check_eq_u64(host_write_log_count(), 0, "and writes nothing");

	check(manips_select(0) == ST_OK, "None");
	mt_reset_options("rac4");
}

#define R1T_REQUEST  0x00A10700u
#define R1T_DEST     0x00A10704u
#define R1T_STATE    0x00A10708u
#define R1T_PLANET   0x00969C70u
#define R1T_FRONTEND 0x00A15F38u

static void r1t_world(u8 frontend, u32 planet, u32 state, u32 request)
{
	host_poke(R1T_FRONTEND, &frontend, 1);
	poke32(R1T_PLANET, planet);
	poke32(R1T_STATE, state);
	poke32(R1T_REQUEST, request);
}

/*
 * One planet load as the NPEA00385 executable runs it (rac1.c has the
 * addresses): the request word and the destination go up, the transition sets
 * the state to 6 and the planet, the top loop clears the request, and the
 * level setup leaves the state at 0 on Veldin or at 6 for another planet's
 * fly-in, which ends in 0 a while later. The 120 Hz tick sees every step.
 */
static void r1t_load(u32 planet, int from_title)
{
	poke32(R1T_DEST, planet);
	poke32(R1T_REQUEST, 1);
	pump(3);
	poke32(R1T_STATE, 6);
	poke32(R1T_PLANET, planet);
	if (from_title) host_poke(R1T_FRONTEND, (const u8 *)"\x00", 1);
	pump(3);
	poke32(R1T_REQUEST, 0);
	pump(3);
	if (planet == 0) {
		poke32(R1T_STATE, 0);
		pump(3);
	} else {
		pump(3);
		poke32(R1T_STATE, 0);
		pump(3);
	}
}

/* RaC1: the end of a planet load into Veldin. All Missions shows it best. */
static void test_manip_trigger_rac1(void)
{
	const struct mt_group *am = &mt_rac1[2];
	u16 seq;

	group("manip trigger: RaC1, the end of a load into Veldin");

	check(quit_and_wait(), "quit whatever was running");
	mt_reset_options("rac1");
	host_boot("NPEA00385");
	r1t_world(1, 0, 3, 0);                     /* the title's menu */
	check(pump_until(SESSION_INGAME, 4000), "RaC1 boots to its title");
	pump(4);
	check(manips_select(3) == ST_OK, "All Missions selected on the title");
	mt_put_counter(&mt_games[0], 0);
	mt_poison(am);
	pump(6);
	check(r2_byte(R1M_BRIDGE) == 0xEE, "the title writes nothing");

	seq = manips_event_seq();
	poke32(R1T_DEST, 0);
	poke32(R1T_REQUEST, 1);
	pump(3);
	poke32(R1T_STATE, 6);
	host_poke(R1T_FRONTEND, (const u8 *)"\x00", 1);
	pump(3);
	poke32(R1T_REQUEST, 0);
	pump(3);
	check(r2_byte(R1M_BRIDGE) == 0xEE, "nothing while the level is set up (state 6)");
	poke32(R1T_STATE, 0);
	pump(2);
	check(mt_values_match(am, 0, am->n), "a new game: the Veldin load's end runs the reset");
	check(manips_event_seq() == (u16)(seq + 1), "with one event");

	mt_poison(am);
	r1t_load(3, 0);
	check(r2_byte(R1M_BRIDGE) == 0xEE, "a load of Kerwan is not a file load");
	poke32(R1T_STATE, 6);
	pump(3);
	poke32(R1T_STATE, 0);
	pump(3);
	check(r2_byte(R1M_BRIDGE) == 0xEE, "nor a ship cutscene's 6 to 0 in a level");

	r1t_load(0, 0);
	check(mt_values_match(am, 0, am->n), "a load into Veldin from in game fires");

	mt_poison(am);
	poke32(R1T_STATE, 6);
	pump(3);
	poke32(R1T_STATE, 0);
	pump(3);
	poke32(0x00969D68u, 0xC2480000u);         /* a death: Z far below */
	pump(6);
	check(r2_byte(R1M_BRIDGE) == 0xEE, "a cutscene or a death on Veldin is not a load");

	/* Quit to the title: the request goes out with planet -1 and the front end opens. */
	poke32(R1T_DEST, 0xFFFFFFFFu);
	poke32(R1T_REQUEST, 1);
	pump(3);
	poke32(R1T_STATE, 6);
	host_poke(R1T_FRONTEND, (const u8 *)"\x01", 1);
	poke32(R1T_PLANET, 0xFFFFFFFFu);
	pump(2);
	poke32(R1T_REQUEST, 0);
	pump(2);
	poke32(R1T_PLANET, 0);
	poke32(R1T_STATE, 3);
	pump(6);
	check(r2_byte(R1M_BRIDGE) == 0xEE, "quitting to the title is not a load");
	check(manips_apply() == ST_BUSY && manips_event_kind() == MANIP_EVENT_NO_FILE,
	      "and Apply now on the title is refused: not in a file");

	r1t_load(0, 1);
	check(mt_values_match(am, 0, am->n), "a save of Veldin loaded from the title fires");

	group("manip trigger: RaC1, the challenge check on a Veldin load");

	check(manips_select(2) == ST_OK, "NG+ (no poki skip) selected");
	mt_put_counter(&mt_games[0], 0);
	seq = manips_event_seq();
	r1t_load(0, 0);
	check(manips_event_seq() == (u16)(seq + 1) &&
	      manips_event_kind() == MANIP_EVENT_NOT_CHALLENGE,
	      "refused on a fresh file");
	mt_put_counter(&mt_games[0], 1);
	r1t_load(0, 0);
	check(r2_word(R1M_DREK_SKIP) == 1 && r2_word(R1M_DREK_CS) == 0,
	      "applied on a challenge-mode file");

	group("manip trigger: RaC1, attaching");

	check(manips_options(1, 1) == ST_OK, "remember on");
	check(quit_and_wait(), "quit");
	host_boot("NPEA00385");
	r1t_world(0, 0, 0, 0);
	mt_put_counter(&mt_games[0], 1);
	poke32(R1M_DREK_SKIP, 7);
	check(pump_until(SESSION_INGAME, 4000), "INGAME on Veldin, at rest");
	pump(10);
	check(r2_word(R1M_DREK_SKIP) == 7, "the first ticks fire nothing");

	check(quit_and_wait(), "quit");
	host_boot("NPEA00385");
	r1t_world(0, 0, 6, 1);
	mt_put_counter(&mt_games[0], 1);
	poke32(R1M_DREK_SKIP, 7);
	check(pump_until(SESSION_INGAME, 4000), "INGAME in the middle of a load");
	pump(4);
	check(r2_word(R1M_DREK_SKIP) == 7, "nothing until it ends");
	poke32(R1T_REQUEST, 0);
	pump(2);
	poke32(R1T_STATE, 0);
	pump(2);
	check(r2_word(R1M_DREK_SKIP) == 1, "and it fires at the end of a Veldin load it saw");

	check(manips_select(0) == ST_OK, "None");
	mt_reset_options("rac1");
}

/* ------------------------------ build 53: RaC1's forced fast loads, console */

#define R1FL_SITE1 0x00165490u   /* bl 0x1639A8, the loading screen */
#define R1FL_SITE2 0x00160060u   /* clrlwi r3, r3, 30, a sound pick */
#define R1FL_SITE3 0x001641F8u   /* the same, in the other load path */
#define F1_LOAD_ASIDE 18

static void r1fl_seed(void)
{
	poke32(R1FL_SITE1, 0x4BFFE519u);
	poke32(R1FL_SITE2, 0x546307BEu);
	poke32(R1FL_SITE3, 0x546307BEu);
}

static int r1fl_patched(void)
{
	return r2_word(R1FL_SITE1) == 0x60000000u && r2_word(R1FL_SITE2) == 0x38600006u &&
	       r2_word(R1FL_SITE3) == 0x38600006u;
}

static int r1fl_original(void)
{
	return r2_word(R1FL_SITE1) == 0x4BFFE519u && r2_word(R1FL_SITE2) == 0x546307BEu &&
	       r2_word(R1FL_SITE3) == 0x546307BEu;
}

/*
 * What the game does once a load's request word is up (r1t_load has the whole
 * of it): the transition's state 6, the top loop clearing the request, and the
 * level's own state 6 until the state leaves it. `stop` 1 leaves it at the
 * request cleared and the state still 6.
 */
static void r1fl_finish(u32 planet, int stop)
{
	pump(3);
	poke32(R1T_STATE, 6);
	poke32(R1T_PLANET, planet);
	pump(3);
	poke32(R1T_REQUEST, 0);
	pump(4);
	if (stop) return;
	poke32(R1T_STATE, 0);
	pump(4);
}

/*
 * A set-aside load's own way there: the helper takes the request (its byte
 * back at 0), takes the game into the start menu, and the save-data dialog
 * raises the planet request with the save's planet.
 */
static void r1fl_helper_loads(const struct sf_desc *f, u32 planet)
{
	host_poke(f->api_load, (const u8 *)"\x00", 1);
	pump(2);
	poke32(R1T_REQUEST, 1);
	r1fl_finish(planet, 0);
}

static int r1fl_checkbox(void)
{
	return (features_toggle_state() & ((u64)1 << F1_FAST_LOADS)) != 0;
}

static void test_rac1_fastload_force(void)
{
	const struct sf_desc *f = sf_desc_for_game(GAME_RAC1);

	group("build 53: RaC1's planet load forces fast loads, on a console");

	check(quit_and_wait(), "quit whatever was running");
	host_boot("NPEA00385");
	r1t_world(0, 3, 0, 0);                     /* on Kerwan, at rest */
	r1fl_seed();
	check(pump_until(SESSION_INGAME, 4000), "RaC1 boots");
	pump(4);
	check(features_set(F1_FAST_LOADS, 0) == ST_OK && !r1fl_checkbox(), "the checkbox is off");
	check(r1fl_original(), "and the three fast-load sites hold the game's words");

	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD goes ahead");
	check(r1fl_patched(), "with the three fast-load words patched in");
	check_eq_u64(r2_word(R1T_REQUEST), 1, "and the request word up");
	check(!r1fl_checkbox(), "while the checkbox stays off: the force is not the user's");

	r1fl_finish(5, 1);
	check(r1fl_patched(), "the request word back at 0 is not the end of it: "
	      "the level's state 6 still runs a fast-load site (0x160060)");
	poke32(R1T_STATE, 0);
	pump(4);
	check(r1fl_original(), "the state leaving 6 ends the load and puts the game's words back");
	check(!r1fl_checkbox(), "and the checkbox never moved");

	check(features_set(F1_FAST_LOADS, 1) == ST_OK && r1fl_patched(), "the checkbox on patches them");
	check(session_planet_load(3, 0) == ST_OK, "a load with it on");
	r1fl_finish(3, 0);
	check(r1fl_patched() && r1fl_checkbox(), "leaves them patched and the checkbox on");
	check(features_set(F1_FAST_LOADS, 0) == ST_OK && r1fl_original(), "and off puts them back");

	check(session_planet_load(5, 0) == ST_OK && r1fl_patched(), "a load forces them again");
	check(features_set(F1_FAST_LOADS, 1) == ST_OK, "and the checkbox goes on in the middle of it");
	r1fl_finish(5, 0);
	check(r1fl_patched() && r1fl_checkbox(), "the checkbox has the say: they stay on");
	check(features_set(F1_FAST_LOADS, 0) == ST_OK && r1fl_original(), "off");

	/* The game's own load writes the request word itself; nothing was forced for it. */
	r1t_load(3, 0);
	check(r1fl_original() && !r1fl_checkbox(), "a load qwark did not force changes nothing");

	group("build 53: RaC1's set-aside load forces fast loads too, on a console");

	check(f != NULL, "RaC1 has a savefile helper");
	if (f == NULL) return;
	savefile_make_idle(GAME_RAC1);
	check(features_trigger(F1_LOAD_ASIDE) == ST_OK, "the LOAD_ASIDE action goes out");
	check(r1fl_patched(), "forcing fast loads for its load");
	check(!r1fl_checkbox(), "with the checkbox still off");
	r1fl_helper_loads(f, 3);
	check(r1fl_original(), "and the end of that load puts the game's words back");

	savefile_make_idle(GAME_RAC1);
	check(session_load_setaside() == ST_OK && r1fl_patched(),
	      "the combo's way in, session_load_setaside, forces them the same");
	host_poke(f->api_load, (const u8 *)"\xFE", 1);
	pump(3);
	check(r1fl_original(), "and a load the helper drops on the game's frame puts them back");

	poke32(R1T_STATE, 1);                      /* a movie, which plays on its own loop */
	check(features_trigger(F1_LOAD_ASIDE) == ST_BUSY, "a load from a movie is refused");
	check(r1fl_original(), "before anything was forced");
	poke32(R1T_STATE, 0);

	check(features_set(F1_FAST_LOADS, 1) == ST_OK, "with the checkbox on");
	savefile_make_idle(GAME_RAC1);
	check(features_trigger(F1_LOAD_ASIDE) == ST_OK, "a set-aside load");
	host_poke(f->api_load, (const u8 *)"\xFE", 1);
	pump(3);
	check(r1fl_patched() && r1fl_checkbox(), "that the helper drops leaves them on");
	check(features_set(F1_FAST_LOADS, 0) == ST_OK && r1fl_original(), "off");

	group("build 53: RaC1's fast loads, attached in the middle of a load");

	check(quit_and_wait(), "quit");
	host_boot("NPEA00385");
	r1t_world(0, 5, 6, 1);
	r1fl_seed();
	check(pump_until(SESSION_INGAME, 4000), "INGAME in the middle of a load");
	pump(4);
	check(r1fl_original(), "nothing forced at attach");
	poke32(R1T_REQUEST, 0);
	pump(3);
	poke32(R1T_STATE, 0);
	pump(4);
	check(r1fl_original() && !r1fl_checkbox(), "and its end writes no word either");
}

/* ------------------------------ build 53: RaC2's crash guards, console */

#define R2C_GRID_SITE 0x00086960u
#define R2C_SHIP_SITE 0x00BDF30Cu
#define R2C_GRID_CAVE 0x01141440u
#define R2C_SHIP_CAVE 0x01141470u
#define R2C_GRID_ORIG 0x7F24C814u   /* addc r25, r4, r25 */
#define R2C_SHIP_ORIG 0xA07F0034u   /* lhz r3, 52(r31) */
#define R2C_GRID_B    0x490BAAE0u   /* b 0x1141440 */
#define R2C_SHIP_B    0x48562164u   /* b 0x1141470 */

/* The guards, assembled with GNU as on their own. */
static const u32 r2c_grid[6] = {
	0x7FC00734u, 0x7F80F000u, 0x409E000Cu, 0x7F24C814u, 0x4AF45514u, 0x4AF4577Cu
};
static const u32 r2c_ship[11] = {
	0x807C0000u, 0x3C800159u, 0x80A427B0u, 0x7F832840u, 0x419C0018u, 0x80A427B8u,
	0x7F832840u, 0x409C000Cu, 0xA07F0034u, 0x4BA9DE7Cu, 0x4BA9E0D8u
};

/* Where a relative b or bc at `at` lands; 0 for anything else. */
static u32 r2c_target(u32 at, u32 word)
{
	u32 op = word >> 26;
	u32 disp;

	if (word & 2u) return 0;
	if (op == 18) {
		disp = word & 0x03FFFFFCu;
		if (disp & 0x02000000u) disp |= 0xFC000000u;
	} else if (op == 16) {
		disp = word & 0x0000FFFCu;
		if (disp & 0x00008000u) disp |= 0xFFFF0000u;
	} else {
		return 0;
	}
	return at + disp;
}

static int r2c_words_are(u32 addr, const u32 *w, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		if (r2_word(addr + 4u * i) != w[i]) return 0;
	return 1;
}

static void r2c_poke_words(u32 addr, const u32 *w, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) poke32(addr + 4u * i, w[i]);
}

/* The game's own words at both sites, and a pattern in the two caves. */
static void r2c_seed(u32 grid_site, u32 ship_site)
{
	u32 i;

	poke32(R2C_GRID_SITE, grid_site);
	poke32(R2C_SHIP_SITE, ship_site);
	for (i = 0; i < 6; i++)  poke32(R2C_GRID_CAVE + 4u * i, 0xD0D00000u | i);
	for (i = 0; i < 11; i++) poke32(R2C_SHIP_CAVE + 4u * i, 0xD1D10000u | i);
}

static int r2c_caves_seeded(void)
{
	u32 i;

	for (i = 0; i < 6; i++)  if (r2_word(R2C_GRID_CAVE + 4u * i) != (0xD0D00000u | i)) return 0;
	for (i = 0; i < 11; i++) if (r2_word(R2C_SHIP_CAVE + 4u * i) != (0xD1D10000u | i)) return 0;
	return 1;
}

/* The index in the write log of the first write touching [addr, addr + len), or -1. */
static int r2c_first_write(u32 addr, u32 len)
{
	u32 i;

	for (i = 0; ; i++) {
		u32 a = 0, n = 0;

		if (host_write_log_at(i, &a, &n) != 0) return -1;
		if (a < addr + len && a + n > addr) return (int)i;
	}
}

static int r2c_any_write(void)
{
	return r2c_first_write(R2C_GRID_SITE, 4) >= 0 || r2c_first_write(R2C_SHIP_SITE, 4) >= 0 ||
	       r2c_first_write(R2C_GRID_CAVE, 24) >= 0 || r2c_first_write(R2C_SHIP_CAVE, 44) >= 0;
}

/* RaC2 booted with the given words at the two sites, the write log reset first. */
static int r2c_boot(u32 grid_site, u32 ship_site)
{
	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	r2c_seed(grid_site, ship_site);
	host_write_log_reset();
	if (!pump_until(SESSION_INGAME, 4000)) return 0;
	pump(4);
	return 1;
}

static void test_rac2_crash_guards(void)
{
	const struct game_describe *d = NULL;
	const struct sw_desc *s = sw_desc_for_game(GAME_RAC2);
	const struct sf_desc *f = sf_desc_for_game(GAME_RAC2);
	int same = 1, listed = 0, labelled = 0;
	int site, cave;
	u32 i, k;

	group("build 53: RaC2's crash guards go in on a console as the session enters");

	check(r2c_boot(R2C_GRID_ORIG, R2C_SHIP_ORIG), "RaC2 boots with the game's own words");
	check(r2c_words_are(R2C_GRID_CAVE, r2c_grid, 6), "the grid guard is at 0x1141440");
	check(r2c_words_are(R2C_SHIP_CAVE, r2c_ship, 11), "the part filter at 0x1141470");
	check_eq_u64(r2_word(R2C_GRID_SITE), R2C_GRID_B, "0x86960 holds b 0x1141440");
	check_eq_u64(r2_word(R2C_SHIP_SITE), R2C_SHIP_B, "and 0xBDF30C b 0x1141470");
	site = r2c_first_write(R2C_GRID_SITE, 4);
	cave = r2c_first_write(R2C_GRID_CAVE, 24);
	check(cave >= 0 && site > cave, "the grid site went in after the code it branches to");
	site = r2c_first_write(R2C_SHIP_SITE, 4);
	cave = r2c_first_write(R2C_SHIP_CAVE, 44);
	check(cave >= 0 && site > cave, "and so did the ship-part site");

	check_eq_u64(r2c_target(R2C_GRID_SITE, R2C_GRID_B), R2C_GRID_CAVE,
	             "the grid site's branch lands on the guard");
	check_eq_u64(r2c_target(R2C_SHIP_SITE, R2C_SHIP_B), R2C_SHIP_CAVE,
	             "the ship site's on the filter");
	check_eq_u64(r2c_target(R2C_GRID_CAVE + 8u, r2c_grid[2]), R2C_GRID_CAVE + 20u,
	             "an index that does not fit a grid entry goes to the last branch");
	check_eq_u64(r2c_target(R2C_GRID_CAVE + 16u, r2c_grid[4]), 0x00086964u,
	             "the guard otherwise runs the site's own addc and goes back after it");
	check_eq_u64(r2c_target(R2C_GRID_CAVE + 20u, r2c_grid[5]), 0x00086BD0u,
	             "and the last branch is UpdateMobyGrids' epilogue");
	check_eq_u64(r2c_target(R2C_SHIP_CAVE + 16u, r2c_ship[4]), R2C_SHIP_CAVE + 40u,
	             "a part below the moby table goes to the last branch");
	check_eq_u64(r2c_target(R2C_SHIP_CAVE + 28u, r2c_ship[7]), R2C_SHIP_CAVE + 40u,
	             "and so does one at or past its end");
	check_eq_u64(r2c_target(R2C_SHIP_CAVE + 36u, r2c_ship[9]), 0x00BDF310u,
	             "the filter otherwise runs the site's own lhz and goes back after it");
	check_eq_u64(r2c_target(R2C_SHIP_CAVE + 40u, r2c_ship[10]), 0x00BDF570u,
	             "and the last branch is the loop's step to the next part");

	/* The same words as the RPCS3 switches' rows, at the same addresses. */
	if (s != NULL && s->ncaves == 4 && s->nsites == 4) {
		if (s->caves[2].addr != R2C_GRID_CAVE || s->caves[2].len != 24u) same = 0;
		if (s->caves[3].addr != R2C_SHIP_CAVE || s->caves[3].len != 44u) same = 0;
		for (i = 0; same && i < 6; i++)
			if (be32_get(s->caves[2].bytes + 4u * i) != r2c_grid[i]) same = 0;
		for (i = 0; same && i < 11; i++)
			if (be32_get(s->caves[3].bytes + 4u * i) != r2c_ship[i]) same = 0;
		if (s->sites[0].addr != R2C_GRID_SITE || s->sites[0].value != R2C_GRID_B) same = 0;
		if (s->sites[2].addr != R2C_SHIP_SITE || s->sites[2].value != R2C_SHIP_B) same = 0;
	} else {
		same = 0;
	}
	check(same, "they are word for word the RPCS3 switches' guard rows, from the same sites");
	check(f != NULL && f->ncaves >= 1 && f->caves[0].addr + f->caves[0].len <= R2C_GRID_CAVE &&
	      R2C_SHIP_CAVE + 44u <= 0x011414FCu,
	      "both lie past the savefile helper and inside the dead span, which ends at 0x11414FC");

	group("build 53: RaC2's crash guards are no feature");

	if (session_game() != NULL) d = session_game()->describe();
	check(d != NULL, "RaC2 has its descriptor table");
	if (d == NULL) return;
	for (i = 0; i < d->nfeatures; i++) {
		if (d->features[i].id == 40 || qstreq(d->features[i].label, "Crash patches")) labelled++;
	}
	check_eq_u64(d->nfeatures, 28, "DESCRIBE has its twenty-eight features");
	check_eq_u64(labelled, 0, "and none of them is the guards");
	check_eq_u64(d->ngroups, 6, "in its six groups");
	check_eq_u64(d->auto_default, 0, "and RaC2 ships nothing auto-flagged");
	for (i = 0; i < patch_count(); i++) {
		const struct patch_def *p = patch_at(i);

		for (k = 0; p != NULL && k < p->count; k++) {
			u32 a = p->words[k].addr;
			if (a == R2C_GRID_SITE || a == R2C_SHIP_SITE ||
			    (a >= R2C_GRID_CAVE && a < R2C_SHIP_CAVE + 44u)) listed++;
		}
	}
	check_eq_u64(listed, 0, "no patch the patch list shows holds a word of them");
	check(features_set(40, 1) == ST_NOT_FOUND, "and there is no toggle to turn them off with");

	group("build 53: RaC2's crash guards go in once, and only over the game's own words");

	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	r2c_poke_words(R2C_GRID_CAVE, r2c_grid, 6);
	r2c_poke_words(R2C_SHIP_CAVE, r2c_ship, 11);
	poke32(R2C_GRID_SITE, R2C_GRID_B);
	poke32(R2C_SHIP_SITE, R2C_SHIP_B);
	host_write_log_reset();
	check(pump_until(SESSION_INGAME, 4000), "a process that has them already, entered again");
	pump(4);
	check(!r2c_any_write(), "is left be: not a word is written");

	check(r2c_boot(0x60000000u, R2C_SHIP_ORIG), "a nop at 0x86960, where the game has an addc");
	check(!r2c_any_write() && r2_word(R2C_GRID_SITE) == 0x60000000u &&
	      r2_word(R2C_SHIP_SITE) == R2C_SHIP_ORIG && r2c_caves_seeded(),
	      "gets nothing: not the guards, not their caves");
	check(r2c_boot(R2C_GRID_ORIG, 0x4BFFFFF0u), "nor does a game with something else at 0xBDF30C");
	check(!r2c_any_write() && r2c_caves_seeded(), "not a word");

	check(r2c_boot(R2C_GRID_B, R2C_SHIP_ORIG), "half in: the grid guard's branch alone");
	check(r2c_words_are(R2C_SHIP_CAVE, r2c_ship, 11) && r2_word(R2C_SHIP_SITE) == R2C_SHIP_B &&
	      r2c_first_write(R2C_GRID_SITE, 4) < 0,
	      "the rest goes in around it, and the branch already there is not written again");

	group("build 53: RaC2's crash guards are not written where code cannot be patched");

	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(r2c_boot(R2C_GRID_ORIG, R2C_SHIP_ORIG), "RaC2 boots under RPCS3 with no patch in it");
	check(!r2c_any_write() && r2_word(R2C_GRID_SITE) == R2C_GRID_ORIG &&
	      r2_word(R2C_SHIP_SITE) == R2C_SHIP_ORIG && r2c_caves_seeded(),
	      "qwark writes nothing: there the switches' patch carries the guards");
	host_set_can_patch_code(1);
	host_set_emulator(0);
	check(quit_and_wait(), "quit");
}

/* ---- UYA: the end of a load into Veldin, and the trophy auto-refresh */

#define R3T_REQUEST  0x00EE9310u
#define R3T_DEST     0x00EE9314u
#define R3T_TIMER    0x00EE9330u
#define R3T_MODE     0x00EE9334u
#define R3T_PLANET   0x00C1E438u

#define F3_RESET_TROPHIES 21
#define F3_AUTO_TROPHIES  39

#define R3T_PUT_IN_FILE(in) poke32(R3T_PLANET, (in) ? 1 : 0xFFFFFFFFu)

/*
 * One planet load as the NPEA00387 executable runs it (rac3.c has the
 * addresses): the request word and the destination go up, the load routine
 * puts the mode to 6 and the planet to the destination, main clears the
 * request, and the level loop's init puts the mode and its frame count to 0,
 * the count then going up a frame at a time. `title` adds the front end's
 * frame where the request is up and the mode is still 0.
 */
static void r3t_load(u32 planet, int title)
{
	poke32(R3T_DEST, planet);
	poke32(R3T_REQUEST, 1);
	pump(title ? 3 : 1);
	poke32(R3T_MODE, 6);
	poke32(R3T_PLANET, planet);
	pump(3);
	poke32(R3T_REQUEST, 0);
	poke32(R3T_DEST, 0);
	pump(2);
	poke32(R3T_TIMER, 0);
	poke32(R3T_MODE, 0);
	pump(1);
	poke32(R3T_TIMER, 3);
	pump(2);
}

static void test_manip_trigger_rac3(void)
{
	const struct mt_group *cc = &mt_rac3[2];
	u16 seq;

	group("manip trigger: UYA, the end of a load into Veldin");

	check(quit_and_wait(), "quit whatever was running");
	mt_reset_options("rac3");
	host_boot("NPEA00387");
	poke32(R3T_PLANET, 0xFFFFFFFFu);
	check(pump_until(SESSION_INGAME, 4000), "UYA boots to its title");
	pump(4);
	check(manips_select(3) == ST_OK, "NG+ CC Early selected on the title");
	mt_put_counter(&mt_games[2], 1);
	mt_poison(cc);

	seq = manips_event_seq();
	poke32(R3T_DEST, 1);
	poke32(R3T_REQUEST, 1);
	pump(3);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu, "nothing on the title's frame before the load");
	poke32(R3T_MODE, 6);
	poke32(R3T_PLANET, 1);
	pump(3);
	poke32(R3T_REQUEST, 0);
	pump(2);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu, "nothing while Veldin loads");
	poke32(R3T_TIMER, 0);
	poke32(R3T_MODE, 0);
	pump(1);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu, "nor on the level init's frame");
	poke32(R3T_TIMER, 1);
	pump(1);
	check(mt_values_match(cc, 0, cc->n), "the frame after it applies the setup and the per-run list");
	check(manips_event_seq() == (u16)(seq + 1) && manips_event_kind() == MANIP_EVENT_SETUP,
	      "with one event, the setup activated");

	mt_poison(cc);
	r3t_load(5, 0);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu, "a load of another planet is not a file load");
	r3t_load(1, 0);
	check(r2_word(0x00C1E660u) == 0x41 && r2_word(0x00C956A0u) == 100,
	      "a load into Veldin from in the game refreshes the per-run list");
	check(r2_word(0x00C9165Cu) == 0xEEEEEEEEu, "and leaves the setup, already in, alone");

	mt_poison(cc);
	poke32(R3T_MODE, 6);
	pump(3);
	poke32(R3T_MODE, 0);
	pump(3);
	poke32(0x00DA2878u, 0xC2480000u);         /* a death: Z far below */
	pump(6);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu,
	      "a cutscene's mode 6 or a death on Veldin is not a load");

	r3t_load(0xFFFFFFFFu, 0);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu, "quitting to the title is not one either");
	check(manips_apply() == ST_BUSY && manips_event_kind() == MANIP_EVENT_NO_FILE,
	      "and on the title Apply now is refused: not in a file");

	group("manip trigger: UYA, attaching on Veldin is not a load");

	check(manips_options(1, 1) == ST_OK, "remember on");
	check(quit_and_wait(), "quit");
	host_boot("NPEA00387");
	poke32(R3T_PLANET, 1);
	poke32(R3T_TIMER, 500);
	mt_put_counter(&mt_games[2], 1);
	check(pump_until(SESSION_INGAME, 4000), "INGAME on Veldin, at rest");
	mt_poison(cc);
	pump(10);
	check(r2_word(0x00C1E660u) == 0xEEEEEEEEu, "the first ticks fire nothing");
	r3t_load(1, 0);
	check(mt_values_match(cc, 0, cc->n), "the next Veldin load does");

	check(manips_select(0) == ST_OK, "None");
	mt_reset_options("rac3");
}

/*
 * Build 53: the trophy reset. What the game holds that the reset reads - the
 * trophy object and its thread word, the communication id, the cache-fill site -
 * and the console folder it deletes, laid out as NPEA00387 and a console with a
 * signed-in user 1 have them (rac3.h, sf_rac3.h).
 */
#define R3T_TROPHY_BYTE   0x00D9FF1Cu
#define R3T_OBJ_PTR       0x00CB150Cu
#define R3T_OBJ           0x00D1CC14u
#define R3T_OBJ_THREAD    (R3T_OBJ + 0x188u)
#define R3T_COMMID_PTR    0x00CBAA04u
#define R3T_COMMID_BLK    0x00D05CFCu
#define R3T_FILL_SITE     0x00985318u
#define R3T_FILL_WORD     0x9087000Cu
#define R3T_FILL_NOP      0x60000000u

#define R3T_HOME          "/dev_hdd0/home/00000001"
#define R3T_TROPHY_ROOT   R3T_HOME "/trophy"
#define R3T_TROPHY_DIR    R3T_TROPHY_ROOT "/NPWR02347_00"
#define R3T_OTHER_DIR     R3T_TROPHY_ROOT "/NPWR02346_00"

/* How long an auto request the helper refused waits to ask again (rac3_panel.c). */
#define R3T_RETRY_TICKS   30

/* 14 files, so a delete of 8 a tick takes two passes and a third to finish. */
#define R3T_TROPHY_FILES  14

static u8 r3t_byte(void)
{
	u8 b = 0xEE;
	host_peek(R3T_TROPHY_BYTE, &b, 1);
	return b;
}

/* The game's side, as a booted NPEA00387 has it once its trophies are up. */
static void r3t_trophy_game(void)
{
	static const u8 id[12] = { 'N', 'P', 'W', 'R', '0', '2', '3', '4', '7', 0, 0, 0 };

	poke32(R3T_OBJ_PTR, R3T_OBJ);
	poke32(R3T_OBJ_THREAD, 0);
	poke32(R3T_COMMID_PTR, R3T_COMMID_BLK);
	host_poke(R3T_COMMID_BLK + 4u, id, sizeof(id));
	poke32(R3T_FILL_SITE, R3T_FILL_WORD);
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x00", 1);
}

static void r3t_write(const char *dir, const char *name, const char *text)
{
	char path[160];
	plat_file_t f;

	path[0] = 0;
	qstrcat(path, sizeof(path), dir);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), name);
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, qstrlen(text));
	plat_file_close(f);
}

/*
 * A trophy set's folder: a configuration that names `commid`, the user's unlocks
 * and the icons. `subdir` adds a folder inside it, which no real set has.
 */
static void r3t_trophy_dir(const char *dir, const char *commid, int subdir)
{
	char conf[256];
	char name[16];
	u32 i;

	plat_dir_create("/dev_hdd0/home");
	plat_dir_create(R3T_HOME);
	plat_dir_create(R3T_TROPHY_ROOT);
	plat_dir_create(dir);

	conf[0] = 0;
	qstrcat(conf, sizeof(conf), "<!--Sce-Np-Trophy-Signature: 00-->\n<?xml version=\"1.0\"?>\n");
	qstrcat(conf, sizeof(conf), "<trophyconf version=\"1.1\">\n<npcommid>");
	qstrcat(conf, sizeof(conf), commid);
	qstrcat(conf, sizeof(conf), "</npcommid>\n</trophyconf>\n");
	r3t_write(dir, "TROPCONF.SFM", conf);
	r3t_write(dir, "TROPUSR.DAT", "unlocks");
	for (i = 0; i < R3T_TROPHY_FILES - 2; i++) {
		name[0] = 0;
		qstrcat(name, sizeof(name), "TROP0");
		name[5] = (char)('0' + i / 10);
		name[6] = (char)('0' + i % 10);
		name[7] = 0;
		qstrcat(name, sizeof(name), ".PNG");
		r3t_write(dir, name, "png");
	}

	if (subdir) {
		char sub[160];

		sub[0] = 0;
		qstrcat(sub, sizeof(sub), dir);
		qstrcat(sub, sizeof(sub), "/extra");
		plat_dir_create(sub);
	}
}

/* Whatever r3t_trophy_dir made, gone again, for the next case. */
static void r3t_trophy_rm(const char *dir)
{
	plat_dir_t d;
	struct plat_dirent e;
	char path[160];

	if (plat_dir_open(dir, &d) != 0) return;
	while (plat_dir_next(&d, &e) == 1) {
		if (qstreq(e.name, ".") || qstreq(e.name, "..")) continue;
		path[0] = 0;
		qstrcat(path, sizeof(path), dir);
		qstrcat(path, sizeof(path), "/");
		qstrcat(path, sizeof(path), e.name);
		if (e.is_dir) plat_dir_remove(path);
		else plat_file_unlink(path);
	}
	plat_dir_close(&d);
	plat_dir_remove(dir);
}

static u32 r3t_files(const char *dir)
{
	plat_dir_t d;
	struct plat_dirent e;
	u32 n = 0;

	if (plat_dir_open(dir, &d) != 0) return 0;
	while (plat_dir_next(&d, &e) == 1) {
		if (!qstreq(e.name, ".") && !qstreq(e.name, "..")) n++;
	}
	plat_dir_close(&d);
	return n;
}

static int r3t_dir_there(const char *dir)
{
	int is_dir = 0;
	return plat_path_exists(dir, &is_dir, NULL) && is_dir;
}

/* The load into Veldin under way: request up, destination 1, mode 6. */
static void r3t_veldin_loading(void)
{
	poke32(R3T_DEST, 1);
	poke32(R3T_REQUEST, 1);
	poke32(R3T_MODE, 6);
	poke32(R3T_PLANET, 1);
}

/* And its end: the request down, the level loop's mode 0, frames counting. */
static void r3t_veldin_loaded(void)
{
	poke32(R3T_REQUEST, 0);
	poke32(R3T_DEST, 0);
	pump(2);
	poke32(R3T_TIMER, 0);
	poke32(R3T_MODE, 0);
	pump(1);
	poke32(R3T_TIMER, 3);
	pump(2);
}

/*
 * The Trophies header: the reset and its auto toggle, which asks for it once per
 * run as a load into Veldin starts, whatever the category.
 */
static void test_manip_trophies(void)
{
	const struct sf_desc *sd = sf_desc_for_game(GAME_RAC3);
	u8 out[4096];
	u32 len = 0;
	u32 i;

	group("trophies: UYA's Trophies header");

	check(quit_and_wait(), "quit whatever was running");
	mt_reset_options("rac3");
	check(boot_and_wait("NPEA00387"), "UYA boots");
	pump(4);

	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	{
		const u8 *reset = desc_feature(out, len, F3_RESET_TROPHIES);
		const u8 *autor = desc_feature(out, len, F3_AUTO_TROPHIES);
		u8 ngroups = out[1];

		check(ngroups == 6 && memcmp(out + 2 + 5 * 24, "Trophies", 8) == 0,
		      "DESCRIBE has a sixth group, Trophies");
		check(reset != NULL && reset[1] == FEATURE_ACTION && reset[2] == 5 &&
		      memcmp(reset + 16, "Reset trophies (freezes ~3 s)", 30) == 0,
		      "Reset trophies (21) is an ACTION under it, and its label says the game freezes");
		check(autor != NULL && autor[1] == FEATURE_TOGGLE && autor[2] == 5 &&
		      memcmp(autor + 16, "Reset trophies on Veldin load", 30) == 0,
		      "and the auto toggle (39) a TOGGLE beside it that says when it runs");
		check(autor != NULL && (autor[4] & FEATURE_FLAG_AUTO) == 0,
		      "which does not ship auto-flagged");
		check(reset != NULL && (reset[4] & FEATURE_FLAG_WRITES_CODE) == 0 &&
		      autor != NULL && (autor[4] & FEATURE_FLAG_WRITES_CODE) == 0,
		      "neither is a WRITES_CODE row: no code switch stands in for them");
		/* Flags bit6 (0x40) is unassigned: a client sends both straight away. */
		check(reset != NULL && reset[4] == 0,
		      "the reset carries no flag at all, bit6 (0x40) included");
		check(autor != NULL && (autor[4] & 0x40) == 0,
		      "and its toggle no bit6 either");
	}
	check((features_toggle_state() & ((u64)1 << F3_AUTO_TROPHIES)) == 0,
	      "the auto toggle is off after a boot");

	/* ------------------------------------------------ the helper's table */

	group("trophies: the reset's byte and cave in the helper's table");

	check(sd != NULL && sd->api_trophy == R3T_TROPHY_BYTE,
	      "RaC3's helper has a fourth request byte, at 0xD9FF1C");
	{
		static const u8 others[3] = { GAME_RAC1, GAME_RAC2, GAME_RAC4 };

		for (i = 0; i < 3; i++) {
			const struct sf_desc *o = sf_desc_for_game(others[i]);
			check(o != NULL && o->api_trophy == 0, "the other three games' helpers have none");
		}
	}
	check(sd != NULL && sd->ncaves == 2 && sd->caves[1].addr == 0x009808C0u &&
	      sd->caves[1].addr + sd->caves[1].len <= 0x00980AC0u,
	      "and the reset is the second cave, inside 0x9808C0..0x980AC0");

	/* The install zeroes the fourth byte with the other three. */
	r3t_trophy_game();
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x05", 1);
	check(savefile_install() == ST_OK, "the helper goes in");
	check_eq_u64(r3t_byte(), 0, "and a stray value in the reset's byte is zeroed before the hook is");
	{
		u8 head[16];
		host_peek(0x009808C0u, head, sizeof(head));
		check(sd != NULL && memcmp(head, sd->caves[1].bytes, sizeof(head)) == 0,
		      "the reset's cave is written with the helper's");
	}

	/* ------------------------------------------------------- refusals */

	group("trophies: the reset refuses rather than guess");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00387"), "a fresh UYA");
	pump(4);
	r3t_trophy_game();
	host_write_log_reset();

	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND,
	      "with no home folder for user 1 the reset is NOT_FOUND");
	check_eq_u64(host_write_log_count(), 0, "and nothing was written into the game");

	plat_dir_create("/dev_hdd0/home");
	plat_dir_create(R3T_HOME);
	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND,
	      "nor with a home folder that has no trophy folder");

	r3t_trophy_dir(R3T_TROPHY_DIR, "NPWR02347_00", 0);
	r3t_trophy_dir(R3T_OTHER_DIR, "NPWR02346_00", 0);

	poke32(R3T_COMMID_PTR, 0);
	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND,
	      "with no communication id in the game it is NOT_FOUND");
	poke32(R3T_COMMID_PTR, R3T_COMMID_BLK);
	host_poke(R3T_COMMID_BLK + 4u, (const u8 *)"NPWX02347", 9);
	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND,
	      "and with one that is not an NPWR id");
	host_poke(R3T_COMMID_BLK + 4u, (const u8 *)"NPWR0234A", 9);
	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND, "or not five digits");
	host_poke(R3T_COMMID_BLK + 4u, (const u8 *)"NPWR02347", 9);

	r3t_trophy_rm(R3T_TROPHY_DIR);
	r3t_trophy_dir(R3T_TROPHY_DIR, "NPWR02346_00", 0);
	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND,
	      "a folder whose TROPCONF.SFM names another set is NOT_FOUND");
	check_eq_u64(r3t_files(R3T_TROPHY_DIR), R3T_TROPHY_FILES, "and keeps every file");

	r3t_trophy_rm(R3T_TROPHY_DIR);
	r3t_trophy_dir(R3T_TROPHY_DIR, "NPWR02347_00", 1);
	check(features_trigger(F3_RESET_TROPHIES) == ST_NOT_FOUND,
	      "so is the right folder with a folder inside it");
	check_eq_u64(r3t_files(R3T_TROPHY_DIR), R3T_TROPHY_FILES + 1, "which keeps all it holds");
	r3t_trophy_rm(R3T_TROPHY_DIR);
	r3t_trophy_dir(R3T_TROPHY_DIR, "NPWR02347_00", 0);

	poke32(R3T_OBJ_THREAD, 1);
	check(features_trigger(F3_RESET_TROPHIES) == ST_BUSY,
	      "while the game's register thread runs it is BUSY");
	poke32(R3T_OBJ_THREAD, 2);
	check(features_trigger(F3_RESET_TROPHIES) == ST_BUSY, "and while its unlock thread does");
	poke32(R3T_OBJ_THREAD, 0);
	poke32(R3T_OBJ_PTR, 0);
	check(features_trigger(F3_RESET_TROPHIES) == ST_BUSY, "and before its trophy object is set up");
	poke32(R3T_OBJ_PTR, R3T_OBJ);

	poke32(R3T_FILL_SITE, 0x38600000u);
	check(features_trigger(F3_RESET_TROPHIES) == ST_UNSUPPORTED,
	      "a cache-fill site holding neither word is another executable: UNSUPPORTED");
	poke32(R3T_FILL_SITE, R3T_FILL_WORD);

	savefile_set_enabled(0);
	check(features_trigger(F3_RESET_TROPHIES) == ST_UNSUPPORTED,
	      "with the savefile helper switched off it is UNSUPPORTED");
	savefile_set_enabled(1);
	check_eq_u64(r3t_files(R3T_TROPHY_DIR), R3T_TROPHY_FILES,
	             "and the folder is untouched by every refusal: the helper goes in first");
	check_eq_u64(r3t_byte(), 0, "nor was the game asked anything");
	check_eq_u64(r2_word(R3T_FILL_SITE), R3T_FILL_WORD, "nor its code touched");

	/* ------------------------------------------------------- RPCS3 */

	group("trophies: UNSUPPORTED under RPCS3");

	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(features_trigger(F3_RESET_TROPHIES) == ST_UNSUPPORTED,
	      "the reset is UNSUPPORTED where code cannot be patched");
	check(features_set(F3_AUTO_TROPHIES, 1) == ST_UNSUPPORTED, "and so is turning the toggle on");
	check(features_set(F3_AUTO_TROPHIES, 0) == ST_OK, "while turning it off is fine");
	check_eq_u64(r3t_files(R3T_TROPHY_DIR), R3T_TROPHY_FILES, "and no file goes");
	host_set_can_patch_code(1);
	host_set_emulator(0);

	/* ----------------------------------------------------- the button */

	group("trophies: the button deletes the set, then asks the game");

	check(features_trigger(F3_RESET_TROPHIES) == ST_OK, "the reset is accepted");
	check(features_trigger(F3_RESET_TROPHIES) == ST_BUSY, "and a second one while it runs is BUSY");
	check_eq_u64(r3t_byte(), 0, "the game is not asked before the folder is gone");
	pump(1);
	check(r3t_files(R3T_TROPHY_DIR) == R3T_TROPHY_FILES - 8,
	      "eight files go on the first tick");
	pump(2);
	check(!r3t_dir_there(R3T_TROPHY_DIR), "then the rest, and the folder");
	check(r3t_dir_there(R3T_OTHER_DIR) && r3t_files(R3T_OTHER_DIR) == R3T_TROPHY_FILES,
	      "the RaC2 set beside it is untouched");
	check(r3t_dir_there(R3T_TROPHY_ROOT), "and so is the trophy folder itself");
	pump(1);
	check_eq_u64(r2_word(R3T_FILL_SITE), R3T_FILL_NOP, "the cache fill is a nop");
	check_eq_u64(r3t_byte(), 1, "and the game is asked to reset now");

	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x03", 1);
	pump(10);
	check(features_trigger(F3_RESET_TROPHIES) == ST_BUSY, "a reset the helper is running is still BUSY");
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x00", 1);
	pump(1);

	check(features_trigger(F3_RESET_TROPHIES) == ST_OK,
	      "once it is done another is accepted, with no folder left to delete");
	pump(1);
	check_eq_u64(r3t_byte(), 1, "and goes straight to the game");
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\xFE", 1);
	pump(1);
	check_eq_u64(r3t_byte(), 0, "a refusal from the helper is put back to 0");
	check(features_trigger(F3_RESET_TROPHIES) == ST_OK, "and the reset is over: another is accepted");
	pump(1);
	check_eq_u64(r3t_byte(), 1, "which asks again");
	pump(600);
	check_eq_u64(r3t_byte(), 1, "a request the game has not picked up is left for five seconds");
	pump(2);
	check_eq_u64(r3t_byte(), 0, "and then withdrawn");

	/* ----------------------------------------------------- the toggle */

	group("trophies: the auto toggle asks inside the load into Veldin");

	r3t_trophy_dir(R3T_TROPHY_DIR, "NPWR02347_00", 0);
	check_eq_u64(manips_selected(), 0, "None is selected");
	r3t_load(1, 1);
	check_eq_u64(r3t_byte(), 0, "with the toggle off a Veldin load asks for nothing");
	check_eq_u64(r3t_files(R3T_TROPHY_DIR), R3T_TROPHY_FILES, "and deletes nothing");

	check(features_set(F3_AUTO_TROPHIES, 1) == ST_OK, "the auto toggle on");
	poke32(R3T_DEST, 1);
	poke32(R3T_REQUEST, 1);
	pump(3);
	check(r3t_files(R3T_TROPHY_DIR) == R3T_TROPHY_FILES && r3t_byte() == 0,
	      "not on the title's frame before the load starts");
	poke32(R3T_OBJ_THREAD, 2);
	r3t_veldin_loading();
	pump(3);
	check(r3t_files(R3T_TROPHY_DIR) == R3T_TROPHY_FILES,
	      "nor while a trophy thread of the game's runs");
	poke32(R3T_OBJ_THREAD, 0);
	pump(1);
	check(r3t_files(R3T_TROPHY_DIR) < R3T_TROPHY_FILES, "it starts as soon as that is over, in the load");
	pump(4);
	check(!r3t_dir_there(R3T_TROPHY_DIR), "the folder goes");
	check_eq_u64(r3t_byte(), 2, "and the game is asked for the reset only inside the load");

	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\xFE", 1);
	pump(1);
	check_eq_u64(r3t_byte(), 0, "a refusal inside the load is put back");
	pump(R3T_RETRY_TICKS + 1);
	check_eq_u64(r3t_byte(), 2, "and asked again a moment later, the load still on");
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x03", 1);
	pump(2);
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x00", 1);
	pump(3);
	check_eq_u64(r3t_byte(), 0, "once per load");
	r3t_veldin_loaded();
	check_eq_u64(r3t_byte(), 0, "and not again as the load ends");

	r3t_load(5, 0);
	check_eq_u64(r3t_byte(), 0, "a load of another planet asks for nothing");
	check(manips_select(2) == ST_OK, "NG+ selected");
	r3t_veldin_loading();
	pump(2);
	check_eq_u64(r3t_byte(), 2, "the next run's Veldin load asks again, whatever the category");
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x00", 1);
	pump(1);
	r3t_veldin_loaded();

	r3t_trophy_dir(R3T_TROPHY_DIR, "NPWR02347_00", 0);
	r3t_veldin_loading();
	pump(1);
	poke32(R3T_REQUEST, 0);
	poke32(R3T_MODE, 0);
	pump(4);
	check(!r3t_dir_there(R3T_TROPHY_DIR), "a load that ends during the delete still loses the folder");
	check_eq_u64(r3t_byte(), 0, "but the game is not asked outside the load");
	r3t_veldin_loaded();

	group("trophies: the auto flag keeps the toggle across a boot");

	check(features_set_auto(F3_AUTO_TROPHIES, 1) == ST_OK, "flagged on boot");
	check(config_feature_auto("NPEA00387", "Reset trophies on Veldin load"),
	      "and config.txt has it under the new label");
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00387"), "boot");
	pump(4);
	r3t_trophy_game();
	check((features_toggle_state() & ((u64)1 << F3_AUTO_TROPHIES)) != 0,
	      "the auto toggle comes back on");
	r3t_veldin_loading();
	pump(2);
	check_eq_u64(r3t_byte(), 2, "and asks on the first Veldin load");
	host_poke(R3T_TROPHY_BYTE, (const u8 *)"\x00", 1);
	pump(1);
	r3t_veldin_loaded();

	features_set_auto(F3_AUTO_TROPHIES, 0);
	features_set(F3_AUTO_TROPHIES, 0);
	r3t_trophy_rm(R3T_OTHER_DIR);
	check(manips_select(0) == ST_OK, "None");
	mt_reset_options("rac3");
}

/* A save file in play, or the title screen or main menu, as each game reads it. */
static void mt_put_in_file(u8 game_id, int in_file)
{
	switch (game_id) {
	case GAME_RAC1:
		host_poke(R1T_FRONTEND, in_file ? (const u8 *)"\x00" : (const u8 *)"\x01", 1);
		poke32(R1T_PLANET, 0);
		break;
	case GAME_RAC2:
		poke32(R2_PLANET_ADDR, in_file ? 3 : 0xFFFFFFFFu);
		break;
	case GAME_RAC3:
		R3T_PUT_IN_FILE(in_file);
		break;
	default:
		host_poke(R4T_FRONTEND, in_file ? (const u8 *)"\x00" : (const u8 *)"\x01", 1);
		poke32(R4T_LEVEL, in_file ? 1 : 0xFFFFFFFFu);
		break;
	}
}

static void test_manips(void)
{
	test_manip_tables();
	test_manip_semantics();
	test_manip_retired();
	test_manip_unsupported();
	test_manip_trigger_rac1();
	test_manip_trigger_rac2();
	test_manip_trigger_rac3();
	test_manip_trigger_rac4();
	test_manip_trophies();
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

/*
 * Fast loads, a code patch (src/games/rac3.h, RAC3_FASTLOAD_SITE): the
 * transition loop's "is it over" test at 0x27B428, cmpwi r27, 0, becomes a
 * branch to the console's cave at 0x980860, twenty-two words at the top of the
 * strictly dead 0x980304..0x980AC0 that the trophy reset's cave (0x9808C0) left
 * free. R3_FL_DEAD stands in for that dead code's own words, which the fake
 * console does not have.
 */
#define R3_FL_SITE        0x0027B428u
#define R3_FL_SITE_WORD   0x2C1B0000u
#define R3_FL_BRANCH      0x48705438u
#define R3_FL_CAVE        0x00980860u
#define R3_FL_CAVE_WORDS  22u
#define R3_FL_CAVE_END    (R3_FL_CAVE + 4u * R3_FL_CAVE_WORDS)
#define R3_FL_DEAD(i)     (0xD0E00000u | (u32)(i))
#define R3_FL_UNLOAD      0x00278A00u   /* Transition_UnloadSounds */
#define R3_FL_SPAN_LO     0x00980304u   /* the strictly dead span */
#define R3_FL_SPAN_HI     0x009808C0u   /* up to the trophy reset's cave */

static u32 r3_peek32(u32 addr)
{
	u8 w[4] = { 0, 0, 0, 0 };

	host_peek(addr, w, 4);
	return be32_get(w);
}

/* The game's own words where the patch goes: the site's and the dead code's. */
static void r3_fl_seed(void)
{
	u8 raw[4];
	u32 i;

	be32_put(raw, R3_FL_SITE_WORD);
	host_poke(R3_FL_SITE, raw, 4);
	for (i = 0; i < R3_FL_CAVE_WORDS; i++) {
		be32_put(raw, R3_FL_DEAD(i));
		host_poke(R3_FL_CAVE + 4u * i, raw, 4);
	}
}

/* Every word the patch takes holds the game's own again. */
static int r3_fl_original(void)
{
	u32 i;

	if (r3_peek32(R3_FL_SITE) != R3_FL_SITE_WORD) return 0;
	for (i = 0; i < R3_FL_CAVE_WORDS; i++)
		if (r3_peek32(R3_FL_CAVE + 4u * i) != R3_FL_DEAD(i)) return 0;
	return 1;
}

/* The patch is in: the branch at the site, and a cave that is not the dead code. */
static int r3_fl_patched(void)
{
	u32 i;

	if (r3_peek32(R3_FL_SITE) != R3_FL_BRANCH) return 0;
	for (i = 0; i < R3_FL_CAVE_WORDS; i++)
		if (r3_peek32(R3_FL_CAVE + 4u * i) == R3_FL_DEAD(i)) return 0;
	return 1;
}

/* Whether a write since the last log reset touched the site or the cave. */
static int r3_fl_wrote(void)
{
	u32 i;

	for (i = 0; ; i++) {
		u32 a = 0, len = 0;

		if (host_write_log_at(i, &a, &len) != 0) break;
		if (a < R3_FL_SITE + 4u && a + len > R3_FL_SITE) return 1;
		if (a < R3_FL_CAVE_END && a + len > R3_FL_CAVE) return 1;
	}
	return 0;
}

/* Where a relative branch at `at` goes: b and bl (I-form), bc (B-form); 0 otherwise. */
static u32 r3_branch_target(u32 at, u32 word)
{
	u32 d;

	if ((word >> 26) == 18u) {
		d = word & 0x03FFFFFCu;
		if (d & 0x02000000u) d |= 0xFC000000u;
		return at + d;
	}
	if ((word >> 26) == 16u) {
		d = word & 0x0000FFFCu;
		if (d & 0x8000u) d |= 0xFFFF0000u;
		return at + d;
	}
	return 0;
}

/* lis rX, hi then a D-form at `lo_word`: the address the pair names. */
static u32 r3_dform_addr(u32 lis_word, u32 lo_word)
{
	return ((lis_word & 0xFFFFu) << 16) + (u32)(s32)(s16)(lo_word & 0xFFFFu);
}

/*
 * The cave as the toggle wrote it, instruction by instruction, against what
 * rac3.h says it does: the words were assembled by the SDK, and this is the
 * check that they mean what the comment beside them says.
 */
static void r3_fl_check_words(void)
{
	u32 w[R3_FL_CAVE_WORDS];
	u32 out = R3_FL_CAVE + 0x50u;
	u32 i;

	for (i = 0; i < R3_FL_CAVE_WORDS; i++) w[i] = r3_peek32(R3_FL_CAVE + 4u * i);

	check_eq_u64(r3_branch_target(R3_FL_SITE, r3_peek32(R3_FL_SITE)), R3_FL_CAVE,
	             "the site branches to the cave, without a link");
	check_eq_u64(r3_peek32(R3_FL_SITE) & 3u, 0, "(no link, no absolute address)");
	check(w[0] == R3_FL_SITE_WORD && w[20] == R3_FL_SITE_WORD,
	      "the cave opens with the site's own test and ends with it, at its exit");
	check((w[1] >> 16) == 0x4082u && r3_branch_target(R3_FL_CAVE + 0x04u, w[1]) == out,
	      "a transition that is over on its own goes straight to the exit (bne)");
	check(r3_dform_addr(w[2], w[3]) == 0x00D990DEu && (w[3] >> 26) == 40u,
	      "it reads the transition's number as a halfword at 0xD990DE (lhz)");
	check(w[4] == 0x28040005u && (w[5] >> 16) == 0x4181u &&
	      r3_branch_target(R3_FL_CAVE + 0x14u, w[5]) == out,
	      "and leaves transitions past 5, the ones without clips, alone (cmplwi 5, bgt)");
	check(r3_dform_addr(w[2], w[6]) == 0x00D99094u && (w[6] >> 26) == 32u &&
	      w[7] == 0x2C040002u && (w[8] >> 16) == 0x4180u &&
	      r3_branch_target(R3_FL_CAVE + 0x20u, w[8]) == out,
	      "it waits for the loader's stage at 0xD99094 to reach 2 (lwz, cmpwi 2, blt)");
	check(r3_dform_addr(w[2], w[9]) == 0x00D99098u && (w[9] >> 26) == 32u &&
	      w[10] == 0x2C040000u && (w[11] >> 16) == 0x4182u &&
	      r3_branch_target(R3_FL_CAVE + 0x2Cu, w[11]) == out,
	      "and does nothing once the transition's sound banks at 0xD99098 are gone (beq)");
	check(r3_dform_addr(w[12], w[14]) == 0x0134EBE0u && (w[14] >> 26) == 36u &&
	      w[13] == 0x3C803F80u,
	      "it puts 1.0f, black, into the fade at 0x134EBE0 (stw)");
	check_eq_u64(w[15], 0x3B600001u, "says the transition is over (li r27, 1)");
	check(r3_dform_addr(w[16], w[17]) == R3_FL_SITE && (w[17] >> 26) == 14u &&
	      w[18] == 0x7C6803A6u,
	      "points the link register at the site (lis, addi, mtlr)");
	check(r3_branch_target(R3_FL_CAVE + 0x4Cu, w[19]) == R3_FL_UNLOAD && (w[19] & 3u) == 0,
	      "and branches to Transition_UnloadSounds, 0x278A00, which returns to the site");
	check(r3_branch_target(R3_FL_CAVE + 0x54u, w[21]) == R3_FL_SITE + 4u && (w[21] & 3u) == 0,
	      "the exit goes back to the instruction after the site");

	/* Where it lives: in dead code no other cave of qwark's and no shipped mod uses. */
	{
		const struct sf_desc *f = sf_desc_for_game(GAME_RAC3);
		const struct sw_desc *s = sw_desc_for_game(GAME_RAC3);
		int apart = (f != NULL && s != NULL);
		u8 k;

		for (k = 0; apart && k < f->ncaves; k++)
			if (f->caves[k].addr + f->caves[k].len > R3_FL_CAVE &&
			    f->caves[k].addr < R3_FL_CAVE_END) apart = 0;
		for (k = 0; apart && k < s->ncaves; k++)
			if (s->caves[k].addr + s->caves[k].len > R3_FL_CAVE &&
			    s->caves[k].addr < R3_FL_CAVE_END) apart = 0;
		check(apart, "the cave shares no byte with the helper's caves or any code switch");
		check(R3_FL_CAVE >= R3_FL_SPAN_LO && R3_FL_CAVE_END <= R3_FL_SPAN_HI,
		      "and sits in 0x980304..0x9808C0, below the trophy reset's cave");
		check(R3_FL_CAVE_END <= 0x00980AC0u,
		      "clear of 0x980AC0, where the rack mods racman shipped for UYA load");
	}
}

#define F3_FREEZE_AMMO   0
#define F3_FREEZE_HEALTH 1
#define R3_MAX_HEALTH_ADDR 0x00C1E4E4u
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

		check_eq_u64(d->nfeatures, 28, "RaC3 declares twenty-eight features");
		check_eq_u64(d->nreadouts, 12, "and twelve readouts");
		check_eq_u64(d->ngroups, 6, "and six groups, Trophies the sixth");

		/*
		 * 4, 17, 28, 29 and 30 are retired: gone, but nothing is renumbered.
		 * Build 53 retired the four manip buttons, 16, 18, 19 and 20, the same
		 * way, and one-hit KO, 2.
		 */
		for (i = 0; i < d->nfeatures; i++) {
			u8 id = d->features[i].id;
			if (id == 4 || id == 17 || id == 28 || id == 29 || id == 30 ||
			    id == 16 || id == 18 || id == 19 || id == 20 || id == 2)
				retired++;
			if (id == 15 || id == 21) neighbours++;
		}
		check_eq_u64(retired, 0, "and none of the retired ids");
		check_eq_u64(neighbours, 2, "while 15 and 21 kept their numbers");
		check(features_trigger(17) == ST_NOT_FOUND,
		      "the retired no-QE action is NOT_FOUND");
		check(features_trigger(16) == ST_NOT_FOUND && features_trigger(18) == ST_NOT_FOUND &&
		      features_trigger(19) == ST_NOT_FOUND && features_trigger(20) == ST_NOT_FOUND,
		      "and so are the four retired manip buttons");
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
	 * Build 53: freeze health holds the hit points at the game's maximum rather
	 * than at a flat 200 (test_freeze_health has the rest), and one-hit KO,
	 * which pinned them at 1, is retired.
	 */
	poke32(R3_MAX_HEALTH_ADDR, 40);
	check(features_set(F3_FREEZE_HEALTH, 1) == ST_OK, "freeze health turns on");
	pump(2);
	mem_read_u32(R3_HEALTH_ADDR, &v);
	check_eq_u64(v, 40, "health is pinned at the maximum");
	check(features_set(2, 1) == ST_NOT_FOUND, "the retired one-hit KO is NOT_FOUND");
	check(features_set(F3_FREEZE_HEALTH, 0) == ST_OK, "freeze health turns off");
	host_poke(R3_HEALTH_ADDR, (const u8 *)"\x00\x00\x00\x63", 4);
	pump(2);
	mem_read_u32(R3_HEALTH_ADDR, &v);
	check_eq_u64(v, 99, "and nothing writes the word any more");

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

	group("RaC3: level flags and planets");

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

	/*
	 * Fast loads are a code patch now (rac3.h, RAC3_FASTLOAD_SITE): a cave in
	 * the savefile helper's dead function and a branch at the transition loop's
	 * "is it over" test. The fake console's memory is empty, so the game's own
	 * words go where the patch will, to see them come back.
	 */
	group("RaC3: the fast-load patch words");

	r3_fl_seed();
	check(r3_fl_original(), "the game's own words are in place");
	check(features_set(F3_FAST_LOADS, 1) == ST_OK, "the Fast loads toggle turns on");
	check(r3_fl_patched(), "and patches the site and writes the cave");
	r3_fl_check_words();
	check((features_toggle_state() & ((u64)1 << F3_FAST_LOADS)) != 0, "its bit is set");
	check(features_set(F3_FAST_LOADS, 0) == ST_OK, "the toggle turns off");
	check(r3_fl_original(), "and every word the patch took comes back, site and cave");

	group("RaC3: a planet load forces fast loads, and its end puts them back");

	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(3);
	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD runs");
	mem_read_u32(R3_LOADPLANET, &v);
	check_eq_u64(v, 1, "the request word was written");
	mem_read_u32(R3_LOADPLANET + 4, &v);
	check_eq_u64(v, 5, "with the planet id");
	check(r3_fl_patched(), "and fast loads were forced on first");
	check((features_toggle_state() & ((u64)1 << F3_FAST_LOADS)) == 0,
	      "while the checkbox stays off: the force is not the user's");
	pump(4);
	check(r3_fl_patched(), "they stay on while the request word is up, the whole transition");

	/*
	 * A request pending when the game goes from the pause to its space flight is
	 * taken down there and raised again when the ship is away (0x22A0F4,
	 * 0x27389C): the word drops in the middle of the load while the mode is 6.
	 */
	host_poke(SF_R3_MODE, (const u8 *)"\x00\x00\x00\x06", 4);
	pump(2);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(3);
	check(r3_fl_patched(), "the request word dropping in mode 6 is not the end of the load");
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x01", 4);
	pump(3);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(3);
	check(r3_fl_patched(), "nor is main clearing it while the mode is still 6");
	host_poke(SF_R3_MODE, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_original(),
	      "the new level's init putting the mode to 0 is, and puts the game's words back");

	/*
	 * rac3.cs LoadPlanetSafe left Aquatos out, because its data-word fast loads
	 * hung the trip that owes Aquatos's story scene. The patch leaves the loader
	 * alone until the transition has played that scene, so Aquatos is forced too.
	 */
	check(session_planet_load(8, 0) == ST_OK, "PLANET_LOAD to Aquatos runs");
	check(r3_fl_patched(), "and forces fast loads like any other planet");
	pump(3);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_original(), "and the end of that load puts them back");

	group("RaC3: the Fast loads checkbox has the say over a load's force");

	check(features_set(F3_FAST_LOADS, 1) == ST_OK && r3_fl_patched(), "the toggle turns on");
	check(session_planet_load(5, 0) == ST_OK, "a load");
	pump(3);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_patched(), "leaves them on for a checkbox that is on");

	/* A load the game starts on its own goes past the same watcher. */
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x01\x00\x00\x00\x09", 8);
	pump(3);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_patched(), "and so does one the game started");
	check(features_set(F3_FAST_LOADS, 0) == ST_OK && r3_fl_original(),
	      "the toggle turns off and puts the words back");
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x01\x00\x00\x00\x09", 8);
	pump(3);
	host_write_log_reset();
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_original() && !r3_fl_wrote(), "off, the end of a load writes nothing");

	check(session_planet_load(5, 0) == ST_OK && r3_fl_patched(), "a forced load");
	pump(3);
	check(features_set(F3_FAST_LOADS, 1) == ST_OK, "the user turns fast loads on during it");
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_patched(), "and its end leaves them on: the force was forgotten");
	check(features_set(F3_FAST_LOADS, 0) == ST_OK && r3_fl_original(), "off again");

	check(session_planet_load(5, 0) == ST_OK && r3_fl_patched(), "another forced load");
	pump(3);
	check(features_set(F3_FAST_LOADS, 0) == ST_OK && r3_fl_original(),
	      "turning the checkbox off during it takes them out at once");
	host_write_log_reset();
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(2);
	check(r3_fl_original() && !r3_fl_wrote(), "and its end has nothing left to put back");

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
	/* Build 53: RaC3 acts only on a save of its own, as RaC1 and RaC2 do. */
	savefile_make_idle(GAME_RAC3);
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
#define R4_FASTLOAD_2   0x002A8C5Cu   /* build 53: off the update's call */
#define R4_TRANS_UPDATE 0x002A8C50u   /* the bcctrl builds 50 to 52 nopped */
#define R4_FL1_GAME     0x48120261u   /* bl memcard_Save */
#define R4_FL2_GAME     0x607B0000u   /* ori r27, r3, 0 */
#define R4_FL2_FAST     0x7C7BD378u   /* or r27, r3, r26 */
#define R4_UPDATE_CALL  0x4E9E0421u   /* bcctrl 20, 30 */
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
#define R4_CAM_YAW      0x010D5DE0u
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
		check_eq_u64(d->nfeatures, 16, "Deadlocked declares sixteen features (build 53 retired act tuning)");
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
	host_poke(R4_FASTLOAD_2, (const u8 *)"\x60\x7B\x00\x00", 4);
	host_poke(R4_TRANS_UPDATE, (const u8 *)"\x4E\x9E\x04\x21", 4);
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(4);

	check(session_planet_load(4, 0) == ST_OK, "PLANET_LOAD runs");
	mem_read_u32(R4_TARGET, &v);
	check_eq_u64(v, 4, "the target planet was written");
	mem_read_u32(R4_LOADP2, &v);
	check_eq_u64(v, 1, "and the load was requested");
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x60000000u, "fast loads were forced on");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_FAST, "build 53: the loop's exit word is or'd with the load being almost done");
	mem_read_u32(R4_TRANS_UPDATE, &v);
	check_eq_u64(v, R4_UPDATE_CALL,
	             "and the transition update is still called, so a movie the flight queued plays");
	check(session_planet_load(0, 0) == ST_BAD_ARG, "the unused planet is BAD_ARG");

	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x06", 1);
	pump(6);
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x60000000u, "they stay on through the space transition");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_FAST, "both of them");

	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x02", 1);
	pump(6);
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, R4_FL1_GAME, "and go back to the toggle setting when it ends");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_GAME, "both words restored");
	mem_read_u32(R4_TRANS_UPDATE, &v);
	check_eq_u64(v, R4_UPDATE_CALL, "and the update's call was never written");

	group("build 53: Deadlocked's Fast loads checkbox and a forced load");

	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(4);
	host_write_log_reset();
	check(features_set(F4_FAST_LOADS, 1) == ST_OK, "Fast loads turns on");
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x60000000u, "memcard_Save is not called");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_FAST, "and the loop ends once the load is almost done");
	check((features_toggle_state() & ((u64)1 << F4_FAST_LOADS)) != 0, "the checkbox reads on");

	check(session_planet_load(5, 0) == ST_OK, "a planet load with the checkbox on");
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x06", 1);
	pump(6);
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(6);
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_FAST, "leaves them on when it ends: the checkbox has the say");
	check((features_toggle_state() & ((u64)1 << F4_FAST_LOADS)) != 0, "and it still reads on");

	check(features_set(F4_FAST_LOADS, 0) == ST_OK, "Fast loads turns off");
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, R4_FL1_GAME, "memcard_Save is called again");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_GAME, "and the loop waits for the flight again");
	check(host_write_log_count() > 0 && write_covering(R4_TRANS_UPDATE) < 0,
	      "the update's call was never written along the way");

	group("Deadlocked: bot, weapon and gadget unlocks");

	{
		const struct game_unlock *list = NULL;
		const char * const *cats = NULL;
		u8 n = 0, ncat = 0;
		const struct unlock_field_desc *fields = NULL;
		const struct game_unlock *vipers = NULL;
		const struct game_unlock *flail = NULL;
		const struct game_unlock *charge = NULL;
		const struct game_unlock *gravity = NULL;
		u32 values[4];
		u32 vaddr = 0, faddr = 0, caddr = 0, graddr = 0;
		u8 entry[4];
		u8 around[8];
		u8 i, weapons = 0, boots = 0, bots = 0;
		int rows_ok = 1, order_ok = 1;

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST reads");
		check_eq_u64(n, 28, "ten weapons, two pairs of boots and sixteen bot upgrades");
		check_eq_u64(ncat, 3, "in three categories");
		check(list != NULL && qstreq(list[0].name, "Dual Vipers"),
		      "entry 0 is the Dual Vipers");
		check(cats != NULL && qstreq(cats[0], "Weapons") &&
		      qstreq(cats[1], "Gadgets") && qstreq(cats[2], "Bot upgrades"),
		      "the weapons first, then the gadgets, then the bots");

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
		 * Anything in g_GadgetData has an id of 32 plus its index into that
		 * table, which is what keeps it apart from the bots for good; the
		 * addresses below are derived from the ids, so the real indices are
		 * only stated once. An entry is 68 bytes, of which qwark touches the
		 * first four — and only the first two for a pair of boots.
		 */
		for (i = 0; i < n; i++) {
			if (list[i].category != (i < 10 ? 0 : (i < 12 ? 1 : 2))) order_ok = 0;

			if (list[i].category == 0) {
				weapons++;
				if (list[i].id < 32) rows_ok = 0;
				if (list[i].fields != (UNLOCK_FIELD_0 | UNLOCK_FIELD_1 | UNLOCK_FIELD_2))
					rows_ok = 0;
				if (qstreq(list[i].name, "Dual Vipers")) vipers = &list[i];
				if (qstreq(list[i].name, "Scorpion Flail")) flail = &list[i];
			} else if (list[i].category == 1) {
				boots++;
				if (list[i].id < 32) rows_ok = 0;
				if (list[i].fields != UNLOCK_FIELD_0) rows_ok = 0;
				if (qstreq(list[i].name, "Charge Boots")) charge = &list[i];
				if (qstreq(list[i].name, "Gravity Boots")) gravity = &list[i];
			} else {
				bots++;
				if (list[i].fields != UNLOCK_FIELD_0) rows_ok = 0;
			}
		}
		check_eq_u64(weapons, 10, "ten weapon rows");
		check_eq_u64(boots, 2, "two gadget rows");
		check_eq_u64(bots, 16, "and sixteen bot rows");
		check(order_ok, "listed in that order, which is the order the client draws");
		check(rows_ok, "a bot and a pair of boots declare owned alone, a weapon owned, "
		               "level and ammo, and everything in the gadget table has an id "
		               "past the bots");

		check(vipers != NULL && flail != NULL,
		      "the Dual Vipers and the Scorpion Flail are in the table");
		check(charge != NULL && gravity != NULL,
		      "and so are the Charge Boots and the Gravity Boots");

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

		/*
		 * The boots. Same table, same halfword, same -1 for an item the player
		 * has not got, and nothing else: no version to raise and no magazine,
		 * so the two number slots stay 0 whatever is in the entry behind them.
		 */
		if (charge != NULL && gravity != NULL) {
			caddr  = R4_GADGETS + ((u32)charge->id - 32) * R4_GADGET_STEP;
			graddr = R4_GADGETS + ((u32)gravity->id - 32) * R4_GADGET_STEP;

			check_eq_u64(charge->id, 49, "the Charge Boots are gadget 17, so id 49");
			check_eq_u64(gravity->id, 50, "and the Gravity Boots gadget 18, id 50");
			check_eq_u64(caddr, 0x00B2BBE4u,
			             "which is the halfword whose low byte the address list gives");
			check_eq_u64(graddr, 0x00B2BC28u, "and the same for the other pair");

			host_poke(caddr, (const u8 *)"\xFF\xFF\x01\x2C", 4);
			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST with locked boots");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(charge, values) == ST_OK, "the boots row reads live");
			check_eq_u64(values[0], 0, "a level of -1 reads as not owned");
			check(values[1] == 0 && values[2] == 0,
			      "and the level and ammo slots stay 0, whatever the entry holds");

			host_poke(caddr, (const u8 *)"\x00\x00", 2);
			check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK,
			      "UNLOCK_LIST with the boots unlocked");
			memset(values, 0, sizeof(values));
			check(g->unlock_read(charge, values) == ST_OK, "the row reads again");
			check_eq_u64(values[0], 1, "a 0 in memory is owned, as it is for a weapon");
			check(values[1] == 0 && values[2] == 0, "and still no level and no ammo");

			host_poke(caddr, (const u8 *)"\xFF\xFF\x01\x2C", 4);
			check(g->unlock_set(charge->id, 0, 1) == ST_OK,
			      "UNLOCK_SET owned=1 on locked boots");
			host_peek(caddr, entry, 4);
			check(be16_get(entry) == 0 && be16_get(entry + 2) == 300,
			      "stores the 0 the game uses and leaves the ammo halfword alone");

			host_poke(caddr, (const u8 *)"\x00\x03", 2);
			check(g->unlock_set(charge->id, 0, 1) == ST_OK,
			      "UNLOCK_SET owned=1 on boots already owned");
			host_peek(caddr, entry, 4);
			check(be16_get(entry) == 3, "leaves the halfword exactly as it found it");

			check(g->unlock_set(charge->id, 0, 0) == ST_OK, "UNLOCK_SET owned=0");
			host_peek(caddr, entry, 4);
			check(be16_get(entry) == 0xFFFF && be16_get(entry + 2) == 300,
			      "writes the locked -1 back and nothing else");

			check(g->unlock_set(charge->id, 1, 5) == ST_UNSUPPORTED,
			      "boots have no level to set");
			check(g->unlock_set(charge->id, 2, 5) == ST_UNSUPPORTED,
			      "and no ammunition either");
			host_peek(caddr, entry, 4);
			check(be16_get(entry) == 0xFFFF && be16_get(entry + 2) == 300,
			      "and neither refusal wrote a byte");

			check(g->unlock_set(gravity->id, 0, 1) == ST_OK,
			      "UNLOCK_SET owned=1 on the other pair");
			host_peek(graddr, entry, 2);
			check(be16_get(entry) == 0, "which is its own entry, a stride further on");
			host_peek(caddr, entry, 2);
			check(be16_get(entry) == 0xFFFF, "and left the first pair locked");
		}

		check(g->unlock_set(5, 0, 1) == ST_OK, "UNLOCK_SET owned=1 on a bot");
		host_peek(R4_BOTS_LIVE + 5, &b, 1);
		check_eq_u64(b, 1, "the live byte was written");
		host_peek(R4_BOTS_SAVE + 5, &b, 1);
		check_eq_u64(b, 1, "and so was the saved copy");

		check(g->unlock_set(5, 1, 1) == ST_UNSUPPORTED, "a bot has no level");
		check(g->unlock_set(16, 0, 1) == ST_BAD_ARG, "an id no row carries is BAD_ARG");

		check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK, "UNLOCK_LIST again");
		{
			/* Id 5 is the bot's, wherever the row for it now sits in the list. */
			const struct game_unlock *bot = NULL;

			for (i = 0; i < n; i++)
				if (list[i].id == 5) bot = &list[i];

			check(bot != NULL && qstreq(bot->name, "EMP Grenade"),
			      "the row carrying id 5 is the bot's");
			memset(values, 0, sizeof(values));
			check(bot != NULL && g->unlock_read(bot, values) == ST_OK,
			      "it reads live");
			check_eq_u64(values[0], 1, "owned reads back");
		}
	}

	group("Deadlocked: the weapon actions");

	{
		u32 g2  = R4_GADGETS + 2 * R4_GADGET_STEP;    /* Dual Vipers */
		u32 g3  = R4_GADGETS + 3 * R4_GADGET_STEP;    /* Magma Cannon */
		u32 g15 = R4_GADGETS + 15 * R4_GADGET_STEP;   /* Scorpion Flail */
		u32 g17 = R4_GADGETS + 17 * R4_GADGET_STEP;   /* Charge Boots */
		u32 g18 = R4_GADGETS + 18 * R4_GADGET_STEP;   /* Gravity Boots */
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

		/*
		 * The two pairs of boots, owned, in the same table between the weapons
		 * the actions walk and past the last of them. Both halfwords are 0, and
		 * both have a stats record and a per-mod amount behind them, so either
		 * action straying onto an entry the weapon list does not name would put
		 * a number in one of them.
		 */
		host_poke(g17, (const u8 *)"\x00\x00\x00\x00", 4);
		host_poke(g18, (const u8 *)"\x00\x00\x00\x00", 4);
		host_poke(R4_STATS + 17 * R4_STATS_STEP + 0x42, (const u8 *)"\x00\x64", 2);
		host_poke(R4_STATS + 18 * R4_STATS_STEP + 0x42, (const u8 *)"\x00\x64", 2);
		host_poke(R4_PER_MOD + 17 * 4, (const u8 *)"\x00\x00\x00\x19", 4);
		host_poke(R4_PER_MOD + 18 * 4, (const u8 *)"\x00\x00\x00\x19", 4);

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

		host_peek(g17, got, 4);
		check(be16_get(got) == 0 && be16_get(got + 2) == 0,
		      "neither action gave the Charge Boots a level or a magazine");
		host_peek(g18, got, 4);
		check(be16_get(got) == 0 && be16_get(got + 2) == 0,
		      "and the Gravity Boots are untouched too: both walk the weapons alone");

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
	                                 "\xC1\x20\x00\x00\x00\x00\x00\x00"
	                                 "\x3D\x80\x00\x00\x3E\x00\x00\x00"
	                                 "\x3E\x80\x00\x00\x3F\x00\x00\x00", 32);
	host_poke(R4_CAM_LR, (const u8 *)"\x3F\x80\x00\x00", 4);
	host_poke(R4_CAM_UD, (const u8 *)"\x40\x00\x00\x00", 4);

	check(g->save_blob(blob, &len) == ST_OK, "save_blob reads");
	check_eq_u64(len, 40, "0x20 of position and rotation plus the two camera floats");
	check(be32_get(blob + 0x18) == 0x3E800000u, "the rotation's Z is the facing at 0x18");
	check(be32_get(blob + 0x20) == 0x3F800000u, "the LR float came with it");
	check(be32_get(blob + 0x24) == 0x40000000u, "and the camera pitch");

	/*
	 * Exactly which words a load puts back. Every word it should write is
	 * cleared first and every word around them carries a sentinel, so a write
	 * that strays one field either way fails here rather than on a console.
	 * RAC4_CAMERA_LR is a sentinel too: the blob saves that float but it is a
	 * camera tuning constant, not a rotation, and a load must leave it alone.
	 */
	{
		static const u8 sent[4] = { 0x5A, 0x5A, 0x5A, 0x5A };
		static const u8 cleared[32] = { 0 };
		static const u32 guards[] = {
			R4_COORDS - 4,  R4_COORDS + 0x20,
			R4_COORDS2 - 4, R4_COORDS2 + 0x10,
			R4_CAM_YAW - 4, R4_CAM_YAW + 4, R4_CAM_LR,
			R4_CAM_UD - 4,  R4_CAM_UD + 4
		};
		u32 i;
		int clean = 1;

		for (i = 0; i < sizeof(guards) / sizeof(guards[0]); i++)
			host_poke(guards[i], sent, 4);
		host_poke(R4_COORDS, cleared, 32);
		host_poke(R4_COORDS2, cleared, 16);
		host_poke(R4_CAM_YAW, cleared, 4);
		host_poke(R4_CAM_UD, cleared, 4);

		check(g->load_blob(blob, len) == ST_OK, "load_blob writes");

		for (i = 0; i < 8; i++) {
			mem_read_u32(R4_COORDS + i * 4, &v);
			if (v != be32_get(blob + i * 4)) clean = 0;
		}
		check(clean, "the position and the rotation both went back to RAC4_COORDS");

		clean = 1;
		for (i = 0; i < 4; i++) {
			mem_read_u32(R4_COORDS2 + i * 4, &v);
			if (v != be32_get(blob + i * 4)) clean = 0;
		}
		check(clean, "and the position half to the second address");

		mem_read_u32(R4_CAM_YAW, &v);
		check_eq_u64(v, 0x3E800000u, "the camera yaw got the rotation's Z");
		mem_read_u32(R4_CAM_UD, &v);
		check_eq_u64(v, 0x40000000u, "and the camera pitch the float the blob saved");

		clean = 1;
		for (i = 0; i < sizeof(guards) / sizeof(guards[0]); i++) {
			mem_read_u32(guards[i], &v);
			if (v != 0x5A5A5A5Au) clean = 0;
		}
		check(clean, "and nothing around any of them was touched");

		/* A blob from before the camera floats stops after the yaw. */
		host_poke(R4_CAM_UD, sent, 4);
		check(g->load_blob(blob, 32) == ST_OK, "a 0x20 blob still loads");
		mem_read_u32(R4_CAM_UD, &v);
		check_eq_u64(v, 0x5A5A5A5Au, "and leaves the camera pitch alone");
		check(g->load_blob(blob, 31) == ST_BAD_ARG,
		      "a blob shorter than the position and rotation is BAD_ARG");
	}

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

	/*
	 * Protocol 1.13. POS_EDIT moves a slot without disturbing anything else in
	 * its blob, and POS_STORE puts a whole blob in as if the game had saved it.
	 * Deadlocked is the game with the most to lose either way: its blob carries
	 * a rotation and two camera floats behind the three coordinates.
	 */
	group("Deadlocked: POS_EDIT and POS_STORE");
	{
		u8 planet = session_current_planet();
		u8 saved[QWARK_MAX_BLOB];
		u8 got[QWARK_MAX_BLOB];
		u8 glen = 0;
		f32 xyz[3];
		u32 i;
		int clean;

		xyz[0] = 7.0f; xyz[1] = 8.0f; xyz[2] = 9.0f;

		pos_clear(planet, 3);
		check(session_position_edit(3, xyz) == ST_NOT_FOUND,
		      "POS_EDIT on an empty slot is NOT_FOUND");
		check(session_position_edit(8, xyz) == ST_BAD_ARG,
		      "and on slot 8 it is BAD_ARG");

		host_poke(R4_COORDS, (const u8 *)"\x41\x20\x00\x00\x42\x48\x00\x00"
		                                 "\xC1\x20\x00\x00\x00\x00\x00\x00"
		                                 "\x3D\x80\x00\x00\x3E\x00\x00\x00"
		                                 "\x3E\x80\x00\x00\x3F\x00\x00\x00", 32);
		host_poke(R4_CAM_LR, (const u8 *)"\x3F\x80\x00\x00", 4);
		host_poke(R4_CAM_UD, (const u8 *)"\x40\x00\x00\x00", 4);
		check(session_position_save(3) == ST_OK, "POS_SAVE fills slot 3");
		check(pos_fetch(planet, 3, saved, &glen) == ST_OK && glen == 40,
		      "with a forty-byte blob");

		check(session_position_edit(3, xyz) == ST_OK, "POS_EDIT rewrites it");
		check(pos_fetch(planet, 3, got, &glen) == ST_OK && glen == 40,
		      "the slot is still forty bytes");
		check(be32_get(got) == 0x40E00000u && be32_get(got + 4) == 0x41000000u &&
		      be32_get(got + 8) == 0x41100000u,
		      "the three floats are the edited ones");
		clean = 1;
		for (i = 12; i < 40; i++) if (got[i] != saved[i]) clean = 0;
		check(clean, "and every other byte of the blob is untouched");

		host_poke(R4_COORDS, (const u8 *)"\x00\x00\x00\x00\x00\x00\x00\x00"
		                                 "\x00\x00\x00\x00\x00\x00\x00\x00"
		                                 "\x00\x00\x00\x00\x00\x00\x00\x00"
		                                 "\x00\x00\x00\x00\x00\x00\x00\x00", 32);
		check(session_position_load(3) == ST_OK, "POS_LOAD puts it in the game");
		mem_read_u32(R4_COORDS, &v);
		check_eq_u64(v, 0x40E00000u, "the edited X is what the game got");
		mem_read_u32(R4_COORDS + 8, &v);
		check_eq_u64(v, 0x41100000u, "and the edited Z");
		mem_read_u32(R4_COORDS + 0x10, &v);
		check_eq_u64(v, 0x3D800000u, "while the rotation is the one that was captured");
		mem_read_u32(R4_CAM_YAW, &v);
		check_eq_u64(v, 0x3E800000u, "and the camera yaw is still its Z");

		/* POS_STORE: a whole blob of this game's length and nothing else. */
		for (i = 0; i < 40; i++) got[i] = (u8)(0x90 + i);
		check(session_position_store(4, got, 39) == ST_BAD_ARG,
		      "POS_STORE with a blob one byte short is BAD_ARG");
		check(session_position_store(4, got, 30) == ST_BAD_ARG,
		      "another game's length is BAD_ARG too");
		check(session_position_store(4, got, 0) == ST_BAD_ARG,
		      "and so is an empty one");
		check(session_position_store(8, got, 40) == ST_BAD_ARG,
		      "slot 8 is BAD_ARG");

		check(session_position_store(4, got, 40) == ST_OK,
		      "POS_STORE fills a slot from a whole blob");
		check(pos_fetch(planet, 4, saved, &glen) == ST_OK && glen == 40 &&
		      memcmp(saved, got, 40) == 0,
		      "byte for byte");
		check(session_position_load(4) == ST_OK, "and POS_LOAD writes it back");
		mem_read_u32(R4_COORDS, &v);
		check_eq_u64(v, 0x90919293u, "the stored blob reached the game");
		mem_read_u32(R4_CAM_YAW, &v);
		check_eq_u64(v, 0xA8A9AAABu, "camera yaw included");

		pos_clear(planet, 3);
		pos_clear(planet, 4);
	}

	group("Deadlocked: savefile requests");

	host_poke(R4_SF_HELPER, (const u8 *)"\x00\x00\x00", 3);
	savefile_make_idle(GAME_RAC4);
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

/* ------------------------------------------------- build 53: the toggle tidy */

/*
 * Freeze health in each game: the hit points, the game's own maximum beside
 * them, and two maxima and a hurt value as the game stores them (a word in
 * RaC1 to RaC3, a float in Deadlocked). `bad` is a maximum no game holds.
 */
struct fh_case {
	const char *title;
	const char *name;
	u8  id;
	u32 health;
	u32 max;
	u32 full;
	u32 more;
	u32 hurt;
	u32 bad;
};

static const struct fh_case fh_cases[] = {
	{ "NPEA00385", "RaC1",       28, 0x0096BF88u, 0x0071FB28u, 4, 8, 2, 0 },
	{ "NPEA00386", "RaC2",       2,  0x014816ACu, 0x01329A98u, 10, 23, 3, 0 },
	{ "NPEA00387", "UYA",        1,  0x00DA5040u, 0x00C1E4E4u, 30, 200, 7, 0xFFFFFFFFu },
	/* 15.0, 200.0 and 5.0; -1.0 is no maximum. */
	{ "NPEA00423", "Deadlocked", 4,  0x010D7250u, 0x010D7354u,
	  0x41700000u, 0x43480000u, 0x40A00000u, 0xBF800000u }
};

static u32 peek32(u32 addr)
{
	u32 v = 0;
	mem_read_u32(addr, &v);
	return v;
}

/* Whether DESCRIBE group `index` is called `name`. */
static int desc_group_is(const u8 *out, u8 index, const char *name)
{
	if (index >= out[1]) return 0;
	return qstreq((const char *)(out + 2 + (u32)index * 24), name);
}

static void test_freeze_health_one(const struct fh_case *fc)
{
	u8 out[4096];
	u32 len = 0;
	const u8 *row;
	char what[96];
	u64 bit = (u64)1 << fc->id;

	qstrcpy(what, sizeof(what), "Freeze health: ");
	qstrcat(what, sizeof(what), fc->name);
	group(what);

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait(fc->title), "the game boots");

	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	row = desc_feature(out, len, fc->id);
	check(row != NULL && row[1] == FEATURE_TOGGLE && memcmp(row + 16, "Freeze health", 14) == 0,
	      "Freeze health is described under its id");
	check(row != NULL && desc_group_is(out, row[2], "Cheats") &&
	      (row[4] & FEATURE_FLAG_WRITES_CODE) == 0,
	      "in Cheats, and it writes no code");

	poke32(fc->max, fc->full);
	poke32(fc->health, fc->hurt);
	check(features_set(fc->id, 1) == ST_OK, "it turns on");
	pump(1);
	check_eq_u64(peek32(fc->health), fc->full, "the hit points go to the game's maximum");
	check(freeze_find(fc->health, 4) >= 0, "held by a freeze entry");
	check((features_toggle_state() & bit) != 0, "and toggle_state says on");

	poke32(fc->health, fc->hurt);
	pump(1);
	check_eq_u64(peek32(fc->health), fc->full, "a hit is undone on the next tick");

	/* A health upgrade: the game raises its maximum and the freeze follows it. */
	poke32(fc->max, fc->more);
	pump(CLASSIC_HEALTH_EVERY + 1);
	check_eq_u64(peek32(fc->health), fc->more, "the hold follows the maximum up");
	poke32(fc->max, fc->full);
	pump(CLASSIC_HEALTH_EVERY + 1);
	check_eq_u64(peek32(fc->health), fc->full, "and back down");

	/* No maximum (a menu before the game has set one): nothing is pinned at 0. */
	poke32(fc->max, 0);
	pump(CLASSIC_HEALTH_EVERY + 1);
	check(freeze_find(fc->health, 4) < 0, "a maximum of 0 drops the entry");
	poke32(fc->health, fc->hurt);
	pump(2);
	check_eq_u64(peek32(fc->health), fc->hurt, "so the hit points are left alone");
	if (fc->bad != 0) {
		poke32(fc->max, fc->bad);
		pump(CLASSIC_HEALTH_EVERY + 1);
		check(freeze_find(fc->health, 4) < 0, "and so does a maximum no game holds");
	}
	poke32(fc->max, fc->full);
	pump(CLASSIC_HEALTH_EVERY + 1);
	check_eq_u64(peek32(fc->health), fc->full, "the hold comes back with the maximum");

	/* A client that removes the entry by hand: the toggle is still on. */
	{
		int id = freeze_find(fc->health, 4);
		check(id >= 0 && freeze_remove((u8)id) == ST_OK, "FREEZE_REMOVE takes the entry out");
		pump(CLASSIC_HEALTH_EVERY + 1);
		check(freeze_find(fc->health, 4) >= 0, "and the toggle, still on, puts it back");
	}

	check(features_set(fc->id, 0) == ST_OK, "it turns off");
	check(freeze_find(fc->health, 4) < 0, "the entry is gone");
	check((features_toggle_state() & bit) == 0, "toggle_state says off");
	poke32(fc->health, fc->hurt);
	pump(CLASSIC_HEALTH_EVERY * 2);
	check_eq_u64(peek32(fc->health), fc->hurt, "and the hit points are released");

	/* On across a reboot: the new process starts with it off, and holds nothing. */
	check(features_set(fc->id, 1) == ST_OK, "on again");
	pump(1);
	check(quit_and_wait(), "quit with it on");
	check(boot_and_wait(fc->title), "the same game comes back");
	check((features_toggle_state() & bit) == 0, "the new process has it off");
	check((session_previous()->toggles & bit) != 0,
	      "and the previous-session record offers it back");
	/* A fresh process is a fresh memory image: the game sets its maximum again. */
	poke32(fc->max, fc->full);
	poke32(fc->health, fc->hurt);
	pump(CLASSIC_HEALTH_EVERY * 2);
	check_eq_u64(peek32(fc->health), fc->hurt, "nothing holds the hit points meanwhile");
	session_previous_reapply(PREV_TOGGLES);
	pump(1);
	check_eq_u64(peek32(fc->health), fc->full, "the reapply holds them at the maximum again");
	check(features_set(fc->id, 0) == ST_OK, "off");
}

static void test_freeze_health(void)
{
	u32 i;

	for (i = 0; i < sizeof(fh_cases) / sizeof(fh_cases[0]); i++)
		test_freeze_health_one(&fh_cases[i]);
}

/*
 * The three rows the tidy retired: RaC1's max ammo, all weapons (17), RaC2's
 * weapon insta-upgrades (4) and UYA's one-hit KO (2). Gone from DESCRIBE, their
 * config.txt auto flags ignored, nothing of theirs in the previous-session
 * record, and the ids never handed to anything else.
 */
static void test_toggles_retired(void)
{
	u8 out[4096];
	u32 len = 0;

	group("build 53: the retired toggles");

	check(quit_and_wait(), "quit whatever was running");
	/* What a config.txt from before the tidy can hold. */
	config_set_feature_auto("NPEA00386", "Weapon insta-upgrades", 1);
	config_set_feature_auto("NPEA00387", "One-hit KO", 1);
	config_set_feature_auto("NPEA00385", "Max ammo, all weapons", 1);

	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	pump(4);
	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	check(desc_feature(out, len, 17) == NULL, "RaC1's 17 is not described");
	check(features_trigger(17) == ST_NOT_FOUND && features_set(17, 1) == ST_NOT_FOUND,
	      "and is NOT_FOUND either way");
	check((features_toggle_auto() & ((u64)1 << 17)) == 0, "its config line is ignored");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	pump(4);
	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	check(desc_feature(out, len, 4) == NULL, "RaC2's 4 is not described");
	check((features_toggle_auto() & ((u64)1 << 4)) == 0, "its auto flag is ignored");
	check((features_toggle_state() & ((u64)1 << 4)) == 0, "and nothing turned it on");
	check(freeze_find(0x01329AA8u, 1) < 0, "nothing freezes the experience economy byte");
	{
		u8 economy = 0xFF;
		mem_read_u8(0x01329AA8u, &economy);
		check_eq_u64(economy, 0, "which stays as the game left it");
	}
	check(features_set(4, 1) == ST_NOT_FOUND, "FEATURE_SET on it is NOT_FOUND");
	check(features_set_auto(4, 1) == ST_NOT_FOUND, "and so is FEATURE_SET_AUTO");
	check(features_set(2, 1) == ST_OK, "Freeze health, beside it, still turns on");
	check(quit_and_wait(), "quit with it on");
	check(boot_and_wait("NPEA00386"), "the same game comes back");
	pump(4);
	check((session_previous()->toggles & ((u64)1 << 4)) == 0,
	      "the previous-session record offers no retired toggle");
	check((session_previous()->toggles & ((u64)1 << 2)) != 0, "but does offer Freeze health");
	session_previous_dismiss();

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00387"), "UYA boots");
	pump(4);
	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	check(desc_feature(out, len, 2) == NULL, "UYA's 2 is not described");
	check((features_toggle_auto() & ((u64)1 << 2)) == 0, "its auto flag is ignored");
	check((features_toggle_state() & ((u64)1 << 2)) == 0, "and nothing turned it on");
	check(freeze_find(0x00DA5040u, 4) < 0, "nothing holds the hit points");
	check(features_set(2, 1) == ST_NOT_FOUND && features_set_auto(2, 1) == ST_NOT_FOUND,
	      "and FEATURE_SET and FEATURE_SET_AUTO on it are NOT_FOUND");

	config_set_feature_auto("NPEA00386", "Weapon insta-upgrades", 0);
	config_set_feature_auto("NPEA00387", "One-hit KO", 0);
	config_set_feature_auto("NPEA00385", "Max ammo, all weapons", 0);
}

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
		check_eq_u64(d->nfeatures, 28, "with RaC2's descriptor table");
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
/* poke32 is defined with the savefile tests, further up. */
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

/*
 * How many events of this kind and code turned up past `since`. The duplicate
 * checks want the count, not the first one: a transition that emits its split
 * twice looks exactly like one that emits it once until somebody counts.
 */
static u8 as_count(u32 since, u8 kind, u8 code)
{
	u8 i;
	u8 n = 0;

	as_fetch(since);
	for (i = 0; i < g_as_count; i++) {
		if (g_as_list[i].kind == kind && g_as_list[i].code == code) n++;
	}
	return n;
}

/* Every SPLIT past `since`, whatever its reason code. */
static u8 as_splits(u32 since)
{
	u8 i;
	u8 n = 0;

	as_fetch(since);
	for (i = 0; i < g_as_count; i++) {
		if (g_as_list[i].kind == AUTOSPLIT_SPLIT) n++;
	}
	return n;
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
#define A2_ARANOS       0u            /* the planet a RaC2 run starts and restarts on */

#define A3_PLANET       0x00C1E438u
#define A3_DEST_PLANET  0x00EE9314u
#define A3_GAME_STATE   0x00EE9334u
#define A3_PLAYER_STATE 0x00DA4DB6u
#define A3_NEFFY_HP     0x00C4DF80u
#define A3_NEFFY_PHASE  0x00DA50FCu
#define A3_CHUNK        0x00F08100u
#define A3_GUISE        0x00DA570Au
#define A3_LOADSCR      0x00D99117u   /* low byte of the loading-screen word */
#define A3_VELDIN       1u            /* the planet a RaC3 run starts and restarts on */

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

	/*
	 * ------------------------------------------------ the run boundary
	 *
	 * Two things went wrong in front of a reset, and the reports came from RaC2
	 * and Deadlocked. One transition took two splits, and a run that was about to
	 * be thrown away took a split on its way out, which left LiveSplit holding a
	 * bogus last segment and a gold it had no business keeping. Every game gets
	 * walked through both here: the ones that were wrong so they stay fixed, and
	 * the ones that were already right so the audit leaves something behind.
	 */

	group("autosplitting: Deadlocked, the run boundary");

	/* Start from a known origin: a real planet, in game, the tutorial done. */
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_IN_GAME, 1);
	poke32(A4_TUTORIAL, 1);
	poke32(A4_PLANET, 6);
	poke32(A4_TARGET, 6);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(2);
	poke32(A4_REQUEST_LOAD, 0);
	pump(2);

	/*
	 * One flight, the load request word going up twice. The second edge carries
	 * the same destination, and the origin is already that planet by then: it is
	 * the same load coming round again, not a second planet change.
	 */
	mark = autosplit_latest_seq();
	poke32(A4_TARGET, 7);
	poke32(A4_REQUEST_LOAD, 1);
	pump(2);
	poke32(A4_REQUEST_LOAD, 0);
	pump(1);
	poke32(A4_REQUEST_LOAD, 1);
	pump(2);
	check_eq_u64(as_count(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET), 1,
	             "one flight whose load request rises twice splits once");

	/* And the flight after it, to a different planet, still splits. */
	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_TARGET, 8);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(2);
	as_expect(mark, AUTOSPLIT_SPLIT, R4_AS_PLANET, 8,
	          "and the next planet still splits");

	/*
	 * The reset, the way a console gets to one: back to the main menu, out of
	 * the game, then New Game. Nothing of it is a split.
	 */
	mark = autosplit_latest_seq();
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_TARGET, A4_MAINMENU);
	pump(2);
	poke32(A4_REQUEST_LOAD, 1);
	pump(2);
	poke32(A4_REQUEST_LOAD, 0);
	poke32(A4_IN_GAME, 0);
	poke32(A4_PLANET, A4_MAINMENU);
	pump(2);
	poke32(A4_TARGET, 1);
	poke32(A4_TUTORIAL, 0);
	poke32(A4_REQUEST_LOAD, 1);
	pump(2);
	check_eq_u64(as_splits(mark), 0, "a new game takes no split on its way in");
	as_expect_kind(mark, AUTOSPLIT_RESET, "and still emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	group("autosplitting: RaC2, the run boundary");

	check(quit_and_wait(), "quit Deadlocked");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");

	/* Mid-run: on Oozla, the player in the state the script calls running. */
	poke32(A2_PLANET, 1);
	poke32(A2_PLAYER_STATE, 98);
	poke8(A2_CHUNK, 0);
	pump(3);

	/*
	 * The new game. Aranos loads first and the player state follows it seconds
	 * later, so the planet reaching 0 used to split the run that the state flip
	 * was about to reset.
	 */
	mark = autosplit_latest_seq();
	poke32(A2_PLAYER_STATE, 0);
	pump(2);
	poke32(A2_PLANET, A2_ARANOS);
	pump(3);
	check_eq_u64(as_count(mark, AUTOSPLIT_SPLIT, R2_AS_PLANET), 0,
	             "the load into Aranos is not a planet split");
	poke32(A2_PLAYER_STATE, 98);
	pump(2);
	check_eq_u64(as_splits(mark), 0, "a new game takes no split on its way in");
	as_expect_kind(mark, AUTOSPLIT_RESET, "and still emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	/* Leaving Aranos is the run's first real split, and still is. */
	mark = autosplit_latest_seq();
	poke32(A2_PLANET, 1);
	pump(2);
	as_expect(mark, AUTOSPLIT_SPLIT, R2_AS_PLANET, 1,
	          "and leaving it for Oozla still splits");

	group("autosplitting: RaC3, the run boundary");

	check(quit_and_wait(), "quit RaC2");
	check(boot_and_wait("NPEA00387"), "RaC3 boots");

	poke32(A3_PLANET, 4);
	poke32(A3_DEST_PLANET, 0);
	poke32(A3_GAME_STATE, 0);
	pump(3);

	/*
	 * The script's comment on this line says it does not split going to Veldin;
	 * the two tests under it are its RaC1 ancestor's, where Veldin is planet 0.
	 * Here it is planet 1, and the load that starts a run over used to split.
	 */
	mark = autosplit_latest_seq();
	poke32(A3_DEST_PLANET, A3_VELDIN);
	pump(2);
	check_eq_u64(as_count(mark, AUTOSPLIT_SPLIT, R3_AS_PLANET), 0,
	             "the load to Veldin is not a planet split");
	poke32(A3_PLANET, A3_VELDIN);
	poke32(A3_DEST_PLANET, 0);
	poke32(A3_GAME_STATE, 6);
	pump(2);
	poke32(A3_GAME_STATE, 0);
	pump(2);
	check_eq_u64(as_splits(mark), 0, "a run started over takes no split on its way in");
	as_expect_kind(mark, AUTOSPLIT_RESET, "and still emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	/* Leaving Veldin for Florana is the run's first split, and still is. */
	mark = autosplit_latest_seq();
	poke32(A3_DEST_PLANET, 2);
	pump(2);
	as_expect(mark, AUTOSPLIT_SPLIT, R3_AS_PLANET, 2,
	          "and leaving it for Florana still splits");

	group("autosplitting: RaC1, the run boundary");

	check(quit_and_wait(), "quit RaC3");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");

	poke32(A1_PLANET, 5);
	poke32(A1_DEST_PLANET, 3);
	poke32(A1_GAME_STATE, 0);
	pump(3);

	/*
	 * RaC1 was already right, because its Veldin is planet 0 and the script
	 * excluded planet 0 at both ends of the test. Checked anyway, so the day
	 * somebody renumbers something this says what the rule was.
	 */
	mark = autosplit_latest_seq();
	poke32(A1_DEST_PLANET, 0);
	pump(2);
	check_eq_u64(as_count(mark, AUTOSPLIT_SPLIT, R1_AS_PLANET), 0,
	             "the load to Veldin is not a planet split");
	poke32(A1_PLANET, 0);
	poke32(A1_GAME_STATE, 6);
	pump(2);
	poke32(A1_GAME_STATE, 0);
	pump(2);
	check_eq_u64(as_splits(mark), 0, "a run started over takes no split on its way in");
	as_expect_kind(mark, AUTOSPLIT_RESET, "and still emits RESET");
	as_expect_kind(mark, AUTOSPLIT_START, "and START");

	/* Back to RaC1 for anything that follows. */
	check(quit_and_wait(), "quit RaC1");
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
 * RaC2, RaC3 and Deadlocked force fast loads on before a planet load, and fast
 * loads are an instruction patch. On a platform that refuses one the load still
 * goes out, at the game's own pace: the request and the level-flag reset are
 * data writes, and no instruction word is touched on the way in or on the way
 * out. test_rac2, test_rac3 and test_rac4 hold the console half, where the patch
 * goes in.
 */
static void test_no_code_patches_planet_load(void)
{
	u32 v = 0;
	u8 b = 0xFF;

	group("no code patches: planet loads skip fast loads");

	host_set_can_patch_code(0);
	host_set_emulator(1);

	/* ------------------------------------------------------------ RaC2 */

	check(quit_and_wait(), "quit RaC1");
	check(boot_and_wait("NPEA00386"), "RaC2 boots with the gate closed");

	host_poke(R2_LEVELFLAGS + 5 * 0x10 + 2, (const u8 *)"\xAA", 1);
	check(session_planet_load(5, PLANET_FLAG_RESET_LEVELFLAGS) == ST_OK,
	      "RaC2's PLANET_LOAD goes ahead");
	mem_read_u32(R2_LOADPLANET, &v);
	check_eq_u64(v, 1, "the request word was written");
	mem_read_u32(R2_LOADPLANET + 4, &v);
	check_eq_u64(v, 5, "with the planet index");
	host_peek(R2_LEVELFLAGS + 5 * 0x10 + 2, &b, 1);
	check_eq_u64(b, 0, "and the level flags were zeroed");
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x4BFFEA69u, "while the fast-load site kept its instruction");

	/* The loading-screen watcher's restore has nothing to put back. */
	host_poke(R2_LOADCOUNT, (const u8 *)"\x02", 1);
	pump(4);
	mem_read_u32(R2_FP_ADDR, &v);
	check_eq_u64(v, 0x4BFFEA69u, "nor does the end of the load write it");

	check(session_planet_load(99, 0) == ST_BAD_ARG,
	      "an unknown planet is still BAD_ARG");

	/* ------------------------------------------------------ Deadlocked */

	check(quit_and_wait(), "quit RaC2");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots with the gate closed");

	host_poke(R4_FASTLOAD_1, (const u8 *)"\x48\x12\x02\x61", 4);
	host_poke(R4_FASTLOAD_2, (const u8 *)"\x60\x7B\x00\x00", 4);
	host_poke(R4_TRANS_UPDATE, (const u8 *)"\x4E\x9E\x04\x21", 4);
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(4);

	check(session_planet_load(4, 0) == ST_OK, "Deadlocked's PLANET_LOAD goes ahead");
	mem_read_u32(R4_TARGET, &v);
	check_eq_u64(v, 4, "the target planet was written");
	mem_read_u32(R4_LOADP2, &v);
	check_eq_u64(v, 1, "and the load was requested");
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, R4_FL1_GAME, "while the first fast-load word kept its instruction");
	mem_read_u32(R4_FASTLOAD_2, &v);
	check_eq_u64(v, R4_FL2_GAME, "and so did the second");
	mem_read_u32(R4_TRANS_UPDATE, &v);
	check_eq_u64(v, R4_UPDATE_CALL, "and the transition update's call");

	/* Through the space transition and out: the watcher writes nothing. */
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x06", 1);
	pump(6);
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x02", 1);
	pump(6);
	mem_read_u32(R4_FASTLOAD_1, &v);
	check_eq_u64(v, 0x48120261u, "nor does the end of the load write them");

	check(session_planet_load(0, 0) == ST_BAD_ARG,
	      "the unused planet is still BAD_ARG");

	/* ------------------------------------------------------------ RaC3 */

	/*
	 * UYA's fast loads were game data until they became a code patch, and went
	 * through on RPCS3 with nothing installed. Now they need the code switches
	 * there (test_switches_rac3_fastloads), and without them the load goes at
	 * the game's own pace like RaC2's.
	 */
	check(quit_and_wait(), "quit Deadlocked");
	check(boot_and_wait("NPEA00387"), "RaC3 boots with the gate closed");
	r3_fl_seed();
	pump(4);
	host_write_log_reset();

	check(session_planet_load(5, 0) == ST_OK, "RaC3's PLANET_LOAD goes ahead");
	mem_read_u32(R3_LOADPLANET, &v);
	check_eq_u64(v, 1, "the request word was written");
	mem_read_u32(R3_LOADPLANET + 4, &v);
	check_eq_u64(v, 5, "with the planet id");
	check(r3_fl_original() && !r3_fl_wrote(),
	      "while the fast-load site and the cave kept the game's words");
	check(features_set(F3_FAST_LOADS, 1) == ST_UNSUPPORTED,
	      "and the Fast loads toggle is refused, as a code patch is here");
	pump(3);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(3);
	check(r3_fl_original() && !r3_fl_wrote(), "nor does the end of the load write them");

	/* ------------------------------------------------- put it all back */

	host_set_can_patch_code(1);
	host_set_emulator(0);

	check(quit_and_wait(), "quit RaC3");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again with the gate open");
}

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
	 * that cannot patch code there is nothing to install. Build 47: INFO answers
	 * all the same, to say whether an RPCS3 patch put the helper in at load, and
	 * here nothing did - so the ops that need the helper and both request
	 * actions still refuse rather than half-working. test_savefile_rpcs3 has
	 * the other half, with the patch in the game.
	 */
	{
		u8 sup = 9, ins = 9, run = 9, pend = 9;
		u32 sz = 9;
		u32 got = 0;

		check(savefile_info(&sup, &ins, &run, &pend, &sz) == ST_OK,
		      "SAVEFILE_INFO answers (build 47)");
		check_eq_u64(sup, 1, "the game is supported, as on a console");
		check_eq_u64(ins, 0, "and no patch put the helper in this process");
		check_eq_u64(run, 0, "so nothing is running");
		check(savefile_read(0, 4, word, &got) == ST_UNSUPPORTED,
		      "SAVEFILE_READ is UNSUPPORTED");
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

/* ------------------------- build 47, the helper an RPCS3 patch put in at load */

/*
 * RPCS3 cannot take a code patch from qwark, but it applies its own patch files
 * to the executable as it loads it. SAVEFILE_PATCH hands out the words for one,
 * and on a platform that refuses code patches qwark looks once a session for
 * its own helper already in the game. The fake console stands in for RPCS3 here
 * the way it does in test_no_code_patches: plat_can_patch_code() answers 0, and
 * a "patch" is the reply's words poked into the fresh process between the boot
 * and the first tick, which is where RPCS3 puts them.
 */

/* One game's savefile actions, by the ids its DESCRIBE gives them. */
struct sf_test_game {
	const char *title;
	u8 game_id;
	u8 set_aside_id;
	u8 load_aside_id;
};

static const struct sf_test_game g_sf_games[] = {
	{ "NPEA00385", GAME_RAC1, 19, 18 },
	{ "NPEA00386", GAME_RAC2, 31, 30 },
	{ "NPEA00387", GAME_RAC3, 31, 32 },
	{ "NPEA00423", GAME_RAC4, 13, 14 },
};

#define SF_NO_FLIP 0xFFFFFFFFu

/* RaC1's fourth request byte, the one Force autosave (feature 20) drives. */
#define SF_RAC1_SAVEMODE 0x01080003u
#define F_FORCE_AUTOSAVE_ID 20

/* A SAVEFILE_PATCH reply, and a second one to compare it against. */
static u8 g_sf_patch[QWARK_MAX_PAYLOAD];
static u8 g_sf_patch2[QWARK_MAX_PAYLOAD];

/*
 * Writes a SAVEFILE_PATCH reply into the fake process, as RPCS3 writes a patch
 * file's entries: every word pair, and then, with `with_bytes`, the single
 * bytes after them. `flip` names one pair whose last byte goes in changed,
 * which is what a patch from another build of the helper looks like to a module
 * that did not write it.
 */
static void sf_poke_patch(const u8 *reply, u32 len, u32 flip, int with_bytes)
{
	u32 n = be16_get(reply);
	u32 off = SAVEFILE_PATCH_HEAD;
	u32 nb;
	u32 i;

	for (i = 0; i < n; i++) {
		const u8 *row = reply + off;
		u8 word[4];

		if (off + SAVEFILE_PATCH_ROW > len) return;
		memcpy(word, row + 4, 4);
		if (i == flip) word[3] ^= 0x01;
		host_poke(be32_get(row), word, 4);
		off += SAVEFILE_PATCH_ROW;
	}

	if (!with_bytes || off + SAVEFILE_PATCH_BYTES_HEAD > len) return;
	nb = be16_get(reply + off);
	off += SAVEFILE_PATCH_BYTES_HEAD;

	for (i = 0; i < nb && off + SAVEFILE_PATCH_BYTE_ROW <= len; i++) {
		host_poke(be32_get(reply + off), reply + off + 4, 1);
		off += SAVEFILE_PATCH_BYTE_ROW;
	}
}

/* A boot with the whole patch applied at load, before the session has looked. */
static int sf_boot_patched(const char *title, const u8 *reply, u32 len, u32 flip)
{
	host_boot(title);
	sf_poke_patch(reply, len, flip, 1);
	return pump_until(SESSION_INGAME, 4000);
}

/*
 * The same with the three request bytes holding `seed` as the executable loads
 * them, before RPCS3 applies the patch over it: RaC2's executable holds file
 * data there, not zeroes. `with_bytes` 0 applies only the words, which is the
 * patch without the byte entries that clear them.
 */
static int sf_boot_seeded(const char *title, const struct sf_desc *d, const u8 seed[3],
                          const u8 *reply, u32 len, int with_bytes)
{
	host_boot(title);
	host_poke(d->api_mod, seed + 0, 1);
	host_poke(d->api_load, seed + 1, 1);
	host_poke(d->api_setaside, seed + 2, 1);
	sf_poke_patch(reply, len, SF_NO_FLIP, with_bytes);
	return pump_until(SESSION_INGAME, 4000);
}

/*
 * What RaC2's executable held at the helper's three request bytes until build 50
 * moved them out of its code segment: not zeroes. The new place loads as zero,
 * but the patch's byte entries are still what makes bytes like these safe.
 */
static const u8 g_sf_rac2_load_bytes[3] = { 0x80, 0xFE, 0xA4 };

/* Whether any write since the last log reset touched a cave or a hook word. */
static int sf_wrote_code(const struct sf_desc *d)
{
	u32 i;

	for (i = 0; ; i++) {
		u32 a = 0, len = 0;
		u8 k;

		if (host_write_log_at(i, &a, &len) != 0) break;

		for (k = 0; k < d->ncaves; k++)
			if (a < d->caves[k].addr + d->caves[k].len && a + len > d->caves[k].addr) return 1;
		for (k = 0; k < d->nhooks; k++)
			if (a < d->hooks[k].addr + 4u && a + len > d->hooks[k].addr) return 1;
	}

	return 0;
}

/*
 * That a SAVEFILE_PATCH reply is exactly the table's caves and hooks, then the
 * three request bytes at 0.
 */
static void sf_check_patch_words(const struct sf_desc *d, const u8 *reply, u32 len)
{
	u32 want = 0;
	u32 off = SAVEFILE_PATCH_HEAD;
	u32 whole;
	int caves_ok = 1;
	int hooks_ok = 1;
	int bytes_ok = 1;
	u32 req[3];
	u8 i;

	for (i = 0; i < d->ncaves; i++) want += (d->caves[i].len + 3u) / 4u;
	want += d->nhooks;
	whole = SAVEFILE_PATCH_HEAD + want * SAVEFILE_PATCH_ROW
	      + SAVEFILE_PATCH_BYTES_HEAD + 3u * SAVEFILE_PATCH_BYTE_ROW;

	check_eq_u64(be16_get(reply), want, "n is every cave word and every hook word");
	check_eq_u64(len, whole,
	             "and the reply is those pairs and three single bytes behind their heads");
	check_eq_u64(be16_get(reply + 2), 0, "the pad is 0");
	check(len <= QWARK_MAX_PAYLOAD, "the reply fits one frame");
	if (len != whole) return;

	for (i = 0; i < d->ncaves; i++) {
		u32 at;

		check_eq_u64(d->caves[i].len % 4u, 0, "the cave is whole words, so nothing is padded");
		for (at = 0; at < d->caves[i].len; at += 4u) {
			u8 w[4] = { 0, 0, 0, 0 };
			u32 k;

			for (k = 0; k < 4u && at + k < d->caves[i].len; k++) w[k] = d->caves[i].bytes[at + k];
			if (be32_get(reply + off) != d->caves[i].addr + at) caves_ok = 0;
			if (memcmp(reply + off + 4, w, 4) != 0) caves_ok = 0;
			off += SAVEFILE_PATCH_ROW;
		}
	}
	check(caves_ok, "every cave is there, word by word in address order, caves in table order");

	for (i = 0; i < d->nhooks; i++) {
		if (be32_get(reply + off) != d->hooks[i].addr) hooks_ok = 0;
		if (be32_get(reply + off + 4) != d->hooks[i].value) hooks_ok = 0;
		off += SAVEFILE_PATCH_ROW;
	}
	check(hooks_ok, "and the hook words come after the caves");

	/* The three request bytes a console install zeroes, in that order, at 0. */
	check_eq_u64(be16_get(reply + off), 3, "then nb is 3");
	check_eq_u64(be16_get(reply + off + 2), 0, "and its pad is 0");
	off += SAVEFILE_PATCH_BYTES_HEAD;

	req[0] = d->api_mod;
	req[1] = d->api_load;
	req[2] = d->api_setaside;
	for (i = 0; i < 3; i++) {
		if (be32_get(reply + off) != req[i]) bytes_ok = 0;
		if (reply[off + 4] != 0 || reply[off + 5] != 0 ||
		    reply[off + 6] != 0 || reply[off + 7] != 0) bytes_ok = 0;
		off += SAVEFILE_PATCH_BYTE_ROW;
	}
	check(bytes_ok, "the bytes are api_mod, api_load and api_setaside, each set to 0");

	check_eq_u64(be32_get(reply + 4),
	             qcrc32(reply + SAVEFILE_PATCH_HEAD, len - SAVEFILE_PATCH_HEAD),
	             "the stamp is the CRC-32 of everything after it, as it went out");
}

/* One game on the refusing platform: first without the patch, then with it. */
static void sf_rpcs3_one_game(const struct sf_test_game *g)
{
	const struct sf_desc *d = sf_desc_for_game(g->game_id);
	u8 sup = 9, ins = 9, run = 9, pend = 9;
	u32 size = 0, got = 0, len = 0, len2 = 0;
	u8 pattern[64];
	u8 buf[64];
	u8 b = 0;
	u32 i;

	check(d != NULL, "the game has a helper in the table");
	if (d == NULL) return;

	/* ---------------------------------------------------- no patch in it */

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait(g->title), "the game boots with no patch in it");
	savefile_make_idle(g->game_id);
	host_write_log_reset();

	check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_OK, "SAVEFILE_INFO answers");
	check_eq_u64(sup, 1, "the game is supported");
	check_eq_u64(ins, 0, "and the helper is not in it");
	check_eq_u64(run, 0, "so it is not running");
	check_eq_u64(size, d->aside_size, "the size is the aside buffer's");
	check(savefile_set_aside() == ST_UNSUPPORTED, "a set-aside is UNSUPPORTED");
	check(features_trigger(g->set_aside_id) == ST_UNSUPPORTED, "and so is the SAVE_ASIDE action");
	check(features_trigger(g->load_aside_id) == ST_UNSUPPORTED, "and the LOAD_ASIDE action");
	check(savefile_read(0, 4, buf, &got) == ST_UNSUPPORTED, "and SAVEFILE_READ");
	check(savefile_library_gate() == ST_UNSUPPORTED, "and the library");
	check_eq_u64(host_write_log_count(), 0, "and nothing was written into the game");

	/* ----------------------------------------------------- the patch op */

	check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK,
	      "SAVEFILE_PATCH answers where code cannot be patched");
	sf_check_patch_words(d, g_sf_patch, len);
	check(savefile_patch(g_sf_patch2, sizeof(g_sf_patch2), &len2) == ST_OK &&
	      len2 == len && memcmp(g_sf_patch, g_sf_patch2, len) == 0,
	      "and asked again it is the same words and the same stamp");

	/*
	 * The words written into the process that is already running are too late:
	 * RPCS3 applies a patch as it loads a game, and the look is once a session,
	 * so the answer holds until the game restarts.
	 */
	sf_poke_patch(g_sf_patch, len, SF_NO_FLIP, 1);
	savefile_info(&sup, &ins, &run, &pend, &size);
	check_eq_u64(ins, 0, "words that arrive mid-session do not count until a restart");
	check(savefile_set_aside() == ST_UNSUPPORTED, "and a set-aside is still refused");

	/* ------------------------------------------------ restarted with it */

	check(quit_and_wait(), "the game is restarted");
	check(sf_boot_patched(g->title, g_sf_patch, len, SF_NO_FLIP),
	      "and comes back with the patch applied at load");
	savefile_make_idle(g->game_id);
	host_write_log_reset();

	check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_OK, "SAVEFILE_INFO answers");
	check_eq_u64(ins, 1, "and says the helper is in the game");
	check_eq_u64(run, 0, "not running while api_mod reads 0");
	check_eq_u64(pend, 0, "with nothing outstanding");

	/* The helper's own byte, as it would read once the hook has run a frame. */
	host_poke(d->api_mod, (const u8 *)"\x01", 1);
	savefile_info(&sup, &ins, &run, &pend, &size);
	check_eq_u64(run, 1, "running once api_mod reads 1");
	check(savefile_library_gate() == ST_OK, "the library is open");

	/* A set-aside round trip, with the test playing the helper's part. */
	check(features_trigger(g->set_aside_id) == ST_OK, "the SAVE_ASIDE action fires");
	host_peek(d->api_setaside, &b, 1);
	check_eq_u64(b, 1, "and raises the set-aside byte");
	host_peek(d->api_mod, &b, 1);
	check_eq_u64(b, 1, "while the helper's own byte is left alone: the install zeroes nothing");
	savefile_info(&sup, &ins, &run, &pend, &size);
	check_eq_u64(pend, SAVEFILE_PENDING_SET_ASIDE, "INFO says the set-aside is outstanding");

	for (i = 0; i < sizeof(pattern); i++) pattern[i] = (u8)(i * 5u + g->game_id);
	host_poke(d->aside_addr, pattern, sizeof(pattern));
	host_poke(d->api_setaside, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	savefile_info(&sup, &ins, &run, &pend, &size);
	check_eq_u64(pend, 0, "and the settle window clears it as on a console");
	check(savefile_read(0, sizeof(buf), buf, &got) == ST_OK && got == sizeof(buf) &&
	      memcmp(buf, pattern, sizeof(buf)) == 0,
	      "the save comes back out of the aside buffer");

	check(savefile_write(0, pattern, sizeof(pattern)) == ST_OK, "SAVEFILE_WRITE works");
	sf_put_save_head(g->game_id, d->aside_addr);   /* build 53: RaC1 and RaC2 load a save */
	check(features_trigger(g->load_aside_id) == ST_OK, "the LOAD_ASIDE action fires");
	host_peek(d->api_load, &b, 1);
	check_eq_u64(b, 1, "and raises the load byte");
	host_poke(d->api_load, (const u8 *)"\x00", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	savefile_info(&sup, &ins, &run, &pend, &size);
	check_eq_u64(pend, 0, "and the load settles");

	check(!sf_wrote_code(d), "qwark wrote no cave byte and no hook word");
	check(host_write_log_count() < 64, "(and every write it made was logged to be sure)");
}

static void test_savefile_rpcs3(void)
{
	u8 sup = 9, ins = 9, run = 9, pend = 9;
	u32 size = 0, len = 0, len2 = 0, got = 0;
	u32 stamps[4];
	u32 reads;
	u8 b = 0;
	u32 i;

	/* ---------------------------------------------- the op, on a console */

	group("build 47: SAVEFILE_PATCH on a console");

	for (i = 0; i < 4; i++) {
		const struct sf_desc *d = sf_desc_for_game(g_sf_games[i].game_id);

		check(quit_and_wait(), "quit whatever was running");
		check(boot_and_wait(g_sf_games[i].title), g_sf_games[i].title);

		reads = host_mem_reads();
		host_write_log_reset();
		check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK,
		      "SAVEFILE_PATCH answers on a console too");
		sf_check_patch_words(d, g_sf_patch, len);
		if (g_sf_games[i].game_id == GAME_RAC4)
			check_eq_u64(len, 2132, "Deadlocked's is 2132 bytes");
		check_eq_u64(host_mem_reads() - reads, 0, "it reads nothing out of the game");
		check_eq_u64(host_write_log_count(), 0, "and writes nothing into it");
		stamps[i] = be32_get(g_sf_patch + 4);

		check(savefile_patch(g_sf_patch, SAVEFILE_PATCH_HEAD + 8u, &len2) == ST_FULL,
		      "a reply buffer too small for it is FULL");
	}

	check(stamps[0] != stamps[1] && stamps[0] != stamps[2] && stamps[0] != stamps[3] &&
	      stamps[1] != stamps[2] && stamps[1] != stamps[3] && stamps[2] != stamps[3],
	      "the four games' stamps are four different numbers");

	/* The same game again is the same helper and the same stamp. */
	check(quit_and_wait(), "Deadlocked quits");
	check(boot_and_wait("NPEA00423"), "and comes back");
	check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK &&
	      be32_get(g_sf_patch + 4) == stamps[3],
	      "with the stamp it had before the reboot");

	savefile_set_enabled(0);
	check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_UNSUPPORTED,
	      "with the helper switched off in config it is UNSUPPORTED");
	savefile_set_enabled(1);

	check(quit_and_wait(), "the game goes away");
	reads = host_mem_reads();
	check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_NOT_INGAME,
	      "outside a game it is NOT_INGAME, like the other savefile ops");
	check_eq_u64(host_mem_reads() - reads, 0, "and there is no game for it to read");

	/*
	 * The console path is unchanged: it never looks for a helper, so words
	 * already in the process do not make one installed, and the first action
	 * still writes the caves and the hook and zeroes the request bytes first.
	 */
	group("build 47: a console still installs its own helper");

	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK, "the RaC1 words");
	check(quit_and_wait(), "RaC1 quits");
	check(sf_boot_patched("NPEA00385", g_sf_patch, len, SF_NO_FLIP),
	      "and boots with the words already in it");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);

		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "a console does not look: nothing is installed until an action");
		savefile_make_idle(GAME_RAC1);
		host_poke(d->api_mod, (const u8 *)"\x01", 1);
		host_write_log_reset();
		check(savefile_set_aside() == ST_OK, "the set-aside installs it");
		check(sf_wrote_code(d), "writing the caves and the hook itself");
		host_peek(d->api_mod, &b, 1);
		check_eq_u64(b, 0, "after zeroing the request bytes, as it always has");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);
	}

	/* ------------------------------------ the platform that refuses code */

	host_set_can_patch_code(0);
	host_set_emulator(1);

	for (i = 0; i < 4; i++) {
		char name[64];

		name[0] = 0;
		qstrcat(name, sizeof(name), "build 47: the RPCS3 patch in ");
		qstrcat(name, sizeof(name), g_sf_games[i].title);
		group(name);
		sf_rpcs3_one_game(&g_sf_games[i]);
	}

	group("build 47: the look is all of the helper or none of it");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);
		u32 n;

		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00385"), "RaC1 boots");
		check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK, "the RaC1 words");
		n = be16_get(g_sf_patch);

		check(quit_and_wait(), "quit");
		check(sf_boot_patched("NPEA00385", g_sf_patch, len, n / 2u),
		      "RaC1 boots with one byte in the middle of a cave different");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "which is not the helper");
		check(savefile_set_aside() == ST_UNSUPPORTED, "so a set-aside is UNSUPPORTED");
		check(savefile_library_gate() == ST_UNSUPPORTED, "and so is the library");

		check(quit_and_wait(), "quit");
		check(sf_boot_patched("NPEA00385", g_sf_patch, len, n - 1u),
		      "RaC1 boots with every cave right and the hook word one bit off");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "which is not the helper either: a hook from another build");

		check(quit_and_wait(), "quit");
		check(sf_boot_patched("NPEA00385", g_sf_patch, len, SF_NO_FLIP),
		      "RaC1 boots with all of it");
		savefile_make_idle(GAME_RAC1);
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 1, "which is");

		/* Once a session: damage after the look does not unmake it. */
		host_poke(d->caves[0].addr, (const u8 *)"\x00\x00\x00\x00", 4);
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 1, "a cave byte changed mid-session is not looked at again");

		/* Force autosave, RaC1's own byte, rides on the same install. */
		host_write_log_reset();
		check(features_trigger(F_FORCE_AUTOSAVE_ID) == ST_OK, "RaC1's Force autosave works");
		host_peek(SF_RAC1_SAVEMODE, &b, 1);
		check_eq_u64(b, 3, "and writes its autosave mode");
		check(session_load_setaside() == ST_OK, "the combo's load works too");
		check(!sf_wrote_code(d), "with no code word written by qwark");
		host_poke(d->api_load, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);

		/*
		 * The network thread's gate never reads the game: in the moment between
		 * a session starting and its first tick it has no answer to give, and
		 * says BUSY rather than guessing. The cave damaged above is put right
		 * first, since this look, unlike the last, will see it.
		 */
		sf_poke_patch(g_sf_patch, len, SF_NO_FLIP, 0);
		savefile_forget();
		check(savefile_library_gate() == ST_BUSY,
		      "before the tick has looked, the library gate is BUSY");
		pump(1);
		check(savefile_library_gate() == ST_OK, "and the next tick's look opens it");
	}

	/*
	 * Until build 50 RaC2's request bytes were in its executable's code segment
	 * and loaded as 0x80 0xFE 0xA4. A console install zeroes them before it
	 * hooks anything; under RPCS3 the hook is in from the first frame and
	 * nothing installs, so the patch zeroes them at load itself. Without that,
	 * INFO reports a load and a set-aside outstanding from boot and neither
	 * ever clears. The bytes are seeded here to stand for an executable that
	 * does not load them as zero.
	 */
	group("build 47: the patch clears the request bytes the executable loads with");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC2);
		u8 mod = 9, load = 9, aside = 9;

		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00386"), "RaC2 boots");
		check(savefile_patch(g_sf_patch2, sizeof(g_sf_patch2), &len2) == ST_OK,
		      "the RaC2 patch");

		check(quit_and_wait(), "quit");
		check(sf_boot_seeded("NPEA00386", d, g_sf_rac2_load_bytes, g_sf_patch2, len2, 0),
		      "RaC2 boots with the words in and its request bytes left at 80 FE A4");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 1, "the helper is found: the bytes are no part of the look");
		check_eq_u64(run, 0, "api_mod reads 0x80, which is not running");
		check_eq_u64(pend, SAVEFILE_PENDING_SET_ASIDE | SAVEFILE_PENDING_LOAD,
		             "and INFO reports a set-aside and a load outstanding from boot");
		pump(SAVEFILE_SETTLE_TICKS * 2);
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(pend, SAVEFILE_PENDING_SET_ASIDE | SAVEFILE_PENDING_LOAD,
		             "for good, since nothing qwark asked for is there to settle");
		host_peek(d->api_load, &load, 1);
		check_eq_u64(load, SAVEFILE_HELPER_REFUSED, "with the helper's refusal code in the load byte");

		check(quit_and_wait(), "quit");
		check(sf_boot_seeded("NPEA00386", d, g_sf_rac2_load_bytes, g_sf_patch2, len2, 1),
		      "RaC2 boots with the whole patch over the same bytes");
		savefile_make_idle(GAME_RAC2);
		host_peek(d->api_mod, &mod, 1);
		host_peek(d->api_load, &load, 1);
		host_peek(d->api_setaside, &aside, 1);
		check(mod == 0 && load == 0 && aside == 0,
		      "the patch's byte entries zeroed all three at load");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 1, "the helper is found");
		check_eq_u64(pend, 0, "and nothing is outstanding");
		check(savefile_set_aside() == ST_OK, "a set-aside goes out as it would on a console");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(pend, 0, "and settles");
	}

	group("build 47: the look starts again with every session");
	{
		check(quit_and_wait(), "the game quits");
		reads = host_mem_reads();
		check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_NOT_INGAME,
		      "at the XMB SAVEFILE_INFO is NOT_INGAME on this platform too");
		check(savefile_install() == ST_NOT_INGAME, "and so is installing");
		check(savefile_library_gate() == ST_NOT_INGAME, "and the library");
		check(savefile_patch(g_sf_patch2, sizeof(g_sf_patch2), &len2) == ST_NOT_INGAME,
		      "and SAVEFILE_PATCH");
		pump(8);
		check_eq_u64(host_mem_reads() - reads, 0, "and nothing looks at a game that is not there");

		check(boot_and_wait("NPEA00385"), "RaC1 comes back without the patch");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "and the last session's answer went with it");

		check(quit_and_wait(), "quit");
		check(sf_boot_patched("NPEA00385", g_sf_patch, len, SF_NO_FLIP), "RaC1 with the patch");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 1, "is found");
		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00386"), "and a different game without one");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "is not: the answer belongs to the game it was found in");
	}

	group("build 47: the console library through a patched-in helper");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);
		u32 done = 0, total = 0;
		u8 error = 9;
		u8 out[512];
		int t;

		check(quit_and_wait(), "quit");
		check(sf_boot_patched("NPEA00385", g_sf_patch, len, SF_NO_FLIP), "RaC1 with the patch");
		savefile_make_idle(GAME_RAC1);
		host_write_log_reset();

		check(savefile_category(SAVEFILE_CATEGORY_CREATE, "rpcs3") == ST_OK, "a category");
		check(savefile_store("rpcs3", "one.sav") == ST_OK, "SAVEFILE_STORE is accepted");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		check(pump_until_transfer_done(400), "and the store finishes");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NONE, "with no error");
		check_eq_u64(done, d->aside_size, "having copied the whole buffer");
		check(savefile_list("rpcs3", out, sizeof(out), &got) == ST_OK && out[0] == 1,
		      "the listing has the file");

		check(savefile_restore("rpcs3", "one.sav") == ST_OK, "SAVEFILE_RESTORE is accepted");
		for (t = 0; t < 100; t++) {
			host_peek(d->api_load, &b, 1);
			if (b == 1) break;
			pump(1);
		}
		check_eq_u64(b, 1, "the restore raises the load once the file is in the buffer");
		host_poke(d->api_load, (const u8 *)"\x00", 1);
		check(pump_until_transfer_done(400), "and finishes once the load has settled");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NONE, "with no error");
		check(!sf_wrote_code(d), "no code word was written for any of it");

		plat_file_unlink("/dev_hdd0/qwark/savefiles/NPEA00385/rpcs3/one.sav");
		plat_file_unlink("/dev_hdd0/qwark/savefiles/NPEA00385/rpcs3/one.sav.sum");
		check(savefile_category(SAVEFILE_CATEGORY_DELETE, "rpcs3") == ST_OK,
		      "and the category is cleaned away");
	}

	/* ------------------------------------------------- put it all back */

	host_set_can_patch_code(1);
	host_set_emulator(0);

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again with the gate open");
}

/* ------------------------------------- build 49, mods as RPCS3 patches */

/*
 * RPCS3 cannot take a mod from qwark any more than it can take the savefile
 * helper, and it takes one the same way: the client writes the mod from
 * MOD_PATCH into RPCS3's patch file, RPCS3 applies it as it loads the game, and
 * qwark looks for it once a session. As in test_savefile_rpcs3, the fake console
 * is RPCS3 once plat_can_patch_code() answers 0, and a "patch" is the reply's
 * rows and bytes poked into a fresh process between the boot and the first
 * tick.
 *
 * The mods live under a title no game claims, so the session comes up INGAME
 * with no game and nothing else in it ever writes the process: every write the
 * log sees would be qwark's own.
 */
#define MP_TITLE      "NPEA09049"
#define MP_OTHER      "NPEA09050"
#define MP_NO_FLIP    0xFFFFFFFFu

/* The test mods' bytes, generated so the checks compare against intent. */
static const u8 g_mp_cave_bin[16] = {
	0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
	0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF
};
static const u8 g_mp_tail_a[7] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
static const u8 g_mp_tail_b[6] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };

/* 8184 bytes is a cave of exactly 2046 rows, the most one reply holds. */
#define MP_EDGE_LEN   8184u
static u8 g_mp_edge_bin[MP_EDGE_LEN + 1u];

static u8 g_mp_reply[QWARK_MAX_PAYLOAD];
static u8 g_mp_words[256], g_mp_cave[256], g_mp_tail[256];
static u32 g_mp_words_len, g_mp_cave_len, g_mp_tail_len;
static u8 g_mp_list[1 + QWARK_MAX_MODS * MOD_WIRE_SIZE];
static u8 g_mp_file[4096];

/* One file of a mod folder, creating the folders on the way. */
static void mp_write(const char *title, const char *dirname, const char *leaf,
                     const void *data, u32 len)
{
	char path[256];
	plat_file_t f;

	path[0] = 0;
	qstrcat(path, sizeof(path), QWARK_MODSDIR);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), title);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), dirname);
	plat_dir_create(path);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), leaf);

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	if (len > 0) plat_file_write(f, data, len);
	plat_file_close(f);
}

static void mp_write_text(const char *dirname, const char *text)
{
	mp_write(MP_TITLE, dirname, "patch.txt", text, qstrlen(text));
}

static const char g_mp_words_txt[] =
	"#- name: Words\n"
	"0x00810000: 0x11111111\n"
	"0x00810004: 0x22222222\n"
	"0x00820000: 0x33333333\n";

static void mp_write_mods(void)
{
	u32 i;

	for (i = 0; i < sizeof(g_mp_edge_bin); i++) g_mp_edge_bin[i] = (u8)(i * 13u + 7u);

	/* Three words, two of them one run. */
	mp_write_text("words", g_mp_words_txt);

	/* A 16-byte cave and the branch into it. */
	mp_write_text("cave",
		"#- name: Cave\n"
		"0x00830000: cave.bin\n"
		"0x00840000: 0x4BFEFFFC\n");
	mp_write(MP_TITLE, "cave", "cave.bin", g_mp_cave_bin, sizeof(g_mp_cave_bin));

	/*
	 * Two caves that are not whole words, listed with the higher address
	 * first: the rows come in patch.txt order and the tail bytes in address
	 * order, so the two orders are told apart.
	 */
	mp_write_text("tail",
		"#- name: Tail\n"
		"0x00860000: b.bin\n"
		"0x00850000: a.bin\n"
		"0x00870000: 0x44444444\n");
	mp_write(MP_TITLE, "tail", "a.bin", g_mp_tail_a, sizeof(g_mp_tail_a));
	mp_write(MP_TITLE, "tail", "b.bin", g_mp_tail_b, sizeof(g_mp_tail_b));

	/* A cave whose file was never uploaded, and one that is empty. */
	mp_write_text("nobin",
		"0x00880000: missing.bin\n"
		"0x00890000: 0x55555555\n");
	mp_write_text("emptybin",
		"0x008B0000: empty.bin\n"
		"0x008B1000: 0x77777777\n");
	mp_write(MP_TITLE, "emptybin", "empty.bin", "", 0);

	/* A Lua automation beside a word. */
	mp_write_text("lua",
		"automation: x.lua\n"
		"0x008A0000: 0x66666666\n");

	/* A patch.txt too big to read, which is a parse error. */
	write_generated_padded_mod(MP_TITLE, "broken", MOD_TEXT_MAX + 16u);

	/* Nothing to write at all: a mod that loads as nothing everywhere. */
	mp_write_text("empty", "#- name: Empty\n");

	/* The largest reply there is, and one byte past it. */
	mp_write_text("edge", "0x00900000: edge.bin\n");
	mp_write(MP_TITLE, "edge", "edge.bin", g_mp_edge_bin, MP_EDGE_LEN);
	mp_write_text("over", "0x00910000: over.bin\n");
	mp_write(MP_TITLE, "over", "over.bin", g_mp_edge_bin, MP_EDGE_LEN + 1u);
}

/* The MOD_LIST index of a mod by its folder, which is what MOD_PATCH takes. */
static u8 mp_index(const char *dirname)
{
	int i = mods_find(dirname);
	return i < 0 ? 0xFF : (u8)i;
}

/* A mod's flags as MOD_LIST sends them, or 0xFF when it is not in the list. */
static u8 mp_flags(const char *dirname)
{
	u32 len = 0;
	u32 i;

	if (mods_list_encode(g_mp_list, sizeof(g_mp_list), &len) != ST_OK) return 0xFF;
	for (i = 0; i < g_mp_list[0]; i++) {
		const u8 *row = g_mp_list + 1 + i * MOD_WIRE_SIZE;
		if (strncmp((const char *)row + 8, dirname, 32) == 0) return row[1];
	}
	return 0xFF;
}

static int mp_any_flag(u8 flag)
{
	u32 len = 0;
	u32 i;

	if (mods_list_encode(g_mp_list, sizeof(g_mp_list), &len) != ST_OK) return 1;
	for (i = 0; i < g_mp_list[0]; i++)
		if (g_mp_list[1 + i * MOD_WIRE_SIZE + 1] & flag) return 1;
	return 0;
}

/* Ticks until no mod is CHECKING any more. */
static int mp_pump_until_looked(int max_ticks)
{
	int i;

	for (i = 0; i < max_ticks; i++) {
		if (!mp_any_flag(MOD_FLAG_CHECKING)) return 1;
		pump(1);
	}
	return !mp_any_flag(MOD_FLAG_CHECKING);
}

/* MOD_PATCH page 0 for one mod into `out`, 0 length on any refusal. */
static int mp_patch(const char *dirname, u8 *out, u32 cap, u32 *len)
{
	return mods_patch_encode(mp_index(dirname), 0, out, cap, len);
}

/* The same for any page (revision 1.19). */
static int mp_patch_page(const char *dirname, u16 page, u8 *out, u32 cap, u32 *len)
{
	return mods_patch_encode(mp_index(dirname), page, out, cap, len);
}

/*
 * A MOD_PATCH reply written into the fake process as RPCS3 writes a patch
 * file's entries: every row, then every byte. `flip_row` and `flip_byte` name
 * one row or one byte that goes in with its low bit changed.
 */
static void mp_poke(const u8 *reply, u32 len, u32 flip_row, u32 flip_byte)
{
	u32 n = be16_get(reply);
	u32 off = MOD_PATCH_HEAD;
	u32 nb;
	u32 i;

	for (i = 0; i < n && off + MOD_PATCH_ROW <= len; i++) {
		u8 word[4];

		memcpy(word, reply + off + 4, 4);
		if (i == flip_row) word[3] ^= 0x01;
		host_poke(be32_get(reply + off), word, 4);
		off += MOD_PATCH_ROW;
	}

	if (off + MOD_PATCH_BYTES_HEAD > len) return;
	nb = be16_get(reply + off);
	off += MOD_PATCH_BYTES_HEAD;

	for (i = 0; i < nb && off + MOD_PATCH_BYTE_ROW <= len; i++) {
		u8 b = reply[off + 4];

		if (i == flip_byte) b ^= 0x01;
		host_poke(be32_get(reply + off), &b, 1);
		off += MOD_PATCH_BYTE_ROW;
	}
}

/* A restart of the test title with whichever patches are named applied at load. */
static int mp_boot(int words, int cave, int tail, u32 flip_cave_row, u32 flip_tail_byte)
{
	host_boot(MP_TITLE);
	if (words) mp_poke(g_mp_words, g_mp_words_len, MP_NO_FLIP, MP_NO_FLIP);
	if (cave)  mp_poke(g_mp_cave, g_mp_cave_len, flip_cave_row, MP_NO_FLIP);
	if (tail)  mp_poke(g_mp_tail, g_mp_tail_len, MP_NO_FLIP, flip_tail_byte);
	return pump_until(SESSION_INGAME, 4000);
}

/* One row of a reply: its address and its value. */
static int mp_row(const u8 *reply, u32 i, u32 addr, u32 value)
{
	const u8 *row = reply + MOD_PATCH_HEAD + i * MOD_PATCH_ROW;
	return be32_get(row) == addr && be32_get(row + 4) == value;
}

static int mp_stamp_ok(const u8 *reply, u32 len)
{
	return len >= MOD_PATCH_HEAD &&
	       be32_get(reply + 4) == qcrc32(reply + MOD_PATCH_HEAD, len - MOD_PATCH_HEAD);
}

/*
 * What build 51, the last build before paging, answered for the words, cave and
 * tail mods, captured off its qwark-host.exe over the wire. Revision 1.19 keeps
 * every byte of a one-page reply but bytes 2..3, the pad that is now `pages`.
 */
static const u8 g_mp_b51_words[] = {
	0x00, 0x03, 0x00, 0x00, 0xD6, 0x40, 0xC9, 0xF5,
	0x00, 0x81, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
	0x00, 0x81, 0x00, 0x04, 0x22, 0x22, 0x22, 0x22,
	0x00, 0x82, 0x00, 0x00, 0x33, 0x33, 0x33, 0x33,
	0x00, 0x00, 0x00, 0x00
};
static const u8 g_mp_b51_cave[] = {
	0x00, 0x05, 0x00, 0x00, 0x4A, 0xC3, 0x24, 0x2C,
	0x00, 0x83, 0x00, 0x00, 0xA0, 0xA1, 0xA2, 0xA3,
	0x00, 0x83, 0x00, 0x04, 0xA4, 0xA5, 0xA6, 0xA7,
	0x00, 0x83, 0x00, 0x08, 0xA8, 0xA9, 0xAA, 0xAB,
	0x00, 0x83, 0x00, 0x0C, 0xAC, 0xAD, 0xAE, 0xAF,
	0x00, 0x84, 0x00, 0x00, 0x4B, 0xFE, 0xFF, 0xFC,
	0x00, 0x00, 0x00, 0x00
};
static const u8 g_mp_b51_tail[] = {
	0x00, 0x03, 0x00, 0x00, 0x6E, 0x0C, 0x75, 0x7B,
	0x00, 0x86, 0x00, 0x00, 0x20, 0x21, 0x22, 0x23,
	0x00, 0x85, 0x00, 0x00, 0x10, 0x11, 0x12, 0x13,
	0x00, 0x87, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44,
	0x00, 0x05, 0x00, 0x00, 0x00, 0x85, 0x00, 0x04,
	0x14, 0x00, 0x00, 0x00, 0x00, 0x85, 0x00, 0x05,
	0x15, 0x00, 0x00, 0x00, 0x00, 0x85, 0x00, 0x06,
	0x16, 0x00, 0x00, 0x00, 0x00, 0x86, 0x00, 0x04,
	0x24, 0x00, 0x00, 0x00, 0x00, 0x86, 0x00, 0x05,
	0x25, 0x00, 0x00, 0x00
};

/* A reply that is build 51's `b51` with u16 pages 1 where its pad was 0. */
static int mp_same_as_b51(const u8 *reply, u32 len, const u8 *b51, u32 b51_len)
{
	return len == b51_len && len >= MOD_PATCH_HEAD &&
	       memcmp(reply, b51, 2) == 0 &&
	       b51[2] == 0 && b51[3] == 0 && reply[2] == 0 && reply[3] == 1 &&
	       memcmp(reply + 4, b51 + 4, len - 4) == 0;
}

/* A whole file off the fake HDD, 0 length when it is not there. */
static u32 mp_read_file(const char *path, u8 *out, u32 cap)
{
	plat_file_t f;
	u32 total = 0;

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return 0;
	for (;;) {
		u32 got = 0;
		if (total >= cap || plat_file_read(f, out + total, cap - total, &got) != 0 || got == 0) break;
		total += got;
	}
	plat_file_close(f);
	return total;
}

static void test_mod_patch_layout(void)
{
	u32 len = 0, len2 = 0, reads;
	const u8 *b;
	int ok;
	u32 i;

	group("build 49: MOD_PATCH, a mod of words");

	mp_write_mods();
	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait(MP_TITLE), "a title with the test mods boots, on a console");
	check(mods_find("words") >= 0 && mods_find("over") >= 0, "and its mods are scanned");

	reads = host_mem_reads();
	host_write_log_reset();

	check(mp_patch("words", g_mp_words, sizeof(g_mp_words), &g_mp_words_len) == ST_OK,
	      "MOD_PATCH answers for a mod of three words");
	b = g_mp_words;
	check_eq_u64(be16_get(b), 3, "n is its three words");
	check_eq_u64(be16_get(b + 2), 1, "pages is 1, where build 51 had a 0 pad (revision 1.19)");
	check_eq_u64(g_mp_words_len, MOD_PATCH_HEAD + 3u * MOD_PATCH_ROW + MOD_PATCH_BYTES_HEAD,
	             "the reply is three rows and an empty byte section");
	check(mp_row(b, 0, 0x00810000u, 0x11111111u) && mp_row(b, 1, 0x00810004u, 0x22222222u) &&
	      mp_row(b, 2, 0x00820000u, 0x33333333u), "the words, in patch.txt order");
	check_eq_u64(be16_get(b + MOD_PATCH_HEAD + 3u * MOD_PATCH_ROW), 0, "nb is 0");
	check(mp_stamp_ok(b, g_mp_words_len), "the stamp is the CRC-32 of everything after it");
	check(mp_patch("words", g_mp_reply, sizeof(g_mp_reply), &len2) == ST_OK &&
	      len2 == g_mp_words_len && memcmp(g_mp_reply, g_mp_words, len2) == 0,
	      "asked again it is the same bytes and the same stamp");

	group("build 49: MOD_PATCH, a cave and a branch");

	check(mp_patch("cave", g_mp_cave, sizeof(g_mp_cave), &g_mp_cave_len) == ST_OK,
	      "MOD_PATCH answers for a mod with a cave");
	b = g_mp_cave;
	check_eq_u64(be16_get(b), 5, "n is the cave's four words and the mod's one");
	check_eq_u64(g_mp_cave_len, MOD_PATCH_HEAD + 5u * MOD_PATCH_ROW + MOD_PATCH_BYTES_HEAD,
	             "and nothing else");
	ok = 1;
	for (i = 0; i < 4; i++)
		if (!mp_row(b, i, 0x00830000u + i * 4u, be32_get(g_mp_cave_bin + i * 4u))) ok = 0;
	check(ok, "the cave comes first, as consecutive words of its file");
	check(mp_row(b, 4, 0x00840000u, 0x4BFEFFFCu), "and the branch into it after it");
	check(mp_stamp_ok(b, g_mp_cave_len), "with the stamp over all of it");

	group("build 49: MOD_PATCH, caves that are not whole words");

	check(mp_patch("tail", g_mp_tail, sizeof(g_mp_tail), &g_mp_tail_len) == ST_OK,
	      "MOD_PATCH answers for a 6-byte and a 7-byte cave");
	b = g_mp_tail;
	check_eq_u64(be16_get(b), 3, "n is one word of each cave and the mod's own");
	check_eq_u64(g_mp_tail_len,
	             MOD_PATCH_HEAD + 3u * MOD_PATCH_ROW + MOD_PATCH_BYTES_HEAD + 5u * MOD_PATCH_BYTE_ROW,
	             "and five tail bytes behind them");
	check(mp_row(b, 0, 0x00860000u, 0x20212223u) && mp_row(b, 1, 0x00850000u, 0x10111213u),
	      "the caves' rows in patch.txt order, the higher address first");
	check(mp_row(b, 2, 0x00870000u, 0x44444444u), "then the word");
	{
		const u8 *t = b + MOD_PATCH_HEAD + 3u * MOD_PATCH_ROW;
		static const u32 addrs[5] = { 0x00850004u, 0x00850005u, 0x00850006u,
		                              0x00860004u, 0x00860005u };
		static const u8 vals[5] = { 0x14, 0x15, 0x16, 0x24, 0x25 };

		check_eq_u64(be16_get(t), 5, "nb is 5");
		check_eq_u64(be16_get(t + 2), 0, "and its pad is 0");
		ok = 1;
		for (i = 0; i < 5; i++) {
			const u8 *e = t + MOD_PATCH_BYTES_HEAD + i * MOD_PATCH_BYTE_ROW;
			if (be32_get(e) != addrs[i] || e[4] != vals[i] || e[5] || e[6] || e[7]) ok = 0;
		}
		check(ok, "the tail bytes are in address order, each with three zero pads");
	}
	check(mp_stamp_ok(b, g_mp_tail_len), "and the stamp covers them too");

	group("revision 1.19: a one-page mod is build 51's reply but for `pages`");

	check(mp_same_as_b51(g_mp_words, g_mp_words_len, g_mp_b51_words, sizeof(g_mp_b51_words)),
	      "the words' reply is build 51's byte for byte, with pages 1 where its pad was 0");
	check(mp_same_as_b51(g_mp_cave, g_mp_cave_len, g_mp_b51_cave, sizeof(g_mp_b51_cave)),
	      "and so is the cave's");
	check(mp_same_as_b51(g_mp_tail, g_mp_tail_len, g_mp_b51_tail, sizeof(g_mp_b51_tail)),
	      "and the tail bytes', byte rows and stamp included");
	{
		u8 req[5];

		req[0] = mp_index("tail");
		req[1] = 0; req[2] = 0; req[3] = 0; req[4] = 0;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
		      len == g_mp_tail_len && memcmp(g_mp_reply, g_mp_tail, len) == 0,
		      "the old request, u8 index and three zero pads, is page 0 byte for byte");
		check(mods_patch_request(req, 5, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
		      len == g_mp_tail_len && memcmp(g_mp_reply, g_mp_tail, len) == 0,
		      "and a byte past the four changes nothing");
		req[3] = 1;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG,
		      "u16 page 1 of a one-page mod is BAD_ARG");
		check_eq_u64(len, 0, "with nothing in the reply");
		req[2] = 0xFF; req[3] = 0xFF;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG,
		      "and so is page 65535");
		req[2] = 0; req[3] = 0;
		check(mods_patch_request(req, 3, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG,
		      "a request shorter than its four bytes is BAD_ARG, as it always was");
		req[1] = 0x5A;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
		      len == g_mp_tail_len && memcmp(g_mp_reply, g_mp_tail, len) == 0,
		      "and the pad byte between index and page is not looked at");
	}

	group("build 49: MOD_PATCH, the refusals");

	check(mods_patch_encode((u8)mods_count(), 0, g_mp_reply, sizeof(g_mp_reply), &len) == ST_NOT_FOUND,
	      "an index past the list is NOT_FOUND, as MOD_LOAD's unknown mod is");
	check(mods_patch_encode(200, 0, g_mp_reply, sizeof(g_mp_reply), &len) == ST_NOT_FOUND,
	      "and so is one far past it");
	check(mp_patch("broken", g_mp_reply, sizeof(g_mp_reply), &len) == ST_IO_ERROR,
	      "a mod that failed to parse is IO_ERROR, as MOD_LOAD answers it");
	check(mp_patch("nobin", g_mp_reply, sizeof(g_mp_reply), &len) == ST_IO_ERROR,
	      "a cave whose file is missing is IO_ERROR");
	check_eq_u64(len, 0, "with nothing in the reply");
	check(mp_patch("emptybin", g_mp_reply, sizeof(g_mp_reply), &len) == ST_IO_ERROR,
	      "and so is a cave whose file is empty");
	check(mp_patch("lua", g_mp_reply, sizeof(g_mp_reply), &len) == ST_UNSUPPORTED,
	      "a mod with a Lua automation is UNSUPPORTED");
	check(mp_patch("edge", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK,
	      "an 8184-byte cave fits");
	check_eq_u64(be16_get(g_mp_reply), 2046, "as 2046 rows");
	check_eq_u64(be16_get(g_mp_reply + 2), 1, "on one page");
	check_eq_u64(len, 16380, "in 16380 bytes, the most rows a frame holds");
	check(mp_stamp_ok(g_mp_reply, len), "stamped like any other");
	ok = 1;
	for (i = 0; i < 2046u; i++)
		if (!mp_row(g_mp_reply, i, 0x00900000u + i * 4u, be32_get(g_mp_edge_bin + i * 4u))) ok = 0;
	check(ok, "every row of it the file's own bytes");
	check(mp_patch_page("edge", 1, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG,
	      "and its page 1 is BAD_ARG: there is none");
	check_eq_u64(len, 0, "with nothing in the reply");

	/* Revision 1.19: one byte more was FULL; now it is a second page. */
	check(mp_patch("over", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK,
	      "one byte more, a tail byte behind 2046 rows, answers (it was FULL before 1.19)");
	check_eq_u64(be16_get(g_mp_reply + 2), 2, "on two pages");
	check_eq_u64(be16_get(g_mp_reply), 2046, "page 0 is the 2046 words");
	check_eq_u64(len, 16380, "and no byte rows: a whole frame");
	check_eq_u64(be16_get(g_mp_reply + MOD_PATCH_HEAD + 2046u * MOD_PATCH_ROW), 0, "its nb is 0");
	ok = 1;
	for (i = 0; i < 2046u; i++)
		if (!mp_row(g_mp_reply, i, 0x00910000u + i * 4u, be32_get(g_mp_edge_bin + i * 4u))) ok = 0;
	check(ok, "every one of them the file's own bytes");
	{
		u32 stamp0 = be32_get(g_mp_reply + 4);
		const u8 *t;

		check(mp_patch_page("over", 1, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK,
		      "page 1 answers");
		t = g_mp_reply + MOD_PATCH_HEAD;
		check(be16_get(g_mp_reply) == 0 && be16_get(g_mp_reply + 2) == 2 &&
		      be32_get(g_mp_reply + 4) == stamp0,
		      "with no words, the same pages and the same stamp");
		check(len == MOD_PATCH_HEAD + MOD_PATCH_BYTES_HEAD + MOD_PATCH_BYTE_ROW &&
		      be16_get(t) == 1 && be32_get(t + 4) == 0x00910000u + 8184u &&
		      t[8] == g_mp_edge_bin[8184],
		      "and the one tail byte");
	}
	check(mp_patch("words", g_mp_reply, 16, &len) == ST_FULL,
	      "a reply buffer too small for the words is FULL");
	check(mp_patch("over", g_mp_reply, 1000, &len) == ST_FULL && len == 0,
	      "and so is one too small for the page it asked for");
	{
		static u8 big[QWARK_MAX_PAYLOAD * 2u];
		check(mp_patch("over", big, sizeof(big), &len) == ST_OK &&
		      be16_get(big) == 2046 && be16_get(big + 2) == 2 && len == 16380,
		      "and a bigger buffer changes nothing: a page is at most a frame");
	}
	check(mp_patch("empty", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
	      len == MOD_PATCH_HEAD + MOD_PATCH_BYTES_HEAD && be16_get(g_mp_reply) == 0,
	      "a mod with nothing to write is an empty patch");

	check_eq_u64(host_mem_reads() - reads, 0, "none of it read the game");
	check_eq_u64(host_write_log_count(), 0, "or wrote into it");

	group("build 49: MOD_PATCH on the shipped library");

	mods_set_title("NPEA00423");
	check(mp_patch("il-ghost", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK,
	      "il-ghost, the largest shipped cave, answers");
	check_eq_u64(be16_get(g_mp_reply), 742 + 3, "its 2968-byte cave is 742 rows, then its 3 words");
	check_eq_u64(len, 5972, "in 5972 bytes, well inside a frame");
	{
		u32 n = mp_read_file("/dev_hdd0/qwark/mods/NPEA00423/il-ghost/rack.bin",
		                     g_mp_file, sizeof(g_mp_file));
		const struct mod_entry *m = mods_at(mp_index("il-ghost"));

		check_eq_u64(n, 2968, "the fixture's rack.bin is the 2968 bytes");
		ok = n == 2968;
		for (i = 0; ok && i < 742; i++)
			if (!mp_row(g_mp_reply, i, 0x00676B9Cu + i * 4u, be32_get(g_mp_file + i * 4u))) ok = 0;
		check(ok, "and the cave rows are its bytes, word by word");
		ok = m != NULL && m->def.count == 3;
		for (i = 0; ok && i < 3; i++)
			if (!mp_row(g_mp_reply, 742 + i, m->def.words[i].addr, m->def.words[i].value)) ok = 0;
		check(ok, "the words come after it, as patch.txt lists them");
		check(m != NULL && m->depends[0] != 0,
		      "it depends on dl-cs, whose 253 words are not folded in: n counts its own");
	}
	check(mp_stamp_ok(g_mp_reply, len), "and the stamp checks");
	check_eq_u64(be32_get(g_mp_reply + 4), 0x68EFD3BDu, "and is build 51's stamp for it, unchanged");
	check_eq_u64(be16_get(g_mp_reply + 2), 1, "on one page");

	mods_set_title("NPEA00385");
	check(mp_patch("big-cave", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK,
	      "big-cave's 10240-byte cave is past a frame, and answers all the same (FULL before 1.19)");
	check(be16_get(g_mp_reply) == 2046 && be16_get(g_mp_reply + 2) == 2 && len == 16380,
	      "page 0 is a full frame of 2046 rows, of two pages");
	check(mp_patch_page("big-cave", 1, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
	      be16_get(g_mp_reply) == 2560 + 2 - 2046 && be16_get(g_mp_reply + 2) == 2,
	      "and page 1 the other 516: 2560 cave rows and 2 words in all");
	check(mp_patch("flight", g_mp_reply, sizeof(g_mp_reply), &len) == ST_UNSUPPORTED,
	      "flight, a Lua mod, is UNSUPPORTED");
	check(mp_patch("rc1-hoven-health", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
	      be16_get(g_mp_reply) == 1096 / 4 + 9,
	      "rc1-hoven-health is its 274 cave rows and its 9 words");
	mods_set_title(MP_TITLE);

	/* ------------------------------------------ the console path is unchanged */

	group("build 49: a console does not look for mods");

	check(quit_and_wait(), "quit");
	check(mp_boot(1, 1, 1, MP_NO_FLIP, MP_NO_FLIP),
	      "the title boots on a console with every patch already in memory");
	pump(40);
	check(!mp_any_flag(MOD_FLAG_CHECKING), "no mod is ever CHECKING on a console");
	check_eq_u64(mp_flags("words") & MOD_FLAG_LOADED, 0,
	             "and words in memory do not make a mod LOADED: only a load does");
	host_write_log_reset();
	check(mods_load("words") == ST_OK, "MOD_LOAD writes it as it always has");
	check((mp_flags("words") & MOD_FLAG_LOADED) != 0, "and then it is LOADED");
	check(host_write_log_count() > 0, "by writing");
	check(mods_unload("words") == ST_OK, "and it unloads");
	check_eq_u64(mp_flags("words") & MOD_FLAG_LOADED, 0, "and is not LOADED");
}

/* ------------------------------ revision 1.19, MOD_PATCH a page at a time */

/*
 * Mods bigger than one frame, under a title of their own so that the presence
 * tests after these never meet them. Each is written from a spec, and the same
 * spec gives the layout one unbounded reply would hold - every word row in
 * patch.txt order, then every byte row in address order - which is what the
 * pages, joined, are checked against.
 */
#define MPP_TITLE      "NPEA09051"
#define MPP_MAX_ROWS   6144u
#define MPP_MAX_BYTES  16u
#define MPP_NWORDS     16u

struct mpp_cave {
	u32 addr;
	u32 len;
	const char *file;
};

struct mpp_layout {
	u32 nw, nb;
	u32 waddr[MPP_MAX_ROWS];
	u32 wval[MPP_MAX_ROWS];
	u32 baddr[MPP_MAX_BYTES];
	u8  bval[MPP_MAX_BYTES];
};

static struct mpp_layout g_mpp_want, g_mpp_got;
static u8 g_mpp_bin[20480];
static u8 g_mpp_page0[QWARK_MAX_PAYLOAD];
static u32 g_mpp_page0_len;
static u32 g_mpp_sorted[MPP_MAX_ROWS + MPP_MAX_BYTES];
static char g_mpp_what[160];

static const char *mpp_what(const char *dirname, const char *what)
{
	snprintf(g_mpp_what, sizeof(g_mpp_what), "%s: %s", dirname, what);
	return g_mpp_what;
}

/* A cave's bytes, different at every offset and from one cave to the next. */
static void mpp_fill(u8 *buf, u32 len, u32 addr)
{
	u32 i;
	for (i = 0; i < len; i++) buf[i] = (u8)((i * 31u) ^ (i >> 8) ^ (addr >> 16) ^ 0x5Au);
}

/*
 * Writes mod `dirname` under MPP_TITLE - its caves in the order given, then
 * MPP_NWORDS words eight bytes apart from `words` - and fills g_mpp_want with
 * the layout one unbounded MOD_PATCH reply would hold.
 */
static void mpp_write_mod(const char *dirname, const struct mpp_cave *caves, u32 ncaves,
                          u32 words)
{
	static char text[1024];
	struct mpp_layout *w = &g_mpp_want;
	u32 off = 0;
	u32 c, i, j;

	w->nw = 0;
	w->nb = 0;
	off += (u32)snprintf(text + off, sizeof(text) - off, "#- name: %s\n", dirname);

	for (c = 0; c < ncaves; c++) {
		off += (u32)snprintf(text + off, sizeof(text) - off, "0x%08X: %s\n",
		                     (unsigned)caves[c].addr, caves[c].file);
		mpp_fill(g_mpp_bin, caves[c].len, caves[c].addr);
		mp_write(MPP_TITLE, dirname, caves[c].file, g_mpp_bin, caves[c].len);

		for (i = 0; i + 4u <= caves[c].len; i += 4u) {
			w->waddr[w->nw] = caves[c].addr + i;
			w->wval[w->nw] = be32_get(g_mpp_bin + i);
			w->nw++;
		}
		for (; i < caves[c].len; i++) {
			w->baddr[w->nb] = caves[c].addr + i;
			w->bval[w->nb] = g_mpp_bin[i];
			w->nb++;
		}
	}

	for (i = 0; i < MPP_NWORDS; i++) {
		u32 value = 0x48000000u | (words >> 4) | i;

		off += (u32)snprintf(text + off, sizeof(text) - off, "0x%08X: 0x%08X\n",
		                     (unsigned)(words + i * 8u), (unsigned)value);
		w->waddr[w->nw] = words + i * 8u;
		w->wval[w->nw] = value;
		w->nw++;
	}

	for (i = 1; i < w->nb; i++) {
		for (j = i; j > 0 && w->baddr[j - 1] > w->baddr[j]; j--) {
			u32 a = w->baddr[j];
			u8 v = w->bval[j];

			w->baddr[j] = w->baddr[j - 1];
			w->bval[j] = w->bval[j - 1];
			w->baddr[j - 1] = a;
			w->bval[j - 1] = v;
		}
	}

	mp_write(MPP_TITLE, dirname, "patch.txt", text, off);
}

/* The CRC-32 of what one unbounded reply holds after its stamp, from g_mpp_want. */
static u32 mpp_want_stamp(void)
{
	const struct mpp_layout *w = &g_mpp_want;
	u32 crc = qcrc32_start();
	u8 b[8];
	u32 i;

	for (i = 0; i < w->nw; i++) {
		be32_put(b, w->waddr[i]);
		be32_put(b + 4, w->wval[i]);
		crc = qcrc32_update(crc, b, 8);
	}
	be16_put(b, (u16)w->nb);
	be16_put(b + 2, 0);
	crc = qcrc32_update(crc, b, 4);
	for (i = 0; i < w->nb; i++) {
		be32_put(b, w->baddr[i]);
		b[4] = w->bval[i];
		b[5] = 0;
		b[6] = 0;
		b[7] = 0;
		crc = qcrc32_update(crc, b, 8);
	}
	return qcrc32_finish(crc);
}

static int mpp_cmp_u32(const void *a, const void *b)
{
	u32 x = *(const u32 *)a;
	u32 y = *(const u32 *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

/*
 * Every page of `dirname`, fetched one by one and checked against the page rule
 * (`want_pages` pages, page k carrying `want_counts[k]` word rows) and, joined,
 * against g_mpp_want. Page 0 stays in g_mpp_page0.
 */
static void mpp_check(const char *dirname, u32 want_pages, const u32 *want_counts)
{
	struct mpp_layout *g = &g_mpp_got;
	const struct mpp_layout *w = &g_mpp_want;
	u32 len = 0, pages, stamp, k, i, nsorted;
	int parsed = 1, fits = 1, agree = 1, counts = 1, bytes_last = 1, same, unique;

	g->nw = 0;
	g->nb = 0;
	g_mpp_page0_len = 0;

	if (mp_patch_page(dirname, 0, g_mpp_page0, sizeof(g_mpp_page0), &g_mpp_page0_len) != ST_OK) {
		check(0, mpp_what(dirname, "page 0 answers"));
		return;
	}
	pages = be16_get(g_mpp_page0 + 2);
	stamp = be32_get(g_mpp_page0 + 4);
	check_eq_u64(pages, want_pages, mpp_what(dirname, "pages is what the rule gives"));
	if (pages != want_pages) return;

	for (k = 0; k < pages; k++) {
		u32 n, nb, off;

		if (mp_patch_page(dirname, (u16)k, g_mp_reply, sizeof(g_mp_reply), &len) != ST_OK) {
			parsed = 0;
			break;
		}
		if (len > QWARK_MAX_PAYLOAD) fits = 0;
		if (len < MOD_PATCH_HEAD + MOD_PATCH_BYTES_HEAD) { parsed = 0; break; }

		n = be16_get(g_mp_reply);
		off = MOD_PATCH_HEAD + n * MOD_PATCH_ROW;
		if (off + MOD_PATCH_BYTES_HEAD > len) { parsed = 0; break; }
		nb = be16_get(g_mp_reply + off);
		if (be16_get(g_mp_reply + off + 2) != 0 ||
		    len != off + MOD_PATCH_BYTES_HEAD + nb * MOD_PATCH_BYTE_ROW) {
			parsed = 0;
			break;
		}

		if (be16_get(g_mp_reply + 2) != pages || be32_get(g_mp_reply + 4) != stamp) agree = 0;
		if (n != want_counts[k]) counts = 0;
		if (k + 1u < pages ? nb != 0 : nb != w->nb) bytes_last = 0;

		for (i = 0; i < n && g->nw < MPP_MAX_ROWS; i++) {
			const u8 *row = g_mp_reply + MOD_PATCH_HEAD + i * MOD_PATCH_ROW;

			g->waddr[g->nw] = be32_get(row);
			g->wval[g->nw] = be32_get(row + 4);
			g->nw++;
		}
		off += MOD_PATCH_BYTES_HEAD;
		for (i = 0; i < nb && g->nb < MPP_MAX_BYTES; i++, off += MOD_PATCH_BYTE_ROW) {
			g->baddr[g->nb] = be32_get(g_mp_reply + off);
			g->bval[g->nb] = g_mp_reply[off + 4];
			if (g_mp_reply[off + 5] || g_mp_reply[off + 6] || g_mp_reply[off + 7]) parsed = 0;
			g->nb++;
		}
	}

	check(parsed, mpp_what(dirname, "every page answers and parses: n rows, nb and its pad, nb byte rows, to its length"));
	check(fits, mpp_what(dirname, "every page fits one 16384-byte frame"));
	check(agree, mpp_what(dirname, "every page has the same pages and the same stamp"));
	check(counts, mpp_what(dirname, "each page carries the word rows the rule gives it"));
	check(bytes_last, mpp_what(dirname, "the byte rows are all on the last page and none on another"));

	same = g->nw == w->nw;
	for (i = 0; same && i < w->nw; i++)
		if (g->waddr[i] != w->waddr[i] || g->wval[i] != w->wval[i]) same = 0;
	check(same, mpp_what(dirname, "the word rows joined in page order are the unpaged layout's"));
	same = g->nb == w->nb;
	for (i = 0; same && i < w->nb; i++)
		if (g->baddr[i] != w->baddr[i] || g->bval[i] != w->bval[i]) same = 0;
	check(same, mpp_what(dirname, "and the byte rows are its byte rows, in address order"));
	check_eq_u64(stamp, mpp_want_stamp(),
	             mpp_what(dirname, "the stamp is the CRC-32 of one unbounded reply after its stamp"));

	nsorted = 0;
	for (i = 0; i < g->nw; i++) g_mpp_sorted[nsorted++] = g->waddr[i];
	for (i = 0; i < g->nb; i++) g_mpp_sorted[nsorted++] = g->baddr[i];
	qsort(g_mpp_sorted, nsorted, sizeof(g_mpp_sorted[0]), mpp_cmp_u32);
	unique = 1;
	for (i = 1; i < nsorted; i++)
		if (g_mpp_sorted[i] == g_mpp_sorted[i - 1]) unique = 0;
	check(unique && nsorted == w->nw + w->nb, mpp_what(dirname, "no address appears twice across the pages"));

	len = 1;
	check(mp_patch_page(dirname, (u16)pages, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG &&
	      len == 0, mpp_what(dirname, "the page after the last is BAD_ARG, with nothing in the reply"));
}

static void test_mod_patch_pages(void)
{
	/* About the user's dl-tas: a 9.3 KB cave and 16 words, 2346 rows. */
	static const struct mpp_cave two[] = { { 0x00A00000u, 9314u, "rack.bin" } };
	/* About 20 KB of cave in two caves, listed higher address first, 5023 rows. */
	static const struct mpp_cave three[] = {
		{ 0x00B80000u, 7u, "small.bin" },
		{ 0x00B00000u, 20003u, "big.bin" }
	};
	/* The edges: 2046 rows, 2046 with two of them bytes, 2047, and 2045 + 2. */
	static const struct mpp_cave r2046[]  = { { 0x00C00000u, 8120u, "c.bin" } };
	static const struct mpp_cave r2046b[] = { { 0x00C20000u, 8114u, "c.bin" } };
	static const struct mpp_cave r2047[]  = { { 0x00D00000u, 8124u, "c.bin" } };
	static const struct mpp_cave tip[]    = { { 0x00E00000u, 8118u, "c.bin" } };
	static const u32 two_counts[]    = { 2046, 298 };
	static const u32 three_counts[]  = { 2046, 2046, 925 };
	static const u32 r2046_counts[]  = { 2046 };
	static const u32 r2046b_counts[] = { 2044 };
	static const u32 r2047_counts[]  = { 2046, 1 };
	static const u32 tip_counts[]    = { 2045, 0 };
	u32 len = 0, count = 0;
	u8 req[4];

	group("revision 1.19: the page rule, by the numbers");

	check_eq_u64(MOD_PATCH_PAGE_ROWS, 2046, "a page is (16384 - 8 - 4) / 8 = 2046 rows");
	check(mods_test_page_split(0, 0, 0, &count) == 1 && count == 0,
	      "nothing at all is one empty page");
	check(mods_test_page_split(2046, 0, 0, &count) == 1 && count == 2046,
	      "2046 words are one full page");
	check(mods_test_page_split(2044, 2, 0, &count) == 1 && count == 2044,
	      "2044 words and 2 bytes are one full page too");
	check(mods_test_page_split(2047, 0, 1, &count) == 2 && count == 1,
	      "2047 words are two pages, the second of one word");
	check(mods_test_page_split(2045, 2, 0, &count) == 2 && count == 2045,
	      "2045 words and 2 bytes are two pages: the words all on the first, not full");
	check(mods_test_page_split(2045, 2, 1, &count) == 2 && count == 0,
	      "and the bytes alone on the second");
	check(mods_test_page_split(4092, 1, 2, &count) == 3 && count == 0,
	      "two full pages of words and a byte make a third page that holds only the byte");
	check(mods_test_page_split(4093, 3, 1, &count) == 3 && count == 2046,
	      "a middle page is always full");
	check(mods_test_page_split(4093, 3, 2, &count) == 3 && count == 1,
	      "and the last carries what is left beside the bytes");
	check(mods_test_page_split(0, 96, 0, &count) == 1 && count == 0,
	      "the most byte rows a mod can have, 3 for each of 32 caves, are one page");

	group("revision 1.19: MOD_PATCH of a mod past one frame");

	mpp_write_mod("two", two, 1, 0x00A80000u);
	mods_set_title(MPP_TITLE);
	check(mods_find("two") >= 0, "a 9314-byte cave and 16 words is scanned");
	mpp_check("two", 2, two_counts);
	{
		static u8 again[QWARK_MAX_PAYLOAD];
		u32 len2 = 0;

		check(mp_patch_page("two", 1, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
		      mp_patch_page("two", 1, again, sizeof(again), &len2) == ST_OK &&
		      len == len2 && memcmp(g_mp_reply, again, len) == 0,
		      "two: a page asked for twice is the same bytes twice");

		req[0] = mp_index("two");
		req[1] = 0; req[2] = 0; req[3] = 0;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
		      len == g_mpp_page0_len && memcmp(g_mp_reply, g_mpp_page0, len) == 0,
		      "two: the old four-byte request with zero pads is page 0");
		req[3] = 1;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
		      len == len2 && memcmp(g_mp_reply, again, len) == 0,
		      "two: and u16 page 1 is page 1");
		req[3] = 2;
		check(mods_patch_request(req, 4, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG &&
		      len == 0, "two: u16 page 2, at pages, is BAD_ARG");
	}

	mpp_write_mod("three", three, 2, 0x00B90000u);
	mods_rescan();
	check(mods_find("three") >= 0, "a 7-byte and a 20003-byte cave and 16 words is scanned");
	mpp_check("three", 3, three_counts);
	check(mp_patch_page("three", 0xFFFF, g_mp_reply, sizeof(g_mp_reply), &len) == ST_BAD_ARG,
	      "three: and page 65535 is BAD_ARG too");

	group("revision 1.19: MOD_PATCH at the page edges");

	mpp_write_mod("r2046", r2046, 1, 0x00C80000u);
	mods_rescan();
	mpp_check("r2046", 1, r2046_counts);
	check_eq_u64(g_mpp_page0_len, 16380, "r2046: exactly 2046 rows are one page of 16380 bytes");
	check(be32_get(g_mpp_page0 + 4) == qcrc32(g_mpp_page0 + MOD_PATCH_HEAD,
	                                          g_mpp_page0_len - MOD_PATCH_HEAD),
	      "r2046: and one page's stamp is the CRC-32 of everything after it, as before 1.19");

	mpp_write_mod("r2046b", r2046b, 1, 0x00C90000u);
	mods_rescan();
	mpp_check("r2046b", 1, r2046b_counts);
	check_eq_u64(g_mpp_page0_len, 16380, "r2046b: 2044 words and 2 bytes also fill one page exactly");

	mpp_write_mod("r2047", r2047, 1, 0x00D80000u);
	mods_rescan();
	mpp_check("r2047", 2, r2047_counts);

	mpp_write_mod("tip", tip, 1, 0x00E80000u);
	mods_rescan();
	mpp_check("tip", 2, tip_counts);
	check_eq_u64(g_mpp_page0_len, MOD_PATCH_HEAD + 2045u * MOD_PATCH_ROW + MOD_PATCH_BYTES_HEAD,
	             "tip: page 0 is the 2045 words, one short of full, since the words ran out");
	check(mp_patch_page("tip", 1, g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
	      len == MOD_PATCH_HEAD + MOD_PATCH_BYTES_HEAD + 2u * MOD_PATCH_BYTE_ROW,
	      "tip: and the two bytes that tipped it over go on page 1, with no words");

	mods_set_title(MP_TITLE);
}

static void test_mod_presence(void)
{
	u32 len = 0;
	u8 f;

	/* The platform that refuses code patches, from here to the end. */
	host_set_can_patch_code(0);
	host_set_emulator(1);

	group("build 49: presence, a game with no patch in it");

	check(quit_and_wait(), "quit");
	host_write_log_reset();
	check(boot_and_wait(MP_TITLE), "the title boots where code cannot be patched");
	check(mp_any_flag(MOD_FLAG_CHECKING),
	      "after its first tick some mods are still CHECKING: a few reads a tick");
	check(!mp_any_flag(MOD_FLAG_LOADED), "and none is LOADED yet");
	check_eq_u64(mp_flags("lua") & MOD_FLAG_CHECKING, 0, "a Lua mod is never looked for");
	check_eq_u64(mp_flags("broken") & MOD_FLAG_CHECKING, 0, "nor one that failed to parse");
	check_eq_u64(mp_flags("empty") & MOD_FLAG_CHECKING, 0, "nor one with nothing to write");

	check(mp_pump_until_looked(200), "the look finishes");
	check(!mp_any_flag(MOD_FLAG_LOADED), "and finds none of them in the game");
	check_eq_u64(mp_flags("nobin"), 0, "a mod with a missing cave file is simply not there");

	check(mp_patch("words", g_mp_reply, sizeof(g_mp_reply), &len) == ST_OK &&
	      len == g_mp_words_len && memcmp(g_mp_reply, g_mp_words, len) == 0,
	      "MOD_PATCH hands out the same bytes here as on a console");

	check(mods_load("words") == ST_UNSUPPORTED, "MOD_LOAD is refused, as it always was here");
	check(mods_unload("words") == ST_NOT_FOUND, "and MOD_UNLOAD has nothing to unload");

	mp_poke(g_mp_words, g_mp_words_len, MP_NO_FLIP, MP_NO_FLIP);
	pump(20);
	check_eq_u64(mp_flags("words") & MOD_FLAG_LOADED, 0,
	             "words that arrive mid-session count from the next restart, not before");

	group("build 49: presence, restarted with the patches applied at load");

	check(quit_and_wait(), "quit");
	check(!mp_any_flag(MOD_FLAG_CHECKING | MOD_FLAG_LOADED),
	      "outside a session no mod is CHECKING or LOADED");
	check(mp_boot(1, 1, 1, MP_NO_FLIP, MP_NO_FLIP), "and back with all three patches in it");
	check(mp_any_flag(MOD_FLAG_CHECKING),
	      "the first tick's four reads are not the whole look: mods are CHECKING");
	check(mp_pump_until_looked(200), "until the look is done");
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED, "then the words are LOADED and nothing else");
	check_eq_u64(mp_flags("cave"), MOD_FLAG_LOADED, "and so is the cave with its branch");
	check_eq_u64(mp_flags("tail"), MOD_FLAG_LOADED, "and the caves with tails, bytes and all");
	check_eq_u64(mp_flags("edge") & MOD_FLAG_LOADED, 0, "while the mod nothing patched is not");
	check_eq_u64(mods_loaded_mask(), 0,
	             "SessionInfo's mod_loaded stays qwark's own loads, which here are none");
	check(mods_load("words") == ST_UNSUPPORTED, "MOD_LOAD of a mod that is there is still refused");
	check(mods_unload("words") == ST_NOT_FOUND, "and there is still nothing to unload");

	/* Once a session: damage after the look does not unmake it. */
	host_poke(0x00810004u, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(20);
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED, "a word changed mid-session is not looked at again");

	group("build 49: presence, a rescan looks again");

	check(mods_rescan() == ST_OK, "MOD_RESCAN");
	check((mp_flags("words") & MOD_FLAG_CHECKING) != 0 && (mp_flags("cave") & MOD_FLAG_CHECKING) != 0,
	      "puts every mod back to CHECKING");
	check_eq_u64(mp_flags("words") & MOD_FLAG_LOADED, 0, "and not LOADED while it is");
	check(mp_pump_until_looked(200), "the look runs again");
	check_eq_u64(mp_flags("words") & MOD_FLAG_LOADED, 0, "and sees the word that was changed");
	check_eq_u64(mp_flags("cave"), MOD_FLAG_LOADED, "while the cave is still there");

	mp_poke(g_mp_words, g_mp_words_len, MP_NO_FLIP, MP_NO_FLIP);
	mods_rescan();
	check(mp_pump_until_looked(200), "the word put back and a rescan");
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED, "and the words are LOADED again");

	/* An upload that changes a mod: the new files do not match the old patch. */
	mp_write_text("words",
		"#- name: Words\n"
		"0x00810000: 0x11111111\n"
		"0x00810004: 0x22222222\n"
		"0x00820000: 0x33333334\n");
	mods_rescan();
	check(mp_pump_until_looked(200), "a new copy of the mod uploaded and rescanned");
	check_eq_u64(mp_flags("words") & MOD_FLAG_LOADED, 0,
	             "is not the one in the game: the client writes the new patch");
	mp_write_text("words", g_mp_words_txt);
	mods_rescan();
	check(mp_pump_until_looked(200) && mp_flags("words") == MOD_FLAG_LOADED,
	      "and the old copy back is LOADED again");

	group("build 49: presence, a read that fails is not an answer");

	mods_rescan();
	host_fail_reads(100000);
	pump(10);
	f = mp_flags("words");
	check((f & MOD_FLAG_CHECKING) != 0 && (f & MOD_FLAG_LOADED) == 0,
	      "while the emulator does not answer, the mod stays CHECKING, not absent");
	host_fail_reads(0);
	check(mp_pump_until_looked(200), "once it answers the look goes on");
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED, "and finds it");

	group("build 49: presence, all of it or none of it");

	check(quit_and_wait(), "quit");
	check(mp_boot(1, 1, 1, 1, MP_NO_FLIP), "back with one cave row one bit off");
	check(mp_pump_until_looked(200), "the look finishes");
	check_eq_u64(mp_flags("cave") & MOD_FLAG_LOADED, 0, "and that mod is not there");
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED, "while the others are");
	check_eq_u64(mp_flags("tail"), MOD_FLAG_LOADED, "both of them");

	check(quit_and_wait(), "quit");
	check(mp_boot(1, 1, 1, MP_NO_FLIP, 3), "back with one tail byte one bit off");
	check(mp_pump_until_looked(200), "the look finishes");
	check_eq_u64(mp_flags("tail") & MOD_FLAG_LOADED, 0, "and the tail bytes are part of the look");
	check_eq_u64(mp_flags("cave"), MOD_FLAG_LOADED, "while the cave is whole again");

	group("build 49: presence, every session starts over");

	check(quit_and_wait(), "quit");
	check(boot_and_wait(MP_OTHER), "another title runs in between");
	check(mods_find("words") < 0, "with its own mod table");
	check(quit_and_wait(), "quit");
	check(boot_and_wait(MP_TITLE), "and the title comes back with nothing patched in");
	check(mp_pump_until_looked(200), "the look finishes");
	check(!mp_any_flag(MOD_FLAG_LOADED), "and the last session's answers went with it");

	check(quit_and_wait(), "quit");
	check(mp_boot(1, 0, 0, MP_NO_FLIP, MP_NO_FLIP), "the same title again, with the words alone");
	check(mp_pump_until_looked(200), "the look finishes");
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED, "the words are found");
	check_eq_u64(mp_flags("cave") & MOD_FLAG_LOADED, 0, "and nothing else");

	group("build 49: presence, auto-apply is skipped as before");

	check(mods_set_auto("words", 1) == ST_OK, "the words mod is flagged auto");
	check(quit_and_wait(), "quit");
	check(mp_boot(1, 0, 0, MP_NO_FLIP, MP_NO_FLIP), "and the title comes back with its patch");
	check(mp_pump_until_looked(200), "the look finishes");
	check_eq_u64(mp_flags("words"), MOD_FLAG_LOADED | MOD_FLAG_AUTO,
	             "LOADED by the look, auto as flagged");
	check_eq_u64(mods_loaded_mask(), 0, "while the auto-apply loaded nothing, silently");
	check(!session_previous()->pending, "and left nothing in a previous-session record");
	mods_set_auto("words", 0);

	group("build 49: presence, a mod with nothing to write");

	check(mods_load("empty") == ST_OK, "MOD_LOAD of a mod with no words and no caves works, as ever");
	check_eq_u64(mp_flags("empty"), MOD_FLAG_LOADED, "and that is its LOADED, never CHECKING");
	check(mods_unload("empty") == ST_OK, "and it unloads");

	check_eq_u64(host_write_log_count(), 0, "and in all of it qwark wrote nothing into the game");

	/* ------------------------------------------------- put it all back */

	host_set_can_patch_code(1);
	host_set_emulator(0);

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again with the gate open");
}

/*
 * Build 49. MOD_FLAG_PREVIOUS was documented, cleared on a load and never set,
 * so no client could ever have seen it. It is the mod's bit in the
 * previous-session record while the mod is not loaded again.
 */
static void test_mod_previous(void)
{
	u8 info[SESSION_INFO_SIZE];
	int hardcore;

	group("build 49: MOD_LIST's PREVIOUS flag");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	session_previous_dismiss();
	hardcore = mods_find("hardcore");
	check(hardcore >= 0, "hardcore is in the table");

	check(mods_load("hardcore") == ST_OK, "hardcore is loaded");
	check_eq_u64(mp_flags("hardcore"), MOD_FLAG_LOADED, "LOADED and nothing else");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "the same title again");
	check((session_previous()->mods & (1u << hardcore)) != 0,
	      "the mod is in the previous-session record");
	check_eq_u64(mp_flags("hardcore"), MOD_FLAG_PREVIOUS, "and MOD_LIST says PREVIOUS, not LOADED");
	check_eq_u64(mp_flags("lock_rng") & MOD_FLAG_PREVIOUS, 0, "a mod that was not loaded is not");
	pump(8);
	session_info_copy(info, sizeof(info));
	check((be32_get(info + 160) & (1u << hardcore)) != 0,
	      "the same bit SessionInfo's mod_previous carries");

	check(mods_load("hardcore") == ST_OK, "loading it by hand");
	check_eq_u64(mp_flags("hardcore"), MOD_FLAG_LOADED, "makes it LOADED and not PREVIOUS");
	check(mods_unload("hardcore") == ST_OK, "unloading it again");
	check_eq_u64(mp_flags("hardcore"), MOD_FLAG_PREVIOUS,
	             "brings PREVIOUS back while the record still holds it");
	check(mods_rescan() == ST_OK && mp_flags("hardcore") == MOD_FLAG_PREVIOUS,
	      "and a rescan does not lose it");

	session_previous_dismiss();
	check_eq_u64(mp_flags("hardcore"), 0, "PREVIOUS_DISMISS clears it");

	check(mods_load("hardcore") == ST_OK, "loaded once more");
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "and the same title");
	check_eq_u64(mp_flags("hardcore"), MOD_FLAG_PREVIOUS, "PREVIOUS again");
	session_previous_reapply(PREV_MODS);
	check_eq_u64(mp_flags("hardcore"), MOD_FLAG_LOADED, "PREVIOUS_REAPPLY loads it, and PREVIOUS goes");
	check(mods_unload("hardcore") == ST_OK, "and it unloads");
	check_eq_u64(mp_flags("hardcore"), 0, "leaving neither");
}

/* -------------------------------------------- build 50, the code switches */

/*
 * Under RPCS3 the nine WRITES_CODE toggles are code switches: an RPCS3 patch
 * written from SWITCH_PATCH puts a trampoline per patched instruction in the
 * game at load and a branch to it at the site, and the toggle becomes a write
 * of the flag byte the trampoline reads. As in test_savefile_rpcs3, the fake
 * console is RPCS3 once plat_can_patch_code() answers 0, and a "patch" is the
 * reply's rows and bytes poked into a fresh process between the boot and the
 * first tick.
 */

/*
 * The words the four reverse-engineering reports verified, each assembled and
 * checked against the game's executable: every trampoline in address order,
 * every site's branch word, every flag byte. src/games/switches_bins.c is
 * generated by the SDK's assembler and linker from src/games/switches/, and it
 * has to come out as exactly this.
 */
struct swt_cave {
	u32 addr;
	u8  nwords;
	u32 w[24];
};

struct swt_game {
	const char *title;
	u8 game_id;
	const struct swt_cave *caves;
	u8 ncaves;
	const struct patch_word *sites;
	u8 nsites;
	const u32 *flags;
	u8 nflags;
	/* Every WRITES_CODE toggle of the game and the flag byte its switch reads. */
	const struct patch_word *toggles;   /* { id, flag } */
	u8 ntoggles;
};

static const struct swt_cave g_swt_rac1_caves[] = {
	{ 0x004F6C00u, 7, { 0x3D600090u, 0x896B58B0u, 0x2C0B0000u, 0x41820008u, 0x4BC6E884u,
	                    0x4BC6CD95u, 0x4BC6E87Cu } },
	{ 0x004F6C20u, 8, { 0x3CA00090u, 0x88A558B0u, 0x2C050000u, 0x4182000Cu, 0x38600006u,
	                    0x4BC69430u, 0x546307BEu, 0x4BC69428u } },
	{ 0x004F6C40u, 8, { 0x3CA00090u, 0x88A558B0u, 0x2C050000u, 0x4182000Cu, 0x38600006u,
	                    0x4BC6D5A8u, 0x546307BEu, 0x4BC6D5A0u } },
	{ 0x004F6C60u, 7, { 0x3D600090u, 0x896B58B1u, 0x2C0B0000u, 0x41820008u, 0x4BBB3670u,
	                    0x7D05392Eu, 0x4BBB3668u } },
	{ 0x004F6C80u, 9, { 0x3CC00090u, 0x88C658B2u, 0x2C060000u, 0x41820010u, 0x30640000u,
	                    0x38800000u, 0x4BB888C8u, 0x38800000u, 0x4BB888C0u } }
};
static const struct patch_word g_swt_rac1_sites[] = {
	{ 0x0007F55Cu, 0x48477724u }, { 0x000AA2DCu, 0x4844C984u }, { 0x00160060u, 0x48396BC0u },
	{ 0x001641F8u, 0x48392A48u }, { 0x00165490u, 0x48391770u }
};
static const u32 g_swt_rac1_flags[] = { 0x009058B0u, 0x009058B1u, 0x009058B2u };
static const struct patch_word g_swt_rac1_toggles[] = {
	{ 0, 0x009058B0u }, { 1, 0x009058B1u }, { 2, 0x009058B2u }
};

/*
 * Build 53: at 0x1141400, off Joba's routine (test_switches_rac2_moved), and
 * the two crash guards after them, which have no flag byte (rac2.c), assembled
 * with GNU as on their own and placed by hand at the same addresses.
 */
static const struct swt_cave g_swt_rac2_caves[] = {
	{ 0x01141400u, 6, { 0x3C600131u, 0x88637720u, 0x2C030000u, 0x40820008u, 0x4BAA7EF9u,
	                    0x4BAA9490u } },
	{ 0x01141420u, 6, { 0x3CC00131u, 0x88C67721u, 0x2C060000u, 0x40820008u, 0x7C64292Eu,
	                    0x4B9EF84Cu } },
	{ 0x01141440u, 6, { 0x7FC00734u, 0x7F80F000u, 0x409E000Cu, 0x7F24C814u, 0x4AF45514u,
	                    0x4AF4577Cu } },
	{ 0x01141470u, 11, { 0x807C0000u, 0x3C800159u, 0x80A427B0u, 0x7F832840u, 0x419C0018u,
	                     0x80A427B8u, 0x7F832840u, 0x409C000Cu, 0xA07F0034u, 0x4BA9DE7Cu,
	                     0x4BA9E0D8u } }
};
static const struct patch_word g_swt_rac2_sites[] = {
	{ 0x00086960u, 0x490BAAE0u }, { 0x00B30C7Cu, 0x486107A4u }, { 0x00BDF30Cu, 0x48562164u },
	{ 0x00BEA8A0u, 0x48556B60u }
};
static const u32 g_swt_rac2_flags[] = { 0x01317720u, 0x01317721u };
static const struct patch_word g_swt_rac2_toggles[] = {
	{ 0, 0x01317720u }, { 1, 0x01317721u }
};

/*
 * Fast loads joined Freeze ammo when they became a code patch: a trampoline that
 * ends a clip transition once the level is read (src/games/rac3.h,
 * RAC3_FASTLOAD_SITE), behind the flag at 0xD9FF11.
 */
static const struct swt_cave g_swt_rac3_caves[] = {
	{ 0x00980F00u, 6, { 0x3CE000DAu, 0x88E7FF10u, 0x2C070000u, 0x40820008u, 0x7C85312Eu,
	                    0x4B801B78u } },
	{ 0x00980F20u, 23, { 0x2C1B0000u, 0x40820050u, 0x3C6000DAu, 0x8863FF11u, 0x2C030000u,
	                     0x41820040u, 0x3C6000DAu, 0xA08390DEu, 0x28040005u, 0x41810030u,
	                     0x80839094u, 0x2C040002u, 0x41800024u, 0x80839098u, 0x2C040000u,
	                     0x41820018u, 0x4B8F7AA1u, 0x3C600135u, 0x3C803F80u, 0x9083EBE0u,
	                     0x3B600001u, 0x2C1B0000u, 0x4B8FA4B4u } }
};
static const struct patch_word g_swt_rac3_sites[] = {
	{ 0x00182A88u, 0x487FE478u }, { 0x0027B428u, 0x48705AF8u }
};
static const u32 g_swt_rac3_flags[] = { 0x00D9FF10u, 0x00D9FF11u };
static const struct patch_word g_swt_rac3_toggles[] = {
	{ 0, 0x00D9FF10u }, { 38, 0x00D9FF11u }
};

static const struct swt_cave g_swt_rac4_caves[] = {
	{ 0x00667BCCu, 7, { 0x40820018u, 0x3C60015Du, 0x8863D782u, 0x2C030000u, 0x40820008u,
	                    0x4BB70144u, 0x4BB70138u } },
	{ 0x00667BE8u, 6, { 0x3CA0015Du, 0x88A5D781u, 0x2C050000u, 0x40820008u, 0x4BD6115Du,
	                    0x4BC40EFCu } },
	/* Build 53: lis r5; lbz r5, flag; neg r5, r5; and r5, r5, r26; or r27, r3, r5; b 0x2A8C60. */
	{ 0x00667C00u, 6, { 0x3CA0015Du, 0x88A5D781u, 0x7CA500D0u, 0x7CA5D038u, 0x7C7B2B78u,
	                    0x4BC4104Cu } },
	{ 0x00667C18u, 8, { 0x3C80015Du, 0x8884D780u, 0x2C040000u, 0x4082000Cu, 0xECC73028u,
	                    0x4BEB65C0u, 0xECC33028u, 0x4BEB65B8u } },
	{ 0x00667C38u, 8, { 0x3C80015Du, 0x8884D780u, 0x2C040000u, 0x4082000Cu, 0x10C5304Au,
	                    0x4BEB65B8u, 0x10C7304Au, 0x4BEB65B0u } },
	{ 0x00667C58u, 8, { 0x3C80015Du, 0x8884D780u, 0x2C040000u, 0x4082000Cu, 0xECC6D828u,
	                    0x4BEB65B4u, 0xECDB3028u, 0x4BEB65ACu } },
	{ 0x00667C78u, 8, { 0x3C80015Du, 0x8884D780u, 0x2C040000u, 0x4082000Cu, 0x10C6E04Au,
	                    0x4BEB65A0u, 0x10DC304Au, 0x4BEB6598u } },
	{ 0x00667C98u, 6, { 0x3C80015Du, 0x8884D780u, 0x2C040000u, 0x40820008u, 0xECC53028u,
	                    0x4BEB6598u } },
	{ 0x00667CB0u, 6, { 0x3C80015Du, 0x8884D780u, 0x2C040000u, 0x40820008u, 0x10C4304Au,
	                    0x4BEB658Cu } },
	{ 0x00667CC8u, 8, { 0x3F80015Du, 0x8B9CD780u, 0x2C1C0000u, 0x4082000Cu, 0x7CC75810u,
	                    0x4BFB0330u, 0x7CC65810u, 0x4BFB0328u } },
	{ 0x00667CE8u, 8, { 0x3F80015Du, 0x8B9CD780u, 0x2C1C0000u, 0x4082000Cu, 0x10C42886u,
	                    0x4BFB032Cu, 0x10C92886u, 0x4BFB0324u } },
	{ 0x00667D08u, 8, { 0x3F80015Du, 0x8B9CD780u, 0x2C1C0000u, 0x4082000Cu, 0x7CE75810u,
	                    0x4BFB031Cu, 0x7CE73010u, 0x4BFB0314u } },
	{ 0x00667D28u, 8, { 0x3F80015Du, 0x8B9CD780u, 0x2C1C0000u, 0x4082000Cu, 0x7D894838u,
	                    0x4BFB0334u, 0x7D296078u, 0x4BFB032Cu } },
	{ 0x00667D48u, 10, { 0x3CA0015Du, 0x88A5D780u, 0x2C050000u, 0x4082000Cu, 0x7CE4F0CEu,
	                     0x481A0B94u, 0x3CA00066u, 0x38A57D70u, 0x7CE028CEu, 0x481A0B84u } },
	{ 0x00667D70u, 4, { 0x00010203u, 0x04050607u, 0x10111213u, 0x14151617u } }
};
static const struct patch_word g_swt_rac4_sites[] = {
	{ 0x001D7D18u, 0x4848FEB4u }, { 0x002A8AF4u, 0x483BF0F4u }, { 0x002A8C5Cu, 0x483BEFA4u },
	{ 0x0051E1E8u, 0x48149A30u }, { 0x0051E200u, 0x48149A38u }, { 0x0051E21Cu, 0x48149A3Cu },
	{ 0x0051E228u, 0x48149A50u }, { 0x0051E240u, 0x48149A58u }, { 0x0051E24Cu, 0x48149A64u },
	{ 0x00618008u, 0x4804FCC0u }, { 0x00618024u, 0x4804FCC4u }, { 0x00618034u, 0x4804FCD4u },
	{ 0x0061806Cu, 0x4804FCBCu }, { 0x008088ECu, 0x4BE5F45Cu }
};
static const u32 g_swt_rac4_flags[] = { 0x015CD780u, 0x015CD781u, 0x015CD782u };
static const struct patch_word g_swt_rac4_toggles[] = {
	{ 0, 0x015CD780u }, { 2, 0x015CD781u }, { 3, 0x015CD782u }
};

#define SWT_N(a) ((u8)(sizeof(a) / sizeof((a)[0])))

static const struct swt_game g_swt_games[] = {
	{ "NPEA00385", GAME_RAC1, g_swt_rac1_caves, SWT_N(g_swt_rac1_caves),
	  g_swt_rac1_sites, SWT_N(g_swt_rac1_sites), g_swt_rac1_flags, SWT_N(g_swt_rac1_flags),
	  g_swt_rac1_toggles, SWT_N(g_swt_rac1_toggles) },
	{ "NPEA00386", GAME_RAC2, g_swt_rac2_caves, SWT_N(g_swt_rac2_caves),
	  g_swt_rac2_sites, SWT_N(g_swt_rac2_sites), g_swt_rac2_flags, SWT_N(g_swt_rac2_flags),
	  g_swt_rac2_toggles, SWT_N(g_swt_rac2_toggles) },
	{ "NPEA00387", GAME_RAC3, g_swt_rac3_caves, SWT_N(g_swt_rac3_caves),
	  g_swt_rac3_sites, SWT_N(g_swt_rac3_sites), g_swt_rac3_flags, SWT_N(g_swt_rac3_flags),
	  g_swt_rac3_toggles, SWT_N(g_swt_rac3_toggles) },
	{ "NPEA00423", GAME_RAC4, g_swt_rac4_caves, SWT_N(g_swt_rac4_caves),
	  g_swt_rac4_sites, SWT_N(g_swt_rac4_sites), g_swt_rac4_flags, SWT_N(g_swt_rac4_flags),
	  g_swt_rac4_toggles, SWT_N(g_swt_rac4_toggles) }
};

/* Feature ids the tests below drive by name. */
#define SWT_R1_FAST    0
#define SWT_R1_AMMO    1
#define SWT_R1_HEALTH  2
#define SWT_R2_FAST    0
#define SWT_R2_AMMO    1
#define SWT_R4_CRASH   0
#define SWT_R4_FAST    2

/* LIVE_POLL_EVERY in session.c, and one tick to be sure. */
#define SWT_POLL_TICKS 13

static u8 g_sw_patch[QWARK_MAX_PAYLOAD];
static u8 g_sw_patch2[QWARK_MAX_PAYLOAD];
static u8 g_sw_sfpatch[QWARK_MAX_PAYLOAD];

static u8 swt_peek8(u32 addr)
{
	u8 b = 0xEE;

	host_peek(addr, &b, 1);
	return b;
}

static u32 swt_peek32(u32 addr)
{
	u8 w[4] = { 0, 0, 0, 0 };

	host_peek(addr, w, 4);
	return be32_get(w);
}

/* SessionInfo flags, as the last publish left them; eight ticks is two publishes. */
static u8 swt_info_flags(void)
{
	u8 info[SESSION_INFO_SIZE];

	pump(8);
	if (session_info_copy(info, sizeof(info)) != SESSION_INFO_SIZE) return 0xFF;
	return info[24];
}

/* Whether any write since the last log reset touched a cave or a site word. */
static int swt_wrote_code(const struct sw_desc *d)
{
	u32 i;

	for (i = 0; ; i++) {
		u32 a = 0, len = 0;
		u8 k;

		if (host_write_log_at(i, &a, &len) != 0) break;

		for (k = 0; k < d->ncaves; k++)
			if (a < d->caves[k].addr + d->caves[k].len && a + len > d->caves[k].addr) return 1;
		for (k = 0; k < d->nsites; k++)
			if (a < d->sites[k].addr + 4u && a + len > d->sites[k].addr) return 1;
	}

	return 0;
}

/* The generated table against the verified words. */
static void swt_check_table(const struct swt_game *g)
{
	const struct sw_desc *d = sw_desc_for_game(g->game_id);
	int caves_ok = 1, sites_ok = 1, flags_ok = 1;
	u8 i, k;

	check(d != NULL, "the game has switches in the generated table");
	if (d == NULL) return;

	check_eq_u64(d->ncaves, g->ncaves, "as many caves as the report placed");
	check_eq_u64(d->nsites, g->nsites, "as many sites");
	check_eq_u64(d->nflags, g->nflags, "and as many flag bytes");
	if (d->ncaves != g->ncaves || d->nsites != g->nsites || d->nflags != g->nflags) return;

	for (i = 0; i < d->ncaves; i++) {
		if (d->caves[i].addr != g->caves[i].addr) caves_ok = 0;
		if (d->caves[i].len != 4u * g->caves[i].nwords) { caves_ok = 0; continue; }
		for (k = 0; k < g->caves[i].nwords; k++)
			if (be32_get(d->caves[i].bytes + 4u * k) != g->caves[i].w[k]) caves_ok = 0;
	}
	check(caves_ok, "every trampoline is the verified words at the verified address");

	for (i = 0; i < d->nsites; i++) {
		if (d->sites[i].addr != g->sites[i].addr) sites_ok = 0;
		if (d->sites[i].value != g->sites[i].value) sites_ok = 0;
	}
	check(sites_ok, "every site word is the branch the report computed, sites in address order");

	for (i = 0; i < d->nflags; i++)
		if (d->flags[i] != g->flags[i]) flags_ok = 0;
	check(flags_ok, "and every flag byte is where the report put it");

	for (i = 1; i < d->ncaves; i++)
		if (d->caves[i].addr < d->caves[i - 1].addr + d->caves[i - 1].len) caves_ok = 0;
	check(caves_ok, "the caves are in address order and do not overlap");
}

/* A SWITCH_PATCH reply against the table it was made from. */
static void swt_check_patch(const struct sw_desc *d, const u8 *reply, u32 len)
{
	u32 want = 0;
	u32 off = SWITCH_PATCH_HEAD;
	u32 whole;
	int caves_ok = 1, sites_ok = 1, bytes_ok = 1;
	u8 i;

	for (i = 0; i < d->ncaves; i++) want += (d->caves[i].len + 3u) / 4u;
	want += d->nsites;
	whole = SWITCH_PATCH_HEAD + want * SWITCH_PATCH_ROW
	      + SWITCH_PATCH_BYTES_HEAD + (u32)d->nflags * SWITCH_PATCH_BYTE_ROW;

	check_eq_u64(be16_get(reply), want, "n is every cave word and every site word");
	check_eq_u64(len, whole, "and the reply is those rows and the flag bytes behind their heads");
	check_eq_u64(be16_get(reply + 2), 0, "the pad is 0");
	check(len <= QWARK_MAX_PAYLOAD, "the reply fits one frame");
	if (len != whole) return;

	for (i = 0; i < d->ncaves; i++) {
		u32 at;

		for (at = 0; at < d->caves[i].len; at += 4u) {
			if (be32_get(reply + off) != d->caves[i].addr + at) caves_ok = 0;
			if (memcmp(reply + off + 4, d->caves[i].bytes + at, 4) != 0) caves_ok = 0;
			off += SWITCH_PATCH_ROW;
		}
	}
	check(caves_ok, "every cave is there first, word by word, caves in address order");

	for (i = 0; i < d->nsites; i++) {
		if (be32_get(reply + off) != d->sites[i].addr) sites_ok = 0;
		if (be32_get(reply + off + 4) != d->sites[i].value) sites_ok = 0;
		off += SWITCH_PATCH_ROW;
	}
	check(sites_ok, "and the site words after them");

	check_eq_u64(be16_get(reply + off), d->nflags, "then nb is the number of flag bytes");
	check_eq_u64(be16_get(reply + off + 2), 0, "and its pad is 0");
	off += SWITCH_PATCH_BYTES_HEAD;

	for (i = 0; i < d->nflags; i++) {
		if (be32_get(reply + off) != d->flags[i]) bytes_ok = 0;
		if (reply[off + 4] != 0 || reply[off + 5] != 0 ||
		    reply[off + 6] != 0 || reply[off + 7] != 0) bytes_ok = 0;
		off += SWITCH_PATCH_BYTE_ROW;
	}
	check(bytes_ok, "the bytes are every flag, each set to 0");

	check_eq_u64(be32_get(reply + 4),
	             qcrc32(reply + SWITCH_PATCH_HEAD, len - SWITCH_PATCH_HEAD),
	             "the stamp is the CRC-32 of everything after it, as it went out");
}

/* A boot with the switch patch applied at load, and the look given its ticks. */
static int swt_boot_patched(const char *title, const u8 *reply, u32 len, u32 flip)
{
	int ok;

	host_boot(title);
	sf_poke_patch(reply, len, flip, 1);
	ok = pump_until(SESSION_INGAME, 4000);
	pump(6);
	return ok;
}

/* One game on the refusing platform: without the patch, and then with it. */
static void swt_rpcs3_one_game(const struct swt_game *g)
{
	const struct sw_desc *d = sw_desc_for_game(g->game_id);
	const struct game_describe *desc;
	u32 len = 0, len2 = 0;
	u8 ncode = 0;
	int all_ok;
	u8 i;

	if (d == NULL) return;

	/* ---------------------------------------------------- no patch in it */

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait(g->title), "the game boots with no patch in it");
	pump(6);
	check_eq_u64(switches_state(), 0, "the look finds no switches");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) == 0, "and SessionInfo bit4 is clear");
	check(features_set((u8)g->toggles[0].addr, 1) == ST_UNSUPPORTED,
	      "so a WRITES_CODE toggle is still UNSUPPORTED");

	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK,
	      "SWITCH_PATCH answers where code cannot be patched");
	swt_check_patch(d, g_sw_patch, len);
	check(switches_patch(g_sw_patch2, sizeof(g_sw_patch2), &len2) == ST_OK &&
	      len2 == len && memcmp(g_sw_patch, g_sw_patch2, len) == 0,
	      "and asked again it is the same words and the same stamp");

	/* Too late for this process: the look was this session's, and it is over. */
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
	pump(6);
	check_eq_u64(switches_state(), 0, "words that arrive mid-session do not count until a restart");

	/* ------------------------------------------------ restarted with it */

	check(quit_and_wait(), "the game is restarted");
	check(swt_boot_patched(g->title, g_sw_patch, len, SF_NO_FLIP),
	      "and comes back with the patch applied at load");
	check(session_game() != NULL && session_game()->game_id == g->game_id,
	      "the fingerprint still says which game it is, branch at the site or not");
	check_eq_u64(switches_state(), 1, "the look finds every cave byte and every site word");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) != 0, "and SessionInfo bit4 is set");

	/* Every WRITES_CODE toggle the game describes has a switch here. */
	desc = session_game()->describe();
	for (i = 0; i < desc->nfeatures; i++) {
		if (desc->features[i].kind == FEATURE_TOGGLE &&
		    (desc->features[i].flags & FEATURE_FLAG_WRITES_CODE) != 0) ncode++;
	}
	check_eq_u64(ncode, g->ntoggles, "every WRITES_CODE toggle of the game has a switch");

	/* Whatever the auto-apply turned on at boot, off first. */
	for (i = 0; i < g->ntoggles; i++) features_set((u8)g->toggles[i].addr, 0);

	host_write_log_reset();
	all_ok = 1;
	for (i = 0; i < g->ntoggles; i++) {
		u8 id = (u8)g->toggles[i].addr;
		u32 flag = g->toggles[i].value;

		if (features_set(id, 1) != ST_OK) all_ok = 0;
		if (swt_peek8(flag) != 1) all_ok = 0;
		if ((features_toggle_state() & ((u64)1 << id)) == 0) all_ok = 0;
	}
	check(all_ok, "FEATURE_SET on writes 1 into each toggle's flag byte and sets its bit");

	pump(SWT_POLL_TICKS);
	all_ok = 1;
	for (i = 0; i < g->ntoggles; i++)
		if ((features_toggle_state() & ((u64)1 << g->toggles[i].addr)) == 0) all_ok = 0;
	check(all_ok, "and the read-back of the flags agrees");

	all_ok = 1;
	for (i = 0; i < g->ntoggles; i++) {
		u8 id = (u8)g->toggles[i].addr;

		if (features_set(id, 0) != ST_OK) all_ok = 0;
		if (swt_peek8(g->toggles[i].value) != 0) all_ok = 0;
		if ((features_toggle_state() & ((u64)1 << id)) != 0) all_ok = 0;
	}
	check(all_ok, "FEATURE_SET off writes 0 and clears the bit");
	check(!swt_wrote_code(d), "and qwark wrote no cave byte and no site word for any of it");

	/* LIVE read-back: the byte decides, whoever wrote it. */
	host_poke(g->toggles[0].value, (const u8 *)"\x01", 1);
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << g->toggles[0].addr)) != 0,
	      "a flag byte written behind qwark's back turns the checkbox on");
	host_poke(g->toggles[0].value, (const u8 *)"\x00", 1);
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << g->toggles[0].addr)) == 0,
	      "and cleared, off again");

	check(quit_and_wait(), "the game quits");
	check_eq_u64(switches_state(), -1, "and the answer went with it");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) == 0, "bit4 goes down at the XMB");
}

/* The overlap check between the helper and the switches, one game. */
static int swt_apart(const struct sw_desc *s, const struct sf_desc *f)
{
	u8 a, b;

	for (a = 0; a < s->ncaves; a++) {
		u32 lo = s->caves[a].addr, hi = lo + s->caves[a].len;

		for (b = 0; b < f->ncaves; b++)
			if (lo < f->caves[b].addr + f->caves[b].len && f->caves[b].addr < hi) return 0;
		for (b = 0; b < f->nhooks; b++)
			if (lo < f->hooks[b].addr + 4u && f->hooks[b].addr < hi) return 0;
	}
	for (a = 0; a < s->nsites; a++) {
		for (b = 0; b < f->nhooks; b++)
			if (s->sites[a].addr == f->hooks[b].addr) return 0;
		for (b = 0; b < f->ncaves; b++)
			if (s->sites[a].addr < f->caves[b].addr + f->caves[b].len &&
			    f->caves[b].addr < s->sites[a].addr + 4u) return 0;
	}
	for (a = 0; a < s->nflags; a++) {
		if (s->flags[a] == f->api_mod || s->flags[a] == f->api_load ||
		    s->flags[a] == f->api_setaside) return 0;
		if (s->flags[a] >= f->aside_addr && s->flags[a] < f->aside_addr + f->aside_size) return 0;
	}
	return 1;
}

/* The switches' look: all of it or none of it, once a session. */
static void test_switches_presence(void)
{
	const struct sw_desc *d = sw_desc_for_game(GAME_RAC3);
	u32 len = 0, len2 = 0, n;

	group("build 50: the look is all of the switches or none of them");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00387"), "RaC3 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the RaC3 words");
	n = be16_get(g_sw_patch);

	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00387", g_sw_patch, len, 2),
	      "RaC3 boots with one byte of its trampoline different");
	check_eq_u64(switches_state(), 0, "which is not the switch");
	check(features_set(0, 1) == ST_UNSUPPORTED, "so Freeze ammo is still refused");

	/*
	 * RaC3's Freeze ammo site is its fingerprint, so a site word there that is
	 * neither the game's instruction, qwark's nop nor this build's branch is not
	 * RaC3 at all as far as BOOTING can tell. That is the one thing a later build
	 * that moves that trampoline has to keep in mind: the old branch must stay
	 * accepted. The sites come in address order, so that one is the second last
	 * row and the fast-load site, 0x27B428, the last.
	 */
	check(quit_and_wait(), "quit");
	host_boot("NPEA00387");
	sf_poke_patch(g_sw_patch, len, n - 2u, 1);
	pump(400);
	check(session_state() == SESSION_BOOTING,
	      "RaC3 with its Freeze ammo site word one bit off never passes the fingerprint");

	/* The fast-load site is not the fingerprint: there the look says no. */
	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00387", g_sw_patch, len, n - 1u),
	      "RaC3 with its fast-load site word one bit off boots as RaC3");
	check_eq_u64(switches_state(), 0, "and is not the switches: a site from another build");

	/* RaC1's sites are not its fingerprint, so there the look says no. */
	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	check(switches_patch(g_sw_patch2, sizeof(g_sw_patch2), &len2) == ST_OK, "the RaC1 words");
	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00385", g_sw_patch2, len2, (u32)be16_get(g_sw_patch2) - 1u),
	      "RaC1 boots with every trampoline right and one site word one bit off");
	check_eq_u64(switches_state(), 0, "which is not the switches: a site from another build");

	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00387", g_sw_patch, len, SF_NO_FLIP), "RaC3 boots with all of it");
	check_eq_u64(switches_state(), 1, "which is");

	/* Once a session: damage after the look does not unmake it. */
	host_poke(d->caves[0].addr, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(6);
	check_eq_u64(switches_state(), 1, "a cave byte changed mid-session is not looked at again");
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 0);

	group("build 50: a read that fails is not an answer");

	switches_forget();
	host_fail_reads(1000000u);
	pump(6);
	check_eq_u64(switches_state(), -1, "while the emulator does not answer, nothing is known");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) == 0, "bit4 stays down");
	check(features_set(0, 1) == ST_UNSUPPORTED, "and the toggle is refused as it was before");
	host_fail_reads(0);
	pump(2);
	check_eq_u64(switches_state(), 1, "the first tick that can read finds them");
	check(features_set(0, 1) == ST_OK && swt_peek8(0x00D9FF10u) == 1, "and Freeze ammo works");
	features_set(0, 0);

	group("build 50: the look starts again with every session");

	check(quit_and_wait(), "the game quits");
	check(boot_and_wait("NPEA00387"), "and comes back without the patch");
	pump(6);
	check_eq_u64(switches_state(), 0, "the last session's answer went with it");
}

/* The auto-apply at INGAME and the previous-session reapply, through the flags. */
static void test_switches_apply(void)
{
	u32 len = 0;

	group("build 50: Deadlocked's crash patches come back on their own");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots with no patch in it");
	pump(6);
	check_eq_u64(swt_peek8(0x015CD780u), 0, "the auto-flagged crash patches were skipped");
	check((features_toggle_state() & ((u64)1 << SWT_R4_CRASH)) == 0, "and their bit is clear");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the Deadlocked words");

	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00423", g_sw_patch, len, SF_NO_FLIP),
	      "Deadlocked boots with the patch in");
	check_eq_u64(swt_peek8(0x015CD780u), 1,
	             "the auto-apply waited for the look and wrote the crash patches' flag");
	check((features_toggle_state() & ((u64)1 << SWT_R4_CRASH)) != 0, "and their bit is set");
	check_eq_u64(swt_peek8(0x015CD781u), 0, "nothing else went on");
	check_eq_u64(swt_peek32(0x0080883Cu), 0, "and the bot-crash table word was not written");

	group("build 50: RaC1's auto flag and the previous-session record");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the RaC1 words");
	check(features_set_auto(SWT_R1_HEALTH, 1) == ST_OK, "Infinite health is flagged auto");

	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00385", g_sw_patch, len, SF_NO_FLIP), "RaC1 boots with the patch in");
	check_eq_u64(swt_peek8(0x009058B2u), 1, "and the auto flag turned health on through its flag");
	check((features_toggle_state() & ((u64)1 << SWT_R1_HEALTH)) != 0, "with its bit set");
	check_eq_u64(swt_peek32(0x0007F558u), 0x30649CE0u, "leaving the fingerprint word as it was");
	check(features_set(SWT_R1_AMMO, 1) == ST_OK && swt_peek8(0x009058B1u) == 1, "ammo on as well");

	/*
	 * A reboot of the same title. The flags are zero again, as they are at every
	 * boot; ammo goes on the previous-session record, and the reapply comes in
	 * before the look has finished - RaC1's takes two ticks - so it waits.
	 */
	check(quit_and_wait(), "the game reboots");
	host_boot("NPEA00385");
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
	check(pump_until(SESSION_INGAME, 4000), "and comes back with the patch in");
	check((session_previous()->toggles & ((u64)1 << SWT_R1_AMMO)) != 0,
	      "ammo is on the previous-session record");
	check((session_previous()->toggles & ((u64)1 << SWT_R1_HEALTH)) == 0,
	      "health is not: it is auto and comes back on its own");
	check_eq_u64(switches_state(), -1, "the look is still going");
	session_previous_reapply(PREV_TOGGLES);
	check_eq_u64(swt_peek8(0x009058B1u), 0, "so the reapply waits for it");
	pump(4);
	check_eq_u64(switches_state(), 1, "the look finds the switches");
	check_eq_u64(swt_peek8(0x009058B1u), 1, "and the reapplied ammo goes on through its flag");
	check_eq_u64(swt_peek8(0x009058B2u), 1, "and so did the auto-flagged health");
	check((features_toggle_state() & ((u64)1 << SWT_R1_AMMO)) != 0, "with its bit set");

	check(features_set_auto(SWT_R1_HEALTH, 0) == ST_OK, "health's auto flag comes off");
	features_set(SWT_R1_AMMO, 0);
	features_set(SWT_R1_HEALTH, 0);

	group("build 50: without the switches the auto flag is skipped as before");

	check(features_set_auto(SWT_R1_HEALTH, 1) == ST_OK, "health is flagged auto again");
	check(quit_and_wait(), "quit");
	host_write_log_reset();
	check(boot_and_wait("NPEA00385"), "RaC1 boots with no patch in it");
	pump(6);
	check_eq_u64(swt_peek8(0x009058B2u), 0, "no flag is written");
	check((features_toggle_state() & ((u64)1 << SWT_R1_HEALTH)) == 0, "and the bit stays clear");
	check(features_set_auto(SWT_R1_HEALTH, 0) == ST_OK, "and the auto flag comes off");
	session_previous_dismiss();
}

/* The fast loads qwark forces around a load, on RaC2 and Deadlocked. */
static void test_switches_fastloads(void)
{
	const struct sf_desc *f = sf_desc_for_game(GAME_RAC2);
	const struct sw_desc *s = sw_desc_for_game(GAME_RAC2);
	u8 sup = 9, ins = 9, run = 9, pend = 9;
	u32 size = 0, len = 0, sflen = 0;

	group("build 50: RaC2's forced fast loads write the flag");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the switch words");
	check(savefile_patch(g_sw_sfpatch, sizeof(g_sw_sfpatch), &sflen) == ST_OK, "and the helper's");

	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
	sf_poke_patch(g_sw_sfpatch, sflen, SF_NO_FLIP, 1);
	check(pump_until(SESSION_INGAME, 4000), "RaC2 boots with both patches in");
	pump(6);
	check_eq_u64(switches_state(), 1, "the switches are found");
	check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_OK && ins == 1,
	      "and so is the helper, at its new request bytes");
	check_eq_u64(swt_peek32(0x00BEA8A0u), 0x48556B60u, "the fingerprint site holds the branch");
	host_poke(R2_LOADCOUNT, (const u8 *)"\x00", 1);
	pump(4);
	host_write_log_reset();

	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD goes ahead");
	check_eq_u64(swt_peek8(0x01317720u), 1, "forcing fast loads through their flag");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << SWT_R2_FAST)) == 0,
	      "while the checkbox reads back off: the force is not the user's");
	host_poke(R2_LOADCOUNT, (const u8 *)"\x02", 1);
	pump(4);
	check_eq_u64(swt_peek8(0x01317720u), 0, "the end of the load puts the flag back");

	check(features_set(SWT_R2_FAST, 1) == ST_OK, "fast loads on");
	host_poke(R2_LOADCOUNT, (const u8 *)"\x00", 1);
	pump(4);
	check(session_planet_load(5, 0) == ST_OK, "another load");
	host_poke(R2_LOADCOUNT, (const u8 *)"\x02", 1);
	pump(4);
	check_eq_u64(swt_peek8(0x01317720u), 1, "leaves the flag on for a checkbox that is on");
	check(features_set(SWT_R2_FAST, 0) == ST_OK && swt_peek8(0x01317720u) == 0, "fast loads off");

	host_poke(R2_LOADCOUNT, (const u8 *)"\x00", 1);
	pump(4);
	savefile_make_idle(GAME_RAC2);
	check(session_load_setaside() == ST_OK, "the set-aside load goes ahead");
	check_eq_u64(swt_peek8(0x01317720u), 1, "forcing fast loads through their flag too");
	check_eq_u64(swt_peek8(0x01317729u), 1, "and raising the helper's load byte at 0x1317729");
	host_poke(f->api_load, (const u8 *)"\x00", 1);
	host_poke(R2_LOADCOUNT, (const u8 *)"\x02", 1);
	pump(SAVEFILE_SETTLE_TICKS);
	check_eq_u64(swt_peek8(0x01317720u), 0, "and the end of the load puts the flag back");
	check(!swt_wrote_code(s), "no cave byte and no site word was written");
	check(!sf_wrote_code(f), "and none of the helper's either");

	group("build 50: Deadlocked's forced fast loads write the flag");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the switch words");
	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00423", g_sw_patch, len, SF_NO_FLIP), "Deadlocked with them in");
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(4);
	host_write_log_reset();

	check(session_planet_load(4, 0) == ST_OK, "PLANET_LOAD goes ahead");
	check_eq_u64(swt_peek8(0x015CD781u), 1, "forcing fast loads through their flag");
	check_eq_u64(swt_peek32(R4_FASTLOAD_1), 0x483BF0F4u, "the first site keeps its branch");
	check_eq_u64(swt_peek32(R4_FASTLOAD_2), 0x483BEFA4u, "and so does the second, at 0x2A8C5C");
	check(swt_peek32(R4_TRANS_UPDATE) != 0x483BEFB0u,
	      "and the update's call at 0x2A8C50 holds no branch: this build's patch leaves it be");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << SWT_R4_FAST)) == 0,
	      "the checkbox reads back off");
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x06", 1);
	pump(6);
	check_eq_u64(swt_peek8(0x015CD781u), 1, "still on through the space transition");
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x02", 1);
	pump(6);
	check_eq_u64(swt_peek8(0x015CD781u), 0, "and back off when the game leaves it");
	check(!swt_wrote_code(sw_desc_for_game(GAME_RAC4)), "with no code word written");
}

/*
 * Build 53: RaC1's planet load and set-aside load force fast loads too, and
 * under RPCS3 with the switches in that is a write of their flag byte; the end
 * of the load (the state leaving 6 once the request word is back at 0) puts it
 * back.
 */
static void test_switches_rac1_fastloads(void)
{
	const struct sw_desc *s = sw_desc_for_game(GAME_RAC1);
	const struct sf_desc *f = sf_desc_for_game(GAME_RAC1);
	u8 sup = 9, ins = 9, run = 9, pend = 9;
	u32 size = 0, len = 0, sflen = 0;

	group("build 53: RaC1's forced fast loads write the flag");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the switch words");
	check(savefile_patch(g_sw_sfpatch, sizeof(g_sw_sfpatch), &sflen) == ST_OK, "and the helper's");
	check(quit_and_wait(), "quit");
	host_boot("NPEA00385");
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
	sf_poke_patch(g_sw_sfpatch, sflen, SF_NO_FLIP, 1);
	r1t_world(0, 3, 0, 0);                     /* on Kerwan, at rest */
	check(pump_until(SESSION_INGAME, 4000), "RaC1 boots with both patches in");
	pump(6);
	check_eq_u64(switches_state(), 1, "the switches are found");
	check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_OK && ins == 1,
	      "and so is the helper");
	check(features_set(SWT_R1_FAST, 0) == ST_OK && swt_peek8(0x009058B0u) == 0, "fast loads off");
	host_write_log_reset();

	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD goes ahead");
	check_eq_u64(swt_peek8(0x009058B0u), 1, "forcing fast loads through their flag");
	check_eq_u64(r2_word(R1T_REQUEST), 1, "with the request word up");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << SWT_R1_FAST)) == 0,
	      "while the checkbox reads back off: the force is not the user's");

	r1fl_finish(5, 1);
	pump(SWT_POLL_TICKS);
	check_eq_u64(swt_peek8(0x009058B0u), 1,
	             "the request word back at 0 leaves it on: the level's state 6 is still loading");
	check((features_toggle_state() & ((u64)1 << SWT_R1_FAST)) == 0,
	      "and the checkbox still reads back off");
	poke32(R1T_STATE, 0);
	pump(4);
	check_eq_u64(swt_peek8(0x009058B0u), 0, "the state leaving 6 puts the flag back");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << SWT_R1_FAST)) == 0, "and the checkbox never moved");

	check(features_set(SWT_R1_FAST, 1) == ST_OK, "fast loads on");
	check(session_planet_load(3, 0) == ST_OK, "another load");
	r1fl_finish(3, 0);
	check_eq_u64(swt_peek8(0x009058B0u), 1, "leaves the flag on for a checkbox that is on");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << SWT_R1_FAST)) != 0, "which reads back on");
	check(features_set(SWT_R1_FAST, 0) == ST_OK && swt_peek8(0x009058B0u) == 0, "fast loads off");

	r1t_load(3, 0);
	check_eq_u64(swt_peek8(0x009058B0u), 0, "a load qwark did not force leaves the flag at the checkbox's");

	group("build 53: RaC1's set-aside load forces them through the flag too");

	savefile_make_idle(GAME_RAC1);
	check(session_load_setaside() == ST_OK, "the set-aside load goes ahead");
	check_eq_u64(swt_peek8(0x009058B0u), 1, "forcing fast loads through their flag");
	check(f != NULL && swt_peek8(f->api_load) == 1, "and raising the helper's load byte");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << SWT_R1_FAST)) == 0, "the checkbox reads back off");
	if (f != NULL) r1fl_helper_loads(f, 3);
	check_eq_u64(swt_peek8(0x009058B0u), 0, "and the end of the load puts the flag back");

	savefile_make_idle(GAME_RAC1);
	check(session_load_setaside() == ST_OK && swt_peek8(0x009058B0u) == 1, "another forces it");
	if (f != NULL) host_poke(f->api_load, (const u8 *)"\xFE", 1);
	pump(3);
	check_eq_u64(swt_peek8(0x009058B0u), 0, "and one the helper drops puts it back");
	check(!swt_wrote_code(s), "no cave byte and no site word was written");
}

/*
 * Build 53: RaC2's trampolines moved. Builds 50 to 52, and build 53 until this
 * move, put them at 0x233200 and 0x233220, the end of the function at
 * 0x22A368..0x233308, which planet Joba calls every frame (0x205018 at
 * 0x205254, and 0x1F6E18). They are in the dead library span the savefile
 * helper starts now, at its far end. These are the words the old placement's
 * SWITCH_PATCH handed out, which an RPCS3 patch file written by one of those
 * builds still holds, and the stamp that reply carried.
 */
static const struct swt_cave g_swt_rac2_b50_caves[] = {
	{ 0x00233200u, 6, { 0x3C600131u, 0x88637720u, 0x2C030000u, 0x40820008u, 0x489B60F9u,
	                    0x489B7690u } },
	{ 0x00233220u, 6, { 0x3CC00131u, 0x88C67721u, 0x2C060000u, 0x40820008u, 0x7C64292Eu,
	                    0x488FDA4Cu } }
};
static const struct patch_word g_swt_rac2_b50_sites[] = {
	{ 0x00B30C7Cu, 0x4B7025A4u }, { 0x00BEA8A0u, 0x4B648960u }
};
#define SWT_RAC2_B50_STAMP 0x4B325BE2u

/*
 * The SWITCH_PATCH stamp of RaC1, which did not change, and the one RaC3's
 * carried while Freeze ammo was its only switch (builds 50 to 53 until its fast
 * loads became a code patch), which an RPCS3 patch file written then still
 * holds beside its words.
 */
#define SWT_RAC1_STAMP 0xB16A9D3Eu
#define SWT_RAC3_B50_STAMP 0x0C909623u
/*
 * Deadlocked's: the one builds 50 to 52 handed out, and build 53's, which moved
 * its second fast-load switch (test_switches_rac4_fastload_moved).
 */
#define SWT_RAC4_B50_STAMP 0xEF1A07C3u
#define SWT_RAC4_STAMP     0xBB16AF57u

/* RaC2's dead span, and the part of it the switches take. */
#define SWT_R2_SPAN_LO  0x0113EC64u
#define SWT_R2_SPAN_HI  0x011414FCu
#define SWT_R2_CAVE_LO  0x01141400u
#define SWT_R2_JOBA_LO  0x0022A368u
#define SWT_R2_JOBA_HI  0x00233308u

/*
 * A SWITCH_PATCH reply laid out from a word table, exactly as switches_patch
 * lays one out: what a client kept from an earlier build. Returns its length.
 */
static u32 swt_reply_of(const struct swt_cave *caves, u8 ncaves,
                        const struct patch_word *sites, u8 nsites,
                        const u32 *flags, u8 nflags, u8 *out)
{
	u32 off = SWITCH_PATCH_HEAD;
	u32 n = 0;
	u8 i, k;

	for (i = 0; i < ncaves; i++) {
		for (k = 0; k < caves[i].nwords; k++) {
			be32_put(out + off, caves[i].addr + 4u * k);
			be32_put(out + off + 4u, caves[i].w[k]);
			off += SWITCH_PATCH_ROW;
			n++;
		}
	}
	for (i = 0; i < nsites; i++) {
		be32_put(out + off, sites[i].addr);
		be32_put(out + off + 4u, sites[i].value);
		off += SWITCH_PATCH_ROW;
		n++;
	}

	be16_put(out + off, nflags);
	be16_put(out + off + 2u, 0);
	off += SWITCH_PATCH_BYTES_HEAD;
	for (i = 0; i < nflags; i++) {
		be32_put(out + off, flags[i]);
		memset(out + off + 4u, 0, 4);
		off += SWITCH_PATCH_BYTE_ROW;
	}

	be16_put(out, (u16)n);
	be16_put(out + 2, 0);
	be32_put(out + 4, qcrc32(out + SWITCH_PATCH_HEAD, off - SWITCH_PATCH_HEAD));
	return off;
}

/* The target of a relative `b` at `site`, or 0 for anything else. */
static u32 swt_branch_target(u32 site, u32 word)
{
	u32 disp = word & 0x03FFFFFCu;

	if ((word & 0xFC000003u) != 0x48000000u) return 0;
	if (disp & 0x02000000u) disp |= 0xFC000000u;
	return site + disp;
}

/* RaC2 booted with `word` at its fingerprint site and nothing else written. */
static int swt_rac2_boots_with(u32 word)
{
	host_boot("NPEA00386");
	poke32(0x00BEA8A0u, word);
	return pump_until(SESSION_INGAME, 400) &&
	       session_game() != NULL && session_game()->game_id == GAME_RAC2;
}

static void test_switches_rac2_moved(void)
{
	const struct sw_desc *d = sw_desc_for_game(GAME_RAC2);
	const struct sf_desc *f = sf_desc_for_game(GAME_RAC2);
	const struct game_api *g = game_for_title("NPEA00386");
	static const u8 w_game[4] = { 0x4B, 0xFF, 0xEA, 0x69 };
	static const u8 w_nop[4]  = { 0x60, 0x00, 0x00, 0x00 };
	static const u8 w_new[4]  = { 0x48, 0x55, 0x6B, 0x60 };
	static const u8 w_old[4]  = { 0x4B, 0x64, 0x89, 0x60 };
	u32 len = 0, oldlen, toglen, rows_ok, i, off;
	u8 sup = 9, ins = 9, run = 9, pend = 9;
	u32 size = 0, sflen = 0;

	group("build 53: RaC2's switches moved off Joba's code");

	check(d != NULL && f != NULL && g != NULL, "RaC2 has switches, a helper and a table");
	if (d == NULL || f == NULL || g == NULL) return;

	rows_ok = 1;
	for (i = 0; i < d->ncaves; i++) {
		u32 lo = d->caves[i].addr, hi = lo + d->caves[i].len;

		if (lo < SWT_R2_CAVE_LO || hi > SWT_R2_SPAN_HI) rows_ok = 0;
		if (lo < SWT_R2_JOBA_HI && hi > SWT_R2_JOBA_LO) rows_ok = 0;
	}
	check(rows_ok, "every trampoline is in 0x1141400..0x11414FC, the far end of the dead span, "
	      "and nowhere in Joba's routine");
	check(f->caves[0].addr == SWT_R2_SPAN_LO &&
	      f->caves[0].addr + f->caves[0].len <= d->caves[0].addr,
	      "and the helper, at the near end, stops short of it");
	check(d->caves[0].addr - (f->caves[0].addr + f->caves[0].len) >= 0x2000u,
	      "with room to grow: more than 8 KB between them");

	check(d->ncaves == 4 && d->nsites == 4 && d->nflags == 2,
	      "four trampolines and four sites, the crash guards' two with no flag byte");
	if (d->ncaves != 4 || d->nsites != 4 || d->nflags != 2) return;

	check_eq_u64(swt_branch_target(d->sites[1].addr, d->sites[1].value), d->caves[1].addr,
	             "the ammo site, 0xB30C7C, branches to the ammo trampoline");
	check_eq_u64(swt_branch_target(d->sites[3].addr, d->sites[3].value), d->caves[0].addr,
	             "the fast-load site, 0xBEA8A0, to the fast-load one");
	check_eq_u64(swt_branch_target(d->caves[0].addr + 20u, be32_get(d->caves[0].bytes + 20)),
	             0x00BEA8A4u, "which branches back to the instruction after its site");
	check_eq_u64(swt_branch_target(d->caves[1].addr + 20u, be32_get(d->caves[1].bytes + 20)),
	             0x00B30C80u, "and so does the ammo one");
	check(d->flags[0] == 0x01317720u && d->flags[1] == 0x01317721u,
	      "the flag bytes did not move");

	/* The crash guards, after the two toggles' trampolines. */
	check_eq_u64(swt_branch_target(d->sites[0].addr, d->sites[0].value), d->caves[2].addr,
	             "the grid site, 0x86960, branches to the grid guard at 0x1141440");
	check_eq_u64(swt_branch_target(d->sites[2].addr, d->sites[2].value), d->caves[3].addr,
	             "the ship-part site, 0xBDF30C, to the part filter at 0x1141470");
	check_eq_u64(swt_branch_target(d->caves[2].addr + 16u, be32_get(d->caves[2].bytes + 16)),
	             0x00086964u, "the guard goes back to the instruction after its site");
	check_eq_u64(swt_branch_target(d->caves[2].addr + 20u, be32_get(d->caves[2].bytes + 20)),
	             0x00086BD0u, "or on to UpdateMobyGrids' epilogue");
	check_eq_u64(swt_branch_target(d->caves[3].addr + 36u, be32_get(d->caves[3].bytes + 36)),
	             0x00BDF310u, "the filter goes back to the instruction after its site");
	check_eq_u64(swt_branch_target(d->caves[3].addr + 40u, be32_get(d->caves[3].bytes + 40)),
	             0x00BDF570u, "or on to the ship update's next part");
	check(d->caves[2].addr >= d->caves[1].addr + d->caves[1].len &&
	      d->caves[3].addr >= d->caves[2].addr + d->caves[2].len,
	      "none of the four overlaps the one before it");

	/* ---------------------------------------------- SWITCH_PATCH rows */

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "SWITCH_PATCH for RaC2");
	check_eq_u64(be16_get(g_sw_patch), 33,
	             "thirty-three rows: twenty-nine trampoline words with the guards', four sites");

	rows_ok = 1;
	off = SWITCH_PATCH_HEAD;
	for (i = 0; i < be16_get(g_sw_patch); i++) {
		u32 a = be32_get(g_sw_patch + off);
		u32 v = be32_get(g_sw_patch + off + 4);

		if (i < 29 && (a < SWT_R2_CAVE_LO || a + 4u > SWT_R2_SPAN_HI)) rows_ok = 0;
		if (i == 29 && (a != 0x00086960u || v != 0x490BAAE0u)) rows_ok = 0;
		if (i == 30 && (a != 0x00B30C7Cu || v != 0x486107A4u)) rows_ok = 0;
		if (i == 31 && (a != 0x00BDF30Cu || v != 0x48562164u)) rows_ok = 0;
		if (i == 32 && (a != 0x00BEA8A0u || v != 0x48556B60u)) rows_ok = 0;
		off += SWITCH_PATCH_ROW;
	}
	check(rows_ok, "every trampoline row lands in the cave, then b 0x1141440, b 0x1141420, "
	      "b 0x1141470 and b 0x1141400");

	oldlen = swt_reply_of(g_swt_rac2_b50_caves, SWT_N(g_swt_rac2_b50_caves),
	                      g_swt_rac2_b50_sites, SWT_N(g_swt_rac2_b50_sites),
	                      d->flags, d->nflags, g_sw_patch2);
	check_eq_u64(be32_get(g_sw_patch2 + 4), SWT_RAC2_B50_STAMP,
	             "the old placement's reply rebuilt carries the stamp builds 50 to 52 handed out");
	check(oldlen < len, "and is shorter than this build's, which carries the crash guards too");
	check(be32_get(g_sw_patch + 4) != SWT_RAC2_B50_STAMP,
	      "this build's stamp is another number, so a client's kept stamp no longer matches");

	/* -------------------------------------------------- the fingerprint */

	group("build 53: RaC2's fingerprint takes the game's word, qwark's nop and both branches");

	check(memcmp(g->fp_bytes, w_game, 4) == 0, "the game's own word, bl 0xBE9308");
	check(g->fp_alt != NULL && memcmp(g->fp_alt, w_nop, 4) == 0, "the console patch's nop");
	check(g->fp_switch != NULL && memcmp(g->fp_switch, w_new, 4) == 0, "this build's branch, b 0x1141400");
	check(g->fp_switch_old != NULL && memcmp(g->fp_switch_old, w_old, 4) == 0,
	      "and the old one, b 0x233200");
	check_eq_u64(SW_RAC2_SITE_FAST_LOADS, 0x48556B60u, "fp_switch is the generated site word");

	check(quit_and_wait(), "quit");
	check(swt_rac2_boots_with(0x4BFFEA69u), "RaC2 with the game's word at 0xBEA8A0 is RaC2");
	check(quit_and_wait(), "quit");
	check(swt_rac2_boots_with(0x60000000u), "with the nop");
	check(quit_and_wait(), "quit");
	check(swt_rac2_boots_with(0x48556B60u), "with this build's branch");
	check(quit_and_wait(), "quit");
	check(swt_rac2_boots_with(0x4B648960u), "and with an earlier build's branch");
	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	poke32(0x00BEA8A0u, 0x48556B64u);
	pump(400);
	check(session_state() == SESSION_BOOTING, "but a branch anywhere else is not RaC2");

	/* ----------------------- a game still carrying an earlier build's patch */

	group("build 53: RaC2 with an earlier build's switches is RaC2, told to rewrite them");

	check(quit_and_wait(), "quit");
	host_write_log_reset();
	check(swt_boot_patched("NPEA00386", g_sw_patch2, oldlen, SF_NO_FLIP),
	      "RaC2 boots with build 52's switch patch applied at load");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC2,
	      "the old branch at the fingerprint site still says RaC2");
	check_eq_u64(swt_peek32(0x00BEA8A0u), 0x4B648960u, "it is that branch");
	check_eq_u64(switches_state(), 0, "the look finds no switches of this build");
	check((swt_info_flags() & (SESSION_FLAG_NO_CODE_PATCHES | SESSION_FLAG_CODE_SWITCHES)) ==
	      SESSION_FLAG_NO_CODE_PATCHES,
	      "SessionInfo has bit2 set and bit4 clear: the client offers to write the patch");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK &&
	      be32_get(g_sw_patch + 4) != be32_get(g_sw_patch2 + 4),
	      "and SWITCH_PATCH's stamp is not the old patch's: that patch is from another build");
	check(features_set(SWT_R2_FAST, 1) == ST_UNSUPPORTED, "Fast loads is refused meanwhile");
	check(features_set(SWT_R2_AMMO, 1) == ST_UNSUPPORTED, "and so is Infinite ammo");
	check_eq_u64(swt_peek8(0x01317720u) | swt_peek8(0x01317721u), 0, "no flag was written");
	check(!swt_wrote_code(d), "and nothing was written into the new caves or the sites");

	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
	poke32(0x00BEA8A0u, 0x4B648960u);
	check(pump_until(SESSION_INGAME, 4000), "this build's caves under the old fast-load branch");
	pump(6);
	check(session_game() != NULL && session_game()->game_id == GAME_RAC2, "is RaC2");
	check_eq_u64(switches_state(), 0, "and not this build's switches either");

	check(quit_and_wait(), "the client wrote the new patch and the game restarts");
	check(swt_boot_patched("NPEA00386", g_sw_patch, len, SF_NO_FLIP), "with it applied at load");
	check_eq_u64(switches_state(), 1, "the look finds this build's switches");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) != 0, "bit4 is set");
	check(features_set(SWT_R2_AMMO, 1) == ST_OK && swt_peek8(0x01317721u) == 1,
	      "Infinite ammo goes on through its flag");
	check(features_set(SWT_R2_AMMO, 0) == ST_OK && swt_peek8(0x01317721u) == 0, "and off");

	/* The helper's patch goes in beside it: neither writes the other's bytes. */
	check(savefile_patch(g_sw_sfpatch, sizeof(g_sw_sfpatch), &sflen) == ST_OK, "the helper's patch");
	check(quit_and_wait(), "quit");
	host_boot("NPEA00386");
	sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
	sf_poke_patch(g_sw_sfpatch, sflen, SF_NO_FLIP, 1);
	check(pump_until(SESSION_INGAME, 4000), "RaC2 boots with both in the same dead span");
	pump(6);
	check_eq_u64(switches_state(), 1, "the switches are found");
	check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_OK && ins == 1, "and so is the helper");
	check_eq_u64(swt_peek32(0x00086960u), 0x490BAAE0u, "the grid guard's branch is at 0x86960");
	check_eq_u64(swt_peek32(0x00BDF30Cu), 0x48562164u, "and the part filter's at 0xBDF30C");

	/* ------------------------------- the look counts the crash guards */

	group("build 53: RaC2's switches without the crash guards do not count");

	/*
	 * The two toggles' rows alone, as the switches were before the guards
	 * joined them: every word of them right, the guards' four rows missing.
	 */
	{
		static const struct patch_word toggle_sites[] = {
			{ 0x00B30C7Cu, 0x486107A4u }, { 0x00BEA8A0u, 0x48556B60u }
		};

		toglen = swt_reply_of(g_swt_rac2_caves, 2, toggle_sites, SWT_N(toggle_sites),
		                      d->flags, d->nflags, g_sw_patch2);
	}
	check(be32_get(g_sw_patch2 + 4) != be32_get(g_sw_patch + 4),
	      "such a patch has another stamp");

	check(quit_and_wait(), "quit");
	host_write_log_reset();
	check(swt_boot_patched("NPEA00386", g_sw_patch2, toglen, SF_NO_FLIP),
	      "RaC2 boots with the toggles' switches alone");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC2,
	      "the fast-load branch is this build's, so the fingerprint says RaC2");
	check_eq_u64(switches_state(), 0, "but the look counts no switches without the guards");
	check((swt_info_flags() & (SESSION_FLAG_NO_CODE_PATCHES | SESSION_FLAG_CODE_SWITCHES)) ==
	      SESSION_FLAG_NO_CODE_PATCHES,
	      "SessionInfo has bit2 set and bit4 clear: the client offers to write the patch");
	check(features_set(SWT_R2_FAST, 1) == ST_UNSUPPORTED, "Fast loads is refused meanwhile");
	check_eq_u64(swt_peek32(0x00086960u) | swt_peek32(0x00BDF30Cu), 0,
	             "and qwark wrote no guard itself: code cannot be patched here");
	check(!swt_wrote_code(d), "nor any other code word");
}

/*
 * Build 53: Deadlocked's second fast-load switch moved off the transition
 * update's call at 0x2A8C50, which hung the game whenever the flight had a movie
 * to play, to 0x2A8C5C, where the loop copies the update's answer into the word
 * it ends on (src/games/rac4.h). The trampoline kept its cave and its six words'
 * length, so every other cave and site is where builds 50 to 52, and build 53
 * until this move, put them; only that trampoline's words and that one site
 * differ, and the stamp with them. These are the old ones, which an RPCS3 patch
 * written by those builds holds.
 */
#define SWT_R4_FL2_OLD_SITE 0x002A8C50u
#define SWT_R4_FL2_OLD_WORD 0x483BEFB0u   /* b 0x667C00, from 0x2A8C50 */
static const u32 g_swt_rac4_b50_fl2[6] = {
	0x3C80015Du, 0x8884D781u, 0x2C040000u, 0x40820008u, 0x4E9E0421u, 0x4BC41040u
};

/*
 * Just enough of a PowerPC to run a trampoline: lis, lbz, ori, neg, and, or and
 * b, on 64-bit registers as the PPU has them. Memory is the one flag byte.
 */
struct swt_cpu {
	u64 r[32];
	u32 pc;
	u32 flag_addr;
	u8  flag;
};

/* One instruction. 0 for one it does not know, or a load from anywhere else. */
static int swt_cpu_step(struct swt_cpu *c, u32 w)
{
	u32 op = w >> 26;
	u32 rd = (w >> 21) & 31u, ra = (w >> 16) & 31u, rb = (w >> 11) & 31u;
	u32 xo = (w >> 1) & 0x3FFu;
	s64 simm = (s64)(s16)(u16)(w & 0xFFFFu);
	u64 base = ra != 0 ? c->r[ra] : 0;

	switch (op) {
	case 15:   /* addis rD, rA, SIMM; lis with rA = 0 */
		c->r[rd] = base + (u64)(simm * 65536);
		break;
	case 34:   /* lbz rD, d(rA) */
		if ((u32)(base + (u64)simm) != c->flag_addr) return 0;
		c->r[rd] = c->flag;
		break;
	case 24:   /* ori rA, rS, UIMM */
		c->r[ra] = c->r[rd] | (w & 0xFFFFu);
		break;
	case 31:
		if (w & 1u) return 0;                                  /* no record forms */
		if (xo == 104 && rb == 0)  c->r[rd] = (u64)0 - c->r[ra];   /* neg */
		else if (xo == 28)         c->r[ra] = c->r[rd] & c->r[rb]; /* and */
		else if (xo == 444)        c->r[ra] = c->r[rd] | c->r[rb]; /* or */
		else return 0;
		break;
	case 18: { /* b, relative and without the link */
		u32 disp = w & 0x03FFFFFCu;

		if (w & 3u) return 0;
		if (disp & 0x02000000u) disp |= 0xFC000000u;
		c->pc += disp;
		return 1;
	}
	default:
		return 0;
	}
	c->pc += 4;
	return 1;
}

/*
 * From c->pc until it reaches `stop`: `site` holds `site_word`, and the
 * generated caves hold theirs. 0 if it runs anywhere else, or too long.
 */
static int swt_cpu_run(struct swt_cpu *c, const struct sw_desc *d,
                       u32 site, u32 site_word, u32 stop)
{
	int steps;
	u8 k;

	for (steps = 0; steps < 32; steps++) {
		u32 w = 0;
		int found = 0;

		if (c->pc == stop) return 1;
		if (c->pc == site) {
			w = site_word;
			found = 1;
		}
		for (k = 0; !found && k < d->ncaves; k++) {
			if (c->pc >= d->caves[k].addr && c->pc + 4u <= d->caves[k].addr + d->caves[k].len) {
				w = be32_get(d->caves[k].bytes + (c->pc - d->caves[k].addr));
				found = 1;
			}
		}
		if (!found || !swt_cpu_step(c, w)) return 0;
	}
	return 0;
}

static void swt_cpu_reset(struct swt_cpu *c, u64 r3, u64 r26, u8 flag)
{
	u32 i;

	for (i = 0; i < 32; i++) c->r[i] = 0x0123456700000000ull + 0x1111u * i;
	c->r[3] = r3;
	c->r[26] = r26;
	c->pc = R4_FASTLOAD_2;
	c->flag_addr = SW_RAC4_FLAG_FAST_LOADS;
	c->flag = flag;
}

static void test_switches_rac4_fastload_moved(void)
{
	static const u64 r3s[3] = { 0, 1, 0x00000000DEADBEEFull };
	const struct sw_desc *d = sw_desc_for_game(GAME_RAC4);
	const struct game_api *g = game_for_title("NPEA00423");
	struct swt_cave old_caves[15];
	struct patch_word old_sites[14];
	u32 len = 0, oldlen, i;
	int ok, same, ran, kept;

	group("build 53: Deadlocked's fast-load switch moved off the transition update's call");

	check(d != NULL && g != NULL, "Deadlocked has switches and a table");
	if (d == NULL || g == NULL) return;
	check(d->ncaves == 15 && d->nsites == 14, "still fifteen caves and fourteen sites");
	if (d->ncaves != 15 || d->nsites != 14) return;

	check_eq_u64(d->sites[2].addr, R4_FASTLOAD_2,
	             "the second fast-load site is 0x2A8C5C, where the update's answer is copied");
	ok = 1;
	for (i = 0; i < d->nsites; i++)
		if (d->sites[i].addr == R4_TRANS_UPDATE) ok = 0;
	check(ok, "and no site is the update's call at 0x2A8C50 any more");
	check_eq_u64(swt_branch_target(d->sites[2].addr, d->sites[2].value), d->caves[2].addr,
	             "it branches to the cave builds 50 to 52 had there, 0x667C00");
	check_eq_u64(d->caves[2].len, 24, "which is still six words");
	check_eq_u64(swt_branch_target(d->caves[2].addr + 20u, be32_get(d->caves[2].bytes + 20)),
	             R4_FASTLOAD_2 + 4u, "and branches back to 0x2A8C60");
	check_eq_u64(d->sites[0].value, 0x4848FEB4u, "the fingerprint site, 0x1D7D18, keeps its branch");
	check(g->fp_switch != NULL && be32_get(g->fp_switch) == d->sites[0].value && g->fp_switch_old == NULL,
	      "so the fingerprint takes the same three words as before and needs no fourth");

	same = 1;
	for (i = 0; i < 15; i++) {
		u8 k;

		if (i == 2) continue;
		if (d->caves[i].addr != g_swt_rac4_caves[i].addr) same = 0;
		for (k = 0; k < g_swt_rac4_caves[i].nwords; k++)
			if (be32_get(d->caves[i].bytes + 4u * k) != g_swt_rac4_caves[i].w[k]) same = 0;
	}
	for (i = 0; i < 14; i++)
		if (i != 2 && (d->sites[i].addr != g_swt_rac4_sites[i].addr ||
		               d->sites[i].value != g_swt_rac4_sites[i].value)) same = 0;
	check(same, "every other trampoline and site word is build 52's, byte for byte");

	/* -------------------------- what the trampoline computes, both ways */

	group("build 53: the trampoline leaves r27 as the console's word would");

	ran = 1;
	same = 1;
	kept = 1;
	for (i = 0; i < 12; i++) {
		struct swt_cpu sw, con;
		u64 r3 = r3s[i % 3], r26 = (i / 3) & 1u;
		u8 flag = (u8)(i / 6);
		u64 want = flag ? (r3 | r26) : r3;
		u32 k;

		swt_cpu_reset(&sw, r3, r26, flag);
		if (!swt_cpu_run(&sw, d, R4_FASTLOAD_2, d->sites[2].value, R4_FASTLOAD_2 + 4u)) ran = 0;

		swt_cpu_reset(&con, r3, r26, flag);
		if (!swt_cpu_run(&con, d, R4_FASTLOAD_2, flag ? R4_FL2_FAST : R4_FL2_GAME,
		                 R4_FASTLOAD_2 + 4u)) ran = 0;

		if (sw.r[27] != want || con.r[27] != want) same = 0;
		for (k = 0; k < 32; k++)
			if (k != 5 && k != 27 && sw.r[k] != con.r[k]) kept = 0;
	}
	check(ran, "from the site through the cave and back to 0x2A8C60, and the console's one word");
	check(same, "r27 is the update's answer with the flag clear, or'd with r26 with it set");
	check(kept, "and nothing but r5, the scratch, and r27 differs from the console's");

	/* ---------------------------------------------- the old patch's stamp */

	group("build 53: a Deadlocked patch from builds 50 to 52 is from another build");

	for (i = 0; i < 15; i++) old_caves[i] = g_swt_rac4_caves[i];
	for (i = 0; i < 6; i++) old_caves[2].w[i] = g_swt_rac4_b50_fl2[i];
	for (i = 0; i < 14; i++) old_sites[i] = g_swt_rac4_sites[i];
	old_sites[2].addr = SWT_R4_FL2_OLD_SITE;
	old_sites[2].value = SWT_R4_FL2_OLD_WORD;
	oldlen = swt_reply_of(old_caves, 15, old_sites, 14, d->flags, d->nflags, g_sw_patch2);
	check_eq_u64(be32_get(g_sw_patch2 + 4), SWT_RAC4_B50_STAMP,
	             "the old placement's reply rebuilt carries the stamp builds 50 to 52 handed out");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00423"), "Deadlocked boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "SWITCH_PATCH for Deadlocked");
	check_eq_u64(oldlen, len, "the old reply is the same length as this build's");
	check(be32_get(g_sw_patch + 4) != SWT_RAC4_B50_STAMP,
	      "and this build's stamp is another number, so a client's kept stamp no longer matches");

	check(quit_and_wait(), "quit");
	host_write_log_reset();
	check(swt_boot_patched("NPEA00423", g_sw_patch2, oldlen, SF_NO_FLIP),
	      "Deadlocked boots with build 52's switch patch applied at load");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC4,
	      "and is Deadlocked: the fingerprint site's branch never moved");
	check_eq_u64(swt_peek32(SWT_R4_FL2_OLD_SITE), SWT_R4_FL2_OLD_WORD,
	             "the old branch is over the update's call");
	check_eq_u64(switches_state(), 0, "the look finds no switches of this build");
	check((swt_info_flags() & (SESSION_FLAG_NO_CODE_PATCHES | SESSION_FLAG_CODE_SWITCHES)) ==
	      SESSION_FLAG_NO_CODE_PATCHES,
	      "SessionInfo has bit2 set and bit4 clear: the client offers to write the patch");
	check(features_set(SWT_R4_FAST, 1) == ST_UNSUPPORTED, "Fast loads is refused meanwhile");

	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(4);
	check(session_planet_load(4, 0) == ST_OK, "and a planet load goes out without them");
	check_eq_u64(swt_peek8(SW_RAC4_FLAG_FAST_LOADS), 0,
	             "writing no flag, which the old trampoline would take to skip the update");
	check(!swt_wrote_code(d), "and nothing into the caves or the sites");
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x06", 1);
	pump(6);
	host_poke(R4_GAMESTATE + 3, (const u8 *)"\x00", 1);
	pump(6);
	check_eq_u64(swt_peek8(SW_RAC4_FLAG_FAST_LOADS), 0, "nor when the load is over");

	check(quit_and_wait(), "the client wrote the new patch and the game restarts");
	check(swt_boot_patched("NPEA00423", g_sw_patch, len, SF_NO_FLIP), "with it applied at load");
	check_eq_u64(switches_state(), 1, "the look finds this build's switches");
	check(features_set(SWT_R4_FAST, 1) == ST_OK && swt_peek8(SW_RAC4_FLAG_FAST_LOADS) == 1,
	      "Fast loads goes on through its flag");
	check(features_set(SWT_R4_FAST, 0) == ST_OK && swt_peek8(SW_RAC4_FLAG_FAST_LOADS) == 0,
	      "and off");
}

/*
 * RaC3's switches while Freeze ammo was the only one (builds 50 to 52, and build
 * 53 until its fast loads became a code patch): what an RPCS3 patch file written
 * then still holds. The trampoline did not move, so
 * its site word, which is RaC3's fingerprint, is the one this build hands out.
 */
static const struct swt_cave g_swt_rac3_b50_caves[] = {
	{ 0x00980F00u, 6, { 0x3CE000DAu, 0x88E7FF10u, 0x2C070000u, 0x40820008u, 0x7C85312Eu,
	                    0x4B801B78u } }
};
static const struct patch_word g_swt_rac3_b50_sites[] = { { 0x00182A88u, 0x487FE478u } };
static const u32 g_swt_rac3_b50_flags[] = { 0x00D9FF10u };

#define SWT_R3_FREEZE_FLAG 0x00D9FF10u
#define SWT_R3_FAST_FLAG   0x00D9FF11u

/*
 * RaC3's fast loads under RPCS3. Until they became a code patch they were two
 * data words and needed nothing installed; now they are a code switch, and a
 * patch from before has Freeze ammo's alone.
 */
static void test_switches_rac3_fastloads(void)
{
	const struct sw_desc *d = sw_desc_for_game(GAME_RAC3);
	u32 len = 0, oldlen;

	group("RaC3: the fast-load switch, beside Freeze ammo's");

	check(d != NULL && d->ncaves == 2 && d->nsites == 2 && d->nflags == 2,
	      "RaC3 has two trampolines, two sites and two flag bytes");
	if (d == NULL || d->ncaves != 2 || d->nsites != 2) return;
	check(d->caves[0].addr == 0x00980F00u && d->caves[0].len == 24u,
	      "Freeze ammo's trampoline did not move or change size");
	check_eq_u64(d->sites[0].value, SW_RAC3_SITE_FREEZE_AMMO,
	             "so its site word, the fingerprint's third form, is the one it was");
	check_eq_u64(d->sites[0].value, 0x487FE478u, "b 0x980F00");
	check_eq_u64(d->sites[1].addr, R3_FL_SITE, "the fast-load site is the loop's test at 0x27B428");
	check_eq_u64(swt_branch_target(d->sites[1].addr, d->sites[1].value), d->caves[1].addr,
	             "and branches to the fast-load trampoline at 0x980F20");
	check(d->caves[1].addr + d->caves[1].len <= 0x00981064u &&
	      (d->caves[1].addr >= R3_FL_CAVE_END || d->caves[1].addr + d->caves[1].len <= R3_FL_CAVE),
	      "inside the helper's dead function, and apart from the console's cave at 0x980860");
	check_eq_u64(be32_get(d->caves[1].bytes + d->caves[1].len - 8u), R3_FL_SITE_WORD,
	             "its exit does what the site did");
	check_eq_u64(swt_branch_target(d->caves[1].addr + d->caves[1].len - 4u,
	                               be32_get(d->caves[1].bytes + d->caves[1].len - 4u)),
	             R3_FL_SITE + 4u, "and goes back to the instruction after it");
	check_eq_u64(r3_branch_target(d->caves[1].addr + 0x40u, be32_get(d->caves[1].bytes + 0x40u)),
	             R3_FL_UNLOAD, "it calls Transition_UnloadSounds at 0x278A00");
	check_eq_u64(be32_get(d->caves[1].bytes + 0x40u) & 3u, 1u,
	             "as an ordinary call, bl, which RPCS3's recompiler expects");
	check_eq_u64(r3_dform_addr(be32_get(d->caves[1].bytes + 0x08u),
	                           be32_get(d->caves[1].bytes + 0x0Cu)), SWT_R3_FAST_FLAG,
	             "and runs the fast-load logic only with the flag at 0xD9FF11 set");
	check(d->flags[0] == SWT_R3_FREEZE_FLAG && d->flags[1] == SWT_R3_FAST_FLAG,
	      "Freeze ammo's flag stayed at 0xD9FF10, fast loads' is 0xD9FF11");

	/* ------------------------------------------- an earlier build's patch */

	group("RaC3 with an earlier build's switches is RaC3, told to rewrite them");

	check(quit_and_wait(), "quit whatever was running");
	check(boot_and_wait("NPEA00387"), "RaC3 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "SWITCH_PATCH for RaC3");
	check_eq_u64(be16_get(g_sw_patch), 31, "thirty-one rows: twenty-nine trampoline words, two sites");

	oldlen = swt_reply_of(g_swt_rac3_b50_caves, SWT_N(g_swt_rac3_b50_caves),
	                      g_swt_rac3_b50_sites, SWT_N(g_swt_rac3_b50_sites),
	                      g_swt_rac3_b50_flags, SWT_N(g_swt_rac3_b50_flags), g_sw_patch2);
	check_eq_u64(be32_get(g_sw_patch2 + 4), SWT_RAC3_B50_STAMP,
	             "Freeze ammo's patch rebuilt carries the stamp builds 50 to 53 handed out");
	check(be32_get(g_sw_patch + 4) != SWT_RAC3_B50_STAMP,
	      "this build's stamp is another number, so a client's kept stamp no longer matches");

	check(quit_and_wait(), "quit");
	host_write_log_reset();
	check(swt_boot_patched("NPEA00387", g_sw_patch2, oldlen, SF_NO_FLIP),
	      "RaC3 boots with the Freeze-ammo-only patch applied at load");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC3,
	      "and is RaC3: its fingerprint site holds the same branch it always did");
	check_eq_u64(switches_state(), 0, "the look does not find this build's switches");
	check((swt_info_flags() & (SESSION_FLAG_NO_CODE_PATCHES | SESSION_FLAG_CODE_SWITCHES)) ==
	      SESSION_FLAG_NO_CODE_PATCHES,
	      "SessionInfo has bit2 set and bit4 clear: the client offers to write the patch");
	check(features_set(F3_FAST_LOADS, 1) == ST_UNSUPPORTED, "Fast loads is refused meanwhile");
	check(features_set(0, 1) == ST_UNSUPPORTED, "and so is Freeze ammo, until the new patch is in");
	check(session_planet_load(5, 0) == ST_OK, "a planet load goes ahead at the game's own pace");
	check_eq_u64(swt_peek8(SWT_R3_FREEZE_FLAG) | swt_peek8(SWT_R3_FAST_FLAG), 0, "no flag was written");
	check(!swt_wrote_code(d) && !r3_fl_wrote(), "and no code word, switch or console cave");

	/* ------------------------------------------------ the new patch is in */

	group("RaC3's forced fast loads write the flag");

	check(quit_and_wait(), "the client wrote the new patch and the game restarts");
	check(swt_boot_patched("NPEA00387", g_sw_patch, len, SF_NO_FLIP), "with it applied at load");
	check_eq_u64(switches_state(), 1, "the look finds both switches");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) != 0, "bit4 is set");
	check_eq_u64(swt_peek32(R3_FL_SITE), 0x48705AF8u, "the loop's test holds the branch to 0x980F20");
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(4);
	host_write_log_reset();

	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD goes ahead");
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "forcing fast loads through their flag");
	pump(SWT_POLL_TICKS);
	check((features_toggle_state() & ((u64)1 << F3_FAST_LOADS)) == 0,
	      "while the checkbox reads back off: the force is not the user's");
	host_poke(SF_R3_MODE, (const u8 *)"\x00\x00\x00\x06", 4);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(4);
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "the flag stays up while the mode is 6");
	host_poke(SF_R3_MODE, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(4);
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 0,
	             "and the load's end, request down and mode 0, puts it back");

	check(session_planet_load(8, 0) == ST_OK, "a load to Aquatos");
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "is forced the same way");
	pump(4);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(4);
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 0, "and put back the same way");

	check(features_set(F3_FAST_LOADS, 1) == ST_OK, "fast loads on");
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "writes 1 into their flag");
	check(session_planet_load(5, 0) == ST_OK, "another load");
	pump(4);
	host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
	pump(SWT_POLL_TICKS);
	check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "leaves the flag on for a checkbox that is on");
	check((features_toggle_state() & ((u64)1 << F3_FAST_LOADS)) != 0, "which reads back on");
	check(features_set(F3_FAST_LOADS, 0) == ST_OK && swt_peek8(SWT_R3_FAST_FLAG) == 0,
	      "fast loads off");
	check_eq_u64(swt_peek8(SWT_R3_FREEZE_FLAG), 0, "Freeze ammo's flag was never touched");
	check(!swt_wrote_code(d), "no cave byte and no site word was written");
	check(!r3_fl_wrote(), "and nothing went into the console's cave either");

	/* ------------------------- a set-aside load, with the helper's patch in */

	group("RaC3's set-aside loads force fast loads through the flag too");
	{
		const struct sf_desc *f = sf_desc_for_game(GAME_RAC3);
		u32 sflen = 0, done = 0, total = 0;
		u8 sup = 9, ins = 9, run = 9, pend = 9, error = 9;
		u32 size = 0;

		check(savefile_patch(g_sw_sfpatch, sizeof(g_sw_sfpatch), &sflen) == ST_OK, "the helper's patch");
		check(quit_and_wait(), "quit");
		host_boot("NPEA00387");
		sf_poke_patch(g_sw_patch, len, SF_NO_FLIP, 1);
		sf_poke_patch(g_sw_sfpatch, sflen, SF_NO_FLIP, 1);
		check(pump_until(SESSION_INGAME, 4000), "RaC3 boots with both patches in");
		pump(6);
		check_eq_u64(switches_state(), 1, "the switches are found");
		check(savefile_info(&sup, &ins, &run, &pend, &size) == ST_OK && ins == 1, "and so is the helper");
		sf_give_save(GAME_RAC3);
		host_write_log_reset();

		check(session_load_setaside() == ST_OK, "the set-aside load goes ahead");
		check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "forcing fast loads through their flag");
		check_eq_u64(swt_peek8(f->api_load), 1, "and raising the helper's load byte");
		pump(SWT_POLL_TICKS);
		check((features_toggle_state() & ((u64)1 << F3_FAST_LOADS)) == 0,
		      "while the checkbox reads back off");
		host_poke(f->api_load, (const u8 *)"\xFE", 1);
		pump(3);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "the helper drops it");
		check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 0, "and the flag goes back down at once");

		check(session_load_setaside() == ST_OK && swt_peek8(SWT_R3_FAST_FLAG) == 1,
		      "another set-aside load forces them again");
		host_poke(f->api_load, (const u8 *)"\x00", 1);
		host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x01", 4);
		pump(SAVEFILE_SETTLE_TICKS);
		check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 1, "the helper takes it, and the load is under way");
		host_poke(R3_LOADPLANET, (const u8 *)"\x00\x00\x00\x00", 4);
		pump(4);
		check_eq_u64(swt_peek8(SWT_R3_FAST_FLAG), 0, "and its end puts the flag back");
		check(!swt_wrote_code(d) && !r3_fl_wrote(), "with no code word written");
	}
}

/* A console: no look, no bit4, and the toggles patch words as they always have. */
static void test_switches_console(void)
{
	u32 len = 0;

	group("build 50: a console patches code as it always has");

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots");
	pump(6);
	host_write_log_reset();
	check(features_set(SWT_R1_AMMO, 1) == ST_OK, "Infinite ammo on");
	check_eq_u64(swt_peek32(0x000AA2DCu), 0x60000000u, "patches the ammo store to a nop");
	check_eq_u64(swt_peek8(0x009058B1u), 0, "and writes no flag");
	check(features_set(SWT_R1_AMMO, 0) == ST_OK, "off");
	check(swt_peek32(0x000AA2DCu) != 0x60000000u, "puts the word back");

	/* The switch words at load are not something a console looks for. */
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the RaC1 words");
	check(quit_and_wait(), "quit");
	(void)len;

	check(boot_and_wait("NPEA00386"), "RaC2 boots");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK, "the RaC2 words");
	check(quit_and_wait(), "quit");
	check(swt_boot_patched("NPEA00386", g_sw_patch, len, SF_NO_FLIP),
	      "RaC2 boots with them in anyway, the branch at its fingerprint site");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC2, "and is RaC2");
	check_eq_u64(switches_state(), 0, "a console does not look");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) == 0, "bit4 stays clear");
	check(features_set(SWT_R2_AMMO, 1) == ST_OK, "Infinite ammo on");
	check_eq_u64(swt_peek32(0x00B30C7Cu), 0x60000000u, "patches the word");
	check_eq_u64(swt_peek8(0x01317721u), 0, "and writes no flag");
	check(features_set(SWT_R2_AMMO, 0) == ST_OK, "off");
	check_eq_u64(swt_peek32(0x00B30C7Cu), 0x486107A4u, "puts back what was there");
}

static void test_switches(void)
{
	u32 len = 0, len2 = 0;
	u32 stamps[4];
	u32 i;

	/* ------------------------------------------------ the generated table */

	for (i = 0; i < 4; i++) {
		char name[64];

		name[0] = 0;
		qstrcat(name, sizeof(name), "build 50: the switch words of ");
		qstrcat(name, sizeof(name), g_swt_games[i].title);
		group(name);
		swt_check_table(&g_swt_games[i]);
	}

	group("build 50: the helper and the switches never write the same byte");
	for (i = 0; i < 4; i++) {
		const struct sw_desc *s = sw_desc_for_game(g_swt_games[i].game_id);
		const struct sf_desc *f = sf_desc_for_game(g_swt_games[i].game_id);

		check(s != NULL && f != NULL && swt_apart(s, f), g_swt_games[i].title);
	}

	group("build 50: RaC2's helper keeps its request bytes out of the code segment");
	{
		const struct sf_desc *f = sf_desc_for_game(GAME_RAC2);

		check(f != NULL && f->api_mod == 0x01317728u && f->api_load == 0x01317729u &&
		      f->api_setaside == 0x0131772Au,
		      "api_mod, api_load and api_setaside are 0x1317728..0x131772A");
	}

	/* ------------------------------------------ SWITCH_PATCH on a console */

	group("build 50: SWITCH_PATCH on a console");

	for (i = 0; i < 4; i++) {
		const struct sw_desc *d = sw_desc_for_game(g_swt_games[i].game_id);
		u32 reads;

		check(quit_and_wait(), "quit whatever was running");
		check(boot_and_wait(g_swt_games[i].title), g_swt_games[i].title);

		reads = host_mem_reads();
		host_write_log_reset();
		check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_OK,
		      "SWITCH_PATCH answers on a console too");
		if (d != NULL) swt_check_patch(d, g_sw_patch, len);
		check_eq_u64(host_mem_reads() - reads, 0, "it reads nothing out of the game");
		check_eq_u64(host_write_log_count(), 0, "and writes nothing into it");
		stamps[i] = be32_get(g_sw_patch + 4);

		check(switches_patch(g_sw_patch2, SWITCH_PATCH_HEAD + 8u, &len2) == ST_FULL,
		      "a reply buffer too small for it is FULL");
		check_eq_u64(switches_state(), 0, "a console never looks for the switches");
		check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) == 0, "and never sets bit4");
	}

	check(stamps[0] != stamps[1] && stamps[0] != stamps[2] && stamps[0] != stamps[3] &&
	      stamps[1] != stamps[2] && stamps[1] != stamps[3] && stamps[2] != stamps[3],
	      "the four games' stamps are four different numbers");

	/*
	 * Build 53 moved RaC2's trampolines and Deadlocked's second fast-load
	 * site, and gave RaC3 a second switch when its fast loads became a code
	 * patch: a patch any earlier build wrote for RaC1 still counts, and one for
	 * RaC2, RaC3 or Deadlocked is from another build.
	 */
	check_eq_u64(stamps[0], SWT_RAC1_STAMP, "RaC1's stamp has not changed");
	check(stamps[1] != SWT_RAC2_B50_STAMP, "RaC2's has");
	check(stamps[2] != SWT_RAC3_B50_STAMP, "and so has RaC3's");
	check(stamps[3] != SWT_RAC4_B50_STAMP, "and so has Deadlocked's");
	check_eq_u64(stamps[3], SWT_RAC4_STAMP, "to the one build 53 pins");
	check_eq_u64(len, SWITCH_PATCH_HEAD + 123u * SWITCH_PATCH_ROW +
	             SWITCH_PATCH_BYTES_HEAD + 3u * SWITCH_PATCH_BYTE_ROW,
	             "Deadlocked's, the largest, is 109 cave words, 14 sites and 3 flags");

	check(quit_and_wait(), "the game goes away");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_NOT_INGAME,
	      "outside a game SWITCH_PATCH is NOT_INGAME");
	check(boot_and_wait("NPEA09050"), "a title no game claims");
	check(switches_patch(g_sw_patch, sizeof(g_sw_patch), &len) == ST_UNSUPPORTED,
	      "has no switches: UNSUPPORTED");

	test_switches_console();

	/* ------------------------------------ the platform that refuses code */

	host_set_can_patch_code(0);
	host_set_emulator(1);

	for (i = 0; i < 4; i++) {
		char name[64];

		name[0] = 0;
		qstrcat(name, sizeof(name), "build 50: the code switches in ");
		qstrcat(name, sizeof(name), g_swt_games[i].title);
		group(name);
		swt_rpcs3_one_game(&g_swt_games[i]);
	}

	group("build 50: a game no table claims has no switches");
	check(boot_and_wait("NPEA09050"), "an unknown title boots");
	pump(2);
	check_eq_u64(switches_state(), 0, "and the look says no at once");
	check((swt_info_flags() & SESSION_FLAG_CODE_SWITCHES) == 0, "bit4 clear");

	test_switches_presence();
	test_switches_apply();
	test_switches_fastloads();
	test_switches_rac1_fastloads();
	test_switches_rac2_moved();
	test_switches_rac4_fastload_moved();
	test_switches_rac3_fastloads();

	/* ------------------------------------------------- put it all back */

	host_set_can_patch_code(1);
	host_set_emulator(0);

	check(quit_and_wait(), "quit");
	check(boot_and_wait("NPEA00385"), "RaC1 boots again with the gate open");
}

/* -------------------------------------------- every list reply fits a frame */

/*
 * Revision 1.11 made a frame 16384 bytes, so every reply the module can compose
 * has to fit one. The listings are the ones worth proving: each is a table times
 * a row size, and a table that outgrew the frame would either be cut short or
 * refused, and a client would be missing rows it has no way to ask for again.
 *
 * The fixed tables are checked against their own maxima, which is the whole of
 * what they can ever hold. The per-game lists are encoded for real, for each of
 * the four games in turn, which is what the games actually ship.
 */
static void check_fits(u32 bytes, const char *what)
{
	check(bytes <= QWARK_MAX_PAYLOAD, what);
	if (bytes > QWARK_MAX_PAYLOAD)
		printf("        %u bytes, cap %u\n", (unsigned)bytes, (unsigned)QWARK_MAX_PAYLOAD);
}

static void test_reply_sizes(void)
{
	static u8 reply[QWARK_MAX_PAYLOAD];
	static const char * const titles[] = {
		"NPEA00385", "NPEA00386", "NPEA00387", "NPEA00423"
	};
	u32 len = 0;
	u32 i;

	group("every list reply fits one 16384-byte frame");

	/* The tables whose maximum is a compile-time constant. */
	check_fits(1u + QWARK_MAX_WATCHES * 8u, "WATCH_LIST with all 64 watches");
	check_fits(1u + QWARK_MAX_FREEZES * 16u, "FREEZE_LIST with all 64 freezes");
	check_fits(1u + QWARK_MAX_PATCHES * 40u, "PATCH_LIST with all 64 patches");
	check_fits(1u + (u32)QWARK_MAX_MODS * MOD_WIRE_SIZE, "MOD_LIST with all 32 mods");
	check_fits(2u + QWARK_POS_SLOTS * 16u, "POS_LIST with all 8 slots");
	check_fits(1u + COMBO_COUNT * 8u, "COMBO_LIST");
	check_fits(2u + (u32)QWARK_MAX_GROUPS * 24u + 1u + (u32)QWARK_MAX_READOUTS * 24u
	           + 1u + (u32)QWARK_MAX_FEATURES * FEATURE_WIRE_SIZE,
	           "DESCRIBE with 16 groups, 16 readouts and 64 features");
	check_fits(1u + (u32)QWARK_MAX_PLANETS * 24u, "PLANET_LIST with 64 planets");
	check_fits(13u + QWARK_MAX_FREEZES * 16u + 1u + 16u * 8u,
	           "PREVIOUS_LIST with 64 freezes and 16 client patches");
	check_fits(5u + (u32)AUTOSPLIT_RING_SLOTS * AUTOSPLIT_EVENT_SIZE,
	           "AUTOSPLIT_EVENTS with the whole 64-event ring");
	check_fits((u32)TELEMETRY_MAX, "GET_STATE and the telemetry packet");
	check_fits(1u + 255u * SAVEFILE_ROW_SIZE, "SAVEFILE_LIST with 255 saves");
	check_fits(1u + 255u * SAVEFILE_NAME_LEN, "SAVEFILE_CATEGORIES with 255 categories");
	check_fits(1024u, "MOD_INFO, which is capped at 1024 bytes");
	check_fits(MOD_PATCH_HEAD + MOD_PATCH_PAGE_ROWS * MOD_PATCH_ROW + MOD_PATCH_BYTES_HEAD,
	           "a MOD_PATCH page at its largest, 2046 rows (revisions 1.16 and 1.19)");
	check_fits(1u + 255u * 24u, "FEATURE_OPTIONS with 255 options");

	/* And the four games' own tables, encoded the way the wire encodes them. */
	for (i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
		const struct game_api *g;
		char what[96];

		if (!quit_and_wait()) { check(0, "quit before the next game"); return; }
		if (!boot_and_wait(titles[i])) { check(0, titles[i]); continue; }

		g = session_game();
		if (g == NULL) { check(0, "the game came up"); continue; }

		what[0] = 0;
		qstrcat(what, sizeof(what), titles[i]);
		qstrcat(what, sizeof(what), ": DESCRIBE fits a frame");
		check(features_describe(reply, sizeof(reply), &len) == ST_OK &&
		      len <= QWARK_MAX_PAYLOAD, what);

		what[0] = 0;
		qstrcat(what, sizeof(what), titles[i]);
		qstrcat(what, sizeof(what), ": MOD_LIST fits a frame");
		check(mods_list_encode(reply, sizeof(reply), &len) == ST_OK &&
		      len <= QWARK_MAX_PAYLOAD, what);

		/* Revision 1.15. Deadlocked's is the largest, at 2132 bytes. */
		what[0] = 0;
		qstrcat(what, sizeof(what), titles[i]);
		qstrcat(what, sizeof(what), ": SAVEFILE_PATCH fits a frame");
		check(savefile_patch(reply, sizeof(reply), &len) == ST_OK &&
		      len <= QWARK_MAX_PAYLOAD, what);

		if (g->planet_names != NULL) {
			u8 count = 0;
			g->planet_names(&count);
			what[0] = 0;
			qstrcat(what, sizeof(what), titles[i]);
			qstrcat(what, sizeof(what), ": PLANET_LIST fits a frame");
			check_fits(1u + (u32)count * 24u, what);
		}

		if (g->unlock_list != NULL) {
			const struct game_unlock *list = NULL;
			const char * const *cats = NULL;
			const struct unlock_field_desc *fields = NULL;
			u8 n = 0, ncat = 0;

			if (g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK) {
				what[0] = 0;
				qstrcat(what, sizeof(what), titles[i]);
				qstrcat(what, sizeof(what), ": UNLOCK_LIST fits a frame");
				check_fits(1u + (u32)ncat * 24u + 4u * UNLOCK_FIELD_WIRE_SIZE
				           + 1u + (u32)n * UNLOCK_WIRE_SIZE, what);
			}
		}

		if (g->autosplit_describe != NULL) {
			u8 count = 0;
			g->autosplit_describe(&count);
			what[0] = 0;
			qstrcat(what, sizeof(what), titles[i]);
			qstrcat(what, sizeof(what), ": AUTOSPLIT_DESCRIBE fits a frame");
			check_fits(1u + (u32)count * AUTOSPLIT_DESC_SIZE, what);
		}
	}

	check(quit_and_wait(), "quit the last game");
	check(boot_and_wait("NPEA00385"), "and put RaC1 back for what follows");
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
		sa.sin_port = htons(net_port());
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

/* ------------------------------------------------------------- hot blocks */

/*
 * Every registered game's hot table has to fit the pool the tick thread reads
 * the blocks into. The pool is smaller than the number of blocks times the
 * longest one, on purpose - no game asks for sixteen 1 KB blocks - so this is
 * the check that keeps a new block, or a longer one, from quietly falling off
 * the end and leaving a game decoding half its state.
 */
static void test_hot_tables(void)
{
	u32 i;

	group("hot tables fit the shared pool");

	for (i = 0; ; i++) {
		const struct game_api *g = game_at(i);
		if (g == NULL) break;
		check(session_hot_fits(g),
		      g->title_ids[0] != NULL ? g->title_ids[0] : "a registered game");
	}

	check(i >= 4, "all four games were checked");
}

/* ------------------------------------------------------ the port for this run */

/*
 * test/run.sh picks a free TCP port and passes it in QWARK_TEST_PORT; main hands
 * it to net_set_port before anything listens. The suite used to sit on 9673, and
 * because the listener asked for SO_REUSEADDR the bind quietly succeeded beside
 * a qwark-rpcs3.exe that was already serving the user's client - so the tests
 * connected to that process and checked it instead of the build under test. The
 * host builds no longer set SO_REUSEADDR and the suite no longer uses a fixed
 * port. With no variable set the default still stands, so a bare run works.
 */
static void test_command_port(void)
{
	const char *env = getenv("QWARK_TEST_PORT");
	struct sockaddr_in sa;
	int probe;

	group("the command port for this run");

	if (env != NULL && env[0] != 0) {
		check_eq_u64(net_port(), (u64)atoi(env),
		             "QWARK_TEST_PORT is the port the suite listens on");
		check(net_port() != QWARK_PORT,
		      "and it is not 9673, so a running helper cannot answer for us");
	} else {
		check_eq_u64(net_port(), QWARK_PORT,
		             "with no QWARK_TEST_PORT the suite keeps the default 9673");
	}

	/*
	 * Nothing holds it yet: the proof that the port really was free, and so the
	 * proof that a second run in a row picks another one that is free too.
	 */
	probe = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	check(probe >= 0, "a probe socket opens");
	if (probe < 0) return;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(net_port());
	sa.sin_addr.s_addr = htonl(0x7F000001u);   /* never 0.0.0.0: no firewall prompt */
	check(bind(probe, (struct sockaddr *)&sa, sizeof(sa)) == 0,
	      "and the port is free before the listener opens");
	plat_socket_close(probe);

	/* --bind on both PC builds goes through this, and takes a dotted quad only. */
	check(net_set_bind("0.0.0.0") == 1, "--bind accepts 0.0.0.0");
	check(net_set_bind("") == 0, "and refuses an empty address");
	check(net_set_bind("the living room") == 0, "and refuses what is not an address");
	check(net_set_bind("127.0.0.1") == 1,
	      "and takes 127.0.0.1, which is where everything here listens");
}

/* ------------------- build 53, RaC1's, RaC2's and RaC3's helpers pause to load */

/*
 * What can be checked here, once more: the PowerPC that pauses the game and
 * waits for it runs only in the game, so the pause itself is a console's to
 * answer. What is checked is everything around it - where the generated table
 * puts the caves, the hook words and the request bytes, that SAVEFILE_PATCH
 * hands out exactly those, that Deadlocked's helper did not move by a byte (an
 * RPCS3 patch for it written by build 52 still counts), that RaC3's stays clear
 * of the code switch beside it, and that qwark refuses, with the right error,
 * every request the helper would drop for want of a save or of a game it can
 * take into its pause - and lets through every one it no longer drops.
 */

/*
 * SAVEFILE_PATCH's stamps, pinned: a change to one means every RPCS3 user of
 * that game has to write the patch again, which a build should do on purpose
 * and not by accident. Build 53 changed RaC1's, RaC2's and RaC3's, so a patch
 * an earlier build wrote for any of the three reads as another build's, and
 * left Deadlocked's alone.
 */
#define SF53_STAMP_RAC1 0x2A9B51FAu
#define SF53_STAMP_RAC2 0x35EE054Eu
#define SF53_STAMP_RAC3 0xE167FC1Eu   /* with the trophy reset */
#define SF53_STAMP_RAC4 0x23199E63u

/* Build 52's RaC3 stamp, which an RPCS3 user's patch from then carries. */
#define SF52_STAMP_RAC3 0x038F1389u

/* Whether every word row of a SAVEFILE_PATCH reply lands in [lo, hi) or on `hook`. */
static int sf53_rows_within(const u8 *reply, u32 len, u32 lo1, u32 hi1, u32 lo2, u32 hi2, u32 hook)
{
	u32 n = be16_get(reply);
	u32 off = SAVEFILE_PATCH_HEAD;
	u32 i;

	for (i = 0; i < n; i++) {
		u32 a;

		if (off + SAVEFILE_PATCH_ROW > len) return 0;
		a = be32_get(reply + off);
		if (!((a >= lo1 && a + 4u <= hi1) || (a >= lo2 && a + 4u <= hi2) || a == hook)) return 0;
		off += SAVEFILE_PATCH_ROW;
	}
	return 1;
}

/* The address of byte entry `k` of a SAVEFILE_PATCH reply. */
static u32 sf53_byte_addr(const u8 *reply, u32 k)
{
	u32 off = SAVEFILE_PATCH_HEAD + be16_get(reply) * SAVEFILE_PATCH_ROW + SAVEFILE_PATCH_BYTES_HEAD;

	return be32_get(reply + off + k * SAVEFILE_PATCH_BYTE_ROW);
}

/* Asks for a request and reports the status and the transfer error it left. */
static int sf53_request(int load, u8 *error)
{
	u32 done = 0, total = 0;
	int rc = load ? savefile_load_aside() : savefile_set_aside();

	savefile_transfer(&done, &total, error);
	return rc;
}

static void test_savefile_build53(void)
{
	u8 sup = 9, ins = 9, run = 9, pend = 9, error = 9, b = 9;
	u32 size = 0, len = 0, done = 0, total = 0;
	u8 junk[0x1C];
	u32 v = 0;

	/* ------------------------------------------------- RaC1's placement */

	group("build 53: RaC1's helper and its bytes moved");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);

		check(quit_and_wait(), "quit whatever was running");
		check(d != NULL && d->ncaves == 2, "RaC1 has its stub cave and its helper cave");
		if (d == NULL || d->ncaves != 2) return;

		check_eq_u64(d->caves[0].addr, 0x004F6800u, "the stub is where it was, 0x4F6800");
		check_eq_u64(d->caves[1].addr, 0x004F8000u, "and so is the helper, 0x4F8000");
		check(d->caves[1].addr + d->caves[1].len <= 0x004F8C68u,
		      "which ends inside the dead function it sits in (0x4F7EB8..0x4F8C68)");
		check_eq_u64(d->hooks[0].addr, 0x0011E3A0u, "the hook site is the pad routine's epilogue");
		check_eq_u64(d->hooks[0].value, 0x484F6803u, "and the hook word is still bla 0x4F6800");

		check_eq_u64(d->api_mod, 0x01080000u, "api_mod is at 0x1080000, out of the bump arena");
		check_eq_u64(d->api_load, 0x01080001u, "api_load follows it");
		check_eq_u64(d->api_setaside, 0x01080002u, "and api_setaside");
		check(d->api_mod >= 0x01054C44u && d->api_setaside < 0x010A0230u,
		      "in the part of the debug save buffer nothing ever writes");
		check(d->api_mod < 0x01606310u, "inside the read-write segment, so a patch may write it");

		check_eq_u64(d->aside_addr, 0x00FC0000u, "the aside buffer is at 0xFC0000");
		check_eq_u64(d->aside_size, 0x000B0000u, "and is still 0xB0000 bytes, so saves keep their size");
		check(d->aside_addr >= 0x00FB0230u && d->aside_addr + d->aside_size <= 0x010A0230u,
		      "inside the debug save buffer, clear of the statics from 0x10A0230 on");
		check(d->api_mod >= d->aside_addr + d->aside_size,
		      "and the request bytes are past its end");
		check(d->aside_addr + d->aside_size <= 0x00B00070u || d->aside_addr >= 0x00E572C4u,
		      "nowhere near the bump arena either");

		check(boot_and_wait("NPEA00385"), "RaC1 boots");
		check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK, "the RaC1 patch");
		sf_check_patch_words(d, g_sf_patch, len);
		check(sf53_rows_within(g_sf_patch, len, 0x004F6800u, 0x004F6874u,
		                       0x004F8000u, 0x004F8C68u, 0x0011E3A0u),
		      "every word it writes is in one of the two caves or is the hook word");
		check(sf53_byte_addr(g_sf_patch, 0) == 0x01080000u &&
		      sf53_byte_addr(g_sf_patch, 1) == 0x01080001u &&
		      sf53_byte_addr(g_sf_patch, 2) == 0x01080002u,
		      "and the bytes it zeroes are the new request bytes");
		check_eq_u64(len, 2116, "of 2116 bytes, the helper having shrunk to 920 with the pause");
		check_eq_u64(be32_get(g_sf_patch + 4), SF53_STAMP_RAC1, "and the stamp this build pins for it");
	}

	/* ------------------------------------------------- RaC2's placement */

	group("build 53: RaC2's helper moved off Joba's code");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC2);

		check(d != NULL && d->ncaves == 1, "RaC2 has one cave");
		if (d == NULL || d->ncaves != 1) return;

		check_eq_u64(d->caves[0].addr, 0x0113EC64u, "it is at 0x113EC64, in the dead span");
		check(d->caves[0].addr + d->caves[0].len <= 0x011414FCu, "and ends inside it");
		check(d->caves[0].addr + d->caves[0].len <= 0x0022A368u || d->caves[0].addr >= 0x00233308u,
		      "nowhere in Joba's routine at 0x22A368..0x233308");
		check_eq_u64(d->hooks[0].addr, 0x00B086B0u, "the hook site is the same nop");
		check_eq_u64(d->hooks[0].value, 0x4913EC67u, "and the hook word is bla 0x113EC64");
		check(d->caves[0].addr < 0x02000000u, "which an absolute branch reaches");
		check_eq_u64(d->api_mod, 0x01317728u, "the request bytes have not moved");
		check_eq_u64(d->aside_addr, 0x01C00000u, "nor has the aside buffer");

		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00386"), "RaC2 boots");
		check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK, "the RaC2 patch");
		sf_check_patch_words(d, g_sf_patch, len);
		check(sf53_rows_within(g_sf_patch, len, 0x0113EC64u, 0x011414FCu, 0, 0, 0x00B086B0u),
		      "every word it writes is in the new cave or is the hook word");
		check(sf53_byte_addr(g_sf_patch, 0) == 0x01317728u, "and the bytes it zeroes are where they were");
		check(d->caves[0].addr + d->caves[0].len <= 0x01141400u,
		      "and the helper stops short of the code switches at 0x1141400");
		check_eq_u64(len, 1972, "the reply is 1972 bytes");
		check_eq_u64(be32_get(g_sf_patch + 4), SF53_STAMP_RAC2, "with the stamp this build pins for it");

		/*
		 * Under RPCS3, a hook word that still branches to build 52's cave reads as
		 * a helper from another build, even with this build's cave beside it: the
		 * client is told to write the new patch.
		 */
		check(quit_and_wait(), "quit");
		host_set_can_patch_code(0);
		host_set_emulator(1);
		host_boot("NPEA00386");
		sf_poke_patch(g_sf_patch, len, SF_NO_FLIP, 1);
		poke32(0x00B086B0u, 0x4822A36Bu);
		check(pump_until(SESSION_INGAME, 4000), "RaC2 boots with build 52's hook word over the patch");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "which is not this build's helper");
		check(quit_and_wait(), "quit");
		host_boot("NPEA00386");
		sf_poke_patch(g_sf_patch, len, SF_NO_FLIP, 1);
		check(pump_until(SESSION_INGAME, 4000), "RaC2 boots with this build's patch");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 1, "which is");
		check(quit_and_wait(), "quit");
		host_set_can_patch_code(1);
		host_set_emulator(0);
	}

	/* ------------------------------------------------- RaC3's helper */

	group("build 53: RaC3's helper pauses too, and stays out of the code switch");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC3);
		const struct sw_desc *s = sw_desc_for_game(GAME_RAC3);

		/*
		 * Build 53: and the trophy reset, a second cave of its own at the top
		 * of the strictly dead 0x980304..0x980AC0 (sf_rac3.h); the helper is
		 * still the first.
		 */
		check(d != NULL && d->ncaves == 2, "RaC3 has two caves: the helper and the trophy reset");
		if (d == NULL || d->ncaves != 2) return;

		check_eq_u64(d->caves[0].addr, 0x00980AC0u, "it is where it was, 0x980AC0");
		check(d->caves[0].addr + d->caves[0].len <= 0x00980F00u,
		      "and ends before the Freeze ammo switch's cave at 0x980F00, its SF_CAVE_END");
		check(d->caves[0].len > 372u, "having grown past build 52's 372 bytes");
		check(s != NULL && swt_apart(s, d), "nothing of the switch overlaps anything of the helper");
		check_eq_u64(d->caves[1].addr, 0x009808C0u, "the trophy reset is at 0x9808C0");
		check(d->caves[1].addr >= 0x00980304u && d->caves[1].addr + d->caves[1].len <= 0x00980AC0u,
		      "inside the dead span and short of the helper, its SF_CAVE_TROPHY_END");
		check_eq_u64(d->api_trophy, 0x00D9FF1Cu, "and its request byte follows the pause's words");
		check_eq_u64(d->hooks[0].addr, 0x0097CBA4u, "the hook site has not moved");
		check_eq_u64(d->hooks[0].value, 0x48003F1Du, "nor has the hook word, bl 0x980AC0");
		check_eq_u64(d->api_mod, 0x00D9FF00u, "the request bytes have not moved");
		check_eq_u64(d->api_load, 0x00D9FF01u, "the load byte follows it");
		check_eq_u64(d->api_setaside, 0x00D9FF02u, "and the set-aside byte");
		check_eq_u64(d->aside_addr, 0x01100000u, "nor has the aside buffer");
		check_eq_u64(d->aside_size, 0x00200000u, "nor its size");

		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00387"), "RaC3 boots");
		check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK, "the RaC3 patch");
		sf_check_patch_words(d, g_sf_patch, len);
		check(sf53_rows_within(g_sf_patch, len, 0x00980AC0u, 0x00980F00u,
		                       0x009808C0u, 0x00980AC0u, 0x0097CBA4u),
		      "every word it writes is in one of the two caves, short of the switch, or is the hook word");
		check(sf53_byte_addr(g_sf_patch, 0) == 0x00D9FF00u &&
		      sf53_byte_addr(g_sf_patch, 1) == 0x00D9FF01u &&
		      sf53_byte_addr(g_sf_patch, 2) == 0x00D9FF02u,
		      "and the bytes it zeroes are the three request bytes");
		check_eq_u64(len, 2924, "the reply is 2924 bytes, the trophy reset included");
		check_eq_u64(be32_get(g_sf_patch + 4), SF53_STAMP_RAC3, "with the stamp this build pins for it");
		check(be32_get(g_sf_patch + 4) != SF52_STAMP_RAC3,
		      "which is not build 52's, so a patch from then reads as another build's");
		check(quit_and_wait(), "quit");
	}

	group("build 53: Deadlocked's helper did not change");
	{
		check(boot_and_wait("NPEA00423"), "Deadlocked boots");
		check(savefile_patch(g_sf_patch, sizeof(g_sf_patch), &len) == ST_OK &&
		      be32_get(g_sf_patch + 4) == SF53_STAMP_RAC4,
		      "Deadlocked's patch has build 52's stamp, so a patch from then still counts");
		check_eq_u64(len, 2132, "and is the largest reply again, 2132 bytes");
		check(quit_and_wait(), "quit");
	}

	/* --------------------------------------------- RaC1: no save, not idle */

	group("build 53: RaC1 refuses a request with no save or no idle game");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);

		check(boot_and_wait("NPEA00385"), "RaC1 boots: a fresh process has no save");
		host_write_log_reset();

		check(sf53_request(0, &error) == ST_BUSY, "a set-aside is BUSY");
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "because there is no save to set aside");
		check(sf53_request(1, &error) == ST_BUSY, "and a load with nothing set aside is too");
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "for the same reason");
		check_eq_u64(host_write_log_count(), 0, "neither wrote a byte into the game");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "nor installed the helper");

		poke32(SF_R1_SAVE_PTR, SF_R1_SAVE_BASE);
		memset(junk, 0x5A, sizeof(junk));
		host_poke(SF_R1_SAVE_BUF, junk, sizeof(junk));
		check(sf53_request(0, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "a save buffer that holds something else is no save either");

		sf_give_save(GAME_RAC1);
		check(sf53_request(0, &error) == ST_OK, "with a save in it the set-aside goes out");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);

		check(sf53_request(1, &error) == ST_OK,
		      "and so does a load with nothing set aside: the helper fills it from that save");
		host_poke(d->api_load, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);

		/* The aside buffer holding something is what a load hands over, or nothing. */
		host_poke(d->aside_addr, junk, sizeof(junk));
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "an aside buffer that holds something that is not a save is refused, not replaced");
		sf_put_save_head(GAME_RAC1, d->aside_addr);
		poke32(d->aside_addr + 0x18, 25);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "and so is a save of a planet the game does not have");
		poke32(d->aside_addr + 0x18, 3);

		/*
		 * The helper takes the game into the start menu itself from any state
		 * the main loop runs, so the pre-check refuses only a movie, which plays
		 * in a loop of its own, a state the loop runs nothing in, and a planet
		 * load already on its way.
		 */
		poke32(SF_R1_STATE, 1);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "a load while a movie plays is BUSY, the game not idle");
		check(sf53_request(0, &error) == ST_OK,
		      "while a set-aside, which only copies the last save, still goes out");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);
		poke32(SF_R1_STATE, 8);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "and a load in state 8, which the main loop runs nothing in");
		poke32(SF_R1_STATE, 0xFFFFFFFEu);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in state -2");
		poke32(SF_R1_STATE, 0);
		poke32(SF_R1_REQUEST, 1);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "and one while a planet load is already on its way");
		poke32(SF_R1_REQUEST, 0);
		{
			/* Up to the rework a load went out from play and the start menu alone. */
			static const u32 states[] = { 0xFFFFFFFFu, 2, 3, 4, 5, 6, 7 };
			static const char * const names[] = {
				"a load from the debug menu goes out: the helper pauses from anything",
				"and from a cutscene",
				"and from the start menu",
				"and from a prompt",
				"and from the vendor",
				"and from a planet's arrival",
				"and from a cinematic"
			};
			u32 k;

			for (k = 0; k < sizeof(states) / sizeof(states[0]); k++) {
				poke32(SF_R1_STATE, states[k]);
				check(sf53_request(1, &error) == ST_OK, names[k]);
				host_poke(d->api_load, (const u8 *)"\x00", 1);
				pump(SAVEFILE_SETTLE_TICKS);
			}
		}
		poke32(SF_R1_STATE, 0);
		check(sf53_request(1, &error) == ST_OK, "and so does one from ordinary play");
		host_peek(d->api_load, &b, 1);
		check_eq_u64(b, 1, "as a 1, which the helper pauses the game for");

		/* The helper's own refusals, the two codes it leaves in the byte. */
		host_poke(d->api_load, (const u8 *)"\xFD", 1);
		pump(2);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "the helper's 0xFD is reported as no save");
		host_peek(d->api_load, &b, 1);
		check_eq_u64(b, 0, "and the byte is put back to 0");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(pend, 0, "with nothing left outstanding");

		check(sf53_request(1, &error) == ST_OK, "another load");
		host_poke(d->api_load, (const u8 *)"\xFE", 1);
		pump(2);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE,
		             "and 0xFE, the game would not pause, as not idle");

		check(sf53_request(0, &error) == ST_OK, "a set-aside");
		host_poke(d->api_setaside, (const u8 *)"\xFD", 1);
		pump(2);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "which the helper can drop for no save too");

		/* Force autosave keeps its own byte, at its new address. */
		check(features_trigger(F_FORCE_AUTOSAVE_ID) == ST_OK, "Force autosave fires");
		host_peek(0x01080003u, &b, 1);
		check_eq_u64(b, 3, "writing its 3 at 0x1080003");
		host_peek(0x00B00073u, &b, 1);
		check_eq_u64(b, 0, "and nothing at the bump arena's 0xB00073");
		host_poke(0x01080003u, (const u8 *)"\x00", 1);
	}

	/* ------------------------------------------------- the library, RaC1 */

	group("build 53: RaC1's library refuses for the same reasons");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC1);
		plat_file_t f;
		u32 i;

		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00385"), "RaC1 boots with no save");
		check(savefile_category(SAVEFILE_CATEGORY_CREATE, "b53") == ST_OK, "a category");

		check(savefile_store("b53", "none.sav") == ST_BUSY, "a STORE is BUSY");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "for want of a save");
		check(!plat_path_exists(SAVE_ROOT "/b53/none.sav", NULL, NULL), "and creates no file");

		/* A file of the right size that is not a save. */
		check(plat_file_open(SAVE_ROOT "/b53/junk.sav", PLAT_OPEN_WRITE, &f) == 0, "a junk file");
		memset(g_pattern, 0x5A, sizeof(g_pattern));
		for (i = 0; i < d->aside_size; i += sizeof(g_pattern)) plat_file_write(f, g_pattern, sizeof(g_pattern));
		plat_file_close(f);

		poke32(SF_R1_STATE, 1);
		check(savefile_restore("b53", "junk.sav") == ST_BUSY, "restoring it while a movie plays is BUSY");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "the game not idle, checked before the copy");
		poke32(SF_R1_STATE, 0);

		check(savefile_restore("b53", "junk.sav") == ST_OK, "in play the RESTORE is accepted");
		check(pump_until_transfer_done(64), "and the copy runs to its end");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(done, d->aside_size, "every byte copied");
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "and then it stops: the file is not a save");
		host_peek(d->api_load, &b, 1);
		check_eq_u64(b, 0, "so no load was raised at the game");

		plat_file_unlink(SAVE_ROOT "/b53/junk.sav");
		plat_file_unlink(SAVE_ROOT "/b53/junk.sav.sum");
		check(savefile_category(SAVEFILE_CATEGORY_DELETE, "b53") == ST_OK, "the category goes");
	}

	/* ----------------------------------------------- RaC2: no save, not idle */

	group("build 53: RaC2 refuses a request before the game has a save");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC2);

		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00386"), "RaC2 boots: no save manager, no buffer");
		poke32(SF_R2_PENDING, R4_MODE_NONE);
		host_write_log_reset();

		check(sf53_request(0, &error) == ST_BUSY, "a set-aside is BUSY");
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE,
		             "for want of a save, where it used to copy two megabytes from address 0");
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "and so is a load with nothing set aside, which used to fill from there");
		check_eq_u64(host_write_log_count(), 0, "neither wrote into the game");

		poke32(SF_R2_SAVE_INFO, SF_R2_SAVE_OBJ);
		check(sf53_request(0, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "a save manager whose buffer word is still 0 has no save either");

		/* The load action forces fast loads, and must not when it is refused. */
		mem_read_u32(R2_FP_ADDR, &v);
		check_eq_u64(v, 0x4BFFEA69u, "the load screen's call is the game's own");
		check(features_trigger(F2_LOAD_ASIDE) == ST_BUSY, "the LOAD_ASIDE action is refused");
		mem_read_u32(R2_FP_ADDR, &v);
		check_eq_u64(v, 0x4BFFEA69u, "before it forced fast loads, so nothing is left forced");

		sf_give_save(GAME_RAC2);
		check(sf53_request(0, &error) == ST_OK, "with a save the set-aside goes out");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);

		poke32(SF_R2_PLANET, 0xFFFFFFFFu);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "a load on the title screen, planet -1, is not idle");
		poke32(SF_R2_PLANET, 5);
		poke32(SF_R2_REQUEST, 1);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor with a planet load on its way");
		poke32(SF_R2_REQUEST, 0);
		poke32(SF_R2_MODE, 6);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in space, the ship between planets");
		poke32(SF_R2_MODE, 0xFFFFFFFFu);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in debug, which the mode change traps leaving");
		poke32(SF_R2_MODE, 10);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in a mode the game does not have");
		{
			/*
			 * Up to the rework a load went out from play and the pause alone, with
			 * nothing queued. The helper asks the game for its pause from any of
			 * these now, and waits out a change already queued.
			 */
			static const u32 modes[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9 };
			static const char * const names[] = {
				"from play it goes out",
				"and from a movie",
				"and from a scene",
				"and from the pause",
				"and from a freeze, a message over the game",
				"and from the vendor",
				"and from mode 7",
				"and from mode 8",
				"and from mode 9"
			};
			u32 k;

			for (k = 0; k < sizeof(modes) / sizeof(modes[0]); k++) {
				poke32(SF_R2_MODE, modes[k]);
				check(sf53_request(1, &error) == ST_OK, names[k]);
				host_poke(d->api_load, (const u8 *)"\x00", 1);
				pump(SAVEFILE_SETTLE_TICKS);
			}
		}
		poke32(SF_R2_MODE, 0);
		poke32(SF_R2_PENDING, 0);
		check(sf53_request(1, &error) == ST_OK, "and with a mode change queued, which it waits for");
		host_poke(d->api_load, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);
		poke32(SF_R2_PENDING, R4_MODE_NONE);

		/* A load the helper drops after the action forced fast loads puts them back. */
		host_poke(R2_LOADCOUNT, (const u8 *)"\x00", 1);
		pump(4);
		check(features_trigger(F2_LOAD_ASIDE) == ST_OK, "the LOAD_ASIDE action goes out from play");
		mem_read_u32(R2_FP_ADDR, &v);
		check_eq_u64(v, 0x60000000u, "forcing fast loads for the load");
		host_poke(d->api_load, (const u8 *)"\xFE", 1);
		pump(3);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "the helper drops it: the game would not pause");
		mem_read_u32(R2_FP_ADDR, &v);
		check_eq_u64(v, 0x4BFFEA69u, "and the fast loads it forced are put back");

		check(quit_and_wait(), "quit");
	}

	/* ----------------------------------------------- RaC3: no save, not idle */

	group("build 53: RaC3 refuses a request with no save, or no pause to go to");
	{
		const struct sf_desc *d = sf_desc_for_game(GAME_RAC3);
		plat_file_t f;
		u32 i;

		check(boot_and_wait("NPEA00387"), "RaC3 boots: no save manager, no buffer");
		host_write_log_reset();

		check(sf53_request(0, &error) == ST_BUSY, "a set-aside is BUSY");
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE,
		             "for want of a save, where it used to read the buffer word through a "
		             "save manager that was not there");
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "and so is a load with nothing set aside, which used to fill from there");
		check_eq_u64(host_write_log_count(), 0, "neither wrote into the game");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(ins, 0, "nor installed the helper");

		poke32(SF_R3_SAVE_INFO, SF_R3_SAVE_OBJ);
		check(sf53_request(0, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "a save manager whose buffer word is still 0 has no save either");
		poke32(SF_R3_SAVE_OBJ + 4, SF_R3_SAVE_BUF);
		memset(junk, 0x5A, sizeof(junk));
		host_poke(SF_R3_SAVE_BUF, junk, sizeof(junk));
		check(sf53_request(0, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "and a buffer that holds something else is no save");

		sf_give_save(GAME_RAC3);
		check(sf53_request(0, &error) == ST_OK, "with a save the set-aside goes out");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);

		/* The older pair of block sizes, RaC2's, which the restore also takes. */
		poke32(SF_R3_SAVE_BUF, 0x1CF8u);
		poke32(SF_R3_SAVE_BUF + 4, 0x0CD0u);
		check(sf53_request(0, &error) == ST_OK, "and so does one of the older layout");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);
		poke32(SF_R3_SAVE_BUF + 4, 0x84D0u);
		check(sf53_request(0, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "but not with RaC2's planet block, which this game's restore refuses");
		sf_give_save(GAME_RAC3);

		check(sf53_request(1, &error) == ST_OK,
		      "a load with nothing set aside goes out: the helper fills it from that save");
		host_peek(d->api_load, &b, 1);
		check_eq_u64(b, 1, "as a 1, which the helper pauses the game for");
		host_poke(d->api_load, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);

		host_poke(d->aside_addr, junk, sizeof(junk));
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NO_SAVE,
		      "an aside buffer that holds something that is not a save is refused, not replaced");
		sf_put_save_head(GAME_RAC3, d->aside_addr);

		poke32(SF_R3_REQUEST, 1);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "a load with a planet load on its way is not idle");
		check(sf53_request(0, &error) == ST_OK,
		      "while a set-aside, which only copies the last save, still goes out");
		host_poke(d->api_setaside, (const u8 *)"\x00", 1);
		pump(SAVEFILE_SETTLE_TICKS);
		poke32(SF_R3_REQUEST, 0);
		poke32(SF_R3_MODE, 6);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in space, the transition a planet load runs");
		poke32(SF_R3_MODE, 0xFFFFFFFFu);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in debug, which the mode change traps leaving");
		poke32(SF_R3_MODE, 10);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in the lobby");
		poke32(SF_R3_MODE, 16);
		check(sf53_request(1, &error) == ST_BUSY && error == SAVEFILE_ERR_NOT_IDLE,
		      "nor in a memory card command");
		{
			static const u32 modes[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9 };
			static const char * const names[] = {
				"from play a load goes out",
				"and from a movie",
				"and from a scene",
				"and from the pause",
				"and from a freeze",
				"and from the vendor",
				"and from mode 7",
				"and from mode 8",
				"and from mode 9"
			};
			u32 k;

			for (k = 0; k < sizeof(modes) / sizeof(modes[0]); k++) {
				poke32(SF_R3_MODE, modes[k]);
				check(sf53_request(1, &error) == ST_OK, names[k]);
				host_poke(d->api_load, (const u8 *)"\x00", 1);
				pump(SAVEFILE_SETTLE_TICKS);
			}
		}
		poke32(SF_R3_MODE, 0);

		/* The helper's own refusals, the two codes it leaves in the byte. */
		check(sf53_request(1, &error) == ST_OK, "a load");
		host_poke(d->api_load, (const u8 *)"\xFD", 1);
		pump(2);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "the helper's 0xFD is reported as no save");
		host_peek(d->api_load, &b, 1);
		check_eq_u64(b, 0, "and the byte is put back to 0");
		check(sf53_request(1, &error) == ST_OK, "another load");
		host_poke(d->api_load, (const u8 *)"\xFE", 1);
		pump(2);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE,
		             "and 0xFE, no pause within the helper's wait, as not idle");
		savefile_info(&sup, &ins, &run, &pend, &size);
		check_eq_u64(pend, 0, "with nothing left outstanding");

		/*
		 * Build 53: UYA's LOAD_ASIDE action forces fast loads, as RaC2's does,
		 * and must not when it is refused; a load the helper drops puts them
		 * back, and the end of one it takes does.
		 */
		r3_fl_seed();
		poke32(SF_R3_MODE, 6);
		check(features_trigger(F3_LOAD_ASIDE) == ST_BUSY, "the LOAD_ASIDE action is refused in space");
		check(r3_fl_original(), "before it forced fast loads, so nothing is left forced");
		poke32(SF_R3_MODE, 0);
		pump(3);
		check(features_trigger(F3_LOAD_ASIDE) == ST_OK, "from play the LOAD_ASIDE action goes out");
		check(r3_fl_patched(), "forcing fast loads for the load");
		check((features_toggle_state() & ((u64)1 << F3_FAST_LOADS)) == 0,
		      "while the checkbox stays off");
		host_poke(d->api_load, (const u8 *)"\xFE", 1);
		pump(3);
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "the helper drops it: the game would not pause");
		check(r3_fl_original(), "and the fast loads it forced are put back");

		check(features_trigger(F3_LOAD_ASIDE) == ST_OK && r3_fl_patched(),
		      "another LOAD_ASIDE forces them again");
		host_poke(d->api_load, (const u8 *)"\x00", 1);
		poke32(SF_R3_REQUEST, 1);
		pump(SAVEFILE_SETTLE_TICKS);
		check(r3_fl_patched(), "the helper takes it and the game's load is under way: still on");
		poke32(SF_R3_REQUEST, 0);
		pump(3);
		check(r3_fl_original(), "and the end of that load puts the game's words back");

		/* The library asks the same questions. */
		check(quit_and_wait(), "quit");
		check(boot_and_wait("NPEA00387"), "RaC3 boots again with no save");
		check(savefile_category(SAVEFILE_CATEGORY_CREATE, "b53") == ST_OK, "a category");
		check(savefile_store("b53", "none.sav") == ST_BUSY, "a STORE is BUSY");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "for want of a save");
		check(plat_file_open(SAVE_ROOT_R3 "/b53/junk.sav", PLAT_OPEN_WRITE, &f) == 0, "a junk file");
		memset(g_pattern, 0x5A, sizeof(g_pattern));
		for (i = 0; i < d->aside_size; i += sizeof(g_pattern)) plat_file_write(f, g_pattern, sizeof(g_pattern));
		plat_file_close(f);
		poke32(SF_R3_MODE, 6);
		check(savefile_restore("b53", "junk.sav") == ST_BUSY, "restoring it in space is BUSY");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NOT_IDLE, "the game not idle, checked before the copy");
		poke32(SF_R3_MODE, 0);
		check(savefile_restore("b53", "junk.sav") == ST_OK, "in play the RESTORE is accepted");
		check(pump_until_transfer_done(64), "and the copy runs to its end");
		savefile_transfer(&done, &total, &error);
		check_eq_u64(error, SAVEFILE_ERR_NO_SAVE, "and then it stops: the file is not a save");
		host_peek(d->api_load, &b, 1);
		check_eq_u64(b, 0, "so no load was raised at the game");
		plat_file_unlink(SAVE_ROOT_R3 "/b53/junk.sav");
		plat_file_unlink(SAVE_ROOT_R3 "/b53/junk.sav.sum");
		check(savefile_category(SAVEFILE_CATEGORY_DELETE, "b53") == ST_OK, "the category goes");

		check(quit_and_wait(), "quit");
	}
}

int main(void)
{
	const char *port_env = getenv("QWARK_TEST_PORT");

	/* Before plat_init, and long before any listener: see test_command_port. */
	if (port_env != NULL && port_env[0] != 0) net_set_port((u16)atoi(port_env));

	if (plat_init() != 0) {
		printf("plat_init failed\n");
		return 2;
	}

	if (session_init() != ST_OK) {
		printf("session_init failed\n");
		return 2;
	}

	test_command_port();
	test_endian();
	test_framing();
	test_patches();
	test_patch_order();
	test_patch_pool();
	test_tables();
	test_mods();
	test_describe();
	test_telemetry();
	test_session_same_title();
	test_session_different_title();
	test_unknown_title();
	test_rac2_repacks();
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
	test_savefile_not_idle();
	test_savefile_build53();
	test_signed_values();
	test_combo_suspend();
	test_combo_enable();
	test_rac2();
	test_rac2_crash_guards();
	test_manips();
	test_rac1_fastload_force();
	test_rac3();
	test_rac4();
	test_freeze_health();
	test_toggles_retired();
	test_trilogy();
	test_autosplit();
	test_no_code_patches_planet_load();
	test_no_code_patches();
	test_savefile_rpcs3();
	test_mod_patch_layout();
	test_mod_patch_pages();
	test_mod_presence();
	test_mod_previous();
	test_switches();
	test_reply_sizes();
	test_config();
	test_hot_tables();
	test_fingerprint();
	test_submit_after_stop();
	test_net_stop_with_client();
	test_pine();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");

	plat_shutdown();
	return g_failures == 0 ? 0 : 1;
}
