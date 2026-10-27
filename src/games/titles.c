/*
 * The title index (titles.h): every title id qwark knows, and what the core
 * needs of its game before the game's own code is there.
 *
 * Each of the four originals owns its own NPEA id. The disc trilogy BCES01503
 * hosts RaC1, RaC2 and RaC3 behind one title id, so it is registered on all
 * three and the session fingerprints each candidate in turn: whichever
 * executable is actually mapped answers, and the old disc-collection picker
 * goes away. Under game modules (src/games/module.h) the fingerprint is also
 * what says which module (rac1.sprx .. rac4.sprx) to load.
 */
#include "titles.h"
#include "rac1.h"
#include "rac2.h"
#include "rac3.h"
#include "rac4.h"
#if QWARK_HAS_TOD
#include "tod.h"
#endif
#include "switches_bins.h"
#include "../core/util.h"

/* --------------------------------------------------------- fingerprints */

/* See each game's header for what these are and why all of them are accepted. */
const u8 rac1_fp[4]         = { 0x30, 0x64, 0x9C, 0xE0 };
const u8 rac1_fp_patched[4] = { 0x30, 0x64, 0x00, 0x00 };

const u8 rac2_fp[4]         = { 0x4B, 0xFF, 0xEA, 0x69 };
const u8 rac2_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };
const u8 rac2_fp_switch[4]  = {
	(u8)(SW_RAC2_SITE_FAST_LOADS >> 24), (u8)(SW_RAC2_SITE_FAST_LOADS >> 16),
	(u8)(SW_RAC2_SITE_FAST_LOADS >> 8),  (u8)SW_RAC2_SITE_FAST_LOADS
};
const u8 rac2_fp_switch_old[4] = { 0x4B, 0x64, 0x89, 0x60 };

const u8 rac3_fp[4]         = { 0x7C, 0x85, 0x31, 0x2E };
const u8 rac3_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };
const u8 rac3_fp_switch[4]  = {
	(u8)(SW_RAC3_SITE_FREEZE_AMMO >> 24), (u8)(SW_RAC3_SITE_FREEZE_AMMO >> 16),
	(u8)(SW_RAC3_SITE_FREEZE_AMMO >> 8),  (u8)SW_RAC3_SITE_FREEZE_AMMO
};

const u8 rac4_fp[4]         = { 0x41, 0x82, 0x00, 0x0C };
const u8 rac4_fp_patched[4] = { 0x60, 0x00, 0x00, 0x00 };
const u8 rac4_fp_switch[4]  = {
	(u8)(SW_RAC4_SITE_REFILL_AMMO >> 24), (u8)(SW_RAC4_SITE_REFILL_AMMO >> 16),
	(u8)(SW_RAC4_SITE_REFILL_AMMO >> 8),  (u8)SW_RAC4_SITE_REFILL_AMMO
};

#if QWARK_HAS_TOD
/* Revision 1.27: stw r31,0x6e8(r11) in LoadLevel, which nothing patches (tod.h). */
const u8 tod_fp[4]          = { 0x93, 0xEB, 0x06, 0xE8 };
#endif

/* ------------------------------------------- Deadlocked's boot patches */

/*
 * The autosplitter's quit hook, verbatim:
 *   li   r3, 0xFF
 *   lis  r4, 0x0170
 *   stb  r3, 0(r4)
 * so the game writes 0xFF to RAC4_QUIT_FLAG on its way out.
 */
static const struct patch_word rac4_quit_hook_words[] = {
	{ RAC4_QUIT_HOOK_ADDR + 0, 0x386000FFu },
	{ RAC4_QUIT_HOOK_ADDR + 4, 0x3C800170u },
	{ RAC4_QUIT_HOOK_ADDR + 8, 0x98640000u }
};

/*
 * The autosplitter's loading hook, verbatim: ten words of trampoline that save
 * r3 and r4, store 0xFF at RAC4_LOADING_VAL, put them back and branch home,
 *
 *   stwu r1, -16(r1) / stw r3, 8(r1) / stw r4, 12(r1)
 *   li   r3, 0xFF    / lis r4, 0x0171 / stb r3, 0(r4)
 *   lwz  r3, 8(r1)   / lwz r4, 12(r1) / addi r1, r1, 16 / b -160
 *
 * followed by the branch into it at RAC4_LOADING_HOOK_1. The words go in as one
 * patch, trampoline first, so nothing can branch to a half-written cave.
 */
static const struct patch_word rac4_loading_hook_words[] = {
	{ RAC4_LOADING_HOOK_2 + 0x00, 0x9421FFF0u },
	{ RAC4_LOADING_HOOK_2 + 0x04, 0x90610008u },
	{ RAC4_LOADING_HOOK_2 + 0x08, 0x9081000Cu },
	{ RAC4_LOADING_HOOK_2 + 0x0C, 0x386000FFu },
	{ RAC4_LOADING_HOOK_2 + 0x10, 0x3C800171u },
	{ RAC4_LOADING_HOOK_2 + 0x14, 0x98640000u },
	{ RAC4_LOADING_HOOK_2 + 0x18, 0x80610008u },
	{ RAC4_LOADING_HOOK_2 + 0x1C, 0x8081000Cu },
	{ RAC4_LOADING_HOOK_2 + 0x20, 0x38210010u },
	{ RAC4_LOADING_HOOK_2 + 0x24, 0x4BFFFF60u },
	{ RAC4_LOADING_HOOK_1,        0x48000080u }
};

/*
 * Revision 1.26: the core's, not the game's. Both used to go in at
 * Deadlocked's on_enter; a game module may load seconds into the session, after
 * the SCE logo the loading hook watches for, so the core puts them in as the
 * session starts instead, in every build. Neither is ever reverted: the quit
 * hook is what tells the session a quit is coming, and a game that is quitting
 * is not a good time to write words back.
 */
static u32 g_rac4_quit_orig[3];
static u32 g_rac4_loading_orig[11];

static const struct patch_def rac4_quit_hook = {
	"Quit hook", PATCH_KIND_FEATURE, rac4_quit_hook_words, 3, g_rac4_quit_orig
};
static const struct patch_def rac4_loading_hook = {
	"Loading hook", PATCH_KIND_FEATURE, rac4_loading_hook_words, 11, g_rac4_loading_orig
};

/*
 * The flag bytes go to 0 right after their hooks, or a fresh boot reads
 * whatever was left in that page: the session would quit at once, and the
 * RESUME fire before the logo.
 */
static const struct game_boot_patch rac4_boot[] = {
	{ &rac4_quit_hook,    RAC4_QUIT_FLAG },
	{ &rac4_loading_hook, RAC4_LOADING_VAL }
};

/* ----------------------------------------------------------------- index */

#if QWARK_GAME_MODULES && !defined(QWARK_HOST)
#define LINKED(g) NULL
#else
extern const struct game_api rac1_game;
extern const struct game_api rac2_game;
extern const struct game_api rac3_game;
extern const struct game_api rac4_game;
#if QWARK_HAS_TOD
extern const struct game_api tod_game;
#endif
#define LINKED(g) (&(g))
#endif

#ifdef QWARK_TEST
/* A second title, so the host tests can switch titles (test/test_game.c). */
extern const struct game_api testgame_game;
extern const u8 testgame_fp[4];
#define TESTGAME_FP_ADDR 0x00200000u
#define TESTGAME_PAD     0x00300000u
#endif

static const struct game_title g_titles[] = {
	{
		{ "NPEA00385", "BCES01503", NULL, NULL }, GAME_RAC1,
		RAC1_FP_ADDR, rac1_fp, rac1_fp_patched,
		NULL,                 /* fp_switch: the old health switch hooks the word after it */
		NULL,                 /* so no earlier build's branch is there either */
		sizeof(rac1_fp),
		0,                    /* no quit hook is known for RaC1 yet */
		RAC1_INPUTS,
		NULL, 0, 0, 0, 0, 0,
		LINKED(rac1_game)
	},
	{
		{ "NPEA00386", "BCES01503", "RC2ILYOOO", "BLMODYOOO" }, GAME_RAC2,
		RAC2_FP_ADDR, rac2_fp, rac2_fp_patched,
		GAME_FP_SWITCH(rac2_fp_switch), GAME_FP_SWITCH(rac2_fp_switch_old),
		sizeof(rac2_fp),
		0,                    /* no quit hook is known for RaC2 yet */
		RAC2_INPUTS,
		NULL, 0, 0, 0, 0, 0,
		LINKED(rac2_game)
	},
	{
		{ "NPEA00387", "BCES01503", NULL, NULL }, GAME_RAC3,
		RAC3_FP_ADDR, rac3_fp, rac3_fp_patched,
		GAME_FP_SWITCH(rac3_fp_switch),
		NULL,                 /* the trampoline has never moved */
		sizeof(rac3_fp),
		0,                    /* no quit hook is known for RaC3 yet */
		RAC3_INPUTS,
		NULL, 0, 0, 0, 0, 0,
		LINKED(rac3_game)
	},
	{
		{ "NPEA00423", NULL, NULL, NULL }, GAME_RAC4,
		RAC4_FP_ADDR, rac4_fp, rac4_fp_patched,
		GAME_FP_SWITCH(rac4_fp_switch),
		NULL,                 /* the trampoline has never moved */
		sizeof(rac4_fp),
		RAC4_QUIT_FLAG,       /* the autosplitter's quit hook, a boot patch below */
		RAC4_INPUTS,
		rac4_boot, (u8)(sizeof(rac4_boot) / sizeof(rac4_boot[0])),
		R4_AS_QUIT, RAC4_LOADING_VAL,
		0, 0,                 /* the loading hook's byte is the gate */
		LINKED(rac4_game)
	}
#if QWARK_HAS_TOD
	/*
	 * Revision 1.27: Tools of Destruction, the PSN EU v01.00 alone (BCES00052
	 * is another executable, every address moved). No code switches and no
	 * boot patches, and its Lua helper goes in on demand (src/core/luacore.c).
	 * The quit byte is the game's own, set by SystemQuitCallback on its way out.
	 *
	 * Revision 1.28 (pending): the pause pair around a quit to the XMB, the
	 * old ToD autosplitter's. Its RESUME waits for the game's own front end
	 * (g_inFrontEnd, set by FrontEnd_Enter in the same frame the old SPRX's
	 * hook in Memory_InitHeaps fired), or a level's lua_State should the front
	 * end come and go unseen: no patch, so a console and RPCS3 time it alike.
	 */
	, {
		{ TOD_TITLE, NULL, NULL, NULL }, GAME_TOD,
		TOD_FP_ADDR, tod_fp, NULL,
		NULL, NULL,           /* no code switches */
		sizeof(tod_fp),
		TOD_QUIT_BYTE,
		TOD_PAD_MASK,
		NULL, 0,
		TOD_AS_QUIT, 0,       /* revision 1.28 (pending): the quit's pause pair, */
		TOD_FRONT_END, TOD_LUA_STATE, /* resumed on the game's own memory */
		LINKED(tod_game)
	}
#endif
#ifdef QWARK_TEST
	, {
		{ "TEST00001", NULL, NULL, NULL }, GAME_RAC2,
		TESTGAME_FP_ADDR, testgame_fp, NULL, NULL, NULL, 4,
		0, TESTGAME_PAD,
		NULL, 0, 0, 0, 0, 0,
		&testgame_game
	}
#endif
};

#define TITLE_COUNT ((u32)(sizeof(g_titles) / sizeof(g_titles[0])))

static int has_title(const struct game_title *t, const char *title_id)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (t->title_ids[i] == NULL) return 0;
		if (qstreq(t->title_ids[i], title_id)) return 1;
	}
	return 0;
}

u32 title_candidates(const char *title_id, const struct game_title **out, u32 cap)
{
	u32 i, n = 0;

	if (title_id == NULL || title_id[0] == 0 || out == NULL) return 0;
	for (i = 0; i < TITLE_COUNT && n < cap; i++) {
		if (has_title(&g_titles[i], title_id)) out[n++] = &g_titles[i];
	}
	return n;
}

const struct game_title *title_at(u32 index)
{
	return index < TITLE_COUNT ? &g_titles[index] : NULL;
}

const struct game_title *title_for_game(u8 game_id)
{
	u32 i;

	if (game_id == GAME_NONE) return NULL;
	for (i = 0; i < TITLE_COUNT; i++) {
		if (g_titles[i].game_id == game_id) return &g_titles[i];
	}
	return NULL;
}

/* Each game's module, by game id: the user's names, rac1.sprx .. rac4.sprx. */
static const char *const g_module_names[] = {
	NULL,                 /* GAME_NONE */
	"rac1.sprx",          /* GAME_RAC1 */
	"rac2.sprx",          /* GAME_RAC2 */
	"rac3.sprx",          /* GAME_RAC3 */
	"rac4.sprx"           /* GAME_RAC4 */
#if QWARK_HAS_TOD
	, "tod.sprx"          /* GAME_TOD, revision 1.27: a name of its own */
#endif
};

#define MODULE_NAMES ((u32)(sizeof(g_module_names) / sizeof(g_module_names[0])))

const char *title_module_name(u8 game_id)
{
	return game_id < MODULE_NAMES ? g_module_names[game_id] : NULL;
}

void title_module_path(u8 game_id, char *out, u32 cap, int dot_new)
{
	const char *name = title_module_name(game_id);

	if (cap == 0) return;
	out[0] = 0;
	if (name == NULL) return;
	qstrcpy(out, cap, GAME_MODULE_DIR);
	qstrcat(out, cap, name);
	if (dot_new) qstrcat(out, cap, ".new");
}
