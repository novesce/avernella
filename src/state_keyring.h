/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

enum {
	LINE_KEYRING_KEY_BYTES = 32
};

/*
 * Obtain this profile's wrapping key before the worker starts threads or
 * applies its sandbox. The key is cleared on failure.
 */
gboolean line_keyring_key(guint8 key[LINE_KEYRING_KEY_BYTES]);
