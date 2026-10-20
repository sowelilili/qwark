/*
 * Revision 1.23, build 55: the console menu's Help row as a switch, config.txt's
 * `menu.help` (MENU_HELP), on qwark's fake console, in all four games.
 *
 *   the table     each renderer's Help row words are the last `nhelp` hooks;
 *   the swap      at 0 the Help row's words go back to the game's own while every
 *                 other word stays the renderer's, and at 1 they go in again,
 *                 those words alone, in the running game, through the same order
 *                 install() and uninstall() keep;
 *   the deferral  no swap while the game is paused, a mode change is queued, a
 *                 screen of the renderer's is up or the menu is open: it waits
 *                 for the first tick of ordinary play;
 *   the rest      SessionInfo flags bit6, the combo still opening the menu at 0,
 *                 menu.enabled = 0 putting every word back either way, an install
 *                 with menu.help = 0, a renderer taken over, another mod's word
 *                 at a Help site, and a combo press that installs in the pause.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/games/menuhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot(const char *title)
{
	int i;

	host_boot(title);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME) return 1;
	}
	return 0;
}

static int quit(void)
{
	int i;

	host_quit();
	for (i = 0; i < 2000; i++) {
		tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

static u32 peek32(u32 a)
{
	u8 b[4];
	host_peek(a, b, 4);
	return be32_get(b);
}

static u8 peek8(u32 a)
{
	u8 b = 0;
	host_peek(a, &b, 1);
	return b;
}

static void poke32(u32 a, u32 v)
{
	u8 b[4];
	be32_put(b, v);
	host_poke(a, b, 4);
}

static void poke8(u32 a, u8 v)
{
	host_poke(a, &v, 1);
}

static void press(u32 mask)
{
	host_set_pad(0);
	tick(2);
	host_set_pad(mask);
	tick(2);
	host_set_pad(0);
	tick(2);
}

static int first_help(const struct mh_desc *d)
{
	return (int)d->nhooks - (int)d->nhelp;
}

/* Hooks [from, to) all hold the renderer's word (`ours`), or all the game's. */
static int words_are(const struct mh_desc *d, int from, int to, int ours)
{
	int i;
	for (i = from; i < to; i++) {
		u32 want = ours ? d->hooks[i].value : d->hooks[i].original;
		if (peek32(d->hooks[i].addr) != want) return 0;
	}
	return 1;
}

static int help_in(const struct mh_desc *d)  { return words_are(d, first_help(d), d->nhooks, 1); }
static int help_out(const struct mh_desc *d) { return words_are(d, first_help(d), d->nhooks, 0); }
static int rest_in(const struct mh_desc *d)  { return words_are(d, 0, first_help(d), 1); }
static int rest_out(const struct mh_desc *d) { return words_are(d, 0, first_help(d), 0); }

/* The game in ordinary play, as the renderer's desc reads it: mode 0, nothing queued, no screen of the menu's. */
static void playing(const struct mh_desc *d)
{
	poke32(d->mode_addr, 0);
	poke32(d->pending_addr, d->pending_none);
	poke32(d->screen_addr, 0);
}

static u8 info_flags(void)
{
	u8 info[SESSION_INFO_SIZE];
	session_info_copy(info, sizeof(info));
	return info[24];
}

/* Whether `a` is one of the renderer's hook sites. */
static int is_hook(const struct mh_desc *d, u32 a)
{
	u8 i;
	for (i = 0; i < d->nhooks; i++) {
		if (d->hooks[i].addr == a) return 1;
	}
	return 0;
}

/*
 * Since the write log was reset, of the renderer's hook sites: the Help row's
 * words and no other, each once, in the table's order (`forward`) or the
 * reverse. A Help word another mod holds (`skip`, or -1) is never written.
 */
static int wrote_help_only(const struct mh_desc *d, int forward, int skip)
{
	u32 n = host_write_log_count(), k, a = 0, l = 0;
	int want = forward ? first_help(d) : (int)d->nhooks - 1;
	int step = forward ? 1 : -1;

	for (k = 0; k < n; k++) {
		host_write_log_at(k, &a, &l);
		if (!is_hook(d, a)) continue;
		if (want == skip) want += step;
		if (want < first_help(d) || want >= (int)d->nhooks) return 0;
		if (a != d->hooks[want].addr || l != 4) return 0;
		want += step;
	}
	if (want == skip) want += step;
	return forward ? want == (int)d->nhooks : want == first_help(d) - 1;
}

/* Waits out the session's install in play; answers whether the menu serves. */
static int installed(const struct mh_desc *d)
{
	int i;

	playing(d);
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) tick(1);
	tick(2);
	return menu_state() == MENU_READY;
}

/* ------------------------------------------------------------ every game */

static void help_switch(u8 game, const char *title, const char *name, u8 nhelp)
{
	const struct mh_desc *d = mh_desc_for_game(game);
	char what[160];
	u32 mb;

	if (d == NULL) {
		check(0, "the game has a renderer");
		return;
	}
	snprintf(what, sizeof(what), "%s: the Help row is the table's last %u words", name, (unsigned)nhelp);
	check(d->nhelp == nhelp && d->nhooks > nhelp, what);

	check(boot(title), "the game boots");
	check(installed(d) && rest_in(d) && help_in(d) && (info_flags() & SESSION_FLAG_MENU_HELP_OFF) == 0,
	      "menu.help never written: the renderer goes in with the Help row's words, flags bit6 clear");
	mb = menu_mailbox();

	/* 0, in play: those words back, the rest kept, the menu serving. */
	host_write_log_reset();
	check(config_set_menu_help(0) == ST_OK, "menu.help = 0");
	tick(8);
	check(help_out(d) && rest_in(d) && menu_state() == MENU_READY,
	      "within a poll the Help row is the game's own again, every other word the renderer's");
	check(wrote_help_only(d, 0, -1), "and nothing else was written, the Help words last in first out");
	check((info_flags() & SESSION_FLAG_MENU_HELP_OFF) != 0, "flags bit6 says so");

	/* 1 again, with the game somewhere the rows may be read: it waits. */
	poke32(d->mode_addr, d->mode_paused);
	config_set_menu_help(1);
	tick(30);
	check(help_out(d), "menu.help = 1 in the pause: nothing yet");
	poke32(d->mode_addr, 0);
	poke32(d->pending_addr, d->pending_none ^ 0x10u);
	tick(30);
	check(help_out(d), "back in play with a mode change queued: nothing yet");
	poke32(d->pending_addr, d->pending_none);
	poke32(d->screen_addr, d->screen_id);
	tick(30);
	check(help_out(d), "nothing queued, but a screen of the renderer's up: nothing yet");
	poke32(d->screen_addr, 0);
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_OPEN);
	tick(30);
	check(help_out(d), "the menu open (from its combo): nothing yet");
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_PICKER);
	tick(30);
	check(help_out(d), "the menu's planet picker up: nothing yet");
	host_write_log_reset();
	poke8(mb + MBOX_OFF_HSTATE, MBOX_HSTATE_CLOSED);
	tick(8);
	check(help_in(d) && rest_in(d), "all of it over, ordinary play: the Help row is the menu's again");
	check(wrote_help_only(d, 1, -1), "those words alone, in the table's order");
	check((info_flags() & SESSION_FLAG_MENU_HELP_OFF) == 0, "and flags bit6 is clear again");

	/* At 0 the combo still opens the menu. */
	config_set_menu_help(0);
	tick(8);
	poke8(mb + MBOX_OFF_REQUEST, 0);
	press(COMBO_MENU_DEFAULT);
	check(help_out(d) && peek8(mb + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "at 0 the menu combo still asks the renderer to open");
	poke8(mb + MBOX_OFF_REQUEST, 0);
	config_set_menu_help(1);
	tick(8);
	check(help_in(d), "and 1 puts the Help row back for the next game");
	check(quit(), "quit");
}

/* ------------------------------------------- Deadlocked, the rest of it */

static void help_switch_dl(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC4);
	u32 label, label_orig, n0, k, a = 0, l = 0;
	int i, wrote;

	if (d == NULL) return;
	label = d->hooks[first_help(d)].addr;
	label_orig = d->hooks[first_help(d)].original;

	group("menu.help: Deadlocked, menu.enabled, the install and the combo");

	/* menu.enabled = 0 with the Help row already the game's: every other word back too. */
	check(boot("NPEA00423") && installed(d), "Deadlocked serves");
	config_set_menu_help(0);
	tick(8);
	config_set_u32("menu.enabled", 0);
	tick(8);
	check(menu_state() == MENU_IDLE && rest_out(d) && help_out(d),
	      "menu.enabled = 0 at menu.help = 0: every word the game's");
	host_write_log_reset();
	config_set_u32("menu.enabled", 1);
	tick(4);
	wrote = 0;
	for (k = 0; k < host_write_log_count(); k++) {
		host_write_log_at(k, &a, &l);
		for (i = first_help(d); i < (int)d->nhooks; i++) {
			if (a == d->hooks[i].addr) wrote = 1;
		}
	}
	check(menu_state() == MENU_READY && rest_in(d) && help_out(d) && !wrote,
	      "menu.enabled = 1 at menu.help = 0: the renderer goes back in and no Help word is written");

	/* A renderer taken over with the Help words in, at menu.help = 0: they come out at the safe tick. */
	for (i = first_help(d); i < (int)d->nhooks; i++) poke32(d->hooks[i].addr, d->hooks[i].value);
	menu_enter(session_game());
	check(menu_state() == MENU_IDLE, "a module loaded again into the game starts idle");
	check(installed(d) && rest_in(d) && help_out(d),
	      "it takes the renderer over and puts the Help row back to the game's, as menu.help says");
	check(quit(), "quit Deadlocked");

	/* Another mod's word at a Help site: at 0 the install goes ahead and leaves it. */
	host_boot("NPEA00423");
	poke32(label, 0x48000001u);
	for (i = 0; i < 4000 && session_state() != SESSION_INGAME; i++) tick(1);
	check(installed(d) && rest_in(d) && peek32(label) == 0x48000001u &&
	      words_are(d, first_help(d) + 1, d->nhooks, 0),
	      "menu.help = 0 and another mod at the Help row's label call: installed, that word left alone");
	n0 = host_notify_count();
	host_write_log_reset();
	config_set_menu_help(1);
	tick(30);
	wrote = 0;
	for (k = 0; k < host_write_log_count(); k++) {
		host_write_log_at(k, &a, &l);
		if (is_hook(d, a)) wrote = 1;
	}
	check(peek32(label) == 0x48000001u && words_are(d, first_help(d) + 1, d->nhooks, 0) &&
	      !wrote && host_notify_count() == n0,
	      "menu.help = 1 then: neither Help word goes in while the mod holds one of them");
	poke32(label, label_orig);
	tick(4 * 120);
	check(help_out(d), "and nothing is tried again before ten seconds are up");
	tick(7 * 120);
	check(help_in(d) && rest_in(d), "the mod gone, ten seconds on the Help row is the menu's");
	check(quit(), "quit Deadlocked");

	/* A combo press installs at once: the Help row follows menu.help either way. */
	menu_set_install_delay_us(60000000ull);
	config_set_menu_help(0);
	check(boot("NPEA00423"), "Deadlocked boots, its install not due for a minute");
	playing(d);
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_READY && rest_in(d) && help_out(d) &&
	      peek8(d->mailbox + MBOX_OFF_REQUEST) == MBOX_REQ_OPEN,
	      "menu.help = 0: the combo installs everything but the Help row and asks the renderer to open");
	check(quit(), "quit Deadlocked");

	config_set_menu_help(1);
	check(boot("NPEA00423"), "Deadlocked boots again");
	playing(d);
	poke32(d->mode_addr, d->mode_paused);
	press(COMBO_MENU_DEFAULT);
	check(menu_state() == MENU_READY && rest_in(d) && help_out(d),
	      "menu.help = 1, the combo pressed in the game's pause: the renderer goes in, the Help row waits");
	tick(60);
	check(help_out(d), "for as long as the pause lasts");
	poke32(d->mode_addr, 0);
	tick(8);
	check(help_in(d), "and goes in with the first tick of play");
	menu_set_install_delay_us(3000000ull);
	check(quit(), "quit Deadlocked");

	/* RPCS3: nothing, whatever menu.help says. */
	host_set_can_patch_code(0);
	config_set_menu_help(0);
	check(boot("NPEA00423"), "Deadlocked boots under RPCS3");
	playing(d);
	tick(4 * 120);
	config_set_menu_help(1);
	tick(8);
	check(menu_state() == MENU_OFF && rest_out(d) && help_out(d), "no menu and no Help word under RPCS3");
	check(quit(), "quit");
	host_set_can_patch_code(1);
}

void test_menu_help(void)
{
	group("menu.help: the default");
	check(config_menu_help() == 1, "a config.txt that never mentions menu.help takes the Help row");
	check(config_set_menu_help(2) == ST_BAD_ARG, "menu.help is 0 or 1, nothing else");
	check(OP_MENU_HELP == 0x0092 && SESSION_FLAG_MENU_HELP_OFF == 0x40, "MENU_HELP at 0x0092, flags bit6");

	group("menu.help: Deadlocked");
	help_switch(GAME_RAC4, "NPEA00423", "Deadlocked", 2);
	group("menu.help: Up Your Arsenal");
	help_switch(GAME_RAC3, "NPEA00387", "UYA", 6);
	group("menu.help: Going Commando");
	help_switch(GAME_RAC2, "NPEA00386", "RaC2", 2);
	group("menu.help: Ratchet & Clank");
	help_switch(GAME_RAC1, "NPEA00385", "RaC1", 2);

	help_switch_dl();
	config_set_menu_help(1);
}
