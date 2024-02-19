// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"

static void guest_code(uint64_t *mem, uint64_t magic_val)
{
	uint64_t val = READ_ONCE(*mem);

	GUEST_ASSERT_EQ(val, magic_val);

	WRITE_ONCE(*mem, ~magic_val);
	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	const uint64_t magic_val = 0xaa55aa55aa55aa55;
	const uint64_t gpa = 0xc0000000ull;
	const int slot = 1;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, gpa, slot, 1,
				    KVM_MEM_READONLY);

	virt_map(vm, gpa, gpa, 1);
	*(volatile uint64_t *)addr_gpa2hva(vm, gpa) = magic_val;

	vcpu_args_set(vcpu, 2, gpa, magic_val);

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(vcpu->run->exit_reason, KVM_EXIT_MMIO);
	TEST_ASSERT_EQ(vcpu->run->mmio.is_write, true);
	TEST_ASSERT_EQ(vcpu->run->mmio.len, 8);
	TEST_ASSERT_EQ(vcpu->run->mmio.phys_addr, gpa);
	TEST_ASSERT_EQ(*(uint64_t *)vcpu->run->mmio.data, ~magic_val);

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	kvm_vm_free(vm);
	return 0;
}
