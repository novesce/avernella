// SPDX-License-Identifier: GPL-3.0-or-later
#include "security/vault.h"
#include "sealing.h"
#include "state.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_memfd_secret
#define SYS_memfd_secret 447
#endif

#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 16
#endif

static size_t align_page(size_t n)
{
	long ps = sysconf(_SC_PAGESIZE);
	size_t page = ps > 0 ? (size_t)ps : 4096U;

	if (!n)
		return page;
	return (n + page - 1U) & ~(page - 1U);
}

int avernella_secret_alloc(struct avernella_secret_buf *buf, size_t len)
{
	size_t map_len;
	int fd;
	void *ptr;

	if (!buf || !len)
		return -EINVAL;
	memset(buf, 0, sizeof(*buf));
	map_len = align_page(len);

	/* Prefer kernel secretmem (unmapped from kernel direct map). */
	fd = (int)syscall(SYS_memfd_secret, O_CLOEXEC);
	if (fd >= 0) {
		if (ftruncate(fd, (off_t)map_len) == 0) {
			ptr = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd, 0);
			close(fd);
			if (ptr != MAP_FAILED) {
				buf->ptr = ptr;
				buf->len = len;
				buf->map_len = map_len;
				buf->from_memfd_secret = true;
				return 0;
			}
		} else {
			close(fd);
		}
	}

	/* Fallback: non-dumpable, mlocked anonymous mapping. */
	if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0)
		(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	ptr = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED)
		return -errno;

	(void)madvise(ptr, map_len, MADV_DONTDUMP);
	buf->locked = (mlock(ptr, map_len) == 0);
	buf->ptr = ptr;
	buf->len = len;
	buf->map_len = map_len;
	buf->from_memfd_secret = false;
	return 0;
}

void avernella_secret_free(struct avernella_secret_buf *buf)
{
	if (!buf || !buf->ptr)
		return;
	explicit_bzero(buf->ptr, buf->map_len);
	if (buf->locked)
		(void)munlock(buf->ptr, buf->map_len);
	munmap(buf->ptr, buf->map_len);
	memset(buf, 0, sizeof(*buf));
}

int avernella_vault_init(struct avernella_vault *vault, int dirfd)
{
	int err;

	if (!vault)
		return -EINVAL;
	memset(vault, 0, sizeof(*vault));
	vault->dirfd = dirfd;
	vault->session.service_code = -1;
	if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0)
		(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

	err = avernella_secret_alloc(&vault->mem,
				     sizeof(struct avernella_vault_secrets));
	if (err)
		return err;
	vault->secrets = (struct avernella_vault_secrets *)vault->mem.ptr;
	return 0;
}

void avernella_vault_destroy(struct avernella_vault *vault)
{
	if (!vault)
		return;
	/* Do not free pointers that alias the secret mapping directly. */
	vault->session.access = NULL;
	vault->session.refresh = NULL;
	vault->session.obs_token = NULL;
	vault->session.mid = NULL;
	line_session_clear(&vault->session);
	if (vault->dirfd >= 0) {
		close(vault->dirfd);
		vault->dirfd = -1;
	}
	avernella_secret_free(&vault->mem);
	vault->secrets = NULL;
}

static int copy_secret_str(char *dst, size_t cap, const char *src)
{
	size_t n;

	if (!src) {
		explicit_bzero(dst, cap);
		return 0;
	}
	n = strlen(src);
	if (n >= cap)
		return -E2BIG;
	memcpy(dst, src, n + 1);
	return 0;
}

int avernella_vault_load_secrets(struct avernella_vault *vault, const char *mid,
				 const char *access, const char *refresh,
				 J *keys, gint32 seq)
{
	int err;

	if (!vault || !vault->secrets)
		return -EINVAL;
	err = copy_secret_str(vault->secrets->mid, sizeof(vault->secrets->mid),
			      mid);
	if (err)
		return err;
	err = copy_secret_str(vault->secrets->access,
			      sizeof(vault->secrets->access), access);
	if (err)
		return err;
	err = copy_secret_str(vault->secrets->refresh,
			      sizeof(vault->secrets->refresh), refresh);
	if (err)
		return err;

	vault->session.mid = vault->secrets->mid[0] ? vault->secrets->mid :
						      NULL;
	vault->session.access =
		vault->secrets->access[0] ? vault->secrets->access : NULL;
	vault->session.refresh =
		vault->secrets->refresh[0] ? vault->secrets->refresh : NULL;
	vault->session.seq = seq;
	vault->session.seq_reserved = seq;
	if (vault->session.keys)
		json_object_put(vault->session.keys);
	vault->session.keys = keys ? json_object_get(keys) : NULL;
	return 0;
}

static int reply_json(int fd, uint32_t req_id, J *body)
{
	struct avernella_ipc_hdr hdr;
	size_t len = 0;
	const char *text = body ? json_object_to_json_string_length(
					  body, JSON_C_TO_STRING_PLAIN, &len) :
				  "";

	if (len > AVERNELLA_IPC_MAX_PAYLOAD)
		return -EMSGSIZE;
	memset(&hdr, 0, sizeof(hdr));
	hdr.op = IPC_OP_REPLY_OK;
	hdr.req_id = req_id;
	hdr.payload_len = (uint32_t)len;
	hdr.status = 0;
	return avernella_ipc_send(fd, &hdr, len ? text : NULL, NULL, 0);
}

static int reply_error(int fd, uint32_t req_id, int status, const char *msg)
{
	struct avernella_ipc_hdr hdr;
	size_t len = msg ? strlen(msg) : 0;

	if (len > 512)
		len = 512;
	memset(&hdr, 0, sizeof(hdr));
	hdr.op = IPC_OP_REPLY_ERR;
	hdr.req_id = req_id;
	hdr.payload_len = (uint32_t)len;
	hdr.status = status;
	return avernella_ipc_send(fd, &hdr, len ? msg : NULL, NULL, 0);
}

static bool valid_service_path(const char *path)
{
	if (!path || path[0] != '/' || strlen(path) > 256)
		return false;
	if (strstr(path, ".."))
		return false;
	for (const char *p = path + 1; *p; p++) {
		char c = *p;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '/' || c == '_' ||
		      c == '-'))
			return false;
	}
	return true;
}

static bool valid_token_bytes(const char *tok)
{
	size_t n;

	if (!tok)
		return false;
	n = strlen(tok);
	if (!n || n >= 16384)
		return false;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)tok[i];

		if (c < 33 || c > 126)
			return false;
	}
	return true;
}

int avernella_vault_serve_once(struct avernella_vault *vault, int client_fd)
{
	struct avernella_ipc_hdr hdr;
	char *buf = NULL;
	J *req = NULL;
	J *out = NULL;
	GError *error = NULL;
	int err;

	int recv_fd = -1;
	size_t nfds = 0;

	if (!vault || client_fd < 0)
		return -EINVAL;

	buf = calloc(1, AVERNELLA_IPC_MAX_PAYLOAD + 1U);
	if (!buf)
		return -ENOMEM;

	err = avernella_ipc_recv(client_fd, &hdr, buf,
				 AVERNELLA_IPC_MAX_PAYLOAD, &recv_fd, 1, &nfds);
	if (err)
		goto out;
	buf[hdr.payload_len] = '\0';

	if (hdr.op == IPC_OP_PING) {
		struct avernella_ipc_hdr pong;

		memset(&pong, 0, sizeof(pong));
		pong.op = IPC_OP_REPLY_OK;
		pong.req_id = hdr.req_id;
		pong.payload_len = 4;
		err = avernella_ipc_send(client_fd, &pong, "pong", NULL, 0);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_GET_PUBLIC_INFO) {
		out = obj();
		json_object_object_add(
			out, "mid",
			json_object_new_string(
				vault->session.mid ? vault->session.mid : ""));
		json_object_object_add(out, "seq",
				       json_object_new_int(vault->session.seq));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_RESERVE_SEQ) {
		gint32 seq;

		if (vault->dirfd >= 0) {
			seq = line_store_sequence(vault->dirfd, &vault->session,
						  &error);
			if (seq <= 0) {
				err = reply_error(
					client_fd, hdr.req_id, -EIO,
					error ? error->message :
						"Sequence reservation failed");
				goto out;
			}
		} else {
			if (vault->session.seq >= INT32_MAX) {
				err = reply_error(client_fd, hdr.req_id,
						  -EOVERFLOW,
						  "Sequence overflow");
				goto out;
			}
			seq = ++vault->session.seq;
			vault->session.seq_reserved = seq;
		}
		out = obj();
		json_object_object_add(out, "seq", json_object_new_int(seq));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_WIPE_SESSION) {
		if (vault->secrets)
			explicit_bzero(vault->secrets, sizeof(*vault->secrets));
		vault->session.access = NULL;
		vault->session.refresh = NULL;
		vault->session.obs_token = NULL;
		vault->session.mid = NULL;
		if (vault->session.keys) {
			json_object_put(vault->session.keys);
			vault->session.keys = NULL;
		}
		if (vault->dirfd >= 0)
			(void)line_store_forget(vault->dirfd, NULL);
		out = obj();
		json_object_object_add(out, "wiped",
				       json_object_new_boolean(TRUE));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	/* Remaining operations carry a JSON object body. */
	req = hdr.payload_len ? json_tokener_parse(buf) : NULL;
	if (!req || !json_object_is_type(req, json_type_object)) {
		err = reply_error(client_fd, hdr.req_id, -EINVAL,
				  "Invalid Vault request payload");
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_SEAL_PAYLOAD) {
		const char *chat = strfield(req, "chat");
		int ctype = json_object_get_int(field(req, "contentType"));
		J *payload = field(req, "payload");
		J *chunks;

		if (!chat || !payload) {
			err = reply_error(client_fd, hdr.req_id, -EINVAL,
					  "Missing chat or payload");
			goto out;
		}
		chunks = line_seal_payload(&vault->session, chat, ctype,
					   payload, &error);
		if (!chunks) {
			err = reply_error(client_fd, hdr.req_id, -EACCES,
					  error ? error->message :
						  "Letter Sealing failed");
			goto out;
		}
		out = obj();
		json_object_object_add(out, "chunks", chunks);
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_OPEN_PAYLOAD) {
		int ctype = json_object_get_int(field(req, "contentType"));
		J *msg = field(req, "message");
		J *plain;

		if (!msg) {
			err = reply_error(client_fd, hdr.req_id, -EINVAL,
					  "Missing message");
			goto out;
		}
		plain = line_open_payload(&vault->session, msg, ctype, &error);
		if (!plain) {
			err = reply_error(client_fd, hdr.req_id, -EACCES,
					  error ? error->message :
						  "Letter Sealing open failed");
			goto out;
		}
		out = obj();
		json_object_object_add(out, "payload", plain);
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_SIGN_REQUEST) {
		const char *path = strfield(req, "path");

		if (!valid_service_path(path)) {
			err = reply_error(client_fd, hdr.req_id, -EINVAL,
					  "Invalid LINE service path");
			goto out;
		}
		if (!vault->session.access) {
			err = reply_error(client_fd, hdr.req_id, -EACCES,
					  "Not signed in");
			goto out;
		}
		out = obj();
		json_object_object_add(
			out, "x-line-access",
			json_object_new_string(vault->session.access));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_ROTATE_TOKEN) {
		const char *next = strfield(req, "access");
		const char *next_refresh = strfield(req, "refresh");
		J *refresh_at = field(req, "refreshAt");

		if (!valid_token_bytes(next)) {
			err = reply_error(client_fd, hdr.req_id, -EINVAL,
					  "Invalid rotated token");
			goto out;
		}
		err = copy_secret_str(vault->secrets->access,
				      sizeof(vault->secrets->access), next);
		if (err) {
			err = reply_error(client_fd, hdr.req_id, err,
					  "Token too large");
			goto out;
		}
		vault->session.access = vault->secrets->access;
		if (next_refresh && *next_refresh) {
			if (!valid_token_bytes(next_refresh)) {
				err = reply_error(client_fd, hdr.req_id,
						  -EINVAL,
						  "Invalid refresh token");
				goto out;
			}
			err = copy_secret_str(vault->secrets->refresh,
					      sizeof(vault->secrets->refresh),
					      next_refresh);
			if (err) {
				err = reply_error(client_fd, hdr.req_id, err,
						  "Refresh token too large");
				goto out;
			}
			vault->session.refresh = vault->secrets->refresh;
		}
		if (refresh_at &&
		    json_object_is_type(refresh_at, json_type_int))
			vault->session.refresh_at =
				json_object_get_int64(refresh_at);
		if (vault->dirfd >= 0)
			(void)line_store_save(vault->dirfd, &vault->session,
					      NULL);
		out = obj();
		json_object_object_add(out, "rotated",
				       json_object_new_boolean(TRUE));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_LOAD_SESSION) {
		const char *mid = strfield(req, "mid");
		const char *access = strfield(req, "access");
		const char *refresh = strfield(req, "refresh");
		J *keys = field(req, "keys");
		gint32 seq = json_object_get_int(field(req, "seq"));

		if (nfds == 1 && recv_fd >= 0) {
			if (vault->dirfd >= 0)
				close(vault->dirfd);
			vault->dirfd = recv_fd;
			recv_fd = -1;
		}
		err = avernella_vault_load_secrets(
			vault, mid, access, refresh,
			keys && json_object_is_type(keys, json_type_object) ?
				keys :
				NULL,
			seq);
		if (err) {
			err = reply_error(client_fd, hdr.req_id, err,
					  "Failed to load session into Vault");
			goto out;
		}
		out = obj();
		json_object_object_add(out, "loaded",
				       json_object_new_boolean(TRUE));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_ECDH) {
		gint32 key_id = json_object_get_int(field(req, "keyId"));
		const char *peer_b64 = strfield(req, "peerPub");
		gsize peer_len = 0;
		guint8 *peer_pub = NULL;
		guint8 shared[32];

		if (peer_b64)
			peer_pub = g_base64_decode(peer_b64, &peer_len);
		if (!peer_pub || peer_len != 32) {
			g_free(peer_pub);
			err = reply_error(client_fd, hdr.req_id, -EINVAL,
					  "Invalid peer public key");
			goto out;
		}
		if (!line_own_ecdh(&vault->session, key_id, peer_pub, shared,
				   &error)) {
			g_free(peer_pub);
			err = reply_error(client_fd, hdr.req_id, -EACCES,
					  error ? error->message :
						  "Vault ECDH failed");
			goto out;
		}
		g_free(peer_pub);
		g_autofree char *shared_b64 =
			g_base64_encode(shared, sizeof(shared));

		explicit_bzero(shared, sizeof(shared));
		out = obj();
		json_object_object_add(out, "shared",
				       json_object_new_string(shared_b64));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	if (hdr.op == IPC_OP_VAULT_OWN_PUBLIC) {
		gint32 key_id = json_object_get_int(field(req, "keyId"));
		guint8 pub[32];

		if (!line_own_public(&vault->session, key_id, pub, &error)) {
			err = reply_error(
				client_fd, hdr.req_id, -EACCES,
				error ? error->message :
					"Vault own public key failed");
			goto out;
		}
		g_autofree char *pub_b64 = g_base64_encode(pub, sizeof(pub));

		out = obj();
		json_object_object_add(out, "pub",
				       json_object_new_string(pub_b64));
		err = reply_json(client_fd, hdr.req_id, out);
		goto out;
	}

	err = reply_error(client_fd, hdr.req_id, -ENOSYS,
			  "Unknown Vault operation");
out:
	if (recv_fd >= 0)
		close(recv_fd);
	if (error)
		g_error_free(error);
	if (out)
		json_object_put(out);
	if (req)
		json_object_put(req);
	if (buf) {
		explicit_bzero(buf, AVERNELLA_IPC_MAX_PAYLOAD);
		free(buf);
	}
	return err;
}

int avernella_vault_run(struct avernella_vault *vault, int client_fd)
{
	for (;;) {
		int rc = avernella_vault_serve_once(vault, client_fd);

		if (rc == -EPIPE)
			return 0;
		if (rc < 0)
			return rc;
	}
}

static J *vault_rpc_fd(int vault_fd, enum avernella_ipc_op op, J *req,
		       int pass_fd, GError **error)
{
	static uint32_t seq_id = 1;
	struct avernella_ipc_hdr tx, rx;
	size_t tx_len = 0;
	const char *tx_str = NULL;
	char *rx_buf = NULL;
	J *reply = NULL;
	int err;

	memset(&tx, 0, sizeof(tx));
	tx.op = (uint16_t)op;
	tx.req_id = seq_id++;
	if (req) {
		tx_str = json_object_to_json_string_length(
			req, JSON_C_TO_STRING_PLAIN, &tx_len);
		if (tx_len > AVERNELLA_IPC_MAX_PAYLOAD) {
			g_set_error_literal(error, LINE_ERROR, 1,
					    "Vault request exceeds IPC limit.");
			return NULL;
		}
		tx.payload_len = (uint32_t)tx_len;
	}

	err = avernella_ipc_send(vault_fd, &tx, tx_len ? tx_str : NULL,
				 pass_fd >= 0 ? &pass_fd : NULL,
				 pass_fd >= 0 ? 1 : 0);
	if (err) {
		g_set_error(error, LINE_ERROR, 1, "Vault IPC send failed: %s",
			    strerror(-err));
		return NULL;
	}

	rx_buf = calloc(1, AVERNELLA_IPC_MAX_PAYLOAD + 1U);
	if (!rx_buf) {
		g_set_error_literal(error, LINE_ERROR, 1, "Out of memory.");
		return NULL;
	}

	err = avernella_ipc_recv(vault_fd, &rx, rx_buf,
				 AVERNELLA_IPC_MAX_PAYLOAD, NULL, 0, NULL);
	if (err) {
		g_set_error(error, LINE_ERROR, 1, "Vault IPC recv failed: %s",
			    strerror(-err));
		goto out;
	}
	rx_buf[rx.payload_len] = '\0';

	if (rx.req_id != tx.req_id) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Vault IPC correlation mismatch.");
		goto out;
	}
	if (rx.op != IPC_OP_REPLY_OK) {
		g_set_error(error, LINE_ERROR, 1, "%s",
			    rx.payload_len ? rx_buf :
					     "Vault rejected request.");
		goto out;
	}
	reply = rx.payload_len ? json_tokener_parse(rx_buf) : obj();
	if (!reply)
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Unreadable Vault reply.");
out:
	explicit_bzero(rx_buf, AVERNELLA_IPC_MAX_PAYLOAD);
	free(rx_buf);
	return reply;
}

static J *vault_rpc(int vault_fd, enum avernella_ipc_op op, J *req,
		    GError **error)
{
	return vault_rpc_fd(vault_fd, op, req, -1, error);
}

int avernella_vault_client_ping(int vault_fd)
{
	struct avernella_ipc_hdr tx, rx;
	char buf[16];
	int err;

	memset(&tx, 0, sizeof(tx));
	tx.op = IPC_OP_PING;
	tx.req_id = 1;
	err = avernella_ipc_send(vault_fd, &tx, NULL, NULL, 0);
	if (err)
		return err;
	err = avernella_ipc_recv(vault_fd, &rx, buf, sizeof(buf), NULL, 0,
				 NULL);
	if (err)
		return err;
	if (rx.op != IPC_OP_REPLY_OK || rx.payload_len != 4 ||
	    memcmp(buf, "pong", 4) != 0)
		return -EPROTO;
	return 0;
}

J *avernella_vault_client_public_info(int vault_fd, GError **error)
{
	return vault_rpc(vault_fd, IPC_OP_VAULT_GET_PUBLIC_INFO, NULL, error);
}

J *avernella_vault_client_seal(int vault_fd, const char *chat, int content_type,
			       J *payload, GError **error)
{
	g_autoptr(J) req = obj();
	g_autoptr(J) rep = NULL;
	J *chunks;

	if (!chat || !payload) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid seal arguments.");
		return NULL;
	}
	json_object_object_add(req, "chat", json_object_new_string(chat));
	json_object_object_add(req, "contentType",
			       json_object_new_int(content_type));
	json_object_object_add(req, "payload", json_object_get(payload));

	rep = vault_rpc(vault_fd, IPC_OP_VAULT_SEAL_PAYLOAD, req, error);
	if (!rep)
		return NULL;
	chunks = field(rep, "chunks");
	if (!chunks || !json_object_is_type(chunks, json_type_array)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Vault returned no sealed chunks.");
		return NULL;
	}
	return json_object_get(chunks);
}

J *avernella_vault_client_open(int vault_fd, J *message, int content_type,
			       GError **error)
{
	g_autoptr(J) req = obj();
	g_autoptr(J) rep = NULL;
	J *plain;

	if (!message) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid open arguments.");
		return NULL;
	}
	json_object_object_add(req, "contentType",
			       json_object_new_int(content_type));
	json_object_object_add(req, "message", json_object_get(message));

	rep = vault_rpc(vault_fd, IPC_OP_VAULT_OPEN_PAYLOAD, req, error);
	if (!rep)
		return NULL;
	plain = field(rep, "payload");
	if (!plain || !json_object_is_type(plain, json_type_object)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Vault returned no decrypted payload.");
		return NULL;
	}
	return json_object_get(plain);
}

gint32 avernella_vault_client_reserve_seq(int vault_fd, GError **error)
{
	g_autoptr(J) rep =
		vault_rpc(vault_fd, IPC_OP_VAULT_RESERVE_SEQ, NULL, error);

	if (!rep)
		return -1;
	return json_object_get_int(field(rep, "seq"));
}

gboolean avernella_vault_client_load_session(int vault_fd, const char *mid,
					     const char *access,
					     const char *refresh, J *keys,
					     gint32 seq, int dirfd,
					     GError **error)
{
	g_autoptr(J) req = obj();

	if (mid)
		json_object_object_add(req, "mid", json_object_new_string(mid));
	if (access)
		json_object_object_add(req, "access",
				       json_object_new_string(access));
	if (refresh)
		json_object_object_add(req, "refresh",
				       json_object_new_string(refresh));
	if (keys)
		json_object_object_add(req, "keys", json_object_get(keys));
	json_object_object_add(req, "seq", json_object_new_int(seq));
	g_autoptr(J) rep = vault_rpc_fd(vault_fd, IPC_OP_VAULT_LOAD_SESSION,
					req, dirfd, error);
	return rep != NULL;
}

gboolean avernella_vault_client_ecdh(int vault_fd, gint32 key_id,
				     const guint8 peer_pub[32],
				     guint8 shared_out[32], GError **error)
{
	char *peer_b64 = g_base64_encode(peer_pub, 32);
	J *req = obj();
	J *rep = NULL;
	const char *shared_b64;
	gsize len = 0;
	guint8 *raw = NULL;
	gboolean ok = FALSE;

	json_object_object_add(req, "keyId", json_object_new_int(key_id));
	json_object_object_add(req, "peerPub",
			       json_object_new_string(peer_b64));
	g_free(peer_b64);
	rep = vault_rpc(vault_fd, IPC_OP_VAULT_ECDH, req, error);
	json_object_put(req);
	if (!rep)
		return FALSE;
	shared_b64 = strfield(rep, "shared");
	if (shared_b64)
		raw = g_base64_decode(shared_b64, &len);
	json_object_put(rep);
	if (!raw || len != 32) {
		g_free(raw);
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Vault returned invalid ECDH secret.");
		return FALSE;
	}
	memcpy(shared_out, raw, 32);
	explicit_bzero(raw, 32);
	g_free(raw);
	ok = TRUE;
	return ok;
}

gboolean avernella_vault_client_own_public(int vault_fd, gint32 key_id,
					   guint8 pub_out[32], GError **error)
{
	g_autoptr(J) req = obj();

	json_object_object_add(req, "keyId", json_object_new_int(key_id));
	g_autoptr(J) rep =
		vault_rpc(vault_fd, IPC_OP_VAULT_OWN_PUBLIC, req, error);
	if (!rep)
		return FALSE;
	const char *pub_b64 = strfield(rep, "pub");
	gsize len = 0;
	g_autofree guint8 *raw = pub_b64 ? g_base64_decode(pub_b64, &len) :
					   NULL;

	if (!raw || len != 32) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Vault returned invalid public key.");
		return FALSE;
	}
	memcpy(pub_out, raw, 32);
	return TRUE;
}

gboolean avernella_vault_client_rotate_token(int vault_fd, const char *access,
					     const char *refresh,
					     gint64 refresh_at, GError **error)
{
	g_autoptr(J) req = obj();

	if (access)
		json_object_object_add(req, "access",
				       json_object_new_string(access));
	if (refresh && *refresh)
		json_object_object_add(req, "refresh",
				       json_object_new_string(refresh));
	if (refresh_at)
		json_object_object_add(req, "refreshAt",
				       json_object_new_int64(refresh_at));
	g_autoptr(J) rep =
		vault_rpc(vault_fd, IPC_OP_VAULT_ROTATE_TOKEN, req, error);
	return rep != NULL;
}

gboolean avernella_vault_client_wipe(int vault_fd, GError **error)
{
	g_autoptr(J) rep =
		vault_rpc(vault_fd, IPC_OP_VAULT_WIPE_SESSION, NULL, error);
	return rep != NULL;
}

int avernella_supervisor_start(struct avernella_supervised_worker *slot,
			       const struct avernella_sandbox_opts *opts,
			       avernella_worker_fn entry, void *userdata,
			       uint32_t max_restarts)
{
	if (!slot || !opts || !entry)
		return -EINVAL;
	memset(slot, 0, sizeof(*slot));
	slot->opts = *opts;
	slot->entry = entry;
	slot->userdata = userdata;
	slot->max_restarts = max_restarts;
	slot->proc.pid = -1;
	slot->proc.pidfd = -1;
	slot->proc.ctrl_fd = -1;
	return avernella_spawn_worker(&slot->opts, slot->entry, slot->userdata,
				      &slot->proc);
}

int avernella_supervisor_poll_respawn(struct avernella_supervised_worker *slot)
{
	int status = 0;
	pid_t done;
	int err;

	if (!slot || slot->proc.pid <= 0)
		return -EINVAL;

	done = waitpid(slot->proc.pid, &status, WNOHANG);
	if (done == 0)
		return 0; /* Still running cleanly. */
	if (done < 0)
		return -errno;

	slot->last_status = status;
	slot->proc.pid = -1;
	avernella_worker_close(&slot->proc);

	if (slot->restarts >= slot->max_restarts)
		return -EAGAIN;

	slot->restarts++;
	err = avernella_spawn_worker(&slot->opts, slot->entry, slot->userdata,
				     &slot->proc);
	if (err)
		return err;
	return 1;
}

void avernella_supervisor_stop(struct avernella_supervised_worker *slot)
{
	if (!slot)
		return;
	avernella_worker_close(&slot->proc);
}
