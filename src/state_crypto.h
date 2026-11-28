/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

#define LINE_STATE_KEY_SIZE 32
#define LINE_STATE_CRYPTO_OVERHEAD 36
#define LINE_STATE_MAX_BYTES (8U * 1024U * 1024U)

/* Identifies the encrypted file family independently of its version. */
gboolean line_state_is_encrypted(const void *file, gsize size);

/*
 * Return owned buffers. Decrypted output is authenticated, NUL-terminated
 * and must be wiped before freeing. The reported lengths exclude the NUL.
 */
guint8 *line_state_encrypt(const guint8 key[LINE_STATE_KEY_SIZE],
			   const void *plain, gsize size, gsize *written,
			   GError **error);
char *line_state_decrypt(const guint8 key[LINE_STATE_KEY_SIZE],
			 const void *file, gsize size, gsize *written,
			 GError **error);
