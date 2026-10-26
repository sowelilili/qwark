#include "config.h"
#include "util.h"
#include "savefile.h"
#include "../plat/plat.h"

#include <string.h>

/* CONFIG_MAX_ENTRIES and CONFIG_TEXT_MAX are in config.h, beside the key and
 * value widths they are derived from. */

/*
 * positions/<game>.txt. A line is "<planet>.<slot> = <hex>\n": a planet index,
 * which is a u8 and so at most three digits, a dot, a slot digit (there are
 * eight), " = ", two hex digits per blob byte, and the newline. The blob is 40
 * bytes in RaC4 and 30 in the other three, and QWARK_MAX_BLOB, the most any game
 * may store, is 64.
 *
 * So the file this table can write is POS_MAX_ENTRIES of the widest line, and
 * that is what the buffer is, rather than the round 8192 it was: a full table of
 * maximum-length blobs came to 8769 bytes, which the old buffer could not read
 * back, and a positions file that does not fit is a file whose every slot is
 * silently dropped. What the four games actually write is 89 bytes a line, so a
 * full table of RaC4 positions is 5.6 KB and always was.
 */
#define POS_MAX_ENTRIES    64
#define POS_LINE_MAX       (3 + 1 + 1 + 3 + 2 * QWARK_MAX_BLOB + 1)
#define POS_TEXT_MAX       (POS_MAX_ENTRIES * POS_LINE_MAX + 2)

struct kv {
	char key[CONFIG_KEY_MAX];
	char value[CONFIG_VAL_MAX];
	u8   used;
};

static struct kv g_kv[CONFIG_MAX_ENTRIES];
/*
 * Revision 1.24. The active profile's config.txt, the same shape as the
 * console's: a profile holds only per-user keys and combo overrides, which are
 * at most what the console's file holds of them.
 */
static struct kv g_pkv[CONFIG_MAX_ENTRIES];
static u32 g_version;

/* Revision 1.24. The active profile (config.h, the resolver). */
static struct {
	u8   active;    /* per-user keys come from g_pkv */
	u8   exists;    /* its folder and config.txt have been made */
	u8   seq;       /* SessionInfo byte 30 */
	u8   pad;
	u32  user;      /* the user it is, 0 when none is active */
	char folder[PROFILE_FOLDER_MAX];
} g_prof;

static int pos_load_file(void);
/* The positions' title (rac1 .. rac4), which the slot table further down is of. */
static char g_pos_title[16];

/*
 * config.txt and the positions file are read into the core's shared text buffer
 * (util.h), under the core lock like every other user of it, rather than into
 * two buffers of their own. gnu99, so no _Static_assert: a cap that outgrew that
 * buffer fails the build here rather than running off the end of it.
 */
typedef char config_text_fits[CONFIG_TEXT_MAX <= QTEXT_BYTES ? 1 : -1];
typedef char pos_text_fits[POS_TEXT_MAX <= QTEXT_BYTES ? 1 : -1];

static const char * const g_combo_key[COMBO_COUNT] = {
	"combo.save",
	"combo.load",
	"combo.die",
	"combo.planet",
	"combo.setaside",
	"combo.menu",       /* revision 1.22, COMBO_OPEN_MENU */
	"combo.fastloads"   /* revision 1.23, COMBO_TOGGLE_FAST_LOADS */
};

/* Revision 1.12. The switch under those five, and in the same scope as them. */
static const char g_combo_enabled_key[] = "combo.enabled";

/* ------------------------------------------------------------------ store */

static struct kv *kv_find(struct kv *t, const char *key)
{
	int i;
	for (i = 0; i < CONFIG_MAX_ENTRIES; i++) {
		if (t[i].used && qstreq(t[i].key, key)) return &t[i];
	}
	return NULL;
}

static int kv_put(struct kv *t, const char *key, const char *value)
{
	struct kv *e = kv_find(t, key);
	int i;

	if (e == NULL) {
		for (i = 0; i < CONFIG_MAX_ENTRIES; i++) {
			if (!t[i].used) { e = &t[i]; break; }
		}
		if (e == NULL) return ST_FULL;
		e->used = 1;
		qstrcpy(e->key, sizeof(e->key), key);
	}

	qstrcpy(e->value, sizeof(e->value), value);
	g_version++;
	return ST_OK;
}

/* ------------------------------------------------------------ key scopes */

static int has_prefix(const char *s, const char *p)
{
	while (*p != 0) {
		if (*s != *p) return 0;
		s++;
		p++;
	}
	return 1;
}

/*
 * "<TITLEID>.auto.<slug>" and "<TITLEID>.mod.<dirname>.auto": nine capitals
 * and digits, the title id every key of the kind starts with (feature_key and
 * mod_key, below), then ".auto." or ".mod.". The NUL fails the character test,
 * so a short key stops there.
 */
static int title_auto_key(const char *key)
{
	int i;

	for (i = 0; i < 9; i++) {
		char c = key[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
	}
	return has_prefix(key + 9, ".auto.") || has_prefix(key + 9, ".mod.");
}

int config_key_scope(const char *key)
{
	int i;

	if (key == NULL) return CONFIG_SCOPE_CONSOLE;
	for (i = 0; i < COMBO_COUNT; i++) {
		if (qstreq(key, g_combo_key[i])) return CONFIG_SCOPE_COMBO;
	}
	if (has_prefix(key, "selected.") || has_prefix(key, "manip.") ||
	    has_prefix(key, "profile.") || qstreq(key, "menu.help") || title_auto_key(key))
		return CONFIG_SCOPE_USER;
	return CONFIG_SCOPE_CONSOLE;
}

/*
 * A per-user key of an active profile is the profile's alone: one the profile
 * does not hold is unset, whatever the console's file says, so a profile made
 * fresh reads every default. A combo is the profile's override when it holds
 * one, and the console's otherwise.
 */
const char *config_get(const char *key)
{
	struct kv *e;

	if (g_prof.active) {
		int scope = config_key_scope(key);
		if (scope == CONFIG_SCOPE_USER) {
			e = kv_find(g_pkv, key);
			return e ? e->value : NULL;
		}
		if (scope == CONFIG_SCOPE_COMBO) {
			e = kv_find(g_pkv, key);
			if (e != NULL) return e->value;
		}
	}
	e = kv_find(g_kv, key);
	return e ? e->value : NULL;
}

u32 config_version(void)
{
	return g_version;
}

static int global_save(void);
static int profile_save(void);
static int profile_make(void);

/*
 * Every write lands in the active profile's file when the key is a per-user
 * one, making the profile first if it has never been made, and in the
 * console's file otherwise. COMBO_SET's masks are the console's: an override
 * goes through config_set_combo_override.
 */
int config_set(const char *key, const char *value)
{
	int rc;

	if (g_prof.active && config_key_scope(key) == CONFIG_SCOPE_USER) {
		rc = profile_make();
		if (rc != ST_OK) return rc;
		rc = kv_put(g_pkv, key, value);
		if (rc != ST_OK) return rc;
		return profile_save();
	}

	rc = kv_put(g_kv, key, value);
	if (rc != ST_OK) return rc;
	return global_save();
}

u32 config_get_u32(const char *key, u32 fallback)
{
	const char *v = config_get(key);
	int ok = 0;
	u32 out;

	if (v == NULL) return fallback;
	out = qparse_u32(v, &ok);
	return ok ? out : fallback;
}

int config_set_u32(const char *key, u32 value)
{
	char buf[16];
	qfmt_u32(buf, sizeof(buf), value);
	return config_set(key, buf);
}

/* -------------------------------------------------------------- load/save */

/*
 * Build 56. A file is written as "<path>.new" and then put in the old one's
 * place, so a write cut off part of the way leaves the last whole file rather
 * than half of the new one. cellFs will not rename onto a file that exists, so
 * the old one goes first; a read that finds no file but a ".new" takes that
 * (qfile_read below), which is all the gap between the two can leave.
 */
#define FILE_PATH_MAX 128

static void new_path(const char *path, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, path);
	qstrcat(out, cap, ".new");
}

static int file_begin(const char *path, plat_file_t *f)
{
	char tmp[FILE_PATH_MAX];

	new_path(path, tmp, sizeof(tmp));
	return plat_file_open(tmp, PLAT_OPEN_WRITE, f) == 0 ? ST_OK : ST_IO_ERROR;
}

/* Closes the ".new" and puts it in place, or throws it away when `ok` is not. */
static int file_end(const char *path, plat_file_t f, int ok)
{
	char tmp[FILE_PATH_MAX];

	plat_file_close(f);
	new_path(path, tmp, sizeof(tmp));
	if (!ok) {
		plat_file_unlink(tmp);
		return ST_IO_ERROR;
	}
	plat_file_unlink(path);
	return plat_file_rename(tmp, path) == 0 ? ST_OK : ST_IO_ERROR;
}

/*
 * qread_file, and the ".new" a cut-off write left when there is no file. The
 * ".new" stays where it is: the next write of the file replaces it.
 */
static int qfile_read(const char *path, char *buf, u32 cap, u32 *len)
{
	char tmp[FILE_PATH_MAX];
	int rc = qread_file(path, buf, cap, len);

	if (rc != ST_NOT_FOUND) return rc;
	new_path(path, tmp, sizeof(tmp));
	return qread_file(tmp, buf, cap, len);
}

/* One "key = value" file into a table, which is emptied first whatever the file is. */
static int store_load(struct kv *t, const char *path)
{
	char *text = qtext();
	char *cursor;
	char *line;
	u32 len = 0;
	int rc;

	memset(t, 0, CONFIG_MAX_ENTRIES * sizeof(struct kv));
	g_version++;

	rc = qfile_read(path, text, CONFIG_TEXT_MAX, &len);
	if (rc != ST_OK) return rc;

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		char *eq;
		char *key;
		char *value;

		line = qtrim(line);
		if (line[0] == 0 || line[0] == '#') continue;

		eq = line;
		while (*eq != 0 && *eq != '=') eq++;
		if (*eq != '=') continue;

		*eq = 0;
		key = qtrim(line);
		value = qtrim(eq + 1);

		if (key[0] == 0) continue;
		kv_put(t, key, value);
	}
	return ST_OK;
}

static int store_save(const struct kv *t, const char *path)
{
	plat_file_t f;
	int i;
	int ok = 1;

	if (file_begin(path, &f) != ST_OK) return ST_IO_ERROR;

	for (i = 0; i < CONFIG_MAX_ENTRIES && ok; i++) {
		char line[CONFIG_KEY_MAX + CONFIG_VAL_MAX + 8];
		if (!t[i].used) continue;

		line[0] = 0;
		qstrcat(line, sizeof(line), t[i].key);
		qstrcat(line, sizeof(line), " = ");
		qstrcat(line, sizeof(line), t[i].value);
		qstrcat(line, sizeof(line), "\n");

		if (plat_file_write(f, line, qstrlen(line)) != 0) ok = 0;
	}

	return file_end(path, f, ok);
}

static void profile_path(const char *folder, const char *leaf, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, QWARK_PROFILEDIR);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, folder);
	if (leaf != NULL) {
		qstrcat(out, cap, "/");
		qstrcat(out, cap, leaf);
	}
}

static int global_save(void)
{
	return store_save(g_kv, QWARK_CONFIG);
}

static int profile_save(void)
{
	char path[FILE_PATH_MAX];

	if (!g_prof.active || !g_prof.exists) return ST_OK;
	profile_path(g_prof.folder, "config.txt", path, sizeof(path));
	return store_save(g_pkv, path);
}

/* The active profile's config.txt into g_pkv; nothing for one never made. */
static void profile_load(void)
{
	char path[FILE_PATH_MAX];

	memset(g_pkv, 0, sizeof(g_pkv));
	g_version++;
	if (!g_prof.active || !g_prof.exists) return;
	profile_path(g_prof.folder, "config.txt", path, sizeof(path));
	store_load(g_pkv, path);   /* one too big to read leaves every key unset, as config.txt does */
}

int config_load(void)
{
	int rc = store_load(g_kv, QWARK_CONFIG);

	/*
	 * Revision 1.24. CONFIG_RELOAD re-reads the active profile with the
	 * console's file, and says so in the profile sequence. Whether the switch
	 * itself changed by hand is the XMB's to act on (session.c), never a game's.
	 */
	if (g_prof.active) {
		profile_load();
		g_prof.seq++;
	}

	if (rc != ST_OK) {
		plat_log_enable(1);   /* no file, so the default applies */
		return rc;
	}

	/*
	 * Every log line costs a cellFs open, write and close, so a user who does
	 * not want that pays nothing for it. Default on, since the module is new.
	 */
	plat_log_enable((int)config_get_u32("log", 1));

	/* savefile_helper = 0: never write the savefile helper into a game. */
	savefile_set_enabled((int)config_get_u32("savefile_helper", 1));

	return ST_OK;
}

/* Both files: the console's, and the active profile's when it has been made. */
int config_save(void)
{
	int rc = global_save();
	int rc2 = profile_save();
	return rc != ST_OK ? rc : rc2;
}

int config_init(void)
{
	plat_dir_create("/dev_hdd0/qwark");
	plat_dir_create(QWARK_POSDIR);
	plat_dir_create(QWARK_MODSDIR);
	plat_dir_create(QWARK_SAVEDIR);
	plat_trace("qwark:     config dirs ok");

	config_load();      /* a missing file is fine, we start empty */
	plat_trace("qwark:     config file read");
	return ST_OK;
}

/* ----------------------------------------------------------------- combos */

/*
 * Revision 1.22: the menu combo is the one with a mask of its own when the file
 * has never named it, COMBO_MENU_DEFAULT. A 0 written there turns it off like
 * any other, and stays off.
 */
u32 config_combo(u8 action)
{
	if (action >= COMBO_COUNT) return 0;
	return config_get_u32(g_combo_key[action],
	                      action == COMBO_OPEN_MENU ? COMBO_MENU_DEFAULT : 0);
}

int config_set_combo(u8 action, u32 mask)
{
	char buf[16];

	if (action >= COMBO_COUNT) return ST_BAD_ARG;

	buf[0] = '0';
	buf[1] = 'x';
	qfmt_hex(buf + 2, sizeof(buf) - 2, mask, 8);
	return config_set(g_combo_key[action], buf);
}

/*
 * Revision 1.12. A config.txt written before this switch existed, or one a user
 * has never touched it in, has no such key, and combos fire: the switch is only
 * ever off because somebody turned it off, and it stays off until somebody
 * turns it back on. Written as a plain 0 or 1, the way every other flag in the
 * file is, so it reads the same by hand as through COMBO_ENABLE.
 */
int config_combo_enabled(void)
{
	return config_get_u32(g_combo_enabled_key, 1) != 0;
}

int config_set_combo_enabled(u8 on)
{
	if (on > 1) return ST_BAD_ARG;
	return config_set_u32(g_combo_enabled_key, on);
}

/*
 * Revision 1.22. `menu.enabled`, the console menu's own switch: 1 for a file
 * that has never mentioned it. Nothing on the wire writes it; a user edits it,
 * and CONFIG_RELOAD or a console reboot picks it up.
 */
int config_menu_enabled(void)
{
	return config_get_u32("menu.enabled", 1) != 0;
}

/*
 * Revision 1.23. `menu.help`: 1 for a file that has never mentioned it, as the
 * menu took the Help row before the key existed. MENU_HELP writes it, a plain
 * 0 or 1 like `combo.enabled`, so it reads the same by hand.
 */
int config_menu_help(void)
{
	return config_get_u32("menu.help", 1) != 0;
}

int config_set_menu_help(u8 on)
{
	if (on > 1) return ST_BAD_ARG;
	return config_set_u32("menu.help", on);
}

/* -------------------------------------------------------------- selection */

u8 config_selected_slot(void)
{
	u32 v = config_get_u32("selected.slot", 0);
	return (u8)(v < QWARK_POS_SLOTS ? v : 0);
}

void config_set_selected_slot(u8 slot)
{
	config_set_u32("selected.slot", slot);
}

/*
 * Build 53. "selected.<game>.<field>", beside the manips' "manip.<game>.*": the
 * longest is "selected.rac4.planet_flags", 26 of CONFIG_KEY_MAX.
 */
static void selected_key(const char *game, const char *field, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, "selected.");
	qstrcat(out, cap, game);
	qstrcat(out, cap, ".");
	qstrcat(out, cap, field);
}

/*
 * The game's own key whenever the file holds one, as a relabelled toggle's auto
 * flag answers from its new label (config_feature_auto_former): a value that
 * does not parse is 0 then, not the old key's. Until then the key the console
 * shared before build 53, `former`, and 0 where neither is there.
 */
static u8 selected_get(const char *game, const char *field, const char *former)
{
	char key[CONFIG_KEY_MAX];

	if (game == NULL) return 0;

	selected_key(game, field, key, sizeof(key));
	if (config_get(key) != NULL) return (u8)config_get_u32(key, 0);
	return (u8)config_get_u32(former, 0);
}

u8 config_selected_planet(const char *game)
{
	return selected_get(game, "planet", "selected.planet");
}

u8 config_selected_planet_flags(const char *game)
{
	return selected_get(game, "planet_flags", "selected.planet_flags");
}

/*
 * Both of the game's own keys, every time, so a game that has made one
 * selection never reads either half of it from the old pair again. The old pair
 * is left as it is: it is what the games with no selection of their own still
 * answer.
 */
int config_set_selected_planet(const char *game, u8 planet, u8 flags)
{
	char key[CONFIG_KEY_MAX];
	int rc;

	if (game == NULL) return ST_BAD_ARG;

	selected_key(game, "planet", key, sizeof(key));
	rc = config_set_u32(key, planet);
	if (rc != ST_OK) return rc;

	selected_key(game, "planet_flags", key, sizeof(key));
	return config_set_u32(key, flags);
}

/* ------------------------------------------------------------- auto flags */

static void feature_key(const char *title, const char *label, char *out, u32 cap)
{
	char slug[48];

	qslug(label, slug, sizeof(slug));

	out[0] = 0;
	qstrcat(out, cap, title);
	qstrcat(out, cap, ".auto.");
	qstrcat(out, cap, slug);
}

static void mod_key(const char *title, const char *dirname, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, title);
	qstrcat(out, cap, ".mod.");
	qstrcat(out, cap, dirname);
	qstrcat(out, cap, ".auto");
}

u32 config_feature_auto(const char *title, const char *label)
{
	return config_feature_auto_default(title, label, 0);
}

/* Revision 1.24: the key's number as it stands, which a COLOR's re-apply needs. */
u32 config_feature_auto_default(const char *title, const char *label, int fallback)
{
	char key[CONFIG_KEY_MAX];
	feature_key(title, label, key, sizeof(key));
	return config_get_u32(key, fallback != 0);
}

u32 config_feature_auto_former(const char *title, const char *label, const char *former,
                               int fallback)
{
	char key[CONFIG_KEY_MAX];

	if (former != NULL) {
		feature_key(title, label, key, sizeof(key));
		if (config_get(key) == NULL)
			return config_feature_auto_default(title, former, fallback);
	}
	return config_feature_auto_default(title, label, fallback);
}

void config_set_feature_auto(const char *title, const char *label, u32 value)
{
	char key[CONFIG_KEY_MAX];
	feature_key(title, label, key, sizeof(key));
	config_set_u32(key, value);
}

int config_mod_auto(const char *title, const char *dirname)
{
	char key[CONFIG_KEY_MAX];
	mod_key(title, dirname, key, sizeof(key));
	return config_get_u32(key, 0) != 0;
}

void config_set_mod_auto(const char *title, const char *dirname, int on)
{
	char key[CONFIG_KEY_MAX];
	mod_key(title, dirname, key, sizeof(key));
	config_set_u32(key, on ? 1 : 0);
}

/* ---------------------------------------------------------- manip options */

static void manip_key(const char *game, const char *field, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, "manip.");
	qstrcat(out, cap, game);
	qstrcat(out, cap, ".");
	qstrcat(out, cap, field);
}

u32 config_manip(const char *game, const char *field, u32 fallback)
{
	char key[CONFIG_KEY_MAX];
	manip_key(game, field, key, sizeof(key));
	return config_get_u32(key, fallback);
}

int config_set_manip(const char *game, const char *field, u32 value)
{
	char key[CONFIG_KEY_MAX];
	manip_key(game, field, key, sizeof(key));
	return config_set_u32(key, value);
}

/* --------------------------------------------------------------- profiles */

/* The XMB's user numbers are eight digits, 00000001 on; 0 and a negative answer are nobody. */
static int user_valid(u32 user)
{
	return user != 0 && user <= 99999999u;
}

/* Eight digits, leading zeros: the way /dev_hdd0/home names the user's folder. */
static void user_digits(u32 user, char *out, u32 cap)
{
	char num[12];
	u32 n = qfmt_u32(num, sizeof(num), user);

	out[0] = 0;
	for (; n < 8; n++) qstrcat(out, cap, "0");
	qstrcat(out, cap, num);
}

void config_profile_folder_for(u32 user, char *out, u32 cap)
{
	char digits[12];

	out[0] = 0;
	if (!user_valid(user)) return;
	user_digits(user, digits, sizeof(digits));
	qstrcat(out, cap, "user-");
	qstrcat(out, cap, digits);
}

int config_profile_exists(u32 user)
{
	char folder[PROFILE_FOLDER_MAX];
	char path[FILE_PATH_MAX];

	config_profile_folder_for(user, folder, sizeof(folder));
	if (folder[0] == 0) return 0;
	profile_path(folder, "config.txt", path, sizeof(path));
	return plat_path_exists(path, NULL, NULL);
}

/*
 * The active profile's folder, its config.txt and its positions/, made the
 * first time anything is written into a profile that has none: it starts
 * empty, every per-user key at its default. Nothing for a profile that exists
 * or while none is active.
 */
static int profile_make(void)
{
	char path[FILE_PATH_MAX];

	if (!g_prof.active || g_prof.exists) return ST_OK;

	plat_dir_create(QWARK_PROFILEDIR);
	profile_path(g_prof.folder, NULL, path, sizeof(path));
	plat_dir_create(path);
	profile_path(g_prof.folder, "positions", path, sizeof(path));
	plat_dir_create(path);

	g_prof.exists = 1;
	if (profile_save() != ST_OK) {
		g_prof.exists = 0;
		return ST_IO_ERROR;
	}
	return ST_OK;
}

/*
 * PROFILE_ENABLE's COPY, into the profile just made: every per-user key of the
 * console's config.txt, and the console's four position files. The combos are
 * not copied: the console's are in force for every profile until it overrides
 * one. A position file that is not there, or does not read, is not copied.
 */
static void profile_copy_shared(void)
{
	static const char * const games[4] = { "rac1", "rac2", "rac3", "rac4" };
	char keep[sizeof(g_pos_title)];
	int i;

	for (i = 0; i < CONFIG_MAX_ENTRIES; i++) {
		if (!g_kv[i].used) continue;
		if (config_key_scope(g_kv[i].key) != CONFIG_SCOPE_USER) continue;
		kv_put(g_pkv, g_kv[i].key, g_kv[i].value);
	}
	profile_save();

	/*
	 * The slots, a game at a time, through the slot table itself: read with the
	 * console's folder in force and written with the profile's. The resolver
	 * reads the running game's back once this is done.
	 */
	qstrcpy(keep, sizeof(keep), g_pos_title);
	for (i = 0; i < 4; i++) {
		qstrcpy(g_pos_title, sizeof(g_pos_title), games[i]);
		g_prof.active = 0;
		pos_load_file();
		g_prof.active = 1;
		pos_save_file();
	}
	qstrcpy(g_pos_title, sizeof(g_pos_title), keep);
}

int config_profiles_enabled(void)
{
	return config_get_u32("profiles.enabled", 0) != 0;
}

int config_set_profiles_enabled(u8 on)
{
	if (on > 1) return ST_BAD_ARG;
	return config_set_u32("profiles.enabled", on);
}

int config_profile_resolve(u32 user, u8 setup)
{
	int want = plat_has_users() && config_profiles_enabled() && user_valid(user);
	int make = 0;

	if (want && setup != PROFILE_SETUP_NONE && !config_profile_exists(user)) make = 1;

	if (!make && want == g_prof.active && (!want || user == g_prof.user)) return 0;

	/* The slots go back to the file they came from before the folder changes. */
	pos_save_file();

	memset(g_pkv, 0, sizeof(g_pkv));
	g_prof.active = 0;
	g_prof.exists = 0;
	g_prof.user = 0;
	g_prof.folder[0] = 0;

	if (want) {
		g_prof.active = 1;
		g_prof.user = user;
		config_profile_folder_for(user, g_prof.folder, sizeof(g_prof.folder));
		g_prof.exists = (u8)config_profile_exists(user);

		if (make && profile_make() == ST_OK && setup == PROFILE_SETUP_COPY) profile_copy_shared();
		profile_load();
	}

	pos_load_file();
	g_prof.seq++;
	g_version++;

	plat_log("qwark: profile %s", want ? g_prof.folder : "off");
	return 1;
}

int config_profile_active(void)        { return g_prof.active; }
u32 config_profile_user(void)          { return g_prof.user; }
u8  config_profile_seq(void)           { return g_prof.seq; }

u8 config_profile_flags(void)
{
	u8 flags = 0;

	if (!plat_has_users()) return 0;
	flags |= SESSION_PROFILE_SUPPORTED;
	if (config_profiles_enabled()) flags |= SESSION_PROFILE_ON;
	if (g_prof.active)             flags |= SESSION_PROFILE_ACTIVE;
	return flags;
}

int config_combo_overridden(u8 action)
{
	if (action >= COMBO_COUNT || !g_prof.active) return 0;
	return kv_find(g_pkv, g_combo_key[action]) != NULL;
}

int config_set_combo_override(u8 action, int set, u32 mask)
{
	char buf[16];
	int rc;

	if (action >= COMBO_COUNT) return ST_BAD_ARG;
	if (!g_prof.active) return ST_NOT_FOUND;

	if (!set) {
		struct kv *e = kv_find(g_pkv, g_combo_key[action]);
		if (e == NULL) return ST_OK;
		memset(e, 0, sizeof(*e));
		g_version++;
		return profile_save();
	}

	rc = profile_make();
	if (rc != ST_OK) return rc;

	buf[0] = '0';
	buf[1] = 'x';
	qfmt_hex(buf + 2, sizeof(buf) - 2, mask, 8);
	rc = kv_put(g_pkv, g_combo_key[action], buf);
	if (rc != ST_OK) return rc;
	return profile_save();
}

/* -------------------------------------------------------------- positions */

/*
 * A planet is a u8 everywhere it is asked for, so it is one here too: a
 * hand-edited line naming a planet past 255 used to take a slot it could never
 * be fetched from again, and is skipped instead.
 */
struct pos_entry {
	u8  planet;
	u8  slot;
	u8  len;
	u8  used;
	u8  blob[QWARK_MAX_BLOB];
};

static struct pos_entry g_pos[POS_MAX_ENTRIES];

/*
 * Revision 1.24: the slots are the active profile's, in its own positions/
 * folder, and the console's while none is active.
 */
static void pos_dir(char *out, u32 cap)
{
	if (g_prof.active) profile_path(g_prof.folder, "positions", out, cap);
	else {
		out[0] = 0;
		qstrcat(out, cap, QWARK_POSDIR);
	}
}

static void pos_path(char *out, u32 cap)
{
	pos_dir(out, cap);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, g_pos_title);
	qstrcat(out, cap, ".txt");
}

static int pos_load_file(void)
{
	char path[128];
	char *text = qtext();
	char *cursor;
	char *line;

	memset(g_pos, 0, sizeof(g_pos));

	if (g_pos_title[0] == 0) return ST_OK;

	pos_path(path, sizeof(path));
	if (qfile_read(path, text, POS_TEXT_MAX, NULL) != ST_OK) return ST_OK;

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		char *eq;
		char *dot;
		char *key;
		char *value;
		u32 planet;
		u32 slot;
		int ok = 0;
		int i;

		line = qtrim(line);
		if (line[0] == 0 || line[0] == '#') continue;

		eq = line;
		while (*eq != 0 && *eq != '=') eq++;
		if (*eq != '=') continue;
		*eq = 0;
		key = qtrim(line);
		value = qtrim(eq + 1);

		dot = key;
		while (*dot != 0 && *dot != '.') dot++;
		if (*dot != '.') continue;
		*dot = 0;

		planet = qparse_u32(key, &ok);
		if (!ok || planet > 0xFFu) continue;
		slot = qparse_u32(dot + 1, &ok);
		if (!ok || slot >= QWARK_POS_SLOTS) continue;

		for (i = 0; i < POS_MAX_ENTRIES; i++) {
			if (g_pos[i].used) continue;
			g_pos[i].planet = (u8)planet;
			g_pos[i].slot   = (u8)slot;
			g_pos[i].len    = (u8)qhex_to_bytes(value, g_pos[i].blob, QWARK_MAX_BLOB);
			g_pos[i].used   = 1;
			break;
		}
	}

	return ST_OK;
}

int pos_save_file(void)
{
	char path[128];
	plat_file_t f;
	int i;

	int ok = 1;

	if (g_pos_title[0] == 0) return ST_OK;
	/* A profile never made has no folder, and nothing to save: a store makes it. */
	if (g_prof.active && !g_prof.exists) return ST_OK;

	pos_path(path, sizeof(path));
	if (file_begin(path, &f) != ST_OK) return ST_IO_ERROR;

	for (i = 0; i < POS_MAX_ENTRIES && ok; i++) {
		char line[16 + 2 * QWARK_MAX_BLOB + 8];
		char hex[2 * QWARK_MAX_BLOB + 2];
		char num[12];

		if (!g_pos[i].used) continue;

		qbytes_to_hex(g_pos[i].blob, g_pos[i].len, hex, sizeof(hex));

		line[0] = 0;
		qfmt_u32(num, sizeof(num), g_pos[i].planet);
		qstrcat(line, sizeof(line), num);
		qstrcat(line, sizeof(line), ".");
		qfmt_u32(num, sizeof(num), g_pos[i].slot);
		qstrcat(line, sizeof(line), num);
		qstrcat(line, sizeof(line), " = ");
		qstrcat(line, sizeof(line), hex);
		qstrcat(line, sizeof(line), "\n");

		if (plat_file_write(f, line, qstrlen(line)) != 0) ok = 0;
	}

	return file_end(path, f, ok);
}

int pos_use_title(const char *title)
{
	if (title != NULL && qstreq(g_pos_title, title)) return ST_OK;

	if (g_pos_title[0] != 0) pos_save_file();

	qstrcpy(g_pos_title, sizeof(g_pos_title), title ? title : "");
	return pos_load_file();
}

static struct pos_entry *pos_find(u8 planet, u8 slot)
{
	int i;
	for (i = 0; i < POS_MAX_ENTRIES; i++) {
		if (g_pos[i].used && g_pos[i].planet == planet && g_pos[i].slot == slot)
			return &g_pos[i];
	}
	return NULL;
}

int pos_store(u8 planet, u8 slot, const u8 *blob, u8 len)
{
	struct pos_entry *e;
	int i;

	if (slot >= QWARK_POS_SLOTS || len > QWARK_MAX_BLOB) return ST_BAD_ARG;

	/* Revision 1.24: a slot stored into a profile never made makes it. */
	{
		int rc = profile_make();
		if (rc != ST_OK) return rc;
	}

	e = pos_find(planet, slot);
	if (e == NULL) {
		for (i = 0; i < POS_MAX_ENTRIES; i++) {
			if (!g_pos[i].used) { e = &g_pos[i]; break; }
		}
		if (e == NULL) return ST_FULL;
		e->used   = 1;
		e->planet = planet;
		e->slot   = slot;
	}

	memset(e->blob, 0, sizeof(e->blob));
	memcpy(e->blob, blob, len);
	e->len = len;

	return pos_save_file();
}

int pos_fetch(u8 planet, u8 slot, u8 *blob, u8 *len)
{
	struct pos_entry *e = pos_find(planet, slot);

	if (e == NULL) return ST_NOT_FOUND;

	memcpy(blob, e->blob, e->len);
	*len = e->len;
	return ST_OK;
}

int pos_clear(u8 planet, u8 slot)
{
	struct pos_entry *e = pos_find(planet, slot);

	if (e == NULL) return ST_NOT_FOUND;

	memset(e, 0, sizeof(*e));
	return pos_save_file();
}
