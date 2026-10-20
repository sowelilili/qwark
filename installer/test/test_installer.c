/*
 * The installer's host tests: the boot_plugins.txt edit on its own, then the
 * whole install over a fake console HDD, with failures put at every step.
 * installer/test/run.sh builds and runs it; test/run.sh runs that.
 */
#include "../bootlist.h"
#include "../install.h"
#include "ifs_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_host(p) mkdir((p), 0777)
#endif

#define BOOT BOOTLIST_BOOT_PATH
#define WEBMAN "/dev_hdd0/plugins/webftp_server.sprx"
#define FPS "/dev_hdd0/plugins/VshFpsCounter.sprx"

static int g_pass, g_fail;
static const char *g_group = "";
static char g_root[1024];

static void group(const char *name)
{
	g_group = name;
	printf("-- %s\n", name);
}

static void check(int ok, const char *what)
{
	if (ok) {
		g_pass++;
	} else {
		g_fail++;
		printf("FAIL [%s] %s\n", g_group, what);
	}
}

/* ------------------------------------------------------------------ the edit */

static bootlist_plan g_plan;
static char g_out[BOOTLIST_OUT_MAX(4096)];

static int plan(const char *in)
{
	memset(g_out, 0, sizeof(g_out));
	return bootlist_plan_edit(in, in ? strlen(in) : 0, g_out, sizeof(g_out), &g_plan);
}

/* The list after the edit: the new text, or the old one when it is left as it is. */
static const char *after(const char *in)
{
	return g_plan.outcome == BOOTLIST_ALREADY_FIRST ? in : g_out;
}

static void expect_edit(const char *in, int outcome, const char *want, int lines, const char *what)
{
	char msg[256];
	char again_in[sizeof(g_out)];

	check(plan(in) == 0, what);
	snprintf(msg, sizeof(msg), "%s: outcome %d, want %d", what, g_plan.outcome, outcome);
	check(g_plan.outcome == outcome, msg);
	snprintf(msg, sizeof(msg), "%s: %d lines, want %d", what, g_plan.lines, lines);
	check(g_plan.lines == lines, msg);
	if (outcome == BOOTLIST_ALREADY_FIRST) {
		check(g_plan.len == 0, "an untouched list writes nothing");
	} else {
		snprintf(msg, sizeof(msg), "%s: got [%s]", what, g_out);
		check(g_plan.len == strlen(want) && strcmp(g_out, want) == 0, msg);
	}

	/* Running twice: the edit of an edited list leaves it as it is. */
	snprintf(again_in, sizeof(again_in), "%s", after(in));
	check(plan(again_in) == 0 && g_plan.outcome == BOOTLIST_ALREADY_FIRST, "a second edit changes nothing");
	snprintf(msg, sizeof(msg), "%s: second run %d lines, want %d", what, g_plan.lines, lines);
	check(g_plan.lines == lines, msg);
}

static void test_bootlist(void)
{
	char big[2048];
	char small[64];
	int i;

	group("boot_plugins.txt edit");

	/* No list, or nothing in it. */
	expect_edit(NULL, BOOTLIST_ADDED, BOOT "\n", 1, "no list at all");
	expect_edit("", BOOTLIST_ADDED, BOOT "\n", 1, "an empty list");
	expect_edit("\r\n\r\n  \n\t\n", BOOTLIST_ADDED, BOOT "\n", 1, "blank lines only");

	/* qwark there already. */
	expect_edit(BOOT "\n", BOOTLIST_ALREADY_FIRST, NULL, 1, "qwark alone");
	expect_edit(BOOT, BOOTLIST_ALREADY_FIRST, NULL, 1, "qwark alone, no final newline");
	expect_edit(BOOT "\r\n" WEBMAN "\r\n", BOOTLIST_ALREADY_FIRST, NULL, 2, "qwark first, CRLF kept as it is");
	expect_edit("  " BOOT " \t\n" WEBMAN "\n", BOOTLIST_ALREADY_FIRST, NULL, 2, "qwark first with spaces round it");
	expect_edit("/DEV_HDD0/PLUGINS/QWARK.SPRX\n", BOOTLIST_ALREADY_FIRST, NULL, 1,
	            "qwark first in capitals (the client's OrdinalIgnoreCase)");
	expect_edit("\n\n" BOOT "\n" WEBMAN "\n", BOOTLIST_ALREADY_FIRST, NULL, 2,
	            "blank lines before qwark do not count as lines");

	/* Other plugins, no qwark. */
	expect_edit(WEBMAN "\n", BOOTLIST_ADDED, BOOT "\n" WEBMAN "\n", 2, "webMAN alone");
	expect_edit(WEBMAN "\n" FPS "\n", BOOTLIST_ADDED, BOOT "\n" WEBMAN "\n" FPS "\n", 3, "two plugins keep their order");
	expect_edit(WEBMAN, BOOTLIST_ADDED, BOOT "\n" WEBMAN "\n", 2, "no final newline");
	expect_edit(WEBMAN "\r\n" FPS "\r\n", BOOTLIST_ADDED, BOOT "\n" WEBMAN "\n" FPS "\n", 3, "CRLF comes back as LF");
	expect_edit(WEBMAN "\r\r\n", BOOTLIST_ADDED, BOOT "\n" WEBMAN "\n", 2, "every trailing CR goes");
	expect_edit(WEBMAN "\n\n\r\n" FPS "\n", BOOTLIST_ADDED, BOOT "\n" WEBMAN "\n" FPS "\n", 3, "blank lines go");
	expect_edit("\t" WEBMAN " \n", BOOTLIST_ADDED, BOOT "\n\t" WEBMAN " \n", 2,
	            "a kept line keeps its own spaces, as in the client");

	/* webMAN first, qwark later: qwark goes first. */
	expect_edit(WEBMAN "\n" BOOT "\n", BOOTLIST_MOVED, BOOT "\n" WEBMAN "\n", 2, "webMAN first, then qwark");
	expect_edit(WEBMAN "\r\n" FPS "\r\n" BOOT "\r\n", BOOTLIST_MOVED, BOOT "\n" WEBMAN "\n" FPS "\n", 3,
	            "qwark last, CRLF");

	/* An old path, and more than one qwark line. */
	expect_edit("/dev_hdd0/tmp/qwark.sprx\n" WEBMAN "\n", BOOTLIST_MOVED, BOOT "\n" WEBMAN "\n", 2,
	            "the old /dev_hdd0/tmp path is replaced");
	plan("/dev_hdd0/tmp/qwark.sprx\n" WEBMAN "\n");
	check(g_plan.dropped == 1, "one old line dropped");
	expect_edit(BOOT "\n" BOOT "\n" WEBMAN "\n", BOOTLIST_MOVED, BOOT "\n" WEBMAN "\n", 2, "qwark twice");
	expect_edit(BOOT "\n" WEBMAN "\n/dev_hdd0/qwark.sprx\n", BOOTLIST_MOVED, BOOT "\n" WEBMAN "\n", 2,
	            "qwark first and at another path");
	plan(BOOT "\n" WEBMAN "\n/dev_hdd0/qwark.sprx\n");
	check(g_plan.dropped == 2, "both qwark lines dropped, the first written again");
	expect_edit("/dev_hdd0/plugins/myqwark.sprx\n", BOOTLIST_MOVED, BOOT "\n", 1,
	            "a name ending in qwark.sprx counts as qwark, as in the client");

	/* The six-line limit: counted and never enforced. */
	expect_edit("/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n", BOOTLIST_ADDED,
	            BOOT "\n/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n", 6, "five plugins and qwark: six lines");
	expect_edit("/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n/f.sprx\n", BOOTLIST_ADDED,
	            BOOT "\n/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n/f.sprx\n", 7, "six plugins and qwark: seven, none dropped");
	check(g_plan.lines > BOOTLIST_HEN_LIMIT, "seven lines are past the limit");
	expect_edit("/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n/f.sprx\n" BOOT "\n", BOOTLIST_MOVED,
	            BOOT "\n/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n/f.sprx\n", 7,
	            "qwark seventh, out of the console's reach, comes first");

	/* The output bound is exact. */
	check(bootlist_plan_edit("a", 1, small, BOOTLIST_OUT_MAX(1), &g_plan) == 0 &&
	      g_plan.len == BOOTLIST_OUT_MAX(1), "BOOTLIST_OUT_MAX holds a last line with no newline");
	check(bootlist_plan_edit("a", 1, small, BOOTLIST_OUT_MAX(1) - 1, &g_plan) == -1, "one byte less does not");
	check(bootlist_plan_edit("a", 1, NULL, 0, &g_plan) == -1, "no room at all");
	check(bootlist_plan_edit(BOOT, strlen(BOOT), NULL, 0, &g_plan) == 0 &&
	      g_plan.outcome == BOOTLIST_ALREADY_FIRST, "an untouched list needs no room");

	/* Bytes are bytes: a NUL in a line is kept, not the end of the list. */
	memcpy(big, WEBMAN "\n/x\0y.sprx\n", sizeof(WEBMAN "\n/x\0y.sprx\n") - 1);
	check(bootlist_plan_edit(big, sizeof(WEBMAN "\n/x\0y.sprx\n") - 1, g_out, sizeof(g_out), &g_plan) == 0 &&
	      g_plan.lines == 3 && g_plan.len == strlen(BOOT "\n") + sizeof(WEBMAN "\n/x\0y.sprx\n") - 1 &&
	      memcmp(g_out + strlen(BOOT "\n"), WEBMAN "\n/x\0y.sprx\n", sizeof(WEBMAN "\n/x\0y.sprx\n") - 1) == 0,
	      "a NUL inside a line");

	/* A long list of other plugins: all kept, in order. */
	big[0] = '\0';
	for (i = 0; i < 20; i++) {
		char line[32];
		snprintf(line, sizeof(line), "/p%02d.sprx\r\n", i);
		strcat(big, line);
	}
	check(plan(big) == 0 && g_plan.lines == 21 && strncmp(g_out, BOOT "\n/p00.sprx\n/p01.sprx\n", strlen(BOOT) + 21) == 0,
	      "twenty plugins, all kept");
}

/* ------------------------------------------------------------------ the fake console */

static void path_join(char *out, size_t cap, const char *console_path)
{
	snprintf(out, cap, "%s%s", g_root, console_path);
}

static void make_dirs(const char *console_path)
{
	char p[1300];
	char *s;

	path_join(p, sizeof(p), console_path);
	for (s = p + strlen(g_root) + 1; *s; s++) {
		if (*s == '/') {
			*s = '\0';
			mkdir_host(p);
			*s = '/';
		}
	}
	mkdir_host(p);
}

static void put_file(const char *console_path, const void *data, size_t len)
{
	char p[1300];
	FILE *f;

	path_join(p, sizeof(p), console_path);
	f = fopen(p, "wb");
	if (!f) {
		printf("cannot write %s\n", p);
		exit(2);
	}
	if (len) fwrite(data, 1, len, f);
	fclose(f);
}

static void put_text(const char *console_path, const char *text)
{
	put_file(console_path, text, strlen(text));
}

/* A file's whole contents, NUL-terminated; len -1 when it is not there. */
static char g_file[1 << 20];
static long g_file_len;

static const char *get_file(const char *console_path)
{
	char p[1300];
	FILE *f;

	path_join(p, sizeof(p), console_path);
	g_file_len = -1;
	g_file[0] = '\0';
	if ((f = fopen(p, "rb")) == NULL) return NULL;
	g_file_len = (long)fread(g_file, 1, sizeof(g_file) - 1, f);
	g_file[g_file_len] = '\0';
	fclose(f);
	return g_file;
}

static int file_is(const char *console_path, const char *text)
{
	return get_file(console_path) != NULL && g_file_len == (long)strlen(text) && strcmp(g_file, text) == 0;
}

static int file_is_bytes(const char *console_path, const unsigned char *data, size_t len)
{
	return get_file(console_path) != NULL && g_file_len == (long)len && memcmp(g_file, data, len) == 0;
}

static int present(const char *console_path)
{
	return get_file(console_path) != NULL;
}

static void remove_tree(const char *host_path)
{
	DIR *d = opendir(host_path);
	struct dirent *e;
	char p[1300];
	struct stat st;

	if (!d) {
		remove(host_path);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		snprintf(p, sizeof(p), "%s/%s", host_path, e->d_name);
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
		else remove(p);
	}
	closedir(d);
	rmdir(host_path);
}

/* The package as the pkg installs it: a module and a pak of this build. */
static unsigned char g_sprx[3000];
static unsigned char g_pak[2000];
static unsigned char g_old_sprx[1500];
static unsigned char g_old_pak[700];

static void make_package_files(void)
{
	size_t i;

	for (i = 0; i < sizeof(g_sprx); i++) g_sprx[i] = (unsigned char)(i * 7 + 3);
	memcpy(g_sprx, "SCE\0", 4);
	for (i = 0; i < sizeof(g_pak); i++) g_pak[i] = (unsigned char)(i * 13 + 1);
	memcpy(g_pak, "QPAK", 4);
	g_pak[8] = (unsigned char)((INST_QWARK_BUILD >> 24) & 0xFF);
	g_pak[9] = (unsigned char)((INST_QWARK_BUILD >> 16) & 0xFF);
	g_pak[10] = (unsigned char)((INST_QWARK_BUILD >> 8) & 0xFF);
	g_pak[11] = (unsigned char)(INST_QWARK_BUILD & 0xFF);

	for (i = 0; i < sizeof(g_old_sprx); i++) g_old_sprx[i] = (unsigned char)(i * 5 + 9);
	memcpy(g_old_sprx, "SCE\0", 4);
	for (i = 0; i < sizeof(g_old_pak); i++) g_old_pak[i] = (unsigned char)(i * 3 + 2);
	memcpy(g_old_pak, "QPAK", 4);
}

/* A console with nothing of qwark's on it and the package installed. */
static void fresh_console(void)
{
	ifs_host_reset();
	remove_tree(g_root);
	mkdir_host(g_root);
	make_dirs("/dev_hdd0/tmp");
	make_dirs(INST_APP_USRDIR);
	put_file(INST_APP_USRDIR "/qwark.sprx", g_sprx, sizeof(g_sprx));
	put_file(INST_APP_USRDIR "/ui.pak", g_pak, sizeof(g_pak));
	put_text(INST_APP_USRDIR "/EBOOT.BIN", "an eboot");
}

/* A console that has run an older qwark from boot, after webMAN, and two other plugins. */
static void used_console(void)
{
	fresh_console();
	make_dirs(INST_PLUGINS_DIR);
	make_dirs(INST_QWARK_DIR);
	put_file(INST_SPRX, g_old_sprx, sizeof(g_old_sprx));
	put_file(INST_PAK, g_old_pak, sizeof(g_old_pak));
	put_text(INST_QWARK_DIR "/config.txt", "log = 1\n");
	put_text(INST_LIST, WEBMAN "\r\n" BOOT "\r\n" FPS "\r\n");
}

/* Every file under the console's root, with its contents: what "nothing changed" is held against. */
static char g_snap[2][65536];

static void snap_dir(const char *host_dir, const char *rel, char *out, size_t cap)
{
	DIR *d = opendir(host_dir);
	struct dirent *e;
	char p[1300], r[1300];
	struct stat st;
	char *names[256];
	int n = 0, i, j;

	if (!d) return;
	while ((e = readdir(d)) != NULL && n < 256) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	/* Sorted, so two snapshots of the same tree read the same. */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }

	for (i = 0; i < n; i++) {
		snprintf(p, sizeof(p), "%s/%s", host_dir, names[i]);
		snprintf(r, sizeof(r), "%s/%s", rel, names[i]);
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			snap_dir(p, r, out, cap);
		} else {
			FILE *f = fopen(p, "rb");
			unsigned long sum = 5381;
			int c;
			size_t len = strlen(out);
			if (f) {
				while ((c = fgetc(f)) != EOF) sum = sum * 33 + (unsigned long)c;
				fclose(f);
			}
			snprintf(out + len, cap - len, "%s %ld %lx\n", r, (long)st.st_size, sum);
		}
		free(names[i]);
	}
}

static const char *snapshot(int which)
{
	g_snap[which][0] = '\0';
	snap_dir(g_root, "", g_snap[which], sizeof(g_snap[which]));
	return g_snap[which];
}

static int no_leftovers(void)
{
	return !present(INST_SPRX ".tmp") && !present(INST_PAK ".tmp") && !present(INST_LIST ".tmp") &&
	       !present(INST_LIST_BAK ".tmp") && !present(INST_LIST ".old") && !present(INST_PAK ".old") &&
	       !present(INST_LIST_BAK ".old");
}

static int message_fits(const inst_result *r)
{
	char msg[2048];
	int hen;

	for (hen = 0; hen < 2; hen++) {
		inst_result_message(r, hen, msg, sizeof(msg));
		if (strlen(msg) >= INST_MESSAGE_MAX) {
			printf("message of %u bytes:\n%s\n", (unsigned)strlen(msg), msg);
			return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ the install */

static void test_install_fresh(void)
{
	inst_result r;
	char msg[INST_MESSAGE_MAX];

	group("install on a console with no list");
	fresh_console();
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok, "installed");
	check(r.outcome == BOOTLIST_ADDED && r.lines == 1, "qwark added, one line");
	check(!r.list_existed && !r.backed_up && !r.recovered, "no list before, so no backup");
	check(file_is(INST_LIST, BOOT "\n"), "the list is qwark's line");
	check(file_is_bytes(INST_SPRX, g_sprx, sizeof(g_sprx)), "the module is the package's");
	check(file_is_bytes(INST_PAK, g_pak, sizeof(g_pak)), "the pak is the package's");
	check(!present(INST_LIST_BAK), "no backup of a list that was not there");
	check(!present(INST_SPRX ".old"), "no old module");
	check(no_leftovers(), "no .tmp or .old left");
	check(!r.usb_list, "no USB list");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "qwark build") != NULL && strstr(msg, "Restart now?") != NULL, "the message asks to restart");
	check(strstr(msg, "HEN") == NULL, "no HEN line off HEN");
	check(message_fits(&r), "the message fits a dialog");

	group("running it twice");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok, "installed again");
	check(r.outcome == BOOTLIST_ALREADY_FIRST && r.lines == 1, "the list already starts with qwark");
	check(file_is(INST_LIST, BOOT "\n"), "the list is unchanged");
	check(!r.backed_up && !present(INST_LIST_BAK), "an unchanged list is not backed up");
	check(file_is_bytes(INST_SPRX, g_sprx, sizeof(g_sprx)), "the module is the package's");
	check(file_is_bytes(INST_SPRX ".old", g_sprx, sizeof(g_sprx)), "the first run's module is qwark.sprx.old");
	check(file_is_bytes(INST_PAK, g_pak, sizeof(g_pak)), "the pak is the package's");
	check(no_leftovers(), "no .tmp or .old left");
	inst_result_message(&r, 1, msg, sizeof(msg));
	check(strstr(msg, "already started with qwark") != NULL, "the message says the list was right");
	check(strstr(msg, "enable HEN") != NULL, "on HEN it says to enable HEN after the restart");
}

static void test_install_used(void)
{
	inst_result r;
	char msg[INST_MESSAGE_MAX];

	group("install over webMAN and an older qwark");
	used_console();
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok, "installed");
	check(r.outcome == BOOTLIST_MOVED && r.lines == 3, "qwark moved first, three lines");
	check(file_is(INST_LIST, BOOT "\n" WEBMAN "\n" FPS "\n"), "qwark first, the others behind it in their order");
	check(r.backed_up && file_is(INST_LIST_BAK, WEBMAN "\r\n" BOOT "\r\n" FPS "\r\n"), "the old list, byte for byte, in the backup");
	check(file_is_bytes(INST_SPRX, g_sprx, sizeof(g_sprx)), "the new module");
	check(file_is_bytes(INST_SPRX ".old", g_old_sprx, sizeof(g_old_sprx)), "the old module kept as qwark.sprx.old");
	check(file_is_bytes(INST_PAK, g_pak, sizeof(g_pak)), "the new pak");
	check(file_is(INST_QWARK_DIR "/config.txt", "log = 1\n"), "qwark's config untouched");
	check(no_leftovers(), "no .tmp or .old left");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "now first") != NULL && strstr(msg, INST_LIST_BAK) != NULL, "the message names the backup");

	group("a second run keeps the first run's backup");
	put_text(INST_LIST, BOOT "\n" WEBMAN "\n" FPS "\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.outcome == BOOTLIST_ALREADY_FIRST && !r.backed_up, "nothing to edit");
	check(file_is(INST_LIST_BAK, WEBMAN "\r\n" BOOT "\r\n" FPS "\r\n"), "the backup still holds the list from before qwark");

	group("a changed list after that is backed up again");
	put_text(INST_LIST, WEBMAN "\n" BOOT "\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.outcome == BOOTLIST_MOVED && r.backed_up, "moved, and backed up");
	check(file_is(INST_LIST, BOOT "\n" WEBMAN "\n"), "qwark first");
	check(file_is(INST_LIST_BAK, WEBMAN "\n" BOOT "\n"), "the backup is the list just before");
	check(no_leftovers(), "no .tmp or .old left");
}

static void test_install_lists(void)
{
	inst_result r;
	char msg[INST_MESSAGE_MAX];
	char big[INST_LIST_MAX + 100];

	group("seven plugins");
	fresh_console();
	put_text(INST_LIST, "/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n/f.sprx\n/g.sprx\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.lines == 8, "installed, eight lines");
	check(file_is(INST_LIST, BOOT "\n/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n/f.sprx\n/g.sprx\n"), "nobody's line dropped");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "The list has 8 plugins. The console loads only the first 6.") != NULL, "the message warns");
	check(message_fits(&r), "the message fits a dialog");

	group("six lines exactly");
	fresh_console();
	put_text(INST_LIST, "/a.sprx\n/b.sprx\n/c.sprx\n/d.sprx\n/e.sprx\n");
	inst_run(INST_APP_USRDIR, &r);
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(r.ok && r.lines == 6 && strstr(msg, "loads only") == NULL, "no warning at six");

	group("a list on a USB drive");
	fresh_console();
	make_dirs("/dev_usb000");
	put_text(INST_USB0_LIST, WEBMAN "\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.usb_list, "installed, and the USB list seen");
	check(file_is(INST_USB0_LIST, WEBMAN "\n"), "the USB list is not touched");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "USB drive") != NULL, "the message says the USB list wins");
	fresh_console();
	make_dirs("/dev_usb001");
	put_text(INST_USB1_LIST, WEBMAN "\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.usb_list, "the second USB port too (PS3HEN reads it)");

	group("a list too large to be one");
	fresh_console();
	memset(big, 'x', sizeof(big));
	put_file(INST_LIST, big, INST_LIST_MAX + 1);
	snapshot(0);
	inst_run(INST_APP_USRDIR, &r);
	check(!r.ok && r.step == INST_STEP_READ_LIST, "refused at reading the list");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "nothing changed");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "over 64 KB") != NULL && strstr(msg, "Nothing on the console was changed.") != NULL,
	      "the message says why, and that nothing changed");

	group("a list of exactly 64 KB");
	fresh_console();
	memset(big, 'x', INST_LIST_MAX - 1);
	big[INST_LIST_MAX - 1] = '\n';
	put_file(INST_LIST, big, INST_LIST_MAX);
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.lines == 2, "taken");
	check(get_file(INST_LIST) && g_file_len == (long)(INST_LIST_MAX + strlen(BOOT "\n")), "written whole");
}

static void test_install_package(void)
{
	inst_result r;
	char msg[INST_MESSAGE_MAX];
	unsigned char bad[64];

	group("a damaged package");
	used_console();
	ifs_host_reset();
	{
		char p[1300];
		path_join(p, sizeof(p), INST_APP_USRDIR "/ui.pak");
		remove(p);
	}
	snapshot(0);
	inst_run(INST_APP_USRDIR, &r);
	check(!r.ok && r.step == INST_STEP_PACKAGE_PAK, "no pak: refused");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "nothing changed");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "ui.pak in this package: it is missing") != NULL, "the message names the file");

	used_console();
	memcpy(bad, g_pak, sizeof(bad));
	bad[11] ^= 1;
	put_file(INST_APP_USRDIR "/ui.pak", bad, sizeof(bad));
	snapshot(0);
	inst_run(INST_APP_USRDIR, &r);
	check(!r.ok && r.step == INST_STEP_PACKAGE_PAK && strstr(r.why, "another build") != NULL, "another build's pak: refused");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "nothing changed");

	used_console();
	put_text(INST_APP_USRDIR "/ui.pak", "QPAK");
	inst_run(INST_APP_USRDIR, &r);
	check(!r.ok && r.step == INST_STEP_PACKAGE_PAK, "a pak cut short: refused");

	used_console();
	memcpy(bad, g_sprx, sizeof(bad));
	bad[0] = 0x7F;
	put_file(INST_APP_USRDIR "/qwark.sprx", bad, sizeof(bad));
	snapshot(0);
	inst_run(INST_APP_USRDIR, &r);
	check(!r.ok && r.step == INST_STEP_PACKAGE_SPRX && strstr(r.why, "signed") != NULL, "an unsigned module: refused");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "nothing changed");

	used_console();
	put_text(INST_APP_USRDIR "/qwark.sprx", "");
	inst_run(INST_APP_USRDIR, &r);
	check(!r.ok && r.step == INST_STEP_PACKAGE_SPRX, "an empty module: refused");
}

static void test_install_recover(void)
{
	inst_result r;

	group("an interrupted run's list");
	fresh_console();
	make_dirs(INST_QWARK_DIR);
	put_text(INST_LIST ".old", WEBMAN "\n" FPS "\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.recovered && r.list_existed, "installed from boot_plugins.txt.old");
	check(file_is(INST_LIST, BOOT "\n" WEBMAN "\n" FPS "\n"), "the other plugins come back behind qwark");
	check(file_is(INST_LIST_BAK, WEBMAN "\n" FPS "\n"), "and are backed up");
	check(no_leftovers(), "boot_plugins.txt.old is gone");

	fresh_console();
	put_text(INST_LIST ".old", BOOT "\r\n" WEBMAN "\r\n");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && r.recovered && r.outcome == BOOTLIST_ALREADY_FIRST, "an interrupted list that was already right");
	check(file_is(INST_LIST, BOOT "\r\n" WEBMAN "\r\n"), "goes back exactly as it was");
	check(no_leftovers(), "boot_plugins.txt.old is gone");

	group("an interrupted run's leftovers");
	used_console();
	put_text(INST_SPRX ".tmp", "half a module");
	put_text(INST_LIST ".tmp", "half a list");
	put_text(INST_PAK ".tmp", "half a pak");
	put_text(INST_LIST ".old", "the list before the last run");
	inst_run(INST_APP_USRDIR, &r);
	check(r.ok && !r.recovered, "installed; the list in place wins over the .old");
	check(file_is(INST_LIST, BOOT "\n" WEBMAN "\n" FPS "\n"), "the list");
	check(file_is_bytes(INST_SPRX, g_sprx, sizeof(g_sprx)), "the module");
	check(no_leftovers(), "every leftover is gone");
}

/*
 * Every call the installer makes, failed in turn. Whatever fails, the console
 * is either installed or exactly as it was (or, when putting things back fails
 * too, the message says so).
 */
static void sweep(void (*setup)(void), const char *name)
{
	inst_result r;
	char what[256];
	char msg[INST_MESSAGE_MAX];
	int calls, n, failures = 0, unchanged = 0, installed = 0, broken = 0;
	char want_list[256];
	static char before[65536];

	setup();
	snprintf(before, sizeof(before), "%s", snapshot(0));
	inst_run(INST_APP_USRDIR, &r);
	calls = ifs_host_calls();
	snprintf(want_list, sizeof(want_list), "%s", get_file(INST_LIST) ? g_file : "");
	snprintf(what, sizeof(what), "%s: a clean run makes %d calls", name, calls);
	check(r.ok && calls > 20, what);

	for (n = 1; n <= calls; n++) {
		setup();
		ifs_host_fail_at(n);
		inst_run(INST_APP_USRDIR, &r);
		ifs_host_reset();
		inst_result_message(&r, 0, msg, sizeof(msg));
		if (r.ok) {
			installed++;
			snprintf(what, sizeof(what), "%s, call %d failed: installed anyway, and right", name, n);
			check(file_is(INST_LIST, want_list) && file_is_bytes(INST_SPRX, g_sprx, sizeof(g_sprx)) &&
			      file_is_bytes(INST_PAK, g_pak, sizeof(g_pak)), what);
		} else if (r.undo_failed) {
			broken++;
			snprintf(what, sizeof(what), "%s, call %d failed: the message says what to check", name, n);
			check(strstr(msg, "could not be put back") != NULL, what);
		} else {
			failures++;
			snprintf(what, sizeof(what), "%s, call %d failed (step %d): nothing changed", name, n, r.step);
			check(strcmp(snapshot(1), before) == 0, what);
			if (strcmp(g_snap[1], before) != 0) printf("before:\n%safter:\n%s", before, g_snap[1]);
			if (strcmp(g_snap[1], before) == 0) unchanged++;
			check(strstr(msg, "Nothing on the console was changed.") != NULL, "the message says nothing changed");
		}
		check(message_fits(&r), "the message fits a dialog");
	}
	printf("   %s: %d calls; %d failures left it as it was, %d still installed, %d could not undo\n",
	       name, calls, unchanged, installed, broken);
	check(broken == 0, "a single failed call can always be undone");
	check(failures > 10, "most failures stop the install");
}

static void fresh_with_webman(void)
{
	fresh_console();
	put_text(INST_LIST, WEBMAN "\n");
}

/* An earlier run stopped between its two renames: no list, the old one as boot_plugins.txt.old. */
static void interrupted_console(void)
{
	fresh_console();
	make_dirs(INST_PLUGINS_DIR);
	make_dirs(INST_QWARK_DIR);
	put_file(INST_SPRX, g_old_sprx, sizeof(g_old_sprx));
	put_text(INST_LIST ".old", WEBMAN "\n" FPS "\n");
}

static void test_install_faults(void)
{
	inst_result r;
	char msg[INST_MESSAGE_MAX];

	group("a failure at every call");
	sweep(fresh_console, "fresh console");
	sweep(fresh_with_webman, "webMAN only");
	sweep(used_console, "older qwark after webMAN");
	sweep(interrupted_console, "an interrupted run's list");

	group("named failures");
	interrupted_console();
	snapshot(0);
	ifs_host_fault(IFS_OP_RENAME, "/dev_hdd0/boot_plugins.txt");
	inst_run(INST_APP_USRDIR, &r);
	ifs_host_reset();
	check(!r.ok && r.recovered && r.step == INST_STEP_PLACE_LIST, "an interrupted run's list cannot be moved in");
	check(file_is(INST_LIST ".old", WEBMAN "\n" FPS "\n"), "its only copy, boot_plugins.txt.old, is still there");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "and nothing else changed");

	used_console();
	snapshot(0);
	ifs_host_fault(IFS_OP_RENAME, "/dev_hdd0/boot_plugins.txt");
	inst_run(INST_APP_USRDIR, &r);
	ifs_host_reset();
	check(!r.ok && r.step == INST_STEP_PLACE_LIST, "the list cannot be moved in");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "the module, the pak and the backup are put back");
	check(file_is_bytes(INST_SPRX, g_old_sprx, sizeof(g_old_sprx)), "the old module is back");

	used_console();
	snapshot(0);
	ifs_host_corrupt("qwark.sprx.tmp");
	inst_run(INST_APP_USRDIR, &r);
	ifs_host_reset();
	check(!r.ok && r.step == INST_STEP_COPY_SPRX && r.why && strstr(r.why, "read back") != NULL,
	      "a copy that reads back wrong is caught");
	check(strcmp(snapshot(1), g_snap[0]) == 0, "nothing changed");
	inst_result_message(&r, 0, msg, sizeof(msg));
	check(strstr(msg, "copying qwark.sprx: the copy did not read back the same.") != NULL, "the message says so");

	used_console();
	ifs_host_corrupt("boot_plugins.txt.tmp");
	inst_run(INST_APP_USRDIR, &r);
	ifs_host_reset();
	check(!r.ok && r.step == INST_STEP_WRITE_LIST, "a list that reads back wrong is caught");
	check(file_is(INST_LIST, WEBMAN "\r\n" BOOT "\r\n" FPS "\r\n"), "the list is as it was");

	group("a drive that stops answering");
	{
		/* Every call from one of the last few on fails, putting back included. */
		int calls, n, said = 0;
		used_console();
		inst_run(INST_APP_USRDIR, &r);
		calls = ifs_host_calls();
		for (n = calls - 8; n <= calls; n++) {
			used_console();
			ifs_host_fail_from(n);
			inst_run(INST_APP_USRDIR, &r);
			ifs_host_reset();
			if (!r.ok && r.undo_failed) {
				inst_result_message(&r, 0, msg, sizeof(msg));
				said += strstr(msg, "Check /dev_hdd0/boot_plugins.txt before you restart.") != NULL;
			}
		}
		check(said > 0, "an undo that fails says what to check");
	}
}

static void test_messages(void)
{
	inst_result r;
	char msg[2048];
	int step;

	group("messages");
	inst_confirm_message(msg, sizeof(msg));
	check(strlen(msg) < INST_MESSAGE_MAX && strstr(msg, "Install qwark build") != NULL, "the question fits");
	check(strstr(msg, "Your other plugins stay in the list.") != NULL, "and says the other plugins stay");

	memset(&r, 0, sizeof(r));
	r.ok = 1;
	r.lines = 99;
	r.backed_up = 1;
	r.usb_list = 1;
	check(message_fits(&r), "the longest success fits");

	for (step = INST_STEP_PACKAGE_SPRX; step <= INST_STEP_PLACE_LIST; step++) {
		memset(&r, 0, sizeof(r));
		r.step = step;
		r.err = (int)0x80010006u;
		check(message_fits(&r), "a failure fits");
		inst_result_message(&r, 0, msg, sizeof(msg));
		check(strstr(msg, "unknown") == NULL && strstr(msg, "(error 0x80010006)") != NULL, "every step has a name and its code");
		r.why = "it is over 64 KB, so it is not a plugin list";
		r.undo_failed = 1;
		check(message_fits(&r), "the longest failure fits");
	}

	inst_restart_failed_message(0x80010003u, msg, sizeof(msg));
	check(strlen(msg) < INST_MESSAGE_MAX && strstr(msg, "0x80010003") != NULL, "the restart failure fits");

	group("the package's folder");
	inst_usrdir("/dev_hdd0/game/QWRK00001/USRDIR/EBOOT.BIN", msg, sizeof(msg));
	check(strcmp(msg, "/dev_hdd0/game/QWRK00001/USRDIR") == 0, "from argv[0]");
	inst_usrdir("/dev_hdd0/game/OTHER0001/USRDIR/EBOOT.BIN", msg, sizeof(msg));
	check(strcmp(msg, "/dev_hdd0/game/OTHER0001/USRDIR") == 0, "wherever the console put it");
	inst_usrdir(NULL, msg, sizeof(msg));
	check(strcmp(msg, INST_APP_USRDIR) == 0, "no argv[0]");
	inst_usrdir("EBOOT.BIN", msg, sizeof(msg));
	check(strcmp(msg, INST_APP_USRDIR) == 0, "a relative argv[0]");
}

int main(int argc, char **argv)
{
	const char *root = argc > 1 ? argv[1] : "installer/test/root";

	snprintf(g_root, sizeof(g_root), "%s", root);
	ifs_host_root(g_root);
	make_package_files();

	test_bootlist();
	test_install_fresh();
	test_install_used();
	test_install_lists();
	test_install_package();
	test_install_recover();
	test_install_faults();
	test_messages();

	remove_tree(g_root);
	printf("installer: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
