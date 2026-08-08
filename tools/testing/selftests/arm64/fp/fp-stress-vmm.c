// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal arm64 KVM VMM used to run fp-stress test loads in VMs.
 *
 * Runs a single vCPU guest loaded from a flat binary read from a file, with
 * PSCI 0.2 enabled so the guest can shut itself down via SYSTEM_OFF and a
 * simple MMIO console for guest output.
 *
 * Unhandled guest exceptions are reported via the guest's vector
 * table: each entry is a BRK instruction and we use software * breakpoint
 * debugging to us with KVM_EXIT_DEBUG.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/kvm.h>

#include <asm/sigcontext.h>

#include <asm/brk-imm.h>
#include <asm/esr.h>
#include <asm/sysreg.h>

#include "fp-stress-vmm.h"

#define ARM64_CORE_REG(name)					\
	(KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |	\
	 KVM_REG_ARM_CORE_REG(name))

#define CPACR_EL1	ARM64_SYS_REG(3, 0, 1, 0, 2)
#define CPTR_EL2	ARM64_SYS_REG(3, 4, 1, 1, 2)
#define ELR_EL2		ARM64_SYS_REG(3, 4, 4, 0, 1)
#define ESR_EL1		ARM64_SYS_REG(3, 0, 5, 2, 0)
#define ESR_EL2		ARM64_SYS_REG(3, 4, 5, 2, 0)
#define FAR_EL1		ARM64_SYS_REG(3, 0, 6, 0, 0)
#define FAR_EL2		ARM64_SYS_REG(3, 4, 6, 0, 0)
#define HCR_EL2		ARM64_SYS_REG(3, 4, 1, 1, 0)
#define ZCR_EL1		ARM64_SYS_REG(3, 0, 1, 2, 0)
#define ZCR_EL2		ARM64_SYS_REG(3, 4, 1, 2, 0)

/* Both halves of the field, as the guest's own no-trapping value */
#define CPACR_EL1_FPEN	(CPACR_EL1_FPEN_EL1EN | CPACR_EL1_FPEN_EL0EN)
#define CPACR_EL1_ZEN	(CPACR_EL1_ZEN_EL1EN | CPACR_EL1_ZEN_EL0EN)

/* We should fix the build system to let us get these from sysreg-defs.h */
#define HCR_EL2_E2H	BIT(34)
#define ZCR_ELx_LEN	0xf

/* VGIC MMIO ranges, below the console and guest RAM */
#define VGIC_DIST_BASE		0x08000000
#define VGIC_REDIST_BASE	0x08010000

/* The vector length set is a bitmap of vector quadwords, index into it */
#define vq_word(vq)	(((vq) - KVM_ARM64_SVE_VQ_MIN) / 64)
#define vq_mask(vq)	(1ULL << (((vq) - KVM_ARM64_SVE_VQ_MIN) % 64))
#define vq_present(vqs, vq) (!!((vqs)[vq_word(vq)] & vq_mask(vq)))

/* The guest's vector entries BRK with their entry number as immediate */
#define NR_VECTORS	16

/* The vCPU our signal handler reports the state of, -1 until we have one */
static int vcpu_fd = -1;

static bool guest_pkvm;
static bool guest_el2;

/* SVE vector length in bits for the guest, 0 for no SVE, from --sve */
static int guest_sve_vl;

static const char *const vector_origins[] = {
	"current EL, SP_EL0",
	"current EL, SP_ELx",
	"lower EL, AArch64",
	"lower EL, AArch32",
};

static const char *const vector_kinds[] = {
	"synchronous", "IRQ", "FIQ", "SError",
};

static void __attribute__((noreturn, format(printf, 1, 2)))
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');

	exit(EXIT_FAILURE);
}

static void __attribute__((noreturn)) die_perror(const char *what)
{
	die("%s: %d (%s)", what, errno, strerror(errno));
}

static void *load_guest(const char *path, size_t *ram_size)
{
	struct stat st;
	long page_size;
	ssize_t ret;
	size_t off;
	void *ram;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		die("%s: %s", path, strerror(errno));

	if (fstat(fd, &st))
		die_perror("fstat");

	if (!st.st_size)
		die("%s: image is empty", path);

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		die_perror("sysconf(_SC_PAGESIZE)");

	/* Guest memory is allocated in whole pages */
	*ram_size = ((size_t)st.st_size + GUEST_EXTRA_RAM + page_size - 1) &
		~((size_t)page_size - 1);

	ram = mmap(NULL, *ram_size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ram == MAP_FAILED)
		die_perror("mmap(guest RAM)");

	for (off = 0; off < (size_t)st.st_size; off += ret) {
		ret = read(fd, (char *)ram + off, st.st_size - off);
		if (ret < 0)
			die_perror("read");
		if (!ret)
			die("%s: short read", path);
	}

	close(fd);

	return ram;
}

static uint64_t get_reg(int vcpu_fd, uint64_t id)
{
	uint64_t val;
	struct kvm_one_reg reg = {
		.id = id,
		.addr = (uint64_t)&val,
	};

	if (ioctl(vcpu_fd, KVM_GET_ONE_REG, &reg))
		die_perror("KVM_GET_ONE_REG");

	return val;
}

/*
 * The guest shouldn't be doing anything that results in us seeing an
 * exception, report anything we do see as an error.
 */
static void __attribute__((noreturn))
report_guest_exception(int vcpu_fd, unsigned int vector)
{
	uint64_t esr, elr, far, ec;
	int el;

	if (guest_el2) {
		esr = get_reg(vcpu_fd, ESR_EL2);
		elr = get_reg(vcpu_fd, ELR_EL2);
		far = get_reg(vcpu_fd, FAR_EL2);
		el = 2;
	} else {
		esr = get_reg(vcpu_fd, ESR_EL1);
		elr = get_reg(vcpu_fd, ARM64_CORE_REG(elr_el1));
		far = get_reg(vcpu_fd, FAR_EL1);
		el = 1;
	}

	ec = ESR_ELx_EC(esr);

	printf("Unhandled guest exception: %s, %s\n",
	       vector_kinds[vector % 4], vector_origins[vector / 4]);
	printf("  ESR_EL%d 0x%016lx (EC 0x%02lx)\n", el, esr, ec);
	printf("  ELR_EL%d 0x%016lx FAR_EL%d 0x%016lx\n", el, elr, el, far);

	exit(EXIT_FAILURE);
}

/*
 * The natively built loads are poked with SIGUSR1 and SIGUSR2 while they
 * run and count what they were sent in x23.  Those signals come to us
 * rather than the guest so keep the count out here instead, the guest
 * leaves its own x23 at zero.
 */
static volatile sig_atomic_t signal_count;

static void handle_count_signal(int sig)
{
	signal_count++;
}

/*
 * We were asked to stop, report where the guest got to and exit
 * successfully in the same form as the natively built loads do from
 * their own terminate handler.  They keep their iteration count in x22.
 */
static void handle_exit_signal(int sig)
{
	uint64_t iterations = 0;
	char buf[128];
	int len;

	/* We can be signalled before there is a vCPU */
	if (vcpu_fd >= 0)
		iterations = get_reg(vcpu_fd, ARM64_CORE_REG(regs.regs[22]));

	len = snprintf(buf, sizeof(buf),
		       "Terminated by signal %d, iterations=%llu, signals=%llu\n",
		       sig, (unsigned long long)iterations,
		       (unsigned long long)signal_count);
	write(STDOUT_FILENO, buf, len);

	_exit(EXIT_SUCCESS);
}

/*
 * Arrange for SIGTERM and SIGINT to stop us, reporting where the guest
 * got to rather than dying on the spot, and for SIGUSR1 and SIGUSR2 to
 * be counted.  Done before we have a VM so that we do not die by default
 * action if we are signalled while setting one up.
 */
static void setup_signals(void)
{
	struct sigaction stop = { .sa_handler = handle_exit_signal };
	struct sigaction count = { .sa_handler = handle_count_signal };

	sigemptyset(&stop.sa_mask);
	if (sigaction(SIGTERM, &stop, NULL) || sigaction(SIGINT, &stop, NULL))
		die_perror("sigaction");

	sigemptyset(&count.sa_mask);
	sigaddset(&count.sa_mask, SIGUSR1);
	sigaddset(&count.sa_mask, SIGUSR2);
	if (sigaction(SIGUSR1, &count, NULL) || sigaction(SIGUSR2, &count, NULL))
		die_perror("sigaction");
}

static void set_reg(int vcpu_fd, uint64_t id, uint64_t val)
{
	struct kvm_one_reg reg = {
		.id = id,
		.addr = (uint64_t)&val,
	};

	if (ioctl(vcpu_fd, KVM_SET_ONE_REG, &reg))
		die_perror("KVM_SET_ONE_REG");
}

static void set_device_attr(int fd, uint32_t group, uint64_t attr,
			    void *addr)
{
	struct kvm_device_attr device_attr = {
		.group = group,
		.attr = attr,
		.addr = (uint64_t)addr,
	};

	if (ioctl(fd, KVM_SET_DEVICE_ATTR, &device_attr))
		die_perror("KVM_SET_DEVICE_ATTR");
}

/*
 * For NV we need a vGIC since KVM uses it to provide the virtual
 * maintenance interrupt,
 */
static void setup_vgic(int vm_fd)
{
	struct kvm_create_device device = {
		.type = KVM_DEV_TYPE_ARM_VGIC_V3,
	};
	uint64_t addr;

	if (ioctl(vm_fd, KVM_CREATE_DEVICE, &device))
		die_perror("KVM_CREATE_DEVICE(VGICv3)");

	addr = VGIC_DIST_BASE;
	set_device_attr(device.fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
			KVM_VGIC_V3_ADDR_TYPE_DIST, &addr);

	addr = VGIC_REDIST_BASE;
	set_device_attr(device.fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
			KVM_VGIC_V3_ADDR_TYPE_REDIST, &addr);

	set_device_attr(device.fd, KVM_DEV_ARM_VGIC_GRP_CTRL,
			KVM_DEV_ARM_VGIC_CTRL_INIT, NULL);
	close(device.fd);
}

/*
 * Tell KVM which vector lengths the guest may use.  It can only limit
 * the maximum rather than hide individual VLs, the set we ask for has to
 * match the ones the host has exactly up to that maximum, so start from
 * what KVM is offering us and mask off everything above what we want.
 */
static void setup_sve(int vl)
{
	__u64 vqs[KVM_ARM64_SVE_VLS_WORDS];
	struct kvm_one_reg reg = {
		.id = KVM_REG_ARM64_SVE_VLS,
		.addr = (uint64_t)vqs,
	};
	/* Our VLs are in bits, the vector quadword helpers work in bytes */
	unsigned int max_vq = sve_vq_from_vl(vl / 8);
	int feature = KVM_ARM_VCPU_SVE;
	unsigned int vq;

	if (ioctl(vcpu_fd, KVM_GET_ONE_REG, &reg))
		die_perror("KVM_GET_ONE_REG(SVE_VLS)");

	if (!vq_present(vqs, max_vq))
		die("KVM has no SVE VL %d", vl);

	for (vq = max_vq + 1; vq <= KVM_ARM64_SVE_VQ_MAX; vq++)
		vqs[vq_word(vq)] &= ~vq_mask(vq);

	if (ioctl(vcpu_fd, KVM_SET_ONE_REG, &reg))
		die_perror("KVM_SET_ONE_REG(SVE_VLS)");

	if (ioctl(vcpu_fd, KVM_ARM_VCPU_FINALIZE, &feature))
		die_perror("KVM_ARM_VCPU_FINALIZE(SVE)");

	/* Rely on the VM level configuration to constrain the VL */
	if (guest_el2)
		set_reg(vcpu_fd, ZCR_EL2, ZCR_ELx_LEN);
	set_reg(vcpu_fd, ZCR_EL1, ZCR_ELx_LEN);
}

static void setup_vm(const char *path, struct kvm_run **run)
{
	struct kvm_userspace_memory_region mem;
	struct kvm_guest_debug dbg;
	struct kvm_vcpu_init init;
	uint64_t cpacr, pstate;
	unsigned long vm_type;
	int kvm_fd, vm_fd;
	size_t ram_size;
	long mmap_size;
	void *ram;

	kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm_fd < 0)
		die_perror("/dev/kvm");

	if (ioctl(kvm_fd, KVM_GET_API_VERSION, 0) != KVM_API_VERSION)
		die("KVM API version mismatch");

	if (guest_pkvm)
		vm_type = KVM_VM_TYPE_ARM_PROTECTED;
	else
		vm_type = 0;
	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, vm_type);
	if (vm_fd < 0)
		die_perror("KVM_CREATE_VM");

	ram = load_guest(path, &ram_size);

	mem = (struct kvm_userspace_memory_region) {
		.slot = 0,
		.guest_phys_addr = GUEST_RAM_BASE,
		.memory_size = ram_size,
		.userspace_addr = (uint64_t)ram,
	};
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &mem))
		die_perror("KVM_SET_USER_MEMORY_REGION");

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU");

	if (ioctl(vm_fd, KVM_ARM_PREFERRED_TARGET, &init))
		die_perror("KVM_ARM_PREFERRED_TARGET");

	if (!ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PSCI_0_2))
		die("KVM_CAP_ARM_PSCI_0_2 not supported");
	init.features[0] |= 1UL << KVM_ARM_VCPU_PSCI_0_2;

	/*
	 * Running the guest at EL2 gets us coverage of the nested paths
	 * through the floating point context switching.
	 */
	if (guest_el2) {
		if (!ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_EL2))
			die("KVM_CAP_ARM_EL2 not supported");
		init.features[0] |= 1UL << KVM_ARM_VCPU_HAS_EL2;
	}

	if (guest_sve_vl) {
		if (!ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_SVE))
			die("KVM_CAP_ARM_SVE not supported");
		init.features[0] |= 1UL << KVM_ARM_VCPU_SVE;
	}

	if (ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init))
		die_perror("KVM_ARM_VCPU_INIT");

	/*
	 * A vGIC is only needed for EL2 guests so skip otherwise,
	 * avoids issues with configuring on GICv2 systems.
	 */
	if (guest_el2)
		setup_vgic(vm_fd);

	if (guest_sve_vl)
		setup_sve(guest_sve_vl);

	set_reg(vcpu_fd, ARM64_CORE_REG(regs.pc), GUEST_RAM_BASE);

	/*
	 * Disable FP traps for the guest, we set things up here since it's
	 * easier than looking at ID registers in the guest.
	 */
	if (guest_el2) {
		cpacr = get_reg(vcpu_fd, CPTR_EL2) | CPACR_EL1_FPEN;
		if (guest_sve_vl)
			cpacr |= CPACR_EL1_ZEN;
		set_reg(vcpu_fd, CPTR_EL2, cpacr);

	}
	cpacr = get_reg(vcpu_fd, CPACR_EL1) | CPACR_EL1_FPEN;
	if (guest_sve_vl)
		cpacr |= CPACR_EL1_ZEN;
	set_reg(vcpu_fd, CPACR_EL1, cpacr);

	/*
	 * An EL2 guest needs to be in VHE mode, everything it sets up
	 * uses the EL1 registers and those only reach the EL2 ones it is
	 * really running on while E2H is set.  KVM only sets E2H for us
	 * at reset on hardware without FEAT_E2H0.
	 */
	if (guest_el2)
		set_reg(vcpu_fd, HCR_EL2,
			get_reg(vcpu_fd, HCR_EL2) | HCR_EL2_E2H);

	/*
	 * Guests can't use getpid() to disambiguate their data, let's call
	 * it for them.
	 */
	set_reg(vcpu_fd, ARM64_CORE_REG(regs.regs[20]), getpid());

	/*
	 * Trap guest BRKs to us so the guest's vector stubs can report
	 * unhandled exceptions.
	 */
	if (!ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG))
		die("KVM_CAP_SET_GUEST_DEBUG not supported");

	dbg = (struct kvm_guest_debug) {
		.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP,
	};
	if (ioctl(vcpu_fd, KVM_SET_GUEST_DEBUG, &dbg))
		die_perror("KVM_SET_GUEST_DEBUG");

	mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE");

	*run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    vcpu_fd, 0);
	if (*run == MAP_FAILED)
		die_perror("mmap(kvm_run)");

	/* Mask everything by default, and start in the expected mode */
	pstate = PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;
	if (guest_el2)
		pstate |= PSR_MODE_EL2h;
	else
		pstate |= PSR_MODE_EL1h;
	set_reg(vcpu_fd, ARM64_CORE_REG(regs.pstate), pstate);
}

static const struct option options[] = {
	{ "el1", no_argument, NULL, 'e' },
	{ "el2", no_argument, NULL, 'E' },
	{ "protected", no_argument, NULL, 'p' },
	{ "sve", required_argument, NULL, 's' },
	{ }
};

int main(int argc, char *argv[])
{
	struct kvm_run *run;
	int c;

	while ((c = getopt_long(argc, argv, "es:", options, NULL)) != -1) {
		switch (c) {
		case 'e':
			break;
		case 'E':
			guest_el2 = true;
			break;
		case 'p':
			guest_pkvm = true;
			break;
		case 's':
			if (sscanf(optarg, "%d", &guest_sve_vl) != 1)
				die("Failed to parse SVE VL %s", optarg);
			if (guest_sve_vl < 128 || guest_sve_vl % 128 ||
			    guest_sve_vl / 8 > __SVE_VL_MAX)
				die("Invalid SVE VL %d", guest_sve_vl);
			break;
		default:
			die("Usage: %s [--el2] [--sve VL] <guest flat binary>",
			    argv[0]);
		}
	}

	if (optind != argc - 1)
		die("Usage: %s [--el2] [--protected] [--sve VL] <guest image>",
		    argv[0]);

	/* Ensure output is unbuffered to get it to the monitor promptly */
	setvbuf(stdout, NULL, _IONBF, 0);

	setup_signals();

	setup_vm(argv[optind], &run);

	for (;;) {
		if (ioctl(vcpu_fd, KVM_RUN, 0)) {
			/*
			 * The signals we handle do not come back here,
			 * anything else that interrupts us can just go
			 * round again.
			 */
			if (errno == EINTR)
				continue;

			die_perror("KVM_RUN");
		}

		switch (run->exit_reason) {
		case KVM_EXIT_MMIO:
			if (!run->mmio.is_write ||
			    run->mmio.phys_addr != CONSOLE_BASE ||
			    run->mmio.len != 1)
				die("Unhandled MMIO %s at 0x%llx len %u",
				    run->mmio.is_write ? "write" : "read",
				    run->mmio.phys_addr, run->mmio.len);

			putchar(run->mmio.data[0]);
			break;
		case KVM_EXIT_DEBUG: {
			uint32_t esr = run->debug.arch.hsr;

			if (ESR_ELx_EC(esr) != ESR_ELx_EC_BRK64 ||
			    esr_brk_comment(esr) >= NR_VECTORS)
				die("Unexpected debug exit, ESR 0x%x", esr);

			report_guest_exception(vcpu_fd, esr_brk_comment(esr));
		}
		case KVM_EXIT_SYSTEM_EVENT:
			switch (run->system_event.type) {
			case KVM_SYSTEM_EVENT_SHUTDOWN:
				/*
				 * The guest should report errors by logging
				 * and shutting down, add our own log in case
				 * we get here without a log.
				 */
				die("Guest requested shutdown");
			default:
				die("Unexpected system event %u",
				    run->system_event.type);
			}
			break;
		default:
			die("Unexpected exit reason %u", run->exit_reason);
		}
	}

	/* We should never get here */
	return EXIT_FAILURE;
}
