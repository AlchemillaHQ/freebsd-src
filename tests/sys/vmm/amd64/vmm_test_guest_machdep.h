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
 * Every IDT vector has an entry point.  An exception or interrupt fails the
 * test case with its vector, error code and %rip, and for a page fault the
 * faulting address.  Vector numbers (IDT_UD, IDT_GP, IDT_PF, ...) are in
 * <machine/segments.h>.
 */
void	vmm_guest_trap(struct trapframe *);	/* called by the IDT stubs */

/* Flush the TLB entry for a page whose mapping the host changed. */
static __inline void
vmm_guest_flush_tlb(uint64_t gva)
{

	invlpg(gva);
}

#endif /* !_VMM_TEST_GUEST_MACHDEP_H_ */
