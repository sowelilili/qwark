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

int classic_cb_write(u32 addr, u8 alpha, u32 rgb)
{
	u8 word[4];

	word[0] = alpha;
	word[1] = (u8)(rgb & 0xFF);          /* B */
	word[2] = (u8)((rgb >> 8) & 0xFF);   /* G */
	word[3] = (u8)((rgb >> 16) & 0xFF);  /* R */

	return mem_write(addr, word, 4);
}

u32 classic_cb_to_rgb(u32 word)
{
	u32 b = (word >> 16) & 0xFF;
	u32 g = (word >> 8) & 0xFF;
	u32 r = word & 0xFF;

	return (r << 16) | (g << 8) | b;
}
