/*
 * Ratchet & Clank's shim in two files: mh_rac1.c (the MAIN widget, the
 * pages, the moves, the entries from the game) and mh_rac1_cfg.c (the CONFIG
 * widget and the text helpers), whose functions build.sh spreads over RaC1's
 * code areas (mh_rac1.h). What each uses of the other.
 * C only; included after mh_common.h, mh_shim.h and the game header. In the host
 * tests both files are one translation unit and everything here is file-local.
 */
#ifndef QWARK_MH_RAC1_SHIM_H
#define QWARK_MH_RAC1_SHIM_H

#define SH(o)        ((u32)MH_SHIM + (u32)(o))
#define PAGE_LIST    SH(SH_PAGE_LIST)
#define PAGE_CONF(c) SH(SH_PAGE_CONF + (u32)(c) * R1_PAGE_SIZE)
#define ROW(k)       SH(SH_ROWS + (u32)(k) * R1_LIST_SIZE)
#define TITLE        SH(SH_TITLE)
#define LIST         SH(SH_LIST)
#define ENTRY(k)     SH(SH_ENTRIES + (u32)(k) * R1_ENTRY_SIZE)
#define TEXT(k)      SH(SH_TEXT + (u32)(k) * MH_TEXT_LEN)
#define STRTAB(id)   SH(SH_STRTAB + 4u * ((u32)(id) - R1_ID_BASE))
#define PAGE_MAP     SH(SH_PAGE_MAP)
#define MAP_LIST     SH(SH_MAP_LIST)
#define MAP_GALAXY   SH(SH_MAP_GALAXY)
#define MAP_ENTRY(i) SH(SH_MAP_ENTRIES + (u32)(i) * R1_GENTRY_SIZE)

/* A page (REPORT-rac1-ui.md 2.3) and an item's head (2.4), and the generic list's tail. */
#define PG_ANIM      0x00
#define PG_PARENT    0x38
#define PG_SCREEN    0x3C
#define PG_FOCUS     0x40
#define PG_ITEM      0x44
#define PG_PENDING   0x80
#define IT_UPDATE    0x00
#define IT_DRAW      0x04
#define IT_ENTER     0x08
#define IT_MOBY      0x14
#define IT_W         0x20
#define IT_H         0x24
#define IT_FLAGS     0x30
#define IT_DATA      0x34
#define LI_UP        0x38
#define LI_DOWN      0x3C
#define LI_CURSOR    0x40
#define LI_SCROLL    0x44

/* Where the shim's items are: the page the game shows is one of the shim's, or another. */
enum { AT_NONE, AT_LIST, AT_CONF, AT_MAP };
/* The panes: still, flying in, flying out, or the game's transition asked for. */
enum { PH_IDLE, PH_IN, PH_OUT, PH_WAIT };
/* What follows a fly-out: the menu's screen again (another page), the game's main page, the planet picker. */
enum { T_NONE, T_RESTART, T_GAME, T_PICK };
/* How a pane moves (r1_panes). */
enum { MOVE_OUT, MOVE_IN, MOVE_HIDE };
/* The planet picker: none, the MAIN page's panes going, its page up, or its panes going back. */
enum { PK_NONE, PK_OPENING, PK_UP, PK_RETURN };

/* Functions one object calls in the other: file-local in the host tests. */
#ifdef MH_HOST
#define R1_FN static __attribute__((unused))
#else
#define R1_FN
#endif

/* mh_rac1.c */
R1_FN u32  r1_at(void);
R1_FN u32  r1_arrive(void);
R1_FN u32  r1_phase(void);
R1_FN int  r1_closing(void);
R1_FN int  r1_enabled(u32 i);
R1_FN u32  r1_steps(u32 e);
R1_FN void r1_sound(u32 id, u32 moby);
R1_FN void r1_copy_str(u32 dst, u32 src, u32 cap);
R1_FN int  r1_text_w(u32 font, u32 s);
R1_FN int  r1_cut(u32 font, u32 s, int w, float scale);
R1_FN void r1_goodies_pane(int want);
R1_FN void r1_panes(u32 ph, u32 lo, u32 hi, int how, u32 pose0);
R1_FN void r1_leave(u32 then);
R1_FN u32  r1_main_cursor(void);

/* mh_rac1_cfg.c */
R1_FN void r1_conf_start(void);
R1_FN void r1_keep_visible(void);

/* mh_rac1_pick.c */
R1_FN void r1_pick_setup(void);
R1_FN void r1_pick_open(void);
R1_FN int  r1_pick_returned(u32 n);
R1_FN void r1_pick_reset(void);

/* The entries the game reaches, through descriptors. */
MH_API int  mh_row_update(u32 item);
MH_API int  mh_conf_update(u32 item);
MH_API int  mh_conf_draw(u32 item);
MH_API void mh_play(void);
MH_API int  mh_pick_update(u32 item);

/* The descriptors of the shim's own functions, as the game will hold them. */
#ifdef MH_HOST
#define MH_FN_ROW        1u
#define MH_FN_CONF       2u
#define MH_FN_DRAW       3u
#define MH_FN_PICK       4u
#else
#define MH_FN_ROW        ((u32)mh_row_update)
#define MH_FN_CONF       ((u32)mh_conf_update)
#define MH_FN_DRAW       ((u32)mh_conf_draw)
#define MH_FN_PICK       ((u32)mh_pick_update)
#endif

#endif /* QWARK_MH_RAC1_SHIM_H */
