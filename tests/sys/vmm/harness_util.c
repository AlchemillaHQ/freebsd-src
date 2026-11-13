/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#include <sys/param.h>
#include <sys/wait.h>

#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#include "harness_util.h"

static int failure_fd = -1;

/* Pass the message to the parent instead of failing the child. */
static void
send_failure(const char *msg)
{

	(void)write(failure_fd, msg, strlen(msg));
	_exit(0);
}

void
harness_expect_failure(vmm_test_guest_fn_t *guest, harness_host_fn_t *host,
    const char *const *expected)
{
	struct vmm_test_vcpu *vcpu;
	char msg[512];
	size_t len;
	ssize_t n;
	pid_t pid;
	int fds[2], status;

	ATF_REQUIRE(pipe(fds) == 0);
	pid = fork();
	ATF_REQUIRE(pid != -1);
	if (pid == 0) {
		(void)close(fds[0]);
		failure_fd = fds[1];
		vmm_test_fail_hook = send_failure;
		(void)vmm_test_vm_create_with_vcpu(guest, &vcpu);
		if (host != NULL)
			host(vcpu);
		else
			vmm_test_run_until_done(vcpu);
		_exit(1);
	}
	(void)close(fds[1]);
	for (len = 0; len < sizeof(msg) - 1; len += n) {
		n = read(fds[0], msg + len, sizeof(msg) - 1 - len);
		ATF_REQUIRE(n != -1);
		if (n == 0)
			break;
	}
	msg[len] = '\0';
	(void)close(fds[0]);
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	ATF_REQUIRE_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "the harness did not report a failure (status %#x)", status);
	for (; *expected != NULL; expected++)
		ATF_CHECK_MSG(strstr(msg, *expected) != NULL,
		    "\"%s\" not found in \"%s\"", *expected, msg);
}
