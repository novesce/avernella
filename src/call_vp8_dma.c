// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8_dma.h"
#include <errno.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <sys/epoll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

/* Two in flight and one awaiting presentation for each of camera and screen. */
enum {
	LEASES = 3,
	TOTAL_LEASES = 2 * LEASES
};
static gint total_leases;

struct call_dma_pool {
	int epoll, stop, rejected;
	gint count, failed;
	/* Only the producing call thread touches the static fallback. */
	AVFrame *latest;
	guint rotation;
};

struct surface_lease {
	AVFrame *mapped;
	int sock;
};

static gpointer reap_surfaces(gpointer arg)
{
	struct call_dma_pool *p = arg;
	gboolean retired = FALSE;

	while (!retired || g_atomic_int_get(&p->count)) {
		struct epoll_event event;

		if (epoll_wait(p->epoll, &event, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			g_usleep(G_USEC_PER_SEC / 10);
			continue;
		}
		struct surface_lease *lease = event.data.ptr;
		eventfd_t count;

		if (!lease) {
			(void)eventfd_read(p->stop, &count);
			retired = TRUE;
			continue;
		}
		char bytes[16];
		ssize_t n =
			recv(lease->sock, bytes, sizeof(bytes), MSG_DONTWAIT);

		if (n > 0) {
			if (memchr(bytes, 'R', n) &&
			    g_atomic_int_compare_and_exchange(&p->failed, 0,
							      1)) {
				while (eventfd_write(p->rejected, 1) < 0 &&
				       errno == EINTR)
					;
			}
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		/* The consumer has finished using the image on its GPU. */
		close(lease->sock);
		av_frame_free(&lease->mapped);
		g_free(lease);
		g_atomic_int_add(&p->count, -1);
		g_atomic_int_add(&total_leases, -1);
	}
	close(p->rejected);
	close(p->epoll);
	close(p->stop);
	g_free(p);
	return NULL;
}

static struct call_dma_pool *new_pool(void)
{
	struct call_dma_pool *p = g_new0(struct call_dma_pool, 1);
	struct epoll_event stop = { .events = EPOLLIN };

	p->epoll = epoll_create1(EPOLL_CLOEXEC);
	p->stop = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	p->rejected = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (p->epoll >= 0 && p->stop >= 0 && p->rejected >= 0 &&
	    epoll_ctl(p->epoll, EPOLL_CTL_ADD, p->stop, &stop) == 0) {
		GThread *thread = g_thread_try_new("line-dma-release",
						   reap_surfaces, p, NULL);
		if (thread) {
			g_thread_unref(thread);
			return p;
		}
	}
	if (p->epoll >= 0)
		close(p->epoll);
	if (p->stop >= 0)
		close(p->stop);
	if (p->rejected >= 0)
		close(p->rejected);
	g_free(p);
	return NULL;
}

int call_dma_reject_fd(const struct call_dma_pool *p)
{
	return p ? p->rejected : -1;
}

gboolean call_dma_rejected(const struct call_dma_pool *p)
{
	return p && g_atomic_int_get(&p->failed);
}

void call_dma_forget_latest(struct call_dma_pool *p)
{
	if (p)
		av_frame_free(&p->latest);
}

AVFrame *call_dma_take_latest(struct call_dma_pool *p, guint *rotation)
{
	if (!call_dma_rejected(p))
		return NULL;
	*rotation = p->rotation;
	AVFrame *last = p->latest;

	p->latest = NULL;
	return last;
}

void call_dma_retire(struct call_dma_pool **pool)
{
	struct call_dma_pool *p = *pool;
	*pool = NULL;
	if (p) {
		call_dma_forget_latest(p);
		while (eventfd_write(p->stop, 1) < 0 && errno == EINTR)
			;
	}
}

/* NV12 with interleaved Cb,Cr. FFmpeg's VA-API export usually gives two
 * single-plane layers, R8 and GR88, referencing the same DRM object.
 * An auxiliary compression plane or multi-object layout needs another
 * import contract. Leave it to the CPU readback path.
 */
static gboolean describe(AVFrame *hw, AVFrame *mapped,
			 const struct line_camera_frame *frame,
			 struct line_dmabuf_layout *layout, int *fd)
{
	const AVDRMFrameDescriptor *drm = (const void *)mapped->data[0];
	const AVHWFramesContext *ctx = (const void *)hw->hw_frames_ctx->data;
	const AVDRMPlaneDescriptor *planes[2];

	if (!drm || drm->nb_objects != 1 || ctx->sw_format != AV_PIX_FMT_NV12 ||
	    ctx->width <= 0 || ctx->height <= 0 ||
	    (ctx->width | ctx->height) & 1 || ctx->width > 4096 ||
	    ctx->height > 4096 || hw->width > ctx->width ||
	    hw->height > ctx->height || !frame->width || !frame->height ||
	    hw->crop_left >= (gsize)ctx->width ||
	    hw->crop_top >= (gsize)ctx->height ||
	    ((hw->crop_left | hw->crop_top) & 1) ||
	    frame->width > (gsize)ctx->width - hw->crop_left ||
	    frame->height > (gsize)ctx->height - hw->crop_top ||
	    (hw->chroma_location != AVCHROMA_LOC_UNSPECIFIED &&
	     hw->chroma_location != AVCHROMA_LOC_CENTER) ||
	    (hw->colorspace != AVCOL_SPC_UNSPECIFIED &&
	     hw->colorspace != AVCOL_SPC_BT470BG &&
	     hw->colorspace != AVCOL_SPC_SMPTE170M))
		return FALSE;
	const AVDRMObjectDescriptor *obj = &drm->objects[0];
	/* DRM_FORMAT_MOD_INVALID is vendor zero, low 56 bits all ones. */
	if (obj->fd < 0 || !obj->size ||
	    obj->format_modifier == (UINT64_MAX >> 8))
		return FALSE;
	if (drm->nb_layers == 1 && drm->layers[0].nb_planes == 2 &&
	    drm->layers[0].format == MKTAG('N', 'V', '1', '2')) {
		planes[0] = &drm->layers[0].planes[0];
		planes[1] = &drm->layers[0].planes[1];
	} else if (drm->nb_layers == 2 && drm->layers[0].nb_planes == 1 &&
		   drm->layers[1].nb_planes == 1 &&
		   drm->layers[0].format == MKTAG('R', '8', ' ', ' ') &&
		   drm->layers[1].format == MKTAG('G', 'R', '8', '8')) {
		planes[0] = &drm->layers[0].planes[0];
		planes[1] = &drm->layers[1].planes[0];
	} else
		return FALSE;
	*layout = (struct line_dmabuf_layout){
		.width = ctx->width,
		.height = ctx->height,
		.crop_x = hw->crop_left,
		.crop_y = hw->crop_top,
		.full_range = hw->color_range == AVCOL_RANGE_JPEG,
		.modifier = obj->format_modifier,
	};
	for (guint i = 0; i < 2; i++) {
		const AVDRMPlaneDescriptor *s = planes[i];
		guint rows = ctx->height >> i;

		if (s->object_index || s->offset < 0 || s->pitch < ctx->width ||
		    s->offset > G_MAXUINT32 || s->pitch > G_MAXUINT32 ||
		    (guint64)s->offset + (guint64)s->pitch * (rows - 1) +
				    ctx->width >
			    obj->size)
			return FALSE;
		layout->offset[i] = s->offset;
		layout->stride[i] = s->pitch;
	}
	*fd = obj->fd;
	return TRUE;
}

int call_dma_submit(struct call_dma_pool **pool, AVFrame *hw,
		    const struct line_camera_frame *frame,
		    gboolean presentation, call_dma_send_fn send, gpointer data)
{
	struct call_dma_pool *p = *pool;
	struct line_dmabuf_layout layout;
	struct epoll_event event = { .events = EPOLLIN };
	int pair[2], fd;

	if (hw->format != AV_PIX_FMT_VAAPI || !hw->hw_frames_ctx ||
	    call_dma_rejected(p))
		return FALSE;
	if (p && g_atomic_int_get(&p->count) >= LEASES)
		return 0;
	/* Retired decoders also consume this budget until the GPU releases
	 * their last images. Reserve before mapping another surface.
	 */
	gint live;

	do {
		live = g_atomic_int_get(&total_leases);
		if (live >= TOTAL_LEASES)
			return 0;
	} while (!g_atomic_int_compare_and_exchange(&total_leases, live,
						    live + 1));
	AVFrame *mapped = av_frame_alloc();

	if (!mapped)
		goto fail;
	mapped->format = AV_PIX_FMT_DRM_PRIME;
	/* READ synchronizes the VA producer; DIRECT excludes staging copies. */
	if (av_hwframe_map(mapped, hw,
			   AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0 ||
	    !describe(hw, mapped, frame, &layout, &fd))
		goto fail;
	if (!p)
		p = *pool = new_pool();
	if (!p ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
		       pair) < 0)
		goto fail;
	AVFrame *latest = av_frame_clone(hw);

	if (!latest) {
		close(pair[0]);
		close(pair[1]);
		goto fail;
	}
	struct surface_lease *lease = g_new(struct surface_lease, 1);
	*lease = (struct surface_lease){ .mapped = mapped, .sock = pair[0] };
	event.data.ptr = lease;
	if (epoll_ctl(p->epoll, EPOLL_CTL_ADD, pair[0], &event) < 0) {
		close(pair[0]);
		close(pair[1]);
		g_free(lease);
		av_frame_free(&latest);
		goto fail;
	}
	g_atomic_int_inc(&p->count);
	gboolean sent = send(presentation, frame, &layout, fd, pair[1], data);

	close(pair[1]);
	if (sent) {
		call_dma_forget_latest(p);
		p->latest = latest;
		p->rotation = frame->rotation;
	} else
		av_frame_free(&latest);
	return sent;
fail:
	av_frame_free(&mapped);
	g_atomic_int_add(&total_leases, -1);
	return -1;
}
