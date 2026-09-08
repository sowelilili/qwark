#include "util.h"
#include "proto.h"

void qstrcpy(char *dst, u32 cap, const char *src)
{
	u32 i = 0;

	if (dst == NULL || cap == 0) return;
	if (src == NULL) { dst[0] = 0; return; }

	while (i + 1 < cap && src[i] != 0) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
}

void qstrcat(char *dst, u32 cap, const char *src)
{
	u32 n;

	if (dst == NULL || cap == 0 || src == NULL) return;

	n = 0;
	while (n < cap && dst[n] != 0) n++;
	if (n >= cap) return;

	qstrcpy(dst + n, cap - n, src);
}

u32 qstrlen(const char *s)
{
	u32 n = 0;
	if (s == NULL) return 0;
	while (s[n] != 0) n++;
	return n;
}

int qstreq(const char *a, const char *b)
{
	if (a == NULL || b == NULL) return a == b;
	while (*a != 0 && *a == *b) { a++; b++; }
	return *a == *b;
}

int qstrneq(const char *a, const char *b, u32 n)
{
	u32 i;
	if (a == NULL || b == NULL) return a == b;
	for (i = 0; i < n; i++) {
		if (a[i] != b[i]) return 0;
		if (a[i] == 0) return 1;
	}
	return 1;
}

char qlower(char c)
{
	if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
	return c;
}

int qstreq_ci(const char *a, const char *b)
{
	if (a == NULL || b == NULL) return a == b;
	while (*a != 0 && qlower(*a) == qlower(*b)) { a++; b++; }
	return qlower(*a) == qlower(*b);
}

static int qdigit(char c, u32 base, u32 *out)
{
	u32 v;

	if (c >= '0' && c <= '9')      v = (u32)(c - '0');
	else if (c >= 'a' && c <= 'f') v = (u32)(c - 'a') + 10;
	else if (c >= 'A' && c <= 'F') v = (u32)(c - 'A') + 10;
	else return 0;

	if (v >= base) return 0;
	*out = v;
	return 1;
}

u64 qparse_u64(const char *s, int *ok)
{
	u64 value = 0;
	u32 base = 10;
	int any = 0;

	if (ok) *ok = 0;
	if (s == NULL) return 0;

	while (*s == ' ' || *s == '\t') s++;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}

	for (;;) {
		u32 d;
		if (!qdigit(*s, base, &d)) break;
		value = value * (u64)base + (u64)d;
		any = 1;
		s++;
	}

	if (ok) *ok = any;
	return value;
}

u32 qparse_u32(const char *s, int *ok)
{
	return (u32)qparse_u64(s, ok);
}

void qfmt_hex(char *dst, u32 cap, u64 value, u32 digits)
{
	static const char hex[] = "0123456789abcdef";
	u32 i;

	if (dst == NULL || cap == 0) return;
	if (digits == 0) digits = 1;
	if (digits > 16) digits = 16;
	if (digits + 1 > cap) digits = cap - 1;

	for (i = 0; i < digits; i++) {
		u32 shift = (digits - 1 - i) * 4;
		dst[i] = hex[(value >> shift) & 0xF];
	}
	dst[digits] = 0;
}

u32 qfmt_u32(char *dst, u32 cap, u32 value)
{
	char tmp[12];
	u32 n = 0;
	u32 i;

	if (dst == NULL || cap == 0) return 0;

	do {
		tmp[n++] = (char)('0' + (value % 10));
		value /= 10;
	} while (value != 0 && n < sizeof(tmp));

	if (n + 1 > cap) n = cap - 1;

	for (i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
	dst[n] = 0;
	return n;
}

u32 qhex_to_bytes(const char *s, u8 *out, u32 cap)
{
	u32 n = 0;

	if (s == NULL || out == NULL) return 0;

	while (*s == ' ' || *s == '\t') s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	while (n < cap) {
		u32 hi, lo;
		if (!qdigit(s[0], 16, &hi)) break;
		if (!qdigit(s[1], 16, &lo)) break;
		out[n++] = (u8)((hi << 4) | lo);
		s += 2;
	}

	return n;
}

void qbytes_to_hex(const u8 *in, u32 len, char *out, u32 cap)
{
	static const char hex[] = "0123456789abcdef";
	u32 i;

	if (out == NULL || cap == 0) return;
	out[0] = 0;
	if (in == NULL) return;

	for (i = 0; i < len; i++) {
		if (2 * i + 2 >= cap) break;
		out[2 * i]     = hex[(in[i] >> 4) & 0xF];
		out[2 * i + 1] = hex[in[i] & 0xF];
	}

	if (2 * i < cap) out[2 * i] = 0;
	else out[cap - 1] = 0;
}

static int qspace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char *qtrim(char *s)
{
	u32 n;

	if (s == NULL) return s;

	while (*s != 0 && qspace(*s)) s++;

	n = qstrlen(s);
	while (n > 0 && qspace(s[n - 1])) {
		s[n - 1] = 0;
		n--;
	}

	return s;
}

void qslug(const char *src, char *dst, u32 cap)
{
	u32 n = 0;
	int pending = 0;

	if (dst == NULL || cap == 0) return;
	dst[0] = 0;
	if (src == NULL) return;

	while (*src != 0 && n + 1 < cap) {
		char c = *src++;
		int alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

		if (alnum) {
			if (pending && n > 0 && n + 1 < cap) dst[n++] = '_';
			pending = 0;
			if (n + 1 < cap) dst[n++] = qlower(c);
		} else {
			if (n > 0) pending = 1;
		}
	}

	dst[n] = 0;
}

/* ------------------------------------------------------------- text files */

int qread_file(const char *path, char *buf, u32 cap, u32 *len)
{
	plat_file_t f;
	u32 total = 0;

	if (len) *len = 0;
	if (buf == NULL || cap == 0) return ST_BAD_ARG;
	buf[0] = 0;

	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return ST_NOT_FOUND;

	for (;;) {
		u32 got = 0;
		if (total + 1 >= cap) { plat_file_close(f); return ST_FULL; }
		if (plat_file_read(f, buf + total, cap - 1 - total, &got) != 0) {
			plat_file_close(f);
			return ST_IO_ERROR;
		}
		if (got == 0) break;
		total += got;
	}

	plat_file_close(f);
	buf[total] = 0;
	if (len) *len = total;
	return ST_OK;
}

char *qnext_line(char **cursor)
{
	char *start;
	char *p;

	if (cursor == NULL || *cursor == NULL) return NULL;

	start = *cursor;
	if (*start == 0) { *cursor = NULL; return NULL; }

	p = start;
	while (*p != 0 && *p != 0x0A) p++;

	if (*p == 0x0A) {
		*p = 0;
		*cursor = p + 1;
	} else {
		*cursor = p;
	}

	/* The mod and config files on this project are all CRLF. */
	{
		u32 n = qstrlen(start);
		while (n > 0 && start[n - 1] == 0x0D) { start[n - 1] = 0; n--; }
	}

	return start;
}
