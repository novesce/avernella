// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Writes the seed corpora: seeds/NAME/ for each fuzzer, in that fuzzer's
 * input format. Seeds rebuild the synthetic fixtures of tests/pdtp.c,
 * tests/call_video.c, tests/call.c, tests/call_group.c and tests/thrift.c,
 * or capture what Avernella's own encoders send with the same synthetic
 * keys and pictures. No seed holds a real token, key, MID or message.
 */
#include "call_group.h"
#include "call_pdtp.h"
#include "call_srtp.h"
#include "call_video.h"
#include "fuzz.h"
#include <string.h>
#include <zlib.h>

static const char *root;
static guint written;

/* Keys and IDs from tests/pdtp.c and tests/call_video.c. */
static const guint8 local_key[30] = { 1, 2, 3 };
static const guint8 relay_key[30] = { 9, 8, 7 };
static const guint8 video_key[30] = { 1, 2, 3 };
static const char guest[] = "u00000000000000000000000000000003";

enum {
	DATA_PT = 101,
	LOCAL_SSRC = 0x1234,
	RELAY_SSRC = 0xabcd,
	VIDEO_PT = 97,
	VIDEO_SSRC = 101,
};

/* From tests/call_video.c: a 16x16 VP8 keyframe and the next frame. */
static const guint8 gray_key[] = {
	0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x10, 0x00, 0x10, 0x00,
	0x00, 0x47, 0x08, 0x85, 0x85, 0x88, 0x85, 0x84, 0x88, 0x02,
	0x02, 0x00, 0x0c, 0x0e, 0x60, 0x00, 0xfe, 0x68, 0x00,
};
static const guint8 gray_delta[] = {
	0xb1, 0x01, 0x00, 0x05, 0x10, 0xac, 0x00, 0x18, 0x00,
	0x18, 0x58, 0x2f, 0xf4, 0x00, 0x08, 0x00, 0x00,
};

J *line_rpc(Line *session, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)session;
	(void)path;
	(void)method;
	(void)args;
	g_set_error_literal(error, LINE_ERROR, 1, "No network for seeds.");
	return NULL;
}

static void save(const char *fuzzer, const char *name, const guint8 *p, gsize n)
{
	g_autofree char *dir = g_build_filename(root, fuzzer, NULL);
	g_autofree char *path = g_build_filename(dir, name, NULL);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, (const char *)p, n, NULL));
	written++;
}

static void save_array(const char *fuzzer, const char *name, GByteArray *a)
{
	save(fuzzer, name, a->data, a->len);
	g_byte_array_unref(a);
}

static void byte(GByteArray *out, guint8 b)
{
	g_byte_array_append(out, &b, 1);
}

static void u16(GByteArray *out, guint v)
{
	byte(out, v >> 8);
	byte(out, v);
}

/* An operation with a length-prefixed body (fuzz_chunk() in fuzz.h). */
static void record(GByteArray *out, guint8 op, const guint8 *body, gsize n)
{
	byte(out, op);
	u16(out, n);
	g_byte_array_append(out, body, n);
}

/* A selector, an argument, then a message, for the pb and group fuzzers. */
static void message(const char *fuzzer, const char *name, guint select,
		    guint arg, const Pb *body)
{
	GByteArray *out = g_byte_array_new();

	g_assert_false(body->overflow);
	byte(out, select);
	byte(out, arg);
	g_byte_array_append(out, body->p, body->n);
	save_array(fuzzer, name, out);
}

/* ---- PDTP ---- */

static int relay_rtp(guint8 *packet, guint8 seq, const guint8 *payload,
		     gsize size)
{
	const guint8 header[] = {
		0x90, 0x80 | DATA_PT,	 0,    seq,  0, 0, 0, 1, 0, 0,
		0,    RELAY_SSRC & 0xff, 0x02, 0x40, 0, 0,
	};

	memcpy(packet, header, sizeof(header));
	packet[10] = RELAY_SSRC >> 8;
	memcpy(packet + sizeof(header), payload, size);
	return sizeof(header) + size;
}

static void pdtp_receive(GByteArray *out, guint8 seq, const guint8 *payload,
			 gsize size)
{
	guint8 packet[256];

	record(out, 0, packet, relay_rtp(packet, seq, payload, size));
}

static void pdtp_wait(GByteArray *out, guint ms)
{
	byte(out, 4);
	u16(out, ms);
}

static void pdtp_fixtures(void)
{
	/* tests/pdtp.c: SET_MAX_ACK_DELAY, then STRM_OPEN for stream 110. */
	static const guint8 open[] = {
		0x80, 0,    'L',  'I', 'N', 'E',  0,	0x02, 0x08,
		2,    0x40, 0xc8, 5,   1,   0x40, 0x6e, 0x12, 0,
	};
	static const guint8 bad[] = { 0x80, 0, 0, 0x02, 0, 2, 0x40 };
	static const guint8 receipt[] = { 0x80, 0, 0, 0x40, 0, 4, 1, 1, 0, 0 };
	static const guint8 truncated[] = { 0x80, 0, 0, 0x42, 0, 4,
					    1,	  1, 0, 0,    2, 0x40 };
	static const guint8 late[] = {
		0x80, 0, 0, 0x40, 0, 6, 4, 1, 0, 1, 2, 1
	};
	static const guint8 traced[] = "\x80\0"
				       "192.0.2.7"
				       "\0\0\x08"
				       "\x05\x01\x40\x6e\x12\0";
	static const guint8 unknown[] = "\x80\0\0\x80\0"
					"fixture-body";
	/* Group, handlers, every member accepted. */
	const guint8 config = 0x01 | 0x04 | 0x10;
	GByteArray *out = g_byte_array_new();

	byte(out, config);
	byte(out, 3);
	pdtp_receive(out, 40, bad, sizeof(bad));
	pdtp_receive(out, 42, open, sizeof(open));
	pdtp_wait(out, 25);
	byte(out, 3);
	pdtp_receive(out, 43, receipt, sizeof(receipt));
	byte(out, 3);
	save_array("pdtp", "group-ack", out);

	out = g_byte_array_new();
	byte(out, config);
	for (guint i = 0; i < 3; i++) {
		byte(out, 3);
		pdtp_wait(out, 300 << i);
	}
	pdtp_receive(out, 8, truncated, sizeof(truncated));
	pdtp_receive(out, 9, late, sizeof(late));
	byte(out, 3);
	save_array("pdtp", "group-setup", out);

	out = g_byte_array_new();
	byte(out, config | 0x02);
	byte(out, 3);
	pdtp_receive(out, 1, traced, sizeof(traced) - 1);
	pdtp_receive(out, 2, unknown, sizeof(unknown) - 1);
	save_array("pdtp", "trace-metadata", out);
}

struct capture {
	GPtrArray *packets;
};

static gboolean capture(const guint8 *packet, gsize size, gpointer data)
{
	struct capture *c = data;

	g_ptr_array_add(c->packets, g_bytes_new(packet, size));
	return TRUE;
}

static void capture_reset(struct capture *c)
{
	g_ptr_array_set_size(c->packets, 0);
}

/* Records the relay's packets as plaintext receive operations. */
static void relay_records(GByteArray *out, srtp_t observer, GPtrArray *wire)
{
	for (guint i = 0; i < wire->len; i++) {
		gsize size;
		const guint8 *p = g_bytes_get_data(wire->pdata[i], &size);
		guint8 packet[4096 + SRTP_MAX_TRAILER_LEN];
		int n = size;

		g_assert_cmpuint(size, <=, sizeof(packet));
		memcpy(packet, p, size);
		g_assert_cmpint(srtp_unprotect(observer, packet, &n), ==,
				srtp_err_status_ok);
		record(out, 0, packet, n);
	}
}

static void deliver_all(struct call_pdtp *to, GPtrArray *wire, gint64 now)
{
	for (guint i = 0; i < wire->len; i++) {
		gsize size;
		const guint8 *p = g_bytes_get_data(wire->pdata[i], &size);

		call_pdtp_receive(to, p, size, now);
	}
}

/*
 * A one-to-one exchange with Avernella's own encoder on both sides. The
 * fuzzer's session opens and writes a stream and polls, as the recorded
 * operations make it do again. A relay session receives that, opens its
 * own stream and answers; its packets, decrypted, become receive records,
 * so their ACKs name the packets the fuzzer's session sent.
 */
static void pdtp_exchange(void)
{
	struct call_pdtp *local = call_pdtp_open(local_key, relay_key, DATA_PT,
						 LOCAL_SSRC, RELAY_SSRC, NULL);
	struct call_pdtp *relay = call_pdtp_open(relay_key, local_key, DATA_PT,
						 RELAY_SSRC, LOCAL_SSRC, NULL);
	struct capture from_local = { g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_bytes_unref) };
	struct capture from_relay = { g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_bytes_unref) };
	GByteArray *out = g_byte_array_new();
	guint8 message[600];
	gint64 now = 1000000;
	srtp_t observer;

	g_assert_nonnull(local);
	g_assert_nonnull(relay);
	g_assert_true(call_srtp_open(&observer, relay_key, ssrc_specific,
				     RELAY_SSRC));
	for (guint i = 0; i < sizeof(message); i++)
		message[i] = i * 7;
	byte(out, 0x04); /* One-to-one with handlers. */

	/* Outgoing: open stream 4 on PLANET, send one message, poll. */
	call_pdtp_stream_open(local, 4, "PLANET", 0x12);
	byte(out, 7);
	byte(out, 0);
	byte(out, 4);
	byte(out, 0x12);
	call_pdtp_stream_send(local, 4, message, sizeof(message), 33);
	byte(out, 7);
	byte(out, 1);
	byte(out, 4);
	u16(out, sizeof(message));
	g_byte_array_append(out, message, sizeof(message));
	u16(out, 33);
	/* The first poll starts the send budget; 2 ms later it sends. */
	call_pdtp_poll(local, now, capture, &from_local);
	byte(out, 3);
	now += 2000;
	call_pdtp_poll(local, now, capture, &from_local);
	byte(out, 0x23);
	g_assert_cmpuint(from_local.packets->len, >, 0);

	/* The relay receives it, opens stream 3, sends and acknowledges. */
	deliver_all(relay, from_local.packets, now);
	call_pdtp_stream_open(relay, 3, "PLANET", 0x12);
	call_pdtp_stream_send(relay, 3, message, 200, 40);
	call_pdtp_stream_send(relay, 3, message + 200, 300, 80);
	call_pdtp_poll(relay, now, capture, &from_relay);
	call_pdtp_poll(relay, now + 2000, capture, &from_relay);
	call_pdtp_poll(relay, now + 30000, capture, &from_relay);
	g_assert_cmpuint(from_relay.packets->len, >, 1);
	relay_records(out, observer, from_relay.packets);
	pdtp_wait(out, 30);
	byte(out, 3);
	save("pdtp", "exchange", out->data, out->len);

	/* Then the relay closes its stream. */
	capture_reset(&from_relay);
	call_pdtp_stream_close(relay, 3);
	now += 60000;
	call_pdtp_poll(relay, now, capture, &from_relay);
	relay_records(out, observer, from_relay.packets);
	byte(out, 3);
	save_array("pdtp", "close", out);

	srtp_dealloc(observer);
	g_ptr_array_unref(from_relay.packets);
	g_ptr_array_unref(from_local.packets);
	call_pdtp_close(relay);
	call_pdtp_close(local);
}

/* ---- Video ---- */

/* An EVS3 fragment as in tests/call_video.c source_fragment(). */
static gsize evs3_fragment(guint8 *packet, guint8 seq, guint8 picture,
			   int source, guint offset, guint length)
{
	const guint8 header[] = { 0x80, VIDEO_PT,   0,	     seq, 0,
				  0,	0,	    picture, 0,	  0,
				  0,	VIDEO_SSRC, 0x80,    0,	  picture };
	gsize n = sizeof(header);

	memcpy(packet, header, n);
	if (offset + length == sizeof(gray_key)) {
		packet[1] |= 0x80;
		packet[12] |= 0x04;
	}
	if (!offset) {
		guint size = sizeof(gray_key) + 3;

		packet[12] |= 0x3a;
		packet[n++] = 0x10;
		packet[n++] = source < 0 ? 0x20 : 0x21;
		if (source >= 0) {
			const guint8 tag[] = {
				0x80, 0x84, 1, picture, 2, source
			};

			memcpy(packet + n, tag, sizeof(tag));
			n += sizeof(tag);
		}
		packet[n++] = 0x08 | (size & 3);
		packet[n++] = size >> 2;
		packet[n++] = size >> 10;
	}
	memcpy(packet + n, gray_key + offset, length);
	return n + length;
}

static void video_fixtures(void)
{
	guint8 packet[256];
	GByteArray *out = g_byte_array_new();

	/* Group receive with statistics: whole, split and untagged. */
	byte(out, 0x03);
	record(out, 0, packet, evs3_fragment(packet, 1, 1, 0, 0, 29));
	record(out, 0, packet, evs3_fragment(packet, 2, 2, 1, 0, 18));
	record(out, 0, packet, evs3_fragment(packet, 3, 2, -1, 18, 11));
	record(out, 0, packet, evs3_fragment(packet, 5, 3, -1, 0, 29));
	byte(out, 3);
	u16(out, 0);
	save_array("video", "evs3-group", out);

	/* RFC 7741 descriptors: a keyframe, then the following frame. */
	out = g_byte_array_new();
	byte(out, 0);
	const guint8 key_header[] = {
		0x80, 0x80 | VIDEO_PT, 0,   1, 0, 0, 0, 1, 0, 0,
		0,    VIDEO_SSRC,      0x10
	};
	const guint8 delta_header[] = {
		0x80, 0x80 | VIDEO_PT, 0,    2,	   0,	0, 0x0b, 0xb9, 0, 0,
		0,    VIDEO_SSRC,      0x90, 0x80, 0x01
	};

	memcpy(packet, key_header, sizeof(key_header));
	memcpy(packet + sizeof(key_header), gray_key, sizeof(gray_key));
	record(out, 0, packet, sizeof(key_header) + sizeof(gray_key));
	memcpy(packet, delta_header, sizeof(delta_header));
	memcpy(packet + sizeof(delta_header), gray_delta, sizeof(gray_delta));
	record(out, 0, packet, sizeof(delta_header) + sizeof(gray_delta));
	save_array("video", "rfc7741", out);

	/* A screen-share message, as PDTP delivers it. */
	out = g_byte_array_new();
	byte(out, 0);
	gsize n = evs3_fragment(packet, 1, 9, -1, 0, sizeof(gray_key));

	/* Without the RTP header and the frame length. */
	memmove(packet + 12 + 5, packet + 12 + 8, n - 12 - 8);
	record(out, 2, packet + 12, n - 12 - 3);
	save_array("video", "share-fixture", out);
}

struct video_capture {
	GPtrArray *packets;
};

static gboolean video_collect(const guint8 *wire, gsize size, gpointer data)
{
	struct video_capture *c = data;

	g_ptr_array_add(c->packets, g_bytes_new(wire, size));
	return TRUE;
}

static void fill_picture(guint8 *pixels, guint w, guint h, guint shift)
{
	for (guint y = 0; y < h; y++)
		for (guint x = 0; x < w; x++)
			pixels[y * w + x] = (x * 3 + y * 5 + shift) & 0xff;
	memset(pixels + w * h, 128, w * h / 2);
}

/* Two pictures from the real sender, decrypted into receive records. */
static void video_encoded(const char *name, gboolean group,
			  enum call_video_rtp_extension extension)
{
	enum {
		W = 160,
		H = 96
	};
	g_autofree guint8 *pixels = g_malloc(W * H * 3 / 2);
	struct video_capture c = { g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_bytes_unref) };
	struct line_camera_frame frame;
	GByteArray *out = g_byte_array_new();
	struct call_video *tx;
	srtp_t observer;

	tx = group ? call_video_open_group_tx(video_key, VIDEO_PT, VIDEO_SSRC,
					      300, 10, NULL) :
		     call_video_open_tx(video_key, VIDEO_PT, VIDEO_SSRC, 300,
					10, NULL);
	g_assert_nonnull(tx);
	if (group)
		call_video_set_group_format(tx, extension, FALSE);
	g_assert_true(call_srtp_open(&observer, video_key, ssrc_specific,
				     VIDEO_SSRC));
	byte(out, group ? 0x01 : 0);
	for (guint i = 0; i < 2; i++) {
		g_autoptr(GError) error = NULL;

		fill_picture(pixels, W, H, i * 9);
		line_camera_i420_frame(&frame, pixels, W, H);
		g_assert_true(call_video_encode(tx, &frame,
						1000000 + i * 100000,
						video_collect, &c, &error));
	}
	for (guint i = 0; i < c.packets->len; i++) {
		gsize size;
		const guint8 *p = g_bytes_get_data(c.packets->pdata[i], &size);
		guint8 packet[4096 + SRTP_MAX_TRAILER_LEN];
		int n = size;

		memcpy(packet, p, size);
		if (srtp_unprotect(observer, packet, &n) == srtp_err_status_ok)
			record(out, 0, packet, n);
	}
	save_array("video", name, out);
	srtp_dealloc(observer);
	g_ptr_array_unref(c.packets);
	call_video_close(tx);
}

static gboolean share_collect(const guint8 *fragment, gsize size, gboolean key,
			      gpointer data)
{
	(void)key;
	record(data, 2, fragment, size);
	return TRUE;
}

static void video_share(void)
{
	enum {
		W = 128,
		H = 72
	};
	g_autofree guint8 *pixels = g_malloc(W * H * 3 / 2);
	struct call_share_encoder *enc = call_share_encoder_new(1000, 5);
	struct line_camera_frame frame;
	GByteArray *out = g_byte_array_new();

	g_assert_nonnull(enc);
	byte(out, 0);
	for (guint i = 0; i < 2; i++) {
		g_autoptr(GError) error = NULL;

		fill_picture(pixels, W, H, i * 40);
		line_camera_i420_frame(&frame, pixels, W, H);
		g_assert_true(call_share_encode(enc, &frame,
						1000000 + i * 250000,
						share_collect, out, &error));
	}
	save_array("video", "share-encoded", out);
	call_share_encoder_free(enc);
}

/* ---- Session descriptions (pb) ---- */

/* tests/call_video.c peer_video(). */
static void peer_video(Pb *out, gboolean packed, guint8 packet_version,
		       guint8 pt, guint32 local, guint32 remote)
{
	static const guint8 packed_maps[] = { 4, 2 };
	Pb stream = { 0 }, head = { 0 }, rtp = { 0 }, feature = { 0 };

	pb_text(&head, 1, "V");
	pb_uint(&head, 2, 3);
	pb_uint(&head, 3, 1);
	pb_uint(&head, 4, 2500);
	pb_uint(&head, 5, 30);
	pb_uint(&head, 6, 3);
	pb_uint(&head, 50, 3);
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
	pb_uint(&feature, 2, 2);
	pb_msg(&stream, 51, &feature);
	if (packet_version) {
		feature.n = 0;
		pb_uint(&feature, 1, 2);
		pb_uint(&feature, 2, packet_version);
		pb_msg(&stream, 51, &feature);
	}
	pb_msg(out, 1, &stream);
}

static void pb_seeds(void)
{
	/* The official app's inactive VP8 answer from tests/call_video.c. */
	static const char live[] =
		"0a110a0156100318002090032818300290030212170861587062050843"
		"109235e803d401f20305084310a26a1a21087f12170861587062050843"
		"109235e803d401f20305084310a26a5870e803d401";
	guint8 live_bytes[sizeof(live) / 2];
	CallInfo info = { .local = "u00000000000000000000000000000001",
			  .target = "u00000000000000000000000000000002",
			  .token = "fixture-token",
			  .i_zone = "test-i",
			  .r_zone = "test-r",
			  .stid = "fixture-stid",
			  .cid = "fixture-cid",
			  .audio_payload = 106,
			  .video_payload = 109,
			  .video = TRUE,
			  .nonce = 7 };
	Pb pb = { 0 };

	peer_video(&pb, TRUE, 2, 109, 0x10203040, 0);
	message("pb", "offer-packed", 1, 0, &pb);
	message("pb", "limits", 3, 0, &pb);
	message("pb", "describe", 4, 0, &pb);
	pb.n = 0;
	peer_video(&pb, FALSE, 0, 110, 0x10203040, 0x50607080);
	message("pb", "answer", 1, 0, &pb);
	message("pb", "session-video", 2, 1, &pb);
	pb.n = 0;
	for (gsize i = 0; i < sizeof(live_bytes); i++)
		live_bytes[i] = g_ascii_xdigit_value(live[2 * i]) << 4 |
				g_ascii_xdigit_value(live[2 * i + 1]);
	pb_bytes(&pb, 1, (Slice){ live_bytes, sizeof(live_bytes) });
	message("pb", "answer-inactive", 1, 0, &pb);

	pb.n = 0;
	call_offer(&pb, &info);
	message("pb", "offer", 1, 0, &pb);
	message("pb", "session-audio", 2, 0, &pb);
	message("pb", "session-data", 2, 2, &pb);
	message("pb", "walk-offer", 0, 50, &pb);
	pb.n = 0;
	call_answer_offer(&pb, &info);
	message("pb", "answer-offer", 1, 0, &pb);
	pb.n = 0;
	call_video_propose(&info);
	call_offer(&pb, &info);
	message("pb", "video-options-offer", 1, 0, &pb);
	pb.n = 0;
	call_answer_offer(&pb, &info);
	message("pb", "video-options-answer", 1, 0, &pb);
	pb.n = 0;
	call_setup(&pb, &info);
	message("pb", "walk-setup", 0, 9, &pb);
	pb.n = 0;
	call_verify(&pb, &info);
	message("pb", "walk-verify", 0, 5, &pb);
	pb.n = 0;
	call_stream_spec(&pb, 10010, 10020, 20010, 20020, 122, 222, FALSE);
	message("pb", "walk-stream-spec", 0, 1, &pb);
	message("pb", "wire", 6, 0, &pb);
	message("pb", "wire-boot", 6, 1, &pb);
	message("pb", "wire-raw", 6, 2, &pb);

	/* A reply header: transaction ID and sequence. */
	const guint8 transaction[16] = { 1, 2, 3, 4 };

	pb.n = 0;
	pb_text(&pb, 1, info.target);
	pb_bytes(&pb, 4, (Slice){ transaction, sizeof(transaction) });
	pb_uint(&pb, 5, 9);
	message("pb", "envelope", 5, 0, &pb);
	message("pb", "envelope-media", 5, 2, &pb);
}

/* ---- Group calls ---- */

/* tests/call_group.c make_answer(). */
static void make_answer(Pb *reply, guint media, guint video_map,
			int vp8_version, int packet_version, gboolean packed)
{
	static const char *const label[] = { "A", "V", "D" };
	guint8 key[30];
	Pb offer = { 0 }, security = { 0 }, secret = { 0 }, features = { 0 };

	for (guint i = 0; i < sizeof(key); i++)
		key[i] = i;
	for (guint i = 0; i < G_N_ELEMENTS(label); i++) {
		Pb record = { 0 }, codec = { 0 }, path = { 0 };
		Pb rx_aux = { 0 }, tx_aux = { 0 };

		if (!(media & (1u << i)))
			continue;
		pb_text(&codec, 1, label[i]);
		if (i == 1 && video_map) {
			Pb feature = { 0 };

			pb_uint(&codec, 2, 3);
			pb_uint(&codec, 3, 1);
			if (packed) {
				const guint8 maps[] = { video_map,
							video_map == 2 ? 7 :
									 2 };

				pb_bytes(&codec, 50,
					 (Slice){ maps, sizeof(maps) });
			} else {
				pb_uint(&codec, 50, video_map);
			}
			if (vp8_version >= 0) {
				pb_uint(&feature, 1, 0);
				pb_uint(&feature, 2, vp8_version);
				pb_msg(&record, 51, &feature);
				feature.n = 0;
			}
			if (packet_version >= 0) {
				pb_uint(&feature, 1, 2);
				pb_uint(&feature, 2, packet_version);
				pb_msg(&record, 51, &feature);
			}
		}
		if (i == 2) {
			const guint8 maps[] = { 6, 2 };

			pb_uint(&codec, 2, 3);
			pb_uint(&codec, 3, 1);
			if (packed)
				pb_bytes(&codec, 50,
					 (Slice){ maps, sizeof(maps) });
			else
				pb_uint(&codec, 50, 6);
		}
		pb_uint(&path, 1, i == 1 && video_map ? 105 : 96 + i);
		pb_uint(&path, 11, 0xe3530179 + 10 * i);
		pb_uint(&rx_aux, 1, 67);
		pb_uint(&rx_aux, 2, 0x05060708 + i);
		pb_msg(&path, 12, &rx_aux);
		pb_uint(&path, 61, 0xe353017d + 10 * i);
		pb_uint(&tx_aux, 1, 67);
		pb_uint(&tx_aux, 2, 0x01020304 + i);
		pb_msg(&path, 62, &tx_aux);
		pb_msg(&record, 1, &codec);
		pb_msg(&record, 2, &path);
		pb_msg(&offer, 1, &record);
	}
	pb_bytes(&secret, 1, (Slice){ key, sizeof(key) });
	pb_msg(&security, 2, &secret);
	pb_msg(&offer, 2, &security);
	pb_uint(&features, 1, 0);
	pb_uint(&features, 2, 3);
	pb_msg(&offer, 3, &features);
	pb_uint(reply, 1, 0);
	pb_msg(reply, 6, &offer);
	pb_uint(reply, 7, 0x12345678);
	pb_uint(reply, 102, 25);
}

static void add_source(Pb *out, guint field, const char *kind, guint32 ssrc)
{
	Pb source = { 0 };

	pb_text(&source, 1, kind);
	pb_uint(&source, 2, ssrc);
	pb_msg(out, field, &source);
}

static void group_presence_seeds(void)
{
	Pb info = { 0 }, member = { 0 }, reply = { 0 }, count = { 0 };
	guint8 compressed[256];
	uLongf compressed_size = sizeof(compressed);

	/* tests/call_group.c conference_members(). */
	pb_uint(&info, 1, 1);
	pb_uint(&info, 2, 123);
	pb_uint(&count, 1, 2);
	pb_msg(&info, 3, &count);
	pb_text(&member, 1, guest);
	pb_uint(&member, 2, 1);
	pb_uint(&member, 3, 1);
	add_source(&member, 10, "A", 0xabab);
	add_source(&member, 10, "V", 0x12345678);
	add_source(&member, 10, "D", 0x12345677);
	pb_msg(&info, 50, &member);
	member.n = 0;
	pb_text(&member, 1, "service-participant");
	pb_uint(&member, 3, 1);
	add_source(&member, 10, "V", 0x12345679);
	pb_msg(&info, 50, &member);
	pb_uint(&reply, 8, 1);
	pb_msg(&reply, 9, &info);
	message("group", "presence-info", 1, 0, &reply);

	info.n = member.n = reply.n = 0;
	pb_text(&member, 1, guest);
	pb_uint(&member, 2, 1);
	pb_uint(&member, 3, 1);
	pb_msg(&info, 1, &member);
	g_assert_cmpint(compress2(compressed, &compressed_size, info.p, info.n,
				  Z_BEST_SPEED),
			==, Z_OK);
	pb_uint(&reply, 1, 5);
	pb_uint(&reply, 3, 1);
	pb_bytes(&reply, 2, (Slice){ compressed, compressed_size });
	message("group", "presence-compressed", 1, 1, &reply);
	reply.n = 0;
	pb_uint(&reply, 1, 5);
	pb_msg(&reply, 2, &info);
	message("group", "presence-push", 1, 1, &reply);

	/* tests/call_group.c channel_members(). */
	info.n = member.n = reply.n = 0;
	pb_uint(&info, 1, 7);
	pb_uint(&info, 2, 42);
	pb_text(&member, 1, guest);
	pb_uint(&member, 3, 2);
	add_source(&member, 11, "V", 47);
	pb_msg(&info, 11, &member);
	pb_uint(&reply, 1, 42);
	pb_uint(&reply, 2, 21);
	pb_msg(&reply, 11, &info);
	message("group", "presence-channel", 1, 2, &reply);
}

static void group_seeds(void)
{
	static const struct {
		const char *name;
		guint media, map;
		int vp8, packet;
		gboolean packed;
	} answers[] = {
		{ "answer-audio", 1, 0, -1, 0, FALSE },
		{ "answer-data", 7, 0, -1, 0, TRUE },
		{ "answer-vp8", 7, CALL_GROUP_VIDEO_VP8, 1, 0, FALSE },
		{ "answer-svc", 3, CALL_GROUP_VIDEO_SVC, 2, 1, TRUE },
		{ "answer-svc-sw", 3, CALL_GROUP_VIDEO_SVC_SW, 1, 0, FALSE },
	};
	Pb pb = { 0 }, update = { 0 };

	for (guint i = 0; i < G_N_ELEMENTS(answers); i++) {
		pb.n = 0;
		make_answer(&pb, answers[i].media, answers[i].map,
			    answers[i].vp8, answers[i].packet,
			    answers[i].packed);
		message("group", answers[i].name, 0, i, &pb);
	}
	group_presence_seeds();

	/* tests/call_group.c stream_control(). */
	pb.n = 0;
	pb_uint(&update, 1, CALL_GROUP_STREAM_RESUME);
	pb_uint(&update, 3, 0x82314567);
	pb_uint(&update, 5, 42);
	pb_uint(&pb, 1, 0);
	pb_msg(&pb, 4, &update);
	message("group", "stream-response", 2, 1, &pb);
	pb.n = 0;
	update.p[1] = CALL_GROUP_STREAM_STOP;
	pb_text(&update, 2, guest);
	pb_msg(&pb, 1, &update);
	message("group", "stream-notify", 2, 0, &pb);

	/* tests/call_group.c camera_resolution(), camera source 42. */
	pb.n = 0;
	pb_uint(&pb, 1, 1);
	pb_uint(&pb, 3, 42);
	pb_uint(&pb, 4, 0);
	for (guint layer = 0; layer < 4; layer++) {
		Pb record = { 0 };

		pb_uint(&record, 1, layer);
		pb_uint(&record, 2, layer & 1);
		pb_msg(&pb, 8, &record);
	}
	message("group", "camera", 3, 84, &pb);
	pb_uint(&pb, 5, 2);
	message("group", "camera-clear", 3, 84, &pb);

	/* Raw RTCP from tests/call_group.c. */
	static const guint8 retransmit[] = {
		0xa5, 205,  0, 4, 0,	0,    0, 0, 0x11, 0x22,
		0x33, 0x44, 0, 2, 0xff, 0xfe, 0, 1, 0,	  0,
	};
	static const guint8 challenge[] = {
		0xac, 205, 0, 4, 5,    6,    7,	   8,	 5, 6,
		7,    8,   6, 1, 0x12, 0x34, 0x56, 0x78, 0, 0,
	};
	static const guint8 interval[] = {
		0x8c, 205, 0, 3, 5, 6, 7, 8, 5, 6, 7, 8, 0x82, 0, 0, 80,
	};
	memcpy(pb.p, retransmit, sizeof(retransmit));
	pb.n = sizeof(retransmit);
	message("group", "retransmit", 4, 0, &pb);
	memcpy(pb.p, interval, sizeof(interval));
	pb.n = sizeof(interval);
	message("group", "control-interval", 5, 0, &pb);
	memcpy(pb.p, challenge, sizeof(challenge));
	pb.n = sizeof(challenge);
	message("group", "control-ping", 5, 0, &pb);

	/* Arrivals with a gap and a wrap, reports, a control packet. */
	static const guint16 sequences[] = { 65534, 65535, 1, 2, 0, 5 };
	GByteArray *out = g_byte_array_new();

	byte(out, 6);
	byte(out, 0);
	for (guint i = 0; i < G_N_ELEMENTS(sequences); i++) {
		byte(out, (i % 2) << 2);
		byte(out, 40);
		u16(out, sequences[i]);
		u16(out, 0);
		u16(out, 90 * i);
	}
	/* Report: operation, clock, capacity. */
	byte(out, 2);
	byte(out, 200);
	u16(out, 1200);
	/* Control: operation, clock, packet, response delay. */
	byte(out, 3);
	byte(out, 1);
	u16(out, sizeof(interval));
	g_byte_array_append(out, interval, sizeof(interval));
	byte(out, 2);
	byte(out, 2);
	byte(out, 10);
	u16(out, 64);
	/* Forget the second source. */
	byte(out, 1 << 2 | 1);
	byte(out, 0);
	save_array("group", "feedback", out);
}

/* ---- Thrift ---- */

static void thrift_seed(const char *name, guint select, const guint8 *p,
			gsize n)
{
	GByteArray *out = g_byte_array_new();

	byte(out, select);
	g_byte_array_append(out, p, n);
	save_array("thrift", name, out);
}

static void thrift_seeds(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(J) args = obj();
	/* tests/thrift.c */
	static const guint8 call[] = { 0x82, 0x21, 0,	 4,    't', 'e',
				       's',  't',  0x1c, 0x18, 2,   'a',
				       'b',  0x25, 13,	 0x12, 0,   0 };
	static const guint8 reply[] = { 0x82, 0x41, 0,	 1, 'm', 0x0c, 0,
					0x18, 3,    'A', 0, 'B', 0,    0 };
	static const guint8 denied[] = { 0x82, 0x41, 0,	 1, 'm',
					 0x1c, 0x15, 16, 0, 0 };
	static const guint8 auth[] = { 0x80, 1, 0, 2,	0, 0,	0,  1, 'm', 0,
				       0,    0, 0, 12,	0, 0,	11, 0, 1,   0,
				       0,    0, 3, 'A', 0, 'B', 10, 0, 3,   0,
				       0,    0, 0, 0,	0, 0,	60, 0, 0 };
	static const guint8 invalid[] = { 0x15, 0x02, 0x05, 0x02, 0x04, 0 };
	J *s = obj(), *list = json_object_new_array(), *map = obj();

	thrift_seed("struct", 0, call + 8, sizeof(call) - 8);
	thrift_seed("reply", 1, reply, sizeof(reply));
	thrift_seed("reply-exception", 1, denied, sizeof(denied));
	thrift_seed("auth", 2, auth, sizeof(auth));
	thrift_seed("invalid", 0, invalid, sizeof(invalid));

	/* Every Compact type the encoder writes, nested. */
	json_object_object_add(s, "1:str", json_object_new_string("ab"));
	json_object_object_add(s, "2:i64", json_object_new_int64(-1234567));
	json_object_object_add(s, "3:i32", json_object_new_int(-7));
	json_object_object_add(s, "4:bool", json_object_new_boolean(TRUE));
	json_object_array_add(list, json_object_new_string("x"));
	json_object_array_add(list, json_object_new_string("y"));
	json_object_object_add(s, "5:list:str", list);
	json_object_object_add(map, "k", json_object_new_string("v"));
	json_object_object_add(s, "6:map:str:str", map);
	json_object_object_add(args, "1:obj", s);
	g_autoptr(GBytes) packet = line_encode_call("m", args, &error);

	g_assert_no_error(error);
	gsize size;
	const guint8 *p = g_bytes_get_data(packet, &size);

	thrift_seed("struct-types", 0, p + 5, size - 5);
	/* The same structure as a successful reply's field 0. */
	GByteArray *wrapped = g_byte_array_new();

	g_byte_array_append(wrapped,
			    (const guint8 *)"\x82\x41\x00\x01m\x0c\x00", 7);
	g_byte_array_append(wrapped, p + 5, size - 5);
	g_byte_array_append(wrapped, (const guint8 *)"\0", 1);
	thrift_seed("reply-types", 1, wrapped->data, wrapped->len);
	g_byte_array_unref(wrapped);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		g_printerr("usage: %s DIRECTORY\n", argv[0]);
		return 2;
	}
	root = argv[1];
	g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	pdtp_fixtures();
	pdtp_exchange();
	video_fixtures();
	video_encoded("encoded", FALSE, CALL_VIDEO_RTP_EXT_NONE);
	video_encoded("encoded-group-vfd", TRUE, CALL_VIDEO_RTP_EXT_VFD);
	video_encoded("encoded-group-ved", TRUE, CALL_VIDEO_RTP_EXT_VED);
	video_share();
	pb_seeds();
	group_seeds();
	thrift_seeds();
	g_print("Wrote %u seeds to %s\n", written, root);
	return 0;
}
