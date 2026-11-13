/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#include <sys/param.h>
#include <sys/cpuset.h>

#include <machine/vmm.h>

#include <errno.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vmmapi.h>

#include <atf-c.h>

#include "vmm_test.h"

/* Leave the first megabyte of guest memory unused. */
#define	VMM_TEST_ALLOC_OFFSET	(1024 * 1024)

static const char *const ucall_names[] = {
	[UCALL_NONE] = "none",
	[UCALL_SYNC] = "SYNC",
	[UCALL_DONE] = "DONE",
	[UCALL_ABORT] = "ABORT",
	[UCALL_EXCEPTION] = "EXCEPTION",
};

/* The test binary's loadable segments, as mapped in this process. */
static struct {
	uintptr_t	start;
	uintptr_t	end;
} vmm_test_image[8];
static u_int vmm_test_image_nsegs;

void (*vmm_test_fail_hook)(const char *);

static const char *
ucall_name(uint64_t cmd)
{

	if (cmd >= nitems(ucall_names))
		return ("unknown");
	return (ucall_names[cmd]);
}

void
vmm_test_fail(const char *fmt, ...)
{
	char msg[512];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (vmm_test_fail_hook != NULL)
		vmm_test_fail_hook(msg);
	atf_tc_fail("%s", msg);
}

void *
vmm_test_gpa2hva(struct vmm_test_vm *vm, vm_paddr_t gpa, size_t len)
{
	void *hva;

	hva = vm_map_gpa(vm->ctx, gpa, len);
	if (hva == NULL)
		vmm_test_fail("%#jx+%#zx is not guest memory", (uintmax_t)gpa,
		    len);
	return (hva);
}

vm_paddr_t
vmm_test_alloc_pages(struct vmm_test_vm *vm, size_t npages)
{
	vm_paddr_t gpa;
	size_t len;

	len = npages * PAGE_SIZE;
	gpa = vm->next_free;
	if (gpa + len > vm->mem_base + VMM_TEST_MEMSIZE)
		vmm_test_fail("out of guest memory allocating %zu pages",
		    npages);
	vm->next_free += len;
	memset(vmm_test_gpa2hva(vm, gpa, len), 0, len);
	return (gpa);
}

static void
vmm_test_read_gva(struct vmm_test_vm *vm, uint64_t gva, void *buf, size_t len)
{
	vm_paddr_t gpa;
	size_t n;

	for (; len > 0; gva += n, buf = (char *)buf + n, len -= n) {
		n = MIN(len, PAGE_SIZE - (gva & PAGE_MASK));
		if (!vmm_test_md_gva2gpa(vm, gva, &gpa))
			vmm_test_fail("guest address %#jx is not mapped",
			    (uintmax_t)gva);
		memcpy(buf, vmm_test_gpa2hva(vm, gpa, n), n);
	}
}

/*
 * Copy the loadable segments of the test binary into the guest, at the same
 * virtual addresses they have in this process.
 */
static int
vmm_test_load_image(struct dl_phdr_info *info, size_t size __unused,
    void *arg)
{
	struct vmm_test_vm *vm;
	const Elf_Phdr *phdr;
	char *hva;
	uintptr_t self, start, end, va;
	vm_paddr_t gpa;
	bool found;
	int i;

	/* Skip shared objects; the test binary is the one containing us. */
	self = (uintptr_t)vmm_test_load_image;
	found = false;
	for (i = 0; i < info->dlpi_phnum; i++) {
		phdr = &info->dlpi_phdr[i];
		start = info->dlpi_addr + phdr->p_vaddr;
		if (phdr->p_type == PT_LOAD && self >= start &&
		    self < start + phdr->p_memsz)
			found = true;
	}
	if (!found)
		return (0);

	vm = arg;
	vmm_test_image_nsegs = 0;
	for (i = 0; i < info->dlpi_phnum; i++) {
		phdr = &info->dlpi_phdr[i];
		if (phdr->p_type != PT_LOAD)
			continue;
		start = trunc_page(info->dlpi_addr + phdr->p_vaddr);
		end = round_page(info->dlpi_addr + phdr->p_vaddr +
		    phdr->p_memsz);
		if (vmm_test_image_nsegs == nitems(vmm_test_image))
			vmm_test_fail("the test binary has too many segments");
		vmm_test_image[vmm_test_image_nsegs].start = start;
		vmm_test_image[vmm_test_image_nsegs].end = end;
		vmm_test_image_nsegs++;
		/*
		 * Allocate the segment in one piece before mapping it, so that
		 * page table pages do not split it in guest-physical memory
		 * and VMM_TEST_GUEST_VAR() works for any variable.
		 */
		gpa = vmm_test_alloc_pages(vm, atop(end - start));
		hva = vmm_test_gpa2hva(vm, gpa, end - start);
		memcpy(hva, (const void *)start, end - start);
		__builtin___clear_cache(hva, hva + (end - start));
		for (va = start; va < end; va += PAGE_SIZE, gpa += PAGE_SIZE)
			vmm_test_md_map_page(vm, va, gpa);
	}
	return (1);
}

struct vmm_test_vm *
vmm_test_vm_create(void)
{
	static u_int seq;
	struct vmm_test_vm *vm;

	vm = calloc(1, sizeof(*vm));
	if (vm == NULL)
		vmm_test_fail("allocating a VM: %s", strerror(errno));
	(void)snprintf(vm->name, sizeof(vm->name), "vmm-test-%d-%u",
	    (int)getpid(), seq++);

	vm->ctx = vm_openf(vm->name,
	    VMMAPI_OPEN_CREATE | VMMAPI_OPEN_CREATE_DESTROY_ON_CLOSE);
	if (vm->ctx == NULL)
		vmm_test_fail("creating VM %s: %s", vm->name, strerror(errno));
	if (vm_setup_memory(vm->ctx, VMM_TEST_MEMSIZE, VM_MMAP_ALL) != 0)
		vmm_test_fail("setting up guest memory: %s", strerror(errno));
	vm->mem_base = vm_get_lowmem_size(vm->ctx) != 0 ? 0 :
	    vm_get_highmem_base(vm->ctx);
	vm->next_free = vm->mem_base + VMM_TEST_ALLOC_OFFSET;

	vmm_test_md_vm_init(vm);
	if (dl_iterate_phdr(vmm_test_load_image, vm) == 0)
		vmm_test_fail("test binary not found among loaded objects");
	return (vm);
}

struct vmm_test_vm *
vmm_test_vm_create_with_vcpu(vmm_test_guest_fn_t *fn,
    struct vmm_test_vcpu **vcpup)
{
	struct vmm_test_vm *vm;

	vm = vmm_test_vm_create();
	*vcpup = vmm_test_vcpu_add(vm, 0, fn);
	return (vm);
}

struct vmm_test_vcpu *
vmm_test_vcpu_add(struct vmm_test_vm *vm, int id, vmm_test_guest_fn_t *fn)
{
	struct vmm_test_vcpu *vcpu;

	vcpu = calloc(1, sizeof(*vcpu));
	if (vcpu == NULL)
		vmm_test_fail("allocating a vCPU: %s", strerror(errno));
	vcpu->vm = vm;
	vcpu->id = id;
	vcpu->vcpu = vm_vcpu_open(vm->ctx, id);
	if (vcpu->vcpu == NULL)
		vmm_test_fail("opening vCPU %d: %s", id, strerror(errno));
	if (vm_activate_cpu(vcpu->vcpu) != 0)
		vmm_test_fail("activating vCPU %d: %s", id, strerror(errno));
	vmm_test_md_vcpu_init(vcpu, fn);
	return (vcpu);
}

uint64_t
vmm_test_get_reg(struct vmm_test_vcpu *vcpu, int reg)
{
	uint64_t val;

	if (vm_get_register(vcpu->vcpu, reg, &val) != 0)
		vmm_test_fail("getting register %d of vCPU %d: %s", reg,
		    vcpu->id, strerror(errno));
	return (val);
}

void
vmm_test_set_reg(struct vmm_test_vcpu *vcpu, int reg, uint64_t val)
{

	if (vm_set_register(vcpu->vcpu, reg, val) != 0)
		vmm_test_fail("setting register %d of vCPU %d: %s", reg,
		    vcpu->id, strerror(errno));
}

/*
 * Return a string that the guest named in a ucall.  The host reads its own
 * copy, so the string must lie in the test binary; describe any other
 * pointer instead of following it.
 */
static const char *
vmm_test_ucall_string(const char *s, char *buf, size_t len)
{
	uintptr_t p;
	u_int i;

	p = (uintptr_t)s;
	for (i = 0; i < vmm_test_image_nsegs; i++) {
		if (p >= vmm_test_image[i].start &&
		    p < vmm_test_image[i].end &&
		    memchr(s, '\0', vmm_test_image[i].end - p) != NULL)
			return (s);
	}
	(void)snprintf(buf, len, "<bad pointer %#jx>", (uintmax_t)p);
	return (buf);
}

/* The base name of the guest source file that made the ucall. */
static const char *
vmm_test_ucall_file(const struct vmm_ucall *uc, char *buf, size_t len)
{
	const char *p, *path;

	path = vmm_test_ucall_string(uc->file, buf, len);
	p = strrchr(path, '/');
	return (p != NULL ? p + 1 : path);
}

static void __dead2
vmm_test_report_guest_failure(struct vmm_test_vcpu *vcpu)
{
	const struct vmm_ucall *uc;
	const char *file, *msg;
	char filebuf[40], msgbuf[40];

	uc = &vcpu->ucall;
	file = vmm_test_ucall_file(uc, filebuf, sizeof(filebuf));
	msg = uc->msg != NULL ?
	    vmm_test_ucall_string(uc->msg, msgbuf, sizeof(msgbuf)) : "";
	if (uc->args[2] != 0) {
		vmm_test_fail("vCPU %d: guest assertion failed at %s:%d: %s "
		    "(%#jx != %#jx)", vcpu->id, file, uc->line, msg,
		    (uintmax_t)uc->args[0], (uintmax_t)uc->args[1]);
	}
	vmm_test_fail("vCPU %d: guest assertion failed at %s:%d: %s",
	    vcpu->id, file, uc->line, msg);
}

static void __dead2
vmm_test_report_guest_exception(struct vmm_test_vcpu *vcpu)
{
	char desc[256];

	vmm_test_md_describe_exception(&vcpu->ucall, desc, sizeof(desc));
	vmm_test_fail("vCPU %d: unhandled guest exception %s", vcpu->id, desc);
}

static int
vmm_test_run(struct vmm_test_vcpu *vcpu, bool try)
{
	struct vm_run vmrun;
	cpuset_t dmask;
	uint64_t gva;
	int error;

	memset(&vcpu->ucall, 0, sizeof(vcpu->ucall));
	do {
		memset(&vmrun, 0, sizeof(vmrun));
		vmrun.vm_exit = &vcpu->exit;
		vmrun.cpuset = &dmask;
		vmrun.cpusetsize = sizeof(dmask);
		if (vm_run(vcpu->vcpu, &vmrun) != 0) {
			error = errno;
			if (try)
				return (error);
			vmm_test_fail("VM_RUN on vCPU %d: %s", vcpu->id,
			    strerror(error));
		}
	} while (vmm_test_md_exit_reenter(&vcpu->exit));

	if (vmm_test_md_exit_ucall(vcpu, &gva)) {
		vmm_test_read_gva(vcpu->vm, gva, &vcpu->ucall,
		    sizeof(vcpu->ucall));
		if (vcpu->ucall.cmd == UCALL_ABORT)
			vmm_test_report_guest_failure(vcpu);
		if (vcpu->ucall.cmd == UCALL_EXCEPTION)
			vmm_test_report_guest_exception(vcpu);
	}
	return (0);
}

struct vm_exit *
vmm_test_vcpu_run(struct vmm_test_vcpu *vcpu)
{

	(void)vmm_test_run(vcpu, false);
	return (&vcpu->exit);
}

int
vmm_test_vcpu_try_run(struct vmm_test_vcpu *vcpu)
{

	return (vmm_test_run(vcpu, true));
}

void
vmm_test_fail_exit(struct vmm_test_vcpu *vcpu, const char *why)
{
	const struct vmm_ucall *uc;
	char desc[256], filebuf[40];

	uc = &vcpu->ucall;
	if (uc->cmd != UCALL_NONE) {
		vmm_test_fail("vCPU %d: %s, got %s ucall from %s:%d", vcpu->id,
		    why, ucall_name(uc->cmd),
		    vmm_test_ucall_file(uc, filebuf, sizeof(filebuf)),
		    uc->line);
	}
	vmm_test_md_describe_exit(&vcpu->exit, desc, sizeof(desc));
	vmm_test_fail("vCPU %d: %s, got %s", vcpu->id, why, desc);
}

void
vmm_test_run_expect_ucall(struct vmm_test_vcpu *vcpu, uint64_t cmd,
    struct vmm_ucall *uc)
{
	char why[64];

	(void)vmm_test_vcpu_run(vcpu);
	if (vcpu->ucall.cmd != cmd) {
		(void)snprintf(why, sizeof(why), "expected %s ucall",
		    ucall_name(cmd));
		vmm_test_fail_exit(vcpu, why);
	}
	if (uc != NULL)
		*uc = vcpu->ucall;
}

void
vmm_test_run_until_done(struct vmm_test_vcpu *vcpu)
{

	vmm_test_run_expect_ucall(vcpu, UCALL_DONE, NULL);
}

struct vm_exit *
vmm_test_run_expect_exit(struct vmm_test_vcpu *vcpu, enum vm_exitcode exitcode)
{
	char why[64];

	(void)vmm_test_vcpu_run(vcpu);
	if (vcpu->ucall.cmd != UCALL_NONE || vcpu->exit.exitcode != exitcode) {
		(void)snprintf(why, sizeof(why), "expected %s exit",
		    vmm_test_md_exitcode_name(exitcode));
		vmm_test_fail_exit(vcpu, why);
	}
	return (&vcpu->exit);
}
