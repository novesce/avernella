// SPDX-License-Identifier: GPL-3.0-or-later
#include "sealing.h"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <string.h>

#define EMAIL "fixture@example.invalid"
#define PASSWORD "a sample password"
#define VERIFIER "pending-phone-verifier"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY, EVP_PKEY_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY_CTX, EVP_PKEY_CTX_free)

static EVP_PKEY *rsa;
static unsigned int stage;
static gboolean invalid_credentials, rejected_keys;
static char pin[7];

static void field_string(J *o, const char *key, const char *value)
{
	json_object_object_add(o, key, json_object_new_string(value));
}

static void verify_cipher(J *request)
{
	const char *hex = strfield(request, "4:str");
	const guint8 expected[] = "\007session\027" EMAIL "\021" PASSWORD;
	guint8 cipher[256], plain[256];
	size_t len = sizeof(plain);

	g_assert_nonnull(hex);
	g_assert_cmpuint(strlen(hex), ==, 2 * sizeof(cipher));
	for (size_t i = 0; i < sizeof(cipher); i++) {
		int high = g_ascii_xdigit_value(hex[2 * i]);
		int low = g_ascii_xdigit_value(hex[2 * i + 1]);

		g_assert_cmpint(high, >=, 0);
		g_assert_cmpint(low, >=, 0);
		cipher[i] = (high << 4) | low;
	}
	g_autoptr(EVP_PKEY_CTX) ctx = EVP_PKEY_CTX_new(rsa, NULL);

	g_assert_cmpint(EVP_PKEY_decrypt_init(ctx), ==, 1);
	g_assert_cmpint(EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING),
			==, 1);
	g_assert_cmpint(EVP_PKEY_decrypt(ctx, plain, &len, cipher,
					 sizeof(cipher)),
			==, 1);
	g_assert_cmpmem(plain, len, expected, sizeof(expected) - 1);
}

J *line_auth_call(Line *s, const char *path, const char *method, J *args,
		  GError **error)
{
	unsigned int call = stage++;

	if (call == 0) {
		BIGNUM *n = NULL, *e = NULL;

		g_assert_cmpstr(path, ==, "/api/v3/TalkService.do");
		g_assert_cmpstr(method, ==, "getRSAKeyInfo");
		g_assert_cmpint(json_object_get_int(
					field(field(args, "1:obj"), "2:i32")),
				==, 0);
		g_assert_cmpint(
			EVP_PKEY_get_bn_param(rsa, OSSL_PKEY_PARAM_RSA_N, &n),
			==, 1);
		g_assert_cmpint(
			EVP_PKEY_get_bn_param(rsa, OSSL_PKEY_PARAM_RSA_E, &e),
			==, 1);
		char *modulus = BN_bn2hex(n), *exponent = BN_bn2hex(e);
		J *key = obj();

		field_string(key, "1", "fixture-key");
		field_string(key, "2", modulus);
		field_string(key, "3", exponent);
		field_string(key, "4", "session");
		OPENSSL_free(modulus);
		OPENSSL_free(exponent);
		BN_free(n);
		BN_free(e);
		return key;
	}
	g_assert_cmpstr(path, ==, "/api/v3p/rs");
	if (call == 3) {
		g_assert_cmpstr(method, ==, "confirmE2EELogin");
		g_assert_cmpstr(strfield(args, "1:str"), ==, VERIFIER);
		g_assert_cmpint(
			json_object_get_string_len(field(args, "2:str")), ==,
			16);
		g_assert_cmpmem(strfield(args, "2:str"), 16, "confirmation-key",
				16);
		return json_object_new_string("confirmed-verifier");
	}

	J *req = field(args, "2:obj");

	g_assert_cmpstr(method, ==, "loginV2");
	g_assert_cmpstr(strfield(req, "3:str"), ==, "fixture-key");
	g_assert_cmpint(json_object_get_int(field(req, "2:i32")), ==, 1);
	g_assert_cmpint(json_object_get_string_len(field(req, "10:str")), ==,
			32);
	verify_cipher(req);

	if (call == 1) {
		g_assert_cmpint(json_object_get_int(field(req, "1:i32")), ==,
				2);
		g_assert_null(field(req, "9:str"));
		if (invalid_credentials) {
			s->service_code = 18;
			g_set_error_literal(error, LINE_ERROR, 1, "Rejected.");
			return NULL;
		}
		J *challenge = obj();

		field_string(challenge, "3", VERIFIER);
		json_object_object_add(challenge, "5", json_object_new_int(3));
		return challenge;
	}
	g_assert_cmpuint(call, ==, 4);
	g_assert_cmpint(json_object_get_int(field(req, "1:i32")), ==, 1);
	g_assert_cmpstr(strfield(req, "9:str"), ==, "confirmed-verifier");
	J *account = obj(), *token = obj();

	json_object_object_add(account, "9", token);
	field_string(token, "1", "desktop-access-token");
	field_string(token, "2", "desktop-refresh-token");
	json_object_object_add(token, "3", json_object_new_int(86400));
	return account;
}

J *line_auth_wait(Line *s, const char *verifier, GError **error)
{
	(void)s;
	(void)error;
	g_assert_cmpuint(stage++, ==, 2);
	g_assert_cmpstr(verifier, ==, VERIFIER);
	g_assert_cmpuint(strlen(pin), ==, 6);
	g_assert_cmpuint(strspn(pin, "0123456789"), ==, 6);
	return json_tokener_parse("{\"result\":{\"metadata\":{\"keyId\":10}}}");
}

gboolean line_import_login_keys(Line *s, J *meta, const guint8 secret[32],
				guint8 proof[16], GError **error)
{
	(void)secret;
	g_assert_cmpint(json_object_get_int(field(meta, "keyId")), ==, 10);
	if (rejected_keys) {
		g_set_error_literal(error, LINE_ERROR, 1, "Invalid key chain.");
		return FALSE;
	}
	memcpy(proof, "confirmation-key", 16);
	s->keys = obj();
	return TRUE;
}

J *line_call(Line *s, const char *path, const char *method, J *args,
	     const char *access, int timeout, GError **error)
{
	(void)s;
	(void)path;
	(void)method;
	(void)args;
	(void)access;
	(void)timeout;
	(void)error;
	g_assert_not_reached();
}

void line_event(const char *event, const char *key, const char *value)
{
	g_assert_cmpstr(event, ==, "pin");
	g_assert_cmpstr(key, ==, "pin");
	g_strlcpy(pin, value, sizeof(pin));
}

void line_schedule_refresh(Line *s, J *duration, J *issued)
{
	(void)issued;
	s->refresh_at = json_object_get_int64(duration);
	s->token_changed = TRUE;
}

static void attempt(gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	int outcome = GPOINTER_TO_INT(data);
	char password[] = PASSWORD;
	const char wiped[sizeof(password)] = { 0 };
	Line s = { 0 };

	stage = 0;
	invalid_credentials = outcome == 1;
	rejected_keys = outcome == 2;
	g_assert_cmpint(line_password_login(&s, EMAIL, password, &error), ==,
			outcome == 0);
	g_assert_cmpmem(password, sizeof(password), wiped, sizeof(wiped));
	if (outcome) {
		g_assert_nonnull(error);
		g_assert_null(s.access);
		g_assert_null(s.keys);
		g_assert_cmpuint(stage, ==, outcome + 1);
	} else {
		g_assert_no_error(error);
		g_assert_nonnull(s.keys);
		g_assert_cmpstr(s.access, ==, "desktop-access-token");
		g_assert_cmpstr(s.refresh, ==, "desktop-refresh-token");
		g_assert_cmpint(s.refresh_at, ==, 86400);
		g_assert_true(s.token_changed);
		g_assert_cmpuint(stage, ==, 5);
	}
	json_object_put(s.keys);
	g_free(s.access);
	g_free(s.refresh);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	rsa = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048);
	g_assert_nonnull(rsa);
	g_test_add_data_func("/password-login/approve", GINT_TO_POINTER(0),
			     attempt);
	g_test_add_data_func("/password-login/reject-credentials",
			     GINT_TO_POINTER(1), attempt);
	g_test_add_data_func("/password-login/reject-keychain",
			     GINT_TO_POINTER(2), attempt);
	int result = g_test_run();

	EVP_PKEY_free(rsa);
	return result;
}
