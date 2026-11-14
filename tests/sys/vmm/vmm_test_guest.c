/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Machine-independent guest-side support. */

#include <sys/param.h>

#include "vmm_test_guest.h"

static vmm_guest_handler_t *vmm_guest_handlers[VMM_GUEST_NVECTORS];

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

void
vmm_guest_set_handler(int vector, vmm_guest_handler_t *handler)
{

	GUEST_ASSERT(vector >= 0 && vector < VMM_GUEST_NVECTORS);
	vmm_guest_handlers[vector] = handler;
}

bool
vmm_guest_dispatch(struct trapframe *tf, int vector)
{
	vmm_guest_handler_t *handler;

	if (vector < 0 || vector >= VMM_GUEST_NVECTORS)
		return (false);
	handler = vmm_guest_handlers[vector];
	if (handler == NULL)
		return (false);
	handler(tf, vector);
	return (true);
}

static void
vmm_guest_capture_handler(struct trapframe *tf, int vector)
{

	vmm_guest_captured.frame = *tf;
	vmm_guest_captured.vector = vector;
	vmm_guest_captured.count++;
	_GUEST_UCALL(.cmd = UCALL_CAPTURE, .args = { vector,
	    vmm_guest_captured.count });
	*tf = vmm_guest_captured.frame;
	vmm_guest_md_capture_return(tf, vector);
}

void
vmm_guest_capture_exception(int vector)
{

	vmm_guest_set_handler(vector, vmm_guest_capture_handler);
}
