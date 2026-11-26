/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Supervisor & Key Vault process:
 *   - Stores LINE session tokens and Letter Sealing private keys inside
 *     memfd_secret() pages (fallback: mmap + mlock + MADV_DONTDUMP +
 *     PR_SET_DUMPABLE(0)).
 *   - Never parses untrusted Thrift frames, network media, or web content.
 *   - Performs Letter Sealing encrypt/decrypt, request header signing, and
 *     sequence reservation over authenticated SOCK_SEQPACKET IPC.
 *   - Supervises sandboxed workers and respawns them cleanly after crashes.
 */
#ifndef AVERNELLA_SECURITY_VAULT_H
#define AVERNELLA_SECURITY_VAULT_H

#include "line.h"
#include "security/ipc.h"
#include "security/sandbox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct avernella_secret_buf {
	void *ptr;
	size_t len;
	size_t map_len;
	bool from_memfd_secret;
	bool locked;
};

/*
 * Allocates a page-aligned secret buffer backed by memfd_secret() when
 * available, falling back to anonymous mmap + mlock + MADV_DONTDUMP +
 * PR_SET_DUMPABLE(0). Closes the memfd_secret descriptor immediately after
 * mapping so no secret FD can leak across fork().
 */
int avernella_secret_alloc(struct avernella_secret_buf *buf, size_t len);
void avernella_secret_free(struct avernella_secret_buf *buf);

struct avernella_vault_secrets {
	char access[16384];
	char refresh[16384];
	char obs_token[16384];
	char mid[64];
};

struct avernella_vault {
	struct avernella_secret_buf mem;
	struct avernella_vault_secrets *secrets;
	Line session;
	int dirfd;
	uint32_t next_req_id;
};

/*
 * Initializes an in-memory Key Vault with memfd_secret() backing storage.
 * dirfd may be an open 0700 state directory FD (owned by vault) or -1 for
 * ephemeral / unit-test vaults.
 */
int avernella_vault_init(struct avernella_vault *vault, int dirfd);
void avernella_vault_destroy(struct avernella_vault *vault);

/* Copies session tokens/MID into the secret page and adopts key material. */
int avernella_vault_load_secrets(struct avernella_vault *vault, const char *mid,
				 const char *access, const char *refresh,
				 J *keys, gint32 seq);

/* Serves one authenticated SOCK_SEQPACKET request on client_fd. */
int avernella_vault_serve_once(struct avernella_vault *vault, int client_fd);

/* Runs the Vault IPC event loop until EOF or fatal error. */
int avernella_vault_run(struct avernella_vault *vault, int client_fd);

/* Client-side helpers called by sandboxed workers (no secrets in worker). */
int avernella_vault_client_ping(int vault_fd);
J *avernella_vault_client_public_info(int vault_fd, GError **error);
J *avernella_vault_client_seal(int vault_fd, const char *chat, int content_type,
			       J *payload, GError **error);
J *avernella_vault_client_open(int vault_fd, J *message, int content_type,
			       GError **error);
gint32 avernella_vault_client_reserve_seq(int vault_fd, GError **error);

/* Supervisor worker slot with bounded crash-respawn policy. */
struct avernella_supervised_worker {
	struct avernella_sandbox_opts opts;
	avernella_worker_fn entry;
	void *userdata;
	struct avernella_worker_proc proc;
	uint32_t restarts;
	uint32_t max_restarts;
	int last_status;
};

int avernella_supervisor_start(struct avernella_supervised_worker *slot,
			       const struct avernella_sandbox_opts *opts,
			       avernella_worker_fn entry, void *userdata,
			       uint32_t max_restarts);

/*
 * Polls child status without blocking. If the worker crashed or was killed
 * (e.g. SIGSYS / SIGSEGV) and restarts < max_restarts, respawns it cleanly
 * and returns 1. Returns 0 if still running, or -errno if unrecoverable.
 */
int avernella_supervisor_poll_respawn(struct avernella_supervised_worker *slot);
void avernella_supervisor_stop(struct avernella_supervised_worker *slot);

#endif /* AVERNELLA_SECURITY_VAULT_H */
