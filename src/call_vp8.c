// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8.h"
#include "call_hwdecode_config.h"
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#if CALL_HWDECODE
#include "call_vp8_device.h"
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#endif

struct call_vp8_decoder {
	vpx_codec_ctx_t software;
	guint8 *converted;
	gsize capacity;
	const char *backend;
	gboolean presentation;
#if CALL_HWDECODE
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

	if (vpx_codec_dec_init(&d->software, vpx_codec_vp8_dx(), &config, 0)) {
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

static gboolean display_surface(struct call_vp8_decoder *d, guint rotation,
				call_video_display_fn display, gpointer data)
{
	AVFrame *hw = d->surface, *cpu = d->download;
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
		gboolean valid = display_surface(d, rotation, display, data);

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

int call_vp8_decode(struct call_vp8_decoder *d, const guint8 *packet,
		    gsize size, gboolean key, guint rotation,
		    call_video_display_fn display, gpointer data)
{
	vpx_codec_iter_t iter = NULL;
	vpx_image_t *image;
	int shown = 0;

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
	if (shown)
		decoded_with(d, "libvpx");
	return shown;
}

void call_vp8_close(struct call_vp8_decoder *d)
{
	if (!d)
		return;
#if CALL_HWDECODE
	close_hardware(d);
	av_buffer_unref(&d->device);
#endif
	vpx_codec_destroy(&d->software);
	g_free(d->converted);
	g_free(d);
}
