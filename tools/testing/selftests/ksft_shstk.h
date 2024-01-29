/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Helpers for shadow stack enablement, this is intended to only be
 * used by low level test programs directly exercising interfaces for
 * working with shadow stacks.
 *
 * Copyright (C) 2024 ARM Ltd.
 */

#ifndef __KSFT_SHSTK_H
#define __KSFT_SHSTK_H

#include <asm/mman.h>

/* This is currently only defined for x86 */
#ifndef SHADOW_STACK_SET_TOKEN
#define SHADOW_STACK_SET_TOKEN (1ULL << 0)
#endif

static bool shadow_stack_enabled;

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
static inline __attribute__((always_inline)) void enable_shadow_stack(void)
{
	int ret = ARCH_PRCTL(ARCH_SHSTK_ENABLE, ARCH_SHSTK_SHSTK);
	if (ret == 0)
		shadow_stack_enabled = true;
}

#endif

#ifndef __NR_map_shadow_stack
#define __NR_map_shadow_stack 453
#endif

#ifndef ENABLE_SHADOW_STACK
static inline void enable_shadow_stack(void) { }
#endif

#endif
