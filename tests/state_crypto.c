// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_crypto.h"
#include "line.h"
#include <string.h>

static const guint8 key[LINE_STATE_KEY_SIZE] = { 1 };
static const char record[] = "{\"access\":\"fixture\",\"seq\":32}";

static void round_trip(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree guint8 *file = NULL;
	gsize size = 0, other_size = 0, clear_size = 0;

	file = line_state_encrypt(key, record, sizeof(record) - 1, &size,
				  &error);
	g_assert_no_error(error);
	g_assert_nonnull(file);
	g_assert_cmpuint(size, ==,
			 sizeof(record) - 1 + LINE_STATE_CRYPTO_OVERHEAD);
	g_assert_true(line_state_is_encrypted(file, size));
	g_assert_false(line_state_is_encrypted(record, sizeof(record) - 1));
	g_assert_null(memmem(file, size, record, sizeof(record) - 1));

	/* Independent writes of the same record use fresh nonces. */
	g_autofree guint8 *other = line_state_encrypt(
		key, record, sizeof(record) - 1, &other_size, &error);

	g_assert_no_error(error);
	g_assert_nonnull(other);
	g_assert_cmpuint(other_size, ==, size);
	g_assert_cmpint(memcmp(file, other, size), !=, 0);

	g_autofree char *clear =
		line_state_decrypt(key, file, size, &clear_size, &error);

	g_assert_no_error(error);
	g_assert_nonnull(clear);
	g_assert_cmpuint(clear_size, ==, sizeof(record) - 1);
	g_assert_cmpmem(clear, clear_size + 1, record, sizeof(record));
	explicit_bzero(clear, clear_size);
}

static void rejects_tampering(void)
{
	g_autoptr(GError) error = NULL;
	gsize size, clear_size;
	g_autofree guint8 *file = line_state_encrypt(
		key, record, sizeof(record) - 1, &size, &error);

	g_assert_no_error(error);
	g_assert_nonnull(file);
	for (gsize i = 0; i < size; i++) {
		file[i] ^= 1;
		clear_size = size;
		g_assert_null(line_state_decrypt(key, file, size, &clear_size,
						 &error));
		g_assert_error(error, LINE_ERROR, 1);
		g_assert_cmpuint(clear_size, ==, 0);
		g_clear_error(&error);
		file[i] ^= 1;
	}
	guint8 wrong_key[LINE_STATE_KEY_SIZE] = { 2 };

	g_assert_null(line_state_decrypt(wrong_key, file, size, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_null(line_state_decrypt(key, file, size - 1, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
}

static void rejects_invalid_sizes(void)
{
	g_autoptr(GError) error = NULL;
	static const gsize length[] = { 0, LINE_STATE_MAX_BYTES + 1 };

	for (gsize i = 0; i < G_N_ELEMENTS(length); i++) {
		gsize written = 1;

		g_assert_null(line_state_encrypt(key, record, length[i],
						 &written, &error));
		g_assert_error(error, LINE_ERROR, 1);
		g_assert_cmpuint(written, ==, 0);
		g_clear_error(&error);
	}
	g_assert_null(line_state_decrypt(key, record, 0, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_null(line_state_decrypt(key, record, G_MAXSIZE, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/state-crypto/round-trip", round_trip);
	g_test_add_func("/state-crypto/tampering", rejects_tampering);
	g_test_add_func("/state-crypto/invalid-sizes", rejects_invalid_sizes);
	return g_test_run();
}
