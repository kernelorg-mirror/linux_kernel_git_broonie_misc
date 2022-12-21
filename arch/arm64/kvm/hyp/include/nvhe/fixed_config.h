/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Google LLC
 * Author: Fuad Tabba <tabba@google.com>
 */

#ifndef __ARM64_KVM_FIXED_CONFIG_H__
#define __ARM64_KVM_FIXED_CONFIG_H__

#include <asm/sysreg.h>

/*
 * This file contains definitions for features to be allowed or restricted for
 * guest virtual machines, depending on the mode KVM is running in and on the
 * type of guest that is running.
 *
 * The ALLOW masks represent a bitmask of feature fields that are allowed
 * without any restrictions as long as they are supported by the system.
 *
 * The RESTRICT_UNSIGNED masks, if present, represent unsigned fields for
 * features that are restricted to support at most the specified feature.
 *
 * If a feature field is not present in either, than it is not supported.
 *
 * The approach taken for protected VMs is to allow features that are:
 * - Needed by common Linux distributions (e.g., floating point)
 * - Trivial to support, e.g., supporting the feature does not introduce or
 * require tracking of additional state in KVM
 * - Cannot be trapped or prevent the guest from using anyway
 */

/*
 * Allow for protected VMs:
 * - Floating-point and Advanced SIMD
 * - Data Independent Timing
 */
#define PVM_ID_AA64PFR0_ALLOW (\
	ID_AA64PFR0_EL1_FP_MASK | \
	ID_AA64PFR0_EL1_AdvSIMD_MASK | \
	ID_AA64PFR0_EL1_DIT_MASK \
	)

/*
 * Restrict to the following *unsigned* features for protected VMs:
 * - AArch64 guests only (no support for AArch32 guests):
 *	AArch32 adds complexity in trap handling, emulation, condition codes,
 *	etc...
 * - RAS (v1)
 *	Supported by KVM
 */
#define PVM_ID_AA64PFR0_RESTRICT_UNSIGNED (\
	SYS_FIELD_PREP_ENUM(ID_AA64PFR0_EL1, EL0, IMP) | \
	SYS_FIELD_PREP_ENUM(ID_AA64PFR0_EL1, EL1, IMP) | \
	SYS_FIELD_PREP_ENUM(ID_AA64PFR0_EL1, EL2, IMP) | \
	SYS_FIELD_PREP_ENUM(ID_AA64PFR0_EL1, EL3, IMP) | \
	SYS_FIELD_PREP_ENUM(ID_AA64PFR0_EL1, RAS, IMP)	 \
	)

/*
 * Allow for protected VMs:
 * - Branch Target Identification
 * - Speculative Store Bypassing
 */
#define PVM_ID_AA64PFR1_ALLOW (\
	ID_AA64PFR1_EL1_BT_MASK | \
	ID_AA64PFR1_EL1_SSBS_MASK \
	)

/*
 * Allow for protected VMs:
 * - Mixed-endian
 * - Distinction between Secure and Non-secure Memory
 * - Mixed-endian at EL0 only
 * - Non-context synchronizing exception entry and exit
 */
#define PVM_ID_AA64MMFR0_ALLOW (\
	ID_AA64MMFR0_EL1_BIGEND_MASK | \
	ID_AA64MMFR0_EL1_SNSMEM_MASK | \
	ID_AA64MMFR0_EL1_BIGENDEL0_MASK | \
	ID_AA64MMFR0_EL1_EXS_MASK \
	)

/*
 * Restrict to the following *unsigned* features for protected VMs:
 * - 40-bit IPA
 * - 16-bit ASID
 */
#define PVM_ID_AA64MMFR0_RESTRICT_UNSIGNED (\
	SYS_FIELD_PREP_ENUM(ID_AA64MMFR0_EL1, PARANGE, 40) |	\
	SYS_FIELD_PREP_ENUM(ID_AA64MMFR0_EL1, ASIDBITS, 16)	\
	)

/*
 * Allow for protected VMs:
 * - Hardware translation table updates to Access flag and Dirty state
 * - Number of VMID bits from CPU
 * - Hierarchical Permission Disables
 * - Privileged Access Never
 * - SError interrupt exceptions from speculative reads
 * - Enhanced Translation Synchronization
 */
#define PVM_ID_AA64MMFR1_ALLOW (\
	ID_AA64MMFR1_EL1_HAFDBS_MASK | \
	ID_AA64MMFR1_EL1_VMIDBits_MASK | \
	ID_AA64MMFR1_EL1_HPDS_MASK | \
	ID_AA64MMFR1_EL1_PAN_MASK | \
	ID_AA64MMFR1_EL1_SpecSEI_MASK | \
	ID_AA64MMFR1_EL1_ETS_MASK \
	)

/*
 * Allow for protected VMs:
 * - Common not Private translations
 * - User Access Override
 * - IESB bit in the SCTLR_ELx registers
 * - Unaligned single-copy atomicity and atomic functions
 * - ESR_ELx.EC value on an exception by read access to feature ID space
 * - TTL field in address operations.
 * - Break-before-make sequences when changing translation block size
 * - E0PDx mechanism
 */
#define PVM_ID_AA64MMFR2_ALLOW (\
	ID_AA64MMFR2_EL1_CnP_MASK | \
	ID_AA64MMFR2_EL1_UAO_MASK | \
	ID_AA64MMFR2_EL1_IESB_MASK | \
	ID_AA64MMFR2_EL1_AT_MASK | \
	ID_AA64MMFR2_EL1_IDS_MASK | \
	ID_AA64MMFR2_EL1_TTL_MASK | \
	ID_AA64MMFR2_EL1_BBM_MASK | \
	ID_AA64MMFR2_EL1_E0PD_MASK \
	)

/*
 * No support for Scalable Vectors for protected VMs:
 *	Requires additional support from KVM, e.g., context-switching and
 *	trapping at EL2
 */
#define PVM_ID_AA64ZFR0_ALLOW (0ULL)

/*
 * No support for debug, including breakpoints, and watchpoints for protected
 * VMs:
 *	The Arm architecture mandates support for at least the Armv8 debug
 *	architecture, which would include at least 2 hardware breakpoints and
 *	watchpoints. Providing that support to protected guests adds
 *	considerable state and complexity. Therefore, the reserved value of 0 is
 *	used for debug-related fields.
 */
#define PVM_ID_AA64DFR0_ALLOW (0ULL)
#define PVM_ID_AA64DFR1_ALLOW (0ULL)

/*
 * No support for implementation defined features.
 */
#define PVM_ID_AA64AFR0_ALLOW (0ULL)
#define PVM_ID_AA64AFR1_ALLOW (0ULL)

/*
 * No restrictions on instructions implemented in AArch64.
 */
#define PVM_ID_AA64ISAR0_ALLOW (\
	ID_AA64ISAR0_EL1_AES_MASK | \
	ID_AA64ISAR0_EL1_SHA1_MASK | \
	ID_AA64ISAR0_EL1_SHA2_MASK | \
	ID_AA64ISAR0_EL1_CRC32_MASK | \
	ID_AA64ISAR0_EL1_ATOMIC_MASK | \
	ID_AA64ISAR0_EL1_RDM_MASK | \
	ID_AA64ISAR0_EL1_SHA3_MASK | \
	ID_AA64ISAR0_EL1_SM3_MASK | \
	ID_AA64ISAR0_EL1_SM4_MASK | \
	ID_AA64ISAR0_EL1_DP_MASK | \
	ID_AA64ISAR0_EL1_FHM_MASK | \
	ID_AA64ISAR0_EL1_TS_MASK | \
	ID_AA64ISAR0_EL1_TLB_MASK | \
	ID_AA64ISAR0_EL1_RNDR_MASK \
	)

#define PVM_ID_AA64ISAR1_ALLOW (\
	ID_AA64ISAR1_EL1_DPB_MASK | \
	ID_AA64ISAR1_EL1_APA_MASK | \
	ID_AA64ISAR1_EL1_API_MASK | \
	ID_AA64ISAR1_EL1_JSCVT_MASK | \
	ID_AA64ISAR1_EL1_FCMA_MASK | \
	ID_AA64ISAR1_EL1_LRCPC_MASK | \
	ID_AA64ISAR1_EL1_GPA_MASK | \
	ID_AA64ISAR1_EL1_GPI_MASK | \
	ID_AA64ISAR1_EL1_FRINTTS_MASK | \
	ID_AA64ISAR1_EL1_SB_MASK | \
	ID_AA64ISAR1_EL1_SPECRES_MASK | \
	ID_AA64ISAR1_EL1_BF16_MASK | \
	ID_AA64ISAR1_EL1_DGH_MASK | \
	ID_AA64ISAR1_EL1_I8MM_MASK \
	)

#define PVM_ID_AA64ISAR2_ALLOW (\
	ID_AA64ISAR2_EL1_GPA3_MASK | \
	ID_AA64ISAR2_EL1_APA3_MASK \
	)

u64 pvm_read_id_reg(const struct kvm_vcpu *vcpu, u32 id);
bool kvm_handle_pvm_sysreg(struct kvm_vcpu *vcpu, u64 *exit_code);
bool kvm_handle_pvm_restricted(struct kvm_vcpu *vcpu, u64 *exit_code);
int kvm_check_pvm_sysreg_table(void);

#endif /* __ARM64_KVM_FIXED_CONFIG_H__ */
