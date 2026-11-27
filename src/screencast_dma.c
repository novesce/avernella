// SPDX-License-Identifier: GPL-3.0-or-later
#include "screencast_dma.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* A codec may retain an imported surface after a portal session closes.
 * The pool outlives the stream and avoids returning removed pw_buffers.
 */
struct cast_dma_pool {
	gint refs, rejected;
	GMutex lock;
	struct pw_stream *stream;
	struct pw_loop *loop;
	struct spa_source *renegotiate;
};
struct capture_lease {
	struct line_capture_dma dma;
	struct cast_dma_pool *pool;
	struct pw_buffer *buffer;
};

static void pool_unref(struct cast_dma_pool *pool)
{
	if (pool && g_atomic_int_dec_and_test(&pool->refs)) {
		g_mutex_clear(&pool->lock);
		g_free(pool);
	}
}

void cast_dma_reject(gpointer data)
{
	struct cast_dma_pool *pool = data;

	if (!g_atomic_int_compare_and_exchange(&pool->rejected, FALSE, TRUE))
		return;
	g_mutex_lock(&pool->lock);
	if (pool->stream && pool->renegotiate)
		pw_loop_signal_event(pool->loop, pool->renegotiate);
	g_mutex_unlock(&pool->lock);
}

static void release_capture(gpointer data)
{
	struct capture_lease *lease = data;
	struct cast_dma_pool *pool = lease->pool;

	g_mutex_lock(&pool->lock);
	if (pool->stream && lease->buffer) {
		lease->buffer->user_data = NULL;
		pw_stream_queue_buffer(pool->stream, lease->buffer);
	}
	g_mutex_unlock(&pool->lock);
	close(lease->dma.fd);
	pool_unref(pool);
	g_free(lease);
}

struct cast_dma_pool *cast_dma_pool_new(struct pw_stream *stream,
					struct pw_loop *loop,
					spa_source_event_func_t renegotiate,
					void *data)
{
	struct cast_dma_pool *pool = g_new0(struct cast_dma_pool, 1);

	pool->refs = 1;
	g_mutex_init(&pool->lock);
	pool->stream = stream;
	pool->loop = loop;
	pool->renegotiate = pw_loop_add_event(loop, renegotiate, data);
	if (!pool->renegotiate) {
		pool_unref(pool);
		return NULL;
	}
	return pool;
}

/* The owning PipeWire thread has stopped. External references may remain. */
void cast_dma_pool_stop(struct cast_dma_pool *pool)
{
	g_mutex_lock(&pool->lock);
	pool->stream = NULL;
	pw_loop_destroy_source(pool->loop, pool->renegotiate);
	g_mutex_unlock(&pool->lock);
	pool_unref(pool);
}

gboolean cast_dma_rejected(struct cast_dma_pool *pool)
{
	return !pool || g_atomic_int_get(&pool->rejected);
}

/* A single DMA object may be described by two independent fd numbers.
 * Offsets are relative to each SPA mapping origin. Implicit-sync buffers
 * alone are negotiated; an explicit-sync producer stays on the SHM path.
 */
GBytes *cast_dma_take(struct cast_dma_pool *pool, struct pw_buffer *buffer,
		      const struct spa_video_info_raw *format, guint max_width,
		      guint max_height, guint64 modifier)
{
	struct spa_buffer *buf = buffer->buffer;
	struct capture_lease lease = { .pool = pool, .buffer = buffer };
	struct stat first = { 0 };
	guint width = format->size.width, height = format->size.height;

	if (!pool || buffer->user_data || buf->n_datas != 2 || !buf->datas ||
	    format->format != SPA_VIDEO_FORMAT_NV12 ||
	    !line_camera_frame_size_valid(width, height) ||
	    modifier == ((G_GUINT64_CONSTANT(1) << 56) - 1) ||
	    g_atomic_int_get(&pool->rejected) ||
	    !(format->flags & SPA_VIDEO_FLAG_MODIFIER) ||
	    format->modifier != modifier || width != max_width ||
	    height != max_height || ((width | height) & 1) ||
	    format->color_range != SPA_VIDEO_COLOR_RANGE_16_235 ||
	    format->color_matrix != SPA_VIDEO_COLOR_MATRIX_BT601)
		return NULL;
#if PW_CHECK_VERSION(1, 2, 0)
	if (spa_buffer_find_meta(buf, SPA_META_SyncTimeline))
		return NULL;
#endif
	for (guint i = 0; i < 2; i++) {
		const struct spa_data *d = &buf->datas[i];
		const struct spa_chunk *chunk = d->chunk;
		struct stat st;

		if (d->type != SPA_DATA_DmaBuf || d->fd < 0 ||
		    d->fd > G_MAXINT || !chunk || chunk->stride < (int)width ||
		    fstat(d->fd, &st) || st.st_size <= 0 ||
		    (i &&
		     (first.st_dev != st.st_dev || first.st_ino != st.st_ino)))
			return NULL;
		guint64 offset = (guint64)d->mapoffset + chunk->offset;

		if (offset > G_MAXUINT32 ||
		    !line_capture_plane_valid(st.st_size, offset, chunk->stride,
					      width, height >> i, modifier))
			return NULL;
		lease.dma.offset[i] = offset;
		lease.dma.stride[i] = chunk->stride;
		first = st;
	}
	if (lease.dma.offset[0] == lease.dma.offset[1])
		return NULL;
	lease.dma.fd = fcntl(buf->datas[0].fd, F_DUPFD_CLOEXEC, 3);
	if (lease.dma.fd < 0)
		return NULL;
	lease.dma.size = first.st_size;
	lease.dma.modifier = modifier;
	lease.dma.owner = pool;
	lease.dma.reject = cast_dma_reject;
	struct capture_lease *owner = g_memdup2(&lease, sizeof(lease));

	g_atomic_int_inc(&pool->refs);
	buffer->user_data = owner;
	return g_bytes_new_with_free_func(&owner->dma, sizeof(owner->dma),
					  release_capture, owner);
}

void cast_dma_remove(struct cast_dma_pool *pool, struct pw_buffer *buffer)
{
	if (!pool)
		return;
	g_mutex_lock(&pool->lock);
	struct capture_lease *lease = buffer->user_data;

	if (lease) {
		lease->buffer = NULL;
		buffer->user_data = NULL;
	}
	g_mutex_unlock(&pool->lock);
}
