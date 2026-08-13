/*
 * gov_test.h — tiny host-test harness for the portable modules. Header-only,
 * no deps.  shared infra (used by all tests/<module>/). Ztest is used
 * for the on-target/twister builds; this is for fast standalone host runs +
 * property tests. Keep it minimal on purpose.
 */
#ifndef GOV_TEST_H
#define GOV_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gov_test_failures;
static int gov_test_count;

#define GOV_RUN(fn)                                                            \
	do {                                                                   \
		int before = gov_test_failures;                                \
		fn();                                                          \
		printf("  [%s] %s\n", gov_test_failures == before ? "PASS"     \
								   : "FAIL",   \
		       #fn);                                                   \
	} while (0)

#define GOV_CHECK(cond)                                                        \
	do {                                                                   \
		gov_test_count++;                                              \
		if (!(cond)) {                                                 \
			gov_test_failures++;                                   \
			printf("    CHECK FAILED: %s (%s:%d)\n", #cond,        \
			       __FILE__, __LINE__);                            \
		}                                                              \
	} while (0)

#define GOV_CHECK_EQ(a, b)                                                     \
	do {                                                                   \
		gov_test_count++;                                              \
		long _va = (long)(a), _vb = (long)(b);                         \
		if (_va != _vb) {                                              \
			gov_test_failures++;                                   \
			printf("    EQ FAILED: %s(%ld) != %s(%ld) (%s:%d)\n",  \
			       #a, _va, #b, _vb, __FILE__, __LINE__);          \
		}                                                              \
	} while (0)

/* Place at end of main(): returns process exit code (0 == all passed). */
#define GOV_TEST_SUMMARY()                                                     \
	(printf("== %d checks, %d failure(s) ==\n", gov_test_count,            \
		gov_test_failures),                                            \
	 gov_test_failures == 0 ? 0 : 1)

#endif /* GOV_TEST_H */
