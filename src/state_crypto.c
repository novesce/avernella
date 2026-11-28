// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_crypto.h"
#include "line.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_CIPHER_CTX, EVP_CIPHER_CTX_free)

/* File layout: magic, version, nonce, ciphertext, authentication tag. */
static const guint8 state_magic[] = { 'A', 'V', 'S', 'E', 'S', 'S', 0 };
enum {
	STATE_VERSION = 1,
	STATE_NONCE_SIZE = 12,
	STATE_TAG_SIZE = 16,
	STATE_NONCE_OFFSET = sizeof(state_magic) + 1,
	STATE_HEADER_SIZE = STATE_NONCE_OFFSET + STATE_NONCE_SIZE,
};

G_STATIC_ASSERT(STATE_HEADER_SIZE + STATE_TAG_SIZE ==
		LINE_STATE_CRYPTO_OVERHEAD);
G_STATIC_ASSERT(LINE_STATE_MAX_BYTES <= G_MAXINT);

gboolean line_state_is_encrypted(const void *file, gsize size)
{
	return file && size >= sizeof(state_magic) &&
	       !memcmp(file, state_magic, sizeof(state_magic));
}

guint8 *line_state_encrypt(const guint8 key[LINE_STATE_KEY_SIZE],
			   const void *plain, gsize size, gsize *written,
			   GError **error)
{
	g_autoptr(EVP_CIPHER_CTX) ctx = NULL;
	g_autofree guint8 *file = NULL;
	int used = 0, tail = 0;

	if (written)
		*written = 0;
	if (!key || !plain || !size || size > LINE_STATE_MAX_BYTES)
		goto fail;

	ctx = EVP_CIPHER_CTX_new();
	file = g_malloc(size + LINE_STATE_CRYPTO_OVERHEAD);
	memcpy(file, state_magic, sizeof(state_magic));
	file[sizeof(state_magic)] = STATE_VERSION;
	guint8 *nonce = file + STATE_NONCE_OFFSET;
	guint8 *cipher = file + STATE_HEADER_SIZE;

	if (!ctx || RAND_bytes(nonce, STATE_NONCE_SIZE) != 1 ||
	    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, nonce) != 1 ||
	    EVP_EncryptUpdate(ctx, NULL, &used, file, STATE_HEADER_SIZE) != 1 ||
	    EVP_EncryptUpdate(ctx, cipher, &used, plain, size) != 1 ||
	    used != (int)size ||
	    EVP_EncryptFinal_ex(ctx, cipher + used, &tail) != 1 || tail ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, STATE_TAG_SIZE,
				cipher + size) != 1)
		goto fail;
	if (written)
		*written = size + LINE_STATE_CRYPTO_OVERHEAD;
	return g_steal_pointer(&file);

fail:
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Could not encrypt the local LINE session.");
	return NULL;
}

char *line_state_decrypt(const guint8 key[LINE_STATE_KEY_SIZE],
			 const void *data, gsize size, gsize *written,
			 GError **error)
{
	g_autoptr(EVP_CIPHER_CTX) ctx = NULL;
	g_autofree char *plain = NULL;
	const guint8 *file = data;
	guint8 final[EVP_MAX_BLOCK_LENGTH];
	int used = 0, tail = 0;

	if (written)
		*written = 0;
	if (!key || size <= LINE_STATE_CRYPTO_OVERHEAD ||
	    size > LINE_STATE_MAX_BYTES + LINE_STATE_CRYPTO_OVERHEAD ||
	    !line_state_is_encrypted(file, size) ||
	    file[sizeof(state_magic)] != STATE_VERSION)
		goto fail;

	gsize length = size - LINE_STATE_CRYPTO_OVERHEAD;
	const guint8 *nonce = file + STATE_NONCE_OFFSET;
	const guint8 *cipher = file + STATE_HEADER_SIZE;

	ctx = EVP_CIPHER_CTX_new();
	plain = g_malloc(length + 1);
	gboolean valid =
		ctx &&
		EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, nonce) ==
			1 &&
		EVP_DecryptUpdate(ctx, NULL, &used, file, STATE_HEADER_SIZE) ==
			1 &&
		EVP_DecryptUpdate(ctx, (guint8 *)plain, &used, cipher,
				  length) == 1 &&
		used == (int)length &&
		EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, STATE_TAG_SIZE,
				    (void *)(cipher + length)) == 1 &&
		EVP_DecryptFinal_ex(ctx, final, &tail) == 1 && !tail;

	explicit_bzero(final, sizeof(final));
	if (!valid) {
		explicit_bzero(plain, length + 1);
		goto fail;
	}
	plain[length] = 0;
	if (written)
		*written = length;
	return g_steal_pointer(&plain);

fail:
	g_set_error_literal(
		error, LINE_ERROR, 1,
		"Could not authenticate the encrypted LINE session.");
	return NULL;
}
