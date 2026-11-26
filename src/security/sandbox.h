/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Linux kernel sandboxing & privilege separation for Avernella:
 *   - Runtime Landlock ABI probing (ABI 1..11: FS, TCP ports, IOCTL_DEV,
 *     RESOLVE_UNIX, abstract UNIX & signal scopes, TSYNC)
 *   - Unprivileged namespaces (USER/PID/NS/IPC/UTS, optional NET) +
 *     pivot_root() into an empty read-only tmpfs
 *   - libseccomp allowlist filters with SCMP_ACT_KILL_PROCESS and BPF
 *     PROT_EXEC + CLONE_NEW* argument guards
 *   - Pre-sandbox FD sanitization and deterministic worker spawning
 */
#ifndef AVERNELLA_SECURITY_SANDBOX_H
#define AVERNELLA_SECURITY_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum avernella_sandbox_role {
	SANDBOX_ROLE_VAULT =
		0, /* Supervisor/Key Vault: holds secrets, no untrusted parsing */
	SANDBOX_ROLE_NET =
		1, /* LEGY/Thrift worker: empty RO root, 0-FS Landlock, TCP 443 */
	SANDBOX_ROLE_MEDIA =
		2, /* PipeWire/Opus/VP8/SRTP worker: scoped HW/PW, 0 general FS */
	SANDBOX_ROLE_UI =
		3, /* SDL2/Vulkan UI: RO system assets, RW Avernella XDG only */
};

struct avernella_sandbox_caps {
	int landlock_abi; /* >= 1 if Landlock available, 0 otherwise */
	bool has_memfd_secret; /* SYS_memfd_secret supported */
	bool has_userns; /* unprivileged CLONE_NEWUSER available */
	bool has_mdwe; /* PR_SET_MDWE supported */
	bool has_seccomp; /* seccomp-bpf supported */
};

struct avernella_sandbox_opts {
	enum avernella_sandbox_role role;
	bool isolate_mount_ns; /* pivot_root() to empty RO tmpfs (NET role) */
	bool isolate_net_ns; /* CLONE_NEWNET (all sockets brokered via SCM_RIGHTS) */
	bool allow_gpu_ioctl; /* allow /dev/dri ioctls in MEDIA/UI */
	bool allow_camera; /* allow /dev/video* in MEDIA */
	bool keep_stderr; /* retain fd 2 in spawned worker (debug/tests) */
	const uint16_t *tcp_connect_ports;
	size_t n_tcp_connect_ports;
	const char *const *extra_ro_paths;
	size_t n_extra_ro_paths;
	const char *const *extra_rw_paths;
	size_t n_extra_rw_paths;
};

/* Active confinement bitmask returned by avernella_sandbox_apply(). */
enum avernella_sandbox_active {
	AVSB_ACTIVE_NO_DUMP = 1U << 0,
	AVSB_ACTIVE_NO_NEW_PRIVS = 1U << 1,
	AVSB_ACTIVE_MDWE = 1U << 2,
	AVSB_ACTIVE_EMPTY_ROOT = 1U << 3,
	AVSB_ACTIVE_NET_NS = 1U << 4,
	AVSB_ACTIVE_LANDLOCK_FS = 1U << 5,
	AVSB_ACTIVE_LANDLOCK_NET = 1U << 6,
	AVSB_ACTIVE_LANDLOCK_SCOPE = 1U << 7,
	AVSB_ACTIVE_SECCOMP = 1U << 8,
};

struct avernella_worker_proc {
	pid_t pid;
	int pidfd;
	int ctrl_fd; /* Parent end of SOCK_SEQPACKET control channel */
};

/* Probes kernel sandboxing features at runtime without modifying state. */
void avernella_sandbox_probe(struct avernella_sandbox_caps *caps);

/*
 * Enters unprivileged USER/NS/IPC/UTS (and optionally NET) namespaces and
 * pivots into an empty read-only tmpfs with the old root unmounted.
 * Must be called while single-threaded, prior to Landlock/seccomp.
 * Returns 0 on success or -errno on failure.
 */
int avernella_enter_empty_root_ns(bool isolate_net);

/*
 * Applies Landlock FS, TCP port, and IPC/signal scope rules tailored to the
 * role using the maximum ABI supported by the running kernel (ABI 1..11).
 * Sets active_mask bits for each enforced subsystem.
 * Returns 0 on success or -errno on failure.
 */
int avernella_landlock_apply(const struct avernella_sandbox_opts *opts,
			     uint32_t *active_mask);

/*
 * Installs a libseccomp allowlist filter (SCMP_ACT_KILL_PROCESS default) with
 * BPF argument checks banning PROT_EXEC on mmap/mprotect and CLONE_NEW* on
 * clone/clone3. Returns 0 on success or -errno on failure.
 */
int avernella_seccomp_apply(const struct avernella_sandbox_opts *opts);

/*
 * Applies the full ordered confinement stack for opts->role:
 *   1. PR_SET_DUMPABLE(0) + PR_SET_MDWE
 *   2. Optional namespaces + pivot_root() empty RO tmpfs
 *   3. PR_SET_NO_NEW_PRIVS(1)
 *   4. Landlock (FS + TCP ports + scopes)
 *   5. libseccomp SCMP_ACT_KILL_PROCESS allowlist
 * Returns 0 on success or -errno; active_mask records every active layer.
 */
int avernella_sandbox_apply(const struct avernella_sandbox_opts *opts,
			    uint32_t *active_mask);

/*
 * Forks a clean child worker with a dedicated SOCK_SEQPACKET pair, strips all
 * inherited FDs via close_range(2), applies avernella_sandbox_apply(opts),
 * and invokes entry(child_ctrl_fd, active_mask, userdata).
 * Never returns in the child. Parent receives pid, pidfd, and ctrl_fd.
 */
typedef int (*avernella_worker_fn)(int ctrl_fd, uint32_t active_mask,
				   void *userdata);

int avernella_spawn_worker(const struct avernella_sandbox_opts *opts,
			   avernella_worker_fn entry, void *userdata,
			   struct avernella_worker_proc *out);

void avernella_worker_close(struct avernella_worker_proc *proc);

#endif /* AVERNELLA_SECURITY_SANDBOX_H */
