/*
 * What the installer does, as one call: the PS3 app (main.c) shows the dialogs
 * around it and the host tests (test/test_installer.c) run it over a fake HDD.
 *
 * It puts the module and the console menu's file where the PC client's
 * "Install to boot_plugins.txt" puts them, and edits the list the way that
 * install does (bootlist.h). It is written so that a failure at any point
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
/* The module is about 100 KB and the pak 66 KB; this is only a bound on nonsense. */
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
} inst_result;

/* Installs from the package's USRDIR. Never fails half-way: see above. */
void inst_run(const char *usrdir, inst_result *r);

/* The package's USRDIR from the app's own path (argv[0]), or INST_APP_USRDIR. */
void inst_usrdir(const char *argv0, char *out, size_t cap);

/*
 * The dialogs' text. A message dialog takes 512 bytes with its NUL, and the
 * tests hold every message to that.
 */
#define INST_MESSAGE_MAX    512

/* The question before anything is written. */
void inst_confirm_message(char *out, size_t cap);

/* What happened, and for a success the question whether to restart; hen adds the line HEN needs. */
void inst_result_message(const inst_result *r, int hen, char *out, size_t cap);

/* The restart did not happen: error is what the call answered. */
void inst_restart_failed_message(unsigned int error, char *out, size_t cap);

#endif
