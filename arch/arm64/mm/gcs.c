// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/syscalls.h>
#include <linux/types.h>

#include <asm/cpufeature.h>
#include <asm/page.h>

static unsigned long alloc_gcs(unsigned long addr, unsigned long size)
{
	int flags = MAP_ANONYMOUS | MAP_PRIVATE;
	struct mm_struct *mm = current->mm;
	unsigned long mapped_addr, unused;

	if (addr)
		flags |= MAP_FIXED_NOREPLACE;

	mmap_write_lock(mm);
	mapped_addr = do_mmap(NULL, addr, size, PROT_READ, flags,
			      VM_SHADOW_STACK | VM_WRITE, 0, &unused, NULL);
	mmap_write_unlock(mm);

	return mapped_addr;
}

static unsigned long gcs_size(unsigned long size)
{
	if (size)
		return PAGE_ALIGN(size);

	/* Allocate RLIMIT_STACK/2 with limits of PAGE_SIZE..2G */
	size = PAGE_ALIGN(min_t(unsigned long long,
				rlimit(RLIMIT_STACK) / 2, SZ_2G));
	return max(PAGE_SIZE, size);
}

static bool gcs_consume_token(struct mm_struct *mm, unsigned long user_addr)
{
	u64 expected = GCS_CAP(user_addr);
	u64 val;
	int ret;

	/* This should really be an atomic cmpxchg.  It is not. */
	ret = access_remote_vm(mm, user_addr, &val, sizeof(val),
			       FOLL_FORCE);
	if (ret != sizeof(val))
		return false;

	if (val != expected)
		return false;

	val = 0;
	ret = access_remote_vm(mm, user_addr, &val, sizeof(val),
			       FOLL_FORCE | FOLL_WRITE);
	if (ret != sizeof(val))
		return false;

	return true;
}

int arch_shstk_post_fork(struct task_struct *tsk,
			 struct kernel_clone_args *args)
{
	struct mm_struct *mm;
	unsigned long addr, size, gcspr_el0;
	int ret = 0;

	mm = get_task_mm(tsk);
	if (!mm)
		return -EFAULT;

	addr = args->shadow_stack;
	size = args->shadow_stack_size;

	/*
	 * There should be a token, and there is likely to be an optional
	 * end of stack marker above it.
	 */
	gcspr_el0 = addr + size - (2 * sizeof(u64));
	if (!gcs_consume_token(mm, gcspr_el0)) {
		gcspr_el0 += sizeof(u64);
		if (!gcs_consume_token(mm, gcspr_el0)) {
			ret = -EINVAL;
			goto out;
		}
	}

	tsk->thread.gcspr_el0 = gcspr_el0 + sizeof(u64);

out:
	mmput(mm);

	return ret;
}

unsigned long gcs_alloc_thread_stack(struct task_struct *tsk,
				     const struct kernel_clone_args *args)
{
	unsigned long addr, size;

	/* If the user specified a GCS use it. */
	if (args->shadow_stack_size) {
		if (!system_supports_gcs())
			return (unsigned long)ERR_PTR(-EINVAL);

		/* GCSPR_EL0 will be set up when verifying token post fork */
		addr = args->shadow_stack;
	} else {

		/*
		 * Otherwise fall back to legacy clone() support and
		 * implicitly allocate a GCS if we need a new one.
		 */

		if (!system_supports_gcs())
			return 0;

		if (!task_gcs_el0_enabled(tsk))
			return 0;

		if ((args->flags & (CLONE_VFORK | CLONE_VM)) != CLONE_VM) {
			tsk->thread.gcspr_el0 = read_sysreg_s(SYS_GCSPR_EL0);
			return 0;
		}

		size = args->stack_size;

		size = gcs_size(size);
		addr = alloc_gcs(0, size);
		if (IS_ERR_VALUE(addr))
			return addr;

		tsk->thread.gcs_base = addr;
		tsk->thread.gcs_size = size;
		tsk->thread.gcspr_el0 = addr + size - sizeof(u64);
	}

	return addr;
}

SYSCALL_DEFINE3(map_shadow_stack, unsigned long, addr, unsigned long, size, unsigned int, flags)
{
	unsigned long alloc_size;
	unsigned long __user *cap_ptr;
	unsigned long cap_val;
	int ret = 0;
	int cap_offset;

	if (!system_supports_gcs())
		return -EOPNOTSUPP;

	if (flags & ~(SHADOW_STACK_SET_TOKEN | SHADOW_STACK_SET_MARKER))
		return -EINVAL;

	if (addr && (addr % PAGE_SIZE))
		return -EINVAL;

	if (size == 8 || size % 8)
		return -EINVAL;

	/*
	 * An overflow would result in attempting to write the restore token
	 * to the wrong location. Not catastrophic, but just return the right
	 * error code and block it.
	 */
	alloc_size = PAGE_ALIGN(size);
	if (alloc_size < size)
		return -EOVERFLOW;

	addr = alloc_gcs(addr, alloc_size);
	if (IS_ERR_VALUE(addr))
		return addr;

	/*
	 * Put a cap token at the end of the allocated region so it
	 * can be switched to.
	 */
	if (flags & SHADOW_STACK_SET_TOKEN) {
		/* Leave an extra empty frame as a top of stack marker? */
		if (flags & SHADOW_STACK_SET_MARKER)
			cap_offset = 2;
		else
			cap_offset = 1;

		cap_ptr = (unsigned long __user *)(addr + size -
						   (cap_offset * sizeof(unsigned long)));
		cap_val = GCS_CAP(cap_ptr);

		put_user_gcs(cap_val, cap_ptr, &ret);
		if (ret != 0) {
			vm_munmap(addr, size);
			return -EFAULT;
		}

		/* Ensure the new cap is viaible for GCS */
		gcsb_dsync();
	}

	return addr;
}

/*
 * Apply the GCS mode configured for the specified task to the
 * hardware.
 */
void gcs_set_el0_mode(struct task_struct *task)
{
	u64 gcscre0_el1 = GCSCRE0_EL1_nTR;

	if (task->thread.gcs_el0_mode & PR_SHADOW_STACK_ENABLE)
		gcscre0_el1 |= GCSCRE0_EL1_RVCHKEN | GCSCRE0_EL1_PCRSEL;

	if (task->thread.gcs_el0_mode & PR_SHADOW_STACK_WRITE)
		gcscre0_el1 |= GCSCRE0_EL1_STREn;

	if (task->thread.gcs_el0_mode & PR_SHADOW_STACK_PUSH)
		gcscre0_el1 |= GCSCRE0_EL1_PUSHMEn;

	write_sysreg_s(gcscre0_el1, SYS_GCSCRE0_EL1);
}

void gcs_free(struct task_struct *task)
{

	/*
	 * When fork() with CLONE_VM fails, the child (tsk) already
	 * has a GCS allocated, and exit_thread() calls this function
	 * to free it.  In this case the parent (current) and the
	 * child share the same mm struct.
	 */
	if (!task->mm || task->mm != current->mm)
		return;

	if (task->thread.gcs_base)
		vm_munmap(task->thread.gcs_base, task->thread.gcs_size);

	task->thread.gcspr_el0 = 0;
	task->thread.gcs_base = 0;
	task->thread.gcs_size = 0;
}

int arch_set_shadow_stack_status(struct task_struct *task, unsigned long arg)
{
	unsigned long gcs, size;
	int ret;

	if (!system_supports_gcs())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(task)))
		return -EINVAL;

	/* Reject unknown flags */
	if (arg & ~PR_SHADOW_STACK_SUPPORTED_STATUS_MASK)
		return -EINVAL;

	ret = gcs_check_locked(task, arg);
	if (ret != 0)
		return ret;

	/* If we are enabling GCS then make sure we have a stack */
	if (arg & PR_SHADOW_STACK_ENABLE) {
		if (!task_gcs_el0_enabled(task)) {
			/* Do not allow GCS to be reenabled */
			if (task->thread.gcs_base)
				return -EINVAL;

			if (task != current)
				return -EBUSY;

			size = gcs_size(0);
			gcs = alloc_gcs(0, size);
			if (!gcs)
				return -ENOMEM;

			task->thread.gcspr_el0 = gcs + size - sizeof(u64);
			task->thread.gcs_base = gcs;
			task->thread.gcs_size = size;
			if (task == current)
				write_sysreg_s(task->thread.gcspr_el0,
					       SYS_GCSPR_EL0);

		}
	}

	task->thread.gcs_el0_mode = arg;
	if (task == current)
		gcs_set_el0_mode(task);

	return 0;
}

int arch_get_shadow_stack_status(struct task_struct *task,
				 unsigned long __user *arg)
{
	if (!system_supports_gcs())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(task)))
		return -EINVAL;

	return put_user(task->thread.gcs_el0_mode, arg);
}

int arch_lock_shadow_stack_status(struct task_struct *task,
				  unsigned long arg)
{
	if (!system_supports_gcs())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(task)))
		return -EINVAL;

	/*
	 * We support locking unknown bits so applications can prevent
	 * any changes in a future proof manner.
	 */
	task->thread.gcs_el0_locked |= arg;

	return 0;
}
