/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#ifndef _VMM_TEST_MACHDEP_H_
#define	_VMM_TEST_MACHDEP_H_

/*
 * The test image is mapped at its host addresses through TTBR0_EL1, and all
 * of guest-physical memory is mapped with 2MB blocks at VMM_TEST_DMAP_BASE
 * through TTBR1_EL1.  Page tables and stacks are allocated from guest-physical
 * memory and accessed through the direct map.
 */
#define	VMM_TEST_DMAP_BASE	0xffff000000000000UL
#define	VMM_TEST_PHYS_TO_DMAP(pa) (VMM_TEST_DMAP_BASE + (pa))

/* Private SMCCC vendor-hypervisor call, with the ucall pointer in x1. */
#define	VMM_TEST_UCALL_ID	0xc600ff00

/* Registers that start a function: program counter, stack, first argument. */
#define	VMM_TEST_REG_PC		VM_REG_GUEST_PC
#define	VMM_TEST_REG_SP		VM_REG_GUEST_SP
#define	VMM_TEST_REG_ARG0	VM_REG_GUEST_X0

struct vmm_test_vm_md {
	vm_paddr_t	ttbr0;
	vm_paddr_t	ttbr1;
	vm_paddr_t	boot;		/* identity-mapped bootstrap page */
};

/* Guest code referenced by the host-side VM and vCPU setup. */
extern const char vmm_guest_boot[], vmm_guest_boot_end[];
extern const char vmm_guest_vectors[];

#endif /* !_VMM_TEST_MACHDEP_H_ */
