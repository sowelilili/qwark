//------------
/* Based on PHTNC's code to write VSH Notify notifications with icons */

#ifndef __VSH_NOTIFY_H
#define __VSH_NOTIFY_H

#include "vsh.h"

#include "common.h"
#include "types.h"

/*
 * qwark shows one icon, the game's, from plat_notify. The original took an icon
 * number and carried a table of 51 RCO texture names for it (18 from
 * system_plugin, the rest from explore_plugin, ICON_GAME being 33, "tex_game"),
 * and with show_msg beside it as a static in this header it was compiled into
 * main.c and plat_ps3.c both: twice the code, twice 204 bytes of pointers in the
 * RW segment and twice about 1 KB of names (SPEC-footprint.md, cut C13). Only
 * the one name is left, and main.c's messages, none of which asked for an icon,
 * go straight to vshtask_notify, which is where show_msg sent them.
 */
#define VSH_NOTIFY_GAME_PLUGIN  "explore_plugin"
#define VSH_NOTIFY_GAME_TEXTURE "tex_game"

static __attribute__((unused)) int32_t vshNotify_Game(const char *msg)
{
	if(IS_INGAME)
		return vshtask_notify(0, msg);

	uint32_t _plugin = View_Find(VSH_NOTIFY_GAME_PLUGIN);
	if (_plugin <= 0)
		return FAILED;

	int len = strlen(msg); if(len >= 200) len = 198;

	wchar_t message[len + 2];

	mbstowcs((wchar_t *)message, (const char *)msg, len + 1);  //size_t stdc_FCAC2E8E(wchar_t *dest, const char *src, size_t max)

	int teximg, dummy = 0;
	LoadRCOTexture(&teximg, _plugin, VSH_NOTIFY_GAME_TEXTURE);
	return NotifyWithTexture(0, VSH_NOTIFY_GAME_TEXTURE, 0, &teximg, &dummy, "", "", 0, message, 0, 0, 0);
}
//------------

#endif
