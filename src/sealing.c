// SPDX-License-Identifier: GPL-3.0-or-later
#include "sealing.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY, EVP_PKEY_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY_CTX, EVP_PKEY_CTX_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_CIPHER_CTX, EVP_CIPHER_CTX_free)

static gboolean fail(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return FALSE;
}

static const guint8 *binary(J *v, gsize *length)
{
	if (!json_object_is_type(v, json_type_string))
		return NULL;
	*length = (gsize)json_object_get_string_len(v);
	return (const guint8 *)json_object_get_string(v);
}

static gboolean read_id(J *v, gint32 *id)
{
	gint64 n;

	if (json_object_is_type(v, json_type_int)) {
		n = json_object_get_int64(v);
	} else if (json_object_is_type(v, json_type_string)) {
		const char *p = json_object_get_string(v);

		if (strlen(p) != (size_t)json_object_get_string_len(v) ||
		    !g_ascii_string_to_signed(p, 10, 0, G_MAXINT32, &n, NULL))
			return FALSE;
	} else {
		return FALSE;
	}
	if (n < 0 || n > G_MAXINT32)
		return FALSE;
	*id = (gint32)n;
	return TRUE;
}

static gboolean public_from_private(const guint8 priv[32], guint8 pub[32])
{
	g_autoptr(EVP_PKEY) key =
		EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
	size_t len = 32;

	return key && EVP_PKEY_get_raw_public_key(key, pub, &len) == 1 &&
	       len == 32;
}

static gboolean shared_secret(const guint8 own[32], const guint8 peer[32],
			      guint8 out[32])
{
	g_autoptr(EVP_PKEY) priv =
		EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, own, 32);
	g_autoptr(EVP_PKEY) pub =
		EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
	g_autoptr(EVP_PKEY_CTX) ctx = priv ? EVP_PKEY_CTX_new(priv, NULL) :
					     NULL;
	size_t n = 32;

	return ctx && pub && EVP_PKEY_derive_init(ctx) == 1 &&
	       EVP_PKEY_derive_set_peer(ctx, pub) == 1 &&
	       EVP_PKEY_derive(ctx, out, &n) == 1 && n == 32;
}

static void digest(const guint8 *data, gsize n, const guint8 *salt,
		   gsize salt_len, const char *label, guint8 out[32])
{
	g_autoptr(GChecksum) h = g_checksum_new(G_CHECKSUM_SHA256);
	g_checksum_update(h, data, n);
	if (salt)
		g_checksum_update(h, salt, salt_len);
	if (label)
		g_checksum_update(h, (const guint8 *)label, strlen(label));
	gsize len = 32;

	g_checksum_get_digest(h, out, &len);
}

static void fold_digest(guint8 hash[32])
{
	for (size_t i = 0; i < 16; i++)
		hash[i] ^= hash[i + 16];
}

static guint8 *decode64(J *v, gsize *size)
{
	gsize length = 0;
	const char *s = (const char *)binary(v, &length);

	if (!s || !length || length > 2 * 1024 * 1024 || strlen(s) != length)
		return NULL;
	guint8 *out = g_base64_decode(s, size);

	g_autofree char *encoded = g_base64_encode(out, *size);

	if (strcmp(s, encoded) == 0)
		return out;
	OPENSSL_cleanse(out, *size);
	g_free(out);
	return NULL;
}

static gboolean decode32(J *v, guint8 out[32])
{
	gsize n = 0;
	g_autofree guint8 *buffer = decode64(v, &n);

	if (!buffer)
		return FALSE;
	if (n == 32)
		memcpy(out, buffer, 32);
	OPENSSL_cleanse(buffer, n);
	return n == 32;
}

static J *from64(const guint8 *data, gsize len)
{
	g_autofree char *s = g_base64_encode(data, len);
	J *v = json_object_new_string(s);

	OPENSSL_cleanse(s, strlen(s));
	return v;
}

static gboolean chain_proof(const guint8 *ciphertext, gsize n,
			    const guint8 key[32], guint8 proof[16])
{
	guint8 hash[32], encrypted[32];

	digest(ciphertext, n, NULL, 0, NULL, hash);
	fold_digest(hash);
	g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();
	int first = 0, rest = 0;
	gboolean ok =
		ctx &&
		EVP_EncryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key, NULL) ==
			1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, encrypted, &first, hash, 16) == 1 &&
		EVP_EncryptFinal_ex(ctx, encrypted + first, &rest) == 1 &&
		first + rest == 16;
	if (ok)
		memcpy(proof, encrypted, 16);
	OPENSSL_cleanse(hash, sizeof(hash));
	return ok;
}

static gboolean check_mac(const guint8 *given, gsize mac_len,
			  const guint8 *ciphertext, gsize n,
			  const guint8 key[32])
{
	guint8 expected[16];

	return given && mac_len == sizeof(expected) &&
	       chain_proof(ciphertext, n, key, expected) &&
	       CRYPTO_memcmp(expected, given, sizeof(expected)) == 0;
}

static gboolean unwrap(const guint8 key[32], const guint8 iv[16],
		       const guint8 *input, int input_len, guint8 *plain,
		       gsize *plain_len)
{
	g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();
	int first = 0, last = 0;

	if (!ctx ||
	    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1 ||
	    EVP_DecryptUpdate(ctx, plain, &first, input, input_len) != 1 ||
	    EVP_DecryptFinal_ex(ctx, plain + first, &last) != 1)
		return FALSE;
	*plain_len = (gsize)(first + last);
	return TRUE;
}

static void wrapping_key(const guint8 shared[32], guint8 key[32], guint8 iv[32])
{
	digest(shared, 32, NULL, 0, "Key", key);
	digest(shared, 32, NULL, 0, "IV", iv);
	fold_digest(iv);
}

static J *key_generations(J *decoded, gint32 active, GError **error)
{
	J *entries = field(decoded, "1");

	g_autoptr(J) gens = obj();
	gboolean found = FALSE;

	if (!json_object_is_type(entries, json_type_array))
		goto invalid;
	for (size_t i = 0; i < json_object_array_length(entries); i++) {
		J *entry = json_object_array_get_idx(entries, i);
		gsize pub_len = 0, priv_len = 0;
		const guint8 *pub = binary(field(entry, "4"), &pub_len);
		const guint8 *priv = binary(field(entry, "5"), &priv_len);
		gint32 id;

		if (pub_len != 32 || priv_len != 32 ||
		    !read_id(field(entry, "2"), &id))
			goto invalid;
		guint8 derived[32];

		if (!public_from_private(priv, derived) ||
		    CRYPTO_memcmp(derived, pub, 32) != 0)
			goto invalid;

		char name[16];

		g_snprintf(name, sizeof(name), "%d", id);
		if (field(gens, name))
			goto invalid;
		json_object_object_add(gens, name, from64(priv, priv_len));
		found |= id == active;
	}
	if (!found)
		goto invalid;
	J *keys = obj();

	json_object_object_add(keys, "active", json_object_new_int(active));
	json_object_object_add(keys, "own", g_steal_pointer(&gens));
	json_object_object_add(keys, "peers", obj());
	return keys;
invalid:
	fail(error,
	     "The phone supplied an invalid or incomplete Letter Sealing identity.");
	return NULL;
}

gboolean line_import_login_keys(Line *s, J *meta, const guint8 secret[32],
				guint8 proof[16], GError **error)
{
	g_autoptr(J) parsed = NULL;
	if (json_object_is_type(meta, json_type_string)) {
		parsed = json_tokener_parse(json_object_get_string(meta));
		meta = parsed;
	}
	gint32 active;
	guint8 peer[32], shared[32], key[32], iv_hash[32];
	gsize length = 0;
	g_autofree guint8 *chain =
		decode64(field(meta, "encryptedKeyChain"), &length);
	if (!read_id(field(meta, "keyId"), &active) ||
	    !decode32(field(meta, "publicKey"), peer) || !chain || !length ||
	    length % 16 || !shared_secret(secret, peer, shared))
		return fail(
			error,
			"The phone did not supply a usable Letter Sealing keychain.");
	wrapping_key(shared, key, iv_hash);
	OPENSSL_cleanse(shared, sizeof(shared));
	g_autofree guint8 *plain = g_malloc(length + EVP_MAX_BLOCK_LENGTH);
	J *mac = field(meta, "hashKeyChain");
	gsize mac_len = 0;
	g_autofree guint8 *given = mac ? decode64(mac, &mac_len) : NULL;
	gsize plain_len = 0;
	guint8 expected[16];
	gboolean ok =
		(!(mac || proof) ||
		 chain_proof(chain, length, key, expected)) &&
		(!mac ||
		 (given && mac_len == sizeof(expected) &&
		  CRYPTO_memcmp(expected, given, sizeof(expected)) == 0)) &&
		unwrap(key, iv_hash, chain, (int)length, plain, &plain_len);
	OPENSSL_cleanse(key, sizeof(key));
	OPENSSL_cleanse(iv_hash, sizeof(iv_hash));
	g_autoptr(J) chain_struct = NULL;
	if (ok)
		chain_struct = line_decode_struct(plain, plain_len, error);
	OPENSSL_cleanse(plain, length);
	if (!ok)
		return fail(
			error,
			"The phone's Letter Sealing keychain failed verification.");
	if (!chain_struct)
		return FALSE;
	J *keys = key_generations(chain_struct, active, error);

	if (!keys)
		return FALSE;
	json_object_put(s->keys);
	g_clear_pointer(&s->history_secrets, g_array_unref);
	s->next_secret = 0;
	s->keys = keys;
	if (proof)
		memcpy(proof, expected, sizeof(expected));
	return TRUE;
}

gboolean line_import_keys(Line *s, J *meta, const guint8 secret[32],
			  GError **error)
{
	return line_import_login_keys(s, meta, secret, NULL, error);
}

static gboolean own_key(Line *s, gint32 id, guint8 out[32], GError **error)
{
	char name[16];

	g_snprintf(name, sizeof(name), "%d", id);
	if (decode32(field(field(s->keys, "own"), name), out))
		return TRUE;
	g_set_error_literal(
		error, LINE_ERROR, LINE_OWN_KEY_MISSING,
		"This device does not have the required generation of your Letter Sealing key.");
	return FALSE;
}

static gboolean type_allowed(J *types, int content_type)
{
	/*
	 * Encrypted media uses the separate OBS media flow. The allowedTypes
	 * list describes inline content such as text and location.
	 */
	if (!types || (content_type != 0 && content_type != 15))
		return TRUE;
	if (!json_object_is_type(types, json_type_array))
		return FALSE;
	for (size_t i = 0; i < json_object_array_length(types); i++) {
		J *type = json_object_array_get_idx(types, i);

		if (json_object_is_type(type, json_type_int) &&
		    json_object_get_int64(type) == content_type)
			return TRUE;
	}
	return FALSE;
}

/*
 * Sending negotiates the recipient's current key. Reading looks up the exact
 * generation carried in the message, even when newer keys are available.
 */
static gboolean peer_key(Line *s, const char *peer, gboolean negotiate,
			 int type, gint32 *id, guint8 out[32], GError **error)
{
	J *peers = field(s->keys, "peers");
	g_autofree char *cache_key = NULL;

	if (!negotiate) {
		cache_key = g_strdup_printf("%s/%d", peer, *id);
		if (decode32(field(peers, cache_key), out))
			return TRUE;
	}
	g_autoptr(J) args = obj();
	json_object_object_add(args, "2:str", json_object_new_string(peer));
	if (!negotiate) {
		json_object_object_add(args, "3:i32", json_object_new_int(1));
		json_object_object_add(args, "4:i32", json_object_new_int(*id));
	}
	const char *method = negotiate ? "negotiateE2EEPublicKey" :
					 "getE2EEPublicKey";
	g_autoptr(GError) lookup_error = NULL;
	g_autoptr(J) response = line_rpc(s, "/S4", method, args, &lookup_error);
	gboolean current_response = negotiate;

	/*
	 * Some companion routes only expose key negotiation. The negotiated
	 * key is usable for history only if it is the requested generation.
	 */
	if (!response && !negotiate &&
	    g_error_matches(lookup_error, LINE_ERROR,
			    LINE_METHOD_UNAVAILABLE)) {
		g_clear_error(&lookup_error);
		json_object_object_del(args, "3:i32");
		json_object_object_del(args, "4:i32");
		response = line_rpc(s, "/S4", "negotiateE2EEPublicKey", args,
				    &lookup_error);
		current_response = TRUE;
	}
	if (!response) {
		if (lookup_error)
			g_propagate_error(error,
					  g_steal_pointer(&lookup_error));
		return FALSE;
	}
	if (negotiate && (json_object_get_int(field(response, "3")) != 2 ||
			  !type_allowed(field(response, "1"), type)))
		return fail(
			error,
			"This contact does not accept Letter Sealing v2 for this message type.");
	J *key = current_response ? field(response, "2") : response;
	J *returned_id = field(key, "2");
	gint32 returned = *id;
	gsize n = 0;
	const guint8 *bytes = binary(field(key, "4"), &n);

	if (n != 32 ||
	    ((current_response || returned_id) &&
	     !read_id(returned_id, &returned)) ||
	    (!negotiate && *id != returned))
		return fail(error,
			    "LINE did not return the requested contact key.");
	*id = returned;
	memcpy(out, bytes, 32);
	if (!cache_key)
		cache_key = g_strdup_printf("%s/%d", peer, *id);
	if (peers)
		json_object_object_add(peers, cache_key, from64(bytes, n));
	return TRUE;
}

static gboolean valid_mid(const char *mid, const char *types)
{
	if (!mid || strlen(mid) != 33 || !strchr(types, mid[0]))
		return FALSE;
	for (size_t i = 1; i < 33; i++)
		if (!g_ascii_isxdigit(mid[i]))
			return FALSE;
	return TRUE;
}

static void make_aad(guint8 out[82], const char *to, const char *from,
		     guint32 sender, guint32 receiver, guint32 type)
{
	memcpy(out, to, 33);
	memcpy(out + 33, from, 33);
	guint32 numbers[4] = { GUINT32_TO_BE(sender), GUINT32_TO_BE(receiver),
			       GUINT32_TO_BE(2), GUINT32_TO_BE(type) };
	memcpy(out + 66, numbers, sizeof(numbers));
}

/*
 * Keep shared secrets in RAM for this approved keychain. Replacing the
 * keychain or signing out releases and wipes the complete bounded array.
 * Read-side entries are promoted only after successful GCM authentication.
 */
enum {
	SHARED_SLOTS = 32
};

struct history_secret {
	char to[34], from[34];
	gint32 sender, receiver;
	guint8 secret[32];
};

static void wipe_secret(gpointer entry)
{
	OPENSSL_cleanse(entry, sizeof(struct history_secret));
}

static struct history_secret *history_secret(Line *s, const char *to,
					     const char *from, gint32 sender,
					     gint32 receiver)
{
	GArray *cache = s->history_secrets;

	if (!cache)
		return NULL;
	for (guint i = 0; i < cache->len; i++) {
		struct history_secret *entry =
			&g_array_index(cache, struct history_secret, i);
		if (entry->sender == sender && entry->receiver == receiver &&
		    !memcmp(entry->to, to, 33) &&
		    !memcmp(entry->from, from, 33))
			return entry;
	}
	return NULL;
}

static void remember_secret(Line *s, const char *to, const char *from,
			    gint32 sender, gint32 receiver,
			    const guint8 secret[32])
{
	if (!s->history_secrets) {
		s->history_secrets = g_array_sized_new(
			FALSE, TRUE, sizeof(struct history_secret),
			SHARED_SLOTS);
		g_array_set_clear_func(s->history_secrets, wipe_secret);
	}
	GArray *cache = s->history_secrets;

	if (cache->len < SHARED_SLOTS)
		g_array_set_size(cache, cache->len + 1);
	struct history_secret *entry =
		&g_array_index(cache, struct history_secret, s->next_secret);
	memcpy(entry->to, to, 34);
	memcpy(entry->from, from, 34);
	entry->sender = sender;
	entry->receiver = receiver;
	memcpy(entry->secret, secret, sizeof(entry->secret));
	s->next_secret = (s->next_secret + 1) % SHARED_SLOTS;
}

static struct line_sealing_vault_ops sealing_vault_ops;
static void *sealing_vault_ctx;

void line_sealing_set_vault(const struct line_sealing_vault_ops *ops, void *ctx)
{
	if (ops)
		sealing_vault_ops = *ops;
	else
		memset(&sealing_vault_ops, 0, sizeof(sealing_vault_ops));
	sealing_vault_ctx = ops ? ctx : NULL;
}

gboolean line_own_ecdh(Line *s, gint32 key_id, const guint8 peer_pub[32],
		       guint8 shared_out[32], GError **error)
{
	guint8 priv[32];

	if (!own_key(s, key_id, priv, error))
		return FALSE;
	gboolean ok = shared_secret(priv, peer_pub, shared_out);

	OPENSSL_cleanse(priv, sizeof(priv));
	if (!ok)
		return fail(error, "Invalid Letter Sealing public key.");
	return TRUE;
}

gboolean line_own_public(Line *s, gint32 key_id, guint8 pub_out[32],
			 GError **error)
{
	guint8 priv[32];

	if (!own_key(s, key_id, priv, error))
		return FALSE;
	gboolean ok = public_from_private(priv, pub_out);

	OPENSSL_cleanse(priv, sizeof(priv));
	if (!ok)
		return fail(error, "Invalid local Letter Sealing identity.");
	return TRUE;
}

static gboolean direct_shared_secret(Line *s, gint32 own_id, const char *peer,
				     gint32 *peer_id, gboolean negotiate,
				     int type, guint8 shared[32],
				     GError **error)
{
	guint8 pub[32];

	if (!peer_key(s, peer, negotiate, type, peer_id, pub, error))
		return FALSE;
	if (!field(s->keys, "own") && sealing_vault_ops.ecdh)
		return sealing_vault_ops.ecdh(sealing_vault_ctx, own_id, pub,
					      shared, error);
	return line_own_ecdh(s, own_id, pub, shared, error);
}

static gboolean public_by_id(Line *s, const char *user, gint32 id,
			     guint8 out[32], GError **error)
{
	if (strcmp(user, s->mid))
		return peer_key(s, user, FALSE, 0, &id, out, error);
	if (!field(s->keys, "own") && sealing_vault_ops.pub)
		return sealing_vault_ops.pub(sealing_vault_ctx, id, out, error);
	return line_own_public(s, id, out, error);
}

static gboolean group_private_key(Line *s, const char *chat, gboolean latest,
				  int type, gint32 *id, guint8 group[32],
				  GError **error)
{
	J *groups = field(s->keys, "groups");
	g_autofree char *slot = NULL;

	if (!latest) {
		slot = g_strdup_printf("%s/%d", chat, *id);
		if (decode32(field(groups, slot), group))
			return TRUE;
	}
	g_autoptr(J) args = obj();
	json_object_object_add(args, "2:i32", json_object_new_int(1));
	json_object_object_add(args, "3:str", json_object_new_string(chat));
	if (!latest)
		json_object_object_add(args, "4:i32", json_object_new_int(*id));
	g_autoptr(J) result = line_rpc(s, "/S4",
				       latest ? "getLastE2EEGroupSharedKey" :
						"getE2EEGroupSharedKey",
				       args, error);
	if (!result)
		return FALSE;
	const char *creator = strfield(result, "3"),
		   *receiver = strfield(result, "5");
	gint32 returned, creator_id, receiver_id;
	gsize len = 0;
	const guint8 *wrapped = binary(field(result, "7"), &len);

	if (!valid_mid(creator, "u") ||
	    (receiver && strcmp(receiver, s->mid)) ||
	    !read_id(field(result, "2"), &returned) ||
	    (!latest && returned != *id) ||
	    !read_id(field(result, "4"), &creator_id) ||
	    !read_id(field(result, "6"), &receiver_id) || len != 48)
		return fail(
			error,
			"LINE did not supply an existing group key for this device.");
	if (latest && ((field(result, "9") &&
			json_object_get_int(field(result, "9")) != 2) ||
		       !type_allowed(field(result, "8"), type)))
		return fail(
			error,
			"This group does not accept Letter Sealing v2 for this message type.");

	guint8 pub[32], shared[32], key[32], iv[32],
		plain[48 + EVP_MAX_BLOCK_LENGTH];
	if (!public_by_id(s, creator, creator_id, pub, error))
		return FALSE;
	gboolean ok = (!field(s->keys, "own") && sealing_vault_ops.ecdh) ?
			      sealing_vault_ops.ecdh(sealing_vault_ctx,
						     receiver_id, pub, shared,
						     error) :
			      line_own_ecdh(s, receiver_id, pub, shared, error);
	if (!ok)
		return FALSE;
	gsize size = 0;

	if (ok) {
		wrapping_key(shared, key, iv);
		ok = unwrap(key, iv, wrapped, (int)len, plain, &size) &&
		     size == 32;
		OPENSSL_cleanse(key, sizeof(key));
		OPENSSL_cleanse(iv, sizeof(iv));
	}
	OPENSSL_cleanse(shared, sizeof(shared));
	if (ok)
		memcpy(group, plain, 32);
	OPENSSL_cleanse(plain, sizeof(plain));
	if (!ok)
		return fail(
			error,
			"This device could not unwrap the existing group key.");
	*id = returned;
	if (!slot)
		slot = g_strdup_printf("%s/%d", chat, *id);
	if (!groups) {
		groups = obj();
		json_object_object_add(s->keys, "groups", groups);
	}
	json_object_object_add(groups, slot, from64(group, 32));
	return TRUE;
}

static gboolean message_shared_secret(Line *s, const char *to, const char *from,
				      gint32 sender, gint32 *receiver,
				      gboolean sending, int type,
				      guint8 shared[32], GError **error)
{
	gboolean mine = !strcmp(from, s->mid);

	if (to[0] == 'u') {
		return direct_shared_secret(s, mine ? sender : *receiver,
					    mine ? to : from,
					    mine ? receiver : &sender, sending,
					    type, shared, error);
	}
	guint8 pub[32], group[32];

	if (!public_by_id(s, from, sender, pub, error) ||
	    !group_private_key(s, to, sending, type, receiver, group, error))
		return FALSE;
	gboolean ok = shared_secret(group, pub, shared);

	OPENSSL_cleanse(group, sizeof(group));
	return ok || fail(error, "Invalid group Letter Sealing key.");
}

static gboolean gcm(gboolean encrypt, const guint8 key[32], const guint8 *nonce,
		    int nonce_len, const guint8 aad[82], const guint8 *input,
		    int n, guint8 *out, guint8 tag[16])
{
	g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();
	int written = 0, last = 0;

	if (!ctx ||
	    EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL,
			      encrypt) != 1 ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce_len, NULL) !=
		    1 ||
	    EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, encrypt) != 1 ||
	    EVP_CipherUpdate(ctx, NULL, &written, aad, 82) != 1 ||
	    EVP_CipherUpdate(ctx, out, &written, input, n) != 1)
		return FALSE;
	if (!encrypt &&
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1)
		return FALSE;
	if (EVP_CipherFinal_ex(ctx, out + written, &last) != 1 ||
	    written + last != n)
		return FALSE;
	return !encrypt ||
	       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
}

static void add_bytes(J *list, const void *data, int n)
{
	json_object_array_add(list, json_object_new_string_len(data, n));
}

J *line_seal_payload(Line *s, const char *chat, int type, J *payload,
		     GError **error)
{
	gint32 sender, receiver = 0;

	if (!valid_mid(chat, "ucr") || !valid_mid(s->mid, "u")) {
		fail(error, "Select a LINE conversation before sending.");
		return NULL;
	}
	if (!json_object_is_type(payload, json_type_object)) {
		fail(error, "Invalid Letter Sealing message payload.");
		return NULL;
	}
	if (!read_id(field(s->keys, "active"), &sender)) {
		line_letter_sealing_relink(error);
		return NULL;
	}
	guint8 salt[16], nonce[12], aad[82], key[32], tag[16];

	if (RAND_bytes(salt, sizeof(salt)) != 1 ||
	    RAND_bytes(nonce, sizeof(nonce)) != 1) {
		fail(error, "Unable to generate a message nonce.");
		return NULL;
	}
	size_t len;
	const char *plain = json_object_to_json_string_length(
		payload, JSON_C_TO_STRING_PLAIN, &len);
	if (len > G_MAXINT - EVP_MAX_BLOCK_LENGTH - 16) {
		fail(error, "Message is too large.");
		return NULL;
	}
	g_autoptr(GError) key_error = NULL;
	guint8 shared[32];

	if (!message_shared_secret(s, chat, s->mid, sender, &receiver, TRUE,
				   type, shared, &key_error)) {
		OPENSSL_cleanse(shared, sizeof(shared));
		if (g_error_matches(key_error, LINE_ERROR,
				    LINE_OWN_KEY_MISSING))
			line_letter_sealing_relink(error);
		else if (key_error)
			g_propagate_error(error, g_steal_pointer(&key_error));
		return NULL;
	}
	digest(shared, 32, salt, 16, "Key", key);
	OPENSSL_cleanse(shared, sizeof(shared));
	make_aad(aad, chat, s->mid, sender, receiver, (guint32)type);
	g_autofree guint8 *cipher = g_malloc(len + EVP_MAX_BLOCK_LENGTH + 16);
	gboolean ok = gcm(TRUE, key, nonce, sizeof(nonce), aad,
			  (const guint8 *)plain, (int)len, cipher, tag);
	OPENSSL_cleanse(key, sizeof(key));
	if (!ok) {
		fail(error, "Letter Sealing encryption failed.");
		return NULL;
	}
	memcpy(cipher + len, tag, 16);
	guint32 sid = GUINT32_TO_BE((guint32)sender),
		rid = GUINT32_TO_BE((guint32)receiver);
	J *chunks = json_object_new_array();

	add_bytes(chunks, salt, sizeof(salt));
	add_bytes(chunks, cipher, (int)len + 16);
	add_bytes(chunks, nonce, sizeof(nonce));
	add_bytes(chunks, &sid, 4);
	add_bytes(chunks, &rid, 4);
	return chunks;
}

static gboolean message_version(J *message, gint32 *version)
{
	J *value = field(field(message, "18"), "e2eeVersion");

	if (!value || (json_object_is_type(value, json_type_string) &&
		       !json_object_get_string_len(value))) {
		*version = 2;
		return TRUE;
	}
	return read_id(value, version) && (*version == 1 || *version == 2);
}

static guint32 envelope_id(const guint8 *bytes, gsize len)
{
	guint32 result = 0;

	for (gsize i = 0; i < len; i++)
		result = (result << 8) | bytes[i];
	return result;
}

J *line_open_payload(Line *s, J *message, int type, GError **error)
{
	const char *from = strfield(message, "1"), *to = strfield(message, "2");
	gint32 version;

	if (!valid_mid(from, "u") || !valid_mid(to, "ucr") ||
	    !valid_mid(s->mid, "u") ||
	    (to[0] == 'u' && strcmp(from, s->mid) && strcmp(to, s->mid)) ||
	    !message_version(message, &version) ||
	    json_object_get_int(field(message, "15")) != type) {
		fail(error,
		     "This encrypted message requires an unsupported chat type or Letter Sealing version.");
		return NULL;
	}
	J *chunks = field(message, "20");

	if (!json_object_is_type(chunks, json_type_array) ||
	    json_object_array_length(chunks) != 5) {
		fail(error,
		     "The message has an invalid Letter Sealing envelope.");
		return NULL;
	}
	const guint8 *part[5];
	gsize n[5] = { 0 };

	for (size_t i = 0; i < 5; i++)
		part[i] = binary(json_object_array_get_idx(chunks, i), &n[i]);
	gboolean legacy = version == 1;

	if (n[1] < 16 || n[1] > G_MAXINT || !n[3] || n[3] > 4 || !n[4] ||
	    n[4] > 4 ||
	    (legacy ? (n[0] != 8 || n[1] % 16 || n[2] != 16) :
		      (n[0] != 16 || !n[2] || n[2] > 32))) {
		fail(error,
		     "The message has invalid Letter Sealing chunk lengths.");
		return NULL;
	}
	guint32 sid = envelope_id(part[3], n[3]);
	guint32 rid = envelope_id(part[4], n[4]);

	if (sid > G_MAXINT32 || rid > G_MAXINT32) {
		fail(error, "The message has invalid Letter Sealing key IDs.");
		return NULL;
	}
	gint32 receiver = (gint32)rid;
	guint8 key[32];
	guint8 shared[32];
	struct history_secret *cached =
		history_secret(s, to, from, (gint32)sid, receiver);
	if (!cached &&
	    !message_shared_secret(s, to, from, (gint32)sid, &receiver, FALSE,
				   type, shared, error)) {
		OPENSSL_cleanse(shared, sizeof(shared));
		return NULL;
	}
	const guint8 *secret = cached ? cached->secret : shared;
	gsize len = legacy ? n[1] : n[1] - 16;
	gsize room = len + EVP_MAX_BLOCK_LENGTH;
	g_autofree guint8 *plain = g_malloc(room);
	gboolean ok;

	digest(secret, 32, part[0], n[0], "Key", key);
	if (legacy) {
		guint8 iv[32];

		digest(secret, 32, part[0], n[0], "IV", iv);
		fold_digest(iv);
		ok = check_mac(part[2], n[2], part[1], n[1], key) &&
		     unwrap(key, iv, part[1], (int)n[1], plain, &len);
		OPENSSL_cleanse(iv, sizeof(iv));
	} else {
		guint8 aad[82], tag[16];

		make_aad(aad, to, from, sid, rid, (guint32)type);
		memcpy(tag, part[1] + len, sizeof(tag));
		ok = gcm(FALSE, key, part[2], (int)n[2], aad, part[1], (int)len,
			 plain, tag);
	}
	OPENSSL_cleanse(key, sizeof(key));
	J *result = NULL;

	if (ok) {
		struct json_tokener *tok = json_tokener_new();

		json_tokener_set_flags(tok, JSON_TOKENER_STRICT |
						    JSON_TOKENER_VALIDATE_UTF8);
		g_autoptr(J) body = json_tokener_parse_ex(
			tok, (const char *)plain, (int)len);
		if (json_tokener_get_error(tok) == json_tokener_success &&
		    json_tokener_get_parse_end(tok) == (size_t)len &&
		    json_object_is_type(body, json_type_object))
			result = g_steal_pointer(&body);
		json_tokener_free(tok);
	}
	OPENSSL_cleanse(plain, room);
	if (result && !cached)
		remember_secret(s, to, from, (gint32)sid, receiver, shared);
	OPENSSL_cleanse(shared, sizeof(shared));
	if (!result)
		fail(error,
		     ok ? "The sealed message has an invalid payload." :
			  "Letter Sealing could not authenticate this message.");
	return result;
}

J *line_seal_text(Line *s, const char *chat, const char *text, GError **error)
{
	if (!text || !g_utf8_validate(text, -1, NULL)) {
		fail(error, "The message must be valid UTF-8.");
		return NULL;
	}
	g_autoptr(J) payload = obj();
	json_object_object_add(payload, "text", json_object_new_string(text));
	return line_seal_payload(s, chat, 0, payload, error);
}

char *line_open_text(Line *s, J *message, GError **error)
{
	g_autoptr(J) payload = line_open_payload(s, message, 0, error);
	if (!payload)
		return NULL;
	const char *text = strfield(payload, "text");

	if (!text || strlen(text) != (size_t)json_object_get_string_len(
					     field(payload, "text"))) {
		fail(error, "The sealed message has no valid UTF-8 text.");
		return NULL;
	}
	return g_strdup(text);
}
