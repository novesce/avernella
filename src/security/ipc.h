/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Authenticated SOCK_SEQPACKET IPC framing and SCM_RIGHTS descriptor passing
 * between the Supervisor/Key Vault and sandboxed Avernella workers.
 */
#ifndef AVERNELLA_SECURITY_IPC_H
#define AVERNELLA_SECURITY_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define AVERNELLA_IPC_MAGIC 0x41565342U /* "AVSB" */
#define AVERNELLA_IPC_VERSION 1U
#define AVERNELLA_IPC_MAX_PAYLOAD (64U * 1024U)
#define AVERNELLA_IPC_MAX_FDS 4U

#define AVERNELLA_IPC_FLAG_HAS_FDS (1U << 0)

enum avernella_ipc_op {
	/* Vault <-> Net Worker (secret-isolated crypto & session ops) */
	IPC_OP_VAULT_SEAL_PAYLOAD = 0x0101,
	IPC_OP_VAULT_OPEN_PAYLOAD = 0x0102,
	IPC_OP_VAULT_SIGN_REQUEST = 0x0103,
	IPC_OP_VAULT_IMPORT_KEYS = 0x0104,
	IPC_OP_VAULT_ROTATE_TOKEN = 0x0105,
	IPC_OP_VAULT_GET_PUBLIC_INFO = 0x0106,
	IPC_OP_VAULT_RESERVE_SEQ = 0x0107,
	IPC_OP_VAULT_WIPE_SESSION = 0x0108,
	IPC_OP_VAULT_ECDH = 0x010a,
	IPC_OP_VAULT_OWN_PUBLIC = 0x010b,
	IPC_OP_VAULT_RESTORE = 0x010c,
	IPC_OP_VAULT_RECONNECT = 0x010d,
	IPC_OP_VAULT_DISCONNECT = 0x010e,

	/* Supervisor <-> UI / Workers (brokered resources & health) */
	IPC_OP_SPAWN_LIFF = 0x0201,
	IPC_OP_OPEN_MEDIA_SOCKET = 0x0202,
	IPC_OP_MEDIA_FRAME = 0x0301,
	IPC_OP_PING = 0x0401,

	IPC_OP_REPLY_OK = 0x8000,
	IPC_OP_REPLY_ERR = 0x8001,
};

struct avernella_ipc_hdr {
	uint32_t magic;
	uint16_t version;
	uint16_t op;
	uint32_t req_id;
	uint32_t payload_len;
	int32_t status;
	uint32_t flags;
};

/*
 * Creates a SOCK_SEQPACKET | SOCK_CLOEXEC Unix socket pair.
 * Returns 0 on success or -errno on failure.
 */
int avernella_ipc_socketpair(int fds[2]);

/*
 * Sends one framed message with optional SCM_RIGHTS file descriptors.
 * Returns 0 on success or -errno on failure.
 */
int avernella_ipc_send(int sock, const struct avernella_ipc_hdr *hdr,
		       const void *payload, const int *pass_fds, size_t n_fds);

/*
 * Receives one framed message and any attached descriptors (with
 * MSG_CMSG_CLOEXEC). Rejects truncated messages or control data and closes
 * any stray descriptors on error. Returns 0 on success, -EPIPE on EOF, or
 * -errno on failure.
 */
int avernella_ipc_recv(int sock, struct avernella_ipc_hdr *hdr, void *payload,
		       size_t cap, int *recv_fds, size_t max_fds,
		       size_t *out_nfds);

/*
 * Closes every open descriptor strictly greater than max_keep_fd using
 * close_range(2) with a /proc/self/fd fallback. Safe to call in a freshly
 * forked single-threaded child before entering Landlock/seccomp.
 */
int avernella_close_extra_fds(int max_keep_fd);

#endif /* AVERNELLA_SECURITY_IPC_H */
