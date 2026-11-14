/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#ifndef _VMM_TEST_GUEST_MACHDEP_H_
#define	_VMM_TEST_GUEST_MACHDEP_H_

/*
 * Guest code may use the inline instruction wrappers from these headers
 * (inb(), outb(), rdmsr(), wrmsr(), do_cpuid(), ...).
 */
#include <machine/cpufunc.h>
#include <machine/frame.h>
#include <machine/segments.h>
#include <machine/specialreg.h>

/*
 * Exceptions and interrupts.  Every IDT vector has an entry point, and
 * handlers are numbered by IDT vector (IDT_UD, IDT_GP, IDT_PF, ... in
 * <machine/segments.h>).  An exception or interrupt that guest code neither
 * handles nor expects with GUEST_ASM_SAFE() fails the test case with its
 * vector, error code and %rip, and for a page fault the faulting address,
 * which the frame holds in tf_addr.
 */
#define	VMM_GUEST_NVECTORS	NIDT

void	vmm_guest_trap(struct trapframe *);	/* called by the IDT stubs */

/* Flush the TLB entry for a page whose mapping the host changed. */
static __inline void
vmm_guest_flush_tlb(uint64_t gva)
{

	invlpg(gva);
}

/*
 * GUEST_ASM_SAFE() runs one instruction that may fault, in extended asm with
 * named operands:
 *
 *	__asm __volatile(GUEST_ASM_SAFE("rdmsr")
 *	    : GUEST_ASM_SAFE_OUTPUTS(vector, error), "=a" (lo), "=d" (hi)
 *	    : "c" (msr)
 *	    : GUEST_ASM_SAFE_CLOBBERS);
 *
 * 'vector' (an int) is GUEST_NO_EXCEPTION if the instruction completes.  If
 * it faults, execution continues after it with 'vector' and 'error' (a
 * uint64_t) set to the exception's vector and error code, and its outputs
 * undefined.  Exceptions that are not faults on the instruction itself, such
 * as #BP from int3, go to the handler for the vector.  The instruction must
 * not use %r9, %r10, %r11 or the local labels 1 and 2.
 */
#define	VMM_GUEST_SAFE_MAGIC	0x5afe5afe5afe5afe
#define	GUEST_NO_EXCEPTION	(-1)

#define	GUEST_ASM_SAFE(insn)						\
	"movabsq $" __XSTRING(VMM_GUEST_SAFE_MAGIC) ", %%r9\n\t"	\
	"leaq 1f(%%rip), %%r10\n\t"					\
	"leaq 2f(%%rip), %%r11\n"					\
	"1:\t" insn "\n\t"						\
	"movq $-1, %%r9\n\t"						\
	"xorl %%r10d, %%r10d\n"						\
	"2:\tmovq %%r9, %q[_safe_vector]\n\t"				\
	"movq %%r10, %q[_safe_error]"
#define	GUEST_ASM_SAFE_OUTPUTS(vector, error)				\
	[_safe_vector] "=r" (vector), [_safe_error] "=r" (error)
#define	GUEST_ASM_SAFE_CLOBBERS	"r9", "r10", "r11"

/* Return the vector of the exception raised, or GUEST_NO_EXCEPTION. */
static __inline int
vmm_guest_rdmsr_safe(u_int msr, uint64_t *val)
{
	uint64_t error;
	uint32_t hi, lo;
	int vector;

	__asm __volatile(GUEST_ASM_SAFE("rdmsr")
	    : GUEST_ASM_SAFE_OUTPUTS(vector, error), "=a" (lo), "=d" (hi)
	    : "c" (msr)
	    : GUEST_ASM_SAFE_CLOBBERS);
	(void)error;
	*val = vector == GUEST_NO_EXCEPTION ? (uint64_t)hi << 32 | lo : 0;
	return (vector);
}

static __inline int
vmm_guest_wrmsr_safe(u_int msr, uint64_t val)
{
	uint64_t error;
	int vector;

	__asm __volatile(GUEST_ASM_SAFE("wrmsr")
	    : GUEST_ASM_SAFE_OUTPUTS(vector, error)
	    : "c" (msr), "a" ((uint32_t)val), "d" ((uint32_t)(val >> 32))
	    : GUEST_ASM_SAFE_CLOBBERS, "memory");
	(void)error;
	return (vector);
}

#endif /* !_VMM_TEST_GUEST_MACHDEP_H_ */
