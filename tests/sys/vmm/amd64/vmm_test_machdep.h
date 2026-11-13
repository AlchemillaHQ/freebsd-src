/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#ifndef _VMM_TEST_MACHDEP_H_
#define	_VMM_TEST_MACHDEP_H_

/*
 * Guest memory layout.  The test image is mapped at its host addresses in the
 * lower half of the address space, and all of guest-physical memory is
 * mapped with 2MB pages at VMM_TEST_DMAP_BASE in the upper half, so the two
 * never collide.  Page tables, stacks and descriptor tables are allocated
 * from guest-physical memory and accessed through the direct map.
 */
#define	VMM_TEST_DMAP_BASE	0xffff800000000000UL
#define	VMM_TEST_PHYS_TO_DMAP(pa) (VMM_TEST_DMAP_BASE + (pa))

/* I/O port used for ucalls. */
#define	VMM_TEST_UCALL_PORT	0xef00

/* Registers that start a function: program counter, stack, first argument. */
#define	VMM_TEST_REG_PC		VM_REG_GUEST_RIP
#define	VMM_TEST_REG_SP		VM_REG_GUEST_RSP
#define	VMM_TEST_REG_ARG0	VM_REG_GUEST_RDI

struct vmm_test_vm_md {
	vm_paddr_t	pml4;		/* root of the guest page tables */
	vm_paddr_t	idt;		/* shared by all vCPUs */
};

/* IDT entry points, referenced by the host-side VM setup. */
extern const char vmm_guest_trap_stubs[];	/* 16 bytes per vector */

#endif /* !_VMM_TEST_MACHDEP_H_ */
