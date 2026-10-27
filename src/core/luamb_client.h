/*
 * qwark's side of the Lua mailbox (luamb.h): the header codec and the request
 * state machine that LUA_EXEC will run on the tick thread in phase 2
 * (docs/tod-poc.md section 2).
 *
 * Phase 1: nothing in qwark.sprx or the host executables links this yet; the
 * unit tests in test/test_tod.c do. It reaches game memory only through the
 * two callbacks in struct luamb_io, which phase 2 points at mem_read and
 * mem_write, so the rule that only the tick thread touches memory holds as
 * long as only the tick thread calls in.
 */
#ifndef QWARK_LUAMB_CLIENT_H
#define QWARK_LUAMB_CLIENT_H

#include "../plat/plat.h"
#include "luamb.h"

/* ------------------------------------------------------------- the header */

struct luamb_hdr {
	u32 magic;
	u16 version;
	u16 size;
	u32 req;
	u32 ack;
	u32 cancel;
	u32 busy;
	s32 status;
	u32 chunk_len;
	u32 out_len;
	u8  out_type;
	u8  out_flags;
	u32 heartbeat;
	u32 last_state;
	u32 err_seq;
	u32 err_vm;
	u32 err_len;
};

void luamb_hdr_decode(const u8 *raw, struct luamb_hdr *h);   /* LUAMB_HDR_SIZE bytes */
void luamb_hdr_encode(const struct luamb_hdr *h, u8 *raw);   /* the same, for tests and installs */

/* The first eight bytes an install writes: magic, version, size. */
void luamb_ident_bytes(u8 out[8]);

/* 1 when the header says a helper of this layout owns the block. */
int  luamb_hdr_valid(const struct luamb_hdr *h);

/*
 * The text in `out` (LUAMB_OUT_CAP bytes as read from the game) as a C string
 * in dst: at most cap - 1 bytes, always ended, and never past out_len or the
 * first NUL, whichever comes first, whatever the game left there. Returns the
 * length.
 */
u32  luamb_out_text(const u8 *out_area, u32 out_len, char *dst, u32 cap);

/* ---------------------------------------------------------- the requests */

struct luamb_io {
	int (*read)(void *ctx, u32 addr, void *buf, u32 len);    /* 0 on success */
	int (*write)(void *ctx, u32 addr, const void *buf, u32 len);
	void *ctx;
};

/*
 * How long qwark waits. A gameplay frame is at most 33 ms, so an answer takes
 * a frame or two when the game is playing; ANSWER covers a hitch. HUNG is how
 * long a request the helper has started may run before qwark calls the game
 * hung (an endless loop in a chunk hangs the game's thread, and nothing qwark
 * does can stop it).
 */
#define LUAMB_T_ANSWER_MS 1500u
#define LUAMB_T_HUNG_MS   5000u

/* The client's phases. */
#define LUAMB_C_IDLE       0   /* no request outstanding: req == ack, or the last was withdrawn */
#define LUAMB_C_SENT       1   /* waiting for ack == seq */
#define LUAMB_C_CANCELLING 2   /* withdrawn while the helper was inside it: waiting for its ack */

/* What luamb_send answers. */
#define LUAMB_SEND_OK        0
#define LUAMB_SEND_BUSY      1  /* a request is outstanding or being withdrawn */
#define LUAMB_SEND_NO_HELPER 2  /* no valid header: the helper is not installed */
#define LUAMB_SEND_NO_STATE  3  /* the game has no lua_State: between levels, or not in a level */
#define LUAMB_SEND_TOO_LONG  4  /* empty, or over LUAMB_CHUNK_CAP */
#define LUAMB_SEND_IO        5  /* a read or write failed */

/* What luamb_poll answers. */
#define LUAMB_POLL_IDLE    0   /* nothing outstanding */
#define LUAMB_POLL_PENDING 1   /* still waiting */
#define LUAMB_POLL_DONE    2   /* *res holds the answer */
#define LUAMB_POLL_IO      3   /* a read failed; try again next tick */

struct luamb_result {
	u32 seq;
	s32 status;       /* LUAMB_ST_*, including TIMEOUT and NOT_RUNNING from this side */
	u32 out_len;      /* valid when the helper answered */
	u8  out_type;
	u8  out_flags;
};

struct luamb_client {
	u32 base;         /* the mailbox */
	u32 state_ptr;    /* the address of the game's lua_State* (vm + vm_state) */
	u8  phase;
	u32 seq;          /* the request outstanding or withdrawn */
	u32 t_sent;
	u32 t_cancel;
	u32 hb_sent;      /* the helper's heartbeat when the request went out */
	u8  hung;         /* CANCELLING for longer than LUAMB_T_HUNG_MS */
};

void luamb_client_init(struct luamb_client *c, u32 base, u32 state_ptr);

/*
 * Takes up whatever an earlier session left in the mailbox: a request still
 * outstanding (req != ack) is withdrawn, so it can never run late. Call it on
 * every attach before the first send. LUAMB_POLL_IO on a failed read.
 */
int  luamb_attach(struct luamb_client *c, const struct luamb_io *io, u32 now_ms);

int  luamb_send(struct luamb_client *c, const struct luamb_io *io,
                const char *src, u32 len, u32 now_ms);

int  luamb_poll(struct luamb_client *c, const struct luamb_io *io, u32 now_ms,
                struct luamb_result *res);

/* The text of the answer luamb_poll just gave, read out of the mailbox. */
int  luamb_read_out(const struct luamb_client *c, const struct luamb_io *io,
                    const struct luamb_result *res, char *dst, u32 cap);

#endif /* QWARK_LUAMB_CLIENT_H */
