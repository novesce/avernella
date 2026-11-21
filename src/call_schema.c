// SPDX-License-Identifier: GPL-3.0-or-later
/* Field maps and advertised audio streams follow evex-dev/linejs v3.6.5 (MIT). */
#include "call_schema.h"
#include <openssl/rand.h>
#include <string.h>

static void pair(Pb *b, guint field, guint64 a, guint64 c)
{
	Pb child = { 0 };

	pb_uint(&child, 1, a);
	pb_uint(&child, 2, c);
	pb_msg(b, field, &child);
}

static void offer_stream(Pb *out, const CallInfo *s, guint index,
			 gboolean answer)
{
	static const char *const labels[] = { "A", "V", "D" };
	const guint bitrate[] = { 32, CALL_VIDEO_RECEIVE_KBPS, 2000 },
		    kinds[] = { 1, 2, 6 };
	guint rtp = 101 + 10 * index + answer, rtcp = 201 + 10 * index + answer;
	guint payload = 96 + index;
	Pb stream = { 0 }, codec = { 0 }, path = { 0 };

	if (index == 0 && s->audio_payload)
		payload = s->audio_payload;
	else if (index == 1 && s->video_payload)
		payload = s->video_payload;
	else if (index == 2 && s->data_payload)
		payload = s->data_payload;
	pb_text(&codec, 1, labels[index]);
	pb_uint(&codec, 2, 3);
	pb_uint(&codec, 3, index != 1 || s->video);
	pb_uint(&codec, 4, bitrate[index]);
	if (index == 1) {
		pb_uint(&codec, 5, CALL_VIDEO_RECEIVE_FPS);
		pb_uint(&codec, 6, CALL_VIDEO_RECEIVE_RESOLUTION);
	}
	pb_uint(&codec, 50, kinds[index]);
	pb_uint(&path, 1, payload);
	pb_uint(&path, 11, rtp);
	pair(&path, 12, 67, 6801 + answer);
	pb_uint(&path, 61, rtcp);
	pair(&path, 62, 67, 13601 + answer);
	pb_msg(&stream, 1, &codec);
	pb_msg(&stream, 2, &path);
	if (index == 1 && (s->video || s->video_capable))
		pair(&stream, 51, 0,
		     1); /* Base VP8 without the VP8A extension. */
	if (index == 2)
		pair(&stream, 51, 0, 2);
	else {
		Pb channel = { 0 };

		pb_uint(&channel, 1, 127);
		pb_msg(&channel, 2, &path);
		pb_uint(&channel, 11, rtp);
		pb_uint(&channel, 61, rtcp);
		pb_msg(&stream, 3, &channel);
	}
	pb_msg(out, 1, &stream);
}

static void media_offer(Pb *out, const CallInfo *s, gboolean answer)
{
	for (guint i = 0; i < 3; i++)
		offer_stream(out, s, i, answer);
	Pb key = { 0 }, agreement = { 0 }, secret = { 0 }, extra = { 0 };

	pb_bytes(&key, 1, (Slice){ s->pub, 33 });
	pb_uint(&key, 2, s->media_id);
	pb_bytes(&key, 3, (Slice){ s->media_nonce, 16 });
	pb_msg(&agreement, 3, &key);
	pb_msg(out, 2, &agreement);
	pb_bytes(&secret, 1, (Slice){ s->media_secret, 30 });
	pb_msg(&extra, 2, &secret);
	if (!answer)
		pb_msg(out, 2, &extra);
	pair(out, 3, 0, 3);
}

void call_offer(Pb *out, const CallInfo *s)
{
	media_offer(out, s, FALSE);
}

void call_answer_offer(Pb *out, const CallInfo *s)
{
	media_offer(out, s, TRUE);
}

static gboolean packed_maps(Slice bytes, gboolean *vp8)
{
	while (bytes.n) {
		guint32 value = 0;

		for (guint shift = 0;; shift += 7) {
			if (!bytes.n)
				return FALSE;
			guint8 byte = *bytes.p++;

			bytes.n--;
			if (shift == 28 && byte > 15)
				return FALSE;
			value |= (guint32)(byte & 127) << shift;
			if (!(byte & 128))
				break;
		}
		*vp8 |= value == 2;
	}
	return TRUE;
}

static gboolean baseline_map(Slice head)
{
	gboolean vp8 = FALSE;
	PbField field;

	while (head.n) {
		if (!pb_next(&head, &field))
			return FALSE;
		if (field.field != 50)
			continue;
		if (field.type == 0 && field.value <= G_MAXUINT32)
			vp8 |= field.value == 2;
		else if (field.type != 2 || !packed_maps(field.data, &vp8))
			return FALSE;
	}
	return vp8;
}

static gboolean baseline_features(Slice stream, gboolean initial_offer)
{
	PbField field;

	while (stream.n) {
		if (!pb_next(&stream, &field))
			return FALSE;
		if (field.field != 51 || field.type != 2)
			continue;
		guint64 id = pb_number(field.data, 1, G_MAXUINT64);
		guint64 version = pb_number(field.data, 2, 0);

		/*
		 * The shipped engine enables baseline VP8 for feature versions
		 * 1 and 2. Version 2 also enables VP8A. Higher packet-format
		 * capabilities can appear in an offer, but our selected answer
		 * uses version zero.
		 */
		if ((id == 0 && version > 2) ||
		    (id == 2 && !initial_offer && version))
			return FALSE;
	}
	/*
	 * The official app answers a voice call with an inactive VP8 stream
	 * and no feature list. That stream still starts when its camera turns
	 * on, so a missing list selects baseline VP8.
	 */
	return TRUE;
}

static gboolean video_profile(Slice description, gboolean initial_offer,
			      guint8 *payload, guint32 *local_ssrc,
			      guint32 *remote_ssrc)
{
	PbField record;
	guint8 selected = 0;
	guint32 local_id = 0, remote_id = 0;

	while (description.n) {
		if (!pb_next(&description, &record))
			return FALSE;
		if (record.field != 1 || record.type != 2)
			continue;
		Slice head = pb_data(record.data, 1);
		Slice name = pb_data(head, 1);

		if (name.n != 1 || *name.p != 'V')
			continue;
		guint64 state = pb_number(head, 3, 0);

		if (selected || pb_number(head, 2, 0) != 3 || state > 1 ||
		    !baseline_map(head) ||
		    !baseline_features(record.data, initial_offer))
			return FALSE;
		Slice transport = pb_data(record.data, 2);
		guint64 pt = pb_number(transport, 1, 0);
		guint64 local = pb_number(transport, 11, 0);
		guint64 remote = pb_number(transport, 61, 0);

		if (pt < 96 || pt > 127 || !local ||
		    (!initial_offer && !remote) || local > G_MAXUINT32 ||
		    remote > G_MAXUINT32 || local == remote)
			return FALSE;
		selected = pt;
		local_id = local;
		remote_id = remote;
	}
	if (!selected)
		return FALSE;
	*payload = selected;
	*local_ssrc = local_id;
	*remote_ssrc = remote_id;
	return TRUE;
}

void call_video_peer_limits(Slice description, struct call_video_limits *limits)
{
	PbField record;

	*limits = (struct call_video_limits){ 0 };
	while (description.n && pb_next(&description, &record)) {
		if (record.field != 1 || record.type != 2)
			continue;
		Slice head = pb_data(record.data, 1), name = pb_data(head, 1);

		if (name.n != 1 || *name.p != 'V')
			continue;
		limits->kbps = MIN(pb_number(head, 4, 0), 100000);
		limits->fps = MIN(pb_number(head, 5, 0), 120);
		limits->resolution = MIN(pb_number(head, 6, 0), 4);
		return;
	}
}

guint8 call_session_payload(Slice description, char label)
{
	PbField record;

	while (description.n && pb_next(&description, &record)) {
		if (record.field != 1 || record.type != 2)
			continue;
		Slice head = pb_data(record.data, 1), name = pb_data(head, 1);
		guint64 pt = pb_number(pb_data(record.data, 2), 1, 0);

		if (name.n == 1 && *name.p == label)
			return pt >= 96 && pt <= 127 ? pt : 0;
	}
	return 0;
}

gboolean call_session_rtp(Slice description, char label, guint8 *payload,
			  guint32 *local_ssrc, guint32 *remote_ssrc)
{
	PbField record;

	while (description.n && pb_next(&description, &record)) {
		if (record.field != 1 || record.type != 2)
			continue;
		Slice head = pb_data(record.data, 1), name = pb_data(head, 1);

		if (name.n != 1 || *name.p != label)
			continue;
		Slice rtp = pb_data(record.data, 2);
		guint64 pt = pb_number(rtp, 1, 0),
			local = pb_number(rtp, 11, 0),
			remote = pb_number(rtp, 61, 0);

		if (pt < 96 || pt > 127 || !local || !remote ||
		    local > G_MAXUINT32 || remote > G_MAXUINT32 ||
		    local == remote || pb_number(head, 2, 0) != 3)
			return FALSE;
		*payload = pt;
		*local_ssrc = local;
		*remote_ssrc = remote;
		return TRUE;
	}
	return FALSE;
}

gboolean call_video_offer_profile(Slice offer, guint8 *payload,
				  guint32 *local_ssrc, guint32 *remote_ssrc)
{
	return video_profile(offer, TRUE, payload, local_ssrc, remote_ssrc);
}

gboolean call_video_profile(Slice answer, guint8 *payload, guint32 *local_ssrc,
			    guint32 *remote_ssrc)
{
	return video_profile(answer, FALSE, payload, local_ssrc, remote_ssrc);
}

void call_user_agent(Pb *b)
{
	pb_text(b, 1, "Linux");
	pb_text(b, 2, "1");
	pb_text(b, 3, "Avernella");
}

static void call_credential(const CallInfo *s, gboolean answer,
			    guint8 credential[32])
{
	GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
	const char *const fields[] = { answer ? s->target : s->local,
				       answer ? s->local : s->target, s->token,
				       s->cid };
	for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
		if (i)
			g_checksum_update(checksum, (const guint8 *)"::", 2);
		g_checksum_update(checksum, (const guint8 *)fields[i],
				  strlen(fields[i]));
	}
	gsize n = 32;

	g_checksum_get_digest(checksum, credential, &n);
	g_checksum_free(checksum);
}

void call_verify(Pb *out, const CallInfo *s)
{
	Pb ua = { 0 };

	call_user_agent(&ua);
	pb_text(out, 1, s->target);
	pb_text(out, 2, s->local);
	pb_text(out, 3, s->i_zone);
	pb_text(out, 4, s->r_zone);
	pb_msg(out, 5, &ua);
	g_autofree char *device = g_base64_encode(s->device, 32);

	pb_text(out, 6, device);
	pb_uint(out, 7, 1);
	const guint caps[] = { 1, 2, 3, 6, 7 };

	for (guint i = 0; i < G_N_ELEMENTS(caps); i++)
		pb_uint(out, 8, caps[i]);
	guint8 credential[32];

	call_credential(s, TRUE, credential);
	pb_bytes(out, 9, (Slice){ credential, 32 });
	pb_text(out, 10, s->video ? "freecall.video" : "freecall.audio");
	pb_uint(out, 12, 1);
	pb_uint(out, 105, 0);
}

void call_setup(Pb *out, const CallInfo *s)
{
	Pb ua = { 0 }, offer = { 0 };

	call_user_agent(&ua);
	call_offer(&offer, s);
	pb_text(out, 1, s->local);
	pb_text(out, 2, s->target);
	pb_text(out, 3, s->i_zone);
	pb_text(out, 4, s->r_zone);
	pb_msg(out, 5, &ua);
	g_autofree char *device = g_base64_encode(s->device, 32);

	pb_text(out, 6, device);
	pb_uint(out, 7, 1);
	const guint capa[] = { 1, 2, 3, 6, 7 };

	for (guint i = 0; i < G_N_ELEMENTS(capa); i++)
		pb_uint(out, 8, capa[i]);
	pb_msg(out, 9, &offer);
	guint8 credential[32];

	call_credential(s, FALSE, credential);
	pb_bytes(out, 10, (Slice){ credential, 32 });
	pb_uint(out, 11, 0);
	pb_text(out, 12, s->video ? "freecall.video" : "freecall.audio");
	pb_uint(out, 14, 1);
	pb_text(out, 21, s->stid);
	const guint features[] = { 16, 17, 0 };

	for (guint i = 0; i < G_N_ELEMENTS(features); i++) {
		Pb feature = { 0 };

		pb_uint(&feature, 1, features[i]);
		pb_uint(&feature, 2, i == 0);
		pb_uint(&feature, 3, 0);
		pb_msg(out, 25, &feature);
	}
	pb_uint(out, 53, 0);
	pb_uint(out, 105, 0);
}

gboolean call_envelope(Pb *out, CallInfo *s, guint plane, guint oneof,
		       guint msg_id, const Pb *body, Slice reply_header)
{
	Pb header = { 0 }, choice = { 0 };
	guint8 txn[16];
	guint64 seq;
	Slice tran;

	if (reply_header.p) {
		tran = pb_data(reply_header, 4);
		seq = pb_number(reply_header, 5, G_MAXUINT64);
		if (tran.n != 16 || seq > G_MAXUINT32)
			return FALSE;
	} else {
		if (RAND_bytes(txn, sizeof(txn)) != 1)
			return FALSE;
		tran = (Slice){ txn, sizeof(txn) };
		seq = s->transaction++;
	}
	pb_text(&header, 1, s->local);
	pb_uint(&header, 2, msg_id);
	if (s->have_session)
		pb_bytes(&header, 3, (Slice){ s->session, 16 });
	pb_bytes(&header, 4, tran);
	pb_uint(&header, 5, seq);
	pb_uint(&header, 6, s->nonce);
	if (s->remote_nonce)
		pb_uint(&header, 7, s->remote_nonce);
	pb_msg(out, 1, &header);
	pb_msg(&choice, oneof, body);
	if (plane == 2)
		pb_msg(out, 2, &choice);
	else {
		Pb channel = { 0 }, scope = { 0 };

		pb_text(&channel, 1, s->cid);
		pb_uint(&channel, 2,
			(plane == 4 && s->local_media) ? s->local_media :
							 s->local_channel);
		guint64 dst = plane == 4 ? s->remote_media : s->remote_channel;

		if (dst)
			pb_uint(&channel, 3, dst);
		pb_msg(&scope, 1, &channel);
		pb_msg(&scope, 2, &choice);
		pb_msg(out, plane, &scope);
	}
	return !out->overflow;
}

/* Control-plane stream report. SSRC values are the one-to-one stream IDs. */
void call_stream_spec(Pb *out, guint32 audio_rx, guint32 audio_tx,
		      guint32 video_rx, guint32 video_tx, guint32 data_rx,
		      guint32 data_tx, gboolean camera_enabled)
{
	Pb state = { 0 };
	const guint32 rx[] = { audio_rx ? audio_rx : 102,
			       video_rx ? video_rx : 112,
			       data_rx ? data_rx : 122 };
	const guint32 tx_ids[] = { audio_tx ? audio_tx : 202,
				   video_tx ? video_tx : 212, data_tx };

	pb_uint(&state, 1, 0);
	pb_uint(&state, 2, 0);
	for (guint i = 0; i < 3; i++) {
		Pb strm = { 0 }, br = { 0 }, retx = { 0 }, fec = { 0 };

		pb_uint(&strm, 1, rx[i]);
		if (i == 1) {
			pb_uint(&br, 1, 100);
			pb_uint(&br, 2, CALL_VIDEO_RECEIVE_KBPS);
		}
		if (i == 2)
			pb_uint(&br, 2, 2000);
		else
			pb_uint(&br, 3, i == 0 ? 32 : 1500);
		pb_msg(&strm, 2, &br);
		pb_msg(&strm, 3, &state);
		if (!i)
			pb_uint(&strm, 5, 40);
		pb_uint(&retx, 1, !i);
		if (!i) {
			pb_uint(&retx, 2, 40);
			pb_uint(&retx, 3, 0);
			pb_uint(&retx, 3, 0);
			pb_uint(&retx, 3, 20);
		}
		pb_msg(&strm, 6, &retx);
		if (i == 1) {
			pb_uint(&fec, 1, 0);
			pb_uint(&fec, 1, 1);
			pb_uint(&fec, 1, 10);
		}
		pb_msg(&strm, 7, &fec);
		pb_msg(out, 1, &strm);
	}
	pb_uint(out, 2, 200);
	pb_uint(out, 3, 1);
	pb_uint(out, 5, 1);
	/*
	 * List the data stream too once it exists: a separate media relay
	 * forwards only the streams a report names.
	 */
	for (guint i = 0; i < (data_tx ? 3u : 2u); i++) {
		Pb tx = { 0 }, retx = { 0 };

		pb_uint(&tx, 1, tx_ids[i]);
		if (i == 1 && video_tx && !camera_enabled) {
			Pb paused = { 0 };

			pb_uint(&paused, 1, 1);
			pb_uint(&paused, 2, 0);
			pb_msg(&tx, 2, &paused);
		} else {
			pb_msg(&tx, 2, &state);
		}
		pb_uint(&retx, 1, !i);
		if (!i)
			pb_uint(&retx, 2, 300);
		pb_msg(&tx, 3, &retx);
		pb_msg(out, 6, &tx);
	}
	Pb link = { 0 };

	pb_uint(&link, 1, 886);
	pb_uint(&link, 2, 3000);
	/* Protobuf fixed32 little-endian IEEE float 0.2. */
	const guint8 rate[] = { 0x1d, 0xcd, 0xcc, 0x4c, 0x3e };

	memcpy(link.p + link.n, rate, sizeof(rate));
	link.n += sizeof(rate);
	pb_uint(&link, 4, 200);
	pb_msg(out, 7, &link);
}
