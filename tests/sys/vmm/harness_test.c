/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Tests of the test harness itself. */

#include <sys/param.h>

#include <errno.h>
#include <string.h>
#include <vmmapi.h>

#include <atf-c.h>

#include "harness_util.h"
#include "vmm_test.h"
#include "vmm_test_guest.h"

#define	REDIRECT_ARG	0x0123456789abcdefUL

#if defined(__aarch64__)
/* A vendor-specific hypervisor call that is not a ucall. */
#define	OTHER_SMCCC_ID	(VMM_TEST_UCALL_ID + 1)
#endif

static uint64_t guest_value;

static void
guest_done(void)
{

	GUEST_DONE();
}

/* The guest reports completion with GUEST_DONE(). */
ATF_TC_WITHOUT_HEAD(guest_done);
ATF_TC_BODY(guest_done, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_done, &vcpu);
	vmm_test_run_until_done(vcpu);
}

static void
guest_return(void)
{
}

/* Returning from the guest function also reports completion. */
ATF_TC_WITHOUT_HEAD(guest_return);
ATF_TC_BODY(guest_return, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_return, &vcpu);
	vmm_test_run_until_done(vcpu);
}

static void
guest_sync(void)
{
	uint64_t i;

	for (i = 1; i <= 3; i++)
		GUEST_SYNC(i);
}

/* The guest resumes after each GUEST_SYNC(). */
ATF_TC_WITHOUT_HEAD(guest_sync);
ATF_TC_BODY(guest_sync, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_ucall uc;
	uint64_t i;

	(void)vmm_test_vm_create_with_vcpu(guest_sync, &vcpu);
	for (i = 1; i <= 3; i++) {
		vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, &uc);
		ATF_CHECK_EQ(i, uc.args[0]);
	}
	vmm_test_run_until_done(vcpu);
}

static void
guest_assert(void)
{

	GUEST_ASSERT(guest_value == 1);
}

/* A failed GUEST_ASSERT() fails the test case with its location. */
ATF_TC_WITHOUT_HEAD(guest_assert);
ATF_TC_BODY(guest_assert, tc)
{

	harness_expect_failure(guest_assert, NULL, (const char *const []){
	    "vCPU 0: guest assertion failed at harness_test.c:",
	    ": guest_value == 1", NULL });
}

static void
guest_assert_eq(void)
{

	GUEST_ASSERT_EQ(guest_value, 1);
}

/* A failed GUEST_ASSERT_EQ() also reports both values. */
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
guest_other_exit(void)
{

#if defined(__amd64__)
	outb(0x80, 0x5a);
#elif defined(__aarch64__)
	register uint64_t x0 __asm("x0") = OTHER_SMCCC_ID;
	register uint64_t x1 __asm("x1") = 0x5a;

	__asm __volatile("hvc #0" : "+r" (x0), "+r" (x1) : : "memory");
#endif
}

/*
 * vmm_test_run_expect_exit() returns the expected exit, and the guest
 * resumes after the instruction that caused it.
 */
ATF_TC_WITHOUT_HEAD(expect_exit);
ATF_TC_BODY(expect_exit, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vm_exit *vme;

	(void)vmm_test_vm_create_with_vcpu(guest_other_exit, &vcpu);
#if defined(__amd64__)
	vme = vmm_test_run_expect_exit(vcpu, VM_EXITCODE_INOUT);
	ATF_CHECK_EQ(vme->u.inout.port, 0x80);
	ATF_CHECK_EQ(vme->u.inout.bytes, 1);
	ATF_CHECK_EQ(vme->u.inout.in, 0);
	ATF_CHECK_EQ(vme->u.inout.eax & 0xff, 0x5a);
#elif defined(__aarch64__)
	vme = vmm_test_run_expect_exit(vcpu, VM_EXITCODE_SMCCC);
	ATF_CHECK_EQ(vme->u.smccc_call.func_id, OTHER_SMCCC_ID);
	ATF_CHECK_EQ(vme->u.smccc_call.args[0], 0x5a);
#endif
	vmm_test_run_until_done(vcpu);
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
guest_redirected(uint64_t arg)
{

	GUEST_ASSERT_EQ(arg, REDIRECT_ARG);
	GUEST_DONE();
}

static void
guest_wait_redirect(void)
{

	GUEST_SYNC(1);
	GUEST_FAIL("the host did not redirect the vCPU");
}

/* Registers set by the host take effect when the vCPU resumes. */
ATF_TC_WITHOUT_HEAD(set_reg);
ATF_TC_BODY(set_reg, tc)
{
	struct vmm_test_vcpu *vcpu;
	uint64_t sp;

	(void)vmm_test_vm_create_with_vcpu(guest_wait_redirect, &vcpu);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);

	/* Call guest_redirected() on a correctly aligned stack. */
	sp = vmm_test_get_reg(vcpu, VMM_TEST_REG_SP) & ~0xfUL;
#if defined(__amd64__)
	sp -= sizeof(uint64_t);		/* as if a return address was pushed */
#endif
	vmm_test_set_reg(vcpu, VMM_TEST_REG_SP, sp);
	vmm_test_set_reg(vcpu, VMM_TEST_REG_ARG0, REDIRECT_ARG);
	vmm_test_set_reg(vcpu, VMM_TEST_REG_PC, (uintptr_t)guest_redirected);
	ATF_CHECK_EQ(vmm_test_get_reg(vcpu, VMM_TEST_REG_SP), sp);
	ATF_CHECK_EQ(vmm_test_get_reg(vcpu, VMM_TEST_REG_ARG0), REDIRECT_ARG);
	ATF_CHECK_EQ(vmm_test_get_reg(vcpu, VMM_TEST_REG_PC),
	    (uintptr_t)guest_redirected);
	vmm_test_run_until_done(vcpu);
}

/*
 * vmm_test_vcpu_try_run() returns VM_RUN's errno, here for a vCPU that was
 * never activated, and otherwise runs the vCPU like vmm_test_vcpu_run().
 */
ATF_TC_WITHOUT_HEAD(try_run);
ATF_TC_BODY(try_run, tc)
{
	struct vmm_test_vcpu *vcpu, inactive;
	struct vmm_test_vm *vm;

	vm = vmm_test_vm_create_with_vcpu(guest_sync_once, &vcpu);
	ATF_REQUIRE_EQ(vmm_test_vcpu_try_run(vcpu), 0);
	ATF_CHECK_EQ(vcpu->ucall.cmd, UCALL_SYNC);

	memset(&inactive, 0, sizeof(inactive));
	inactive.vm = vm;
	inactive.id = 1;
	inactive.vcpu = vm_vcpu_open(vm->ctx, 1);
	ATF_REQUIRE(inactive.vcpu != NULL);
	ATF_CHECK_EQ(vmm_test_vcpu_try_run(&inactive), EINVAL);

	vmm_test_run_until_done(vcpu);
}

static void
guest_page_fault(void)
{

	*(volatile uint64_t *)VMM_TEST_UNMAPPED_VA = 1;
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

#if defined(__amd64__)
	__asm __volatile("ud2");
#elif defined(__aarch64__)
	__asm __volatile("udf #0");
#endif
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

#if defined(__amd64__)
static void
guest_triple_fault(void)
{
	struct region_descriptor rd = { .rd_limit = 0, .rd_base = 0 };

	lidt(&rd);
	__builtin_trap();
}

/* With no usable IDT, an exception is a triple fault and suspends the VM. */
ATF_TC_WITHOUT_HEAD(triple_fault);
ATF_TC_BODY(triple_fault, tc)
{

	harness_expect_failure(guest_triple_fault, NULL, (const char *const []){
	    "vCPU 0: expected DONE ucall, got SUSPENDED exit",
	    ": triple fault", NULL });
}
#endif

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, guest_done);
	ATF_TP_ADD_TC(tp, guest_return);
	ATF_TP_ADD_TC(tp, guest_sync);
	ATF_TP_ADD_TC(tp, guest_assert);
	ATF_TP_ADD_TC(tp, guest_assert_eq);
	ATF_TP_ADD_TC(tp, guest_bad_message);
	ATF_TP_ADD_TC(tp, unexpected_exit);
	ATF_TP_ADD_TC(tp, unexpected_ucall);
	ATF_TP_ADD_TC(tp, expect_exit);
	ATF_TP_ADD_TC(tp, expect_exit_ucall);
	ATF_TP_ADD_TC(tp, set_reg);
	ATF_TP_ADD_TC(tp, try_run);
	ATF_TP_ADD_TC(tp, exception_unhandled);
	ATF_TP_ADD_TC(tp, exception_undefined);
#if defined(__amd64__)
	ATF_TP_ADD_TC(tp, triple_fault);
#endif
	return (atf_no_error());
}
