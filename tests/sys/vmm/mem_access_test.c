/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Tests of what guest code sees when it reads, writes and executes guest
 * memory: fresh memory, memory the host maps without write or execute
 * access, aliases, memory the host replaces or removes between runs of the
 * guest, device memory, and addresses with no memory behind them.
 */

#include <sys/param.h>
#include <sys/domainset.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <machine/pte.h>
#include <machine/vmm.h>
#include <machine/vmm_dev.h>

#include <dev/vmm/vmm_mem.h>

#include <errno.h>
#include <string.h>
#include <vmmapi.h>

#include <atf-c.h>

#include "vmm_test.h"
#include "vmm_test_guest.h"

/* Guest-physical memory for the tests, above the harness's own. */
#define	TEST_GPA	(8UL << 30)
/* Where guest code sees it. */
#define	TEST_VA		VMM_TEST_VA
#define	ALIAS_VA	(VMM_TEST_VA + 0x1000000)

#define	PAGES(n)	((size_t)(n) * PAGE_SIZE)
#define	WORDS		(PAGE_SIZE / sizeof(uint64_t))
#define	PROT_RW		(PROT_READ | PROT_WRITE)

/*
 * System memory segments beside the harness's, VM_SYSMEM, and a device memory
 * segment, which must use an identifier from VM_BOOTROM on.
 */
#define	SEG_A		(VM_SYSMEM + 1)
#define	SEG_B		(VM_SYSMEM + 2)
#define	SEG_DEV		VM_BOOTROM

#if defined(__amd64__)
#define	PC_REG		VM_REG_GUEST_RIP
/* The kernel's VIE_INVALID_GLA: no guest-virtual address was reported. */
#define	INVALID_GLA	(1UL << 63)
#define	RET_INSN	0xc3			/* ret */
#define	RET_LEN		1
#elif defined(__aarch64__)
#define	PC_REG		VM_REG_GUEST_PC
#define	RET_INSN	0xd65f03c0		/* ret */
#define	RET_LEN		4
#endif

/* Contents the host and the guest write and check; guest code calls it too. */
static uint64_t
pattern(uint64_t seed, u_int i)
{

	return (seed * 0x9e3779b97f4a7c15UL + i);
}

static const char *
error_name(int error)
{

	return (error == 0 ? "success" : strerror(error));
}

static void
map_memory(struct vmm_test_vm *vm, int segid, vm_paddr_t gpa, size_t len,
    int prot)
{

	ATF_REQUIRE_MSG(vm_mmap_memseg(vm->ctx, gpa, segid, 0, len, prot) == 0,
	    "mapping segment %d at %#jx with prot %#x: %s", segid,
	    (uintmax_t)gpa, prot, strerror(errno));
}

static void
unmap_memory(struct vmm_test_vm *vm, vm_paddr_t gpa, size_t len)
{

	ATF_REQUIRE_MSG(vm_munmap_memseg(vm->ctx, gpa, len) == 0,
	    "unmapping %#jx: %s", (uintmax_t)gpa, strerror(errno));
}

/*
 * Create a system memory segment, map it at 'gpa' with protection 'prot',
 * and return the host's view of it.  The host does not touch the memory, so
 * the guest's first access is the first access to each page.
 */
static uint64_t *
add_memory(struct vmm_test_vm *vm, int segid, vm_paddr_t gpa, size_t len,
    int prot)
{
	struct vm_memseg seg;
	void *p;

	memset(&seg, 0, sizeof(seg));
	seg.segid = segid;
	seg.len = len;
	seg.ds_policy = DOMAINSET_POLICY_INVALID;
	ATF_REQUIRE_MSG(ioctl(vm_get_device_fd(vm->ctx), VM_ALLOC_MEMSEG,
	    &seg) == 0, "allocating segment %d: %s", segid, strerror(errno));
	map_memory(vm, segid, gpa, len, prot);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vm_get_device_fd(vm->ctx), gpa);
	ATF_REQUIRE_MSG(p != MAP_FAILED, "mapping %#jx in the host: %s",
	    (uintmax_t)gpa, strerror(errno));
	return (p);
}

static void
fill(uint64_t *p, u_int nwords, uint64_t seed)
{
	u_int i;

	for (i = 0; i < nwords; i++)
		p[i] = pattern(seed, i);
}

/* Put a return instruction at the start of 'p' for the guest to call. */
static void
put_ret(void *p)
{
	uint32_t insn;

	insn = RET_INSN;
	memcpy(p, &insn, RET_LEN);
	__builtin___clear_cache(p, (char *)p + RET_LEN);
}

static void
guest_fresh_memory(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

	/* The first access to the first page reads, to the second writes. */
	GUEST_ASSERT_EQ(p[0], 0);
	p[0] = pattern(1, 0);
	GUEST_ASSERT_EQ(p[0], pattern(1, 0));
	p[WORDS] = pattern(1, 1);
	GUEST_ASSERT_EQ(p[WORDS], pattern(1, 1));
}

/*
 * Fresh memory reads as zero and takes writes, whether the guest's first
 * access to a page reads or writes, and the host sees what the guest wrote.
 */
ATF_TC_WITHOUT_HEAD(fresh_memory);
ATF_TC_BODY(fresh_memory, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem;

	vm = vmm_test_vm_create_with_vcpu(guest_fresh_memory, &vcpu);
	mem = add_memory(vm, SEG_A, TEST_GPA, PAGES(2), PROT_RW);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(2),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_until_done(vcpu);
	ATF_CHECK_EQ(mem[0], pattern(1, 0));
	ATF_CHECK_EQ(mem[WORDS], pattern(1, 1));
}

static void
guest_read_only(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;
	u_int i;

	for (i = 0; i < WORDS; i++)
		GUEST_ASSERT_EQ(p[i], pattern(2, i));
	GUEST_SYNC(1);
	p[1] = pattern(3, 1);		/* refused until the host allows it */
	GUEST_ASSERT_EQ(p[1], pattern(3, 1));
}

/*
 * The guest can read memory the host maps read-only.  A write makes VM_RUN
 * fail with EFAULT, without changing the memory or moving the guest past
 * the write, as often as it is retried.  Once the host maps the memory
 * writable, the write completes.
 *
 * EFAULT is how vmm(4) refuses an access that the host's mapping does not
 * allow, on every architecture; bhyve(8) then stops running the vCPU.
 */
ATF_TC_WITHOUT_HEAD(read_only);
ATF_TC_BODY(read_only, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem, pc;
	int error, try;

	vm = vmm_test_vm_create_with_vcpu(guest_read_only, &vcpu);
	mem = add_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_READ);
	fill(mem, WORDS, 2);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);

	pc = 0;
	for (try = 0; try < 2; try++) {
		error = vmm_test_vcpu_try_run(vcpu);
		ATF_REQUIRE_MSG(error == EFAULT,
		    "writing read-only memory: %s, expected %s",
		    error_name(error), strerror(EFAULT));
		if (try == 0)
			pc = vmm_test_get_reg(vcpu, PC_REG);
		else
			ATF_CHECK_EQ_MSG(vmm_test_get_reg(vcpu, PC_REG), pc,
			    "the guest moved past the refused write");
		ATF_CHECK_EQ_MSG(mem[1], pattern(2, 1),
		    "the refused write changed the memory");
	}

	unmap_memory(vm, TEST_GPA, PAGES(1));
	map_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_RW);
	vmm_test_run_until_done(vcpu);
	ATF_CHECK_EQ(mem[1], pattern(3, 1));
}

static void
guest_read_exec(void)
{
	volatile uint32_t *p = (volatile uint32_t *)TEST_VA;

	GUEST_ASSERT_EQ(p[0] & ((1UL << (8 * RET_LEN)) - 1), RET_INSN);
	((void (*)(void))TEST_VA)();
	GUEST_SYNC(1);
	p[1] = 0;			/* refused */
	GUEST_FAIL("wrote to memory the host maps without write access");
}

/*
 * The guest can read and execute memory the host maps readable and
 * executable, and a write makes VM_RUN fail with EFAULT, without changing
 * the memory or moving the guest past the write.
 */
ATF_TC_WITHOUT_HEAD(read_exec);
ATF_TC_BODY(read_exec, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem, pc;
	int error, try;

	vm = vmm_test_vm_create_with_vcpu(guest_read_exec, &vcpu);
	mem = add_memory(vm, SEG_A, TEST_GPA, PAGES(1),
	    PROT_READ | PROT_EXEC);
	put_ret(mem);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1), VMM_TEST_PROT_ALL);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);

	pc = 0;
	for (try = 0; try < 2; try++) {
		error = vmm_test_vcpu_try_run(vcpu);
		ATF_REQUIRE_MSG(error == EFAULT,
		    "writing memory mapped without write access: %s, "
		    "expected %s", error_name(error), strerror(EFAULT));
		if (try == 0)
			pc = vmm_test_get_reg(vcpu, PC_REG);
		else
			ATF_CHECK_EQ_MSG(vmm_test_get_reg(vcpu, PC_REG), pc,
			    "the guest moved past the refused write");
		ATF_CHECK_EQ_MSG(((uint32_t *)mem)[1], 0,
		    "the refused write changed the memory");
	}
}

static void
guest_read_write(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

	GUEST_ASSERT_EQ(p[1], pattern(4, 1));
	p[1] = pattern(5, 1);
	GUEST_ASSERT_EQ(p[1], pattern(5, 1));
	GUEST_SYNC(1);
	((void (*)(void))TEST_VA)();	/* refused */
	GUEST_FAIL("executed memory the host maps without execute access");
}

/*
 * The guest can read and write memory the host maps readable and writable,
 * and executing it makes VM_RUN fail with EFAULT at the first instruction.
 */
ATF_TC_WITHOUT_HEAD(read_write);
ATF_TC_BODY(read_write, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem;
	int error;

	vm = vmm_test_vm_create_with_vcpu(guest_read_write, &vcpu);
	mem = add_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_RW);
	put_ret(mem);
	mem[1] = pattern(4, 1);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1), VMM_TEST_PROT_ALL);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	ATF_CHECK_EQ(mem[1], pattern(5, 1));

	error = vmm_test_vcpu_try_run(vcpu);
	ATF_CHECK_MSG(error == EFAULT,
	    "executing memory mapped without execute access: %s, expected %s",
	    error_name(error), strerror(EFAULT));
	if (error == EFAULT)
		ATF_CHECK_EQ_MSG(vmm_test_get_reg(vcpu, PC_REG), TEST_VA,
		    "the refused instruction fetch was not at the start of "
		    "the memory");
}

static void
guest_alias(void)
{
	volatile uint64_t *a = (volatile uint64_t *)TEST_VA;
	volatile uint64_t *b = (volatile uint64_t *)ALIAS_VA;

	GUEST_ASSERT_EQ(b[2], pattern(6, 2));
	a[3] = pattern(7, 3);
	GUEST_ASSERT_EQ(b[3], pattern(7, 3));
	b[4] = pattern(8, 4);
	GUEST_ASSERT_EQ(a[4], pattern(8, 4));
}

/*
 * One segment mapped at two guest-physical addresses is the same memory
 * through either, for the guest and for the host.
 */
ATF_TC_WITHOUT_HEAD(alias);
ATF_TC_BODY(alias, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem;

	vm = vmm_test_vm_create_with_vcpu(guest_alias, &vcpu);
	mem = add_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_RW);
	map_memory(vm, SEG_A, TEST_GPA + PAGES(16), PAGES(1), PROT_RW);
	mem[2] = pattern(6, 2);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_map(vm, ALIAS_VA, TEST_GPA + PAGES(16), PAGES(1),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_until_done(vcpu);
	ATF_CHECK_EQ(mem[3], pattern(7, 3));
	ATF_CHECK_EQ(mem[4], pattern(8, 4));
}

static void
guest_remap(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

	GUEST_ASSERT_EQ(p[0], pattern(9, 0));
	GUEST_SYNC(1);			/* the host replaces the segment */
	GUEST_ASSERT_EQ(p[0], pattern(10, 0));
	GUEST_SYNC(2);			/* the host unmaps it */
	(void)p[0];			/* exits to the host */
	GUEST_SYNC(3);			/* the host maps the first again */
	GUEST_ASSERT_EQ(p[0], pattern(9, 0));
}

/*
 * When the host replaces or removes the memory at a guest-physical address
 * between runs, the guest's next access sees the change, without the guest
 * flushing anything: the new memory, an exit for memory that is gone, or
 * the old memory mapped again.
 */
ATF_TC_WITHOUT_HEAD(remap);
ATF_TC_BODY(remap, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	struct vm_exit *vme;
	uint64_t *a, *b;

	vm = vmm_test_vm_create_with_vcpu(guest_remap, &vcpu);
	a = add_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_RW);
	b = add_memory(vm, SEG_B, TEST_GPA + PAGES(32), PAGES(1), PROT_RW);
	a[0] = pattern(9, 0);
	b[0] = pattern(10, 0);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1), VMM_TEST_PROT_READ);

	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	unmap_memory(vm, TEST_GPA, PAGES(1));
	map_memory(vm, SEG_B, TEST_GPA, PAGES(1), PROT_RW);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	unmap_memory(vm, TEST_GPA, PAGES(1));
	vme = vmm_test_run_expect_exit(vcpu, VM_EXITCODE_INST_EMUL);
	ATF_CHECK_EQ_MSG(vme->u.inst_emul.gpa, TEST_GPA,
	    "an access to removed memory at %#jx exited for %#jx",
	    (uintmax_t)TEST_GPA, (uintmax_t)vme->u.inst_emul.gpa);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	map_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_RW);
	vmm_test_run_until_done(vcpu);
}

/*
 * Loads and stores to unbacked memory: the instruction, the offset from
 * TEST_VA, and on arm64 what the exit must report: the direction, size,
 * sign extension and register.  The guest runs them in this order with the
 * base address in x0 or %rdi.
 */
#if defined(__amd64__)
#define	ACCESSES(X)							\
	X("movb 0x10(%%rdi), %%al", 0x10)				\
	X("movw 0x20(%%rdi), %%ax", 0x20)				\
	X("movl 0x30(%%rdi), %%eax", 0x30)				\
	X("movq 0x40(%%rdi), %%rax", 0x40)				\
	X("movzbl 0x51(%%rdi), %%eax", 0x51)				\
	X("movsbq 0x62(%%rdi), %%rax", 0x62)				\
	X("movb %%al, 0x73(%%rdi)", 0x73)				\
	X("movw %%ax, 0x84(%%rdi)", 0x84)				\
	X("movl %%eax, 0x98(%%rdi)", 0x98)				\
	X("movq %%rax, 0xff8(%%rdi)", 0xff8)

#define	ACCESS_INSN(insn, off)	insn "\n\t"
#define	ACCESS_ENTRY(insn, off)	{ insn, off },

struct access {
	const char	*insn;
	u_int		off;
};
#elif defined(__aarch64__)
#define	RD	VM_DIR_READ
#define	WR	VM_DIR_WRITE
#define	ACCESSES(X)							\
	X("ldrb w1, [x0, #0x10]", 0x10, RD, 1, 0, 1)			\
	X("ldrh w1, [x0, #0x20]", 0x20, RD, 2, 0, 1)			\
	X("ldr w1, [x0, #0x30]", 0x30, RD, 4, 0, 1)			\
	X("ldr x1, [x0, #0x40]", 0x40, RD, 8, 0, 1)			\
	X("ldrsb x1, [x0, #0x51]", 0x51, RD, 1, 1, 1)			\
	X("ldrsh w1, [x0, #0x62]", 0x62, RD, 2, 1, 1)			\
	X("ldrsw x1, [x0, #0x74]", 0x74, RD, 4, 1, 1)			\
	X("ldr x2, [x0, #0x88]", 0x88, RD, 8, 0, 2)			\
	X("strb w1, [x0, #0x93]", 0x93, WR, 1, 0, 1)			\
	X("strh w1, [x0, #0xa4]", 0xa4, WR, 2, 0, 1)			\
	X("str w1, [x0, #0xb8]", 0xb8, WR, 4, 0, 1)			\
	X("str x1, [x0, #0xff8]", 0xff8, WR, 8, 0, 1)			\
	X("str xzr, [x0, #0xc0]", 0xc0, WR, 8, 0, 31)

#define	ACCESS_INSN(insn, off, dir, size, sign, reg)	insn "\n\t"
#define	ACCESS_ENTRY(insn, off, dir, size, sign, reg)	\
	{ insn, off, dir, size, sign, reg },

struct access {
	const char	*insn;
	u_int		off;
	int		dir;
	int		size;
	int		sign;
	int		reg;
};
#endif

static const struct access accesses[] = { ACCESSES(ACCESS_ENTRY) };

static void
guest_unbacked_access(void)
{
#if defined(__amd64__)
	uint64_t val = 0;

	__asm __volatile(ACCESSES(ACCESS_INSN)
	    : "+a" (val) : "D" (TEST_VA) : "memory");
#elif defined(__aarch64__)
	register uint64_t base __asm("x0") = TEST_VA;

	__asm __volatile(ACCESSES(ACCESS_INSN)
	    : : "r" (base) : "x1", "x2", "memory");
#endif
}

/*
 * Each load or store to memory with nothing behind it exits to the host,
 * which can let the guest continue after it, and the exit describes the
 * access: its guest-physical address, and on amd64 its guest-virtual
 * address where the hardware reports one, or on arm64 its direction, size,
 * sign extension and register.
 */
ATF_TC_WITHOUT_HEAD(unbacked_access);
ATF_TC_BODY(unbacked_access, tc)
{
	struct vmm_test_vcpu *vcpu;
	const struct access *a;
	struct vm_exit *vme;
	u_int i;

	(void)vmm_test_vm_create_with_vcpu(guest_unbacked_access, &vcpu);
	vmm_test_map(vcpu->vm, TEST_VA, TEST_GPA, PAGES(1),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	for (i = 0; i < nitems(accesses); i++) {
		a = &accesses[i];
		vme = vmm_test_run_expect_exit(vcpu, VM_EXITCODE_INST_EMUL);
		ATF_CHECK_EQ_MSG(vme->u.inst_emul.gpa, TEST_GPA + a->off,
		    "%s: reported address %#jx, expected %#jx", a->insn,
		    (uintmax_t)vme->u.inst_emul.gpa,
		    (uintmax_t)(TEST_GPA + a->off));
#if defined(__amd64__)
		/* AMD's nested page faults do not report the address. */
		ATF_CHECK_MSG(vme->u.inst_emul.gla == TEST_VA + a->off ||
		    vme->u.inst_emul.gla == INVALID_GLA,
		    "%s: reported guest-virtual address %#jx, expected %#jx "
		    "or none", a->insn,
		    (uintmax_t)vme->u.inst_emul.gla,
		    (uintmax_t)(TEST_VA + a->off));
#elif defined(__aarch64__)
		ATF_CHECK_MSG(vme->u.inst_emul.vie.dir == a->dir &&
		    vme->u.inst_emul.vie.access_size == a->size &&
		    vme->u.inst_emul.vie.sign_extend == a->sign &&
		    (int)vme->u.inst_emul.vie.reg == a->reg,
		    "%s: reported a %d-byte %s%s of register %d, expected a "
		    "%d-byte %s%s of register %d", a->insn,
		    vme->u.inst_emul.vie.access_size,
		    vme->u.inst_emul.vie.dir == WR ? "write" : "read",
		    vme->u.inst_emul.vie.sign_extend ? " (sign-extended)" : "",
		    (int)vme->u.inst_emul.vie.reg, a->size,
		    a->dir == WR ? "write" : "read",
		    a->sign ? " (sign-extended)" : "", a->reg);
#endif
	}
	vmm_test_run_until_done(vcpu);
}

static void
guest_devmem(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

	GUEST_ASSERT_EQ(p[5], pattern(11, 5));
	p[6] = pattern(12, 6);
	GUEST_ASSERT_EQ(p[6], pattern(12, 6));
}

/*
 * Device memory mapped into the guest is the same memory the host sees
 * through vm_create_devmem().
 */
ATF_TC_WITHOUT_HEAD(devmem);
ATF_TC_BODY(devmem, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem;

	vm = vmm_test_vm_create_with_vcpu(guest_devmem, &vcpu);
	mem = vm_create_devmem(vm->ctx, SEG_DEV, "vmm-test-dev", PAGES(1));
	ATF_REQUIRE_MSG(mem != MAP_FAILED, "creating device memory: %s",
	    strerror(errno));
	map_memory(vm, SEG_DEV, TEST_GPA, PAGES(1), PROT_RW);
	mem[5] = pattern(11, 5);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_until_done(vcpu);
	ATF_CHECK_EQ(mem[6], pattern(12, 6));
}

static void
guest_page_table_updates(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

#if defined(__aarch64__)
	GUEST_SYNC(READ_SPECIALREG(id_aa64mmfr1_el1));
#endif
	(void)p[0];
	GUEST_SYNC(1);
	p[1] = pattern(13, 1);
	GUEST_SYNC(2);
	GUEST_ASSERT_EQ(p[1], pattern(13, 1));
}

/*
 * The processor's own updates to the guest's page tables work: the first
 * access to a page sets the accessed flag in its page table entry, and on
 * amd64 the first write sets the dirty flag.  On arm64 the guest enables
 * hardware access flag updates (TCR_EL1.HA), where it has them, and the
 * host-built page tables start with the flag clear.
 */
ATF_TC_WITHOUT_HEAD(page_table_updates);
ATF_TC_BODY(page_table_updates, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *pte;
#if defined(__aarch64__)
	struct vmm_ucall uc;
#endif

	vm = vmm_test_vm_create_with_vcpu(guest_page_table_updates, &vcpu);
	vmm_test_map(vm, TEST_VA, vmm_test_alloc_pages(vm, 1), PAGES(1),
	    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	pte = vmm_test_md_pte(vm, TEST_VA);
	ATF_REQUIRE(pte != NULL);
#if defined(__amd64__)
	*pte &= ~(X86_PG_A | X86_PG_M);
#elif defined(__aarch64__)
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, &uc);
	if (ID_AA64MMFR1_HAFDBS_VAL(uc.args[0]) == ID_AA64MMFR1_HAFDBS_NONE)
		atf_tc_skip("the guest has no hardware access flag updates");
	vmm_test_set_reg(vcpu, VM_REG_GUEST_TCR_EL1,
	    vmm_test_get_reg(vcpu, VM_REG_GUEST_TCR_EL1) | TCR_HA);
	*pte &= ~ATTR_AF;
#endif

	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
#if defined(__amd64__)
	ATF_CHECK_MSG((*pte & X86_PG_A) != 0,
	    "a read did not set the accessed flag: %#jx", (uintmax_t)*pte);
	ATF_CHECK_MSG((*pte & X86_PG_M) == 0,
	    "a read set the dirty flag: %#jx", (uintmax_t)*pte);
#elif defined(__aarch64__)
	ATF_CHECK_MSG((*pte & ATTR_AF) != 0,
	    "a read did not set the access flag: %#jx", (uintmax_t)*pte);
#endif
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
#if defined(__amd64__)
	ATF_CHECK_MSG((*pte & X86_PG_M) != 0,
	    "a write did not set the dirty flag: %#jx", (uintmax_t)*pte);
#endif
	vmm_test_run_until_done(vcpu);
}

#if defined(__aarch64__)
/*
 * Loads and stores whose exception syndrome does not describe them: ISV is
 * clear, because they move two registers, write back their base register,
 * or use a SIMD register.
 */
#define	UNDESCRIBED(X)							\
	X(0, "a load pair", "ldp x1, x2, [x0]")				\
	X(1, "a store pair", "stp x1, x2, [x0]")			\
	X(2, "a load with writeback", "ldr x1, [x0], #8")		\
	X(3, "a SIMD store", "str q0, [x0]")

#define	UNDESCRIBED_NAME(i, what, insn)	what,
static const char *const undescribed_names[] = {
	UNDESCRIBED(UNDESCRIBED_NAME)
};
static int undescribed_form;

static void
guest_undescribed(void)
{
	register uint64_t base __asm("x0") = TEST_VA;
	uint64_t error;
	int vector;

	vector = GUEST_NO_EXCEPTION;
	error = 0;
	switch (undescribed_form) {
#define	UNDESCRIBED_CASE(i, what, insn)					\
	case i:								\
		__asm __volatile(GUEST_ASM_SAFE(insn)			\
		    : GUEST_ASM_SAFE_OUTPUTS(vector, error),		\
		      "+r" (base)					\
		    :							\
		    : GUEST_ASM_SAFE_CLOBBERS, "x1", "x2", "v0", "memory"); \
		break;
	UNDESCRIBED(UNDESCRIBED_CASE)
#undef UNDESCRIBED_CASE
	}
	GUEST_SYNC(vector);
	(void)error;
}

/*
 * vmm(4) must not describe such an access to the host as a different one.
 * It may instead deliver an abort to the guest or fail VM_RUN, but an
 * emulation exit would make the host emulate the wrong access.
 */
ATF_TC_WITHOUT_HEAD(unbacked_undescribed);
ATF_TC_BODY(unbacked_undescribed, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	struct vm_exit *vme;
	char desc[128];
	u_int i;
	int error;

	for (i = 0; i < nitems(undescribed_names); i++) {
		vm = vmm_test_vm_create_with_vcpu(guest_undescribed, &vcpu);
		*VMM_TEST_GUEST_VAR(vm, undescribed_form) = i;
		vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1),
		    VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
		error = vmm_test_vcpu_try_run(vcpu);
		if (error != 0)
			continue;	/* VM_RUN refused the access */
		vme = &vcpu->exit;
		if (vcpu->ucall.cmd == UCALL_SYNC) {
			ATF_CHECK_MSG(vcpu->ucall.args[0] == EXCP_DATA_ABORT,
			    "%s to unbacked memory: the guest saw exception "
			    "class %#jx, expected a data abort (%#x)",
			    undescribed_names[i],
			    (uintmax_t)vcpu->ucall.args[0], EXCP_DATA_ABORT);
		} else if (vme->exitcode == VM_EXITCODE_INST_EMUL) {
			atf_tc_fail_nonfatal("%s to unbacked memory at %#jx "
			    "was reported as a %d-byte %s of register %d",
			    undescribed_names[i],
			    (uintmax_t)vme->u.inst_emul.gpa,
			    vme->u.inst_emul.vie.access_size,
			    vme->u.inst_emul.vie.dir == WR ? "write" : "read",
			    (int)vme->u.inst_emul.vie.reg);
		} else {
			vmm_test_md_describe_exit(vme, desc, sizeof(desc));
			atf_tc_fail_nonfatal("%s to unbacked memory: "
			    "unexpected %s", undescribed_names[i], desc);
		}
	}
}

static void
guest_cache_maint_ram(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

	GUEST_ASSERT_EQ(p[0], 0);	/* only read so far */
	__asm __volatile("dc cvac, %0" : : "r" (TEST_VA) : "memory");
	__asm __volatile("dc ivac, %0" : : "r" (TEST_VA + 64) : "memory");
	p[0] = pattern(14, 0);
	GUEST_ASSERT_EQ(p[0], pattern(14, 0));
}

/*
 * Cache maintenance by address works on memory the guest has only read:
 * DC CVAC needs read access to the page, and DC IVAC, which can discard
 * data, needs write access.
 */
ATF_TC_WITHOUT_HEAD(cache_maint_ram);
ATF_TC_BODY(cache_maint_ram, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_cache_maint_ram, &vcpu);
	vmm_test_map(vcpu->vm, TEST_VA, vmm_test_alloc_pages(vcpu->vm, 1),
	    PAGES(1), VMM_TEST_PROT_READ | VMM_TEST_PROT_WRITE);
	vmm_test_run_until_done(vcpu);
}

static void
guest_cache_maint_read_only(void)
{
	volatile uint64_t *p = (volatile uint64_t *)TEST_VA;

	GUEST_ASSERT_EQ(p[0], pattern(15, 0));
	__asm __volatile("dc cvac, %0" : : "r" (TEST_VA) : "memory");
	GUEST_SYNC(1);
	__asm __volatile("dc ivac, %0" : : "r" (TEST_VA) : "memory");
	GUEST_SYNC(2);
}

/*
 * On memory the host maps read-only, DC CVAC works.  DC IVAC needs write
 * access, so it either makes VM_RUN fail with EFAULT, like a write, or, on
 * an implementation that does not check, completes; either way the memory
 * keeps its contents.
 */
ATF_TC_WITHOUT_HEAD(cache_maint_read_only);
ATF_TC_BODY(cache_maint_read_only, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	uint64_t *mem;
	int error;

	vm = vmm_test_vm_create_with_vcpu(guest_cache_maint_read_only, &vcpu);
	mem = add_memory(vm, SEG_A, TEST_GPA, PAGES(1), PROT_READ);
	mem[0] = pattern(15, 0);
	vmm_test_map(vm, TEST_VA, TEST_GPA, PAGES(1), VMM_TEST_PROT_READ);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	error = vmm_test_vcpu_try_run(vcpu);
	ATF_CHECK_MSG(error == EFAULT ||
	    (error == 0 && vcpu->ucall.cmd == UCALL_SYNC),
	    "DC IVAC on read-only memory: %s, expected %s or completion",
	    error_name(error), strerror(EFAULT));
	ATF_CHECK_EQ_MSG(mem[0], pattern(15, 0),
	    "DC IVAC changed read-only memory");
}

static void
guest_exec_unbacked(void)
{

	vmm_guest_capture_exception(EXCP_INSN_ABORT);
	((void (*)(void))TEST_VA)();
	GUEST_ASSERT_EQ(vmm_guest_captured.count, 1);
}

/*
 * Executing memory with nothing behind it gives the guest an instruction
 * abort for an external abort at the fetched address: vmm(4) does not
 * emulate instruction fetches from device memory.
 */
ATF_TC_WITHOUT_HEAD(exec_unbacked);
ATF_TC_BODY(exec_unbacked, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct trapframe *tf;

	(void)vmm_test_vm_create_with_vcpu(guest_exec_unbacked, &vcpu);
	vmm_test_map(vcpu->vm, TEST_VA, TEST_GPA, PAGES(1), VMM_TEST_PROT_ALL);
	tf = vmm_test_run_expect_exception(vcpu, EXCP_INSN_ABORT);
	ATF_CHECK_EQ_MSG(tf->tf_esr & ISS_INSN_IFSC_MASK, ISS_DATA_DFSC_EXT,
	    "the instruction abort has fault status %#jx, expected an "
	    "external abort (%#x)", (uintmax_t)(tf->tf_esr &
	    ISS_INSN_IFSC_MASK), ISS_DATA_DFSC_EXT);
	ATF_CHECK_EQ_MSG(tf->tf_far, TEST_VA,
	    "the instruction abort is for %#jx, expected %#jx",
	    (uintmax_t)tf->tf_far, (uintmax_t)TEST_VA);
	tf->tf_elr = tf->tf_lr;		/* return from the call */
	vmm_test_run_until_done(vcpu);
}
#endif

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, fresh_memory);
	ATF_TP_ADD_TC(tp, read_only);
	ATF_TP_ADD_TC(tp, read_exec);
	ATF_TP_ADD_TC(tp, read_write);
	ATF_TP_ADD_TC(tp, alias);
	ATF_TP_ADD_TC(tp, remap);
	ATF_TP_ADD_TC(tp, unbacked_access);
	ATF_TP_ADD_TC(tp, devmem);
	ATF_TP_ADD_TC(tp, page_table_updates);
#if defined(__aarch64__)
	ATF_TP_ADD_TC(tp, cache_maint_ram);
	ATF_TP_ADD_TC(tp, cache_maint_read_only);
	ATF_TP_ADD_TC(tp, unbacked_undescribed);
	ATF_TP_ADD_TC(tp, exec_unbacked);
#endif
	return (atf_no_error());
}
