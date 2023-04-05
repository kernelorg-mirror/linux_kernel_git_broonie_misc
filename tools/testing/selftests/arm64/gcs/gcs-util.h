/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 ARM Limited.
 */

#ifndef GCS_UTIL_H
#define GCS_UTIL_H

#include <stdbool.h>

#ifndef __NR_map_shadow_stack
#define __NR_map_shadow_stack 452
#endif

#ifndef __NR_prctl
#define __NR_prctl 167
#endif

/* Shadow Stack/Guarded Control Stack interface */
#define PR_GET_SHADOW_STACK_STATUS	71
#define PR_SET_SHADOW_STACK_STATUS      72
# define PR_SHADOW_STACK_LOCK           (1UL << 0)
# define PR_SHADOW_STACK_ENABLE         (1UL << 1)
# define PR_SHADOW_STACK_WRITE		(1UL << 2)
# define PR_SHADOW_STACK_PUSH		(1UL << 3)

#define GCS_CAP_ADDR_MASK	(0xfffffffffffff000UL)
#define GCS_CAP_VALID_TOKEN	1

static unsigned long *get_gcspr(void)
{
	unsigned long *gcspr;

	asm volatile(
		"mrs	%0, S3_3_C2_C5_1"
	: "=r" (gcspr)
	:
	: "cc");

	return gcspr;
}

static inline void __attribute__((always_inline)) gcsss1(unsigned long Xt)
{
	asm volatile (
		"sys #3, C7, C7, #2, %0\n"
		:
		: "rZ" (Xt)
		: "memory");
}

static inline unsigned long __attribute__((always_inline)) gcsss2(void)
{
	unsigned long Xt;

	asm volatile(
		"SYSL %0, #3, C7, C7, #3\n"
		: "=r" (Xt)
		:
		: "memory");

	return Xt;
}

#endif
