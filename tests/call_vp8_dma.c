// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8_dma.h"
#include <errno.h>
#include <fcntl.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
	W = 128,
	H = 64
};

struct offer {
	struct line_dmabuf_layout layout;
	int dma, release;
};

static gboolean accept_frame(gboolean screen,
			     const struct line_camera_frame *frame,
			     const struct line_dmabuf_layout *layout, int dma,
			     int release, gpointer user)
{
	struct offer *o = user;

	g_assert_true(screen);
	g_assert_cmpuint(frame->width, ==, W);
	g_assert_cmpuint(layout->stride[0], >=, W);
	g_assert_cmpuint(layout->stride[1], >=, W);
	o->layout = *layout;
	o->dma = fcntl(dma, F_DUPFD_CLOEXEC, 0);
	o->release = fcntl(release, F_DUPFD_CLOEXEC, 0);
	g_assert_cmpint(o->dma, >=, 0);
	g_assert_cmpint(o->release, >=, 0);
	return TRUE;
}

static gboolean mock;
static gint unmapped;
static gboolean invalid_layout;

static void unmap(void *unused, uint8_t *bytes)
{
	(void)unused;
	AVDRMFrameDescriptor *drm = (void *)bytes;

	close(drm->objects[0].fd);
	av_free(drm);
	g_atomic_int_inc(&unmapped);
}

int __real_av_hwframe_map(AVFrame *, const AVFrame *, int);
int __wrap_av_hwframe_map(AVFrame *dst, const AVFrame *src, int flags)
{
	if (!mock)
		return __real_av_hwframe_map(dst, src, flags);
	g_assert_cmpint(dst->format, ==, AV_PIX_FMT_DRM_PRIME);
	g_assert_cmpint(flags, ==, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
	AVDRMFrameDescriptor *drm = av_mallocz(sizeof(*drm));
	int fd = memfd_create("synthetic-nv12", MFD_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(ftruncate(fd, W * H * 3 / 2), ==, 0);
	drm->nb_objects = 1;
	drm->objects[0] =
		(AVDRMObjectDescriptor){ .fd = fd, .size = W * H * 3 / 2 };
	drm->nb_layers = 2;
	drm->layers[0].format = MKTAG('R', '8', ' ', ' ');
	drm->layers[1].format = MKTAG('G', 'R', '8', '8');
	for (int i = 0; i < 2; i++) {
		drm->layers[i].nb_planes = 1;
		drm->layers[i].planes[0].pitch = W;
		drm->layers[i].planes[0].offset = i * W * H;
	}
	if (invalid_layout)
		drm->layers[1].planes[0].object_index = 1;
	dst->buf[0] =
		av_buffer_create((void *)drm, sizeof(*drm), unmap, NULL, 0);
	dst->data[0] = (void *)drm;
	return 0;
}

static AVFrame *cpu_picture(int width, int height)
{
	AVFrame *f = av_frame_alloc();

	f->format = AV_PIX_FMT_NV12;
	f->width = width;
	f->height = height;
	g_assert_cmpint(av_frame_get_buffer(f, 0), ==, 0);
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
			f->data[0][y * f->linesize[0] + x] = 32 + x;
	for (int y = 0; y < height / 2; y++)
		memset(f->data[1] + y * f->linesize[1], 128, width);
	return f;
}

static void check_export(AVFrame *hw)
{
	struct call_dma_pool *p = NULL;
	struct line_camera_frame desc = { .width = W,
					  .height = H,
					  .rotation = 90 };
	struct offer o = { .release = -1, .dma = -1 };
	int count = g_atomic_int_get(&unmapped);

	if (!mock &&
	    call_dma_submit(&p, hw, &desc, TRUE, accept_frame, &o) < 0) {
		g_test_skip(
			"VA driver did not export single-object modifier NV12");
		call_dma_retire(&p);
		return;
	}
	if (mock)
		g_assert_cmpint(call_dma_submit(&p, hw, &desc, TRUE,
						accept_frame, &o),
				==, 1);
	g_assert_cmpint(o.release, >=, 0);
	g_assert_cmpuint(o.layout.offset[0], <, o.layout.offset[1]);
	g_assert_cmpint(send(o.release, "R", 1, MSG_NOSIGNAL), ==, 1);
	struct pollfd rejected = { .fd = call_dma_reject_fd(p),
				   .events = POLLIN };
	g_assert_cmpint(poll(&rejected, 1, 1000), ==, 1);
	guint rotation = 0;
	AVFrame *last = call_dma_take_latest(p, &rotation);

	g_assert_nonnull(last);
	g_assert_cmpuint(rotation, ==, 90);
	if (!mock) {
		AVFrame *cpu = av_frame_alloc();

		g_assert_cmpint(av_hwframe_transfer_data(cpu, last, 0), ==, 0);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				g_assert_cmpuint(
					cpu->data[0][y * cpu->linesize[0] + x],
					==, 32 + x);
		av_frame_free(&cpu);
	}
	av_frame_free(&last);
	g_assert_null(call_dma_take_latest(p, &rotation));
	g_assert_false(call_dma_submit(&p, hw, &desc, TRUE, accept_frame, &o));
	call_dma_retire(&p);
	close(o.dma);
	g_assert_cmpint(g_atomic_int_get(&unmapped), ==, count);
	close(o.release);
	if (mock) {
		gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;

		while (g_atomic_int_get(&unmapped) == count &&
		       g_get_monotonic_time() < deadline)
			g_usleep(1000);
		g_assert_cmpint(g_atomic_int_get(&unmapped), ==, count + 1);
	}
}

static AVFrame *mock_surface(void)
{
	AVFrame *hw = cpu_picture(W, H);

	hw->format = AV_PIX_FMT_VAAPI;
	hw->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
	AVHWFramesContext *ctx = (void *)hw->hw_frames_ctx->data;

	ctx->width = W;
	ctx->height = H;
	ctx->sw_format = AV_PIX_FMT_NV12;
	return hw;
}

static void synthetic_layout(void)
{
	mock = TRUE;
	AVFrame *hw = mock_surface();

	check_export(hw);
	invalid_layout = TRUE;
	struct call_dma_pool *p = NULL;
	struct line_camera_frame f = { .width = W, .height = H };

	g_assert_cmpint(call_dma_submit(&p, hw, &f, TRUE, accept_frame, NULL),
			<, 0);
	g_assert_null(p);
	invalid_layout = FALSE;
	av_frame_free(&hw);
	mock = FALSE;
}

/* Closing a call cannot free surfaces still in use by a previous GPU
 * submission. Across generations, even a stalled consumer has a bound.
 */
static void retirement_budget(void)
{
	mock = TRUE;
	AVFrame *hw = mock_surface();
	struct call_dma_pool *p = NULL;
	struct offer leases[7];
	struct line_camera_frame f = { .width = W, .height = H };
	int count = g_atomic_int_get(&unmapped);

	for (guint i = 0; i < 6; i++) {
		g_assert_cmpint(call_dma_submit(&p, hw, &f, TRUE, accept_frame,
						&leases[i]),
				==, 1);
		if (i == 2 || i == 5) {
			g_assert_cmpint(call_dma_submit(&p, hw, &f, TRUE,
							accept_frame, NULL),
					==, 0);
			call_dma_retire(&p);
		}
	}
	g_assert_cmpint(call_dma_submit(&p, hw, &f, TRUE, accept_frame, NULL),
			==, 0);
	g_assert_null(p);
	g_assert_cmpint(g_atomic_int_get(&unmapped), ==, count);
	close(leases[3].dma);
	close(leases[3].release);
	gint64 until = g_get_monotonic_time() + G_USEC_PER_SEC;
	int result;

	do {
		result = call_dma_submit(&p, hw, &f, TRUE, accept_frame,
					 &leases[6]);
		if (!result)
			g_usleep(1000);
	} while (!result && g_get_monotonic_time() < until);
	g_assert_cmpint(result, ==, 1);
	call_dma_retire(&p);
	av_frame_free(&hw);
	for (guint i = 0; i < G_N_ELEMENTS(leases); i++) {
		if (i == 3)
			continue;
		close(leases[i].dma);
		close(leases[i].release);
	}
	until = g_get_monotonic_time() + G_USEC_PER_SEC;
	while (g_atomic_int_get(&unmapped) != count + 7 &&
	       g_get_monotonic_time() < until)
		g_usleep(1000);
	g_assert_cmpint(g_atomic_int_get(&unmapped), ==, count + 7);
	mock = FALSE;
}

static gboolean upload_picture(const char *node, int width, int height,
			       AVFrame **out)
{
	AVBufferRef *device = NULL;

	if (!node) {
		g_test_skip(
			"Set AVERNELLA_TEST_VAAPI_NODE to a VA-API render node");
		return FALSE;
	}
	g_assert_cmpint(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI,
					       node, NULL, 0),
			==, 0);
	AVBufferRef *frames = av_hwframe_ctx_alloc(device);
	AVHWFramesContext *ctx = (void *)frames->data;

	ctx->format = AV_PIX_FMT_VAAPI;
	ctx->sw_format = AV_PIX_FMT_NV12;
	ctx->width = width;
	ctx->height = height;
	g_assert_cmpint(av_hwframe_ctx_init(frames), ==, 0);
	AVFrame *cpu = cpu_picture(width, height), *hw = av_frame_alloc();

	g_assert_cmpint(av_hwframe_get_buffer(frames, hw, 0), ==, 0);
	g_assert_cmpint(av_hwframe_transfer_data(hw, cpu, 0), ==, 0);
	av_frame_free(&cpu);
	av_buffer_unref(&frames);
	av_buffer_unref(&device);
	*out = hw;
	return TRUE;
}

static void vaapi_upload(void)
{
	AVFrame *hw = NULL;

	if (upload_picture(g_getenv("AVERNELLA_TEST_VAAPI_NODE"), W, H, &hw)) {
		check_export(hw);
		av_frame_free(&hw);
	}
}

static gint time_order(gconstpointer a, gconstpointer b)
{
	gint64 l = *(const gint64 *)a, r = *(const gint64 *)b;

	return (l > r) - (l < r);
}

static gboolean consume_immediately(gboolean screen,
				    const struct line_camera_frame *f,
				    const struct line_dmabuf_layout *layout,
				    int dma, int release, gpointer unused)
{
	(void)screen;
	(void)f;
	(void)layout;
	(void)dma;
	(void)release;
	(void)unused;
	return TRUE;
}

static void producer_latency(void)
{
	enum {
		N = 300
	};
	struct line_camera_frame picture = { .width = 1280, .height = 720 };
	struct call_dma_pool *pool = NULL;
	gint64 copying[N], exporting[N];
	AVFrame *hw = NULL;

	if (!g_test_perf()) {
		g_test_skip(
			"Run with -m perf for the VA-API producer benchmark");
		return;
	}
	if (!upload_picture(g_getenv("AVERNELLA_TEST_VAAPI_NODE"),
			    picture.width, picture.height, &hw))
		return;
	AVFrame *cpu = av_frame_alloc();

	/* Alternate on the same resident surface; discard the first iteration.
	 * The DMA measurement includes mapping and registering the release fd.
	 * The readback baseline reuses its allocated NV12 destination.
	 */
	for (int i = -1; i < N; i++) {
		gint64 start = g_get_monotonic_time();

		g_assert_cmpint(av_hwframe_transfer_data(cpu, hw, 0), ==, 0);
		if (i >= 0)
			copying[i] = g_get_monotonic_time() - start;
		int result;

		do {
			start = g_get_monotonic_time();
			result = call_dma_submit(&pool, hw, &picture, FALSE,
						 consume_immediately, NULL);
			if (!result)
				g_usleep(100);
		} while (!result);
		if (result < 0) {
			g_test_skip(
				"VA driver did not export single-object modifier NV12");
			goto out;
		}
		if (i >= 0)
			exporting[i] = g_get_monotonic_time() - start;
	}
	qsort(copying, N, sizeof(copying[0]), time_order);
	qsort(exporting, N, sizeof(exporting[0]), time_order);
	g_test_message("720p VA readback p50/p95/p99=%" G_GINT64_FORMAT
		       "/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
		       " us; DMA export + lease=%" G_GINT64_FORMAT
		       "/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
		       " us (%d samples)",
		       copying[N / 2 - 1], copying[N * 95 / 100 - 1],
		       copying[N * 99 / 100 - 1], exporting[N / 2 - 1],
		       exporting[N * 95 / 100 - 1], exporting[N * 99 / 100 - 1],
		       N);
out:
	call_dma_retire(&pool);
	av_frame_free(&hw);
	av_frame_free(&cpu);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/dma/lease-and-rejection", synthetic_layout);
	g_test_add_func("/dma/retirement-budget", retirement_budget);
	g_test_add_func("/dma/vaapi-synthetic-upload", vaapi_upload);
	g_test_add_func("/dma/producer-latency", producer_latency);
	return g_test_run();
}
