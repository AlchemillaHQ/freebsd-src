/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Guest exception vectors.  Each of the 16 VBAR_EL1 entries builds a struct
 * trapframe on the current stack, followed by the FP/SIMD state, and calls
 * vmm_guest_trap(frame, vector), where 'vector' is the entry's offset divided
 * by 0x80.  The general-purpose registers, ELR_EL1 and SPSR_EL1 are restored
 * from the frame on return; SP is not.
 */

#include <machine/asm.h>

/* struct trapframe, <machine/frame.h> */
#define	TF_SP		0
#define	TF_LR		8
#define	TF_ELR		16
#define	TF_ESR		32
#define	TF_X(n)		(48 + (n) * 8)
#define	TF_SIZE		288

/* q0-q31, then FPCR and FPSR */
#define	FP_FPCR		512
#define	FP_FPSR		520
#define	FP_SIZE		528

#define	FRAME_SIZE	(TF_SIZE + FP_SIZE)

	.text
	.balign	2048
	.globl	vmm_guest_vectors
	.type	vmm_guest_vectors, %function
vmm_guest_vectors:
	vector = 0
	.rept	16
	.balign	128
	sub	sp, sp, #FRAME_SIZE
	stp	x0, x1, [sp, #TF_X(0)]
	mov	x1, #vector
	b	trap_common
	vector = vector + 1
	.endr
	.size	vmm_guest_vectors, . - vmm_guest_vectors

	.type	trap_common, %function
trap_common:
	stp	x2, x3, [sp, #TF_X(2)]
	stp	x4, x5, [sp, #TF_X(4)]
	stp	x6, x7, [sp, #TF_X(6)]
	stp	x8, x9, [sp, #TF_X(8)]
	stp	x10, x11, [sp, #TF_X(10)]
	stp	x12, x13, [sp, #TF_X(12)]
	stp	x14, x15, [sp, #TF_X(14)]
	stp	x16, x17, [sp, #TF_X(16)]
	stp	x18, x19, [sp, #TF_X(18)]
	stp	x20, x21, [sp, #TF_X(20)]
	stp	x22, x23, [sp, #TF_X(22)]
	stp	x24, x25, [sp, #TF_X(24)]
	stp	x26, x27, [sp, #TF_X(26)]
	stp	x28, x29, [sp, #TF_X(28)]
	str	x30, [sp, #TF_LR]
	add	x2, sp, #FRAME_SIZE		/* the interrupted SP */
	str	x2, [sp, #TF_SP]
	mrs	x2, elr_el1
	mrs	x3, spsr_el1
	stp	x2, x3, [sp, #TF_ELR]		/* tf_elr, tf_spsr */
	mrs	x2, esr_el1
	mrs	x3, far_el1
	stp	x2, x3, [sp, #TF_ESR]		/* tf_esr, tf_far */

	/* Handlers are C and may use FP/SIMD registers. */
	add	x2, sp, #TF_SIZE
	stp	q0, q1, [x2, #(0 * 32)]
	stp	q2, q3, [x2, #(1 * 32)]
	stp	q4, q5, [x2, #(2 * 32)]
	stp	q6, q7, [x2, #(3 * 32)]
	stp	q8, q9, [x2, #(4 * 32)]
	stp	q10, q11, [x2, #(5 * 32)]
	stp	q12, q13, [x2, #(6 * 32)]
	stp	q14, q15, [x2, #(7 * 32)]
	stp	q16, q17, [x2, #(8 * 32)]
	stp	q18, q19, [x2, #(9 * 32)]
	stp	q20, q21, [x2, #(10 * 32)]
	stp	q22, q23, [x2, #(11 * 32)]
	stp	q24, q25, [x2, #(12 * 32)]
	stp	q26, q27, [x2, #(13 * 32)]
	stp	q28, q29, [x2, #(14 * 32)]
	stp	q30, q31, [x2, #(15 * 32)]
	mrs	x3, fpcr
	str	x3, [x2, #FP_FPCR]
	mrs	x3, fpsr
	str	x3, [x2, #FP_FPSR]

	mov	x0, sp
	bl	vmm_guest_trap

	add	x2, sp, #TF_SIZE
	ldr	x3, [x2, #FP_FPCR]
	msr	fpcr, x3
	ldr	x3, [x2, #FP_FPSR]
	msr	fpsr, x3
	ldp	q0, q1, [x2, #(0 * 32)]
	ldp	q2, q3, [x2, #(1 * 32)]
	ldp	q4, q5, [x2, #(2 * 32)]
	ldp	q6, q7, [x2, #(3 * 32)]
	ldp	q8, q9, [x2, #(4 * 32)]
	ldp	q10, q11, [x2, #(5 * 32)]
	ldp	q12, q13, [x2, #(6 * 32)]
	ldp	q14, q15, [x2, #(7 * 32)]
	ldp	q16, q17, [x2, #(8 * 32)]
	ldp	q18, q19, [x2, #(9 * 32)]
	ldp	q20, q21, [x2, #(10 * 32)]
	ldp	q22, q23, [x2, #(11 * 32)]
	ldp	q24, q25, [x2, #(12 * 32)]
	ldp	q26, q27, [x2, #(13 * 32)]
	ldp	q28, q29, [x2, #(14 * 32)]
	ldp	q30, q31, [x2, #(15 * 32)]

	ldp	x2, x3, [sp, #TF_ELR]
	msr	elr_el1, x2
	msr	spsr_el1, x3
	ldr	x30, [sp, #TF_LR]
	ldp	x0, x1, [sp, #TF_X(0)]
	ldp	x2, x3, [sp, #TF_X(2)]
	ldp	x4, x5, [sp, #TF_X(4)]
	ldp	x6, x7, [sp, #TF_X(6)]
	ldp	x8, x9, [sp, #TF_X(8)]
	ldp	x10, x11, [sp, #TF_X(10)]
	ldp	x12, x13, [sp, #TF_X(12)]
	ldp	x14, x15, [sp, #TF_X(14)]
	ldp	x16, x17, [sp, #TF_X(16)]
	ldp	x18, x19, [sp, #TF_X(18)]
	ldp	x20, x21, [sp, #TF_X(20)]
	ldp	x22, x23, [sp, #TF_X(22)]
	ldp	x24, x25, [sp, #TF_X(24)]
	ldp	x26, x27, [sp, #TF_X(26)]
	ldp	x28, x29, [sp, #TF_X(28)]
	add	sp, sp, #FRAME_SIZE
	eret
	.size	trap_common, . - trap_common

	.section .note.GNU-stack,"",%progbits
