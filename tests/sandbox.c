// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The worker sandbox, applied in a child process: what it refuses, and
 * that everything the worker does keeps working under it.
 */
#include "../src/sandbox.h"
#include "../src/udp_batch.h"
#include <errno.h>
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
	return g_test_run();
}
