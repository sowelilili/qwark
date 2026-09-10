/*
 * Small string and number helpers.
 *
 * The PS3 build links against the VSH allocator and a hand-rolled printf, so
 * there is no strtoul, no atoi and no sscanf. Everything the core needs to parse
 * or format lives here so both builds behave identically.
 */
#ifndef QWARK_UTIL_H
#define QWARK_UTIL_H

#include "../plat/plat.h"

/* Copies at most cap-1 bytes and always terminates. */
void qstrcpy(char *dst, u32 cap, const char *src);

/* Appends, always terminates. */
void qstrcat(char *dst, u32 cap, const char *src);

u32  qstrlen(const char *s);
int  qstreq(const char *a, const char *b);
int  qstrneq(const char *a, const char *b, u32 n);

/* Case-insensitive compare over ASCII. */
int  qstreq_ci(const char *a, const char *b);

char qlower(char c);

/*
 * Parses a decimal or 0x-prefixed hexadecimal number. Leading whitespace is
 * skipped, trailing text ends the number. ok is set to 0 when no digit was
 * found. Never reads past a NUL.
 */
u32  qparse_u32(const char *s, int *ok);
u64  qparse_u64(const char *s, int *ok);

/* Writes value as 0-padded hex of exactly `digits` characters plus a NUL. */
void qfmt_hex(char *dst, u32 cap, u64 value, u32 digits);

/* Writes value in decimal. Returns the number of characters written. */
u32  qfmt_u32(char *dst, u32 cap, u32 value);

/* Hex blob helpers: "0a1b2c" <-> bytes. Return the number of bytes handled. */
u32  qhex_to_bytes(const char *s, u8 *out, u32 cap);
void qbytes_to_hex(const u8 *in, u32 len, char *out, u32 cap);

/* Trims ASCII whitespace in place, returns the new start. */
char *qtrim(char *s);

/* ------------------------------------------------------------------- CRC32 */

/*
 * CRC-32, the ordinary reflected one that zlib, PNG and the PC client's
 * Crc32.cs all compute, so a sum written on the console and a sum computed on
 * the PC agree byte for byte.
 *
 * It is a running state so a 2 MB save can be summed as it goes past in 64 KB
 * chunks without ever being held whole. The table is sixteen entries and takes
 * two lookups a byte, which is a compromise on purpose: a full 256-entry table
 * is a kilobyte of the module and the bit-at-a-time loop is eight shifts a byte,
 * which is too slow to sit on the tick thread.
 */
u32  qcrc32_start(void);
u32  qcrc32_update(u32 state, const u8 *data, u32 len);
u32  qcrc32_finish(u32 state);

/* The whole of a buffer in one call, for a caller that has it all already. */
u32  qcrc32(const u8 *data, u32 len);

/*
 * Lowercases, replaces every run of non-alphanumeric characters with a single
 * underscore, and trims leading/trailing underscores. Used to turn a feature
 * label into a config key ("Infinite ammo" -> "infinite_ammo").
 */
void qslug(const char *src, char *dst, u32 cap);

/* ------------------------------------------------------------- text files */

/* Reads a whole file, NUL-terminates it. Returns ST_OK / ST_NOT_FOUND / ST_FULL. */
int qread_file(const char *path, char *buf, u32 cap, u32 *len);

/*
 * Splits a NUL-terminated buffer into lines in place. Pass the address of a
 * cursor initialised to the start of the buffer; returns the next line with its
 * terminator removed, or NULL at the end. Handles LF and CRLF.
 */
char *qnext_line(char **cursor);

#endif /* QWARK_UTIL_H */
