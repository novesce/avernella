/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

char *line_obs_token(Line *s, GError **error);
char *line_obs_upload(const char *token, const char *sid, const char *oid,
		      const char *name, GBytes *encrypted, GError **error);
GBytes *line_obs_download(const char *token, const char *sid, const char *oid,
			  const char *message_id, GError **error);
GBytes *line_obs_download_preview(const char *token, const char *sid,
				  const char *oid, const char *message_id,
				  GError **error);
gboolean line_obs_upload_profile(const char *token, const char *mid,
				 GBytes *jpeg, GError **error);
GBytes *line_avatar_download(const char *path, GError **e);

GBytes *line_public_asset(const char *url, gsize limit, GError **error);
void line_obs_http_close(void);
