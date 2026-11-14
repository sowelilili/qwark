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
 *     manip.*, menu.help, run.warnings, the toggles' and the mods' auto flags
 *     (<TITLEID>.auto.<slug>, <TITLEID>.mod.<dirname>.auto), profile.*, and
 *     the position files;
 *   - combos, per combo: the seven masks are the console's, and the active
 *     profile's key of the same name, when it holds one, is that combo's
 *     override and wins for that combo alone (config_set_combo_override);
 *   - the console's, always config.txt's: combo.enabled, profiles.enabled,
 *     menu.enabled, log, savefile_helper, telemetry.hz (revision 1.32) and
 *     every key not named above.
 * A profile that has never been set up has no folder: its per-user keys read
 * as unset, so as their defaults, and its first write makes it (empty). The
 * only copy is the one PROFILE_ENABLE asks for as the switch goes on.
 *
 * A module built with QWARK_PREVIEW 0 (proto.h; build 56's release) leaves
 * the profiles out: every key is
 * config.txt's and the slots are positions/'s, as in 1.4.1, whatever
 * `profiles.enabled` says (the key is kept, like any other), and nothing
 * under QWARK_PROFILEDIR is read or written.
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
 * Build 56: what the key store holds, in bytes rather than in keys.
 *
 * Until build 56 the store was a table of CONFIG_MAX_ENTRIES (64) slots, each
 * a full CONFIG_KEY_MAX key and CONFIG_VAL_MAX value, and the settings profiles
 * had a second table like it. A console that had been used for a while held
 * more than 64 keys: the auto flags of every toggle and colour in four games,
 * a flag per mod, the manips and the selections. A file with more keys than
 * the table loaded the first 64 and the next write cut the rest out of it, a
 * new key was ST_FULL (PROFILE_ENABLE's `profiles.enabled` among them, which
 * is how build 56 found it), and a toggle's or a mod's auto flag was quietly
 * not kept.
 *
 * Now every key is packed into one pool, "<store><key>\0<value>\0", where
 * <store> says whose file it is (the console's config.txt or the active
 * profile's), so a key costs its own length and not the widest one's. While a
 * profile is active the console's per-user keys are not held at all: they stay
 * in config.txt, which every write of it carries over from the old file
 * (config.c, store_save), so the pool holds one set of per-user keys,
 * whichever is in use, and a COPY reads them out of the file rather than
 * doubling them.
 *
 * What qwark itself can write is bounded: the fixed keys (the combos and their
 * switch, the menu's, the selections and manips of four games, `log`,
 * `savefile_helper`, `profiles.enabled`), the auto flag of every toggle and
 * colour under every title id the four games answer to, and a mod library of
 * QWARK_MAX_MODS mods with the longest dirnames on each of the four games'
 * own titles. test/test_profiles.c builds exactly that file, from the games'
 * own tables: 231 keys, 220 of them per user, 10,416 bytes of pool; 12 KB
 * holds it with room to spare. The pool's last byte is always 0.
 *
 * A file the pool cannot hold all of (a hand-edited one) loads as much as
 * fits, and its store is then never written back: every write to it answers
 * ST_FULL rather than cut the file down to what was read.
 *
 * Build 57: a write is the old file with the store's keys put in (config.c,
 * store_save), never the store alone, so a line the store does not hold - a
 * comment, a key qwark does not know or cannot hold whole, or one a load did
 * not keep - goes back into the file as it was.
 */
#define CONFIG_POOL_BYTES 12288

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

/*
 * Revision 1.30, the run-start warnings (src/core/runwarn.h):
 * `run.warnings`, 1 when the file has never mentioned it, 0 the console says
 * nothing at a run start. RUN_WARN writes it; a per-user key, as menu.help
 * is. Anything but 0 or 1 is ST_BAD_ARG.
 */
int  config_run_warnings(void);
int  config_set_run_warnings(u8 on);

/*
 * Revision 1.32, TELEMETRY_RATE: `telemetry.hz`, how often the telemetry
 * packet goes out, 30 (the default, and what any other value in the file
 * reads as), 60 or 120. Console-wide. Setting anything else is ST_BAD_ARG.
 */
u8   config_telemetry_hz(void);
int  config_set_telemetry_hz(u8 hz);

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

/*
 * Per-game auto flags. `label` is slugged, `dirname` is used verbatim.
 *
 * Revision 1.24: a feature's key holds a number rather than a flag, which the
 * three below answer as it stands, 0 when unset: a toggle's is 1 or 0, and a
 * COLOR's is 0x80000000 with the FEATURE_SET value to re-apply in the low 25
 * bits (features.h), 0 when it is not re-applied.
 */
u32  config_feature_auto(const char *title, const char *label);
/*
 * The same, but for a feature whose auto flag ships on: a config file that has
 * never mentioned this feature answers `fallback`, and a user who wrote a 0
 * still wins.
 */
u32  config_feature_auto_default(const char *title, const char *label, int fallback);
/*
 * Build 53: the same for a toggle that was relabelled (game.h, struct
 * feature_former). The flag under `label` answers whenever the file holds one;
 * until then the flag under `former` does, and then `fallback`. NULL `former`
 * is config_feature_auto_default. Nothing is written: the first
 * FEATURE_SET_AUTO writes the new key, which wins from then on.
 */
u32  config_feature_auto_former(const char *title, const char *label, const char *former,
                                int fallback);
/* Build 56: both answer the write's status, ST_FULL with no room, as config_set does. */
int  config_set_feature_auto(const char *title, const char *label, u32 value);
int  config_mod_auto(const char *title, const char *dirname);
int  config_set_mod_auto(const char *title, const char *dirname, int on);

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

#if QWARK_PREVIEW

int  config_key_scope(const char *key);

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

/*
 * PROFILE_ENABLE (build 56): `profiles.enabled` written, then the resolver run
 * for `user` with `setup`, all or nothing. A config.txt with no room for the
 * switch answers ST_FULL and changes nothing. A setup that cannot finish (a
 * folder that cannot be made, a file that cannot be written) takes away the
 * profile it began, leaves config.txt as it was, the switch included, and
 * answers why. `*changed` is 1 when the session should read the auto flags
 * again (session_profile_changed). `on` (0 or 1) and `setup` are the
 * caller's to check (net.c); the caller holds the core lock.
 */
int  config_profiles_enable(u8 on, u8 setup, u32 user, int *changed);

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

#else /* !QWARK_PREVIEW */

/*
 * QWARK_PREVIEW 0: no profile is ever active, so SessionInfo's flags2 bits 1 to 3
 * and byte 30 are 0 and no COMBO_LIST row is overridden, as 1.4.1's pad
 * bytes were; net.c answers the three profile ops UNSUPPORTED.
 */
static __inline int config_profile_active(void)        { return 0; }
static __inline u8  config_profile_seq(void)           { return 0; }
static __inline u8  config_profile_flags(void)         { return 0; }
static __inline int config_combo_overridden(u8 action) { (void)action; return 0; }

#endif /* QWARK_PREVIEW */

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
