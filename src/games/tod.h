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
#define TOD_NOBJ_LISTED      12u          /* OBJ_ 0..11 on the Unlocks panel; 12..14 have no use there (tod_panel.c) */
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

/*
 * The old RaCMAN's god ratchet: g_heroProxyMobyHandle, the remote-controlled
 * proxy moby's handle; while the word is non-zero Hero_Update drops the
 * hero's damage (dec). The old trainer wrote one byte, 154, into its low byte
 * (TOD_PROXY_LOW) and 0 there to turn it off (TOD_LEGACY).
 */
#define TOD_PROXY_HANDLE     0x1020BD48u  /* g_heroProxyMobyHandle, u32 */
#define TOD_PROXY_LOW        0x1020BD4Bu  /* its low byte, the one the old trainer wrote */
#define TOD_PROXY_FAKE       154u

/*
 * g_levelTransitionMinTime, f32 seconds (the memory panel's row). Fast loads
 * wrote 0 here until 2026-10-28, which did nothing on a planet load: in
 * LevelTransition_waitLoad (0x3C5810) the wait is only for a load that plays
 * a story FMV instead of the flight (g_levelTransitionFmvBuffer 0x1037AB28 set:
 * Metropolis and Cobalia on a file that has not seen their intros), and there
 * it holds back the start of the load while the FMV plays on regardless (dec).
 */
#define TOD_TRANSITION_MIN   0x10073548u
#define TOD_TRANSITION_GAME  0x40A00000u  /* 5.0f, the shipped value */

/*
 * Fast loads (id 17), a code patch since 2026-10-28 (dec). A planet load's
 * screen is the level_transitions level playing cinematics: the departure
 * (g_loadScreenType 0) when there is a level to fly from, then long-load
 * screens (2 and up) for as long as the data is not in, then the arrival (1).
 * Each one plays to its end; LoadScreen_Update (0x25E560, every frame) starts
 * the next only once the player is idle, and the arrival only once
 * LoadScreen_LoadDone has set g_loadScreenLoadDone, which the loader thread's
 * LevelTransition_waitLoad calls every 200 ms once the data is read. The load
 * is over when LoadScreen_LoadDone finds the arrival (1) with the player idle.
 * So a load lasts at least the departure and the arrival, and the rest of
 * whichever long-load screen is playing when the data comes in.
 *
 * The patch is seventeen words of LoadScreen_Update from 0x25E588, the
 * `bne cr7` that returns while a cinematic plays, on: the arrival up (1),
 * as the game; the load done, the cut; otherwise as the game (a cinematic
 * playing returns, with none the next long-load screen is picked). The cut:
 * the screen black at once (0x25ED38(1.0), what Level_Enter does for the
 * planet), the cinematic stopped as a level's exit stops it (0x84EF4), the
 * player reset as Level_StartGameplay resets it (0x84E28), and 1 in
 * g_loadScreenType; LoadScreen_LoadDone's next look finds the load over, and
 * no arrival plays. The words after them, the arrival's own start, are no
 * longer reached; 0x25E5F4 (the long-load screen's play) and on are untouched.
 * The originals are the game's own (patch_def keeps them).
 *
 * Under RPCS3 the code switch does the same (src/games/switches/sw_tod.s,
 * SW_TOD_FLAG_FAST_LOADS): its trampoline's second half is these words.
 */
#define TOD_FASTLOAD_AT      0x0025E588u  /* LoadScreen_Update + 0x28 */
#define TOD_FASTLOAD_NWORDS  17u
#define TOD_LOAD_SCREEN_DONE 0x102034FCu  /* g_loadScreenLoadDone, u8 */

/*
 * The save system (docs/tod-savefile.md; Ghidra /ToD/Save/ToDSaveManager).
 * g_saveManager sits right after g_saveData and holds, at +0x1FD08, the image
 * of the last GAME.SAV the game wrote or read: SaveManager_copyForSave copies
 * g_saveData into it before a save, and SaveManager_onLoaded copies it back
 * and calls LoadLevel(level, 1, 0) after a load. SaveSystem_update (end of
 * every GameState_Dispatch, main thread) runs the state machine at +0xE0 (dec).
 */
#define TOD_SAVE_SYSTEM      0x1074D400u  /* g_saveSystem, ToDSaveManager*: g_saveManager (dec) */
#define TOD_SAVE_MANAGER     0x1029C560u  /* g_saveManager, 0xB03F8 bytes (dec) */
#define TOD_SAVE_VTABLE      0x10071C60u  /* vtbl_SaveManager, its first word (dis) */
#define TOD_SM_SAVE_THREAD   0x08u        /* ToDSaveThread: list save, auto save */
#define TOD_SM_LOAD_THREAD   0x20u        /* ToDSaveThread: list load, auto load, the boot check */
#define TOD_SM_DELETE_THREAD 0x38u        /* ToDDeleteThread */
#define TOD_ST_ID            0x08u        /* u64 thread id, TOD_THREAD_NONE while none */
#define TOD_ST_STATE         0x10u        /* s32; a load thread's 5 is "file read", which the poll applies */
#define TOD_SM_STATE         0xE0u        /* s32: 0 not up, 1 idle, 2 saving, 3 loading, 4 deleting */
#define TOD_SM_SLOT          0xECu        /* s32, the slot last saved or loaded, -1 none (autosave needs one) */
#define TOD_NO_SLOT          0xFFFFFFFFu  /* `slot` -1: SaveSystem_request's op 4 then does nothing, autoSavePending left set (dis) */
#define TOD_SM_RESULT        0xF0u        /* s32, what finished: 1 saved, 3 or 4 loaded */
#define TOD_SM_AUTOSAVE      0x104u       /* s32, an autosave asked for while the system was busy */
#define TOD_SM_HEAD_LEN      0x108u       /* everything above, one read */
#define TOD_SAVE_IMAGE       0x102BC268u  /* g_saveManager + 0x1FD08: the aside buffer */
#define TOD_SAVE_SIZE        0x906F0u     /* GAME.SAV, as the stat callback insists */
#define TOD_SAVE_LEVEL_OFF   0x906E8u     /* the save's LEVEL_, what onLoaded loads */
#define TOD_SAVE_CHAL_OFF    0x906ECu     /* challenge mode, at most 1000 */
#define TOD_SS_IDLE          1u
#define TOD_SS_LOADING       3u
#define TOD_SS_HELD          0x5157484Cu  /* qwark's own: SaveSystem_update and _request leave it be (dec) */
#define TOD_LT_READ_OK       5u
#define TOD_THREAD_NONE      0xFFFFFFFFu  /* the low word; the high word is 0 */
#define TOD_THREAD_GONE      0xFFFFFFFEu  /* no thread: sys_ppu_thread_get_priority answers ESRCH (INFER) */
#define TOD_FRONTEND_EXIT    0x1037A458u  /* g_frontEndExitRequested, u8: set by FrontEnd_requestLevel and never cleared (xrefs) */
#define TOD_MAX_CHALLENGE    1000u

/* Lua (REPORT-tod-lua section 2). */
#define TOD_LUA_VM           0x102043CCu  /* g_gameplayLuaVM */
#define TOD_LUA_STATE        0x102043D0u  /* its lua_State*, 0 between levels */
#define TOD_GAME_CLOCK       0x1074DDA8u  /* f32 time scale at +0, game time at +0x14 */

/*
 * The autosplitter (revision 1.28; tod_autosplit.c). Ghidra names,
 * all read in the decompile (dec) unless the mark says otherwise.
 */
#define TOD_FRONT_END        0x1037A459u  /* g_inFrontEnd, u8: 1 from FrontEnd_Enter to FrontEnd_Exit (the title) */
#define TOD_STATE_CREDITS    11           /* g_gameState: GameState_RequestCredits, from Lua_show_credits */
#define TOD_MISSIONS_DONE    0x1021C968u  /* g_saveData+0x10AF8: ToDMissionList[20] by LEVEL_, the completed ones */
#define TOD_MISSION_LIST_LEN 0x7Cu        /* 10 x { u32 label text id, u32 desc text id, u32 flags }, u32 count */
#define TOD_MISSION_STRIDE   0x0Cu
#define TOD_MISSION_COUNT    0x78u
#define TOD_NMISSIONS        10u
#define TOD_LEVEL_METROPOLIS 0            /* LEVEL_METROPOLIS, where a run starts */
#define TOD_LEVEL_FASTOON_II 18           /* LEVEL_FASTOON_II, Tachyon's */

/*
 * Revision 1.29 (pending). The old autosplitter's "long load": game time
 * stopped while the low byte of g_currentLevel (its 0x1029C55B) read 0 and
 * the low byte of g_loadScreenType (its 0x102034FB) read more than 1.
 * g_loadScreenType is the movie the loading screen plays: LoadScreen_Init
 * puts 0 there, or a LoadScreen_PickType pick (2 and up, never 3) when the
 * load has no level to fly from; LoadScreen_Update picks another (2 and up)
 * each time one ends while the load is still going, and 1, the arrival,
 * once LoadScreen_LoadDone has said the load is done. It stays 1 until the
 * next load screen.
 */
#define TOD_LOAD_SCREEN      0x102034F8u  /* g_loadScreenType, s32; its low byte is the legacy's 0x102034FB (dec) */
#define TOD_LONG_LOAD_MIN    2u           /* the legacy's "> 1" */

/*
 * Revision 1.29 (pending): the run's start. MOVIE_METROPOLIS_TACHYON, the
 * level flag the game sets as the first planet is beaten: clear on a new
 * file, set on every save made after Metropolis.
 */
#define TOD_LF_METROPOLIS_TACHYON 185u

/*
 * Revision 1.29 (pending): Tachyon's defeat. g_mobyPool's ToDMoby slots
 * (stride 0x180): the object at +0x40, the handle at +0x64. Tachyon is the
 * object whose vtable is vtbl_TachyonBoss (ToDTachyonBoss); its health
 * component is at +0x20, the health itself at +0x24. TachyonBoss_OnDamage
 * holds the health at each phase's threshold and only ever lets it reach 0
 * on the killing hit, which is where it calls TachyonBoss_SetState(15).
 */
#define TOD_MOBY_POOL        0x1066C300u  /* g_mobyPool, ToDMobyPool: the slots at +0, the highest index at +0x10 (dec) */
#define TOD_MOBY_POOL_MAX    0x10u
#define TOD_MOBY_STRIDE      0x180u
#define TOD_MOBY_OBJECT      0x40u        /* ToDMoby.object */
#define TOD_MOBY_HANDLE      0x64u        /* ToDMoby.handle: a generation in the high half, the index in the low */
#define TOD_MOBY_LOOK_LEN    0x28u        /* +0x40 .. +0x67, the object and the handle in one read */
#define TOD_MOBY_MAX         4096u        /* a pool bigger than this is not the pool */
#define TOD_VTBL_TACHYON     0x10060CB0u  /* vtbl_TachyonBoss (dec) */
#define TOD_TACHYON_HEALTH   0x24u        /* ToDTachyonBoss.health.health, f32 */
#define TOD_TACHYON_STATE    0x762u       /* ToDTachyonBoss.state, u8: 15 once dead */

/*
 * Revision 1.29 (pending): the run's two ends as the community times them.
 *
 * The realtime cinematics: g_moviePlayer (ToDMoviePlayer) plays one level
 * movie at a time, by its index in the level's movie list (MovieTable_build,
 * at every level load: g_movieRecords[index] is the record, whose name is
 * the char* just before it). MoviePlayer_update's states: 0 idle, 1 fading
 * out (it waits for ScreenFade's progress to reach 1.0, black, and then asks
 * for g_gameState 9), 2 playing, 3 fading back in, 4 done (MovieQueue_update
 * then plays what is queued next). g_movieQueue holds what waits, its count
 * at +0x404. A named FMV is not the player's: Movie_requestPlay replaces the
 * top of the game-state stack with state 7 and the FMV's record (ToDFmvRecord,
 * its name the char* at +0).
 */
#define TOD_MOVIE_PLAYER     0x101D4F50u  /* g_moviePlayer, ToDMoviePlayer (dec) */
#define TOD_MP_STATE         0x58u        /* s32 0..4, above */
#define TOD_MP_MOVIE         0x64u        /* s32, the movie's index in g_movieRecords */
#define TOD_MP_LOOK_LEN      0x10u        /* +0x58 .. +0x67: the state and the movie in one read */
#define TOD_MOVIE_QUEUE_N    0x101D4F44u  /* g_movieQueue (0x101D4B40) + 0x404, s32: movies waiting */
#define TOD_MOVIE_RECORDS    0x101D50C4u  /* g_movieRecords, record pointers [0x200] by index; the name at [-4] */
#define TOD_NMOVIES          0x200u
#define TOD_FADE_PROGRESS    0x5B62Cu     /* [TOD_LEVEL_OBJ] + this: f32, 0 clear .. 1.0 black (ScreenFade_GetProgress) */
#define TOD_FADE_BLACK       0x3F800000u  /* 1.0f, MoviePlayer_update's own "faded out" test */
#define TOD_STATE_FMV        7            /* g_gameState: a named FMV (Movie_requestPlay) */
#define TOD_STATE_CINEMATIC  9            /* g_gameState: a realtime cinematic playing */
#define TOD_GS_DEPTH         0x10387DC8u  /* g_gameStateDepth, s32: the top of g_gameStateStack */
#define TOD_GS_STACK         0x10387DCCu  /* g_gameStateStack, ToDGameStateEntry[], 0x30 each: the arg at +0 */
#define TOD_GS_STRIDE        0x30u
#define TOD_GS_MAX_DEPTH     8u           /* ApplyPending pushes no deeper (7 entries fit before g_gameStatePending) */
#define TOD_GS_ID            0x20u        /* ToDGameStateEntry.stateId */
/* The ending: TachyonEnding_update queues the cinematic, then the two FMVs. */
#define TOD_END_CINEMATIC    "l4b_s04_dimx_rt"
#define TOD_END_FMV          "l4b_s05_end1_fmv"
#define TOD_MOVIE_NAME_LEN   17u          /* the longer name and its NUL */

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
#define TF_INFINITE_HEALTH  9      /* "God Ratchet" until 2026-10-27; the label changed, the id did not */
#define TF_UNLOCK_SP        10
#define TF_RESET_SP         11
#define TF_UNLOCK_GB        12
#define TF_RESET_GB         13
/* 14 and 15, the RYNO plans' unlock and reset, are retired, never to be another feature's. */
#define TF_GOD_RATCHET      16     /* the old RaCMAN's, the proxy handle (TOD_PROXY_LOW) */
#define TF_FAST_LOADS       17     /* the load screen's cut, a code patch (TOD_FASTLOAD_AT) */

/* The savefile manager's round (docs/tod-savefile.md): ids 40..47. */
#define TF_SET_ASIDE        40
#define TF_LOAD_ASIDE       41

/*
 * Unlock row ids: WPN_ 1..31 as themselves, then these bases. Since
 * 2026-10-27 the list has OBJ_ 0..11 alone: 32 + 12..14 (the two sector
 * upgrades and the washer) and the skill point and gold bolt rows, 64.. and
 * 128.., are no longer listed, and their ids are reserved.
 */
#define TOD_UL_OBJ          32u    /* + OBJ_, 0..11 */
#define TOD_UL_SP           64u    /* reserved: + SKILLPOINT, 0..59 */
#define TOD_UL_GB           128u   /* reserved: + the gold bolt's index, 0..31 (tod_gold_bolt) */
#define TOD_NGOLD_BOLTS     32u

/*
 * The autosplitter's reason codes (revision 1.28; PROTOCOL.md 8.5).
 * Code 1 is every game's planet split; 3 is the title index's pause pair.
 */
#define TOD_AS_PLANET       1      /* SPLIT, arg the LEVEL_ just loaded */
#define TOD_AS_TACHYON      2      /* SPLIT, Tachyon's health reaching 0 on Fastoon II (revision 1.29) */
#define TOD_AS_QUIT         3      /* PAUSE / RESUME around a quit to the XMB */
#define TOD_AS_LOAD         4      /* LOAD_START / LOAD_END, the old autosplitter's long load (revision 1.29) */
#define TOD_AS_CREDITS      5      /* SPLIT, the credits start */
#define TOD_AS_MISSION      6      /* SPLIT, any mission completed: arg level << 16 | its label id */
#define TOD_AS_END_FADE     7      /* SPLIT, the screen black before the ending cinematic (revision 1.29) */
#define TOD_AS_END_FMV      8      /* SPLIT, the ending FMV l4b_s05_end1_fmv starting (revision 1.29) */
#define TOD_QUIT_PAUSE_US   19950000u  /* the old ToD script's reboot: 19.95 s of game time */

/*
 * What the watcher keeps, a part of tod_game.c's state (the lent area in a
 * module). The `cur_` fields are the hot block's, written by its decode.
 */
struct tod_boss {
	u32 moby;              /* its ToDMoby's address */
	u32 obj;               /* the ToDTachyonBoss */
	u32 handle;            /* the slot's handle when it was found */
	u8  alive;             /* its health read above 0 the last time */
	u8  pad[3];
};
#define TOD_NBOSS           4      /* Fastoon II has two */
#define TOD_SCAN_PER_TICK   4      /* pool slots looked at per tick: the whole pool in about 3 s */

struct tod_as_state {
	u32 cur_lua;           /* the gameplay lua_State*, 0 between levels */
	s32 cur_level;         /* g_currentLevel */
	s32 cur_state;         /* g_gameState */
	u8  cur_front;         /* g_inFrontEnd */
	u8  cur_screen;        /* g_loadScreenType's low byte, the legacy's 0x102034FB */
	u8  primed;
	u8  seen;              /* this process has shown its front end or a level */
	u32 lua;               /* the last tick's */
	s32 state;
	u8  front;
	u8  missions_primed;
	u8  nmissions;
	u8  long_load;         /* the legacy's is_loading, as last evaluated */
	u32 origin;            /* the level the player is on, TOD_AS_NOWHERE when none */
	u32 mission_level;     /* whose completed list `missions` is */
	u32 missions[TOD_NMISSIONS];
	u32 ticks;
	/* Revision 1.29: Tachyon, looked for on Fastoon II only. */
	u16 scan;              /* the next pool slot to look at */
	u8  nboss;
	u8  tachyon_done;      /* split once per level up */
	struct tod_boss boss[TOD_NBOSS];
	/* Revision 1.29: the start at control, the end at the fade and the FMV. */
	u8  start_wait;        /* TOD_SW_*: a new run's RESET is out and its START waits */
	u8  end_primed;        /* the ending's first look at this level up is taken */
	u8  end_fade_done;     /* code 7 is out, or was learnt, this level up */
	u8  end_fmv_done;      /* code 8 the same */
	s32 movie_seen;        /* the player's movie index last named, -1 none */
	u8  movie_is_end;      /* whether that index is TOD_END_CINEMATIC */
	u8  pad2[3];
};
#define TOD_SW_NONE         0      /* no run waiting */
#define TOD_SW_LOADING      1      /* the load is under way: no level up since the RESET */
#define TOD_SW_LEVEL        2      /* Metropolis is up: START on the first tick of control */
#define TOD_AS_NOWHERE      0xFFFFFFFFu

/* tod_autosplit.c */
void tod_autosplit_forget(struct tod_as_state *as);
/* The old autosplitter's is_loading: the level's low byte 0, the load screen's above 1. */
int  tod_long_load(s32 level, u8 screen);
void tod_autosplit_tick(struct tod_as_state *as);
/* The game is going: a long load still open is closed before the quit's PAUSE (revision 1.29). */
void tod_autosplit_quit(struct tod_as_state *as);
const struct autosplit_desc *tod_autosplit_describe(u8 *count);

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
void tod_panel_init(void);   /* once a module: Fast loads' patch */

/*
 * tod_save.c, the savefile manager (docs/tod-savefile.md). The two flagged
 * ACTIONs, which keep the set-aside on the HDD; the core's descriptor and
 * driver for the game's own save system; and a session's first look, which
 * gives back a hold or a load a qwark that went away left in the game.
 */
int  tod_set_aside(void);
int  tod_load_aside(void);
u8   tod_savefile_ready(u8 what);
const struct sf_desc *tod_savefile_desc(u8 game_id);
extern const struct sf_driver tod_sf_driver;
void tod_save_recover(void);

/* The gold bolt `index` (0..31) as its level and its bit: 1, or 0 for none. */
int  tod_gold_bolt(u8 index, u8 *level, u8 *bit);
/* A device's (WPN_ 16..23) full count as f32 bits, what Infinite ammo keeps it at; 0 for none. */
u32  tod_device_max_ammo(u8 wpn);
/* The XP weapon `wpn` (1..15) has at level index `level` (0..9), whole points; 0 for none. */
u32  tod_weapon_xp(u8 wpn, u8 level);

/*
 * The level flags (tod_flags.c, revision 1.28): the game's
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
