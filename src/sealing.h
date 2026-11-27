/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * Imports the key generations approved by the phone. Replaces keys only after
 * every entry, the requested generation and any supplied checksum validate.
 */
gboolean line_import_keys(Line *s, J *meta, const guint8 secret[32],
			  GError **error);
gboolean line_import_login_keys(Line *s, J *meta, const guint8 secret[32],
				guint8 proof[16], GError **error);

/* Letter Sealing v2 text in direct and already-keyed group chats. Caller owns the five chunks or UTF-8 text. */
J *line_seal_text(Line *s, const char *chat, const char *text, GError **error);
char *line_open_text(Line *s, J *message, GError **error);

/*
 * content_type participates in the authentication tag. The payload is borrowed
 * on seal and returned as an owned UTF-8 JSON object on open.
 */
J *line_seal_payload(Line *s, const char *chat, int content_type, J *payload,
		     GError **error);
J *line_open_payload(Line *s, J *message, int content_type, GError **error);

/*
 * Low-level local identity operations used by the isolated memfd_secret()
 * Key Vault process so X25519 private keys never reside in the worker.
 */
gboolean line_own_ecdh(Line *s, gint32 key_id, const guint8 peer_pub[32],
		       guint8 shared_out[32], GError **error);
gboolean line_own_public(Line *s, gint32 key_id, guint8 pub_out[32],
			 GError **error);

struct line_sealing_vault_ops {
	gboolean (*ecdh)(void *ctx, gint32 key_id, const guint8 peer_pub[32],
			 guint8 shared_out[32], GError **error);
	gboolean (*pub)(void *ctx, gint32 key_id, guint8 pub_out[32],
			GError **error);
};

/* Installs an optional out-of-process Key Vault for own X25519 private keys. */
void line_sealing_set_vault(const struct line_sealing_vault_ops *ops,
			    void *ctx);
