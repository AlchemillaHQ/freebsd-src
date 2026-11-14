/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Tests of guest exception handling: handlers, the exception frame,
 * GUEST_ASM_SAFE(), exceptions injected by the host, and capturing an
 * exception for the host to inspect and resolve.
 */

#include <sys/param.h>

#if defined(__amd64__)
#include <machine/pte.h>
#endif

#include <errno.h>
#include <string.h>
#include <vmmapi.h>

#include <atf-c.h>

#include "harness_util.h"
#include "vmm_test.h"
#include "vmm_test_guest.h"

#define	EDIT_VALUE	0xfeedfacecafebeefUL
#define	FRAME_VAL(i)	(0x1111111111111111UL * ((i) + 1))

/*
 * The instructions and frame fields that the tests use on both
 * architectures.  EDIT_REG is the register that the host sets in a captured
 * frame.
 */
#if defined(__amd64__)
#define	UNDEF_INSN	"ud2"
#define	UNDEF_LEN	2
#define	UNDEF_VECTOR	IDT_UD
#define	UNDEF_ERROR	0
#define	TRAP_INSN	"int3"
#define	TRAP_VECTOR	IDT_BP
#define	FAULT_VECTOR	IDT_PF
#define	LOAD_INSN	"movq (%[addr]), %[val]"
#define	STORE_INSN	"movq %[val], (%[addr])"
#define	EDIT_REG	"rax"
#define	FRAME_EDIT_REG(tf)	((tf)->tf_rax)
#define	FRAME_PC(tf)	((tf)->tf_rip)
#define	FRAME_SP(tf)	((tf)->tf_rsp)
#elif defined(__aarch64__)
#define	UNDEF_INSN	"udf #0"
#define	UNDEF_LEN	4
#define	UNDEF_VECTOR	EXCP_UNKNOWN
#define	UNDEF_ERROR	ESR_ELx_IL
#define	TRAP_INSN	"svc #0"
#define	TRAP_VECTOR	EXCP_SVC64
#define	FAULT_VECTOR	EXCP_DATA_ABORT
#define	LOAD_INSN	"ldr %x[val], [%x[addr]]"
#define	STORE_INSN	"str %x[val], [%x[addr]]"
#define	EDIT_REG	"x0"
#define	FRAME_EDIT_REG(tf)	((tf)->tf_x[0])
#define	FRAME_PC(tf)	((tf)->tf_elr)
#define	FRAME_SP(tf)	((tf)->tf_sp)
#define	ESR_DFSC_TF(esr)	(((esr) & ISS_DATA_DFSC_MASK & ~3UL) == 0x04)
#endif

static int handler_count, other_count, other_vector;
static uint64_t handler_pc, handler_sp;
#if defined(__aarch64__)
static uint64_t other_syndrome;
#endif

static void
guest_undef_handler(struct trapframe *tf, int vector)
{

	GUEST_ASSERT_EQ(vector, UNDEF_VECTOR);
#if defined(__amd64__)
	GUEST_ASSERT_EQ(tf->tf_trapno, IDT_UD);
	GUEST_ASSERT_EQ(tf->tf_err, 0);
#elif defined(__aarch64__)
	GUEST_ASSERT_EQ(ESR_ELx_EXCEPTION(tf->tf_esr), EXCP_UNKNOWN);
#endif
	handler_sp = FRAME_SP(tf);
	FRAME_PC(tf) += UNDEF_LEN;
	handler_pc = FRAME_PC(tf);
	handler_count++;
}

static void
guest_other_handler(struct trapframe *tf, int vector)
{

	other_count++;
	other_vector = vector;
#if defined(__amd64__)
	GUEST_ASSERT_EQ(tf->tf_trapno, vector);
#elif defined(__aarch64__)
	other_syndrome = tf->tf_esr & ESR_ELx_ISS_MASK;
#endif
}

static void
guest_handler(void)
{
	uint64_t pc, sp;

	vmm_guest_set_handler(UNDEF_VECTOR, guest_undef_handler);
#if defined(__amd64__)
	__asm __volatile(
	    "movq %%rsp, %[sp]\n"
	    "1:\tud2\n\t"
	    "leaq 1b(%%rip), %[pc]"
	    : [pc] "=r" (pc), [sp] "=r" (sp) : : "memory");
#elif defined(__aarch64__)
	__asm __volatile(
	    "mov %[sp], sp\n\t"
	    "adr %[pc], 1f\n"
	    "1:\tudf #0"
	    : [pc] "=r" (pc), [sp] "=r" (sp) : : "memory");
#endif
	GUEST_ASSERT_EQ(handler_count, 1);
	GUEST_ASSERT_EQ(handler_pc, pc + UNDEF_LEN);
	GUEST_ASSERT_EQ(handler_sp, sp);

#if defined(__amd64__)
	/* Vectors from 0x80 have longer entry stubs. */
	vmm_guest_set_handler(0x40, guest_other_handler);
	vmm_guest_set_handler(0xff, guest_other_handler);
	__asm __volatile("int $0x40" : : : "memory");
	GUEST_ASSERT_EQ(other_vector, 0x40);
	__asm __volatile("int $0xff" : : : "memory");
	GUEST_ASSERT_EQ(other_vector, 0xff);
	GUEST_ASSERT_EQ(other_count, 2);
#elif defined(__aarch64__)
	/* The preferred return address of SVC is the next instruction. */
	vmm_guest_set_handler(EXCP_SVC64, guest_other_handler);
	__asm __volatile("svc #0x123" : : : "memory");
	GUEST_ASSERT_EQ(other_vector, EXCP_SVC64);
	GUEST_ASSERT_EQ(other_syndrome, 0x123);
	GUEST_ASSERT_EQ(other_count, 1);
#endif
}

/* An installed handler gets the exception and can resume the guest. */
ATF_TC_WITHOUT_HEAD(handler);
ATF_TC_BODY(handler, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_handler, &vcpu);
	vmm_test_run_until_done(vcpu);
}

#if defined(__amd64__)
/* The general-purpose registers in the order guest_frame() uses. */
#define	FRAME_REG(tf, i)	(*frame_reg(tf, i))

static register_t *
frame_reg(struct trapframe *tf, u_int i)
{
	register_t *regs[] = {
		&tf->tf_rax, &tf->tf_rbx, &tf->tf_rcx, &tf->tf_rdx,
		&tf->tf_rsi, &tf->tf_rdi, &tf->tf_r8, &tf->tf_r9,
		&tf->tf_r10, &tf->tf_r11, &tf->tf_r12, &tf->tf_r13,
		&tf->tf_r14, &tf->tf_r15,
	};

	return (regs[i]);
}
#define	NFRAME_REGS	14
#elif defined(__aarch64__)
/* x0-x15 and x19-x28: not x16-x18 (IP0, IP1, platform) or x29 (FP). */
#define	FRAME_REG(tf, i)	((tf)->tf_x[(i) < 16 ? (i) : (i) + 3])
#define	NFRAME_REGS	26
#endif

static void
guest_frame_handler(struct trapframe *tf, int vector)
{
	u_int i;

	GUEST_ASSERT_EQ(vector, TRAP_VECTOR);
	for (i = 0; i < NFRAME_REGS; i++) {
		GUEST_ASSERT_EQ(FRAME_REG(tf, i), FRAME_VAL(i));
		FRAME_REG(tf, i) = ~FRAME_VAL(i);
	}
}

static void
guest_frame(void)
{
	uint64_t regs[NFRAME_REGS];
	u_int i;

	vmm_guest_set_handler(TRAP_VECTOR, guest_frame_handler);
#if defined(__amd64__)
	{
		register uint64_t r8 __asm("r8") = FRAME_VAL(6);
		register uint64_t r9 __asm("r9") = FRAME_VAL(7);
		register uint64_t r10 __asm("r10") = FRAME_VAL(8);
		register uint64_t r11 __asm("r11") = FRAME_VAL(9);
		register uint64_t r12 __asm("r12") = FRAME_VAL(10);
		register uint64_t r13 __asm("r13") = FRAME_VAL(11);
		register uint64_t r14 __asm("r14") = FRAME_VAL(12);
		register uint64_t r15 __asm("r15") = FRAME_VAL(13);
		uint64_t rax, rbx, rcx, rdx, rsi, rdi;

		rax = FRAME_VAL(0);
		rbx = FRAME_VAL(1);
		rcx = FRAME_VAL(2);
		rdx = FRAME_VAL(3);
		rsi = FRAME_VAL(4);
		rdi = FRAME_VAL(5);
		__asm __volatile("int3"
		    : "+a" (rax), "+b" (rbx), "+c" (rcx), "+d" (rdx),
		      "+S" (rsi), "+D" (rdi), "+r" (r8), "+r" (r9),
		      "+r" (r10), "+r" (r11), "+r" (r12), "+r" (r13),
		      "+r" (r14), "+r" (r15)
		    : : "memory");
		memcpy(regs, (uint64_t [NFRAME_REGS]){ rax, rbx, rcx, rdx,
		    rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15 },
		    sizeof(regs));
	}
#elif defined(__aarch64__)
	{
		register uint64_t x0 __asm("x0") = FRAME_VAL(0);
		register uint64_t x1 __asm("x1") = FRAME_VAL(1);
		register uint64_t x2 __asm("x2") = FRAME_VAL(2);
		register uint64_t x3 __asm("x3") = FRAME_VAL(3);
		register uint64_t x4 __asm("x4") = FRAME_VAL(4);
		register uint64_t x5 __asm("x5") = FRAME_VAL(5);
		register uint64_t x6 __asm("x6") = FRAME_VAL(6);
		register uint64_t x7 __asm("x7") = FRAME_VAL(7);
		register uint64_t x8 __asm("x8") = FRAME_VAL(8);
		register uint64_t x9 __asm("x9") = FRAME_VAL(9);
		register uint64_t x10 __asm("x10") = FRAME_VAL(10);
		register uint64_t x11 __asm("x11") = FRAME_VAL(11);
		register uint64_t x12 __asm("x12") = FRAME_VAL(12);
		register uint64_t x13 __asm("x13") = FRAME_VAL(13);
		register uint64_t x14 __asm("x14") = FRAME_VAL(14);
		register uint64_t x15 __asm("x15") = FRAME_VAL(15);
		register uint64_t x19 __asm("x19") = FRAME_VAL(16);
		register uint64_t x20 __asm("x20") = FRAME_VAL(17);
		register uint64_t x21 __asm("x21") = FRAME_VAL(18);
		register uint64_t x22 __asm("x22") = FRAME_VAL(19);
		register uint64_t x23 __asm("x23") = FRAME_VAL(20);
		register uint64_t x24 __asm("x24") = FRAME_VAL(21);
		register uint64_t x25 __asm("x25") = FRAME_VAL(22);
		register uint64_t x26 __asm("x26") = FRAME_VAL(23);
		register uint64_t x27 __asm("x27") = FRAME_VAL(24);
		register uint64_t x28 __asm("x28") = FRAME_VAL(25);

		__asm __volatile("svc #0"
		    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4),
		      "+r" (x5), "+r" (x6), "+r" (x7), "+r" (x8), "+r" (x9),
		      "+r" (x10), "+r" (x11), "+r" (x12), "+r" (x13),
		      "+r" (x14), "+r" (x15), "+r" (x19), "+r" (x20),
		      "+r" (x21), "+r" (x22), "+r" (x23), "+r" (x24),
		      "+r" (x25), "+r" (x26), "+r" (x27), "+r" (x28)
		    : : "memory");
		memcpy(regs, (uint64_t [NFRAME_REGS]){ x0, x1, x2, x3, x4,
		    x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x19,
		    x20, x21, x22, x23, x24, x25, x26, x27, x28 },
		    sizeof(regs));
	}
#endif
	for (i = 0; i < NFRAME_REGS; i++)
		GUEST_ASSERT_EQ(regs[i], ~FRAME_VAL(i));
}

/*
 * The frame holds the interrupted code's registers, and changes to it take
 * effect on return.
 */
ATF_TC_WITHOUT_HEAD(frame);
ATF_TC_BODY(frame, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_frame, &vcpu);
	vmm_test_run_until_done(vcpu);
}

/* Clobber the caller-saved FP/SIMD registers, which the entry code saves. */
static void
guest_fp_clobber_handler(struct trapframe *tf __unused, int vector __unused)
{

#if defined(__amd64__)
	__asm __volatile(
	    "xorps %%xmm0, %%xmm0\n\t"	"xorps %%xmm1, %%xmm1\n\t"
	    "xorps %%xmm2, %%xmm2\n\t"	"xorps %%xmm3, %%xmm3\n\t"
	    "xorps %%xmm4, %%xmm4\n\t"	"xorps %%xmm5, %%xmm5\n\t"
	    "xorps %%xmm6, %%xmm6\n\t"	"xorps %%xmm7, %%xmm7\n\t"
	    "xorps %%xmm8, %%xmm8\n\t"	"xorps %%xmm9, %%xmm9\n\t"
	    "xorps %%xmm10, %%xmm10\n\t" "xorps %%xmm11, %%xmm11\n\t"
	    "xorps %%xmm12, %%xmm12\n\t" "xorps %%xmm13, %%xmm13\n\t"
	    "xorps %%xmm14, %%xmm14\n\t" "xorps %%xmm15, %%xmm15"
	    : : : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
	    "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
	    "xmm14", "xmm15");
#elif defined(__aarch64__)
	__asm __volatile(
	    "movi v0.16b, #0\n\t"	"movi v1.16b, #0\n\t"
	    "movi v2.16b, #0\n\t"	"movi v3.16b, #0\n\t"
	    "movi v4.16b, #0\n\t"	"movi v5.16b, #0\n\t"
	    "movi v6.16b, #0\n\t"	"movi v7.16b, #0\n\t"
	    "movi v16.16b, #0\n\t"	"movi v17.16b, #0\n\t"
	    "movi v18.16b, #0\n\t"	"movi v19.16b, #0\n\t"
	    "movi v20.16b, #0\n\t"	"movi v21.16b, #0\n\t"
	    "movi v22.16b, #0\n\t"	"movi v23.16b, #0\n\t"
	    "movi v24.16b, #0\n\t"	"movi v25.16b, #0\n\t"
	    "movi v26.16b, #0\n\t"	"movi v27.16b, #0\n\t"
	    "movi v28.16b, #0\n\t"	"movi v29.16b, #0\n\t"
	    "movi v30.16b, #0\n\t"	"movi v31.16b, #0"
	    : : : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16",
	    "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25",
	    "v26", "v27", "v28", "v29", "v30", "v31");
#endif
}

static void
guest_frame_fp(void)
{
	uint64_t first, last;

	vmm_guest_set_handler(TRAP_VECTOR, guest_fp_clobber_handler);
#if defined(__amd64__)
	__asm __volatile(
	    "movq %[in], %%xmm0\n\t"
	    "movq %[in], %%xmm15\n\t"
	    "int3\n\t"
	    "movq %%xmm0, %[first]\n\t"
	    "movq %%xmm15, %[last]"
	    : [first] "=&r" (first), [last] "=&r" (last)
	    : [in] "r" (EDIT_VALUE)
	    : "xmm0", "xmm15", "memory");
#elif defined(__aarch64__)
	__asm __volatile(
	    "fmov d0, %x[in]\n\t"
	    "fmov d31, %x[in]\n\t"
	    "svc #0\n\t"
	    "fmov %x[first], d0\n\t"
	    "fmov %x[last], d31"
	    : [first] "=&r" (first), [last] "=&r" (last)
	    : [in] "r" (EDIT_VALUE)
	    : "v0", "v31", "memory");
#endif
	GUEST_ASSERT_EQ(first, EDIT_VALUE);
	GUEST_ASSERT_EQ(last, EDIT_VALUE);
}

/* FP/SIMD registers survive a handler that clobbers them. */
ATF_TC_WITHOUT_HEAD(frame_fp);
ATF_TC_BODY(frame_fp, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_frame_fp, &vcpu);
	vmm_test_run_until_done(vcpu);
}

static uint64_t safe_value = 42;

static int
guest_load_safe(uint64_t addr, uint64_t *valp, uint64_t *errorp)
{
	uint64_t error, val;
	int vector;

	__asm __volatile(GUEST_ASM_SAFE(LOAD_INSN)
	    : GUEST_ASM_SAFE_OUTPUTS(vector, error), [val] "=r" (val)
	    : [addr] "r" (addr)
	    : GUEST_ASM_SAFE_CLOBBERS, "memory");
	*valp = val;
	*errorp = error;
	return (vector);
}

static int
guest_store_safe(uint64_t addr, uint64_t val, uint64_t *errorp)
{
	uint64_t error;
	int vector;

	__asm __volatile(GUEST_ASM_SAFE(STORE_INSN)
	    : GUEST_ASM_SAFE_OUTPUTS(vector, error)
	    : [addr] "r" (addr), [val] "r" (val)
	    : GUEST_ASM_SAFE_CLOBBERS, "memory");
	*errorp = error;
	return (vector);
}

static void
guest_safe(void)
{
	uint64_t error, val;
	int vector;

	vector = guest_load_safe((uintptr_t)&safe_value, &val, &error);
	GUEST_ASSERT_EQ(vector, GUEST_NO_EXCEPTION);
	GUEST_ASSERT_EQ(error, 0);
	GUEST_ASSERT_EQ(val, 42);

	/* EXCP_UNKNOWN on arm64 is 0, which must not read as "no exception". */
	__asm __volatile(GUEST_ASM_SAFE(UNDEF_INSN)
	    : GUEST_ASM_SAFE_OUTPUTS(vector, error)
	    :
	    : GUEST_ASM_SAFE_CLOBBERS);
	GUEST_ASSERT_EQ(vector, UNDEF_VECTOR);
	GUEST_ASSERT_EQ(error, UNDEF_ERROR);

#if defined(__amd64__)
	{
		uint32_t eax, edx;

		/* #DE is vector 0, which must not read as "no exception". */
		eax = 1;
		edx = 0;
		__asm __volatile(GUEST_ASM_SAFE("divl %[divisor]")
		    : GUEST_ASM_SAFE_OUTPUTS(vector, error), "+a" (eax),
		      "+d" (edx)
		    : [divisor] "r" (0U)
		    : GUEST_ASM_SAFE_CLOBBERS);
		GUEST_ASSERT_EQ(vector, IDT_DE);
		GUEST_ASSERT_EQ(error, 0);
	}

	vector = guest_load_safe(0x8000000000000000UL, &val, &error);
	GUEST_ASSERT_EQ(vector, IDT_GP);
	GUEST_ASSERT_EQ(error, 0);

	vector = guest_load_safe(VMM_TEST_UNMAPPED_VA, &val, &error);
	GUEST_ASSERT_EQ(vector, IDT_PF);
	GUEST_ASSERT_EQ(error, 0);

	vector = guest_store_safe(VMM_TEST_VA, 1, &error);
	GUEST_ASSERT_EQ(vector, IDT_PF);
	GUEST_ASSERT_EQ(error, PGEX_P | PGEX_W);
#elif defined(__aarch64__)
	vector = guest_load_safe(VMM_TEST_UNMAPPED_VA, &val, &error);
	GUEST_ASSERT_EQ(vector, EXCP_DATA_ABORT);
	GUEST_ASSERT_EQ(error & ISS_DATA_WnR, 0);
	GUEST_ASSERT(ESR_DFSC_TF(error));

	vector = guest_store_safe(VMM_TEST_VA, 1, &error);
	GUEST_ASSERT_EQ(vector, EXCP_DATA_ABORT);
	GUEST_ASSERT_EQ(error & ISS_DATA_WnR, ISS_DATA_WnR);
	GUEST_ASSERT_EQ(error & ISS_DATA_DFSC_MASK, ISS_DATA_DFSC_PF_L3);
#endif
	GUEST_ASSERT_EQ(*(volatile uint64_t *)VMM_TEST_VA, 0);
}

static void
map_read_only(struct vmm_test_vm *vm)
{

	vmm_test_map(vm, VMM_TEST_VA, vmm_test_alloc_pages(vm, 1), PAGE_SIZE,
	    VMM_TEST_PROT_READ);
}

/* GUEST_ASM_SAFE() reports the vector and error code of a fault. */
ATF_TC_WITHOUT_HEAD(safe);
ATF_TC_BODY(safe, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;

	vm = vmm_test_vm_create_with_vcpu(guest_safe, &vcpu);
	map_read_only(vm);
	vmm_test_run_until_done(vcpu);
}

static void
guest_safe_trap(void)
{
	uint64_t error;
	int vector;

	vmm_guest_set_handler(TRAP_VECTOR, guest_other_handler);
	__asm __volatile(GUEST_ASM_SAFE(TRAP_INSN)
	    : GUEST_ASM_SAFE_OUTPUTS(vector, error)
	    :
	    : GUEST_ASM_SAFE_CLOBBERS, "memory");
	GUEST_ASSERT_EQ(vector, GUEST_NO_EXCEPTION);
	GUEST_ASSERT_EQ(error, 0);
	GUEST_ASSERT_EQ(other_count, 1);
	GUEST_ASSERT_EQ(other_vector, TRAP_VECTOR);
}

/*
 * An exception reported after the instruction, rather than as a fault on
 * it, reaches its handler and not the safe-instruction fixup.
 */
ATF_TC_WITHOUT_HEAD(safe_trap);
ATF_TC_BODY(safe_trap, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_safe_trap, &vcpu);
	vmm_test_run_until_done(vcpu);
}

#if defined(__amd64__)
static void
guest_safe_msr(void)
{
	static const uint64_t values[] = {
		0, 0x123456789abcUL, 0xffff800012345678UL,
	};
	uint64_t original, val;
	u_int i;

	/* Guest code does not use %fs; exercise both halves of the MSR. */
	GUEST_ASSERT_EQ(vmm_guest_rdmsr_safe(MSR_FSBASE, &original),
	    GUEST_NO_EXCEPTION);
	for (i = 0; i < nitems(values); i++) {
		GUEST_ASSERT_EQ(vmm_guest_wrmsr_safe(MSR_FSBASE, values[i]),
		    GUEST_NO_EXCEPTION);
		GUEST_ASSERT_EQ(vmm_guest_rdmsr_safe(MSR_FSBASE, &val),
		    GUEST_NO_EXCEPTION);
		GUEST_ASSERT_EQ(val, values[i]);
	}
	GUEST_ASSERT_EQ(vmm_guest_wrmsr_safe(MSR_FSBASE, original),
	    GUEST_NO_EXCEPTION);
}

/* Successful safe MSR accesses preserve the full 64-bit value. */
ATF_TC_WITHOUT_HEAD(safe_msr);
ATF_TC_BODY(safe_msr, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_safe_msr, &vcpu);
	vmm_test_run_until_done(vcpu);
}

/* An MSR that vmm(4) leaves to user space, and that bhyve(8) would not know. */
#define	UNKNOWN_MSR	0x0badc0de

static void
guest_injected(void)
{
	uint64_t val;

	GUEST_ASSERT_EQ(vmm_guest_rdmsr_safe(UNKNOWN_MSR, &val), IDT_GP);
	GUEST_ASSERT_EQ(vmm_guest_wrmsr_safe(UNKNOWN_MSR, 1), IDT_GP);
}

static void
inject_msr_gp(struct vmm_test_vcpu *vcpu, enum vm_exitcode exitcode)
{
	struct vm_exit *vme;

	vme = vmm_test_run_expect_exit(vcpu, exitcode);
	ATF_REQUIRE_EQ(vme->u.msr.code, UNKNOWN_MSR);
	/* As bhyve(8) does for an MSR it does not emulate. */
	ATF_REQUIRE_MSG(vm_inject_exception(vcpu->vcpu, IDT_GP, 1, 0, 1) == 0,
	    "injecting #GP: %s", strerror(errno));
}

/* An exception injected by the host reaches the guest's handling. */
ATF_TC_WITHOUT_HEAD(injected);
ATF_TC_BODY(injected, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_injected, &vcpu);
	inject_msr_gp(vcpu, VM_EXITCODE_RDMSR);
	inject_msr_gp(vcpu, VM_EXITCODE_WRMSR);
	vmm_test_run_until_done(vcpu);
}
#elif defined(__aarch64__)
#define	INJECT_ESR	(((uint64_t)EXCP_DATA_ABORT << ESR_ELx_EC_SHIFT) | \
	ESR_ELx_IL | ISS_DATA_DFSC_TF_L3)
#define	INJECT_FAR	0x12345000UL

static uint64_t injected_esr, injected_far;

static void
guest_injected_handler(struct trapframe *tf, int vector)
{

	GUEST_ASSERT_EQ(vector, EXCP_DATA_ABORT);
	injected_esr = tf->tf_esr;
	injected_far = tf->tf_far;
	other_count++;
}

static void
guest_injected(void)
{

	vmm_guest_set_handler(EXCP_DATA_ABORT, guest_injected_handler);
	GUEST_SYNC(1);
	GUEST_ASSERT_EQ(other_count, 1);
	GUEST_ASSERT_EQ(injected_esr, INJECT_ESR);
	GUEST_ASSERT_EQ(injected_far, INJECT_FAR);
}

/*
 * An exception injected by the host reaches the guest's handler, with the
 * syndrome and fault address the host gave.
 */
ATF_TC_WITHOUT_HEAD(injected);
ATF_TC_BODY(injected, tc)
{
	struct vmm_test_vcpu *vcpu;

	(void)vmm_test_vm_create_with_vcpu(guest_injected, &vcpu);
	vmm_test_run_expect_ucall(vcpu, UCALL_SYNC, NULL);
	ATF_REQUIRE_MSG(vm_inject_exception(vcpu->vcpu, INJECT_ESR,
	    INJECT_FAR) == 0, "injecting a data abort: %s", strerror(errno));
	vmm_test_run_until_done(vcpu);
}
#endif

static void
guest_capture_retry(void)
{

	vmm_guest_capture_exception(FAULT_VECTOR);
	GUEST_ASSERT_EQ(*(volatile uint64_t *)VMM_TEST_VA, 0x1234);
	GUEST_ASSERT_EQ(vmm_guest_captured.count, 1);
}

/*
 * A captured fault stops the vCPU with its frame; after the host maps the
 * page, the access is retried and succeeds.
 */
ATF_TC_WITHOUT_HEAD(capture_retry);
ATF_TC_BODY(capture_retry, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct vmm_test_vm *vm;
	struct trapframe *tf;
	vm_paddr_t gpa;

	vm = vmm_test_vm_create_with_vcpu(guest_capture_retry, &vcpu);
	tf = vmm_test_run_expect_exception(vcpu, FAULT_VECTOR);
#if defined(__amd64__)
	ATF_CHECK_EQ(tf->tf_addr, VMM_TEST_VA);
	ATF_CHECK_EQ(tf->tf_err, 0);
#elif defined(__aarch64__)
	ATF_CHECK_EQ(tf->tf_far, VMM_TEST_VA);
	ATF_CHECK_EQ(ESR_ELx_EXCEPTION(tf->tf_esr), EXCP_DATA_ABORT);
	ATF_CHECK_EQ(tf->tf_esr & ISS_DATA_WnR, 0);
	ATF_CHECK(ESR_DFSC_TF(tf->tf_esr));
#endif
	ATF_CHECK_EQ(VMM_TEST_GUEST_VAR(vm, vmm_guest_captured)->count, 1);
	gpa = vmm_test_alloc_pages(vm, 1);
	*(uint64_t *)vmm_test_gpa2hva(vm, gpa, 8) = 0x1234;
	vmm_test_map(vm, VMM_TEST_VA, gpa, PAGE_SIZE, VMM_TEST_PROT_READ);
	vmm_test_run_until_done(vcpu);
}

static void
guest_capture_edit(void)
{
	register uint64_t reg __asm(EDIT_REG);

	vmm_guest_capture_exception(UNDEF_VECTOR);
	reg = 0;
	__asm __volatile(UNDEF_INSN : "+r" (reg) : : "memory");
	GUEST_ASSERT_EQ(reg, EDIT_VALUE);
}

/*
 * The host's changes to a captured frame take effect: here it skips the
 * faulting instruction and sets a register.
 */
ATF_TC_WITHOUT_HEAD(capture_edit);
ATF_TC_BODY(capture_edit, tc)
{
	struct vmm_test_vcpu *vcpu;
	struct trapframe *tf;

	(void)vmm_test_vm_create_with_vcpu(guest_capture_edit, &vcpu);
	tf = vmm_test_run_expect_exception(vcpu, UNDEF_VECTOR);
	FRAME_PC(tf) += UNDEF_LEN;
	FRAME_EDIT_REG(tf) = EDIT_VALUE;
	vmm_test_run_until_done(vcpu);
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

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, handler);
	ATF_TP_ADD_TC(tp, frame);
	ATF_TP_ADD_TC(tp, frame_fp);
	ATF_TP_ADD_TC(tp, safe);
	ATF_TP_ADD_TC(tp, safe_trap);
#if defined(__amd64__)
	ATF_TP_ADD_TC(tp, safe_msr);
#endif
	ATF_TP_ADD_TC(tp, injected);
	ATF_TP_ADD_TC(tp, capture_retry);
	ATF_TP_ADD_TC(tp, capture_edit);
	ATF_TP_ADD_TC(tp, capture_unexpected);
	return (atf_no_error());
}
