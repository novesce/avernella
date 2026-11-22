// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_schema.h"
#include "call_video.h"
#include "call_srtp.h"
#include "screencast.h"
#include <string.h>

enum {
	W = 320,
	H = 240
};

static const guint8 key_a[30] = { 1, 2, 3 }, key_b[30] = { 4, 5, 6 };

struct loop {
	struct call_video *tx, *rx;
	GPtrArray *packets;
	guint count, rotation;
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

static void prepare(struct loop *loop)
{
	g_autoptr(GError) e = NULL;
	g_autoptr(GRand) rand = g_rand_new_with_seed(123);
	guint ysize = W * H;

	loop->tx = call_video_open(key_a, key_b, 97, 101, 102, 400, 10, &e);
	g_assert_no_error(e);
	loop->rx = call_video_open(key_b, key_a, 97, 102, 101, 400, 10, &e);
	g_assert_no_error(e);
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
		g_assert_cmpuint(loop.count, ==, i == 4 ? 3 : MIN(i + 1, 2));
		g_assert_cmpint(call_video_needs_keyframe(loop.rx), ==, i == 3);
	}
	srtp_dealloc(sender);
	srtp_dealloc(observer);
	done(&loop);
}

static void single_packet_length(void)
{
	struct loop loop = { 0 };
	srtp_t observer, sender;
	guint8 packet[1200 + SRTP_MAX_TRAILER_LEN];
	gsize size;

	prepare(&loop);
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

static void share_display(const struct line_camera_frame *frame,
			  gpointer data)
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
	enum { SW = 1280, SH = 720 };
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
		const guint8 *f = g_bytes_get_data(loop.fragments->pdata[i],
						   &size);

		g_assert_cmpuint(size, <=, 1100);
		if (i && (f[0] & 8) && !first_delta)
			first_delta = i;
		if (!first_delta)
			vp8 += size - (i ? 3 : 15);
	}
	/* Extension 9 holds the keyframe size plus three. */
	g_assert_cmpuint((guint32)p[11] << 24 | p[12] << 16 | p[13] << 8 | p[14],
			 ==, vp8 + 3);
	g_assert_cmpuint(first_delta, >, 0);
	p = g_bytes_get_data(loop.fragments->pdata[first_delta], &n);
	g_assert_cmphex(p[0] & 0xfb, ==, 0xfa);
	g_assert_cmpmem(p + 3, 4, ((const guint8[]){ 0x10, 3, 1, 0 }), 4);
	for (guint i = 0; i < loop.fragments->len; i++) {
		gsize size;
		const guint8 *f = g_bytes_get_data(loop.fragments->pdata[i],
						   &size);

		g_assert_true(call_share_decode(dec, f, size, share_display,
						&loop));
	}
	g_assert_cmpuint(loop.shown, ==, 3);
	g_assert_cmpint(loop.sample, <=, 80);
	g_ptr_array_unref(loop.fragments);
	call_share_encoder_free(enc);
	call_share_decoder_free(dec);
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
	g_assert_true(call_share_encode(enc, &frame, 0, share_collect, &loop,
					NULL));
	for (guint i = 0; i < loop.fragments->len; i++) {
		gsize size;
		const guint8 *f = g_bytes_get_data(loop.fragments->pdata[i],
						   &size);

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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/video/evs3-vp8-srtp-reordering", roundtrip);
	g_test_add_func("/video/evs3-picture-metadata", evs3_picture_metadata);
	g_test_add_func("/video/authentication-loss-and-keyframe-recovery",
			recovery);
	g_test_add_func("/video/standard-vp8-reordering", standard_vp8);
	g_test_add_data_func("/video/single-packet/evs3", GINT_TO_POINTER(0),
			     single_packet);
	g_test_add_data_func("/video/single-packet/rfc7741", GINT_TO_POINTER(1),
			     single_packet);
	g_test_add_func("/video/single-packet/declared-length",
			single_packet_length);
	g_test_add_func("/video/encrypted-keyframe-feedback",
			encrypted_feedback);
	g_test_add_func("/video/profile-negotiation", negotiation);
	g_test_add_func("/video/negotiated-stream-reports", negotiated_streams);
	g_test_add_func("/video/screen-share-roundtrip",
			screen_share_roundtrip);
	g_test_add_func("/video/screen-share-official-layout",
			screen_share_official_layout);
	g_test_add_func("/video/screen-capture-conversion",
			screen_capture_conversion);
	g_test_add_func("/video/software-yuv-conversion",
			software_video_pixels);
	return g_test_run();
}
