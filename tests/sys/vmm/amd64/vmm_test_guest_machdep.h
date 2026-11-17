/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@freebsd.org>
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
 * Exceptions and interrupts.  Every IDT vector has an entry point.  An
 * exception or interrupt fails the test case with its vector, error code and
 * %rip, and for a page fault the faulting address, which the frame holds in
 * tf_addr.
 */
void	vmm_guest_trap(struct trapframe *);	/* called by the IDT stubs */

#endif /* !_VMM_TEST_GUEST_MACHDEP_H_ */
