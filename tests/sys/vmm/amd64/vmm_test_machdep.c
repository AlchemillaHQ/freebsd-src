/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * Host-side amd64 support: guest page tables, descriptor tables and the
 * initial vCPU state, which places the vCPU directly in 64-bit mode at CPL 0.
 */

#include <sys/param.h>
#include <sys/cpuset.h>

#include <machine/psl.h>
#include <machine/pte.h>
#include <machine/segments.h>
#include <machine/specialreg.h>
#include <machine/tss.h>
#include <machine/vmm.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <vmmapi.h>

#include "vmm_test.h"

/* Guest GDT layout. */
#define	GDT_CODE	1
#define	GDT_DATA	2
#define	GDT_TSS		3	/* occupies two slots */
#define	GDT_NENTRIES	5

/* Segment access rights in the VMX format used by vm_set_desc(). */
#define	ACCESS_CODE64	0xa09b	/* G, L, P, S, execute/read, accessed */
#define	ACCESS_DATA	0xc093	/* G, D/B, P, S, read/write, accessed */
#define	ACCESS_TSS	0x008b	/* P, busy 64-bit TSS */
#define	ACCESS_UNUSABLE	0x10000

/* Entries per page table page. */
#define	NPTES		(PAGE_SIZE / sizeof(uint64_t))

/* Size of each vCPU's stack. */
#define	STACK_PAGES	4

/* Size of each IDT entry stub in vmm_test_exception.S. */
#define	TRAP_STUB_SIZE	16

/* Exception mnemonics, and whether the exception has an error code. */
static const struct {
	const char	*name;
	bool		errcode;
} exception_names[NRSVIDT] = {
	[IDT_DE] = { "#DE", false },
	[IDT_DB] = { "#DB", false },
	[IDT_NMI] = { "NMI", false },
	[IDT_BP] = { "#BP", false },
	[IDT_OF] = { "#OF", false },
	[IDT_BR] = { "#BR", false },
	[IDT_UD] = { "#UD", false },
	[IDT_NM] = { "#NM", false },
	[IDT_DF] = { "#DF", true },
	[IDT_TS] = { "#TS", true },
	[IDT_NP] = { "#NP", true },
	[IDT_SS] = { "#SS", true },
	[IDT_GP] = { "#GP", true },
	[IDT_PF] = { "#PF", true },
	[IDT_MF] = { "#MF", false },
	[IDT_AC] = { "#AC", true },
	[IDT_MC] = { "#MC", false },
	[IDT_XF] = { "#XM", false },
	[20] = { "#VE", false },
	[21] = { "#CP", true },
	[28] = { "#HV", false },
	[29] = { "#VC", true },
	[30] = { "#SX", true },
};

static const char *const exitcode_names[] = {
	[VM_EXITCODE_INOUT] = "INOUT",
	[VM_EXITCODE_VMX] = "VMX",
	[VM_EXITCODE_BOGUS] = "BOGUS",
	[VM_EXITCODE_RDMSR] = "RDMSR",
	[VM_EXITCODE_WRMSR] = "WRMSR",
	[VM_EXITCODE_HLT] = "HLT",
	[VM_EXITCODE_MTRAP] = "MTRAP",
	[VM_EXITCODE_PAUSE] = "PAUSE",
	[VM_EXITCODE_PAGING] = "PAGING",
	[VM_EXITCODE_INST_EMUL] = "INST_EMUL",
	[VM_EXITCODE_SPINUP_AP] = "SPINUP_AP",
	[VM_EXITCODE_RENDEZVOUS] = "RENDEZVOUS",
	[VM_EXITCODE_IOAPIC_EOI] = "IOAPIC_EOI",
	[VM_EXITCODE_SUSPENDED] = "SUSPENDED",
	[VM_EXITCODE_INOUT_STR] = "INOUT_STR",
	[VM_EXITCODE_TASK_SWITCH] = "TASK_SWITCH",
	[VM_EXITCODE_MONITOR] = "MONITOR",
	[VM_EXITCODE_MWAIT] = "MWAIT",
	[VM_EXITCODE_SVM] = "SVM",
	[VM_EXITCODE_REQIDLE] = "REQIDLE",
	[VM_EXITCODE_DEBUG] = "DEBUG",
	[VM_EXITCODE_VMINSN] = "VMINSN",
	[VM_EXITCODE_BPT] = "BPT",
	[VM_EXITCODE_IPI] = "IPI",
	[VM_EXITCODE_DB] = "DB",
};

static const char *const suspend_names[] = {
	[VM_SUSPEND_NONE] = "none",
	[VM_SUSPEND_RESET] = "reset",
	[VM_SUSPEND_POWEROFF] = "poweroff",
	[VM_SUSPEND_HALT] = "halt",
	[VM_SUSPEND_TRIPLEFAULT] = "triple fault",
	[VM_SUSPEND_DESTROY] = "destroy",
};

/*
 * Return the page table entry for 'gva' at 'level' (1 for a 4KB page, 2 for a
 * 2MB page).  With 'alloc', allocate missing page table pages; otherwise
 * return NULL if one is missing.  Also return NULL, or fail with 'alloc', if
 * a large page maps 'gva' above 'level'.
 */
static uint64_t *
vmm_test_pte(struct vmm_test_vm *vm, uint64_t gva, int level, bool alloc)
{
	uint64_t *table, *pte;
	vm_paddr_t pa;
	int lvl;

	pa = vm->md.pml4;
	for (lvl = 4;; lvl--) {
		table = vmm_test_gpa2hva(vm, pa, PAGE_SIZE);
		pte = &table[(gva >> (PAGE_SHIFT + NPTEPGSHIFT * (lvl - 1))) &
		    (NPTES - 1)];
		if (lvl == level)
			return (pte);
		if ((*pte & X86_PG_V) == 0) {
			if (!alloc)
				return (NULL);
			*pte = vmm_test_alloc_pages(vm, 1) | X86_PG_V |
			    X86_PG_RW;
		}
		if ((*pte & X86_PG_PS) != 0) {
			if (!alloc)
				return (NULL);
			vmm_test_fail("%#jx is already mapped by a large page",
			    (uintmax_t)gva);
		}
		pa = *pte & PG_FRAME;
	}
}

/*
 * A software bit in every leaf entry the harness makes, so that a page
 * protected without read access, which is not present, still counts as
 * mapped, even at guest-physical address 0.
 */
#define	PTE_MAPPED	X86_PG_AVAIL1
#define	PTE_CACHE	(X86_PG_NC_PCD | X86_PG_NC_PWT)

/*
 * Leaf entry bits for vmm_test_map() flags.  An inaccessible page keeps its
 * memory type, so that it can be made accessible again.
 */
static uint64_t
vmm_test_pte_bits(int flags)
{
	uint64_t bits;

	bits = PTE_MAPPED;
	if ((flags & VMM_TEST_MAP_DEVICE) != 0)
		bits |= PTE_CACHE;
	if ((flags & VMM_TEST_PROT_READ) == 0)
		return (bits);
	bits |= X86_PG_V;
	if ((flags & VMM_TEST_PROT_WRITE) != 0)
		bits |= X86_PG_RW;
	if ((flags & VMM_TEST_PROT_EXEC) == 0)
		bits |= X86_PG_NX;
	return (bits);
}

/* Return the 4KB page entry for 'gva', which must already have one. */
static uint64_t *
vmm_test_mapped_pte(struct vmm_test_vm *vm, uint64_t gva)
{
	uint64_t *pte;

	pte = vmm_test_pte(vm, gva, 1, false);
	if (pte == NULL || (*pte & PTE_MAPPED) == 0)
		vmm_test_fail("%#jx is not mapped", (uintmax_t)gva);
	return (pte);
}

void
vmm_test_md_map(struct vmm_test_vm *vm, uint64_t gva, vm_paddr_t gpa,
    int flags)
{
	uint64_t *pte;

	/* The guest uses 4-level paging. */
	if (gva >= (1UL << 47))
		vmm_test_fail("%#jx is not a lower-half 48-bit address",
		    (uintmax_t)gva);
	pte = vmm_test_pte(vm, gva, 1, true);
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
	/* As on arm64, where changing it needs break-before-make. */
	if ((*pte & PTE_CACHE) != (bits & PTE_CACHE))
		vmm_test_fail("cannot change the memory type of %#jx",
		    (uintmax_t)gva);
	*pte = (*pte & PG_FRAME) | bits;
}

uint64_t *
vmm_test_md_pte(struct vmm_test_vm *vm, uint64_t gva)
{

	return (vmm_test_pte(vm, gva, 1, false));
}

/*
 * Translate using the page tables set up by the harness rather than the
 * vCPU's current %cr3, so ucalls rely on the guest keeping them loaded.
 */
bool
vmm_test_md_gva2gpa(struct vmm_test_vm *vm, uint64_t gva, vm_paddr_t *gpap)
{
	uint64_t *table, mask, pte;
	vm_paddr_t pa;
	int lvl, shift;

	pa = vm->md.pml4;
	for (lvl = 4; lvl >= 1; lvl--) {
		shift = PAGE_SHIFT + NPTEPGSHIFT * (lvl - 1);
		table = vmm_test_gpa2hva(vm, pa, PAGE_SIZE);
		pte = table[(gva >> shift) & (NPTES - 1)];
		if ((pte & X86_PG_V) == 0)
			break;
		if (lvl == 1 || (pte & X86_PG_PS) != 0) {
			mask = (1UL << shift) - 1;
			*gpap = (pte & PG_FRAME & ~mask) | (gva & mask);
			return (true);
		}
		pa = pte & PG_FRAME;
	}
	return (false);
}

/* Point every IDT vector at its stub, with an interrupt gate. */
static void
vmm_test_idt_init(struct vmm_test_vm *vm)
{
	struct gate_descriptor *gd, *idt;
	uintptr_t stub;
	int i;

	_Static_assert(NIDT * sizeof(*idt) <= PAGE_SIZE, "IDT too large");
	vm->md.idt = vmm_test_alloc_pages(vm, 1);
	idt = vmm_test_gpa2hva(vm, vm->md.idt, PAGE_SIZE);
	for (i = 0; i < NIDT; i++) {
		stub = (uintptr_t)vmm_guest_trap_stubs + i * TRAP_STUB_SIZE;
		gd = &idt[i];
		gd->gd_looffset = stub;
		gd->gd_selector = GSEL(GDT_CODE, SEL_KPL);
		gd->gd_type = SDT_SYSIGT;
		gd->gd_dpl = SEL_KPL;
		gd->gd_p = 1;
		gd->gd_hioffset = stub >> 16;
	}
}

void
vmm_test_md_vm_init(struct vmm_test_vm *vm)
{
	uint64_t *pde;
	vm_paddr_t gpa;

	vm->md.pml4 = vmm_test_alloc_pages(vm, 1);
	for (gpa = 0; gpa < VMM_TEST_MEMSIZE; gpa += NBPDR) {
		pde = vmm_test_pte(vm, VMM_TEST_PHYS_TO_DMAP(gpa), 2, true);
		*pde = gpa | X86_PG_V | X86_PG_RW | X86_PG_PS;
	}
	vmm_test_idt_init(vm);
}

static void
vmm_test_set_desc(struct vmm_test_vcpu *vcpu, int reg, uint64_t base,
    uint32_t limit, uint32_t access)
{

	if (vm_set_desc(vcpu->vcpu, reg, base, limit, access) != 0)
		vmm_test_fail("setting descriptor %d of vCPU %d: %s", reg,
		    vcpu->id, strerror(errno));
}

/* Start the vCPU at 'rip' in 64-bit mode at CPL 0. */
static void
vmm_test_set_regs(struct vmm_test_vcpu *vcpu, uint64_t rip, uint64_t rsp,
    uint64_t rdi)
{
	static const int regnums[] = {
		VM_REG_GUEST_CR0,
		VM_REG_GUEST_CR3,
		VM_REG_GUEST_CR4,
		VM_REG_GUEST_EFER,
		VM_REG_GUEST_RFLAGS,
		VM_REG_GUEST_CS,
		VM_REG_GUEST_DS,
		VM_REG_GUEST_ES,
		VM_REG_GUEST_FS,
		VM_REG_GUEST_GS,
		VM_REG_GUEST_SS,
		VM_REG_GUEST_TR,
		VM_REG_GUEST_LDTR,
		VM_REG_GUEST_RIP,
		VM_REG_GUEST_RSP,
		VM_REG_GUEST_RDI,
	};
	uint64_t regvals[] = {
		CR0_PE | CR0_MP | CR0_NE | CR0_WP | CR0_PG,
		vcpu->vm->md.pml4,
		/* Compilers use SSE freely, so enable it. */
		CR4_PAE | CR4_FXSR | CR4_XMM,
		/* NXE lets mappings be non-executable. */
		EFER_LME | EFER_LMA | EFER_NXE,
		PSL_KERNEL,	/* interrupts disabled */
		GSEL(GDT_CODE, SEL_KPL),
		GSEL(GDT_DATA, SEL_KPL),
		GSEL(GDT_DATA, SEL_KPL),
		GSEL(GDT_DATA, SEL_KPL),
		GSEL(GDT_DATA, SEL_KPL),
		GSEL(GDT_DATA, SEL_KPL),
		GSEL(GDT_TSS, SEL_KPL),
		0,
		rip,
		rsp,
		rdi,
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
	static const int dataregs[] = {
		VM_REG_GUEST_DS, VM_REG_GUEST_ES, VM_REG_GUEST_FS,
		VM_REG_GUEST_GS, VM_REG_GUEST_SS,
	};
	struct vmm_test_vm *vm;
	struct user_segment_descriptor *gdt, *sd;
	struct system_segment_descriptor *ssd;
	uint64_t gdt_va, stack_top, tss_va;
	vm_paddr_t gpa;
	u_int i;

	vm = vcpu->vm;

	/*
	 * Each vCPU has its own GDT and TSS, sharing a page.  VMX requires a
	 * usable TSS even though nothing here uses it.
	 */
	gpa = vmm_test_alloc_pages(vm, 1);
	gdt = vmm_test_gpa2hva(vm, gpa, PAGE_SIZE);
	gdt_va = VMM_TEST_PHYS_TO_DMAP(gpa);
	tss_va = gdt_va + GDT_NENTRIES * sizeof(*gdt);

	sd = &gdt[GDT_CODE];
	sd->sd_lolimit = 0xffff;
	sd->sd_type = SDT_MEMERA;
	sd->sd_dpl = SEL_KPL;
	sd->sd_p = 1;
	sd->sd_hilimit = 0xf;
	sd->sd_long = 1;
	sd->sd_gran = 1;

	sd = &gdt[GDT_DATA];
	sd->sd_lolimit = 0xffff;
	sd->sd_type = SDT_MEMRWA;
	sd->sd_dpl = SEL_KPL;
	sd->sd_p = 1;
	sd->sd_hilimit = 0xf;
	sd->sd_def32 = 1;
	sd->sd_gran = 1;

	ssd = (struct system_segment_descriptor *)(void *)&gdt[GDT_TSS];
	ssd->sd_lolimit = sizeof(struct amd64tss) - 1;
	ssd->sd_lobase = tss_va;
	ssd->sd_type = SDT_SYSBSY;
	ssd->sd_dpl = SEL_KPL;
	ssd->sd_p = 1;
	ssd->sd_hibase = tss_va >> 24;

	gpa = vmm_test_alloc_pages(vm, STACK_PAGES);
	stack_top = VMM_TEST_PHYS_TO_DMAP(gpa + ptoa(STACK_PAGES));

	vmm_test_set_desc(vcpu, VM_REG_GUEST_CS, 0, UINT32_MAX, ACCESS_CODE64);
	for (i = 0; i < nitems(dataregs); i++)
		vmm_test_set_desc(vcpu, dataregs[i], 0, UINT32_MAX,
		    ACCESS_DATA);
	vmm_test_set_desc(vcpu, VM_REG_GUEST_TR, tss_va,
	    sizeof(struct amd64tss) - 1, ACCESS_TSS);
	vmm_test_set_desc(vcpu, VM_REG_GUEST_LDTR, 0, 0, ACCESS_UNUSABLE);
	vmm_test_set_desc(vcpu, VM_REG_GUEST_GDTR, gdt_va,
	    GDT_NENTRIES * sizeof(*gdt) - 1, 0);
	vmm_test_set_desc(vcpu, VM_REG_GUEST_IDTR,
	    VMM_TEST_PHYS_TO_DMAP(vm->md.idt),
	    NIDT * sizeof(struct gate_descriptor) - 1, 0);

	/* Enter as if called, with a zero return address on the stack. */
	vmm_test_set_regs(vcpu, (uintptr_t)vmm_guest_start,
	    stack_top - sizeof(uint64_t), (uintptr_t)fn);
}

bool
vmm_test_md_exit_reenter(const struct vm_exit *vme)
{

	return (vme->exitcode == VM_EXITCODE_BOGUS ||
	    vme->exitcode == VM_EXITCODE_REQIDLE);
}

/* A ucall is a 4-byte OUT to the ucall port with %rdi pointing at it. */
bool
vmm_test_md_exit_ucall(struct vmm_test_vcpu *vcpu, uint64_t *gvap)
{
	const struct vm_exit *vme;

	vme = &vcpu->exit;
	if (vme->exitcode != VM_EXITCODE_INOUT || vme->u.inout.in ||
	    vme->u.inout.port != VMM_TEST_UCALL_PORT ||
	    vme->u.inout.bytes != 4)
		return (false);
	*gvap = vmm_test_get_reg(vcpu, VM_REG_GUEST_RDI);
	return (true);
}

/*
 * Describe an exception reported by vmm_guest_trap(), using the SDM notation
 * for the error code: "#PF(0x2) at rip 0x...".
 */
void
vmm_test_md_describe_exception(const struct vmm_ucall *uc, char *buf,
    size_t len)
{
	uint64_t vector, error, rip, addr;
	const char *name;
	int n;

	vector = uc->args[0];
	error = uc->args[1];
	rip = uc->args[2];
	addr = uc->args[3];

	name = vector < nitems(exception_names) ?
	    exception_names[vector].name : NULL;
	if (name == NULL)
		n = snprintf(buf, len, "vector %ju", (uintmax_t)vector);
	else if (exception_names[vector].errcode)
		n = snprintf(buf, len, "%s(%#jx)", name, (uintmax_t)error);
	else
		n = snprintf(buf, len, "%s", name);
	if (n < 0 || (size_t)n >= len)
		return;
	buf += n;
	len -= n;

	n = snprintf(buf, len, " at rip %#jx", (uintmax_t)rip);
	if (n < 0 || (size_t)n >= len)
		return;
	if (vector == IDT_PF)
		(void)snprintf(buf + n, len - n, ", address %#jx",
		    (uintmax_t)addr);
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

	n = snprintf(buf, len, "%s exit (code %d) at rip %#jx",
	    vmm_test_md_exitcode_name(vme->exitcode), vme->exitcode,
	    (uintmax_t)vme->rip);
	if (n < 0 || (size_t)n >= len)
		return;
	buf += n;
	len -= n;

	switch (vme->exitcode) {
	case VM_EXITCODE_INOUT:
		(void)snprintf(buf, len, ": %s port %#x, %d bytes, eax %#x",
		    vme->u.inout.in ? "in" : "out", vme->u.inout.port,
		    vme->u.inout.bytes, vme->u.inout.eax);
		break;
	case VM_EXITCODE_RDMSR:
		(void)snprintf(buf, len, ": msr %#x", vme->u.msr.code);
		break;
	case VM_EXITCODE_WRMSR:
		(void)snprintf(buf, len, ": msr %#x, value %#jx",
		    vme->u.msr.code, (uintmax_t)vme->u.msr.wval);
		break;
	case VM_EXITCODE_PAGING:
		(void)snprintf(buf, len, ": gpa %#jx, fault type %d",
		    (uintmax_t)vme->u.paging.gpa, vme->u.paging.fault_type);
		break;
	case VM_EXITCODE_INST_EMUL:
		(void)snprintf(buf, len, ": gpa %#jx, gla %#jx",
		    (uintmax_t)vme->u.inst_emul.gpa,
		    (uintmax_t)vme->u.inst_emul.gla);
		break;
	case VM_EXITCODE_SUSPENDED:
		(void)snprintf(buf, len, ": %s",
		    (u_int)vme->u.suspended.how < nitems(suspend_names) ?
		    suspend_names[vme->u.suspended.how] : "unknown");
		break;
	case VM_EXITCODE_VMX:
		(void)snprintf(buf, len,
		    ": status %d, reason %u, qualification %#jx, error %d",
		    vme->u.vmx.status, vme->u.vmx.exit_reason,
		    (uintmax_t)vme->u.vmx.exit_qualification,
		    vme->u.vmx.inst_error);
		break;
	case VM_EXITCODE_SVM:
		(void)snprintf(buf, len, ": exitcode %#jx, info1 %#jx, "
		    "info2 %#jx", (uintmax_t)vme->u.svm.exitcode,
		    (uintmax_t)vme->u.svm.exitinfo1,
		    (uintmax_t)vme->u.svm.exitinfo2);
		break;
	default:
		break;
	}
}
