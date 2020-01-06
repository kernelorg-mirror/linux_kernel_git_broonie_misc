/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARCHRANDOM_H
#define _ASM_ARCHRANDOM_H

#ifdef CONFIG_ARCH_RANDOM

#include <linux/random.h>
#include <asm/cpufeature.h>

static inline bool __arm64_rndr(unsigned long *v)
{
	bool ok;

	/*
	 * Reads of RNDR set PSTATE.NZCV to 0b0000 on success,
	 * and set PSTATE.NZCV to 0b0100 otherwise.
	 */
	asm volatile(
		__mrs_s("%0", SYS_RNDR_EL0) "\n"
	"	cset %w1, ne\n"
	: "=r" (*v), "=r" (ok)
	:
	: "cc");

	return ok;
}

static inline bool __must_check arch_get_random_long(unsigned long *v)
{
	return false;
}

static inline bool __must_check arch_get_random_int(unsigned int *v)
{
	return false;
}

static inline bool __must_check arch_get_random_seed_long(unsigned long *v)
{
	/*
	 * Only support the generic interface after we have detected
	 * the system wide capability, avoiding complexity with the
	 * cpufeature code and with potential scheduling between CPUs
	 * with and without the feature.
	 */
	if (!cpus_have_const_cap(ARM64_HAS_RNG))
		return false;

	return __arm64_rndr(v);
}


static inline bool __must_check arch_get_random_seed_int(unsigned int *v)
{
	unsigned long val;
	bool ok = arch_get_random_seed_long(&val);

	*v = val;
	return ok;
}

static inline bool __init __early_cpu_has_rndr(void)
{
	/* Open code as we run prior to the first call to cpufeature. */
	unsigned long ftr = read_sysreg_s(SYS_ID_AA64ISAR0_EL1);
	return (ftr >> ID_AA64ISAR0_RNDR_SHIFT) & 0xf;
}

/*
 * Our ARCH_RANDOM implementation does not function until relatively
 * late in the boot when cpufeature has detertmined system
 * capabilities so the core code can't use arch_get_random*() to
 * initialize, instead we call this function to inject data from
 * setup_arch() if the boot CPU supports v8.5-RNG.
 */
static inline void __init arm64_add_early_rndr_entropy(void)
{
	unsigned long val;
	int i;

	if (!__early_cpu_has_rndr())
		return;

	/* Add multiple values to mirror the generic code. */
	for (i = 0; i < 16; i++)
		if (__arm64_rndr(&val))
			add_device_randomness(&val, sizeof(val));
}

#else

static inline bool __arm64_rndr(unsigned long *v) { return false; }
static inline bool __init __early_cpu_has_rndr(void) { return false; }
static inline void __init arm64_add_early_rndr_entropy(void) { }

#endif /* CONFIG_ARCH_RANDOM */
#endif /* _ASM_ARCHRANDOM_H */
