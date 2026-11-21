// SPDX-License-Identifier: GPL-3.0-or-later
#include "call.h"
#include "call_audio.h"
#include "call_pdtp.h"
#include "call_srtp.h"
#include "call_schema.h"
#include "call_video.h"
#include "media_io.h"
#include "screencast.h"
#include <sys/timerfd.h>
#include <arpa/inet.h>
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
static GAsyncQueue *events;
static int server_fd, server_port, rpc_count;
static EVP_PKEY *relay_key;
static guint8 relay_public[33];
static gint playback_frames;
static gint audio_devices;
static gboolean bad_route, video_fixture, voice_fallback, migrate_media;
static gboolean start_with_camera_off;
static gboolean share_fixture;
static gint cameras_opened;
static int test_camera_fd = -1, preview_fd = -1;
#define audio_active (g_atomic_int_get(&audio_devices) > 0)

J *line_rpc(Line *s, const char *path, const char *method, J *args, GError **e)
{
	(void)e;
	g_assert_cmpstr(s->mid, ==, self);
	g_assert_cmpstr(path, ==, "/V4");
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

	g_assert_cmpstr(strfield(event, "chat"), ==, peer);
	if (!g_strcmp0(type, "call_incoming"))
		line_event(type, "chat", peer);
	else if (!g_strcmp0(type, "call_share")) {
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
						!start_with_camera_off);
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

static void synthetic_frame(struct line_camera_frame *frame)
{
	static guint8 y[160 * 120], u[80 * 60], v[80 * 60];

	static gsize initialized;

	if (g_once_init_enter(&initialized)) {
		memset(y, 96, sizeof(y));
		memset(u, 128, sizeof(u));
		memset(v, 128, sizeof(v));
		g_once_init_leave(&initialized, 1);
	}
	*frame = (struct line_camera_frame){
		.width = 160,
		.height = 120,
		.plane = { y, u, v },
		.stride = { 160, 80, 80 },
	};
}

int line_camera_read(struct line_camera *camera,
		     struct line_camera_frame *frame, GError **error)
{
	(void)error;
	g_assert_nonnull(camera);
	guint64 n;

	if (read(test_camera_fd, &n, sizeof(n)) != sizeof(n))
		return 0;
	synthetic_frame(frame);
	return 1;
}

enum {
	SCREEN_WIDTH = 320,
	SCREEN_HEIGHT = 180
};

struct line_screencast {
	int fd;
	gboolean ready;
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
	static const struct itimerspec first = { .it_value.tv_nsec = 1000000 };

	cast->fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	g_assert_cmpint(cast->fd, >=, 0);
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

	(void)message;
	cast->ready = read(cast->fd, &ticks, sizeof(ticks)) == sizeof(ticks);
	return LINE_SCREENCAST_RUNNING;
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
	gsize size = line_camera_i420_size(*width, *height);
	guint8 *pixels = g_malloc(size);

	memset(pixels, 168, SCREEN_WIDTH * SCREEN_HEIGHT);
	memset(pixels + SCREEN_WIDTH * SCREEN_HEIGHT, 128,
	       size - SCREEN_WIDTH * SCREEN_HEIGHT);
	return g_bytes_new_take(pixels, size);
}

SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *device, int capture,
					     const SDL_AudioSpec *desired,
					     SDL_AudioSpec *obtained,
					     int changes);
void __real_SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
int __real_SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *name, int input,
					     const SDL_AudioSpec *want,
					     SDL_AudioSpec *got, int flags)
{
	SDL_AudioDeviceID device =
		__real_SDL_OpenAudioDevice(name, input, want, got, flags);
	if (device)
		g_atomic_int_inc(&audio_devices);
	return device;
}

void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID device)
{
	__real_SDL_CloseAudioDevice(device);
	g_atomic_int_add(&audio_devices, -1);
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

static void expect_audio(int fd, srtp_t ctx, guint8 pt, guint32 ssrc)
{
	guint8 packet[2048];
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	int n = 0;
	gint64 expires = g_get_monotonic_time() + 2000000;

	while (g_get_monotonic_time() < expires && poll(&pfd, 1, 200) == 1) {
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

	g_assert_cmpuint(frame->width, ==, 160);
	g_assert_cmpuint(frame->height, ==, 120);
	endpoint->got_frame = TRUE;
}

static GBytes *direct_video(int *width, int *height, int *rotation,
			    gboolean *camera_enabled)
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
			*width = msg.width;
			*height = msg.height;
			*rotation = msg.rotation;
			*camera_enabled = msg.camera_enabled;
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
	int width = 0, height = 0, rotation = -1;
	gboolean camera_enabled = FALSE;

	g_autoptr(GBytes) picture = NULL;

	if (preview_fd >= 0)
		picture = direct_video(&width, &height, &rotation,
				       &camera_enabled);
	else {
		expect_event("call_frame u22222222222222222222222222222222");
		picture = line_call_take_frame(peer, &width, &height, &rotation,
					       &camera_enabled);
	}

	g_assert_nonnull(picture);
	g_assert_cmpint(camera_enabled, ==, local_camera);
	g_assert_cmpint(rotation, ==, 0);
	g_assert_cmpint(width, ==, WIDTH);
	g_assert_cmpint(height, ==, HEIGHT);
	g_assert_cmpuint(g_bytes_get_size(picture), ==,
			 WIDTH * HEIGHT + 2 * CHROMA * CHROMA_ROWS);
	struct line_camera_frame decoded;

	line_camera_i420_frame(&decoded, g_bytes_get_data(picture, NULL), width,
			       height);
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
	g_assert_null(
		line_call_take_frame(peer, &width, &height, &rotation, NULL));
	if (!local_camera) {
		call_video_close(video);
		return;
	}
	struct pollfd f = { .fd = fd, .events = POLLIN };

	gint64 deadline = g_get_monotonic_time() + 2000000;

	while (!endpoint.got_frame && g_get_monotonic_time() < deadline &&
	       poll(&f, 1, 200) == 1) {
		guint8 message[2048];
		ssize_t n = recv(fd, message, sizeof(message), 0);

		if (n >= 12 && (message[1] & 127) == pt)
			call_video_receive(video, message, n, peer_display,
					   &endpoint);
		/* Like the official app, ask again after missing a keyframe. */
		if (call_video_needs_keyframe(video))
			call_video_send_feedback(video, send_peer_video,
						 &endpoint);
	}
	g_assert_true(endpoint.got_frame);
	/* The same captured frames feed the window's local preview. */
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
	guint width;
	guint8 luma;
	guint received;
};

static void check_share_picture(const struct line_camera_frame *frame,
				gpointer data)
{
	struct share_picture *expected = data;

	if (frame->width != expected->width)
		return;
	g_assert_cmpint(ABS((int)frame->plane[0][0] - expected->luma), <=, 8);
	expected->received++;
}

static void receive_share_pictures(int fd, struct call_video *video,
				   struct share_picture *expected, guint count,
				   gint64 timeout)
{
	gint64 deadline = g_get_monotonic_time() + timeout;
	struct pollfd fds = { .fd = fd, .events = POLLIN };

	while (expected->received < count) {
		gint64 remaining = deadline - g_get_monotonic_time();
		guint8 packet[2048];

		if (remaining <= 0 ||
		    poll(&fds, 1, (remaining + 999) / 1000) != 1)
			break;
		ssize_t n = recv(fd, packet, sizeof(packet), 0);

		if (n >= 12 && (packet[0] & 0xc0) == 0x80 &&
		    (packet[1] & 127) == 109)
			call_video_receive(video, packet, n,
					   check_share_picture, expected);
	}
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
	struct share_picture screen = { .width = SCREEN_WIDTH, .luma = 168 };

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
		struct share_picture camera = { .width = 160, .luma = 96 };

		receive_share_pictures(fd, video, &camera, 1,
				       2 * G_USEC_PER_SEC);
		g_assert_cmpuint(camera.received, ==, 1);
		g_assert_cmpint(g_atomic_int_get(&cameras_opened), ==, 2);
	}
	call_video_close(video);
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
	if (share_fixture)
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
		start_with_camera_off =
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
	if (!video_fixture) {
		g_autoptr(J) video = incoming_op(++revision, 0, "CV");

		line_voice_operation(&session, video);
	}
	g_assert_cmpint(g_async_queue_length(events), ==, 0);
	g_autoptr(J) op =
		incoming_op(++revision, 0, video_fixture ? "CV" : "CA");
	line_voice_operation(&session, op);
	line_voice_operation(&session, op);
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
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_test_init(&argc, &argv, NULL);
	events = g_async_queue_new_full(g_free);
	g_test_add_func("/call/wire-vector", frame_vector);
	g_test_add_func("/call/pdtp-varints", pdtp_varints);
	g_test_add_func("/call/pdtp-loopback", pdtp_loopback);
	g_test_add_func("/call/pdtp-official-packets", pdtp_official_packets);
	g_test_add_func("/call/outgoing", outgoing);
	g_test_add_func("/call/authenticated-media-migration", media_migration);
	g_test_add_func("/call/video-outgoing", video_outgoing);
	g_test_add_func("/call/voice-then-video", voice_then_video);
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
