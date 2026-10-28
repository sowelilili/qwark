/*
 * Revision 1.27: Tools of Destruction (game 5) on qwark's fake console, linked
 * in as every PC build links it: the title index entry, the hot block (the
 * pad's remap, the analogs, the planet, the hero's position two pointers
 * deep), the features, positions, Die, the planet load, the unlock table, and
 * the core's Lua ops (src/core/luacore.c) against the simulator's stand-in for
 * the helper, on a console (qwark installs it) and under RPCS3 (the patch is in
 * at boot, or it is not), and the savefile manager over the game's own save
 * system (revision 1.28), against the simulator's stand-in for it.
 *
 * The mailbox's own codec and state machine are test/test_tod.c's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/features.h"
#include "../src/core/luacore.h"
#include "../src/core/luamb.h"
#include "../src/core/savefile.h"
#include "../src/core/config.h"
#include "../src/core/util.h"
#include "../src/core/autosplit.h"
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

/* ------------------------------------------- the autosplitter, rev 1.28 */

#define AS_STATE   0x30030000u   /* the simulator's lua_State (backend_fake.c, seed_tod) */

static u8 g_ev[5 + AUTOSPLIT_RING_SLOTS * AUTOSPLIT_EVENT_SIZE];

/* How many events of this kind and code came after `seq`; *arg the last one's. */
static u32 as_count(u32 seq, u8 kind, u8 code, u32 *arg)
{
	u32 n, i, hits = 0;

	if (autosplit_encode_events(seq, g_ev, sizeof(g_ev)) < 5) return 0;
	n = g_ev[4];
	for (i = 0; i < n; i++) {
		const u8 *e = g_ev + 5 + i * AUTOSPLIT_EVENT_SIZE;
		if (e[8] != kind || e[9] != code) continue;
		hits++;
		if (arg != NULL) *arg = be32_get(e + 12);
	}
	return hits;
}

static u32 as_all(u32 seq)
{
	if (autosplit_encode_events(seq, g_ev, sizeof(g_ev)) < 5) return 0;
	return g_ev[4];
}

static void wr8(u32 addr, u8 v)
{
	host_poke(addr, &v, 1);
}

static u8 rd8(u32 addr)
{
	u8 b = 0;
	host_peek(addr, &b, 1);
	return b;
}

/* A level's completed missions, the first `n` of `ids`. */
static void put_missions(u32 level, const u32 *ids, u32 n)
{
	u32 base = TOD_MISSIONS_DONE + level * TOD_MISSION_LIST_LEN, i;

	for (i = 0; i < n; i++) {
		wr32(base + i * TOD_MISSION_STRIDE, ids[i]);
		wr32(base + i * TOD_MISSION_STRIDE + 4, ids[i] + 79);
		wr32(base + i * TOD_MISSION_STRIDE + 8, 2);
	}
	wr32(base + TOD_MISSION_COUNT, n);
}

/* The position of the first event of this kind and code after `seq`, -1 for none. */
static int as_first(u32 seq, u8 kind, u8 code)
{
	u32 n, i;

	if (autosplit_encode_events(seq, g_ev, sizeof(g_ev)) < 5) return -1;
	n = g_ev[4];
	for (i = 0; i < n; i++) {
		const u8 *e = g_ev + 5 + i * AUTOSPLIT_EVENT_SIZE;
		if (e[8] == kind && e[9] == code) return (int)i;
	}
	return -1;
}

/* MOVIE_METROPOLIS_TACHYON, set or clear, in the five big-endian words. */
static void metropolis_beaten(int on)
{
	u32 at = TOD_LEVEL_FLAGS + TOD_LF_BYTE(TOD_LF_METROPOLIS_TACHYON);
	u8 b = rd8(at), bit = (u8)(1u << (TOD_LF_METROPOLIS_TACHYON & 7u));

	wr8(at, on ? (u8)(b | bit) : (u8)(b & ~bit));
}

static void test_tod_autosplit_rows(void)
{
	const struct game_api *g = session_game();
	const struct autosplit_desc *d;
	u8 n = 0, i;

	group("ToD: the autosplitter's rows");
	check(g != NULL && g->autosplit_describe != NULL, "ToD describes an autosplitter");
	if (g == NULL || g->autosplit_describe == NULL) return;
	d = g->autosplit_describe(&n);
	check(n == 6, "six rows");
	if (n != 6) return;
	check(d[0].code == TOD_AS_PLANET && d[0].kind == AUTOSPLIT_SPLIT &&
	      d[0].flags == (AUTOSPLIT_FLAG_DEFAULT | AUTOSPLIT_FLAG_ROUTE) && qstreq(d[0].label, "Planet entered"),
	      "code 1 is the planet split, on, with the route");
	check(d[1].code == TOD_AS_TACHYON && d[1].flags == AUTOSPLIT_FLAG_DEFAULT && qstreq(d[1].label, "Tachyon defeated"),
	      "Tachyon's defeat is the final split, on by default");
	check(d[2].code == TOD_AS_CREDITS && d[2].flags == 0 && d[3].code == TOD_AS_MISSION && d[3].flags == 0,
	      "the credits and every mission are there, off");
	check(d[4].code == TOD_AS_LOAD && d[4].kind == AUTOSPLIT_LOAD_START &&
	      d[4].flags == (AUTOSPLIT_FLAG_DEFAULT | AUTOSPLIT_FLAG_HOLD) && d[4].param_us == 0 &&
	      qstreq(d[4].label, "Long load"),
	      "the long load holds game time (HOLD, rev 1.29), no NORMALISE");
	check(d[5].code == TOD_AS_QUIT && d[5].kind == AUTOSPLIT_PAUSE &&
	      d[5].flags == (AUTOSPLIT_FLAG_DEFAULT | AUTOSPLIT_FLAG_NORMALISE) && d[5].param_us == 19950000u,
	      "the quit to the XMB costs the old script's 19.95 s");
	for (i = 0; i < n; i++) check(strlen(d[i].label) <= AUTOSPLIT_LABEL_LEN, "a label fits the wire");
}

/*
 * The old autosplitter's is_loading (tod-autosplitter-src/ratchetron.c):
 * savePlanetID == 0 && loadScreenType > 1, two bytes, 0x1029C55B and
 * 0x102034FB, the low bytes of the two words. Every case it decides.
 */
static void test_tod_long_load_table(void)
{
	static const struct { s32 level; u8 screen; int want; const char *what; } cases[] = {
		{ 0,          0,    0, "Metropolis, load screen 0 (just begun, the flight): no" },
		{ 0,          1,    0, "Metropolis, 1 (the arrival, and every frame after it): no" },
		{ 0,          2,    1, "Metropolis, 2: a long load" },
		{ 0,          3,    1, "Metropolis, 3: a long load (never picked, still > 1)" },
		{ 0,          4,    1, "Metropolis, 4: a long load" },
		{ 0,          5,    1, "Metropolis, 5: a long load" },
		{ 0,          0xFF, 1, "Metropolis, 255: a long load (a byte, unsigned)" },
		{ 1,          4,    0, "Cobalia, 4: never (only level 0 pauses)" },
		{ 9,          2,    0, "Rykan V, 2: never" },
		{ 18,         5,    0, "Fastoon II, 5: never" },
		{ 0x100,      2,    1, "a level word of 256: its low byte is 0, a long load" },
		{ -1,         2,    0, "a level of -1: its low byte is 255, no" },
		{ 0x200,      1,    0, "low byte 0, screen 1: no" },
	};
	u32 i;

	group("ToD: the long load is the old autosplitter's is_loading, case by case");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		check(tod_long_load(cases[i].level, cases[i].screen) == cases[i].want, cases[i].what);
}

static void test_tod_autosplit_planets(void)
{
	u32 seq, arg = 0;

	group("ToD: planet splits, and ordinary loads that hold nothing");
	wr32(TOD_CURRENT_LEVEL, 1);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LOAD_SCREEN, 1);
	metropolis_beaten(1);
	tick(4);

	/* Cobalia to Rykan V: the destination is written, then the level goes down. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 9);
	tick(2);
	check(as_all(seq) == 0, "the level word alone is no split (Lua_set_save_level writes it too)");
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 9, "a planet change splits, arg Rykan V");
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0, "no reset on a played file");
	wr32(TOD_LOAD_SCREEN, 0);
	tick(10);
	wr32(TOD_LOAD_SCREEN, 4);
	tick(30);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 0,
	      "a long load screen into Rykan V holds nothing: the old splitter's level 0 only");
	wr32(TOD_LOAD_SCREEN, 1);
	wr32(TOD_LUA_STATE, AS_STATE + 0x100);
	tick(2);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 0, "and so nothing to end");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 1, "one split for one trip");

	/* The same planet again: a reload, no split. */
	seq = autosplit_latest_seq();
	wr32(TOD_LUA_STATE, 0);
	tick(5);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_all(seq) == 0, "reloading the planet is no split and no load event");

	/* A death or a checkpoint keeps the lua_State: nothing at all. */
	seq = autosplit_latest_seq();
	wrf(FAKE_HERO + TOD_HERO_HEALTH, 0.0f);
	tick(10);
	wrf(FAKE_HERO + TOD_HERO_HEALTH, 10.0f);
	tick(4);
	check(as_all(seq) == 0, "a death or a checkpoint emits nothing");

	/* Quit to the title, then a save of another planet: no split either way. */
	seq = autosplit_latest_seq();
	wr32(TOD_LUA_STATE, 0);
	tick(4);
	wr8(TOD_FRONT_END, 1);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "going to the title is no split");
	seq = autosplit_latest_seq();
	tick(40);
	check(as_all(seq) == 0, "the title emits nothing");
	wr32(TOD_CURRENT_LEVEL, 5);
	tick(2);
	wr8(TOD_FRONT_END, 0);
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "a save is no run start");
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "loading a save is no split");

	/* From there, a planet change splits again. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 6);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 6, "the next trip splits");

	/* A load shorter than a tick (Fast loads): a new lua_State where the last tick read another. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 10);
	wr32(TOD_LUA_STATE, AS_STATE + 0x300);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 10, "a load between two ticks still splits");
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 1, "and the same planet's new state does not again");

	/* With the flag set, Metropolis is a planet like any other. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 0);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 0 &&
	      as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "Sargasso to Metropolis on a file past it: a planet split, arg 0, no start");
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
}

/*
 * The long load through the session, edge by edge, as the old SPRX sent
 * CMD_PAUSE and CMD_UNPAUSE on the edges of is_loading at its poll.
 */
static void test_tod_autosplit_long_load(void)
{
	u32 seq;

	group("ToD: the long load holds game time exactly where the old splitter paused");
	wr32(TOD_CURRENT_LEVEL, 3);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_LOAD_SCREEN, 1);
	metropolis_beaten(1);
	tick(4);

	/* Fastoon to Metropolis: the destination is 0 by the time the load screen runs. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 0);
	wr32(TOD_LUA_STATE, 0);
	wr32(TOD_LOAD_SCREEN, 0);
	tick(4);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 0, "screen 0, the flight out: not held");
	wr32(TOD_LOAD_SCREEN, 2);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 1, "a long load screen: LOAD_START the same tick");
	wr32(TOD_LOAD_SCREEN, 5);
	tick(20);
	wr32(TOD_LOAD_SCREEN, 4);
	tick(20);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 1 &&
	      as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 0, "one long screen after another is one hold");
	wr32(TOD_LOAD_SCREEN, 1);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "the arrival, screen 1: LOAD_END the same tick");
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(4);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "the level coming up adds nothing");

	/* Its quirks, kept: the condition alone decides, level up or not, and only low bytes count. */
	seq = autosplit_latest_seq();
	wr32(TOD_LOAD_SCREEN, 3);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 1,
	      "level 0 and a screen above 1 holds with a level up too (the old condition knows no level)");
	wr32(TOD_LOAD_SCREEN, 0x201);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "0x201's low byte is 1: released");
	wr32(TOD_LOAD_SCREEN, 0x102);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 2, "0x102's low byte is 2: held");
	wr32(TOD_CURRENT_LEVEL, 0x100);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "a level word of 0x100 still reads 0: still held");
	wr32(TOD_CURRENT_LEVEL, 7);
	tick(1);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 2, "the level leaving 0 releases it");
	wr32(TOD_LOAD_SCREEN, 1);
	tick(1);

	/* The title: the old SPRX evaluated it there too. */
	seq = autosplit_latest_seq();
	wr32(TOD_LUA_STATE, 0);
	wr8(TOD_FRONT_END, 1);
	wr32(TOD_CURRENT_LEVEL, 0);
	wr32(TOD_LOAD_SCREEN, 4);
	tick(2);
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 1, "the title with level 0 and screen 4: held");
	wr32(TOD_LOAD_SCREEN, 1);
	tick(2);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "and released");
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_CURRENT_LEVEL, 3);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(4);
}

/* A Tachyon in the fake pool: slot `slot` holds `obj`, a TachyonBoss with `hp` health. */
#define AS_POOL     0x30100000u
#define AS_POOL_MAX 40u
static void put_tachyon(u32 slot, u32 obj, u32 handle, f32 hp)
{
	u32 m = AS_POOL + slot * TOD_MOBY_STRIDE;

	wr32(m + TOD_MOBY_OBJECT, obj);
	wr32(m + TOD_MOBY_HANDLE, handle);
	wr32(obj, TOD_VTBL_TACHYON);
	wrf(obj + TOD_TACHYON_HEALTH, hp);
}

static void test_tod_autosplit_runs(void)
{
	static const u32 before[] = { 635, 636, 637 };
	u32 ids[4], seq, arg = 0, i;

	group("ToD: run starts on the Metropolis flag, Tachyon and the credits");

	/* A new game: the title, a file with MOVIE_METROPOLIS_TACHYON clear, Metropolis. */
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wr8(TOD_FRONT_END, 1);
	wr32(TOD_LOAD_SCREEN, 1);
	tick(4);
	metropolis_beaten(0);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	seq = autosplit_latest_seq();
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_LOAD_SCREEN, 4);   /* no level to fly from: a long load screen at once */
	tick(4);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 1,
	      "a new game is RESET and START, whatever the play time says");
	check(as_first(seq, AUTOSPLIT_RESET, 0) < as_first(seq, AUTOSPLIT_START, 0) &&
	      as_first(seq, AUTOSPLIT_START, 0) < as_first(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD),
	      "RESET, START, then the long load's hold, in that order");
	wr32(TOD_LOAD_SCREEN, 1);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "the hold ends at the arrival");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "Metropolis is no planet split");

	/* A Metropolis reloaded from Metropolis, flag still clear: the run goes on. */
	seq = autosplit_latest_seq();
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0, "Metropolis reloaded from Metropolis is no reset");

	/* A Metropolis save made after the first planet: flag set, no start, no split. */
	metropolis_beaten(1);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wr8(TOD_FRONT_END, 1);
	tick(4);
	seq = autosplit_latest_seq();
	wr8(TOD_FRONT_END, 0);
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0,
	      "a Metropolis save with the flag set is no run start");
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "and, out of the title, no split");

	/* Fastoon II: the missions as the level left them, and two Tachyons in the pool. */
	wr32(TOD_CURRENT_LEVEL, 18);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	for (i = 0; i < 3; i++) ids[i] = before[i];
	put_missions(18, ids, 3);
	wr32(TOD_MOBY_POOL, AS_POOL);
	wr32(TOD_MOBY_POOL + TOD_MOBY_POOL_MAX, AS_POOL_MAX);
	put_tachyon(7, 0x30200000u, 0x02F90007u, 79644.0f);
	put_tachyon(25, 0x30210000u, 0x02F90019u, 106192.0f);
	wr32(TOD_LUA_STATE, AS_STATE + 0x200);
	tick(20);   /* the pool, 41 slots at four a tick */
	seq = autosplit_latest_seq();
	wrf(0x30210000u + TOD_TACHYON_HEALTH, 26548.0f);
	tick(4);
	check(as_all(seq) == 0, "Tachyon hurt, held at a phase's threshold: nothing");
	ids[3] = 638;
	put_missions(18, ids, 4);
	tick(8);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_MISSION, &arg) == 1 && arg == ((18u << 16) | 638u),
	      "a mission done in the fight is the mission row");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, NULL) == 0, "and no Tachyon split");
	wrf(0x30210000u + TOD_TACHYON_HEALTH, 0.0f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, &arg) == 1 && arg == 0,
	      "the killing hit, health 0: the final split, the same tick");
	tick(20);
	wrf(0x30200000u + TOD_TACHYON_HEALTH, 0.0f);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, NULL) == 1, "only once a level, whichever Tachyon falls next");

	/* The level again (a checkpoint load): Tachyon already dead is learnt, not split on. */
	seq = autosplit_latest_seq();
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE + 0x400);
	tick(20);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, NULL) == 0, "a Tachyon found dead splits nothing");
	wrf(0x30210000u + TOD_TACHYON_HEALTH, 106192.0f);
	tick(2);
	wrf(0x30210000u + TOD_TACHYON_HEALTH, -1.0f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, NULL) == 1, "one seen alive, then not, does");

	/* A slot handed to something else is forgotten. */
	seq = autosplit_latest_seq();
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wrf(0x30210000u + TOD_TACHYON_HEALTH, 106192.0f);
	wr32(TOD_LUA_STATE, AS_STATE + 0x500);
	tick(20);
	wr32(AS_POOL + 25 * TOD_MOBY_STRIDE + TOD_MOBY_HANDLE, 0x03000019u);
	tick(2);
	wrf(0x30210000u + TOD_TACHYON_HEALTH, 0.0f);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, NULL) == 0, "a slot whose handle changed is not Tachyon any more");

	/* Not on another planet. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 9);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	put_tachyon(25, 0x30210000u, 0x02F90019u, 106192.0f);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(20);
	wrf(0x30210000u + TOD_TACHYON_HEALTH, 0.0f);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_TACHYON, NULL) == 0, "a TachyonBoss off Fastoon II is never the split");
	wr32(TOD_CURRENT_LEVEL, 18);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE + 0x200);
	tick(4);

	/* The credits. */
	seq = autosplit_latest_seq();
	wr32(TOD_GAME_STATE, TOD_STATE_CREDITS);
	tick(3);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_CREDITS, &arg) == 1 && arg == 18, "the credits split, on Fastoon II");

	/* Challenge mode out of the credits: the file built from a fresh one, flag clear, into Metropolis. */
	seq = autosplit_latest_seq();
	metropolis_beaten(0);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	wr32(TOD_LUA_STATE, 0);
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 1,
	      "a challenge mode start is RESET and START");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "and no planet split with it");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LUA_STATE, AS_STATE);
	put_missions(18, ids, 0);
	wr32(TOD_MOBY_POOL, 0);
	metropolis_beaten(1);
	tick(4);
}

static void test_tod_autosplit_quit(void)
{
	u32 seq;
	int i, in = 0;

	group("ToD: the quit to the XMB pauses, and the game coming back resumes");
	seq = autosplit_latest_seq();
	check(quit(), "quit to the XMB");
	check(as_count(seq, AUTOSPLIT_PAUSE, TOD_AS_QUIT, NULL) == 1, "PAUSE with code 3 as it left");
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 0, "with no long load open, nothing else");

	/* A boot as the game makes it: no level and no title yet. */
	seq = autosplit_latest_seq();
	host_boot_as(TOD_TITLE, "tod");
	wr32(TOD_LUA_STATE, 0);
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_CURRENT_LEVEL, 0);
	wr32(TOD_LOAD_SCREEN, 4);
	for (i = 0; i < 4000 && !in; i++) {
		tick(1);
		in = session_state() == SESSION_INGAME && session_game() != NULL;
	}
	check(in, "ToD is back");
	tick(120);
	check(as_count(seq, AUTOSPLIT_RESUME, TOD_AS_QUIT, NULL) == 0, "no RESUME while the game boots");
	check(as_all(seq) == 0, "and no long load either, level 0 and screen 4 or not: a boot is the pause's");
	wr8(TOD_FRONT_END, 1);
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESUME, TOD_AS_QUIT, NULL) == 1, "RESUME once the front end is up");
	check(as_first(seq, AUTOSPLIT_RESUME, TOD_AS_QUIT) >= 0 &&
	      as_first(seq, AUTOSPLIT_RESUME, TOD_AS_QUIT) < as_first(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD),
	      "and then the long load the old splitter's first poll would have found");
	tick(10);
	check(as_count(seq, AUTOSPLIT_RESUME, TOD_AS_QUIT, NULL) == 1, "only once");
	check(rd8(TOD_FRONT_END) == 1, "and the game's byte is left alone");

	/* A quit during a long load: the hold closes before the quit's pause opens. */
	seq = autosplit_latest_seq();
	check(quit(), "quit during the long load");
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1 &&
	      as_first(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD) < as_first(seq, AUTOSPLIT_PAUSE, TOD_AS_QUIT),
	      "LOAD_END, then PAUSE");

	/* Missed the title: a level's lua_State resumes it too. */
	seq = autosplit_latest_seq();
	wr8(TOD_FRONT_END, 0);
	host_boot_as(TOD_TITLE, "tod");
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_LOAD_SCREEN, 1);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME) break;
	}
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESUME, TOD_AS_QUIT, NULL) == 1, "a level already up resumes at once");
}

/* ------------------------------------- the savefile manager, revision 1.28 */

#define SM(off) (TOD_SAVE_MANAGER + (off))
#define LOAD_ID_LO SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4)
#define LOAD_ST    SM(TOD_SM_LOAD_THREAD + TOD_ST_STATE)

static u8 sf_info(u8 *installed, u8 *running, u8 *pending, u32 *size, u8 *error)
{
	u8 supported = 0;
	u32 done = 0, total = 0;

	savefile_info(&supported, installed, running, pending, size);
	savefile_transfer(&done, &total, error);
	return supported;
}

/* Ticks, with a frame of the game's save system after each when `frames`, until nothing is pending. */
static int sf_settle(int frames, int max)
{
	u8 installed, running, pending = 0xFF, error;
	u32 size;
	int i;

	for (i = 0; i < max; i++) {
		tick(1);
		if (frames) host_fake_tod_save_frame();
		sf_info(&installed, &running, &pending, &size, &error);
		if (pending == 0) return 1;
	}
	return 0;
}

/* The bytes of a file on the fake HDD, up to cap; the length, or -1. */
static long sf_file(const char *path, u8 *out, u32 cap)
{
	plat_file_t f;
	u32 got = 0;

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return -1;
	if (plat_file_read(f, out, cap, &got) != 0) got = 0;
	plat_file_close(f);
	return (long)got;
}

static u8 g_sf_buf[TOD_SAVE_SIZE + 16];

static void test_tod_savefile(void)
{
	const struct game_api *g;
	const struct game_describe *dsc;
	u8 installed = 0, running = 0, pending = 0, error = 0, patch[64];
	u32 size = 0, len = 0, i;
	int sa = 0, la = 0;
	long n;

	group("ToD: the savefile manager, the game's own save system (revision 1.28)");
	check(quit(), "the game quits");
	check(boot_tod(), "ToD boots");
	tick(4);
	g = session_game();
	check(g != NULL && g->savefile_driver != NULL && g->load_setaside != NULL && g->savefile_ready != NULL,
	      "it has a save-system driver, a load-set-aside and a readiness test");

	dsc = g != NULL ? g->describe() : NULL;
	for (i = 0; dsc != NULL && i < dsc->nfeatures; i++) {
		if (dsc->features[i].id == TF_SET_ASIDE && (dsc->features[i].flags & FEATURE_FLAG_SAVE_ASIDE)) sa = 1;
		if (dsc->features[i].id == TF_LOAD_ASIDE && (dsc->features[i].flags & FEATURE_FLAG_LOAD_ASIDE)) la = 1;
	}
	check(sa && la, "DESCRIBE has the two flagged savefile ACTIONs, 40 and 41");

	check(sf_info(&installed, &running, &pending, &size, &error) == 1 && installed == 1 && running == 1 &&
	      pending == 0 && size == TOD_SAVE_SIZE, "INFO: supported, installed, running, 0x906F0 bytes, nothing pending");
	check(savefile_install() == ST_OK, "an install is OK, with nothing to write");

	/* No slot yet: nothing to set aside, nothing to store. */
	check(g->trigger(TF_SET_ASIDE) == ST_BUSY, "Set aside with no save yet is BUSY");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NO_SAVE, "with error 7, no save");
	check(savefile_store("runs", "a.sav") == ST_BUSY, "and so is a STORE");
	check(g->trigger(TF_LOAD_ASIDE) == ST_BUSY, "Load set-aside with nothing set aside is BUSY");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NO_SAVE && rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE,
	      "with error 7, and the save system is left idle");

	/* The game saves into slot 2 with 1000 bolts: that is the save. */
	wr32(TOD_BOLTS, 1000);
	host_fake_tod_saved(2);
	check(savefile_store("runs", "a.sav") == ST_OK, "STORE of the game's save is accepted");
	check(rd32(SM(TOD_SM_STATE)) == TOD_SS_HELD, "the save system is held for the copy");
	check(sf_settle(0, 200), "the STORE finishes");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NONE && rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE, "cleanly, the save system given back");
	n = sf_file(QWARK_SAVEDIR "/" TOD_TITLE "/runs/a.sav", g_sf_buf, sizeof(g_sf_buf));
	check(n == (long)TOD_SAVE_SIZE && be32_get(g_sf_buf + 0x41C) == 1000 && be32_get(g_sf_buf + TOD_WEAPON_STRIDE) == 1,
	      "the file is the game's save image, 1000 bolts");

	/* An autosave the game asks for during a STORE waits for the hold and then runs. */
	wr32(TOD_BOLTS, 2000);
	check(g->trigger(TF_SET_ASIDE) == ST_OK, "Set aside is a STORE into the set-aside file");
	wr32(SM(TOD_SM_AUTOSAVE), 1);
	host_fake_tod_save_frame();
	check(rd32(SM(TOD_SM_AUTOSAVE)) == 1, "an autosave asked for under the hold waits");
	check(sf_settle(0, 200), "the set-aside finishes");
	n = sf_file(QWARK_SAVEDIR "/" TOD_TITLE "/" SAVEFILE_ASIDE_FILE, g_sf_buf, sizeof(g_sf_buf));
	check(n == (long)TOD_SAVE_SIZE && be32_get(g_sf_buf + 0x41C) == 1000,
	      "set-aside.sav, beside the categories, holds the last save, not the live 2000");
	host_fake_tod_save_frame();
	check(rd32(SM(TOD_SM_AUTOSAVE)) == 0 && rd32(TOD_SAVE_IMAGE + 0x41C) == 2000,
	      "and the autosave runs once the system is given back");
	check(savefile_categories(g_sf_buf, 4096, &len) == ST_OK && g_sf_buf[0] == 1, "the categories list runs alone");

	/* A load: RESTORE a.sav over a game with 5000 bolts. */
	wr32(TOD_BOLTS, 5000);
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	check(savefile_restore("runs", "a.sav") == ST_OK, "RESTORE is accepted");
	check(rd32(SM(TOD_SM_STATE)) == TOD_SS_HELD, "the save system is held for the copy");
	tick(1);
	wr32(SM(TOD_SM_AUTOSAVE), 1);   /* the game asks for an autosave meanwhile */
	for (i = 0; i < 40 && rd32(SM(TOD_SM_STATE)) == TOD_SS_HELD; i++) tick(1);
	check(rd32(SM(TOD_SM_STATE)) == TOD_SS_LOADING && rd32(LOAD_ST) == TOD_LT_READ_OK &&
	      rd32(LOAD_ID_LO) == TOD_THREAD_GONE && rd32(SM(TOD_SM_AUTOSAVE)) == 0,
	      "after the copy: a load thread that read its file and is gone, the autosave dropped");
	check(rd32(TOD_BOLTS) == 5000, "the game's live save is untouched until its update takes it");
	check(sf_settle(1, 200), "the game's update takes it and the load settles");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NONE, "with no error");
	check(rd32(TOD_BOLTS) == 1000 && rd32(TOD_LOAD_REQUEST) == 1 && rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE &&
	      rd32(LOAD_ID_LO) == TOD_THREAD_NONE,
	      "g_saveData is the file's, its level is loading, the save system idle again");

	/* Load set-aside: the set-aside file again, through the flagged action and the combo's seam. */
	wr32(TOD_BOLTS, 7);
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	check(g->load_setaside() == ST_OK, "Load set-aside (the combo's seam) is accepted");
	check(sf_settle(1, 300), "and settles");
	check(rd32(TOD_BOLTS) == 1000, "the set-aside save is loaded");

	/* Refusals. */
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_GAME_STATE, 3);
	check(savefile_restore("runs", "a.sav") == ST_BUSY, "no load outside gameplay");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NOT_IDLE && rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE, "error 6, nothing held");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LOAD_REQUEST, 4);
	check(savefile_restore("runs", "a.sav") == ST_BUSY, "nor with a planet load queued");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(SM(TOD_SM_STATE), 2);
	check(savefile_store("runs", "b.sav") == ST_BUSY && savefile_restore("runs", "a.sav") == ST_BUSY,
	      "nor a STORE or a RESTORE while the game saves");
	wr32(SM(TOD_SM_STATE), TOD_SS_IDLE);
	wr32(SM(TOD_SM_AUTOSAVE), 1);
	check(savefile_store("runs", "b.sav") == ST_BUSY, "nor with an autosave waiting");
	wr32(SM(TOD_SM_AUTOSAVE), 0);

	/* A game request that slips in under the hold stops the copy. */
	check(savefile_restore("runs", "a.sav") == ST_OK, "a RESTORE starts");
	wr32(SM(TOD_SM_STATE), 3);
	wr32(SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4), 0x01000123u);
	tick(1);
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NOT_IDLE && (pending & SAVEFILE_PENDING_TRANSFER) == 0 &&
	      rd32(SM(TOD_SM_STATE)) == 3, "the game's own load under the hold stops it, and is left alone");
	wr32(SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4), TOD_THREAD_NONE);
	wr32(SM(TOD_SM_STATE), TOD_SS_IDLE);

	/* A load the game never takes is put back. */
	check(savefile_restore("runs", "a.sav") == ST_OK, "a RESTORE with the game's update stopped");
	for (i = 0; i < 800; i++) {
		tick(1);
		sf_info(&installed, &running, &pending, &size, &error);
		if (pending == 0) break;
	}
	check(pending == 0 && error == SAVEFILE_ERR_NOT_IDLE, "gives up after its wait with error 6");
	check(rd32(LOAD_ID_LO) == TOD_THREAD_NONE && rd32(LOAD_ST) == 0, "and the load thread is put back");
	host_fake_tod_save_frame();
	check(rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE, "which the game's next update takes back to idle");

	/* A file of the right size that is not a ToD save. */
	memset(g_sf_buf, 0x5A, TOD_SAVE_SIZE);
	{
		plat_file_t f;
		check(plat_file_open(QWARK_SAVEDIR "/" TOD_TITLE "/runs/junk.sav", PLAT_OPEN_WRITE, &f) == 0 &&
		      plat_file_write(f, g_sf_buf, TOD_SAVE_SIZE) == 0, "a junk file is written");
		plat_file_close(f);
	}
	check(savefile_restore("runs", "junk.sav") == ST_OK, "its RESTORE is accepted");
	check(sf_settle(1, 100), "and stops");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NO_SAVE && rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE && rd32(TOD_BOLTS) == 1000,
	      "error 7: not a save of this game, nothing loaded, the system given back");

	/* What a qwark that went away leaves is given back at the next session. */
	wr32(SM(TOD_SM_STATE), TOD_SS_HELD);
	wr32(LOAD_ID_LO, TOD_THREAD_GONE);
	tod_save_recover();
	check(rd32(SM(TOD_SM_STATE)) == TOD_SS_IDLE && rd32(LOAD_ID_LO) == TOD_THREAD_NONE,
	      "a stale hold and a stale load are put back");

	check(savefile_patch(patch, sizeof(patch), &len) == ST_UNSUPPORTED, "SAVEFILE_PATCH: nothing to patch in");

	/* Under RPCS3 exactly the same, with no patch. */
	check(quit(), "the game quits");
	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(boot_tod(), "ToD boots under RPCS3, no patch of any kind");
	tick(4);
	check(sf_info(&installed, &running, &pending, &size, &error) == 1 && installed == 1 && running == 1,
	      "INFO: installed and running");
	check(savefile_library_gate() == ST_OK, "the library ops are open");
	wr32(TOD_BOLTS, 9);
	check(savefile_restore("runs", "a.sav") == ST_OK && sf_settle(1, 300) && rd32(TOD_BOLTS) == 1000,
	      "a RESTORE loads through data writes alone");
	check(quit(), "the game quits");
	host_set_can_patch_code(1);
	host_set_emulator(0);
}

static int sf_trigger(u8 id)
{
	const struct game_api *g = session_game();

	return g != NULL ? g->trigger(id) : ST_NOT_INGAME;
}

/* The refusal a RESTORE of runs/a.sav gets now: its error, with `why` and a log line to match. */
static int sf_refused(const char *log, u8 err, u8 why)
{
	u8 installed, running, pending, error = 0;
	u32 size;
	int rc;

	host_log_clear();
	rc = savefile_restore("runs", "a.sav");
	sf_info(&installed, &running, &pending, &size, &error);
	return rc == ST_BUSY && error == err && savefile_why_now() == why && host_log_seen(log) &&
	       rd32(SM(TOD_SM_STATE)) != TOD_SS_HELD;
}

/*
 * Build 57's console report: STORE worked, every load was refused with error 6
 * and logged nothing. The gate asked g_frontEndExitRequested, which the title
 * sets on its way out and nothing clears. Here: loads after every kind of
 * boot, and every refusal with its `why` and its qwark.log line.
 */
static void test_tod_savefile_boots(void)
{
	u8 installed = 0, running = 0, pending = 0, error = 0;
	u32 size = 0, i;

	group("ToD: savefile loads after any boot, and why a refusal was (revision 1.29, pending)");
	check(boot_tod(), "ToD boots");
	tick(4);

	/* The user's boot: through the title's continue, so the exit flag is 1 for good. */
	check(rd8(TOD_FRONTEND_EXIT) == 1 && rd8(TOD_FRONT_END) == 0,
	      "after the title, g_frontEndExitRequested still reads 1 and g_inFrontEnd 0");
	wr32(TOD_BOLTS, 77);
	host_log_clear();
	check(savefile_restore("runs", "a.sav") == ST_OK, "a RESTORE in gameplay after the title is accepted");
	check(sf_settle(1, 300) && rd32(TOD_BOLTS) == 1000, "and loads (build 57 refused it with error 6)");
	check(host_log_seen("load handed to the game: load thread state 5, id 00000000:FFFFFFFE, manager state 3"),
	      "the load is logged as it goes out");

	/* A boot with no slot at all, and an autosave of the game's left waiting for good. */
	host_fake_tod_saved(0xFFFFFFFFu);
	wr32(SM(TOD_SM_AUTOSAVE), 1);
	wr32(TOD_BOLTS, 78);
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	check(savefile_restore("runs", "a.sav") == ST_OK, "no slot, an autosave pending that can never run: a RESTORE is accepted");
	check(sf_settle(1, 300) && rd32(TOD_BOLTS) == 1000, "and loads");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_BOLTS, 79);
	check(sf_trigger(TF_LOAD_ASIDE) == ST_OK && sf_settle(1, 300) && rd32(TOD_BOLTS) == 1000,
	      "Load set-aside too");
	wr32(SM(TOD_SM_AUTOSAVE), 1);
	check(savefile_store("runs", "c.sav") == ST_OK && sf_settle(0, 200),
	      "and a STORE of the save qwark restored, with no slot");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NONE, "cleanly");
	wr32(SM(TOD_SM_AUTOSAVE), 0);

	/* Each refusal: error, why, and the line in qwark.log. */
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr8(TOD_FRONT_END, 1);
	check(sf_refused("tod: restore refused, the title screen is up (g_inFrontEnd 1", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_TITLE), "the title screen: error 6, why 3, logged");
	check(host_log_seen("savefile: restore refused by the game, error 6 (why 3)"), "and the core's line says so too");
	wr8(TOD_FRONT_END, 0);

	wr32(TOD_GAME_STATE, 3);
	check(sf_refused("tod: restore refused, not in gameplay (g_gameState 3, wants 4", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_NOT_GAMEPLAY), "another game state: why 1, logged with its value");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LEVEL_OBJ, 0);
	check(sf_refused("level object 0x00000000", SAVEFILE_ERR_NOT_IDLE, SAVEFILE_WHY_NOT_GAMEPLAY),
	      "no level up: why 1");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);

	wr32(TOD_LOAD_REQUEST, 4);
	check(sf_refused("tod: restore refused, a planet load is queued (g_planetLoadRequest 4)", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_PLANET_LOAD), "a planet load queued: why 2");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);

	wr32(SM(TOD_SM_STATE), 2);
	check(sf_refused("tod: restore refused, the save system is busy: state 0x00000002", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_SAVING), "the game saving: why 4, with the manager's words");
	wr32(SM(TOD_SM_STATE), TOD_SS_IDLE);
	host_fake_tod_saved(2);
	wr32(SM(TOD_SM_AUTOSAVE), 1);
	check(sf_refused("autosave 1, slot 2", SAVEFILE_ERR_NOT_IDLE, SAVEFILE_WHY_SAVING),
	      "an autosave about to run, with a slot: why 4");
	wr32(SM(TOD_SM_AUTOSAVE), 0);
	wr32(SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4), 0x01000123u);
	check(sf_refused("load 00000000:01000123", SAVEFILE_ERR_NOT_IDLE, SAVEFILE_WHY_SAVING),
	      "a load thread of the game's running: why 4");
	wr32(SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4), TOD_THREAD_NONE);

	wr32(TOD_SAVE_SYSTEM, 0);
	check(sf_refused("tod: restore refused, the save system is not up (g_saveSystem 0x00000000", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_NO_SYSTEM), "no save system: why 5");
	wr32(TOD_SAVE_SYSTEM, TOD_SAVE_MANAGER);

	/* A set-aside with no slot, and nothing of qwark's in the buffer: why 8. */
	check(quit() && boot_tod(), "a fresh boot");
	tick(4);
	host_log_clear();
	check(sf_trigger(TF_SET_ASIDE) == ST_BUSY && savefile_why_now() == SAVEFILE_WHY_NO_SLOT &&
	      host_log_seen("tod: set-aside refused, the game has not saved into a slot or loaded one since it started"),
	      "Set aside with no slot: error 7, why 8, logged");

	/* The file is no save of ToD: why 7, the failing word logged. */
	host_log_clear();
	check(savefile_restore("runs", "junk.sav") == ST_OK && sf_settle(1, 100), "a junk file's RESTORE runs and stops");
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NO_SAVE && savefile_why_now() == SAVEFILE_WHY_NOT_A_SAVE &&
	      host_log_seen("tod: not a ToD save: weapon slot 0's id is 0x5A5A5A5A"),
	      "error 7, why 7, and the word that is wrong");

	/* A missing file and one of the wrong size are the core's, logged too. */
	host_log_clear();
	check(savefile_restore("runs", "none.sav") == ST_NOT_FOUND && host_log_seen("savefile: restore refused, no such file"),
	      "a missing file is logged");
	{
		plat_file_t f;
		check(plat_file_open(QWARK_SAVEDIR "/" TOD_TITLE "/runs/short.sav", PLAT_OPEN_WRITE, &f) == 0 &&
		      plat_file_write(f, g_sf_buf, 100) == 0, "a short file is written");
		plat_file_close(f);
	}
	check(savefile_restore("runs", "short.sav") == ST_BAD_ARG &&
	      host_log_seen("short.sav is 100 bytes and a save of this game is 591600"), "and a short one");

	/* A load the game's update never takes: why 6, logged with the manager's words. */
	host_log_clear();
	check(savefile_restore("runs", "a.sav") == ST_OK, "a RESTORE with the game's update stopped");
	for (i = 0; i < 800; i++) {
		tick(1);
		sf_info(&installed, &running, &pending, &size, &error);
		if (pending == 0) break;
	}
	check(error == SAVEFILE_ERR_NOT_IDLE && savefile_why_now() == SAVEFILE_WHY_NOT_TAKEN &&
	      host_log_seen("tod: the load was not taken in 600 ticks: manager state 0x00000003, load thread state 5"),
	      "not taken: error 6, why 6, logged");
	host_fake_tod_save_frame();

	/* A load of the game's own under the copy: why 9. */
	host_log_clear();
	check(savefile_restore("runs", "a.sav") == ST_OK, "a RESTORE starts");
	wr32(SM(TOD_SM_STATE), 3);
	wr32(SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4), 0x01000123u);
	tick(1);
	sf_info(&installed, &running, &pending, &size, &error);
	check(error == SAVEFILE_ERR_NOT_IDLE && savefile_why_now() == SAVEFILE_WHY_HOLD_LOST &&
	      host_log_seen("savefile: the copy stopped at") && host_log_seen("transfer failed, error 6 (why 9)"),
	      "the hold lost: error 6, why 9, logged");
	wr32(SM(TOD_SM_LOAD_THREAD + TOD_ST_ID + 4), TOD_THREAD_NONE);
	wr32(SM(TOD_SM_STATE), TOD_SS_IDLE);

	/* The next request starts with no reason. */
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	check(savefile_restore("runs", "a.sav") == ST_OK && savefile_why_now() == SAVEFILE_WHY_NONE,
	      "a request that goes out clears `why`");
	check(sf_settle(1, 300), "and settles");
	check(quit(), "the game quits");
}

/* ------------------- a RESTORE is the set-aside too, revision 1.29 (pending) */

#define SF_ASIDE_PATH QWARK_SAVEDIR "/" TOD_TITLE "/" SAVEFILE_ASIDE_FILE
#define SF_ASIDE_NEW  SF_ASIDE_PATH ".new"

/* A file's CRC, or 0 when it is not exactly a save's length. */
static u32 sf_crc(const char *path)
{
	long n = sf_file(path, g_sf_buf, sizeof(g_sf_buf));

	if (n != (long)TOD_SAVE_SIZE) return 0;
	return qcrc32_finish(qcrc32_update(qcrc32_start(), g_sf_buf, TOD_SAVE_SIZE));
}

/* The bolts a save file holds, or 0xFFFFFFFF when it is not a save's length. */
static u32 sf_bolts(const char *path)
{
	return sf_file(path, g_sf_buf, sizeof(g_sf_buf)) == (long)TOD_SAVE_SIZE ? be32_get(g_sf_buf + 0x41C) : 0xFFFFFFFFu;
}

static int sf_exists(const char *path)
{
	return plat_path_exists(path, NULL, NULL);
}

/* A RESTORE of runs/<name> that the game takes, ending with error 0 and `bolts` loaded. */
static int sf_restore_ok(const char *name, u32 bolts)
{
	u8 installed, running, pending, error = 0xFF;
	u32 size;

	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	if (savefile_restore("runs", name) != ST_OK || !sf_settle(1, 300)) return 0;
	sf_info(&installed, &running, &pending, &size, &error);
	return error == SAVEFILE_ERR_NONE && rd32(TOD_BOLTS) == bolts;
}

static int sf_load_aside_ok(u32 bolts)
{
	u8 installed, running, pending, error = 0xFF;
	u32 size;

	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	if (sf_trigger(TF_LOAD_ASIDE) != ST_OK || !sf_settle(1, 300)) return 0;
	sf_info(&installed, &running, &pending, &size, &error);
	return error == SAVEFILE_ERR_NONE && rd32(TOD_BOLTS) == bolts;
}

static void sf_faults(u32 open_fails, u32 cut, u32 close_fails)
{
	struct host_file_faults f;

	f.read_chunk = 0;
	f.open_fails = open_fails;
	f.write_cut = cut;
	f.close_fails = close_fails;
	host_set_file_faults(SF_ASIDE_NEW, &f);
}

/*
 * The user's report: a file loaded from the Save files panel, then the Load
 * set-aside combo, and nothing happened until a Set aside by hand. In the four
 * original games a RESTORE fills the aside buffer it loads, so it is the
 * set-aside from then on; ToD's set-aside file now gets the same.
 */
static void test_tod_savefile_aside(void)
{
	char sum[16], want[16];
	long n;
	u32 crc_b;

	group("ToD: a RESTORE is the set-aside too, as in the other games (revision 1.29, pending)");
	check(boot_tod(), "ToD boots");
	tick(4);

	/* b.sav holds 3000 bolts, and the set-aside 4000, set aside by hand. */
	wr32(TOD_BOLTS, 3000);
	host_fake_tod_saved(2);
	check(savefile_store("runs", "b.sav") == ST_OK && sf_settle(0, 200), "b.sav is stored, 3000 bolts");
	crc_b = sf_crc(QWARK_SAVEDIR "/" TOD_TITLE "/runs/b.sav");
	wr32(TOD_BOLTS, 4000);
	host_fake_tod_saved(2);
	check(sf_trigger(TF_SET_ASIDE) == ST_OK && sf_settle(0, 200) && sf_bolts(SF_ASIDE_PATH) == 4000,
	      "Set aside by hand: the set-aside holds 4000");

	/* RESTORE, then Load set-aside: the same bytes. */
	host_log_clear();
	wr32(TOD_BOLTS, 5);
	check(sf_restore_ok("b.sav", 3000), "RESTORE of b.sav loads its 3000");
	check(host_log_seen("savefile: the restored file is the set-aside now (591600 bytes)"), "and makes it the set-aside, logged");
	check(crc_b != 0 && sf_crc(SF_ASIDE_PATH) == crc_b, "set-aside.sav is b.sav byte for byte");
	n = sf_file(SF_ASIDE_PATH SAVEFILE_SUM_EXT, (u8 *)sum, 8);
	sum[n == 8 ? 8 : 0] = 0;
	qfmt_hex(want, sizeof(want), crc_b, 8);
	check(n == 8 && strcmp(sum, want) == 0, "with its sum beside it");
	check(!sf_exists(SF_ASIDE_NEW), "and no .new left over");

	host_log_clear();
	wr32(TOD_BOLTS, 6);
	check(sf_load_aside_ok(3000), "Load set-aside then loads the restored file, not the 4000 set aside before");
	check(!host_log_seen("is the set-aside now") && !sf_exists(SF_ASIDE_NEW) && sf_crc(SF_ASIDE_PATH) == crc_b,
	      "and copies nothing onto itself: the set-aside is as it was");
	wr32(TOD_BOLTS, 7);
	check(sf_load_aside_ok(3000), "and again");

	/* A snapshot: a later STORE over b.sav leaves it be. */
	wr32(TOD_BOLTS, 6000);
	host_fake_tod_saved(2);
	check(savefile_store("runs", "b.sav") == ST_OK && sf_settle(0, 200), "b.sav is stored over, 6000 bolts");
	wr32(TOD_BOLTS, 8);
	check(sf_load_aside_ok(3000) && sf_crc(SF_ASIDE_PATH) == crc_b, "the set-aside is still the 3000 that was restored");

	/* A Set aside by hand after a RESTORE replaces it with the game's save. */
	check(sf_restore_ok("a.sav", 1000) && sf_bolts(SF_ASIDE_PATH) == 1000, "RESTORE of a.sav: the set-aside is 1000");
	wr32(TOD_BOLTS, 8000);
	host_fake_tod_saved(2);
	check(sf_trigger(TF_SET_ASIDE) == ST_OK && sf_settle(0, 200) && sf_bolts(SF_ASIDE_PATH) == 8000,
	      "a Set aside by hand after it replaces it with the game's 8000");
	wr32(TOD_BOLTS, 9);
	check(sf_load_aside_ok(8000), "which Load set-aside loads");

	/* A RESTORE that does not load leaves the set-aside alone. */
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	check(savefile_restore("runs", "junk.sav") == ST_OK && sf_settle(1, 100), "a junk file's RESTORE runs and stops");
	check(sf_bolts(SF_ASIDE_PATH) == 8000 && !sf_exists(SF_ASIDE_NEW), "and the set-aside is still 8000, no .new");

	/* The set-aside's copy failing, each way: the load is OK all the same. */
	sf_faults(1, 0, 0);
	host_log_clear();
	check(sf_restore_ok("b.sav", 6000), "the .new cannot be created: the RESTORE still loads, error 0");
	check(host_log_seen("set-aside.sav.new cannot be created; the load goes on and the set-aside stays as it was"),
	      "logged");
	check(sf_bolts(SF_ASIDE_PATH) == 8000 && !sf_exists(SF_ASIDE_NEW), "the set-aside is still 8000");

	sf_faults(0, 100000, 0);
	host_log_clear();
	check(sf_restore_ok("b.sav", 6000), "a write of the .new fails part way: the RESTORE still loads, error 0");
	check(host_log_seen("the set-aside's copy could not be written at") &&
	      host_log_seen("savefile: the restored file is not the set-aside; the load goes on"), "logged");
	check(sf_bolts(SF_ASIDE_PATH) == 8000 && !sf_exists(SF_ASIDE_NEW), "the set-aside is still 8000, the .new gone");

	sf_faults(0, 0, 1);
	host_log_clear();
	check(sf_restore_ok("b.sav", 6000), "the .new's close fails: the RESTORE still loads, error 0");
	check(host_log_seen("the set-aside's copy could not be closed"), "logged");
	check(sf_bolts(SF_ASIDE_PATH) == 8000 && !sf_exists(SF_ASIDE_NEW), "the set-aside is still 8000");
	host_set_file_faults(NULL, NULL);
	wr32(TOD_BOLTS, 10);
	check(sf_load_aside_ok(8000), "and Load set-aside loads it");

	/* With the faults gone, a RESTORE is the set-aside again. */
	check(sf_restore_ok("b.sav", 6000) && sf_bolts(SF_ASIDE_PATH) == 6000, "a RESTORE with the disk well again is the set-aside");
	/* a.sav back in, as the groups before this one leave the game for the autosplitter's. */
	check(sf_restore_ok("a.sav", 1000) && sf_bolts(SF_ASIDE_PATH) == 1000, "and a.sav back in, the set-aside with it");
	check(quit(), "the game quits");
}

void test_tod_game(void)
{
	test_tod_session();
	test_tod_features();
	test_tod_level_flags();
	test_tod_lua_console();
	test_tod_lua_rpcs3();
	test_tod_savefile();
	test_tod_savefile_boots();
	test_tod_savefile_aside();
	quit();
	check(boot_tod(), "ToD again, for the autosplitter");
	test_tod_autosplit_rows();
	test_tod_long_load_table();
	test_tod_autosplit_planets();
	test_tod_autosplit_long_load();
	test_tod_autosplit_runs();
	test_tod_autosplit_quit();
	quit();
}
