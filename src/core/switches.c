#include "switches.h"
#include "mem.h"
#include "session.h"
#include "util.h"
#include "../games/switches_bins.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * Whether the running game's switches are in it: -1 until the tick thread has
 * looked, then 0 or 1 for the rest of the session. Only ever anything but -1 on
 * a platform that refuses code patches; switches_state() answers 0 on a console
 * whatever this holds.
 *
 * One look a session is the whole of the question, as it is for the savefile
 * helper: a patch goes in as RPCS3 loads the executable or not at all, and
 * nothing takes one out while the game runs.
 */
static int g_state = -1;

/*
 * How far the look has got: caves 0..ncaves-1 and then the sites, one part
 * each, and how many bytes into the current cave.
 */
static u32 g_part;
static u32 g_at;

/*
 * At most this many reads a tick. Each is one PINE round trip under RPCS3, and
 * a tick that waits on the emulator for long holds everything else up, so the
 * look is spread over a few ticks the way the mods' look is. Caves that follow
 * one another with no gap between them are one read, so Deadlocked's eighteen,
 * 492 bytes back to back, are one, and its fifteen site words are fifteen:
 * four ticks. RaC1's five trampolines are three runs, and with its five sites
 * that is two ticks; RaC2's four and four (build 53's crash guards) are two
 * as well, and RaC3 takes one.
 */
#define SW_LOOK_READS 4u

static const struct sw_desc *desc_now(void)
{
	const struct game_api *g = session_game();

	if (g == NULL) return NULL;
	return sw_desc_for_game(g->game_id);
}

void switches_forget(void)
{
	g_state = -1;
	g_part = 0;
	g_at = 0;
}

int switches_state(void)
{
	if (plat_can_patch_code()) return 0;
	return g_state;
}

int switches_present(void)
{
	return switches_state() == 1;
}

/*
 * One read's worth of the look, from the cursor: 2 when there is more to read,
 * 1 when every cave byte and every site word has read back as the table has it,
 * 0 at the first one that does not, and -1 when the read failed and nothing can
 * be said yet.
 *
 * All of it or nothing. A patch from an older build of the switches differs
 * somewhere - a cave byte, or a site word that branches to where an older
 * trampoline began - and reads as not present, and so does a game with only
 * some of it in: a trampoline without its site is dead code, and a site
 * without its trampoline is a crash waiting for its first frame. The flag bytes
 * are no part of it. The patch zeroes them at load and qwark writes them from
 * then on, so what they hold says nothing about whether the patch went in.
 */
static int look_step(const struct sw_desc *d)
{
	/* The tick thread's scratch buffer, filled and compared inside this call. */
	u8 *buf = qscratch();

	if (g_part < d->ncaves) {
		u32 addr = d->caves[g_part].addr + g_at;
		u32 part = g_part;
		u32 at = g_at;
		u32 n = 0;
		u32 off = 0;

		/* As far as the caves run on with no gap, and the buffer holds. */
		while (part < d->ncaves && n < QSCRATCH_BYTES) {
			u32 take = d->caves[part].len - at;

			if (n > 0 && d->caves[part].addr != addr + n) break;
			if (take > QSCRATCH_BYTES - n) take = QSCRATCH_BYTES - n;

			n += take;
			at += take;
			if (at < d->caves[part].len) break;
			part++;
			at = 0;
		}

		if (mem_read(addr, buf, n) != ST_OK) return -1;

		while (off < n) {
			const struct sw_cave *c = &d->caves[g_part];
			u32 take = c->len - g_at;

			if (take > n - off) take = n - off;
			if (memcmp(buf + off, c->bytes + g_at, take) != 0) return 0;

			off += take;
			g_at += take;
			if (g_at == c->len) {
				g_part++;
				g_at = 0;
			}
		}
		return 2;
	}

	if (g_part - d->ncaves < d->nsites) {
		const struct patch_word *s = &d->sites[g_part - d->ncaves];
		u32 word = 0;

		if (mem_read_u32(s->addr, &word) != ST_OK) return -1;
		if (word != s->value) return 0;

		g_part++;
		if (g_part - d->ncaves < d->nsites) return 2;
	}

	return 1;
}

int switches_tick(void)
{
	const struct sw_desc *d;
	u32 reads;

	if (g_state >= 0 || plat_can_patch_code() || !mem_is_ingame()) return 0;

	/*
	 * A process no game claims, or a game with no switches, has nothing to
	 * look for: the answer is no, and it is an answer, so whatever was waiting
	 * for it is told.
	 */
	d = desc_now();
	if (d == NULL) {
		g_state = 0;
		return 1;
	}

	for (reads = 0; reads < SW_LOOK_READS; reads++) {
		int rc = look_step(d);

		/*
		 * A read that fails leaves the answer unknown and the cursor where it
		 * was: a stalled emulator is not a game without the switches, and the
		 * session would otherwise carry that wrong answer to its end.
		 */
		if (rc < 0) return 0;
		if (rc == 2) continue;

		g_state = rc;
		if (rc) plat_log("switches: the code switches are in game %d, patched in at load", (int)d->game_id);
		else    plat_log("switches: no code switches in game %d, or ones from another build", (int)d->game_id);
		return 1;
	}

	return 0;
}

/* ------------------------------------------------ SWITCH_PATCH, revision 1.17 */

int switches_patch(u8 *out, u32 cap, u32 *len)
{
	const struct sw_desc *d;
	u32 n = 0;
	u32 off = SWITCH_PATCH_HEAD;
	u32 need;
	u32 i;

	*len = 0;

	/*
	 * The platform is not asked and neither is the game: this describes the
	 * switches and does not need them to be there. (The PS3 module has no
	 * SWITCH_PATCH since build 51; where the simulator stands in for a console
	 * it is informational.)
	 */
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	d = desc_now();
	if (d == NULL) return ST_UNSUPPORTED;

	for (i = 0; i < d->ncaves; i++) n += (d->caves[i].len + 3u) / 4u;
	n += d->nsites;

	need = SWITCH_PATCH_HEAD + n * SWITCH_PATCH_ROW
	     + SWITCH_PATCH_BYTES_HEAD + (u32)d->nflags * SWITCH_PATCH_BYTE_ROW;
	if (n > 0xFFFFu || need > cap) return ST_FULL;

	/*
	 * Caves first, as consecutive words in address order, and the site words
	 * after them: a reader applying the words one at a time never has a branch
	 * in place before the trampoline it branches to. Every cave is whole words -
	 * instructions, and Deadlocked's sixteen-byte table - so none is padded; the
	 * rounding is there for one that one day is not.
	 */
	for (i = 0; i < d->ncaves; i++) {
		const struct sw_cave *c = &d->caves[i];
		u32 at;

		for (at = 0; at < c->len; at += 4u) {
			u32 k;

			be32_put(out + off, c->addr + at);
			for (k = 0; k < 4u; k++) out[off + 4u + k] = at + k < c->len ? c->bytes[at + k] : 0;
			off += SWITCH_PATCH_ROW;
		}
	}

	for (i = 0; i < d->nsites; i++) {
		be32_put(out + off, d->sites[i].addr);
		be32_put(out + off + 4u, d->sites[i].value);
		off += SWITCH_PATCH_ROW;
	}

	/*
	 * Then every flag byte, at 0, so every switch is off at every boot whatever
	 * the executable holds there. Every flag is zero in the file today - a gap
	 * at the end of a data segment, or zero-filled BSS - so this is the belt to
	 * that: a flag that loaded non-zero would turn a cheat on at boot that the
	 * client's checkbox says is off.
	 */
	be16_put(out + off, (u16)d->nflags);
	be16_put(out + off + 2u, 0);
	off += SWITCH_PATCH_BYTES_HEAD;

	for (i = 0; i < d->nflags; i++) {
		be32_put(out + off, d->flags[i]);
		out[off + 4u] = 0;
		out[off + 5u] = 0;
		out[off + 6u] = 0;
		out[off + 7u] = 0;
		off += SWITCH_PATCH_BYTE_ROW;
	}

	/* The CRC of everything after the stamp, exactly as it goes out. */
	be16_put(out, (u16)n);
	be16_put(out + 2, 0);
	be32_put(out + 4, qcrc32(out + SWITCH_PATCH_HEAD, off - SWITCH_PATCH_HEAD));

	*len = off;
	return ST_OK;
}
