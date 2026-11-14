/*
 * The fake console: the game half of the host platform for qwark-host.exe.
 *
 * Game memory is a sparse image of 64 KB pages allocated on first touch, so the
 * whole 32-bit address space is addressable and untouched pages read as zeros.
 * The console state (in game, PID, title) is driven from stdin by host_main.c.
 *
 * Everything that is not about the game - threads, files, sockets, time - lives
 * in plat_host.c and is shared with the PINE backend.
 */
#include "../plat.h"
#include "plat_host.h"
#include "../../games/game.h"
#include "../../games/titles.h"
#include "../../games/module.h"
#include "../../games/core_api.h"
#include "../../games/menuhelper_bins.h"
#include "../../games/tod.h"
#include "../../games/luahelper_patch.h"
#include "../../core/luamb.h"
#include "../../core/menubox.h"
#include "../../games/luahelper/lh_menu.h"
#include "../../games/luahelper/lh_tod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------ fake memory */

#define HOST_PAGE_SHIFT 16
#define HOST_PAGE_SIZE  (1u << HOST_PAGE_SHIFT)
#define HOST_PAGE_COUNT (1u << (32 - HOST_PAGE_SHIFT))

static u8 **g_pages;
static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_game_running;
static u32 g_game_pid;
static char g_game_title[16];
static u32 g_next_pid = 0x01000100;

/* Which game this boot seeded, so `pad` writes to the right address. */
static const struct game_title *g_boot_game;

/* The fake console is a console: it patches code and it is not an emulator. */
static int g_can_patch_code = 1;
static int g_is_emulator;

static void seed_tod(void);

static u8 *page_for(u32 index, int create)
{
	if (g_pages == NULL) return NULL;
	if (g_pages[index] == NULL && create) {
		g_pages[index] = (u8 *)calloc(1, HOST_PAGE_SIZE);
	}
	return g_pages[index];
}

static void mem_access(u32 addr, void *buf, u32 len, int write)
{
	u8 *p = (u8 *)buf;
	u32 done = 0;

	while (done < len) {
		u32 cur = addr + done;
		u32 index = cur >> HOST_PAGE_SHIFT;
		u32 offset = cur & (HOST_PAGE_SIZE - 1);
		u32 chunk = HOST_PAGE_SIZE - offset;
		u8 *page;

		if (chunk > len - done) chunk = len - done;

		page = page_for(index, write);
		if (write) {
			if (page != NULL) memcpy(page + offset, p + done, chunk);
		} else {
			if (page != NULL) memcpy(p + done, page + offset, chunk);
			else              memset(p + done, 0, chunk);
		}

		done += chunk;
	}
}

/* --------------------------------------------------------------- lifecycle */

int plat_init(void)
{
	if (host_common_init(NULL, "qwark-host-root") != 0) return -1;

	g_pages = (u8 **)calloc(HOST_PAGE_COUNT, sizeof(u8 *));
	if (g_pages == NULL) return -1;

	return 0;
}

void plat_shutdown(void)
{
	host_common_shutdown();
}

/* ------------------------------------------------------------ fake console */

/* Which of a multi-game title's candidates `which` names. 0 = the first one. */
static u8 host_wanted_game_id(const char *which)
{
	if (which == NULL) return 0;

	if (strcmp(which, "rac1") == 0 || strcmp(which, "1") == 0) return GAME_RAC1;
	if (strcmp(which, "rac2") == 0 || strcmp(which, "2") == 0) return GAME_RAC2;
	if (strcmp(which, "rac3") == 0 || strcmp(which, "3") == 0) return GAME_RAC3;
	if (strcmp(which, "rac4") == 0 || strcmp(which, "4") == 0 ||
	    strcmp(which, "dl") == 0)
		return GAME_RAC4;
	if (strcmp(which, "tod") == 0 || strcmp(which, "5") == 0) return GAME_TOD;

	return 0;
}

void host_boot_as(const char *title, const char *which)
{
	const struct game_title *cands[GAME_MAX_CANDIDATES];
	const struct game_title *game = NULL;
	u8 wanted;
	u32 n;
	u32 i;

	pthread_mutex_lock(&g_mem_lock);

	/* A fresh process means a fresh, empty memory image. */
	for (i = 0; i < HOST_PAGE_COUNT; i++) {
		if (g_pages[i] != NULL) { free(g_pages[i]); g_pages[i] = NULL; }
	}

	snprintf(g_game_title, sizeof(g_game_title), "%s", title);
	g_game_pid = g_next_pid++;
	g_game_running = 1;

	/*
	 * Only one candidate's fingerprint is seeded, so BOOTING finds exactly one
	 * match: that is how the simulator says which of the trilogy is running.
	 */
	n = title_candidates(g_game_title, cands, GAME_MAX_CANDIDATES);
	wanted = host_wanted_game_id(which);

	for (i = 0; i < n; i++) {
		if (wanted == 0 || cands[i]->game_id == wanted) { game = cands[i]; break; }
	}
	if (game == NULL && n > 0) game = cands[0];

	g_boot_game = game;

	if (game != NULL && game->fp_bytes != NULL && game->fp_len > 0) {
		mem_access(game->fp_addr, (void *)game->fp_bytes, game->fp_len, 1);
	}

	/*
	 * Revision 1.22. And the game's own words at every place its console menu
	 * renderer hooks, which a real executable has there: qwark's install looks
	 * at each before it writes anything, and refuses a word it does not know.
	 */
	if (game != NULL) {
		const struct mh_desc *mh = mh_desc_for_game(game->game_id);
		u8 w;

		for (w = 0; mh != NULL && w < mh->nhooks; w++) {
			u8 raw[4];
			be32_put(raw, mh->hooks[w].original);
			mem_access(mh->hooks[w].addr, raw, 4, 1);
		}
	}

	/* Revision 1.27: Tools of Destruction's level, hero and Lua state. */
	if (game != NULL && game->game_id == GAME_TOD) seed_tod();

	pthread_mutex_unlock(&g_mem_lock);

	plat_log("host: booted %s as pid %d%s", g_game_title, (int)g_game_pid,
	         game != NULL ? "" : " (no registered game, nothing seeded)");
}

/* ------------------------------------------- Tools of Destruction, rev 1.27 */

/*
 * A ToD boot is a level, as if the game were in gameplay: g_gameState 4, no
 * load asked for, a level object whose hero has a moby, 10 hit points, and a
 * lua_State. Every place the Lua helper hooks holds the game's own word, as
 * the executable does, unless `luapatch on` asked for the boot RPCS3 makes
 * with the helper's patch in, which writes the patch's words instead.
 */
#define FAKE_TOD_ROOT  0x30000000u
#define FAKE_TOD_HERO  0x30010000u
#define FAKE_TOD_MOBY  0x30020000u
#define FAKE_TOD_STATE 0x30030000u
/* Revision 1.29: the pause's menu system and its VM, while `fakepause` is on. */
#define FAKE_TOD_MS         0x30040000u
#define FAKE_TOD_MENU_STATE 0x30050000u
/* The crash fix: its Lua bridge, Anark runtime and presentation (lh_tod.h LH_MS_*). */
#define FAKE_TOD_BRIDGE     0x30060000u
#define FAKE_TOD_RUNTIME    0x30060100u
#define FAKE_TOD_PRES       0x30060200u
/* Revision 1.29, the galactic map (docs/tod-menu.md 17): its menu system and VM, while it is up. */
#define FAKE_TOD_MAP_MS     0x30080000u
#define FAKE_TOD_MAP_STATE  0x30090000u
/* 2026-11-14: the levels' names (Level_getName's table) and the five tiers of a level's state. */
#define FAKE_TOD_NAMES      0x300A0000u
#define FAKE_TOD_TIERS      0x300B0000u
#define FAKE_TOD_NTIERS     5u

static int g_lua_patch_at_boot;
static u8 g_lua_patch[QWARK_MAX_PAYLOAD];

void host_set_lua_patch(int on)
{
	g_lua_patch_at_boot = on ? 1 : 0;
}

static void poke32(u32 addr, u32 v)
{
	u8 b[4];

	be32_put(b, v);
	mem_access(addr, b, 4, 1);
}

static u32 peek32(u32 addr)
{
	u8 b[4];

	mem_access(addr, b, 4, 0);
	return be32_get(b);
}

/*
 * Revision 1.28: ToD's save system (docs/tod-savefile.md), as the
 * game's boot leaves it once its check has run: g_saveSystem pointing at the
 * manager, the manager idle with no thread, no slot yet, and g_saveData and the
 * manager's file image both holding a save (weapon slot i's id is i, level 1).
 */
static void tod_save_header(u32 base, u32 level)
{
	u32 i;

	for (i = 0; i < 4; i++) poke32(base + i * TOD_WEAPON_STRIDE, i);
	poke32(base + TOD_SAVE_LEVEL_OFF, level);
	poke32(base + TOD_SAVE_CHAL_OFF, 0);
}

static void tod_thread_none(u32 thread)
{
	poke32(TOD_SAVE_MANAGER + thread + TOD_ST_ID, 0);
	poke32(TOD_SAVE_MANAGER + thread + TOD_ST_ID + 4, TOD_THREAD_NONE);
	poke32(TOD_SAVE_MANAGER + thread + TOD_ST_STATE, 0);
}

/* Under g_mem_lock. */
static void seed_tod_save(void)
{
	poke32(TOD_SAVE_SYSTEM, TOD_SAVE_MANAGER);
	poke32(TOD_SAVE_MANAGER, TOD_SAVE_VTABLE);
	tod_thread_none(TOD_SM_SAVE_THREAD);
	tod_thread_none(TOD_SM_LOAD_THREAD);
	tod_thread_none(TOD_SM_DELETE_THREAD);
	poke32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_IDLE);
	poke32(TOD_SAVE_MANAGER + TOD_SM_SLOT, 0xFFFFFFFFu);
	poke32(TOD_SAVE_MANAGER + TOD_SM_RESULT, 0);
	poke32(TOD_SAVE_MANAGER + TOD_SM_AUTOSAVE, 0);
	tod_save_header(TOD_SAVE_DATA, 1);
	tod_save_header(TOD_SAVE_IMAGE, 1);

	/*
	 * Build 57's refusal: every real boot reaches gameplay through the title's
	 * LoadLevel, whose FrontEnd_requestLevel sets g_frontEndExitRequested, and
	 * nothing ever clears it again. The title itself is gone (g_inFrontEnd 0).
	 */
	{
		u8 one = 1, zero = 0;

		mem_access(TOD_FRONTEND_EXIT, &one, 1, 1);
		mem_access(TOD_FRONT_END, &zero, 1, 1);
	}
}

static u8 g_tod_copy[TOD_SAVE_SIZE];

/*
 * 2026-11-14: LoadLevel(level, 1, 0) outside play (tod.h, TOD_LEVEL_NAMES):
 * Level_setName, g_saveData.currentLevel, GameState_Request(3, 3). Under
 * g_mem_lock.
 */
static void fake_tod_load_now(u32 level)
{
	u32 count = peek32(TOD_TIER_STACK + TOD_TS_COUNT), name = peek32(TOD_LEVEL_NAMES + level * 4u), i;
	u8 one = 1;

	poke32(TOD_LEVEL_NAME_LAST, name);
	poke32(TOD_PREV_LEVEL_NAME, peek32(TOD_LEVEL_NAME));
	poke32(TOD_LEVEL_NAME, name);
	poke32(TOD_CURRENT_LEVEL, level);
	for (i = 0; i < TOD_LEVEL_LOAD_TIERS && i < count; i++)
		mem_access(peek32(TOD_TIER_STACK + TOD_TS_TIERS + (count - 1u - i) * 4u) + TOD_TIER_DYING, &one, 1, 1);
	poke32(TOD_REQUESTED_STATE, TOD_LEVEL_LOAD_STATE);
}

/*
 * One frame of SaveSystem_update, as far as the save manager's own words go
 * (src/games/tod_save.c's model of it): no thread is ever real here, so a load
 * thread id that is not none is one sys_ppu_thread_get_priority answers ESRCH
 * for, and its state 5 is applied as SaveManager_onLoaded does - the image
 * into g_saveData, then LoadLevel(level, 1, 0), which in gameplay is the load
 * request. Idle with an autosave asked for and a slot, it saves: g_saveData
 * into the image (SaveManager_copyForSave). Returns 1 when it loaded.
 */
int host_fake_tod_save_frame(void)
{
	u32 state, lt = TOD_SAVE_MANAGER + TOD_SM_LOAD_THREAD;
	int loaded = 0;

	pthread_mutex_lock(&g_mem_lock);
	if (!g_game_running || g_boot_game == NULL || g_boot_game->game_id != GAME_TOD ||
	    peek32(TOD_SAVE_SYSTEM) != TOD_SAVE_MANAGER) {
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}

	state = peek32(TOD_SAVE_MANAGER + TOD_SM_STATE);
	if (state == TOD_SS_LOADING) {
		if (peek32(lt + TOD_ST_ID + 4) != TOD_THREAD_NONE || peek32(lt + TOD_ST_ID) != 0) {
			if (peek32(lt + TOD_ST_STATE) == TOD_LT_READ_OK) {
				u32 level;

				mem_access(TOD_SAVE_IMAGE, g_tod_copy, TOD_SAVE_SIZE, 0);
				mem_access(TOD_SAVE_DATA, g_tod_copy, TOD_SAVE_SIZE, 1);
				level = peek32(TOD_SAVE_DATA + TOD_SAVE_LEVEL_OFF);
				if (peek32(TOD_GAME_STATE) == TOD_STATE_GAMEPLAY) poke32(TOD_LOAD_REQUEST, level);
				else if (level < TOD_NPLANETS) fake_tod_load_now(level);   /* 2026-11-14: the pause's own Load */
				poke32(lt + TOD_ST_STATE, 0);
				loaded = 1;
			}
			poke32(lt + TOD_ST_ID, 0);
			poke32(lt + TOD_ST_ID + 4, TOD_THREAD_NONE);
		}
		poke32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_IDLE);
	} else if (state == 2 || state == 4) {
		poke32(TOD_SAVE_MANAGER + TOD_SM_STATE, TOD_SS_IDLE);
	} else if (state == TOD_SS_IDLE && peek32(TOD_SAVE_MANAGER + TOD_SM_AUTOSAVE) != 0 &&
	           peek32(TOD_SAVE_MANAGER + TOD_SM_SLOT) != 0xFFFFFFFFu) {
		mem_access(TOD_SAVE_DATA, g_tod_copy, TOD_SAVE_SIZE, 0);
		mem_access(TOD_SAVE_IMAGE, g_tod_copy, TOD_SAVE_SIZE, 1);
		poke32(TOD_SAVE_MANAGER + TOD_SM_AUTOSAVE, 0);
	}
	pthread_mutex_unlock(&g_mem_lock);
	return loaded;
}

/*
 * The game's own save into `slot`, as a save from its menu or an autosave
 * leaves it: g_saveData into the image, the slot noted. -1 is a new game's
 * "no slot yet" again.
 */
void host_fake_tod_saved(u32 slot)
{
	pthread_mutex_lock(&g_mem_lock);
	if (slot != 0xFFFFFFFFu) {
		mem_access(TOD_SAVE_DATA, g_tod_copy, TOD_SAVE_SIZE, 0);
		mem_access(TOD_SAVE_IMAGE, g_tod_copy, TOD_SAVE_SIZE, 1);
	}
	poke32(TOD_SAVE_MANAGER + TOD_SM_SLOT, slot);
	pthread_mutex_unlock(&g_mem_lock);
}

/* Under g_mem_lock. */
static void seed_tod(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);
	u8 i;

	poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
	poke32(TOD_LOAD_REQUEST, 0xFFFFFFFFu);
	poke32(TOD_REQUESTED_STATE, TOD_REQUESTED_NONE);   /* 2026-11-13: no state change pending, as the game's data has it */
	poke32(TOD_CURRENT_LEVEL, 1);
	poke32(TOD_LEVEL_OBJ, FAKE_TOD_ROOT);
	poke32(FAKE_TOD_ROOT + TOD_HERO_PTR, FAKE_TOD_HERO);
	poke32(FAKE_TOD_HERO + TOD_HERO_MOBY, FAKE_TOD_MOBY);
	poke32(FAKE_TOD_HERO + TOD_HERO_HEALTH, 0x41200000u);       /* 10.0f */
	poke32(FAKE_TOD_HERO + TOD_HERO_MAX_HEALTH, 0x41200000u);
	poke32(TOD_LUA_STATE, FAKE_TOD_STATE);
	/*
	 * 2026-11-13: player 0's follow camera (tod.h, TOD_CAM_*), its third slot
	 * (the first two another type and none), looking at a point 1.5 above
	 * the hero from 6 behind and 2 up: target, offset, desired offset and
	 * desired position, and the camera's position.
	 */
	{
		u32 c0 = TOD_CAM_CLIENT + TOD_CAM_SLOTS, cam = c0 + 2u * TOD_CAM_SLOT_LEN;
		static const u32 st[16] = {
			0, 0, 0x3FC00000u, 0,                       /* target 0, 0, 1.5 */
			0, 0xC0C00000u, 0x40000000u, 0,             /* offset 0, -6, 2 */
			0, 0xC0C00000u, 0x40000000u, 0,             /* desired, the same */
			0, 0xC0C00000u, 0x40600000u, 0              /* desired pos 0, -6, 3.5 */
		};
		u32 k;

		poke32(c0 + TOD_CAM_TYPE, 1);
		poke32(c0 + TOD_CAM_SLOT_LEN + TOD_CAM_TYPE, 0);
		poke32(cam + TOD_CAM_TYPE, TOD_CAM_FOLLOW);
		for (k = 0; k < 16; k++) poke32(cam + TOD_CAM_STATE + k * 4, st[k]);
		poke32(cam + TOD_CAM_POS, 0);
		poke32(cam + TOD_CAM_POS + 4, 0xC0C00000u);
		poke32(cam + TOD_CAM_POS + 8, 0x40600000u);
		poke32(cam + TOD_CAM_POS + 12, 0);
		/* 2026-11-14: and the camera client 0 is using (ToDCamClient.active). */
		poke32(TOD_CAM_CLIENT + TOD_CAM_ACTIVE, cam);
	}
	/*
	 * 2026-11-14: Level_getName's table (a name per level), g_levelName the
	 * level's, and g_tierStack with a level's five tiers, none marked to go.
	 */
	{
		u32 k;
		char name[16];

		for (k = 0; k < TOD_NPLANETS; k++) {
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name), "level%02u", (unsigned)k);
			mem_access(FAKE_TOD_NAMES + k * 0x20u, name, sizeof(name), 1);
			poke32(TOD_LEVEL_NAMES + k * 4u, FAKE_TOD_NAMES + k * 0x20u);
		}
		poke32(TOD_LEVEL_NAME, FAKE_TOD_NAMES + 1u * 0x20u);
		poke32(TOD_TIER_STACK + TOD_TS_COUNT, FAKE_TOD_NTIERS);
		for (k = 0; k < FAKE_TOD_NTIERS; k++) {
			u8 zero = 0;

			poke32(TOD_TIER_STACK + TOD_TS_TIERS + k * 4u, FAKE_TOD_TIERS + k * 0x40u);
			mem_access(FAKE_TOD_TIERS + k * 0x40u + TOD_TIER_DYING, &zero, 1, 1);
		}
	}
	/* A level up after its load: g_levelLoadTier loaded, the tier loader idle (revision 1.29). */
	{
		u8 one = 1;

		mem_access(TOD_LEVEL_LOAD_TIER + TOD_TIER_LOADED, &one, 1, 1);
		poke32(TOD_LOAD_JOB, 0);
	}
	seed_tod_save();

	if (d == NULL) return;
	for (i = 0; i < d->nhooks; i++) poke32(d->hooks[i].addr, d->hooks[i].orig);

	if (g_lua_patch_at_boot) {
		u32 len = 0, n, k;

		if (lh_patch_build(d, g_lua_patch, sizeof(g_lua_patch), &len) != ST_OK) return;
		n = be16_get(g_lua_patch);
		for (k = 0; k < n; k++) {
			const u8 *row = g_lua_patch + LH_PATCH_HEAD + k * LH_PATCH_ROW;
			mem_access(be32_get(row), (void *)(row + 4), 4, 1);
		}
	}
}

/*
 * The Lua helper's lh_serve (src/games/luahelper/luahelper.c), with a
 * stand-in for Lua that knows a handful of chunks: `_VERSION` answers "Lua
 * 5.0.2", `time_set_scale(0.5` and `time_set_scale(1` set the game clock's
 * scale, `error(` is a runtime error, `while true` never comes back (the
 * helper stays inside it), and anything else answers nothing. Revision 1.29:
 * a request for the other VM (target) is left; a chunk that defines
 * qwark_frame (the console menu's glue) says so in *glue; the helper's look
 * (lh_diag.lua, the third console test) answers FAKE_DIAG_OUT, a line longer
 * than a log line holds. Under g_mem_lock. Returns 1 when it answered a
 * request.
 */
#define FAKE_DIAG_OUT "g=301,301,1,0,-,0 s=uuutffn sl=nil reg=t,23,23,4 blankHandler: optHandler:+ " 	"controlHandler: moveHandler: helpHandler: TextScroller: invhandler: skillHandler: itemhandler: " 	"slideStepper: quickHandler: cineListUpdater: ScrollableList:+ skinHandler: gadgetHandler: " 	"weaponHandler: conceptHandler: ListUpdater:+ ScrollableList: ListUpdater:mh+ end"
/* Revision 1.29: a gmap() that opens nothing (host_fake_map_off). */
static volatile int g_fake_map_off;
/* Revision 1.29, every planet: how the map's glue answers (host_fake_map_glue): 0 in, 1 wait, 2 an error. */
static volatile int g_fake_map_glue;

static int fake_lua_serve(u32 mb, u32 L, u8 target, int *glue)
{
	/* `fakepause tight` (the second crash): a menu request finds the pool short of a load's room (glue NULL). */
	int no_room = target == LUAMB_TARGET_MENU && glue == NULL;
	u32 req, ack, cancel, len, out_len = 0;
	s32 status = LUAMB_ST_OK;
	u8 type = LUAMB_T_NONE, t = 0;
	const char *out = "";
	static char chunk[LUAMB_CHUNK_CAP + 1];

	req = peek32(mb + LUAMB_OFF_REQ);
	ack = peek32(mb + LUAMB_OFF_ACK);
	if (req == ack || peek32(mb + LUAMB_OFF_BUSY) == req) return 0;
	mem_access(mb + LUAMB_OFF_TARGET, &t, 1, 0);
	if (t != target) return 0;
	poke32(mb + LUAMB_OFF_BUSY, req);
	cancel = peek32(mb + LUAMB_OFF_CANCEL);
	len = peek32(mb + LUAMB_OFF_CHUNK_LEN);

	if (cancel == req) status = LUAMB_ST_CANCELLED;
	else if (L == 0) status = LUAMB_ST_NO_STATE;
	else if (len == 0 || len > LUAMB_CHUNK_CAP) status = LUAMB_ST_BAD_LEN;
	else if (no_room) {
		status = LUAMB_ST_NO_ROOM;
		poke32(LM_STATE + LM_SHORT, peek32(LM_STATE + LM_SHORT) + 1u);
	} else {
		mem_access(mb + LUAMB_OFF_CHUNK, chunk, len, 0);
		chunk[len] = 0;
		if (strstr(chunk, "while true") != NULL) {
			/* Inside the chunk for ever: busy stays up, no ack. */
			return 0;
		}
		/* Revision 1.29: the gameplay package's gmap(), GalMap_request: the map up (GalMapState_enter). */
		if (target == LUAMB_TARGET_GAMEPLAY && strstr(chunk, "gmap()") != NULL && !g_fake_map_off) {
			const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);

			poke32(TOD_GAME_STATE, TOD_STATE_GALMAP);
			poke32(TOD_GALMAP_PICK, 0xFFFFFFFFu);
			/* Its menu system (GalMap_loadMenu), with a VM. */
			poke32(d->map_ptr, FAKE_TOD_MAP_MS);
			poke32(FAKE_TOD_MAP_MS + d->menu_off, FAKE_TOD_MAP_STATE);
		}
		if (strstr(chunk, "time_set_scale(0.5") != NULL) poke32(TOD_GAME_CLOCK, 0x3F000000u);
		else if (strstr(chunk, "time_set_scale(1") != NULL) poke32(TOD_GAME_CLOCK, 0x3F800000u);
		if (glue != NULL && strstr(chunk, "function qwark_frame()") != NULL) *glue = 1;
		/* The glue raises errors of its own inside qwark_frame, which loading it never runs. */
		if (target == LUAMB_TARGET_MAP && strstr(chunk, "levelArray") != NULL) {
			/* Revision 1.29, every planet: the map's glue (lh_gmap.lua) answers as host_fake_map_glue says. */
			if (g_fake_map_glue == 2) {
				status = LUAMB_ST_ERRRUN;
				out = "qwark:1: x";
			} else {
				out = g_fake_map_glue == 1 ? "wait" : "in23";
			}
			type = LUAMB_T_STRING;
		} else if (strstr(chunk, "error(") != NULL && strstr(chunk, "function qwark_frame()") == NULL) {
			status = LUAMB_ST_ERRRUN;
			out = "qwark:1: x";
			type = LUAMB_T_STRING;
		} else if (strstr(chunk, "_VERSION") != NULL) {
			out = "Lua 5.0.2";
			type = LUAMB_T_STRING;
		} else if (strstr(chunk, "qwark_state") != NULL && strstr(chunk, "table.concat") != NULL) {
			out = FAKE_DIAG_OUT;
			type = LUAMB_T_STRING;
		}
	}

	out_len = (u32)strlen(out);
	mem_access(mb + LUAMB_OFF_OUT, (void *)out, out_len + 1, 1);
	poke32(mb + LUAMB_OFF_OUT_LEN, out_len);
	{
		u8 tf[2] = { type, 0 };
		mem_access(mb + LUAMB_OFF_OUT_TYPE, tf, 2, 1);
	}
	poke32(mb + LUAMB_OFF_STATUS, (u32)status);
	poke32(mb + LUAMB_OFF_ACK, req);
	poke32(mb + LUAMB_OFF_BUSY, 0);
	return 1;
}

/* Whether the simulated ToD has the Lua helper in (its VM hook, the mailbox's identity). Under g_mem_lock. */
static const struct lh_desc *fake_lua_in(void)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);

	if (d == NULL || !g_game_running || g_boot_game == NULL || g_boot_game->game_id != GAME_TOD ||
	    peek32(d->hooks[0].addr) != d->hooks[0].value || peek32(d->mailbox) != LUAMB_MAGIC)
		return NULL;
	return d;
}

/*
 * One gameplay frame of the Lua helper, after luahelper.c's lh_tick: the
 * heartbeat, the lua_State, the console menu's bookkeeping of a gameplay
 * frame (lh_menu.c's lm_gameplay_frame: no pause menu, so none of its VM and
 * the menu closed) and a gameplay request. Returns 1 when it answered one.
 */
int host_fake_lua_frame(void)
{
	const struct lh_desc *d;
	u32 mb, L;
	int answered;

	pthread_mutex_lock(&g_mem_lock);
	d = fake_lua_in();
	if (d == NULL) {
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	/* Revision 1.29: no gameplay frame while the galactic map is up (GameState_Dispatch runs the map's alone). */
	if (peek32(TOD_GAME_STATE) == TOD_STATE_GALMAP) {
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	mb = d->mailbox;
	L = peek32(d->vm + d->vm_state);
	poke32(mb + LUAMB_OFF_HEARTBEAT, peek32(mb + LUAMB_OFF_HEARTBEAT) + 1u);
	poke32(mb + LUAMB_OFF_LAST_STATE, L);
	poke32(LM_STATE + LM_L, 0);
	{
		u8 z = 0;
		mem_access(LM_STATE + LM_TRIED, &z, 1, 1);
		if (peek32(LM_BOX) == MBOX_MAGIC) {
			u8 closed = MBOX_HSTATE_CLOSED;
			mem_access(LM_BOX + MBOX_OFF_HSTATE, &closed, 1, 1);
		}
	}
	answered = fake_lua_serve(mb, L, LUAMB_TARGET_GAMEPLAY, NULL);
	pthread_mutex_unlock(&g_mem_lock);
	return answered;
}

/*
 * Revision 1.29, the galactic map (docs/tod-menu.md 17): one frame of the
 * map, after luahelper.c's lh_menu_tick for the map's menu system: the menu
 * heartbeat, and a request of target MAP served. Nothing else: the helper
 * runs nothing in a map but such a request. Returns 1 when it answered one.
 */
int host_fake_map_frame(void)
{
	const struct lh_desc *d;
	u32 ms = 0;
	int answered;

	pthread_mutex_lock(&g_mem_lock);
	d = fake_lua_in();
	if (d != NULL) ms = peek32(d->map_ptr);
	if (d == NULL || peek32(TOD_GAME_STATE) != TOD_STATE_GALMAP || ms == 0 ||
	    peek32(d->hooks[4].addr) != d->hooks[4].value) {
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	poke32(d->mailbox + LUAMB_OFF_MENU_BEAT, peek32(d->mailbox + LUAMB_OFF_MENU_BEAT) + 1u);
	answered = fake_lua_serve(d->mailbox, peek32(ms + d->menu_off), LUAMB_TARGET_MAP, NULL);
	pthread_mutex_unlock(&g_mem_lock);
	return answered;
}

void host_fake_map_glue(int how)
{
	g_fake_map_glue = how;
}

/*
 * Revision 1.29: one frame of the pause menu, after luahelper.c's
 * lh_menu_tick, while `fakepause` is on: the menu heartbeat, a new pause
 * menu VM given its qw and asking for the glue (LM_GLUE WANTED, LM_LOADS
 * moved), a menu request served, and the glue taken for loaded once a chunk
 * that defines qwark_frame ran. Returns 1 when it answered a request.
 *
 * The second crash (docs/tod-menu.md 10): the pause menu's Lua pool, which
 * lh_menu.c's lm_room counts, as `fakepause short` and `fakepause tight` set
 * it. Short: too little even after a collection, so no frame goes in (LM_WHY
 * MEMORY), a collection every LM_ROOM_HOLD frames counts in LM_SHORT, and a
 * glue that was in stops (LM_GLUE STOPPED, as lh_menu_quit leaves it). Tight:
 * enough for a frame of the glue, not for its load, so a menu request is
 * answered NO_ROOM after a collection that did not help.
 */
static volatile int g_fake_pause_on;
static volatile int g_fake_pool;     /* 0 room, 1 short, 2 tight */
static u32 g_fake_pool_frames;
/*
 * Revision 1.29, the fifth console test (docs/tod-menu.md 13): the glue
 * taking qwark's close (exit_pause_menu) and the pause going away as the
 * game's does: its state 4 (closing) a few frames, then 0 for a dozen more
 * with the game still in its pause and nothing queued (the frame before
 * GameState_requestGameplay, longer here so a load run in it would be
 * seen), then play. `fakepause hold`: a glue that never takes a request.
 */
static volatile int g_fake_hold;
static int g_fake_closing;
#define FAKE_CLOSE_DOWN 3
#define FAKE_CLOSE_GAP  12
static void fake_pause_words(int on);

int host_fake_menu_frame(void)
{
	const struct lh_desc *d;
	u32 mb, ms, L;
	int answered, glue = 0;
	u8 tried = 0;

	pthread_mutex_lock(&g_mem_lock);
	d = fake_lua_in();
	if (d == NULL || peek32(d->hooks[2].addr) != d->hooks[2].value) {
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	if (g_fake_closing > 0) {
		/* The fifth console test: a pause the glue closed, going away as the game's does. */
		g_fake_closing++;
		if (g_fake_closing > 1 + FAKE_CLOSE_DOWN + FAKE_CLOSE_GAP) {
			fake_pause_words(0);
			g_fake_closing = 0;
			g_fake_pause_on = 0;
		} else if (g_fake_closing > 1 + FAKE_CLOSE_DOWN) {
			poke32(TOD_PAUSE_STATE, 0);
		}
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	mb = d->mailbox;
	poke32(mb + LUAMB_OFF_MENU_BEAT, peek32(mb + LUAMB_OFF_MENU_BEAT) + 1u);
	ms = peek32(d->menu_ptr);
	L = ms != 0 ? peek32(ms + d->menu_off) : 0;
	/*
	 * The crash fix: lh_menu.c's lm_ready, in short. A menu system whose
	 * presentation's first update has not run is never entered; the frames
	 * kept out are counted (LM_WHY, LM_WAITS).
	 */
	if (L == 0 || peek32(ms + LH_MS_PRES) == 0 || (peek32(ms + (LH_MS_FIRST & ~3u)) & 0xFFu) != 0) {
		poke32(LM_STATE + LM_WHY, L == 0 || peek32(ms + LH_MS_PRES) == 0 ? LM_WHY_PARTS : LM_WHY_FIRST);
		poke32(LM_STATE + LM_WAITS, peek32(LM_STATE + LM_WAITS) + 1u);
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	if (g_fake_pool == 1) {
		poke32(LM_STATE + LM_ROOM, (2u << 16) | 300u);
		if (g_fake_pool_frames++ % LM_ROOM_HOLD == 0) {
			poke32(LM_STATE + LM_SHORT, peek32(LM_STATE + LM_SHORT) + 1u);
			if (peek32(LM_STATE + LM_GLUE) == LM_GLUE_LOADED) poke32(LM_STATE + LM_GLUE, LM_GLUE_STOPPED);
		}
		poke32(LM_STATE + LM_WHY, LM_WHY_MEMORY);
		poke32(LM_STATE + LM_WAITS, peek32(LM_STATE + LM_WAITS) + 1u);
		pthread_mutex_unlock(&g_mem_lock);
		return 0;
	}
	poke32(LM_STATE + LM_ROOM, g_fake_pool == 2 ? (20u << 16) | 1500u : (150u << 16) | 4000u);
	poke32(LM_STATE + LM_WHY, 0);
	poke32(LM_STATE + LM_WAITS, 0);
	if (peek32(LM_STATE + LM_L) != L || peek32(LM_STATE + LM_MS) != ms) {
		poke32(LM_STATE + LM_L, L);
		poke32(LM_STATE + LM_MS, ms);
		mem_access(LM_STATE + LM_TRIED, &tried, 1, 1);
	}
	mem_access(LM_STATE + LM_TRIED, &tried, 1, 0);
	if (!tried) {
		tried = 1;
		mem_access(LM_STATE + LM_TRIED, &tried, 1, 1);
		poke32(LM_STATE + LM_LOADS, peek32(LM_STATE + LM_LOADS) + 1u);
		poke32(LM_STATE + LM_GLUE, LM_GLUE_WANTED);
	}
	answered = fake_lua_serve(mb, L, LUAMB_TARGET_MENU, g_fake_pool == 2 ? NULL : &glue);
	if (glue && peek32(LM_STATE + LM_GLUE) == LM_GLUE_WANTED) poke32(LM_STATE + LM_GLUE, LM_GLUE_LOADED);
	/* The fifth console test: the glue in takes qwark's close (lm_take_request), leaves the menu and the pause. */
	if (!answered && !g_fake_hold && peek32(LM_STATE + LM_GLUE) == LM_GLUE_LOADED && peek32(LM_BOX) == MBOX_MAGIC) {
		u8 req = 0;

		mem_access(LM_BOX + MBOX_OFF_REQUEST, &req, 1, 0);
		if (req == MBOX_REQ_CLOSE) {
			u8 b = MBOX_REQ_NONE;

			mem_access(LM_BOX + MBOX_OFF_REQUEST, &b, 1, 1);
			b = MBOX_HSTATE_CLOSED;
			mem_access(LM_BOX + MBOX_OFF_HSTATE, &b, 1, 1);
			poke32(TOD_PAUSE_STATE, 4);
			g_fake_closing = 1;
		}
	}
	pthread_mutex_unlock(&g_mem_lock);
	return answered;
}

/*
 * `fakepause on`: the game in its pause menu, a menu system with a VM; off,
 * none. 2 (wait): one whose presentation never comes up. 3 (short) and 4
 * (tight): one whose Lua pool is short (host_fake_menu_frame). 5 (hold, the
 * fifth console test): one whose glue never takes a request.
 */
void host_set_fake_pause(int on)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);

	if (d == NULL) return;
	g_fake_pool = on == 3 ? 1 : on == 4 ? 2 : 0;
	g_fake_pool_frames = 0;
	g_fake_hold = on == 5;
	if (on >= 3) on = 1;
	pthread_mutex_lock(&g_mem_lock);
	g_fake_closing = 0;
	fake_pause_words(on);
	pthread_mutex_unlock(&g_mem_lock);
	g_fake_pause_on = on ? 1 : 0;
}

/*
 * Revision 1.29: the galactic map going, as GalMapState_exit leaves things:
 * play again, and for a pick the level in selectedLevel and, through
 * LoadLevel, in g_planetLoadRequest.
 */
void host_fake_map(int level)
{
	pthread_mutex_lock(&g_mem_lock);
	if (peek32(TOD_GAME_STATE) == TOD_STATE_GALMAP) {
		const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);

		poke32(TOD_GALMAP_PICK, level < 0 ? 0xFFFFFFFFu : (u32)level);
		if (level >= 0) poke32(TOD_LOAD_REQUEST, (u32)level);
		poke32(TOD_GAME_STATE, TOD_STATE_GAMEPLAY);
		/* GalMapState_exit frees its menu system. */
		poke32(d->map_ptr, 0);
	}
	pthread_mutex_unlock(&g_mem_lock);
}

void host_fake_map_off(int off)
{
	g_fake_map_off = off ? 1 : 0;
}

/* The pause's words, as host_set_fake_pause wants them. Under g_mem_lock. */
static void fake_pause_words(int on)
{
	const struct lh_desc *d = lh_desc_for_title(TOD_TITLE);

	if (d == NULL) return;
	poke32(d->menu_ptr, on ? FAKE_TOD_MS : 0);
	poke32(FAKE_TOD_MS + d->menu_off, on ? FAKE_TOD_MENU_STATE : 0);
	/* The crash fix: its presentation up (lm_ready), or (2) never, its first update still to come. */
	poke32(FAKE_TOD_MS + LH_MS_BRIDGE, on ? FAKE_TOD_BRIDGE : 0);
	poke32(FAKE_TOD_MS + LH_MS_RUNTIME, on ? FAKE_TOD_RUNTIME : 0);
	poke32(FAKE_TOD_MS + LH_MS_PRES, on ? FAKE_TOD_PRES : 0);
	poke32(FAKE_TOD_MS + LH_MS_FRAMES, on ? 2u : 0u);
	poke32(FAKE_TOD_MS + (LH_MS_FIRST & ~3u), on == 2 ? 1u : 0u);
	poke32(FAKE_TOD_RUNTIME + LH_RT_PUSHED, 0);
	poke32(FAKE_TOD_BRIDGE + LH_BR_PRES, 0);
	poke32(TOD_PAUSE_STATE, on ? TOD_PAUSE_UP : 0);
	poke32(TOD_GAME_STATE, on ? TOD_STATE_PAUSE : TOD_STATE_GAMEPLAY);
}

static volatile int g_fake_lua_on;
static volatile int g_fake_save_on;
static int g_fake_lua_started;
static pthread_t g_fake_lua_thread;

static void *fake_lua_loop(void *arg)
{
	(void)arg;
	for (;;) {
		/* Revision 1.29: in the pause, the pause menu's frame runs and no gameplay frame does. */
		if (g_fake_lua_on && !g_fake_pause_on) host_fake_lua_frame();
		if (g_fake_lua_on && g_fake_pause_on) host_fake_menu_frame();
		/* Revision 1.29: the galactic map's frame (no gameplay frame runs while it is up). */
		if (g_fake_lua_on && !g_fake_pause_on) host_fake_map_frame();
		if (g_fake_save_on) host_fake_tod_save_frame();
		plat_sleep_us(16000);
	}
	return NULL;
}

static void fake_frames_start(void)
{
	if (!g_fake_lua_started) {
		g_fake_lua_started = 1;
		pthread_create(&g_fake_lua_thread, NULL, fake_lua_loop, NULL);
		pthread_detach(g_fake_lua_thread);
	}
}

/* Revision 1.28: ToD's save system a frame every 16 ms, on the same thread. */
void host_set_fake_tod_save(int on)
{
	g_fake_save_on = on ? 1 : 0;
	if (on) fake_frames_start();
}

/* A gameplay frame every 16 ms from a thread of its own, for the simulator. */
void host_set_fake_lua(int on)
{
	g_fake_lua_on = on ? 1 : 0;
	if (on && !g_fake_lua_started) {
		g_fake_lua_started = 1;
		pthread_create(&g_fake_lua_thread, NULL, fake_lua_loop, NULL);
		pthread_detach(g_fake_lua_thread);
	}
}

void host_boot(const char *title)
{
	host_boot_as(title, NULL);
}

void host_quit(void)
{
	pthread_mutex_lock(&g_mem_lock);
	g_game_running = 0;
	g_game_pid = 0;
	g_game_title[0] = 0;
	g_boot_game = NULL;
	pthread_mutex_unlock(&g_mem_lock);

	plat_log("host: game quit");
}

int host_set_pad(u32 mask)
{
	u8 raw[4];

	/*
	 * The seeded game, not the first candidate for the title: under BCES01503
	 * the three games keep their pad masks at three different addresses.
	 */
	if (!g_game_running) return -1;
	if (g_boot_game == NULL || g_boot_game->pad_mask_addr == 0) return -1;

	be32_put(raw, mask);
	return host_poke(g_boot_game->pad_mask_addr, raw, 4);
}

int host_poke(u32 addr, const u8 *data, u32 len)
{
	pthread_mutex_lock(&g_mem_lock);
	mem_access(addr, (void *)data, len, 1);
	pthread_mutex_unlock(&g_mem_lock);
	return 0;
}

int host_peek(u32 addr, u8 *out, u32 len)
{
	pthread_mutex_lock(&g_mem_lock);
	mem_access(addr, out, len, 0);
	pthread_mutex_unlock(&g_mem_lock);
	return 0;
}

/*
 * Revision 1.22. A pointer to one byte of the fake game, the page made if it is
 * not there yet, so the console menu's renderer, compiled for the PC
 * (test/test_menu.c), runs on the bytes qwark reads and writes.
 */
u8 *host_mem_ptr(u32 addr)
{
	u8 *page;

	pthread_mutex_lock(&g_mem_lock);
	page = page_for(addr >> HOST_PAGE_SHIFT, 1);
	pthread_mutex_unlock(&g_mem_lock);
	return page != NULL ? page + (addr & (HOST_PAGE_SIZE - 1)) : NULL;
}

void host_set_can_patch_code(int on)
{
	g_can_patch_code = on ? 1 : 0;
}

void host_set_emulator(int on)
{
	g_is_emulator = on ? 1 : 0;
}

/*
 * Every time the core asks for the title, which on a console is a call into the
 * XMB's game_plugin. The boot test counts them: that call stays out of the first
 * second of a launch.
 */
static u32 g_title_calls;
static u32 g_pid_calls;
static u32 g_presence_calls;

u32 host_pid_calls(void) { return g_pid_calls; }
u32 host_presence_calls(void) { return g_presence_calls; }

int plat_game_running(void)
{
	g_presence_calls++;
	return g_game_running;
}

u32 host_title_calls(void)
{
	return g_title_calls;
}

u32 plat_game_pid(void)
{
	g_pid_calls++;
	return g_game_pid;
}

int plat_game_title(char out[16])
{
	g_title_calls++;
	snprintf(out, 16, "%s", g_game_title);
	return out[0] != 0;
}

int plat_can_patch_code(void)
{
	return g_can_patch_code;
}

/*
 * Revision 1.31, the VSH pad probe: the fake VSH's libpad, with libpad's len
 * rule (a change is handed over once, then len 0 until the next change).
 */
static int g_vpad_rc = PLAT_PAD_NONE;
static struct plat_pad g_vpad;
static u8 g_vpad_fresh;
static u32 g_vpad_port;
static u32 g_vpad_system_info;
static u32 g_vpad_reads;

void host_set_vsh_pad(int rc, u8 digital1, u8 digital2, const u8 stick[4])
{
	int i;

	pthread_mutex_lock(&g_mem_lock);
	g_vpad_rc = rc;
	memset(&g_vpad, 0, sizeof(g_vpad));
	g_vpad.digital1 = digital1;
	g_vpad.digital2 = digital2;
	for (i = 0; i < 4; i++) g_vpad.stick[i] = stick != NULL ? stick[i] : 0x80;
	g_vpad_fresh = (rc == 0);
	pthread_mutex_unlock(&g_mem_lock);
}

void host_set_vsh_pad_port(u32 port, u32 system_info)
{
	pthread_mutex_lock(&g_mem_lock);
	g_vpad_port = port;
	g_vpad_system_info = system_info;
	pthread_mutex_unlock(&g_mem_lock);
}

int host_vsh_pad_take(void)
{
	int took;

	pthread_mutex_lock(&g_mem_lock);
	took = g_vpad_rc == 0 && g_vpad_fresh;
	g_vpad_fresh = 0;
	pthread_mutex_unlock(&g_mem_lock);
	return took;
}

u32 host_vsh_pad_reads(void)
{
	u32 n;

	pthread_mutex_lock(&g_mem_lock);
	n = g_vpad_reads;
	pthread_mutex_unlock(&g_mem_lock);
	return n;
}

int plat_vsh_pad_info(struct plat_pad_info *out)
{
	int rc;

	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&g_mem_lock);
	rc = g_vpad_rc;
	if (rc == 0) {
		out->max_connect = PLAT_PAD_PORTS;
		out->now_connect = 1;
		out->system_info = g_vpad_system_info;
		if (g_vpad_port < PLAT_PAD_PORTS) out->port_status[g_vpad_port] = 1;
	}
	pthread_mutex_unlock(&g_mem_lock);
	return rc;
}

int plat_vsh_pad_read(u32 port, struct plat_pad *out)
{
	int rc;

	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&g_mem_lock);
	g_vpad_reads++;
	rc = g_vpad_rc;
	if (rc == 0 && port != g_vpad_port) {
		rc = (int)0x80121107u;   /* CELL_PAD_ERROR_NO_DEVICE */
	} else if (rc == 0 && g_vpad_fresh) {
		*out = g_vpad;
		out->len = 24;
		out->fresh = 1;
		g_vpad_fresh = 0;
	}
	pthread_mutex_unlock(&g_mem_lock);
	return rc;
}

/*
 * Build 54. The fake VSH's memory, in bytes. A total of 0 is "cannot be read",
 * which is what the simulator answers until the `vshmem` command (or a test)
 * gives it a figure, as qwark-rpcs3 always does.
 */
static u32 g_vsh_free;
static u32 g_vsh_total;

void host_set_vsh_memory(u32 free_bytes, u32 total_bytes)
{
	pthread_mutex_lock(&g_mem_lock);
	g_vsh_free = free_bytes;
	g_vsh_total = total_bytes;
	pthread_mutex_unlock(&g_mem_lock);
}

int plat_vsh_memory(u32 *free_bytes, u32 *total_bytes)
{
	int rc = -1;

	pthread_mutex_lock(&g_mem_lock);
	if (g_vsh_total != 0) {
		*free_bytes = g_vsh_free;
		*total_bytes = g_vsh_total;
		rc = 0;
	}
	pthread_mutex_unlock(&g_mem_lock);
	return rc;
}

int plat_is_emulator(void)
{
	return g_is_emulator;
}

/*
 * Revision 1.24. The fake XMB's signed-in user, 1 from the start, which the
 * `user` command and the tests change; 0 is nobody signed in. The session
 * reads it once a second at the XMB, as it does the VSH's on a console.
 */
static volatile u32 g_user_id = 1;
static volatile int g_has_users = 1;

void host_set_user(u32 user) { g_user_id = user; }
void host_set_has_users(int on) { g_has_users = on ? 1 : 0; }
u32 plat_user_id(void) { return g_user_id; }
int plat_has_users(void) { return g_has_users; }

/*
 * Every read the core makes of the "process", for the test that a boot leaves it
 * alone for its first second. host_peek does not go through here, so a test can look at the
 * fake console's memory without disturbing the count.
 */
static u32 g_mem_reads;
static u32 g_fail_reads;

u32 host_mem_reads(void)
{
	return g_mem_reads;
}

void host_fail_reads(u32 count)
{
	pthread_mutex_lock(&g_mem_lock);
	g_fail_reads = count;
	pthread_mutex_unlock(&g_mem_lock);
}

int plat_mem_read(u32 pid, u32 addr, void *buf, u32 len)
{
	if (len == 0 || len > PLAT_MEM_MAX) return -1;
	if (!g_game_running || pid != g_game_pid) return -1;

	pthread_mutex_lock(&g_mem_lock);
	if (g_fail_reads) {
		g_fail_reads--;
		pthread_mutex_unlock(&g_mem_lock);
		return -1;
	}
	g_mem_reads++;
	mem_access(addr, buf, len, 0);
	pthread_mutex_unlock(&g_mem_lock);
	return 0;
}

/* Where each write landed, for the tests of the order code goes in. */
#define HOST_WRITE_LOG 64

static u32 g_write_log_addr[HOST_WRITE_LOG];
static u32 g_write_log_len[HOST_WRITE_LOG];
static u32 g_write_log_count;
static u32 g_fail_write_addr;
static u32 g_fail_write_count;

void host_fail_writes(u32 addr, u32 count)
{
	pthread_mutex_lock(&g_mem_lock);
	g_fail_write_addr = addr;
	g_fail_write_count = count;
	pthread_mutex_unlock(&g_mem_lock);
}

void host_write_log_reset(void)
{
	pthread_mutex_lock(&g_mem_lock);
	g_write_log_count = 0;
	pthread_mutex_unlock(&g_mem_lock);
}

u32 host_write_log_count(void)
{
	return g_write_log_count;
}

int host_write_log_at(u32 index, u32 *addr, u32 *len)
{
	if (index >= g_write_log_count || index >= HOST_WRITE_LOG) return -1;
	*addr = g_write_log_addr[index];
	*len = g_write_log_len[index];
	return 0;
}

int plat_mem_write(u32 pid, u32 addr, const void *buf, u32 len)
{
	if (len == 0 || len > PLAT_MEM_MAX) return -1;
	if (!g_game_running || pid != g_game_pid) return -1;

	pthread_mutex_lock(&g_mem_lock);
	if (g_fail_write_count && addr == g_fail_write_addr) {
		g_fail_write_count--;
		pthread_mutex_unlock(&g_mem_lock);
		return -1;
	}
	if (g_write_log_count < HOST_WRITE_LOG) {
		g_write_log_addr[g_write_log_count] = addr;
		g_write_log_len[g_write_log_count] = len;
	}
	g_write_log_count++;
	mem_access(addr, (void *)buf, len, 1);
	pthread_mutex_unlock(&g_mem_lock);
	return 0;
}

/* What the console has shown, for the tests (plat_host.h). */
static u32 g_notify_count;
static char g_notify_last[128];

void plat_notify(const char *msg)
{
	char ts[16];
	host_stamp(ts, sizeof(ts));
	fprintf(stderr, "[%s] notify: %s\n", ts, msg);
	fflush(stderr);

	snprintf(g_notify_last, sizeof(g_notify_last), "%s", msg);
	g_notify_count++;
}

u32 host_notify_count(void)
{
	return g_notify_count;
}

const char *host_notify_last(void)
{
	return g_notify_last;
}

/* ------------------------------------------------ game modules (1.26) */

/*
 * The simulator's stand-in for lv2's loader (plat.h, plat_prx_*; plat_host.h
 * host_write_fake_module). A "module" is a file of the simulator's own: a
 * SELF header shaped as scetool writes one, so the core reads its pages as it
 * does a real module's, and a line naming the game, the build and the ID the
 * file stands for. Its start checks the core's argument block the way
 * src/plat/ps3/game_module.c does and hands back a copy of the linked game's
 * vtable, which its unload overwrites with 0xA5 bytes: a pointer the core kept
 * past the unload faults on its next use instead of reaching a game that is
 * still linked in.
 */
#define FAKE_MODULES     4
#define FAKE_SELF_BYTES  0x100
#define FAKE_ID_BASE     0x23000000

static struct {
	int used;
	u8  game;
	u8  pages;
	u8  started;
	u32 build;
	u32 build_id;
	struct game_api copy;
} g_fake_mod[FAKE_MODULES];

static u32 g_fake_loads;

int host_write_fake_module(const char *path, u8 game, u32 build, u32 build_id, u8 pages, u32 fail)
{
	u8 buf[FAKE_SELF_BYTES + 128];
	char line[128];
	plat_file_t f;
	u32 n = 0, wrote = 0;
	int rc;

	memset(buf, 0, sizeof(buf));
	if (pages != 0) {
		u32 memsz = (u32)pages * 0x10000u - 0x100u;
		memcpy(buf, "SCE\0", 4);
		be32_put(buf + 4, 2);
		be16_put(buf + 0x0A, 1);                 /* a SELF */
		be32_put(buf + 0x34, 0x80);              /* the ELF header's offset */
		be32_put(buf + 0x3C, 0xC0);              /* the program headers' */
		memcpy(buf + 0x80, "\177ELF", 4);
		buf[0x84] = 2;
		buf[0x85] = 2;
		be16_put(buf + 0x80 + 0x38, 1);          /* one program header */
		be32_put(buf + 0xC0, 1);                 /* PT_LOAD */
		be32_put(buf + 0xC4, 5);                 /* R-X */
		be32_put(buf + 0xC0 + 44, memsz);        /* p_memsz, the low word */
		n = FAKE_SELF_BYTES;
	}
	snprintf(line, sizeof(line), "QWARKFAKE game=%u build=%u id=0x%08X fail=0x%08X\n",
	         (unsigned)game, (unsigned)build, (unsigned)build_id, (unsigned)fail);
	memcpy(buf + n, line, strlen(line));
	n += (u32)strlen(line);

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return -1;
	rc = plat_file_write(f, buf, n);
	wrote = n;
	plat_file_close(f);
	return rc == 0 && wrote == n ? 0 : -1;
}

u32 host_fake_modules_loaded(void)
{
	u32 i, n = 0;
	for (i = 0; i < FAKE_MODULES; i++) n += g_fake_mod[i].used ? 1u : 0u;
	return n;
}

u32 host_fake_module_loads(void) { return g_fake_loads; }

static void fake_vsh_take(u8 pages, int give_back)
{
	u32 d = (u32)pages * 0x10000u;

	pthread_mutex_lock(&g_mem_lock);
	if (g_vsh_total != 0) {
		if (give_back) g_vsh_free += d;
		else g_vsh_free = g_vsh_free > d ? g_vsh_free - d : 0;
	}
	pthread_mutex_unlock(&g_mem_lock);
}

int plat_prx_load(const char *path)
{
	char buf[FAKE_SELF_BYTES + 128];
	unsigned game = 0, build = 0, id = 0, fail = 0;
	plat_file_t f;
	u32 got = 0, i, pages = 0;
	const char *line = NULL;

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return (int)0x80010006u;   /* ENOENT */
	if (plat_file_read(f, buf, sizeof(buf) - 1, &got) != 0) got = 0;
	plat_file_close(f);
	buf[got] = 0;

	for (i = 0; i + 9 < got; i++) {
		if (memcmp(buf + i, "QWARKFAKE", 9) == 0) { line = buf + i; break; }
	}
	if (line == NULL ||
	    sscanf(line, "QWARKFAKE game=%u build=%u id=0x%x fail=0x%x", &game, &build, &id, &fail) != 4)
		return (int)0x8001112Eu;   /* not a module lv2 takes */
	if (fail != 0) return (int)fail;
	if (got >= 0xC0 + 48 && memcmp(buf, "SCE", 3) == 0)
		pages = (be32_get((const u8 *)buf + 0xC0 + 44) + 0xFFFFu) >> 16;

	for (i = 0; i < FAKE_MODULES; i++) {
		if (g_fake_mod[i].used) continue;
		memset(&g_fake_mod[i], 0, sizeof(g_fake_mod[i]));
		g_fake_mod[i].used = 1;
		g_fake_mod[i].game = (u8)game;
		g_fake_mod[i].build = build;
		g_fake_mod[i].build_id = id;
		g_fake_mod[i].pages = (u8)pages;
		g_fake_loads++;
		fake_vsh_take((u8)pages, 0);
		return FAKE_ID_BASE + (int)i;
	}
	return (int)0x80010004u;   /* ENOMEM */
}

static int fake_slot(int id)
{
	int i = id - FAKE_ID_BASE;
	return (i >= 0 && i < FAKE_MODULES && g_fake_mod[i].used) ? i : -1;
}

int plat_prx_start(int id, void *argp, u32 size, int *modres)
{
	struct qwark_mod_args *a = (struct qwark_mod_args *)argp;
	const struct game_api *linked;
	int i = fake_slot(id);

	*modres = 1;
	if (i < 0) return (int)0x80010005u;   /* ESRCH */
	if (a == NULL || size < sizeof(*a) || a->magic != QWARK_MOD_MAGIC || a->size != sizeof(*a)) {
		if (a != NULL && size >= sizeof(*a)) a->refusal = QWARK_MOD_REFUSE_ARGS;
		return 0;
	}
	a->module_game = g_fake_mod[i].game;
	a->module_build_id = g_fake_mod[i].build_id;
	a->game = NULL;
	if (a->build != g_fake_mod[i].build)            a->refusal = QWARK_MOD_REFUSE_BUILD;
	else if (a->build_id != g_fake_mod[i].build_id) a->refusal = QWARK_MOD_REFUSE_ID;
	else if (a->api_size != sizeof(struct core_api)) a->refusal = QWARK_MOD_REFUSE_API;
	else if (a->game_id != g_fake_mod[i].game)       a->refusal = QWARK_MOD_REFUSE_GAME;
	else a->refusal = QWARK_MOD_REFUSE_NONE;
	if (a->refusal != QWARK_MOD_REFUSE_NONE) return 0;

	linked = game_linked(g_fake_mod[i].game);
	if (linked == NULL) { a->refusal = QWARK_MOD_REFUSE_GAME; return 0; }
	g_fake_mod[i].copy = *linked;
	g_fake_mod[i].started = 1;
	a->game = &g_fake_mod[i].copy;
	*modres = 0;
	return 0;
}

int plat_prx_stop(int id, void *argp, u32 size, int *modres)
{
	int i = fake_slot(id);

	(void)argp;
	(void)size;
	*modres = 0;
	if (i < 0) return (int)0x80010005u;
	g_fake_mod[i].started = 0;
	return 0;
}

int plat_prx_unload(int id)
{
	int i = fake_slot(id);

	if (i < 0) return (int)0x80010005u;
	memset(&g_fake_mod[i].copy, 0xA5, sizeof(g_fake_mod[i].copy));
	fake_vsh_take(g_fake_mod[i].pages, 1);
	g_fake_mod[i].used = 0;
	return 0;
}

int plat_prx_contains(int id, const void *p, u32 len)
{
	int i = fake_slot(id);
	const u8 *lo, *q = (const u8 *)p;

	if (i < 0) return 0;
	lo = (const u8 *)&g_fake_mod[i].copy;
	return q >= lo && len <= sizeof(g_fake_mod[i].copy) &&
	       (size_t)(q - lo) <= sizeof(g_fake_mod[i].copy) - len;
}
