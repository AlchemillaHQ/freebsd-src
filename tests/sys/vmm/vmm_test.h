/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * vmm(4) test harness.
 *
 * Tests run small pieces of guest code in a VM and check how vmm(4) handles
 * them.  Guest code is an ordinary function in the test's source file: the
 * harness copies the test binary into the VM's memory, mapped at the same
 * virtual addresses it has in the host process, and starts a vCPU at that
 * function.  Guest functions run at CPL 0 on amd64 and EL1 on arm64, with
 * address translation enabled and interrupts disabled.
 *
 * A minimal amd64 test:
 *
 *	static void
 *	guest_efer(void)
 *	{
 *		GUEST_SYNC(1);
 *		GUEST_ASSERT_EQ(rdmsr(MSR_EFER) & EFER_LMA, EFER_LMA);
 *	}
 *
 *	ATF_TC_WITHOUT_HEAD(efer);
 *	ATF_TC_BODY(efer, tc)
 *	{
 *		struct vmm_test_vcpu *vcpu;
 *		struct vmm_ucall uc;
 *
 *		(void)vmm_test_vm_create_with_vcpu(guest_efer, &vcpu);
 *		vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, &uc);
 *		ATF_CHECK_EQ(1, uc.args[0]);
 *		vmm_test_run_until_done(vcpu);
 *	}
 *
 * The guest talks to the host with "ucalls", declared in vmm_test_guest.h:
 * GUEST_SYNC() stops the vCPU so that the host can inspect or change its
 * state, GUEST_DONE() (or returning from the guest function) reports success,
 * and GUEST_ASSERT(), GUEST_ASSERT_EQ() and GUEST_FAIL() report failure.  The
 * host runs the vCPU with vmm_test_vcpu_run(), which returns each exit the
 * harness does not handle itself, or with the vmm_test_run_*() wrappers,
 * which also check that the expected ucall or exit happened.  A failed guest
 * assertion, an unhandled guest exception, or an exit other than the one
 * expected fails the test case with the details, for example:
 *
 *	vCPU 0: guest assertion failed at foo_test.c:42: x == 1 (0 != 0x1)
 *	vCPU 0: expected DONE ucall, got SUSPENDED exit (code 14) at rip ...
 *	vCPU 0: unhandled guest exception #PF(0x2) at rip ..., address 0x1000
 *
 * Guest code runs on its own copy of the test binary, so:
 *
 *  - it may call other functions in the test binary and read its globals,
 *    but a global changed on one side is not seen by the other;
 *  - it must not call into shared libraries (libc included), use
 *    thread-local storage, or touch memory the host allocated at run time.
 *    The harness provides memset() and memcpy(), which the compiler may call
 *    to initialize or copy structures.
 *
 * The host decodes ucalls using the page tables the harness sets up, so guest
 * code must not switch to other page tables before making one.  Guests enable
 * SSE on amd64 and FP/Advanced SIMD on arm64.  The tests are built without
 * CPUTYPE-specific flags that could enable other extensions.  On amd64,
 * exceptions are taken on the interrupted code's stack, so the tests are
 * built without a red zone.
 *
 * The tests need root and a loaded vmm.ko with working hardware
 * virtualization; the Makefile declares this as test metadata.  Each VM is
 * destroyed when the test process exits, whether or not the test passes.
 * Run the tests with:
 *
 *	kyua test -k /usr/tests/sys/vmm/Kyuafile
 */

#ifndef _VMM_TEST_H_
#define	_VMM_TEST_H_

#include <sys/param.h>
#include <sys/cpuset.h>

#include <machine/vmm.h>

#include <dev/vmm/vmm_param.h>

#include <stdbool.h>
#include <stdint.h>

#include "vmm_test_machdep.h"

/* Guest-physical memory given to every test VM. */
#define	VMM_TEST_MEMSIZE	(64 * 1024 * 1024)

/*
 * A guest-virtual address that is never mapped: the test binary is loaded
 * above it, and the harness maps nothing there.
 */
#define	VMM_TEST_UNMAPPED_VA	0x1000UL

/*
 * Ucall ABI shared between guest and host.  The guest fills in a struct
 * vmm_ucall and traps to the host with a pointer to it (see
 * vmm_guest_ucall()).  'file' and 'msg' must point to string literals, or
 * other strings in the test binary that never change: the host reads its own
 * copy of them, and reports any other pointer instead of following it.
 */
enum vmm_ucall_cmd {
	UCALL_NONE,
	UCALL_SYNC,		/* args[0]: test-defined stage */
	UCALL_DONE,
	UCALL_ABORT,		/* args[0], args[1]: values, if args[2] != 0 */
	UCALL_EXCEPTION,	/* args: machine-dependent */
};

struct vmm_ucall {
	uint64_t	cmd;
	uint64_t	args[4];
	const char	*file;
	const char	*msg;
	int		line;
};

typedef void vmm_test_guest_fn_t(void);

struct vmm_test_vm {
	struct vmctx	*ctx;
	char		name[VM_MAX_NAMELEN + 1];
	vm_paddr_t	mem_base;
	vm_paddr_t	next_free;	/* page allocator watermark */
	struct vmm_test_vm_md md;
};

struct vmm_test_vcpu {
	struct vmm_test_vm *vm;
	struct vcpu	*vcpu;
	int		id;
	struct vm_exit	exit;		/* most recent exit */
	struct vmm_ucall ucall;		/* valid if ucall.cmd != UCALL_NONE */
};

/* Guest entry point, started by the machine-dependent vCPU setup. */
void	vmm_guest_start(vmm_test_guest_fn_t *) __dead2;

struct vmm_test_vm *vmm_test_vm_create(void);
struct vmm_test_vm *vmm_test_vm_create_with_vcpu(vmm_test_guest_fn_t *,
	    struct vmm_test_vcpu **);
struct vmm_test_vcpu *vmm_test_vcpu_add(struct vmm_test_vm *, int,
	    vmm_test_guest_fn_t *);

/*
 * Run the vCPU until it exits for a reason the harness does not handle
 * itself.  Guest assertion failures and unhandled guest exceptions fail the
 * test case.
 */
struct vm_exit *vmm_test_vcpu_run(struct vmm_test_vcpu *);

/*
 * As vmm_test_vcpu_run(), but return VM_RUN's errno instead of failing the
 * test case.  The exit is only valid if 0 is returned.
 */
int	vmm_test_vcpu_try_run(struct vmm_test_vcpu *);

/* Run the vCPU and fail unless it makes the given ucall. */
void	vmm_test_run_expect_ucall(struct vmm_test_vcpu *, uint64_t,
	    struct vmm_ucall *);
void	vmm_test_run_until_done(struct vmm_test_vcpu *);

/* Run the vCPU and fail unless its next exit has the given exit code. */
struct vm_exit *vmm_test_run_expect_exit(struct vmm_test_vcpu *,
	    enum vm_exitcode);

void	vmm_test_fail_exit(struct vmm_test_vcpu *, const char *) __dead2;

/*
 * Fail the test case.  Every failure the harness reports, including those
 * from the machine-dependent code, goes through vmm_test_fail().  If
 * vmm_test_fail_hook is set, it is called with the message first; the
 * harness's self-tests use it to check these messages.
 */
void	vmm_test_fail(const char *, ...) __dead2 __printflike(1, 2);
extern void (*vmm_test_fail_hook)(const char *);

uint64_t vmm_test_get_reg(struct vmm_test_vcpu *, int);
void	vmm_test_set_reg(struct vmm_test_vcpu *, int, uint64_t);

/* Allocate zeroed guest-physical pages, and access guest memory. */
vm_paddr_t vmm_test_alloc_pages(struct vmm_test_vm *, size_t);
void	*vmm_test_gpa2hva(struct vmm_test_vm *, vm_paddr_t, size_t);

/*
 * Implemented by the machine-dependent code.  vmm_test_md_map_page() maps a
 * page for reading, writing and execution in the harness's page tables.
 */
void	vmm_test_md_vm_init(struct vmm_test_vm *);
void	vmm_test_md_vcpu_init(struct vmm_test_vcpu *, vmm_test_guest_fn_t *);
void	vmm_test_md_map_page(struct vmm_test_vm *, uint64_t, vm_paddr_t);
bool	vmm_test_md_gva2gpa(struct vmm_test_vm *, uint64_t, vm_paddr_t *);
bool	vmm_test_md_exit_reenter(const struct vm_exit *);
bool	vmm_test_md_exit_ucall(struct vmm_test_vcpu *, uint64_t *);
const char *vmm_test_md_exitcode_name(int);
void	vmm_test_md_describe_exit(const struct vm_exit *, char *, size_t);
void	vmm_test_md_describe_exception(const struct vmm_ucall *, char *,
	    size_t);

#endif /* !_VMM_TEST_H_ */
