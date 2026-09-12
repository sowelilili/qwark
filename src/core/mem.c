#include "mem.h"
#include "../plat/plat.h"

#include <string.h>

#define CLIENT_PATCH_SLOTS 16
#define CLIENT_PATCH_WORDS 64
#define PATCH_POOL_WORDS   1024

/* ------------------------------------------------------------------ the gate */

static u32 g_pid;
static int g_ingame;

void mem_set_context(u32 pid, int ingame)
{
	g_pid = pid;
	g_ingame = ingame;
}

int mem_is_ingame(void) { return g_ingame; }
u32 mem_pid(void)       { return g_pid; }

/*
 * Every call that reaches the platform, which on a console is one PS3MAPI
 * syscall against the game process each. Only the tick thread calls these, so a
 * plain counter is exact; the ring reads it either side of a command.
 */
static u32 g_read_calls;
static u32 g_write_calls;

u32 mem_read_calls(void)  { return g_read_calls; }
u32 mem_write_calls(void) { return g_write_calls; }

int mem_read(u32 addr, void *buf, u32 len)
{
	if (!g_ingame || g_pid == 0) return ST_NOT_INGAME;
	if (len == 0 || len > PLAT_MEM_MAX) return ST_BAD_ARG;
	g_read_calls++;
	if (plat_mem_read(g_pid, addr, buf, len) != 0) return ST_IO_ERROR;
	return ST_OK;
}

int mem_write(u32 addr, const void *buf, u32 len)
{
	if (!g_ingame || g_pid == 0) return ST_NOT_INGAME;
	if (len == 0 || len > PLAT_MEM_MAX) return ST_BAD_ARG;
	g_write_calls++;
	if (plat_mem_write(g_pid, addr, buf, len) != 0) return ST_IO_ERROR;
	return ST_OK;
}

int mem_read_u32(u32 addr, u32 *out)
{
	u8 b[4];
	int rc = mem_read(addr, b, 4);
	if (rc != ST_OK) return rc;
	*out = be32_get(b);
	return ST_OK;
}

int mem_write_u32(u32 addr, u32 value)
{
	u8 b[4];
	be32_put(b, value);
	return mem_write(addr, b, 4);
}

int mem_write_u8(u32 addr, u8 value)
{
	return mem_write(addr, &value, 1);
}

int mem_read_u8(u32 addr, u8 *out)
{
	return mem_read(addr, out, 1);
}

int mem_write_fill(u32 addr, u8 value, u32 len)
{
	u8 fill[256];
	u32 done = 0;

	memset(fill, value, sizeof(fill));

	while (done < len) {
		u32 chunk = len - done;
		int rc;
		if (chunk > sizeof(fill)) chunk = sizeof(fill);
		rc = mem_write(addr + done, fill, chunk);
		if (rc != ST_OK) return rc;
		done += chunk;
	}

	return ST_OK;
}

int mem_write_zeros(u32 addr, u32 len)
{
	return mem_write_fill(addr, 0, len);
}

/* --------------------------------------------------------------- watch table */

static struct watch_entry g_watches[QWARK_MAX_WATCHES];

int watch_add(u32 addr, u8 size, u8 *id_out)
{
	int i;

	if (size != 1 && size != 2 && size != 4 && size != 8) return ST_BAD_ARG;

	/* Keyed by address and size, so ids stay stable across a same-game reboot. */
	for (i = 0; i < QWARK_MAX_WATCHES; i++) {
		if (g_watches[i].used && g_watches[i].addr == addr && g_watches[i].size == size) {
			if (id_out) *id_out = (u8)i;
			return ST_OK;
		}
	}

	for (i = 0; i < QWARK_MAX_WATCHES; i++) {
		if (g_watches[i].used) continue;
		g_watches[i].addr  = addr;
		g_watches[i].size  = size;
		g_watches[i].used  = 1;
		g_watches[i].valid = 0;
		g_watches[i].value = 0;
		if (id_out) *id_out = (u8)i;
		return ST_OK;
	}

	return ST_FULL;
}

int watch_remove(u8 id)
{
	if (id >= QWARK_MAX_WATCHES || !g_watches[id].used) return ST_NOT_FOUND;
	memset(&g_watches[id], 0, sizeof(g_watches[id]));
	return ST_OK;
}

void watch_clear(void)
{
	memset(g_watches, 0, sizeof(g_watches));
}

void watch_invalidate(void)
{
	int i;
	for (i = 0; i < QWARK_MAX_WATCHES; i++) g_watches[i].valid = 0;
}

const struct watch_entry *watch_slot(u8 id)
{
	if (id >= QWARK_MAX_WATCHES || !g_watches[id].used) return NULL;
	return &g_watches[id];
}

void watch_tick(void)
{
	int i;

	if (!g_ingame) {
		watch_invalidate();
		return;
	}

	for (i = 0; i < QWARK_MAX_WATCHES; i++) {
		u8 buf[8];
		if (!g_watches[i].used) continue;

		if (mem_read(g_watches[i].addr, buf, g_watches[i].size) != ST_OK) {
			g_watches[i].valid = 0;
			continue;
		}

		g_watches[i].value = be_get_sized(buf, g_watches[i].size);
		g_watches[i].valid = 1;
	}
}

/* -------------------------------------------------------------- freeze table */

static struct freeze_entry g_freezes[QWARK_MAX_FREEZES];

int freeze_add(u32 addr, u8 size, u64 value, u8 *id_out)
{
	int i;

	if (size == 0 || size > 8) return ST_BAD_ARG;

	for (i = 0; i < QWARK_MAX_FREEZES; i++) {
		if (g_freezes[i].used && g_freezes[i].addr == addr && g_freezes[i].size == size) {
			g_freezes[i].value = value;
			if (id_out) *id_out = (u8)i;
			return ST_OK;
		}
	}

	for (i = 0; i < QWARK_MAX_FREEZES; i++) {
		if (g_freezes[i].used) continue;
		g_freezes[i].addr  = addr;
		g_freezes[i].size  = size;
		g_freezes[i].value = value;
		g_freezes[i].used  = 1;
		if (id_out) *id_out = (u8)i;
		return ST_OK;
	}

	return ST_FULL;
}

int freeze_remove(u8 id)
{
	if (id >= QWARK_MAX_FREEZES || !g_freezes[id].used) return ST_NOT_FOUND;
	memset(&g_freezes[id], 0, sizeof(g_freezes[id]));
	return ST_OK;
}

int freeze_find(u32 addr, u8 size)
{
	int i;
	for (i = 0; i < QWARK_MAX_FREEZES; i++) {
		if (g_freezes[i].used && g_freezes[i].addr == addr && g_freezes[i].size == size)
			return i;
	}
	return -1;
}

void freeze_clear(void)
{
	memset(g_freezes, 0, sizeof(g_freezes));
}

const struct freeze_entry *freeze_slot(u8 id)
{
	if (id >= QWARK_MAX_FREEZES || !g_freezes[id].used) return NULL;
	return &g_freezes[id];
}

u64 freeze_mask(void)
{
	u64 mask = 0;
	int i;
	for (i = 0; i < QWARK_MAX_FREEZES; i++) {
		if (g_freezes[i].used) mask |= (u64)1 << i;
	}
	return mask;
}

void freeze_tick(void)
{
	int i;

	if (!g_ingame) return;

	for (i = 0; i < QWARK_MAX_FREEZES; i++) {
		u8 buf[8];
		if (!g_freezes[i].used) continue;
		be_put_sized(buf, g_freezes[i].size, g_freezes[i].value);
		mem_write(g_freezes[i].addr, buf, g_freezes[i].size);
	}
}

/* --------------------------------------------------------------- patch table */

static const struct patch_def *g_patches[QWARK_MAX_PATCHES];
static u32 g_patch_pool[PATCH_POOL_WORDS];
static u16 g_patch_pool_used;

u32 *patch_pool_alloc(u16 words)
{
	u32 *p;
	if (words == 0) return NULL;
	if ((u32)g_patch_pool_used + (u32)words > PATCH_POOL_WORDS) return NULL;
	p = &g_patch_pool[g_patch_pool_used];
	g_patch_pool_used = (u16)(g_patch_pool_used + words);
	return p;
}

void patch_pool_reset(void)
{
	g_patch_pool_used = 0;
}

/* ---------------------------------------------------------- patch write order */

/*
 * Nothing stops the game while a patch goes in, because nothing can: the RSX
 * pause these writes used to sit inside stops the GPU's command stream and no
 * more, and every PPU thread of the game keeps running through it. The order of
 * the writes is what keeps running code off a half-written patch.
 *
 * A def's words are cut into runs, words listed one after another at
 * consecutive addresses, and each run goes out as a single write. A run that
 * branches into another run of the same def goes in after that run, so no branch
 * lands on code that is not there yet. Revert walks the same order backwards,
 * taking the branches into a trampoline out before the trampoline itself.
 *
 * Only b and bc (primary opcodes 18 and 16) name a target; bclr and bcctr jump
 * through a register. Two runs that branch into each other have no safe order
 * and go in as listed.
 */
#define PATCH_ORDER_WORDS 2048          /* the largest def, a mod holding the whole word pool */
#define PATCH_RUN_WORDS   256           /* how much of a run goes out in one write */
#define NO_RUN            0xFFFFu

static u16 g_run_first[PATCH_ORDER_WORDS];  /* the index of each run's first word */
static u16 g_run_order[PATCH_ORDER_WORDS];  /* runs, in the order they are written */
static u16 g_word_dep[PATCH_ORDER_WORDS];   /* the other run a word branches into */
static u8  g_run_done[PATCH_ORDER_WORDS];
static u8  g_run_buf[PATCH_RUN_WORDS * 4];

/* Where a b or bc at addr lands. 0 for any other word. */
static int branch_target(u32 addr, u32 word, u32 *target)
{
	u32 op = word >> 26;
	u32 disp;

	if (op == 18) {
		disp = word & 0x03FFFFFCu;
		if (disp & 0x02000000u) disp |= 0xFC000000u;
	} else if (op == 16) {
		disp = word & 0x0000FFFCu;
		if (disp & 0x00008000u) disp |= 0xFFFF0000u;
	} else {
		return 0;
	}

	*target = (word & 2u) ? disp : addr + disp;   /* AA: an absolute address */
	return 1;
}

static u16 run_end(const struct patch_def *def, u16 nruns, u16 r)
{
	return (u16)(r + 1 < nruns ? g_run_first[r + 1] : def->count);
}

/* Cuts def into runs and fills g_run_order. Returns the number of runs. */
static u16 patch_order(const struct patch_def *def)
{
	u16 nruns = 0;
	u16 w, r, k;

	if (def->count > PATCH_ORDER_WORDS) return 0;

	for (w = 0; w < def->count; w++) {
		if (w == 0 || def->words[w].addr != def->words[w - 1].addr + 4u)
			g_run_first[nruns++] = w;
	}

	for (r = 0; r < nruns; r++) {
		u16 end = run_end(def, nruns, r);

		g_run_done[r] = 0;
		for (w = g_run_first[r]; w < end; w++) {
			u32 target;

			g_word_dep[w] = NO_RUN;
			if (!branch_target(def->words[w].addr, def->words[w].value, &target)) continue;

			for (k = 0; k < nruns; k++) {
				u32 base = def->words[g_run_first[k]].addr;
				u32 len = (u32)(run_end(def, nruns, k) - g_run_first[k]) * 4u;
				if (k != r && target - base < len) { g_word_dep[w] = k; break; }
			}
		}
	}

	for (k = 0; k < nruns; k++) {
		u16 pick = NO_RUN;

		/* The first run listed whose branch targets are all in already. */
		for (r = 0; r < nruns && pick == NO_RUN; r++) {
			u16 end = run_end(def, nruns, r);
			if (g_run_done[r]) continue;
			for (w = g_run_first[r]; w < end; w++) {
				if (g_word_dep[w] != NO_RUN && !g_run_done[g_word_dep[w]]) break;
			}
			if (w == end) pick = r;
		}

		/* None: a cycle, so the first run still waiting goes in as listed. */
		for (r = 0; r < nruns && pick == NO_RUN; r++) {
			if (!g_run_done[r]) pick = r;
		}

		g_run_order[k] = pick;
		g_run_done[pick] = 1;
	}

	return nruns;
}

/*
 * One run's patched or original words. A run too long for one write goes out
 * back to front: code runs forward, so what a word falls through to is already
 * in by the time the word itself is.
 */
static void write_run(const struct patch_def *def, u16 nruns, u16 r, int revert)
{
	u16 first = g_run_first[r];
	u16 stop = run_end(def, nruns, r);

	while (stop > first) {
		u16 start = (u16)(stop - first > PATCH_RUN_WORDS ? stop - PATCH_RUN_WORDS : first);
		u16 w;

		for (w = start; w < stop; w++)
			be32_put(&g_run_buf[(w - start) * 4], revert ? def->originals[w] : def->words[w].value);

		mem_write(def->words[start].addr, g_run_buf, (u32)(stop - start) * 4u);
		stop = start;
	}
}

int patch_is_applied(const struct patch_def *def)
{
	int i;
	for (i = 0; i < QWARK_MAX_PATCHES; i++) {
		if (g_patches[i] == def) return 1;
	}
	return 0;
}

int patch_apply(const struct patch_def *def)
{
	int slot = -1;
	int i;
	u16 w, k, nruns;

	if (def == NULL || def->words == NULL || def->count == 0 || def->originals == NULL)
		return ST_BAD_ARG;
	if (def->count > PATCH_ORDER_WORDS) return ST_FULL;

	/* A second apply is a no-op. It must never re-capture the originals. */
	if (patch_is_applied(def)) return ST_OK;

	/*
	 * Under RPCS3 the PPU code is already recompiled, so writing an instruction
	 * word changes memory and nothing else. Refusing here is what makes every
	 * caller refuse: FEATURE_SET on a WRITES_CODE toggle, MOD_LOAD of a mod with
	 * patch words, PATCH_APPLY from a client and the games' own helpers.
	 */
	if (!plat_can_patch_code()) return ST_UNSUPPORTED;

	if (!g_ingame || g_pid == 0) return ST_NOT_INGAME;

	for (i = 0; i < QWARK_MAX_PATCHES; i++) {
		if (g_patches[i] == NULL) { slot = i; break; }
	}
	if (slot < 0) return ST_FULL;

	/* Read every original before writing anything, so a failure changes nothing. */
	for (w = 0; w < def->count; w++) {
		u32 original = 0;
		if (mem_read_u32(def->words[w].addr, &original) != ST_OK) return ST_IO_ERROR;
		def->originals[w] = original;
	}

	nruns = patch_order(def);
	for (k = 0; k < nruns; k++) write_run(def, nruns, g_run_order[k], 0);

	g_patches[slot] = def;
	return ST_OK;
}

int patch_revert(const struct patch_def *def)
{
	int slot = -1;
	int i;
	u16 k, nruns;

	if (def == NULL) return ST_BAD_ARG;

	for (i = 0; i < QWARK_MAX_PATCHES; i++) {
		if (g_patches[i] == def) { slot = i; break; }
	}
	if (slot < 0) return ST_NOT_FOUND;

	if (g_ingame && g_pid != 0) {
		nruns = patch_order(def);
		for (k = nruns; k > 0; k--) write_run(def, nruns, g_run_order[k - 1], 1);
	}

	g_patches[slot] = NULL;
	return ST_OK;
}

void patch_forget_all(void)
{
	memset(g_patches, 0, sizeof(g_patches));
}

u32 patch_count(void)
{
	u32 n = 0;
	int i;
	for (i = 0; i < QWARK_MAX_PATCHES; i++) if (g_patches[i]) n++;
	return n;
}

const struct patch_def *patch_at(u32 index)
{
	u32 n = 0;
	int i;
	for (i = 0; i < QWARK_MAX_PATCHES; i++) {
		if (!g_patches[i]) continue;
		if (n == index) return g_patches[i];
		n++;
	}
	return NULL;
}

/* ------------------------------------------------------------ client patches */

struct client_patch {
	struct patch_def  def;
	struct patch_word words[CLIENT_PATCH_WORDS];
	u32               originals[CLIENT_PATCH_WORDS];
	char              name[32];
	u8                used;
};

static struct client_patch g_client[CLIENT_PATCH_SLOTS];

static struct client_patch *client_find(u32 first_addr)
{
	int i;
	for (i = 0; i < CLIENT_PATCH_SLOTS; i++) {
		if (g_client[i].used && g_client[i].words[0].addr == first_addr)
			return &g_client[i];
	}
	return NULL;
}

static void client_name(struct client_patch *c, u32 first_addr)
{
	static const char hex[] = "0123456789abcdef";
	int i;
	memcpy(c->name, "clt 0x", 6);
	for (i = 0; i < 8; i++) {
		c->name[6 + i] = hex[(first_addr >> (28 - 4 * i)) & 0xF];
	}
	c->name[14] = 0;
}

int client_patch_apply(const struct patch_word *words, u16 count)
{
	struct client_patch *c;
	int rc;
	int i;

	if (words == NULL || count == 0 || count > CLIENT_PATCH_WORDS) return ST_BAD_ARG;

	c = client_find(words[0].addr);
	if (c != NULL) {
		/* Applying the same client patch twice is a no-op. */
		if (patch_is_applied(&c->def)) return ST_OK;
	} else {
		for (i = 0; i < CLIENT_PATCH_SLOTS; i++) {
			if (!g_client[i].used) { c = &g_client[i]; break; }
		}
		if (c == NULL) return ST_FULL;

		memset(c, 0, sizeof(*c));
		memcpy(c->words, words, sizeof(struct patch_word) * count);
		client_name(c, words[0].addr);
		c->def.name      = c->name;
		c->def.kind      = PATCH_KIND_CLIENT;
		c->def.words     = c->words;
		c->def.count     = count;
		c->def.originals = c->originals;
		c->used          = 1;
	}

	rc = patch_apply(&c->def);
	if (rc != ST_OK && !patch_is_applied(&c->def)) {
		/* Nothing was written, so give the slot straight back. */
		c->used = 0;
	}
	return rc;
}

int client_patch_revert(u32 first_addr)
{
	struct client_patch *c = client_find(first_addr);
	int rc;

	if (c == NULL) return ST_NOT_FOUND;

	rc = patch_revert(&c->def);
	if (rc == ST_OK) c->used = 0;
	return rc;
}

void client_patch_clear(void)
{
	int i;
	for (i = 0; i < CLIENT_PATCH_SLOTS; i++) {
		if (!g_client[i].used) continue;
		patch_revert(&g_client[i].def);
		g_client[i].used = 0;
	}
}

u32 client_patch_count(void)
{
	u32 n = 0;
	int i;
	for (i = 0; i < CLIENT_PATCH_SLOTS; i++) if (g_client[i].used) n++;
	return n;
}

const struct patch_def *client_patch_at(u32 index)
{
	u32 n = 0;
	int i;
	for (i = 0; i < CLIENT_PATCH_SLOTS; i++) {
		if (!g_client[i].used) continue;
		if (n == index) return &g_client[i].def;
		n++;
	}
	return NULL;
}

void client_patch_drop_all(void)
{
	int i;
	for (i = 0; i < CLIENT_PATCH_SLOTS; i++) g_client[i].used = 0;
}

void client_patch_gc(void)
{
	int i;
	for (i = 0; i < CLIENT_PATCH_SLOTS; i++) {
		if (!g_client[i].used) continue;
		if (patch_is_applied(&g_client[i].def)) continue;
		g_client[i].used = 0;
	}
}

void mem_clear_client(void)
{
	client_patch_clear();
	watch_clear();
	freeze_clear();
}
