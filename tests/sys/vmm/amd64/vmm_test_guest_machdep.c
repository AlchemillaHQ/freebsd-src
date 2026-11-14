/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/* Guest-side amd64 support. */

#include <sys/param.h>

#include <string.h>

#include "vmm_test_guest.h"

/*
 * The compiler may implement structure initialization and copies with calls
 * to memset() and memcpy(), which guest code cannot make into libc.  Define
 * them in the test binary instead.  They are hidden so that shared libraries
 * keep using libc's versions; code in the test binary, host or guest, uses
 * these.
 */
__hidden void *
memset(void *dst, int c, size_t len)
{
	void *p;

	p = dst;
	__asm __volatile("rep stosb"
	    : "+D" (p), "+c" (len) : "a" (c) : "memory");
	return (dst);
}

__hidden void *
memcpy(void * __restrict dst, const void * __restrict src, size_t len)
{
	void *p;

	p = dst;
	__asm __volatile("rep movsb"
	    : "+D" (p), "+S" (src), "+c" (len) : : "memory");
	return (dst);
}

_Static_assert(sizeof(struct trapframe) == 192,
    "vmm_test_exception.S does not match struct trapframe");

void
vmm_guest_trap(struct trapframe *tf)
{

	if (tf->tf_trapno == IDT_PF)
		tf->tf_addr = rcr2();

	/* A fault on the instruction in GUEST_ASM_SAFE(). */
	if ((uint64_t)tf->tf_r9 == VMM_GUEST_SAFE_MAGIC &&
	    tf->tf_rip == tf->tf_r10 && tf->tf_trapno < NRSVIDT &&
	    tf->tf_trapno != IDT_NMI) {
		tf->tf_r9 = tf->tf_trapno;
		tf->tf_r10 = tf->tf_err;
		tf->tf_rip = tf->tf_r11;
		return;
	}

	if (vmm_guest_dispatch(tf, tf->tf_trapno))
		return;
	_GUEST_UCALL(.cmd = UCALL_EXCEPTION, .args = { tf->tf_trapno,
	    tf->tf_err, tf->tf_rip, tf->tf_addr });
	vmm_guest_halt();
}

/* Let the instruction see a mapping the host fixed. */
void
vmm_guest_md_capture_return(struct trapframe *tf, int vector)
{

	if (vector == IDT_PF)
		vmm_guest_flush_tlb(tf->tf_addr);
}

/* A ucall is a 4-byte OUT to the ucall port with %rdi pointing at it. */
void
vmm_guest_ucall(struct vmm_ucall *uc)
{

	__asm __volatile("outl %%eax, %%dx"
	    : : "a" (0), "d" (VMM_TEST_UCALL_PORT), "D" (uc) : "memory");
}

void
vmm_guest_halt(void)
{

	for (;;)
		__asm __volatile("cli; hlt");
}
