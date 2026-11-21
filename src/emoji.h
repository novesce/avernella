/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include <string.h>

/*
 * The contemporary sticon CDN uses a lowercase ObjectId and a zero-padded
 * decimal item number. Keep all untrusted identifiers out of path parsing.
 */
static inline gboolean line_emoji_id_valid(const char *product_id,
					   const char *emoji_id)
{
	if (!product_id || strlen(product_id) != 24 || !emoji_id ||
	    strlen(emoji_id) != 3)
		return FALSE;
	for (unsigned int i = 0; i < 24; i++)
		if (!g_ascii_isdigit(product_id[i]) &&
		    (product_id[i] < 'a' || product_id[i] > 'f'))
			return FALSE;
	for (unsigned int i = 0; i < 3; i++)
		if (!g_ascii_isdigit(emoji_id[i]))
			return FALSE;
	return strcmp(emoji_id, "000") != 0;
}

/* Owned flat array of {productId, emojiId, label, pack, version, resourceType}. */
J *line_emoji_catalog(Line *s, GError **error);
GBytes *line_emoji_image(const char *product_id, const char *emoji_id,
			 GError **error);
J *line_emoji_catalog_page(Line *s, guint offset, guint limit,
			   const char *query, GError **error);
GBytes *line_emoji_image_variant(const char *product_id, const char *emoji_id,
				 gboolean animated, GError **error);
