/*
 * Mods on the console: /dev_hdd0/qwark/mods/<TITLEID>/<dirname>/.
 *
 * patch.txt is parsed exactly the way racman's ModLoaderForm does it, so the
 * 35 shipped mods load unchanged:
 *
 *   #- key: value      metadata (name, version, author, description, depends)
 *   # ...              comment
 *   0xADDR: 0xVALUE    one 4-byte word, reverted on unload
 *   0xADDR: file.bin   a code cave, written in scratch-sized pieces and never
 *                      restored
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

/*
 * How much of a "#- description:" line MOD_INFO hands back. It is not kept per
 * mod any more: build 37 reads the line out of patch.txt when MOD_INFO asks for
 * it, because 256 bytes a mod times 32 mods was half of the mod table and a
 * client asks for one description at a time, when a user opens a mod.
 */
#define MOD_DESC_MAX    256
#define MOD_DEPENDS_MAX 128

/*
 * The shared word pool, bump allocated across every mod of one title, and the
 * largest patch def anything can hand patch_apply: a single mod may hold the
 * whole pool, so the two are the same number and mem.h owns it.
 *
 * It was 2048 until build 35. The shipped library's busiest title is Deadlocked
 * at 279 words over five mods, the largest single mod being dl-cs at 253, so 640
 * is better than twice what the library needs and two and a half times the
 * fixtures. A mod that does not fit is refused with ST_FULL and flagged
 * MOD_FLAG_PARSE_ERROR, exactly as it was before.
 */
#define MOD_WORD_POOL   PATCH_ORDER_WORDS

/*
 * Code caves, also pooled across a title. Four in the shipped library, and a
 * mod rarely has more than one; 32 is eight times the busiest title and costs
 * 68 bytes a slot.
 */
#define MOD_CAVE_POOL   32

/*
 * One patch.txt is read whole, so the text buffer has to hold a file that fills
 * the word pool on its own: MOD_WORD_POOL lines of "0xADDR: 0xVALUE" is about
 * 15 KB, which is why this stays at 16 KB while the pool shrank. A file bigger
 * than it fails to read and the mod is flagged MOD_FLAG_PARSE_ERROR, which is
 * what it has always done.
 */
#define MOD_TEXT_MAX    16384

/*
 * Build 49. Whether a mod is in the running game, on a platform that refuses
 * code patches: RPCS3, where a mod gets into the game as an RPCS3 patch the
 * client writes from MOD_PATCH and RPCS3 applies as it loads the executable.
 *
 *   NONE     not looked for: a console, no session, or a mod that cannot be in
 *            the game this way (a parse error, a Lua automation) or has nothing
 *            to put there (no words and no caves)
 *   PENDING  to be looked for this session; MOD_LIST says CHECKING
 *   ABSENT   looked for and not all there, or its cave file is gone
 *   PRESENT  every cave byte and every word reads back; MOD_LIST says LOADED
 *
 * A read that fails leaves a mod PENDING: an emulator that is not answering
 * says nothing about whether the patch went in.
 */
#define MOD_LOOK_NONE     0
#define MOD_LOOK_PENDING  1
#define MOD_LOOK_ABSENT   2
#define MOD_LOOK_PRESENT  3

struct mod_entry {
	char dirname[32];
	char name[32];
	char version[16];
	char author[32];
	/*
	 * No description here: MOD_INFO re-reads patch.txt for the one mod a user
	 * asked about. `depends` stays, because the loader walks it on the tick
	 * thread and a file read per level of a dependency chain is not a trade
	 * worth making for 4 KB.
	 */
	char depends[MOD_DEPENDS_MAX];

	u32  hash;                 /* qwark.sum, 0 when absent */
	u8   index;
	/*
	 * MOD_FLAG_* as parsed and loaded. PREVIOUS and CHECKING are never stored
	 * here: MOD_LIST works both out when it encodes a row, and so does LOADED on
	 * a platform that refuses code patches, from `look` below.
	 */
	u8   flags;
	u8   used;
	u8   look;                 /* MOD_LOOK_*, build 49 */

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

/*
 * Revision 1.16. Encodes the MOD_PATCH reply for MOD_LIST index `index`: the
 * mod as the words and bytes an RPCS3 patch writes. It reads the mod's files
 * and never the game, so it runs on a network thread, under the core lock.
 * The reply is capped at QWARK_MAX_PAYLOAD whatever `cap` says.
 */
int  mods_patch_encode(u8 index, u8 *out, u32 cap, u32 *len);

/*
 * Build 49, the presence look. `start` when a session reaches INGAME: on a
 * platform that refuses code patches every mod that can be looked for is
 * PENDING, and on a console it does nothing. `stop` when the session leaves
 * the game, forgetting every answer. `tick` looks a little further each tick,
 * on the tick thread while INGAME, and a rescan starts the look over.
 */
void mods_presence_start(void);
void mods_presence_stop(void);
void mods_presence_tick(void);

#endif /* QWARK_MODS_H */
