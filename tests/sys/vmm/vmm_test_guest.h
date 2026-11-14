/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Guest-side interface of the vmm(4) test harness.  See vmm_test.h for an
 * overview, including the rules guest code must follow.
 */

#ifndef _VMM_TEST_GUEST_H_
#define	_VMM_TEST_GUEST_H_

#include "vmm_test.h"
#include "vmm_test_guest_machdep.h"

void	vmm_guest_ucall(struct vmm_ucall *);
void	vmm_guest_halt(void) __dead2;

#define	_GUEST_UCALL(...)						\
	vmm_guest_ucall(&(struct vmm_ucall){ .file = __FILE__,		\
	    .line = __LINE__, __VA_ARGS__ })

/* Stop and let the host inspect state; the guest resumes on the next run. */
#define	GUEST_SYNC(stage)						\
	_GUEST_UCALL(.cmd = UCALL_SYNC, .args = { (stage) })

/* Report success.  Returning from the guest function does the same. */
#define	GUEST_DONE() do {						\
	_GUEST_UCALL(.cmd = UCALL_DONE);				\
	vmm_guest_halt();						\
} while (0)

/* Fail the test case. */
#define	GUEST_FAIL(m) do {						\
	_GUEST_UCALL(.cmd = UCALL_ABORT, .msg = (m));			\
	vmm_guest_halt();						\
} while (0)

#define	GUEST_ASSERT(cond) do {						\
	if (!(cond))							\
		GUEST_FAIL(#cond);					\
} while (0)

#define	GUEST_ASSERT_EQ(a, b) do {					\
	uint64_t _ga = (a), _gb = (b);					\
									\
	if (_ga != _gb) {						\
		_GUEST_UCALL(.cmd = UCALL_ABORT, .msg = #a " == " #b,	\
		    .args = { _ga, _gb, 1 });				\
		vmm_guest_halt();					\
	}								\
} while (0)

/*
 * Exception handlers, installed for the whole VM.  Vectors are numbered as
 * described in vmm_test_guest_machdep.h.  A handler runs on the interrupted
 * code's stack with interrupts disabled, and may change the frame, for
 * example to resume after the faulting instruction; the registers and the
 * program counter are restored from the frame when it returns, except the
 * stack pointer on arm64.
 */
typedef void vmm_guest_handler_t(struct trapframe *, int);

void	vmm_guest_set_handler(int, vmm_guest_handler_t *);

/*
 * Capture exceptions with the given vector: record the frame in
 * vmm_guest_captured, stop with a CAPTURE ucall, and when the host runs the
 * vCPU again, return with the recorded frame, which the host may have
 * changed.  Before returning, the guest flushes the TLB entry for a faulting
 * address, so the host may fix a mapping and let the instruction retry.  See
 * vmm_test_run_expect_exception().
 */
struct vmm_guest_capture {
	struct trapframe frame;
	uint64_t	vector;
	uint64_t	count;		/* exceptions captured so far */
};

extern struct vmm_guest_capture vmm_guest_captured;

void	vmm_guest_capture_exception(int);

/* Called by the machine-dependent trap code; true if a handler ran. */
bool	vmm_guest_dispatch(struct trapframe *, int);

/* Machine-dependent: prepare to return from a captured exception. */
void	vmm_guest_md_capture_return(struct trapframe *, int);

#endif /* !_VMM_TEST_GUEST_H_ */
