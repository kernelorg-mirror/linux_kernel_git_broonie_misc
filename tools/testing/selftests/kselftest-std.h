/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kselftest-std.h:	Full stdio based kselftest output functions
 *
 * Copyright (c) 2014 Shuah Khan <shuahkh@osg.samsung.com>
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *
 */

#ifndef __KSELFTEST_H
#error This file should never be included directly, always include kselftest.h
#endif

static inline void ksft_print_msg(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	va_start(args, msg);
	printf("# ");
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);
}

static inline void ksft_test_result_pass(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	ksft_cnt.ksft_pass++;

	va_start(args, msg);
	printf("ok %d ", ksft_test_num());
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);
}

static inline void ksft_test_result_fail(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	ksft_cnt.ksft_fail++;

	va_start(args, msg);
	printf("not ok %d ", ksft_test_num());
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);
}

/**
 * ksft_test_result() - Report test success based on truth of condition
 *
 * @condition: if true, report test success, otherwise failure.
 */
#define ksft_test_result(condition, fmt, ...) do {	\
	if (!!(condition))				\
		ksft_test_result_pass(fmt, ##__VA_ARGS__);\
	else						\
		ksft_test_result_fail(fmt, ##__VA_ARGS__);\
	} while (0)

static inline void ksft_test_result_xfail(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	ksft_cnt.ksft_xfail++;

	va_start(args, msg);
	printf("ok %d # XFAIL ", ksft_test_num());
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);
}

static inline void ksft_test_result_skip(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	ksft_cnt.ksft_xskip++;

	va_start(args, msg);
	printf("ok %d # SKIP ", ksft_test_num());
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);
}

/* TODO: how does "error" differ from "fail" or "skip"? */
static inline void ksft_test_result_error(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	ksft_cnt.ksft_error++;

	va_start(args, msg);
	printf("not ok %d # error ", ksft_test_num());
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);
}

static inline int ksft_exit_fail_msg(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	va_start(args, msg);
	printf("Bail out! ");
	errno = saved_errno;
	vprintf(msg, args);
	va_end(args);

	ksft_print_cnts();
	exit(KSFT_FAIL);
}

static inline int ksft_exit_skip(const char *msg, ...)
{
	int saved_errno = errno;
	va_list args;

	va_start(args, msg);

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
	if (msg) {
		errno = saved_errno;
		vprintf(msg, args);
		va_end(args);
	}
	if (ksft_test_num())
		ksft_print_cnts();
	exit(KSFT_SKIP);
}
