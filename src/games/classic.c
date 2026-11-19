#include "core_import.h"   /* first: src/games/module.h */
#include "classic.h"
#include "../core/mem.h"
#include "../core/switches.h"

void classic_decode_analogs(const u8 *block, f32 out[4])
{
	int i;

	if (block == NULL) {
		for (i = 0; i < 4; i++) out[i] = 0.0f;
		return;
	}

	for (i = 0; i < 4; i++) out[i] = bef32_get(block + i * 4);
}

int classic_planet_request(u32 addr, u8 planet)
{
	u8 request[8];

	be32_put(request, 1);
	be32_put(request + 4, planet);

	return mem_write(addr, request, sizeof(request));
}

int classic_die_set_z(u32 coords_addr)
{
	return mem_write_u32(coords_addr + 8, CLASSIC_DEATH_Z_BITS);
}

int classic_ghost(u32 timer_addr, int on)
{
	int id;

	if (on) return freeze_add(timer_addr, 4, CLASSIC_GHOST_VALUE, NULL);

	id = freeze_find(timer_addr, 4);
	if (id < 0) return ST_OK;          /* already off */
	return freeze_remove((u8)id);
}

/* classic.h: a maximum a game could really be holding, as its own type. */
static int health_max_ok(const struct classic_health *h, u32 bits)
{
	/* At least one hit point, and finite: 1.0f up to the infinities. */
	if (h->is_float) return bits >= 0x3F800000u && bits < 0x7F800000u;
	return (s32)bits > 0;
}

/*
 * Build 53. The entry on the hit points, and whether it is the toggle's own
 * (classic.h): one it put there whose value is still the one it gave it. *id is
 * the entry's slot, or -1 when there is none.
 */
static const struct freeze_entry *health_entry(const struct classic_health *h, int *id, int *ours)
{
	const struct freeze_entry *f;

	*id = freeze_find(h->health, 4);
	f = (*id >= 0) ? freeze_slot((u8)*id) : NULL;
	*ours = f != NULL && h->owned && f->value == (u64)h->held;
	return f;
}

/*
 * Puts the entry where the maximum says, or takes it out while there is none.
 * A client that removed the entry by hand gets it back here: the toggle is
 * still on, and the entry is all the toggle is.
 *
 * Build 53: an entry that is not the toggle's own is the user's freeze, and is
 * left alone, value and all, until it is gone.
 */
static int health_refresh(struct classic_health *h)
{
	const struct freeze_entry *f;
	u32 max = 0;
	int id, ours, rc;

	rc = mem_read_u32(h->max, &max);
	if (rc != ST_OK) return rc;

	f = health_entry(h, &id, &ours);
	if (f != NULL && !ours) {
		h->owned = 0;
		return ST_OK;
	}

	if (!health_max_ok(h, max)) {
		if (f != NULL) freeze_remove((u8)id);
		h->owned = 0;
		return ST_OK;
	}

	if (f != NULL && f->value == (u64)max) return ST_OK;
	rc = freeze_add(h->health, 4, max, NULL);
	if (rc == ST_OK) {
		h->owned = 1;
		h->held = max;
	}
	return rc;
}

int classic_health_toggle(struct classic_health *h, int on)
{
	const struct freeze_entry *f;
	int id, ours, rc;

	if (on) {
		h->on = 1;
		h->wait = CLASSIC_HEALTH_EVERY - 1;
		rc = health_refresh(h);
		/* A full freeze table is the client's to hear about; a read the tick retries. */
		if (rc == ST_FULL) {
			h->on = 0;
			return rc;
		}
		return ST_OK;
	}

	h->on = 0;
	f = health_entry(h, &id, &ours);
	h->owned = 0;
	/* Already off, or the entry is the user's own freeze, which stays. */
	if (f == NULL || !ours) return ST_OK;
	return freeze_remove((u8)id);
}

void classic_health_tick(struct classic_health *h)
{
	if (!h->on) return;
	if (h->wait != 0) {
		h->wait--;
		return;
	}
	h->wait = CLASSIC_HEALTH_EVERY - 1;
	health_refresh(h);
}

void classic_health_forget(struct classic_health *h)
{
	h->on = 0;
	h->wait = 0;
	h->owned = 0;
	h->held = 0;
}

int classic_patch_toggle(const struct patch_def *def, int on)
{
	int rc;

	if (on) return patch_apply(def);

	rc = patch_revert(def);
	return rc == ST_NOT_FOUND ? ST_OK : rc;
}

/*
 * classic.h: the running game's unlock snapshot, and whose it is. Initialised
 * so that they are this file's own .bss and not common symbols, which the
 * footprint tool could not tell from the SDK's.
 */
#if QWARK_GAME_MODULE
typedef char classic_state_fits[sizeof(struct classic_state) <= QWARK_STATE_CLASSIC_MAX ? 1 : -1];
#else
struct classic_state classic_state;
#endif

#ifdef QWARK_EMULATOR   /* classic.h: the PS3's are the patch calls themselves */

int classic_code_toggle(const struct patch_def *def, u32 flag, int on)
{
	/*
	 * Under RPCS3 with the switches in, the instruction the patch would write
	 * is a branch to a trampoline that asks this byte which form to run. Off
	 * is a write like on: nothing was patched, so there is nothing to revert.
	 */
	if (switches_present()) return mem_write_u8(flag, on ? 1 : 0);
	return classic_patch_toggle(def, on);
}

int classic_code_force(const struct patch_def *def, u32 flag)
{
	if (switches_present()) return mem_write_u8(flag, 1);
	return patch_apply(def);
}

int classic_code_read(u32 flag, int *on)
{
	u8 b = 0;
	int rc;

	if (!switches_present()) return ST_NOT_FOUND;

	rc = mem_read_u8(flag, &b);
	if (rc != ST_OK) return rc;

	*on = (b != 0);
	return ST_OK;
}

#endif /* QWARK_EMULATOR */

u32 classic_cb_to_rgb(u32 word)
{
	u32 b = (word >> 16) & 0xFF;
	u32 g = (word >> 8) & 0xFF;
	u32 r = word & 0xFF;

	return (r << 16) | (g << 8) | b;
}

/* Revision 1.24: front, back, tint and the word after the tint (classic.h). */
static const u8  classic_cb_off[4]   = { 0x00, 0x04, 0x10, 0x14 };
static const u8  classic_cb_alpha[3] = { CLASSIC_CB_ALPHA_FRONT, CLASSIC_CB_ALPHA_BACK, CLASSIC_CB_ALPHA_FRONT };
static const u32 classic_cb_stock[4] = { 0x40FF8020u, 0x00CF6000u, 0x20FFFF00u, 0x00008000u };

/* UYA's twin word takes the tint's alpha, the front one. */
int classic_cb_set(u32 front, u8 which, u32 value, int twin)
{
	u8 at = which;
	int rc;

	do {
		rc = mem_write_u32(front + classic_cb_off[at], (value & FEATURE_COLOR_AUTO) != 0
		                   ? classic_cb_stock[at]
		                   : ((u32)classic_cb_alpha[which] << 24) | classic_cb_to_rgb(value));
		at = 3;
	} while (rc == ST_OK && twin-- > 0);
	return rc;
}

void classic_cb_readouts(const u8 *words, u32 *ro)
{
	u32 w;
	u8 i;

	for (i = 0; i < 3; i++) {
		w = be32_get(words + classic_cb_off[i]);
		ro[i] = classic_cb_to_rgb(w) | (w == classic_cb_stock[i] ? FEATURE_COLOR_AUTO : 0);
	}
}

/* Revision 1.34: one of `n` words is `w`. */
static int classic_one_of(u32 w, const u32 *set, u8 n)
{
	u8 i;

	for (i = 0; i < n; i++) {
		if (set[i] == w) return 1;
	}
	return 0;
}

s32 classic_save_level(const u8 *head, u32 size, const u32 *globals, u8 nglobals, const u32 *planets, u8 nplanets)
{
	u32 global = be32_get(head);
	s32 level;

	if (size < CLASSIC_SAVE_HEAD) return -1;
	if (!classic_one_of(global, globals, nglobals) || !classic_one_of(be32_get(head + 4), planets, nplanets)) return -1;
	/* The global block's own length, its checksum word, then field 0's tag and size. */
	if (be32_get(head + 8) != global - 8 || be32_get(head + 0x10) != 0 || be32_get(head + 0x14) != 4) return -1;
	level = (s32)be32_get(head + 0x18);
	return level >= 0 ? level : -1;
}
