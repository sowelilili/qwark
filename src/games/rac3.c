/*
 * Ratchet & Clank 3 (NPEA00387, BCES01503): addresses, patch words, planets, the
 * fingerprint and the fast-load patch. The feature handlers, unlocks and
 * level flags live next door in rac3_panel.c; this file owns the numbers, the
 * hot block and the vtable.
 *
 * Every address comes from racman's RaCTrainer/Games/RAC3. Where the old code
 * was ambiguous the comment says what was assumed.
 */
#include "rac3.h"
#include "classic.h"
#include "switches_bins.h"
#include "../core/mem.h"
#include "../core/autosplit.h"
#include "../core/savefile.h"
#include "../core/manips.h"

#include <string.h>

/* See rac3.h for what these three are and why all three are accepted. */
const u8 rac3_fp[4]         = { 0x7C, 0x85, 0x31, 0x2E };
const u8 rac3_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };
const u8 rac3_fp_switch[4]  = {
	(u8)(SW_RAC3_SITE_FREEZE_AMMO >> 24), (u8)(SW_RAC3_SITE_FREEZE_AMMO >> 16),
	(u8)(SW_RAC3_SITE_FREEZE_AMMO >> 8),  (u8)SW_RAC3_SITE_FREEZE_AMMO
};

const u8 rac3_vendor_qe[2] = { 0x00, 0x17 };

/* ------------------------------------------------------------- hot blocks */

/*
 * Nine reads every tick and four staggered slow ones:
 *
 *   0  inputs   pad +0x00, analogs +0x1DC                             every tick
 *   1  state    QE offset +0x000, planet +0x178, bolts +0x21C,        every tick
 *               challenge +0x24E, health XP +0x250, armour +0x25C
 *   2  player   the coordinate Vec4 and the rotation behind it        every tick
 *   3  pstate   player state +0x002, health +0x28C, Neffy phase +0x348 every tick
 *   4  load     request +0x00, destination planet +0x07, mode +0x24   every tick
 *   5  neffy    the Biobliterator's health bar                        every tick
 *   6  chunk    the loaded chunk                                      every tick
 *   7  guise    the Tyhrraguise unlock byte                           every tick
 *   8  loadscr  the loading-screen id, low byte                       every tick
 *   9  ship colour                                                   every 8th
 *  10  file time                                                     every 8th
 *  11  savefile the helper's own byte                                  every 8th
 *  12  chargeboot colour words                                       every 8th
 *
 * Blocks 3 to 8 belong to the autosplit watcher and are per-tick because a split
 * has to reach the PC in milliseconds; block 3 also carries the health readout
 * that used to have a slow block of its own. The slow blocks sit on phases 0..3,
 * so a tick costs nine reads plus at most one: 9.5 on average, ten at worst.
 * The load block 4 also carries the manips' file-load trigger, the trophy
 * auto-reset (revision 1.21) and the fast-load watcher. Build 53: the load
 * request word was a block of its own, 13, read after this one; it is the head
 * of block 4 now, so the request and the mode beside it always come out of the
 * same read and never straddle a change (rac3_fastload_watch).
 */
static const struct game_hot_block rac3_hot[] = {
	{ RAC3_HOT_INPUTS_ADDR, RAC3_HOT_INPUTS_LEN, 1, 0 },
	{ RAC3_HOT_STATE_ADDR,  RAC3_HOT_STATE_LEN,  1, 0 },
	{ RAC3_HOT_PLAYER_ADDR, RAC3_HOT_PLAYER_LEN, 1, 0 },
	{ RAC3_HOT_PSTATE_ADDR, RAC3_HOT_PSTATE_LEN, 1, 0 },
	{ RAC3_HOT_LOAD_ADDR,   RAC3_HOT_LOAD_LEN,   1, 0 },
	{ RAC3_NEFFY_HEALTH,    4,                   1, 0 },
	{ RAC3_LOADED_CHUNK,    4,                   1, 0 },
	{ RAC3_TYHRRAGUISE,     1,                   1, 0 },
	{ RAC3_LOADING_SCREEN,  1,                   1, 0 },
	{ RAC3_SHIP_COLOUR,     4,                   8, 0 },
	{ RAC3_FILE_TIME,       4,                   8, 1 },
	{ RAC3_SF_HELPER,       1,                   8, 2 },
	{ RAC3_CB_PRIMARY_FRONT, 0x18,               8, 3 }
};

#define HOT_INPUTS   0
#define HOT_STATE    1
#define HOT_PLAYER   2
#define HOT_PSTATE   3
#define HOT_LOAD     4
#define HOT_NEFFY    5
#define HOT_CHUNK    6
#define HOT_GUISE    7
#define HOT_LOADSCR  8
#define HOT_SHIP     9
#define HOT_TIME     10
#define HOT_SAVEFILE 11
#define HOT_COLOURS  12

#define OFF_ANALOGS   (RAC3_ANALOGS        - RAC3_HOT_INPUTS_ADDR)   /* 0x1DC */

#define OFF_PLANET    (RAC3_CURRENT_PLANET - RAC3_HOT_STATE_ADDR)    /* 0x178 */
#define OFF_BOLTS     (RAC3_BOLTS          - RAC3_HOT_STATE_ADDR)    /* 0x21C */
#define OFF_CHALLENGE (RAC3_CHALLENGE_MODE - RAC3_HOT_STATE_ADDR)    /* 0x24E */
#define OFF_HEALTH_XP (RAC3_HEALTH_XP      - RAC3_HOT_STATE_ADDR)    /* 0x250 */
#define OFF_ARMOUR    (RAC3_CURRENT_ARMOR  - RAC3_HOT_STATE_ADDR)    /* 0x25C */

#define OFF_CB_BACK   (RAC3_CB_PRIMARY_BACK - RAC3_CB_PRIMARY_FRONT) /* 0x04 */
#define OFF_CB_TINT_F (RAC3_CB_TINT_FRONT   - RAC3_CB_PRIMARY_FRONT) /* 0x10 */
#define OFF_CB_TINT_B (RAC3_CB_TINT_BACK    - RAC3_CB_PRIMARY_FRONT) /* 0x14 */

#define OFF_HEALTH     (RAC3_PLAYER_HEALTH - RAC3_HOT_PSTATE_ADDR)   /* 0x28C */
#define OFF_NEFFY_PH   (RAC3_NEFFY_PHASE   - RAC3_HOT_PSTATE_ADDR)   /* 0x348 */
#define OFF_DEST       (RAC3_DEST_PLANET   - RAC3_HOT_LOAD_ADDR)     /* 0x04 */
#define OFF_GAMESTATE  (RAC3_GAME_STATE    - RAC3_HOT_LOAD_ADDR)     /* 0x24 */
#define OFF_MODETIMER  (RAC3_MODE_TIMER    - RAC3_HOT_LOAD_ADDR)     /* 0x20 */

/* What the autosplit watcher reads, named the way rac3-autosplitter.asl names it. */
struct rac3_as_state {
	u8  dest_planet;
	u8  planet;
	u16 player_state;
	u32 game_state;
	f32 neffy_health;
	u32 neffy_phase;
	u32 chunk;
	u8  guise;
	u8  loading_screen;
};

static struct rac3_as_state g_as;

/*
 * What the manips' file-load trigger reads (revision 1.21), filled by the
 * decode: the load request word, the destination and the planet as the signed
 * words they are, the game mode and its frame count.
 */
static u32 g_load_request;
static u32 g_load_dest;
static u32 g_planet_word;
static u32 g_mode;
static u32 g_mode_timer;

/* The trigger's own state; see rac3_file_load_watch. */
static int g_fl_primed;
static int g_fl_armed;
static int g_fl_edge;
static u32 g_fl_prev_mode;

/* The trophy auto-reset's (R3_AUTO_TROPHIES); see rac3_file_load_watch. */
static int g_auto_trophies;
static int g_trophies_asked;

/*
 * Build 53: Freeze health, held at the game's own maximum (rac3.h) rather than
 * at a flat 200, which is more than a player has until the health XP is full.
 */
static struct classic_health rac3_freeze_health = {
	RAC3_PLAYER_HEALTH, RAC3_MAX_HEALTH, 0, 0, 0, 0, 0
};

static void rac3_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	const u8 *inputs = blocks[HOT_INPUTS];
	const u8 *state  = blocks[HOT_STATE];
	const u8 *player = blocks[HOT_PLAYER];
	int i;

	memset(out, 0, sizeof(*out));

	if (inputs != NULL) {
		out->pad_mask = be32_get(inputs);
		classic_decode_analogs(inputs + OFF_ANALOGS, out->analog);
	}

	if (state != NULL) {
		/*
		 * RaC3's planet ids start at 1 and the autosplitter reads the low byte of
		 * the word, so PLANET_LIST index 0 is a placeholder and index n is the id
		 * the game reports here.
		 */
		g_planet_word = be32_get(state + OFF_PLANET);
		out->current_planet = state[OFF_PLANET + 3];
		out->readout[RAC3_RO_BOLTS]     = be32_get(state + OFF_BOLTS);
		out->readout[RAC3_RO_CHALLENGE] = state[OFF_CHALLENGE];
		out->readout[RAC3_RO_HEALTH_XP] = be32_get(state + OFF_HEALTH_XP);
		out->readout[RAC3_RO_ARMOUR]    = be16_get(state + OFF_ARMOUR);
		/*
		 * The QE offset is a signed 16-bit field. The readout carries the raw
		 * halfword and the feature row says SIGNED, 16 bits, so the client is
		 * the one that sign-extends it (protocol 1.7).
		 */
		out->readout[RAC3_RO_QE_OFFSET] = be16_get(state);
	}

	if (player != NULL) {
		for (i = 0; i < 3; i++) out->pos[i] = bef32_get(player + i * 4);
	}

	if (blocks[HOT_PSTATE] != NULL)
		out->readout[RAC3_RO_HEALTH] = be32_get(blocks[HOT_PSTATE] + OFF_HEALTH);

	if (blocks[HOT_SHIP] != NULL)
		out->readout[RAC3_RO_SHIP] = blocks[HOT_SHIP][0];

	if (blocks[HOT_TIME] != NULL)
		out->readout[RAC3_RO_FILE_TIME] = be32_get(blocks[HOT_TIME]);

	/* 1 once the helper has run a frame; SAVEFILE_INFO reports the same byte. */
	if (blocks[HOT_SAVEFILE] != NULL)
		out->readout[RAC3_RO_SAVEFILE] = blocks[HOT_SAVEFILE][0];

	if (blocks[HOT_COLOURS] != NULL) {
		const u8 *c = blocks[HOT_COLOURS];
		out->readout[RAC3_RO_CB_FRONT] = classic_cb_to_rgb(be32_get(c));
		out->readout[RAC3_RO_CB_BACK]  = classic_cb_to_rgb(be32_get(c + OFF_CB_BACK));
		out->readout[RAC3_RO_CB_TINT]  = classic_cb_to_rgb(be32_get(c + OFF_CB_TINT_F));
	}

	/* --------------------------------------------- the autosplit watcher's view */

	g_as.planet = out->current_planet;

	if (blocks[HOT_PSTATE] != NULL) {
		g_as.player_state = be16_get(blocks[HOT_PSTATE] + 2);
		g_as.neffy_phase  = be32_get(blocks[HOT_PSTATE] + OFF_NEFFY_PH);
	}

	if (blocks[HOT_LOAD] != NULL) {
		g_load_request   = be32_get(blocks[HOT_LOAD]);
		g_as.dest_planet = blocks[HOT_LOAD][OFF_DEST + 3];
		g_load_dest  = be32_get(blocks[HOT_LOAD] + OFF_DEST);
		g_mode_timer = be32_get(blocks[HOT_LOAD] + OFF_MODETIMER);
		g_mode       = be32_get(blocks[HOT_LOAD] + OFF_GAMESTATE);
		g_as.game_state  = be32_get(blocks[HOT_LOAD] + OFF_GAMESTATE);
	}

	if (blocks[HOT_NEFFY] != NULL) g_as.neffy_health = bef32_get(blocks[HOT_NEFFY]);
	if (blocks[HOT_CHUNK] != NULL) g_as.chunk        = be32_get(blocks[HOT_CHUNK]);
	if (blocks[HOT_GUISE] != NULL) g_as.guise        = blocks[HOT_GUISE][0];

	if (blocks[HOT_LOADSCR] != NULL)
		g_as.loading_screen = blocks[HOT_LOADSCR][0];
}

/* --------------------------------------------------------------- patches */

static const struct patch_word rac3_ammo_words[] = {
	{ RAC3_AMMO_INSTR, 0x60000000u }   /* nop the ammo decrement */
};

/* Build 53: named as its checkbox is, "Infinite ammo" rather than "Freeze ammo". */
static struct patch_def rac3_ammo = {
	"Infinite ammo", PATCH_KIND_FEATURE, rac3_ammo_words, 1, NULL
};

/*
 * Fast loads: the console's cave at RAC3_FASTLOAD_CAVE and the branches into it
 * at RAC3_FASTLOAD_DRAW_SITE and RAC3_FASTLOAD_SITE, which rac3.h describes. The
 * sites go last, and patch_apply puts the cave in before them and takes it out
 * after them; the pass site goes in after the draw site and comes out before it.
 * Either alone, as a load sees the patch go in or out under it, is harmless: the
 * draw site alone spins the transition undrawn as RaC2's does, the pass site
 * alone holds it still and drawn.
 *
 * sw_rac3.s has the same logic behind a flag byte for RPCS3. The console's
 * differs in how it calls the game: it points the link register at the loop
 * and branches, so a call comes back to the game's own code rather than into
 * the cave - the fade and Transition_UnloadSounds to RAC3_FASTLOAD_RESUME, the
 * draw site's own call to the word after it. The fade takes six frames, and
 * turning fast loads off in the middle of one takes the cave out from under
 * nothing. (RPCS3 never takes its trampolines out, and keeps the ordinary call
 * its recompiler expects.)
 *
 *   0x9807F0  lis    r5, 0xDA
 *             lhz    r6, -0x6F22(r5)  RAC3_TRANSITION_ID
 *             cmplwi r6, 7
 *             beq    run              multiplayer: the game's own
 *             lhz    r3, -0x6F2E(r5)  RAC3_TRANSITION_FRAME
 *             cmpwi  r3, 0
 *             beq    first
 *             lis    r3, 0xF4
 *             lwz    r3, 0x4E90(r3)   RAC3_SCENE_OWED
 *             cmpwi  r3, 0
 *             bne    run              the transition plays it, undrawn
 *             lwz    r3, -0x6F6C(r5)  RAC3_LOADER_STAGE
 *             cmpwi  r3, 2
 *             blt    resume           not read yet: the transition stands still
 *             li     r27, 1           read: the transition is over
 *             lis    r3, 0x135
 *             lis    r0, 0x3F80
 *             stw    r0, -0x1420(r3)  RAC3_TRANSITION_FADE = 1.0f, black
 *             cmplwi r6, 6
 *             bgt    resume           no banks of its own
 *             lwz    r3, -0x6F68(r5)  RAC3_TRANSITION_HOLD
 *             cmpwi  r3, 0
 *             beq    resume           its banks are gone already
 *             lis    r3, 0x28
 *             addi   r3, r3, -0x4AC8
 *             mtlr   r3               back to RAC3_FASTLOAD_RESUME
 *             b      0x278A00         Transition_UnloadSounds
 *   first:    lis    r5, 0xCB
 *             lwz    r5, 0x9BC(r5)    RAC3_RENDERER
 *             li     r0, 0
 *             stw    r0, 0x1720(r5)   RAC3_RENDERER_HELD = 0: every flip shows
 *             lis    r5, 0x28
 *             addi   r5, r5, -0x4AC8
 *             mtlr   r5               back to RAC3_FASTLOAD_RESUME
 *             li     r3, 6
 *             b      0x123988         RAC3_SCREEN_FADE, six frames
 *   resume:   b      0x27B538
 *   run:      ori    r3, r26, 0       the site's own instruction
 *             b      0x27B4A0
 *   0x98088C  lis    r3, 0xDA
 *             lhz    r3, -0x6F22(r3)  RAC3_TRANSITION_ID
 *             cmplwi r3, 7
 *             beq    1f
 *             b      0x27B570         past the frame: nothing drawn, no flip
 *   1:        lis    r3, 0x28
 *             addi   r3, r3, -0x4AAC
 *             mtlr   r3               back to 0x27B554
 *             b      0x11AD44         the draw site's own call
 *   0x27B49C  b      0x9807F0
 *   0x27B550  b      0x98088C
 *
 * Assembled with the SDK's ppu-lv2-gcc and ld at these addresses.
 */
static const struct patch_word rac3_fastload_words[] = {
	{ RAC3_FASTLOAD_CAVE + 0x00, 0x3CA000DAu },
	{ RAC3_FASTLOAD_CAVE + 0x04, 0xA0C590DEu },
	{ RAC3_FASTLOAD_CAVE + 0x08, 0x28060007u },
	{ RAC3_FASTLOAD_CAVE + 0x0C, 0x41820088u },
	{ RAC3_FASTLOAD_CAVE + 0x10, 0xA06590D2u },
	{ RAC3_FASTLOAD_CAVE + 0x14, 0x2C030000u },
	{ RAC3_FASTLOAD_CAVE + 0x18, 0x41820054u },
	{ RAC3_FASTLOAD_CAVE + 0x1C, 0x3C6000F4u },
	{ RAC3_FASTLOAD_CAVE + 0x20, 0x80634E90u },
	{ RAC3_FASTLOAD_CAVE + 0x24, 0x2C030000u },
	{ RAC3_FASTLOAD_CAVE + 0x28, 0x4082006Cu },
	{ RAC3_FASTLOAD_CAVE + 0x2C, 0x80659094u },
	{ RAC3_FASTLOAD_CAVE + 0x30, 0x2C030002u },
	{ RAC3_FASTLOAD_CAVE + 0x34, 0x4180005Cu },
	{ RAC3_FASTLOAD_CAVE + 0x38, 0x3B600001u },
	{ RAC3_FASTLOAD_CAVE + 0x3C, 0x3C600135u },
	{ RAC3_FASTLOAD_CAVE + 0x40, 0x3C003F80u },
	{ RAC3_FASTLOAD_CAVE + 0x44, 0x9003EBE0u },
	{ RAC3_FASTLOAD_CAVE + 0x48, 0x28060006u },
	{ RAC3_FASTLOAD_CAVE + 0x4C, 0x41810044u },
	{ RAC3_FASTLOAD_CAVE + 0x50, 0x80659098u },
	{ RAC3_FASTLOAD_CAVE + 0x54, 0x2C030000u },
	{ RAC3_FASTLOAD_CAVE + 0x58, 0x41820038u },
	{ RAC3_FASTLOAD_CAVE + 0x5C, 0x3C600028u },
	{ RAC3_FASTLOAD_CAVE + 0x60, 0x3863B538u },
	{ RAC3_FASTLOAD_CAVE + 0x64, 0x7C6803A6u },
	{ RAC3_FASTLOAD_CAVE + 0x68, 0x4B8F81A8u },
	{ RAC3_FASTLOAD_CAVE + 0x6C, 0x3CA000CBu },
	{ RAC3_FASTLOAD_CAVE + 0x70, 0x80A509BCu },
	{ RAC3_FASTLOAD_CAVE + 0x74, 0x38000000u },
	{ RAC3_FASTLOAD_CAVE + 0x78, 0x90051720u },
	{ RAC3_FASTLOAD_CAVE + 0x7C, 0x3CA00028u },
	{ RAC3_FASTLOAD_CAVE + 0x80, 0x38A5B538u },
	{ RAC3_FASTLOAD_CAVE + 0x84, 0x7CA803A6u },
	{ RAC3_FASTLOAD_CAVE + 0x88, 0x38600006u },
	{ RAC3_FASTLOAD_CAVE + 0x8C, 0x4B7A310Cu },
	{ RAC3_FASTLOAD_CAVE + 0x90, 0x4B8FACB8u },
	{ RAC3_FASTLOAD_CAVE + 0x94, 0x63430000u },
	{ RAC3_FASTLOAD_CAVE + 0x98, 0x4B8FAC18u },
	{ RAC3_FASTLOAD_CAVE + 0x9C, 0x3C6000DAu },
	{ RAC3_FASTLOAD_CAVE + 0xA0, 0xA06390DEu },
	{ RAC3_FASTLOAD_CAVE + 0xA4, 0x28030007u },
	{ RAC3_FASTLOAD_CAVE + 0xA8, 0x41820008u },
	{ RAC3_FASTLOAD_CAVE + 0xAC, 0x4B8FACD4u },
	{ RAC3_FASTLOAD_CAVE + 0xB0, 0x3C600028u },
	{ RAC3_FASTLOAD_CAVE + 0xB4, 0x3863B554u },
	{ RAC3_FASTLOAD_CAVE + 0xB8, 0x7C6803A6u },
	{ RAC3_FASTLOAD_CAVE + 0xBC, 0x4B79A498u },
	{ RAC3_FASTLOAD_DRAW_SITE,   0x4870533Cu },
	{ RAC3_FASTLOAD_SITE,        0x48705354u }
};

#define RAC3_FASTLOAD_WORDS \
	(u16)(sizeof(rac3_fastload_words) / sizeof(rac3_fastload_words[0]))

static struct patch_def rac3_fastload = {
	"Fast loads", PATCH_KIND_FEATURE, rac3_fastload_words, RAC3_FASTLOAD_WORDS, NULL
};

/*
 * Build 50. Under RPCS3 both are code switches instead (src/games/switches/
 * sw_rac3.s), chosen by the flag bytes SW_RAC3_FLAG_* name.
 */

/* What the checkbox holds. */
static int g_fastload_on;

/*
 * Fast loads are on because a planet load forced them, not because the
 * checkbox is: from rac3_fastload_force until the watcher's restore. On a
 * console that is invisible, since the checkbox is qwark's own bit. With the
 * code switches in, the checkbox is read back out of the flag byte, and while
 * this is set the read-back reports the checkbox rather than the force, so it
 * does not flick on for the length of every planet load. rac2.c's
 * g_fastload_forced, word for word.
 */
static int g_fastload_forced;

/*
 * What rac3_fastload_watch has seen since the force: the load request word up
 * (loading), and the game in mode 6 with it (ran). Both start again at every
 * force, so the end of a load that was under way before it cannot pass for the
 * end of the load it is for.
 */
static int g_fastload_loading;
static int g_fastload_ran;

/*
 * savefile_load_refusals as rac3_on_tick last saw it, rac2.c's g_sf_refusals:
 * a set-aside load forces fast loads before its request goes out, and the
 * helper can still drop the request on the game's frame - the game would not
 * pause, or there was no save - when no load follows to put them back.
 */
static u32 g_sf_refusals;

/*
 * A planet load and a set-aside load force fast loads on and the watcher puts
 * the checkbox back when the load is over, as RaC2's do. patch_apply is a no-op
 * when the toggle already applied it, so nothing is captured twice and nothing
 * is written twice.
 *
 * Under RPCS3 the flag byte outlives qwark: a qwark-rpcs3 started against a game
 * that is already running finds it however the last one left it, and the
 * checkbox reads it back as it is. A flag at 1 that this process did not force
 * is the user's setting, so the force takes it for the checkbox's rather than
 * its own, and the end of the load leaves it up. On a console the read is
 * NOT_FOUND: the patch is qwark's own, and a new process starts without it.
 */
int rac3_fastload_force(void)
{
	int on = 0;
	int rc;

	if (!g_fastload_forced &&
	    classic_code_read(SW_RAC3_FLAG_FAST_LOADS, &on) == ST_OK && on)
		g_fastload_on = 1;

	rc = classic_code_force(&rac3_fastload, SW_RAC3_FLAG_FAST_LOADS);
	if (rc == ST_OK && !g_fastload_on) g_fastload_forced = 1;

	/*
	 * The commands run before the tick reads the game, so this tick's read may
	 * still show the end of the load before: the watch starts over from here.
	 */
	g_fastload_loading = 0;
	g_fastload_ran = 0;
	return rc;
}

int rac3_fastload_restore(void)
{
	g_fastload_forced = 0;
	g_fastload_loading = 0;
	g_fastload_ran = 0;
	if (g_fastload_on) return ST_OK;
	return classic_code_toggle(&rac3_fastload, SW_RAC3_FLAG_FAST_LOADS, 0);
}

int rac3_fastload_forced(void)
{
	return g_fastload_forced;
}

static void rac3_init(void)
{
	static int done = 0;

	if (done) return;
	done = 1;

	rac3_ammo.originals     = patch_pool_alloc(rac3_ammo.count);
	rac3_fastload.originals = patch_pool_alloc(rac3_fastload.count);
}

/* ------------------------------------------------- the autosplit watcher */

/*
 * rac3-autosplitter.asl, condition for condition. Everything it gates on a
 * *setting* is emitted anyway with its own reason code, because the client owns
 * the settings; everything it gates on game state is a condition below. The
 * split-route bookkeeping (vars.SplitRoute, vars.SplitCount, STRICT_ORDER) is
 * the client's business.
 *
 * The long load is not: the script took a second off game time whenever the
 * loading screen became 1, unless either end of the trip was one of the five
 * planets whose loading screen is a different one that only looks long. That is
 * code 6, a LOAD_START with FLAT, emitted under exactly that condition, and a
 * LOAD_END when the screen leaves 1 so a client can draw the interval.
 */
static struct rac3_as_state g_as_prev;
static int g_as_primed;

/* vars.biobliterator and vars.korosTBs. */
static int g_biobliterator;
static int g_koros_bolts;

/* vars.originPlanet, and whether the load in progress was counted as long. */
static u8  g_origin_planet;
static int g_long_load_open;

/* vars.llIgnorePlanets, as a test rather than a 37-entry table. */
static int rac3_ll_ignored(u8 planet)
{
	return planet == RAC3_LL_IGNORE_LAUNCHSITE ||
	       planet == RAC3_LL_IGNORE_METRO ||
	       planet == RAC3_LL_IGNORE_AQ_CLANK ||
	       planet == RAC3_LL_IGNORE_AQ_SEWERS ||
	       planet == RAC3_LL_IGNORE_TYHRRA;
}

static void rac3_autosplit_tick(void)
{
	const struct rac3_as_state *p = &g_as_prev;

	/* LiveSplit's first update has old == current; prime and wait a tick. */
	if (!g_as_primed) {
		g_as_prev = g_as;
		g_as_primed = 1;
		return;
	}

	/*
	 * The script's origin tracking, verbatim: the planet you left when a planet
	 * change is under way, and the planet you are standing on when one is not.
	 */
	if (g_as.planet != p->planet && g_as.dest_planet != 0)
		g_origin_planet = p->planet;
	else if (g_as.planet == p->planet && g_as.dest_planet == 0)
		g_origin_planet = g_as.planet;

	/* The long load, both ends of it. */
	if (g_as.loading_screen == RAC3_LOADING_LONG &&
	    p->loading_screen != RAC3_LOADING_LONG) {
		if (!rac3_ll_ignored(g_as.dest_planet) &&
		    !rac3_ll_ignored(g_origin_planet)) {
			g_long_load_open = 1;
			autosplit_emit(AUTOSPLIT_LOAD_START, R3_AS_LONG_LOAD,
			               g_as.dest_planet);
		}
	} else if (g_as.loading_screen != RAC3_LOADING_LONG &&
	           p->loading_screen == RAC3_LOADING_LONG && g_long_load_open) {
		g_long_load_open = 0;
		autosplit_emit(AUTOSPLIT_LOAD_END, R3_AS_LONG_LOAD, g_as.dest_planet);
	}

	/* The script's update block, which runs before start/split/reset. */
	if (!g_biobliterator && g_as.game_state == 0 &&
	    g_as.planet == RAC3_PLANET_LAUNCHSITE &&
	    (g_as.neffy_phase % 2) == 1 && g_as.neffy_health == 1.0f) {
		g_biobliterator = 1;
	}

	if (g_as.player_state == RAC3_KOROS_BOLT_STATE &&
	    p->player_state != RAC3_KOROS_BOLT_STATE &&
	    g_as.planet == RAC3_PLANET_KOROS) {
		g_koros_bolts++;
	}

	/*
	 * start and reset are the same expression in this script. Emit both and let
	 * the client apply whichever suits its timer.
	 */
	if (g_as.planet == RAC3_PLANET_VELDIN &&
	    p->game_state == RAC3_GAME_STATE_LOAD && g_as.game_state == 0) {
		autosplit_emit(AUTOSPLIT_RESET, 0, 0);
		g_biobliterator = 0;
		g_koros_bolts = 0;
		autosplit_emit(AUTOSPLIT_START, 0, 0);
	}

	if (g_as.planet == RAC3_PLANET_MARCADIA && g_as.chunk == 1 && p->chunk != 1)
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_LDF, 0);

	if (g_as.guise == 1 && p->guise == 0)
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_TYHRRAGUISE, 0);

	if (g_as.planet == RAC3_PLANET_KOROS && g_koros_bolts >= 2) {
		g_koros_bolts = 0;
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_KOROS_BOLT, 0);
	}

	/*
	 * The script writes these two as if/else, so a Biobliterator split takes the
	 * tick and the planet split does not also fire on it.
	 */
	if (g_as.neffy_health == 0.0f && g_biobliterator &&
	    g_as.planet == RAC3_PLANET_LAUNCHSITE) {
		g_biobliterator = 0;
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_BIOBLITERATOR, 0);
	} else if (g_as.dest_planet != p->dest_planet &&
	           g_as.planet != g_as.dest_planet &&
	           g_as.dest_planet != 0 && g_as.planet != 0 &&
	           g_as.dest_planet != RAC3_PLANET_VELDIN) {
		/*
		 * The script's own comment on this line says "not reloading, and not
		 * going to or from veldin", but the two tests under it are its RaC1
		 * ancestor's, where Veldin is planet 0; here it is planet 1 and neither
		 * test catches it. So a run being started over — the load to Veldin that
		 * the game-state edge above turns into RESET a few seconds later — took
		 * a split first, and the run it threw away got a bogus last segment out
		 * of it. A load to Veldin is the beginning of a run, not a planet change.
		 *
		 * Only the destination: leaving Veldin for Florana is the run's first
		 * real split, and RaC3 has no Veldin split of its own to carry it the way
		 * RaC1 does.
		 */
		autosplit_emit(AUTOSPLIT_SPLIT, R3_AS_PLANET, g_as.dest_planet);
	}

	g_as_prev = g_as;
}

#define DF AUTOSPLIT_FLAG_DEFAULT
#define RT AUTOSPLIT_FLAG_ROUTE
#define FL AUTOSPLIT_FLAG_FLAT

/*
 * The script's settings.Add list, in its order; DF is what it defaults to true.
 * The long-load row is last and is not a split: DF because the second the script
 * took off was not a setting either.
 */
static const struct autosplit_desc rac3_autosplits[] = {
	{ R3_AS_PLANET,        AUTOSPLIT_SPLIT,      DF | RT, 0, "Planet entered" },
	{ R3_AS_BIOBLITERATOR, AUTOSPLIT_SPLIT,      DF,      0, "Biobliterator defeated" },
	{ R3_AS_LDF,           AUTOSPLIT_SPLIT,      0,       0, "LDF entered" },
	{ R3_AS_KOROS_BOLT,    AUTOSPLIT_SPLIT,      0,       0, "Koros bolt 2" },
	{ R3_AS_TYHRRAGUISE,   AUTOSPLIT_SPLIT,      0,       0, "Tyhrraguise obtained" },
	{ R3_AS_LONG_LOAD,     AUTOSPLIT_LOAD_START, DF | FL, RAC3_LONG_LOAD_US,
	  "Long load" }
};

#undef DF
#undef RT
#undef FL

static const struct autosplit_desc *rac3_autosplit_describe(u8 *count)
{
	*count = (u8)(sizeof(rac3_autosplits) / sizeof(rac3_autosplits[0]));
	return rac3_autosplits;
}

static void rac3_on_enter(void)
{
	/*
	 * A fresh process: nothing is forced, nothing has been seen yet. The patch
	 * went with the old process, and a new one starts without it.
	 */
	g_fastload_on = 0;
	g_fastload_forced = 0;
	g_fastload_loading = 0;
	g_fastload_ran = 0;
	g_sf_refusals = savefile_load_refusals();

	memset(&g_as, 0, sizeof(g_as));
	memset(&g_as_prev, 0, sizeof(g_as_prev));
	g_as_primed = 0;
	g_biobliterator = 0;
	g_koros_bolts = 0;
	g_origin_planet = 0;
	g_long_load_open = 0;

	/*
	 * The trophy toggle is qwark's own state and goes with the process, and so
	 * does Freeze health's tracking of the maximum.
	 */
	g_auto_trophies = 0;
	g_trophies_asked = 0;
	classic_health_forget(&rac3_freeze_health);
	rac3_trophy_forget();
	g_load_request = 0;
	g_load_dest = 0;
	g_planet_word = 0;
	g_mode = 0;
	g_mode_timer = 0;
	g_fl_primed = 0;
	g_fl_armed = 0;
	g_fl_edge = 0;
}

/*
 * The end of a forced load, for the fast-load restore: the load request word
 * (RAC3_LOAD_PLANET) down again with the game out of mode 6, after the load has
 * been seen to run - request and mode 6 - since the force.
 *
 * Whoever asks for a load - qwark's PLANET_LOAD, the galactic map, a save
 * loaded, the savefile helper - sets the request word. The load itself runs in
 * mode 6: the transition (0x27B360) and the finish behind it, during which the
 * word stays set; main's init 0x13E10 clears it (at 0x13F74) only once the
 * transition has returned, and the new level loop's init then puts the mode to
 * 0 (0x1D7068). By then the patched loop is not running, so the restore can take
 * the patch out from under it; RaC2's watcher puts its fast loads back at the
 * end of the load screen for the same reason.
 *
 * The word goes down before the end, too. A request pending as the game leaves
 * the pause for mode 6 - the galactic map's way of going - is cleared at
 * 0x22A0F4 and turned into the ship leaving the planet (0x26EF40, which saves
 * the game first, 0x1E17B8), and the mode reads 3, the pause, until 0x22A274
 * stores the new one after that; the ship leaves in mode 6 and raises the
 * request again for the transition (0x27389C). So the word down with the mode
 * out of 6 is the end only once mode 6 has been seen: before that it is this,
 * or a request no load has taken up yet. Seeing the request and the mode out of
 * one read (block 4) is what keeps a tick from pairing one moment's word with
 * another's mode.
 *
 * Only a load that was forced has anything to put back. The force starts the
 * watch over (rac3_fastload_force), because a command runs before the tick's
 * read: the load before it may be ending in the very read that follows, and its
 * end is not this load's.
 */
static void rac3_fastload_watch(void)
{
	if (!g_fastload_forced) {
		g_fastload_loading = 0;
		g_fastload_ran = 0;
		return;
	}

	if (g_load_request != 0) {
		g_fastload_loading = 1;
		if (g_mode == RAC3_GAME_STATE_LOAD) g_fastload_ran = 1;
		return;
	}

	if (!g_fastload_loading) return;

	if (g_mode == RAC3_GAME_STATE_LOAD) {
		g_fastload_ran = 1;
		return;
	}

	if (!g_fastload_ran) return;

	/* The load is over: put fast loads back where the toggle wants them. */
	rac3_fastload_restore();
}

/* ---------------------- the manips' file-load trigger and the trophies, 1.21 */

/*
 * A load of Veldin, for the manip groups (src/core/manips.h): the user's rule
 * is the end of a planet load whose planet is the first one, which every run
 * starts on and none comes back to. From the NPEA00387 executable; not yet run
 * on hardware.
 *
 * Every planet load is one routine, 0x275B18, entered from the front end
 * (0x12528) or from the level loop (0x1D87E8) once the load request word is
 * set: its first step puts the game mode to 6 (0x27ACCC, at 0x27AD58) and the
 * planet to the destination (0x275BA0). The request word stays set for the
 * whole load; main clears it (0x13E10, at 0x13F74) before it starts the level
 * loop, whose init puts the mode to 0 (0x1D7068) and the mode timer to 0
 * (0x1D7078), and the timer then counts frames. Every way into a run asks for
 * Veldin that way: a new game (the front-end menu at 0x11FC4, or new_game
 * 0x1DFD30 and goto_planet 0x14AA54 through 0x28A76C), a save loaded from the
 * title (perform_load 0x1E1CF4, the request at 0x1E203C) or from the game
 * (0x1E2198, the mode-6 handler then calling goto_planet at 0x22A08C, which is
 * also the savefile helper's way), and a challenge-mode start (0x2EF3BC, then
 * 0x28A76C).
 *
 * So: arm while a load into Veldin is under way (request set, destination 1,
 * mode 6) - requiring mode 6 keeps out the title's frame where the request is
 * set and the mode is still 0 - then fire once the mode has left 6 on Veldin
 * and the level init has run a frame (mode timer 1 or more). Mode 6 also comes
 * and goes without a load, in ship cutscenes, but never with the request the
 * arm needs, and Veldin skips the level init's own push (0x1D7298). No death
 * or respawn writes the request word. The challenge counter is final by then:
 * the restore, the new-game clear (0x1DF81C) and the challenge start
 * (0x2EF43C) all write it before the request, and the load never does.
 *
 * The first tick of a process only learns the mode, and never fires.
 */
/*
 * Revision 1.21: the trophy auto-reset, on while R3_AUTO_TROPHIES is. Whatever
 * category is selected, None included, it asks for the reset (rac3_panel.c,
 * which says what it is) once per run, from the first tick a load into Veldin
 * is seen - request up, destination 1, mode 6 - and, build 53, asks again on
 * the next tick for as long as that load lasts if the game's own trophy thread
 * is running then (BUSY).
 *
 * Why there. The reset must be between runs: after the last run's trophies,
 * and before this run can win one. A load into Veldin is the start of every run
 * (rac3_file_load_watch's block comment has every way into one), the trophies
 * of the run before are all behind it, and a run's timer starts at its end -
 * the autosplitter's START is Veldin with the mode going from 6 to 0
 * (rac3_autosplit_tick), LiveSplit's script the same. The helper runs during
 * that load: the transition loop the load starts with (0x27B360, which keeps
 * the mode at 6) calls the pad routine every pass (0x27B464), and so the
 * helper; and the helper refuses an auto request it finds outside a load
 * (sf_rac3.h), so the reset always starts inside the load. Up to the first
 * hardware test it also stood the game's main thread still for three seconds,
 * which the load hid; it runs on a thread of the helper's own now, and the game
 * goes on through it. The folder's delete runs first, a few files a tick, and
 * is done long before the load is. Nothing the reset leaves the game able to
 * win - the RY3NO, the Metropolis blimp, the snowman skin - is on Veldin, and
 * the reset, a few seconds of the game's register threads, is over long before
 * the run leaves it. The title screen was the other place considered: it would
 * miss a run started from a load in the game.
 */
static void rac3_file_load_watch(void)
{
	if (!g_fl_primed) {
		g_fl_primed = 1;
		g_fl_armed = 0;
		g_fl_edge = 0;
		g_fl_prev_mode = g_mode;
		g_trophies_asked = 0;
		return;
	}

	if (g_load_request != 0) {
		g_fl_armed = (g_load_dest == RAC3_PLANET_VELDIN &&
		              g_mode == RAC3_GAME_STATE_LOAD);
		g_fl_edge = 0;

		if (g_fl_armed && g_auto_trophies && !g_trophies_asked)
			g_trophies_asked = rac3_trophy_auto();
	} else {
		g_trophies_asked = 0;
	}

	if (g_fl_armed && g_fl_prev_mode == RAC3_GAME_STATE_LOAD &&
	    g_mode != RAC3_GAME_STATE_LOAD)
		g_fl_edge = 1;

	g_fl_prev_mode = g_mode;

	if (g_fl_edge && g_planet_word == RAC3_PLANET_VELDIN &&
	    (g_mode_timer >= 1 || g_mode != 0)) {
		g_fl_armed = 0;
		g_fl_edge = 0;
		manips_file_loaded();
	}
}

/*
 * Whether a save file is in play, for the manips: the planet word is -1 on the
 * title (its initial value, and written at 0x1181C and on a quit at 0x27A9F8)
 * and a planet's number from the frame a save is restored or a new game set up.
 * Tick thread.
 */
static int rac3_in_file(void)
{
	u32 planet = 0;

	if (mem_read_u32(RAC3_CURRENT_PLANET, &planet) != ST_OK) return -1;
	return (s32)planet >= 1;
}

static void rac3_on_tick(const struct game_hot *hot)
{
	(void)hot;

	/* A set-aside load the helper dropped puts the forced fast loads back. */
	if (savefile_load_refusals() != g_sf_refusals) {
		g_sf_refusals = savefile_load_refusals();
		if (g_fastload_forced) rac3_fastload_restore();
	}

	classic_health_tick(&rac3_freeze_health);
	rac3_fastload_watch();
	rac3_autosplit_tick();
	rac3_file_load_watch();
	rac3_trophy_tick();
}

/* -------------------------------------------------------------- describe */

static const char * const rac3_groups[] = {
	"Cheats",
	"Player",
	"Progress",
	"Savefile",
	"Cosmetics",
	"Trophies"
};

#define G_CHEATS    0
#define G_PLAYER    1
#define G_PROGRESS  2
#define G_SAVEFILE  3
#define G_COSMETICS 4
/* Build 53: the trophy reset and its auto toggle, MANIP_LIST's "Trophies". */
#define G_TROPHIES  5

/* Indexed by RAC3_RO_*; the order is part of the wire contract for this game. */
static const char * const rac3_readouts[] = {
	"Bolts",
	"Savefile helper",
	"Challenge mode",
	"Health XP",
	"Health",
	"Armour",
	"QE offset",
	"File time",
	"Ship colour",
	"Chargeboot front",
	"Chargeboot back",
	"Chargeboot tint"
};

#define NR (u8)(sizeof(rac3_readouts) / sizeof(rac3_readouts[0]))
#define NO FEATURE_NO_READOUT
#define WC FEATURE_FLAG_WRITES_CODE
#define SA FEATURE_FLAG_SAVE_ASIDE
#define LA FEATURE_FLAG_LOAD_ASIDE
/* Protocol 1.3: a toggle qwark reads back out of game memory. */
#define LV FEATURE_FLAG_LIVE
/* Protocol 1.7: the field behind this VALUE is two's complement, `bits` wide. */
#define SG FEATURE_FLAG_SIGNED

static const struct feature_desc rac3_features[] = {
	/* id, kind, group, aux (ENUM: options; VALUE: field bits), flags, readout, min, max, label */
	/*
	 * Build 53: the four cheats every game has lead Cheats, in the same order
	 * and spelt alike in all four but for Deadlocked's Refill ammo (the client
	 * draws them in this order). Fast loads are a code patch now, as RaC1's
	 * and RaC2's are (rac3.h, RAC3_FASTLOAD_SITE); until build 53 two game
	 * words, and no WRITES_CODE. Infinite ammo was "Freeze ammo": the label is
	 * RaC1's and RaC2's, and the patch is the same nop of the ammo store, so
	 * the ammo still does not go down.
	 */
	{ R3_FAST_LOADS,    FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Fast loads" },
	{ R3_GHOST,         FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Ghost Ratchet" },
	{ R3_FREEZE_AMMO,   FEATURE_TOGGLE, G_CHEATS,   0, WC, NO, 0, 0, "Infinite ammo" },
	{ R3_FREEZE_HEALTH, FEATURE_TOGGLE, G_CHEATS,   0, 0,  NO, 0, 0, "Freeze health" },
	/* Build 53: one-hit KO is retired, id 2 and all (rac3.h). */
	{ R3_QS_PAUSE,      FEATURE_TOGGLE, G_CHEATS,   0, LV, NO, 0, 0, "Quick-select pause" },

	{ R3_DIE,           FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Die" },
	{ R3_BOLTS,         FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_BOLTS,     0, 0, "Bolts" },
	{ R3_CHALLENGE,     FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_CHALLENGE, 0, 255, "Challenge mode" },
	/* Health XP is a signed 32-bit field, so the row says so and stays unbounded. */
	{ R3_HEALTH_XP,     FEATURE_VALUE,  G_PLAYER,  32, SG, RAC3_RO_HEALTH_XP, 0, 0, "Health XP" },
	{ R3_HEALTH,        FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_HEALTH,    0, 0, "Health" },
	{ R3_ARMOUR,        FEATURE_ENUM,   G_PLAYER,   RAC3_ARMOUR_COUNT, 0, RAC3_RO_ARMOUR, 0, RAC3_ARMOUR_COUNT - 1, "Armour" },
	{ R3_SHIP_COLOUR,   FEATURE_ENUM,   G_PLAYER,   RAC3_SHIP_COUNT,   0, RAC3_RO_SHIP,   0, RAC3_SHIP_COUNT - 1, "Ship colour" },
	{ R3_FILE_TIME,     FEATURE_VALUE,  G_PLAYER,   0, 0,  RAC3_RO_FILE_TIME, 0, 0, "File time" },
	/*
	 * The QE offset is the signed halfword at RAC3_QE_OFFSET, the one qeTextBox
	 * edited. min and max stay 0: the width is the range.
	 */
	{ R3_QE_OFFSET,     FEATURE_VALUE,  G_PLAYER,  16, SG, RAC3_RO_QE_OFFSET, 0, 0, "QE offset" },
	{ R3_VENDOR_QE,     FEATURE_ACTION, G_PLAYER,   0, 0,  NO, 0, 0, "Enable vendor QE" },

	/*
	 * Build 53: Setup NG+ manips, CC early setup, Untune bosses and Reset
	 * dropship health are retired, ids and all (rac3.h). The first, the second
	 * and the dropship are the manip groups below now.
	 */
	{ R3_UNLOCK_SKILL,   FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Unlock all skill points" },
	{ R3_RESET_SKILL,    FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset all skill points" },
	{ R3_UNLOCK_TITANIUM,FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Unlock all titanium bolts" },
	{ R3_RESET_TITANIUM, FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset all titanium bolts" },
	{ R3_UPGRADE_ALL,    FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Max all weapon levels" },
	{ R3_DOWNGRADE_ALL,  FEATURE_ACTION, G_PROGRESS, 0, 0, NO, 0, 0, "Reset all weapon levels" },

	/*
	 * Protocol 1.2: the two the save-file manager drives. The set-aside byte is
	 * the save manager's own, because RAC3Form's dedicated set-aside byte
	 * (0xD9FF02) only ever existed in a commented-out line.
	 */
	/*
	 * Protocol 1.9: one pair. "Save manager: load file" drove Gigahelper's
	 * tempsave reader and is retired with its id.
	 */
	{ R3_SET_ASIDE,  FEATURE_ACTION, G_SAVEFILE, 0, SA, NO, 0, 0, "Set aside file" },
	{ R3_LOAD_ASIDE, FEATURE_ACTION, G_SAVEFILE, 0, LA, NO, 0, 0, "Load set-aside file" },

	{ R3_CB_PRIMARY_FRONT, FEATURE_COLOR, G_COSMETICS, 0, 0, RAC3_RO_CB_FRONT,  0, 0, "Chargeboots primary front" },
	{ R3_CB_PRIMARY_BACK,  FEATURE_COLOR, G_COSMETICS, 0, 0, RAC3_RO_CB_BACK,   0, 0, "Chargeboots primary back" },
	{ R3_CB_TINT_FRONT,    FEATURE_COLOR, G_COSMETICS, 0, 0, RAC3_RO_CB_TINT,   0, 0, "Chargeboots tint" },

	/*
	 * Build 53: the trophy reset deletes the console's trophy data for this
	 * game and has the game re-read its trophies, on a thread of the savefile
	 * helper's, while it goes on running (rac3_panel.c). Up to the first
	 * hardware test the game froze for three seconds and the label said so.
	 *
	 * Last, and RAC3_TROPHY_ROWS of them, so that DESCRIBE can leave both out
	 * where they can never work (rac3_describe): they are the only rows of the
	 * Trophies group, so where to put them in the table changes nothing a
	 * client draws.
	 */
	{ R3_RESET_TROPHIES, FEATURE_ACTION, G_TROPHIES, 0, 0, NO, 0, 0, "Reset trophies" },
	{ R3_AUTO_TROPHIES,  FEATURE_TOGGLE, G_TROPHIES, 0, 0, NO, 0, 0, "Reset trophies on Veldin load" }
};

#define RAC3_TROPHY_ROWS 2

#undef NO
#undef WC
#undef SA
#undef LA
#undef LV
#undef SG

/* Build 53: an auto flag ticked for Freeze ammo stays with Infinite ammo. */
static const struct feature_former rac3_former[] = {
	{ R3_FREEZE_AMMO, "Freeze ammo" }
};

static const struct game_describe rac3_describe_table = {
	rac3_groups,   (u8)(sizeof(rac3_groups) / sizeof(rac3_groups[0])),
	rac3_readouts, NR,
	rac3_features, (u8)(sizeof(rac3_features) / sizeof(rac3_features[0])),
	0,             /* no toggle ships auto-flagged */
	rac3_former,   (u8)(sizeof(rac3_former) / sizeof(rac3_former[0]))
};

/* Build 53: the same, without the trophy rows at the end of the table. */
static const struct game_describe rac3_describe_no_trophies = {
	rac3_groups,   (u8)(sizeof(rac3_groups) / sizeof(rac3_groups[0])),
	rac3_readouts, NR,
	rac3_features, (u8)(sizeof(rac3_features) / sizeof(rac3_features[0]) - RAC3_TROPHY_ROWS),
	0,
	rac3_former,   (u8)(sizeof(rac3_former) / sizeof(rac3_former[0]))
};

#undef NR

/*
 * Build 53. Whether the trophy reset can work in this process at all: it needs
 * a platform that patches code - RPCS3 keeps its trophy data where qwark cannot
 * reach it, and the helper's thread is code (rac3_panel.c) - and the savefile
 * helper, which config.txt can switch off.
 */
static int rac3_trophies_offered(void)
{
	return plat_can_patch_code() && savefile_enabled();
}

/*
 * Build 53: where the trophy reset can never work, DESCRIBE leaves out its
 * button and its toggle rather than offer two rows that always answer
 * UNSUPPORTED, and that a client drew as usable: no feature flag says "never
 * here" short of WRITES_CODE, which a client offers again under RPCS3 once the
 * code switches are in. A client draws a group, and MANIP_LIST's Trophies
 * header, only around features DESCRIBE has, so both go with them; FEATURE_SET
 * and FEATURE_TRIGGER answer NOT_FOUND for ids DESCRIBE does not list.
 */
static const struct game_describe *rac3_describe(void)
{
	return rac3_trophies_offered() ? &rac3_describe_table : &rac3_describe_no_trophies;
}

/* --------------------------------------------------------------- toggles */

static int rac3_set_toggle(u8 id, int on)
{
	int rc;

	switch (id) {
	case R3_FREEZE_AMMO:   return classic_code_toggle(&rac3_ammo, SW_RAC3_FLAG_FREEZE_AMMO, on);
	case R3_FREEZE_HEALTH: return classic_health_toggle(&rac3_freeze_health, on);
	case R3_GHOST:         return classic_ghost(RAC3_GHOST_TIMER, on);
	case R3_QS_PAUSE:      return mem_write_u8(RAC3_QUICK_SELECT, on ? 1 : 0);
	case R3_FAST_LOADS:
		rc = classic_code_toggle(&rac3_fastload, SW_RAC3_FLAG_FAST_LOADS, on);
		if (rc == ST_OK) {
			g_fastload_on = (on != 0);
			/* Whatever a load forced, the checkbox has the say now. */
			g_fastload_forced = 0;
		}
		return rc;
	/*
	 * qwark-side state, read by rac3_file_load_watch. Build 53: the reset it
	 * asks for is refused under RPCS3 and with the savefile helper switched
	 * off (rac3_panel.c), so turning it on is too, rather than have it refused
	 * on every load into Veldin.
	 */
	case R3_AUTO_TROPHIES:
		if (on && !rac3_trophies_offered()) return ST_UNSUPPORTED;
		g_auto_trophies = (on != 0);
		return ST_OK;
	default:               return ST_NOT_FOUND;
	}
}

/*
 * Protocol 1.3. The quick-select pause is one byte the game owns, so its
 * checkbox follows memory rather than the last thing qwark wrote. It rides in
 * the state hot block already, but the poll reads it straight so a client that
 * connects mid-session does not have to wait for a decode. Tick thread only.
 */
static int rac3_toggle_read(u8 id, int *on)
{
	u8 b = 0;
	int rc;

	/* Build 50: the code switches' flag bytes, while the switches are in. */
	if (id == R3_FREEZE_AMMO) return classic_code_read(SW_RAC3_FLAG_FREEZE_AMMO, on);
	if (id == R3_FAST_LOADS) {
		rc = classic_code_read(SW_RAC3_FLAG_FAST_LOADS, on);
		/* A load's force is not the checkbox; see g_fastload_forced. */
		if (rc == ST_OK && g_fastload_forced) *on = g_fastload_on;
		return rc;
	}

	if (id != R3_QS_PAUSE) return ST_NOT_FOUND;

	rc = mem_read_u8(RAC3_QUICK_SELECT, &b);
	if (rc != ST_OK) return rc;

	*on = (b != 0);
	return ST_OK;
}

/* -------------------------------------------------------------- positions */

static int rac3_save_blob(u8 *blob, u8 *len)
{
	int rc = mem_read(RAC3_COORDS, blob, RAC3_POS_BLOB_LEN);
	if (rc != ST_OK) return rc;
	*len = RAC3_POS_BLOB_LEN;
	return ST_OK;
}

static int rac3_load_blob(const u8 *blob, u8 len)
{
	if (len == 0 || len > QWARK_MAX_BLOB) return ST_BAD_ARG;
	return mem_write(RAC3_COORDS, blob, len);
}

static int rac3_blob_xyz(const u8 *blob, u8 len, f32 out[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) out[i] = bef32_get(blob + i * 4);
	return ST_OK;
}

static int rac3_blob_set_xyz(u8 *blob, u8 len, const f32 xyz[3])
{
	int i;

	if (len < 12) return ST_BAD_ARG;
	for (i = 0; i < 3; i++) bef32_put(blob + i * 4, xyz[i]);
	return ST_OK;
}

/* ---------------------------------------------------------------- planets */

/*
 * RaC3's planet ids are one-based: RAC3Form turned the combo box index into
 * `index + 1` before handing it to LoadPlanet, and current_planet reports the
 * same numbering. PLANET_LIST index has to equal the id, so index 0 is a
 * placeholder that planet_load refuses; index 1 is Veldin, and so on.
 *
 * Ids 15 and 25 are gaps in the game's own numbering, not planets qwark failed
 * to name. rac3.cs called them "Unknown" and "Unknown2"; the client's autosplit
 * route file has `null` on both lines; and the community address list's planet
 * table runs 14 = Koros, 16 = Metropolis and 24 = Insomniac Museum, 26 =
 * Metropolis Battle, skipping them. They are named the way Deadlocked's list
 * names its own gaps, and they keep their places so the index stays the id.
 */
static const char * const rac3_planets[] = {
	"(none)",
	"Veldin",
	"Florana",
	"Starship Phoenix",
	"Marcadia",
	"Daxx",
	"Phoenix Rescue",
	"Annihilation Nation",
	"Aquatos",
	"Tyhrranosis",
	"Zeldrin Starport",
	"Obani Gemini",
	"Blackwater City",
	"Holostar",
	"Koros",
	"(unused)",
	"Metropolis",
	"Crash Site",
	"Aridia",
	"Qwark's Hideout",
	"Launch Site",
	"Obani Draco",
	"Command Center",
	"Holostar Clank",
	"Insomniac Museum",
	"(unused)",
	"Metropolis Rangers",
	"Aquatos Clank",
	"Aquatos Sewers",
	"Tyhrranosis Rangers",
	"Vid Comic 6",
	"Vid Comic 1",
	"Vid Comic 4",
	"Vid Comic 2",
	"Vid Comic 3",
	"Vid Comic 5",
	"Vid Comic 1 SE"
};

/*
 * Build 53. The planets in the order the game visits them, for a menu: the old
 * client's RAC3Form combo box, which listed them that way and parsed each name
 * back into its id, rather than rac3.cs planetsList, which is the numbering
 * above. Each mission level sits after the planet it belongs to (Aquatos's
 * Clank and sewer levels after Aquatos, Holostar's Clank level after
 * Holostar), the arena and ranger levels follow the story, and the vid comics
 * go by their own number. The combo box named Zeldrin Starport twice, after
 * Tyhrranosis and again after Obani Draco; it is listed once, at the first.
 * The placeholders go last, where the client's combo leaves them out anyway.
 */
static const u8 rac3_planet_order[] = {
	1,  2,  3,  4,  5,      /* Veldin, Florana, Starship Phoenix, Marcadia, Daxx */
	11, 12,                 /* Obani Gemini, Blackwater City */
	7,                      /* Annihilation Nation */
	8,  27, 28,             /* Aquatos, Aquatos Clank, Aquatos Sewers */
	9,  10,                 /* Tyhrranosis, Zeldrin Starport */
	13, 23,                 /* Holostar, Holostar Clank */
	21,                     /* Obani Draco */
	16, 17, 18, 19,         /* Metropolis, Crash Site, Aridia, Qwark's Hideout */
	6,  14, 22, 20,         /* Phoenix Rescue, Koros, Command Center, Launch Site */
	24,                     /* Insomniac Museum */
	29, 26,                 /* Tyhrranosis Rangers, Metropolis Rangers */
	31, 33, 34, 32, 35, 30, /* Vid Comics 1 to 6 */
	36,                     /* Vid Comic 1 SE */
	15, 25, 0               /* the two gaps and the placeholder */
};

u8 rac3_planet_count(void)
{
	return (u8)(sizeof(rac3_planets) / sizeof(rac3_planets[0]));
}

static const char * const *rac3_planet_names(u8 *count)
{
	*count = rac3_planet_count();
	return rac3_planets;
}

static int rac3_die(void)
{
	return classic_die_set_z(RAC3_COORDS);
}

static int rac3_moby_table(u32 *table_ptr_addr, u32 *table_end_ptr_addr, u16 *stride)
{
	*table_ptr_addr     = RAC3_MOBY_TABLE;
	*table_end_ptr_addr = RAC3_MOBY_TABLE_END;
	*stride             = RAC3_MOBY_STRIDE;
	return ST_OK;
}

/* ----------------------------------------------------------------- manips */


/*
 * Revision 1.21, build 53: UYA's manip groups (SPEC-manips.md section 2), every
 * write in the order the spec lists it.
 *
 * The five setup writes are rac3.cs SetupFile, the old "Setup NG+ manips"
 * button: Klunk's two tunings, Neffy's, the vid comic menu cursor (Ende) and
 * the CC help desk message. NG+ adds Vendor QE after them, and NG+ no QE never
 * writes it. CC Early refreshes its quit-exploit fake item and the Veldin
 * dropship health every run. "Untune bosses" belongs to no group and is gone.
 */
static const struct manip_write r3_setup[] = {
	MW_U32(RAC3_KLUNK_TUNING_1, 7),
	MW_U32(RAC3_KLUNK_TUNING_2, 3),
	MW_U32(RAC3_NEFFY_TUNING, 0xE),
	MW_U32(RAC3_VID_COMIC_MENU, 2),
	MW_U32(RAC3_CC_HELP_DESK, 1)
};

static const struct manip_write r3_vendor_qe[] = {
	MW_BYTES(RAC3_QE_OFFSET, rac3_vendor_qe, 2)
};

static const struct manip_write r3_cc_early[] = {
	MW_U32(RAC3_CC_FAKE_ITEM_A, 0x00000041u),
	MW_U32(RAC3_CC_FAKE_ITEM_B, 0x41414141u),
	MW_U32(RAC3_DROPSHIP_HEALTH, 100)
};

#define L(a) { a, (u8)(sizeof(a) / sizeof(a[0])) }
#define NONE { NULL, 0 }
#define NG   MANIP_FLAG_NGPLUS
#define EXT  MANIP_FLAG_EXTENSION

static const struct manip_group rac3_manip_groups[] = {
	{ 1, NG, "NG+ no QE", "NG+ no QE",
	  { L(r3_setup), NONE }, { NONE, NONE } },
	{ 2, NG, "NG+", "NG+",
	  { L(r3_setup), L(r3_vendor_qe) }, { NONE, NONE } },
	{ 3, NG | EXT, "NG+ CC Early", "NG+ CC Early",
	  { L(r3_setup), L(r3_vendor_qe) }, { L(r3_cc_early), NONE } }
};

#undef L
#undef NONE
#undef NG
#undef EXT

/* The trophy reset and its auto toggle, beside the manips. */
static const u8 rac3_trophies[] = { R3_RESET_TROPHIES, R3_AUTO_TROPHIES };

static const struct manip_header rac3_manip_headers[] = {
	{ "Trophies", rac3_trophies, (u8)(sizeof(rac3_trophies) / sizeof(rac3_trophies[0])) }
};

static const struct game_manips rac3_manips = {
	"rac3",
	rac3_manip_groups,
	(u8)(sizeof(rac3_manip_groups) / sizeof(rac3_manip_groups[0])),
	RAC3_CHALLENGE_MODE, 1,    /* the challenge counter, one byte */
	rac3_manip_headers,
	(u8)(sizeof(rac3_manip_headers) / sizeof(rac3_manip_headers[0])),
	rac3_in_file
};

/* ---------------------------------------------------------------- vtable */

const struct game_api rac3_game = {
	{ "NPEA00387", "BCES01503", NULL, NULL },
	GAME_RAC3,

	RAC3_FP_ADDR,
	rac3_fp,
	rac3_fp_patched,
	GAME_FP_SWITCH(rac3_fp_switch),
	NULL,                 /* the trampoline has never moved */
	sizeof(rac3_fp),

	0,                    /* no quit hook is known for RaC3 yet */
	RAC3_INPUTS,

	rac3_init,

	rac3_hot,
	(u8)(sizeof(rac3_hot) / sizeof(rac3_hot[0])),
	rac3_hot_decode,

	rac3_describe,

	rac3_set_toggle,
	rac3_toggle_read,
	rac3_trigger,
	rac3_set_value,
	rac3_get_options,

	rac3_save_blob,
	rac3_load_blob,
	rac3_blob_xyz,
	rac3_blob_set_xyz,
	RAC3_POS_BLOB_LEN,

	rac3_planet_names,
	rac3_planet_load,
	rac3_die,
	rac3_load_setaside,
	rac3_savefile_ready,  /* build 53: no planet load; a save to act on */

	rac3_unlock_list,
	rac3_unlock_read,
	rac3_unlock_set,

	rac3_levelflags_get,
	rac3_levelflags_reset,
	rac3_levelflags_set,

	rac3_moby_table,

	rac3_on_enter,
	NULL,                 /* on_quit */
	rac3_on_tick,

	rac3_autosplit_describe,

	&rac3_manips,         /* revision 1.21 */
	rac3_planet_order     /* build 53 */
};
