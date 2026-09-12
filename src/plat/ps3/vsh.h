#ifndef __VSH_H
#define __VSH_H

#include "common.h"

// We're not using the rest of this function pointer struct so I just removed everything except for the gameInfo thing we use lol.
typedef struct game_plugin_interface_t
{
    void *unk[8];
    int32_t (*gameInfo)(void *);
} game_plugin_interface;

// Gets interface from plugin, uint32_t plugin->GetInterface(uint32_t return from paf_F21655F3, uint32_t identifier)
extern uint32_t *plugin_GetInterface(uint32_t plugin, uint32_t id) __asm__("paf_23AFB290");

// Return the texture address by plugin name and texture name.
extern int LoadRCOTexture(int32_t *tex, uint32_t plugin, const char *name) __asm__("paf_3A8454FC");

// Finds a loaded plugin 
extern uint32_t View_Find(const char *name) __asm__("paf_F21655F3");

// Displays a notification in XMB, calls vshcommon_A20E43DB
extern int32_t vshtask_notify(int32_t arg, const char *msg) __asm__("vshtask_A02D46E7");

// Displays a notification in XMB
extern uint32_t NotifyWithTexture(int32_t, const char* eventName, int32_t, int32_t* texture, int32_t*, const char*, const char*, float, const wchar_t* text, int32_t, int32_t, int32_t) __asm__("vshcommon_A20E43DB");

// Get running mode flag, 0 = xmb, 1 = game, 2 = dvd/bd, 3 = psx/psp emu 
extern uint32_t GetCurrentRunningMode(void) __asm__("vshmain_EB757101");
#define IS_ON_XMB		(GetCurrentRunningMode() == 0)
#define IS_INGAME		(GetCurrentRunningMode() != 0)

// Returns game u32 process id
extern uint32_t GetGameProcessID(void) __asm__("vshmain_0624D3AE");

// Returns current user ID
typedef struct {
    int32_t (*unk[12])(void);
    int32_t (*GetCurrentUserNumber)(void);
} xusers_class;

extern xusers_class* xusers(void)__asm__("xsetting_CC56EB2D");

// Gets current game ID
game_plugin_interface * game_interface;

/*
 * This is the one place qwark calls into another VSH plugin, and it is called
 * exactly when the XMB is handing the machine over to a game, which is when
 * game_plugin is least likely to be a thing worth calling into.
 *
 * As it came from racman it took three answers on trust: that View_Find gave a
 * plugin, that plugin_GetInterface gave an interface, and that the interface
 * had the function. The second and third were never checked, so a moment where
 * either is not yet there was a jump through a null or stale pointer on the
 * tick thread, which on a console is a reboot. Consoles have been rebooting at
 * exactly this point. Every step is checked now, and a step that is not ready
 * means "no title yet" - the caller simply asks again.
 *
 * The other half of that fix is in the session: it lets the process id hold
 * still for a while before it asks any of this, so the question is put to the
 * XMB after the handover rather than in the middle of it.
 */
static __attribute__((unused)) int get_game_info(void) {
	uint32_t plugin;
	game_plugin_interface *iface;
	char _game_info[0x120];

	if(IS_ON_XMB) return 0;

	plugin = View_Find("game_plugin");
	if(!plugin) return 0;

	iface = (game_plugin_interface *)plugin_GetInterface(plugin, 1);
	if(iface == NULL || iface->gameInfo == NULL) return 0;

	game_interface = iface;

	memset(_game_info, 0, sizeof(_game_info));
	iface->gameInfo(_game_info);

	snprintf(_game_TitleID, 10, "%s", _game_info+0x04);
	snprintf(_game_Title,   63, "%s", _game_info+0x14);

	return 1;
}

#endif