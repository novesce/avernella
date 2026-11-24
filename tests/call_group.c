// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_group.h"
#include <string.h>
#include <zlib.h>

static const char chat[] = "c00000000000000000000000000000001";
static const char user[] = "u00000000000000000000000000000000";
static const char guest[] = "u00000000000000000000000000000003";
static gboolean online, video, denied, invalid_port;
static guint requests;

J *line_rpc(Line *session, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)session;
	g_assert_cmpstr(path, ==, "/V4");
	g_assert_cmpstr(strfield(args, "2:str"), ==, chat);
	if (++requests == 1) {
		g_assert_cmpstr(method, ==, "getGroupCall");
		g_assert_cmpuint(json_object_object_length(args), ==, 1);
		if (denied) {
			g_set_error_literal(error, LINE_ERROR,
					    LINE_ACCESS_DENIED,
					    "Fixture access refused.");
			return NULL;
		}
		J *reply = obj();

		json_object_object_add(reply, "1",
				       json_object_new_boolean(online));
		json_object_object_add(reply, "2",
				       json_object_new_string(chat));
		if (online) {
			J *members = json_object_new_array();

			json_object_array_add(members,
					      json_object_new_string(user));
			json_object_array_add(members,
					      json_object_new_string(guest));
			json_object_array_add(members,
					      json_object_new_string(user));
			json_object_array_add(members,
					      json_object_new_string(chat));
			json_object_array_add(members, json_object_new_int(42));
			json_object_object_add(reply, "4", members);
		}
		return reply;
	}
	g_assert_cmpuint(requests, ==, 2);
	g_assert_cmpstr(method, ==, "acquireGroupCallRoute");
	g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
			video ? 2 : 1);
	g_assert_cmpint(json_object_get_boolean(field(args, "4:bool")), ==,
			!online);
	J *caps = field(args, "5:list:str");

	g_assert_cmpuint(json_object_array_length(caps), ==, video ? 2 : 1);
	g_assert_cmpstr(
		json_object_get_string(json_object_array_get_idx(caps, 0)), ==,
		"AUDIO");
	if (video)
		g_assert_cmpstr(json_object_get_string(
					json_object_array_get_idx(caps, 1)),
				==, "VIDEO");
	J *reply = json_tokener_parse(
		"{\"1\":\"test-token\",\"6\":1,\"7\":\"relay.example.org\","
		"\"8\":6001,\"10\":\"test-i\","
		"\"11\":\"{\\\"mpkey\\\":\\\"A2sX0fLhLEJH+Lzm5WOkQPJ3A32BLeszoPShOUXYmMKW\\\"}\","
		"\"12\":\"mixer.example.org\",\"13\":6002,\"14\":\"test-r\","
		"\"15\":\"orion.example.org\",\"16\":\"2001:db8::1\"}");

	if (invalid_port)
		json_object_object_add(reply, "8", json_object_new_int(65536));
	return reply;
}

static void route(void)
{
	Line session = { 0 };
	struct call_group_route data = { 0 };
	g_autoptr(GError) error = NULL;

	for (guint i = 0; i < 2; i++) {
		requests = 0;
		online = video = i != 0;
		g_assert_true(call_group_acquire(&session, chat, video, &data,
						 &error));
		g_assert_no_error(error);
		g_assert_cmpuint(requests, ==, 2);
		g_assert_cmpstr(data.token, ==, "test-token");
		g_assert_cmpstr(data.relay, ==, "relay.example.org");
		g_assert_cmpstr(data.relay6, ==, "2001:db8::1");
		g_assert_cmpuint(data.relay_port, ==, 6001);
		g_assert_cmpuint(data.mixer_port, ==, 6002);
		g_assert_cmpuint(data.relay_key[0], ==, 3);
		g_assert_cmpuint(json_object_array_length(data.members), ==,
				 online ? 2 : 0);
		if (online)
			g_assert_cmpstr(json_object_get_string(
						json_object_array_get_idx(
							data.members, 1)),
					==, guest);
		call_group_route_clear(&data);
	}
	requests = 0;
	g_assert_false(
		call_group_acquire(&session, user, FALSE, &data, &error));
	g_assert_cmpuint(requests, ==, 0);
	g_clear_error(&error);

	denied = TRUE;
	g_assert_false(
		call_group_acquire(&session, chat, FALSE, &data, &error));
	g_assert_error(error, LINE_ERROR, LINE_ACCESS_DENIED);
	g_assert_cmpuint(requests, ==, 1);
	g_clear_error(&error);
	denied = FALSE;

	requests = 0;
	video = FALSE;
	invalid_port = TRUE;
	g_assert_false(
		call_group_acquire(&session, chat, FALSE, &data, &error));
	g_assert_nonnull(error);
	g_assert_null(data.token);
}

static void participate(void)
{
	CallInfo call = {
		.local = (char *)user,
		.target = (char *)chat,
		.token = "fixture-token",
		.cid = "fixture-call",
		.i_zone = "test-i",
		.r_zone = "test-r",
		.local_media = 0x7d3158f8,
	};
	struct call_group_route route = {
		.mixer = "mixer.example.org",
		.orion = "orion.example.org",
		.mixer_port = 6002,
	};
	static const guint8 credential[] = {
		0x4f, 0xd9, 0xb7, 0x9b, 0xd2, 0x7d, 0x30, 0x14,
		0x1d, 0x49, 0x58, 0xef, 0xe1, 0x01, 0x72, 0xb9,
		0xb8, 0x67, 0x82, 0x7a, 0x77, 0x2a, 0x6c, 0x9a,
		0x72, 0xc2, 0x11, 0xed, 0x7d, 0x96, 0xae, 0x1e,
	};
	Pb request = { 0 };

	memset(call.media_secret, 7, sizeof(call.media_secret));
	call_group_participate(&request, &call, &route);
	g_assert_false(request.overflow);
	Slice on_wire = PB_SLICE(request);
	Slice room = pb_data(on_wire, 2);
	Slice key = pb_data(on_wire, 12);
	Slice service = pb_data(on_wire, 13);
	Slice offer = pb_data(on_wire, 11);

	g_assert_cmpmem(room.p, room.n, chat, strlen(chat));
	g_assert_cmpmem(key.p, key.n, credential, sizeof(credential));
	g_assert_cmpmem(service.p, service.n, "groupcall.audio", 15);
	g_assert_cmpuint(pb_number(on_wire, 15, 0), ==, call.local_media);
	g_assert_cmpuint(pb_number(on_wire, 17, 0), ==, route.mixer_port);
	g_assert_cmpuint(pb_number(on_wire, 20, 0), ==, 2);
	g_assert_cmpuint(offer.n, ==, 265);
	int streams = 0;
	PbField item;
	Slice fields = on_wire;
	guint caps = 0;

	while (fields.n) {
		g_assert_true(pb_next(&fields, &item));
		if (item.field == 10) {
			g_assert_cmpuint(item.value, !=, 4);
			g_assert_cmpuint(item.value, !=, 5);
			caps++;
		}
		g_assert_cmpuint(item.field, !=, 24);
	}
	g_assert_cmpuint(caps, ==, 4);

	while (offer.n) {
		g_assert_true(pb_next(&offer, &item));
		if (item.field != 1)
			continue;
		Slice codec = pb_data(item.data, 1);
		Slice path = pb_data(item.data, 2);

		g_assert_cmpuint(pb_number(codec, 3, 2), ==, streams != 1);
		g_assert_cmpuint(pb_number(path, 11, 0), ==,
				 103 + 10 * streams);
		g_assert_cmpuint(pb_number(path, 61, 0), ==,
				 203 + 10 * streams);
		streams++;
	}
	g_assert_cmpint(streams, ==, 3);
	request.n = 0;
	call.video = TRUE;
	call_group_participate(&request, &call, &route);
	g_assert_false(request.overflow);
	on_wire = PB_SLICE(request);
	service = pb_data(on_wire, 13);
	g_assert_cmpmem(service.p, service.n, "groupcall.video", 15);
	g_assert_cmpuint(pb_number(on_wire, 9, 0), ==, 3);
	g_assert_cmpuint(pb_number(on_wire, 20, 0), ==, 2);
	guint8 pt = 0;
	guint32 local = 0, remote = 0;

	g_assert_true(call_video_offer_profile(pb_data(on_wire, 11), &pt,
					       &local, &remote));
	g_assert_cmpuint(pb_data(on_wire, 24).n, ==, 0);
	g_assert_cmpuint(pt, ==, 97);
	g_assert_cmpuint(local, ==, 113);
	g_assert_cmpuint(remote, ==, 213);
	static const guint maps[] = { 7, 4, 2 };
	guint count = 0, features_seen = 0;

	offer = pb_data(on_wire, 11);
	while (pb_next(&offer, &item)) {
		Slice codec = pb_data(item.data, 1);
		Slice name = pb_data(codec, 1);
		PbField field;

		if (item.field != 1 || name.n != 1 || *name.p != 'V')
			continue;
		g_assert_cmpuint(pb_number(codec, 4, 0), ==, 800);
		g_assert_cmpuint(pb_number(codec, 5, 0), ==, 15);
		g_assert_cmpuint(pb_number(codec, 6, 0), ==, 2);
		while (pb_next(&codec, &field)) {
			if (field.field != 50)
				continue;
			g_assert_cmpuint(count, <, G_N_ELEMENTS(maps));
			g_assert_cmpuint(field.value, ==, maps[count++]);
		}
		while (pb_next(&item.data, &field)) {
			if (field.field != 51)
				continue;
			guint id = pb_number(field.data, 1, 99);

			g_assert_true(id == 0 || id == 2);
			g_assert_cmpuint(pb_number(field.data, 2, 0), ==,
					 id == 0 ? 2 : 1);
			features_seen |= 1u << id;
		}
	}
	g_assert_cmpuint(count, ==, G_N_ELEMENTS(maps));
	g_assert_cmpuint(features_seen, ==, 5);
}

static void make_answer(Pb *reply, guint media, guint video_map,
			int vp8_version, int packet_version, gboolean packed)
{
	static const char *const label[] = { "A", "V", "D" };
	guint8 key[30];
	Pb offer = { 0 }, security = { 0 }, secret = { 0 }, features = { 0 };

	for (guint i = 0; i < sizeof(key); i++)
		key[i] = i;
	for (guint i = 0; i < G_N_ELEMENTS(label); i++) {
		Pb record = { 0 }, codec = { 0 }, path = { 0 };
		Pb rx_aux = { 0 }, tx_aux = { 0 };

		if (!(media & (1u << i)))
			continue;
		pb_text(&codec, 1, label[i]);
		if (i == 1 && video_map) {
			Pb feature = { 0 }, scoped = { 0 },
			   scoped_secret = { 0 };
			const guint8 scoped_key[30] = { 42 };

			pb_uint(&codec, 2, 3);
			pb_uint(&codec, 3, 1);
			if (packed) {
				const guint8 maps[] = { video_map,
							video_map == 2 ? 7 :
									 2 };

				pb_bytes(&codec, 50,
					 (Slice){ maps, sizeof(maps) });
			} else {
				pb_uint(&codec, 50, video_map);
			}
			if (vp8_version >= 0) {
				pb_uint(&feature, 1, 0);
				pb_uint(&feature, 2, vp8_version);
				pb_msg(&record, 51, &feature);
				feature.n = 0;
			}
			if (packet_version >= 0) {
				pb_uint(&feature, 1, 2);
				pb_uint(&feature, 2, packet_version);
				pb_msg(&record, 51, &feature);
			}
			pb_bytes(&scoped_secret, 1,
				 (Slice){ scoped_key, sizeof(scoped_key) });
			pb_msg(&scoped, 2, &scoped_secret);
			pb_msg(&record, 50, &scoped);
		}
		if (i == 2) {
			const guint8 maps[] = { 6, 2 };

			pb_uint(&codec, 2, 3);
			pb_uint(&codec, 3, 1);
			if (packed)
				pb_bytes(&codec, 50,
					 (Slice){ maps, sizeof(maps) });
			else
				pb_uint(&codec, 50, 6);
		}
		pb_uint(&path, 1, i == 1 && video_map ? 105 : 96 + i);
		pb_uint(&path, 11, 0xe3530179 + 10 * i);
		pb_uint(&rx_aux, 1, 67);
		pb_uint(&rx_aux, 2, 0x05060708 + i);
		pb_msg(&path, 12, &rx_aux);
		pb_uint(&path, 61, 0xe353017d + 10 * i);
		pb_uint(&tx_aux, 1, 67);
		pb_uint(&tx_aux, 2, 0x01020304 + i);
		pb_msg(&path, 62, &tx_aux);
		pb_msg(&record, 1, &codec);
		pb_msg(&record, 2, &path);
		pb_msg(&offer, 1, &record);
	}
	pb_bytes(&secret, 1, (Slice){ key, sizeof(key) });
	pb_msg(&security, 2, &secret);
	pb_msg(&offer, 2, &security);
	pb_uint(&features, 1, 0);
	pb_uint(&features, 2, 3);
	pb_msg(&offer, 3, &features);
	pb_uint(reply, 1, 0);
	pb_msg(reply, 6, &offer);
	pb_uint(reply, 7, 0x12345678);
	pb_uint(reply, 102, 25);
}

static void answer_and_streams(void)
{
	static const guint8 audio_key[] = {
		0x3e, 0xbc, 0x66, 0x2d, 0x4d, 0x81, 0x2b, 0x04, 0xd9, 0x40,
		0x36, 0x25, 0x40, 0xd6, 0x4e, 0xcf, 0x18, 0xbb, 0xcc, 0x2c,
		0xb2, 0x25, 0x3c, 0xf7, 0x22, 0x67, 0x6d, 0x1e, 0x25, 0x54,
	};
	struct call_group_answer answer;
	Pb reply = { 0 };

	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_PENDING);
	pb_uint(&reply, 2, 1);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_REJECTED);
	reply.n = 0;
	make_answer(&reply, 6, 0, -1, 0, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_INVALID);
	reply.n = 0;
	make_answer(&reply, 7, 0, -1, 0, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_JOINED);
	g_assert_cmpuint(answer.media_channel, ==, 0x12345678);
	g_assert_cmpuint(answer.keepalive, ==, 25);
	g_assert_cmpuint(answer.audio_rx, ==, 0xe3530179);
	g_assert_cmpuint(answer.audio_tx, ==, 0xe353017d);
	g_assert_cmpuint(answer.audio_payload, ==, 96);
	g_assert_cmpuint(answer.data_payload, ==, 98);
	g_assert_true(answer.data_supported);
	g_assert_cmpuint(answer.feedback_version, ==, 3);
	g_assert_cmpuint(answer.feedback_tx, ==, 0x01020304);
	g_assert_cmpuint(answer.feedback_rx, ==, 0x05060708);
	g_assert_false(answer.video_supported);
	guint8 key[30];

	call_group_media_key("AUDIO", answer.secret.p, key);
	g_assert_cmpmem(key, sizeof(key), audio_key, sizeof(audio_key));
	Pb registration = { 0 };

	call_group_register(&registration, &answer, FALSE);
	g_assert_false(registration.overflow);
	g_assert_cmpuint(pb_number(PB_SLICE(registration), 3, 0), ==, 2);
	Slice data = pb_data(PB_SLICE(registration), 4);

	g_assert_cmpuint(data.n, >=, 8);
	g_assert_cmpuint(data.n % 4, ==, 0);
	g_assert_cmpuint(data.p[1], ==, 2);
	guint encoded_len = data.p[4] * 256 + data.p[5];

	g_assert_cmpuint(encoded_len, <=, data.n - 8);
	Slice stream_report = { data.p + 8, encoded_len };
	Slice audio_rx = pb_data(stream_report, 1);
	Slice audio_tx = pb_data(stream_report, 6);

	g_assert_cmpuint(pb_number(audio_rx, 1, 0), ==, answer.audio_rx);
	g_assert_cmpuint(pb_number(audio_rx, 5, 0), ==, 40);
	g_assert_cmpuint(pb_number(audio_tx, 1, 0), ==, answer.audio_tx);

	reply.n = registration.n = 0;
	make_answer(&reply, 1, 0, -1, 0, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_JOINED);
	g_assert_cmpuint(answer.video_rx, ==, 0);
	g_assert_cmpuint(answer.data_rx, ==, 0);
	g_assert_false(answer.data_supported);
	call_group_register(&registration, &answer, FALSE);
	data = pb_data(PB_SLICE(registration), 4);
	stream_report = (Slice){ data.p + 8, data.p[4] * 256 + data.p[5] };
	guint count = 0;
	PbField item;

	while (stream_report.n) {
		g_assert_true(pb_next(&stream_report, &item));
		if (item.field == 1 || item.field == 6) {
			g_assert_cmpuint(pb_number(item.data, 1, 0), ==,
					 item.field == 1 ? answer.audio_rx :
							   answer.audio_tx);
			count++;
		}
	}
	g_assert_cmpuint(count, ==, 2);
}

static void retransmit_request(void)
{
	/* Native RRR with two lost sequences, including a wrap, and alignment. */
	guint8 packet[] = {
		0xa5, 205,  0, 4, 0,	0,    0, 0, 0x11, 0x22,
		0x33, 0x44, 0, 2, 0xff, 0xfe, 0, 1, 0,	  0,
	};
	Slice data = { packet, sizeof(packet) };
	guint32 target = 0;

	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 2);
	g_assert_cmphex(target, ==, 0x11223344);
	packet[19] = 2;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 2);
	packet[0] = 0x85;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 0);
	packet[13] = 3;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 3);
	packet[13] = 4;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 0);
	packet[13] = 1;
	packet[3] = 3;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 0);
	data.n = 16;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 1);
	memset(packet + 8, 0, 4);
	target = 1;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 0);
	g_assert_cmpuint(target, ==, 1);
	data.n = 13;
	g_assert_cmpuint(call_group_retransmit_request(data, &target), ==, 0);
}

static void congestion_feedback(void)
{
	static const guint8 report[] = {
		0x8b, 205,  0,	  11,	5,  6,	 7,    8,    5,	   6,
		7,    8,    0,	  0,	0,  200, 0,    1,    0,	   200,
		0x11, 0x22, 0x33, 0x44, 0,  5,	 0xff, 0xfe, 0,	   0x2c,
		0x0a, 10,   1,	  2,	80, 0,	 0xaa, 0xbb, 0xcc, 0xdd,
		0,    1,    0,	  8,	2,  150, 0,    0,
	};
	static const guint8 next_report[] = {
		0x8b, 205, 0, 9, 5,    6,    7,	   8,	 5,    6,    7, 8, 0, 0,
		0,    250, 0, 2, 0,    50,   0x11, 0x22, 0x33, 0x44, 0, 1, 0, 3,
		0x34, 0,   0, 0, 0xaa, 0xbb, 0xcc, 0xdd, 0,    0,    0, 8,
	};
	struct call_group_answer answer = {
		.feedback_version = 3,
		.feedback_tx = 0x01020304,
		.feedback_rx = 0x05060708,
	};
	struct call_group_feedback feedback;
	guint8 packet[256];
	gsize size;

	call_group_feedback_init(&feedback, &answer);
	call_group_feedback_receive(&feedback, 0x11223344, 65534, 0, 1000000);
	call_group_feedback_receive(&feedback, 0x11223344, 0, 0, 1040000);
	call_group_feedback_receive(&feedback, 0x11223344, 65535, 0, 1050000);
	call_group_feedback_receive(&feedback, 0x11223344, 2, 0, 1120000);
	call_group_feedback_receive(&feedback, 0x11223344, 0, 0, 1125000);
	call_group_feedback_receive(&feedback, 0xaabbccdd, 8, 0, 1150000);
	g_assert_cmpuint(call_group_feedback_report(&feedback, 1200000, packet,
						    31),
			 ==, 0);
	size = call_group_feedback_report(&feedback, 1200000, packet,
					  sizeof(packet));
	g_assert_cmpmem(packet, size, report, sizeof(report));
	call_group_feedback_receive(&feedback, 0x11223344, 1, 0, 1230000);
	call_group_feedback_receive(&feedback, 0x11223344, 3, 0, 1235000);
	size = call_group_feedback_report(&feedback, 1250000, packet,
					  sizeof(packet));
	g_assert_cmpmem(packet, size, next_report, sizeof(next_report));

	answer.feedback_version = 1;
	call_group_feedback_init(&feedback, &answer);
	call_group_feedback_receive(&feedback, 0x11223344, 1, 0, 1250000);
	g_assert_cmpuint(call_group_feedback_report(&feedback, 1300000, packet,
						    sizeof(packet)),
			 ==, 0);
}

static void congestion_quiet_window(void)
{
	/* Native report for an established source with no new arrivals. */
	static const guint8 idle[] = {
		0x8b, 205, 0, 6, 5, 6,	 7,    8,    5,	   6,	 7, 8, 0, 0,
		1,    144, 0, 2, 0, 200, 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 55,
	};
	const struct call_group_answer answer = {
		.feedback_version = 3,
		.feedback_rx = 0x05060708,
	};
	struct call_group_feedback feedback;
	guint8 packet[64];
	gint64 now = 1000000;
	gsize size;

	call_group_feedback_init(&feedback, &answer);
	g_assert_cmpuint(call_group_feedback_report(&feedback, now, packet,
						    sizeof(packet)),
			 ==, 0);
	call_group_feedback_receive(&feedback, 0x11223344, 55, 0, now);
	now += 200000;
	size = call_group_feedback_report(&feedback, now, packet,
					  sizeof(packet));
	g_assert_cmpuint(size, ==, 32);
	for (guint i = 0; i < 25; i++) {
		now += 200000;
		size = call_group_feedback_report(&feedback, now, packet,
						  sizeof(idle));
		g_assert_cmpuint(size, ==, sizeof(idle));
		if (!i)
			g_assert_cmpmem(packet, size, idle, sizeof(idle));
		g_assert_cmpuint(packet[17], ==, i + 2);
		g_assert_cmpuint(packet[19], ==, 200);
		g_assert_cmpmem(packet + 20, 8, idle + 20, 8);
	}
	/* After five seconds of silence the next arrival is 37 ms past
	 * the previous window (a short delta), and sequence 56 is contiguous.
	 */
	call_group_feedback_receive(&feedback, 0x11223344, 56, 0, now + 37000);
	now += 200000;
	size = call_group_feedback_report(&feedback, now, packet,
					  sizeof(packet));
	g_assert_cmpuint(size, ==, 32);
	g_assert_cmpuint(packet[17], ==, 27);
	g_assert_cmpuint(packet[25], ==, 1);
	g_assert_cmpuint(packet[27], ==, 56);
	g_assert_cmpuint(packet[28], ==, 0x54);

	call_group_feedback_forget(&feedback, 0x11223344);
	call_group_feedback_forget(&feedback, 0x11223344);
	g_assert_cmpuint(feedback.active_streams, ==, 0);
	g_assert_cmpuint(call_group_feedback_report(&feedback, now, packet,
						    sizeof(packet)),
			 ==, 0);
	/* Replacing the only source after a long pause starts a new window. */
	now += 8000000;
	call_group_feedback_receive(&feedback, 99, 8, 0, now);
	size = call_group_feedback_report(&feedback, now + 200000, packet,
					  sizeof(packet));
	g_assert_cmpuint(size, ==, 32);
	g_assert_cmpuint(packet[19], ==, 200);
	g_assert_cmpuint(packet[23], ==, 99);
	g_assert_cmpuint(packet[28], ==, 0);
}

static void congestion_control(void)
{
	static const guint8 challenge[] = {
		0xac, 205, 0, 4, 5,    6,    7,	   8,	 5, 6,
		7,    8,   6, 1, 0x12, 0x34, 0x56, 0x78, 0, 0,
	};
	static const guint8 echo[] = {
		0xac, 205, 0,	 5,    5,    6,	   7, 8, 5, 6, 7, 8,
		8,    2,   0x12, 0x34, 0x56, 0x78, 0, 0, 0, 2, 0, 2,
	};
	guint8 interval[] = {
		0x8c, 205, 0, 3, 5, 6, 7, 8, 5, 6, 7, 8, 0x82, 0, 0, 80,
	};
	struct call_group_answer answer = {
		.feedback_version = 3,
		.feedback_rx = 0x05060708,
	};
	struct call_group_feedback feedback;
	guint8 response[24];
	gboolean changed;
	gsize size;

	call_group_feedback_init(&feedback, &answer);
	g_assert_cmpuint(feedback.interval_ms, ==, 200);
	size = call_group_feedback_control(
		&feedback, (Slice){ challenge, sizeof(challenge) }, 1000000,
		1002468, response, &changed);
	g_assert_cmpmem(response, size, echo, sizeof(echo));
	g_assert_false(changed);
	size = call_group_feedback_control(
		&feedback, (Slice){ challenge, sizeof(challenge) - 1 }, 1000000,
		1002468, response, &changed);
	g_assert_cmpuint(size, ==, 0);
	g_assert_false(changed);

	/* Both a different ID and a missing second TLV leave the interval intact. */
	for (guint i = 0; i < 2; i++) {
		interval[7] = i ? 8 : 9;
		interval[12] = i ? 0x83 : 0x82;
		size = call_group_feedback_control(
			&feedback, (Slice){ interval, sizeof(interval) },
			1000000, 1002468, response, &changed);
		g_assert_cmpuint(size, ==, 0);
		g_assert_false(changed);
		g_assert_cmpuint(feedback.interval_ms, ==, 200);
	}
	const guint16 requested[] = { 80, 0, 1, G_MAXUINT16 };
	const guint expected[] = { 80, 0, 20, 2000 };

	call_group_feedback_receive(&feedback, 55, 1, 0, 1000000);
	interval[12] = 0x82;
	for (guint i = 0; i < G_N_ELEMENTS(requested); i++) {
		interval[14] = requested[i] >> 8;
		interval[15] = (guint8)requested[i];
		size = call_group_feedback_control(
			&feedback, (Slice){ interval, sizeof(interval) },
			1000000, 1002468, response, &changed);
		g_assert_cmpuint(size, ==, 0);
		g_assert_true(changed);
		g_assert_cmpuint(feedback.interval_ms, ==, expected[i]);
		g_assert_cmpuint(feedback.active_streams, ==, i == 0 ? 1 : 0);
	}
}

static gboolean registered_camera(const Pb *registration,
				  const struct call_group_answer *answer)
{
	Slice data = pb_data(PB_SLICE(*registration), 4);
	PbField field;

	g_assert_false(registration->overflow);
	g_assert_cmpuint(data.n, >=, 8);
	guint length = data.p[4] * 256 + data.p[5];

	g_assert_cmpuint(length, <=, data.n - 8);
	Slice report = { data.p + 8, length };

	while (report.n) {
		g_assert_true(pb_next(&report, &field));
		if (field.field == 6 &&
		    pb_number(field.data, 1, 0) == answer->video_tx)
			return !pb_number(pb_data(field.data, 2), 1, 2);
	}
	g_assert_not_reached();
}

static void video_answer(void)
{
	struct call_group_answer answer;
	Pb reply = { 0 }, registration = { 0 };

	make_answer(&reply, 3, 2, -1, 0, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_JOINED);
	g_assert_true(answer.video_supported);
	g_assert_cmpuint(answer.secret.p[0], ==, 0);
	g_assert_cmpuint(answer.video_payload, ==, 105);
	g_assert_cmpuint(answer.video_rx, ==, 0xe3530183);
	g_assert_cmpuint(answer.video_tx, ==, 0xe3530187);
	call_group_register(&registration, &answer, TRUE);
	g_assert_true(registered_camera(&registration, &answer));
	registration.n = 0;
	call_group_register(&registration, &answer, FALSE);
	g_assert_false(registered_camera(&registration, &answer));

	reply.n = 0;
	make_answer(&reply, 7, 7, -1, 0, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_JOINED);
	g_assert_false(answer.video_supported);
	registration.n = 0;
	call_group_register(&registration, &answer, TRUE);
	g_assert_false(registered_camera(&registration, &answer));
	reply.n = 0;
	make_answer(&reply, 7, 2, -1, 1, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_JOINED);
	g_assert_true(answer.video_supported);
	reply.n = 0;
	make_answer(&reply, 7, 2, -1, 2, FALSE);
	g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
			CALL_GROUP_JOINED);
	g_assert_true(answer.video_supported);
	for (guint i = 0; i < 2; i++) {
		reply.n = 0;
		make_answer(&reply, 7, i ? 7 : 2, -1, 0, TRUE);
		g_assert_cmpint(call_group_answer(PB_SLICE(reply), &answer), ==,
				CALL_GROUP_JOINED);
		g_assert_cmpint(answer.video_supported, ==, !i);
		g_assert_true(answer.data_supported);
	}

	static const struct {
		guint map;
		int vp8, packet;
		gboolean supported, vp8a;
	} cases[] = {
		{ 7, 2, 1, TRUE, TRUE },   { 4, 2, 0, TRUE, TRUE },
		{ 4, 1, 1, TRUE, FALSE },  { 7, 1, -1, TRUE, FALSE },
		{ 7, 0, 1, FALSE, FALSE }, { 4, -1, 1, FALSE, FALSE },
		{ 7, 2, 2, FALSE, FALSE }, { 4, 3, 1, FALSE, FALSE },
		{ 5, 2, 1, FALSE, FALSE }, { 10, 2, 1, FALSE, FALSE },
		{ 2, 2, 1, TRUE, FALSE },
	};

	for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
		guint expected = cases[i].supported ? cases[i].map : 0;

		for (guint packed = 0; packed <= 1; packed++) {
			reply.n = 0;
			make_answer(&reply, 7, cases[i].map, cases[i].vp8,
				    cases[i].packet, packed);
			g_assert_cmpint(call_group_answer(PB_SLICE(reply),
							  &answer),
					==, CALL_GROUP_JOINED);
			g_assert_cmpint(answer.video_supported, ==,
					cases[i].supported);
			g_assert_cmpint(answer.video_vp8a, ==, cases[i].vp8a);
			g_assert_cmpuint(answer.video_map, ==, expected);
			if (cases[i].supported)
				g_assert_cmpuint(answer.video_packet_version,
						 ==, MAX(0, cases[i].packet));
		}
	}
}

static void add_source(Pb *out, guint field, const char *kind, guint32 ssrc)
{
	Pb source = { 0 };

	pb_text(&source, 1, kind);
	pb_uint(&source, 2, ssrc);
	pb_msg(out, field, &source);
}

static void conference_members(void)
{
	Pb info = { 0 }, member = { 0 }, reply = { 0 }, count = { 0 };
	struct call_group_presence presence;
	guint8 compressed[256];
	uLongf compressed_size = sizeof(compressed) - 1;

	pb_uint(&info, 1, 1);
	pb_uint(&info, 2, 123);
	pb_uint(&count, 1, 2);
	pb_msg(&info, 3, &count);
	pb_text(&member, 1, guest);
	pb_uint(&member, 2, 1);
	pb_uint(&member, 3, 1);
	add_source(&member, 10, "A", 0xabab);
	add_source(&member, 10, "V", 0x12345678);
	pb_msg(&info, 50, &member);
	member.n = 0;
	pb_text(&member, 1, "service-participant");
	pb_uint(&member, 3, 1);
	add_source(&member, 10, "V", 0x12345679);
	pb_msg(&info, 50, &member);
	pb_uint(&reply, 8, 1);
	pb_msg(&reply, 9, &info);
	g_assert_true(call_group_presence(
		PB_SLICE(reply), CALL_GROUP_PARTICIPATE_RSP, &presence));
	g_assert_true(presence.full);
	g_assert_cmpuint(presence.version, ==, 123);
	g_assert_cmpuint(presence.channel, ==, 0);
	g_assert_cmpuint(presence.members->len, ==, 1);
	g_assert_cmpuint(presence.wire_members, ==, 2);
	g_assert_cmpuint(presence.reported_members, ==, 2);
	g_assert_cmpuint(presence.unmapped_cameras, ==, 1);
	struct call_group_member *camera =
		&g_array_index(presence.members, struct call_group_member, 0);

	g_assert_cmpstr(camera->mid, ==, guest);
	g_assert_true(camera->active);
	g_assert_cmpuint(camera->ssrc, ==, 0x12345678);
	g_array_unref(presence.members);

	/* Compressed partial member update disables the existing camera. */
	info.n = member.n = 0;
	pb_text(&member, 1, guest);
	pb_uint(&member, 2, 1);
	pb_uint(&member, 3, 1);
	pb_msg(&info, 1, &member);
	g_assert_cmpint(compress2(compressed, &compressed_size, info.p, info.n,
				  Z_BEST_SPEED),
			==, Z_OK);
	reply.n = 0;
	pb_uint(&reply, 1, 5);
	pb_uint(&reply, 3, 1);
	pb_bytes(&reply, 2, (Slice){ compressed, compressed_size });
	g_assert_true(call_group_presence(PB_SLICE(reply), CALL_GROUP_PUSH_REQ,
					  &presence));
	g_assert_false(presence.full);
	g_assert_cmpuint(presence.members->len, ==, 1);
	camera = &g_array_index(presence.members, struct call_group_member, 0);
	g_assert_cmpstr(camera->mid, ==, guest);
	g_assert_false(camera->active);
	g_array_unref(presence.members);

	/* Reject trailing bytes and a compressed stream over the output bound. */
	reply.n = 0;
	pb_uint(&reply, 1, 5);
	pb_uint(&reply, 3, 1);
	gsize content_offset = reply.n;

	compressed[compressed_size] = 0;
	pb_bytes(&reply, 2, (Slice){ compressed, compressed_size + 1 });
	g_assert_false(call_group_presence(PB_SLICE(reply), CALL_GROUP_PUSH_REQ,
					   &presence));
	g_assert_null(presence.members);
	g_autofree guint8 *too_big = g_malloc0(65537);

	compressed_size = sizeof(compressed);
	g_assert_cmpint(compress2(compressed, &compressed_size, too_big, 65537,
				  Z_BEST_COMPRESSION),
			==, Z_OK);
	reply.n = content_offset;
	pb_bytes(&reply, 2, (Slice){ compressed, compressed_size });
	g_assert_false(call_group_presence(PB_SLICE(reply), CALL_GROUP_PUSH_REQ,
					   &presence));
	g_assert_null(presence.members);
}

static void channel_members(void)
{
	Pb info = { 0 }, member = { 0 }, push = { 0 };
	struct call_group_presence presence;

	pb_uint(&info, 1, 7);
	pb_uint(&info, 2, 42);
	pb_text(&member, 1, guest);
	pb_uint(&member, 3, 1);
	add_source(&member, 11, "V", 47);
	pb_msg(&info, 11, &member);
	pb_uint(&push, 1, 42);
	pb_uint(&push, 2, 21);
	pb_msg(&push, 11, &info);
	g_assert_true(call_group_presence(PB_SLICE(push),
					  CALL_GROUP_SUBPUSH_REQ, &presence));
	g_assert_false(presence.full);
	g_assert_cmpuint(presence.version, ==, 7);
	g_assert_cmpuint(presence.channel, ==, 42);
	g_assert_cmpuint(presence.members->len, ==, 1);
	struct call_group_member *camera =
		&g_array_index(presence.members, struct call_group_member, 0);

	g_assert_cmpstr(camera->mid, ==, guest);
	g_assert_true(camera->active);
	g_assert_cmpuint(camera->ssrc, ==, 47);
	g_assert_cmpuint(camera->channel, ==, 42);
	g_array_unref(presence.members);
	push.p[1] = 43;
	g_assert_false(call_group_presence(PB_SLICE(push),
					   CALL_GROUP_SUBPUSH_REQ, &presence));
}

static void stream_control(void)
{
	Pb request = { 0 }, reply = { 0 }, update = { 0 };

	call_group_stream_request(&request, guest, 0x82314567, 42, TRUE, 5);
	g_assert_false(request.overflow);
	Slice stream = pb_data(PB_SLICE(request), 1);
	Slice layer = pb_data(stream, 8);

	g_assert_cmpuint(pb_number(stream, 1, 0), ==, 1);
	g_assert_cmpuint(pb_data(stream, 2).n, ==, 0);
	g_assert_cmpuint(pb_number(stream, 3, 0), ==, 0x82314567);
	g_assert_cmpuint(pb_number(stream, 5, 0), ==, 1);
	g_assert_cmpuint(pb_number(stream, 7, 0), ==, 42);
	g_assert_cmpuint(pb_number(layer, 1, 0), ==, 2);
	g_assert_cmpuint(pb_number(layer, 2, 1), ==, 0);
	g_assert_cmpuint(pb_number(PB_SLICE(request), 2, 0), ==, 5);
	g_assert_cmpuint(pb_number(PB_SLICE(request), 3, 0), ==, 1);
	request.n = 0;
	call_group_stream_request(&request, guest, 0x82314567, 42, FALSE, 6);
	stream = pb_data(PB_SLICE(request), 1);
	g_assert_cmpuint(pb_number(stream, 1, 1), ==, 0);
	g_assert_cmpuint(pb_data(stream, 8).n, ==, 0);

	pb_uint(&update, 1, CALL_GROUP_STREAM_RESUME);
	pb_uint(&update, 3, 0x82314567);
	pb_uint(&update, 5, 42);
	pb_uint(&reply, 1, 0);
	pb_msg(&reply, 4, &update);
	g_autoptr(GArray) updates =
		call_group_stream_updates(PB_SLICE(reply), CALL_GROUP_STRM_RSP);

	g_assert_nonnull(updates);
	g_assert_cmpuint(updates->len, ==, 1);
	struct call_group_stream *record =
		&g_array_index(updates, struct call_group_stream, 0);

	g_assert_cmpuint(record->mid[0], ==, 0);
	g_assert_cmpuint(record->ssrc, ==, 0x82314567);
	g_assert_cmpuint(record->channel, ==, 42);
	g_assert_cmpint(record->state, ==, CALL_GROUP_STREAM_RESUME);
	g_clear_pointer(&updates, g_array_unref);
	reply.p[1] = 1;
	g_assert_null(call_group_stream_updates(PB_SLICE(reply),
						CALL_GROUP_STRM_RSP));
	reply.n = 0;
	update.p[1] = CALL_GROUP_STREAM_STOP;
	pb_text(&update, 2, guest);
	pb_msg(&reply, 1, &update);
	updates = call_group_stream_updates(PB_SLICE(reply),
					    CALL_GROUP_NOTIFY_REQ);
	g_assert_nonnull(updates);
	g_assert_cmpuint(updates->len, ==, 1);
	record = &g_array_index(updates, struct call_group_stream, 0);
	g_assert_cmpint(record->state, ==, CALL_GROUP_STREAM_STOP);
	g_assert_cmpstr(record->mid, ==, guest);
	g_assert_null(call_group_stream_updates((Slice){ reply.p, reply.n - 1 },
						CALL_GROUP_NOTIFY_REQ));
}

static void add_camera_layer(Pb *request, int layer, int codec)
{
	Pb record = { 0 };

	if (layer >= 0)
		pb_uint(&record, 1, layer);
	if (codec >= 0)
		pb_uint(&record, 2, codec);
	pb_msg(request, 8, &record);
}

static void camera_resolution(void)
{
	guint classes;
	Pb request = { 0 };
	guint32 camera = 42;

	pb_uint(&request, 1, 1); /* START */
	pb_uint(&request, 3, camera);
	pb_uint(&request, 4, 0); /* Legacy layerid alone has no effect. */
	add_camera_layer(&request, -1, 0);
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_UNCHANGED);
	add_camera_layer(&request, 2, 2); /* AVC */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_UNCHANGED);
	add_camera_layer(&request, 3, 0);
	add_camera_layer(&request, 1, -1); /* Defaults to VP8. */
	add_camera_layer(&request, 0, 1); /* VP8A is a distinct codec. */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     TRUE, &classes),
			==, 0);
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, 1);
	g_assert_cmpuint(classes, ==, 0xf);
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), 43,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_UNCHANGED);
	g_assert_cmpuint(classes, ==, 0);
	add_camera_layer(&request, 2, 0);
	add_camera_layer(&request, 0,
			 0); /* Seventh pair is outside the list. */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, 1);
	gsize base = request.n;

	pb_uint(&request, 5, 1); /* NEW_STREAM belongs to the subscriber. */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_UNCHANGED);
	request.n = base;
	pb_uint(&request, 5, 2); /* Explicit NO_RESOLUTION. */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_CLEAR);
	request.n = base;
	pb_uint(&request, 8, 0); /* Invalid wire type. */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_UNCHANGED);
	request.n = 0;
	pb_uint(&request, 1, 1);
	pb_uint(&request, 3, camera);
	add_camera_layer(&request, 0, 0); /* Explicit thumbnail. */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, 0);
	request.p[1] = 0; /* STOP */
	g_assert_cmpint(call_group_camera_resolution(PB_SLICE(request), camera,
						     FALSE, &classes),
			==, CALL_GROUP_LAYER_UNCHANGED);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/group-call/route-validation", route);
	g_test_add_func("/group-call/participation-request", participate);
	g_test_add_func("/group-call/answer-and-streams", answer_and_streams);
	g_test_add_func("/group-call/retransmit-request", retransmit_request);
	g_test_add_func("/group-call/congestion-feedback", congestion_feedback);
	g_test_add_func("/group-call/congestion-quiet-window",
			congestion_quiet_window);
	g_test_add_func("/group-call/congestion-control", congestion_control);
	g_test_add_func("/group-call/video-answer", video_answer);
	g_test_add_func("/group-call/conference-members", conference_members);
	g_test_add_func("/group-call/channel-members", channel_members);
	g_test_add_func("/group-call/stream-control", stream_control);
	g_test_add_func("/group-call/camera-resolution", camera_resolution);
	return g_test_run();
}
