// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Screen capture for call screen sharing: the XDG Desktop Portal
 * ScreenCast session picks the screen or window, and PipeWire delivers
 * its frames. https://flatpak.github.io/xdg-desktop-portal/docs/
 * doc-org.freedesktop.portal.ScreenCast.html
 */
#include "screencast.h"
#include "screencast_cinnamon.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <string.h>
#include <sys/eventfd.h>
#include "screencast_dma.h"
#include <unistd.h>

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SCREENCAST "org.freedesktop.portal.ScreenCast"

struct line_screencast {
	GThread *thread;
	GMainContext *context;
	GCancellable *cancel;
	int wake;
	unsigned int max_width, max_height, fps, kbps;
	line_cast_probe_fn probe;
	guint64 modifier;
	struct cast_dma_pool *pool;
	gboolean offered_dma, frame_dma;

	GMutex lock;
	enum line_screencast_state state;
	char *message;
	GBytes *frame;
	unsigned int width, height;
	guint64 time_ns;

	/* Owned by the worker thread. */
	GDBusConnection *bus;
	char *session;
	struct line_cinnamon_cast *cinnamon;
	struct pw_thread_loop *pw_loop;
	struct pw_context *pw_context;
	struct pw_core *pw_core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info_raw format;
	gint64 last_frame;
	gboolean trace, first_buffer;
	guint8 *scratch;
	gsize scratch_size;
};

static void notify(struct line_screencast *c)
{
	eventfd_t one = 1;

	(void)eventfd_write(c->wake, one);
}

static void set_state(struct line_screencast *c,
		      enum line_screencast_state state, const char *message)
{
	g_mutex_lock(&c->lock);
	/* A failure or stop is final. */
	if (c->state != LINE_SCREENCAST_FAILED &&
	    c->state != LINE_SCREENCAST_STOPPED) {
		c->state = state;
		if (message && !c->message)
			c->message = g_strdup(message);
	}
	g_mutex_unlock(&c->lock);
	notify(c);
}

void line_screencast_fit(unsigned int width, unsigned int height,
			 unsigned int max_width, unsigned int max_height,
			 unsigned int *out_width, unsigned int *out_height)
{
	double scale = 1;

	if (width > max_width)
		scale = (double)max_width / width;
	if (height * scale > max_height)
		scale = (double)max_height / height;
	*out_width = MAX(2u, (unsigned int)(width * scale) & ~1u);
	*out_height = MAX(2u, (unsigned int)(height * scale) & ~1u);
}

struct rgb {
	int r, g, b;
};

struct sample_axis {
	unsigned int first, second, weight;
};

static inline __attribute__((always_inline)) struct rgb
read_rgb(const guint8 *p, int red, int blue)
{
	return (struct rgb){ p[red], p[1], p[blue] };
}

/* Limited-range BT.601, matching the VP8 decoder. */
static inline __attribute__((always_inline)) guint8 screen_luma(struct rgb p)
{
	return ((66 * p.r + 129 * p.g + 25 * p.b + 128) >> 8) + 16;
}

static inline __attribute__((always_inline)) void
screen_chroma(struct rgb a, struct rgb b, struct rgb c, struct rgb d, guint8 *u,
	      guint8 *v)
{
	int r = a.r + b.r + c.r + d.r, g = a.g + b.g + c.g + d.g,
	    blue = a.b + b.b + c.b + d.b;

	*u = ((-38 * r - 74 * g + 112 * blue + 512) >> 10) + 128;
	*v = ((112 * r - 94 * g - 18 * blue + 512) >> 10) + 128;
}

static inline __attribute__((always_inline)) struct sample_axis
interpolate(unsigned int at, guint32 step, unsigned int limit)
{
	guint32 center = at * step + step / 2;

	if (center < 32768)
		return (struct sample_axis){ 0, 0, 0 };
	center -= 32768;
	unsigned int first = MIN(center >> 16, limit - 1);

	return (struct sample_axis){ first, MIN(first + 1, limit - 1),
				     (center >> 8) & 255 };
}

static inline __attribute__((always_inline)) struct rgb
sample_rgb(const guint8 *top, const guint8 *bottom, struct sample_axis x,
	   unsigned int wy, int red, int blue)
{
	const guint8 *a = top + 4 * x.first, *b = top + 4 * x.second;
	const guint8 *c = bottom + 4 * x.first, *d = bottom + 4 * x.second;
	int result[3];

	for (int i = 0; i < 3; i++) {
		int upper = (a[i] << 8) + (b[i] - a[i]) * (int)x.weight;
		int lower = (c[i] << 8) + (d[i] - c[i]) * (int)x.weight;

		result[i] =
			((upper << 8) + (lower - upper) * (int)wy + 32768) >>
			16;
	}
	return (struct rgb){ result[red], result[1], result[blue] };
}

/* Let GCC optimize the pixel loop for throughput in size-optimized builds. */
__attribute__((optimize("O3"))) void
line_screencast_to_i420(const guint8 *src, unsigned int width,
			unsigned int height, unsigned int stride, gboolean bgr,
			guint8 *dst, unsigned int out_width,
			unsigned int out_height)
{
	guint8 *u = dst + (gsize)out_width * out_height;
	guint8 *v = u + (gsize)(out_width / 2) * (out_height / 2);
	int red = bgr ? 2 : 0, blue = bgr ? 0 : 2;

	if (width == out_width && height == out_height) {
		for (unsigned int y = 0; y < height; y += 2) {
			const guint8 *top = src + (gsize)y * stride;
			const guint8 *bottom = top + stride;
			guint8 *y0 = dst + (gsize)y * width, *y1 = y0 + width;

			for (unsigned int x = 0; x < width; x += 2) {
				struct rgb a = read_rgb(top + 4 * x, red, blue);
				struct rgb b =
					read_rgb(top + 4 * x + 4, red, blue);
				struct rgb c =
					read_rgb(bottom + 4 * x, red, blue);
				struct rgb d =
					read_rgb(bottom + 4 * x + 4, red, blue);

				y0[x] = screen_luma(a);
				y0[x + 1] = screen_luma(b);
				y1[x] = screen_luma(c);
				y1[x + 1] = screen_luma(d);
				screen_chroma(a, b, c, d, u++, v++);
			}
		}
		return;
	}

	/* The PipeWire consumer clamps captures to 3840 pixels wide. */
	struct sample_axis columns[3840];
	guint32 sx = ((guint64)width << 16) / out_width;
	guint32 sy = ((guint64)height << 16) / out_height;

	g_assert(out_width <= G_N_ELEMENTS(columns));
	for (unsigned int x = 0; x < out_width; x++)
		columns[x] = interpolate(x, sx, width);
	for (unsigned int y = 0; y < out_height; y += 2) {
		struct sample_axis a = interpolate(y, sy, height);
		struct sample_axis b = interpolate(y + 1, sy, height);
		const guint8 *a0 = src + (gsize)a.first * stride;
		const guint8 *a1 = src + (gsize)a.second * stride;
		const guint8 *b0 = src + (gsize)b.first * stride;
		const guint8 *b1 = src + (gsize)b.second * stride;
		guint8 *y0 = dst + (gsize)y * out_width, *y1 = y0 + out_width;

		for (unsigned int x = 0; x < out_width; x += 2) {
			struct rgb p = sample_rgb(a0, a1, columns[x], a.weight,
						  red, blue);
			struct rgb q = sample_rgb(a0, a1, columns[x + 1],
						  a.weight, red, blue);
			struct rgb r = sample_rgb(b0, b1, columns[x], b.weight,
						  red, blue);
			struct rgb s = sample_rgb(b0, b1, columns[x + 1],
						  b.weight, red, blue);

			y0[x] = screen_luma(p);
			y0[x + 1] = screen_luma(q);
			y1[x] = screen_luma(r);
			y1[x + 1] = screen_luma(s);
			screen_chroma(p, q, r, s, u++, v++);
		}
	}
}

/* ---- PipeWire ---- */

/* Native I420 avoids converting every pixel of an already planar capture.
 * Each negotiated plane carries its own origin, valid length and row pitch.
 */
GBytes *line_screencast_planar(const struct spa_buffer *buf, guint w, guint h)
{
	struct line_camera_frame img = { .width = w & ~1u, .height = h & ~1u };

	if (!buf || buf->n_datas != 3 || w < 2 || h < 2 || w > 3840 || h > 3840)
		return NULL;
	for (guint i = 0; i < 3; i++) {
		const struct spa_data *d = &buf->datas[i];
		const struct spa_chunk *ch = d->chunk;
		guint cols = img.width >> !!i, rows = img.height >> !!i;

		if (!d->data || !ch || (ch->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
		    ch->stride < (gint)cols || ch->offset > d->maxsize ||
		    ch->size > d->maxsize - ch->offset ||
		    (guint64)(rows - 1) * ch->stride + cols > ch->size)
			return NULL;
		img.plane[i] = (const guint8 *)d->data + ch->offset;
		img.stride[i] = ch->stride;
	}
	gsize size = line_camera_i420_size(img.width, img.height);
	guint8 *pixels = g_malloc(size);

	line_camera_pack_i420(pixels, &img);
	return g_bytes_new_take(pixels, size);
}

static void on_param_changed(void *data, uint32_t id,
			     const struct spa_pod *param)
{
	struct line_screencast *c = data;
	struct spa_video_info info = { 0 };
	guint8 buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
	const struct spa_pod *params[1];

	if (!param || id != SPA_PARAM_Format ||
	    spa_format_parse(param, &info.media_type, &info.media_subtype) <
		    0 ||
	    info.media_type != SPA_MEDIA_TYPE_video ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_video_raw_parse(param, &info.info.raw) < 0)
		return;
	c->format = info.info.raw;
	if (c->trace)
		g_printerr("group-stats capture format=%u width=%u height=%u\n",
			   c->format.format, c->format.size.width,
			   c->format.size.height);
	gboolean dma = c->format.format == SPA_VIDEO_FORMAT_NV12 &&
		       c->offered_dma;
	if (dma) {
		const struct spa_pod_prop *p = spa_pod_find_prop(
			param, NULL, SPA_FORMAT_VIDEO_modifier);
		if (p && (p->flags & SPA_POD_PROP_FLAG_DONT_FIXATE))
			return; /* Wait for the producer's chosen modifier. */
		if (!p || c->format.modifier != c->modifier) {
			cast_dma_reject(c->pool);
			return;
		}
	}
	/* Latest, in-flight, queued and producer-held surfaces can coexist. */
	int blocks = c->format.format == SPA_VIDEO_FORMAT_I420 ? 3 : 1;
	int types = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);

	params[0] = spa_pod_builder_add_object(
		&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
		SPA_POD_CHOICE_RANGE_Int(dma ? 4 : 3, dma ? 4 : 1, G_MAXINT),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(dma ? 2 : blocks),
		SPA_PARAM_BUFFERS_dataType,
		SPA_POD_CHOICE_FLAGS_Int(dma ? (1 << SPA_DATA_DmaBuf) : types));
	if (pw_stream_update_params(c->stream, params, 1) < 0 && dma)
		cast_dma_reject(c->pool);
}

static void on_state_changed(void *data, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	struct line_screencast *c = data;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR)
		set_state(c, LINE_SCREENCAST_FAILED,
			  error ? error : "The screen capture stream failed.");
	else if (state == PW_STREAM_STATE_STREAMING)
		set_state(c, LINE_SCREENCAST_RUNNING, NULL);
	else if (state == PW_STREAM_STATE_UNCONNECTED)
		set_state(c, LINE_SCREENCAST_STOPPED, "Screen sharing ended.");
}

static void on_process(void *data)
{
	struct line_screencast *c = data;
	struct pw_buffer *buffer = NULL, *next;

	/* Keep only the newest frame. */
	while ((next = pw_stream_dequeue_buffer(c->stream))) {
		if (buffer)
			pw_stream_queue_buffer(c->stream, buffer);
		buffer = next;
	}
	if (!buffer)
		return;
	struct spa_buffer *spa = buffer->buffer;

	if (c->trace && !c->first_buffer && spa->n_datas) {
		const struct spa_data *plane = &spa->datas[0];

		c->first_buffer = TRUE;
		g_printerr("group-stats capture mapped=%d stride=%d bytes=%u\n",
			   plane->data != NULL,
			   plane->chunk ? plane->chunk->stride : 0,
			   plane->chunk ? plane->chunk->size : 0);
	}
	if (!spa->n_datas || g_cancellable_is_cancelled(c->cancel))
		goto done;
	struct spa_data *d = &spa->datas[0];
	gint64 now = g_get_monotonic_time();
	enum spa_video_format f = c->format.format;
	gboolean bgr = f == SPA_VIDEO_FORMAT_BGRx || f == SPA_VIDEO_FORMAT_BGRA;
	unsigned int w = c->format.size.width, h = c->format.size.height;

	if (!w || !h || w > 16384 || h > 16384 ||
	    now - c->last_frame < G_USEC_PER_SEC / c->fps * 9 / 10)
		goto done;
	unsigned int ow, oh;
	GBytes *frame;

	line_screencast_fit(w, h, c->max_width, c->max_height, &ow, &oh);
	gboolean dma = f == SPA_VIDEO_FORMAT_NV12;

	if (dma) {
		for (guint i = 0; i < spa->n_datas; i++)
			if (spa->datas[i].chunk &&
			    (spa->datas[i].chunk->flags &
			     (SPA_CHUNK_FLAG_CORRUPTED | SPA_CHUNK_FLAG_EMPTY)))
				goto done;
		frame = cast_dma_take(c->pool, buffer, &c->format, c->max_width,
				      c->max_height, c->modifier);
		if (!frame) {
			if (c->pool)
				cast_dma_reject(c->pool);
			goto done;
		}
		buffer =
			NULL; /* release_capture returns the buffer to PipeWire. */
	} else if (f == SPA_VIDEO_FORMAT_I420) {
		if (ow != (w & ~1u) || oh != (h & ~1u))
			goto done;
		frame = line_screencast_planar(spa, w, h);
		if (!frame)
			goto done;
	} else {
		if (!d->data || !d->chunk ||
		    (!bgr && f != SPA_VIDEO_FORMAT_RGBx &&
		     f != SPA_VIDEO_FORMAT_RGBA) ||
		    (d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED))
			goto done;
		unsigned int stride = d->chunk->stride ?
					      (unsigned int)d->chunk->stride :
					      w * 4;
		if ((guint64)stride * h + d->chunk->offset > d->maxsize ||
		    stride < w * 4)
			goto done;
		gsize size = line_camera_i420_size(ow, oh);
		guint8 *pixels = g_malloc(size);

		line_screencast_to_i420((const guint8 *)d->data +
						d->chunk->offset,
					w, h, stride, bgr, pixels, ow, oh);
		frame = g_bytes_new_take(pixels, size);
	}
	c->last_frame = now;
	g_mutex_lock(&c->lock);
	g_clear_pointer(&c->frame, g_bytes_unref);
	c->frame = frame;
	c->frame_dma = dma;
	c->width = ow;
	c->height = oh;
	c->time_ns = (guint64)now * 1000;
	g_mutex_unlock(&c->lock);
	notify(c);
done:
	if (buffer)
		pw_stream_queue_buffer(c->stream, buffer);
}

static void remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct line_screencast *c = data;

	cast_dma_remove(c->pool, buffer);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,	   .state_changed = on_state_changed,
	.param_changed = on_param_changed, .process = on_process,
	.remove_buffer = remove_buffer,
};

static guint capture_formats(struct line_screencast *c,
			     struct spa_pod_builder *b,
			     const struct spa_pod **params)
{
	struct spa_rectangle cap = SPA_RECTANGLE(c->max_width, c->max_height);
	struct spa_rectangle def = SPA_RECTANGLE(1920, 1080),
			     min = SPA_RECTANGLE(1, 1),
			     max = SPA_RECTANGLE(16384, 16384);
	struct spa_fraction rate = SPA_FRACTION(c->fps, 1),
			    rmin = SPA_FRACTION(0, 1),
			    rmax = SPA_FRACTION(c->fps, 1);

	guint count = 0;

	if (c->offered_dma && !cast_dma_rejected(c->pool)) {
		struct spa_pod_frame f, choice;

		spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format,
					    SPA_PARAM_EnumFormat);
		spa_pod_builder_add(
			b, SPA_FORMAT_mediaType,
			SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,
			SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,
			SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
			SPA_FORMAT_VIDEO_colorMatrix,
			SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_BT601),
			SPA_FORMAT_VIDEO_colorRange,
			SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_16_235),
			SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&cap),
			SPA_FORMAT_VIDEO_framerate,
			SPA_POD_CHOICE_RANGE_Fraction(&rate, &rmin, &rmax), 0);
		/* The allocating compositor fixates the common modifier. */
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
				     SPA_POD_PROP_FLAG_MANDATORY |
					     SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_long(b, c->modifier); /* default */
		spa_pod_builder_long(b, c->modifier);
		spa_pod_builder_pop(b, &choice);
		params[count++] = spa_pod_builder_pop(b, &f);
	}
	params[count++] = spa_pod_builder_add_object(
		b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_I420),
		SPA_FORMAT_VIDEO_colorMatrix,
		SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_BT601),
		SPA_FORMAT_VIDEO_colorRange,
		SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_16_235), SPA_FORMAT_VIDEO_size,
		SPA_POD_CHOICE_RANGE_Rectangle(&cap, &min, &cap),
		SPA_FORMAT_VIDEO_framerate,
		SPA_POD_CHOICE_RANGE_Fraction(&rate, &rmin, &rmax));
	params[count++] = spa_pod_builder_add_object(
		b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,
		SPA_POD_CHOICE_ENUM_Id(
			5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
			SPA_VIDEO_FORMAT_RGBA),
		SPA_FORMAT_VIDEO_size,
		SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max),
		SPA_FORMAT_VIDEO_framerate,
		SPA_POD_CHOICE_RANGE_Fraction(&rate, &rmin, &rmax));
	return count;
}

static void request_shm(void *data, uint64_t count G_GNUC_UNUSED)
{
	struct line_screencast *c = data;
	guint8 buffer[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[3];
	guint n = capture_formats(c, &b, params);

	if (pw_stream_update_params(c->stream, params, n) < 0)
		set_state(c, LINE_SCREENCAST_FAILED,
			  "Could not renegotiate screen capture.");
}

static gboolean start_pipewire(struct line_screencast *c, int fd, guint32 node,
			       GError **e)
{
	static gsize initialized;
	guint8 buffer[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[3];
	if (g_once_init_enter(&initialized)) {
		pw_init(NULL, NULL);
		g_once_init_leave(&initialized, 1);
	}
	c->pw_loop = pw_thread_loop_new("line-screencast", NULL);
	if (!c->pw_loop)
		goto failed;
	pw_thread_loop_lock(c->pw_loop);
	c->pw_context =
		pw_context_new(pw_thread_loop_get_loop(c->pw_loop), NULL, 0);
	if (!c->pw_context)
		goto unlock;
	if (fd >= 0) {
		c->pw_core = pw_context_connect_fd(c->pw_context, fd, NULL, 0);
		fd = -1;
	} else {
		c->pw_core = pw_context_connect(c->pw_context, NULL, 0);
	}
	if (!c->pw_core)
		goto unlock;
	c->stream = pw_stream_new(
		c->pw_core, "Avernella screen share",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Screen",
				  "node.dont-fallback", "true", NULL));
	if (!c->stream)
		goto unlock;
	if (c->probe && c->probe(c->max_width, c->max_height, c->fps, c->kbps,
				 &c->modifier)) {
		c->pool = cast_dma_pool_new(c->stream,
					    pw_thread_loop_get_loop(c->pw_loop),
					    request_shm, c);
		c->offered_dma = c->pool != NULL;
	}
	pw_stream_add_listener(c->stream, &c->stream_listener, &stream_events,
			       c);
	guint count = capture_formats(c, &b, params);
	if (pw_stream_connect(c->stream, PW_DIRECTION_INPUT, node,
			      PW_STREAM_FLAG_AUTOCONNECT |
				      PW_STREAM_FLAG_MAP_BUFFERS |
				      PW_STREAM_FLAG_DONT_RECONNECT,
			      params, count) < 0)
		goto unlock;
	pw_thread_loop_unlock(c->pw_loop);
	if (pw_thread_loop_start(c->pw_loop) < 0)
		goto failed;
	return TRUE;
unlock:
	pw_thread_loop_unlock(c->pw_loop);
failed:
	if (fd >= 0)
		close(fd);
	g_set_error_literal(
		e, LINE_ERROR, 1,
		"Could not connect to PipeWire for screen capture.");
	return FALSE;
}

static void stop_pipewire(struct line_screencast *c)
{
	if (!c->pw_loop)
		return;
	pw_thread_loop_stop(c->pw_loop);
	g_clear_pointer(&c->pool, cast_dma_pool_stop);
	if (c->stream)
		pw_stream_destroy(c->stream);
	if (c->pw_core)
		pw_core_disconnect(c->pw_core);
	if (c->pw_context)
		pw_context_destroy(c->pw_context);
	pw_thread_loop_destroy(c->pw_loop);
	c->stream = NULL;
	c->pw_core = NULL;
	c->pw_context = NULL;
	c->pw_loop = NULL;
}

/* ---- Portal ---- */

struct portal_reply {
	gboolean done;
	guint32 response;
	GVariant *results;
};

static void on_response(GDBusConnection *bus, const char *sender,
			const char *path, const char *iface, const char *signal,
			GVariant *params, gpointer data)
{
	struct portal_reply *r = data;

	(void)bus, (void)sender, (void)path, (void)iface, (void)signal;
	g_variant_get(params, "(u@a{sv})", &r->response, &r->results);
	r->done = TRUE;
}

/* Calls a portal method that answers through a Request object. */
static GVariant *portal_request(struct line_screencast *c, const char *method,
				GVariant *args, const char *token, GError **e)
{
	g_autofree char *sender =
		g_strdup(g_dbus_connection_get_unique_name(c->bus) + 1);
	struct portal_reply reply = { 0 };

	for (char *p = sender; *p; p++)
		if (*p == '.')
			*p = '_';
	g_autofree char *path =
		g_strdup_printf(PORTAL_PATH "/request/%s/%s", sender, token);
	guint id = g_dbus_connection_signal_subscribe(
		c->bus, PORTAL_NAME, "org.freedesktop.portal.Request",
		"Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response,
		&reply, NULL);
	/* Subscribed before the call, so the response cannot be missed. */
	g_autoptr(GVariant) handle = g_dbus_connection_call_sync(
		c->bus, PORTAL_NAME, PORTAL_PATH, SCREENCAST, method, args,
		G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, c->cancel,
		e);

	while (handle && !reply.done && !g_cancellable_is_cancelled(c->cancel))
		g_main_context_iteration(c->context, TRUE);
	g_dbus_connection_signal_unsubscribe(c->bus, id);
	if (!handle)
		return NULL;
	if (!reply.done) {
		g_set_error_literal(e, LINE_ERROR, 1,
				    "Screen sharing was cancelled.");
		return NULL;
	}
	if (reply.response) {
		g_clear_pointer(&reply.results, g_variant_unref);
		g_set_error_literal(
			e, LINE_ERROR, 1,
			reply.response == 1 ?
				"Screen sharing was cancelled." :
				"The desktop portal refused screen sharing.");
		return NULL;
	}
	return reply.results;
}

static guint32 cursor_modes(struct line_screencast *c)
{
	g_autoptr(GVariant) v = g_dbus_connection_call_sync(
		c->bus, PORTAL_NAME, PORTAL_PATH,
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", SCREENCAST, "AvailableCursorModes"),
		G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, c->cancel,
		NULL);
	g_autoptr(GVariant) inner = NULL;

	if (!v)
		return 0;
	g_variant_get(v, "(v)", &inner);
	return g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32) ?
		       g_variant_get_uint32(inner) :
		       0;
}

static gboolean cinnamon_wayland(void)
{
	const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
	const char *session = g_getenv("XDG_SESSION_TYPE");
	gboolean wayland = session ? !strcmp(session, "wayland") :
				     g_getenv("WAYLAND_DISPLAY") ||
					     g_getenv("WAYLAND_SOCKET");

	if (wayland && desktop) {
		g_auto(GStrv) names = g_strsplit(desktop, ":", -1);

		for (char **name = names; *name; name++)
			if (!g_ascii_strcasecmp(*name, "X-Cinnamon") ||
			    !g_ascii_strcasecmp(*name, "Cinnamon"))
				return TRUE;
	}
	return FALSE;
}

/* A missing file lets the portal look at its next candidate. */
static int cinnamon_preference(const char *directory, const char *filename)
{
	g_autofree char *path = g_build_filename(
		directory, "xdg-desktop-portal", filename, NULL);
	g_autoptr(GKeyFile) config = g_key_file_new();
	g_autoptr(GError) error = NULL;

	if (!g_key_file_load_from_file(config, path, G_KEY_FILE_NONE, &error))
		return g_error_matches(error, G_FILE_ERROR,
				       G_FILE_ERROR_NOENT) ?
			       -1 :
			       FALSE;
	g_auto(GStrv) backends = g_key_file_get_string_list(
		config, "preferred", "default", NULL, NULL);

	return backends &&
	       g_strv_equal((const char *const *)backends,
			    (const char *[]){ "xapp", "gtk", NULL }) &&
	       !g_key_file_has_key(config, "preferred",
				   "org.freedesktop.impl.portal.ScreenCast",
				   NULL);
}

/* Match portals.conf(5) search order. Only Cinnamon's stock xapp/gtk
 * selection is eligible. A user or admin override can disable ScreenCast.
 */
static gboolean cinnamon_portal_policy(void)
{
	const char *const user[] = { g_get_user_config_dir(), NULL };
	const char *const system[] = { "/etc", g_get_user_data_dir(), NULL };
	static const char *const vendor[] = { "/usr/share", NULL };
	const char *const *dirs[] = { user, g_get_system_config_dirs(), system,
				      g_get_system_data_dirs(), vendor };
	g_autofree char *desktop =
		g_ascii_strdown(g_getenv("XDG_CURRENT_DESKTOP"), -1);
	g_auto(GStrv) names = g_strsplit(desktop, ":", -1);
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);

	if (strchr(desktop, '/') || g_getenv("XDG_DESKTOP_PORTAL_DIR"))
		return FALSE;
	for (char **name = names; *name; name++)
		if (**name)
			g_ptr_array_add(files,
					g_strconcat(*name, "-portals.conf",
						    NULL));
	g_ptr_array_add(files, g_strdup("portals.conf"));
	for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
		for (const char *const *dir = dirs[i]; *dir; dir++) {
			for (guint j = 0; j < files->len; j++) {
				int allow = cinnamon_preference(
					*dir, files->pdata[j]);

				if (allow >= 0)
					return allow;
			}
		}
	}
	return FALSE;
}

static gboolean missing_portal(const GError *error)
{
	if (!error || error->domain != G_DBUS_ERROR)
		return FALSE;
	switch (error->code) {
	case G_DBUS_ERROR_SERVICE_UNKNOWN:
	case G_DBUS_ERROR_NAME_HAS_NO_OWNER:
	case G_DBUS_ERROR_UNKNOWN_OBJECT:
	case G_DBUS_ERROR_UNKNOWN_INTERFACE:
	case G_DBUS_ERROR_UNKNOWN_METHOD:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Fall back only if the portal does not implement CreateSession. */
static gboolean missing_session(struct line_screencast *c, GError **error)
{
	if (!error || !missing_portal(*error))
		return FALSE;
	gboolean direct = ((*error)->code == G_DBUS_ERROR_UNKNOWN_METHOD ||
			   (*error)->code == G_DBUS_ERROR_UNKNOWN_INTERFACE) &&
			  cinnamon_wayland() && !g_getenv("FLATPAK_ID") &&
			  !g_getenv("SNAP") &&
			  !g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS) &&
			  cinnamon_portal_policy();

	g_clear_error(error);
	if (direct) {
		guint32 node = PW_ID_ANY;

		if (g_cancellable_set_error_if_cancelled(c->cancel, error))
			return FALSE;
		c->cinnamon = line_cinnamon_start(c->context, c->cancel, &node,
						  error);
		if (c->cinnamon)
			return start_pipewire(c, -1, node, error);
		if (!missing_portal(*error))
			return FALSE;
		g_clear_error(error);
	}
	g_set_error_literal(
		error, LINE_ERROR, 1,
		direct ?
			"Screen sharing is unavailable in this Cinnamon session. Try GNOME or Plasma Wayland." :
			"Screen sharing is unavailable. Enable a ScreenCast portal for your desktop.");
	return FALSE;
}

static gboolean run_portal(struct line_screencast *c, GError **e)
{
	guint32 token = g_random_int();
	g_autofree char *t1 = g_strdup_printf("line_%08x_1", token),
			*t2 = g_strdup_printf("line_%08x_2", token),
			*t3 = g_strdup_printf("line_%08x_3", token),
			*st = g_strdup_printf("line_%08x_s", token);
	GVariantBuilder o;

	c->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, c->cancel, e);
	if (!c->bus)
		return FALSE;
	g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&o, "{sv}", "handle_token",
			      g_variant_new_string(t1));
	g_variant_builder_add(&o, "{sv}", "session_handle_token",
			      g_variant_new_string(st));
	g_autoptr(GVariant) created = portal_request(
		c, "CreateSession", g_variant_new("(a{sv})", &o), t1, e);
	const char *session = NULL;

	if (!created)
		return missing_session(c, e);
	if (!g_variant_lookup(created, "session_handle", "&s", &session)) {
		g_set_error_literal(
			e, LINE_ERROR, 1,
			"The desktop portal did not open a screen cast session.");
		return FALSE;
	}
	c->session = g_strdup(session);
	guint32 modes = cursor_modes(c);

	g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&o, "{sv}", "handle_token",
			      g_variant_new_string(t2));
	/* Monitors and windows; one source. */
	g_variant_builder_add(&o, "{sv}", "types", g_variant_new_uint32(3));
	g_variant_builder_add(&o, "{sv}", "multiple",
			      g_variant_new_boolean(FALSE));
	if (modes & 2) /* The pointer drawn into the frames. */
		g_variant_builder_add(&o, "{sv}", "cursor_mode",
				      g_variant_new_uint32(2));
	g_autoptr(GVariant) selected = portal_request(
		c, "SelectSources", g_variant_new("(oa{sv})", c->session, &o),
		t2, e);

	if (!selected)
		return FALSE;
	g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&o, "{sv}", "handle_token",
			      g_variant_new_string(t3));
	g_autoptr(GVariant) started = portal_request(
		c, "Start", g_variant_new("(osa{sv})", c->session, "", &o), t3,
		e);
	g_autoptr(GVariant) streams = NULL;
	guint32 node = 0;

	if (!started ||
	    !(streams = g_variant_lookup_value(started, "streams",
					       G_VARIANT_TYPE("a(ua{sv})")))) {
		if (e && !*e)
			g_set_error_literal(
				e, LINE_ERROR, 1,
				"No screen or window was selected.");
		return FALSE;
	}
	GVariantIter iter;
	g_autoptr(GVariant) props = NULL;

	g_variant_iter_init(&iter, streams);
	if (!g_variant_iter_next(&iter, "(u@a{sv})", &node, &props)) {
		g_set_error_literal(e, LINE_ERROR, 1,
				    "No screen or window was selected.");
		return FALSE;
	}
	g_autoptr(GUnixFDList) fds = NULL;
	g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
	g_autoptr(GVariant) remote =
		g_dbus_connection_call_with_unix_fd_list_sync(
			c->bus, PORTAL_NAME, PORTAL_PATH, SCREENCAST,
			"OpenPipeWireRemote",
			g_variant_new("(oa{sv})", c->session, &o),
			G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			&fds, c->cancel, e);
	gint32 index;

	if (!remote)
		return FALSE;
	g_variant_get(remote, "(h)", &index);
	int fd = fds ? g_unix_fd_list_get(fds, index, e) : -1;

	if (fd < 0) {
		if (e && !*e)
			g_set_error_literal(
				e, LINE_ERROR, 1,
				"The desktop portal sent no PipeWire connection.");
		return FALSE;
	}
	return start_pipewire(c, fd, node, e);
}

static void close_session(struct line_screencast *c)
{
	if (!c->bus || !c->session)
		return;
	g_autoptr(GVariant) r = g_dbus_connection_call_sync(
		c->bus, PORTAL_NAME, c->session,
		"org.freedesktop.portal.Session", "Close", NULL, NULL,
		G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
}

static gpointer worker(gpointer data)
{
	struct line_screencast *c = data;
	g_autoptr(GError) error = NULL;

	g_main_context_push_thread_default(c->context);
	if (!run_portal(c, &error)) {
		set_state(c,
			  g_cancellable_is_cancelled(c->cancel) ?
				  LINE_SCREENCAST_STOPPED :
				  LINE_SCREENCAST_FAILED,
			  error ? error->message : "Screen sharing failed.");
	} else {
		while (!g_cancellable_is_cancelled(c->cancel))
			g_main_context_iteration(c->context, TRUE);
		set_state(c, LINE_SCREENCAST_STOPPED, "Screen sharing ended.");
	}
	stop_pipewire(c);
	if (c->cinnamon)
		line_cinnamon_stop(c->cinnamon);
	close_session(c);
	g_clear_object(&c->bus);
	g_main_context_pop_thread_default(c->context);
	return NULL;
}

struct line_screencast *line_screencast_start(unsigned int max_width,
					      unsigned int max_height,
					      unsigned int fps, guint kbps,
					      line_cast_probe_fn probe,
					      GError **e)
{
	struct line_screencast *c = g_new0(struct line_screencast, 1);

	g_mutex_init(&c->lock);
	c->trace = !g_strcmp0(g_getenv("LINE_GROUP_CALL_STATS"), "1");
	c->max_width = CLAMP(max_width, 16, 3840);
	c->max_height = CLAMP(max_height, 16, 2160);
	c->fps = CLAMP(fps, 1, 60);
	c->probe = probe;
	c->kbps = kbps;
	c->state = LINE_SCREENCAST_STARTING;
	c->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	c->context = g_main_context_new();
	c->cancel = g_cancellable_new();
	if (c->wake < 0 ||
	    !(c->thread = g_thread_try_new("line-screencast", worker, c, e))) {
		if (c->wake >= 0)
			close(c->wake);
		g_main_context_unref(c->context);
		g_object_unref(c->cancel);
		g_mutex_clear(&c->lock);
		g_free(c);
		return NULL;
	}
	return c;
}

void line_screencast_stop(struct line_screencast *c)
{
	if (!c)
		return;
	g_cancellable_cancel(c->cancel);
	g_main_context_wakeup(c->context);
	g_thread_join(c->thread);
	close(c->wake);
	g_main_context_unref(c->context);
	g_object_unref(c->cancel);
	g_clear_pointer(&c->frame, g_bytes_unref);
	g_free(c->message);
	g_free(c->session);
	g_free(c->scratch);
	g_mutex_clear(&c->lock);
	g_free(c);
}

int line_screencast_fd(const struct line_screencast *c)
{
	return c ? c->wake : -1;
}

enum line_screencast_state line_screencast_poll(struct line_screencast *c,
						char **message)
{
	eventfd_t n;

	(void)eventfd_read(c->wake, &n);
	g_mutex_lock(&c->lock);
	enum line_screencast_state state = c->state;

	if (message)
		*message = g_strdup(c->message);
	g_mutex_unlock(&c->lock);
	return state;
}

GBytes *line_screencast_take(struct line_screencast *c, unsigned int *width,
			     unsigned int *height, guint64 *time_ns,
			     gboolean *dma)
{
	g_mutex_lock(&c->lock);
	GBytes *frame = g_steal_pointer(&c->frame);

	if (g_cancellable_is_cancelled(c->cancel))
		g_clear_pointer(&frame, g_bytes_unref);
	*width = c->width;
	*height = c->height;
	*time_ns = c->time_ns;
	*dma = c->frame_dma;
	g_mutex_unlock(&c->lock);
	return frame;
}
