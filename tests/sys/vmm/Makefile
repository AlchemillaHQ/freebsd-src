# Guest code is compiled into the test programs.  CPUTYPE must not enable
# extensions beyond the SSE (amd64) or FP/Advanced SIMD (arm64) guest setup.
# This has to be set before bsd.cpu.mk is included.
NO_CPU_CFLAGS=
# Sanitizer instrumentation calls into runtimes the guest cannot reach.
MK_ASAN:=	no
MK_UBSAN:=	no

.include <src.opts.mk>

PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/vmm

BINDIR=		${TESTSDIR}

ATF_TESTS_SH+=	vmm_cred_jail

${PACKAGE}FILES+=	utils.subr

.if ${MK_BHYVE} != "no" && \
    (${MACHINE_CPUARCH} == "amd64" || ${MACHINE_CPUARCH} == "aarch64")
.PATH:		${.CURDIR}/${MACHINE_CPUARCH}

ATF_TESTS_C+=	harness_test
ATF_TESTS_C+=	harness_mem_test

# Every program links the harness, whose objects bsd.progs.mk builds once.
.for t in ${ATF_TESTS_C}
SRCS.${t}=	${t}.c vmm_test.c vmm_test_guest.c vmm_test_machdep.c \
		vmm_test_guest_machdep.c vmm_test_exception.S
.if ${MACHINE_CPUARCH} == "aarch64"
SRCS.${t}+=	vmm_test_boot.S
.endif
TEST_METADATA.${t}+=	required_kmods="vmm"
TEST_METADATA.${t}+=	required_user="root"
TEST_METADATA.${t}+=	timeout=30
.endfor

# The harness's own tests share support code.
.for t in ${ATF_TESTS_C:Mharness_*}
SRCS.${t}+=	harness_util.c
.endfor

.if ${MACHINE_CPUARCH} == "amd64"
# Guest exceptions are taken on the interrupted code's stack.
CFLAGS+=	-mno-red-zone
.endif

CFLAGS+=	-I${.CURDIR} -I${.CURDIR}/${MACHINE_CPUARCH}
LIBADD+=	vmmapi

# Guest code runs from a copy of the test binary, where libc and with it the
# stack protector's canary are not mapped.  Fortified string functions would
# call into libc too, and would replace the harness's memset() and memcpy().
SSP_CFLAGS=
FORTIFY_SOURCE=	0
.endif

.include <bsd.test.mk>
