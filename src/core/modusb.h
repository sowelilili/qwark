/*
 * Build 56, revision 1.24: mods on a USB drive (PROTOCOL.md 5.7.2).
 *
 * A console with a USB drive in it may carry mods there, in the same layout as
 * the console's own library, in either of two places on each drive:
 *
 *   /dev_usb00N/qwark/mods/<TITLEID>/<dir>/
 *   /dev_usb00N/mods/<TITLEID>/<dir>/
 *
 * for N in 0, 1, 2, 3, 6 and 7, the mount points the console gives USB storage.
 * Such a mod is available, not installed: MOD_USB_LIST lists it, and it goes
 * into the console's library, /dev_hdd0/qwark/mods/<TITLEID>/<dir>/, only when
 * it is loaded (MOD_LOAD, or an auto-load or reapply of a mod that is installed)
 * or its auto-load is turned on (MOD_SET_AUTO). A mod both places have is
 * compared by its "#- version": a newer USB copy is taken the next time the mod
 * loads while it is not in the game, and the console's copy stays otherwise.
 *
 * Everything here runs under the core lock, on the tick thread (a load, the
 * auto-load at boot) or a network thread (MOD_USB_LIST, MOD_SET_AUTO), and uses
 * the shared text buffer and small stack buffers; there is no per-tick file
 * I/O. Nothing happens on a platform that refuses code patches, where a mod is
 * an RPCS3 patch: the table stays empty and MOD_USB_LIST is UNSUPPORTED.
 */
#ifndef QWARK_MODUSB_H
#define QWARK_MODUSB_H

#include "proto.h"

/* As many USB mods for one title as the console's own table holds. */
#define MODUSB_MAX          QWARK_MAX_MODS

/* The bounds on what one take copies: patch.txt, then each .bin it names. */
#define MODUSB_FILE_MAX     (256u * 1024u)
#define MODUSB_TOTAL_MAX    (1024u * 1024u)
#define MODUSB_MAX_BINS     32u

/* A scan answers for this long before an op asking for the list looks again. */
#define MODUSB_FRESH_US     1000000u

/* Where the copy is: the `root` of an entry. */
#define MODUSB_ROOT_QWARK   0      /* /dev_usb00N/qwark/mods */
#define MODUSB_ROOT_MODS    1      /* /dev_usb00N/mods */

/* What the USB copy is to the console's copy: a row's `state`. */
#define MODUSB_AVAILABLE    0      /* only the USB drive has it */
#define MODUSB_SAME         1      /* the console has this copy */
#define MODUSB_NEWER        2      /* an update: taken at the mod's next load */
#define MODUSB_OLDER        3      /* the console's is newer, and stays */
#define MODUSB_CONFLICT     4      /* the same version or none to compare, other contents: the console's stays */
#define MODUSB_BROKEN       5      /* it cannot be taken; `problem` says why */

/* A row's `flags`. */
#define MODUSB_FLAG_WAITING 0x01   /* NEWER while the console's copy is loaded: it waits for the next load */
#define MODUSB_FLAG_ROOT_MODS 0x02 /* under /dev_usb00N/mods rather than /dev_usb00N/qwark/mods */

/* A BROKEN row's `problem`. */
#define MODUSB_PROBLEM_NONE    0
#define MODUSB_PROBLEM_PATCH   1   /* patch.txt unreadable, or bigger than qwark parses */
#define MODUSB_PROBLEM_MISSING 2   /* a .bin patch.txt names is not there */
#define MODUSB_PROBLEM_NAME    3   /* a .bin name qwark will not copy: "..", absolute, '\', ':' or too long */
#define MODUSB_PROBLEM_SIZE    4   /* a file over 256 KB, or the mod over 1 MB */
#define MODUSB_PROBLEM_COUNT   5   /* more than 32 .bin files */
#define MODUSB_PROBLEM_READ    6   /* a read failed: the drive went away, or a bad sector */

#define MODUSB_WIRE_SIZE    120

struct modusb_entry {
	char dirname[32];
	char name[32];
	char version[16];
	char author[32];
	u32  hash;        /* CRC-32 of the USB copy, qwark.sum's rule */
	u32  hdd_hash;    /* the console copy's, worked out when it has no qwark.sum; else 0 */
	u8   port;        /* N of /dev_usb00N */
	u8   root;        /* MODUSB_ROOT_* */
	u8   problem;     /* MODUSB_PROBLEM_* */
	u8   pad;
};

/* 0 where code cannot be patched (RPCS3): no scan, no take, no list. */
int  modusb_enabled(void);

/*
 * Reads the USB drives for the title mods.c is on. With `force` 0 it does
 * nothing while the last scan of that title is under MODUSB_FRESH_US old.
 * A boot and MOD_RESCAN force one; MOD_USB_LIST and a load do not.
 */
void modusb_scan(int force);

u32  modusb_count(void);
const struct modusb_entry *modusb_at(u32 index);
int  modusb_find(const char *dirname);

/* MODUSB_AVAILABLE..BROKEN against the console's table now, and the row flags. */
u8   modusb_state(const struct modusb_entry *u, u8 *flags);

/* MOD_USB_LIST: `u8 n, UsbMod[n]`, after a scan if the last is stale. */
int  modusb_list_encode(u8 *out, u32 cap, u32 *len);

/*
 * Before MOD_LOAD of `dirname` (mods_load, which every load goes through):
 * takes the mod from USB when only USB has it, and a newer USB copy of it or of
 * anything it depends on that is not loaded, and a dependency only USB has,
 * rescanning the console's table after. A take that fails for a mod the console
 * has leaves the console's copy, logged; for one it does not have it is the
 * load's answer. Outside a game it does nothing, as the load is refused anyway.
 */
int  modusb_prepare(const char *dirname);

/* MOD_SET_AUTO on for a mod only USB has: copies it into the library and rescans. */
int  modusb_install(const char *dirname);

/*
 * The version rule: up to four numbers separated by dots, an optional leading
 * 'v', nothing else. 1 with the order in *cmp (-1, 0, 1) when both parse, 0
 * when either does not, which is "no version to compare".
 */
int  modusb_version_cmp(const char *a, const char *b, int *cmp);

#endif /* QWARK_MODUSB_H */
