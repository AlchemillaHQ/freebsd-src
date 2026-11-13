/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Guest exception and interrupt entry.  IDT vector n enters the 16-byte stub
 * at vmm_guest_trap_stubs + 16 * n, which pushes an error code if the CPU did
 * not, and the vector.  The common code completes a struct trapframe, saves
 * the FPU state and calls vmm_guest_trap().
 */

	.text
	.p2align 4
	.globl	vmm_guest_trap_stubs
	.type	vmm_guest_trap_stubs, @function
vmm_guest_trap_stubs:
	vector = 0
	.rept	256
	.org	vmm_guest_trap_stubs + vector * 16, 0xcc
	.if vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || \
	    vector == 21 || vector == 29 || vector == 30
	/* The CPU pushed an error code. */
	.else
	pushq	$0			/* tf_err */
	.endif
	pushq	$vector
	jmp	trap_common
	vector = vector + 1
	.endr
	.org	vmm_guest_trap_stubs + 256 * 16, 0xcc
	.size	vmm_guest_trap_stubs, . - vmm_guest_trap_stubs

	.p2align 4
	.type	trap_common, @function
trap_common:
	/* The vector is where tf_flags, tf_es and tf_ds go. */
	pushq	$0			/* tf_addr */
	pushq	8(%rsp)			/* tf_trapno, with tf_fs and tf_gs 0 */
	movq	$0, 16(%rsp)
	pushq	%r15
	pushq	%r14
	pushq	%r13
	pushq	%r12
	pushq	%r11
	pushq	%r10
	pushq	%rbp
	pushq	%rbx
	pushq	%rax
	pushq	%r9
	pushq	%r8
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	movq	%rsp, %rdi

	/*
	 * The CPU aligned the stack to 16 bytes before pushing the 48-byte
	 * hardware frame, so the 192-byte trapframe is aligned too.  Handlers
	 * are C and may use SSE, so save the interrupted code's FPU state.
	 */
	subq	$512, %rsp
	fxsave64 (%rsp)
	cld
	call	vmm_guest_trap
	fxrstor64 (%rsp)
	addq	$512, %rsp

	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%r8
	popq	%r9
	popq	%rax
	popq	%rbx
	popq	%rbp
	popq	%r10
	popq	%r11
	popq	%r12
	popq	%r13
	popq	%r14
	popq	%r15
	addq	$32, %rsp		/* tf_trapno to tf_err */
	iretq
	.size	trap_common, . - trap_common

	.section .note.GNU-stack,"",%progbits
