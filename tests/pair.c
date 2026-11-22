// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include "sealing.h"
#include <openssl/evp.h>
#include <string.h>

enum pair_outcome {
	APPROVE,
	DENY_SCAN,
	DENY_PIN,
	NO_PIN,
	NO_ACCESS,
	BAD_LINK,
	BAD_KEYCHAIN,
	EXPIRE,
	CERT_ACCEPTED,
	QR_DISALLOWED,
	CERT_TRANSPORT
};
static enum pair_outcome outcome;
static unsigned int polls, pins, scans, verified_pins, auths, imports,
	revocations;
static unsigned int certificates;
static guint8 presented_public_key[32];

void line_event(const char *event, const char *key, const char *value)
{
	if (!strcmp(event, "qr")) {
		g_assert_cmpstr(key, ==, "url");
		g_autoptr(GError) e = NULL;
		g_autoptr(GUri) uri =
			g_uri_parse(value, G_URI_FLAGS_ENCODED_QUERY, &e);
		g_assert_no_error(e);
		g_autoptr(GHashTable) query = g_uri_parse_params(
			g_uri_get_query(uri), -1, "&", G_URI_PARAMS_NONE, &e);
		g_assert_no_error(e);
		g_assert_cmpstr(g_hash_table_lookup(query, "e2eeVersion"), ==,
				"1");
		const char *encoded = g_hash_table_lookup(query, "secret");

		g_assert_nonnull(encoded);
		gsize len;

		g_autofree guint8 *decoded = g_base64_decode(encoded, &len);

		g_assert_cmpuint(len, ==, sizeof(presented_public_key));
		memcpy(presented_public_key, decoded, len);
		scans++;
		return;
	}
	g_assert_cmpstr(event, ==, "pin");
	g_assert_cmpstr(key, ==, "pin");
	g_assert_cmpstr(value, ==, "012345");
	pins++;
}

gboolean line_import_keys(Line *s, J *meta, const guint8 secret[32],
			  GError **error)
{
	(void)s;
	imports++;
	g_assert_cmpstr(strfield(meta, "fixture"), ==, "phone-approved");
	/*
	 * The ephemeral public key displayed in the QR must correspond to the
	 * private key used to unwrap the phone's approved keychain.
	 */
	EVP_PKEY *key =
		EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, secret, 32);
	g_assert_nonnull(key);
	size_t len = 32;
	guint8 derived[32];

	g_assert_cmpint(EVP_PKEY_get_raw_public_key(key, derived, &len), ==, 1);
	EVP_PKEY_free(key);
	g_assert_cmpmem(derived, len, presented_public_key,
			sizeof(presented_public_key));
	if (outcome != BAD_KEYCHAIN)
		return TRUE;
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Fixture keychain validation failed.");
	return FALSE;
}

void line_schedule_refresh(Line *s, J *duration, J *issued)
{
	/* TokenV3IssueResult fields 3 and 6, recorded as the real helper does. */
	s->refresh_at =
		json_object_get_int64(issued) + json_object_get_int64(duration);
}

J *line_call(Line *s, const char *path, const char *method, J *args,
	     const char *access, int seconds, GError **error)
{
	s->service_code = -1;
	if (!strcmp(method, "logoutV2")) {
		g_assert_cmpstr(path, ==, "/AS4");
		g_assert_cmpstr(access, ==, "fixture-access");
		revocations++;
		return obj();
	}
	if (!strcmp(method, "createSession")) {
		g_assert_cmpstr(path, ==, "/acct/lgn/sq/v1");
		g_assert_null(access);
		return json_tokener_parse("{\"1\":\"fixture-session\"}");
	}
	J *request = field(args, "1:obj");

	g_assert_cmpstr(strfield(request, "1:str"), ==, "fixture-session");
	if (!strcmp(method, "checkQrCodeVerified") ||
	    !strcmp(method, "checkPinCodeVerified")) {
		g_assert_cmpstr(path, ==, "/acct/lp/lgn/sq/v1");
		g_assert_cmpstr(access, ==, "fixture-session");
		g_assert_cmpint(seconds, >, 0);
		if (!strcmp(method, "checkQrCodeVerified")) {
			polls++;
			g_assert_cmpuint(scans, ==, 1);
			if (outcome == EXPIRE ||
			    (outcome == APPROVE && polls == 1)) {
				g_set_error_literal(error, LINE_ERROR,
						    LINE_TIMEOUT,
						    "Poll timeout.");
				return NULL;
			}
			if (outcome != DENY_SCAN)
				return obj();
		} else {
			g_assert_cmpuint(pins, ==, 1);
			if (outcome != DENY_PIN) {
				verified_pins++;
				return obj();
			}
		}
		g_set_error_literal(error, LINE_ERROR, 1, "Approval denied.");
		return NULL;
	}
	g_assert_cmpstr(path, ==, "/acct/lgn/sq/v1");
	g_assert_null(access);
	if (!strcmp(method, "createQrCodeForSecure")) {
		J *qr = obj();
		const char *link = outcome == BAD_LINK ?
					   "https://line.me.invalid/" :
					   "https://line.me/R/au/q/fixture";
		json_object_object_add(qr, "1", json_object_new_string(link));
		json_object_object_add(qr, "2", json_object_new_int(2));
		json_object_object_add(qr, "3", json_object_new_int(1));
		json_object_object_add(qr, "4",
				       json_object_new_string("test-nonce"));
		return qr;
	}
	if (!strcmp(method, "verifyCertificate")) {
		g_assert_null(field(
			request,
			"2:str")); /* new client has no saved certificate */
		certificates++;
		if (outcome == CERT_ACCEPTED)
			return obj();
		if (outcome == CERT_TRANSPORT) {
			g_set_error_literal(error, LINE_ERROR, LINE_TIMEOUT,
					    "Connection timeout.");
			return NULL;
		}
		s->service_code = outcome == QR_DISALLOWED ? 3 : 2;
		g_set_error_literal(error, LINE_ERROR, 1,
				    "New device requires a PIN.");
		return NULL;
	}
	if (!strcmp(method, "createPinCode")) {
		g_assert_cmpuint(polls, >, 0);
		g_assert_cmpuint(certificates, ==, 1);
		g_assert_true(outcome != CERT_ACCEPTED &&
			      outcome != QR_DISALLOWED &&
			      outcome != CERT_TRANSPORT);
		return json_tokener_parse(outcome == NO_PIN ?
						  "{\"1\":\"\"}" :
						  "{\"1\":\"012345\"}");
	}
	g_assert_cmpstr(method, ==, "qrCodeLoginV2ForSecure");
	g_assert_cmpuint(verified_pins, ==, outcome != CERT_ACCEPTED);
	g_assert_cmpstr(strfield(request, "2:str"), ==, "Avernella");
	g_assert_cmpstr(strfield(request, "3:str"), ==, "Avernella");
	g_assert_true(json_object_get_boolean(field(request, "4:bool")));
	g_assert_cmpstr(strfield(request, "5:str"), ==, "test-nonce");
	auths++;
	if (outcome == NO_ACCESS)
		return json_tokener_parse("{\"3\":{\"1\":\"\"}}");
	return json_tokener_parse(
		"{\"3\":{\"1\":\"fixture-access\",\"2\":\"fixture-refresh\","
		"\"3\":604800,\"6\":1800000000},"
		"\"10\":{\"fixture\":\"phone-approved\"}}");
}

static void exercise(gconstpointer data)
{
	outcome = GPOINTER_TO_INT(data);
	polls = pins = scans = verified_pins = auths = imports = revocations =
		certificates = 0;
	Line s = { 0 };

	g_autoptr(GError) error = NULL;
	gint64 started = g_get_monotonic_time();

	if (outcome == APPROVE || outcome == CERT_ACCEPTED) {
		g_assert_true(line_pair(&s, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(polls, ==, outcome == APPROVE ? 2 : 1);
		/* The fixture's instant timeout still costs a whole 1 s round. */
		if (outcome == APPROVE)
			g_assert_cmpint(g_get_monotonic_time() - started, >=,
					G_USEC_PER_SEC - 20000);
		g_assert_cmpuint(certificates, ==, 1);
		g_assert_cmpuint(pins, ==, outcome == APPROVE);
		g_assert_cmpuint(imports, ==, 1);
		g_assert_true(s.token_changed);
		g_assert_cmpstr(s.access, ==, "fixture-access");
		g_assert_cmpstr(s.refresh, ==, "fixture-refresh");
		g_assert_cmpint(s.refresh_at, ==, 1800000000 + 604800);
		g_free(s.access);
		g_free(s.refresh);
		return;
	}
	g_assert_false(line_pair(&s, &error));
	g_assert_nonnull(error);
	g_assert_null(s.access);
	g_assert_false(s.token_changed);
	g_assert_cmpuint(auths, ==,
			 outcome == BAD_KEYCHAIN || outcome == NO_ACCESS);
	g_assert_cmpuint(imports, ==, outcome == BAD_KEYCHAIN);
	g_assert_cmpuint(revocations, ==, outcome == BAD_KEYCHAIN);
	if (outcome == BAD_LINK)
		g_assert_cmpuint(scans, ==, 0);
	if (outcome == EXPIRE) {
		/* Two early answers never become a burst: two rounds, ~2 s. */
		g_assert_cmpuint(polls, ==, 2);
		g_assert_cmpint(g_get_monotonic_time() - started, >=,
				2 * G_USEC_PER_SEC - 40000);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	const char *const names[] = { "approve",
				      "deny-scan",
				      "deny-pin",
				      "empty-pin",
				      "empty-token",
				      "invalid-qr-link",
				      "invalid-keychain",
				      "expired",
				      "certificate-accepted",
				      "qr-login-disallowed",
				      "certificate-transport-error" };
	for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) {
		g_autofree char *name = g_strconcat("/pair/", names[i], NULL);

		g_test_add_data_func(name, GINT_TO_POINTER((int)i), exercise);
	}
	return g_test_run();
}
