/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "camera.h"
#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>

struct cast_dma_pool;
struct cast_dma_pool *cast_dma_pool_new(struct pw_stream *stream,
					struct pw_loop *loop,
					spa_source_event_func_t renegotiate,
					void *data);
void cast_dma_pool_stop(struct cast_dma_pool *pool);
gboolean cast_dma_rejected(struct cast_dma_pool *pool);
void cast_dma_reject(gpointer pool);
void cast_dma_remove(struct cast_dma_pool *pool, struct pw_buffer *buffer);
GBytes *cast_dma_take(struct cast_dma_pool *pool, struct pw_buffer *buffer,
		      const struct spa_video_info_raw *format, guint max_width,
		      guint max_height, guint64 modifier);
