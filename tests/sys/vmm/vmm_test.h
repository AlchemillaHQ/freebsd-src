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
 *    but a global changed on one side is not seen by the other.  The host
 *    reaches the guest's copy of a global with VMM_TEST_GUEST_VAR(), while
 *    the vCPU is stopped;
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
 * Guest-virtual addresses that tests may map: VMM_TEST_VA_SIZE bytes from
 * VMM_TEST_VA.  The test binary is loaded at a few megabytes, or with
 * address space layout randomization above the data size limit (1 GB on
 * arm64, 32 GB on amd64), and the harness maps nothing there.
 */
#define	VMM_TEST_VA		0x10000000UL
#define	VMM_TEST_VA_SIZE	0x10000000UL

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

/*
 * Guest memory.  Pages come from a simple allocator over the VM's memory and
 * are zeroed.  Guest-virtual mappings use the harness's own page tables, and
 * cover whole pages.
 */
vm_paddr_t vmm_test_alloc_pages(struct vmm_test_vm *, size_t);
void	*vmm_test_gpa2hva(struct vmm_test_vm *, vm_paddr_t, size_t);

/*
 * Mapping flags.  A mapping without VMM_TEST_PROT_READ is not present; one
 * with VMM_TEST_PROT_WRITE or VMM_TEST_PROT_EXEC must also be readable.
 * VMM_TEST_MAP_DEVICE selects uncached device memory: Device-nGnRnE on arm64,
 * UC on amd64.  On arm64 the guest's accesses to such memory bypass the
 * caches, so data the host writes there must first be cleaned to the point
 * of coherency.
 */
#define	VMM_TEST_PROT_READ	0x01
#define	VMM_TEST_PROT_WRITE	0x02
#define	VMM_TEST_PROT_EXEC	0x04
#define	VMM_TEST_PROT_ALL	\
	(VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE | VMM_TEST_PROT_EXEC)
#define	VMM_TEST_MAP_DEVICE	0x08

/*
 * Map, unmap or change the protection of the pages in [gva, gva + len).
 * Mapping fails if a page is already mapped, unmapping if one is not.
 * vmm_test_protect() keeps each page's guest-physical address and memory
 * type, so protecting a page without VMM_TEST_PROT_READ and then with it
 * restores the mapping.  It cannot change VMM_TEST_MAP_DEVICE, so the flags
 * for a device page always include it, and VMM_TEST_MAP_DEVICE alone makes
 * the page inaccessible.  To change the memory type, unmap the page, have
 * the guest flush it, and map it again.
 *
 * The host cannot flush the guest's TLB.  After any change to a page that
 * the guest may have accessed, other than mapping it, the guest must flush
 * it with vmm_guest_flush_tlb() before relying on the change.  Raising the
 * protection of a present page needs the flush too: until then, the guest
 * may still fault on it.
 */
void	vmm_test_map(struct vmm_test_vm *, uint64_t, vm_paddr_t, size_t, int);
void	vmm_test_unmap(struct vmm_test_vm *, uint64_t, size_t);
void	vmm_test_protect(struct vmm_test_vm *, uint64_t, size_t, int);

/*
 * Translate a guest-virtual address.  vmm_test_gva2hva() fails unless the
 * whole range is mapped to contiguous guest-physical memory.
 */
bool	vmm_test_gva2gpa(struct vmm_test_vm *, uint64_t, vm_paddr_t *);
void	*vmm_test_gva2hva(struct vmm_test_vm *, uint64_t, size_t);

/*
 * The guest's copy of a global variable in the test binary, for the host to
 * read or write while the vCPU is stopped.
 */
#define	VMM_TEST_GUEST_VAR(vm, var)					\
	((__typeof__(&(var)))vmm_test_gva2hva((vm), (uintptr_t)&(var),	\
	    sizeof(var)))

/*
 * Implemented by the machine-dependent code.  vmm_test_md_pte() returns the
 * last-level entry mapping a page, or NULL if a table on the way is missing
 * or the page is part of a larger mapping; tests may use it for bits the
 * mapping flags do not cover.
 */
void	vmm_test_md_vm_init(struct vmm_test_vm *);
void	vmm_test_md_vcpu_init(struct vmm_test_vcpu *, vmm_test_guest_fn_t *);
void	vmm_test_md_map(struct vmm_test_vm *, uint64_t, vm_paddr_t, int);
void	vmm_test_md_unmap(struct vmm_test_vm *, uint64_t);
void	vmm_test_md_protect(struct vmm_test_vm *, uint64_t, int);
uint64_t *vmm_test_md_pte(struct vmm_test_vm *, uint64_t);
bool	vmm_test_md_gva2gpa(struct vmm_test_vm *, uint64_t, vm_paddr_t *);
bool	vmm_test_md_exit_reenter(const struct vm_exit *);
bool	vmm_test_md_exit_ucall(struct vmm_test_vcpu *, uint64_t *);
const char *vmm_test_md_exitcode_name(int);
void	vmm_test_md_describe_exit(const struct vm_exit *, char *, size_t);
void	vmm_test_md_describe_exception(const struct vmm_ucall *, char *,
	    size_t);

#endif /* !_VMM_TEST_H_ */
