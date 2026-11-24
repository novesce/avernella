// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_video.h"
#include "call_vp8.h"
#include "call_vp8_encode.h"
#include "call_srtp.h"
#include <math.h>
#include <openssl/rand.h>
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

enum {
	RTP_HEADER = 12,
	VIDEO_MTU = 1200,
	MAX_DATAGRAM = 4096,
	FRAME_LIMIT = (1 << 18) - 4,
	SHARE_FRAME_LIMIT = 1 << 20,
	MAX_FRAGMENTS = 512,
	ASSEMBLY_LIMIT = FRAME_LIMIT + MAX_FRAGMENTS * 16,
	RTCP_PSFB = 206,
	RTCP_PLI = 1,
	RTCP_FIR = 4,
	LINE_FIR = 17,
	LINE_FIR_SIZE = 28,
	SRTCP_TRAILER = 14,
	FRAME_SIZE_DELTAS = 8,
	KEY_OFFSET_LIMIT = 64,
	EVS3_GROUP_PROFILE_VP8 = 3,
	EVS3_GROUP_PROFILE_VP8A = 4,
	EVS3_GROUP_LAYER_SYNC = 0x10,
	EVS3_PROFILE_HEADER = 6,
	EVS3_CAPTURE_SOURCE_SIZE = 6,
	EVS3_ENCODE_DELAY_SIZE = 4,
	EVS3_BASE_LAYER_SIZE = 3,
	EVS3_RTP_VFD_SIZE = 12,
	EVS3_RTP_VED_SIZE = 20,
	EVS3_VED_TARGET_BR_SIZE = 4,
};

enum video_format {
	VIDEO_UNKNOWN,
	VIDEO_EVS3,
	VIDEO_EVS3_WIDE,
	VIDEO_EVS3_PROFILED,
	VIDEO_EVS3_UNSIZED,
	VIDEO_RFC7741
};

struct video_fragment {
	GBytes *bytes;
	guint16 seq;
	gboolean marker;
};

struct video_payload {
	gsize offset, expected;
	gboolean start, end;
	guint rotation;
	guint32 extended_size;
};

/* Encoding time over the last second, for call_video_pace(). */
struct encode_pace {
	int base, speed;
	gint64 spent_us;
	guint frames;
};

/* WebRTC's lowest real-time VP8 speed setting for slow machines. */
enum {
	FASTEST_SPEED = -12
};

int call_video_pace(int speed, int base, gint64 average_us, guint fps)
{
	gint64 interval = G_USEC_PER_SEC / MAX(fps, 1u);

	if (average_us * 100 > interval * 60)
		return MAX(speed - 1, FASTEST_SPEED);
	if (average_us * 100 < interval * 25 && speed < base)
		return speed + 1;
	return speed;
}

static void pace_start(struct encode_pace *p, int speed)
{
	*p = (struct encode_pace){ .base = speed, .speed = speed };
}

/* Records one encode; changes the speed once a second's frames are in. */
static gboolean pace_encoder(vpx_codec_ctx_t *enc, struct encode_pace *p,
			     gint64 spent_us, guint fps)
{
	p->spent_us += MAX(spent_us, 0);
	if (++p->frames < fps)
		return TRUE;
	int next = call_video_pace(p->speed, p->base, p->spent_us / p->frames,
				   fps);

	p->spent_us = p->frames = 0;
	if (next == p->speed)
		return TRUE;
	p->speed = next;
	return vpx_codec_control(enc, VP8E_SET_CPUUSED, next) == VPX_CODEC_OK;
}

enum vp8_source {
	VP8_CAMERA,
	VP8_SCREEN_RTP,
	VP8_SCREEN_PDTP,
};

struct vp8_sender {
	struct call_vp8_encoder *hw;
	vpx_codec_ctx_t sw;
	struct encode_pace pace;
	enum vp8_source source;
	guint width, height, kbps, fps;
	gboolean ready, force_key, hw_failed, announced;
};

struct vp8_picture {
	const guint8 *data;
	gsize size;
	gboolean key;
	guint16 delay_ms;
};

static void sender_close(struct vp8_sender *s)
{
	if (s->hw)
		g_clear_pointer(&s->hw, call_vp8_encoder_close);
	else if (s->ready)
		vpx_codec_destroy(&s->sw);
	s->ready = FALSE;
	s->announced = FALSE;
}

/* Both transports submit exactly one image at a time. Without lookahead,
 * alt-ref frames or partition output, libvpx returns a complete VP8 picture.
 */
static gboolean sender_open_software(struct vp8_sender *s)
{
	vpx_codec_enc_cfg_t cfg;
	vpx_codec_iface_t *codec = vpx_codec_vp8_cx();
	gboolean screen = s->source != VP8_CAMERA;
	gboolean pdtp = s->source == VP8_SCREEN_PDTP;
	guint pixels = s->width * s->height;
	guint cores = MAX(1, g_get_num_processors());
	int speed = pixels >= 1280 * 720 ? -6 : pixels >= 640 * 360 ? -5 : -4;

	if (vpx_codec_enc_config_default(codec, &cfg, 0))
		return FALSE;
	cfg.g_w = s->width;
	cfg.g_h = s->height;
	cfg.g_threads = pixels >= 1280 * 720 ? MIN(cores, screen ? 8 : 4) :
			pixels >= 640 * 360  ? MIN(cores, 2) :
					       1;
	cfg.g_timebase = (vpx_rational_t){ 1, 90000 };
	cfg.g_lag_in_frames = 0;
	cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
	cfg.g_pass = VPX_RC_ONE_PASS;
	cfg.rc_end_usage = screen ? VPX_VBR : VPX_CBR;
	cfg.rc_target_bitrate = s->kbps;
	cfg.rc_min_quantizer = 2;
	cfg.rc_max_quantizer = screen ? 40 : 56;
	cfg.rc_undershoot_pct = 100;
	cfg.rc_overshoot_pct = screen ? 50 : 15;
	cfg.rc_buf_initial_sz = 500;
	cfg.rc_buf_optimal_sz = 600;
	cfg.rc_buf_sz = 1000;
	cfg.rc_dropframe_thresh = 0;
	cfg.rc_resize_allowed = 0;
	cfg.kf_mode = VPX_KF_AUTO;
	cfg.kf_max_dist = 3 * s->fps;

	guint intra = screen ? CLAMP(131072u * s->fps / s->kbps, 400, 1000) :
			       300;
	int partitions = screen && cfg.g_threads >= 4 ?
				 VP8_FOUR_TOKENPARTITION :
			 cfg.g_threads >= 2 ? VP8_TWO_TOKENPARTITION :
					      VP8_ONE_TOKENPARTITION;
	int threshold = screen ? 20 : 1;

	/* Reliable screen-share transport has a larger buffer and favors text. */
	if (pdtp) {
		cfg.g_threads = MIN(cores, 8);
		cfg.g_error_resilient = 0;
		cfg.rc_buf_initial_sz = 1000;
		cfg.rc_buf_optimal_sz = 1500;
		cfg.rc_buf_sz = 3000;
		cfg.kf_max_dist = 10 * s->fps;
		speed = pixels > 1280 * 720 ? -8 : -6;
		intra = 1000;
		threshold = 100;
		partitions = VP8_FOUR_TOKENPARTITION;
	}
	if (vpx_codec_enc_init(&s->sw, codec, &cfg, 0))
		return FALSE;
	pace_start(&s->pace, speed);
	if (vpx_codec_control(&s->sw, VP8E_SET_CPUUSED, speed) ||
	    vpx_codec_control(&s->sw, VP8E_SET_SCREEN_CONTENT_MODE,
			      (int)screen) ||
	    vpx_codec_control(&s->sw, VP8E_SET_STATIC_THRESHOLD, threshold) ||
	    vpx_codec_control(&s->sw, VP8E_SET_NOISE_SENSITIVITY, 0) ||
	    vpx_codec_control(&s->sw, VP8E_SET_TOKEN_PARTITIONS, partitions) ||
	    vpx_codec_control(&s->sw, VP8E_SET_MAX_INTRA_BITRATE_PCT, intra)) {
		vpx_codec_destroy(&s->sw);
		return FALSE;
	}
	s->ready = TRUE;
	s->force_key = TRUE;
	return TRUE;
}

static gboolean sender_open(struct vp8_sender *s, guint w, guint h, guint kbps,
			    guint fps, enum vp8_source source)
{
	sender_close(s);
	s->width = w;
	s->height = h;
	s->kbps = kbps;
	s->fps = fps;
	s->source = source;
	if (!s->hw_failed)
		s->hw = call_vp8_encoder_open(
			w, h, kbps, fps,
			fps * (source == VP8_SCREEN_PDTP ? 10 : 3));
	if (s->hw) {
		s->ready = TRUE;
		s->force_key = TRUE;
		return TRUE;
	}
	return sender_open_software(s);
}

static gboolean sender_image(vpx_image_t *img,
			     const struct line_camera_frame *frame)
{
	*img = (vpx_image_t){ .fmt = VPX_IMG_FMT_I420,
			      .bit_depth = 8,
			      .x_chroma_shift = 1,
			      .y_chroma_shift = 1,
			      .bps = 12,
			      .w = frame->width,
			      .d_w = frame->width,
			      .h = frame->height,
			      .d_h = frame->height };
	for (int i = 0; i < 3; i++) {
		guint w = i ? (img->w + 1) / 2 : img->w;

		if (!frame->plane[i] || frame->stride[i] < (int)w)
			return FALSE;
		img->planes[i] = (guint8 *)frame->plane[i];
		img->stride[i] = frame->stride[i];
	}
	return TRUE;
}

static void sender_emitted(struct vp8_sender *s)
{
	s->force_key = FALSE;
	if (s->announced)
		return;
	s->announced = TRUE;
	if (!g_strcmp0(g_getenv("LINE_RENDER_LOG"), "1"))
		g_printerr(
			"Outgoing %s VP8 encoder produced a picture using %s\n",
			s->source == VP8_CAMERA ? "camera" : "screen",
			s->hw ? call_vp8_encoder_name(s->hw) : "libvpx");
}

static gboolean sender_encode(struct vp8_sender *s, const vpx_image_t *img,
			      gint64 pts, struct vp8_picture *picture)
{
	const vpx_codec_cx_pkt_t *pkt;
	vpx_codec_iter_t iter = NULL;
	gint64 started = g_get_monotonic_time();

	if (s->hw) {
		const struct line_camera_frame frame = {
			.width = img->d_w,
			.height = img->d_h,
			.plane = { img->planes[0], img->planes[1],
				   img->planes[2] },
			.stride = { img->stride[0], img->stride[1],
				    img->stride[2] },
		};
		if (call_vp8_encoder_frame(s->hw, &frame, pts, s->force_key,
					   &picture->data, &picture->size,
					   &picture->key) &&
		    picture->data && picture->size &&
		    picture->size <= FRAME_LIMIT) {
			picture->delay_ms =
				MIN((g_get_monotonic_time() - started) / 1000,
				    G_MAXUINT16);
			sender_emitted(s);
			return TRUE;
		}
		/* A fresh software keyframe can replace this very picture. No
		 * packet or sequence number has gone to the peer yet. */
		if (!g_strcmp0(g_getenv("LINE_RENDER_LOG"), "1"))
			g_printerr(
				"Call VP8 encoder %s failed, switching to libvpx\n",
				call_vp8_encoder_name(s->hw));
		sender_close(s);
		s->hw_failed = TRUE;
		if (!sender_open_software(s))
			return FALSE;
	}
	if (vpx_codec_encode(&s->sw, img, pts, 90000 / s->fps,
			     s->force_key ? VPX_EFLAG_FORCE_KF : 0,
			     VPX_DL_REALTIME))
		return FALSE;
	gint64 elapsed = g_get_monotonic_time() - started;

	if (!pace_encoder(&s->sw, &s->pace, elapsed, s->fps))
		return FALSE;
	picture->delay_ms = MIN(elapsed / 1000, G_MAXUINT16);
	picture->size = 0;
	while ((pkt = vpx_codec_get_cx_data(&s->sw, &iter))) {
		if (pkt->kind != VPX_CODEC_CX_FRAME_PKT)
			continue;
		picture->data = pkt->data.frame.buf;
		picture->size = pkt->data.frame.sz;
		picture->key = !!(pkt->data.frame.flags & VPX_FRAME_IS_KEY);
		sender_emitted(s);
		break;
	}
	return TRUE;
}

struct call_video {
	srtp_t tx, rx;
	struct vp8_sender sender;
	struct call_vp8_decoder *dec;
	gboolean need_key;
	gboolean group_tx;
	int requested_resolution;
	vpx_image_t *scaled;
	guint kbps, fps;
	guint screen_kbps, screen_fps;
	guint rx_rotation;
	guint32 tx_ssrc, rx_ssrc, ts_base, rx_ts;
	guint16 seq, picture, rx_picture, first, last, previous_end;
	guint16 ved_seq[8];
	guint8 pt, capture_source, capture_generation, group_profile;
	enum call_video_rtp_extension tx_extension;
	gint64 epoch, last_encoded, next_layer_frame;
	gint64 group_fir_at;
	enum video_format framing, rx_format;
	gboolean have_ts, have_last, have_previous_end, finished, group_rx;
	gboolean group_rx_trace;
	gsize received, expected;
	guint32 extended_size;
	unsigned int fragments, authenticated;

	struct {
		guint evs_starts, rfc_starts, markers, unknown, incomplete;
		guint evs_metadata;
		guint evs_sized_keys, evs_wide_keys, evs_wide_starts;
		guint evs_profiled_keys, evs_profiled_starts;
		guint profiled_key_candidates, profiled_ids, profiled_sizes;
		guint profiled_same_size, profiled_codec_bounds;
		guint evs_unsized_keys, evs_unsized_starts;
		guint assembly, length, pictures, keys, deltas;
		guint bad_key, dimensions, decode, hidden;
		guint flags[16];
		guint size_delta[2 * FRAME_SIZE_DELTAS + 3];
		guint ext_sizes, ext_size_matches;
		guint rtp_vfd, rtp_ved, rtp_no_extension, rtp_other_extension;
		guint payload_offsets[KEY_OFFSET_LIMIT];
		guint vp8_key_offsets[KEY_OFFSET_LIMIT];
		guint avc_key_offsets[KEY_OFFSET_LIMIT];
	} stats;
	struct video_fragment part[MAX_FRAGMENTS];
};

static guint16 read16(const guint8 *p)
{
	return ((guint16)p[0] << 8) | p[1];
}

static guint32 read32(const guint8 *p)
{
	return ((guint32)read16(p) << 16) | read16(p + 2);
}

static void write16(guint8 *p, guint16 n)
{
	p[0] = n >> 8;
	p[1] = n;
}

static void write32(guint8 *p, guint32 n)
{
	write16(p, n >> 16);
	write16(p + 2, n);
}

static void clear_picture(struct call_video *v)
{
	for (guint i = 0; v->fragments && i < MAX_FRAGMENTS; i++) {
		if (!v->part[i].bytes)
			continue;
		g_clear_pointer(&v->part[i].bytes, g_bytes_unref);
		v->fragments--;
	}
	v->received = v->expected = 0;
	v->framing = VIDEO_UNKNOWN;
	v->have_last = FALSE;
}

void call_video_close(struct call_video *v)
{
	if (!v)
		return;
	clear_picture(v);
	sender_close(&v->sender);
	call_vp8_close(v->dec);
	if (v->tx)
		srtp_dealloc(v->tx);
	if (v->rx)
		srtp_dealloc(v->rx);
	g_clear_pointer(&v->scaled, vpx_img_free);
	g_free(v);
}

static struct call_video *open_video(const guint8 tx[30], const guint8 rx[30],
				     guint8 pt, guint32 tx_ssrc,
				     guint32 rx_ssrc, guint kbps, guint fps,
				     GError **e)
{
	struct call_video *v = g_new0(struct call_video, 1);
	/* Bound decoder threads when a conference subscribes to several peers. */
	guint threads = tx ? MIN(4, g_get_num_processors()) : 1;

	if ((!tx && !rx) || pt < 96 || pt > 127)
		goto failed;
	if (tx && (!kbps || kbps > 8000 || !fps || fps > 30 ||
		   !call_srtp_open(&v->tx, tx, ssrc_specific, tx_ssrc) ||
		   RAND_bytes((guint8 *)&v->seq, sizeof(v->seq)) != 1 ||
		   RAND_bytes((guint8 *)&v->picture, sizeof(v->picture)) != 1 ||
		   RAND_bytes((guint8 *)&v->ts_base, sizeof(v->ts_base)) != 1))
		goto failed;
	if (rx && (!call_srtp_open(&v->rx, rx, ssrc_specific, rx_ssrc) ||
		   !(v->dec = call_vp8_open(threads))))
		goto failed;
	v->need_key = rx != NULL;

	v->pt = pt;
	v->tx_ssrc = tx_ssrc;
	v->rx_ssrc = rx_ssrc;
	v->kbps = kbps;
	v->fps = fps;
	v->epoch = v->last_encoded = -1;
	return v;

failed:
	call_video_close(v);
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not initialize encrypted VP8 video.");
	return NULL;
}

struct call_video *call_video_open(const guint8 tx[30], const guint8 rx[30],
				   guint8 pt, guint32 tx_ssrc, guint32 rx_ssrc,
				   guint kbps, guint fps, GError **e)
{
	if (!tx || !rx) {
		g_set_error_literal(
			e, LINE_ERROR, 1,
			"Could not initialize encrypted VP8 video.");
		return NULL;
	}
	return open_video(tx, rx, pt, tx_ssrc, rx_ssrc, kbps, fps, e);
}

struct call_video *call_video_open_tx(const guint8 key[30], guint8 pt,
				      guint32 ssrc, guint kbps, guint fps,
				      GError **e)
{
	return open_video(key, NULL, pt, ssrc, 0, kbps, fps, e);
}

struct call_video *call_video_open_group_tx(const guint8 key[30], guint8 pt,
					    guint32 ssrc, guint kbps, guint fps,
					    GError **error)
{
	struct call_video *v =
		call_video_open_tx(key, pt, ssrc, kbps, fps, error);

	if (v) {
		v->group_tx = TRUE;
		v->requested_resolution = -1;
		v->capture_source = 0xff; /* No frame has been published yet. */
	}
	return v;
}

struct call_video *call_video_open_rx(const guint8 key[30], guint8 pt,
				      guint32 ssrc, GError **e)
{
	return open_video(NULL, key, pt, 0, ssrc, 0, 0, e);
}

struct call_video *call_video_open_group_rx(const guint8 key[30], guint8 pt,
					    guint32 ssrc, GError **error)
{
	struct call_video *v = call_video_open_rx(key, pt, ssrc, error);

	if (v) {
		v->group_rx = TRUE;
		v->group_rx_trace =
			!g_strcmp0(g_getenv("LINE_GROUP_CALL_STATS"), "1");
	}
	return v;
}

/* Resolution classes used by vns_get_resolution_type_from_wxh. */
static const guint resolution_pixels[] = { 19200, 172800, 691200, 2764800,
					   11059200 };

static guint resolution_class(guint count)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(resolution_pixels) &&
		    count > resolution_pixels[i];
	     i++)
		;
	return i;
}

static gboolean configure_encoder(struct call_video *v, guint w, guint h)
{
	struct vp8_sender *s = &v->sender;
	guint pixels = w * h;
	gboolean screen = v->screen_kbps != 0;
	guint kbps = screen ? v->screen_kbps : v->kbps;
	guint fps = screen ? v->screen_fps : v->fps;

	/* The negotiated stream rate is a ceiling for the whole conference
	 * video. The published SVC camera layer has its own rate budget.
	 */
	if (v->group_profile && !screen) {
		static const guint max_kbps[] = { 150, 300, 800, 3000 };
		guint layer = resolution_class(pixels);

		if (layer < G_N_ELEMENTS(max_kbps))
			kbps = MIN(kbps, max_kbps[layer]);
		if (!layer)
			fps = MIN(fps, 15u);
	}
	if (s->ready && w == s->width && h == s->height && kbps == s->kbps &&
	    fps == s->fps)
		return TRUE;
	if (!sender_open(s, w, h, kbps, fps,
			 screen ? VP8_SCREEN_RTP : VP8_CAMERA))
		return FALSE;
	if (v->group_tx && !g_strcmp0(g_getenv("LINE_GROUP_CALL_STATS"), "1"))
		g_printerr(
			"group-stats camera-encoder width=%u height=%u kbps=%u fps=%u profile=%u backend=%s\n",
			w, h, kbps, fps, v->group_profile,
			s->hw ? call_vp8_encoder_name(s->hw) : "libvpx");
	return TRUE;
}

void call_video_set_group_resolution(struct call_video *v, int resolution)
{
	if (!v || !v->group_tx || resolution < -1 ||
	    resolution >= (int)G_N_ELEMENTS(resolution_pixels))
		return;
	v->requested_resolution = resolution;
}

/* Bilinear sampling at pixel centers; the source may be a padded camera plane. */
static void scale_plane(const guint8 *src, int pitch, guint sw, guint sh,
			guint8 *dst, int stride, guint dw, guint dh)
{
	if (sw == dw && sh == dh) {
		for (guint row = 0; row < sh;
		     row++, dst += stride, src += pitch)
			memcpy(dst, src, sw);
		return;
	}
	guint dx = (sw << 16) / dw, dy = (sh << 16) / dh;
	guint y = (dy - 65536) / 2;

	for (guint row = 0; row < dh; row++, y += dy, dst += stride) {
		guint sy = y >> 16, fy = (y >> 8) & 255;
		const guint8 *a = src + (gsize)sy * pitch;
		const guint8 *b = src + (gsize)MIN(sy + 1, sh - 1) * pitch;
		guint x = (dx - 65536) / 2;

		for (guint col = 0; col < dw; col++, x += dx) {
			guint sx = x >> 16, right = MIN(sx + 1, sw - 1);
			int fx = (x >> 8) & 255;
			int top = 256 * a[sx] + fx * (a[right] - a[sx]);
			int bot = 256 * b[sx] + fx * (b[right] - b[sx]);

			dst[col] = ((256 - fy) * top + fy * bot + 32768) >> 16;
		}
	}
}

static gboolean group_encoder_image(struct call_video *v, vpx_image_t *img)
{
	guint w = img->w, h = img->h;

	if (v->requested_resolution >= 0) {
		guint source = resolution_class(w * h);
		guint target = v->requested_resolution;

		if (target < source && v->screen_kbps) {
			double ratio = sqrt((double)resolution_pixels[target] /
					    (w * h));

			w = MAX(2u, (guint)(w * ratio) & ~1u);
			h = MAX(2u, (guint)(h * ratio) & ~1u);
		} else if (target < source) {
			/* LINE's camera transcoder halves both dimensions per class.
			 * Align the full-size image before reducing to even I420 planes.
			 */
			guint mask = (1u << MAX(source, 1u)) - 1;
			guint shift = source - target;

			w = MAX(2u, ((w & ~mask) >> shift) & ~1u);
			h = MAX(2u, ((h & ~mask) >> shift) & ~1u);
		}
	}
	if (w == img->w && h == img->h && img->stride[1] == img->stride[2]) {
		/* libvpx accepts external planes with a shared chroma stride. */
		g_clear_pointer(&v->scaled, vpx_img_free);
		return TRUE;
	}

	if (!v->scaled || v->scaled->d_w != w || v->scaled->d_h != h) {
		g_clear_pointer(&v->scaled, vpx_img_free);
		v->scaled = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 16);
		if (!v->scaled)
			return FALSE;
	}
	for (guint i = 0; i < 3; i++) {
		guint shift = i != 0;

		scale_plane(img->planes[i], img->stride[i],
			    (img->w + shift) >> shift,
			    (img->h + shift) >> shift, v->scaled->planes[i],
			    v->scaled->stride[i], (w + shift) >> shift,
			    (h + shift) >> shift);
	}
	*img = *v->scaled;
	return TRUE;
}

/* Conference SVC reserves SID 7 for the thumbnail and SIDs 1..3 for
 * progressively larger camera layers. TID 1 is the first temporal stream.
 */
static guint group_spatial_id(guint resolution)
{
	return resolution ? MIN(resolution, 3) : 7;
}

/* The first fragment carries the negotiated codec header after the picture
 * descriptor. Group membership does not select a different VP8 payload map.
 */
static gsize write_descriptor(const struct call_video *v, guint8 *p, guint16 id,
			      gboolean first, gboolean last, gboolean key,
			      guint32 size, guint16 delay_ms)
{
	guint res = resolution_class(v->sender.width * v->sender.height);
	guint sid = group_spatial_id(res);
	guint layer = (1u << 5) | (sid << 1);
	gsize pos = 3;

	p[0] = 0x80 | (last ? 4 : 0);
	write16(p + 1, id);
	if (!first)
		return pos;
	p[0] |= key ? 0x3a : 0x7a;
	/* In conference EVS3, bit 4 identifies temporal layer zero. */
	p[pos++] = v->group_profile ? layer : (v->group_tx ? 0x10 : 0);
	if (!key) {
		p[pos++] = 3;
		p[pos++] = 1;
		p[pos++] = 0;
	}
	p[pos++] = (res << 1) | (key ? 0x20 : 0);
	if (v->group_tx) {
		p[pos - 1] |= 1;
		/* PLANET camera user data precedes the encoder's delay record. */
		p[pos++] = 0x80;
		p[pos++] = 4;
		p[pos++] = 1;
		p[pos++] = v->capture_generation;
		p[pos++] = 2;
		p[pos++] = v->capture_source;
		p[pos++] = 5;
		p[pos++] = v->group_profile ? 2 : 0x82;
		write16(p + pos, delay_ms);
		pos += 2;
	}
	if (v->group_profile) {
		gboolean wide = v->group_profile == EVS3_GROUP_PROFILE_VP8A;

		/* The SVC picture carries the same base-layer identity as RTP. */
		p[pos++] = 8;
		p[pos++] = 0x81;
		p[pos++] = (1u << 5) | (sid << 2);
		p[pos++] = v->group_profile;
		p[pos++] = layer | (key ? EVS3_GROUP_LAYER_SYNC : 0);
		write32(p + pos, size + (wide ? 4 : 3));
		pos += 4;
		if (wide) {
			write32(p + pos, size);
			return pos + 4;
		}
	}

	/* The compact length includes the three-byte field itself. */
	size += 3;
	p[pos++] = (v->group_tx ? 0x08 : 0) | (size & 3);
	p[pos++] = size >> 2;
	p[pos++] = size >> 10;
	return pos;
}

void call_video_set_group_format(struct call_video *v,
				 enum call_video_rtp_extension extension,
				 gboolean vp8a)
{
	guint8 profile = 0;

	if (extension != CALL_VIDEO_RTP_EXT_NONE)
		profile = vp8a ? EVS3_GROUP_PROFILE_VP8A :
				 EVS3_GROUP_PROFILE_VP8;
	if (!v || !v->group_tx ||
	    (v->tx_extension == extension && v->group_profile == profile))
		return;
	v->tx_extension = extension;
	v->group_profile = profile;
	v->sender.force_key = TRUE;
}

enum call_video_rtp_extension
call_video_group_rtp_extension(const struct call_video *v)
{
	return v->tx_extension;
}

/* PLANET RTP extension words hold one record for the single VP8 layer. */
static void write_group_rtp_extension(struct call_video *v, guint8 *p,
				      gsize size, guint16 id, gboolean first,
				      gboolean last, gboolean key)
{
	const guint8 codec = v->group_profile;
	guint kbps = v->sender.kbps;
	guint res = resolution_class(v->sender.width * v->sender.height);
	guint sid = group_spatial_id(res);
	guint layer = (sid << 5) | (1u << 2);

	/* Main-channel video uses PLANET transmission method zero. */
	write16(p, 0x0200);
	write16(p + 2, (size - 4) / 4);
	if (v->tx_extension == CALL_VIDEO_RTP_EXT_VFD) {
		p[4] = 2;
		p[5] = 3; /* Six-byte VFD layer descriptor. */
		p[6] = 0x80 | (last ? 0x40 : 0) | (sid << 3) | 1;
		p[7] = (MIN(res, 3) << 4) | 8 | (key ? 4 : 0) | (first ? 2 : 0);
		p[8] = codec;
		p[9] = MIN(kbps / 8, 255);
		write16(p + 10, v->seq);
		return;
	}
	p[4] = 4;
	gboolean target_rate = first && key;

	p[5] = target_rate ? 8 : 6; /* Length in two-byte words. */
	p[6] = target_rate ? 0x22 : 0x02; /* One layer, optional fields flag. */
	p[7] = 0;
	write16(p + 8, id);
	p[10] = layer;
	/* Single referenceable VP8 layer, with no inter-layer dependency. */
	p[11] = (res << 5) | 4 | (key ? 2 : 0);
	p[12] = codec;
	p[13] = MIN(kbps / 20, 255);
	p[14] = layer | (first ? 2 : 0) | (last ? 1 : 0);
	p[15] = 0x18 | (key ? 0x20 : 0); /* Last record for this packet. */
	/* The mixer tracks continuity separately for each layer. */
	write16(p + 16, ++v->ved_seq[sid]);
	if (target_rate) {
		/* Target bitrate for the base layer, in units of 20 kbit/s. */
		p[18] = 0xa1;
		p[19] = layer;
		p[20] = p[13];
		p[21] = 0;
	}
	write16(p + size - 2, 0); /* Align the extension to four bytes. */
}

static gboolean send_picture(struct call_video *v, guint32 timestamp,
			     const guint8 *picture, gsize size, gboolean key,
			     guint16 delay_ms, call_video_send_fn send,
			     gpointer data)
{
	const gsize codec_header =
		(v->group_profile ? EVS3_PROFILE_HEADER : 0) +
		(v->group_profile == EVS3_GROUP_PROFILE_VP8A ? 4 : 3);
	const gsize metadata = v->group_tx ? EVS3_CAPTURE_SOURCE_SIZE +
						     EVS3_ENCODE_DELAY_SIZE :
					     0;
	const gsize header_max = 8 + codec_header + metadata +
				 (v->group_profile ? EVS3_BASE_LAYER_SIZE : 0);
	const gsize base_extension =
		v->tx_extension == CALL_VIDEO_RTP_EXT_VFD ? EVS3_RTP_VFD_SIZE :
		v->tx_extension == CALL_VIDEO_RTP_EXT_VED ? EVS3_RTP_VED_SIZE :
							    0;
	guint16 id = v->picture++;
	guint8 source = v->screen_kbps != 0; /* Camera 0, virtual display 1. */

	if (size > FRAME_LIMIT) {
		v->sender.force_key = TRUE;
		return TRUE;
	}
	/* Count source changes on published pictures, including the first. */
	if (v->group_tx && v->capture_source != source) {
		v->capture_source = source;
		v->capture_generation++;
	}
	for (gsize pos = 0; pos < size;) {
		guint8 packet[VIDEO_MTU + SRTP_MAX_TRAILER_LEN];
		gsize extension = base_extension;

		if (!pos && key && v->tx_extension == CALL_VIDEO_RTP_EXT_VED)
			extension += EVS3_VED_TARGET_BR_SIZE;
		gsize chunk =
			VIDEO_MTU - RTP_HEADER - extension - header_max - 10;
		gsize len = MIN(chunk, size - pos);
		gboolean last = pos + len == size;
		gsize header;
		int n;

		packet[0] = extension ? 0x90 : 0x80;
		packet[1] = v->pt | (last ? 0x80 : 0);
		if (extension)
			write_group_rtp_extension(v, packet + RTP_HEADER,
						  extension, id, !pos, last,
						  key);
		write16(packet + 2, v->seq++);
		write32(packet + 4, timestamp);
		write32(packet + 8, v->tx_ssrc);
		header = RTP_HEADER + extension;
		header += write_descriptor(v, packet + header, id, !pos, last,
					   key, size, delay_ms);
		memcpy(packet + header, picture + pos, len);
		n = header + len;
		if (srtp_protect(v->tx, packet, &n) != srtp_err_status_ok ||
		    !send(packet, n, data))
			return FALSE;
		pos += len;
	}
	return TRUE;
}

void call_video_keyframe(struct call_video *v)
{
	if (v && v->tx)
		v->sender.force_key = TRUE;
}

void call_video_screen(struct call_video *v, guint kbps, guint fps)
{
	fps = kbps ? CLAMP(fps, 1, 30) : 0;
	if (!v || !v->tx || (kbps == v->screen_kbps && fps == v->screen_fps))
		return;
	sender_close(&v->sender);
	v->screen_kbps = kbps;
	v->screen_fps = fps;
	v->sender.force_key = TRUE;
}

gboolean call_video_encode(struct call_video *v,
			   const struct line_camera_frame *frame, gint64 now,
			   call_video_send_fn send, gpointer data, GError **e)
{
	vpx_image_t img;
	struct vp8_picture picture = { 0 };
	guint64 pts;

	if (!v || !v->tx || !send || !frame || now < 0 ||
	    !line_camera_frame_size_valid(frame->width, frame->height))
		goto failed;
	/* Accept jitter, and switch sources immediately even at the same size. */
	if (v->sender.ready && v->last_encoded >= 0) {
		if (v->next_layer_frame) {
			if (now < v->next_layer_frame)
				return TRUE;
		} else if (now - v->last_encoded <
			   G_USEC_PER_SEC / v->sender.fps * 3 / 4) {
			return TRUE;
		}
	}
	if (!sender_image(&img, frame) ||
	    (v->group_tx && !group_encoder_image(v, &img)) ||
	    !configure_encoder(v, img.d_w, img.d_h))
		goto failed;
	if (v->epoch < 0)
		v->epoch = now;
	pts = ((guint64)(now - v->epoch) / 100) * 9 +
	      ((guint64)(now - v->epoch) % 100) * 9 / 100;
	if (!sender_encode(&v->sender, &img, pts, &picture))
		goto failed;
	v->last_encoded = now;
	if (!v->screen_kbps && v->sender.fps < v->fps) {
		gint64 interval = G_USEC_PER_SEC / v->sender.fps;
		gint64 due = v->next_layer_frame;

		/* Keep the reduced layer clock on cadence between camera samples. */
		v->next_layer_frame =
			(due && now - due < interval ? due : now) + interval;
	} else {
		v->next_layer_frame = 0;
	}
	if (picture.size &&
	    !send_picture(v, v->ts_base + pts, picture.data, picture.size,
			  picture.key, picture.delay_ms, send, data))
		goto failed;
	return TRUE;
failed:
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode or send the call video frame.");
	return FALSE;
}

static gboolean decode_picture(struct call_video *v, const guint8 *p, gsize n,
			       call_video_display_fn display, gpointer data)
{
	int shown;
	gboolean key = !(p[0] & 1);

	v->stats.pictures++;
	if (key) {
		vpx_codec_stream_info_t info = { .sz = sizeof(info) };

		v->stats.keys++;
		if (vpx_codec_peek_stream_info(vpx_codec_vp8_dx(), p, n,
					       &info) ||
		    !info.is_kf) {
			v->stats.bad_key++;
			return FALSE;
		}
		if (!line_camera_frame_size_valid(info.w, info.h)) {
			v->stats.dimensions++;
			return FALSE;
		}
	} else if (v->need_key) {
		v->stats.deltas++;
		return TRUE;
	}
	shown = call_vp8_decode(v->dec, p, n, key, v->rx_rotation, display,
				data);
	if (shown < 0) {
		v->stats.decode++;
		v->need_key = TRUE;
		return FALSE;
	}
	v->need_key = FALSE;
	v->stats.hidden += !shown;
	return TRUE;
}

static guint32 evs3_compact_length(const guint8 *p)
{
	return (p[0] & 3) | (p[1] << 2) | (p[2] << 10);
}

/*
 * Skip optional layer references, rotation and extensions. EVS3 format 1
 * includes a three-byte length in the first fragment. Format 2 and PDTP
 * screen-share frames omit it.
 */
static gboolean parse_evs3_framed(const guint8 *p, gsize n, gboolean length,
				  struct video_payload *out)
{
	gsize pos = 3, expected = 0;
	guint flags, rotation = 0;
	guint32 extended_size = 0;
	gboolean ext = FALSE;

	if (n <= pos || !(p[0] & 0x80))
		return FALSE;
	flags = p[0];
	if (flags & 0x20) {
		if (pos == n)
			return FALSE;
		/* Layer and reference metadata do not change the VP8 offset. */
		pos++;
		if ((flags & 0x50) == 0x50) {
			for (int i = 0; i < 3; i++) {
				if (pos == n)
					return FALSE;
				if (!(p[pos++] & 1))
					break;
				if (i == 2)
					return FALSE;
			}
		}
	}
	if (flags & 2) {
		if (pos == n)
			return FALSE;
		rotation = (p[pos] >> 6) * 90;
		ext = p[pos++] & 1;
	}
	while (ext) {
		if (n - pos < 2)
			return FALSE;
		guint len = p[pos + 1] & 127;
		guint type = p[pos];

		ext = !(p[pos + 1] & 128);
		pos += 2;
		if (len > n - pos)
			return FALSE;
		if (type == 9 && len == 4)
			extended_size = read32(p + pos);
		pos += len;
	}
	if ((flags & 8) && length) {
		guint total;

		if (n - pos < 3)
			return FALSE;
		total = evs3_compact_length(p + pos);
		if (total <= 3)
			return FALSE;
		expected = total - 3;
		pos += 3;
	}
	if (pos >= n)
		return FALSE;
	*out = (struct video_payload){ .offset = pos,
				       .expected = expected,
				       .start = !!(flags & 8),
				       .end = !!(flags & 4),
				       .rotation = rotation,
				       .extended_size = extended_size };
	return TRUE;
}

static gboolean vp8_key_at(const guint8 *frame, gsize n)
{
	if (n < 10)
		return FALSE;
	vpx_codec_stream_info_t info = { .sz = sizeof(info) };

	return !(frame[0] & 1) && frame[3] == 0x9d && frame[4] == 0x01 &&
	       frame[5] == 0x2a &&
	       !vpx_codec_peek_stream_info(vpx_codec_vp8_dx(), frame, n,
					   &info) &&
	       info.is_kf && line_camera_frame_size_valid(info.w, info.h);
}

/* The type 2 EVS3 codec header holds the exact frame size in big endian. */
static gboolean evs3_wide_size(const guint8 *p, gsize n,
			       struct video_payload *out)
{
	if (!out->start)
		return TRUE;
	if (n - out->offset <= 4)
		return FALSE;
	guint32 size = read32(p + out->offset);

	if (size > FRAME_LIMIT || size < n - out->offset - 4)
		return FALSE;
	out->offset += 4;
	out->expected = size;
	return TRUE;
}

/* The conference profile gives the size of the codec header and VP8 data. */
static gboolean evs3_profiled_size(const guint8 *p, gsize n,
				   struct video_payload *out)
{
	if (!out->start)
		return TRUE;
	gsize pos = out->offset;

	if (n - pos <= 6)
		return FALSE;
	guint32 total = read32(p + pos + 2);

	out->offset += 6;
	if (p[pos] == EVS3_GROUP_PROFILE_VP8A)
		return evs3_wide_size(p, n, out) && total == out->expected + 4;
	if (p[pos] != EVS3_GROUP_PROFILE_VP8 || n - out->offset <= 3 ||
	    total <= 3 || total - 3 > FRAME_LIMIT || total < n - out->offset ||
	    total != evs3_compact_length(p + out->offset))
		return FALSE;
	out->offset += 3;
	out->expected = total - 3;
	return TRUE;
}

static void record_key_offsets(struct call_video *v, const guint8 *p, gsize n)
{
	/* Record offsets only. The media bytes remain in the decryption buffer. */
	for (guint i = 3; i < KEY_OFFSET_LIMIT && i + 4 < n; i++) {
		if (p[i + 3] == 0x9d && vp8_key_at(p + i, n - i))
			v->stats.vp8_key_offsets[i]++;
		if (!p[i] && !p[i + 1] && !p[i + 2] && p[i + 3] == 1 &&
		    ((p[i + 4] & 31) == 5 || (p[i + 4] & 31) == 7))
			v->stats.avc_key_offsets[i]++;
	}
}

static void record_profile_key(struct call_video *v, const guint8 *p, gsize n)
{
	if (!n)
		return;
	guint header = p[0] == EVS3_GROUP_PROFILE_VP8 ? 3 : 4;

	if (n <= 6 + header || !vp8_key_at(p + 6 + header, n - 6 - header))
		return;
	guint32 profile_size = read32(p + 2);
	guint64 framed_size = header == 3 ? evs3_compact_length(p + 6) :
					    (guint64)read32(p + 6) + 4;

	v->stats.profiled_key_candidates++;
	v->stats.profiled_ids += p[0] == EVS3_GROUP_PROFILE_VP8 ||
				 p[0] == EVS3_GROUP_PROFILE_VP8A;
	v->stats.profiled_sizes += profile_size == framed_size;
	v->stats.profiled_same_size += (guint64)profile_size + header ==
				       framed_size;
	v->stats.profiled_codec_bounds += framed_size <= FRAME_LIMIT + header &&
					  framed_size >= n - 6;
}

static enum video_format evs3_payload(struct call_video *v, const guint8 *p,
				      gsize n, struct video_payload *out)
{
	gboolean sized = parse_evs3_framed(p, n, TRUE, out);
	enum video_format format = sized ? VIDEO_EVS3 : VIDEO_UNKNOWN;
	struct video_payload unsized;

	if (!v->group_rx || !(p[0] & 8) ||
	    !parse_evs3_framed(p, n, FALSE, &unsized))
		return format;
	gboolean sized_key = sized &&
			     vp8_key_at(p + out->offset, n - out->offset);
	gboolean unsized_key =
		vp8_key_at(p + unsized.offset, n - unsized.offset);
	struct video_payload wide = unsized;
	gboolean wide_size = evs3_wide_size(p, n, &wide);
	gboolean wide_key = wide_size &&
			    vp8_key_at(p + wide.offset, n - wide.offset);
	struct video_payload profiled = unsized;
	gboolean profiled_size = evs3_profiled_size(p, n, &profiled);
	gboolean profiled_key =
		profiled_size &&
		vp8_key_at(p + profiled.offset, n - profiled.offset);

	if (v->group_rx_trace) {
		v->stats.evs_sized_keys += sized_key;
		v->stats.evs_wide_keys += wide_key;
		v->stats.evs_profiled_keys += profiled_key;
		v->stats.evs_unsized_keys += unsized_key;
		if (sized && out->offset < KEY_OFFSET_LIMIT)
			v->stats.payload_offsets[out->offset]++;
		record_key_offsets(v, p, n);
		record_profile_key(v, p + unsized.offset, n - unsized.offset);
	}
	/* Latch alternate framing only on a recognizable VP8 keyframe. */
	if (sized_key)
		return format;
	if (profiled_size &&
	    (profiled_key || (!wide_key && !unsized_key &&
			      v->rx_format == VIDEO_EVS3_PROFILED))) {
		*out = profiled;
		v->stats.evs_profiled_starts += v->group_rx_trace;
		return VIDEO_EVS3_PROFILED;
	}
	if (wide_size &&
	    (wide_key || (!unsized_key && v->rx_format == VIDEO_EVS3_WIDE))) {
		*out = wide;
		v->stats.evs_wide_starts += v->group_rx_trace;
		return VIDEO_EVS3_WIDE;
	}
	if (unsized_key || v->rx_format == VIDEO_EVS3_UNSIZED) {
		*out = unsized;
		v->stats.evs_unsized_starts += v->group_rx_trace;
		return VIDEO_EVS3_UNSIZED;
	}
	return format;
}

static gboolean parse_vp8(const guint8 *p, gsize n, gboolean marker,
			  struct video_payload *out)
{
	gsize pos = 1;

	/* RFC 7741 reserved bits distinguish frame starts from EVS3. */
	if (n < 2 || (p[0] & 0x48))
		return FALSE;
	if (p[0] & 0x80) {
		guint8 extensions = p[pos++];

		if (extensions & 0x80) {
			if (pos == n)
				return FALSE;
			pos += p[pos] & 0x80 ? 2 : 1;
		}
		pos += !!(extensions & 0x40) + !!(extensions & 0x30);
	}
	if (pos >= n)
		return FALSE;
	*out = (struct video_payload){ .offset = pos,
				       .start = (p[0] & 0x17) == 0x10,
				       .end = marker };
	return TRUE;
}

static gboolean describe(enum video_format framing, const guint8 *p, gsize n,
			 gboolean marker, struct video_payload *out)
{
	if (framing == VIDEO_RFC7741)
		return parse_vp8(p, n, marker, out);
	return parse_evs3_framed(p, n, framing == VIDEO_EVS3, out) &&
	       (framing != VIDEO_EVS3_WIDE || evs3_wide_size(p, n, out)) &&
	       (framing != VIDEO_EVS3_PROFILED ||
		evs3_profiled_size(p, n, out));
}

static gboolean finish_picture(struct call_video *v, const guint8 *picture,
			       gsize len, call_video_display_fn display,
			       gpointer data)
{
	if (v->group_rx_trace && v->extended_size) {
		v->stats.ext_sizes++;
		v->stats.ext_size_matches += len + 3 == v->extended_size ||
					     len + 6 == v->extended_size;
	}
	if (v->group_rx_trace && v->expected) {
		int delta = (int)len - (int)v->expected;

		delta = CLAMP(delta, -FRAME_SIZE_DELTAS - 1,
			      FRAME_SIZE_DELTAS + 1);
		v->stats.size_delta[delta + FRAME_SIZE_DELTAS + 1]++;
	}
	if (v->expected && len != v->expected) {
		v->stats.length++;
		return FALSE;
	}
	if (v->have_previous_end && (guint16)(v->first - v->previous_end) != 1)
		v->need_key = TRUE;
	v->previous_end = v->last;
	v->have_previous_end = TRUE;
	if (!v->group_rx) {
		v->rx_format = v->framing;
		return decode_picture(v, picture, len, display, data);
	}
	if (!decode_picture(v, picture, len, display, data))
		return FALSE;
	v->rx_format = v->framing;
	return TRUE;
}

static gboolean assemble_picture(struct call_video *v, guint count,
				 call_video_display_fn display, gpointer data)
{
	g_autofree guint8 *picture = g_malloc(v->received);
	gsize len = 0;

	for (guint i = 0; i < count; i++) {
		guint16 seq = v->first + i;
		const struct video_fragment *part =
			&v->part[seq % MAX_FRAGMENTS];
		struct video_payload payload;
		gsize bytes;

		if (!part->bytes || part->seq != seq)
			return FALSE;
		const guint8 *p = g_bytes_get_data(part->bytes, &bytes);

		if (!describe(v->framing, p, bytes, part->marker, &payload) ||
		    payload.start != (i == 0) ||
		    payload.end != (i + 1 == count) ||
		    (v->framing != VIDEO_RFC7741 &&
		     read16(p + 1) != v->rx_picture))
			return FALSE;
		bytes -= payload.offset;
		if (bytes > FRAME_LIMIT - len)
			return FALSE;
		memcpy(picture + len, p + payload.offset, bytes);
		len += bytes;
	}
	return finish_picture(v, picture, len, display, data);
}

static gboolean mark_end(struct call_video *v, guint16 seq)
{
	if (v->have_last)
		return FALSE;
	v->last = seq;
	v->have_last = TRUE;
	return TRUE;
}

static gboolean find_early_end(struct call_video *v)
{
	for (guint i = 0, checked = 0;
	     i < MAX_FRAGMENTS && checked < v->fragments; i++) {
		const struct video_fragment *part = &v->part[i];
		struct video_payload payload;
		gsize size;

		if (!part->bytes)
			continue;
		checked++;
		const guint8 *p = g_bytes_get_data(part->bytes, &size);

		if (!describe(v->framing, p, size, part->marker, &payload) ||
		    payload.start || (payload.end && !mark_end(v, part->seq)))
			return FALSE;
	}
	return TRUE;
}

static gboolean accept_fragment(struct call_video *v, const guint8 *p, gsize n,
				guint16 seq, guint32 ts, gboolean marker,
				call_video_display_fn display, gpointer data)
{
	struct video_fragment *part = &v->part[seq % MAX_FRAGMENTS];
	struct video_payload evs3 = { 0 }, vp8 = { 0 }, payload;
	enum video_format evs = evs3_payload(v, p, n, &evs3);
	gboolean rfc = parse_vp8(p, n, marker, &vp8);

	v->stats.flags[p[0] >> 4]++;
	if ((p[0] & 0xa8) == 0xa8 && n > 3 && p[3])
		v->stats.evs_metadata++;
	v->stats.evs_starts += evs && evs3.start;
	v->stats.rfc_starts += rfc && vp8.start;
	v->stats.markers += marker;
	if (!evs && !rfc) {
		v->stats.unknown++;
		return FALSE;
	}
	if (!v->have_ts || (gint32)(ts - v->rx_ts) > 0) {
		if (v->have_ts && !v->finished) {
			v->stats.incomplete++;
			v->need_key = TRUE;
		}
		clear_picture(v);
		v->have_ts = TRUE;
		v->rx_ts = ts;
		v->finished = FALSE;
	}
	if (ts != v->rx_ts || v->finished || part->bytes ||
	    n > ASSEMBLY_LIMIT - v->received)
		return FALSE;
	if (v->framing == VIDEO_UNKNOWN && (evs3.start || vp8.start)) {
		v->framing = evs3.start ? evs : VIDEO_RFC7741;
		payload = evs3.start ? evs3 : vp8;
		v->rx_picture = evs3.start ? read16(p + 1) : 0;
		v->expected = payload.expected;
		v->extended_size = payload.extended_size;
		v->rx_rotation = payload.rotation;
		v->first = seq;
		if (!find_early_end(v))
			goto invalid;
	} else {
		payload = v->framing == VIDEO_RFC7741 ? vp8 : evs3;
		if (v->framing != VIDEO_UNKNOWN &&
		    (!payload.offset || payload.start))
			return FALSE;
	}
	if (v->framing != VIDEO_UNKNOWN && v->framing != VIDEO_RFC7741 &&
	    read16(p + 1) != v->rx_picture)
		return FALSE;
	if (v->framing != VIDEO_UNKNOWN && payload.end && !mark_end(v, seq))
		goto invalid;
	/* Decryption owns this buffer through the synchronous VP8 decode. */
	if (!v->fragments && payload.start && payload.end) {
		if (!finish_picture(v, p + payload.offset, n - payload.offset,
				    display, data))
			goto invalid;
		goto complete;
	}
	part->seq = seq;
	part->marker = marker;
	part->bytes = g_bytes_new(p, n);
	v->received += n;
	v->fragments++;
	if (!v->have_last)
		return TRUE;
	guint count = (guint16)(v->last - v->first) + 1u;

	if (count > MAX_FRAGMENTS || v->fragments > count)
		goto invalid;
	if (v->fragments < count)
		return TRUE;
	if (!assemble_picture(v, count, display, data))
		goto invalid;
complete:
	v->finished = TRUE;
	clear_picture(v);
	return TRUE;
invalid:
	v->stats.assembly++;
	v->need_key = v->finished = TRUE;
	clear_picture(v);
	return FALSE;
}

static void record_group_rtp_extensions(struct call_video *v, const guint8 *p,
					gsize n)
{
	gboolean vfd = FALSE, ved = FALSE;

	if (p[0] == 2) {
		/* Routing words precede the media elements in a PLANET extension. */
		for (gsize i = 4u + 4u * (p[1] & 15); i < n;) {
			if (!p[i]) {
				i++;
				continue;
			}
			if (n - i < 2)
				break;
			guint length = 2u * p[i + 1];

			if (length > n - i - 2)
				break;
			vfd |= p[i] == 2 && length >= 6;
			ved |= p[i] == 4 && length >= 4;
			i += 2 + length;
		}
	}
	v->stats.rtp_vfd += vfd;
	v->stats.rtp_ved += ved;
	v->stats.rtp_other_extension += !vfd && !ved;
}

gboolean call_video_receive(struct call_video *v, const guint8 *packet,
			    gsize size, call_video_display_fn display,
			    gpointer data)
{
	guint8 buf[MAX_DATAGRAM];
	int plain = size;
	gsize pos, end;

	if (!v || !v->rx || !packet || !display || size <= RTP_HEADER + 10 ||
	    size > sizeof(buf) || packet[0] >> 6 != 2 ||
	    (packet[1] & 127) != v->pt || read32(packet + 8) != v->rx_ssrc)
		return FALSE;
	memcpy(buf, packet, size);
	if (srtp_unprotect(v->rx, buf, &plain) != srtp_err_status_ok)
		return FALSE;
	v->authenticated++;
	if (plain <= RTP_HEADER)
		return FALSE;
	end = plain;
	pos = RTP_HEADER + 4u * (buf[0] & 15);
	if (pos >= end)
		return FALSE;
	if (buf[0] & 0x10) {
		if (end - pos < 4)
			return FALSE;
		gsize extension = 4u + 4u * read16(buf + pos + 2);

		if (extension >= end - pos)
			return FALSE;
		if (v->group_rx_trace)
			record_group_rtp_extensions(v, buf + pos, extension);
		pos += extension;
	} else if (v->group_rx_trace) {
		v->stats.rtp_no_extension++;
	}
	if (buf[0] & 0x20) {
		guint pad = buf[end - 1];

		if (!pad || pad >= end - pos)
			return FALSE;
		end -= pad;
	}
	return accept_fragment(v, buf + pos, end - pos, read16(buf + 2),
			       read32(buf + 4), !!(buf[1] & 0x80), display,
			       data);
}

gboolean call_video_needs_keyframe(const struct call_video *v)
{
	return v && v->need_key;
}

guint call_video_authenticated_packets(const struct call_video *v)
{
	return v ? v->authenticated : 0;
}

static void report_group_framing(const struct call_video *v)
{
	GString *s = g_string_new(
		"group-stats video size-deltas actual-minus-declared");

	for (guint i = 0; i < G_N_ELEMENTS(v->stats.size_delta); i++)
		if (v->stats.size_delta[i])
			g_string_append_printf(s, " %d:%u",
					       (int)i - FRAME_SIZE_DELTAS - 1,
					       v->stats.size_delta[i]);
	g_printerr("%s\n", s->str);
	g_string_assign(s, "group-stats video keyframe offsets");
	for (guint i = 3; i < KEY_OFFSET_LIMIT; i++) {
		if (v->stats.payload_offsets[i])
			g_string_append_printf(s, " sized@%u:%u", i,
					       v->stats.payload_offsets[i]);
		if (v->stats.vp8_key_offsets[i])
			g_string_append_printf(s, " vp8@%u:%u", i,
					       v->stats.vp8_key_offsets[i]);
		if (v->stats.avc_key_offsets[i])
			g_string_append_printf(s, " avc@%u:%u", i,
					       v->stats.avc_key_offsets[i]);
	}
	g_printerr("%s\n", s->str);
	g_printerr("group-stats video extended-length found=%u matches=%u\n",
		   v->stats.ext_sizes, v->stats.ext_size_matches);
	g_printerr(
		"group-stats video profile-candidates keys=%u native-id=%u lengths-match=%u lengths-equal=%u codec-size-valid=%u\n",
		v->stats.profiled_key_candidates, v->stats.profiled_ids,
		v->stats.profiled_sizes, v->stats.profiled_same_size,
		v->stats.profiled_codec_bounds);
	g_printerr(
		"group-stats video RTP extensions vfd=%u ved=%u none=%u other=%u\n",
		v->stats.rtp_vfd, v->stats.rtp_ved, v->stats.rtp_no_extension,
		v->stats.rtp_other_extension);
	g_string_free(s, TRUE);
}

void call_video_report(const struct call_video *v, gboolean group)
{
	if (!v)
		return;
	g_printerr(
		"%s video payload evs-start=%u metadata=%u rfc-start=%u marker=%u unknown=%u incomplete=%u assembly=%u length=%u pictures=%u keys=%u waiting-key=%u bad-key=%u dimensions=%u decode=%u hidden=%u decoder=%s\n",
		group ? "group-stats" : "line-call", v->stats.evs_starts,
		v->stats.evs_metadata, v->stats.rfc_starts, v->stats.markers,
		v->stats.unknown, v->stats.incomplete, v->stats.assembly,
		v->stats.length, v->stats.pictures, v->stats.keys,
		v->stats.deltas, v->stats.bad_key, v->stats.dimensions,
		v->stats.decode, v->stats.hidden, call_vp8_backend(v->dec));
	if (group && v->group_rx_trace) {
		g_printerr(
			"group-stats video size-framing sized-keys=%u wide-keys=%u wide-starts=%u profiled-keys=%u profiled-starts=%u unsized-keys=%u unsized-starts=%u\n",
			v->stats.evs_sized_keys, v->stats.evs_wide_keys,
			v->stats.evs_wide_starts, v->stats.evs_profiled_keys,
			v->stats.evs_profiled_starts, v->stats.evs_unsized_keys,
			v->stats.evs_unsized_starts);
		report_group_framing(v);
	}
	GString *flags =
		g_string_new(group ? "group-stats video descriptor classes" :
				     "line-call video descriptor classes");

	for (guint i = 0; i < G_N_ELEMENTS(v->stats.flags); i++)
		if (v->stats.flags[i])
			g_string_append_printf(flags, " %x:%u", i,
					       v->stats.flags[i]);
	g_printerr("%s\n", flags->str);
	g_string_free(flags, TRUE);
}

gboolean call_video_send_peer_feedback(struct call_video *tx,
				       const struct call_video *rx,
				       call_video_send_fn send, gpointer data)
{
	guint8 packet[LINE_FIR_SIZE + SRTP_MAX_TRAILER_LEN + 4] = { 0 };
	gboolean standard;
	int size;

	if (!tx || !tx->tx || !rx || !rx->rx || !send)
		return FALSE;
	standard = rx->framing == VIDEO_RFC7741 ||
		   (rx->framing == VIDEO_UNKNOWN &&
		    rx->rx_format == VIDEO_RFC7741);
	size = standard ? RTP_HEADER : LINE_FIR_SIZE;
	packet[0] = 0x80 | (standard ? RTCP_PLI : LINE_FIR);
	packet[1] = RTCP_PSFB;
	write16(packet + 2, size / 4 - 1);
	write32(packet + 4, tx->tx_ssrc);
	write32(packet + 8, rx->rx_ssrc);
	/* Use LINE's base-layer FCI with no reference picture. */
	return srtp_protect_rtcp(tx->tx, packet, &size) == srtp_err_status_ok &&
	       send(packet, size, data);
}

gboolean call_video_send_feedback(struct call_video *v, call_video_send_fn send,
				  gpointer data)
{
	return call_video_send_peer_feedback(v, v, send, data);
}

static gboolean picture_feedback(const guint8 *p, gsize size, guint32 local,
				 guint32 remote)
{
	if (size < RTP_HEADER || p[1] != RTCP_PSFB || read32(p + 4) != remote)
		return FALSE;
	switch (p[0] & 31) {
	case RTCP_PLI:
		return size == RTP_HEADER && read32(p + 8) == local;
	case RTCP_FIR:
		for (gsize pos = RTP_HEADER; size - pos >= 8; pos += 8) {
			if (read32(p + pos) == local)
				return TRUE;
		}
		return FALSE;
	case LINE_FIR:
		return size == LINE_FIR_SIZE && read32(p + 8) == local;
	default:
		return FALSE;
	}
}

gboolean call_video_receive_feedback(struct call_video *v, const guint8 *packet,
				     gsize size)
{
	guint8 buf[MAX_DATAGRAM];
	int plain = size;
	gboolean request = FALSE;
	gsize pos = 0;

	if (!v || !v->tx || !v->rx || !packet || size <= 8 + SRTCP_TRAILER ||
	    size > sizeof(buf) || packet[0] >> 6 != 2 || packet[1] < 192 ||
	    packet[1] > 223 || read32(packet + 4) != v->rx_ssrc)
		return FALSE;
	memcpy(buf, packet, size);
	if (srtp_unprotect_rtcp(v->rx, buf, &plain) != srtp_err_status_ok)
		return FALSE;
	while ((gsize)plain - pos >= 4) {
		const guint8 *p = buf + pos;
		gsize bytes = 4u * (1u + read16(p + 2)), payload = bytes;

		if (p[0] >> 6 != 2 || bytes > (gsize)plain - pos)
			return FALSE;
		if (p[0] & 0x20) {
			guint8 padding = p[bytes - 1];

			if (!padding || padding > bytes - 4)
				return FALSE;
			payload -= padding;
		}
		request |= picture_feedback(p, payload, v->tx_ssrc, v->rx_ssrc);
		pos += bytes;
	}
	if (!request || pos != (gsize)plain)
		return FALSE;
	call_video_keyframe(v);
	return TRUE;
}

gboolean call_video_receive_group_feedback(struct call_video *v,
					   const guint8 *packet, gsize size,
					   guint32 sender, gint64 now)
{
	if (!v || !v->group_tx || !packet || !sender || sender == v->tx_ssrc ||
	    size != LINE_FIR_SIZE || packet[0] != (0x80 | LINE_FIR) ||
	    read16(packet + 2) != LINE_FIR_SIZE / 4 - 1 ||
	    !picture_feedback(packet, size, v->tx_ssrc, sender) || now <= 0)
		return FALSE;
	if (v->group_fir_at && now - v->group_fir_at < G_USEC_PER_SEC)
		return FALSE;
	v->group_fir_at = now;
	call_video_keyframe(v);
	return TRUE;
}

struct call_share_decoder {
	struct call_vp8_decoder *dec;
	gboolean need_key, assembling;
	guint16 picture;
	guint rotation;
	GByteArray *frame;
	guint frames, errors;
};

struct call_share_decoder *call_share_decoder_new(void)
{
	struct call_share_decoder *s = g_new0(struct call_share_decoder, 1);

	s->dec = call_vp8_open(MIN(4, g_get_num_processors()));
	if (!s->dec) {
		g_free(s);
		return NULL;
	}
	s->need_key = TRUE;
	s->frame = g_byte_array_new();
	return s;
}

void call_share_decoder_free(struct call_share_decoder *s)
{
	if (!s)
		return;
	call_vp8_close(s->dec);
	g_byte_array_free(s->frame, TRUE);
	g_free(s);
}

gboolean call_share_decoder_keyframe_needed(const struct call_share_decoder *s)
{
	return !s || s->need_key;
}

static gboolean share_decode_frame(struct call_share_decoder *s,
				   const guint8 *vp8, gsize n,
				   call_video_display_fn display, gpointer data)
{
	gboolean key = n >= 3 && !(vp8[0] & 1);
	int shown;

	if (!key && s->need_key)
		return TRUE;
	if (key) {
		vpx_codec_stream_info_t info = { .sz = sizeof(info) };

		if (vpx_codec_peek_stream_info(vpx_codec_vp8_dx(), vp8, n,
					       &info) ||
		    !info.is_kf ||
		    !line_camera_frame_size_valid(info.w, info.h))
			return FALSE;
	}
	shown = call_vp8_decode(s->dec, vp8, n, key, s->rotation, display,
				data);
	if (shown < 0)
		return FALSE;
	s->need_key = FALSE;
	s->frames += shown;
	return TRUE;
}

gboolean call_share_decode(struct call_share_decoder *s, const guint8 *frag,
			   gsize size, call_video_display_fn display,
			   gpointer data)
{
	struct video_payload payload;

	if (!s || !frag || !display ||
	    !parse_evs3_framed(frag, size, FALSE, &payload))
		goto failed;
	guint16 picture = read16(frag + 1);

	if (payload.start) {
		if (s->assembling)
			s->need_key = TRUE;
		g_byte_array_set_size(s->frame, 0);
		s->assembling = TRUE;
		s->picture = picture;
		s->rotation = payload.rotation;
		if (payload.end) {
			s->assembling = FALSE;
			if (size - payload.offset <= SHARE_FRAME_LIMIT &&
			    share_decode_frame(s, frag + payload.offset,
					       size - payload.offset, display,
					       data))
				return TRUE;
			goto failed;
		}
	} else if (!s->assembling || picture != s->picture) {
		/* A lost start fragment: wait for the next picture. */
		goto failed;
	}
	if (s->frame->len + size - payload.offset > SHARE_FRAME_LIMIT)
		goto failed;
	g_byte_array_append(s->frame, frag + payload.offset,
			    size - payload.offset);
	if (!payload.end)
		return TRUE;
	s->assembling = FALSE;
	if (share_decode_frame(s, s->frame->data, s->frame->len, display, data))
		return TRUE;
failed:
	if (s) {
		s->errors++;
		s->need_key = TRUE;
		s->assembling = FALSE;
	}
	return FALSE;
}

/*
 * Screen share encoder. Screen content is mostly static text, so it uses
 * VP8's screen-content mode and spends its bits on sharp keyframes and
 * small updates rather than motion.
 */
struct call_share_encoder {
	struct vp8_sender sender;
	guint kbps, fps;
	guint16 picture;
	gint64 last_encoded;
};

struct call_share_encoder *call_share_encoder_new(guint kbps, guint fps)
{
	struct call_share_encoder *s = g_new0(struct call_share_encoder, 1);

	s->kbps = CLAMP(kbps, 300, 20000);
	s->fps = CLAMP(fps, 1, 60);
	s->last_encoded = -1;
	if (RAND_bytes((guint8 *)&s->picture, sizeof(s->picture)) != 1)
		s->picture = 1;
	return s;
}

void call_share_encoder_free(struct call_share_encoder *s)
{
	if (!s)
		return;
	sender_close(&s->sender);
	g_free(s);
}

void call_share_encoder_keyframe(struct call_share_encoder *s)
{
	if (s)
		s->sender.force_key = TRUE;
}

/*
 * The descriptor the official app writes for a presentation fragment: the
 * start carries layer byte 0x10, the delta references, the resolution class
 * and two extensions (type 5, the encode delay in ms; type 9, the frame size
 * plus three) instead of the RTP length field.
 */
gsize call_share_descriptor(guint8 *p, guint16 picture, gboolean first,
			    gboolean last, gboolean key, guint resolution,
			    guint delay_ms, guint32 size)
{
	gsize pos = 3;

	p[0] = 0x80 | (last ? 4 : 0);
	write16(p + 1, picture);
	if (!first)
		return pos;
	p[0] |= key ? 0x3a : 0x7a;
	p[pos++] = 0x10;
	if (!key) {
		p[pos++] = 3;
		p[pos++] = 1;
		p[pos++] = 0;
	}
	p[pos++] = (resolution << 1) | (key ? 0x20 : 0) | 1;
	p[pos++] = 5;
	p[pos++] = 2;
	write16(p + pos, MIN(delay_ms, 0xffff));
	pos += 2;
	p[pos++] = 9;
	p[pos++] = 0x84;
	write32(p + pos, size + 3);
	return pos + 4;
}

gboolean call_share_encode(struct call_share_encoder *s,
			   const struct line_camera_frame *frame, gint64 now,
			   call_share_fragment_fn emit, gpointer data,
			   GError **e)
{
	vpx_image_t img;
	struct vp8_picture picture = { 0 };

	if (!s || !emit || !frame || now < 0 ||
	    !line_camera_frame_size_valid(frame->width, frame->height))
		goto failed;
	if (s->last_encoded >= 0 &&
	    now - s->last_encoded < G_USEC_PER_SEC / s->fps * 3 / 4)
		return TRUE;
	if (!sender_image(&img, frame))
		goto failed;
	if ((!s->sender.ready || s->sender.width != img.w ||
	     s->sender.height != img.h) &&
	    !sender_open(&s->sender, img.w, img.h, s->kbps, s->fps,
			 VP8_SCREEN_PDTP))
		goto failed;
	if (!sender_encode(&s->sender, &img, (guint64)now * 9 / 100, &picture))
		goto failed;
	s->last_encoded = now;
	if (!picture.size)
		return TRUE;

	guint16 id = s->picture++;
	guint res = resolution_class(img.w * img.h);

	if (picture.size > FRAME_LIMIT) {
		s->sender.force_key = TRUE;
		return TRUE;
	}
	/* The official app sends about 900 bytes of VP8 per fragment. */
	for (gsize pos = 0; pos < picture.size;) {
		guint8 frag[1024 + 32];
		gsize len = MIN(1000, picture.size - pos);
		gsize header = call_share_descriptor(
			frag, id, !pos, pos + len == picture.size, picture.key,
			res, picture.delay_ms, picture.size);

		memcpy(frag + header, picture.data + pos, len);
		if (!emit(frag, header + len, picture.key, data))
			goto failed;
		pos += len;
	}
	return TRUE;
failed:
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode the shared screen.");
	return FALSE;
}
