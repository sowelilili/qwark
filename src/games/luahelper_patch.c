/*
 * Putting the Lua helper into a game: the RPCS3 patch words, the presence rule
 * and the console install (luahelper_patch.h).
 */
#include "luahelper_patch.h"
#include "../core/proto.h"
#include "../core/util.h"

static u32 cave_words(const struct lh_cave *c)
{
	return (c->len + 3u) / 4u;
}

u32 lh_patch_count(const struct lh_desc *d)
{
	u32 n = 0;
	u8 i;

	if (d == NULL || d->caves == NULL) return 0;
	for (i = 0; i < d->ncaves; i++) n += cave_words(&d->caves[i]);
	return n + LH_PATCH_IDENT_WORDS + d->nhooks;
}

/* The hooks in the order they go in: the error tap first, the VM hook last. */
static const struct lh_hook *hook_in_order(const struct lh_desc *d, u8 k)
{
	return &d->hooks[d->nhooks - 1u - k];
}

int lh_patch_build(const struct lh_desc *d, u8 *out, u32 cap, u32 *len)
{
	u32 n = lh_patch_count(d);
	u32 need, off = LH_PATCH_HEAD;
	u8 ident[8];
	u8 i;

	*len = 0;
	if (n == 0) return ST_UNSUPPORTED;
	need = LH_PATCH_HEAD + n * LH_PATCH_ROW + LH_PATCH_BYTES_HEAD;
	if (n > 0xFFFFu || need > cap) return ST_FULL;

	for (i = 0; i < d->ncaves; i++) {
		const struct lh_cave *c = &d->caves[i];
		u32 at, k;

		for (at = 0; at < c->len; at += 4u) {
			be32_put(out + off, c->addr + at);
			for (k = 0; k < 4u; k++) out[off + 4u + k] = at + k < c->len ? c->bytes[at + k] : 0;
			off += LH_PATCH_ROW;
		}
	}

	luamb_ident_bytes(ident);
	be32_put(out + off, d->mailbox);
	out[off + 4] = ident[0]; out[off + 5] = ident[1]; out[off + 6] = ident[2]; out[off + 7] = ident[3];
	off += LH_PATCH_ROW;
	be32_put(out + off, d->mailbox + 4u);
	out[off + 4] = ident[4]; out[off + 5] = ident[5]; out[off + 6] = ident[6]; out[off + 7] = ident[7];
	off += LH_PATCH_ROW;

	for (i = 0; i < d->nhooks; i++) {
		const struct lh_hook *h = hook_in_order(d, i);

		be32_put(out + off, h->addr);
		be32_put(out + off + 4u, h->value);
		off += LH_PATCH_ROW;
	}

	/* No single bytes. */
	be16_put(out + off, 0);
	be16_put(out + off + 2u, 0);
	off += LH_PATCH_BYTES_HEAD;

	be16_put(out, (u16)n);
	be16_put(out + 2, 0);
	be32_put(out + 4, qcrc32(out + LH_PATCH_HEAD, off - LH_PATCH_HEAD));
	*len = off;
	return ST_OK;
}

static int same(const struct luamb_io *io, u32 addr, const u8 *want, u32 len)
{
	u8 buf[64];
	u32 at = 0;

	while (at < len) {
		u32 take = len - at, k;

		if (take > sizeof(buf)) take = sizeof(buf);
		if (io->read(io->ctx, addr + at, buf, take) != 0) return -1;
		for (k = 0; k < take; k++) if (buf[k] != want[at + k]) return 0;
		at += take;
	}
	return 1;
}

int lh_present(const struct lh_desc *d, const struct luamb_io *io)
{
	u8 b[8];
	u8 i;
	int r;

	if (d == NULL || d->caves == NULL) return 0;
	for (i = 0; i < d->ncaves; i++) {
		r = same(io, d->caves[i].addr, d->caves[i].bytes, d->caves[i].len);
		if (r != 1) return r;
	}
	for (i = 0; i < d->nhooks; i++) {
		be32_put(b, d->hooks[i].value);
		r = same(io, d->hooks[i].addr, b, 4);
		if (r != 1) return r;
	}
	luamb_ident_bytes(b);
	return same(io, d->mailbox, b, 8);
}

static int put32(const struct luamb_io *io, u32 addr, u32 v)
{
	u8 b[4];

	be32_put(b, v);
	return io->write(io->ctx, addr, b, 4) == 0 ? ST_OK : ST_IO_ERROR;
}

int lh_install(const struct lh_desc *d, const struct luamb_io *io)
{
	u8 b[LUAMB_HDR_SIZE];
	u32 k;
	u8 i;

	if (d == NULL || d->caves == NULL) return ST_UNSUPPORTED;

	/* The executable must be the one the helper was built against. */
	for (i = 0; i < d->nhooks; i++) {
		u32 w;

		if (io->read(io->ctx, d->hooks[i].addr, b, 4) != 0) return ST_IO_ERROR;
		w = be32_get(b);
		if (w != d->hooks[i].orig && w != d->hooks[i].value) return ST_BAD_ARG;
	}

	/*
	 * The header first: zero, so no request is outstanding, then the identity
	 * the helper runs on. Nothing reaches the helper until the VM hook goes in.
	 */
	for (k = 0; k < LUAMB_HDR_SIZE; k++) b[k] = 0;
	luamb_ident_bytes(b);
	if (io->write(io->ctx, d->mailbox, b, LUAMB_HDR_SIZE) != 0) return ST_IO_ERROR;

	for (i = 0; i < d->ncaves; i++) {
		if (io->write(io->ctx, d->caves[i].addr, d->caves[i].bytes, d->caves[i].len) != 0)
			return ST_IO_ERROR;
	}
	for (i = 0; i < d->nhooks; i++) {
		const struct lh_hook *h = hook_in_order(d, i);
		int rc = put32(io, h->addr, h->value);

		if (rc != ST_OK) return rc;
	}
	return ST_OK;
}

int lh_uninstall(const struct lh_desc *d, const struct luamb_io *io)
{
	u8 i;

	if (d == NULL) return ST_UNSUPPORTED;
	for (i = 0; i < d->nhooks; i++) {
		int rc = put32(io, d->hooks[i].addr, d->hooks[i].orig);

		if (rc != ST_OK) return rc;
	}
	return ST_OK;
}
