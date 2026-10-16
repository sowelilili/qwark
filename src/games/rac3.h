/*
 * Ratchet & Clank 3: Up Your Arsenal (NPEA00387, and BCES01503 when the disc
 * trilogy boots it): the address table and the seam between rac3.c (numbers, hot
 * block, descriptors, vtable) and rac3_panel.c (the handlers). Nothing outside
 * those two files includes this.
 *
 * Every constant is from racman's RaCTrainer/Games/RAC3: rac3.cs for the address
 * table and the setups, RAC3Form.cs for the button handlers, UYAUnlocks.cs for
 * the item table and RAC3Form.Designer.cs for the armour and ship-colour names.
 * Where the old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC3_H
#define QWARK_RAC3_H

#include "game.h"

/* -------------------------------------------------------- rac3.cs addresses */

#define RAC3_INPUTS            0x00D99370u  /* u32 pad mask, OG layout */
#define RAC3_ANALOGS           0x00D9954Cu  /* f32 rx, ry, lx, ly */
#define RAC3_LOADING_SCREEN_ID 0x00D99114u

#define RAC3_QE_OFFSET         0x00C1E2C0u  /* s16 */
#define RAC3_CURRENT_PLANET    0x00C1E438u
#define RAC3_ITEM_ARRAY        0x00C1E43Cu  /* one byte per item id: its version */
#define RAC3_BOLTS             0x00C1E4DCu
#define RAC3_CHALLENGE_MODE    0x00C1E50Eu  /* one byte */
#define RAC3_HEALTH_XP         0x00C1E510u
#define RAC3_CURRENT_ARMOR     0x00C1E51Cu  /* u16 */
#define RAC3_QUICK_SELECT      0x00C1E652u  /* one byte, quick-select pause */
#define RAC3_CC_FAKE_ITEM_A    0x00C1E660u  /* CC early: 0x41 */
#define RAC3_CC_FAKE_ITEM_B    0x00C1E664u  /* CC early: 0x41414141 */

#define RAC3_CB_PRIMARY_FRONT  0x00C33AB0u
#define RAC3_CB_PRIMARY_BACK   0x00C33AB4u
#define RAC3_CB_TINT_FRONT     0x00C33AC0u
#define RAC3_CB_TINT_BACK      0x00C33AC4u

#define RAC3_KLUNK_TUNING_2    0x00C36BCCu
#define RAC3_VID_COMIC_MENU    0x00C4F918u
#define RAC3_KLUNK_TUNING_1    0x00C9165Cu
#define RAC3_DROPSHIP_HEALTH   0x00C956A0u

/*
 * Build 53: the trophy reset (rac3_panel.c, and sf_trophy_reset in the savefile
 * helper, src/games/sfhelper/sf_rac3.h, which has the object's layout).
 *
 * The old trainer wrote 1 to the word at 0xD9E020 after deleting
 * /dev_hdd0/home/<user>/trophy/NPWR02347_00 over Ratchetron; the rc3-save mod
 * read it. qwark's request is the helper's fourth byte, `api_trophy` in its
 * table (0xD9FF1C), and qwark does the delete itself, on the console.
 *
 * The game's trophy object pointer, and in the object the word that is 1 while
 * its register thread runs and 2 while its unlock thread does. The reset is
 * refused while either runs.
 */
#define RAC3_TROPHY_OBJ_PTR    0x00CB150Cu
#define RAC3_TROPHY_THREAD     0x188u

/*
 * Where the game keeps the NP communication id it registers its trophies under:
 * the word here points at a block (0xD05CFC, set at 0x15870) whose +4 is the
 * SceNpCommunicationId 0x984FA0 hands sceNpTrophyCreateContext, copied there by
 * 0x984768 out of the executable's table of the four games' ids (UYA's is
 * NPWR02347, number 0). The console keeps that trophy set's data, the user's
 * unlocks included, in /dev_hdd0/home/<user>/trophy/<id>_<number>, which is
 * how qwark finds the one folder it deletes rather than naming it.
 */
#define RAC3_TROPHY_COMMID_PTR 0x00CBAA04u
#define RAC3_TROPHY_COMMID_OFF 4u
#define RAC3_TROPHY_COMMID_LEN 12u

/*
 * rc3-save's third ASM word: the register thread's store of each trophy's
 * unlocked flag into the game's cache (0x9850E0's `stw r4,12(r7)`), made a nop,
 * so a registration leaves the cache as the reset set it. qwark writes it on a
 * console just before it asks for a reset, never under RPCS3, and accepts the
 * site holding either word and nothing else.
 */
#define RAC3_TROPHY_FILL_SITE  0x00985318u
#define RAC3_TROPHY_FILL_WORD  0x9087000Cu
#define RAC3_TROPHY_FILL_NOP   0x60000000u

/* What qwark writes in `api_trophy`; sf_rac3.h has the helper's side. */
#define RAC3_TROPHY_NOW        1u
#define RAC3_TROPHY_IN_LOAD    2u
#define RAC3_TROPHY_RUNNING    3u

#define RAC3_COORDS            0x00DA2870u  /* f32 x, y, z (Vec4) */
#define RAC3_GHOST_TIMER       0x00DA29DEu
#define RAC3_PLAYER_STATE      0x00DA4DB4u
#define RAC3_PLAYER_HEALTH     0x00DA5040u
#define RAC3_SKILL_POINTS      0x00DA521Du  /* 30 bytes */
#define RAC3_AMMO_ARRAY        0x00DA5240u  /* + (ammo offset - 0x243) */
#define RAC3_SHIP_COLOUR       0x00DA55E8u  /* one byte */
#define RAC3_UNLOCK_ARRAY      0x00DA56ECu  /* + (unlock offset - 0x4A8) */
#define RAC3_EXP_ARRAY         0x00DA5824u  /* + (exp offset - 0x5F0) */
#define RAC3_VID_COMICS        0x00DA650Bu  /* five bytes, also in the unlock array */
#define RAC3_FILE_TIME         0x00DA64E0u  /* the IGT the old form edited */

#define RAC3_TITANIUM_BOLTS    0x00ECE53Du  /* 128 bytes */
#define RAC3_LEVEL_FLAGS       0x00ECE675u  /* 0x10 per planet, planet ids are 1-based */
#define RAC3_LOAD_PLANET       0x00EE9310u  /* request word, then the planet id */
#define RAC3_DEST_PLANET       0x00EE9314u
#define RAC3_GAME_STATE        0x00EE9334u
#define RAC3_NEFFY_TUNING      0x00EF6098u

#define RAC3_MOBY_TABLE        0x00F22260u
#define RAC3_MOBY_TABLE_END    0x00F22268u
#define RAC3_MOBY_STRIDE       0x100u

#define RAC3_FAST_LOAD_1       0x0134EBD4u  /* set to 3 to force the third screen */
#define RAC3_FAST_LOAD_2       0x0134EE70u  /* then 0x0101 a fifth of a second later */
#define RAC3_CC_HELP_DESK      0x0148A100u

#define RAC3_AMMO_INSTR        0x00182A88u  /* the ammo decrement; the fingerprint */

/*
 * The savefile helper's byte. qwark embeds the helper and installs it on the
 * first request of a session (src/core/savefile.c), and it writes 1 here on
 * every call, so this reads 1 as soon as the game has reached the hook once.
 *
 * 0xD9FF01 and 0xD9FF02 beside it are the load and set-aside requests, driven
 * through the shared table in src/games/sfhelper_bins.c. 0xD9FF03 and 0xD9FF04
 * were Gigahelper's tempsave writer and reader; nothing writes a file any more,
 * so the "Save manager: load file" row that drove one of them is retired.
 */
#define RAC3_SF_HELPER         0x00D9FF00u

/*
 * Build 53: what the savefile's readiness test reads (rac3_savefile_ready).
 *
 * The save manager is the block RAC3_SAVEDATA_INFO points at, and its save
 * buffer is the word at +4 - as in RaC2, the block is not there until the
 * getter 0x976FE8 first runs, and +4 is 0 until the game's first save or load
 * through the save-data utility in this process. A save starts with the sizes
 * of its global block and of one planet's block; the restore (0x1DF5CC) takes
 * those or the older pair beside them, RaC2's, and nothing else.
 *
 * A load goes from the pause, which the helper asks the game for itself from
 * any single-player mode but space (RAC3_GAME_STATE 6, the transition a planet
 * load runs) and debug (-1), which the mode change will not leave; with no
 * planet load already on its way (RAC3_LOAD_PLANET). The modes past 9 are the
 * lobby and multiplayer. src/games/sfhelper/sf_rac3.h has the modes.
 */
#define RAC3_SAVEDATA_INFO          0x00CB0A98u
#define RAC3_SAVEDATA_FIELD         4u
#define RAC3_SAVE_HEAD_GLOBAL       0x37B0u
#define RAC3_SAVE_HEAD_GLOBAL_OLD   0x1CF8u
#define RAC3_SAVE_HEAD_PLANET       0x84C0u
#define RAC3_SAVE_HEAD_PLANET_OLD   0x0CD0u
#define RAC3_GAME_MODE_SPACE        6u
#define RAC3_GAME_MODE_LAST         9u

#define RAC3_POS_BLOB_LEN  30          /* what racman stores for a position slot */
#define RAC3_LF_LEN        0x10        /* level flag bytes per planet */

/* ------------------------------------------------- autosplit watcher, rev 1.5 */

/*
 * rac3.cs AutosplitterAddresses, in the order rac3-autosplitter.asl reads them.
 * The Marcadia mission byte is in that list only "for backwards compatibility"
 * and nothing reads it; the planet frame count is read by the old client but
 * used by none of the script's conditions, so it is not read here. The
 * loading-screen id is: the script's `update` block subtracted a second of game
 * time whenever it became 1 on a load that was not to or from one of the five
 * planets whose loading screen is a different one that is not actually longer.
 */
#define RAC3_LOADING_SCREEN (RAC3_LOADING_SCREEN_ID + 3)  /* the low byte */
#define RAC3_LOADING_LONG   1        /* the id the script watched for */
#define RAC3_LONG_LOAD_US   1000000u /* the second it took off */
#define RAC3_NEFFY_HEALTH  0x00C4DF80u  /* f32, 1.0 full and 0.0 dead */
#define RAC3_NEFFY_PHASE   0x00DA50FCu  /* u32, odd during a Biobliterator phase */
#define RAC3_LOADED_CHUNK  0x00F08100u  /* rac3.cs loadedChunk, s32 */
/* Tyhrraguise: RAC3_UNLOCK_ARRAY + (0x4C6 - 0x4A8), the way the old list wrote it. */
#define RAC3_TYHRRAGUISE   0x00DA570Au

/* Planet ids the split conditions name. PLANET_LIST index == game planet id. */
#define RAC3_PLANET_VELDIN     1
#define RAC3_PLANET_MARCADIA   4
#define RAC3_PLANET_KOROS      14
#define RAC3_PLANET_LAUNCHSITE 20

#define RAC3_KOROS_BOLT_STATE  0x74   /* playerState while a titanium bolt is taken */
#define RAC3_GAME_STATE_LOAD   6      /* the frame before gameplay resumes */

/*
 * Revision 1.21, the manips' file-load trigger and the trophy auto-reset
 * (rac3.c). RAC3_MODE_TIMER counts the frames spent in the current game mode:
 * the level loop puts it to 0 as it starts (0x1D7078), adds one a frame
 * (0x1D82FC) and puts it back to 0 on a mode change (0x1D8604).
 */
#define RAC3_MODE_TIMER        0x00EE9330u

/* Reason codes. Code 1 is "planet entered" in every game. */
#define R3_AS_PLANET        1
#define R3_AS_LDF           2
#define R3_AS_TYHRRAGUISE   3
#define R3_AS_KOROS_BOLT    4
#define R3_AS_BIOBLITERATOR 5
#define R3_AS_LONG_LOAD     6   /* LOAD_START / LOAD_END, not a split */

/*
 * vars.llIgnorePlanets: the loading screen on these five is a different one, and
 * its "long load" is not actually longer, so the script left those alone. The
 * check is on both ends of the trip, the destination and the origin.
 */
#define RAC3_LL_IGNORE_LAUNCHSITE 20  /* Launch site (ranger ship) */
#define RAC3_LL_IGNORE_METRO      26  /* Metro rangers (ranger ship) */
#define RAC3_LL_IGNORE_AQ_CLANK   27  /* Aquatos Clank (submarine / down the pipe) */
#define RAC3_LL_IGNORE_AQ_SEWERS  28  /* Aquatos sewers (down the pipe / submarine) */
#define RAC3_LL_IGNORE_TYHRRA     29  /* Tyhrranosis rangers (ranger ship) */

/* Aquatos: LoadPlanetSafe skips the fast-load arm for this planet id alone. */
#define RAC3_PLANET_AQUATOS 8

/* rac3.cs FastLoadTimer waits 200 ms; the tick loop runs at 120 Hz. */
#define RAC3_FASTLOAD_DELAY_TICKS 24

/*
 * Fingerprint: the original instruction at the infinite-ammo patch site,
 * 0x7C85312E, which racman restores when the toggle is turned off. qwark's own
 * patched form, the nop, is accepted too.
 *
 * Build 50: and the branch into the freeze-ammo trampoline, which is what the
 * site holds on every boot under RPCS3 once the code switch's patch is in
 * (src/games/switches/sw_rac3.s).
 */
#define RAC3_FP_ADDR RAC3_AMMO_INSTR
extern const u8 rac3_fp[4];
extern const u8 rac3_fp_patched[4];
extern const u8 rac3_fp_switch[4];

/* ---------------------------------------------------------------- hot block */

/* inputs: pad at +0, analogs at +0x1DC */
#define RAC3_HOT_INPUTS_ADDR  RAC3_INPUTS
#define RAC3_HOT_INPUTS_LEN   0x1EC
/* state: QE offset at +0 through the quick-select pause byte at +0x392 */
#define RAC3_HOT_STATE_ADDR   RAC3_QE_OFFSET
#define RAC3_HOT_STATE_LEN    0x394
/* player: the coordinate Vec4 plus the rotation behind it */
#define RAC3_HOT_PLAYER_ADDR  RAC3_COORDS
#define RAC3_HOT_PLAYER_LEN   0x20
/*
 * pstate: player state at +0x02, the health word at +0x28C and the Neffyrious
 * phase at +0x348. Health used to have a slow block of its own; the autosplit
 * watcher needs the other two every tick and they all sit inside one read.
 */
#define RAC3_HOT_PSTATE_ADDR  RAC3_PLAYER_STATE
#define RAC3_HOT_PSTATE_LEN   0x34C
/* load: destination planet at +0x00 (+0x03 its low byte), mode timer +0x1C, game state +0x20 */
#define RAC3_HOT_LOAD_ADDR    RAC3_DEST_PLANET
#define RAC3_HOT_LOAD_LEN     0x24

/* ----------------------------------------------------------- readout slots */

#define RAC3_RO_BOLTS      0
#define RAC3_RO_SAVEFILE   1
#define RAC3_RO_CHALLENGE  2
#define RAC3_RO_HEALTH_XP  3
#define RAC3_RO_HEALTH     4
#define RAC3_RO_ARMOUR     5
#define RAC3_RO_QE_OFFSET  6
#define RAC3_RO_FILE_TIME  7
#define RAC3_RO_SHIP       8
#define RAC3_RO_CB_FRONT   9
#define RAC3_RO_CB_BACK    10
/* Tint front and back mirror each other; one readout and one control cover both. */
#define RAC3_RO_CB_TINT    11

/* ------------------------------------------------------------- feature ids */

/*
 * Stable: new features go on the end, a retired one leaves its number behind.
 * Retired, never to be reused: 4 (freeze Klunk tuning), 17 (make NG+ no-QE
 * file), 28 (give 1337 ammo), 29 (setup NG+ weapons) and 30 (equip the bomb
 * glove), and since build 53 the manip buttons the manip groups replaced
 * (protocol 1.21): 16 (setup NG+ manips), 18 (CC early setup), 19 (untune
 * bosses) and 20 (reset dropship health).
 */
#define R3_FREEZE_AMMO       0
#define R3_FREEZE_HEALTH     1
#define R3_OHKO              2
#define R3_GHOST             3
/* 4 retired */
#define R3_QS_PAUSE          5

#define R3_DIE               6
#define R3_BOLTS             7
#define R3_CHALLENGE         8
#define R3_HEALTH_XP         9
#define R3_HEALTH            10
#define R3_ARMOUR            11
#define R3_SHIP_COLOUR       12
#define R3_FILE_TIME         13
#define R3_QE_OFFSET         14
#define R3_VENDOR_QE         15

/* 16 retired: setup NG+ manips */
/* 17 retired */
/* 18, 19 and 20 retired: CC early setup, untune bosses, reset dropship health */
#define R3_RESET_TROPHIES    21
#define R3_UNLOCK_SKILL      22
#define R3_RESET_SKILL       23
#define R3_UNLOCK_TITANIUM   24
#define R3_RESET_TITANIUM    25
#define R3_UPGRADE_ALL       26
#define R3_DOWNGRADE_ALL     27
/* 28, 29 and 30 retired */

#define R3_SET_ASIDE         31
#define R3_LOAD_ASIDE        32
/*
 * 33 retired: "Save manager: load file" drove Gigahelper's tempsave reader,
 * which protocol 1.9 does away with.
 */

#define R3_CB_PRIMARY_FRONT  34
#define R3_CB_PRIMARY_BACK   35
#define R3_CB_TINT_FRONT     36
#define R3_CB_TINT_BACK      37

/*
 * Build 11. RaC1 and RaC2 have had a Fast loads checkbox all along; this game's
 * was the one thing rac3.cs only ever did as a side effect of its own Load
 * planet button.
 */
#define R3_FAST_LOADS        38

/*
 * Build 53, revision 1.21: the trophy reset's auto toggle, qwark-side state
 * like the per-death toggles, off by default and kept across boots through its
 * auto flag. MANIP_LIST's "Trophies" header names it beside R3_RESET_TROPHIES.
 */
#define R3_AUTO_TROPHIES     39

/* ENUM option counts, from RAC3Form.Designer.cs. */
#define RAC3_ARMOUR_COUNT 8
#define RAC3_SHIP_COUNT   32

/*
 * The highest weapon version any RaC3 item has, and so the maximum the level
 * field advertises in UNLOCK_LIST. The R3YNO stops at 5 and every other
 * levelled weapon goes to 8; UNLOCK_SET clamps to the entry's own `levels`.
 */
#define RAC3_MAX_LEVELS 8

/* ------------------------------------------------------ rac3_panel.c exports */

int rac3_trigger(u8 id);
int rac3_set_value(u8 id, u32 value);
int rac3_get_options(u8 id, const char * const **options, u8 *count);
int rac3_load_setaside(void);
u8  rac3_savefile_ready(u8 what);

int rac3_planet_load(u8 planet, u8 flags);

/*
 * Build 53: the trophy reset. rac3_trophy_reset is R3_RESET_TROPHIES's handler
 * (RAC3_TROPHY_NOW) and what rac3_trophy_auto asks with (RAC3_TROPHY_IN_LOAD);
 * rac3_trophy_auto answers 1 once this load's question is settled and 0 to be
 * asked again on the next tick. rac3_trophy_tick runs the delete and watches
 * the request on every INGAME tick, and rac3_trophy_forget drops whatever was
 * under way when a process ends. All tick thread.
 */
int  rac3_trophy_reset(u8 request);
int  rac3_trophy_auto(void);
void rac3_trophy_tick(void);
void rac3_trophy_forget(void);

int rac3_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac3_levelflags_reset(u8 planet);
int rac3_levelflags_set(u8 planet, u16 offset, u8 value);

int rac3_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac3_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac3_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac3.c exports */

u8  rac3_planet_count(void);

/*
 * rac3.cs SetFastLoads: writes fastLoad1 now and arms fastLoad2 for on_tick.
 * The planet load calls it as LoadPlanetSafe did, and while the Fast loads
 * toggle is on rac3.c calls it again around every planet load of the game's own.
 */
int rac3_arm_fast_loads(void);

/*
 * Vendor QE: the two bytes 00 17 at RAC3_QE_OFFSET. Shared by the "Enable
 * vendor QE" action and the NG+ manip groups' setup.
 */
extern const u8 rac3_vendor_qe[2];

#endif /* QWARK_RAC3_H */
