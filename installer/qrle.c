/*
 * qrle's expansion: see qrle.h for the code. The console's installer and the
 * host tests build this same file.
 */
#include "qrle.h"

#include <string.h>

/* The buffered piece, handed on when it is full and at the end. */
typedef struct out {
	unsigned char *buf;
	uint32_t cap, used;
	qrle_sink sink;
	void *ctx;
	int err;
} out;

static int flush(out *o)
{
	int e;

	if (o->used == 0) return 0;
	e = o->sink(o->ctx, o->buf, o->used);
	o->used = 0;
	if (e != 0) {
		o->err = e;
		return -1;
	}
	return 0;
}

static int put(out *o, const unsigned char *p, uint32_t n, int run)
{
	while (n > 0) {
		uint32_t room = o->cap - o->used, k = n < room ? n : room;

		if (run) memset(o->buf + o->used, p[0], k);
		else { memcpy(o->buf + o->used, p, k); p += k; }
		o->used += k;
		n -= k;
		if (o->used == o->cap && flush(o) != 0) return -1;
	}
	return 0;
}

int qrle_expand(const unsigned char *in, uint32_t in_len, uint32_t out_len,
                unsigned char *buf, uint32_t cap, qrle_sink sink, void *ctx, int *sink_err)
{
	out o;
	uint32_t pos = 0, left = out_len;

	if (sink_err) *sink_err = 0;
	if (buf == NULL || cap == 0 || sink == NULL || (in == NULL && in_len != 0)) return QRLE_BAD;
	o.buf = buf;
	o.cap = cap;
	o.used = 0;
	o.sink = sink;
	o.ctx = ctx;
	o.err = 0;

	while (pos < in_len) {
		uint32_t v = 0, n;
		int shift = 0, more = 1;

		/* The varint: five bytes at most, and nothing above bit 31. */
		while (more) {
			unsigned char b;

			if (pos >= in_len || shift > 28) return QRLE_BAD;
			b = in[pos++];
			if (shift == 28 && (b & 0x70u) != 0) return QRLE_BAD;
			v |= (uint32_t)(b & 0x7Fu) << shift;
			more = (b & 0x80u) != 0;
			shift += 7;
		}
		n = (v >> 1) + 1u;
		if (n == 0 || n > left) return QRLE_BAD;     /* n == 0: 0xFFFFFFFF + 1 wrapped */

		if (v & 1u) {
			if (pos >= in_len) return QRLE_BAD;
			if (put(&o, in + pos, n, 1) != 0) goto sink_failed;
			pos++;
		} else {
			if (n > in_len - pos) return QRLE_BAD;
			if (put(&o, in + pos, n, 0) != 0) goto sink_failed;
			pos += n;
		}
		left -= n;
	}
	if (left != 0) return QRLE_BAD;
	if (flush(&o) != 0) goto sink_failed;
	return QRLE_OK;

sink_failed:
	if (sink_err) *sink_err = o.err;
	return QRLE_SINK;
}
