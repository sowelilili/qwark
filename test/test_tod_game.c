/*
 * Revision 1.27: Tools of Destruction (game 5) on qwark's fake console, linked
 * in as every PC build links it: the title index entry, the hot block (the
 * pad's remap, the analogs, the planet, the hero's position two pointers
 * deep), the features, positions, Die, the planet load, the unlock table, and
 * the core's Lua ops (src/core/luacore.c) against the simulator's stand-in for
 * the helper, on a console (qwark installs it) and under RPCS3 (the patch is in
 * at boot, or it is not).
 *
 * The mailbox's own codec and state machine are test/test_tod.c's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/features.h"
#include "../src/core/luacore.h"
#include "../src/core/luamb.h"
#include "../src/core/util.h"
#include "../src/games/game.h"
#include "../src/games/titles.h"
#include "../src/games/tod.h"
#include "../src/games/luahelper_bins.h"
#include "../src/games/luahelper_patch.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define FAKE_ROOT  0x30000000u
#define FAKE_HERO  0x30010000u
#define FAKE_MOBY  0x30020000u

/* A LIVE toggle is read back every twelfth tick (session.c, LIVE_POLL_EVERY). */
#define LIVE_TICKS_FOR_TEST 13

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot_tod(void)
{
	int i;

	host_boot_as(TOD_TITLE, "tod");
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME && session_game() != NULL) return 1;
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

static u32 rd32(u32 addr)
{
	u8 b[4];
	host_peek(addr, b, 4);
	return be32_get(b);
}

static void wr32(u32 addr, u32 v)
{
	u8 b[4];
	be32_put(b, v);
	host_poke(addr, b, 4);
}

static void wrf(u32 addr, f32 v)
{
	wr32(addr, f32_bits(v));
}

static f32 rdf(u32 addr)
{
	return f32_from_bits(rd32(addr));
}

/* LUA_EXEC's two halves, as the ring runs them, with the fake helper's frames between. */
static u16 exec(const char *src, int frames, u8 *reply, u32 *len)
{
	u8 req[64 + LUA_EXEC_HEAD];
	u32 n = (u32)strlen(src), seq = 0;
	int rc, done = 0, i;

	be16_put(req, (u16)n);
	be16_put(req + 2, 0);
	memcpy(req + LUA_EXEC_HEAD, src, n);
	rc = lua_core_exec_start(req, LUA_EXEC_HEAD + n, &seq);
	if (rc != ST_OK) return (u16)rc;
	for (i = 0; i < 400 && !done; i++) {
		if (i < frames) host_fake_lua_frame();
		tick(1);
		rc = lua_core_exec_result(seq, reply, 1100, len, &done);
		if (rc != ST_OK) return (u16)rc;
	}
	return done ? ST_OK : ST_BUSY;
}

static void test_tod_session(void)
{
	const struct game_api *g;
	const struct game_title *t = title_for_game(GAME_TOD);
	u8 info[SESSION_INFO_SIZE];

	group("ToD: the title index, the module's name and the session");
	check(t != NULL && t->linked != NULL && t->fp_addr == TOD_FP_ADDR && t->quit_hook_addr == TOD_QUIT_BYTE,
	      "NPEA00452 is game 5, with its fingerprint and quit byte");
	check(t != NULL && t->pad_mask_addr == 0x10691970u, "the pad mask is the game's");
	check(qstreq(title_module_name(GAME_TOD), "tod.sprx"), "its module is tod.sprx");
	check(boot_tod(), "the session reaches INGAME with the game");
	g = session_game();
	check(g != NULL && g->game_id == GAME_TOD && g->lua_desc != NULL, "the game is ToD, with a Lua helper");
	check(session_hot_fits(g), "its hot block fits the pool");

	/* The hot block: Start in the game's order is the OG layout's 0x800. */
	host_set_pad(0x200);
	wrf(0x10691918u, 0.25f);   /* LX */
	wrf(0x10691920u, -0.5f);   /* RX */
	wrf(FAKE_HERO + TOD_HERO_POS, 100.0f);
	wrf(FAKE_HERO + TOD_HERO_POS + 4, 200.0f);
	wrf(FAKE_HERO + TOD_HERO_POS + 8, 30.5f);
	wr32(TOD_CURRENT_LEVEL, 9);
	wr32(TOD_BOLTS, 12345);
	wr32(TOD_CHALLENGE, 2);
	tick(9);
	session_info_copy(info, sizeof(info));
	check(be32_get(info + 44) == 0x800, "the pad mask goes out in the OG layout");
	check(bef32_get(info + 48) == -0.5f && bef32_get(info + 56) == 0.25f, "rx and lx where the wire has them");
	check(bef32_get(info + 32) == 100.0f && bef32_get(info + 36) == 200.0f && bef32_get(info + 40) == 30.5f,
	      "the position is the hero's, two pointers deep");
	check(info[28] == 9, "the planet is Rykan V's id");
	check((info[29] & SESSION_FLAG2_NO_MENU) != 0, "flags2 NO_MENU: ToD has no console menu");
	check(be32_get(info + 64 + 4 * TOD_RO_BOLTS) == 12345 && be32_get(info + 64 + 4 * TOD_RO_CHALLENGE) == 2,
	      "bolts and challenge mode in their readouts");
	host_set_pad(0);

	wr32(TOD_LEVEL_OBJ, 0);
	tick(2);
	session_info_copy(info, sizeof(info));
	check(bef32_get(info + 32) == 0.0f, "between levels there is no position");
	check(session_position_save(0) == ST_BUSY, "and no position to save");
	check(session_die() == ST_BUSY, "nor anyone to kill");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	tick(2);
}

static void test_tod_features(void)
{
	const struct game_api *g = session_game();
	const struct game_unlock *list = NULL;
	const char * const *cats = NULL;
	const struct unlock_field_desc *fields = NULL;
	u8 n = 0, ncat = 0, i, blob[QWARK_MAX_BLOB];
	u32 values[4];
	int found;

	group("ToD: values, cheats and actions");
	check(features_set(TF_BOLTS, 999999999u) == ST_OK && rd32(TOD_BOLTS) == 999999999u, "Bolts, to the game's cap");
	check(features_set(TF_BOLTS, 1000000000u) == ST_BAD_ARG, "and not past it");
	check(features_set(TF_RARITANIUM, 77) == ST_OK && rd32(TOD_RARITANIUM) == 77, "Raritanium");
	check(features_set(TF_CHALLENGE, 1) == ST_OK && rd32(TOD_CHALLENGE) == 1, "Challenge mode, a word");
	check(features_set(TF_INFINITE_HEALTH, 1) == ST_OK && rd32(TOD_NO_DAMAGE) == 1, "Infinite health is the game's no-damage word");
	check(features_set(TF_INFINITE_HEALTH, 0) == ST_OK && rd32(TOD_NO_DAMAGE) == 0, "and off again");
	wr32(TOD_PROXY_HANDLE, 0x00110000u);
	check(features_set(TF_GOD_RATCHET, 1) == ST_OK && rd32(TOD_PROXY_HANDLE) == 0x0011009Au,
	      "God Ratchet: the old trainer's 154 in the proxy handle's low byte, the rest left alone");
	check(features_set(TF_GOD_RATCHET, 0) == ST_OK && rd32(TOD_PROXY_HANDLE) == 0x00110000u, "and 0 there to turn it off");
	wr32(TOD_PROXY_HANDLE, 0);
	host_poke(TOD_PROXY_LOW, (const u8 *)"\x9A", 1);
	tick(LIVE_TICKS_FOR_TEST);
	check((features_toggle_state() >> TF_GOD_RATCHET) & 1u, "LIVE: a non-zero low byte reads as on");
	wr32(TOD_PROXY_HANDLE, 0);
	tick(LIVE_TICKS_FOR_TEST);
	check(!((features_toggle_state() >> TF_GOD_RATCHET) & 1u), "and 0 as off");
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && rd32(TOD_TRANSITION_MIN) == 0,
	      "Fast loads: the transition's minimum time to 0");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && rd32(TOD_TRANSITION_MIN) == 0x40A00000u, "and back to the game's 5 s");
	check(g->fast_loads == GAME_FEATURE(TF_FAST_LOADS), "the fast-loads combo flips it");
	check(features_trigger(TF_DIE) == ST_OK && rdf(FAKE_HERO + TOD_HERO_HEALTH) == 0.0f &&
	      rdf(FAKE_HERO + TOD_HERO_MAX_HEALTH) == 10.0f, "Die: the hit points to 0, the maximum kept");

	check(features_trigger(TF_UNLOCK_SP) == ST_OK && rd32(TOD_SKILL_POINTS) == 0x0FFFFFFFu &&
	      rd32(TOD_SKILL_POINTS + 4) == 0xFFFFFFFFu, "all 60 skill points");
	check(features_trigger(TF_RESET_SP) == ST_OK && rd32(TOD_SKILL_POINTS) == 0 && rd32(TOD_SKILL_POINTS + 4) == 0,
	      "and none");
	check(features_trigger(TF_UNLOCK_GB) == ST_OK && rd32(TOD_GOLD_BOLTS + 5 * TOD_LEVEL_SAVE_LEN) == 0xF &&
	      rd32(TOD_GOLD_BOLTS + 1 * TOD_LEVEL_SAVE_LEN) == 0x1 && rd32(TOD_GOLD_BOLTS) == 0,
	      "every gold bolt, each level as many as it has");
	check(features_trigger(TF_RESET_GB) == ST_OK && rd32(TOD_GOLD_BOLTS + 5 * TOD_LEVEL_SAVE_LEN) == 0, "and none");
	check(features_trigger(14) == ST_NOT_FOUND && features_trigger(15) == ST_NOT_FOUND,
	      "the RYNO plans' two buttons are retired");

	group("ToD: Infinite ammo holds the ammo without a code patch");
	host_poke(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_OWNED, (const u8 *)"\1", 1);
	wrf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 50.0f);
	check(features_set(TF_INFINITE_AMMO, 1) == ST_OK && rd32(TOD_INFINITE_AMMO) == 1, "the game's own flag goes up");
	tick(12);
	wrf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 41.0f);
	tick(8);
	check(rdf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 50.0f, "a shot's ammo comes back");
	wrf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 60.0f);
	tick(8);
	wrf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 59.0f);
	tick(8);
	check(rdf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 60.0f, "a pickup is held from then on");
	host_poke(TOD_WEAPONS + 23 * TOD_WEAPON_STRIDE + TOD_WS_OWNED, (const u8 *)"\1", 1);
	wrf(TOD_WEAPONS + 23 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 0.0f);
	wrf(TOD_WEAPONS + 21 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 0.0f);
	tick(8);
	check(rdf(TOD_WEAPONS + 23 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 3.0f, "an owned Visi-Copter at 0 is filled, to 3");
	check(rdf(TOD_WEAPONS + 21 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 0.0f, "a device not owned is left alone");
	wrf(TOD_WEAPONS + 23 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 2.0f);
	tick(8);
	check(rdf(TOD_WEAPONS + 23 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 3.0f, "and one used comes back");
	check(tod_device_max_ammo(16) == 0x40800000u && tod_device_max_ammo(21) == 0x3F800000u &&
	      tod_device_max_ammo(15) == 0 && tod_device_max_ammo(24) == 0, "the devices' full counts, 16..23 alone");
	check(features_set(TF_INFINITE_AMMO, 0) == ST_OK && rd32(TOD_INFINITE_AMMO) == 0, "off");
	wrf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO, 3.0f);
	tick(8);
	check(rdf(TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 3.0f, "and the ammo is the game's again");
	wr32(TOD_INFINITE_AMMO, 1);
	tick(LIVE_TICKS_FOR_TEST);
	check((features_toggle_state() >> TF_INFINITE_AMMO) & 1u, "LIVE: the flag set by the game reads as on");
	wr32(TOD_INFINITE_AMMO, 0);
	tick(LIVE_TICKS_FOR_TEST);

	group("ToD: positions");
	wrf(FAKE_HERO + TOD_HERO_POS, 1.0f);
	wrf(FAKE_HERO + TOD_HERO_POS + 4, 2.0f);
	wrf(FAKE_HERO + TOD_HERO_POS + 8, 3.0f);
	wr32(FAKE_HERO + 0x1290, 0xDEADBEEFu);
	check(g->save_blob(blob, &n) == ST_OK && n == TOD_POS_BLOB_LEN && g->blob_len == TOD_POS_BLOB_LEN,
	      "a slot is 48 bytes of the hero's motion");
	wrf(FAKE_HERO + TOD_HERO_POS, 9.0f);
	wr32(FAKE_HERO + 0x1290, 0x12345678u);
	check(g->load_blob(blob, n) == ST_OK && rdf(FAKE_HERO + TOD_HERO_POS) == 1.0f &&
	      rdf(FAKE_HERO + TOD_HERO_PREV_POS) == 1.0f && rdf(FAKE_MOBY + TOD_MOBY_POS) == 1.0f &&
	      rdf(FAKE_MOBY + TOD_MOBY_POS + 8) == 3.0f, "back: the position, the previous one and the moby's");
	check(rd32(FAKE_HERO + 0x1290) == 0x12345678u, "and never the handle after the blob");
	check(g->load_blob(blob, 30) == ST_BAD_ARG, "a blob of another length is refused");

	group("ToD: the planet load is the game's pending request");
	check(session_planet_load(2, 0) == ST_OK && rd32(TOD_LOAD_REQUEST) == 2, "Stratus City asked for");
	check(session_planet_load(3, 0) == ST_BUSY && rd32(TOD_LOAD_REQUEST) == 2, "and never over it");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_GAME_STATE, 3);
	check(session_planet_load(3, 0) == ST_BUSY && rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu, "only in gameplay");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	check(session_planet_load(19, 0) != ST_OK, "no level 19");

	group("ToD: the unlock table");
	check(g->unlock_list(&list, &n, &cats, &ncat, &fields) == ST_OK && ncat == 4 && n == 31 + 12,
	      "four categories: 31 weapons, devices and gadgets, and 12 items");
	found = 1;
	for (i = 0; i < n; i++)
		found &= list[i].id < TOD_UL_OBJ + 12 && strcmp(list[i].name, "Hexagonal Washer") != 0;
	check(found, "no skill point, gold bolt, sector upgrade or washer row");
	found = 1;
	for (i = 0; i < n; i++) found &= list[i].name != NULL && qstrlen(list[i].name) <= 24 && list[i].category < ncat;
	check(found, "every row has a name that fits the wire");
	found = 0;
	for (i = 0; i < TOD_NGOLD_BOLTS; i++) {
		u8 level, bit;
		found += tod_gold_bolt(i, &level, &bit);
	}
	check(found == 32 && !tod_gold_bolt(32, &i, &i), "32 gold bolts, each a level's bit");
	check(tod_unlock_set(3, 1, 3) == ST_OK && *host_mem_ptr(TOD_WEAPONS + 3 * TOD_WEAPON_STRIDE + TOD_WS_LEVEL) == 2 &&
	      rdf(TOD_WEAPONS + 3 * TOD_WEAPON_STRIDE + TOD_WS_XP) == 1760.0f, "a level is the level byte and its XP");
	check(tod_unlock_set(3, 1, 11) == ST_BAD_ARG && tod_unlock_set(3, 1, 0) == ST_BAD_ARG, "V1 to V10 only");
	check(tod_unlock_set(19, 1, 2) == ST_BAD_ARG, "a device has no level");
	check(tod_unlock_set(3, 0, 1) == ST_OK && tod_unlock_set(3, 3, 120) == ST_OK &&
	      rdf(TOD_WEAPONS + 3 * TOD_WEAPON_STRIDE + TOD_WS_AMMO) == 120.0f, "owned, and its ammo as a float");
	check(tod_unlock_set(TOD_UL_OBJ + 5, 0, 1) == ST_OK && rd32(TOD_OBJECTS + 5 * 4) == 1, "the Charge Boots");
	check(tod_unlock_set(TOD_UL_OBJ + 14, 0, 1) == ST_NOT_FOUND && tod_unlock_set(TOD_UL_SP + 59, 0, 1) == ST_NOT_FOUND &&
	      tod_unlock_set(TOD_UL_GB + 7, 0, 1) == ST_NOT_FOUND, "the unlisted rows' ids answer NOT_FOUND");
	g->unlock_list(&list, &n, &cats, &ncat, &fields);
	for (i = 0; i < n && list[i].id != 3; i++) {}
	memset(values, 0, sizeof(values));
	check(i < n && g->unlock_read(&list[i], values) == ST_OK && values[0] == 1 && values[1] == 3 && values[3] == 120,
	      "and read back: owned, V3, 120");
	for (i = 0; i < n && list[i].id != TOD_UL_OBJ + 5; i++) {}
	memset(values, 0, sizeof(values));
	check(i < n && g->unlock_read(&list[i], values) == ST_OK && values[0] == 1, "the Charge Boots read as owned");
}

/* Flag n as the game tests it: bit n & 63 of the big-endian u64 word n >> 6. */
static int game_flag(u32 n)
{
	u8 b[8];
	u32 hi, lo, bit = n & 63u;

	host_peek(TOD_LEVEL_FLAGS + (n >> 6) * 8u, b, 8);
	hi = be32_get(b);
	lo = be32_get(b + 4);
	return bit >= 32 ? (int)((hi >> (bit - 32)) & 1u) : (int)((lo >> bit) & 1u);
}

static void set_game_flag(u32 n)
{
	u8 b = 0;

	host_peek(TOD_LEVEL_FLAGS + TOD_LF_BYTE(n), &b, 1);
	b |= (u8)(1u << (n & 7u));
	host_poke(TOD_LEVEL_FLAGS + TOD_LF_BYTE(n), &b, 1);
}

static const struct levelflag_name *flag_named(const struct levelflag_names *t, u16 number)
{
	u16 i;
	for (i = 0; t != NULL && i < t->nflags; i++) if (t->flags[i].number == number) return &t->flags[i];
	return NULL;
}

static void test_tod_level_flags(void)
{
	const struct game_api *g = session_game();
	const struct levelflag_names *t;
	const struct levelflag_name *f;
	u8 region[64], seen[TOD_NLEVEL_FLAGS], zero[TOD_LEVEL_FLAGS_LEN];
	u16 len = 0, i;
	u32 wire;
	int ok;

	group("ToD: the level flags, one u64 bit array for the whole save");
	check(g->levelflags_get != NULL && g->levelflags_set != NULL && g->levelflags_reset != NULL &&
	      g->levelflag_names != NULL, "all three hooks and the names");
	memset(zero, 0, sizeof(zero));
	host_poke(TOD_LEVEL_FLAGS, zero, TOD_LEVEL_FLAGS_LEN);
	set_game_flag(39);
	check(g->levelflags_get(4, region, sizeof(region), &len) == ST_OK && len == TOD_LEVEL_FLAGS_LEN &&
	      region[3] == 0x80 && game_flag(39) && !game_flag(38) && !game_flag(31),
	      "40 bytes; flag 39 is word 0's bit 39, byte 3's top bit");
	check(g->levelflags_get(19, region, sizeof(region), &len) == ST_BAD_ARG &&
	      g->levelflags_get(0, region, 39, &len) == ST_FULL, "no planet 19, and the region takes 40 bytes");
	check(TOD_LF_BIT(0) == 7u * 8u && TOD_LF_BIT(63) == 7u && TOD_LF_BIT(64) == 15u * 8u &&
	      TOD_LF_BIT(291) == (32u + 7u - 4u) * 8u + 3u, "the bit numbers: the big-endian words' bytes");
	check(g->levelflags_set(0, TOD_LF_BYTE(291), (u8)(1u << (291u & 7u))) == ST_OK && game_flag(291) &&
	      g->levelflags_set(0, TOD_LEVEL_FLAGS_LEN, 1) == ST_BAD_ARG, "a byte write; none past the region");

	t = g->levelflag_names();
	check(t != NULL && t->nflags == TOD_NLEVEL_FLAGS && t->ngroups == 20 && t->groups[19].planet == LEVELFLAG_NO_PLANET,
	      "292 names in 20 groups, the last one no planet's");
	memset(seen, 0, sizeof(seen));
	ok = t != NULL;
	wire = 1u + 20u * LEVELFLAG_GROUP_WIRE_SIZE + 2u;
	for (i = 0; ok && i < t->nflags; i++) {
		f = &t->flags[i];
		ok = f->number < TOD_NLEVEL_FLAGS && !seen[f->number] && f->bit == TOD_LF_BIT(f->number) &&
		     f->group < t->ngroups && (i == 0 || f->group >= t->flags[i - 1].group) &&
		     f->name != NULL && qstrlen(f->name) > 0 && qstrlen(f->name) <= 255u;
		if (ok) seen[f->number] = 1;
		wire += LEVELFLAG_NAME_WIRE_HEAD + qstrlen(f->name);
	}
	check(ok, "every flag once, its bit its number's, the groups in order");
	check(wire < 12000u, "LEVELFLAGS_NAMES fits a reply with room to spare");
	for (i = 0, ok = t != NULL; ok && i + 1u < t->ngroups; i++) ok = t->groups[i].planet == i;
	check(ok, "groups 0 to 18 are the planets, in LEVEL_ order");
	f = flag_named(t, 39);
	check(f != NULL && qstreq(f->name, "LVL_COBALIA_BRIDGE_EXTENDED") && f->group == 1, "the legacy names: 39 on Cobalia");
	f = flag_named(t, 291);
	check(f != NULL && qstreq(f->name, "LVL_FAST_CLANK_MOVIE_1_PLAYED") && f->group == 3, "291 on Fastoon");
	f = flag_named(t, 278);
	check(f != NULL && qstreq(f->name, "SC_PLAYED_BONUS_TARGET_MSG") && f->group == 13, "278 on Space Combat III");
	f = flag_named(t, 0);
	check(f != NULL && qstreq(f->name, "GLOBAL_FLAG_TEST") && f->group == 19, "0, the game's own name, under Other");

	/* Cobalia's reset: 39..71, 186 and 187 go; Metropolis's 38, the RYNO's 33 and the neighbours stay. */
	host_poke(TOD_LEVEL_FLAGS, zero, TOD_LEVEL_FLAGS_LEN);
	for (i = 30; i < 75; i++) set_game_flag(i);
	set_game_flag(186);
	set_game_flag(187);
	set_game_flag(188);
	check(g->levelflags_reset(1) == ST_OK, "LEVELFLAGS_RESET on Cobalia");
	ok = 1;
	for (i = 39; i <= 71; i++) ok &= !game_flag(i);
	check(ok && !game_flag(186) && !game_flag(187), "its 35 flags are clear");
	check(game_flag(38) && game_flag(33) && game_flag(72) && game_flag(74) && game_flag(188),
	      "and Metropolis's, the RYNO's, the Fight Fest's and Stratus City's are not");
	check(g->levelflags_reset(19) == ST_BAD_ARG, "no planet 19");
	host_poke(TOD_LEVEL_FLAGS, zero, TOD_LEVEL_FLAGS_LEN);
}

static void test_tod_lua_console(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u8 reply[1200];
	u32 len = 0;

	group("ToD: Lua on a console: qwark installs the helper on the first LUA_EXEC");
	check(lua_core_status(reply, sizeof(reply), &len) == ST_OK && reply[0] == LUA_INSTALLED_NO &&
	      len == LUA_STATUS_HEAD, "LUA_STATUS: not installed");
	check(rd32(d->hooks[0].addr) == d->hooks[0].orig, "nothing written before it is asked for");
	wr32(TOD_LUA_STATE, 0);
	check(exec("return _VERSION", 4, reply, &len) == ST_NOT_FOUND && rd32(d->hooks[0].addr) == d->hooks[0].orig,
	      "no level: NOT_FOUND, and still nothing installed");
	wr32(TOD_LUA_STATE, 0x30030000u);
	check(exec("return _VERSION", 4, reply, &len) == ST_OK, "the Lua test runs");
	check(rd32(d->hooks[0].addr) == d->hooks[0].value && rd32(d->mailbox) == LUAMB_MAGIC, "the helper is in");
	check(len == LUA_EXEC_REPLY_HEAD + 9 && be32_get(reply) == LUAMB_ST_OK && reply[8] == LUAMB_T_STRING &&
	      memcmp(reply + LUA_EXEC_REPLY_HEAD, "Lua 5.0.2", 9) == 0, "it answers Lua 5.0.2");
	check(exec("time_set_scale(0.5) return _VERSION", 4, reply, &len) == ST_OK && rd32(TOD_GAME_CLOCK) == 0x3F000000u,
	      "the proof of concept: half speed");
	check(exec("time_set_scale(1) return _VERSION", 4, reply, &len) == ST_OK && rd32(TOD_GAME_CLOCK) == 0x3F800000u,
	      "and back to 1");
	check(exec("error(\"x\")", 4, reply, &len) == ST_OK && be32_get(reply) == LUAMB_ST_ERRRUN &&
	      memcmp(reply + LUA_EXEC_REPLY_HEAD, "qwark:1: x", 10) == 0, "a runtime error comes back as one");
	check(exec("return 1", 0, reply, &len) == ST_OK && be32_get(reply) == LUAMB_ST_NOT_RUNNING,
	      "no gameplay frame for a second and a half: NOT_RUNNING, withdrawn");
	check(rd32(d->mailbox + LUAMB_OFF_CANCEL) == rd32(d->mailbox + LUAMB_OFF_REQ), "the request was cancelled");
	host_fake_lua_frame();
	check(rd32(d->mailbox + LUAMB_OFF_STATUS) == LUAMB_ST_CANCELLED, "and the helper never ran it");
	check(exec("return _VERSION", 4, reply, &len) == ST_OK && be32_get(reply) == LUAMB_ST_OK, "the next one runs");
	check(lua_core_status(reply, sizeof(reply), &len) == ST_OK && reply[0] == LUA_INSTALLED_YES &&
	      reply[1] == LUA_PHASE_IDLE && be32_get(reply + 4) != 0 && be32_get(reply + 8) == 0x30030000u,
	      "LUA_STATUS: installed, idle, a heartbeat, the lua_State");
	{
		u8 req[8] = { 0, 0, 0, 0 };
		u32 seq;
		check(lua_core_exec_start(req, 4, &seq) == ST_BAD_ARG, "an empty chunk is refused");
		be16_put(req, 4);
		be16_put(req + 2, 1);
		check(lua_core_exec_start(req, 8, &seq) == ST_BAD_ARG, "and flags other than 0");
	}
}

static void test_tod_lua_rpcs3(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u8 reply[4096];
	u32 len = 0;

	group("ToD: Lua under RPCS3: the patch, or UNSUPPORTED");
	check(quit(), "the game quits");
	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(boot_tod(), "ToD boots under RPCS3 without the patch");
	tick(4);
	check(lua_core_status(reply, sizeof(reply), &len) == ST_OK && reply[0] == LUA_INSTALLED_NO, "LUA_STATUS: not in");
	check(exec("return _VERSION", 4, reply, &len) == ST_UNSUPPORTED && rd32(d->hooks[0].addr) == d->hooks[0].orig,
	      "LUA_EXEC is UNSUPPORTED and writes no code");
	check(lua_core_patch(reply, sizeof(reply), &len) == ST_OK && be16_get(reply) == lh_patch_count(d) &&
	      lh_patch_count(d) > 300, "LUA_PATCH: the helper's words");
	check(quit(), "the game quits");
	host_set_lua_patch(1);
	check(boot_tod(), "and boots with the patch in");
	tick(4);
	check(lua_core_status(reply, sizeof(reply), &len) == ST_OK && reply[0] == LUA_INSTALLED_YES, "LUA_STATUS: in");
	check(exec("return _VERSION", 4, reply, &len) == ST_OK && memcmp(reply + LUA_EXEC_REPLY_HEAD, "Lua 5.0.2", 9) == 0,
	      "and LUA_EXEC answers through PINE's data writes alone");
	check(quit(), "the game quits");
	host_set_lua_patch(0);
	host_set_can_patch_code(1);
	host_set_emulator(0);
}

void test_tod_game(void)
{
	test_tod_session();
	test_tod_features();
	test_tod_level_flags();
	test_tod_lua_console();
	test_tod_lua_rpcs3();
	quit();
}
