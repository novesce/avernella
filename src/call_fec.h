/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/* One CSRC (4 bytes) and a 12-byte RFC 8627 short-mask FEC header. */
enum {
	CALL_FEC_OVERHEAD = 16
};

struct call_fec;
typedef void (*call_fec_recovered_fn)(const guint8 *packet, gsize size,
				      gpointer data);

struct call_fec *call_fec_new(guint8 pt, guint32 tx_media, guint32 rx_media,
			      guint32 tx_repair, guint32 rx_repair,
			      const guint8 tx_key[30], const guint8 rx_key[30]);
void call_fec_free(struct call_fec *f);
gboolean call_fec_matches(const struct call_fec *f, const guint8 *packet,
			  gsize size);
gboolean call_fec_same(const struct call_fec *f, guint8 pt, guint32 tx_repair,
		       guint32 rx_repair);
void call_fec_loss(struct call_fec *f, double loss, guint lost, guint adjacent);
double call_fec_share(const struct call_fec *f);
/* The input is the plain source RTP, the optional output is encrypted repair. */
gsize call_fec_encode(struct call_fec *f, const guint8 *source, gsize size,
		      gboolean end_key, gint64 now, guint fps, guint8 *out,
		      gsize capacity);
gboolean call_fec_receive(struct call_fec *f, const guint8 *packet, gsize size,
			  guint8 media_pt, call_fec_recovered_fn recovered,
			  gpointer data);
/* TRUE if the original was repaired or has aged out of the replay window. */
gboolean call_fec_source(struct call_fec *f, const guint8 *packet, gsize size,
			 guint8 media_pt);
void call_fec_drain(struct call_fec *f, guint8 media_pt,
		    call_fec_recovered_fn recovered, gpointer data);
