// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_crypto.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY_CTX, EVP_PKEY_CTX_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_CIPHER_CTX, EVP_CIPHER_CTX_free)

enum {
	HASH_SIZE = 32,
	CHUNK = 128 * 1024
};

struct media_keys {
	guint8 aes[32], hmac[32], counter[16];
};

static gboolean derive(const guint8 material[32], struct media_keys *keys)
{
	g_autoptr(EVP_PKEY_CTX) ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	const guint8 info[] = "FileEncryption";
	guint8 raw[76];
	size_t size = sizeof(raw);
	gboolean ok =
		ctx && EVP_PKEY_derive_init(ctx) == 1 &&
		EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
		EVP_PKEY_CTX_set1_hkdf_key(ctx, material, 32) == 1 &&
		EVP_PKEY_CTX_add1_hkdf_info(ctx, info, sizeof(info) - 1) == 1 &&
		EVP_PKEY_derive(ctx, raw, &size) == 1 && size == sizeof(raw);
	if (ok) {
		memcpy(keys->aes, raw, 32);
		memcpy(keys->hmac, raw + 32, 32);
		memcpy(keys->counter, raw + 64, 12);
		memset(keys->counter + 12, 0, 4);
	}
	OPENSSL_cleanse(raw, sizeof(raw));
	return ok;
}

static gboolean aes_ctr(guint8 *buf, gsize n, const struct media_keys *keys)
{
	g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();
	guint8 tail[EVP_MAX_BLOCK_LENGTH];
	int written = 0, rest = 0;

	return ctx &&
	       EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, keys->aes,
				  keys->counter) == 1 &&
	       EVP_EncryptUpdate(ctx, buf, &written, buf, (int)n) == 1 &&
	       EVP_EncryptFinal_ex(ctx, tail, &rest) == 1 &&
	       written == (int)n && rest == 0;
}

static void hmac_sha256(const guint8 key[32], const guint8 *data, gsize n,
			guint8 out[32])
{
	g_autoptr(GHmac) mac = g_hmac_new(G_CHECKSUM_SHA256, key, 32);
	g_hmac_update(mac, data, n);
	gsize len = HASH_SIZE;

	g_hmac_get_digest(mac, out, &len);
}

static GBytes *hash_chunks(const guint8 *cipher, gsize len)
{
	g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
	guint8 *digests = g_malloc(((len + CHUNK - 1) / CHUNK) * HASH_SIZE);
	gsize used = 0;

	for (gsize i = 0; i < len; i += CHUNK) {
		gsize n = HASH_SIZE;

		g_checksum_reset(hash);
		g_checksum_update(hash, cipher + i, MIN(len - i, CHUNK));
		g_checksum_get_digest(hash, digests + used, &n);
		used += n;
	}
	return g_bytes_new_take(digests, used);
}

static gboolean authenticated(const guint8 *body, gsize len,
			      const guint8 key[32])
{
	guint8 tag[HASH_SIZE];

	hmac_sha256(key, body, len, tag);
	if (CRYPTO_memcmp(tag, body + len, HASH_SIZE) == 0)
		return TRUE;
	g_autoptr(GBytes) hashes = hash_chunks(body, len);
	gsize n;
	const guint8 *digest = g_bytes_get_data(hashes, &n);

	hmac_sha256(key, digest, n, tag);
	return CRYPTO_memcmp(tag, body + len, HASH_SIZE) == 0;
}

GBytes *line_media_encrypt(GBytes *plain, const guint8 material[32],
			   GBytes **hashes, GError **error)
{
	gsize n;

	g_autofree guint8 *data = g_bytes_unref_to_data(plain, &n);

	if (n == 0 || n > LINE_MEDIA_LIMIT) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Choose a nonempty attachment of up to 64 MiB.");
		return NULL;
	}
	struct media_keys keys;
	gboolean ok = derive(material, &keys) && aes_ctr(data, n, &keys);

	if (ok) {
		data = g_realloc(data, n + HASH_SIZE);
		if (hashes) {
			*hashes = hash_chunks(data, n);
			gsize len;
			const guint8 *digest = g_bytes_get_data(*hashes, &len);

			hmac_sha256(keys.hmac, digest, len, data + n);
		} else {
			hmac_sha256(keys.hmac, data, n, data + n);
		}
	}
	OPENSSL_cleanse(&keys, sizeof(keys));
	if (ok)
		return g_bytes_new_take(g_steal_pointer(&data), n + HASH_SIZE);
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Could not encrypt the attachment.");
	return NULL;
}

GBytes *line_media_decrypt(GBytes *blob, const guint8 material[32],
			   GError **error)
{
	gsize len;
	g_autofree guint8 *data = g_bytes_unref_to_data(blob, &len);

	if (len <= HASH_SIZE || len > LINE_MEDIA_LIMIT + HASH_SIZE) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid encrypted attachment size.");
		return NULL;
	}
	gsize n = len - HASH_SIZE;
	struct media_keys keys;
	gboolean ok = derive(material, &keys) &&
		      authenticated(data, n, keys.hmac) &&
		      aes_ctr(data, n, &keys);
	OPENSSL_cleanse(&keys, sizeof(keys));
	if (ok)
		return g_bytes_new_take(g_steal_pointer(&data), n);
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Could not authenticate the encrypted attachment.");
	return NULL;
}
