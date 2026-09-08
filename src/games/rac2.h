/*
 * Ratchet & Clank 2: Going Commando (NPEA00386, and BCES01503 when the disc
 * trilogy boots it): the address table and the seam between rac2.c (numbers, hot
 * block, descriptors, vtable) and rac2_panel.c (the handlers). Nothing outside
 * those two files includes this.
 *
 * Every constant is from racman's RaCTrainer/Games/RAC2: rac2.cs for the address
 * table, RAC2Form.cs for the button handlers and the loading-screen watcher,
 * RC2Unlocks.cs for the owned-item table, FormCollectables.cs for the
 * collectable arrays and ChargebootColorPicker.cs for the four colour words.
 * Where the old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC2_H
#define QWARK_RAC2_H

#include "game.h"

/* -------------------------------------------------------- rac2.cs addresses */

#define RAC2_LOADSCREEN_TYPE   0x147A257u  /* which of the five load screens */
#define RAC2_LOADSCREEN_COUNT  0x147A25Bu  /* the watcher's trigger, 2 = final */
#define RAC2_INPUTS            0x147A430u  /* u32 pad mask, OG layout */
#define RAC2_ANALOGS           0x147A60Cu  /* f32 rx, ry, lx, ly */

#define RAC2_SAVE_SLOT         0x13298CCu  /* s16, -1 after QE; the QE feature */
#define RAC2_CURRENT_PLANET    0x1329A3Cu
#define RAC2_BOLTS             0x1329A90u
#define RAC2_RARITANIUM        0x1329A94u
#define RAC2_PREV_HELD_WEAPON  0x1329A9Fu  /* gadget storage; Swingshot is 0x0D */
#define RAC2_CHALLENGE_MODE    0x1329AA2u  /* one byte */
#define RAC2_HEALTH_XP         0x1329AA4u
#define RAC2_EXP_ECONOMY       0x1329AA8u  /* one byte, 100 = insta-upgrades */
#define RAC2_BOLT_DEFICIT      0x1329AACu
#define RAC2_NANOTECH_BOOSTS   0x1329AC0u  /* 10 bytes */

#define RAC2_COORDS            0x147F260u  /* f32 x, y, z (Vec4) */
#define RAC2_GHOST_TIMER       0x147F3CEu
#define RAC2_PLAYER_STATE      0x1481474u
#define RAC2_HERO_TYPE         0x1481494u
#define RAC2_FREEZE_HEALTH     0x14816ACu  /* frozen to 42069 by the old form */
#define RAC2_BOSS_SIBERIUS     0x1481792u  /* one byte, cleared on death */
#define RAC2_BOSS_SNIVELAK     0x14817A3u  /* one byte, cleared on death */
#define RAC2_SLOTS_HIT         0x14817AFu
#define RAC2_SKILL_POINTS      0x1481809u  /* 30 bytes */
#define RAC2_AMMO_ARRAY        0x148185Cu  /* 136 bytes */
#define RAC2_UNLOCK_BASE       0x1481A82u  /* first owned byte, see rac2_panel.c */

#define RAC2_PLATINUM_BOLTS    0x1562540u  /* 0x70 bytes */
#define RAC2_LEVEL_FLAGS       0x15625B0u  /* 0x10 per planet */
#define RAC2_LOAD_PLANET       0x156B050u  /* request word, then the planet index */
#define RAC2_JANKPOT_ACTIVE    0x15718F8u
#define RAC2_CS_STORAGE        0x1578424u
#define RAC2_BOSS_HEALTHBAR    0x15784E8u
#define RAC2_MOBY_TABLE        0x15927B0u
#define RAC2_MOBY_TABLE_END    0x15927B8u
#define RAC2_MOBY_STRIDE       0x100u      /* sizeof(Moby) from rac2.cs */
#define RAC2_DEBUG_FEATURES    0x15B3070u  /* one byte, the built-in debug flag */
#define RAC2_RESPAWN_COORDS    0x15D26E0u  /* 32 bytes copied from the player */

#define RAC2_CHARGE_BUFFER     0x145C180u  /* auto buffer charge, set to 30 */
#define RAC2_AMMO_RESET_INSTR  0x0B30C7Cu  /* the ammo decrement, nopped */
#define RAC2_FASTLOAD_INSTR    0x00BEA8A0u /* also the fingerprint site */

#define RAC2_PBOLTS            0x1390C27u  /* Maktar slots */
#define RAC2_PJACKPOT          0x1390C37u

#define RAC2_CB_PRIMARY_FRONT  0x1318590u
#define RAC2_CB_PRIMARY_BACK   0x1318594u
/*
 * rac2.cs lists chargebootsTintFrontColor and chargebootsTintBackColor at the
 * SAME address, 0x13185a0. The colour picker writes the front colour there and
 * then the mid colour over it, so the mid colour is what survives. Both words
 * are exposed, because that is the pair the picker drove, and they share one
 * readout: whichever is written last is the value the game holds. Worth checking
 * on hardware whether the back tint really lives at 0x13185a4.
 */
#define RAC2_CB_TINT_FRONT     0x13185A0u
#define RAC2_CB_TINT_BACK      0x13185A0u
#define RAC2_PAD_MANIP         0x13185B8u  /* jump-pad speed float */

#define RAC2_SHORTCUTS_INDEX   0x1352684u
#define RAC2_IM_SHORTCUTS      0x135268Cu
#define RAC2_OLDSKOOL_SP       0x133E8A0u
#define RAC2_MOBY3595_PTR      0x13A2D90u
#define RAC2_HRUGIS_MISSION    0x143DB0Fu

#define RAC2_FELTZIN_RARI      0x1A30430u
#define RAC2_SAVED_RACE_INDEX  0x1A4D7E0u
#define RAC2_ENDAKO_BOSS_CS    0x1A58158u
#define RAC2_DORBIT_OPENING    0x1A59764u
#define RAC2_SIB_BOSS          0x1A5A99Fu
#define RAC2_SNIV_BOSS         0x1A6FB73u
#define RAC2_FELTZIN_OPENING   0x1A8495Bu
#define RAC2_FELTZIN_MISSION   0x1A84973u
#define RAC2_LAST_RARITANIUM   0x1A849A8u
#define RAC2_GORN_OPENING      0x1A99A34u
#define RAC2_GORN_MANIP        0x1A99A4Cu
#define RAC2_GORN_MISSION      0x1A99A5Bu
#define RAC2_YEEDIL_BOSS       0x1A9DF90u
#define RAC2_PYRAMID_BOLT      0x1AAC767u

/*
 * The savefile helper is a mod, not part of the game. RAC2Form used two pairs of
 * request bytes: the two buttons on the main form (set aside, load) and the save
 * manager's own pair. All five bytes are gated on the helper byte reading 1.
 */
#define RAC2_SF_LOAD_ASIDE     0x1BF0000u  /* loadFileButton */
#define RAC2_SF_MGR_LOAD       0x1BF0001u  /* SavefileLoader api_loadfile */
#define RAC2_SF_HELPER         0x1BF0002u  /* 1 when the mod is loaded */
#define RAC2_SF_MGR_SAVE       0x1BF0003u  /* SavefileLoader api_savefile */
#define RAC2_SF_SET_ASIDE      0x1BF0004u  /* setAsideFileButton */

#define RAC2_POS_BLOB_LEN  30          /* what racman stores for a position slot */
#define RAC2_LF_LEN        0x10        /* level flag bytes per planet */

/*
 * Fingerprint: the original instruction at the fast-load patch site, 0x4BFFEA69
 * ("b -5527"), which racman restores when fast loads are turned off. qwark's own
 * patched form, the nop, is accepted too, so a console where the site is already
 * patched does not sit in BOOTING for ever.
 */
#define RAC2_FP_ADDR RAC2_FASTLOAD_INSTR
extern const u8 rac2_fp[4];
extern const u8 rac2_fp_patched[4];

/* ---------------------------------------------------------------- hot block */

/* inputs: 0x147A250 .. 0x147A61C, one read covering load screen, pad and analogs */
#define RAC2_HOT_INPUTS_ADDR  0x147A250u
#define RAC2_HOT_INPUTS_LEN   0x3CC
/* state: 0x13298CC .. 0x1329AB0, QE offset through the bolt deficit */
#define RAC2_HOT_STATE_ADDR   RAC2_SAVE_SLOT
#define RAC2_HOT_STATE_LEN    0x1E4
/* player: the coordinate Vec4 plus the rotation that follows it */
#define RAC2_HOT_PLAYER_ADDR  RAC2_COORDS
#define RAC2_HOT_PLAYER_LEN   0x20

/* ----------------------------------------------------------- readout slots */

#define RAC2_RO_BOLTS      0
#define RAC2_RO_SAVEFILE   1
#define RAC2_RO_RARITANIUM 2
#define RAC2_RO_CHALLENGE  3
#define RAC2_RO_HEALTH_XP  4
#define RAC2_RO_QE_OFFSET  5
#define RAC2_RO_CB_FRONT   6
#define RAC2_RO_CB_BACK    7
#define RAC2_RO_CB_TINT    8

/* ------------------------------------------------------------- feature ids */

/* Stable: new features go on the end, a retired one leaves its number behind. */
#define R2_FAST_LOADS        0
#define R2_INFINITE_AMMO     1
#define R2_FREEZE_HEALTH     2
#define R2_GHOST             3
#define R2_INSTA_UPGRADE     4
#define R2_DEBUG_MODE        5

#define R2_DIE               6
#define R2_BOLTS             7
#define R2_RARITANIUM        8
#define R2_CHALLENGE         9
#define R2_HEALTH_XP         10
#define R2_SET_RESPAWN       11
#define R2_STORE_SWINGSHOT   12
#define R2_DEATH_BOSSES      13
#define R2_DEATH_PBOLTS      14

#define R2_RESET_ANYPCT      15
#define R2_RESET_MENUS       16
#define R2_SETUP_NGPLUS      17
#define R2_SETUP_NO_IMG      18
#define R2_SETUP_ALL_MISSION 19
#define R2_MAKTAR_SLOTS      20
#define R2_AUTO_ANYPCT       21
#define R2_AUTO_NGPLUS       22
#define R2_QE_OFFSET         23

#define R2_RESET_PBOLTS      24
#define R2_UNLOCK_PBOLTS     25
#define R2_RESET_NANOTECH    26
#define R2_UNLOCK_NANOTECH   27
#define R2_RESET_SKILL       28
#define R2_UNLOCK_SKILL      29

#define R2_LOAD_ASIDE        30
#define R2_SET_ASIDE         31
#define R2_MGR_SAVE          32
#define R2_MGR_LOAD          33

#define R2_CB_PRIMARY_FRONT  34
#define R2_CB_PRIMARY_BACK   35
#define R2_CB_TINT_FRONT     36
#define R2_CB_TINT_BACK      37

/* ------------------------------------------------------ rac2_panel.c exports */

int rac2_trigger(u8 id);
int rac2_set_value(u8 id, u32 value);
int rac2_load_setaside(void);
int rac2_die(void);

int rac2_planet_load(u8 planet, u8 flags);

int rac2_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac2_levelflags_reset(u8 planet);
int rac2_levelflags_set(u8 planet, u16 offset, u8 value);

int rac2_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac2_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac2_unlock_set(u8 id, u8 field, u32 value);

/* The loading-screen watcher's two optional resets. */
int rac2_reset_anypct(void);
int rac2_reset_menu_storage(void);

/* ---------------------------------------------------------- rac2.c exports */

u8  rac2_planet_count(void);

/* The fast-load patch, shared with the planet load and the set-aside load. */
int rac2_fastload_force(void);
int rac2_fastload_restore(void);

/* RAC2Form's desiredShortcutIndex; 0xFFFFFFFF means no setup has run. */
void rac2_set_shortcut_index(u32 index);
u32  rac2_shortcut_index(void);

/* The two "on death" toggles SelfDeathExtended took as arguments. */
int rac2_death_bosses(void);
int rac2_death_pbolts(void);

#endif /* QWARK_RAC2_H */
