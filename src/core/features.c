#include "features.h"
#include "config.h"
#include "util.h"

#include <string.h>

static const struct game_api *g_game;
static char g_title[16];
static u64 g_toggle_state;
static u64 g_toggle_auto;

static const struct feature_desc *find_desc(u8 id)
{
	const struct game_describe *d;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL) return NULL;

	d = g_game->describe();
	if (d == NULL) return NULL;

	for (i = 0; i < d->nfeatures; i++) {
		if (d->features[i].id == id) return &d->features[i];
	}
	return NULL;
}

u64 features_auto_mask(void)
{
	const struct game_describe *d;
	u64 mask = 0;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL || g_title[0] == 0) return 0;

	d = g_game->describe();
	if (d == NULL) return 0;

	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];
		int fallback;

		if (f->kind != FEATURE_TOGGLE) continue;
		if (f->id >= QWARK_MAX_FEATURES) continue;

		/* A game may ship a toggle auto-flagged; config still overrides it. */
		fallback = (d->auto_default & ((u64)1 << f->id)) != 0;
		if (config_feature_auto_default(g_title, f->label, fallback))
			mask |= (u64)1 << f->id;
	}

	return mask;
}

void features_set_game(const struct game_api *game, const char *title)
{
	g_game = game;
	qstrcpy(g_title, sizeof(g_title), title ? title : "");
	g_toggle_state = 0;
	g_toggle_auto = features_auto_mask();
}

void features_forget_state(void)
{
	g_toggle_state = 0;
}

u64 features_toggle_state(void) { return g_toggle_state; }
u64 features_toggle_auto(void)  { return g_toggle_auto; }

int features_set(u8 id, u32 value)
{
	const struct feature_desc *f = find_desc(id);
	int rc;

	if (g_game == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;

	if (f->kind == FEATURE_TOGGLE) {
		if (g_game->set_toggle == NULL) return ST_UNSUPPORTED;
		rc = g_game->set_toggle(id, value != 0);
		if (rc != ST_OK) return rc;

		if (value) g_toggle_state |= (u64)1 << id;
		else       g_toggle_state &= ~((u64)1 << id);
		return ST_OK;
	}

	if (f->kind == FEATURE_ACTION) return ST_BAD_ARG;

	if (g_game->set_value == NULL) return ST_UNSUPPORTED;

	/* min and max are both 0 when the range is unbounded. */
	if (!(f->min == 0 && f->max == 0)) {
		if (value < f->min || value > f->max) return ST_BAD_ARG;
	}

	return g_game->set_value(id, value);
}

int features_trigger(u8 id)
{
	const struct feature_desc *f = find_desc(id);

	if (g_game == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;
	if (f->kind != FEATURE_ACTION) return ST_BAD_ARG;
	if (g_game->trigger == NULL) return ST_UNSUPPORTED;

	return g_game->trigger(id);
}

int features_set_auto(u8 id, int on)
{
	const struct feature_desc *f = find_desc(id);

	if (g_game == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;
	if (f->kind != FEATURE_TOGGLE) return ST_BAD_ARG;

	config_set_feature_auto(g_title, f->label, on);

	if (on) g_toggle_auto |= (u64)1 << id;
	else    g_toggle_auto &= ~((u64)1 << id);

	return ST_OK;
}

int features_options(u8 id, const char * const **options, u8 *count)
{
	const struct feature_desc *f = find_desc(id);

	if (g_game == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;
	if (f->kind != FEATURE_ENUM) return ST_BAD_ARG;
	if (g_game->get_options == NULL) return ST_UNSUPPORTED;

	return g_game->get_options(id, options, count);
}

void features_apply_mask(u64 mask)
{
	const struct game_describe *d;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL) return;

	d = g_game->describe();
	if (d == NULL) return;

	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];
		if (f->kind != FEATURE_TOGGLE) continue;
		if (f->id >= QWARK_MAX_FEATURES) continue;
		if ((mask & ((u64)1 << f->id)) == 0) continue;
		features_set(f->id, 1);
	}
}

/* -------------------------------------------------------------- DESCRIBE */

static void put_fixed(u8 *dst, u32 cap, const char *src)
{
	u32 i = 0;
	memset(dst, 0, cap);
	if (src == NULL) return;
	while (i < cap && src[i] != 0) { dst[i] = (u8)src[i]; i++; }
}

int features_describe(u8 *out, u32 cap, u32 *len)
{
	const struct game_describe *d;
	u32 need;
	u32 off = 0;
	u8 i;

	*len = 0;

	if (g_game == NULL || g_game->describe == NULL) return ST_UNSUPPORTED;

	d = g_game->describe();
	if (d == NULL) return ST_UNSUPPORTED;

	/* The caps in PROTOCOL.md 5.3, so a client can size its buffers statically. */
	if (d->ngroups > QWARK_MAX_GROUPS || d->nreadouts > QWARK_MAX_READOUTS ||
	    d->nfeatures > QWARK_MAX_FEATURES)
		return ST_FULL;

	need = 1 + 1 + (u32)d->ngroups * 24 + 1 + (u32)d->nreadouts * 24
	     + 1 + (u32)d->nfeatures * FEATURE_WIRE_SIZE;
	if (need > cap) return ST_FULL;

	out[off++] = g_game->game_id;

	out[off++] = d->ngroups;
	for (i = 0; i < d->ngroups; i++) {
		put_fixed(out + off, 24, d->groups[i]);
		off += 24;
	}

	out[off++] = d->nreadouts;
	for (i = 0; i < d->nreadouts; i++) {
		put_fixed(out + off, 24, d->readouts[i]);
		off += 24;
	}

	out[off++] = d->nfeatures;
	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];
		u8 flags = f->flags;
		u8 readout = f->readout;

		if (f->kind == FEATURE_TOGGLE && f->id < QWARK_MAX_FEATURES &&
		    (g_toggle_auto & ((u64)1 << f->id)) != 0)
			flags |= FEATURE_FLAG_AUTO;

		/* Toggle state travels in toggle_state, so those rows never name one. */
		if (f->kind == FEATURE_TOGGLE || f->kind == FEATURE_ACTION)
			readout = FEATURE_NO_READOUT;
		else if (readout >= QWARK_MAX_READOUTS)
			readout = FEATURE_NO_READOUT;

		out[off + 0] = f->id;
		out[off + 1] = f->kind;
		out[off + 2] = f->group;
		out[off + 3] = (f->kind == FEATURE_ENUM) ? f->aux : 0;
		out[off + 4] = flags;
		out[off + 5] = readout;
		out[off + 6] = 0;
		out[off + 7] = 0;
		be32_put(out + off + 8, f->min);
		be32_put(out + off + 12, f->max);
		put_fixed(out + off + 16, 32, f->label);
		off += FEATURE_WIRE_SIZE;
	}

	*len = off;
	return ST_OK;
}
