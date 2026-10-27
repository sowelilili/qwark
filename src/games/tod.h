/*
 * Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): the facts a
 * ToD game module needs, as data (docs/tod-poc.md, REPORT-tod-lua section 7).
 *
 * Phase 1: this file and tod.c compile on their own and register nothing.
 * Phase 2 wires them in behind the core_api seam as a hardcoded ToD module
 * (game 5), and later turns the same table into the per-title data file of the
 * generic `game-lua` module (SPEC-game-modules 4.12). So the table is shaped
 * like that data file: plain numbers and strings, no handlers.
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

#include "../plat/plat.h"

#define TOD_TITLE "NPEA00452"

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
	u8  weapon_xp;         /* u32 */
	u8  weapon_ammo;       /* u32 */
	u8  weapon_mods;       /* u16 */
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

extern const struct tod_facts tod_facts;

/* The game's pad mask in the OG layout. */
u32 tod_pad_to_og(u32 tod_mask);

/*
 * An ENUM Lua row's chunk with "%v" replaced by `value` in decimal, into dst
 * (cap bytes, always ended). Returns the length, or -1 when it does not fit.
 */
int tod_row_chunk(const struct tod_row *row, u32 value, char *dst, u32 cap);

#endif /* QWARK_TOD_H */
