// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_audio.h"
#include "call_denoise.h"
#include <SDL.h>
#include <arpa/inet.h>
#include <opus/opus.h>
#include "call_srtp.h"
#include <openssl/rand.h>
#include <string.h>

enum {
	RATE = 48000,
	FRAME = 960,
	MIN_FRAME = 120,
	MAX_OPUS = 1275,
	MAX_RTP = 4096,
	MAX_FRAMES = 48,
	MAX_PCM = 5760,
	REORDER_PACKETS = 8,
	REORDER_US = 40000,
	PLAYBACK_PREROLL = FRAME * 3,
	PLAYBACK_LIMIT = FRAME * 12
};

struct mixed_group {
	guint8 toc[MAX_FRAMES];
	guint16 sizes[MAX_FRAMES];
	int count, offset;
};

struct audio_packet {
	guint8 *data;
	guint32 stamp;
	guint16 seq;
	int len, samples;
	gboolean marker;
	struct mixed_group group;
};

struct LineAudio {
	SDL_AudioDeviceID capture, playback;
	OpusEncoder *encoder;
	OpusDecoder *decoder;
	srtp_t tx, rx;
	guint32 tx_ssrc, rx_ssrc, timestamp, next_stamp;
	guint16 seq, received;
	guint8 pt;
	int last_samples;
	struct audio_packet pending[REORDER_PACKETS];
	guint pending_count;
	gint64 reorder_until;
	gboolean first_rx, first_tx, muted, sdl;
	struct line_denoise *denoise;
};

static void clear_pending(LineAudio *a)
{
	for (guint i = 0; i < a->pending_count; i++)
		g_free(a->pending[i].data);
	a->pending_count = 0;
}

void line_audio_close(LineAudio *a)
{
	if (!a)
		return;
	clear_pending(a);
	if (a->capture)
		SDL_CloseAudioDevice(a->capture);
	line_denoise_free(a->denoise);
	if (a->playback)
		SDL_CloseAudioDevice(a->playback);
	if (a->encoder)
		opus_encoder_destroy(a->encoder);
	if (a->decoder)
		opus_decoder_destroy(a->decoder);
	if (a->tx)
		srtp_dealloc(a->tx);
	if (a->rx)
		srtp_dealloc(a->rx);
	if (a->sdl)
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	g_free(a);
}

LineAudio *line_audio_open(const guint8 tx[30], const guint8 rx[30], guint8 pt,
			   guint32 tx_ssrc, guint32 rx_ssrc, GError **e)
{
	LineAudio *a = g_new0(LineAudio, 1);

	a->pt = pt;
	a->tx_ssrc = tx_ssrc;
	a->rx_ssrc = rx_ssrc;
	a->first_rx = a->first_tx = TRUE;
	int rc;

	if (pt > 127 || !tx || !rx || !tx_ssrc || !rx_ssrc ||
	    !call_srtp_open(&a->tx, tx, ssrc_specific, tx_ssrc) ||
	    !call_srtp_open(&a->rx, rx, ssrc_specific, rx_ssrc) ||
	    RAND_bytes((guint8 *)&a->seq, sizeof(a->seq)) != 1 ||
	    RAND_bytes((guint8 *)&a->timestamp, sizeof(a->timestamp)) != 1) {
		g_set_error_literal(
			e, LINE_ERROR, 1,
			"Could not initialize encrypted call audio.");
		goto fail;
	}
	a->encoder = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &rc);
	a->decoder = opus_decoder_create(RATE, 1, &rc);
	if (!a->encoder || !a->decoder ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_BITRATE(32000)) != OPUS_OK ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) !=
		    OPUS_OK ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_INBAND_FEC(1)) != OPUS_OK ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_PACKET_LOSS_PERC(5)) !=
		    OPUS_OK) {
		g_set_error_literal(e, LINE_ERROR, 1,
				    "Could not open the Opus call codec.");
		goto fail;
	}
	a->sdl = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
	SDL_AudioSpec wanted = { .freq = RATE,
				 .format = AUDIO_S16SYS,
				 .channels = 1,
				 .samples = 512 };
	if (a->sdl) {
		a->playback = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
		if (a->playback)
			a->capture =
				SDL_OpenAudioDevice(NULL, 1, &wanted, NULL, 0);
	}
	if (!a->sdl || !a->playback || !a->capture) {
		g_set_error(e, LINE_ERROR, 1,
			    "Could not open the microphone and speaker: %s.",
			    SDL_GetError());
		goto fail;
	}
	a->denoise = line_denoise_new();
	SDL_PauseAudioDevice(a->playback, 0);
	SDL_PauseAudioDevice(a->capture, 0);
	return a;
fail:
	line_audio_close(a);
	return NULL;
}

void line_audio_mute(LineAudio *a, gboolean muted)
{
	if (!a || a->muted == muted)
		return;
	SDL_PauseAudioDevice(a->capture, 1);
	SDL_ClearQueuedAudio(a->capture);
	a->muted = muted;
	if (!muted) {
		a->first_tx = TRUE;
		SDL_PauseAudioDevice(a->capture, 0);
	}
}

int line_audio_capture(LineAudio *a, guint8 *packet, gsize capacity, GError **e)
{
	if (!a || !packet || capacity < 13 + MAX_OPUS + SRTP_MAX_TRAILER_LEN) {
		g_set_error_literal(e, LINE_ERROR, 1,
				    "Invalid call audio buffer.");
		return -1;
	}
	if (a->muted)
		return 0;
	opus_int16 pcm[FRAME];
	Uint32 available = SDL_GetQueuedAudioSize(a->capture);

	if (available > sizeof(pcm) * 50) {
		SDL_ClearQueuedAudio(a->capture);
		a->timestamp += available / sizeof(*pcm);
		a->first_tx = TRUE;
		return 0;
	}
	if (available > sizeof(pcm) * 6) {
		/* Keep recent microphone audio after a scheduling stall. */
		while (available >= sizeof(pcm) * 3) {
			if (SDL_DequeueAudio(a->capture, pcm, sizeof(pcm)) !=
			    sizeof(pcm))
				return 0;
			a->timestamp += FRAME;
			available -= sizeof(pcm);
		}
		a->first_tx = TRUE;
	}
	if (available < sizeof(pcm) ||
	    SDL_DequeueAudio(a->capture, pcm, sizeof(pcm)) != sizeof(pcm))
		return 0;
	line_denoise_frame(a->denoise, pcm, FRAME);
	/* Prefix used by LINE's one-to-one Opus packetizer. RTP uses a 48 kHz clock. */
	packet[0] = 0x80;
	packet[1] = a->pt | (a->first_tx ? 0x80 : 0);
	guint16 seq = htons(a->seq++);
	guint32 stamp = htonl(a->timestamp), ssrc = htonl(a->tx_ssrc);

	memcpy(packet + 2, &seq, 2);
	memcpy(packet + 4, &stamp, 4);
	memcpy(packet + 8, &ssrc, 4);
	packet[12] = 0;
	a->timestamp += FRAME;
	a->first_tx = FALSE;
	int n = opus_encode(a->encoder, pcm, FRAME, packet + 13, MAX_OPUS);

	if (n > 0) {
		n += 13;
		if (srtp_protect(a->tx, packet, &n) == srtp_err_status_ok)
			return n;
	}
	g_set_error_literal(e, LINE_ERROR, 1,
			    "Could not encode encrypted call audio.");
	return -1;
}

/*
 * PLANET normal-audio is EAS2. Its single-frame TOC uses bit 0 for
 * activity. A same-TOC group of CELT frames carries its activity bitmap
 * after the count byte, before the Opus sizes and frame data.
 */
static int unpack_audio(guint8 **data, int size)
{
	guint8 *payload = *data;

	if (size < 1)
		return -1;
	unsigned int code = payload[0] & 3;

	if (code < 2) {
		payload[0] &= 0xfe;
		return size;
	}
	if (size < 2)
		return -1;
	guint8 count_byte = payload[1];
	int count = count_byte & 63;

	if ((count_byte & 0x40) || !count || count > MAX_FRAMES)
		return -1;
	if (payload[0] & 0x80) {
		int activity = (count + 7) / 8;

		if (size <= 2 + activity)
			return -1;
		/* Move the two header bytes instead of shifting every frame. */
		payload[activity] = payload[0];
		payload[activity + 1] = count_byte;
		*data += activity;
		size -= activity;
	}
	return size;
}

/*
 * EAS2 code 2 lists individual TOCs. The low bit of each subsequent TOC
 * marks the last explicit TOC. That TOC applies to any remaining
 * frames described by the count byte.
 */
static int unpack_mixed(const guint8 *data, int size, struct mixed_group *group)
{
	int pos = 1, headers = 1;
	guint8 byte;

	group->toc[0] = data[0] & 0xfc;
	do {
		if (pos >= size || headers == MAX_FRAMES)
			return 0;
		byte = data[pos++];
		group->toc[headers++] = byte & 0xfc;
	} while (!(byte & 1));
	if (pos >= size)
		return 0;
	byte = data[pos++];
	group->count = byte & 63;
	if (byte & 0x40 || group->count < headers || group->count > MAX_FRAMES)
		return 0;
	int celt = 0, samples = 0;

	for (int i = 0; i < group->count; i++) {
		if (i >= headers)
			group->toc[i] = group->toc[headers - 1];
		celt += !!(group->toc[i] & 0x80);
		samples +=
			opus_packet_get_samples_per_frame(&group->toc[i], RATE);
	}
	pos += (celt + 7) / 8;
	if (pos > size || samples > MAX_PCM)
		return 0;
	if (byte & 0x80) {
		int left = size - pos;

		for (int i = 0; i < group->count - 1; i++) {
			if (left <= 0)
				return 0;
			int len = data[pos++];

			left--;
			if (len >= 252) {
				if (left <= 0)
					return 0;
				len += data[pos++] * 4;
				left--;
			}
			if (len > left)
				return 0;
			group->sizes[i] = len;
			left -= len;
		}
		if (left > MAX_OPUS)
			return 0;
		group->sizes[group->count - 1] = left;
	} else {
		int left = size - pos, len = left / group->count;

		if (left % group->count || len > MAX_OPUS)
			return 0;
		for (int i = 0; i < group->count; i++)
			group->sizes[i] = len;
	}
	group->offset = pos;
	return samples;
}

static int decode_mixed(OpusDecoder *decoder, const struct audio_packet *packet,
			opus_int16 pcm[MAX_PCM])
{
	guint8 frame[1 + MAX_OPUS];
	const struct mixed_group *group = &packet->group;
	const guint8 *payload = packet->data + group->offset;
	int written = 0;

	for (int i = 0; i < group->count; i++) {
		guint16 len = group->sizes[i];

		frame[0] = group->toc[i];
		memcpy(frame + 1, payload, len);
		int n = opus_decode(decoder, frame, len + 1, pcm + written,
				    MAX_PCM - written, 0);

		if (n <= 0)
			return -1;
		written += n;
		payload += len;
	}
	return written;
}

static gboolean queue_pcm(LineAudio *a, const opus_int16 *pcm, int samples)
{
	Uint32 queued = SDL_GetQueuedAudioSize(a->playback) / sizeof(*pcm);

	/* Start with a short playout cushion and bound long-term queue drift. */
	if (!queued) {
		opus_int16 silence[PLAYBACK_PREROLL] = { 0 };

		if (SDL_QueueAudio(a->playback, silence, sizeof(silence)) < 0)
			return FALSE;
	}
	if (queued >= PLAYBACK_LIMIT)
		return TRUE;
	return SDL_QueueAudio(a->playback, pcm, samples * sizeof(*pcm)) == 0;
}

static void conceal_loss(LineAudio *a, const struct audio_packet *next)
{
	guint gap = (guint16)(next->seq - a->received - 1);

	if (!gap || a->first_rx)
		return;
	gint32 ticks = (gint32)(next->stamp - a->next_stamp);

	/* A lost sequence number does not imply a gap on the audio clock. */
	if (ticks <= 0)
		return;
	if (ticks > MAX_PCM || ticks % MIN_FRAME || next->marker) {
		opus_decoder_ctl(a->decoder, OPUS_RESET_STATE);
		return;
	}
	int remaining = ticks;
	opus_int16 pcm[MAX_PCM];

	while (remaining) {
		int length = MIN(remaining, a->last_samples);
		/* The next packet may carry FEC for the immediately preceding frame. */
		gboolean fec = !next->group.count && remaining == length &&
			       next->samples == length;
		int count = opus_decode(a->decoder, fec ? next->data : NULL,
					fec ? next->len : 0, pcm, length, fec);

		if (count <= 0)
			break;
		queue_pcm(a, pcm, count);
		remaining -= count;
	}
}

static gboolean play_packet(LineAudio *a, const struct audio_packet *packet)
{
	opus_int16 pcm[MAX_PCM];
	int count;

	conceal_loss(a, packet);
	if (packet->group.count) {
		count = decode_mixed(a->decoder, packet, pcm);
	} else {
		count = opus_decode(a->decoder, packet->data, packet->len, pcm,
				    G_N_ELEMENTS(pcm), 0);
	}
	if (count <= 0)
		return FALSE;
	a->first_rx = FALSE;
	a->received = packet->seq;
	a->next_stamp = packet->stamp + count;
	a->last_samples = count;
	return queue_pcm(a, pcm, count);
}

gint64 line_audio_playout(LineAudio *a, gint64 now)
{
	while (a->pending_count) {
		struct audio_packet first = a->pending[0];

		if ((guint16)(first.seq - a->received) > 1 &&
		    a->pending_count < REORDER_PACKETS &&
		    now < a->reorder_until)
			return a->reorder_until;
		play_packet(a, &first);
		g_free(first.data);
		a->pending_count--;
		memmove(a->pending, a->pending + 1,
			a->pending_count * sizeof(*a->pending));
	}
	return G_MAXINT64;
}

gboolean line_audio_receive(LineAudio *a, const guint8 *packet, gsize size)
{
	if (!a || !packet || size < 23 || size > MAX_RTP ||
	    (packet[0] >> 6) != 2 || (packet[1] & 127) != a->pt)
		return FALSE;
	guint32 ssrc;

	memcpy(&ssrc, packet + 8, sizeof(ssrc));
	if (ntohl(ssrc) != a->rx_ssrc)
		return FALSE;
	guint8 plain[MAX_RTP];

	memcpy(plain, packet, size);
	int n = size;

	if (srtp_unprotect(a->rx, plain, &n) != srtp_err_status_ok)
		return FALSE;
	int off = 12 + 4 * (plain[0] & 15);

	if (plain[0] & 0x10) {
		if (off + 4 > n)
			return FALSE;
		off += 4 + 4 * ((plain[off + 2] << 8) | plain[off + 3]);
	}
	if (plain[0] & 0x20) {
		if (plain[n - 1] == 0 || plain[n - 1] > n - off)
			return FALSE;
		n -= plain[n - 1];
	}
	if (off + 1 >= n)
		return FALSE;
	guint16 seq = ((guint16)plain[2] << 8) | plain[3];
	gint16 delta = (gint16)(seq - a->received);

	if (!a->first_rx && delta <= 0)
		return FALSE;
	struct audio_packet frame = {
		.data = plain + off + 1,
		.stamp = ((guint32)plain[4] << 24) | ((guint32)plain[5] << 16) |
			 ((guint32)plain[6] << 8) | plain[7],
		.seq = seq,
		.len = n - off - 1,
		.marker = (plain[1] & 0x80) != 0,
	};

	if ((frame.data[0] & 3) == 2) {
		frame.samples =
			unpack_mixed(frame.data, frame.len, &frame.group);
	} else {
		opus_int16 frame_lengths[MAX_FRAMES];

		frame.len = unpack_audio(&frame.data, frame.len);
		if (frame.len < 0)
			return FALSE;
		int count = opus_packet_parse(frame.data, frame.len, NULL, NULL,
					      frame_lengths, NULL);

		if (count <= 0)
			return FALSE;
		frame.samples = count * opus_packet_get_samples_per_frame(
						frame.data, RATE);
	}
	if (frame.samples <= 0 || frame.samples > MAX_PCM)
		return FALSE;

	if (a->first_rx || delta == 1 || delta > REORDER_PACKETS + 1) {
		if (!a->first_rx && delta > REORDER_PACKETS + 1)
			line_audio_playout(a, G_MAXINT64);
		gboolean played = play_packet(a, &frame);

		if (a->pending_count)
			line_audio_playout(a, g_get_monotonic_time());
		return played;
	}
	gint64 now = g_get_monotonic_time();

	if (!a->pending_count)
		a->reorder_until = now + REORDER_US;
	guint pos = a->pending_count++;

	while (pos && (gint16)(seq - a->pending[pos - 1].seq) < 0) {
		a->pending[pos] = a->pending[pos - 1];
		pos--;
	}
	frame.data = g_memdup2(frame.data, frame.len);
	a->pending[pos] = frame;
	line_audio_playout(a, now);
	return TRUE;
}
