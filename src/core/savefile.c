#include "savefile.h"
#include "mem.h"
#include "session.h"
#include "../games/sfhelper_bins.h"
#include "../plat/plat.h"

/*
 * Whether this process has the helper in it. It is a plain flag rather than a
 * read-back, because the read-back is api_mod and that only goes to 1 once the
 * game has reached the hook: between the write and the next frame the helper is
 * installed and not yet running, and a second install in that window would write
 * the caves out from under code that is about to execute them.
 */
static int g_installed;

void savefile_forget(void)
{
	g_installed = 0;
}

static const struct sf_desc *desc_now(void)
{
	const struct game_api *g = session_game();

	if (g == NULL) return NULL;
	return sf_desc_for_game(g->game_id);
}

/*
 * The gate every entry point starts with. UNSUPPORTED for a platform that
 * refuses code patches, because the helper *is* a code patch; NOT_INGAME
 * outside INGAME, because there is no process to write into.
 */
static int gate(const struct sf_desc **out)
{
	const struct sf_desc *d;

	*out = NULL;

	if (!plat_can_patch_code()) return ST_UNSUPPORTED;
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;

	*out = d;
	return ST_OK;
}

int savefile_install(void)
{
	const struct sf_desc *d;
	int rc = gate(&d);
	u8 i;

	if (rc != ST_OK) return rc;
	if (g_installed) return ST_OK;

	/*
	 * A request byte left over from whatever used to live at these addresses
	 * would fire the moment the hook goes in, so the three go to zero while
	 * nothing is reading them yet.
	 */
	mem_write_u8(d->api_mod, 0);
	mem_write_u8(d->api_load, 0);
	mem_write_u8(d->api_setaside, 0);

	/*
	 * Caves first and hook words second, the order mods.c uses: the words branch
	 * into the caves, so the target exists before anything can jump to it. The
	 * caves go in with the RSX paused, as every other code write does.
	 */
	plat_rsx_pause(1);
	for (i = 0; i < d->ncaves; i++) {
		rc = mem_write(d->caves[i].addr, d->caves[i].bytes, d->caves[i].len);
		if (rc != ST_OK) break;
	}
	plat_rsx_pause(0);

	if (rc != ST_OK) return rc;

	for (i = 0; i < d->nhooks; i++) {
		rc = mem_write_u32(d->hooks[i].addr, d->hooks[i].value);
		if (rc != ST_OK) return rc;
	}

	/*
	 * Nothing reverts it. The hook is a branch the game may be executing at any
	 * moment, and taking it back out from under running code is the crash
	 * mods.c documents; with no request outstanding the helper is a byte write
	 * and three comparisons a frame.
	 */
	g_installed = 1;
	plat_log("savefile: helper installed for game %d", (int)d->game_id);
	return ST_OK;
}

int savefile_info(u8 *supported, u8 *installed, u8 *running, u8 *pending, u32 *size)
{
	const struct sf_desc *d;
	u8 byte = 0;

	*supported = 0;
	*installed = 0;
	*running   = 0;
	*pending   = 0;
	*size      = 0;

	if (!plat_can_patch_code()) return ST_UNSUPPORTED;
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	/*
	 * A game qwark has no helper for is not an error: the client asks every
	 * game and hides its save-file panel for the ones that answer 0 here.
	 */
	if (d == NULL) return ST_OK;

	*supported = 1;
	*size = d->aside_size;

	/* Asking about the helper is a first use, so this is where it goes in. */
	savefile_install();
	*installed = (u8)(g_installed != 0);

	if (mem_read_u8(d->api_mod, &byte) == ST_OK && byte == 1) *running = 1;

	if (mem_read_u8(d->api_setaside, &byte) == ST_OK && byte != 0)
		*pending |= SAVEFILE_PENDING_SET_ASIDE;
	if (mem_read_u8(d->api_load, &byte) == ST_OK && byte != 0)
		*pending |= SAVEFILE_PENDING_LOAD;

	return ST_OK;
}

int savefile_read(u32 offset, u32 len, u8 *out, u32 *outlen)
{
	const struct sf_desc *d;
	int rc = gate(&d);

	*outlen = 0;
	if (rc != ST_OK) return rc;
	if (len == 0 || len > PLAT_MEM_MAX) return ST_BAD_ARG;
	if (offset >= d->aside_size) return ST_BAD_ARG;

	/* A read that runs off the end is trimmed, so a client can ask for a round
	 * chunk at every offset and let the last one come back short. */
	if (len > d->aside_size - offset) len = d->aside_size - offset;

	rc = savefile_install();
	if (rc != ST_OK) return rc;

	rc = mem_read(d->aside_addr + offset, out, len);
	if (rc != ST_OK) return rc;

	*outlen = len;
	return ST_OK;
}

int savefile_write(u32 offset, const u8 *data, u32 len)
{
	const struct sf_desc *d;
	int rc = gate(&d);

	if (rc != ST_OK) return rc;
	if (len == 0 || len > PLAT_MEM_MAX) return ST_BAD_ARG;
	if (offset >= d->aside_size) return ST_BAD_ARG;

	/* Unlike a read, a write past the end is refused rather than trimmed: a
	 * client sending more than the buffer holds has the wrong file. */
	if (len > d->aside_size - offset) return ST_BAD_ARG;

	rc = savefile_install();
	if (rc != ST_OK) return rc;

	return mem_write(d->aside_addr + offset, data, len);
}

static int request(int load)
{
	const struct sf_desc *d;
	int rc = gate(&d);

	if (rc != ST_OK) return rc;

	rc = savefile_install();
	if (rc != ST_OK) return rc;

	return mem_write_u8(load ? d->api_load : d->api_setaside, 1);
}

int savefile_set_aside(void) { return request(0); }
int savefile_load_aside(void) { return request(1); }
