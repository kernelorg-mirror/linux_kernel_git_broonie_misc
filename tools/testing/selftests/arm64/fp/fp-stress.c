// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 ARM Limited.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <asm/hwcap.h>
#include <linux/kvm.h>

#include "kselftest.h"

#define MAX_VLS 16

#define SIGNAL_INTERVAL_MS 25
#define LOG_INTERVALS (1000 / SIGNAL_INTERVAL_MS)

struct child_data {
	char *name, *output;
	pid_t pid;
	int stdout;
	bool output_seen;
	bool exited;
	int exit_status;
};

static int epoll_fd;
static struct child_data *children;
static struct epoll_event *evs;
static int tests;
static int num_children;
static bool terminate;
static int sve_vl_count, sme_vl_count;
static int sve_vls[MAX_VLS], sme_vls[MAX_VLS];
static bool have_kvm;
static bool have_kvm_el2;
static bool have_kvm_sve;

static int startup_pipe[2];

static int num_processors(void)
{
	long nproc = sysconf(_SC_NPROCESSORS_CONF);
	if (nproc < 0) {
		perror("Unable to read number of processors\n");
		exit(EXIT_FAILURE);
	}

	return nproc;
}

static char *kvm_el_arg(int cpu)
{
	if (have_kvm_el2 && ((cpu % 4) == 3))
		return "--el2";
	else
		return "--el1";
}

static void child_start(struct child_data *child, char *const prog_args[])
{
	int ret, pipefd[2], i;
	struct epoll_event ev;

	ret = pipe(pipefd);
	if (ret != 0)
		ksft_exit_fail_msg("Failed to create stdout pipe: %s (%d)\n",
				   strerror(errno), errno);

	child->pid = fork();
	if (child->pid == -1)
		ksft_exit_fail_msg("fork() failed: %s (%d)\n",
				   strerror(errno), errno);

	if (!child->pid) {
		/*
		 * In child, replace stdout with the pipe, errors to
		 * stderr from here as kselftest prints to stdout.
		 */
		ret = dup2(pipefd[1], 1);
		if (ret == -1) {
			printf("dup2() %d\n", errno);
			exit(EXIT_FAILURE);
		}

		/*
		 * Duplicate the read side of the startup pipe to
		 * FD 3 so we can close everything else.
		 */
		ret = dup2(startup_pipe[0], 3);
		if (ret == -1) {
			printf("dup2() %d\n", errno);
			exit(EXIT_FAILURE);
		}

		/*
		 * Very dumb mechanism to clean open FDs other than
		 * stdio. We don't want O_CLOEXEC for the pipes...
		 */
		for (i = 4; i < 8192; i++)
			close(i);

		/*
		 * Read from the startup pipe, there should be no data
		 * and we should block until it is closed. We just
		 * carry-on on error since this isn't super critical.
		 */
		ret = read(3, &i, sizeof(i));
		if (ret < 0)
			printf("read(startp pipe) failed: %s (%d)\n",
			       strerror(errno), errno);
		if (ret > 0)
			printf("%d bytes of data on startup pipe\n", ret);
		close(3);

		ret = execv(prog_args[0], prog_args);
		printf("execv(%s) failed: %d (%s)\n",
		       prog_args[0], errno, strerror(errno));

		exit(EXIT_FAILURE);
	} else {
		/*
		 * In parent, remember the child and close our copy of the
		 * write side of stdout.
		 */
		close(pipefd[1]);
		child->stdout = pipefd[0];
		child->output = NULL;
		child->exited = false;
		child->output_seen = false;

		ev.events = EPOLLIN | EPOLLHUP;
		ev.data.ptr = child;

		ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, child->stdout, &ev);
		if (ret < 0) {
			ksft_exit_fail_msg("%s EPOLL_CTL_ADD failed: %s (%d)\n",
					   child->name, strerror(errno), errno);
		}
	}
}

static bool child_output_read(struct child_data *child)
{
	char read_data[1024];
	char work[1024];
	int ret, len, cur_work, cur_read;

	ret = read(child->stdout, read_data, sizeof(read_data));
	if (ret < 0) {
		if (errno == EINTR)
			return true;

		ksft_print_msg("%s: read() failed: %s (%d)\n",
			       child->name, strerror(errno),
			       errno);
		return false;
	}
	len = ret;

	child->output_seen = true;

	/* Pick up any partial read */
	if (child->output) {
		strncpy(work, child->output, sizeof(work) - 1);
		cur_work = strnlen(work, sizeof(work));
		free(child->output);
		child->output = NULL;
	} else {
		cur_work = 0;
	}

	cur_read = 0;
	while (cur_read < len) {
		work[cur_work] = read_data[cur_read++];

		if (work[cur_work] == '\n') {
			work[cur_work] = '\0';
			ksft_print_msg("%s: %s\n", child->name, work);
			cur_work = 0;
		} else {
			cur_work++;
		}
	}

	if (cur_work) {
		work[cur_work] = '\0';
		ret = asprintf(&child->output, "%s", work);
		if (ret == -1)
			ksft_exit_fail_msg("Out of memory\n");
	}

	return false;
}

static void child_output(struct child_data *child, uint32_t events,
			 bool flush)
{
	bool read_more;

	if (events & EPOLLIN) {
		do {
			read_more = child_output_read(child);
		} while (read_more);
	}

	if (events & EPOLLHUP) {
		close(child->stdout);
		child->stdout = -1;
		flush = true;
	}

	if (flush && child->output) {
		ksft_print_msg("%s: %s<EOF>\n", child->name, child->output);
		free(child->output);
		child->output = NULL;
	}
}

static void child_tickle(struct child_data *child)
{
	if (child->output_seen && !child->exited)
		kill(child->pid, SIGUSR1);
}

static void child_stop(struct child_data *child)
{
	if (!child->exited)
		kill(child->pid, SIGTERM);
}

static void child_cleanup(struct child_data *child)
{
	pid_t ret;
	int status;
	bool fail = false;

	if (!child->exited) {
		do {
			ret = waitpid(child->pid, &status, 0);
			if (ret == -1 && errno == EINTR)
				continue;

			if (ret == -1) {
				ksft_print_msg("waitpid(%d) failed: %s (%d)\n",
					       child->pid, strerror(errno),
					       errno);
				fail = true;
				break;
			}
		} while (!WIFEXITED(status));
		child->exit_status = WEXITSTATUS(status);
	}

	if (!child->output_seen) {
		ksft_print_msg("%s no output seen\n", child->name);
		fail = true;
	}

	if (child->exit_status != 0) {
		ksft_print_msg("%s exited with error code %d\n",
			       child->name, child->exit_status);
		fail = true;
	}

	ksft_test_result(!fail, "%s\n", child->name);
}

static void handle_child_signal(int sig, siginfo_t *info, void *context)
{
	int i;
	bool found = false;

	for (i = 0; i < num_children; i++) {
		if (children[i].pid == info->si_pid) {
			children[i].exited = true;
			children[i].exit_status = info->si_status;
			found = true;
			break;
		}
	}

	if (!found)
		ksft_print_msg("SIGCHLD for unknown PID %d with status %d\n",
			       info->si_pid, info->si_status);
}

static void handle_exit_signal(int sig, siginfo_t *info, void *context)
{
	int i;

	/* If we're already exiting then don't signal again */
	if (terminate)
		return;

	ksft_print_msg("Got signal, exiting...\n");

	terminate = true;

	/*
	 * This should be redundant, the main loop should clean up
	 * after us, but for safety stop everything we can here.
	 */
	for (i = 0; i < num_children; i++)
		child_stop(&children[i]);
}

static void start_fpsimd(struct child_data *child, int cpu, int copy)
{
	char *args[] = { "./fpsimd-test", NULL };
	int ret;

	ret = asprintf(&child->name, "FPSIMD-%d-%d", cpu, copy);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_fpsimd_kvm(struct child_data *child, int cpu, int copy)
{
	char *args[] = { "./fp-stress-vmm", kvm_el_arg(cpu),
			 "./fpsimd-test-kvm.bin", NULL };
	int ret;

	ret = asprintf(&child->name, "FPSIMD-KVM-%d-%d", cpu, copy);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_kernel(struct child_data *child, int cpu, int copy)
{
	char *args[] = { "./kernel-test", NULL };
	int ret;

	ret = asprintf(&child->name, "KERNEL-%d-%d", cpu, copy);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_sve(struct child_data *child, int vl, int cpu)
{
	char *args[] = { "./sve-test", NULL };
	int ret;

	ret = prctl(PR_SVE_SET_VL, vl | PR_SVE_VL_INHERIT);
	if (ret < 0)
		ksft_exit_fail_msg("Failed to set SVE VL %d\n", vl);

	ret = asprintf(&child->name, "SVE-VL-%d-%d", vl, cpu);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_sve_kvm(struct child_data *child, int vl, int cpu)
{
	char *args[] = { "./fp-stress-vmm", "--sve", NULL, kvm_el_arg(cpu),
			 "./sve-test-kvm.bin", NULL };
	char vl_str[32];
	int ret;

	/* Our VLs are in bytes, fp-stress-vmm wants bits */
	snprintf(vl_str, sizeof(vl_str), "%d", vl * 8);
	args[2] = vl_str;

	ret = asprintf(&child->name, "SVE-KVM-VL-%d-%d", vl, cpu);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_ssve(struct child_data *child, int vl, int cpu)
{
	char *args[] = { "./ssve-test", NULL };
	int ret;

	ret = asprintf(&child->name, "SSVE-VL-%d-%d", vl, cpu);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	ret = prctl(PR_SME_SET_VL, vl | PR_SME_VL_INHERIT);
	if (ret < 0)
		ksft_exit_fail_msg("Failed to set SME VL %d\n", ret);

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_za(struct child_data *child, int vl, int cpu)
{
	char *args[] = { "./za-test", NULL };
	int ret;

	ret = prctl(PR_SME_SET_VL, vl | PR_SVE_VL_INHERIT);
	if (ret < 0)
		ksft_exit_fail_msg("Failed to set SME VL %d\n", ret);

	ret = asprintf(&child->name, "ZA-VL-%d-%d", vl, cpu);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void start_zt(struct child_data *child, int cpu)
{
	char *args[] = { "./zt-test", NULL };
	int ret;

	ret = asprintf(&child->name, "ZT-%d", cpu);
	if (ret == -1)
		ksft_exit_fail_msg("asprintf() failed\n");

	child_start(child, args);

	ksft_print_msg("Started %s\n", child->name);
}

static void kvm_put_vcpu(int vm_fd, int vcpu_fd)
{
	if (vcpu_fd >= 0)
		close(vcpu_fd);
	if (vm_fd >= 0)
		close(vm_fd);
}

static bool kvm_get_vcpu(int kvm_fd, int feature, int *vm_fd, int *vcpu_fd)
{
	struct kvm_vcpu_init init;
	int ret;

	*vcpu_fd = -1;

	*vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (*vm_fd < 0) {
		ksft_print_msg("KVM_CREATE_VM failed: %s (%d)\n",
			       strerror(errno), errno);
		return false;
	}

	*vcpu_fd = ioctl(*vm_fd, KVM_CREATE_VCPU, 0);
	if (*vcpu_fd < 0) {
		ksft_print_msg("KVM_CREATE_VCPU failed: %s (%d)\n",
			       strerror(errno), errno);
		goto err;
	}

	ret = ioctl(*vm_fd, KVM_ARM_PREFERRED_TARGET, &init);
	if (ret) {
		ksft_print_msg("KVM_ARM_PREFERRED_TARGET failed: %s (%d)\n",
			       strerror(errno), errno);
		goto err;
	}

	init.features[0] |= 1 << feature;
	ret = ioctl(*vcpu_fd, KVM_ARM_VCPU_INIT, &init);
	if (ret) {
		ksft_print_msg("KVM_ARM_VCPU_INIT feature %d failed: %s (%d)\n",
			       feature, strerror(errno), errno);
		goto err;
	}

	return true;

err:
	kvm_put_vcpu(*vm_fd, *vcpu_fd);
	return false;
}

static void probe_kvm_sve(int kvm_fd)
{
	__u64 vqs[KVM_ARM64_SVE_VLS_WORDS];
	struct kvm_one_reg reg = {
		.id = KVM_REG_ARM64_SVE_VLS,
		.addr = (__u64)vqs,
	};
	int vm_fd, vcpu_fd, i;
	unsigned int vq;

	if (!sve_vl_count)
		return;

	if (!ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_SVE)) {
		ksft_print_msg("No KVM SVE support\n");
		return;
	}

	if (!kvm_get_vcpu(kvm_fd, KVM_ARM_VCPU_SVE, &vm_fd, &vcpu_fd))
		return;

	/*
	 * The vector length set is readable without finalizing the vCPU,
	 * we only want to look at it rather than run anything.
	 */
	if (ioctl(vcpu_fd, KVM_GET_ONE_REG, &reg)) {
		ksft_print_msg("Failed to read KVM SVE VLs: %s (%d)\n",
			       strerror(errno), errno);
		goto out;
	}

	have_kvm_sve = true;
	for (i = 0; i < sve_vl_count; i++) {
		vq = sve_vq_from_vl(sve_vls[i]);

		if (!(vqs[(vq - KVM_ARM64_SVE_VQ_MIN) / 64] &
		      (1ULL << ((vq - KVM_ARM64_SVE_VQ_MIN) % 64)))) {
			ksft_print_msg("KVM has no SVE VL %d\n", sve_vls[i]);
			have_kvm_sve = false;
		}
	}

out:
	kvm_put_vcpu(vm_fd, vcpu_fd);
}

/*
 * We don't act as a VMM for guests directly, this just enumerates what
 * can be run with KVM.
 */
static void probe_kvm(void)
{
	int fd, ret;

	fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ksft_print_msg("Not using KVM, /dev/kvm: %s (%d)\n",
			       strerror(errno), errno);
		return;
	}

	ret = ioctl(fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION) {
		ksft_print_msg("Not using KVM, API version %d not %d\n",
			       ret, KVM_API_VERSION);
		close(fd);
		return;
	}

	have_kvm = true;

	if (ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_EL2))
		have_kvm_el2 = true;

	probe_kvm_sve(fd);

	close(fd);
}

static void probe_vls(int vls[], int *vl_count, int set_vl)
{
	unsigned int vq;
	int vl;

	*vl_count = 0;

	for (vq = SVE_VQ_MAX; vq > 0; vq /= 2) {
		vl = prctl(set_vl, vq * 16);
		if (vl == -1)
			ksft_exit_fail_msg("SET_VL failed: %s (%d)\n",
					   strerror(errno), errno);

		vl &= PR_SVE_VL_LEN_MASK;

		if (*vl_count && (vl == vls[*vl_count - 1]))
			break;

		vq = sve_vq_from_vl(vl);

		vls[*vl_count] = vl;
		*vl_count += 1;
	}
}

/* Handle any pending output without blocking */
static void drain_output(bool flush)
{
	int ret = 1;
	int i;

	while (ret > 0) {
		ret = epoll_wait(epoll_fd, evs, tests, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ksft_print_msg("epoll_wait() failed: %s (%d)\n",
				       strerror(errno), errno);
		}

		for (i = 0; i < ret; i++)
			child_output(evs[i].data.ptr, evs[i].events, flush);
	}
}

static const struct option options[] = {
	{ "timeout",	required_argument, NULL, 't' },
	{ }
};

int main(int argc, char **argv)
{
	int ret;
	int timeout = 10 * (1000 / SIGNAL_INTERVAL_MS);
	int poll_interval = 5000;
	int cpus, i, j, c;
	bool all_children_started = false;
	int seen_children;
	bool have_sme2;
	struct sigaction sa;

	while ((c = getopt_long(argc, argv, "t:", options, NULL)) != -1) {
		switch (c) {
		case 't':
			ret = sscanf(optarg, "%d", &timeout);
			if (ret != 1)
				ksft_exit_fail_msg("Failed to parse timeout %s\n",
						   optarg);
			break;
		default:
			ksft_exit_fail_msg("Unknown argument\n");
		}
	}

	cpus = num_processors();
	tests = 0;

	if (getauxval(AT_HWCAP) & HWCAP_SVE) {
		probe_vls(sve_vls, &sve_vl_count, PR_SVE_SET_VL);
		tests += sve_vl_count * cpus;
	} else {
		sve_vl_count = 0;
	}

	if (getauxval(AT_HWCAP2) & HWCAP2_SME) {
		probe_vls(sme_vls, &sme_vl_count, PR_SME_SET_VL);
		tests += sme_vl_count * cpus * 2;
	} else {
		sme_vl_count = 0;
	}

	if (getauxval(AT_HWCAP2) & HWCAP2_SME2) {
		tests += cpus;
		have_sme2 = true;
	} else {
		have_sme2 = false;
	}

	tests += cpus * 2;

	ksft_print_header();
	ksft_set_plan(tests);

	probe_kvm();

	ksft_print_msg("%d CPUs, %d SVE VLs, %d SME VLs, SME2 %s\n",
		       cpus, sve_vl_count, sme_vl_count,
		       have_sme2 ? "present" : "absent");
	ksft_print_msg("KVM: %s, %s EL2, %s SVE\n",
		       have_kvm ? "present" : "absent",
		       have_kvm_el2 ? "with" : "without",
		       have_kvm_sve ? "with" : "without");

	if (timeout > 0)
		ksft_print_msg("Will run for %d\n", timeout);
	else
		ksft_print_msg("Will run until terminated\n");

	children = calloc(sizeof(*children), tests);
	if (!children)
		ksft_exit_fail_msg("Unable to allocate child data\n");

	ret = epoll_create1(EPOLL_CLOEXEC);
	if (ret < 0)
		ksft_exit_fail_msg("epoll_create1() failed: %s (%d)\n",
				   strerror(errno), ret);
	epoll_fd = ret;

	/* Create a pipe which children will block on before execing */
	ret = pipe(startup_pipe);
	if (ret != 0)
		ksft_exit_fail_msg("Failed to create startup pipe: %s (%d)\n",
				   strerror(errno), errno);

	/* Get signal handers ready before we start any children */
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handle_exit_signal;
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	ret = sigaction(SIGINT, &sa, NULL);
	if (ret < 0)
		ksft_print_msg("Failed to install SIGINT handler: %s (%d)\n",
			       strerror(errno), errno);
	ret = sigaction(SIGTERM, &sa, NULL);
	if (ret < 0)
		ksft_print_msg("Failed to install SIGTERM handler: %s (%d)\n",
			       strerror(errno), errno);
	sa.sa_sigaction = handle_child_signal;
	ret = sigaction(SIGCHLD, &sa, NULL);
	if (ret < 0)
		ksft_print_msg("Failed to install SIGCHLD handler: %s (%d)\n",
			       strerror(errno), errno);

	evs = calloc(tests, sizeof(*evs));
	if (!evs)
		ksft_exit_fail_msg("Failed to allocate %d epoll events\n",
				   tests);

	for (i = 0; i < cpus; i++) {
		if (have_kvm && (i % 2))
			start_fpsimd_kvm(&children[num_children++], i, 0);
		else
			start_fpsimd(&children[num_children++], i, 0);
		start_kernel(&children[num_children++], i, 0);

		for (j = 0; j < sve_vl_count; j++) {
			if (have_kvm_sve && (i % 2))
				start_sve_kvm(&children[num_children++],
					      sve_vls[j], i);
			else
				start_sve(&children[num_children++],
					  sve_vls[j], i);
		}

		for (j = 0; j < sme_vl_count; j++) {
			start_ssve(&children[num_children++], sme_vls[j], i);
			start_za(&children[num_children++], sme_vls[j], i);
		}

		if (have_sme2)
			start_zt(&children[num_children++], i);
	}

	/*
	 * All children started, close the startup pipe and let them
	 * run.
	 */
	close(startup_pipe[0]);
	close(startup_pipe[1]);

	for (;;) {
		/* Did we get a signal asking us to exit? */
		if (terminate)
			break;

		/*
		 * Timeout is counted in poll intervals with no
		 * output, the tests print during startup then are
		 * silent when running so this should ensure they all
		 * ran enough to install the signal handler, this is
		 * especially useful in emulation where we will both
		 * be slow and likely to have a large set of VLs.
		 */
		ret = epoll_wait(epoll_fd, evs, tests, poll_interval);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ksft_exit_fail_msg("epoll_wait() failed: %s (%d)\n",
					   strerror(errno), errno);
		}

		/* Output? */
		if (ret > 0) {
			for (i = 0; i < ret; i++) {
				child_output(evs[i].data.ptr, evs[i].events,
					     false);
			}
			continue;
		}

		/* Otherwise epoll_wait() timed out */

		/*
		 * If the child processes have not produced output they
		 * aren't actually running the tests yet .
		 */
		if (!all_children_started) {
			seen_children = 0;

			for (i = 0; i < num_children; i++)
				if (children[i].output_seen ||
				    children[i].exited)
					seen_children++;

			if (seen_children != num_children) {
				ksft_print_msg("Waiting for %d children\n",
					       num_children - seen_children);
				continue;
			}

			all_children_started = true;
			poll_interval = SIGNAL_INTERVAL_MS;
		}

		if ((timeout % LOG_INTERVALS) == 0)
			ksft_print_msg("Sending signals, timeout remaining: %d\n",
				       timeout);

		for (i = 0; i < num_children; i++)
			child_tickle(&children[i]);

		/* Negative timeout means run indefinitely */
		if (timeout < 0)
			continue;
		if (--timeout == 0)
			break;
	}

	ksft_print_msg("Finishing up...\n");
	terminate = true;

	for (i = 0; i < tests; i++)
		child_stop(&children[i]);

	drain_output(false);

	for (i = 0; i < tests; i++)
		child_cleanup(&children[i]);

	drain_output(true);

	ksft_finished();
}
