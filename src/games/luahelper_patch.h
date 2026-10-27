/*
 * Putting the Lua helper into a game, both ways qwark does it for the other
 * helpers (docs/tod-poc.md sections 3 and 4):
 *
 *   - on RPCS3, as an RPCS3 patch the client writes from the words below
 *     (LUA_PATCH in phase 2, laid out exactly as SAVEFILE_PATCH, PROTOCOL.md
 *     5.12.1), and the presence rule that tells qwark the patch went in;
 *   - on a console, as writes qwark makes itself into the running game, and
 *     the writes that take it out again.
 *
 * Phase 1 links this only into the unit tests. The bytes are the emulator
 * builds' (luahelper_bins.c, QWARK_EMULATOR), so everything here that needs
 * them answers ST_UNSUPPORTED without it.
 */
#ifndef QWARK_LUAHELPER_PATCH_H
#define QWARK_LUAHELPER_PATCH_H

#include "luahelper_bins.h"
#include "../core/luamb_client.h"

/*
 * The patch reply, SAVEFILE_PATCH's layout (PROTOCOL.md 5.12.1): u16 n, u16 0,
 * u32 stamp (CRC-32 of everything after it), n x { u32 addr, u32 value }, then
 * u16 nb, u16 0 and nb x { u32 addr, u8 value, u8 pad[3] }.
 *
 * The words are, in this order: every cave word; the mailbox's first two
 * words (magic, then version and size), which make the helper live from the
 * first frame; and the hook words, the error tap before the VM hook, so a
 * reader applying them in order never has a branch in place before what it
 * reaches. No single bytes: the mailbox lies past the end of the executable's
 * file data and is zero at load.
 */
#define LH_PATCH_HEAD       8u
#define LH_PATCH_ROW        8u
#define LH_PATCH_BYTES_HEAD 4u
#define LH_PATCH_IDENT_WORDS 2u

/* Words in the patch, or 0 without the bytes. */
u32 lh_patch_count(const struct lh_desc *d);

/* ST_OK, ST_UNSUPPORTED (no bytes in this build), ST_FULL (cap too small). */
int lh_patch_build(const struct lh_desc *d, u8 *out, u32 cap, u32 *len);

/*
 * The presence rule, the savefile helper's (PROTOCOL.md 5.12.1): every cave
 * byte, both hook words and the mailbox's identity read back exactly as the
 * patch writes them. 1 present, 0 not, -1 a read failed (unknown).
 */
int lh_present(const struct lh_desc *d, const struct luamb_io *io);

/*
 * A console install, for a platform that patches code: the mailbox header
 * zeroed and given its identity, the cave, then the error tap and last the VM
 * hook. Refuses (ST_BAD_ARG) a game whose hook sites do not hold the words the
 * helper replaces, unless they already hold the helper's own. ST_IO_ERROR on
 * a failed access, ST_UNSUPPORTED without the bytes.
 */
int lh_install(const struct lh_desc *d, const struct luamb_io *io);

/* Puts the game's own words back, the VM hook first. The cave stays. */
int lh_uninstall(const struct lh_desc *d, const struct luamb_io *io);

#endif /* QWARK_LUAHELPER_PATCH_H */
