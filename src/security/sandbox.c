// SPDX-License-Identifier: GPL-3.0-or-later
#include "security/sandbox.h"
#include "security/ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/landlock.h>
#include <sched.h>
#include <seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif

#ifndef SYS_memfd_secret
#define SYS_memfd_secret 447
#endif

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#ifndef LANDLOCK_ACCESS_FS_RESOLVE_UNIX
#define LANDLOCK_ACCESS_FS_RESOLVE_UNIX (1ULL << 16)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#endif
#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif
#ifndef LANDLOCK_RESTRICT_SELF_TSYNC
#define LANDLOCK_RESTRICT_SELF_TSYNC (1U << 3)
#endif

#define AVSB_PR_SET_MDWE 65
#define AVSB_PR_GET_MDWE 66
#define AVSB_MDWE_REFUSE_EXEC_GAIN (1UL << 0)
#define AVSB_MDWE_NO_INHERIT (1UL << 1)

/* Full ABI-gated filesystem access masks. */
#define AVSB_FS_ABI1_ALL                                                  \
	(LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |     \
	 LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |     \
	 LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
	 LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |     \
	 LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |     \
	 LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |   \
	 LANDLOCK_ACCESS_FS_MAKE_SYM)

#define AVSB_FS_RO_BASE \
	(LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR)

#define AVSB_FS_RW_BASE                                                   \
	(AVSB_FS_RO_BASE | LANDLOCK_ACCESS_FS_WRITE_FILE |                \
	 LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
	 LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |      \
	 LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_SYM)

#define AVSB_CLONE_NS_MASK                                             \
	(CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | \
	 CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_VFORK)

static int probe_landlock_abi(void)
{
	long abi = syscall(SYS_landlock_create_ruleset, NULL, 0,
			   LANDLOCK_CREATE_RULESET_VERSION);

	return abi > 0 ? (int)abi : 0;
}

static bool probe_memfd_secret(void)
{
	int fd = (int)syscall(SYS_memfd_secret, O_CLOEXEC);

	if (fd < 0)
		return false;
	close(fd);
	return true;
}

static bool probe_userns(void)
{
	char buf[8];
	int fd = open("/proc/sys/kernel/unprivileged_userns_clone",
		      O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);

		close(fd);
		if (n > 0 && buf[0] != '1')
			return false;
	}
	/*
	 * Ubuntu 24.04+ restricts unprivileged userns capabilities (including
	 * mount/pivot_root) via AppArmor when this sysctl is 1.
	 */
	fd = open("/proc/sys/kernel/apparmor_restrict_unprivileged_userns",
		  O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);

		close(fd);
		if (n > 0 && buf[0] == '1')
			return false;
	}
	return true;
}

void avernella_sandbox_probe(struct avernella_sandbox_caps *caps)
{
	if (!caps)
		return;
	memset(caps, 0, sizeof(*caps));
	caps->landlock_abi = probe_landlock_abi();
	caps->has_memfd_secret = probe_memfd_secret();
	caps->has_userns = probe_userns();
	caps->has_mdwe = prctl(AVSB_PR_GET_MDWE, 0, 0, 0, 0) >= 0;
	caps->has_seccomp = prctl(PR_GET_SECCOMP, 0, 0, 0, 0) >= 0;
}

static int write_proc_file(const char *path, const char *content)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	size_t len = strlen(content);
	ssize_t n;
	int err = 0;

	if (fd < 0)
		return -errno;
	n = write(fd, content, len);
	if (n < 0)
		err = -errno;
	else if ((size_t)n != len)
		err = -EIO;
	close(fd);
	return err;
}

int avernella_enter_empty_root_ns(bool isolate_net)
{
	uid_t uid = geteuid();
	gid_t gid = getegid();
	int ns_flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS |
		       CLONE_NEWIPC | CLONE_NEWUTS;
	char map[64];
	char tmp_dir[] = "/tmp/.avernella-root-XXXXXX";
	char old_root[PATH_MAX];
	int err;

	if (isolate_net)
		ns_flags |= CLONE_NEWNET;

	if (unshare(ns_flags) < 0)
		return -errno;

	/* Map current uid/gid to 0 inside the unprivileged user namespace. */
	(void)write_proc_file("/proc/self/setgroups", "deny");
	snprintf(map, sizeof(map), "0 %u 1\n", (unsigned int)uid);
	err = write_proc_file("/proc/self/uid_map", map);
	if (err)
		return err;
	snprintf(map, sizeof(map), "0 %u 1\n", (unsigned int)gid);
	err = write_proc_file("/proc/self/gid_map", map);
	if (err)
		return err;

	/* Prevent mount propagation back to the host namespace. */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		return -errno;

	if (!mkdtemp(tmp_dir))
		return -errno;

	if (mount("none", tmp_dir, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
		  "size=64k,mode=0755") < 0) {
		err = -errno;
		rmdir(tmp_dir);
		return err;
	}

	snprintf(old_root, sizeof(old_root), "%s/old", tmp_dir);
	if (mkdir(old_root, 0700) < 0) {
		err = -errno;
		umount2(tmp_dir, MNT_DETACH);
		rmdir(tmp_dir);
		return err;
	}

	if (syscall(SYS_pivot_root, tmp_dir, old_root) < 0) {
		err = -errno;
		rmdir(old_root);
		umount2(tmp_dir, MNT_DETACH);
		rmdir(tmp_dir);
		return err;
	}

	if (chdir("/") < 0)
		return -errno;
	if (umount2("/old", MNT_DETACH) < 0)
		return -errno;
	if (rmdir("/old") < 0)
		return -errno;

	/* Lock the empty root filesystem strictly read-only. */
	if (mount(NULL, "/", NULL,
		  MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV |
			  MS_NOEXEC,
		  NULL) < 0) {
		/* Fallback for kernels requiring direct MS_REMOUNT on tmpfs. */
		if (mount(NULL, "/", NULL,
			  MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV |
				  MS_NOEXEC,
			  NULL) < 0)
			return -errno;
	}
	return 0;
}

static uint64_t fs_mask_for_abi(int abi)
{
	uint64_t mask = AVSB_FS_ABI1_ALL;

	if (abi >= 2)
		mask |= LANDLOCK_ACCESS_FS_REFER;
	if (abi >= 3)
		mask |= LANDLOCK_ACCESS_FS_TRUNCATE;
	if (abi >= 5)
		mask |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
	if (abi >= 9)
		mask |= LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
	return mask;
}

static int add_path_rule(int ruleset, const char *path, uint64_t allowed,
			 uint64_t handled_fs)
{
	struct landlock_path_beneath_attr attr;
	struct stat st;
	int fd;
	int err = 0;

	if (!path || !*path)
		return 0;
	fd = open(path, O_PATH | O_CLOEXEC);
	if (fd < 0)
		return (errno == ENOENT || errno == ENOTDIR) ? 0 : -errno;

	/* Regular files/sockets reject directory-only Landlock access flags. */
	if (fstat(fd, &st) < 0) {
		err = -errno;
		goto out;
	}
	allowed &= handled_fs;
	if (!S_ISDIR(st.st_mode)) {
		uint64_t file_only = LANDLOCK_ACCESS_FS_EXECUTE |
				     LANDLOCK_ACCESS_FS_WRITE_FILE |
				     LANDLOCK_ACCESS_FS_READ_FILE |
				     LANDLOCK_ACCESS_FS_TRUNCATE |
				     LANDLOCK_ACCESS_FS_IOCTL_DEV |
				     LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
		allowed &= file_only;
	}
	if (!allowed)
		goto out;

	memset(&attr, 0, sizeof(attr));
	attr.allowed_access = allowed;
	attr.parent_fd = fd;
	if (syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH,
		    &attr, 0) < 0)
		err = -errno;
out:
	close(fd);
	return err;
}

static int add_net_rule(int ruleset, uint16_t port, uint64_t access)
{
	struct landlock_net_port_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.allowed_access = access;
	attr.port = port;
	if (syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_NET_PORT,
		    &attr, 0) < 0)
		return -errno;
	return 0;
}

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

static void build_xdg_base(char *buf, size_t len, const char *env_var,
			   const char *home_rel)
{
	const char *base = getenv(env_var);
	const char *home = getenv("HOME");

	buf[0] = '\0';
	if (base && base[0] == '/')
		snprintf(buf, len, "%s", base);
	else if (home && home[0] == '/')
		snprintf(buf, len, "%s/%s", home, home_rel);
}

static void build_xdg_path(char *buf, size_t len, const char *env_var,
			   const char *home_rel, const char *sub)
{
	char base[PATH_MAX];

	buf[0] = '\0';
	build_xdg_base(base, sizeof(base), env_var, home_rel);
	if (base[0])
		snprintf(buf, len, "%s/%s", base, sub);
}

/*
 * Landlock rules bind to an existing inode via open(O_PATH). On a fresh
 * Flatpak or native install, Avernella's XDG subdirectories do not exist yet
 * when the worker/UI enters Landlock, so add_path_rule() would skip them on
 * ENOENT and later session creation would fail with EACCES. Create missing
 * directories with 0700 permissions before attaching the rule.
 */
static void ensure_dir_0700(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;

	if (!path || path[0] != '/')
		return;
	len = strlen(path);
	if (len >= sizeof(tmp))
		return;
	memcpy(tmp, path, len + 1);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		(void)mkdir(tmp, 0700);
		*p = '/';
	}
	(void)mkdir(tmp, 0700);
}

static bool has_legacy_state_dir(const char *state_base)
{
	static const char *const legacy[] = { "kitatide", "line-linux" };
	char probe[PATH_MAX];
	struct stat st;

	if (!state_base || state_base[0] != '/')
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(legacy); i++) {
		int n = snprintf(probe, sizeof(probe), "%s/%s", state_base,
				 legacy[i]);

		if (n > 0 && (size_t)n < sizeof(probe) &&
		    lstat(probe, &st) == 0)
			return true;
	}
	return false;
}

static int populate_ui_rules(int ruleset, uint64_t handled_fs, int abi,
			     const struct avernella_sandbox_opts *opts)
{
	static const char *const ro_dirs[] = {
		"/app",
		"/usr",
		"/lib",
		"/lib64",
		"/run/host",
		"/run/NetworkManager",
		"/run/resolvconf",
		"/etc/fonts",
		"/etc/ld.so.cache",
		"/etc/ld.so.conf",
		"/etc/ld.so.conf.d",
		"/etc/xdg",
		"/etc/vulkan",
		"/etc/egl",
		"/etc/glvnd",
		"/etc/ssl",
		"/etc/ca-certificates",
		"/etc/pki",
		"/etc/resolv.conf",
		"/etc/hosts",
		"/etc/nsswitch.conf",
		"/etc/localtime",
		"/etc/alsa",
		"/etc/pipewire",
		"/etc/pulse",
		"/sys/dev/char",
		"/sys/devices",
		"/sys/bus/pci",
		"/sys/class/drm",
		"/sys/class/video4linux",
		"/proc/self",
	};
	static const char *const rw_dev_dirs[] = {
		"/dev/null",	  "/dev/zero",
		"/dev/urandom",	  "/dev/random",
		"/dev/dri",	  "/dev/shm",
		"/tmp/.X11-unix", "/run/systemd/resolve",
	};
	uint64_t ro = AVSB_FS_RO_BASE;
	uint64_t ro_exec = AVSB_FS_RO_BASE | LANDLOCK_ACCESS_FS_EXECUTE;
	uint64_t rw = AVSB_FS_RW_BASE |
		      (abi >= 2 ? LANDLOCK_ACCESS_FS_REFER : 0) |
		      (abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0);
	uint64_t dev_rw = rw | (abi >= 5 ? LANDLOCK_ACCESS_FS_IOCTL_DEV : 0) |
			  (abi >= 9 ? LANDLOCK_ACCESS_FS_RESOLVE_UNIX : 0);
	char path[PATH_MAX];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(ro_dirs); i++) {
		uint64_t right = (!strcmp(ro_dirs[i], "/app") ||
				  !strcmp(ro_dirs[i], "/usr") ||
				  !strcmp(ro_dirs[i], "/lib") ||
				  !strcmp(ro_dirs[i], "/lib64")) ?
					 ro_exec :
					 ro;
		err = add_path_rule(ruleset, ro_dirs[i], right, handled_fs);
		if (err)
			return err;
	}
	for (size_t i = 0; i < ARRAY_SIZE(rw_dev_dirs); i++) {
		err = add_path_rule(ruleset, rw_dev_dirs[i], dev_rw,
				    handled_fs);
		if (err)
			return err;
	}
	if (opts && opts->allow_camera) {
		for (int i = 0; i < 8; i++) {
			snprintf(path, sizeof(path), "/dev/video%d", i);
			err = add_path_rule(ruleset, path, dev_rw, handled_fs);
			if (err)
				return err;
		}
	}

	/* Allow strictly Avernella's own XDG config, cache, data, runtime. */
	build_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config",
		       "avernella");
	if (path[0]) {
		ensure_dir_0700(path);
		err = add_path_rule(ruleset, path, rw, handled_fs);
		if (err)
			return err;
	}

	build_xdg_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
		       "avernella");
	if (path[0]) {
		ensure_dir_0700(path);
		err = add_path_rule(ruleset, path, rw, handled_fs);
		if (err)
			return err;
	}

	build_xdg_path(path, sizeof(path), "XDG_DATA_HOME", ".local/share",
		       "avernella");
	if (path[0]) {
		ensure_dir_0700(path);
		err = add_path_rule(ruleset, path, rw, handled_fs);
		if (err)
			return err;
	}

	{
		char state_base[PATH_MAX];
		struct stat st;

		build_xdg_base(state_base, sizeof(state_base), "XDG_STATE_HOME",
			       ".local/state");
		if (state_base[0]) {
			snprintf(path, sizeof(path), "%s/avernella",
				 state_base);
			if (lstat(path, &st) == 0) {
				err = add_path_rule(ruleset, path, rw,
						    handled_fs);
			} else if (has_legacy_state_dir(state_base)) {
				/*
				 * Preserve one-time migration from kitatide or
				 * line-linux without pre-creating avernella/.
				 */
				err = add_path_rule(ruleset, state_base, rw,
						    handled_fs);
			} else {
				ensure_dir_0700(path);
				err = add_path_rule(ruleset, path, rw,
						    handled_fs);
			}
			if (err)
				return err;
		}
	}

	if (runtime && runtime[0] == '/') {
		snprintf(path, sizeof(path), "%s/avernella", runtime);
		ensure_dir_0700(path);
		err = add_path_rule(ruleset, path, rw, handled_fs);
		if (err)
			return err;

		/* Wayland, PipeWire, PulseAudio, and D-Bus sockets only. */
		static const char *const rt_sockets[] = {
			"wayland-0",	      "wayland-1", "pipewire-0",
			"pipewire-0-manager", "bus",	   "pulse",
		};
		uint64_t sock_r =
			LANDLOCK_ACCESS_FS_READ_FILE |
			LANDLOCK_ACCESS_FS_WRITE_FILE |
			LANDLOCK_ACCESS_FS_READ_DIR |
			(abi >= 9 ? LANDLOCK_ACCESS_FS_RESOLVE_UNIX : 0);
		for (size_t i = 0; i < ARRAY_SIZE(rt_sockets); i++) {
			snprintf(path, sizeof(path), "%s/%s", runtime,
				 rt_sockets[i]);
			err = add_path_rule(ruleset, path, sock_r, handled_fs);
			if (err)
				return err;
		}
		const char *wl = getenv("WAYLAND_DISPLAY");

		if (wl && *wl && !strchr(wl, '/')) {
			snprintf(path, sizeof(path), "%s/%s", runtime, wl);
			err = add_path_rule(ruleset, path, sock_r, handled_fs);
			if (err)
				return err;
		}
	}
	return 0;
}

static int populate_media_rules(int ruleset, uint64_t handled_fs, int abi,
				const struct avernella_sandbox_opts *opts)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];
	uint64_t dev_r = LANDLOCK_ACCESS_FS_READ_FILE |
			 LANDLOCK_ACCESS_FS_WRITE_FILE |
			 LANDLOCK_ACCESS_FS_READ_DIR |
			 (abi >= 5 ? LANDLOCK_ACCESS_FS_IOCTL_DEV : 0);
	uint64_t sock_r = LANDLOCK_ACCESS_FS_READ_FILE |
			  LANDLOCK_ACCESS_FS_WRITE_FILE |
			  (abi >= 9 ? LANDLOCK_ACCESS_FS_RESOLVE_UNIX : 0);
	int err;

	if (opts->allow_gpu_ioctl) {
		err = add_path_rule(ruleset, "/dev/dri", dev_r, handled_fs);
		if (err)
			return err;
	}
	if (opts->allow_camera) {
		for (int i = 0; i < 8; i++) {
			snprintf(path, sizeof(path), "/dev/video%d", i);
			err = add_path_rule(ruleset, path, dev_r, handled_fs);
			if (err)
				return err;
		}
	}
	if (runtime && runtime[0] == '/') {
		snprintf(path, sizeof(path), "%s/pipewire-0", runtime);
		err = add_path_rule(ruleset, path, sock_r, handled_fs);
		if (err)
			return err;
	}
	return 0;
}

int avernella_landlock_apply(const struct avernella_sandbox_opts *opts,
			     uint32_t *active_mask)
{
	int abi = probe_landlock_abi();
	struct landlock_ruleset_attr attr;
	int ruleset = -1;
	uint32_t active = 0;
	int err = 0;

	if (!opts)
		return -EINVAL;
	if (abi < 1)
		return 0;

	memset(&attr, 0, sizeof(attr));

	/* Filesystem containment for NET (0 rules), MEDIA (scoped), UI (scoped). */
	if (opts->role == SANDBOX_ROLE_NET ||
	    opts->role == SANDBOX_ROLE_MEDIA || opts->role == SANDBOX_ROLE_UI) {
		attr.handled_access_fs = fs_mask_for_abi(abi);
		active |= AVSB_ACTIVE_LANDLOCK_FS;
	}

	/* TCP port containment (ABI 4+): deny all bind; allowlist connect. */
	if (abi >= 4 && (opts->role == SANDBOX_ROLE_NET ||
			 opts->role == SANDBOX_ROLE_MEDIA ||
			 opts->role == SANDBOX_ROLE_VAULT)) {
		attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP |
					  LANDLOCK_ACCESS_NET_CONNECT_TCP;
		active |= AVSB_ACTIVE_LANDLOCK_NET;
	}

	/* IPC & signal scopes (ABI 6+): block abstract UNIX & cross-domain kill. */
	if (abi >= 6) {
		attr.scoped = LANDLOCK_SCOPE_SIGNAL;
		if (opts->role != SANDBOX_ROLE_UI)
			attr.scoped |= LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET;
		active |= AVSB_ACTIVE_LANDLOCK_SCOPE;
	}

	ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr),
			       0);
	if (ruleset < 0) {
		err = -errno;
		goto out;
	}

	if (opts->role == SANDBOX_ROLE_UI) {
		err = populate_ui_rules(ruleset, attr.handled_access_fs, abi,
					opts);
		if (err)
			goto out;
	} else if (opts->role == SANDBOX_ROLE_MEDIA) {
		err = populate_media_rules(ruleset, attr.handled_access_fs, abi,
					   opts);
		if (err)
			goto out;
	}
	/* SANDBOX_ROLE_NET intentionally adds ZERO filesystem rules! */

	uint64_t ro = AVSB_FS_RO_BASE;
	uint64_t rw = AVSB_FS_RW_BASE |
		      (abi >= 2 ? LANDLOCK_ACCESS_FS_REFER : 0) |
		      (abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0);

	for (size_t i = 0; i < opts->n_extra_ro_paths; i++) {
		err = add_path_rule(ruleset, opts->extra_ro_paths[i], ro,
				    attr.handled_access_fs);
		if (err)
			goto out;
	}
	for (size_t i = 0; i < opts->n_extra_rw_paths; i++) {
		err = add_path_rule(ruleset, opts->extra_rw_paths[i], rw,
				    attr.handled_access_fs);
		if (err)
			goto out;
	}

	if (active & AVSB_ACTIVE_LANDLOCK_NET) {
		if (opts->role == SANDBOX_ROLE_NET && !opts->isolate_net_ns) {
			static const uint16_t default_ports[] = { 443 };
			const uint16_t *ports =
				opts->n_tcp_connect_ports ?
					opts->tcp_connect_ports :
					default_ports;
			size_t n_ports = opts->n_tcp_connect_ports ?
						 opts->n_tcp_connect_ports :
						 1;

			for (size_t i = 0; i < n_ports; i++) {
				err = add_net_rule(
					ruleset, ports[i],
					LANDLOCK_ACCESS_NET_CONNECT_TCP);
				if (err)
					goto out;
			}
		} else if (opts->n_tcp_connect_ports) {
			for (size_t i = 0; i < opts->n_tcp_connect_ports; i++) {
				err = add_net_rule(
					ruleset, opts->tcp_connect_ports[i],
					LANDLOCK_ACCESS_NET_CONNECT_TCP);
				if (err)
					goto out;
			}
		}
	}

	if (syscall(SYS_landlock_restrict_self, ruleset,
		    LANDLOCK_RESTRICT_SELF_TSYNC) < 0) {
		if (errno != EINVAL ||
		    syscall(SYS_landlock_restrict_self, ruleset, 0) < 0) {
			err = -errno;
			goto out;
		}
	}

	if (active_mask)
		*active_mask |= active;
out:
	if (ruleset >= 0)
		close(ruleset);
	return err;
}

static int allow_list(scmp_filter_ctx ctx, const int *syscalls, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, syscalls[i], 0);

		if (rc < 0)
			return rc;
	}
	return 0;
}

int avernella_seccomp_apply(const struct avernella_sandbox_opts *opts)
{
	/* Common glibc/threading/IPC/event-loop syscalls (no exec/fork/ns). */
	static const int common_syscalls[] = {
		SCMP_SYS(read),
		SCMP_SYS(write),
		SCMP_SYS(pread64),
		SCMP_SYS(pwrite64),
		SCMP_SYS(readv),
		SCMP_SYS(writev),
		SCMP_SYS(preadv),
		SCMP_SYS(pwritev),
		SCMP_SYS(close),
		SCMP_SYS(close_range),
		SCMP_SYS(lseek),
		SCMP_SYS(fstat),
		SCMP_SYS(newfstatat),
		SCMP_SYS(statx),
		SCMP_SYS(fcntl),
		SCMP_SYS(dup),
		SCMP_SYS(dup3),
		SCMP_SYS(pipe2),
		SCMP_SYS(poll),
		SCMP_SYS(ppoll),
		SCMP_SYS(epoll_create1),
		SCMP_SYS(epoll_ctl),
		SCMP_SYS(epoll_wait),
		SCMP_SYS(epoll_pwait),
		SCMP_SYS(epoll_pwait2),
		SCMP_SYS(eventfd2),
		SCMP_SYS(timerfd_create),
		SCMP_SYS(timerfd_settime),
		SCMP_SYS(timerfd_gettime),
		SCMP_SYS(memfd_create),
		SCMP_SYS(ftruncate),
		SCMP_SYS(brk),
		SCMP_SYS(munmap),
		SCMP_SYS(mremap),
		SCMP_SYS(madvise),
		SCMP_SYS(futex),
		SCMP_SYS(futex_waitv),
		SCMP_SYS(set_robust_list),
		SCMP_SYS(get_robust_list),
		SCMP_SYS(rseq),
		SCMP_SYS(membarrier),
		SCMP_SYS(sched_yield),
		SCMP_SYS(sched_getaffinity),
		SCMP_SYS(clock_gettime),
		SCMP_SYS(clock_getres),
		SCMP_SYS(clock_nanosleep),
		SCMP_SYS(nanosleep),
		SCMP_SYS(gettimeofday),
		SCMP_SYS(getrandom),
		SCMP_SYS(rt_sigaction),
		SCMP_SYS(rt_sigprocmask),
		SCMP_SYS(rt_sigreturn),
		SCMP_SYS(sigaltstack),
		SCMP_SYS(tgkill),
		SCMP_SYS(getpid),
		SCMP_SYS(gettid),
		SCMP_SYS(getuid),
		SCMP_SYS(geteuid),
		SCMP_SYS(getgid),
		SCMP_SYS(getegid),
		SCMP_SYS(exit),
		SCMP_SYS(exit_group),
		SCMP_SYS(restart_syscall),
		/* IPC socket operations (also used by NET & MEDIA). */
		SCMP_SYS(sendto),
		SCMP_SYS(recvfrom),
		SCMP_SYS(sendmsg),
		SCMP_SYS(recvmsg),
		SCMP_SYS(sendmmsg),
		SCMP_SYS(recvmmsg),
		SCMP_SYS(shutdown),
		SCMP_SYS(getsockname),
		SCMP_SYS(getpeername),
		SCMP_SYS(getsockopt),
		SCMP_SYS(setsockopt),
		SCMP_SYS(socketpair),
		/* Open/connect are bounded by empty root + Landlock ABI 1..11. */
		SCMP_SYS(openat),
		SCMP_SYS(connect),
	};
	static const int net_extra[] = {
		SCMP_SYS(
			bind), /* TCP blocked by Landlock; needed for netlink/UDP */
		SCMP_SYS(getdents64),
	};
	static const int media_extra[] = {
		SCMP_SYS(bind),
		SCMP_SYS(sched_setscheduler),
		SCMP_SYS(sched_getscheduler),
		SCMP_SYS(sched_setparam),
		SCMP_SYS(sched_getparam),
		SCMP_SYS(sched_get_priority_min),
		SCMP_SYS(sched_get_priority_max),
	};
	static const int vault_extra[] = {
		SCMP_SYS(flock),    SCMP_SYS(fsync),	 SCMP_SYS(fdatasync),
		SCMP_SYS(renameat), SCMP_SYS(renameat2), SCMP_SYS(unlinkat),
		SCMP_SYS(mkdirat),  SCMP_SYS(fchmod),	 SCMP_SYS(memfd_secret),
		SCMP_SYS(mlock),    SCMP_SYS(munlock),
	};
	static const int allowed_prctl[] = {
		PR_SET_NAME,	     PR_GET_NAME,      PR_SET_TIMERSLACK,
		PR_GET_TIMERSLACK,   AVSB_PR_GET_MDWE, PR_GET_DUMPABLE,
		PR_GET_NO_NEW_PRIVS,
	};
	scmp_filter_ctx ctx = NULL;
	int rc;

	if (!opts)
		return -EINVAL;
	if (opts->role == SANDBOX_ROLE_UI)
		return 0; /* UI relies on Landlock + MDWE(NO_INHERIT) + mseal */

	ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
	if (!ctx)
		return -ENOMEM;

	/* Synchronize across all existing threads when supported. */
	(void)seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1);

	rc = allow_list(ctx, common_syscalls, ARRAY_SIZE(common_syscalls));
	if (rc < 0)
		goto out;

	/* Ban PROT_EXEC on mmap() and mprotect() at BPF level (SIGSYS kill). */
	rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 1,
			      SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0));
	if (rc < 0)
		goto out;
	rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 1,
			      SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0));
	if (rc < 0)
		goto out;

	/*
	 * Force glibc pthread_create() to fall back from clone3() to clone(),
	 * where BPF enforces CLONE_THREAD and rejects all CLONE_NEW* & fork().
	 */
	rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0);
	if (rc < 0)
		goto out;
	rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clone), 1,
			      SCMP_A0(SCMP_CMP_MASKED_EQ,
				      CLONE_THREAD | AVSB_CLONE_NS_MASK,
				      CLONE_THREAD));
	if (rc < 0)
		goto out;

	/* Allow only safe prctl() inspection/thread-naming operations. */
	for (size_t i = 0; i < ARRAY_SIZE(allowed_prctl); i++) {
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(prctl), 1,
				      SCMP_A0(SCMP_CMP_EQ, allowed_prctl[i]));
		if (rc < 0)
			goto out;
	}

	/* Restrict socket() domains strictly by role. */
	rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
			      SCMP_A0(SCMP_CMP_EQ, AF_UNIX));
	if (rc < 0)
		goto out;
	if (opts->role == SANDBOX_ROLE_NET ||
	    opts->role == SANDBOX_ROLE_MEDIA) {
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
				      SCMP_A0(SCMP_CMP_EQ, AF_INET));
		if (rc < 0)
			goto out;
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
				      SCMP_A0(SCMP_CMP_EQ, AF_INET6));
		if (rc < 0)
			goto out;
	}
	if (opts->role == SANDBOX_ROLE_NET) {
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
				      SCMP_A0(SCMP_CMP_EQ, AF_NETLINK));
		if (rc < 0)
			goto out;
		rc = allow_list(ctx, net_extra, ARRAY_SIZE(net_extra));
		if (rc < 0)
			goto out;
		/* Safe non-device socket ioctls only (FIONREAD / FIONBIO). */
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
				      SCMP_A1(SCMP_CMP_EQ, FIONREAD));
		if (rc < 0)
			goto out;
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
				      SCMP_A1(SCMP_CMP_EQ, FIONBIO));
		if (rc < 0)
			goto out;
	} else if (opts->role == SANDBOX_ROLE_MEDIA) {
		rc = allow_list(ctx, media_extra, ARRAY_SIZE(media_extra));
		if (rc < 0)
			goto out;
		/*
		 * Allow only DRM ('d' = 0x64) and V4L2 ('V' = 0x56) ioctl
		 * type bytes when GPU/camera are enabled; TTY ioctls such as
		 * TIOCSTI/TIOCLINUX ('T' = 0x54) are killed with SIGSYS.
		 */
		if (opts->allow_gpu_ioctl) {
			rc = seccomp_rule_add(
				ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
				SCMP_A1(SCMP_CMP_MASKED_EQ, 0xff00U,
					(unsigned int)'d' << 8));
			if (rc < 0)
				goto out;
		}
		if (opts->allow_camera) {
			rc = seccomp_rule_add(
				ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
				SCMP_A1(SCMP_CMP_MASKED_EQ, 0xff00U,
					(unsigned int)'V' << 8));
			if (rc < 0)
				goto out;
		}
	} else if (opts->role == SANDBOX_ROLE_VAULT) {
		rc = allow_list(ctx, vault_extra, ARRAY_SIZE(vault_extra));
		if (rc < 0)
			goto out;
	}

	rc = seccomp_load(ctx);
out:
	if (ctx)
		seccomp_release(ctx);
	return rc;
}

int avernella_sandbox_apply(const struct avernella_sandbox_opts *opts,
			    uint32_t *active_mask)
{
	uint32_t active = 0;
	unsigned long mdwe_flags = AVSB_MDWE_REFUSE_EXEC_GAIN;
	int err;

	if (!opts)
		return -EINVAL;

	/*
	 * Enter namespaces + pivot_root() BEFORE PR_SET_DUMPABLE(0),
	 * PR_SET_NO_NEW_PRIVS, and Landlock/seccomp: PR_SET_DUMPABLE(0) makes
	 * /proc/self/{uid_map,gid_map,setgroups} root-owned, which would break
	 * unprivileged user-namespace mapping with EACCES.
	 */
	if (opts->isolate_mount_ns) {
		err = avernella_enter_empty_root_ns(opts->isolate_net_ns);
		if (err == 0) {
			active |= AVSB_ACTIVE_EMPTY_ROOT;
			if (opts->isolate_net_ns)
				active |= AVSB_ACTIVE_NET_NS;
		} else if (err != -EPERM && err != -ENOSYS && err != -EINVAL) {
			goto out;
		}
	} else if (opts->isolate_net_ns) {
		if (unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0)
			active |= AVSB_ACTIVE_NET_NS;
	}

	if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0)
		active |= AVSB_ACTIVE_NO_DUMP;

	if (opts->role == SANDBOX_ROLE_UI)
		mdwe_flags |= AVSB_MDWE_NO_INHERIT;
	if (prctl(AVSB_PR_SET_MDWE, mdwe_flags, 0, 0, 0) == 0)
		active |= AVSB_ACTIVE_MDWE;

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		err = -errno;
		goto out;
	}
	active |= AVSB_ACTIVE_NO_NEW_PRIVS;

	err = avernella_landlock_apply(opts, &active);
	if (err)
		goto out;

	err = avernella_seccomp_apply(opts);
	if (err)
		goto out;
	if (opts->role != SANDBOX_ROLE_UI)
		active |= AVSB_ACTIVE_SECCOMP;

	err = 0;
out:
	if (active_mask)
		*active_mask = active;
	return err;
}

int avernella_spawn_worker(const struct avernella_sandbox_opts *opts,
			   avernella_worker_fn entry, void *userdata,
			   struct avernella_worker_proc *out)
{
	int pair[2] = { -1, -1 };
	pid_t parent_pid = getpid();
	pid_t pid;
	int pidfd = -1;
	int err;

	if (!opts || !entry || !out)
		return -EINVAL;

	out->pid = -1;
	out->pidfd = -1;
	out->ctrl_fd = -1;

	err = avernella_ipc_socketpair(pair);
	if (err)
		return err;

	pid = fork();
	if (pid < 0) {
		err = -errno;
		close(pair[0]);
		close(pair[1]);
		return err;
	}

	if (pid == 0) {
		/* Child: async-signal-safe FD sanitization before sandbox. */
		int devnull;
		int ctrl_slot = 3;
		uint32_t active = 0;
		int rc;

		if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 ||
		    getppid() != parent_pid)
			_exit(126);

		close(pair[0]);
		if (pair[1] != ctrl_slot) {
			if (dup2(pair[1], ctrl_slot) < 0)
				_exit(125);
			close(pair[1]);
		}
		fcntl(ctrl_slot, F_SETFD, FD_CLOEXEC);

		devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (devnull >= 0) {
			(void)dup2(devnull, STDIN_FILENO);
			(void)dup2(devnull, STDOUT_FILENO);
			if (!opts->keep_stderr)
				(void)dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}

		/* Strip EVERY stray FD before entering Landlock/seccomp! */
		if (avernella_close_extra_fds(ctrl_slot) < 0)
			_exit(124);

		if (avernella_sandbox_apply(opts, &active) < 0)
			_exit(123);

		rc = entry(ctrl_slot, active, userdata);
		_exit(rc & 0xff);
	}

	/* Parent: close child socket end and open pidfd for supervision. */
	close(pair[1]);
	pidfd = (int)syscall(SYS_pidfd_open, pid, 0U);
	out->pid = pid;
	out->pidfd = pidfd; /* -1 on pre-5.3 kernels; waitpid() still works */
	out->ctrl_fd = pair[0];
	return 0;
}

void avernella_worker_close(struct avernella_worker_proc *proc)
{
	if (!proc)
		return;
	if (proc->ctrl_fd >= 0) {
		shutdown(proc->ctrl_fd, SHUT_RDWR);
		close(proc->ctrl_fd);
		proc->ctrl_fd = -1;
	}
	if (proc->pid > 0) {
		int status = 0;

		if (waitpid(proc->pid, &status, WNOHANG) == 0) {
			kill(proc->pid, SIGTERM);
			waitpid(proc->pid, &status, 0);
		}
		proc->pid = -1;
	}
	if (proc->pidfd >= 0) {
		close(proc->pidfd);
		proc->pidfd = -1;
	}
}
