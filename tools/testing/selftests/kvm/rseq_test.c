// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE /* for program_invocation_short_name */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <syscall.h>
#include <sys/ioctl.h>
#include <linux/rseq.h>
#include <linux/unistd.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define VCPU_ID 0

static __thread volatile struct rseq __rseq = {
	.cpu_id = RSEQ_CPU_ID_UNINITIALIZED,
};

#define RSEQ_SIG 0xdeadbeef

static pthread_t migration_thread;
static cpu_set_t possible_mask;
static bool done;

static void guest_code(void)
{
	for (;;)
		GUEST_SYNC(0);
}

static void sys_rseq(int flags)
{
	int r;

	r = syscall(__NR_rseq, &__rseq, sizeof(__rseq), flags, RSEQ_SIG);
	TEST_ASSERT(!r, "rseq failed, errno = %d (%s)", errno, strerror(errno));
}

static void * migration_worker(void *ign)
{
	cpu_set_t allowed_mask;
	int r, i, nr_cpus, cpu;

	CPU_ZERO(&allowed_mask);

	nr_cpus = CPU_COUNT(&possible_mask);

	for (i = 0; i < 20000; i++) {
		cpu = i % nr_cpus;
		if (!CPU_ISSET(cpu, &possible_mask))
			continue;

		CPU_SET(cpu, &allowed_mask);

		r = sched_setaffinity(0, sizeof(allowed_mask), &allowed_mask);
		TEST_ASSERT(!r, "sched_setaffinity failed, errno = %d (%s)", errno,
			    strerror(errno));

		CPU_CLR(cpu, &allowed_mask);

		usleep(10);
	}
	done = true;
	return NULL;
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	u32 cpu, rseq_cpu;
	int r;

	/* Tell stdout not to buffer its content */
	setbuf(stdout, NULL);

	r = sched_getaffinity(0, sizeof(possible_mask), &possible_mask);
	TEST_ASSERT(!r, "sched_getaffinity failed, errno = %d (%s)", errno,
		    strerror(errno));

	if (CPU_COUNT(&possible_mask) < 2) {
		print_skip("Only one CPU, task migration not possible\n");
		exit(KSFT_SKIP);
	}

	sys_rseq(0);

	/*
	 * Create and run a dummy VM that immediately exits to userspace via
	 * GUEST_SYNC, while concurrently migrating the process by setting its
	 * CPU affinity.
	 */
	vm = vm_create_default(VCPU_ID, 0, guest_code);

	pthread_create(&migration_thread, NULL, migration_worker, 0);

	while (!done) {
		vcpu_run(vm, VCPU_ID);
		TEST_ASSERT(get_ucall(vm, VCPU_ID, NULL) == UCALL_SYNC,
			    "Guest failed?");

		cpu = sched_getcpu();
		rseq_cpu = READ_ONCE(__rseq.cpu_id);

		/*
		 * Verify rseq's CPU matches sched's CPU, and that sched's CPU
		 * is stable.  This doesn't handle the case where the task is
		 * migrated between sched_getcpu() and reading rseq, and again
		 * between reading rseq and sched_getcpu(), but in practice no
		 * false positives have been observed, while on the other hand
		 * blocking migration while this thread reads CPUs messes with
		 * the timing and prevents hitting failures on a buggy kernel.
		 */
		TEST_ASSERT(rseq_cpu == cpu || cpu != sched_getcpu(),
			    "rseq CPU = %d, sched CPU = %d\n", rseq_cpu, cpu);
	}

	pthread_join(migration_thread, NULL);

	kvm_vm_free(vm);

	sys_rseq(RSEQ_FLAG_UNREGISTER);

	return 0;
}
