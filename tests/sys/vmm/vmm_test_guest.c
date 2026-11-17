/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@freebsd.org>
 */

/* Machine-independent guest-side support. */

#include <sys/param.h>

#include "vmm_test_guest.h"

static bool vmm_guest_capturing[VMM_GUEST_NVECTORS];

/* The host finds this through its address in the test binary. */
struct vmm_guest_capture vmm_guest_captured;

/*
 * Entry point of every vCPU, called by the machine-dependent startup code
 * with the test's guest function.
 */
void
vmm_guest_start(vmm_test_guest_fn_t *fn)
{

	fn();
	GUEST_DONE();
}

bool
vmm_guest_dispatch(struct trapframe *tf, int vector)
{

	if (vector < 0 || vector >= VMM_GUEST_NVECTORS ||
	    !vmm_guest_capturing[vector])
		return (false);
	vmm_guest_captured.frame = *tf;
	vmm_guest_captured.vector = vector;
	vmm_guest_captured.count++;
	_GUEST_UCALL(.cmd = UCALL_CAPTURE, .args = { vector,
	    vmm_guest_captured.count });
	*tf = vmm_guest_captured.frame;
	return (true);
}

void
vmm_guest_capture_exception(int vector)
{

	if (vector < 0 || vector >= VMM_GUEST_NVECTORS)
		GUEST_FAIL("capturing an exception vector out of range");
	vmm_guest_capturing[vector] = true;
}
