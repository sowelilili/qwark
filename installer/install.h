/*
 * What the installer does, as one call: the PS3 app (main.c) shows the dialogs
 * around it and the host tests (test/test_installer.c) run it over a fake HDD.
 *
 * It puts the module and the console menu's file where the PC client's
 * "Install to boot_plugins.txt" puts them, and edits the list the way that
 * install does (bootlist.h). Revision 1.26: the package carries the game
 * modules too, rac1.sprx to rac4.sprx and tod.sprx (qwark's PROTOCOL.md 9.11),
 * which go beside ui.pak: a test build's and, since build 57, a release's
 * (build 56's release carried none). It is written so that a failure at any point
 * leaves the console booting what it booted before:
 *
 *  1. It checks the package's own qwark.sprx and ui.pak, reads the list and
 *     plans the edit. Nothing is written yet.
 *  2. It writes every new file beside its final name as <name>.tmp and reads
 *     each one back: the module, the pak, a backup of the old list and the new
 *     list. A failure here deletes the .tmp files, and nothing has changed.
 *  3. It puts them in place one at a time, in an order where the list never
 *     names a module that is not there: the backup, the pak, the module, the
 *     list. Each move first renames the file it replaces to <name>.old, so a
 *     failure puts back everything moved so far.
 *  4. Once qwark is in, the library of mods and premade savefiles the package
 *     carries, if it carries one (below): only what the console does not have
 *     yet, each one whole or not at all, and a failure only a warning.
 *
 * So a power cut leaves either the old list or the new one, or at worst, in the
 * moment between two renames, no list at all, which boots with no plugins; a
 * second run then finds boot_plugins.txt.old and takes the list from it.
 *
 * Running it again is safe: a list that already starts with qwark is left as
 * it is, and the two files are replaced by the same ones.
 */
#ifndef QWARK_INSTALLER_INSTALL_H
#define QWARK_INSTALLER_INSTALL_H

#include <stddef.h>

#include "bootlist.h"

/* The build the module and the pak in the package are, from src/core/proto.h by way of the makefile. */
#ifndef INST_QWARK_BUILD
#error "INST_QWARK_BUILD must be defined: QWARK_BUILD from src/core/proto.h"
#endif

/* The package: its title id, and where it installs its files. pkg.py checks it against its own. */
#define INST_TITLE_ID       "QWRK00001"
#define INST_APP_USRDIR     "/dev_hdd0/game/" INST_TITLE_ID "/USRDIR"

/* Where the files go: the PC client's boot install's places. */
#define INST_PLUGINS_DIR    "/dev_hdd0/plugins"
#define INST_SPRX           BOOTLIST_BOOT_PATH
#define INST_QWARK_DIR      "/dev_hdd0/qwark"
#define INST_PAK            "/dev_hdd0/qwark/ui.pak"
/*
 * Revision 1.26: the game modules a package carries (a test build's, and a
 * release's since build 57), any of the
 * five (revision 1.27 added Tools of Destruction's), each named for its game as qwark's title index names it
 * (src/games/titles.c): INST_GAME_NAME_n is the file, in the package's USRDIR
 * and in /dev_hdd0/qwark, of game id n.
 */
#define INST_GAME_NAME_1    "rac1.sprx"
#define INST_GAME_NAME_2    "rac2.sprx"
#define INST_GAME_NAME_3    "rac3.sprx"
#define INST_GAME_NAME_4    "rac4.sprx"
#define INST_GAME_NAME_5    "tod.sprx"     /* revision 1.27: Tools of Destruction */
#define INST_GAME_1         INST_QWARK_DIR "/" INST_GAME_NAME_1
#define INST_GAME_2         INST_QWARK_DIR "/" INST_GAME_NAME_2
#define INST_GAME_3         INST_QWARK_DIR "/" INST_GAME_NAME_3
#define INST_GAME_4         INST_QWARK_DIR "/" INST_GAME_NAME_4
#define INST_GAME_5         INST_QWARK_DIR "/" INST_GAME_NAME_5
#define INST_GAMES          5
#define INST_LIST           "/dev_hdd0/boot_plugins.txt"

/*
 * The old list's copy. Not /dev_hdd0/boot_plugins.txt.bak: that is the name
 * Cobra's recovery mode renames the list to, to boot without plugins, and a file
 * already there could stand in its way.
 */
#define INST_LIST_BAK       "/dev_hdd0/qwark/boot_plugins.txt.bak"

/* The lists the console reads before INST_LIST when they exist: Cobra the first, PS3HEN both. */
#define INST_USB0_LIST      "/dev_usb000/boot_plugins.txt"
#define INST_USB1_LIST      "/dev_usb001/boot_plugins.txt"

/* A plugin list is a few hundred bytes; anything past this is not one. */
#define INST_LIST_MAX       65536u
/* The module is about 100 KB and the pak 74 KB (build 56; 96 KB with ToD's Lua helper, build 57); this is only a bound on nonsense. */
#define INST_FILE_MAX       (16u << 20)

/* Where it stopped. */
#define INST_STEP_NONE          0
#define INST_STEP_PACKAGE_SPRX  1
#define INST_STEP_PACKAGE_PAK   2
#define INST_STEP_READ_LIST     3
#define INST_STEP_FOLDERS       4
#define INST_STEP_COPY_SPRX     5
#define INST_STEP_COPY_PAK      6
#define INST_STEP_WRITE_BACKUP  7
#define INST_STEP_WRITE_LIST    8
#define INST_STEP_PLACE_BACKUP  9
#define INST_STEP_PLACE_PAK     10
#define INST_STEP_PLACE_SPRX    11
#define INST_STEP_PLACE_LIST    12
/* Revision 1.26: a game module the package carries. */
#define INST_STEP_PACKAGE_GAME  13
#define INST_STEP_COPY_GAME     14
#define INST_STEP_PLACE_GAME    15

/*
 * The library: the mods and premade savefiles RaCMAN Reloaded ships, which a
 * package built with the client beside it carries (pkg.py --client) as one
 * file, USRDIR/library.bin, so console-only users have them too. They go where
 * the client's uploads put them, so qwark finds them exactly as if the client
 * had sent them:
 *
 *   /dev_hdd0/qwark/mods/<TITLEID>/<dir>/      patch.txt, the .bin files it
 *                                              names and qwark.sum, the
 *                                              client's ModLibrary upload
 *   /dev_hdd0/qwark/savefiles/<TITLEID>/(premade savefiles)[ <game>]/<name>.sav
 *                                              and <name>.sav.sum, the client's
 *                                              load of a premade savefile
 *
 * They are installed after qwark is, and only what is missing: a mod whose
 * folder is already there and a savefile already there are left exactly as
 * they are and counted as skipped, whoever put them there and whatever is in
 * them. Each one goes in whole or not at all: every file is expanded once to
 * check it against the CRC-32 the library gives it, and a file that does not
 * match is never written; a mod is written into a folder of its own under
 * INST_LIB_STAGE and moved into place in one rename; a savefile is written
 * as <name>.sav.tmp, read back, and renamed. A failure is a warning, with qwark
 * installed: that mod or savefile is not, the others go on.
 *
 * library.bin, every integer big-endian (pkg.py's make_library writes it):
 *
 *   0   "QLIB", u16 format 1, u16 entries
 *   8   u32 the CRC-32 of the table, u32 the data's length
 *   16  16 bytes of 0
 *   32  the table, INST_LIB_ENTRY bytes an entry:
 *         u8 kind (INST_LIB_MOD_FILE, INST_LIB_SAVE), u8 code (INST_LIB_STORED,
 *         INST_LIB_QRLE), u16 the mod's number (a mod's files are together,
 *         under one number; 0 for a savefile), u32 offset into the data,
 *         u32 its length there, u32 the file's size, u32 the file's CRC-32,
 *         12 bytes of 0, then its path under /dev_hdd0/qwark, NUL-padded to
 *         INST_LIB_PATH_MAX
 *   then the data. Entries may share their data: an alias title's savefiles
 *   (the client's premade-savefiles/titles.txt) are its folder's bytes.
 */
#define INST_LIBRARY_NAME   "library.bin"
#define INST_LIB_MAX        (4u << 20)      /* about 370 KB today */
#define INST_LIB_HEAD       32u
#define INST_LIB_ENTRY      160u
#define INST_LIB_PATH_MAX   128u
#define INST_LIB_MOD_FILE   1
#define INST_LIB_SAVE       2
#define INST_LIB_STORED     0
#define INST_LIB_QRLE       1
#define INST_MODS_DIR       "/dev_hdd0/qwark/mods"
#define INST_SAVES_DIR      "/dev_hdd0/qwark/savefiles"
#define INST_LIB_STAGE      "/dev_hdd0/qwark/staging"
/* The longest name qwark takes for a mod folder (ModFolderName.MaxBytes) and for a savefile or category. */
#define INST_MOD_NAME_MAX   31u
#define INST_SAVE_NAME_MAX  32u

/* Where the library's first failure was. */
#define INST_LIB_NONE       0
#define INST_LIB_READ       1   /* reading library.bin */
#define INST_LIB_DAMAGED    2   /* library.bin is not a whole library; nothing of it was written */
#define INST_LIB_CHECK      3   /* a file in it does not expand to its CRC; it was not written */
#define INST_LIB_FOLDER     4   /* making a folder */
#define INST_LIB_WRITE      5   /* writing a file, or reading it back */
#define INST_LIB_PLACE      6   /* moving a mod or a savefile into place */
#define INST_LIB_LOOK       7   /* the console could not say whether one was there */

typedef struct inst_result {
	int ok;             /* installed */
	int step;           /* INST_STEP_*: where it failed */
	int err;            /* that step's file system error, or 0 */
	const char *why;    /* or what was wrong, for a failure that is not a file system error */
	int undo_failed;    /* the failure came after the moves began, and putting things back failed too */

	int outcome;        /* BOOTLIST_*: what happened to the list */
	int lines;          /* plugin lines in the list afterwards */
	int list_existed;   /* there was a list before */
	int recovered;      /* the list came from boot_plugins.txt.old, an interrupted run's */
	int backed_up;      /* the old list is in INST_LIST_BAK */
	int usb_list;       /* a list on a USB drive, which the console reads instead */
	int games;          /* revision 1.26: game modules installed */
	int game;           /* the one a game step failed on, 1 to 5 */

	/* The library (INST_LIBRARY_NAME above): only once qwark itself is in, so none of it is a reason for !ok. */
	int library;        /* the package carries one */
	int mods, saves;                    /* installed */
	int mods_skipped, saves_skipped;    /* already on the console, so left exactly as they were */
	int mods_failed, saves_failed;      /* not installed, and nothing of them left behind */
	int lib_step;       /* INST_LIB_*: where the first failure was */
	int lib_err;        /* its file system error, or 0 */
	const char *lib_why;    /* or what was wrong */
} inst_result;

/* Installs from the package's USRDIR. Never fails half-way: see above. */
void inst_run(const char *usrdir, inst_result *r);

/*
 * Called between the files the library writes, so the app can keep its screen
 * and the system's callbacks going through a long install. NULL for none.
 */
void inst_set_progress(void (*fn)(void));

/* Whether the package in usrdir carries a library, for the question before anything is written. */
int inst_has_library(const char *usrdir);

/* The package's USRDIR from the app's own path (argv[0]), or INST_APP_USRDIR. */
void inst_usrdir(const char *argv0, char *out, size_t cap);

/*
 * The dialogs' text. A message dialog takes 512 bytes with its NUL, and the
 * tests hold every message to that.
 */
#define INST_MESSAGE_MAX    512

/* The question before anything is written; library: the package carries one. */
void inst_confirm_message(int library, char *out, size_t cap);

/* What happened, and for a success the question whether to restart; hen adds the line HEN needs. */
void inst_result_message(const inst_result *r, int hen, char *out, size_t cap);

/* The restart did not happen: error is what the call answered. */
void inst_restart_failed_message(unsigned int error, char *out, size_t cap);

#endif
