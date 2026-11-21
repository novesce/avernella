/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <openssl/evp.h>

typedef struct {
	const guint8 *p;
	gsize n;
} Slice;

typedef struct {
	guint8 p[2048];
	gsize n;
	gboolean overflow;
} Pb;

typedef struct {
	guint field, type;
	guint64 value;
	Slice data;
} PbField;

typedef struct {
	EVP_PKEY *key;
	guint8 pub[33], peer[33], tx_base[64], rx_base[64], tx[64], rx[64],
		prefix[51];
	guint16 seq;
	guint count;
	gboolean have_rx;
} CallWire;

#define PB_SLICE(b) ((Slice){ (b).p, (b).n })
void pb_uint(Pb *b, guint field, guint64 v);
void pb_fixed64(Pb *b, guint field, guint64 v);
void pb_bytes(Pb *b, guint field, Slice v);
void pb_text(Pb *b, guint field, const char *v);
void pb_msg(Pb *b, guint field, const Pb *child);
gboolean pb_next(Slice *remaining, PbField *field);
Slice pb_data(Slice b, guint field);
guint64 pb_number(Slice b, guint field, guint64 fallback);
gboolean call_keygen(EVP_PKEY **key, guint8 pub[33]);
gboolean call_shared(EVP_PKEY *key, const guint8 peer[33], guint8 out[32]);
gboolean call_wire_init(CallWire *wire, const guint8 peer[33]);
void call_wire_clear(CallWire *wire);
gboolean call_wire_pack(CallWire *wire, const Pb *plain, gboolean bootstrap,
			gboolean raw, Pb *packet);
gboolean call_wire_open(CallWire *wire, Slice packet, Pb *plain);
gboolean call_audio_keys(EVP_PKEY *key, const guint8 local[33],
			 const guint8 peer[33], const guint8 local_nonce[16],
			 const guint8 peer_nonce[16], guint32 local_id,
			 guint32 peer_id, guint8 tx[30], guint8 rx[30]);

gboolean call_video_keys(EVP_PKEY *key, const guint8 local[33],
			 const guint8 peer[33], const guint8 local_nonce[16],
			 const guint8 peer_nonce[16], guint32 local_id,
			 guint32 peer_id, guint8 tx[30], guint8 rx[30]);
gboolean call_data_keys(EVP_PKEY *key, const guint8 local[33],
			const guint8 peer[33], const guint8 local_nonce[16],
			const guint8 peer_nonce[16], guint32 local_id,
			guint32 peer_id, guint8 tx[30], guint8 rx[30]);
