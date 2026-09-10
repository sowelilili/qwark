/*
 * Ratchet & Clank 2, Going Commando (NPEA00386): the savefile helper's address
 * table, and the template the other three are read against.
 *
 * From https://github.com/king-dedede1/rc2-save, src/patch1.c and
 * src/rc2-save.h. The cave is racman's, from mods/NPEA00386/rc2-save/patch.txt:
 * that repo builds at 0x10CD11C and the mod racman shipped was placed at
 * 0x22A368, which is the address a console has actually run.
 *
 * The rack.bin racman ships is a later build of the same helper that also wrote
 * /dev_hdd0/game/NPEA00386/USRDIR/tempsave through sys_fs_open and friends. That
 * file is what qwark replaces: the save is parked in the aside buffer and qwark
 * streams it, so the helper makes no syscall and touches no file.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC2_H
#define QWARK_SF_RAC2_H

/* ------------------------------------------------------------------- caves */

#define SF_CAVE 0x0022A368

/*
 * The hook site, from the repo's own patch.txt: a `bla` into the helper, which
 * returns the way any other call does. Absolute, as it is upstream.
 */
#define SF_HOOK_ADDR     0x00B086B0
#define SF_HOOK_ABSOLUTE 1

/* -------------------------------------------------------- the request bytes */

#define SF_API_MOD      0x010CD71D
#define SF_API_LOAD     0x010CD71E
#define SF_API_SETASIDE 0x010CD71F
#define SF_API_I        0x010CD720

/* ------------------------------------------------------- the aside buffer */

#define SF_ASIDE_ADDR 0x01C00000
#define SF_ASIDE_SIZE 0x00200000

/* --------------------------------------------------------- game addresses */

/* savedata_buf is the word at +4 of the block savedata_info points at. */
#define SF_SAVEDATA_INFO  0x01410E50
#define SF_SAVEDATA_FIELD 0x00000004

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY       0x010D4FFC
#define SF_FN_PERFORM_LOAD 0x00083250

#endif /* QWARK_SF_RAC2_H */
