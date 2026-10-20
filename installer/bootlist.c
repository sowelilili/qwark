/*
 * The boot_plugins.txt edit: see bootlist.h for the rules, which are the PC
 * client's. Everything works on lengths rather than on NUL-terminated strings,
 * since a list is whatever bytes the file holds.
 */
#include "bootlist.h"

#include <string.h>

/* The white space .NET's Trim() takes off a line, as far as ASCII goes. */
static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* One line, trimmed: [*start, *start + *len). */
static void trim(const char *line, size_t len, const char **start, size_t *tlen)
{
	while (len > 0 && is_space(line[0])) { line++; len--; }
	while (len > 0 && is_space(line[len - 1])) len--;
	*start = line;
	*tlen = len;
}

static int ends_with_nocase(const char *s, size_t len, const char *tail)
{
	size_t n = strlen(tail);
	size_t i;

	if (len < n) return 0;
	for (i = 0; i < n; i++) {
		if (lower(s[len - n + i]) != lower(tail[i])) return 0;
	}
	return 1;
}

static int equals_nocase(const char *s, size_t len, const char *other)
{
	size_t n = strlen(other);
	size_t i;

	if (len != n) return 0;
	for (i = 0; i < n; i++) {
		if (lower(s[i]) != lower(other[i])) return 0;
	}
	return 1;
}

/*
 * Walks the list a line at a time. *pos is where the next piece starts; the
 * line comes back without its '\n' and without its trailing '\r's. Returns 0
 * once the list is used up. A piece that is all white space is skipped here,
 * so the caller sees only plugin lines.
 */
static int next_line(const char *in, size_t inlen, size_t *pos, const char **line, size_t *len)
{
	while (*pos <= inlen) {
		const char *piece = in + *pos;
		const char *nl = (*pos < inlen) ? (const char *)memchr(piece, '\n', inlen - *pos) : NULL;
		size_t plen = nl ? (size_t)(nl - piece) : inlen - *pos;
		const char *t;
		size_t tlen;

		/* .NET's Split('\n') yields one more piece after a final '\n': an empty one. */
		*pos += plen + 1;

		while (plen > 0 && piece[plen - 1] == '\r') plen--;
		trim(piece, plen, &t, &tlen);
		if (tlen == 0) continue;

		*line = piece;
		*len = plen;
		return 1;
	}
	return 0;
}

static int names_qwark(const char *line, size_t len)
{
	const char *t;
	size_t tlen;

	trim(line, len, &t, &tlen);
	return ends_with_nocase(t, tlen, BOOTLIST_SPRX_NAME);
}

static int put(char *out, size_t outcap, size_t *at, const char *s, size_t len)
{
	if (len > outcap - *at) return -1;
	memcpy(out + *at, s, len);
	*at += len;
	return 0;
}

int bootlist_plan_edit(const char *in, size_t inlen, char *out, size_t outcap, bootlist_plan *plan)
{
	const char *line;
	const char *t;
	size_t len, tlen, pos, at;
	int lines = 0, listed = 0, first_is_boot = 0;

	memset(plan, 0, sizeof(*plan));
	if (in == NULL) {
		in = "";
		inlen = 0;
	}

	/* First pass: what the list holds. */
	pos = 0;
	while (next_line(in, inlen, &pos, &line, &len)) {
		if (lines == 0) {
			trim(line, len, &t, &tlen);
			first_is_boot = equals_nocase(t, tlen, BOOTLIST_BOOT_PATH);
		}
		if (names_qwark(line, len)) listed++;
		lines++;
	}

	if (listed == 1 && first_is_boot) {
		plan->outcome = BOOTLIST_ALREADY_FIRST;
		plan->lines = lines;
		return 0;
	}

	/* Second pass: the boot path, then everything that is not qwark. */
	at = 0;
	if (out == NULL) outcap = 0;
	if (put(out, outcap, &at, BOOTLIST_BOOT_PATH "\n", sizeof(BOOTLIST_BOOT_PATH)) != 0) return -1;
	plan->lines = 1;

	pos = 0;
	while (next_line(in, inlen, &pos, &line, &len)) {
		if (names_qwark(line, len)) {
			plan->dropped++;
			continue;
		}
		if (put(out, outcap, &at, line, len) != 0 || put(out, outcap, &at, "\n", 1) != 0) return -1;
		plan->lines++;
	}

	plan->outcome = listed == 0 ? BOOTLIST_ADDED : BOOTLIST_MOVED;
	plan->len = at;
	return 0;
}
