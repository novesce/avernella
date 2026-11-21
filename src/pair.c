// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include "sealing.h"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <string.h>

#define QR_SERVICE "/acct/lgn/sq/v1"
#define QR_POLL "/acct/lp/lgn/sq/v1"

static gboolean fail(GError **e, const char *why)
{
	g_set_error_literal(e, LINE_ERROR, 1, why);
	return FALSE;
}

static gboolean poll_approval(Line *s, const char *method, const char *session,
			      J *args, int rounds, int seconds, GError **error)
{
	gint64 deadline = g_get_monotonic_time() +
			  (gint64)rounds * seconds * G_USEC_PER_SEC;
	for (int i = 0; i < rounds && g_get_monotonic_time() < deadline; i++) {
		g_autoptr(GError) failure = NULL;
		g_autoptr(J) response = line_call(s, QR_POLL, method, args,
						  session, seconds, &failure);
		if (response)
			return TRUE;
		if (!g_error_matches(failure, LINE_ERROR, LINE_TIMEOUT)) {
			g_propagate_error(error, g_steal_pointer(&failure));
			return FALSE;
		}
	}
	return fail(
		error,
		"The QR code expired. Choose sign in to request a new one.");
}

static gboolean pair_with_key(Line *s, const guint8 secret[32],
			      const guint8 public[32], GError **error)
{
	g_autoptr(J) created =
		line_call(s, QR_SERVICE, "createSession", NULL, NULL, 0, error);
	if (!created)
		return FALSE;
	const char *session = strfield(created, "1");

	if (!session || !*session)
		return fail(error, "LINE did not return a pairing session.");

	g_autoptr(J) args = obj();
	J *request = obj();

	json_object_object_add(args, "1:obj", request);
	json_object_object_add(request, "1:str",
			       json_object_new_string(session));

	g_autoptr(J) qr = line_call(s, QR_SERVICE, "createQrCodeForSecure",
				    args, NULL, 0, error);
	if (!qr)
		return FALSE;
	const char *link = strfield(qr, "1");
	const char *nonce = strfield(qr, "4");

	if (!link ||
	    (!g_str_has_prefix(link, "line://") &&
	     !g_str_has_prefix(link, "https://line.me/")) ||
	    strlen(link) > 2048 || !nonce || !*nonce) {
		return fail(error,
			    "LINE returned an unrecognized pairing code.");
	}
	g_autofree char *pk = g_base64_encode(public, 32);
	g_autofree char *escaped = g_uri_escape_string(pk, NULL, FALSE);
	g_autofree char *url =
		g_strdup_printf("%s%csecret=%s&e2eeVersion=1", link,
				strchr(link, '?') ? '&' : '?', escaped);
	line_event("qr", "url", url);
	int rounds = field(qr, "2") ?
			     CLAMP(json_object_get_int(field(qr, "2")), 1, 12) :
			     12;
	int seconds =
		field(qr, "3") ?
			CLAMP(json_object_get_int(field(qr, "3")), 1, 60) :
			30;
	if (!poll_approval(s, "checkQrCodeVerified", session, args, rounds,
			   seconds, error))
		return FALSE;

	/*
	 * Ask LINE whether the scanned session needs an additional phone PIN.
	 * A fresh pairing sends no previously issued certificate.
	 */
	g_autoptr(GError) check_error = NULL;
	g_autoptr(J) certified = line_call(s, QR_SERVICE, "verifyCertificate",
					   args, NULL, 0, &check_error);
	if (!certified) {
		if (s->service_code != 1 && s->service_code != 2) {
			g_propagate_error(error, g_steal_pointer(&check_error));
			return FALSE;
		}
		g_autoptr(J) pin = line_call(s, QR_SERVICE, "createPinCode",
					     args, NULL, 0, error);
		if (!pin)
			return FALSE;
		const char *digits = strfield(pin, "1");

		if (!digits || strlen(digits) < 4 || strlen(digits) > 12 ||
		    digits[strspn(digits, "0123456789")])
			return fail(
				error,
				"LINE did not return a valid confirmation PIN.");
		line_event("pin", "pin", digits);
		if (!poll_approval(s, "checkPinCodeVerified", session, args,
				   rounds, seconds, error))
			return FALSE;
	}

	json_object_object_add(request, "2:str",
			       json_object_new_string("Avernella"));
	json_object_object_add(request, "3:str",
			       json_object_new_string("Avernella"));
	json_object_object_add(request, "4:bool",
			       json_object_new_boolean(TRUE));
	json_object_object_add(request, "5:str", json_object_new_string(nonce));
	g_autoptr(J) paired = line_call(s, QR_SERVICE, "qrCodeLoginV2ForSecure",
					args, NULL, 0, error);
	if (!paired)
		return FALSE;
	J *token_info = field(paired, "3");
	const char *access = strfield(token_info, "1");

	if (!access || !*access)
		return fail(error, "LINE did not issue a desktop session.");
	J *meta = field(paired, "10");

	if (!meta)
		meta = field(field(paired, "6"), "e2eeInfo");
	if (!meta)
		meta = field(paired, "6");
	if (!line_import_keys(s, meta, secret, error)) {
		/*
		 * Approval may already have created a remote session. Release it if
		 * key verification failed before we could keep a usable session.
		 */
		g_autoptr(J) revoked =
			line_call(s, "/AS4", "logoutV2", NULL, access, 0, NULL);
		return FALSE;
	}
	s->access = g_strdup(access);
	s->refresh = g_strdup(strfield(token_info, "2"));
	/* TokenV3IssueResult: 3 durationUntilRefreshInSec, 6 tokenIssueTimeEpochSec. */
	line_schedule_refresh(s, field(token_info, "3"),
			      field(token_info, "6"));
	s->token_changed = TRUE;
	return TRUE;
}

gboolean line_pair(Line *s, GError **error)
{
	guint8 secret[32] = { 0 }, public[32] = { 0 };
	size_t slen = sizeof(secret), plen = sizeof(public);
	EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
	gboolean generated =
		key && EVP_PKEY_get_raw_private_key(key, secret, &slen) == 1 &&
		EVP_PKEY_get_raw_public_key(key, public, &plen) == 1 &&
		slen == 32 && plen == 32;
	EVP_PKEY_free(key);
	gboolean ok = generated ?
			      pair_with_key(s, secret, public, error) :
			      fail(error, "Could not create a pairing key.");
	OPENSSL_cleanse(secret, sizeof(secret));
	return ok;
}
