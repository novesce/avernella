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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pdtp/group-ack", group_ack);
	g_test_add_func("/pdtp/private-stream-dump", private_stream_dump);
	g_test_add_func("/pdtp/trace-metadata", trace_metadata);
	g_test_add_data_func("/pdtp/group-setup-delayed-ack",
			     GINT_TO_POINTER(1), group_setup_retries);
	g_test_add_data_func("/pdtp/group-setup-no-ack", NULL,
			     group_setup_retries);
	return g_test_run();
}
