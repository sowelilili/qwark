/*
 * ui.pak (build 55): the console menu's renderers, on the console's HDD rather
 * than in the module, and since build 56 the savefile helpers too.
 *
 * Each of the four renderers is PowerPC code and data qwark writes into the
 * running game (src/games/menuhelper/). Build 54 carried all four inside the
 * module, 66 KB of it, for the whole life of the module in the VSH's memory,
 * when only the running game's is ever wanted and only for the moment it goes
 * in. They are in /dev_hdd0/qwark/ui.pak now, beside qwark's other files,
 * which src/games/menuhelper/pak.py writes out of the committed
 * menuhelper_bins.c and `make` puts beside qwark.sprx. qwark reads the running
 * game's part of it when the menu goes into the game, a scratch buffer at a
 * time, straight into the game's memory, and never holds the file.
 *
 * Build 56: the four savefile helpers (src/games/sfhelper/) moved in beside
 * them, 6 KB of the module's code segment that, like the renderers, only ever
 * goes into the running game. The module keeps each helper's numbers (its
 * request bytes and aside buffer, sfhelper_bins.h) and reads the caves and the
 * hook word out of the pak when the helper is first wanted in a session
 * (uipak_sf_entry, savefile.c). The emulator builds still link the bytes, for
 * SAVEFILE_PATCH: RPCS3 never reads the pak.
 *
 * A module accepts only the pak built with it: the header must name its build
 * (QWARK_BUILD) and carry its renderers' identity (MH_PAK_ID, the CRC-32 of the
 * pak's renderer entries, which build.sh writes into menuhelper_bins.h) and its
 * helpers' (SF_PAK_ID, the same of the helper entries, in sfhelper_bins.h).
 * Without one, nothing of the menu goes into a game and no savefile helper
 * either, the log says why, the console says so once, and SessionInfo flags
 * bit7 UI_PAK_BAD tells a client, which can write the right one. A save file
 * action answers BUSY with SAVEFILE_ERR_NO_PAK (savefile.h). Everything else
 * works as before.
 *
 * PROTOCOL.md section 9.10 has the file's layout; pak.py says the same.
 */
#ifndef QWARK_UIPAK_H
#define QWARK_UIPAK_H

#include "../plat/plat.h"
#include "buildmodes.h"
#include "../games/menuhelper_bins.h"
#include "../games/sfhelper_bins.h"

#define UIPAK_PATH          "/dev_hdd0/qwark/ui.pak"
/* What a client's update leaves for the next module: it takes it at its start. */
#define UIPAK_NEW_PATH      "/dev_hdd0/qwark/ui.pak.new"

#define UIPAK_MAGIC         0x5150414Bu    /* "QPAK" */
/* Build 56: format 2, with the helpers (format 1 was build 55's, renderers only). */
#define UIPAK_FORMAT        2u
#define UIPAK_HEADER_SIZE   32u
#define UIPAK_TOC_SIZE      32u
#define UIPAK_DESC_SIZE     40u
#define UIPAK_CAVE_SIZE     20u
#define UIPAK_HOOK_SIZE     12u
#define UIPAK_KIND_RENDERER 1u
#define UIPAK_KIND_SFHELPER 2u
/* What the reader has room for; pak.py and check.py hold the pak to it. */
#define UIPAK_MAX_ENTRIES   16u
#define UIPAK_MAX_CAVES     8u
#define UIPAK_MAX_HOOKS     32u
/* A pak is about 72 KB; anything past this is not one. */
#define UIPAK_MAX_BODY      0x100000u
/* Build 56: a helper's entry is read whole into the tick's scratch (pak.py's MAX_SF_ENTRY). */
#define UIPAK_MAX_SF_ENTRY  QSCRATCH_BYTES

/* What the last look at the pak found. */
#define UIPAK_UNCHECKED     0   /* not looked at yet, or never (RPCS3, which has no menu) */
#define UIPAK_OK            1
#define UIPAK_MISSING       2   /* no file, or one that will not open */
#define UIPAK_OTHER         3   /* a pak, but another build's or format, or other renderers or helpers */
#define UIPAK_DAMAGED       4   /* not a pak, cut short, or a CRC that does not hold */

/*
 * The tick thread, every tick at the XMB and in a game: looks at the pak at
 * the module's start, taking ui.pak.new first when it is this build's, and
 * again after uipak_touch. Nothing at all where code cannot be patched.
 */
void uipak_tick(void);

/*
 * Any thread: a file op wrote, renamed or deleted `path` (net.c). When it is
 * the pak or a folder above it, the next tick looks again.
 */
void uipak_touch(const char *path);
int  uipak_names(const char *path);

int  uipak_status(void);
/* 1 when the last look found no usable pak: SessionInfo flags bit7. */
int  uipak_bad(void);
/* Looks finished since the module started; the menu tries again after a new one. */
u32  uipak_looks(void);

#ifdef QWARK_TEST
void uipak_test_start(void);
#endif

/*
 * The seam menu.c reaches the renderers through, on the tick thread.
 *
 * mh_pak_desc: the game's renderer as struct mh_desc, out of the pak's header
 * and table of contents; its caves carry no bytes. NULL with *status UIPAK_OK
 * for a game the pak has no renderer for, and NULL with the reason for a pak
 * that is missing, damaged or another build's. The struct is the module's one
 * and stays valid until the next call.
 *
 * mh_pak_open: before any cave's bytes are wanted, opens the pak again, checks
 * its header is still this build's and the game's part of it whole (its CRC),
 * and keeps it open; UIPAK_OK or the reason. mh_pak_read then reads a cave's
 * bytes at an offset, ST_OK or ST_IO_ERROR, and mh_pak_close closes it. Both
 * of the first two use the tick's scratch buffer.
 */
const struct mh_desc *mh_pak_desc(u8 game_id, int *status);
int  mh_pak_open(void);
int  mh_pak_read(const struct mh_cave *c, u32 off, void *buf, u32 len);
void mh_pak_close(void);

/*
 * Build 56, savefile.c's seam, on the tick thread: the savefile helper entry
 * for `game_id`, whole, in the tick's scratch buffer at *entry, its length in
 * *len (at most UIPAK_MAX_SF_ENTRY), its CRC checked against the table of
 * contents and the header this build's. UIPAK_OK, or the reason: MISSING,
 * OTHER, or DAMAGED, which is also a pak of this build with no helper for the
 * game. The entry is the layout PROTOCOL.md 9.10 gives a renderer's; its
 * records are the caller's to check against what it expects. Nothing is held
 * open, and the bytes are good until the scratch is next used.
 */
int  uipak_sf_entry(u8 game_id, const u8 **entry, u32 *len);

#if QWARK_HAS_TOD
/*
 * Revision 1.29: Tools of Destruction's Lua helper, which a build
 * with ToD reads out of the pak too (pak.py --tod): its code, kind 3, laid
 * out as a savefile helper's entry (its words the mailbox, its size, the VM,
 * the lua_State's offset, the pause menu's pointer and offset, the map's
 * pointer), and the three Lua chunks the core sends, kinds 4 to 6, each the
 * bytes as sent. They are outside the header's identities, so a pak without
 * them (a build without ToD's) is what it was; a module with ToD takes a pak only when the table of
 * contents gives LH_PAK_ID (luahelper_bins.h), the CRC-32 of the Lua entries'
 * CRC-32s, each big-endian, in the table's order, and a pak without them, or
 * with another build's, is another build's pak (UIPAK_OTHER), refused whole
 * like one with other renderers. A module without ToD never looks at them.
 *
 * uipak_lh_open: the game's helper entry, its CRC checked over the whole
 * entry (through the scratch, a piece at a time: the entry is bigger than
 * the scratch), the file kept open and the descriptor and records (the
 * caves' and the hook words') read into the scratch at *rec; *len the
 * entry's length. UIPAK_OK or the reason, as uipak_sf_entry. uipak_lh_read
 * reads `len` bytes at `off` into the entry, ST_OK or ST_IO_ERROR, and
 * uipak_lh_close closes the file. Tick thread, inside one install.
 *
 * uipak_lh_chunk: a Lua chunk (kind 4, 5 or 6) of the game whole in the
 * scratch at *bytes, its CRC checked, at most UIPAK_MAX_LUA_CHUNK bytes;
 * UIPAK_OK or the reason. Nothing is held open.
 */
#include "../games/luahelper_bins.h"

#define UIPAK_KIND_LUAHELPER  3u
#define UIPAK_KIND_LUA_GLUE   4u    /* the pause menu's glue, lh_glue.lua as sent */
#define UIPAK_KIND_LUA_DIAG   5u    /* the look at the pause, lh_diag.lua */
#define UIPAK_KIND_LUA_GMAP   6u    /* the galactic map's glue, lh_gmap.lua */
/* A chunk goes whole into the scratch, and must fit a request (luamb.h). */
#define UIPAK_MAX_LUA_CHUNK   QSCRATCH_BYTES

int  uipak_lh_open(u8 game_id, const u8 **rec, u32 *len);
int  uipak_lh_read(u32 off, void *buf, u32 len);
void uipak_lh_close(void);
int  uipak_lh_chunk(u8 game_id, u8 kind, const u8 **bytes, u32 *len);
#endif

#endif /* QWARK_UIPAK_H */
