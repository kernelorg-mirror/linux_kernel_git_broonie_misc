// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 userspace implementations of getcpu()
 *
 * Copyright (C) 2020 ARM Limited
 *
 */

#include <asm/unistd.h>
#include <asm/vdso/datapage.h>

struct getcpucache;

static __always_inline
int getcpu_fallback(unsigned int *_cpu, unsigned int *_node,
		    struct getcpucache *_c)
{
	register unsigned int *cpu asm("x0") = _cpu;
	register unsigned int *node asm("x1") = _node;
	register struct getcpucache *c asm("x2") = _c;
	register long ret asm ("x0");
	register long nr asm("x8") = __NR_getcpu;

	asm volatile(
	"       svc #0\n"
	: "=r" (ret)
	: "r" (cpu), "r" (node), "r" (c), "r" (nr)
	: "memory");

	return ret;
}

int __kernel_getcpu(unsigned int *cpu, unsigned int *node,
		    struct getcpucache *c)
{
	struct vdso_cpu_data *cpu_data = __vdso_cpu_data();

	if (cpu_data) {
		if (cpu)
			*cpu = cpu_data->cpu;
		if (node)
			*node = cpu_data->node;

		return 0;
	}

	return getcpu_fallback(cpu, node, c);
}
