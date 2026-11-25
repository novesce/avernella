// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The worker sandbox and the memory hardening, applied in child processes:
 * what they refuse, and that everything the worker does keeps working.
 */
#include "../src/sandbox.h"
#include "../src/udp_batch.h"
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MDWE_GET 66
#define MDWE_REFUSE_EXEC_GAIN 1
#define MDWE_NO_INHERIT 2
#ifndef SYS_mseal
#define SYS_mseal 462
#endif

#if defined(__x86_64__) || defined(__aarch64__)
#define FILTERED TRUE
#else
#define FILTERED FALSE
#endif

static gboolean exec_true(void)
{
	char *const argv[] = { "true", NULL };
	pid_t pid = fork();
	int status;

	g_assert_cmpint(pid, >=, 0);
	if (!pid) {
		execve("/bin/true", argv, environ);
		_exit(errno == EPERM ? 42 : 43);
	}
	g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
	return WIFEXITED(status) && !WEXITSTATUS(status);
}

static gpointer thread_works(gpointer data)
{
	return data;
}

/* A thread that existed before the filter must be covered too. */
static GMutex early_lock;
static GCond early_cond;
static gboolean early_go;

static gpointer early_thread(gpointer data)
{
	(void)data;
	g_mutex_lock(&early_lock);
	while (!early_go)
		g_cond_wait(&early_cond, &early_lock);
	g_mutex_unlock(&early_lock);
	return GINT_TO_POINTER(syscall(SYS_ptrace, PTRACE_TRACEME, 0, 0, 0) <
				       0 &&
			       errno == EPERM);
}

static void refused(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	/* A listener made before the sandbox keeps working. */
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET,
				    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof(addr);

	g_assert_cmpint(bind(listener, (void *)&addr, sizeof(addr)), ==, 0);
	g_assert_cmpint(listen(listener, 1), ==, 0);
	g_assert_cmpint(getsockname(listener, (void *)&addr, &len), ==, 0);
	GThread *early = g_thread_new("early", early_thread, NULL);

	g_assert_cmpint(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0), ==, 0);
	/* What this kernel offers, asked before the sandbox. */
	gboolean has_mdwe = prctl(MDWE_GET, 0, 0, 0, 0) >= 0;
	gboolean has_mseal = !syscall(SYS_mseal, 0UL, 0UL, 0UL);
	guint layers = line_sandbox_worker();

	g_mutex_lock(&early_lock);
	early_go = TRUE;
	g_cond_signal(&early_cond);
	g_mutex_unlock(&early_lock);
	g_assert_true(layers & LINE_SANDBOX_NO_DUMP);
	g_assert_true(layers & LINE_SANDBOX_NO_NEW_PRIVS);
	g_assert_cmpint(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0), ==, 0);
	g_assert_cmpint(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0), ==, 1);
	if (FILTERED) {
		g_assert_true(layers & LINE_SANDBOX_SECCOMP);
		g_assert_true(g_thread_join(early));
		g_assert_false(exec_true());
		g_assert_cmpint(ptrace(PTRACE_TRACEME, 0, 0, 0), ==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		g_assert_cmpint(unshare(CLONE_NEWUSER), ==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		g_assert_cmpint(syscall(SYS_io_uring_setup, 1, NULL), ==, -1);
		g_assert_cmpint(errno, ==, ENOSYS);
		g_assert_cmpint(syscall(SYS_clone3, NULL, 0), ==, -1);
		g_assert_cmpint(errno, ==, ENOSYS);
		g_assert_cmpint(syscall(SYS_clone, CLONE_NEWNET | SIGCHLD, 0, 0,
					0, 0),
				==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		g_assert_cmpint(syscall(SYS_bpf, 0, NULL, 0), ==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		/* Unfiltered, a pipe answers ENOTTY; the filter answers first. */
		int pipefd[2], waiting = -1;
		char key = 'x';

		g_assert_cmpint(pipe2(pipefd, O_CLOEXEC), ==, 0);
		g_assert_cmpint(ioctl(pipefd[0], TIOCSTI, &key), ==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		g_assert_cmpint(ioctl(pipefd[0], TIOCLINUX, &key), ==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		/* High bits the kernel ignores must not slip past. */
		g_assert_cmpint(syscall(SYS_ioctl, pipefd[0],
					(1UL << 32) | TIOCSTI, &key),
				==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		g_assert_cmpint(ioctl(pipefd[0], FIONREAD, &waiting), ==, 0);
		g_assert_cmpint(waiting, ==, 0);
		close(pipefd[0]);
		close(pipefd[1]);
	} else {
		g_thread_join(early);
	}
	/* What the worker does: threads, fork, memfds, UDP batches, TCP. */
	g_assert_true(g_thread_join(
		g_thread_new("after", thread_works, GINT_TO_POINTER(1))));
	pid_t child = fork();
	int status;

	g_assert_cmpint(child, >=, 0);
	if (!child)
		_exit(7);
	g_assert_cmpint(waitpid(child, &status, 0), ==, child);
	g_assert_cmpint(WEXITSTATUS(status), ==, 7);
	int memory = memfd_create("check", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	g_assert_cmpint(memory, >=, 0);
	close(memory);
	struct sockaddr_in udp = { .sin_family = AF_INET,
				   .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int rx = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0),
	    tx = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	len = sizeof(udp);
	g_assert_cmpint(bind(rx, (void *)&udp, sizeof(udp)), ==, 0);
	g_assert_cmpint(getsockname(rx, (void *)&udp, &len), ==, 0);
	struct line_udp_tx *t = line_udp_tx_new(tx, LINE_UDP_GSO);
	guint8 packet[100] = { 1 };

	g_assert_true(line_udp_tx_queue(t, &udp, sizeof(udp), packet, 100));
	g_assert_true(line_udp_tx_queue(t, &udp, sizeof(udp), packet, 50));
	g_assert_true(line_udp_tx_flush(t));
	g_assert_cmpint(line_udp_tx_mode(t), ==, LINE_UDP_GSO);
	line_udp_tx_free(t);
	int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	g_assert_cmpint(connect(client, (void *)&addr, sizeof(addr)), ==, 0);
	close(client);
	if (layers & LINE_SANDBOX_LANDLOCK_NET) {
		int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_in any = { .sin_family = AF_INET };

		g_assert_cmpint(bind(server, (void *)&any, sizeof(any)), ==,
				-1);
		g_assert_cmpint(errno, ==, EACCES);
		close(server);
	}
	if (layers & LINE_SANDBOX_LANDLOCK_SIGNAL) {
		g_assert_cmpint(kill(getppid(), 0), ==, -1);
		g_assert_cmpint(errno, ==, EPERM);
		g_assert_cmpint(kill(getpid(), 0), ==, 0);
	}
	g_assert_true(layers & LINE_SANDBOX_HEAP_WIPE);
	g_assert_true(layers & LINE_SANDBOX_GUARD_PAGES);
	g_assert_cmpuint(!!(layers & LINE_SANDBOX_MDWE), ==, has_mdwe);
	g_assert_cmpuint(!!(layers & LINE_SANDBOX_SEAL), ==, has_mseal);
}

/* Writable and executable memory, or memory that becomes executable. */
static void assert_no_exec_gain(void)
{
	long page = sysconf(_SC_PAGESIZE);
	void *both = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	g_assert_true(both == MAP_FAILED);
	g_assert_cmpint(errno, ==, EACCES);
	void *data = mmap(NULL, page, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	g_assert_true(data != MAP_FAILED);
	g_assert_cmpint(mprotect(data, page, PROT_READ | PROT_EXEC), ==, -1);
	g_assert_cmpint(errno, ==, EACCES);
	munmap(data, page);
}

struct relro {
	uintptr_t start, end;
};

static int relro_of_program(struct dl_phdr_info *info, size_t size, void *data)
{
	struct relro *relro = data;

	(void)size;
	for (int i = 0; i < info->dlpi_phnum; i++)
		if (info->dlpi_phdr[i].p_type == PT_GNU_RELRO) {
			relro->start =
				info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
			relro->end = relro->start + info->dlpi_phdr[i].p_memsz;
			break;
		}
	/* The program comes first. */
	return 1;
}

/* Code and RELRO cannot be made writable, unmapped or moved. */
static void assert_sealed(void)
{
	uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
	void *code = (void *)((uintptr_t)&assert_sealed & ~(page - 1));
	struct relro relro = { 0 };

	g_assert_cmpint(mprotect(code, page, PROT_READ | PROT_WRITE), ==, -1);
	g_assert_cmpint(errno, ==, EPERM);
	g_assert_cmpint(munmap(code, page), ==, -1);
	g_assert_cmpint(errno, ==, EPERM);
	g_assert_true(mremap(code, page, page, MREMAP_MAYMOVE) == MAP_FAILED);
	g_assert_cmpint(errno, ==, EPERM);
	dl_iterate_phdr(relro_of_program, &relro);
	g_assert_cmpuint(relro.start, !=, 0);
	/* glibc protects RELRO from its first page to its last whole one. */
	relro.start &= ~(page - 1);
	if (relro.start < (relro.end & ~(page - 1))) {
		g_assert_cmpint(mprotect((void *)relro.start, page,
					 PROT_READ | PROT_WRITE),
				==, -1);
		g_assert_cmpint(errno, ==, EPERM);
	}
}

static void hardening(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	gboolean has_mdwe = prctl(MDWE_GET, 0, 0, 0, 0) >= 0;
	gboolean has_mseal = !syscall(SYS_mseal, 0UL, 0UL, 0UL);
	/* A chunk past the per-thread cache, kept off the top by another. */
	guint8 *old = malloc(4096), *fence = malloc(64);

	memset(old, 'S', 4096);
	uintptr_t where = (uintptr_t)old;

	free(old);
	g_assert_cmpint(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0), ==, 0);
	guint layers = line_sandbox_worker();

	if (has_mdwe) {
		g_assert_true(layers & LINE_SANDBOX_MDWE);
		g_assert_cmpint(prctl(MDWE_GET, 0, 0, 0, 0) &
					MDWE_REFUSE_EXEC_GAIN,
				!=, 0);
		assert_no_exec_gain();
	}
	if (has_mseal) {
		g_assert_true(layers & LINE_SANDBOX_SEAL);
		assert_sealed();
	}
	/* Chunks handed out again are zeroed, and freed ones overwritten. */
	g_assert_true(layers & LINE_SANDBOX_HEAP_WIPE);
	guint8 *again = malloc(4096);

	if ((uintptr_t)again == where)
		for (gsize i = 0; i < 4096; i++)
			g_assert_cmpuint(again[i], ==, 0);
	free(again);
	free(fence);
	/* Libraries loaded after sealing still load and unload. */
	g_assert_true(g_thread_join(
		g_thread_new("after", thread_works, GINT_TO_POINTER(1))));
}

static void switched_off(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	g_assert_cmpint(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0), ==, 0);
	/* An outer process may have set MDWE already; it must not change. */
	int outer_mdwe = prctl(MDWE_GET, 0, 0, 0, 0);

	g_setenv("LINE_DISABLE_SANDBOX", "mdwe, seal,wipe,guard", TRUE);
	guint layers = line_sandbox_worker();
	struct line_guarded window;
	const guint8 byte = 1;

	g_assert_false(layers &
		       (LINE_SANDBOX_MDWE | LINE_SANDBOX_SEAL |
			LINE_SANDBOX_HEAP_WIPE | LINE_SANDBOX_GUARD_PAGES));
	g_assert_true(layers & LINE_SANDBOX_NO_DUMP);
	if (FILTERED)
		g_assert_true(layers & LINE_SANDBOX_SECCOMP);
	g_assert_cmpint(prctl(MDWE_GET, 0, 0, 0, 0), ==, outer_mdwe);
	g_assert_false(line_guard_copy(&window, &byte, 1));
	g_assert_null(window.data);
	line_guard_free(&window);
}

static const guint8 reply[] = { 0x80, 0x01, 0x00, 0x02, 'r',  'e', 'p',
				'l',  'y',  0x0c, 0x00, 0x00, 0x00 };

static void guarded(void)
{
	struct line_guarded window;
	long page = sysconf(_SC_PAGESIZE);

	g_assert_true(line_guard_copy(&window, reply, sizeof(reply)));
	g_assert_cmpmem(window.data, window.size, reply, sizeof(reply));
	/* The byte after the copy is the first byte of the guard page. */
	g_assert_cmpuint(((uintptr_t)window.data + window.size) % page, ==, 0);
	line_guard_free(&window);
	g_assert_null(window.map);
	/* An empty reply still gets a copy to point at. */
	g_assert_true(line_guard_copy(&window, NULL, 0));
	g_assert_nonnull(window.data);
	g_assert_cmpuint(window.size, ==, 0);
	line_guard_free(&window);
	/* A page-sized reply leaves no slack before the guard either. */
	g_autofree guint8 *large = g_malloc0(page);

	g_assert_true(line_guard_copy(&window, large, page));
	g_assert_cmpmem(window.data, window.size, large, page);
	line_guard_free(&window);
}

static void guarded_overread(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_failed();
		g_test_trap_assert_stdout("*ready*");
		g_test_trap_assert_stdout_unmatched("*read past the end*");
		return;
	}
	struct line_guarded window;

	g_assert_true(line_guard_copy(&window, reply, sizeof(reply)));
	const guint8 *end = window.data + window.size;

	printf("ready\n");
	fflush(stdout);
	printf("read past the end: %u\n", *end);
}

static void guarded_write(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_failed();
		g_test_trap_assert_stdout("*ready*");
		g_test_trap_assert_stdout_unmatched("*wrote*");
		return;
	}
	struct line_guarded window;

	g_assert_true(line_guard_copy(&window, reply, sizeof(reply)));
	guint8 *start = (guint8 *)window.data;

	printf("ready\n");
	fflush(stdout);
	start[0] = 0;
	printf("wrote\n");
}

static void tunables(void)
{
	const guint shstk = LINE_SANDBOX_SHADOW_STACK,
		    mte = LINE_SANDBOX_MEMORY_TAGGING;
	struct {
		const char *current;
		guint layers;
		const char *want;
	} cases[] = {
		{ NULL, 0, NULL },
		{ "glibc.malloc.arena_max=2", 0, NULL },
		{ NULL, shstk,
		  "glibc.cpu.hwcaps=SHSTK:glibc.cpu.x86_shstk=on" },
		{ "", mte, "glibc.mem.tagging=1" },
		{ "glibc.malloc.arena_max=2", shstk | mte,
		  "glibc.malloc.arena_max=2:glibc.cpu.hwcaps=SHSTK:glibc.cpu.x86_shstk=on:glibc.mem.tagging=1" },
		{ "glibc.cpu.hwcaps=-AVX512F", shstk,
		  "glibc.cpu.hwcaps=-AVX512F,SHSTK:glibc.cpu.x86_shstk=on" },
		{ "glibc.cpu.hwcaps=", shstk,
		  "glibc.cpu.hwcaps=SHSTK:glibc.cpu.x86_shstk=on" },
		/* The user's own choice wins. */
		{ "glibc.cpu.x86_shstk=off", shstk, NULL },
		{ "glibc.mem.tagging=0", shstk | mte,
		  "glibc.mem.tagging=0:glibc.cpu.hwcaps=SHSTK:glibc.cpu.x86_shstk=on" },
		{ "a=1::b=2", mte, "a=1:b=2:glibc.mem.tagging=1" },
	};

	for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree char *got = line_sandbox_tunables(cases[i].current,
							     cases[i].layers);

		g_assert_cmpstr(got, ==, cases[i].want);
	}
}

typedef ElfW(Phdr) line_phdr;

static int program_notes(struct dl_phdr_info *info, size_t size, void *data)
{
	guint32 *features = data;

	(void)size;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const line_phdr *ph = &info->dlpi_phdr[i];

		if (ph->p_type != PT_GNU_PROPERTY)
			continue;
		const guint8 *note =
			(const guint8 *)(info->dlpi_addr + ph->p_vaddr) + 12 +
			4;
		const guint8 *end = note + ph->p_memsz - 16;

		/* Properties: type, size, value, padded to 8 bytes. */
		while (note + 8 <= end) {
			guint32 type, length;

			memcpy(&type, note, 4);
			memcpy(&length, note + 4, 4);
			if (type == GNU_PROPERTY_X86_FEATURE_1_AND &&
			    length == 4)
				memcpy(features, note + 8, 4);
			note += 8 + ((length + 7) & ~7U);
		}
	}
	return 1;
}

/* -fcf-protection marked the program, and the worker gets the stack. */
static void shadow_stack(void)
{
#if defined(__x86_64__) && defined(__CET__)
	guint32 features = 0;

	dl_iterate_phdr(program_notes, &features);
	g_assert_cmpuint(features & GNU_PROPERTY_X86_FEATURE_1_SHSTK, !=, 0);
#endif
	if (!(line_sandbox_worker_spawn_layers() & LINE_SANDBOX_SHADOW_STACK)) {
		g_test_skip("no user shadow stack on this CPU or kernel");
		return;
	}
	if (g_test_subprocess()) {
		g_assert_cmpint(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0), ==, 0);
		g_assert_true(line_sandbox_worker() &
			      LINE_SANDBOX_SHADOW_STACK);
		/* Threads get their own shadow stacks. */
		g_assert_true(g_thread_join(g_thread_new("after", thread_works,
							 GINT_TO_POINTER(1))));
		return;
	}
#if GLIB_CHECK_VERSION(2, 80, 0)
	g_auto(GStrv) env = g_get_environ();
	g_autofree char *value =
		line_sandbox_tunables(g_environ_getenv(env, "GLIBC_TUNABLES"),
				      LINE_SANDBOX_SHADOW_STACK);

	/* NULL: the environment asks for the shadow stack already. */
	if (value)
		env = g_environ_setenv(env, "GLIBC_TUNABLES", value, TRUE);
	g_test_trap_subprocess_with_envp(NULL, (const char *const *)env, 0,
					 G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
#else
	g_test_skip("needs GLib 2.80");
#endif
}

#if defined(__x86_64__) && GLIB_CHECK_VERSION(2, 80, 0)
static G_GNUC_NO_INLINE void landing(void)
{
	printf("hijacked\n");
	fflush(stdout);
	_exit(0);
}

/* What a stack overflow does: replace the return address. */
static G_GNUC_NO_INLINE void smash(void)
{
	void **frame = __builtin_frame_address(0);
	void (*target)(void) = landing;

	memcpy(&frame[1], &target, sizeof(target));
	__asm__ volatile("" ::: "memory");
}

static void return_hijack(void)
{
	if (g_test_subprocess()) {
		printf("ready\n");
		fflush(stdout);
		smash();
		printf("returned\n");
		return;
	}
	if (!(line_sandbox_worker_spawn_layers() & LINE_SANDBOX_SHADOW_STACK)) {
		g_test_skip("no user shadow stack on this CPU or kernel");
		return;
	}
	g_auto(GStrv) env = g_get_environ();
	g_autofree char *value =
		line_sandbox_tunables(g_environ_getenv(env, "GLIBC_TUNABLES"),
				      LINE_SANDBOX_SHADOW_STACK);

	if (value)
		env = g_environ_setenv(env, "GLIBC_TUNABLES", value, TRUE);
	g_test_trap_subprocess_with_envp(NULL, (const char *const *)env, 0,
					 G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
	g_test_trap_assert_stdout("*ready*");
	g_test_trap_assert_stdout_unmatched("*hijacked*");
	/* Without the shadow stack the same overwrite takes control. */
	env = g_environ_unsetenv(env, "GLIBC_TUNABLES");
	g_test_trap_subprocess_with_envp(NULL, (const char *const *)env, 0,
					 G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
	g_test_trap_assert_stdout("*hijacked*");
}
#endif

static void ui_layers(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	gboolean has_mseal = !syscall(SYS_mseal, 0UL, 0UL, 0UL);
	guint layers = line_sandbox_ui_start();

	g_assert_cmpuint(!!(layers & LINE_SANDBOX_SEAL), ==, has_mseal);
	if (has_mseal)
		assert_sealed();
	/* A renderer that may need a JIT keeps the UI without MDWE. */
	layers = line_sandbox_ui_renderer(FALSE);
	g_assert_false(layers & LINE_SANDBOX_MDWE);
	layers = line_sandbox_ui_renderer(TRUE);
	g_assert_cmpuint(layers, ==, line_sandbox_ui_layers());
	if (!(layers & LINE_SANDBOX_MDWE)) {
		/* Linux before 6.7 cannot keep it from the LIFF browser. */
		g_test_message("no PR_MDWE_NO_INHERIT");
		return;
	}
	g_assert_cmpint(prctl(MDWE_GET, 0, 0, 0, 0), ==,
			MDWE_REFUSE_EXEC_GAIN | MDWE_NO_INHERIT);
	assert_no_exec_gain();
	/* Children (the LIFF browser, the worker) start without it. */
	pid_t child = fork();
	int status;

	g_assert_cmpint(child, >=, 0);
	if (!child)
		_exit(prctl(MDWE_GET, 0, 0, 0, 0) == 0 ? 0 : 1);
	g_assert_cmpint(waitpid(child, &status, 0), ==, child);
	g_assert_true(WIFEXITED(status) && !WEXITSTATUS(status));
	g_assert_true(exec_true());
}

static void disabled(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	/* GLib's test children start non-dumpable; undo that first. */
	g_assert_cmpint(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0), ==, 0);
	g_setenv("LINE_DISABLE_SANDBOX", "1", TRUE);
	g_assert_cmpuint(line_sandbox_worker(), ==, 0);
	g_assert_cmpint(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0), ==, 1);
	g_assert_true(exec_true());
}

/* Inside an outer filter that already refuses seccomp() and prctl(). */
static void outer_sandbox(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	g_assert_cmpint(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), ==, 0);
	g_setenv("LINE_DEBUG_SANDBOX", "1", TRUE);
	/* Stacked filters are allowed; the second must not break the first. */
	guint first = line_sandbox_worker();
	guint second = line_sandbox_worker();

	g_assert_cmpuint(first & LINE_SANDBOX_NO_DUMP, ==,
			 LINE_SANDBOX_NO_DUMP);
	g_assert_cmpuint(second & LINE_SANDBOX_NO_DUMP, ==,
			 LINE_SANDBOX_NO_DUMP);
	g_assert_true(g_thread_join(
		g_thread_new("after", thread_works, GINT_TO_POINTER(1))));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sandbox/refused-and-working", refused);
	g_test_add_func("/sandbox/disabled", disabled);
	g_test_add_func("/sandbox/applied-twice", outer_sandbox);
	g_test_add_func("/sandbox/memory-hardening", hardening);
	g_test_add_func("/sandbox/switched-off-by-name", switched_off);
	g_test_add_func("/sandbox/guard/copy", guarded);
	g_test_add_func("/sandbox/guard/over-read", guarded_overread);
	g_test_add_func("/sandbox/guard/write", guarded_write);
	g_test_add_func("/sandbox/tunables", tunables);
	g_test_add_func("/sandbox/shadow-stack", shadow_stack);
#if defined(__x86_64__) && GLIB_CHECK_VERSION(2, 80, 0)
	g_test_add_func("/sandbox/return-hijack", return_hijack);
#endif
	g_test_add_func("/sandbox/ui", ui_layers);
	return g_test_run();
}
