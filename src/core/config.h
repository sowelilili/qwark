/*
 * /dev_hdd0/qwark/config.txt and /dev_hdd0/qwark/positions/<game>.txt (rac1..rac4).
 *
 * Both are plain "key = value" text so a user can edit them by hand. Config is
 * written on every change and re-read on CONFIG_RELOAD. Since build 56 every
 * write goes to "<file>.new" first, which then takes the file's place, and a
 * read that finds only the ".new" (a write cut off between the two) takes it.
 *
 * Revision 1.24, build 56: settings profiles. A profile is a folder,
 * /dev_hdd0/qwark/profiles/<folder>/, with a config.txt of its own and a
 * positions/ folder laid out like the console's. Today a profile is a console
 * user's, <folder> "user-00000001" after the XMB's user number; a later
 * revision adds named ones ("name-<slug>") beside them, which is why nothing
 * here is about users but the resolver. The console's switch is config.txt's
 * `profiles.enabled`, 0 out of the box: off, nothing reads a profile.
 *
 * On, the signed-in user's profile is the active one, and every key falls on
 * one side (config_key_scope):
 *   - per user, read from and written to the active profile only: selected.*,
 *     manip.*, menu.help, the toggles' and the mods' auto flags
 *     (<TITLEID>.auto.<slug>, <TITLEID>.mod.<dirname>.auto), profile.*, and
 *     the position files;
 *   - combos, per combo: the seven masks are the console's, and the active
 *     profile's key of the same name, when it holds one, is that combo's
 *     override and wins for that combo alone (config_set_combo_override);
 *   - the console's, always config.txt's: combo.enabled, profiles.enabled,
 *     menu.enabled, log, savefile_helper and every key not named above.
 * A profile that has never been set up has no folder: its per-user keys read
 * as unset, so as their defaults, and its first write makes it (empty). The
 * only copy is the one PROFILE_ENABLE asks for as the switch goes on.
 */
#ifndef QWARK_CONFIG_H
#define QWARK_CONFIG_H

#include "proto.h"

#define QWARK_ROOT      "/dev_hdd0/qwark"
#define QWARK_CONFIG    "/dev_hdd0/qwark/config.txt"
#define QWARK_POSDIR    "/dev_hdd0/qwark/positions"
#define QWARK_MODSDIR   "/dev_hdd0/qwark/mods"
/*
 * Revision 1.10. The savefile library, laid out like the mod library:
 * <QWARK_SAVEDIR>/<TITLEID>/<category>/<name>.sav, with the CRC of each file
 * beside it as <name>.sav.sum. See src/core/savefile.h.
 */
#define QWARK_SAVEDIR   "/dev_hdd0/qwark/savefiles"
/* Revision 1.24. Settings profiles: <QWARK_PROFILEDIR>/<folder>/config.txt and positions/. */
#define QWARK_PROFILEDIR "/dev_hdd0/qwark/profiles"

/*
 * A key is at most "<title>.auto.<slug>": nine characters of title id, six of
 * separator and the 47 a slug can be (see qslug and feature_key), so 64 is
 * exactly the bound rather than a guess.
 *
 * A value is a number. Every value qwark writes is one, decimal or "0x" and
 * eight hex digits, which is eleven characters at the widest, and every value it
 * reads goes through qparse_u32. 32 was 80 until build 35; anything longer in a
 * hand-edited file is cut at the same place it was always cut, and a value that
 * does not parse falls back the same way.
 */
#define CONFIG_KEY_MAX  64
#define CONFIG_VAL_MAX  32

/*
 * What one config.txt holds. It was 128 until build 37.
 *
 * What a title actually writes: the six combo masks, the combo switch, the
 * selected slot, the selected planet and its flags, and `log` and
 * `savefile_helper` - eleven keys - plus one key per feature auto flag and one
 * per mod auto flag, and those two are only written when a user ticks the box.
 * The shipped mod library's busiest title has five mods, and a user who
 * auto-flagged a dozen features in each of the four games is still inside 64.
 * Revision 1.21 adds at most three manip keys per game played, written only
 * when the user changes a manip option or picks a category. Build 53 keeps the
 * selected planet and its flags per game, two keys per game played and eight
 * at most, and a file from before then keeps the console's old pair beside
 * them: the four games' manips and selections together are twenty of the 64.
 * Revision 1.23 adds two console-wide keys, the seventh combo mask and
 * `menu.help`, each written only once a client sets it.
 */
#define CONFIG_MAX_ENTRIES 64

/*
 * config.txt is read whole, and config_save writes one line per entry, so the
 * file qwark itself can produce is bounded: CONFIG_MAX_ENTRIES lines of a key, a
 * " = ", a value and a newline. This is that bound with the 8 bytes config_save
 * already budgets per line, so a config qwark wrote always reads back. A file
 * larger than this - a hand-edited one - makes qread_file answer ST_FULL and
 * config_load hand that back without applying any of it, exactly as it did when
 * this was a flat 16384.
 */
#define CONFIG_TEXT_MAX (CONFIG_MAX_ENTRIES * (CONFIG_KEY_MAX + CONFIG_VAL_MAX + 8) + 1)

/* Creates the directory tree and reads config.txt. */
int  config_init(void);
int  config_load(void);
int  config_save(void);

const char *config_get(const char *key);
int  config_set(const char *key, const char *value);
u32  config_get_u32(const char *key, u32 fallback);

/* Bumped on every change, so hot readers can cache. */
u32  config_version(void);
int  config_set_u32(const char *key, u32 value);

/* Combos, indexed by COMBO_* action. Mask 0 disables. Revision 1.23: seven of them. */
u32  config_combo(u8 action);
int  config_set_combo(u8 action, u32 mask);

/*
 * Revision 1.12, COMBO_ENABLE. The one switch under all five of them: 0 holds
 * every combo off whatever its mask says. It is `combo.enabled`, console-wide
 * like the masks beside it rather than per title, and 1 for a config.txt that
 * has never mentioned it. Anything but 0 or 1 is ST_BAD_ARG.
 */
int  config_combo_enabled(void);
int  config_set_combo_enabled(u8 on);

/*
 * Revision 1.22, the console menu's switch: `menu.enabled`, 1 when the file has
 * never mentioned it, 0 turns the whole menu off (src/core/menu.h).
 */
int  config_menu_enabled(void);

/*
 * Revision 1.23, MENU_HELP: `menu.help`, 1 when the file has never mentioned
 * it. 1 the menu's Trainer row takes the game's Help row, 0 the Help row is
 * the game's own and the rest of the menu stays in. Anything but 0 or 1 is
 * ST_BAD_ARG.
 */
int  config_menu_help(void);
int  config_set_menu_help(u8 on);

u8   config_selected_slot(void);
void config_set_selected_slot(u8 slot);

/*
 * Build 53, the planet loader's selection of one game: the planet and the
 * PlanetFlags bits, "selected.<game>.planet" and "selected.<game>.planet_flags",
 * where <game> is the manips' and the position files' key, rac1 .. rac4. A
 * planet id means something only inside its own game, and so do the boxes
 * beside it.
 *
 * Until build 53 both were one pair for the console, "selected.planet" and
 * "selected.planet_flags". Those two are never written again, but a game whose
 * own key the file does not hold yet answers the old one, so a selection made
 * before the split is not lost; the first PLANET_SELECT in the game writes its
 * own pair, which wins from then on, and leaves the old pair alone for the
 * games that have none yet. A NULL `game` answers 0, and the setter refuses it
 * with ST_BAD_ARG.
 */
u8   config_selected_planet(const char *game);
u8   config_selected_planet_flags(const char *game);
int  config_set_selected_planet(const char *game, u8 planet, u8 flags);

/* Per-game auto flags. `label` is slugged, `dirname` is used verbatim. */
int  config_feature_auto(const char *title, const char *label);
/*
 * The same, but for a feature whose auto flag ships on: a config file that has
 * never mentioned this feature answers `fallback`, and a user who wrote a 0
 * still wins.
 */
int  config_feature_auto_default(const char *title, const char *label, int fallback);
/*
 * Build 53: the same for a toggle that was relabelled (game.h, struct
 * feature_former). The flag under `label` answers whenever the file holds one;
 * until then the flag under `former` does, and then `fallback`. NULL `former`
 * is config_feature_auto_default. Nothing is written: the first
 * FEATURE_SET_AUTO writes the new key, which wins from then on.
 */
int  config_feature_auto_former(const char *title, const char *label, const char *former,
                                int fallback);
void config_set_feature_auto(const char *title, const char *label, int on);
int  config_mod_auto(const char *title, const char *dirname);
void config_set_mod_auto(const char *title, const char *dirname, int on);

/*
 * Revision 1.21, the manip options of one game: "manip.<game>.<field>", where
 * <game> is the game's own key (rac1 .. rac4, not the title id, so the disc
 * trilogy keeps three sets) and <field> is auto_reset, remember or category.
 * A key the file has never held answers `fallback`.
 */
u32  config_manip(const char *game, const char *field, u32 fallback);
int  config_set_manip(const char *game, const char *field, u32 value);

/* -------------------------------------------------------------- profiles */

/* Revision 1.24. Which side a key is on (the comment at the top). */
#define CONFIG_SCOPE_CONSOLE 0
#define CONFIG_SCOPE_USER    1
#define CONFIG_SCOPE_COMBO   2
int  config_key_scope(const char *key);

/* "user-00000001" and its NUL; "name-<slug>" will have to fit too. */
#define PROFILE_FOLDER_MAX  24
/*
 * PROFILE_INFO's name field, NUL included: a named profile's name, which a
 * later revision brings. Empty for a console user's profile.
 */
#define PROFILE_NAME_MAX    32

/* PROFILE_ENABLE's setup byte. */
#define PROFILE_SETUP_NONE  0
#define PROFILE_SETUP_COPY  1   /* the console's per-user keys and position files */
#define PROFILE_SETUP_FRESH 2   /* empty: every per-user key at its default */

/* `profiles.enabled`, the console's switch: 0 when the file has never named it. */
int  config_profiles_enabled(void);
int  config_set_profiles_enabled(u8 on);

/*
 * The resolver. Makes the profile that should be active the active one: the
 * user's when the switch is on, the platform has users and `user` is a real
 * user number (1..99999999), none otherwise. Before a user's profile goes
 * active, `setup` COPY or FRESH makes it when it has never been made; a profile
 * that exists is never touched. Switching saves the position slots to the
 * file they came from and reads the new profile's config and slots, and bumps
 * the config version and the profile sequence. Answers 1 when the active
 * profile changed (or was made), 0 when it was already the one. Takes the core
 * text buffer, so the caller holds the core lock.
 *
 * Who calls it: the session at the XMB once a second with plat_user_id(), and
 * never in a game (session.c); PROFILE_ENABLE, at once, in any state.
 */
int  config_profile_resolve(u32 user, u8 setup);

int  config_profile_active(void);          /* per-user keys come from a profile now */
u32  config_profile_user(void);            /* the active profile's user, 0 none */
u8   config_profile_seq(void);             /* SessionInfo byte 30 */
/* SessionInfo byte 29, flags2: its SESSION_PROFILE_* bits (proto.h). */
u8   config_profile_flags(void);

/* "user-00000001" for user 1; "" for a user number that is no user. */
void config_profile_folder_for(u32 user, char *out, u32 cap);
/* Whether the user's profile has been made: its config.txt is there. */
int  config_profile_exists(u32 user);

/*
 * Revision 1.24, COMBO_OVERRIDE: the active profile's own mask for one combo.
 * `set` 1 stores `mask` (0 too: the combo is off for this user), 0 removes it
 * so the console's mask is in force again. ST_NOT_FOUND with no profile
 * active, ST_BAD_ARG for an action past the last. config_combo answers the
 * override where there is one.
 */
int  config_combo_overridden(u8 action);
int  config_set_combo_override(u8 action, int set, u32 mask);

/* ------------------------------------------------------------- positions */

/*
 * One title's slots are held in memory at a time. Switching titles writes the
 * old file out and reads the new one.
 */
int  pos_use_title(const char *title);
int  pos_store(u8 planet, u8 slot, const u8 *blob, u8 len);
int  pos_fetch(u8 planet, u8 slot, u8 *blob, u8 *len);
int  pos_clear(u8 planet, u8 slot);
int  pos_save_file(void);

#endif /* QWARK_CONFIG_H */
