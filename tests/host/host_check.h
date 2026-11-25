/* SPDX-License-Identifier: GPL-2.0 */
/*
 * host_check.h - the host unit tests' check: a condition that does not hold
 * is counted and printed with its place, and the test goes on, so one run
 * shows every failure.  Every test_* program reports with it.
 */
#ifndef HOST_CHECK_H
#define HOST_CHECK_H

#include <stdio.h>

static int failures; /* the checks that failed so far */

#define CHECK(cond, ...)                                         \
	do                                                           \
	{                                                            \
		if (!(cond))                                             \
		{                                                        \
			failures++;                                          \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                        \
			fprintf(stderr, "\n");                               \
		}                                                        \
	} while (0)

#endif /* HOST_CHECK_H */
