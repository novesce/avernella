// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8_jetson.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

struct call_vp8_jetson {
	GstElement *pipeline, *source, *codec, *sink;
	GstBufferPool *pool;
	GstVideoInfo input;
	GstSample *sample;
	GstMapInfo bits;
	gboolean started;
};

/* Device-tree compatible strings are separated by NUL bytes. */
static gboolean compatible(const char *file, const char *prefix)
{
	g_autofree char *values = NULL;
	gsize size;

	if (!g_file_get_contents(file, &values, &size, NULL))
		return FALSE;
	for (gsize i = 0; i < size; i += strlen(values + i) + 1)
		if (g_str_has_prefix(values + i, prefix))
			return TRUE;
	return FALSE;
}

static gboolean jetson_available(void)
{
	const char *node = g_getenv("AVERNELLA_RENDER_NODE");
	const char *board = "/proc/device-tree/compatible";

	if (!compatible(board, "nvidia,tegra210") &&
	    !compatible(board, "nvidia,tegra186"))
		return FALSE;
	if (!node || !*node)
		return TRUE;
	struct stat st;

	if (stat(node, &st) || !S_ISCHR(st.st_mode))
		return FALSE;
	g_autofree char *device =
		g_strdup_printf("/sys/dev/char/%u:%u/device/of_node/compatible",
				major(st.st_rdev), minor(st.st_rdev));

	return compatible(device, "nvidia,");
}

static void release_packet(struct call_vp8_jetson *e)
{
	if (e->sample) {
		gst_buffer_unmap(gst_sample_get_buffer(e->sample), &e->bits);
		gst_sample_unref(e->sample);
		e->sample = NULL;
	}
}

void call_vp8_jetson_close(struct call_vp8_jetson *e)
{
	if (!e)
		return;
	release_packet(e);
	if (e->pipeline)
		gst_element_set_state(e->pipeline, GST_STATE_NULL);
	if (e->pool)
		gst_buffer_pool_set_active(e->pool, FALSE);
	g_clear_object(&e->pool);
	g_clear_object(&e->source);
	g_clear_object(&e->codec);
	g_clear_object(&e->sink);
	g_clear_object(&e->pipeline);
	g_free(e);
}

struct call_vp8_jetson *call_vp8_jetson_open(guint width, guint height,
					     guint kbps, guint fps,
					     guint key_distance)
{
	if (!jetson_available() || !gst_init_check(NULL, NULL, NULL))
		return NULL;

	struct call_vp8_jetson *e = g_new0(struct call_vp8_jetson, 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(GstCaps) input = NULL;

	e->pipeline = gst_parse_launch(
		"appsrc name=input format=time is-live=true ! nvvidconv ! "
		"video/x-raw(memory:NVMM),format=I420 ! nvv4l2vp8enc name=vp8 ! "
		"video/x-vp8 ! appsink name=output sync=false async=false "
		"max-buffers=1 enable-last-sample=false",
		&error);
	if (error || !e->pipeline)
		goto failed;
	e->source = gst_bin_get_by_name(GST_BIN(e->pipeline), "input");
	e->codec = gst_bin_get_by_name(GST_BIN(e->pipeline), "vp8");
	e->sink = gst_bin_get_by_name(GST_BIN(e->pipeline), "output");
	if (!gst_video_info_set_format(&e->input, GST_VIDEO_FORMAT_I420, width,
				       height))
		goto failed;
	e->input.fps_n = fps;
	e->input.fps_d = 1;
	input = gst_video_info_to_caps(&e->input);
	gst_app_src_set_caps(GST_APP_SRC(e->source), input);
	g_object_set(e->codec, "bitrate", kbps * 1000u, "iframeinterval",
		     key_distance, NULL);
	e->pool = gst_buffer_pool_new();
	GstStructure *params = gst_buffer_pool_get_config(e->pool);

	gst_buffer_pool_config_set_params(params, input, e->input.size, 0, 0);
	if (!gst_buffer_pool_set_config(e->pool, params) ||
	    !gst_buffer_pool_set_active(e->pool, TRUE) ||
	    gst_element_set_state(e->pipeline, GST_STATE_PLAYING) ==
		    GST_STATE_CHANGE_FAILURE)
		goto failed;
	return e;
failed:
	call_vp8_jetson_close(e);
	return NULL;
}

gboolean call_vp8_jetson_frame(struct call_vp8_jetson *e,
			       const struct line_camera_frame *frame,
			       gint64 pts90k, gboolean force_key,
			       const guint8 **picture, gsize *size,
			       gboolean *key)
{
	GstBuffer *input;
	GstVideoFrame mapped;
	GstClockTime pts = gst_util_uint64_scale(pts90k, GST_SECOND, 90000);

	if (frame->width != (guint)e->input.width ||
	    frame->height != (guint)e->input.height)
		return FALSE;
	release_packet(e);
	if (gst_buffer_pool_acquire_buffer(e->pool, &input, NULL) !=
	    GST_FLOW_OK)
		return FALSE;
	if (!gst_video_frame_map(&mapped, &e->input, input, GST_MAP_WRITE)) {
		gst_buffer_unref(input);
		return FALSE;
	}
	for (guint i = 0; i < 3; i++) {
		const guint8 *row = frame->plane[i];
		guint8 *dest = GST_VIDEO_FRAME_PLANE_DATA(&mapped, i);
		guint w = GST_VIDEO_FRAME_COMP_WIDTH(&mapped, i);
		guint h = GST_VIDEO_FRAME_COMP_HEIGHT(&mapped, i);
		int pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, i);

		for (guint y = 0; y < h;
		     y++, row += frame->stride[i], dest += pitch)
			memcpy(dest, row, w);
	}
	gst_video_frame_unmap(&mapped);
	GST_BUFFER_PTS(input) = pts;
	GST_BUFFER_DURATION(input) = GST_SECOND / e->input.fps_n;
	if (e->started && force_key)
		gst_element_send_event(
			e->codec, gst_video_event_new_downstream_force_key_unit(
					  pts, GST_CLOCK_TIME_NONE,
					  GST_CLOCK_TIME_NONE, TRUE, 0));
	if (gst_app_src_push_buffer(GST_APP_SRC(e->source), input) !=
	    GST_FLOW_OK)
		return FALSE;
	g_autoptr(GstSample) sample = gst_app_sink_try_pull_sample(
		GST_APP_SINK(e->sink),
		e->started ? 200 * GST_MSECOND : GST_SECOND);

	if (!sample)
		return FALSE;
	GstBuffer *out = gst_sample_get_buffer(sample);
	const GstSegment *segment = gst_sample_get_segment(sample);

	if (!out || !segment || segment->format != GST_FORMAT_TIME ||
	    gst_segment_to_stream_time(segment, GST_FORMAT_TIME,
				       GST_BUFFER_PTS(out)) != pts ||
	    !gst_buffer_map(out, &e->bits, GST_MAP_READ))
		return FALSE;
	e->sample = g_steal_pointer(&sample);
	e->started = TRUE;
	if (e->bits.size < 3)
		return FALSE;
	*key = !(e->bits.data[0] & 1);
	*picture = e->bits.data;
	*size = e->bits.size;
	return !force_key || *key;
}
