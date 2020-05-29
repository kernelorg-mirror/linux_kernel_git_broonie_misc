/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 ARM Limited
 */
#ifndef __ASM_VDSO_DATAPAGE_H
#define __ASM_VDSO_DATAPAGE_H

#include <vdso/datapage.h>

struct vdso_cpu_data {
	unsigned int cpu;
	unsigned int node;
};

struct arm64_vdso_data {
	/* Must be first in struct, we cast to vdso_data */
	struct vdso_data data[CS_BASES];
	struct vdso_cpu_data cpu_data[];
};

#ifdef __VDSO__
static inline struct vdso_cpu_data *__vdso_cpu_data(void)
{
	unsigned long offset;

	asm volatile(
		"mrs %0, tpidrro_el0\n"
	: "=r" (offset)
	:
	: "cc");

	if (offset)
		return (void *)(_vdso_data) + offset;

	return NULL;
}
#else
static inline size_t vdso_cpu_offset(void)
{
	size_t offset, data_end;

	offset = offsetof(struct arm64_vdso_data, cpu_data) +
		smp_processor_id() * sizeof(struct vdso_cpu_data);
	data_end = offset + sizeof(struct vdso_cpu_data) + 1;

	/* We only map a single page for vDSO data currently */
	if (data_end > PAGE_SIZE)
		return 0;

	return offset;
}
#endif

#endif
