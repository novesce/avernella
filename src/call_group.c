// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Group call field layouts from evex-dev/linejs v3.6.5 (MIT).
 * See analysis/linejs-LICENSE.
 */
#include "call_group.h"
#include <openssl/crypto.h>
#include <string.h>
#include <zlib.h>

gboolean call_group_id(const char *chat)
{
	if (!chat || (chat[0] != 'c' && chat[0] != 'r') || strlen(chat) != 33)
		return FALSE;
	for (const char *id = chat + 1; *id; id++)
		if (!g_ascii_isxdigit(*id))
			return FALSE;
	return TRUE;
}

void call_group_route_clear(struct call_group_route *route)
{
	if (route->token)
		OPENSSL_cleanse(route->token, strlen(route->token));
	g_free(route->token);
	g_free(route->relay);
	g_free(route->relay6);
	g_free(route->i_zone);
	g_free(route->r_zone);
	g_free(route->orion);
	g_free(route->mixer);
	json_object_put(route->members);
	*route = (struct call_group_route){ 0 };
}

static gboolean group_error(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return FALSE;
}

static const char *bounded_field(J *object, const char *name, size_t max)
{
	const char *text = strfield(object, name);

	if (!text || !*text)
		return NULL;
	size_t length = json_object_get_string_len(field(object, name));

	if (length > max || strlen(text) != length)
		return NULL;
	for (size_t i = 0; i < length; i++)
		if ((guint8)text[i] < 33 || (guint8)text[i] > 126)
			return NULL;
	return text;
}

static guint16 route_port(J *object, const char *name)
{
	J *port = field(object, name);

	if (!json_object_is_type(port, json_type_int))
		return 0;
	gint64 value = json_object_get_int64(port);

	return value > 0 && value <= G_MAXUINT16 ? value : 0;
}

static gboolean parse_route(J *reply, struct call_group_route *route,
			    GError **error)
{
	const char *token = bounded_field(reply, "1", 4096);
	const char *relay = bounded_field(reply, "7", 4096);
	const char *relay6 = bounded_field(reply, "16", 4096);
	const char *i_zone = bounded_field(reply, "10", 128);
	const char *r_zone = bounded_field(reply, "14", 128);
	const char *orion = bounded_field(reply, "15", 1024);
	const char *mixer = bounded_field(reply, "12", 1024);
	guint16 relay_port = route_port(reply, "8");
	guint16 mixer_port = route_port(reply, "13");
	const char *config = strfield(reply, "11");
	J *proto = field(reply, "6");
	int version = json_object_get_int(proto);

	if (!token || (!relay && !relay6) || !i_zone || !r_zone || !orion ||
	    !mixer || !relay_port || !mixer_port || !config ||
	    json_object_get_string_len(field(reply, "11")) > 16384 ||
	    (proto && (!json_object_is_type(proto, json_type_int) ||
		       (version != 1 && version != 2))))
		return group_error(
			error,
			"LINE returned an unsupported group call route.");
	g_autoptr(J) params = json_tokener_parse(config);
	const char *mpkey = bounded_field(params, "mpkey", 44);

	if (!mpkey || strlen(mpkey) != 44)
		return group_error(
			error, "LINE did not supply a group call relay key.");
	gsize size;
	g_autofree guint8 *public = g_base64_decode(mpkey, &size);

	if (size != 33 || (public[0] != 2 && public[0] != 3))
		return group_error(
			error,
			"LINE returned an invalid group call relay key.");
	*route = (struct call_group_route){
		.token = g_strdup(token),
		.relay = g_strdup(relay),
		.relay6 = g_strdup(relay6),
		.i_zone = g_strdup(i_zone),
		.r_zone = g_strdup(r_zone),
		.orion = g_strdup(orion),
		.mixer = g_strdup(mixer),
		.relay_port = relay_port,
		.mixer_port = mixer_port,
	};
	memcpy(route->relay_key, public, sizeof(route->relay_key));
	return TRUE;
}

static J *group_state(Line *session, const char *chat, GError **error)
{
	if (!call_group_id(chat)) {
		group_error(error, "Select a LINE group conversation.");
		return NULL;
	}
	g_autoptr(J) args = obj();

	json_object_object_add(args, "2:str", json_object_new_string(chat));
	g_autoptr(J) state =
		line_rpc(session, "/V4", "getGroupCall", args, error);

	if (!state)
		return NULL;
	J *online = field(state, "1");
	const char *room = strfield(state, "2");

	if (!json_object_is_type(online, json_type_boolean) ||
	    (room && strcmp(room, chat))) {
		group_error(error, "LINE returned invalid group call state.");
		return NULL;
	}
	return g_steal_pointer(&state);
}

/* Only the dedicated call roster describes who is in the call. */
static J *member_ids(J *state)
{
	J *list = field(state, "4");

	if (!json_object_get_boolean(field(state, "1")))
		return json_object_new_array();
	if (!json_object_is_type(list, json_type_array))
		return NULL;
	J *members = json_object_new_array();
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	size_t count = MIN(json_object_array_length(list), 500);

	for (size_t i = 0; i < count; i++) {
		J *value = json_object_array_get_idx(list, i);
		const char *id = json_object_get_string(value);

		if (!id || !json_object_is_type(value, json_type_string) ||
		    json_object_get_string_len(value) != 33 ||
		    strlen(id) != 33 || *id != 'u' ||
		    strspn(id + 1, "0123456789abcdefABCDEF") != 32 ||
		    !g_hash_table_add(seen, (gpointer)id))
			continue;
		json_object_array_add(members, json_object_get(value));
	}
	return members;
}

J *call_group_members(Line *session, const char *chat, GError **error)
{
	g_autoptr(J) state = group_state(session, chat, error);

	return state ? member_ids(state) : NULL;
}

gboolean call_group_acquire(Line *session, const char *chat, gboolean video,
			    struct call_group_route *route, GError **error)
{
	g_autoptr(J) state = group_state(session, chat, error);

	if (!state)
		return FALSE;
	g_autoptr(J) args = obj();
	J *capabilities = json_object_new_array();

	json_object_object_add(args, "2:str", json_object_new_string(chat));
	json_object_array_add(capabilities, json_object_new_string("AUDIO"));
	if (video)
		json_object_array_add(capabilities,
				      json_object_new_string("VIDEO"));
	json_object_object_add(args, "3:i32",
			       json_object_new_int(video ? 2 : 1));
	json_object_object_add(args, "4:bool",
			       json_object_new_boolean(!json_object_get_boolean(
				       field(state, "1"))));
	json_object_object_add(args, "5:list:str", capabilities);
	g_autoptr(J) reply =
		line_rpc(session, "/V4", "acquireGroupCallRoute", args, error);

	if (!reply || !parse_route(reply, route, error))
		return FALSE;
	route->members = member_ids(state);
	return TRUE;
}

static void pair(Pb *out, guint field, guint first, guint second)
{
	Pb values = { 0 };

	pb_uint(&values, 1, first);
	pb_uint(&values, 2, second);
	pb_msg(out, field, &values);
}

static void offer_stream(Pb *offer, guint index, gboolean video)
{
	static const char *const labels[] = { "A", "V", "D" };
	static const guint bitrates[] = { 32, 800, 2000 };
	static const guint codecs[] = { 1, CALL_GROUP_VIDEO_SVC, 6 };
	guint receive = 103 + 10 * index, transmit = 203 + 10 * index;
	Pb record = { 0 }, codec = { 0 }, path = { 0 };

	pb_text(&codec, 1, labels[index]);
	pb_uint(&codec, 2, 3);
	pb_uint(&codec, 3, index != 1 || video);
	pb_uint(&codec, 4, bitrates[index]);
	if (index == 1) {
		pb_uint(&codec, 5, 15);
		pb_uint(&codec, 6, 2);
	}
	pb_uint(&codec, 50, codecs[index]);
	if (index == 1) {
		pb_uint(&codec, 50, CALL_GROUP_VIDEO_SVC_SW);
		if (video)
			pb_uint(&codec, 50, CALL_GROUP_VIDEO_VP8);
	}
	pb_uint(&path, 1, 96 + index);
	pb_uint(&path, 11, receive);
	pair(&path, 12, 67, 6803);
	pb_uint(&path, 61, transmit);
	pair(&path, 62, 67, 13603);
	pb_msg(&record, 1, &codec);
	pb_msg(&record, 2, &path);
	if (index != 2) {
		Pb channel = { 0 };

		pb_uint(&channel, 1, 127);
		pb_msg(&channel, 2, &path);
		pb_uint(&channel, 11, receive);
		pb_uint(&channel, 61, transmit);
		pb_msg(&record, 3, &channel);
	}
	if (index != 0)
		pair(&record, 51, 0, 2);
	if (index == 1)
		pair(&record, 51, 2, 1);
	pb_msg(offer, 1, &record);
}

static void group_offer(Pb *offer, const CallInfo *call)
{
	Pb secret = { 0 }, key = { 0 };

	for (guint i = 0; i < 3; i++)
		offer_stream(offer, i, call->video);
	pb_bytes(&key, 1,
		 (Slice){ call->media_secret, sizeof(call->media_secret) });
	pb_msg(&secret, 2, &key);
	pb_msg(offer, 2, &secret);
	pair(offer, 3, 0, 3);
}

static void feature(Pb *out, guint id, gboolean enabled)
{
	Pb spec = { 0 };

	pb_uint(&spec, 1, id);
	pb_uint(&spec, 2, enabled);
	pb_uint(&spec, 3, 0);
	pb_msg(out, 18, &spec);
}

static void credential(const CallInfo *call, guint8 hash[32])
{
	g_autoptr(GChecksum) sha256 = g_checksum_new(G_CHECKSUM_SHA256);
	const char *const parts[] = { call->local, call->target, call->token,
				      call->cid };

	for (guint i = 0; i < G_N_ELEMENTS(parts); i++) {
		if (i)
			g_checksum_update(sha256, (const guint8 *)"::", 2);
		g_checksum_update(sha256, (const guint8 *)parts[i], -1);
	}
	gsize length = 32;

	g_checksum_get_digest(sha256, hash, &length);
}

void call_group_participate(Pb *out, const CallInfo *call,
			    const struct call_group_route *route)
{
	/* Group roster updates use call control. Capabilities 4 and 5 require
	 * on-demand conference-info application streams.
	 */
	static const guint caps[] = { 1, 2, 3, 6 };
	g_autofree char *device =
		g_base64_encode(call->device, sizeof(call->device));
	Pb ua = { 0 }, offer = { 0 }, extra = { 0 };
	guint8 hash[32];

	call_user_agent(&ua);
	group_offer(&offer, call);
	credential(call, hash);

	pb_text(out, 1, call->local);
	pb_text(out, 2, call->target);
	pb_text(out, 3, call->i_zone);
	pb_text(out, 4, call->r_zone);
	pb_text(out, 5, route->orion);
	pb_text(out, 6, route->mixer);
	pb_msg(out, 7, &ua);
	pb_text(out, 8, device);
	pb_uint(out, 9, call->video ? 3 : 1);
	for (guint i = 0; i < G_N_ELEMENTS(caps); i++)
		pb_uint(out, 10, caps[i]);
	pb_msg(out, 11, &offer);
	pb_bytes(out, 12, (Slice){ hash, sizeof(hash) });
	pb_text(out, 13, call->video ? "groupcall.video" : "groupcall.audio");
	pb_uint(out, 14, 1);
	pb_uint(out, 15, call->local_media);
	pb_uint(out, 17, route->mixer_port);
	feature(out, 16, TRUE);
	feature(out, 17, FALSE);
	pb_uint(out, 19, 1);
	/* The conference relay negotiates group media with XRTP reception. */
	pb_uint(out, 20, 2);
	pb_uint(out, 22, 30);
	pb_uint(out, 23, 0);
	pb_uint(&extra, 1, 1);
	pb_msg(out, 53, &extra);
	pb_uint(out, 102, 0);
	OPENSSL_cleanse(hash, sizeof(hash));
	OPENSSL_cleanse(&offer, sizeof(offer));
}

static guint32 feedback_srcid(Slice path, guint direction)
{
	PbField field;

	while (path.n && pb_next(&path, &field)) {
		if (field.field != direction || field.type != 2 ||
		    pb_number(field.data, 1, 0) != 67)
			continue;
		guint64 ssrc = pb_number(field.data, 2, 0);

		return ssrc <= G_MAXUINT32 ? ssrc : 0;
	}
	return 0;
}

static guint first_codec_map(Slice head)
{
	guint64 selected = G_MAXUINT64;
	PbField field;

	while (head.n) {
		if (!pb_next(&head, &field))
			return 0;
		if (field.field != 50)
			continue;
		if (!field.type && field.value <= G_MAXUINT32) {
			if (selected == G_MAXUINT64)
				selected = field.value;
			continue;
		}
		if (field.type != 2)
			return 0;
		guint32 value = 0;
		guint shift = 0;

		for (gsize i = 0; i < field.data.n; i++) {
			guint8 byte = field.data.p[i];

			if (shift == 28 && byte > 15)
				return 0;
			value |= (guint32)(byte & 127) << shift;
			if (byte & 128) {
				shift += 7;
				continue;
			}
			if (selected == G_MAXUINT64)
				selected = value;
			value = 0;
			shift = 0;
		}
		if (shift)
			return 0;
	}
	return selected <= G_MAXUINT ? selected : 0;
}

static gboolean group_video_profile(Slice record, Slice head,
				    struct call_group_answer *answer)
{
	guint64 vp8 = G_MAXUINT64, packet = G_MAXUINT64;
	guint map = first_codec_map(head);
	PbField field;

	if (pb_number(head, 2, 0) != 3 || pb_number(head, 3, 0) > 1 ||
	    (map != CALL_GROUP_VIDEO_VP8 && map != CALL_GROUP_VIDEO_SVC_SW &&
	     map != CALL_GROUP_VIDEO_SVC))
		return FALSE;
	while (record.n) {
		if (!pb_next(&record, &field))
			return FALSE;
		if (field.field != 51)
			continue;
		if (field.type != 2)
			return FALSE;
		guint64 id = pb_number(field.data, 1, G_MAXUINT64);
		guint64 version = pb_number(field.data, 2, 0);

		if (id == 0) {
			if (version > 2 ||
			    (vp8 != G_MAXUINT64 && vp8 != version))
				return FALSE;
			vp8 = version;
		} else if (id == 2) {
			if (version > G_MAXUINT8 ||
			    (packet != G_MAXUINT64 && packet != version))
				return FALSE;
			packet = version;
		}
	}
	if (packet == G_MAXUINT64)
		packet = 0;
	/* The baseline map fixes the codec. An SVC map names a container,
	 * so the final answer must also select a supported layer codec.
	 */
	if (map != CALL_GROUP_VIDEO_VP8 &&
	    ((vp8 != 1 && vp8 != 2) || packet > 1))
		return FALSE;
	answer->video_map = map;
	answer->video_packet_version = packet;
	answer->video_vp8a = map != CALL_GROUP_VIDEO_VP8 && vp8 == 2;
	return TRUE;
}

static gboolean streams(Slice description, struct call_group_answer *answer)
{
	guint32 *const rx[] = { &answer->audio_rx, &answer->video_rx,
				&answer->data_rx };
	guint32 *const tx[] = { &answer->audio_tx, &answer->video_tx,
				&answer->data_tx };
	guint8 *const payload[] = { &answer->audio_payload,
				    &answer->video_payload,
				    &answer->data_payload };
	guint32 feedback_rx[2] = { 0 }, feedback_tx[2] = { 0 };
	PbField field;

	while (description.n) {
		if (!pb_next(&description, &field))
			return FALSE;
		if (field.type != 2)
			continue;
		if (field.field == 3 && !pb_number(field.data, 1, 0)) {
			guint64 version = pb_number(field.data, 2, 0);

			if (!answer->feedback_version && version <= G_MAXUINT)
				answer->feedback_version = version;
		}
		if (field.field == 2) {
			Slice secret = pb_data(pb_data(field.data, 2), 1);

			if (secret.n) {
				if (answer->secret.n || secret.n != 30)
					return FALSE;
				answer->secret = secret;
			}
		}
		if (field.field != 1)
			continue;
		Slice head = pb_data(field.data, 1);
		Slice label = pb_data(head, 1);

		if (label.n != 1 || !*label.p)
			continue;
		static const char labels[] = "AVD";
		const char *match = strchr(labels, *label.p);

		if (!match)
			continue;
		size_t index = match - labels;
		Slice path = pb_data(field.data, 2);
		guint64 from = pb_number(path, 11, 0);
		guint64 to = pb_number(path, 61, 0);
		guint64 pt = pb_number(path, 1, 0);

		if (*rx[index] || !from || !to || from > G_MAXUINT32 ||
		    to > G_MAXUINT32 || from == to || pt < 96 || pt > 127)
			return FALSE;
		*rx[index] = from;
		*tx[index] = to;
		*payload[index] = pt;
		if (index == 1)
			answer->video_supported =
				group_video_profile(field.data, head, answer);
		if (index < G_N_ELEMENTS(feedback_rx)) {
			feedback_rx[index] = feedback_srcid(path, 12);
			feedback_tx[index] = feedback_srcid(path, 62);
		} else {
			answer->data_supported = pb_number(head, 2, 0) == 3 &&
						 pb_number(head, 3, 0) == 1 &&
						 first_codec_map(head) == 6;
		}
	}
	answer->feedback_rx = feedback_rx[0] ? feedback_rx[0] : feedback_rx[1];
	answer->feedback_tx = feedback_tx[0] ? feedback_tx[0] : feedback_tx[1];
	return answer->audio_rx && answer->secret.n == 30;
}

enum call_group_result call_group_answer(Slice reply,
					 struct call_group_answer *answer)
{
	gboolean result_seen = FALSE;
	guint64 result = 0, release = 0;
	PbField field;

	*answer = (struct call_group_answer){ 0 };
	while (reply.n) {
		if (!pb_next(&reply, &field))
			return CALL_GROUP_INVALID;
		switch (field.field) {
		case 1:
			if (result_seen || field.type != 0)
				return CALL_GROUP_INVALID;
			result_seen = TRUE;
			result = field.value;
			break;
		case 2:
			if (field.type != 0)
				return CALL_GROUP_INVALID;
			release |= field.value;
			break;
		case 6:
			if (answer->description.p || field.type != 2)
				return CALL_GROUP_INVALID;
			answer->description = field.data;
			break;
		case 7:
			if (answer->media_channel || field.type != 0)
				return CALL_GROUP_INVALID;
			answer->media_channel = field.value;
			break;
		case 101:
			if (answer->bridge.p || field.type != 2)
				return CALL_GROUP_INVALID;
			answer->bridge = field.data;
			break;
		case 102:
			if (field.type != 0 || field.value > G_MAXUINT)
				return CALL_GROUP_INVALID;
			answer->keepalive = field.value;
			break;
		}
	}
	if (release || result)
		return CALL_GROUP_REJECTED;
	if (!result_seen && !answer->description.n)
		return CALL_GROUP_PENDING;
	if (!result_seen || !answer->media_channel || !answer->description.n ||
	    !streams(answer->description, answer))
		return CALL_GROUP_INVALID;
	return CALL_GROUP_JOINED;
}

void call_group_feedback_init(struct call_group_feedback *feedback,
			      const struct call_group_answer *answer)
{
	memset(feedback, 0, sizeof(*feedback));
	if (answer->feedback_version == 3 && answer->feedback_rx) {
		feedback->ssrc = answer->feedback_rx;
		feedback->interval_ms = 200;
	}
}

static struct call_group_feedback_stream *
feedback_stream(struct call_group_feedback *feedback, guint32 ssrc)
{
	struct call_group_feedback_stream *oldest = &feedback->stream[0];

	for (guint i = 0; i < G_N_ELEMENTS(feedback->stream); i++) {
		struct call_group_feedback_stream *stream =
			&feedback->stream[i];

		if (stream->ssrc == ssrc)
			return stream;
		if (stream->touched_us < oldest->touched_us)
			oldest = stream;
	}
	if (!oldest->ssrc)
		feedback->active_streams++;
	memset(oldest, 0, sizeof(*oldest));
	oldest->ssrc = ssrc;
	return oldest;
}

void call_group_feedback_forget(struct call_group_feedback *feedback,
				guint32 ssrc)
{
	if (!ssrc)
		return;
	for (guint i = 0; i < G_N_ELEMENTS(feedback->stream); i++) {
		if (feedback->stream[i].ssrc != ssrc)
			continue;
		memset(&feedback->stream[i], 0, sizeof(feedback->stream[i]));
		feedback->active_streams--;
		return;
	}
}

static guint feedback_slot(guint16 sequence)
{
	return sequence % CALL_GROUP_FEEDBACK_WINDOW;
}

void call_group_feedback_receive(struct call_group_feedback *feedback,
				 guint32 ssrc, guint16 sequence,
				 guint32 timestamp G_GNUC_UNUSED,
				 gint64 received_us)
{
	struct call_group_feedback_stream *stream;
	int offset;

	if (!feedback->interval_ms || !ssrc || received_us <= 0)
		return;
	if (!feedback->epoch_us)
		feedback->epoch_us = received_us;
	if (!feedback->active_streams)
		feedback->previous_ms =
			(received_us - feedback->epoch_us) / 1000;
	stream = feedback_stream(feedback, ssrc);
	if (!stream->touched_us)
		stream->first = sequence;
	offset = (gint16)(sequence - stream->first);
	if (offset < 0) {
		if (stream->reported ||
		    stream->count - offset > CALL_GROUP_FEEDBACK_WINDOW)
			return;
		stream->first = sequence;
		stream->count -= offset;
		offset = 0;
	}
	if (offset >= CALL_GROUP_FEEDBACK_WINDOW) {
		guint advance = offset - CALL_GROUP_FEEDBACK_WINDOW + 1;
		guint drop = MIN(advance, stream->count);

		for (guint i = 0; i < drop; i++)
			stream->arrival_us[feedback_slot(stream->first + i)] =
				0;
		stream->first += advance;
		stream->count -= drop;
		offset -= advance;
	}
	if (stream->arrival_us[feedback_slot(sequence)])
		return;
	stream->arrival_us[feedback_slot(sequence)] = received_us;
	stream->count = MAX(stream->count, offset + 1);
	stream->touched_us = received_us;
}

static void feedback_write16(guint8 *dst, guint16 value)
{
	value = GUINT16_TO_BE(value);
	memcpy(dst, &value, sizeof(value));
}

static void feedback_write32(guint8 *dst, guint32 value)
{
	value = GUINT32_TO_BE(value);
	memcpy(dst, &value, sizeof(value));
}

static guint32 feedback_read32(const guint8 *src)
{
	guint32 value;

	memcpy(&value, src, sizeof(value));
	return GUINT32_FROM_BE(value);
}

guint call_group_retransmit_request(Slice packet, guint32 *target)
{
	guint count, size;
	guint32 ssrc;

	if (!target || !packet.p || packet.n < 16 ||
	    (packet.p[0] & 0xdf) != 0x85 || packet.p[1] != 205 ||
	    4u * (1 + ((guint)packet.p[2] << 8) + packet.p[3]) != packet.n)
		return 0;
	count = ((guint)packet.p[12] << 8) | packet.p[13];
	size = 14 + 2 * count;
	/* RRR aligns its sequence list to a word and marks two padding bytes.
	 * The native writer leaves the alignment bytes unspecified.
	 */
	if (!count || ((size + 3) & ~3u) != packet.n ||
	    !!(packet.p[0] & 0x20) != !!(size & 3))
		return 0;
	ssrc = feedback_read32(packet.p + 8);
	if (!ssrc)
		return 0;
	*target = ssrc;
	return count;
}

gsize call_group_feedback_control(struct call_group_feedback *feedback,
				  Slice packet, gint64 received_us,
				  gint64 now_us, guint8 response[24],
				  gboolean *interval_changed)
{
	const guint8 *ping = NULL;
	guint interval = feedback->interval_ms;
	gsize offset = 12;
	guint field;

	*interval_changed = FALSE;
	if (!feedback->ssrc || packet.n < 16 || (packet.p[0] & 0xdf) != 0x8c ||
	    packet.p[1] != 205 ||
	    4u * (1 + ((guint)packet.p[2] << 8) + packet.p[3]) != packet.n ||
	    feedback_read32(packet.p + 4) != feedback->ssrc ||
	    feedback_read32(packet.p + 8) != feedback->ssrc)
		return 0;
	do {
		if (packet.n - offset < 2)
			return 0;
		field = packet.p[offset] | ((guint)packet.p[offset + 1] << 8);
		guint type = (field >> 1) & 31;
		guint bytes = field >> 6;

		offset += 2;
		if (bytes > packet.n - offset)
			return 0;
		const guint8 *data = packet.p + offset;

		if (type == 1) {
			if (bytes != 2)
				return 0;
			interval = ((guint)data[0] << 8) | data[1];
			if (interval)
				interval = CLAMP(interval, 20u, 2000u);
		} else if (type == 3) {
			if (bytes != 4)
				return 0;
			ping = data;
		}
		offset += bytes;
	} while (field & 1);
	/* CCFS also permits alignment bytes without an RTCP padding count. */
	if (packet.n - offset > 3 ||
	    (!!(packet.n - offset) != !!(packet.p[0] & 0x20)))
		return 0;
	if (interval != feedback->interval_ms) {
		feedback->interval_ms = interval;
		*interval_changed = TRUE;
		if (!interval) {
			memset(feedback->stream, 0, sizeof(feedback->stream));
			feedback->next_stream = 0;
			feedback->active_streams = 0;
		}
	}
	if (!ping || received_us <= 0 || now_us < received_us)
		return 0;
	response[0] = 0xac;
	response[1] = 205;
	feedback_write16(response + 2, 5);
	feedback_write32(response + 4, feedback->ssrc);
	feedback_write32(response + 8, feedback->ssrc);
	response[12] = 8;
	response[13] = 2;
	memcpy(response + 14, ping, 4);
	feedback_write32(response + 18,
			 MIN((now_us - received_us) / 1000, G_MAXUINT32));
	response[22] = 0;
	response[23] = 2;
	return 24;
}

static guint feedback_delay(guint8 *packet, gint64 ms)
{
	if (ms >= 0 && ms < 64) {
		packet[0] = ((ms & 15) << 4) | ((ms & 16) >> 1) |
			    ((ms & 32) >> 3);
		return 1;
	}
	guint magnitude = MIN(ms < 0 ? -ms : ms, 4095);

	packet[0] = 2 | (ms < 0 ? 8 : 0) | ((magnitude >> 8) << 4);
	packet[1] = magnitude;
	return 2;
}

/* CCFS v3 uses RTPFB format 11. Each source block describes one
 * sequence range, with millisecond arrival deltas and explicit gaps.
 */
gsize call_group_feedback_report(struct call_group_feedback *feedback,
				 gint64 now_us, guint8 *packet, gsize capacity)
{
	const guint header = 20, block_header = 8;
	gsize size = header;
	guint start = feedback->next_stream;
	gint64 now_ms;

	if (!feedback->interval_ms || !feedback->active_streams ||
	    now_us < feedback->epoch_us || capacity < header + block_header)
		return 0;
	now_ms = (now_us - feedback->epoch_us) / 1000;
	if (now_ms < feedback->previous_ms)
		return 0;
	for (guint i = 0; i < G_N_ELEMENTS(feedback->stream); i++) {
		guint index = (start + i) % G_N_ELEMENTS(feedback->stream);
		struct call_group_feedback_stream *stream =
			&feedback->stream[index];
		guint8 entries[CALL_GROUP_FEEDBACK_WINDOW * 2];
		gint64 previous = feedback->previous_ms;
		guint16 first = stream->first;
		guint used = 0;

		if (!stream->ssrc)
			continue;
		for (guint j = 0; j < stream->count; j++) {
			gint64 arrival = stream->arrival_us[feedback_slot(
				stream->first + j)];

			if (!arrival) {
				entries[used++] = 1;
				continue;
			}
			gint64 time = (arrival - feedback->epoch_us) / 1000;

			used += feedback_delay(entries + used, time - previous);
			previous = time;
		}
		guint padded = (used + 3) & ~3u;

		if (block_header + padded > capacity - size) {
			if (size != header)
				break;
			continue;
		}
		feedback_write32(packet + size, stream->ssrc);
		feedback_write16(packet + size + 4, stream->count);
		/* A quiet source repeats its last reported sequence. */
		if (!stream->count)
			first--;
		feedback_write16(packet + size + 6, first);
		size += block_header;
		if (stream->count) {
			memcpy(packet + size, entries, used);
			memset(packet + size + used, 0, padded - used);
			size += padded;
			stream->first += stream->count;
			stream->reported = TRUE;
			stream->count = 0;
			memset(stream->arrival_us, 0,
			       sizeof(stream->arrival_us));
		}
		feedback->next_stream =
			(index + 1) % G_N_ELEMENTS(feedback->stream);
	}
	if (size == header)
		return 0;
	packet[0] = 0x8b;
	packet[1] = 205;
	feedback_write16(packet + 2, size / 4 - 1);
	/* The relay addresses the receive feedback agent by its negotiated
	 * source ID in both header slots. RTP sources are named per block.
	 */
	feedback_write32(packet + 4, feedback->ssrc);
	feedback_write32(packet + 8, feedback->ssrc);
	feedback_write32(packet + 12, now_ms);
	feedback_write16(packet + 16, ++feedback->sequence);
	feedback_write16(packet + 18, now_ms - feedback->previous_ms);
	feedback->previous_ms = now_ms;
	return size;
}

enum {
	GROUP_CONTENT_LIMIT = 64 * 1024,
	GROUP_MEMBER_LIMIT = 512,
	GROUP_CONFERENCE_INFO = 1,
	GROUP_CONFERENCE_MEMBERS = 5,
	GROUP_CHANNEL_INFO = 21,
	GROUP_CHANNEL_MEMBERS = 23,
};

static gboolean member_id(Slice id, char out[34])
{
	if (id.n != 33 || *id.p != 'u')
		return FALSE;
	for (gsize i = 1; i < id.n; i++)
		if (!g_ascii_isxdigit(id.p[i]))
			return FALSE;
	memcpy(out, id.p, id.n);
	out[id.n] = '\0';
	return TRUE;
}

static gboolean member_source(Slice body, struct call_group_member *member)
{
	Slice name = { 0 };
	guint32 source = 0;
	PbField field;

	while (body.n) {
		if (!pb_next(&body, &field))
			return FALSE;
		if (field.field == 1) {
			if (field.type != 2 || name.p)
				return FALSE;
			name = field.data;
		} else if (field.field == 2) {
			if (field.type != 0 || field.value > G_MAXUINT32)
				return FALSE;
			source = field.value;
		}
	}
	if (name.n == 1 && (*name.p == 'V' || *name.p == 'D')) {
		guint32 *ssrc = *name.p == 'V' ? &member->ssrc :
						 &member->data_ssrc;

		if (*ssrc && source && *ssrc != source)
			return FALSE;
		*ssrc = source;
	}
	return TRUE;
}

static gboolean presence_member(Slice body, gboolean subgroup,
				struct call_group_member *member)
{
	const guint source_field = subgroup ? 11 : 10;
	guint state = 0;
	Slice id = { 0 };
	PbField field;

	while (body.n) {
		if (!pb_next(&body, &field))
			return FALSE;
		if (field.field == 1) {
			if (id.p || field.type != 2)
				return FALSE;
			id = field.data;
		} else if (field.field == 3) {
			if (field.type != 0 || field.value > (subgroup ? 2 : 1))
				return FALSE;
			state = field.value;
		} else if (field.field == source_field) {
			if (field.type != 2 ||
			    !member_source(field.data, member))
				return FALSE;
		}
	}
	/* Media capability flags describe the participant's call mode. Camera
	 * subscriptions use the named source of a connected member instead.
	 */
	member_id(id, member->mid);
	member->active = member->ssrc && state;
	if (!state)
		member->data_ssrc = 0;
	return TRUE;
}

static gboolean presence_contents(Slice content, guint type,
				  struct call_group_presence *presence)
{
	g_autoptr(GArray) members =
		g_array_new(FALSE, FALSE, sizeof(struct call_group_member));
	gboolean subgroup = type == GROUP_CHANNEL_INFO ||
			    type == GROUP_CHANNEL_MEMBERS;
	guint member_field = type == GROUP_CONFERENCE_INFO ? 50 : 1;
	guint channel_field = 0, version_field = 0;
	PbField field;

	if (type == GROUP_CONFERENCE_INFO) {
		version_field = 2;
	} else if (type == GROUP_CHANNEL_INFO) {
		channel_field = 2;
		version_field = 1;
		member_field = 11;
	} else if (type == GROUP_CHANNEL_MEMBERS) {
		channel_field = 1;
		member_field = 2;
	}
	gboolean channel_seen = FALSE;

	while (content.n) {
		if (!pb_next(&content, &field))
			return FALSE;
		if (field.field == member_field) {
			struct call_group_member member = {
				.channel = presence->channel,
			};

			if (field.type != 2 ||
			    presence->wire_members >= GROUP_MEMBER_LIMIT ||
			    !presence_member(field.data, subgroup, &member))
				return FALSE;
			presence->wire_members++;
			if (member.mid[0])
				g_array_append_val(members, member);
			else
				presence->unmapped_cameras += member.active;
		} else if (field.field == channel_field) {
			if (channel_seen || field.type != 0 ||
			    field.value != presence->channel)
				return FALSE;
			channel_seen = TRUE;
		} else if (field.field == version_field) {
			if (field.type != 0 || field.value > G_MAXUINT32)
				return FALSE;
			presence->version = field.value;
		} else if (type == GROUP_CONFERENCE_INFO && field.field == 1) {
			if (field.type != 0 || field.value > 1)
				return FALSE;
			presence->full = field.value;
		} else if (version_field && field.field == 3 &&
			   field.type == 2) {
			guint64 count = pb_number(field.data, 1, 0);

			if (count <= G_MAXUINT32)
				presence->reported_members = count;
		}
	}
	if (subgroup && !channel_seen)
		return FALSE;
	presence->members = g_steal_pointer(&members);
	return TRUE;
}

gboolean call_group_presence(Slice body, guint choice,
			     struct call_group_presence *out)
{
	guint type_field, content_field, compression_field, channel_field = 0;
	struct call_group_presence presence = { 0 };
	guint64 type = 0, compression = 0;
	Slice contents = { 0 };
	PbField field;

	*out = presence;
	switch (choice) {
	case CALL_GROUP_PARTICIPATE_RSP:
		type_field = 8;
		content_field = 9;
		compression_field = 12;
		break;
	case CALL_GROUP_PUSH_REQ:
		type_field = 1;
		content_field = 2;
		compression_field = 3;
		break;
	case CALL_GROUP_SUBPUSH_REQ:
		channel_field = 1;
		type_field = 2;
		content_field = 11;
		compression_field = 3;
		break;
	default:
		return FALSE;
	}
	while (body.n) {
		if (!pb_next(&body, &field))
			return FALSE;
		if (field.field == content_field) {
			if (field.type != 2 || contents.p)
				return FALSE;
			contents = field.data;
		} else if (field.field == type_field) {
			if (field.type != 0 || type)
				return FALSE;
			type = field.value;
		} else if (field.field == compression_field) {
			if (field.type != 0 || compression)
				return FALSE;
			compression = field.value;
		} else if (field.field == channel_field) {
			if (field.type != 0 || presence.channel ||
			    !field.value || field.value > G_MAXUINT32)
				return FALSE;
			presence.channel = field.value;
		}
	}
	if (channel_field) {
		if (!presence.channel || (type != GROUP_CHANNEL_INFO &&
					  type != GROUP_CHANNEL_MEMBERS))
			return FALSE;
	} else if (type != GROUP_CONFERENCE_INFO &&
		   type != GROUP_CONFERENCE_MEMBERS) {
		return FALSE;
	}
	if (!contents.n || contents.n > GROUP_CONTENT_LIMIT || compression > 1)
		return FALSE;
	g_autofree guint8 *inflated = NULL;

	if (compression == 1) {
		uLongf size = GROUP_CONTENT_LIMIT;
		uLong used = contents.n;

		inflated = g_malloc(size);
		if (uncompress2(inflated, &size, contents.p, &used) != Z_OK ||
		    used != contents.n || !size)
			return FALSE;
		contents = (Slice){ inflated, size };
	}
	if (!presence_contents(contents, type, &presence))
		return FALSE;
	*out = presence;
	return TRUE;
}

void call_group_stream_request(Pb *out, const char *mid, guint32 ssrc,
			       guint32 channel, gboolean start,
			       guint32 sequence)
{
	Pb request = { 0 }, layer = { 0 };
	char checked[34];

	if (!mid || !ssrc || !sequence ||
	    !member_id((Slice){ (const guint8 *)mid, strlen(mid) }, checked)) {
		out->overflow = TRUE;
		return;
	}
	/* The native request addresses a stream by its source and channel. */
	pb_uint(&request, 1, !!start);
	pb_uint(&request, 3, ssrc);
	pb_uint(&request, 5, start ? 1 : 2);
	pb_uint(&request, 6, 0);
	pb_uint(&request, 7, channel);
	if (start) {
		pb_uint(&layer, 1, 2); /* VGA grid layer. */
		pb_uint(&layer, 2, 0); /* VP8. */
		pb_msg(&request, 8, &layer);
	}
	pb_msg(out, 1, &request);
	pb_uint(out, 2, sequence);
	pb_uint(out, 3, 1); /* Grid view. */
}

static gboolean camera_layer(Slice layer, gboolean vp8a, guint *requested,
			     guint *matching)
{
	PbField field;
	int resolution = -1;
	guint codec = 0; /* VP8 is the default in strm_layer_info. */

	while (layer.n) {
		if (!pb_next(&layer, &field))
			return FALSE;
		switch (field.field) {
		case 1:
			if (field.type || field.value > 4)
				return FALSE;
			resolution = field.value;
			break;
		case 2:
			if (field.type || field.value > 3)
				return FALSE;
			codec = field.value;
			break;
		}
	}
	if (resolution >= 0) {
		*requested |= 1u << resolution;
		if (codec == (guint)vp8a)
			*matching |= 1u << resolution;
	}
	return TRUE;
}

int call_group_camera_resolution(Slice request, guint32 local_ssrc,
				 gboolean vp8a, guint *requested)
{
	guint action = 0, source = 0, operation = 0;
	guint classes = 0, matching = 0, layers = 0;
	PbField field;

	*requested = 0;
	while (request.n) {
		if (!pb_next(&request, &field))
			return CALL_GROUP_LAYER_UNCHANGED;
		if (field.field == 8) {
			if (field.type != 2)
				return CALL_GROUP_LAYER_UNCHANGED;
			/* The conference stream request carries up to six pairs. */
			if (layers++ < 6 && !camera_layer(field.data, vp8a,
							  &classes, &matching))
				return CALL_GROUP_LAYER_UNCHANGED;
			continue;
		}
		if (field.field == 2) {
			if (field.type != 2)
				return CALL_GROUP_LAYER_UNCHANGED;
			continue;
		}
		if (field.field > 7)
			continue;
		if (field.type || field.value > G_MAXUINT32)
			return CALL_GROUP_LAYER_UNCHANGED;
		switch (field.field) {
		case 1:
			action = field.value;
			break;
		case 3:
			source = field.value;
			break;
		case 4:
			if (field.value > 4)
				return CALL_GROUP_LAYER_UNCHANGED;
			break;
		case 5:
			operation = field.value;
			break;
		case 6:
			if (field.value > 2)
				return CALL_GROUP_LAYER_UNCHANGED;
			break;
		}
	}
	if (!local_ssrc || source != local_ssrc || action != 1)
		return CALL_GROUP_LAYER_UNCHANGED;
	*requested = classes;
	if (operation == 2) /* NO_RESOLUTION clears the layer list. */
		return CALL_GROUP_LAYER_CLEAR;
	/* UPDATE_RESOLUTION uses layer_info. */
	if (operation || !matching)
		return CALL_GROUP_LAYER_UNCHANGED;
	/* A single base layer can serve every tile at the smallest requested size. */
	return g_bit_nth_lsf(matching, -1);
}

static gboolean stream_update(Slice body, struct call_group_stream *stream)
{
	PbField field;

	while (body.n) {
		if (!pb_next(&body, &field))
			return FALSE;
		if (field.field == 2) {
			if (field.type != 2 || stream->mid[0] ||
			    !member_id(field.data, stream->mid))
				return FALSE;
			continue;
		}
		if (field.field != 1 && field.field != 3 && field.field != 5)
			continue;
		if (field.type != 0 || field.value > G_MAXUINT32)
			return FALSE;
		if (field.field == 1) {
			if (field.value > CALL_GROUP_STREAM_RESUME)
				return FALSE;
			stream->state = field.value;
		} else if (field.field == 3) {
			stream->ssrc = field.value;
		} else {
			stream->channel = field.value;
		}
	}
	return stream->ssrc != 0;
}

GArray *call_group_stream_updates(Slice body, guint choice)
{
	gboolean response = choice == CALL_GROUP_STRM_RSP;
	guint stream_field = response ? 4 : 1;
	PbField field;

	if (!response && choice != CALL_GROUP_NOTIFY_REQ)
		return NULL;
	g_autoptr(GArray) streams =
		g_array_new(FALSE, FALSE, sizeof(struct call_group_stream));

	while (body.n) {
		if (!pb_next(&body, &field))
			return NULL;
		if (response && (field.field == 1 || field.field == 2) &&
		    (field.type != 0 || field.value))
			return NULL;
		if (field.field != stream_field)
			continue;
		struct call_group_stream stream = { 0 };

		if (field.type != 2 || streams->len >= GROUP_MEMBER_LIMIT ||
		    !stream_update(field.data, &stream))
			return NULL;
		g_array_append_val(streams, stream);
	}
	return g_steal_pointer(&streams);
}

/* The server assigned these receive and transmit IDs in the final answer. */
static void stream_report(Pb *out, const struct call_group_answer *answer,
			  gboolean video_paused)
{
	const guint32 rx[] = { answer->audio_rx, answer->video_rx,
			       answer->data_rx };
	const guint32 tx[] = { answer->audio_tx, answer->video_tx };
	Pb state = { 0 }, paused = { 0 }, link = { 0 };

	pb_uint(&state, 1, 0);
	pb_uint(&state, 2, 0);
	pb_uint(&paused, 1, 1);
	pb_uint(&paused, 2, 0);
	for (guint i = 0; i < G_N_ELEMENTS(rx); i++) {
		Pb stream = { 0 }, bitrate = { 0 }, retx = { 0 }, fec = { 0 };

		if (!rx[i])
			continue;
		pb_uint(&stream, 1, rx[i]);
		if (i == 1) {
			pb_uint(&bitrate, 1, 100);
			pb_uint(&bitrate, 2, 1200);
		} else if (i == 2) {
			pb_uint(&bitrate, 2, 2000);
		}
		if (i != 2)
			pb_uint(&bitrate, 3, i == 0 ? 32 : 800);
		pb_msg(&stream, 2, &bitrate);
		pb_msg(&stream, 3, &state);
		pb_uint(&retx, 1, i == 0);
		if (i == 0) {
			pb_uint(&stream, 5, 40);
			pb_uint(&retx, 2, 40);
			pb_uint(&retx, 3, 0);
			pb_uint(&retx, 3, 0);
			pb_uint(&retx, 3, 20);
		} else if (i == 1) {
			pb_uint(&fec, 1, 0);
			pb_uint(&fec, 1, 1);
			pb_uint(&fec, 1, 10);
		}
		pb_msg(&stream, 6, &retx);
		pb_msg(&stream, 7, &fec);
		pb_msg(out, 1, &stream);
	}
	pb_uint(out, 2, 200);
	pb_uint(out, 3, 1);
	pb_uint(out, 5, 1);
	for (guint i = 0; i < G_N_ELEMENTS(tx); i++) {
		Pb stream = { 0 }, retx = { 0 };

		if (!tx[i])
			continue;
		pb_uint(&stream, 1, tx[i]);
		pb_msg(&stream, 2, i == 1 && video_paused ? &paused : &state);
		pb_uint(&retx, 1, i == 0);
		if (i == 0)
			pb_uint(&retx, 2, 300);
		pb_msg(&stream, 3, &retx);
		pb_msg(out, 6, &stream);
	}
	pb_uint(&link, 1, 1200);
	pb_uint(&link, 2, 3000);
	/* Field 3, fixed32 little-endian IEEE float 0.2. */
	static const guint8 probe[] = { 0x1d, 0xcd, 0xcc, 0x4c, 0x3e };

	memcpy(link.p + link.n, probe, sizeof(probe));
	link.n += sizeof(probe);
	pb_uint(&link, 4, 200);
	pb_msg(out, 7, &link);
}

void call_group_register(Pb *out, const struct call_group_answer *answer,
			 gboolean camera_active)
{
	Pb report = { 0 };
	guint8 data[sizeof(report.p) + 11] = { 0, 2 };

	stream_report(&report, answer,
		      !answer->video_supported || !camera_active);
	if (report.overflow) {
		out->overflow = TRUE;
		return;
	}
	data[4] = report.n >> 8;
	data[5] = report.n & 255;
	memcpy(data + 8, report.p, report.n);
	pb_uint(out, 1, 0);
	pb_uint(out, 2, 0);
	pb_uint(out, 3, 2);
	pb_bytes(out, 4, (Slice){ data, 8 + ((report.n + 3) & ~3u) });
}

void call_group_media_key(const char *kind, const guint8 secret[30],
			  guint8 out[30])
{
	g_autoptr(GHmac) hmac = g_hmac_new(G_CHECKSUM_SHA256,
					   (const guint8 *)kind, strlen(kind));
	guint8 digest[32];
	gsize length = sizeof(digest);

	g_hmac_update(hmac, secret, 30);
	g_hmac_get_digest(hmac, digest, &length);
	memcpy(out, digest, 30);
	OPENSSL_cleanse(digest, sizeof(digest));
}
