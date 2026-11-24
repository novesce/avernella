// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Element formats follow the shipped libandromeda.so 26.16.0 encoders:
 * vns_pdtp_packet_build at 0x826604, the parser at 0x823fec, ACK at
 * 0x827378, STRM_OPEN at 0x8289a4, STRM_DATA at 0x82a030 and window
 * updates at 0x827b58. See analysis/video-call-wire.md.
 */
#include "call_pdtp.h"
#include "call_srtp.h"
#include <openssl/rand.h>
#include <string.h>

enum {
	RTP_HEADER = 12,
	GROUP_RTP_HEADER = RTP_HEADER + 4,
	GROUP_RTP_PROFILE = 0x0240,
	MAX_DATAGRAM = 4096,
	/* kPdtpMTU: the largest PDTP payload the engine builds. */
	PDTP_MTU = 900,
	/* kPdtpDefaultMaxReceivableBytes before a window update arrives. */
	DEFAULT_WINDOW = 327680,
	RECEIVE_WINDOW = 16 << 20,
	MAX_STREAMS = 8,
	MAX_SERVICE = 64,
	ACK_HISTORY = 256,
	MAX_SEGMENTS = 4096,
	MAX_INFLIGHT = 2048,
	MAX_BUFFERED = 64 << 20,
	GROUP_MAX_ACK_DELAY_MS = 200,
	GROUP_SETUP_ATTEMPTS = 6,
};

#define STREAM_MASK 0x303eu

struct rx_segment {
	guint64 offset, time;
	guint flags;
	GBytes *bytes;
};

struct rx_stream {
	gboolean used;
	guint64 id;
	char service[MAX_SERVICE];
	guint64 delivered, window_sent;
	gboolean window_due;
	GArray *segments; /* struct rx_segment, sorted by offset */
};

struct tx_stream {
	gboolean used, open_acked, close_requested;
	guint64 id;
	guint flags;
	char service[MAX_SERVICE];
	GByteArray *buf; /* unacknowledged and unsent bytes */
	guint64 buf_offset; /* stream offset of buf->data[0] */
	guint64 next; /* next never-sent offset */
	guint64 peer_window;
	GArray *acked; /* sorted, merged [start, end) pairs above buf_offset */
	GArray *lost; /* [start, end) pairs to resend */
	GArray *messages; /* struct tx_message, oldest first */
	gboolean open_pending, close_pending;
	/* The receiver asked us to pause; reply_ctrl answers it once. */
	gboolean paused;
	guint reply_ctrl;
	guint64 reply_offset;
};

/* One application message: a chunk never spans two of them. */
struct tx_message {
	guint64 start, end, time;
};

struct sent_item {
	guint8 kind; /* PDTP_STREAM_DATA, _OPEN or _CLOSE */
	guint8 slot;
	guint64 start, end;
};

struct sent_packet {
	guint64 pn;
	gint64 sent;
	guint count;
	struct sent_item item[4];
};

struct call_pdtp {
	srtp_t tx, rx;
	guint8 pt;
	guint32 tx_ssrc, rx_ssrc;
	gboolean group;
	gboolean trace;
	call_pdtp_stream_fn on_data;
	call_pdtp_event_fn on_event;
	gpointer user;

	/* Receive state. */
	guint64 largest;
	gboolean have_largest;
	guint8 seen[ACK_HISTORY / 8]; /* bit i: largest - i was received */
	gint64 largest_at, ack_due;
	guint ack_pending;
	guint ack_delay_ms;
	struct rx_stream rx_streams[MAX_STREAMS];

	/* Send state. */
	guint64 next_pn;
	guint32 ts_base;
	gint64 epoch;
	struct tx_stream tx_streams[MAX_STREAMS];
	GArray *inflight; /* struct sent_packet */
	gint64 srtt, rto;
	struct {
		guint64 pn[GROUP_SETUP_ATTEMPTS];
		gint64 due;
		guint attempts, sent;
		gboolean acknowledged;
	} group_setup;
	gdouble tokens;
	gint64 tokens_at;
	guint64 send_bps;
	guint traced;
	guint64 sent_packets, sent_bytes, acked_packets, lost_packets;
	/* Last time an ACK covered new data, or the first send after idle. */
	gint64 last_progress, last_probe;
	/* No ACK since a timeout: send one probe per RTO until one arrives. */
	gboolean probing;
};

static guint16 read16(const guint8 *p)
{
	return (guint16)(p[0] << 8 | p[1]);
}

static guint32 read32(const guint8 *p)
{
	return (guint32)read16(p) << 16 | read16(p + 2);
}

static void write16(guint8 *p, guint16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static void write32(guint8 *p, guint32 v)
{
	write16(p, v >> 16);
	write16(p + 2, v);
}

gsize call_pdtp_varint(guint8 *out, guint64 v)
{
	if (v < 64) {
		out[0] = v;
		return 1;
	}
	if (v < 16384) {
		out[0] = 0x40 | v >> 8;
		out[1] = v;
		return 2;
	}
	if (v < (1u << 30)) {
		write32(out, (guint32)v | 0x80000000u);
		return 4;
	}
	v &= G_GUINT64_CONSTANT(0x3fffffffffffffff);
	write32(out, (guint32)(v >> 32) | 0xc0000000u);
	write32(out + 4, (guint32)v);
	return 8;
}

static gsize varint_size(guint64 v)
{
	return v < 64 ? 1 : v < 16384 ? 2 : v < (1u << 30) ? 4 : 8;
}

gboolean call_pdtp_read_varint(const guint8 **p, const guint8 *end,
			       guint64 *value)
{
	if (*p >= end)
		return FALSE;
	gsize n = (gsize)1 << (**p >> 6);

	if ((gsize)(end - *p) < n)
		return FALSE;
	guint64 v = **p & 0x3f;

	for (gsize i = 1; i < n; i++)
		v = v << 8 | (*p)[i];
	*p += n;
	*value = v;
	return TRUE;
}

static void trace_bytes(const char *label, const guint8 *p, gsize n)
{
	g_autoptr(GString) hex = g_string_new(NULL);

	for (gsize i = 0; i < n && i < 96; i++)
		g_string_append_printf(hex, "%02x", p[i]);
	g_printerr("line-call pdtp %s%s%s\n", label, n > 96 ? " (cut) " : " ",
		   hex->str);
}

static struct rx_stream *rx_stream(struct call_pdtp *d, guint64 id,
				   const char *service, gboolean create)
{
	struct rx_stream *free_slot = NULL;

	for (guint i = 0; i < MAX_STREAMS; i++) {
		struct rx_stream *s = &d->rx_streams[i];

		if (s->used && s->id == id)
			return s;
		if (!s->used && !free_slot)
			free_slot = s;
	}
	if (!create || !free_slot || d->group)
		return NULL;
	*free_slot = (struct rx_stream){ .used = TRUE, .id = id };
	g_strlcpy(free_slot->service, service, sizeof(free_slot->service));
	free_slot->segments =
		g_array_new(FALSE, FALSE, sizeof(struct rx_segment));
	free_slot->window_sent = DEFAULT_WINDOW;
	free_slot->window_due = TRUE;
	return free_slot;
}

static void rx_stream_clear(struct rx_stream *s)
{
	if (!s->used)
		return;
	for (guint i = 0; i < s->segments->len; i++)
		g_bytes_unref(
			g_array_index(s->segments, struct rx_segment, i).bytes);
	g_array_free(s->segments, TRUE);
	*s = (struct rx_stream){ 0 };
}

static struct tx_stream *tx_stream(struct call_pdtp *d, guint64 id)
{
	for (guint i = 0; i < MAX_STREAMS; i++)
		if (d->tx_streams[i].used && d->tx_streams[i].id == id)
			return &d->tx_streams[i];
	return NULL;
}

static void tx_stream_clear(struct tx_stream *s)
{
	if (!s->used)
		return;
	g_byte_array_free(s->buf, TRUE);
	g_array_free(s->acked, TRUE);
	g_array_free(s->lost, TRUE);
	g_array_free(s->messages, TRUE);
	*s = (struct tx_stream){ 0 };
}

void call_pdtp_close(struct call_pdtp *d)
{
	if (!d)
		return;
	for (guint i = 0; i < MAX_STREAMS; i++) {
		rx_stream_clear(&d->rx_streams[i]);
		tx_stream_clear(&d->tx_streams[i]);
	}
	if (d->inflight)
		g_array_free(d->inflight, TRUE);
	if (d->tx)
		srtp_dealloc(d->tx);
	if (d->rx)
		srtp_dealloc(d->rx);
	g_free(d);
}

static struct call_pdtp *pdtp_open(const guint8 tx[30], const guint8 rx[30],
				   guint8 pt, guint32 tx_ssrc, guint32 rx_ssrc,
				   gboolean group, GError **e)
{
	struct call_pdtp *d = g_new0(struct call_pdtp, 1);
	guint16 first;

	if (!tx || !rx || pt < 96 || pt > 127 ||
	    !call_srtp_open(&d->tx, tx, ssrc_specific, tx_ssrc) ||
	    !call_srtp_open(&d->rx, rx, ssrc_specific, rx_ssrc) ||
	    RAND_bytes((guint8 *)&first, sizeof(first)) != 1 ||
	    RAND_bytes((guint8 *)&d->ts_base, sizeof(d->ts_base)) != 1) {
		g_set_error_literal(
			e, LINE_ERROR, 1,
			"Could not initialize the call data channel.");
		call_pdtp_close(d);
		return NULL;
	}
	d->pt = pt;
	d->tx_ssrc = tx_ssrc;
	d->rx_ssrc = rx_ssrc;
	d->group = group;
	/* The official engine starts its packet numbers at 1. */
	d->next_pn = 1;
	(void)first;
	d->inflight = g_array_new(FALSE, FALSE, sizeof(struct sent_packet));
	d->rto = 300000;
	d->ack_delay_ms = 25;
	d->send_bps = 12000000;
	d->epoch = -1;
	return d;
}

struct call_pdtp *call_pdtp_open(const guint8 tx[30], const guint8 rx[30],
				 guint8 pt, guint32 tx_ssrc, guint32 rx_ssrc,
				 GError **e)
{
	return pdtp_open(tx, rx, pt, tx_ssrc, rx_ssrc, FALSE, e);
}

struct call_pdtp *call_pdtp_open_group(const guint8 tx[30], const guint8 rx[30],
				       guint8 pt, guint32 tx_ssrc,
				       guint32 rx_ssrc, GError **e)
{
	return pdtp_open(tx, rx, pt, tx_ssrc, rx_ssrc, TRUE, e);
}

gboolean call_pdtp_group_setup_acknowledged(const struct call_pdtp *d)
{
	return d && d->group_setup.acknowledged;
}

void call_pdtp_set_trace(struct call_pdtp *d, gboolean trace)
{
	if (d)
		d->trace = trace;
}

void call_pdtp_set_handlers(struct call_pdtp *d, call_pdtp_stream_fn data,
			    call_pdtp_event_fn event, gpointer user)
{
	if (!d)
		return;
	d->on_data = data;
	d->on_event = event;
	d->user = user;
}

gboolean call_pdtp_matches(const struct call_pdtp *d, const guint8 *p, gsize n)
{
	return d && n > RTP_HEADER && p[0] >> 6 == 2 && (p[1] & 127) == d->pt &&
	       read32(p + 8) == d->rx_ssrc;
}

/* ---- Receiving ---- */

static void note_packet(struct call_pdtp *d, guint64 pn, gint64 now)
{
	if (!d->have_largest || pn > d->largest) {
		guint64 shift = d->have_largest ? pn - d->largest : ACK_HISTORY;
		guint8 next[sizeof(d->seen)] = { 0 };

		if (shift < ACK_HISTORY) {
			guint bytes = shift / 8, bits = shift % 8;
			guint carry = 0;

			for (guint i = 0; i + bytes < sizeof(next); i++) {
				guint shifted = (guint)d->seen[i] << bits;

				next[i + bytes] = shifted | carry;
				carry = shifted >> 8;
			}
		}
		memcpy(d->seen, next, sizeof(next));
		d->seen[0] |= 1;
		d->largest = pn;
		d->have_largest = TRUE;
		d->largest_at = now;
	} else if (d->largest - pn < ACK_HISTORY) {
		guint64 i = d->largest - pn;

		d->seen[i / 8] |= 1u << (i % 8);
	}
}

static void consume_segment(struct call_pdtp *d, struct rx_stream *s,
			    guint64 offset, guint64 time, guint flags,
			    const guint8 *p, gsize size)
{
	if (offset + size <= s->delivered)
		return;
	gsize skip = s->delivered - offset;

	if (skip)
		flags &= ~PDTP_MESSAGE_START;
	if (d->on_data)
		d->on_data(s->id, s->service, s->delivered, time, flags,
			   p + skip, size - skip, d->user);
	s->delivered = offset + size;
}

static void deliver(struct call_pdtp *d, struct rx_stream *s)
{
	while (s->segments->len) {
		struct rx_segment *seg =
			&g_array_index(s->segments, struct rx_segment, 0);
		gsize size;
		const guint8 *p = g_bytes_get_data(seg->bytes, &size);

		if (seg->offset > s->delivered)
			break;
		consume_segment(d, s, seg->offset, seg->time, seg->flags, p,
				size);
		g_bytes_unref(seg->bytes);
		g_array_remove_index(s->segments, 0);
	}
	if (s->delivered + RECEIVE_WINDOW / 2 > s->window_sent)
		s->window_due = TRUE;
}

static void stream_data(struct call_pdtp *d, struct rx_stream *s,
			guint64 offset, guint64 time, guint flags,
			const guint8 *p, gsize n)
{
	if (!n || offset + n <= s->delivered ||
	    offset + n > s->delivered + RECEIVE_WINDOW + DEFAULT_WINDOW)
		return;
	/* In-order data is borrowed from the authenticated packet. */
	if (offset <= s->delivered) {
		consume_segment(d, s, offset, time, flags, p, n);
		deliver(d, s);
		return;
	}
	if (s->segments->len >= MAX_SEGMENTS)
		return;
	guint i = 0;

	while (i < s->segments->len &&
	       g_array_index(s->segments, struct rx_segment, i).offset < offset)
		i++;
	if (i < s->segments->len &&
	    g_array_index(s->segments, struct rx_segment, i).offset == offset)
		return; /* Duplicate retransmission. */
	struct rx_segment seg = { offset, time, flags, g_bytes_new(p, n) };

	g_array_insert_val(s->segments, i, seg);
	deliver(d, s);
}

static void acked_range(struct tx_stream *s, guint64 start, guint64 end);

static void peer_acked(struct call_pdtp *d, guint64 pn, gint64 now)
{
	for (guint i = 0; i < d->inflight->len; i++) {
		struct sent_packet *sp =
			&g_array_index(d->inflight, struct sent_packet, i);

		if (sp->pn != pn)
			continue;
		gint64 rtt = now - sp->sent;

		d->srtt = d->srtt ? (7 * d->srtt + rtt) / 8 : rtt;
		d->rto = CLAMP(2 * d->srtt + 20000, 80000, 2000000);
		for (guint j = 0; j < sp->count; j++) {
			struct sent_item *it = &sp->item[j];
			struct tx_stream *s = &d->tx_streams[it->slot];

			if (!s->used)
				continue;
			if (it->kind == PDTP_STREAM_OPEN)
				s->open_acked = TRUE;
			else if (it->kind == PDTP_STREAM_DATA)
				acked_range(s, it->start, it->end);
		}
		g_array_remove_index(d->inflight, i);
		d->acked_packets++;
		d->last_progress = now;
		d->probing = FALSE;
		return;
	}
}

static gboolean setup_ack_range(const struct call_pdtp *d, guint64 lo,
				guint64 hi)
{
	for (guint i = 0; i < d->group_setup.sent; i++)
		if (d->group_setup.pn[i] >= lo && d->group_setup.pn[i] <= hi)
			return TRUE;
	return FALSE;
}

static gboolean parse_ack(struct call_pdtp *d, const guint8 *p,
			  const guint8 *end, gint64 now, gboolean *setup_acked)
{
	guint64 largest, reason, late = 0, delay, pairs;

	if (!call_pdtp_read_varint(&p, end, &largest) ||
	    !call_pdtp_read_varint(&p, end, &reason) ||
	    (reason == 5 && !call_pdtp_read_varint(&p, end, &late)) ||
	    !call_pdtp_read_varint(&p, end, &delay) ||
	    !call_pdtp_read_varint(&p, end, &pairs) || pairs > 256)
		return FALSE;
	guint64 pn = largest;

	if (d->group)
		*setup_acked = setup_ack_range(d, pn, pn);
	else
		peer_acked(d, pn, now);
	for (guint64 i = 0; i < pairs; i++) {
		guint64 gap, count;

		if (!call_pdtp_read_varint(&p, end, &gap) ||
		    !call_pdtp_read_varint(&p, end, &count) || gap + count > pn)
			return FALSE;
		pn -= gap;
		if (d->group) {
			if (count)
				*setup_acked |=
					setup_ack_range(d, pn - count, pn - 1);
			pn -= count;
		} else
			for (guint64 j = 0; j < count; j++)
				peer_acked(d, --pn, now);
	}
	if (d->trace && d->traced < 4000)
		g_printerr("line-call pdtp ack largest=%" G_GUINT64_FORMAT
			   " reason=%" G_GUINT64_FORMAT
			   " ranges=%" G_GUINT64_FORMAT " inflight=%u\n",
			   largest, reason, pairs, d->inflight->len);
	return !d->group || p == end;
}

/* Parses one element; returns FALSE if the packet is malformed. */
static gboolean element(struct call_pdtp *d, guint type, const char *service,
			const guint8 **pp, const guint8 *end, gint64 now,
			gboolean *setup_acked)
{
	const guint8 *p = *pp;
	guint64 stream = 0, a = 0, b = 0;

	switch (type) {
	case PDTP_STREAM_DATA: {
		guint64 offset, length;

		if (!call_pdtp_read_varint(&p, end, &stream) || p >= end)
			return FALSE;
		/*
		 * Flag bytes chain through bit 7. In the first byte, 0x40,
		 * 0x20 and 0x01 carry a length-prefixed varint (0x01 is the
		 * frame's source time) and 0x10..0x02 carry one varint each,
		 * as vns_pdtp_pkt_strmdata decodes them. Later bytes carry
		 * length-prefixed values that are skipped.
		 */
		guint64 ext[7] = { 0 };
		guint flags = 0;

		for (guint byte = 0;; byte++) {
			if (p >= end || byte > 8)
				return FALSE;
			guint f = *p++;

			if (!byte)
				flags = f;
			for (int bit = 6; bit >= 0; bit--) {
				guint64 len, value;

				if (!(f & (1u << bit)))
					continue;
				if (!call_pdtp_read_varint(&p, end, &len))
					return FALSE;
				if (byte || !(0x61 & (1u << bit))) {
					if (!byte)
						ext[bit] = len;
					else if ((guint64)(end - p) < len)
						return FALSE;
					else
						p += len;
					continue;
				}
				const guint8 *value_end = p + len;

				if ((guint64)(end - p) < len ||
				    !call_pdtp_read_varint(&p, value_end,
							   &value))
					return FALSE;
				ext[bit] = value;
				p = value_end;
			}
			if (!(f & 0x80))
				break;
		}
		if (!call_pdtp_read_varint(&p, end, &offset) ||
		    !call_pdtp_read_varint(&p, end, &length) ||
		    (guint64)(end - p) < length)
			return FALSE;
		struct rx_stream *s = rx_stream(d, stream, service, TRUE);

		if (d->trace && d->traced++ < 4000)
			g_printerr(
				"line-call pdtp data stream=%" G_GUINT64_FORMAT
				" service=%s flags=%02x offset=%" G_GUINT64_FORMAT
				" length=%" G_GUINT64_FORMAT
				" time=%" G_GUINT64_FORMAT
				" x40=%" G_GUINT64_FORMAT
				" x20=%" G_GUINT64_FORMAT
				" x10=%" G_GUINT64_FORMAT
				" x08=%" G_GUINT64_FORMAT
				" x04=%" G_GUINT64_FORMAT
				" x02=%" G_GUINT64_FORMAT "\n",
				stream, service, flags, offset, length, ext[0],
				ext[6], ext[5], ext[4], ext[3], ext[2], ext[1]);
		if (s)
			stream_data(d, s, offset, ext[0], flags, p, length);
		p += length;
		break;
	}
	case PDTP_STREAM_CTRL: {
		/*
		 * STRM_CTRL: stream, direction (2 from a receiver, 1 from a
		 * sender), offset, control. A receiver pauses and resumes a
		 * stream with REQ_PAUSE (6) and REQ_RESUME (7); the sender
		 * answers PAUSE (1) or RESUME (2).
		 */
		guint64 ctrl;

		if (!call_pdtp_read_varint(&p, end, &stream) || p >= end)
			return FALSE;
		guint dir = *p++;

		if (!call_pdtp_read_varint(&p, end, &a) ||
		    !call_pdtp_read_varint(&p, end, &ctrl))
			return FALSE;
		if (d->trace)
			g_printerr(
				"line-call pdtp control stream=%" G_GUINT64_FORMAT
				" service=%s direction=%u offset=%" G_GUINT64_FORMAT
				" control=%u\n",
				stream, service, dir, a, (guint)ctrl);
		struct tx_stream *s = tx_stream(d, stream);

		if (s && dir == 2 && (ctrl == 6 || ctrl == 7)) {
			s->paused = ctrl == 6;
			s->reply_ctrl = ctrl == 6 ? 1 : 2;
			s->reply_offset = s->next;
			if (d->on_event)
				d->on_event(stream, service,
					    ctrl == 6 ? PDTP_EVENT_PAUSE :
							PDTP_EVENT_RESUME,
					    d->user);
		}
		/* Extra fields of the _USER controls end with the group. */
		p = end;
		break;
	}
	case PDTP_STREAM_OPEN:
		if (!call_pdtp_read_varint(&p, end, &stream) ||
		    !call_pdtp_read_varint(&p, end, &a) ||
		    !call_pdtp_read_varint(&p, end, &b))
			return FALSE;
		if (d->trace && d->traced++ < 4000)
			g_printerr(
				"line-call pdtp open stream=%" G_GUINT64_FORMAT
				" service=%s flags=%" G_GUINT64_FORMAT
				" start=%" G_GUINT64_FORMAT "\n",
				stream, service, a, b);
		{
			struct rx_stream *s =
				rx_stream(d, stream, service, TRUE);

			if (s && !s->delivered && !s->segments->len)
				s->delivered = b;
		}
		if (d->on_event)
			d->on_event(stream, service, type, d->user);
		break;
	case PDTP_WINDOW_UPDATE:
		if (!call_pdtp_read_varint(&p, end, &stream) ||
		    !call_pdtp_read_varint(&p, end, &a))
			return FALSE;
		{
			struct tx_stream *s = tx_stream(d, stream);

			if (s && a > s->peer_window)
				s->peer_window = a;
		}
		if (d->trace && d->traced++ < 4000)
			g_printerr(
				"line-call pdtp window stream=%" G_GUINT64_FORMAT
				" max=%" G_GUINT64_FORMAT "\n",
				stream, a);
		break;
	case PDTP_REQUEST_WINDOW:
		if (!call_pdtp_read_varint(&p, end, &stream))
			return FALSE;
		{
			struct rx_stream *s =
				rx_stream(d, stream, service, TRUE);

			if (s)
				s->window_due = TRUE;
		}
		break;
	case PDTP_SET_MAX_ACK_DELAY:
		if (!call_pdtp_read_varint(&p, end, &a))
			return FALSE;
		/* Group D uses a 1 kHz clock. Keep our normal 25 ms target
		 * unless the peer asks for a shorter maximum.
		 */
		d->ack_delay_ms = d->group ? MIN(a, 25) : CLAMP(a / 4, 5, 50);
		break;
	case PDTP_INCLUDE_RETRANS:
		if (!call_pdtp_read_varint(&p, end, &a))
			return FALSE;
		break;
	case PDTP_ACK:
		if (!parse_ack(d, p, end, now, setup_acked))
			return FALSE;
		p = end;
		break;
	default:
		/* Closes and resets: skipped by the group length. */
		if (d->trace) {
			const guint8 *q = *pp;
			guint64 v1 = 0, v2 = 0;

			call_pdtp_read_varint(&q, end, &stream);
			call_pdtp_read_varint(&q, end, &v1);
			call_pdtp_read_varint(&q, end, &v2);
			g_printerr(
				"line-call pdtp element=%u stream=%" G_GUINT64_FORMAT
				" service=%s values=%" G_GUINT64_FORMAT
				",%" G_GUINT64_FORMAT "\n",
				type, stream, service, v1, v2);
		}
		p = end;
		if (d->on_event && type <= PDTP_STREAM_RESET) {
			const guint8 *q = *pp;

			if (call_pdtp_read_varint(&q, end, &stream))
				d->on_event(stream, service, type, d->user);
		}
		break;
	}
	*pp = p;
	return TRUE;
}

static gboolean parse_packet(struct call_pdtp *d, const guint8 *p, gsize n,
			     guint16 seq, gint64 now)
{
	const guint8 *end = p + n;
	guint64 pn;

	if (!n)
		return FALSE;
	/* The packet number's last two bytes travel as the RTP sequence. */
	gsize len = (gsize)1 << (*p >> 6);

	if (len < 4 || n < len - 2)
		return FALSE;
	guint8 number[8];

	memcpy(number, p, len - 2);
	write16(number + len - 2, seq);
	const guint8 *q = number;

	call_pdtp_read_varint(&q, number + len, &pn);
	p += len - 2;
	const guint8 *nul = memchr(p, 0, end - p);

	if (!nul || nul - p >= MAX_SERVICE || end - nul < 3)
		return FALSE;
	char service[MAX_SERVICE];

	memcpy(service, p, nul - p);
	service[nul - p] = 0;
	p = nul + 1;
	guint mask = read16(p);

	if (d->group && (mask & 0x8000))
		return FALSE;
	p += 2;
	gboolean eliciting = FALSE, setup_acked = FALSE;
	gboolean presentation = (mask & 1) && !strcmp(service, "PLANET");

	for (guint type = 15; type >= 1; type--) {
		guint64 size, count = 1;

		if (!(mask & (1u << (type - 1))))
			continue;
		if (!call_pdtp_read_varint(&p, end, &size))
			return FALSE;
		gboolean counted = STREAM_MASK & (1u << type);

		/*
		 * The engine adds the length varint's own size to a counted
		 * group, so a group of 64 or more bytes declares one byte more
		 * than it holds. Its parser walks counted groups by count.
		 */
		if ((guint64)(end - p) < size) {
			if (!counted || size - (guint64)(end - p) > 8)
				return FALSE;
			size = end - p;
		}
		const guint8 *group_end = p + size;

		if (counted) {
			if (!call_pdtp_read_varint(&p, group_end, &count))
				return FALSE;
		}
		if (type != PDTP_ACK && type != PDTP_WINDOW_UPDATE)
			eliciting = TRUE;
		for (guint64 i = 0; i < count && p < group_end; i++)
			if (!element(d, type, service, &p, group_end, now,
				     &setup_acked))
				return FALSE;
		/* Counted groups end after their last element, see above. */
		if (!counted)
			p = group_end;
	}
	if (d->group && p != end)
		return FALSE;
	if (setup_acked) {
		d->group_setup.acknowledged = TRUE;
		d->group_setup.due = G_MAXINT64;
	}
	note_packet(d, pn, now);
	if (eliciting) {
		if (!d->ack_pending++)
			d->ack_due = now + d->ack_delay_ms * 1000;
		if (presentation)
			d->ack_due = MIN(d->ack_due, now + 12000);
		if (d->ack_pending >= 8)
			d->ack_due = now;
	}
	return TRUE;
}

gboolean call_pdtp_receive(struct call_pdtp *d, const guint8 *packet,
			   gsize size, gint64 now)
{
	guint8 buf[MAX_DATAGRAM];
	int plain = size;
	gsize pos;

	if (!call_pdtp_matches(d, packet, size) || size > sizeof(buf))
		return FALSE;
	memcpy(buf, packet, size);
	if (srtp_unprotect(d->rx, buf, &plain) != srtp_err_status_ok)
		return FALSE;
	pos = RTP_HEADER + 4u * (buf[0] & 15);
	if (buf[0] & 0x10) {
		if ((gsize)plain < pos + 4)
			return FALSE;
		pos += 4u + 4u * read16(buf + pos + 2);
	}
	if ((gsize)plain <= pos)
		return FALSE;
	if (d->trace && d->traced++ < 400)
		trace_bytes("rx", buf + pos, plain - pos);
	if (parse_packet(d, buf + pos, plain - pos, read16(buf + 2), now))
		return TRUE;
	if (d->trace)
		trace_bytes("rx malformed", buf + pos, plain - pos);
	return !d->group;
}

/* ---- Sending ---- */

struct builder {
	guint8 group[16][PDTP_MTU];
	gsize len[16];
	guint count[16];
	char service[MAX_SERVICE];
	gsize total;
};

static gsize header_size(const struct builder *b)
{
	return 2 + strlen(b->service) + 1 + 2;
}

static gsize room(const struct builder *b)
{
	gsize used = b->total + header_size(b) + 24;

	return used < PDTP_MTU ? PDTP_MTU - used : 0;
}

static void add(struct builder *b, guint type, const guint8 *p, gsize n)
{
	memcpy(b->group[type] + b->len[type], p, n);
	b->len[type] += n;
	b->count[type]++;
	b->total += n + 6;
}

static gboolean emit(struct call_pdtp *d, struct builder *b, gint64 now,
		     call_pdtp_send_fn send, gpointer data,
		     struct sent_packet *record)
{
	guint8 packet[GROUP_RTP_HEADER + PDTP_MTU + 64 + SRTP_MAX_TRAILER_LEN];
	gsize rtp_header = d->group ? GROUP_RTP_HEADER : RTP_HEADER;
	gsize pos = rtp_header;
	guint mask = 0;
	guint64 pn = d->next_pn++;
	guint8 number[8];
	gsize nlen = varint_size(pn) < 4 ? 4 : varint_size(pn);

	if (nlen == 4)
		write32(number, (guint32)pn | 0x80000000u);
	else
		call_pdtp_varint(number, pn);
	memcpy(packet + pos, number, nlen - 2);
	pos += nlen - 2;
	gsize slen = strlen(b->service);

	memcpy(packet + pos, b->service, slen + 1);
	pos += slen + 1;
	for (guint type = 1; type <= 15; type++)
		if (b->count[type])
			mask |= 1u << (type - 1);
	write16(packet + pos, mask);
	pos += 2;
	for (guint type = 15; type >= 1; type--) {
		if (!b->count[type])
			continue;
		gsize body = b->len[type];
		gboolean counted = STREAM_MASK & (1u << type);
		/* The engine's group length also covers its varint size. */
		gsize len = body + (counted ? varint_size(body) : 0);

		pos += call_pdtp_varint(packet + pos, len);
		if (counted)
			pos += call_pdtp_varint(packet + pos, b->count[type]);
		memcpy(packet + pos, b->group[type], body);
		pos += body;
	}
	if (d->epoch < 0)
		d->epoch = now;
	packet[0] = d->group ? 0x90 : 0x80;
	packet[1] = 0x80 | d->pt;
	write16(packet + 2, (guint16)pn);
	write32(packet + 4, d->ts_base + (guint32)((now - d->epoch) / 1000));
	write32(packet + 8, d->tx_ssrc);
	if (d->group) {
		write16(packet + RTP_HEADER, GROUP_RTP_PROFILE);
		write16(packet + RTP_HEADER + 2, 0);
	}
	if (d->trace && d->traced++ < 4000)
		trace_bytes("tx", packet + rtp_header, pos - rtp_header);
	int n = pos;

	if (srtp_protect(d->tx, packet, &n) != srtp_err_status_ok ||
	    !send(packet, n, data))
		return FALSE;
	d->sent_packets++;
	d->sent_bytes += n;
	if (record && record->count) {
		record->pn = pn;
		record->sent = now;
		if (!d->inflight->len)
			d->last_progress = now;
		if (d->inflight->len < MAX_INFLIGHT)
			g_array_append_val(d->inflight, *record);
	}
	memset(b->count, 0, sizeof(b->count));
	memset(b->len, 0, sizeof(b->len));
	b->total = 0;
	return TRUE;
}

static void send_group_setup(struct call_pdtp *d, gint64 now,
			     call_pdtp_send_fn send, gpointer data)
{
	g_autofree struct builder *b = g_new0(struct builder, 1);
	guint8 value[8];
	gsize len = call_pdtp_varint(value, GROUP_MAX_ACK_DELAY_MS);
	guint64 pn = d->next_pn;

	add(b, PDTP_SET_MAX_ACK_DELAY, value, len);
	if (d->group_setup.sent) {
		/* Every retry identifies the first transmitted packet. */
		len = call_pdtp_varint(value, d->group_setup.pn[0]);
		add(b, PDTP_INCLUDE_RETRANS, value, len);
	}
	if (emit(d, b, now, send, data, NULL))
		d->group_setup.pn[d->group_setup.sent++] = pn;
	gint64 delay = MIN(d->rto << d->group_setup.attempts, 8000000);

	if (++d->group_setup.attempts == GROUP_SETUP_ATTEMPTS)
		d->group_setup.due = G_MAXINT64;
	else
		d->group_setup.due = now + delay;
}

static gboolean send_ack(struct call_pdtp *d, gint64 now,
			 call_pdtp_send_fn send, gpointer data)
{
	struct builder *b = g_new0(struct builder, 1);
	guint8 el[ACK_HISTORY * 2 + 32];
	gsize n = 0;
	guint64 gaps[ACK_HISTORY], counts[ACK_HISTORY];
	guint pairs = 0;

	/* (gap, count) ranges below the largest, as the shipped writer. */
	{
		guint i = 1, g = 0;

		while (i < ACK_HISTORY && i <= d->largest && pairs < 32) {
			while (i < ACK_HISTORY && i <= d->largest &&
			       !(d->seen[i / 8] & (1u << (i % 8)))) {
				g++;
				i++;
			}
			guint c = 0;

			while (i < ACK_HISTORY && i <= d->largest &&
			       (d->seen[i / 8] & (1u << (i % 8)))) {
				c++;
				i++;
			}
			if (!c)
				break;
			gaps[pairs] = g;
			counts[pairs] = c;
			pairs++;
			g = 0;
		}
	}
	gint64 delay = MAX(0, now - d->largest_at) / 1000;

	n += call_pdtp_varint(el + n, d->largest);
	n += call_pdtp_varint(el + n, 1); /* LONG_DELAY_SCHEDULED */
	n += call_pdtp_varint(el + n, delay);
	n += call_pdtp_varint(el + n, pairs);
	for (guint i = 0; i < pairs; i++) {
		n += call_pdtp_varint(el + n, gaps[i]);
		n += call_pdtp_varint(el + n, counts[i]);
	}
	add(b, PDTP_ACK, el, n);
	gboolean ok = emit(d, b, now, send, data, NULL);

	g_free(b);
	d->ack_pending = 0;
	d->ack_due = 0;
	return ok;
}

static gboolean send_windows(struct call_pdtp *d, gint64 now,
			     call_pdtp_send_fn send, gpointer data)
{
	for (guint i = 0; i < MAX_STREAMS; i++) {
		struct rx_stream *s = &d->rx_streams[i];

		if (!s->used || !s->window_due)
			continue;
		struct builder *b = g_new0(struct builder, 1);
		guint8 el[24];
		gsize n = 0;
		guint64 max = s->delivered + RECEIVE_WINDOW;

		g_strlcpy(b->service, s->service, sizeof(b->service));
		n += call_pdtp_varint(el + n, s->id);
		n += call_pdtp_varint(el + n, max);
		add(b, PDTP_WINDOW_UPDATE, el, n);
		gboolean ok = emit(d, b, now, send, data, NULL);

		g_free(b);
		if (!ok)
			return FALSE;
		s->window_sent = max;
		s->window_due = FALSE;
	}
	return TRUE;
}

/* ---- Outgoing streams ---- */

gboolean call_pdtp_stream_open(struct call_pdtp *d, guint64 id,
			       const char *service, guint flags)
{
	if (!d || d->group || tx_stream(d, id) ||
	    strlen(service) >= MAX_SERVICE)
		return FALSE;
	for (guint i = 0; i < MAX_STREAMS; i++) {
		struct tx_stream *s = &d->tx_streams[i];

		if (s->used)
			continue;
		*s = (struct tx_stream){ .used = TRUE,
					 .id = id,
					 .flags = flags };
		g_strlcpy(s->service, service, sizeof(s->service));
		s->buf = g_byte_array_new();
		s->acked = g_array_new(FALSE, FALSE, sizeof(guint64));
		s->lost = g_array_new(FALSE, FALSE, sizeof(guint64));
		s->messages =
			g_array_new(FALSE, FALSE, sizeof(struct tx_message));
		s->peer_window = DEFAULT_WINDOW;
		s->open_pending = TRUE;
		return TRUE;
	}
	return FALSE;
}

gboolean call_pdtp_stream_send(struct call_pdtp *d, guint64 id,
			       const guint8 *bytes, gsize size, guint64 time)
{
	struct tx_stream *s = d ? tx_stream(d, id) : NULL;

	if (!s || !size || s->close_requested ||
	    s->buf->len + size > MAX_BUFFERED)
		return FALSE;
	struct tx_message msg = { s->buf_offset + s->buf->len, 0, time };

	msg.end = msg.start + size;
	g_array_append_val(s->messages, msg);
	g_byte_array_append(s->buf, bytes, size);
	return TRUE;
}

gboolean call_pdtp_stream_write(struct call_pdtp *d, guint64 id,
				const guint8 *bytes, gsize size)
{
	return call_pdtp_stream_send(d, id, bytes, size, 0);
}

void call_pdtp_stream_close(struct call_pdtp *d, guint64 id)
{
	struct tx_stream *s = d ? tx_stream(d, id) : NULL;

	if (s)
		s->close_requested = s->close_pending = TRUE;
}

guint call_pdtp_stream_waiting(const struct call_pdtp *d, guint64 stream)
{
	for (guint i = 0; d && i < MAX_STREAMS; i++)
		if (d->rx_streams[i].used && d->rx_streams[i].id == stream)
			return d->rx_streams[i].segments->len;
	return 0;
}

gboolean call_pdtp_stream_paused(const struct call_pdtp *d, guint64 stream)
{
	for (guint i = 0; d && i < MAX_STREAMS; i++)
		if (d->tx_streams[i].used && d->tx_streams[i].id == stream)
			return d->tx_streams[i].paused;
	return FALSE;
}

gsize call_pdtp_stream_backlog(const struct call_pdtp *d, guint64 id)
{
	for (guint i = 0; d && i < MAX_STREAMS; i++)
		if (d->tx_streams[i].used && d->tx_streams[i].id == id)
			return d->tx_streams[i].buf->len;
	return 0;
}

static void range_add(GArray *ranges, guint64 start, guint64 end)
{
	guint i = 0;

	while (i < ranges->len && g_array_index(ranges, guint64, i + 1) < start)
		i += 2;
	while (i < ranges->len && g_array_index(ranges, guint64, i) <= end) {
		start = MIN(start, g_array_index(ranges, guint64, i));
		end = MAX(end, g_array_index(ranges, guint64, i + 1));
		g_array_remove_range(ranges, i, 2);
	}
	guint64 pair[2] = { start, end };

	g_array_insert_vals(ranges, i, pair, 2);
}

static void acked_range(struct tx_stream *s, guint64 start, guint64 end)
{
	range_add(s->acked, start, end);
	/* Forget a retransmission that is no longer needed. */
	for (guint i = 0; i < s->lost->len; i += 2) {
		guint64 *r = &g_array_index(s->lost, guint64, i);

		if (r[0] >= start && r[1] <= end) {
			g_array_remove_range(s->lost, i, 2);
			i -= 2;
		}
	}
	while (s->acked->len &&
	       g_array_index(s->acked, guint64, 0) <= s->buf_offset) {
		guint64 top = g_array_index(s->acked, guint64, 1);

		if (top > s->buf_offset) {
			gsize drop = MIN(top - s->buf_offset, s->buf->len);

			g_byte_array_remove_range(s->buf, 0, drop);
			s->buf_offset += drop;
			while (s->messages->len &&
			       g_array_index(s->messages, struct tx_message, 0)
					       .end <= s->buf_offset)
				g_array_remove_index(s->messages, 0);
		}
		g_array_remove_range(s->acked, 0, 2);
	}
}

/*
 * Retransmission after a timeout. While the peer acknowledges nothing at
 * all, resend only the oldest packet as a probe and back off, rather than
 * the whole window.
 */
static void detect_loss(struct call_pdtp *d, gint64 now)
{
	gboolean stalled;
	guint declared = 0;

	if (d->inflight->len && now - d->last_progress > d->rto)
		d->probing = TRUE;
	stalled = d->probing;
	if (stalled && now - d->last_probe < d->rto)
		return;

	for (guint i = 0; i < d->inflight->len; i++) {
		struct sent_packet *sp =
			&g_array_index(d->inflight, struct sent_packet, i);

		if (now - sp->sent < d->rto)
			continue;
		if (stalled && declared)
			break;
		declared++;
		for (guint j = 0; j < sp->count; j++) {
			struct sent_item *it = &sp->item[j];
			struct tx_stream *s = &d->tx_streams[it->slot];

			if (!s->used)
				continue;
			if (it->kind == PDTP_STREAM_OPEN && !s->open_acked)
				s->open_pending = TRUE;
			else if (it->kind == PDTP_STREAM_CLOSE)
				s->close_pending = TRUE;
			else if (it->kind == PDTP_STREAM_CTRL &&
				 !s->reply_ctrl &&
				 (it->start == 1) == s->paused) {
				s->reply_ctrl = it->start;
				s->reply_offset = it->end;
			} else if (it->kind == PDTP_STREAM_DATA &&
				   it->end > s->buf_offset)
				range_add(s->lost,
					  MAX(it->start, s->buf_offset),
					  it->end);
		}
		g_array_remove_index(d->inflight, i--);
		d->lost_packets++;
	}
	if (declared)
		d->rto = stalled ? MIN(d->rto * 2, 8000000) :
				   MIN(d->rto * 3 / 2, 2000000);
	if (stalled && declared)
		d->last_probe = now;
}

static const struct tx_message *message_at(const struct tx_stream *s,
					   guint64 offset)
{
	for (guint i = 0; i < s->messages->len; i++) {
		const struct tx_message *m =
			&g_array_index(s->messages, struct tx_message, i);

		if (offset >= m->start && offset < m->end)
			return m;
	}
	return NULL;
}

/* Never let a chunk cross into the next message. */
static guint64 clamp_to_message(const struct tx_stream *s, guint64 offset,
				guint64 len)
{
	const struct tx_message *m = message_at(s, offset);

	return m ? MIN(len, m->end - offset) : len;
}

/*
 * Mirrors the official sender: bits 0x10 and 0x02 with zero values, 0x08
 * on a message's first chunk, 0x04 on its last, and 0x01 with the frame's
 * source time.
 */
static gsize data_element(guint8 *el, struct tx_stream *s, guint64 offset,
			  gsize len)
{
	const struct tx_message *m = message_at(s, offset);
	gsize n = call_pdtp_varint(el, s->id);
	guint flags = 0x12;

	if (m && m->start == offset)
		flags |= PDTP_MESSAGE_START;
	if (m && m->end == offset + len)
		flags |= PDTP_MESSAGE_END;
	if (m && m->time)
		flags |= 0x01;
	el[n++] = flags;
	for (int bit = 4; bit >= 1; bit--)
		if (flags & (1u << bit))
			el[n++] = 0;
	if (flags & 0x01) {
		el[n++] = varint_size(m->time);
		n += call_pdtp_varint(el + n, m->time);
	}
	n += call_pdtp_varint(el + n, offset);
	n += call_pdtp_varint(el + n, len);
	memcpy(el + n, s->buf->data + (offset - s->buf_offset), len);
	return n + len;
}

static gboolean send_streams(struct call_pdtp *d, gint64 now,
			     call_pdtp_send_fn send, gpointer data)
{
	if (!d->tokens_at)
		d->tokens_at = now;
	d->tokens = MIN(d->tokens + (gdouble)(now - d->tokens_at) *
					    d->send_bps / 8e6,
			64.0 * PDTP_MTU);
	d->tokens_at = now;
	for (guint slot = 0; slot < MAX_STREAMS; slot++) {
		struct tx_stream *s = &d->tx_streams[slot];

		if (!s->used)
			continue;
		while (d->tokens > 0 && d->inflight->len < MAX_INFLIGHT) {
			struct builder *b = g_new0(struct builder, 1);
			struct sent_packet rec = { 0 };
			guint8 el[PDTP_MTU];
			gsize n = 0;

			g_strlcpy(b->service, s->service, sizeof(b->service));
			if (s->reply_ctrl) {
				n = call_pdtp_varint(el, s->id);
				el[n++] = 1; /* From the sender. */
				n += call_pdtp_varint(el + n, s->reply_offset);
				n += call_pdtp_varint(el + n, s->reply_ctrl);
				add(b, PDTP_STREAM_CTRL, el, n);
				rec.item[rec.count++] =
					(struct sent_item){ PDTP_STREAM_CTRL,
							    slot, s->reply_ctrl,
							    s->reply_offset };
				s->reply_ctrl = 0;
			}
			if (s->open_pending) {
				n = call_pdtp_varint(el, s->id);
				n += call_pdtp_varint(el + n, s->flags);
				n += call_pdtp_varint(el + n, 0);
				add(b, PDTP_STREAM_OPEN, el, n);
				rec.item[rec.count++] =
					(struct sent_item){ PDTP_STREAM_OPEN,
							    slot, 0, 0 };
				s->open_pending = FALSE;
			}
			guint64 start = 0, len = 0;

			if (s->lost->len) {
				start = g_array_index(s->lost, guint64, 0);
				guint64 stop =
					g_array_index(s->lost, guint64, 1);

				len = clamp_to_message(s, start,
						       MIN(stop - start,
							   room(b) - 32));
				if (start + len >= stop)
					g_array_remove_range(s->lost, 0, 2);
				else
					g_array_index(s->lost, guint64, 0) =
						start + len;
			} else if (!s->paused &&
				   s->next < s->buf_offset + s->buf->len &&
				   s->next < s->peer_window) {
				start = s->next;
				len = MIN(MIN(s->buf_offset + s->buf->len,
					      s->peer_window) -
						  start,
					  room(b) - 32);
				len = clamp_to_message(s, start, len);
				s->next += len;
			}
			if (len) {
				n = data_element(el, s, start, len);
				add(b, PDTP_STREAM_DATA, el, n);
				rec.item[rec.count++] =
					(struct sent_item){ PDTP_STREAM_DATA,
							    slot, start,
							    start + len };
			}
			if (!rec.count) {
				g_free(b);
				break;
			}
			gboolean ok = emit(d, b, now, send, data, &rec);

			g_free(b);
			if (!ok)
				return FALSE;
			d->tokens -= RTP_HEADER + PDTP_MTU;
		}
	}
	return TRUE;
}

gint64 call_pdtp_poll(struct call_pdtp *d, gint64 now, call_pdtp_send_fn send,
		      gpointer data)
{
	if (!d)
		return G_MAXINT64;
	if (d->group && d->group_setup.attempts < GROUP_SETUP_ATTEMPTS &&
	    now >= d->group_setup.due)
		send_group_setup(d, now, send, data);
	detect_loss(d, now);
	if (d->ack_pending && now >= d->ack_due)
		send_ack(d, now, send, data);
	send_windows(d, now, send, data);
	send_streams(d, now, send, data);
	gint64 next = d->group ? d->group_setup.due : G_MAXINT64;

	if (d->ack_pending)
		next = MIN(next, d->ack_due);
	for (guint i = 0; i < d->inflight->len; i++)
		next = MIN(
			next,
			g_array_index(d->inflight, struct sent_packet, i).sent +
				d->rto);
	for (guint i = 0; i < MAX_STREAMS; i++) {
		struct tx_stream *s = &d->tx_streams[i];

		if (s->used &&
		    (s->open_pending || s->lost->len || s->reply_ctrl ||
		     (!s->paused && s->next < s->buf_offset + s->buf->len &&
		      s->next < s->peer_window)))
			next = MIN(next, now + 2000);
	}
	return next;
}

void call_pdtp_report(const struct call_pdtp *d)
{
	if (!d)
		return;
	g_printerr("line-call pdtp sent=%" G_GUINT64_FORMAT
		   " bytes=%" G_GUINT64_FORMAT " acked=%" G_GUINT64_FORMAT
		   " lost=%" G_GUINT64_FORMAT
		   " inflight=%u rto=%" G_GINT64_FORMAT "ms\n",
		   d->sent_packets, d->sent_bytes, d->acked_packets,
		   d->lost_packets, d->inflight->len, d->rto / 1000);
	for (guint i = 0; i < MAX_STREAMS; i++) {
		const struct tx_stream *s = &d->tx_streams[i];

		if (s->used)
			g_printerr(
				"line-call pdtp tx stream=%" G_GUINT64_FORMAT
				" service=%s open-acked=%d acked-to=%" G_GUINT64_FORMAT
				" sent-to=%" G_GUINT64_FORMAT
				" queued-to=%" G_GUINT64_FORMAT
				" window=%" G_GUINT64_FORMAT "\n",
				s->id, s->service, s->open_acked, s->buf_offset,
				s->next, s->buf_offset + s->buf->len,
				s->peer_window);
	}
}
