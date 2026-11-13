/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#include <machine/asm.h>
#include <machine/armreg.h>

/*
 * SCTLR_EL1: the MMU and caches enabled, little-endian, no alignment
 * checking, and the other fields at their ARMv8.0 defaults.
 */
#define	BOOT_SCTLR	(SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I)

/*
 * Copied to an identity-mapped page and entered with the MMU off.
 * x0: test function, x1: virtual entry point, x2: stack, x3: vectors.
 * The host has already set TTBR0_EL1, TTBR1_EL1 and TCR_EL1.
 */
ENTRY(vmm_guest_boot)
	/* Index 1 remains zero: Device-nGnRnE for device mappings. */
	mov	x4, #0xff		/* MAIR index 0: normal, write-back */
	msr	mair_el1, x4
	mov	x4, #CPACR_FPEN_TRAP_NONE
	msr	cpacr_el1, x4
	dsb	sy
	/* Not a literal load: the literal pool would be outside the page. */
	movz	x4, #(BOOT_SCTLR & 0xffff)
	movk	x4, #(BOOT_SCTLR >> 16), lsl #16
	msr	sctlr_el1, x4
	isb
	msr	vbar_el1, x3
	isb
	mov	sp, x2
	br	x1
END(vmm_guest_boot)

	.globl	vmm_guest_boot_end
vmm_guest_boot_end:

	.section .note.GNU-stack,"",%progbits
