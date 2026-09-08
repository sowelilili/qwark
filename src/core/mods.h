/*
 * Mods on the console: /dev_hdd0/qwark/mods/<TITLEID>/<dirname>/.
 *
 * patch.txt is parsed exactly the way racman's ModLoaderForm does it, so the
 * 35 shipped mods load unchanged:
 *
 *   #- key: value      metadata (name, version, author, description, depends)
 *   # ...              comment
 *   0xADDR: 0xVALUE    one 4-byte word, reverted on unload
 *   0xADDR: file.bin   a code cave, written in 64 KB chunks and never restored
 *   automation: x.lua  a Lua automation; the mod is flagged needs_lua and the
 *                      line is skipped
 *
 * Every mod gets its own patch_def at parse time, with its own originals buffer,
 * so patch_apply's "capture the originals once" rule covers mods too.
 */
#ifndef QWARK_MODS_H
#define QWARK_MODS_H

#include "proto.h"
#include "mem.h"

#define MOD_DESC_MAX    256
#define MOD_DEPENDS_MAX 128

struct mod_entry {
	char dirname[32];
	char name[32];
	char version[16];
	char author[32];
	char description[MOD_DESC_MAX];
	char depends[MOD_DEPENDS_MAX];

	u32  hash;                 /* qwark.sum, 0 when absent */
	u8   index;
	u8   flags;                /* MOD_FLAG_* */
	u8   used;

	struct patch_def def;
	u16  cave_first;
	u16  ncaves;
};

/* Points the scanner at a title and rescans. NULL or "" clears the table. */
void mods_set_title(const char *title);

int  mods_rescan(void);

u32  mods_count(void);
const struct mod_entry *mods_at(u32 index);
int  mods_find(const char *dirname);

int  mods_load(const char *dirname);
int  mods_unload(const char *dirname);
int  mods_set_auto(const char *dirname, int on);
int  mods_info(const char *dirname, char *out, u32 cap, u32 *len);

u32  mods_loaded_mask(void);
u32  mods_auto_mask(void);

/* Drops the loaded flags without writing anything (a game reboot did it for us). */
void mods_forget_loaded(void);

/* Loads every mod whose index bit is set, dependencies first. */
void mods_apply_mask(u32 mask);

/* Encodes the MOD_LIST reply. */
int  mods_list_encode(u8 *out, u32 cap, u32 *len);

#endif /* QWARK_MODS_H */
