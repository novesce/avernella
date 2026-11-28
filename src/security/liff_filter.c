// SPDX-License-Identifier: GPL-3.0-or-later
#include "liff_filter.h"
#include <errno.h>
#include <seccomp.h>
#include <stddef.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

int line_liff_filter_apply(void)
{
	static const struct {
		int syscall_nr;
		int error;
	} blocked[] = {
		/* Cross-process memory and descriptor access. */
		{ SCMP_SYS(ptrace), EPERM },
		{ SCMP_SYS(process_vm_readv), EPERM },
		{ SCMP_SYS(process_vm_writev), EPERM },
		{ SCMP_SYS(pidfd_getfd), EPERM },
		/* Kernel interfaces the browser does not need. */
		{ SCMP_SYS(bpf), EPERM },
		{ SCMP_SYS(perf_event_open), EPERM },
		{ SCMP_SYS(userfaultfd), EPERM },
		/* Let libraries fall back to their ordinary I/O path. */
		{ SCMP_SYS(io_uring_setup), ENOSYS },
		{ SCMP_SYS(io_uring_enter), ENOSYS },
		{ SCMP_SYS(io_uring_register), ENOSYS },
	};
	scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
	int rc;

	if (!ctx)
		return -ENOMEM;

	rc = seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1);
	if (rc < 0)
		goto out;

	/* WebKit continues to manage namespaces and its own child sandbox. */
	for (size_t i = 0; i < ARRAY_SIZE(blocked); i++) {
		rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(blocked[i].error),
				      blocked[i].syscall_nr, 0);
		if (rc < 0)
			goto out;
	}
	rc = seccomp_load(ctx);
out:
	seccomp_release(ctx);
	return rc;
}
