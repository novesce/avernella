// SPDX-License-Identifier: GPL-3.0-or-later
/* PLANET packet format and key schedule from evex-dev/linejs v3.6.5 (MIT). */
#include "call_wire.h"
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <string.h>

static void append(Pb *b, const void *p, gsize n)
{
	if (b->overflow || n > sizeof(b->p) - b->n) {
		b->overflow = TRUE;
		return;
	}
	if (n)
		memcpy(b->p + b->n, p, n);
	b->n += n;
}

static void varint(Pb *b, guint64 n)
{
	do {
		guint8 byte = (guint8)(n & 127);

		n >>= 7;
		if (n)
			byte |= 128;
		append(b, &byte, 1);
	} while (n);
}

void pb_uint(Pb *b, guint f, guint64 n)
{
	varint(b, f * 8);
	varint(b, n);
}

void pb_fixed64(Pb *b, guint f, guint64 n)
{
	n = GUINT64_TO_LE(n);
	varint(b, f * 8 + 1);
	append(b, &n, sizeof(n));
}

void pb_bytes(Pb *b, guint f, Slice v)
{
	varint(b, f * 8 + 2);
	varint(b, v.n);
	append(b, v.p, v.n);
}

void pb_text(Pb *b, guint f, const char *v)
{
	if (v)
		pb_bytes(b, f, (Slice){ (const guint8 *)v, strlen(v) });
}

void pb_msg(Pb *b, guint f, const Pb *child)
{
	if (child->overflow)
		b->overflow = TRUE;
	else
		pb_bytes(b, f, PB_SLICE(*child));
}

static gboolean read_varint(Slice *s, guint64 *v)
{
	*v = 0;
	for (guint shift = 0; shift < 64 && s->n; shift += 7) {
		guint8 byte = *s->p++;

		s->n--;
		if (shift == 63 && byte > 1)
			return FALSE;
		*v |= (guint64)(byte & 127) << shift;
		if (!(byte & 128))
			return TRUE;
	}
	return FALSE;
}

gboolean pb_next(Slice *s, PbField *f)
{
	guint64 k, n;
	*f = (PbField){ 0 };
	if (!read_varint(s, &k) || k < 8 || k > G_MAXUINT32)
		return FALSE;
	f->field = k >> 3;
	f->type = k & 7;
	switch (f->type) {
	case 0:
		return read_varint(s, &f->value);
	case 1:
		n = 8;
		break;
	case 2:
		if (!read_varint(s, &n))
			return FALSE;
		break;
	case 5:
		n = 4;
		break;
	default:
		return FALSE;
	}
	if (n > s->n)
		return FALSE;
	f->data = (Slice){ s->p, n };
	s->p += n;
	s->n -= n;
	return TRUE;
}

Slice pb_data(Slice b, guint field)
{
	PbField f;

	while (b.n && pb_next(&b, &f))
		if (f.field == field && f.type == 2)
			return f.data;
	return (Slice){ 0 };
}

guint64 pb_number(Slice b, guint field, guint64 fallback)
{
	PbField f;

	while (b.n && pb_next(&b, &f))
		if (f.field == field && !f.type)
			return f.value;
	return fallback;
}

gboolean call_keygen(EVP_PKEY **key, guint8 pub[33])
{
	*key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
	size_t n = 0;

	return *key &&
	       EVP_PKEY_set_utf8_string_param(
		       *key, OSSL_PKEY_PARAM_EC_POINT_CONVERSION_FORMAT,
		       "compressed") &&
	       EVP_PKEY_get_octet_string_param(*key, OSSL_PKEY_PARAM_PUB_KEY,
					       pub, 33, &n) &&
	       n == 33;
}

gboolean call_shared(EVP_PKEY *key, const guint8 peer[33], guint8 out[32])
{
	EVP_PKEY *remote = NULL;
	EVP_PKEY_CTX *import = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	EVP_PKEY_CTX *derive = EVP_PKEY_CTX_new(key, NULL);
	char group[] = "prime256v1";
	OSSL_PARAM params[] = {
		OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
						 group, 0),
		OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
						  (void *)peer, 33),
		OSSL_PARAM_construct_end()
	};
	size_t len = 32;
	gboolean ok = import && derive && EVP_PKEY_fromdata_init(import) == 1 &&
		      EVP_PKEY_fromdata(import, &remote, EVP_PKEY_PUBLIC_KEY,
					params) == 1 &&
		      EVP_PKEY_derive_init(derive) == 1 &&
		      EVP_PKEY_derive_set_peer(derive, remote) == 1 &&
		      EVP_PKEY_derive(derive, out, &len) == 1 && len == 32;
	EVP_PKEY_CTX_free(import);
	EVP_PKEY_CTX_free(derive);
	EVP_PKEY_free(remote);
	return ok;
}

static void mac(GChecksumType hash, Slice key, Slice data, guint8 *result)
{
	GHmac *h = g_hmac_new(hash, key.p, key.n);
	gsize n = g_checksum_type_get_length(hash);

	g_hmac_update(h, data.p, data.n);
	g_hmac_get_digest(h, result, &n);
	g_hmac_unref(h);
}

/*
 * PLANET's two HMAC passes use a zero-based, big-endian block counter. All
 * key-schedule outputs used here fit into the first block.
 */
static void kdf(GChecksumType hash, Slice salt, Slice ikm, Slice info,
		guint8 *out)
{
	guint8 prk[64], input[37] = { 0 };

	g_assert(info.n <= sizeof(input) - 4);
	mac(hash, ikm, salt, prk);
	memcpy(input, info.p, info.n);
	mac(hash, (Slice){ prk, g_checksum_type_get_length(hash) },
	    (Slice){ input, info.n + 4 }, out);
	OPENSSL_cleanse(prk, sizeof(prk));
}

static void transport_stage2(const guint8 base[64], const guint8 prefix[18],
			     guint8 out[64])
{
	kdf(G_CHECKSUM_SHA512, (Slice){ base, 64 }, (Slice){ prefix + 2, 16 },
	    (Slice){ prefix, 2 }, out);
}

gboolean call_wire_init(CallWire *w, const guint8 peer[33])
{
	guint8 secret[32] = { 0 };
	gboolean ok = call_keygen(&w->key, w->pub) &&
		      call_shared(w->key, peer, secret) &&
		      RAND_bytes(w->prefix, 18) == 1 &&
		      RAND_bytes((guint8 *)&w->seq, 2) == 1;
	if (ok) {
		w->seq = 1 + (w->seq % 1023);
		memcpy(w->peer, peer, 33);
		memcpy(w->prefix + 18, w->pub, 33);
		kdf(G_CHECKSUM_SHA512, (Slice){ secret, 32 },
		    (Slice){ peer, 33 }, (Slice){ w->pub, 33 }, w->tx_base);
		kdf(G_CHECKSUM_SHA512, (Slice){ secret, 32 },
		    (Slice){ w->pub, 33 }, (Slice){ peer, 33 }, w->rx_base);
		transport_stage2(w->tx_base, w->prefix, w->tx);
	}
	OPENSSL_cleanse(secret, sizeof(secret));
	return ok;
}

void call_wire_clear(CallWire *w)
{
	EVP_PKEY_free(w->key);
	OPENSSL_cleanse(w, sizeof(*w));
}

static gboolean frame_cipher(const guint8 key[64], const guint8 seq[2],
			     Slice src, guint8 *dst)
{
	guint8 iv[16];

	for (guint i = 0; i < sizeof(iv); i++)
		iv[i] = key[16 + i] ^ seq[i % 2];
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int n, final;
	gboolean ok = c &&
		      EVP_EncryptInit_ex(c, EVP_aes_128_ctr(), NULL, key, iv) ==
			      1 &&
		      EVP_EncryptUpdate(c, dst, &n, src.p, (int)src.n) == 1 &&
		      n == (int)src.n &&
		      EVP_EncryptFinal_ex(c, dst + n, &final) == 1 && !final;
	EVP_CIPHER_CTX_free(c);
	return ok;
}

gboolean call_wire_pack(CallWire *w, const Pb *plain, gboolean boot,
			gboolean raw, Pb *packet)
{
	gsize off = boot ? 62 : 6, total = off + plain->n + 16;

	if (plain->overflow || total > sizeof(packet->p) || w->count >= 65536)
		return FALSE;
	guint16 chunk = ((total - 4) << 5) | (boot ? 0x1d : 0x0d);
	guint16 encoded = ((chunk << 7) & 0x800) | (chunk << 12) | (chunk >> 5);
	guint8 *h = packet->p;

	h[0] = encoded >> 8;
	h[1] = encoded;
	h[2] = w->seq >> 8;
	h[3] = w->seq;
	w->seq++;
	w->count++;
	if (boot) {
		h[4] = 6;
		h[5] = 2;
		memcpy(h + 6, w->prefix, 51);
		memset(h + 57, 0, 3);
		h[60] = 0x28 | (plain->n >> 8);
		h[61] = plain->n;
	} else {
		h[4] = (raw ? 0x48 : 0x18) | (plain->n >> 8);
		h[5] = plain->n;
	}
	if (!frame_cipher(w->tx, h + 2, PB_SLICE(*plain), h + off))
		return FALSE;
	guint8 digest[32];

	mac(G_CHECKSUM_SHA256, (Slice){ w->tx + 32, 32 },
	    (Slice){ h, total - 16 }, digest);
	memcpy(h + total - 16, digest, 16);
	packet->n = total;
	return TRUE;
}

gboolean call_wire_open(CallWire *w, Slice pkt, Pb *plain)
{
	if (pkt.n < 22 || pkt.n > 2048)
		return FALSE;
	gboolean boot = pkt.p[4] == 6 && pkt.p[5] == 2;
	gsize off = boot ? 62 : 6;

	if (pkt.n < off + 16 || (!boot && !w->have_rx))
		return FALSE;
	guint8 candidate[64], digest[32];

	if (boot)
		transport_stage2(w->rx_base, pkt.p + 6, candidate);
	const guint8 *key = boot ? candidate : w->rx;

	mac(G_CHECKSUM_SHA256, (Slice){ key + 32, 32 },
	    (Slice){ pkt.p, pkt.n - 16 }, digest);
	gsize n = pkt.n - off - 16;
	gboolean ok = !CRYPTO_memcmp(digest, pkt.p + pkt.n - 16, 16) &&
		      frame_cipher(key, pkt.p + 2, (Slice){ pkt.p + off, n },
				   plain->p);
	if (ok) {
		plain->n = n;
		if (boot) {
			memcpy(w->rx, candidate, 64);
			w->have_rx = TRUE;
		}
	}
	OPENSSL_cleanse(candidate, sizeof(candidate));
	return ok;
}

static void media_direction(const char *label, const guint8 shared[32],
			    const guint8 a[33], const guint8 b[33],
			    const guint8 nonce[16], guint32 id, guint8 out[30])
{
	guint8 stage1[32], stage2[32], stream[32], info[8] = { 0 };

	id = GUINT32_TO_BE(id);
	memcpy(info, &id, 4);
	kdf(G_CHECKSUM_SHA256, (Slice){ shared, 32 }, (Slice){ a, 33 },
	    (Slice){ b, 33 }, stage1);
	kdf(G_CHECKSUM_SHA256, (Slice){ stage1, 32 }, (Slice){ nonce, 16 },
	    (Slice){ info, 8 }, stage2);
	mac(G_CHECKSUM_SHA256, (Slice){ (const guint8 *)label, strlen(label) },
	    (Slice){ stage2, 30 }, stream);
	memcpy(out, stream, 30);
	OPENSSL_cleanse(stage1, 32);
	OPENSSL_cleanse(stage2, 32);
	OPENSSL_cleanse(stream, 32);
}

static gboolean media_keys(const char *label, EVP_PKEY *key,
			   const guint8 local[33], const guint8 peer[33],
			   const guint8 local_nonce[16],
			   const guint8 peer_nonce[16], guint32 local_id,
			   guint32 peer_id, guint8 tx[30], guint8 rx[30])
{
	guint8 shared[32] = { 0 };
	gboolean ok = call_shared(key, peer, shared);

	if (ok) {
		media_direction(label, shared, peer, local, local_nonce,
				local_id, tx);
		media_direction(label, shared, local, peer, peer_nonce, peer_id,
				rx);
	}
	OPENSSL_cleanse(shared, sizeof(shared));
	return ok;
}

gboolean call_audio_keys(EVP_PKEY *key, const guint8 local[33],
			 const guint8 peer[33], const guint8 local_nonce[16],
			 const guint8 peer_nonce[16], guint32 local_id,
			 guint32 peer_id, guint8 tx[30], guint8 rx[30])
{
	return media_keys("AUDIO", key, local, peer, local_nonce, peer_nonce,
			  local_id, peer_id, tx, rx);
}

gboolean call_video_keys(EVP_PKEY *key, const guint8 local[33],
			 const guint8 peer[33], const guint8 local_nonce[16],
			 const guint8 peer_nonce[16], guint32 local_id,
			 guint32 peer_id, guint8 tx[30], guint8 rx[30])
{
	return media_keys("VIDEO", key, local, peer, local_nonce, peer_nonce,
			  local_id, peer_id, tx, rx);
}

/* The PDTP transport labels its keys with the DATA media kind name. */
gboolean call_data_keys(EVP_PKEY *key, const guint8 local[33],
			const guint8 peer[33], const guint8 local_nonce[16],
			const guint8 peer_nonce[16], guint32 local_id,
			guint32 peer_id, guint8 tx[30], guint8 rx[30])
{
	return media_keys("DATA", key, local, peer, local_nonce, peer_nonce,
			  local_id, peer_id, tx, rx);
}
