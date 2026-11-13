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
 * Exception vectors, numbered by their offset in the VBAR_EL1 table divided
 * by 0x80.  Guest code runs at EL1 on SP_EL1, so its exceptions arrive at
 * the "current EL with SPx" vectors.
 */
#define	VMM_GUEST_VEC_SYNC	4
#define	VMM_GUEST_VEC_IRQ	5
#define	VMM_GUEST_VEC_FIQ	6
#define	VMM_GUEST_VEC_SERROR	7

/*
 * An exception fails the test case with its vector, ESR_EL1, ELR_EL1 and,
 * where relevant, FAR_EL1.
 */
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

#endif /* !_VMM_TEST_GUEST_MACHDEP_H_ */
