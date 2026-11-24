// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Group call parsing in src/call_group.c: the PARTICIPATE answer and its
 * stream, codec and feedback records, presence pushes (also compressed),
 * stream updates, camera layer requests, RTCP retransmission requests and
 * feedback control, and the feedback reports built from fuzzed arrivals.
 *
 * Input: a selector byte, an argument byte, then the message. The feedback
 * selector reads operations instead: see feedback().
 */
#include "call_group.h"
#include "fuzz.h"
#include <string.h>

enum {
	SELECT_ANSWER,
	SELECT_PRESENCE,
	SELECT_STREAMS,
	SELECT_CAMERA,
	SELECT_RETRANSMIT,
	SELECT_CONTROL,
	SELECT_FEEDBACK,
	SELECTS
};

enum {
	FEEDBACK_SSRC = 0x05060708,
};

/* call_group_acquire() is not fuzzed; the stub keeps it linkable. */
J *line_rpc(Line *session, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)session;
	(void)path;
	(void)method;
	(void)args;
	g_set_error_literal(error, LINE_ERROR, 1, "No network in the fuzzer.");
	return NULL;
}

static void check_slice(Slice outer, Slice inner)
{
	if (!inner.n)
		return;
	g_assert_true(inner.p >= outer.p);
	g_assert_true(inner.p + inner.n <= outer.p + outer.n);
	fuzz_touch(inner.p, inner.n);
}

static void answer(Slice reply, guint arg)
{
	struct call_group_answer a;
	struct call_group_feedback feedback;
	enum call_group_result result = call_group_answer(reply, &a);
	Pb registration = { 0 };
	guint8 key[30];

	g_assert_cmpint(result, >=, CALL_GROUP_INVALID);
	g_assert_cmpint(result, <=, CALL_GROUP_JOINED);
	if (result != CALL_GROUP_JOINED)
		return;
	check_slice(reply, a.description);
	check_slice(reply, a.bridge);
	check_slice(reply, a.secret);
	g_assert_cmpuint(a.secret.n, ==, 30);
	g_assert_cmpuint(a.audio_rx, !=, 0);
	g_assert_cmpuint(a.audio_payload, >=, 96);
	g_assert_cmpuint(a.audio_payload, <=, 127);
	call_group_media_key(arg & 1 ? "VIDEO" : "AUDIO", a.secret.p, key);
	call_group_register(&registration, &a, arg & 2);
	call_group_feedback_init(&feedback, &a);
}

static void presence(Slice body, guint arg)
{
	static const guint choices[] = {
		CALL_GROUP_PARTICIPATE_RSP,
		CALL_GROUP_PUSH_REQ,
		CALL_GROUP_SUBPUSH_REQ,
		CALL_GROUP_NOTIFY_REQ,
	};
	struct call_group_presence p;

	if (!call_group_presence(body, choices[arg % 4], &p))
		return;
	g_assert_nonnull(p.members);
	g_assert_cmpuint(p.members->len, <=, p.wire_members);
	for (guint i = 0; i < p.members->len; i++) {
		const struct call_group_member *m =
			&g_array_index(p.members, struct call_group_member, i);

		g_assert_cmpuint(strlen(m->mid), ==, 33);
		g_assert_cmpint(m->mid[0], ==, 'u');
	}
	g_array_unref(p.members);
}

static void streams(Slice body, guint arg)
{
	GArray *updates = call_group_stream_updates(
		body, arg & 1 ? CALL_GROUP_STRM_RSP : CALL_GROUP_NOTIFY_REQ);

	if (!updates)
		return;
	for (guint i = 0; i < updates->len; i++) {
		const struct call_group_stream *s =
			&g_array_index(updates, struct call_group_stream, i);

		g_assert_cmpuint(s->ssrc, !=, 0);
		g_assert_cmpuint(s->state, <=, CALL_GROUP_STREAM_RESUME);
		g_assert_true(!s->mid[0] || strlen(s->mid) == 33);
	}
	g_array_unref(updates);
}

static void camera(Slice request, guint arg)
{
	guint requested = 0;
	int layer = call_group_camera_resolution(request, arg >> 1, arg & 1,
						 &requested);

	g_assert_cmpint(layer, >=, CALL_GROUP_LAYER_UNCHANGED);
	g_assert_cmpint(layer, <=, 4);
	g_assert_cmpuint(requested, <, 1u << 5);
}

static void retransmit(Slice packet)
{
	guint32 target = 0;
	guint count = call_group_retransmit_request(packet, &target);

	/* The sequence list fits in the packet. */
	g_assert_true(!count || (target && 14 + 2 * count <= packet.n));
}

static void init_feedback(struct call_group_feedback *feedback)
{
	struct call_group_answer a = {
		.feedback_version = 3,
		.feedback_rx = FEEDBACK_SSRC,
	};

	call_group_feedback_init(feedback, &a);
}

static void control(Slice packet, guint arg)
{
	struct call_group_feedback feedback;
	gboolean changed;
	guint8 response[24];

	init_feedback(&feedback);
	gsize n = call_group_feedback_control(&feedback, packet, 1000 + arg,
					      2000, response, &changed);

	g_assert_true(n == 0 || n == sizeof(response));
}

/*
 * Arrivals, forgotten sources, reports and control packets in any order.
 * Each operation is a byte, low two bits choosing it, then its fields.
 */
static void feedback(struct fuzz_input *in)
{
	struct call_group_feedback feedback;
	gint64 now = 1000000;
	guint8 report[1500];

	init_feedback(&feedback);
	while (in->n) {
		guint op = fuzz_byte(in);
		guint32 ssrc = 1 + (op >> 2) % 12;
		const guint8 *body;
		gsize n, capacity;
		gboolean changed;
		guint8 response[24];

		now += fuzz_byte(in) * 500;
		switch (op & 3) {
		case 0:
			call_group_feedback_receive(&feedback, ssrc,
						    fuzz_u16(in), fuzz_u32(in),
						    now);
			break;
		case 1:
			call_group_feedback_forget(&feedback, ssrc);
			break;
		case 2:
			capacity = fuzz_u16(in) % (sizeof(report) + 1);
			n = call_group_feedback_report(&feedback, now, report,
						       capacity);
			g_assert_cmpuint(n, <=, capacity);
			g_assert_cmpuint(n % 4, ==, 0);
			break;
		default:
			n = fuzz_chunk(in, &body);
			call_group_feedback_control(&feedback,
						    (Slice){ body, n }, now,
						    now + fuzz_byte(in) * 1000,
						    response, &changed);
			break;
		}
		g_assert_cmpuint(feedback.active_streams, <=,
				 CALL_GROUP_FEEDBACK_STREAMS);
		for (guint i = 0; i < CALL_GROUP_FEEDBACK_STREAMS; i++)
			g_assert_cmpuint(feedback.stream[i].count, <=,
					 CALL_GROUP_FEEDBACK_WINDOW);
	}
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	fuzz_quiet();
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fuzz_input in = { data, size };
	guint select = fuzz_byte(&in) % SELECTS, arg = fuzz_byte(&in);
	Slice message = { in.p, in.n };

	switch (select) {
	case SELECT_ANSWER:
		answer(message, arg);
		break;
	case SELECT_PRESENCE:
		presence(message, arg);
		break;
	case SELECT_STREAMS:
		streams(message, arg);
		break;
	case SELECT_CAMERA:
		camera(message, arg);
		break;
	case SELECT_RETRANSMIT:
		retransmit(message);
		break;
	case SELECT_CONTROL:
		control(message, arg);
		break;
	default:
		feedback(&in);
		break;
	}
	return 0;
}
