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
#include "../src/core/switches.h"
#include "../src/games/game.h"
#include "../src/games/titles.h"
#include "../src/games/tod.h"
#include "../src/games/luahelper_bins.h"
#include "../src/games/luahelper_patch.h"
#include "../src/games/switches_bins.h"
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
	check((info[29] & SESSION_FLAG2_NO_MENU) != 0, "flags2 NO_MENU: ToD has no console menu while menu.tod, its prototype's switch, is 0");
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
	{
		const struct game_describe *dd = g->describe();
		u8 k, live = 0, ammo_live = 1, health_live = 1;

		for (k = 0; k < dd->nfeatures; k++) {
			if ((dd->features[k].flags & FEATURE_FLAG_LIVE) == 0) {
				if (dd->features[k].id == TF_INFINITE_AMMO) ammo_live = 0;
				if (dd->features[k].id == TF_INFINITE_HEALTH) health_live = 0;
				continue;
			}
			live++;
			if (dd->features[k].id != TF_GOD_RATCHET) live = 99;
		}
		check(!ammo_live && !health_live, "Infinite ammo and Infinite health are not LIVE, so they can come back on boot");
		check(live == 1, "God Ratchet alone is LIVE, as RaC1's goodies are");
	}
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
	check(g->fast_loads == GAME_FEATURE(TF_FAST_LOADS), "the fast-loads combo flips Fast loads (test_tod_fastloads)");
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
	check(!((features_toggle_state() >> TF_INFINITE_AMMO) & 1u), "not LIVE: the word set behind qwark's back is not its bit");
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

/* ----------------------------------------- Fast loads, 2026-10-28 */

/*
 * The game's own words at 0x25E588..0x25E5CC (EBOOT.ELF md5 df9d68ad...,
 * LoadScreen_Update + 0x28), and the seventeen a console writes over them,
 * assembled and read back by hand (tod.h, TOD_FASTLOAD_AT). The eighteenth is
 * the first word after the patch, which stays the game's.
 */
static const u32 g_fl_orig[18] = {
	0x409E0074u, 0x83A27D78u, 0x807D0000u, 0x2C030001u, 0x41820064u, 0x81227D74u,
	0x7FE3FB78u, 0x88A90000u, 0x2C850000u, 0x41860068u, 0x809F003Cu, 0x4BFFFCDDu,
	0x38E00001u, 0x90FD0000u, 0x7D6C42E6u, 0x2F8B0000u, 0x41DEFFF8u, 0x817D0000u
};
static const u32 g_fl_words[17] = {
	0x81227D74u, 0x89290000u, 0x83A27D78u, 0x807D0000u, 0x2C830001u, 0x4186002Cu,
	0x2F090000u, 0x409A000Cu, 0x409E0020u, 0x48000068u, 0xC0227D84u, 0x48000785u,
	0x4BE2693Du, 0x4BE2686Du, 0x38000001u, 0x901D0000u, 0x48000034u
};

#define FL_FLAG 0x108A8560u

/*
 * Revision 1.29: the game's words at qwark's own pieces' three sites, read in
 * Ghidra (LevelTransition_update or r3, r28, r28 and addi r12, r28, 0x14;
 * LevelTransition_waitLoad lwz r9, 0x2BBC(r2)), and the cave's words at its
 * start, the dead function's, which a console has before the cave goes in.
 */
static const u32 g_fl_site_at[3]   = { 0x003C5744u, 0x003C57DCu, 0x003C58C8u };
static const u32 g_fl_site_orig[3] = { 0x7F83E378u, 0x399C0014u, 0x81222BBCu };

static void fl_seed_game(void)
{
	u32 i;

	for (i = 0; i < 18; i++) wr32(TOD_FASTLOAD_AT + 4u * i, g_fl_orig[i]);
	for (i = 0; i < 3; i++) wr32(g_fl_site_at[i], g_fl_site_orig[i]);
}

static int fl_sites_are(int patched)
{
	u32 i;

	for (i = 0; i < 3; i++)
		if (rd32(g_fl_site_at[i]) != (patched ? sw_tod_own_sites[i].value : g_fl_site_orig[i])) return 0;
	return 1;
}

static int fl_cave_in(void)
{
	u8 b[256];
	const struct sw_cave *c = &sw_tod_own_caves[0];

	if (c->len > sizeof(b)) return 0;
	host_peek(c->addr, b, c->len);
	return memcmp(b, c->bytes, c->len) == 0;
}

static int fl_words_are(const u32 *w, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		if (rd32(TOD_FASTLOAD_AT + 4u * i) != w[i]) return 0;
	return 1;
}

static u8 fl_rd8(u32 addr)
{
	u8 b = 0xEE;

	host_peek(addr, &b, 1);
	return b;
}

/* Where a b or bc at `at` lands, or 0 for any other word. */
static u32 fl_target(u32 at, u32 w)
{
	u32 op = w >> 26, d;

	if (op == 18) {
		d = w & 0x03FFFFFCu;
		if (d & 0x02000000u) d |= 0xFC000000u;
	} else if (op == 16) {
		d = w & 0x0000FFFCu;
		if (d & 0x00008000u) d |= 0xFFFF0000u;
	} else {
		return 0;
	}
	return (w & 2u) ? d : at + d;
}

/* A SWITCH_PATCH reply's rows and flag bytes, poked in as RPCS3 applies them at load. */
static void fl_poke_patch(const u8 *reply, u32 len)
{
	u32 n = be16_get(reply), off = SWITCH_PATCH_HEAD, nb, i;

	for (i = 0; i < n && off + SWITCH_PATCH_ROW <= len; i++, off += SWITCH_PATCH_ROW)
		host_poke(be32_get(reply + off), reply + off + 4, 4);
	if (off + SWITCH_PATCH_BYTES_HEAD > len) return;
	nb = be16_get(reply + off);
	off += SWITCH_PATCH_BYTES_HEAD;
	for (i = 0; i < nb && off + SWITCH_PATCH_BYTE_ROW <= len; i++, off += SWITCH_PATCH_BYTE_ROW)
		host_poke(be32_get(reply + off), reply + off + 4, 1);
}

static u8 g_fl_patch[QWARK_MAX_PAYLOAD];

static void test_tod_fastloads(void)
{
	const struct game_describe *dsc;
	const struct sw_desc *sw = sw_desc_for_game(GAME_TOD);
	const struct lh_desc *lh = lh_desc_for_title(TOD_TITLE);
	u32 len = 0, i;
	int found = 0, same = 1, apart = 1, on = -1;
	u8 b = 0;

	group("ToD: Fast loads, the load screen's cut, on a console");
	check(boot_tod(), "ToD boots on a console");
	if (session_game() == NULL) return;
	dsc = session_game()->describe();
	for (i = 0; i < dsc->nfeatures; i++)
		if (dsc->features[i].id == TF_FAST_LOADS)
			found = dsc->features[i].kind == FEATURE_TOGGLE &&
			        (dsc->features[i].flags & FEATURE_FLAG_WRITES_CODE) != 0 &&
			        (dsc->features[i].flags & FEATURE_FLAG_LIVE) == 0 &&
			        qstreq(dsc->features[i].label, "Fast loads");
	check(found, "id 17, Fast loads, a WRITES_CODE toggle and not LIVE, so it can come back at boot");
	check(TOD_FASTLOAD_AT == 0x0025E588u && TOD_FASTLOAD_NWORDS == 17u && TOD_LOAD_SCREEN_DONE == 0x102034FCu,
	      "the patch is seventeen words of LoadScreen_Update from 0x25E588");

	fl_seed_game();
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	b = 0;
	host_poke(FL_FLAG, &b, 1);
	check(!fl_cave_in(), "the dead function holds no cave of qwark's before");
	host_write_log_reset();
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_words_are(g_fl_words, 17),
	      "on: the seventeen words, as assembled");
	check(rd32(TOD_FASTLOAD_AT + 68u) == g_fl_orig[17], "and not a word past them");
	check(fl_cave_in() && fl_sites_are(1), "revision 1.29: qwark's own cave in, and its three sites branch to it");
	check(fl_rd8(FL_FLAG) == 1, "the flag byte its pieces read, at 1");
	check(rd32(TOD_TRANSITION_MIN) == 0, "and g_levelTransitionMinTime at 0: a story FMV load reads at once");
	check(host_write_log_count() > 1, "in a few writes, the cave before the branches");
	host_write_log_reset();
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_words_are(g_fl_words, 17) && fl_sites_are(1),
	      "on again is no change");
	check(tod_toggle_read(TF_FAST_LOADS, &on) == ST_NOT_FOUND, "a console reads no flag: qwark's bit says");
	check((features_toggle_state() >> TF_FAST_LOADS) & 1u, "and it is set");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_words_are(g_fl_orig, 18) && fl_sites_are(0),
	      "off: the game's own words back, at the three sites too");
	check(fl_rd8(FL_FLAG) == 0 && rd32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME,
	      "the flag at 0 and g_levelTransitionMinTime at the game's 5 s");
	check(fl_cave_in(), "the cave stays: nothing reaches it without the sites");
	check(!((features_toggle_state() >> TF_FAST_LOADS) & 1u), "and the bit clear");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK, "off again is not an error");

	/* The words' sense, read off them: what each branch reaches. */
	check(fl_target(0x25E59Cu, g_fl_words[5]) == 0x25E5C8u && fl_target(0x25E5A8u, g_fl_words[8]) == 0x25E5C8u &&
	      fl_target(0x25E5C8u, g_fl_words[16]) == 0x25E5FCu,
	      "the arrival up and a cinematic playing return through the epilogue");
	check(fl_target(0x25E5A4u, g_fl_words[7]) == 0x25E5B0u, "the load done goes to the cut");
	check(fl_target(0x25E5ACu, g_fl_words[9]) == 0x25E614u, "nothing playing picks the next long-load screen, as the game");
	check(fl_target(0x25E5B4u, g_fl_words[11]) == 0x25ED38u && (g_fl_words[11] & 1u) &&
	      fl_target(0x25E5B8u, g_fl_words[12]) == 0x084EF4u && (g_fl_words[12] & 1u) &&
	      fl_target(0x25E5BCu, g_fl_words[13]) == 0x084E28u && (g_fl_words[13] & 1u),
	      "the cut calls the fade, the cinematic stop and the player reset");
	check(g_fl_words[14] == 0x38000001u && g_fl_words[15] == 0x901D0000u && g_fl_words[2] == 0x83A27D78u,
	      "and stores the arrival's 1 through &g_loadScreenType");
	check(fl_target(0x25E588u, g_fl_orig[0]) == 0x25E5FCu,
	      "the first word replaced is the game's bne cr7 to the same epilogue");

	group("ToD: Fast loads' code switch is the console's words");
	check(sw != NULL && sw->ncaves == 2 && sw->nsites == 4 && sw->nflags == 1,
	      "two caves (the switch, qwark's own), four sites, one flag");
	if (sw != NULL && sw->ncaves == 2 && sw->caves[0].len == 4u * 24u) {
		u32 cave = sw->caves[0].addr;

		for (i = 0; i < 17; i++) {
			u32 cw = be32_get(sw->caves[0].bytes + 4u * (7u + i));
			u32 tc = fl_target(cave + 4u * (7u + i), cw);
			u32 ts = fl_target(TOD_FASTLOAD_AT + 4u * i, g_fl_words[i]);

			if (cw == g_fl_words[i]) continue;
			/* A branch out of the seventeen: the same place, and the same link bit. */
			if ((cw >> 26) == 18 && (g_fl_words[i] >> 26) == 18 && tc == ts &&
			    (cw & 3u) == (g_fl_words[i] & 3u) &&
			    (ts < TOD_FASTLOAD_AT || ts >= TOD_FASTLOAD_AT + 68u)) continue;
			same = 0;
		}
		check(same, "the trampoline's second half is the seventeen words, its branches out to the same places");
		check(be32_get(sw->caves[0].bytes + 16) == 0x419E0008u &&
		      fl_target(cave + 20u, be32_get(sw->caves[0].bytes + 20)) == 0x25E5FCu &&
		      fl_target(cave + 24u, be32_get(sw->caves[0].bytes + 24)) == 0x25E58Cu,
		      "and with the flag clear it is the game's bne cr7, taken or not");
	} else {
		check(0, "the cave is the 24 words this test knows");
	}
	check(sw != NULL && sw->sites[0].addr == TOD_FASTLOAD_AT && sw->flags[0] == FL_FLAG &&
	      SW_TOD_FLAG_FAST_LOADS == FL_FLAG, "the site is the console's first word, the flag past the BSS");
	if (sw != NULL && sw->ncaves == 2 && sw->nsites == 4) {
		const struct sw_cave *own = &sw->caves[1];
		int sites_ok = 1, back_ok = 1;

		for (i = 0; i < 3; i++) {
			u32 site = sw->sites[1 + i].addr, to = fl_target(site, sw->sites[1 + i].value), k, w;
			int found_orig = 0, found_back = 0;

			if (site != g_fl_site_at[i] || sw->sites[1 + i].value != sw_tod_own_sites[i].value ||
			    to < own->addr || to >= own->addr + own->len) sites_ok = 0;
			/* Its piece runs the site's own word on the way back, and comes back to site + 4. */
			for (k = to; k + 4u <= own->addr + own->len; k += 4u) {
				w = be32_get(own->bytes + (k - own->addr));
				if (w == g_fl_site_orig[i]) found_orig = 1;
				if ((w >> 26) == 18 && (w & 3u) == 0 && fl_target(k, w) == site + 4u) { found_back = 1; break; }
			}
			if (!found_orig || !found_back) back_ok = 0;
		}
		check(sites_ok && own->addr == sw->caves[0].addr + sw->caves[0].len && own->addr + own->len <= 0x003EB708u,
		      "qwark's own three sites branch into its cave, right after the switch in the dead function");
		check(back_ok, "and each piece runs the word its site replaced and comes back after it");
		check(own->len == sw_tod_own_caves[0].len && memcmp(own->bytes, sw_tod_own_caves[0].bytes, own->len) == 0 &&
		      SW_TOD_DATA_FMV_IN == TOD_FL_FMV_IN && TOD_FL_FMV_IN == FL_FLAG + 4u,
		      "a console writes the same bytes the patch does, and the FMV stamp is the word after the flag");
	} else {
		check(0, "qwark's own cave and sites are in the table");
	}
	if (sw != NULL && lh != NULL) {
		u8 c, k;

		for (c = 0; c < lh->ncaves; c++)
			for (k = 0; k < sw->ncaves; k++)
				if (sw->caves[k].addr < lh->caves[c].addr + lh->caves[c].len &&
				    lh->caves[c].addr < sw->caves[k].addr + sw->caves[k].len) apart = 0;
		for (c = 0; c < lh->nhooks; c++)
			if (lh->hooks[c].addr == sw->sites[0].addr ||
			    (lh->hooks[c].addr >= sw->caves[0].addr &&
			     lh->hooks[c].addr < sw->caves[0].addr + sw->caves[0].len)) apart = 0;
		if (FL_FLAG >= 0x008CA000u && FL_FLAG < 0x008D0000u) apart = 0;
	} else {
		apart = 0;
	}
	check(apart, "and none of it is the Lua helper's caves, hooks or mailbox");

	group("ToD: Fast loads under RPCS3, the switch or UNSUPPORTED");
	check(quit(), "the game quits");
	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(boot_tod(), "ToD boots under RPCS3 without the patch");
	tick(6);
	fl_seed_game();
	check(switches_state() == 0, "the look finds no switches");
	host_write_log_reset();
	check(features_set(TF_FAST_LOADS, 1) == ST_UNSUPPORTED && fl_words_are(g_fl_orig, 18) &&
	      host_write_log_count() == 0, "so Fast loads is UNSUPPORTED and writes nothing");
	check(switches_patch(g_fl_patch, sizeof(g_fl_patch), &len) == ST_OK &&
	      be16_get(g_fl_patch) == 24u + 63u + 4u, "SWITCH_PATCH: 24 and 63 cave words and the four sites");
	check(quit(), "the game quits");
	host_boot_as(TOD_TITLE, "tod");
	fl_poke_patch(g_fl_patch, len);
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	tick(6);
	check(session_game() != NULL && session_game()->game_id == GAME_TOD, "ToD boots with the patch applied at load");
	check(switches_state() == 1, "and the look finds it");
	check(rd32(TOD_FASTLOAD_AT) == SW_TOD_SITE_FAST_LOADS, "the site is the branch to the trampoline");
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	host_write_log_reset();
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_rd8(FL_FLAG) == 1 && rd32(TOD_TRANSITION_MIN) == 0 &&
	      host_write_log_count() == 2, "on: the flag byte to 1 and g_levelTransitionMinTime to 0, two writes");
	check(rd32(TOD_FASTLOAD_AT) == SW_TOD_SITE_FAST_LOADS && rd32(g_fl_site_at[0]) == SW_TOD_SITE_OWN_DEPARTURE,
	      "and no code");
	check(tod_toggle_read(TF_FAST_LOADS, &on) == ST_OK && on == 1, "the flag reads back as on");
	host_poke(FL_FLAG, &b, 1);
	check(tod_toggle_read(TF_FAST_LOADS, &on) == ST_OK && on == 0, "and a flag cleared behind qwark's back as off");
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && features_set(TF_FAST_LOADS, 0) == ST_OK &&
	      fl_rd8(FL_FLAG) == 0 && rd32(TOD_FASTLOAD_AT) == SW_TOD_SITE_FAST_LOADS &&
	      rd32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME, "off: the flag to 0 and the 5 s back, still no code");
	check(quit(), "the game quits");
	host_set_can_patch_code(1);
	host_set_emulator(0);
	check(boot_tod(), "and comes back on a console");

}

/* --------------------------------- qwark's own loads' fade, revision 1.29 */

static void fade_world(void)
{
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	wr32(TOD_LUA_STATE, 0x30030000u);
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_LOAD_FADE_TIME, TOD_LOAD_FADE_GAME);
	wr32(TOD_LOAD_FADE_DONE, 0x3F800000u);
}

/* LVL_FAST_SHIP_FIXED (106) in the game's level flags, set or clear. */
static void fade_flag_106(int set)
{
	u8 b = 0;
	const u32 at = TOD_LEVEL_FLAGS + TOD_LF_BYTE(TOD_LF_FAST_SHIP_FIXED);

	host_peek(at, &b, 1);
	b = set ? (u8)(b | (1u << (TOD_LF_FAST_SHIP_FIXED & 7u))) : (u8)(b & ~(1u << (TOD_LF_FAST_SHIP_FIXED & 7u)));
	host_poke(at, &b, 1);
}

static void test_tod_load_fade(void)
{
	int i;

	group("ToD: no fade-out before a load qwark asks for (revision 1.29)");
	check(TOD_LOAD_FADE_TIME == 0x008BA51Cu && TOD_LOAD_FADE_QUICK == 0x3A800000u &&
	      f32_from_bits(TOD_LOAD_FADE_QUICK) * 1024.0f == 1.0f, "k_loadLevelFadeTime, and 2^-10 s for it");
	fade_world();
	check(session_planet_load(2, 0) == ST_OK && rd32(TOD_LOAD_REQUEST) == 2 &&
	      rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK, "PLANET_LOAD: the short fade, then the request");
	check(rd32(TOD_LOAD_FADE_DONE) == 0x3F800000u, "k_loadLevelFadeDone left at 1.0");
	tick(3);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK, "kept while the request waits");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);   /* LoadLevel_ProcessPending took it */
	tick(1);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "the game's 0.5 s back once the load has left the request");
	tick(3);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "and left alone after");

	/* A tick that never saw the request: the level going is the load's start too. */
	fade_world();
	check(session_planet_load(3, 0) == ST_OK && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK, "another");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_LUA_STATE, 0);
	tick(1);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "Level_Exit seen first: the time back as well");

	/* A load the game never takes: back after the wait. */
	fade_world();
	check(session_planet_load(4, 0) == ST_OK, "and one the game never takes");
	tick(TOD_FADE_WAIT_TICKS - 2);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK, "kept for its wait");
	tick(4);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "then the game's time back");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);

	/* Refused loads write nothing. */
	fade_world();
	wr32(TOD_GAME_STATE, 3);
	check(session_planet_load(2, 0) == ST_BUSY && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME,
	      "a refused PLANET_LOAD leaves the fade alone");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);

	/* 2026-10-31: a story FMV load fades for a few frames: no load screen hides the cut. */
	check(TOD_LOAD_FADE_STORY == 0x3DA00000u && f32_from_bits(TOD_LOAD_FADE_STORY) * 64.0f == 5.0f &&
	      TOD_LF_BYTE(TOD_LF_FAST_SHIP_FIXED) == 10u && (TOD_LF_FAST_SHIP_FIXED & 7u) == 2u,
	      "5/64 s for a story load, and LVL_FAST_SHIP_FIXED is bit 42 of g_saveData+0x5530");
	fade_flag_106(0);
	check(tod_story_load(0, TOD_LEVEL_FLAGS) && tod_story_load(1, TOD_LEVEL_FLAGS) &&
	      !tod_story_load(2, TOD_LEVEL_FLAGS) && !tod_story_load(18, TOD_LEVEL_FLAGS),
	      "flag 106 clear: Metropolis and Cobalia are story loads, the rest are not");
	fade_world();
	check(session_planet_load(0, 0) == ST_OK && rd32(TOD_LOAD_REQUEST) == 0 &&
	      rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_STORY, "PLANET_LOAD to a fresh Metropolis: the few frames' fade");
	tick(2);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_STORY, "kept while the request waits");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(1);
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "and the game's 0.5 s back as for any load");
	fade_world();
	check(session_planet_load(1, 0) == ST_OK && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_STORY,
	      "Cobalia's story load the same");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(1);
	fade_world();
	check(session_planet_load(2, 0) == ST_OK && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK,
	      "any other planet keeps the one frame");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(1);
	fade_flag_106(1);
	check(!tod_story_load(0, TOD_LEVEL_FLAGS) && !tod_story_load(1, TOD_LEVEL_FLAGS),
	      "flag 106 set: no story load");
	fade_world();
	check(session_planet_load(0, 0) == ST_OK && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK,
	      "so Metropolis with the ship fixed has the load screen and the one frame");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(1);
	fade_flag_106(0);
	check(tod_story_load(0, 0xFFFFFF00u), "a flag that cannot be read counts as clear: the longer fade");

	/* A qwark that went away with it armed: the next session's arrival puts it back. */
	fade_world();
	check(session_planet_load(5, 0) == ST_OK && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_QUICK, "armed");
	check(quit(), "and the game quits with it so");
	host_boot_as(TOD_TITLE, "tod");
	wr32(TOD_LOAD_FADE_TIME, TOD_LOAD_FADE_QUICK);   /* the memory as that boot found it */
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	check(session_game() != NULL && rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME,
	      "a session's arrival writes the game's 0.5 s");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(2);
}

/* ------------------------------------------ Remove cutscenes, revision 1.29 */

#define CUT_TOC     0x30200000u
#define CUT_TABLE   0x30210000u
#define CUT_STRINGS 0x30230000u

/* A record's two files: each a path string, 0 for none. */
struct cut_rec {
	const char *name;
	const char *file;
	const char *alt;
};

static const struct cut_rec g_cut_recs[] = {
	{ "l00_s00_attr_fmv", "l00_s00_attr_fmv.bik", "fmv/l00_s00_attr_fmv_textless.bik" },
	{ "l01_s01_ntro_fmv", "l01_s01_ntro_fmv.bik", NULL },
	{ "l01_s02_walk_fmv", "movies/l01_s02_walk_fmv.bik", NULL },
	{ "l01_s03_tach_fmv", "l01_s03_tach_fmv.bik", NULL },
	{ "l02_s01_hypr_fmv", "l02_s01_hypr_fmv.bik", NULL },
	{ "l02_s02_zoni_fmv", "l02_s02_zoni_fmv.bik", NULL },
	{ "l4b_s05_end1_fmv", "l4b_s05_end1_fmv.bik", NULL },
	{ "l7b_s01_end2_fmv", "l7b_s01_end2_fmv.bik", NULL },
	{ "l17_s03a_rmid_fmv", "l17_s03a_rmid_fmv.bik", NULL },
	{ "l17_s03_rmid_fmv", "l17_s03_rmid_fmv.bik", NULL }
};

#define CUT_NRECS ((u32)(sizeof(g_cut_recs) / sizeof(g_cut_recs[0])))

static u32 g_cut_file_at[CUT_NRECS], g_cut_alt_at[CUT_NRECS];

static void cut_build(void)
{
	u8 rec[TOD_FMV_STRIDE];
	u32 at = CUT_STRINGS, i;

	wr32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, CUT_TOC);
	wr32(CUT_TOC + TOD_TOC_FMVS, CUT_TABLE);
	wr32(CUT_TABLE - 4u, CUT_NRECS);
	for (i = 0; i < CUT_NRECS; i++) {
		memset(rec, 0, sizeof(rec));
		host_poke(at, (const u8 *)g_cut_recs[i].name, (u32)strlen(g_cut_recs[i].name) + 1);
		be32_put(rec, at);
		at += 0x40;
		host_poke(at, (const u8 *)g_cut_recs[i].file, (u32)strlen(g_cut_recs[i].file) + 1);
		be32_put(rec + TOD_FMV_FILE, at);
		be32_put(rec + TOD_FMV_FILE + 4, 0x1000);
		g_cut_file_at[i] = at;
		at += 0x40;
		g_cut_alt_at[i] = 0;
		if (g_cut_recs[i].alt != NULL) {
			host_poke(at, (const u8 *)g_cut_recs[i].alt, (u32)strlen(g_cut_recs[i].alt) + 1);
			be32_put(rec + TOD_FMV_ALT, at);
			be32_put(rec + TOD_FMV_ALT + 4, 0x1000);
			g_cut_alt_at[i] = at;
			at += 0x40;
		}
		host_poke(CUT_TABLE + i * TOD_FMV_STRIDE, rec, sizeof(rec));
	}
}

static int cut_str_is(u32 at, const char *want)
{
	char b[64];

	host_peek(at, (u8 *)b, sizeof(b));
	return memcmp(b, want, strlen(want) + 1) == 0;
}

/* Every path as cut_build wrote it (0), or the eight's names' first letters 0 (1). */
static int cut_all(int removed)
{
	u32 i;

	for (i = 0; i < CUT_NRECS; i++) {
		const char *f = g_cut_recs[i].file;
		int listed = !(i == 5 || i == 6 || i == 9);   /* zoni, end1 and the other rmid stay */
		u32 base = (u32)(strrchr(f, '/') != NULL ? strrchr(f, '/') - f + 1 : 0);

		if (removed && listed) {
			char want[64];

			memcpy(want, f, strlen(f) + 1);
			want[base] = 0;
			if (!cut_str_is(g_cut_file_at[i], want)) return 0;
			if (!cut_str_is(g_cut_file_at[i] + base + 1, f + base + 1)) return 0;
		} else if (!cut_str_is(g_cut_file_at[i], f)) {
			return 0;
		}
		if (!cut_str_is(g_cut_file_at[i] - 0x40, g_cut_recs[i].name)) return 0;   /* names untouched */
	}
	/* The attract FMV's textless file, behind its folder. */
	if (removed) return cut_str_is(g_cut_alt_at[0], "fmv/") && cut_str_is(g_cut_alt_at[0] + 5, "00_s00_attr_fmv_textless.bik");
	return cut_str_is(g_cut_alt_at[0], g_cut_recs[0].alt);
}

/* The eight's first letters at 0 by hand, as a qwark that went away left them. */
static void cut_blank(void)
{
	static const u8 zero = 0;
	u32 i;

	for (i = 0; i < CUT_NRECS; i++) {
		const char *f = g_cut_recs[i].file;
		u32 base = (u32)(strrchr(f, '/') != NULL ? strrchr(f, '/') - f + 1 : 0);

		if (i == 5 || i == 6 || i == 9) continue;
		host_poke(g_cut_file_at[i] + base, &zero, 1);
	}
	host_poke(g_cut_alt_at[0] + 4, &zero, 1);
}

static void test_tod_cutscenes(void)
{
	const struct game_describe *dsc;
	int found = 0, n, i;

	group("ToD: Remove cutscenes, the eight FMVs' files missing (revision 1.29)");
	check(session_game() != NULL, "ToD is up");
	if (session_game() == NULL) return;
	dsc = session_game()->describe();
	for (i = 0; i < dsc->nfeatures; i++)
		if (dsc->features[i].id == TF_REMOVE_CUTSCENES)
			found = dsc->features[i].kind == FEATURE_TOGGLE &&
			        (dsc->features[i].flags & (FEATURE_FLAG_WRITES_CODE | FEATURE_FLAG_LIVE)) == 0 &&
			        qstreq(dsc->features[i].label, "Remove cutscenes");
	check(found && TF_REMOVE_CUTSCENES == 18, "id 18, Remove cutscenes, a toggle of data writes, not LIVE");

	wr32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, 0);
	check(tod_cutscenes_apply(1) == -1, "no table yet: nothing to do");
	check(features_set(TF_REMOVE_CUTSCENES, 1) == ST_OK, "on with no table is still on");
	cut_build();
	tick(TOD_CUT_EVERY + 2);
	check(cut_all(1), "the table that comes later is seen to by the tick");
	check(features_set(TF_REMOVE_CUTSCENES, 0) == ST_OK && cut_all(0), "off: every path back, letter for letter");

	cut_build();
	check(features_set(TF_REMOVE_CUTSCENES, 1) == ST_OK && cut_all(1),
	      "on: the eight's files, and the attract's textless one behind its folder, at once");
	check(tod_cutscenes_apply(1) == 0 && cut_all(1), "again: nothing more to write");
	check(rd32(CUT_TABLE + 5u * TOD_FMV_STRIDE) == g_cut_file_at[5] - 0x40u,
	      "the records themselves (their names) are not touched");
	n = tod_cutscenes_apply(0);
	check(n == 8 && cut_all(0), "the way back is read off the paths alone: eight strings, no state");
	check(features_set(TF_REMOVE_CUTSCENES, 1) == ST_OK && cut_all(1), "on once more");

	/* The archive read again (a new table): the tick removes them again while wanted. */
	cut_build();
	tick(TOD_CUT_EVERY + 2);
	check(cut_all(1), "a table read again is seen to");

	/*
	 * 2026-10-31: the toggle ships auto-flagged (the user's call), so with no
	 * config.txt key for it a new session puts the files back as it arrives
	 * and the auto flag takes them out again.
	 */
	check((features_auto_mask() & ((u64)1 << TF_REMOVE_CUTSCENES)) != 0 &&
	      (features_toggle_auto() & ((u64)1 << TF_REMOVE_CUTSCENES)) != 0,
	      "on boot by default, with no config.txt key for it");
	check(quit(), "the game quits with them removed");
	host_boot_as(TOD_TITLE, "tod");
	cut_build();
	cut_blank();   /* the memory as that boot found it */
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	tick(8);
	check(session_game() != NULL && cut_all(1), "the next session: the default auto flag removes them again");

	/* A key set off wins over the default: the arrival puts them back, and nothing takes them out. */
	check(features_set_auto(TF_REMOVE_CUTSCENES, 0) == ST_OK &&
	      (features_auto_mask() & ((u64)1 << TF_REMOVE_CUTSCENES)) == 0, "the auto flag off, a config.txt key of 0");
	check(quit(), "the game quits with them removed");
	host_boot_as(TOD_TITLE, "tod");
	cut_build();
	cut_blank();
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	tick(8);
	check(session_game() != NULL && cut_all(0), "the next session's arrival puts them back, and they stay");
	check(features_set(TF_REMOVE_CUTSCENES, 1) == ST_OK && cut_all(1) &&
	      features_set(TF_REMOVE_CUTSCENES, 0) == ST_OK && cut_all(0), "and the toggle works there");

	/* Nonsense tables are left alone. */
	wr32(CUT_TABLE - 4u, TOD_FMV_MAX + 1u);
	check(tod_cutscenes_apply(1) == -1 && cut_all(0), "a count past TOD_FMV_MAX is no table");
	wr32(CUT_TABLE - 4u, CUT_NRECS);
}

/* -------------------------------------------- on boot, 2026-10-28 */

/*
 * Infinite ammo and Infinite health flagged "on boot": FEATURE_SET_AUTO takes
 * them (a LIVE toggle it refuses), a new boot of the game writes their words
 * as the session enters it, and the ammo hold runs on what the boot wrote.
 * God Ratchet, LIVE, still has no auto flag.
 */
static void test_tod_on_boot(void)
{
	u32 slot = TOD_WEAPONS + 2 * TOD_WEAPON_STRIDE;
	int i;

	group("ToD: Infinite ammo and Infinite health on boot");
	check(boot_tod(), "ToD boots");
	check(features_set_auto(TF_INFINITE_AMMO, 1) == ST_OK, "Infinite ammo takes the auto flag");
	check(features_set_auto(TF_INFINITE_HEALTH, 1) == ST_OK, "and so does Infinite health");
	check(features_set_auto(TF_GOD_RATCHET, 1) == ST_UNSUPPORTED, "God Ratchet, LIVE, does not");
	check(quit(), "the game quits");
	host_boot_as(TOD_TITLE, "tod");
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	tick(4);
	check(rd32(TOD_INFINITE_AMMO) == 1 && rd32(TOD_NO_DAMAGE) == 1, "a new boot: both words written as the session enters it");
	check(((features_toggle_state() >> TF_INFINITE_AMMO) & 1u) && ((features_toggle_state() >> TF_INFINITE_HEALTH) & 1u),
	      "and both bits set");
	check(fl_rd8(TOD_PROXY_LOW) == 0, "God Ratchet is left alone");
	host_poke(slot + TOD_WS_OWNED, (const u8 *)"\1", 1);
	wrf(slot + TOD_WS_AMMO, 20.0f);
	tick(12);
	wrf(slot + TOD_WS_AMMO, 15.0f);
	tick(8);
	check(rdf(slot + TOD_WS_AMMO) == 20.0f, "the ammo hold runs on the boot's write");
	check(features_set_auto(TF_INFINITE_AMMO, 0) == ST_OK && features_set_auto(TF_INFINITE_HEALTH, 0) == ST_OK,
	      "the auto flags come off");
	features_set(TF_INFINITE_AMMO, 0);
	features_set(TF_INFINITE_HEALTH, 0);
	check(rd32(TOD_INFINITE_AMMO) == 0 && rd32(TOD_NO_DAMAGE) == 0, "and the cheats with them");
	session_previous_dismiss();
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

/*
 * Revision 1.29: a level load as the game makes one, and a death, which is
 * none. The tier loader's job and g_levelLoadTier's loaded byte are read
 * every second tick, so every step here ticks at least twice.
 */
#define AS_TIER       (TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED)
#define AS_JOB_TITLE  0x10073278u   /* g_gameStateStacks[2], the title's tiers */

/* A planet load begins: Level_Exit, the level's three tiers out, the loader on state 3. */
static void load_begins(void)
{
	wr32(TOD_LUA_STATE, 0);
	wr8(AS_TIER, 0);
	wr32(TOD_LOAD_JOB, TOD_LOAD_JOB_LEVEL);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
}

/* Its data in: LevelTransition_waitLoad done, g_levelLoadTier loaded. */
static void load_data_in(void)
{
	wr8(AS_TIER, 1);
}

/* Level_Enter: the lua_State, then black and the fade in started; the loader idle. */
static void level_enters(u32 lua)
{
	wr8(AS_TIER, 1);
	wr32(TOD_LUA_STATE, lua);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	wr32(TOD_LOAD_JOB, 0);
}

/* Half a second into Level_Enter's fade in: its progress down to 0.5, the run's START (2026-10-31). */
static void fade_halfway(void)
{
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.5f);
}

/* A whole planet load, `level` written first as LoadLevel does. */
static void planet_load(u32 level, u32 lua)
{
	wr32(TOD_CURRENT_LEVEL, level);
	tick(2);
	load_begins();
	tick(10);
	load_data_in();
	tick(2);
	level_enters(lua);
	tick(2);
}

/*
 * A death, as the game makes it: the hero's health 0, the death's fade to
 * black, then GameState_Request(3, 1): only the level tier goes and comes
 * back, Level_Exit and Level_Enter, a new lua_State and Level_Enter's fade
 * in. The loader works on state 3, but g_levelLoadTier stays loaded.
 */
static void death(u32 lua)
{
	wrf(FAKE_HERO + TOD_HERO_HEALTH, 0.0f);
	tick(4);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	tick(4);
	wr32(TOD_LUA_STATE, 0);
	wr32(TOD_LOAD_JOB, TOD_LOAD_JOB_LEVEL);
	tick(4);
	wrf(FAKE_HERO + TOD_HERO_HEALTH, 10.0f);
	level_enters(lua);
	tick(4);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	tick(2);
}

/* A death whose whole Level_Exit .. Level_Enter falls between two ticks: only a new lua_State. */
static void quick_death(u32 lua)
{
	wr32(TOD_LUA_STATE, lua);
	tick(4);
}

/* A quit to the title: the level goes, the loader on state 2, then the front end. */
static void to_title(void)
{
	wr32(TOD_LUA_STATE, 0);
	wr8(AS_TIER, 0);
	wr32(TOD_LOAD_JOB, AS_JOB_TITLE);
	tick(4);
	wr8(TOD_FRONT_END, 1);
	wr32(TOD_LOAD_JOB, 0);
	tick(4);
}

/* The title gives way to a level load (a new game, a save): the front end out, the loader on state 3. */
static void from_title(void)
{
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_LOAD_JOB, TOD_LOAD_JOB_LEVEL);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	tick(4);
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
	check(d[1].code == TOD_AS_END_FADE && d[1].kind == AUTOSPLIT_SPLIT && d[1].flags == AUTOSPLIT_FLAG_DEFAULT &&
	      qstreq(d[1].label, "Tachyon defeated"),
	      "code 7, the fade to black after Tachyon, is the final split, \"Tachyon defeated\", on (rev 1.29)");
	for (i = 0; i < n; i++) check(d[i].code != 2 && d[i].code != 8, "codes 2 and 8 are retired");
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
	wr32(TOD_LOAD_JOB, 0);
	wr8(AS_TIER, 1);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LOAD_SCREEN, 1);
	metropolis_beaten(1);
	tick(4);

	/* Cobalia to Rykan V: the destination is written, then the load begins. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 9);
	tick(2);
	check(as_all(seq) == 0, "the level word alone is no split (Lua_set_save_level writes it too)");
	wr32(TOD_LUA_STATE, 0);
	tick(1);
	check(as_all(seq) == 0, "Level_Exit alone, the load's tiers not yet out, is nothing yet");
	load_begins();
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
	load_data_in();
	tick(2);
	level_enters(AS_STATE + 0x100);
	tick(2);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 0, "and so nothing to end");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 1, "one split for one trip");

	/* The same planet again: a reload, no split. */
	seq = autosplit_latest_seq();
	planet_load(9, AS_STATE);
	check(as_all(seq) == 0, "reloading the planet is no split and no load event");

	/* A death or a checkpoint: a new lua_State and fade in, no load. */
	seq = autosplit_latest_seq();
	death(AS_STATE + 0x140);
	check(as_all(seq) == 0, "a death or a checkpoint emits nothing");
	seq = autosplit_latest_seq();
	quick_death(AS_STATE + 0x180);
	check(as_all(seq) == 0, "nor one whose whole respawn falls between two ticks");

	/* A death with the level word moved first (Lua_set_save_level): still no split. */
	seq = autosplit_latest_seq();
	wr32(TOD_CURRENT_LEVEL, 4);
	tick(2);
	death(AS_STATE);
	check(as_all(seq) == 0, "a death after the level word moved is still no planet change");
	wr32(TOD_CURRENT_LEVEL, 9);
	tick(2);

	/* Quit to the title, then a save of another planet: no split either way. */
	seq = autosplit_latest_seq();
	to_title();
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "going to the title is no split");
	seq = autosplit_latest_seq();
	tick(40);
	check(as_all(seq) == 0, "the title emits nothing");
	wr32(TOD_CURRENT_LEVEL, 5);
	tick(2);
	from_title();
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0,
	      "a save is no run start");
	tick(10);
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "loading a save is no split");

	/* From there, a planet change splits again. */
	seq = autosplit_latest_seq();
	planet_load(6, AS_STATE + 0x40);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 6, "the next trip splits");

	/* A load whose new level has the old lua_State's address (the pool is reset each level). */
	seq = autosplit_latest_seq();
	planet_load(10, AS_STATE + 0x40);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 10,
	      "the same lua_State pointer back after a load still split it");

	/* With the flag set, Metropolis is a planet like any other. */
	seq = autosplit_latest_seq();
	planet_load(0, AS_STATE);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 0 &&
	      as_count(seq, AUTOSPLIT_START, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0,
	      "Sargasso to Metropolis on a file past it: a planet split, arg 0, no run");
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

static void test_tod_autosplit_runs(void)
{
	static const u32 before[] = { 635, 636, 637 };
	u32 ids[4], seq, arg = 0, i;

	group("ToD: run starts on the Metropolis flag, deaths, and the credits");

	/* A new game: the title, a file with MOVIE_METROPOLIS_TACHYON clear, Metropolis. */
	wr32(TOD_CURRENT_LEVEL, 3);
	wr32(TOD_LOAD_SCREEN, 1);
	tick(2);
	to_title();
	metropolis_beaten(0);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	seq = autosplit_latest_seq();
	wr32(TOD_LOAD_SCREEN, 4);   /* no level to fly from: a long load screen at once */
	from_title();
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "a new game is a RESET as its load begins, and no START yet");
	check(as_first(seq, AUTOSPLIT_RESET, 0) >= 0 && as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 1,
	      "RESET, and the long load's hold");
	wr32(TOD_LOAD_SCREEN, 1);
	tick(2);
	check(as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 1, "the hold ends at the arrival");
	load_data_in();
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "the data in, no level up yet: no START");
	level_enters(AS_STATE);
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "Level_Enter's fade in from black starts: no START yet");
	fade_halfway();
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1 &&
	      as_first(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD) < as_first(seq, AUTOSPLIT_START, 0),
	      "half a second into it: START, after the hold");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "Metropolis is no planet split");
	tick(10);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "one START");

	/* A death on Metropolis, flag still clear: a new lua_State and a new fade in, and nothing at all. */
	seq = autosplit_latest_seq();
	wr32(TOD_GAME_STATE, 5);
	death(AS_STATE + 0x20);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(4);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0, "a death on Metropolis is no RESET");
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "and no START at its fade in");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "and no split");
	check(as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 0 &&
	      as_count(seq, AUTOSPLIT_LOAD_END, TOD_AS_LOAD, NULL) == 0, "and no load edges");
	check(as_all(seq) == 0, "nothing at all");
	seq = autosplit_latest_seq();
	quick_death(AS_STATE + 0x60);
	death(AS_STATE + 0x60);
	check(as_all(seq) == 0, "two more, one between two ticks, one with the same lua_State back: nothing");

	/* Metropolis loaded again on Metropolis, flag still clear (a new game or a save there): a new run. */
	seq = autosplit_latest_seq();
	load_begins();
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "Metropolis reloaded on a fresh file is a RESET as the load begins");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "and no planet split");
	tick(10);
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	fade_halfway();
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "and START at its fade in");

	/* A Metropolis save made after the first planet: flag set, no start, no split. */
	metropolis_beaten(1);
	to_title();
	seq = autosplit_latest_seq();
	from_title();
	tick(4);
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	fade_halfway();
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0,
	      "a Metropolis save with the flag set is no run start");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "and, out of the title, no split");

	/* Fastoon II: the missions as the level left them, learnt; one done in the fight is the mission row. */
	for (i = 0; i < 3; i++) ids[i] = before[i];
	put_missions(18, ids, 3);
	planet_load(18, AS_STATE + 0x200);
	tick(8);
	seq = autosplit_latest_seq();
	ids[3] = 638;
	put_missions(18, ids, 4);
	tick(8);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_MISSION, &arg) == 1 && arg == ((18u << 16) | 638u),
	      "a mission done on Fastoon II is the mission row, arg level << 16 | its id");
	seq = autosplit_latest_seq();
	death(AS_STATE + 0x240);
	tick(8);
	check(as_all(seq) == 0, "a death there learns the list again and splits nothing");

	/* The credits, on Fastoon II. */
	seq = autosplit_latest_seq();
	wr32(TOD_GAME_STATE, TOD_STATE_CREDITS);
	tick(3);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_CREDITS, &arg) == 1 && arg == 18, "the credits split, on Fastoon II");

	put_missions(18, ids, 0);

	/* Challenge mode out of the credits: the file built from a fresh one, flag clear, into Metropolis. */
	seq = autosplit_latest_seq();
	metropolis_beaten(0);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	load_begins();
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "a challenge mode start is a RESET");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "and no planet split with it");
	tick(10);
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	fade_halfway();
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "and START at the fade in");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	metropolis_beaten(1);
	tick(4);
}

/*
 * Revision 1.29: the START is Level_Enter's fade in, ScreenFade_StartIn's
 * target 0.0, on the run's own level up. Case by case.
 */
#define AS_MP_STATE  (TOD_MOVIE_PLAYER + TOD_MP_STATE)
#define AS_MP_MOVIE  (TOD_MOVIE_PLAYER + TOD_MP_MOVIE)

/* A new game out of the title: flag 185 clear, Metropolis, the load under way. */
static void new_game_load(void)
{
	metropolis_beaten(1);
	wr32(TOD_CURRENT_LEVEL, 3);
	tick(2);
	to_title();
	metropolis_beaten(0);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	from_title();
	tick(6);
}

static void test_tod_autosplit_start_fade(void)
{
	u32 seq;

	group("ToD: a run starts halfway into the fade in that shows Ratchet (rev 1.29)");
	wr32(TOD_LOAD_SCREEN, 1);

	/* The level up, still black with its fade not started (Level_Enter between its two calls). */
	seq = autosplit_latest_seq();
	new_game_load();
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "the load: RESET, no START");
	load_data_in();
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	wr32(TOD_LOAD_JOB, 0);
	tick(6);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "Metropolis up, the screen held black: no START");
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "ScreenFade_StartIn's target 0.0, still black: no START yet");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.51f);
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "0.51 of the way: not yet");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.5f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "half a second in, the progress at 0.5: START, the same tick");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.25f);
	tick(4);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "the rest of the fade in adds nothing");

	/* A tick that misses the crossing, and a fade the game starts again: still one START, at or past 0.5. */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.7f);
	tick(2);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);   /* Set(1.0), then StartIn again */
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "a fade in started again from black: no START");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.3f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "the tick that finds it past 0.5 (0.3): START");
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	wr32(FAKE_ROOT + TOD_FADE_PROGRESS, 0x80000000u);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "a progress of -0.0 is past the half too");

	/* A target of -0.0 is 0.0; a fade that cannot be read is no fade in. */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	wr32(TOD_LEVEL_OBJ, 0);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr32(TOD_LOAD_JOB, 0);
	wr32(FAKE_ROOT + TOD_FADE_TARGET, 0x80000000u);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.25f);
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "no level object: no START");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "the object back, its target -0.0: START");

	/* The data in with a fade in left over from the load screen, no level up: no START. */
	seq = autosplit_latest_seq();
	new_game_load();
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	load_data_in();
	tick(6);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "the data in and a fade going in, no lua_State: no START");
	level_enters(AS_STATE);
	fade_halfway();
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "Level_Enter, and its fade in halfway: START");

	/* A death before the fade in (a new lua_State, no load): that level up is not the run's any more. */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	wr32(TOD_LOAD_JOB, 0);
	tick(4);
	death(AS_STATE + 0x40);
	fade_halfway();
	tick(4);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1,
	      "a death before the fade in: no START at the death's fade in, no second RESET");

	/* A reload before the fade in: a second RESET, one START, at the second level's fade in. */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	wr32(TOD_LOAD_JOB, 0);
	tick(2);
	load_begins();
	tick(4);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 2 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "Metropolis reloaded before the fade in: RESET again, no START");
	load_data_in();
	tick(2);
	level_enters(AS_STATE + 0x40);
	fade_halfway();
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "the level up and its fade in: START, once");

	/* Back to the title before the fade in drops the START. */
	seq = autosplit_latest_seq();
	new_game_load();
	to_title();
	metropolis_beaten(1);
	from_title();
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	fade_halfway();
	tick(4);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "the title before the fade in, then a played file: no START");

	/* A load elsewhere before the fade in drops it too. */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	wr32(TOD_LOAD_JOB, 0);
	tick(2);
	planet_load(1, AS_STATE + 0x80);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "another planet's load before the fade in: no START");

	/* A Metropolis save with the flag set never starts, whatever the fade does. */
	seq = autosplit_latest_seq();
	to_title();
	metropolis_beaten(1);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	from_title();
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	fade_halfway();
	tick(6);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0,
	      "a Metropolis save with flag 185 set: no RESET, no START at its fade in");

	/* Fast loads: the load screen cut black (ScreenFade_Set(1.0)) before Level_Enter; the same START. */
	seq = autosplit_latest_seq();
	new_game_load();
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);
	wr32(TOD_LOAD_SCREEN, 1);
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	fade_halfway();
	tick(2);
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 1,
	      "with the load screen cut: RESET at the load, START at the fade in");
	metropolis_beaten(1);
	planet_load(3, AS_STATE);
}

/* A level movie in the fake tables: index `id` named `name`, its name kept at `at`. */
static void put_movie(u32 id, u32 rec, u32 at, const char *name)
{
	host_poke(at, (const u8 *)name, (u32)strlen(name) + 1u);
	wr32(rec - 4u, at);
	wr32(TOD_MOVIE_RECORDS + id * 4u, rec);
}

static void test_tod_autosplit_ending(void)
{
	u32 seq, arg = 1;

	group("ToD: the end of a run, the fade to black after Tachyon (rev 1.29)");
	put_movie(5, 0x30400100u, 0x30400800u, "l4b_s03_boss_rt");
	put_movie(6, 0x30400200u, 0x30400840u, TOD_END_CINEMATIC);
	wr32(AS_MP_STATE, 0);
	wr32(AS_MP_MOVIE, 1);
	wr32(TOD_MOVIE_QUEUE_N, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	planet_load(18, AS_STATE + 0x600);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	tick(4);

	/* Another cinematic's fade to black is not the end. */
	seq = autosplit_latest_seq();
	wr32(AS_MP_MOVIE, 5);
	wr32(AS_MP_STATE, 1);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	tick(2);
	wr32(AS_MP_STATE, 2);
	wr32(TOD_GAME_STATE, TOD_STATE_CINEMATIC);
	tick(4);
	check(as_all(seq) == 0, "the boss cinematic going black: nothing");
	wr32(AS_MP_STATE, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	tick(2);

	/* The ending cinematic queued: the fade runs, and the split is the tick it reads 1.0. */
	seq = autosplit_latest_seq();
	wr32(AS_MP_MOVIE, 6);
	wr32(AS_MP_STATE, 1);
	tick(2);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.5f);
	tick(2);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.99f);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_END_FADE, NULL) == 0, "the fade on its way to black: no split");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_END_FADE, &arg) == 1 && arg == 0,
	      "black, the player still in state 1: \"Tachyon defeated\", the same tick, arg 0");
	wr32(AS_MP_STATE, 2);
	wr32(TOD_GAME_STATE, TOD_STATE_CINEMATIC);
	tick(4);
	wr32(AS_MP_STATE, 3);
	tick(2);
	wr32(AS_MP_STATE, 4);
	wr32(TOD_GAME_STATE, TOD_STATE_FMV);
	tick(4);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_END_FADE, NULL) == 1, "the cinematic and the FMVs split nothing more");
	check(as_all(seq) == 1, "and no retired code 2 or 8 comes out");

	/* A missed state 1: the player found already playing the ending still splits. */
	seq = autosplit_latest_seq();
	wr32(AS_MP_STATE, 0);
	planet_load(18, AS_STATE + 0x700);
	tick(4);
	wr32(AS_MP_MOVIE, 6);
	wr32(AS_MP_STATE, 2);
	tick(2);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_END_FADE, NULL) == 1, "state 2 of the ending, state 1 unseen: the split");

	/* A first look that finds the ending under way (a module come in late) learns it. */
	seq = autosplit_latest_seq();
	planet_load(18, AS_STATE + 0x800);
	tick(6);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_END_FADE, NULL) == 0,
	      "the ending already playing at the first look: no split");

	/* Not off Fastoon II. */
	seq = autosplit_latest_seq();
	wr32(AS_MP_STATE, 0);
	planet_load(9, AS_STATE);
	tick(4);
	wr32(AS_MP_STATE, 1);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_END_FADE, NULL) == 0, "the same movie index on another planet: nothing");

	wr32(AS_MP_STATE, 0);
	wr32(AS_MP_MOVIE, 0);
	wr32(TOD_MOVIE_RECORDS + 5u * 4u, 0);
	wr32(TOD_MOVIE_RECORDS + 6u * 4u, 0);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(2);
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
	fade_flag_106(0);   /* 2026-10-31: a save whose Cobalia is a story FMV load */
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
	/* 2026-10-31: the save's own level and flag 106 say how long its fade-out is. */
	check(rd32(TOD_SAVE_IMAGE + TOD_SAVE_LEVEL_OFF) == TOD_LEVEL_COBALIA &&
	      rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_STORY,
	      "the save's Cobalia with its flag 106 clear is a story load: the few frames' fade");
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
	test_tod_autosplit_start_fade();
	test_tod_autosplit_ending();
	test_tod_autosplit_quit();
	test_tod_fastloads();   /* last: it boots and ticks, and the autosplitter's run start reads tick phases */
	test_tod_load_fade();
	test_tod_cutscenes();
	test_tod_on_boot();
	quit();
}
