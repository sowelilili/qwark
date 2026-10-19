/*
 * The little PASS/FAIL harness test_qwark.c owns, shared with the other test
 * translation units so every check lands in one count.
 */
#ifndef QWARK_TEST_COMMON_H
#define QWARK_TEST_COMMON_H

#include "../src/plat/plat.h"

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

#endif /* QWARK_TEST_COMMON_H */
