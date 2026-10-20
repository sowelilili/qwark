/*
 * The little PASS/FAIL harness test_qwark.c owns, shared with the other test
 * translation units so every check lands in one count.
 */
#ifndef QWARK_TEST_COMMON_H
#define QWARK_TEST_COMMON_H

#include "../src/plat/plat.h"
#include "../src/core/menu.h"

/*
 * The console menu's root in a game that has every part of it (Game, Cheats,
 * Planets, Manips, Unlocks, Save files): six rows, and the grey build row after
 * them when src/core/menu.h builds it in (MENU_BUILD_ROW 1; the release does
 * not). A test that counts the root's rows, or looks at its last, goes by this
 * and by MENU_BUILD_ROW, so the suite passes under -DMENU_BUILD_ROW=1 as well.
 */
#define ROOT_ROWS (6 + MENU_BUILD_ROW)

void group(const char *name);
void check(int ok, const char *what);
void check_eq_u64(u64 got, u64 want, const char *what);

/* test_pine.c: the PINE backend against a fake PINE server. */
void test_pine(void);

/* test_menu.c: the console menu, its model, its mailbox and Deadlocked's renderer. */
void test_menu(void);

/* test_menu_core.c: the renderers' shared core, against a fake shim. */
void test_menu_core(void);

/* test_menu_uya.c: UYA's pages, install and renderer. */
void test_menu_uya(void);

/* test_menu_rac2.c: RaC2's pages, install and renderer. */
void test_menu_rac2(void);

/* test_menu_rac1.c: RaC1's pages, install and renderer. */
void test_menu_rac1(void);

/* test_vshmem.c: the VSH's free memory, build 54. */
void test_vshmem(void);

#endif /* QWARK_TEST_COMMON_H */
