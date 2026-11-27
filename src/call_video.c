// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_video.h"
#include "call_fec.h"
#include "codec_affinity.h"
#include "call_vp8.h"
#include "call_vp8_encode.h"
#include "call_srtp.h"
#include "line.h"
#include <errno.h>
#include <math.h>
#include <sys/eventfd.h>
#include <unistd.h>
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
	EVS3_SOURCE_CAMERA = 0,
	GROUP_SCREEN_CLASS = 3, /* HD_FHD, up to 1920x1440 pixels */
	EVS3_ENCODE_DELAY_SIZE = 4,
	EVS3_BASE_LAYER_SIZE = 3,
	EVS3_RTP_VFD_SIZE = 12,
	EVS3_RTP_VED_SIZE = 20,
	EVS3_VED_TARGET_BR_SIZE = 4,
	RTCP_SR = 200,
	RTCP_RR = 201,
	RTCP_RTPFB = 205,
	RTCP_NACK = 1,
	LINE_RRR = 5,
	RTCP_REMB = 15,
	/* Packets held to put a picture back in order or wait for a resend. */
	RX_SLOTS = 256,
	/* Sent packets kept for retransmission, about a second at 4 Mbit/s. */
	TX_HISTORY = 512,
	RRR_MAX = 64,
	RTX_AGE_US = 1000000,
	MIN_KBPS = 60,
	/* Local queue: cut the rate above HIGH for HOLD; DEEP cuts deeper. */
	LOCAL_QUEUE_HIGH_MS = 60,
	LOCAL_QUEUE_HOLD_MS = 100,
	LOCAL_QUEUE_DEEP_MS = 200,
	LOCAL_QUEUE_CUT_GAP_MS = 300,
	KEY_SPACING_US = 300000,
};

/* One RTP sequence slot of the receive reorder buffer. */
struct rx_packet {
	GBytes *payload;
	gint64 missing_since, requested_at;
	guint32 ts;
	guint16 seq;
	guint8 requests;
	gboolean marker;
};

struct tx_packet {
	gint64 sent, resent;
	guint16 seq, size;
	guint8 resends;
	gboolean requested;
	guint8 data[VIDEO_MTU + SRTP_MAX_TRAILER_LEN];
};

/*
 * Sending rate from receiver feedback: loss from retransmission requests,
 * receiver reports and keyframe requests, and a delay-based cap from REMB.
 * Without any feedback the rate stays at the negotiated ceiling.
 */
struct rate_control {
	guint target, remb;
	gint64 window_start, remb_at, scaled_at, last_fir, decreased_at;
	guint sent, lost, adjacent, firs;
	double report_loss, repair_share, request_fraction;
	guint repair_kbps;
	guint64 original_bytes, repair_bytes;
	gboolean feedback; /* the peer sends more than keyframe requests */
	guint scale; /* 0 full size, then 3/4, 1/2, 3/8, 1/4 */
};

/*
 * The kernel's own send queue for the call socket (SIOCOUTQ): datagrams
 * the qdisc, the Wi-Fi stack or the driver still hold. On a weak Wi-Fi
 * uplink this grows before anything is lost, so it is the earliest sign
 * that the sender is too fast, and pictures queued behind it only add
 * delay.
 */
struct local_queue {
	gint64 at, high_since, cut_at;
	guint64 held, sent; /* at the last sample; payload sent since */
	double drain_kbps; /* what actually left, smoothed */
	guint queue_ms, cut_queue_ms, cuts;
};

/* Receive-side delay-based estimate, sent to the sender as REMB. */
struct remb_estimate {
	gint64 base_us, min_delay, min_next, min_reset, sent_at, decreased_at;
	gint64 window_start, updated;
	guint32 last_ts;
	guint64 media_ticks, window_bytes, picture_bytes;
	double queue_ms, calm_ms, rate_kbps, estimate;
	gboolean started, send_now, have_ts, timed_picture;
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

enum video_source {
	VIDEO_SOURCE_UNKNOWN,
	VIDEO_SOURCE_CAMERA,
	VIDEO_SOURCE_DISPLAY,
};

struct video_payload {
	enum video_source source;
	gsize offset, expected;
	gboolean start, end, temporal, enhancement;
	guint16 picture_id;
	guint8 tl0;
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
	gboolean layered, next_enhancement;
};

struct vp8_picture {
	const guint8 *data;
	gsize size;
	gboolean key, enhancement;
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

/*
 * Two pictures per cycle, both predicted only from the previous base.
 * Give the reference picture twice the bits of a disposable picture.
 * libvpx expects cumulative, rather than incremental, layer budgets.
 */
static void temporal_rates(vpx_codec_enc_cfg_t *cfg, guint kbps)
{
	cfg->ts_target_bitrate[0] = MAX(1u, kbps * 2 / 3);
	cfg->ts_target_bitrate[1] = kbps;
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
	/* The call loop owns pacing and packetizes only emitted pictures. */
	cfg.rc_dropframe_thresh = 0;
	cfg.rc_resize_allowed = 0;
	cfg.kf_mode = VPX_KF_AUTO;
	cfg.kf_max_dist = 3 * s->fps;

	if (s->layered) {
		cfg.ts_number_layers = 2;
		cfg.ts_periodicity = 2;
		cfg.ts_layer_id[0] = 0;
		cfg.ts_layer_id[1] = 1;
		cfg.ts_rate_decimator[0] = 2;
		cfg.ts_rate_decimator[1] = 1;
		temporal_rates(&cfg, s->kbps);
		s->next_enhancement = FALSE;
	}

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
	cpu_set_t saved;
	gboolean local = line_codec_local_cpus(&saved, cfg.g_threads);
	vpx_codec_err_t result = vpx_codec_enc_init(&s->sw, codec, &cfg, 0);

	if (local)
		(void)sched_setaffinity(0, sizeof(saved), &saved);
	if (result != VPX_CODEC_OK)
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
	/* The hardware backends cannot select per-frame VP8 references. */
	if (!s->hw_failed && !s->layered)
		s->hw = call_vp8_encoder_open(
			w, h, kbps, fps,
			fps * (source == VP8_SCREEN_PDTP ? 10 : 3));
	s->force_key = TRUE;
	if (s->hw) {
		s->ready = TRUE;
		return TRUE;
	}
	return sender_open_software(s);
}

/* libvpx changes its target rate between frames without a keyframe. */
static gboolean sender_set_rate(struct vp8_sender *s, guint kbps)
{
	vpx_codec_enc_cfg_t cfg;

	if (!s->ready || s->hw || !s->sw.config.enc)
		return FALSE;
	cfg = *s->sw.config.enc;
	cfg.rc_target_bitrate = kbps;
	if (s->layered)
		temporal_rates(&cfg, kbps);
	if (vpx_codec_enc_config_set(&s->sw, &cfg))
		return FALSE;
	s->kbps = kbps;
	return TRUE;
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
			      .d_h = frame->height,
			      .user_priv = frame->capture_dma };
	if (frame->capture_dma)
		return g_bytes_get_size(frame->capture_dma) ==
		       sizeof(struct line_capture_dma);
	for (int i = 0; i < 3; i++) {
		guint w = i ? (img->w + 1) / 2 : img->w;

		if (!frame->plane[i] || frame->stride[i] < (int)w)
			return FALSE;
		img->planes[i] = (guint8 *)frame->plane[i];
		img->stride[i] = frame->stride[i];
	}
	return TRUE;
}

/* The encoder thread may run this; force_key belongs to the call thread. */
static void sender_emitted(struct vp8_sender *s)
{
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
			      gint64 pts, gboolean force,
			      struct vp8_picture *picture)
{
	const vpx_codec_cx_pkt_t *pkt;
	vpx_codec_iter_t iter = NULL;
	gint64 started = g_get_monotonic_time();

	if (s->hw) {
		const struct line_camera_frame frame = {
			.width = img->d_w,
			.height = img->d_h,
			.capture_dma = img->user_priv,
			.plane = { img->planes[0], img->planes[1],
				   img->planes[2] },
			.stride = { img->stride[0], img->stride[1],
				    img->stride[2] },
		};
		if (call_vp8_encoder_frame(s->hw, &frame, pts, force,
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
		if (img->user_priv) {
			/* The next shared-memory capture opens a fresh encoder. */
			sender_close(s);
			line_capture_reject(img->user_priv);
			picture->size = 0;
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
		force = TRUE;
	}
	if (img->user_priv) {
		line_capture_reject(img->user_priv);
		picture->size = 0;
		return TRUE;
	}
	int flags = force ? VPX_EFLAG_FORCE_KF : 0;
	gboolean high = s->layered && s->next_enhancement && !force;

	if (s->layered) {
		flags |= VP8_EFLAG_NO_REF_GF | VP8_EFLAG_NO_REF_ARF |
			 VP8_EFLAG_NO_UPD_GF | VP8_EFLAG_NO_UPD_ARF;
		if (high)
			flags |= VP8_EFLAG_NO_UPD_LAST |
				 VP8_EFLAG_NO_UPD_ENTROPY;
		if (vpx_codec_control(&s->sw, VP8E_SET_TEMPORAL_LAYER_ID,
				      (int)high))
			return FALSE;
	}
	if (vpx_codec_encode(&s->sw, img, pts, 90000 / s->fps, flags,
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
		picture->enhancement =
			high && !picture->key &&
			(pkt->data.frame.flags & VPX_FRAME_IS_DROPPABLE);
		s->next_enhancement = !picture->enhancement;
		sender_emitted(s);
		break;
	}
	return TRUE;
}

/*
 * An encoder thread, so a 10-45 ms HD encode never holds up the call
 * thread, which also receives and plays audio and captures the
 * microphone. One picture at a time: a camera frame that arrives while the
 * thread is busy is skipped, as the call thread used to fall behind
 * anyway. Packetizing, sequence numbers and the retransmission history
 * stay on the call thread, which collects the result when the eventfd
 * fires. While a picture is queued or encoding, only this thread touches
 * the sender's encoder; the call thread waits (encoder_settle()) before
 * it reconfigures or closes it.
 */
enum encode_state {
	ENCODE_IDLE,
	ENCODE_QUEUED,
	ENCODE_RUNNING,
	ENCODE_DONE,
	ENCODE_QUIT,
};

struct encode_thread {
	GThread *thread;
	GMutex lock;
	GCond cond;
	int wake;
	enum encode_state state;
	struct vp8_sender *sender;
	/* Nonzero for an immutable PipeWire presentation handed to this worker. */
	guint screen_kbps, screen_fps;
	GBytes *capture;
	/* The picture to encode: a private copy of the frame's planes. */
	vpx_image_t img;
	guint8 *pixels;
	gsize pixels_size;
	guint64 pts;
	gint64 captured;
	gboolean force;
	/* The result. */
	gboolean ok, key, enhancement;
	guint8 *out;
	gsize out_size, out_capacity;
	guint delay_ms;
};

static gpointer encode_main(gpointer data)
{
	struct encode_thread *t = data;

	line_tune_thread(LINE_THREAD_MEDIA);
	g_mutex_lock(&t->lock);
	for (;;) {
		while (t->state != ENCODE_QUEUED && t->state != ENCODE_QUIT)
			g_cond_wait(&t->cond, &t->lock);
		if (t->state == ENCODE_QUIT)
			break;
		t->state = ENCODE_RUNNING;
		g_mutex_unlock(&t->lock);

		struct vp8_picture picture = { 0 };
		gboolean force = t->force, ok = TRUE;

		if (t->screen_kbps &&
		    (!t->sender->ready || t->sender->width != t->img.w ||
		     t->sender->height != t->img.h)) {
			ok = sender_open(t->sender, t->img.w, t->img.h,
					 t->screen_kbps, t->screen_fps,
					 VP8_SCREEN_PDTP);
			force = TRUE;
		}
		if (ok)
			ok = sender_encode(t->sender, &t->img, t->pts, force,
					   &picture);
		g_clear_pointer(&t->capture, g_bytes_unref);

		if (ok && picture.size > t->out_capacity) {
			t->out_capacity = picture.size;
			t->out = g_realloc(t->out, t->out_capacity);
		}
		if (ok && picture.size)
			memcpy(t->out, picture.data, picture.size);
		g_mutex_lock(&t->lock);
		t->ok = ok;
		t->key = picture.key;
		t->enhancement = picture.enhancement;
		t->out_size = ok ? picture.size : 0;
		/* From capture, including any wait behind the last picture. */
		t->delay_ms =
			CLAMP((g_get_monotonic_time() - t->captured) / 1000, 0,
			      G_MAXUINT16);
		if (t->state == ENCODE_RUNNING)
			t->state = ENCODE_DONE;
		while (eventfd_write(t->wake, 1) < 0 && errno == EINTR)
			;
		g_cond_broadcast(&t->cond);
	}
	g_mutex_unlock(&t->lock);
	return NULL;
}

static struct encode_thread *encoder_start(struct vp8_sender *sender)
{
	struct encode_thread *t = g_new0(struct encode_thread, 1);

	t->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (t->wake < 0) {
		g_free(t);
		return NULL;
	}
	g_mutex_init(&t->lock);
	g_cond_init(&t->cond);
	t->sender = sender;
	t->thread = g_thread_try_new("line-encode", encode_main, t, NULL);
	if (!t->thread) {
		close(t->wake);
		g_mutex_clear(&t->lock);
		g_cond_clear(&t->cond);
		g_free(t);
		return NULL;
	}
	return t;
}

/* Waits for a queued or running picture and drops it. */
static void encoder_settle(struct encode_thread *t)
{
	eventfd_t n;

	if (!t)
		return;
	g_mutex_lock(&t->lock);
	while (t->state == ENCODE_QUEUED || t->state == ENCODE_RUNNING)
		g_cond_wait(&t->cond, &t->lock);
	t->state = ENCODE_IDLE;
	g_mutex_unlock(&t->lock);
	(void)eventfd_read(t->wake, &n);
}

static void encoder_stop(struct encode_thread *t)
{
	if (!t)
		return;
	encoder_settle(t);
	g_mutex_lock(&t->lock);
	t->state = ENCODE_QUIT;
	g_cond_broadcast(&t->cond);
	g_mutex_unlock(&t->lock);
	g_thread_join(t->thread);
	close(t->wake);
	g_mutex_clear(&t->lock);
	g_cond_clear(&t->cond);
	g_free(t->pixels);
	g_free(t->out);
	g_free(t);
}

static void encoder_queue(struct encode_thread *t, guint64 pts, gint64 captured,
			  gboolean force)
{
	g_mutex_lock(&t->lock);
	t->pts = pts;
	t->captured = captured;
	t->force = force;
	t->state = ENCODE_QUEUED;
	g_cond_signal(&t->cond);
	g_mutex_unlock(&t->lock);
}

/* Copies the planes, so the camera may reuse its buffer at once. */
static void encoder_submit(struct encode_thread *t, const vpx_image_t *img,
			   guint64 pts, gint64 captured, gboolean force)
{
	guint cw = (img->d_w + 1) / 2, ch = (img->d_h + 1) / 2;
	gsize luma = (gsize)img->d_w * img->d_h, chroma = (gsize)cw * ch;
	gsize need = luma + 2 * chroma;

	if (img->user_priv) {
		t->capture = g_bytes_ref(img->user_priv);
		t->img = *img;
		encoder_queue(t, pts, captured, force);
		return;
	}
	if (need > t->pixels_size) {
		t->pixels = g_realloc(t->pixels, need);
		t->pixels_size = need;
	}
	t->img = *img;
	t->img.img_data = NULL;
	t->img.img_data_owner = 0;
	t->img.self_allocd = 0;
	t->img.planes[0] = t->pixels;
	t->img.planes[1] = t->pixels + luma;
	t->img.planes[2] = t->pixels + luma + chroma;
	t->img.stride[0] = img->d_w;
	t->img.stride[1] = t->img.stride[2] = cw;
	for (int p = 0; p < 3; p++) {
		guint w = p ? cw : img->d_w, h = p ? ch : img->d_h;

		for (guint y = 0; y < h; y++)
			memcpy(t->img.planes[p] + (gsize)y * t->img.stride[p],
			       img->planes[p] + (gsize)y * img->stride[p], w);
	}
	encoder_queue(t, pts, captured, force);
}

struct call_video {
	srtp_t tx, rx;
	struct call_fec *fec;
	struct vp8_sender sender;
	struct encode_thread *encoder;
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
	guint8 tx_tl0, base_tl0;
	guint16 base_picture;
	struct video_payload temporal_picture;
	gboolean have_base;
	guint8 pt, capture_source, capture_generation, group_profile;
	enum call_video_rtp_extension tx_extension;
	gint64 epoch, last_encoded, next_layer_frame;
	gint64 group_fir_at;
	enum video_format framing, rx_format;
	enum video_source picture_source, displayed_source;
	gboolean have_ts, have_last, have_previous_end, finished, group_rx;
	gboolean group_rx_trace, picture_disposable;
	gsize received, expected;
	guint32 extended_size;
	unsigned int fragments, authenticated;

	/* Loss recovery and congestion control. */
	struct rx_packet *rx_ring;
	guint16 rx_next, rx_high;
	guint rx_held;
	gboolean rx_started, rtx_answered;
	gint64 reorder_us, min_rtt_us, rtx_rtt_us, next_fir, fir_interval, now;
	/* Kernel receive time of the packet being handled, or 0 for now. */
	gint64 rx_arrival;
	gint64 last_key_at, key_due, rtx_window;
	guint source_w, source_h;
	guint rtx_bytes;
	struct tx_packet *history;
	struct rate_control rc;
	struct local_queue lq;
	gboolean rr_seen;
	struct remb_estimate remb;
	/* Media SSRC receiver report counts authenticated on-wire originals. */
	struct call_reception_report rr;
	struct {
		guint requests, requested, retransmitted, gave_up, late;
		guint fec_sent, fec_recovered;
		guint firs, rembs, rate_changes, scaled;
	} recovery;

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
	v->picture_disposable = FALSE;
	v->temporal_picture.temporal = FALSE;
	v->picture_source = VIDEO_SOURCE_UNKNOWN;
	v->have_last = FALSE;
}

void call_video_close(struct call_video *v)
{
	if (!v)
		return;
	clear_picture(v);
	for (guint i = 0; v->rx_ring && i < RX_SLOTS; i++)
		g_clear_pointer(&v->rx_ring[i].payload, g_bytes_unref);
	g_free(v->rx_ring);
	g_free(v->history);
	call_fec_free(v->fec);
	encoder_stop(v->encoder);
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
		   !(v->dec = call_vp8_open(threads, FALSE))))
		goto failed;
	v->need_key = rx != NULL;
	if (rx)
		v->rx_ring = g_new0(struct rx_packet, RX_SLOTS);
	if (tx)
		v->history = g_new0(struct tx_packet, TX_HISTORY);
	v->reorder_us = 30000;
	v->fir_interval = KEY_SPACING_US;

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

gboolean call_video_set_fec(struct call_video *v, guint8 pt, guint32 tx_repair,
			    guint32 rx_repair, const guint8 tx_key[30],
			    const guint8 rx_key[30])
{
	if (!v || !v->tx || !v->rx || v->group_tx || v->group_rx || pt == v->pt)
		return FALSE;
	if (!v->fec)
		v->fec = call_fec_new(pt, v->tx_ssrc, v->rx_ssrc, tx_repair,
				      rx_repair, tx_key, rx_key);
	return call_fec_same(v->fec, pt, tx_repair, rx_repair);
}

gboolean call_video_is_fec(const struct call_video *v, const guint8 *packet,
			   gsize size)
{
	return v && call_fec_matches(v->fec, packet, size);
}

struct call_video *call_video_open_tx(const guint8 key[30], guint8 pt,
				      guint32 ssrc, guint kbps, guint fps,
				      GError **e)
{
	return open_video(key, NULL, pt, ssrc, 0, kbps, fps, e);
}

void call_video_set_temporal_layers(struct call_video *v, guint layers)
{
	if (v && !v->group_tx && !v->group_rx && !v->sender.ready &&
	    !v->have_ts)
		v->sender.layered = layers == 2;
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
	 * video. The published SVC layer has its own rate budget, whether it
	 * carries the camera or a shared screen.
	 */
	if (v->group_profile) {
		static const guint max_kbps[] = { 150, 300, 800, 3000 };
		guint layer = resolution_class(pixels);

		if (layer < G_N_ELEMENTS(max_kbps))
			kbps = MIN(kbps, max_kbps[layer]);
		if (!layer)
			fps = MIN(fps, 15u);
		/* Text stays sharper with more bits for each picture. */
		if (screen)
			fps = MIN(fps, kbps < 1200 ? 12u :
				       kbps < 2400 ? 20u :
						     30u);
	}
	/*
	 * Feedback can lower the rate below the negotiated ceiling. A group
	 * screen share keeps the HD layer's rate so shared text stays sharp.
	 */
	if (v->rc.target && !(v->group_tx && screen))
		kbps = MAX(MIN(kbps, v->rc.target), (guint)MIN_KBPS);
	/* C = new VP8 bytes + repair bytes. Allocate C(1 - alpha) to
	 * the encoder, with alpha measured on this uplink. Reliable PDTP
	 * uses a different transport and accounts for its own repairs.
	 */
	if ((v->fec || v->rc.request_fraction >= 0.02) &&
	    !(v->group_tx && screen)) {
		double share = MAX(call_fec_share(v->fec), v->rc.repair_share);

		kbps = MAX((guint)(kbps * (1.0 - share)), (guint)MIN_KBPS);
		if (v->rc.repair_kbps)
			kbps = MAX(MIN(kbps, v->rc.repair_kbps),
				   (guint)MIN_KBPS);
	}
	if (!screen && !v->group_tx)
		fps = MIN(fps, kbps < 120 ? 10u : kbps < 250 ? 15u : fps);
	if (s->ready && w == s->width && h == s->height && kbps == s->kbps &&
	    fps == s->fps)
		return TRUE;
	if (s->ready && w == s->width && h == s->height && fps == s->fps &&
	    kbps != s->kbps) {
		/* Hardware encoders restart with a keyframe, so they follow
		 * only larger changes.
		 */
		if (s->hw && (guint)ABS((int)kbps - (int)s->kbps) * 4 < s->kbps)
			return TRUE;
		if (sender_set_rate(s, kbps)) {
			v->recovery.rate_changes++;
			return TRUE;
		}
	}
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

/* Picture sizes for rate adaptation, in eighths of the camera's size. */
static const guint scale_eighths[] = { 8, 6, 4, 3, 2 };

static gboolean encoder_image(struct call_video *v, vpx_image_t *img)
{
	guint w = img->w, h = img->h;

	v->source_w = w;
	v->source_h = h;
	if (!v->group_tx) {
		guint e = scale_eighths[v->rc.scale];

		if (e == 8 || v->screen_kbps)
			return TRUE;
		w = MAX(16u, w * e / 8) & ~1u;
		h = MAX(16u, h * e / 8) & ~1u;
	} else if (v->requested_resolution >= 0) {
		guint source = resolution_class(w * h);
		guint target = v->requested_resolution;

		/*
		 * Grid tiles request small layers, which make shared text
		 * unreadable. The relay still forwards the HD layer and LINE
		 * Android displays it, so a shared screen never goes below it.
		 */
		if (v->screen_kbps)
			target = MAX(target, (guint)GROUP_SCREEN_CLASS);
		/*
		 * Fill the requested class with the source's shape. LINE
		 * Android displays any size within the class, so a grid tile
		 * gets 554x310 from 720p instead of the 320x180 that halving
		 * each dimension per class would give.
		 */
		if (target < source) {
			guint limit = resolution_pixels[target];
			double ratio = sqrt((double)limit / (w * h));

			w = MAX(2u, (guint)(w * ratio) & ~1u);
			h = MAX(2u, (guint)(h * ratio) & ~1u);
			if (w * h > limit) {
				if (w >= h)
					w -= 2;
				else
					h -= 2;
			}
		}
	}
	if (w == img->w && h == img->h && img->stride[1] == img->stride[2]) {
		/* libvpx accepts external planes with a shared chroma stride. */
		g_clear_pointer(&v->scaled, vpx_img_free);
		return TRUE;
	}

	if (img->user_priv)
		return FALSE;
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
		/*
		 * LINE Android leaves a group participant's tile on the
		 * avatar for any source other than a camera, so a shared
		 * screen is published as camera video. The generation still
		 * marks each switch between webcam and desktop capture.
		 */
		p[pos++] = 2;
		p[pos++] = EVS3_SOURCE_CAMERA;
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
			     gboolean enhancement, guint16 delay_ms,
			     call_video_send_fn send, gpointer data)
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
	guint8 source = v->screen_kbps != 0; /* Local webcam 0, desktop 1. */

	if (size > FRAME_LIMIT) {
		v->sender.force_key = TRUE;
		return TRUE;
	}
	/* Count source changes on published pictures, including the first. */
	if (v->group_tx && v->capture_source != source) {
		v->capture_source = source;
		v->capture_generation++;
	}
	if (v->sender.layered && !enhancement)
		v->tx_tl0++;
	for (gsize pos = 0; pos < size;) {
		guint8 packet[VIDEO_MTU + SRTP_MAX_TRAILER_LEN];
		gsize extension = base_extension;

		if (!pos && key && v->tx_extension == CALL_VIDEO_RTP_EXT_VED)
			extension += EVS3_VED_TARGET_BR_SIZE;
		gsize chunk =
			VIDEO_MTU - RTP_HEADER - extension - header_max - 10;
		if (v->fec)
			chunk -= CALL_FEC_OVERHEAD;
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
		if (v->sender.layered) {
			guint8 *p = packet + header;

			/* RFC 7741: X,N,S / I,L,T / PictureID / TL0PICIDX / TID,Y. */
			p[0] = 0x80 | (enhancement ? 0x20 : 0) |
			       (!pos ? 0x10 : 0);
			p[1] = 0xe0;
			write16(p + 2, 0x8000 | (id & 0x7fff));
			p[4] = v->tx_tl0;
			p[5] = enhancement ? 0x60 : 0;
			header += 6;
		} else {
			header += write_descriptor(v, packet + header, id, !pos,
						   last, key, size, delay_ms);
		}
		memcpy(packet + header, picture + pos, len);
		n = header + len;
		guint8 repair[VIDEO_MTU + SRTP_MAX_TRAILER_LEN];
		gsize repair_size = 0;

		if (v->fec)
			repair_size = call_fec_encode(v->fec, packet, n,
						      key && last, v->now,
						      v->sender.fps, repair,
						      sizeof(repair));
		if (srtp_protect(v->tx, packet, &n) != srtp_err_status_ok)
			return FALSE;
		if (v->history && (gsize)n <= sizeof(v->history->data)) {
			guint16 seq = v->seq - 1;
			struct tx_packet *h = &v->history[seq % TX_HISTORY];

			h->seq = seq;
			h->size = n;
			h->sent = v->now;
			h->resent = 0;
			h->resends = 0;
			h->requested = FALSE;
			memcpy(h->data, packet, n);
		}
		v->rc.sent++;
		v->rc.original_bytes += n;
		v->lq.sent += n;
		if (!send(packet, n, data))
			return FALSE;
		if (repair_size && send(repair, repair_size, data)) {
			v->rc.repair_bytes += repair_size;
			v->lq.sent += repair_size;
			v->recovery.fec_sent++;
		}
		pos += len;
	}
	if (key)
		v->last_key_at = v->now;
	return TRUE;
}

/* The rate the current source may use before feedback lowers it. */
static guint rate_ceiling(const struct call_video *v)
{
	return v->screen_kbps ? v->screen_kbps : v->kbps;
}

static guint rate_current(const struct call_video *v)
{
	guint ceiling = rate_ceiling(v);

	return v->rc.target ? MIN(v->rc.target, ceiling) : ceiling;
}

static void rate_set(struct call_video *v, guint kbps, gint64 now)
{
	guint ceiling = rate_ceiling(v);

	(void)now;
	kbps = MAX(kbps, (guint)MIN_KBPS);
	/* At the ceiling with no REMB limit, feedback no longer applies. */
	v->rc.target = kbps >= ceiling ? 0 : kbps;
}

/*
 * Choose the camera picture size for the rate, WebRTC-style: step down
 * by a quarter when each pixel gets too few bits, step up again only
 * when the larger size would get clearly more, after a longer wait.
 */
static void rate_scale(struct call_video *v, gint64 now)
{
	struct rate_control *rc = &v->rc;
	guint kbps = rate_current(v);
	guint fps = MAX(1u, MIN(v->fps, kbps < 120 ? 10u :
					kbps < 250 ? 15u :
						     v->fps));
	double pixels = (double)v->source_w * v->source_h;

	if (v->group_tx || v->screen_kbps || !pixels)
		return;
	double bpp = 0;
	guint e = scale_eighths[rc->scale];

	bpp = kbps * 1000.0 / (pixels * e * e / 64 * fps);
	if (bpp < 0.05 && rc->scale + 1 < G_N_ELEMENTS(scale_eighths) &&
	    now - rc->scaled_at >= 2 * G_USEC_PER_SEC) {
		rc->scale++;
		rc->scaled_at = now;
		v->recovery.scaled++;
		return;
	}
	if (!rc->scale)
		return;
	e = scale_eighths[rc->scale - 1];
	bpp = kbps * 1000.0 / (pixels * e * e / 64 * fps);
	if (bpp > 0.09 && now - rc->scaled_at >= 4 * G_USEC_PER_SEC) {
		rc->scale--;
		rc->scaled_at = now;
		v->recovery.scaled++;
	}
}

/*
 * Loss-based control after Google Congestion Control: above 10% loss
 * cut by half the loss rate, below 2% grow 8% a second, in between hold.
 * Windows of at least half a second and 20 packets.
 */
static void rate_update(struct call_video *v, gint64 now)
{
	struct rate_control *rc = &v->rc;

	if (!v->tx)
		return;
	if (!rc->window_start) {
		rc->window_start = now;
		return;
	}
	gint64 span = now - rc->window_start;

	if (span < G_USEC_PER_SEC / 2 ||
	    (rc->sent < 20 && span < 2 * G_USEC_PER_SEC))
		return;
	double loss = rc->sent ? (double)rc->lost / rc->sent : 0;
	guint current = rate_current(v), next = current;

	/* Receiver reports separate real losses from reordering. */
	loss = v->rr_seen ? rc->report_loss :
			    MAX(MIN(loss, 1.0), rc->report_loss);
	/* A peer that only asks for keyframes still signals a bad path. */
	if (rc->firs > 1 && !rc->feedback)
		loss = MAX(loss, 0.12);
	call_fec_loss(v->fec, loss, rc->lost, rc->adjacent);
	gboolean remb = rc->remb && now - rc->remb_at < 5 * G_USEC_PER_SEC / 2;

	/* One loss cut a second: reports lag the queue they describe. */
	if (loss > 0.10 && now - rc->decreased_at >= G_USEC_PER_SEC) {
		next = current * (1 - loss / 2);
		if (remb)
			next = MAX(next, rc->remb * 7 / 10);
		rc->decreased_at = now;
	} else if (loss < 0.02 && rc->target && !v->lq.high_since) {
		/* Recover faster once the path has been calm for a while. */
		double growth = now - rc->decreased_at >= 3 * G_USEC_PER_SEC ?
					0.25 :
					0.10;

		next = current +
		       MAX(current * growth, 8.0) * span / G_USEC_PER_SEC;
	}
	if (remb)
		next = MIN(next, rc->remb);
	if (next != current)
		rate_set(v, next, now);
	if (rc->original_bytes) {
		guint64 wire_bytes = rc->original_bytes + rc->repair_bytes;

		rc->repair_share = (double)rc->repair_bytes / wire_bytes;
	}
	/* Keep the expected unrepaired-picture rate below 1% if retries
	 * are independent. The receiver holds gaps for a request
	 * and one repeat. After three attempts a fragment is missing with
	 * probability p^3. For n fragments
	 * per picture the delivery probability is (1-p^3)^n. Reports
	 * retain one hundred prior observations (one per percentage point)
	 * to prevent individual NACKs from reconfiguring the encoder.
	 */
	if (rc->sent) {
		rc->request_fraction =
			(100 * rc->request_fraction + MIN(rc->lost, rc->sent)) /
			(100 + rc->sent);
		double p2 = rc->request_fraction * rc->request_fraction;
		double max_packets =
			p2 ? MAX(1.0, log(0.99) / log1p(-p2 *
							rc->request_fraction)) :
			     G_MAXUINT;
		double limit = max_packets * (VIDEO_MTU - RTP_HEADER) * 8 *
			       v->fps / 1000;

		rc->repair_kbps = limit < rate_ceiling(v) ? (guint)limit : 0;
	}
	rc->original_bytes = rc->repair_bytes = 0;
	rc->window_start = now;
	rc->sent = rc->lost = rc->adjacent = rc->firs = 0;
	rc->report_loss = 0;
	rate_scale(v, now);
}

void call_video_local_queue(struct call_video *v, guint64 held_bytes,
			    gint64 now)
{
	struct local_queue *q = v ? &v->lq : NULL;

	if (!q || !v->tx)
		return;
	/*
	 * The kernel counts each datagram's whole buffer, about twice the
	 * payload of a full video packet, so this is a coarse delay.
	 */
	guint kbps = MAX(rate_current(v), (guint)MIN_KBPS);
	gint64 elapsed = q->at ? now - q->at : 0;

	/*
	 * What the uplink really carried since the last sample: what was
	 * sent, less what the queue grew by. The kernel measures the
	 * radio's rate without any feedback from the peer.
	 */
	if (elapsed >= 20000 && elapsed <= G_USEC_PER_SEC) {
		double grew = ((double)held_bytes - (double)q->held) / 2;
		double drained = MAX((double)q->sent - grew, 0.0);
		double rate = drained * 8000.0 / elapsed;

		q->drain_kbps =
			q->drain_kbps ? 0.5 * q->drain_kbps + 0.5 * rate : rate;
	}
	q->held = held_bytes;
	q->sent = 0;
	/* Time to drain at the rate the uplink really carries. */
	if (q->drain_kbps >= MIN_KBPS / 2 && held_bytes)
		kbps = MIN(kbps, (guint)q->drain_kbps);
	q->queue_ms = MIN(held_bytes * 8 / (2 * (guint64)kbps), G_MAXUINT);
	q->at = now;
	if (q->queue_ms < LOCAL_QUEUE_HIGH_MS) {
		q->high_since = 0;
		return;
	}
	if (!q->high_since)
		q->high_since = now;
	/* Again while it still grows; one cut takes a while to show. */
	if (now - q->high_since < LOCAL_QUEUE_HOLD_MS * 1000 ||
	    (q->cut_at && (now - q->cut_at < LOCAL_QUEUE_CUT_GAP_MS * 1000 ||
			   q->queue_ms <= q->cut_queue_ms)))
		return;
	/*
	 * Below what drains, so the queue empties: 85% of it, or 70% when
	 * a long queue must also clear. Never more than half at once.
	 */
	guint next = rate_current(v) * 3 / 4;

	if (q->drain_kbps > 0)
		next = MIN(next,
			   (guint)(q->drain_kbps *
				   (q->queue_ms > LOCAL_QUEUE_DEEP_MS ? 0.70 :
									0.85)));
	rate_set(v, MAX(next, rate_current(v) / 2), now);
	v->rc.decreased_at = now;
	q->cut_at = now;
	q->cut_queue_ms = q->queue_ms;
	q->cuts++;
	rate_scale(v, now);
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
	encoder_settle(v->encoder);
	sender_close(&v->sender);
	v->screen_kbps = kbps;
	v->screen_fps = fps;
	v->sender.force_key = TRUE;
}

gboolean call_video_encode_async(struct call_video *v)
{
	if (!v || !v->tx)
		return FALSE;
	if (!v->encoder)
		v->encoder = encoder_start(&v->sender);
	return v->encoder != NULL;
}

int call_video_encoder_fd(const struct call_video *v)
{
	return v && v->encoder ? v->encoder->wake : -1;
}

gboolean call_video_encoded(struct call_video *v, gint64 now,
			    call_video_send_fn send, gpointer data)
{
	struct encode_thread *t = v ? v->encoder : NULL;
	eventfd_t n;

	if (!t || !send)
		return TRUE;
	(void)eventfd_read(t->wake, &n);
	g_mutex_lock(&t->lock);
	if (t->state != ENCODE_DONE) {
		g_mutex_unlock(&t->lock);
		return TRUE;
	}
	gboolean ok = t->ok, key = t->key, forced = t->force;
	gsize size = t->out_size;
	guint delay_ms = t->delay_ms;
	guint64 pts = t->pts;

	g_mutex_unlock(&t->lock);
	/* The buffers stay the call thread's until the next submit. */
	v->now = now;
	if (!size && forced)
		v->sender.force_key = TRUE;
	ok = ok &&
	     (!size || send_picture(v, v->ts_base + pts, t->out, size, key,
				    t->enhancement, delay_ms, send, data));
	g_mutex_lock(&t->lock);
	t->state = ENCODE_IDLE;
	g_mutex_unlock(&t->lock);
	return ok;
}

static gboolean encoder_busy(struct encode_thread *t)
{
	g_mutex_lock(&t->lock);
	gboolean busy = t->state != ENCODE_IDLE;

	g_mutex_unlock(&t->lock);
	return busy;
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
	/* A finished picture goes out first, then the encoder is free. */
	if (v->encoder && (!call_video_encoded(v, now, send, data) ||
			   encoder_busy(v->encoder))) {
		if (encoder_busy(v->encoder))
			return TRUE;
		goto failed;
	}
	v->now = now;
	if (v->key_due && now >= v->key_due) {
		v->key_due = 0;
		v->sender.force_key = TRUE;
	}
	rate_update(v, now);
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
	if (!sender_image(&img, frame) || !encoder_image(v, &img) ||
	    !configure_encoder(v, img.d_w, img.d_h)) {
		if (frame->capture_dma) {
			line_capture_reject(frame->capture_dma);
			return TRUE;
		}
		goto failed;
	}
	if (v->epoch < 0)
		v->epoch = now;
	pts = ((guint64)(now - v->epoch) / 100) * 9 +
	      ((guint64)(now - v->epoch) % 100) * 9 / 100;
	if (v->encoder) {
		encoder_submit(v->encoder, &img, pts, now, v->sender.force_key);
		v->sender.force_key = FALSE;
	} else {
		if (!sender_encode(&v->sender, &img, pts, v->sender.force_key,
				   &picture))
			goto failed;
		if (picture.size)
			v->sender.force_key = FALSE;
	}
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
			  picture.key, picture.enhancement, picture.delay_ms,
			  send, data))
		goto failed;
	return TRUE;
failed:
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode or send the call video frame.");
	return FALSE;
}

static void need_video_key(struct call_video *v)
{
	v->need_key = TRUE;
	call_vp8_discontinuity(v->dec);
}

static gboolean video_decode_result(struct call_video *v, int shown)
{
	if (shown < 0) {
		v->stats.decode++;
		need_video_key(v);
		return FALSE;
	}
	v->need_key = FALSE;
	v->next_fir = 0;
	v->fir_interval =
		v->group_rx ? G_USEC_PER_SEC :
			      MAX((gint64)KEY_SPACING_US, 2 * v->rtx_rtt_us);
	v->stats.hidden += !shown;
	return TRUE;
}

void call_video_decoded(struct call_video *v, call_video_display_fn display,
			gpointer data)
{
	struct call_vp8_output out;

	if (!v || !call_vp8_take(v->dec, &out))
		return;
	if (out.shown > 0) {
		if (v->group_rx && out.tag != VIDEO_SOURCE_UNKNOWN)
			v->displayed_source = out.tag;
		display(&out.image, data);
	}
	video_decode_result(v, out.shown);
	call_vp8_release(v->dec);
}

static gboolean decode_picture(struct call_video *v, const guint8 *p, gsize n,
			       call_video_display_fn display, gpointer data)
{
	int shown;
	gboolean key = !(p[0] & 1);

	call_video_decoded(v, display, data);
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
	switch (call_vp8_submit(v->dec, p, n, key, v->rx_rotation,
				v->picture_source)) {
	case CALL_VP8_QUEUED:
		/* Keep subsequent deltas in order behind the queued key. */
		v->need_key = FALSE;
		return TRUE;
	case CALL_VP8_FULL:
		if (v->sender.layered && v->temporal_picture.enhancement)
			return TRUE;
		need_video_key(v);
		return FALSE;
	case CALL_VP8_INLINE:
		break;
	}
	enum video_source previous = v->displayed_source;

	if (v->group_rx && v->picture_source != VIDEO_SOURCE_UNKNOWN)
		v->displayed_source = v->picture_source;
	shown = call_vp8_decode(v->dec, p, n, key, v->rx_rotation, display,
				data);
	if (shown <= 0)
		v->displayed_source = previous;
	return video_decode_result(v, shown);
}

gboolean call_video_frame_is_screen(const struct call_video *v)
{
	return v && v->displayed_source == VIDEO_SOURCE_DISPLAY;
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
	enum video_source source = VIDEO_SOURCE_UNKNOWN;
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
		/* PLANET user data carries the capture generation and source. */
		if (type == 0x80 && len == 4 && p[pos] == 1 && p[pos + 2] == 2)
			source = p[pos + 3] == 1 ? VIDEO_SOURCE_DISPLAY :
						   VIDEO_SOURCE_CAMERA;
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
				       .extended_size = extended_size,
				       .source = source };
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
	/* Our negotiated two-layer stream uses an unpartitioned payload,
	 * a 15-bit picture ID and one independently disposable TID 1.
	 */
	if (pos == 6 && !(p[0] & 7) && p[1] == 0xe0 && (p[2] & 0x80) &&
	    p[5] == ((p[0] & 0x20) ? 0x60 : 0)) {
		out->temporal = TRUE;
		out->picture_id = read16(p + 2) & 0x7fff;
		out->tl0 = p[4];
		out->enhancement = !!(p[0] & 0x20);
	}
	return TRUE;
}

static gboolean follows_base(const struct call_video *v,
			     const struct video_payload *p)
{
	guint16 delta = (p->picture_id - v->base_picture) & 0x7fff;

	if (!p->temporal || !v->have_base || v->need_key)
		return FALSE;
	if (p->enhancement)
		return p->tl0 == v->base_tl0 && delta == 1;
	return p->tl0 == (guint8)(v->base_tl0 + 1) &&
	       (delta == 1 || delta == 2);
}

static gboolean same_layer(const struct call_video *v,
			   const struct video_payload *p)
{
	const struct video_payload *start = &v->temporal_picture;

	return !v->sender.layered || v->framing != VIDEO_RFC7741 ||
	       (p->temporal && start->picture_id == p->picture_id &&
		start->tl0 == p->tl0 && start->enhancement == p->enhancement);
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
	/* An intermediary can strip the answer. Permit legacy EVS3 in either
	 * direction, but never carry a reference chain across framing modes.
	 */
	if (v->sender.layered && v->rx_format != VIDEO_UNKNOWN &&
	    v->rx_format != v->framing) {
		v->have_base = FALSE;
		need_video_key(v);
	}
	if (v->sender.layered && v->temporal_picture.temporal) {
		if (!len ||
		    (!(picture[0] & 1) && v->temporal_picture.enhancement))
			return FALSE;
		if ((picture[0] & 1) &&
		    !follows_base(v, &v->temporal_picture)) {
			need_video_key(v);
			return TRUE;
		}
	} else if (v->have_previous_end &&
		   (guint16)(v->first - v->previous_end) != 1) {
		need_video_key(v);
	}
	v->previous_end = v->last;
	v->have_previous_end = TRUE;
	if (!decode_picture(v, picture, len, display, data))
		return FALSE;
	if (v->sender.layered && v->temporal_picture.temporal &&
	    !v->temporal_picture.enhancement && !v->need_key) {
		v->base_tl0 = v->temporal_picture.tl0;
		v->base_picture = v->temporal_picture.picture_id;
		v->have_base = TRUE;
	}
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
		    !same_layer(v, &payload) || payload.start != (i == 0) ||
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

		if (v->framing == VIDEO_RFC7741 && !(p[0] & 0x20))
			v->picture_disposable = FALSE;
		if (!describe(v->framing, p, size, part->marker, &payload) ||
		    !same_layer(v, &payload) || payload.start ||
		    (payload.end && !mark_end(v, part->seq)))
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
	if ((!evs && !rfc) ||
	    (v->sender.layered && !evs3.start && !vp8.temporal &&
	     ((rfc && vp8.start) ||
	      (ts == v->rx_ts && v->framing == VIDEO_RFC7741)))) {
		v->stats.unknown++;
		return FALSE;
	}
	if (!v->have_ts || (gint32)(ts - v->rx_ts) > 0) {
		if (v->have_ts && !v->finished) {
			v->stats.incomplete++;
			/* RFC 7741 N: every missing packet lies strictly
			 * inside one non-reference picture with known ends.
			 * The decoder still holds the preceding reference.
			 */
			if (v->picture_disposable && v->have_previous_end &&
			    v->have_last && rfc && vp8.start && !evs3.start &&
			    (guint16)(v->first - v->previous_end) == 1 &&
			    (guint16)(seq - v->last) == 1)
				v->previous_end = v->last;
			else if (!(v->sender.layered && vp8.start &&
				   follows_base(v, &vp8) &&
				   (v->framing == VIDEO_UNKNOWN ||
				    v->temporal_picture.enhancement)))
				need_video_key(v);
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
		v->picture_disposable = !evs3.start && (p[0] & 0x20);
		payload = evs3.start ? evs3 : vp8;
		v->temporal_picture = payload;
		v->rx_picture = evs3.start ? read16(p + 1) : 0;
		v->expected = payload.expected;
		v->extended_size = payload.extended_size;
		v->rx_rotation = payload.rotation;
		v->picture_source = payload.source;
		v->first = seq;
		if (!find_early_end(v))
			goto invalid;
	} else {
		payload = v->framing == VIDEO_RFC7741 ? vp8 : evs3;
		if (v->framing != VIDEO_UNKNOWN &&
		    (!payload.offset || payload.start ||
		     !same_layer(v, &payload)))
			return FALSE;
	}
	if (v->framing == VIDEO_RFC7741 && !(p[0] & 0x20))
		v->picture_disposable = FALSE;
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
	need_video_key(v);
	v->finished = TRUE;
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

static gboolean feedback_enabled(void)
{
	return g_strcmp0(g_getenv("LINE_CALL_FEEDBACK"), "0") != 0;
}

/* A gap shorter than the usual reordering is not worth a request. */
static gint64 rtx_grace(const struct call_video *v)
{
	return CLAMP(v->reorder_us * 3 / 4, 5000, 100000);
}

/* How long a missing packet holds later ones: reordering, or a resend. */
static gint64 rx_hold(const struct call_video *v)
{
	gint64 hold = v->reorder_us;

	if (v->rtx_answered)
		/* Room for a request and one repeat. */
		hold = MAX(hold,
			   MIN(rtx_grace(v) + v->rtx_rtt_us * 5 / 2 + 30000,
			       500000));
	return hold;
}

gint64 call_video_min_rtt(const struct call_video *v)
{
	return v ? v->min_rtt_us : 0;
}

static void rtx_sample(struct call_video *v, gint64 sample)
{
	if (sample > 0 && (!v->min_rtt_us || sample < v->min_rtt_us))
		v->min_rtt_us = sample;
	sample = CLAMP(sample, 1000, (gint64)G_USEC_PER_SEC);
	v->rtx_answered = TRUE;
	v->recovery.retransmitted++;
	/* Follow increases at once and decreases slowly. */
	v->rtx_rtt_us = sample > v->rtx_rtt_us ?
				sample :
				v->rtx_rtt_us - (v->rtx_rtt_us - sample) / 32;
}

static void reorder_sample(struct call_video *v, gint64 sample)
{
	gint64 decayed = v->reorder_us - MAX(0, v->reorder_us - 20000) / 64;

	v->reorder_us =
		CLAMP(MAX(decayed, sample * 5 / 4 + 5000), 20000, 150000);
}

/*
 * Passes buffered packets on in sequence order, skipping expired gaps.
 * FALSE when picture assembly rejected any of them.
 */
static gboolean rx_release(struct call_video *v, gint64 now, gboolean flush,
			   call_video_display_fn display, gpointer data)
{
	gboolean ok = TRUE;

	while (v->rx_held) {
		struct rx_packet *r = &v->rx_ring[v->rx_next % RX_SLOTS];

		if (r->seq == v->rx_next && r->payload) {
			gsize n;
			GBytes *payload = g_steal_pointer(&r->payload);
			const guint8 *p = g_bytes_get_data(payload, &n);

			v->rx_held--;
			v->rx_next++;
			ok &= accept_fragment(v, p, n, r->seq, r->ts, r->marker,
					      display, data);
			g_bytes_unref(payload);
			continue;
		}
		if (!flush && r->seq == v->rx_next &&
		    now - r->missing_since < rx_hold(v))
			break;
		v->recovery.gave_up++;
		v->rx_next++;
	}
	return ok;
}

/*
 * Receive-side delay estimate in the manner of WebRTC's REMB: the delay
 * of each picture above the lowest of the last 15-30 s is
 * queueing at a bottleneck. Growing queues cut the estimate to 85% of
 * the measured arrival rate; short queues let it grow 8% a second.
 */
static void remb_packet(struct call_video *v, guint32 ts, gsize size,
			gboolean marker, gint64 now)
{
	struct remb_estimate *e = &v->remb;

	if (!e->started) {
		*e = (struct remb_estimate){ .started = TRUE,
					     .base_us = now,
					     .min_delay = G_MAXINT64,
					     .min_next = G_MAXINT64,
					     .min_reset =
						     now + 15 * G_USEC_PER_SEC,
					     .window_start = now,
					     .updated = now };
	}
	e->window_bytes += size;
	if (now - e->window_start >= G_USEC_PER_SEC / 2) {
		double rate =
			e->window_bytes * 8000.0 / (now - e->window_start);

		e->rate_kbps = e->rate_kbps ? (e->rate_kbps + rate) / 2 : rate;
		e->window_bytes = 0;
		e->window_start = now;
	}
	/* Bytes from late repairs still traversed the bottleneck. Their old
	 * capture timestamp must not enter the one-way delay gradient.
	 * Extend the 90 kHz clock a frame at a time so the transit estimate
	 * also stays continuous beyond half an RTP timestamp cycle.
	 */
	if (e->have_ts && (gint32)(ts - e->last_ts) < 0)
		return;
	if (!e->have_ts || ts != e->last_ts) {
		if (e->have_ts)
			e->media_ticks += (guint32)(ts - e->last_ts);
		e->have_ts = TRUE;
		e->last_ts = ts;
		e->picture_bytes = 0;
		e->timed_picture = FALSE;
	}
	e->picture_bytes += size;
	if (!marker || e->timed_picture)
		return;
	e->timed_picture = TRUE;
	gint64 media = (gint64)(e->media_ticks * 1000 / 90);
	/* Time each picture by its last packet. */
	gint64 own = e->rate_kbps ? e->picture_bytes * 8000.0 / e->rate_kbps :
				    0;
	gint64 delay = now - e->base_us - media;

	e->min_delay = MIN(e->min_delay, delay);
	e->min_next = MIN(e->min_next, delay);
	if (now >= e->min_reset) {
		e->min_delay = e->min_next;
		e->min_next = delay;
		e->min_reset = now + 15 * G_USEC_PER_SEC;
	}
	double previous = e->queue_ms;
	double dt = (now - e->updated) / 1e6;

	e->updated = now;
	/*
	 * Overuse uses the whole delay. Growth may ignore the picture's own
	 * crossing time, an upper bound since the arrival rate bounds the
	 * bottleneck from below, so a slow link can still recover.
	 */
	gint64 queued = delay - e->min_delay;

	e->queue_ms += (queued / 1000.0 - e->queue_ms) / 8;
	e->calm_ms += (MAX(0, queued - own) / 1000.0 - e->calm_ms) / 8;
	if (!e->rate_kbps)
		return;
	if (((e->queue_ms > 50 && e->queue_ms > previous) ||
	     e->queue_ms > 120) &&
	    now - e->decreased_at >= G_USEC_PER_SEC / 2) {
		/* A long queue needs a deeper cut to drain. */
		double drain = CLAMP(1 - (e->queue_ms - 50) / 400, 0.5, 1.0);
		double cut = 0.85 * e->rate_kbps * drain;

		e->estimate = e->estimate ? MIN(e->estimate, cut) : cut;
		e->estimate = MAX(e->estimate, (double)MIN_KBPS);
		e->decreased_at = now;
		e->send_now = TRUE;
	} else if (e->estimate && e->calm_ms < 30) {
		gboolean calm = now - e->decreased_at >= 3 * G_USEC_PER_SEC;
		double cap = 1.5 * e->rate_kbps + 50;

		e->estimate *= pow(calm ? 1.25 : 1.10, MIN(dt, 1.0));
		/* An estimate that no longer limits the sender is dropped. */
		if (e->estimate >= cap && calm)
			e->estimate = 0;
		else
			e->estimate = MIN(e->estimate, cap);
	}
}

/* Whether a fragment starts a VP8 keyframe in any supported framing. */
static gboolean starts_keyframe(const guint8 *p, gsize n, gboolean marker)
{
	struct video_payload out;

	if (parse_vp8(p, n, marker, &out) && out.start &&
	    vp8_key_at(p + out.offset, n - out.offset))
		return TRUE;
	for (int sized = 0; sized < 2; sized++) {
		if (!parse_evs3_framed(p, n, sized, &out) || !out.start)
			continue;
		struct video_payload wide = out, profiled = out;

		if (vp8_key_at(p + out.offset, n - out.offset) ||
		    (evs3_wide_size(p, n, &wide) &&
		     vp8_key_at(p + wide.offset, n - wide.offset)) ||
		    (evs3_profiled_size(p, n, &profiled) &&
		     vp8_key_at(p + profiled.offset, n - profiled.offset)))
			return TRUE;
	}
	return FALSE;
}

/*
 * Nothing before a keyframe matters to the decoder. When the held packets
 * ending at the marker seq form a whole keyframe, return its first seq.
 */
static gboolean held_keyframe(const struct call_video *v, guint16 end,
			      guint16 *start)
{
	const struct rx_packet *last = &v->rx_ring[end % RX_SLOTS];

	for (guint16 seq = end; (gint16)(seq - v->rx_next) >= 0; seq--) {
		const struct rx_packet *r = &v->rx_ring[seq % RX_SLOTS];
		gsize n;

		if (r->seq != seq || !r->payload || r->ts != last->ts)
			return FALSE;
		const guint8 *p = g_bytes_get_data(r->payload, &n);

		if (starts_keyframe(p, n, r->marker)) {
			*start = seq;
			return TRUE;
		}
	}
	return FALSE;
}

static void rx_init_missing(struct call_video *v, guint16 from, guint16 to,
			    gint64 now)
{
	for (guint16 seq = from; seq != to; seq++) {
		struct rx_packet *r = &v->rx_ring[seq % RX_SLOTS];

		g_clear_pointer(&r->payload, g_bytes_unref);
		*r = (struct rx_packet){ .seq = seq, .missing_since = now };
	}
}

/*
 * A small jitter buffer in front of picture assembly. Packets in order
 * pass straight through; after a gap, later packets wait for reordering
 * or a retransmission, at most rx_hold(), before assembly sees the loss.
 */
static gboolean reorder(struct call_video *v, const guint8 *p, gsize n,
			guint16 seq, guint32 ts, gboolean marker, gsize wire,
			call_video_display_fn display, gpointer data)
{
	gint64 now = v->rx_arrival ? v->rx_arrival : g_get_monotonic_time();
	struct rx_packet *r = &v->rx_ring[seq % RX_SLOTS];

	if (wire) {
		remb_packet(v, ts, wire, marker, now);
		call_rtcp_arrival(&v->rr, seq, ts, 90000, now);
	}
	if (!v->rx_started) {
		v->rx_started = TRUE;
		v->rx_next = seq;
		v->rx_high = seq - 1;
	}
	gint16 ahead = seq - v->rx_next;

	/* Far behind is a restarted sequence, not a late packet. */
	if (ahead < -RX_SLOTS) {
		(void)rx_release(v, now, TRUE, display, data);
		v->rx_next = seq;
		v->rx_high = seq - 1;
		ahead = 0;
	}
	if (ahead < 0) {
		/* A resend after the gap expired still measures the delay. */
		if (wire && r->seq == seq && !r->payload && r->requested_at) {
			rtx_sample(v, now - r->requested_at);
			r->requested_at = 0;
		}
		/* The picture still assembling takes fragments in any order. */
		if (v->have_ts && ts == v->rx_ts && !v->finished)
			return accept_fragment(v, p, n, seq, ts, marker,
					       display, data);
		v->recovery.late++;
		return FALSE;
	}
	if (ahead >= RX_SLOTS) {
		(void)rx_release(v, now, TRUE, display, data);
		v->rx_next = seq;
		v->rx_high = seq - 1;
		ahead = 0;
	}
	/* A confirmed next base does not depend on intervening enhancement
	 * fragments. Retire those holes before the NACK/reordering timer.
	 */
	if (ahead && v->sender.layered && (gint32)(ts - v->rx_ts) > 0) {
		struct video_payload next;

		if (parse_vp8(p, n, marker, &next) && next.start &&
		    !next.enhancement && follows_base(v, &next)) {
			while (v->rx_next != seq) {
				struct rx_packet *old =
					&v->rx_ring[v->rx_next % RX_SLOTS];

				if (old->seq == v->rx_next && old->payload) {
					g_clear_pointer(&old->payload,
							g_bytes_unref);
					v->rx_held--;
				}
				v->rx_next++;
			}
			if ((gint16)(seq - v->rx_high) > 0)
				v->rx_high = seq - 1;
			ahead = 0;
		}
	}
	if (!ahead && !v->rx_held) {
		v->rx_next = seq + 1;
		if ((gint16)(seq - v->rx_high) > 0)
			v->rx_high = seq;
		return accept_fragment(v, p, n, seq, ts, marker, display, data);
	}
	if ((gint16)(seq - v->rx_high) > 0) {
		rx_init_missing(v, v->rx_high + 1, seq, now);
		v->rx_high = seq;
	} else if (r->seq == seq && !r->payload) {
		/* Soon after a request it was reordering, not a resend. */
		if (wire && r->requested_at &&
		    now - r->requested_at >= v->rtx_rtt_us / 2) {
			rtx_sample(v, now - r->requested_at);
		} else if (wire) {
			reorder_sample(v, now - r->missing_since);
		}
	} else {
		return FALSE;
	}
	r->seq = seq;
	r->ts = ts;
	r->marker = marker;
	r->requested_at = 0;
	r->payload = g_bytes_new(p, n);
	v->rx_held++;
	guint16 key;
	gboolean ok = TRUE;

	if (marker && held_keyframe(v, seq, &key)) {
		/* Pass older packets on and stop waiting for older gaps. */
		while (v->rx_next != key) {
			struct rx_packet *old =
				&v->rx_ring[v->rx_next % RX_SLOTS];

			if (old->seq == v->rx_next && old->payload) {
				gsize size;
				GBytes *payload =
					g_steal_pointer(&old->payload);
				const guint8 *bytes =
					g_bytes_get_data(payload, &size);

				v->rx_held--;
				ok &= accept_fragment(v, bytes, size, old->seq,
						      old->ts, old->marker,
						      display, data);
				g_bytes_unref(payload);
			} else {
				v->recovery.gave_up++;
			}
			v->rx_next++;
		}
	}
	return rx_release(v, now, FALSE, display, data) && ok;
}

static gboolean receive_packet(struct call_video *v, const guint8 *packet,
			       gsize size, call_video_display_fn display,
			       gpointer data);

gboolean call_video_receive_at(struct call_video *v, const guint8 *packet,
			       gsize size, gint64 arrival,
			       call_video_display_fn display, gpointer data)
{
	if (!v)
		return FALSE;
	v->rx_arrival = MAX(arrival, 0);
	gboolean ok = receive_packet(v, packet, size, display, data);

	v->rx_arrival = 0;
	return ok;
}

gboolean call_video_receive(struct call_video *v, const guint8 *packet,
			    gsize size, call_video_display_fn display,
			    gpointer data)
{
	return call_video_receive_at(v, packet, size, 0, display, data);
}

struct fec_display {
	struct call_video *video;
	call_video_display_fn display;
	gpointer data;
};

static gboolean receive_plain(struct call_video *v, const guint8 *buf,
			      gsize plain, gsize wire,
			      call_video_display_fn display, gpointer data)
{
	gsize pos, end;

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
	return reorder(v, buf + pos, end - pos, read16(buf + 2),
		       read32(buf + 4), !!(buf[1] & 0x80), wire, display, data);
}

static void recovered_packet(const guint8 *p, gsize n, gpointer data)
{
	struct fec_display *d = data;

	d->video->recovery.fec_recovered++;
	receive_plain(d->video, p, n, 0, d->display, d->data);
}

static gboolean receive_packet(struct call_video *v, const guint8 *packet,
			       gsize size, call_video_display_fn display,
			       gpointer data)
{
	struct fec_display d = { v, display, data };
	guint8 buf[MAX_DATAGRAM];
	int plain = size;

	if (!v->rx || !packet || !display || size <= RTP_HEADER + 10 ||
	    size > sizeof(buf) || packet[0] >> 6 != 2)
		return FALSE;
	if (call_video_is_fec(v, packet, size)) {
		if (!call_fec_receive(v->fec, packet, size, v->pt,
				      recovered_packet, &d))
			return FALSE;
		v->authenticated++;
		return TRUE;
	}
	if ((packet[1] & 127) != v->pt || read32(packet + 8) != v->rx_ssrc)
		return FALSE;
	memcpy(buf, packet, size);
	if (srtp_unprotect(v->rx, buf, &plain) != srtp_err_status_ok)
		return FALSE;
	v->authenticated++;
	if (v->fec && call_fec_source(v->fec, buf, plain, v->pt)) {
		/* Count the late wire arrival once in the media RR. Its copy
		 * already reached the decoder through the repair stream.
		 */
		gint64 now = v->rx_arrival ? v->rx_arrival :
					     g_get_monotonic_time();

		remb_packet(v, read32(buf + 4), size, buf[1] & 128, now);
		call_rtcp_arrival(&v->rr, read16(buf + 2), read32(buf + 4),
				  90000, now);
		return TRUE;
	}
	gboolean ok = receive_plain(v, buf, plain, size, display, data);

	if (v->fec)
		call_fec_drain(v->fec, v->pt, recovered_packet, &d);
	return ok;
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

/* LINE's retransmission request, RTPFB format 5: a count and a list of
 * 16-bit sequence numbers, word aligned with RTCP padding.
 */
static gboolean send_rrr(struct call_video *tx, const struct call_video *rx,
			 const guint16 *list, guint count,
			 call_video_send_fn send, gpointer data)
{
	guint8 packet[16 + 2 * RRR_MAX + SRTP_MAX_TRAILER_LEN] = { 0 };
	gsize body = 14 + 2 * count, size = (body + 3) & ~(gsize)3;
	int n = size;

	if (!tx || !tx->tx || !count || count > RRR_MAX)
		return FALSE;
	packet[0] = 0x80 | LINE_RRR | (size != body ? 0x20 : 0);
	packet[1] = RTCP_RTPFB;
	write16(packet + 2, size / 4 - 1);
	write32(packet + 4, tx->tx_ssrc);
	write32(packet + 8, rx->rx_ssrc);
	write16(packet + 12, count);
	for (guint i = 0; i < count; i++)
		write16(packet + 14 + 2 * i, list[i]);
	if (size != body)
		packet[size - 1] = size - body;
	return srtp_protect_rtcp(tx->tx, packet, &n) == srtp_err_status_ok &&
	       send(packet, n, data);
}

/* Standard REMB (draft-alvestrand-rmcat-remb): PSFB format 15. */
static gboolean send_remb(struct call_video *tx, const struct call_video *rx,
			  guint64 bps, call_video_send_fn send, gpointer data)
{
	guint8 packet[24 + SRTP_MAX_TRAILER_LEN] = { 0 };
	guint exponent = 0;
	int n = 24;

	if (!tx || !tx->tx)
		return FALSE;
	while ((bps >> exponent) >= 1u << 18)
		exponent++;
	guint32 mantissa = bps >> exponent;

	packet[0] = 0x80 | RTCP_REMB;
	packet[1] = RTCP_PSFB;
	write16(packet + 2, 5);
	write32(packet + 4, tx->tx_ssrc);
	memcpy(packet + 12, "REMB", 4);
	packet[16] = 1;
	packet[17] = (exponent << 2) | (mantissa >> 16);
	write16(packet + 18, mantissa);
	write32(packet + 20, rx->rx_ssrc);
	return srtp_protect_rtcp(tx->tx, packet, &n) == srtp_err_status_ok &&
	       send(packet, n, data);
}

/* RFC 3550 receiver report. Its loss counts packets that neither arrived
 * nor were repaired; reordered packets are not lost.
 */
static gboolean send_rr(struct call_video *tx, struct call_video *rx,
			call_video_send_fn send, gpointer data)
{
	guint8 packet[32 + SRTP_MAX_TRAILER_LEN + 4];
	gsize size;

	if (!tx || !tx->tx)
		return FALSE;
	size = call_rtcp_report(&rx->rr, tx->tx, tx->tx_ssrc, rx->rx_ssrc,
				packet, sizeof(packet));
	return size && send(packet, size, data);
}

gint64 call_video_poll(struct call_video *rx, struct call_video *feedback,
		       gint64 now, gboolean keyframes, call_video_send_fn send,
		       gpointer data, call_video_display_fn display,
		       gpointer display_data)
{
	gint64 next = G_MAXINT64;

	if (!rx || !rx->rx || !display)
		return next;
	(void)rx_release(rx, now, FALSE, display, display_data);
	gboolean requests = !rx->group_rx && send && feedback_enabled();

	if (rx->rx_held && requests) {
		guint16 list[RRR_MAX];
		guint count = 0;
		gint64 hold = rx_hold(rx);
		gint64 grace = rtx_grace(rx);
		gint64 retry = MAX(rx->rtx_rtt_us * 5 / 4, 40000);

		for (guint16 seq = rx->rx_next;
		     seq != (guint16)(rx->rx_high + 1) && count < RRR_MAX;
		     seq++) {
			struct rx_packet *r = &rx->rx_ring[seq % RX_SLOTS];

			if (r->seq != seq || r->payload || r->requests >= 3 ||
			    now - r->missing_since >= hold)
				continue;
			gint64 due = r->requested_at ? r->requested_at + retry :
						       r->missing_since + grace;

			if (due > now) {
				next = MIN(next, due);
				continue;
			}
			list[count++] = seq;
			r->requested_at = now;
			r->requests++;
		}
		if (count && send_rrr(feedback, rx, list, count, send, data)) {
			rx->recovery.requests++;
			rx->recovery.requested += count;
		}
	}
	if (rx->rx_held) {
		const struct rx_packet *r =
			&rx->rx_ring[rx->rx_next % RX_SLOTS];

		next = MIN(next, r->seq == rx->rx_next && !r->payload ?
					 r->missing_since + rx_hold(rx) :
					 now);
	}
	if (keyframes && rx->need_key && send) {
		if (now >= rx->next_fir) {
			if (call_video_send_peer_feedback(feedback, rx, send,
							  data))
				rx->recovery.firs++;
			rx->next_fir = now + rx->fir_interval;
			/* A conference relay rate-limits keyframes itself. */
			rx->fir_interval = rx->group_rx ?
						   G_USEC_PER_SEC :
						   MIN(rx->fir_interval * 2,
						       G_USEC_PER_SEC);
		}
		next = MIN(next, rx->next_fir);
	}
	struct remb_estimate *e = &rx->remb;

	if (requests && e->estimate &&
	    (e->send_now || now - e->sent_at >= G_USEC_PER_SEC)) {
		if (send_remb(feedback, rx, (guint64)(e->estimate * 1000), send,
			      data))
			rx->recovery.rembs++;
		e->send_now = FALSE;
		e->sent_at = now;
	}
	if (requests && e->estimate)
		next = MIN(next, e->sent_at + G_USEC_PER_SEC);
	if (requests && rx->rx_started) {
		if (now - rx->rr.sent_at >= G_USEC_PER_SEC) {
			send_rr(feedback, rx, send, data);
			rx->rr.sent_at = now;
		}
		next = MIN(next, rx->rr.sent_at + G_USEC_PER_SEC);
	}
	return next;
}

guint call_video_retransmit(struct call_video *v, const guint16 *list,
			    guint count, gint64 now, call_video_send_fn send,
			    gpointer data)
{
	guint sent = 0, previous = G_MAXUINT;

	if (!v || !v->history || !list || !send)
		return 0;
	if (now - v->rtx_window >= G_USEC_PER_SEC) {
		v->rtx_window = now;
		v->rtx_bytes = 0;
	}
	/* Resends may add about a third to the rate, at least 8 KB/s. */
	guint budget = MAX(rate_current(v) * 40, 8000u);

	for (guint i = 0; i < count; i++) {
		struct tx_packet *h = &v->history[list[i] % TX_HISTORY];

		if (h->seq != list[i] || !h->size || now - h->sent > RTX_AGE_US)
			continue;
		if (!h->requested) {
			if (previous != G_MAXUINT &&
			    (guint16)(list[i] - previous) == 1)
				v->rc.adjacent++;
			previous = list[i];
			h->requested = TRUE;
			v->rc.lost++;
		}
		if (h->resends >= 3 || (h->resent && now - h->resent < 20000) ||
		    v->rtx_bytes + h->size > budget)
			continue;
		h->resent = now;
		h->resends++;
		if (!send(h->data, h->size, data))
			break;
		v->rtx_bytes += h->size;
		v->rc.repair_bytes += h->size;
		v->lq.sent += h->size;
		v->recovery.retransmitted++;
		sent++;
	}
	return sent;
}

void call_video_recovery(const struct call_video *v,
			 struct call_video_recovery *out)
{
	*out = (struct call_video_recovery){ 0 };
	if (!v)
		return;
	*out = (struct call_video_recovery){
		.requests = v->recovery.requests,
		.requested = v->recovery.requested,
		.retransmitted = v->recovery.retransmitted,
		.fec_sent = v->recovery.fec_sent,
		.fec_recovered = v->recovery.fec_recovered,
		.gave_up = v->recovery.gave_up,
		.late = v->recovery.late,
		.keyframe_requests = v->recovery.firs,
		.rembs = v->recovery.rembs,
		.rate_changes = v->recovery.rate_changes,
		.rescales = v->recovery.scaled,
		.local_queue_ms = v->lq.queue_ms,
		.local_cuts = v->lq.cuts,
		.target_kbps = v->tx ? rate_current(v) : 0,
		.encoder_kbps = v->sender.kbps,
		.rtt_ms = v->rtx_rtt_us / 1000,
		.hold_ms = v->rx ? rx_hold(v) / 1000 : 0,
		.width = v->sender.width,
		.height = v->sender.height,
	};
}

void call_video_uplink_loss(struct call_video *v, guint packets, guint received)
{
	if (v && v->tx && packets >= 10 && received <= packets) {
		v->rc.feedback = TRUE;
		v->rc.report_loss = MAX(v->rc.report_loss,
					1.0 - (double)received / packets);
	}
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

static void request_keyframe(struct call_video *v, gint64 now)
{
	v->rc.firs++;
	v->rc.last_fir = now;
	/* Requests that cross a keyframe in flight wait for its spacing. */
	if (!v->last_key_at || now - v->last_key_at >= KEY_SPACING_US)
		v->sender.force_key = TRUE;
	else if (!v->key_due)
		v->key_due = v->last_key_at + KEY_SPACING_US;
}

/* Returns TRUE for a feedback block addressed to this sender. */
static gboolean sender_feedback(struct call_video *v, const guint8 *p,
				gsize size, gint64 now, call_video_send_fn send,
				gpointer data)
{
	guint fmt = p[0] & 31;

	if (picture_feedback(p, size, v->tx_ssrc, v->rx_ssrc)) {
		request_keyframe(v, now);
		return TRUE;
	}
	if ((p[1] == RTCP_RTPFB || p[1] == RTCP_PSFB) &&
	    (fmt == LINE_RRR || fmt == RTCP_NACK || fmt == RTCP_REMB))
		v->rc.feedback = TRUE;
	if (p[1] == RTCP_RTPFB && size >= 16 && read32(p + 8) == v->tx_ssrc &&
	    (fmt == LINE_RRR || fmt == RTCP_NACK)) {
		guint16 list[RRR_MAX + 16 * 17];
		guint count = 0;

		if (fmt == LINE_RRR) {
			guint n = read16(p + 12);

			if (!n || n > RRR_MAX || 14 + 2 * (gsize)n > size)
				return FALSE;
			for (guint i = 0; i < n; i++)
				list[count++] = read16(p + 14 + 2 * i);
		} else {
			for (gsize pos = 12; pos + 4 <= size &&
					     count + 17 <= G_N_ELEMENTS(list);
			     pos += 4) {
				guint16 pid = read16(p + pos);
				guint16 mask = read16(p + pos + 2);

				list[count++] = pid;
				for (guint b = 0; b < 16; b++)
					if (mask & (1u << b))
						list[count++] = pid + b + 1;
			}
		}
		call_video_retransmit(v, list, count, now, send, data);
		return TRUE;
	}
	if (p[1] == RTCP_PSFB && fmt == RTCP_REMB && size >= 24 &&
	    !memcmp(p + 12, "REMB", 4) && 20 + 4 * (gsize)p[16] <= size) {
		guint64 bps = (((guint32)(p[17] & 3) << 16) | read16(p + 18))
			      << (p[17] >> 2);
		gboolean ours = FALSE;

		for (guint i = 0; i < p[16]; i++)
			ours |= read32(p + 20 + 4 * i) == v->tx_ssrc;
		if (!ours)
			return FALSE;
		v->rc.remb = MAX(bps / 1000, (guint64)MIN_KBPS);
		v->rc.remb_at = now;
		if (v->rc.remb < rate_current(v)) {
			rate_set(v, v->rc.remb, now);
			v->rc.decreased_at = now;
		}
		return TRUE;
	}
	if ((p[1] == RTCP_SR || p[1] == RTCP_RR) && fmt) {
		gsize pos = p[1] == RTCP_SR ? 28 : 8;
		gboolean ours = FALSE;

		for (guint i = 0; i < fmt && pos + 24 <= size; i++, pos += 24) {
			if (read32(p + pos) != v->tx_ssrc)
				continue;
			v->rc.report_loss =
				MAX(v->rc.report_loss, p[pos + 4] / 256.0);
			v->rc.feedback = ours = TRUE;
			v->rr_seen = TRUE;
		}
		return ours;
	}
	return FALSE;
}

gboolean call_video_receive_rtcp(struct call_video *v, const guint8 *packet,
				 gsize size, gint64 now,
				 call_video_send_fn send, gpointer data)
{
	guint8 buf[MAX_DATAGRAM];
	int plain = size;
	gboolean known = FALSE;
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
		known |= sender_feedback(v, p, payload, now, send, data);
		pos += bytes;
	}
	if (known)
		rate_update(v, now);
	return known && pos == (gsize)plain;
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

struct call_vp8_decoder *call_video_decoder(struct call_video *v)
{
	return v ? v->dec : NULL;
}

struct call_vp8_decoder *call_share_video_decoder(struct call_share_decoder *s)
{
	return s ? s->dec : NULL;
}

struct call_share_decoder *call_share_decoder_new(void)
{
	struct call_share_decoder *s = g_new0(struct call_share_decoder, 1);

	s->dec = call_vp8_open(MIN(4, g_get_num_processors()), TRUE);
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

static void need_share_key(struct call_share_decoder *s)
{
	s->need_key = TRUE;
	call_vp8_discontinuity(s->dec);
}

void call_share_decoded(struct call_share_decoder *s,
			call_video_display_fn display, gpointer data)
{
	struct call_vp8_output out;

	if (!s || !call_vp8_take(s->dec, &out))
		return;
	if (out.shown < 0) {
		need_share_key(s);
		s->errors++;
	} else if (out.shown > 0) {
		display(&out.image, data);
		s->frames += out.shown;
	}
	call_vp8_release(s->dec);
}

static gboolean share_decode_frame(struct call_share_decoder *s,
				   const guint8 *vp8, gsize n,
				   call_video_display_fn display, gpointer data)
{
	gboolean key = n >= 3 && !(vp8[0] & 1);
	int shown;

	call_share_decoded(s, display, data);
	if (!key && s->need_key)
		return TRUE;
	if (key) {
		vpx_codec_stream_info_t info = { .sz = sizeof(info) };

		if (vpx_codec_peek_stream_info(vpx_codec_vp8_dx(), vp8, n,
					       &info) ||
		    !info.is_kf ||
		    !line_preview_size_valid(info.w, info.h, TRUE))
			return FALSE;
	}
	switch (call_vp8_submit(s->dec, vp8, n, key, s->rotation, 0)) {
	case CALL_VP8_QUEUED:
		s->need_key = FALSE;
		return TRUE;
	case CALL_VP8_FULL:
		return FALSE;
	case CALL_VP8_INLINE:
		break;
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
			need_share_key(s);
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
		need_share_key(s);
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
	struct encode_thread *worker;
	/* The latest capture replaces any waiting behind the active job. */
	GBytes *pending;
	gboolean pending_dma;
	guint pending_width, pending_height;
	guint64 pending_ns, active_ns;
	gboolean request_key;
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
	encoder_stop(s->worker);
	g_clear_pointer(&s->pending, g_bytes_unref);
	sender_close(&s->sender);
	g_free(s);
}

void call_share_encoder_keyframe(struct call_share_encoder *s)
{
	if (s)
		s->request_key = TRUE;
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

static gboolean share_emit(struct call_share_encoder *s,
			   struct vp8_picture picture, guint res,
			   call_share_fragment_fn emit, gpointer data)
{
	guint16 id = s->picture++;

	if (picture.size > FRAME_LIMIT) {
		s->request_key = TRUE;
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
			return FALSE;
		pos += len;
	}
	return TRUE;
}

gboolean call_share_encode(struct call_share_encoder *s,
			   const struct line_camera_frame *frame, gint64 now,
			   call_share_fragment_fn emit, gpointer data,
			   GError **e)
{
	vpx_image_t img;
	struct vp8_picture picture = { 0 };

	if (!s || s->worker || !emit || !frame || now < 0 ||
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
	if (!sender_encode(&s->sender, &img, (guint64)now * 9 / 100,
			   s->sender.force_key || s->request_key, &picture))
		goto failed;
	s->last_encoded = now;
	if (!picture.size)
		return TRUE;
	s->sender.force_key = FALSE;

	s->request_key = FALSE;
	if (share_emit(s, picture, resolution_class(img.w * img.h), emit, data))
		return TRUE;
failed:
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode the shared screen.");
	return FALSE;
}

void call_share_encode_async(struct call_share_encoder *s)
{
	if (!s || s->worker)
		return;
	s->worker = encoder_start(&s->sender);
	if (s->worker) {
		s->worker->screen_kbps = s->kbps;
		s->worker->screen_fps = s->fps;
	}
}

int call_share_encoder_fd(const struct call_share_encoder *s)
{
	return s && s->worker ? s->worker->wake : -1;
}

static void share_submit_pending(struct call_share_encoder *s)
{
	struct encode_thread *t = s->worker;
	struct line_camera_frame frame;

	if (!s->pending || encoder_busy(t))
		return;
	gint64 now = s->pending_ns / 1000;

	if (s->last_encoded >= 0 &&
	    now - s->last_encoded < G_USEC_PER_SEC / s->fps * 3 / 4) {
		g_clear_pointer(&s->pending, g_bytes_unref);
		return;
	}
	s->last_encoded = now;
	t->capture = g_steal_pointer(&s->pending);
	line_capture_frame(&frame, t->capture, s->pending_dma, s->pending_width,
			   s->pending_height);
	sender_image(&t->img, &frame);
	s->active_ns = s->pending_ns;
	encoder_queue(t,
		      s->active_ns / 100000 * 9 +
			      s->active_ns % 100000 * 9 / 100000,
		      s->active_ns / 1000, s->request_key);
	s->request_key = FALSE;
}

gboolean call_share_queue(struct call_share_encoder *s, GBytes *pixels,
			  gboolean dma, guint width, guint height,
			  guint64 capture_ns, guint64 *stream_time,
			  call_share_fragment_fn emit, gpointer data)
{
	struct line_camera_frame frame;

	if (!s || !pixels || !emit || !stream_time ||
	    !line_camera_frame_size_valid(width, height) ||
	    g_bytes_get_size(pixels) !=
		    (dma ? sizeof(struct line_capture_dma) :
			   line_camera_i420_size(width, height)))
		return FALSE;
	if (!s->worker) {
		*stream_time = capture_ns;
		line_capture_frame(&frame, pixels, dma, width, height);
		return call_share_encode(s, &frame, capture_ns / 1000, emit,
					 data, NULL);
	}
	g_clear_pointer(&s->pending, g_bytes_unref);
	s->pending = g_bytes_ref(pixels);
	s->pending_dma = dma;
	s->pending_width = width;
	s->pending_height = height;
	s->pending_ns = capture_ns;
	share_submit_pending(s);
	return TRUE;
}

gboolean call_share_encoded(struct call_share_encoder *s, gboolean transmit,
			    guint64 *stream_time, call_share_fragment_fn emit,
			    gpointer data)
{
	struct vp8_picture picture;
	eventfd_t n;
	gboolean ok;

	if (!s || !s->worker || !emit || !stream_time)
		return FALSE;
	struct encode_thread *t = s->worker;

	(void)eventfd_read(t->wake, &n);
	g_mutex_lock(&t->lock);
	if (t->state != ENCODE_DONE) {
		g_mutex_unlock(&t->lock);
		return TRUE;
	}
	*stream_time = s->active_ns;
	ok = t->ok;
	picture = (struct vp8_picture){ .data = t->out,
					.size = t->out_size,
					.key = t->key,
					.delay_ms = t->delay_ms };
	t->state = ENCODE_IDLE;
	g_mutex_unlock(&t->lock);
	if (!transmit) {
		g_clear_pointer(&s->pending, g_bytes_unref);
		s->request_key = TRUE;
	} else if (ok && picture.size) {
		ok = share_emit(s, picture,
				resolution_class(t->img.w * t->img.h), emit,
				data);
	} else if (t->force) {
		s->request_key = TRUE;
	}
	if (ok)
		share_submit_pending(s);
	return ok;
}
