/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Support for the tests of the harness itself. */

#ifndef _HARNESS_UTIL_H_
#define	_HARNESS_UTIL_H_

#include "vmm_test.h"

typedef void harness_host_fn_t(struct vmm_test_vcpu *);

/*
 * In a child process, create a VM with one vCPU running 'guest', then call
 * 'host' with the vCPU, or run the guest to completion if 'host' is NULL.
 * Expect the harness to fail the test case, and check that its message
 * contains each string in the NULL-terminated array 'expected'.
 */
void	harness_expect_failure(vmm_test_guest_fn_t *guest,
	    harness_host_fn_t *host, const char *const *expected);

#endif /* !_HARNESS_UTIL_H_ */
