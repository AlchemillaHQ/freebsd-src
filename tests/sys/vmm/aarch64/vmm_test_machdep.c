/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Host-side arm64 support: stage-1 translation tables, the bootstrap that
 * enables the MMU, and the initial vCPU state at EL1.
 */

#include <sys/param.h>
#include <sys/cpuset.h>

#include <machine/armreg.h>
#include <machine/pte.h>
#include <machine/vmm.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <vmmapi.h>

#include "vmm_test.h"
#include "vmm_test_guest.h"

#define	STACK_PAGES	4

static const char *const exitcode_names[] = {
	[VM_EXITCODE_BOGUS] = "BOGUS",
	[VM_EXITCODE_INST_EMUL] = "INST_EMUL",
	[VM_EXITCODE_REG_EMUL] = "REG_EMUL",
	[VM_EXITCODE_HVC] = "HVC",
	[VM_EXITCODE_SUSPENDED] = "SUSPENDED",
	[VM_EXITCODE_HYP] = "HYP",
	[VM_EXITCODE_WFI] = "WFI",
	[VM_EXITCODE_PAGING] = "PAGING",
	[VM_EXITCODE_SMCCC] = "SMCCC",
	[VM_EXITCODE_DEBUG] = "DEBUG",
	[VM_EXITCODE_BRK] = "BRK",
	[VM_EXITCODE_SS] = "SS",
};

/* Names of exception classes (ESR_EL1.EC) seen at EL1. */
static const char *const exception_names[] = {
	[EXCP_UNKNOWN] = "undefined instruction",
	[EXCP_TRAP_WFI_WFE] = "WFI/WFE trap",
	[EXCP_FP_SIMD] = "FP/SIMD access trap",
	[EXCP_BTI] = "branch target exception",
	[EXCP_ILL_STATE] = "illegal execution state",
	[EXCP_SVC64] = "SVC",
	[EXCP_HVC] = "HVC",
	[EXCP_MSR] = "MSR/MRS trap",
	[EXCP_SVE] = "SVE access trap",
	[EXCP_FPAC] = "pointer authentication failure",
	[EXCP_INSN_ABORT_L] = "instruction abort",
	[EXCP_INSN_ABORT] = "instruction abort",
	[EXCP_PC_ALIGN] = "PC alignment fault",
	[EXCP_DATA_ABORT_L] = "data abort",
	[EXCP_DATA_ABORT] = "data abort",
	[EXCP_SP_ALIGN] = "SP alignment fault",
	[EXCP_TRAP_FP] = "FP exception",
	[EXCP_SERROR] = "SError",
	[EXCP_BRKPT_EL1] = "hardware breakpoint",
	[EXCP_SOFTSTP_EL1] = "software step",
	[EXCP_WATCHPT_EL1] = "watchpoint",
	[EXCP_BRK] = "BRK",
};

/* Vector table groups, in VBAR_EL1 order. */
static const char *const vector_origins[] = {
	"current EL with SP0",
	"current EL with SPx",
	"lower EL using AArch64",
	"lower EL using AArch32",
};
static const char *const vector_types[] = {
	"synchronous", "IRQ", "FIQ", "SError",
};

/*
 * Return an entry in the four-level, 48-bit stage-1 tables: level 3 for a
 * 4KB page, 2 for a 2MB block.  With 'alloc', allocate missing tables;
 * otherwise return NULL if one is missing.  Also return NULL, or fail with
 * 'alloc', if a block maps 'gva' above 'level'.
 */
static uint64_t *
vmm_test_pte(struct vmm_test_vm *vm, uint64_t gva, int level, bool alloc)
{
	uint64_t *table, *pte;
	vm_paddr_t pa;
	int lvl, shift;

	if (gva >= (1UL << 48) && gva < VMM_TEST_DMAP_BASE)
		vmm_test_fail("%#jx is not a 48-bit address", (uintmax_t)gva);
	pa = gva < VMM_TEST_DMAP_BASE ? vm->md.ttbr0 : vm->md.ttbr1;
	for (lvl = 0;; lvl++) {
		shift = L0_SHIFT - lvl * Ln_ENTRIES_SHIFT;
		table = vmm_test_gpa2hva(vm, pa, PAGE_SIZE);
		pte = &table[(gva >> shift) &
		    (lvl == 0 ? L0_ADDR_MASK : Ln_ADDR_MASK)];
		if (lvl == level)
			return (pte);
		if ((*pte & ATTR_DESCR_VALID) == 0) {
			if (!alloc)
				return (NULL);
			*pte = vmm_test_alloc_pages(vm, 1) | L0_TABLE;
		}
		if ((*pte & ATTR_DESCR_MASK) != L0_TABLE) {
			if (!alloc)
				return (NULL);
			vmm_test_fail("%#jx is already mapped by a block",
			    (uintmax_t)gva);
		}
		pa = *pte & ATTR_ADDR;
	}
}

/*
 * Descriptor bits for vmm_test_map() flags.  An inaccessible page keeps its
 * address in an invalid descriptor, so that it can be made accessible again.
 */
static uint64_t
vmm_test_pte_bits(int flags)
{
	uint64_t bits;

	bits = ATTR_AF | ATTR_SH(ATTR_SH_IS) |
	    ATTR_S1_IDX((flags & VMM_TEST_MAP_DEVICE) != 0 ? 1 : 0) |
	    ATTR_S1_AP((flags & VMM_TEST_PROT_WRITE) != 0 ?
	    ATTR_S1_AP_RW : ATTR_S1_AP_RO);
	if ((flags & VMM_TEST_PROT_EXEC) == 0)
		bits |= ATTR_S1_XN;
	if ((flags & VMM_TEST_PROT_READ) != 0)
		bits |= L3_PAGE;
	else
		bits |= L3_PAGE & ~ATTR_DESCR_VALID;
	return (bits);
}

/* Return the 4KB page entry for 'gva', which must already have one. */
static uint64_t *
vmm_test_mapped_pte(struct vmm_test_vm *vm, uint64_t gva)
{
	uint64_t *pte;

	pte = vmm_test_pte(vm, gva, 3, false);
	if (pte == NULL || *pte == 0)
		vmm_test_fail("%#jx is not mapped", (uintmax_t)gva);
	return (pte);
}

void
vmm_test_md_map(struct vmm_test_vm *vm, uint64_t gva, vm_paddr_t gpa,
    int flags)
{
	uint64_t *pte;

	pte = vmm_test_pte(vm, gva, 3, true);
	if (*pte != 0)
		vmm_test_fail("%#jx is already mapped", (uintmax_t)gva);
	*pte = gpa | vmm_test_pte_bits(flags);
}

void
vmm_test_md_unmap(struct vmm_test_vm *vm, uint64_t gva)
{

	*vmm_test_mapped_pte(vm, gva) = 0;
}

void
vmm_test_md_protect(struct vmm_test_vm *vm, uint64_t gva, int flags)
{
	uint64_t bits, *pte;

	pte = vmm_test_mapped_pte(vm, gva);
	bits = vmm_test_pte_bits(flags);
	/* Arm requires break-before-make to change the memory type. */
	if ((*pte & ATTR_S1_IDX_MASK) != (bits & ATTR_S1_IDX_MASK))
		vmm_test_fail("cannot change the memory type of %#jx",
		    (uintmax_t)gva);
	*pte = (*pte & ATTR_ADDR) | bits;
}

uint64_t *
vmm_test_md_pte(struct vmm_test_vm *vm, uint64_t gva)
{

	return (vmm_test_pte(vm, gva, 3, false));
}

/* Ucalls use the page tables installed by the harness. */
bool
vmm_test_md_gva2gpa(struct vmm_test_vm *vm, uint64_t gva, vm_paddr_t *gpap)
{
	uint64_t *table, mask, pte;
	vm_paddr_t pa;
	int lvl, shift;

	if (gva >= (1UL << 48) && gva < VMM_TEST_DMAP_BASE)
		return (false);
	pa = gva < VMM_TEST_DMAP_BASE ? vm->md.ttbr0 : vm->md.ttbr1;
	for (lvl = 0; lvl <= 3; lvl++) {
		shift = L0_SHIFT - lvl * Ln_ENTRIES_SHIFT;
		table = vmm_test_gpa2hva(vm, pa, PAGE_SIZE);
		pte = table[(gva >> shift) &
		    (lvl == 0 ? L0_ADDR_MASK : Ln_ADDR_MASK)];
		if ((pte & ATTR_DESCR_VALID) == 0)
			break;
		if (lvl == 3 || (pte & ATTR_DESCR_TYPE_MASK) == 0) {
			mask = (1UL << shift) - 1;
			*gpap = (pte & ATTR_ADDR & ~mask) | (gva & mask);
			return (true);
		}
		pa = pte & ATTR_ADDR;
	}
	return (false);
}

void
vmm_test_md_vm_init(struct vmm_test_vm *vm)
{
	uint64_t *pde;
	char *hva;
	vm_paddr_t gpa;
	size_t len;

	vm->md.ttbr0 = vmm_test_alloc_pages(vm, 1);
	vm->md.ttbr1 = vmm_test_alloc_pages(vm, 1);
	for (gpa = vm->mem_base;
	    gpa < vm->mem_base + VMM_TEST_MEMSIZE; gpa += L2_SIZE) {
		pde = vmm_test_pte(vm, VMM_TEST_PHYS_TO_DMAP(gpa), 2, true);
		*pde = gpa | ATTR_AF | ATTR_SH(ATTR_SH_IS) | ATTR_S1_IDX(0) |
		    L2_BLOCK;
	}

	/* The bootstrap must remain executable across enabling the MMU. */
	vm->md.boot = vmm_test_alloc_pages(vm, 1);
	vmm_test_md_map(vm, vm->md.boot, vm->md.boot, VMM_TEST_PROT_ALL);
	len = vmm_guest_boot_end - vmm_guest_boot;
	if (len > PAGE_SIZE)
		vmm_test_fail("the bootstrap does not fit in a page");
	hva = vmm_test_gpa2hva(vm, vm->md.boot, PAGE_SIZE);
	memcpy(hva, vmm_guest_boot, len);
	__builtin___clear_cache(hva, hva + len);
}

static void
vmm_test_set_regs(struct vmm_test_vcpu *vcpu, vmm_test_guest_fn_t *fn,
    uint64_t stack_top)
{
	static const int regnums[] = {
		VM_REG_GUEST_TTBR0_EL1, VM_REG_GUEST_TTBR1_EL1,
		VM_REG_GUEST_TCR_EL1, VM_REG_GUEST_CPSR, VM_REG_GUEST_PC,
		VM_REG_GUEST_X0, VM_REG_GUEST_X1,
		VM_REG_GUEST_X2, VM_REG_GUEST_X3,
	};
	const uint64_t tcr = TCR_TxSZ(16) | TCR_IPS_36BIT |
	    TCR_TG0_4K | TCR_TG1_4K | TCR_SH0_IS | TCR_SH1_IS |
	    TCR_IRGN0_WBWA | TCR_IRGN1_WBWA | TCR_ORGN0_WBWA |
	    TCR_ORGN1_WBWA;
	uint64_t regvals[] = {
		vcpu->vm->md.ttbr0, vcpu->vm->md.ttbr1, tcr,
		PSR_DAIF | PSR_M_EL1h, vcpu->vm->md.boot, (uintptr_t)fn,
		(uintptr_t)vmm_guest_start, stack_top,
		(uintptr_t)vmm_guest_vectors,
	};
	_Static_assert(nitems(regnums) == nitems(regvals),
	    "register list mismatch");

	if (vm_set_register_set(vcpu->vcpu, nitems(regnums), regnums,
	    regvals) != 0)
		vmm_test_fail("setting registers of vCPU %d: %s", vcpu->id,
		    strerror(errno));
}

void
vmm_test_md_vcpu_init(struct vmm_test_vcpu *vcpu, vmm_test_guest_fn_t *fn)
{
	vm_paddr_t stack;

	stack = vmm_test_alloc_pages(vcpu->vm, STACK_PAGES);
	/* vmm_guest_halt() stops in a BRK exit. */
	if (vm_set_capability(vcpu->vcpu, VM_CAP_BRK_EXIT, 1) != 0)
		vmm_test_fail("enabling breakpoint exits: %s",
		    strerror(errno));
	vmm_test_set_regs(vcpu, fn,
	    VMM_TEST_PHYS_TO_DMAP(stack + ptoa(STACK_PAGES)));
}

bool
vmm_test_md_exit_reenter(const struct vm_exit *vme)
{

	return (vme->exitcode == VM_EXITCODE_BOGUS);
}

bool
vmm_test_md_exit_ucall(struct vmm_test_vcpu *vcpu, uint64_t *gvap)
{
	const struct vm_exit *vme;

	vme = &vcpu->exit;
	if (vme->exitcode != VM_EXITCODE_SMCCC ||
	    vme->u.smccc_call.func_id != VMM_TEST_UCALL_ID)
		return (false);
	*gvap = vme->u.smccc_call.args[0];
	return (true);
}

/* Decode an abort's fault status code (DFSC or IFSC). */
static void
vmm_test_fault_status(uint64_t fsc, char *buf, size_t len)
{

	switch (fsc & ~3UL) {
	case 0x04:
		(void)snprintf(buf, len, "translation fault, level %ju",
		    (uintmax_t)(fsc & 3));
		return;
	case 0x08:
		(void)snprintf(buf, len, "access flag fault, level %ju",
		    (uintmax_t)(fsc & 3));
		return;
	case 0x0c:
		(void)snprintf(buf, len, "permission fault, level %ju",
		    (uintmax_t)(fsc & 3));
		return;
	}
	if (fsc == 0x10)
		(void)snprintf(buf, len, "synchronous external abort");
	else if (fsc == 0x21)
		(void)snprintf(buf, len, "alignment fault");
	else
		(void)snprintf(buf, len, "fault status %#jx", (uintmax_t)fsc);
}

/*
 * Describe an exception reported by vmm_guest_trap(), for example:
 * "data abort (EC 0x25, ESR 0x96000047: write, translation fault, level 3)
 * at pc 0x..., address 0x1000".
 */
void
vmm_test_md_describe_exception(const struct vmm_ucall *uc, char *buf,
    size_t len)
{
	uint64_t vector, esr, elr, far, ec;
	const char *name;
	char status[64];
	bool address;
	int n;

	vector = uc->args[0];
	esr = uc->args[1];
	elr = uc->args[2];
	far = uc->args[3];
	ec = ESR_ELx_EXCEPTION(esr);

	if (vector >= 16) {
		(void)snprintf(buf, len, "vector %ju", (uintmax_t)vector);
		return;
	}
	address = false;
	if (vector % 4 != 0) {
		n = snprintf(buf, len, "%s", vector_types[vector % 4]);
	} else {
		name = ec < nitems(exception_names) ? exception_names[ec] :
		    NULL;
		n = snprintf(buf, len, "%s (EC %#jx, ESR %#jx",
		    name != NULL ? name : "exception", (uintmax_t)ec,
		    (uintmax_t)esr);
		if (n < 0 || (size_t)n >= len)
			return;
		buf += n;
		len -= n;
		switch (ec) {
		case EXCP_DATA_ABORT:
		case EXCP_DATA_ABORT_L:
			vmm_test_fault_status(esr & ISS_DATA_DFSC_MASK, status,
			    sizeof(status));
			n = snprintf(buf, len, ": %s, %s)",
			    (esr & ISS_DATA_WnR) != 0 ? "write" : "read",
			    status);
			address = true;
			break;
		case EXCP_INSN_ABORT:
		case EXCP_INSN_ABORT_L:
			vmm_test_fault_status(esr & ISS_DATA_DFSC_MASK, status,
			    sizeof(status));
			n = snprintf(buf, len, ": %s)", status);
			address = true;
			break;
		case EXCP_PC_ALIGN:
		case EXCP_WATCHPT_EL1:
			address = true;
			/* FALLTHROUGH */
		default:
			n = snprintf(buf, len, ")");
			break;
		}
	}
	if (n < 0 || (size_t)n >= len)
		return;
	buf += n;
	len -= n;

	n = snprintf(buf, len, " at pc %#jx", (uintmax_t)elr);
	if (n < 0 || (size_t)n >= len)
		return;
	buf += n;
	len -= n;
	if (address) {
		n = snprintf(buf, len, ", address %#jx", (uintmax_t)far);
		if (n < 0 || (size_t)n >= len)
			return;
		buf += n;
		len -= n;
	}
	if (vector / 4 != 1)
		(void)snprintf(buf, len, " from %s",
		    vector_origins[vector / 4]);
}

void
vmm_test_md_vector_name(int vector, char *buf, size_t len)
{
	static const char *const others[] = { "IRQ", "FIQ", "SError" };

	if (vector >= 0 && vector < (int)nitems(exception_names) &&
	    exception_names[vector] != NULL)
		(void)snprintf(buf, len, "%s", exception_names[vector]);
	else if (vector >= VMM_GUEST_EXC_IRQ &&
	    vector <= VMM_GUEST_EXC_SERROR)
		(void)snprintf(buf, len, "%s", others[vector -
		    VMM_GUEST_EXC_IRQ]);
	else
		(void)snprintf(buf, len, "EC %#x", vector);
}

const char *
vmm_test_md_exitcode_name(int exitcode)
{

	if ((u_int)exitcode >= nitems(exitcode_names) ||
	    exitcode_names[exitcode] == NULL)
		return ("unknown");
	return (exitcode_names[exitcode]);
}

void
vmm_test_md_describe_exit(const struct vm_exit *vme, char *buf, size_t len)
{
	int n;

	n = snprintf(buf, len, "%s exit (code %d) at pc %#jx",
	    vmm_test_md_exitcode_name(vme->exitcode), vme->exitcode,
	    (uintmax_t)vme->pc);
	if (n < 0 || (size_t)n >= len)
		return;
	buf += n;
	len -= n;

	switch (vme->exitcode) {
	case VM_EXITCODE_REG_EMUL:
		(void)snprintf(buf, len, ": syndrome %#x",
		    vme->u.reg_emul.vre.inst_syndrome);
		break;
	case VM_EXITCODE_HYP:
		(void)snprintf(buf, len, ": esr %#jx, far %#jx",
		    (uintmax_t)vme->u.hyp.esr_el2,
		    (uintmax_t)vme->u.hyp.far_el2);
		break;
	case VM_EXITCODE_INST_EMUL:
		(void)snprintf(buf, len, ": gpa %#jx",
		    (uintmax_t)vme->u.inst_emul.gpa);
		break;
	case VM_EXITCODE_PAGING:
		(void)snprintf(buf, len, ": gpa %#jx, esr %#jx",
		    (uintmax_t)vme->u.paging.gpa, (uintmax_t)vme->u.paging.esr);
		break;
	case VM_EXITCODE_SMCCC:
		(void)snprintf(buf, len, ": function %#jx",
		    (uintmax_t)vme->u.smccc_call.func_id);
		break;
	default:
		break;
	}
}
