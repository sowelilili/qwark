/*
 * The linked registry: title id to game vtable, in the builds whose games are
 * linked into the module (a release module, the host builds and the tests).
 * The title index (titles.c) says which games a title can be; here each entry
 * hands out its vtable. A test module's games are modules (src/games/module.h)
 * and this file is not in it.
 */
#include "game.h"
#include "titles.h"
#include "../core/util.h"

static const struct game_api *linked_init(const struct game_title *t)
{
	const struct game_api *g = t != NULL ? t->linked : NULL;

	if (g != NULL && g->init != NULL) g->init();
	return g;
}

const struct game_api *game_for_title(const char *title_id)
{
	const struct game_title *t[GAME_MAX_CANDIDATES];

	if (title_candidates(title_id, t, GAME_MAX_CANDIDATES) == 0) return NULL;
	return linked_init(t[0]);
}

u32 game_candidates_for_title(const char *title_id, const struct game_api **out, u32 cap)
{
	const struct game_title *t[GAME_MAX_CANDIDATES];
	u32 i, n, k = 0;

	if (out == NULL) return 0;
	n = title_candidates(title_id, t, GAME_MAX_CANDIDATES);
	for (i = 0; i < n && k < cap; i++) {
		const struct game_api *g = linked_init(t[i]);
		if (g != NULL) out[k++] = g;
	}
	return k;
}

const struct game_api *game_at(u32 index)
{
	const struct game_title *t = title_at(index);
	return t != NULL ? t->linked : NULL;
}

const struct game_api *game_linked(u8 game_id)
{
	const struct game_title *t = title_for_game(game_id);
	return linked_init(t);
}

const struct game_title *game_title_of(const struct game_api *g)
{
	const struct game_title *t;
	u32 i;

	for (i = 0; (t = title_at(i)) != NULL; i++) {
		if (t->linked == g) return t;
	}
	return NULL;
}
