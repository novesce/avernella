// SPDX-License-Identifier: GPL-3.0-or-later
#include "security/ipc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef SYS_close_range
#define SYS_close_range 436
#endif

int avernella_ipc_socketpair(int fds[2])
{
	if (!fds)
		return -EINVAL;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0)
		return -errno;
	return 0;
}

int avernella_ipc_send(int sock, const struct avernella_ipc_hdr *hdr,
		       const void *payload, const int *pass_fds, size_t n_fds)
{
	struct avernella_ipc_hdr wire;
	struct iovec iov[2];
	struct msghdr msg;
	union {
		char buf[CMSG_SPACE(sizeof(int) * AVERNELLA_IPC_MAX_FDS)];
		struct cmsghdr align;
	} cmsg_buf;
	ssize_t sent;
	size_t total;

	if (sock < 0 || !hdr || n_fds > AVERNELLA_IPC_MAX_FDS)
		return -EINVAL;
	if (hdr->payload_len > AVERNELLA_IPC_MAX_PAYLOAD)
		return -EMSGSIZE;
	if (hdr->payload_len && !payload)
		return -EINVAL;
	if (n_fds && !pass_fds)
		return -EINVAL;

	wire = *hdr;
	wire.magic = AVERNELLA_IPC_MAGIC;
	wire.version = AVERNELLA_IPC_VERSION;
	if (n_fds)
		wire.flags |= AVERNELLA_IPC_FLAG_HAS_FDS;
	else
		wire.flags &= ~AVERNELLA_IPC_FLAG_HAS_FDS;

	iov[0].iov_base = &wire;
	iov[0].iov_len = sizeof(wire);
	iov[1].iov_base = (void *)payload;
	iov[1].iov_len = wire.payload_len;
	total = sizeof(wire) + wire.payload_len;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = wire.payload_len ? 2 : 1;

	if (n_fds) {
		struct cmsghdr *cmsg;

		memset(&cmsg_buf, 0, sizeof(cmsg_buf));
		msg.msg_control = cmsg_buf.buf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * n_fds);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n_fds);
		memcpy(CMSG_DATA(cmsg), pass_fds, sizeof(int) * n_fds);
	}

	do {
		sent = sendmsg(sock, &msg, MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);

	if (sent < 0)
		return -errno;
	if ((size_t)sent != total)
		return -EIO;
	return 0;
}

static void close_fd_list(int *fds, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (fds[i] >= 0) {
			close(fds[i]);
			fds[i] = -1;
		}
	}
}

int avernella_ipc_recv(int sock, struct avernella_ipc_hdr *hdr, void *payload,
		       size_t cap, int *recv_fds, size_t max_fds,
		       size_t *out_nfds)
{
	struct iovec iov[2];
	struct msghdr msg;
	union {
		char buf[CMSG_SPACE(sizeof(int) * AVERNELLA_IPC_MAX_FDS)];
		struct cmsghdr align;
	} cmsg_buf;
	struct cmsghdr *cmsg;
	int got_fds[AVERNELLA_IPC_MAX_FDS];
	size_t n_got = 0;
	ssize_t n;
	int err = 0;

	if (out_nfds)
		*out_nfds = 0;
	if (sock < 0 || !hdr || (cap && !payload))
		return -EINVAL;
	if (cap > AVERNELLA_IPC_MAX_PAYLOAD)
		cap = AVERNELLA_IPC_MAX_PAYLOAD;

	for (size_t i = 0; i < AVERNELLA_IPC_MAX_FDS; i++)
		got_fds[i] = -1;

	iov[0].iov_base = hdr;
	iov[0].iov_len = sizeof(*hdr);
	iov[1].iov_base = payload;
	iov[1].iov_len = cap;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = cap ? 2 : 1;
	msg.msg_control = cmsg_buf.buf;
	msg.msg_controllen = sizeof(cmsg_buf.buf);

	do {
		n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);

	if (n < 0)
		return -errno;
	if (n == 0)
		return -EPIPE;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_RIGHTS)
			continue;
		if (cmsg->cmsg_len < CMSG_LEN(0)) {
			err = -EPROTO;
			goto fail;
		}
		size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
		size_t count = bytes / sizeof(int);
		const unsigned char *data = CMSG_DATA(cmsg);

		for (size_t i = 0; i < count; i++) {
			int fd;

			memcpy(&fd, data + i * sizeof(int), sizeof(int));
			if (n_got < AVERNELLA_IPC_MAX_FDS) {
				got_fds[n_got++] = fd;
			} else {
				close(fd);
				err = -EMFILE;
			}
		}
	}

	if (err)
		goto fail;
	if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		err = -EMSGSIZE;
		goto fail;
	}
	if ((size_t)n < sizeof(*hdr) || hdr->magic != AVERNELLA_IPC_MAGIC ||
	    hdr->version != AVERNELLA_IPC_VERSION ||
	    hdr->payload_len > AVERNELLA_IPC_MAX_PAYLOAD ||
	    (size_t)n != sizeof(*hdr) + hdr->payload_len) {
		err = -EPROTO;
		goto fail;
	}
	if (n_got > max_fds || (n_got && !recv_fds)) {
		err = -EMFILE;
		goto fail;
	}

	for (size_t i = 0; i < n_got; i++)
		recv_fds[i] = got_fds[i];
	if (out_nfds)
		*out_nfds = n_got;
	return 0;

fail:
	close_fd_list(got_fds, n_got);
	return err;
}

int avernella_close_extra_fds(int max_keep_fd)
{
	unsigned int first = max_keep_fd < 0 ? 0U :
					       (unsigned int)max_keep_fd + 1U;
	DIR *dir;
	struct dirent *ent;
	int dir_fd, err;

	if (syscall(SYS_close_range, first, ~0U, 0U) == 0)
		return 0;
	/* An outer sandbox may block close_range while exposing /proc. */
	if (errno != ENOSYS && errno != EINVAL && errno != EPERM &&
	    errno != EACCES)
		return -errno;

	dir = opendir("/proc/self/fd");
	if (!dir)
		return -errno;
	dir_fd = dirfd(dir);
	for (;;) {
		char *end = NULL;
		long fd;

		errno = 0;
		ent = readdir(dir);
		if (!ent) {
			err = -errno;
			break;
		}
		if (ent->d_name[0] == '.')
			continue;
		errno = 0;
		fd = strtol(ent->d_name, &end, 10);
		if (errno || !end || *end != '\0' || fd < 0)
			continue;
		if ((unsigned int)fd >= first && (int)fd != dir_fd)
			close((int)fd);
	}
	closedir(dir);
	return err;
}
