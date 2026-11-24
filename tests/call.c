// SPDX-License-Identifier: GPL-3.0-or-later
#include "call.h"
#include "call_audio.h"
#include "call_group.h"
#include "call_pdtp.h"
#include "call_srtp.h"
#include "call_schema.h"
#include "call_video.h"
#include "media_io.h"
#include "screencast.h"
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <SDL.h>
#include <srtp2/srtp.h>
#include <opus/opus.h>
#include <openssl/evp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char self[] = "u11111111111111111111111111111111";
static const char peer[] = "u22222222222222222222222222222222";
static const char group[] = "c33333333333333333333333333333333";
static const char other[] = "u44444444444444444444444444444444";
static GAsyncQueue *events;
static int server_fd, server_port, rpc_count;
static EVP_PKEY *relay_key;
static guint8 relay_public[33];
static gint playback_frames;
static gint audio_devices;
static guint microphone_channels;
static gboolean bad_route, video_fixture, voice_fallback, migrate_media;
static gboolean start_with_camera_off;
static gboolean share_fixture, group_fixture, camera_unavailable;
static gboolean peer_share_fixture;
static guint fixture_camera_width = 160;
static gint cameras_opened;
static gint picker_fd = -1;
static gint peer_video_ready;
static int test_camera_fd = -1, preview_fd = -1;
#define audio_active (g_atomic_int_get(&audio_devices) > 0)

J *line_rpc(Line *s, const char *path, const char *method, J *args, GError **e)
{
	(void)e;
	g_assert_cmpstr(s->mid, ==, self);
	g_assert_cmpstr(path, ==, "/V4");
	if (group_fixture) {
		g_assert_cmpstr(strfield(args, "2:str"), ==, group);
		J *reply = obj();

		if (++rpc_count == 1) {
			g_assert_cmpstr(method, ==, "getGroupCall");
			json_object_object_add(reply, "1",
					       json_object_new_boolean(TRUE));
			json_object_object_add(reply, "2",
					       json_object_new_string(group));
			return reply;
		}
		g_assert_cmpint(rpc_count, ==, 2);
		g_assert_cmpstr(method, ==, "acquireGroupCallRoute");
		g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
				video_fixture ? 2 : 1);
		J *caps = field(args, "5:list:str");

		g_assert_cmpuint(json_object_array_length(caps), ==,
				 video_fixture ? 2 : 1);
		if (video_fixture)
			g_assert_cmpstr(
				json_object_get_string(
					json_object_array_get_idx(caps, 1)),
				==, "VIDEO");
		g_assert_false(json_object_get_boolean(field(args, "4:bool")));
		g_autofree char *pub = g_base64_encode(relay_public, 33);
		g_autofree char *config =
			g_strdup_printf("{\"mpkey\":\"%s\"}", pub);
		const char *const paths[] = {
			"1",  "fixture-group-token",
			"7",  "127.0.0.1",
			"10", "zone-a",
			"11", config,
			"12", "127.0.0.1",
			"14", "zone-b",
			"15", "127.0.0.1",
		};

		for (size_t i = 0; i < G_N_ELEMENTS(paths); i += 2)
			json_object_object_add(
				reply, paths[i],
				json_object_new_string(paths[i + 1]));
		json_object_object_add(reply, "8",
				       json_object_new_int(server_port));
		json_object_object_add(reply, "13",
				       json_object_new_int(server_port));
		return reply;
	}
	g_assert_cmpstr(method, ==, "acquireCallRoute");
	g_assert_cmpstr(strfield(args, "2:str"), ==, peer);
	g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
			video_fixture && !start_with_camera_off ? 2 : 1);
	rpc_count++;
	g_autofree char *b64 = g_base64_encode(relay_public, 33);
	J *route = obj();

	g_autoptr(J) comm = obj();
	json_object_object_add(comm, "mpkey", json_object_new_string(b64));
	json_object_object_add(
		route, "12",
		json_object_new_string(json_object_to_json_string(comm)));
	json_object_object_add(route, "10",
			       json_object_new_string(bad_route ? self : peer));
	json_object_object_add(route, "3", json_object_new_string("127.0.0.1"));
	json_object_object_add(route, "4", json_object_new_int(server_port));
	json_object_object_add(route, "1",
			       json_object_new_string("test-only-route-token"));
	return route;
}

static gint self_frames;

void line_event(const char *event, const char *k, const char *v)
{
	(void)k;
	/* Local preview frames arrive independently of the signaling order. */
	if (!g_strcmp0(event, "call_self_frame")) {
		g_atomic_int_inc(&self_frames);
		return;
	}
	g_async_queue_push(events, g_strdup_printf("%s %s", event, v));
}

void line_emit(J *event)
{
	const char *type = strfield(event, "event");

	g_assert_cmpstr(strfield(event, "chat"), ==,
			group_fixture ? group : peer);
	if (!g_strcmp0(type, "call_incoming"))
		line_event(type, "chat", peer);
	else if (!g_strcmp0(type, "call_reception")) {
		g_assert_true(group_fixture);
		int stage = json_object_get_int(field(event, "stage"));

		g_assert_true(stage == LINE_MEDIA_RECEIVING ||
			      stage == LINE_MEDIA_PLAYABLE);
		line_event(type, "stage",
			   stage == LINE_MEDIA_RECEIVING ? "receiving" :
							   "playable");
	} else if (!g_strcmp0(type, "call_video_peer")) {
		g_assert_true(group_fixture && video_fixture);
		g_assert_cmpint(
			json_object_get_int64(field(event, "changedAt")), >, 0);
		line_event(type, "id", strfield(event, "id"));
	} else if (!g_strcmp0(type, "call_share") &&
		   field(event, "peerSharing")) {
		g_assert_true(peer_share_fixture);
		gboolean sharing =
			json_object_get_boolean(field(event, "peerSharing"));

		line_event(type, "state",
			   sharing ? "peer-started" : "peer-stopped");
	} else if (!g_strcmp0(type, "call_share")) {
		gboolean sharing =
			json_object_get_boolean(field(event, "selfSharing"));

		g_assert_true(share_fixture);
		g_assert_cmpint(
			json_object_get_boolean(field(event, "shareInVideo")),
			==, sharing);
		line_event(type, "state", sharing ? "started" : "stopped");
	} else {
		g_assert_cmpstr(type, ==, "call");
		if (!g_strcmp0(strfield(event, "state"), "connected")) {
			g_assert_cmpint(
				json_object_get_boolean(field(event, "video")),
				==, video_fixture && !voice_fallback);
			g_assert_cmpint(json_object_get_boolean(
						field(event, "cameraEnabled")),
					==,
					video_fixture && !voice_fallback &&
						!start_with_camera_off &&
						!camera_unavailable);
			if (camera_unavailable)
				g_assert_cmpstr(strfield(event, "message"), ==,
						"The test camera is busy.");
			if (voice_fallback)
				g_assert_nonnull(strfield(event, "message"));
		}
		line_event("call", "state", strfield(event, "state"));
	}
}

struct line_camera *line_camera_open(unsigned int max_width,
				     unsigned int max_height, unsigned int fps,
				     GError **error)
{
	(void)error;
	/* The fixture peer advertises this client's own HD receive limits. */
	g_assert_cmpuint(max_width, ==, 1920);
	g_assert_cmpuint(max_height, ==, 1080);
	g_assert_cmpuint(fps, ==, 30);
	static const struct itimerspec interval = {
		.it_interval.tv_nsec = 100000000,
		.it_value.tv_nsec = 1000000,
	};

	g_assert_true(video_fixture);
	g_assert_false(voice_fallback);
	if (camera_unavailable) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "The test camera is busy.");
		return NULL;
	}
	test_camera_fd =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	g_assert_cmpint(test_camera_fd, >=, 0);
	g_assert_cmpint(timerfd_settime(test_camera_fd, 0, &interval, NULL), ==,
			0);
	g_atomic_int_inc(&cameras_opened);
	return GINT_TO_POINTER(1);
}

void line_camera_close(struct line_camera *camera)
{
	if (camera) {
		close(test_camera_fd);
		test_camera_fd = -1;
	}
}

int line_camera_fd(const struct line_camera *camera)
{
	g_assert_nonnull(camera);
	return test_camera_fd;
}

static void synthetic_frame(struct line_camera_frame *frame, guint width)
{
	static guint8 y[640 * 480], u[320 * 240], v[320 * 240];

	static gsize initialized;

	if (g_once_init_enter(&initialized)) {
		memset(y, 96, sizeof(y));
		memset(u, 128, sizeof(u));
		memset(v, 128, sizeof(v));
		g_once_init_leave(&initialized, 1);
	}
	*frame = (struct line_camera_frame){
		.width = width,
		.height = width * 3 / 4,
		.plane = { y, u, v },
		.stride = { width, width / 2, width / 2 },
	};
}

int line_camera_read(struct line_camera *camera,
		     struct line_camera_frame *frame, GError **error)
{
	(void)error;
	g_assert_nonnull(camera);
	guint64 n;

	if (read(test_camera_fd, &n, sizeof(n)) != sizeof(n) ||
	    !g_atomic_int_get(&peer_video_ready))
		return 0;
	synthetic_frame(frame, fixture_camera_width);
	return 1;
}

enum {
	SCREEN_WIDTH = 320,
	SCREEN_HEIGHT = 180
};

struct line_screencast {
	int fd;
	gboolean ready, picker;
};

struct line_screencast *line_screencast_start(unsigned int width,
					      unsigned int height,
					      unsigned int fps, GError **error)
{
	(void)error;
	g_assert_true(share_fixture);
	g_assert_cmpuint(width, >=, SCREEN_WIDTH);
	g_assert_cmpuint(height, >=, SCREEN_HEIGHT);
	g_assert_cmpuint(fps, <=, 30);
	struct line_screencast *cast = g_new0(struct line_screencast, 1);
	struct itimerspec first = { .it_value.tv_nsec = 1000000 };

	cast->fd = g_atomic_int_get(&picker_fd);
	if (cast->fd >= 0) {
		cast->picker = TRUE;
		g_atomic_int_set(&picker_fd, -1);
		return cast;
	}
	cast->fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	g_assert_cmpint(cast->fd, >=, 0);
	/* Small regular compositor updates suppress the idle repeat timer. */
	if (group_fixture && camera_unavailable)
		first.it_interval.tv_nsec = 250000000;
	g_assert_cmpint(timerfd_settime(cast->fd, 0, &first, NULL), ==, 0);
	return cast;
}

void line_screencast_stop(struct line_screencast *cast)
{
	if (cast) {
		close(cast->fd);
		g_free(cast);
	}
}

int line_screencast_fd(const struct line_screencast *cast)
{
	return cast ? cast->fd : -1;
}

enum line_screencast_state line_screencast_poll(struct line_screencast *cast,
						char **message)
{
	guint64 ticks;

	cast->ready = read(cast->fd, &ticks, sizeof(ticks)) == sizeof(ticks);
	if (cast->picker) {
		g_assert_true(cast->ready);
		if (ticks == 1) {
			line_event("test_picker", "state", "open");
			return LINE_SCREENCAST_STARTING;
		}
		g_assert_cmpuint(ticks, ==, 2);
		*message = g_strdup("Screen selection canceled.");
		return LINE_SCREENCAST_STOPPED;
	}
	return LINE_SCREENCAST_RUNNING;
}

static GBytes *fixture_screen_pixels(void)
{
	gsize size = line_camera_i420_size(SCREEN_WIDTH, SCREEN_HEIGHT);
	guint8 *pixels = g_malloc(size);

	gsize half = SCREEN_WIDTH * SCREEN_HEIGHT / 2;

	memset(pixels, 168, half);
	memset(pixels + half, 72, half);
	memset(pixels + SCREEN_WIDTH * SCREEN_HEIGHT, 128,
	       size - SCREEN_WIDTH * SCREEN_HEIGHT);
	return g_bytes_new_take(pixels, size);
}

GBytes *line_screencast_take(struct line_screencast *cast, unsigned int *width,
			     unsigned int *height, guint64 *time)
{
	if (!cast->ready)
		return NULL;
	cast->ready = FALSE;
	*width = SCREEN_WIDTH;
	*height = SCREEN_HEIGHT;
	*time = (guint64)g_get_monotonic_time() * 1000;
	return fixture_screen_pixels();
}

SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *device, int capture,
					     const SDL_AudioSpec *desired,
					     SDL_AudioSpec *obtained,
					     int changes);
void __real_SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
int __real_SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);
Uint32 __real_SDL_DequeueAudio(SDL_AudioDeviceID dev, void *data, Uint32 len);

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *name, int input,
					     const SDL_AudioSpec *want,
					     SDL_AudioSpec *got, int flags)
{
	SDL_AudioDeviceID device =
		__real_SDL_OpenAudioDevice(name, input, want, got, flags);
	if (device && input)
		microphone_channels = got ? got->channels : want->channels;
	if (device)
		g_atomic_int_inc(&audio_devices);
	return device;
}

void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID device)
{
	__real_SDL_CloseAudioDevice(device);
	g_atomic_int_add(&audio_devices, -1);
}

/* Replace the dummy microphone's silence with a quiet 200 Hz test signal. */
Uint32 __wrap_SDL_DequeueAudio(SDL_AudioDeviceID device, void *data, Uint32 n)
{
	Uint32 bytes = __real_SDL_DequeueAudio(device, data, n);
	gint16 *pcm = data;
	static guint phase;

	if (!group_fixture)
		return bytes;
	g_assert_cmpuint(microphone_channels, >=, 1);
	for (guint i = microphone_channels - 1; i < bytes / sizeof(*pcm);
	     i += microphone_channels) {
		int sample = 24 * (MIN((int)phase, 240 - (int)phase) - 60);

		pcm[i] = sample;
		phase = (phase + 1) % 240;
	}
	return bytes;
}

int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void *data, Uint32 n)
{
	const gint16 *pcm = data;

	for (guint i = 0; i < n / sizeof(*pcm); i++)
		if (ABS((int)pcm[i]) > 64) {
			g_atomic_int_inc(&playback_frames);
			break;
		}
	return __real_SDL_QueueAudio(device, data, n);
}

static void expect_event(const char *msg)
{
	g_autofree char *actual = g_async_queue_timeout_pop(events, 2000000);

	g_assert_cmpstr(actual, ==, msg);
}

static void frame_vector(void)
{
	static const char want[] =
		"d026ab1d181423ad9bcc442f1550effcbc2e43400a30f360e3d65dfd8e782ed8df537d1e5e051ab338e8";
	CallWire w = { .seq = 0xab1d, .have_rx = TRUE };

	for (guint i = 0; i < 64; i++)
		w.tx[i] = w.rx[i] = i;
	Pb plain = { .p = "local PLANET fixture", .n = 20 }, packet = { 0 },
	   decoded = { 0 };
	g_assert_true(call_wire_pack(&w, &plain, FALSE, FALSE, &packet));
	g_assert_cmpuint(packet.n * 2, ==, strlen(want));
	for (guint i = 0; i < packet.n; i++) {
		guint8 byte = (g_ascii_xdigit_value(want[2 * i]) << 4) |
			      g_ascii_xdigit_value(want[2 * i + 1]);
		g_assert_cmpuint(packet.p[i], ==, byte);
	}
	g_assert_true(call_wire_open(&w, PB_SLICE(packet), &decoded));
	g_assert_cmpmem(decoded.p, decoded.n, plain.p, plain.n);
	packet.p[packet.n - 1] ^= 1;
	g_assert_false(call_wire_open(&w, PB_SLICE(packet), &decoded));
	w.count = 65536;
	g_assert_false(call_wire_pack(&w, &plain, FALSE, FALSE, &packet));
}

/*
 * Independent OpenSSL HMAC-based relay implementation of the published SHA512
 * schedule. It authenticates the actual UDP bytes emitted by a call worker.
 */
static void relay_kdf(Slice salt, Slice key, Slice info, guint8 out[64])
{
	guint8 prk[64], label[37] = { 0 };
	size_t n = 0;

	g_assert_nonnull(EVP_Q_mac(NULL, "HMAC", NULL, "SHA512", NULL, key.p,
				   key.n, salt.p, salt.n, prk, 64, &n));
	g_assert_cmpuint(info.n, <=, sizeof(label) - 4);
	memcpy(label, info.p, info.n);
	g_assert_nonnull(EVP_Q_mac(NULL, "HMAC", NULL, "SHA512", NULL, prk, 64,
				   label, info.n + 4, out, 64, &n));
}

static void init_server(CallWire *w, const guint8 client_pub[33])
{
	guint8 shared[32], base[64];

	g_assert_true(call_shared(relay_key, client_pub, shared));
	relay_kdf((Slice){ shared, 32 }, (Slice){ relay_public, 33 },
		  (Slice){ client_pub, 33 }, w->rx_base);
	relay_kdf((Slice){ shared, 32 }, (Slice){ client_pub, 33 },
		  (Slice){ relay_public, 33 }, base);
	memset(w->prefix, 0xaa, 18);
	memcpy(w->prefix + 18, relay_public, 33);
	w->seq = 17;
	relay_kdf((Slice){ base, 64 }, (Slice){ w->prefix + 2, 16 },
		  (Slice){ w->prefix, 2 }, w->tx);
}

struct fake_relay {
	CallWire wire;
	CallInfo info;
	Pb camera_start;
	struct sockaddr_in remote;
	socklen_t remote_len;
};

static Pb receive_packet(struct fake_relay *r)
{
	struct pollfd pfd = { .fd = server_fd, .events = POLLIN };

	g_assert_cmpint(poll(&pfd, 1, 2000), ==, 1);
	Pb pkt = { 0 };

	r->remote_len = sizeof(r->remote);
	ssize_t n = recvfrom(server_fd, pkt.p, sizeof(pkt.p), 0,
			     (void *)&r->remote, &r->remote_len);
	g_assert_cmpint(n, >=, 22);
	pkt.n = n;
	return pkt;
}

static void emit_peer(struct fake_relay *r, guint msg, guint id, Pb *body,
		      gboolean first)
{
	Pb plain = { 0 }, packet = { 0 };

	g_assert_true(call_envelope(&plain, &r->info, 3, msg, id, body,
				    (Slice){ 0 }));
	g_assert_true(call_wire_pack(&r->wire, &plain, first, FALSE, &packet));
	g_assert_cmpint(sendto(server_fd, packet.p, packet.n, 0,
			       (void *)&r->remote, r->remote_len),
			==, (int)packet.n);
}

static void setup_server(void)
{
	g_assert_true(call_keygen(&relay_key, relay_public));
	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
	g_assert_cmpint(server_fd, >=, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET,
				    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	g_assert_cmpint(bind(server_fd, (void *)&addr, sizeof(addr)), ==, 0);
	socklen_t n = sizeof(addr);

	g_assert_cmpint(getsockname(server_fd, (void *)&addr, &n), ==, 0);
	server_port = ntohs(addr.sin_port);
}

static void start_outgoing(struct fake_relay *r, Pb *plain)
{
	Line s = { .mid = (char *)self };

	g_autoptr(GError) e = NULL;
	g_autoptr(J) started = line_call_start(
		&s, peer, video_fixture && !start_with_camera_off, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(strfield(started, "state"), ==, "connecting");
	Pb pkt = receive_packet(r);

	init_server(&r->wire, pkt.p + 24);
	g_assert_true(call_wire_open(&r->wire, PB_SLICE(pkt), plain));
	Slice hdr = pb_data(PB_SLICE(*plain), 1),
	      cc = pb_data(PB_SLICE(*plain), 3), ch = pb_data(cc, 1);
	Slice cid = pb_data(ch, 1);

	r->info.cid = g_strndup((const char *)cid.p, cid.n);
	r->info.local = (char *)peer;
	r->info.local_channel = 0x1001;
	r->info.nonce = 0x2001;
	r->info.remote_nonce = pb_number(hdr, 6, 0);
	r->info.remote_channel = pb_number(ch, 2, 0);
	memset(r->info.session, 0xcc, 16);
	r->info.have_session = TRUE;
	g_assert_cmpuint(pb_number(hdr, 2, 0), ==, 0x2141);
	g_assert_false(audio_active);
}

static srtp_t peer_context(guint8 key[30], gboolean sending)
{
	srtp_policy_t p = { 0 };
	srtp_t ctx = NULL;

	srtp_crypto_policy_set_rtp_default(&p.rtp);
	srtp_crypto_policy_set_rtcp_default(&p.rtcp);
	p.ssrc.type = sending ? ssrc_any_outbound : ssrc_any_inbound;
	p.key = key;
	p.window_size = 128;
	g_assert_cmpint(srtp_create(&ctx, &p), ==, srtp_err_status_ok);
	return ctx;
}

static gboolean readable_until(int fd, gint64 deadline)
{
	struct pollfd input = { .fd = fd, .events = POLLIN };

	for (;;) {
		gint64 left = deadline - g_get_monotonic_time();
		int rc;

		if (left <= 0)
			return FALSE;
		rc = poll(&input, 1, (left + 999) / 1000);
		if (rc < 0 && errno == EINTR)
			continue;
		g_assert_cmpint(rc, >=, 0);
		if (rc)
			g_assert_true(input.revents & POLLIN);
		return rc != 0;
	}
}

static void expect_audio(int fd, srtp_t ctx, guint8 pt, guint32 ssrc)
{
	guint8 packet[2048];
	int n = 0;
	gint64 expires = g_get_monotonic_time() + 2000000;

	while (readable_until(fd, expires)) {
		n = recv(fd, packet, sizeof(packet), 0);
		if (n > 22 && (packet[0] >> 6) == 2 && (packet[1] & 127) == pt)
			break;
		n = 0;
	}

	g_assert_cmpint(n, >, 22);
	g_assert_cmpint(srtp_unprotect(ctx, packet, &n), ==,
			srtp_err_status_ok);
	guint32 id;

	memcpy(&id, packet + 8, sizeof(id));
	g_assert_cmpuint(ntohl(id), ==, ssrc);
	g_assert_cmpuint(packet[12], ==, 0);
	g_assert_cmpint(opus_packet_get_nb_samples(packet + 13, n - 13, 48000),
			==, 960);
}

static void send_audio(int fd, const struct sockaddr_in *client, srtp_t ctx,
		       guint8 pt, guint32 ssrc, guint16 seq, gboolean corrupt)
{
	int rc = 0;
	OpusEncoder *encoder =
		opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &rc);
	guint8 packet[2048] = { 0x80, pt, seq >> 8, seq };
	guint32 id = htonl(ssrc);
	guint32 stamp = htonl((guint32)seq * 960);

	memcpy(packet + 4, &stamp, sizeof(stamp));
	memcpy(packet + 8, &id, sizeof(id));
	g_assert_cmpint(rc, ==, OPUS_OK);
	opus_int16 tone[960];

	for (guint i = 0; i < 960; i++)
		tone[i] = (i % 80 < 40 ? 4000 : -4000);
	int n = opus_encode(encoder, tone, 960, packet + 13, 1200);

	opus_encoder_destroy(encoder);
	g_assert_cmpint(n, >, 0);
	n += 13;
	g_assert_cmpint(srtp_protect(ctx, packet, &n), ==, srtp_err_status_ok);
	if (corrupt)
		packet[n - 1] ^= 1;
	g_assert_cmpint(sendto(fd, packet, n, 0, (const void *)client,
			       sizeof(*client)),
			==, n);
}

static void expect_playback(int before)
{
	gint64 end = g_get_monotonic_time() + 2000000;

	while (g_atomic_int_get(&playback_frames) <= before &&
	       g_get_monotonic_time() < end)
		g_usleep(1000);
	g_assert_cmpint(g_atomic_int_get(&playback_frames), >, before);
}

/* Exercise the relay's media channel before testing the user's microphone. */
static void group_audio_control(struct fake_relay *r, guint choice,
				guint action, guint32 rx, guint32 tx)
{
	guint8 control[24] = {
		[1] = 1, [5] = 16, [9] = action, [11] = 1, [15] = 1
	};
	guint32 id = htonl(tx);
	Pb request = { 0 }, plain = { 0 }, wire = { 0 };
	guint msg = choice == 9			  ? 0x3185 :
		    choice == 11		  ? 0x3186 :
		    choice == CALL_GROUP_STRM_REQ ? CALL_GROUP_STRM_REQ_ID :
						    0x3189;

	memcpy(control + 20, &id, sizeof(id));
	if (choice == CALL_GROUP_STRM_REQ)
		call_group_stream_request(&request, self, tx, 0, TRUE, 1);
	if (choice == CALL_GROUP_DATA_REQ) {
		pb_uint(&request, 3, 2);
		pb_bytes(&request, 4, (Slice){ control, sizeof(control) });
	}
	g_assert_true(call_envelope(&plain, &r->info, 4, choice, msg, &request,
				    (Slice){ 0 }));
	Slice transaction = pb_data(pb_data(PB_SLICE(plain), 1), 4);

	g_assert_true(call_wire_pack(&r->wire, &plain, FALSE, FALSE, &wire));
	g_assert_cmpint(sendto(server_fd, wire.p, wire.n, 0,
			       (const void *)&r->remote, r->remote_len),
			==, (ssize_t)wire.n);
	gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

	while (readable_until(server_fd, deadline)) {
		Pb packet = receive_packet(r), reply = { 0 };

		if (!call_wire_open(&r->wire, PB_SLICE(packet), &reply))
			continue;
		Slice scope = pb_data(PB_SLICE(reply), 4);
		Slice body = pb_data(pb_data(scope, 2), choice + 1);
		Slice hdr = pb_data(PB_SLICE(reply), 1);
		Slice echoed = pb_data(hdr, 4);

		if (!body.n || echoed.n != transaction.n ||
		    memcmp(echoed.p, transaction.p, echoed.n))
			continue;
		g_assert_cmpuint(pb_number(hdr, 2, 0), ==, msg + 0x100);
		if (choice == CALL_GROUP_STRM_REQ) {
			g_assert_cmpuint(pb_number(body, 1, 1), ==, 0);
			g_assert_cmpuint(pb_number(body, 2, 1), ==, 0);
			g_assert_null(pb_data(body, 4).p);
		} else if (choice == CALL_GROUP_DATA_REQ) {
			Slice ack = pb_data(body, 5);

			g_assert_cmpmem(ack.p, ack.n, control, sizeof(control));
		} else {
			Slice spec = pb_data(body, 17);

			g_assert_cmpuint(pb_number(pb_data(spec, 1), 1, 0), ==,
					 rx);
			g_assert_cmpuint(pb_number(pb_data(spec, 6), 1, 0), ==,
					 tx);
		}
		return;
	}
	g_error("No group audio control reply for %u", msg);
}

static void group_video_exchange(struct fake_relay *relay, const guint8 *local,
				 const guint8 *remote, guint8 pt,
				 guint32 tx_ssrc, gboolean trial,
				 guint32 accepted_ssrc,
				 enum call_video_rtp_extension format);
static void group_share_exchange(struct fake_relay *relay, const guint8 *local,
				 guint8 pt, guint32 rx_ssrc, guint32 ssrc);
static void receive_presentation(int fd, struct fake_relay *relay,
				 const guint8 key[30], guint8 pt, guint32 ssrc,
				 const char *chat, srtp_t feedback);
static void expect_camera_control(struct fake_relay *relay, guint action,
				  guint32 tx_ssrc, gboolean expect_report);

static void assert_tx_paused(Slice report, guint32 ssrc, gboolean paused)
{
	PbField stream;

	g_assert_cmpuint(report.n, >=, 8);
	g_assert_cmpuint(report.p[1], ==, 2);
	guint length = report.p[4] * 256u + report.p[5];

	g_assert_cmpuint(length, <=, report.n - 8);
	Slice spec = { report.p + 8, length };

	while (spec.n) {
		g_assert_true(pb_next(&spec, &stream));
		if (stream.field == 6 && pb_number(stream.data, 1, 0) == ssrc) {
			g_assert_cmpuint(pb_number(pb_data(stream.data, 2), 1,
						   2),
					 ==, paused);
			return;
		}
	}
	g_error("No transmit state for the fixture source");
}

/* A group relay sends data control independently of microphone activity.
 * Check both feedback transports over the call's actual UDP socket. */
static void group_data_exchange(struct fake_relay *r, const guint8 *local,
				const guint8 *remote, guint8 pt,
				guint32 rx_ssrc, guint32 tx_ssrc)
{
	static const guint8 open[] = { 0x80, 0,	   'L',	 'I',  'N',  'E',
				       0,    0x02, 0x08, 2,    0x40, 0xc8,
				       5,    1,	   0x40, 0x6e, 0x12, 0 };
	guint8 tx[30], rx[30], packet[2048] = { 0x90, 0x80 | pt, 0, 42 };
	guint32 source = htonl(rx_ssrc);
	gboolean ack = FALSE, feedback = FALSE;
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	int n = 16 + sizeof(open);

	call_group_media_key("DATA", local, rx);
	call_group_media_key("DATA", remote, tx);
	srtp_t relay_tx = peer_context(tx, TRUE),
	       relay_rx = peer_context(rx, FALSE);

	memcpy(packet + 8, &source, 4);
	packet[12] = 2;
	packet[13] = 0x40;
	memcpy(packet + 16, open, sizeof(open));
	g_assert_cmpint(srtp_protect(relay_tx, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpint(sendto(server_fd, packet, n, 0,
			       (const void *)&r->remote, r->remote_len),
			==, n);
	while ((!ack || !feedback) && readable_until(server_fd, end)) {
		n = recv(server_fd, packet, sizeof(packet), 0);
		if (n >= 32 && packet[0] == 0x8b && packet[1] == 205) {
			g_assert_cmpmem(packet + 4, 8,
					"\x00\x00\x00\x51\x00\x00\x00\x51", 8);
			int off = 20;

			while (off + 8 <= n) {
				guint count = (packet[off + 4] << 8) |
					      packet[off + 5];
				guint16 first = (packet[off + 6] << 8) |
						packet[off + 7];
				guint32 id;
				memcpy(&id, packet + off, 4);
				off += 8;
				for (guint j = 0; j < count; j++) {
					g_assert_cmpint(off, <, n);
					if (id == source &&
					    (guint16)(first + j) == 42 &&
					    !(packet[off] & 1))
						feedback = TRUE;
					off += 1 + !!(packet[off] & 2);
				}
				off = (off + 3) & ~3;
			}
		} else if (n >= 26 && packet[0] == 0x90 &&
			   (packet[1] & 127) == pt) {
			guint32 id;

			g_assert_cmpint(srtp_unprotect(relay_rx, packet, &n),
					==, srtp_err_status_ok);
			memcpy(&id, packet + 8, 4);
			g_assert_cmpuint(ntohl(id), ==, tx_ssrc);
			g_assert_cmpint(n, >=, 21);
			g_assert_cmpmem(packet + 12, 4, "\x02\x40\x00\x00", 4);
			/* Other reliable D-channel packets can precede our ACK. */
			if (packet[19] != 0x40 || packet[20] != 0)
				continue;
			g_assert_cmpint(n, >=, 26);
			g_assert_cmpint(packet[22], ==, 42);
			ack = TRUE;
		}
	}
	g_assert_true(ack);
	g_assert_true(feedback);
	srtp_dealloc(relay_tx);
	srtp_dealloc(relay_rx);
}

static Pb expect_group_release(struct fake_relay *relay, guint media)
{
	gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC;

	while (readable_until(server_fd, end)) {
		guint8 packet[2048];
		int n = recv(server_fd, packet, sizeof(packet), 0);
		Pb decoded = { 0 };

		if (n <= 0 || !call_wire_open(&relay->wire,
					      (Slice){ packet, n }, &decoded))
			continue;
		Slice plain = PB_SLICE(decoded), cc = pb_data(plain, 3);
		Slice release = pb_data(pb_data(cc, 2), 7);

		if (!release.p)
			continue;
		Slice releaser = pb_data(release, 3);

		g_assert_cmpuint(pb_number(pb_data(plain, 1), 2, 0), ==,
				 0x2145);
		g_assert_cmpuint(pb_number(release, 1, 0), ==, 1);
		g_assert_cmpmem(releaser.p, releaser.n, "participant", 11);
		g_assert_cmpuint(pb_number(release, 4, 0), ==, media);
		g_assert_cmpuint(pb_number(release, 6, 1), ==, 0);
		g_assert_cmpuint(pb_number(release, 51, 1), ==, 0);
		return decoded;
	}
	g_error("Fixture relay did not receive the group release request");
}

static void group_release_ack(struct fake_relay *relay, Slice request,
			      guint result, guint release)
{
	Pb body = { 0 }, plain = { 0 }, wire = { 0 };

	pb_uint(&body, 1, result);
	if (release)
		pb_uint(&body, 2, release);
	g_assert_true(call_envelope(&plain, &relay->info, 3, 8, 0x2245, &body,
				    request));
	g_assert_true(
		call_wire_pack(&relay->wire, &plain, FALSE, FALSE, &wire));
	g_assert_cmpint(sendto(server_fd, wire.p, wire.n, 0,
			       (void *)&relay->remote, relay->remote_len),
			==, (int)wire.n);
}

enum {
	FIRST_PRESENTER = 801,
	UNKNOWN_PRESENTER = 802,
	NEXT_PRESENTER = 803
};

static void fixture_member(Pb *presence, const char *mid, const char *media,
			   guint32 ssrc)
{
	Pb member = { 0 }, source = { 0 };

	pb_text(&member, 1, mid);
	pb_uint(&member, 2, 3);
	pb_uint(&member, 3, 1);
	pb_text(&source, 1, media);
	pb_uint(&source, 2, ssrc);
	pb_msg(&member, 10, &source);
	pb_msg(presence, 50, &member);
}

static void group_call(gconstpointer args)
{
	guint mode = GPOINTER_TO_UINT(args);

	if (!g_test_subprocess()) {
		GTestSubprocessFlags flags = G_TEST_SUBPROCESS_DEFAULT;

		if (mode == 12 &&
		    !g_strcmp0(g_getenv("AVERNELLA_TEST_MEDIA_CONTRACT"), "1"))
			flags = G_TEST_SUBPROCESS_INHERIT_STDOUT;
		g_test_trap_subprocess(NULL, 10000000, flags);
		g_test_trap_assert_passed();
		return;
	}
	enum {
		RX_SSRC = 501,
		TX_SSRC = 601,
		RTP_PT = 110
	};
	Line session = { .mid = (char *)self };
	struct fake_relay relay = { 0 };
	g_autoptr(GError) error = NULL;

	g_assert_true(line_group_voice_enabled());
	g_setenv("LINE_EXPERIMENTAL_GROUP_CALLS", "0", TRUE);
	g_assert_false(line_group_voice_enabled());
	g_assert_null(line_call_start(&session, group, FALSE, &error));
	g_assert_cmpint(rpc_count, ==, 0);
	g_clear_error(&error);
	g_unsetenv("LINE_EXPERIMENTAL_GROUP_CALLS");
	g_unsetenv("LINE_EXPERIMENTAL_VIDEO");
	g_setenv("LINE_EXPERIMENTAL_GROUP_VIDEO", "0", TRUE);
	g_assert_false(line_group_video_enabled());
	g_assert_null(line_call_start(&session, group, TRUE, &error));
	g_assert_cmpint(rpc_count, ==, 0);
	g_clear_error(&error);
	g_unsetenv("LINE_EXPERIMENTAL_GROUP_VIDEO");
	g_assert_true(line_group_video_enabled());
	group_fixture = TRUE;
	gboolean trial = mode == 3 || mode == 4;
	enum call_video_rtp_extension format = CALL_VIDEO_RTP_EXT_NONE;
	guint map = CALL_GROUP_VIDEO_VP8;
	if (mode == 8) {
		format = CALL_VIDEO_RTP_EXT_VED;
		map = CALL_GROUP_VIDEO_SVC;
	} else if (mode == 9) {
		format = CALL_VIDEO_RTP_EXT_VFD;
		map = CALL_GROUP_VIDEO_SVC_SW;
	}
	video_fixture = mode != 0 && mode != 3 && mode != 13;
	voice_fallback = mode == 2;
	camera_unavailable = mode == 6 || mode == 11;
	share_fixture = mode == 10 || mode == 11;
	peer_share_fixture = mode == 12 || mode == 13;
	if (share_fixture)
		g_setenv("LINE_SHARE_TRANSPORT", "presentation", TRUE);
	fixture_camera_width = mode == 8 ? 640 : 160;
	if (trial)
		g_setenv("LINE_EXPERIMENTAL_GROUP_UPLINK", "1", TRUE);
	else
		g_unsetenv("LINE_EXPERIMENTAL_GROUP_UPLINK");
	setup_server();
	g_autoptr(J) started =
		line_call_start(&session, group, video_fixture, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(strfield(started, "state"), ==, "connecting");
	g_assert_cmpint(rpc_count, ==, 2);
	Pb wire = receive_packet(&relay), plain = { 0 };

	init_server(&relay.wire, wire.p + 24);
	g_assert_true(call_wire_open(&relay.wire, PB_SLICE(wire), &plain));
	Slice header = pb_data(PB_SLICE(plain), 1);
	Slice scope = pb_data(PB_SLICE(plain), 3);
	Slice chan = pb_data(scope, 1), cid = pb_data(chan, 1);
	Slice req = pb_data(pb_data(scope, 2), CALL_GROUP_PARTICIPATE_REQ);
	Slice offer = pb_data(req, 11);
	Slice local_key = pb_data(pb_data(pb_data(offer, 2), 2), 1);

	g_assert_cmpuint(pb_number(header, 2, 0), ==,
			 CALL_GROUP_PARTICIPATE_ID);
	g_assert_cmpuint(pb_number(req, 9, 0), ==, video_fixture ? 3 : 1);
	g_assert_cmpuint(local_key.n, ==, 30);
	g_assert_false(audio_active);
	relay.info.cid = g_strndup((const char *)cid.p, cid.n);
	relay.info.local = (char *)peer;
	relay.info.local_channel = 101;
	relay.info.local_media = 102;
	relay.info.remote_channel = pb_number(chan, 2, 0);
	relay.info.remote_nonce = pb_number(header, 6, 0);
	relay.info.nonce = 103;
	relay.info.have_session = TRUE;
	memset(relay.info.session, 0xaf, sizeof(relay.info.session));

	Pb provisional = { 0 };
	emit_peer(&relay, CALL_GROUP_PARTICIPATE_RSP, 0x224e, &provisional,
		  TRUE);
	g_assert_false(
		readable_until(server_fd, g_get_monotonic_time() + 40000));
	g_assert_false(audio_active);
	if (mode == 5) {
		Pb refusal = { 0 };

		pb_uint(&refusal, 1, 1);
		pb_uint(&refusal, 2, 6);
		emit_peer(&relay, CALL_GROUP_PARTICIPATE_RSP, 0x224e, &refusal,
			  FALSE);
		expect_event(
			"call_error LINE sees this account in a call on another device. End that call before joining here.");
		expect_event("call ended");
		line_voice_close();
		g_assert_false(audio_active);
		g_assert_cmpint(cameras_opened, ==, 0);
		g_free(relay.info.cid);
		EVP_PKEY_free(relay_key);
		close(server_fd);
		return;
	}
	static const guint8 remote_key[30] = { 1, 7, 12 };
	Pb response = { 0 }, desc = { 0 }, security = { 0 }, secret = { 0 };
	const char *const labels[] = { "A", "V", "D" };

	for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
		Pb stream = { 0 }, codec = { 0 }, path = { 0 };

		pb_text(&codec, 1, labels[i]);
		if (!video_fixture && i == 0) {
			Pb recv = { 0 }, send = { 0 }, feature = { 0 };

			pb_uint(&recv, 1, 67);
			pb_uint(&recv, 2, 0x51);
			pb_uint(&send, 1, 67);
			pb_uint(&send, 2, 0x52);
			pb_msg(&path, 12, &recv);
			pb_msg(&path, 62, &send);
			pb_uint(&feature, 1, 0);
			pb_uint(&feature, 2, 3);
			pb_msg(&desc, 3, &feature);
		}
		if (i == 2) {
			pb_uint(&codec, 2, 3);
			pb_uint(&codec, 3, !voice_fallback);
			pb_uint(&codec, 50, 6);
		}
		if (i == 1 && video_fixture && !voice_fallback) {
			Pb version = { 0 };

			pb_uint(&codec, 2, 3);
			pb_uint(&codec, 3, 1);
			pb_uint(&codec, 4, 4000);
			pb_uint(&codec, 5, 30);
			pb_uint(&codec, 6, 3);
			pb_uint(&codec, 50, map);
			if (format != CALL_VIDEO_RTP_EXT_NONE) {
				Pb vp8 = { 0 };

				pb_uint(&vp8, 1, 0);
				pb_uint(&vp8, 2,
					format == CALL_VIDEO_RTP_EXT_VED ? 2 :
									   1);
				pb_msg(&stream, 51, &vp8);
			}
			pb_uint(&version, 1, 2);
			pb_uint(&version, 2, 1);
			if (mode != 7 && mode != 9)
				pb_msg(&stream, 51, &version);
		}
		pb_uint(&path, 1, RTP_PT + i);
		pb_uint(&path, 11, RX_SSRC + i);
		pb_uint(&path, 61, TX_SSRC + i);
		pb_msg(&stream, 1, &codec);
		pb_msg(&stream, 2, &path);
		pb_msg(&desc, 1, &stream);
	}
	pb_bytes(&secret, 1, (Slice){ remote_key, sizeof(remote_key) });
	pb_msg(&security, 2, &secret);
	pb_msg(&desc, 2, &security);
	pb_uint(&response, 1, 0);
	pb_msg(&response, 6, &desc);
	pb_uint(&response, 7, relay.info.local_media);
	pb_uint(&response, 102, 5);
	emit_peer(&relay, CALL_GROUP_PARTICIPATE_RSP, 0x224e, &response, FALSE);
	expect_event("call connected");
	g_assert_true(audio_active);

	Pb registration = receive_packet(&relay), control = { 0 };

	g_assert_true(
		call_wire_open(&relay.wire, PB_SLICE(registration), &control));
	Slice media = pb_data(PB_SLICE(control), 4);
	Slice media_channel = pb_data(media, 1);
	Slice registration_body =
		pb_data(pb_data(media, 2), CALL_GROUP_DATA_REQ);

	g_assert_cmpuint(pb_number(media_channel, 3, 0), ==,
			 relay.info.local_media);
	g_assert_nonnull(registration_body.p);
	Slice initial_report = pb_data(registration_body, 4);
	gboolean camera_running = video_fixture && !voice_fallback &&
				  !camera_unavailable;

	assert_tx_paused(initial_report, TX_SSRC, FALSE);
	assert_tx_paused(initial_report, TX_SSRC + 1, !camera_running);
	relay.info.remote_media = pb_number(media_channel, 2, 0);
	if (camera_running)
		expect_camera_control(&relay, 1, TX_SSRC + 1, FALSE);
	group_audio_control(&relay, 9, 0, RX_SSRC, TX_SSRC);
	group_audio_control(&relay, 11, 0, RX_SSRC, TX_SSRC);
	group_audio_control(&relay, CALL_GROUP_STRM_REQ, 1, RX_SSRC, TX_SSRC);
	group_audio_control(&relay, CALL_GROUP_DATA_REQ, 1, RX_SSRC, TX_SSRC);
	group_audio_control(&relay, CALL_GROUP_DATA_REQ, 4, RX_SSRC, TX_SSRC);
	Pb query = { 0 }, query_plain = { 0 }, query_wire = { 0 };

	pb_uint(&query, 3, 2);
	g_assert_true(call_envelope(&query_plain, &relay.info, 4,
				    CALL_GROUP_DATA_REQ, CALL_GROUP_DATA_ID,
				    &query, (Slice){ 0 }));
	g_assert_true(call_wire_pack(&relay.wire, &query_plain, FALSE, FALSE,
				     &query_wire));
	g_assert_cmpint(sendto(server_fd, query_wire.p, query_wire.n, 0,
			       (void *)&relay.remote, relay.remote_len),
			==, (int)query_wire.n);

	guint8 tx[30], rx[30], packet[2048];
	call_group_media_key("AUDIO", local_key.p, tx);
	call_group_media_key("AUDIO", remote_key, rx);
	srtp_t sink = peer_context(tx, FALSE), source = peer_context(rx, TRUE);
	gboolean heard = FALSE, acknowledged = FALSE;
	int rc;
	OpusDecoder *listener = opus_decoder_create(48000, 1, &rc);
	gint64 end = g_get_monotonic_time() + 2000000;

	g_assert_cmpint(rc, ==, OPUS_OK);
	while ((!heard || !acknowledged) && readable_until(server_fd, end)) {
		int n = recv(server_fd, packet, sizeof(packet), 0);

		if (n <= 24)
			continue;
		if (packet[0] != 0x90 || (packet[1] & 127) != RTP_PT) {
			Pb decoded = { 0 };

			if (!call_wire_open(&relay.wire, (Slice){ packet, n },
					    &decoded))
				continue;
			Slice mc = pb_data(PB_SLICE(decoded), 4);
			Slice ack =
				pb_data(pb_data(mc, 2), CALL_GROUP_DATA_RSP);

			if (ack.p) {
				g_assert_cmpuint(pb_number(ack, 1, 1), ==, 0);
				g_assert_cmpuint(pb_number(ack, 4, 0), ==, 2);
				acknowledged = TRUE;
			}
			continue;
		}
		g_assert_cmpint(srtp_unprotect(sink, packet, &n), ==,
				srtp_err_status_ok);
		guint32 ssrc;

		memcpy(&ssrc, packet + 8, sizeof(ssrc));
		g_assert_cmpuint(ntohl(ssrc), ==, TX_SSRC);
		g_assert_cmpint(n, >, 28);
		/* Strip the EAS2 voice bitmap to decode the two Opus frames. */
		g_assert_cmpuint(packet[25] & 3, ==, 3);
		g_assert_cmphex(packet[25] & 0x80, ==, 0x80);
		gboolean voiced = ((packet[19] >> 4) & 3) == 2 ||
				  ((packet[19] >> 2) & 3) == 2;
		gint16 pcm[1920];

		packet[27] = packet[26];
		packet[26] = packet[25];
		g_assert_cmpint(opus_decode(listener, packet + 26, n - 26, pcm,
					    G_N_ELEMENTS(pcm), 0),
				==, G_N_ELEMENTS(pcm));
		for (guint i = 0; voiced && i < G_N_ELEMENTS(pcm); i++)
			if (ABS((int)pcm[i]) > 100) {
				heard = TRUE;
				break;
			}
	}
	opus_decoder_destroy(listener);
	g_assert_true(heard);
	g_assert_true(acknowledged);
	int before = g_atomic_int_get(&playback_frames);

	if (!mode) {
		/* This packet authenticates but carries no decodable EAS2 frame. */
		guint8 empty[64] = { 0x80, RTP_PT };
		guint32 id = htonl(RX_SSRC);
		int size = 13;

		memcpy(empty + 8, &id, sizeof(id));
		g_assert_cmpint(srtp_protect(source, empty, &size), ==,
				srtp_err_status_ok);
		g_assert_cmpint(sendto(server_fd, empty, size, 0,
				       (const void *)&relay.remote,
				       relay.remote_len),
				==, size);
		expect_event("call_reception receiving");
		g_assert_cmpint(g_atomic_int_get(&playback_frames), ==, before);
	}
	send_audio(server_fd, &relay.remote, source, RTP_PT, RX_SSRC, 1, FALSE);
	expect_playback(before);
	expect_event("call_reception playable");
	int mixer = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	g_assert_cmpint(mixer, >=, 0);
	before = g_atomic_int_get(&playback_frames);
	send_audio(mixer, &relay.remote, source, RTP_PT, RX_SSRC, 2, TRUE);
	g_assert_false(readable_until(mixer, g_get_monotonic_time() + 60000));
	g_assert_cmpint(g_atomic_int_get(&playback_frames), ==, before);
	send_audio(mixer, &relay.remote, source, RTP_PT, RX_SSRC, 3, FALSE);
	expect_playback(before);
	g_assert_false(readable_until(mixer, g_get_monotonic_time() + 60000));
	close(mixer);
	if (!video_fixture && !peer_share_fixture)
		group_data_exchange(&relay, local_key.p, remote_key, RTP_PT + 2,
				    RX_SSRC + 2, TX_SSRC + 2);
	if (peer_share_fixture) {
		guint8 key[30], local_data[30];
		srtp_t feedback;

		call_group_media_key("DATA", remote_key, key);
		call_group_media_key("DATA", local_key.p, local_data);
		g_assert_true(call_srtp_open(&feedback, local_data,
					     ssrc_specific, TX_SSRC + 2));
		receive_presentation(server_fd, &relay, key, RTP_PT + 2,
				     RX_SSRC + 2, group, feedback);
		srtp_dealloc(feedback);
	} else if (share_fixture)
		group_share_exchange(&relay, local_key.p, RTP_PT + 1,
				     RX_SSRC + 1, TX_SSRC + 1);
	else if (video_fixture && !voice_fallback && !camera_unavailable)
		group_video_exchange(&relay, local_key.p, remote_key,
				     RTP_PT + 1, TX_SSRC + 1, trial,
				     mode == 7 ? RX_SSRC + 1 : TX_SSRC + 1,
				     format);
	g_autoptr(J) muted = line_voice_mute(TRUE, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(muted, "muted")));
	if (!video_fixture || voice_fallback) {
		g_assert_null(line_call_share(TRUE, &error));
		g_assert_error(error, LINE_ERROR, 1);
		g_clear_error(&error);
	}
	gint64 leaving = g_get_monotonic_time();
	g_autoptr(J) left = line_voice_hangup(&error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(left, "state"), ==, "ended");
	expect_event("call ended");
	g_assert_false(audio_active);
	guint media_mask = 1;

	if (!voice_fallback)
		media_mask |= 4 | (video_fixture ? 2 : 0);
	Pb request = expect_group_release(&relay, media_mask);
	Slice release_header = pb_data(PB_SLICE(request), 1);

	if (mode == 0) {
		/* An authenticated response for another transaction must not end this one. */
		Slice txn = pb_data(release_header, 4);
		guint8 other[16];
		Pb wrong = { 0 };

		g_assert_cmpuint(txn.n, ==, sizeof(other));
		memcpy(other, txn.p, sizeof(other));
		other[0] ^= 1;
		pb_bytes(&wrong, 4, (Slice){ other, sizeof(other) });
		pb_uint(&wrong, 5, pb_number(release_header, 5, 0));
		send_audio(server_fd, &relay.remote, source, RTP_PT, RX_SSRC, 4,
			   FALSE);
		group_release_ack(&relay, PB_SLICE(wrong), 0, 0);
		group_release_ack(&relay, release_header, 1, 0);
		group_release_ack(&relay, release_header, 0, 1);
		Pb retry = expect_group_release(&relay, media_mask);

		g_assert_cmpmem(request.p, request.n, retry.p, retry.n);
	}
	g_assert_cmpint(g_get_monotonic_time() - leaving, <, G_USEC_PER_SEC);
	if (mode != 2)
		group_release_ack(&relay, release_header, 0, 0);
	line_voice_close();
	gint64 elapsed = g_get_monotonic_time() - leaving;

	if (mode == 2) {
		/* The local devices were closed before the unreachable relay timed out. */
		g_assert_cmpint(elapsed, >=, 1900000);
		g_assert_cmpint(elapsed, <, 2700000);
	} else {
		g_assert_cmpint(elapsed, <, 1400000);
	}
	srtp_dealloc(sink);
	srtp_dealloc(source);
	g_free(relay.info.cid);
	EVP_PKEY_free(relay_key);
	close(server_fd);
}

static void media_exchange(int media_fd, const struct sockaddr_in *client,
			   guint8 tx[30], guint8 rx[30], guint8 pt,
			   guint32 peer_ssrc, guint32 local_ssrc)
{
	srtp_t read_ctx = peer_context(rx, FALSE);
	srtp_t write_ctx = peer_context(tx, TRUE);
	int before = g_atomic_int_get(&playback_frames);

	expect_audio(media_fd, read_ctx, pt, local_ssrc);
	send_audio(media_fd, client, write_ctx, pt, peer_ssrc, 1, FALSE);
	expect_playback(before);
	if (migrate_media) {
		int moved = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		};

		g_assert_cmpint(moved, >=, 0);
		g_assert_cmpint(bind(moved, (void *)&addr, sizeof(addr)), ==,
				0);
		struct pollfd p = { .fd = moved, .events = POLLIN };

		send_audio(moved, client, write_ctx, pt, peer_ssrc, 2, TRUE);
		send_audio(moved, client, write_ctx, pt, peer_ssrc + 1, 2,
			   FALSE);
		g_assert_cmpint(poll(&p, 1, 150), ==, 0);
		before = g_atomic_int_get(&playback_frames);
		send_audio(moved, client, write_ctx, pt, peer_ssrc, 3, FALSE);
		expect_playback(before);
		expect_audio(moved, read_ctx, pt, local_ssrc);
		before = g_atomic_int_get(&playback_frames);
		send_audio(media_fd, client, write_ctx, pt, peer_ssrc, 4,
			   FALSE);
		expect_playback(before);
		expect_audio(moved, read_ctx, pt, local_ssrc);
		close(moved);
	}
	srtp_dealloc(read_ctx);
	srtp_dealloc(write_ctx);
}

struct video_peer {
	int fd;
	const struct sockaddr_in *client;
	guint expected_width;
	gboolean got_frame;
};

static gboolean send_peer_video(const guint8 *packet, gsize size, gpointer data)
{
	const struct video_peer *endpoint = data;

	return sendto(endpoint->fd, packet, size, 0,
		      (const void *)endpoint->client,
		      sizeof(*endpoint->client)) == (ssize_t)size;
}

static void peer_display(const struct line_camera_frame *frame, gpointer data)
{
	struct video_peer *endpoint = data;

	guint width = endpoint->expected_width ? endpoint->expected_width :
						 fixture_camera_width;

	g_assert_cmpuint(frame->width, ==, width);
	g_assert_cmpuint(frame->height, ==, width * 3 / 4);
	endpoint->got_frame = TRUE;
}

static void expect_subscription(struct fake_relay *relay, guint32 ssrc,
				gboolean start)
{
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

	while (readable_until(server_fd, end)) {
		Pb packet = receive_packet(relay), control = { 0 };

		if (!call_wire_open(&relay->wire, PB_SLICE(packet), &control))
			continue;
		Slice media = pb_data(PB_SLICE(control), 4);
		Slice request = pb_data(pb_data(media, 2), CALL_GROUP_STRM_REQ);
		Slice stream = pb_data(request, 1);

		if (!stream.p)
			continue;
		g_assert_cmpuint(pb_number(stream, 1, 0), ==, start);
		g_assert_cmpuint(pb_number(stream, 3, 0), ==, ssrc);
		g_assert_cmpuint(pb_number(stream, 7, 0), ==, 0);
		if (start) {
			Pb reply = { 0 }, state = { 0 }, plain = { 0 };
			Slice header = pb_data(PB_SLICE(control), 1);

			pb_uint(&state, 1, CALL_GROUP_STREAM_RESUME);
			pb_uint(&state, 3, ssrc);
			pb_uint(&reply, 1, 0);
			pb_msg(&reply, 4, &state);
			g_assert_true(call_envelope(&plain, &relay->info, 4,
						    CALL_GROUP_STRM_RSP, 0x328d,
						    &reply, header));
			g_assert_true(call_wire_pack(&relay->wire, &plain,
						     FALSE, FALSE, &packet));
			g_assert_cmpint(sendto(server_fd, packet.p, packet.n, 0,
					       (const void *)&relay->remote,
					       relay->remote_len),
					==, (ssize_t)packet.n);
		}
		return;
	}
	g_error("No subscription request for the fixture camera");
}

static void expect_camera_control(struct fake_relay *relay, guint action,
				  guint32 tx_ssrc, gboolean expect_report)
{
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	gboolean reported = FALSE;

	while (readable_until(server_fd, end)) {
		Pb packet = receive_packet(relay), control = { 0 };

		if (!call_wire_open(&relay->wire, PB_SLICE(packet), &control))
			continue;
		Slice media = pb_data(PB_SLICE(control), 4);
		Slice request = pb_data(pb_data(media, 2), CALL_GROUP_DATA_REQ);
		Slice frame = pb_data(request, 4);

		if (frame.n >= 8 && frame.p[1] == 2) {
			g_assert_true(expect_report);
			g_assert_false(reported);
			assert_tx_paused(frame, tx_ssrc, action != 1);
			reported = TRUE;
			continue;
		}
		if (frame.n < 28 || frame.p[1] != 1)
			continue;
		guint32 source;

		memcpy(&source, frame.p + 24, sizeof(source));
		g_assert_cmpuint(frame.p[9], ==, action);
		g_assert_cmpuint(frame.p[15], ==, 2);
		g_assert_cmpuint(ntohl(source), ==, tx_ssrc);
		g_assert_cmpint(reported, ==, expect_report);
		if (action == 1) {
			Slice header = pb_data(PB_SLICE(control), 1);

			memcpy(relay->camera_start.p, header.p, header.n);
			relay->camera_start.n = header.n;
		}
		return;
	}
	g_error("No stream control request for the local camera");
}

static gboolean group_camera_keyframe(struct fake_relay *relay,
				      const guint8 *key, guint8 pt,
				      guint32 tx_ssrc, gint64 duration)
{
	struct call_video *receiver =
		call_video_open_group_rx(key, pt, tx_ssrc, NULL);
	struct video_peer endpoint = { 0 };
	gint64 end = g_get_monotonic_time() + duration;

	g_assert_nonnull(receiver);
	while (!endpoint.got_frame && readable_until(server_fd, end)) {
		Pb packet = receive_packet(relay);

		call_video_receive(receiver, packet.p, packet.n, peer_display,
				   &endpoint);
	}
	g_assert_cmpuint(call_video_authenticated_packets(receiver), >, 0);
	call_video_close(receiver);
	return endpoint.got_frame;
}

static void camera_start_reply(struct fake_relay *relay, Slice header,
			       guint32 accepted, guint32 code)
{
	Pb body = { 0 }, plain = { 0 }, wire = { 0 };
	guint8 ack[24] = { [1] = 1, [5] = 16, [9] = 1, [11] = 1, [15] = 2 };

	accepted = htonl(accepted);
	code = htonl(code);
	memcpy(ack + 16, &code, sizeof(code));
	memcpy(ack + 20, &accepted, sizeof(accepted));
	pb_uint(&body, 1, 0);
	pb_uint(&body, 2, 0);
	pb_uint(&body, 4, 2);
	pb_bytes(&body, 5, (Slice){ ack, sizeof(ack) });
	g_assert_true(call_envelope(&plain, &relay->info, 4,
				    CALL_GROUP_DATA_RSP, 0x3289, &body,
				    header));
	g_assert_true(
		call_wire_pack(&relay->wire, &plain, FALSE, FALSE, &wire));
	g_assert_cmpint(sendto(server_fd, wire.p, wire.n, 0,
			       (void *)&relay->remote, relay->remote_len),
			==, (ssize_t)wire.n);
}

static void assert_camera_map(const Pb *packet,
			      enum call_video_rtp_extension format)
{
	g_assert_cmpuint(!!(packet->p[0] & 0x10), ==,
			 format != CALL_VIDEO_RTP_EXT_NONE);
	if (format != CALL_VIDEO_RTP_EXT_NONE) {
		g_assert_cmpuint(packet->n, >=, 16);
		g_assert_cmphex(packet->p[12], ==, 2);
		g_assert_cmphex(packet->p[13], ==, 0);
	}
	if (format == CALL_VIDEO_RTP_EXT_VFD) {
		g_assert_cmpuint(packet->n, >=, 24);
		g_assert_cmpuint(packet->p[15], ==, 2);
		g_assert_cmpuint(packet->p[16], ==, 2);
		g_assert_cmpuint(packet->p[17], ==, 3);
		g_assert_cmpuint(packet->p[20], ==, 3);
	} else if (format == CALL_VIDEO_RTP_EXT_VED) {
		const guint8 *p = packet->p;

		g_assert_cmpuint(packet->n, >=, 32);
		g_assert_cmpuint(p[16], ==, 4); /* VED element */
		g_assert_cmpuint(p[24] & 0x1f, ==, 4); /* VP8A layer */
		gboolean first_key = (p[26] & 2) && (p[23] & 2);

		g_assert_cmpuint(p[15], ==, first_key ? 5 : 4);
		g_assert_cmpuint(p[17], ==, first_key ? 8 : 6);
		if (first_key) {
			g_assert_cmpuint(packet->n, >=, 36);
			g_assert_cmphex(p[30], ==, 0xa1);
			g_assert_cmpuint(p[32], >, 0);
		}
	}
}

static void request_group_camera(struct fake_relay *relay, guint32 ssrc,
				 guint encoding,
				 enum call_video_rtp_extension format)
{
	Pb stream = { 0 }, request = { 0 }, plain = { 0 }, wire = { 0 };

	pb_uint(&stream, 1, 1); /* START */
	pb_uint(&stream, 3, ssrc);
	pb_uint(&stream, 6, encoding);
	if (format == CALL_VIDEO_RTP_EXT_VED) {
		/* Android requested a class-1 VP8A camera after the relay
		 * acknowledged the laptop's stream in the live group call. */
		Pb layer = { 0 };

		pb_uint(&layer, 1, 1);
		pb_uint(&layer, 2, 1);
		pb_msg(&stream, 8, &layer);
		pb_uint(&request, 2, encoding);
	}
	pb_msg(&request, 1, &stream);
	g_assert_true(call_envelope(&plain, &relay->info, 4,
				    CALL_GROUP_STRM_REQ, CALL_GROUP_STRM_REQ_ID,
				    &request, (Slice){ 0 }));
	g_assert_true(
		call_wire_pack(&relay->wire, &plain, FALSE, FALSE, &wire));
	g_assert_cmpint(sendto(server_fd, wire.p, wire.n, 0,
			       (const void *)&relay->remote, relay->remote_len),
			==, (ssize_t)wire.n);
}

static void expect_group_camera_restart(struct fake_relay *relay,
					const guint8 *key, guint8 pt,
					guint32 ssrc, guint encoding,
					enum call_video_rtp_extension format)
{
	struct video_peer decoded = { 0 };
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	struct call_video *receiver =
		call_video_open_group_rx(key, pt, ssrc, NULL);
	gboolean acknowledged = FALSE;

	g_assert_nonnull(receiver);
	if (format == CALL_VIDEO_RTP_EXT_VED)
		decoded.expected_width = 320;
	request_group_camera(relay, ssrc, encoding, format);
	while (!decoded.got_frame && readable_until(server_fd, end)) {
		Pb packet = receive_packet(relay), control = { 0 };

		if (!acknowledged) {
			if (call_wire_open(&relay->wire, PB_SLICE(packet),
					   &control)) {
				Slice media = pb_data(PB_SLICE(control), 4);
				Slice reply = pb_data(pb_data(media, 2),
						      CALL_GROUP_STRM_RSP);

				acknowledged = reply.n &&
					       !pb_number(reply, 1, 1);
			}
			continue;
		}
		if (call_video_receive(receiver, packet.p, packet.n,
				       peer_display, &decoded)) {
			assert_camera_map(&packet, format);
			if (format == CALL_VIDEO_RTP_EXT_VED) {
				/* The Android receive filter resolves class 1 from
				 * spatial and temporal IDs 1/1 on each fragment. */
				g_assert_cmphex(packet.p[22] & 0xfc, ==, 0x24);
				g_assert_cmphex(packet.p[26] & 0xfc, ==, 0x24);
				g_assert_cmpuint(packet.p[23] >> 5, ==, 1);
				g_assert_cmpuint(packet.p[25], ==, 15);
				if ((packet.p[26] & 2) && (packet.p[23] & 2))
					g_assert_cmphex(packet.p[31], ==, 0x24);
			}
		}
	}
	call_video_close(receiver);
	g_assert_true(acknowledged);
	g_assert_true(decoded.got_frame);
}

static void group_video_exchange(struct fake_relay *relay, const guint8 *local,
				 const guint8 *remote, guint8 pt,
				 guint32 tx_ssrc, gboolean trial,
				 guint32 accepted_ssrc,
				 enum call_video_rtp_extension format)
{
	enum {
		FIRST_CAMERA = 701,
		SECOND_CAMERA = 702
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) image = NULL;
	Pb presence = { 0 }, push = { 0 };
	guint8 tx[30], rx[30];

	pb_uint(&presence, 1, 1);
	pb_uint(&presence, 2, 1);
	fixture_member(&presence, self, "V", tx_ssrc);
	fixture_member(&presence, peer, "V", FIRST_CAMERA);
	fixture_member(&presence, other, "V", SECOND_CAMERA);
	pb_uint(&push, 1, 1);
	pb_msg(&push, 2, &presence);
	emit_peer(relay, CALL_GROUP_PUSH_REQ, 0x2150, &push, FALSE);
	expect_event("call_video_peer u22222222222222222222222222222222");
	expect_subscription(relay, FIRST_CAMERA, TRUE);
	call_group_media_key("VIDEO", local, rx);
	call_group_media_key("VIDEO", remote, tx);
	struct call_video *sender =
		call_video_open_group_tx(tx, pt, FIRST_CAMERA, 400, 10, &error);
	struct call_video *receiver =
		call_video_open_group_rx(rx, pt, tx_ssrc, &error);

	g_assert_no_error(error);
	g_assert_nonnull(sender);
	g_assert_nonnull(receiver);
	/* The mixer's forwarded stream may be layered while our uplink is baseline. */
	call_video_set_group_format(sender, CALL_VIDEO_RTP_EXT_VED, TRUE);
	int mixer = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	g_assert_cmpint(mixer, >=, 0);
	struct video_peer endpoint = { .fd = mixer, .client = &relay->remote };
	struct line_camera_frame frame;

	synthetic_frame(&frame, 160);
	g_atomic_int_set(&peer_video_ready, TRUE);
	g_assert_true(call_video_encode(sender, &frame, g_get_monotonic_time(),
					send_peer_video, &endpoint, &error));
	g_assert_no_error(error);
	expect_event("call_frame c33333333333333333333333333333333");
	struct line_preview_frame info;

	image = line_call_take_frame(group, &info);
	g_assert_nonnull(image);
	g_assert_cmpuint(info.width, ==, 160);
	g_assert_cmpuint(info.height, ==, 120);
	g_assert_true(info.camera_enabled);
	g_assert_false(info.screen);
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

	while (!endpoint.got_frame && readable_until(server_fd, end)) {
		Pb packet = receive_packet(relay);

		if (call_video_receive(receiver, packet.p, packet.n,
				       peer_display, &endpoint)) {
			assert_camera_map(&packet, format);
		}
	}
	g_assert_true(endpoint.got_frame);

	guint8 fir[28] = { 0x91, 206, 0, 6 };
	guint32 sender_id = htonl(FIRST_CAMERA), target_id = htonl(tx_ssrc);
	Slice feedback = { fir, sizeof(fir) };
	Pb request = { 0 }, plain = { 0 }, wire = { 0 };

	memcpy(fir + 4, &sender_id, sizeof(sender_id));
	memcpy(fir + 8, &target_id, sizeof(target_id));
	g_assert_cmpint(sendto(mixer, fir, sizeof(fir), 0,
			       (const void *)&relay->remote, relay->remote_len),
			==, sizeof(fir));
	/* An unrelated transaction must not restart our camera encoder. */
	camera_start_reply(relay, (Slice){ 0 }, tx_ssrc, 0);
	g_assert_false(group_camera_keyframe(relay, rx, pt, tx_ssrc, 350000));
	Slice start = PB_SLICE(relay->camera_start);
	gboolean accept_camera = accepted_ssrc == tx_ssrc;

	g_assert_nonnull(pb_data(start, 4).p);
	camera_start_reply(relay, start, accepted_ssrc,
			   trial || !accept_camera ? 203 : 0);
	gint64 timeout = accept_camera ? G_USEC_PER_SEC : 350000;

	g_assert_cmpint(group_camera_keyframe(relay, rx, pt, tx_ssrc, timeout),
			==, accept_camera);
	camera_start_reply(relay, start, tx_ssrc, 0);
	g_assert_false(group_camera_keyframe(relay, rx, pt, tx_ssrc, 350000));
	if (trial) {
		pb_uint(&request, 3, 1);
		pb_bytes(&request, 4, feedback);
		g_assert_true(call_envelope(
			&plain, &relay->info, 4, CALL_GROUP_DATA_REQ,
			CALL_GROUP_DATA_ID, &request, (Slice){ 0 }));
		g_assert_true(call_wire_pack(&relay->wire, &plain, FALSE, FALSE,
					     &wire));
		feedback = PB_SLICE(wire);
	}
	g_assert_cmpint(sendto(server_fd, feedback.p, feedback.n, 0,
			       (const void *)&relay->remote, relay->remote_len),
			==, (ssize_t)feedback.n);
	g_assert_true(
		group_camera_keyframe(relay, rx, pt, tx_ssrc, G_USEC_PER_SEC));
	expect_group_camera_restart(relay, rx, pt, tx_ssrc, 1, format);
	expect_group_camera_restart(relay, rx, pt, tx_ssrc, 2, format);

	g_autoptr(J) focused = line_call_video_focus(group, other, &error);

	g_assert_no_error(error);
	g_assert_nonnull(focused);
	expect_event("call_video_peer u44444444444444444444444444444444");
	expect_subscription(relay, FIRST_CAMERA, FALSE);
	expect_subscription(relay, SECOND_CAMERA, TRUE);
	call_video_close(sender);
	sender = call_video_open_group_tx(tx, pt, SECOND_CAMERA, 400, 10,
					  &error);
	g_assert_no_error(error);
	g_assert_nonnull(sender);
	g_autoptr(J) disabled = line_call_camera(FALSE, &error);

	g_assert_no_error(error);
	g_assert_nonnull(disabled);
	expect_camera_control(relay, 2, tx_ssrc, TRUE);
	synthetic_frame(&frame, 320);
	g_assert_true(call_video_encode(sender, &frame, g_get_monotonic_time(),
					send_peer_video, &endpoint, &error));
	g_assert_no_error(error);
	expect_event("call_frame c33333333333333333333333333333333");
	g_clear_pointer(&image, g_bytes_unref);
	image = line_call_take_frame(group, &info);
	g_assert_nonnull(image);
	g_assert_false(info.camera_enabled);
	g_assert_false(info.screen);
	g_assert_cmpuint(info.width, ==, 320);
	g_assert_cmpuint(info.height, ==, 240);

	/* The focused participant can publish a display on this same stream. */
	g_autoptr(GBytes) desktop = fixture_screen_pixels();

	line_camera_i420_frame(&frame, g_bytes_get_data(desktop, NULL),
			       SCREEN_WIDTH, SCREEN_HEIGHT);
	call_video_screen(sender, 600, 10);
	g_assert_true(call_video_encode(sender, &frame, g_get_monotonic_time(),
					send_peer_video, &endpoint, &error));
	g_assert_no_error(error);
	expect_event("call_frame c33333333333333333333333333333333");
	g_clear_pointer(&image, g_bytes_unref);
	image = line_call_take_frame(group, &info);
	g_assert_nonnull(image);
	g_assert_cmpuint(info.width, ==, SCREEN_WIDTH);
	g_assert_cmpuint(info.height, ==, SCREEN_HEIGHT);
	g_assert_cmpuint(g_bytes_get_size(image), ==,
			 g_bytes_get_size(desktop));
	const guint8 *pixels = g_bytes_get_data(image, NULL);

	g_assert_cmpint(ABS((int)pixels[0] - 168), <=, 8);
	g_assert_cmpint(ABS((int)pixels[info.width * (info.height - 1)] - 72),
			<=, 8);
	g_assert_false(info.camera_enabled);
	g_assert_true(info.screen);

	call_video_screen(sender, 0, 0);
	synthetic_frame(&frame, 320);
	g_assert_true(call_video_encode(sender, &frame, g_get_monotonic_time(),
					send_peer_video, &endpoint, &error));
	g_assert_no_error(error);
	expect_event("call_frame c33333333333333333333333333333333");
	g_clear_pointer(&image, g_bytes_unref);
	image = line_call_take_frame(group, &info);
	g_assert_nonnull(image);
	g_assert_false(info.screen);
	g_assert_cmpuint(info.width, ==, 320);
	g_assert_cmpuint(info.height, ==, 240);

	g_autoptr(J) enabled = line_call_camera(TRUE, &error);

	g_assert_no_error(error);
	g_assert_nonnull(enabled);
	expect_camera_control(relay, 1, tx_ssrc, TRUE);
	g_assert_false(readable_until(mixer, g_get_monotonic_time() + 30000));
	close(mixer);
	call_video_close(sender);
	call_video_close(receiver);
}

static GBytes *direct_video(struct line_preview_frame *info)
{
	struct pollfd input = { .fd = preview_fd, .events = POLLIN };
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	GBytes *remote = NULL;
	gboolean saw_self = FALSE;

	while (!(remote && saw_self) && g_get_monotonic_time() < end) {
		struct line_preview_frame msg;

		if (poll(&input, 1, 100) <= 0)
			continue;
		int fd = line_media_recv_frame(preview_fd, &msg);

		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpstr(msg.chat, ==, peer);
		g_assert_cmpint(msg.decoded_at, >, 0);
		if (msg.source == LINE_PREVIEW_REMOTE) {
			gsize size =
				line_camera_i420_size(msg.width, msg.height);
			guint8 *pixels = g_malloc(size);

			g_assert_cmpint(read(fd, pixels, size), ==, size);
			g_clear_pointer(&remote, g_bytes_unref);
			remote = g_bytes_new_take(pixels, size);
			*info = msg;
		} else {
			guint8 y;

			g_assert_cmpuint(msg.source, ==, LINE_PREVIEW_SELF);
			g_assert_cmpuint(msg.width, ==, 160);
			g_assert_cmpuint(msg.height, ==, 120);
			g_assert_cmpint(read(fd, &y, 1), ==, 1);
			g_assert_cmpuint(y, ==, 96);
			saw_self = TRUE;
		}
		close(fd);
	}
	g_assert_true(saw_self);
	return remote;
}

static void video_exchange(int fd, const struct fake_relay *r, EVP_PKEY *key,
			   Slice local_material, guint8 pt, gboolean incoming,
			   gboolean local_camera)
{
	enum {
		WIDTH = 719,
		HEIGHT = 1279,
		CHROMA = (WIDTH + 1) / 2,
		CHROMA_ROWS = (HEIGHT + 1) / 2
	};

	guint8 tx[30], rx[30];

	g_autoptr(GError) error = NULL;

	g_assert_true(call_video_keys(
		key, r->info.pub, pb_data(local_material, 1).p,
		r->info.media_nonce, pb_data(local_material, 3).p,
		r->info.media_id, pb_number(local_material, 2, 0), tx, rx));
	struct call_video *video =
		call_video_open(tx, rx, pt, incoming ? 212 : 112,
				incoming ? 112 : 212, 400, 10, &error);

	g_assert_no_error(error);
	g_assert_nonnull(video);
	/*
	 * Signaling and audio read from the same socket during setup. Keep the
	 * first synthetic camera frame until the peer has its video decoder.
	 */
	g_atomic_int_set(&peer_video_ready, TRUE);
	struct video_peer endpoint = { .fd = fd, .client = &r->remote };
	g_autofree guint8 *y = g_malloc(WIDTH * HEIGHT);
	g_autofree guint8 *u = g_malloc(CHROMA * CHROMA_ROWS);
	g_autofree guint8 *v = g_malloc(CHROMA * CHROMA_ROWS);
	struct line_camera_frame frame = {
		.width = WIDTH,
		.height = HEIGHT,
		.plane = { y, u, v },
		.stride = { WIDTH, CHROMA, CHROMA },
	};

	memset(y, 96, WIDTH * HEIGHT);
	memset(u, 80, CHROMA * CHROMA_ROWS);
	memset(v, 160, CHROMA * CHROMA_ROWS);
	g_assert_true(call_video_encode(video, &frame, g_get_monotonic_time(),
					send_peer_video, &endpoint, &error));
	g_assert_no_error(error);
	struct line_preview_frame info = { 0 };

	g_autoptr(GBytes) picture = NULL;

	if (preview_fd >= 0)
		picture = direct_video(&info);
	else {
		expect_event("call_frame u22222222222222222222222222222222");
		picture = line_call_take_frame(peer, &info);
	}

	g_assert_nonnull(picture);
	g_assert_cmpint(info.camera_enabled, ==, local_camera);
	g_assert_false(info.screen);
	g_assert_cmpuint(info.rotation, ==, 0);
	g_assert_cmpuint(info.width, ==, WIDTH);
	g_assert_cmpuint(info.height, ==, HEIGHT);
	g_assert_cmpuint(g_bytes_get_size(picture), ==,
			 WIDTH * HEIGHT + 2 * CHROMA * CHROMA_ROWS);
	struct line_camera_frame decoded;

	line_camera_i420_frame(&decoded, g_bytes_get_data(picture, NULL),
			       info.width, info.height);
	const guint expected[] = { 96, 80, 160 };

	for (unsigned int i = 0; i < G_N_ELEMENTS(expected); i++) {
		gsize size = i ? CHROMA * CHROMA_ROWS : WIDTH * HEIGHT;

		g_assert_cmpint(
			ABS((int)decoded.plane[i][0] - (int)expected[i]), <=,
			8);
		g_assert_cmpint(
			ABS((int)decoded.plane[i][size - 1] - (int)expected[i]),
			<=, 8);
	}
	g_assert_null(line_call_take_frame(peer, &info));
	if (!local_camera) {
		call_video_close(video);
		return;
	}
	gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	gint64 next_feedback = 0;

	while (!endpoint.got_frame) {
		guint8 message[2048];
		gint64 now = g_get_monotonic_time();

		if (now >= deadline)
			break;
		if (now >= next_feedback) {
			if (call_video_needs_keyframe(video))
				g_assert_true(call_video_send_feedback(
					video, send_peer_video, &endpoint));
			next_feedback = now + G_USEC_PER_SEC;
		}
		if (!readable_until(fd, MIN(next_feedback, deadline)))
			continue;
		ssize_t n = recv(fd, message, sizeof(message), 0);

		if (n >= 12 && (message[0] >> 6) == 2 &&
		    (message[1] & 127) == pt)
			call_video_receive(video, message, n, peer_display,
					   &endpoint);
	}
	if (!endpoint.got_frame) {
		g_test_message("Authenticated camera packets: %u",
			       call_video_authenticated_packets(video));
		call_video_report(video, FALSE);
	}
	g_assert_true(endpoint.got_frame);
	/* The same captured frames feed the window's local preview. */
	int width = 0, height = 0;
	g_autoptr(GBytes) self =
		line_call_take_self_frame(peer, &width, &height);

	if (preview_fd >= 0) {
		g_assert_cmpint(g_atomic_int_get(&self_frames), ==, 0);
		g_assert_null(self);
	} else {
		g_assert_cmpint(g_atomic_int_get(&self_frames), >, 0);
		g_assert_nonnull(self);
		g_assert_cmpuint(g_bytes_get_size(self), ==,
				 line_camera_i420_size(width, height));
	}
	call_video_close(video);
}

struct share_picture {
	guint width, height;
	guint8 top, bottom;
	guint received;
};

static void check_share_picture(const struct line_camera_frame *frame,
				gpointer data)
{
	struct share_picture *expected = data;

	if (frame->width != expected->width)
		return;
	g_assert_cmpuint(frame->height, ==, expected->height);
	const guint8 *last =
		frame->plane[0] + (frame->height - 1) * frame->stride[0];

	g_assert_cmpint(ABS((int)frame->plane[0][0] - expected->top), <=, 8);
	g_assert_cmpint(ABS((int)last[0] - expected->bottom), <=, 8);
	expected->received++;
}

static void receive_share_pictures(int fd, struct call_video *video,
				   struct share_picture *expected, guint count,
				   gint64 timeout)
{
	gint64 deadline = g_get_monotonic_time() + timeout;

	while (expected->received < count && readable_until(fd, deadline)) {
		guint8 packet[2048];
		ssize_t n = recv(fd, packet, sizeof(packet), 0);

		if (n > 0)
			call_video_receive(video, packet, n,
					   check_share_picture, expected);
	}
}

static void group_share_exchange(struct fake_relay *relay, const guint8 *local,
				 guint8 pt, guint32 rx_ssrc, guint32 ssrc)
{
	struct share_picture camera = {
		.width = 160, .height = 120, .top = 96, .bottom = 96
	};
	struct share_picture screen = { .width = SCREEN_WIDTH,
					.height = SCREEN_HEIGHT,
					.top = 168,
					.bottom = 72 };
	g_autoptr(GError) error = NULL;
	guint8 rx[30];

	call_group_media_key("VIDEO", local, rx);
	struct call_video *viewer =
		call_video_open_group_rx(rx, pt, ssrc, &error);

	g_assert_no_error(error);
	g_assert_nonnull(viewer);
	g_atomic_int_set(&peer_video_ready, TRUE);
	if (!camera_unavailable) {
		receive_share_pictures(server_fd, viewer, &camera, 1,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(camera.received, ==, 1);

		/* The capture fixture owns this fd until it handles cancellation. */
		int picker = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);

		g_assert_cmpint(picker, >=, 0);
		g_atomic_int_set(&picker_fd, picker);
		g_autoptr(J) denied = line_call_share(TRUE, &error);

		g_assert_no_error(error);
		g_assert_true(
			json_object_get_boolean(field(denied, "pending")));
		expect_event("test_picker open");
		receive_share_pictures(server_fd, viewer, &camera, 2,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(camera.received, ==, 2);
		g_assert_cmpint(eventfd_write(picker, 2), ==, 0);
		expect_event("call_share stopped");
		receive_share_pictures(server_fd, viewer, &camera, 3,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(camera.received, ==, 3);
		g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==, 1);
	}
	g_autoptr(J) start = line_call_share(TRUE, &error);

	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(start, "pending")));
	expect_event("call_share started");
	if (camera_unavailable)
		expect_camera_control(relay, 1, ssrc, TRUE);
	receive_share_pictures(server_fd, viewer, &screen, 2,
			       3 * G_USEC_PER_SEC);
	g_assert_cmpuint(screen.received, ==, 2);

	/* A receiver that missed the first picture needs a recovery frame
	 * even on a slowly updating screen and without a viewer request.
	 */
	if (camera_unavailable) {
		call_video_close(viewer);
		viewer = call_video_open_group_rx(rx, pt, ssrc, &error);
		g_assert_no_error(error);
		g_assert_nonnull(viewer);
		struct share_picture late = screen;

		late.received = 0;
		receive_share_pictures(server_fd, viewer, &late, 1,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(late.received, ==, 1);
	}
	/* An explicit request should also refresh a static desktop promptly. */
	call_video_close(viewer);
	viewer = call_video_open_group_rx(rx, pt, ssrc, &error);
	g_assert_no_error(error);
	g_assert_nonnull(viewer);
	if (camera_unavailable) {
		guint8 fir[28] = { 0x91, 206, 0, 6 };
		guint32 sender = htonl(rx_ssrc), target = htonl(ssrc);

		memcpy(fir + 4, &sender, sizeof(sender));
		memcpy(fir + 8, &target, sizeof(target));
		g_assert_cmpint(sendto(server_fd, fir, sizeof(fir), 0,
				       (const void *)&relay->remote,
				       relay->remote_len),
				==, sizeof(fir));
	} else {
		request_group_camera(relay, ssrc, 1, CALL_VIDEO_RTP_EXT_NONE);
	}
	receive_share_pictures(server_fd, viewer, &screen, 3, 800000);
	g_assert_cmpuint(screen.received, ==, 3);

	g_autoptr(J) stop = line_call_share(FALSE, &error);

	g_assert_no_error(error);
	g_assert_false(json_object_get_boolean(field(stop, "pending")));
	expect_event("call_share stopped");
	if (camera_unavailable) {
		expect_camera_control(relay, 2, ssrc, TRUE);
		receive_share_pictures(server_fd, viewer, &screen, 4, 1200000);
		g_assert_cmpuint(screen.received, ==, 3);
		g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==, 0);
	} else {
		receive_share_pictures(server_fd, viewer, &camera, 4,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(camera.received, ==, 4);
		g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==, 2);
	}
	call_video_close(viewer);
}

static void share_exchange(int fd, const struct fake_relay *r, EVP_PKEY *media,
			   Slice local_material)
{
	guint8 tx[30], rx[30];
	g_autoptr(GError) error = NULL;

	g_assert_true(call_video_keys(
		media, r->info.pub, pb_data(local_material, 1).p,
		r->info.media_nonce, pb_data(local_material, 3).p,
		r->info.media_id, pb_number(local_material, 2, 0), tx, rx));
	struct call_video *video =
		call_video_open(tx, rx, 109, 112, 212, 400, 10, &error);

	g_assert_no_error(error);
	g_assert_nonnull(video);
	g_autoptr(J) started = line_call_share(TRUE, &error);

	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(started, "pending")));
	expect_event("call_share started");
	struct share_picture screen = { .width = SCREEN_WIDTH,
					.height = SCREEN_HEIGHT,
					.top = 168,
					.bottom = 72 };

	/* The portal gave us just one frame. A late viewer still needs it. */
	receive_share_pictures(fd, video, &screen, 2, 3 * G_USEC_PER_SEC);
	g_assert_cmpuint(screen.received, ==, 2);
	g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==,
			start_with_camera_off ? 0 : 1);
	g_autoptr(J) stopped = line_call_share(FALSE, &error);

	g_assert_no_error(error);
	g_assert_false(json_object_get_boolean(field(stopped, "pending")));
	expect_event("call_share stopped");
	if (start_with_camera_off) {
		receive_share_pictures(fd, video, &screen, 3, 1200000);
		g_assert_cmpuint(screen.received, ==, 2);
		g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==, 0);
	} else {
		struct share_picture camera = {
			.width = 160, .height = 120, .top = 96, .bottom = 96
		};

		receive_share_pictures(fd, video, &camera, 1,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(camera.received, ==, 1);
		g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==, 2);
	}
	call_video_close(video);
}

/*
 * An independent tablet peer. Use the captured PLANET stream IDs, commands
 * and EVS3 descriptor layout (analysis/video-call-wire.md). The VP8 payload
 * is a synthetic 32x48, two-band I420 card encoded once with ffmpeg/libvpx.
 * Keep both the PDTP packet writer and the picture independent of our sender.
 */
struct presentation_peer {
	struct fake_relay *relay;
	int fd;
	guint8 pt;
	guint32 ssrc, contributor;
	guint32 channel, destination;
	guint16 seq;
	gboolean conference;
	srtp_t tx;
};

static void send_presentation_packet(struct presentation_peer *sender,
				     const char *service, guint8 mask,
				     const guint8 *elements, gsize size)
{
	guint8 packet[256] = { 0 };
	guint32 ssrc = htonl(sender->ssrc);
	guint16 seq = htons(++sender->seq);
	gsize pos = 12, service_size = strlen(service) + 1;

	packet[0] = sender->conference ? 0x90 : 0x80;
	packet[1] = 0x80 | sender->pt;
	memcpy(packet + 2, &seq, sizeof(seq));
	memcpy(packet + 8, &ssrc, sizeof(ssrc));
	if (sender->contributor) {
		guint32 source = htonl(sender->contributor);

		packet[0] |= 1;
		memcpy(packet + pos, &source, sizeof(source));
		pos += sizeof(source);
	}
	if (sender->conference) {
		guint words = !!sender->channel + !!sender->destination;

		packet[pos++] = 2;
		packet[pos++] = 0x40 | (sender->channel ? 0x20 : 0) | words;
		pos++;
		packet[pos++] = words;
		if (sender->channel) {
			guint32 channel = htonl(sender->channel);

			memcpy(packet + pos, &channel, sizeof(channel));
			pos += sizeof(channel);
		}
		if (sender->destination) {
			guint32 destination = htonl(sender->destination);

			memcpy(packet + pos, &destination, sizeof(destination));
			pos += sizeof(destination);
		}
	}
	g_assert_cmpuint(size, <, 64);
	g_assert_cmpuint(pos + service_size + size + 5 + SRTP_MAX_TRAILER_LEN,
			 <=, sizeof(packet));
	packet[pos++] = 0x80;
	pos++; /* high bits of packet number */
	memcpy(packet + pos, service, service_size);
	pos += service_size;
	pos++; /* high byte of element mask */
	packet[pos++] = mask;
	packet[pos++] = size;
	memcpy(packet + pos, elements, size);
	int n = pos + size;

	g_assert_cmpint(srtp_protect(sender->tx, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpint(sendto(sender->fd, packet, n, 0,
			       (const void *)&sender->relay->remote,
			       sender->relay->remote_len),
			==, n);
}

static guint64 fixture_varint(const guint8 **p, const guint8 *end)
{
	guint64 n;

	g_assert_true(call_pdtp_read_varint(p, end, &n));
	return n;
}

static void expect_targeted_resume(const guint8 **p, const guint8 *end,
				   guint32 contributor, guint64 source_offset)
{
	const guint8 *start = *p;

	g_assert_cmpuint(fixture_varint(p, end), ==, 4);
	g_assert_true(*p < end);
	g_assert_cmpuint(*(*p)++, ==, 2);
	g_assert_cmpuint(fixture_varint(p, end), ==, 0);
	g_assert_cmpuint(fixture_varint(p, end), ==, 9);
	g_assert_cmpuint(fixture_varint(p, end), ==, contributor);
	g_assert_cmpuint(fixture_varint(p, end), ==, source_offset);
	if (!g_strcmp0(g_getenv("AVERNELLA_TEST_MEDIA_CONTRACT"), "1")) {
		g_autoptr(GString) hex = g_string_new(NULL);

		while (start < *p)
			g_string_append_printf(hex, "%02x", *start++);
		g_print("# group-resume %s %u %" G_GUINT64_FORMAT "\n",
			hex->str, contributor, source_offset);
	}
}

/* A relay waits for both receive credit and a request naming the presenter. */
static void expect_presentation_request(const struct presentation_peer *sender,
					srtp_t feedback, guint64 source_offset)
{
	gboolean credit = FALSE;
	guint attempts = 0, first_packet = 0;
	/* Lose one request in the voice-only call before forwarding any data. */
	guint required =
		!video_fixture && sender->contributor == FIRST_PRESENTER ? 2 :
									   1;
	gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC * 2;

	while ((!credit || attempts < required) &&
	       readable_until(sender->fd, deadline)) {
		guint8 pkt[2048];
		int n = recv(sender->fd, pkt, sizeof(pkt), 0);

		if (n < 32 || (pkt[0] >> 6) != 2 ||
		    (pkt[1] & 127) != sender->pt)
			continue;
		g_assert_cmpint(srtp_unprotect(feedback, pkt, &n), ==,
				srtp_err_status_ok);
		gsize header = 12u + 4u * (pkt[0] & 15);

		g_assert_cmpuint(header, <=, (gsize)n);
		const guint8 *p = pkt + header, *end = pkt + n;

		g_assert_true(pkt[0] & 0x10);
		g_assert_cmpint(end - p, >=, 4);
		g_assert_cmpmem(p, 2, "\x02\x40", 2);
		gsize ext = 4u + 4u * ((p[2] << 8) | p[3]);

		g_assert_cmpuint(ext, <=, (gsize)(end - p));
		p += ext;
		/* Short test calls use a four-byte packet number (two in RTP). */
		if (end - p < 11 || p[0] != 0x80 || memcmp(p + 2, "PLANET", 7))
			continue;
		p += 9;
		guint mask = p[0] << 8 | p[1];
		guint packet = pkt[2] << 8 | pkt[3];

		p += 2;
		for (int bit = 14; bit >= 0; bit--) {
			if (!(mask & (1u << bit)))
				continue;
			guint64 size = fixture_varint(&p, end);

			g_assert_cmpuint(size, <=, (guint64)(end - p));
			const guint8 *next = p + size;

			if (bit == 8) {
				g_assert_cmpuint(fixture_varint(&p, next), ==,
						 first_packet);
				g_assert_true(p == next);
			} else if (bit == 12 || bit == 2) {
				guint64 count = fixture_varint(&p, next);

				for (guint64 i = 0; i < count; i++) {
					if (bit == 12) {
						guint64 stream = fixture_varint(
							&p, next);
						guint64 limit = fixture_varint(
							&p, next);

						credit |= stream == 4 &&
							  limit >= 115;
						continue;
					}
					expect_targeted_resume(
						&p, next, sender->contributor,
						source_offset);
					if (attempts++)
						g_assert_true(mask & (1u << 8));
					else
						first_packet = packet;
				}
				g_assert_true(p == next);
			}
			p = next;
		}
	}
	g_assert_true(credit);
	g_assert_cmpuint(attempts, >=, required);
}

static void announce_presentation(struct presentation_peer *sender,
				  guint32 source, guint64 source_offset)
{
	/* An ordinary PAUSE and a source-specific DATA_AVAIL share a packet. */
	guint8 element[32] = { 2, 4, 1, 0, 1, 4, 1, 0, 12 };
	gsize n = 9;

	n += call_pdtp_varint(element + n, source);
	n += call_pdtp_varint(element + n, source_offset);
	send_presentation_packet(sender, "PLANET", 4, element, n);
}

static void exchange_presentation(struct presentation_peer *sender,
				  srtp_t feedback, const char *chat,
				  guint32 source, guint64 source_offset)
{
	static const guint8 open[] = { 2, 3, 0x12, 0, 4, 0x12, 0 };
	/* count, stream, flags, start/end extensions, offset, length, data */
	static const guint8 start[] = { 1, 3, 0x0c, 0, 0, 0, 2, 1, 1 };
	static const guint8 stop[] = { 1, 3, 0x0c, 0, 0, 2, 3, 2, 1, 0x33 };
	static const guint8 first[] = {
		1,    4,    0x0c, 0,	0,    0,    36,	  0xba, 0,
		1,    0x10, 0x27, 5,	2,    0,    0x3c, 9,	0x84,
		0,    0,    0,	  44,	0xd0, 0x02, 0x00, 0x9d, 0x01,
		0x2a, 0x20, 0x00, 0x30, 0x00, 0x00, 0x47, 0x08, 0x85,
		0x85, 0x88, 0x85, 0x84, 0x88, 0x02, 0x02,
	};
	static const guint8 last[] = {
		1,    4,    0x0c, 0,	0,    36,   23,	  0x84, 0,    1,
		0x00, 0x08, 0x0f, 0x42, 0x43, 0x22, 0x22, 0xff, 0x51, 0xb8,
		0x00, 0xfe, 0xfe, 0xf9, 0x79, 0xfb, 0xff, 0xd8, 0x80, 0x00,
	};
	g_autofree char *frame_event =
		g_strdup_printf("call_share_frame %s", chat);
	struct line_preview_frame info;
	gboolean sharing;

	sender->contributor = 0;
	send_presentation_packet(sender, "PLANET", 8, open, sizeof(open));
	if (sender->conference) {
		/* DATA_AVAIL arrives on the relay SSRC before tagged media. */
		announce_presentation(sender, UNKNOWN_PRESENTER, 0);
		announce_presentation(sender, source, source_offset);
		if (source == FIRST_PRESENTER && source_offset) {
			/* The reliable D announcement and CC roster use separate
			 * channels. Deliver DATA_AVAIL first, exactly once.
			 */
			Pb presence = { 0 }, push = { 0 };

			pb_uint(&presence, 1, 1);
			pb_uint(&presence, 2, 1);
			fixture_member(&presence, peer, "D", FIRST_PRESENTER);
			fixture_member(&presence, other, "D", NEXT_PRESENTER);
			pb_uint(&push, 1, 1);
			pb_msg(&push, 2, &presence);
			emit_peer(sender->relay, CALL_GROUP_PUSH_REQ, 0x2150,
				  &push, FALSE);
		}
		sender->contributor = source;
		expect_presentation_request(sender, feedback, source_offset);
	}
	if (source == NEXT_PRESENTER) {
		/* One START command delivered in two ordered stream chunks. */
		const guint8 command[] = { 1, 3, 0x08, 0, 0, 1, 1 };
		const guint8 media[] = { 1, 3, 0x04, 0, 1, 1, 1 };

		send_presentation_packet(sender, "PLANET", 1, command,
					 sizeof(command));
		send_presentation_packet(sender, "PLANET", 1, media,
					 sizeof(media));
	} else {
		send_presentation_packet(sender, "PLANET", 1, start,
					 sizeof(start));
	}
	expect_event("call_share peer-started");

	if (sender->conference) {
		/* An idle member cannot stop the current presenter. */
		sender->contributor = source == FIRST_PRESENTER ?
					      NEXT_PRESENTER :
					      FIRST_PRESENTER;
		send_presentation_packet(sender, "PLANET", 1, stop,
					 sizeof(stop));
		sender->contributor = source;
		/* A valid STOP for another route leaves the main room untouched. */
		sender->channel = 444;
		send_presentation_packet(sender, "PLANET", 1, stop,
					 sizeof(stop));
		sender->channel = 0;
		sender->destination = UNKNOWN_PRESENTER;
		send_presentation_packet(sender, "PLANET", 1, stop,
					 sizeof(stop));
		sender->destination = 0;
		/* LINE control streams may use the same numeric ID as PLANET. */
		send_presentation_packet(sender, "LINE", 1, stop, sizeof(stop));
		/* The second fragment arrives early and is retransmitted below. */
		send_presentation_packet(sender, "PLANET", 1, last,
					 sizeof(last));
	}
	send_presentation_packet(sender, "PLANET", 1, first, sizeof(first));
	send_presentation_packet(sender, "PLANET", 1, last, sizeof(last));
	expect_event(frame_event);
	g_autoptr(GBytes) image =
		line_call_take_share_frame(chat, &info, &sharing);

	g_assert_true(sharing);
	g_assert_nonnull(image);
	g_assert_cmpuint(info.source, ==, LINE_PREVIEW_SCREEN);
	g_assert_cmpstr(info.chat, ==, chat);
	g_assert_cmpint(info.width, ==, 32);
	g_assert_cmpint(info.height, ==, 48);
	g_assert_cmpint(info.rotation, ==, 0);
	g_assert_cmpuint(g_bytes_get_size(image), ==, 32 * 48 * 3 / 2);
	const guint8 *y = g_bytes_get_data(image, NULL);

	g_assert_cmpint(ABS((int)y[info.width * 12] - 50), <=, 3);
	g_assert_cmpint(ABS((int)y[info.width * 36] - 194), <=, 3);

	/* Leave a second complete picture queued, as on a slow UI thread. */
	guint8 whole[63] = { 1, 4, 0x0c, 0, 0, 59, 56 };

	memcpy(whole + 7, first + 7, sizeof(first) - 7);
	memcpy(whole + sizeof(first), last + 10, sizeof(last) - 10);
	whole[7] = 0xbe; /* Single-fragment keyframe. */
	whole[9] = 2;
	send_presentation_packet(sender, "PLANET", 1, whole, sizeof(whole));
	expect_event(frame_event);
	if (source == NEXT_PRESENTER) {
		/* The second participant leaves without sending STOP. */
		Pb presence = { 0 }, push = { 0 };

		pb_uint(&presence, 1, 1);
		pb_uint(&presence, 2, 2);
		fixture_member(&presence, peer, "D", FIRST_PRESENTER);
		pb_uint(&push, 1, 1);
		pb_msg(&push, 2, &presence);
		emit_peer(sender->relay, CALL_GROUP_PUSH_REQ, 0x2150, &push,
			  FALSE);
	} else if (sender->conference && !video_fixture) {
		/* Both incoming streams close, then this member may open them again. */
		const guint8 close[] = { 2, 3, 1, 0, 4, 1, 0 };

		if (source_offset)
			send_presentation_packet(sender, "PLANET", 1, stop,
						 sizeof(stop));
		send_presentation_packet(sender, "PLANET", 2, close,
					 sizeof(close));
	} else {
		send_presentation_packet(sender, "PLANET", 1, stop,
					 sizeof(stop));
	}
	expect_event("call_share peer-stopped");
	g_assert_null(line_call_take_share_frame(chat, &info, &sharing));
	g_assert_false(sharing);
}

static void receive_presentation(int fd, struct fake_relay *r,
				 const guint8 key[30], guint8 pt, guint32 ssrc,
				 const char *chat, srtp_t feedback)
{
	struct presentation_peer sender = {
		.relay = r,
		.fd = fd,
		.pt = pt,
		.ssrc = ssrc,
		.conference = feedback != NULL,
	};

	g_assert_true(call_srtp_open(&sender.tx, key, ssrc_specific, ssrc));
	if (feedback) {
		/* A late subscriber, then a new presenter starting at byte zero. */
		exchange_presentation(&sender, feedback, chat, FIRST_PRESENTER,
				      1234);
		if (!video_fixture)
			exchange_presentation(&sender, feedback, chat,
					      FIRST_PRESENTER, 0);
		exchange_presentation(&sender, feedback, chat, NEXT_PRESENTER,
				      0);
	} else {
		exchange_presentation(&sender, NULL, chat, 0, 0);
	}
	srtp_dealloc(sender.tx);
}

static gboolean reported_running;

static gboolean check_video_start(const Pb *plain, gboolean incoming)
{
	Slice message = PB_SLICE(*plain), mc = pb_data(message, 4);
	Slice request = pb_data(pb_data(mc, 2), 16);

	if (!request.n)
		return FALSE;
	g_assert_cmpuint(pb_number(pb_data(message, 1), 2, 0), ==, 0x3189);
	g_assert_cmpuint(pb_number(request, 1, 1), ==, 0);
	g_assert_cmpuint(pb_number(request, 2, 1), ==, 0);
	g_assert_cmpuint(pb_number(request, 3, 0), ==, 2);
	Slice control = pb_data(request, 4);
	guint32 video_tx = incoming ? 112 : 212;

	if (control.n >= 8 && control.p[1] == 2) {
		/* A stream report precedes a mid-call camera start. */
		gsize length = (gsize)control.p[4] << 8 | control.p[5];

		g_assert_cmpuint(control.n, >=, 8 + length);
		Slice spec = { control.p + 8, length };
		PbField tx;

		reported_running = FALSE;
		while (pb_next(&spec, &tx))
			if (tx.field == 6 && tx.type == 2 &&
			    pb_number(tx.data, 1, 0) == video_tx)
				reported_running =
					!pb_number(pb_data(tx.data, 2), 1, 1);
		return FALSE;
	}
	if (start_with_camera_off)
		g_assert_true(reported_running);
	guint8 expected[] = {
		0, 1,
		0, 0,
		0, 56,
		0, 0,
		0, 1,
		0, 2,
		0, 0,
		0, 2,
		0, 0,
		0, 0,
		0, 0,
		0, incoming ? 212 : 112,
		0, 0,
		0, incoming ? 112 : 212,
		0, sizeof(self),
	};

	g_assert_cmpuint(control.n, ==, sizeof(expected) + sizeof(self));
	g_assert_cmpmem(control.p, sizeof(expected), expected,
			sizeof(expected));
	g_assert_cmpmem(control.p + sizeof(expected), sizeof(self), self,
			sizeof(self));
	return TRUE;
}

static const guint8 peer_video_start[] = {
	0, 1, 0, 0, 0, 20, 0, 0, /* Header */
	0, 1, 0, 2, 0, 0,  0, 2, /* START two video streams */
	0, 0, 0, 0, 0, 0,  0, 112, 0, 0, 0, 212,
};

static void check_media_video_start(struct fake_relay *r, int fd)
{
	Pb req = { 0 }, plain = { 0 }, pkt = { 0 };
	gboolean got_start = FALSE;
	struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (!got_start && g_get_monotonic_time() < deadline &&
	       poll(&poll_fd, 1, 2000) == 1) {
		ssize_t n = recv(fd, pkt.p, sizeof(pkt.p), 0);

		if (n <= 0 ||
		    !call_wire_open(&r->wire, (Slice){ pkt.p, n }, &plain) ||
		    !check_video_start(&plain, FALSE))
			continue;
		Slice channel = pb_data(pb_data(PB_SLICE(plain), 4), 1);

		g_assert_cmpuint(pb_number(channel, 2, 0), ==,
				 r->info.remote_media);
		g_assert_cmpuint(pb_number(channel, 3, 0), ==,
				 r->info.local_media);
		got_start = TRUE;
	}
	g_assert_true(got_start);

	pb_uint(&req, 3, 2);
	pb_bytes(&req, 4,
		 (Slice){ peer_video_start, sizeof(peer_video_start) });
	plain = (Pb){ 0 };
	g_assert_true(call_envelope(&plain, &r->info, 4, 16, 0x3189, &req,
				    (Slice){ 0 }));
	Slice sent_header = pb_data(PB_SLICE(plain), 1);

	g_assert_true(call_wire_pack(&r->wire, &plain, FALSE, FALSE, &pkt));
	g_assert_cmpint(sendto(fd, pkt.p, pkt.n, 0, (void *)&r->remote,
			       r->remote_len),
			==, (int)pkt.n);

	gboolean got_ack = FALSE;

	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (!got_ack && g_get_monotonic_time() < deadline &&
	       poll(&poll_fd, 1, 1000) == 1) {
		Pb decoded = { 0 };
		ssize_t n = recv(fd, pkt.p, sizeof(pkt.p), 0);

		if (n <= 0 ||
		    !call_wire_open(&r->wire, (Slice){ pkt.p, n }, &decoded))
			continue;
		Slice mc = pb_data(PB_SLICE(decoded), 4);
		Slice ack = pb_data(pb_data(mc, 2), 17);

		if (!ack.n)
			continue;
		Slice hdr = pb_data(PB_SLICE(decoded), 1);
		Slice txn = pb_data(hdr, 4), expected = pb_data(sent_header, 4);
		Slice reply = pb_data(ack, 5);

		g_assert_cmpuint(pb_number(hdr, 2, 0), ==, 0x3289);
		g_assert_cmpmem(txn.p, txn.n, expected.p, expected.n);
		g_assert_cmpuint(pb_number(ack, 4, 0), ==, 2);
		g_assert_cmpmem(reply.p, reply.n, peer_video_start,
				sizeof(peer_video_start));
		got_ack = TRUE;
	}
	g_assert_true(got_ack);
}

static void outgoing(void)
{
	setup_server();
	struct fake_relay r = { 0 };
	Pb setup = { 0 };

	start_outgoing(&r, &setup);
	Slice req = pb_data(pb_data(pb_data(PB_SLICE(setup), 3), 2), 1);
	Slice offer = pb_data(req, 9);

	Slice client_material = pb_data(pb_data(offer, 2), 3);
	guint8 video_pt;
	guint32 video_tx, video_rx;

	g_assert_cmpuint(pb_data(client_material, 1).n, ==, 33);
	g_assert_cmpuint(pb_data(client_material, 3).n, ==, 16);
	/* Every offer lists VP8. Only a camera-on video call marks it active. */
	g_assert_true(call_video_offer_profile(offer, &video_pt, &video_tx,
					       &video_rx));
	Slice records = offer;
	PbField record;
	gint64 video_state = -1;

	while (pb_next(&records, &record)) {
		Slice head = pb_data(record.data, 1), name = pb_data(head, 1);

		if (record.field == 1 && name.n == 1 && *name.p == 'V')
			video_state = pb_number(head, 3, 0);
	}
	g_assert_cmpint(video_state, ==,
			video_fixture && !start_with_camera_off);
	Pb ack = { 0 };

	pb_uint(&ack, 1, 0);
	pb_uint(&ack, 101, 5);
	emit_peer(&r, 2, 0x2241, &ack, TRUE);
	expect_event("call ringing");
	EVP_PKEY *media = NULL;

	r.info.media_id = 33;
	memset(r.info.media_nonce, 0xab, 16);
	g_assert_true(call_keygen(&media, r.info.pub));
	Pb response = { 0 }, remote_offer = { 0 };

	r.info.video = video_fixture && !voice_fallback &&
		       !start_with_camera_off;
	r.info.video_capable = start_with_camera_off;
	r.info.audio_payload = 110;
	/* A payload type outside the dynamic range makes video unusable. */
	r.info.video_payload = voice_fallback ? 90 : 109;
	call_answer_offer(&remote_offer, &r.info);
	pb_msg(&response, 1, &remote_offer);
	pb_uint(&response, 2, 777);
	int media_fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET,
				    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	g_assert_cmpint(bind(media_fd, (void *)&addr, sizeof(addr)), ==, 0);
	socklen_t addr_size = sizeof(addr);

	g_assert_cmpint(getsockname(media_fd, (void *)&addr, &addr_size), ==,
			0);
	Pb endpoint = { 0 };

	pb_text(&endpoint, 3, "127.0.0.1");
	pb_uint(&endpoint, 5, ntohs(addr.sin_port));
	pb_msg(&response, 101, &endpoint);
	emit_peer(&r, 5, 0x2144, &response, FALSE);
	expect_event("call connected");
	g_assert_true(audio_active);
	if (start_with_camera_off && !share_fixture) {
		video_exchange(media_fd, &r, media, client_material, 109, FALSE,
			       FALSE);
		g_autoptr(GError) error = NULL;
		g_autoptr(J) camera = line_call_camera(TRUE, &error);

		g_assert_no_error(error);
		g_assert_true(json_object_get_boolean(
			field(camera, "cameraEnabled")));
		r.info.video = TRUE;
	}
	gboolean saw_conn = FALSE, saw_info = FALSE, saw_video_start = FALSE;

	for (guint i = 0; i < 23 && (!saw_conn || !saw_info ||
				     (r.info.video && !saw_video_start));
	     i++) {
		Pb packet = receive_packet(&r), plain = { 0 };

		g_assert_true(
			call_wire_open(&r.wire, PB_SLICE(packet), &plain));
		Slice cc = pb_data(PB_SLICE(plain), 3), body = pb_data(cc, 2);
		Slice conn = pb_data(body, 6);

		if (conn.n) {
			r.info.remote_media = pb_number(conn, 4, 0);
			g_assert_cmpuint(r.info.remote_media, !=, 0);
			g_assert_cmpint(
				r.info.remote_media != r.info.remote_channel,
				==, r.info.video || r.info.video_capable);
			saw_conn = TRUE;
		}
		saw_info |= pb_data(body, 17).p != NULL;
		saw_video_start |= check_video_start(&plain, FALSE);
	}
	g_assert_true(saw_conn && saw_info);
	g_assert_cmpint(saw_video_start, ==, r.info.video);
	if (r.info.video) {
		r.info.local_media = 777;
		check_media_video_start(&r, media_fd);
	}
	guint8 expected_rx[30], expected_tx[30];

	g_assert_true(call_audio_keys(
		media, r.info.pub, pb_data(client_material, 1).p,
		r.info.media_nonce, pb_data(client_material, 3).p,
		r.info.media_id, pb_number(client_material, 2, 0), expected_tx,
		expected_rx));
	media_exchange(media_fd, &r.remote, expected_tx, expected_rx, 110, 102,
		       202);
	if (r.info.video && !start_with_camera_off)
		video_exchange(media_fd, &r, media, client_material, 109, FALSE,
			       TRUE);
	if (peer_share_fixture) {
		guint8 data_tx[30], data_rx[30];

		g_assert_true(call_data_keys(
			media, r.info.pub, pb_data(client_material, 1).p,
			r.info.media_nonce, pb_data(client_material, 3).p,
			r.info.media_id, pb_number(client_material, 2, 0),
			data_tx, data_rx));
		receive_presentation(media_fd, &r, data_tx, 98, 122, peer,
				     NULL);
	} else if (share_fixture)
		share_exchange(media_fd, &r, media, client_material);
	if (voice_fallback) {
		g_autoptr(GError) e = NULL;

		g_assert_null(line_call_camera(TRUE, &e));
		g_assert_error(e, LINE_ERROR, 1);
	}
	close(media_fd);
	g_autoptr(J) muted = line_voice_mute(TRUE, NULL);
	g_assert_true(json_object_get_boolean(field(muted, "muted")));
	Pb release = { 0 };

	pb_uint(&release, 1, 1);
	emit_peer(&r, 7, 0x2145, &release, FALSE);
	expect_event("call ended");
	line_voice_close();
	g_assert_false(audio_active);
	gboolean saw_release = FALSE;

	for (guint i = 0; i < 23 && !saw_release; i++) {
		Pb packet = receive_packet(&r), plain = { 0 };

		g_assert_true(
			call_wire_open(&r.wire, PB_SLICE(packet), &plain));
		Slice cc = pb_data(PB_SLICE(plain), 3), body = pb_data(cc, 2);

		saw_release |= pb_data(body, 8).p != NULL;
	}
	g_assert_true(saw_release);
	g_free(r.info.cid);
	EVP_PKEY_free(media);
	EVP_PKEY_free(relay_key);
	close(server_fd);
}

static void video_outgoing(void)
{
	if (g_test_subprocess()) {
		int pair[2];

		g_assert_cmpint(socketpair(AF_UNIX,
					   SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
					   pair),
				==, 0);
		line_call_frame_socket(pair[0]);
		preview_fd = pair[1];
		g_unsetenv("LINE_EXPERIMENTAL_VIDEO");
		video_fixture = TRUE;
		outgoing();
		line_call_frame_socket(-1);
		preview_fd = -1;
		close(pair[0]);
		close(pair[1]);
		return;
	}
	g_test_trap_subprocess(NULL, 8000000, 0);
	g_test_trap_assert_passed();
}

static void media_migration(void)
{
	migrate_media = TRUE;
	outgoing();
	migrate_media = FALSE;
}

static void voice_then_video(void)
{
	if (g_test_subprocess()) {
		g_unsetenv("LINE_EXPERIMENTAL_VIDEO");
		video_fixture = start_with_camera_off = TRUE;
		outgoing();
		return;
	}
	g_test_trap_subprocess(NULL, 8000000, 0);
	g_test_trap_assert_passed();
}

static void video_voice_fallback(void)
{
	if (g_test_subprocess()) {
		g_unsetenv("LINE_EXPERIMENTAL_VIDEO");
		video_fixture = voice_fallback = TRUE;
		outgoing();
		return;
	}
	g_test_trap_subprocess(NULL, 8000000, 0);
	g_test_trap_assert_passed();
}

static void share_on_tablet(void)
{
	if (g_test_subprocess()) {
		g_unsetenv("LINE_EXPERIMENTAL_VIDEO");
		g_unsetenv("LINE_SHARE_TRANSPORT");
		video_fixture = share_fixture = TRUE;
		peer_share_fixture =
			!strcmp(g_test_get_path(), "/call/share-from-tablet");
		start_with_camera_off =
			peer_share_fixture ||
			g_str_has_suffix(g_test_get_path(), "voice");
		outgoing();
		return;
	}
	g_test_trap_subprocess(NULL, 10000000, 0);
	g_test_trap_assert_passed();
}

static void rejected(void)
{
	setup_server();
	struct fake_relay r = { 0 };
	Pb setup = { 0 };

	start_outgoing(&r, &setup);
	Pb refusal = { 0 };

	pb_uint(&refusal, 1, 1);
	emit_peer(&r, 2, 0x2241, &refusal, TRUE);
	expect_event("call_error LINE declined the call setup.");
	expect_event("call ended");
	line_voice_close();
	g_assert_false(audio_active);
	g_free(r.info.cid);
	EVP_PKEY_free(relay_key);
	close(server_fd);
}

static void route_checks(void)
{
	Line s = { .mid = (char *)self };
	int before = rpc_count;

	g_autoptr(GError) e = NULL;

	g_assert_false(line_video_enabled());
	g_assert_null(line_call_start(&s, peer, TRUE, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_clear_error(&e);
	g_assert_cmpint(rpc_count, ==, before);

	g_assert_null(line_voice_start(&s, self, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	g_assert_cmpint(rpc_count, ==, before);
	bad_route = TRUE;
	g_assert_null(line_voice_start(&s, peer, &e));
	bad_route = FALSE;
	g_assert_nonnull(e);
}

static J *incoming_op(gint64 revision, gint64 age_ms, const char *kind)
{
	g_autoptr(J) config = obj(), payload = obj();
	g_autofree char *pub = g_base64_encode(relay_public, 33);

	json_object_object_add(config, "mpkey", json_object_new_string(pub));
	const char *fields[][2] = { { "k", kind },
				    { "n", "fixture-token" },
				    { "vs", "fixture-call" },
				    { "vfz", "JP" },
				    { "vtz", "JP" },
				    { "h", "127.0.0.1" },
				    { "vc",
				      json_object_to_json_string(config) } };
	for (guint i = 0; i < G_N_ELEMENTS(fields); i++)
		json_object_object_add(payload, fields[i][0],
				       json_object_new_string(fields[i][1]));
	json_object_object_add(payload, "p", json_object_new_int(server_port));
	J *op = obj();

	json_object_object_add(op, "1", json_object_new_int64(revision));
	json_object_object_add(
		op, "2",
		json_object_new_int64(g_get_real_time() / 1000 - age_ms));
	json_object_object_add(op, "3", json_object_new_int(50));
	json_object_object_add(op, "10", json_object_new_string(peer));
	json_object_object_add(op, "11",
			       json_object_new_string("fixture-token"));
	json_object_object_add(
		op, "12",
		json_object_new_string(json_object_to_json_string(payload)));
	return op;
}

static void echo_response(struct fake_relay *r, Slice hdr, guint tag, guint id,
			  Pb *body, gboolean first)
{
	Pb plain = { 0 }, pkt = { 0 };

	g_assert_true(call_envelope(&plain, &r->info, 3, tag, id, body, hdr));
	g_assert_true(call_wire_pack(&r->wire, &plain, first, FALSE, &pkt));
	g_assert_cmpint(sendto(server_fd, pkt.p, pkt.n, 0, (void *)&r->remote,
			       r->remote_len),
			==, (int)pkt.n);
}

static void incoming(gconstpointer mode)
{
	static gint64 revision = 1000;
	gboolean decline = GPOINTER_TO_INT(mode);

	setup_server();
	int before = rpc_count;
	Line session = { .mid = (char *)self };

	g_autoptr(GError) e = NULL;
	g_autoptr(J) stale = incoming_op(++revision, 50000, "CA");
	line_voice_operation(&session, stale);
	g_assert_cmpint(line_voice_tick(), ==, 0);
	if (!video_fixture) {
		g_autoptr(J) video = incoming_op(++revision, 0, "CV");

		line_voice_operation(&session, video);
	}
	g_assert_cmpint(g_async_queue_length(events), ==, 0);
	g_autoptr(J) op =
		incoming_op(++revision, 0, video_fixture ? "CV" : "CA");
	line_voice_operation(&session, op);
	line_voice_operation(&session, op);
	g_assert_cmpint(line_voice_tick(), >, g_get_monotonic_time());
	expect_event("call_incoming u22222222222222222222222222222222");
	g_assert_cmpint(g_async_queue_length(events), ==, 0);
	struct pollfd fd = { .fd = server_fd, .events = POLLIN };

	g_assert_cmpint(
		poll(&fd, 1, 5), ==,
		0); /* no verification or microphone before user action */
	g_assert_null(line_voice_answer(self, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	g_autoptr(J) result = decline ? line_voice_decline(peer, &e) :
					line_voice_answer(peer, &e);
	g_assert_no_error(e);
	g_assert_cmpint(line_voice_tick(), ==, 0);
	g_assert_cmpstr(strfield(result, "state"), ==, "connecting");
	g_assert_cmpint(json_object_get_boolean(field(result, "video")), ==,
			video_fixture);
	struct fake_relay r = { 0 };
	Pb verify = receive_packet(&r), decoded = { 0 };

	init_server(&r.wire, verify.p + 24);
	g_assert_true(call_wire_open(&r.wire, PB_SLICE(verify), &decoded));
	Slice hdr = pb_data(PB_SLICE(decoded), 1),
	      cc = pb_data(PB_SLICE(decoded), 3), ch = pb_data(cc, 1);
	g_assert_cmpuint(pb_number(hdr, 2, 0), ==, 0x2142);
	Slice vbody = pb_data(pb_data(cc, 2), 3);

	g_assert_cmpuint(pb_data(vbody, 9).n, ==, 32);
	Slice app_name = pb_data(vbody, 10);
	const char *expected_app = video_fixture ? "freecall.video" :
						   "freecall.audio";

	g_assert_cmpmem(app_name.p, app_name.n, expected_app,
			strlen(expected_app));
	r.info.cid = (char *)"fixture-call";
	r.info.local = (char *)peer;
	r.info.local_channel = 77;
	r.info.remote_channel = pb_number(ch, 2, 0);
	r.info.nonce = 99;
	r.info.remote_nonce = pb_number(hdr, 6, 0);
	memset(r.info.session, 0xa5, 16);
	r.info.have_session = TRUE;
	Pb verified = { 0 }, offer = { 0 };
	EVP_PKEY *media = NULL;

	g_assert_true(call_keygen(&media, r.info.pub));
	r.info.media_id = 0x33445566;
	r.info.video = video_fixture;
	r.info.audio_payload = 110;
	r.info.video_payload = 109;
	call_offer(&offer, &r.info);
	pb_msg(&verified, 6, &offer);
	pb_uint(&verified, 1, 0);
	emit_peer(&r, 4, 0x2242, &verified,
		  TRUE); /* mismatched transaction ignored */
	g_assert_cmpint(poll(&fd, 1, 30), ==, 0);
	g_assert_false(audio_active);
	echo_response(&r, hdr, 4, 0x2242, &verified, TRUE);
	Pb packet = receive_packet(&r), answer = { 0 };

	g_assert_true(call_wire_open(&r.wire, PB_SLICE(packet), &answer));
	hdr = pb_data(PB_SLICE(answer), 1);
	cc = pb_data(PB_SLICE(answer), 3);
	if (decline) {
		g_assert_cmpuint(pb_number(hdr, 2, 0), ==, 0x2145);
		g_assert_false(audio_active);
	} else {
		g_assert_cmpuint(pb_number(hdr, 2, 0), ==, 0x2144);
		g_assert_true(pb_data(pb_data(cc, 2), 5).p != NULL);
		g_assert_false(audio_active);
		Pb conn = { 0 };

		pb_uint(&conn, 1, 0);
		pb_uint(&conn, 4, 356);
		echo_response(&r, hdr, 6, 0x2244, &conn, FALSE);
		expect_event("call connected");
		g_assert_true(audio_active);
		if (video_fixture) {
			gboolean started = FALSE;

			for (guint i = 0; i < 12 && !started; i++) {
				Pb message = receive_packet(&r), plain = { 0 };

				if (call_wire_open(&r.wire, PB_SLICE(message),
						   &plain))
					started =
						check_video_start(&plain, TRUE);
			}
			g_assert_true(started);
		}
		Slice local_offer = pb_data(pb_data(pb_data(cc, 2), 5), 1);
		Slice keys = pb_data(pb_data(local_offer, 2), 3);
		guint8 tx[30], rx[30];

		g_assert_true(call_audio_keys(
			media, r.info.pub, pb_data(keys, 1).p,
			r.info.media_nonce, pb_data(keys, 3).p, r.info.media_id,
			pb_number(keys, 2, 0), tx, rx));
		media_exchange(server_fd, &r.remote, tx, rx, 110, 202, 102);
		if (video_fixture)
			video_exchange(server_fd, &r, media, keys, 109, TRUE,
				       TRUE);
		Pb rel = { 0 };

		pb_uint(&rel, 1, 2);
		emit_peer(&r, 7, 0x2145, &rel, FALSE);
	}
	expect_event("call ended");
	line_voice_close();
	g_assert_false(audio_active);
	g_assert_cmpint(
		rpc_count, ==,
		before); /* no outgoing call/route on answer or decline */
	close(server_fd);
	EVP_PKEY_free(media);
	EVP_PKEY_free(relay_key);
}

static void video_incoming(void)
{
	if (g_test_subprocess()) {
		g_unsetenv("LINE_EXPERIMENTAL_VIDEO");
		video_fixture = TRUE;
		incoming(GINT_TO_POINTER(FALSE));
		return;
	}
	g_test_trap_subprocess(NULL, 8000000, 0);
	g_test_trap_assert_passed();
}

/* ---- PDTP: the data channel that carries screen shares ---- */

static const guint8 pdtp_key_a[30] = { 9, 8, 7 }, pdtp_key_b[30] = { 6, 5, 4 };

struct pdtp_chunk {
	guint64 stream, offset, time;
	guint flags;
	char service[16];
	GBytes *bytes;
};

struct pdtp_peer {
	struct call_pdtp *pdtp;
	GPtrArray *wire; /* SRTP packets it sent */
	GArray *chunks; /* struct pdtp_chunk it received */
	guint opens;
};

static gboolean pdtp_collect(const guint8 *packet, gsize size, gpointer data)
{
	struct pdtp_peer *peer = data;

	g_assert_cmpuint(size, <=, 12 + 900 + 64 + 16);
	g_ptr_array_add(peer->wire, g_bytes_new(packet, size));
	return TRUE;
}

static void pdtp_data(guint64 stream, const char *service, guint64 offset,
		      guint64 time, guint flags, const guint8 *bytes,
		      gsize size, gpointer data)
{
	struct pdtp_peer *peer = data;
	struct pdtp_chunk chunk = { stream, offset, time,
				    flags,  { 0 },  g_bytes_new(bytes, size) };

	g_strlcpy(chunk.service, service, sizeof(chunk.service));
	g_array_append_val(peer->chunks, chunk);
}

static void pdtp_event(guint64 stream, const char *service, int element,
		       gpointer data)
{
	struct pdtp_peer *peer = data;

	(void)stream;
	(void)service;
	peer->opens += element == PDTP_STREAM_OPEN;
}

static void pdtp_peer_init(struct pdtp_peer *peer, const guint8 *tx,
			   const guint8 *rx, guint32 tx_ssrc, guint32 rx_ssrc)
{
	g_autoptr(GError) e = NULL;

	peer->pdtp = call_pdtp_open(tx, rx, 98, tx_ssrc, rx_ssrc, &e);
	g_assert_no_error(e);
	peer->wire =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	peer->chunks = g_array_new(FALSE, TRUE, sizeof(struct pdtp_chunk));
	peer->opens = 0;
	call_pdtp_set_handlers(peer->pdtp, pdtp_data, pdtp_event, peer);
}

static void pdtp_peer_clear(struct pdtp_peer *peer)
{
	for (guint i = 0; i < peer->chunks->len; i++)
		g_bytes_unref(
			g_array_index(peer->chunks, struct pdtp_chunk, i).bytes);
	g_array_free(peer->chunks, TRUE);
	g_ptr_array_unref(peer->wire);
	call_pdtp_close(peer->pdtp);
}

/* Delivers everything 'from' sent to 'to', then clears the wire. */
static guint pdtp_deliver(struct pdtp_peer *from, struct pdtp_peer *to,
			  gint64 now)
{
	guint n = from->wire->len;

	for (guint i = 0; i < n; i++) {
		gsize size;
		const guint8 *p = g_bytes_get_data(from->wire->pdata[i], &size);

		g_assert_true(call_pdtp_receive(to->pdtp, p, size, now));
	}
	g_ptr_array_set_size(from->wire, 0);
	return n;
}

static void pdtp_loopback(void)
{
	struct pdtp_peer a, b;
	g_autofree guint8 *frame = g_malloc(3000);
	static const guint8 command[] = { 1, 1 };
	gint64 now = 1000000;

	for (guint i = 0; i < 3000; i++)
		frame[i] = i * 7;
	pdtp_peer_init(&a, pdtp_key_a, pdtp_key_b, 122, 222);
	pdtp_peer_init(&b, pdtp_key_b, pdtp_key_a, 222, 122);
	g_assert_true(call_pdtp_stream_open(a.pdtp, 3, "PLANET", 0x12));
	g_assert_true(call_pdtp_stream_open(a.pdtp, 4, "PLANET", 0x12));
	g_assert_true(call_pdtp_stream_send(a.pdtp, 3, command, 2, 0));
	g_assert_true(call_pdtp_stream_send(a.pdtp, 4, frame, 3000, 77));
	g_assert_true(call_pdtp_stream_send(a.pdtp, 4, frame, 500, 78));
	for (guint round = 0; round < 6; round++) {
		call_pdtp_poll(a.pdtp, now, pdtp_collect, &a);
		pdtp_deliver(&a, &b, now);
		now += 30000;
		call_pdtp_poll(b.pdtp, now, pdtp_collect, &b);
		pdtp_deliver(&b, &a, now);
	}
	g_assert_cmpuint(b.opens, ==, 2);
	/* The command arrives as one message without a source time. */
	g_autoptr(GByteArray) video = g_byte_array_new();
	guint starts = 0, ends = 0, control = 0;

	for (guint i = 0; i < b.chunks->len; i++) {
		struct pdtp_chunk *c =
			&g_array_index(b.chunks, struct pdtp_chunk, i);
		gsize size;
		const guint8 *p = g_bytes_get_data(c->bytes, &size);

		g_assert_cmpstr(c->service, ==, "PLANET");
		if (c->stream == 3) {
			g_assert_cmpmem(p, size, command, 2);
			g_assert_cmphex(c->flags & 0x0d, ==, 0x0c);
			control++;
			continue;
		}
		g_assert_cmpuint(c->stream, ==, 4);
		g_assert_cmphex(c->flags & 0x13, ==, 0x13);
		/* No chunk mixes the two messages. */
		g_assert_cmpuint(c->time, ==, c->offset < 3000 ? 77 : 78);
		starts += !!(c->flags & PDTP_MESSAGE_START);
		ends += !!(c->flags & PDTP_MESSAGE_END);
		g_assert_cmpuint(c->offset, ==, video->len);
		g_byte_array_append(video, p, size);
	}
	g_assert_cmpuint(control, ==, 1);
	g_assert_cmpuint(starts, ==, 2);
	g_assert_cmpuint(ends, ==, 2);
	g_assert_cmpuint(video->len, ==, 3500);
	g_assert_cmpmem(video->data, 3000, frame, 3000);
	/* The receiver's ACKs released everything the sender kept. */
	g_assert_cmpuint(call_pdtp_stream_backlog(a.pdtp, 4), ==, 0);
	pdtp_peer_clear(&a);
	pdtp_peer_clear(&b);
}

/* An SRTP packet as the official app sends it, from a PDTP payload. */
static GBytes *pdtp_wire(const guint8 *payload, gsize n, guint16 seq)
{
	srtp_t tx;
	guint8 packet[1400] = { 0x80, 0x80 | 98 };
	int size = 12 + n;

	g_assert_true(call_srtp_open(&tx, pdtp_key_b, ssrc_specific, 222));
	packet[2] = seq >> 8;
	packet[3] = seq;
	packet[11] = 222;
	memcpy(packet + 12, payload, n);
	g_assert_cmpint(srtp_protect(tx, packet, &size), ==,
			srtp_err_status_ok);
	srtp_dealloc(tx);
	return g_bytes_new(packet, size);
}

static void pdtp_official_packets(void)
{
	struct pdtp_peer a;
	g_autoptr(GByteArray) p = g_byte_array_new();
	guint8 data[120];

	pdtp_peer_init(&a, pdtp_key_a, pdtp_key_b, 122, 222);
	/* "LINE" stream 110 opened with flags 2, as on every call. */
	static const guint8 open[] = { 0x80, 0x00, 'L',	 'I',  'N',
				       'E',  0,	   0x00, 0x08, 0x05,
				       0x01, 0x40, 0x6e, 0x02, 0x00 };
	g_autoptr(GBytes) w1 = pdtp_wire(open, sizeof(open), 2);
	gsize size;
	const guint8 *wire = g_bytes_get_data(w1, &size);

	g_assert_true(call_pdtp_receive(a.pdtp, wire, size, 0));
	g_assert_cmpuint(a.opens, ==, 1);
	/*
	 * A PLANET packet with INCLUDE_RETRANS and two STRM_DATA elements.
	 * The data group declares one byte more than it holds, because the
	 * engine adds its length varint's size.
	 */
	for (guint i = 0; i < sizeof(data); i++)
		data[i] = i;
	static const guint8 head[] = { 0x80, 0x00, 'P',	 'L',  'A',  'N', 'E',
				       'T',  0,	   0x01, 0x01, 0x01, 0x05 };
	static const guint8 time[] = { 0x08, 0xc0, 0x00, 0x46, 0x36,
				       0x2f, 0x8a, 0x00, 0x63 };
	g_byte_array_append(p, head, sizeof(head));
	gsize group = p->len;
	g_byte_array_append(p, (const guint8[]){ 0x40, 0x00, 0x02 }, 3);
	/* Start of a message: flags 0x5b, extensions 0x40, 0x10, 0x08, 0x02, 0x01. */
	g_byte_array_append(
		p, (const guint8[]){ 0x04, 0x5b, 0x01, 0x0a, 0, 0, 0 }, 7);
	g_byte_array_append(p, time, sizeof(time));
	g_byte_array_append(p, (const guint8[]){ 0x00, 0x40, 60 }, 3);
	g_byte_array_append(p, data, 60);
	/* End of it: flags 0x17, extensions 0x10, 0x04, 0x02, 0x01. */
	g_byte_array_append(p, (const guint8[]){ 0x04, 0x17, 0, 0, 0 }, 5);
	g_byte_array_append(p, time, sizeof(time));
	g_byte_array_append(p, (const guint8[]){ 0x40, 60, 0x40, 60 }, 4);
	g_byte_array_append(p, data + 60, 60);
	gsize body = p->len - group - 3 + 1;
	/* Declared length = body + varint size of body (2), count is 1 byte. */
	p->data[group] = 0x40 | (body + 1) >> 8;
	p->data[group + 1] = body + 1;
	g_autoptr(GBytes) w2 = pdtp_wire(p->data, p->len, 3);

	wire = g_bytes_get_data(w2, &size);
	g_assert_true(call_pdtp_receive(a.pdtp, wire, size, 0));
	g_assert_cmpuint(a.chunks->len, ==, 2);
	struct pdtp_chunk *c0 = &g_array_index(a.chunks, struct pdtp_chunk, 0),
			  *c1 = &g_array_index(a.chunks, struct pdtp_chunk, 1);

	g_assert_cmpuint(c0->stream, ==, 4);
	g_assert_cmpuint(c0->time, ==, G_GUINT64_CONSTANT(0x0046362f8a0063));
	g_assert_true(c0->flags & PDTP_MESSAGE_START);
	g_assert_true(c1->flags & PDTP_MESSAGE_END);
	g_assert_cmpuint(c1->offset, ==, 60);
	/* Both packets are acknowledged, the newest as the largest. */
	call_pdtp_poll(a.pdtp, 100000, pdtp_collect, &a);
	g_assert_cmpuint(a.wire->len, >=, 1);
	pdtp_peer_clear(&a);
}

static void pdtp_varints(void)
{
	static const guint64 values[] = {
		0,	  63,
		64,	  16383,
		16384,	  (1u << 30) - 1,
		1u << 30, G_GUINT64_CONSTANT(0x3fffffffffffffff)
	};
	for (guint i = 0; i < G_N_ELEMENTS(values); i++) {
		guint8 buf[8];
		gsize n = call_pdtp_varint(buf, values[i]);
		const guint8 *p = buf;
		guint64 v;

		g_assert_true(call_pdtp_read_varint(&p, buf + n, &v));
		g_assert_cmpuint(v, ==, values[i]);
		g_assert_true(p == buf + n);
	}
	static const guint8 two[] = { 0x40, 0x6e };
	const guint8 *p = two;
	guint64 v;

	g_assert_true(call_pdtp_read_varint(&p, two + 2, &v));
	g_assert_cmpuint(v, ==, 110);
	p = two;
	g_assert_false(call_pdtp_read_varint(&p, two + 1, &v));
}

int main(int argc, char **argv)
{
	g_setenv("LINE_EXPERIMENTAL_VIDEO", "0", TRUE);
	g_unsetenv("LINE_EXPERIMENTAL_GROUP_CALLS");
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_test_init(&argc, &argv, NULL);
	events = g_async_queue_new_full(g_free);
	g_test_add_func("/call/wire-vector", frame_vector);
	g_test_add_func("/call/pdtp-varints", pdtp_varints);
	g_test_add_func("/call/pdtp-loopback", pdtp_loopback);
	g_test_add_func("/call/pdtp-official-packets", pdtp_official_packets);
	g_test_add_func("/call/outgoing", outgoing);
	g_test_add_data_func("/call/group-voice", GUINT_TO_POINTER(0),
			     group_call);
	g_test_add_data_func("/call/group-video", GUINT_TO_POINTER(1),
			     group_call);
	g_test_add_data_func("/call/group-share/camera", GUINT_TO_POINTER(10),
			     group_call);
	g_test_add_data_func("/call/group-share/no-camera",
			     GUINT_TO_POINTER(11), group_call);
	g_test_add_data_func("/call/group-share/native-presentation",
			     GUINT_TO_POINTER(12), group_call);
	g_test_add_data_func("/call/group-share/native-presentation-voice",
			     GUINT_TO_POINTER(13), group_call);
	g_test_add_data_func("/call/group-video-software-svc",
			     GUINT_TO_POINTER(8), group_call);
	g_test_add_data_func("/call/group-video-legacy-svc",
			     GUINT_TO_POINTER(9), group_call);
	g_test_add_data_func("/call/group-video-no-packet-version",
			     GUINT_TO_POINTER(7), group_call);
	g_test_add_data_func("/call/group-voice-native-uplink",
			     GUINT_TO_POINTER(3), group_call);
	g_test_add_data_func("/call/group-video-native-uplink",
			     GUINT_TO_POINTER(4), group_call);
	g_test_add_data_func("/call/group-camera-unavailable",
			     GUINT_TO_POINTER(6), group_call);
	g_test_add_data_func("/call/group-another-device", GUINT_TO_POINTER(5),
			     group_call);
	g_test_add_data_func("/call/group-video-voice-fallback",
			     GUINT_TO_POINTER(2), group_call);
	g_test_add_func("/call/authenticated-media-migration", media_migration);
	g_test_add_func("/call/video-outgoing", video_outgoing);
	g_test_add_func("/call/voice-then-video", voice_then_video);
	g_test_add_func("/call/share-from-tablet", share_on_tablet);
	g_test_add_func("/call/share-to-tablet/video", share_on_tablet);
	g_test_add_func("/call/share-to-tablet/voice", share_on_tablet);
	g_test_add_func("/call/video-to-voice-fallback", video_voice_fallback);
	g_test_add_func("/call/video-incoming", video_incoming);
	g_test_add_func("/call/rejected", rejected);
	g_test_add_func("/call/route-checks", route_checks);
	g_test_add_data_func("/call/incoming-accept", GINT_TO_POINTER(FALSE),
			     incoming);
	g_test_add_data_func("/call/incoming-decline", GINT_TO_POINTER(TRUE),
			     incoming);
	int result = g_test_run();

	g_async_queue_unref(events);
	return result;
}
