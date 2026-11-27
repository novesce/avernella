// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Protobuf reading in src/call_wire.c and the session descriptions in
 * src/call_schema.c: pb_next(), pb_data() and pb_number(), the video
 * profile and its codec maps and features, session RTP parameters, peer
 * receive limits, the debug description, reply headers, and the encrypted
 * signaling frames of call_wire_pack() and call_wire_open().
 *
 * Input: a selector byte, an argument byte, then the message.
 */
#include "call_schema.h"
#include "fuzz.h"
#include <string.h>

enum {
	SELECT_WALK,
	SELECT_VIDEO_PROFILE,
	SELECT_SESSION,
	SELECT_LIMITS,
	SELECT_DESCRIBE,
	SELECT_ENVELOPE,
	SELECT_WIRE,
	SELECTS
};

/* A loopback wire: its receive keys are its own send keys. */
static CallWire loopback;

/* Walks nested messages the way the parsers do, with every accessor. */
static void walk(Slice message, guint depth, guint key)
{
	Slice rest = message;
	PbField field;

	pb_data(message, key);
	pb_number(message, key, 0);
	while (rest.n && pb_next(&rest, &field)) {
		g_assert_cmpuint(field.field, >=, 1);
		g_assert_cmpuint(field.type, <=, 5);
		g_assert_true(field.data.n <= message.n);
		if (field.data.n) {
			g_assert_true(field.data.p >= message.p);
			g_assert_true(field.data.p + field.data.n <=
				      message.p + message.n);
			fuzz_touch(field.data.p, field.data.n);
		}
		if (field.type == 2 && depth < 8)
			walk(field.data, depth + 1, key);
	}
}

static void video_profile(Slice description)
{
	guint8 pt = 0;
	guint32 local = 0, remote = 0;

	CallInfo caller = { .video = TRUE };

	call_video_propose(&caller);
	struct call_video_options offer =
		call_video_options_read(description, NULL);
	struct call_video_options answer =
		call_video_options_read(description, &caller.video_options);

	g_assert_false(offer.flags && answer.flags);
	if (offer.flags || answer.flags) {
		const struct call_video_options *o = offer.flags ? &offer :
								   &answer;
		g_assert_cmpuint(o->flags & ~CALL_VIDEO_FEATURES, ==, 0);
		g_assert_cmpuint(o->repair_pt, >=, 96);
		g_assert_cmpuint(o->repair_pt, <, 127);
		g_assert_cmpuint(o->caller_repair, !=, o->callee_repair);
	}
	if (call_video_offer_profile(description, &pt, &local, &remote)) {
		g_assert_cmpuint(pt, >=, 96);
		g_assert_cmpuint(pt, <=, 127);
		g_assert_cmpuint(local, !=, 0);
		g_assert_cmpuint(local, !=, remote);
	}
	if (call_video_profile(description, &pt, &local, &remote)) {
		g_assert_cmpuint(pt, >=, 96);
		g_assert_cmpuint(pt, <=, 127);
		g_assert_cmpuint(local, !=, 0);
		g_assert_cmpuint(remote, !=, 0);
		g_assert_cmpuint(local, !=, remote);
	}
}

static void session(Slice description, guint arg)
{
	static const char labels[] = "AVD";
	char label = labels[arg % 3];
	guint8 pt = 0, payload = call_session_payload(description, label);
	guint32 local = 0, remote = 0;

	g_assert_true(!payload || (payload >= 96 && payload <= 127));
	if (call_session_rtp(description, label, &pt, &local, &remote)) {
		g_assert_cmpuint(pt, >=, 96);
		g_assert_cmpuint(pt, <=, 127);
		g_assert_cmpuint(local, !=, 0);
		g_assert_cmpuint(remote, !=, 0);
		g_assert_cmpuint(local, !=, remote);
	}
}

static void limits(Slice description)
{
	struct call_video_limits limits;

	call_video_peer_limits(description, &limits);
	g_assert_cmpuint(limits.kbps, <=, 100000);
	g_assert_cmpuint(limits.fps, <=, 120);
	g_assert_cmpuint(limits.resolution, <=, 4);
}

static void envelope(Slice header, guint arg)
{
	CallInfo info = { .local = "u00000000000000000000000000000000",
			  .cid = "fuzz",
			  .nonce = 1,
			  .remote_nonce = arg & 1,
			  .have_session = !!(arg & 2) };
	Pb body = { 0 }, out = { 0 };

	pb_uint(&body, 1, arg);
	if (call_envelope(&out, &info, 2 + arg % 3, 1 + (arg >> 4), 7, &body,
			  header))
		walk(PB_SLICE(out), 0, 1);
}

/* Packed frames must open to the same plaintext, and fail once altered. */
static void wire(Slice input, guint arg)
{
	CallWire tx = loopback, rx = loopback;
	Pb plain = { 0 }, packet = { 0 }, opened = { 0 };
	gboolean boot = arg & 1;

	plain.n = MIN(input.n, sizeof(plain.p));
	if (plain.n)
		memcpy(plain.p, input.p, plain.n);
	rx.have_rx = !boot;
	if (call_wire_pack(&tx, &plain, boot, arg & 2, &packet)) {
		g_assert_true(call_wire_open(&rx, PB_SLICE(packet), &opened));
		g_assert_cmpmem(opened.p, opened.n, plain.p, plain.n);
		packet.p[(arg >> 2) * 7 % packet.n] ^= 1;
		g_assert_false(call_wire_open(&rx, PB_SLICE(packet), &opened));
	}
	/* The input itself as a frame. */
	call_wire_open(&rx, input, &opened);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	EVP_PKEY *peer = NULL;
	guint8 pub[33];

	(void)argc;
	(void)argv;
	fuzz_quiet();
	g_assert_true(call_keygen(&peer, pub));
	g_assert_true(call_wire_init(&loopback, pub));
	memcpy(loopback.rx_base, loopback.tx_base, sizeof(loopback.rx_base));
	memcpy(loopback.rx, loopback.tx, sizeof(loopback.rx));
	EVP_PKEY_free(peer);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fuzz_input in = { data, size };
	guint select = fuzz_byte(&in) % SELECTS, arg = fuzz_byte(&in);
	Slice message = { in.p, in.n };

	switch (select) {
	case SELECT_WALK:
		walk(message, 0, arg);
		break;
	case SELECT_VIDEO_PROFILE:
		video_profile(message);
		break;
	case SELECT_SESSION:
		session(message, arg);
		break;
	case SELECT_LIMITS:
		limits(message);
		break;
	case SELECT_DESCRIBE:
		call_video_describe(message, "fuzz");
		break;
	case SELECT_ENVELOPE:
		envelope(message, arg);
		break;
	default:
		wire(message, arg);
		break;
	}
	return 0;
}
