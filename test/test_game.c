/*
 * A second registered title, compiled into the host test build only.
 *
 * Its only job is to make the "different title came back" branch of the session
 * state machine reachable: without it the registry has exactly one game and the
 * branch could never be exercised.
 */
#include "../src/games/game.h"
#include "../src/core/mem.h"

#include <string.h>

#define TG_FP_ADDR 0x00200000u
#define TG_PAD     0x00300000u
#define TG_COORDS  0x00300100u

static const u8 tg_fp[4] = { 0x54, 0x45, 0x53, 0x54 };   /* "TEST" */

static const struct game_hot_block tg_hot[] = {
	{ TG_PAD, 0x104, 1, 0 }
};

static void tg_hot_decode(const u8 * const *blocks, struct game_hot *out)
{
	memset(out, 0, sizeof(*out));
	if (blocks[0] == NULL) return;

	out->pad_mask = be32_get(blocks[0]);
	out->pos[0] = bef32_get(blocks[0] + (TG_COORDS - TG_PAD));
}

static const char * const tg_groups[] = { "Test" };
static const char * const tg_readouts[] = { "Value" };

static const struct feature_desc tg_features[] = {
	/* id, kind, group, aux, flags, readout, min, max, label */
	{ 0, FEATURE_TOGGLE, 0, 0, 0, FEATURE_NO_READOUT, 0, 0, "Test toggle" }
};

static const struct game_describe tg_table = {
	tg_groups, 1,
	tg_readouts, 1,
	tg_features, 1,
	0             /* auto_default */
};

static const struct game_describe *tg_describe(void) { return &tg_table; }

static const struct patch_word tg_words[] = {
	{ 0x00400000u, 0xDEADBEEFu }
};

static struct patch_def tg_patch = { "Test toggle", PATCH_KIND_FEATURE, tg_words, 1, NULL };

static void tg_init(void)
{
	static int done = 0;
	if (done) return;
	done = 1;
	tg_patch.originals = patch_pool_alloc(1);
}

static int tg_set_toggle(u8 id, int on)
{
	int rc;

	if (id != 0) return ST_NOT_FOUND;
	if (on) return patch_apply(&tg_patch);

	rc = patch_revert(&tg_patch);
	return rc == ST_NOT_FOUND ? ST_OK : rc;
}

static const char * const tg_planets[] = { "Nowhere" };

static const char * const *tg_planet_names(u8 *count)
{
	*count = 1;
	return tg_planets;
}

const struct game_api testgame_game = {
	{ "TEST00001", NULL, NULL, NULL },
	GAME_RAC2,

	TG_FP_ADDR,
	tg_fp,
	NULL,                 /* no alternative fingerprint */
	sizeof(tg_fp),

	0,
	TG_PAD,

	tg_init,

	tg_hot,
	1,
	tg_hot_decode,

	tg_describe,

	tg_set_toggle,
	NULL,
	NULL,
	NULL,

	NULL, NULL, NULL,

	tg_planet_names,
	NULL,                 /* planet_load */
	NULL,                 /* die */
	NULL,                 /* load_setaside */

	NULL, NULL, NULL,     /* unlocks */
	NULL, NULL, NULL,     /* level flags */

	NULL,                 /* moby_table */

	NULL,                 /* on_enter */
	NULL,                 /* on_quit */
	NULL                  /* on_tick */
};
