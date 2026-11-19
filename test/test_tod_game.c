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
#include "../src/core/runwarn.h"
#include "../src/core/mods.h"
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
/* Build 59: SKIN_JAILBIRD's owned word, ToDSaveData.skinOwned[7] (g_saveData+0x45C+7*4). */
#define TOD_SKIN_OWNED_TEST 0x1020C2E8u

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
	check((info[29] & SESSION_FLAG2_NO_MENU) == 0, "flags2 NO_MENU clear: ToD's console menu is its own pause menu (revision 1.29)");
	check(be32_get(info + 64 + 4 * TOD_RO_BOLTS) == 12345 && be32_get(info + 64 + 4 * TOD_RO_CHALLENGE) == 2,
	      "bolts and challenge mode in their readouts");
	host_set_pad(0);

	wr32(TOD_LEVEL_OBJ, 0);
	tick(4);   /* SessionInfo goes out every fourth tick: two passed only on some tick counts */
	session_info_copy(info, sizeof(info));
	check(bef32_get(info + 32) == 0.0f, "between levels there is no position");
	check(session_position_save(0) == ST_BUSY, "and no position to save");
	check(session_die() == ST_BUSY, "nor anyone to kill");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	tick(2);
}

/* Build 59: Infinite ammo's patch on the table, or NULL. */
static const struct patch_def *tod_ammo_patch(void)
{
	u32 i;

	for (i = 0; i < patch_count(); i++) {
		const struct patch_def *p = patch_at(i);
		if (p != NULL && p->kind == PATCH_KIND_FEATURE && strcmp(p->name, "Infinite ammo") == 0) return p;
	}
	return NULL;
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
	/* Die: test_tod_die, the game's own respawn since 2026-11-13. */

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

	group("ToD: the armour and skin pickers, Hero_setArmor's writes and Skin_select's (build 59)");
	{
		static const f32 mult[TOD_ARMOUR_COUNT] = { 1.0f, 0.75f, 0.6f, 0.45f, 0.35f };
		u8 k;
		u8 info[SESSION_INFO_SIZE];

		/* g_armorConfigs as armor.csv fills it; the hero wears Blackstar, owned. */
		for (k = 0; k < TOD_ARMOUR_COUNT; k++) wrf(TOD_ARMOUR_CONFIGS + k * TOD_ARMOUR_CONFIG_LEN, mult[k]);
		for (k = 0; k < TOD_ARMOUR_COUNT; k++) wr32(TOD_ARMOUR_OWNED + k * 4u, k == 1);
		wr32(TOD_ARMOUR, 1);
		wrf(FAKE_HERO + TOD_HERO_DAMAGE_SCALE, 0.75f);
		check(features_set(TF_ARMOUR, 3) == ST_OK, "Terraflux is picked");
		check(rd32(TOD_ARMOUR) == 3 && rd32(TOD_ARMOUR_OWNED + 12) == 1 && rd32(TOD_ARMOUR_OWNED + 4) == 1 &&
		      rd32(TOD_ARMOUR_OWNED + 8) == 0, "the save wears it and owns it, Blackstar still owned, Helios not");
		check(rdf(FAKE_HERO + TOD_HERO_DAMAGE_SCALE) == 0.45f, "and the hero takes its damage scale, 0.45, from the game's table");
		check(features_set(TF_ARMOUR, 0) == ST_OK && rd32(TOD_ARMOUR) == 0 &&
		      rdf(FAKE_HERO + TOD_HERO_DAMAGE_SCALE) == 1.0f, "no armour: the whole damage again");
		check(features_set(TF_ARMOUR, 5) == ST_BAD_ARG && rd32(TOD_ARMOUR) == 0 &&
		      rd32(TOD_ARMOUR_OWNED + 20) == 0, "an armour past Trillium is BAD_ARG, and writes nothing past the owned words");

		wr32(TOD_LEVEL_OBJ, 0);
		check(features_set(TF_ARMOUR, 4) == ST_OK && rd32(TOD_ARMOUR) == 4,
		      "between levels the save takes it (the next hero's Hero_Init gives itself the scale)");
		check(rdf(FAKE_HERO + TOD_HERO_DAMAGE_SCALE) == 1.0f, "and no hero is written");
		wr32(TOD_LEVEL_OBJ, FAKE_ROOT);

		wr32(TOD_SKIN_OWNED_TEST, 0);
		check(features_set(TF_SKIN, 7) == ST_OK && rd32(TOD_SKIN) == 7, "Convict Ratchet is picked: the skin word is SKIN_JAILBIRD");
		check(rd32(TOD_SKIN_OWNED_TEST) == 0 && rd32(TOD_ARMOUR) == 4, "and neither its owned word nor the armour is touched");
		check(features_set(TF_SKIN, 9) == ST_BAD_ARG && rd32(TOD_SKIN) == 7, "a skin past Mustachio Furioso is BAD_ARG");
		tick(9);
		session_info_copy(info, sizeof(info));
		check(be32_get(info + 64 + 4 * TOD_RO_ARMOUR) == 4 && be32_get(info + 64 + 4 * TOD_RO_SKIN) == 7,
		      "the Armour and Skin readouts read both back out of the save");
		wr32(TOD_ARMOUR, 0);
		wr32(TOD_SKIN, 0);
	}

	group("ToD: Infinite ammo fills each weapon and device and nops Weapon_useAmmo's call (build 59)");
	{
		const u32 w1 = TOD_WEAPONS + 1 * TOD_WEAPON_STRIDE, w2 = TOD_WEAPONS + 2 * TOD_WEAPON_STRIDE;
		const u32 d21 = TOD_WEAPONS + 21 * TOD_WEAPON_STRIDE, d23 = TOD_WEAPONS + 23 * TOD_WEAPON_STRIDE;
		const struct patch_def *p;
		const char *who;

		/*
		 * The fake game's weapon table (backend_fake.c): weapon 1 at V3 holds
		 * 20 + 2 * 2 + 1 = 25, the Visi-Copter (23) its csv's 3. Weapon 1 and
		 * the Visi-Copter held, weapon 2 and the Mega Leech (21) not.
		 */
		wr32(TOD_AMMO_INSTR, TOD_AMMO_INSTR_WORD);
		host_poke(w1 + TOD_WS_OWNED, (const u8 *)"\1\2", 2);   /* owned, level index 2 */
		wr32(w1 + TOD_WS_MODS, 0);
		wrf(w1 + TOD_WS_AMMO, 5.0f);
		host_poke(w2 + TOD_WS_OWNED, (const u8 *)"\0", 1);
		wrf(w2 + TOD_WS_AMMO, 4.0f);
		host_poke(d23 + TOD_WS_OWNED, (const u8 *)"\1\0", 2);
		wrf(d23 + TOD_WS_AMMO, 0.0f);
		host_poke(d21 + TOD_WS_OWNED, (const u8 *)"\0", 1);
		wrf(d21 + TOD_WS_AMMO, 0.0f);
		wr32(TOD_INFINITE_AMMO, 0);

		check(features_set(TF_INFINITE_AMMO, 1) == ST_OK, "Infinite ammo turns on");
		p = tod_ammo_patch();
		who = mem_owner(TOD_AMMO_INSTR, 4);
		check(rd32(TOD_AMMO_INSTR) == 0x60000000u && p != NULL && p->count == 1 &&
		      p->originals[0] == 0x4BFFB7D9u && who != NULL && strcmp(who, "Infinite ammo") == 0,
		      "0x49EF08 nopped, bl WeaponSlot_addAmmo kept as the original, Infinite ammo's to the guard");
		check(rdf(w1 + TOD_WS_AMMO) == 25.0f, "weapon 1 is filled to what it holds at V3, not to some huge number");
		check(rdf(d23 + TOD_WS_AMMO) == 3.0f, "the Visi-Copter at 0 is filled, to 3");
		check(rdf(w2 + TOD_WS_AMMO) == 4.0f && rdf(d21 + TOD_WS_AMMO) == 0.0f,
		      "a weapon and a device not held are left alone");
		check(rd32(TOD_INFINITE_AMMO) == 0, "and g_infiniteAmmo, the word until build 58, is not written");

		/* The game's own counts from here on: no tick writes them any more. */
		wrf(w1 + TOD_WS_AMMO, 24.0f);
		wrf(d23 + TOD_WS_AMMO, 2.0f);
		tick(24);
		check(rdf(w1 + TOD_WS_AMMO) == 24.0f && rdf(d23 + TOD_WS_AMMO) == 2.0f,
		      "a count the game itself changes is left as it is: the hold of builds 57 and 58 is gone");

		check(features_set(TF_INFINITE_AMMO, 1) == ST_OK && tod_ammo_patch() == p && p->originals[0] == 0x4BFFB7D9u,
		      "a second on takes no original again from the nop");
		check(rdf(w1 + TOD_WS_AMMO) == 25.0f, "and fills again");
		wr32(TOD_LEVEL_OBJ, 0);
		tick(4);
		wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
		tick(4);
		check(rd32(TOD_AMMO_INSTR) == 0x60000000u && ((features_toggle_state() >> TF_INFINITE_AMMO) & 1u),
		      "a new level object, as a planet load or a death makes, leaves the patch and the toggle in");

		check(features_set(TF_INFINITE_AMMO, 0) == ST_OK, "off");
		check(rd32(TOD_AMMO_INSTR) == 0x4BFFB7D9u && tod_ammo_patch() == NULL && mem_owner(TOD_AMMO_INSTR, 4) == NULL,
		      "the call is the game's again, word for word, and nobody's to the guard");
		check(rdf(w1 + TOD_WS_AMMO) == 25.0f, "and the ammo stays where it was");
		check(tod_device_max_ammo(16) == 0x40800000u && tod_device_max_ammo(21) == 0x3F800000u &&
		      tod_device_max_ammo(15) == 0 && tod_device_max_ammo(24) == 0,
		      "the simulator's device counts, 16..23 alone");
		wr32(TOD_INFINITE_AMMO, 1);
		tick(LIVE_TICKS_FOR_TEST);
		check(!((features_toggle_state() >> TF_INFINITE_AMMO) & 1u), "not LIVE: the old word set behind qwark's back is not its bit");
		wr32(TOD_INFINITE_AMMO, 0);
		tick(LIVE_TICKS_FOR_TEST);
		host_poke(w1 + TOD_WS_OWNED, (const u8 *)"\0\0", 2);
		host_poke(d23 + TOD_WS_OWNED, (const u8 *)"\0", 1);
	}

	group("ToD: positions");
	wrf(FAKE_HERO + TOD_HERO_POS, 1.0f);
	wrf(FAKE_HERO + TOD_HERO_POS + 4, 2.0f);
	wrf(FAKE_HERO + TOD_HERO_POS + 8, 3.0f);
	wr32(FAKE_HERO + 0x1290, 0xDEADBEEFu);
	check(g->save_blob(blob, &n) == ST_OK && n == TOD_POS_SLOT_LEN && g->blob_len == TOD_POS_SLOT_LEN,
	      "a slot is 48 bytes of the hero's motion and 16 of the follow camera's (test_tod_positions_camera)");
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

static void test_tod_level_flags(void)
{
	static const u8 per_planet[TOD_NPLANETS] = {   /* the client's data/levelflags/tod.json, group by group */
		4, 35, 13, 15, 2, 32, 19, 19, 1, 17, 8, 20, 9, 3, 4, 11, 7, 9, 18
	};
	const struct game_api *g = session_game();
	u8 region[64], ones[TOD_LEVEL_FLAGS_LEN], zero[TOD_LEVEL_FLAGS_LEN];
	u16 len = 0, i, p, left, cleared;
	int ok;

	group("ToD: the level flags, one u64 bit array for the whole save");
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

	/* Each planet's reset clears its own group alone, as many as the client's file lists under it. */
	memset(ones, 0, sizeof(ones));
	for (i = 0; i < TOD_NLEVEL_FLAGS; i++) ones[TOD_LF_BYTE(i)] |= (u8)(1u << (i & 7u));
	for (p = 0, ok = 1, left = TOD_NLEVEL_FLAGS; p < TOD_NPLANETS; p++) {
		host_poke(TOD_LEVEL_FLAGS, ones, TOD_LEVEL_FLAGS_LEN);
		ok &= g->levelflags_reset((u8)p) == ST_OK;
		for (i = 0, cleared = 0; i < TOD_NLEVEL_FLAGS; i++) cleared += !game_flag(i);
		ok &= cleared == per_planet[p];
		left -= cleared;
	}
	check(ok, "every planet's reset clears exactly its own flags, group by group");
	check(left == 46, "and the 46 under Other (hints, RYNO messages, items) are no planet's");
	host_poke(TOD_LEVEL_FLAGS, ones, TOD_LEVEL_FLAGS_LEN);
	for (p = 0; p < TOD_NPLANETS; p++) g->levelflags_reset((u8)p);
	ok = 1;
	for (i = 0; i <= 37; i++) ok &= game_flag(i);
	check(ok && game_flag(271) && game_flag(287) && !game_flag(38) && !game_flag(291),
	      "every planet reset: 0..37, 271 and 287 stay (Other), 38 (Metropolis) and 291 (Fastoon) go");
	host_poke(TOD_LEVEL_FLAGS, ones, TOD_LEVEL_FLAGS_LEN);
	check(g->levelflags_reset(3) == ST_OK && !game_flag(105) && !game_flag(112) && !game_flag(221) && !game_flag(226) &&
	      !game_flag(291) && game_flag(104) && game_flag(113) && game_flag(220), "Fastoon's: 105..112, 221..226 and 291");
	check(g->levelflags_reset(13) == ST_OK && !game_flag(268) && !game_flag(270) && !game_flag(278) && game_flag(269),
	      "Space Combat III's: 268, 270 and 278, not Space Combat I's 269");

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
		be16_put(req + 2, 2);
		check(lua_core_exec_start(req, 8, &seq) == ST_BAD_ARG, "and flags other than 0 or 1 (revision 1.29)");
		be16_put(req + 2, LUA_EXEC_MENU);
		check(lua_core_exec_start(req, 8, &seq) == ST_NOT_FOUND, "flags 1, the pause menu's VM, with no pause: NOT_FOUND");
	}
}

static void test_tod_lua_rpcs3(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	static u8 reply[QWARK_MAX_PAYLOAD];
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
	      lh_patch_count(d) > 300 &&
	      LH_PATCH_HEAD + lh_patch_count(d) * LH_PATCH_ROW + LH_PATCH_BYTES_HEAD <= QWARK_MAX_PAYLOAD,
	      "LUA_PATCH: the helper's words, all in one reply");
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
 * The game's own words at 0x25E588..0x25E5EC (EBOOT.ELF md5 df9d68ad...,
 * LoadScreen_Update + 0x28), and the twenty-five a console writes over them,
 * assembled and read back by hand (tod.h, TOD_FASTLOAD_AT). The twenty-sixth
 * is the first word after the patch, which stays the game's. Revision 1.29:
 * words 14 to 21 make the transition prop's handles invalid (TOD_PROP_*).
 */
#define FL_N 25u
static const u32 g_fl_orig[FL_N + 1] = {
	0x409E0074u, 0x83A27D78u, 0x807D0000u, 0x2C030001u, 0x41820064u, 0x81227D74u,
	0x7FE3FB78u, 0x88A90000u, 0x2C850000u, 0x41860068u, 0x809F003Cu, 0x4BFFFCDDu,
	0x38E00001u, 0x90FD0000u, 0x7D6C42E6u, 0x2F8B0000u, 0x41DEFFF8u, 0x83A27D78u,
	0x3880FFFFu, 0x80C27D8Cu, 0x38A0FFFFu, 0xF9660000u, 0x38C00000u, 0x80FD0000u,
	0x2F070001u, 0x7CE33B78u
};
static const u32 g_fl_words[FL_N] = {
	0x81227D74u, 0x89290000u, 0x83A27D78u, 0x807D0000u, 0x2C830001u, 0x4186004Cu,
	0x2F090000u, 0x409A000Cu, 0x409E0040u, 0x48000068u, 0xC0227D84u, 0x48000785u,
	0x4BE2693Du, 0x4BE2686Du, 0x3D20101Du, 0x3D400400u, 0x394AFFFFu, 0x3960FFFFu,
	0x914960E0u, 0x916960E4u, 0x914960E8u, 0x916960ECu, 0x38000001u, 0x901D0000u,
	0x48000014u
};
#define FL_PROP_FROM 14u      /* the eight words of the transition prop's handles */
#define FL_PROP_N    8u

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

	for (i = 0; i <= FL_N; i++) wr32(TOD_FASTLOAD_AT + 4u * i, g_fl_orig[i]);
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

/*
 * Revision 1.29: runs the cut's handle words (lis, addi, stw, as the console's
 * and the switch's are) against the host's memory, as the PPU would: 1 if every
 * word was one of those, 0 for any other word. Registers start at a junk value,
 * so a word that reads one it never set shows.
 */
static int fl_run(const u32 *w, u32 n)
{
	u32 r[32], i;

	for (i = 0; i < 32; i++) r[i] = 0xDEAD0000u + i;
	for (i = 0; i < n; i++) {
		u32 op = w[i] >> 26, rd = (w[i] >> 21) & 31u, ra = (w[i] >> 16) & 31u;
		u32 si = (u32)(s32)(s16)(w[i] & 0xFFFFu);

		if (op == 15)      r[rd] = (ra ? r[ra] : 0u) + (si << 16);   /* addis, lis */
		else if (op == 14) r[rd] = (ra ? r[ra] : 0u) + si;           /* addi, li */
		else if (op == 36) wr32((ra ? r[ra] : 0u) + si, r[rd]);      /* stw */
		else return 0;
	}
	return 1;
}

/* The two handles as a cinematic early in a level leaves them: the names of slots 2 and 3. */
static void fl_seed_prop(void)
{
	wr32(TOD_PROP_OWNER, 0x00000000u);
	wr32(TOD_PROP_OWNER + 4u, 0x00030002u);
	wr32(TOD_PROP_HANDLE, 0x00000000u);
	wr32(TOD_PROP_HANDLE + 4u, 0x00040003u);
}

static int fl_prop_invalid(void)
{
	return rd32(TOD_PROP_OWNER) == TOD_PROP_INVALID_HI && rd32(TOD_PROP_OWNER + 4u) == TOD_PROP_INVALID_LO &&
	       rd32(TOD_PROP_HANDLE) == TOD_PROP_INVALID_HI && rd32(TOD_PROP_HANDLE + 4u) == TOD_PROP_INVALID_LO;
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
	check(TOD_FASTLOAD_AT == 0x0025E588u && TOD_FASTLOAD_NWORDS == FL_N && TOD_LOAD_SCREEN_DONE == 0x102034FCu,
	      "the patch is twenty-five words of LoadScreen_Update from 0x25E588");

	fl_seed_game();
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	b = 0;
	host_poke(FL_FLAG, &b, 1);
	check(!fl_cave_in(), "the dead function holds no cave of qwark's before");
	host_write_log_reset();
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_words_are(g_fl_words, FL_N),
	      "on: the twenty-five words, as assembled");
	check(rd32(TOD_FASTLOAD_AT + 4u * FL_N) == g_fl_orig[FL_N], "and not a word past them");
	check(fl_cave_in() && fl_sites_are(1), "revision 1.29: qwark's own cave in, and its three sites branch to it");
	check(fl_rd8(FL_FLAG) == 1, "the flag byte its pieces read, at 1");
	check(rd32(TOD_TRANSITION_MIN) == 0, "and g_levelTransitionMinTime at 0: a story FMV load reads at once");
	check(host_write_log_count() > 1, "in a few writes, the cave before the branches");
	host_write_log_reset();
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_words_are(g_fl_words, FL_N) && fl_sites_are(1),
	      "on again is no change");
	check(tod_toggle_read(TF_FAST_LOADS, &on) == ST_NOT_FOUND, "a console reads no flag: qwark's bit says");
	check((features_toggle_state() >> TF_FAST_LOADS) & 1u, "and it is set");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_words_are(g_fl_orig, FL_N + 1) && fl_sites_are(0),
	      "off: the game's own words back, at the three sites too");
	check(fl_rd8(FL_FLAG) == 0 && rd32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME,
	      "the flag at 0 and g_levelTransitionMinTime at the game's 5 s");
	check(fl_cave_in(), "the cave stays: nothing reaches it without the sites");
	check(!((features_toggle_state() >> TF_FAST_LOADS) & 1u), "and the bit clear");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK, "off again is not an error");

	/* The words' sense, read off them: what each branch reaches. */
	check(fl_target(0x25E59Cu, g_fl_words[5]) == 0x25E5E8u && fl_target(0x25E5A8u, g_fl_words[8]) == 0x25E5E8u &&
	      fl_target(0x25E5E8u, g_fl_words[24]) == 0x25E5FCu,
	      "the arrival up and a cinematic playing return through the epilogue");
	check(fl_target(0x25E5A4u, g_fl_words[7]) == 0x25E5B0u, "the load done goes to the cut");
	check(fl_target(0x25E5ACu, g_fl_words[9]) == 0x25E614u, "nothing playing picks the next long-load screen, as the game");
	check(fl_target(0x25E5B4u, g_fl_words[11]) == 0x25ED38u && (g_fl_words[11] & 1u) &&
	      fl_target(0x25E5B8u, g_fl_words[12]) == 0x084EF4u && (g_fl_words[12] & 1u) &&
	      fl_target(0x25E5BCu, g_fl_words[13]) == 0x084E28u && (g_fl_words[13] & 1u),
	      "the cut calls the fade, the cinematic stop and the player reset");
	wr32(0x101D60DCu, 0x11111111u);
	wr32(0x101D60F0u, 0x22222222u);
	fl_seed_prop();
	check(fl_run(g_fl_words + FL_PROP_FROM, FL_PROP_N) && fl_prop_invalid(),
	      "revision 1.29: between the reset and the 1, both of the transition prop's handles made invalid, "
	      "0x03FFFFFF_FFFFFFFF as the game's MobyHandle64_invalidate makes them");
	check(TOD_PROP_OWNER == 0x101D60E0u && TOD_PROP_HANDLE == 0x101D60E8u && rd32(0x101D60DCu) == 0x11111111u &&
	      rd32(0x101D60F0u) == 0x22222222u,
	      "at g_transitionPropOwnerHandle and g_transitionPropHandle, and not a word either side");
	check(fl_target(0x25E588u, g_fl_orig[0]) == 0x25E5FCu,
	      "the first word replaced is the game's bne cr7 to the same epilogue");

	group("ToD: Fast loads' code switch is the console's words");
	check(sw != NULL && sw->ncaves == 3 && sw->nsites == 5 && sw->nflags == 2,
	      "three caves (the switch, qwark's own, build 59's Infinite ammo), five sites, two flags");
	if (sw != NULL && sw->ncaves == 3 && sw->caves[0].len == 4u * (7u + FL_N)) {
		u32 cave = sw->caves[0].addr, pw[FL_PROP_N];

		for (i = 0; i < FL_N; i++) {
			u32 cw = be32_get(sw->caves[0].bytes + 4u * (7u + i));
			u32 tc = fl_target(cave + 4u * (7u + i), cw);
			u32 ts = fl_target(TOD_FASTLOAD_AT + 4u * i, g_fl_words[i]);

			if (cw == g_fl_words[i]) continue;
			/* A branch out of the twenty-five: the same place, and the same link bit. */
			if ((cw >> 26) == 18 && (g_fl_words[i] >> 26) == 18 && tc == ts &&
			    (cw & 3u) == (g_fl_words[i] & 3u) &&
			    (ts < TOD_FASTLOAD_AT || ts >= TOD_FASTLOAD_AT + 4u * FL_N)) continue;
			same = 0;
		}
		check(same, "the trampoline's second half is the twenty-five words, its branches out to the same places");
		for (i = 0; i < FL_PROP_N; i++) pw[i] = be32_get(sw->caves[0].bytes + 4u * (7u + FL_PROP_FROM + i));
		fl_seed_prop();
		check(fl_run(pw, FL_PROP_N) && fl_prop_invalid(),
		      "revision 1.29: under RPCS3 it makes the transition prop's handles invalid the same way");
		check(be32_get(sw->caves[0].bytes + 16) == 0x419E0008u &&
		      fl_target(cave + 20u, be32_get(sw->caves[0].bytes + 20)) == 0x25E5FCu &&
		      fl_target(cave + 24u, be32_get(sw->caves[0].bytes + 24)) == 0x25E58Cu,
		      "and with the flag clear it is the game's bne cr7, taken or not");
	} else {
		check(0, "the cave is the 32 words this test knows");
	}
	check(sw != NULL && sw->sites[0].addr == TOD_FASTLOAD_AT && sw->flags[0] == FL_FLAG &&
	      SW_TOD_FLAG_FAST_LOADS == FL_FLAG, "the site is the console's first word, the flag past the BSS");
	if (sw != NULL && sw->ncaves == 3 && sw->nsites == 5) {
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
	check(features_set(TF_FAST_LOADS, 1) == ST_UNSUPPORTED && fl_words_are(g_fl_orig, FL_N + 1) &&
	      host_write_log_count() == 0, "so Fast loads is UNSUPPORTED and writes nothing");
	wr32(TOD_AMMO_INSTR, TOD_AMMO_INSTR_WORD);
	check(features_set(TF_INFINITE_AMMO, 1) == ST_UNSUPPORTED && rd32(TOD_AMMO_INSTR) == TOD_AMMO_INSTR_WORD &&
	      fl_rd8(SW_TOD_FLAG_INFINITE_AMMO) == 0,
	      "build 59: and so is Infinite ammo, a code patch now: its word and its flag byte untouched");
	check(switches_patch(g_fl_patch, sizeof(g_fl_patch), &len) == ST_OK &&
	      be16_get(g_fl_patch) == 32u + 63u + 5u + 5u,
	      "SWITCH_PATCH: 32, 63 and (build 59) 5 cave words and the five sites");
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

	/* Build 59: Infinite ammo through its own flag, the byte after Fast loads'. */
	check(rd32(TOD_AMMO_INSTR) == SW_TOD_SITE_INFINITE_AMMO && SW_TOD_FLAG_INFINITE_AMMO == 0x108A8561u,
	      "build 59: Weapon_useAmmo's call is the switch's bl, and Infinite ammo's flag is 0x108A8561");
	host_write_log_reset();
	check(features_set(TF_INFINITE_AMMO, 1) == ST_OK && fl_rd8(SW_TOD_FLAG_INFINITE_AMMO) == 1 &&
	      fl_rd8(FL_FLAG) == 0 && rd32(TOD_AMMO_INSTR) == SW_TOD_SITE_INFINITE_AMMO,
	      "Infinite ammo on: its flag byte to 1, Fast loads' left alone, and no code");
	check(tod_toggle_read(TF_INFINITE_AMMO, &on) == ST_OK && on == 1, "its flag reads back as on");
	check(features_set(TF_INFINITE_AMMO, 0) == ST_OK && fl_rd8(SW_TOD_FLAG_INFINITE_AMMO) == 0 &&
	      rd32(TOD_AMMO_INSTR) == SW_TOD_SITE_INFINITE_AMMO, "and off: the flag to 0, still no code");
	check(tod_toggle_read(TF_INFINITE_AMMO, &on) == ST_OK && on == 0, "and reads back as off");
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
	check(TOD_LOAD_FADE_STORY == 0x3DE00000u && f32_from_bits(TOD_LOAD_FADE_STORY) * 64.0f == 7.0f &&
	      TOD_LF_BYTE(TOD_LF_FAST_SHIP_FIXED) == 10u && (TOD_LF_FAST_SHIP_FIXED & 7u) == 2u,
	      "7/64 s for a story load, and LVL_FAST_SHIP_FIXED is bit 42 of g_saveData+0x5530");
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
 * them (a LIVE toggle it refuses), and a new boot of the game puts them in as
 * the session enters it: Infinite health's word, and since build 59 Infinite
 * ammo's patch. God Ratchet, LIVE, still has no auto flag.
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
	wr32(TOD_AMMO_INSTR, TOD_AMMO_INSTR_WORD);   /* a new process: the game's own call */
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	tick(4);
	check(rd32(TOD_AMMO_INSTR) == 0x60000000u && rd32(TOD_NO_DAMAGE) == 1 && rd32(TOD_INFINITE_AMMO) == 0,
	      "a new boot: Infinite ammo's patch and Infinite health's word in as the session enters it");
	check(((features_toggle_state() >> TF_INFINITE_AMMO) & 1u) && ((features_toggle_state() >> TF_INFINITE_HEALTH) & 1u),
	      "and both bits set");
	check(fl_rd8(TOD_PROXY_LOW) == 0, "God Ratchet is left alone");
	(void)slot;
	check(features_set_auto(TF_INFINITE_AMMO, 0) == ST_OK && features_set_auto(TF_INFINITE_HEALTH, 0) == ST_OK,
	      "the auto flags come off");
	features_set(TF_INFINITE_AMMO, 0);
	features_set(TF_INFINITE_HEALTH, 0);
	check(rd32(TOD_AMMO_INSTR) == TOD_AMMO_INSTR_WORD && rd32(TOD_NO_DAMAGE) == 0, "and the cheats with them");
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

/*
 * Half a second into Level_Enter's fade in: seen under way in its dark half
 * first (a tick at 0.75), then its progress down to 0.5, the run's START
 * (2026-10-31; seen under way first since the console's START came early).
 */
static void fade_halfway(void)
{
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.75f);
	tick(1);
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

/*
 * Revision 1.29: a load qwark starts is fast-loaded, as the four
 * original games' PLANET_LOAD is (tod.h, tod_galmap_up): Fast loads forced
 * on for it and put back once the new level is up, unless the toggle is on.
 */
static int fl_patched(void)
{
	return fl_words_are(g_fl_words, FL_N) && fl_sites_are(1) && fl_rd8(FL_FLAG) == 1 && rd32(TOD_TRANSITION_MIN) == 0;
}

static int fl_unpatched(void)
{
	return fl_words_are(g_fl_orig, FL_N + 1) && fl_sites_are(0) && fl_rd8(FL_FLAG) == 0 &&
	       rd32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME;
}

static int fl_bit(void)
{
	return (int)((features_toggle_state() >> TF_FAST_LOADS) & 1u);
}

/* The world fade_world makes, with a level up and the loader idle. */
static void fl_world(u32 level)
{
	fade_world();
	wr32(TOD_CURRENT_LEVEL, level);
	wr32(TOD_LOAD_JOB, 0);
	wr8(AS_TIER, 1);
	wr8(TOD_FRONT_END, 0);
	metropolis_beaten(1);
	tick(4);
}

/* The game takes the request (ProcessPending), the load runs, the new level comes up. */
static void fl_game_loads(u32 level, u32 lua)
{
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_CURRENT_LEVEL, level);
	tick(2);
	load_begins();
	tick(6);
	load_data_in();
	tick(4);
}

static void test_tod_load_fast(void)
{
	u32 seq, len = 0, i;
	int on = -1;

	group("ToD: qwark's planet loads are fast-loaded, the toggle on or off (revision 1.29)");
	fl_seed_game();
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_unpatched() && !fl_bit(), "Fast loads off");
	fl_world(2);
	seq = autosplit_latest_seq();
	check(session_planet_load(3, 0) == ST_OK && rd32(TOD_LOAD_REQUEST) == 3 && fl_patched(),
	      "PLANET_LOAD: Fast loads forced on for it (the words, the sites, the flag, the 0 s minimum)");
	check(!fl_bit(), "the toggle's bit stays clear: the force is not the toggle");
	tick(6);
	check(fl_patched(), "kept while the request waits");
	fl_game_loads(3, 0x30040000u);
	check(fl_patched(), "and through the load, the data in included (the cut comes then)");
	level_enters(0x30040000u);
	tick(4);
	check(fl_unpatched() && !fl_bit(), "the new level up: the game's own words back, the toggle still off");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 1 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0 &&
	      as_count(seq, AUTOSPLIT_LOAD_START, TOD_AS_LOAD, NULL) == 0,
	      "the autosplitter as for any load: one planet split, no reset, no long load");

	/* The toggle on: nothing forced, nothing put back. */
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_patched() && fl_bit(), "Fast loads on");
	fl_world(3);
	check(session_planet_load(5, 0) == ST_OK && fl_patched(), "PLANET_LOAD with it on");
	fl_game_loads(5, 0x30050000u);
	level_enters(0x30050000u);
	tick(4);
	check(fl_patched() && fl_bit(), "the load over: still on, as the toggle has it");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_unpatched(), "and off when the toggle goes off");

	/* A load the game never takes: put back after the wait, as the fade is. */
	fl_world(5);
	check(session_planet_load(7, 0) == ST_OK && fl_patched(), "a load the game never takes, forced");
	tick(TOD_FADE_WAIT_TICKS - 4);
	check(fl_patched(), "kept for its wait");
	tick(8);
	check(fl_unpatched(), "then put back");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);

	/* A refused load forces nothing. */
	fl_world(5);
	wr32(TOD_GAME_STATE, 3);
	check(session_planet_load(7, 0) == ST_BUSY && fl_unpatched(), "a refused PLANET_LOAD forces nothing");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);

	/* The toggle turned on during a forced load: the toggle has the say. */
	fl_world(5);
	check(session_planet_load(8, 0) == ST_OK && fl_patched(), "forced again");
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_patched(), "the toggle on during the load");
	fl_game_loads(8, 0x30060000u);
	level_enters(0x30060000u);
	tick(4);
	check(fl_patched() && fl_bit(), "the load over: left on");
	check(tod_toggle_read(TF_FAST_LOADS, &on) == ST_NOT_FOUND, "a console reads no flag: qwark's bit says");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_unpatched(), "off");
	fl_world(8);

	group("ToD: qwark's planet loads under RPCS3: the switch's flag for the load");
	check(quit(), "the game quits");
	host_set_can_patch_code(0);
	host_set_emulator(1);
	check(boot_tod(), "ToD boots under RPCS3");
	tick(6);
	check(switches_patch(g_fl_patch, sizeof(g_fl_patch), &len) == ST_OK, "the switches' patch");
	check(quit(), "the game quits");
	host_boot_as(TOD_TITLE, "tod");
	fl_poke_patch(g_fl_patch, len);
	for (i = 0; i < 4000 && !(session_state() == SESSION_INGAME && session_game() != NULL); i++) tick(1);
	tick(6);
	check(switches_state() == 1 && fl_rd8(FL_FLAG) == 0, "and comes back with it in, Fast loads off");
	fl_world(2);
	host_write_log_reset();
	check(session_planet_load(3, 0) == ST_OK && fl_rd8(FL_FLAG) == 1 && rd32(TOD_TRANSITION_MIN) == 0 &&
	      rd32(TOD_FASTLOAD_AT) == SW_TOD_SITE_FAST_LOADS, "PLANET_LOAD: the flag at 1 for the load, no code written");
	check(tod_toggle_read(TF_FAST_LOADS, &on) == ST_OK && on == 0 && !fl_bit(),
	      "the toggle still reads off: the force is not the toggle");
	fl_game_loads(3, 0x30070000u);
	check(fl_rd8(FL_FLAG) == 1, "kept through the load");
	level_enters(0x30070000u);
	tick(4);
	check(fl_rd8(FL_FLAG) == 0 && rd32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME, "the level up: the flag back at 0");
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_rd8(FL_FLAG) == 1, "Fast loads on");
	fl_world(3);
	check(session_planet_load(5, 0) == ST_OK, "PLANET_LOAD with it on");
	fl_game_loads(5, 0x30080000u);
	level_enters(0x30080000u);
	tick(4);
	check(fl_rd8(FL_FLAG) == 1 && tod_toggle_read(TF_FAST_LOADS, &on) == ST_OK && on == 1, "left on");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_rd8(FL_FLAG) == 0, "off");
	wr8(FL_FLAG, 1);   /* a flag this process did not set: the user's setting, as on RaC1 */
	fl_world(5);
	check(session_planet_load(7, 0) == ST_OK, "PLANET_LOAD over a flag left at 1");
	fl_game_loads(7, 0x30090000u);
	level_enters(0x30090000u);
	tick(4);
	check(fl_rd8(FL_FLAG) == 1, "left at 1: it was not this load's");
	wr8(FL_FLAG, 0);
	check(quit(), "the game quits");
	host_set_can_patch_code(1);
	host_set_emulator(0);
	check(boot_tod(), "and comes back on a console");
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

/* The flags of the last event of this kind and code after `seq`, 0xFFFFFFFF for none. */
static u32 as_flags(u32 seq, u8 kind, u8 code)
{
	u32 n, i, flags = 0xFFFFFFFFu;

	if (autosplit_encode_events(seq, g_ev, sizeof(g_ev)) < 5) return flags;
	n = g_ev[4];
	for (i = 0; i < n; i++) {
		const u8 *e = g_ev + 5 + i * AUTOSPLIT_EVENT_SIZE;
		if (e[8] == kind && e[9] == code) flags = be16_get(e + 10);
	}
	return flags;
}

/*
 * The seen movies list LevelTransition_begin adds a story load's FMVs to
 * before they play (LevelFlags_markMovieSeen: g_levelFlags + 0x30D8, a u32
 * name hash each, the count at + 0x3178; the hash djb2 over the lower-cased
 * name, Movie_hashNameNoCase). Not qwark's: the test writes it as the game
 * would, so a watcher that took "seen" for "revisit" is caught.
 */
#define AS_SEEN_LIST  (TOD_LEVEL_FLAGS + 0x30D8u)
#define AS_SEEN_COUNT (TOD_LEVEL_FLAGS + 0x3178u)

static void seen_mark(const char *name)
{
	u32 h = 0x1505u, n = rd32(AS_SEEN_COUNT);

	for (; *name != 0; name++) h = h * 33u + (u32)((*name >= 'A' && *name <= 'Z') ? *name + 32 : *name);
	wr32(AS_SEEN_LIST + 4u * n, h);
	wr32(AS_SEEN_COUNT, n + 1u);
}

/* A planet load qwark starts (PLANET_LOAD), as the game takes it. */
static void qwark_load(u32 level, u32 lua)
{
	check(session_planet_load(level, 0) == ST_OK, "PLANET_LOAD is taken");
	fl_game_loads(level, lua);
	level_enters(lua);
	tick(4);
}

/*
 * Revision 1.33 (pending): a load into Metropolis or Cobalia is marked a
 * revisit, AUTOSPLIT_EV_REVISIT in the planet event's flags, when
 * LVL_FAST_SHIP_FIXED (106) is set, the game's own test for their arrival
 * FMVs (LevelTransition_begin), and never otherwise: not on another planet,
 * not from Metropolis being beaten (185), not from the FMVs in the seen list,
 * and not from Fast loads or Remove cutscenes, which are run on and off here.
 */
static void test_tod_autosplit_revisit_one(int fast, int cut)
{
	char what[160];
	u32 seq, arg = 0;
	const char *mode = fast ? (cut ? "Fast loads on, Remove cutscenes on" : "Fast loads on, Remove cutscenes off")
	                        : (cut ? "Fast loads off, Remove cutscenes on" : "Fast loads off, Remove cutscenes off");

#define WHAT(s) (sprintf(what, "%s (%s)", (s), mode), what)
	fl_seed_game();
	cut_build();
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	check(features_set(TF_FAST_LOADS, fast ? 1u : 0u) == ST_OK && (fast ? fl_patched() : fl_unpatched()),
	      WHAT("Fast loads set"));
	check(features_set(TF_REMOVE_CUTSCENES, cut ? 1u : 0u) == ST_OK && cut_all(cut), WHAT("Remove cutscenes set"));
	wr32(AS_SEEN_COUNT, 0);
	fl_world(8);
	fade_flag_106(0);
	planet_load(9, AS_STATE + 0x1C0);   /* Rykan V up by a load, so it is where the next load comes from */

	/* Cobalia's first arrival, the game's own load; its FMVs seen before the load is. */
	seq = autosplit_latest_seq();
	seen_mark("l02_s01_hypr_fmv");
	seen_mark("l02_s02_zoni_fmv");
	planet_load(TOD_LEVEL_COBALIA, AS_STATE + 0x200);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 1 &&
	      as_flags(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET) == 0,
	      WHAT("Cobalia, flag 106 clear: a planet split with no revisit bit, its FMVs seen or not"));

	/* Metropolis with the ship not yet fixed: the game plays its intro FMV again, so no revisit. */
	seq = autosplit_latest_seq();
	seen_mark("l01_s01_ntro_fmv");
	qwark_load(TOD_LEVEL_METROPOLIS, AS_STATE + 0x240);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 0 &&
	      as_flags(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET) == 0 && as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0,
	      WHAT("Metropolis, beaten, flag 106 clear (PLANET_LOAD): a split, no revisit, no run"));

	/* The ship fixed on Fastoon: flag 106 set. Another planet is never a revisit. */
	fade_flag_106(1);
	seq = autosplit_latest_seq();
	planet_load(9, AS_STATE + 0x280);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 9 &&
	      as_flags(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET) == 0,
	      WHAT("Rykan V with flag 106 set: no revisit bit (only Metropolis and Cobalia have one)"));

	/* Cobalia again, through PLANET_LOAD: a revisit. */
	seq = autosplit_latest_seq();
	qwark_load(TOD_LEVEL_COBALIA, AS_STATE + 0x2C0);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 1 &&
	      as_flags(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET) == AUTOSPLIT_EV_REVISIT,
	      WHAT("Cobalia with flag 106 set (PLANET_LOAD): the split carries the revisit bit"));

	/* Reloaded, and a death there: still never a second split. */
	seq = autosplit_latest_seq();
	planet_load(TOD_LEVEL_COBALIA, AS_STATE + 0x300);
	death(AS_STATE + 0x340);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, WHAT("Cobalia reloaded, then a death: no split"));

	/* Metropolis again, the game's own load: a revisit, no run. */
	seq = autosplit_latest_seq();
	planet_load(TOD_LEVEL_METROPOLIS, AS_STATE + 0x380);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 0 &&
	      as_flags(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET) == AUTOSPLIT_EV_REVISIT &&
	      as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 0 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      WHAT("Metropolis with flag 106 set: the split carries the revisit bit, no run"));

	/* A Cobalia save from before Fastoon loaded over it: its flags in first, then the load. */
	seq = autosplit_latest_seq();
	fade_flag_106(0);
	planet_load(TOD_LEVEL_COBALIA, AS_STATE + 0x3C0);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 1 &&
	      as_flags(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET) == 0,
	      WHAT("a save's flags with 106 clear, then Cobalia: the story's arrival again, no revisit"));

	check((fast ? fl_patched() : fl_unpatched()) && cut_all(cut), WHAT("both switches as set throughout"));
#undef WHAT
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && features_set(TF_REMOVE_CUTSCENES, 0) == ST_OK,
	      "both switches off again");
	wr32(AS_SEEN_COUNT, 0);
	fade_flag_106(0);
}

static void test_tod_autosplit_revisit(void)
{
	group("ToD: a load into Metropolis or Cobalia past their arrival FMVs is a revisit (rev 1.33, pending)");
	test_tod_autosplit_revisit_one(0, 0);
	test_tod_autosplit_revisit_one(1, 0);
	test_tod_autosplit_revisit_one(0, 1);
	test_tod_autosplit_revisit_one(1, 1);
}

/*
 * Revision 1.34: "Fastoon: Clank section", code 9, the player put in control
 * of Clank on Fastoon (g_heroList's current slot becoming 4, tod.h), once an
 * arrival: never on Fastoon II, never again after a death or a checkpoint as
 * Clank (whose respawn passes through -1 and Ratchet's slot first), a reload
 * of Fastoon, a switch back to Ratchet and to Clank again, or a quit to the
 * title and a Fastoon save loaded; again after a trip elsewhere and back (a
 * new run's load into Metropolis is one). A slot that reads Clank's at the
 * first look is not the switch.
 */
#define AS_SLOT (TOD_HERO_LIST + TOD_HL_CURRENT)

static void hero_slot(u32 slot)
{
	wr32(AS_SLOT, slot);
	tick(4);
}

/* A death as the current hero, the hero list rebuilt on the respawn: -1, Ratchet's slot, then `slot` again. */
static void death_as(u32 slot, u32 lua)
{
	wr32(AS_SLOT, TOD_HERO_SLOT_NONE);
	death(lua);
	hero_slot(0);
	hero_slot(slot);
}

static void test_tod_autosplit_clank(void)
{
	u32 seq, arg = 0;

	group("ToD: Fastoon: Clank section, code 9, once an arrival (rev 1.34)");
	fl_world(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	hero_slot(0);

	/* Stratus City to Fastoon, then the Clank pad. */
	seq = autosplit_latest_seq();
	planet_load(3, AS_STATE + 0x400);
	hero_slot(0);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 3 &&
	      as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0, "Fastoon arrived at as Ratchet: no subsplit yet");
	seq = autosplit_latest_seq();
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, &arg) == 1 && arg == 3 && as_all(seq) == 1,
	      "the switch to Clank: one subsplit, arg Fastoon, and nothing else");

	/* Dying as Clank and coming back as Clank, a checkpoint, a switch back and forth, a reload. */
	seq = autosplit_latest_seq();
	death_as(TOD_HERO_SLOT_CLANK, AS_STATE + 0x440);
	death_as(TOD_HERO_SLOT_CLANK, AS_STATE + 0x480);
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	planet_load(3, AS_STATE + 0x4C0);
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0,
	      "deaths as Clank, a switch back to Ratchet and to Clank again, Fastoon reloaded: no second subsplit");

	/* A quit to the title and a Fastoon save loaded: the same arrival's. */
	seq = autosplit_latest_seq();
	to_title();
	from_title();
	tick(6);
	load_data_in();
	level_enters(AS_STATE + 0x500);
	tick(4);
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0,
	      "the title, then a Fastoon save, then Clank: no second subsplit");

	/* Somewhere else and back: a new arrival splits again. Elsewhere, Clank's slot is nothing. */
	seq = autosplit_latest_seq();
	planet_load(9, AS_STATE + 0x540);
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0, "Rykan V, the same slot: no subsplit");
	planet_load(3, AS_STATE + 0x580);
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 1, "back on Fastoon: the switch splits again");

	/* Fastoon II is another level: nothing there. */
	seq = autosplit_latest_seq();
	planet_load(TOD_LEVEL_FASTOON_II, AS_STATE + 0x5C0);
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0, "Fastoon II: no subsplit");

	/* An arrival already on Clank's slot (nothing seen before it): never the switch. */
	seq = autosplit_latest_seq();
	planet_load(3, AS_STATE + 0x600);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0, "Clank's slot from the first look: no subsplit");
	hero_slot(TOD_HERO_SLOT_NONE);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 0, "nor after a -1, which is no hero");
	hero_slot(0);
	hero_slot(TOD_HERO_SLOT_CLANK);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_FASTOON_CLANK, NULL) == 1, "Ratchet, then Clank: the switch");
	hero_slot(0);
	planet_load(2, AS_STATE + 0x6C0);
	wr32(AS_SLOT, 0);
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
	      d[0].flags == (AUTOSPLIT_FLAG_DEFAULT | AUTOSPLIT_FLAG_ROUTE | AUTOSPLIT_FLAG_REVISIT) &&
	      qstreq(d[0].label, "Planet entered"),
	      "code 1 is the planet split, on, with the route, marking revisits (rev 1.33, pending)");
	for (i = 1; i < n; i++) check((d[i].flags & AUTOSPLIT_FLAG_REVISIT) == 0, "no other row marks revisits");
	check(d[1].code == TOD_AS_END_FADE && d[1].kind == AUTOSPLIT_SPLIT && d[1].flags == AUTOSPLIT_FLAG_DEFAULT &&
	      qstreq(d[1].label, "Tachyon defeated"),
	      "code 7, the fade to black after Tachyon, is the final split, \"Tachyon defeated\", on (rev 1.29)");
	check(d[2].code == TOD_AS_MISSION && d[2].flags == 0, "every mission is there, off");
	check(d[3].code == 9 && TOD_AS_FASTOON_CLANK == 9 && d[3].kind == AUTOSPLIT_SPLIT && d[3].flags == 0 &&
	      qstreq(d[3].label, "Fastoon: Clank section"),
	      "code 9, the next free (2, 5 and 8 retired), Fastoon: Clank section, off and not the route's (rev 1.34)");
	check(d[4].code == TOD_AS_LOAD && d[4].kind == AUTOSPLIT_LOAD_START &&
	      d[4].flags == (AUTOSPLIT_FLAG_DEFAULT | AUTOSPLIT_FLAG_HOLD) && d[4].param_us == 0 &&
	      qstreq(d[4].label, "Long load"),
	      "the long load holds game time (HOLD, rev 1.29), no NORMALISE");
	check(d[5].code == TOD_AS_QUIT && d[5].kind == AUTOSPLIT_PAUSE &&
	      d[5].flags == (AUTOSPLIT_FLAG_DEFAULT | AUTOSPLIT_FLAG_NORMALISE) && d[5].param_us == 16900000u,
	      "the quit to the XMB costs 16.9 s");
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

/* Revision 1.33: a ToD mod whose patch.txt says it is not run-legal, made (1) or taken away (0). */
static void rw_mod_put(int make)
{
	static const char folder[] = "/dev_hdd0/qwark/mods/" TOD_TITLE "/tod-rw-illegal";
	static const char text[] = "#- name: ToD RW Illegal\n#- legal: false\n0x00880000: 0x60000000\n";
	char path[160];
	plat_file_t f;

	snprintf(path, sizeof(path), "%s/patch.txt", folder);
	if (!make) {
		plat_file_unlink(path);
		plat_dir_remove(folder);
		return;
	}
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, (u32)strlen(text));
	plat_file_close(f);
}

static void test_tod_autosplit_runs(void)
{
	static const u32 before[] = { 635, 636, 637 };
	u32 ids[4], seq, arg = 0, i;
	u32 rw_starts, rw_notes;
	int rc_was;

	group("ToD: run starts on the Metropolis flag, deaths, and no credits split");

	/* A new game: the title, a file with MOVIE_METROPOLIS_TACHYON clear, Metropolis. */
	wr32(TOD_CURRENT_LEVEL, 3);
	wr32(TOD_LOAD_SCREEN, 1);
	tick(2);
	to_title();
	metropolis_beaten(0);
	wr32(TOD_CURRENT_LEVEL, 0);
	tick(2);
	/*
	 * Revision 1.30, the run-start warnings: two cheats on, and
	 * Remove cutscenes, run-legal, on as well; the START says the two.
	 * 2026-11-12: and Remove black screens, a practice cheat, makes three.
	 */
	runwarn_forget();
	rc_was = (features_toggle_state() & ((u64)1 << TF_REMOVE_CUTSCENES)) != 0;
	check(features_set(TF_INFINITE_AMMO, 1) == ST_OK && features_set(TF_INFINITE_HEALTH, 1) == ST_OK &&
	      features_set(TF_REMOVE_CUTSCENES, 1) == ST_OK && features_set(TF_REMOVE_BLACK, 1) == ST_OK,
	      "Infinite ammo, Infinite health, Remove cutscenes and Remove black screens on");
	rw_starts = runwarn_starts();
	rw_notes = host_notify_count();
	seq = autosplit_latest_seq();
	wr32(TOD_LOAD_SCREEN, 4);   /* no level to fly from: a long load screen at once */
	from_title();
	check(as_count(seq, AUTOSPLIT_RESET, 0, NULL) == 1 && as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "a new game is a RESET as its load begins, and no START yet");
	check(runwarn_starts() == rw_starts, "the RESET is no run start for the warnings");
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
	check(runwarn_starts() == rw_starts + 1 && runwarn_last_cheats() == 3 &&
	      host_notify_count() == rw_notes + 1 && strcmp(host_notify_last(), "Warning: 3 cheats enabled") == 0,
	      "the START is the run start: \"Warning: 3 cheats enabled\", Remove black screens counted, Remove cutscenes not");
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, NULL) == 0, "Metropolis is no planet split");
	tick(10);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "one START");

	/* A death on Metropolis, flag still clear: a new lua_State and a new fade in, and nothing at all. */
	seq = autosplit_latest_seq();
	wr32(TOD_GAME_STATE, 5);
	death(AS_STATE + 0x20);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(4);
	check(runwarn_starts() == rw_starts + 1, "and no run-start warning");
	features_set(TF_INFINITE_AMMO, 0);
	features_set(TF_INFINITE_HEALTH, 0);
	features_set(TF_REMOVE_BLACK, 0);
	features_set(TF_REMOVE_CUTSCENES, (u32)rc_was);
	check(as_all(seq) == 0, "nothing at all");
	seq = autosplit_latest_seq();
	quick_death(AS_STATE + 0x60);
	death(AS_STATE + 0x60);
	check(as_all(seq) == 0, "two more, one between two ticks, one with the same lua_State back: nothing");

	/*
	 * Metropolis loaded again on Metropolis, flag still clear (a new game or a save there): a new run.
	 * Revision 1.33: with run.warnings 2, cheats only, and a mod the run may not have in the game,
	 * its START says the cheats line and no mod line.
	 */
	rw_mod_put(1);
	check(mods_rescan() == ST_OK && mods_load("tod-rw-illegal") == ST_OK, "a mod marked not run-legal loaded in ToD");
	check(features_set(TF_INFINITE_AMMO, 1) == ST_OK, "Infinite ammo on again");
	check(config_set_run_warnings(RUN_WARN_CHEATS_ONLY) == ST_OK && config_run_warnings() == RUN_WARN_CHEATS_ONLY,
	      "run.warnings = 2, cheats only");
	rw_starts = runwarn_starts();
	rw_notes = host_notify_count();
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
	check(runwarn_starts() == rw_starts + 1 && runwarn_last_cheats() == 1 && runwarn_last_illegal() == 1 &&
	      host_notify_count() == rw_notes + 1 && strcmp(host_notify_last(), "Warning: 1 cheat enabled") == 0,
	      "cheats only: the mod counted, and the START says \"Warning: 1 cheat enabled\" and no mod line");
	config_set_run_warnings(RUN_WARN_ALL);
	features_set(TF_INFINITE_AMMO, 0);
	mods_unload("tod-rw-illegal");
	rw_mod_put(0);
	mods_rescan();

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

	/* The credits, on Fastoon II: no split since revision 1.29 retired code 5. */
	seq = autosplit_latest_seq();
	wr32(TOD_GAME_STATE, TOD_STATE_CREDITS);
	tick(3);
	check(as_count(seq, AUTOSPLIT_SPLIT, 5, NULL) == 0 && as_all(seq) == 0, "the credits are no split (code 5 retired)");

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
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.56f);
	tick(2);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "0.56 of the way: not yet");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.55f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "0.45 s in, the progress at 0.55: START, the same tick (2026-11-02)");
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
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "the tick that finds it past 0.55 (0.3): START");
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	level_enters(AS_STATE);
	tick(1);
	wr32(FAKE_ROOT + TOD_FADE_PROGRESS, 0x80000000u);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "a progress of -0.0 is past 0.55 too");

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
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "the object back, its target -0.0, a quarter dark and still: not seen under way, no START");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.2f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "the progress going down (0.25, 0.2): under way, START");

	/*
	 * The console's START half a second early (revision 1.29): the level is
	 * up, its data in and a lua_State, while Level_StartGameplay runs, before
	 * Level_Enter's Set(1.0) and StartIn, with the fade still clear from the
	 * screens before it (target 0.0, progress 0). That passed the test at
	 * once. Now the fade in must be seen under way first.
	 */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr32(TOD_LOAD_JOB, 0);
	tick(60);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0,
	      "Metropolis up before Level_Enter's fade, the fade left clear (0.0, 0): no START for half a second and more");
	{
		u32 i, at = 0;

		/* Level_Enter: Set(1.0), StartIn(1.0, 0); then the game's frames take it down 1/60 at a time, a frame a tick. */
		wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
		wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
		for (i = 0; i <= 60 && at == 0; i++) {
			wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f - (f32)i / 60.0f);
			tick(1);
			if (as_count(seq, AUTOSPLIT_START, 0, NULL) == 1) at = i;
		}
		check(at == 27, "then START 27 of the fade's 60 frames in: the fade in's start + 0.45 s (2026-11-02)");
	}

	/* A load screen's own fade in, still clear at the level's up, does not arm it either; a fade out starts the look again. */
	seq = autosplit_latest_seq();
	new_game_load();
	load_data_in();
	tick(2);
	wr32(TOD_LUA_STATE, AS_STATE);
	wr32(TOD_LOAD_JOB, 0);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.75f);
	tick(1);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 1.0f);      /* Level_Enter's Set(1.0) seen between its two calls */
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.3f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "a target of 1.0: no START");
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.3f);
	tick(3);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 0, "after it, a still 0.3 is not a fade in seen under way");
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 1.0f);
	tick(1);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.5f);
	tick(1);
	check(as_count(seq, AUTOSPLIT_START, 0, NULL) == 1, "its fade in from black at half: START");

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

static int sf_refused(const char *log, u8 err, u8 why);

/*
 * 2026-11-14: a load at once, as LoadLevel makes one outside play (tod.h,
 * TOD_LEVEL_NAMES). How many of g_tierStack's tiers are marked to go (the
 * simulator's five, bottom first: a level load marks the top three).
 */
static u32 tiers_dying(void)
{
	u32 count = rd32(TOD_TIER_STACK + TOD_TS_COUNT), i, n = 0;

	for (i = 0; i < count && i < TOD_TIER_MAX; i++) {
		u32 tier = rd32(TOD_TIER_STACK + TOD_TS_TIERS + i * 4u);

		if (rd8(tier + TOD_TIER_DYING) == 0) continue;
		if (i + TOD_LEVEL_LOAD_TIERS < count) return 99;   /* one below the level's three: never */
		n++;
	}
	return n;
}

/* Level_setName's three words for `level`, and g_saveData.currentLevel. */
static int level_names_at(u32 level)
{
	u32 name = rd32(TOD_LEVEL_NAMES + level * 4u);

	return name != 0 && rd32(TOD_LEVEL_NAME) == name && rd32(TOD_LEVEL_NAME_LAST) == name &&
	       rd32(TOD_CURRENT_LEVEL) == level;
}

/* The game's GameState_Update taking it: the request back to none, the marked tiers gone and back. */
static void load_now_taken(void)
{
	u32 count = rd32(TOD_TIER_STACK + TOD_TS_COUNT), i;

	wr32(TOD_REQUESTED_STATE, TOD_REQUESTED_NONE);
	for (i = 0; i < count && i < TOD_TIER_MAX; i++) wr8(rd32(TOD_TIER_STACK + TOD_TS_TIERS + i * 4u) + TOD_TIER_DYING, 0);
}

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

	/*
	 * 2026-11-14: in the pause, the pause menu's own Load: the save in, and
	 * onLoaded's LoadLevel(level, 1, 0) loading at once, not a request only a
	 * gameplay frame would take. Until now error 6, "not in gameplay".
	 */
	wr32(TOD_BOLTS, 7);
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_CURRENT_LEVEL, 9);
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_UP);
	check(savefile_restore("runs", "a.sav") == ST_OK, "a RESTORE in the pause is accepted");
	check(sf_settle(1, 300), "and settles");
	check(rd32(TOD_BOLTS) == 1000 && rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu &&
	      rd32(TOD_REQUESTED_STATE) == TOD_LEVEL_LOAD_STATE && level_names_at(1) && tiers_dying() == 3,
	      "the save in, its level loading at once as LoadLevel does in the pause: g_requestedGameState 3, "
	      "the top three tiers marked, the name set, and no planet request");
	load_now_taken();
	wr32(TOD_BOLTS, 7);
	wr32(TOD_GAME_STATE, TOD_STATE_CINEMATIC);
	check(g->load_setaside() == ST_OK && sf_settle(1, 300) && rd32(TOD_BOLTS) == 1000 &&
	      rd32(TOD_REQUESTED_STATE) == TOD_LEVEL_LOAD_STATE && rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "Load set-aside during a cinematic: the same, the cinematic's own exit run by the unload");
	load_now_taken();
	/* Kept: an FMV, the galactic map, a pause still being built, a state change already asked for. */
	wr32(TOD_GAME_STATE, TOD_STATE_FMV);
	check(sf_refused("tod: restore refused, not in gameplay (g_gameState 7", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_NOT_GAMEPLAY), "an FMV: still refused (its exit, FmvState_exit, is not the unload's)");
	wr32(TOD_GAME_STATE, TOD_STATE_GALMAP);
	check(sf_refused("tod: restore refused, not in gameplay (g_gameState 6", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_NOT_GAMEPLAY), "the galactic map: still refused");
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_LOADING);
	check(sf_refused("pause 1)", SAVEFILE_ERR_NOT_IDLE, SAVEFILE_WHY_NOT_GAMEPLAY),
	      "a pause whose loading thread is still building it: refused");
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_UP);
	wr32(TOD_REQUESTED_STATE, 2);
	check(sf_refused("a state change is already asked for (g_requestedGameState 2)", SAVEFILE_ERR_NOT_IDLE,
	                 SAVEFILE_WHY_NOT_GAMEPLAY), "nor over a state change already asked for");
	check(rd32(TOD_REQUESTED_STATE) == 2 && tiers_dying() == 0, "which is left as it was");
	wr32(TOD_REQUESTED_STATE, TOD_REQUESTED_NONE);
	wr32(TOD_PAUSE_STATE, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);

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

	group("ToD: savefile loads after any boot, and why a refusal was (revision 1.29)");
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

/* ------------------- a RESTORE is the set-aside too, revision 1.29 */

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

	group("ToD: a RESTORE is the set-aside too, as in the other games (revision 1.29)");
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

/*
 * Revision 1.30: Load set-aside is fast-loaded, as RaC1's, RaC2's and UYA's
 * set-aside loads are (rac1_load_setaside), with PLANET_LOAD's force and
 * watch; a RESTORE of a library file forces nothing, in ToD as in every game.
 */
static void test_tod_savefile_aside_fast(void)
{
	group("ToD: Load set-aside is fast-loaded, as RaC1's to UYA's are; a RESTORE is not (revision 1.30)");
	check(boot_tod(), "ToD boots");
	tick(4);
	fl_seed_game();
	wr32(TOD_TRANSITION_MIN, TOD_TRANSITION_GAME);
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_unpatched() && !fl_bit(), "Fast loads off");

	/* A RESTORE of a library file: the short fade, no force. */
	wr32(TOD_BOLTS, 11);
	check(sf_restore_ok("a.sav", 1000) && sf_bolts(SF_ASIDE_PATH) == 1000, "a RESTORE of a.sav loads, and is the set-aside");
	check(fl_unpatched() && !fl_bit(), "and forces nothing: the game's own words, the flag 0, the 5 s");

	/* Load set-aside: forced from its acceptance until the new level is up. */
	fl_world(2);
	wr32(TOD_BOLTS, 12);
	check(sf_load_aside_ok(1000), "Load set-aside loads the set-aside");
	check(fl_patched(), "with Fast loads forced for its load (the words, the sites, the flag, the 0 s minimum)");
	check(!fl_bit(), "the toggle's bit stays clear: the force is not the toggle");
	fl_game_loads(5, 0x300A0000u);
	check(fl_patched(), "kept through the level's load");
	level_enters(0x300A0000u);
	tick(4);
	check(fl_unpatched() && !fl_bit(), "the new level up: the game's own words back, the toggle still off");

	/* A refused Load set-aside forces nothing. */
	fl_world(5);
	wr32(TOD_GAME_STATE, 3);
	check(sf_trigger(TF_LOAD_ASIDE) == ST_BUSY && fl_unpatched(), "a refused Load set-aside forces nothing");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);

	/* A load the game never takes: put back after the wait, as PLANET_LOAD's is. */
	fl_world(5);
	check(sf_load_aside_ok(1000) && fl_patched(), "forced again");
	tick(TOD_FADE_WAIT_TICKS + 8);
	check(fl_unpatched(), "no level load in six seconds: put back");

	/* With the toggle on, nothing is forced or put back. */
	check(features_set(TF_FAST_LOADS, 1) == ST_OK && fl_patched() && fl_bit(), "Fast loads on");
	fl_world(5);
	check(sf_load_aside_ok(1000), "Load set-aside with it on");
	fl_game_loads(6, 0x300B0000u);
	level_enters(0x300B0000u);
	tick(4);
	check(fl_patched() && fl_bit(), "the load over: still on, as the toggle has it");
	check(features_set(TF_FAST_LOADS, 0) == ST_OK && fl_unpatched(), "and off again");
	fl_world(5);
	check(quit(), "the game quits");
}

/* -------------------------------------- the cutscene cache, revision 1.29 */

/*
 * Cache cutscenes (19), Cache cutscenes on reset (20) and Disable cutscene
 * caching (21), tod_cache.c. The state machine is driven directly, with a
 * state and a watcher's view of its own, against a stand-in for the game's
 * warmer that does what LevelPrefetch_setup, LevelPrefetch_update and its
 * read's callback do, slots, steps and all; then once through the session,
 * the feature table and the readout.
 */
#define CC_TOC       0x30400000u
#define CC_LEVELS    0x30400100u
#define CC_STR       0x30401000u
#define CC_CACHE     0x30410000u
#define CC_BLOCKS    0x30411000u
#define CC_PATHS     0x30420000u
#define CC_GAME      0x30430000u   /* the level's own four files: { path, size } */
#define CC_PF        TOD_PREFETCH
#define CC_COBALIA   (CC_LEVELS + 0u * TOD_LA_STRIDE + TOD_LA_UNCACHED)
#define CC_FASTOON   (CC_LEVELS + 1u * TOD_LA_STRIDE + TOD_LA_UNCACHED)
#define CC_STRATUS   (CC_LEVELS + 3u * TOD_LA_STRIDE + TOD_LA_UNCACHED)
#define CC_C_AT      10928u
#define CC_F_AT      7769u
#define CC_S_AT      9716u
#define CC_C_END     69919408u
#define CC_F_END     52633177u
#define CC_S_END     66944500u
#define CC_C_SIZE    318591664u    /* Cobalia's level_uncached.psarc */
#define CC_NBLOCKS   70u
#define CC_LUA       0x30030000u
#define CC_MIB       0x100000u
#define CC_GAME_FILE(i) (CC_GAME + (i) * 8u)

static const u32 cc_game_size[4] = { 8u * CC_MIB, 3u * CC_MIB, 12u * CC_MIB, 6u * CC_MIB };

static void wrstr(u32 addr, const char *s)
{
	host_poke(addr, (const u8 *)s, (u32)strlen(s) + 1u);
}

static void wr64(u32 addr, u64 v)
{
	wr32(addr, (u32)(v >> 32));
	wr32(addr + 4u, (u32)v);
}

static u64 rd64(u32 addr)
{
	return ((u64)rd32(addr) << 32) | rd32(addr + 4u);
}

/* game.toc's level table: cobalia, fastoon, metropolis, stratus city, with their level_uncached sizes. */
static void cc_toc(void)
{
	static const char * const dirs[4] = { "cobalia", "fastoon", "metropolis", "stratus city" };
	static const u32 sizes[4] = { CC_C_SIZE, 215391833u, 233549981u, 278255092u };
	u32 i;

	wr32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, CC_TOC);
	wr32(CC_TOC + TOD_TOC_LEVELS, CC_LEVELS);
	wr32(CC_LEVELS - 4u, 4);
	for (i = 0; i < 4; i++) {
		wrstr(CC_STR + i * 0x40u, dirs[i]);
		wrstr(CC_STR + 0x100u + i * 0x80u, "/packed/levels/x/level_uncached.psarc");
		wr32(CC_LEVELS + i * TOD_LA_STRIDE, CC_STR + i * 0x40u);
		wr32(CC_LEVELS + i * TOD_LA_STRIDE + TOD_LA_UNCACHED, CC_STR + 0x100u + i * 0x80u);
		wr32(CC_LEVELS + i * TOD_LA_STRIDE + TOD_LA_UNCACHED + 4u, sizes[i]);
	}
}

/* The warmer idle in gameplay: every file read, none in hand. */
static void cc_warmer_idle(void)
{
	wr32(CC_PF + TOD_PF_READY, 0);
	wr32(CC_PF + TOD_PF_BUSY, 0);
	wr32(CC_PF + TOD_PF_CHUNK, 0);
	wr32(CC_PF + TOD_PF_CUR, 0);
	wr32(CC_PF + TOD_PF_STATE, TOD_PF_NFILES);
	wr8(CC_PF + TOD_PF_ENABLED, 1);
}

/* LevelPrefetch_setup: off, the reads' words zeroed, the level's four files, readReady. */
static void cc_setup(void)
{
	u32 i;

	wr8(CC_PF + TOD_PF_ENABLED, 0);
	for (i = 0x24u; i < 0x54u; i += 4u) wr32(CC_PF + i, 0);
	for (i = 0; i < 4; i++) {
		wr32(CC_GAME_FILE(i), CC_STR + 0x400u + i * 0x40u);
		wr32(CC_GAME_FILE(i) + 4u, cc_game_size[i]);
		wr32(CC_PF + TOD_PF_FILE + i * 4u, CC_GAME_FILE(i));
		wr32(CC_PF + TOD_PF_SIZE + i * 4u, cc_game_size[i]);
	}
	wr32(CC_PF + TOD_PF_READY, 1);
}

/* What the stand-in read: each read's file in order of change, and the bytes per file. */
#define CC_SEQ 16
static u32 cc_seq[CC_SEQ], cc_nseq, cc_bytes_c, cc_bytes_s, cc_bytes_f, cc_bytes_g[4], cc_first_g0;

static void cc_reads_reset(void)
{
	cc_nseq = 0;
	cc_bytes_c = cc_bytes_s = cc_bytes_f = 0;
	memset(cc_bytes_g, 0, sizeof(cc_bytes_g));
	cc_first_g0 = 0xFFFFFFFFu;
}

static void cc_note(u32 cur, u32 off, u32 chunk)
{
	u32 i;

	if (cc_nseq == 0 || cc_seq[cc_nseq - 1u] != cur) {
		if (cc_nseq < CC_SEQ) cc_seq[cc_nseq++] = cur;
		if (cur == CC_GAME_FILE(0) && cc_nseq > 1) cc_first_g0 = off;   /* where the slot's file is taken up again */
	}
	if (cur == CC_COBALIA) cc_bytes_c += chunk;
	if (cur == CC_STRATUS) cc_bytes_s += chunk;
	if (cur == CC_FASTOON) cc_bytes_f += chunk;
	for (i = 0; i < 4; i++) if (cur == CC_GAME_FILE(i)) cc_bytes_g[i] += chunk;
}

/*
 * One frame of the game's warmer, as LevelPrefetch_update and its read's
 * callback (the read done at once): nothing unless on and readReady; with
 * chunkSize 0 the step, state s taking slot s (offset 0, the first chunk) or,
 * past the last, curFile and readBusy let go; then the read, offset on by
 * chunkSize, the next chunk min(fileEnd - offset, 256 KB), unsigned as the
 * game's.
 */
static void cc_frame(void)
{
	u32 off, end, chunk, st, cur;

	if (rd8(CC_PF + TOD_PF_ENABLED) == 0 || rd32(CC_PF + TOD_PF_READY) == 0) return;
	wr32(CC_PF + TOD_PF_READY, 0);
	if (rd32(CC_PF + TOD_PF_CHUNK) == 0) {
		st = rd32(CC_PF + TOD_PF_STATE);
		if (st < TOD_PF_NFILES) {
			wr32(CC_PF + TOD_PF_STATE, st + 1u);
			wr32(CC_PF + TOD_PF_END, rd32(CC_PF + TOD_PF_SIZE + st * 4u));
			wr32(CC_PF + TOD_PF_CUR, rd32(CC_PF + TOD_PF_FILE + st * 4u));
			if (rd32(CC_PF + TOD_PF_FILE + st * 4u) == 0) wr32(CC_PF + TOD_PF_READY, 1);
		} else {
			wr32(CC_PF + TOD_PF_CUR, 0);
		}
		if (rd32(CC_PF + TOD_PF_CUR) == 0) {
			wr32(CC_PF + TOD_PF_BUSY, 0);
			return;
		}
		end = rd32(CC_PF + TOD_PF_END);
		wr32(CC_PF + TOD_PF_OFFSET, 0);
		wr32(CC_PF + TOD_PF_CHUNK, end < TOD_PF_CHUNK_MAX ? end : TOD_PF_CHUNK_MAX);
	}
	cur = rd32(CC_PF + TOD_PF_CUR);
	if (cur == 0) {
		wr32(CC_PF + TOD_PF_BUSY, 0);
		return;
	}
	wr32(CC_PF + TOD_PF_BUSY, 1);
	chunk = rd32(CC_PF + TOD_PF_CHUNK);
	off = rd32(CC_PF + TOD_PF_OFFSET);
	cc_note(cur, off, chunk);
	off += chunk;
	end = rd32(CC_PF + TOD_PF_END);
	wr32(CC_PF + TOD_PF_OFFSET, off);
	wr32(CC_PF + TOD_PF_CHUNK, end - off < TOD_PF_CHUNK_MAX ? end - off : TOD_PF_CHUNK_MAX);
	wr32(CC_PF + TOD_PF_READY, 1);
}

/* A watcher's view: in gameplay on `level`, the loader idle. */
static void cc_play(struct tod_as_state *as, s32 level, u32 lua)
{
	as->cur_level = level;
	as->cur_lua = lua;
	as->cur_state = TOD_STATE_GAMEPLAY;
	as->cur_front = 0;
	as->cur_job = 0;
	as->cur_tier = 1;
}

/* A planet load under way into `level`: the level gone, the loader on its tiers. */
static void cc_loading(struct tod_as_state *as, s32 level)
{
	as->cur_level = level;
	as->cur_lua = 0;
	as->cur_state = 3;
	as->cur_job = TOD_LOAD_JOB_LEVEL;
	as->cur_tier = 0;
}

/* `n` ticks of the state machine, each with `frames` frames of the warmer after it. */
static void cc_run(struct tod_cc_state *cc, const struct tod_as_state *as, u32 n, u32 frames)
{
	u32 i, f;

	for (i = 0; i < n; i++) {
		tod_cc_tick(cc, as);
		for (f = 0; f < frames; f++) cc_frame();
	}
}

/* `n` ticks with one frame of the warmer every fourth: 256 KB in 1/30 s, 7.5 MiB/s. */
static void cc_run_slow(struct tod_cc_state *cc, const struct tod_as_state *as, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		tod_cc_tick(cc, as);
		if ((i & 3u) == 3u) cc_frame();
	}
}

/* Ticks until the request ends (at most `n`), the warmer `frames` frames a tick. */
static void cc_run_out(struct tod_cc_state *cc, const struct tod_as_state *as, u32 n, u32 frames)
{
	u32 i;

	for (i = 0; i < n && (tod_cc_status(cc) >> 28) <= TOD_CC_CACHING; i++) cc_run(cc, as, 1, frames);
}

static u32 cc_st(const struct tod_cc_state *cc)   { return tod_cc_status(cc) >> 28; }
static u32 cc_det(const struct tod_cc_state *cc)  { return (tod_cc_status(cc) >> 24) & 15u; }
static u32 cc_pct(const struct tod_cc_state *cc)  { return (tod_cc_status(cc) >> 16) & 0xFFu; }

/* Ticks, one frame of the warmer each, until the warmer reads `desc` (at most `n`). */
static void cc_run_to(struct tod_cc_state *cc, const struct tod_as_state *as, u32 desc, u32 n)
{
	u32 i;

	for (i = 0; i < n && rd32(CC_PF + TOD_PF_CUR) != desc; i++) cc_run(cc, as, 1, 1);
}

/* A cache block: lastUse, its offset, its path handle. */
static void cc_block(u32 i, u64 use, u32 off, u32 handle)
{
	u32 at = CC_BLOCKS + i * TOD_FC_DESC_LEN;

	wr64(at + TOD_FC_LAST_USE, use);
	wr64(at + TOD_FC_OFFSET, off);
	wr32(at + 0x20u, 0x100000u);   /* the valid bytes, as the game's */
	wr32(at + TOD_FC_PATH, handle);
}

static u64 cc_use(u32 i)
{
	return rd64(CC_BLOCKS + i * TOD_FC_DESC_LEN + TOD_FC_LAST_USE);
}

/* The cache object and its path table, `n` blocks; the blocks are the caller's. */
static void cc_cache(u32 n)
{
	static const char * const paths[6] = {
		"/packed/levels/stratus city/level_uncached.psarc",
		"/packed/levels/stratus city/level_textures.psarc",
		"/packed/levels/fastoon/level_uncached.psarc",
		"/packed/game/global_uncached.psarc",
		"/packed/levels/metropolis/level_uncached.psarc",
		"/packed/levels/cobalia/level_uncached.psarc"
	};
	u32 i;

	wr32(TOD_FIOS_CACHE, CC_CACHE);
	wr32(CC_CACHE + TOD_FC_BLOCK_SIZE, 0x100000u);
	wr32(CC_CACHE + TOD_FC_NBLOCKS, n);
	wr32(CC_CACHE + TOD_FC_BLOCKS, CC_BLOCKS);
	wr32(TOD_FIOS_PATHS, CC_PATHS);
	wr32(CC_PATHS + TOD_FPATH_STRS, CC_PATHS + 0x100u);
	wr32(CC_PATHS + TOD_FPATH_COUNT, 6);
	for (i = 0; i < 6; i++) {
		wrstr(CC_PATHS + 0x200u + i * 0x80u, paths[i]);
		wr32(CC_PATHS + 0x100u + i * 4u, CC_PATHS + 0x200u + i * 0x80u);
	}
}

/*
 * The block table: 70 blocks (three pieces of the walk), in use with a time
 * of 1000 + i, by handle: 1 Stratus City's level_uncached, 2 its textures,
 * 3 Fastoon's level_uncached, 4 global_uncached, 5 Metropolis's
 * level_uncached, 6 Cobalia's level_uncached. Block 5 is filling and block 6
 * free (Stratus City's both), and the last Stratus City block is number 66,
 * in the third piece; Cobalia's sound blocks 0..4 are blocks 45..49.
 */
static void cc_blocks(void)
{
	u32 i;

	cc_cache(CC_NBLOCKS);
	for (i = 0; i < CC_NBLOCKS; i++) cc_block(i, 1000u + i, (i % 8u) << 20, 2);   /* textures, by default */
	for (i = 0; i < 5; i++) cc_block(i, 1000u + i, i << 20, 1);
	cc_block(5, TOD_FC_FILLING, 5u << 20, 1);
	cc_block(6, TOD_FC_FREE, 6u << 20, 1);
	cc_block(66, 1066u, 63u << 20, 1);
	for (i = 10; i < 15; i++) cc_block(i, 1000u + i, (i - 10u) << 20, 3);
	cc_block(20, 1020u, 0, 4);
	cc_block(21, 1021u, 56u << 20, 4);
	cc_block(22, 1022u, 57u << 20, 4);
	cc_block(30, 1030u, 113u << 20, 5);
	for (i = 45; i < 50; i++) cc_block(i, 1000u + i, (i - 45u) << 20, 6);
}

/* Every block of the three planets' sound in the cache: Cobalia's 0..66, Stratus City's 0..63, Fastoon's 0..50. */
static void cc_blocks_all(void)
{
	u32 i;

	cc_cache(190u);
	for (i = 0; i < 190u; i++) cc_block(i, 2000u + i, 0, 2);
	for (i = 0; i < 64u; i++) cc_block(i, 2000u + i, i << 20, 1);
	for (i = 0; i < 51u; i++) cc_block(64u + i, 2100u + i, i << 20, 3);
	for (i = 0; i < 67u; i++) cc_block(120u + i, 2200u + i, i << 20, 6);
}

/* The level's own warm-up under way on its slot 0, `mib` MiB in, in gameplay on `level`. */
static void cc_level_up(struct tod_as_state *as, s32 level, u32 frames)
{
	cc_setup();
	cc_play(as, level, CC_LUA);
	wr8(CC_PF + TOD_PF_ENABLED, 1);
	while (frames-- > 0) cc_frame();
}

/*
 * Revision 1.34: the sound file by the game's language (tod.h,
 * TOD_SYSTEM_LANGUAGE). LevelPrefetch_setup reads streaming_sound.jp.dat on
 * a console set to Japanese (g_systemLanguage 0) and streaming_sound.dat in
 * every other language; g_systemParam122 (the word before it) only picks US
 * or UK English, and revision 1.29 took its 2 for Japanese. The warmer's
 * slot 0 holds the game's own choice, which wins once seen; a streaming_sound
 * file of another name fails the request before anything is written.
 * Called with the warmer idle on Fastoon, no block table.
 */
#define CC_SND_DESC  (CC_GAME + 0x100u)   /* a slot 0 descriptor { path, size } of the test's own */
#define CC_SND_STR   (CC_STR + 0x800u)
#define CC_C_LEN     69908480u
#define CC_S_LEN     66934784u
#define CC_F_LEN     52625408u
#define CC_PARAM122  (TOD_SYSTEM_LANGUAGE - 4u)   /* g_systemParam122, 0x100EB570 */

static void cc_slot0(const char *path)
{
	wrstr(CC_SND_STR, path);
	wr32(CC_SND_DESC, CC_SND_STR);
	wr32(CC_SND_DESC + 4u, CC_F_LEN);
	wr32(CC_PF + TOD_PF_FILE, CC_SND_DESC);
}

static void test_tod_cc_language(struct tod_cc_state *cc, struct tod_as_state *as)
{
	char line[ACTION_STATUS_TEXT_MAX + 1], row[24];
	u32 i, f0 = rd32(CC_PF + TOD_PF_FILE);

	/* English, g_systemParam122 at 2 (UK English): the plain file, which revision 1.29 got wrong. */
	wr32(TOD_SYSTEM_LANGUAGE, 1);
	wr32(CC_PARAM122, 2);
	check(tod_cc_trigger(cc, as) == ST_OK, "English with g_systemParam122 at 2");
	cc_run(cc, as, 4, 0);
	check(rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA && rd32(CC_PF + TOD_PF_OFFSET) == CC_C_AT &&
	      rd32(CC_PF + TOD_PF_END) == CC_C_END,
	      "streaming_sound.dat: the parameter picks US or UK English, never the .jp twin (rev 1.34)");
	cc_run_out(cc, as, 2000, 4);
	check(cc_st(cc) == TOD_CC_DONE && cc_det(cc) == 0, "done, its detail not Japanese");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(cc), MENU_STATUS_LINE, line, sizeof(line));
	check(strstr(line, "Cutscenes cached (") == line && strstr(line, "Japanese") == NULL, "its line names no language");
	wr32(CC_PARAM122, 1);

	/* Japanese: each planet's .jp twin, whole, and nothing of the plain files. */
	wr32(TOD_SYSTEM_LANGUAGE, TOD_LANG_JAPANESE);
	cc_reads_reset();
	check(tod_cc_trigger(cc, as) == ST_OK, "a console set to Japanese");
	cc_run(cc, as, 4, 0);
	check(rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA && rd32(CC_PF + TOD_PF_OFFSET) == CC_C_END &&
	      rd32(CC_PF + TOD_PF_END) == CC_C_END + CC_C_LEN,
	      "Japanese: Cobalia's streaming_sound.jp.dat, the twin after its streaming_sound.dat");
	cc_run_out(cc, as, 4000, 4);
	check(cc_st(cc) == TOD_CC_DONE && cc_det(cc) == TOD_CC_DONE_JP, "done, the detail says Japanese");
	check(cc_bytes_c == CC_C_LEN && cc_bytes_s == CC_S_LEN && cc_bytes_f == CC_F_LEN,
	      "each twin read once, its length exactly");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(cc), MENU_STATUS_LINE, line, sizeof(line));
	check(strstr(line, "Japanese audio)") != NULL, "the line says the Japanese audio was cached");

	/* Already cached in Japanese: the twins' blocks are in, the plain files' are not. */
	cc_cache(190u);
	for (i = 0; i < 190u; i++) cc_block(i, 3000u + i, 0, 2);
	for (i = 0; i < 68u; i++) cc_block(i, 3000u + i, (66u + i) << 20, 6);          /* Cobalia's twin, 66..133 */
	for (i = 0; i < 65u; i++) cc_block(68u + i, 3100u + i, (63u + i) << 20, 1);    /* Stratus City's, 63..127 */
	for (i = 0; i < 51u; i++) cc_block(133u + i, 3200u + i, (50u + i) << 20, 3);   /* Fastoon's, 50..100 */
	cc_reads_reset();
	check(tod_cc_trigger(cc, as) == ST_OK, "Japanese, every twin's block in the cache");
	cc_run_out(cc, as, 200, 4);
	check(cc_st(cc) == TOD_CC_DONE && cc_pct(cc) == 0 && cc_det(cc) == TOD_CC_DONE_JP && cc_nseq == 0,
	      "already cached: done, nothing read, the twins' blocks counted");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(cc), MENU_STATUS_LINE, line, sizeof(line));
	check(qstreq(line, "Cutscenes already cached (all three planets, Japanese audio)"), "its line");
	wr32(TOD_SYSTEM_LANGUAGE, 1);
	check(tod_cc_trigger(cc, as) == ST_OK, "the same cache in English");
	cc_run(cc, as, 16, 0);
	check(cc_st(cc) == TOD_CC_CACHING && rd32(CC_PF + TOD_PF_OFFSET) == CC_C_AT,
	      "the plain files are not in: Cobalia's streaming_sound.dat is read");
	cc_run_out(cc, as, 4000, 4);
	wr32(TOD_FIOS_CACHE, 0);

	/* The game's own choice wins: slot 0 names the twin while the language reads English. */
	cc_slot0("/built/levels/fastoon/streaming_sound.jp.dat");
	check(tod_cc_trigger(cc, as) == ST_OK, "slot 0 streaming_sound.jp.dat, the language English");
	cc_run(cc, as, 8, 0);
	check(rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA && rd32(CC_PF + TOD_PF_OFFSET) == CC_C_END &&
	      rd32(CC_PF + TOD_PF_END) == CC_C_END + CC_C_LEN, "the twin: what the game reads, not what the language says");
	cc_run_out(cc, as, 4000, 4);
	check(cc_st(cc) == TOD_CC_DONE && cc_det(cc) == TOD_CC_DONE_JP, "done, Japanese");

	/* And the other way: slot 0 the plain file on a Japanese console. */
	cc_slot0("/built/levels/fastoon/streaming_sound.dat");
	wr32(TOD_SYSTEM_LANGUAGE, TOD_LANG_JAPANESE);
	check(tod_cc_trigger(cc, as) == ST_OK, "slot 0 streaming_sound.dat, the language Japanese");
	cc_run(cc, as, 8, 0);
	check(rd32(CC_PF + TOD_PF_OFFSET) == CC_C_AT && rd32(CC_PF + TOD_PF_END) == CC_C_END, "the plain file");
	cc_run_out(cc, as, 4000, 4);
	check(cc_st(cc) == TOD_CC_DONE && cc_det(cc) == 0, "done, not Japanese");

	/* A streaming_sound file of another name (another build's layout): it fails, visibly, nothing written. */
	cc_slot0("/built/levels/fastoon/streaming_sound.fr.dat");
	check(tod_cc_trigger(cc, as) == ST_OK, "slot 0 a streaming_sound.fr.dat");
	cc_run(cc, as, 8, 0);
	check(cc_st(cc) == TOD_CC_FAILED && cc_det(cc) == TOD_CC_WHY_SOUND && rd32(CC_PF + TOD_PF_CUR) == 0 &&
	      rd32(CC_PF + TOD_PF_READY) == 0, "fails before writing anything");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(cc), MENU_STATUS_LINE, line, sizeof(line));
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(cc), MENU_STATUS_ROW, row, sizeof(row));
	check(strstr(line, "sound file is not one qwark knows") != NULL && qstreq(row, "Unknown sound file") &&
	      strlen(row) <= 19, "and says so, on the line and on the menu's row");

	/* A slot 0 that is no sound file (a texture archive): the language decides. */
	cc_slot0("/built/levels/fastoon/texstream.dat");
	check(tod_cc_trigger(cc, as) == ST_OK, "slot 0 another file, the language Japanese");
	cc_run(cc, as, 8, 0);
	check(rd32(CC_PF + TOD_PF_OFFSET) == CC_C_END, "the language's choice, the twin");
	cc_run_out(cc, as, 4000, 4);
	wr32(TOD_SYSTEM_LANGUAGE, 1);
	wr32(CC_PF + TOD_PF_FILE, f0);
}

static void test_tod_cutscene_cache(void)
{
	struct tod_cc_state cc;
	struct tod_as_state as;
	const struct game_describe *dsc;
	u32 old_toc = rd32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC), info_word, i, freed, off;
	u8 info[SESSION_INFO_SIZE];
	char text[24];
	int ok_19 = 0, ok_20 = 0, ok_21 = 0;

	group("ToD: Cache cutscenes, its auto on reset and Disable cutscene caching (revision 1.29)");
	check(session_game() != NULL, "ToD is up");
	if (session_game() == NULL) return;
	dsc = session_game()->describe();
	for (i = 0; i < dsc->nfeatures; i++) {
		const struct feature_desc *f = &dsc->features[i];
		u8 bad = (u8)(f->flags & (FEATURE_FLAG_WRITES_CODE | FEATURE_FLAG_LIVE));
		if (f->id == TF_CACHE_CUTSCENES)
			ok_19 = f->kind == FEATURE_ACTION && !bad && f->readout == TOD_RO_CUTSCENE_CACHE && qstreq(f->label, "Cache cutscenes");
		if (f->id == TF_CACHE_ON_RESET)
			ok_20 = f->kind == FEATURE_TOGGLE && !bad && qstreq(f->label, "Cache cutscenes on reset");
		if (f->id == TF_NO_CUTSCENE_CACHE)
			ok_21 = f->kind == FEATURE_TOGGLE && !bad && qstreq(f->label, "Disable cutscene caching");
	}
	check(ok_19 && TF_CACHE_CUTSCENES == 19, "19 Cache cutscenes, an ACTION naming its status readout, data writes");
	check(ok_20 && ok_21, "20 Cache cutscenes on reset and 21 Disable cutscene caching, toggles, not LIVE");
	check(dsc->nreadouts == 6 && qstreq(dsc->readouts[TOD_RO_CUTSCENE_CACHE], "Cutscene cache"),
	      "readout 3, Cutscene cache, where it was (build 59 put the armour's and the skin's after it)");

	/* ---- the button with the warmer idle: Cobalia, Stratus City, then Fastoon, the single-range form */
	cc_toc();
	wr32(TOD_FIOS_CACHE, 0);                      /* no block table: everything is read */
	wr32(TOD_SYSTEM_LANGUAGE, 1);                 /* English, as the simulator's console */
	memset(&as, 0, sizeof(as));
	tod_cc_forget(&cc);
	cc_play(&as, 3, CC_LUA);
	cc_warmer_idle();
	wr32(CC_PF + TOD_PF_STATE, 0);                /* the level's own files just set up, not started */
	wr32(CC_PF + TOD_PF_READY, 1);
	wr8(CC_PF + TOD_PF_ENABLED, 0);
	cc_run(&cc, &as, 2, 0);
	check(tod_cc_status(&cc) == 0, "no run this boot: the word is 0");
	check(tod_cc_trigger(&cc, &as) == ST_OK, "Cache cutscenes is taken");
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && cc_det(&cc) == TOD_CC_WAIT_WARMER && rd32(CC_PF + TOD_PF_CUR) == 0 &&
	      rd32(CC_PF + TOD_PF_STATE) == 0, "the warmer set up but not reading yet: waiting, and left alone");
	check(tod_cc_trigger(&cc, &as) == ST_BUSY, "one at a time: BUSY while one waits");
	cc_warmer_idle();
	wr8(CC_PF + TOD_PF_ENABLED, 0);
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && rd32(CC_PF + TOD_PF_CUR) == 0, "idle but switched off (a pause): still waiting");
	wr8(CC_PF + TOD_PF_ENABLED, 1);
	cc_reads_reset();
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 3 && cc_pct(&cc) == 0, "idle: caching Cobalia first, 0%");
	check(rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA && rd32(CC_PF + TOD_PF_OFFSET) == CC_C_AT &&
	      rd32(CC_PF + TOD_PF_END) == CC_C_END && rd32(CC_PF + TOD_PF_CHUNK) == TOD_PF_CHUNK_MAX &&
	      rd32(CC_PF + TOD_PF_STATE) == TOD_PF_NFILES && rd32(CC_PF + TOD_PF_READY) == 1,
	      "the single-range form: Cobalia's .uncached descriptor, its sound's start to end, 256 KB, state 4, readReady");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(qstreq(text, "Cobalia 0%"), "the menu's row: Cobalia 0%");
	cc_run_slow(&cc, &as, 4 * 128);              /* 128 reads: 32 MiB */
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 3 && cc_pct(&cc) >= 45 && cc_pct(&cc) <= 52,
	      "about half way: its percentage");
	cc_run_slow(&cc, &as, 4 * 145);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 2 && rd32(CC_PF + TOD_PF_CUR) == CC_STRATUS &&
	      rd32(CC_PF + TOD_PF_END) == CC_S_END, "Cobalia in, Stratus City next, to its sound's end");
	cc_run_slow(&cc, &as, 4 * 262);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 1 && rd32(CC_PF + TOD_PF_CUR) == CC_FASTOON &&
	      rd32(CC_PF + TOD_PF_END) == CC_F_END, "Stratus City in, Fastoon last, to its sound's end");
	cc_run_slow(&cc, &as, 4 * 220);
	check(cc_st(&cc) == TOD_CC_DONE && (tod_cc_status(&cc) & 0xFFu) == 1, "all three in: done, one run ended");
	check(cc_pct(&cc) >= 70 && cc_pct(&cc) <= 76, "done: the read rate in tenths of a MiB/s (7.5 MiB/s here)");
	check(cc_nseq == 3 && cc_seq[0] == CC_COBALIA && cc_seq[1] == CC_STRATUS && cc_seq[2] == CC_FASTOON &&
	      cc_bytes_c == CC_C_END - CC_C_AT && cc_bytes_s == CC_S_END - CC_S_AT && cc_bytes_f == CC_F_END - CC_F_AT,
	      "each sound read once, exactly, in the order a run needs them");
	check(rd32(CC_PF + TOD_PF_CUR) == 0 && rd32(CC_PF + TOD_PF_BUSY) == 0 && rd32(CC_PF + TOD_PF_STATE) == TOD_PF_NFILES,
	      "and the warmer idle again, as the game leaves it");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(strncmp(text, "Done (", 6) == 0 && text[strlen(text) - 2] == 's' && text[strlen(text) - 1] == ')' &&
	      strstr(text, "MB/s") == NULL && strlen(text) <= 11, "the menu's row: Done (Ns), the seconds alone (2026-11-02)");

	test_tod_cc_language(&cc, &as);

	/* ---- the archive too small, no table */
	wr32(CC_LEVELS + TOD_LA_UNCACHED + 4u, 1000u);
	check(tod_cc_trigger(&cc, &as) == ST_OK, "a level table whose Cobalia archive is too small for the range");
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_TABLE && rd32(CC_PF + TOD_PF_CUR) == 0,
	      "fails before writing anything");
	wr32(CC_LEVELS + TOD_LA_UNCACHED + 4u, CC_C_SIZE);
	wr32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, 0);
	check(tod_cc_trigger(&cc, &as) == ST_OK, "no level table at all");
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_TABLE, "fails: no level table");
	cc_toc();

	/* ---- going first: ahead of the level's own slot, which is read again after */
	cc_level_up(&as, 9, 8);                      /* Rykan V's warmer on its slot 0, 2 MiB in */
	cc_reads_reset();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "asked for with the warmer on the level's own sound");
	cc_run(&cc, &as, 4, 0);
	off = rd32(CC_PF + TOD_PF_OFFSET);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 3 && rd32(CC_PF + TOD_PF_FILE) == CC_COBALIA &&
	      rd32(CC_PF + TOD_PF_SIZE) == CC_C_END && rd32(CC_PF + TOD_PF_STATE) == 0 &&
	      rd32(CC_PF + TOD_PF_CUR) == CC_GAME_FILE(0) && rd32(CC_PF + TOD_PF_END) == off + 0x100000u,
	      "slot 0 holds Cobalia's sound, state 0 again, and the slot's own read cut 1 MiB on");
	cc_run(&cc, &as, 6, 1);
	check(rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA && rd32(CC_PF + TOD_PF_STATE) == 1, "at the cut the step takes ours");
	cc_run(&cc, &as, 8, 1);
	check(rd32(CC_PF + TOD_PF_FILE) == CC_STRATUS && rd32(CC_PF + TOD_PF_SIZE) == CC_S_END && rd32(CC_PF + TOD_PF_STATE) == 0,
	      "while it is read, Stratus City goes into the same slot");
	cc_run_to(&cc, &as, CC_STRATUS, 600);
	check(cc_st(&cc) == TOD_CC_CACHING && rd32(CC_PF + TOD_PF_CUR) == CC_STRATUS,
	      "Cobalia in, Stratus City read: the step took it from the same slot");
	cc_run(&cc, &as, 8, 1);
	check(cc_det(&cc) == 2 && rd32(CC_PF + TOD_PF_FILE) == CC_FASTOON && rd32(CC_PF + TOD_PF_SIZE) == CC_F_END &&
	      rd32(CC_PF + TOD_PF_STATE) == 0, "caching Stratus City, and Fastoon into the slot after it");
	cc_run_to(&cc, &as, CC_FASTOON, 600);
	cc_run(&cc, &as, 8, 1);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 1 && rd32(CC_PF + TOD_PF_CUR) == CC_FASTOON,
	      "Stratus City in, Fastoon read from the same slot");
	check(rd32(CC_PF + TOD_PF_FILE) == CC_GAME_FILE(0) && rd32(CC_PF + TOD_PF_SIZE) == cc_game_size[0] &&
	      rd32(CC_PF + TOD_PF_STATE) == 0, "and the slot's own file back in it, state 0, while Fastoon is read");
	cc_run_out(&cc, &as, 600, 1);
	check(cc_st(&cc) == TOD_CC_DONE, "done");
	cc_run(&cc, &as, 200, 1);
	check(cc_nseq == 8 && cc_seq[0] == CC_GAME_FILE(0) && cc_seq[1] == CC_COBALIA && cc_seq[2] == CC_STRATUS &&
	      cc_seq[3] == CC_FASTOON && cc_seq[4] == CC_GAME_FILE(0) && cc_seq[5] == CC_GAME_FILE(1) &&
	      cc_seq[6] == CC_GAME_FILE(2) && cc_seq[7] == CC_GAME_FILE(3),
	      "the warmer's order: its slot 0 to the cut, Cobalia, Stratus City, Fastoon, slot 0 again, 1, 2, 3");
	check(cc_bytes_c == CC_C_END && cc_bytes_s == CC_S_END && cc_bytes_f == CC_F_END, "a slot reads ours from 0: whole, once");
	check(cc_first_g0 == 0 && cc_bytes_g[0] <= cc_game_size[0] - 2u * CC_MIB + 4u * TOD_PF_CHUNK_MAX &&
	      cc_bytes_g[1] == cc_game_size[1] && cc_bytes_g[2] == cc_game_size[2] && cc_bytes_g[3] == cc_game_size[3],
	      "the slot's file taken up again from 0, put on to its cut at the next look; the rest as the game has them");
	check(rd32(CC_PF + TOD_PF_CUR) == 0 && rd32(CC_PF + TOD_PF_STATE) == TOD_PF_NFILES &&
	      rd32(CC_PF + TOD_PF_FILE) == CC_GAME_FILE(0) && rd32(CC_PF + TOD_PF_SIZE) == cc_game_size[0],
	      "and the warmer idle with its slots as the game set them up");

	/* ---- resumed across a level load, what is in skipped */
	cc_level_up(&as, TOD_LEVEL_METROPOLIS, 4);
	cc_reads_reset();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again, on Metropolis");
	cc_run_to(&cc, &as, CC_STRATUS, 800);        /* the cut, all of Cobalia's, */
	cc_run(&cc, &as, 85, 1);                     /* then about 20 MiB of Stratus City's */
	off = rd32(CC_PF + TOD_PF_OFFSET);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 2 && rd32(CC_PF + TOD_PF_CUR) == CC_STRATUS &&
	      off > 18u * CC_MIB && off < 24u * CC_MIB && cc_bytes_c == CC_C_END, "Cobalia in, Stratus City part read");
	cc_loading(&as, TOD_LEVEL_COBALIA);           /* the runner leaves: Level_Exit, the warmer off */
	wr8(CC_PF + TOD_PF_ENABLED, 0);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && cc_det(&cc) == TOD_CC_WAIT_PLAY,
	      "a load into Cobalia, its sound in: no end, waiting");
	cc_run(&cc, &as, 30, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && rd32(CC_PF + TOD_PF_CUR) == CC_STRATUS, "and nothing written during the load");
	cc_reads_reset();
	cc_level_up(&as, TOD_LEVEL_COBALIA, 2);
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 2 && rd32(CC_PF + TOD_PF_FILE) == CC_STRATUS &&
	      rd32(CC_PF + TOD_PF_STATE) == 0, "Cobalia's warmer up: Stratus City goes ahead of its own again");
	cc_run(&cc, &as, 12, 1);
	check(rd32(CC_PF + TOD_PF_CUR) == CC_STRATUS && rd32(CC_PF + TOD_PF_OFFSET) >= 16u * CC_MIB &&
	      rd32(CC_PF + TOD_PF_OFFSET) <= off + 2u * CC_MIB && cc_bytes_s <= 2u * CC_MIB,
	      "taken from 0, then put on at the block it was let go in");
	cc_run_out(&cc, &as, 2000, 1);
	check(cc_st(&cc) == TOD_CC_DONE, "and done on Cobalia");
	check(cc_bytes_s < CC_S_END - 16u * CC_MIB && cc_bytes_c == 0,
	      "what Metropolis read is not read again (but the slot's first chunks), Cobalia's not at all");

	/* ---- too late: a load into a planet before its sound is in drops it, the rest goes on */
	cc_level_up(&as, TOD_LEVEL_METROPOLIS, 2);
	cc_reads_reset();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 20, 1);
	check(cc_det(&cc) == 3, "caching Cobalia");
	cc_loading(&as, TOD_LEVEL_COBALIA);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_WAITING, "a load into Cobalia first: too late for it, but the request goes on");
	cc_level_up(&as, TOD_LEVEL_COBALIA, 2);
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 2 && rd32(CC_PF + TOD_PF_FILE) == CC_STRATUS,
	      "on Cobalia: Stratus City next, Cobalia dropped");
	cc_run_out(&cc, &as, 2000, 1);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_LATE_CB, "the rest in, it ends saying too late for Cobalia");
	check(cc_bytes_s == CC_S_END && cc_bytes_f == CC_F_END && cc_bytes_c < CC_C_END, "Stratus City and Fastoon read whole");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(qstreq(text, "Too late: Cobalia"), "the menu's row says so");
	cc_level_up(&as, TOD_LEVEL_METROPOLIS, 2);
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 20, 1);
	cc_loading(&as, 2);                          /* a planet load from the menu, past Cobalia */
	cc_run(&cc, &as, 2, 0);
	cc_level_up(&as, 2, 2);
	cc_run(&cc, &as, 2, 0);
	cc_loading(&as, 3);                          /* and on to Fastoon */
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_WAITING, "loads into Stratus City and Fastoon first: both too late, Cobalia still to read");
	cc_level_up(&as, 3, 2);
	cc_run_out(&cc, &as, 2000, 1);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_LATE_SC,
	      "Cobalia in, then the first late one by a run's order: Stratus City");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(qstreq(text, "Too late: Stratus"), "the menu's row says so");
	cc_level_up(&as, TOD_LEVEL_METROPOLIS, 2);
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 20, 1);
	cc_loading(&as, TOD_LEVEL_COBALIA);
	cc_run(&cc, &as, 2, 0);
	cc_level_up(&as, TOD_LEVEL_COBALIA, 2);
	cc_run(&cc, &as, 20, 1);
	cc_loading(&as, 2);
	cc_run(&cc, &as, 2, 0);
	cc_level_up(&as, 2, 2);
	cc_run(&cc, &as, 20, 1);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 1, "too late for Cobalia and Stratus City: Fastoon under way");
	cc_loading(&as, 3);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_LATE_CB,
	      "too late for all three: it ends at the load, the first late one said");
	cc_play(&as, 3, CC_LUA);
	cc_warmer_idle();

	/* ---- what the cache has already is skipped */
	cc_blocks();
	cc_warmer_idle();
	cc_play(&as, 3, CC_LUA);
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again, the block table holding some of all three");
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && rd32(CC_PF + TOD_PF_CUR) == 0, "the block table first: three ticks for 70 blocks");
	cc_run(&cc, &as, 6, 0);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 3 && rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA &&
	      rd32(CC_PF + TOD_PF_OFFSET) == 5u * CC_MIB && rd32(CC_PF + TOD_PF_END) == CC_C_END,
	      "Cobalia: blocks 0..4 in, so from 5 MiB to its end");
	cc_run_to(&cc, &as, CC_STRATUS, 600);
	check(cc_det(&cc) == 2 && rd32(CC_PF + TOD_PF_OFFSET) >= 6u * CC_MIB &&
	      rd32(CC_PF + TOD_PF_OFFSET) <= 6u * CC_MIB + TOD_PF_CHUNK_MAX && rd32(CC_PF + TOD_PF_END) == 63u * CC_MIB,
	      "Stratus City: blocks 0..5 (a filling one too) and 63 in, so 6 MiB to 63 MiB");
	cc_run(&cc, &as, 300, 1);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 1 && rd32(CC_PF + TOD_PF_OFFSET) >= 5u * CC_MIB &&
	      rd32(CC_PF + TOD_PF_END) == CC_F_END, "Fastoon: blocks 0..4 in, from 5 MiB");
	cc_run_out(&cc, &as, 400, 1);
	check(cc_st(&cc) == TOD_CC_DONE && cc_pct(&cc) != 0, "done");
	cc_blocks_all();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again, with every block of all three in");
	cc_run(&cc, &as, 12, 0);
	check(cc_st(&cc) == TOD_CC_DONE && cc_pct(&cc) == 0 && rd32(CC_PF + TOD_PF_CUR) == 0,
	      "done without a read: rate 0, nothing written");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(qstreq(text, "Already cached"), "the menu's row: Already cached");
	cc_block(150, TOD_FC_FREE, 30u << 20, 6);     /* Cobalia's block 30 free again */
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again, all but one Cobalia block in");
	cc_run(&cc, &as, 12, 0);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 3 && rd32(CC_PF + TOD_PF_OFFSET) == 30u * CC_MIB &&
	      rd32(CC_PF + TOD_PF_END) == 31u * CC_MIB, "only that block is read");
	cc_run_out(&cc, &as, 100, 1);
	check(cc_st(&cc) == TOD_CC_DONE && cc_pct(&cc) != 0, "and done, a rate said");
	wr32(TOD_FIOS_CACHE, 0);

	/* ---- cancelled: the title, a quit, a stall; a death and a pause are not */
	cc_warmer_idle();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 20, 4);
	check(cc_st(&cc) == TOD_CC_CACHING, "caching");
	as.cur_lua = 0;                               /* a death: Level_Exit, the level tier alone */
	as.cur_job = TOD_LOAD_JOB_LEVEL;
	as.cur_state = 3;
	cc_run(&cc, &as, 8, 0);
	cc_play(&as, 3, CC_LUA + 0x20u);
	cc_run(&cc, &as, 4, 4);
	check(cc_st(&cc) == TOD_CC_CACHING && rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA,
	      "a death (no load: g_levelLoadTier stays loaded) goes on");
	as.cur_state = TOD_STATE_PAUSE;
	wr8(CC_PF + TOD_PF_ENABLED, 0);
	cc_run(&cc, &as, TOD_CC_STALL_TICKS + 40u, 4);
	check(cc_st(&cc) == TOD_CC_CACHING, "paused far longer than the stall limit: no read, but no stall either");
	as.cur_state = TOD_STATE_GAMEPLAY;
	wr8(CC_PF + TOD_PF_ENABLED, 1);
	as.cur_front = 1;
	as.cur_lua = 0;
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_LEVEL, "the title mid-way: failed");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(qstreq(text, "Title came first"), "the menu's row says so");
	cc_play(&as, 3, CC_LUA);
	cc_warmer_idle();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 8, 1);
	tod_cc_quit(&cc);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_QUIT, "the game going: failed, the game closed");
	cc_warmer_idle();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 8, 0);            /* the range handed over, and no frame of the warmer runs */
	cc_run(&cc, &as, TOD_CC_STALL_TICKS + 8u, 0);
	check(cc_st(&cc) == TOD_CC_FAILED && cc_det(&cc) == TOD_CC_WHY_STALLED,
	      "in gameplay with the warmer on and no read for 30 s: the warmer stalled");
	wr32(CC_PF + TOD_PF_CUR, 0);
	wr32(CC_PF + TOD_PF_READY, 0);

	/* A request made at the title waits through the load into a level. */
	as.cur_front = 1;
	as.cur_lua = 0;
	check(tod_cc_trigger(&cc, &as) == ST_OK, "asked for at the title");
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && cc_det(&cc) == TOD_CC_WAIT_PLAY, "waiting for gameplay");
	as.cur_front = 0;
	cc_loading(&as, 0);
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_WAITING, "the load out of the title does not end it");
	cc_level_up(&as, 0, 2);
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_CACHING, "gameplay and the warmer on its slot: caching");
	cc_run_out(&cc, &as, 2000, 4);

	/* ---- Disable cutscene caching ends one, and refuses the button */
	cc_warmer_idle();
	check(tod_cc_trigger(&cc, &as) == ST_OK, "again");
	cc_run(&cc, &as, 8, 1);
	check(tod_cc_set(&cc, TF_NO_CUTSCENE_CACHE, 1) == ST_OK && cc_st(&cc) == TOD_CC_FAILED &&
	      cc_det(&cc) == TOD_CC_WHY_DISABLED, "Disable cutscene caching on: the run under way ends");
	check(tod_cc_trigger(&cc, &as) == ST_BUSY && cc_det(&cc) == TOD_CC_WHY_DISABLED,
	      "and the button is refused while it is on, saying why");
	tod_cc_menu_status(TF_CACHE_CUTSCENES, tod_cc_status(&cc), MENU_STATUS_ROW, text, sizeof(text));
	check(qstreq(text, "Caching disabled"), "the menu's row says so");
	cc_level_up(&as, 1, 2);
	cc_run(&cc, &as, 40, 1);
	check(rd32(CC_PF + TOD_PF_FILE) == CC_GAME_FILE(0) && cc_st(&cc) == TOD_CC_FAILED,
	      "and nothing goes ahead of the warmer while it is on");
	tod_cc_set(&cc, TF_NO_CUTSCENE_CACHE, 0);
	cc_run(&cc, &as, 200, 4);
	cc_warmer_idle();

	/* ---- the auto: a fresh file's Metropolis load, and nothing else */
	tod_cc_forget(&cc);
	memset(&as, 0, sizeof(as));
	cc_play(&as, 3, CC_LUA);
	cc_run(&cc, &as, 2, 0);
	check(tod_cc_set(&cc, TF_CACHE_ON_RESET, 1) == ST_OK, "Cache cutscenes on reset on");
	metropolis_beaten(0);
	cc_loading(&as, 3);
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_NONE, "a load into Fastoon, flag clear: nothing");
	cc_play(&as, 3, CC_LUA);
	cc_run(&cc, &as, 2, 0);
	metropolis_beaten(1);
	cc_loading(&as, TOD_LEVEL_METROPOLIS);
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_NONE, "a load into Metropolis on a save past it (flag set): nothing");
	cc_play(&as, TOD_LEVEL_METROPOLIS, CC_LUA);
	cc_run(&cc, &as, 4, 0);
	as.cur_lua = CC_LUA + 0x40u;               /* a death on Metropolis, flag clear: no load */
	metropolis_beaten(0);
	cc_run(&cc, &as, 8, 0);
	check(cc_st(&cc) == TOD_CC_NONE, "a death on Metropolis with the flag clear: nothing");
	cc_loading(&as, TOD_LEVEL_METROPOLIS);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_WAITING && cc_det(&cc) == TOD_CC_WAIT_PLAY,
	      "a fresh file's load into Metropolis: asked for at once, waiting for gameplay");
	cc_run(&cc, &as, 30, 0);
	check(cc_st(&cc) == TOD_CC_WAITING, "the run's own load does not end it");
	cc_reads_reset();
	cc_level_up(&as, TOD_LEVEL_METROPOLIS, 1);   /* Level_Enter: the warmer on Metropolis's slot 0 */
	cc_run(&cc, &as, 4, 0);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) == 3 && rd32(CC_PF + TOD_PF_FILE) == CC_COBALIA,
	      "Metropolis's first frames: Cobalia ahead of its warm-up");
	/* 26 s on Metropolis at 7.5 MiB/s, 256 KB a 30 Hz frame: about 195 MiB of reading, 182 of it ours. */
	cc_run_slow(&cc, &as, 10u * TOD_CC_TICK_HZ);
	check(cc_st(&cc) == TOD_CC_CACHING && cc_det(&cc) != 3, "Cobalia in within Metropolis's first 10 s at 7.5 MiB/s");
	cc_run_slow(&cc, &as, 16u * TOD_CC_TICK_HZ);
	check(cc_st(&cc) == TOD_CC_DONE, "all three in within Metropolis's 26 s at 7.5 MiB/s");
	check(cc_nseq >= 5 && cc_seq[1] == CC_COBALIA && cc_seq[2] == CC_STRATUS && cc_seq[3] == CC_FASTOON &&
	      cc_seq[4] == CC_GAME_FILE(0), "ahead of the whole of Metropolis's own warm-up");
	cc_loading(&as, TOD_LEVEL_COBALIA);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_DONE, "and a load after that changes nothing");
	cc_level_up(&as, TOD_LEVEL_COBALIA, 2);
	cc_run(&cc, &as, 8, 1);
	check(rd32(CC_PF + TOD_PF_FILE) == CC_GAME_FILE(0), "Cobalia's warmer left alone");
	metropolis_beaten(0);
	cc_loading(&as, TOD_LEVEL_METROPOLIS);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_WAITING, "a reset to a new file again: asked for again");
	tod_cc_set(&cc, TF_NO_CUTSCENE_CACHE, 1);
	check(cc_st(&cc) == TOD_CC_FAILED, "Disable cutscene caching on ends it");
	cc_play(&as, TOD_LEVEL_METROPOLIS, CC_LUA);
	cc_run(&cc, &as, 2, 0);
	cc_loading(&as, TOD_LEVEL_METROPOLIS);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_FAILED, "with Disable cutscene caching on, a fresh file's load asks for nothing");
	cc_play(&as, TOD_LEVEL_METROPOLIS, CC_LUA);
	cc_run(&cc, &as, 2, 0);
	tod_cc_set(&cc, TF_NO_CUTSCENE_CACHE, 0);
	tod_cc_set(&cc, TF_CACHE_ON_RESET, 0);
	cc_loading(&as, TOD_LEVEL_METROPOLIS);
	cc_run(&cc, &as, 2, 0);
	check(cc_st(&cc) == TOD_CC_FAILED, "and with the auto off, nothing either");
	metropolis_beaten(1);

	/* ---- Disable cutscene caching: the planet's sound blocks, during its load, in pieces */
	tod_cc_forget(&cc);
	memset(&as, 0, sizeof(as));
	cc_play(&as, 3, CC_LUA);
	cc_blocks();
	cc_run(&cc, &as, 2, 0);
	tod_cc_set(&cc, TF_NO_CUTSCENE_CACHE, 1);
	cc_run(&cc, &as, 20, 0);
	check(cc_use(0) == 1000u, "on, but no load: nothing is touched");
	cc_loading(&as, 2);                         /* into Stratus City */
	cc_run(&cc, &as, 1, 0);
	check(cc_use(0) == TOD_FC_FREE && cc_use(4) == TOD_FC_FREE && cc_use(66) == 1066u,
	      "the load's first tick: the first piece alone, its Stratus City sound blocks freed");
	cc_run(&cc, &as, 2, 0);
	check(cc_use(66) == TOD_FC_FREE, "the third tick, the last piece: its block 66 too");
	check(cc_use(5) == TOD_FC_FILLING && cc_use(6) == TOD_FC_FREE, "a filling block is left alone, a free one stays free");
	check(cc_use(7) == 1007u && cc_use(10) == 1010u && cc_use(20) == 1020u && cc_use(30) == 1030u && cc_use(45) == 1045u,
	      "Stratus City's textures, Fastoon's and Cobalia's sound, global_uncached and Metropolis's archive are left alone");
	for (i = 0, freed = 0; i < CC_NBLOCKS; i++) freed += cc_use(i) == TOD_FC_FREE;
	check(freed == 7, "seven blocks free: five freed, one already free, block 66 (and nothing else)");
	cc_play(&as, 2, CC_LUA);
	cc_run(&cc, &as, 4, 0);
	cc_blocks();
	cc_loading(&as, TOD_LEVEL_METROPOLIS);
	cc_run(&cc, &as, 4, 0);
	check(cc_use(30) == TOD_FC_FREE && cc_use(20) == TOD_FC_FREE && cc_use(21) == TOD_FC_FREE && cc_use(22) == 1022u,
	      "into Metropolis: its level_uncached, and global_uncached blocks 0..56 (its sound), not block 57");
	check(cc_use(0) == 1000u && cc_use(10) == 1010u && cc_use(45) == 1045u, "and no other planet's");
	cc_play(&as, TOD_LEVEL_METROPOLIS, CC_LUA);
	cc_run(&cc, &as, 4, 0);
	cc_blocks();
	cc_loading(&as, TOD_LEVEL_COBALIA);
	cc_run(&cc, &as, 4, 0);
	check(cc_use(45) == TOD_FC_FREE && cc_use(49) == TOD_FC_FREE && cc_use(0) == 1000u && cc_use(10) == 1010u,
	      "into Cobalia: its sound blocks, no other planet's");
	cc_play(&as, TOD_LEVEL_COBALIA, CC_LUA);
	cc_run(&cc, &as, 4, 0);
	cc_blocks();
	cc_block(40, 1040u, 0, 3);                  /* a Fastoon sound block in the second piece */
	cc_loading(&as, 3);                         /* into Fastoon, ending after one tick */
	cc_run(&cc, &as, 1, 0);
	cc_play(&as, 3, CC_LUA);
	cc_run(&cc, &as, 4, 0);
	check(cc_use(10) == TOD_FC_FREE && cc_use(14) == TOD_FC_FREE, "into Fastoon: its sound blocks in the first piece");
	check(cc_use(40) == 1040u, "the load over after one tick: the walk stops, nothing in gameplay is touched");
	tod_cc_set(&cc, TF_NO_CUTSCENE_CACHE, 0);
	cc_blocks();
	cc_loading(&as, 2);
	cc_run(&cc, &as, 4, 0);
	check(cc_use(0) == 1000u, "off: a load frees nothing (and what was freed refills through the warmer)");
	cc_play(&as, 2, CC_LUA);
	cc_run(&cc, &as, 2, 0);
	wr32(TOD_FIOS_CACHE, 0);

	/* ---- through the session: the feature, the readout */
	cc_toc();
	cc_warmer_idle();
	wr32(TOD_LUA_STATE, CC_LUA);
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LOAD_JOB, 0);
	wr8(TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED, 1);
	tick(4);
	check(features_trigger(TF_CACHE_CUTSCENES) == ST_OK, "FEATURE_TRIGGER 19 is taken");
	tick(12);
	session_info_copy(info, sizeof(info));
	info_word = be32_get(info + 64 + 4 * TOD_RO_CUTSCENE_CACHE);
	check((info_word >> 28) == TOD_CC_CACHING && ((info_word >> 24) & 15u) == 3 && rd32(CC_PF + TOD_PF_CUR) == CC_COBALIA,
	      "SessionInfo's readout 3 says caching Cobalia, and the warmer has its range");
	for (i = 0; i < 300; i++) {
		cc_frame();
		cc_frame();
		cc_frame();
		tick(1);
	}
	session_info_copy(info, sizeof(info));
	info_word = be32_get(info + 64 + 4 * TOD_RO_CUTSCENE_CACHE);
	check((info_word >> 28) == TOD_CC_DONE && ((info_word >> 16) & 0xFFu) != 0, "and done, with a read rate");
	check(features_set(TF_NO_CUTSCENE_CACHE, 1) == ST_OK && features_trigger(TF_CACHE_CUTSCENES) == ST_BUSY,
	      "with Disable cutscene caching on, the trigger is BUSY");
	check(features_set(TF_NO_CUTSCENE_CACHE, 0) == ST_OK && features_set(TF_CACHE_ON_RESET, 1) == ST_OK &&
	      features_set(TF_CACHE_ON_RESET, 0) == ST_OK, "both toggles answer OK through the feature table");
	wr32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, old_toc);
}

/* ------------------------------------------- Remove black screens, 2026-11-12 */

#define BS_LUA     0x30031000u   /* a lua_State of the test's own */
#define BS_ALPHA   (FAKE_ROOT + TOD_FADE_ALPHA)

/* The fade as ScreenFade leaves it: its progress, its target and the alpha it drew. */
static void bs_fade(f32 progress, f32 target, f32 alpha)
{
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, progress);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, target);
	wrf(BS_ALPHA, alpha);
}

static void bs_movie(u32 state)
{
	wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, state);
}

static int bs_fade_is(f32 progress, f32 target)
{
	return rdf(FAKE_ROOT + TOD_FADE_PROGRESS) == progress && rdf(FAKE_ROOT + TOD_FADE_TARGET) == target;
}

static void test_tod_black_screens(void)
{
	group("ToD: Remove black screens (24), the lead-in's black hold not drawn");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	wr32(TOD_LUA_STATE, 0);
	bs_movie(0);
	tick(2);
	check(features_set(TF_REMOVE_BLACK, 1) == ST_OK, "on");

	/* A level up: a still black that is not the movie player's (a load's or a death's fade) stays. */
	wr32(TOD_LUA_STATE, BS_LUA);
	bs_fade(1.0f, 1.0f, 1.0f);
	tick(6);
	check(rdf(BS_ALPHA) == 1.0f, "a still black with the movie player idle: left as it is");

	/* The lead-in: MoviePlayer_startFade(5), Start then Set(1.0), and state 1 while the autosave runs. */
	bs_movie(TOD_MP_PREROLL);
	tick(2);
	check(rd32(BS_ALPHA) == 0, "the lead-in's hold, black and still, soon after the level came up: drawn alpha 0");
	check(bs_fade_is(1.0f, 1.0f),
	      "its progress and target still 1.0, as MoviePlayer_update, the warmer and the autosplitter read them");
	wrf(BS_ALPHA, 1.0f);
	tick(2);
	check(rd32(BS_ALPHA) == 0, "the game's Set(1.0) again in the hold: cleared again");
	/* A timed fade (types 1 and 3): ScreenFade_update draws it every frame, so it is left until it is still. */
	bs_fade(0.6f, 1.0f, 0.65f);
	tick(2);
	check(rdf(BS_ALPHA) == 0.65f, "a fade still moving in the hold: left, the game redraws it every frame");
	bs_fade(1.0f, 1.0f, 1.0f);
	tick(2);
	check(rd32(BS_ALPHA) == 0, "and once it is still at black, cleared");

	/* The cinematic plays (state 2): its own fade, and nothing of qwark's. */
	bs_movie(2);
	bs_fade(1.0f, 1.0f, 1.0f);
	tick(4);
	check(rdf(BS_ALPHA) == 1.0f, "the hold over (state 2): the fade is the game's again");

	/* Later in the level: another cinematic's hold, long after the level came up, keeps its black. */
	bs_movie(0);
	tick((int)TOD_LEADIN_TICKS);
	bs_movie(TOD_MP_PREROLL);
	tick(4);
	check(rdf(BS_ALPHA) == 1.0f, "a cinematic's hold 15 s after the level came up: not a lead-in, left black");
	bs_movie(0);
	tick(2);

	/* A new lua_State (the next level, or a death's): its first cinematic's hold is a lead-in again. */
	wr32(TOD_LUA_STATE, BS_LUA + 0x100);
	tick(20);
	bs_movie(TOD_MP_PREROLL);
	tick(2);
	check(rd32(BS_ALPHA) == 0, "a new lua_State, a hold soon after: cleared");

	/* Off in the hold: black again; and no more after. */
	check(features_set(TF_REMOVE_BLACK, 0) == ST_OK && rdf(BS_ALPHA) == 1.0f && bs_fade_is(1.0f, 1.0f),
	      "off during the hold it cleared: black again at once, the progress untouched");
	bs_movie(0);
	wr32(TOD_LUA_STATE, BS_LUA + 0x200);
	tick(4);
	bs_movie(TOD_MP_PREROLL);
	tick(4);
	check(rdf(BS_ALPHA) == 1.0f, "off: a lead-in's hold stays black");

	/* On in the middle of that hold: the level counts as just up, so it clears at once. */
	check(features_set(TF_REMOVE_BLACK, 1) == ST_OK, "on again in the hold");
	tick(2);
	check(rd32(BS_ALPHA) == 0, "and the hold under way is cleared");

	/* No level object: nothing written anywhere. */
	bs_movie(0);
	tick(2);
	wr32(TOD_LEVEL_OBJ, 0);
	wr32(TOD_LUA_STATE, BS_LUA + 0x300);
	bs_fade(1.0f, 1.0f, 1.0f);
	tick(2);
	bs_movie(TOD_MP_PREROLL);
	tick(4);
	check(rdf(BS_ALPHA) == 1.0f, "no level object: nothing written");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	tick(2);
	check(rd32(BS_ALPHA) == 0, "the object back in the same hold: cleared");

	features_set(TF_REMOVE_BLACK, 0);
	bs_movie(0);
	bs_fade(0.0f, 0.0f, 0.0f);
	tick(2);
}

/* ------------------------------- positions with the camera, Die (2026-11-13) */

/* Player 0's follow camera, as backend_fake.c seeds it: the third slot (tod.h, TOD_CAM_*). */
#define CAM        (TOD_CAM_CLIENT + TOD_CAM_SLOTS + 2u * TOD_CAM_SLOT_LEN)
#define CAM_ST     (CAM + TOD_CAM_STATE)

static void put3(u32 addr, f32 x, f32 y, f32 z)
{
	wrf(addr, x);
	wrf(addr + 4, y);
	wrf(addr + 8, z);
}

static int is3(u32 addr, f32 x, f32 y, f32 z)
{
	return rdf(addr) == x && rdf(addr + 4) == y && rdf(addr + 8) == z;
}

/* The camera's whole state and position, to tell whether a load touched it. */
static void cam_snapshot(u8 out[0x50])
{
	host_peek(CAM_ST, out, 0x40);
	host_peek(CAM + TOD_CAM_POS, out + 0x40, 0x10);
}

static void test_tod_positions_camera(void)
{
	u8 blob[QWARK_MAX_BLOB], before[0x50], after[0x50];
	u8 len = 0, planet;
	int i;

	group("ToD: a position slot carries the follow camera's direction (2026-11-13)");
	planet = session_current_planet();
	/* The hero at (10, 20, 5), the camera looking at 1.5 above him from (3, -4, 2) off. */
	put3(FAKE_HERO + TOD_HERO_POS, 10.0f, 20.0f, 5.0f);
	put3(CAM_ST, 10.0f, 20.0f, 6.5f);
	put3(CAM_ST + TOD_CAM_OFFSET, 3.0f, -4.0f, 2.0f);
	put3(CAM_ST + TOD_CAM_DESIRED, 3.0f, -4.0f, 2.5f);
	check(session_position_save(3) == ST_OK && pos_fetch(planet, 3, blob, &len) == ST_OK && len == TOD_POS_SLOT_LEN,
	      "POS_SAVE keeps 64 bytes in the positions file");
	check(bef32_get(blob) == 10.0f && bef32_get(blob + 48) == 3.0f && bef32_get(blob + 52) == -4.0f &&
	      bef32_get(blob + 56) == 2.0f, "the hero's 48, then the camera's offset as it is on screen, not the desired one");

	/* Somewhere else, the camera turned round. */
	put3(FAKE_HERO + TOD_HERO_POS, 100.0f, 200.0f, 50.0f);
	put3(CAM_ST, 100.0f, 200.0f, 51.5f);
	put3(CAM_ST + TOD_CAM_OFFSET, -5.0f, 0.0f, 1.0f);
	put3(CAM_ST + TOD_CAM_DESIRED, -5.0f, 0.0f, 1.0f);
	put3(CAM_ST + TOD_CAM_DESIRED_POS, 95.0f, 200.0f, 52.5f);
	put3(CAM + TOD_CAM_POS, 95.0f, 200.0f, 52.5f);
	check(session_position_load(3) == ST_OK && is3(FAKE_HERO + TOD_HERO_POS, 10.0f, 20.0f, 5.0f),
	      "POS_LOAD puts the hero back");
	check(is3(CAM_ST + TOD_CAM_OFFSET, 3.0f, -4.0f, 2.0f) && is3(CAM_ST + TOD_CAM_DESIRED, 3.0f, -4.0f, 2.0f),
	      "and the saved offset as both the offset and the desired one, so the next frame keeps it");
	check(is3(CAM_ST, 10.0f, 20.0f, 6.5f), "the target moved by the hero's own jump, still 1.5 above him");
	check(is3(CAM_ST + TOD_CAM_DESIRED_POS, 13.0f, 16.0f, 8.5f) && is3(CAM + TOD_CAM_POS, 13.0f, 16.0f, 8.5f),
	      "and the camera's two positions, target + offset");

	/* A slot saved before the camera: 48 bytes, the hero alone. */
	memset(blob, 0, sizeof(blob));
	bef32_put(blob, 7.0f);
	bef32_put(blob + 4, 8.0f);
	bef32_put(blob + 8, 9.0f);
	check(pos_store(planet, 4, blob, TOD_POS_BLOB_LEN) == ST_OK, "an old 48-byte slot in the file");
	cam_snapshot(before);
	check(session_position_load(4) == ST_OK && is3(FAKE_HERO + TOD_HERO_POS, 7.0f, 8.0f, 9.0f),
	      "it still loads the hero");
	cam_snapshot(after);
	check(memcmp(before, after, sizeof(before)) == 0, "and leaves the camera exactly as it was");
	/* A 48-byte slot read into a buffer that still holds an earlier slot's camera past its end. */
	memset(blob, 0, sizeof(blob));
	bef32_put(blob, 7.5f);
	bef32_put(blob + 48, -9.0f);
	bef32_put(blob + 52, 1.0f);
	bef32_put(blob + 56, 3.0f);
	check(session_game()->load_blob(blob, TOD_POS_BLOB_LEN) == ST_OK && rdf(FAKE_HERO + TOD_HERO_POS) == 7.5f,
	      "a 48-byte blob with a camera's bytes after it loads the hero");
	cam_snapshot(after);
	check(memcmp(before, after, sizeof(before)) == 0, "and never reads the bytes past its length as a camera");

	/* A camera that is not following the hero: the slot is the hero's 48 bytes. */
	put3(CAM_ST, 900.0f, 20.0f, 6.5f);
	check(session_position_save(5) == ST_OK && pos_fetch(planet, 5, blob, &len) == ST_OK && len == TOD_POS_BLOB_LEN,
	      "a follow camera 890 units off is not this hero's: a 48-byte slot");
	check(session_position_load(3) == ST_OK && is3(FAKE_HERO + TOD_HERO_POS, 10.0f, 20.0f, 5.0f) &&
	      rdf(CAM_ST) == 900.0f && rdf(CAM_ST + TOD_CAM_OFFSET) == 3.0f,
	      "and a 64-byte slot loaded then moves the hero and leaves that camera alone");
	put3(CAM_ST, 10.0f, 20.0f, 6.5f);

	/* No follow camera at all (the slot's type another): 48 bytes. */
	wr32(CAM + TOD_CAM_TYPE, 6);
	check(session_position_save(5) == ST_OK && pos_fetch(planet, 5, blob, &len) == ST_OK && len == TOD_POS_BLOB_LEN,
	      "no slot of type 3: a 48-byte slot");
	wr32(CAM + TOD_CAM_TYPE, TOD_CAM_FOLLOW);

	/* An imported 64-byte blob whose offset is no camera's: the hero, and the camera kept. */
	check(session_position_save(6) == ST_OK && pos_fetch(planet, 6, blob, &len) == ST_OK && len == TOD_POS_SLOT_LEN,
	      "a 64-byte slot again");
	be32_put(blob + 52, 0x7FC00000u);   /* NaN */
	bef32_put(blob, 1.0f);
	check(session_position_store(6, blob, TOD_POS_SLOT_LEN) == ST_OK, "POS_STORE takes 64 bytes");
	cam_snapshot(before);
	check(session_position_load(6) == ST_OK && rdf(FAKE_HERO + TOD_HERO_POS) == 1.0f, "a NaN offset still loads the hero");
	cam_snapshot(after);
	check(memcmp(before, after, sizeof(before)) == 0, "and leaves the camera alone");
	for (i = 48; i < 64; i++) blob[i] = 0;
	check(session_position_store(6, blob, TOD_POS_SLOT_LEN) == ST_OK && session_position_load(6) == ST_OK &&
	      memcmp(before, (cam_snapshot(after), after), sizeof(before)) == 0,
	      "a zero offset, on top of its target, is no camera's either");
	/* POS_STORE of an older ToD slot, 48 bytes: stored as it is, and loaded as the hero alone. */
	memset(blob, 0, sizeof(blob));
	bef32_put(blob, 21.0f);
	bef32_put(blob + 4, 22.0f);
	bef32_put(blob + 8, 23.0f);
	bef32_put(blob + 48, -9.0f);   /* bytes past the 48, never stored */
	bef32_put(blob + 56, 3.0f);
	check(session_position_store(6, blob, TOD_POS_BLOB_LEN) == ST_OK && pos_fetch(planet, 6, blob, &len) == ST_OK &&
	      len == TOD_POS_BLOB_LEN && bef32_get(blob) == 21.0f,
	      "POS_STORE takes a 48-byte slot, a slot without the camera, and keeps it at 48");
	cam_snapshot(before);
	check(session_position_load(6) == ST_OK && is3(FAKE_HERO + TOD_HERO_POS, 21.0f, 22.0f, 23.0f),
	      "and POS_LOAD puts the hero there");
	cam_snapshot(after);
	check(memcmp(before, after, sizeof(before)) == 0, "with the camera untouched");
	check(session_position_store(6, blob, 30) == ST_BAD_ARG && session_position_store(6, blob, 47) == ST_BAD_ARG &&
	      session_position_store(6, blob, 49) == ST_BAD_ARG && session_position_store(6, blob, 63) == ST_BAD_ARG &&
	      session_position_store(6, blob, 40) == ST_BAD_ARG,
	      "any other length is refused: 30, 40 (the four games'), 47, 49, 63");
	check(pos_fetch(planet, 6, blob, &len) == ST_OK && len == TOD_POS_BLOB_LEN && bef32_get(blob) == 21.0f,
	      "and leaves the slot as it was");
	pos_clear(planet, 3);
	pos_clear(planet, 4);
	pos_clear(planet, 5);
	pos_clear(planet, 6);
}

/* The play Die wants: in gameplay, nothing pending, no movie, the fade clear. */
static void die_world(void)
{
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	wr32(FAKE_ROOT + TOD_HERO_PTR, FAKE_HERO);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_REQUESTED_STATE, TOD_REQUESTED_NONE);
	wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, 0);
	wr32(TOD_RESPAWN_FADE, 0);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS + 4, 0.5f);
	wrf(FAKE_ROOT + TOD_FADE_TARGET, 0.0f);
	wrf(FAKE_ROOT + TOD_FADE_ALPHA, 0.0f);
	wrf(FAKE_HERO + TOD_HERO_HEALTH, 10.0f);
}

/* What Die asks the game for: Hero_respawnWhenBlack's two conditions, and nothing of a death's. */
static int respawn_asked(void)
{
	return rd32(TOD_RESPAWN_FADE) == 1 && rdf(FAKE_ROOT + TOD_FADE_PROGRESS) >= 1.0f &&
	       rdf(FAKE_ROOT + TOD_FADE_TARGET) == 1.0f && rdf(FAKE_ROOT + TOD_FADE_PROGRESS + 4) == 0.0f &&
	       rdf(FAKE_ROOT + TOD_FADE_ALPHA) == 1.0f;
}

/*
 * The game's side, as its executable does it: Hero_respawnWhenBlack sees the
 * state 1 and the fade black and calls GameState_Request(3, 1), the state
 * back to 0; GameState_Update then runs Level_Exit (the lua_State 0) with
 * the loader on state 3's tiers while g_levelLoadTier stays loaded, and
 * Level_Enter (a new hero's hit points, the new lua_State, black and its
 * fade in). Returns 0 when there was no respawn to make.
 */
static int game_respawns(u32 lua)
{
	if (rd32(TOD_RESPAWN_FADE) != 1 || rdf(FAKE_ROOT + TOD_FADE_PROGRESS) < 1.0f) return 0;
	wr32(TOD_RESPAWN_FADE, 0);
	wr32(TOD_LUA_STATE, 0);
	wr32(TOD_LOAD_JOB, TOD_LOAD_JOB_LEVEL);
	tick(4);
	wrf(FAKE_HERO + TOD_HERO_HEALTH, 10.0f);
	level_enters(lua);
	tick(4);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.4f);
	tick(2);
	wrf(FAKE_ROOT + TOD_FADE_PROGRESS, 0.0f);
	tick(2);
	return 1;
}

static void test_tod_die(void)
{
	u32 seq, arg = 0;

	group("ToD: Die is the game's own respawn, no death animation (2026-11-13)");
	die_world();
	check(session_die() == ST_OK && respawn_asked(), "Die: the fade at black and g_heroRespawnFadeState 1");
	check(rdf(FAKE_HERO + TOD_HERO_HEALTH) == 10.0f && rdf(FAKE_HERO + TOD_HERO_MAX_HEALTH) == 10.0f,
	      "and the hit points not zeroed: no death, no death animation");
	check(rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu && rd32(TOD_REQUESTED_STATE) == TOD_REQUESTED_NONE,
	      "no planet load and no state written from qwark's thread: the game asks for the respawn itself");
	check(features_trigger(TF_DIE) == ST_OK && respawn_asked(), "the Die button is the same");

	die_world();
	wr32(TOD_LOAD_REQUEST, 4);
	check(session_die() == ST_BUSY && rd32(TOD_RESPAWN_FADE) == 0, "not over a planet load already asked for");
	die_world();
	wr32(TOD_REQUESTED_STATE, 3);
	check(session_die() == ST_BUSY && rd32(TOD_RESPAWN_FADE) == 0, "nor over a state change pending");
	die_world();
	wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, 2);
	check(session_die() == ST_BUSY && rd32(TOD_RESPAWN_FADE) == 0 && rdf(FAKE_ROOT + TOD_FADE_PROGRESS) == 0.0f,
	      "nor while a cinematic plays, as Hero_beginRespawnFade refuses");
	die_world();
	wr32(TOD_GAME_STATE, 3);
	check(session_die() == ST_BUSY && rd32(TOD_RESPAWN_FADE) == 0, "nor outside gameplay and the pause");

	/* From the pause menu: kept until play comes back. */
	die_world();
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	check(session_die() == ST_OK && rd32(TOD_RESPAWN_FADE) == 0 && rdf(FAKE_ROOT + TOD_FADE_ALPHA) == 0.0f,
	      "Die in the pause writes nothing while the pause is up");
	tick(30);
	check(rd32(TOD_RESPAWN_FADE) == 0, "however long it stays up");
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(2);
	check(respawn_asked(), "and the respawn is asked for as play comes back");
	die_world();
	tick(4);
	check(rd32(TOD_RESPAWN_FADE) == 0, "once");
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	session_die();
	wr32(TOD_GAME_STATE, 3);   /* a quit or a load from the pause */
	tick(2);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(4);
	check(rd32(TOD_RESPAWN_FADE) == 0, "a Die from a pause that left for anything but play is dropped");
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	session_die();
	wr32(FAKE_ROOT + TOD_HERO_PTR, FAKE_HERO + 0x4000);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	tick(4);
	check(rd32(TOD_RESPAWN_FADE) == 0, "and so is one whose hero is gone");
	wr32(FAKE_ROOT + TOD_HERO_PTR, FAKE_HERO);

	group("ToD: the autosplitter takes Die's respawn for no load and no run (2026-11-13)");
	/* A fresh file's Metropolis, where a load would be a run's RESET. */
	die_world();
	wr32(TOD_CURRENT_LEVEL, TOD_LEVEL_METROPOLIS);
	wr32(TOD_LOAD_JOB, 0);
	wr8(AS_TIER, 1);
	wr8(TOD_FRONT_END, 0);
	wr32(TOD_LOAD_SCREEN, 1);
	wr32(TOD_LUA_STATE, AS_STATE);
	metropolis_beaten(0);
	tick(8);
	seq = autosplit_latest_seq();
	check(session_die() == ST_OK, "Die on a fresh Metropolis");
	tick(2);
	check(game_respawns(AS_STATE + 0x200), "the game makes the respawn it asked for");
	tick(8);
	check(as_all(seq) == 0, "and the autosplitter emits nothing: no RESET, START, split or long load");
	check(rd8(AS_TIER) == 1, "(g_levelLoadTier stayed loaded, which is what tells a respawn from a load)");

	/* A played Cobalia, then on to another planet: the origin is kept through the respawn. */
	metropolis_beaten(1);
	wr32(TOD_CURRENT_LEVEL, 1);
	planet_load(1, AS_STATE);
	tick(4);
	seq = autosplit_latest_seq();
	die_world();
	check(session_die() == ST_OK && game_respawns(AS_STATE + 0x240), "Die on Cobalia, the respawn made");
	tick(4);
	check(as_all(seq) == 0, "nothing emitted");
	planet_load(9, AS_STATE + 0x280);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 9 && as_all(seq) == 1,
	      "and the next trip splits once, from Cobalia, as after any death");
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(4);
}

/*
 * 2026-11-14: the root cause of "loading a position never restores the camera".
 * With Game Options' Lock-Strafe on (g_saveData.lockStrafe 1), the camera
 * manager's default camera and Hero_setStrafeMode's are type 26, the
 * lock-strafe camera, not the follow camera: the client's active slot is a
 * ToDStrafeCam and no slot has type 3 at all, and its +0xD0 is its unit look
 * direction. Until now only client 0's type-3 slot was looked for, so every
 * slot saved there was 48 bytes and no load ever turned the camera. Here the
 * hero's client is client 1, as CamMgr_allocClient hands a player any free one.
 */
#define STRAFE_CLIENT  (TOD_CAM_CLIENT + 1u * TOD_CAM_CLIENT_LEN)
#define STRAFE         (STRAFE_CLIENT + TOD_CAM_SLOTS + 5u * TOD_CAM_SLOT_LEN)
#define STRAFE_ST      (STRAFE + TOD_CAM_STATE)

static void test_tod_strafe_camera(void)
{
	u8 blob[QWARK_MAX_BLOB], up[16], up_after[16];
	u8 len = 0, planet;
	u32 cam_type = rd32(CAM + TOD_CAM_TYPE), active0 = rd32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE);

	group("ToD: a position slot carries the lock-strafe camera's direction (2026-11-14, the root cause)");
	planet = session_current_planet();
	/* Lock-Strafe on: client 0 has no camera any more, client 1's active slot is the lock-strafe camera. */
	wr32(TOD_LOCK_STRAFE, 1);
	wr32(CAM + TOD_CAM_TYPE, 0xFFFFFFFFu);
	wr32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE, 0);
	wr32(STRAFE + TOD_CAM_TYPE, TOD_CAM_STRAFE);
	wr32(STRAFE_CLIENT + TOD_CAM_ACTIVE, STRAFE);
	/* The hero at (10, 20, 5), the camera looking along (0.6, 0.8, 0) at a point 1.5 above him. */
	put3(FAKE_HERO + TOD_HERO_POS, 10.0f, 20.0f, 5.0f);
	put3(STRAFE_ST, 10.0f, 20.0f, 6.5f);
	put3(STRAFE_ST + TOD_CAM_OFFSET, 0.6f, 0.8f, 0.0f);
	put3(STRAFE_ST + TOD_CAM_DESIRED, 0.0f, 0.0f, 1.0f);   /* its up vector */
	check(session_position_save(3) == ST_OK && pos_fetch(planet, 3, blob, &len) == ST_OK && len == TOD_POS_SLOT_LEN,
	      "POS_SAVE with Lock-Strafe on keeps 64 bytes: the camera is found (it was a 48-byte slot)");
	check(bef32_get(blob + 48) == -0.6f * TOD_CAM_STRAFE_DIST && bef32_get(blob + 52) == -0.8f * TOD_CAM_STRAFE_DIST &&
	      bef32_get(blob + 56) == 0.0f, "the offset where the lock-strafe camera puts itself: 5.5 behind, along its look");

	/* Elsewhere, the camera turned to look the other way. */
	put3(FAKE_HERO + TOD_HERO_POS, 100.0f, 200.0f, 50.0f);
	put3(STRAFE_ST, 100.0f, 200.0f, 51.5f);
	put3(STRAFE_ST + TOD_CAM_OFFSET, -1.0f, 0.0f, 0.0f);
	put3(STRAFE + TOD_CAM_POS, 105.5f, 200.0f, 51.5f);
	host_peek(STRAFE_ST + TOD_CAM_DESIRED, up, sizeof(up));
	check(session_position_load(3) == ST_OK && is3(FAKE_HERO + TOD_HERO_POS, 10.0f, 20.0f, 5.0f),
	      "POS_LOAD puts the hero back");
	check(rdf(STRAFE_ST + TOD_CAM_OFFSET) > 0.5999f && rdf(STRAFE_ST + TOD_CAM_OFFSET) < 0.6001f &&
	      rdf(STRAFE_ST + TOD_CAM_OFFSET + 4) > 0.7999f && rdf(STRAFE_ST + TOD_CAM_OFFSET + 4) < 0.8001f &&
	      rdf(STRAFE_ST + TOD_CAM_OFFSET + 8) == 0.0f,
	      "and the lock-strafe camera looks where it looked when saved: its unit direction back");
	check(is3(STRAFE_ST, 10.0f, 20.0f, 6.5f), "its target moved by the hero's jump");
	check(rdf(STRAFE + TOD_CAM_POS) > 6.69f && rdf(STRAFE + TOD_CAM_POS) < 6.71f &&
	      rdf(STRAFE + TOD_CAM_POS + 4) > 15.59f && rdf(STRAFE + TOD_CAM_POS + 4) < 15.61f,
	      "and its position 5.5 behind the target, nothing swinging in from the old place");
	host_peek(STRAFE_ST + TOD_CAM_DESIRED, up_after, sizeof(up_after));
	check(memcmp(up, up_after, sizeof(up)) == 0, "its up vector (+0xE0) is the game's");

	/* A slot saved with the follow camera loads into the lock-strafe camera: the same direction. */
	memset(blob, 0, sizeof(blob));
	bef32_put(blob, 10.0f);
	bef32_put(blob + 4, 20.0f);
	bef32_put(blob + 8, 5.0f);
	bef32_put(blob + 48, 0.0f);
	bef32_put(blob + 52, -3.0f);
	bef32_put(blob + 56, 0.0f);
	check(session_position_store(4, blob, TOD_POS_SLOT_LEN) == ST_OK && session_position_load(4) == ST_OK &&
	      is3(STRAFE_ST + TOD_CAM_OFFSET, 0.0f, 1.0f, 0.0f),
	      "a follow camera's slot (offset 0, -3, 0) turns the lock-strafe camera to look along +y");

	/* The camera client 1 uses is another type (a cut's blend): its slots are looked through. */
	wr32(STRAFE_CLIENT + TOD_CAM_ACTIVE, STRAFE_CLIENT + TOD_CAM_SLOTS);
	wr32(STRAFE_CLIENT + TOD_CAM_SLOTS + TOD_CAM_TYPE, 1);
	check(session_position_save(5) == ST_OK && pos_fetch(planet, 5, blob, &len) == ST_OK && len == TOD_POS_SLOT_LEN,
	      "a blend camera in use for a few frames: the lock-strafe slot is still found");
	/* A direction that is no unit vector is no lock-strafe camera's. */
	put3(STRAFE_ST + TOD_CAM_OFFSET, 0.0f, 0.0f, 0.0f);
	check(session_position_save(5) == ST_OK && pos_fetch(planet, 5, blob, &len) == ST_OK && len == TOD_POS_BLOB_LEN,
	      "a zero direction: a 48-byte slot");

	/* Back: Lock-Strafe off, client 0's follow camera, client 1 unused. */
	wr32(TOD_LOCK_STRAFE, 0);
	wr32(STRAFE + TOD_CAM_TYPE, 0);
	wr32(STRAFE_CLIENT + TOD_CAM_ACTIVE, 0);
	wr32(STRAFE_CLIENT + TOD_CAM_SLOTS + TOD_CAM_TYPE, 0);
	wr32(CAM + TOD_CAM_TYPE, cam_type);
	wr32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE, active0);
	pos_clear(planet, 3);
	pos_clear(planet, 4);
	pos_clear(planet, 5);
}

/*
 * Revision 1.34: a position is saved wherever the hero is, the HUD's copy of
 * his pointer ([root] + TOD_HERO_PTR) 0 or not. A fresh level object has it 0
 * until the HUD's first update (tod.h), through an opening cinematic, the
 * movie player's pre-roll that waits for the arrival autosave, a pause
 * opened there, or a load still running: the hero is then the game's own
 * current one, g_heroList's slot through g_mobyPool. Until now every POS_SAVE
 * and POS_LOAD there was BUSY. Nothing stops them but no hero.
 */
static void test_tod_positions_no_hud_hero(void)
{
	static const struct { u32 state, movie, saving, pause, job; const char *what; } when[] = {
		{ TOD_STATE_GAMEPLAY, TOD_MP_PREROLL, 2, 0, 0,                  "the arrival autosave, the movie player's pre-roll waiting on it" },
		{ TOD_STATE_PAUSE,    0,              1, 3, 0,                  "the pause" },
		{ TOD_STATE_CINEMATIC, 2,             1, 0, 0,                  "a realtime cinematic" },
		{ 3,                  0,              1, 0, TOD_LOAD_JOB_LEVEL, "a level load, the tier loader on the level's tiers" }
	};
	u8 blob[QWARK_MAX_BLOB], before[0x50], after[0x50];
	u8 len = 0, planet;
	u32 mobys = rd32(TOD_MOBY_SLOTS), moby = mobys + 2u * TOD_MOBY_STRIDE, handle = rd32(moby + TOD_MOBY_HANDLE);
	u32 state0 = rd32(TOD_GAME_STATE), movie0 = rd32(TOD_MOVIE_PLAYER + TOD_MP_STATE);
	u32 sm0 = rd32(TOD_SAVE_MANAGER + TOD_SM_STATE), pause0 = rd32(TOD_PAUSE_STATE), job0 = rd32(TOD_LOAD_JOB);
	u32 obj0 = rd32(moby + TOD_MOBY_OBJECT), back0 = rd32(FAKE_HERO + TOD_HERO_MOBY);
	u8 moby_pos0[16];
	u32 type0 = rd32(CAM + TOD_CAM_TYPE), active0 = rd32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE);
	char what[192];
	unsigned i;

	group("ToD: a position saves wherever the hero is, the HUD's copy of him or not (revision 1.34)");
	planet = session_current_planet();
	host_peek(moby + TOD_MOBY_POS, moby_pos0, sizeof(moby_pos0));
	check(handle == 0x00030002u, "the simulator's moby pool has slot 2 live");
	/* Ratchet, slot 0 of g_heroList, the moby in g_mobyPool's slot 2, and his object's moby that slot. */
	wr32(TOD_HERO_LIST + TOD_HL_CURRENT, 0);
	wr32(TOD_HERO_LIST + TOD_HL_HANDLES, 0);
	wr32(TOD_HERO_LIST + TOD_HL_HANDLES + 4, handle);
	wr32(moby + TOD_MOBY_OBJECT, FAKE_HERO);
	wr32(FAKE_HERO + TOD_HERO_MOBY, moby);
	put3(CAM_ST, 10.0f, 20.0f, 6.5f);
	put3(CAM_ST + TOD_CAM_OFFSET, 3.0f, -4.0f, 2.0f);

	for (i = 0; i < sizeof(when) / sizeof(when[0]); i++) {
		/* A fresh level object: BigObj5B8F0_Init cleared it, the HUD has not run. */
		wr32(FAKE_ROOT + TOD_HERO_PTR, 0);
		wr32(TOD_GAME_STATE, when[i].state);
		wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, when[i].movie);
		wr32(TOD_SAVE_MANAGER + TOD_SM_STATE, when[i].saving);
		wr32(TOD_PAUSE_STATE, when[i].pause);
		wr32(TOD_LOAD_JOB, when[i].job);
		put3(FAKE_HERO + TOD_HERO_POS, 10.0f + (f32)i, 20.0f, 5.0f);
		put3(CAM_ST, 10.0f + (f32)i, 20.0f, 6.5f);
		pos_clear(planet, 3);
		snprintf(what, sizeof(what), "POS_SAVE in %s, the HUD's copy 0: the hero's 64 bytes", when[i].what);
		check(session_position_save(3) == ST_OK && pos_fetch(planet, 3, blob, &len) == ST_OK && len == TOD_POS_SLOT_LEN &&
		      bef32_get(blob) == 10.0f + (f32)i && bef32_get(blob + 48) == 3.0f, what);
		put3(FAKE_HERO + TOD_HERO_POS, 500.0f, 500.0f, 500.0f);
		put3(CAM_ST, 500.0f, 500.0f, 501.5f);
		snprintf(what, sizeof(what), "and POS_LOAD in %s puts him and his moby back", when[i].what);
		check(session_position_load(3) == ST_OK && is3(FAKE_HERO + TOD_HERO_POS, 10.0f + (f32)i, 20.0f, 5.0f) &&
		      is3(moby + TOD_MOBY_POS, 10.0f + (f32)i, 20.0f, 5.0f), what);
	}

	/* No camera following him yet (an opening cinematic's own): the 48-byte slot, never a refusal. */
	wr32(CAM + TOD_CAM_TYPE, 6);
	wr32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE, 0);
	pos_clear(planet, 3);
	check(session_position_save(3) == ST_OK && pos_fetch(planet, 3, blob, &len) == ST_OK && len == TOD_POS_BLOB_LEN &&
	      bef32_get(blob) == 13.0f, "no camera of the hero's: POS_SAVE keeps the hero's 48 bytes");
	wr32(CAM + TOD_CAM_TYPE, type0);
	wr32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE, active0);

	/* No hero: each of these is BUSY, stores nothing and moves nothing. */
	pos_clear(planet, 3);
	pos_clear(planet, 4);
	put3(FAKE_HERO + TOD_HERO_POS, 1.0f, 2.0f, 3.0f);
	memset(blob, 0, sizeof(blob));
	bef32_put(blob, 77.0f);
	check(pos_store(planet, 4, blob, TOD_POS_BLOB_LEN) == ST_OK, "a slot to try loading");
	cam_snapshot(before);
	wr32(TOD_HERO_LIST + TOD_HL_CURRENT, TOD_HERO_SLOT_NONE);
	check(session_position_save(3) == ST_BUSY && pos_fetch(planet, 3, blob, &len) == ST_NOT_FOUND,
	      "no current hero (g_heroList's slot -1): POS_SAVE is BUSY and stores nothing");
	check(session_position_load(4) == ST_BUSY && is3(FAKE_HERO + TOD_HERO_POS, 1.0f, 2.0f, 3.0f),
	      "and POS_LOAD is BUSY and writes nothing");
	wr32(TOD_HERO_LIST + TOD_HL_CURRENT, 0);
	wr32(moby + TOD_MOBY_HANDLE, 0xFFFE0002u);
	check(session_position_save(3) == ST_BUSY && session_position_load(4) == ST_BUSY &&
	      is3(FAKE_HERO + TOD_HERO_POS, 1.0f, 2.0f, 3.0f),
	      "his moby freed (its slot's handle 0xFFFE0002): BUSY both ways");
	wr32(moby + TOD_MOBY_HANDLE, handle);
	wr32(TOD_HERO_LIST + TOD_HL_HANDLES + 4, 0x00070002u);
	check(session_position_save(3) == ST_BUSY, "a handle of another serial (a moby of an earlier level): BUSY");
	wr32(TOD_HERO_LIST + TOD_HL_HANDLES + 4, 0x0011000Fu);
	check(session_position_save(3) == ST_BUSY, "a handle past the pool's highest index in use: BUSY");
	wr32(TOD_HERO_LIST + TOD_HL_HANDLES + 4, handle);
	wr32(FAKE_HERO + TOD_HERO_MOBY, FAKE_MOBY);
	check(session_position_save(3) == ST_BUSY, "an object whose own moby is another (no hero's): BUSY");
	wr32(FAKE_HERO + TOD_HERO_MOBY, moby);
	wr32(TOD_LEVEL_OBJ, 0);
	check(session_position_save(3) == ST_BUSY && session_position_load(4) == ST_BUSY &&
	      is3(FAKE_HERO + TOD_HERO_POS, 1.0f, 2.0f, 3.0f),
	      "no level object (between levels), whatever the hero list says: BUSY both ways");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	cam_snapshot(after);
	check(memcmp(before, after, sizeof(before)) == 0 && pos_fetch(planet, 3, blob, &len) == ST_NOT_FOUND,
	      "and none of them touched the camera or filled the slot");

	/* The HUD's copy when it has one: the hero it names, as before. */
	wr32(FAKE_ROOT + TOD_HERO_PTR, FAKE_HERO);
	wr32(TOD_HERO_LIST + TOD_HL_CURRENT, TOD_HERO_SLOT_NONE);
	check(session_position_save(3) == ST_OK && pos_fetch(planet, 3, blob, &len) == ST_OK && bef32_get(blob) == 1.0f,
	      "with the HUD's copy set, it is the hero, as it always was");

	/* Back as the simulator seeds it. */
	wr32(TOD_HERO_LIST + TOD_HL_CURRENT, 0);
	wr32(TOD_HERO_LIST + TOD_HL_HANDLES + 4, 0);
	wr32(moby + TOD_MOBY_OBJECT, obj0);
	wr32(FAKE_HERO + TOD_HERO_MOBY, back0);
	host_poke(moby + TOD_MOBY_POS, moby_pos0, sizeof(moby_pos0));
	wr32(TOD_GAME_STATE, state0);
	wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, movie0);
	wr32(TOD_SAVE_MANAGER + TOD_SM_STATE, sm0);
	wr32(TOD_PAUSE_STATE, pause0);
	wr32(TOD_LOAD_JOB, job0);
	put3(CAM_ST, 10.0f, 20.0f, 6.5f);
	pos_clear(planet, 3);
	pos_clear(planet, 4);
}

/*
 * 2026-11-14: PLANET_LOAD in the pause and in a realtime cinematic loads at
 * once, as LoadLevel does there (the pause menu's own Load); the galactic map,
 * an FMV and a pause still being built stay BUSY. The autosplitter sees the
 * load as any planet load: one split, no RESET, no long load.
 */
static int nothing_asked(void)
{
	return rd32(TOD_REQUESTED_STATE) == TOD_REQUESTED_NONE && rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu && tiers_dying() == 0;
}

static void test_tod_pause_loads(void)
{
	u32 seq, arg = 0, old;

	group("ToD: a planet load from the pause or a cinematic is the game's own load there (2026-11-14)");
	die_world();
	metropolis_beaten(1);
	wr32(TOD_CURRENT_LEVEL, 1);
	planet_load(1, AS_STATE);
	tick(4);

	/* The pause up, no Trainer glue in it (RPCS3 without the helper's patch, or the menu off). */
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_UP);
	tick(4);
	seq = autosplit_latest_seq();
	old = rd32(TOD_LEVEL_NAME);
	check(session_planet_load(9, 0) == ST_OK, "PLANET_LOAD in the pause is accepted (it was BUSY)");
	check(rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu && rd32(TOD_REQUESTED_STATE) == TOD_LEVEL_LOAD_STATE &&
	      tiers_dying() == 3 && level_names_at(9) && rd32(TOD_PREV_LEVEL_NAME) == old,
	      "as LoadLevel in the pause: no request only play would take, the name set, the top three tiers "
	      "marked and g_requestedGameState 3, which the game's next frame acts on");
	check(rd32(TOD_LOAD_FADE_TIME) == TOD_LOAD_FADE_GAME, "no fade armed: none plays on this path");
	check(fl_rd8(FL_FLAG) == 1 && rd32(TOD_TRANSITION_MIN) == 0, "Fast loads forced for it, as for every load qwark starts");
	check(session_planet_load(3, 0) == ST_BUSY && rd32(TOD_CURRENT_LEVEL) == 9,
	      "and never a second one over it");
	tick(4);
	check(as_all(seq) == 0, "the autosplitter emits nothing for the pause or the request");
	/* The game takes it: the pause and the level go, the tiers load, the new level comes up in play. */
	load_now_taken();
	load_begins();
	tick(10);
	load_data_in();
	tick(2);
	wr32(TOD_PAUSE_STATE, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	level_enters(AS_STATE + 0x300);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 9 && as_all(seq) == 1,
	      "the autosplitter: one planet split, to Rykan V, no RESET, START or long load");
	check(fl_rd8(FL_FLAG) == 0 && rd32(TOD_TRANSITION_MIN) == TOD_TRANSITION_GAME,
	      "and Fast loads put back once the new level is up (the flag, and the game's 5 s transition)");

	/* A realtime cinematic: the same; its own exit (Movie_stopAll) is what the unload runs. */
	tick(4);
	seq = autosplit_latest_seq();
	wr32(TOD_GAME_STATE, TOD_STATE_CINEMATIC);
	wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, 2);
	check(session_planet_load(5, 0) == ST_OK && rd32(TOD_REQUESTED_STATE) == TOD_LEVEL_LOAD_STATE &&
	      tiers_dying() == 3 && level_names_at(5) && rd32(TOD_LOAD_REQUEST) == 0xFFFFFFFFu,
	      "PLANET_LOAD during a cinematic loads at once (it was BUSY)");
	load_now_taken();
	load_begins();
	tick(10);
	load_data_in();
	tick(2);
	wr32(TOD_MOVIE_PLAYER + TOD_MP_STATE, 0);
	wr32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	level_enters(AS_STATE + 0x340);
	tick(4);
	check(as_count(seq, AUTOSPLIT_SPLIT, TOD_AS_PLANET, &arg) == 1 && arg == 5 && as_all(seq) == 1,
	      "one split, to Mukow");

	/* A pause on its way out (its state 4): the level still loads at once. */
	wr32(TOD_GAME_STATE, TOD_STATE_PAUSE);
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_CLOSING);
	check(session_planet_load(2, 0) == ST_OK && rd32(TOD_REQUESTED_STATE) == TOD_LEVEL_LOAD_STATE && level_names_at(2),
	      "a pause closing: at once too");
	load_now_taken();

	/* Kept, each with nothing written. */
	wr32(TOD_CURRENT_LEVEL, 5);
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_LOADING);
	check(session_planet_load(2, 0) == ST_BUSY && nothing_asked() && rd32(TOD_CURRENT_LEVEL) == 5,
	      "kept: a pause whose loading thread is still building the menu PauseMenu_exit would free");
	wr32(TOD_PAUSE_STATE, TOD_PAUSE_UP);
	wr32(TOD_REQUESTED_STATE, 2);
	check(session_planet_load(2, 0) == ST_BUSY && rd32(TOD_REQUESTED_STATE) == 2 && tiers_dying() == 0,
	      "kept: never over a state change already asked for (a quit to the title from the pause)");
	wr32(TOD_REQUESTED_STATE, TOD_REQUESTED_NONE);
	wr32(TOD_LEVEL_OBJ, 0);
	check(session_planet_load(2, 0) == ST_BUSY && nothing_asked(), "kept: no level up");
	wr32(TOD_LEVEL_OBJ, FAKE_ROOT);
	wr32(TOD_GAME_STATE, TOD_STATE_FMV);
	check(session_planet_load(2, 0) == ST_BUSY && nothing_asked() && rd32(TOD_CURRENT_LEVEL) == 5,
	      "kept: an FMV, whose own exit (FmvState_exit) the level's unload never runs");
	wr32(TOD_GAME_STATE, TOD_STATE_GALMAP);
	check(session_planet_load(2, 0) == ST_BUSY && nothing_asked(),
	      "kept: the galactic map, whose own exit (GalMapState_exit) the unload never runs either");
	wr32(TOD_GAME_STATE, 3);
	check(session_planet_load(2, 0) == ST_BUSY && nothing_asked(), "kept: a load under way");

	/* Play again: the request as before. */
	wr32(TOD_PAUSE_STATE, 0);
	die_world();
	check(session_planet_load(2, 0) == ST_OK && rd32(TOD_LOAD_REQUEST) == 2 && rd32(TOD_REQUESTED_STATE) == TOD_REQUESTED_NONE &&
	      tiers_dying() == 0, "in play it is still the game's request, faded and loaded by its next frames");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	wr32(TOD_LUA_STATE, AS_STATE);
	tick(800);
	die_world();
}

/*
 * Build 59: the weapon bulk actions. The game's caps come from its own
 * weapon.csv, read through g_weaponDefTable: built here per weapon so each
 * has a cap of its own, with one weapon whose def says it cannot level, one
 * whose def the game has not loaded, and an ammo mod of each kind.
 */
#define TW_DEFS  0x31000000u
#define TW_CFGS  0x31010000u

static u32 tw_slot(u8 w)
{
	return TOD_WEAPONS + (u32)w * TOD_WEAPON_STRIDE;
}

static void tw_def(u8 w, u32 flags, u32 levels)
{
	u32 def = TW_DEFS + (u32)w * 0x80u, cfg = TW_CFGS + (u32)w * 0x800u, l;

	wr32(TOD_WEAPON_DEFS + (u32)w * 4u, def);
	wr32(def + TOD_WD_FLAGS, flags);
	wr32(def + TOD_WD_CONFIG, cfg);
	wr32(cfg + TOD_WC_NLEVELS, levels);
	wr32(cfg + TOD_WC_NMODS, 0);
	for (l = 0; l < TOD_MAX_LEVEL; l++) wrf(cfg + TOD_WC_MAX_AMMO + l * 4u, (f32)(10u * (l + 1u) + w));
}

static void tw_hold(u8 w, int owned, u8 level, f32 xp, f32 ammo)
{
	u8 b[2];

	b[0] = owned ? 1 : 0;
	b[1] = level;
	host_poke(tw_slot(w) + TOD_WS_OWNED, b, 2);
	wrf(tw_slot(w) + TOD_WS_XP, xp);
	wrf(tw_slot(w) + TOD_WS_AMMO, ammo);
	wr32(tw_slot(w) + TOD_WS_MODS, 0);
}

static u8 tw_level(u8 w)
{
	return *host_mem_ptr(tw_slot(w) + TOD_WS_LEVEL);
}

static void test_tod_weapon_actions(void)
{
	u8 before[TOD_NWEAPONS * TOD_WEAPON_STRIDE], after[TOD_NWEAPONS * TOD_WEAPON_STRIDE];
	u8 around[2][0x40], w;
	u32 k, changed, pass;
	int others;

	for (pass = 0; pass < 2; pass++) {
	group(pass == 0 ? "ToD: Max all weapon levels, each weapon to the game's own top, out of challenge mode (build 59)"
	                : "ToD: Max all weapon levels, the same top in challenge mode");
	/*
	 * Combuster (1): ten levels, owned at V2, below the old cap of V5. Shock
	 * Ravager (3): five levels, owned at V1. Buzz Blades (5): three levels.
	 * Predator Launcher (6): its def says it cannot level. Alpha Disruptor (7):
	 * ten levels, not owned. Pyro Blaster (8): owned, no def loaded. Shard
	 * Reaper (10): ten levels, at V5, the old cap. Negotiator (11): Omega
	 * already, V8, above it. Nano-Swarmers (12): at V10, the top. Every other
	 * weapon: ten levels, not owned. Challenge mode makes no difference.
	 */
	for (w = TOD_WPN_FIRST; w < TOD_WPN_GADGETS; w++) {
		tw_def(w, 0, w < TOD_WPN_DEVICES ? 10 : 0);
		tw_hold(w, 0, 0, 0.0f, 0.0f);
	}
	tw_def(3, 0, 5);
	tw_def(5, 0, 3);
	tw_def(6, 1, 10);
	wr32(TOD_WEAPON_DEFS + 8u * 4u, 0);
	tw_hold(1, 1, 1, 1000.0f, 3.0f);
	tw_hold(3, 1, 0, 0.0f, 3.0f);
	tw_hold(5, 1, 0, 0.0f, 3.0f);
	tw_hold(6, 1, 0, 0.0f, 3.0f);
	tw_hold(8, 1, 2, 2200.0f, 3.0f);
	tw_hold(10, 1, 4, 10736.0f, 3.0f);
	tw_hold(11, 1, 7, 90000.0f, 3.0f);
	tw_hold(12, 1, 9, 91520.0f, 3.0f);
	wr32(TOD_CHALLENGE, pass);
	host_peek(TOD_WEAPONS - 0x40u, around[0], 0x40);
	host_peek(TOD_WEAPONS + sizeof(before), around[1], 0x40);
	host_peek(TOD_WEAPONS, before, sizeof(before));

	check(features_trigger(TF_MAX_LEVELS) == ST_OK, "Max all weapon levels runs");
	check(tw_level(1) == 9 && rdf(tw_slot(1) + TOD_WS_XP) == (f32)tod_weapon_xp(1, 9),
	      "the Combuster, below the old cap, to V10 with V10's XP");
	check(tw_level(10) == 9 && rdf(tw_slot(10) + TOD_WS_XP) == (f32)tod_weapon_xp(10, 9),
	      "the Shard Reaper, at the old cap of V5, to V10 with its XP");
	check(tw_level(11) == 9 && rdf(tw_slot(11) + TOD_WS_XP) == (f32)tod_weapon_xp(11, 9),
	      "the Negotiator, above the old cap at V8, to V10");
	check(tw_level(12) == 9 && rdf(tw_slot(12) + TOD_WS_XP) == 91520.0f, "the Nano-Swarmers, at V10 already, left as they are");
	check(tw_level(3) == 5, "the Shock Ravager's five levels: index 5, min(numLevels, 9) as an Omega slot's WeaponSlot_getMaxLevel has it");
	check(tw_level(5) == 3, "the Buzz Blades' three: V4, min(numLevels, 9)");
	check(tw_level(6) == 0 && rdf(tw_slot(6) + TOD_WS_XP) == 0.0f, "the Predator Launcher, which cannot level, is left alone");
	check(tw_level(7) == 0, "the Alpha Disruptor, not owned, is passed over");
	check(tw_level(8) == 2, "the Pyro Blaster, whose def the game has not loaded, is left as it is");
	host_peek(TOD_WEAPONS, after, sizeof(after));
	changed = 0;
	others = 1;
	for (k = 0; k < sizeof(after); k++) {
		u32 slotw = k / TOD_WEAPON_STRIDE, off = k % TOD_WEAPON_STRIDE;
		if (after[k] == before[k]) continue;
		changed++;
		if ((slotw != 1 && slotw != 3 && slotw != 5 && slotw != 10 && slotw != 11) ||
		    !(off == TOD_WS_LEVEL || (off >= TOD_WS_XP && off < TOD_WS_XP + 4)))
			others = 0;
	}
	check(others && changed > 0, "only the raised weapons' level bytes and XP changed");
	{
		u8 now[2][0x40];
		host_peek(TOD_WEAPONS - 0x40u, now[0], 0x40);
		host_peek(TOD_WEAPONS + sizeof(before), now[1], 0x40);
		check(memcmp(now, around, sizeof(now)) == 0, "and nothing either side of the weapon slots");
	}
	check(features_trigger(TF_MAX_AMMO) == ST_OK && rdf(tw_slot(1) + TOD_WS_AMMO) == 101.0f &&
	      rdf(tw_slot(11) + TOD_WS_AMMO) == 111.0f, "Max all weapon ammo after it fills to V10's magazine");
	check(features_trigger(TF_MAX_LEVELS) == ST_OK && tw_level(1) == 9 && tw_level(3) == 5, "a second Max changes nothing");
	}

	group("ToD: Reset all weapon levels");
	check(features_trigger(TF_RESET_LEVELS) == ST_OK, "Reset all weapon levels runs");
	check(tw_level(1) == 0 && rdf(tw_slot(1) + TOD_WS_XP) == 0.0f && tw_level(11) == 0 && tw_level(3) == 0 &&
	      tw_level(5) == 0, "every owned weapon that levels back to V1, its XP 0");
	check(tw_level(8) == 2 && rdf(tw_slot(8) + TOD_WS_XP) == 2200.0f, "but the Pyro Blaster, whose def is not there");
	wr32(TOD_CHALLENGE, 0);

	group("ToD: Max all weapon ammo, the game's WeaponDef_getMaxAmmo");
	tw_def(8, 0, 10);
	/* The Combuster at V3 with two ammo mods: +5 (mod 1) and x0.5 more (mod 3); mod 2 is no ammo mod, mod 0 not fitted. */
	{
		u32 cfg = TW_CFGS + 1u * 0x800u, m = cfg + TOD_WC_MODS;

		wr32(cfg + TOD_WC_NMODS, 4);
		wrf(m + 0 * TOD_WC_MOD_LEN, 9.0f); wrf(m + 0 * TOD_WC_MOD_LEN + 4, 0.0f); wrf(m + 0 * TOD_WC_MOD_LEN + 8, 100.0f);
		wrf(m + 1 * TOD_WC_MOD_LEN, 9.0f); wrf(m + 1 * TOD_WC_MOD_LEN + 4, 0.0f); wrf(m + 1 * TOD_WC_MOD_LEN + 8, 5.0f);
		wrf(m + 2 * TOD_WC_MOD_LEN, 3.0f); wrf(m + 2 * TOD_WC_MOD_LEN + 4, 0.0f); wrf(m + 2 * TOD_WC_MOD_LEN + 8, 40.0f);
		wrf(m + 3 * TOD_WC_MOD_LEN, 9.0f); wrf(m + 3 * TOD_WC_MOD_LEN + 4, 1.0f); wrf(m + 3 * TOD_WC_MOD_LEN + 8, 0.5f);
		tw_hold(1, 1, 2, 2200.0f, 1.0f);
		wr32(tw_slot(1) + TOD_WS_MODS, 0xEu);
	}
	tw_hold(16, 1, 0, 0.0f, 0.0f);   /* Mr. Zurkon, a device */
	tw_hold(17, 0, 0, 0.0f, 0.0f);   /* the Confuzzler, not owned */
	host_peek(TOD_WEAPONS, before, sizeof(before));
	check(features_trigger(TF_MAX_AMMO) == ST_OK, "Max all weapon ammo runs");
	check(rdf(tw_slot(1) + TOD_WS_AMMO) == (31.0f + 5.0f) * 1.5f, "the Combuster: V3's 31 plus 5, times 1.5");
	check(rdf(tw_slot(3) + TOD_WS_AMMO) == 13.0f && rdf(tw_slot(8) + TOD_WS_AMMO) == 38.0f,
	      "the Shock Ravager at V1 and the Pyro Blaster at V3 (its def back): each its own level's magazine");
	check(rdf(tw_slot(16) + TOD_WS_AMMO) == 26.0f, "Mr. Zurkon, a device, filled from its own def");
	check(rdf(tw_slot(17) + TOD_WS_AMMO) == 0.0f && rdf(tw_slot(7) + TOD_WS_AMMO) == 0.0f,
	      "the Confuzzler and the Alpha Disruptor, not owned, are passed over");
	host_peek(TOD_WEAPONS, after, sizeof(after));
	others = 1;
	for (k = 0; k < sizeof(after); k++) {
		u32 off = k % TOD_WEAPON_STRIDE;
		if (after[k] != before[k] && !(off >= TOD_WS_AMMO && off < TOD_WS_AMMO + 4)) others = 0;
		if (after[k] != before[k] && k / TOD_WEAPON_STRIDE >= TOD_WPN_GADGETS) others = 0;
	}
	check(others, "nothing but the ammo words of weapons and devices changed");

	group("ToD: with no def table the weapon actions are BUSY and write nothing");
	for (w = 0; w < TOD_NWEAPONS; w++) wr32(TOD_WEAPON_DEFS + (u32)w * 4u, 0);
	host_peek(TOD_WEAPONS, before, sizeof(before));
	check(features_trigger(TF_MAX_LEVELS) == ST_BUSY && features_trigger(TF_RESET_LEVELS) == ST_BUSY &&
	      features_trigger(TF_MAX_AMMO) == ST_BUSY, "all three BUSY");
	host_peek(TOD_WEAPONS, after, sizeof(after));
	check(memcmp(before, after, sizeof(after)) == 0, "and the slots as they were");
	for (w = TOD_WPN_FIRST; w < TOD_WPN_GADGETS; w++) tw_hold(w, 0, 0, 0.0f, 0.0f);
}

/*
 * Revision 1.34: MOBY_TABLE for ToD. The walk a client makes (PROTOCOL.md
 * 5.5) over a pool laid out as the game lays it: slots up to the last index
 * in use, the free ones among them told by the handle's serial.
 */
static u32 moby_walk(const struct game_moby_table *t, u32 *live, u32 cap)
{
	u32 start = rd32(t->table_ptr_addr), end_word = rd32(t->table_end_ptr_addr), n = 0, i, rows;

	if ((t->flags & MOBY_FLAG_LAST_INDEX) != 0) rows = (s32)end_word < 0 ? 0 : end_word + 1u;
	else rows = (end_word - start) / t->stride;
	for (i = 0; i < rows; i++) {
		u8 b[2];
		host_peek(start + i * t->stride + t->live_offset, b, 2);
		if ((t->flags & MOBY_FLAG_LIVE_TEST) != 0 && be16_get(b) == t->dead_value) continue;
		if (n < cap) live[n] = i;
		n++;
	}
	return n;
}

static void test_tod_moby_table(void)
{
	const struct game_api *g = session_game();
	struct game_moby_table t;
	u32 live[16], n, k, slots = 0x31100000u;

	group("ToD: the moby table (revision 1.34)");
	memset(&t, 0, sizeof(t));
	check(g != NULL && g->moby_table != NULL && g->moby_table(&t) == ST_OK, "ToD answers MOBY_TABLE");
	check(t.table_ptr_addr == 0x1066C300u && t.table_end_ptr_addr == 0x1066C310u && t.stride == 0x180,
	      "g_mobyPool's slots pointer and maxIndex, a slot 0x180 bytes");
	check(t.flags == (MOBY_FLAG_LAST_INDEX | MOBY_FLAG_LIVE_TEST) && t.live_offset == 0x64 && t.dead_value == 0xFFFE,
	      "the end is the last index, a free slot's serial 0xFFFE");

	/* The simulator's level: four slots taken, slot 1 freed again. */
	n = moby_walk(&t, live, 16);
	check(n == 3 && live[0] == 0 && live[1] == 2 && live[2] == 3, "the seeded pool: slots 0, 2 and 3 live");

	/* A pool of ten, MobyPool_init's: every slot free, maxIndex -1. */
	wr32(TOD_MOBY_SLOTS, slots);
	wr32(TOD_MOBY_LAST, 0xFFFFFFFFu);
	for (k = 0; k < 10; k++) wr32(slots + k * 0x180u + 0x64u, 0xFFFE0000u | k);
	check(moby_walk(&t, live, 16) == 0, "a level's fresh pool: nothing");
	/* MobyPool_alloc takes 0..5 and raises maxIndex; processFrees frees 2 and 5 and lowers it to 4. */
	for (k = 0; k < 6; k++) wr32(slots + k * 0x180u + 0x64u, ((k + 1u) << 16) | k);
	wr32(slots + 2u * 0x180u + 0x64u, 0xFFFE0002u);
	wr32(slots + 5u * 0x180u + 0x64u, 0xFFFE0005u);
	wr32(TOD_MOBY_LAST, 4);
	/* Slot 6 holds a live-looking handle past the end: a walk that ignores the end would list it. */
	wr32(slots + 6u * 0x180u + 0x64u, 0x00070006u);
	n = moby_walk(&t, live, 16);
	check(n == 4 && live[0] == 0 && live[1] == 1 && live[2] == 3 && live[3] == 4,
	      "0, 1, 3 and 4: the freed slot 2 is not listed, and the walk stops at index 4");
	/* A serial whose low half is 0xFFFE is a live moby still: only the high half is the test. */
	wr32(slots + 1u * 0x180u + 0x64u, 0x0001FFFEu);
	check(moby_walk(&t, live, 16) == 4, "a live handle whose index half reads 0xFFFE is still live");
}

/*
 * Build 59 (D3): ToD's PLANET_LOAD honours both planet flags, as RaC1's and
 * UYA's: bit0 clears the destination's level flags (its group alone), bit1
 * its gold bolts (its own mask alone), each only on a load that goes ahead,
 * and DESCRIBE's extension says the load acts on both.
 */
static void test_tod_planet_flags(void)
{
	u8 all[TOD_LEVEL_FLAGS_LEN], got[TOD_LEVEL_FLAGS_LEN], out[4096];
	const u32 mine = 72, other = 140;   /* a flag of Mukow's (5) group and one of Rykan V's (9), tod_flags.c */
	u32 i, len = 0, off, cleared = 0;

	group("build 59: ToD's planet load acts on both planet flags (D3)");
	die_world();
	memset(all, 0xFF, sizeof(all));
	host_poke(TOD_LEVEL_FLAGS, all, sizeof(all));
	wr32(TOD_GOLD_BOLTS + 5u * TOD_LEVEL_SAVE_LEN, 0xFu);
	wr32(TOD_GOLD_BOLTS + 9u * TOD_LEVEL_SAVE_LEN, 0x3u);

	check(session_planet_load(5, 0) == ST_OK, "a load of Mukow with no flag");
	host_peek(TOD_LEVEL_FLAGS, got, sizeof(got));
	check(memcmp(got, all, sizeof(all)) == 0 && rd32(TOD_GOLD_BOLTS + 5u * TOD_LEVEL_SAVE_LEN) == 0xFu,
	      "clears nothing");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(800);
	die_world();

	check(session_planet_load(5, PLANET_FLAG_RESET_LEVELFLAGS) == ST_OK && rd32(TOD_LOAD_REQUEST) == 5,
	      "a load of Mukow with bit0 goes out");
	host_peek(TOD_LEVEL_FLAGS, got, sizeof(got));
	for (i = 0; i < TOD_NLEVEL_FLAGS; i++) {
		if ((got[TOD_LF_BYTE(i)] & (1u << (i & 7u))) == 0) cleared++;
	}
	check((got[TOD_LF_BYTE(mine)] & (1u << (mine & 7u))) == 0 &&
	      (got[TOD_LF_BYTE(other)] & (1u << (other & 7u))) != 0 && cleared == 32,
	      "Mukow's 32 level flags (72 to 103) are cleared, and no other: Rykan V's are not");
	check(rd32(TOD_GOLD_BOLTS + 5u * TOD_LEVEL_SAVE_LEN) == 0xFu, "and no gold bolt without bit1");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(800);
	die_world();

	check(session_planet_load(5, PLANET_FLAG_RESET_BOLTS) == ST_OK, "a load of Mukow with bit1 goes out");
	check(rd32(TOD_GOLD_BOLTS + 5u * TOD_LEVEL_SAVE_LEN) == 0 && rd32(TOD_GOLD_BOLTS + 9u * TOD_LEVEL_SAVE_LEN) == 0x3u,
	      "Mukow's gold bolts are taken back, Rykan V's kept");
	wr32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	tick(800);
	die_world();

	/* A load that is refused touches neither. */
	host_poke(TOD_LEVEL_FLAGS, all, sizeof(all));
	wr32(TOD_GOLD_BOLTS + 9u * TOD_LEVEL_SAVE_LEN, 0x3u);
	wr32(TOD_GAME_STATE, TOD_STATE_FMV);
	check(session_planet_load(9, PLANET_FLAG_RESET_LEVELFLAGS | PLANET_FLAG_RESET_BOLTS) == ST_BUSY,
	      "a load refused (an FMV)");
	host_peek(TOD_LEVEL_FLAGS, got, sizeof(got));
	check(memcmp(got, all, sizeof(all)) == 0 && rd32(TOD_GOLD_BOLTS + 9u * TOD_LEVEL_SAVE_LEN) == 0x3u,
	      "clears nothing");
	die_world();

	/* What the client is told: both bits (DESCRIBE's extension), and the pause row. */
	check(features_describe(out, sizeof(out), &len) == ST_OK, "DESCRIBE encodes");
	off = 2 + (u32)out[1] * 24;
	off += 1 + (u32)out[off] * 24;
	off += 1 + (u32)out[off] * FEATURE_WIRE_SIZE;
	check(len == off + 1 + DESCRIBE_EXT_LEN && out[off] == DESCRIBE_EXT_LEN &&
	      out[off + 1] == (PLANET_FLAG_RESET_LEVELFLAGS | PLANET_FLAG_RESET_BOLTS),
	      "and DESCRIBE says ToD's load acts on both");
	check(memcmp(out + off + 3, "Inventory\0", 10) == 0 && (out[off + 2] & DESCRIBE_MENU_HERE) != 0,
	      "that the Trainer takes Inventory, and that it can come up here");

	/* Revision 1.34, ACTION_STATUS: the client's line for Cache cutscenes' word, worded by the module. */
	group("revision 1.34: ToD's Cache cutscenes line, for a client (ACTION_STATUS)");
	check(features_action_status(TF_CACHE_CUTSCENES, (3u << 28) | (139u << 16) | (104u << 8) | 1u, out, sizeof(out), &len)
	      == ST_OK && out[0] == MENU_TONE_GOOD && out[1] == 36 && len == 38 &&
	      memcmp(out + 2, "Cutscenes cached (104 s, 13.9 MiB/s)", 36) == 0,
	      "done: the seconds and the read rate, a tenth of a MiB/s, green");
	check(features_action_status(TF_CACHE_CUTSCENES, (2u << 28) | (3u << 24) | (42u << 16), out, sizeof(out), &len)
	      == ST_OK && out[0] == MENU_TONE_BUSY && len == 2u + out[1] &&
	      memcmp(out + 2, "Caching cutscenes: Cobalia 42%", out[1]) == 0 && out[1] == 30,
	      "caching: the planet and how far, grey");
	check(features_action_status(TF_CACHE_CUTSCENES, (4u << 28) | (9u << 24), out, sizeof(out), &len) == ST_OK &&
	      out[0] == MENU_TONE_BAD && memcmp(out + 2, "Cutscene cache: too late for Cobalia, the run got there first", out[1]) == 0,
	      "too late for a planet, red");
	check(features_action_status(TF_CACHE_CUTSCENES, 0, out, sizeof(out), &len) == ST_OK && out[0] == MENU_TONE_NONE &&
	      out[1] == 0, "no run this boot: nothing to say");
	check(features_action_status(TF_FAST_LOADS, 0, out, sizeof(out), &len) == ST_NOT_FOUND &&
	      features_action_status(TF_DIE, 0, out, sizeof(out), &len) == ST_NOT_FOUND,
	      "NOT_FOUND for a toggle and for an action with no status readout");
}

void test_tod_game(void)
{
	test_tod_session();
	test_tod_features();
	test_tod_weapon_actions();
	test_tod_moby_table();
	test_tod_level_flags();
	test_tod_lua_console();
	test_tod_lua_rpcs3();
	test_tod_savefile();
	test_tod_savefile_boots();
	test_tod_savefile_aside();
	test_tod_savefile_aside_fast();
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
	test_tod_positions_camera();   /* 2026-11-13 */
	test_tod_strafe_camera();      /* 2026-11-14 */
	test_tod_positions_no_hud_hero();   /* revision 1.34 */
	test_tod_die();
	test_tod_pause_loads();        /* 2026-11-14 */
	test_tod_planet_flags();       /* build 59 */
	test_tod_fastloads();   /* last: it boots and ticks, and the autosplitter's run start reads tick phases */
	test_tod_load_fade();
	test_tod_load_fast();
	test_tod_cutscenes();
	test_tod_autosplit_revisit();   /* rev 1.33 (pending): after the Fast loads and cutscene tests' helpers */
	test_tod_autosplit_clank();     /* rev 1.34 */
	test_tod_cutscene_cache();
	test_tod_on_boot();
	test_tod_black_screens();
	quit();
}
