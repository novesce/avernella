// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8_encode.h"
#include "call_hwdecode_config.h"
#include "call_jetson_config.h"
#if CALL_VP8_JETSON
#include "call_vp8_jetson.h"
#endif
#if CALL_HWDECODE
#include "call_vp8_device.h"
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#endif

#if CALL_HWDECODE || CALL_VP8_JETSON
struct call_vp8_encoder {
#if CALL_VP8_JETSON
	struct call_vp8_jetson *jetson;
#endif
#if CALL_HWDECODE
	AVCodecContext *codec;
	AVFrame *surface, *cpu;
	AVPacket *packet;
	gboolean direct_map;
#endif
};
#endif

#if CALL_HWDECODE
struct encoder_probe {
	guint width, height, kbps, fps, key_distance;
	struct call_vp8_encoder *encoder;
};
#endif

void call_vp8_encoder_close(struct call_vp8_encoder *e)
{
	if (!e)
		return;
#if CALL_VP8_JETSON
	if (e->jetson)
		call_vp8_jetson_close(e->jetson);
#endif
#if CALL_HWDECODE
	av_packet_free(&e->packet);
	av_frame_free(&e->surface);
	av_frame_free(&e->cpu);
	avcodec_free_context(&e->codec);
#endif
	g_free(e);
}

const char *call_vp8_encoder_name(const struct call_vp8_encoder *e
				  G_GNUC_UNUSED)
{
#if CALL_VP8_JETSON
	if (e && e->jetson)
		return "nvv4l2vp8enc";
#endif
#if CALL_HWDECODE
	if (e)
		return e->codec->codec->name;
#endif
	return NULL;
}

#if CALL_HWDECODE
static AVBufferRef *encoder_device(enum AVHWDeviceType type, const char *node,
				   guint vendor, const char *pci)
{
	AVBufferRef *device = NULL;
	char ordinal[16];

	switch (type) {
	case AV_HWDEVICE_TYPE_VAAPI:
		/* The NVIDIA VA-API bridge only decodes. Initialising it here
		 * also brings up a CUDA context on every camera reconfigure.
		 */
		if (vendor != 0x10de)
			av_hwdevice_ctx_create(&device, type, node, NULL, 0);
		break;
	case AV_HWDEVICE_TYPE_QSV: {
		AVBufferRef *vaapi = NULL;

		if (vendor == 0x8086 &&
		    av_hwdevice_ctx_create(&vaapi, AV_HWDEVICE_TYPE_VAAPI, node,
					   NULL, 0) >= 0)
			av_hwdevice_ctx_create_derived(&device, type, vaapi, 0);
		av_buffer_unref(&vaapi);
		break;
	}
	case AV_HWDEVICE_TYPE_CUDA:
		if (vendor == 0x10de && call_vp8_cuda_ordinal(pci, ordinal))
			av_hwdevice_ctx_create(&device, type, ordinal, NULL, 0);
		break;
	default:
		break;
	}
	return device;
}

static struct call_vp8_encoder *open_codec(const struct encoder_probe *params,
					   const AVCodec *codec,
					   const AVCodecHWConfig *config,
					   AVBufferRef *device)
{
	struct call_vp8_encoder *e = g_new0(struct call_vp8_encoder, 1);
	AVCodecContext *c = avcodec_alloc_context3(codec);
	AVHWFramesContext *pool;
	int ret, log_offset;

	e->codec = c;
	if (!c)
		goto fail;
	c->hw_frames_ctx = av_hwframe_ctx_alloc(device);
	if (!c->hw_frames_ctx)
		goto fail;
	pool = (AVHWFramesContext *)c->hw_frames_ctx->data;
	pool->format = config->pix_fmt;
	pool->sw_format = AV_PIX_FMT_NV12;
	pool->width = params->width;
	pool->height = params->height;
	if (config->device_type == AV_HWDEVICE_TYPE_QSV) {
		/* QSV requires an aligned, fixed-size input surface pool. */
		pool->width = FFALIGN(pool->width, 16);
		pool->height = FFALIGN(pool->height, 16);
		pool->initial_pool_size = 6;
	}
	if (av_hwframe_ctx_init(c->hw_frames_ctx) < 0)
		goto fail;
	c->pix_fmt = config->pix_fmt;
	c->width = params->width;
	c->height = params->height;
	c->bit_rate = (gint64)params->kbps * 1000;
	c->rc_max_rate = c->bit_rate;
	c->rc_buffer_size = c->bit_rate;
	c->framerate = (AVRational){ params->fps, 1 };
	c->time_base = (AVRational){ 1, 90000 };
	c->gop_size = params->key_distance;
	c->max_b_frames = 0;
	c->flags |= AV_CODEC_FLAG_LOW_DELAY;
	c->color_range = AVCOL_RANGE_MPEG;
	/* One submission must produce its own packet on this call tick. */
	if (c->priv_data &&
	    av_opt_find(c->priv_data, "async_depth", NULL, 0, 0))
		av_opt_set_int(c->priv_data, "async_depth", 1, 0);
	/* Missing codec profiles are expected when probing multiple GPUs.
	 * Keep this candidate's diagnostics available at debug verbosity. */
	log_offset = c->log_level_offset;
	if (config->device_type == AV_HWDEVICE_TYPE_VAAPI)
		c->log_level_offset += AV_LOG_DEBUG - AV_LOG_ERROR;
	ret = avcodec_open2(c, codec, NULL);
	c->log_level_offset = log_offset;
	if (ret < 0)
		goto fail;
	e->cpu = av_frame_alloc();
	e->surface = av_frame_alloc();
	e->packet = av_packet_alloc();
	if (!e->cpu || !e->surface || !e->packet)
		goto fail;
	e->direct_map = config->device_type == AV_HWDEVICE_TYPE_VAAPI;
	return e;
fail:
	call_vp8_encoder_close(e);
	return NULL;
}

static gboolean try_encoder_node(const char *node, guint vendor,
				 const char *pci, gpointer data)
{
	struct encoder_probe *params = data;
	const AVCodec *codec;
	void *iter = NULL;

	while ((codec = av_codec_iterate(&iter))) {
		const AVCodecHWConfig *config;

		if (codec->id != AV_CODEC_ID_VP8 || !av_codec_is_encoder(codec))
			continue;
		for (int i = 0; (config = avcodec_get_hw_config(codec, i));
		     i++) {
			if (!(config->methods &
			      AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX))
				continue;
			AVBufferRef *device = encoder_device(
				config->device_type, node, vendor, pci);
			if (!device)
				continue;
			params->encoder =
				open_codec(params, codec, config, device);
			av_buffer_unref(&device);
			if (params->encoder)
				return TRUE;
		}
	}
	return FALSE;
}

#endif

struct call_vp8_encoder *call_vp8_encoder_open(guint width, guint height,
					       guint kbps, guint fps,
					       guint key_distance)
{
	if (!line_camera_frame_size_valid(width, height) || !kbps ||
	    kbps > G_MAXINT / 1000 || !fps || fps > G_MAXINT || !key_distance ||
	    key_distance > G_MAXINT ||
	    !g_strcmp0(g_getenv("LINE_DISABLE_GPU"), "1"))
		return NULL;
#if CALL_HWDECODE
	struct encoder_probe params = { .width = width,
					.height = height,
					.kbps = kbps,
					.fps = fps,
					.key_distance = key_distance };

	if (call_vp8_try_render_nodes(TRUE, try_encoder_node, &params))
		return params.encoder;
#endif
#if CALL_VP8_JETSON
	struct call_vp8_jetson *jetson =
		call_vp8_jetson_open(width, height, kbps, fps, key_distance);
	if (jetson) {
		struct call_vp8_encoder *e = g_new0(struct call_vp8_encoder, 1);

		e->jetson = jetson;
		return e;
	}
#endif
	return NULL;
}

#if CALL_HWDECODE
/* Convert the incoming I420 directly into mapped NV12 where available.
 * Input planes can be padded independently, including either chroma plane.
 */
static void copy_nv12(AVFrame *dst, const struct line_camera_frame *src)
{
	guint cw = (src->width + 1) / 2, ch = (src->height + 1) / 2;

	for (guint y = 0; y < src->height; y++)
		memcpy(dst->data[0] + (gsize)y * dst->linesize[0],
		       src->plane[0] + (gsize)y * src->stride[0], src->width);
	for (guint y = 0; y < ch; y++) {
		guint8 *uv = dst->data[1] + (gsize)y * dst->linesize[1];
		const guint8 *u = src->plane[1] + (gsize)y * src->stride[1];
		const guint8 *v = src->plane[2] + (gsize)y * src->stride[2];

		for (guint x = 0; x < cw; x++) {
			uv[2 * x] = u[x];
			uv[2 * x + 1] = v[x];
		}
	}
}

/* FFmpeg retains the capture owner on any reference held by the VA driver. */
static void release_nv12(void *capture, guint8 *descriptor)
{
	g_bytes_unref(capture);
	av_free(descriptor);
}

gboolean call_capture_import_nv12(AVFrame *dst, AVBufferRef *frames_ctx,
				  const struct line_camera_frame *frame)
{
	GBytes *storage = frame->capture_dma;
	const struct line_capture_dma *d = g_bytes_get_data(storage, NULL);

	if (g_bytes_get_size(storage) != sizeof(*d) || d->fd < 0 || !d->size ||
	    d->size > G_MAXSIZE ||
	    !line_camera_frame_size_valid(frame->width, frame->height) ||
	    ((frame->width | frame->height) & 1) ||
	    d->modifier == (UINT64_MAX >> 8) || d->offset[0] == d->offset[1])
		return FALSE;
	AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
	AVFrame *src = av_frame_alloc();
	gboolean ok = FALSE;

	if (!src || !desc) {
		av_free(desc);
		goto out;
	}
	src->buf[0] = av_buffer_create((guint8 *)desc, sizeof(*desc),
				       release_nv12, storage,
				       AV_BUFFER_FLAG_READONLY);
	if (!src->buf[0]) {
		av_free(desc);
		goto out;
	}
	g_bytes_ref(storage);
	desc->nb_objects = desc->nb_layers = 1;
	desc->objects[0] = (AVDRMObjectDescriptor){
		.fd = d->fd, .size = d->size, .format_modifier = d->modifier
	};
	AVDRMLayerDescriptor *layer = &desc->layers[0];

	layer->format = MKTAG('N', 'V', '1', '2');
	layer->nb_planes = 2;
	for (int i = 0; i < 2; i++) {
		if (!line_capture_plane_valid(d->size, d->offset[i],
					      d->stride[i], frame->width,
					      frame->height >> i, d->modifier))
			goto out;
		layer->planes[i] =
			(AVDRMPlaneDescriptor){ .object_index = 0,
						.offset = d->offset[i],
						.pitch = d->stride[i] };
	}
	src->width = frame->width;
	src->height = frame->height;
	src->format = AV_PIX_FMT_DRM_PRIME;
	src->data[0] = src->buf[0]->data;
	av_frame_unref(dst);
	dst->hw_frames_ctx = av_buffer_ref(frames_ctx);
	dst->format = AV_PIX_FMT_VAAPI;
	ok = dst->hw_frames_ctx &&
	     av_hwframe_map(dst, src,
			    AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) >= 0;
out:
	av_frame_free(&src);
	return ok;
}

static gboolean upload_frame(struct call_vp8_encoder *e,
			     const struct line_camera_frame *src)
{
	AVFrame *hw = e->surface, *cpu = e->cpu;
	AVHWFramesContext *pool =
		(AVHWFramesContext *)e->codec->hw_frames_ctx->data;

	av_frame_unref(hw);
	if (av_hwframe_get_buffer(e->codec->hw_frames_ctx, hw, 0) < 0)
		return FALSE;
	hw->width = src->width;
	hw->height = src->height;
	if (e->direct_map) {
		cpu->format = AV_PIX_FMT_NV12;
		if (av_hwframe_map(cpu, hw,
				   AV_HWFRAME_MAP_WRITE |
					   AV_HWFRAME_MAP_OVERWRITE |
					   AV_HWFRAME_MAP_DIRECT) >= 0) {
			copy_nv12(cpu, src);
			av_frame_unref(cpu);
			return TRUE;
		}
		/* Some discrete devices only support a staged upload. */
		av_frame_unref(cpu);
		e->direct_map = FALSE;
	}
	if (!cpu->buf[0]) {
		cpu->width = pool->width;
		cpu->height = pool->height;
		cpu->format = AV_PIX_FMT_NV12;
		if (av_frame_get_buffer(cpu, 32) < 0)
			return FALSE;
		cpu->width = src->width;
		cpu->height = src->height;
	}
	copy_nv12(cpu, src);
	return av_hwframe_transfer_data(hw, cpu, 0) >= 0;
}

#endif

#if CALL_HWDECODE || CALL_VP8_JETSON
/* Validate the raw VP8 uncompressed header, including the shown-frame
 * bit and requested keyframe size. Container flags are insufficient.
 */
static gboolean check_picture(const struct line_camera_frame *frame,
			      const guint8 *picture, gsize size,
			      gboolean force_key, gboolean *key)
{
	if (!picture || size <= 3)
		return FALSE;
	guint tag = picture[0] | (picture[1] << 8) | (picture[2] << 16);
	gboolean is_key = !(tag & 1);
	guint header = is_key ? 10 : 3;
	guint partition = tag >> 5;

	if (!(tag & 0x10) || ((tag >> 1) & 7) > 3 || (force_key && !is_key) ||
	    size <= header || !partition || partition > size - header)
		return FALSE;
	if (is_key) {
		guint w = picture[6] | ((picture[7] & 0x3f) << 8);
		guint h = picture[8] | ((picture[9] & 0x3f) << 8);

		if (memcmp(picture + 3, "\x9d\x01\x2a", 3) ||
		    w != frame->width || h != frame->height)
			return FALSE;
	}
	*key = is_key;
	return TRUE;
}
#endif

gboolean call_vp8_encoder_frame(struct call_vp8_encoder *e,
				const struct line_camera_frame *frame,
				gint64 pts90k, gboolean force_key,
				const guint8 **picture, gsize *size,
				gboolean *key)
{
	if (!e || !frame || !picture || !size || !key || pts90k < 0 ||
	    !line_camera_frame_size_valid(frame->width, frame->height))
		return FALSE;
	for (guint i = 0; !frame->capture_dma && i < 3; i++) {
		guint w = i ? (frame->width + 1) / 2 : frame->width;

		if (!frame->plane[i] || frame->stride[i] < (int)w)
			return FALSE;
	}
#if CALL_VP8_JETSON
	if (e->jetson) {
		if (frame->capture_dma ||
		    !call_vp8_jetson_frame(e->jetson, frame, pts90k, force_key,
					   picture, size, key))
			return FALSE;
		return check_picture(frame, *picture, *size, force_key, key);
	}
#endif
#if CALL_HWDECODE
	if (frame->width != (guint)e->codec->width ||
	    frame->height != (guint)e->codec->height)
		return FALSE;
	if (frame->capture_dma) {
		if (e->codec->pix_fmt != AV_PIX_FMT_VAAPI ||
		    !call_capture_import_nv12(e->surface,
					      e->codec->hw_frames_ctx, frame))
			return FALSE;
	} else if (!upload_frame(e, frame)) {
		return FALSE;
	}
	e->surface->pts = pts90k;
	e->surface->pict_type = force_key ? AV_PICTURE_TYPE_I :
					    AV_PICTURE_TYPE_NONE;
	e->surface->color_range = AVCOL_RANGE_MPEG;
	av_packet_unref(e->packet);
	if (avcodec_send_frame(e->codec, e->surface) < 0 ||
	    avcodec_receive_packet(e->codec, e->packet) < 0)
		return FALSE;
	if (e->packet->pts != AV_NOPTS_VALUE && e->packet->pts != pts90k)
		return FALSE;
	if (e->packet->size <= 0 ||
	    !check_picture(frame, e->packet->data, e->packet->size, force_key,
			   key))
		return FALSE;
	*picture = e->packet->data;
	*size = e->packet->size;
	return TRUE;
#else
	(void)force_key;
	return FALSE;
#endif
}

gboolean call_vp8_capture_probe(guint w, guint h, guint fps, guint kbps,
				guint64 *modifier)
{
#if CALL_HWDECODE
	struct call_vp8_encoder *e =
		call_vp8_encoder_open(w, h, kbps, fps, fps * 3);
	AVFrame *owned = NULL, *map = NULL, *black = NULL;
	gboolean ok = FALSE;

	if (!e || !e->codec || e->codec->pix_fmt != AV_PIX_FMT_VAAPI)
		goto out;
	owned = av_frame_alloc();
	map = av_frame_alloc();
	black = av_frame_alloc();
	if (!owned || !map || !black ||
	    av_hwframe_get_buffer(e->codec->hw_frames_ctx, owned, 0) < 0)
		goto out;
	black->width = w;
	black->height = h;
	black->format = AV_PIX_FMT_NV12;
	if (av_frame_get_buffer(black, 32) < 0)
		goto out;
	for (guint i = 0; i < 2; i++)
		for (guint y = 0; y < (h >> i); y++)
			memset(black->data[i] + y * black->linesize[i],
			       i ? 128 : 16, w);
	if (av_hwframe_transfer_data(owned, black, 0) < 0)
		goto out;
	map->format = AV_PIX_FMT_DRM_PRIME;
	if (av_hwframe_map(map, owned,
			   AV_HWFRAME_MAP_DIRECT | AV_HWFRAME_MAP_READ) < 0)
		goto out;
	const AVDRMFrameDescriptor *desc = (const void *)map->data[0];

	if (!desc || desc->nb_objects != 1 ||
	    desc->objects[0].format_modifier == ((UINT64_C(1) << 56) - 1))
		goto out;
	struct line_capture_dma dma = {
		.fd = desc->objects[0].fd,
		.size = desc->objects[0].size,
		.modifier = desc->objects[0].format_modifier
	};
	for (int i = 0; i < 2; i++) {
		const AVDRMPlaneDescriptor *p;

		if (desc->nb_layers == 1 &&
		    desc->layers[0].format == MKTAG('N', 'V', '1', '2') &&
		    desc->layers[0].nb_planes == 2)
			p = &desc->layers[0].planes[i];
		else if (desc->nb_layers == 2 &&
			 desc->layers[i].nb_planes == 1 &&
			 desc->layers[i].format ==
				 (i ? MKTAG('G', 'R', '8', '8') :
				      MKTAG('R', '8', ' ', ' ')))
			p = &desc->layers[i].planes[0];
		else
			goto out;
		if (p->object_index || p->offset < 0 ||
		    p->offset > G_MAXUINT32 || p->pitch <= 0 ||
		    (guint64)p->pitch > G_MAXUINT32)
			goto out;
		dma.offset[i] = p->offset;
		dma.stride[i] = p->pitch;
	}
	/* The exported surface stays live through the encoder's final unref. */
	GBytes *capture = g_bytes_new(&dma, sizeof(dma));
	struct line_camera_frame frame = { .capture_dma = capture,
					   .width = w,
					   .height = h };
	const guint8 *picture;
	gsize size;
	gboolean key;

	ok = call_vp8_encoder_frame(e, &frame, 0, TRUE, &picture, &size,
				    &key) &&
	     key;
	g_bytes_unref(capture);
	if (ok)
		*modifier = dma.modifier;
out:
	call_vp8_encoder_close(e);
	av_frame_free(&map);
	av_frame_free(&owned);
	av_frame_free(&black);
	return ok;
#else
	(void)w;
	(void)h;
	(void)fps;
	(void)kbps;
	(void)modifier;
	return FALSE;
#endif
}
