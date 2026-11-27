// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Single-stream RFC 8627 short-mask FlexFEC. Operate on authenticated
 * plaintext RTP; the independent repair SSRC has its own SRTP replay state.
 */
#include "call_fec.h"
#include "call_srtp.h"
#include <math.h>
#include <openssl/rand.h>
#include <string.h>

enum {
	RTP = 12,
	REPAIR_RTP = 16,
	FEC_HEADER = 12,
	MASK_BITS = 15,
	MIN_BLOCK = 3, /* At most one repair per three source packets. */
	MAX_STRIDE = (MASK_BITS - 1) / (MIN_BLOCK - 1),
	/* Keep at most one SRTP replay window of source and repair history. */
	HISTORY = 128,
	PENDING = 32,
	MAX_MEDIA = 1200,
	TAG = 10,
};

struct equation {
	gint64 base;
	guint16 mask, used;
	guint8 header[8], payload[MAX_MEDIA - RTP];
};

struct source_slot {
	gint64 seq;
	guint16 size;
	gboolean repaired;
	guint8 packet[MAX_MEDIA];
};

struct call_fec {
	srtp_t tx, rx;
	guint32 tx_media, rx_media, tx_repair, rx_repair;
	guint16 seq;
	guint32 clock_base;
	guint stripe_next;
	guint8 pt, block, stride;
	guint credit;
	struct {
		struct equation eq;
		guint count;
		gint64 since;
	} stripe[MAX_STRIDE];
	gint64 high;
	gboolean started;
	struct source_slot history[HISTORY];
	struct equation pending[PENDING];
	guint next, queued;
};

static guint16 get16(const guint8 *p)
{
	return ((guint16)p[0] << 8) | p[1];
}

static guint32 get32(const guint8 *p)
{
	return ((guint32)get16(p) << 16) | get16(p + 2);
}

static void put16(guint8 *p, guint16 n)
{
	p[0] = n >> 8;
	p[1] = n;
}

static void put32(guint8 *p, guint32 n)
{
	put16(p, n >> 16);
	put16(p + 2, n);
}

struct call_fec *call_fec_new(guint8 pt, guint32 tx_media, guint32 rx_media,
			      guint32 tx_repair, guint32 rx_repair,
			      const guint8 tx_key[30], const guint8 rx_key[30])
{
	if (pt < 96 || pt > 127 || !tx_key || !rx_key || !tx_media ||
	    !rx_media || !tx_repair || !rx_repair || tx_repair == rx_repair ||
	    tx_repair == tx_media || tx_repair == rx_media ||
	    rx_repair == tx_media || rx_repair == rx_media)
		return NULL;
	struct call_fec *f = g_try_new0(struct call_fec, 1);

	if (!f)
		return NULL;
	if (!call_srtp_open(&f->tx, tx_key, ssrc_specific, tx_repair) ||
	    !call_srtp_open(&f->rx, rx_key, ssrc_specific, rx_repair) ||
	    RAND_bytes((guint8 *)&f->seq, sizeof(f->seq)) != 1 ||
	    RAND_bytes((guint8 *)&f->clock_base, sizeof(f->clock_base)) != 1) {
		call_fec_free(f);
		return NULL;
	}
	f->pt = pt;
	f->tx_media = tx_media;
	f->rx_media = rx_media;
	f->tx_repair = tx_repair;
	f->rx_repair = rx_repair;
	f->block = MASK_BITS;
	f->stride = 1;
	f->credit = MAX_MEDIA;
	return f;
}

void call_fec_free(struct call_fec *f)
{
	if (!f)
		return;
	if (f->tx)
		srtp_dealloc(f->tx);
	if (f->rx)
		srtp_dealloc(f->rx);
	g_free(f);
}

gboolean call_fec_same(const struct call_fec *f, guint8 pt, guint32 tx_repair,
		       guint32 rx_repair)
{
	return f && pt == f->pt && tx_repair == f->tx_repair &&
	       rx_repair == f->rx_repair;
}

gboolean call_fec_matches(const struct call_fec *f, const guint8 *p, gsize n)
{
	return f && p && n >= REPAIR_RTP + FEC_HEADER + TAG && p[0] >> 6 == 2 &&
	       (p[1] & 127) == f->pt && get32(p + 8) == f->rx_repair;
}

void call_fec_loss(struct call_fec *f, double loss, guint lost, guint adjacent)
{
	if (!f)
		return;
	/* One repair per expected loss. The mean observed burst is the number
	 * of distinct NACKed packets divided by the number of runs. Stripe
	 * by that many packets so adjacent losses land in distinct equations.
	 * A short mask requires (block - 1) * stride < MASK_BITS.
	 */
	guint block = loss > 1.0 / MASK_BITS ?
			      CLAMP((guint)ceil(1.0 / MIN(loss, 1.0)),
				    (guint)MIN_BLOCK, (guint)MASK_BITS) :
			      MASK_BITS;
	guint stride = block <= (MASK_BITS + 1) / 2 ? 2 : 1;

	if (lost > adjacent && adjacent) {
		guint runs = lost - adjacent;

		stride = CLAMP(MAX(stride, 1u + (lost - 1) / runs), 1u,
			       (guint)MAX_STRIDE);
	}
	block = MIN(block, 1u + (MASK_BITS - 1u) / stride);
	if (block != f->block || stride != f->stride) {
		for (guint i = 0; i < MAX_STRIDE; i++)
			f->stripe[i].count = 0;
		f->block = block;
		f->stride = stride;
		f->stripe_next = 0;
	}
}

double call_fec_share(const struct call_fec *f)
{
	return f ? 1.0 / (1 + f->block) : 0.0;
}

/* XOR the protected header fields and all bytes following the fixed RTP
 * header, including any CSRCs, extension and padding. Shorter inputs are
 * implicitly zero-extended. Never XOR the RTP version, sequence or SSRC.
 */
static void add_source(struct equation *e, const guint8 *p, gsize n)
{
	gsize bytes = n - RTP;

	e->header[0] ^= p[0] & 0x3f;
	e->header[1] ^= p[1];
	e->header[2] ^= bytes >> 8;
	e->header[3] ^= bytes;
	for (guint i = 4; i < 8; i++)
		e->header[i] ^= p[i];
	for (gsize i = 0; i < bytes; i++)
		e->payload[i] ^= p[RTP + i];
	e->used = MAX(e->used, bytes);
}

gsize call_fec_encode(struct call_fec *f, const guint8 *p, gsize size,
		      gboolean end_key, gint64 now, guint fps, guint8 *out,
		      gsize capacity)
{
	if (!f || !p || !out || size <= RTP || size > MAX_MEDIA ||
	    p[0] >> 6 != 2 || get32(p + 8) != f->tx_media || !fps)
		return 0;
	guint16 seq = get16(p + 2);
	/* Count stripes separately so non-power-of-two strides survive a
	 * 16-bit source sequence number rollover.
	 */
	guint stripe = f->stripe_next;

	f->stripe_next = (stripe + 1) % f->stride;
	struct equation *e = &f->stripe[stripe].eq;
	guint16 offset = seq - (guint16)e->base;
	gint64 interval = G_USEC_PER_SEC / fps;

	if (!f->stripe[stripe].count || offset >= MASK_BITS ||
	    now - f->stripe[stripe].since > 2 * interval) {
		memset(e, 0, sizeof(*e));
		e->base = seq;
		f->stripe[stripe].count = 0;
		f->stripe[stripe].since = now;
		offset = 0;
	}
	add_source(e, p, size);
	e->mask |= 0x4000u >> offset;
	f->stripe[stripe].count++;
	f->credit =
		MIN(2u * MAX_MEDIA, f->credit + (guint)(size + TAG) / f->block);
	if (f->stripe[stripe].count < f->block && !end_key &&
	    now - f->stripe[stripe].since < interval)
		return 0;
	int n = REPAIR_RTP + FEC_HEADER + e->used;

	if (f->credit < (guint)n + TAG ||
	    capacity < (gsize)n + SRTP_MAX_TRAILER_LEN)
		return 0;
	f->stripe[stripe].count = 0;
	out[0] = 0x81;
	out[1] = f->pt;
	put16(out + 2, f->seq++);
	/* Repair packets use transmission time on their own 90 kHz clock. */
	put32(out + 4, f->clock_base + (guint64)now * 9 / 100);
	put32(out + 8, f->tx_repair);
	put32(out + RTP, f->tx_media);
	memcpy(out + REPAIR_RTP, e->header, sizeof(e->header));
	put16(out + REPAIR_RTP + 8, e->base);
	put16(out + REPAIR_RTP + 10, e->mask);
	memcpy(out + REPAIR_RTP + FEC_HEADER, e->payload, e->used);
	if (srtp_protect(f->tx, out, &n) != srtp_err_status_ok)
		return 0;
	f->credit -= n;
	return n;
}

static gint64 extended(const struct call_fec *f, guint16 seq)
{
	return f->started ? f->high + (gint16)(seq - (guint16)f->high) : seq;
}

static void store(struct call_fec *f, gint64 seq, const guint8 *p, gsize size,
		  gboolean repaired)
{
	struct source_slot *s = &f->history[(guint)seq % HISTORY];

	s->seq = seq;
	s->size = size;
	s->repaired = repaired;
	memcpy(s->packet, p, size);
	if (!f->started || seq > f->high) {
		f->high = seq;
		f->started = TRUE;
	}
}

static gboolean valid_media(const guint8 *p, gsize n)
{
	gsize off = RTP + 4u * (p[0] & 15);

	if (p[0] >> 6 != 2 || off >= n)
		return FALSE;
	if (p[0] & 0x10) {
		if (n - off < 4)
			return FALSE;
		off += 4u + 4u * get16(p + off + 2);
		if (off >= n)
			return FALSE;
	}
	return !(p[0] & 0x20) || (p[n - 1] && p[n - 1] < n - off);
}

/* Resolve the equations in arrival order until no newly reconstructed source
 * can help another. The cache and iteration bound are independent of input.
 */
void call_fec_drain(struct call_fec *f, guint8 pt,
		    call_fec_recovered_fn recover, gpointer data)
{
	gboolean again;

	if (!f || !f->queued || !recover)
		return;
	do {
		again = FALSE;
		for (guint i = 0; i < PENDING && f->queued; i++) {
			struct equation *e = &f->pending[i];

			if (!e->mask)
				continue;
			if (f->started && e->base < f->high - HISTORY + 1)
				e->mask = 0;
			guint count = 0, missing = 0;

			for (guint j = 0; j < MASK_BITS && e->mask; j++) {
				guint bit = 0x4000u >> j;

				if (!(e->mask & bit))
					continue;
				gint64 seq = e->base + j;
				struct source_slot *s =
					&f->history[(guint)seq % HISTORY];

				if (!s->size || s->seq != seq) {
					missing = j;
					count++;
					continue;
				}
				if (s->size - RTP > e->used) {
					e->mask = 0;
					count = 0;
					break;
				}
				add_source(e, s->packet, s->size);
				e->mask &= ~bit;
			}
			if (count > 1)
				continue;
			e->mask = 0;
			f->queued--;
			guint bytes = get16(e->header + 2);

			if (count != 1 || !bytes || bytes > e->used ||
			    (e->header[1] & 127) != pt)
				continue;
			gint64 seq = e->base + missing;
			guint8 p[MAX_MEDIA];

			p[0] = 0x80 | e->header[0];
			p[1] = e->header[1];
			put16(p + 2, seq);
			memcpy(p + 4, e->header + 4, 4);
			put32(p + 8, f->rx_media);
			memcpy(p + RTP, e->payload, bytes);
			if (!valid_media(p, RTP + bytes))
				continue;
			store(f, seq, p, RTP + bytes, TRUE);
			recover(p, RTP + bytes, data);
			again = TRUE;
		}
	} while (again && f->queued);
}

gboolean call_fec_source(struct call_fec *f, const guint8 *p, gsize n,
			 guint8 media_pt)
{
	if (!f || !p || n <= RTP || n > MAX_MEDIA || !valid_media(p, n) ||
	    get32(p + 8) != f->rx_media || (p[1] & 127) != media_pt)
		return FALSE;
	gint64 seq = extended(f, get16(p + 2));
	struct source_slot *s = &f->history[(guint)seq % HISTORY];

	if (s->size && s->seq == seq)
		return s->repaired;
	if (f->started && seq < f->high - HISTORY + 1)
		return TRUE;
	store(f, seq, p, n, FALSE);
	return FALSE;
}

gboolean call_fec_receive(struct call_fec *f, const guint8 *packet, gsize size,
			  guint8 media_pt, call_fec_recovered_fn recover,
			  gpointer data)
{
	guint8 plain[MAX_MEDIA + CALL_FEC_OVERHEAD + SRTP_MAX_TRAILER_LEN];
	int n = size;

	if (!recover || !call_fec_matches(f, packet, size) ||
	    size > sizeof(plain))
		return FALSE;
	memcpy(plain, packet, size);
	if (srtp_unprotect(f->rx, plain, &n) != srtp_err_status_ok ||
	    (plain[0] & 15) != 1 || get32(plain + RTP) != f->rx_media)
		return FALSE;
	int pos = REPAIR_RTP;

	if (plain[0] & 0x20) {
		guint pad = plain[n - 1];

		if (!pad || (int)pad >= n - pos)
			return FALSE;
		n -= pad;
	}
	if (plain[0] & 0x10) {
		if (n - pos < 4)
			return FALSE;
		pos += 4 + 4 * get16(plain + pos + 2);
	}
	int bytes = n - pos - FEC_HEADER;

	if (bytes <= 0 || bytes > MAX_MEDIA - RTP || (plain[pos] & 0xc0))
		return FALSE;
	guint16 mask = get16(plain + pos + 10);
	gint64 base = extended(f, get16(plain + pos + 8));

	if (!mask || (mask & 0x8000) ||
	    (f->started &&
	     (base < f->high - HISTORY + 1 || base > f->high + HISTORY)))
		return FALSE;
	struct equation *e = &f->pending[f->next++ % PENDING];

	if (!e->mask)
		f->queued++;
	e->base = base;
	e->mask = mask;
	e->used = bytes;
	memcpy(e->header, plain + pos, sizeof(e->header));
	memcpy(e->payload, plain + pos + FEC_HEADER, bytes);
	if (!f->started) {
		f->started = TRUE;
		f->high = base;
	}
	call_fec_drain(f, media_pt, recover, data);
	return TRUE;
}
