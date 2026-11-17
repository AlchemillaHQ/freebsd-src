/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@freebsd.org>
 */

/*
 * Tests of the test harness itself: mostly that it fails a test case when the
 * guest or the host does something other than expected, so that a broken
 * test cannot pass.
 */

#include <sys/param.h>

#include <atf-c.h>

#include "harness_util.h"
#include "vmm_test.h"
#include "vmm_test_guest.h"

/* Never mapped: the test binary is loaded far above it. */
#define	UNMAPPED_VA	0x1000UL

#if defined(__amd64__)
#define	UNDEF_INSN	"ud2"
#define	UNDEF_VECTOR	IDT_UD
#define	FAULT_VECTOR	IDT_PF
#elif defined(__aarch64__)
#define	UNDEF_INSN	"udf #0"
#define	UNDEF_VECTOR	EXCP_UNKNOWN
#define	FAULT_VECTOR	EXCP_DATA_ABORT
#endif

static uint64_t guest_value;

static void
guest_nothing(void)
{
}

static void
guest_assert_eq(void)
{

	GUEST_ASSERT_EQ(guest_value, 1);
}

/* A failed GUEST_ASSERT_EQ() fails the test case with its location. */
ATF_TC_WITHOUT_HEAD(guest_assert_eq);
ATF_TC_BODY(guest_assert_eq, tc)
{

	harness_expect_failure(guest_assert_eq, NULL, (const char *const []){
	    "vCPU 0: guest assertion failed at harness_test.c:",
	    ": guest_value == 1 (0 != 0x1)", NULL });
}

/* An address outside the test binary, as a guest stack address would be. */
#define	BAD_POINTER	0xffff800000123000UL

static void
guest_bad_message(void)
{

	GUEST_FAIL((const char *)BAD_POINTER);
}

/*
 * The host does not follow a message pointer outside the test binary, where
 * it has no copy of the string, but still reports the failure.
 */
ATF_TC_WITHOUT_HEAD(guest_bad_message);
ATF_TC_BODY(guest_bad_message, tc)
{

	harness_expect_failure(guest_bad_message, NULL, (const char *const []){
	    "vCPU 0: guest assertion failed at harness_test.c:",
	    ": <bad pointer 0xffff800000123000>", NULL });
}

static void
guest_unexpected_exit(void)
{

#if defined(__amd64__)
	/* Neither vmm(4) nor the harness handles this I/O port. */
	outb(0x80, 0);
#elif defined(__aarch64__)
	/* The harness enables breakpoint exits. */
	__builtin_trap();
#endif
}

/* An unexpected exit fails the test case with a description of the exit. */
ATF_TC_WITHOUT_HEAD(unexpected_exit);
ATF_TC_BODY(unexpected_exit, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_unexpected_exit, NULL,
	    (const char *const []){
	    "vCPU 0: expected DONE ucall, got INOUT exit",
	    ": out port 0x80, 1 bytes", NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_unexpected_exit, NULL,
	    (const char *const []){
	    "vCPU 0: expected DONE ucall, got BRK exit", NULL });
#endif
}

static void
guest_sync_once(void)
{

	GUEST_SYNC(1);
}

/* An unexpected ucall fails the test case with the ucall's location. */
ATF_TC_WITHOUT_HEAD(unexpected_ucall);
ATF_TC_BODY(unexpected_ucall, tc)
{

	harness_expect_failure(guest_sync_once, NULL, (const char *const []){
	    "vCPU 0: expected DONE ucall, got SYNC ucall from harness_test.c:",
	    NULL });
}

static void
host_expect_other_exit(struct vmm_test_vcpu *vcpu)
{

#if defined(__amd64__)
	(void)vmm_test_run_expect_exit(vcpu, VM_EXITCODE_INOUT);
#elif defined(__aarch64__)
	(void)vmm_test_run_expect_exit(vcpu, VM_EXITCODE_SMCCC);
#endif
}

/* A ucall where an exit was expected fails the test case. */
ATF_TC_WITHOUT_HEAD(expect_exit_ucall);
ATF_TC_BODY(expect_exit_ucall, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_sync_once, host_expect_other_exit,
	    (const char *const []){
	    "vCPU 0: expected INOUT exit, got SYNC ucall from harness_test.c:",
	    NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_sync_once, host_expect_other_exit,
	    (const char *const []){
	    "vCPU 0: expected SMCCC exit, got SYNC ucall from harness_test.c:",
	    NULL });
#endif
}

static void
guest_page_fault(void)
{

	*(volatile uint64_t *)UNMAPPED_VA = 1;
}

/* An unhandled exception fails the test case with its details. */
ATF_TC_WITHOUT_HEAD(exception_unhandled);
ATF_TC_BODY(exception_unhandled, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_page_fault, NULL, (const char *const []){
	    "vCPU 0: unhandled guest exception #PF(0x2) at rip 0x",
	    ", address 0x1000", NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_page_fault, NULL, (const char *const []){
	    "vCPU 0: unhandled guest exception data abort (EC 0x25, ESR 0x",
	    ": write, translation fault, level ", ", address 0x1000", NULL });
#endif
}

static void
guest_undefined(void)
{

	__asm __volatile(UNDEF_INSN);
}

/* An undefined instruction is reported without an address. */
ATF_TC_WITHOUT_HEAD(exception_undefined);
ATF_TC_BODY(exception_undefined, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_undefined, NULL, (const char *const []){
	    "vCPU 0: unhandled guest exception #UD at rip 0x", NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_undefined, NULL, (const char *const []){
	    "vCPU 0: unhandled guest exception undefined instruction "
	    "(EC 0, ESR 0x2000000) at pc 0x", NULL });
#endif
}

static void
guest_capture_undef(void)
{

	vmm_guest_capture_exception(UNDEF_VECTOR);
	__asm __volatile(UNDEF_INSN);
}

static void
guest_capture_nothing(void)
{

	vmm_guest_capture_exception(UNDEF_VECTOR);
}

static void
host_expect_fault(struct vmm_test_vcpu *vcpu)
{

	(void)vmm_test_run_expect_exception(vcpu, FAULT_VECTOR);
}

static void
host_expect_undef(struct vmm_test_vcpu *vcpu)
{

	(void)vmm_test_run_expect_exception(vcpu, UNDEF_VECTOR);
}

/* Capturing another exception, or none, fails the test case. */
ATF_TC_WITHOUT_HEAD(capture_unexpected);
ATF_TC_BODY(capture_unexpected, tc)
{

#if defined(__amd64__)
	harness_expect_failure(guest_capture_undef, host_expect_fault,
	    (const char *const []){
	    "vCPU 0: expected captured #PF, captured #UD", NULL });
	harness_expect_failure(guest_capture_nothing, host_expect_undef,
	    (const char *const []){
	    "vCPU 0: expected captured #UD, got DONE ucall from ", NULL });
#elif defined(__aarch64__)
	harness_expect_failure(guest_capture_undef, host_expect_fault,
	    (const char *const []){ "vCPU 0: expected captured data abort, "
	    "captured undefined instruction", NULL });
	harness_expect_failure(guest_capture_nothing, host_expect_undef,
	    (const char *const []){ "vCPU 0: expected captured undefined "
	    "instruction, got DONE ucall from ", NULL });
#endif
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

	harness_expect_failure(guest_nothing, host_map_twice,
	    (const char *const []){ "0x10000000 is already mapped", NULL });
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, guest_assert_eq);
	ATF_TP_ADD_TC(tp, guest_bad_message);
	ATF_TP_ADD_TC(tp, unexpected_exit);
	ATF_TP_ADD_TC(tp, unexpected_ucall);
	ATF_TP_ADD_TC(tp, expect_exit_ucall);
	ATF_TP_ADD_TC(tp, exception_unhandled);
	ATF_TP_ADD_TC(tp, exception_undefined);
	ATF_TP_ADD_TC(tp, capture_unexpected);
	ATF_TP_ADD_TC(tp, guest_var);
	ATF_TP_ADD_TC(tp, map_twice);
	return (atf_no_error());
}
