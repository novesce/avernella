/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

char *line_obs_token(Line *s, GError **error);
char *line_obs_upload(const char *token, const char *sid, const char *oid,
		      const char *name, GBytes *encrypted, GError **error);
GBytes *line_obs_download(const char *token, const char *sid, const char *oid,
			  const char *message_id, GError **error);
GBytes *line_obs_download_original(const char *token, const char *sid,
				   const char *oid, const char *message_id,
				   GError **error);
GBytes *line_obs_download_preview(const char *token, const char *sid,
				  const char *oid, const char *message_id,
				  GError **error);
gboolean line_obs_upload_profile(const char *token, const char *mid,
				 GBytes *jpeg, GError **error);
gboolean line_obs_upload_group_profile(const char *token, const char *chat_mid,
				       GBytes *jpeg, GError **error);
char *line_obs_upload_square(const char *token, const char *chat, gint32 seq,
			     const char *name, const char *media_type,
			     guint64 duration_ms, GBytes *plain,
			     GError **error);
GBytes *line_obs_download_square(const char *token, const char *message_id,
				 GError **error);
GBytes *line_obs_download_square_original(const char *token,
					  const char *message_id,
					  GError **error);
GBytes *line_obs_download_square_preview(const char *token,
					 const char *message_id,
					 GError **error);
char *line_obs_upload_square_profile(const char *token, const char *scope,
				     const char *oid, GBytes *jpeg,
				     GError **error);
GBytes *line_avatar_download(const char *path, GError **e);

GBytes *line_public_asset(const char *url, gsize limit, GError **error);
void line_obs_http_close(void);
