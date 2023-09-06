// SPDX-License-Identifier: GPL-2.0
/*
 * arch/arm64/kvm/fpsimd.c: Guest/host FPSIMD context coordination helpers
 *
 * Copyright 2018 Arm Limited
 * Author: Dave Martin <Dave.Martin@arm.com>
 */
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/kvm_host.h>
#include <asm/fpsimd.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/sysreg.h>

/* We present Z and P to userspace with the maximum of the SVE or SME VL */
int vcpu_max_vq(struct kvm_vcpu *vcpu)
{
	int sve, sme;

	if (vcpu_has_sve(vcpu))
		sve = vcpu_sve_max_vq(vcpu);
	else
		sve = 0;

	if (vcpu_has_sme(vcpu))
		sme = vcpu_sme_max_vq(vcpu);
	else
		sme = 0;

	return max(sve, sme);
}

void kvm_vcpu_unshare_task_fp(struct kvm_vcpu *vcpu)
{
	struct task_struct *p = vcpu->arch.parent_task;
	struct user_fpsimd_state *fpsimd;

	if (!is_protected_kvm_enabled() || !p)
		return;

	fpsimd = &p->thread.uw.fpsimd_state;
	kvm_unshare_hyp(fpsimd, fpsimd + 1);
	put_task_struct(p);
}

/*
 * Called on entry to KVM_RUN unless this vcpu previously ran at least
 * once and the most recent prior KVM_RUN for this vcpu was called from
 * the same task as current (highly likely).
 *
 * This is guaranteed to execute before kvm_arch_vcpu_load_fp(vcpu),
 * such that on entering hyp the relevant parts of current are already
 * mapped.
 */
int kvm_arch_vcpu_run_map_fp(struct kvm_vcpu *vcpu)
{
	int ret;

	struct user_fpsimd_state *fpsimd = &current->thread.uw.fpsimd_state;

	kvm_vcpu_unshare_task_fp(vcpu);

	/* Make sure the host task fpsimd state is visible to hyp: */
	ret = kvm_share_hyp(fpsimd, fpsimd + 1);
	if (ret)
		return ret;

	vcpu->arch.host_fpsimd_state = kern_hyp_va(fpsimd);

	/*
	 * We need to keep current's task_struct pinned until its data has been
	 * unshared with the hypervisor to make sure it is not re-used by the
	 * kernel and donated to someone else while already shared -- see
	 * kvm_vcpu_unshare_task_fp() for the matching put_task_struct().
	 */
	if (is_protected_kvm_enabled()) {
		get_task_struct(current);
		vcpu->arch.parent_task = current;
	}

	return 0;
}

static bool vcpu_fp_user_format_needed(struct kvm_vcpu *vcpu)
{
	/* Only systems with SME need rewrites */
	if (!system_supports_sme())
		return false;

	/*
	 * If we have both SVE and SME and the two VLs are the same
	 * and no rewrite is needed.
	 */
	if (vcpu_has_sve(vcpu) &&
	    (vcpu_sve_max_vq(vcpu) == vcpu_sme_max_vq(vcpu)))
		return false;

	return true;
}

static bool vcpu_sm_active(struct kvm_vcpu *vcpu)
{
	return __vcpu_sys_reg(vcpu, SVCR) & SVCR_SM;
}

static int vcpu_active_vq(struct kvm_vcpu *vcpu)
{
	if (vcpu_sm_active(vcpu))
		return vcpu_sme_max_vq(vcpu);
	else
		return vcpu_sve_max_vq(vcpu);
}

static void *buf_zreg(void *buf, int vq, int reg)
{
	return buf + __SVE_ZREG_OFFSET(vq, reg) - __SVE_ZREGS_OFFSET;
}

static void *buf_preg(void *buf, int vq, int reg)
{
	return buf + __SVE_PREG_OFFSET(vq, reg) - __SVE_ZREGS_OFFSET;
}

static void vcpu_rewrite_sve(struct kvm_vcpu *vcpu, int vq_in, int vq_out)
{
	void *new_buf;
	int copy_size, i;

	new_buf = kzalloc(vcpu_sve_state_size(vcpu), GFP_KERNEL);
	if (!new_buf)
		return;

	if (WARN_ON_ONCE(vq_in == vq_out))
		return;

	/* Z registers */
	if (vq_in < vq_out)
		copy_size = vq_in * __SVE_VQ_BYTES;
	else
		copy_size = vq_out * __SVE_VQ_BYTES;

	for (i = 0; i < SVE_NUM_ZREGS; i++)
		memcpy(buf_zreg(new_buf, vq_out, i),
		       buf_zreg(vcpu->arch.sve_state, vq_in, i),
		       copy_size);

	/* P and FFR, FFR is stored as an additional P */
	copy_size /= 8;
	for (i = 0; i <= SVE_NUM_PREGS; i++)
		memcpy(buf_preg(new_buf, vq_out, i),
		       buf_preg(vcpu->arch.sve_state, vq_in, i),
		       copy_size);

	/*
	 * Ideally we would unmap the existing SVE buffer and remap
	 * the new one.
	 */
	memcpy(vcpu->arch.sve_state, new_buf, vcpu_sve_state_size(vcpu));
	kfree(new_buf);
}

/*
 * If both SVE and SME are supported we present userspace with the SVE
 * Z, P and FFR registers configured with the larger of the SVE and
 * SME vector length, and if we have SME then even without SVE we
 * present the V registers via Z.
 */
static void vcpu_fp_user_to_guest(struct kvm_vcpu *vcpu)
{
	if (likely(vcpu->arch.fp_state != FP_STATE_USER_OWNED))
		return;

	if (!vcpu_fp_user_format_needed(vcpu)) {
		vcpu->arch.fp_state = FP_STATE_FREE;
		return;
	}

	if (vcpu_has_sve(vcpu)) {
		/*
		 * The register state is stored in SVE format, rewrite
		 * from the larger VL to the one the guest is
		 * currently using.
		 */
		if (vcpu_active_vq(vcpu) != vcpu_max_vq(vcpu))
			vcpu_rewrite_sve(vcpu, vcpu_max_vq(vcpu),
					 vcpu_active_vq(vcpu));
	} else {
		/*
		 * A FPSIMD only system will store non-streaming guest
		 * state in FPSIMD format when running the guest but
		 * present to userspace via the SVE regset.
		 */
		if (!vcpu_sm_active(vcpu))
			__sve_to_fpsimd(&vcpu->arch.ctxt.fp_regs,
					vcpu->arch.sve_state,
					vcpu_sme_max_vq(vcpu));
	}

	vcpu->arch.fp_state = FP_STATE_FREE;
}

void vcpu_fp_guest_to_user(struct kvm_vcpu *vcpu)
{
	if (vcpu->arch.fp_state == FP_STATE_USER_OWNED)
		return;

	if (!vcpu_fp_user_format_needed(vcpu))
		return;

	if (vcpu_has_sve(vcpu)) {
		/*
		 * The register state is stored in SVE format, rewrite
		 * to the largest VL.
		 */
		if (vcpu_active_vq(vcpu) != vcpu_max_vq(vcpu))
			vcpu_rewrite_sve(vcpu, vcpu_active_vq(vcpu),
					 vcpu_max_vq(vcpu));
	} else {
		/*
		 * A FPSIMD only system will store non-streaming guest
		 * state in FPSIMD format when running the guest but
		 * present to userspace via the SVE regset, rewrite
		 * with zero padding.
		 */
		if (!vcpu_sm_active(vcpu)) {
			memset(vcpu->arch.sve_state, 0,
			       vcpu_sve_state_size(vcpu));
			__fpsimd_to_sve(vcpu->arch.sve_state,
					&vcpu->arch.ctxt.fp_regs,
					vcpu_sme_max_vq(vcpu));
		}
	}

	vcpu->arch.fp_state = FP_STATE_USER_OWNED;
}

/*
 * Prepare vcpu for saving the host's FPSIMD state and loading the guest's.
 * The actual loading is done by the FPSIMD access trap taken to hyp.
 *
 * Here, we just set the correct metadata to indicate that the FPSIMD
 * state in the cpu regs (if any) belongs to current on the host.
 */
void kvm_arch_vcpu_load_fp(struct kvm_vcpu *vcpu)
{
	BUG_ON(!current->mm);

	if (!system_supports_fpsimd())
		return;

	fpsimd_kvm_prepare();

	vcpu_fp_user_to_guest(vcpu);

	/*
	 * We will check TIF_FOREIGN_FPSTATE just before entering the
	 * guest in kvm_arch_vcpu_ctxflush_fp() and override this to
	 * FP_STATE_FREE if the flag set.
	 */
	vcpu->arch.fp_state = FP_STATE_HOST_OWNED;

	vcpu_clear_flag(vcpu, HOST_SVE_ENABLED);
	if (read_sysreg(cpacr_el1) & CPACR_EL1_ZEN_EL0EN)
		vcpu_set_flag(vcpu, HOST_SVE_ENABLED);

	if (system_supports_sme()) {
		vcpu_clear_flag(vcpu, HOST_SME_ENABLED);
		if (read_sysreg(cpacr_el1) & CPACR_EL1_SMEN_EL0EN)
			vcpu_set_flag(vcpu, HOST_SME_ENABLED);

		/*
		 * If PSTATE.SM is enabled then save any pending FP
		 * state and disable PSTATE.SM. If we leave PSTATE.SM
		 * enabled and the guest does not enable SME via
		 * CPACR_EL1.SMEN then operations that should be valid
		 * may generate SME traps from EL1 to EL1 which we
		 * can't intercept and which would confuse the guest.
		 *
		 * Do the same for PSTATE.ZA in the case where there
		 * is state in the registers which has not already
		 * been saved, this is very unlikely to happen.
		 */
		if (read_sysreg_s(SYS_SVCR) & (SVCR_SM_MASK | SVCR_ZA_MASK)) {
			vcpu->arch.fp_state = FP_STATE_FREE;
			fpsimd_save_and_flush_cpu_state();
		}
	}
}

/*
 * Called just before entering the guest once we are no longer preemptable
 * and interrupts are disabled. If we have managed to run anything using
 * FP while we were preemptible (such as off the back of an interrupt),
 * then neither the host nor the guest own the FP hardware (and it was the
 * responsibility of the code that used FP to save the existing state).
 */
void kvm_arch_vcpu_ctxflush_fp(struct kvm_vcpu *vcpu)
{
	if (test_thread_flag(TIF_FOREIGN_FPSTATE))
		vcpu->arch.fp_state = FP_STATE_FREE;
}

/*
 * Called just after exiting the guest. If the guest FPSIMD state
 * was loaded, update the host's context tracking data mark the CPU
 * FPSIMD regs as dirty and belonging to vcpu so that they will be
 * written back if the kernel clobbers them due to kernel-mode NEON
 * before re-entry into the guest.
 */
void kvm_arch_vcpu_ctxsync_fp(struct kvm_vcpu *vcpu)
{
	struct cpu_fp_state fp_state;

	WARN_ON_ONCE(!irqs_disabled());

	if (vcpu->arch.fp_state == FP_STATE_GUEST_OWNED) {

		/*
		 * We peer into the registers since SVCR is saved as
		 * part of the floating point state, determining which
		 * registers exist and their size, so is saved by
		 * fpsimd_save().
		 */
		fp_state.st = &vcpu->arch.ctxt.fp_regs;
		fp_state.sve_state = vcpu->arch.sve_state;
		fp_state.sve_vl = vcpu->arch.max_vl[ARM64_VEC_SVE];
		fp_state.sme_vl = vcpu->arch.max_vl[ARM64_VEC_SME];
		fp_state.sme_state = vcpu->arch.sme_state;
		fp_state.svcr = &(vcpu->arch.ctxt.sys_regs[SVCR]);
		fp_state.fp_type = &vcpu->arch.fp_type;

		/*
		 * If we are in streaming mode PSTATE.SM will override
		 * FP_STATE_FPSIMD during save for SME only guests.
		 */
		if (vcpu_has_sve(vcpu))
			fp_state.to_save = FP_STATE_SVE;
		else
			fp_state.to_save = FP_STATE_FPSIMD;

		fpsimd_bind_state_to_cpu(&fp_state);

		clear_thread_flag(TIF_FOREIGN_FPSTATE);
	}
}

/*
 * Write back the vcpu FPSIMD regs if they are dirty, and invalidate the
 * cpu FPSIMD regs so that they can't be spuriously reused if this vcpu
 * disappears and another task or vcpu appears that recycles the same
 * struct fpsimd_state.
 */
void kvm_arch_vcpu_put_fp(struct kvm_vcpu *vcpu)
{
	unsigned long flags;
	u64 cpacr_set, cpacr_clear;
	u64 smcr_cur, smcr_new;

	local_irq_save(flags);

	if (vcpu->arch.fp_state == FP_STATE_GUEST_OWNED) {
		if (vcpu_has_sve(vcpu)) {
			__vcpu_sys_reg(vcpu, ZCR_EL1) = read_sysreg_el1(SYS_ZCR);

			/* Restore the VL that was saved when bound to the CPU */
			if (!has_vhe())
				sve_cond_update_zcr_vq(vcpu_sve_max_vq(vcpu) - 1,
						       SYS_ZCR_EL1);
		}

		if (vcpu_has_sme(vcpu)) {
			smcr_cur = read_sysreg_el1(SYS_SMCR);
			__vcpu_sys_reg(vcpu, SMCR_EL1) = smcr_cur;

			/* Restore the full VL and feature set */
			if (!has_vhe()) {
				smcr_new = vcpu_sme_max_vq(vcpu) - 1;

				if (system_supports_fa64())
					smcr_new |= SMCR_ELx_FA64;
				if (system_supports_sme2())
					smcr_new |= SMCR_ELx_EZT0_MASK;

				if (smcr_cur != smcr_new)
					write_sysreg_s(smcr_new, SYS_SMCR_EL1);
			}
		}

		fpsimd_save_and_flush_cpu_state();
	} else if (has_vhe()) {
		/*
		 * The FP state in the CPU has not been touched, and
		 * we have SVE or SME (and VHE): CPACR_EL1 (alias
		 * CPTR_EL2) has been reset by kvm_reset_cptr_el2() in
		 * the Hyp code, disabling SVE/SME for EL0.  To avoid
		 * spurious traps, restore the trap state seen by
		 * kvm_arch_vcpu_load_fp():
		 */

		cpacr_clear = 0;
		cpacr_set = 0;

		if (system_supports_sve()) {
			if (vcpu_get_flag(vcpu, HOST_SVE_ENABLED)) {
				cpacr_set |= CPACR_EL1_ZEN_EL0EN;
			} else {
				cpacr_clear |= CPACR_EL1_ZEN_EL0EN;
			}
		}

		if (system_supports_sme()) {
			if (vcpu_get_flag(vcpu, HOST_SME_ENABLED)) {
				cpacr_set |= CPACR_EL1_SMEN_EL0EN;
			} else {
				cpacr_clear |= CPACR_EL1_SMEN_EL0EN;
			}
		}

		if (cpacr_clear || cpacr_set)
			sysreg_clear_set(CPACR_EL1, cpacr_clear, cpacr_set);
	}

	local_irq_restore(flags);
}
