// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Worker confinement with plain kernel interfaces, no helper process.
 *
 * The worker keeps the tokens and keys and parses LINE replies, pictures
 * and call media, so it is where an exploit would land. After start-up it
 * never needs to run a program, create namespaces, trace or read other
 * processes, load BPF or use io_uring, and it never listens on TCP. Each
 * of these is a common step from a memory bug to a persistent foothold.
 *
 * - PR_SET_DUMPABLE 0 keeps other processes of the same user out of
 *   /proc/<pid>/mem, fd and environ, stops ptrace without CAP_SYS_PTRACE,
 *   and writes no core dump that would hold the session. Media leases
 *   reach the UI as passed descriptors because of this.
 * - A seccomp filter refuses those system calls, and the TIOCSTI and
 *   TIOCLINUX ioctls that type into a terminal, with an errno, so a
 *   library that probes one sees an ordinary failure and carries on.
 *   clone3() reports ENOSYS: its flags are out of the filter's reach, and
 *   glibc then uses clone(), whose namespace flags the filter checks.
 * - Landlock removes TCP listening sockets and signals to processes
 *   outside the worker, where the kernel has those rules.
 *
 * Files are not restricted: the worker saves attachments wherever the user
 * chooses. Every layer is optional and the worker runs without any.
 */
#include "sandbox.h"
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__) && !defined(__ILP32__)
#define LINE_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LINE_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

/* Landlock, declared here so older kernel headers still build. */
struct line_landlock_ruleset_attr {
	guint64 handled_access_fs, handled_access_net, scoped;
};

#define LINE_LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#define LINE_LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LINE_LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#define LINE_LANDLOCK_RESTRICT_SELF_TSYNC (1U << 3)

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_restrict_self 446
#endif

static guint landlock(void)
{
	long abi = syscall(SYS_landlock_create_ruleset, NULL, 0,
			   LINE_LANDLOCK_CREATE_RULESET_VERSION);
	struct line_landlock_ruleset_attr attr = { 0 };
	guint layers = 0;

	if (abi < 4)
		return 0;
	attr.handled_access_net = LINE_LANDLOCK_ACCESS_NET_BIND_TCP;
	layers |= LINE_SANDBOX_LANDLOCK_NET;
	if (abi >= 6) {
		attr.scoped = LINE_LANDLOCK_SCOPE_SIGNAL;
		layers |= LINE_SANDBOX_LANDLOCK_SIGNAL;
	}
	/* No rules: nothing handled is allowed. */
	int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr,
				   sizeof(attr), 0);

	if (ruleset < 0)
		return 0;
	/* Cover threads a library may have started; older ABIs cannot. */
	long done = syscall(SYS_landlock_restrict_self, ruleset,
			    LINE_LANDLOCK_RESTRICT_SELF_TSYNC);

	if (done && errno == EINVAL)
		done = syscall(SYS_landlock_restrict_self, ruleset, 0);
	close(ruleset);
	return done ? 0 : layers;
}

#ifdef LINE_AUDIT_ARCH
struct refusal {
	int nr, err;
};

static const struct refusal refusals[] = {
	/* Running programs. */
	{ SYS_execve, EPERM },
	{ SYS_execveat, EPERM },
	/* Other processes. */
	{ SYS_ptrace, EPERM },
	{ SYS_process_vm_readv, EPERM },
	{ SYS_process_vm_writev, EPERM },
#ifdef SYS_pidfd_getfd
	{ SYS_pidfd_getfd, EPERM },
#endif
#ifdef SYS_process_madvise
	{ SYS_process_madvise, EPERM },
#endif
	/* Namespaces and mounts, the usual way into kernel attack surface. */
	{ SYS_unshare, EPERM },
	{ SYS_setns, EPERM },
	{ SYS_mount, EPERM },
	{ SYS_umount2, EPERM },
	{ SYS_pivot_root, EPERM },
	{ SYS_chroot, EPERM },
#ifdef SYS_open_tree
	{ SYS_open_tree, EPERM },
	{ SYS_move_mount, EPERM },
	{ SYS_fsopen, EPERM },
	{ SYS_fsconfig, EPERM },
	{ SYS_fsmount, EPERM },
	{ SYS_fspick, EPERM },
#endif
#ifdef SYS_mount_setattr
	{ SYS_mount_setattr, EPERM },
#endif
	{ SYS_open_by_handle_at, EPERM },
	{ SYS_name_to_handle_at, EPERM },
	/* Large kernel interfaces the worker never uses. */
	{ SYS_bpf, EPERM },
	{ SYS_perf_event_open, EPERM },
	{ SYS_userfaultfd, EPERM },
	{ SYS_io_uring_setup, ENOSYS },
	{ SYS_io_uring_enter, ENOSYS },
	{ SYS_io_uring_register, ENOSYS },
	{ SYS_clone3, ENOSYS },
	/* Administration. */
	{ SYS_kexec_load, EPERM },
#ifdef SYS_kexec_file_load
	{ SYS_kexec_file_load, EPERM },
#endif
	{ SYS_init_module, EPERM },
	{ SYS_finit_module, EPERM },
	{ SYS_delete_module, EPERM },
	{ SYS_reboot, EPERM },
	{ SYS_swapon, EPERM },
	{ SYS_swapoff, EPERM },
	{ SYS_acct, EPERM },
	{ SYS_settimeofday, EPERM },
	{ SYS_clock_settime, EPERM },
	{ SYS_clock_adjtime, EPERM },
	{ SYS_adjtimex, EPERM },
	{ SYS_syslog, EPERM },
	{ SYS_quotactl, EPERM },
	{ SYS_vhangup, EPERM },
#ifdef SYS_iopl
	{ SYS_iopl, EPERM },
	{ SYS_ioperm, EPERM },
#endif
};

#define NAMESPACE_FLAGS                                                \
	(CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | \
	 CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)

static gboolean seccomp(void)
{
	g_autoptr(GArray) code =
		g_array_new(FALSE, FALSE, sizeof(struct sock_filter));
	struct sock_filter head[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, arch)),
		/* Another ABI (i386 int 0x80, compat) has other numbers. */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, LINE_AUDIT_ARCH, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
#ifdef __x86_64__
		/* x32 calls share the numbers with a high bit. */
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
#endif
	};
	struct sock_filter tail[] = {
		/*
		 * Typing into a terminal the worker inherited (stderr) would
		 * run commands outside the sandbox. The kernel reads the
		 * request as 32 bits, so the low word is the whole of it.
		 */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 5),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, args[1])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TIOCSTI, 2, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TIOCLINUX, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 3),
		/* The low word of the flags (little-endian). */
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, args[0])),
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, NAMESPACE_FLAGS, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};

	g_array_append_vals(code, head, G_N_ELEMENTS(head));
	for (gsize i = 0; i < G_N_ELEMENTS(refusals); i++) {
		struct sock_filter rule[] = {
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
				 (guint32)refusals[i].nr, 0, 1),
			BPF_STMT(BPF_RET | BPF_K,
				 SECCOMP_RET_ERRNO | refusals[i].err),
		};
		g_array_append_vals(code, rule, G_N_ELEMENTS(rule));
	}
	g_array_append_vals(code, tail, G_N_ELEMENTS(tail));
	struct sock_fprog program = {
		.len = (unsigned short)code->len,
		.filter = (struct sock_filter *)code->data,
	};

	/* Every thread at once where the kernel can; else this one. */
	if (!syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		     SECCOMP_FILTER_FLAG_TSYNC, &program))
		return TRUE;
	return !prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0);
}
#else
static gboolean seccomp(void)
{
	return FALSE;
}
#endif

static gboolean enabled(const char *name)
{
	const char *value = getenv(name);

	return value && *value && strcmp(value, "0");
}

guint line_sandbox_worker(void)
{
	guint layers = 0;

	if (enabled("LINE_DISABLE_SANDBOX"))
		return 0;
	if (!prctl(PR_SET_DUMPABLE, 0, 0, 0, 0))
		layers |= LINE_SANDBOX_NO_DUMP;
	if (!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		layers |= LINE_SANDBOX_NO_NEW_PRIVS;
		layers |= landlock();
		if (seccomp())
			layers |= LINE_SANDBOX_SECCOMP;
	}
	if (enabled("LINE_DEBUG_SANDBOX"))
		g_printerr(
			"line-sandbox no-dump=%d no-new-privs=%d seccomp=%d landlock-net=%d landlock-signal=%d\n",
			!!(layers & LINE_SANDBOX_NO_DUMP),
			!!(layers & LINE_SANDBOX_NO_NEW_PRIVS),
			!!(layers & LINE_SANDBOX_SECCOMP),
			!!(layers & LINE_SANDBOX_LANDLOCK_NET),
			!!(layers & LINE_SANDBOX_LANDLOCK_SIGNAL));
	return layers;
}
