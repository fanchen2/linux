// SPDX-License-Identifier: GPL-2.0
/*
 * Test the consistency of the PMU's CPUID and its features
 *
 * Copyright (C) 2023, Tencent, Inc.
 *
 * Check that the VM's PMU behaviour is consistent with the
 * VM CPUID definition.
 */

#define _GNU_SOURCE /* for program_invocation_short_name */
#include <x86intrin.h>

#include "pmu.h"
#include "processor.h"

/* Guest payload for any performance counter counting */
#define NUM_BRANCHES		10

static struct kvm_vm *pmu_vm_create_with_one_vcpu(struct kvm_vcpu **vcpu,
						  void *guest_code)
{
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(vcpu, guest_code);
	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(*vcpu);

	return vm;
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	do {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_DONE:
			break;
		default:
			TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
		}
	} while (uc.cmd != UCALL_DONE);
}

static bool pmu_is_intel_event_stable(uint8_t idx)
{
	switch (idx) {
	case INTEL_ARCH_CPU_CYCLES:
	case INTEL_ARCH_INSTRUCTIONS_RETIRED:
	case INTEL_ARCH_REFERENCE_CYCLES:
	case INTEL_ARCH_BRANCHES_RETIRED:
		return true;
	default:
		return false;
	}
}

static void guest_measure_pmu_v1(struct kvm_x86_pmu_feature event,
				 uint32_t counter_msr, uint32_t nr_gp_counters)
{
	uint8_t idx = event.f.bit;
	unsigned int i;

	for (i = 0; i < nr_gp_counters; i++) {
		wrmsr(counter_msr + i, 0);
		wrmsr(MSR_P6_EVNTSEL0 + i, ARCH_PERFMON_EVENTSEL_OS |
		      ARCH_PERFMON_EVENTSEL_ENABLE | intel_pmu_arch_events[idx]);
		__asm__ __volatile__("loop ." : "+c"((int){NUM_BRANCHES}));

		if (pmu_is_intel_event_stable(idx))
			GUEST_ASSERT_EQ(this_pmu_has(event), !!_rdpmc(i));

		wrmsr(MSR_P6_EVNTSEL0 + i, ARCH_PERFMON_EVENTSEL_OS |
		      !ARCH_PERFMON_EVENTSEL_ENABLE |
		      intel_pmu_arch_events[idx]);
		wrmsr(counter_msr + i, 0);
		__asm__ __volatile__("loop ." : "+c"((int){NUM_BRANCHES}));

		if (pmu_is_intel_event_stable(idx))
			GUEST_ASSERT(!_rdpmc(i));
	}

	GUEST_DONE();
}

static void guest_measure_loop(uint8_t idx)
{
	const struct {
		struct kvm_x86_pmu_feature gp_event;
	} intel_event_to_feature[] = {
		[INTEL_ARCH_CPU_CYCLES]		   = { X86_PMU_FEATURE_CPU_CYCLES },
		[INTEL_ARCH_INSTRUCTIONS_RETIRED]  = { X86_PMU_FEATURE_INSNS_RETIRED },
		[INTEL_ARCH_REFERENCE_CYCLES]	   = { X86_PMU_FEATURE_REFERENCE_CYCLES },
		[INTEL_ARCH_LLC_REFERENCES]	   = { X86_PMU_FEATURE_LLC_REFERENCES },
		[INTEL_ARCH_LLC_MISSES]		   = { X86_PMU_FEATURE_LLC_MISSES },
		[INTEL_ARCH_BRANCHES_RETIRED]	   = { X86_PMU_FEATURE_BRANCH_INSNS_RETIRED },
		[INTEL_ARCH_BRANCHES_MISPREDICTED] = { X86_PMU_FEATURE_BRANCHES_MISPREDICTED },
	};

	uint32_t nr_gp_counters = this_cpu_property(X86_PROPERTY_PMU_NR_GP_COUNTERS);
	uint32_t pmu_version = this_cpu_property(X86_PROPERTY_PMU_VERSION);
	struct kvm_x86_pmu_feature gp_event;
	uint32_t counter_msr;
	unsigned int i;

	if (rdmsr(MSR_IA32_PERF_CAPABILITIES) & PMU_CAP_FW_WRITES)
		counter_msr = MSR_IA32_PMC0;
	else
		counter_msr = MSR_IA32_PERFCTR0;

	gp_event = intel_event_to_feature[idx].gp_event;
	TEST_ASSERT_EQ(idx, gp_event.f.bit);

	if (pmu_version < 2) {
		guest_measure_pmu_v1(gp_event, counter_msr, nr_gp_counters);
		return;
	}

	for (i = 0; i < nr_gp_counters; i++) {
		wrmsr(counter_msr + i, 0);
		wrmsr(MSR_P6_EVNTSEL0 + i, ARCH_PERFMON_EVENTSEL_OS |
		      ARCH_PERFMON_EVENTSEL_ENABLE |
		      intel_pmu_arch_events[idx]);

		wrmsr(MSR_CORE_PERF_GLOBAL_CTRL, BIT_ULL(i));
		__asm__ __volatile__("loop ." : "+c"((int){NUM_BRANCHES}));
		wrmsr(MSR_CORE_PERF_GLOBAL_CTRL, 0);

		if (pmu_is_intel_event_stable(idx))
			GUEST_ASSERT_EQ(this_pmu_has(gp_event), !!_rdpmc(i));
	}

	GUEST_DONE();
}

static void test_arch_events_cpuid(uint8_t i, uint8_t j, uint8_t idx)
{
	uint8_t arch_events_unavailable_mask = BIT_ULL(j);
	uint8_t arch_events_bitmap_size = BIT_ULL(i);
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	vm = pmu_vm_create_with_one_vcpu(&vcpu, guest_measure_loop);

	vcpu_set_cpuid_property(vcpu, X86_PROPERTY_PMU_EBX_BIT_VECTOR_LENGTH,
				arch_events_bitmap_size);
	vcpu_set_cpuid_property(vcpu, X86_PROPERTY_PMU_EVENTS_MASK,
				arch_events_unavailable_mask);

	vcpu_args_set(vcpu, 1, idx);

	run_vcpu(vcpu);

	kvm_vm_free(vm);
}

static void test_intel_arch_events(void)
{
	uint8_t idx, i, j;

	for (idx = 0; idx < NR_INTEL_ARCH_EVENTS; idx++) {
		/*
		 * A brute force iteration of all combinations of values is
		 * likely to exhaust the limit of the single-threaded thread
		 * fd nums, so it's test by iterating through all valid
		 * single-bit values.
		 */
		for (i = 0; i < NR_INTEL_ARCH_EVENTS; i++) {
			for (j = 0; j < NR_INTEL_ARCH_EVENTS; j++)
				test_arch_events_cpuid(i, j, idx);
		}
	}
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(get_kvm_param_bool("enable_pmu"));

	TEST_REQUIRE(host_cpu_is_intel);
	TEST_REQUIRE(kvm_cpu_has_p(X86_PROPERTY_PMU_VERSION));
	TEST_REQUIRE(kvm_cpu_property(X86_PROPERTY_PMU_VERSION) > 0);
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_PDCM));

	test_intel_arch_events();

	return 0;
}
