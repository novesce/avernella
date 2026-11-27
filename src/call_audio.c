// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_audio.h"
#include "call_denoise.h"
#include <SDL.h>
#include <arpa/inet.h>
#include <opus/opus.h>
#include <math.h>
#include "call_srtp.h"
#include <openssl/rand.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* Available since libopus 1.5. Older runtimes can still decode FEC. */
#pragma weak opus_packet_has_lbrr
extern int opus_packet_has_lbrr(const unsigned char *packet, opus_int32 len);

enum {
	RATE = 48000,
	FRAME = 960,
	GROUP_FRAME = 2 * FRAME,
	GROUP_HEADER = 24,
	GROUP_OPUS = 384,
	CAPTURE_GAIN_ONE = 256,
	CAPTURE_GAIN_MAX = 4 * CAPTURE_GAIN_ONE,
	CAPTURE_GAIN_SILENCE = 128,
	CAPTURE_TARGET_PEAK = 6144,
	MIN_FRAME = 120,
	MAX_OPUS = 1275,
	MAX_RTP = 4096,
	MAX_FRAMES = 48,
	MAX_PCM = 5760,
	XRTP_PARTS = 16,
	XRTP_TRACKS = 16,
	REORDER_PACKETS = 8,
	REORDER_US = 40000,
	PLAYBACK_PREROLL = FRAME * 3,
	PLAYBACK_LIMIT = FRAME * 12,
	GROUP_REORDER_US = 80000,
	GROUP_OUTPUT_GUARD_US = 20000,
	GROUP_PLAYBACK_PREROLL = FRAME * 5,
	GROUP_PLAYBACK_LIMIT = FRAME * 18,
	GROUP_CONCEAL_LIMIT = 2 * GROUP_FRAME,
	/* One-to-one playout: an adaptive jitter buffer (see jb_playout()). */
	JB_PACKETS = 50,
	/* Two observations per one-percent tail bin. */
	TRANSIT_WINDOW = 200,
	/* PipeWire can drain SDL's queue 1024 samples at a time. */
	JB_DEVICE_LOW = FRAME * 2, /* 40 ms waiting at the speaker */
	JB_DECIDE = 1024, /* 21 ms: a missing packet waits until then */
	JB_MIN_MS = 40,
	JB_MAX_MS = 500,
	JB_EXPAND_LIMIT = 10, /* frames of concealment with nothing buffered */
	JB_JUMP = 5, /* a longer gap is skipped rather than concealed */
};

struct mixed_group {
	guint8 toc[MAX_FRAMES];
	guint16 sizes[MAX_FRAMES];
	int count, offset;
};

struct xrtp_layout {
	guint16 sizes[XRTP_PARTS];
	guint count;
};

struct xrtp_track {
	OpusDecoder *decoder;
	guint64 packet;
	guint32 ssrc, start_stamp, next_stamp;
	int frame_samples;
	guint predicted;
	gboolean played;
};

struct audio_packet {
	guint8 *data;
	guint32 stamp;
	guint16 seq;
	int len, samples, frame_samples;
	gint64 reorder_until, arrival;
	gboolean marker;
	struct mixed_group group;
	struct xrtp_layout xrtp;
};

/*
 * Absolute one-way delay is unknowable without synchronised clocks.
 * Rank local arrival minus unwrapped sender time in a sliding window.
 */
struct transit_window {
	gint64 fifo[TRANSIT_WINDOW], sorted[TRANSIT_WINDOW];
	gint64 ticks;
	guint32 stamp;
	guint count, slot, margin_ms;
	double mean_us, variance;
};

struct jitter_buffer {
	struct audio_packet packet[JB_PACKETS];
	guint count, expanded;
	gint64 excess_since;
	guint32 next_stamp;
	guint16 next_seq;
	gboolean have_seq, playing, restart;
};

struct audio_interval {
	guint wire, other_ssrc, bytes, auth, rejected, replay, invalid, late;
	guint packets_20ms, packets_40ms, packets_other, mixed, celt, csrc;
	guint payload_bytes, payload_max, reordered, arrival_max_us;
	guint silence_headers, chunk_changes;
	guint tocs[4], pld, xrtp, xrtp_parts, xrtp_p40;
	guint vsd_frames[3], tx_vsd[3];
	guint32 toc_configs, tx_configs;
	guint decoded_packets, decoded_samples, decoded_errors;
	guint missed_ticks, conceal_samples, decoder_resets;
	guint queued_samples, queue_max, queue_drops, queue_prerolls, trimmed;
	guint capture_checks, capture_blocks, capture_power_max;
	guint tx_frames, tx_power_max;
	guint64 pcm_square_sum, tx_power_sum;
};

struct audio_diagnostics {
	struct audio_interval interval;
	gint64 report_at, last_arrival;
	guint32 last_stamp, jitter_16us;
	guint16 last_seq;
	guint8 last_chunk;
	gboolean have_chunk;
};

/* Four slots cover one scheduling stall of up to 80 ms (160 ms for EAS2).
 * SDL is the only producer and the call thread is the only consumer. A slot
 * is never overwritten before the consumer releases it.
 */
enum {
	MIC_SLOTS = 4
};
struct mic_ring {
	_Alignas(64) atomic_uint head;
	atomic_uint produced;
	guint offset;
	gboolean filling;

	_Alignas(64) atomic_uint tail;
	guint consumed;
	int wake;
	struct {
		guint stamp;
		opus_int16 pcm[2 * GROUP_FRAME];
	} slot[MIC_SLOTS];
};

struct LineAudio {
	enum line_media_reception reception;
	SDL_AudioDeviceID capture, playback;
	struct mic_ring *mic;
	OpusEncoder *encoder;
	OpusDecoder *decoder;
	srtp_t tx, rx;
	guint32 tx_ssrc, rx_ssrc, timestamp, next_stamp;
	guint16 seq, received;
	guint8 tx_pt, rx_pt, group_chunk;
	guint8 capture_channels, capture_channel, clean_hold;
	guint capture_gain;
	int last_samples;
	struct audio_packet pending[REORDER_PACKETS];
	guint pending_count;
	gint64 reorder_until;
	gboolean first_rx, first_tx, muted, sdl, group;
	gboolean group_active, last_xrtp;
	struct line_denoise *denoise;
	struct audio_diagnostics *diagnostics;
	struct line_audio_stats totals;
	struct call_reception_report rr;
	struct jitter_buffer *jb;
	struct transit_window transit;
	guint trim_credit, group_expanded;
	guint16 transit_seq;
	gboolean transit_xrtp;
	/* Kernel receive time of the packet being handled, or 0 for now. */
	gint64 rx_arrival;
	struct xrtp_track *tracks;
	guint64 xrtp_packet;
};

enum line_media_reception line_audio_reception(const LineAudio *a)
{
	return a->reception;
}

static guint target_ms(const LineAudio *a, int samples);

void line_audio_stats(const LineAudio *a, struct line_audio_stats *out)
{
	*out = a ? a->totals : (struct line_audio_stats){ 0 };
	if (a)
		out->target_ms = target_ms(a, a->group ? GROUP_FRAME :
					      a->last_samples ?
							 a->last_samples :
							 FRAME);
}

void line_audio_enable_diagnostics(LineAudio *a)
{
	if (!a || a->diagnostics)
		return;
	a->diagnostics = g_new0(struct audio_diagnostics, 1);
	a->diagnostics->report_at = g_get_monotonic_time();
}

void line_audio_report(LineAudio *a)
{
	if (!a || !a->diagnostics)
		return;
	struct audio_diagnostics *d = a->diagnostics;
	struct audio_interval *s = &d->interval;
	gint64 now = g_get_monotonic_time();

	if (s->wire || s->decoded_samples || s->conceal_samples ||
	    s->capture_checks || s->tx_frames)
		g_printerr(
			"group-stats audio ms=%" G_GINT64_FORMAT
			" wire=%u bytes=%u other-ssrc=%u auth=%u reject=%u replay=%u invalid=%u late=%u"
			" p20=%u p40=%u other-ptime=%u mixed=%u celt=%u csrc=%u payload-avg=%u payload-max=%u"
			" silence=%u chunk-changes=%u"
			" toc=%u/%u/%u/%u configs=%08x vsd=%u/%u/%u pld=%u xrtp=%u xrtp-parts=%u xrtp-p40=%u"
			" reordered=%u arrival-max-us=%u jitter-us=%u"
			" decoded=%u samples=%u decode-error=%u power=%" G_GUINT64_FORMAT
			" missed=%u concealed=%u reset=%u queued=%u queue=%u queue-max=%u drops=%u preroll=%u trim=%u"
			" mic-checks=%u mic-blocks=%u mic-ac-power-max=%u"
			" tx-power=%" G_GUINT64_FORMAT
			" tx-power-max=%u tx-configs=%08x tx-vsd=%u/%u/%u\n",
			(now - d->report_at) / 1000, s->wire, s->bytes,
			s->other_ssrc, s->auth, s->rejected, s->replay,
			s->invalid, s->late, s->packets_20ms, s->packets_40ms,
			s->packets_other, s->mixed, s->celt, s->csrc,
			s->payload_bytes /
				MAX(1, s->packets_20ms + s->packets_40ms +
					       s->packets_other),
			s->payload_max, s->silence_headers, s->chunk_changes,
			s->tocs[0], s->tocs[1], s->tocs[2], s->tocs[3],
			s->toc_configs, s->vsd_frames[0], s->vsd_frames[1],
			s->vsd_frames[2], s->pld, s->xrtp, s->xrtp_parts,
			s->xrtp_p40, s->reordered, s->arrival_max_us,
			d->jitter_16us / 16, s->decoded_packets,
			s->decoded_samples, s->decoded_errors,
			s->pcm_square_sum / MAX(1, s->decoded_samples),
			s->missed_ticks, s->conceal_samples, s->decoder_resets,
			s->queued_samples,
			(guint)(SDL_GetQueuedAudioSize(a->playback) /
				sizeof(opus_int16)),
			s->queue_max, s->queue_drops, s->queue_prerolls,
			s->trimmed, s->capture_checks, s->capture_blocks,
			s->capture_power_max,
			s->tx_power_sum / MAX(1, s->tx_frames), s->tx_power_max,
			s->tx_configs, s->tx_vsd[0], s->tx_vsd[1],
			s->tx_vsd[2]);
	*s = (struct audio_interval){ 0 };
	d->report_at = now;
}

static void jb_free(struct jitter_buffer *jb)
{
	for (guint i = 0; jb && i < jb->count; i++)
		g_free(jb->packet[i].data);
	g_free(jb);
}

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
	g_clear_pointer(&a->jb, jb_free);
	if (a->capture)
		SDL_CloseAudioDevice(a->capture);
	if (a->mic) {
		close(a->mic->wake);
		explicit_bzero(a->mic, sizeof(*a->mic));
		free(a->mic);
	}
	line_denoise_free(a->denoise);
	if (a->playback)
		SDL_CloseAudioDevice(a->playback);
	if (a->encoder)
		opus_encoder_destroy(a->encoder);
	if (a->decoder)
		opus_decoder_destroy(a->decoder);
	for (guint i = 0; a->tracks && i < XRTP_TRACKS; i++) {
		if (a->tracks[i].decoder)
			opus_decoder_destroy(a->tracks[i].decoder);
	}
	g_free(a->tracks);
	if (a->tx)
		srtp_dealloc(a->tx);
	if (a->rx)
		srtp_dealloc(a->rx);
	if (a->sdl)
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	g_free(a->diagnostics);
	g_free(a);
}

static void microphone_input(void *data, Uint8 *samples, int length)
{
	LineAudio *a = data;
	struct mic_ring *r = a->mic;
	guint frames = a->group ? GROUP_FRAME : FRAME;
	guint bytes = frames * a->capture_channels * sizeof(opus_int16);
	guint head = atomic_load_explicit(&r->head, memory_order_relaxed);

	guint produced =
		atomic_load_explicit(&r->produced, memory_order_relaxed);
	while (length > 0) {
		guint amount = MIN((guint)length, bytes - r->offset);
		guint index = head % MIC_SLOTS;

		if (!r->offset)
			r->filling =
				head - atomic_load_explicit(
					       &r->tail, memory_order_acquire) <
				MIC_SLOTS;
		if (r->filling)
			memcpy((guint8 *)r->slot[index].pcm + r->offset,
			       samples, amount);
		samples += amount;
		length -= amount;
		r->offset += amount;
		if (r->offset != bytes)
			continue;
		/* Publish capture time even when a full ring discarded this slot. */
		produced += frames;
		atomic_store_explicit(&r->produced, produced,
				      memory_order_release);
		if (r->filling) {
			r->slot[index].stamp = produced - frames;
			atomic_store_explicit(&r->head, ++head,
					      memory_order_release);
			while (eventfd_write(r->wake, 1) < 0 && errno == EINTR)
				;
		}
		r->offset = 0;
	}
}

int line_audio_capture_fd(const LineAudio *a)
{
	return a && a->mic ? a->mic->wake : -1;
}

gboolean line_audio_capture_ready(const LineAudio *a)
{
	const struct mic_ring *r = a ? a->mic : NULL;

	return r &&
	       atomic_load_explicit(&r->head, memory_order_acquire) !=
		       atomic_load_explicit(&r->tail, memory_order_relaxed);
}

static gboolean take_microphone(LineAudio *a, opus_int16 *pcm, guint bytes,
				guint samples)
{
	struct mic_ring *r = a->mic;
	eventfd_t count;

	(void)eventfd_read(r->wake, &count);
	guint tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	guint head = atomic_load_explicit(&r->head, memory_order_acquire);

	if (tail == head)
		return FALSE;
	/* After a stalled consumer, resume with the most recent full frame. */
	if (head - tail == MIC_SLOTS) {
		tail = head - 1;
		guint end = r->slot[tail % MIC_SLOTS].stamp + samples;
		guint produced = atomic_load_explicit(&r->produced,
						      memory_order_acquire);

		/* Even the newest retained slot predates a completed, dropped
		 * frame. Discard backlog and wait for the next fresh capture.
		 */
		if (produced != end) {
			atomic_store_explicit(&r->tail, head,
					      memory_order_release);
			return FALSE;
		}
	}
	guint i = tail % MIC_SLOTS;
	guint skipped = r->slot[i].stamp - r->consumed;

	memcpy(pcm, r->slot[i].pcm, bytes);
	r->consumed = r->slot[i].stamp + samples;
	atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
	a->timestamp += skipped;
	if (skipped)
		a->first_tx = TRUE;
	return TRUE;
}

static LineAudio *open_audio(const guint8 tx[30], const guint8 rx[30],
			     guint8 tx_pt, guint8 rx_pt, guint32 tx_ssrc,
			     guint32 rx_ssrc, gboolean group, GError **e)
{
	LineAudio *a = g_new0(LineAudio, 1);

	a->tx_pt = tx_pt;
	a->rx_pt = rx_pt;
	a->group = group;
	a->tx_ssrc = tx_ssrc;
	a->rx_ssrc = rx_ssrc;
	a->first_rx = a->first_tx = TRUE;
	a->capture_gain = CAPTURE_GAIN_ONE;
	a->totals.expected_loss_pct = 5;
	a->totals.can_count_lbrr = opus_packet_has_lbrr != NULL;
	/* EAS2 timestamps each pair at the end of its first 20 ms frame. */
	a->timestamp = FRAME;
	int rc;

	if (tx_pt > 127 || rx_pt > 127 || !tx || !rx || !tx_ssrc || !rx_ssrc ||
	    !call_srtp_open(&a->tx, tx, ssrc_specific, tx_ssrc) ||
	    !call_srtp_open(&a->rx, rx, ssrc_specific, rx_ssrc) ||
	    RAND_bytes((guint8 *)&a->seq, sizeof(a->seq)) != 1 ||
	    (!group &&
	     RAND_bytes((guint8 *)&a->timestamp, sizeof(a->timestamp)) != 1)) {
		g_set_error_literal(
			e, LINE_ERROR, 1,
			"Could not initialize encrypted call audio.");
		goto fail;
	}
	/*
	 * The EAS2 pair carries one Opus configuration for both frames. Fix
	 * the conference encoder's CELT mode and bandwidth so it cannot
	 * change that configuration halfway through a 40 ms packet.
	 */
	a->encoder = opus_encoder_create(
		RATE, 1,
		group ? OPUS_APPLICATION_RESTRICTED_LOWDELAY :
			OPUS_APPLICATION_VOIP,
		&rc);
	a->decoder = opus_decoder_create(RATE, 1, &rc);
	if (!a->encoder || !a->decoder ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_BITRATE(32000)) != OPUS_OK ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) !=
		    OPUS_OK ||
	    opus_encoder_ctl(a->encoder, OPUS_SET_INBAND_FEC(1)) != OPUS_OK ||
	    opus_encoder_ctl(a->encoder,
			     OPUS_SET_PACKET_LOSS_PERC(
				     a->totals.expected_loss_pct)) != OPUS_OK ||
	    (group &&
	     (opus_encoder_ctl(a->encoder, OPUS_SET_DTX(0)) != OPUS_OK ||
	      opus_encoder_ctl(a->encoder,
			       OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) !=
		      OPUS_OK))) {
		g_set_error_literal(e, LINE_ERROR, 1,
				    "Could not open the Opus call codec.");
		goto fail;
	}
	a->sdl = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
	SDL_AudioSpec wanted = { .freq = RATE,
				 .format = AUDIO_S16SYS,
				 .channels = 1,
				 .samples = FRAME / 2 };
	if (a->sdl) {
		a->playback = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
		if (a->playback &&
		    g_strcmp0(g_getenv("LINE_CALL_CAPTURE_EVENTS"), "0")) {
			int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

			if (fd >= 0)
				a->mic =
					aligned_alloc(_Alignof(struct mic_ring),
						      sizeof(*a->mic));
			if (a->mic) {
				memset(a->mic, 0, sizeof(*a->mic));
				a->mic->wake = fd;
				atomic_init(&a->mic->head, 0);
				atomic_init(&a->mic->tail, 0);
				atomic_init(&a->mic->produced, 0);
				wanted.callback = microphone_input;
				wanted.userdata = a;
			} else if (fd >= 0)
				close(fd);
		}
		if (a->playback && group) {
			wanted.channels = 2;
			a->capture =
				SDL_OpenAudioDevice(NULL, 1, &wanted, NULL, 0);
		}
		if (a->playback && !a->capture) {
			wanted.channels = 1;
			a->capture =
				SDL_OpenAudioDevice(NULL, 1, &wanted, NULL, 0);
		}
	}
	if (!a->sdl || !a->playback || !a->capture) {
		g_set_error(e, LINE_ERROR, 1,
			    "Could not open the microphone and speaker: %s.",
			    SDL_GetError());
		goto fail;
	}
	a->capture_channels = wanted.channels;
	a->denoise = line_denoise_new();
	if (!group)
		a->jb = g_new0(struct jitter_buffer, 1);
	SDL_PauseAudioDevice(a->playback, 0);
	SDL_PauseAudioDevice(a->capture, 0);
	return a;
fail:
	line_audio_close(a);
	return NULL;
}

LineAudio *line_audio_open(const guint8 tx[30], const guint8 rx[30], guint8 pt,
			   guint32 tx_ssrc, guint32 rx_ssrc, GError **e)
{
	return open_audio(tx, rx, pt, pt, tx_ssrc, rx_ssrc, FALSE, e);
}

LineAudio *line_audio_open_group(const guint8 tx[30], const guint8 rx[30],
				 guint8 tx_pt, guint8 rx_pt, guint32 tx_ssrc,
				 guint32 rx_ssrc, GError **e)
{
	return open_audio(tx, rx, tx_pt, rx_pt, tx_ssrc, rx_ssrc, TRUE, e);
}

void line_audio_mute(LineAudio *a, gboolean muted)
{
	if (!a || a->muted == muted)
		return;
	SDL_PauseAudioDevice(a->capture, 1);
	if (a->mic) {
		struct mic_ring *r = a->mic;
		eventfd_t count;

		SDL_LockAudioDevice(a->capture);
		r->offset = 0;
		r->consumed = atomic_load(&r->produced);
		explicit_bzero(r->slot, sizeof(r->slot));
		atomic_store(&r->tail, atomic_load(&r->head));
		(void)eventfd_read(r->wake, &count);
		SDL_UnlockAudioDevice(a->capture);
	} else {
		SDL_ClearQueuedAudio(a->capture);
	}
	a->muted = muted;
	if (muted) {
		/* Drop the samples still held by the denoiser and encoder. */
		a->group_active = FALSE;
		g_clear_pointer(&a->denoise, line_denoise_free);
		opus_encoder_ctl(a->encoder, OPUS_RESET_STATE);
		a->capture_gain = CAPTURE_GAIN_ONE;
		a->clean_hold = 0;
	}
	if (!muted) {
		a->denoise = line_denoise_new();
		a->first_tx = TRUE;
		SDL_PauseAudioDevice(a->capture, 0);
	}
}

enum group_signal {
	GROUP_SILENCE,
	GROUP_UNVOICED,
	GROUP_VOICED,
};

static enum group_signal group_signal_type(const opus_int16 *pcm, guint stride,
					   guint64 *squares);

struct channel_level {
	guint64 energy;
	guint clipped;
};

static struct channel_level channel_level(const opus_int16 *pcm, guint stride)
{
	struct channel_level level = { 0 };
	int sum = 0;

	for (guint i = 0; i < GROUP_FRAME; i++) {
		int sample = pcm[i * stride];

		level.energy += sample * sample;
		level.clipped += ABS(sample) >= 32000;
		sum += sample;
	}
	/* A constant microphone offset contributes no audible signal. */
	level.energy -= (gint64)sum * sum / GROUP_FRAME;
	return level;
}

static guint choose_microphone(LineAudio *a, const opus_int16 *pcm,
			       const struct channel_level levels[2])
{
	guint current = a->capture_channel, other = current ^ 1;
	guint pick = levels[other].energy > 2 * levels[current].energy ?
			     other :
			     current;
	guint alt = pick ^ 1;

	/*
	 * Override the energy preference when one input is hitting the rails
	 * on at least 3% of samples and the other has comparable speech.
	 * Retain that choice for up to 120 ms of unvoiced consonants.
	 */
	if (levels[pick].clipped >= GROUP_FRAME / 32 &&
	    levels[pick].clipped >= levels[alt].clipped + GROUP_FRAME / 64 &&
	    levels[alt].energy * 16 >= levels[pick].energy) {
		guint64 squares;

		if (group_signal_type(pcm + alt, 2, &squares) == GROUP_VOICED ||
		    group_signal_type(pcm + 2 * FRAME + alt, 2, &squares) ==
			    GROUP_VOICED) {
			a->clean_hold = 3;
			return alt;
		}
		if (current == alt && a->clean_hold) {
			a->clean_hold--;
			return alt;
		}
	}
	a->clean_hold = 0;
	return pick;
}

static void group_capture_pcm(LineAudio *a, opus_int16 *pcm)
{
	guint64 energy = 0;

	if (a->capture_channels == 2) {
		struct channel_level levels[2] = { channel_level(pcm, 2),
						   channel_level(pcm + 1, 2) };

		/* Keep a stable mono source even on phase-inverted mic arrays. */
		a->capture_channel = choose_microphone(a, pcm, levels);
		energy = levels[a->capture_channel].energy;
		for (guint i = 0; i < GROUP_FRAME; i++)
			pcm[i] = pcm[2 * i + a->capture_channel];
	} else if (a->diagnostics) {
		energy = channel_level(pcm, 1).energy;
	}
	if (a->diagnostics) {
		struct audio_interval *stats = &a->diagnostics->interval;
		guint mean_square = energy / GROUP_FRAME;

		stats->capture_blocks++;
		stats->capture_power_max =
			MAX(stats->capture_power_max, mean_square);
	}
	if (!a->denoise)
		return;

	/* Lift quiet microphones before RNNoise estimates the noise floor. */
	int peak = 0;

	for (guint i = 0; i < GROUP_FRAME; i++)
		peak = MAX(peak, ABS((int)pcm[i]));
	if (peak >= CAPTURE_GAIN_SILENCE) {
		guint target =
			CLAMP(CAPTURE_TARGET_PEAK * CAPTURE_GAIN_ONE / peak,
			      CAPTURE_GAIN_ONE, CAPTURE_GAIN_MAX);

		if (target < a->capture_gain)
			a->capture_gain = target;
		else
			a->capture_gain += (target - a->capture_gain + 3) / 4;
		if (a->capture_gain > CAPTURE_GAIN_ONE) {
			for (guint i = 0; i < GROUP_FRAME; i++)
				pcm[i] = pcm[i] * (int)a->capture_gain /
					 CAPTURE_GAIN_ONE;
		}
	}
	line_denoise_frame(a->denoise, pcm, GROUP_FRAME);
}

/*
 * VSD distinguishes voiced frames from active but unvoiced frames. Stock
 * Opus has no public encoder voicing query. Look for a repeating waveform
 * in the 80–500 Hz pitch range of the processed microphone signal.
 */
static enum group_signal group_signal_type(const opus_int16 *pcm, guint stride,
					   guint64 *squares)
{
	enum {
		STRIDE = RATE / 4000,
		SAMPLES = FRAME / STRIDE
	};
	int band[SAMPLES], magnitude[SAMPLES + 1] = { 0 };
	int mean = 0;

	*squares = 0;
	for (int i = 0; i < SAMPLES; i++) {
		int sum = 0;

		for (int j = 0; j < STRIDE; j++) {
			int sample = pcm[(i * STRIDE + j) * stride];

			*squares += sample * sample;
			sum += sample;
		}
		band[i] = sum / STRIDE;
		mean += band[i];
	}
	/* Leave quantization noise below -80 dBov out of the activity bits. */
	if (*squares <= 10 * FRAME)
		return GROUP_SILENCE;

	mean /= SAMPLES;
	for (int i = 0; i < SAMPLES; i++) {
		band[i] -= mean;
		magnitude[i + 1] = magnitude[i] + ABS(band[i]);
	}
	for (int lag = 4000 / 500; lag <= 4000 / 80; lag++) {
		int scale = magnitude[SAMPLES] - magnitude[lag] +
			    magnitude[SAMPLES - lag];
		int difference = 0;

		for (int i = lag; i < SAMPLES; i++)
			difference += ABS(band[i] - band[i - lag]);
		if (4 * difference < scale)
			return GROUP_VOICED;
	}
	return GROUP_UNVOICED;
}

/*
 * Build a PLANET EAS2 packet from two raw Opus code-0 frames. See
 * packetizeNativeGroupOpusPairs() in linejs v3.6.5 (MIT).
 */
static int encode_group(LineAudio *a, guint8 *packet,
			opus_int16 pcm[GROUP_FRAME], gboolean *active)
{
	guint8 first[GROUP_OPUS], second[GROUP_OPUS];
	int nfirst = opus_encode(a->encoder, pcm, FRAME, first, sizeof(first));
	int nsecond = opus_encode(a->encoder, pcm + FRAME, FRAME, second,
				  sizeof(second));

	if (nfirst <= 1 || nsecond <= 1 || first[0] != second[0] ||
	    (first[0] & 7) ||
	    opus_packet_get_samples_per_frame(first, RATE) != FRAME)
		return -1;
	guint64 squared1, squared2;
	enum group_signal signal1 = group_signal_type(pcm, 1, &squared1);
	enum group_signal signal2 =
		group_signal_type(pcm + FRAME, 1, &squared2);
	guint64 loudest = MAX(squared1, squared2);
	guint active1 = signal1 != GROUP_SILENCE;
	guint active2 = signal2 != GROUP_SILENCE;

	*active = active1 || active2;
	if (a->diagnostics) {
		struct audio_interval *stats = &a->diagnostics->interval;

		stats->tx_frames += 2;
		stats->tx_configs |= 1u << (first[0] >> 3);
		stats->tx_power_sum += (squared1 + squared2) / FRAME;
		stats->tx_power_max = MAX(stats->tx_power_max, loudest / FRAME);
		stats->tx_vsd[signal1]++;
		stats->tx_vsd[signal2]++;
	}
	/* VSD carries rounded -dBov for the louder of the two frames. */
	guint8 level =
		loudest ?
			0.5 - 10 * SDL_log10(loudest / (FRAME * 1073741824.0)) :
			127;
	guint8 status = 0x40 | (signal1 << 4) | (signal2 << 2);
	const guint8 extension[] = { 0x02, 0x40,   0,	  2, 1, 2,
				     0xc0, status, level, 0, 0, 0 };

	memcpy(packet + 12, extension, sizeof(extension));
	guint8 *payload = packet + GROUP_HEADER;
	gboolean vbr = nfirst != nsecond;

	/* The high nibble advances on the RTP-marked start of a talkspurt. */
	guint chunk = a->group_chunk + (*active && !a->group_active);
	*payload++ = chunk << 4;
	*payload++ = first[0] | 3;
	*payload++ = vbr ? 0x82 : 2;
	if (first[0] & 0x80)
		*payload++ = (active1 << 7) | (active2 << 6);
	if (vbr) {
		int length = nfirst - 1;

		if (length >= 252) {
			int low = 252 + (length & 3);

			*payload++ = low;
			length = (length - low) / 4;
		}
		*payload++ = length;
	}
	memcpy(payload, first + 1, nfirst - 1);
	memcpy(payload + nfirst - 1, second + 1, nsecond - 1);
	return payload - packet + nfirst + nsecond - 2;
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
	opus_int16 pcm[2 * GROUP_FRAME];
	guint samples = a->group ? GROUP_FRAME : FRAME;
	gsize stride = a->capture_channels * sizeof(*pcm);
	gsize bytes = samples * stride;
	if (a->diagnostics)
		a->diagnostics->interval.capture_checks++;
	if (a->mic) {
		if (!take_microphone(a, pcm, bytes, samples))
			return 0;
	} else {
		const gsize frame_bytes = FRAME * stride;
		Uint32 available = SDL_GetQueuedAudioSize(a->capture);

		if (available > frame_bytes * 50) {
			SDL_ClearQueuedAudio(a->capture);
			a->timestamp += available / stride;
			a->first_tx = TRUE;
			return 0;
		}
		if (available > frame_bytes * 6) {
			/* Keep recent microphone audio after a scheduling stall. */
			while (available >= frame_bytes * 3) {
				if (SDL_DequeueAudio(a->capture, pcm,
						     frame_bytes) !=
				    frame_bytes)
					return 0;
				a->timestamp += FRAME;
				available -= frame_bytes;
			}
			a->first_tx = TRUE;
		}
		if (available < bytes ||
		    SDL_DequeueAudio(a->capture, pcm, bytes) != bytes)
			return 0;
	}
	int n;
	gboolean group_active = FALSE;
	gboolean marker = a->first_tx;

	if (a->group) {
		group_capture_pcm(a, pcm);
		n = encode_group(a, packet, pcm, &group_active);
		marker = group_active && !a->group_active;
	} else {
		line_denoise_frame(a->denoise, pcm, FRAME);
		packet[12] = 0;
		n = opus_encode(a->encoder, pcm, FRAME, packet + 13, MAX_OPUS);
		if (n > 0)
			n += 13;
	}
	packet[0] = a->group ? 0x90 : 0x80;
	packet[1] = a->tx_pt | (marker ? 0x80 : 0);
	guint16 seq = htons(a->seq);
	guint32 stamp = htonl(a->timestamp), ssrc = htonl(a->tx_ssrc);

	memcpy(packet + 2, &seq, 2);
	memcpy(packet + 4, &stamp, 4);
	memcpy(packet + 8, &ssrc, 4);
	a->timestamp += samples;
	if (n > 0 && srtp_protect(a->tx, packet, &n) == srtp_err_status_ok) {
		a->seq++;
		a->first_tx = FALSE;
		if (a->group) {
			a->group_chunk += marker;
			a->group_active = group_active;
		}
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

static gboolean parse_eas2(struct audio_packet *frame)
{
	if (frame->len <= 0)
		return FALSE;
	if ((frame->data[0] & 3) == 2) {
		frame->samples =
			unpack_mixed(frame->data, frame->len, &frame->group);
		if (frame->samples > 0)
			frame->frame_samples = opus_packet_get_samples_per_frame(
				&frame->group.toc[frame->group.count - 1],
				RATE);
	} else {
		opus_int16 lengths[MAX_FRAMES];

		frame->len = unpack_audio(&frame->data, frame->len);
		if (frame->len < 0)
			return FALSE;
		int count = opus_packet_parse(frame->data, frame->len, NULL,
					      NULL, lengths, NULL);

		if (count <= 0)
			return FALSE;
		frame->frame_samples =
			opus_packet_get_samples_per_frame(frame->data, RATE);
		frame->samples = count * frame->frame_samples;
	}
	return frame->samples > 0 && frame->samples <= MAX_PCM;
}

static int decode_frames(OpusDecoder *decoder, const struct mixed_group *group,
			 const guint8 *data, int *skip, opus_int16 pcm[MAX_PCM])
{
	guint8 frame[1 + MAX_OPUS];
	const guint8 *payload = data + group->offset;
	int written = 0;

	for (int i = 0; i < group->count; i++) {
		guint16 len = group->sizes[i];
		int samples =
			opus_packet_get_samples_per_frame(&group->toc[i], RATE);

		/* Re-decoding a retransmitted frame changes Opus prediction state. */
		if (*skip >= samples) {
			*skip -= samples;
			payload += len;
			continue;
		}
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

static int decode_packet(OpusDecoder *decoder,
			 const struct audio_packet *packet, int *skip,
			 opus_int16 pcm[MAX_PCM])
{
	if (packet->group.count)
		return decode_frames(decoder, &packet->group, packet->data,
				     skip, pcm);
	if (*skip < packet->frame_samples)
		return opus_decode(decoder, packet->data, packet->len, pcm,
				   MAX_PCM, 0);

	struct mixed_group group;
	opus_int16 sizes[MAX_FRAMES];
	guint8 toc;
	int count = opus_packet_parse(packet->data, packet->len, &toc, NULL,
				      sizes, &group.offset);

	if (count <= 0)
		return -1;
	group.count = count;
	for (int i = 0; i < count; i++) {
		group.toc[i] = toc & 0xfc;
		group.sizes[i] = sizes[i];
	}
	return decode_frames(decoder, &group, packet->data, skip, pcm);
}

static int decode_xrtp(LineAudio *a, const struct audio_packet *packet,
		       opus_int16 pcm[MAX_PCM]);

/*
 * Remove whole pitch periods. Search one 80–500 Hz speech period for
 * matching waveforms, then overlap-add across one 2.5 ms Opus subframe.
 * Budget credit over packets so slow drift does not need resampling.
 */
static int shorten_pcm(LineAudio *a, opus_int16 *pcm, int n, guint budget)
{
	const int overlap = MIN_FRAME;
	const int min_lag = RATE / 500;
	int limit = MIN(n - overlap, RATE / 80);

	a->trim_credit = MIN(a->trim_credit + budget, RATE / 80);
	if (limit < min_lag || a->trim_credit < min_lag)
		return n;
	/* Only search once a full available pitch period can be removed. */
	if ((int)a->trim_credit < limit)
		return n;
	int start = (n - limit - overlap) / 2;
	guint64 best = G_MAXUINT64;
	int lag = min_lag;

	for (int k = min_lag; k <= limit; k++) {
		guint64 error = 0;

		/* Coarse correlation at 12 kHz, sample-accurate lag. */
		for (int j = 0; j < overlap; j += 4) {
			gint32 d = pcm[start + j] - pcm[start + k + j];

			error += (gint64)d * d;
		}
		if (error < best) {
			best = error;
			lag = k;
		}
	}
	for (int j = 0; j < overlap; j++) {
		int x = pcm[start + j], y = pcm[start + lag + j];

		pcm[start + j] = x + (y - x) * j / overlap;
	}
	start += overlap;
	memmove(pcm + start, pcm + start + lag,
		(n - start - lag) * sizeof(*pcm));
	a->trim_credit -= lag;
	return n - lag;
}

static gboolean queue_pcm(LineAudio *a, opus_int16 *pcm, int samples)
{
	Uint32 queued = SDL_GetQueuedAudioSize(a->playback) / sizeof(*pcm);
	Uint32 limit = a->group ? GROUP_PLAYBACK_LIMIT : PLAYBACK_LIMIT;
	struct audio_interval *stats =
		a->diagnostics ? &a->diagnostics->interval : NULL;
	guint depth = queued;

	/* Start with a short playout cushion and bound long-term queue drift. */
	if (!queued) {
		static opus_int16 silence[GROUP_PLAYBACK_PREROLL];
		int preroll = a->group ? GROUP_PLAYBACK_PREROLL :
					 PLAYBACK_PREROLL;

		if (a->group && a->transit.count > 1)
			preroll = MIN((guint)preroll,
				      target_ms(a, samples) * (RATE / 1000));

		if (SDL_QueueAudio(a->playback, silence,
				   preroll * sizeof(*pcm)) < 0)
			return FALSE;
		depth += preroll;
		a->totals.silence += preroll;
		if (stats)
			stats->queue_prerolls++;
	}
	if (queued >= limit) {
		a->totals.dropped += samples;
		if (stats)
			stats->queue_drops++;
		return TRUE;
	}

	int input = samples;

	if (a->group && queued > target_ms(a, samples) * (RATE / 1000) &&
	    samples >= MIN_FRAME)
		samples = shorten_pcm(a, pcm, samples, samples / 50);
	else
		a->trim_credit = 0;
	if (SDL_QueueAudio(a->playback, pcm, samples * sizeof(*pcm)) < 0)
		return FALSE;
	a->reception = LINE_MEDIA_PLAYABLE;
	if (stats) {
		stats->queued_samples += samples;
		stats->queue_max = MAX(stats->queue_max, depth + samples);
		stats->trimmed += input - samples;
	}
	return TRUE;
}

/* opus_decode(..., decode_fec=1) also succeeds using PLC when a packet
 * contains no LBRR. Count only redundancy which the packet actually has.
 */
static void count_repair(LineAudio *a, const struct audio_packet *next)
{
	a->totals.fec_attempts++;
	if (opus_packet_has_lbrr &&
	    opus_packet_has_lbrr(next->data, next->len) > 0)
		a->totals.fec++;
}

static void conceal_loss(LineAudio *a, const struct audio_packet *next)
{
	guint gap = (guint16)(next->seq - a->received - 1);

	if (a->first_rx || (!gap && !a->group))
		return;
	gint32 ticks = (gint32)(next->stamp - a->next_stamp);

	/* Conceal actual gaps on the audio clock, independent of group sequence. */
	/*
	 * Some missing media may already have been concealed at the speaker
	 * deadline. Do not synthesize that interval twice.
	 */
	if (ticks > 0)
		ticks -= MIN((guint)ticks, a->group_expanded);
	if (ticks <= 0)
		return;
	int limit = a->group ? GROUP_CONCEAL_LIMIT : MAX_PCM;
	struct audio_interval *stats =
		a->diagnostics ? &a->diagnostics->interval : NULL;

	if (stats)
		stats->missed_ticks += ticks;

	if (ticks > limit || ticks % MIN_FRAME || (!a->group && next->marker)) {
		if (stats)
			stats->decoder_resets++;
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
		if (a->group)
			fec = !next->group.count && remaining == length &&
			      next->frame_samples == length &&
			      !(next->data[0] & 0x80);
		int count;

		if (next->xrtp.count) {
			/* Child decoders advance on their own RTP clocks. */
			memset(pcm, 0, length * sizeof(*pcm));
			count = length;
		} else {
			count = opus_decode(a->decoder, fec ? next->data : NULL,
					    fec ? next->len : 0, pcm, length,
					    fec);
		}

		if (count <= 0)
			break;
		a->totals.concealed += count;
		if (fec && !next->xrtp.count)
			count_repair(a, next);
		a->totals.lost++;
		if (stats)
			stats->conceal_samples += count;
		queue_pcm(a, pcm, count);
		remaining -= count;
	}
}

static gboolean play_packet(LineAudio *a, const struct audio_packet *packet)
{
	opus_int16 pcm[MAX_PCM];
	struct audio_packet rebased;
	gboolean xrtp = packet->xrtp.count > 0;
	gboolean same_clock = a->last_xrtp == xrtp;
	int skip = 0;

	if (xrtp && same_clock && !a->first_rx) {
		guint missing = (guint16)(packet->seq - a->received - 1);

		/* The outer clock is for transport; children carry media time. */
		rebased = *packet;
		rebased.stamp = a->next_stamp + missing * GROUP_FRAME;
		packet = &rebased;
	}
	if (a->group && same_clock && !a->first_rx) {
		gint32 delta = (gint32)(packet->stamp - a->next_stamp);

		if (delta <= -packet->samples) {
			a->received = packet->seq;
			return FALSE;
		}
		skip = MAX(0, -delta);
	}
	if (same_clock)
		conceal_loss(a, packet);
	int count = xrtp ? decode_xrtp(a, packet, pcm) :
			   decode_packet(a->decoder, packet, &skip, pcm);
	struct audio_interval *stats =
		a->diagnostics ? &a->diagnostics->interval : NULL;

	if (count <= skip) {
		if (stats)
			stats->decoded_errors++;
		return FALSE;
	}
	a->totals.played += count - skip;
	a->totals.delay_ms_sum +=
		SDL_GetQueuedAudioSize(a->playback) / sizeof(*pcm) / 48;
	a->totals.delay_count++;
	if (stats) {
		stats->decoded_packets++;
		stats->decoded_samples += count - skip;
		for (int i = skip; i < count; i++)
			stats->pcm_square_sum += (gint32)pcm[i] * pcm[i];
	}
	a->first_rx = FALSE;
	a->group_expanded = 0;
	a->last_xrtp = xrtp;
	a->received = packet->seq;
	a->next_stamp = packet->stamp + packet->samples;
	a->last_samples = a->group ? MIN(packet->frame_samples, FRAME) :
				     packet->samples;
	return queue_pcm(a, pcm + skip, count - skip);
}

/*
 * Fast EWMA variance (one device-delay budget), slow empirical p99 (two
 * hundred arrivals). Use the normal one-sided 99% quantile only for the
 * fast attack; the observed tail covers bursty, non-Gaussian arrivals.
 * Clamp the prediction to the range we actually saw. At 50 packets/s
 * shifting this short sorted window costs less than 160 KiB/s.
 */
static void observe_transit(struct transit_window *w, guint32 stamp, gint64 now,
			    int samples)
{
	gint64 ticks = w->ticks + (gint32)(stamp - w->stamp);

	if (!w->count)
		ticks = 0;
	w->stamp = stamp;
	w->ticks = ticks;
	gint64 transit = now - ticks * G_USEC_PER_SEC / RATE;
	guint pos = 0;

	if (w->count == TRANSIT_WINDOW) {
		while (w->sorted[pos] != w->fifo[w->slot])
			pos++;
		memmove(w->sorted + pos, w->sorted + pos + 1,
			(--w->count - pos) * sizeof(*w->sorted));
	}
	pos = w->count;
	while (pos && w->sorted[pos - 1] > transit) {
		w->sorted[pos] = w->sorted[pos - 1];
		pos--;
	}
	w->sorted[pos] = transit;
	w->fifo[w->slot] = transit;
	w->slot = (w->slot + 1) % TRANSIT_WINDOW;
	if (!w->count)
		w->mean_us = transit;
	w->count++;

	/* Matched first-order low pass: dt / (tau + dt), tau = max delay. */
	double gain = (double)samples / (JB_MAX_MS * (RATE / 1000) + samples);
	double error = transit - w->mean_us;

	w->mean_us += gain * error;
	w->variance = (1 - gain) * (w->variance + gain * error * error);
	gint64 tail = w->sorted[w->count - 1 - w->count / 100];
	double predict = w->mean_us + 2.32635 * sqrt(w->variance);
	double high = MIN(predict, (double)w->sorted[w->count - 1]);
	double margin = MAX((double)tail, high) - w->sorted[0];

	w->margin_ms = (guint)ceil(MIN(margin / 1000, (double)JB_MAX_MS));
}

static guint target_ms(const LineAudio *a, int samples)
{
	return CLAMP(a->transit.margin_ms + (guint)samples / (RATE / 1000),
		     JB_MIN_MS, JB_MAX_MS);
}

/* Audio waiting after the next one to play, in milliseconds. */
static guint jb_span_ms(const struct jitter_buffer *jb)
{
	if (!jb->count)
		return 0;
	const struct audio_packet *last = &jb->packet[jb->count - 1];
	gint32 span = last->stamp + last->samples -
		      (jb->playing ? jb->next_stamp : jb->packet[0].stamp);

	return span > 0 ? span / 48 : 0;
}

static gboolean jb_insert(LineAudio *a, struct audio_packet *frame)
{
	struct jitter_buffer *jb = a->jb;

	/* Far behind is a restarted stream: start over from this packet. */
	if (jb->have_seq &&
	    (gint16)(frame->seq - jb->next_seq) < -4 * JB_PACKETS) {
		for (guint i = 0; i < jb->count; i++)
			g_free(jb->packet[i].data);
		jb->count = 0;
		jb->have_seq = jb->playing = FALSE;
		jb->restart = TRUE;
		a->transit = (struct transit_window){ 0 };
	}
	/* Include late packets so the estimator sees the entire tail. */
	observe_transit(&a->transit, frame->stamp, frame->arrival,
			frame->samples);
	/* Late: its audio has been played or concealed already. */
	if (jb->have_seq &&
	    ((gint16)(frame->seq - jb->next_seq) < 0 ||
	     (gint32)(frame->stamp + frame->samples - jb->next_stamp) <= 0)) {
		a->totals.late++;
		return FALSE;
	}
	guint pos = jb->count;

	while (pos && (gint16)(frame->seq - jb->packet[pos - 1].seq) < 0)
		pos--;
	if (pos && jb->packet[pos - 1].seq == frame->seq)
		return FALSE;
	if (jb->count == JB_PACKETS) {
		/* Full: the oldest gives way. */
		a->totals.dropped += jb->packet[0].samples;
		g_free(jb->packet[0].data);
		memmove(jb->packet, jb->packet + 1,
			--jb->count * sizeof(*jb->packet));
		if (jb->playing)
			jb->next_seq = jb->packet[0].seq;
		if (!pos)
			return FALSE;
		pos--;
	}
	memmove(jb->packet + pos + 1, jb->packet + pos,
		(jb->count - pos) * sizeof(*jb->packet));
	frame->data = g_memdup2(frame->data, frame->len);
	jb->packet[pos] = *frame;
	jb->count++;
	return TRUE;
}

static void jb_pop(struct jitter_buffer *jb)
{
	g_free(jb->packet[0].data);
	memmove(jb->packet, jb->packet + 1, --jb->count * sizeof(*jb->packet));
}

/*
 * One-to-one playout, after WebRTC's NetEq: when the speaker runs low,
 * decode the next packet. The RTP clock decides what is missing. A gap
 * waits with concealment while less than the target is buffered behind
 * it; after that it is concealed from the following packet's in-band FEC
 * or with Opus PLC. An empty buffer is stretched with PLC for up to
 * 200 ms, then playout stops and buffers to the target again. Delay above
 * the target is shed by playing frames faster or skipping them. A now of
 * G_MAXINT64 drains the buffer at once.
 */
static gint64 jb_playout(LineAudio *a, gint64 now)
{
	struct jitter_buffer *jb = a->jb;
	gboolean drain = now == G_MAXINT64;
	guint queued = SDL_GetQueuedAudioSize(a->playback) / sizeof(opus_int16);
	guint target = target_ms(a, a->last_samples ? a->last_samples : FRAME);

	if (!jb->playing) {
		if (!jb->count)
			return G_MAXINT64;
		gint64 waited = now - jb->packet[0].arrival;

		if (!drain && queued / 48 + jb_span_ms(jb) < target &&
		    waited < (gint64)target * 1000)
			return now + 5000;
		jb->playing = TRUE;
		jb->have_seq = TRUE;
		jb->next_seq = jb->packet[0].seq;
		jb->next_stamp = jb->packet[0].stamp;
		jb->expanded = 0;
		if (jb->restart)
			opus_decoder_ctl(a->decoder, OPUS_RESET_STATE);
		jb->restart = FALSE;
	}
	while (drain ? jb->count > 0 : queued < JB_DEVICE_LOW) {
		opus_int16 pcm[MAX_PCM];
		int n, skip = 0,
		       length = a->last_samples ? a->last_samples : FRAME;
		struct audio_packet *head = jb->count ? &jb->packet[0] : NULL;
		gint32 ticks = head ? (gint32)(head->stamp - jb->next_stamp) :
				      0;
		guint level = queued / 48 + jb_span_ms(jb);

		if (head && ticks > JB_JUMP * FRAME) {
			/* A long loss or pause: resume at the next packet. */
			a->totals.lost += ticks / FRAME;
			jb->next_stamp = head->stamp;
			opus_decoder_ctl(a->decoder, OPUS_RESET_STATE);
			continue;
		}
		if (head && ticks <= 0) {
			/* Skip audio already concealed in its place. */
			skip = MIN(-ticks, head->samples);
			if (skip == head->samples) {
				jb_pop(jb);
				a->totals.late++;
				continue;
			}
			n = decode_packet(a->decoder, head, &skip, pcm) - skip;
			if (n > 0)
				memmove(pcm, pcm + skip, n * sizeof(*pcm));
			jb->next_seq = head->seq + 1;
			jb->next_stamp = head->stamp + head->samples;
			jb->expanded = 0;
			a->last_samples = MIN(head->samples, MAX_PCM);
			jb_pop(jb);
			if (n <= 0)
				continue;
			a->totals.played += n;
			level = (queued + n) / 48 + jb_span_ms(jb);
			a->totals.delay_ms_sum += level;
			a->totals.delay_count++;
			if (level <= target + FRAME / 48) {
				jb->excess_since = 0;
				a->trim_credit = 0;
			} else if (!jb->excess_since)
				jb->excess_since = now;
			if (!drain && level > target + 200) {
				a->totals.dropped += n;
				continue;
			}
			if (!drain && jb->excess_since &&
			    now - jb->excess_since >= G_USEC_PER_SEC / 2) {
				int shorter = shorten_pcm(a, pcm, n, n / 10);

				a->totals.dropped += n - shorter;
				n = shorter;
			}
		} else if (!drain && queued >= JB_DECIDE) {
			/* The speaker still has audio: decide as late as can be. */
			break;
		} else if (head && (drain || level >= target ||
				    jb->expanded >= JB_EXPAND_LIMIT)) {
			/* Lost: the following packet may carry it as FEC. */
			int missing = MIN(ticks, length);
			gboolean fec = missing == ticks && !head->group.count &&
				       head->samples == missing &&
				       head->frame_samples == missing;

			n = opus_decode(a->decoder, fec ? head->data : NULL,
					fec ? head->len : 0, pcm, missing, fec);
			jb->next_stamp += missing;
			a->totals.lost++;
			if (fec && n > 0)
				count_repair(a, head);
			if (n <= 0)
				continue;
			a->totals.concealed += n;
		} else {
			/* Wait for a late or reordered packet with concealment. */
			if (!head && jb->expanded >= JB_EXPAND_LIMIT) {
				jb->playing = FALSE;
				jb->restart = TRUE;
				break;
			}
			n = opus_decode(a->decoder, NULL, 0, pcm, length, 0);
			jb->expanded++;
			if (n <= 0)
				break;
			a->totals.concealed += n;
		}
		if (SDL_QueueAudio(a->playback, pcm, n * sizeof(*pcm)) < 0)
			break;
		a->reception = LINE_MEDIA_PLAYABLE;
		queued += n;
	}
	if (drain || !jb->playing)
		return drain || !jb->count ? G_MAXINT64 : now + 5000;
	guint low = !jb->count || jb->packet[0].stamp != jb->next_stamp ?
			    JB_DECIDE :
			    JB_DEVICE_LOW;

	return now + MAX(1000, (gint64)(queued - MIN(queued, low)) *
				       G_USEC_PER_SEC / RATE);
}

/*
 * A lost conference packet used to be concealed only after another
 * packet arrived, often after a real speaker underrun. Feed Opus PLC at
 * the device deadline. A silent/muted source stops scheduling after the
 * same bounded expansion as direct calls. XRTP has one codec per child;
 * conceal only participants present in the last authenticated mix.
 */
static gint64 group_playout_due(LineAudio *a, gint64 now)
{
	if (now == G_MAXINT64 || a->first_rx ||
	    a->group_expanded >= JB_EXPAND_LIMIT * FRAME)
		return G_MAXINT64;
	guint queued = SDL_GetQueuedAudioSize(a->playback) / sizeof(opus_int16);

	if (queued < FRAME) {
		opus_int16 pcm[FRAME];

		if (a->last_xrtp) {
			gint32 mixed[FRAME] = { 0 };

			for (int i = 0; a->tracks && i < XRTP_TRACKS; i++) {
				struct xrtp_track *track = &a->tracks[i];

				if (!track->played ||
				    track->packet != a->xrtp_packet)
					continue;
				if (opus_decode(track->decoder, NULL, 0, pcm,
						FRAME, FALSE) != FRAME)
					continue;
				track->predicted += FRAME;
				for (int j = 0; j < FRAME; j++)
					mixed[j] += pcm[j];
			}
			for (int j = 0; j < FRAME; j++)
				pcm[j] =
					CLAMP(mixed[j], G_MININT16, G_MAXINT16);
		} else if (opus_decode(a->decoder, NULL, 0, pcm, FRAME,
				       FALSE) != FRAME)
			return G_MAXINT64;
		if (SDL_QueueAudio(a->playback, pcm, sizeof(pcm)) < 0)
			return G_MAXINT64;
		a->group_expanded += FRAME;
		a->totals.concealed += FRAME;
		queued += FRAME;
	}
	return now + MAX(G_GINT64_CONSTANT(1000),
			 (gint64)(queued - FRAME) * G_USEC_PER_SEC / RATE);
}

gint64 line_audio_playout(LineAudio *a, gint64 now)
{
	if (a->jb)
		return jb_playout(a, now);
	while (a->pending_count) {
		struct audio_packet first = a->pending[0];
		gint64 until = a->group ? first.reorder_until :
					  a->reorder_until;
		gboolean gap = first.xrtp.count || !a->group ||
			       (gint32)(first.stamp - a->next_stamp) > 0;

		if (gap && (guint16)(first.seq - a->received) > 1 &&
		    a->pending_count < REORDER_PACKETS && now < until)
			return until;
		play_packet(a, &first);
		g_free(first.data);
		a->pending_count--;
		memmove(a->pending, a->pending + 1,
			a->pending_count * sizeof(*a->pending));
	}
	return group_playout_due(a, now);
}

static gint64 group_reorder_time(LineAudio *a)
{
	gint64 queued =
		SDL_GetQueuedAudioSize(a->playback) / sizeof(opus_int16);
	gint64 available =
		queued * G_USEC_PER_SEC / RATE - GROUP_OUTPUT_GUARD_US;

	gint64 limit = a->transit.count < 3 ?
			       GROUP_REORDER_US :
			       (gint64)target_ms(a, GROUP_FRAME) * 1000;

	return CLAMP(available, 0, limit);
}

static int nested_rtp_payload(const guint8 *packet, int size)
{
	if (size < 12 || packet[0] >> 6 != 2)
		return -1;
	int off = 12 + 4 * (packet[0] & 15);

	if (packet[0] & 0x10) {
		if (off + 4 > size)
			return -1;
		off += 4 + 4 * ((packet[off + 2] << 8) | packet[off + 3]);
	}
	return off < size ? off : -1;
}

static guint32 rtp_u32(const guint8 *packet)
{
	return ((guint32)packet[0] << 24) | ((guint32)packet[1] << 16) |
	       ((guint32)packet[2] << 8) | packet[3];
}

/* Called after receiving audio. A muted peer generates no timer wakeups. */
gsize line_audio_receiver_report(LineAudio *a, gint64 now, guint8 *packet,
				 gsize capacity)
{
	if (!a || a->group || !a->rr.started ||
	    now - a->rr.sent_at < G_USEC_PER_SEC)
		return 0;
	gsize size = call_rtcp_report(&a->rr, a->tx, a->tx_ssrc, a->rx_ssrc,
				      packet, capacity);

	if (size)
		a->rr.sent_at = now;
	return size;
}

/* RFC 3550 section 6.4: fraction lost is an unsigned Q0.8 value
 * for the report interval. Opus wants the nearest integer percentage
 * and allocates its own LBRR bits within the existing 32 kb/s budget.
 * EAS2 conference packets require CELT, which has no SILK LBRR.
 */
gboolean line_audio_receive_report(LineAudio *a, const guint8 *packet,
				   gsize size)
{
	guint8 plain[MAX_RTP];
	gsize pos = 0;
	int length, loss = -1;

	if (!a || a->group || !packet || size < 8 + 14 ||
	    size > sizeof(plain) || packet[0] >> 6 != 2 ||
	    (packet[1] != 200 && packet[1] != 201) ||
	    rtp_u32(packet + 4) != a->rx_ssrc)
		return FALSE;
	memcpy(plain, packet, size);
	length = size;
	if (srtp_unprotect_rtcp(a->rx, plain, &length) != srtp_err_status_ok)
		return FALSE;
	while (pos < (gsize)length) {
		const guint8 *p = plain + pos;
		gsize remaining = length - pos;

		if (remaining < 4 || p[0] >> 6 != 2)
			return FALSE;
		gsize bytes = 4u * (1u + ((guint)p[2] << 8) + p[3]);
		gsize payload = bytes;

		if (bytes > remaining)
			return FALSE;
		if (p[0] & 0x20) {
			guint pad = p[bytes - 1];

			if (bytes != remaining || !pad || pad > bytes - 4)
				return FALSE;
			payload -= pad;
		}
		pos += bytes;
		if (p[1] != 200 && p[1] != 201)
			continue;
		gsize off = p[1] == 200 ? 28 : 8;
		guint count = p[0] & 31;

		if (off + count * 24u > payload)
			return FALSE;
		if (rtp_u32(p + 4) != a->rx_ssrc)
			continue;
		for (guint i = 0; i < count; i++, off += 24)
			if (rtp_u32(p + off) == a->tx_ssrc)
				loss = (p[off + 4] * 100u + 128) / 256;
	}
	if (loss < 0)
		return FALSE;
	if ((guint)loss != a->totals.expected_loss_pct) {
		if (opus_encoder_ctl(a->encoder, OPUS_SET_PACKET_LOSS_PERC(
							 loss)) != OPUS_OK)
			return FALSE;
		a->totals.expected_loss_pct = loss;
	}
	return TRUE;
}

static struct xrtp_track *find_xrtp_track(LineAudio *a, guint32 ssrc)
{
	if (!a->tracks)
		a->tracks = g_new0(struct xrtp_track, XRTP_TRACKS);
	struct xrtp_track *oldest = a->tracks;

	for (guint i = 0; i < XRTP_TRACKS; i++) {
		struct xrtp_track *track = &a->tracks[i];

		if (track->decoder && track->ssrc == ssrc)
			return track;
		if (track->packet < oldest->packet)
			oldest = track;
	}
	OpusDecoder *decoder = oldest->decoder;
	if (decoder)
		opus_decoder_ctl(decoder, OPUS_RESET_STATE);
	else
		decoder = opus_decoder_create(RATE, 1, NULL);
	if (!decoder)
		return NULL;
	*oldest = (struct xrtp_track){ .decoder = decoder, .ssrc = ssrc };
	return oldest;
}

static void xrtp_track_gap(struct xrtp_track *track, gint32 gap,
			   opus_int16 pcm[GROUP_FRAME])
{
	if (gap > GROUP_CONCEAL_LIMIT || gap % MIN_FRAME) {
		opus_decoder_ctl(track->decoder, OPUS_RESET_STATE);
		return;
	}
	while (gap) {
		int length = MIN(gap, track->frame_samples);
		int count =
			opus_decode(track->decoder, NULL, 0, pcm, length, 0);
		if (count <= 0)
			break;
		gap -= count;
	}
}

static void diagnose_toc(struct audio_interval *stats,
			 const struct audio_packet *frame, guint8 wire_toc)
{
	guint32 configs = 1u << (wire_toc >> 3);

	for (int i = 0; i < frame->group.count; i++)
		configs |= 1u << (frame->group.toc[i] >> 3);
	stats->tocs[wire_toc & 3]++;
	stats->toc_configs |= configs;
	stats->mixed += frame->group.count != 0;
	stats->celt += (configs >> 16) != 0;
}

static int decode_xrtp(LineAudio *a, const struct audio_packet *packet,
		       opus_int16 pcm[MAX_PCM])
{
	gint32 mixed[GROUP_FRAME];
	guint64 number = ++a->xrtp_packet;
	gboolean decoded = FALSE;
	guint8 *child = packet->data;

	if (packet->xrtp.count > 1)
		memset(mixed, 0, sizeof(mixed));
	/* The authenticated outer packet defines one conference playout slot. */
	for (guint i = 0; i < packet->xrtp.count; i++) {
		guint8 *rtp = child;
		int size = packet->xrtp.sizes[i];
		int off = nested_rtp_payload(rtp, size);
		child += size;
		if (off < 0 || (rtp[1] & 127) != a->rx_pt)
			continue;
		if (rtp[0] & 0x20) {
			guint8 pad = rtp[size - 1];
			if (!pad || pad > size - off)
				continue;
			size -= pad;
		}
		struct audio_packet frame = {
			.data = rtp + off + 1,
			.stamp = rtp_u32(rtp + 4),
			.len = size - off - 1,
		};
		guint32 ssrc = rtp_u32(rtp + 8);

		if (!ssrc || frame.len < 1)
			continue;
		guint8 wire_toc = frame.data[0];

		if (!parse_eas2(&frame) || frame.samples > GROUP_FRAME)
			continue;
		struct xrtp_track *track = find_xrtp_track(a, ssrc);
		if (!track)
			continue;
		if (track->packet != number) {
			track->packet = number;
			track->start_stamp = frame.stamp;
		}
		gint32 start = (gint32)(frame.stamp - track->start_stamp);
		gint32 gap = (gint32)(frame.stamp - track->next_stamp);

		if (start < 0 || start >= GROUP_FRAME ||
		    (track->played && gap < 0))
			continue;
		if (track->played && (guint)gap > track->predicted)
			xrtp_track_gap(track, gap - track->predicted, pcm);
		int skip = 0;
		int count = decode_packet(track->decoder, &frame, &skip, pcm);
		if (count <= 0 || count > GROUP_FRAME)
			continue;
		if (a->diagnostics)
			diagnose_toc(&a->diagnostics->interval, &frame,
				     wire_toc);
		track->next_stamp = frame.stamp + frame.samples;
		track->frame_samples = MIN(frame.frame_samples, FRAME);
		track->played = TRUE;
		track->predicted = 0;
		if (packet->xrtp.count == 1) {
			memset(pcm + count, 0,
			       (GROUP_FRAME - count) * sizeof(*pcm));
			return GROUP_FRAME;
		}
		for (int j = 0; j < MIN(count, GROUP_FRAME - start); j++)
			mixed[start + j] += pcm[j];
		decoded = TRUE;
	}
	if (!decoded)
		return -1;
	for (int i = 0; i < GROUP_FRAME; i++)
		pcm[i] = CLAMP(mixed[i], G_MININT16, G_MAXINT16);
	return GROUP_FRAME;
}

static gboolean eas2_pair_40ms(const guint8 *payload, int size)
{
	if (size < 3 || size > MAX_RTP || (payload[0] & 3) < 2)
		return FALSE;
	if ((payload[0] & 3) == 2) {
		struct mixed_group mixed;

		return unpack_mixed(payload, size, &mixed) == GROUP_FRAME &&
		       mixed.count == 2;
	}
	if ((payload[1] & 63) != 2 ||
	    opus_packet_get_samples_per_frame(payload, RATE) != FRAME)
		return FALSE;
	guint8 scratch[MAX_RTP], *data = scratch;
	opus_int16 lengths[MAX_FRAMES];

	memcpy(scratch, payload, size);
	int length = unpack_audio(&data, size);

	return length > 0 &&
	       opus_packet_parse(data, length, NULL, NULL, lengths, NULL) ==
		       2 &&
	       lengths[0] && lengths[1];
}

static gboolean parse_pld(struct xrtp_layout *layout, const guint8 *sizes,
			  int size, const guint8 *payload, int length, guint pt,
			  struct audio_interval *stats)
{
	guint parts = 0, p40 = 0;

	if (stats)
		stats->pld++;
	while (size && *sizes && parts < XRTP_PARTS) {
		/* PLANET's varint uses the top two bits as a width exponent. */
		unsigned int width = 1u << (*sizes >> 6);

		if (width > (unsigned int)size)
			return FALSE;
		guint64 count = *sizes & 63;

		for (guint i = 1; i < width; i++)
			count = count << 8 | sizes[i];
		if (count > (guint64)length)
			return FALSE;
		int off = nested_rtp_payload(payload, count);

		if (off < 0)
			return FALSE;
		if (stats && (payload[1] & 127) == pt && !(payload[off] & 7))
			p40 += eas2_pair_40ms(payload + off + 1,
					      count - off - 1);
		layout->sizes[parts] = count;
		payload += count;
		length -= count;
		sizes += width;
		size -= width;
		parts++;
	}
	if (length || !parts)
		return FALSE;
	for (int i = 0; i < size; i++) {
		if (sizes[i])
			return FALSE;
	}
	layout->count = parts;
	if (stats) {
		stats->xrtp++;
		stats->xrtp_parts += parts;
		stats->xrtp_p40 += p40;
	}
	return TRUE;
}

static gboolean unpack_audio_extensions(LineAudio *a,
					struct xrtp_layout *layout,
					const guint8 *packet, int offset,
					int size)
{
	int fixed = 12 + 4 * (packet[0] & 15);
	struct audio_interval *stats =
		a->diagnostics ? &a->diagnostics->interval : NULL;

	if (!(packet[0] & 0x10) || packet[fixed] != 2)
		return TRUE;
	for (int i = fixed + 4; i + 2 <= offset;) {
		guint id = packet[i++];
		int bytes = 2 * packet[i++];

		if (bytes > offset - i)
			return id != 3 || !a->group;
		if (stats && id == 1 && bytes >= 2 && (packet[i] & 0x80))
			stats->vsd_frames[MIN(packet[i + 1] >> 6, 2)]++;
		if (id == 3 && a->group &&
		    (layout->count ||
		     !parse_pld(layout, packet + i, bytes, packet + offset,
				size - offset, a->rx_pt, stats)))
			return FALSE;
		i += bytes;
	}
	return TRUE;
}

static void diagnose_received_audio(struct audio_diagnostics *d,
				    const struct audio_packet *frame,
				    guint payload, guint csrc)
{
	struct audio_interval *stats = &d->interval;
	gint64 now = g_get_monotonic_time();

	stats->csrc += csrc > 0;
	stats->packets_20ms += frame->samples == FRAME;
	stats->packets_40ms += frame->samples == GROUP_FRAME;
	stats->packets_other += frame->samples != FRAME &&
				frame->samples != GROUP_FRAME;
	stats->payload_bytes += payload;
	stats->payload_max = MAX(stats->payload_max, payload);
	if (d->last_arrival) {
		gint64 elapsed = now - d->last_arrival;
		gint32 ticks = (gint32)(frame->stamp - d->last_stamp);

		if ((gint16)(frame->seq - d->last_seq) > 0 && ticks > 0) {
			gint64 media = (gint64)ticks * G_USEC_PER_SEC / RATE;
			guint jitter =
				MIN(ABS(elapsed - media), 60 * G_USEC_PER_SEC);

			/* RTP transit variation weighted as in RFC 3550. */
			d->jitter_16us -= (d->jitter_16us + 8) / 16;
			d->jitter_16us += jitter;
		}
		stats->arrival_max_us =
			MAX(stats->arrival_max_us, MIN(elapsed, G_MAXUINT));
	}
	d->last_arrival = now;
	d->last_stamp = frame->stamp;
	d->last_seq = frame->seq;
}

static gboolean receive_packet(LineAudio *a, const guint8 *packet, gsize size);

gboolean line_audio_receive_at(LineAudio *a, const guint8 *packet, gsize size,
			       gint64 arrival)
{
	if (!a)
		return FALSE;
	a->rx_arrival = MAX(arrival, 0);
	gboolean ok = receive_packet(a, packet, size);

	a->rx_arrival = 0;
	return ok;
}

gboolean line_audio_receive(LineAudio *a, const guint8 *packet, gsize size)
{
	return line_audio_receive_at(a, packet, size, 0);
}

static gboolean receive_packet(LineAudio *a, const guint8 *packet, gsize size)
{
	if (!a || !packet || size < 23 || size > MAX_RTP ||
	    (packet[0] >> 6) != 2 || (packet[1] & 127) != a->rx_pt)
		return FALSE;
	guint32 ssrc;
	struct audio_diagnostics *d = a->diagnostics;

	if (d) {
		d->interval.wire++;
		d->interval.bytes += size;
	}

	memcpy(&ssrc, packet + 8, sizeof(ssrc));
	if (ntohl(ssrc) != a->rx_ssrc) {
		if (d)
			d->interval.other_ssrc++;
		return FALSE;
	}
	guint8 plain[MAX_RTP];

	memcpy(plain, packet, size);
	int n = size;
	srtp_err_status_t result = srtp_unprotect(a->rx, plain, &n);

	if (result != srtp_err_status_ok) {
		if (d && (result == srtp_err_status_replay_fail ||
			  result == srtp_err_status_replay_old))
			d->interval.replay++;
		else if (d)
			d->interval.rejected++;
		return FALSE;
	}
	if (a->reception == LINE_MEDIA_WAITING)
		a->reception = LINE_MEDIA_RECEIVING;
	if (d)
		d->interval.auth++;
	int off = 12 + 4 * (plain[0] & 15);

	if (plain[0] & 0x10) {
		if (off + 4 > n)
			goto invalid;
		off += 4 + 4 * ((plain[off + 2] << 8) | plain[off + 3]);
	}
	if (plain[0] & 0x20) {
		if (plain[n - 1] == 0 || plain[n - 1] > n - off)
			goto invalid;
		n -= plain[n - 1];
	}
	if (off >= n)
		goto invalid;
	guint16 seq = ((guint16)plain[2] << 8) | plain[3];
	gint16 delta = (gint16)(seq - a->received);

	struct audio_packet frame = {
		.data = plain + off,
		.stamp = rtp_u32(plain + 4),
		.seq = seq,
		.len = n - off,
		.marker = (plain[1] & 0x80) != 0,
	};

	if ((a->group || d) &&
	    !unpack_audio_extensions(a, &frame.xrtp, plain, off, n))
		goto invalid;
	if (frame.xrtp.count) {
		frame.samples = GROUP_FRAME;
		frame.frame_samples = FRAME;
	} else {
		if (frame.len < 2)
			goto invalid;
		guint8 wire_toc = frame.data[1];

		if (d) {
			guint8 chunk = frame.data[0] >> 4;

			d->interval.silence_headers += (frame.data[0] & 8) != 0;
			d->interval.chunk_changes += d->have_chunk &&
						     chunk != d->last_chunk;
			d->last_chunk = chunk;
			d->have_chunk = TRUE;
		}
		frame.data++;
		frame.len--;
		if (!parse_eas2(&frame))
			goto invalid;
		if (d)
			diagnose_toc(&d->interval, &frame, wire_toc);
	}
	if (d)
		diagnose_received_audio(d, &frame, n - off - !frame.xrtp.count,
					plain[0] & 15);

	frame.arrival = a->rx_arrival ? a->rx_arrival : g_get_monotonic_time();
	if (!a->group) {
		if (!a->rr.started)
			a->rr.sent_at = frame.arrival;
		call_rtcp_arrival(&a->rr, frame.seq, frame.stamp, RATE,
				  frame.arrival);
	}
	if (a->jb)
		return jb_insert(a, &frame);
	/*
	 * The mixer's XRTP children have independent RTP clocks. The outer
	 * sequence advances one fixed conference slot per packet.
	 */
	gboolean xrtp = frame.xrtp.count != 0;

	if (xrtp != a->transit_xrtp)
		a->transit = (struct transit_window){ 0 };
	guint32 stamp =
		xrtp ? a->transit.stamp +
				(gint16)(seq - a->transit_seq) * GROUP_FRAME :
		       frame.stamp;

	observe_transit(&a->transit, stamp, frame.arrival, frame.samples);
	a->transit_xrtp = xrtp;
	a->transit_seq = seq;
	if (!a->first_rx && delta <= 0) {
		a->totals.late++;
		if (d)
			d->interval.late++;
		return FALSE;
	}
	if (a->first_rx || delta == 1 || delta > REORDER_PACKETS + 1) {
		if (!a->first_rx && delta > REORDER_PACKETS + 1)
			line_audio_playout(a, G_MAXINT64);
		gboolean played = play_packet(a, &frame);

		if (a->pending_count)
			line_audio_playout(a, g_get_monotonic_time());
		return played;
	}
	gint64 now = g_get_monotonic_time();
	if (d)
		d->interval.reordered++;

	if (a->group)
		frame.reorder_until = frame.arrival + group_reorder_time(a);
	else if (!a->pending_count)
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

invalid:
	if (d)
		d->interval.invalid++;
	return FALSE;
}
