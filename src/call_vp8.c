// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8.h"
#include "codec_affinity.h"
#include "line.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include "call_hwdecode_config.h"
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#if CALL_HWDECODE
#include "call_vp8_device.h"
#include "call_vp8_dma.h"
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#endif

/* Two slots cover the picture being decoded/displayed and one arrival
 * during that service interval. More only adds queued latency.
 */
struct decode_job {
	guint8 *bytes;
	gsize size, capacity;
	guint rotation, tag;
	guint64 generation;
	gboolean key;
};

struct decode_worker {
	struct decode_job job[2];
	struct call_vp8_output out;
	GThread *thread;
	GMutex lock;
	GCond cond;
	guint head, count;
	gboolean done, stop;
	int wake;
};

struct call_vp8_decoder {
	struct decode_worker *worker;
	gboolean allow_worker;
	guint64 generation;
	vpx_codec_ctx_t software;
	guint8 *converted;
	gsize capacity;
	const char *backend;
	gboolean presentation;
#if CALL_HWDECODE
	gboolean (*visible)(gpointer);
	call_dma_send_fn send;
	gpointer sink;
	struct call_dma_pool *leases;
	call_video_display_fn display;
	gpointer display_data;
	gboolean disable_export;
	struct AVCodecContext *codec;
	AVBufferRef *device;
	AVFrame *surface, *download;
	AVPacket *packet;
	enum AVPixelFormat format;
	guint width, height;
	gboolean tried, retry_resize;
#endif
};

struct call_vp8_decoder *call_vp8_open(guint threads, gboolean presentation)
{
	struct call_vp8_decoder *d = g_new0(struct call_vp8_decoder, 1);
	vpx_codec_dec_cfg_t config = { .threads = threads };

	cpu_set_t saved;
	gboolean local = line_codec_local_cpus(&saved, threads);
	vpx_codec_err_t result = vpx_codec_dec_init(
		&d->software, vpx_codec_vp8_dx(), &config, 0);
	if (local)
		(void)sched_setaffinity(0, sizeof(saved), &saved);
	if (result != VPX_CODEC_OK) {
		g_free(d);
		return NULL;
	}
	d->presentation = presentation;
	return d;
}

const char *call_vp8_backend(const struct call_vp8_decoder *d)
{
	return d && d->backend ? d->backend : "pending";
}

static void decoded_with(struct call_vp8_decoder *d, const char *backend)
{
	if (d->backend == backend)
		return;
	d->backend = backend;
	g_debug("Call VP8 decoder displayed a frame using %s", backend);
}

static guint8 limited_chroma(int sample)
{
	int value = sample - 128;
	int delta = (224 * ABS(value) + 127) / 255;

	return value >= 0 ? 128 + delta : 128 - delta;
}

/*
 * The call renderer expects limited-range I420. Borrow compatible planes
 * during the display callback and reuse scratch space for converted planes.
 */
static gboolean display_frame(struct call_vp8_decoder *d,
			      struct line_camera_frame frame, gboolean nv12,
			      gboolean full, call_video_display_fn display,
			      gpointer data)
{
	guint w = frame.width, h = frame.height;
	guint cw = (w + 1) / 2, ch = (h + 1) / 2;
	gsize chroma = (gsize)cw * ch;

	if (!line_preview_size_valid(w, h, d->presentation) ||
	    !frame.plane[0] || !frame.plane[1] || frame.stride[0] < (int)w ||
	    frame.stride[1] < (int)(nv12 ? 2 * cw : cw) ||
	    (!nv12 && (!frame.plane[2] || frame.stride[2] < (int)cw)))
		return FALSE;
	if (nv12 || full) {
		gsize luma = full ? (gsize)w * h : 0;
		gsize needed = luma + 2 * chroma;

		if (needed > d->capacity) {
			d->converted = g_realloc(d->converted, needed);
			d->capacity = needed;
		}
		if (full) {
			for (guint y = 0; y < h; y++) {
				const guint8 *row = frame.plane[0] +
						    (gsize)y * frame.stride[0];
				guint8 *out = d->converted + (gsize)y * w;

				for (guint x = 0; x < w; x++)
					out[x] =
						16 + (219 * row[x] + 127) / 255;
			}
			frame.plane[0] = d->converted;
			frame.stride[0] = w;
		}
		for (guint i = 1; i <= 2; i++) {
			guint8 *out = d->converted + luma + (i - 1) * chroma;
			guint plane = nv12 ? 1 : i;
			const guint8 *src = frame.plane[plane];
			int stride = frame.stride[plane];

			for (guint y = 0; y < ch; y++) {
				for (guint x = 0; x < cw; x++) {
					guint v = src[nv12 ? 2 * x + i - 1 : x];

					out[y * cw + x] =
						full ? limited_chroma(v) : v;
				}
				src += stride;
			}
		}
		frame.plane[1] = d->converted + luma;
		frame.plane[2] = frame.plane[1] + chroma;
		frame.stride[1] = frame.stride[2] = cw;
	}
	display(&frame, data);
	return TRUE;
}

#if CALL_HWDECODE
static enum AVPixelFormat hardware_format(AVCodecContext *ctx,
					  const enum AVPixelFormat *formats)
{
	struct call_vp8_decoder *d = ctx->opaque;

	for (; *formats != AV_PIX_FMT_NONE; formats++)
		if (*formats == d->format)
			return *formats;
	return AV_PIX_FMT_NONE;
}

static void close_hardware(struct call_vp8_decoder *d)
{
	call_dma_retire(&d->leases);
	d->disable_export = FALSE;
	av_packet_free(&d->packet);
	av_frame_free(&d->download);
	av_frame_free(&d->surface);
	avcodec_free_context(&d->codec);
}

struct decoder_device {
	struct call_vp8_decoder *decoder;
	const AVCodec *codec;
};

static gboolean open_device(const char *node, guint vendor, const char *pci,
			    gpointer data)
{
	const struct decoder_device *probe = data;
	struct call_vp8_decoder *d = probe->decoder;
	const AVCodec *codec = probe->codec;
	enum AVHWDeviceType type;
	AVDictionary *options = NULL;
	char ordinal[16];
	const AVCodecHWConfig *config;
	int index = 0, error;

	switch (vendor) {
	case 0x10de:
		type = AV_HWDEVICE_TYPE_CUDA;
		d->format = AV_PIX_FMT_CUDA;
		break;
	case 0x1002:
	case 0x8086:
		type = AV_HWDEVICE_TYPE_VAAPI;
		d->format = AV_PIX_FMT_VAAPI;
		break;
	default:
		return FALSE;
	}
	while ((config = avcodec_get_hw_config(codec, index++))) {
		if (config->device_type == type &&
		    config->pix_fmt == d->format &&
		    (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
			break;
	}
	if (!config)
		return FALSE;
	if (type == AV_HWDEVICE_TYPE_CUDA) {
		if (!call_vp8_cuda_ordinal(pci, ordinal))
			return FALSE;
		node = ordinal;
		av_dict_set(&options, "primary_ctx", "1", 0);
	}
	error = av_hwdevice_ctx_create(&d->device, type, node, options, 0);
	av_dict_free(&options);
	return error >= 0;
}

static void open_hardware(struct call_vp8_decoder *d, guint width, guint height)
{
	const AVCodec *codec = avcodec_find_decoder_by_name("vp8");
	struct decoder_device probe = { d, codec };

	if (!codec || !g_strcmp0(g_getenv("LINE_DISABLE_GPU"), "1") ||
	    (!d->device &&
	     !call_vp8_try_render_nodes(FALSE, open_device, &probe)))
		return;
	d->codec = avcodec_alloc_context3(codec);
	if (!d->codec) {
		av_buffer_unref(&d->device);
		return;
	}
	d->codec->hw_device_ctx = av_buffer_ref(d->device);
	d->codec->thread_count = 1;
	d->codec->thread_type = 0;
	d->codec->apply_cropping = 0;
	/* UI may hold three mapped surfaces and a static fallback. */
	if (d->send && d->format == AV_PIX_FMT_VAAPI)
		d->codec->extra_hw_frames = 4;
	/* Drivers can align a presentation surface beyond its visible edges. */
	d->codec->max_pixels =
		d->presentation ?
			((width + 63u) & ~63u) * ((height + 63u) & ~63u) :
			1920 * 1088;
	d->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	d->codec->opaque = d;
	d->codec->get_format = hardware_format;
	d->codec->width = d->width = width;
	d->codec->height = d->height = height;
	d->surface = av_frame_alloc();
	d->download = av_frame_alloc();
	d->packet = av_packet_alloc();
	if (!d->codec->hw_device_ctx || !d->surface || !d->download ||
	    !d->packet || avcodec_open2(d->codec, codec, NULL) < 0) {
		close_hardware(d);
		av_buffer_unref(&d->device);
	}
}

static gboolean display_surface(struct call_vp8_decoder *d, AVFrame *hw,
				guint rotation, call_video_display_fn display,
				gpointer data)
{
	AVFrame *cpu = d->download;
	struct line_camera_frame frame = { .width = d->width,
					   .height = d->height,
					   .rotation = rotation };

	if (hw->format != d->format || !hw->hw_frames_ctx ||
	    hw->width < (int)d->width || hw->height < (int)d->height)
		return FALSE;
	/*
	 * A VP8 keyframe gives the visible size. NVDEC can return an even
	 * surface for odd-sized pictures without marking the padded edges.
	 */
	gsize spare_w = hw->width - d->width, spare_h = hw->height - d->height;

	/*
	 * Some NVDEC drivers round odd VP8 dimensions and offset the chroma
	 * rows in the downloaded image without reporting any crop. Decoding
	 * this keyframe with libvpx preserves the original edge samples.
	 */
	if (d->format == AV_PIX_FMT_CUDA &&
	    (((d->width & 1) && spare_w && !hw->crop_right) ||
	     ((d->height & 1) && spare_h && !hw->crop_bottom))) {
		d->retry_resize = TRUE;
		return FALSE;
	}
	if (hw->crop_left > spare_w ||
	    hw->crop_right > spare_w - hw->crop_left ||
	    hw->crop_top > spare_h ||
	    hw->crop_bottom > spare_h - hw->crop_top ||
	    ((hw->crop_left | hw->crop_top) & 1))
		return FALSE;
	if (d->visible && !d->visible(d->sink)) {
		/* Do update reception state, without exporting or reading back. */
		display(&frame, data);
		call_dma_forget_latest(d->leases);
		return TRUE;
	}
	if (d->format == AV_PIX_FMT_VAAPI && d->send && !d->disable_export &&
	    !call_dma_rejected(d->leases)) {
		int result = call_dma_submit(&d->leases, hw, &frame,
					     d->presentation, d->send, d->sink);

		if (result > 0) {
			d->display = display;
			d->display_data = data;
			display(&frame, data);
			return TRUE;
		}
		d->disable_export = result < 0;
	}
	if (cpu->width != hw->width || cpu->height != hw->height)
		av_frame_unref(cpu);
	if (av_hwframe_transfer_data(cpu, hw, 0) < 0 ||
	    cpu->width < hw->width || cpu->height < hw->height ||
	    (cpu->format != AV_PIX_FMT_NV12 &&
	     cpu->format != AV_PIX_FMT_YUV420P))
		return FALSE;
	gboolean nv12 = cpu->format == AV_PIX_FMT_NV12;

	for (int i = 0; i < (nv12 ? 2 : 3); i++) {
		gsize x = hw->crop_left >> !!i, y = hw->crop_top >> !!i;
		gsize samples = (d->width + !!i) >> !!i;
		gsize step = nv12 && i ? 2 : 1;

		if (!cpu->data[i] || cpu->linesize[i] < 0 ||
		    (x + samples) * step > (gsize)cpu->linesize[i])
			return FALSE;
		frame.plane[i] = cpu->data[i] + y * cpu->linesize[i] + x * step;
		frame.stride[i] = cpu->linesize[i];
	}
	return display_frame(d, frame, nv12,
			     hw->color_range == AVCOL_RANGE_JPEG, display,
			     data);
}

static int decode_hardware(struct call_vp8_decoder *d, const guint8 *packet,
			   gsize size, guint rotation,
			   call_video_display_fn display, gpointer data)
{
	int result, shown = 0;

	if (size > G_MAXINT || av_new_packet(d->packet, size) < 0)
		return -1;
	/* The depacketizer's borrowed buffer lacks libavcodec's tail padding. */
	memcpy(d->packet->data, packet, size);
	result = avcodec_send_packet(d->codec, d->packet);
	av_packet_unref(d->packet);
	if (result < 0)
		return -1;
	while ((result = avcodec_receive_frame(d->codec, d->surface)) >= 0) {
		/* A newer picture supersedes any rejected static preview. */
		call_dma_forget_latest(d->leases);
		gboolean valid =
			display_surface(d, d->surface, rotation, display, data);

		av_frame_unref(d->surface);
		if (!valid)
			return -1;
		shown++;
	}
	if (shown)
		decoded_with(d,
			     d->format == AV_PIX_FMT_CUDA ? "nvdec" : "vaapi");
	return result == AVERROR(EAGAIN) ? shown : -1;
}
#endif

void call_vp8_set_sink(struct call_vp8_decoder *d,
		       gboolean (*visible)(gpointer), call_dma_send_fn send,
		       gpointer data)
{
#if CALL_HWDECODE
	if (d && !g_strcmp0(g_getenv("AVERNELLA_MEDIA_DMABUF"), "1")) {
		d->visible = visible;
		d->send = send;
		d->sink = data;
	}
#else
	(void)d;
	(void)visible;
	(void)send;
	(void)data;
#endif
}

int call_vp8_release_fd(struct call_vp8_decoder *d)
{
#if CALL_HWDECODE
	return d && !d->disable_export ? call_dma_reject_fd(d->leases) : -1;
#else
	(void)d;
	return -1;
#endif
}

void call_vp8_handle_rejection(struct call_vp8_decoder *d)
{
#if CALL_HWDECODE
	if (!d || !call_dma_rejected(d->leases) || d->disable_export)
		return;
	d->disable_export = TRUE;
	guint rotation;
	AVFrame *latest = call_dma_take_latest(d->leases, &rotation);

	if (latest) {
		display_surface(d, latest, rotation, d->display,
				d->display_data);
		av_frame_free(&latest);
	}
#else
	(void)d;
#endif
}

static int decode_software(struct call_vp8_decoder *d, const guint8 *packet,
			   gsize size, guint rotation,
			   call_video_display_fn display, gpointer data)
{
	vpx_codec_iter_t iter = NULL;
	vpx_image_t *image;
	int shown = 0;

	if (vpx_codec_decode(&d->software, packet, size, NULL, VPX_DL_REALTIME))
		return -1;
	while ((image = vpx_codec_get_frame(&d->software, &iter))) {
		struct line_camera_frame frame = { .width = image->d_w,
						   .height = image->d_h,
						   .rotation = rotation };

		for (int i = 0; i < 3; i++) {
			frame.plane[i] = image->planes[i];
			frame.stride[i] = image->stride[i];
		}
		if (image->fmt != VPX_IMG_FMT_I420 ||
		    !display_frame(d, frame, FALSE,
				   image->range == VPX_CR_FULL_RANGE, display,
				   data))
			return -1;
		shown++;
	}
	return shown;
}

static void save_output(const struct line_camera_frame *image, gpointer data)
{
	struct call_vp8_output *out = data;

	out->image = *image;
}

static gpointer decode_main(gpointer data)
{
	struct call_vp8_decoder *d = data;
	struct decode_worker *t = d->worker;
	gboolean need_key = FALSE;

	line_tune_thread(LINE_THREAD_MEDIA);
	g_mutex_lock(&t->lock);
	while (!t->stop) {
		if (t->done || !t->count) {
			g_cond_wait(&t->cond, &t->lock);
			continue;
		}
		const struct decode_job *job = &t->job[t->head];

		g_mutex_unlock(&t->lock);

		t->out.tag = job->tag;
		t->out.shown = need_key && !job->key ?
				       -1 :
				       decode_software(d, job->bytes, job->size,
						       job->rotation,
						       save_output, &t->out);
		need_key = t->out.shown < 0;
		g_mutex_lock(&t->lock);
		t->done = TRUE;
		while (eventfd_write(t->wake, 1) < 0 && errno == EINTR)
			;
	}
	g_mutex_unlock(&t->lock);
	return NULL;
}

void call_vp8_enable_worker(struct call_vp8_decoder *d)
{
	if (d)
		d->allow_worker =
			g_strcmp0(g_getenv("LINE_CALL_DECODER_THREAD"), "0") !=
			0;
}

static void start_worker(struct call_vp8_decoder *d)
{
	struct decode_worker *t = g_new0(struct decode_worker, 1);

	d->allow_worker = FALSE;
	t->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (t->wake >= 0) {
		g_mutex_init(&t->lock);
		g_cond_init(&t->cond);
		d->worker = t;
		t->thread =
			g_thread_try_new("line-decode", decode_main, d, NULL);
		if (t->thread)
			return;
		d->worker = NULL;
		g_mutex_clear(&t->lock);
		g_cond_clear(&t->cond);
		close(t->wake);
	}
	g_free(t);
}

int call_vp8_worker_fd(struct call_vp8_decoder *d)
{
	return d && d->worker ? d->worker->wake : -1;
}

void call_vp8_discontinuity(struct call_vp8_decoder *d)
{
	if (d)
		d->generation++;
}

enum call_vp8_submit call_vp8_submit(struct call_vp8_decoder *d,
				     const guint8 *p, gsize n, gboolean key,
				     guint rotation, guint tag)
{
	if (!d->worker) {
		if (!d->allow_worker || g_strcmp0(d->backend, "libvpx"))
			return CALL_VP8_INLINE;
#if CALL_HWDECODE
		/* A later keyframe may reopen hardware for another size. */
		if (d->codec || !d->tried || d->retry_resize)
			return CALL_VP8_INLINE;
#endif
		start_worker(d);
		if (!d->worker)
			return CALL_VP8_INLINE;
	}
	struct decode_worker *t = d->worker;

	g_mutex_lock(&t->lock);
	if (t->count == G_N_ELEMENTS(t->job)) {
		if (!key) {
			g_mutex_unlock(&t->lock);
			return CALL_VP8_FULL;
		}
		/* A fresh key can replace the pending delta or an older key. */
		t->count--;
	}
	struct decode_job *job = &t->job[(t->head + t->count) % 2];

	if (n > job->capacity) {
		job->bytes = g_realloc(job->bytes, n);
		job->capacity = n;
	}
	memcpy(job->bytes, p, n);
	job->size = n;
	job->key = key;
	job->tag = tag;
	job->rotation = rotation;
	job->generation = d->generation;
	t->count++;
	g_cond_signal(&t->cond);
	g_mutex_unlock(&t->lock);
	return CALL_VP8_QUEUED;
}

void call_vp8_release(struct call_vp8_decoder *d)
{
	struct decode_worker *t = d->worker;
	eventfd_t count;

	g_mutex_lock(&t->lock);
	while (eventfd_read(t->wake, &count) < 0 && errno == EINTR)
		;
	t->done = FALSE;
	t->head ^= 1;
	t->count--;
	g_cond_signal(&t->cond);
	g_mutex_unlock(&t->lock);
}

gboolean call_vp8_take(struct call_vp8_decoder *d, struct call_vp8_output *out)
{
	struct decode_worker *t = d ? d->worker : NULL;

	if (!t)
		return FALSE;
	g_mutex_lock(&t->lock);
	gboolean done = t->done;
	gboolean current = done && t->job[t->head].generation == d->generation;

	/*
	 * A ready key restores references even if the preceding delta failed.
	 * Keep successful output in sequence, including consecutive keys.
	 */
	if (current && t->out.shown < 0 && t->count == 2 &&
	    t->job[t->head ^ 1].key)
		current = FALSE;
	if (current)
		*out = t->out;
	g_mutex_unlock(&t->lock);
	if (done && !current)
		call_vp8_release(d);
	return current;
}

static void stop_worker(struct decode_worker *t)
{
	if (!t)
		return;
	g_mutex_lock(&t->lock);
	t->stop = TRUE;
	g_cond_signal(&t->cond);
	g_mutex_unlock(&t->lock);
	g_thread_join(t->thread);
	for (guint i = 0; i < G_N_ELEMENTS(t->job); i++)
		g_free(t->job[i].bytes);
	close(t->wake);
	g_mutex_clear(&t->lock);
	g_cond_clear(&t->cond);
	g_free(t);
}

int call_vp8_decode(struct call_vp8_decoder *d, const guint8 *packet,
		    gsize size, gboolean key, guint rotation,
		    call_video_display_fn display, gpointer data)
{
#if CALL_HWDECODE
	if (key && size >= 10 && (!d->tried || d->codec || d->retry_resize)) {
		guint width = (packet[6] | packet[7] << 8) & 0x3fff;
		guint height = (packet[8] | packet[9] << 8) & 0x3fff;

		/* Hardware surfaces can retain their previous allocation on resize. */
		if (!d->tried || d->width != width || d->height != height) {
			d->tried = TRUE;
			d->retry_resize = FALSE;
			close_hardware(d);
			open_hardware(d, width, height);
		}
	}
	if (d->codec) {
		int count = decode_hardware(d, packet, size, rotation, display,
					    data);

		if (count >= 0)
			return count;
		close_hardware(d);
		av_buffer_unref(&d->device);
		/* libvpx has no reference pictures from the failed decoder. */
		if (!key)
			return -1;
	}
#else
	(void)key;
#endif
	int shown = decode_software(d, packet, size, rotation, display, data);

	if (shown > 0)
		decoded_with(d, "libvpx");
	return shown;
}

void call_vp8_close(struct call_vp8_decoder *d)
{
	if (!d)
		return;
	stop_worker(d->worker);
#if CALL_HWDECODE
	close_hardware(d);
	av_buffer_unref(&d->device);
#endif
	vpx_codec_destroy(&d->software);
	g_free(d->converted);
	g_free(d);
}
