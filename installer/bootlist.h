/*
 * The edit the installer makes to /dev_hdd0/boot_plugins.txt. It is the same
 * edit the PC client's "Install to boot_plugins.txt" makes (racman-reloaded,
 * src/RaCMAN.Protocol/WebManLoader.cs, PlanBootList), rule for rule, so a
 * console ends up with the same list whichever of the two installed qwark:
 *
 *  - a line is what is left of a '\n'-separated piece once its trailing '\r's
 *    are gone; a line that is all white space is not a plugin and is dropped;
 *  - a line names qwark when, trimmed, it ends in "qwark.sprx", in any case,
 *    at whatever path it was installed to (the old /dev_hdd0/tmp one included);
 *  - a list whose first line is the boot path and which names qwark exactly
 *    once is left exactly as it is, line endings and all;
 *  - any other list is written out again: the boot path first, then every line
 *    that does not name qwark, in its order, each ending in '\n'. So qwark is
 *    named once and first, and nobody's plugin is dropped.
 *
 * The console loads at most BOOTLIST_HEN_LIMIT plugins from the list (PS3HEN
 * and Cobra both fill VSH slots 1 to 6 from it), so a longer list is not an
 * error, as in the client: qwark is first whatever happens, the lines past the
 * sixth are somebody else's plugins, and only the user can say which to drop.
 *
 * Pure: no file system, so the host tests build it as it is.
 */
#ifndef QWARK_INSTALLER_BOOTLIST_H
#define QWARK_INSTALLER_BOOTLIST_H

#include <stddef.h>

/* Where the installer, like the client's boot install, puts the module. */
#define BOOTLIST_BOOT_PATH  "/dev_hdd0/plugins/qwark.sprx"
#define BOOTLIST_SPRX_NAME  "qwark.sprx"
#define BOOTLIST_HEN_LIMIT  6

#define BOOTLIST_ADDED          0   /* the list did not name qwark; it is first now */
#define BOOTLIST_MOVED          1   /* named lower down, more than once, or at another path */
#define BOOTLIST_ALREADY_FIRST  2   /* already began with the boot path: left as it is */

typedef struct bootlist_plan {
	int outcome;        /* BOOTLIST_* */
	int lines;          /* plugin lines the list holds afterwards */
	int dropped;        /* lines naming qwark that were dropped on the way */
	size_t len;         /* bytes of the new list in out; 0 for ALREADY_FIRST */
} bootlist_plan;

/*
 * Plans the edit of the list in in[0..inlen) (inlen 0 for no list at all) and,
 * unless the list stays as it is, writes the new one to out. Returns 0, or -1
 * when out cannot hold it, which is never for an out of BOOTLIST_OUT_MAX(inlen)
 * bytes: the boot path and its '\n', then every kept line with the '\n' that
 * followed it in the list, or one more for a last line that had none.
 */
int bootlist_plan_edit(const char *in, size_t inlen, char *out, size_t outcap, bootlist_plan *plan);

/* What bootlist_plan_edit needs out to hold for a list of inlen bytes. */
#define BOOTLIST_OUT_MAX(inlen) ((inlen) + sizeof(BOOTLIST_BOOT_PATH) + 1)

#endif
