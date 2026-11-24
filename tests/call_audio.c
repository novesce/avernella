// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_audio.h"
#include <SDL.h>
#include <arpa/inet.h>
#include <opus/opus.h>
#include <srtp2/srtp.h>
#include <string.h>

static guint frames_20ms, frames_40ms;
static gboolean hold_playback;
static guint queued_samples;
static const guint8 *captured_pcm;
static Uint32 captured_bytes;

Uint32 __real_SDL_GetQueuedAudioSize(SDL_AudioDeviceID device);
Uint32 __real_SDL_DequeueAudio(SDL_AudioDeviceID device, void *pcm, Uint32 len);

Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID device)
{
	return captured_pcm ? captured_bytes :
			      __real_SDL_GetQueuedAudioSize(device);
}

Uint32 __wrap_SDL_DequeueAudio(SDL_AudioDeviceID device, void *pcm, Uint32 len)
{
	if (!captured_pcm)
		return __real_SDL_DequeueAudio(device, pcm, len);
	len = MIN(len, captured_bytes);
	memcpy(pcm, captured_pcm, len);
	captured_pcm += len;
	captured_bytes -= len;
	return len;
}
static opus_int16 played_20ms[960];
static opus_int16 played_40ms[1920];

int __real_SDL_QueueAudio(SDL_AudioDeviceID device, const void *pcm,
			  Uint32 len);

int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void *pcm, Uint32 len)
{
	if (hold_playback) {
		SDL_PauseAudioDevice(device, 1);
		queued_samples += len / sizeof(opus_int16);
		if (len == sizeof(played_20ms))
			memcpy(played_20ms, pcm, sizeof(played_20ms));
		if (len == sizeof(played_40ms))
			memcpy(played_40ms, pcm, sizeof(played_40ms));
	}
	if (len == 960 * sizeof(opus_int16))
		frames_20ms++;
	else if (len == 1920 * sizeof(opus_int16))
		frames_40ms++;
	return __real_SDL_QueueAudio(device, pcm, len);
}

static void loopback(void)
{
	guint8 key[30] = { 1, 2, 3 };

	g_autoptr(GError) e = NULL;
	LineAudio *a = line_audio_open(key, key, 96, 45678, 45678, &e);

	g_assert_no_error(e);
	g_assert_nonnull(a);
	guint8 packet[2048];
	int n = 0;

	for (int i = 0; i < 100 && !n; i++) {
		g_usleep(5000);
		n = line_audio_capture(a, packet, sizeof(packet), &e);
	}
	g_assert_no_error(e);
	g_assert_cmpint(n, >, 23);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_false(line_audio_receive(a, packet, n));
	srtp_policy_t policy = { 0 };

	srtp_crypto_policy_set_rtp_default(&policy.rtp);
	srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
	policy.key = key;
	policy.ssrc.type = ssrc_any_inbound;
	srtp_t peer = NULL;

	g_assert_cmpint(srtp_create(&peer, &policy), ==, srtp_err_status_ok);
	g_assert_cmpint(srtp_unprotect(peer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmpint(packet[1] & 127, ==, 96);
	g_assert_cmpint(packet[12], ==, 0);
	g_assert_cmpint(opus_packet_get_nb_samples(packet + 13, n - 13, 48000),
			==, 960);
	line_audio_mute(a, TRUE);
	g_assert_cmpint(line_audio_capture(a, packet, sizeof(packet), &e), ==,
			0);
	line_audio_mute(a, FALSE);
	n = 0;
	for (int i = 0; i < 100 && !n; i++) {
		g_usleep(5000);
		n = line_audio_capture(a, packet, sizeof(packet), &e);
	}
	g_assert_no_error(e);
	g_assert_cmpint(n, >, 23);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpint(srtp_unprotect(peer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmphex(packet[1], ==, 0x80 | 96);
	srtp_dealloc(peer);
	line_audio_close(a);
}

static int protect_payload_at(guint8 *packet, guint8 key[30], guint8 pt,
			      guint32 ssrc, guint16 seq, guint32 stamp,
			      const guint8 *payload, int payload_len)
{
	srtp_policy_t policy = { 0 };
	srtp_t sender = NULL;

	memset(packet, 0, 12);
	packet[0] = 0x80;
	packet[1] = pt;
	stamp = htonl(stamp);
	seq = htons(seq);
	ssrc = htonl(ssrc);
	memcpy(packet + 2, &seq, sizeof(seq));
	memcpy(packet + 4, &stamp, sizeof(stamp));
	memcpy(packet + 8, &ssrc, sizeof(ssrc));
	memcpy(packet + 12, payload, payload_len);
	int n = 12 + payload_len;

	srtp_crypto_policy_set_rtp_default(&policy.rtp);
	srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
	policy.key = key;
	policy.ssrc.type = ssrc_any_outbound;
	g_assert_cmpint(srtp_create(&sender, &policy), ==, srtp_err_status_ok);
	g_assert_cmpint(srtp_protect(sender, packet, &n), ==,
			srtp_err_status_ok);
	srtp_dealloc(sender);
	return n;
}

static int protect_payload(guint8 *packet, guint8 key[30], guint8 pt,
			   guint32 ssrc, guint16 seq, const guint8 *payload,
			   int payload_len)
{
	return protect_payload_at(packet, key, pt, ssrc, seq,
				  (guint32)seq * 960, payload, payload_len);
}

static int protect_at(guint8 *packet, guint8 key[30], guint8 pt, guint32 ssrc,
		      guint16 seq, guint32 stamp)
{
	guint8 payload[1276] = { 0 };
	opus_int16 samples[960] = { 0 };
	int ret;
	OpusEncoder *encoder =
		opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &ret);

	g_assert_cmpint(ret, ==, OPUS_OK);
	int n = opus_encode(encoder, samples, G_N_ELEMENTS(samples),
			    payload + 1, sizeof(payload) - 1);

	g_assert_cmpint(n, >, 0);
	opus_encoder_destroy(encoder);
	return protect_payload_at(packet, key, pt, ssrc, seq, stamp, payload,
				  n + 1);
}

static int protect(guint8 *packet, guint8 key[30], guint8 pt, guint32 ssrc,
		   guint16 seq)
{
	return protect_at(packet, key, pt, ssrc, seq, (guint32)seq * 960);
}

static void incoming_stream(void)
{
	guint8 tx[30] = { 1, 2, 3 }, rx[30] = { 4, 5, 6 };
	guint8 packet[2048];

	g_autoptr(GError) error = NULL;
	LineAudio *audio = line_audio_open(tx, rx, 111, 201, 101, &error);

	g_assert_no_error(error);
	g_assert_nonnull(audio);
	int n = protect(packet, rx, 96, 101, 2);

	g_assert_false(line_audio_receive(audio, packet, n));
	n = protect(packet, rx, 111, 201, 2);
	g_assert_false(line_audio_receive(audio, packet, n));
	n = protect(packet, tx, 111, 101, 2);
	g_assert_false(line_audio_receive(audio, packet, n));
	n = protect(packet, rx, 111, 101, 2);
	packet[n - 1] ^= 1;
	g_assert_false(line_audio_receive(audio, packet, n));
	packet[n - 1] ^= 1;
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_false(line_audio_receive(audio, packet, n));
	line_audio_close(audio);
}

static void reorder_and_loss(void)
{
	guint8 tx[30] = { 1 }, rx[30] = { 2 }, packet[2048];

	g_autoptr(GError) error = NULL;
	LineAudio *audio = line_audio_open(tx, rx, 96, 202, 102, &error);

	g_assert_no_error(error);
	g_assert_nonnull(audio);
	int n = protect(packet, rx, 96, 102, 10);

	g_assert_true(line_audio_receive(audio, packet, n));
	n = protect(packet, rx, 96, 102, 12);
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_cmpint(line_audio_playout(audio, 0), <, G_MAXINT64);
	n = protect(packet, rx, 96, 102, 11);
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_cmpint(line_audio_playout(audio, 0), ==, G_MAXINT64);

	n = protect(packet, rx, 96, 102, 14);
	g_assert_true(line_audio_receive(audio, packet, n));
	gint64 due = line_audio_playout(audio, 0);

	g_assert_cmpint(due, <, G_MAXINT64);
	g_assert_cmpint(line_audio_playout(audio, due - 1), ==, due);
	g_assert_cmpint(line_audio_playout(audio, due), ==, G_MAXINT64);
	n = protect(packet, rx, 96, 102, 13);
	g_assert_false(line_audio_receive(audio, packet, n));
	line_audio_close(audio);
}

static void rtp_audio_timeline(void)
{
	guint8 tx[30] = { 1 }, rx[30] = { 2 }, packet[2048];

	g_autoptr(GError) error = NULL;
	LineAudio *audio = line_audio_open(tx, rx, 96, 202, 102, &error);

	g_assert_no_error(error);
	g_assert_nonnull(audio);
	guint before = frames_20ms;
	int n = protect_at(packet, rx, 96, 102, 1, 0);

	g_assert_true(line_audio_receive(audio, packet, n));
	n = protect_at(packet, rx, 96, 102, 3, 960);
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_cmpint(line_audio_playout(audio, G_MAXINT64), ==, G_MAXINT64);
	/* A gap in sequence alone must not produce concealed PCM. */
	g_assert_cmpuint(frames_20ms - before, ==, 2);

	n = protect_at(packet, rx, 96, 102, 5, 1920);
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_cmpint(line_audio_playout(audio, 0), <, G_MAXINT64);
	n = protect_at(packet, rx, 96, 102, 16, 2880);
	g_assert_true(line_audio_receive(audio, packet, n));
	/* The newer packet must not discard the earlier waiting speech. */
	g_assert_cmpuint(frames_20ms - before, ==, 4);

	n = protect_at(packet, rx, 96, 102, 18, 4800);
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_cmpint(line_audio_playout(audio, G_MAXINT64), ==, G_MAXINT64);
	/* A real 20 ms gap still gets one frame of Opus FEC or concealment. */
	g_assert_cmpuint(frames_20ms - before, ==, 6);
	line_audio_close(audio);
}

static void eas2(void)
{
	guint8 tx[30] = { 10 }, rx[30] = { 11 }, packet[2048], frame[1275];
	guint8 payload[2048];
	opus_int16 pcm[960];

	g_autoptr(GError) error = NULL;
	int status;
	LineAudio *audio = line_audio_open(tx, rx, 96, 202, 102, &error);

	g_assert_no_error(error);
	g_assert_nonnull(audio);
	OpusEncoder *encoder = opus_encoder_create(
		48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &status);

	g_assert_cmpint(status, ==, OPUS_OK);
	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)), ==,
			OPUS_OK);
	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000)), ==,
			OPUS_OK);
	for (guint i = 0; i < G_N_ELEMENTS(pcm); i++)
		pcm[i] = i % 80 < 40 ? 4000 : -4000;
	int n = opus_encode(encoder, pcm, G_N_ELEMENTS(pcm), frame,
			    sizeof(frame));

	g_assert_cmpint(n, ==, 60);
	g_assert_cmphex(frame[0] & 0x83, ==, 0x80);
	payload[0] = 0;
	payload[1] = frame[0] | 1;
	memcpy(payload + 2, frame + 1, n - 1);
	int wire = protect_payload(packet, rx, 96, 102, 1, payload, n + 1);

	g_assert_true(line_audio_receive(audio, packet, wire));
	/* Two same-TOC CELT frames, CBR, with the native activity byte. */
	payload[0] = 0x10;
	payload[1] = frame[0] | 3;
	payload[2] = 2;
	payload[3] = 0xc0;
	memcpy(payload + 4, frame + 1, n - 1);
	memcpy(payload + 4 + n - 1, frame + 1, n - 1);
	opus_int16 sizes[48];
	int len = 4 + (n - 1) * 2;

	g_assert_cmpint(opus_packet_parse(payload + 1, len - 1, NULL, NULL,
					  sizes, NULL),
			<, 0);
	wire = protect_payload(packet, rx, 96, 102, 2, payload, len);
	g_assert_true(line_audio_receive(audio, packet, wire));
	/* VBR lengths also follow the activity byte. */
	payload[2] = 0x82;
	payload[4] = n - 1;
	memcpy(payload + 5, frame + 1, n - 1);
	memcpy(payload + 5 + n - 1, frame + 1, n - 1);
	wire = protect_payload(packet, rx, 96, 102, 3, payload, len + 1);
	g_assert_true(line_audio_receive(audio, packet, wire));
	wire = protect_payload(packet, rx, 96, 102, 4, payload, 4);
	g_assert_false(line_audio_receive(audio, packet, wire));
	payload[2] = 0xc2;
	wire = protect_payload(packet, rx, 96, 102, 5, payload, len + 1);
	g_assert_false(line_audio_receive(audio, packet, wire));
	/* Different frame configurations use separate TOCs under code 2. */
	guint8 second[1275];

	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000)), ==,
			OPUS_OK);
	g_assert_cmpint(
		opus_encoder_ctl(encoder,
				 OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND)),
		==, OPUS_OK);
	int n2 = opus_encode(encoder, pcm, G_N_ELEMENTS(pcm), second,
			     sizeof(second));

	g_assert_cmpint(n2, ==, 40);
	g_assert_cmphex(second[0] & 0x83, ==, 0x80);
	g_assert_cmphex(second[0], !=, frame[0]);
	payload[1] = frame[0] | 2;
	payload[2] = second[0] | 1;
	payload[3] = 0x82;
	payload[4] = 0xc0;
	payload[5] = n - 1;
	memcpy(payload + 6, frame + 1, n - 1);
	memcpy(payload + 6 + n - 1, second + 1, n2 - 1);
	wire = protect_payload(packet, rx, 96, 102, 6, payload, 4 + n + n2);
	guint before = frames_40ms;

	g_assert_true(line_audio_receive(audio, packet, wire));
	g_assert_cmpint(line_audio_playout(audio, G_MAXINT64), ==, G_MAXINT64);
	g_assert_cmpuint(frames_40ms - before, ==, 1);
	payload[2] &= 0xfe;
	wire = protect_payload(packet, rx, 96, 102, 7, payload, 3);
	g_assert_false(line_audio_receive(audio, packet, wire));
	payload[2] |= 1;
	payload[3] = 0xc2;
	wire = protect_payload(packet, rx, 96, 102, 8, payload, 4 + n + n2);
	g_assert_false(line_audio_receive(audio, packet, wire));
	payload[3] = 0x82;
	payload[5] = 252;
	wire = protect_payload(packet, rx, 96, 102, 9, payload, 6);
	g_assert_false(line_audio_receive(audio, packet, wire));
	opus_encoder_destroy(encoder);
	line_audio_close(audio);
}

static char *audio_report;

static void save_audio_report(const gchar *message)
{
	g_free(audio_report);
	audio_report = g_strdup(message);
}

static void group_send(void)
{
	guint8 key[30] = { 14 }, packet[2048], wire[2048];
	g_autoptr(GError) e = NULL;
	LineAudio *a =
		line_audio_open_group(key, key, 98, 98, 45678, 45678, &e);

	g_assert_no_error(e);
	g_assert_nonnull(a);
	line_audio_enable_diagnostics(a);
	srtp_policy_t policy = { 0 };
	srtp_t peer = NULL;

	srtp_crypto_policy_set_rtp_default(&policy.rtp);
	srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
	policy.key = key;
	policy.ssrc.type = ssrc_any_inbound;
	g_assert_cmpint(srtp_create(&peer, &policy), ==, srtp_err_status_ok);
	guint32 previous = 0, configs = 0;
	guint before = frames_40ms;

	for (int i = 0; i < 4; i++) {
		int n = 0;

		for (int poll = 0; poll < 100 && !n; poll++) {
			g_usleep(5000);
			n = line_audio_capture(a, packet, sizeof(packet), &e);
		}
		g_assert_no_error(e);
		g_assert_cmpint(n, >, 40);
		g_assert_true(line_audio_receive(a, packet, n));
		memcpy(wire, packet, n);
		g_assert_cmpint(srtp_unprotect(peer, wire, &n), ==,
				srtp_err_status_ok);
		g_assert_cmphex(wire[0], ==, 0x90);
		g_assert_cmphex(wire[1], ==, 98);
		static const guint8 extension[] = { 0x02, 0x40, 0,   2, 1, 2,
						    0xc0, 0x40, 127, 0, 0, 0 };

		g_assert_cmpmem(wire + 12, sizeof(extension), extension,
				sizeof(extension));
		g_assert_cmphex(wire[24], ==, 0);
		g_assert_cmphex(wire[25] & 0x87, ==, 0x83);
		configs |= 1u << (wire[25] >> 3);
		g_assert_cmphex(wire[26] & 127, ==, 2);
		g_assert_cmphex(wire[27], ==, 0);
		/* Removing the bitmap leaves an ordinary two-frame Opus payload. */
		wire[27] = wire[26];
		wire[26] = wire[25];
		opus_int16 sizes[48];

		g_assert_cmpint(opus_packet_parse(wire + 26, n - 26, NULL, NULL,
						  sizes, NULL),
				==, 2);
		guint32 stamp;

		memcpy(&stamp, wire + 4, sizeof(stamp));
		stamp = ntohl(stamp);
		if (i)
			g_assert_cmpuint(stamp - previous, ==, 1920);
		previous = stamp;
	}
	g_assert_cmpuint(frames_40ms - before, ==, 4);
	g_autofree char *expected =
		g_strdup_printf(" tx-configs=%08x", configs);
	GPrintFunc previous_handler = g_set_printerr_handler(save_audio_report);

	line_audio_report(a);
	g_set_printerr_handler(previous_handler);
	g_assert_nonnull(audio_report);
	g_assert_nonnull(strstr(audio_report, expected));
	g_clear_pointer(&audio_report, g_free);
	srtp_dealloc(peer);
	line_audio_close(a);
}

static void group_microphone(gconstpointer stuck_channel)
{
	guint8 key[30] = { 42 }, packet[2048];
	opus_int16 stereo[2 * 1920], decoded[1920];
	g_autoptr(GError) e = NULL;
	LineAudio *audio =
		line_audio_open_group(key, key, 98, 98, 202, 102, &e);
	srtp_policy_t policy = { 0 };
	srtp_t peer = NULL;
	int rc;
	OpusDecoder *decoder = opus_decoder_create(48000, 1, &rc);

	g_assert_no_error(e);
	g_assert_nonnull(audio);
	g_assert_cmpint(rc, ==, OPUS_OK);
	line_audio_enable_diagnostics(audio);
	srtp_crypto_policy_set_rtp_default(&policy.rtp);
	srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
	policy.key = key;
	policy.ssrc.type = ssrc_any_inbound;
	g_assert_cmpint(srtp_create(&peer, &policy), ==, srtp_err_status_ok);
	for (guint i = 0; i < G_N_ELEMENTS(decoded); i++) {
		int phase = i % 240;
		int sample = 4 * (MIN(phase, 240 - phase) - 60);

		/* One channel can be stuck at DC, or the pair wired in antiphase. */
		stereo[2 * i] = stuck_channel ? 4096 : sample;
		stereo[2 * i + 1] = -sample;
	}
	captured_pcm = (const guint8 *)stereo;
	captured_bytes = sizeof(stereo) / 2;
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			==, 0);
	g_assert_cmpuint(captured_bytes, ==, sizeof(stereo) / 2);
	guint chunk = 0;
	gboolean was_active = FALSE;

	for (guint i = 0; i < 8; i++) {
		captured_pcm = (const guint8 *)stereo;
		captured_bytes = sizeof(stereo);
		int n = line_audio_capture(audio, packet, sizeof(packet), &e);

		g_assert_no_error(e);
		g_assert_cmpint(n, >, 40);
		g_assert_cmpuint(captured_bytes, ==, 0);
		g_assert_cmpint(srtp_unprotect(peer, packet, &n), ==,
				srtp_err_status_ok);
		guint32 stamp;

		memcpy(&stamp, packet + 4, sizeof(stamp));
		stamp = ntohl(stamp);
		g_assert_cmpuint(stamp, ==, 960 + 1920u * i);
		guint first = (packet[19] >> 4) & 3,
		      second = (packet[19] >> 2) & 3;

		g_assert_cmpuint(first, <=, 2);
		g_assert_cmpuint(second, <=, 2);
		g_assert_cmphex(packet[27], ==,
				(first ? 0x80 : 0) | (second ? 0x40 : 0));
		gboolean active = packet[27] != 0;
		gboolean marker = active && !was_active;

		chunk += marker;
		was_active = active;
		g_assert_cmphex(packet[1], ==, 98 | (marker ? 0x80 : 0));
		g_assert_cmphex(packet[24], ==, chunk << 4);
		if (i == 7) {
			g_assert_cmphex(packet[19], ==, 0x68);
			g_assert_cmpuint(packet[20], <, 127);
			g_assert_cmphex(packet[27], ==, 0xc0);
		}
		packet[27] = packet[26];
		packet[26] = packet[25];
		g_assert_cmpint(opus_decode(decoder, packet + 26, n - 26,
					    decoded, G_N_ELEMENTS(decoded), 0),
				==, G_N_ELEMENTS(decoded));
	}
	guint64 power = 0;

	for (guint i = 0; i < G_N_ELEMENTS(decoded); i++)
		power += (int)decoded[i] * decoded[i];
	g_assert_cmpuint(power / G_N_ELEMENTS(decoded), >, 500);
	GPrintFunc previous_handler = g_set_printerr_handler(save_audio_report);

	line_audio_report(audio);
	g_set_printerr_handler(previous_handler);
	g_assert_nonnull(audio_report);
	g_assert_nonnull(
		strstr(audio_report,
		       " mic-checks=9 mic-blocks=8 mic-ac-power-max=19202 "));
	g_clear_pointer(&audio_report, g_free);
	line_audio_mute(audio, TRUE);
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			==, 0);
	line_audio_mute(audio, FALSE);
	static const guint8 silence[sizeof(stereo)];

	captured_pcm = silence;
	captured_bytes = sizeof(silence);
	int n = line_audio_capture(audio, packet, sizeof(packet), &e);

	g_assert_no_error(e);
	g_assert_cmpint(n, >, 40);
	g_assert_cmpint(srtp_unprotect(peer, packet, &n), ==,
			srtp_err_status_ok);
	g_assert_cmphex(packet[19], ==, 0x40);
	g_assert_cmphex(packet[27], ==, 0);
	g_assert_cmpuint(packet[20], ==, 127);
	g_assert_cmphex(packet[24], ==, chunk << 4);
	g_assert_cmphex(packet[1], ==, 98);

	/* The next talkspurt advances the chunk and sets the RTP marker. */
	for (guint i = 0; i < 8; i++) {
		captured_pcm = (const guint8 *)stereo;
		captured_bytes = sizeof(stereo);
		n = line_audio_capture(audio, packet, sizeof(packet), &e);
		g_assert_no_error(e);
		g_assert_cmpint(n, >, 40);
		g_assert_cmpint(srtp_unprotect(peer, packet, &n), ==,
				srtp_err_status_ok);
		if (!packet[27])
			continue;
		g_assert_cmphex(packet[24], ==, (chunk + 1) << 4);
		g_assert_cmphex(packet[1], ==, 98 | 0x80);
		break;
	}
	g_assert_cmphex(packet[27], !=, 0);
	captured_pcm = NULL;
	opus_decoder_destroy(decoder);
	srtp_dealloc(peer);
	line_audio_close(audio);
}

static void hear_microphones(const opus_int16 *left, const opus_int16 *right,
			     opus_int16 *output)
{
	LineAudio *audio;
	guint8 key[30] = { 41 }, packet[2048];
	opus_int16 stereo[2 * 1920];
	guint before = frames_40ms;
	GError *error = NULL;

	audio = line_audio_open_group(key, key, 98, 98, 202, 202, &error);
	g_assert_no_error(error);
	g_assert_nonnull(audio);
	for (int i = 0; i < 1920; i++) {
		stereo[2 * i] = left[i];
		stereo[2 * i + 1] = right[i];
	}
	hold_playback = TRUE;
	for (int i = 0; i < 8; i++) {
		captured_pcm = (const guint8 *)stereo;
		captured_bytes = sizeof(stereo);
		int n = line_audio_capture(audio, packet, sizeof(packet),
					   &error);

		g_assert_no_error(error);
		g_assert_cmpint(n, >, 0);
		g_assert_true(line_audio_receive(audio, packet, n));
	}
	g_assert_cmpuint(frames_40ms - before, ==, 8);
	memcpy(output, played_40ms, sizeof(played_40ms));
	hold_playback = FALSE;
	captured_pcm = NULL;
	line_audio_close(audio);
}

static void group_microphone_clipping(void)
{
	opus_int16 voice[1920], clipped[1920], noise[1920];
	opus_int16 reference[1920], decoded[1920];
	guint32 random = 1;

	for (int i = 0; i < 1920; i++) {
		int phase = i % 240;

		voice[i] = 128 * (MIN(phase, 240 - phase) - 60);
		clipped[i] = phase < 24 ? -32768 :
			     phase < 48 ? 32767 :
					  voice[i];
		random = 1664525 * random + 1013904223;
		noise[i] = (int)(random >> 18) - 8192;
	}
	hear_microphones(voice, voice, reference);
	guint64 power = 0;

	for (guint i = 0; i < G_N_ELEMENTS(reference); i++)
		power += (int)reference[i] * reference[i];
	g_assert_cmpuint(power / G_N_ELEMENTS(reference), >, 500);
	hear_microphones(clipped, voice, decoded);
	g_assert_cmpmem(decoded, sizeof(decoded), reference, sizeof(reference));
	/* The overload alone must not switch the mic to a noise-only channel. */
	hear_microphones(clipped, clipped, reference);
	hear_microphones(noise, clipped, decoded);
	g_assert_cmpmem(decoded, sizeof(decoded), reference, sizeof(reference));
}

/* Inspect the exact encoded VSD before the receiver's playback processing. */
static void group_microphone_signals(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed();
		return;
	}
	g_setenv("LINE_NOISE_SUPPRESSION", "0", TRUE);
	guint8 key[30] = { 84 }, packet[2048];
	g_autoptr(GError) error = NULL;
	LineAudio *audio =
		line_audio_open_group(key, key, 98, 98, 202, 102, &error);
	srtp_policy_t policy = { 0 };
	srtp_t peer = NULL;
	opus_int16 pcm[2 * 1920];
	guint32 noise = 1;
	static const guint8 expected[][3] = {
		{ 0x48, 0x40, 10 }, /* silence, voiced */
		{ 0x58, 0xc0, 10 }, /* unvoiced, voiced */
		{ 0x40, 0x00, 90 }, /* quantization noise, silence */
	};

	g_assert_no_error(error);
	g_assert_nonnull(audio);
	srtp_crypto_policy_set_rtp_default(&policy.rtp);
	srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
	policy.key = key;
	policy.ssrc.type = ssrc_any_inbound;
	g_assert_cmpint(srtp_create(&peer, &policy), ==, srtp_err_status_ok);
	for (guint i = 0; i < G_N_ELEMENTS(expected); i++) {
		for (int j = 0; j < 1920; j++) {
			int sample = 0;

			if (i < 2 && j >= 960)
				sample = j % 240 < 120 ? 10000 : -10000;
			else if (i == 1) {
				noise = 1664525 * noise + 1013904223;
				sample = (int)(noise >> 19) - 4096;
			} else if (i == 2 && j < 960)
				sample = j & 1 ? 1 : -1;
			pcm[2 * j] = pcm[2 * j + 1] = sample;
		}
		captured_pcm = (const guint8 *)pcm;
		captured_bytes = sizeof(pcm);
		int n = line_audio_capture(audio, packet, sizeof(packet),
					   &error);

		g_assert_no_error(error);
		g_assert_cmpint(n, >, 40);
		g_assert_cmpint(srtp_unprotect(peer, packet, &n), ==,
				srtp_err_status_ok);
		g_assert_cmphex(packet[19], ==, expected[i][0]);
		g_assert_cmphex(packet[27], ==, expected[i][1]);
		g_assert_cmpuint(packet[20], ==, expected[i][2]);
	}
	captured_pcm = NULL;
	srtp_dealloc(peer);
	line_audio_close(audio);
}

static int group_fixture(guint8 payload[256])
{
	opus_int16 pcm[960];
	guint8 frame[120];
	int status;
	OpusEncoder *encoder = opus_encoder_create(
		48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &status);

	g_assert_cmpint(status, ==, OPUS_OK);
	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000)), ==,
			OPUS_OK);
	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)), ==,
			OPUS_OK);
	for (guint i = 0; i < G_N_ELEMENTS(pcm); i++)
		pcm[i] = i % 80 < 40 ? 4000 : -4000;
	int n = opus_encode(encoder, pcm, G_N_ELEMENTS(pcm), frame,
			    sizeof(frame));

	g_assert_cmpint(n, ==, 60);
	g_assert_cmphex(frame[0] & 0x83, ==, 0x80);
	opus_encoder_destroy(encoder);
	payload[0] = 0x10;
	payload[1] = frame[0] | 3;
	payload[2] = 2;
	payload[3] = 0xc0;
	memcpy(payload + 4, frame + 1, n - 1);
	memcpy(payload + 4 + n - 1, frame + 1, n - 1);
	return 4 + 2 * (n - 1);
}

static void group_reorder(void)
{
	guint8 key[30] = { 42 }, packet[2048], payload[256];
	g_autoptr(GError) e = NULL;
	LineAudio *a = line_audio_open_group(key, key, 98, 98, 202, 102, &e);

	g_assert_no_error(e);
	g_assert_nonnull(a);
	line_audio_enable_diagnostics(a);
	hold_playback = TRUE;
	queued_samples = 0;
	int len = group_fixture(payload);
	int n = protect_payload_at(packet, key, 98, 102, 100, 0, payload, len);

	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpuint(queued_samples, ==, 4800 + 1920);
	n = protect_payload_at(packet, key, 98, 102, 102, 3840, payload, len);
	gint64 before = g_get_monotonic_time();

	g_assert_true(line_audio_receive(a, packet, n));
	gint64 due = line_audio_playout(a, before + 41000);

	g_assert_cmpint(due, <, G_MAXINT64);
	g_assert_cmpint(due, >=, before + 80000);
	g_assert_cmpuint(queued_samples, ==, 4800 + 1920);
	n = protect_payload_at(packet, key, 98, 102, 101, 1920, payload, len);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpint(line_audio_playout(a, G_MAXINT64), ==, G_MAXINT64);
	/* A reordered burst keeps every packet, with a small drift correction. */
	g_assert_cmpuint(queued_samples, ==, 4800 + 3 * 1920 - 1920 / 50);
	GPrintFunc previous = g_set_printerr_handler(save_audio_report);

	line_audio_report(a);
	g_set_printerr_handler(previous);
	g_assert_nonnull(audio_report);
	g_assert_nonnull(strstr(audio_report, " wire=3 "));
	g_assert_nonnull(strstr(audio_report, " auth=3 "));
	g_assert_nonnull(strstr(audio_report, " p40=3 "));
	g_assert_nonnull(strstr(audio_report, " decoded=3 samples=5760 "));
	g_assert_nonnull(strstr(audio_report, " missed=0 concealed=0 "));
	g_clear_pointer(&audio_report, g_free);
	hold_playback = FALSE;
	line_audio_close(a);
}

static void group_xrtp(void)
{
	guint8 key[30] = { 83 }, packet[1024] = { 0 }, child[256] = { 0 };
	g_autoptr(GError) e = NULL;
	LineAudio *audio =
		line_audio_open_group(key, key, 98, 98, 202, 102, &e);
	srtp_policy_t policy = { 0 };
	srtp_t peer = NULL;

	g_assert_no_error(e);
	g_assert_nonnull(audio);
	line_audio_enable_diagnostics(audio);
	child[0] = 0x80;
	child[1] = 98;
	child[11] = 80;
	guint8 fixture[256];
	int size = group_fixture(fixture);
	guint32 config = 1u << (fixture[1] >> 3);
	g_autofree char *expected_toc =
		g_strdup_printf(" toc=0/0/0/2 configs=%08x ", config);

	g_assert_cmpint(size + 12, <, (int)sizeof(child));
	memcpy(child + 12, fixture, size);
	size += 12;
	packet[0] = 0x90;
	packet[1] = 98;
	packet[11] = 102;
	/* PLANET extension 3 carries the native varint length of each child. */
	const guint8 header[] = { 2,
				  0x40,
				  0,
				  2,
				  3,
				  2,
				  0x40 | (size >> 8),
				  size & 255,
				  0x40 | (size >> 8),
				  size & 255,
				  0,
				  0 };

	memcpy(packet + 12, header, sizeof(header));
	memcpy(packet + 24, child, size);
	child[11] = 81;
	memcpy(packet + 24 + size, child, size);
	int n = 24 + 2 * size;

	srtp_crypto_policy_set_rtp_default(&policy.rtp);
	srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
	policy.key = key;
	policy.ssrc.type = ssrc_any_outbound;
	g_assert_cmpint(srtp_create(&peer, &policy), ==, srtp_err_status_ok);
	g_assert_cmpint(srtp_protect(peer, packet, &n), ==, srtp_err_status_ok);
	hold_playback = TRUE;
	queued_samples = 0;
	g_assert_true(line_audio_receive(audio, packet, n));
	g_assert_cmpuint(queued_samples, ==, 4800 + G_N_ELEMENTS(played_40ms));
	hold_playback = FALSE;
	opus_int16 mono[1920];
	OpusDecoder *decoder = opus_decoder_create(48000, 1, NULL);
	/* Remove the native prefix and CELT bitmap to obtain stock Opus. */
	fixture[3] = fixture[2];
	fixture[2] = fixture[1];
	g_assert_cmpint(opus_decode(decoder, fixture + 2, size - 14, mono,
				    G_N_ELEMENTS(mono), 0),
			==, G_N_ELEMENTS(mono));
	for (guint i = 0; i < G_N_ELEMENTS(mono); i++)
		g_assert_cmpint(played_40ms[i], ==,
				CLAMP(2 * (int)mono[i], G_MININT16,
				      G_MAXINT16));
	opus_decoder_destroy(decoder);
	GPrintFunc previous = g_set_printerr_handler(save_audio_report);

	line_audio_report(audio);
	g_set_printerr_handler(previous);
	g_assert_nonnull(audio_report);
	g_assert_nonnull(
		strstr(audio_report, " pld=1 xrtp=1 xrtp-parts=2 xrtp-p40=2 "));
	g_assert_nonnull(strstr(audio_report, expected_toc));
	g_assert_nonnull(strstr(audio_report, " celt=2 "));
	g_clear_pointer(&audio_report, g_free);
	/* The child clock advances by 40 ms despite a 20 ms outer step. */
	packet[3] = 1;
	guint32 stamp = htonl(960);

	memcpy(packet + 4, &stamp, sizeof(stamp));
	stamp = htonl(1920);
	memcpy(child + 4, &stamp, sizeof(stamp));
	packet[17] = 1;
	packet[20] = packet[21] = 0;
	memcpy(packet + 24, child, size);
	n = 24 + size;
	g_assert_cmpint(srtp_protect(peer, packet, &n), ==, srtp_err_status_ok);
	g_assert_true(line_audio_receive(audio, packet, n));
	previous = g_set_printerr_handler(save_audio_report);
	line_audio_report(audio);
	g_set_printerr_handler(previous);
	g_assert_nonnull(audio_report);
	g_assert_nonnull(strstr(audio_report, " decoded=1 samples=1920 "));
	g_assert_nonnull(strstr(audio_report, " missed=0 concealed=0 "));
	g_clear_pointer(&audio_report, g_free);

	/* A signed outer packet with unaccounted trailing data is malformed. */
	packet[3] = 2;
	memcpy(packet + 24, child, size);
	n = 24 + size;
	packet[n++] = 0;
	g_assert_cmpint(srtp_protect(peer, packet, &n), ==, srtp_err_status_ok);
	g_assert_false(line_audio_receive(audio, packet, n));
	srtp_dealloc(peer);
	line_audio_close(audio);
}

static void group_timeline(void)
{
	guint8 key[30] = { 43 }, packet[2048], payload[256];
	g_autoptr(GError) e = NULL;
	LineAudio *a = line_audio_open_group(key, key, 98, 98, 202, 102, &e);

	g_assert_no_error(e);
	g_assert_nonnull(a);
	hold_playback = TRUE;
	queued_samples = 0;
	int len = group_fixture(payload);
	int n = protect_payload_at(packet, key, 98, 102, 10, 0, payload, len);

	g_assert_true(line_audio_receive(a, packet, n));
	guint before = queued_samples, short_frames = frames_20ms;

	/* A redundant packet can have a new sequence number but the same time. */
	n = protect_payload_at(packet, key, 98, 102, 11, 0, payload, len);
	g_assert_false(line_audio_receive(a, packet, n));
	g_assert_cmpuint(queued_samples, ==, before);

	/* The timing gap still exists with a contiguous sequence and marker. */
	n = protect_payload_at(packet, key, 98 | 0x80, 102, 12, 3840, payload,
			       len);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpuint(frames_20ms - short_frames, ==, 2);
	g_assert_cmpuint(queued_samples - before, ==, 3840 - 1920 / 50);
	before = queued_samples;
	/* Only the unheard half of an overlapping packet reaches the speaker. */
	n = protect_payload_at(packet, key, 98, 102, 13, 4800, payload, len);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpuint(queued_samples - before, ==, 960 - 960 / 50);
	hold_playback = FALSE;
	line_audio_close(a);
}

static void group_sequence_gap(void)
{
	guint8 key[30] = { 44 }, packet[2048], payload[256];
	g_autoptr(GError) e = NULL;
	LineAudio *a = line_audio_open_group(key, key, 98, 98, 202, 102, &e);

	g_assert_no_error(e);
	g_assert_nonnull(a);
	hold_playback = TRUE;
	queued_samples = 0;
	int len = group_fixture(payload);
	int n = protect_payload_at(packet, key, 98, 102, 10, 0, payload, len);

	g_assert_true(line_audio_receive(a, packet, n));
	guint before = queued_samples;

	/* The skipped sequence number did not leave a hole in decoded audio. */
	n = protect_payload_at(packet, key, 98, 102, 12, 1920, payload, len);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpint(line_audio_playout(a, 0), ==, G_MAXINT64);
	g_assert_cmpuint(queued_samples - before, ==, 1920);
	hold_playback = FALSE;
	line_audio_close(a);
}

static int group_pair(guint8 payload[256], const guint8 *first,
		      const guint8 *second, int frame_len, gboolean mixed)
{
	int pos = 0;

	payload[pos++] = 0x10;
	payload[pos++] = first[0] | (mixed ? 2 : 3);
	if (mixed)
		payload[pos++] = second[0] | 1;
	payload[pos++] = 2;
	payload[pos++] = 0xc0;
	memcpy(payload + pos, first + 1, frame_len - 1);
	pos += frame_len - 1;
	memcpy(payload + pos, second + 1, frame_len - 1);
	return pos + frame_len - 1;
}

static void group_overlap(gconstpointer data)
{
	gboolean mixed = GPOINTER_TO_INT(data);
	guint8 key[30] = { 45 }, packet[2048], payload[256], frames[3][120];
	opus_int16 pcm[960], expected[960];
	int rc;
	OpusEncoder *encoder = opus_encoder_create(
		48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &rc);
	g_assert_cmpint(rc, ==, OPUS_OK);
	OpusDecoder *decoder = opus_decoder_create(48000, 1, &rc);

	g_assert_cmpint(rc, ==, OPUS_OK);
	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000)), ==,
			OPUS_OK);
	g_assert_cmpint(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)), ==,
			OPUS_OK);
	const int frame_len = 80;

	for (guint f = 0; f < G_N_ELEMENTS(frames); f++) {
		int bw = mixed && f == 1 ? OPUS_BANDWIDTH_WIDEBAND :
					   OPUS_BANDWIDTH_FULLBAND;

		g_assert_cmpint(opus_encoder_ctl(encoder,
						 OPUS_SET_BANDWIDTH(bw)),
				==, OPUS_OK);
		for (guint i = 0; i < G_N_ELEMENTS(pcm); i++) {
			guint period = 56 + 19 * f;

			pcm[i] = i % period < period / 2 ? 7000 : -7000;
		}
		g_assert_cmpint(opus_encode(encoder, pcm, G_N_ELEMENTS(pcm),
					    frames[f], sizeof(frames[f])),
				==, frame_len);
		g_assert_cmpint(opus_decode(decoder, frames[f], frame_len,
					    expected, G_N_ELEMENTS(expected),
					    0),
				==, 960);
	}
	opus_decoder_destroy(decoder);
	opus_encoder_destroy(encoder);

	g_autoptr(GError) e = NULL;
	LineAudio *a = line_audio_open_group(key, key, 98, 98, 202, 102, &e);

	g_assert_no_error(e);
	g_assert_nonnull(a);
	hold_playback = TRUE;
	queued_samples = 0;
	int len = group_pair(payload, frames[0], frames[1], frame_len, mixed);
	int n = protect_payload_at(packet, key, 98, 102, 10, 0, payload, len);

	g_assert_true(line_audio_receive(a, packet, n));
	guint before = queued_samples;

	len = group_pair(payload, frames[1], frames[2], frame_len, mixed);
	n = protect_payload_at(packet, key, 98, 102, 11, 960, payload, len);
	g_assert_true(line_audio_receive(a, packet, n));
	g_assert_cmpuint(queued_samples - before, ==, 960);
	g_assert_cmpmem(played_20ms, sizeof(played_20ms), expected,
			sizeof(expected));
	hold_playback = FALSE;
	line_audio_close(a);
}

int main(int argc, char **argv)
{
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/audio/device-codec-srtp-loopback", loopback);
	g_test_add_func("/audio/negotiated-inbound-stream", incoming_stream);
	g_test_add_func("/audio/reorder-and-loss", reorder_and_loss);
	g_test_add_func("/audio/rtp-audio-timeline", rtp_audio_timeline);
	g_test_add_func("/audio/native-eas2-celt", eas2);
	g_test_add_func("/audio/conference-eas2-send-and-receive", group_send);
	g_test_add_data_func("/audio/conference-microphone", NULL,
			     group_microphone);
	g_test_add_data_func("/audio/conference-microphone-stuck-channel",
			     GINT_TO_POINTER(1), group_microphone);
	g_test_add_func("/audio/conference-microphone-clipping",
			group_microphone_clipping);
	g_test_add_func("/audio/conference-microphone-signals",
			group_microphone_signals);
	g_test_add_func("/audio/conference-absorbs-reordered-burst",
			group_reorder);
	g_test_add_func("/audio/conference-xrtp-mixes-tracks", group_xrtp);
	g_test_add_func("/audio/conference-uses-rtp-timeline", group_timeline);
	g_test_add_func("/audio/conference-contiguous-audio-sequence-gap",
			group_sequence_gap);
	g_test_add_data_func("/audio/conference-skips-played-eas2-frame", NULL,
			     group_overlap);
	g_test_add_data_func("/audio/conference-skips-played-mixed-frame",
			     GINT_TO_POINTER(1), group_overlap);
	return g_test_run();
}
