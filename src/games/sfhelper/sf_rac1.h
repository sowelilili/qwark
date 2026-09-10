/*
 * Ratchet & Clank (NPEA00385): the savefile helper's address table.
 *
 * From racman's mods/NPEA00385/sfhelper: input.c for the logic, npea00385.h for
 * the game addresses and patch.txt for the caves and the hook. That mod is
 * "originally written by doesthisusername for Rackets".
 *
 * What was left out of the port, and why:
 *
 *   lv2.bin  a syscall thunk at 0x4F6500, plus the pointer word at 0x710FC0 that
 *            registered it. The savefile path never makes a syscall - it moves
 *            bytes inside the process with the game's own memcpy - so nothing
 *            reads either of them.
 *   render.bin, tmp_fpu.bin  the on-screen display of a different mod. make.sh
 *            builds them and patch.txt never writes them.
 *   tramp.bin  a register-preserving trampoline at 0x4F63C0 that inputp called
 *            with the helper's address in r0. Its work is folded into the input
 *            hook below, which saves the same registers and branches straight to
 *            the helper, so one cave does what two did.
 *
 * The build.sh parser reads the #define lines below, so each one is a plain name
 * and a hexadecimal literal with nothing else on it.
 */
#ifndef QWARK_SF_RAC1_H
#define QWARK_SF_RAC1_H

/* ------------------------------------------------------------------- caves */

/*
 * Two caves, both addresses racman's own patch.txt writes to.
 *
 *   SF_CAVE_STUB  the input hook, which took inputp.bin
 *   SF_CAVE       the helper itself, which took input.bin (644 bytes there)
 */
#define SF_CAVE_STUB 0x004F6800
#define SF_CAVE      0x004F8000

/*
 * The hook site: the post-input-poll instruction patch.txt replaces, which is
 * `ld r0, 0xA0(r1)`. The stub runs the helper, puts every register back and then
 * executes that displaced instruction itself before returning.
 *
 * Absolute, because that is the form racman's word (0x484F6803) has: `bla`.
 */
#define SF_HOOK_ADDR     0x0011E3A0
#define SF_HOOK_ABSOLUTE 1

/* -------------------------------------------------------- the request bytes */

/*
 * The four bytes the client drives, unchanged from the mod: qwark's rac1.h names
 * the same addresses. api_mod reads 1 for as long as the helper is running,
 * because the helper writes it on every call.
 */
#define SF_API_MOD      0x00B00070
#define SF_API_LOAD     0x00B00071
#define SF_API_SETASIDE 0x00B00072
#define SF_API_SAVEMODE 0x00B00073

/* The copy loop's counter, which lives just below the aside buffer. */
#define SF_API_I 0x00FFFFFC

/* ------------------------------------------------------- the aside buffer */

#define SF_ASIDE_ADDR 0x01000000
#define SF_ASIDE_SIZE 0x000B0000

/* Where the planet the save was made on sits inside the save data. */
#define SF_ASIDE_PLANET_OFFSET 0x18

/* --------------------------------------------------------- game addresses */

/*
 * savedata_buf is a pointer plus a bias in this game rather than a struct field:
 * *(0xA10928) + 0x100000, exactly as npea00385.h spells it.
 */
#define SF_SAVEDATA_PTR  0x00A10928
#define SF_SAVEDATA_BIAS 0x00100000

/* The planet load the helper kicks off once the save is in. */
#define SF_DEST_PLANET 0x00A10704
#define SF_SHOULD_LOAD 0x00A10700

/* ---------------------------------------------------------- game functions */

#define SF_FN_MEMCPY       0x005C5AD0
#define SF_FN_PERFORM_LOAD 0x000E8CA0
#define SF_FN_SAVE_HANDLER 0x000E8888

#endif /* QWARK_SF_RAC1_H */
