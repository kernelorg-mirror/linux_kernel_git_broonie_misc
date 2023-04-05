/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kselftest-nolibc	Cut down nolibc based kselftest output functions
 *
 * Copyright (c) 2014 Shuah Khan <shuahkh@osg.samsung.com>
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *
 */

#ifndef __KSELFTEST_H
#error This file should never be included directly, always include kselftest.h
#endif

static inline void ksft_print_msg(const char *msg)
{
	printf("# %s", msg);
}

static inline void ksft_test_result_pass(const char *msg)
{
	ksft_cnt.ksft_pass++;

	printf("ok %d %s", ksft_test_num(), msg);
}

static inline void ksft_test_result_fail(const char *msg)
{
	ksft_cnt.ksft_fail++;

	printf("not ok %d %s", ksft_test_num(), msg);
}

/**
 * ksft_test_result() - Report test success based on truth of condition
 *
 * @condition: if true, report test success, otherwise failure.
 */
#define ksft_test_result(condition, fmt) do {	\
	if (!!(condition))				\
		ksft_test_result_pass(fmt);\
	else						\
		ksft_test_result_fail(fmt);\
	} while (0)

static inline void ksft_test_result_xfail(const char *msg)
{
	ksft_cnt.ksft_xfail++;

	printf("ok %d # XFAIL %s", ksft_test_num(), msg);
}

static inline void ksft_test_result_skip(const char *msg)
{
	ksft_cnt.ksft_xskip++;

	printf("ok %d # SKIP %s", ksft_test_num(), msg);
}

static inline void ksft_test_result_error(const char *msg)
{
	ksft_cnt.ksft_error++;

	printf("not ok %d # error %s", ksft_test_num(), msg);
}

static inline int ksft_exit_fail_msg(const char *msg)
{
	printf("Bail out! %s", msg);

	ksft_print_cnts();
	exit(KSFT_FAIL);
}

static inline int ksft_exit_skip(const char *msg)
{
	/*
	 * FIXME: several tests misuse ksft_exit_skip so produce
	 * something sensible if some tests have already been run
	 * or a plan has been printed.  Those tests should use
	 * ksft_test_result_skip or ksft_exit_fail_msg instead.
	 */
	if (ksft_plan || ksft_test_num()) {
		ksft_cnt.ksft_xskip++;
		printf("ok %d # SKIP ", 1 + ksft_test_num());
	} else {
		printf("1..0 # SKIP ");
	}
	if (msg)
		printf("%s", msg);
	if (ksft_test_num())
		ksft_print_cnts();
	exit(KSFT_SKIP);
}
