// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_audio.h"
#include <SDL.h>
#include <arpa/inet.h>
#include <opus/opus.h>
#include <srtp2/srtp.h>
#include <string.h>

static guint frames_20ms, frames_40ms;

int __real_SDL_QueueAudio(SDL_AudioDeviceID device, const void *pcm,
			  Uint32 len);

int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void *pcm, Uint32 len)
{
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

int main(int argc, char **argv)
{
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/audio/device-codec-srtp-loopback", loopback);
	g_test_add_func("/audio/negotiated-inbound-stream", incoming_stream);
	g_test_add_func("/audio/reorder-and-loss", reorder_and_loss);
	g_test_add_func("/audio/rtp-audio-timeline", rtp_audio_timeline);
	g_test_add_func("/audio/native-eas2-celt", eas2);
	return g_test_run();
}
