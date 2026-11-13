/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Machine-independent guest-side support. */

#include <sys/param.h>

#include "vmm_test_guest.h"

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
