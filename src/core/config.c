#include "config.h"
#include "util.h"
#include "net.h"
#include "savefile.h"
#include "../plat/plat.h"

#include <string.h>

#define CONFIG_MAX_ENTRIES 128
#define CONFIG_TEXT_MAX    16384
#define POS_MAX_ENTRIES    64
#define POS_TEXT_MAX       8192

struct kv {
	char key[CONFIG_KEY_MAX];
	char value[CONFIG_VAL_MAX];
	u8   used;
};

static struct kv g_kv[CONFIG_MAX_ENTRIES];
static u32 g_version;
static char g_text[CONFIG_TEXT_MAX];

static const char * const g_combo_key[COMBO_COUNT] = {
	"combo.save",
	"combo.load",
	"combo.die",
	"combo.planet",
	"combo.setaside"
};

/* ------------------------------------------------------------------ store */

static struct kv *kv_find(const char *key)
{
	int i;
	for (i = 0; i < CONFIG_MAX_ENTRIES; i++) {
		if (g_kv[i].used && qstreq(g_kv[i].key, key)) return &g_kv[i];
	}
	return NULL;
}

const char *config_get(const char *key)
{
	struct kv *e = kv_find(key);
	return e ? e->value : NULL;
}

static int kv_put(const char *key, const char *value)
{
	struct kv *e = kv_find(key);
	int i;

	if (e == NULL) {
		for (i = 0; i < CONFIG_MAX_ENTRIES; i++) {
			if (!g_kv[i].used) { e = &g_kv[i]; break; }
		}
		if (e == NULL) return ST_FULL;
		e->used = 1;
		qstrcpy(e->key, sizeof(e->key), key);
	}

	qstrcpy(e->value, sizeof(e->value), value);
	g_version++;
	return ST_OK;
}

u32 config_version(void)
{
	return g_version;
}

int config_set(const char *key, const char *value)
{
	int rc = kv_put(key, value);
	if (rc != ST_OK) return rc;
	return config_save();
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

int config_load(void)
{
	char *cursor;
	char *line;
	u32 len = 0;
	int rc;

	memset(g_kv, 0, sizeof(g_kv));
	g_version++;

	rc = qread_file(QWARK_CONFIG, g_text, sizeof(g_text), &len);
	if (rc != ST_OK) {
		plat_log_enable(1);   /* no file, so the default applies */
		return rc;
	}

	cursor = g_text;
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
		kv_put(key, value);
	}

	/*
	 * Every log line costs a cellFs open, write and close, so a user who does
	 * not want that pays nothing for it. Default on, since the module is new.
	 */
	plat_log_enable((int)config_get_u32("log", 1));

	/*
	 * Two switches for a console that is crashing and will not say why.
	 *
	 *   trace_ops = 1        every request in the log, so the last line before a
	 *                        crash names the operation it happened in
	 *   savefile_helper = 0  never write the helper into a game. It is the
	 *                        largest write qwark makes, so turning it off and
	 *                        seeing whether the crashes stop is worth more than
	 *                        another theory about it.
	 */
	net_set_trace_ops((int)config_get_u32("trace_ops", 0));
	net_set_telemetry((int)config_get_u32("telemetry", 1));
	savefile_set_enabled((int)config_get_u32("savefile_helper", 1));

	return ST_OK;
}

int config_save(void)
{
	plat_file_t f;
	int i;

	if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) != 0) return ST_IO_ERROR;

	for (i = 0; i < CONFIG_MAX_ENTRIES; i++) {
		char line[CONFIG_KEY_MAX + CONFIG_VAL_MAX + 8];
		if (!g_kv[i].used) continue;

		line[0] = 0;
		qstrcat(line, sizeof(line), g_kv[i].key);
		qstrcat(line, sizeof(line), " = ");
		qstrcat(line, sizeof(line), g_kv[i].value);
		qstrcat(line, sizeof(line), "\n");

		if (plat_file_write(f, line, qstrlen(line)) != 0) {
			plat_file_close(f);
			return ST_IO_ERROR;
		}
	}

	plat_file_close(f);
	return ST_OK;
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

u32 config_combo(u8 action)
{
	if (action >= COMBO_COUNT) return 0;
	return config_get_u32(g_combo_key[action], 0);
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

/* -------------------------------------------------------------- selection */

u8 config_selected_slot(void)
{
	u32 v = config_get_u32("selected.slot", 0);
	return (u8)(v < QWARK_POS_SLOTS ? v : 0);
}

u8 config_selected_planet(void)
{
	return (u8)config_get_u32("selected.planet", 0);
}

u8 config_selected_planet_flags(void)
{
	return (u8)config_get_u32("selected.planet_flags", 0);
}

void config_set_selected_slot(u8 slot)
{
	config_set_u32("selected.slot", slot);
}

void config_set_selected_planet(u8 planet, u8 flags)
{
	config_set_u32("selected.planet", planet);
	config_set_u32("selected.planet_flags", flags);
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

int config_feature_auto(const char *title, const char *label)
{
	return config_feature_auto_default(title, label, 0);
}

int config_feature_auto_default(const char *title, const char *label, int fallback)
{
	char key[CONFIG_KEY_MAX];
	feature_key(title, label, key, sizeof(key));
	return config_get_u32(key, fallback ? 1 : 0) != 0;
}

void config_set_feature_auto(const char *title, const char *label, int on)
{
	char key[CONFIG_KEY_MAX];
	feature_key(title, label, key, sizeof(key));
	config_set_u32(key, on ? 1 : 0);
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

/* -------------------------------------------------------------- positions */

struct pos_entry {
	u32 planet;
	u8  slot;
	u8  len;
	u8  used;
	u8  blob[QWARK_MAX_BLOB];
};

static struct pos_entry g_pos[POS_MAX_ENTRIES];
static char g_pos_title[16];
static char g_pos_text[POS_TEXT_MAX];

static void pos_path(char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, QWARK_POSDIR);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, g_pos_title);
	qstrcat(out, cap, ".txt");
}

static int pos_load_file(void)
{
	char path[128];
	char *cursor;
	char *line;

	memset(g_pos, 0, sizeof(g_pos));

	if (g_pos_title[0] == 0) return ST_OK;

	pos_path(path, sizeof(path));
	if (qread_file(path, g_pos_text, sizeof(g_pos_text), NULL) != ST_OK) return ST_OK;

	cursor = g_pos_text;
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
		if (!ok) continue;
		slot = qparse_u32(dot + 1, &ok);
		if (!ok || slot >= QWARK_POS_SLOTS) continue;

		for (i = 0; i < POS_MAX_ENTRIES; i++) {
			if (g_pos[i].used) continue;
			g_pos[i].planet = planet;
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

	if (g_pos_title[0] == 0) return ST_OK;

	pos_path(path, sizeof(path));
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return ST_IO_ERROR;

	for (i = 0; i < POS_MAX_ENTRIES; i++) {
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

		if (plat_file_write(f, line, qstrlen(line)) != 0) {
			plat_file_close(f);
			return ST_IO_ERROR;
		}
	}

	plat_file_close(f);
	return ST_OK;
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
