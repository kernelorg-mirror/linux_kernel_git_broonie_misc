// SPDX-License-Identifier: GPL-2.0

/* Based on Christian Brauner's clone3() example */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

#include "../kselftest.h"
#include "clone3_selftests.h"

static bool shadow_stack_enabled;
static bool shadow_stack_supported;
static size_t max_supported_args_size;

enum test_mode {
	CLONE3_ARGS_NO_TEST,
	CLONE3_ARGS_ALL_0,
	CLONE3_ARGS_INVAL_EXIT_SIGNAL_BIG,
	CLONE3_ARGS_INVAL_EXIT_SIGNAL_NEG,
	CLONE3_ARGS_INVAL_EXIT_SIGNAL_CSIG,
	CLONE3_ARGS_INVAL_EXIT_SIGNAL_NSIG,
	CLONE3_ARGS_SHADOW_STACK,
};

typedef bool (*filter_function)(void);
typedef size_t (*size_function)(void);

struct test {
	const char *name;
	uint64_t flags;
	size_t size;
	size_function size_function;
	int expected;
	bool e2big_valid;
	enum test_mode test_mode;
	filter_function filter;
};

#ifndef __NR_map_shadow_stack
#define __NR_map_shadow_stack 453
#endif

/*
 * We check for shadow stack support by attempting to use
 * map_shadow_stack() since features may have been locked by the
 * dynamic linker resulting in spurious errors when we attempt to
 * enable on startup.  We warn if the enable failed.
 */
static void test_shadow_stack_supported(void)
{
        long shadow_stack;

	shadow_stack = syscall(__NR_map_shadow_stack, 0, getpagesize(), 0);
	if (shadow_stack == -1) {
		ksft_print_msg("map_shadow_stack() not supported\n");
	} else if ((void *)shadow_stack == MAP_FAILED) {
		ksft_print_msg("Failed to map shadow stack\n");
	} else {
		ksft_print_msg("Shadow stack supportd\n");
		shadow_stack_supported = true;

		if (!shadow_stack_enabled)
			ksft_print_msg("Mapped but did not enable shadow stack\n");

		munmap((void *)shadow_stack, getpagesize());
	}
}

static int call_clone3(uint64_t flags, size_t size, enum test_mode test_mode)
{
	struct __clone_args args = {
		.flags = flags,
		.exit_signal = SIGCHLD,
	};

	struct clone_args_extended {
		struct __clone_args args;
		__aligned_u64 excess_space[2];
	} args_ext;

	pid_t pid = -1;
	int status;

	memset(&args_ext, 0, sizeof(args_ext));
	if (size > sizeof(struct __clone_args))
		args_ext.excess_space[1] = 1;

	if (size == 0)
		size = sizeof(struct __clone_args);

	switch (test_mode) {
	case CLONE3_ARGS_NO_TEST:
		/*
		 * Uses default 'flags' and 'SIGCHLD'
		 * assignment.
		 */
		break;
	case CLONE3_ARGS_ALL_0:
		args.flags = 0;
		args.exit_signal = 0;
		break;
	case CLONE3_ARGS_INVAL_EXIT_SIGNAL_BIG:
		args.exit_signal = 0xbadc0ded00000000ULL;
		break;
	case CLONE3_ARGS_INVAL_EXIT_SIGNAL_NEG:
		args.exit_signal = 0x0000000080000000ULL;
		break;
	case CLONE3_ARGS_INVAL_EXIT_SIGNAL_CSIG:
		args.exit_signal = 0x0000000000000100ULL;
		break;
	case CLONE3_ARGS_INVAL_EXIT_SIGNAL_NSIG:
		args.exit_signal = 0x00000000000000f0ULL;
		break;
	case CLONE3_ARGS_SHADOW_STACK:
		args.shadow_stack_size = getpagesize();
		break;
	}

	memcpy(&args_ext.args, &args, sizeof(struct __clone_args));

	pid = sys_clone3((struct __clone_args *)&args_ext, size);
	if (pid < 0) {
		ksft_print_msg("%s - Failed to create new process\n",
				strerror(errno));
		return -errno;
	}

	if (pid == 0) {
		ksft_print_msg("I am the child, my PID is %d\n", getpid());
		_exit(EXIT_SUCCESS);
	}

	ksft_print_msg("I am the parent (%d). My child's pid is %d\n",
			getpid(), pid);

	if (waitpid(-1, &status, __WALL) < 0) {
		ksft_print_msg("Child returned %s\n", strerror(errno));
		return -errno;
	}
	if (WEXITSTATUS(status))
		return WEXITSTATUS(status);

	return 0;
}

static void test_clone3(const struct test *test)
{
	size_t size;
	int ret;

	if (test->filter && test->filter()) {
		ksft_test_result_skip("%s\n", test->name);
		return;
	}

	if (test->size_function)
		size = test->size_function();
	else
		size = test->size;

	ksft_print_msg("Running test '%s'\n", test->name);

	ksft_print_msg(
		"[%d] Trying clone3() with flags %#" PRIx64 " (size %zu)\n",
		getpid(), test->flags, size);
	ret = call_clone3(test->flags, size, test->test_mode);
	ksft_print_msg("[%d] clone3() with flags says: %d expected %d\n",
			getpid(), ret, test->expected);
	if (ret != test->expected) {
		if (test->e2big_valid && ret == -E2BIG) {
			ksft_print_msg("Test reported -E2BIG\n");
			ksft_test_result_skip("%s\n", test->name);
			return;
		}
		ksft_print_msg(
			"[%d] Result (%d) is different than expected (%d)\n",
			getpid(), ret, test->expected);
		ksft_test_result_fail("%s\n", test->name);
		return;
	}

	ksft_test_result_pass("%s\n", test->name);
}

static bool not_root(void)
{
	if (getuid() != 0) {
		ksft_print_msg("Not running as root\n");
		return true;
	}

	return false;
}

static bool no_timenamespace(void)
{
	if (not_root())
		return true;

	if (!access("/proc/self/ns/time", F_OK))
		return false;

	ksft_print_msg("Time namespaces are not supported\n");
	return true;
}

static bool have_shadow_stack(void)
{
	if (shadow_stack_supported) {
		ksft_print_msg("Shadow stack supported\n");
		return true;
	}

	return false;
}

static bool no_shadow_stack(void)
{
	if (!shadow_stack_supported) {
		ksft_print_msg("Shadow stack not supported\n");
		return true;
	}

	return false;
}

static size_t page_size_plus_8(void)
{
	return getpagesize() + 8;
}

static const struct test tests[] = {
	{
		.name = "simple clone3()",
		.flags = 0,
		.size = 0,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "clone3() in a new PID_NS",
		.flags = CLONE_NEWPID,
		.size = 0,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
		.filter = not_root,
	},
	{
		.name = "CLONE_ARGS_SIZE_VER0",
		.flags = 0,
		.size = CLONE_ARGS_SIZE_VER0,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "CLONE_ARGS_SIZE_VER0 - 8",
		.flags = 0,
		.size = CLONE_ARGS_SIZE_VER0 - 8,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "sizeof(struct clone_args) + 8",
		.flags = 0,
		.size = sizeof(struct __clone_args) + 8,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "exit_signal with highest 32 bits non-zero",
		.flags = 0,
		.size = 0,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_INVAL_EXIT_SIGNAL_BIG,
	},
	{
		.name = "negative 32-bit exit_signal",
		.flags = 0,
		.size = 0,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_INVAL_EXIT_SIGNAL_NEG,
	},
	{
		.name = "exit_signal not fitting into CSIGNAL mask",
		.flags = 0,
		.size = 0,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_INVAL_EXIT_SIGNAL_CSIG,
	},
	{
		.name = "NSIG < exit_signal < CSIG",
		.flags = 0,
		.size = 0,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_INVAL_EXIT_SIGNAL_NSIG,
	},
	{
		.name = "Arguments sizeof(struct clone_args) + 8",
		.flags = 0,
		.size = sizeof(struct __clone_args) + 8,
		.expected = 0,
		.test_mode = CLONE3_ARGS_ALL_0,
	},
	{
		.name = "Arguments sizeof(struct clone_args) + 16",
		.flags = 0,
		.size = sizeof(struct __clone_args) + 16,
		.expected = -E2BIG,
		.test_mode = CLONE3_ARGS_ALL_0,
	},
	{
		.name = "Arguments sizeof(struct clone_arg) * 2",
		.flags = 0,
		.size = sizeof(struct __clone_args) + 16,
		.expected = -E2BIG,
		.test_mode = CLONE3_ARGS_ALL_0,
	},
	{
		.name = "Arguments > page size",
		.flags = 0,
		.size_function = page_size_plus_8,
		.expected = -E2BIG,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "CLONE_ARGS_SIZE_VER0 in a new PID NS",
		.flags = CLONE_NEWPID,
		.size = CLONE_ARGS_SIZE_VER0,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
		.filter = not_root,
	},
	{
		.name = "CLONE_ARGS_SIZE_VER0 - 8 in a new PID NS",
		.flags = CLONE_NEWPID,
		.size = CLONE_ARGS_SIZE_VER0 - 8,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "sizeof(struct clone_args) + 8 in a new PID NS",
		.flags = CLONE_NEWPID,
		.size = sizeof(struct __clone_args) + 8,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
		.filter = not_root,
	},
	{
		.name = "Arguments > page size in a new PID NS",
		.flags = CLONE_NEWPID,
		.size_function = page_size_plus_8,
		.expected = -E2BIG,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "New time NS",
		.flags = CLONE_NEWTIME,
		.size = 0,
		.expected = 0,
		.test_mode = CLONE3_ARGS_NO_TEST,
		.filter = no_timenamespace,
	},
	{
		.name = "exit signal (SIGCHLD) in flags",
		.flags = SIGCHLD,
		.size = 0,
		.expected = -EINVAL,
		.test_mode = CLONE3_ARGS_NO_TEST,
	},
	{
		.name = "Shadow stack on system with shadow stack",
		.flags = CLONE_VM,
		.size = 0,
		.expected = 0,
		.e2big_valid = true,
		.test_mode = CLONE3_ARGS_SHADOW_STACK,
		.filter = no_shadow_stack,
	},
	{
		.name = "Shadow stack on system without shadow stack",
		.flags = CLONE_VM,
		.size = 0,
		.expected = -EINVAL,
		.e2big_valid = true,
		.test_mode = CLONE3_ARGS_SHADOW_STACK,
		.filter = have_shadow_stack,
	},
};

#ifdef __x86_64__
#define ARCH_SHSTK_ENABLE	0x5001
#define ARCH_SHSTK_SHSTK	(1ULL <<  0)

#define ARCH_PRCTL(arg1, arg2)					\
({								\
	long _ret;						\
	register long _num  asm("eax") = __NR_arch_prctl;	\
	register long _arg1 asm("rdi") = (long)(arg1);		\
	register long _arg2 asm("rsi") = (long)(arg2);		\
								\
	asm volatile (						\
		"syscall\n"					\
		: "=a"(_ret)					\
		: "r"(_arg1), "r"(_arg2),			\
		  "0"(_num)					\
		: "rcx", "r11", "memory", "cc"			\
	);							\
	_ret;							\
})

#define ENABLE_SHADOW_STACK
static inline void enable_shadow_stack(void)
{
	int ret = ARCH_PRCTL(ARCH_SHSTK_ENABLE, ARCH_SHSTK_SHSTK);
	if (ret == 0)
		shadow_stack_enabled = true;
}

#endif

#ifdef __aarc64__
#define PR_SET_SHADOW_STACK_STATUS      72
# define PR_SHADOW_STACK_ENABLE         (1UL << 0)

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

#define ENABLE_SHADOW_STACK
static inline void enable_shadow_stack(void)
{
	int ret;

	ret = my_syscall2(__NR_prctl, PR_SET_SHADOW_STACK_STATUS,
			  PR_SHADOW_STACK_ENABLE);
	if (ret == 0)
		shadow_stack_enabled = true;
}

#endif

#ifndef ENABLE_SHADOW_STACK
static void enable_shadow_stack(void)
{
}
#endif

int main(int argc, char *argv[])
{
	size_t size;
	int i;

	enable_shadow_stack();

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));
	test_clone3_supported();
	test_shadow_stack_supported();

	for (i = 0; i < ARRAY_SIZE(tests); i++)
		test_clone3(&tests[i]);

	ksft_finished();
}
