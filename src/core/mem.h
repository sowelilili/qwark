/*
 * The gated memory primitives plus the freeze, watch and patch tables.
 *
 * Everything in here is touched from the tick thread only, except the LIST
 * helpers, which network threads call under the core lock.
 */
#ifndef QWARK_MEM_H
#define QWARK_MEM_H

#include "proto.h"

/* ------------------------------------------------------------------ the gate */

/*
 * The session tells the gate which process is live and whether we are INGAME.
 * mem_read/mem_write refuse everything else, so no PS3MAPI call can ever go out
 * against a dead PID.
 */
void mem_set_context(u32 pid, int ingame);

/* How many reads and writes have reached the platform, for the op trace. */
u32  mem_read_calls(void);
u32  mem_write_calls(void);
int  mem_is_ingame(void);
u32  mem_pid(void);

int  mem_read(u32 addr, void *buf, u32 len);
int  mem_write(u32 addr, const void *buf, u32 len);

/* Convenience wrappers. Values are big-endian in game memory, as on hardware. */
int  mem_read_u32(u32 addr, u32 *out);
int  mem_read_u8(u32 addr, u8 *out);
int  mem_write_u32(u32 addr, u32 value);
int  mem_write_u8(u32 addr, u8 value);
int  mem_write_fill(u32 addr, u8 value, u32 len);
int  mem_write_zeros(u32 addr, u32 len);

/* --------------------------------------------------------------- watch table */

struct watch_entry {
	u32 addr;
	u8  size;
	u8  used;
	u8  valid;
	u64 value;
};

int  watch_add(u32 addr, u8 size, u8 *id_out);
int  watch_remove(u8 id);
void watch_clear(void);
void watch_invalidate(void);
const struct watch_entry *watch_slot(u8 id);
void watch_tick(void);

/* -------------------------------------------------------------- freeze table */

struct freeze_entry {
	u32 addr;
	u64 value;
	u8  size;
	u8  used;
};

int  freeze_add(u32 addr, u8 size, u64 value, u8 *id_out);
int  freeze_remove(u8 id);
int  freeze_find(u32 addr, u8 size);
void freeze_clear(void);
const struct freeze_entry *freeze_slot(u8 id);
u64  freeze_mask(void);
void freeze_tick(void);

/* --------------------------------------------------------------- patch table */

struct patch_word {
	u32 addr;
	u32 value;
};

struct patch_def {
	const char *name;
	u8 kind;                        /* PATCH_KIND_CLIENT / FEATURE / MOD */
	const struct patch_word *words;
	u16 count;
	u32 *originals;                 /* count words, filled on the first apply */
};

/*
 * Apply captures the original words from the live process the first time only:
 * a second apply is a no-op that must not re-capture, or a patched word ends up
 * recorded as the original (the RaC2 fast-load bug in BUGS.md).
 *
 * Nothing pauses the game around either. Contiguous words go out as one write,
 * and a branch never goes in before the code it lands on (mem.c has the order).
 */
int patch_apply(const struct patch_def *def);
int patch_revert(const struct patch_def *def);
/* Also true for a partial operation whose cleanup failed. Revert must retry;
 * apply returns IO_ERROR until cleanup succeeds, without recapturing originals. */
int patch_is_applied(const struct patch_def *def);
void patch_forget_all(void);        /* drops the table without writing anything */

u32 patch_count(void);
const struct patch_def *patch_at(u32 index);

/* A static originals pool for feature and other compile-time defs. */
u32 *patch_pool_alloc(u16 words);
void patch_pool_reset(void);

/* --------------------------------------------------------- client patches */

/*
 * PATCH_APPLY builds one of these, keyed by the first address. Applying the same
 * first address twice is a no-op.
 */
int  client_patch_apply(const struct patch_word *words, u16 count);
int  client_patch_revert(u32 first_addr);
int  client_patch_clear(void);
u32  client_patch_count(void);
const struct patch_def *client_patch_at(u32 index);

/* Frees every client patch slot without writing anything. */
void client_patch_drop_all(void);

/* Frees the slots that are no longer in the patch table (dismissed records). */
void client_patch_gc(void);

/* CLEAR_CLIENT: every watch, every freeze, every client patch. */
int mem_clear_client(void);

#endif /* QWARK_MEM_H */
