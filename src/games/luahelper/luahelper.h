/*
 * The Lua helper's shared declarations, and the switch that picks one game's
 * address table.
 *
 * Compiled only by the PS3 cross compiler (build.sh), into code that runs
 * inside the game. Nothing in qwark includes it; qwark carries the generated
 * bytes in src/games/luahelper_bins.c and the mailbox layout in
 * src/core/luamb.h, which this file includes too.
 *
 * Every game function is declared here and left undefined; the link binds
 * each to the hard address of its LH_FN_* line with --defsym, so a call is a
 * plain relative branch, as in the savefile helper.
 */
#ifndef QWARK_LUAHELPER_H
#define QWARK_LUAHELPER_H

#define LH_TOD 1

#ifndef LH_GAME
#error "LH_GAME must be set; build.sh passes it"
#endif

#if LH_GAME == LH_TOD
#include "lh_tod.h"
#else
#error "LH_GAME is not a game this helper knows"
#endif

#include "../../core/luamb.h"

#ifndef __ASSEMBLER__

typedef unsigned char      lh_u8;
typedef unsigned int       lh_u32;

/*
 * Everything the game owns is a 32-bit address; the compiler's pointers are
 * eight bytes, so addresses travel as lh_u32 and become a pointer only here.
 */
#define LH_BYTE(addr) (*(volatile lh_u8 *)(unsigned long)(lh_u32)(addr))
#define LH_WORD(addr) (*(volatile lh_u32 *)(unsigned long)(lh_u32)(addr))

/*
 * The Lua 5.0 API, as the game has it. `L` and every string are 32-bit
 * addresses, passed zero-extended, which is how the game's own 64-bit-register
 * calls pass them; indices are ints, sign-extended.
 */
void   lh_alloc_set_current(lh_u32 alloc);
int    lh_gettop(lh_u32 L);
void   lh_settop(lh_u32 L, int idx);
int    lh_loadbuffer(lh_u32 L, lh_u32 buf, lh_u32 size, lh_u32 name);
int    lh_pcall(lh_u32 L, int nargs, int nresults, int errfunc);
int    lh_type(lh_u32 L, int idx);
int    lh_toboolean(lh_u32 L, int idx);
lh_u32 lh_tostring(lh_u32 L, int idx);

/* The C halves of the two entries (luahelper.c). */
void lh_tick(lh_u32 vm);
void lh_err_tap(lh_u32 vm, lh_u32 msg);

#endif /* __ASSEMBLER__ */

#endif /* QWARK_LUAHELPER_H */
