// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The PDTP receive path in src/call_pdtp.c: SRTP authentication, the RTP
 * header and conference route words, packet numbers, element groups and
 * each element, stream reassembly, ACKs and presenter selection. Outgoing
 * streams and polls give ACKs, window updates and pauses something to act
 * on, and every packet PDTP sends must authenticate at the relay.
 *
 * Input: a configuration byte, then operations. Each operation is a byte
 * whose low three bits choose it and whose high nibble advances the clock
 * by that many milliseconds. Packet bodies are a two-byte big-endian length
 * and the plaintext RTP packet. The harness holds the relay's synthetic SRTP
 * key, as tests/pdtp.c does, so fuzzed plaintext is protected and reaches
 * the parser. It fixes the RTP version, payload type and SSRC so that the
 * packet matches the session; everything else comes from the input.
 */
#include "call_pdtp.h"
#include "call_srtp.h"
#include "fuzz.h"
#include <string.h>

enum {
	DATA_PT = 101,
	LOCAL_SSRC = 0x1234,
	RELAY_SSRC = 0xabcd,
	MAX_PACKET = 4096,
};

static const guint8 local_key[30] = { 1, 2, 3 };
static const guint8 relay_key[30] = { 9, 8, 7 };

enum {
	CONFIG_GROUP = 1 << 0,
	CONFIG_TRACE = 1 << 1,
	CONFIG_HANDLERS = 1 << 2,
	CONFIG_STATS = 1 << 3,
	CONFIG_MEMBERS_SHIFT = 4,
};

enum {
	OP_RECEIVE,
	OP_RECEIVE_2,
	OP_RECEIVE_3,
	OP_POLL,
	OP_WAIT,
	OP_WATCH,
	OP_MEMBERS,
	OP_OUTGOING,
};

struct session {
	struct call_pdtp *pdtp;
	srtp_t relay, relay_rx;
	guint members;
	gint64 now;
	gboolean drop;
};

static gboolean member(guint32 source, gpointer data)
{
	const struct session *s = data;

	switch (s->members) {
	case 1:
		return TRUE;
	case 2:
		return !(source & 1);
	case 3:
		return source < 256;
	}
	return FALSE;
}

static void on_data(guint64 stream, const char *service, guint64 offset,
		    guint64 time, guint flags, const guint8 *bytes, gsize size,
		    gpointer data)
{
	g_assert_cmpuint(strlen(service), <, 64);
	g_assert_cmpuint(size, >, 0);
	g_assert_cmpuint(size, <=, MAX_PACKET);
	fuzz_touch(bytes, size);
	(void)stream;
	(void)offset;
	(void)time;
	(void)flags;
	(void)data;
}

static void on_event(guint64 stream, const char *service, int element,
		     gpointer data)
{
	g_assert_cmpuint(strlen(service), <, 64);
	g_assert_true(
		element == PDTP_STREAM_OPEN || element == PDTP_STREAM_CLOSE ||
		element == PDTP_STREAM_RESET || element == PDTP_EVENT_PAUSE ||
		element == PDTP_EVENT_RESUME);
	(void)stream;
	(void)data;
}

/* What PDTP sends must authenticate as an RTP packet of this session. */
static gboolean sink(const guint8 *packet, gsize size, gpointer data)
{
	struct session *s = data;
	guint8 buf[MAX_PACKET + SRTP_MAX_TRAILER_LEN];
	int n = size;

	if (s->drop)
		return FALSE;
	g_assert_cmpuint(size, >, 12);
	g_assert_cmpuint(size, <=, MAX_PACKET);
	memcpy(buf, packet, size);
	g_assert_cmpint(srtp_unprotect(s->relay_rx, buf, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpuint(buf[0] >> 6, ==, 2);
	g_assert_cmpuint(buf[1] & 127, ==, DATA_PT);
	return TRUE;
}

static void receive(struct session *s, struct fuzz_input *in, gboolean corrupt)
{
	guint8 packet[MAX_PACKET + SRTP_MAX_TRAILER_LEN] = { 0 };
	const guint8 *body;
	gsize n = fuzz_chunk(in, &body);
	int size;

	n = MIN(n, MAX_PACKET - SRTP_MAX_TRAILER_LEN);
	size = MAX(n, 12u);
	memcpy(packet, body, n);
	packet[0] = 0x80 | (packet[0] & 0x3f);
	packet[1] = (packet[1] & 0x80) | DATA_PT;
	packet[8] = 0;
	packet[9] = 0;
	packet[10] = RELAY_SSRC >> 8;
	packet[11] = RELAY_SSRC & 0xff;
	if (srtp_protect(s->relay, packet, &size) != srtp_err_status_ok)
		return;
	if (corrupt)
		packet[fuzz_u16(in) % size] ^= 1 + fuzz_byte(in) % 255;
	call_pdtp_receive(s->pdtp, packet, size, s->now);
}

static void outgoing(struct session *s, struct fuzz_input *in)
{
	static const char *const services[] = { "PLANET", "LINE", "" };
	guint what = fuzz_byte(in);
	guint64 stream = fuzz_byte(in) % 16;
	const guint8 *bytes;
	gsize n;

	switch (what & 3) {
	case 0:
		call_pdtp_stream_open(s->pdtp, stream,
				      services[(what >> 2) % 3], fuzz_byte(in));
		break;
	case 1:
		n = fuzz_chunk(in, &bytes);
		call_pdtp_stream_send(s->pdtp, stream, bytes, n, fuzz_u16(in));
		break;
	case 2:
		n = fuzz_chunk(in, &bytes);
		call_pdtp_stream_write(s->pdtp, stream, bytes, n);
		break;
	default:
		call_pdtp_stream_close(s->pdtp, stream);
		break;
	}
	call_pdtp_stream_backlog(s->pdtp, stream);
	call_pdtp_stream_paused(s->pdtp, stream);
	call_pdtp_stream_waiting(s->pdtp, stream);
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
	struct session s = { 0 };
	struct call_pdtp_share_rx stats;
	guint config = fuzz_byte(&in);

	if (config & CONFIG_STATS)
		g_setenv("LINE_GROUP_CALL_STATS", "1", TRUE);
	else
		g_unsetenv("LINE_GROUP_CALL_STATS");
	s.pdtp = config & CONFIG_GROUP ?
			 call_pdtp_open_group(local_key, relay_key, DATA_PT,
					      LOCAL_SSRC, RELAY_SSRC, NULL) :
			 call_pdtp_open(local_key, relay_key, DATA_PT,
					LOCAL_SSRC, RELAY_SSRC, NULL);
	g_assert_nonnull(s.pdtp);
	g_assert_true(
		call_srtp_open(&s.relay, relay_key, ssrc_specific, RELAY_SSRC));
	g_assert_true(call_srtp_open(&s.relay_rx, local_key, ssrc_specific,
				     LOCAL_SSRC));
	s.members = (config >> CONFIG_MEMBERS_SHIFT) & 3;
	s.now = g_get_monotonic_time();
	call_pdtp_set_trace(s.pdtp, !!(config & CONFIG_TRACE));
	call_pdtp_set_handlers(s.pdtp,
			       config & CONFIG_HANDLERS ? on_data : NULL,
			       config & CONFIG_HANDLERS ? on_event : NULL, &s);
	if (s.members)
		call_pdtp_set_group_members(s.pdtp, member);

	while (in.n) {
		guint op = fuzz_byte(&in);

		s.now += (op >> 4) * 1000;
		switch (op & 7) {
		case OP_RECEIVE:
		case OP_RECEIVE_2:
		case OP_RECEIVE_3:
			receive(&s, &in, op & 8);
			break;
		case OP_POLL:
			s.drop = op & 8;
			call_pdtp_poll(s.pdtp, s.now, sink, &s);
			break;
		case OP_WAIT:
			s.now += fuzz_u16(&in) * 1000;
			break;
		case OP_WATCH:
			call_pdtp_watch_presentation(s.pdtp, op & 8);
			break;
		case OP_MEMBERS:
			s.members = (s.members + 1) % 4;
			call_pdtp_group_members_changed(s.pdtp);
			break;
		default:
			outgoing(&s, &in);
			break;
		}
	}
	call_pdtp_share_rx_stats(s.pdtp, &stats);
	call_pdtp_group_setup_acknowledged(s.pdtp);
	call_pdtp_report(s.pdtp);
	call_pdtp_close(s.pdtp);
	srtp_dealloc(s.relay_rx);
	srtp_dealloc(s.relay);
	return 0;
}
