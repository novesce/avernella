// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_schema.h"
#include "call_hwdecode_config.h"
#include "call_video.h"
#include "call_srtp.h"
#include "call_vp8.h"
#include "call_vp8_encode.h"
#include "screencast.h"
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#if CALL_HWDECODE
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

static guint hardware_transfers;
static gboolean fail_transfer;
static const char *mock_drm_vendor;
static gboolean tried_vaapi;
char *__real_g_file_read_link(const char *path, GError **error);
__attribute__((visibility("default"))) char *
__wrap_g_file_read_link(const char *path, GError **error)
{
	if (mock_drm_vendor && !strcmp(path, "/sys/dev/char/1:3/device"))
		return g_strdup("../../../0000:00:02.0");
	return __real_g_file_read_link(path, error);
}

gboolean __real_g_file_get_contents(const char *path, char **contents,
				    gsize *len, GError **error);
__attribute__((visibility("default"))) gboolean __wrap_g_file_get_contents(
	const char *path, char **contents, gsize *len, GError **error)
{
	if (mock_drm_vendor &&
	    !strcmp(path, "/sys/dev/char/1:3/device/vendor")) {
		*contents = g_strdup(mock_drm_vendor);
		if (len)
			*len = strlen(*contents);
		return TRUE;
	}
	return __real_g_file_get_contents(path, contents, len, error);
}

int __real_av_hwdevice_ctx_create(AVBufferRef **ctx, enum AVHWDeviceType type,
				  const char *node, AVDictionary *opts,
				  int flags);
int __wrap_av_hwdevice_ctx_create(AVBufferRef **ctx, enum AVHWDeviceType type,
				  const char *node, AVDictionary *opts,
				  int flags)
{
	if (mock_drm_vendor) {
		g_assert_cmpint(type, ==, AV_HWDEVICE_TYPE_VAAPI);
		g_assert_cmpstr(node, ==, "/dev/null");
		tried_vaapi = TRUE;
		return AVERROR(ENODEV);
	}
	return __real_av_hwdevice_ctx_create(ctx, type, node, opts, flags);
}

int __real_av_hwframe_transfer_data(AVFrame *dst, const AVFrame *src,
				    int flags);
int __wrap_av_hwframe_transfer_data(AVFrame *dst, const AVFrame *src, int flags)
{
	hardware_transfers++;
	return fail_transfer ? AVERROR_EXTERNAL :
			       __real_av_hwframe_transfer_data(dst, src, flags);
}
#endif

/* A fixed two-frame libvpx stream lets the sender tests exercise the actual
 * decrypt, reassembly and decode paths on machines without a VP8 encoder. */
static const guint8 gray_key[] = {
	0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x10, 0x00, 0x10, 0x00,
	0x00, 0x47, 0x08, 0x85, 0x85, 0x88, 0x85, 0x84, 0x88, 0x02,
	0x02, 0x00, 0x0c, 0x0e, 0x60, 0x00, 0xfe, 0x68, 0x00,
};
static const guint8 gray_delta[] = {
	0xb1, 0x01, 0x00, 0x05, 0x10, 0xac, 0x00, 0x18, 0x00,
	0x18, 0x58, 0x2f, 0xf4, 0x00, 0x08, 0x00, 0x00,
};

static struct encoder_fixture {
	guint opens, frames, closes, kbps, fps;
	gboolean enabled, fail, force_key;
} encoder;

struct call_vp8_encoder *__wrap_call_vp8_encoder_open(guint w, guint h,
						      guint kbps, guint fps,
						      guint key_distance)
{
	if (!encoder.enabled)
		return NULL;
	g_assert_cmpuint(w, ==, 16);
	g_assert_cmpuint(h, ==, 16);
	g_assert_cmpuint(key_distance, >, 0);
	encoder.opens++;
	encoder.kbps = kbps;
	encoder.fps = fps;
	return (struct call_vp8_encoder *)&encoder;
}

gboolean __wrap_call_vp8_encoder_frame(struct call_vp8_encoder *enc,
				       const struct line_camera_frame *frame,
				       gint64 pts, gboolean force_key,
				       const guint8 **data, gsize *size,
				       gboolean *key)
{
	g_assert_true(enc == (struct call_vp8_encoder *)&encoder);
	g_assert_cmpint(pts, >=, 0);
	g_assert_cmpuint(frame->width, ==, 16);
	g_assert_cmpuint(frame->height, ==, 16);
	encoder.frames++;
	if (encoder.fail)
		return FALSE;
	*key = force_key || encoder.force_key;
	*data = *key ? gray_key : gray_delta;
	*size = *key ? sizeof(gray_key) : sizeof(gray_delta);
	return TRUE;
}

const char *__wrap_call_vp8_encoder_name(const struct call_vp8_encoder *enc)
{
	g_assert_true(enc == (struct call_vp8_encoder *)&encoder);
	return "fixture";
}

void __wrap_call_vp8_encoder_close(struct call_vp8_encoder *enc)
{
	if (enc) {
		g_assert_true(enc == (struct call_vp8_encoder *)&encoder);
		encoder.closes++;
	}
}

enum {
	W = 320,
	H = 240
};

static const guint8 key_a[30] = { 1, 2, 3 }, key_b[30] = { 4, 5, 6 };

struct loop {
	struct call_video *tx, *rx;
	GPtrArray *packets;
	guint count, rotation, color_tolerance;
	guint8 pixels[W * H * 3 / 2];
	struct line_camera_frame frame;
};

static gboolean collect(const guint8 *wire, gsize size, gpointer data)
{
	struct loop *loop = data;

	g_assert_cmpuint(size, <=, 1200);
	g_ptr_array_add(loop->packets, g_bytes_new(wire, size));
	return TRUE;
}

static void display(const struct line_camera_frame *frame, gpointer data)
{
	struct loop *loop = data;

	g_assert_cmpuint(frame->width, ==, W);
	g_assert_cmpuint(frame->height, ==, H);
	g_assert_nonnull(frame->plane[2]);
	g_assert_cmpint(frame->stride[1], >=, W / 2);
	g_assert_cmpint(frame->plane[1][0], >=, 100);
	g_assert_cmpint(frame->plane[1][0], <=, 140);
	g_assert_cmpuint(frame->rotation, ==, loop->rotation);
	loop->count++;
}

static void prepare_frame(struct loop *loop)
{
	g_autoptr(GRand) rand = g_rand_new_with_seed(123);
	guint ysize = W * H;

	loop->packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	for (guint i = 0; i < ysize; i++)
		loop->pixels[i] = g_rand_int_range(rand, 24, 180);
	memset(loop->pixels + ysize, 120, ysize / 2);
	loop->frame = (struct line_camera_frame){
		.plane = { loop->pixels, loop->pixels + ysize,
			   loop->pixels + ysize + ysize / 4 },
		.stride = { W, W / 2, W / 2 },
		.width = W,
		.height = H
	};
}

static void prepare(struct loop *loop)
{
	g_autoptr(GError) e = NULL;

	loop->tx = call_video_open(key_a, key_b, 97, 101, 102, 400, 10, &e);
	g_assert_no_error(e);
	loop->rx = call_video_open(key_b, key_a, 97, 102, 101, 400, 10, &e);
	g_assert_no_error(e);
	prepare_frame(loop);
}

static void done(struct loop *loop)
{
	g_ptr_array_unref(loop->packets);
	call_video_close(loop->rx);
	call_video_close(loop->tx);
}

static void encode(struct loop *loop, gint64 time_us)
{
	g_autoptr(GError) e = NULL;
	g_ptr_array_set_size(loop->packets, 0);
	g_assert_true(call_video_encode(loop->tx, &loop->frame, time_us,
					collect, loop, &e));
	g_assert_no_error(e);
}

static gboolean deliver(struct loop *loop, guint i)
{
	gsize size;
	const guint8 *packet = g_bytes_get_data(loop->packets->pdata[i], &size);

	return call_video_receive(loop->rx, packet, size, display, loop);
}

static void decoder_recovery(void)
{
#if CALL_HWDECODE
	struct loop loop = { 0 };
	guint before = hardware_transfers;

	prepare(&loop);
	encode(&loop, 1000000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 1);
	if (before == hardware_transfers) {
		g_test_skip("No hardware VP8 decoder on the selected adapter");
		done(&loop);
		return;
	}
	fail_transfer = TRUE;
	encode(&loop, 1100000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_cmpint(deliver(&loop, i), ==,
				i + 1 < loop.packets->len);
	g_assert_true(call_video_needs_keyframe(loop.rx));
	before = hardware_transfers;
	encode(&loop, 1200000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 1);
	call_video_keyframe(loop.tx);
	encode(&loop, 1300000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 2);
	g_assert_false(call_video_needs_keyframe(loop.rx));
	g_assert_cmpuint(hardware_transfers, ==, before);
	fail_transfer = FALSE;
	done(&loop);
#else
	g_test_skip("Built without FFmpeg acceleration");
#endif
}

static void intel_decoder_selection(void)
{
#if CALL_HWDECODE
	const AVCodec *codec = avcodec_find_decoder_by_name("vp8");
	const AVCodecHWConfig *cfg;
	guint i = 0;

	while (codec && (cfg = avcodec_get_hw_config(codec, i++))) {
		if (cfg->device_type == AV_HWDEVICE_TYPE_VAAPI)
			break;
	}
	if (!codec || !cfg) {
		g_test_skip("FFmpeg was built without VA-API VP8");
		return;
	}
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	/* Exercise node selection and fallback without an Intel device. */
	g_setenv("AVERNELLA_RENDER_NODE", "/dev/null", TRUE);
	g_unsetenv("LINE_DISABLE_GPU");
	mock_drm_vendor = "0x8086";
	struct loop loop = { 0 };

	prepare(&loop);
	encode(&loop, 1000000);
	for (guint j = 0; j < loop.packets->len; j++)
		g_assert_true(deliver(&loop, j));
	g_assert_true(tried_vaapi);
	g_assert_cmpuint(loop.count, ==, 1);
	done(&loop);
#else
	g_test_skip("Built without FFmpeg acceleration");
#endif
}

static void check_visible_edges(const struct line_camera_frame *frame,
				gpointer data)
{
	struct loop *loop = data;
	const int expected[] = { 96, 80, 160 };

	g_assert_cmpuint(frame->width, ==, loop->frame.width);
	g_assert_cmpuint(frame->height, ==, loop->frame.height);
	g_assert_cmpuint(frame->rotation, ==, loop->rotation);
	for (guint i = 0; i < 3; i++) {
		guint width = (frame->width + !!i) >> !!i;
		guint height = (frame->height + !!i) >> !!i;
		const guint8 *first = frame->plane[i];
		const guint8 *last = first + (height - 1) * frame->stride[i];

		g_assert_cmpint(ABS(first[0] - expected[i]), <=,
				loop->color_tolerance);
		g_assert_cmpint(ABS(last[width - 1] - expected[i]), <=,
				loop->color_tolerance);
	}
	loop->count++;
}

static void decoder_visible_dimensions(void)
{
	/* Flat YUV(96,80,160) generated with libvpx. 159x119 and 160x120
	 * have identical macroblocks; only the keyframe dimensions differ. */
	guint8 key[] = {
		0xf0, 0x06, 0x00, 0x9d, 0x01, 0x2a, 0xa0, 0x00, 0x78, 0x00,
		0x00, 0x47, 0x08, 0x85, 0x85, 0x88, 0x85, 0x84, 0x88, 0x02,
		0x02, 0x02, 0x75, 0xaa, 0x03, 0xf8, 0x02, 0x06, 0x93, 0x8f,
		0x05, 0x10, 0x9c, 0x52, 0xd2, 0xa1, 0x38, 0xa5, 0xa5, 0x42,
		0x71, 0x4b, 0x4a, 0x84, 0xe2, 0x96, 0x95, 0x09, 0xc5, 0x2d,
		0x2a, 0x13, 0x8a, 0x5a, 0x54, 0x27, 0x14, 0xb4, 0xa8, 0x4e,
		0x29, 0x69, 0x50, 0x9b, 0x00, 0xfe, 0xfa, 0x79, 0x5f, 0xff,
		0xa8, 0xd9, 0xfa, 0x8d, 0x9f, 0xa8, 0xd9, 0xff, 0xa8, 0xd9,
		0xff, 0xde, 0xc1, 0x7d, 0xd8, 0x2f, 0xbb, 0x05, 0xfe, 0xb6,
		0x00,
	};
	struct loop loop = { .rotation = 270, .color_tolerance = 3 };
	struct call_vp8_decoder *decoder = call_vp8_open(1, FALSE);
	const char *initial = NULL;

	g_assert_nonnull(decoder);
	for (guint i = 0; i < 3; i++) {
		key[6] = loop.frame.width = i == 1 ? 159 : 160;
		key[8] = loop.frame.height = i == 1 ? 119 : 120;
		g_assert_cmpint(call_vp8_decode(decoder, key, sizeof(key), TRUE,
						loop.rotation,
						check_visible_edges, &loop),
				==, 1);
		if (!i)
			initial = call_vp8_backend(decoder);
	}
	g_assert_cmpuint(loop.count, ==, 3);
	g_assert_cmpstr(call_vp8_backend(decoder), ==, initial);
	call_vp8_close(decoder);
}

static void roundtrip(void)
{
	struct loop loop = { 0 };
	guint8 plain[1200];
	guint16 id = 0;
	gsize expected = 0, total = 0;
	srtp_t observer;

	prepare(&loop);
	encode(&loop, 1000000);
	g_assert_cmpuint(loop.packets->len, >, 2);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	for (guint i = 0; i < loop.packets->len; i++) {
		gboolean last = i + 1 == loop.packets->len;
		gsize size;
		const guint8 *packet =
			g_bytes_get_data(loop.packets->pdata[i], &size);
		int n = size;

		memcpy(plain, packet, n);
		g_assert_cmpint(srtp_unprotect(observer, plain, &n), ==,
				srtp_err_status_ok);
		g_assert_cmpuint(plain[1], ==, last ? 0xe1 : 97);
		g_assert_cmpuint(plain[12], ==,
				 i ? (last ? 0x84 : 0x80) : 0xba);
		if (!i) {
			id = plain[13] * 256 + plain[14];
			g_assert_cmpuint(plain[15], ==, 0);
			g_assert_cmpuint(plain[16], ==, 0x22);
			expected = (plain[17] & 3) | (plain[18] << 2) |
				   (plain[19] << 10);
		}
		g_assert_cmpuint(plain[13] * 256 + plain[14], ==, id);
		total += n - (i ? 15 : 20);
	}
	g_assert_cmpuint(total + 3, ==, expected);
	srtp_dealloc(observer);
	/* Fragments can arrive before the start and after the marker. */
	for (guint i = loop.packets->len; i--;)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 1);
	g_assert_false(deliver(&loop, 0));
	encode(&loop, 1010000);
	g_assert_cmpuint(loop.packets->len, ==, 0);
	encode(&loop, 1100000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 2);
	done(&loop);
}

static void conference_directions(void)
{
	g_autoptr(GError) e = NULL;
	struct loop first = { 0 }, second = { 0 };

	first.tx = call_video_open_tx(key_a, 97, 101, 400, 10, &e);
	g_assert_no_error(e);
	first.rx = call_video_open_rx(key_a, 97, 101, &e);
	g_assert_no_error(e);
	second.tx = call_video_open_tx(key_b, 98, 202, 500, 15, &e);
	g_assert_no_error(e);
	second.rx = call_video_open_rx(key_b, 98, 202, &e);
	g_assert_no_error(e);
	prepare_frame(&first);
	prepare_frame(&second);

	g_assert_false(call_video_encode(first.rx, &first.frame, 1000000,
					 collect, &first, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	encode(&first, 1000000);
	encode(&second, 1000000);
	while (first.packets->len || second.packets->len) {
		struct loop *loops[] = { &first, &second };

		for (guint i = 0; i < G_N_ELEMENTS(loops); i++) {
			struct loop *loop = loops[i];
			guint last = loop->packets->len;

			if (!last)
				continue;
			gsize size;
			const guint8 *packet = g_bytes_get_data(
				loop->packets->pdata[last - 1], &size);

			g_assert_false(call_video_receive(
				loops[1 - i]->rx, packet, size, display, loop));
			g_assert_false(call_video_receive(loop->tx, packet,
							  size, display, loop));
			g_assert_true(deliver(loop, last - 1));
			g_ptr_array_set_size(loop->packets, last - 1);
		}
	}
	g_assert_cmpuint(first.count, ==, 1);
	g_assert_cmpuint(second.count, ==, 1);
	done(&first);
	done(&second);
}

static void recovery(void)
{
	struct loop loop = { 0 };
	gsize size;

	prepare(&loop);
	encode(&loop, 1000000);
	const guint8 *p = g_bytes_get_data(loop.packets->pdata[0], &size);

	g_autofree guint8 *changed = g_memdup2(p, size);

	changed[size - 1] ^= 1;
	g_assert_false(
		call_video_receive(loop.rx, changed, size, display, &loop));
	changed[8] ^= 1;
	g_assert_false(
		call_video_receive(loop.rx, changed, size, display, &loop));
	g_assert_true(deliver(&loop, 0));
	for (guint i = 2; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 0);
	g_assert_true(call_video_needs_keyframe(loop.rx));
	encode(&loop, 1100000);
	for (guint i = 0; i < loop.packets->len; i++)
		deliver(&loop, i);
	g_assert_cmpuint(loop.count, ==, 0);
	call_video_keyframe(loop.tx);
	encode(&loop, 1200000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 1);
	g_assert_false(call_video_needs_keyframe(loop.rx));
	done(&loop);
}

struct source_view {
	struct call_video *rx;
	guint frames;
	gboolean screen;
};

static void source_display(const struct line_camera_frame *frame, gpointer data)
{
	struct source_view *view = data;

	g_assert_cmpuint(frame->width, ==, 16);
	g_assert_cmpuint(frame->height, ==, 16);
	view->screen = call_video_frame_is_screen(view->rx);
	view->frames++;
}

/* A known VP8 keyframe with a compact EVS3 header. First and last
 * fragments can be submitted independently of camera capture or encoding.
 */
static gboolean source_fragment(struct source_view *view, srtp_t sender,
				guint8 seq, guint8 picture, int source,
				guint offset, guint length)
{
	guint8 packet[128] = { 0x80, 121, 0, seq, 0,	0, 0,	   picture,
			       0,    0,	  0, 101, 0x80, 0, picture };
	int n = 15;

	g_assert_cmpuint(offset + length, <=, sizeof(gray_key));
	if (offset + length == sizeof(gray_key)) {
		packet[1] |= 0x80;
		packet[12] |= 0x04;
	}
	if (!offset) {
		unsigned int size = sizeof(gray_key) + 3;

		packet[12] |= 0x3a;
		packet[n++] = 0x10;
		packet[n++] = source < 0 ? 0x20 : 0x21;
		if (source >= 0) {
			guint8 tag[] = { 0x80, 0x84, 1, picture, 2, source };

			memcpy(packet + n, tag, sizeof(tag));
			n += sizeof(tag);
		}
		packet[n++] = 0x08 | (size & 3);
		packet[n++] = size >> 2;
		packet[n++] = size >> 10;
	}
	memcpy(packet + n, gray_key + offset, length);
	n += length;
	g_assert_cmpint(srtp_protect(sender, packet, &n), ==,
			srtp_err_status_ok);
	return call_video_receive(view->rx, packet, n, source_display, view);
}

static void group_source_visibility(void)
{
	enum {
		FULL = sizeof(gray_key),
		FIRST = 18,
		TAIL = FULL - FIRST
	};
	static const struct {
		guint8 seq, picture;
		int source;
		guint offset, length, displayed;
		gboolean screen, accepted;
	} packets[] = {
		{ 1, 1, 0, 0, FULL, 1, FALSE, TRUE },
		/* Lose the end of a screen picture. The next key has no source. */
		{ 2, 2, 1, 0, FIRST, 1, FALSE, TRUE },
		{ 4, 3, -1, 0, FULL, 2, FALSE, TRUE },
		{ 3, 2, -1, FIRST, TAIL, 2, FALSE, FALSE },
		/* Reorder a complete screen picture, then omit its optional tag. */
		{ 6, 4, -1, FIRST, TAIL, 2, FALSE, TRUE },
		{ 5, 4, 1, 0, FIRST, 3, TRUE, TRUE },
		{ 7, 5, -1, 0, FULL, 4, TRUE, TRUE },
		/* Keep the screen visible until the camera keyframe completes. */
		{ 8, 6, 0, 0, FIRST, 4, TRUE, TRUE },
		{ 9, 6, -1, FIRST, TAIL, 5, FALSE, TRUE },
	};
	struct source_view view = { 0 };
	srtp_t sender;

	view.rx = call_video_open_group_rx(key_a, 121, 101, NULL);
	g_assert_nonnull(view.rx);
	g_assert_true(call_srtp_open(&sender, key_a, ssrc_specific, 101));
	for (guint i = 0; i < G_N_ELEMENTS(packets); i++) {
		gboolean accepted =
			source_fragment(&view, sender, packets[i].seq,
					packets[i].picture, packets[i].source,
					packets[i].offset, packets[i].length);

		g_assert_cmpint(accepted, ==, packets[i].accepted);
		g_assert_cmpuint(view.frames, ==, packets[i].displayed);
		g_assert_cmpint(view.screen, ==, packets[i].screen);
		g_assert_cmpint(call_video_frame_is_screen(view.rx), ==,
				packets[i].screen);
	}
	srtp_dealloc(sender);
	call_video_close(view.rx);
}

static void evs3_picture_metadata(void)
{
	struct loop loop = { 0 };
	srtp_t observer, sender;

	prepare(&loop);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	g_assert_true(call_srtp_open(&sender, key_a, ssrc_specific, 101));

	for (guint i = 0; i < 2; i++) {
		guint8 packet[1200 + SRTP_MAX_TRAILER_LEN];
		gsize size;

		encode(&loop, 1000000 + 100000 * i);
		g_assert_cmpuint(loop.packets->len, >, 0);
		const guint8 *original =
			g_bytes_get_data(loop.packets->pdata[0], &size);
		int n = size;

		memcpy(packet, original, size);
		g_assert_cmpint(srtp_unprotect(observer, packet, &n), ==,
				srtp_err_status_ok);
		g_assert_cmphex(packet[12] & 0x28, ==, 0x28);
		/* Base spatial layer with reference and temporal flags. */
		packet[15] = i ? 0x12 : 0x10;
		/* Orientation can change between pictures without a new stream. */
		packet[i ? 19 : 16] |= i ? 0xc0 : 0x40;
		loop.rotation = i ? 270 : 90;
		g_assert_cmpint(srtp_protect(sender, packet, &n), ==,
				srtp_err_status_ok);
		g_assert_true(
			call_video_receive(loop.rx, packet, n, display, &loop));
		for (guint j = 1; j < loop.packets->len; j++)
			g_assert_true(deliver(&loop, j));
		g_assert_cmpuint(loop.count, ==, i + 1);
	}
	srtp_dealloc(sender);
	srtp_dealloc(observer);
	done(&loop);
}

static void reframe_vp8(struct loop *loop, srtp_t observer, srtp_t sender)
{
	for (guint i = 0; i < loop->packets->len; i++) {
		GBytes *original = loop->packets->pdata[i];
		gsize size;
		const guint8 *wire = g_bytes_get_data(original, &size);
		guint8 packet[1200 + SRTP_MAX_TRAILER_LEN];
		int n = size;

		memcpy(packet, wire, size);
		g_assert_cmpint(srtp_unprotect(observer, packet, &n), ==,
				srtp_err_status_ok);
		guint8 descriptor[] = {
			i ? 0x80 : 0x90, 0xf0, 0x81, 0x07, 0x09, 0x00,
		};
		guint pos = i ? 15 : (packet[12] & 0x40 ? 23 : 20);

		g_assert_cmpuint(n, >, pos);
		memmove(packet + 12 + sizeof(descriptor), packet + pos,
			n - pos);
		memcpy(packet + 12, descriptor, sizeof(descriptor));
		n = n - pos + 12 + sizeof(descriptor);
		g_assert_cmpint(srtp_protect(sender, packet, &n), ==,
				srtp_err_status_ok);
		loop->packets->pdata[i] = g_bytes_new(packet, n);
		g_bytes_unref(original);
	}
}

static void standard_vp8(void)
{
	struct loop loop = { 0 };
	srtp_t observer, sender;

	prepare(&loop);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	g_assert_true(call_srtp_open(&sender, key_a, ssrc_specific, 101));
	for (guint i = 0; i < 2; i++) {
		encode(&loop, 1000000 + i * 100000);
		reframe_vp8(&loop, observer, sender);
		for (guint j = loop.packets->len; j--;)
			g_assert_true(deliver(&loop, j));
		g_assert_cmpuint(loop.count, ==, i + 1);
	}
	srtp_dealloc(sender);
	srtp_dealloc(observer);

	g_ptr_array_set_size(loop.packets, 0);
	g_assert_true(call_video_send_feedback(loop.rx, collect, &loop));
	g_assert_cmpuint(loop.packets->len, ==, 1);
	gsize size;
	const guint8 *wire = g_bytes_get_data(loop.packets->pdata[0], &size);
	guint8 packet[64];
	int n = size;

	memcpy(packet, wire, size);
	g_assert_true(call_srtp_open(&observer, key_b, ssrc_specific, 102));
	g_assert_cmpint(srtp_unprotect_rtcp(observer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpuint(n, ==, 12);
	g_assert_cmpuint(packet[0], ==, 0x81);
	g_assert_cmpuint(packet[1], ==, 206);
	g_assert_true(call_video_receive_feedback(loop.tx, wire, size));
	srtp_dealloc(observer);
	done(&loop);
}

static gboolean ignore_feedback(const guint8 *packet, gsize size, gpointer data)
{
	(void)packet;
	(void)size;
	(void)data;
	return TRUE;
}

static void single_packet(gconstpointer mode)
{
	gboolean rfc = GPOINTER_TO_INT(mode);
	struct loop loop = { 0 };
	srtp_t observer, sender;

	prepare(&loop);
	memset(loop.pixels, 120, W * H);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	g_assert_true(call_srtp_open(&sender, key_a, ssrc_specific, 101));
	for (guint i = 0; i < 5; i++) {
		if (i == 4)
			call_video_keyframe(loop.tx);
		encode(&loop, 1000000 + i * 100000);
		g_assert_cmpuint(loop.packets->len, ==, 1);
		if (rfc)
			reframe_vp8(&loop, observer, sender);
		if (i == 2)
			continue; /* Losing a whole frame requires a keyframe. */
		g_assert_true(deliver(&loop, 0));
		/* The next frame waits for reordering or a resend first. */
		if (i == 3)
			call_video_poll(loop.rx, loop.rx,
					g_get_monotonic_time() + G_USEC_PER_SEC,
					FALSE, ignore_feedback, NULL, display,
					&loop);
		g_assert_cmpuint(loop.count, ==, i == 4 ? 3 : MIN(i + 1, 2));
		g_assert_cmpint(call_video_needs_keyframe(loop.rx), ==, i == 3);
	}
	srtp_dealloc(sender);
	srtp_dealloc(observer);
	done(&loop);
}

static void single_packet_length(gconstpointer data)
{
	struct loop loop = { 0 };
	srtp_t observer, sender;
	guint8 packet[1200 + SRTP_MAX_TRAILER_LEN];
	gsize size;

	prepare(&loop);
	if (GPOINTER_TO_INT(data)) {
		call_video_close(loop.rx);
		loop.rx = call_video_open_group_rx(key_a, 97, 101, NULL);
		g_assert_nonnull(loop.rx);
	}
	memset(loop.pixels, 120, W * H);
	encode(&loop, 1000000);
	g_assert_cmpuint(loop.packets->len, ==, 1);
	const guint8 *wire = g_bytes_get_data(loop.packets->pdata[0], &size);
	int n = size;

	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	g_assert_true(call_srtp_open(&sender, key_a, ssrc_specific, 101));
	memcpy(packet, wire, size);
	g_assert_cmpint(srtp_unprotect(observer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmphex(packet[12], ==, 0xbe); /* Single EVS3 keyframe. */
	packet[17] ^= 1; /* Change the declared length, keep the VP8 data. */
	g_assert_cmpint(srtp_protect(sender, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_false(call_video_receive(loop.rx, packet, n, display, &loop));
	g_assert_cmpuint(loop.count, ==, 0);
	g_assert_true(call_video_needs_keyframe(loop.rx));
	call_video_keyframe(loop.tx);
	encode(&loop, 1100000);
	g_assert_true(deliver(&loop, 0));
	g_assert_cmpuint(loop.count, ==, 1);
	srtp_dealloc(sender);
	srtp_dealloc(observer);
	done(&loop);
}

enum profile_length_error {
	PROFILE_VALID,
	PROFILE_OUTER_MISMATCH,
	PROFILE_INNER_MISMATCH,
	PROFILE_TRUNCATED,
};

static void reframe_evs3_size(struct loop *loop, srtp_t observer, srtp_t sender,
			      guint header, enum profile_length_error error)
{
	static const guint8 extensions[] = { 5, 2, 0, 0, 0x80, 0x87, 0,
					     0, 0, 0, 0, 0,    0 };
	GBytes *original = loop->packets->pdata[0];
	gsize size;
	const guint8 *wire = g_bytes_get_data(original, &size);
	guint8 packet[1200 + SRTP_MAX_TRAILER_LEN];
	int n = size;

	memcpy(packet, wire, size);
	g_assert_cmpint(srtp_unprotect(observer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_true(packet[12] & 8);
	/* The synthetic camera emits no optional extensions on its descriptor. */
	guint offset = 12 + (packet[12] & 0x40 ? 8 : 5);

	g_assert_cmpint(n, >, offset + 3);
	guint extension_size = header >= 9 ? sizeof(extensions) : 0;
	guint total = (packet[offset] & 3) | (packet[offset + 1] << 2) |
		      (packet[offset + 2] << 10);
	guint payload = total - 3 + (error == PROFILE_TRUNCATED);

	memmove(packet + offset + extension_size + header, packet + offset + 3,
		n - offset - 3);
	n += (int)(extension_size + header) - 3;
	if (extension_size) {
		guint32 size = GUINT32_TO_BE(payload + header - 6 +
					     (error == PROFILE_OUTER_MISMATCH));

		packet[offset - 1] |= 1;
		memcpy(packet + offset, extensions, extension_size);
		offset += extension_size;
		packet[offset++] = header == 9 ? 3 : 4;
		packet[offset++] = 0x10;
		memcpy(packet + offset, &size, sizeof(size));
		offset += sizeof(size);
	}
	payload += error == PROFILE_INNER_MISMATCH;
	if (header == 9) {
		total = payload + 3;
		packet[offset] = total & 3;
		packet[offset + 1] = total >> 2;
		packet[offset + 2] = total >> 10;
	} else if (header) {
		guint32 size = GUINT32_TO_BE(payload);

		memcpy(packet + offset, &size, sizeof(size));
	}
	g_assert_cmpint(srtp_protect(sender, packet, &n), ==,
			srtp_err_status_ok);
	loop->packets->pdata[0] = g_bytes_new(packet, n);
	g_bytes_unref(original);
}

static void group_evs3_sizes(void)
{
	static const struct {
		guint header;
		gboolean key;
		enum profile_length_error error;
	} pictures[] = {
		{ 0, TRUE, PROFILE_VALID },
		{ 0, FALSE, PROFILE_VALID },
		{ 4, TRUE, PROFILE_VALID },
		{ 4, FALSE, PROFILE_VALID },
		{ 10, TRUE, PROFILE_VALID },
		{ 10, FALSE, PROFILE_VALID },
		{ 10, TRUE, PROFILE_TRUNCATED },
		{ 10, TRUE, PROFILE_OUTER_MISMATCH },
		{ 10, TRUE, PROFILE_INNER_MISMATCH },
		{ 9, TRUE, PROFILE_VALID },
		{ 9, FALSE, PROFILE_VALID },
		{ 9, TRUE, PROFILE_OUTER_MISMATCH },
		{ 9, TRUE, PROFILE_INNER_MISMATCH },
		{ 9, TRUE, PROFILE_TRUNCATED },
		{ 3, TRUE, PROFILE_VALID },
		{ 3, FALSE, PROFILE_VALID },
	};
	struct loop loop = { 0 };
	srtp_t observer, sender;
	guint shown = 0;

	prepare(&loop);
	call_video_close(loop.rx);
	loop.rx = call_video_open_group_rx(key_a, 97, 101, NULL);
	g_assert_nonnull(loop.rx);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	g_assert_true(call_srtp_open(&sender, key_a, ssrc_specific, 101));
	for (guint i = 0; i < G_N_ELEMENTS(pictures); i++) {
		gboolean valid = pictures[i].error == PROFILE_VALID;

		if (pictures[i].key)
			call_video_keyframe(loop.tx);
		encode(&loop, 1000000 + i * 100000);
		if (pictures[i].header != 3)
			reframe_evs3_size(&loop, observer, sender,
					  pictures[i].header,
					  pictures[i].error);
		for (guint j = loop.packets->len; j--;)
			g_assert_cmpint(deliver(&loop, j), ==, j || valid);
		shown += valid;
		g_assert_cmpuint(loop.count, ==, shown);
	}
	srtp_dealloc(sender);
	srtp_dealloc(observer);
	done(&loop);
}

struct expected_image {
	guint width, height, frames;
};

static void check_resolution(const struct line_camera_frame *frame,
			     gpointer data)
{
	struct expected_image *expected = data;

	g_assert_cmpuint(frame->width, ==, expected->width);
	g_assert_cmpuint(frame->height, ==, expected->height);
	for (guint i = 0; i < 3; i++) {
		guint w = frame->width >> !!i, h = frame->height >> !!i;
		guint8 last =
			frame->plane[i][(h - 1) * frame->stride[i] + w - 1];

		/* Padding in the incoming camera rows is white. */
		g_assert_cmpint(ABS((int)last - (64 + (int)i * 40)), <=, 8);
	}
	expected->frames++;
}

static void group_resolution(void)
{
	static const struct {
		int requested;
		guint width, height, descriptor, rate;
		gboolean key;
	} phases[] = {
		{ -1, 640, 480, 2, 20, TRUE }, { 0, 160, 120, 0, 7, TRUE },
		{ 0, 160, 120, 0, 7, FALSE },  { 1, 480, 360, 1, 15, TRUE },
		{ -1, 640, 480, 2, 20, TRUE }, { 4, 640, 480, 2, 20, FALSE },
	};
	struct loop loop = { 0 };
	struct expected_image expected = { 0 };
	guint16 layer_sequences[8] = { 0 };
	static const int strides[] = { 656, 344, 352 };
	g_autofree guint8 *storage =
		g_malloc(480 * strides[0] + 240 * (strides[1] + strides[2]));
	guint8 *plane = storage;
	struct line_camera_frame *f = &loop.frame;

	loop.tx = call_video_open_group_tx(key_a, 121, 101, 400, 10, NULL);
	loop.rx = call_video_open_group_rx(key_a, 121, 101, NULL);
	loop.packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	g_assert_nonnull(loop.tx);
	g_assert_nonnull(loop.rx);
	call_video_set_group_format(loop.tx, CALL_VIDEO_RTP_EXT_VED, TRUE);
	f->width = 640;
	f->height = 480;
	for (guint i = 0; i < 3; i++) {
		guint w = f->width >> !!i, h = f->height >> !!i;

		f->stride[i] = strides[i];
		f->plane[i] = plane;
		for (guint y = 0; y < h; y++, plane += f->stride[i]) {
			memset(plane, 64 + i * 40, w);
			memset(plane + w, 255, f->stride[i] - w);
		}
	}
	for (guint i = 0; i < G_N_ELEMENTS(phases); i++) {
		if (i == 4) {
			/* A camera with matching chroma strides can be encoded
			 * directly on restore, releasing the scaled buffer. */
			guint8 *v = (guint8 *)f->plane[2];

			for (guint row = 0; row < f->height / 2; row++)
				memmove(v + row * f->stride[1],
					v + row * f->stride[2], f->width / 2);
			f->stride[2] = f->stride[1];
		}
		expected.width = phases[i].width;
		expected.height = phases[i].height;
		call_video_set_group_resolution(loop.tx, phases[i].requested);
		encode(&loop, 1000000 + i * 100000);
		g_assert_cmpuint(loop.packets->len, >, 0);
		for (guint j = 0; j < loop.packets->len; j++) {
			gsize n;
			const guint8 *p =
				g_bytes_get_data(loop.packets->pdata[j], &n);

			/* Thumbnail, grid and larger layers have separate sequences. */
			guint sid =
				phases[i].descriptor ? phases[i].descriptor : 7;
			guint id = (sid << 5) | 4;

			g_assert_cmpuint(n, >, 32);
			g_assert_cmpuint(p[22], ==, id);
			g_assert_cmpuint(p[26] & 0xfc, ==, id);
			g_assert_cmpuint(((guint)p[28] << 8) | p[29], ==,
					 ++layer_sequences[sid]);
			if (!j && phases[i].key)
				g_assert_cmpuint(p[31], ==, id);
			g_assert_cmpuint(p[23] >> 5, ==, phases[i].descriptor);
			g_assert_cmpuint(p[25], ==, phases[i].rate);
			if (!j && phases[i].key)
				g_assert_cmpuint(p[32], ==, phases[i].rate);
			g_assert_cmpint(!!(p[23] & 2), ==, phases[i].key);
			g_assert_true(call_video_receive(
				loop.rx, p, n, check_resolution, &expected));
		}
		g_assert_cmpuint(expected.frames, ==, i + 1);
	}
	done(&loop);
}

/* A camera delivering 20 frames/s must fit the 15 frames/s thumbnail
 * layer without dropping to 10 frames/s through a relative timeout. */
static void group_thumbnail_rate(void)
{
	struct loop loop = { 0 };
	guint frames = 0;

	loop.tx = call_video_open_group_tx(key_a, 121, 101, 4000, 30, NULL);
	g_assert_nonnull(loop.tx);
	call_video_set_group_format(loop.tx, CALL_VIDEO_RTP_EXT_VED, TRUE);
	call_video_set_group_resolution(loop.tx, 0);
	prepare_frame(&loop);
	for (guint i = 0; i < 40; i++) {
		encode(&loop, 1000000 + i * 50000);
		frames += loop.packets->len > 0;
	}
	g_assert_cmpuint(frames, ==, 30);
	done(&loop);
}

static void group_resolution_aspect(void)
{
	static const struct {
		guint width, height, output_width, output_height;
	} cases[] = {
		/* Class 1 holds up to 172,800 pixels in the source's shape. */
		{ 640, 360, 554, 310 },	 { 1280, 720, 554, 310 },
		{ 720, 1280, 310, 554 }, { 1920, 1080, 554, 310 },
		{ 160, 120, 160, 120 },
	};
	struct loop loop = { 0 };
	struct expected_image expected = { 0 };

	loop.tx = call_video_open_group_tx(key_a, 121, 101, 400, 10, NULL);
	loop.rx = call_video_open_group_rx(key_a, 121, 101, NULL);
	loop.packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	g_assert_nonnull(loop.tx);
	g_assert_nonnull(loop.rx);
	call_video_set_group_format(loop.tx, CALL_VIDEO_RTP_EXT_VED, TRUE);
	call_video_set_group_resolution(loop.tx, 1);
	for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
		guint width = cases[i].width, height = cases[i].height;
		g_autofree guint8 *image =
			g_malloc(line_camera_i420_size(width, height));

		line_camera_i420_frame(&loop.frame, image, width, height);
		for (guint j = 0; j < 3; j++)
			memset((guint8 *)loop.frame.plane[j], 64 + j * 40,
			       (gsize)loop.frame.stride[j] * (height >> !!j));
		expected.width = cases[i].output_width;
		expected.height = cases[i].output_height;
		encode(&loop, 1000000 + i * G_USEC_PER_SEC);
		for (guint j = 0; j < loop.packets->len; j++) {
			gsize n;
			const guint8 *packet =
				g_bytes_get_data(loop.packets->pdata[j], &n);

			g_assert_true(call_video_receive(loop.rx, packet, n,
							 check_resolution,
							 &expected));
		}
		g_assert_cmpuint(expected.frames, ==, i + 1);
	}
	done(&loop);
}

/* A shared screen keeps the HD layer for a viewer that requested a grid
 * tile, while the camera on the same stream fills the smaller class. */
static void group_screen_resolution(void)
{
	static const struct {
		guint screen_kbps, width, height, descriptor;
	} cases[] = {
		{ 3000, 1280, 720, 3 },
		{ 0, 554, 310, 1 },
		{ 3000, 1280, 720, 3 },
	};
	struct loop loop = { 0 };
	struct expected_image expected = { 0 };
	g_autofree guint8 *image = g_malloc(line_camera_i420_size(1280, 720));

	loop.tx = call_video_open_group_tx(key_a, 121, 101, 3000, 30, NULL);
	loop.rx = call_video_open_group_rx(key_a, 121, 101, NULL);
	loop.packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	g_assert_nonnull(loop.tx);
	g_assert_nonnull(loop.rx);
	call_video_set_group_format(loop.tx, CALL_VIDEO_RTP_EXT_VED, TRUE);
	call_video_set_group_resolution(loop.tx, 1);
	line_camera_i420_frame(&loop.frame, image, 1280, 720);
	for (guint j = 0; j < 3; j++)
		memset((guint8 *)loop.frame.plane[j], 64 + j * 40,
		       (gsize)loop.frame.stride[j] * (720 >> !!j));
	for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
		call_video_screen(loop.tx, cases[i].screen_kbps, 10);
		expected.width = cases[i].width;
		expected.height = cases[i].height;
		encode(&loop, 1000000 + i * G_USEC_PER_SEC);
		g_assert_cmpuint(loop.packets->len, >, 0);
		for (guint j = 0; j < loop.packets->len; j++) {
			gsize n;
			const guint8 *p =
				g_bytes_get_data(loop.packets->pdata[j], &n);

			g_assert_cmpuint(n, >, 32);
			g_assert_cmpuint(p[23] >> 5, ==, cases[i].descriptor);
			g_assert_true(call_video_receive(
				loop.rx, p, n, check_resolution, &expected));
		}
		g_assert_cmpuint(expected.frames, ==, i + 1);
	}
	done(&loop);
}

struct group_test_format {
	enum call_video_rtp_extension extension;
	gboolean vp8a;
};

static void group_evs3_tx(gconstpointer data)
{
	static const struct {
		guint screen_kbps;
		guint8 generation, source, ved_rate, vfd_rate;
		gboolean key;
	} pictures[] = {
		/* A 320x240 screen keeps the 300 kbit/s class 1 budget. */
		{ 0, 1, 0, 15, 37, TRUE },    { 0, 1, 0, 15, 37, FALSE },
		{ 400, 2, 0, 15, 37, TRUE },  { 500, 2, 0, 15, 37, TRUE },
		{ 8000, 2, 0, 15, 37, TRUE }, { 0, 3, 0, 15, 37, TRUE },
	};
	struct loop loop = { 0 };
	srtp_t observer;
	guint16 layer_seq = 0;
	const struct group_test_format *format = data;
	gboolean report =
		!g_strcmp0(g_getenv("AVERNELLA_TEST_MEDIA_CONTRACT"), "1");
	enum call_video_rtp_extension extension = format->extension;
	guint profile = format->vp8a ? 4 : 3;

	loop.tx = call_video_open_group_tx(key_a, 121, 101, 400, 10, NULL);
	loop.rx = call_video_open_group_rx(key_a, 121, 101, NULL);
	g_assert_nonnull(loop.tx);
	g_assert_nonnull(loop.rx);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, 101));
	call_video_set_group_format(loop.tx, extension, format->vp8a);
	prepare_frame(&loop);
	for (guint frame = 0; frame < G_N_ELEMENTS(pictures); frame++) {
		gboolean key = pictures[frame].key;
		gboolean single = frame + 1 == G_N_ELEMENTS(pictures);
		guint32 total = 0, profile_size = 0, codec_size = 0;
		guint8 source_record[4] = { 0 }, codec_header[4] = { 0 };

		if (single)
			memset(loop.pixels, 120, W * H);
		call_video_screen(loop.tx, pictures[frame].screen_kbps, 10);
		encode(&loop, 1000000 + frame * 100000);
		if (single)
			g_assert_cmpuint(loop.packets->len, ==, 1);
		else
			g_assert_cmpuint(loop.packets->len, >, key ? 1 : 0);
		for (guint i = 0; i < loop.packets->len; i++) {
			gboolean last = i + 1 == loop.packets->len;
			guint rtp = 12;
			guint8 packet[1200];
			gsize size;
			const guint8 *wire =
				g_bytes_get_data(loop.packets->pdata[i], &size);
			int n = size;

			g_assert_cmpuint(size, <=, sizeof(packet));
			memcpy(packet, wire, size);
			g_assert_cmpint(srtp_unprotect(observer, packet, &n),
					==, srtp_err_status_ok);
			g_assert_cmphex(packet[0], ==, extension ? 0x90 : 0x80);
			g_assert_cmpuint(packet[1], ==, 121 | (last ? 128 : 0));
			if (extension == CALL_VIDEO_RTP_EXT_VFD) {
				const guint8 vfd[] = {
					2,
					0,
					0,
					2,
					2,
					3,
					0x89 | (last ? 0x40 : 0),
					0x18 | (key ? 4 : 0) | (!i ? 2 : 0),
					profile,
					pictures[frame].vfd_rate,
					packet[2],
					packet[3],
				};

				g_assert_cmpmem(packet + rtp, sizeof(vfd), vfd,
						sizeof(vfd));
				rtp += sizeof(vfd);
			} else if (extension == CALL_VIDEO_RTP_EXT_VED) {
				/* The first fragment of each key picture advertises
				 * the rate of its single base layer. */
				gboolean target = !i && key;
				guint pd = rtp + (target ? 24 : 20);

				++layer_seq;
				const guint8 ved[] = {
					2,
					0,
					0,
					target ? 5 : 4,
					4,
					target ? 8 : 6,
					target ? 0x22 : 0x02,
					0,
					packet[pd + 1],
					packet[pd + 2],
					0x24,
					key ? 0x26 : 0x24,
					profile,
					pictures[frame].ved_rate,
					0x24 | (!i ? 2 : 0) | (last ? 1 : 0),
					key ? 0x38 : 0x18,
					layer_seq >> 8,
					(guint8)layer_seq,
				};

				g_assert_cmpmem(packet + rtp, sizeof(ved), ved,
						sizeof(ved));
				rtp += sizeof(ved);
				if (target) {
					const guint8 opt[] = {
						0xa1,
						0x24,
						pictures[frame].ved_rate,
						0,
					};

					g_assert_cmpmem(packet + rtp,
							sizeof(opt), opt,
							sizeof(opt));
					rtp += sizeof(opt);
				}
				g_assert_cmpuint(packet[rtp++], ==, 0);
				g_assert_cmpuint(packet[rtp++], ==, 0);
			}
			guint offset = rtp + 3;

			g_assert_cmpuint(packet[rtp], ==,
					 (i ? 0x80 : (key ? 0xba : 0xfa)) |
						 (last ? 4 : 0));
			if (!i) {
				guint descriptor = rtp + (key ? 5 : 8);
				const guint8 capture[] = {
					0x80, 4,
					1,    pictures[frame].generation,
					2,    pictures[frame].source,
				};

				/* Class 1 uses spatial ID 1 and temporal ID 1. */
				g_assert_cmphex(packet[rtp + 3], ==,
						extension ? 0x22 : 0x10);
				g_assert_cmphex(packet[descriptor - 1], ==,
						key ? 0x23 : 0x03);
				g_assert_cmpmem(packet + descriptor,
						sizeof(capture), capture,
						sizeof(capture));
				memcpy(source_record, packet + descriptor + 2,
				       sizeof(source_record));
				descriptor += sizeof(capture);
				g_assert_cmphex(packet[descriptor], ==, 5);
				g_assert_cmphex(packet[descriptor + 1], ==,
						extension ? 2 : 0x82);
				descriptor += 4;
				if (extension) {
					const guint8 base[] = { 8, 0x81, 0x24 };

					g_assert_cmpint(
						n, >,
						descriptor + sizeof(base) + 6);
					g_assert_cmpmem(packet + descriptor,
							sizeof(base), base,
							sizeof(base));
					descriptor += sizeof(base);
					g_assert_cmpuint(packet[descriptor], ==,
							 profile);
					g_assert_cmpuint(packet[descriptor + 1],
							 ==, key ? 0x32 : 0x22);
					memcpy(&profile_size,
					       packet + descriptor + 2, 4);
					descriptor += 6;
				}
				if (format->vp8a) {
					offset = descriptor + 4;
					g_assert_cmpint(n, >, offset);
					memcpy(&codec_size, packet + descriptor,
					       4);
					codec_size =
						GUINT32_FROM_BE(codec_size);
				} else {
					offset = descriptor + 3;
					g_assert_cmpint(n, >, offset);
					guint declared =
						(packet[descriptor] & 3) |
						(packet[descriptor + 1] << 2) |
						(packet[descriptor + 2] << 10);

					g_assert_cmphex(packet[descriptor] &
								0xfc,
							==, 0x08);
					g_assert_cmpuint(declared, >, 3);
					codec_size = declared - 3;
				}
				memcpy(codec_header, packet + descriptor,
				       offset - descriptor);
				g_assert_cmpuint(packet[offset] & 1, ==, !key);
			}
			total += n - offset;
		}
		if (extension)
			g_assert_cmpuint(GUINT32_FROM_BE(profile_size), ==,
					 total + (format->vp8a ? 4 : 3));
		g_assert_cmpuint(codec_size, ==, total);
		if (report)
			g_test_message("group-media %u %u %02x%02x%02x%02x "
				       "%02x%02x%02x%02x",
				       frame, total, source_record[0],
				       source_record[1], source_record[2],
				       source_record[3], codec_header[0],
				       codec_header[1], codec_header[2],
				       codec_header[3]);
		for (guint i = loop.packets->len; i--;)
			g_assert_true(deliver(&loop, i));
		g_assert_cmpuint(loop.count, ==, frame + 1);
	}
	srtp_dealloc(observer);
	done(&loop);
}

static void encrypted_feedback(void)
{
	struct loop loop = { 0 };
	srtp_t observer;
	guint8 packet[64];
	gsize size;

	prepare(&loop);
	g_assert_true(call_video_needs_keyframe(loop.rx));
	encode(&loop, 1000000);
	g_ptr_array_set_size(loop.packets, 0);
	g_assert_true(call_video_send_feedback(loop.rx, collect, &loop));
	g_assert_cmpuint(loop.packets->len, ==, 1);
	const guint8 *wire = g_bytes_get_data(loop.packets->pdata[0], &size);

	memcpy(packet, wire, size);
	packet[size - 1] ^= 1;
	g_assert_false(call_video_receive_feedback(loop.tx, packet, size));
	g_assert_true(call_video_receive_feedback(loop.tx, wire, size));
	g_assert_false(call_video_receive_feedback(loop.tx, wire, size));

	memcpy(packet, wire, size);
	int n = size;

	g_assert_true(call_srtp_open(&observer, key_b, ssrc_specific, 102));
	g_assert_cmpint(srtp_unprotect_rtcp(observer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpuint(n, ==, 28);
	static const guint8 header[] = {
		0x91, 206, 0, 6, 0, 0, 0, 102, 0, 0, 0, 101,
	};
	g_assert_cmpmem(packet, sizeof(header), header, sizeof(header));
	srtp_dealloc(observer);

	/* The encrypted request triggers a fresh keyframe after the first was lost. */
	encode(&loop, 1100000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 1);
	g_assert_false(call_video_needs_keyframe(loop.rx));
	done(&loop);
}

static void conference_feedback(void)
{
	struct loop loop = {
		.tx = call_video_open_tx(key_a, 97, 100, 400, 10, NULL),
		.packets = g_ptr_array_new_with_free_func(
			(GDestroyNotify)g_bytes_unref),
	};
	struct call_video *const targets[] = {
		call_video_open_rx(key_b, 97, 101, NULL),
		call_video_open_rx(key_b, 97, 102, NULL),
	};
	srtp_t relay;

	g_assert_nonnull(loop.tx);
	g_assert_true(call_srtp_open(&relay, key_a, ssrc_specific, 100));
	for (guint i = 0; i < 3; i++) {
		struct call_video *target = targets[i % G_N_ELEMENTS(targets)];
		guint8 packet[64];
		gsize size;

		g_assert_nonnull(target);
		g_assert_true(call_video_send_peer_feedback(loop.tx, target,
							    collect, &loop));
		g_assert_cmpuint(loop.packets->len, ==, i + 1);
		const guint8 *wire =
			g_bytes_get_data(loop.packets->pdata[i], &size);
		int n = size;

		g_assert_cmpuint(size, <=, sizeof(packet));
		memcpy(packet, wire, n);
		/* A single relay context rejects reusing an SRTCP index. */
		g_assert_cmpint(srtp_unprotect_rtcp(relay, packet, &n), ==,
				srtp_err_status_ok);
		const guint8 header[] = {
			0x91, 206, 0, 6, 0, 0,
			0,    100, 0, 0, 0, 101 + i % G_N_ELEMENTS(targets),
		};

		g_assert_cmpuint(n, ==, 28);
		g_assert_cmpmem(packet, sizeof(header), header, sizeof(header));
	}
	srtp_dealloc(relay);
	for (guint i = 0; i < G_N_ELEMENTS(targets); i++)
		call_video_close(targets[i]);
	done(&loop);
}

static void conference_receive_fir(void)
{
	struct loop loop = { 0 };
	guint8 packet[28] = {
		0x91, 206, 0, 6, 0, 0, 0, 102, 0, 0, 0, 101,
	};
	const gint64 first = 1100000;

	loop.tx = call_video_open_group_tx(key_a, 97, 101, 400, 10, NULL);
	loop.rx = call_video_open_group_rx(key_a, 97, 101, NULL);
	g_assert_nonnull(loop.tx);
	g_assert_nonnull(loop.rx);
	prepare_frame(&loop);
	/* Lose the initial keyframe, then reject control for unknown streams. */
	encode(&loop, 1000000);
	g_assert_false(call_video_receive_group_feedback(
		loop.tx, packet, sizeof(packet), 103, first));
	packet[11]++;
	g_assert_false(call_video_receive_group_feedback(
		loop.tx, packet, sizeof(packet), 102, first));
	packet[11]--;
	packet[3]--;
	g_assert_false(call_video_receive_group_feedback(
		loop.tx, packet, sizeof(packet), 102, first));
	packet[3]++;
	encode(&loop, first);
	for (guint i = 0; i < loop.packets->len; i++)
		deliver(&loop, i);
	g_assert_true(call_video_needs_keyframe(loop.rx));

	g_assert_true(call_video_receive_group_feedback(
		loop.tx, packet, sizeof(packet), 102, first));
	g_assert_false(call_video_receive_group_feedback(
		loop.tx, packet, sizeof(packet), 102,
		first + G_USEC_PER_SEC - 1));
	encode(&loop, first + 100000);
	for (guint i = 0; i < loop.packets->len; i++)
		g_assert_true(deliver(&loop, i));
	g_assert_cmpuint(loop.count, ==, 1);
	g_assert_false(call_video_needs_keyframe(loop.rx));
	g_assert_true(call_video_receive_group_feedback(
		loop.tx, packet, sizeof(packet), 102, first + G_USEC_PER_SEC));
	done(&loop);
}

static void peer_video(Pb *out, gboolean packed, guint8 packet_version,
		       guint8 pt, guint32 local, guint32 remote)
{
	Pb stream = { 0 }, head = { 0 }, rtp = { 0 }, feature = { 0 };
	static const guint8 packed_maps[] = { 4, 2 };

	pb_text(&head, 1, "V");
	pb_uint(&head, 2, 3);
	pb_uint(&head, 3, 1);
	pb_uint(&head, 50, 3); /* AVC comes before the common VP8 map. */
	if (packed)
		pb_bytes(&head, 50,
			 (Slice){ packed_maps, sizeof(packed_maps) });
	else
		pb_uint(&head, 50, 2);
	pb_uint(&rtp, 1, pt);
	pb_uint(&rtp, 11, local);
	if (remote)
		pb_uint(&rtp, 61, remote);
	pb_msg(&stream, 1, &head);
	pb_msg(&stream, 2, &rtp);
	pb_uint(&feature, 1, 0);
	pb_uint(&feature, 2, 2); /* Baseline VP8 plus VP8A. */
	pb_msg(&stream, 51, &feature);
	if (packet_version) {
		feature.n = 0;
		pb_uint(&feature, 1, 2);
		pb_uint(&feature, 2, packet_version);
		pb_msg(&stream, 51, &feature);
	}
	pb_msg(out, 1, &stream);
	g_assert_false(out->overflow);
}

static void negotiation(void)
{
	Pb offer = { 0 }, answer = { 0 };
	guint8 pt;
	guint32 local, remote;

	/* Another RTP payload and newer framing are capabilities in the offer. */
	peer_video(&offer, TRUE, 2, 109, 0x10203040, 0);
	g_assert_true(call_video_offer_profile(PB_SLICE(offer), &pt, &local,
					       &remote));
	g_assert_cmpuint(pt, ==, 109);
	g_assert_cmpuint(local, ==, 0x10203040);
	g_assert_cmpuint(remote, ==, 0);

	CallInfo info = { .audio_payload = 106,
			  .video_payload = pt,
			  .video = TRUE };

	call_answer_offer(&answer, &info);
	g_assert_true(
		call_video_profile(PB_SLICE(answer), &pt, &local, &remote));
	g_assert_cmpuint(pt, ==, 109);
	g_assert_cmpuint(local, ==, 112);
	g_assert_cmpuint(remote, ==, 212);
	Slice audio_path = pb_data(pb_data(PB_SLICE(answer), 1), 2);

	g_assert_cmpuint(pb_number(audio_path, 1, 0), ==, 106);
	Slice records = PB_SLICE(answer);
	PbField record;
	gboolean found = FALSE;

	while (pb_next(&records, &record)) {
		Slice name = pb_data(pb_data(record.data, 1), 1);

		if (record.field == 1 && name.n == 1 && *name.p == 'V') {
			Slice selected = pb_data(record.data, 51);

			g_assert_cmpuint(pb_number(selected, 1, 99), ==, 0);
			g_assert_cmpuint(pb_number(selected, 2, 0), ==, 1);
			found = TRUE;
			break;
		}
	}
	g_assert_true(found);

	/* The answer can assign both SSRCs and a different dynamic payload. */
	answer.n = 0;
	peer_video(&answer, FALSE, 0, 110, 0x10203040, 0x50607080);
	g_assert_true(
		call_video_profile(PB_SLICE(answer), &pt, &local, &remote));
	g_assert_cmpuint(pt, ==, 110);
	g_assert_cmpuint(local, ==, 0x10203040);
	g_assert_cmpuint(remote, ==, 0x50607080);

	/* A negotiated framing version cannot be treated as baseline. */
	answer.n = 0;
	peer_video(&answer, FALSE, 2, 109, 0x10203040, 0x50607080);
	g_assert_false(
		call_video_profile(PB_SLICE(answer), &pt, &local, &remote));

	/*
	 * The official Android app answered a voice call with this inactive
	 * VP8 record and no feature list. It later started video when its
	 * camera turned on.
	 */
	static const char live[] =
		"0a110a0156100318002090032818300290030212170861587062050843"
		"109235e803d401f20305084310a26a1a21087f12170861587062050843"
		"109235e803d401f20305084310a26a5870e803d401";
	guint8 record_bytes[sizeof(live) / 2];

	for (gsize i = 0; i < sizeof(record_bytes); i++)
		record_bytes[i] = g_ascii_xdigit_value(live[2 * i]) << 4 |
				  g_ascii_xdigit_value(live[2 * i + 1]);
	answer.n = 0;
	pb_bytes(&answer, 1, (Slice){ record_bytes, sizeof(record_bytes) });
	g_assert_true(
		call_video_profile(PB_SLICE(answer), &pt, &local, &remote));
	g_assert_cmpuint(pt, ==, 97);
	g_assert_cmpuint(local, ==, 112);
	g_assert_cmpuint(remote, ==, 212);
}

static void negotiated_streams(void)
{
	Pb spec = { 0 };
	PbField field;
	const guint32 rx[] = { 10010, 20010, 122 };
	const guint32 tx[] = { 10020, 20020 };
	guint receive = 0, send = 0;

	call_stream_spec(&spec, rx[0], tx[0], rx[1], tx[1], 0, 0, FALSE);
	Slice records = PB_SLICE(spec);

	while (pb_next(&records, &field)) {
		if (field.field == 1) {
			g_assert_cmpuint(receive, <, G_N_ELEMENTS(rx));
			g_assert_cmpuint(pb_number(field.data, 1, 0), ==,
					 rx[receive++]);
		}
		if (field.field == 6) {
			g_assert_cmpuint(send, <, G_N_ELEMENTS(tx));
			g_assert_cmpuint(pb_number(field.data, 1, 0), ==,
					 tx[send]);
			Slice state = pb_data(field.data, 2);

			g_assert_cmpuint(pb_number(state, 1, 99), ==, send);
			send++;
		}
	}
	g_assert_cmpuint(receive, ==, G_N_ELEMENTS(rx));
	g_assert_cmpuint(send, ==, G_N_ELEMENTS(tx));
}

/* A screen-like picture: white with dark text-like bars. */
static void screen_picture(guint8 *pixels, guint w, guint h, guint shift)
{
	memset(pixels, 235, (gsize)w * h);
	memset(pixels + (gsize)w * h, 128, (gsize)w * h / 2);
	for (guint y = 40; y < h - 40; y += 24)
		for (guint x = 40 + shift; x < w - 80; x += 9)
			for (guint dy = 0; dy < 12; dy++)
				for (guint dx = 0; dx < 5; dx++)
					pixels[(y + dy) * w + x + dx] = 20;
}

struct share_loop {
	GPtrArray *fragments;
	guint shown;
	guint8 sample;
};

static gboolean share_collect(const guint8 *frag, gsize size, gboolean key,
			      gpointer data)
{
	struct share_loop *loop = data;

	(void)key;
	g_ptr_array_add(loop->fragments, g_bytes_new(frag, size));
	return TRUE;
}

static void share_display(const struct line_camera_frame *frame, gpointer data)
{
	struct share_loop *loop = data;

	g_assert_cmpuint(frame->width, ==, 1280);
	g_assert_cmpuint(frame->height, ==, 720);
	/* Background stays white and text dark after encoding. */
	g_assert_cmpint(frame->plane[0][5], >=, 220);
	loop->sample = frame->plane[0][(40 + 5) * frame->stride[0] + 42];
	loop->shown++;
}

static void screen_share_roundtrip(void)
{
	enum {
		SW = 1280,
		SH = 720
	};
	g_autofree guint8 *pixels = g_malloc(SW * SH * 3 / 2);
	struct call_share_encoder *enc = call_share_encoder_new(3500, 30);
	struct call_share_decoder *dec = call_share_decoder_new();
	struct share_loop loop = { .fragments = g_ptr_array_new_with_free_func(
					   (GDestroyNotify)g_bytes_unref) };
	struct line_camera_frame frame;
	g_autoptr(GError) e = NULL;

	for (guint i = 0; i < 3; i++) {
		screen_picture(pixels, SW, SH, i);
		line_camera_i420_frame(&frame, pixels, SW, SH);
		g_assert_true(call_share_encode(enc, &frame,
						(gint64)i * G_USEC_PER_SEC / 10,
						share_collect, &loop, &e));
		g_assert_no_error(e);
	}
	g_assert_cmpuint(loop.fragments->len, >, 3);
	/* The first fragment uses the official presentation layout. */
	gsize n;
	const guint8 *p = g_bytes_get_data(loop.fragments->pdata[0], &n);

	g_assert_cmphex(p[0], ==, 0xba);
	g_assert_cmphex(p[3], ==, 0x10);
	g_assert_cmphex(p[4] & 0x21, ==, 0x21);
	g_assert_cmphex(p[5], ==, 5);
	g_assert_cmphex(p[6], ==, 2);
	g_assert_cmphex(p[9], ==, 9);
	g_assert_cmphex(p[10], ==, 0x84);
	gsize vp8 = 0;
	guint first_delta = 0;

	for (guint i = 0; i < loop.fragments->len; i++) {
		gsize size;
		const guint8 *f =
			g_bytes_get_data(loop.fragments->pdata[i], &size);

		g_assert_cmpuint(size, <=, 1100);
		if (i && (f[0] & 8) && !first_delta)
			first_delta = i;
		if (!first_delta)
			vp8 += size - (i ? 3 : 15);
	}
	/* Extension 9 holds the keyframe size plus three. */
	g_assert_cmpuint((guint32)p[11] << 24 | p[12] << 16 | p[13] << 8 |
				 p[14],
			 ==, vp8 + 3);
	g_assert_cmpuint(first_delta, >, 0);
	p = g_bytes_get_data(loop.fragments->pdata[first_delta], &n);
	g_assert_cmphex(p[0] & 0xfb, ==, 0xfa);
	g_assert_cmpmem(p + 3, 4, ((const guint8[]){ 0x10, 3, 1, 0 }), 4);
	for (guint i = 0; i < loop.fragments->len; i++) {
		gsize size;
		const guint8 *f =
			g_bytes_get_data(loop.fragments->pdata[i], &size);

		g_assert_true(
			call_share_decode(dec, f, size, share_display, &loop));
	}
	g_assert_cmpuint(loop.shown, ==, 3);
	g_assert_cmpint(loop.sample, <=, 80);
	g_ptr_array_unref(loop.fragments);
	call_share_encoder_free(enc);
	call_share_decoder_free(dec);
}

struct encoder_output {
	guint pictures, luma;
};

static void check_encoded_frame(const struct line_camera_frame *f,
				gpointer data)
{
	struct encoder_output *out = data;

	g_assert_cmpuint(f->width, ==, 16);
	g_assert_cmpuint(f->height, ==, 16);
	g_assert_cmpint(ABS((int)f->plane[0][0] - (int)out->luma), <=, 3);
	out->pictures++;
}

enum encoder_transport {
	ENC_CAMERA,
	ENC_GROUP_CAMERA,
	ENC_SCREEN_RTP,
	ENC_SCREEN_PDTP,
};

static void outgoing_hardware_vp8(gconstpointer data)
{
	enum encoder_transport transport = GPOINTER_TO_UINT(data);
	struct loop loop = { 0 };
	struct share_loop share = { 0 };
	struct call_share_encoder *presentation = NULL;
	struct call_share_decoder *presentation_rx = NULL;
	guint8 pixels[16 * 16 * 3 / 2];
	struct encoder_output out = { .luma = 126 };
	srtp_t observer = NULL;
	guint expected_opens = 1;

	encoder = (struct encoder_fixture){ .enabled = TRUE };
	prepare(&loop);
	memset(pixels, 212, 16 * 16);
	memset(pixels + 16 * 16, 128, sizeof(pixels) - 16 * 16);
	line_camera_i420_frame(&loop.frame, pixels, 16, 16);
	if (transport == ENC_GROUP_CAMERA) {
		call_video_close(loop.tx);
		call_video_close(loop.rx);
		loop.tx =
			call_video_open_group_tx(key_a, 97, 101, 400, 10, NULL);
		loop.rx = call_video_open_group_rx(key_a, 97, 101, NULL);
		g_assert_nonnull(loop.tx);
		g_assert_nonnull(loop.rx);
		call_video_set_group_format(loop.tx, CALL_VIDEO_RTP_EXT_VED,
					    TRUE);
	} else if (transport == ENC_SCREEN_PDTP) {
		presentation = call_share_encoder_new(900, 20);
		presentation_rx = call_share_decoder_new();
		g_assert_nonnull(presentation);
		g_assert_nonnull(presentation_rx);
		share.fragments = g_ptr_array_new_with_free_func(
			(GDestroyNotify)g_bytes_unref);
	}
	if (!presentation)
		g_assert_true(
			call_srtp_open(&observer, key_a, ssrc_specific, 101));
	for (guint i = 0; i < 6; i++) {
		gint64 now = 1000000 + i * 100000;
		gboolean key = i == 0 || i == 2 || i == 3 || i == 4;
		gsize size;

		/* A driver's spontaneous keyframe, a remote refresh, then a
		 * driver failure requiring the same input as a software keyframe. */
		encoder.force_key = i == 2 && transport != ENC_SCREEN_RTP;
		if (transport == ENC_SCREEN_RTP && i == 2) {
			call_video_screen(loop.tx, 900, 20);
			expected_opens++;
		}
		if (i == 3) {
			if (presentation)
				call_share_encoder_keyframe(presentation);
			else
				call_video_keyframe(loop.tx);
		}
		if (i == 4) {
			encoder.fail = TRUE;
			out.luma = pixels[0];
		}
		/* Returning to the camera after a runtime hardware failure
		 * must restart in software without probing the failed encoder. */
		if (transport == ENC_SCREEN_RTP && i == 5) {
			call_video_screen(loop.tx, 0, 0);
			key = TRUE;
		}
		if (presentation) {
			g_ptr_array_set_size(share.fragments, 0);
			g_assert_true(call_share_encode(
				presentation, &loop.frame, now, share_collect,
				&share, NULL));
			g_assert_cmpuint(share.fragments->len, ==, 1);
			const guint8 *f = g_bytes_get_data(
				share.fragments->pdata[0], &size);
			g_assert_cmpint(!(f[0] & 0x40), ==, key);
			g_assert_true(
				call_share_decode(presentation_rx, f, size,
						  check_encoded_frame, &out));
		} else {
			encode(&loop, now);
			g_assert_cmpuint(loop.packets->len, ==, 1);
			const guint8 *f =
				g_bytes_get_data(loop.packets->pdata[0], &size);
			guint8 packet[1200];
			int length = size;
			guint header = 12;

			g_assert_cmpuint(size, <=, sizeof(packet));
			memcpy(packet, f, size);
			g_assert_cmpint(srtp_unprotect(observer, packet,
						       &length),
					==, srtp_err_status_ok);
			if (packet[0] & 0x10)
				header += 4 +
					  4 * (packet[14] * 256u + packet[15]);
			g_assert_cmpuint(header, <, (guint)length);
			g_assert_cmpint(!(packet[header] & 0x40), ==, key);
			g_assert_true(call_video_receive(
				loop.rx, f, size, check_encoded_frame, &out));
		}
		g_assert_cmpuint(out.pictures, ==, i + 1);
		g_assert_cmpuint(encoder.opens, ==, expected_opens);
		g_assert_cmpuint(encoder.frames, ==, MIN(i + 1, 5u));
	}
	g_assert_cmpuint(encoder.kbps, ==,
			 transport == ENC_GROUP_CAMERA ? 150 :
			 transport == ENC_CAMERA       ? 400 :
							 900);
	g_assert_cmpuint(encoder.fps, ==,
			 transport <= ENC_GROUP_CAMERA ? 10 : 20);
	if (observer)
		srtp_dealloc(observer);
	g_clear_pointer(&share.fragments, g_ptr_array_unref);
	call_share_encoder_free(presentation);
	call_share_decoder_free(presentation_rx);
	done(&loop);
	g_assert_cmpuint(encoder.closes, ==, expected_opens);
	encoder = (struct encoder_fixture){ 0 };
}

struct call_vp8_encoder *__real_call_vp8_encoder_open(guint w, guint h,
						      guint kbps, guint fps,
						      guint key_distance);
gboolean __real_call_vp8_encoder_frame(struct call_vp8_encoder *e,
				       const struct line_camera_frame *f,
				       gint64 pts, gboolean force_key,
				       const guint8 **data, gsize *size,
				       gboolean *key);
void __real_call_vp8_encoder_close(struct call_vp8_encoder *e);
const char *__real_call_vp8_encoder_name(const struct call_vp8_encoder *e);

static void vaapi_encoder_selection(gconstpointer vendor)
{
#if CALL_HWDECODE
	if (!avcodec_find_encoder_by_name("vp8_vaapi")) {
		g_test_skip("FFmpeg was built without the VP8 VA-API encoder");
		return;
	}
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	/* Pin a synthetic DRM render node. Its deliberately unavailable VA
	 * device must be probed once and leave software encoding available. */
	g_unsetenv("LINE_DISABLE_GPU");
	g_setenv("AVERNELLA_RENDER_NODE", "/dev/null", TRUE);
	mock_drm_vendor = vendor;
	g_assert_null(__real_call_vp8_encoder_open(W, H, 400, 10, 30));
	g_assert_true(tried_vaapi);
#else
	(void)vendor;
	g_test_skip("Built without FFmpeg acceleration");
#endif
}

static void native_hardware_vp8(void)
{
	struct call_vp8_encoder *enc =
		__real_call_vp8_encoder_open(W, H, 400, 10, 30);
	if (!enc) {
		g_test_skip(
			"Selected adapter has no usable hardware VP8 encoder");
		return;
	}

	struct call_vp8_decoder *dec = call_vp8_open(1, FALSE);
	struct loop loop = { .color_tolerance = 12 };
	const int strides[] = { W + 9, W / 2 + 3, W / 2 + 11 };
	g_autofree guint8 *storage =
		g_malloc(H * strides[0] + H / 2 * (strides[1] + strides[2]));
	guint8 *p = storage;
	static const guint8 samples[] = { 96, 80, 160 };

	g_test_message("Hardware encoder: %s",
		       __real_call_vp8_encoder_name(enc));
	g_assert_nonnull(dec);
	loop.frame.width = W;
	loop.frame.height = H;
	for (guint i = 0; i < 3; i++) {
		guint w = i ? W / 2 : W, h = i ? H / 2 : H;
		loop.frame.plane[i] = p;
		loop.frame.stride[i] = strides[i];
		for (guint row = 0; row < h; row++, p += strides[i]) {
			memset(p, samples[i], w);
			memset(p + w, 0xff, strides[i] - w);
		}
	}
	for (guint i = 0; i < 3; i++) {
		const guint8 *picture;
		gsize size;
		gboolean key;
		gboolean force_key = i != 1;

		g_assert_true(__real_call_vp8_encoder_frame(
			enc, &loop.frame, i * 9000, force_key, &picture, &size,
			&key));
		g_assert_cmpuint(size, >, 3);
		g_assert_cmpint(key, ==, !(picture[0] & 1));
		if (force_key)
			g_assert_true(key);
		g_assert_cmpint(call_vp8_decode(dec, picture, size, key, 0,
						check_visible_edges, &loop),
				==, 1);
	}
	g_assert_cmpuint(loop.count, ==, 3);
	call_vp8_close(dec);
	__real_call_vp8_encoder_close(enc);
}

/* Bytes captured from LINE Android 26.18.2's presentation stream. */
static void screen_share_official_layout(void)
{
	static const guint8 header[] = { 0xba, 0x00, 0x01, 0x10, 0x27, 0x05,
					 0x02, 0x00, 0x3c, 0x09, 0x84 };
	struct call_share_decoder *dec = call_share_decoder_new();
	struct call_share_encoder *enc = call_share_encoder_new(1000, 10);
	struct share_loop loop = { .fragments = g_ptr_array_new_with_free_func(
					   (GDestroyNotify)g_bytes_unref) };
	g_autofree guint8 *pixels = g_malloc(1280 * 720 * 3 / 2);
	struct line_camera_frame frame;
	g_autoptr(GByteArray) vp8 = g_byte_array_new();

	screen_picture(pixels, 1280, 720, 0);
	line_camera_i420_frame(&frame, pixels, 1280, 720);
	g_assert_true(
		call_share_encode(enc, &frame, 0, share_collect, &loop, NULL));
	for (guint i = 0; i < loop.fragments->len; i++) {
		gsize size;
		const guint8 *f =
			g_bytes_get_data(loop.fragments->pdata[i], &size);

		g_byte_array_append(vp8, f + (i ? 3 : 15), size - (i ? 3 : 15));
	}
	/* Re-fragment like the tablet: 888 bytes, middle 80, end 84. */
	gsize pos = 0;
	guint count = 0;

	while (pos < vp8->len) {
		guint8 frag[1024];
		gsize len = MIN(888, vp8->len - pos), h;

		if (!pos) {
			guint32 total = vp8->len + 3;

			memcpy(frag, header, sizeof(header));
			frag[11] = total >> 24;
			frag[12] = total >> 16;
			frag[13] = total >> 8;
			frag[14] = total;
			h = 15;
		} else {
			frag[0] = pos + len == vp8->len ? 0x84 : 0x80;
			frag[1] = 0;
			frag[2] = 1;
			h = 3;
		}
		memcpy(frag + h, vp8->data + pos, len);
		g_assert_true(call_share_decode(dec, frag, h + len,
						share_display, &loop));
		pos += len;
		count++;
	}
	g_assert_cmpuint(count, >, 1);
	g_assert_cmpuint(loop.shown, ==, 1);
	/* A middle fragment without its start waits for the next picture. */
	static const guint8 orphan[] = { 0x80, 0x00, 0x09, 1, 2, 3 };

	g_assert_false(call_share_decode(dec, orphan, sizeof(orphan),
					 share_display, &loop));
	g_ptr_array_unref(loop.fragments);
	call_share_encoder_free(enc);
	call_share_decoder_free(dec);
}

/* Generate a phone-size presentation independently of our camera encoder. */
static void screen_share_portrait(void)
{
	enum {
		WIDTH = 1080,
		HEIGHT = 2408
	};
	const guint8 header[] = { 0x8c, 0, 1 };
	const guint8 samples[] = { 96, 80, 160 };
	vpx_codec_ctx_t encoder;
	vpx_codec_enc_cfg_t cfg;
	vpx_image_t image;
	vpx_codec_iter_t iter = NULL;
	const vpx_codec_cx_pkt_t *packet;
	struct loop loop = { .frame = { .width = WIDTH, .height = HEIGHT },
			     .color_tolerance = 3 };
	struct call_share_decoder *dec = call_share_decoder_new();
	g_autoptr(GByteArray) frag = g_byte_array_new();

	g_assert_false(line_preview_size_valid(WIDTH, HEIGHT, FALSE));
	g_assert_true(line_preview_size_valid(3840, 2160, TRUE));
	g_assert_true(line_preview_size_valid(4096, 2025, TRUE));
	g_assert_nonnull(dec);
	g_assert_cmpint(vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg,
						     0),
			==, VPX_CODEC_OK);
	cfg.g_w = WIDTH;
	cfg.g_h = HEIGHT;
	cfg.g_lag_in_frames = 0;
	g_assert_cmpint(vpx_codec_enc_init(&encoder, vpx_codec_vp8_cx(), &cfg,
					   0),
			==, VPX_CODEC_OK);
	g_assert_nonnull(
		vpx_img_alloc(&image, VPX_IMG_FMT_I420, WIDTH, HEIGHT, 1));
	for (unsigned int i = 0; i < G_N_ELEMENTS(samples); i++)
		memset(image.planes[i], samples[i],
		       image.stride[i] * (i ? HEIGHT / 2 : HEIGHT));
	g_assert_cmpint(vpx_codec_encode(&encoder, &image, 0, 1,
					 VPX_EFLAG_FORCE_KF, VPX_DL_REALTIME),
			==, VPX_CODEC_OK);
	g_byte_array_append(frag, header, sizeof(header));
	while ((packet = vpx_codec_get_cx_data(&encoder, &iter))) {
		if (packet->kind == VPX_CODEC_CX_FRAME_PKT)
			g_byte_array_append(frag, packet->data.frame.buf,
					    packet->data.frame.sz);
	}
	vpx_img_free(&image);
	vpx_codec_destroy(&encoder);
	g_assert_cmpuint(frag->len, >, sizeof(header) + 10);
	g_assert_true(call_share_decode(dec, frag->data, frag->len,
					check_visible_edges, &loop));
	g_assert_cmpuint(loop.count, ==, 1);
	/* The source dimensions are little-endian in a VP8 keyframe. */
	guint8 *size = frag->data + sizeof(header) + 6;

	const guint invalid[][2] = { { 4097, 1000 },
				     { WIDTH, 4097 },
				     { 3840, 2161 } };
	for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
		size[0] = invalid[i][0] & 255;
		size[1] = invalid[i][0] >> 8;
		size[2] = invalid[i][1] & 255;
		size[3] = invalid[i][1] >> 8;
		g_assert_false(call_share_decode(dec, frag->data, frag->len,
						 check_visible_edges, &loop));
		g_assert_true(call_share_decoder_keyframe_needed(dec));
	}
	g_assert_cmpuint(loop.count, ==, 1);
	call_share_decoder_free(dec);
}

static void screen_capture_conversion(void)
{
	/* BGRx: white, red, green, blue across a 4x2 picture. */
	static const guint8 bgrx[] = {
		255, 255, 255, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0,
		255, 255, 255, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0,
	};
	guint8 out[4 * 2 + 2 * 2];
	unsigned int w, h;

	line_screencast_to_i420(bgrx, 4, 2, 16, TRUE, out, 4, 2);
	g_assert_cmpint(out[0], ==, 235);
	g_assert_cmpint(out[1], ==, 82);
	g_assert_cmpint(out[2], ==, 144);
	g_assert_cmpint(out[3], ==, 41);
	/* V is high for the white-red pair, U high for green-blue. */
	g_assert_cmpint(out[9], >, 128);
	g_assert_cmpint(out[10], >, 128);
	line_screencast_fit(3840, 2160, 1920, 1080, &w, &h);
	g_assert_cmpuint(w, ==, 1920);
	g_assert_cmpuint(h, ==, 1080);
	line_screencast_fit(1080, 2400, 1920, 1080, &w, &h);
	g_assert_cmpuint(h, ==, 1080);
	g_assert_cmpuint(w, ==, 486);
	line_screencast_fit(1280, 721, 1920, 1080, &w, &h);
	g_assert_cmpuint(h, ==, 720);
}

static void software_video_pixels(void)
{
	enum {
		WIDTH = 33,
		HEIGHT = 5,
		Y_STRIDE = 41,
		C_STRIDE = 23
	};
	guint8 y[HEIGHT * Y_STRIDE], u[3 * C_STRIDE], v[3 * C_STRIDE];
	guint32 output[WIDTH * HEIGHT + 2];

	for (size_t i = 0; i < sizeof(y); i++)
		y[i] = i * 71;
	for (size_t i = 0; i < sizeof(u); i++) {
		u[i] = i * 37;
		v[i] = 255 - i * 57;
	}
	for (unsigned int w = 2; w <= WIDTH; w++) {
		for (int direction = -1; direction <= 1; direction += 2) {
			struct line_camera_frame frame = {
				.width = w,
				.height = HEIGHT,
				.stride = { direction * Y_STRIDE,
					    direction * C_STRIDE,
					    direction * C_STRIDE },
				.plane = { y + 1, u + 1, v + 1 },
			};

			if (direction < 0) {
				frame.plane[0] += (HEIGHT - 1) * Y_STRIDE;
				frame.plane[1] += (HEIGHT / 2) * C_STRIDE;
				frame.plane[2] += (HEIGHT / 2) * C_STRIDE;
			}
			memset(output, 0xff, sizeof(output));
			line_camera_xrgb(&frame, output + 1);
			g_assert_cmphex(output[0], ==, G_MAXUINT32);
			g_assert_cmphex(output[1 + w * HEIGHT], ==,
					G_MAXUINT32);
			for (int row = 0; row < HEIGHT; row++) {
				const guint8 *luma =
					frame.plane[0] + row * frame.stride[0];
				const guint8 *cb = frame.plane[1] +
						   (row / 2) * frame.stride[1];
				const guint8 *cr = frame.plane[2] +
						   (row / 2) * frame.stride[2];

				for (unsigned int x = 0; x < w; x++) {
					int l = 298 * MAX(luma[x] - 16, 0) +
						128;
					int b = cb[x / 2] - 128,
					    r = cr[x / 2] - 128;
					guint32 expected =
						(CLAMP((l + 409 * r) >> 8, 0,
						       255)
						 << 16) |
						(CLAMP((l - 100 * b - 208 * r) >>
							       8,
						       0, 255)
						 << 8) |
						CLAMP((l + 516 * b) >> 8, 0,
						      255);

					g_assert_cmphex(output[1 + row * w + x],
							==, expected);
				}
			}
		}
	}
}

/* The conversion the camera used before it moved to camera_pixels.c. */
static void reference_jpeg_to_i420(const guint8 *const planes[3],
				   unsigned int w, unsigned int h,
				   unsigned int cw, unsigned int sx,
				   unsigned int sy, guint8 *out)
{
	guint8 *u = out + w * h, *v = u + w * h / 4;

	for (gsize i = 0; i < (gsize)w * h; i++)
		out[i] = 16 + (planes[0][i] * 219 + 127) / 255;
	for (unsigned int row = 0; row < h / 2; row++)
		for (unsigned int x = 0; x < w / 2; x++)
			for (int c = 1; c <= 2; c++) {
				const guint8 *p = planes[c] +
						  (gsize)row * sy * cw + x * sx;
				int sum = 0, avg;

				for (unsigned int dy = 0; dy < sy; dy++)
					for (unsigned int dx = 0; dx < sx; dx++)
						sum += p[dy * cw + dx];
				avg = (sum + sx * sy / 2) / (sx * sy);
				(c == 1 ? u : v)[row * (w / 2) + x] =
					128 + ((avg - 128) * 224 +
					       (avg >= 128 ? 127 : -127)) /
						      255;
			}
}

/* Every camera chroma layout converts exactly as before, only faster. */
static void camera_jpeg_range(void)
{
	enum {
		CW = 1280,
		CH = 720
	};
	g_autofree guint8 *source = g_malloc(CW * CH * 3);
	g_autofree guint8 *fast = g_malloc(CW * CH * 3 / 2);
	g_autofree guint8 *slow = g_malloc(CW * CH * 3 / 2);
	GRand *rand = g_rand_new_with_seed(7);

	for (gsize i = 0; i < CW * CH * 3; i++)
		source[i] = i < 256 ? (guint8)i :
				      (guint8)g_rand_int_range(rand, 0, 256);
	g_rand_free(rand);
	for (unsigned int sx = 1; sx <= 2; sx++)
		for (unsigned int sy = 1; sy <= 2; sy++) {
			unsigned int cw = CW / 2 * sx, ch = CH / 2 * sy;
			const guint8 *planes[3] = { source, source + CW * CH,
						    source + CW * CH +
							    (gsize)cw * ch };

			line_camera_jpeg_to_i420(planes, CW, CH, cw, sx, sy,
						 fast);
			reference_jpeg_to_i420(planes, CW, CH, cw, sx, sy,
					       slow);
			g_assert_cmpmem(fast, CW * CH * 3 / 2, slow,
					CW * CH * 3 / 2);
			if (!g_test_perf())
				continue;
			gdouble before, after;

			g_test_timer_start();
			for (int i = 0; i < 50; i++)
				reference_jpeg_to_i420(planes, CW, CH, cw, sx,
						       sy, slow);
			before = g_test_timer_elapsed() * 1000 / 50;
			g_test_timer_start();
			for (int i = 0; i < 50; i++)
				line_camera_jpeg_to_i420(planes, CW, CH, cw, sx,
							 sy, fast);
			after = g_test_timer_elapsed() * 1000 / 50;
			g_test_minimized_result(
				after,
				"%ux%u chroma, 720p: %.3f ms (was %.3f ms)", sx,
				sy, after, before);
		}
}

/* One step faster when encoding falls behind, back once it catches up. */
static void encoder_pace(void)
{
	/* At 30 fps a frame has 33.3 ms: 60% is 20 ms, 25% is 8.3 ms. */
	g_assert_cmpint(call_video_pace(-6, -6, 25000, 30), ==, -7);
	g_assert_cmpint(call_video_pace(-12, -6, 25000, 30), ==, -12);
	g_assert_cmpint(call_video_pace(-8, -6, 12000, 30), ==, -8);
	g_assert_cmpint(call_video_pace(-8, -6, 5000, 30), ==, -7);
	g_assert_cmpint(call_video_pace(-6, -6, 5000, 30), ==, -6);
	g_assert_cmpint(call_video_pace(-4, -4, 0, 0), ==, -4);
}

static gboolean discard(const guint8 *wire, gsize size, gpointer data)
{
	(void)wire;
	(void)size;
	++*(guint *)data;
	return TRUE;
}

/* With -m perf: the cost of a 720p camera frame through the call path. */
static void encode_720p(void)
{
	enum {
		EW = 1280,
		EH = 720,
		FRAMES = 90
	};
	g_autofree guint8 *pixels = g_malloc(EW * EH * 3 / 2);
	struct line_camera_frame frame;
	struct call_video *video;
	guint packets = 0;

	if (!g_test_perf()) {
		g_test_skip("Run with -m perf to measure the encoder.");
		return;
	}
	video = call_video_open(key_a, key_b, 96, 1, 2, 1500, 30, NULL);
	g_assert_nonnull(video);
	line_camera_i420_frame(&frame, pixels, EW, EH);
	gdouble seconds = 0;

	for (int i = 0; i < FRAMES; i++) {
		/* A moving gradient with texture, so every frame changes. */
		for (unsigned int y = 0; y < EH; y++)
			for (unsigned int x = 0; x < EW; x++)
				pixels[y * EW + x] = (x + 3 * y + 7 * i) ^
						     (x * y >> 6);
		memset(pixels + EW * EH, 128 + i % 32, EW * EH / 2);
		g_test_timer_start();
		g_assert_true(call_video_encode(video, &frame,
						(gint64)i * G_USEC_PER_SEC / 30,
						discard, &packets, NULL));
		seconds += g_test_timer_elapsed();
	}
	gdouble ms = seconds * 1000 / FRAMES;

	g_assert_cmpuint(packets, >, FRAMES);
	g_test_minimized_result(ms, "720p VP8 at 1.5 Mbit/s: %.2f ms a frame",
				ms);
	call_video_close(video);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/video/evs3-vp8-srtp-reordering", roundtrip);
	g_test_add_func("/video/conference-independent-stream-directions",
			conference_directions);
	g_test_add_func("/video/evs3-picture-metadata", evs3_picture_metadata);
	g_test_add_func("/video/group-source-visibility",
			group_source_visibility);
	g_test_add_func("/video/authentication-loss-and-keyframe-recovery",
			recovery);
	g_test_add_func("/video/standard-vp8-reordering", standard_vp8);
	g_test_add_data_func("/video/single-packet/evs3", GINT_TO_POINTER(0),
			     single_packet);
	g_test_add_data_func("/video/single-packet/rfc7741", GINT_TO_POINTER(1),
			     single_packet);
	g_test_add_data_func("/video/single-packet/declared-length",
			     GINT_TO_POINTER(FALSE), single_packet_length);
	g_test_add_data_func("/video/single-packet/group-declared-length",
			     GINT_TO_POINTER(TRUE), single_packet_length);
	g_test_add_func("/video/conference-evs3-size-framing",
			group_evs3_sizes);
	g_test_add_func("/video/conference-resolution", group_resolution);
	g_test_add_func("/video/conference-resolution-aspect",
			group_resolution_aspect);
	g_test_add_func("/video/conference-screen-resolution",
			group_screen_resolution);
	g_test_add_func("/video/conference-thumbnail-rate",
			group_thumbnail_rate);
	static const struct group_test_format formats[] = {
		{ CALL_VIDEO_RTP_EXT_NONE, FALSE },
		{ CALL_VIDEO_RTP_EXT_VFD, TRUE },
		{ CALL_VIDEO_RTP_EXT_VED, TRUE },
		{ CALL_VIDEO_RTP_EXT_VFD, FALSE },
		{ CALL_VIDEO_RTP_EXT_VED, FALSE }
	};
	g_test_add_data_func("/video/conference-evs3-send", &formats[0],
			     group_evs3_tx);
	g_test_add_data_func("/video/conference-evs3-send-vfd", &formats[1],
			     group_evs3_tx);
	g_test_add_data_func("/video/conference-evs3-send-ved", &formats[2],
			     group_evs3_tx);
	g_test_add_data_func("/video/conference-vp8-compact-vfd", &formats[3],
			     group_evs3_tx);
	g_test_add_data_func("/video/conference-vp8-compact-ved", &formats[4],
			     group_evs3_tx);
	g_test_add_func("/video/encrypted-keyframe-feedback",
			encrypted_feedback);
	g_test_add_func("/video/conference-keyframe-feedback",
			conference_feedback);
	g_test_add_func("/video/conference-receive-fir",
			conference_receive_fir);
	g_test_add_func("/video/profile-negotiation", negotiation);
	g_test_add_func("/video/negotiated-stream-reports", negotiated_streams);
	g_test_add_func("/video/hardware-encode/native", native_hardware_vp8);
	g_test_add_data_func("/video/hardware-encode/amd-selection", "0x1002",
			     vaapi_encoder_selection);
	g_test_add_data_func("/video/hardware-encode/intel-selection", "0x8086",
			     vaapi_encoder_selection);
	g_test_add_data_func("/video/hardware-encode/camera",
			     GUINT_TO_POINTER(ENC_CAMERA),
			     outgoing_hardware_vp8);
	g_test_add_data_func("/video/hardware-encode/group-camera",
			     GUINT_TO_POINTER(ENC_GROUP_CAMERA),
			     outgoing_hardware_vp8);
	g_test_add_data_func("/video/hardware-encode/rtp-screen",
			     GUINT_TO_POINTER(ENC_SCREEN_RTP),
			     outgoing_hardware_vp8);
	g_test_add_data_func("/video/hardware-encode/pdtp-screen",
			     GUINT_TO_POINTER(ENC_SCREEN_PDTP),
			     outgoing_hardware_vp8);
	g_test_add_func("/video/screen-share-roundtrip",
			screen_share_roundtrip);
	g_test_add_func("/video/decoder-visible-dimensions",
			decoder_visible_dimensions);
	g_test_add_func("/video/hardware-decoder-recovery", decoder_recovery);
	g_test_add_func("/video/intel-decoder-selection",
			intel_decoder_selection);
	g_test_add_func("/video/screen-share-official-layout",
			screen_share_official_layout);
	g_test_add_func("/video/screen-share-portrait", screen_share_portrait);
	g_test_add_func("/video/screen-capture-conversion",
			screen_capture_conversion);
	g_test_add_func("/video/software-yuv-conversion",
			software_video_pixels);
	g_test_add_func("/video/camera-jpeg-range", camera_jpeg_range);
	g_test_add_func("/video/encoder-pace", encoder_pace);
	g_test_add_func("/video/encode-720p", encode_720p);
	return g_test_run();
}
