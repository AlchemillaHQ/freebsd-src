/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@freebsd.org>
 */

#ifndef _VMM_TEST_GUEST_MACHDEP_H_
#define	_VMM_TEST_GUEST_MACHDEP_H_

#include <machine/armreg.h>
#include <machine/frame.h>

/*
 * Exceptions.  The VBAR_EL1 vectors save a struct trapframe.  Guest code runs
 * at EL1 on SP_EL1, so its exceptions arrive at the "current EL with SPx"
 * vectors.  An exception fails the test case with its vector, ESR_EL1,
 * ELR_EL1 and, where relevant, FAR_EL1.
 */
void	vmm_guest_trap(struct trapframe *, uint64_t);	/* called by vectors */

#endif /* !_VMM_TEST_GUEST_MACHDEP_H_ */
