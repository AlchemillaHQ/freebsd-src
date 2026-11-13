/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Guest-side arm64 support. */

#include <sys/param.h>

#include <string.h>

#include "vmm_test_guest.h"

/*
 * Keep compiler-generated copies in the test image, as on amd64.  Volatile
 * stores prevent the compiler from turning these loops into libc calls.
 */
__hidden void *
memset(void *dst, int c, size_t len)
{
	volatile unsigned char *d;

	d = dst;
	while (len-- != 0)
		*d++ = c;
	return (dst);
}

__hidden void *
memcpy(void * __restrict dst, const void * __restrict src, size_t len)
{
	volatile unsigned char *d;
	const unsigned char *s;

	d = dst;
	s = src;
	while (len-- != 0)
		*d++ = *s++;
	return (dst);
}

_Static_assert(sizeof(struct trapframe) == 288,
    "vmm_test_exception.S does not match struct trapframe");

void
vmm_guest_trap(struct trapframe *tf, uint64_t vector)
{

	_GUEST_UCALL(.cmd = UCALL_EXCEPTION, .args = { vector, tf->tf_esr,
	    tf->tf_elr, tf->tf_far });
	vmm_guest_halt();
}

void
vmm_guest_ucall(struct vmm_ucall *uc)
{
	register uint64_t x0 __asm("x0") = VMM_TEST_UCALL_ID;
	register struct vmm_ucall *x1 __asm("x1") = uc;

	__asm __volatile("hvc #0" : "+r" (x0), "+r" (x1) : : "memory");
}

/* Breakpoints exit to the host, which does not resume a halted guest. */
void
vmm_guest_halt(void)
{

	for (;;)
		__asm __volatile("brk #0");
}
