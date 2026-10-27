/*
 * qwark's side of the Lua mailbox: the header codec and the request state
 * machine (luamb_client.h). luamb.h has the layout and the protocol; this file
 * is the half of it that runs outside the game.
 *
 * Linked where QWARK_HAS_TOD, for src/core/luacore.c. It has no state of its
 * own beyond the struct luamb_client it is handed (and luamb_read_out's
 * buffer), and it reaches the game only through struct luamb_io.
 */
#include "luamb_client.h"

/* ------------------------------------------------------------- the header */

void luamb_hdr_decode(const u8 *raw, struct luamb_hdr *h)
{
	h->magic      = be32_get(raw + LUAMB_OFF_MAGIC);
	h->version    = be16_get(raw + LUAMB_OFF_VERSION);
	h->size       = be16_get(raw + LUAMB_OFF_SIZE);
	h->req        = be32_get(raw + LUAMB_OFF_REQ);
	h->ack        = be32_get(raw + LUAMB_OFF_ACK);
	h->cancel     = be32_get(raw + LUAMB_OFF_CANCEL);
	h->busy       = be32_get(raw + LUAMB_OFF_BUSY);
	h->status     = (s32)be32_get(raw + LUAMB_OFF_STATUS);
	h->chunk_len  = be32_get(raw + LUAMB_OFF_CHUNK_LEN);
	h->out_len    = be32_get(raw + LUAMB_OFF_OUT_LEN);
	h->out_type   = raw[LUAMB_OFF_OUT_TYPE];
	h->out_flags  = raw[LUAMB_OFF_OUT_FLAGS];
	h->heartbeat  = be32_get(raw + LUAMB_OFF_HEARTBEAT);
	h->last_state = be32_get(raw + LUAMB_OFF_LAST_STATE);
	h->err_seq    = be32_get(raw + LUAMB_OFF_ERR_SEQ);
	h->err_vm     = be32_get(raw + LUAMB_OFF_ERR_VM);
	h->err_len    = be32_get(raw + LUAMB_OFF_ERR_LEN);
}

void luamb_hdr_encode(const struct luamb_hdr *h, u8 *raw)
{
	u32 i;

	for (i = 0; i < LUAMB_HDR_SIZE; i++) raw[i] = 0;
	be32_put(raw + LUAMB_OFF_MAGIC, h->magic);
	be16_put(raw + LUAMB_OFF_VERSION, h->version);
	be16_put(raw + LUAMB_OFF_SIZE, h->size);
	be32_put(raw + LUAMB_OFF_REQ, h->req);
	be32_put(raw + LUAMB_OFF_ACK, h->ack);
	be32_put(raw + LUAMB_OFF_CANCEL, h->cancel);
	be32_put(raw + LUAMB_OFF_BUSY, h->busy);
	be32_put(raw + LUAMB_OFF_STATUS, (u32)h->status);
	be32_put(raw + LUAMB_OFF_CHUNK_LEN, h->chunk_len);
	be32_put(raw + LUAMB_OFF_OUT_LEN, h->out_len);
	raw[LUAMB_OFF_OUT_TYPE] = h->out_type;
	raw[LUAMB_OFF_OUT_FLAGS] = h->out_flags;
	be32_put(raw + LUAMB_OFF_HEARTBEAT, h->heartbeat);
	be32_put(raw + LUAMB_OFF_LAST_STATE, h->last_state);
	be32_put(raw + LUAMB_OFF_ERR_SEQ, h->err_seq);
	be32_put(raw + LUAMB_OFF_ERR_VM, h->err_vm);
	be32_put(raw + LUAMB_OFF_ERR_LEN, h->err_len);
}

void luamb_ident_bytes(u8 out[8])
{
	be32_put(out, LUAMB_MAGIC);
	be16_put(out + 4, LUAMB_VERSION);
	be16_put(out + 6, LUAMB_SIZE);
}

int luamb_hdr_valid(const struct luamb_hdr *h)
{
	return h->magic == LUAMB_MAGIC && h->version == LUAMB_VERSION && h->size == LUAMB_SIZE;
}

u32 luamb_out_text(const u8 *out_area, u32 out_len, char *dst, u32 cap)
{
	u32 n = 0;

	if (cap == 0) return 0;
	if (out_len > LUAMB_OUT_CAP - 1) out_len = LUAMB_OUT_CAP - 1;
	while (n < out_len && n < cap - 1 && out_area[n] != 0) {
		dst[n] = (char)out_area[n];
		n++;
	}
	dst[n] = 0;
	return n;
}

/* ---------------------------------------------------------- the requests */

static int read_hdr(const struct luamb_client *c, const struct luamb_io *io, struct luamb_hdr *h)
{
	u8 raw[LUAMB_HDR_SIZE];

	if (io->read(io->ctx, c->base, raw, LUAMB_HDR_SIZE) != 0) return -1;
	luamb_hdr_decode(raw, h);
	return 0;
}

static int write_u32(const struct luamb_io *io, u32 addr, u32 v)
{
	u8 b[4];

	be32_put(b, v);
	return io->write(io->ctx, addr, b, 4);
}

void luamb_client_init(struct luamb_client *c, u32 base, u32 state_ptr)
{
	c->base = base;
	c->state_ptr = state_ptr;
	c->phase = LUAMB_C_IDLE;
	c->seq = 0;
	c->t_sent = 0;
	c->t_cancel = 0;
	c->hb_sent = 0;
	c->hung = 0;
}

/*
 * Withdraws c->seq: cancel first, then a fresh look at busy. The helper raises
 * busy before it reads cancel, so if busy is not seq now the helper is not in
 * the request and will cancel it when it sees it; if it is, the helper is
 * running it (or about to find the cancel) and its ack follows in the same
 * frame. Returns LUAMB_POLL_DONE with the real answer when the ack turned up in
 * between, LUAMB_POLL_IDLE when the slot is free, LUAMB_POLL_PENDING when the
 * helper still holds it.
 */
static int withdraw(struct luamb_client *c, const struct luamb_io *io, u32 now_ms,
                    struct luamb_hdr *h)
{
	if (write_u32(io, c->base + LUAMB_OFF_CANCEL, c->seq) != 0) return LUAMB_POLL_IO;
	if (read_hdr(c, io, h) != 0) return LUAMB_POLL_IO;

	if (h->ack == c->seq) {
		c->phase = LUAMB_C_IDLE;
		return LUAMB_POLL_DONE;
	}
	if (h->busy == c->seq) {
		c->phase = LUAMB_C_CANCELLING;
		c->t_cancel = now_ms;
		c->hung = 0;
		return LUAMB_POLL_PENDING;
	}
	c->phase = LUAMB_C_IDLE;
	return LUAMB_POLL_IDLE;
}

int luamb_attach(struct luamb_client *c, const struct luamb_io *io, u32 now_ms)
{
	struct luamb_hdr h;

	c->phase = LUAMB_C_IDLE;
	c->hung = 0;
	if (read_hdr(c, io, &h) != 0) return LUAMB_POLL_IO;
	c->seq = h.req;
	if (!luamb_hdr_valid(&h)) return LUAMB_POLL_IDLE;
	if (h.req == h.ack || h.cancel == h.req) return LUAMB_POLL_IDLE;

	/* An earlier session's request, never answered: it must not run late. */
	{
		int rc = withdraw(c, io, now_ms, &h);

		return rc == LUAMB_POLL_DONE ? LUAMB_POLL_IDLE : rc;
	}
}

int luamb_send(struct luamb_client *c, const struct luamb_io *io,
               const char *src, u32 len, u32 now_ms)
{
	struct luamb_hdr h;
	u8 b[4];
	u32 L, next;

	if (c->phase != LUAMB_C_IDLE) return LUAMB_SEND_BUSY;
	if (len == 0 || len > LUAMB_CHUNK_CAP) return LUAMB_SEND_TOO_LONG;
	if (read_hdr(c, io, &h) != 0) return LUAMB_SEND_IO;
	if (!luamb_hdr_valid(&h)) return LUAMB_SEND_NO_HELPER;

	/*
	 * The slot is free when the last request was answered, or withdrawn by
	 * us; a request nobody here sent (another client of the same game) is not
	 * ours to overwrite.
	 */
	if (h.req != h.ack && h.cancel != h.req) return LUAMB_SEND_BUSY;

	if (io->read(io->ctx, c->state_ptr, b, 4) != 0) return LUAMB_SEND_IO;
	L = be32_get(b);
	if (L == 0) return LUAMB_SEND_NO_STATE;

	next = h.req + 1u;
	if (next == h.cancel || next == h.ack) next++;   /* only ever after 2^32 requests */

	if (io->write(io->ctx, c->base + LUAMB_OFF_CHUNK, src, len) != 0) return LUAMB_SEND_IO;
	if (write_u32(io, c->base + LUAMB_OFF_CHUNK_LEN, len) != 0) return LUAMB_SEND_IO;
	/* req last: it is what the helper acts on. */
	if (write_u32(io, c->base + LUAMB_OFF_REQ, next) != 0) return LUAMB_SEND_IO;

	c->phase = LUAMB_C_SENT;
	c->seq = next;
	c->t_sent = now_ms;
	c->hb_sent = h.heartbeat;
	c->hung = 0;
	return LUAMB_SEND_OK;
}

static void fill(struct luamb_result *res, u32 seq, const struct luamb_hdr *h)
{
	res->seq = seq;
	res->status = h->status;
	res->out_len = h->out_len;
	res->out_type = h->out_type;
	res->out_flags = h->out_flags;
}

int luamb_poll(struct luamb_client *c, const struct luamb_io *io, u32 now_ms,
               struct luamb_result *res)
{
	struct luamb_hdr h;
	int rc;

	if (c->phase == LUAMB_C_IDLE) return LUAMB_POLL_IDLE;
	if (read_hdr(c, io, &h) != 0) return LUAMB_POLL_IO;

	if (c->phase == LUAMB_C_CANCELLING) {
		/* The answer to a request already reported as timed out: dropped. */
		if (h.ack == c->seq || h.busy != c->seq) {
			c->phase = LUAMB_C_IDLE;
			c->hung = 0;
			return LUAMB_POLL_IDLE;
		}
		if ((u32)(now_ms - c->t_cancel) >= LUAMB_T_HUNG_MS) c->hung = 1;
		return LUAMB_POLL_PENDING;
	}

	/* SENT */
	if (h.ack == c->seq) {
		c->phase = LUAMB_C_IDLE;
		fill(res, c->seq, &h);
		return LUAMB_POLL_DONE;
	}
	if ((u32)(now_ms - c->t_sent) < LUAMB_T_ANSWER_MS) return LUAMB_POLL_PENDING;

	{
		u32 hb = h.heartbeat;

		rc = withdraw(c, io, now_ms, &h);
		if (rc == LUAMB_POLL_IO) return rc;
		if (rc == LUAMB_POLL_DONE) {
			fill(res, c->seq, &h);
			return LUAMB_POLL_DONE;
		}
		/*
		 * No answer in time. With frames running it is a timeout; with the
		 * heartbeat standing still, no gameplay frame ran at all (paused, a
		 * menu, a load), which the client can say in so many words.
		 */
		res->seq = c->seq;
		res->status = hb != c->hb_sent ? LUAMB_ST_TIMEOUT : LUAMB_ST_NOT_RUNNING;
		res->out_len = 0;
		res->out_type = LUAMB_T_NONE;
		res->out_flags = 0;
		return LUAMB_POLL_DONE;
	}
}

int luamb_read_out(const struct luamb_client *c, const struct luamb_io *io,
                   const struct luamb_result *res, char *dst, u32 cap)
{
	static u8 area[LUAMB_OUT_CAP];
	u32 want = res->out_len + 1u;

	if (cap == 0) return -1;
	dst[0] = 0;
	if (res->status >= LUAMB_ST_TIMEOUT) return 0;
	if (want > LUAMB_OUT_CAP) want = LUAMB_OUT_CAP;
	if (io->read(io->ctx, c->base + LUAMB_OFF_OUT, area, want) != 0) return -1;
	return (int)luamb_out_text(area, res->out_len, dst, cap);
}
