/*
 * qrle: the run-length code the installer package stores its premade
 * savefiles and mods in (installer/pkg.py writes it, install.c expands it).
 * A PS3 package is not compressed, and a savefile is mostly long runs of one
 * byte (zero padding in all of them, 0xFF fill in Deadlocked's), which this
 * takes from 8.7 MB to about 230 KB. The SDK has no zlib a PPU program can
 * link (only Edge's, which runs on the SPUs), and this needs none.
 *
 * The stream is a sequence of tokens. Each starts with a varint v, seven bits
 * a byte, least significant group first, the top bit set on every byte but the
 * last, at most five bytes and at most 0xFFFFFFFF. Its low bit is the kind and
 * the rest, plus one, the count n:
 *
 *   v & 1 == 0   n literal bytes follow
 *   v & 1 == 1   one byte follows, which stands for n copies of itself
 *
 * A stream expands to exactly the size its entry gives and ends exactly at its
 * last byte; anything else, a token past either end included, is a bad stream.
 * pkg.py has the same rules in Python (qrle_encode, qrle_decode), and
 * installer/test/test_pkgdata.py holds the two to each other.
 */
#ifndef QWARK_INSTALLER_QRLE_H
#define QWARK_INSTALLER_QRLE_H

#include <stdint.h>

#define QRLE_OK     0
#define QRLE_BAD    (-1)    /* the stream is not one that expands to out_len bytes */
#define QRLE_SINK   (-2)    /* the sink refused a piece; *sink_err is what it answered */

/* Takes the next n expanded bytes, 0 < n <= cap. Any answer but 0 stops the expansion. */
typedef int (*qrle_sink)(void *ctx, const unsigned char *p, uint32_t n);

/*
 * Expands in[0..in_len) into out_len bytes, handed to sink in pieces of at most
 * cap bytes through buf. Nothing is handed on past the first bad token, but the
 * pieces before it are: a caller that must not write a bad file expands once
 * with a sink that only sums, and writes on a second pass.
 */
int qrle_expand(const unsigned char *in, uint32_t in_len, uint32_t out_len,
                unsigned char *buf, uint32_t cap, qrle_sink sink, void *ctx, int *sink_err);

#endif
