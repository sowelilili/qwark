#include "features.h"
#include "config.h"
#include "switches.h"
#include "util.h"
#include "../plat/plat.h"

#include <string.h>

static const struct game_api *g_game;
static char g_title[16];
static u64 g_toggle_state;
static u64 g_toggle_auto;

/*
 * Build 50. WRITES_CODE toggles features_apply_mask was asked for, on a platform
 * that cannot patch code, before the tick had looked for the game's code
 * switches: the auto-apply at INGAME always comes first, and a PREVIOUS_REAPPLY
 * may. They are applied or dropped by features_apply_waiting when the look
 * answers, and forgotten with the rest of the toggle state.
 *
 * Build 51: the emulator builds' alone (QWARK_EMULATOR, plat.h). A console
 * can patch code, so nothing waits there and the two below are nothing.
 */
#ifdef QWARK_EMULATOR
static u64 g_code_waiting;

static void code_wait(u8 id)       { g_code_waiting |= (u64)1 << id; }
static void code_wait_forget(void) { g_code_waiting = 0; }
#else
static __inline void code_wait(u8 id)       { (void)id; }
static __inline void code_wait_forget(void) { }
#endif

/*
 * Whether a WRITES_CODE feature can be set on this platform: always where code
 * can be patched, and elsewhere only while the game's code switches are in.
 */
static int code_ok(void)
{
	return plat_can_patch_code() || switches_present();
}

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
		/* A LIVE toggle is the game's byte, not ours: there is nothing to re-apply. */
		if ((f->flags & FEATURE_FLAG_LIVE) != 0) continue;

		/* A game may ship a toggle auto-flagged; config still overrides it. */
		fallback = (d->auto_default & ((u64)1 << f->id)) != 0;
		if (config_feature_auto_default(g_title, f->label, fallback))
			mask |= (u64)1 << f->id;
	}

	return mask;
}

u64 features_live_mask(void)
{
	const struct game_describe *d;
	u64 mask = 0;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL) return 0;

	d = g_game->describe();
	if (d == NULL) return 0;

	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];

		if (f->kind != FEATURE_TOGGLE) continue;
		if (f->id >= QWARK_MAX_FEATURES) continue;
		if ((f->flags & FEATURE_FLAG_LIVE) == 0) continue;

		mask |= (u64)1 << f->id;
	}

	return mask;
}

u64 features_confirm_mask(void)
{
	const struct game_describe *d;
	u64 mask = 0;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL) return 0;

	d = g_game->describe();
	if (d == NULL) return 0;

	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];

		if (f->kind != FEATURE_TOGGLE) continue;
		if (f->id >= QWARK_MAX_FEATURES) continue;
		if ((f->flags & FEATURE_FLAG_CONFIRM) == 0) continue;

		mask |= (u64)1 << f->id;
	}

	return mask;
}

void features_poll_live(void)
{
	const struct game_describe *d;
	int switched;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL) return;
	if (g_game->toggle_read == NULL) return;

	d = g_game->describe();
	if (d == NULL) return;

	/*
	 * Build 50. A WRITES_CODE toggle is read back only while the switches are
	 * in: its state is then the flag byte. On a console and without them it is
	 * a patch, which qwark's own bit describes and memory does not.
	 */
	switched = switches_present();

	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];
		int on = 0;

		if (f->kind != FEATURE_TOGGLE) continue;
		if (f->id >= QWARK_MAX_FEATURES) continue;
		if ((f->flags & FEATURE_FLAG_LIVE) == 0 &&
		    !(switched && (f->flags & FEATURE_FLAG_WRITES_CODE) != 0)) continue;

		/* A read that fails leaves the bit where it was rather than lying. */
		if (g_game->toggle_read(f->id, &on) != ST_OK) continue;

		if (on) g_toggle_state |= (u64)1 << f->id;
		else    g_toggle_state &= ~((u64)1 << f->id);
	}
}

void features_set_game(const struct game_api *game, const char *title)
{
	g_game = game;
	qstrcpy(g_title, sizeof(g_title), title ? title : "");
	g_toggle_state = 0;
	code_wait_forget();
	g_toggle_auto = features_auto_mask();
}

void features_forget_state(void)
{
	g_toggle_state = 0;
	code_wait_forget();
}

u64 features_toggle_state(void) { return g_toggle_state; }
u64 features_toggle_auto(void)  { return g_toggle_auto; }

int features_set(u8 id, u32 value)
{
	const struct feature_desc *f = find_desc(id);
	int rc;

	if (g_game == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;

	/*
	 * A WRITES_CODE feature is an instruction patch wearing a checkbox, so on a
	 * platform that cannot patch code it is refused rather than half-applied.
	 * The client already knows: SessionInfo flags bit2 greys the row.
	 *
	 * Build 50: unless the game's code switches are in, put there by an RPCS3
	 * patch at load. Then the game's own handler writes the toggle's flag byte
	 * instead of the instruction (classic_code_toggle), and bit4 tells the
	 * client to offer the row. Before the tick has looked this still refuses,
	 * which is what a client that has not seen bit4 expects.
	 */
	if ((f->flags & FEATURE_FLAG_WRITES_CODE) != 0 && !code_ok())
		return ST_UNSUPPORTED;

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
	/* Same rule as features_set: an ACTION that patches code cannot work here. */
	if ((f->flags & FEATURE_FLAG_WRITES_CODE) != 0 && !plat_can_patch_code())
		return ST_UNSUPPORTED;

	return g_game->trigger(id);
}

int features_set_auto(u8 id, int on)
{
	const struct feature_desc *f = find_desc(id);

	if (g_game == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;
	if (f->kind != FEATURE_TOGGLE) return ST_BAD_ARG;
	/*
	 * A LIVE toggle has no auto bit to set: qwark never re-applies it, so
	 * saying yes here would promise something the module does not do.
	 */
	if ((f->flags & FEATURE_FLAG_LIVE) != 0) return ST_UNSUPPORTED;

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

int features_confirm_text(u8 id, u8 *out, u32 cap, u32 *len)
{
	const struct feature_desc *f = find_desc(id);
	const struct game_describe *d;
	u32 n;
	u8 i;

	*len = 0;
	if (g_game == NULL || g_game->describe == NULL) return ST_UNSUPPORTED;
	if (f == NULL) return ST_NOT_FOUND;
	if ((f->flags & FEATURE_FLAG_CONFIRM) == 0) return ST_BAD_ARG;

	d = g_game->describe();
	if (d == NULL || d->confirms == NULL) return ST_NOT_FOUND;

	for (i = 0; i < d->nconfirms; i++) {
		if (d->confirms[i].id != id || d->confirms[i].text == NULL) continue;

		n = qstrlen(d->confirms[i].text);
		if (n > FEATURE_CONFIRM_TEXT_MAX || n > cap) return ST_FULL;
		memcpy(out, d->confirms[i].text, n);
		*len = n;
		return ST_OK;
	}
	return ST_NOT_FOUND;
}

void features_apply_mask(u64 mask)
{
	const struct game_describe *d;
	int can_patch = plat_can_patch_code();
	int switches = switches_state();
	u8 skipped = 0;
	u8 i;

	if (g_game == NULL || g_game->describe == NULL) return;

	d = g_game->describe();
	if (d == NULL) return;

	for (i = 0; i < d->nfeatures; i++) {
		const struct feature_desc *f = &d->features[i];
		if (f->kind != FEATURE_TOGGLE) continue;
		if (f->id >= QWARK_MAX_FEATURES) continue;
		/* Never write a LIVE toggle behind the user's back; the game owns it. */
		if ((f->flags & FEATURE_FLAG_LIVE) != 0) continue;
		if ((mask & ((u64)1 << f->id)) == 0) continue;
		/*
		 * An auto-flagged WRITES_CODE toggle would answer UNSUPPORTED anyway;
		 * skipping it quietly here keeps the boot path free of noise, and the
		 * one line below says how many went by.
		 *
		 * Build 50: unless the code switches are in, when it goes on like any
		 * other toggle, as a byte write. And while nobody knows yet - at INGAME
		 * the tick has not looked - it waits for the answer rather than being
		 * skipped for want of one.
		 */
		if (!can_patch && (f->flags & FEATURE_FLAG_WRITES_CODE) != 0 && switches != 1) {
			if (switches < 0) code_wait(f->id);
			else              skipped++;
			continue;
		}
		features_set(f->id, 1);
	}

	if (skipped != 0)
		plat_log("qwark: %d code-patch toggle(s) skipped, this platform cannot patch code",
		         (int)skipped);
}

#ifdef QWARK_EMULATOR   /* features.h: nothing waits on the PS3 */
void features_apply_waiting(void)
{
	u64 mask = g_code_waiting;

	g_code_waiting = 0;
	if (mask != 0) features_apply_mask(mask);
}
#endif

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
		u8 bits = 0;

		if (f->kind == FEATURE_TOGGLE && f->id < QWARK_MAX_FEATURES &&
		    (g_toggle_auto & ((u64)1 << f->id)) != 0)
			flags |= FEATURE_FLAG_AUTO;

		/* Toggle state travels in toggle_state, so those rows never name one. */
		if (f->kind == FEATURE_TOGGLE || f->kind == FEATURE_ACTION)
			readout = FEATURE_NO_READOUT;
		else if (readout >= QWARK_MAX_READOUTS)
			readout = FEATURE_NO_READOUT;

		/*
		 * Protocol 1.7. A VALUE's `aux` is the width of the field behind it, and
		 * only 8, 16 and 32 mean anything. A row that names no width sends 0,
		 * which is what every row sent before this revision and what a client is
		 * to read as 32, so the old rows and the new ones read the same way.
		 */
		if (f->kind == FEATURE_VALUE &&
		    (f->aux == 8 || f->aux == 16 || f->aux == 32))
			bits = f->aux;

		out[off + 0] = f->id;
		out[off + 1] = f->kind;
		out[off + 2] = f->group;
		out[off + 3] = (f->kind == FEATURE_ENUM) ? f->aux : 0;
		out[off + 4] = flags;
		out[off + 5] = readout;
		out[off + 6] = bits;
		out[off + 7] = 0;
		be32_put(out + off + 8, f->min);
		be32_put(out + off + 12, f->max);
		put_fixed(out + off + 16, 32, f->label);
		off += FEATURE_WIRE_SIZE;
	}

	*len = off;
	return ST_OK;
}
