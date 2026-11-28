// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Adversarial kernel-enforcement verification suite for Avernella's
 * sandboxing and privilege-separation layer (src/security/).
 *
 * Spawns real sandboxed child workers and proves the Linux kernel actively
 * blocks:
 *   1. Reading /etc/passwd or ~/.ssh (EACCES from Landlock)
 *   2. Connecting to unauthorized local/external ports (EACCES / ENETUNREACH)
 *   3. Executing mmap(..., PROT_READ|PROT_WRITE|PROT_EXEC), mprotect(PROT_EXEC),
 *      execve("/bin/sh"), fork(), or io_uring_setup() (immediate SIGSYS kill)
 *   4. Pre-sandbox FD leakage across fork() (EBADF after close_range)
 *   5. Secret exfiltration (memfd_secret Key Vault + SOCK_SEQPACKET IPC +
 *      supervisor crash respawn)
 */
#include "security/ipc.h"
#include "security/sandbox.h"
#include "security/vault.h"
#include "state.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif

/* Stub for offline sealing.o linkage: returns self's X25519 key when asked. */
static uint8_t test_self_pub[32];
static bool test_self_pub_ready;

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)s;
	(void)path;
	(void)args;
	if (test_self_pub_ready &&
	    !g_strcmp0(method, "negotiateE2EEPublicKey")) {
		J *reply = obj();
		J *key = obj();

		json_object_object_add(reply, "3", json_object_new_int(2));
		json_object_object_add(key, "1", json_object_new_int(2));
		json_object_object_add(key, "2", json_object_new_int(1));
		json_object_object_add(
			key, "4",
			json_object_new_string_len((const char *)test_self_pub,
						   sizeof(test_self_pub)));
		json_object_object_add(reply, "2", key);
		return reply;
	}
	g_set_error_literal(error, LINE_ERROR, 1, "Offline test stub.");
	return NULL;
}

static int start_loopback_listener(uint16_t *out_port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);

	g_assert_cmpint(fd, >=, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==,
			0);
	g_assert_cmpint(listen(fd, 4), ==, 0);
	g_assert_cmpint(getsockname(fd, (struct sockaddr *)&addr, &len), ==, 0);
	*out_port = ntohs(addr.sin_port);
	return fd;
}

static void *noop_thread(void *arg)
{
	return arg;
}

struct net_test_ctx {
	const char *ssh_path;
	uint16_t forbidden_port;
	int canary_fd;
};

static int net_worker_adversarial(int ctrl_fd, uint32_t active, void *userdata)
{
	const struct net_test_ctx *ctx = userdata;
	struct avernella_ipc_hdr hdr;
	struct sockaddr_in addr;
	pthread_t th;
	void *res = NULL;
	void *rw_page;
	int fd, s, ep, ev, mfd;

	if (!(active & AVSB_ACTIVE_SECCOMP) ||
	    !(active & AVSB_ACTIVE_LANDLOCK_FS) ||
	    !(active & AVSB_ACTIVE_LANDLOCK_NET))
		return 10;

	/* 1. Pre-sandbox canary FD must be closed (EBADF). */
	if (ctx->canary_fd >= 0 &&
	    (fcntl(ctx->canary_fd, F_GETFD) != -1 || errno != EBADF))
		return 11;

	/* 2. Reading /etc/passwd must be blocked with EACCES by Landlock. */
	fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
	if (fd >= 0 || errno != EACCES)
		return 12;

	/* 3. Reading ~/.ssh canary must be blocked with EACCES by Landlock. */
	fd = open(ctx->ssh_path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0 || errno != EACCES)
		return 13;

	/* 4. Connecting to an unauthorized local TCP port must return EACCES. */
	s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return 14;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(ctx->forbidden_port);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0 ||
	    errno != EACCES) {
		close(s);
		return 15;
	}

	/* 5. Binding a TCP listener must also return EACCES. */
	addr.sin_port = 0;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0 ||
	    errno != EACCES) {
		close(s);
		return 16;
	}
	close(s);

	/* 6. Legitimate worker primitives (threads, epoll, rw mmap, memfd) work. */
	if (pthread_create(&th, NULL, noop_thread, (void *)0x1234) != 0 ||
	    pthread_join(th, &res) != 0 || res != (void *)0x1234)
		return 17;

	ep = epoll_create1(EPOLL_CLOEXEC);
	ev = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	mfd = memfd_create("avsb-test", MFD_CLOEXEC);
	rw_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ep < 0 || ev < 0 || mfd < 0 || rw_page == MAP_FAILED)
		return 18;
	munmap(rw_page, 4096);
	close(ep);
	close(ev);

	/* Pass sealed memfd back to parent over SCM_RIGHTS to verify IPC. */
	if (write(mfd, "ok", 2) != 2)
		return 19;
	memset(&hdr, 0, sizeof(hdr));
	hdr.op = IPC_OP_REPLY_OK;
	hdr.req_id = 42;
	hdr.payload_len = 2;
	if (avernella_ipc_send(ctrl_fd, &hdr, "ok", &mfd, 1) != 0)
		return 20;
	close(mfd);
	return 0;
}

static void test_probe(void)
{
	struct avernella_sandbox_caps caps;

	avernella_sandbox_probe(&caps);
	g_test_message(
		"Kernel capabilities: landlock_abi=%d memfd_secret=%d userns=%d mdwe=%d seccomp=%d",
		caps.landlock_abi, caps.has_memfd_secret, caps.has_userns,
		caps.has_mdwe, caps.has_seccomp);
	g_assert_cmpint(caps.landlock_abi, >=, 1);
	g_assert_true(caps.has_seccomp);
}

static void test_net_worker_landlock_and_fd_hygiene(void)
{
	const uint16_t allowed_ports[] = { 443 };
	uint16_t forbidden_port = 0;
	int listener = start_loopback_listener(&forbidden_port);
	int canary = open("/etc/passwd", O_RDONLY);
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	struct avernella_ipc_hdr hdr;
	char buf[16];
	int passed_fd = -1;
	size_t nfds = 0;
	int status = 0;

	g_assert_cmpint(canary, >=, 0);

	/* Create a synthetic ~/.ssh canary file inside isolated test HOME. */
	g_autofree char *ssh_dir =
		g_build_filename(g_get_home_dir(), ".ssh", NULL);
	g_autofree char *ssh_key =
		g_build_filename(ssh_dir, "id_ed25519", NULL);
	g_assert_cmpint(g_mkdir_with_parents(ssh_dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(ssh_key, "PRIVATE-KEY", -1, NULL));

	struct net_test_ctx ctx = {
		.ssh_path = ssh_key,
		.forbidden_port = forbidden_port,
		.canary_fd = canary,
	};

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_NET;
	opts.keep_stderr = true;
	opts.tcp_connect_ports = allowed_ports;
	opts.n_tcp_connect_ports = 1;

	g_assert_cmpint(avernella_spawn_worker(&opts, net_worker_adversarial,
					       &ctx, &proc),
			==, 0);

	g_assert_cmpint(avernella_ipc_recv(proc.ctrl_fd, &hdr, buf, sizeof(buf),
					   &passed_fd, 1, &nfds),
			==, 0);
	g_assert_cmpuint(hdr.op, ==, IPC_OP_REPLY_OK);
	g_assert_cmpuint(hdr.req_id, ==, 42);
	g_assert_cmpuint(nfds, ==, 1);
	g_assert_cmpint(passed_fd, >=, 0);
	close(passed_fd);

	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	proc.pid = -1;
	avernella_worker_close(&proc);
	close(canary);
	close(listener);
	g_assert_true(WIFEXITED(status));
	if (WEXITSTATUS(status) == 77) {
		g_test_skip("Unprivileged mount/userns restricted by host LSM");
		return;
	}
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

static int empty_root_worker(int ctrl_fd, uint32_t active, void *userdata)
{
	uint16_t forbidden_port = *(const uint16_t *)userdata;
	struct sockaddr_in addr;
	int fd, s;

	(void)ctrl_fd;
	if (!(active & AVSB_ACTIVE_EMPTY_ROOT) ||
	    !(active & AVSB_ACTIVE_NET_NS))
		return 77;

	/* /etc/passwd must not exist or be accessible in empty RO tmpfs. */
	fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
	if (fd >= 0 || (errno != ENOENT && errno != EACCES))
		return 31;

	/* Creating a file at / must fail (EROFS or EACCES). */
	fd = open("/escape", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	if (fd >= 0 || (errno != EROFS && errno != EACCES))
		return 32;

	/* Connecting to host loopback inside CLONE_NEWNET must fail. */
	s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return 33;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(forbidden_port);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		close(s);
		return 34;
	}
	close(s);
	return 0;
}

static void test_net_worker_empty_root_and_netns(void)
{
	struct avernella_sandbox_caps caps;
	uint16_t forbidden_port = 0;
	int listener = start_loopback_listener(&forbidden_port);
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	int status = 0;

	avernella_sandbox_probe(&caps);
	if (!caps.has_userns) {
		close(listener);
		g_test_skip("Unprivileged user namespaces disabled on host");
		return;
	}

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_NET;
	opts.isolate_mount_ns = true;
	opts.isolate_net_ns = true;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_spawn_worker(&opts, empty_root_worker,
					       &forbidden_port, &proc),
			==, 0);
	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	proc.pid = -1;
	avernella_worker_close(&proc);
	close(listener);
	g_assert_true(WIFEXITED(status));
	if (WEXITSTATUS(status) == 77) {
		g_test_skip("Unprivileged mount/userns restricted by host LSM");
		return;
	}
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

static int attack_rwx_mmap(int ctrl_fd, uint32_t active, void *userdata)
{
	(void)ctrl_fd;
	(void)active;
	(void)userdata;
	/* Must be immediately killed by seccomp-bpf (SIGSYS). */
	void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return p == MAP_FAILED ? 1 : 2;
}

static int attack_mprotect_exec(int ctrl_fd, uint32_t active, void *userdata)
{
	void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	(void)ctrl_fd;
	(void)active;
	(void)userdata;
	if (p == MAP_FAILED)
		return 1;
	/* Must be immediately killed by seccomp-bpf (SIGSYS). */
	(void)mprotect(p, 4096, PROT_READ | PROT_EXEC);
	return 2;
}

static int attack_execve_sh(int ctrl_fd, uint32_t active, void *userdata)
{
	char *const argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)"true",
			       NULL };
	char *const envp[] = { NULL };

	(void)ctrl_fd;
	(void)active;
	(void)userdata;
	/* Must be immediately killed by seccomp-bpf (SIGSYS). */
	execve("/bin/sh", argv, envp);
	return 2;
}

static int attack_fork_raw(int ctrl_fd, uint32_t active, void *userdata)
{
	(void)ctrl_fd;
	(void)active;
	(void)userdata;
	/* Non-CLONE_THREAD clone() must be immediately killed (SIGSYS). */
	(void)syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	return 2;
}

static int attack_io_uring(int ctrl_fd, uint32_t active, void *userdata)
{
	(void)ctrl_fd;
	(void)active;
	(void)userdata;
	/* io_uring_setup() must be immediately killed (SIGSYS). */
	(void)syscall(SYS_io_uring_setup, 1, NULL);
	return 2;
}

static void assert_killed_by_sigsys(avernella_worker_fn attack)
{
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	int status = 0;

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_NET;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_spawn_worker(&opts, attack, NULL, &proc), ==,
			0);
	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	proc.pid = -1;
	avernella_worker_close(&proc);
	g_assert_true(WIFSIGNALED(status));
	g_assert_cmpint(WTERMSIG(status), ==, SIGSYS);
}

static void test_seccomp_kills_attacks(void)
{
	assert_killed_by_sigsys(attack_rwx_mmap);
	assert_killed_by_sigsys(attack_mprotect_exec);
	assert_killed_by_sigsys(attack_execve_sh);
	assert_killed_by_sigsys(attack_fork_raw);
	assert_killed_by_sigsys(attack_io_uring);
}

static int media_worker_check(int ctrl_fd, uint32_t active, void *userdata)
{
	const struct net_test_ctx *ctx = userdata;
	struct sockaddr_in addr;
	pthread_t th;
	int fd, s, mfd;

	(void)ctrl_fd;
	if (!(active & AVSB_ACTIVE_SECCOMP) ||
	    !(active & AVSB_ACTIVE_LANDLOCK_FS) ||
	    !(active & AVSB_ACTIVE_LANDLOCK_NET))
		return 40;

	/* Media worker cannot read /etc/passwd or ~/.ssh. */
	fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
	if (fd >= 0 || errno != EACCES)
		return 41;
	fd = open(ctx->ssh_path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0 || errno != EACCES)
		return 42;

	/* Media worker cannot connect to unauthorized TCP ports. */
	s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return 43;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(ctx->forbidden_port);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0 ||
	    errno != EACCES) {
		close(s);
		return 44;
	}
	close(s);

	/* PipeWire/codec threads and zero-copy memfd ring buffers work. */
	if (pthread_create(&th, NULL, noop_thread, NULL) != 0 ||
	    pthread_join(th, NULL) != 0)
		return 45;
	mfd = memfd_create("pw-ring", MFD_CLOEXEC);
	if (mfd < 0 || ftruncate(mfd, 4096) < 0)
		return 46;
	close(mfd);
	return 0;
}

static void test_media_worker_isolation(void)
{
	uint16_t forbidden_port = 0;
	int listener = start_loopback_listener(&forbidden_port);
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	int status = 0;

	g_autofree char *ssh_dir =
		g_build_filename(g_get_home_dir(), ".ssh", NULL);
	g_autofree char *ssh_key =
		g_build_filename(ssh_dir, "id_ed25519", NULL);
	g_assert_cmpint(g_mkdir_with_parents(ssh_dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(ssh_key, "PRIVATE-KEY", -1, NULL));

	struct net_test_ctx ctx = {
		.ssh_path = ssh_key,
		.forbidden_port = forbidden_port,
		.canary_fd = -1,
	};

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_MEDIA;
	opts.allow_gpu_ioctl = true;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_spawn_worker(&opts, media_worker_check, &ctx,
					       &proc),
			==, 0);
	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	proc.pid = -1;
	avernella_worker_close(&proc);
	close(listener);
	g_assert_true(WIFEXITED(status));
	if (WEXITSTATUS(status) == 77) {
		g_test_skip("Unprivileged mount/userns restricted by host LSM");
		return;
	}
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

struct ui_test_ctx {
	const char *ssh_key;
	const char *gnupg_file;
	const char *session_file;
	const char *cache_file;
};

static int ui_landlock_child(int ctrl_fd, uint32_t active, void *userdata)
{
	const struct ui_test_ctx *ctx = userdata;
	int fd;

	(void)ctrl_fd;
	if (!(active & AVSB_ACTIVE_LANDLOCK_FS))
		return 50;

	/* Allowed: read /usr/share and write inside XDG_CACHE_HOME/avernella. */
	fd = open("/usr/share", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return 51;
	close(fd);

	fd = open(ctx->cache_file, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
		  0600);
	if (fd < 0)
		return 52;
	close(fd);

	/* Blocked: ~/.ssh, ~/.gnupg, and session.json in XDG_STATE_HOME. */
	if (open(ctx->ssh_key, O_RDONLY | O_CLOEXEC) >= 0 || errno != EACCES)
		return 53;
	if (open(ctx->gnupg_file, O_RDONLY | O_CLOEXEC) >= 0 || errno != EACCES)
		return 54;
	if (open(ctx->session_file, O_RDONLY | O_CLOEXEC) >= 0 ||
	    errno != EACCES)
		return 55;
	return 0;
}

static void test_ui_landlock_blocks_secrets(void)
{
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	int status = 0;

	g_autofree char *ssh_dir =
		g_build_filename(g_get_home_dir(), ".ssh", NULL);
	g_autofree char *ssh_key = g_build_filename(ssh_dir, "id_rsa", NULL);
	g_autofree char *gnupg_dir =
		g_build_filename(g_get_home_dir(), ".gnupg", NULL);
	g_autofree char *gnupg_file =
		g_build_filename(gnupg_dir, "secring.gpg", NULL);
	g_autofree char *state_dir =
		g_build_filename(g_get_user_state_dir(), "avernella", NULL);
	g_autofree char *session_file =
		g_build_filename(state_dir, "session.json", NULL);
	g_autofree char *cache_dir =
		g_build_filename(g_get_user_cache_dir(), "avernella", NULL);
	g_autofree char *cache_file =
		g_build_filename(cache_dir, "thumb.bin", NULL);

	g_assert_cmpint(g_mkdir_with_parents(ssh_dir, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(gnupg_dir, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(state_dir, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(cache_dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(ssh_key, "SSH", -1, NULL));
	g_assert_true(g_file_set_contents(gnupg_file, "GPG", -1, NULL));
	g_assert_true(g_file_set_contents(session_file, "{\"access\":\"x\"}",
					  -1, NULL));

	struct ui_test_ctx ctx = {
		.ssh_key = ssh_key,
		.gnupg_file = gnupg_file,
		.session_file = session_file,
		.cache_file = cache_file,
	};

	const char *extra_rw[1];

	extra_rw[0] = cache_dir;

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_UI;
	opts.allow_gpu_ioctl = true;
	opts.keep_stderr = true;
	opts.extra_rw_paths = extra_rw;
	opts.n_extra_rw_paths = 1;

	g_assert_cmpint(avernella_spawn_worker(&opts, ui_landlock_child, &ctx,
					       &proc),
			==, 0);
	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	proc.pid = -1;
	avernella_worker_close(&proc);
	g_assert_true(WIFEXITED(status));
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

static int vault_worker_client(int ctrl_fd, uint32_t active, void *userdata)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(J) info = NULL;
	g_autoptr(J) payload = NULL;
	g_autoptr(J) chunks = NULL;
	g_autoptr(J) msg = NULL;
	g_autoptr(J) opened = NULL;
	gint32 seq;

	(void)active;
	(void)userdata;

	if (avernella_vault_client_ping(ctrl_fd) != 0)
		return 60;

	info = avernella_vault_client_public_info(ctrl_fd, &error);
	if (!info || g_strcmp0(strfield(info, "mid"),
			       "u00000000000000000000000000000001") != 0)
		return 61;

	seq = avernella_vault_client_reserve_seq(ctrl_fd, &error);
	if (seq != 101)
		return 62;

	/* Ask the Key Vault to Letter-Seal and open a payload (worker has 0 keys). */
	payload = obj();
	json_object_object_add(payload, "text",
			       json_object_new_string("Secret via Vault"));
	chunks = avernella_vault_client_seal(
		ctrl_fd, "u00000000000000000000000000000001", 0, payload,
		&error);
	if (!chunks || json_object_array_length(chunks) != 5)
		return 63;

	msg = obj();
	json_object_object_add(
		msg, "1",
		json_object_new_string("u00000000000000000000000000000001"));
	json_object_object_add(
		msg, "2",
		json_object_new_string("u00000000000000000000000000000001"));
	json_object_object_add(msg, "15", json_object_new_int(0));
	json_object_object_add(msg, "18",
			       json_tokener_parse("{\"e2eeVersion\":\"2\"}"));
	json_object_object_add(msg, "20", json_object_get(chunks));

	opened = avernella_vault_client_open(ctrl_fd, msg, 0, &error);
	if (!opened ||
	    g_strcmp0(strfield(opened, "text"), "Secret via Vault") != 0)
		return 64;
	return 0;
}

static J *make_self_key_fixture(void)
{
	/* Generate a valid ephemeral X25519 key pair for offline Vault seal/open. */
	EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
	EVP_PKEY *pkey = NULL;
	uint8_t priv[32], pub[32];
	size_t priv_len = sizeof(priv), pub_len = sizeof(pub);
	g_autofree char *priv_b64 = NULL;
	g_autofree char *pub_b64 = NULL;
	J *keys = obj();
	J *entry = obj();

	g_assert_nonnull(kctx);
	g_assert_cmpint(EVP_PKEY_keygen_init(kctx), ==, 1);
	g_assert_cmpint(EVP_PKEY_keygen(kctx, &pkey), ==, 1);
	g_assert_cmpint(EVP_PKEY_get_raw_private_key(pkey, priv, &priv_len), ==,
			1);
	g_assert_cmpint(EVP_PKEY_get_raw_public_key(pkey, pub, &pub_len), ==,
			1);
	memcpy(test_self_pub, pub, sizeof(test_self_pub));
	test_self_pub_ready = true;
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(kctx);

	priv_b64 = g_base64_encode(priv, sizeof(priv));
	pub_b64 = g_base64_encode(pub, sizeof(pub));
	explicit_bzero(priv, sizeof(priv));

	J *own = obj();
	J *peers = obj();

	json_object_object_add(own, "1", json_object_new_string(priv_b64));
	json_object_object_add(peers, "u00000000000000000000000000000001/1",
			       json_object_new_string(pub_b64));
	json_object_object_add(keys, "active", json_object_new_int(1));
	json_object_object_add(keys, "own", own);
	json_object_object_add(keys, "peers", peers);
	json_object_put(entry);
	return keys;
}

static void test_vault_memfd_secret_and_ipc(void)
{
	struct avernella_sandbox_caps caps;
	struct avernella_vault vault;
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	int status = 0;

	avernella_sandbox_probe(&caps);
	g_assert_cmpint(avernella_vault_init(&vault, -1), ==, 0);
	if (caps.has_memfd_secret)
		g_assert_true(vault.mem.from_memfd_secret);

	g_autoptr(J) keys = make_self_key_fixture();
	g_assert_cmpint(avernella_vault_load_secrets(
				&vault, "u00000000000000000000000000000001",
				"secret-access-token", "secret-refresh-token",
				keys, 100),
			==, 0);

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_NET;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_spawn_worker(&opts, vault_worker_client, NULL,
					       &proc),
			==, 0);

	/* Vault serves all 4 requests (ping, info, reserve_seq, seal, open). */
	g_assert_cmpint(avernella_vault_run(&vault, proc.ctrl_fd), ==, 0);

	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	proc.pid = -1;
	avernella_worker_close(&proc);
	avernella_vault_destroy(&vault);
	g_assert_true(WIFEXITED(status));
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

static int crash_then_survive_worker(int ctrl_fd, uint32_t active,
				     void *userdata)
{
	(void)active;
	(void)userdata;
	/* Wait for a 1-byte command: 'X' triggers SIGSYS, 'P' replies pong. */
	struct avernella_ipc_hdr hdr;
	char buf[8];

	if (avernella_ipc_recv(ctrl_fd, &hdr, buf, sizeof(buf), NULL, 0,
			       NULL) != 0)
		return 0;
	if (buf[0] == 'X') {
		(void)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		return 99;
	}
	hdr.op = IPC_OP_REPLY_OK;
	hdr.payload_len = 2;
	(void)avernella_ipc_send(ctrl_fd, &hdr, "ok", NULL, 0);
	return 0;
}

static void test_supervisor_respawn_after_sigsys(void)
{
	struct avernella_sandbox_opts opts;
	struct avernella_supervised_worker slot;
	struct avernella_ipc_hdr hdr;
	char buf[8];
	pid_t first_pid;
	int rc = 0;

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_NET;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_supervisor_start(&slot, &opts,
						   crash_then_survive_worker,
						   NULL, 2),
			==, 0);
	first_pid = slot.proc.pid;

	/* Trigger SIGSYS inside the first worker. */
	memset(&hdr, 0, sizeof(hdr));
	hdr.op = IPC_OP_PING;
	hdr.req_id = 1;
	hdr.payload_len = 1;
	g_assert_cmpint(avernella_ipc_send(slot.proc.ctrl_fd, &hdr, "X", NULL,
					   0),
			==, 0);

	/* Poll until supervisor detects SIGSYS death and respawns worker. */
	for (int i = 0; i < 200; i++) {
		rc = avernella_supervisor_poll_respawn(&slot);
		if (rc != 0)
			break;
		g_usleep(5000);
	}
	g_assert_cmpint(rc, ==, 1);
	g_assert_true(WIFSIGNALED(slot.last_status));
	g_assert_cmpint(WTERMSIG(slot.last_status), ==, SIGSYS);
	g_assert_cmpint(slot.restarts, ==, 1);
	g_assert_cmpint(slot.proc.pid, !=, first_pid);

	/* New worker answers cleanly over its fresh SOCK_SEQPACKET channel. */
	hdr.req_id = 2;
	g_assert_cmpint(avernella_ipc_send(slot.proc.ctrl_fd, &hdr, "P", NULL,
					   0),
			==, 0);
	g_assert_cmpint(avernella_ipc_recv(slot.proc.ctrl_fd, &hdr, buf,
					   sizeof(buf), NULL, 0, NULL),
			==, 0);
	g_assert_cmpuint(hdr.op, ==, IPC_OP_REPLY_OK);
	g_assert_cmpmem(buf, hdr.payload_len, "ok", 2);
	avernella_supervisor_stop(&slot);
}

static int fresh_state_worker(int ctrl_fd, uint32_t active, void *userdata)
{
	const char *session_file = userdata;
	struct avernella_ipc_hdr hdr;
	int fd;

	if (!(active & AVSB_ACTIVE_LANDLOCK_FS))
		return 10;

	/* Fresh install: state dir must be created & writable for login. */
	fd = open(session_file, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return 11;
	if (write(fd, "{\"access\":\"ok\"}", 15) != 15) {
		close(fd);
		return 12;
	}
	close(fd);

	/* /etc/passwd remains blocked. */
	fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
	if (fd >= 0 || errno != EACCES)
		return 13;

	memset(&hdr, 0, sizeof(hdr));
	hdr.op = IPC_OP_REPLY_OK;
	hdr.payload_len = 2;
	(void)avernella_ipc_send(ctrl_fd, &hdr, "ok", NULL, 0);
	return 0;
}

static void test_ui_landlock_fresh_state_dir(void)
{
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	struct avernella_ipc_hdr hdr;
	char buf[8];
	int status = 0;

	/* Point XDG_STATE_HOME at an empty directory (like a fresh Flatpak). */
	g_autofree char *fresh_root =
		g_dir_make_tmp("avernella-fresh-state-XXXXXX", NULL);
	g_assert_nonnull(fresh_root);
	g_autofree char *session_file =
		g_build_filename(fresh_root, "avernella", "session.json", NULL);

	g_autofree char *prev_state = g_strdup(g_getenv("XDG_STATE_HOME"));

	g_assert_true(g_setenv("XDG_STATE_HOME", fresh_root, TRUE));

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_UI;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_spawn_worker(&opts, fresh_state_worker,
					       session_file, &proc),
			==, 0);
	g_assert_cmpint(avernella_ipc_recv(proc.ctrl_fd, &hdr, buf, sizeof(buf),
					   NULL, 0, NULL),
			==, 0);
	g_assert_cmpuint(hdr.op, ==, IPC_OP_REPLY_OK);
	g_assert_cmpint(waitpid(proc.pid, &status, 0), ==, proc.pid);
	g_assert_true(WIFEXITED(status));
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
	proc.pid = -1;
	avernella_worker_close(&proc);

	if (prev_state)
		g_setenv("XDG_STATE_HOME", prev_state, TRUE);
	else
		g_unsetenv("XDG_STATE_HOME");
	unlink(session_file);
	g_autofree char *av_dir =
		g_build_filename(fresh_root, "avernella", NULL);
	rmdir(av_dir);
	rmdir(fresh_root);
}

static int sandboxed_vault_child(int ctrl_fd, uint32_t active, void *userdata)
{
	struct avernella_vault vault;
	int rc;

	(void)active;
	(void)userdata;
	if (avernella_vault_init(&vault, -1) < 0)
		return 1;
	rc = avernella_vault_run(&vault, ctrl_fd);
	avernella_vault_destroy(&vault);
	return rc < 0 ? 1 : 0;
}

static void test_vault_spawned_child_ecdh(void)
{
	struct avernella_sandbox_opts opts;
	struct avernella_worker_proc proc;
	g_autoptr(GError) err = NULL;
	guint8 priv[32], pub[32], got_pub[32], shared[32];
	size_t priv_len = sizeof(priv), pub_len = sizeof(pub);

	EVP_PKEY *kp = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
	g_assert_nonnull(kp);
	g_assert_cmpint(EVP_PKEY_get_raw_private_key(kp, priv, &priv_len), ==,
			1);
	g_assert_cmpint(EVP_PKEY_get_raw_public_key(kp, pub, &pub_len), ==, 1);
	EVP_PKEY_free(kp);

	g_autofree char *priv_b64 = g_base64_encode(priv, sizeof(priv));
	J *keys = obj();
	J *own = obj();

	json_object_object_add(own, "42", json_object_new_string(priv_b64));
	json_object_object_add(keys, "own", own);

	memset(&opts, 0, sizeof(opts));
	opts.role = SANDBOX_ROLE_VAULT;
	opts.keep_stderr = true;

	g_assert_cmpint(avernella_spawn_worker(&opts, sandboxed_vault_child,
					       NULL, &proc),
			==, 0);
	g_assert_cmpint(avernella_vault_client_ping(proc.ctrl_fd), ==, 0);
	g_assert_true(avernella_vault_client_load_session(
		proc.ctrl_fd, "u1234", "tok", "ref", keys, 1, -1, &err));
	g_assert_no_error(err);
	g_assert_true(avernella_vault_client_own_public(proc.ctrl_fd, 42,
							got_pub, &err));
	g_assert_no_error(err);
	g_assert_cmpmem(got_pub, sizeof(got_pub), pub, sizeof(pub));
	g_assert_true(avernella_vault_client_ecdh(proc.ctrl_fd, 42, pub, shared,
						  &err));
	g_assert_no_error(err);
	g_assert_true(avernella_vault_client_wipe(proc.ctrl_fd, &err));
	g_assert_no_error(err);

	json_object_put(keys);
	avernella_worker_close(&proc);
}

static void test_vault_reconnect(void)
{
	struct avernella_worker_proc broker;

	g_autoptr(GError) error = NULL;
	g_autofree char *root =
		g_dir_make_tmp("avernella-vault-XXXXXX", &error);
	Line session = { .mid = g_strdup("u00000000000000000000000000000001"),
			 .access = g_strdup("fixture-token"),
			 .refresh = g_strdup("fixture-refresh"),
			 .refresh_at = 1234,
			 .keys = make_self_key_fixture(),
			 .service_code = -1 };
	guint8 pub[32], shared[32];

	g_assert_no_error(error);
	g_assert_nonnull(root);
	int dir = open(root, O_DIRECTORY | O_RDONLY | O_CLOEXEC);

	g_assert_cmpint(dir, >=, 0);
	g_assert_cmpint(flock(dir, LOCK_EX | LOCK_NB), ==, 0);
	g_assert_true(line_store_save(dir, &session, &error));
	g_assert_no_error(error);
	g_assert_cmpint(avernella_vault_broker_start(&broker), ==, 0);
	int fd = avernella_vault_reconnect(broker.ctrl_fd);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(avernella_vault_client_ping(fd), ==, 0);
	g_assert_true(
		avernella_vault_client_restore(fd, &session, dir, &error));
	g_assert_no_error(error);
	g_assert_true(avernella_vault_client_own_public(fd, 1, pub, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(pub, sizeof(pub), test_self_pub, sizeof(test_self_pub));
	/* The worker can release its private keys once the Vault adopts them. */
	json_object_object_del(session.keys, "own");
	gint32 first = avernella_vault_client_reserve_seq(fd, &error);

	g_assert_no_error(error);
	g_assert_cmpint(first, >, 0);
	g_assert_cmpint(shutdown(fd, SHUT_WR), ==, 0);
	g_assert_false(avernella_vault_client_own_public(fd, 1, pub, &error));
	g_assert_true(avernella_vault_disconnected(error));
	g_clear_error(&error);
	close(fd);

	fd = avernella_vault_reconnect(broker.ctrl_fd);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(
		avernella_vault_client_restore(fd, &session, dir, &error));
	g_assert_no_error(error);
	g_assert_true(avernella_vault_client_own_public(fd, 1, pub, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(pub, sizeof(pub), test_self_pub, sizeof(test_self_pub));
	g_assert_false(avernella_vault_client_own_public(fd, 2, pub, &error));
	g_assert_error(error, LINE_ERROR, LINE_OWN_KEY_MISSING);
	g_clear_error(&error);
	g_assert_false(avernella_vault_client_ecdh(fd, 2, test_self_pub, shared,
						   &error));
	g_assert_error(error, LINE_ERROR, LINE_OWN_KEY_MISSING);
	g_clear_error(&error);
	g_assert_true(avernella_vault_client_ecdh(fd, 1, test_self_pub, shared,
						  &error));
	g_assert_no_error(error);
	gint32 next = avernella_vault_client_reserve_seq(fd, &error);

	g_assert_no_error(error);
	g_assert_cmpint(next, >, first);
	/* A failed atomic save must reach the caller and leave the old file. */
	g_assert_cmpint(mkdirat(dir, "session.new", 0700), ==, 0);
	g_assert_false(avernella_vault_client_rotate_token(
		fd, "updated-fixture-token", session.refresh, 5678, &error));
	g_assert_nonnull(error);
	g_assert_false(avernella_vault_disconnected(error));
	g_clear_error(&error);
	g_assert_cmpint(unlinkat(dir, "session.new", AT_REMOVEDIR), ==, 0);
	g_assert_true(avernella_vault_client_rotate_token(
		fd, "updated-fixture-token", session.refresh, 5678, &error));
	g_assert_no_error(error);
	Line saved = { .service_code = -1 };

	g_assert_true(line_store_load(dir, &saved, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(saved.access, ==, "updated-fixture-token");
	g_assert_cmpint(saved.refresh_at, ==, 5678);
	g_assert_nonnull(field(saved.keys, "own"));
	g_assert_cmpint(saved.seq, >=, next);
	line_session_clear(&saved);
	line_session_clear(&session);
	close(fd);
	g_assert_cmpint(avernella_vault_disconnect(broker.ctrl_fd), ==, 0);
	avernella_worker_close(&broker);
	g_assert_true(line_store_forget(dir, &error));
	g_assert_no_error(error);
	close(dir);
	g_assert_cmpint(rmdir(root), ==, 0);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/security/probe", test_probe);
	g_test_add_func("/security/net-worker-landlock-and-fd-hygiene",
			test_net_worker_landlock_and_fd_hygiene);
	g_test_add_func("/security/net-worker-empty-root-and-netns",
			test_net_worker_empty_root_and_netns);
	g_test_add_func("/security/seccomp-kills-attacks",
			test_seccomp_kills_attacks);
	g_test_add_func("/security/media-worker-isolation",
			test_media_worker_isolation);
	g_test_add_func("/security/ui-landlock-blocks-secrets",
			test_ui_landlock_blocks_secrets);
	g_test_add_func("/security/ui-landlock-fresh-state-dir",
			test_ui_landlock_fresh_state_dir);
	g_test_add_func("/security/vault-memfd-secret-and-ipc",
			test_vault_memfd_secret_and_ipc);
	g_test_add_func("/security/vault-spawned-child-ecdh",
			test_vault_spawned_child_ecdh);
	g_test_add_func("/security/vault-reconnect", test_vault_reconnect);
	g_test_add_func("/security/supervisor-respawn-after-sigsys",
			test_supervisor_respawn_after_sigsys);
	return g_test_run();
}
