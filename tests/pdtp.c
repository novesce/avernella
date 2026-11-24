// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_pdtp.h"
#include "call_srtp.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	DATA_PT = 101,
	LOCAL_SSRC = 0x1234,
	RELAY_SSRC = 0xabcd,
};

static const guint8 local_key[30] = { 1, 2, 3 };
static const guint8 relay_key[30] = { 9, 8, 7 };

static int relay_packet(srtp_t tx, guint8 *packet, guint8 seq,
			const guint8 *payload, gsize size)
{
	const guint8 header[] = {
		0x90,
		0x80 | DATA_PT,
		0,
		seq,
		0,
		0,
		0,
		1,
		0,
		0,
		RELAY_SSRC >> 8,
		RELAY_SSRC & 0xff,
		0x02,
		0x40,
		0,
		0,
	};
	int n = sizeof(header) + size;

	memcpy(packet, header, sizeof(header));
	memcpy(packet + sizeof(header), payload, size);
	g_assert_cmpint(srtp_protect(tx, packet, &n), ==, srtp_err_status_ok);
	return n;
}

static gboolean save_packet(const guint8 *packet, gsize size, gpointer data)
{
	GBytes **wire = data;

	g_assert_null(*wire);
	*wire = g_bytes_new(packet, size);
	return TRUE;
}

static guint16 verify_setup(srtp_t rx, GBytes *wire, guint64 original)
{
	gsize size;
	const guint8 *bytes = g_bytes_get_data(wire, &size);
	g_autofree guint8 *packet = g_memdup2(bytes, size);
	int n = size;
	guint64 max_delay, source;

	g_assert_cmpint(srtp_unprotect(rx, packet, &n), ==, srtp_err_status_ok);
	g_assert_cmpint(n, >=, 24);
	g_assert_cmpuint(packet[0], ==, 0x90);
	g_assert_cmpmem(packet + 12, 4, "\x02\x40\x00\x00", 4);
	g_assert_cmpuint(packet[18], ==, 0); /* No application service. */
	g_assert_cmpuint(packet[19], ==, original ? 3 : 2);
	g_assert_cmpuint(packet[20], ==, 0);
	const guint8 *p = packet + 21, *end = packet + n;

	g_assert_cmpuint(*p++, ==, 2);
	g_assert_true(call_pdtp_read_varint(&p, end, &max_delay));
	g_assert_cmpuint(max_delay, ==, 200);
	if (original) {
		g_assert_true(p < end);
		g_assert_cmpuint(*p++, ==, 1);
		g_assert_true(call_pdtp_read_varint(&p, end, &source));
		g_assert_cmpuint(source, ==, original);
	}
	g_assert_true(p == end);
	return packet[2] << 8 | packet[3];
}

static void group_ack(void)
{
	/* SET_MAX_ACK_DELAY=200 followed by STRM_OPEN for LINE stream 110. */
	static const guint8 open[] = {
		0x80, 0,    'L',  'I', 'N', 'E',  0,	0x02, 0x08,
		2,    0x40, 0xc8, 5,   1,   0x40, 0x6e, 0x12, 0,
	};
	static const guint8 bad[] = { 0x80, 0, 0, 0x02, 0, 2, 0x40 };
	static const guint8 ack[] = {
		0x80, 0, 0, 0x40, 0, 4, 42, 1, 25, 0,
	};
	static const guint8 receipt[] = {
		0x80, 0, 0, 0x40, 0, 4, 1, 1, 0, 0,
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) wire = NULL;
	struct call_pdtp *pdtp;
	srtp_t relay, relay_rx;
	guint8 packet[128];
	gsize size;
	int n;

	pdtp = call_pdtp_open_group(local_key, relay_key, DATA_PT, LOCAL_SSRC,
				    RELAY_SSRC, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pdtp);
	g_assert_false(call_pdtp_stream_open(pdtp, 3, "PLANET", 0x12));
	g_assert_true(
		call_srtp_open(&relay, relay_key, ssrc_specific, RELAY_SSRC));
	g_assert_true(call_srtp_open(&relay_rx, local_key, ssrc_specific,
				     LOCAL_SSRC));

	gint64 retry = call_pdtp_poll(pdtp, 1000000, save_packet, &wire);

	g_assert_cmpuint(verify_setup(relay_rx, wire, 0), ==, 1);
	g_assert_false(call_pdtp_group_setup_acknowledged(NULL));
	g_assert_false(call_pdtp_group_setup_acknowledged(pdtp));
	g_clear_pointer(&wire, g_bytes_unref);
	n = relay_packet(relay, packet, 40, bad, sizeof(bad));
	g_assert_false(call_pdtp_receive(pdtp, packet, n, 1000000));
	n = relay_packet(relay, packet, 42, open, sizeof(open));
	g_assert_true(call_pdtp_matches(pdtp, packet, n));
	packet[n - 1] ^= 0x01;
	g_assert_false(call_pdtp_receive(pdtp, packet, n, 1000000));
	g_assert_cmpint(call_pdtp_poll(pdtp, 1000000, save_packet, &wire), ==,
			retry);
	g_assert_null(wire);
	packet[n - 1] ^= 0x01;
	g_assert_true(call_pdtp_receive(pdtp, packet, n, 1000000));
	g_assert_false(call_pdtp_receive(pdtp, packet, n, 1000000));
	g_assert_cmpint(call_pdtp_poll(pdtp, 1024999, save_packet, &wire), ==,
			1025000);
	g_assert_null(wire);
	/* The only response is an ACK with the group RTP extension. */
	g_assert_cmpint(call_pdtp_poll(pdtp, 1025000, save_packet, &wire), ==,
			retry);
	g_assert_nonnull(wire);
	const guint8 *sealed = g_bytes_get_data(wire, &size);

	g_assert_cmpuint(size, ==, 16 + sizeof(ack) + 10);
	memcpy(packet, sealed, size);
	n = size;
	g_assert_cmpint(srtp_unprotect(relay_rx, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpuint(packet[0], ==, 0x90);
	g_assert_cmpuint(packet[1], ==, 0x80 | DATA_PT);
	g_assert_cmpmem(packet + 12, 4, "\x02\x40\x00\x00", 4);
	g_assert_cmpmem(packet + 16, n - 16, ack, sizeof(ack));

	/* ACK-only peer packets are accepted without starting an ACK loop. */
	g_clear_pointer(&wire, g_bytes_unref);
	n = relay_packet(relay, packet, 43, receipt, sizeof(receipt));
	packet[n - 1] ^= 1;
	g_assert_false(call_pdtp_receive(pdtp, packet, n, 1100000));
	g_assert_false(call_pdtp_group_setup_acknowledged(pdtp));
	packet[n - 1] ^= 1;
	g_assert_true(call_pdtp_receive(pdtp, packet, n, 1100000));
	g_assert_true(call_pdtp_group_setup_acknowledged(pdtp));
	g_assert_cmpint(call_pdtp_poll(pdtp, 1200000, save_packet, &wire), ==,
			G_MAXINT64);
	g_assert_null(wire);
	srtp_dealloc(relay_rx);
	srtp_dealloc(relay);
	call_pdtp_close(pdtp);
}

static void group_setup_retries(gconstpointer acknowledged)
{
	struct call_pdtp *d = call_pdtp_open_group(
		local_key, relay_key, DATA_PT, LOCAL_SSRC, RELAY_SSRC, NULL);
	srtp_t tx, rx;
	gint64 now = 1000000, next = now;
	guint16 original = 0;
	guint attempts = 0;

	g_assert_nonnull(d);
	g_assert_true(
		call_srtp_open(&tx, relay_key, ssrc_specific, RELAY_SSRC));
	g_assert_true(
		call_srtp_open(&rx, local_key, ssrc_specific, LOCAL_SSRC));
	while (next != G_MAXINT64) {
		g_autoptr(GBytes) wire = NULL;
		guint8 packet[128];

		now = next;
		next = call_pdtp_poll(d, now, save_packet, &wire);
		g_assert_nonnull(wire);
		guint16 pn = verify_setup(rx, wire, original);

		if (!original)
			original = pn;
		g_assert_cmpuint(pn, ==, original + attempts);
		g_assert_cmpint(next, >, now);
		g_assert_cmpuint(++attempts, <=, 8);
		if (!acknowledged)
			continue;
		if (attempts == 2) {
			/* The receipt names our original packet, but its second
			 * element is truncated. It cannot stop retransmission.
			 */
			const guint8 bad[] = { 0x80,	 0, 0, 0x42, 0, 4,
					       original, 1, 0, 0,    2, 0x40 };
			int n = relay_packet(tx, packet, 8, bad, sizeof(bad));

			g_assert_false(call_pdtp_receive(d, packet, n, now));
			g_assert_false(call_pdtp_group_setup_acknowledged(d));
		}
		if (attempts == 3) {
			/* A late ACK through a range below the largest PN. */
			const guint8 ack[] = { 0x80,   0, 0, 0x40, 0, 6,
					       pn + 1, 1, 0, 1,	   2, 1 };
			int n = relay_packet(tx, packet, 9, ack, sizeof(ack));

			g_assert_true(call_pdtp_receive(d, packet, n, now));
			break;
		}
	}
	g_autoptr(GBytes) wire = NULL;

	g_assert_cmpuint(attempts, >=, 3);
	g_assert_cmpint(call_pdtp_poll(d, now + 60000000, save_packet, &wire),
			==, G_MAXINT64);
	g_assert_null(wire);
	g_assert_cmpint(call_pdtp_group_setup_acknowledged(d), ==,
			acknowledged != NULL);
	srtp_dealloc(rx);
	srtp_dealloc(tx);
	call_pdtp_close(d);
}

static void private_stream_dump(void)
{
	g_autofree char *dir = g_dir_make_tmp("avernella-pdtp-XXXXXX", NULL);
	g_assert_nonnull(dir);
	g_autofree char *path = g_build_filename(dir, "PLANET-4.bin", NULL);
	g_autofree char *line = g_build_filename(dir, "LINE-8.bin", NULL);
	g_autofree char *unknown = g_build_filename(dir, "OTHER-4.bin", NULL);
	g_autofree char *link_dir = g_strconcat(dir, "-link", NULL);
	const guint8 part[] = { 1, 0, 3 };
	g_autofree char *content = NULL;
	gsize size;
	struct stat st;

	call_pdtp_dump_stream(dir, "PLANET", 4, part, sizeof(part));
	call_pdtp_dump_stream(dir, "PLANET", 4, part, 1);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
	g_assert_true(g_file_get_contents(path, &content, &size, NULL));
	g_assert_cmpmem(content, size, "\x01\x00\x03\x01", 4);
	call_pdtp_dump_stream(dir, "OTHER", 4, part, sizeof(part));
	g_assert_false(g_file_test(unknown, G_FILE_TEST_EXISTS));

	call_pdtp_dump_stream(dir, "LINE", 8, part, 1);
	g_assert_cmpint(g_remove(path), ==, 0);
	g_assert_cmpint(symlink(line, path), ==, 0);
	call_pdtp_dump_stream(dir, "PLANET", 4, part, sizeof(part));
	g_assert_cmpint(g_remove(path), ==, 0);
	g_assert_cmpint(link(line, path), ==, 0);
	call_pdtp_dump_stream(dir, "PLANET", 4, part, sizeof(part));
	g_assert_cmpint(g_remove(path), ==, 0);
	g_assert_cmpint(chmod(line, 0644), ==, 0);
	call_pdtp_dump_stream(dir, "LINE", 8, part, sizeof(part));
	g_assert_cmpint(stat(line, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, 1);

	g_assert_cmpint(symlink(dir, link_dir), ==, 0);
	call_pdtp_dump_stream(link_dir, "PLANET", 4, part, sizeof(part));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_remove(link_dir), ==, 0);
	g_assert_cmpint(chmod(dir, 0755), ==, 0);
	call_pdtp_dump_stream(dir, "PLANET", 4, part, sizeof(part));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_remove(line), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void trace_metadata(void)
{
	/* Synthetic service name and bytes must stay out of ordinary logs. */
	static const guint8 open[] = "\x80\0"
				     "192.0.2.7"
				     "\0\0\x08"
				     "\x05\x01\x40\x6e\x12\0";
	static const guint8 unknown[] = "\x80\0\0\x80\0"
					"fixture-body";
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 5000000, 0);
		g_test_trap_assert_passed();
		g_test_trap_assert_stderr("*pdtp tx bytes=*");
		g_test_trap_assert_stderr("*pdtp rx bytes=*");
		g_test_trap_assert_stderr("*pdtp rx malformed bytes=*");
		g_test_trap_assert_stderr("*open stream=110 service=other*");
		g_test_trap_assert_stderr_unmatched("*80000002000240c8*");
		g_test_trap_assert_stderr_unmatched("*192.0.2.7*");
		g_test_trap_assert_stderr_unmatched("*3139322e302e322e37*");
		g_test_trap_assert_stderr_unmatched("*fixture-body*");
		g_test_trap_assert_stderr_unmatched(
			"*666978747572652d626f6479*");
		return;
	}

	struct call_pdtp *d = call_pdtp_open_group(
		local_key, relay_key, DATA_PT, LOCAL_SSRC, RELAY_SSRC, NULL);
	g_autoptr(GBytes) wire = NULL;
	guint8 packet[128];
	srtp_t tx;
	int n;

	g_assert_nonnull(d);
	g_assert_true(
		call_srtp_open(&tx, relay_key, ssrc_specific, RELAY_SSRC));
	call_pdtp_set_trace(d, TRUE);
	call_pdtp_poll(d, 1000000, save_packet, &wire);
	g_assert_nonnull(wire);
	n = relay_packet(tx, packet, 1, open, sizeof(open) - 1);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	n = relay_packet(tx, packet, 2, unknown, sizeof(unknown) - 1);
	g_assert_false(call_pdtp_receive(d, packet, n, 1000000));
	srtp_dealloc(tx);
	call_pdtp_close(d);
}

/* ---- One-to-one ACK bounds ---- */

enum {
	PEER_SSRC = 0x5678
};

/* Records every datagram the transport sends. */
static gboolean keep_packet(const guint8 *packet, gsize size, gpointer data)
{
	g_ptr_array_add(data, g_bytes_new(packet, size));
	return TRUE;
}

/* Seals one peer PDTP packet; 'seq' is also its packet number. */
static int peer_packet(srtp_t tx, guint8 *packet, guint16 seq,
		       const guint8 *pdtp, gsize size)
{
	const guint8 header[] = {
		0x80,
		DATA_PT,
		seq >> 8,
		seq,
		0,
		0,
		0,
		1,
		0,
		0,
		PEER_SSRC >> 8,
		PEER_SSRC & 0xff,
		/* The packet number's top half; the rest is the sequence. */
		0x80,
		0,
	};
	int n = sizeof(header) + size;

	memcpy(packet, header, sizeof(header));
	memcpy(packet + sizeof(header), pdtp, size);
	g_assert_cmpint(srtp_protect(tx, packet, &n), ==, srtp_err_status_ok);
	return n;
}

/* A peer packet holding a single ACK element. */
static int peer_ack(srtp_t tx, guint8 *packet, guint16 seq, const guint8 *ack,
		    gsize size)
{
	/* Control service, element mask ACK, group size. */
	guint8 pdtp[64] = { 0, 0x40, 0, size };

	g_assert_cmpuint(size, <=, sizeof(pdtp) - 4);
	memcpy(pdtp + 4, ack, size);
	return peer_packet(tx, packet, seq, pdtp, 4 + size);
}

/* A peer packet with one counted group of one element for LINE. */
static int peer_element(srtp_t tx, guint8 *packet, guint16 seq, guint type,
			const guint8 *el, gsize size)
{
	guint8 pdtp[64] = { 'L',      'I', 'N', 'E', 0, 0, 1u << (type - 1),
			    size + 1, 1 };

	g_assert_cmpuint(type, <=, 8);
	g_assert_cmpuint(size, <=, sizeof(pdtp) - 9);
	memcpy(pdtp + 9, el, size);
	return peer_packet(tx, packet, seq, pdtp, 9 + size);
}

/* STRM_DATA on stream 'id' with no flags. */
static int peer_data(srtp_t tx, guint8 *packet, guint16 seq, guint64 id,
		     guint64 offset, gsize size)
{
	guint8 el[48];
	gsize n = call_pdtp_varint(el, id);

	el[n++] = 0;
	n += call_pdtp_varint(el + n, offset);
	n += call_pdtp_varint(el + n, size);
	g_assert_cmpuint(n + size, <=, sizeof(el));
	memset(el + n, 'x', size);
	return peer_element(tx, packet, seq, PDTP_STREAM_DATA, el, n + size);
}

struct ack_peer {
	struct call_pdtp *d;
	srtp_t tx, rx;
	GPtrArray *sent;
	guint16 seq;
	gint64 now;
};

/* Opens a one-to-one channel with one 10-byte message in each of 'count'
 * packets, numbered 1 to 'count', all still in flight.
 */
static void ack_peer_open(struct ack_peer *a, guint count)
{
	*a = (struct ack_peer){ .seq = 1, .now = 1000000 };
	a->d = call_pdtp_open(local_key, relay_key, DATA_PT, LOCAL_SSRC,
			      PEER_SSRC, NULL);
	g_assert_nonnull(a->d);
	g_assert_true(
		call_srtp_open(&a->tx, relay_key, ssrc_specific, PEER_SSRC));
	g_assert_true(
		call_srtp_open(&a->rx, local_key, ssrc_specific, LOCAL_SSRC));
	a->sent = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	g_assert_true(call_pdtp_stream_open(a->d, 8, "LINE", 0x12));
	/* The first poll only starts the pacing clock. */
	call_pdtp_poll(a->d, a->now, keep_packet, a->sent);
	g_assert_cmpuint(a->sent->len, ==, 0);
	for (guint i = 0; i < count; i++) {
		g_assert_true(call_pdtp_stream_send(
			a->d, 8, (const guint8 *)"0123456789", 10, 0));
		a->now += 1000;
		call_pdtp_poll(a->d, a->now, keep_packet, a->sent);
		g_assert_cmpuint(a->sent->len, ==, i + 1);
	}
	for (guint i = 0; i < a->sent->len; i++) {
		gsize size;
		const guint8 *bytes =
			g_bytes_get_data(a->sent->pdata[i], &size);
		g_autofree guint8 *packet = g_memdup2(bytes, size);
		int n = size;

		g_assert_cmpint(srtp_unprotect(a->rx, packet, &n), ==,
				srtp_err_status_ok);
		g_assert_cmpuint(packet[2] << 8 | packet[3], ==, i + 1);
	}
	g_assert_cmpuint(call_pdtp_stream_backlog(a->d, 8), ==, 10 * count);
}

/* Delivers one ACK and checks that it took well under a second. */
static void ack_peer_send(struct ack_peer *a, const guint8 *ack, gsize size)
{
	guint8 packet[128];
	int n = peer_ack(a->tx, packet, a->seq++, ack, size);
	gint64 start = g_get_monotonic_time();

	g_assert_true(call_pdtp_receive(a->d, packet, n, a->now));
	g_assert_cmpint(g_get_monotonic_time() - start, <, G_USEC_PER_SEC);
}

static void ack_peer_close(struct ack_peer *a)
{
	g_ptr_array_unref(a->sent);
	srtp_dealloc(a->rx);
	srtp_dealloc(a->tx);
	call_pdtp_close(a->d);
}

/* The largest value a PDTP varint carries. */
static const guint64 varint_max = G_GUINT64_CONSTANT(0x3fffffffffffffff);

/*
 * An ACK body: largest, reason 1, no delay and the pair count, then 'n'
 * values, a gap and a count per pair. A short list truncates the ACK.
 */
static gsize ack_body(guint8 *out, guint64 largest, guint64 pairs,
		      const guint64 *values, gsize n)
{
	gsize size = call_pdtp_varint(out, largest);

	out[size++] = 1;
	out[size++] = 0;
	size += call_pdtp_varint(out + size, pairs);
	for (gsize i = 0; i < n; i++)
		size += call_pdtp_varint(out + size, values[i]);
	return size;
}

static void varint_limit(void)
{
	guint8 max[8], out[8];
	const guint8 *p = max;
	guint64 v;

	/* An eight-byte varint carries 62 bits, so UINT64_MAX never arrives. */
	memset(max, 0xff, sizeof(max));
	g_assert_true(call_pdtp_read_varint(&p, max + sizeof(max), &v));
	g_assert_cmpuint(v, ==, varint_max);
	g_assert_cmpuint(call_pdtp_varint(out, G_MAXUINT64), ==, 8);
	g_assert_cmpmem(out, 8, max, 8);
}

static void ack_ranges(void)
{
	struct ack_peer a;
	guint8 ack[64];
	gsize n;

	ack_peer_open(&a, 3);
	/* 3, then a gap of one and a range of one: 1. Packet 2 is missing. */
	n = ack_body(ack, 3, 1, (const guint64[]){ 1, 1 }, 2);
	ack_peer_send(&a, ack, n);
	g_assert_cmpuint(call_pdtp_stream_backlog(a.d, 8), ==, 20);
	/* A duplicate changes nothing; a reordered older ACK covers 2. */
	ack_peer_send(&a, ack, n);
	g_assert_cmpuint(call_pdtp_stream_backlog(a.d, 8), ==, 20);
	n = ack_body(ack, 2, 0, NULL, 0);
	ack_peer_send(&a, ack, n);
	g_assert_cmpuint(call_pdtp_stream_backlog(a.d, 8), ==, 0);
	ack_peer_close(&a);
}

static void ack_hostile(void)
{
	const guint64 max = varint_max;
	/* Each would act below packet zero; the whole ACK is ignored. */
	const struct {
		guint64 largest, pairs, values[4];
		gsize n;
	} rejected[] = {
		{ 3, 1, { 4, 0 }, 2 }, /* a gap below zero */
		{ 3, 1, { 1, 3 }, 2 }, /* a range below zero */
		{ max, 1, { max, max }, 2 }, /* the sum passes the largest */
		{ 3, 2, { 0, 3, 0, 1 }, 4 }, /* a later pair below zero */
		{ 3, 1, { max, 0 }, 2 }, /* the largest gap */
		{ 3, 257, { 0, 0 }, 2 }, /* too many pairs */
		{ 3, 2, { 0, 1 }, 2 }, /* a missing pair */
	};
	struct ack_peer a;
	guint8 ack[64];

	ack_peer_open(&a, 3);
	for (guint i = 0; i < G_N_ELEMENTS(rejected); i++) {
		gsize n = ack_body(ack, rejected[i].largest, rejected[i].pairs,
				   rejected[i].values, rejected[i].n);

		ack_peer_send(&a, ack, n);
		g_assert_cmpuint(call_pdtp_stream_backlog(a.d, 8), ==, 30);
	}
	/*
	 * The largest representable number with a range down to zero names
	 * about 2^62 packets. Only the three in flight cost any work.
	 */
	gsize n = ack_body(ack, max, 1, (const guint64[]){ 0, max }, 2);

	ack_peer_send(&a, ack, n);
	g_assert_cmpuint(call_pdtp_stream_backlog(a.d, 8), ==, 0);
	ack_peer_close(&a);
}

struct delivered {
	guint64 offset;
	gsize bytes;
	guint calls;
};

static void on_data(guint64 stream, const char *service, guint64 offset,
		    guint64 time, guint flags, const guint8 *bytes, gsize size,
		    gpointer data)
{
	struct delivered *r = data;

	(void)stream, (void)service, (void)time, (void)flags, (void)bytes;
	r->offset = offset;
	r->bytes += size;
	r->calls++;
}

static void stream_offset_bounds(void)
{
	const guint64 max = G_GUINT64_CONSTANT(0x3fffffffffffffff);
	/* RECEIVE_WINDOW plus DEFAULT_WINDOW: the furthest data may reach. */
	const guint64 window = (16 << 20) + 327680;
	struct delivered got = { 0 };
	guint8 packet[128], open[16];
	srtp_t tx;
	int n;

	struct call_pdtp *d = call_pdtp_open(local_key, relay_key, DATA_PT,
					     LOCAL_SSRC, PEER_SSRC, NULL);

	g_assert_nonnull(d);
	g_assert_true(call_srtp_open(&tx, relay_key, ssrc_specific, PEER_SSRC));
	call_pdtp_set_handlers(d, on_data, NULL, &got);

	/* Stream 8 opens four bytes below the largest varint. */
	gsize o = call_pdtp_varint(open, 8);

	open[o++] = 0x12;
	o += call_pdtp_varint(open + o, max - 4);
	n = peer_element(tx, packet, 1, PDTP_STREAM_OPEN, open, o);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	/* Data ending past the largest offset is dropped. */
	n = peer_data(tx, packet, 2, 8, max - 4, 8);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_cmpuint(got.calls, ==, 0);
	/* Data ending exactly there is delivered. */
	n = peer_data(tx, packet, 3, 8, max - 4, 4);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_cmpuint(got.calls, ==, 1);
	g_assert_cmpuint(got.offset, ==, max - 4);
	g_assert_cmpuint(got.bytes, ==, 4);
	/* Old, overlapping and further data change nothing. */
	n = peer_data(tx, packet, 4, 8, max - 2, 2);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	n = peer_data(tx, packet, 5, 8, max, 1);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	n = peer_data(tx, packet, 6, 8, max - 1, 4);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_cmpuint(got.calls, ==, 1);
	g_assert_cmpuint(call_pdtp_stream_waiting(d, 8), ==, 0);

	/* A fresh stream buffers data only up to its window. */
	n = peer_data(tx, packet, 7, 10, window - 3, 4);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_cmpuint(call_pdtp_stream_waiting(d, 10), ==, 0);
	n = peer_data(tx, packet, 8, 10, max - 3, 4);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_cmpuint(call_pdtp_stream_waiting(d, 10), ==, 0);
	n = peer_data(tx, packet, 9, 10, window - 4, 4);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_cmpuint(call_pdtp_stream_waiting(d, 10), ==, 1);
	g_assert_cmpuint(got.calls, ==, 1);
	srtp_dealloc(tx);
	call_pdtp_close(d);
}

static void group_ack_hostile(void)
{
	struct call_pdtp *d = call_pdtp_open_group(
		local_key, relay_key, DATA_PT, LOCAL_SSRC, RELAY_SSRC, NULL);
	g_autoptr(GBytes) wire = NULL;
	guint8 packet[128];
	srtp_t tx;
	int n;

	g_assert_nonnull(d);
	g_assert_true(
		call_srtp_open(&tx, relay_key, ssrc_specific, RELAY_SSRC));
	call_pdtp_poll(d, 1000000, save_packet, &wire);
	g_assert_nonnull(wire);
	/* Setup went out as packet 1. A range that wraps names it. */
	guint8 pdtp[64] = { 0x80, 0, 0, 0x40, 0 };
	gsize size = ack_body(pdtp + 6, varint_max, 1,
			      (const guint64[]){ varint_max, varint_max }, 2);
	gint64 start = g_get_monotonic_time();

	pdtp[5] = size;
	n = relay_packet(tx, packet, 1, pdtp, 6 + size);
	g_assert_false(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_false(call_pdtp_group_setup_acknowledged(d));
	size = ack_body(pdtp + 6, varint_max, 1,
			(const guint64[]){ 0, varint_max }, 2);
	pdtp[5] = size;
	n = relay_packet(tx, packet, 2, pdtp, 6 + size);
	g_assert_true(call_pdtp_receive(d, packet, n, 1000000));
	g_assert_true(call_pdtp_group_setup_acknowledged(d));
	g_assert_cmpint(g_get_monotonic_time() - start, <, G_USEC_PER_SEC);
	srtp_dealloc(tx);
	call_pdtp_close(d);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pdtp/varint-limit", varint_limit);
	g_test_add_func("/pdtp/ack-ranges", ack_ranges);
	g_test_add_func("/pdtp/ack-hostile", ack_hostile);
	g_test_add_func("/pdtp/group-ack-hostile", group_ack_hostile);
	g_test_add_func("/pdtp/stream-offset-bounds", stream_offset_bounds);
	g_test_add_func("/pdtp/group-ack", group_ack);
	g_test_add_func("/pdtp/private-stream-dump", private_stream_dump);
	g_test_add_func("/pdtp/trace-metadata", trace_metadata);
	g_test_add_data_func("/pdtp/group-setup-delayed-ack",
			     GINT_TO_POINTER(1), group_setup_retries);
	g_test_add_data_func("/pdtp/group-setup-no-ack", NULL,
			     group_setup_retries);
	return g_test_run();
}
