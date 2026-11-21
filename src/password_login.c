// SPDX-License-Identifier: GPL-3.0-or-later
#include "sealing.h"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <string.h>

/* Desktop email login v3. See the companion protocol reference in README. */
#define AUTH_PATH "/api/v3p/rs"
#define DEVICE_NAME "Avernella"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BIGNUM, BN_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OSSL_PARAM_BLD, OSSL_PARAM_BLD_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OSSL_PARAM, OSSL_PARAM_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY_CTX, EVP_PKEY_CTX_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_CIPHER_CTX, EVP_CIPHER_CTX_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY, EVP_PKEY_free)

static gboolean fail(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return FALSE;
}

static gboolean valid_email(const char *email)
{
	const char *at = strchr(email, '@');

	if (!at || at == email || !at[1] || strchr(at + 1, '@'))
		return FALSE;
	for (const char *p = email; *p; p++)
		if (g_ascii_isspace(*p) || g_ascii_iscntrl(*p))
			return FALSE;
	return TRUE;
}

static BIGNUM *parse_hex(const char *hex, size_t limit)
{
	BIGNUM *number = NULL;

	if (hex && *hex && strlen(hex) <= limit &&
	    strspn(hex, "0123456789abcdefABCDEF") == strlen(hex))
		BN_hex2bn(&number, hex);
	return number;
}

static char *encrypt(J *key, const guint8 *plain, size_t len)
{
	g_autoptr(BIGNUM) n = parse_hex(strfield(key, "2"), 1024);
	g_autoptr(BIGNUM) e = parse_hex(strfield(key, "3"), 8);
	g_autoptr(OSSL_PARAM_BLD) builder = OSSL_PARAM_BLD_new();
	g_autoptr(EVP_PKEY_CTX) load =
		EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	EVP_PKEY *key_out = NULL;

	if (!n || !e || BN_num_bits(n) < 1024 || !builder || !load ||
	    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) != 1 ||
	    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e) != 1)
		return NULL;
	g_autoptr(OSSL_PARAM) params = OSSL_PARAM_BLD_to_param(builder);

	if (!params || EVP_PKEY_fromdata_init(load) != 1 ||
	    EVP_PKEY_fromdata(load, &key_out, EVP_PKEY_PUBLIC_KEY, params) != 1)
		return NULL;
	g_autoptr(EVP_PKEY) public = key_out;
	g_autoptr(EVP_PKEY_CTX) ctx = EVP_PKEY_CTX_new(public, NULL);
	guint8 ciphertext[512];
	size_t size = sizeof(ciphertext);

	if (!ctx || EVP_PKEY_public_check(ctx) != 1 ||
	    EVP_PKEY_encrypt_init(ctx) != 1 ||
	    EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) != 1 ||
	    EVP_PKEY_encrypt(ctx, ciphertext, &size, plain, len) != 1)
		return NULL;

	static const char digits[] = "0123456789abcdef";
	char *hex = g_malloc(2 * size + 1);

	for (size_t i = 0; i < size; i++) {
		hex[2 * i] = digits[ciphertext[i] >> 4];
		hex[2 * i + 1] = digits[ciphertext[i] & 15];
	}
	hex[2 * size] = 0;
	return hex;
}

static char *wrap_credentials(Line *s, const char *email, char *password,
			      char **id, GError **error)
{
	g_autoptr(J) args = obj();
	g_autoptr(J) key = NULL;
	char *hex = NULL;
	size_t password_len = strlen(password);

	if (!valid_email(email) || strlen(email) > LINE_CREDENTIAL_LIMIT ||
	    !password_len || password_len > LINE_CREDENTIAL_LIMIT) {
		fail(error,
		     "Enter your registered LINE email address and password.");
		goto out;
	}
	J *request = obj();

	json_object_object_add(args, "1:obj", request);
	json_object_object_add(request, "2:i32", json_object_new_int(0));
	key = line_auth_call(s, "/api/v3/TalkService.do", "getRSAKeyInfo", args,
			     error);
	if (!key)
		goto out;

	const char *session = strfield(key, "4");
	const char *identifier = strfield(key, "1");

	if (!identifier || !*identifier || !session || !*session ||
	    strlen(session) > LINE_CREDENTIAL_LIMIT) {
		fail(error, "LINE returned an invalid sign-in key.");
		goto out;
	}
	const char *const parts[] = { session, email, password };
	guint8 plain[3 * (LINE_CREDENTIAL_LIMIT + 1)];
	size_t used = 0;

	for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
		size_t n = strlen(parts[i]);

		plain[used++] = n;
		memcpy(plain + used, parts[i], n);
		used += n;
	}
	hex = encrypt(key, plain, used);
	OPENSSL_cleanse(plain, used);
	if (hex)
		*id = g_strdup(identifier);
	else
		fail(error,
		     "Could not encrypt these credentials with LINE's sign-in key.");
out:
	OPENSSL_cleanse(password, password_len);
	return hex;
}

static gboolean make_pin_secret(const char pin[7], const guint8 public[32],
				guint8 encrypted[32 + EVP_MAX_BLOCK_LENGTH])
{
	g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();
	guint8 key[32];
	int written = 0, tail = 0;
	gboolean ok =
		ctx && EVP_Digest(pin, 6, key, NULL, EVP_sha256(), NULL) == 1 &&
		EVP_EncryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key, NULL) ==
			1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, encrypted, &written, public, 32) == 1 &&
		EVP_EncryptFinal_ex(ctx, encrypted + written, &tail) == 1 &&
		written + tail == 32;

	OPENSSL_cleanse(key, sizeof(key));
	return ok;
}

static gboolean make_pairing_key(char pin[7], guint8 secret[32],
				 guint8 encrypted[32 + EVP_MAX_BLOCK_LENGTH])
{
	g_autoptr(EVP_PKEY) key = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
	guint8 public[32];
	size_t n = 32, public_len = 32;
	guint32 random;

	if (!key || EVP_PKEY_get_raw_private_key(key, secret, &n) != 1 ||
	    EVP_PKEY_get_raw_public_key(key, public, &public_len) != 1 ||
	    n != 32 || public_len != 32)
		return FALSE;
	do {
		if (RAND_bytes((guint8 *)&random, sizeof(random)) != 1)
			return FALSE;
	} while (random >= G_MAXUINT32 - G_MAXUINT32 % 1000000);
	g_snprintf(pin, 7, "%06u", random % 1000000);
	return make_pin_secret(pin, public, encrypted);
}

static J *confirm(Line *s, Line *imported, const char *verifier,
		  const char pin[7], const guint8 secret[32], GError **error)
{
	line_event("pin", "pin", pin);
	g_autoptr(J) approval = line_auth_wait(s, verifier, error);

	if (!approval) {
		if (error &&
		    g_error_matches(*error, LINE_ERROR, LINE_TIMEOUT)) {
			g_clear_error(error);
			fail(error,
			     "The PIN expired. Sign in again for a new code.");
		}
		return NULL;
	}
	J *metadata = field(field(approval, "result"), "metadata");

	if (!metadata) {
		fail(error,
		     "LINE did not confirm this device. Try again or sign in with a QR code.");
		return NULL;
	}
	guint8 proof[16];

	if (!line_import_login_keys(imported, metadata, secret, proof, error))
		return NULL;

	g_autoptr(J) args = obj();

	json_object_object_add(args, "1:str", json_object_new_string(verifier));
	json_object_object_add(args, "2:str",
			       json_object_new_string_len((const char *)proof,
							  sizeof(proof)));
	OPENSSL_cleanse(proof, sizeof(proof));
	return line_auth_call(s, AUTH_PATH, "confirmE2EELogin", args, error);
}

static gboolean password_session(Line *s, const char *id, const char *cipher,
				 const char pin[7], const guint8 secret[32],
				 const guint8 pin_secret[32], GError **error)
{
	g_autoptr(J) args = obj();
	J *request = obj();

	json_object_object_add(args, "2:obj", request);
	json_object_object_add(request, "1:i32", json_object_new_int(2));
	json_object_object_add(request, "2:i32", json_object_new_int(1));
	json_object_object_add(request, "3:str", json_object_new_string(id));
	json_object_object_add(request, "4:str",
			       json_object_new_string(cipher));
	json_object_object_add(request, "5:bool",
			       json_object_new_boolean(TRUE));
	json_object_object_add(request, "6:str", json_object_new_string(""));
	json_object_object_add(request, "7:str",
			       json_object_new_string(DEVICE_NAME));
	json_object_object_add(
		request, "10:str",
		json_object_new_string_len((const char *)pin_secret, 32));
	json_object_object_add(request, "11:i32", json_object_new_int(1));
	json_object_object_add(request, "12:str",
			       json_object_new_string(DEVICE_NAME));
	g_autoptr(J) initial =
		line_auth_call(s, AUTH_PATH, "loginV2", args, error);

	if (!initial) {
		if (s->service_code == 18 || s->service_code == 19 ||
		    s->service_code == 21 || s->service_code == 23) {
			g_clear_error(error);
			return fail(
				error,
				"LINE did not accept that email address and password.");
		}
		return FALSE;
	}
	const char *access = strfield(field(initial, "9"), "1");

	if (access && *access) {
		/* No device certificate is sent, so require phone-approved keys. */
		g_autoptr(J) revoked =
			line_call(s, "/AS4", "logoutV2", NULL, access, 0, NULL);
		return fail(
			error,
			"LINE issued a session without sharing encryption keys. Sign in with a QR code.");
	}
	const char *verifier = strfield(initial, "3");

	if (!verifier || !*verifier ||
	    json_object_get_int(field(initial, "5")) == 4)
		return fail(
			error,
			"LINE requires another verification method. Sign in with a QR code.");
	Line imported = { 0 };

	g_autoptr(J) approved =
		confirm(s, &imported, verifier, pin, secret, error);
	g_autoptr(J) keys = imported.keys;

	if (!approved)
		return FALSE;
	if (!json_object_is_type(approved, json_type_string) ||
	    !*json_object_get_string(approved))
		return fail(
			error,
			"LINE did not return the verification needed to sign in.");
	json_object_object_add(request, "1:i32", json_object_new_int(1));
	json_object_object_add(request, "9:str", json_object_get(approved));
	g_autoptr(J) result =
		line_auth_call(s, AUTH_PATH, "loginV2", args, error);

	if (!result)
		return FALSE;
	J *tokens = field(result, "9");

	access = strfield(tokens, "1");
	if (!access || !*access)
		return fail(error, "LINE did not issue a desktop session.");

	s->access = g_strdup(access);
	s->refresh = g_strdup(strfield(tokens, "2"));
	s->mid = g_strdup(strfield(result, "10"));
	json_object_put(s->keys);
	s->keys = g_steal_pointer(&keys);
	line_schedule_refresh(s, field(tokens, "3"), field(tokens, "6"));
	return TRUE;
}

gboolean line_password_login(Line *s, const char *email, char *password,
			     GError **error)
{
	g_autofree char *id = NULL;
	g_autofree char *cipher =
		wrap_credentials(s, email, password, &id, error);

	if (!cipher)
		return FALSE;
	char pin[7];
	guint8 secret[32], encrypted[32 + EVP_MAX_BLOCK_LENGTH];
	gboolean ok = make_pairing_key(pin, secret, encrypted) ?
			      password_session(s, id, cipher, pin, secret,
					       encrypted, error) :
			      fail(error, "Could not create a sign-in key.");

	OPENSSL_cleanse(cipher, strlen(cipher));
	OPENSSL_cleanse(secret, sizeof(secret));
	OPENSSL_cleanse(encrypted, sizeof(encrypted));
	OPENSSL_cleanse(pin, sizeof(pin));
	return ok;
}
