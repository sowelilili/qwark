/*
 * The registry: title id to game.
 *
 * Each of the four originals owns its own NPEA id. The disc trilogy BCES01503
 * hosts RaC1, RaC2 and RaC3 behind one title id, so it is registered on all
 * three and the session fingerprints each candidate in turn: whichever
 * executable is actually mapped answers, and the old disc-collection picker
 * goes away.
 */
#include "game.h"
#include "../core/util.h"

extern const struct game_api rac1_game;
extern const struct game_api rac2_game;
extern const struct game_api rac3_game;
extern const struct game_api rac4_game;

#ifdef QWARK_TEST
/* A second registered title, so the host tests can exercise the different-title
 * path through the session state machine. Defined in test/test_game.c. */
extern const struct game_api testgame_game;
#endif

static const struct game_api * const g_games[] = {
	&rac1_game,
	&rac2_game,
	&rac3_game,
	&rac4_game
#ifdef QWARK_TEST
	, &testgame_game
#endif
};

#define GAME_COUNT ((u32)(sizeof(g_games) / sizeof(g_games[0])))

static int game_has_title(const struct game_api *g, const char *title_id)
{
	int t;

	for (t = 0; t < 4; t++) {
		if (g->title_ids[t] == NULL) return 0;
		if (qstreq(g->title_ids[t], title_id)) return 1;
	}
	return 0;
}

const struct game_api *game_for_title(const char *title_id)
{
	u32 i;

	if (title_id == NULL || title_id[0] == 0) return NULL;

	for (i = 0; i < GAME_COUNT; i++) {
		const struct game_api *g = g_games[i];

		if (!game_has_title(g, title_id)) continue;

		if (g->init != NULL) g->init();
		return g;
	}

	return NULL;
}

u32 game_candidates_for_title(const char *title_id, const struct game_api **out, u32 cap)
{
	u32 i;
	u32 n = 0;

	if (title_id == NULL || title_id[0] == 0 || out == NULL) return 0;

	for (i = 0; i < GAME_COUNT && n < cap; i++) {
		const struct game_api *g = g_games[i];

		if (!game_has_title(g, title_id)) continue;

		if (g->init != NULL) g->init();
		out[n++] = g;
	}

	return n;
}

const struct game_api *game_at(u32 index)
{
	if (index >= GAME_COUNT) return NULL;
	return g_games[index];
}
