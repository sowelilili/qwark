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

#endif /* QWARK_TEST_COMMON_H */
