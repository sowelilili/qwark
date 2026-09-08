#include "classic.h"
#include "../core/mem.h"

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

int classic_patch_toggle(const struct patch_def *def, int on)
{
	int rc;

	if (on) return patch_apply(def);

	rc = patch_revert(def);
	return rc == ST_NOT_FOUND ? ST_OK : rc;
}

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
