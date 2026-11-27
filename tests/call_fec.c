// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_fec.h"
#include "call_srtp.h"
#include <string.h>

enum {
	PT = 97,
	FEC_PT = 110,
	A = 101,
	B = 102,
	RA = 103,
	RB = 104
};
static const guint8 key_a[30] = { 1 }, key_b[30] = { 2 };

/* Independent RFC 8627 short-mask fixture. Protects two different-length
 * RTP packets across the sequence rollover, including their header fields.
 */
static const guint8 one[] = { 0x80, PT, 0xff, 0xff, 0,	  1,	0,   2,
			      0,    0,	0,    A,    0x11, 0x22, 0x33 };
static const guint8 two[] = { 0x80, 128 + PT, 0, 0,    0,    1,	   0,	 5,   0,
			      0,    0,	      A, 0x44, 0x33, 0x22, 0x11, 0x55 };
static const guint8 vector[] = { 0x81, FEC_PT, 0,    1,	   0,	 1,    0,
				 5,    0,      0,    0,	   RA,	 0,    0,
				 0,    A,      0,    0x80, 0,	 6,    0,
				 0,    0,      7,    0xff, 0xff, 0x60, 0,
				 0x55, 0x11,   0x11, 0x11, 0x55 };

static struct call_fec *receiver(void)
{
	return call_fec_new(FEC_PT, B, A, RB, RA, key_b, key_a);
}

static int protect(guint8 *p, const guint8 *plain, gsize len, srtp_t tx)
{
	int n = len;

	memcpy(p, plain, len);
	g_assert_cmpint(srtp_protect(tx, p, &n), ==, srtp_err_status_ok);
	return n;
}

static void collect(const guint8 *p, gsize size, gpointer data)
{
	GPtrArray *packets = data;

	g_ptr_array_add(packets, g_bytes_new(p, size));
}

static void decode_vector(void)
{
	g_autoptr(GPtrArray) packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	struct call_fec *f = receiver();
	srtp_t tx;
	guint8 wire[1400];

	g_assert_nonnull(f);
	g_assert_true(call_srtp_open(&tx, key_a, ssrc_specific, RA));
	int n = protect(wire, vector, sizeof(vector), tx);

	wire[n - 1] ^= 1;
	g_assert_false(call_fec_receive(f, wire, n, PT, collect, packets));
	wire[n - 1] ^= 1;
	g_assert_true(call_fec_receive(f, wire, n, PT, collect, packets));
	g_assert_false(call_fec_receive(f, wire, n, PT, collect, packets));
	g_assert_cmpuint(packets->len, ==, 0);
	g_assert_false(call_fec_source(f, two, sizeof(two), PT));
	call_fec_drain(f, PT, collect, packets);
	g_assert_cmpuint(packets->len, ==, 1);
	gsize size;
	const guint8 *p = g_bytes_get_data(packets->pdata[0], &size);

	g_assert_cmpmem(p, size, one, sizeof(one));
	g_assert_true(call_fec_source(f, one, sizeof(one), PT));
	call_fec_free(f);
	srtp_dealloc(tx);
}

static void encode_vector(void)
{
	struct call_fec *f = call_fec_new(FEC_PT, A, B, RA, RB, key_a, key_b);
	guint8 repair[1400];
	srtp_t observer;

	g_assert_nonnull(f);
	g_assert_cmpint(call_fec_encode(f, one, sizeof(one), FALSE, 0, 30,
					repair, sizeof(repair)),
			==, 0);
	int n = call_fec_encode(f, two, sizeof(two), TRUE, 0, 30, repair,
				sizeof(repair));

	g_assert_cmpint(n, ==, sizeof(vector) + 10);
	g_assert_true(call_srtp_open(&observer, key_a, ssrc_specific, RA));
	g_assert_cmpint(srtp_unprotect(observer, repair, &n), ==,
			srtp_err_status_ok);
	/* The repair stream has its own random sequence and clock. */
	memcpy(repair + 2, vector + 2, 6);
	g_assert_cmpmem(repair, n, vector, sizeof(vector));
	call_fec_free(f);
	srtp_dealloc(observer);
}

static void malformed_repairs(void)
{
	g_autoptr(GPtrArray) packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	struct call_fec *f = receiver();
	srtp_t tx;
	guint8 wire[1400], input[sizeof(vector)];
	/* Structural errors even when the repair packet authenticates. */
	const struct {
		guint off;
		guint8 value;
	} invalid[] = {
		{ 15, B }, { 16, 0x40 }, { 16, 0x80 }, { 26, 0 }, { 26, 0xe0 }
	};

	g_assert_true(call_srtp_open(&tx, key_a, ssrc_specific, RA));
	for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
		memcpy(input, vector, sizeof(input));
		input[3] = i + 1;
		input[invalid[i].off] = invalid[i].value;
		int n = protect(wire, input, sizeof(input), tx);

		g_assert_false(
			call_fec_receive(f, wire, n, PT, collect, packets));
	}
	/* An impossible reconstructed length cannot create a source packet. */
	memcpy(input, vector, sizeof(input));
	input[3] = 6;
	input[18] = 0xff;
	call_fec_source(f, one, sizeof(one), PT);
	int n = protect(wire, input, sizeof(input), tx);

	g_assert_true(call_fec_receive(f, wire, n, PT, collect, packets));
	g_assert_cmpuint(packets->len, ==, 0);
	/* RFC 8627 requires receivers to ignore the outer RTP marker bit. */
	memcpy(input, vector, sizeof(input));
	input[3] = 7;
	input[1] |= 0x80;
	n = protect(wire, input, sizeof(input), tx);
	g_assert_true(call_fec_receive(f, wire, n, PT, collect, packets));
	g_assert_cmpuint(packets->len, ==, 1);
	call_fec_free(f);
	srtp_dealloc(tx);
}

static void measured_burst(void)
{
	struct call_fec *tx = call_fec_new(FEC_PT, A, B, RA, RB, key_a, key_b);

	g_assert_nonnull(tx);
	call_fec_loss(tx, 0.04, 1, 0);
	g_assert_cmpfloat_with_epsilon(call_fec_share(tx), 1.0 / 16, 1e-9);
	/* Six losses in two bursts need a three-packet stride; five source
	 * packets fit the mask at offsets 0,3,6,9,12, hence 1/6 repair share.
	 */
	call_fec_loss(tx, 0.04, 6, 4);
	g_assert_cmpfloat_with_epsilon(call_fec_share(tx), 1.0 / 6, 1e-9);
	call_fec_free(tx);
}

static void bounded_repair(void)
{
	g_autoptr(GPtrArray) packets =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	struct call_fec *tx = call_fec_new(FEC_PT, A, B, RA, RB, key_a, key_b);
	struct call_fec *rx = receiver();
	guint8 p[1024] = { 0x80, PT, 0, 0, 0, 1, 0, 2, 0, 0, 0, A };
	guint8 repair[1400];
	guint repaired = 0;
	guint64 wire = 0, parity = 0;

	call_fec_loss(tx, 0.2, 0, 0);
	for (guint seq = 0; seq < 500; seq++) {
		p[2] = seq >> 8;
		p[3] = seq;
		memset(p + 12, (guint8)seq, sizeof(p) - 12);
		gint64 now = (seq / 10) * 33333;
		int n = call_fec_encode(tx, p, sizeof(p), FALSE, now, 30,
					repair, sizeof(repair));

		wire += sizeof(p) + 10;
		/* Adjacent losses fall in the two distinct parity stripes. */
		if (seq % 10 >= 2) {
			call_fec_source(rx, p, sizeof(p), PT);
			call_fec_drain(rx, PT, collect, packets);
		}
		if (n) {
			parity += n;
			g_assert_true(call_fec_receive(rx, repair, n, PT,
						       collect, packets));
		}
	}
	for (guint i = 0; i < packets->len; i++) {
		gsize n;
		const guint8 *q = g_bytes_get_data(packets->pdata[i], &n);
		guint seq = ((guint)q[2] << 8) | q[3];

		g_assert_cmpuint(seq % 10, <, 2);
		g_assert_cmpuint(n, ==, sizeof(p));
		for (guint j = 12; j < n; j++)
			g_assert_cmpuint(q[j], ==, (guint8)seq);
		repaired++;
	}
	g_test_message("%u/100 adjacent losses repaired, %.2f%% wire overhead",
		       repaired, 100.0 * parity / (wire + parity));
	g_assert_cmpuint(repaired, >=, 90);
	g_assert_cmpuint(parity, <=, wire / 5 + 1200);
	call_fec_free(tx);
	call_fec_free(rx);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/fec/decode-rfc8627-vector", decode_vector);
	g_test_add_func("/fec/encode-rfc8627-vector", encode_vector);
	g_test_add_func("/fec/adjacent-loss-budget", bounded_repair);
	g_test_add_func("/fec/malformed-repairs", malformed_repairs);
	g_test_add_func("/fec/measured-burst", measured_burst);
	return g_test_run();
}
