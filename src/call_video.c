// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_video.h"
#include "call_srtp.h"
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
};

enum video_format {
	VIDEO_UNKNOWN,
	VIDEO_EVS3,
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
};

struct call_video {
	srtp_t tx, rx;
	vpx_codec_ctx_t enc, dec;
	gboolean enc_ready, dec_ready, force_key, need_key;
	guint width, height, kbps, fps;
	guint screen_kbps, screen_fps;
	guint rx_rotation;
	guint32 tx_ssrc, rx_ssrc, ts_base, rx_ts;
	guint16 seq, picture, rx_picture, first, last, previous_end;
	guint8 pt;
	gint64 epoch, last_encoded;
	enum video_format framing, rx_format;
	gboolean have_ts, have_last, have_previous_end, finished;
	gsize received, expected;
	unsigned int fragments, authenticated;

	struct {
		guint evs_starts, rfc_starts, markers, unknown, incomplete;
		guint evs_metadata;
		guint assembly, length, pictures, keys, deltas;
		guint bad_key, dimensions, decode, hidden;
		guint flags[16];
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
	if (v->enc_ready)
		vpx_codec_destroy(&v->enc);
	if (v->dec_ready)
		vpx_codec_destroy(&v->dec);
	if (v->tx)
		srtp_dealloc(v->tx);
	if (v->rx)
		srtp_dealloc(v->rx);
	g_free(v);
}

struct call_video *call_video_open(const guint8 tx[30], const guint8 rx[30],
				   guint8 pt, guint32 tx_ssrc, guint32 rx_ssrc,
				   guint kbps, guint fps, GError **e)
{
	struct call_video *v = g_new0(struct call_video, 1);
	vpx_codec_dec_cfg_t cfg = { .threads = 2 };

	if (!tx || !rx || pt < 96 || pt > 127 || !kbps || kbps > 8000 || !fps ||
	    fps > 30 || !call_srtp_open(&v->tx, tx, ssrc_specific, tx_ssrc) ||
	    !call_srtp_open(&v->rx, rx, ssrc_specific, rx_ssrc) ||
	    RAND_bytes((guint8 *)&v->seq, sizeof(v->seq)) != 1 ||
	    RAND_bytes((guint8 *)&v->picture, sizeof(v->picture)) != 1 ||
	    RAND_bytes((guint8 *)&v->ts_base, sizeof(v->ts_base)) != 1 ||
	    vpx_codec_dec_init(&v->dec, vpx_codec_vp8_dx(), &cfg, 0)) {
		g_set_error_literal(
			e, LINE_ERROR, 1,
			"Could not initialize encrypted VP8 video.");
		call_video_close(v);
		return NULL;
	}
	v->dec_ready = TRUE;
	v->force_key = v->need_key = TRUE;
	v->pt = pt;
	v->tx_ssrc = tx_ssrc;
	v->rx_ssrc = rx_ssrc;
	v->kbps = kbps;
	v->fps = fps;
	v->epoch = v->last_encoded = -1;
	return v;
}

static gboolean configure_encoder(struct call_video *v, guint w, guint h)
{
	vpx_codec_enc_cfg_t cfg;
	vpx_codec_iface_t *codec = vpx_codec_vp8_cx();

	if (v->enc_ready && w == v->width && h == v->height)
		return TRUE;
	if (v->enc_ready) {
		vpx_codec_destroy(&v->enc);
		v->enc_ready = FALSE;
	}
	if (vpx_codec_enc_config_default(codec, &cfg, 0))
		return FALSE;
	guint pixels = w * h;
	guint cores = MAX(1, g_get_num_processors());
	gboolean screen = v->screen_kbps != 0;
	guint fps = screen ? v->screen_fps : v->fps;

	cfg.g_w = w;
	cfg.g_h = h;
	/* Real-time settings follow the WebRTC VP8 encoder defaults. */
	cfg.g_threads = pixels >= 1280 * 720 ? MIN(cores, screen ? 8 : 4) :
			pixels >= 640 * 360  ? MIN(cores, 2) :
					       1;
	cfg.g_timebase = (vpx_rational_t){ 1, 90000 };
	cfg.g_lag_in_frames = 0;
	cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
	cfg.g_pass = VPX_RC_ONE_PASS;
	cfg.rc_end_usage = screen ? VPX_VBR : VPX_CBR;
	cfg.rc_target_bitrate = screen ? v->screen_kbps : v->kbps;
	cfg.rc_min_quantizer = 2;
	cfg.rc_max_quantizer = screen ? 40 : 56;
	cfg.rc_undershoot_pct = 100;
	cfg.rc_overshoot_pct = screen ? 50 : 15;
	cfg.rc_buf_initial_sz = 500;
	cfg.rc_buf_optimal_sz = 600;
	cfg.rc_buf_sz = 1000;
	/* Lower quality briefly rather than stutter after keyframes. */
	cfg.rc_dropframe_thresh = 0;
	cfg.rc_resize_allowed = 0;
	cfg.kf_mode = VPX_KF_AUTO;
	cfg.kf_max_dist = 3 * fps;
	if (vpx_codec_enc_init(&v->enc, codec, &cfg, 0))
		return FALSE;
	v->enc_ready = TRUE;
	v->width = w;
	v->height = h;
	v->force_key = TRUE;
	/*
	 * Speed 6 leaves headroom for 720p on a laptop. Small pictures can
	 * afford the slower, sharper search.
	 */
	int speed = pixels >= 1280 * 720 ? -6 : pixels >= 640 * 360 ? -5 : -4;
	/* Give text-heavy keyframes more room, while limiting RTP bursts. */
	guint intra =
		screen ? CLAMP(131072u * fps / v->screen_kbps, 400, 1000) : 300;
	int partitions = screen && cfg.g_threads >= 4 ?
				 VP8_FOUR_TOKENPARTITION :
			 cfg.g_threads >= 2 ? VP8_TWO_TOKENPARTITION :
					      VP8_ONE_TOKENPARTITION;

	return vpx_codec_control(&v->enc, VP8E_SET_CPUUSED, speed) ==
		       VPX_CODEC_OK &&
	       vpx_codec_control(&v->enc, VP8E_SET_SCREEN_CONTENT_MODE,
				 screen ? 1 : 0) == VPX_CODEC_OK &&
	       vpx_codec_control(&v->enc, VP8E_SET_STATIC_THRESHOLD,
				 screen ? 20 : 1) == VPX_CODEC_OK &&
	       vpx_codec_control(&v->enc, VP8E_SET_NOISE_SENSITIVITY, 0) ==
		       VPX_CODEC_OK &&
	       vpx_codec_control(&v->enc, VP8E_SET_TOKEN_PARTITIONS,
				 partitions) == VPX_CODEC_OK &&
	       vpx_codec_control(&v->enc, VP8E_SET_MAX_INTRA_BITRATE_PCT,
				 intra) == VPX_CODEC_OK;
}

/* Resolution classes used by vns_get_resolution_type_from_wxh. */
static guint resolution_class(const struct call_video *v)
{
	const guint pixels[] = { 19200, 172800, 691200, 2764800, 11059200 };
	guint count = v->width * v->height;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(pixels) && count > pixels[i]; i++)
		;
	return i;
}

/* The single-stream PLANET VP8 packetizer writes an EVS3 descriptor.
 * The first fragment also carries the three-byte size of its bitstream
 * including that size field.
 */
static gsize write_descriptor(guint8 *p, guint16 id, gboolean first,
			      gboolean last, gboolean key, guint resolution,
			      guint32 size)
{
	gsize pos = 3;

	p[0] = 0x80 | (last ? 4 : 0);
	write16(p + 1, id);
	if (!first)
		return pos;
	p[0] |= key ? 0x3a : 0x7a;
	p[pos++] = 0;
	if (!key) {
		p[pos++] = 3;
		p[pos++] = 1;
		p[pos++] = 0;
	}
	p[pos++] = (resolution << 1) | (key ? 0x20 : 0);
	size += 3;
	p[pos++] = size & 3;
	p[pos++] = size >> 2;
	p[pos++] = size >> 10;
	return pos;
}

static gboolean send_picture(struct call_video *v, guint32 timestamp,
			     const guint8 *picture, gsize size, gboolean key,
			     call_video_send_fn send, gpointer data)
{
	const gsize chunk = VIDEO_MTU - RTP_HEADER - 11 - 10;
	guint16 id = v->picture++;

	if (size > FRAME_LIMIT) {
		v->force_key = TRUE;
		return TRUE;
	}
	for (gsize pos = 0; pos < size;) {
		guint8 packet[VIDEO_MTU + SRTP_MAX_TRAILER_LEN];
		gsize len = MIN(chunk, size - pos);
		gboolean last = pos + len == size;
		gsize header;
		int n;

		packet[0] = 0x80;
		packet[1] = v->pt | (last ? 0x80 : 0);
		write16(packet + 2, v->seq++);
		write32(packet + 4, timestamp);
		write32(packet + 8, v->tx_ssrc);
		header = write_descriptor(packet + RTP_HEADER, id, !pos, last,
					  key, resolution_class(v), size);
		memcpy(packet + RTP_HEADER + header, picture + pos, len);
		n = RTP_HEADER + header + len;
		if (srtp_protect(v->tx, packet, &n) != srtp_err_status_ok ||
		    !send(packet, n, data))
			return FALSE;
		pos += len;
	}
	return TRUE;
}

void call_video_keyframe(struct call_video *v)
{
	if (v)
		v->force_key = TRUE;
}

void call_video_screen(struct call_video *v, guint kbps, guint fps)
{
	fps = kbps ? CLAMP(fps, 1, 30) : 0;
	if (!v || (kbps == v->screen_kbps && fps == v->screen_fps))
		return;
	if (v->enc_ready) {
		vpx_codec_destroy(&v->enc);
		v->enc_ready = FALSE;
	}
	v->screen_kbps = kbps;
	v->screen_fps = fps;
	v->force_key = TRUE;
}

gboolean call_video_encode(struct call_video *v,
			   const struct line_camera_frame *frame, gint64 now,
			   call_video_send_fn send, gpointer data, GError **e)
{
	const vpx_codec_cx_pkt_t *packet;
	vpx_codec_iter_t iter = NULL;
	vpx_image_t img = { .fmt = VPX_IMG_FMT_I420,
			    .bit_depth = 8,
			    .x_chroma_shift = 1,
			    .y_chroma_shift = 1,
			    .bps = 12 };
	guint64 pts;

	if (!v || !send || !frame || now < 0 ||
	    !line_camera_frame_size_valid(frame->width, frame->height))
		goto failed;
	guint fps = v->screen_kbps ? v->screen_fps : v->fps;

	/* Accept jitter, and switch sources immediately even at the same size. */
	if (v->enc_ready && v->last_encoded >= 0 &&
	    now - v->last_encoded < G_USEC_PER_SEC / fps * 3 / 4)
		return TRUE;
	img.w = img.d_w = frame->width;
	img.h = img.d_h = frame->height;
	for (int i = 0; i < 3; i++) {
		guint width = i ? (img.w + 1) / 2 : img.w;

		if (!frame->plane[i] || frame->stride[i] < (int)width)
			goto failed;
		img.planes[i] = (guint8 *)frame->plane[i];
		img.stride[i] = frame->stride[i];
	}
	if (!configure_encoder(v, img.w, img.h))
		goto failed;
	if (v->epoch < 0)
		v->epoch = now;
	pts = ((guint64)(now - v->epoch) / 100) * 9 +
	      ((guint64)(now - v->epoch) % 100) * 9 / 100;
	if (vpx_codec_encode(&v->enc, &img, pts, 90000 / fps,
			     v->force_key ? VPX_EFLAG_FORCE_KF : 0,
			     VPX_DL_REALTIME))
		goto failed;
	v->last_encoded = now;
	v->force_key = FALSE;
	while ((packet = vpx_codec_get_cx_data(&v->enc, &iter))) {
		if (packet->kind == VPX_CODEC_CX_FRAME_PKT &&
		    !send_picture(v, v->ts_base + pts, packet->data.frame.buf,
				  packet->data.frame.sz,
				  !!(packet->data.frame.flags &
				     VPX_FRAME_IS_KEY),
				  send, data))
			goto failed;
	}
	return TRUE;
failed:
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode or send the call video frame.");
	return FALSE;
}

static gboolean decode_picture(struct call_video *v, const guint8 *p, gsize n,
			       call_video_display_fn display, gpointer data)
{
	vpx_codec_iter_t iter = NULL;
	vpx_image_t *img;
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
	if (vpx_codec_decode(&v->dec, p, n, NULL, VPX_DL_REALTIME)) {
		v->stats.decode++;
		return FALSE;
	}
	v->need_key = FALSE;
	gboolean displayed = FALSE;

	while ((img = vpx_codec_get_frame(&v->dec, &iter))) {
		struct line_camera_frame frame = { .width = img->d_w,
						   .height = img->d_h,
						   .rotation = v->rx_rotation };

		if (img->fmt != VPX_IMG_FMT_I420 ||
		    !line_camera_frame_size_valid(frame.width, frame.height)) {
			v->stats.dimensions++;
			return FALSE;
		}
		for (int i = 0; i < 3; i++) {
			frame.plane[i] = img->planes[i];
			frame.stride[i] = img->stride[i];
		}
		display(&frame, data);
		displayed = TRUE;
	}
	v->stats.hidden += !displayed;
	return TRUE;
}

/* Skip optional layer references, rotation and extension records before the
 * length field. Only the base-layer EVS3 RTP profile is negotiated.
 */
/*
 * Over RTP the first fragment carries a three-byte frame length. A PDTP
 * stream delivers whole frames and omits it, as in LINE screen shares.
 */
static gboolean parse_evs3_framed(const guint8 *p, gsize n, gboolean length,
				  struct video_payload *out)
{
	gsize pos = 3, expected = 0;
	guint flags, rotation = 0;
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

		ext = !(p[pos + 1] & 128);
		pos += 2;
		if (len > n - pos)
			return FALSE;
		pos += len;
	}
	if ((flags & 8) && length) {
		guint total;

		if (n - pos < 3)
			return FALSE;
		total = (p[pos] & 3) | (p[pos + 1] << 2) | (p[pos + 2] << 10);
		if (total <= 3)
			return FALSE;
		expected = total - 3;
		pos += 3;
	}
	if (pos >= n)
		return FALSE;
	*out = (struct video_payload){ pos, expected, !!(flags & 8),
				       !!(flags & 4), rotation };
	return TRUE;
}

static gboolean parse_evs3(const guint8 *p, gsize n, struct video_payload *out)
{
	return parse_evs3_framed(p, n, TRUE, out);
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
	*out = (struct video_payload){ pos, 0, (p[0] & 0x17) == 0x10, marker,
				       0 };
	return TRUE;
}

static gboolean describe(enum video_format framing, const guint8 *p, gsize n,
			 gboolean marker, struct video_payload *out)
{
	return framing == VIDEO_EVS3 ? parse_evs3(p, n, out) :
				       parse_vp8(p, n, marker, out);
}

static gboolean finish_picture(struct call_video *v, guint count,
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
		    (v->framing == VIDEO_EVS3 &&
		     read16(p + 1) != v->rx_picture))
			return FALSE;
		bytes -= payload.offset;
		if (bytes > FRAME_LIMIT - len)
			return FALSE;
		memcpy(picture + len, p + payload.offset, bytes);
		len += bytes;
	}
	if (v->expected && len != v->expected) {
		v->stats.length++;
		return FALSE;
	}
	if (v->have_previous_end && (guint16)(v->first - v->previous_end) != 1)
		v->need_key = TRUE;
	v->previous_end = v->last;
	v->have_previous_end = TRUE;
	v->rx_format = v->framing;
	return decode_picture(v, picture, len, display, data);
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
	gboolean evs = parse_evs3(p, n, &evs3);
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
		v->framing = evs3.start ? VIDEO_EVS3 : VIDEO_RFC7741;
		payload = evs3.start ? evs3 : vp8;
		v->rx_picture = evs3.start ? read16(p + 1) : 0;
		v->expected = payload.expected;
		v->rx_rotation = payload.rotation;
		v->first = seq;
		if (!find_early_end(v))
			goto invalid;
	} else {
		payload = v->framing == VIDEO_EVS3 ? evs3 : vp8;
		if (v->framing != VIDEO_UNKNOWN &&
		    (!payload.offset || payload.start))
			return FALSE;
	}
	if (v->framing == VIDEO_EVS3 && read16(p + 1) != v->rx_picture)
		return FALSE;
	if (v->framing != VIDEO_UNKNOWN && payload.end && !mark_end(v, seq))
		goto invalid;
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
	if (!finish_picture(v, count, display, data))
		goto invalid;
	v->finished = TRUE;
	clear_picture(v);
	return TRUE;
invalid:
	v->stats.assembly++;
	v->need_key = v->finished = TRUE;
	clear_picture(v);
	return FALSE;
}

gboolean call_video_receive(struct call_video *v, const guint8 *packet,
			    gsize size, call_video_display_fn display,
			    gpointer data)
{
	guint8 buf[MAX_DATAGRAM];
	int plain = size;
	gsize pos, end;

	if (!v || !packet || !display || size <= RTP_HEADER + 10 ||
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
		pos += 4u + 4u * read16(buf + pos + 2);
		if (pos >= end)
			return FALSE;
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

void call_video_report(const struct call_video *v)
{
	if (!v)
		return;
	g_printerr(
		"line-call video payload evs-start=%u metadata=%u rfc-start=%u marker=%u unknown=%u incomplete=%u assembly=%u length=%u pictures=%u keys=%u waiting-key=%u bad-key=%u dimensions=%u decode=%u hidden=%u\n",
		v->stats.evs_starts, v->stats.evs_metadata, v->stats.rfc_starts,
		v->stats.markers, v->stats.unknown, v->stats.incomplete,
		v->stats.assembly, v->stats.length, v->stats.pictures,
		v->stats.keys, v->stats.deltas, v->stats.bad_key,
		v->stats.dimensions, v->stats.decode, v->stats.hidden);
	GString *flags = g_string_new("line-call video descriptor classes");

	for (guint i = 0; i < G_N_ELEMENTS(v->stats.flags); i++)
		if (v->stats.flags[i])
			g_string_append_printf(flags, " %x:%u", i,
					       v->stats.flags[i]);
	g_printerr("%s\n", flags->str);
	g_string_free(flags, TRUE);
}

gboolean call_video_send_feedback(struct call_video *v, call_video_send_fn send,
				  gpointer data)
{
	guint8 packet[LINE_FIR_SIZE + SRTP_MAX_TRAILER_LEN + 4] = { 0 };
	gboolean standard;
	int size;

	if (!v || !send)
		return FALSE;
	standard =
		v->framing == VIDEO_RFC7741 ||
		(v->framing == VIDEO_UNKNOWN && v->rx_format == VIDEO_RFC7741);
	size = standard ? RTP_HEADER : LINE_FIR_SIZE;
	packet[0] = 0x80 | (standard ? RTCP_PLI : LINE_FIR);
	packet[1] = RTCP_PSFB;
	write16(packet + 2, size / 4 - 1);
	write32(packet + 4, v->tx_ssrc);
	write32(packet + 8, v->rx_ssrc);
	/* Use LINE's base-layer FCI with no reference picture. */
	return srtp_protect_rtcp(v->tx, packet, &size) == srtp_err_status_ok &&
	       send(packet, size, data);
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

	if (!v || !packet || size <= 8 + SRTCP_TRAILER || size > sizeof(buf) ||
	    packet[0] >> 6 != 2 || packet[1] < 192 || packet[1] > 223 ||
	    read32(packet + 4) != v->rx_ssrc)
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

struct call_share_decoder {
	vpx_codec_ctx_t dec;
	gboolean need_key, assembling;
	guint16 picture;
	guint rotation;
	GByteArray *frame;
	guint frames, errors;
};

struct call_share_decoder *call_share_decoder_new(void)
{
	struct call_share_decoder *s = g_new0(struct call_share_decoder, 1);
	vpx_codec_dec_cfg_t cfg = { .threads = MIN(4, g_get_num_processors()) };

	if (vpx_codec_dec_init(&s->dec, vpx_codec_vp8_dx(), &cfg, 0)) {
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
	vpx_codec_destroy(&s->dec);
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
	vpx_codec_iter_t iter = NULL;
	vpx_image_t *img;

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
	if (vpx_codec_decode(&s->dec, vp8, n, NULL, VPX_DL_REALTIME))
		return FALSE;
	s->need_key = FALSE;
	while ((img = vpx_codec_get_frame(&s->dec, &iter))) {
		struct line_camera_frame out = { .width = img->d_w,
						 .height = img->d_h,
						 .rotation = s->rotation };

		if (img->fmt != VPX_IMG_FMT_I420 ||
		    !line_camera_frame_size_valid(out.width, out.height))
			return FALSE;
		for (int i = 0; i < 3; i++) {
			out.plane[i] = img->planes[i];
			out.stride[i] = img->stride[i];
		}
		display(&out, data);
		s->frames++;
	}
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
	vpx_codec_ctx_t enc;
	gboolean ready, force_key;
	guint width, height, kbps, fps;
	guint16 picture;
	gint64 last_encoded;
};

struct call_share_encoder *call_share_encoder_new(guint kbps, guint fps)
{
	struct call_share_encoder *s = g_new0(struct call_share_encoder, 1);

	s->kbps = CLAMP(kbps, 300, 20000);
	s->fps = CLAMP(fps, 1, 60);
	s->force_key = TRUE;
	s->last_encoded = -1;
	if (RAND_bytes((guint8 *)&s->picture, sizeof(s->picture)) != 1)
		s->picture = 1;
	return s;
}

void call_share_encoder_free(struct call_share_encoder *s)
{
	if (!s)
		return;
	if (s->ready)
		vpx_codec_destroy(&s->enc);
	g_free(s);
}

void call_share_encoder_keyframe(struct call_share_encoder *s)
{
	if (s)
		s->force_key = TRUE;
}

static gboolean share_encoder_configure(struct call_share_encoder *s, guint w,
					guint h)
{
	vpx_codec_enc_cfg_t cfg;
	vpx_codec_iface_t *codec = vpx_codec_vp8_cx();

	if (s->ready && w == s->width && h == s->height)
		return TRUE;
	if (s->ready) {
		vpx_codec_destroy(&s->enc);
		s->ready = FALSE;
	}
	if (vpx_codec_enc_config_default(codec, &cfg, 0))
		return FALSE;
	cfg.g_w = w;
	cfg.g_h = h;
	cfg.g_threads = MIN(MAX(1, g_get_num_processors()), 8);
	cfg.g_timebase = (vpx_rational_t){ 1, 90000 };
	cfg.g_lag_in_frames = 0;
	cfg.g_error_resilient = 0; /* PDTP delivers every byte. */
	cfg.g_pass = VPX_RC_ONE_PASS;
	cfg.rc_end_usage = VPX_VBR;
	cfg.rc_target_bitrate = s->kbps;
	/* Low quantizers keep small text legible. */
	cfg.rc_min_quantizer = 2;
	cfg.rc_max_quantizer = 40;
	cfg.rc_undershoot_pct = 100;
	cfg.rc_overshoot_pct = 50;
	cfg.rc_buf_initial_sz = 1000;
	cfg.rc_buf_optimal_sz = 1500;
	cfg.rc_buf_sz = 3000;
	cfg.rc_dropframe_thresh = 0;
	cfg.rc_resize_allowed = 0;
	cfg.kf_mode = VPX_KF_AUTO;
	cfg.kf_max_dist = 10 * s->fps;
	if (vpx_codec_enc_init(&s->enc, codec, &cfg, 0))
		return FALSE;
	s->ready = TRUE;
	s->width = w;
	s->height = h;
	s->force_key = TRUE;
	guint pixels = w * h;

	return vpx_codec_control(&s->enc, VP8E_SET_CPUUSED,
				 pixels > 1280 * 720 ? -8 : -6) ==
		       VPX_CODEC_OK &&
	       vpx_codec_control(&s->enc, VP8E_SET_SCREEN_CONTENT_MODE, 1) ==
		       VPX_CODEC_OK &&
	       vpx_codec_control(&s->enc, VP8E_SET_STATIC_THRESHOLD, 100) ==
		       VPX_CODEC_OK &&
	       vpx_codec_control(&s->enc, VP8E_SET_NOISE_SENSITIVITY, 0) ==
		       VPX_CODEC_OK &&
	       vpx_codec_control(&s->enc, VP8E_SET_TOKEN_PARTITIONS,
				 VP8_FOUR_TOKENPARTITION) == VPX_CODEC_OK &&
	       vpx_codec_control(&s->enc, VP8E_SET_MAX_INTRA_BITRATE_PCT,
				 1000) == VPX_CODEC_OK;
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
	const vpx_codec_cx_pkt_t *packet;
	vpx_codec_iter_t iter = NULL;
	vpx_image_t img = { .fmt = VPX_IMG_FMT_I420,
			    .bit_depth = 8,
			    .x_chroma_shift = 1,
			    .y_chroma_shift = 1,
			    .bps = 12 };

	if (!s || !emit || !frame ||
	    !line_camera_frame_size_valid(frame->width, frame->height))
		goto failed;
	if (s->last_encoded >= 0 &&
	    now - s->last_encoded < G_USEC_PER_SEC / s->fps * 3 / 4)
		return TRUE;
	img.w = img.d_w = frame->width;
	img.h = img.d_h = frame->height;
	for (int i = 0; i < 3; i++) {
		img.planes[i] = (guint8 *)frame->plane[i];
		img.stride[i] = frame->stride[i];
	}
	if (!share_encoder_configure(s, img.w, img.h))
		goto failed;
	gint64 started = g_get_monotonic_time();
	guint64 pts = (guint64)now * 9 / 100;

	if (vpx_codec_encode(&s->enc, &img, pts, 90000 / s->fps,
			     s->force_key ? VPX_EFLAG_FORCE_KF : 0,
			     VPX_DL_REALTIME))
		goto failed;
	s->last_encoded = now;
	s->force_key = FALSE;
	guint delay = (g_get_monotonic_time() - started) / 1000;
	const guint pixels[] = { 19200, 172800, 691200, 2764800, 11059200 };
	guint resolution = 0;

	while (resolution < G_N_ELEMENTS(pixels) &&
	       img.w * img.h > pixels[resolution])
		resolution++;
	while ((packet = vpx_codec_get_cx_data(&s->enc, &iter))) {
		if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
			continue;
		const guint8 *vp8 = packet->data.frame.buf;
		gsize size = packet->data.frame.sz;
		gboolean key = !!(packet->data.frame.flags & VPX_FRAME_IS_KEY);
		guint16 id = s->picture++;

		if (size > FRAME_LIMIT) {
			s->force_key = TRUE;
			continue;
		}
		/* The official app sends about 900 bytes of VP8 a fragment. */
		for (gsize pos = 0; pos < size;) {
			guint8 frag[1024 + 32];
			gsize len = MIN(1000, size - pos);
			gsize header = call_share_descriptor(frag, id, !pos,
							     pos + len == size,
							     key, resolution,
							     delay, size);

			memcpy(frag + header, vp8 + pos, len);
			if (!emit(frag, header + len, key, data))
				goto failed;
			pos += len;
		}
	}
	return TRUE;
failed:
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode the shared screen.");
	return FALSE;
}
