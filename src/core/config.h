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

#define CONFIG_KEY_MAX  64
#define CONFIG_VAL_MAX  80

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
