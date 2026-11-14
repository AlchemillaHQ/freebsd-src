/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#ifndef _VMM_TEST_GUEST_MACHDEP_H_
#define	_VMM_TEST_GUEST_MACHDEP_H_

#include <machine/armreg.h>
#include <machine/frame.h>

/*
 * Exceptions.  The VBAR_EL1 vectors save a struct trapframe.  Guest code runs
 * at EL1 on SP_EL1, so its exceptions arrive at the "current EL with SPx"
 * vectors.  Handlers for synchronous exceptions are numbered by exception
 * class (ESR_EL1.EC, EXCP_* in <machine/armreg.h>), and IRQ, FIQ and SError
 * have the numbers below.  An exception that guest code neither handles nor
 * expects with GUEST_ASM_SAFE() fails the test case with its vector, ESR_EL1,
 * ELR_EL1 and, where relevant, FAR_EL1.
 */
#define	VMM_GUEST_EXC_IRQ	64
#define	VMM_GUEST_EXC_FIQ	65
#define	VMM_GUEST_EXC_SERROR	66
#define	VMM_GUEST_NVECTORS	67

void	vmm_guest_trap(struct trapframe *, uint64_t);	/* called by vectors */

/* Flush the TLB entries for a page whose mapping the host changed. */
static __inline void
vmm_guest_flush_tlb(uint64_t gva)
{

	__asm __volatile(
	    "dsb ishst\n\t"
	    "tlbi vaae1is, %0\n\t"
	    "dsb ish\n\t"
	    "isb"
	    : : "r" ((gva >> 12) & ((1UL << 44) - 1)) : "memory");
}

/*
 * GUEST_ASM_SAFE() runs one instruction that may fault, in extended asm with
 * named operands:
 *
 *	__asm __volatile(GUEST_ASM_SAFE("ldr %x[val], [%x[addr]]")
 *	    : GUEST_ASM_SAFE_OUTPUTS(vector, error), [val] "=r" (val)
 *	    : [addr] "r" (addr)
 *	    : GUEST_ASM_SAFE_CLOBBERS, "memory");
 *
 * 'vector' (an int) is GUEST_NO_EXCEPTION if the instruction completes.  If
 * it takes a synchronous exception, execution continues after it with
 * 'vector' set to the exception class and 'error' (a uint64_t) to ESR_EL1,
 * and its outputs undefined.  The instruction must not use x9, x10, x11 or
 * the local labels 1 and 2.
 */
#define	VMM_GUEST_SAFE_MAGIC	0x5afe5afe5afe5afeUL
#define	GUEST_NO_EXCEPTION	(-1)

#define	GUEST_ASM_SAFE(insn)						\
	"movz x9, #0x5afe, lsl #48\n\t"					\
	"movk x9, #0x5afe, lsl #32\n\t"					\
	"movk x9, #0x5afe, lsl #16\n\t"					\
	"movk x9, #0x5afe\n\t"						\
	"adr x10, 1f\n\t"						\
	"adr x11, 2f\n"							\
	"1:\t" insn "\n\t"						\
	"mov x9, #-1\n\t"						\
	"mov x10, xzr\n"						\
	"2:\tmov %x[_safe_vector], x9\n\t"				\
	"mov %x[_safe_error], x10"
#define	GUEST_ASM_SAFE_OUTPUTS(vector, error)				\
	[_safe_vector] "=r" (vector), [_safe_error] "=r" (error)
#define	GUEST_ASM_SAFE_CLOBBERS	"x9", "x10", "x11"

#endif /* !_VMM_TEST_GUEST_MACHDEP_H_ */
