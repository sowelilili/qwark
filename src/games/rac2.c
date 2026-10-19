/*
 * Ratchet & Clank 2 (NPEA00386, BCES01503, RC2ILYOOO, BLMODYOOO): addresses,
 * patch words, planets, the fingerprint and the loading-screen watcher. The
 * feature handlers, unlocks and level flags live next door in rac2_panel.c;
 * this file owns the numbers, the hot block and the vtable.
 *
 * RC2ILYOOO and BLMODYOOO (the letter O, not a zero, in both) are repacked PAL
 * builds the community passes around. They are NPEA00386 under another title id,
 * byte for byte where it matters, so they answer the same fingerprint and get
 * the same table. The old client accepted them and this one does too.
 *
 * Every address comes from racman's RaCTrainer/Games/RAC2. Where the old code
 * was ambiguous the comment says what was assumed.
 */
#include "rac2.h"
#include "classic.h"
#include "switches_bins.h"
#include "../core/mem.h"
#include "../core/features.h"
#include "../core/autosplit.h"
#include "../core/savefile.h"
#include "../core/manips.h"

#include <string.h>

/* See rac2.h for what these four are and why all four are accepted. */
const u8 rac2_fp[4]         = { 0x4B, 0xFF, 0xEA, 0x69 };
const u8 rac2_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };
const u8 rac2_fp_switch[4]  = {
	(u8)(SW_RAC2_SITE_FAST_LOADS >> 24), (u8)(SW_RAC2_SITE_FAST_LOADS >> 16),
	(u8)(SW_RAC2_SITE_FAST_LOADS >> 8),  (u8)SW_RAC2_SITE_FAST_LOADS
};
const u8 rac2_fp_switch_old[4] = { 0x4B, 0x64, 0x89, 0x60 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Seven reads every tick and two staggered slow ones:
 *
 *   0  inputs   load screen type +0x07, count +0x0B, pad +0x1E0,       every tick
 *               analogs +0x3BC
 *   1  state    QE offset +0x00, planet +0x170, bolts +0x1C4,          every tick
 *               raritanium +0x1C8, challenge +0x1D6, health XP +0x1D8
 *   2  player   the coordinate Vec4 and the rotation behind it         every tick
 *   3  pstate   player state +0x00, hero type +0x20                    every tick
 *   4  lflags   Endako exit +0x31, Barlow race +0x47, A2 Clank +0xE9   every tick
 *   5  chunk    the current chunk byte                                 every tick
 *   6  yeedil   the Yeedil scene byte                                  every tick
 *   7  savefile the helper's own byte                                   every 8th
 *   8  chargeboot colour words                                        every 8th
 *
 * Blocks 3 to 6 are the autosplit watcher's, and are per-tick because a split has
 * to reach the PC in milliseconds. The two slow blocks sit on phases 0 and 4, so
 * a tick costs seven reads plus at most one: 7.25 on average, eight at worst.
 */
static const struct game_hot_block rac2_hot[] = {
	{ RAC2_HOT_INPUTS_ADDR, RAC2_HOT_INPUTS_LEN, 1, 0 },
	{ RAC2_HOT_STATE_ADDR,  RAC2_HOT_STATE_LEN,  1, 0 },
	{ RAC2_HOT_PLAYER_ADDR, RAC2_HOT_PLAYER_LEN, 1, 0 },
	{ RAC2_HOT_PSTATE_ADDR, RAC2_HOT_PSTATE_LEN, 1, 0 },
	{ RAC2_HOT_FLAGS_ADDR,  RAC2_HOT_FLAGS_LEN,  1, 0 },
	{ RAC2_CHUNK,           1,                   1, 0 },
	{ RAC2_YEEDIL_SCENE,    1,                   1, 0 },
	{ RAC2_SF_HELPER,       1,                   8, 0 },
	{ RAC2_CB_PRIMARY_FRONT, 0x14,               8, 4 }
};

#define HOT_INPUTS   0
#define HOT_STATE    1
#define HOT_PLAYER   2
#define HOT_PSTATE   3
#define HOT_LFLAGS   4
#define HOT_CHUNK    5
#define HOT_YEEDIL   6
#define HOT_SAVEFILE 7
#define HOT_COLOURS  8

#define OFF_LOADTYPE  (RAC2_LOADSCREEN_TYPE  - RAC2_HOT_INPUTS_ADDR)   /* 0x007 */
#define OFF_LOADCOUNT (RAC2_LOADSCREEN_COUNT - RAC2_HOT_INPUTS_ADDR)   /* 0x00B */
#define OFF_PAD       (RAC2_INPUTS           - RAC2_HOT_INPUTS_ADDR)   /* 0x1E0 */
#define OFF_ANALOGS   (RAC2_ANALOGS          - RAC2_HOT_INPUTS_ADDR)   /* 0x3BC */

#define OFF_PLANET     (RAC2_CURRENT_PLANET - RAC2_HOT_STATE_ADDR)     /* 0x170 */
#define OFF_BOLTS      (RAC2_BOLTS          - RAC2_HOT_STATE_ADDR)     /* 0x1C4 */
#define OFF_RARITANIUM (RAC2_RARITANIUM     - RAC2_HOT_STATE_ADDR)     /* 0x1C8 */
#define OFF_CHALLENGE  (RAC2_CHALLENGE_MODE - RAC2_HOT_STATE_ADDR)     /* 0x1D6 */
#define OFF_HEALTH_XP  (RAC2_HEALTH_XP      - RAC2_HOT_STATE_ADDR)     /* 0x1D8 */

#define OFF_CB_BACK    (RAC2_CB_PRIMARY_BACK - RAC2_CB_PRIMARY_FRONT)  /* 0x04 */
#define OFF_CB_TINT    (RAC2_CB_TINT_FRONT   - RAC2_CB_PRIMARY_FRONT)  /* 0x10 */

#define OFF_HERO_TYPE  (RAC2_HERO_TYPE - RAC2_HOT_PSTATE_ADDR)         /* 0x20 */

#define OFF_LF_ENDAKO_EXIT   (RAC2_LF_ENDAKO_EXIT   - RAC2_HOT_FLAGS_ADDR)  /* 0x31 */
#define OFF_LF_BARLOW_RACE   (RAC2_LF_BARLOW_RACE   - RAC2_HOT_FLAGS_ADDR)  /* 0x47 */
#define OFF_LF_ARANOS2_CLANK (RAC2_LF_ARANOS2_CLANK - RAC2_HOT_FLAGS_ADDR)  /* 0xE9 */

/*
 * What the watcher needs and struct game_hot has no room for. Filled by the
 * decode, read by on_tick, so the watcher costs no syscall of its own.
 */
static u8 g_load_count;

/* What the autosplit watcher reads, named the way rac2-autosplitter.asl names it. */
struct rac2_as_state {
	u32 player_state;
	u8  planet;
	u8  chunk;
	u8  clank;
	u8  yeedil_scene;
	u8  endako_exit;
	u8  barlow_entry;
	u8  hero_type;
	u8  load_screen;
};

static struct rac2_as_state g_as;

static void rac2_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *inputs = blocks[HOT_INPUTS];
	const u8 *state  = blocks[HOT_STATE];
	const u8 *player = blocks[HOT_PLAYER];
	int i;

	memset(out, 0, sizeof(*out));

	if (inputs != NULL) {
		g_load_count  = inputs[OFF_LOADCOUNT];
		out->pad_mask = be32_get(inputs + OFF_PAD);
		classic_decode_analogs(inputs + OFF_ANALOGS, out->analog);
		/* The watcher's, but the block is already read: see below. */
		g_as.load_screen = inputs[OFF_LOADTYPE];
	}

	if (state != NULL) {
		out->current_planet = state[OFF_PLANET + 3];
		out->readout[RAC2_RO_BOLTS]      = be32_get(state + OFF_BOLTS);
		out->readout[RAC2_RO_RARITANIUM] = be32_get(state + OFF_RARITANIUM);
		out->readout[RAC2_RO_CHALLENGE]  = state[OFF_CHALLENGE];
		out->readout[RAC2_RO_HEALTH_XP]  = be32_get(state + OFF_HEALTH_XP);
		/*
		 * The QE offset is a signed 16-bit field. The readout carries the raw
		 * halfword and the feature row says SIGNED, 16 bits, so the client is
		 * the one that sign-extends it (protocol 1.7).
		 */
		out->readout[RAC2_RO_QE_OFFSET]  = be16_get(state);
	}

	if (player != NULL) {
		for (i = 0; i < 3; i++) out->pos[i] = bef32_get(player + i * 4);
	}

	/* 1 once the helper has run a frame; SAVEFILE_INFO reports the same byte. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC2_RO_SAVEFILE] = blocks[HOT_SAVEFILE][0];

	if (blocks[HOT_COLOURS] != NULL) {
		const u8 *c = blocks[HOT_COLOURS];
		out->readout[RAC2_RO_CB_FRONT] = classic_cb_to_rgb(be32_get(c));
		out->readout[RAC2_RO_CB_BACK]  = classic_cb_to_rgb(be32_get(c + OFF_CB_BACK));
		out->readout[RAC2_RO_CB_TINT]  = classic_cb_to_rgb(be32_get(c + OFF_CB_TINT));
	}

	/* --------------------------------------------- the autosplit watcher's view */

	g_as.planet = out->current_planet;

	if (blocks[HOT_PSTATE] != NULL) {
		g_as.player_state = be32_get(blocks[HOT_PSTATE]);
		g_as.hero_type    = blocks[HOT_PSTATE][OFF_HERO_TYPE];
	}

	if (blocks[HOT_LFLAGS] != NULL) {
		const u8 *f = blocks[HOT_LFLAGS];
		g_as.endako_exit  = f[OFF_LF_ENDAKO_EXIT];
		g_as.barlow_entry = f[OFF_LF_BARLOW_RACE];
		g_as.clank        = f[OFF_LF_ARANOS2_CLANK];
	}

	if (blocks[HOT_CHUNK] != NULL)  g_as.chunk        = blocks[HOT_CHUNK][0];
	if (blocks[HOT_YEEDIL] != NULL) g_as.yeedil_scene = blocks[HOT_YEEDIL][0];
}

/* --------------------------------------------------------------- patches */

static const struct patch_word rac2_fastload_words[] = {
	{ RAC2_FASTLOAD_INSTR, 0x60000000u }   /* nop the load-screen branch */
};

static const struct patch_word rac2_ammo_words[] = {
	{ RAC2_AMMO_RESET_INSTR, 0x60000000u } /* nop the magazine clamp */
};

/*
 * Build 50. Under RPCS3 both are code switches instead
 * (src/games/switches/sw_rac2.s), chosen by the flag bytes SW_RAC2_FLAG_* name.
 */

static struct patch_def rac2_fastload = {
	"Fast loads", PATCH_KIND_FEATURE, rac2_fastload_words, 1, NULL
};
static struct patch_def rac2_ammo = {
	"Infinite ammo", PATCH_KIND_FEATURE, rac2_ammo_words, 1, NULL
};

/* ---------------------------------------------------------- crash guards */

/*
 * Build 53, the crash guards: the user's crash going from a level with an
 * in-space cutscene to the Jamming Array, read off a console that had faulted
 * at 0x86A18 with r5 = 0x2390000.
 *
 * The transition (0xBEA188) picks the space wad the load plays in from the two
 * planets: Dobbo Orbit and Damosel Orbit (22, 23) take wad 1, the Jamming Array
 * (26) wad 2, either way round, and Aranos 2 (14) wad 3 while a story flag is
 * clear. Only the ordinary trip's branch, wad 0's (0xBEA7B0), empties the list
 * of the ship's part mobys at 0x15D46B0 and builds them afresh in the wad's
 * moby table (0xBDEA8C); the other three create a model of their own (class
 * 4748, 4749 or 4930) and leave the list as it was. A story cutscene queued
 * for the trip (0x15D4670, 0xBEA2A8..0xBEA400) still plays, from 0xBE5470
 * inside the load, and its scene update (0xBE32C0) hands its ship actor, class
 * 3412, to the ship update (0xBDEF18), which moves every part in the list
 * (0x871D8 -> 0x86C18 -> UpdateMobyGrids at 0x868B0).
 *
 * Those parts are the last level's. In the crash, the part at 0x43B6E680 was
 * moby 474 of its own table and had been moved twice; against the space wad's
 * table, some 41 MB further up, its index came to -160611 (r30 0xFFFD8C9D).
 * UpdateMobyGrids keeps a 16-bit moby index per entry of each grid cell: the
 * first move stored it as 0x8C9D, and the second, taking the part out of the
 * cell it was in, looked for 0xFFFD8C9D among sign-extended halfwords. Its
 * search (0x86A10..0x86A24) has no bound, never matched, and read on through
 * the zeroes after the grid's pool until it reached an unmapped page.
 *
 * Fast loads have no part in it: the cutscene runs from 0xBE7400, which the
 * transition calls before the load screen fast loads take out. The Insomniac
 * Museum takes wad 0 like any other planet, so this is not its crash, if it
 * has one.
 *
 * Only a planet change qwark makes gets there - the story never sends the
 * player to those planets with such a cutscene queued - so the guards are no
 * feature: like the savefile helper they are qwark's own, always in and out
 * of DESCRIBE. A console has them written when the session enters the game
 * (rac2_guards_install, from rac2_on_enter); under RPCS3 they are rows of the
 * code switches' patch with no flag byte (src/games/switches/sw_rac2.s), these
 * same words at these same addresses, and the switches' look counts them.
 *
 * Two guards, each a trampoline in the dead span after the code switches':
 *
 *   GRID  UpdateMobyGrids leaves the grid alone for a moby whose index does
 *         not fit a cell's 16-bit entry: one that is not in the moby table,
 *         which it could only ever have half put into the grid and never got
 *         out again. Every moby the table holds is unaffected.
 *   SHIP  the ship update passes over a part that is not inside the moby
 *         table, [*0x15927B0, *0x15927B8), rather than writing its position
 *         and matrix into memory the next level is loading into. Parts the
 *         game built are always inside it.
 *
 * GRID is the one that stops this crash, and the same crash in the parts'
 * later deletion (DeleteMoby at 0xB575EC calls UpdateMobyGrids too) whenever
 * the game next empties the list; SHIP keeps the dead parts from being
 * written to at all.
 */
static const u32 rac2_guard_grid[] = {
	0x7FC00734u,   /* extsh  r0, r30 */
	0x7F80F000u,   /* cmpw   cr7, r0, r30 */
	0x409E000Cu,   /* bne    cr7, 1f */
	0x7F24C814u,   /* addc   r25, r4, r25: the site's own */
	0x4AF45514u,   /* b      0x86964 */
	0x4AF4577Cu    /* 1: b   0x86BD0, the epilogue */
};

static const u32 rac2_guard_ship[] = {
	0x807C0000u,   /* lwz    r3, 0(r28): the part */
	0x3C800159u,   /* lis    r4, 0x159 */
	0x80A427B0u,   /* lwz    r5, 0x27B0(r4): the table */
	0x7F832840u,   /* cmplw  cr7, r3, r5 */
	0x419C0018u,   /* blt    cr7, 1f */
	0x80A427B8u,   /* lwz    r5, 0x27B8(r4): its end */
	0x7F832840u,   /* cmplw  cr7, r3, r5 */
	0x409C000Cu,   /* bge    cr7, 1f */
	0xA07F0034u,   /* lhz    r3, 52(r31): the site's own */
	0x4BA9DE7Cu,   /* b      0xBDF310 */
	0x4BA9E0D8u    /* 1: b   0xBDF570, the next part */
};

#define RAC2_GUARD_GRID_WORDS ((u32)(sizeof(rac2_guard_grid) / sizeof(rac2_guard_grid[0])))
#define RAC2_GUARD_SHIP_WORDS ((u32)(sizeof(rac2_guard_ship) / sizeof(rac2_guard_ship[0])))

/* One trampoline as the big-endian words it is, in a single write. */
static int rac2_guard_cave(u32 addr, const u32 *words, u32 n)
{
	u8 buf[4 * RAC2_GUARD_SHIP_WORDS];
	u32 i;

	if (n > RAC2_GUARD_SHIP_WORDS) return ST_BAD_ARG;
	for (i = 0; i < n; i++) be32_put(buf + 4u * i, words[i]);
	return mem_write(addr, buf, 4u * n);
}

/*
 * The guards on a console, once a process, as the session enters it. The
 * trampolines go in first and the branches to them after, so nothing can jump
 * to code that is not there yet, and a trampoline that fails to go in leaves
 * its site alone. Nothing takes them out again, as nothing takes the savefile
 * helper out: a branch the game may be running at any moment is not something
 * to pull from under it, and with no stale part about neither changes a thing.
 * They are written with plain writes rather than as a patch, so no patch list
 * shows them.
 *
 * Nothing is written where code cannot be patched - under RPCS3 the switches'
 * patch put them in at load or did not - nor when either site holds neither the
 * game's own word nor the guard's branch: another executable, or a mod that has
 * the spot. A process qwark entered before already has them, and is left be.
 */
static void rac2_guards_install(void)
{
	u32 grid = 0, ship = 0;

	if (!plat_can_patch_code()) return;

	if (mem_read_u32(RAC2_GUARD_GRID_SITE, &grid) != ST_OK ||
	    mem_read_u32(RAC2_GUARD_SHIP_SITE, &ship) != ST_OK) return;

	if (grid == RAC2_GUARD_GRID_BRANCH && ship == RAC2_GUARD_SHIP_BRANCH) return;

	if ((grid != RAC2_GUARD_GRID_ORIG && grid != RAC2_GUARD_GRID_BRANCH) ||
	    (ship != RAC2_GUARD_SHIP_ORIG && ship != RAC2_GUARD_SHIP_BRANCH)) {
		plat_log("rac2: crash guards left out, 0x86960 or 0xBDF30C is not the game's own word");
		return;
	}

	if (rac2_guard_cave(RAC2_GUARD_GRID_CAVE, rac2_guard_grid, RAC2_GUARD_GRID_WORDS) != ST_OK ||
	    rac2_guard_cave(RAC2_GUARD_SHIP_CAVE, rac2_guard_ship, RAC2_GUARD_SHIP_WORDS) != ST_OK)
		return;

	if (grid != RAC2_GUARD_GRID_BRANCH &&
	    mem_write_u32(RAC2_GUARD_GRID_SITE, RAC2_GUARD_GRID_BRANCH) != ST_OK) return;
	if (ship != RAC2_GUARD_SHIP_BRANCH &&
	    mem_write_u32(RAC2_GUARD_SHIP_SITE, RAC2_GUARD_SHIP_BRANCH) != ST_OK) return;

	plat_log("rac2: crash guards in");
}

/* Live toggle state the watcher and the planet load need to see. */
static int g_fastload_on;
/*
 * Build 50. Fast loads are on because a load forced them, not because the
 * checkbox is: from rac2_fastload_force until the watcher's restore. On a
 * console that is invisible, since the checkbox is qwark's own bit. With the
 * code switches in, the checkbox is read back out of the flag byte, and while
 * this is set the read-back reports the checkbox rather than the force, so it
 * does not flick on for the length of every planet load.
 */
static int g_fastload_forced;
/*
 * Build 53. The force is a set-aside load's (rac2_load_setaside), whose request
 * the helper can still drop, when no load follows to end it: rac2_on_tick then
 * puts it back. A planet load's request is the game's own word and is never
 * dropped, so a set-aside load's refusal leaves a planet load's force to the
 * end of that load. rac1.c's g_fastload_aside.
 */
static int g_fastload_aside;
static int g_death_bosses;
static int g_death_pbolts;
static u8  g_prev_load_count;
static int g_load_primed;

/*
 * Build 53. savefile_load_refusals as rac2_on_tick last saw it. A set-aside
 * load forces fast loads before its request goes out, and the helper can still
 * drop the request on the game's frame - the game would not pause - when no
 * load follows to put them back.
 */
static u32 g_sf_refusals;

/*
 * Build 53: Freeze health, held at the game's own maximum (rac2.h) rather than
 * at the old form's 42069.
 */
static struct classic_health rac2_freeze_health = {
	RAC2_PLAYER_HEALTH, RAC2_MAX_HEALTH, 0, 0, 0, 0, 0
};

int rac2_death_bosses(void) { return g_death_bosses; }
int rac2_death_pbolts(void) { return g_death_pbolts; }

/*
 * Every path that loads something (a planet, a set-aside file) forced fast loads
 * on in the old client and let the loading-screen watcher put the checkbox back.
 * patch_apply is a no-op when the toggle already applied it, so nothing is
 * captured twice and nothing is written twice.
 *
 * Build 53. Under RPCS3 the flag byte outlives qwark: a qwark-rpcs3 started
 * against a game that is already running finds it however the last one left
 * it, and the checkbox reads it back as it is. A flag at 1 that this process did
 * not force is the user's setting, so the force takes it for the checkbox's
 * rather than its own and the end of the load leaves it up, as RaC1's and
 * RaC3's do. On a console the read is NOT_FOUND.
 */
int rac2_fastload_force(void)
{
	int on = 0;
	int rc;

	if (!g_fastload_forced &&
	    classic_code_read(SW_RAC2_FLAG_FAST_LOADS, &on) == ST_OK && on)
		g_fastload_on = 1;

	rc = classic_code_force(&rac2_fastload, SW_RAC2_FLAG_FAST_LOADS);
	if (rc == ST_OK && !g_fastload_on) g_fastload_forced = 1;
	return rc;
}

/* Build 53: the load is asked for, and the force is its (g_fastload_aside). */
void rac2_fastload_claim(int aside)
{
	g_fastload_aside = aside;
}

int rac2_fastload_forced(void)
{
	return g_fastload_forced;
}

int rac2_fastload_restore(void)
{
	g_fastload_forced = 0;
	g_fastload_aside = 0;
	if (g_fastload_on) return ST_OK;
	return classic_code_toggle(&rac2_fastload, SW_RAC2_FLAG_FAST_LOADS, 0);
}

static void rac2_init(void)
{
	static int done = 0;

	if (done) return;
	done = 1;

	rac2_fastload.originals = patch_pool_alloc(rac2_fastload.count);
	rac2_ammo.originals     = patch_pool_alloc(rac2_ammo.count);
}

/* ------------------------------------------------- the autosplit watcher */

/*
 * rac2-autosplitter.asl, condition for condition. Everything it gates on a
 * *setting* is emitted anyway with its own reason code, because the client owns
 * the settings; everything it gates on game state is a condition below.
 *
 * The script's `update` block subtracted a fixed slice of game time every time
 * the load-screen byte changed, keyed on the value it changed *to*: 1 frame for
 * 0, 9 for 1, 21 for 3, nothing for anything else. Those are three LOAD_START
 * codes with the FLAT flag, so the client applies the same three subtractions at
 * the same three moments. Nothing closes them: the script never timed the load,
 * it only paid a toll on entering one.
 */
static struct rac2_as_state g_as_prev;
static int g_as_primed;

#define RAC2_START_STATE 98   /* playerState when the file starts on Aranos */
#define RAC2_FLAG_SET    128  /* what the three level-flag bytes read once set */
#define RAC2_YEEDIL_PROTO 6   /* the Protopet cutscene's scene id */

static void rac2_autosplit_tick(void)
{
	const struct rac2_as_state *p = &g_as_prev;

	/* LiveSplit's first update has old == current; prime and wait a tick. */
	if (!g_as_primed) {
		g_as_prev = g_as;
		g_as_primed = 1;
		return;
	}

	/*
	 * The script's `update` block runs before start, reset and split, so the load
	 * transition is emitted first and the client's subtraction lands before
	 * anything else this tick can take a split.
	 */
	if (g_as.load_screen != p->load_screen) {
		switch (g_as.load_screen) {
		case RAC2_LOADSCREEN_SLIDE:
			autosplit_emit(AUTOSPLIT_LOAD_START, R2_AS_LOAD_SLIDE, 0);
			break;
		case RAC2_LOADSCREEN_CURVED:
			autosplit_emit(AUTOSPLIT_LOAD_START, R2_AS_LOAD_CURVED, 0);
			break;
		case RAC2_LOADSCREEN_WIPE:
			autosplit_emit(AUTOSPLIT_LOAD_START, R2_AS_LOAD_WIPE, 0);
			break;
		default:
			/* The script's `norm == 0.0` case: a change that costs nothing. */
			break;
		}
	}

	/*
	 * start and reset are the same expression in this script. Emit both and let
	 * the client apply whichever suits its timer.
	 */
	if (g_as.planet == RAC2_PLANET_ARANOS &&
	    g_as.player_state == RAC2_START_STATE && p->player_state == 0) {
		autosplit_emit(AUTOSPLIT_RESET, 0, 0);
		autosplit_emit(AUTOSPLIT_START, 0, 0);
	}

	/*
	 * Never split entering the Insomniac Museum, and never entering Aranos.
	 *
	 * Aranos is where a run begins, and the planet byte gets there first: a new
	 * game loads Aranos, so the planet changes to 0 seconds before the player
	 * state flips to 98 and the reset above fires. The script split on that, and
	 * the run it was about to throw away got a bogus last segment out of it; the
	 * RaC1 script, whose Veldin is planet 0, excluded its own start planet from
	 * exactly this test, and the RaC3 script says in a comment that it does. A
	 * load to the start planet is the beginning of a run, not a planet change.
	 */
	if (g_as.planet != p->planet && g_as.planet != RAC2_PLANET_MUSEUM &&
	    g_as.planet != RAC2_PLANET_ARANOS)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_PLANET, g_as.planet);

	if (g_as.planet == RAC2_PLANET_MAKTAR && g_as.chunk == 1 && p->chunk == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_MAKTAR_ARENA, 0);

	if (g_as.planet == RAC2_PLANET_ARANOS2 &&
	    g_as.clank == RAC2_FLAG_SET && p->clank == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_A2_CLANK, 0);

	if (g_as.planet == RAC2_PLANET_BARLOW &&
	    g_as.barlow_entry == RAC2_FLAG_SET && p->barlow_entry == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_BARLOW_RACE, 0);

	/* The entry split is the hero type, not the flag byte the old client read. */
	if (g_as.planet == RAC2_PLANET_ENDAKO &&
	    g_as.hero_type == 1 && p->hero_type == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_ENDAKO_ENTER, 0);

	if (g_as.planet == RAC2_PLANET_ENDAKO &&
	    g_as.endako_exit == RAC2_FLAG_SET && p->endako_exit == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_ENDAKO_EXIT, 0);

	if (g_as.planet == RAC2_PLANET_TABORA && g_as.chunk == 0 && p->chunk == 1)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_TABORA_CAVES, 0);

	if (g_as.planet == RAC2_PLANET_YEEDIL &&
	    g_as.yeedil_scene == RAC2_YEEDIL_PROTO &&
	    p->yeedil_scene != RAC2_YEEDIL_PROTO)
		autosplit_emit(AUTOSPLIT_SPLIT, R2_AS_PROTOPET, 0);

	g_as_prev = g_as;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define FL AUTOSPLIT_FLAG_FLAT

/*
 * The script's settings.Add list, in its order; DF is what it defaults to true.
 * Then the three load transitions, which are not splits and not optional: the
 * script subtracted their frames whatever the settings said, and so does the
 * client. The Protopet row carries the seven frames the script took off just
 * before it returned true for that split.
 */
static const struct autosplit_desc rac2_autosplits[] = {
	{ R2_AS_PLANET,       AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ R2_AS_PROTOPET,     AUTOSPLIT_SPLIT,      DF | FL, RAC2_PROTOPET_US,
	  "Protopet defeated" },
	{ R2_AS_A2_CLANK,     AUTOSPLIT_SPLIT,      DF,      0, "Aranos 2 Clank swap" },
	{ R2_AS_MAKTAR_ARENA, AUTOSPLIT_SPLIT,      0,       0, "Maktar arena entry" },
	{ R2_AS_BARLOW_RACE,  AUTOSPLIT_SPLIT,      0,       0, "Barlow race entry" },
	{ R2_AS_ENDAKO_ENTER, AUTOSPLIT_SPLIT,      0,       0, "Endako Clank entry" },
	{ R2_AS_ENDAKO_EXIT,  AUTOSPLIT_SPLIT,      0,       0, "Endako Clank exit" },
	{ R2_AS_TABORA_CAVES, AUTOSPLIT_SPLIT,      0,       0, "Tabora caves" },
	{ R2_AS_LOAD_SLIDE,   AUTOSPLIT_LOAD_START, DF | FL, RAC2_LOAD_SLIDE_US,
	  "Load transition: slide" },
	{ R2_AS_LOAD_CURVED,  AUTOSPLIT_LOAD_START, DF | FL, RAC2_LOAD_CURVED_US,
	  "Load transition: curved" },
	{ R2_AS_LOAD_WIPE,    AUTOSPLIT_LOAD_START, DF | FL, RAC2_LOAD_WIPE_US,
	  "Load transition: wipe" }
};

#undef DF
#undef RT
#undef FL

static const struct autosplit_desc *rac2_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(rac2_autosplits) / sizeof(rac2_autosplits[0]));
	return rac2_autosplits;
}

static void rac2_on_enter(void)
{
	/* A fresh process: nothing is forced, nothing has been seen yet. */
	g_fastload_on     = 0;
	g_fastload_forced = 0;
	g_fastload_aside  = 0;
	g_prev_load_count = 0;
	g_load_primed     = 0;
	g_sf_refusals     = savefile_load_refusals();

	/*
	 * The toggles that are qwark's own state rather than game memory go with
	 * the process, as their bits in toggle_state do (features_forget_state
	 * runs just before this): the two per-death resets and, build 53, Freeze
	 * health's tracking of the maximum. Only the auto-apply that follows this
	 * call and PREVIOUS_REAPPLY turn them back on, both through rac2_set_toggle,
	 * so the flag and the checkbox agree.
	 */
	g_death_bosses = 0;
	g_death_pbolts = 0;
	classic_health_forget(&rac2_freeze_health);

	memset(&g_as, 0, sizeof(g_as));
	memset(&g_as_prev, 0, sizeof(g_as_prev));
	g_as_primed = 0;

	/* Build 53: the crash guards, on a console; see rac2_guards_install. */
	rac2_guards_install();
}

/*
 * RAC2Form_Load's loading-screen watcher, plus the autosplit watcher. The
 * loading-screen half fires once per change of the load counter, acts only on
 * the final load screen, and only the A1 (planet 0) part of it is conditional
 * on the planet.
 *
 * What the counter is, from the NPEA00386 executable. It is the low byte of the
 * word at 0x147A258, written by the load-screen code at 0xBE5470, which the
 * planet transition loop (0xBEA868..0xBEA8FC) calls once a frame.
 * Early in every load it is set to 0 and held there while the level loader's
 * stage word (0x147A1D4) is below 2, which is as long as engine.ps3 and
 * vram.ps3 take to read; then it is 1 for one frame and 2 on the next, and the
 * transition loop ends. It stays 2 until the next load. The current planet
 * word is set to the destination before that loop starts (0xBEA6D4), so an A1
 * load reads planet 0 here.
 *
 * Two goes at 2 when the load SCREEN is done, not the load: the loader goes on
 * to sound.bnk and the chunk banks, and the level is set up after that. The
 * old client wrote at the same point plus its network round trips, which is
 * the timing the runners know, and it is kept: moving the resets past the
 * level setup could land them after a new file's opening cutscene has begun,
 * and one of them clears the cutscene-storage word.
 */
static void rac2_on_tick(const struct game_hot *hot)
{
	u8 count = g_load_count;

	classic_health_tick(&rac2_freeze_health);
	rac2_autosplit_tick();

	/*
	 * Build 53: a set-aside load the helper dropped puts the forced fast loads
	 * back - its own force, not a planet load's (g_fastload_aside).
	 */
	if (savefile_load_refusals() != g_sf_refusals) {
		g_sf_refusals = savefile_load_refusals();
		if (g_fastload_forced && g_fastload_aside) rac2_fastload_restore();
	}

	/*
	 * The first look of a process only learns where the counter stands. A 2
	 * here is a load that ended before qwark was watching - the module loaded
	 * into a game already sitting on Aranos - and treating it as a change wrote
	 * the A1 block at attach: the buffer charge, infinite ammo off, and the
	 * manip resets of any toggle flagged on boot. A load that is still under
	 * way reads 0 or 1 here and fires when it reaches 2, as it always did.
	 */
	if (!g_load_primed) {
		g_prev_load_count = count;
		g_load_primed = 1;
		return;
	}

	if (count == g_prev_load_count) return;
	g_prev_load_count = count;

	if (count != 2) return;

	/*
	 * The load is over: put fast loads back where the toggle wants them. Build
	 * 53: only when a load forced them. The end of a load nothing forced writes
	 * nothing, which under RPCS3 put the fast-load flag down under a checkbox
	 * that reads it as on.
	 */
	if (g_fastload_forced) rac2_fastload_restore();

	/* Everything below is A1-only, exactly as the old handler had it. */
	if (hot->current_planet != 0) return;

	/* A new file starts without infinite ammo, checkbox and all. */
	features_set(R2_INFINITE_AMMO, 0);

	/*
	 * Revision 1.21, the manips' file-load trigger: the end of a load screen
	 * landing on Aranos, the first planet, which every run starts on - a new
	 * game, a challenge-mode start, the NG+ file a runner loads - and which a
	 * run never comes back to. It is the timing the old auto-resets had. The
	 * challenge counter is in place by then (from the NPEA00386 executable):
	 * the restore 0x81070 writes its block back from inside perform_load
	 * (0x832DC) before the load is requested (0x83478), a new game's reset
	 * (0x81224, from 0x817E8) zeroes it at 0x812D4 on the title, and a
	 * challenge-mode start (0xBBB798) stores the incremented value back at
	 * 0xBBBBC0, all before the load screen starts. Nothing found in the level
	 * start after it (0x12244, 0xB4C8E8) writes a manip address.
	 *
	 * A load into Aranos from anywhere else in a run also lands here, which is
	 * the user's rule (2026-10-16): Aranos is not revisited. The buffer charge
	 * the old watcher wrote here unconditionally is the NG+ per-run list's now.
	 */
	manips_file_loaded();
}

/* -------------------------------------------------------------- describe */

static const char * const rac2_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Collectables",
	"Savefile",
	"Cosmetics"
};

#define G_CHEATS       0
#define G_PLAYER       1
#define G_PROGRESS     2
#define G_COLLECTABLES 3
#define G_SAVEFILE     4
#define G_COSMETICS    5

/* Indexed by RAC2_RO_*; the order is part of the wire contract for this game. */
static const char * const rac2_readouts[] = {
	"Bolts",
	"Savefile helper",
	"Raritanium",
	"Challenge mode",
	"Health XP",
	"QE save offset",
	"Chargeboot front",
	"Chargeboot back",
	"Chargeboot tint"
};

#define NR (u8)(sizeof(rac2_readouts) / sizeof(rac2_readouts[0]))
#define NO FEATURE_NO_READOUT
#define WC FEATURE_FLAG_WRITES_CODE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE
/* Protocol 1.3: a toggle qwark reads back out of game memory. */
#define LV FEATURE_FLAG_LIVE
/* Protocol 1.7: the field behind this VALUE is two's complement, `bits` wide. */
#define SG FEATURE_FLAG_SIGNED

static const struct feature_desc rac2_features[] = {
	/* id, kind, group, aux (ENUM: options; VALUE: field bits), flags, readout, min, max, label */
	/*
	 * Build 53: the four cheats every game has lead Cheats, in the same order
	 * and spelt alike in all four but for Deadlocked's Refill ammo (the client
	 * draws them in this order).
	 */
	{ R2_FAST_LOADS,      FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ R2_GHOST,           FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },
	{ R2_INFINITE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite ammo" },
	{ R2_FREEZE_HEALTH,   FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Freeze health" },
	/* Build 53: weapon insta-upgrades is retired, id 4 and all (rac2.h). */
	{ R2_DEBUG_MODE,      FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "Enable debug mode" },

	{ R2_DIE,             FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ R2_BOLTS,           FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_BOLTS,      0, 0, "Bolts" },
	{ R2_RARITANIUM,      FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_RARITANIUM, 0, 0, "Raritanium" },
	{ R2_CHALLENGE,       FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC2_RO_CHALLENGE,  0, 255, "Challenge mode" },
	/* Health XP is a signed 32-bit field, so the row says so and stays unbounded. */
	{ R2_HEALTH_XP,       FEATURE_VALUE,  G_PLAYER,  32, SG, RAC2_RO_HEALTH_XP,  0, 0, "Health XP" },
	{ R2_SET_RESPAWN,     FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Set respawn point" },
	{ R2_STORE_SWINGSHOT, FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Store Swingshot" },
	{ R2_DEATH_BOSSES,    FEATURE_TOGGLE, G_PLAYER,   0, 0,  NO, 0, 0, "Reset bosses on death" },
	{ R2_DEATH_PBOLTS,    FEATURE_TOGGLE, G_PLAYER,   0, 0,  NO, 0, 0, "Reset platinum bolts on death" },

	/*
	 * Build 53: the two resets, the three NG+ setups, Maktar slots and the two
	 * auto-reset toggles are retired, ids 15 to 22 and all (rac2.h). They are
	 * the manip groups below now.
	 */
	/*
	 * The two weapon actions the other three games have. Both pass over a weapon
	 * the player does not have rather than handing it out, and the labels are the
	 * other games' word for word so the client lays them out together.
	 */
	{ R2_MAX_LEVELS,      FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Max all weapon levels" },
	{ R2_MAX_AMMO,        FEATURE_ACTION, G_PROGRESS, 0, 0,  NO, 0, 0, "Max all weapon ammo" },
	/*
	 * The QE offset is the signed halfword at selectedSaveSlot, and the old
	 * dialog's default was -1. min and max stay 0: the width is the range.
	 */
	{ R2_QE_OFFSET,       FEATURE_VALUE,  G_PROGRESS, 16, SG, RAC2_RO_QE_OFFSET, 0, 0, "QE save write-offset" },

	{ R2_UNLOCK_PBOLTS,   FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all platinum bolts" },
	{ R2_RESET_PBOLTS,    FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset platinum bolts" },
	{ R2_UNLOCK_NANOTECH, FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all nanotech boosts" },
	{ R2_RESET_NANOTECH,  FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset nanotech boosts" },
	{ R2_UNLOCK_SKILL,    FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Unlock all skill points" },
	{ R2_RESET_SKILL,     FEATURE_ACTION, G_COLLECTABLES, 0, 0, NO, 0, 0, "Reset skill points" },

	/*
	 * Protocol 1.9: one pair, and it is the same pair the PC's save-file manager
	 * drives. The two "Save manager" rows that used to carry the flags wrote a
	 * tempsave file and are retired with their ids.
	 */
	{ R2_SET_ASIDE,       FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ R2_LOAD_ASIDE,      FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" },

	{ R2_CB_PRIMARY_FRONT, FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_FRONT, 0, 0, "Chargeboots primary front" },
	{ R2_CB_PRIMARY_BACK,  FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_BACK,  0, 0, "Chargeboots primary back" },
	{ R2_CB_TINT_FRONT,    FEATURE_COLOR, G_COSMETICS, 0, 0, RAC2_RO_CB_TINT,  0, 0, "Chargeboots tint" }
};

#undef NO
#undef WC
#undef SA
#undef LA
#undef LV
#undef SG

static const struct game_describe rac2_describe_table = {
	rac2_groups,   (u8)(sizeof(rac2_groups) / sizeof(rac2_groups[0])),
	rac2_readouts, NR,
	rac2_features, (u8)(sizeof(rac2_features) / sizeof(rac2_features[0])),
	0,             /* no toggle ships auto-flagged */
	NULL, 0        /* and none was relabelled */
};

#undef NR

static const struct game_describe *rac2_describe(void)
{
	return &rac2_describe_table;
}

/* --------------------------------------------------------------- toggles */

/*
 * freezeAmmoCheckbox did two things: filled 136 bytes of the ammo array with
 * 0x7FFFFFFF and nopped the decrement. Turning it off only restored the
 * instruction, so the ammo stays where it was put; that is kept. The 136 bytes
 * are ammo[12] through ammo[45] now that the array has a base and a stride, and
 * the window is the one the old checkbox wrote and no wider.
 */
static int rac2_infinite_ammo(int on)
{
	u8 full[RAC2_AMMO_FILL_LEN];
	u32 i;
	int rc;

	if (!on) return classic_code_toggle(&rac2_ammo, SW_RAC2_FLAG_INFINITE_AMMO, 0);

	for (i = 0; i < sizeof(full); i += 4) be32_put(full + i, 0x7FFFFFFFu);

	rc = mem_write(RAC2_AMMO_ARRAY + 4u * RAC2_AMMO_FILL_FIRST, full, sizeof(full));
	if (rc != ST_OK) return rc;

	return classic_code_toggle(&rac2_ammo, SW_RAC2_FLAG_INFINITE_AMMO, 1);
}

static int rac2_set_toggle(u8 id, int on)
{
	int rc;

	switch (id) {
	case R2_FAST_LOADS:
		rc = classic_code_toggle(&rac2_fastload, SW_RAC2_FLAG_FAST_LOADS, on);
		if (rc == ST_OK) {
			g_fastload_on = on;
			/* Whatever a load forced, the checkbox has the say now. */
			g_fastload_forced = 0;
			g_fastload_aside = 0;
		}
		return rc;

	case R2_INFINITE_AMMO:  return rac2_infinite_ammo(on);
	case R2_FREEZE_HEALTH:  return classic_health_toggle(&rac2_freeze_health, on);
	case R2_GHOST:          return classic_ghost(RAC2_GHOST_TIMER, on);
	case R2_DEBUG_MODE:     return mem_write_u8(RAC2_DEBUG_FEATURES, on ? 1 : 0);

	/* The last two are qwark-side state, not memory. */
	case R2_DEATH_BOSSES:   g_death_bosses = on; return ST_OK;
	case R2_DEATH_PBOLTS:   g_death_pbolts = on; return ST_OK;

	default:                return ST_NOT_FOUND;
	}
}

/*
 * Protocol 1.3. Debug mode is one byte the game owns, so its checkbox follows
 * memory rather than the last thing qwark wrote. Tick thread only.
 *
 * Build 50: and the two code switches' flag bytes, while the switches are in.
 */
static int rac2_toggle_read(u8 id, int *on)
{
	u8 b = 0;
	int rc;

	if (id == R2_FAST_LOADS) {
		rc = classic_code_read(SW_RAC2_FLAG_FAST_LOADS, on);
		/* A load's force is not the checkbox; see g_fastload_forced. */
		if (rc == ST_OK && g_fastload_forced) *on = g_fastload_on;
		return rc;
	}
	if (id == R2_INFINITE_AMMO) return classic_code_read(SW_RAC2_FLAG_INFINITE_AMMO, on);

	if (id != R2_DEBUG_MODE) return ST_NOT_FOUND;

	rc = mem_read_u8(RAC2_DEBUG_FEATURES, &b);
	if (rc != ST_OK) return rc;

	*on = (b != 0);
	return ST_OK;
}

/* -------------------------------------------------------------- positions */

static int rac2_save_blob(u8 *blob, u8 *len)
{
	int rc = mem_read(RAC2_COORDS, blob, RAC2_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = RAC2_POS_BLOB_LEN;
	return ST_OK;
}

static int rac2_load_blob(const u8 *blob, u8 len)
{
	if (len == 0 || len > QWARK_MAX_BLOB) return ST_BAD_ARG;
	return mem_write(RAC2_COORDS, blob, len);
}

static int rac2_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

static int rac2_blob_set_xyz(u8 *blob, u8 len, const f32 xyz[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) bef32_put(blob + i * 4, xyz[i]);
	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

/* rac2.cs planetsList, index by index: the game's own planet numbering. */
static const char * const rac2_planets[] = {
	"Aranos",
	"Oozla",
	"Maktar",
	"Endako",
	"Barlow",
	"Feltzin",
	"Notak",
	"Siberius",
	"Tabora",
	"Dobbo",
	"Hrugis",
	"Joba",
	"Todano",
	"Boldan",
	"Aranos 2",
	"Gorn",
	"Snivelak",
	"Smolg",
	"Damosel",
	"Grelbin",
	"Yeedil",
	"Insomniac Museum",
	"Dobbo Orbit",
	"Damosel Orbit",
	"Slim Cognito",
	"Wupash",
	"Jamming Array"
};

u8 rac2_planet_count(void)
{
	return (u8)(sizeof(rac2_planets) / sizeof(rac2_planets[0]));
}

static const char * const *rac2_planet_names(u8 *count)
{
	*count = rac2_planet_count();
	return rac2_planets;
}

static int rac2_moby_table(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride)
{
	*table_ptr_addr     = RAC2_MOBY_TABLE;
	*table_end_ptr_addr = RAC2_MOBY_TABLE_END;
	*stride             = RAC2_MOBY_STRIDE;
	return ST_OK;
}

/* ----------------------------------------------------------------- manips */

/*
 * Whether a save file is in play: the current planet word is -1 on the title
 * screen (written at 0x11648; the main loop runs the title while it is
 * negative, 0x12D50) and a planet's number from then on. A new game puts 0 in
 * it on the title, just before the Aranos load (0x81898), and a title load the
 * save's planet, so for that moment the title counts as in a file.
 */
static int rac2_in_file(void)
{
	u32 planet = 0;

	if (mem_read_u32(RAC2_CURRENT_PLANET, &planet) != ST_OK) return -1;
	return (planet & 0x80000000u) == 0;
}


/*
 * Revision 1.21, build 53: RaC2's manip groups (SPEC-manips.md section 2),
 * every write in the order the spec lists it, at the width the old buttons
 * wrote. Four of the words sit at odd addresses and so clear the three bytes
 * after them, as they always did.
 *
 * G, the NG+ session block, is RAC2Form.SetupGeneralNGPlusMenus; G' is G
 * without its last write, the IM in the shortcuts.
 */
static const struct manip_write r2_g[] = {
	MW_U8(RAC2_FELTZIN_OPENING, 1),
	MW_U8(RAC2_SNIV_BOSS, 20),
	MW_U32(RAC2_PAD_MANIP, 0x41C80000u),     /* 25.0f, what Snivelak sets */
	MW_U8(RAC2_YEEDIL_BOSS, 66),
	MW_U8(RAC2_SIB_BOSS, 20),
	MW_U32(RAC2_GORN_MANIP, 1),
	MW_U32(RAC2_GORN_OPENING, 1),
	MW_U32(RAC2_IM_SHORTCUTS, 1)
};

#define R2_G_LEN       ((u8)(sizeof(r2_g) / sizeof(r2_g[0])))
#define R2_G_PRIME_LEN ((u8)(R2_G_LEN - 1))

/* NG+ All Missions' two writes after G. */
static const struct manip_write r2_all_missions[] = {
	MW_U32(RAC2_ENDAKO_BOSS_CS, 1),
	MW_U8(RAC2_DORBIT_OPENING, 1)
};

/*
 * M(i), the NG+ per-run block: RAC2Form.resetMenuStorage, with the shortcut
 * cursor always written because the group carries its own index, and the
 * charge buffer, which used to be written on every load into Aranos whatever
 * the category.
 */
#define R2_M(i) {                                                            \
	MW_U32(RAC2_SHORTCUTS_INDEX, (i)),                                   \
	MW_U32(RAC2_SAVED_RACE_INDEX, 0),                                    \
	MW_U8(RAC2_FELTZIN_OPENING, 1),                                      \
	MW_U32(RAC2_GORN_OPENING, 1),                                        \
	MW_U32(RAC2_FELTZIN_MISSION, 0),                                     \
	MW_U32(RAC2_HRUGIS_MISSION, 0),                                      \
	MW_U32(RAC2_GORN_MISSION, 0),                                        \
	MW_U32(RAC2_CHARGE_BUFFER, 30)                                       \
}

static const struct manip_write r2_m1[] = R2_M(1);   /* Barlow */
static const struct manip_write r2_m7[] = R2_M(7);   /* the Museum */

#undef R2_M

/*
 * A, the any% reset: RAC2Form.resetAnyPercentVars, unchanged and in its order.
 * It puts the jump pad back to 13.0f, which Snivelak raises on every visit.
 */
static const struct manip_write r2_a[] = {
	MW_U32(RAC2_SAVED_RACE_INDEX, 0),
	MW_U8(RAC2_FELTZIN_OPENING, 0),
	MW_U32(RAC2_GORN_OPENING, 0),
	MW_U32(RAC2_GORN_MANIP, 0),
	MW_U32(RAC2_FELTZIN_MISSION, 0),
	MW_U32(RAC2_HRUGIS_MISSION, 0),
	MW_U32(RAC2_GORN_MISSION, 0),
	MW_U32(RAC2_JANKPOT_ACTIVE, 0),
	MW_U32(RAC2_BOLT_DEFICIT, 0),
	MW_U32(RAC2_ENDAKO_BOSS_CS, 0),
	MW_U32(RAC2_PYRAMID_BOLT, 0),
	MW_U32(RAC2_OLDSKOOL_SP, 0),
	MW_U32(RAC2_BOSS_HEALTHBAR, 0),
	MW_U32(RAC2_CS_STORAGE, 0),
	MW_U32(RAC2_FELTZIN_RARI, 0),
	MW_U32(RAC2_LAST_RARITANIUM, 0),
	MW_U8(RAC2_DORBIT_OPENING, 0),
	MW_U32(RAC2_MOBY3595_PTR, 0),
	MW_U8(RAC2_SNIV_BOSS, 0),
	MW_U8(RAC2_YEEDIL_BOSS, 0),
	MW_U8(RAC2_SIB_BOSS, 0),
	MW_U32(RAC2_PAD_MANIP, 0x41500000u)      /* 13.0f */
};

/* K, the Maktar slots: a slot-machine RNG manip grandfathered into 100%. */
static const struct manip_write r2_k[] = {
	MW_U8(RAC2_SLOTS_HIT, 40),
	MW_U8(RAC2_PBOLTS, 74),
	MW_U8(RAC2_PJACKPOT, 45)
};

#define L(a)    { a, (u8)(sizeof(a) / sizeof(a[0])) }
#define NONE    { NULL, 0 }
#define G_FULL  { r2_g, R2_G_LEN }
#define G_PRIME { r2_g, R2_G_PRIME_LEN }
#define NG      MANIP_FLAG_NGPLUS
#define EXT     MANIP_FLAG_EXTENSION
#define NOCHECK MANIP_FLAG_NO_CHALLENGE
/* M without its first write, the cursor; the rest of M is the same for any i. */
#define M_NO_CURSOR { r2_m1 + 1, (u8)(sizeof(r2_m1) / sizeof(r2_m1[0]) - 1) }

/*
 * NG+ No Specials runs on an overflowed challenge counter, so it skips the
 * check, and its per-run list is M without the shortcut cursor: the shortcuts
 * live in the specials menu, which the category never uses.
 */
static const struct manip_group rac2_manip_groups[] = {
	{ 1, NG, "NG+ no IMG", "NG+ no IMG",
	  { G_PRIME, NONE }, { L(r2_m1), NONE } },
	{ 2, NG, "NG+", "NG+",
	  { G_FULL, NONE }, { L(r2_m7), NONE } },
	{ 3, 0, "Any%", "Any%\nAll Platinum Bolts",
	  { NONE, NONE }, { L(r2_a), NONE } },
	{ 4, 0, "100%", "Max%\n100%",
	  { NONE, NONE }, { L(r2_a), L(r2_k) } },
	{ 5, NG | EXT, "NG+ All Missions", "NG+ All Missions",
	  { G_FULL, L(r2_all_missions) }, { L(r2_m7), NONE } },
	{ 6, NG | EXT | NOCHECK, "NG+ No Specials", "NG+ No Specials",
	  { G_PRIME, NONE }, { M_NO_CURSOR, NONE } }
};

#undef L
#undef NONE
#undef G_FULL
#undef G_PRIME
#undef NG
#undef EXT
#undef NOCHECK
#undef M_NO_CURSOR

static const struct game_manips rac2_manips = {
	"rac2",
	rac2_manip_groups,
	(u8)(sizeof(rac2_manip_groups) / sizeof(rac2_manip_groups[0])),
	RAC2_CHALLENGE_MODE, 1,    /* the challenge counter, one byte */
	NULL, 0,                   /* no headers */
	rac2_in_file
};

/* ----------------------------------------------------------- console menu */

/*
 * Revision 1.22: the console menu's pages for RaC2 (src/core/menu.c), after
 * rewrite/console-ui-framework.md and the client's gamelayout.json for "rac2",
 * as Deadlocked's and UYA's are. Game is the quick block and the client's
 * Values; Cheats the four cheats every game has, then the rest of the Game
 * panel, its Player section: Set respawn point, Store Swingshot and the two
 * resets on death. Debug mode and the QE offset are the client's Debug page and
 * the chargeboot colours its Cosmetics, which the menu leaves out. The weapon
 * buttons are the client's Unlocks panel buttons and the platinum bolt,
 * nanotech and skill point ones its Collectables tab, beside the unlock table:
 * all eight go on a page of their own, Collectables, one row after the
 * Unlocks page's categories (round 3, as UYA's six). RaC2 has no manip header,
 * so Manips is the category and its options.
 */
static const u8 rac2_menu_game[] = {
	R2_DIE, MENU_ITEM_SLOT, MENU_ITEM_POS_SAVE, MENU_ITEM_POS_LOAD,
	R2_SET_ASIDE, R2_LOAD_ASIDE,
	R2_BOLTS, R2_RARITANIUM, R2_CHALLENGE, R2_HEALTH_XP
};

static const u8 rac2_menu_cheats[] = {
	R2_FAST_LOADS, R2_GHOST, R2_INFINITE_AMMO, R2_FREEZE_HEALTH,
	R2_SET_RESPAWN, R2_STORE_SWINGSHOT, R2_DEATH_BOSSES, R2_DEATH_PBOLTS
};

static const struct menu_section rac2_menu_sections[] = {
	{ "Game",   rac2_menu_game,   (u8)sizeof(rac2_menu_game) },
	{ "Cheats", rac2_menu_cheats, (u8)sizeof(rac2_menu_cheats) }
};

static const u8 rac2_menu_unlock_actions[] = {
	R2_MAX_LEVELS, R2_MAX_AMMO,
	R2_UNLOCK_PBOLTS, R2_RESET_PBOLTS, R2_UNLOCK_NANOTECH, R2_RESET_NANOTECH,
	R2_UNLOCK_SKILL, R2_RESET_SKILL
};

/*
 * An unlock row cycles through Locked, Unlocked and Max, on the level, field
 * 1, which UNLOCK_SET holds to the weapon's own last version; a gadget or an
 * item, which have no level, is Locked or Unlocked, the first two names. The
 * game's menus are in title case, and so is the renderer, which keeps capitals
 * as they are, so the names are written so.
 */
static const char * const rac2_unlock_names[] = { "Locked", "Unlocked", "Max" };
static const u32 rac2_unlock_values[] = { 0, 1, RAC2_MAX_LEVELS };

static const struct game_menu rac2_menu = {
	rac2_menu_sections, (u8)(sizeof(rac2_menu_sections) / sizeof(rac2_menu_sections[0])),
	rac2_menu_unlock_actions, (u8)sizeof(rac2_menu_unlock_actions),
	rac2_unlock_names, rac2_unlock_values,
	(u8)(sizeof(rac2_unlock_names) / sizeof(rac2_unlock_names[0])),
	1, "V",
	NULL,                 /* every planet with a name: the plain list, in the ids' order, the game's */
	NULL, NULL,           /* every item goes as far as the set; no action with a status */
	"Collectables",       /* the eight buttons on a page of their own (round 3, as UYA's) */
	NULL                  /* the planet select shows every planet (the pickers' round 2) */
};

/* ---------------------------------------------------------------- vtable */

/*
 * All four slots are taken. Everything qwark keys by the title id rather than by
 * the game - the mods folder, config.txt's per-feature auto flags - goes on
 * using whichever of these four is actually running, exactly as BCES01503 does:
 * a repack has its own mods folder and its own auto flags. Only the position
 * slots are keyed on the game, because the coordinate addresses are.
 */
const struct game_api rac2_game = {
	{ "NPEA00386", "BCES01503", "RC2ILYOOO", "BLMODYOOO" },
	GAME_RAC2,

	RAC2_FP_ADDR,
	rac2_fp,
	rac2_fp_patched,
	GAME_FP_SWITCH(rac2_fp_switch),
	GAME_FP_SWITCH(rac2_fp_switch_old),
	sizeof(rac2_fp),

	0,                    /* no quit hook is known for RaC2 yet */
	RAC2_INPUTS,

	rac2_init,

	rac2_hot,
	(u8)(sizeof(rac2_hot) / sizeof(rac2_hot[0])),
	rac2_hot_decode,

	rac2_describe,

	rac2_set_toggle,
	rac2_toggle_read,
	rac2_trigger,
	rac2_set_value,
	NULL,                 /* no ENUM features */

	rac2_save_blob,
	rac2_load_blob,
	rac2_blob_xyz,
	rac2_blob_set_xyz,
	RAC2_POS_BLOB_LEN,

	rac2_planet_names,
	rac2_planet_load,
	rac2_die,
	rac2_load_setaside,
	rac2_savefile_ready,  /* build 53: on a planet, not in space; a save */

	rac2_unlock_list,
	rac2_unlock_read,
	rac2_unlock_set,

	rac2_levelflags_get,
	rac2_levelflags_reset,
	rac2_levelflags_set,

	rac2_moby_table,

	rac2_on_enter,
	NULL,                 /* on_quit */
	rac2_on_tick,

	rac2_autosplit_describe,

	&rac2_manips,         /* revision 1.21 */
	NULL,                 /* planet_order: the ids run in the game's order */
	&rac2_menu            /* revision 1.22, round 2 */
};
