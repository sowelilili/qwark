/*
 * Putting the Lua helper into a game, both ways qwark does it for the other
 * helpers (docs/tod-poc.md sections 3 and 4):
 *
 *   - on RPCS3, as an RPCS3 patch the client writes from the words below
 *     (LUA_PATCH, revision 1.27, laid out exactly as SAVEFILE_PATCH, PROTOCOL.md
 *     5.12.1), and the presence rule that tells qwark the patch went in;
 *   - on a console, as writes qwark makes itself into the running game, and
 *     the writes that take it out again.
 *
 * The core links this where QWARK_HAS_TOD (src/core/luacore.c). Revision 1.29
 * (pending): the bytes are the emulator builds' alone (luahelper_bins.c, under
 * QWARK_EMULATOR), and so are the patch and the presence rule; a console's
 * install takes the cave's bytes from a source, ui.pak's entry (luacore.c),
 * and the numbers from the descriptor tod.sprx hands the core (lua_desc).
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

#ifdef QWARK_EMULATOR
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
#endif /* QWARK_EMULATOR */

/*
 * Revision 1.29 (pending): where a console install takes each cave's bytes
 * from, a piece at a time through `buf` (`cap` bytes): `read` fills buf with
 * `len` bytes from `off` into cave `cave`, 0 or -1 for a failed read. ui.pak's
 * entry, in luacore.c.
 */
struct lh_source {
	int (*read)(void *ctx, u8 cave, u32 off, void *buf, u32 len);
	void *ctx;
	u8 *buf;
	u32 cap;
};

/*
 * A console install, for a platform that patches code: the mailbox header
 * zeroed and given its identity, the cave, then the error tap and last the VM
 * hook. Refuses (ST_BAD_ARG) a game whose hook sites do not hold the words the
 * helper replaces, unless they already hold the helper's own. ST_IO_ERROR on
 * a failed access or read. The cave's bytes come from `src`, or with none
 * from the descriptor (the emulator builds'); ST_UNSUPPORTED with neither.
 */
int lh_install(const struct lh_desc *d, const struct luamb_io *io, const struct lh_source *src);

/* Puts the game's own words back, the VM hook first. The cave stays. */
int lh_uninstall(const struct lh_desc *d, const struct luamb_io *io);

#endif /* QWARK_LUAHELPER_PATCH_H */
