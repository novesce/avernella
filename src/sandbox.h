/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Kernel confinement for the worker, which holds the tokens and keys and
 * parses everything that arrives from the network.
 */
#ifndef LINE_SANDBOX_H
#define LINE_SANDBOX_H

#include <glib.h>

enum line_sandbox_layer {
	/* PR_SET_DUMPABLE 0: no ptrace, /proc/<pid> access or core dump. */
	LINE_SANDBOX_NO_DUMP = 1 << 0,
	/* PR_SET_NO_NEW_PRIVS: no privilege gain through exec. */
	LINE_SANDBOX_NO_NEW_PRIVS = 1 << 1,
	/* seccomp: no exec, namespaces, ptrace, io_uring, BPF, tty typing. */
	LINE_SANDBOX_SECCOMP = 1 << 2,
	/* Landlock ABI 4 (Linux 6.7): no TCP listening sockets. */
	LINE_SANDBOX_LANDLOCK_NET = 1 << 3,
	/* Landlock ABI 6 (Linux 6.12): no signals to outside processes. */
	LINE_SANDBOX_LANDLOCK_SIGNAL = 1 << 4,
};

/*
 * Applies every layer the kernel offers and returns the ones in force.
 * Call before starting threads. Each layer is optional: a kernel or an
 * outer sandbox (Flatpak, a container) that refuses one leaves the others.
 * LINE_DISABLE_SANDBOX=1 applies nothing, for a debugger;
 * LINE_DEBUG_SANDBOX=1 prints the result.
 */
guint line_sandbox_worker(void);

#endif
