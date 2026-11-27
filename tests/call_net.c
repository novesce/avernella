// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Bad-network harness for call media. Real VP8 video and Opus audio
 * streams cross a simulated path with burst loss, delay, jitter,
 * reordering, a bandwidth-limited bottleneck queue and outages, on a
 * virtual clock. Run with --verbose to print the quality metrics.
 */
#include "call_audio.h"
#include "call_denoise.h"
#include "call_srtp.h"
#include "call_video.h"
#include <SDL.h>
#include <gmodule.h>
#include <math.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static gint64 fake_now = 1000000;
static guint32 channel_seed;

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
	const struct link_profile *reverse;
	gboolean hd;
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
			    .rand = g_rand_new_with_seed(seed ^ channel_seed),
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
	guint latency_count, latencies[4096];
	int width, height, fps;
};

static guint tri(int a)
{
	return ABS((a & 511) - 256);
}

/* Moving gradients and textured boxes, with the frame index in the top band. */
static void source_frame(const struct video_run *run, guint index, guint8 *y,
			 guint8 *u, guint8 *v)
{
	static const int box[3][4] = { { 40, 60, 5, 3 },
				       { 400, 120, -4, 2 },
				       { 250, 200, 3, -5 } };
	int bx[3], by[3];

	for (int b = 0; b < 3; b++) {
		int span_x = run->width - 96,
		    span_y = run->height - MARK_ROWS - 72;
		int px = (box[b][0] + box[b][2] * (int)index) % (2 * span_x);
		int py = (box[b][1] + box[b][3] * (int)index) % (2 * span_y);

		px = px < 0 ? px + 2 * span_x : px;
		py = py < 0 ? py + 2 * span_y : py;
		bx[b] = px < span_x ? px : 2 * span_x - px;
		by[b] = MARK_ROWS + (py < span_y ? py : 2 * span_y - py);
	}
	for (int r = 0; r < run->height; r++) {
		for (int c = 0; c < run->width; c++) {
			guint8 *p = y + r * run->width + c;

			if (r < MARK_ROWS) {
				int block = c * MARK_BLOCKS / run->width;
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
	for (int r = 0; r < run->height / 2; r++) {
		for (int c = 0; c < run->width / 2; c++) {
			u[r * run->width / 2 + c] =
				112 + tri(c * 4 + index * 4) / 8;
			v[r * run->width / 2 + c] =
				144 - tri(r * 4 - index * 2) / 8;
		}
	}
}

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
	double sum = 0, sx = (double)run->screen_w / run->width,
	       sy = (double)run->screen_h / run->height;
	guint count = 0;

	for (int r = MARK_ROWS; r < run->height; r += 2) {
		for (int c = 0; c < run->width; c += 2) {
			int d = sample(run->screen, run->screen_w,
				       run->screen_h, (c + 0.5) * sx - 0.5,
				       (r + 0.5) * sy - 0.5) -
				source[r * run->width + c];

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
			run->cache[slot] =
				g_malloc(run->width * run->height * 3 / 2);
		source_frame(run, index, run->cache[slot],
			     run->cache[slot] + run->width * run->height,
			     run->cache[slot] +
				     run->width * run->height * 5 / 4);
		run->cache_index[slot] = index;
	}
	return run->cache[slot];
}

static gboolean read_index(const struct video_run *run, guint *index)
{
	guint bits = 0;

	for (guint b = 0; b < MARK_BLOCKS; b++) {
		double x = (b + 0.5) * run->width / MARK_BLOCKS *
			   run->screen_w / run->width;
		double y = MARK_ROWS / 2.0 * run->screen_h / run->height;
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
	source_frame(run, index, run->source,
		     run->source + run->width * run->height,
		     run->source + run->width * run->height * 5 / 4);
	if (fake_now < run->measure_from)
		return;
	run->shown++;
	run->decoded_psnr += psnr(run, run->source);
	gint64 latency = fake_now - run->captured[index & 4095];

	if (run->captured[index & 4095] && latency >= 0) {
		run->latency_sum += latency;
		g_assert_cmpuint(run->latency_count, <,
				 G_N_ELEMENTS(run->latencies));
		run->latencies[run->latency_count++] = latency / 1000;
	}
	gint64 gap = fake_now - MAX(run->last_shown, run->measure_from);
	gint64 frame_us = G_USEC_PER_SEC / run->fps;

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

static int compare_latency(const void *a, const void *b)
{
	const guint *x = a, *y = b;

	return (*x > *y) - (*x < *y);
}

/* Nearest rank; every observed frame, including multi-second stalls. */
static guint latency_percentile(const struct video_run *run, guint percentile)
{
	guint rank = (run->latency_count * percentile + 99) / 100;

	return rank ? run->latencies[rank - 1] : 0;
}

struct video_result {
	double fps, decoded_psnr, screen_psnr, kbps, latency_ms;
	guint latency_p50_ms, latency_p95_ms, latency_p99_ms, local_cuts;
	gint64 freeze_ms, recovery_ms;
	guint freezes, firs;
	double loss;
};

static const guint8 key_a[30] = { 1, 2, 3 }, key_b[30] = { 4, 5, 6 };

static struct video_result run_video(const struct link_profile *profile)
{
	struct video_run run = { .profile = profile,
				 .width = profile->hd ? 1280 : VW,
				 .height = profile->hd ? 720 : VH,
				 .fps = profile->hd ? 30 : VFPS };
	guint kbps = profile->hd ? 3000 : VKBPS;
	const gint64 start = fake_now = 10 * G_USEC_PER_SEC;
	const gint64 end = start + VIDEO_SECONDS * G_USEC_PER_SEC;
	guint8 *frame = g_malloc(run.width * run.height * 3 / 2);
	gint64 next_frame = start,
	       next_tick = start + G_USEC_PER_SEC / run.fps / 2;
	guint index = 0;
	struct line_camera_frame image = {
		.plane = { frame, frame + run.width * run.height,
			   frame + run.width * run.height * 5 / 4 },
		.stride = { run.width, run.width / 2, run.width / 2 },
		.width = run.width,
		.height = run.height,
	};
	g_autoptr(GError) e = NULL;

	run.screen = g_malloc(1920 * 1080);
	run.source = g_malloc(run.width * run.height * 3 / 2);
	run.measure_from = start + G_USEC_PER_SEC;
	run.tx = call_video_open(key_a, key_b, 97, 101, 102, kbps, run.fps, &e);
	g_assert_no_error(e);
	run.rx = call_video_open(key_b, key_a, 97, 102, 101, kbps, run.fps, &e);
	g_assert_no_error(e);
	link_init(&run.forward, profile, 7);
	link_init(&run.back, profile->reverse ? profile->reverse : profile, 11);
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
			source_frame(&run, index, frame,
				     frame + run.width * run.height,
				     frame + run.width * run.height * 5 / 4);
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
			next_frame += G_USEC_PER_SEC / run.fps;
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
				run.fps / G_USEC_PER_SEC;

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
			next_tick += G_USEC_PER_SEC / run.fps;
		}
	}
	gint64 tail = end - MAX(run.last_shown, run.measure_from);

	if (tail > 3 * G_USEC_PER_SEC / run.fps) {
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
	qsort(run.latencies, run.latency_count, sizeof(run.latencies[0]),
	      compare_latency);
	r.latency_ms = run.latency_count ?
			       run.latency_sum / (1000.0 * run.latency_count) :
			       0;
	r.latency_p50_ms = latency_percentile(&run, 50);
	r.latency_p95_ms = latency_percentile(&run, 95);
	r.latency_p99_ms = latency_percentile(&run, 99);
	struct call_video_recovery tx, rx;

	call_video_recovery(run.tx, &tx);
	call_video_recovery(run.rx, &rx);
	r.local_cuts = tx.local_cuts;
	g_test_message(
		"video %-18s fps=%5.2f freeze-ms=%6" G_GINT64_FORMAT
		" freezes=%3u psnr-decoded=%5.2f psnr-screen=%5.2f recovery-ms=%5" G_GINT64_FORMAT
		" sent-kbps=%6.1f path-loss=%4.1f%% keyframe-requests=%u latency-ms=%6.1f p50/p95/p99-ms=%u/%u/%u captures=%u decoded=%u",
		profile->name, r.fps, r.freeze_ms, r.freezes, r.decoded_psnr,
		r.screen_psnr, r.recovery_ms, r.kbps, r.loss, r.firs,
		r.latency_ms, r.latency_p50_ms, r.latency_p95_ms,
		r.latency_p99_ms, index, run.decoded_frames);
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
	PROBE_HZ = 3000,
	PROBE_LEVEL = 6000,
	AUDIO_SECONDS = 30,
	MAX_DEVICES = 8,
};

struct device {
	gboolean used, capture, paused;
	guint64 consumed; /* capture sample frames handed out */
	guint channels, quantum;
	SDL_AudioCallback input;
	void *userdata;
	GArray *wave; /* synthetic speaker samples on the capture clock */
	gint64 epoch;
	guint64 queued; /* playback samples waiting */
	guint64 underrun, total, queue_sum, queue_checks;
};

static struct device devices[MAX_DEVICES];
static gboolean reject_stereo;

/* Quality tests rely on audible pilots. Only host service benchmarks
 * include the production RNNoise filter in their CPU measurements.
 */
static gboolean measure_denoise;
struct line_denoise *__real_line_denoise_new(void);
G_MODULE_EXPORT struct line_denoise *__wrap_line_denoise_new(void)
{
	return measure_denoise ? __real_line_denoise_new() : NULL;
}

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
	if (obtained)
		*obtained = *desired;
	(void)changes;
	if (capture && reject_stereo && desired->channels > 1)
		return 0;
	for (guint i = 1; i < MAX_DEVICES; i++) {
		if (devices[i].used)
			continue;
		devices[i] = (struct device){ .used = TRUE,
					      .capture = !!capture,
					      .channels = desired->channels,
					      .quantum = desired->samples,
					      .input = desired->callback,
					      .userdata = desired->userdata,
					      .epoch = fake_now };
		return i;
	}
	return 0;
}

G_MODULE_EXPORT void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID id)
{
	if (id < MAX_DEVICES) {
		g_clear_pointer(&devices[id].wave, g_array_unref);
		devices[id].used = FALSE;
	}
}

G_MODULE_EXPORT void __wrap_SDL_PauseAudioDevice(SDL_AudioDeviceID id,
						 int pause)
{
	devices[id].paused = pause;
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
	struct device *d = &devices[id];
	guint pos = d->total + d->queued;

	if (d->wave) {
		g_array_set_size(d->wave, pos + len / 2);
		memcpy(&g_array_index(d->wave, gint16, pos), pcm, len);
	}
	d->queued += len / 2;
	return 0;
}

G_MODULE_EXPORT Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID id)
{
	struct device *d = &devices[id];

	if (!d->capture)
		return d->queued * 2;
	guint64 produced = (fake_now - d->epoch) * RATE / G_USEC_PER_SEC;

	produced -= produced % d->quantum;
	return produced > d->consumed ?
		       (produced - d->consumed) * 2 * d->channels :
		       0;
}

/* A voiced, syllable-shaped signal: harmonics under a 4 Hz envelope. */
G_MODULE_EXPORT Uint32 __wrap_SDL_DequeueAudio(SDL_AudioDeviceID id, void *pcm,
					       Uint32 len)
{
	struct device *d = &devices[id];
	gint16 *out = pcm;

	for (Uint32 i = 0; i < len / 2; d->consumed++) {
		double t = (double)d->consumed / RATE;
		double envelope = 0.5 + 0.5 * sin(2 * G_PI * 4 * t);
		double s = 0;

		for (int h = 1; h <= 6; h++)
			s += sin(2 * G_PI * 140 * h * t) / h;
		double sample = 4000 * envelope * s;

		/* One 40 ms pilot each second, distinguishable from speech. */
		if (d->consumed % RATE < RATE / 25)
			sample += PROBE_LEVEL * sin(2 * G_PI * PROBE_HZ * t);
		for (guint ch = 0; ch < d->channels; ch++)
			out[i++] = sample;
	}
	return len;
}

/* The playback device takes one callback's worth at a time. */
static void feed_microphones(void)
{
	gint16 pcm[2 * 1024];

	for (guint id = 1; id < MAX_DEVICES; id++) {
		struct device *d = &devices[id];

		if (!d->used || !d->capture || !d->input || d->paused)
			continue;
		guint n = d->quantum * d->channels * sizeof(*pcm);

		g_assert_cmpuint(n, <=, sizeof(pcm));
		while (__wrap_SDL_GetQueuedAudioSize(id) >= n) {
			__wrap_SDL_DequeueAudio(id, pcm, n);
			d->input(d->userdata, (Uint8 *)pcm, n);
		}
	}
}

static void device_consume(struct device *d, gint64 *next)
{
	while (fake_now >= *next) {
		d->total += d->quantum;
		if (d->queued < d->quantum) {
			d->underrun += d->quantum - d->queued;
			d->queued = 0;
		} else {
			d->queued -= d->quantum;
		}
		*next = d->epoch + d->total * G_USEC_PER_SEC / RATE;
	}
	d->queue_sum += d->queued;
	d->queue_checks++;
}

struct audio_result {
	double dead_air_ms, concealed_ms, queue_ms, loss, delay_ms, late_pc;
	guint p50, p95, p99, probes;
	guint64 late, fec, fec_attempts;
	gboolean can_count_lbrr;
	guint loss_reports, min_loss_pct, max_loss_pct;
};

/*
 * Scan the decoded synthetic speaker waveform in 1 ms steps, using 4 ms
 * of coherent integration to reject speech harmonics. Include actual
 * packetisation, codec delay, playout and device drain. Mark a missing
 * probe as lost rather than assigning the next marker to it.
 */
static void acoustic_latency(const struct device *d, guint *p50, guint *p95,
			     guint *p99, guint *received)
{
	enum {
		PERIOD = RATE / PROBE_HZ,
		WINDOW = 4 * RATE / 1000
	};
	double cosine[PERIOD], sine[PERIOD];
	guint delays[AUDIO_SECONDS];
	const gint16 *wave = (gint16 *)d->wave->data;

	for (guint i = 0; i < PERIOD; i++) {
		cosine[i] = cos(2 * G_PI * i / PERIOD);
		sine[i] = sin(2 * G_PI * i / PERIOD);
	}
	for (guint sec = 1; sec < AUDIO_SECONDS; sec++) {
		guint start = sec * RATE;
		guint end = MIN((sec + 1) * RATE, d->total);

		for (guint pos = start;
		     pos + WINDOW <= end && pos + WINDOW <= d->wave->len;
		     pos += RATE / 1000) {
			double x = 0, y = 0;

			for (guint j = 0; j < WINDOW; j++) {
				x += wave[pos + j] * cosine[j % PERIOD];
				y += wave[pos + j] * sine[j % PERIOD];
			}
			/* Magnitude corresponding to 1/4 of the pilot at source. */
			double threshold = PROBE_LEVEL * WINDOW / 8;

			if (x * x + y * y < threshold * threshold)
				continue;
			delays[(*received)++] = (pos - start) / (RATE / 1000);
			break;
		}
	}
	g_assert_cmpuint(*received, >, 0);
	qsort(delays, *received, sizeof(*delays), compare_latency);
	*p50 = delays[(*received * 50 + 99) / 100 - 1];
	*p95 = delays[(*received * 95 + 99) / 100 - 1];
	*p99 = delays[(*received * 99 + 99) / 100 - 1];
}

struct audio_receiver {
	LineAudio *audio;
	guint arrived;
};

static void audio_arrive(const guint8 *packet, gsize size, gint64 arrival,
			 gpointer data)
{
	struct audio_receiver *rx = data;

	rx->arrived++;
	line_audio_receive_at(rx->audio, packet, size, arrival);
}

struct audio_feedback {
	LineAudio *sender;
	guint reports, min_loss_pct, max_loss_pct;
};

static void audio_report_arrive(const guint8 *packet, gsize size,
				gint64 received G_GNUC_UNUSED, gpointer data)
{
	struct audio_feedback *f = data;
	struct line_audio_stats stats = { 0 };

	g_assert_true(line_audio_receive_report(f->sender, packet, size));
	line_audio_stats(f->sender, &stats);
	f->min_loss_pct = MIN(f->min_loss_pct, stats.expected_loss_pct);
	f->max_loss_pct = MAX(f->max_loss_pct, stats.expected_loss_pct);
	f->reports++;
}

static struct audio_result run_audio(const struct link_profile *profile,
				     gboolean conference, gboolean feedback)
{
	g_autoptr(GError) e = NULL;
	const gint64 start = fake_now = 100 * G_USEC_PER_SEC;
	const gint64 end = start + AUDIO_SECONDS * G_USEC_PER_SEC;
	struct link link, back;
	LineAudio *a = conference ?
			       line_audio_open_group(key_a, key_b, 96, 96, 201,
						     202, &e) :
			       line_audio_open(key_a, key_b, 96, 201, 202, &e);

	g_assert_no_error(e);
	LineAudio *b = conference ?
			       line_audio_open_group(key_b, key_a, 96, 96, 202,
						     201, &e) :
			       line_audio_open(key_b, key_a, 96, 202, 201, &e);

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
	link_init(&back, profile->reverse ? profile->reverse : profile, 11);
	struct audio_feedback reports = { .sender = a, .min_loss_pct = 100 };
	gint64 next_capture = start, next_chunk = start;
	struct device *d = &devices[speaker];

	d->wave = g_array_new(FALSE, TRUE, sizeof(gint16));
	guint64 underrun_before = 0, total_before = 0;
	struct line_audio_stats before = { 0 }, stats;
	gboolean measuring = FALSE;
	struct audio_receiver rx = { .audio = b };

	for (; fake_now < end; fake_now += 1000) {
		feed_microphones();
		if (!measuring && fake_now >= start + G_USEC_PER_SEC) {
			measuring = TRUE;
			underrun_before = d->underrun;
			total_before = d->total;
			rx.arrived = 0;
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

		gboolean ready = line_audio_capture_fd(a) >= 0 ?
					 line_audio_capture_ready(a) :
					 fake_now >= next_capture;
		if (!busy && ready) {
			guint8 packet[2048];
			int n = line_audio_capture(a, packet, sizeof(packet),
						   &e);

			g_assert_no_error(e);
			if (n > 0)
				link_send(&link, packet, n);
			next_capture =
				n ? MAX(fake_now + 4000,
					next_capture +
						(conference ? 40000 : 20000)) :
				    fake_now + 4000;
		}
		if (!busy) {
			guint arrived = rx.arrived;

			link_deliver(&link, audio_arrive, &rx);
			if (feedback && arrived != rx.arrived) {
				guint8 packet[512];
				gsize size = line_audio_receiver_report(
					b, fake_now, packet, sizeof(packet));
				if (size)
					link_send(&back, packet, size);
			}
			if (feedback)
				link_deliver(&back, audio_report_arrive,
					     &reports);
			line_audio_playout(b, fake_now);
		}
		device_consume(d, &next_chunk);
	}
	line_audio_stats(b, &stats);
	stats.silence -= before.silence;
	stats.concealed -= before.concealed;
	stats.fec -= before.fec;
	stats.fec_attempts -= before.fec_attempts;
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
		.late_pc = 100.0 * stats.late / MAX(1, rx.arrived),
		.fec = stats.fec,
		.fec_attempts = stats.fec_attempts,
		.can_count_lbrr = stats.can_count_lbrr,
		.loss_reports = reports.reports,
		.min_loss_pct = reports.min_loss_pct,
		.max_loss_pct = reports.max_loss_pct,
		.delay_ms = stats.delay_count ? (double)stats.delay_ms_sum /
							stats.delay_count :
						0,
	};
	acoustic_latency(d, &r.p50, &r.p95, &r.p99, &r.probes);
	g_test_message(
		"synthetic %s microphone-to-speaker p50/p95/p99=%u/%u/%u ms probes=%u/%u late=%.2f%%",
		conference ? "group" : "direct", r.p50, r.p95, r.p99, r.probes,
		AUDIO_SECONDS - 1, r.late_pc);
	g_test_message(
		"audio %-18s dead-air-ms/min=%7.1f concealed-ms/min=%7.1f fec=%3" G_GUINT64_FORMAT
		" late=%4" G_GUINT64_FORMAT
		" playout-delay-ms=%6.1f path-loss=%4.1f%%",
		profile->name, r.dead_air_ms, r.concealed_ms, r.fec, r.late,
		r.delay_ms, r.loss);
	if (!conference)
		g_test_message(
			"Protected loss reports=%u encoder loss=%u..%u%% confirmed LBRR=%" G_GUINT64_FORMAT
			"/%" G_GUINT64_FORMAT " repair attempts (%s)",
			r.loss_reports, r.loss_reports ? r.min_loss_pct : 5,
			r.loss_reports ? r.max_loss_pct : 5, r.fec,
			r.fec_attempts,
			r.can_count_lbrr ? "packet inspection available" :
					   "packet inspection unavailable");
	link_clear(&back);
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

/* Deterministic GE channel: bad state drops all packets. A mean burst of
 * 4 gives P(bad -> good)=1/4. P(good -> bad)=1/12 yields a stationary
 * loss probability of (1/12)/(1/12 + 1/4) = 25%. Keep the receiver's
 * feedback link independent from the throttled, lossy camera uplink.
 */
static const struct link_profile feedback_link = {
	.delay_ms = 15,
	.jitter_ms = 5,
	.kbps = 128,
	.queue_ms = 50,
};
static const struct link_profile asymmetric_burst = {
	.name = "asymmetric-burst-25%",
	.max_dead_air_ms = 600, /* one percent of a minute */
	.max_delay_ms = 150,
	.burst_enter = 1.0 / 12,
	.burst_leave = 0.25,
	.burst_loss = 1.0,
	.delay_ms = 40,
	.jitter_ms = 40,
	.reorder = TRUE,
	.kbps = 2400,
	.queue_ms = 200,
	.reverse = &feedback_link,
	.hd = TRUE,
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

static void video_hd_burst(void)
{
	struct video_result r = run_video(&asymmetric_burst);

	/* This profile deliberately overloads the path. Check recovery and
	 * that the observed packet losses agree with the channel model.
	 */
	g_assert_cmpfloat(r.loss, >, 20.0);
	g_assert_cmpfloat(r.loss, <, 35.0);
	/* Prior controller: 13.68 fps, 26.47 dB on screen, p99 608 ms. */
	g_assert_cmpfloat(r.fps, >=, 14);
	g_assert_cmpfloat(r.screen_psnr, >=, 26);
	g_assert_cmpuint(r.latency_p99_ms, <=, 608);
}

static void audio_scenario(gconstpointer data)
{
	const struct link_profile *p = data;
	struct audio_result r = run_audio(p, FALSE, TRUE);

	if (p == &asymmetric_burst) {
		struct audio_result fixed = run_audio(p, FALSE, FALSE);

		g_assert_cmpuint(r.loss_reports, >, 0);
		g_assert_cmpuint(r.max_loss_pct, >, 5);
		g_assert_cmpuint(fixed.loss_reports, ==, 0);
		if (r.can_count_lbrr)
			g_assert_cmpuint(r.fec, >, 0);
	}
	g_assert_cmpfloat(r.dead_air_ms, <=, p->max_dead_air_ms);
	if (p->max_delay_ms)
		g_assert_cmpfloat(r.delay_ms, <=, p->max_delay_ms);
	g_assert_cmpfloat(r.late_pc, <=, 1.0);
	g_assert_cmpuint(r.probes, >=, (AUDIO_SECONDS - 1) / 2);
	if (p == &clean)
		g_assert_cmpuint(r.p95, <=, p->delay_ms + 4 * 20);
}

static guint32 audio_stamp(const guint8 *rtp)
{
	guint32 stamp;

	memcpy(&stamp, rtp + 4, sizeof(stamp));
	return GUINT32_FROM_BE(stamp);
}

/* Drive the SDL callback directly, including fragments smaller than a PCM
 * sample. No physical audio device is opened by this clocked harness.
 */
static void capture_ring(gconstpointer mode)
{
	guint channels = GPOINTER_TO_UINT(mode);
	gboolean group = channels != 0;

	reject_stereo = channels == 1;
	g_autoptr(GError) e = NULL;
	LineAudio *audio =
		group ? line_audio_open_group(key_a, key_b, 96, 96, 201, 202,
					      &e) :
			line_audio_open(key_a, key_b, 96, 201, 202, &e);
	g_assert_no_error(e);
	struct device *dev = NULL;

	for (guint i = 0; i < MAX_DEVICES; i++)
		if (devices[i].used && devices[i].capture)
			dev = &devices[i];
	g_assert_nonnull(dev);
	g_assert_nonnull(dev->input);
	g_assert_cmpuint(dev->channels, ==, MAX(1u, channels));
	guint samples = group ? 1920 : 960;
	guint bytes = samples * dev->channels * sizeof(gint16);
	guint8 tone[2 * 1920 * sizeof(gint16)], packet[2048];

	memset(tone, 0x11, bytes);
	struct pollfd wake = { .fd = line_audio_capture_fd(audio),
			       .events = POLLIN };

	g_assert_cmpint(wake.fd, >=, 0);
	dev->input(dev->userdata, tone, bytes - 1);
	g_assert_false(line_audio_capture_ready(audio));
	g_assert_cmpint(poll(&wake, 1, 0), ==, 0);
	dev->input(dev->userdata, tone + bytes - 1, 1);
	g_assert_cmpint(poll(&wake, 1, 0), ==, 1);
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			>, 0);
	guint32 first = audio_stamp(packet);

	g_assert_cmpint(poll(&wake, 1, 0), ==, 0);

	/* Rotate through the four slots without losing media time. */
	for (int i = 1; i <= 12; i++) {
		dev->input(dev->userdata, tone, bytes);
		g_assert_cmpint(line_audio_capture(audio, packet,
						   sizeof(packet), &e),
				>, 0);
		g_assert_cmpuint(audio_stamp(packet) - first, ==, i * samples);
	}
	/* Six additional completed frames cannot fit in four slots. */
	for (int i = 0; i < 6; i++)
		dev->input(dev->userdata, tone, bytes);
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			==, 0);
	g_assert_false(line_audio_capture_ready(audio));
	dev->input(dev->userdata, tone, bytes);
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			>, 0);
	g_assert_cmpuint(audio_stamp(packet) - first, ==, 19 * samples);

	/* Mute purges complete frames, the partially assembled slot and wake. */
	dev->input(dev->userdata, tone, bytes);
	dev->input(dev->userdata, tone, bytes / 2);
	line_audio_mute(audio, TRUE);
	g_assert_true(dev->paused);
	g_assert_false(line_audio_capture_ready(audio));
	g_assert_cmpint(poll(&wake, 1, 0), ==, 0);
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			==, 0);
	line_audio_mute(audio, FALSE);
	memset(tone, 0, bytes);
	dev->input(dev->userdata, tone, bytes / 2);
	g_assert_false(line_audio_capture_ready(audio));
	dev->input(dev->userdata, tone, bytes / 2);
	g_assert_cmpint(line_audio_capture(audio, packet, sizeof(packet), &e),
			>, 0);
	if (group)
		g_assert_cmpuint(packet[20], ==, 127);
	line_audio_close(audio);
	reject_stereo = FALSE;
}

/*
 * Measure synchronous Opus service against one persistent codec worker. The
 * condition variable transfers ownership of a single slot, without copying
 * PCM or RTP. The receive side includes due playout and writes decoded PCM
 * to the virtual speaker. The media clock stays deterministic while these
 * measurements use the host CPU and elapsed clocks.
 */
enum audio_job {
	JOB_IDLE,
	JOB_ENCODE,
	JOB_RECEIVE,
	JOB_PLAYOUT,
	JOB_EXIT
};

struct service_slot {
	GMutex lock;
	GCond ready;
	enum audio_job job;
	LineAudio *tx, *rx;
	guint8 packet[4096];
	int size;
	gint64 cpu_ns, next_play;
};

static gint64 service_time(clockid_t clock)
{
	struct timespec t;

	clock_gettime(clock, &t);
	return (gint64)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void do_audio_job(struct service_slot *s, enum audio_job job)
{
	gint64 begin = service_time(CLOCK_THREAD_CPUTIME_ID);

	if (job == JOB_ENCODE)
		s->size = line_audio_capture(s->tx, s->packet,
					     sizeof(s->packet), NULL);
	else {
		if (job == JOB_RECEIVE)
			g_assert_true(line_audio_receive_at(s->rx, s->packet,
							    s->size, fake_now));
		s->next_play = line_audio_playout(s->rx, fake_now);
	}
	s->cpu_ns = service_time(CLOCK_THREAD_CPUTIME_ID) - begin;
}

static gpointer service_worker(gpointer data)
{
	struct service_slot *s = data;

	g_mutex_lock(&s->lock);
	for (;;) {
		while (s->job == JOB_IDLE)
			g_cond_wait(&s->ready, &s->lock);
		if (s->job == JOB_EXIT)
			break;
		do_audio_job(s, s->job);
		s->job = JOB_IDLE;
		g_cond_signal(&s->ready);
	}
	g_mutex_unlock(&s->lock);
	return NULL;
}

static void dispatch_audio_job(struct service_slot *s, enum audio_job job,
			       gboolean threaded)
{
	if (!threaded) {
		do_audio_job(s, job);
		return;
	}
	g_mutex_lock(&s->lock);
	s->job = job;
	g_cond_signal(&s->ready);
	while (s->job != JOB_IDLE && job != JOB_EXIT)
		g_cond_wait(&s->ready, &s->lock);
	g_mutex_unlock(&s->lock);
}

/* A synthetic conference mixer rewraps the microphone payload with a
 * distinct SSRC per child. Relay work is outside the client's timings.
 */
static void mix_service_packet(struct service_slot *s, srtp_t ingest,
			       srtp_t relay, guint voices)
{
	guint8 child[512];
	guint8 *p = s->packet;

	g_assert_cmpint(srtp_unprotect(ingest, p, &s->size), ==,
			srtp_err_status_ok);
	guint ext = 16 + 4 * ((p[14] << 8) + p[15]);

	g_assert_cmpuint(ext, <, (guint)s->size);
	guint child_size = 12 + s->size - ext;

	g_assert_cmpuint(child_size, <=, sizeof(child));
	memcpy(child, p, 12);
	child[0] = 0x80;
	memcpy(child + 12, p + ext, child_size - 12);
	guint words = (2 + 2 * voices + 3) / 4;
	guint pos = 16 + 4 * words;

	g_assert_cmpuint(pos + voices * child_size + SRTP_MAX_TRAILER_LEN, <=,
			 sizeof(s->packet));
	memset(p + 12, 0, pos - 12);
	p[12] = 2;
	p[13] = 0x40;
	p[15] = words;
	p[16] = 3;
	p[17] = voices;
	for (guint n = 0; n < voices; n++) {
		p[18 + n * 2] = 0x40 | child_size >> 8;
		p[19 + n * 2] = child_size;
		child[11] = n + 1;
		memcpy(p + pos, child, child_size);
		pos += child_size;
	}
	s->size = pos;
	g_assert_cmpint(srtp_protect(relay, p, &s->size), ==,
			srtp_err_status_ok);
}

static void audio_service(gconstpointer data)
{
	enum {
		N = 1000,
		WARMUP = 30,
		TX_CPU = 0,
		RX_CPU,
		CALLBACK,
		READY,
		COMBINED,
		METRICS
	};
	guint voices = GPOINTER_TO_UINT(data);
	gboolean group = voices != 0;
	const char *name = voices > 1 ? "XRTP-eight" :
			   group      ? "group" :
					"direct";
	const guint frames = group ? 4 : 2;
	guint us[METRICS][N];

	if (!g_test_perf()) {
		g_test_skip("Run with -m perf to time Opus and SRTP service.");
		return;
	}
	/* The network quality fixtures turn RNNoise off for their pilots. */
	for (guint threaded = 0; threaded <= 1; threaded++) {
		g_autoptr(GError) e = NULL;
		struct service_slot s = { 0 };
		GThread *worker = NULL;
		srtp_t ingest = NULL, relay = NULL;

		measure_denoise = TRUE;
		s.tx = group ? line_audio_open_group(key_a, key_b, 96, 96, 201,
						     202, &e) :
			       line_audio_open(key_a, key_b, 96, 201, 202, &e);
		g_assert_no_error(e);
		s.rx = group ? line_audio_open_group(key_b, key_a, 96, 96, 202,
						     201, &e) :
			       line_audio_open(key_b, key_a, 96, 202, 201, &e);
		g_assert_no_error(e);
		measure_denoise = FALSE;
		if (voices > 1) {
			g_assert_true(call_srtp_open(&ingest, key_a,
						     ssrc_specific, 201));
			g_assert_true(call_srtp_open(&relay, key_a,
						     ssrc_specific, 201));
		}
		struct device *mic = NULL, *speaker = NULL;

		for (guint id = 1; id < MAX_DEVICES; id++) {
			if (!devices[id].used)
				continue;
			if (devices[id].capture && !mic)
				mic = &devices[id];
			if (!devices[id].capture)
				speaker = &devices[id];
		}
		g_assert_nonnull(mic);
		g_assert_nonnull(mic->input);
		g_assert_nonnull(speaker);
		g_assert_cmpuint(mic->quantum, ==, RATE / 100);
		g_assert_cmpuint(speaker->quantum, ==, RATE / 100);
		struct pollfd wake = { .fd = line_audio_capture_fd(s.tx),
				       .events = POLLIN };
		g_assert_cmpint(wake.fd, >=, 0);
		guint bytes = mic->quantum * mic->channels * sizeof(gint16);
		gint16 pcm[2 * RATE / 100];
		gint64 next_drain = fake_now + 10000;

		s.next_play = G_MAXINT64;
		if (threaded) {
			g_mutex_init(&s.lock);
			g_cond_init(&s.ready);
			worker = g_thread_new("bench-opus", service_worker, &s);
		}
		for (int i = -WARMUP; i < N; i++) {
			gint64 rx_cpu = 0, rx_wall = 0, callback = 0;

			for (guint q = 0; q < frames; q++) {
				fake_now += 10000;
				device_consume(speaker, &next_drain);
				__wrap_SDL_DequeueAudio(mic - devices, pcm,
							bytes);
				/* Measure the callback that publishes a complete packet. */
				gint64 at = service_time(CLOCK_MONOTONIC_RAW);

				mic->input(mic->userdata, (Uint8 *)pcm, bytes);
				if (q + 1 == frames) {
					callback =
						service_time(
							CLOCK_MONOTONIC_RAW) -
						at;
					break;
				}
				if (fake_now >= s.next_play) {
					at = service_time(CLOCK_MONOTONIC_RAW);
					dispatch_audio_job(&s, JOB_PLAYOUT,
							   threaded);
					rx_wall +=
						service_time(
							CLOCK_MONOTONIC_RAW) -
						at;
					rx_cpu += s.cpu_ns;
				}
			}
			gint64 at = service_time(CLOCK_MONOTONIC_RAW);

			g_assert_cmpint(poll(&wake, 1, 0), ==, 1);
			g_assert_true(line_audio_capture_ready(s.tx));
			dispatch_audio_job(&s, JOB_ENCODE, threaded);
			gint64 ready = service_time(CLOCK_MONOTONIC_RAW) - at;
			gint64 tx_cpu = s.cpu_ns;

			g_assert_cmpint(s.size, >, 0);
			if (voices > 1)
				mix_service_packet(&s, ingest, relay, voices);
			at = service_time(CLOCK_MONOTONIC_RAW);
			dispatch_audio_job(&s, JOB_RECEIVE, threaded);
			rx_wall += service_time(CLOCK_MONOTONIC_RAW) - at;
			rx_cpu += s.cpu_ns;
			if (i >= 0) {
				us[TX_CPU][i] = tx_cpu / 1000;
				us[RX_CPU][i] = rx_cpu / 1000;
				us[CALLBACK][i] = callback / 1000;
				us[READY][i] = ready / 1000;
				us[COMBINED][i] =
					(callback + ready + rx_wall) / 1000;
			}
		}
		if (worker) {
			dispatch_audio_job(&s, JOB_EXIT, TRUE);
			g_thread_join(worker);
			g_cond_clear(&s.ready);
			g_mutex_clear(&s.lock);
		}
		struct line_audio_stats stats;

		line_audio_stats(s.rx, &stats);
		g_assert_cmpuint(stats.played, >,
				 (N - 5) * frames * RATE / 100);
		static const char *const labels[] = {
			"encode-cpu", "receive/playout-cpu", "capture-callback",
			"ready-to-SRTP", "combined-service"
		};
		for (guint m = 0; m < G_N_ELEMENTS(labels); m++) {
			qsort(us[m], N, sizeof(*us[m]), compare_latency);
			g_test_message("Opus %s %s %s p50/p95/p99=%u/%u/%u us",
				       name, threaded ? "worker" : "inline",
				       labels[m], us[m][N / 2 - 1],
				       us[m][N * 95 / 100 - 1],
				       us[m][N * 99 / 100 - 1]);
		}
		if (ingest)
			srtp_dealloc(ingest);
		if (relay)
			srtp_dealloc(relay);
		line_audio_close(s.rx);
		line_audio_close(s.tx);
	}
}

static void group_audio_scenario(gconstpointer data)
{
	const struct link_profile *p = data;
	struct audio_result r = run_audio(p, TRUE, FALSE);

	g_assert_cmpfloat(r.late_pc, <=, 1.0);
	g_assert_cmpuint(r.probes, >=, (AUDIO_SECONDS - 1) / 2);
	if (p == &clean)
		g_assert_cmpuint(r.p50, <=, p->delay_ms + 3 * 40);
	else {
		/* Before deadline-driven concealment this was 12.2 s/min. */
		g_assert_cmpfloat(r.dead_air_ms, <, 6100);
		g_assert_cmpuint(r.p99, <=,
				 p->delay_ms + p->jitter_ms + 5 * 40);
	}
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
	struct audio_result before = run_audio(&hd_inline, FALSE, TRUE);
	struct audio_result thread = run_audio(&hd_threaded_clock, FALSE, TRUE);
	struct audio_result after = run_audio(&hd_threaded, FALSE, TRUE);

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
	const char *seed = g_getenv("AVERNELLA_NET_SEED");
	guint64 value;

	if (seed && !g_ascii_string_to_unsigned(seed, 10, 0, G_MAXUINT32,
						&value, NULL)) {
		g_printerr(
			"AVERNELLA_NET_SEED must be an unsigned 32-bit decimal integer\n");
		return EXIT_FAILURE;
	}
	if (seed)
		channel_seed = value;
	g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	g_setenv("LINE_NOISE_SUPPRESSION", "1", TRUE);
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
	g_test_add_func("/net/video/asymmetric-burst-25%", video_hd_burst);
	g_test_add_data_func("/net/service/direct", GUINT_TO_POINTER(0),
			     audio_service);
	g_test_add_data_func("/net/service/group", GUINT_TO_POINTER(1),
			     audio_service);
	g_test_add_data_func("/net/service/group-eight-voices",
			     GUINT_TO_POINTER(8), audio_service);
	g_test_add_data_func("/net/mic/direct", GUINT_TO_POINTER(0),
			     capture_ring);
	g_test_add_data_func("/net/mic/group-mono", GUINT_TO_POINTER(1),
			     capture_ring);
	g_test_add_data_func("/net/mic/group-stereo", GUINT_TO_POINTER(2),
			     capture_ring);
	g_test_add_data_func("/net/audio/direct-25pc", &asymmetric_burst,
			     audio_scenario);
	g_test_add_data_func("/net/audio/group-clean", &clean,
			     group_audio_scenario);
	g_test_add_data_func("/net/audio/group-25pc", &asymmetric_burst,
			     group_audio_scenario);
	g_test_add_func("/net/compare/weak-wifi-uplink", uplink_compare);
	g_test_add_func("/net/compare/hd-video-audio", hd_compare);
	return g_test_run();
}
