// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Bad-network harness for call media. Real VP8 video and Opus audio
 * streams cross a simulated path with burst loss, delay, jitter,
 * reordering, a bandwidth-limited bottleneck queue and outages, on a
 * virtual clock. Run with --verbose to print the quality metrics.
 */
#include "call_audio.h"
#include "call_video.h"
#include <SDL.h>
#include <gmodule.h>
#include <math.h>
#include <string.h>

static gint64 fake_now = 1000000;

G_MODULE_EXPORT gint64 __wrap_g_get_monotonic_time(void)
{
	return fake_now;
}

/*
 * Time the call thread spends away from the socket: encoding and decoding
 * pictures on the same thread, as before the encoder thread, or only
 * decoding them. Packets wait in the socket meanwhile.
 */
struct thread_load {
	guint busy_ms, every_ms, key_busy_ms, key_every_ms;
	/* Receivers get the kernel's receive time, not the time of reading. */
	gboolean kernel_stamps;
};

/* A one-way path. Times are milliseconds from the start of the run. */
struct link_profile {
	const char *name;
	double loss; /* loss outside bursts */
	double burst_enter, burst_leave, burst_loss; /* Gilbert-Elliott */
	guint delay_ms, jitter_ms;
	gboolean reorder;
	guint kbps, queue_ms; /* bottleneck rate, zero for none */
	guint low_kbps, low_from_ms, low_to_ms; /* temporary rate drop */
	guint outage_from_ms, outage_ms;
	guint spike_every_ms, spike_ms; /* periodic stalls, as Wi-Fi scans */
	const struct thread_load *load;
	/*
	 * The bottleneck is this machine's Wi-Fi uplink, so the kernel's
	 * send queue (SIOCOUTQ) shows it to the sender.
	 */
	gboolean local_uplink, ignore_local_queue;
	/* Pass marks, each one the code before this harness failed. */
	double min_fps, min_screen_psnr;
	guint max_freeze_ms, max_recovery_ms, max_dead_air_ms, max_delay_ms;
	guint max_latency_ms;
};

/* Whether the call thread is busy at t, in ms from the start of the run. */
static gboolean thread_busy(const struct thread_load *load, gint64 t)
{
	if (!load)
		return FALSE;
	if (load->key_every_ms && t % load->key_every_ms < load->key_busy_ms)
		return TRUE;
	return load->every_ms && t % load->every_ms < load->busy_ms;
}

struct in_flight {
	gint64 at;
	gsize size;
	guint8 data[1600];
};

struct link {
	const struct link_profile *p;
	GRand *rand;
	GQueue queue;
	gint64 start, busy_until, last_arrival;
	gboolean bad;
	guint sent, lost, queue_drops;
	guint64 bytes;
};

static void link_init(struct link *l, const struct link_profile *p,
		      guint32 seed)
{
	*l = (struct link){ .p = p,
			    .rand = g_rand_new_with_seed(seed),
			    .start = fake_now };
	g_queue_init(&l->queue);
}

static void link_clear(struct link *l)
{
	g_queue_clear_full(&l->queue, g_free);
	g_rand_free(l->rand);
}

static gint compare_arrival(gconstpointer a, gconstpointer b, gpointer data)
{
	const struct in_flight *x = a, *y = b;

	(void)data;
	return x->at < y->at ? -1 : x->at > y->at;
}

static gboolean link_send(struct link *l, const guint8 *data, gsize size)
{
	const struct link_profile *p = l->p;
	gint64 t = (fake_now - l->start) / 1000;
	double loss = p->loss;

	g_assert_cmpuint(size, <=, sizeof(((struct in_flight *)0)->data));
	l->sent++;
	l->bytes += size;
	if (p->burst_enter) {
		if (l->bad && g_rand_double(l->rand) < p->burst_leave)
			l->bad = FALSE;
		else if (!l->bad && g_rand_double(l->rand) < p->burst_enter)
			l->bad = TRUE;
		if (l->bad)
			loss = p->burst_loss;
	}
	if ((p->outage_ms && t >= p->outage_from_ms &&
	     t < p->outage_from_ms + p->outage_ms) ||
	    g_rand_double(l->rand) < loss) {
		l->lost++;
		return TRUE;
	}
	gint64 depart = fake_now;
	guint kbps = p->low_kbps && t >= p->low_from_ms && t < p->low_to_ms ?
			     p->low_kbps :
			     p->kbps;

	if (kbps) {
		gint64 begin = MAX(fake_now, l->busy_until);

		if (begin - fake_now > (gint64)p->queue_ms * 1000) {
			l->queue_drops++;
			return TRUE;
		}
		l->busy_until = begin + (gint64)size * 8000 / kbps;
		depart = l->busy_until;
	}
	gint64 at = depart + p->delay_ms * 1000;

	if (p->jitter_ms)
		at += g_rand_int_range(l->rand, 0, p->jitter_ms * 1000);
	if (p->spike_every_ms) {
		gint64 phase = (depart - l->start) / 1000 % p->spike_every_ms;

		if (phase < p->spike_ms)
			at += (p->spike_ms - phase) * 1000;
	}
	if (!p->reorder) {
		at = MAX(at, l->last_arrival);
		l->last_arrival = at;
	}
	struct in_flight *f = g_new(struct in_flight, 1);

	f->at = at;
	f->size = size;
	memcpy(f->data, data, size);
	g_queue_insert_sorted(&l->queue, f, compare_arrival, NULL);
	return TRUE;
}

/* Bytes the kernel would hold for the socket: about twice the payload. */
static guint64 link_held(const struct link *l, guint kbps)
{
	if (!kbps || l->busy_until <= fake_now)
		return 0;
	return (guint64)(l->busy_until - fake_now) * kbps / 8000 * 2;
}

static guint link_rate(const struct link *l)
{
	const struct link_profile *p = l->p;
	gint64 t = (fake_now - l->start) / 1000;

	return p->low_kbps && t >= p->low_from_ms && t < p->low_to_ms ?
		       p->low_kbps :
		       p->kbps;
}

typedef void (*link_receive_fn)(const guint8 *data, gsize size, gint64 arrival,
				gpointer user);

static void link_deliver(struct link *l, link_receive_fn receive, gpointer user)
{
	struct in_flight *f;
	const struct thread_load *load = l->p->load;

	while ((f = g_queue_peek_head(&l->queue)) && f->at <= fake_now) {
		g_queue_pop_head(&l->queue);
		receive(f->data, f->size,
			load && load->kernel_stamps ? f->at : 0, user);
		g_free(f);
	}
}

/* --- Video ------------------------------------------------------------ */

enum {
	VW = 640,
	VH = 360,
	VFPS = 15,
	VKBPS = 800,
	MARK_ROWS = 32,
	MARK_BLOCKS = 16,
	INDEX_BITS = 12,
	VIDEO_SECONDS = 20,
};

static guint tri(int a)
{
	return ABS((a & 511) - 256);
}

/* Moving gradients and textured boxes, with the frame index in the top band. */
static void source_frame(guint index, guint8 *y, guint8 *u, guint8 *v)
{
	static const int box[3][4] = { { 40, 60, 5, 3 },
				       { 400, 120, -4, 2 },
				       { 250, 200, 3, -5 } };
	int bx[3], by[3];

	for (int b = 0; b < 3; b++) {
		int span_x = VW - 96, span_y = VH - MARK_ROWS - 72;
		int px = (box[b][0] + box[b][2] * (int)index) % (2 * span_x);
		int py = (box[b][1] + box[b][3] * (int)index) % (2 * span_y);

		px = px < 0 ? px + 2 * span_x : px;
		py = py < 0 ? py + 2 * span_y : py;
		bx[b] = px < span_x ? px : 2 * span_x - px;
		by[b] = MARK_ROWS + (py < span_y ? py : 2 * span_y - py);
	}
	for (int r = 0; r < VH; r++) {
		for (int c = 0; c < VW; c++) {
			guint8 *p = y + r * VW + c;

			if (r < MARK_ROWS) {
				int block = c * MARK_BLOCKS / VW;
				gboolean bit = block < INDEX_BITS ?
						       (index >> block) & 1 :
						       !(block & 1);

				*p = bit ? 235 : 16;
				continue;
			}
			guint value = 40 + (tri(c * 2 + index * 6) +
					    tri(r * 3 - index * 4)) *
						   3 / 8;

			for (int b = 0; b < 3; b++)
				if (c >= bx[b] && c < bx[b] + 96 &&
				    r >= by[b] && r < by[b] + 72)
					value = ((c - bx[b]) / 8 +
						 (r - by[b]) / 8) &
								1 ?
							200 :
							60;
			value += ((guint)(c * 1103515245u + r * 12345u +
					  index * 2654435761u) >>
				  13) &
				 3;
			*p = MIN(value, 255);
		}
	}
	for (int r = 0; r < VH / 2; r++) {
		for (int c = 0; c < VW / 2; c++) {
			u[r * VW / 2 + c] = 112 + tri(c * 4 + index * 4) / 8;
			v[r * VW / 2 + c] = 144 - tri(r * 4 - index * 2) / 8;
		}
	}
}

struct video_run {
	const struct link_profile *profile;
	struct call_video *tx, *rx;
	struct link forward, back;
	guint8 *screen, *source;
	guint screen_w, screen_h, shown_index, shown;
	gboolean have_screen;
	gint64 last_shown, freeze_us;
	guint freezes, firs, decoded_frames;
	double decoded_psnr, screen_psnr;
	guint screen_samples;
	gint64 measure_from, outage_end, recovered_at;
	guint index_at_outage_end;
	guint8 *cache[8];
	guint cache_index[8];
	/* Capture time of each picture, for capture-to-screen latency. */
	gint64 captured[4096];
	gint64 latency_sum;
	guint latency_count, latency_hist[300];
};

static guint8 sample(const guint8 *p, guint w, guint h, double x, double y)
{
	double fx = CLAMP(x, 0, w - 1.001), fy = CLAMP(y, 0, h - 1.001);
	guint ix = fx, iy = fy;
	double ax = fx - ix, ay = fy - iy;
	const guint8 *q = p + iy * w + ix;

	return (q[0] * (1 - ax) + q[1] * ax) * (1 - ay) +
	       (q[w] * (1 - ax) + q[w + 1] * ax) * ay + 0.5;
}

/* Luma PSNR of the shown picture, scaled to the source size, below the band. */
static double psnr(const struct video_run *run, const guint8 *source)
{
	double sum = 0, sx = (double)run->screen_w / VW,
	       sy = (double)run->screen_h / VH;
	guint count = 0;

	for (int r = MARK_ROWS; r < VH; r += 2) {
		for (int c = 0; c < VW; c += 2) {
			int d = sample(run->screen, run->screen_w,
				       run->screen_h, (c + 0.5) * sx - 0.5,
				       (r + 0.5) * sy - 0.5) -
				source[r * VW + c];

			sum += d * d;
			count++;
		}
	}
	double mse = sum / count;

	return mse < 0.01 ? 60 : 10 * log10(255.0 * 255.0 / mse);
}

static const guint8 *source_cached(struct video_run *run, guint index)
{
	guint slot = index % G_N_ELEMENTS(run->cache);

	if (!run->cache[slot] || run->cache_index[slot] != index) {
		if (!run->cache[slot])
			run->cache[slot] = g_malloc(VW * VH * 3 / 2);
		source_frame(index, run->cache[slot],
			     run->cache[slot] + VW * VH,
			     run->cache[slot] + VW * VH * 5 / 4);
		run->cache_index[slot] = index;
	}
	return run->cache[slot];
}

static gboolean read_index(const struct video_run *run, guint *index)
{
	guint bits = 0;

	for (guint b = 0; b < MARK_BLOCKS; b++) {
		double x = (b + 0.5) * VW / MARK_BLOCKS * run->screen_w / VW;
		double y = MARK_ROWS / 2.0 * run->screen_h / VH;
		gboolean bit = sample(run->screen, run->screen_w, run->screen_h,
				      x, y) > 128;

		if (b < INDEX_BITS)
			bits |= (guint)bit << b;
		else if (bit != !(b & 1))
			return FALSE;
	}
	*index = bits;
	return TRUE;
}

static void video_display(const struct line_camera_frame *frame, gpointer data)
{
	struct video_run *run = data;
	guint index;

	run->screen_w = frame->width;
	run->screen_h = frame->height;
	for (guint r = 0; r < frame->height; r++)
		memcpy(run->screen + r * frame->width,
		       frame->plane[0] + r * frame->stride[0], frame->width);
	run->decoded_frames++;
	if (!read_index(run, &index) ||
	    (run->have_screen &&
	     (guint)((index - run->shown_index) & 4095) > 2048))
		return;
	run->have_screen = TRUE;
	run->shown_index = index;
	source_frame(index, run->source, run->source + VW * VH,
		     run->source + VW * VH * 5 / 4);
	if (fake_now < run->measure_from)
		return;
	run->shown++;
	run->decoded_psnr += psnr(run, run->source);
	gint64 latency = fake_now - run->captured[index & 4095];

	if (latency >= 0 && latency < 3 * G_USEC_PER_SEC) {
		run->latency_sum += latency;
		run->latency_count++;
		run->latency_hist[MIN(latency / 10000, 299)]++;
	}
	gint64 gap = fake_now - MAX(run->last_shown, run->measure_from);
	gint64 frame_us = G_USEC_PER_SEC / VFPS;

	if (gap > MAX(3 * frame_us, frame_us + 150000)) {
		if (g_getenv("NET_TRACE"))
			g_printerr("freeze until %.2f s, %" G_GINT64_FORMAT
				   " ms\n",
				   (fake_now - run->measure_from) / 1e6 + 1,
				   gap / 1000);
		run->freezes++;
		run->freeze_us += gap;
	}
	run->last_shown = fake_now;
	if (run->outage_end && !run->recovered_at &&
	    fake_now >= run->outage_end && index > run->index_at_outage_end)
		run->recovered_at = fake_now;
}

static gboolean video_forward(const guint8 *packet, gsize size, gpointer data)
{
	struct video_run *run = data;

	return link_send(&run->forward, packet, size);
}

static gboolean video_back(const guint8 *packet, gsize size, gpointer data)
{
	struct video_run *run = data;

	if (size >= 2 && packet[1] == 206 && (packet[0] & 31) != 15) {
		run->firs++;
		if (g_getenv("NET_TRACE"))
			g_printerr("keyframe request at %.2f s\n",
				   (fake_now - run->forward.start) / 1e6);
	}
	return link_send(&run->back, packet, size);
}

static void video_arrive(const guint8 *packet, gsize size, gint64 arrival,
			 gpointer data)
{
	struct video_run *run = data;

	call_video_receive_at(run->rx, packet, size, arrival, video_display,
			      run);
}

static void feedback_arrive(const guint8 *packet, gsize size, gint64 arrival,
			    gpointer data)
{
	struct video_run *run = data;

	call_video_receive_rtcp(run->tx, packet, size,
				arrival ? arrival : fake_now, video_forward,
				run);
}

/* Capture to screen, mean and 95th percentile, in milliseconds. */
static void latency_of(const struct video_run *run, double *mean, guint *p95)
{
	guint seen = 0, i = 0;

	*mean = run->latency_count ?
			(double)run->latency_sum / run->latency_count / 1000 :
			0;
	for (; i < G_N_ELEMENTS(run->latency_hist); i++) {
		seen += run->latency_hist[i];
		if (seen * 20 >= run->latency_count * 19)
			break;
	}
	*p95 = i * 10 + 10;
}

struct video_result {
	double fps, decoded_psnr, screen_psnr, kbps, latency_ms;
	guint latency_p95_ms, local_cuts;
	gint64 freeze_ms, recovery_ms;
	guint freezes, firs;
	double loss;
};

static const guint8 key_a[30] = { 1, 2, 3 }, key_b[30] = { 4, 5, 6 };

static struct video_result run_video(const struct link_profile *profile)
{
	struct video_run run = { .profile = profile };
	const gint64 start = fake_now = 10 * G_USEC_PER_SEC;
	const gint64 end = start + VIDEO_SECONDS * G_USEC_PER_SEC;
	guint8 *frame = g_malloc(VW * VH * 3 / 2);
	gint64 next_frame = start,
	       next_tick = start + G_USEC_PER_SEC / VFPS / 2;
	guint index = 0;
	struct line_camera_frame image = {
		.plane = { frame, frame + VW * VH, frame + VW * VH * 5 / 4 },
		.stride = { VW, VW / 2, VW / 2 },
		.width = VW,
		.height = VH,
	};
	g_autoptr(GError) e = NULL;

	run.screen = g_malloc(1920 * 1080);
	run.source = g_malloc(VW * VH * 3 / 2);
	run.measure_from = start + G_USEC_PER_SEC;
	run.tx = call_video_open(key_a, key_b, 97, 101, 102, VKBPS, VFPS, &e);
	g_assert_no_error(e);
	run.rx = call_video_open(key_b, key_a, 97, 102, 101, VKBPS, VFPS, &e);
	g_assert_no_error(e);
	link_init(&run.forward, profile, 7);
	link_init(&run.back, profile, 11);
	if (profile->outage_ms)
		run.outage_end =
			start +
			(profile->outage_from_ms + profile->outage_ms) * 1000;
	for (; fake_now < end; fake_now += 1000) {
		link_deliver(&run.forward, video_arrive, &run);
		link_deliver(&run.back, feedback_arrive, &run);
		if (g_getenv("NET_TRACE") && (fake_now - start) % 500000 == 0) {
			struct call_video_recovery t;

			call_video_recovery(run.tx, &t);
			g_printerr(
				"t=%5.1f target=%u enc=%u %ux%u local=%ums cuts=%u link=%ukbps\n",
				(fake_now - start) / 1e6, t.target_kbps,
				t.encoder_kbps, t.width, t.height,
				t.local_queue_ms, t.local_cuts,
				link_rate(&run.forward));
		}
		if (fake_now >= next_frame) {
			source_frame(index, frame, frame + VW * VH,
				     frame + VW * VH * 5 / 4);
			run.captured[index & 4095] = fake_now;
			if (profile->local_uplink &&
			    !profile->ignore_local_queue)
				call_video_local_queue(
					run.tx,
					link_held(&run.forward,
						  link_rate(&run.forward)),
					fake_now);
			g_assert_true(call_video_encode(run.tx, &image,
							fake_now, video_forward,
							&run, &e));
			if (run.outage_end && fake_now <= run.outage_end)
				run.index_at_outage_end = index;
			index++;
			next_frame += G_USEC_PER_SEC / VFPS;
		}
		/* The call thread's receiver loss recovery. */
		call_video_poll(run.rx, run.rx, fake_now, TRUE, video_back,
				&run, video_display, &run);
		if (fake_now >= next_tick) {
			/*
			 * What a viewer sees now against the newest pictures
			 * the path could have delivered. Showing any of the
			 * last four (267 ms) counts as current, so a frozen
			 * picture scores against the moving source.
			 */
			gint64 ideal =
				(fake_now - profile->delay_ms * 1000 - start) *
				VFPS / G_USEC_PER_SEC;

			if (fake_now >= run.measure_from && ideal >= 3) {
				double best = 0;

				for (gint64 i = ideal - 3;
				     run.have_screen && i <= ideal; i++)
					best = MAX(
						best,
						psnr(&run,
						     source_cached(&run, i)));
				run.screen_psnr += best;
				run.screen_samples++;
			}
			next_tick += G_USEC_PER_SEC / VFPS;
		}
	}
	gint64 tail = end - MAX(run.last_shown, run.measure_from);

	if (tail > 3 * G_USEC_PER_SEC / VFPS) {
		run.freezes++;
		run.freeze_us += tail;
	}
	double seconds = (double)(end - run.measure_from) / G_USEC_PER_SEC;
	struct video_result r = {
		.fps = run.shown / seconds,
		.local_cuts = 0,
		.decoded_psnr = run.shown ? run.decoded_psnr / run.shown : 0,
		.screen_psnr = run.screen_samples ?
				       run.screen_psnr / run.screen_samples :
				       0,
		.kbps = run.forward.bytes * 8.0 / 1000 / VIDEO_SECONDS,
		.freeze_ms = run.freeze_us / 1000,
		.recovery_ms =
			run.recovered_at ?
				(run.recovered_at - run.outage_end) / 1000 :
				-1,
		.freezes = run.freezes,
		.firs = run.firs,
		.loss = run.forward.sent ? 100.0 *
						   (run.forward.lost +
						    run.forward.queue_drops) /
						   run.forward.sent :
					   0,
	};
	latency_of(&run, &r.latency_ms, &r.latency_p95_ms);
	struct call_video_recovery tx, rx;

	call_video_recovery(run.tx, &tx);
	call_video_recovery(run.rx, &rx);
	r.local_cuts = tx.local_cuts;
	g_test_message(
		"video %-18s fps=%5.2f freeze-ms=%6" G_GINT64_FORMAT
		" freezes=%3u psnr-decoded=%5.2f psnr-screen=%5.2f recovery-ms=%5" G_GINT64_FORMAT
		" sent-kbps=%6.1f path-loss=%4.1f%% keyframe-requests=%u latency-ms=%6.1f latency-p95-ms=%4u",
		profile->name, r.fps, r.freeze_ms, r.freezes, r.decoded_psnr,
		r.screen_psnr, r.recovery_ms, r.kbps, r.loss, r.firs,
		r.latency_ms, r.latency_p95_ms);
	g_test_message(
		"      requests=%u seqs=%u resent=%u answered=%u gave-up=%u late=%u rembs=%u rtt=%ums hold=%ums rate-changes=%u rescales=%u final=%ukbps %ux%u local-cuts=%u",
		rx.requests, rx.requested, tx.retransmitted, rx.retransmitted,
		rx.gave_up, rx.late, rx.rembs, rx.rtt_ms, rx.hold_ms,
		tx.rate_changes, tx.rescales, tx.target_kbps, tx.width,
		tx.height, tx.local_cuts);
	link_clear(&run.forward);
	link_clear(&run.back);
	call_video_close(run.tx);
	call_video_close(run.rx);
	g_free(frame);
	g_free(run.screen);
	g_free(run.source);
	for (guint i = 0; i < G_N_ELEMENTS(run.cache); i++)
		g_free(run.cache[i]);
	return r;
}

/* --- Audio ------------------------------------------------------------ */

enum {
	RATE = 48000,
	DEVICE_CHUNK = 512,
	AUDIO_SECONDS = 30,
	MAX_DEVICES = 8,
};

struct device {
	gboolean used, capture;
	guint64 consumed; /* capture samples handed out */
	gint64 epoch;
	guint64 queued; /* playback samples waiting */
	guint64 underrun, total, queue_sum, queue_checks;
};

static struct device devices[MAX_DEVICES];

G_MODULE_EXPORT int __wrap_SDL_InitSubSystem(Uint32 flags)
{
	(void)flags;
	return 0;
}

G_MODULE_EXPORT SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(
	const char *name, int capture, const SDL_AudioSpec *desired,
	SDL_AudioSpec *obtained, int changes)
{
	(void)name;
	(void)desired;
	(void)obtained;
	(void)changes;
	for (guint i = 1; i < MAX_DEVICES; i++) {
		if (devices[i].used)
			continue;
		devices[i] = (struct device){ .used = TRUE,
					      .capture = !!capture,
					      .epoch = fake_now };
		return i;
	}
	return 0;
}

G_MODULE_EXPORT void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID id)
{
	if (id < MAX_DEVICES)
		devices[id].used = FALSE;
}

G_MODULE_EXPORT void __wrap_SDL_PauseAudioDevice(SDL_AudioDeviceID id,
						 int pause)
{
	(void)id;
	(void)pause;
}

G_MODULE_EXPORT void __wrap_SDL_ClearQueuedAudio(SDL_AudioDeviceID id)
{
	struct device *d = &devices[id];

	if (d->capture)
		d->consumed = (fake_now - d->epoch) * RATE / G_USEC_PER_SEC;
	else
		d->queued = 0;
}

G_MODULE_EXPORT int __wrap_SDL_QueueAudio(SDL_AudioDeviceID id, const void *pcm,
					  Uint32 len)
{
	(void)pcm;
	devices[id].queued += len / 2;
	return 0;
}

G_MODULE_EXPORT Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID id)
{
	struct device *d = &devices[id];

	if (!d->capture)
		return d->queued * 2;
	guint64 produced = (fake_now - d->epoch) * RATE / G_USEC_PER_SEC;

	return (produced - d->consumed) * 2;
}

/* A voiced, syllable-shaped signal: harmonics under a 4 Hz envelope. */
G_MODULE_EXPORT Uint32 __wrap_SDL_DequeueAudio(SDL_AudioDeviceID id, void *pcm,
					       Uint32 len)
{
	struct device *d = &devices[id];
	gint16 *out = pcm;

	for (Uint32 i = 0; i < len / 2; i++, d->consumed++) {
		double t = (double)d->consumed / RATE;
		double envelope = 0.5 + 0.5 * sin(2 * G_PI * 4 * t);
		double s = 0;

		for (int h = 1; h <= 6; h++)
			s += sin(2 * G_PI * 140 * h * t) / h;
		out[i] = 4000 * envelope * s;
	}
	return len;
}

/* The playback device takes one callback's worth at a time. */
static void device_consume(struct device *d, gint64 *next)
{
	while (fake_now >= *next) {
		d->total += DEVICE_CHUNK;
		if (d->queued < DEVICE_CHUNK) {
			d->underrun += DEVICE_CHUNK - d->queued;
			d->queued = 0;
		} else {
			d->queued -= DEVICE_CHUNK;
		}
		*next += (gint64)DEVICE_CHUNK * G_USEC_PER_SEC / RATE;
	}
	d->queue_sum += d->queued;
	d->queue_checks++;
}

struct audio_result {
	double dead_air_ms, concealed_ms, queue_ms, loss, delay_ms;
	guint64 late, fec;
};

static void audio_arrive(const guint8 *packet, gsize size, gint64 arrival,
			 gpointer data)
{
	line_audio_receive_at(data, packet, size, arrival);
}

static struct audio_result run_audio(const struct link_profile *profile)
{
	g_autoptr(GError) e = NULL;
	const gint64 start = fake_now = 100 * G_USEC_PER_SEC;
	const gint64 end = start + AUDIO_SECONDS * G_USEC_PER_SEC;
	struct link link;
	LineAudio *a = line_audio_open(key_a, key_b, 96, 201, 202, &e);

	g_assert_no_error(e);
	LineAudio *b = line_audio_open(key_b, key_a, 96, 202, 201, &e);

	g_assert_no_error(e);
	/* Device 2 is the receiver's speaker: a opens 1 and 2... find it. */
	guint speaker = 0;

	for (guint i = MAX_DEVICES - 1; i > 0; i--)
		if (devices[i].used && !devices[i].capture) {
			speaker = i;
			break;
		}
	g_assert_cmpuint(speaker, >, 0);
	link_init(&link, profile, 23);
	gint64 next_capture = start, next_chunk = start;
	struct device *d = &devices[speaker];
	guint64 underrun_before = 0, total_before = 0;
	struct line_audio_stats before = { 0 }, stats;
	gboolean measuring = FALSE;

	for (; fake_now < end; fake_now += 1000) {
		if (!measuring && fake_now >= start + G_USEC_PER_SEC) {
			measuring = TRUE;
			underrun_before = d->underrun;
			total_before = d->total;
			line_audio_stats(b, &before);
			d->queue_sum = d->queue_checks = 0;
		}
		/*
		 * Both ends run the same call thread: while it encodes or
		 * decodes a picture, it neither captures, reads the socket
		 * nor feeds the speaker. The devices keep running.
		 */
		gboolean busy =
			thread_busy(profile->load, (fake_now - start) / 1000);

		if (!busy && fake_now >= next_capture) {
			guint8 packet[2048];
			int n = line_audio_capture(a, packet, sizeof(packet),
						   &e);

			g_assert_no_error(e);
			if (n > 0)
				link_send(&link, packet, n);
			next_capture =
				n ? MAX(fake_now + 4000, next_capture + 20000) :
				    fake_now + 4000;
		}
		if (!busy) {
			link_deliver(&link, audio_arrive, b);
			line_audio_playout(b, fake_now);
		}
		device_consume(d, &next_chunk);
	}
	line_audio_stats(b, &stats);
	stats.silence -= before.silence;
	stats.concealed -= before.concealed;
	stats.fec -= before.fec;
	stats.late -= before.late;
	stats.delay_ms_sum -= before.delay_ms_sum;
	stats.delay_count -= before.delay_count;
	double minutes = (double)(d->total - total_before) / RATE / 60;
	struct audio_result r = {
		.dead_air_ms = ((d->underrun - underrun_before) +
				(double)stats.silence) /
			       48 / minutes,
		.concealed_ms = stats.concealed / 48.0 / minutes,
		.queue_ms = d->queue_checks ? (double)d->queue_sum /
						      d->queue_checks / 48 :
					      0,
		.loss = link.sent ? 100.0 * (link.lost + link.queue_drops) /
					    link.sent :
				    0,
		.late = stats.late,
		.fec = stats.fec,
		.delay_ms = stats.delay_count ? (double)stats.delay_ms_sum /
							stats.delay_count :
						0,
	};
	g_test_message(
		"audio %-18s dead-air-ms/min=%7.1f concealed-ms/min=%7.1f fec=%3" G_GUINT64_FORMAT
		" late=%4" G_GUINT64_FORMAT
		" playout-delay-ms=%6.1f path-loss=%4.1f%%",
		profile->name, r.dead_air_ms, r.concealed_ms, r.fec, r.late,
		r.delay_ms, r.loss);
	link_clear(&link);
	line_audio_close(a);
	line_audio_close(b);
	return r;
}

/* --- Scenarios -------------------------------------------------------- */

static const struct link_profile clean = {
	.name = "clean",
	.delay_ms = 30,
	.jitter_ms = 2,
	.min_fps = 14.5,
	.min_screen_psnr = 44,
	.max_freeze_ms = 0,
	.max_dead_air_ms = 1,
	.max_delay_ms = 65,
};

static const struct link_profile lossy = {
	.name = "burst-loss-5%",
	.loss = 0.02,
	.burst_enter = 0.01,
	.burst_leave = 0.35,
	.burst_loss = 0.9,
	.delay_ms = 40,
	.jitter_ms = 10,
	.min_fps = 13,
	.min_screen_psnr = 42,
	.max_freeze_ms = 4000,
	.max_dead_air_ms = 50,
	.max_delay_ms = 150,
};

static const struct link_profile wifi = {
	.name = "wifi-jitter",
	.loss = 0.01,
	.delay_ms = 20,
	.jitter_ms = 40,
	.reorder = TRUE,
	.spike_every_ms = 2000,
	.spike_ms = 120,
	.min_fps = 13,
	.min_screen_psnr = 42,
	.max_freeze_ms = 3000,
	.max_dead_air_ms = 50,
	.max_delay_ms = 170,
};

static const struct link_profile capped = {
	.name = "cap-400k",
	.delay_ms = 40,
	.jitter_ms = 4,
	.kbps = 400,
	.queue_ms = 250,
	.min_fps = 13,
	.min_screen_psnr = 38,
	.max_freeze_ms = 2500,
};

static const struct link_profile fluctuating = {
	.name = "fluctuating-1.2M",
	.delay_ms = 40,
	.jitter_ms = 4,
	.kbps = 1200,
	.queue_ms = 250,
	.low_kbps = 300,
	.low_from_ms = 6000,
	.low_to_ms = 13000,
	.min_fps = 11,
	.min_screen_psnr = 33.5,
	.max_freeze_ms = 4000,
};

static const struct link_profile outage = {
	.name = "outage-600ms",
	.delay_ms = 40,
	.jitter_ms = 4,
	.outage_from_ms = 8000,
	.outage_ms = 600,
	.min_fps = 13.5,
	.min_screen_psnr = 43,
	.max_freeze_ms = 1200,
	.max_recovery_ms = 600,
};

static const struct link_profile hotspot = {
	.name = "mobile-hotspot",
	.loss = 0.01,
	.burst_enter = 0.005,
	.burst_leave = 0.3,
	.burst_loss = 0.8,
	.delay_ms = 70,
	.jitter_ms = 50,
	.reorder = TRUE,
	.kbps = 1000,
	.queue_ms = 400,
	.low_kbps = 450,
	.low_from_ms = 9000,
	.low_to_ms = 15000,
	.min_fps = 13,
	.min_screen_psnr = 38,
	.max_freeze_ms = 5000,
	.max_dead_air_ms = 50,
	.max_delay_ms = 150,
};

/*
 * A weak Wi-Fi uplink, as from the far side of a flat: the radio drops to
 * a low rate and the driver and air queues hold up to 600 ms. The kernel
 * sees that queue in the socket's send buffer, before anything is lost.
 */
static const struct link_profile uplink = {
	.name = "weak-wifi-uplink",
	.loss = 0.005,
	.delay_ms = 25,
	.jitter_ms = 8,
	.kbps = 700,
	.queue_ms = 600,
	.low_kbps = 260,
	.low_from_ms = 5000,
	.low_to_ms = 13000,
	.local_uplink = TRUE,
	.min_fps = 14,
	.min_screen_psnr = 34,
	.max_freeze_ms = 1000,
	.max_latency_ms = 250,
};

/* The same, with the kernel's send queue ignored, as before. */
static const struct link_profile uplink_blind = {
	.name = "weak-wifi-uplink-blind",
	.loss = 0.005,
	.delay_ms = 25,
	.jitter_ms = 8,
	.kbps = 700,
	.queue_ms = 600,
	.low_kbps = 260,
	.low_from_ms = 5000,
	.low_to_ms = 13000,
	.local_uplink = TRUE,
	.ignore_local_queue = TRUE,
};

static const struct link_profile *const video_profiles[] = {
	&clean,	 &lossy,   &wifi,   &capped,	   &fluctuating,
	&outage, &hotspot, &uplink, &uplink_blind,
};

/*
 * A 720p call: inline, the call thread encodes (about 11 ms of every
 * 33 ms, 40 ms for a keyframe every 3 s) and decodes (3 ms) pictures.
 * With the encoder thread it only decodes, and reads kernel stamps.
 */
static const struct thread_load encode_inline = {
	.busy_ms = 14,
	.every_ms = 33,
	.key_busy_ms = 45,
	.key_every_ms = 3000,
};

static const struct thread_load encode_threaded = {
	.busy_ms = 3,
	.every_ms = 33,
	.key_busy_ms = 8,
	.key_every_ms = 3000,
	.kernel_stamps = TRUE,
};

/* The encoder thread alone, still reading the clock after the socket. */
static const struct thread_load encode_threaded_clock = {
	.busy_ms = 3,
	.every_ms = 33,
	.key_busy_ms = 8,
	.key_every_ms = 3000,
};

static const struct link_profile hd_threaded_clock = {
	.name = "hd-video-threaded-clock",
	.loss = 0.005,
	.delay_ms = 25,
	.jitter_ms = 8,
	.load = &encode_threaded_clock,
};

static const struct link_profile hd_inline = {
	.name = "hd-video-inline",
	.loss = 0.005,
	.delay_ms = 25,
	.jitter_ms = 8,
	.load = &encode_inline,
};

static const struct link_profile hd_threaded = {
	.name = "hd-video-threaded",
	.loss = 0.005,
	.delay_ms = 25,
	.jitter_ms = 8,
	.load = &encode_threaded,
	.max_dead_air_ms = 50,
	.max_delay_ms = 90,
};

static const struct link_profile heavy = {
	.name = "burst-loss-15%",
	.loss = 0.05,
	.burst_enter = 0.04,
	.burst_leave = 0.3,
	.burst_loss = 0.8,
	.delay_ms = 40,
	.jitter_ms = 10,
	.max_dead_air_ms = 50,
	.max_delay_ms = 150,
};

static const struct link_profile *const audio_profiles[] = {
	&clean, &lossy, &heavy, &wifi, &hotspot, &hd_threaded,
};

static void video_scenario(gconstpointer data)
{
	const struct link_profile *p = data;
	struct video_result r = run_video(p);

	if (p->ignore_local_queue)
		return;
	g_assert_cmpfloat(r.fps, >=, p->min_fps);
	g_assert_cmpfloat(r.screen_psnr, >=, p->min_screen_psnr);
	g_assert_cmpint(r.freeze_ms, <=, p->max_freeze_ms);
	if (p->max_latency_ms)
		g_assert_cmpuint(r.latency_p95_ms, <=, p->max_latency_ms);
	if (p->max_recovery_ms) {
		g_assert_cmpint(r.recovery_ms, >=, 0);
		g_assert_cmpint(r.recovery_ms, <=, p->max_recovery_ms);
	}
}

static void audio_scenario(gconstpointer data)
{
	const struct link_profile *p = data;
	struct audio_result r = run_audio(p);

	g_assert_cmpfloat(r.dead_air_ms, <=, p->max_dead_air_ms);
	g_assert_cmpfloat(r.delay_ms, <=, p->max_delay_ms);
}

/*
 * The kernel's send queue lets a weak uplink keep latency low: compared
 * with the same run that ignores it, delay drops and fps does not.
 */
static void uplink_compare(void)
{
	struct video_result blind = run_video(&uplink_blind);
	struct video_result seen = run_video(&uplink);

	g_test_message(
		"weak uplink latency p95 %u -> %u ms, mean %.0f -> %.0f ms, fps %.1f -> %.1f",
		blind.latency_p95_ms, seen.latency_p95_ms, blind.latency_ms,
		seen.latency_ms, blind.fps, seen.fps);
	g_assert_cmpuint(seen.latency_p95_ms * 3, <=, blind.latency_p95_ms * 2);
	g_assert_cmpfloat(seen.fps, >=, blind.fps - 1);
	g_assert_cmpuint(seen.local_cuts, >, 0);
}

/* The encoder thread and kernel stamps against encoding inline. */
static void hd_compare(void)
{
	struct audio_result before = run_audio(&hd_inline);
	struct audio_result thread = run_audio(&hd_threaded_clock);
	struct audio_result after = run_audio(&hd_threaded);

	g_test_message(
		"720p call audio: playout delay %.1f -> %.1f (thread alone %.1f) ms, dead air %.1f -> %.1f (%.1f) ms/min, concealed %.0f -> %.0f (%.0f) ms/min",
		before.delay_ms, after.delay_ms, thread.delay_ms,
		before.dead_air_ms, after.dead_air_ms, thread.dead_air_ms,
		before.concealed_ms, after.concealed_ms, thread.concealed_ms);
	g_assert_cmpfloat(after.delay_ms + 10, <, before.delay_ms);
	g_assert_cmpfloat(after.dead_air_ms, <=, before.dead_air_ms + 1);
}

int main(int argc, char **argv)
{
	g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	g_setenv("LINE_NOISE_SUPPRESSION", "0", TRUE);
	g_test_init(&argc, &argv, NULL);
	for (guint i = 0; i < G_N_ELEMENTS(video_profiles); i++) {
		g_autofree char *path = g_strdup_printf(
			"/net/video/%s", video_profiles[i]->name);

		g_test_add_data_func(path, video_profiles[i], video_scenario);
	}
	for (guint i = 0; i < G_N_ELEMENTS(audio_profiles); i++) {
		g_autofree char *path = g_strdup_printf(
			"/net/audio/%s", audio_profiles[i]->name);

		g_test_add_data_func(path, audio_profiles[i], audio_scenario);
	}
	g_test_add_func("/net/compare/weak-wifi-uplink", uplink_compare);
	g_test_add_func("/net/compare/hd-video-audio", hd_compare);
	return g_test_run();
}
