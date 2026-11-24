// SPDX-License-Identifier: GPL-3.0-or-later
#include "call.h"
#include "call_audio.h"
#include "call_group.h"
#include "call_pdtp.h"
#include "screencast.h"
#include "call_schema.h"
#include "call_video.h"
#include "media_io.h"
#include "camera.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

enum {
	MID_LENGTH = 33,
	MEDIA_CONTROL_HEADER = 8,
	MEDIA_CONTROL_STREAMS = MEDIA_CONTROL_HEADER + 12,
	GROUP_RELEASE_TIMEOUT_US = 2 * G_USEC_PER_SEC,
	GROUP_RELEASE_RETRY_US = 500 * 1000,
};

/* A worker owns at most one call. Receiving a message cannot initiate a call. */
struct call_task {
	GThread *thread;
	int wake;
	gint done, muted, camera_enabled, video_ready, preview_visible;
	gint group_joined;
	gboolean video, group;
	char *incoming_id, *caller;
	char *target;
	GMutex frame_lock;
	GBytes *frame, *self_frame, *share_frame;
	char video_focus[MID_LENGTH + 1];
	gint64 frame_notified, self_notified, share_notified;
	int width, height, rotation, self_width, self_height;
	int share_width, share_height, share_rotation;
	/* The peer is presenting its screen (PLANET presentation streams). */
	gint peer_sharing;
	/* The user asked to share this desktop. */
	gint share_wanted;
};

struct incoming_call {
	J *route;
	char *caller, *local, *id;
	gint64 expires;
	gboolean video;
};
static struct incoming_call pending;
static char last_owner[34];
static gint64 last_call_revision;

static void emit_call_state(const char *chat, const char *state,
			    const char *message, gboolean video,
			    int camera_enabled)
{
	g_autoptr(J) event = obj();
	json_object_object_add(event, "event", json_object_new_string("call"));
	json_object_object_add(event, "chat", json_object_new_string(chat));
	json_object_object_add(event, "state", json_object_new_string(state));
	json_object_object_add(event, "video", json_object_new_boolean(video));
	if (camera_enabled >= 0)
		json_object_object_add(event, "cameraEnabled",
				       json_object_new_boolean(camera_enabled));
	if (message)
		json_object_object_add(event, "message",
				       json_object_new_string(message));
	line_emit(event);
}

static void call_state(const char *chat, const char *state, const char *message,
		       gboolean video)
{
	emit_call_state(chat, state, message, video, -1);
}

static void clear_pending(void)
{
	if (pending.route)
		json_object_put(pending.route);
	g_free(pending.caller);
	g_free(pending.local);
	g_free(pending.id);
	pending = (struct incoming_call){ 0 };
}

gint64 line_voice_tick(void)
{
	if (pending.route && g_get_monotonic_time() >= pending.expires) {
		call_state(pending.caller, "ended", NULL, pending.video);
		clear_pending();
	}
	return pending.expires;
}

static struct call_task *active;
static int frame_socket = -1;

void line_call_frame_socket(int fd)
{
	frame_socket = fd;
}

struct call_endpoint {
	struct sockaddr_storage addr;
	socklen_t len;
};

struct media_route {
	struct call_endpoint current, previous;
	gint64 last_received;
	gboolean reported_mismatch;
};

struct group_video {
	struct call_video *receiver;
	GArray *members;
	GBytes *camera_report;
	struct call_group_member peer;
	gint64 retry_at, last_rtp;
	guint32 version, sequence, request_sequence;
	guint32 layer_sequence;
	guint attempts;
	guint8 rx_key[30], request_id[16];
	guint8 camera_start_id[16];
	guint32 camera_start_sequence;
	gboolean camera_start_pending, vp8a;
};

struct voice_call {
	enum line_media_reception reception;
	struct call_task *task;
	CallInfo info;
	struct call_group_route group_route;
	GBytes *group_report;
	struct call_group_feedback *feedback;
	gint64 next_group_feedback;
	struct group_video *conference_video;
	CallWire wire;
	EVP_PKEY *media_key;
	LineAudio *audio;
	struct line_camera *camera;
	struct call_video *video;
	struct call_pdtp *data;
	struct call_share_decoder *share_rx;
	GByteArray *share_message;
	struct line_screencast *cast;
	struct call_share_encoder *share_tx;
	gboolean share_streams, self_sharing;
	gboolean share_over_video;
	GBytes *share_video_frame;
	guint share_video_width, share_video_height;
	gint64 share_video_repeat, share_video_keyframe;
	/* Learned from the peer's BIG_DATA_REQ: message ID and services. */
	guint big_data_id, big_data_type;
	Pb big_data_services;
	char share_session[37];
	guint64 share_time;
	guint8 video_pt, data_pt;
	guint32 data_rx, data_tx;
	guint data_traced;
	guint32 audio_rx, audio_tx, video_rx, video_tx;
	char *video_notice;
	gint64 next_video, next_video_feedback, video_start_retry;
	/* -1 until the connected call has reported its outgoing video once. */
	int reported_video;
	/* Sending limits chosen from the peer's advertised receive limits. */
	guint send_kbps, send_fps, send_width, send_height;
	guint screen_kbps, screen_fps, screen_width, screen_height;

	struct {
		gint64 report_at;
		guint packets, other_ssrc, frames;
		gboolean enabled;
	} video_trace;
	/* Aggregate group diagnostics, enabled only for a requested test call. */
	struct {
		gint64 next;
		guint audio_sent, audio_active, audio_played, video_packets;
		guint frames, controls, data_packets, camera_sent;
		guint rtcp, audio_reports, video_reports, relay_media;
		guint ccfs_audio, ccfs_camera, feedback_formats,
			rejected_controls;
		guint feedback_sent, data_accepted, data_sent;
		guint mic_acknowledged, camera_acknowledged;
		guint mic_retransmits_requested, camera_retransmits_requested;
		guint rtcp_types, feedback_other_slots, rtcp_other_port;
		guint udp_received, control_opened, unclassified_relay;
	} stats;
	struct call_endpoint relay, media;
	struct media_route audio_route, video_route, data_route;
	int fd;
	gint64 deadline, next_ka, next_capture;
	guint ka_seconds;
	gboolean invited, ringing, remote_end, muted, incoming, decline,
		verified, media_control, group;
	guint8 expected_id[16];
	guint64 expected_seq;
	Pb peer_offer;
};

static void call_connected(struct voice_call *c)
{
	emit_call_state(c->info.target, "connected", c->video_notice,
			c->video != NULL,
			g_atomic_int_get(&c->task->camera_enabled));
}

static void set_error(GError **e, const char *message)
{
	g_set_error_literal(e, LINE_ERROR, 1, message);
}

static gboolean valid_mid(const char *mid)
{
	if (!mid || mid[0] != 'u' || strlen(mid) != MID_LENGTH)
		return FALSE;
	for (int i = 1; i < MID_LENGTH; i++)
		if (!g_ascii_isxdigit(mid[i]))
			return FALSE;
	return TRUE;
}

static gboolean address(struct call_endpoint *ep, const char *host, int port,
			int family, gboolean numeric)
{
	if (!host || !*host || strlen(host) > 253 || port < 1 || port > 65535)
		return FALSE;
	struct addrinfo hints = { .ai_family = family,
				  .ai_socktype = SOCK_DGRAM,
				  .ai_flags = AI_NUMERICSERV |
					      (numeric ? AI_NUMERICHOST : 0) };
	struct addrinfo *result = NULL;
	char service[8];

	g_snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(host, service, &hints, &result))
		return FALSE;
	gboolean ok = result && result->ai_addrlen <= sizeof(ep->addr);

	if (ok) {
		memcpy(&ep->addr, result->ai_addr, result->ai_addrlen);
		ep->len = result->ai_addrlen;
	}
	freeaddrinfo(result);
	return ok;
}

static gboolean equal_address(const struct call_endpoint *a,
			      const struct call_endpoint *b)
{
	if (a->addr.ss_family != b->addr.ss_family)
		return FALSE;
	if (a->addr.ss_family == AF_INET) {
		const struct sockaddr_in *x = (const void *)&a->addr,
					 *y = (const void *)&b->addr;
		return x->sin_port == y->sin_port &&
		       x->sin_addr.s_addr == y->sin_addr.s_addr;
	}
	const struct sockaddr_in6 *x = (const void *)&a->addr,
				  *y = (const void *)&b->addr;
	return x->sin6_port == y->sin6_port &&
	       !memcmp(&x->sin6_addr, &y->sin6_addr, 16) &&
	       x->sin6_scope_id == y->sin6_scope_id;
}

static gboolean send_to(struct voice_call *c, const struct call_endpoint *to,
			Slice pkt)
{
	ssize_t n;

	do {
		n = sendto(c->fd, pkt.p, pkt.n, MSG_NOSIGNAL,
			   (const void *)&to->addr, to->len);
	} while (n < 0 && errno == EINTR);
	return n == (ssize_t)pkt.n;
}

static void format_endpoint(const struct call_endpoint *e, char *out,
			    gsize size)
{
	char host[INET6_ADDRSTRLEN] = "?";
	const struct sockaddr_in *v4 = (const void *)&e->addr;
	const struct sockaddr_in6 *v6 = (const void *)&e->addr;
	gboolean ip4 = e->addr.ss_family == AF_INET;

	inet_ntop(e->addr.ss_family,
		  ip4 ? (const void *)&v4->sin_addr :
			(const void *)&v6->sin6_addr,
		  host, sizeof(host));
	g_snprintf(out, size, "%s:%d", host,
		   ntohs(ip4 ? v4->sin_port : v6->sin6_port));
}

static void media_received(struct voice_call *c, struct media_route *route,
			   const struct call_endpoint *source)
{
	gint64 now = g_get_monotonic_time();

	c->deadline = now + 90 * G_USEC_PER_SEC;
	if (c->video_trace.enabled && !equal_address(&route->current, source) &&
	    !route->reported_mismatch) {
		char from[INET6_ADDRSTRLEN + 8], to[INET6_ADDRSTRLEN + 8];

		route->reported_mismatch = TRUE;
		format_endpoint(source, from, sizeof(from));
		format_endpoint(&route->current, to, sizeof(to));
		g_printerr("line-call %s arrives from %s, sending to %s\n",
			   route == &c->audio_route ? "audio" :
			   route == &c->data_route  ? "data" :
						      "video",
			   from, to);
	}
	if (equal_address(&route->current, source)) {
		route->last_received = now;
		return;
	}
	/*
	 * Media can arrive through a different relay address than signaling
	 * gave. An incoming call's CONN_RSP names the peer's direct address,
	 * which the official app ignores, so both directions follow the relay.
	 * Learn only from authenticated packets of this stream. Briefly keep
	 * late packets from the previous address from undoing a migration.
	 */
	if (!(route->previous.len && equal_address(&route->previous, source) &&
	      now - route->last_received < G_USEC_PER_SEC)) {
		route->previous = route->current;
		route->current = *source;
		route->last_received = now;
		if (c->video_trace.enabled) {
			char host[INET6_ADDRSTRLEN] = "?";
			const struct sockaddr_in *v4 =
				(const void *)&source->addr;
			const struct sockaddr_in6 *v6 =
				(const void *)&source->addr;
			int port = source->addr.ss_family == AF_INET ?
					   ntohs(v4->sin_port) :
					   ntohs(v6->sin6_port);

			inet_ntop(source->addr.ss_family,
				  source->addr.ss_family == AF_INET ?
					  (const void *)&v4->sin_addr :
					  (const void *)&v6->sin6_addr,
				  host, sizeof(host));
			g_printerr("line-call %s route now %s:%d\n",
				   route == &c->audio_route ? "audio" :
				   route == &c->data_route  ? "data" :
							      "video",
				   host, port);
		}
	}
}

static gboolean send_plain(struct voice_call *c, const struct call_endpoint *to,
			   const Pb *plain, gboolean boot, gboolean raw)
{
	Pb packet = { 0 };

	return call_wire_pack(&c->wire, plain, boot, raw, &packet) &&
	       send_to(c, to, PB_SLICE(packet));
}

static gboolean send_control_to(struct voice_call *c,
				const struct call_endpoint *to, guint plane,
				guint choice, guint id, const Pb *body,
				Slice reply)
{
	Pb plain = { 0 };

	if (!call_envelope(&plain, &c->info, plane, choice, id, body, reply))
		return FALSE;
	if (c->incoming && (id == 0x2142 || id == 0x2144)) {
		Slice hdr = pb_data(PB_SLICE(plain), 1),
		      transaction = pb_data(hdr, 4);
		if (transaction.n != 16)
			return FALSE;
		memcpy(c->expected_id, transaction.p, 16);
		c->expected_seq = pb_number(hdr, 5, G_MAXUINT64);
	}
	return send_plain(c, to, &plain,
			  id == 0x2141 || id == 0x2142 ||
				  id == CALL_GROUP_PARTICIPATE_ID,
			  FALSE);
}

static gboolean send_control(struct voice_call *c, guint plane, guint choice,
			     guint id, const Pb *body, Slice reply)
{
	return send_control_to(c, &c->relay, plane, choice, id, body, reply);
}

static void free_call(struct voice_call *c)
{
	line_audio_close(c->audio);
	line_camera_close(c->camera);
	call_video_close(c->video);
	call_pdtp_close(c->data);
	call_share_decoder_free(c->share_rx);
	if (c->share_message)
		g_byte_array_free(c->share_message, TRUE);
	line_screencast_stop(c->cast);
	call_share_encoder_free(c->share_tx);
	g_clear_pointer(&c->share_video_frame, g_bytes_unref);
	if (c->fd >= 0)
		close(c->fd);
	call_wire_clear(&c->wire);
	call_group_route_clear(&c->group_route);
	g_clear_pointer(&c->group_report, g_bytes_unref);
	g_free(c->feedback);
	if (c->conference_video) {
		struct group_video *v = c->conference_video;

		call_video_close(v->receiver);
		g_array_unref(v->members);
		g_clear_pointer(&v->camera_report, g_bytes_unref);
		OPENSSL_cleanse(v, sizeof(*v));
		g_free(v);
	}
	EVP_PKEY_free(c->media_key);
	CallInfo *s = &c->info;

	g_free(s->local);
	g_free(s->target);
	g_free(s->i_zone);
	g_free(s->r_zone);
	g_free(s->stid);
	g_free(s->cid);
	g_free(c->video_notice);
	if (s->token) {
		OPENSSL_cleanse(s->token, strlen(s->token));
		g_free(s->token);
	}
	OPENSSL_cleanse(c, sizeof(*c));
	g_free(c);
}

static gboolean keepalive(struct voice_call *c)
{
	Pb ka = { 0 };

	pb_fixed64(&ka, 1, g_get_real_time() / 1000);
	pb_uint(&ka, 2, 0);
	c->next_ka =
		g_get_monotonic_time() + (gint64)c->ka_seconds * G_USEC_PER_SEC;
	return send_control(c, 2, 1, 0x1101, &ka, (Slice){ 0 });
}

static gboolean pinholes(struct voice_call *c)
{
	for (guint i = 0; i < 16; i++) {
		guint64 probe;
		guint8 pad[500];

		if (RAND_bytes((guint8 *)&probe, sizeof(probe)) != 1 ||
		    RAND_bytes(pad, sizeof(pad)) != 1)
			return FALSE;
		Pb inner = { 0 }, raw = { 0 };

		pb_uint(&inner, 1, 16);
		pb_uint(&inner, 2,
			(probe & G_GUINT64_CONSTANT(0xffffffffffff)) |
				(G_GUINT64_CONSTANT(1) << 47));
		pb_uint(&inner, 3, 300);
		pb_bytes(&inner, 4, (Slice){ pad, sizeof(pad) });
		pb_msg(&raw, 3, &inner);
		if (!send_plain(c, &c->relay, &raw, FALSE, TRUE))
			return FALSE;
	}
	Pb inner = { 0 }, raw = { 0 };

	pb_uint(&inner, 1, 500);
	pb_uint(&inner, 2, 16);
	pb_uint(&inner, 3, 300);
	pb_msg(&raw, 1, &inner);
	return send_plain(c, &c->relay, &raw, FALSE, TRUE);
}

static void copy_services(Pb *b, Slice req)
{
	for (guint i = 151; i <= 152; i++) {
		Slice v = pb_data(req, i);

		if (v.p)
			pb_bytes(b, i, v);
	}
	guint64 domain = pb_number(req, 153, 2);

	if (domain < 2)
		pb_uint(b, 153, domain);
}

static gboolean exchange(struct voice_call *c, Slice req)
{
	Pb body = { 0 }, target = { 0 };
	static const guint8 csv[] = "{\"csv\":1}";

	pb_text(&body, 1, "exchange_app_str_data");
	pb_bytes(&body, 2, (Slice){ csv, sizeof(csv) });
	pb_text(&body, 4, c->info.local);
	pb_text(&target, 1, c->info.target);
	pb_msg(&body, 6, &target);
	copy_services(&body, req);
	return send_control(c, 3, 17, 0x2147, &body, (Slice){ 0 });
}

static gboolean endpoint_from_addr(struct voice_call *c, Slice addr)
{
	Slice host = pb_data(addr, 3);

	if (!host.n || host.n >= INET6_ADDRSTRLEN ||
	    memchr(host.p, 0, host.n) || pb_number(addr, 2, 0) > 1)
		return FALSE;
	char h[INET6_ADDRSTRLEN];

	memcpy(h, host.p, host.n);
	h[host.n] = 0;
	guint64 port = pb_number(addr, 5, 0);

	if (!port) {
		Slice ports = pb_data(addr, 4);
		gsize i = 0;

		while (i < ports.n && !g_ascii_isdigit(ports.p[i]))
			i++;
		while (i < ports.n && g_ascii_isdigit(ports.p[i]) &&
		       port <= 65535)
			port = port * 10 + ports.p[i++] - '0';
	}
	return port > 0 && port <= 65535 &&
	       address(&c->media, h, (int)port, c->relay.addr.ss_family, TRUE);
}

static gboolean send_video_packet(const guint8 *data, gsize size, gpointer user)
{
	struct voice_call *c = user;

	gboolean sent =
		send_to(c, &c->video_route.current, (Slice){ data, size });

	if (sent && c->stats.next && size >= 12 &&
	    (data[1] & 127) == c->video_pt)
		c->stats.camera_sent++;
	return sent;
}

static gboolean send_data_packet(const guint8 *data, gsize size, gpointer user)
{
	struct voice_call *c = user;

	gboolean sent =
		send_to(c, &c->data_route.current, (Slice){ data, size });

	if (sent && c->stats.next)
		c->stats.data_sent++;
	return sent;
}

static GBytes *pack_i420(const struct line_camera_frame *image);

static void emit_share_state(struct voice_call *c, const char *key,
			     gboolean sharing, const char *message)
{
	g_autoptr(J) event = obj();

	json_object_object_add(event, "event",
			       json_object_new_string("call_share"));
	json_object_object_add(event, "chat",
			       json_object_new_string(c->info.target));
	json_object_object_add(event, key, json_object_new_boolean(sharing));
	json_object_object_add(event, "changedAt",
			       json_object_new_int64(g_get_monotonic_time()));
	if (!strcmp(key, "selfSharing"))
		json_object_object_add(event, "shareInVideo",
				       json_object_new_boolean(
					       sharing && c->share_over_video));
	if (message)
		json_object_object_add(event, "message",
				       json_object_new_string(message));
	line_emit(event);
}

static void peer_share_state(struct voice_call *c, gboolean sharing)
{
	struct call_task *task = c->task;

	if (g_atomic_int_get(&task->peer_sharing) == sharing)
		return;
	g_atomic_int_set(&task->peer_sharing, sharing);
	if (!sharing) {
		g_mutex_lock(&task->frame_lock);
		g_clear_pointer(&task->share_frame, g_bytes_unref);
		g_mutex_unlock(&task->frame_lock);
		if (c->share_message)
			g_byte_array_set_size(c->share_message, 0);
	}
	if (c->video_trace.enabled)
		g_printerr("line-call peer screen share %s\n",
			   sharing ? "started" : "stopped");
	emit_share_state(c, "peerSharing", sharing, NULL);
}

/* Return true when the preview is sent or dropped under backpressure. */
static gboolean send_preview(struct voice_call *c,
			     enum line_preview_source source,
			     const struct line_camera_frame *image, gint64 now)
{
	if (frame_socket < 0)
		return FALSE;
	struct line_preview_frame report = {
		.decoded_at = now,
		.width = image->width,
		.height = image->height,
		.rotation = image->rotation,
		.source = source,
		.camera_enabled = g_atomic_int_get(&c->task->camera_enabled),
	};

	g_strlcpy(report.chat, c->info.target, sizeof(report.chat));
	return line_media_send_frame(frame_socket, &report, image);
}

static void display_share(const struct line_camera_frame *image, gpointer user)
{
	struct voice_call *c = user;
	struct call_task *task = c->task;

	if (!g_atomic_int_get(&task->preview_visible) ||
	    !line_camera_frame_size_valid(image->width, image->height))
		return;
	gint64 now = g_get_monotonic_time();

	if (send_preview(c, LINE_PREVIEW_SCREEN, image, now))
		return;
	GBytes *frame = pack_i420(image);

	g_mutex_lock(&task->frame_lock);
	gboolean notify = !task->share_frame ||
			  now - task->share_notified >= G_USEC_PER_SEC;
	GBytes *previous = task->share_frame;

	task->share_frame = frame;
	task->share_width = image->width;
	task->share_height = image->height;
	task->share_rotation = image->rotation;
	if (notify)
		task->share_notified = now;
	g_mutex_unlock(&task->frame_lock);
	g_clear_pointer(&previous, g_bytes_unref);
	if (notify)
		line_event("call_share_frame", "chat", c->info.target);
}

/*
 * A screen share travels on two PDTP streams of the PLANET service: stream
 * 3 carries presentation commands and stream 4 the EVS3 VP8 fragments, one
 * per PDTP message.
 */
enum {
	SHARE_CONTROL_STREAM = 3,
	SHARE_VIDEO_STREAM = 4,
	SHARE_STREAM_FLAGS = 0x12,
	SHARE_START = 1,
	SHARE_STOP = 2,
	SHARE_MEDIA_VIDEO = 1,
};

/* jup_presn_ctrl_send_cmmd: command, media, then a STOP reason code. */
static void share_control(struct voice_call *c, const guint8 *p, gsize n)
{
	const guint8 *end = p + n;
	guint64 command;

	while (p < end && call_pdtp_read_varint(&p, end, &command)) {
		guint64 media = 0, code = 0;

		if (!call_pdtp_read_varint(&p, end, &media))
			return;
		if (command == SHARE_STOP)
			call_pdtp_read_varint(&p, end, &code);
		if (c->video_trace.enabled)
			g_printerr(
				"line-call share command=%u media=%u code=%u\n",
				(guint)command, (guint)media, (guint)code);
		if (command == SHARE_START || command == SHARE_STOP)
			peer_share_state(c, command == SHARE_START);
	}
}

static void data_stream(guint64 stream, const char *service, guint64 offset,
			guint64 time, guint flags, const guint8 *bytes,
			gsize size, gpointer user)
{
	struct voice_call *c = user;
	const char *dump = g_getenv("LINE_CALL_PDTP_DUMP");

	(void)offset;
	(void)time;
	/* Debug aid: append each in-order stream to DIR/SERVICE-STREAM.bin. */
	if (c->video_trace.enabled && dump && *dump) {
		g_autofree char *name =
			g_strdup_printf("%s/%s-%" G_GUINT64_FORMAT ".bin", dump,
					service, stream);
		FILE *f = fopen(name, "ab");

		if (f) {
			fwrite(bytes, 1, size, f);
			fclose(f);
		}
	}
	if (strcmp(service, "PLANET"))
		return;
	if (stream == SHARE_CONTROL_STREAM) {
		share_control(c, bytes, size);
		return;
	}
	if (stream != SHARE_VIDEO_STREAM)
		return;
	if (!c->share_rx)
		c->share_rx = call_share_decoder_new();
	if ((flags & (PDTP_MESSAGE_START | PDTP_MESSAGE_END)) ==
	    (PDTP_MESSAGE_START | PDTP_MESSAGE_END)) {
		/* Most PDTP messages fit in one packet. Borrow its bytes. */
		if (c->share_message)
			g_byte_array_set_size(c->share_message, 0);
		peer_share_state(c, TRUE);
		call_share_decode(c->share_rx, bytes, size, display_share, c);
		return;
	}
	if (!c->share_message)
		c->share_message = g_byte_array_new();
	if (flags & PDTP_MESSAGE_START)
		g_byte_array_set_size(c->share_message, 0);
	if (c->share_message->len + size > (1u << 20))
		return;
	g_byte_array_append(c->share_message, bytes, size);
	if (!(flags & PDTP_MESSAGE_END))
		return;
	/* Video before a START command still means the peer is sharing. */
	peer_share_state(c, TRUE);
	call_share_decode(c->share_rx, c->share_message->data,
			  c->share_message->len, display_share, c);
	g_byte_array_set_size(c->share_message, 0);
}

static void data_event(guint64 stream, const char *service, int element,
		       gpointer user)
{
	struct voice_call *c = user;

	if (strcmp(service, "PLANET") || stream != SHARE_VIDEO_STREAM)
		return;
	/* The viewer resumed our share: start it with a keyframe. */
	if (element == PDTP_EVENT_RESUME)
		call_share_encoder_keyframe(c->share_tx);
	if (element == PDTP_STREAM_CLOSE || element == PDTP_STREAM_RESET)
		peer_share_state(c, FALSE);
}

/* ---- Sharing this desktop ---- */

static void send_share_command(struct voice_call *c, guint command)
{
	guint8 cmd[3] = { command, SHARE_MEDIA_VIDEO, 0 };

	call_pdtp_stream_send(c->data, SHARE_CONTROL_STREAM, cmd,
			      command == SHARE_STOP ? 3 : 2, 0);
}

static gboolean share_fragment(const guint8 *fragment, gsize size, gboolean key,
			       gpointer user)
{
	struct voice_call *c = user;

	(void)key;
	return call_pdtp_stream_send(c->data, SHARE_VIDEO_STREAM, fragment,
				     size, c->share_time);
}

static guint share_setting(const char *name, guint fallback, guint lo, guint hi)
{
	const char *v = g_getenv(name);

	return CLAMP(v && *v ? (guint)g_ascii_strtoull(v, NULL, 10) : fallback,
		     lo, hi);
}

/*
 * LINE announces a screen share as feature-share item 16 in a BIG_DATA_REQ:
 * zlib-compressed JSON {"s":session,"m":"s","p":{"t":start_ns,"p":true}},
 * and withdraws it with operation 1. The official app shows a peer's share
 * only while this item is set.
 */
static void announce_share(struct voice_call *c, gboolean sharing,
			   guint64 start_ns)
{
	Pb body = { 0 }, contents = { 0 }, item = { 0 };
	guint32 id;

	if (!c->share_session[0]) {
		g_autofree char *uuid = g_uuid_string_random();

		g_strlcpy(c->share_session, uuid, sizeof(c->share_session));
	}
	pb_uint(&contents, 1, 16);
	pb_uint(&contents, 2, sharing ? 0 : 1);
	pb_uint(&item, 1, sharing);
	pb_text(&item, 2, c->info.local);
	if (sharing) {
		g_autofree char *json = g_strdup_printf(
			"{\"s\":\"%s\",\"m\":\"s\",\"p\":{\"t\":%" G_GUINT64_FORMAT
			",\"p\":true}}",
			c->share_session, start_ns);
		guint8 packed[256];
		uLongf n = sizeof(packed);

		pb_uint(&item, 3, 0);
		if (compress2(packed, &n, (const Bytef *)json, strlen(json),
			      Z_BEST_COMPRESSION) == Z_OK) {
			pb_bytes(&item, 4, (Slice){ packed, n });
			pb_uint(&item, 5, 1);
		} else {
			pb_text(&item, 4, json);
		}
	}
	pb_msg(&contents, 3, &item);
	if (RAND_bytes((guint8 *)&id, sizeof(id)) != 1)
		id = g_random_int();
	pb_uint(&body, 1, id & 0x7fffffff);
	pb_uint(&body, 2, 0);
	pb_uint(&body, 3, 1);
	pb_uint(&body, 5, c->big_data_type ? c->big_data_type : 6);
	pb_bytes(&body, 6, PB_SLICE(contents));
	if (c->big_data_services.n &&
	    body.n + c->big_data_services.n <= sizeof(body.p)) {
		memcpy(body.p + body.n, c->big_data_services.p,
		       c->big_data_services.n);
		body.n += c->big_data_services.n;
	}
	gboolean sent = send_control(c, 3, 27,
				     c->big_data_id ? c->big_data_id : 0x214c,
				     &body, (Slice){ 0 });

	if (c->video_trace.enabled)
		g_printerr(
			"line-call share announcement sharing=%d id=0x%x sent=%d\n",
			sharing, c->big_data_id, sent);
}

static void stop_sharing(struct voice_call *c, const char *message)
{
	gboolean was = c->self_sharing || c->cast ||
		       g_atomic_int_get(&c->task->share_wanted);

	g_clear_pointer(&c->cast, line_screencast_stop);
	g_clear_pointer(&c->share_tx, call_share_encoder_free);
	g_clear_pointer(&c->share_video_frame, g_bytes_unref);
	c->share_video_repeat = 0;
	c->share_video_keyframe = 0;
	if (c->self_sharing && !c->share_over_video) {
		send_share_command(c, SHARE_STOP);
		announce_share(c, FALSE, 0);
	}
	if (!message && c->share_over_video && c->self_sharing)
		message = "";
	if (c->share_over_video && c->self_sharing)
		call_video_screen(c->video, 0, 0);
	c->self_sharing = FALSE;
	c->share_over_video = FALSE;
	g_atomic_int_set(&c->task->share_wanted, FALSE);
	if (was)
		emit_share_state(c, "selfSharing", FALSE, message);
}

/* Follows the user's share request and the capture session's state. */
static void share_state(struct voice_call *c)
{
	gboolean wanted = g_atomic_int_get(&c->task->share_wanted);

	if (!c->data && !c->video) {
		if (wanted) {
			g_atomic_int_set(&c->task->share_wanted, FALSE);
			emit_share_state(
				c, "selfSharing", FALSE,
				"This call has no video or data channel for screen sharing.");
		}
		return;
	}
	if (!wanted && (c->cast || c->self_sharing)) {
		stop_sharing(c, NULL);
		return;
	}
	if (wanted && !c->cast) {
		g_autoptr(GError) error = NULL;
		guint width =
			share_setting("LINE_SHARE_WIDTH", 1920, 320, 3840);
		guint height =
			share_setting("LINE_SHARE_HEIGHT", 1080, 240, 2160);
		guint fps = share_setting("LINE_SHARE_FPS", 30, 1, 60);

		/* The official tablet may leave the presentation channel idle. */
		c->share_over_video =
			c->video &&
			(!c->data || g_strcmp0(g_getenv("LINE_SHARE_TRANSPORT"),
					       "presentation"));
		if (c->share_over_video) {
			width = MIN(width, c->screen_width);
			height = MIN(height, c->screen_height);
			fps = MIN(fps, c->screen_fps);
			c->screen_fps = fps;
		}
		c->cast = line_screencast_start(width, height, fps, &error);
		if (!c->cast)
			stop_sharing(c,
				     error ? error->message :
					     "Could not start screen capture.");
	}
}

static void camera_state(struct voice_call *c);

static gboolean share_is_video(const struct voice_call *c)
{
	return c->self_sharing && c->share_over_video;
}

static gboolean video_requested(const struct voice_call *c)
{
	return share_is_video(c) || g_atomic_int_get(&c->task->camera_enabled);
}

static gboolean send_share_video(struct voice_call *c, gint64 now)
{
	struct line_camera_frame image;
	g_autoptr(GError) error = NULL;

	line_camera_i420_frame(&image,
			       g_bytes_get_data(c->share_video_frame, NULL),
			       c->share_video_width, c->share_video_height);
	if (!call_video_encode(c->video, &image, now, send_video_packet, c,
			       &error)) {
		stop_sharing(c, error->message);
		return FALSE;
	}
	/* A compositor may stop providing frames until the desktop changes. */
	c->share_video_repeat = now + G_USEC_PER_SEC;
	return TRUE;
}

static void share_capture(struct voice_call *c, gint64 now)
{
	g_autofree char *message = NULL;
	enum line_screencast_state state =
		line_screencast_poll(c->cast, &message);

	if (state == LINE_SCREENCAST_FAILED ||
	    state == LINE_SCREENCAST_STOPPED) {
		if (c->video_trace.enabled)
			g_printerr("line-call screen capture ended: %s\n",
				   message ? message : "");
		stop_sharing(c, message);
		return;
	}
	if (state != LINE_SCREENCAST_RUNNING)
		return;
	unsigned int width, height;
	guint64 time;
	g_autoptr(GBytes) frame =
		line_screencast_take(c->cast, &width, &height, &time);

	if (!frame)
		return;
	if (c->share_over_video) {
		gboolean first = !c->self_sharing;

		if (first) {
			c->self_sharing = TRUE;
			call_video_screen(c->video, c->screen_kbps,
					  c->screen_fps);
			c->share_video_keyframe = now + G_USEC_PER_SEC;
			camera_state(c);
		}
		g_clear_pointer(&c->share_video_frame, g_bytes_unref);
		c->share_video_frame = g_steal_pointer(&frame);
		c->share_video_width = width;
		c->share_video_height = height;
		if (send_share_video(c, now) && first)
			emit_share_state(
				c, "selfSharing", TRUE,
				"Your screen is shown in your video tile.");
		return;
	}
	if (!c->share_streams) {
		c->share_streams =
			call_pdtp_stream_open(c->data, SHARE_CONTROL_STREAM,
					      "PLANET", SHARE_STREAM_FLAGS) &&
			call_pdtp_stream_open(c->data, SHARE_VIDEO_STREAM,
					      "PLANET", SHARE_STREAM_FLAGS);
		if (!c->share_streams) {
			stop_sharing(
				c,
				"Could not open the screen sharing streams.");
			return;
		}
	}
	if (!c->self_sharing) {
		c->self_sharing = TRUE;
		send_share_command(c, SHARE_START);
		announce_share(c, TRUE, (guint64)now * 1000);
		if (!c->share_tx)
			c->share_tx = call_share_encoder_new(
				share_setting("LINE_SHARE_KBPS", 3500, 300,
					      20000),
				share_setting("LINE_SHARE_FPS", 30, 1, 60));
		call_share_encoder_keyframe(c->share_tx);
		emit_share_state(c, "selfSharing", TRUE, NULL);
		if (c->video_trace.enabled)
			g_printerr("line-call screen share started\n");
	}
	/*
	 * Skip frames while the viewer has paused the stream or the link
	 * drains, instead of queueing latency.
	 */
	if (call_pdtp_stream_paused(c->data, SHARE_VIDEO_STREAM) ||
	    call_pdtp_stream_backlog(c->data, SHARE_VIDEO_STREAM) >
		    (2u << 20)) {
		call_share_encoder_keyframe(c->share_tx);
		return;
	}
	struct line_camera_frame image;
	g_autoptr(GError) error = NULL;

	line_camera_i420_frame(&image, g_bytes_get_data(frame, NULL), width,
			       height);
	c->share_time = time;
	if (!call_share_encode(c->share_tx, &image, now, share_fragment, c,
			       &error))
		stop_sharing(c, error->message);
}

static GBytes *pack_i420(const struct line_camera_frame *image)
{
	gsize size = line_camera_i420_size(image->width, image->height);
	guint8 *pixels = g_malloc(size);

	line_camera_pack_i420(pixels, image);
	return g_bytes_new_take(pixels, size);
}

static void group_reception(struct voice_call *c,
			    enum line_media_reception state)
{
	if (!c->group || state <= c->reception)
		return;
	c->reception = state;
	g_autoptr(J) event = obj();

	json_object_object_add(event, "event",
			       json_object_new_string("call_reception"));
	json_object_object_add(event, "chat",
			       json_object_new_string(c->info.target));
	json_object_object_add(event, "stage", json_object_new_int(state));
	line_emit(event);
}

static void display_video(const struct line_camera_frame *image, gpointer user)
{
	struct voice_call *c = user;
	struct call_task *task = c->task;
	unsigned int width = image->width, height = image->height;

	if (!line_camera_frame_size_valid(width, height))
		return;
	group_reception(c, LINE_MEDIA_PLAYABLE);
	c->video_start_retry = 0;
	if (c->video_trace.enabled && ++c->video_trace.frames == 1)
		g_printerr("line-call video first frame decoded\n");
	if (c->stats.next)
		c->stats.frames++;
	if (!g_atomic_int_get(&task->preview_visible))
		return;
	gint64 now = g_get_monotonic_time();

	if (send_preview(c, LINE_PREVIEW_REMOTE, image, now))
		return;
	GBytes *frame = pack_i420(image);

	g_mutex_lock(&task->frame_lock);
	gboolean notify = !task->frame ||
			  now - task->frame_notified >= G_USEC_PER_SEC;
	GBytes *previous = task->frame;

	task->frame = frame;
	task->width = width;
	task->height = height;
	task->rotation = image->rotation;
	if (notify)
		task->frame_notified = now;
	g_mutex_unlock(&task->frame_lock);
	g_clear_pointer(&previous, g_bytes_unref);
	if (notify)
		line_event("call_frame", "chat", c->info.target);
}

static gboolean capture_video(struct voice_call *c, gint64 now, GError **error)
{
	struct line_camera_frame frame;
	int result = line_camera_read(c->camera, &frame, error);

	if (result < 0)
		return FALSE;
	if (!result)
		return TRUE;
	/* Wake a little early; the encoder enforces the frame rate. */
	c->next_video = now + G_USEC_PER_SEC / c->send_fps / 2;
	struct call_task *task = c->task;

	gboolean preview = FALSE;

	if (g_atomic_int_get(&task->preview_visible) &&
	    line_camera_frame_size_valid(frame.width, frame.height) &&
	    !send_preview(c, LINE_PREVIEW_SELF, &frame, now)) {
		g_mutex_lock(&task->frame_lock);
		/* Copy a preview only once the window took the last one. */
		preview = !task->self_frame ||
			  now - task->self_notified >= G_USEC_PER_SEC;
		g_mutex_unlock(&task->frame_lock);
	}
	if (preview) {
		/* The window shows the local camera beside the peer's picture. */
		GBytes *self = pack_i420(&frame);

		g_mutex_lock(&task->frame_lock);
		gboolean notify = !task->self_frame ||
				  now - task->self_notified >= G_USEC_PER_SEC;
		GBytes *previous = task->self_frame;

		task->self_frame = self;
		task->self_width = frame.width;
		task->self_height = frame.height;
		if (notify)
			task->self_notified = now;
		g_mutex_unlock(&task->frame_lock);
		g_clear_pointer(&previous, g_bytes_unref);
		if (notify)
			line_event("call_self_frame", "chat", c->info.target);
	}
	return call_video_encode(c->video, &frame, now, send_video_packet, c,
				 error);
}

static void activate_video(struct voice_call *c);

static void send_stream_spec(struct voice_call *c,
			     const struct call_endpoint *to);

static gboolean group_camera_state(struct voice_call *c, gboolean streaming);

static void update_camera_capture(struct voice_call *c)
{
	gboolean enabled = c->video && !share_is_video(c) &&
			   g_atomic_int_get(&c->task->camera_enabled);

	if (!enabled && c->camera) {
		line_camera_close(c->camera);
		c->camera = NULL;
	} else if (enabled && !c->camera) {
		g_autoptr(GError) error = NULL;

		c->camera = line_camera_open(c->send_width, c->send_height,
					     c->send_fps, &error);
		if (c->video_trace.enabled)
			g_printerr("line-call camera open=%d%s%s\n",
				   c->camera != NULL, error ? " " : "",
				   error ? error->message : "");
		if (c->camera) {
			call_video_keyframe(c->video);
		} else {
			g_atomic_int_set(&c->task->camera_enabled, FALSE);
			/* The group UI learns video readiness in the connected event. */
			if (c->group &&
			    !g_atomic_int_get(&c->task->group_joined))
				c->video_notice = g_strdup(error->message);
			else
				line_event("call_camera_error", "message",
					   error->message);
		}
	}
}

static void camera_state(struct voice_call *c)
{
	update_camera_capture(c);
	int streaming = c->camera != NULL || share_is_video(c);

	if (!c->audio || !c->video || streaming == c->reported_video)
		return;
	if (c->group) {
		group_camera_state(c, streaming);
		return;
	}
	/*
	 * The official app reports its streams again before starting its
	 * camera. The peer otherwise keeps showing this side's video as paused.
	 */
	if (c->reported_video >= 0) {
		send_stream_spec(c, &c->relay);
		if (c->media_control && !equal_address(&c->relay, &c->media))
			send_stream_spec(c, &c->media);
		if (streaming && (!c->info.video || share_is_video(c)))
			activate_video(c);
	}
	c->reported_video = streaming;
}

static void trace_hex(const char *label, Slice data)
{
	g_autoptr(GString) hex = g_string_new(NULL);

	for (gsize i = 0; i < data.n; i++)
		g_string_append_printf(hex, "%02x", data.p[i]);
	g_printerr("line-call %s %s\n", label, hex->str);
}

/* Prints only the video media records, which carry no key material. */
static void trace_video_description(Slice description)
{
	PbField record;

	while (description.n && pb_next(&description, &record)) {
		if (record.field != 1 || record.type != 2)
			continue;
		Slice name = pb_data(pb_data(record.data, 1), 1);

		if (name.n != 1 || *name.p != 'V')
			continue;
		g_autoptr(GString) hex = g_string_new(NULL);

		for (gsize i = 0; i < record.data.n; i++)
			g_string_append_printf(hex, "%02x", record.data.p[i]);
		g_printerr("line-call video description %s\n", hex->str);
	}
}

/*
 * Send the best picture the peer accepts. The bitrate decides how many
 * pixels it can keep sharp, and the resolution class caps the frame size.
 */
static void choose_send_limits(struct voice_call *c, Slice peer)
{
	struct call_video_limits limits;
	const char *kbps_override = g_getenv("LINE_CALL_VIDEO_KBPS");

	call_video_peer_limits(peer, &limits);
	c->send_kbps = CLAMP(limits.kbps ? limits.kbps : 800, 150, 4000);
	if (kbps_override && *kbps_override)
		c->send_kbps = CLAMP(g_ascii_strtoull(kbps_override, NULL, 10),
				     150, 8000);
	c->send_fps = CLAMP(limits.fps ? limits.fps : 24, 10, 30);
	guint resolution = limits.resolution ? limits.resolution : 2;

	if (resolution >= 3 && c->send_kbps >= 3500)
		c->send_width = 1920, c->send_height = 1080;
	else if (resolution >= 3 && c->send_kbps >= 900)
		c->send_width = 1280, c->send_height = 720;
	else if (resolution >= 2 && c->send_kbps >= 600)
		c->send_width = 960, c->send_height = 540;
	else if (resolution >= 2)
		c->send_width = 640, c->send_height = 480;
	else
		c->send_width = 320, c->send_height = 240;
	guint screen_limit = share_setting("LINE_SHARE_KBPS", 8000, 150, 8000);

	c->screen_kbps =
		CLAMP(limits.kbps ? limits.kbps : 1800, 150, screen_limit);
	c->screen_fps = MIN(c->send_fps, c->screen_kbps < 1200 ? 12u :
					 c->screen_kbps < 2400 ? 20u :
								 30u);
	if (resolution >= 3 && c->screen_kbps >= 2400)
		c->screen_width = 1920, c->screen_height = 1080;
	else if (resolution >= 3 && c->screen_kbps >= 1000)
		c->screen_width = 1280, c->screen_height = 720;
	else if (resolution >= 2 && c->screen_kbps >= 500)
		c->screen_width = 960, c->screen_height = 540;
	else
		c->screen_width = c->send_width,
		c->screen_height = c->send_height;
	if (c->video_trace.enabled)
		g_printerr(
			"line-call video peer accepts kbps=%u fps=%u class=%u; sending up to %ux%u at %u fps, %u kbps\n",
			limits.kbps, limits.fps, limits.resolution,
			c->send_width, c->send_height, c->send_fps,
			c->send_kbps);
	if (c->video_trace.enabled)
		g_printerr(
			"line-call screen encode budget=%ux%u fps=%u kbps=%u\n",
			c->screen_width, c->screen_height, c->screen_fps,
			c->screen_kbps);
}

static gboolean setup_video(struct voice_call *c, Slice offer, Slice key,
			    Slice nonce, guint32 id, GError **error)
{
	guint32 local_ssrc, remote_ssrc;
	guint8 pt;
	Pb answer = { 0 };

	choose_send_limits(c, offer);

	if (c->incoming) {
		call_answer_offer(&answer, &c->info);
		offer = PB_SLICE(answer);
	}
	if (!call_video_profile(offer, &pt, &local_ssrc, &remote_ssrc)) {
		if (c->video_trace.enabled)
			trace_video_description(offer);
		set_error(
			error,
			"The peer did not select the supported LINE VP8 video profile.");
		return FALSE;
	}
	guint32 tx_ssrc = c->incoming ? local_ssrc : remote_ssrc;
	guint32 rx_ssrc = c->incoming ? remote_ssrc : local_ssrc;
	guint8 tx[30] = { 0 }, rx[30] = { 0 };

	if (call_video_keys(c->media_key, c->info.pub, key.p,
			    c->info.media_nonce, nonce.p, c->info.media_id, id,
			    tx, rx))
		c->video = call_video_open(tx, rx, pt, tx_ssrc, rx_ssrc,
					   c->send_kbps, c->send_fps, error);
	OPENSSL_cleanse(tx, sizeof(tx));
	OPENSSL_cleanse(rx, sizeof(rx));
	if (!c->video && (!error || !*error))
		set_error(error, "Could not establish the call's video keys.");
	c->video_pt = pt;
	c->video_rx = rx_ssrc;
	c->video_tx = tx_ssrc;
	return c->video != NULL;
}

/* The 'D' session carries PDTP, which transports screen shares. */
static void setup_data(struct voice_call *c, Slice offer, Slice key,
		       Slice nonce, guint32 id)
{
	guint32 local, remote;
	guint8 pt;
	Pb answer = { 0 };

	if (c->incoming) {
		call_answer_offer(&answer, &c->info);
		offer = PB_SLICE(answer);
	}
	if (c->video_trace.enabled) {
		PbField record;
		Slice rest = offer;

		while (rest.n && pb_next(&rest, &record)) {
			Slice name = pb_data(pb_data(record.data, 1), 1);

			if (record.field == 1 && name.n == 1 && *name.p == 'D')
				trace_hex(c->incoming ?
						  "answered data session" :
						  "answer data session",
					  record.data);
		}
	}
	if (!call_session_rtp(offer, 'D', &pt, &local, &remote))
		return;
	guint32 tx_ssrc = c->incoming ? local : remote;
	guint32 rx_ssrc = c->incoming ? remote : local;
	guint8 tx[30] = { 0 }, rx[30] = { 0 };

	if (call_data_keys(c->media_key, c->info.pub, key.p,
			   c->info.media_nonce, nonce.p, c->info.media_id, id,
			   tx, rx)) {
		c->data = call_pdtp_open(tx, rx, pt, tx_ssrc, rx_ssrc, NULL);
	}
	call_pdtp_set_trace(c->data, c->video_trace.enabled);
	call_pdtp_set_handlers(c->data, data_stream, data_event, c);
	OPENSSL_cleanse(tx, sizeof(tx));
	OPENSSL_cleanse(rx, sizeof(rx));
	c->data_pt = pt;
	c->data_rx = rx_ssrc;
	c->data_tx = tx_ssrc;
	if (c->video_trace.enabled)
		g_printerr(
			"line-call data session pt=%u tx=%u rx=%u ready=%d\n",
			pt, tx_ssrc, rx_ssrc, c->data != NULL);
}

struct audio_stream {
	guint8 pt;
	guint32 sender, receiver;
};

static gboolean audio_profile(Slice offer, struct audio_stream *audio)
{
	PbField session;

	while (offer.n && pb_next(&offer, &session)) {
		if (session.field != 1 || session.type != 2)
			continue;
		Slice head = pb_data(session.data, 1), name = pb_data(head, 1);

		if (name.n != 1 || *name.p != 'A' || !pb_number(head, 3, 1))
			continue;
		Slice rtp = pb_data(session.data, 2);
		guint64 pt = pb_number(rtp, 1, 0);
		guint64 sender = pb_number(rtp, 11, 0);
		guint64 receiver = pb_number(rtp, 61, sender);

		if (pt < 96 || pt > 127 || !sender || sender > G_MAXUINT32 ||
		    !receiver || receiver > G_MAXUINT32)
			return FALSE;
		*audio = (struct audio_stream){ pt, sender, receiver };
		return TRUE;
	}
	return FALSE;
}

static void video_unavailable(struct voice_call *c)
{
	if (!c->video_notice)
		c->video_notice = g_strdup(
			"Video could not be negotiated. Voice is connected.");
	c->info.video = FALSE;
	c->info.video_capable = FALSE;
	g_atomic_int_set(&c->task->camera_enabled, FALSE);
}

/* MC stream control for one media kind: 2 video, 4 PDTP data. */
static void control_streams(struct voice_call *c,
			    const struct call_endpoint *to, guint kind,
			    guint32 rx, guint32 tx, gboolean start)
{
	Pb request = { 0 }, plain = { 0 };
	struct group_video *v = kind == 2 ? c->conference_video : NULL;
	const guint32 streams[] = { GUINT32_TO_BE(rx), GUINT32_TO_BE(tx) };

	enum {
		SENDER = MEDIA_CONTROL_STREAMS + sizeof(streams)
	};

	guint8 control[SENDER + 2 + MID_LENGTH + 1] = {
		[1] = 1, /* Stream control */
		[5] = sizeof(control) - MEDIA_CONTROL_HEADER,
		[9] = start ? 1 : 2, /* START or STOP */
		[11] = G_N_ELEMENTS(streams),
		[15] = kind,
		[SENDER + 1] = MID_LENGTH + 1,
	};

	memcpy(control + MEDIA_CONTROL_STREAMS, streams, sizeof(streams));
	memcpy(control + SENDER + 2, c->info.local, MID_LENGTH + 1);
	pb_uint(&request, 1, 0);
	pb_uint(&request, 2, 0);
	pb_uint(&request, 3, 2);
	pb_bytes(&request, 4, (Slice){ control, sizeof(control) });
	gboolean sent = call_envelope(&plain, &c->info, 4, CALL_GROUP_DATA_REQ,
				      CALL_GROUP_DATA_ID, &request,
				      (Slice){ 0 }) &&
			send_plain(c, to, &plain, FALSE, FALSE);

	if (v) {
		/* A subsequent STOP or START supersedes the previous request. */
		v->camera_start_pending = FALSE;
		if (start && sent) {
			Slice header = pb_data(PB_SLICE(plain), 1);
			Slice id = pb_data(header, 4);

			if (id.n == sizeof(v->camera_start_id)) {
				memcpy(v->camera_start_id, id.p, id.n);
				v->camera_start_sequence =
					pb_number(header, 5, 0);
				v->camera_start_pending = TRUE;
			}
		}
	}

	if (c->video_trace.enabled)
		g_printerr("line-call stream %s kind=%u route=%s sent=%d\n",
			   start ? "start" : "stop", kind,
			   to == &c->relay ? "signaling" : "media", sent);
}

static void start_video_streams(struct voice_call *c,
				const struct call_endpoint *to)
{
	control_streams(c, to, 2, c->video_rx, c->video_tx, TRUE);
	if (c->data)
		control_streams(c, to, 4, c->data_rx, c->data_tx, TRUE);
}

static void activate_video(struct voice_call *c)
{
	if (!c->video)
		return;
	start_video_streams(c, &c->relay);
	if (c->media_control && !equal_address(&c->relay, &c->media))
		c->video_start_retry = g_get_monotonic_time() + G_USEC_PER_SEC;
}

static gboolean ack_stream_start(struct voice_call *c, Slice req, Pb *rsp)
{
	Slice control = pb_data(req, 4);
	const guint8 *p = control.p;

	if (pb_number(req, 3, 0) != 2 || control.n < MEDIA_CONTROL_STREAMS ||
	    p[0] || p[1] != 1 || p[8] || (p[9] != 1 && p[9] != 4) || p[12] ||
	    p[13] || p[14])
		return FALSE;
	guint kind = p[15];
	guint32 rx_id, tx_id;

	if (kind == 1 && c->audio) {
		rx_id = c->audio_rx;
		tx_id = c->audio_tx;
	} else if (kind == 2 && c->video) {
		rx_id = c->video_rx;
		tx_id = c->video_tx;
	} else if (kind == 4 && c->data) {
		rx_id = c->data_rx;
		tx_id = c->data_tx;
	} else {
		return FALSE;
	}
	guint bytes = ((guint)p[4] << 8) | p[5];
	guint count = ((guint)p[10] << 8) | p[11];

	if (bytes > control.n - MEDIA_CONTROL_HEADER || bytes < 12 + count * 4)
		return FALSE;
	/* START and RESUME both begin delivery and receive the same reply. */
	guint8 reply[MEDIA_CONTROL_STREAMS + 8] = {
		[1] = 1,
		[9] = p[9],
		[15] = kind,
	};
	guint seen = 0, matched = 0;

	for (guint i = 0; i < count; i++) {
		const guint8 *stream = p + MEDIA_CONTROL_STREAMS + i * 4;
		guint32 id;

		memcpy(&id, stream, sizeof(id));
		id = GUINT32_FROM_BE(id);
		guint bit = id == rx_id ? 1 : id == tx_id ? 2 : 0;

		if (!bit || (seen & bit))
			continue;
		seen |= bit;
		memcpy(reply + MEDIA_CONTROL_STREAMS + matched++ * 4, stream,
		       4);
	}
	if ((seen & 2) && kind == 2)
		call_video_keyframe(c->video);
	reply[5] = 12 + matched * 4;
	reply[11] = matched;
	if (matched != count)
		reply[19] = 203;
	pb_bytes(rsp, 5, (Slice){ reply, MEDIA_CONTROL_STREAMS + matched * 4 });
	return TRUE;
}

static gboolean setup_media(struct voice_call *c, Slice req, Slice offer,
			    GError **e)
{
	Slice media_offer = offer;
	struct audio_stream peer;
	Slice key = { 0 }, nonce = { 0 };
	guint64 id = G_MAXUINT64;
	PbField record;

	while (offer.n && pb_next(&offer, &record)) {
		if (record.type != 2)
			continue;
		if (record.field == 2) {
			Slice k = pb_data(record.data, 3);

			if (k.n) {
				key = pb_data(k, 1);
				id = pb_number(k, 2, G_MAXUINT64);
				nonce = pb_data(k, 3);
			}
		}
	}
	if (key.n != 33 || nonce.n != 16 || id > G_MAXUINT32 ||
	    !audio_profile(media_offer, &peer)) {
		set_error(e,
			  "The peer did not negotiate encrypted Opus audio.");
		return FALSE;
	}
	c->audio_rx = peer.sender;
	c->audio_tx = peer.receiver;
	if (c->incoming) {
		Pb answer = { 0 };
		struct audio_stream local;

		call_answer_offer(&answer, &c->info);
		if (!audio_profile(PB_SLICE(answer), &local) ||
		    local.pt != peer.pt) {
			set_error(
				e,
				"Could not agree on the call's audio stream.");
			return FALSE;
		}
		c->audio_tx = local.sender;
		c->audio_rx = local.receiver;
	}
	guint8 tx[30] = { 0 }, rx[30] = { 0 };

	if (call_audio_keys(c->media_key, c->info.pub, key.p,
			    c->info.media_nonce, nonce.p, c->info.media_id, id,
			    tx, rx))
		c->audio = line_audio_open(tx, rx, peer.pt, c->audio_tx,
					   c->audio_rx, e);
	OPENSSL_cleanse(tx, sizeof(tx));
	OPENSSL_cleanse(rx, sizeof(rx));
	if (!c->audio) {
		if (!e || !*e)
			set_error(e,
				  "Could not establish the call's media keys.");
		return FALSE;
	}
	g_autoptr(GError) video_error = NULL;

	if (c->info.video_capable) {
		if (!setup_video(c, media_offer, key, nonce, id,
				 &video_error) &&
		    c->info.video)
			video_unavailable(c);
	}
	if (c->info.video_capable)
		setup_data(c, media_offer, key, nonce, id);
	g_atomic_int_set(&c->task->video_ready, c->video != NULL);
	if (c->video_trace.enabled) {
		g_printerr("line-call video negotiated=%d%s%s\n",
			   c->video != NULL, video_error ? " " : "",
			   video_error ? video_error->message : "");
		if (c->video)
			c->video_trace.report_at =
				g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	}
	c->media_control = endpoint_from_addr(c, pb_data(req, 101));
	if (!c->media_control)
		endpoint_from_addr(c, pb_data(req, c->incoming ? 102 : 103));
	c->audio_route.current = c->video_route.current =
		c->data_route.current = c->media;
	c->info.remote_media =
		pb_number(req, c->incoming ? 4 : 2, c->info.remote_channel);
	c->muted = g_atomic_int_get(&c->task->muted);
	line_audio_mute(c->audio, c->muted);
	return TRUE;
}

static Slice stream_spec_data(struct voice_call *c, guint8 *data, gsize size)
{
	Pb spec = { 0 };

	call_stream_spec(&spec, c->audio_rx, c->audio_tx, c->video_rx,
			 c->video_tx, c->data_rx, c->data_tx,
			 video_requested(c));
	g_assert(size >= 8 + sizeof(spec.p) + 3);
	memset(data, 0, 8);
	data[1] = 2; /* Stream report */
	data[4] = spec.n >> 8;
	data[5] = spec.n;
	memcpy(data + 8, spec.p, spec.n);
	memset(data + 8 + spec.n, 0, 3);
	return (Slice){ data, 8 + ((spec.n + 3) & ~(gsize)3) };
}

static void send_stream_spec(struct voice_call *c,
			     const struct call_endpoint *to)
{
	Pb request = { 0 };
	guint8 data[sizeof(request.p) + 12];

	pb_uint(&request, 1, 0);
	pb_uint(&request, 2, 0);
	pb_uint(&request, 3, 2);
	pb_bytes(&request, 4, stream_spec_data(c, data, sizeof(data)));
	gboolean sent =
		send_control_to(c, to, 4, 16, 0x3189, &request, (Slice){ 0 });

	if (c->video_trace.enabled)
		g_printerr(
			"line-call stream report video=%d route=%s sent=%d\n",
			video_requested(c),
			to == &c->relay ? "signaling" : "media", sent);
}

static Slice group_report(const struct voice_call *c, gboolean streaming)
{
	GBytes *bytes = streaming ? c->conference_video->camera_report :
				    c->group_report;
	Slice report;

	report.p = g_bytes_get_data(bytes, &report.n);
	return report;
}

static gboolean mc_request(struct voice_call *c,
			   const struct call_endpoint *source, guint choice,
			   Slice req, Slice header)
{
	Pb rsp = { 0 };

	pb_uint(&rsp, 1, 0);
	if (choice == 16 && c->video_trace.enabled) {
		Slice data = pb_data(req, 4);

		g_printerr(
			"line-call media data dispatch=%u bytes=%zu action=%d kind=%d\n",
			(guint)pb_number(req, 3, 0), data.n,
			data.n >= 12 ? data.p[8] << 8 | data.p[9] : -1,
			data.n >= 16 ? data.p[14] << 8 | data.p[15] : -1);
		trace_hex("media data", data);
	}
	if (choice == 16) {
		pb_uint(&rsp, 2, 0);
		pb_uint(&rsp, 4, pb_number(req, 3, 0));
		if (ack_stream_start(c, req, &rsp))
			return send_control_to(c, source, 4, 17, 0x3289, &rsp,
					       header);
	}

	guint8 data[sizeof(rsp.p) + 12];
	Slice report = c->group ? group_report(c, c->conference_video &&
							  c->reported_video) :
				  stream_spec_data(c, data, sizeof(data));

	if (choice == 16) {
		pb_bytes(&rsp, 5, report);
		return send_control_to(c, source, 4, 17, 0x3289, &rsp, header);
	}
	/* The report wraps StrmSpec in an 8-byte MCMMD header and padding. */
	if (report.n < MEDIA_CONTROL_HEADER)
		return FALSE;
	Slice spec = { report.p + MEDIA_CONTROL_HEADER,
		       ((guint)report.p[4] << 8) | report.p[5] };

	if (spec.n > report.n - MEDIA_CONTROL_HEADER)
		return FALSE;
	pb_bytes(&rsp, 17, spec);
	if (choice == 11)
		return send_control_to(c, source, 4, 12, 0x3286, &rsp, header);
	if (!send_control_to(c, source, 4, 10, 0x3285, &rsp, header))
		return FALSE;
	guint64 bepi_token;

	if (RAND_bytes((guint8 *)&bepi_token, sizeof(bepi_token)) != 1)
		return FALSE;
	Pb token = { 0 }, nested = { 0 }, rpt = { 0 };

	pb_fixed64(&token, 1, bepi_token);
	pb_msg(&nested, 3, &token);
	pb_bytes(&rpt, 17, spec);
	return send_control_to(c, source, 2, 2, 0x1102, &nested,
			       (Slice){ 0 }) &&
	       send_control_to(c, source, 4, 13, 0x3287, &rpt, (Slice){ 0 });
}

static gboolean group_camera_state(struct voice_call *c, gboolean streaming)
{
	Pb request = { 0 };

	pb_uint(&request, 1, 0);
	pb_uint(&request, 2, 0);
	pb_uint(&request, 3, 2);
	pb_bytes(&request, 4, group_report(c, streaming));
	gboolean sent = send_control(c, 4, CALL_GROUP_DATA_REQ,
				     CALL_GROUP_DATA_ID, &request,
				     (Slice){ 0 });

	/* Send STOP only for a previously active camera. */
	if (sent && c->conference_video && (streaming || c->reported_video > 0))
		control_streams(c, &c->relay, 2, c->video_rx, c->video_tx,
				streaming);
	c->reported_video = streaming;
	return sent;
}

static void group_camera_start_reply(struct voice_call *c, Slice header,
				     Slice body)
{
	struct group_video *v = c->conference_video;
	Slice id = pb_data(header, 4);

	if (!v || !v->camera_start_pending ||
	    id.n != sizeof(v->camera_start_id) ||
	    memcmp(id.p, v->camera_start_id, id.n) ||
	    pb_number(header, 5, G_MAXUINT64) != v->camera_start_sequence)
		return;
	v->camera_start_pending = FALSE;

	Slice data = pb_data(body, 5);
	const guint8 *p = data.p;
	guint64 result = pb_number(body, 1, G_MAXUINT64);
	guint64 release = pb_number(body, 2, 0);
	guint64 dispatch = pb_number(body, 4, 0);
	guint count = 0, code = 0;
	gboolean valid = FALSE, rx = FALSE, tx = FALSE;

	if (data.n >= MEDIA_CONTROL_STREAMS) {
		guint len = p[4] * 256u + p[5];
		guint32 kind, status;

		count = p[10] * 256u + p[11];
		memcpy(&kind, p + 12, sizeof(kind));
		memcpy(&status, p + 16, sizeof(status));
		code = ntohl(status);
		valid = !p[0] && p[1] == 1 && !p[2] && !p[3] && !p[8] &&
			p[9] == 1 && ntohl(kind) == 2 &&
			len >= 12 + count * 4 &&
			data.n == MEDIA_CONTROL_HEADER + ((len + 3) & ~3u);
		/* Some replies have no sender after the list of accepted SSRCs. */
		if (valid && len != 12 + count * 4) {
			guint at = MEDIA_CONTROL_STREAMS + count * 4;
			guint trailing = len - 12 - count * 4;

			valid = trailing > 2 &&
				p[at] * 256u + p[at + 1] == trailing - 2 &&
				!p[at + trailing - 1];
		}
		for (guint i = 0; valid && i < count; i++) {
			guint32 ssrc;

			memcpy(&ssrc, p + MEDIA_CONTROL_STREAMS + i * 4,
			       sizeof(ssrc));
			rx |= ntohl(ssrc) == c->video_rx;
			tx |= ntohl(ssrc) == c->video_tx;
		}
	}
	if (c->stats.next)
		g_printerr(
			"group-stats camera-start-reply result=%" G_GINT64_FORMAT
			" release=%" G_GUINT64_FORMAT
			" dispatch=%" G_GUINT64_FORMAT
			" bytes=%zu type=%d valid=%d code=%u sources=%u rx=%d tx=%d\n",
			(gint64)result, release, dispatch, data.n,
			data.n >= 2 ? p[0] * 256 + p[1] : -1, valid, code,
			count, rx, tx);
	/* 203 means at least one start failed. Only successful sources are
	 * listed, so a partial reply can still accept the transmitter. */
	if (!result && !release && dispatch == 2 && valid && tx &&
	    (!code || code == 203))
		call_video_keyframe(c->video);
}

static void setup_group_video(struct voice_call *c,
			      const struct call_group_answer *answer)
{
	g_autoptr(GError) error = NULL;
	guint8 tx[30];

	if (!c->info.video)
		return;
	if (answer->video_supported) {
		choose_send_limits(c, answer->description);
		call_group_media_key("VIDEO", c->info.media_secret, tx);
		c->video = call_video_open_group_tx(tx, answer->video_payload,
						    answer->video_tx,
						    c->send_kbps, c->send_fps,
						    &error);
		OPENSSL_cleanse(tx, sizeof(tx));
	}
	if (!c->video) {
		if (c->video_trace.enabled && error)
			g_printerr("line-call group video: %s\n",
				   error->message);
		video_unavailable(c);
		return;
	}
	/* The selected payload map fixes camera framing for this session.
	 * A viewer START can change resolution, but cannot change the map. */
	if (answer->video_map != CALL_GROUP_VIDEO_VP8)
		call_video_set_group_format(c->video,
					    answer->video_packet_version ?
						    CALL_VIDEO_RTP_EXT_VED :
						    CALL_VIDEO_RTP_EXT_VFD,
					    answer->video_vp8a);
	struct group_video *v = g_new0(struct group_video, 1);
	Pb request = { 0 };

	call_group_register(&request, answer, TRUE);
	Slice report = pb_data(PB_SLICE(request), 4);

	v->members =
		g_array_new(FALSE, FALSE, sizeof(struct call_group_member));
	v->camera_report = g_bytes_new(report.p, report.n);
	v->vp8a = answer->video_vp8a;
	call_group_media_key("VIDEO", answer->secret.p, v->rx_key);
	c->conference_video = v;
	c->video_rx = answer->video_rx;
	c->video_tx = answer->video_tx;
	c->video_pt = answer->video_payload;
	c->video_route.current = c->relay;
	c->reported_video = FALSE;
	if (c->video_trace.enabled)
		c->video_trace.report_at =
			g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	g_atomic_int_set(&c->task->video_ready, TRUE);
}

static gboolean group_peer_matches(const struct call_group_member *a,
				   const struct call_group_member *b)
{
	return a->ssrc == b->ssrc && a->channel == b->channel &&
	       !strcmp(a->mid, b->mid);
}

static void group_clear_frame(struct voice_call *c, const char *mid)
{
	g_autoptr(J) event = obj();
	struct call_task *task = c->task;

	g_mutex_lock(&task->frame_lock);
	g_clear_pointer(&task->frame, g_bytes_unref);
	g_mutex_unlock(&task->frame_lock);
	json_object_object_add(event, "event",
			       json_object_new_string("call_video_peer"));
	json_object_object_add(event, "chat",
			       json_object_new_string(c->info.target));
	json_object_object_add(event, "id", json_object_new_string(mid));
	json_object_object_add(event, "changedAt",
			       json_object_new_int64(g_get_monotonic_time()));
	line_emit(event);
}

static void group_stream_request(struct voice_call *c, gboolean start)
{
	struct group_video *v = c->conference_video;
	Pb request = { 0 }, plain = { 0 };

	call_group_stream_request(&request, v->peer.mid, v->peer.ssrc,
				  v->peer.channel, start, ++v->sequence);
	if (!call_envelope(&plain, &c->info, 4, CALL_GROUP_STRM_REQ,
			   CALL_GROUP_STRM_REQ_ID, &request, (Slice){ 0 }))
		return;
	if (start) {
		Slice header = pb_data(PB_SLICE(plain), 1);
		Slice id = pb_data(header, 4);

		if (id.n != sizeof(v->request_id))
			return;
		memcpy(v->request_id, id.p, sizeof(v->request_id));
		v->request_sequence = pb_number(header, 5, 0);
	}
	send_plain(c, &c->relay, &plain, FALSE, FALSE);
	if (c->video_trace.enabled)
		g_printerr(
			"line-call group video subscription start=%d ssrc=%u\n",
			start, v->peer.ssrc);
}

static void group_select_video(struct voice_call *c)
{
	struct group_video *v = c->conference_video;
	const struct call_group_member *first = NULL, *selected = NULL;
	char focus[MID_LENGTH + 1];

	g_mutex_lock(&c->task->frame_lock);
	memcpy(focus, c->task->video_focus, sizeof(focus));
	g_mutex_unlock(&c->task->frame_lock);
	for (guint i = 0; i < v->members->len; i++) {
		const struct call_group_member *m =
			&g_array_index(v->members, struct call_group_member, i);

		if (!m->active || !m->ssrc || m->ssrc == c->video_tx ||
		    m->ssrc == c->audio_rx || !strcmp(m->mid, c->info.local))
			continue;
		if (!first)
			first = m;
		if (!strcmp(m->mid, focus)) {
			selected = m;
			break;
		}
		if (group_peer_matches(m, &v->peer))
			selected = m;
	}
	if (!selected)
		selected = first;
	if (selected && group_peer_matches(selected, &v->peer))
		return;
	if (!selected && !v->peer.ssrc)
		return;
	if (v->peer.ssrc) {
		group_stream_request(c, FALSE);
		if (c->feedback) {
			call_group_feedback_forget(c->feedback, v->peer.ssrc);
			if (!c->feedback->active_streams)
				c->next_group_feedback = 0;
		}
	}
	g_clear_pointer(&v->receiver, call_video_close);
	v->retry_at = 0;
	v->last_rtp = 0;
	c->next_video_feedback = 0;
	v->peer = (struct call_group_member){ 0 };
	if (selected) {
		v->receiver = call_video_open_group_rx(v->rx_key, c->video_pt,
						       selected->ssrc, NULL);
		if (v->receiver) {
			v->peer = *selected;
			v->attempts = 0;
			v->retry_at = g_get_monotonic_time();
		}
	}
	group_clear_frame(c, v->peer.mid);
}

static void group_presence(struct voice_call *c, Slice body, guint choice)
{
	struct group_video *v = c->conference_video;
	struct call_group_presence presence;

	if (!v || !call_group_presence(body, choice, &presence))
		return;
	if (c->stats.next) {
		guint cameras = 0;
		for (guint i = 0; i < presence.members->len; i++)
			cameras += g_array_index(presence.members,
						 struct call_group_member, i)
					   .active;
		g_printerr(
			"group-stats roster rows=%u wire=%u reported=%u cameras=%u unmapped=%u subgroup=%d full=%d\n",
			presence.members->len, presence.wire_members,
			presence.reported_members, cameras,
			presence.unmapped_cameras, !!presence.channel,
			presence.full);
	}
	g_autoptr(GArray) updates = presence.members;

	/* Camera subscriptions currently cover the main conference. */
	if (presence.channel || (presence.version && v->version &&
				 (gint32)(presence.version - v->version) <= 0))
		return;
	if (presence.version)
		v->version = presence.version;
	if (presence.full)
		g_array_set_size(v->members, 0);
	for (guint i = 0; i < updates->len; i++) {
		struct call_group_member *m =
			&g_array_index(updates, struct call_group_member, i);
		guint j;

		for (j = 0; j < v->members->len; j++) {
			struct call_group_member *old = &g_array_index(
				v->members, struct call_group_member, j);

			if (!strcmp(old->mid, m->mid)) {
				*old = *m;
				break;
			}
		}
		if (j == v->members->len && j < 500)
			g_array_append_val(v->members, *m);
	}
	group_select_video(c);
}

/* The mixer can request a transmit stream again when a viewer joins. */
static gboolean group_tx_request(struct voice_call *c, Slice body, Slice header)
{
	Pb reply = { 0 };
	PbField field;
	guint starts = 0, stops = 0, video = 0, audio = 0, other = 0;
	guint camera_modes = 0, requested_classes = 0;
	int selected = CALL_GROUP_LAYER_UNCHANGED;
	guint32 sequence = 0;

	while (body.n) {
		if (!pb_next(&body, &field))
			return TRUE;
		if (field.field == 2) {
			if (field.type || field.value > G_MAXUINT32)
				return TRUE;
			sequence = field.value;
			continue;
		}
		if (field.field != 1)
			continue;
		if (field.type != 2)
			return TRUE;
		guint64 action = pb_number(field.data, 1, 0);
		guint64 source = pb_number(field.data, 3, 0);

		if (action == 1)
			starts++;
		else if (!action)
			stops++;
		if (source && source == c->video_tx) {
			video++;
			if (action == 1 && c->conference_video) {
				guint64 encoding = pb_number(field.data, 6, 0);
				guint classes;
				int layer = call_group_camera_resolution(
					field.data, c->video_tx,
					c->conference_video->vp8a, &classes);

				requested_classes |= classes;
				if (layer != CALL_GROUP_LAYER_UNCHANGED)
					selected = layer;
				camera_modes |= 1u << MIN(encoding, 31);
				/* START requests a fresh picture. Its encoding option
				 * controls simulcast, not the negotiated RTP format. */
				if (c->camera)
					call_video_keyframe(c->video);
			}
		} else if (source && source == c->audio_tx) {
			audio++;
		} else {
			other++;
		}
	}
	if (selected != CALL_GROUP_LAYER_UNCHANGED) {
		struct group_video *v = c->conference_video;

		if (sequence >= v->layer_sequence) {
			v->layer_sequence = sequence;
			call_video_set_group_resolution(c->video, selected);
		} else {
			selected = CALL_GROUP_LAYER_UNCHANGED;
		}
	}
	if (c->stats.next && c->stats.controls <= 100)
		g_printerr(
			"group-stats tx-request starts=%u stops=%u camera=%u audio=%u other=%u camera-modes=%x requested-classes=%x selected-class=%d descriptor=%u\n",
			starts, stops, video, audio, other, camera_modes,
			requested_classes, selected,
			c->video ? (guint)call_video_group_rtp_extension(
					   c->video) :
				   0);
	/* The native reply acknowledges the request transaction, including
	 * repeats. It carries no per-source receive-state records. */
	pb_uint(&reply, 1, 0);
	pb_uint(&reply, 2, 0);
	return send_control(c, 4, CALL_GROUP_STRM_RSP, CALL_GROUP_STRM_RSP_ID,
			    &reply, header);
}

static void group_stream_updates(struct voice_call *c, Slice body, guint choice,
				 Slice header)
{
	struct group_video *v = c->conference_video;

	if (!v || !v->peer.ssrc)
		return;
	if (choice == CALL_GROUP_STRM_RSP) {
		Slice id = pb_data(header, 4);

		if (id.n != sizeof(v->request_id) ||
		    memcmp(id.p, v->request_id, sizeof(v->request_id)) ||
		    pb_number(header, 5, G_MAXUINT64) != v->request_sequence)
			return;
	}
	g_autoptr(GArray) updates = call_group_stream_updates(body, choice);

	for (guint i = 0; updates && i < updates->len; i++) {
		const struct call_group_stream *stream =
			&g_array_index(updates, struct call_group_stream, i);

		if (stream->ssrc != v->peer.ssrc ||
		    stream->channel != v->peer.channel ||
		    (stream->mid[0] && strcmp(stream->mid, v->peer.mid)))
			continue;
		v->retry_at = 0;
		if (stream->state != CALL_GROUP_STREAM_RESUME)
			group_clear_frame(c, v->peer.mid);
		break;
	}
}

static gboolean join_group(struct voice_call *c,
			   const struct call_group_answer *answer, GError **e)
{
	Pb request = { 0 };
	guint8 tx[30], rx[30];

	if (c->stats.next) {
		PbField record;
		Slice description = answer->description;

		while (description.n && pb_next(&description, &record)) {
			if (record.field != 1 || record.type != 2)
				continue;
			Slice head = pb_data(record.data, 1),
			      name = pb_data(head, 1);
			if (name.n != 1 || !strchr("AVD", *name.p) || !*name.p)
				continue;
			if (*name.p == 'V') {
				PbField item;
				Slice session = record.data;
				while (session.n && pb_next(&session, &item)) {
					if (item.field != 51 || item.type != 2)
						continue;
					guint64 type = pb_number(item.data, 1,
								 G_MAXUINT64);
					if (type == 0 || type == 2)
						g_printerr(
							"group-stats video-feature type=%u value=%u\n",
							(guint)type,
							(guint)pb_number(
								item.data, 2,
								0));
				}
			}
			g_printerr(
				"group-stats session=%c state=%u direction=%u map=%u pt=%u\n",
				*name.p, (guint)pb_number(head, 3, 0),
				(guint)pb_number(head, 2, 0),
				(guint)pb_number(head, 50, 0),
				(guint)pb_number(pb_data(record.data, 2), 1,
						 0));
		}
		g_printerr(
			"group-stats joined video-requested=%d compatible=%d shared-material=%d\n",
			c->info.video, answer->video_supported,
			CRYPTO_memcmp(c->info.media_secret, answer->secret.p,
				      30) == 0);
	}
	c->data_pt = answer->data_payload;
	/* The negotiated data channel carries reliable conference control. */
	if (answer->data_supported) {
		call_group_media_key("DATA", c->info.media_secret, tx);
		call_group_media_key("DATA", answer->secret.p, rx);
		c->data = call_pdtp_open_group(tx, rx, answer->data_payload,
					       answer->data_tx, answer->data_rx,
					       e);
		OPENSSL_cleanse(tx, sizeof(tx));
		OPENSSL_cleanse(rx, sizeof(rx));
		if (!c->data)
			return FALSE;
		c->data_route.current = c->relay;
	}
	/* The local receive agent reports arrivals for the negotiated source. */
	if (answer->feedback_version == 3 && answer->feedback_rx &&
	    g_strcmp0(g_getenv("LINE_EXPERIMENTAL_GROUP_UPLINK"), "0")) {
		c->feedback = g_new0(struct call_group_feedback, 1);
		call_group_feedback_init(c->feedback, answer);
	}
	setup_group_video(c, answer);
	/* Cache the paused report for camera-off joins and later toggles. */
	call_group_register(&request, answer, FALSE);
	Slice report = pb_data(PB_SLICE(request), 4);

	if (request.overflow || !report.n) {
		set_error(e, "Could not construct the group media report.");
		return FALSE;
	}
	c->group_report = g_bytes_new(report.p, report.n);
	c->info.remote_media = answer->media_channel;
	/* The relay's first report reflects whether capture actually opened. */
	update_camera_capture(c);
	if (!group_camera_state(c, c->camera != NULL)) {
		set_error(e, "Could not register media with the group call.");
		return FALSE;
	}
	if (c->stats.next)
		g_printerr("group-stats initial-report camera-active=%d\n",
			   c->reported_video);

	/* The offer seeds the participant's SRTP sender. The conference
	 * answer seeds its receiver, independently of the control transport.
	 */
	call_group_media_key("AUDIO", c->info.media_secret, tx);
	call_group_media_key("AUDIO", answer->secret.p, rx);
	c->audio = line_audio_open_group(tx, rx, answer->audio_payload,
					 answer->audio_payload,
					 answer->audio_tx, answer->audio_rx, e);
	OPENSSL_cleanse(tx, sizeof(tx));
	OPENSSL_cleanse(rx, sizeof(rx));
	OPENSSL_cleanse(c->info.media_secret, sizeof(c->info.media_secret));
	if (!c->audio)
		return FALSE;
	if (c->stats.next)
		line_audio_enable_diagnostics(c->audio);
	c->audio_rx = answer->audio_rx;
	c->audio_tx = answer->audio_tx;
	c->audio_route.current = c->relay;
	c->ka_seconds = answer->keepalive;
	c->next_capture = g_get_monotonic_time();
	c->deadline = c->next_capture + 90 * G_USEC_PER_SEC;
	c->muted = g_atomic_int_get(&c->task->muted);
	line_audio_mute(c->audio, c->muted);
	if (!pinholes(c) || !keepalive(c))
		return FALSE;
	camera_state(c);
	g_atomic_int_set(&c->task->group_joined, TRUE);
	call_connected(c);
	return TRUE;
}

/* Group calls use the conference relay and its assigned media streams. */
static void group_camera_feedback(struct voice_call *c, Slice packet)
{
	struct group_video *v = c->conference_video;
	guint32 sender;

	if (!v || !c->camera || packet.n < 12)
		return;
	memcpy(&sender, packet.p + 4, sizeof(sender));
	sender = ntohl(sender);
	if (!sender || sender == c->video_tx)
		return;
	if (sender != c->video_rx &&
	    (!c->feedback || sender != c->feedback->ssrc)) {
		guint i;

		for (i = 0; i < v->members->len; i++) {
			const struct call_group_member *member = &g_array_index(
				v->members, struct call_group_member, i);

			if (member->ssrc == sender && !member->channel)
				break;
		}
		if (i == v->members->len)
			return;
	}
	call_video_receive_group_feedback(c->video, packet.p, packet.n, sender,
					  g_get_monotonic_time());
}

static gboolean handle_group(struct voice_call *c,
			     const struct call_endpoint *source, Slice plain,
			     GError **e)
{
	CallInfo *s = &c->info;
	Slice hdr = pb_data(plain, 1), session = pb_data(hdr, 3);
	Slice scope = pb_data(plain, 3);
	gboolean media = !scope.p;

	if (media)
		scope = pb_data(plain, 4);
	Slice channel = pb_data(scope, 1);
	Slice cid = pb_data(channel, 1), choice = pb_data(scope, 2);
	guint64 remote = pb_number(channel, 2, 0);
	guint64 local = pb_number(channel, 3, 0);
	guint64 expected_local = media ? s->local_media : s->local_channel;
	guint64 expected_remote = media ? s->remote_media : s->remote_channel;
	guint64 nonce = pb_number(hdr, 6, 0);
	guint64 echo = pb_number(hdr, 7, 0);
	PbField message;

	if (c->stats.next && c->stats.rejected_controls < 32) {
		guint64 id = pb_number(hdr, 2, 0);
		if (id && (!scope.p || (s->have_session &&
					(remote != expected_remote ||
					 (local && local != expected_local) ||
					 (echo && echo != s->nonce) ||
					 nonce != s->remote_nonce)))) {
			g_printerr(
				"group-stats other-control id=%u plane=%u source-match=%d target-match=%d\n",
				(guint)id,
				!scope.p ? 2 :
				media	 ? 4 :
					   3,
				remote == expected_remote,
				local == expected_local);
			c->stats.rejected_controls++;
		}
	}
	if (cid.n != strlen(s->cid) || memcmp(cid.p, s->cid, cid.n) ||
	    !remote || (local && local != expected_local) ||
	    pb_data(hdr, 4).n != 16 || (echo && echo != s->nonce) ||
	    !pb_next(&choice, &message) || message.type != 2 || choice.n)
		return TRUE;
	if (s->have_session &&
	    (session.n != sizeof(s->session) ||
	     memcmp(session.p, s->session, sizeof(s->session)) ||
	     nonce != s->remote_nonce || remote != expected_remote))
		return TRUE;

	if (c->stats.next && c->stats.controls++ < 100)
		g_printerr("group-stats plane=%u control=%u id=%u result=%u\n",
			   media ? 4 : 3, message.field,
			   (guint)pb_number(hdr, 2, 0),
			   media && (message.field == 17 ||
				     message.field == 56) ?
				   (guint)pb_number(message.data, 1, 0) :
				   0);
	if (media) {
		if (!c->audio)
			return TRUE;
		Pb reply = { 0 };

		if (message.field == CALL_GROUP_DATA_RSP) {
			if (pb_number(hdr, 2, 0) == 0x3289 &&
			    equal_address(source, &c->relay))
				group_camera_start_reply(c, hdr, message.data);
			return TRUE;
		}
		/* Dispatch 1 carries stream control and requires no reply. */
		if (message.field == CALL_GROUP_DATA_REQ &&
		    pb_number(message.data, 3, 0) == 1) {
			if (s->have_session && equal_address(source, &c->relay))
				group_camera_feedback(c,
						      pb_data(message.data, 4));
			return TRUE;
		}
		if (message.field == CALL_GROUP_STRM_REQ &&
		    pb_number(hdr, 2, 0) == CALL_GROUP_STRM_REQ_ID)
			return group_tx_request(c, message.data, hdr);
		if (message.field == CALL_GROUP_STRM_RSP ||
		    message.field == CALL_GROUP_NOTIFY_REQ) {
			group_stream_updates(c, message.data, message.field,
					     hdr);
			if (message.field == CALL_GROUP_STRM_RSP)
				return TRUE;
			pb_uint(&reply, 1, 0);
			return send_control(c, 4, CALL_GROUP_NOTIFY_RSP,
					    CALL_GROUP_NOTIFY_RSP_ID, &reply,
					    hdr);
		}
		if (message.field == 9 || message.field == 11 ||
		    message.field == CALL_GROUP_DATA_REQ)
			return mc_request(c, &c->relay, message.field,
					  message.data, hdr);
		return TRUE;
	}
	if (message.field == CALL_GROUP_PARTICIPATE_RSP && !c->audio) {
		struct call_group_answer answer;
		enum call_group_result result =
			call_group_answer(message.data, &answer);

		if (c->stats.next && result != CALL_GROUP_JOINED)
			g_printerr(
				"group-stats participation state=%u result=%u release=%u description=%d channel=%d\n",
				result, (guint)pb_number(message.data, 1, 0),
				(guint)pb_number(message.data, 2, 0),
				!!answer.description.n, !!answer.media_channel);
		if (result == CALL_GROUP_PENDING)
			return TRUE;
		if (result != CALL_GROUP_JOINED) {
			if (result == CALL_GROUP_REJECTED &&
			    pb_number(message.data, 2, 0) ==
				    CALL_GROUP_RELEASE_MULTIDEV_IN_USE) {
				set_error(
					e,
					"LINE sees this account in a call on another device. End that call before joining here.");
			} else if (result == CALL_GROUP_REJECTED)
				g_set_error(
					e, LINE_ERROR, 1,
					"LINE declined the group call (result %u, release %u).",
					(guint)pb_number(message.data, 1, 0),
					(guint)pb_number(message.data, 2, 0));
			else
				set_error(
					e,
					"The group call returned an unsupported media answer.");
			return FALSE;
		}
		if (!nonce || session.n != sizeof(s->session)) {
			set_error(
				e,
				"LINE did not confirm the group call session.");
			return FALSE;
		}
		memcpy(s->session, session.p, sizeof(s->session));
		s->have_session = TRUE;
		s->remote_nonce = nonce;
		s->remote_channel = remote;
		c->invited = TRUE;
		if (c->stats.next)
			g_printerr(
				"group-stats joined role-present=%d stops-present=%d ptt=%d feedback-version=%u feedback-directions=%d/%d data=%d\n",
				!!pb_data(message.data, 13).n,
				!!pb_data(message.data, 103).n,
				!!pb_number(message.data, 105, 0),
				answer.feedback_version, !!answer.feedback_tx,
				!!answer.feedback_rx, answer.data_supported);
		if (!join_group(c, &answer, e))
			return FALSE;
		group_presence(c, message.data, message.field);
		return TRUE;
	}
	if (s->have_session && (message.field == CALL_GROUP_PUSH_REQ ||
				message.field == CALL_GROUP_SUBPUSH_REQ)) {
		Pb reply = { 0 };
		gboolean sub = message.field == CALL_GROUP_SUBPUSH_REQ;

		group_presence(c, message.data, message.field);
		pb_uint(&reply, 1, 0);
		if (sub)
			pb_uint(&reply, 5, pb_number(message.data, 1, 0));
		return send_control(c, 3,
				    sub ? CALL_GROUP_SUBPUSH_RSP :
					  CALL_GROUP_PUSH_RSP,
				    sub ? CALL_GROUP_SUBPUSH_RSP_ID :
					  CALL_GROUP_PUSH_RSP_ID,
				    &reply, hdr);
	}
	if (message.field == 7 && s->have_session) {
		Pb reply = { 0 };

		pb_uint(&reply, 1, 0);
		send_control(c, 3, 8, 0x2245, &reply, hdr);
		c->remote_end = TRUE;
		return FALSE;
	}
	return TRUE;
}

static gboolean handle(struct voice_call *c, const struct call_endpoint *source,
		       Slice plain, GError **e)
{
	if (c->group)
		return handle_group(c, source, plain, e);
	Slice hdr = pb_data(plain, 1), sess = pb_data(hdr, 3),
	      channel = pb_data(plain, 3);
	guint plane = 3;

	if (!channel.p) {
		plane = 4;
		channel = pb_data(plain, 4);
	}
	if (!channel.p || sess.n != 16 || pb_data(hdr, 4).n != 16 ||
	    (plane == 3 && !equal_address(source, &c->relay)))
		return TRUE;
	guint64 local_channel = plane == 4 ? c->info.local_media :
					     c->info.local_channel;
	Slice chan_hdr = pb_data(channel, 1), cid = pb_data(chan_hdr, 1),
	      choice = pb_data(channel, 2);
	if (cid.n != strlen(c->info.cid) || memcmp(cid.p, c->info.cid, cid.n) ||
	    (c->info.have_session && memcmp(sess.p, c->info.session, 16)) ||
	    pb_number(hdr, 7, c->info.nonce) != c->info.nonce ||
	    pb_number(chan_hdr, 3, c->incoming ? 0 : local_channel) !=
		    local_channel)
		return TRUE;
	PbField msg;

	if (!pb_next(&choice, &msg) || msg.type != 2 || choice.n)
		return TRUE;
	if (!c->info.have_session) {
		gboolean first = c->incoming ?
					 msg.field == 4 :
					 (msg.field == 2 || msg.field == 7);
		if (plane != 3 || !first)
			return TRUE;
	}
	if (c->incoming) {
		guint64 remote = pb_number(chan_hdr, 2, 0),
			msg_id = pb_number(hdr, 2, 0);
		if (!remote || pb_number(hdr, 7, 0) != c->info.nonce)
			return TRUE;
		if (!c->verified &&
		    (plane != 3 || msg.field != 4 || msg_id != 0x2242))
			return TRUE;
		if (c->verified &&
		    remote != (plane == 3 ? c->info.remote_channel :
					    c->info.remote_media))
			return TRUE;
		if (plane == 3 && (msg.field == 4 || msg.field == 6)) {
			Slice txn = pb_data(hdr, 4);

			if (memcmp(txn.p, c->expected_id, 16) ||
			    pb_number(hdr, 5, G_MAXUINT64) != c->expected_seq)
				return TRUE;
			if (msg.field == 6 &&
			    (msg_id != 0x2244 || !c->verified))
				return TRUE;
		}
	}
	guint64 peer_nonce = pb_number(hdr, 6, 0);

	if (!peer_nonce ||
	    (c->info.remote_nonce && c->info.remote_nonce != peer_nonce))
		return TRUE;
	if (!c->info.have_session) {
		memcpy(c->info.session, sess.p, 16);
		c->info.have_session = TRUE;
		c->info.remote_nonce = peer_nonce;
	}
	guint64 remote = pb_number(chan_hdr, 2, 0);

	if (plane == 3 && remote)
		c->info.remote_channel = remote;
	if (plane == 4 && remote)
		c->info.remote_media = remote;
	if (c->video_trace.enabled && (plane != 4 || msg.field != 16))
		g_printerr("line-call control plane=%u message=%u\n", plane,
			   msg.field);
	/* Feature-set messages carry the peer's shared camera status. */
	if (c->video_trace.enabled && plane == 3 &&
	    (msg.field == 17 || msg.field == 18))
		trace_hex(msg.field == 17 ? "feature set" : "feature set reply",
			  msg.data);
	if (plane == 4) {
		if (c->audio &&
		    (msg.field == 9 || msg.field == 11 || msg.field == 16))
			return mc_request(c, source, msg.field, msg.data, hdr);
		if (c->video_trace.enabled && msg.field == 17 &&
		    pb_number(msg.data, 4, 0) == 2)
			g_printerr("line-call video control ack=%d\n",
				   pb_number(msg.data, 1, G_MAXUINT64) == 0);
		return TRUE;
	}
	Pb rsp = { 0 };

	switch (msg.field) {
	case 4: { /* VERIFY_RSP for a user-accepted or declined incoming call. */
		if (!c->incoming || c->verified)
			return TRUE;
		if (pb_number(msg.data, 1, 1) != 0 ||
		    pb_number(msg.data, 2, 0)) {
			set_error(e,
				  "LINE could not verify this incoming call.");
			return FALSE;
		}
		c->verified = TRUE;
		c->invited = TRUE;
		if (c->decline)
			return FALSE;
		Slice offer = pb_data(msg.data, 6);
		struct audio_stream audio;
		Slice keys = pb_data(pb_data(offer, 2), 3);

		if (!audio_profile(offer, &audio) || pb_data(keys, 1).n != 33 ||
		    pb_data(keys, 3).n != 16 ||
		    pb_number(keys, 2, G_MAXUINT64) > G_MAXUINT32 ||
		    offer.n > sizeof(c->peer_offer.p)) {
			set_error(
				e,
				"The incoming call did not offer compatible encrypted media.");
			return FALSE;
		}
		c->info.audio_payload = audio.pt;
		/* Answer the data session with the payload type it offered. */
		c->info.data_payload = call_session_payload(offer, 'D');
		if (c->video_trace.enabled) {
			PbField record;
			Slice rest = offer;

			g_printerr("line-call offered data payload=%u\n",
				   c->info.data_payload);
			while (rest.n && pb_next(&rest, &record)) {
				Slice name =
					pb_data(pb_data(record.data, 1), 1);

				if (record.field == 1 && name.n == 1 &&
				    *name.p == 'D')
					trace_hex("offered data session",
						  record.data);
			}
		}
		if (c->info.video_capable) {
			guint8 video_pt;
			guint32 video_ssrc, remote_ssrc;

			if (call_video_offer_profile(offer, &video_pt,
						     &video_ssrc, &remote_ssrc))
				c->info.video_payload = video_pt;
			else if (c->info.video)
				video_unavailable(c);
			else
				c->info.video_capable = FALSE;
		}
		memcpy(c->peer_offer.p, offer.p, offer.n);
		c->peer_offer.n = offer.n;
		Pb answer = { 0 }, ua = { 0 };

		call_answer_offer(&answer, &c->info);
		call_user_agent(&ua);
		pb_msg(&rsp, 1, &answer);
		pb_uint(&rsp, 2, c->info.local_media);
		pb_uint(&rsp, 3, 1);
		pb_uint(&rsp, 4, 120);
		const guint features[] = { 16, 17, 0 };

		for (guint i = 0; i < G_N_ELEMENTS(features); i++) {
			Pb feature = { 0 };

			pb_uint(&feature, 1, features[i]);
			pb_uint(&feature, 2, i == 0);
			pb_uint(&feature, 3, 0);
			pb_msg(&rsp, 8, &feature);
		}
		pb_msg(&rsp, 9, &ua);
		pb_uint(&rsp, 51, 0);
		c->deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
		return send_control(c, 3, 5, 0x2144, &rsp, (Slice){ 0 });
	}
	case 6:
		if (!c->incoming || !c->verified || c->audio || c->decline)
			return TRUE;
		if (pb_number(msg.data, 1, 1) != 0 ||
		    pb_number(msg.data, 2, 0) || !pb_number(msg.data, 4, 0)) {
			set_error(e,
				  "LINE did not connect this incoming call.");
			return FALSE;
		}
		if (!setup_media(c, msg.data, PB_SLICE(c->peer_offer), e))
			return FALSE;
		c->ka_seconds = 10;
		if (!keepalive(c))
			return FALSE;
		if (c->info.video)
			activate_video(c);
		c->deadline = g_get_monotonic_time() + 90 * G_USEC_PER_SEC;
		call_connected(c);
		return TRUE;
	case 2: /* SETUP_RSP */
		if (c->incoming)
			return TRUE;
		if (c->ringing)
			return TRUE;
		if (pb_number(msg.data, 1, 0) != 0 ||
		    pb_number(msg.data, 2, 0) != 0) {
			set_error(e, "LINE declined the call setup.");
			return FALSE;
		}
		c->ka_seconds = CLAMP(pb_number(msg.data, 101, 10), 1, 60);
		if (!pinholes(c) || !keepalive(c))
			return FALSE;
		c->ringing = TRUE;
		c->deadline = g_get_monotonic_time() +
			      CLAMP(pb_number(msg.data, 112, 60), 5, 120) *
				      G_USEC_PER_SEC;
		call_state(c->info.target, "ringing", NULL, c->info.video);
		return TRUE;
	case 5: { /* CONN_REQ, including retransmissions */
		if (!c->ringing)
			return TRUE;
		gboolean first = !c->audio;

		if (first && !setup_media(c, msg.data,
					  pb_data(msg.data, 1).n ?
						  pb_data(msg.data, 1) :
						  pb_data(msg.data, 104),
					  e))
			return FALSE;
		if (!c->video)
			c->info.local_media = c->info.local_channel;
		Pb ua = { 0 };

		call_user_agent(&ua);
		pb_uint(&rsp, 1, 0);
		pb_uint(&rsp, 4, c->info.local_media);
		pb_uint(&rsp, 5, pb_number(msg.data, 3, 1));
		pb_uint(&rsp, 6, pb_number(msg.data, 4, 120));
		pb_msg(&rsp, 7, &ua);
		copy_services(&rsp, msg.data);
		if (!send_control(c, 3, 6, 0x2244, &rsp, hdr))
			return FALSE;
		if (first) {
			if (!exchange(c, msg.data))
				return FALSE;
			if (c->info.video)
				activate_video(c);
			c->deadline =
				g_get_monotonic_time() + 90 * G_USEC_PER_SEC;
			call_connected(c);
		}
		return TRUE;
	}
	case 7:
		pb_uint(&rsp, 1, 0);
		c->remote_end = TRUE;
		send_control(c, 3, 8, 0x2245, &rsp, hdr);
		return FALSE;
	case 20:
	case 21:
		c->remote_end = TRUE;
		return FALSE;
	case 17:
		pb_uint(&rsp, 1, 0);
		for (guint i = 1; i <= 2; i++) {
			Slice v = pb_data(msg.data, i);

			if (v.p)
				pb_bytes(&rsp, i + 3, v);
		}
		copy_services(&rsp, msg.data);
		return send_control(c, 3, 18, 0x2247, &rsp, hdr);
	case 27: { /* BIG_DATA_REQ: fragmented application data. */
		if (c->video_trace.enabled) {
			Slice contents = pb_data(msg.data, 6);

			g_printerr(
				"line-call big data msg=%u fragment=%u/%u type=%u bytes=%zu\n",
				(guint)pb_number(msg.data, 1, 0),
				(guint)pb_number(msg.data, 2, 0),
				(guint)pb_number(msg.data, 3, 0),
				(guint)pb_number(msg.data, 5, 0), contents.n);
			trace_hex("big data", contents);
			g_printerr("line-call big data request id=0x%x\n",
				   (guint)pb_number(hdr, 2, 0));
			trace_hex("big data request", msg.data);
		}
		/* Our own feature-share messages reuse its ID and services. */
		c->big_data_id = (guint)pb_number(hdr, 2, 0);
		c->big_data_type = (guint)pb_number(msg.data, 5, 6);
		c->big_data_services.n = 0;
		copy_services(&c->big_data_services, msg.data);
		pb_uint(&rsp, 1, 0);
		copy_services(&rsp, msg.data);
		/* Replies use the request's message ID plus 0x100. */
		return send_control(c, 3, 28,
				    (guint)pb_number(hdr, 2, 0x214c) + 0x100,
				    &rsp, hdr);
	}
	default:
		return TRUE;
	}
}

static void report_video(const struct voice_call *c)
{
	const struct call_video *video =
		c->conference_video ? c->conference_video->receiver : c->video;

	if (c->data)
		call_pdtp_report(c->data);
	g_printerr(
		"line-call video matched=%u other-ssrc=%u authenticated=%u frames=%u\n",
		c->video_trace.packets, c->video_trace.other_ssrc,
		video ? call_video_authenticated_packets(video) : 0,
		c->video_trace.frames);
	if (video)
		call_video_report(video, FALSE);
}

/* CCFS congestion feedback describes RTP packet sequence ranges. Format 11
 * represents each sequence slot with a one- or two-byte packet element. */
static void group_feedback_blocks(struct voice_call *c, const guint8 *p,
				  gsize size, guint fmt)
{
	gsize offset = 20;

	while (offset + 8 <= size) {
		guint32 id;
		guint received = 0;
		guint count = (p[offset + 4] << 8) | p[offset + 5];
		gsize bytes = 0;

		memcpy(&id, p + offset, sizeof(id));
		id = GUINT32_FROM_BE(id);
		offset += 8;
		if (fmt == 9) {
			bytes = (gsize)count * 2;
			if (bytes > size - offset)
				return;
			/* Fixed-length FMT9 sets bit zero for received packets. */
			for (guint i = 0; i < count; i++)
				received += p[offset + 2 * i] & 1;
		} else {
			for (guint i = 0; i < count; i++) {
				if (bytes >= size - offset)
					return;
				received += !(p[offset + bytes] & 1);
				bytes += 1 + !!(p[offset + bytes] & 2);
			}
		}
		bytes = (bytes + 3) & ~(gsize)3;
		if (bytes > size - offset)
			return;
		if (id && id == c->audio_tx) {
			c->stats.ccfs_audio += count;
			c->stats.mic_acknowledged += received;
		}
		if (id && id == c->video_tx) {
			c->stats.ccfs_camera += count;
			c->stats.camera_acknowledged += received;
		}
		if (id != c->audio_tx && id != c->video_tx)
			c->stats.feedback_other_slots += count;
		offset += bytes;
	}
}

/* Handle negotiated conference feedback from the assigned relay. */
static void group_rtcp(struct voice_call *c, Slice packet, gint64 arrival)
{
	if (c->stats.next)
		c->stats.rtcp++;
	while (packet.n >= 4 && (packet.p[0] >> 6) == 2) {
		const guint8 *p = packet.p;
		gsize len = 4u * (1 + (((guint)p[2] << 8) | p[3]));
		gsize offset = p[1] == 200 ? 28 : 8;
		guint blocks = p[0] & 31;

		if (len > packet.n)
			return;
		if (c->stats.next && p[1] >= 192 && p[1] <= 223)
			c->stats.rtcp_types |= 1u << (p[1] - 192);
		if (c->feedback && p[1] == 205 && blocks == 12) {
			guint8 reply[24];
			gboolean changed;
			gsize n = call_group_feedback_control(
				c->feedback, (Slice){ p, len }, arrival,
				g_get_monotonic_time(), reply, &changed);

			if (changed) {
				guint ms = c->feedback->interval_ms;
				c->next_group_feedback =
					ms && c->feedback->active_streams ?
						arrival + ms * 1000 :
						0;
			}
			if (n)
				send_to(c, &c->relay, (Slice){ reply, n });
		}
		if (c->stats.next && p[1] == 205) {
			c->stats.feedback_formats |= 1u << blocks;
			if (blocks == 5) {
				guint32 target = 0;
				guint count = call_group_retransmit_request(
					(Slice){ p, len }, &target);

				if (target == c->video_tx)
					c->stats.camera_retransmits_requested +=
						count;
				else if (target == c->audio_tx)
					c->stats.mic_retransmits_requested +=
						count;
			}
			if ((blocks == 9 || blocks == 11) && len >= 20)
				group_feedback_blocks(c, p, len, blocks);
		}
		if (c->stats.next && (p[1] == 200 || p[1] == 201) &&
		    offset + blocks * 24 <= len) {
			for (guint i = 0; i < blocks; i++, offset += 24) {
				guint32 id;

				memcpy(&id, p + offset, sizeof(id));
				id = GUINT32_FROM_BE(id);
				c->stats.audio_reports += id &&
							  id == c->audio_tx;
				c->stats.video_reports += id &&
							  id == c->video_tx;
			}
		}
		packet.p += len;
		packet.n -= len;
	}
}

/* Record outer RTP only after the stream has authenticated it. */
static void group_packet_received(struct voice_call *c, const guint8 *packet,
				  gint64 now)
{
	guint16 sequence;
	guint32 timestamp, ssrc;

	c->deadline = now + 90 * G_USEC_PER_SEC;
	if (!c->feedback || !c->feedback->interval_ms)
		return;
	memcpy(&sequence, packet + 2, sizeof(sequence));
	memcpy(&timestamp, packet + 4, sizeof(timestamp));
	memcpy(&ssrc, packet + 8, sizeof(ssrc));
	call_group_feedback_receive(c->feedback, ntohl(ssrc), ntohs(sequence),
				    ntohl(timestamp), now);
	if (!c->next_group_feedback && c->feedback->active_streams)
		c->next_group_feedback = now + 1000 * c->feedback->interval_ms;
}

static gboolean run_call(struct voice_call *c, GError **e)
{
	Pb setup = { 0 };

	c->video_trace.enabled = c->info.video_capable &&
				 !g_strcmp0(g_getenv("LINE_CALL_DEBUG"), "1");
	if (c->group) {
		if (!g_strcmp0(g_getenv("LINE_GROUP_CALL_STATS"), "1")) {
			c->stats.next =
				g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
			g_printerr("group-stats joining video=%d\n",
				   c->info.video);
		}
		call_group_participate(&setup, &c->info, &c->group_route);
		call_group_route_clear(&c->group_route);
	} else if (c->incoming)
		call_verify(&setup, &c->info);
	else
		call_setup(&setup, &c->info);
	if (c->info.token) {
		OPENSSL_cleanse(c->info.token, strlen(c->info.token));
		g_clear_pointer(&c->info.token, g_free);
	}
	if (g_atomic_int_get(&c->task->done))
		return TRUE;
	guint choice = c->group	   ? CALL_GROUP_PARTICIPATE_REQ :
		       c->incoming ? 3 :
				     1;
	guint id = c->group    ? CALL_GROUP_PARTICIPATE_ID :
		   c->incoming ? 0x2142 :
				 0x2141;

	gboolean sent = !setup.overflow &&
			send_control(c, 3, choice, id, &setup, (Slice){ 0 });

	OPENSSL_cleanse(&setup, sizeof(setup));
	if (!sent)
		return FALSE;
	c->invited = !c->group && !c->incoming;
	c->deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	struct pollfd fds[] = { { .fd = c->task->wake, .events = POLLIN },
				{ .fd = c->fd, .events = POLLIN },
				{ .fd = -1, .events = POLLIN },
				{ .fd = -1, .events = POLLIN } };
	while (!g_atomic_int_get(&c->task->done)) {
		gint64 now = g_get_monotonic_time();

		if (now >= c->deadline) {
			set_error(e,
				  c->audio ?
					  "The call connection timed out." :
				  c->ringing ?
					  "The call was not answered." :
					  "LINE's call relay did not respond.");
			return FALSE;
		}
		if (c->ka_seconds && now >= c->next_ka && !keepalive(c))
			return FALSE;
		gint64 next = c->ka_seconds ? MIN(c->deadline, c->next_ka) :
					      c->deadline;
		if (c->audio) {
			gboolean mute = g_atomic_int_get(&c->task->muted);

			if (mute != c->muted) {
				line_audio_mute(c->audio, mute);
				c->muted = mute;
				c->next_capture = now;
			}
			if (!mute && now >= c->next_capture) {
				guint8 data[2048];
				int size = line_audio_capture(c->audio, data,
							      sizeof(data), e);
				if (size < 0 ||
				    (size > 0 &&
				     !send_to(c, &c->audio_route.current,
					      (Slice){ data, (gsize)size })))
					return FALSE;
				if (c->stats.next && size > 0) {
					c->stats.audio_sent++;
					/* Clear RTP extension has the capture level in -dBov. */
					c->stats.audio_active += size > 20 &&
								 data[20] < 60;
				}
				/* Avoid accumulating the poll timeout's rounding error. */
				c->next_capture =
					size ? MAX(now + 4000,
						   c->next_capture +
							   (c->group ? 40000 :
								       20000)) :
					       now + 4000;
			}
			if (!mute)
				next = MIN(next, c->next_capture);
			gint64 playout = line_audio_playout(c->audio, now);

			if (c->group && c->reception != LINE_MEDIA_PLAYABLE)
				group_reception(c,
						line_audio_reception(c->audio));

			next = MIN(next, playout);
		}
		if (c->next_group_feedback) {
			if (now >= c->next_group_feedback) {
				guint8 packet[1200];
				gsize n = call_group_feedback_report(
					c->feedback, now, packet,
					sizeof(packet));
				if (n &&
				    send_to(c, &c->relay,
					    (Slice){ packet, n }) &&
				    c->stats.next)
					c->stats.feedback_sent++;
				c->next_group_feedback =
					n ? now + 1000 * c->feedback->interval_ms :
					    0;
			}
			if (c->next_group_feedback)
				next = MIN(next, c->next_group_feedback);
		}
		if (c->stats.next && now >= c->stats.next) {
			struct group_video *v = c->conference_video;
			g_printerr(
				"group-stats mic-packets=%u mic-above-noise=%u audio-rx=%u video-rtp=%u video-auth=%u frames=%u subscribed=%d muted=%d camera-tx=%u data-rtp=%u\n",
				c->stats.audio_sent, c->stats.audio_active,
				c->stats.audio_played, c->stats.video_packets,
				v && v->receiver ?
					call_video_authenticated_packets(
						v->receiver) :
					0,
				c->stats.frames, !!(v && v->peer.ssrc),
				c->muted, c->stats.camera_sent,
				c->stats.data_packets);
			g_printerr(
				"group-stats transport relay-rtp=%u rtcp=%u mic-rr=%u camera-rr=%u fmt-mask=%x mic-fb-slots=%u camera-fb-slots=%u reports-tx=%u data-accepted=%u data-tx=%u data-setup-acked=%d mic-fb-received=%u camera-fb-received=%u other-fb-slots=%u rtcp-type-mask=%x rtcp-other-port=%u udp-rx=%u control-open=%u unclassified-relay=%u mic-rrr-seqs=%u camera-rrr-seqs=%u\n",
				c->stats.relay_media, c->stats.rtcp,
				c->stats.audio_reports, c->stats.video_reports,
				c->stats.feedback_formats, c->stats.ccfs_audio,
				c->stats.ccfs_camera, c->stats.feedback_sent,
				c->stats.data_accepted, c->stats.data_sent,
				call_pdtp_group_setup_acknowledged(c->data),
				c->stats.mic_acknowledged,
				c->stats.camera_acknowledged,
				c->stats.feedback_other_slots,
				c->stats.rtcp_types, c->stats.rtcp_other_port,
				c->stats.udp_received, c->stats.control_opened,
				c->stats.unclassified_relay,
				c->stats.mic_retransmits_requested,
				c->stats.camera_retransmits_requested);
			line_audio_report(c->audio);
			if (v && v->receiver)
				call_video_report(v->receiver, TRUE);
			c->stats.next = now + 3 * G_USEC_PER_SEC;
		}
		if (c->stats.next)
			next = MIN(next, c->stats.next);
		share_state(c);
		camera_state(c);
		if (c->share_video_repeat && now >= c->share_video_repeat) {
			/* A late viewer can catch up even on a static desktop. */
			if (now >= c->share_video_keyframe) {
				call_video_keyframe(c->video);
				c->share_video_keyframe =
					now + 3 * G_USEC_PER_SEC;
			}
			send_share_video(c, now);
		}
		if (c->share_video_repeat)
			next = MIN(next, c->share_video_repeat);
		fds[3].fd = line_screencast_fd(c->cast);
		fds[2].fd = -1;
		if (c->camera) {
			if (now >= c->next_video)
				fds[2].fd = line_camera_fd(c->camera);
			else
				next = MIN(next, c->next_video);
		}
		struct group_video *group_video = c->conference_video;

		if (group_video && group_video->retry_at) {
			if (now >= group_video->retry_at) {
				group_stream_request(c, TRUE);
				guint attempt = ++group_video->attempts;

				group_video->retry_at =
					attempt < 4 ? now + (G_USEC_PER_SEC
							     << (attempt - 1)) :
						      0;
			}
			if (group_video->retry_at)
				next = MIN(next, group_video->retry_at);
		}
		struct call_video *rx_video =
			group_video ? group_video->receiver : c->video;
		gboolean receiving =
			group_video ? group_video->last_rtp &&
					      now - group_video->last_rtp <
						      3 * G_USEC_PER_SEC :
				      c->info.video || c->camera ||
					      share_is_video(c) ||
					      call_video_authenticated_packets(
						      rx_video);

		if (receiving && call_video_needs_keyframe(rx_video)) {
			if (now >= c->next_video_feedback) {
				call_video_send_peer_feedback(c->video,
							      rx_video,
							      send_video_packet,
							      c);
				c->next_video_feedback = now + G_USEC_PER_SEC;
			}
			next = MIN(next, c->next_video_feedback);
		}
		if (c->video_start_retry) {
			if (now >= c->video_start_retry) {
				start_video_streams(c, &c->media);
				c->video_start_retry = 0;
			} else
				next = MIN(next, c->video_start_retry);
		}
		if (c->video_trace.report_at) {
			if (now >= c->video_trace.report_at) {
				report_video(c);
				c->video_trace.report_at =
					now + 5 * G_USEC_PER_SEC;
			} else
				next = MIN(next, c->video_trace.report_at);
		}
		if (c->data)
			next = MIN(next, call_pdtp_poll(c->data, now,
							send_data_packet, c));
		int timeout = (int)CLAMP((next - now + 999) / 1000, 0, 60000);
		int r = poll(fds, G_N_ELEMENTS(fds), timeout);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return FALSE;
		}
		if (fds[0].revents) {
			eventfd_t n;
			(void)eventfd_read(c->task->wake, &n);
			if (group_video && !g_atomic_int_get(&c->task->done))
				group_select_video(c);
			continue;
		}
		if (fds[3].revents & POLLIN)
			share_capture(c, g_get_monotonic_time());
		if (c->camera && fds[2].revents) {
			g_autoptr(GError) camera_error = NULL;

			if (fds[2].revents & (POLLERR | POLLNVAL | POLLHUP) ||
			    !capture_video(c, g_get_monotonic_time(),
					   &camera_error)) {
				g_atomic_int_set(&c->task->camera_enabled,
						 FALSE);
				line_event(
					"call_camera_error", "message",
					camera_error ?
						camera_error->message :
						"The camera stopped sending video.");
				camera_state(c);
			}
		}
		if (fds[1].revents & (POLLERR | POLLNVAL))
			return FALSE;
		if (!(fds[1].revents & POLLIN))
			continue;
		guint8 data[4096];
		struct call_endpoint source = { .len = sizeof(source.addr) };
		ssize_t n = recvfrom(c->fd, data, sizeof(data), 0,
				     (void *)&source.addr, &source.len);
		if (n <= 0)
			continue;
		Slice pkt = { data, (gsize)n };

		if (c->stats.next)
			c->stats.udp_received++;
		if (c->audio && n >= 12 && (data[0] & 0xc0) == 0x80) {
			guint32 ssrc;

			if (data[1] >= 192 && data[1] <= 223) {
				if (c->stats.next &&
				    !equal_address(&source, &c->relay))
					c->stats.rtcp_other_port++;
				if (c->group &&
				    equal_address(&source, &c->relay)) {
					if (c->feedback || c->stats.next)
						group_rtcp(
							c, pkt,
							g_get_monotonic_time());
					group_camera_feedback(c, pkt);
				} else if (!c->group && c->video &&
					   call_video_receive_feedback(c->video,
								       data, n))
					media_received(c, &c->video_route,
						       &source);
				continue;
			}
			if (c->stats.next && equal_address(&source, &c->relay))
				c->stats.relay_media++;
			memcpy(&ssrc, data + 8, sizeof(ssrc));
			if (c->stats.next && (data[1] & 127) == c->data_pt)
				c->stats.data_packets++;
			if (c->data && call_pdtp_matches(c->data, data, n)) {
				gint64 arrival = g_get_monotonic_time();

				if (call_pdtp_receive(c->data, data, n,
						      arrival)) {
					if (c->group) {
						group_packet_received(c, data,
								      arrival);
						if (c->stats.next)
							c->stats.data_accepted++;
					} else {
						media_received(c,
							       &c->data_route,
							       &source);
					}
				} else if (c->video_trace.enabled &&
					   c->data_traced++ < 100)
					g_printerr(
						"line-call pdtp rx unauthenticated bytes=%zd\n",
						n);
				continue;
			}
			if (c->video_trace.enabled && c->info.video_capable &&
			    (data[1] & 127) != c->video_pt &&
			    GUINT32_FROM_BE(ssrc) != c->audio_rx &&
			    c->data_traced++ < 600)
				g_printerr(
					"line-call rtp other pt=%u ssrc=%u bytes=%zd\n",
					data[1] & 127, GUINT32_FROM_BE(ssrc),
					n);
			struct call_video *receiver =
				group_video ? group_video->receiver : c->video;
			guint32 video_ssrc = group_video ?
						     group_video->peer.ssrc :
						     c->video_rx;
			gboolean video = receiver &&
					 (data[1] & 127) == c->video_pt &&
					 GUINT32_FROM_BE(ssrc) == video_ssrc;
			if (c->video_trace.enabled && c->video &&
			    (data[1] & 127) == c->video_pt) {
				c->video_trace.packets += video;
				c->video_trace.other_ssrc += !video;
			}
			guint before = video && group_video ?
					       call_video_authenticated_packets(
						       receiver) :
					       0;
			gboolean ok =
				video ? call_video_receive(receiver, data, n,
							   display_video, c) :
					line_audio_receive(c->audio, data, n);

			if (video && group_video &&
			    call_video_authenticated_packets(receiver) !=
				    before) {
				group_reception(c, LINE_MEDIA_RECEIVING);
				group_video->last_rtp = g_get_monotonic_time();
				group_packet_received(c, data,
						      group_video->last_rtp);
				group_video->retry_at = 0;
			}
			if (c->stats.next && group_video &&
			    (data[1] & 127) == c->video_pt)
				c->stats.video_packets++;
			if (ok) {
				if (c->stats.next && !video)
					c->stats.audio_played++;
				/* Receive authenticated mixer media from any port. The
				 * microphone and camera use the assigned conference relay. */
				if (c->group) {
					if (!video)
						group_packet_received(
							c, data,
							g_get_monotonic_time());
				} else
					media_received(c,
						       video ? &c->video_route :
							       &c->audio_route,
						       &source);
			}
			continue;
		}
		if (!equal_address(&source, &c->relay)) {
			/*
			 * A connection can negotiate a separate media relay. It may
			 * use the established control keys, but bootstrap and call
			 * signaling still belong to the original relay.
			 */
			if (!c->video || !c->media_control ||
			    !equal_address(&source, &c->media) || pkt.n < 22 ||
			    (data[4] == 6 && data[5] == 2))
				continue;
		}
		Pb plain = { 0 };

		if (!call_wire_open(&c->wire, pkt, &plain)) {
			if (c->stats.next)
				c->stats.unclassified_relay++;
			continue;
		}
		if (c->stats.next)
			c->stats.control_opened++;
		if (c->audio)
			c->deadline =
				g_get_monotonic_time() + 90 * G_USEC_PER_SEC;
		if (!handle(c, &source, PB_SLICE(plain), e))
			return c->remote_end || (c->decline && c->verified);
	}
	return TRUE;
}

static gboolean group_release_reply(struct voice_call *c, Slice plain,
				    Slice request)
{
	const CallInfo *info = &c->info;
	Slice header = pb_data(plain, 1), scope = pb_data(plain, 3);
	Slice channel = pb_data(scope, 1), body = pb_data(scope, 2);
	Slice cid = pb_data(channel, 1), session = pb_data(header, 3),
	      transaction = pb_data(header, 4);
	Slice expected = pb_data(request, 4);
	PbField message;

	return pb_number(header, 2, 0) == 0x2245 &&
	       session.n == sizeof(info->session) &&
	       !memcmp(session.p, info->session, session.n) &&
	       transaction.n == expected.n &&
	       !memcmp(transaction.p, expected.p, transaction.n) &&
	       pb_number(header, 5, G_MAXUINT64) == pb_number(request, 5, 0) &&
	       pb_number(header, 6, 0) == info->remote_nonce &&
	       pb_number(header, 7, info->nonce) == info->nonce &&
	       cid.n == strlen(info->cid) && !memcmp(cid.p, info->cid, cid.n) &&
	       pb_number(channel, 2, 0) == info->remote_channel &&
	       pb_number(channel, 3, 0) == info->local_channel &&
	       pb_next(&body, &message) && !body.n && message.field == 8 &&
	       message.type == 2 &&
	       pb_number(message.data, 1, G_MAXUINT64) == 0 &&
	       pb_number(message.data, 2, 0) == 0;
}

static void release_group(struct voice_call *c, guint media)
{
	Pb release = { 0 }, plain = { 0 };
	struct pollfd ready = { .fd = c->fd, .events = POLLIN };

	pb_uint(&release, 1, 1);
	pb_text(&release, 3, "participant");
	pb_uint(&release, 4, media);
	pb_uint(&release, 6, 0);
	pb_uint(&release, 51, 0);
	if (!call_envelope(&plain, &c->info, 3, 7, 0x2145, &release,
			   (Slice){ 0 }))
		return;

	Slice request = pb_data(PB_SLICE(plain), 1);
	gint64 now = g_get_monotonic_time();
	gint64 deadline = now + GROUP_RELEASE_TIMEOUT_US;
	gint64 retry = now + GROUP_RELEASE_RETRY_US;
	guint attempts = 1;
	gboolean confirmed = FALSE;

	send_plain(c, &c->relay, &plain, FALSE, FALSE);
	while ((now = g_get_monotonic_time()) < deadline) {
		if (now >= retry) {
			/* Preserve the Cassini transaction but use a fresh wire counter. */
			send_plain(c, &c->relay, &plain, FALSE, FALSE);
			attempts++;
			retry = deadline;
		}
		int result = poll(&ready, 1, (retry - now + 999) / 1000);

		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0 ||
		    ready.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (!(ready.revents & POLLIN))
			continue;
		guint8 packet[2048];
		struct call_endpoint from = { .len = sizeof(from.addr) };
		ssize_t size = recvfrom(c->fd, packet, sizeof(packet),
					MSG_DONTWAIT, (void *)&from.addr,
					&from.len);
		Pb decoded = { 0 };

		/* A release response uses the regular encrypted control framing. */
		if (size >= 22 && (packet[0] & 0xf0) == 0xd0 &&
		    (packet[4] & 0xf8) == 0x18 &&
		    equal_address(&from, &c->relay) &&
		    call_wire_open(&c->wire, (Slice){ packet, size },
				   &decoded) &&
		    group_release_reply(c, PB_SLICE(decoded), request)) {
			confirmed = TRUE;
			break;
		}
	}
	if (c->stats.next)
		g_printerr("group-stats leave confirmed=%d attempts=%u\n",
			   confirmed, attempts);
}

static gpointer voice_thread(gpointer data)
{
	struct voice_call *c = data;
	struct call_task *task = c->task;

	line_tune_thread(LINE_THREAD_MEDIA);

	g_autoptr(GError) e = NULL;
	gboolean ok = run_call(c, &e);

	if (c->video_trace.enabled && c->video)
		report_video(c);
	gboolean group_leave = c->group && c->invited && !c->remote_end;
	guint group_media = 0;

	if (group_leave) {
		group_media = (c->audio ? 1 : 0) | (c->video ? 2 : 0) |
			      (c->data ? 4 : 0);
		g_clear_pointer(&c->audio, line_audio_close);
		g_clear_pointer(&c->camera, line_camera_close);
		g_clear_pointer(&c->video, call_video_close);
		g_clear_pointer(&c->data, call_pdtp_close);
	} else if (c->invited && !c->remote_end) {
		Pb rel = { 0 };

		pb_uint(&rel, 1, c->incoming ? 1 : 2);
		pb_text(&rel, 3, c->incoming ? "responder" : "initiator");
		pb_uint(&rel, 4, 1);
		send_control(c, 3, 7, 0x2145, &rel, (Slice){ 0 });
	}
	const char *reason =
		!ok && !g_atomic_int_get(&task->done) ?
			(e ? e->message : "The call relay connection failed.") :
			NULL;
	g_autofree char *chat = g_strdup(c->info.target);

	if (!group_leave)
		free_call(c);
	if (reason)
		line_event("call_error", "message", reason);
	g_atomic_int_set(&task->done, 1);
	call_state(chat, "ended", reason, task->video);
	if (group_leave) {
		release_group(c, group_media);
		free_call(c);
	}
	return NULL;
}

static void signal_task(struct call_task *task)
{
	eventfd_t one = 1;
	(void)eventfd_write(task->wake, one);
}

void line_voice_close(void)
{
	clear_pending();
	if (!active)
		return;
	g_atomic_int_set(&active->done, 1);
	signal_task(active);
	g_thread_join(active->thread);
	close(active->wake);
	g_clear_pointer(&active->frame, g_bytes_unref);
	g_clear_pointer(&active->self_frame, g_bytes_unref);
	g_clear_pointer(&active->share_frame, g_bytes_unref);
	g_mutex_clear(&active->frame_lock);
	g_free(active->target);
	g_free(active->caller);
	g_free(active->incoming_id);
	g_free(active);
	active = NULL;
}

GBytes *line_call_take_frame(const char *chat, int *width, int *height,
			     int *rotation, gboolean *camera_enabled)
{
	if (!active || g_strcmp0(chat, active->target))
		return NULL;
	g_mutex_lock(&active->frame_lock);
	GBytes *frame = g_steal_pointer(&active->frame);

	*width = active->width;
	*height = active->height;
	*rotation = active->rotation;
	if (camera_enabled)
		*camera_enabled = g_atomic_int_get(&active->camera_enabled);
	g_mutex_unlock(&active->frame_lock);
	return frame;
}

GBytes *line_call_take_share_frame(const char *chat, int *width, int *height,
				   int *rotation, gboolean *sharing)
{
	if (!active || g_strcmp0(chat, active->target))
		return NULL;
	g_mutex_lock(&active->frame_lock);
	GBytes *frame = g_steal_pointer(&active->share_frame);

	*width = active->share_width;
	*height = active->share_height;
	*rotation = active->share_rotation;
	if (sharing)
		*sharing = g_atomic_int_get(&active->peer_sharing);
	g_mutex_unlock(&active->frame_lock);
	return frame;
}

GBytes *line_call_take_self_frame(const char *chat, int *width, int *height)
{
	if (!active || g_strcmp0(chat, active->target))
		return NULL;
	g_mutex_lock(&active->frame_lock);
	GBytes *frame = g_atomic_int_get(&active->camera_enabled) ?
				g_steal_pointer(&active->self_frame) :
				NULL;

	g_clear_pointer(&active->self_frame, g_bytes_unref);
	*width = active->self_width;
	*height = active->self_height;
	g_mutex_unlock(&active->frame_lock);
	return frame;
}

static J *result_state(const char *state)
{
	J *out = obj();

	json_object_object_add(out, "state", json_object_new_string(state));
	return out;
}

J *line_voice_hangup(GError **e)
{
	(void)e;
	if (active && active->group) {
		g_atomic_int_set(&active->done, 1);
		signal_task(active);
	} else {
		line_voice_close();
	}
	return result_state("ended");
}

J *line_voice_mute(gboolean muted, GError **e)
{
	if (!active || g_atomic_int_get(&active->done)) {
		set_error(e, "There is no active call.");
		return NULL;
	}
	g_atomic_int_set(&active->muted, muted);
	signal_task(active);
	J *out = obj();

	json_object_object_add(out, "muted", json_object_new_boolean(muted));
	return out;
}

J *line_call_share(gboolean enabled, GError **error)
{
	if (!active || g_atomic_int_get(&active->done)) {
		set_error(error, "There is no active call.");
		return NULL;
	}
	if (active->group) {
		set_error(error,
			  "Screen sharing is unavailable in group calls.");
		return NULL;
	}
	g_atomic_int_set(&active->share_wanted, enabled);
	signal_task(active);
	J *out = obj();

	/* The portal picker opens next; call_share reports the outcome. */
	json_object_object_add(out, "selfSharing",
			       json_object_new_boolean(FALSE));
	json_object_object_add(out, "pending",
			       json_object_new_boolean(enabled));
	return out;
}

J *line_call_camera(gboolean enabled, GError **error)
{
	if (!active || !g_atomic_int_get(&active->video_ready) ||
	    g_atomic_int_get(&active->done)) {
		set_error(error, "There is no active video call.");
		return NULL;
	}
	g_atomic_int_set(&active->camera_enabled, enabled);
	signal_task(active);
	J *out = obj();

	json_object_object_add(out, "cameraEnabled",
			       json_object_new_boolean(enabled));
	return out;
}

J *line_call_video_focus(const char *chat, const char *id, GError **error)
{
	if (!active || !active->group || g_strcmp0(chat, active->target) ||
	    !g_atomic_int_get(&active->video_ready) ||
	    g_atomic_int_get(&active->done) || !valid_mid(id)) {
		set_error(error, "There is no active group video call.");
		return NULL;
	}
	g_mutex_lock(&active->frame_lock);
	memcpy(active->video_focus, id, MID_LENGTH + 1);
	g_mutex_unlock(&active->frame_lock);
	signal_task(active);
	return obj();
}

void line_call_preview(const char *chat, gboolean visible)
{
	if (active && !g_strcmp0(chat, active->target))
		g_atomic_int_set(&active->preview_visible, visible);
}

static gboolean setup_relay(struct voice_call *c, const char *host,
			    const char *host6, int port, const guint8 pub[33],
			    GError **e);

static gboolean setup_route(struct voice_call *c, J *route, GError **e)
{
	const char *token = strfield(route, "1"), *host = strfield(route, "3"),
		   *comm = strfield(route, "12");
	const char *to = strfield(route, "10");
	int port = json_object_get_int(field(route, "4"));

	if (to && *to && strcmp(c->info.target, to)) {
		set_error(e, "LINE returned a route for another contact.");
		return FALSE;
	}
	if (!token || !*token || strlen(token) > 4096 || !comm ||
	    strlen(comm) > 16384 ||
	    json_object_get_boolean(field(route, "8"))) {
		set_error(e, "LINE did not return a usable call route.");
		return FALSE;
	}
	g_autoptr(J) config = json_tokener_parse(comm);
	const char *mpkey = strfield(config, "mpkey");

	if (!mpkey || strlen(mpkey) != 44) {
		set_error(e, "LINE did not supply a PLANET relay key.");
		return FALSE;
	}
	gsize n;
	g_autofree guint8 *pub = g_base64_decode(mpkey, &n);

	if (n != 33 || (pub[0] != 2 && pub[0] != 3)) {
		set_error(e, "LINE returned an invalid relay key.");
		return FALSE;
	}
	CallInfo *s = &c->info;

	s->token = g_strdup(token);
	s->i_zone = g_strdup(strfield(route, "6"));
	s->r_zone = g_strdup(strfield(route, "7"));
	s->stid = g_strdup(strfield(route, "13"));
	return setup_relay(c, host, strfield(route, "17"), port, pub, e);
}

static gboolean setup_relay(struct voice_call *c, const char *host,
			    const char *host6, int port, const guint8 pub[33],
			    GError **e)
{
	for (int pass = 0; pass < 2 && !c->relay.len; pass++) {
		const char *hosts = pass ? host6 : host;

		if (!hosts || strlen(hosts) > 4096)
			continue;
		g_auto(GStrv) list = g_strsplit(hosts, ",", 9);
		for (guint i = 0; list[i] && i < 8; i++)
			if (address(&c->relay, g_strstrip(list[i]), port,
				    AF_UNSPEC, FALSE))
				break;
	}
	if (!c->relay.len) {
		set_error(e, "Could not resolve LINE's voice relay.");
		return FALSE;
	}
	c->media = c->relay;
	c->fd = socket(c->relay.addr.ss_family,
		       SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (c->fd < 0) {
		set_error(e, "Could not open the UDP call socket.");
		return FALSE;
	}
	CallInfo *s = &c->info;

	s->cid = g_uuid_string_random();
	guint32 ids[5];

	if (!call_wire_init(&c->wire, pub) ||
	    (!c->group && !call_keygen(&c->media_key, s->pub)) ||
	    RAND_bytes((guint8 *)ids, sizeof(ids)) != 1 ||
	    RAND_bytes(s->media_nonce, sizeof(s->media_nonce)) != 1 ||
	    RAND_bytes(s->media_secret, sizeof(s->media_secret)) != 1 ||
	    RAND_bytes(s->device, sizeof(s->device)) != 1) {
		set_error(e, "Could not initialize encrypted calling.");
		return FALSE;
	}
	for (guint i = 0; i < G_N_ELEMENTS(ids); i++)
		ids[i] = (ids[i] & 0x1fffffff) | 0x20000000;
	s->media_id = ids[0];
	s->nonce = ids[1];
	s->transaction = ids[3];
	if (c->group) {
		s->local_channel = (ids[2] & 0x07ffffff) | 0x08000000;
		s->local_media = (ids[4] & 0x1fffffff) | 0x40000000;
		return TRUE;
	}
	s->local_channel = ids[2];
	if (c->incoming || s->video_capable) {
		s->local_media = ids[4];
		if (s->local_media == s->local_channel)
			s->local_media++;
	} else
		s->local_media = s->local_channel;
	return TRUE;
}

static J *launch_call(struct voice_call *c, GError **e)
{
	g_autofree char *chat = g_strdup(c->info.target);
	g_autoptr(J) members = g_steal_pointer(&c->group_route.members);
	struct call_task *task = g_new0(struct call_task, 1);

	g_mutex_init(&task->frame_lock);
	task->target = g_strdup(chat);
	task->video = c->info.video;
	task->group = c->group;
	task->camera_enabled = c->info.video;
	task->preview_visible = TRUE;
	task->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	c->task = task;
	if (c->incoming) {
		task->incoming_id = g_strdup(c->info.cid);
		task->caller = g_strdup(c->info.target);
	}
	if (task->wake >= 0)
		task->thread =
			g_thread_try_new("line-voice", voice_thread, c, e);
	if (!task->thread) {
		if (task->wake >= 0)
			close(task->wake);
		else
			set_error(
				e,
				"Could not create the call notification descriptor.");
		free_call(c);
		g_mutex_clear(&task->frame_lock);
		g_free(task->target);
		g_free(task->caller);
		g_free(task->incoming_id);
		g_free(task);
		return NULL;
	}
	active = task;
	J *out = result_state("connecting");

	json_object_object_add(out, "chat", json_object_new_string(chat));
	json_object_object_add(out, "video",
			       json_object_new_boolean(task->video));
	if (members)
		json_object_object_add(out, "participantMids",
				       g_steal_pointer(&members));
	return out;
}

gboolean line_video_enabled(void)
{
	const char *setting = g_getenv("LINE_EXPERIMENTAL_VIDEO");

	return !setting || !strcmp(setting, "1");
}

gboolean line_group_voice_enabled(void)
{
	const char *setting = g_getenv("LINE_EXPERIMENTAL_GROUP_CALLS");

	return !setting || !strcmp(setting, "1");
}

gboolean line_group_video_enabled(void)
{
	const char *setting = g_getenv("LINE_EXPERIMENTAL_GROUP_VIDEO");

	return line_group_voice_enabled() && line_video_enabled() &&
	       (!setting || !strcmp(setting, "1"));
}

const char *line_call_group_chat(gboolean connected)
{
	return active && active->group && !g_atomic_int_get(&active->done) &&
			       (!connected ||
				g_atomic_int_get(&active->group_joined)) ?
		       active->target :
		       NULL;
}

J *line_call_start(Line *s, const char *chat, gboolean video, GError **e)
{
	gboolean group = call_group_id(chat);

	if (group && !line_group_voice_enabled()) {
		set_error(e, "Experimental group calls are disabled.");
		return NULL;
	}
	if (group && video && !line_group_video_enabled()) {
		set_error(e, "Experimental group video calls are disabled.");
		return NULL;
	}
	if (!group && video && !line_video_enabled()) {
		set_error(e, "Experimental video calls are disabled.");
		return NULL;
	}
	if (!s || !valid_mid(s->mid) || (!group && !valid_mid(chat)) ||
	    !strcmp(s->mid, chat)) {
		set_error(e, "Select a LINE user or group to call.");
		return NULL;
	}
	if (active && !g_atomic_int_get(&active->done)) {
		set_error(e, "Another call is already in progress.");
		return NULL;
	}
	line_voice_tick();
	if (pending.route) {
		set_error(e, "Answer or dismiss the incoming call first.");
		return NULL;
	}
	line_voice_close();
	if (group) {
		struct voice_call *c = g_new0(struct voice_call, 1);
		struct call_group_route *route = &c->group_route;

		c->fd = -1;
		c->group = TRUE;
		c->info.video = video;
		c->info.video_capable = video;
		c->info.local = g_strdup(s->mid);
		c->info.target = g_strdup(chat);
		if (!call_group_acquire(s, chat, video, route, e) ||
		    !setup_relay(c, route->relay, route->relay6,
				 route->relay_port, route->relay_key, e)) {
			free_call(c);
			return NULL;
		}
		c->info.token = g_steal_pointer(&route->token);
		c->info.i_zone = g_steal_pointer(&route->i_zone);
		c->info.r_zone = g_steal_pointer(&route->r_zone);
		return launch_call(c, e);
	}
	g_autoptr(J) args = obj();
	J *env = obj();

	json_object_object_add(env, "devname",
			       json_object_new_string("Avernella"));
	json_object_object_add(args, "2:str", json_object_new_string(chat));
	json_object_object_add(args, "3:i32",
			       json_object_new_int(video ? 2 : 1));
	json_object_object_add(args, "4:map:str:str", env);
	g_autoptr(J) route = line_rpc(s, "/V4", "acquireCallRoute", args, e);
	if (!route)
		return NULL;
	struct voice_call *c = g_new0(struct voice_call, 1);

	c->reported_video = -1;

	c->fd = -1;
	c->info.video = video;
	c->info.video_capable = line_video_enabled();
	c->info.local = g_strdup(s->mid);
	c->info.target = g_strdup(chat);
	if (!setup_route(c, route, e)) {
		free_call(c);
		return NULL;
	}
	return launch_call(c, e);
}

J *line_voice_start(Line *s, const char *chat, GError **e)
{
	return line_call_start(s, chat, FALSE, e);
}

static gboolean route_token(const char *s)
{
	if (!s || !*s || strlen(s) > 128)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && *s != '_' && *s != '-')
			return FALSE;
	return TRUE;
}

static gboolean route_hosts(const char *s)
{
	if (!s || !*s || strlen(s) > 1024)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && !strchr(":.,-", *s))
			return FALSE;
	return TRUE;
}

void line_voice_operation(Line *s, J *op)
{
	if (!s || !valid_mid(s->mid))
		return;
	const char *caller = strfield(op, "10"), *token = strfield(op, "11"),
		   *serialized = strfield(op, "12");
	int type = json_object_get_int(field(op, "3"));

	if (type != 50 && type != 51)
		return;
	if (strcmp(s->mid, last_owner)) {
		g_strlcpy(last_owner, s->mid, sizeof(last_owner));
		last_call_revision = 0;
		clear_pending();
	}
	line_voice_tick();
	gint64 revision = json_object_get_int64(field(op, "1"));

	if (revision <= last_call_revision)
		return;
	last_call_revision = revision;
	gint64 created = json_object_get_int64(field(op, "2")),
	       now = g_get_real_time() / 1000;
	if (created <= 0 || created > now + 5000 || created < now - 45000)
		return;
	if (type == 51) {
		/* In the public Talk schema param1 is the call ID and param2 the caller. */
		if (caller && token && pending.route &&
		    !strcmp(caller, pending.id) &&
		    !strcmp(token, pending.caller)) {
			call_state(pending.caller, "ended", NULL,
				   pending.video);
			clear_pending();
		} else if (caller && token && active && active->incoming_id &&
			   !strcmp(caller, active->incoming_id) &&
			   !strcmp(token, active->caller)) {
			line_voice_close();
		}
		return;
	}
	if ((active && !g_atomic_int_get(&active->done)) || pending.route ||
	    !valid_mid(caller) || !strcmp(caller, s->mid) ||
	    !route_token(token) || !serialized || strlen(serialized) > 32768)
		return;
	g_autoptr(J) p = json_tokener_parse(serialized);
	const char *id = strfield(p, "vs"), *v6 = strfield(p, "hv6"),
		   *g = strfield(p, "g"), *sei = strfield(p, "sei");
	const char *kind = strfield(p, "k");
	gboolean video = !g_strcmp0(kind, "CV");

	if ((video ? !line_video_enabled() : g_strcmp0(kind, "CA")) ||
	    (g && *g) || (sei && *sei) || !route_token(id) ||
	    g_strcmp0(strfield(p, "n"), token) ||
	    !route_token(strfield(p, "vfz")) ||
	    !route_token(strfield(p, "vtz")) ||
	    !route_hosts(strfield(p, "h")) || (v6 && *v6 && !route_hosts(v6)))
		return;
	const char *config = strfield(p, "vc");

	if (!config || strlen(config) > 16384)
		return;
	g_autoptr(J) cfg = json_tokener_parse(config);
	const char *key = strfield(cfg, "mpkey");

	if (!key || strlen(key) != 44)
		return;
	gsize len;
	g_autofree guint8 *decoded = g_base64_decode(key, &len);

	if (len != 33 || (decoded[0] != 2 && decoded[0] != 3))
		return;
	J *port_value = field(p, "p");

	if (!json_object_is_type(port_value, json_type_int) &&
	    !json_object_is_type(port_value, json_type_string))
		return;
	const char *port_s = json_object_get_string(port_value);

	if (!port_s || !*port_s || strlen(port_s) > 5)
		return;
	for (const char *i = port_s; *i; i++)
		if (!g_ascii_isdigit(*i))
			return;
	int port = json_object_get_int(port_value);

	if (port <= 0 || port > 65535)
		return;
	if (active)
		line_voice_close();
	pending.route = obj();
	const char *names[][2] = { { "1", token },
				   { "3", strfield(p, "h") },
				   { "6", strfield(p, "vfz") },
				   { "7", strfield(p, "vtz") },
				   { "10", caller },
				   { "12", config },
				   { "17", v6 } };
	for (guint i = 0; i < G_N_ELEMENTS(names); i++)
		if (names[i][1])
			json_object_object_add(
				pending.route, names[i][0],
				json_object_new_string(names[i][1]));
	json_object_object_add(pending.route, "4", json_object_new_int(port));
	pending.video = video;
	pending.caller = g_strdup(caller);
	pending.local = g_strdup(s->mid);
	pending.id = g_strdup(id);
	pending.expires = g_get_monotonic_time() +
			  MIN((gint64)45000, 45000 - (now - created)) * 1000;
	g_autoptr(J) incoming = obj();

	json_object_object_add(incoming, "event",
			       json_object_new_string("call_incoming"));
	json_object_object_add(incoming, "chat",
			       json_object_new_string(caller));
	json_object_object_add(incoming, "video",
			       json_object_new_boolean(video));
	line_emit(incoming);
}

static J *respond(const char *caller, gboolean decline, GError **e)
{
	line_voice_tick();
	if (!pending.route || !caller || strcmp(pending.caller, caller)) {
		set_error(e, "This incoming call has expired or was canceled.");
		return NULL;
	}
	struct voice_call *c = g_new0(struct voice_call, 1);

	c->reported_video = -1;

	c->fd = -1;
	c->incoming = TRUE;
	c->info.video = pending.video;
	c->info.video_capable = line_video_enabled();
	c->decline = decline;
	c->info.local = g_strdup(pending.local);
	c->info.target = g_strdup(pending.caller);
	gboolean ok = setup_route(c, pending.route, e);

	g_free(c->info.cid);
	c->info.cid = g_strdup(pending.id);
	clear_pending();
	if (!ok) {
		call_state(c->info.target, "ended",
			   e && *e ? (*e)->message : NULL, c->info.video);
		free_call(c);
		return NULL;
	}
	return launch_call(c, e);
}

J *line_voice_answer(const char *caller, GError **e)
{
	return respond(caller, FALSE, e);
}

J *line_voice_decline(const char *caller, GError **e)
{
	return respond(caller, TRUE, e);
}
