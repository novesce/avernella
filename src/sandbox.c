// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Worker confinement and memory hardening with plain kernel and glibc
 * interfaces, no helper process.
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
 * The rest makes a memory bug harder to turn into running code:
 *
 * - MDWE refuses any mapping that is writable and executable, or that
 *   becomes executable later, so injected bytes never run. Nothing in
 *   the worker generates code: CUDA, VA-API, libvpx and PipeWire work.
 * - mseal() fixes the code, constants and RELRO (with the GOT, as the
 *   program links with -z now) of every object loaded at start-up, so
 *   mprotect() cannot make them writable and munmap() or mremap() cannot
 *   replace them.
 * - The x86 shadow stack keeps a second copy of return addresses that
 *   ordinary stores cannot reach, so an overwritten return address stops
 *   the process. glibc turns it on at start-up, so the UI asks for it in
 *   GLIBC_TUNABLES when it starts the worker. arm64 MTE heap tagging is
 *   requested the same way.
 * - M_PERTURB makes glibc overwrite heap chunks it takes back (all but its
 *   small per-thread cache) and zero the ones it hands out, so freed
 *   replies and messages do not linger and a use after free reads junk
 *   pointers that fault.
 * - LINE replies are decoded from a read-only copy that ends at a guard
 *   page (line_guard_copy()).
 *
 * Files are not restricted: the worker saves attachments wherever the user
 * chooses. Every layer is optional and the worker runs without any.
 */
#include "sandbox.h"
#include "security/sandbox.h"
#include <errno.h>
#include <gnu/libc-version.h>
#include <link.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <malloc.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
#define LINE_LANDLOCK_SCOPE_ABSTRACT_UNIX (1ULL << 0)
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
		attr.scoped = LINE_LANDLOCK_SCOPE_SIGNAL |
			      LINE_LANDLOCK_SCOPE_ABSTRACT_UNIX;
		layers |= LINE_SANDBOX_LANDLOCK_SIGNAL |
			  LINE_SANDBOX_LANDLOCK_ABSTRACT;
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

/* Declared here so older kernel and libc headers still build. */
#define LINE_PR_SET_MDWE 65
#define LINE_PR_GET_MDWE 66
#define LINE_MDWE_REFUSE_EXEC_GAIN (1UL << 0)
#define LINE_MDWE_NO_INHERIT (1UL << 1)
#define LINE_MADV_GUARD_INSTALL 102
#define LINE_ARCH_SHSTK_STATUS 0x5005
#define LINE_ARCH_SHSTK_SHSTK (1ULL << 0)
#define LINE_PR_GET_TAGGED_ADDR_CTRL 56
#define LINE_PR_MTE_TCF_MASK (3UL << 1)
#define LINE_HWCAP2_MTE (1UL << 18)
/* Freed chunks become 0xff, so a stale pointer read from one faults;
 * glibc fills new chunks with the inverse, zero.
 */
#define LINE_HEAP_PERTURB 0xff

#ifndef SYS_mseal
#define SYS_mseal 462
#endif

static const struct {
	const char *name;
	guint layers;
} switches[] = {
	{ "seccomp", LINE_SANDBOX_SECCOMP },
	{ "landlock", LINE_SANDBOX_LANDLOCK_NET | LINE_SANDBOX_LANDLOCK_SIGNAL |
			      LINE_SANDBOX_LANDLOCK_FS |
			      LINE_SANDBOX_LANDLOCK_ABSTRACT },
	{ "landlock-fs", LINE_SANDBOX_LANDLOCK_FS },
	{ "mdwe", LINE_SANDBOX_MDWE },
	{ "seal", LINE_SANDBOX_SEAL },
	{ "shstk", LINE_SANDBOX_SHADOW_STACK },
	{ "mte", LINE_SANDBOX_MEMORY_TAGGING },
	{ "wipe", LINE_SANDBOX_HEAP_WIPE },
	{ "guard", LINE_SANDBOX_GUARD_PAGES },
};

/* LINE_DISABLE_SANDBOX: "1" (or anything else) for all, or a list. */
static guint switched_off(void)
{
	const char *value = getenv("LINE_DISABLE_SANDBOX");
	guint off = 0;

	if (!value || !*value || !strcmp(value, "0"))
		return 0;
	g_auto(GStrv) names = g_strsplit(value, ",", -1);

	for (gsize i = 0; names[i]; i++) {
		gsize j = 0;

		while (j < G_N_ELEMENTS(switches) &&
		       strcmp(g_strstrip(names[i]), switches[j].name))
			j++;
		if (j == G_N_ELEMENTS(switches))
			return G_MAXUINT;
		off |= switches[j].layers;
	}
	return off;
}

static gboolean enabled(const char *name)
{
	const char *value = getenv(name);

	return value && *value && strcmp(value, "0");
}

static gboolean glibc_at_least(int major, int minor)
{
	int have_major = 0, have_minor = 0;

	if (sscanf(gnu_get_libc_version(), "%d.%d", &have_major, &have_minor) !=
	    2)
		return FALSE;
	return have_major > major ||
	       (have_major == major && have_minor >= minor);
}

static guint mdwe(gboolean inherit)
{
	unsigned long flags = LINE_MDWE_REFUSE_EXEC_GAIN;

	if (!inherit)
		flags |= LINE_MDWE_NO_INHERIT;
	/* Linux 6.3 to 6.6 refuse NO_INHERIT; then the caller goes without. */
	prctl(LINE_PR_SET_MDWE, flags, 0, 0, 0);
	int now = prctl(LINE_PR_GET_MDWE, 0, 0, 0, 0);

	if (now < 0 || !(now & LINE_MDWE_REFUSE_EXEC_GAIN))
		return 0;
	if (!inherit && !(now & LINE_MDWE_NO_INHERIT))
		return 0;
	return LINE_SANDBOX_MDWE;
}

typedef ElfW(Phdr) line_phdr;

struct seal_walk {
	uintptr_t page, vdso;
	guint sealed, failed;
};

static void seal_range(struct seal_walk *w, uintptr_t start, uintptr_t end)
{
	start &= ~(w->page - 1);
	if (end <= start)
		return;
	if (syscall(SYS_mseal, start, end - start, 0UL))
		w->failed++;
	else
		w->sealed++;
}

static int seal_object(struct dl_phdr_info *info, size_t size, void *data)
{
	struct seal_walk *w = data;

	(void)size;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const line_phdr *ph = &info->dlpi_phdr[i];
		uintptr_t start = info->dlpi_addr + ph->p_vaddr;

		/* The vDSO belongs to the kernel, which may seal it itself. */
		if (ph->p_type == PT_LOAD && w->vdso >= start &&
		    w->vdso < start + ph->p_memsz)
			return 0;
	}
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const line_phdr *ph = &info->dlpi_phdr[i];
		uintptr_t start = info->dlpi_addr + ph->p_vaddr,
			  end = start + ph->p_memsz;

		if (ph->p_type == PT_LOAD && !(ph->p_flags & PF_W))
			seal_range(w, start,
				   (end + w->page - 1) & ~(w->page - 1));
		/* glibc protects RELRO up to the last whole page. */
		else if (ph->p_type == PT_GNU_RELRO)
			seal_range(w, start, end & ~(w->page - 1));
	}
	return 0;
}

/*
 * Seals what is loaded now, which must not include a library that will be
 * unloaded: munmap() of a sealed range fails. Libraries loaded later (NSS
 * modules, CUDA, VA-API drivers, the renderer) stay unsealed.
 */
static guint seal_loaded(guint *count)
{
	struct seal_walk w = { .page = (uintptr_t)sysconf(_SC_PAGESIZE),
			       .vdso = getauxval(AT_SYSINFO_EHDR) };

	*count = 0;
	/* Linux 6.10 and later accept an empty range. */
	if (syscall(SYS_mseal, 0UL, 0UL, 0UL))
		return 0;
	dl_iterate_phdr(seal_object, &w);
	*count = w.sealed;
	return w.sealed && !w.failed ? LINE_SANDBOX_SEAL : 0;
}

static guint shadow_stack_active(void)
{
#if defined(__x86_64__) && !defined(__ILP32__)
	unsigned long long features = 0;

	if (!syscall(SYS_arch_prctl, LINE_ARCH_SHSTK_STATUS, &features) &&
	    (features & LINE_ARCH_SHSTK_SHSTK))
		return LINE_SANDBOX_SHADOW_STACK;
#endif
	return 0;
}

static guint memory_tagging_active(void)
{
#ifdef __aarch64__
	int control = prctl(LINE_PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0);

	if (control > 0 && (control & LINE_PR_MTE_TCF_MASK))
		return LINE_SANDBOX_MEMORY_TAGGING;
#endif
	return 0;
}

/*
 * The CPU and kernel both support user shadow stacks: the kernel lists
 * user_shstk only then (not with nousershstk or a CPU without CET).
 * glibc must be 2.39 or later, which uses the upstream kernel interface.
 */
static gboolean shadow_stack_supported(void)
{
#if defined(__x86_64__) && !defined(__ILP32__)
	unsigned long long features = 0;
	char *line = NULL;
	size_t size = 0;
	gboolean found = FALSE;

	if (!glibc_at_least(2, 39) ||
	    syscall(SYS_arch_prctl, LINE_ARCH_SHSTK_STATUS, &features))
		return FALSE;
	FILE *cpuinfo = fopen("/proc/cpuinfo", "re");

	if (!cpuinfo)
		return FALSE;
	while (getline(&line, &size, cpuinfo) > 0) {
		if (strncmp(line, "flags", 5))
			continue;
		found = strstr(line, " user_shstk") != NULL;
		break;
	}
	free(line);
	fclose(cpuinfo);
	return found;
#else
	return FALSE;
#endif
}

static gboolean memory_tagging_supported(void)
{
#ifdef __aarch64__
	return (getauxval(AT_HWCAP2) & LINE_HWCAP2_MTE) &&
	       glibc_at_least(2, 33);
#else
	return FALSE;
#endif
}

guint line_sandbox_worker_spawn_layers(void)
{
	guint off = switched_off(), layers = 0;

	if (!(off & LINE_SANDBOX_SHADOW_STACK) && shadow_stack_supported())
		layers |= LINE_SANDBOX_SHADOW_STACK;
	if (!(off & LINE_SANDBOX_MEMORY_TAGGING) && memory_tagging_supported())
		layers |= LINE_SANDBOX_MEMORY_TAGGING;
	return layers;
}

/*
 * glibc 2.39 to 2.44 drop the shadow stack from the CPU features because
 * the kernel has not enabled it yet at exec, and then ignore x86_shstk=on
 * unless hwcaps names SHSTK again. "on" rather than "permissive": several
 * codec libraries are not CET-marked, and they still run correctly with a
 * shadow stack as long as they never switch stacks themselves.
 * glibc.mem.tagging=1 tags malloc chunks with asynchronous checks.
 */
char *line_sandbox_tunables(const char *current, guint layers)
{
	g_auto(GStrv) parts =
		g_strsplit(current && *current ? current : "", ":", -1);
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
	gboolean changed = FALSE, shstk = TRUE, hwcaps = FALSE, tagging = TRUE;

	for (gsize i = 0; parts[i]; i++) {
		if (g_str_has_prefix(parts[i], "glibc.cpu.x86_shstk="))
			shstk = FALSE;
		if (g_str_has_prefix(parts[i], "glibc.mem.tagging="))
			tagging = FALSE;
	}
	shstk = shstk && (layers & LINE_SANDBOX_SHADOW_STACK);
	tagging = tagging && (layers & LINE_SANDBOX_MEMORY_TAGGING);
	for (gsize i = 0; parts[i]; i++) {
		if (!*parts[i])
			continue;
		if (shstk && g_str_has_prefix(parts[i], "glibc.cpu.hwcaps=")) {
			const char *value =
				parts[i] + strlen("glibc.cpu.hwcaps=");

			hwcaps = TRUE;
			g_ptr_array_add(out,
					g_strconcat(parts[i], *value ? "," : "",
						    "SHSTK", NULL));
			continue;
		}
		g_ptr_array_add(out, g_strdup(parts[i]));
	}
	if (shstk) {
		if (!hwcaps)
			g_ptr_array_add(out,
					g_strdup("glibc.cpu.hwcaps=SHSTK"));
		g_ptr_array_add(out, g_strdup("glibc.cpu.x86_shstk=on"));
		changed = TRUE;
	}
	if (tagging) {
		g_ptr_array_add(out, g_strdup("glibc.mem.tagging=1"));
		changed = TRUE;
	}
	if (!changed)
		return NULL;
	g_ptr_array_add(out, NULL);
	return g_strjoinv(":", (char **)out->pdata);
}

static gboolean guard_wanted(void)
{
	static gint wanted;
	gint now = g_atomic_int_get(&wanted);

	if (!now) {
		now = switched_off() & LINE_SANDBOX_GUARD_PAGES ? 1 : 2;
		g_atomic_int_set(&wanted, now);
	}
	return now == 2;
}

static gboolean guard(void *page, gsize size)
{
	if (!madvise(page, size, LINE_MADV_GUARD_INSTALL))
		return TRUE;
	/* Before Linux 6.13: a separate inaccessible mapping. */
	return !mprotect(page, size, PROT_NONE);
}

gboolean line_guard_copy(struct line_guarded *g, const void *data, gsize n)
{
	gsize page = (gsize)sysconf(_SC_PAGESIZE);
	gsize body = n ? (n + page - 1) & ~(page - 1) : page;

	*g = (struct line_guarded){ 0 };
	if (!guard_wanted() || n > G_MAXSIZE - 3 * page)
		return FALSE;
	guint8 *map = mmap(NULL, body + 2 * page, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (map == MAP_FAILED)
		return FALSE;
	guint8 *start = map + page + body - n;

	if (!guard(map, page) || !guard(map + page + body, page)) {
		munmap(map, body + 2 * page);
		return FALSE;
	}
	if (n)
		memcpy(start, data, n);
	/* A parser that writes into its input faults too. */
	mprotect(map + page, body, PROT_READ);
	*g = (struct line_guarded){ .data = start,
				    .size = n,
				    .map = map,
				    .map_size = body + 2 * page };
	return TRUE;
}

void line_guard_free(struct line_guarded *g)
{
	/* The kernel zeroes the pages before it reuses them. */
	if (g->map)
		munmap(g->map, g->map_size);
	*g = (struct line_guarded){ 0 };
}

static void report(const char *who, guint layers, guint sealed)
{
	if (!enabled("LINE_DEBUG_SANDBOX"))
		return;
	g_printerr(
		"line-sandbox %s no-dump=%d no-new-privs=%d seccomp=%d landlock-net=%d landlock-signal=%d mdwe=%d seal=%d(%u) shadow-stack=%d memory-tagging=%d heap-wipe=%d guard-pages=%d\n",
		who, !!(layers & LINE_SANDBOX_NO_DUMP),
		!!(layers & LINE_SANDBOX_NO_NEW_PRIVS),
		!!(layers & LINE_SANDBOX_SECCOMP),
		!!(layers & LINE_SANDBOX_LANDLOCK_NET),
		!!(layers & LINE_SANDBOX_LANDLOCK_SIGNAL),
		!!(layers & LINE_SANDBOX_MDWE), !!(layers & LINE_SANDBOX_SEAL),
		sealed, !!(layers & LINE_SANDBOX_SHADOW_STACK),
		!!(layers & LINE_SANDBOX_MEMORY_TAGGING),
		!!(layers & LINE_SANDBOX_HEAP_WIPE),
		!!(layers & LINE_SANDBOX_GUARD_PAGES));
}

guint line_sandbox_worker(void)
{
	guint off = switched_off(), layers = 0, sealed = 0;

	if (off == G_MAXUINT)
		return 0;
	if (!(off & LINE_SANDBOX_HEAP_WIPE) &&
	    mallopt(M_PERTURB, LINE_HEAP_PERTURB))
		layers |= LINE_SANDBOX_HEAP_WIPE;
	if (!prctl(PR_SET_DUMPABLE, 0, 0, 0, 0))
		layers |= LINE_SANDBOX_NO_DUMP;
	if (!(off & LINE_SANDBOX_MDWE))
		layers |= mdwe(TRUE);
	if (!(off & LINE_SANDBOX_SEAL))
		layers |= seal_loaded(&sealed);
	if (!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		layers |= LINE_SANDBOX_NO_NEW_PRIVS;
		if (!(off & LINE_SANDBOX_LANDLOCK_NET))
			layers |= landlock();
		if (!(off & LINE_SANDBOX_LANDLOCK_FS)) {
			const char *state_dir = getenv("LINE_STATE_DIR");
			const char *extra_rw[1];
			struct avernella_sandbox_opts fs_opts = {
				.role = SANDBOX_ROLE_UI,
				.allow_gpu_ioctl = true,
				.allow_camera = true,
			};
			uint32_t fs_active = 0;

			if (state_dir && state_dir[0] == '/') {
				extra_rw[0] = state_dir;
				fs_opts.extra_rw_paths = extra_rw;
				fs_opts.n_extra_rw_paths = 1;
			}
			if (avernella_landlock_apply(&fs_opts, &fs_active) ==
				    0 &&
			    (fs_active & AVSB_ACTIVE_LANDLOCK_FS))
				layers |= LINE_SANDBOX_LANDLOCK_FS;
		}
		if (!(off & LINE_SANDBOX_SECCOMP) && seccomp())
			layers |= LINE_SANDBOX_SECCOMP;
	}
	layers |= shadow_stack_active() | memory_tagging_active();
	if (!(off & LINE_SANDBOX_GUARD_PAGES))
		layers |= LINE_SANDBOX_GUARD_PAGES;
	report("worker", layers, sealed);
	return layers;
}

static guint ui_layers;

guint line_sandbox_ui_start(void)
{
	guint off = switched_off(), sealed = 0;

	if (off == G_MAXUINT)
		return 0;
	if (!(off & LINE_SANDBOX_SEAL))
		ui_layers |= seal_loaded(&sealed);
	ui_layers |= shadow_stack_active() | memory_tagging_active();
	report("ui", ui_layers, sealed);
	return ui_layers;
}

guint line_sandbox_ui_renderer(gboolean jit_free)
{
	guint off = switched_off();

	if (off == G_MAXUINT || !jit_free || (off & LINE_SANDBOX_MDWE) ||
	    (ui_layers & LINE_SANDBOX_MDWE))
		return ui_layers;
	ui_layers |= mdwe(FALSE);
	report("ui", ui_layers, 0);
	return ui_layers;
}

guint line_sandbox_ui_confine(void)
{
	guint off = switched_off();
	struct avernella_sandbox_opts opts = {
		.role = SANDBOX_ROLE_UI,
		.allow_gpu_ioctl = true,
	};
	uint32_t active = 0;

	if (off == G_MAXUINT || (off & LINE_SANDBOX_LANDLOCK_NET))
		return ui_layers;
	if (!prctl(PR_SET_DUMPABLE, 0, 0, 0, 0))
		ui_layers |= LINE_SANDBOX_NO_DUMP;
	if (!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		ui_layers |= LINE_SANDBOX_NO_NEW_PRIVS;
		if (avernella_landlock_apply(&opts, &active) == 0) {
			if (active & AVSB_ACTIVE_LANDLOCK_FS)
				ui_layers |= LINE_SANDBOX_LANDLOCK_FS;
			if (active & AVSB_ACTIVE_LANDLOCK_SCOPE)
				ui_layers |= LINE_SANDBOX_LANDLOCK_SIGNAL;
		}
	}
	report("ui", ui_layers, 0);
	return ui_layers;
}

guint line_sandbox_ui_layers(void)
{
	return ui_layers;
}
