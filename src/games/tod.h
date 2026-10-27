/*
 * Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): the address
 * table, the seam between tod_game.c (hot block, descriptors, vtable) and
 * tod_panel.c (the handlers), and the fact sheet (tod.c) the Lua helper and
 * the later per-title data file share (docs/tod-poc.md, REPORT-tod-lua
 * section 7, SPEC-game-modules 4.12).
 *
 * Game 5, built only where the games are modules or linked into a PC build:
 * `make` builds it as tod.sprx, the host builds link it, and `make release`
 * leaves it out altogether (titles.c has no ToD entry there).
 *
 * Every fact carries how it is known (struct tod_mark):
 *   TOD_DEC    read in a decompile of the shipped executable
 *   TOD_DIS    read in its disassembly or raw words
 *   TOD_SCAN   a Python pass over the executable (REPORT-tod-lua section 1)
 *   TOD_LEGACY only from the old C# RaCMAN or the autosplitter
 *   TOD_INFER  inferred from the above; says what would settle it
 * Nothing in here has been read on a running game yet (docs/tod-poc.md 5).
 */
#ifndef QWARK_TOD_H
#define QWARK_TOD_H

#include "game.h"

#define TOD_TITLE "NPEA00452"

/* ------------------------------------------------------- the address table */

/*
 * The game's own names (Ghidra, DL-ghidra:/NPEA00452.ELF) beside each. "dec"
 * is the decompile, "dis" the disassembly, "csv" the game's config sheets.
 */
#define TOD_FP_ADDR          0x002EA5F0u  /* stw r31,0x6e8(r11) in LoadLevel, never patched (dis) */
#define TOD_FP_WORD          0x93EB06E8u

#define TOD_HOT_INPUTS       0x10691918u  /* g_padAnalogs: f32 LX, LY, RX, RY, (v - 128) / 128 (dis) */
#define TOD_HOT_INPUTS_LEN   0x5Cu
#define TOD_OFF_PRESSED      0x50u        /* g_padPressed */
#define TOD_OFF_RELEASED     0x54u        /* g_padReleased */
#define TOD_OFF_MASK         0x58u        /* g_padMask, the game's bit order (tod_pad_to_og) */
#define TOD_PAD_MASK         (TOD_HOT_INPUTS + TOD_OFF_MASK)   /* 0x10691970 */

#define TOD_CURRENT_LEVEL    0x1029C558u  /* g_currentLevel, s32 LEVEL_, g_saveData+0x906E8 (dec) */
#define TOD_CHALLENGE        0x1029C55Cu  /* g_challengeMode, u32: 0 normal, N the Nth challenge mode (dec) */
#define TOD_LOAD_REQUEST     0x1006E764u  /* g_planetLoadRequest, s32, -1 none (dec) */
#define TOD_GAME_STATE       0x10387F4Cu  /* g_gameState, s32, 4 in gameplay (dis) */
#define TOD_STATE_GAMEPLAY   4
#define TOD_REQUESTED_STATE  0x100FA21Cu  /* g_requestedGameState, 3 = a level load (dis) */
#define TOD_QUIT_BYTE        0x1037A8E8u  /* g_quitRequested, SystemQuitCallback sets 1 (dis) */
#define TOD_NPLANETS         19u

/*
 * The hero: [[TOD_LEVEL_OBJ] + TOD_HERO_PTR], rewritten every frame from
 * g_heroList (HeroList_getCurrentHero), 0 between levels (dec).
 */
#define TOD_LEVEL_OBJ        0x10740BB4u  /* the per-level object, 0x5B8F0 bytes (BigObj5B8F0_Init) */
#define TOD_HERO_PTR         0x0000C684u
#define TOD_HERO_MOBY        0x14u        /* ToDHero.moby: ToDMoby*, whose position is +0x30 */
#define TOD_MOBY_POS         0x30u
#define TOD_HERO_PREV_POS    0x1250u      /* ToDHeroMotion prevPos, vec4 */
#define TOD_HERO_POS         0x1260u      /* pos, then the collision centre and its last value */
#define TOD_POS_BLOB_LEN     48u          /* pos, centre, previous centre: never the handle at +0x1290 */
#define TOD_HERO_HEALTH      0x1784u      /* f32 hit points; Hero_checkDeath kills at <= 0 */
#define TOD_HERO_MAX_HEALTH  0x1788u      /* f32 */

/* g_saveData and what is in it (ToDSaveData, dec). */
#define TOD_SAVE_DATA        0x1020BE70u
#define TOD_WEAPONS          0x1020BE70u  /* ToDWeaponSlot[32], indexed by WPN_ */
#define TOD_WEAPON_STRIDE    0x14u
#define TOD_NWEAPONS         32u
#define TOD_WS_XP            0x04u        /* f32 */
#define TOD_WS_AMMO          0x08u        /* f32 */
#define TOD_WS_MODS          0x0Cu        /* u32, bit n = mod n */
#define TOD_WS_OWNED         0x10u        /* u8 */
#define TOD_WS_LEVEL         0x11u        /* u8, 0..9 = V1..V10 */
#define TOD_MAX_LEVEL        10u
#define TOD_OBJECTS          0x1020C174u  /* s32[23] by OBJ_, a count; "has" = non-zero */
#define TOD_NOBJ_OWNED       15u          /* OBJ_ 0..14 are owned flags; 15.. are counters */
#define TOD_BOLTS            0x1020C28Cu  /* u32 */
#define TOD_RARITANIUM       0x1020C290u  /* u32 */
#define TOD_MAX_BOLTS        999999999u   /* Hero_addBolts and Hero_addMinerals cap both there */
#define TOD_LEVEL_SAVE       0x1020C2F8u  /* ToDLevelSave[19], 0x408 each */
#define TOD_LEVEL_SAVE_LEN   0x408u
#define TOD_GOLD_BOLTS       0x1020C6E4u  /* u32 bit mask per level, + L * 0x408 (LevelSave_collectGoldBolt) */
#define TOD_RYNO_PLANS       0x10214564u  /* u32 bit mask; the game's "give all" ORs TOD_RYNO_ALL */
#define TOD_RYNO_ALL         0x0007DEE4u
#define TOD_CHEATS           0x10214568u  /* u8[14] */
#define TOD_SKILL_POINTS     0x10214580u  /* u64, bit i = SKILLPOINT i (SkillPoint_set) */
#define TOD_NSKILL_POINTS    60u

/* Cheats the game left in (dec; nothing in the game writes them). */
#define TOD_NO_DAMAGE        0x101F1F00u  /* g_debugHeroNoDamage, u32: the hero's damage is dropped */
#define TOD_INFINITE_AMMO    0x10385BE8u  /* g_infiniteAmmo, u32: Weapon_hasAmmo always true */

/* Lua (REPORT-tod-lua section 2). */
#define TOD_LUA_VM           0x102043CCu  /* g_gameplayLuaVM */
#define TOD_LUA_STATE        0x102043D0u  /* its lua_State*, 0 between levels */
#define TOD_GAME_CLOCK       0x1074DDA8u  /* f32 time scale at +0, game time at +0x14 */

/* WPN_ classes (csv): 0 the wrench, 1..15 weapons, 16..23 devices, 24..31 gadgets. */
#define TOD_WPN_FIRST        1u
#define TOD_WPN_DEVICES      16u
#define TOD_WPN_GADGETS      24u

/* ------------------------------------------------------------- the marks */

#define TOD_DEC    1
#define TOD_DIS    2
#define TOD_SCAN   3
#define TOD_LEGACY 4
#define TOD_INFER  5

struct tod_mark {
	const char *name;
	u32 addr;
	u8  how;
	const char *note;
};

/* ------------------------------------------------------ the feature rows */

/*
 * One feature, REPORT-tod-lua section 5's shape. A row either writes memory
 * (write_addr, write_size) or runs Lua (lua), never both:
 *
 *   ACTION + lua           send the chunk; the reply's text is the result
 *   ENUM   + write_addr    write the chosen option as a write_size-byte
 *                          big-endian integer (the planet picker)
 *   ENUM   + lua           the chunk with every "%v" replaced by the option,
 *                          in decimal
 *
 * `kind` is proto.h's FEATURE_*. Live state (bolts, unlocks, the planet) is
 * never read through Lua: qwark reads and writes it in C every tick, as for
 * the other games; Lua is for actions and mods.
 */
#define TOD_ROW_NEEDS_LUA   0x01   /* the helper must be in */
#define TOD_ROW_GAMEPLAY    0x02   /* only does anything during gameplay frames */

struct tod_row {
	u8  id;
	u8  kind;
	u8  flags;
	u8  options;           /* ENUM: option count */
	const char *label;
	const char *lua;       /* NULL for a memory row */
	u32 write_addr;        /* 0 for a Lua row */
	u8  write_size;
};

/* ------------------------------------------------------ the fact sheet */

struct tod_facts {
	const char *title;

	/* Identity: one word that is never patched. */
	u32 fp_addr;
	u32 fp_word;

	/* The hot block: one read every tick. */
	u32 hot_addr;
	u16 hot_len;
	u16 hot_analog;        /* 4 x f32 LX, LY, RX, RY, already (v - 128) / 128 */
	u16 hot_pressed;       /* u32, the game's layout */
	u16 hot_released;      /* u32 */
	u16 hot_mask;          /* u32 */

	/* The game's pad bit i is the OG layout's bit pad_remap[i] (PROTOCOL.md 6). */
	u8  pad_remap[16];

	/* Planets. */
	u32 cur_level;         /* u32, the save's LEVEL_ id: what a load writes, never what starts one */
	u32 planet_request;    /* s32, -1 none: the pending load the game polls */
	u32 game_state;        /* s32, 4 during gameplay, when a load request is deferred to planet_request */
	u32 requested_state;   /* s32, 3 once a load is under way */
	u8  nplanets;
	const char * const *planet_names;   /* the LEVEL_ enum, 0 .. nplanets - 1 */

	/* The quit byte, set just before the game exits to the XMB. */
	u32 quit_byte;

	/* Positions: [[pos_root] + pos_hero] + pos_blob, 3 reads. */
	u32 pos_root;
	u32 pos_hero;
	u32 pos_blob;
	u16 pos_len;
	u32 hero_health;       /* off the hero, f32 */

	/* Inventory: g_saveData, which the hero's inventory pointer points at. */
	u32 save_data;
	u32 bolts;             /* u32 */
	u32 raritanium;        /* u32 */
	u32 weapons;           /* 32 slots of weapon_stride bytes, indexed by WPN_ */
	u16 weapon_stride;
	u8  nweapons;
	u8  weapon_owned;      /* u8 within a slot */
	u8  weapon_level;      /* u8, level - 1 */
	u8  weapon_xp;         /* f32 */
	u8  weapon_ammo;       /* f32 */
	u8  weapon_mods;       /* u32, a bit per mod */
	u32 weapon_unlocked;   /* 32 bytes, unlock_weapon's */
	u32 cheats;            /* CHEAT_COUNT bytes, 1 = unlocked */
	u8  ncheats;
	u32 level_flags;       /* g_levelFlags */

	/* Lua. */
	u32 lua_vm;            /* g_gameplayLuaVM */
	u32 lua_state;         /* its lua_State*, 0 between levels */
	u32 game_clock;        /* f32 time scale at +0, f32 game time at +0x14 */

	/* The features of the proof of concept. */
	const struct tod_row *rows;
	u8  nrows;

	const struct tod_mark *marks;
	u8  nmarks;
};

/* ------------------------------------------ the seam, tod_game.c / tod_panel.c */

/* Readout slots. */
#define TOD_RO_BOLTS       0
#define TOD_RO_RARITANIUM  1
#define TOD_RO_CHALLENGE   2

/*
 * Feature ids, stable: new ones go on the end. 0 to 3 are the proof of
 * concept's rows (tod_rows below: the Lua test, normal speed, the planet as a
 * write and through Lua), which DESCRIBE does not list: the client's Lua box
 * sends the first two through LUA_EXEC, and planets load through PLANET_LOAD.
 * The four numbers are reserved, never to be another feature's.
 */
#define TF_DIE              4
#define TF_BOLTS            5
#define TF_RARITANIUM       6
#define TF_CHALLENGE        7
#define TF_INFINITE_AMMO    8
#define TF_GOD_RATCHET      9
#define TF_UNLOCK_SP        10
#define TF_RESET_SP         11
#define TF_UNLOCK_GB        12
#define TF_RESET_GB         13
#define TF_UNLOCK_RYNO      14
#define TF_RESET_RYNO       15

/* Unlock row ids: WPN_ 1..31 as themselves, then these bases. */
#define TOD_UL_OBJ          32u    /* + OBJ_, 0..14 */
#define TOD_UL_SP           64u    /* + SKILLPOINT, 0..59 */
#define TOD_UL_GB           128u   /* + the gold bolt's index, 0..31 (tod_gold_bolt) */
#define TOD_NGOLD_BOLTS     32u

/* tod_panel.c */
int  tod_hero(u32 *hero);
int  tod_trigger(u8 id);
int  tod_set_value(u8 id, u32 value);
int  tod_set_toggle(u8 id, int on);
int  tod_toggle_read(u8 id, int *on);
int  tod_save_blob(u8 *blob, u8 *len);
int  tod_load_blob(const u8 *blob, u8 len);
int  tod_planet_load(u8 planet, u8 flags);
int  tod_die(void);
int  tod_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int  tod_unlock_read(const struct game_unlock *entry, u32 values[4]);
int  tod_unlock_set(u8 id, u8 field, u32 value);
/* Infinite ammo's hold: once a tick, with the game's flag and the level object as read. */
void tod_ammo_tick(u32 flag, u32 root);
/* A new process: the hold starts over. */
void tod_panel_reset(void);

/* The gold bolt `index` (0..31) as its level and its bit: 1, or 0 for none. */
int  tod_gold_bolt(u8 index, u8 *level, u8 *bit);
/* The XP weapon `wpn` (1..15) has at level index `level` (0..9), whole points; 0 for none. */
u32  tod_weapon_xp(u8 wpn, u8 level);

/*
 * The level flags (tod_flags.c, revision 1.28 pending): the game's
 * GLOBAL_FLAG_ enum, 292 flags, one bit each in five big-endian u64 words at
 * g_saveData+0x5528. Flag n is bit n & 63 of word n >> 6 (Lua_check_flag,
 * Lua_set_flag, Lua_clear_flag, SetProgressionFlag), so its byte is
 * (n >> 6) * 8 + 7 - ((n & 63) >> 3) and its bit there n & 7. The region is
 * the game's, not a planet's: LEVELFLAGS_GET answers the same 40 bytes for
 * every planet, and LEVELFLAGS_RESET clears the flags named for that planet.
 */
#define TOD_LEVEL_FLAGS      0x10211398u  /* g_levelFlags, ulonglong[5] (dec) */
#define TOD_NLEVEL_FLAGS     292u         /* GLOBAL_FLAG_COUNT */
#define TOD_LEVEL_FLAGS_LEN  40u          /* 5 u64 words */
#define TOD_LF_BYTE(n)       ((((n) >> 6) * 8u) + 7u - (((n) & 63u) >> 3))
#define TOD_LF_BIT(n)        ((u16)(TOD_LF_BYTE(n) * 8u + ((n) & 7u)))

int  tod_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int  tod_levelflags_reset(u8 planet);
int  tod_levelflags_set(u8 planet, u16 offset, u8 value);
const struct levelflag_names *tod_levelflag_names(void);

extern const struct tod_facts tod_facts;

/* The game's pad mask in the OG layout. */
u32 tod_pad_to_og(u32 tod_mask);

/*
 * An ENUM Lua row's chunk with "%v" replaced by `value` in decimal, into dst
 * (cap bytes, always ended). Returns the length, or -1 when it does not fit.
 */
int tod_row_chunk(const struct tod_row *row, u32 value, char *dst, u32 cap);

#endif /* QWARK_TOD_H */
