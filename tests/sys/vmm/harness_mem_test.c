/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Tests of the harness's guest memory mappings and shared variables. */

#include <sys/param.h>

#include <machine/pte.h>

#include <atf-c.h>

#include "harness_util.h"
#include "vmm_test.h"
#include "vmm_test_guest.h"

#if defined(__amd64__)
#define	RET_INSN	0xc3			/* ret */
#elif defined(__aarch64__)
#define	RET_INSN	0xd65f03c0		/* ret */
#endif

static void
guest_done(void)
{

	GUEST_DONE();
}

static uint64_t shared_value;
static uint8_t shared_pages[2 * PAGE_SIZE] __aligned(PAGE_SIZE);

static uint8_t
shared_byte(size_t i)
{

	return (i * 7 + (i >> 8));
}

static void
guest_shared(void)
{
	size_t i;

	GUEST_ASSERT_EQ(shared_value, 0x1234);
	for (i = 0; i < sizeof(shared_pages); i++)
		GUEST_ASSERT_EQ(shared_pages[i], shared_byte(i));
	shared_value = 0x5678;
	GUEST_SYNC(1);
}

/*
 * The host reaches the guest's copy of a global, including one that spans
 * pages, and the two copies are independent.
 */
ATF_TC_WITHOUT_HEAD(guest_var);
ATF_TC_BODY(guest_var, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint8_t *pages;
	size_t i;

	vm = vmm_test_vm_create_with_vcpu(guest_shared, &vcpu);
	*VMM_TEST_GUEST_VAR(vm, shared_value) = 0x1234;
	pages = *VMM_TEST_GUEST_VAR(vm, shared_pages);
	for (i = 0; i < sizeof(shared_pages); i++)
		pages[i] = shared_byte(i);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	ATF_CHECK_EQ(*VMM_TEST_GUEST_VAR(vm, shared_value), 0x5678);
	ATF_CHECK_EQ(shared_value, 0);
	vmm_test_run_until_done(vcpu);
}

static void
host_noncontiguous(struct vmm_test_vcpu *vcpu)
{
	struct vmm_test_vm *vm;
	vm_paddr_t first, second;

	vm = vcpu->vm;
	first = vmm_test_alloc_pages(vm, 1);
	(void)vmm_test_alloc_pages(vm, 1);
	second = vmm_test_alloc_pages(vm, 1);
	vmm_test_map(vm, VMM_TEST_VA, first, PAGE_SIZE, VMM_TEST_PROT_READ);
	vmm_test_map(vm, VMM_TEST_VA + PAGE_SIZE, second, PAGE_SIZE,
	    VMM_TEST_PROT_READ);
	(void)vmm_test_gva2hva(vm, VMM_TEST_VA, 2 * PAGE_SIZE);
}

/* vmm_test_gva2hva() refuses a range that is not contiguous in memory. */
ATF_TC_WITHOUT_HEAD(gva2hva_noncontiguous);
ATF_TC_BODY(gva2hva_noncontiguous, tc)
{

	harness_expect_failure(guest_done, host_noncontiguous,
	    (const char *const []){
	    "guest range 0x10000000+0x2000 is not physically contiguous",
	    NULL });
}

static void
guest_map(void)
{
	volatile uint64_t *p;

	p = (volatile uint64_t *)VMM_TEST_VA;
	GUEST_ASSERT_EQ(p[0], 0x1122334455667788UL);
	p[1] = 0x99aabbccddeeff00UL;
	GUEST_SYNC(1);
}

/* A mapping made by the host is visible to the guest in both directions. */
ATF_TC_WITHOUT_HEAD(map);
ATF_TC_BODY(map, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *hva;
	vm_paddr_t gpa, translated;

	vm = vmm_test_vm_create_with_vcpu(guest_map, &vcpu);
	gpa = vmm_test_alloc_pages(vm, 1);
	hva = vmm_test_gpa2hva(vm, gpa, PAGE_SIZE);
	hva[0] = 0x1122334455667788UL;
	vmm_test_map(vm, VMM_TEST_VA, gpa, PAGE_SIZE,
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	ATF_REQUIRE(vmm_test_gva2gpa(vm, VMM_TEST_VA + 8, &translated));
	ATF_CHECK_EQ(translated, gpa + 8);
	ATF_CHECK(vmm_test_md_pte(vm, VMM_TEST_VA) != NULL);
	ATF_CHECK(vmm_test_gva2hva(vm, VMM_TEST_VA, PAGE_SIZE) == hva);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	ATF_CHECK_EQ(hva[1], 0x99aabbccddeeff00UL);
	vmm_test_run_until_done(vcpu);
}

static void
host_map_twice(struct vmm_test_vcpu *vcpu)
{
	vm_paddr_t gpa;

	gpa = vmm_test_alloc_pages(vcpu->vm, 1);
	vmm_test_map(vcpu->vm, VMM_TEST_VA, gpa, PAGE_SIZE, VMM_TEST_PROT_READ);
	vmm_test_map(vcpu->vm, VMM_TEST_VA, gpa, PAGE_SIZE, VMM_TEST_PROT_READ);
}

/*
 * Mapping a page that is already mapped fails the test case, through the
 * same path as the harness's other failures.
 */
ATF_TC_WITHOUT_HEAD(map_twice);
ATF_TC_BODY(map_twice, tc)
{

	harness_expect_failure(guest_done, host_map_twice,
	    (const char *const []){ "0x10000000 is already mapped", NULL });
}

static void
guest_map_device(void)
{
	volatile uint64_t *p;

	p = (volatile uint64_t *)VMM_TEST_VA;
	p[0] = 0x0123456789abcdefUL;
	GUEST_ASSERT_EQ(p[0], 0x0123456789abcdefUL);
}

/*
 * A device mapping has the uncached memory type, and guest code can use it.
 * The host neither writes nor reads the memory, which its own mapping caches.
 */
ATF_TC_WITHOUT_HEAD(map_device);
ATF_TC_BODY(map_device, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *pte;

	vm = vmm_test_vm_create_with_vcpu(guest_map_device, &vcpu);
	vmm_test_map(vm, VMM_TEST_VA, vmm_test_alloc_pages(vm, 1), PAGE_SIZE,
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE | VMM_TEST_MAP_DEVICE);
	pte = vmm_test_md_pte(vm, VMM_TEST_VA);
	ATF_REQUIRE(pte != NULL);
#if defined(__amd64__)
	ATF_CHECK_EQ(*pte & (X86_PG_NC_PCD | X86_PG_NC_PWT),
	    X86_PG_NC_PCD | X86_PG_NC_PWT);
#elif defined(__aarch64__)
	/* MAIR_EL1 index 1 is Device-nGnRnE. */
	ATF_CHECK_EQ(*pte & ATTR_S1_IDX_MASK, ATTR_S1_IDX(1));
#endif
	vmm_test_run_until_done(vcpu);
}

/* Write, stop, then read after the host changed the mapping. */
static void
guest_read_after_change(void)
{
	volatile uint64_t *p;

	p = (volatile uint64_t *)VMM_TEST_VA;
	*p = 1;
	GUEST_SYNC(1);
	vmm_guest_flush_tlb(VMM_TEST_VA);
	(void)*p;
	GUEST_FAIL("read from an inaccessible page succeeded");
}

static vm_paddr_t
map_test_page(struct vmm_test_vm *vm, int flags)
{
	vm_paddr_t gpa;

	gpa = vmm_test_alloc_pages(vm, 1);
	vmm_test_map(vm, VMM_TEST_VA, gpa, PAGE_SIZE, flags);
	return (gpa);
}

static void
host_unmap(struct vmm_test_vcpu *vcpu)
{
	vm_paddr_t gpa;

	(void)map_test_page(vcpu->vm, VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	vmm_test_unmap(vcpu->vm, VMM_TEST_VA, PAGE_SIZE);
	ATF_REQUIRE(!vmm_test_gva2gpa(vcpu->vm, VMM_TEST_VA, &gpa));
	vmm_test_run_until_done(vcpu);
}

static void
host_protect_none(struct vmm_test_vcpu *vcpu)
{
	vm_paddr_t gpa;

	(void)map_test_page(vcpu->vm, VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	vmm_test_protect(vcpu->vm, VMM_TEST_VA, PAGE_SIZE, 0);
	ATF_REQUIRE(!vmm_test_gva2gpa(vcpu->vm, VMM_TEST_VA, &gpa));
	vmm_test_run_until_done(vcpu);
}

#if defined(__amd64__)
#define	NOT_PRESENT_READ	"#PF(0) at rip 0x"
#elif defined(__aarch64__)
#define	NOT_PRESENT_READ	"read, translation fault, level 3) at pc 0x"
#endif

/* After the host unmaps a page and the guest flushes it, accesses fault. */
ATF_TC_WITHOUT_HEAD(unmap);
ATF_TC_BODY(unmap, tc)
{

	harness_expect_failure(guest_read_after_change, host_unmap,
	    (const char *const []){ "vCPU 0: unhandled guest exception ",
	    NOT_PRESENT_READ, ", address 0x10000000", NULL });
}

/* A page protected without read access is not present. */
ATF_TC_WITHOUT_HEAD(protect_none);
ATF_TC_BODY(protect_none, tc)
{

	harness_expect_failure(guest_read_after_change, host_protect_none,
	    (const char *const []){ "vCPU 0: unhandled guest exception ",
	    NOT_PRESENT_READ, ", address 0x10000000", NULL });
}

static void
guest_protect_restore(void)
{
	volatile uint64_t *p;

	p = (volatile uint64_t *)VMM_TEST_VA;
	*p = 1;
	GUEST_SYNC(1);
	vmm_guest_flush_tlb(VMM_TEST_VA);
	GUEST_ASSERT_EQ(*p, 1);
	*p = 2;
	GUEST_SYNC(2);
}

/* Restoring access to a page keeps its guest-physical address. */
ATF_TC_WITHOUT_HEAD(protect_restore);
ATF_TC_BODY(protect_restore, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	vm_paddr_t gpa, translated;

	vm = vmm_test_vm_create_with_vcpu(guest_protect_restore, &vcpu);
	gpa = map_test_page(vm, VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	vmm_test_protect(vm, VMM_TEST_VA, PAGE_SIZE, 0);
	ATF_CHECK(!vmm_test_gva2gpa(vm, VMM_TEST_VA, &translated));
	vmm_test_protect(vm, VMM_TEST_VA, PAGE_SIZE,
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	ATF_REQUIRE(vmm_test_gva2gpa(vm, VMM_TEST_VA, &translated));
	ATF_CHECK_EQ(translated, gpa);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	ATF_CHECK_EQ(*(uint64_t *)vmm_test_gpa2hva(vm, gpa, 8), 2);
	vmm_test_run_until_done(vcpu);
}

/*
 * Restoring access also works for a page at guest-physical address 0, which
 * the page table entry alone does not tell apart from no mapping on amd64.
 */
ATF_TC_WITHOUT_HEAD(protect_restore_zero);
ATF_TC_BODY(protect_restore_zero, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	vm_paddr_t translated;

	vm = vmm_test_vm_create_with_vcpu(guest_done, &vcpu);
	vmm_test_map(vm, VMM_TEST_VA, 0, PAGE_SIZE, VMM_TEST_PROT_READ);
	vmm_test_protect(vm, VMM_TEST_VA, PAGE_SIZE, 0);
	ATF_CHECK(!vmm_test_gva2gpa(vm, VMM_TEST_VA, &translated));
	vmm_test_protect(vm, VMM_TEST_VA, PAGE_SIZE, VMM_TEST_PROT_READ);
	ATF_REQUIRE(vmm_test_gva2gpa(vm, VMM_TEST_VA, &translated));
	ATF_CHECK_EQ(translated, 0);
	vmm_test_run_until_done(vcpu);
}

/*
 * A device page is made inaccessible and restored with flags that keep
 * VMM_TEST_MAP_DEVICE.
 */
ATF_TC_WITHOUT_HEAD(protect_restore_device);
ATF_TC_BODY(protect_restore_device, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	vm_paddr_t gpa, translated;

	vm = vmm_test_vm_create_with_vcpu(guest_done, &vcpu);
	gpa = vmm_test_alloc_pages(vm, 1);
	vmm_test_map(vm, VMM_TEST_VA, gpa, PAGE_SIZE,
	    VMM_TEST_PROT_READ | VMM_TEST_MAP_DEVICE);
	vmm_test_protect(vm, VMM_TEST_VA, PAGE_SIZE, VMM_TEST_MAP_DEVICE);
	ATF_CHECK(!vmm_test_gva2gpa(vm, VMM_TEST_VA, &translated));
	vmm_test_protect(vm, VMM_TEST_VA, PAGE_SIZE,
	    VMM_TEST_PROT_READ | VMM_TEST_MAP_DEVICE);
	ATF_REQUIRE(vmm_test_gva2gpa(vm, VMM_TEST_VA, &translated));
	ATF_CHECK_EQ(translated, gpa);
	vmm_test_run_until_done(vcpu);
}

static void
host_protect_device(struct vmm_test_vcpu *vcpu)
{

	(void)map_test_page(vcpu->vm, VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_protect(vcpu->vm, VMM_TEST_VA, PAGE_SIZE,
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE | VMM_TEST_MAP_DEVICE);
}

/* vmm_test_protect() refuses to change a page's memory type in place. */
ATF_TC_WITHOUT_HEAD(protect_memory_type);
ATF_TC_BODY(protect_memory_type, tc)
{

	harness_expect_failure(guest_done, host_protect_device,
	    (const char *const []){
	    "cannot change the memory type of 0x10000000", NULL });
}

static void
guest_write_after_protect(void)
{
	volatile uint64_t *p;

	p = (volatile uint64_t *)VMM_TEST_VA;
	*p = 1;
	GUEST_SYNC(1);
	vmm_guest_flush_tlb(VMM_TEST_VA);
	GUEST_ASSERT_EQ(*p, 1);
	*p = 2;
	GUEST_FAIL("write to a read-only page succeeded");
}

static void
host_protect_read(struct vmm_test_vcpu *vcpu)
{

	(void)map_test_page(vcpu->vm, VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	vmm_test_protect(vcpu->vm, VMM_TEST_VA, PAGE_SIZE, VMM_TEST_PROT_READ);
	vmm_test_run_until_done(vcpu);
}

/* A page protected read-only stays readable, and writes fault. */
ATF_TC_WITHOUT_HEAD(protect_write);
ATF_TC_BODY(protect_write, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_write_after_protect, host_protect_read,
	    (const char *const []){ "vCPU 0: unhandled guest exception ",
	    "#PF(0x3) at rip 0x", ", address 0x10000000", NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_write_after_protect, host_protect_read,
	    (const char *const []){ "vCPU 0: unhandled guest exception ",
	    "data abort (EC 0x25, ESR 0x",
	    ": write, permission fault, level 3) at pc 0x",
	    ", address 0x10000000", NULL });
#endif
}

static void
guest_call_test_va(void)
{

	((void (*)(void))VMM_TEST_VA)();
}

static void
host_exec(struct vmm_test_vcpu *vcpu, int flags)
{
	uint32_t *hva;
	vm_paddr_t gpa;

	gpa = map_test_page(vcpu->vm, flags);
	hva = vmm_test_gpa2hva(vcpu->vm, gpa, PAGE_SIZE);
	*hva = RET_INSN;
	__builtin___clear_cache((char *)hva, (char *)(hva + 1));
	vmm_test_run_until_done(vcpu);
}

static void
host_exec_denied(struct vmm_test_vcpu *vcpu)
{

	host_exec(vcpu, VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
}

/* Instructions on a page mapped without VMM_TEST_PROT_EXEC fault. */
ATF_TC_WITHOUT_HEAD(protect_exec);
ATF_TC_BODY(protect_exec, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_call_test_va, host_exec_denied,
	    (const char *const []){ "vCPU 0: unhandled guest exception ",
	    "#PF(0x11) at rip 0x10000000, address 0x10000000", NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_call_test_va, host_exec_denied,
	    (const char *const []){ "vCPU 0: unhandled guest exception ",
	    "instruction abort (EC 0x21, ESR 0x",
	    ": permission fault, level 3) at pc 0x10000000",
	    ", address 0x10000000", NULL });
#endif
}

/* Instructions on an executable page run. */
ATF_TC_WITHOUT_HEAD(exec);
ATF_TC_BODY(exec, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_call_test_va, &vcpu);
	host_exec(vcpu, VMM_TEST_PROT_READ | VMM_TEST_PROT_EXEC);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, guest_var);
	ATF_TP_ADD_TC(tp, gva2hva_noncontiguous);
	ATF_TP_ADD_TC(tp, map);
	ATF_TP_ADD_TC(tp, map_twice);
	ATF_TP_ADD_TC(tp, map_device);
	ATF_TP_ADD_TC(tp, unmap);
	ATF_TP_ADD_TC(tp, protect_none);
	ATF_TP_ADD_TC(tp, protect_restore);
	ATF_TP_ADD_TC(tp, protect_restore_zero);
	ATF_TP_ADD_TC(tp, protect_restore_device);
	ATF_TP_ADD_TC(tp, protect_memory_type);
	ATF_TP_ADD_TC(tp, protect_write);
	ATF_TP_ADD_TC(tp, protect_exec);
	ATF_TP_ADD_TC(tp, exec);
	return (atf_no_error());
}
