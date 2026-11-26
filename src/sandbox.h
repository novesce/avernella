/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Kernel confinement and memory hardening. The worker holds the tokens and
 * keys and parses everything that arrives from the network; the UI decodes
 * pictures and draws untrusted text.
 *
 * None of this makes C memory-safe. It turns memory corruption into a crash
 * more often, takes away the usual next steps of an exploit (new code,
 * rewriting return addresses or the GOT, running programs) and limits what
 * an exploited process can reach.
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
	/* PR_SET_MDWE (Linux 6.3): no memory that is or becomes W and X. */
	LINE_SANDBOX_MDWE = 1 << 5,
	/* mseal (Linux 6.10): code, constants and RELRO fixed in place. */
	LINE_SANDBOX_SEAL = 1 << 6,
	/* x86 user shadow stack (Linux 6.6, glibc 2.39, CET CPUs). */
	LINE_SANDBOX_SHADOW_STACK = 1 << 7,
	/* arm64 MTE heap tagging (Linux 5.10, glibc 2.33, MTE CPUs). */
	LINE_SANDBOX_MEMORY_TAGGING = 1 << 8,
	/* glibc M_PERTURB: freed heap chunks overwritten, new ones zeroed. */
	LINE_SANDBOX_HEAP_WIPE = 1 << 9,
	/* LINE replies decoded from a copy that ends at a guard page. */
	LINE_SANDBOX_GUARD_PAGES = 1 << 10,
	/* Landlock filesystem confinement (ABI 1..11). */
	LINE_SANDBOX_LANDLOCK_FS = 1 << 11,
	/* Landlock ABI 6 (Linux 6.12): no abstract UNIX socket escapes. */
	LINE_SANDBOX_LANDLOCK_ABSTRACT = 1 << 12,
};

/*
 * Applies every layer the kernel offers to the worker and returns the ones
 * in force. Call before starting threads or loading libraries. Each layer
 * is optional: a kernel, CPU or outer sandbox (Flatpak, a container) that
 * refuses one leaves the others.
 *
 * LINE_DISABLE_SANDBOX=1 applies nothing, for a debugger. A comma-separated
 * list of layer names (seccomp, landlock, mdwe, seal, shstk, mte, wipe,
 * guard) leaves out only those. LINE_DEBUG_SANDBOX=1 prints the result.
 */
guint line_sandbox_worker(void);

/*
 * The UI's share, in two steps. line_sandbox_ui_start() seals the program
 * and its startup libraries; call it first thing, before any dlopen().
 * line_sandbox_ui_renderer() turns on MDWE once the renderer is known to
 * need no JIT (a hardware Vulkan device, not llvmpipe or lavapipe); the
 * flag is not inherited, so the LIFF browser keeps its JavaScript JIT.
 * Both return the UI's layers so far, as does line_sandbox_ui_layers().
 */
guint line_sandbox_ui_start(void);
guint line_sandbox_ui_renderer(gboolean jit_free);
guint line_sandbox_ui_confine(void);
guint line_sandbox_ui_layers(void);

/*
 * Layers that must be chosen before the worker starts (shadow stack, MTE),
 * and the GLIBC_TUNABLES value that asks glibc for them. The result keeps
 * every setting in current and never overrides one it already makes;
 * NULL means current needs no change.
 */
guint line_sandbox_worker_spawn_layers(void);
char *line_sandbox_tunables(const char *current, guint layers);

/*
 * A read-only copy of untrusted input that ends exactly at a guard page, so
 * a parser that reads one byte too far faults instead of reading the
 * neighbouring heap. MADV_GUARD_INSTALL (Linux 6.13) or PROT_NONE pages.
 * Returns FALSE (and the caller parses the original) when switched off or
 * out of memory. Freeing unmaps it, so no copy of a reply stays behind.
 */
struct line_guarded {
	const guint8 *data;
	gsize size;
	void *map;
	gsize map_size;
};

gboolean line_guard_copy(struct line_guarded *g, const void *data, gsize n);
void line_guard_free(struct line_guarded *g);

#endif
