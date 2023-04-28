// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 ARM Limited.
 */

#include <pthread.h>
#include <stdbool.h>

#include <sys/auxv.h>
#include <sys/prctl.h>

#include <asm/hwcap.h>

#include "kselftest_harness.h"

#include "gcs-util.h"

#define my_syscall2(num, arg1, arg2)                                          \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("x1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("x2") = 0;                               \
	register long _arg4 __asm__ ("x3") = 0;                               \
	register long _arg5 __asm__ ("x4") = 0;                               \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r"(_arg1)                                                 \
		: "r"(_arg1), "r"(_arg2),                                     \
		  "r"(_arg3), "r"(_arg4),                                     \
		  "r"(_arg5), "r"(_num)					      \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

static  __attribute__((noinline)) void valid_gcs_function(void)
{
	/* Do something the compiler can't optimise out */
	prctl(PR_SVE_GET_VL);
}

/* Smoke test that a function call and return works*/
TEST(can_call_function)
{
	valid_gcs_function();
}

/* Smoke test that GCS is enabled in the current thread */
TEST(gcs_locked)
{
	unsigned long gcs_mode;
	int ret;

	ret = my_syscall2(__NR_prctl, PR_GET_SHADOW_STACK_STATUS, &gcs_mode);
	ASSERT_EQ(ret, 0);
	if (ret != 0)
		return;

	/* We are locked, even a noop reconfiguration should fail */
	ret = my_syscall2(__NR_prctl, PR_SET_SHADOW_STACK_STATUS, gcs_mode);
	ASSERT_NE(0, ret);
}

static void *gcs_test_thread(void *arg)
{
	int ret;
	unsigned long mode;

	/*
	 * Some libcs don't seem to fill unused arguments with 0 but
	 * the kernel validates this so we supply all 5 arguments.
	 */
	ret = prctl(PR_GET_SHADOW_STACK_STATUS, &mode, 0, 0, 0);
	if (ret != 0) {
		ksft_print_msg("PR_GET_SHADOW_STACK_STATUS failed: %d\n", ret);
		return NULL;
	}

	if (!(mode & PR_SHADOW_STACK_ENABLE)) {
		ksft_print_msg("GCS not enabled in thread, mode is %u\n",
			       mode);
		return NULL;
	}

	/* Just in case... */
	valid_gcs_function();

	/* Use a non-NULL value to indicate a pass */
	return &gcs_test_thread;
}

/* Verify that if we start a new thread it has GCS enabled */
TEST(gcs_enabled_thread)
{
	pthread_t thread;
	void *thread_ret;
	int ret;

	ret = pthread_create(&thread, NULL, gcs_test_thread, NULL);
	ASSERT_TRUE(ret == 0);
	if (ret != 0)
		return;

	ret = pthread_join(thread, &thread_ret);
	ASSERT_TRUE(ret == 0);
	if (ret != 0)
		return;

	ASSERT_TRUE(thread_ret != NULL);
}

/* Read the GCS until we find the terminator */
TEST(gcs_find_terminator)
{
	uint64_t *gcs, *cur;

	gcs = get_gcspr();
	cur = gcs;
	while (*cur)
		cur++;

	ksft_print_msg("GCS in use from %p-%p\n", gcs, cur);

	/*
	 * We should have at least whatever called into this test so
	 * the two pointer should differ.
	 */
	ASSERT_TRUE(gcs != cur);
}

/* We can switch between stacks */
TEST(switch_stacks)
{
	unsigned long orig_gcspr_el0, pivot_gcspr_el0;
	unsigned long buf_base, buf_end;
	int ret;
	void *buf;

	buf = (void *)syscall(__NR_map_shadow_stack, 0,
			      sysconf(_SC_PAGE_SIZE), 0);
	ASSERT_FALSE(buf == MAP_FAILED);
	buf_base = (unsigned long)buf;
	buf_end = buf_base + sysconf(_SC_PAGE_SIZE);

	/* Skip over the stack terminator and point at the cap */
	pivot_gcspr_el0 = buf_end - 16;

	ksft_print_msg("Mapped GCS at %p-%p\n", buf, buf_end);

	/* Pivot to the new GCS */
	ksft_print_msg("pivoting to %p from %p, target has value 0x%lx\n",
		       pivot_gcspr_el0, get_gcspr(),
		       *((unsigned long *)pivot_gcspr_el0));
	gcsss1(pivot_gcspr_el0);
	orig_gcspr_el0 = gcsss2();
	ksft_print_msg("pivoted to %p from %p, target has value 0x%lx\n",
		       pivot_gcspr_el0, get_gcspr(),
		       *((uint64_t *)pivot_gcspr_el0));

	/* New GCS must be in the new buffer */
	ASSERT_TRUE((unsigned long)get_gcspr() < buf_base);
	ASSERT_TRUE((unsigned long)get_gcspr() > buf_end);

	/* Make sure we can still do calls */
	valid_gcs_function();
	ksft_print_msg("Pivoted to %p\n", get_gcspr());

	/* Pivot back to the original GCS */
	gcsss1(orig_gcspr_el0);
	pivot_gcspr_el0 = gcsss2();

	valid_gcs_function();
	ksft_print_msg("Pivoted back to 0x%lx\n", get_gcspr());

	ret = munmap(buf, sysconf(_SC_PAGE_SIZE));
	ASSERT_EQ(ret, 0);
}

int main(int argc, char **argv)
{
	unsigned long gcs_mode;
	int ret;

	if (!(getauxval(AT_HWCAP2) & HWCAP2_GCS))
		ksft_exit_skip("SKIP GCS not supported\n");

	/* 
	 * Force shadow stacks on, our tests *should* be fine with or
	 * without libc support and with or without this having ended
	 * up tagged for GCS and enabled by the dynamic linker.  We
	 * can't use the libc prctl() function since we can't return
	 * from enabling the stack.  Also lock GCS if not already
	 * locked so we can test behaviour when it's locked.
	 */
	ret = my_syscall2(__NR_prctl, PR_GET_SHADOW_STACK_STATUS, &gcs_mode);
	if (ret) {
		ksft_print_msg("Failed to read GCS state: %d\n", ret);
		return EXIT_FAILURE;
	}
	
	/* If we are already locked we can't configure */
	if (!(gcs_mode & PR_SHADOW_STACK_LOCK)) {
		gcs_mode |= PR_SHADOW_STACK_ENABLE | PR_SHADOW_STACK_LOCK;

		ret = my_syscall2(__NR_prctl, PR_SET_SHADOW_STACK_STATUS,
				  gcs_mode);
		if (ret) {
			ksft_print_msg("Failed to configure GCS: %d\n", ret);
			return EXIT_FAILURE;
		}
	}

	/* Avoid returning in case libc doesn't understand GCS */
	exit(test_harness_run(argc, argv));
}
