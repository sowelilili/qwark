/*
 * The run-start warnings (revision 1.30). See runwarn.h.
 */
#include "runwarn.h"
#include "features.h"
#include "mods.h"
#include "config.h"
#include "util.h"
#include "../plat/plat.h"

/*
 * The unknown-legality mods already warned about, each the CRC-32 of its
 * title, a slash and its folder name, so a mod is one mod whatever slot a
 * rescan or another game puts it in. Full, the oldest goes, which at worst
 * warns about it again.
 */
#define RUNWARN_WARNED 32

static u32 g_warned[RUNWARN_WARNED];
static u8  g_nwarned;
static u8  g_warned_next;

#ifdef QWARK_TEST
static u32 g_starts, g_last_cheats, g_last_illegal, g_last_unknown;
#endif

static u32 popcount64(u64 v)
{
	u32 n = 0;
	while (v != 0) { v &= v - 1; n++; }
	return n;
}

static u32 mod_key(const char *title, const char *dirname)
{
	u32 st = qcrc32_start();
	st = qcrc32_update(st, (const u8 *)title, qstrlen(title));
	st = qcrc32_update(st, (const u8 *)"/", 1);
	st = qcrc32_update(st, (const u8 *)dirname, qstrlen(dirname));
	return qcrc32_finish(st);
}

static int warned(u32 key)
{
	u32 i;
	for (i = 0; i < g_nwarned; i++) {
		if (g_warned[i] == key) return 1;
	}
	return 0;
}

static void remember(u32 key)
{
	if (warned(key)) return;
	if (g_nwarned < RUNWARN_WARNED) {
		g_warned[g_nwarned++] = key;
		return;
	}
	g_warned[g_warned_next] = key;
	g_warned_next = (u8)((g_warned_next + 1) % RUNWARN_WARNED);
}

static void add_line(char *out, u32 cap, u32 *lines, u32 n, const char *one, const char *many)
{
	char num[12];

	if (n == 0) return;
	if (*lines > 0) qstrcat(out, cap, "\n");
	qstrcat(out, cap, "Warning: ");
	qfmt_u32(num, sizeof(num), n);
	qstrcat(out, cap, num);
	qstrcat(out, cap, n == 1 ? one : many);
	(*lines)++;
}

u32 runwarn_compose(char *out, u32 cap, u32 cheats, u32 illegal, u32 unknown)
{
	u32 lines = 0;

	if (cap == 0) return 0;
	out[0] = 0;
	add_line(out, cap, &lines, cheats,  " cheat enabled", " cheats enabled");
	add_line(out, cap, &lines, illegal, " mod enabled", " mods enabled");
	add_line(out, cap, &lines, unknown, " mod of unknown legality enabled", " mods of unknown legality enabled");
	return lines;
}

void runwarn_run_start(void)
{
	char msg[160];
	const char *title = mods_title();
	u32 illegal_mask, unknown_mask;
	u32 cheats, illegal, unknown, fresh = 0;
	u32 i;

	/* LIVE toggles as memory has them this tick, not as the last 10 Hz read left them. */
	features_poll_live();

	cheats       = popcount64(features_cheats_on());
	illegal_mask = mods_in_game_mask(MOD_FLAG_ILLEGAL);
	unknown_mask = mods_in_game_mask(0);
	illegal      = popcount64(illegal_mask);
	unknown      = popcount64(unknown_mask);

	for (i = 0; i < 32; i++) {
		const struct mod_entry *m;
		if ((unknown_mask & (1u << i)) == 0) continue;
		m = mods_at(i);
		if (m != NULL && !warned(mod_key(title, m->dirname))) fresh++;
	}

#ifdef QWARK_TEST
	g_starts++;
	g_last_cheats = cheats;
	g_last_illegal = illegal;
	g_last_unknown = unknown;
#endif

	if (!config_run_warnings()) {
		plat_log("runwarn: run start, %u cheats, %u illegal and %u unknown mods; warnings off",
		         (unsigned)cheats, (unsigned)illegal, (unsigned)unknown);
		return;
	}

	plat_log("runwarn: run start, %u cheats, %u illegal and %u unknown mods (%u not warned about yet)",
	         (unsigned)cheats, (unsigned)illegal, (unsigned)unknown, (unsigned)fresh);

	/* The unknown ones once: said, all of them, only while one of them is new. */
	if (fresh == 0) {
		unknown = 0;
	} else {
		for (i = 0; i < 32; i++) {
			const struct mod_entry *m;
			if ((unknown_mask & (1u << i)) == 0) continue;
			m = mods_at(i);
			if (m != NULL) remember(mod_key(title, m->dirname));
		}
	}

	if (runwarn_compose(msg, sizeof(msg), cheats, illegal, unknown) > 0) plat_notify(msg);
}

#ifdef QWARK_TEST
void runwarn_forget(void)
{
	g_nwarned = 0;
	g_warned_next = 0;
}

u32 runwarn_starts(void)       { return g_starts; }
u32 runwarn_last_cheats(void)  { return g_last_cheats; }
u32 runwarn_last_illegal(void) { return g_last_illegal; }
u32 runwarn_last_unknown(void) { return g_last_unknown; }
#endif
