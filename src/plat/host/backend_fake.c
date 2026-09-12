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
static const struct game_api *g_boot_game;

/* The fake console is a console: it patches code and it is not an emulator. */
static int g_can_patch_code = 1;
static int g_is_emulator;

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

	return 0;
}

void host_boot_as(const char *title, const char *which)
{
	const struct game_api *cands[GAME_MAX_CANDIDATES];
	const struct game_api *game = NULL;
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
	n = game_candidates_for_title(g_game_title, cands, GAME_MAX_CANDIDATES);
	wanted = host_wanted_game_id(which);

	for (i = 0; i < n; i++) {
		if (wanted == 0 || cands[i]->game_id == wanted) { game = cands[i]; break; }
	}
	if (game == NULL && n > 0) game = cands[0];

	g_boot_game = game;

	if (game != NULL && game->fp_bytes != NULL && game->fp_len > 0) {
		mem_access(game->fp_addr, (void *)game->fp_bytes, game->fp_len, 1);
	}

	pthread_mutex_unlock(&g_mem_lock);

	plat_log("host: booted %s as pid %d%s", g_game_title, (int)g_game_pid,
	         game != NULL ? "" : " (no registered game, nothing seeded)");
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

void host_set_can_patch_code(int on)
{
	g_can_patch_code = on ? 1 : 0;
}

void host_set_emulator(int on)
{
	g_is_emulator = on ? 1 : 0;
}

int plat_game_running(void)
{
	return g_game_running;
}

u32 plat_game_pid(void)
{
	return g_game_pid;
}

int plat_game_title(char out[16])
{
	snprintf(out, 16, "%s", g_game_title);
	return out[0] != 0;
}

int plat_can_patch_code(void)
{
	return g_can_patch_code;
}

int plat_is_emulator(void)
{
	return g_is_emulator;
}

/*
 * A tenth of a second. There is no process here to damage, and the smoke test
 * boots six games over the wire: a console's window would be most of a minute
 * spent proving nothing this platform can get wrong.
 */
u32 plat_boot_settle_ticks(void)
{
	return 12u;
}

/*
 * Every read the core makes of the "process", for the test that the boot window
 * is respected. host_peek does not go through here, so a test can look at the
 * fake console's memory without disturbing the count.
 */
static u32 g_mem_reads;

u32 host_mem_reads(void)
{
	return g_mem_reads;
}

int plat_mem_read(u32 pid, u32 addr, void *buf, u32 len)
{
	if (len == 0 || len > PLAT_MEM_MAX) return -1;
	if (!g_game_running || pid != g_game_pid) return -1;

	pthread_mutex_lock(&g_mem_lock);
	g_mem_reads++;
	mem_access(addr, buf, len, 0);
	pthread_mutex_unlock(&g_mem_lock);
	return 0;
}

int plat_mem_write(u32 pid, u32 addr, const void *buf, u32 len)
{
	if (len == 0 || len > PLAT_MEM_MAX) return -1;
	if (!g_game_running || pid != g_game_pid) return -1;

	pthread_mutex_lock(&g_mem_lock);
	mem_access(addr, (void *)buf, len, 1);
	pthread_mutex_unlock(&g_mem_lock);
	return 0;
}

void plat_rsx_pause(int pause)
{
	plat_log("host: rsx %s", pause ? "pause" : "continue");
}

void plat_notify(const char *msg)
{
	char ts[16];
	host_stamp(ts, sizeof(ts));
	fprintf(stderr, "[%s] notify: %s\n", ts, msg);
	fflush(stderr);
}
