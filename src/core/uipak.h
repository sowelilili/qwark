/*
 * ui.pak (build 55): the console menu's renderers, on the console's HDD rather
 * than in the module.
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
 * A module accepts only the pak built with it: the header must name its build
 * (QWARK_BUILD) and carry its renderers' identity (MH_PAK_ID, the CRC-32 of the
 * pak after its header, which build.sh writes into menuhelper_bins.h). Without
 * one, nothing of the menu goes into a game, the log says why, the console
 * says so once, and SessionInfo flags bit7 UI_PAK_BAD tells a client, which
 * can write the right one. Everything else works as before.
 *
 * PROTOCOL.md section 9.10 has the file's layout; pak.py says the same.
 */
#ifndef QWARK_UIPAK_H
#define QWARK_UIPAK_H

#include "../plat/plat.h"
#include "../games/menuhelper_bins.h"

#define UIPAK_PATH          "/dev_hdd0/qwark/ui.pak"
/* What a client's update leaves for the next module: it takes it at its start. */
#define UIPAK_NEW_PATH      "/dev_hdd0/qwark/ui.pak.new"

#define UIPAK_MAGIC         0x5150414Bu    /* "QPAK" */
#define UIPAK_FORMAT        1u
#define UIPAK_HEADER_SIZE   32u
#define UIPAK_TOC_SIZE      32u
#define UIPAK_DESC_SIZE     40u
#define UIPAK_CAVE_SIZE     20u
#define UIPAK_HOOK_SIZE     12u
#define UIPAK_KIND_RENDERER 1u
/* What the reader has room for; pak.py and check.py hold the pak to it. */
#define UIPAK_MAX_ENTRIES   8u
#define UIPAK_MAX_CAVES     8u
#define UIPAK_MAX_HOOKS     32u
/* A pak is about 66 KB; anything past this is not one. */
#define UIPAK_MAX_BODY      0x100000u

/* What the last look at the pak found. */
#define UIPAK_UNCHECKED     0   /* not looked at yet, or never (RPCS3, which has no menu) */
#define UIPAK_OK            1
#define UIPAK_MISSING       2   /* no file, or one that will not open */
#define UIPAK_OTHER         3   /* a pak, but another build's, or other renderers */
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

#endif /* QWARK_UIPAK_H */
