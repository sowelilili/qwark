#include "manips.h"
#include "config.h"
#include "mem.h"
#include "util.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * The running game's table, NULL where it has none. Everything below is reset
 * by manips_enter on every INGAME entry, which is every game boot: a setup is
 * RAM the port keeps until the game reboots, so a new process needs it again.
 */
static const struct game_manips *g_m;

static u8  g_selected;         /* 0 = None */
static u8  g_setup_done;       /* the selected group's setup is in this boot */
static u32 g_announced;        /* bit id: "<group> manips activated" shown this boot,
                                  on the group's first application in it */

/*
 * The last event, for MANIP_LIST. The sequence number is never reset, so a
 * client that toasts each new number once never toasts one twice, across game
 * boots included; a new boot only clears the kind and the group.
 */
static u16 g_event_seq;
static u8  g_event_kind;
static u8  g_event_group;

#define DO_SETUP 0x01
#define DO_RUN   0x02

/* ----------------------------------------------------------------- tables */

static const struct manip_group *find_group(u8 id)
{
	u8 i;

	if (g_m == NULL || id == 0) return NULL;
	for (i = 0; i < g_m->ngroups; i++)
		if (g_m->groups[i].id == id) return &g_m->groups[i];
	return NULL;
}

static int lists_have_writes(const struct manip_list *l)
{
	u8 i;

	for (i = 0; i < MANIP_LISTS; i++)
		if (l[i].w != NULL && l[i].n != 0) return 1;
	return 0;
}

/* The wire flags: the table's own, plus PER_RUN where there is something per run. */
static u8 wire_flags(const struct manip_group *g)
{
	u8 flags = (u8)(g->flags & (MANIP_FLAG_NGPLUS | MANIP_FLAG_EXTENSION |
	                            MANIP_FLAG_NO_CHALLENGE));

	if (lists_have_writes(g->run)) flags |= MANIP_FLAG_PER_RUN;
	return flags;
}

static int write_one(const struct manip_write *w)
{
	switch (w->kind) {
	case MANIP_W_U8:  return mem_write_u8(w->addr, (u8)w->value);
	case MANIP_W_U32: return mem_write_u32(w->addr, w->value);
	case MANIP_W_BYTES:
		if (w->len == 0) return ST_OK;
		if (w->bytes == NULL) return mem_write_zeros(w->addr, w->len);
		return mem_write(w->addr, w->bytes, w->len);
	default:          return ST_BAD_ARG;
	}
}

/*
 * Every write of the lists, in order. One that fails does not stop the rest,
 * as none of the old buttons stopped; the first failure is the answer.
 */
static int write_lists(const struct manip_list *l)
{
	int first = ST_OK;
	u8 i, j;

	for (i = 0; i < MANIP_LISTS; i++) {
		if (l[i].w == NULL) continue;
		for (j = 0; j < l[i].n; j++) {
			int rc = write_one(&l[i].w[j]);
			if (rc != ST_OK && first == ST_OK) first = rc;
		}
	}
	return first;
}

/* --------------------------------------------------------------- options */

static int opt_auto_reset(void)
{
	return g_m != NULL && config_manip(g_m->key, "auto_reset", 1) != 0;
}

static int opt_remember(void)
{
	return g_m != NULL && config_manip(g_m->key, "remember", 0) != 0;
}

/* ---------------------------------------------------------------- events */

static void event(u8 kind, u8 group)
{
	g_event_seq++;
	g_event_kind = kind;
	g_event_group = group;
}

static void notify_group(const struct manip_group *g, const char *what)
{
	char msg[96];

	msg[0] = 0;
	qstrcat(msg, sizeof(msg), g->name);
	qstrcat(msg, sizeof(msg), what);
	plat_notify(msg);
}

static void refuse(const struct manip_group *g, u8 kind)
{
	event(kind, g->id);

	switch (kind) {
	case MANIP_EVENT_NOT_CHALLENGE:
		notify_group(g, " manips not applied: this is not a challenge mode file");
		break;
	case MANIP_EVENT_CHALLENGE:
		notify_group(g, " manips not applied: this is a challenge mode file");
		break;
	default:
		notify_group(g, " manips not applied: no save file is loaded");
		break;
	}
}

/* ------------------------------------------------------- the check, both ways */

/*
 * 0 when the group may write to the file in play, otherwise the refusal's event
 * kind; negative when the counter could not be read.
 */
static int challenge_check(const struct manip_group *g)
{
	u32 counter = 0;

	if ((g->flags & MANIP_FLAG_NO_CHALLENGE) != 0) return 0;

	if (g_m->challenge_width == 4) {
		if (mem_read_u32(g_m->challenge_addr, &counter) != ST_OK) return -1;
	} else {
		u8 b = 0;
		if (mem_read_u8(g_m->challenge_addr, &b) != ST_OK) return -1;
		counter = b;
	}

	if ((g->flags & MANIP_FLAG_NGPLUS) != 0)
		return counter >= 1 ? 0 : MANIP_EVENT_NOT_CHALLENGE;
	return counter == 0 ? 0 : MANIP_EVENT_CHALLENGE;
}

static int in_file(void)
{
	if (g_m == NULL || g_m->in_file == NULL) return 0;
	return g_m->in_file() > 0;
}

/*
 * One application of `what` (DO_SETUP, DO_RUN) of group `g`, after the
 * challenge check. A group with nothing in a list does not ask for it, and a
 * group with nothing to write at all is not checked: there is nothing to
 * refuse.
 */
static int attempt(const struct manip_group *g, u8 what)
{
	int rc = ST_OK;
	int check;
	int announce = 0;

	if (!lists_have_writes(g->setup)) what &= (u8)~DO_SETUP;
	if (!lists_have_writes(g->run))   what &= (u8)~DO_RUN;
	if (what == 0) return ST_OK;

	check = challenge_check(g);
	if (check < 0) return ST_IO_ERROR;
	if (check > 0) {
		refuse(g, (u8)check);
		return ST_BUSY;
	}

	if ((what & DO_SETUP) != 0) {
		rc = write_lists(g->setup);
		g_setup_done = 1;
	}

	if ((what & DO_RUN) != 0) {
		int rc2 = write_lists(g->run);
		if (rc == ST_OK) rc = rc2;
	}

	/*
	 * Build 53: the first application of a group in a boot is announced
	 * whatever it wrote. It used to take a setup, and a group with per-run
	 * writes alone - all five of RaC1's, RaC2's Any% and 100% - was never
	 * announced at all, on the console or on the PC. A group with a setup is
	 * no different: its first application in a boot always has the setup in
	 * it, since nothing marks the setup done but an application that wrote it.
	 */
	if ((g_announced & ((u32)1 << g->id)) == 0) {
		g_announced |= (u32)1 << g->id;
		announce = 1;
	}

	/*
	 * One event per application. "Activated" takes precedence over "applied",
	 * because the client sees only the latest event and the first application
	 * of a boot is the one worth a toast.
	 */
	if (announce) {
		event(MANIP_EVENT_SETUP, g->id);
		notify_group(g, " manips activated");
	} else {
		event(MANIP_EVENT_APPLIED, g->id);
	}

	return rc;
}

/* ------------------------------------------------------------- the session */

void manips_enter(const struct game_api *game)
{
	g_m = (game != NULL) ? game->manips : NULL;
	if (g_m != NULL && (g_m->groups == NULL || g_m->ngroups == 0)) g_m = NULL;

	g_setup_done = 0;
	g_announced = 0;
	g_event_kind = MANIP_EVENT_NONE;
	g_event_group = 0;
	g_selected = 0;

	if (g_m == NULL) return;

	/*
	 * With remember off the category is forgotten when the game boots; config
	 * still holds it, so turning remember on later has it to hand.
	 */
	if (opt_remember()) {
		u32 id = config_manip(g_m->key, "category", 0);
		if (id <= 0xFF && find_group((u8)id) != NULL) g_selected = (u8)id;
	}
}

void manips_file_loaded(void)
{
	const struct manip_group *g = find_group(g_selected);
	u8 what = 0;

	if (g == NULL) return;

	if (!g_setup_done) what |= DO_SETUP;
	if (opt_auto_reset()) what |= DO_RUN;

	attempt(g, what);
}

int manips_supported(void)
{
	return g_m != NULL;
}

/* ------------------------------------------------------------------- ops */

int manips_list(u8 *out, u32 cap, u32 *len)
{
	u32 off = 0;
	u8 i;

	*len = 0;
	if (g_m == NULL) return ST_UNSUPPORTED;

	if (cap < 8) return ST_FULL;
	out[off++] = g_selected;
	out[off++] = (u8)opt_auto_reset();
	out[off++] = (u8)opt_remember();
	out[off++] = g_m->ngroups;
	be16_put(out + off, g_event_seq);
	off += 2;
	out[off++] = g_event_kind;
	out[off++] = g_event_group;

	for (i = 0; i < g_m->ngroups; i++) {
		const struct manip_group *g = &g_m->groups[i];
		u32 nlen = qstrlen(g->name);
		u32 clen = qstrlen(g->cats);

		if (nlen > 0xFF) nlen = 0xFF;
		if (clen > 0xFF) clen = 0xFF;
		if (off + 3u + nlen + 1u + clen > cap) return ST_FULL;

		out[off++] = g->id;
		out[off++] = wire_flags(g);
		out[off++] = (u8)nlen;
		memcpy(out + off, g->name, nlen);
		off += nlen;
		out[off++] = (u8)clen;
		memcpy(out + off, g->cats, clen);
		off += clen;
	}

	if (off + 1u > cap) return ST_FULL;
	out[off++] = g_m->nheaders;
	for (i = 0; i < g_m->nheaders; i++) {
		const struct manip_header *h = &g_m->headers[i];
		u32 nlen = qstrlen(h->name);
		u8 j;

		if (nlen > 0xFF) nlen = 0xFF;
		if (off + 1u + nlen + 1u + 2u * (u32)h->nfeatures > cap) return ST_FULL;

		out[off++] = (u8)nlen;
		memcpy(out + off, h->name, nlen);
		off += nlen;
		out[off++] = h->nfeatures;
		for (j = 0; j < h->nfeatures; j++) {
			be16_put(out + off, h->features[j]);
			off += 2;
		}
	}

	*len = off;
	return ST_OK;
}

int manips_select(u8 id)
{
	if (g_m == NULL) return ST_UNSUPPORTED;
	if (id != 0 && find_group(id) == NULL) return ST_BAD_ARG;

	/*
	 * The same group again is nothing: a client that re-sends its selection
	 * must not rewrite a setup, nor raise a refusal, behind the user's back.
	 * Apply now is the way to write it again.
	 */
	if (id == g_selected) return ST_OK;

	g_selected = id;
	g_setup_done = 0;
	config_set_manip(g_m->key, "category", id);

	/*
	 * Build 54, round 3: nothing is written now, whatever is in play. The
	 * group's setup is due at the next start of a run that passes its check,
	 * with its per-run writes, or at Apply now. Build 53 put the setup in at
	 * once when a file was in play, so stepping through the categories on the
	 * console menu applied (or refused) each group on the way, and the user's
	 * call is the old checkboxes': manips go in at the start of a run.
	 */
	return ST_OK;
}

int manips_options(u8 auto_reset, u8 remember)
{
	if (g_m == NULL) return ST_UNSUPPORTED;
	if (auto_reset > 1 || remember > 1) return ST_BAD_ARG;

	if ((int)auto_reset != opt_auto_reset())
		config_set_manip(g_m->key, "auto_reset", auto_reset);

	if ((int)remember != opt_remember()) {
		config_set_manip(g_m->key, "remember", remember);
		/* So what comes back at the next boot is what is selected now. */
		if (remember) config_set_manip(g_m->key, "category", g_selected);
	}

	return ST_OK;
}

int manips_apply(void)
{
	const struct manip_group *g;

	if (g_m == NULL) return ST_UNSUPPORTED;

	g = find_group(g_selected);
	if (g == NULL) return ST_OK;   /* None writes nothing */

	if (!in_file()) {
		refuse(g, MANIP_EVENT_NO_FILE);
		return ST_BUSY;
	}

	return attempt(g, DO_SETUP | DO_RUN);
}

u8 manips_options_now(void)
{
	return (u8)(opt_auto_reset() | (opt_remember() << 1));
}

/* ------------------------------------------------------------- the tests */

u8  manips_selected(void)    { return g_selected; }
int manips_setup_done(void)  { return g_setup_done; }
u16 manips_event_seq(void)   { return g_event_seq; }
u8  manips_event_kind(void)  { return g_event_kind; }
u8  manips_event_group(void) { return g_event_group; }
