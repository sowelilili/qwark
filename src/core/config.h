/*
 * /dev_hdd0/qwark/config.txt and /dev_hdd0/qwark/positions/<game>.txt (rac1..rac4).
 *
 * Both are plain "key = value" text so a user can edit them by hand. Config is
 * written on every change and re-read on CONFIG_RELOAD.
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
 * What a title actually writes: the five combo masks, the combo switch, the
 * selected slot, the selected planet and its flags, and `log`, `trace_ops` and
 * `savefile_helper` - twelve keys - plus one key per feature auto flag and one
 * per mod auto flag, and those two are only written when a user ticks the box.
 * The shipped mod library's busiest title has five mods, and a user who
 * auto-flagged a dozen features in each of the four games is still inside 64.
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

/* Combos, indexed by COMBO_* action. Mask 0 disables. */
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

u8   config_selected_slot(void);
u8   config_selected_planet(void);
u8   config_selected_planet_flags(void);
void config_set_selected_slot(u8 slot);
void config_set_selected_planet(u8 planet, u8 flags);

/* Per-game auto flags. `label` is slugged, `dirname` is used verbatim. */
int  config_feature_auto(const char *title, const char *label);
/*
 * The same, but for a feature whose auto flag ships on: a config file that has
 * never mentioned this feature answers `fallback`, and a user who wrote a 0
 * still wins.
 */
int  config_feature_auto_default(const char *title, const char *label, int fallback);
void config_set_feature_auto(const char *title, const char *label, int on);
int  config_mod_auto(const char *title, const char *dirname);
void config_set_mod_auto(const char *title, const char *dirname, int on);

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
