// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <turbojpeg.h>
#include <unistd.h>

enum {
	CAMERA_BUFFERS = 4,
	CAMERA_FALLBACK_WIDTH = 320,
	CAMERA_FALLBACK_HEIGHT = 240
};

struct camera_mode {
	guint32 format;
	unsigned int width, height, fps;
};

struct camera_buffer {
	void *data;
	size_t size;
};

struct line_camera {
	int fd;
	unsigned int count, width, height, pitch;
	guint32 format;
	gboolean streaming;
	guint8 *pixels, *jpeg_planes;
	tjhandle jpeg;
	struct camera_buffer buffers[CAMERA_BUFFERS];
};

static int camera_ioctl(int fd, unsigned long request, void *arg)
{
	int result;

	do {
		result = ioctl(fd, request, arg);
	} while (result < 0 && errno == EINTR);
	return result;
}

void line_camera_close(struct line_camera *camera)
{
	if (!camera)
		return;
	if (camera->streaming) {
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		camera_ioctl(camera->fd, VIDIOC_STREAMOFF, &type);
	}
	for (unsigned int i = 0; i < camera->count; i++)
		munmap(camera->buffers[i].data, camera->buffers[i].size);
	close(camera->fd);
	if (camera->jpeg)
		tjDestroy(camera->jpeg);
	g_free(camera->jpeg_planes);
	g_free(camera->pixels);
	g_free(camera);
}

static unsigned int mode_fps(int fd, guint32 format, unsigned int width,
			     unsigned int height)
{
	struct v4l2_frmivalenum interval = {
		.pixel_format = format,
		.width = width,
		.height = height,
	};
	unsigned int best = 0;

	for (; !camera_ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval);
	     interval.index++) {
		const struct v4l2_fract *f =
			interval.type == V4L2_FRMIVAL_TYPE_DISCRETE ?
				&interval.discrete :
				&interval.stepwise.min;

		if (f->numerator)
			best = MAX(best, f->denominator / f->numerator);
		if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE)
			break;
	}
	return best;
}

/*
 * Prefer the largest picture that keeps motion smooth. Raw YUYV wins a tie
 * because it needs no decoding; MJPEG usually reaches HD at full rate.
 */
static gboolean better_mode(const struct camera_mode *a,
			    const struct camera_mode *b, unsigned int fps)
{
	unsigned int floor = MIN(fps, 20);
	gboolean smooth_a = MIN(a->fps, fps) >= floor,
		 smooth_b = MIN(b->fps, fps) >= floor;
	guint64 pixels_a = (guint64)a->width * a->height,
		pixels_b = (guint64)b->width * b->height;

	if (smooth_a != smooth_b)
		return smooth_a;
	if (pixels_a != pixels_b)
		return pixels_a > pixels_b;
	if (MIN(a->fps, fps) != MIN(b->fps, fps))
		return MIN(a->fps, fps) > MIN(b->fps, fps);
	return a->format == V4L2_PIX_FMT_YUYV && b->format != a->format;
}

static gboolean choose_mode(int fd, unsigned int max_width,
			    unsigned int max_height, unsigned int fps,
			    struct camera_mode *best)
{
	const guint32 formats[] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG };
	gboolean found = FALSE;

	for (guint i = 0; i < G_N_ELEMENTS(formats); i++) {
		struct v4l2_frmsizeenum size = { .pixel_format = formats[i] };

		for (; !camera_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size);
		     size.index++) {
			struct camera_mode mode = { .format = formats[i] };

			if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
				mode.width = size.discrete.width;
				mode.height = size.discrete.height;
			} else {
				const struct v4l2_frmsize_stepwise *step =
					&size.stepwise;

				mode.width = CLAMP(max_width, step->min_width,
						   step->max_width);
				mode.height = CLAMP(max_height,
						    step->min_height,
						    step->max_height);
			}
			if (mode.width <= max_width &&
			    mode.height <= max_height && !(mode.width % 2) &&
			    !(mode.height % 2) &&
			    line_camera_frame_size_valid(mode.width,
							 mode.height)) {
				mode.fps = mode_fps(fd, mode.format, mode.width,
						    mode.height);
				if (!found || better_mode(&mode, best, fps))
					*best = mode;
				found = TRUE;
			}
			if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE)
				break;
		}
	}
	return found;
}

static gboolean camera_format(struct line_camera *camera,
			      unsigned int max_width, unsigned int max_height,
			      unsigned int fps)
{
	struct camera_mode mode = {
		.format = V4L2_PIX_FMT_YUYV,
		.width = MIN(max_width, CAMERA_FALLBACK_WIDTH),
		.height = MIN(max_height, CAMERA_FALLBACK_HEIGHT),
	};

	choose_mode(camera->fd, max_width, max_height, fps, &mode);
	struct v4l2_format format = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = {
			.width = mode.width,
			.height = mode.height,
			.pixelformat = mode.format,
			.field = V4L2_FIELD_ANY,
		},
	};
	struct v4l2_pix_format *pix = &format.fmt.pix;
	gboolean yuyv = mode.format == V4L2_PIX_FMT_YUYV;

	if (camera_ioctl(camera->fd, VIDIOC_S_FMT, &format) ||
	    pix->pixelformat != mode.format || pix->width % 2 ||
	    pix->height % 2 ||
	    !line_camera_frame_size_valid(pix->width, pix->height) ||
	    (yuyv &&
	     (pix->bytesperline < pix->width * 2 || pix->bytesperline > 16384)))
		return FALSE;
	camera->format = mode.format;
	camera->width = pix->width;
	camera->height = pix->height;
	camera->pitch = yuyv ? pix->bytesperline : 0;
	if (!yuyv) {
		camera->jpeg = tjInitDecompress();
		camera->jpeg_planes =
			g_malloc((gsize)camera->width * camera->height * 3);
		if (!camera->jpeg)
			return FALSE;
	}

	struct v4l2_streamparm parm = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.parm.capture.timeperframe = { .numerator = 1,
					       .denominator = MAX(fps, 1) },
	};

	camera_ioctl(camera->fd, VIDIOC_S_PARM, &parm);
	if (!g_strcmp0(g_getenv("LINE_CALL_DEBUG"), "1"))
		g_printerr("line-call camera mode %ux%u %s up to %u fps\n",
			   camera->width, camera->height,
			   yuyv ? "YUYV" : "MJPEG", mode.fps);
	return TRUE;
}

static gboolean camera_buffers(struct line_camera *camera)
{
	struct v4l2_requestbuffers request = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.count = CAMERA_BUFFERS,
	};

	if (camera_ioctl(camera->fd, VIDIOC_REQBUFS, &request) ||
	    !request.count || request.count > CAMERA_BUFFERS)
		return FALSE;
	for (unsigned int i = 0; i < request.count; i++) {
		struct v4l2_buffer buffer = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};

		if (camera_ioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) ||
		    buffer.length < (camera->pitch ?
					     camera->pitch * camera->height :
					     1) ||
		    buffer.length > 16 * 1024 * 1024)
			return FALSE;
		void *data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
				  MAP_SHARED, camera->fd, buffer.m.offset);

		if (data == MAP_FAILED)
			return FALSE;
		camera->buffers[i] =
			(struct camera_buffer){ data, buffer.length };
		camera->count++;
		if (camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer))
			return FALSE;
	}
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	camera->streaming = camera_ioctl(camera->fd, VIDIOC_STREAMON, &type) ==
			    0;
	return camera->streaming;
}

struct line_camera *line_camera_open(unsigned int max_width,
				     unsigned int max_height, unsigned int fps,
				     GError **error)
{
	const char *device = g_getenv("LINE_CAMERA_DEVICE");

	if (!device || !*device)
		device = "/dev/video0";
	int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		g_set_error(error, LINE_ERROR, 1,
			    "Could not open the camera: %s.",
			    g_strerror(errno));
		return NULL;
	}
	struct line_camera *camera = g_new0(struct line_camera, 1);
	struct v4l2_capability capability;

	camera->fd = fd;
	if (camera_ioctl(fd, VIDIOC_QUERYCAP, &capability))
		goto unsupported;
	guint32 caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
			       capability.device_caps :
			       capability.capabilities;

	if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING) ||
	    !camera_format(camera, max_width, max_height, fps))
		goto unsupported;
	if (!camera_buffers(camera)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Could not start the camera. Check whether another application is using it.");
		line_camera_close(camera);
		return NULL;
	}
	camera->pixels = g_malloc(camera->width * camera->height * 3 / 2);
	return camera;

unsupported:
	g_set_error_literal(
		error, LINE_ERROR, 1,
		"The camera does not provide a supported V4L2 video stream.");
	line_camera_close(camera);
	return NULL;
}

int line_camera_fd(const struct line_camera *camera)
{
	return camera ? camera->fd : -1;
}

/* JFIF uses full-range YCbCr; VP8 and the window expect video range. */
static guint8 video_luma(guint8 v)
{
	return 16 + (v * 219 + 127) / 255;
}

static guint8 video_chroma(int v)
{
	return 128 + ((v - 128) * 224 + (v >= 128 ? 127 : -127)) / 255;
}

static gboolean decode_jpeg(struct line_camera *camera, const guint8 *data,
			    gsize size, struct line_camera_frame *frame)
{
	unsigned int w = camera->width, h = camera->height;
	int jw, jh, subsamp, colorspace;

	if (!size || size > G_MAXULONG ||
	    tjDecompressHeader3(camera->jpeg, data, size, &jw, &jh, &subsamp,
				&colorspace) ||
	    jw != (int)w || jh != (int)h || subsamp < 0 ||
	    subsamp == TJSAMP_GRAY)
		return FALSE;
	int cw = tjPlaneWidth(1, jw, subsamp),
	    ch = tjPlaneHeight(1, jh, subsamp);

	if (cw <= 0 || ch <= 0 ||
	    (gsize)w * h + 2 * (gsize)cw * ch > (gsize)w * h * 3)
		return FALSE;
	guint8 *planes[3] = { camera->jpeg_planes,
			      camera->jpeg_planes + (gsize)w * h,
			      camera->jpeg_planes + (gsize)w * h +
				      (gsize)cw * ch };
	int strides[3] = { (int)w, cw, cw };

	if (tjDecompressToYUVPlanes(camera->jpeg, data, size, planes, jw,
				    strides, jh, TJFLAG_FASTDCT))
		return FALSE;
	guint8 *y = camera->pixels, *u = y + w * h, *v = u + w * h / 4;
	unsigned int sx = cw / (w / 2), sy = ch / (h / 2);

	if (!sx || !sy || sx > 2 || sy > 2)
		return FALSE;
	for (gsize i = 0; i < (gsize)w * h; i++)
		y[i] = video_luma(planes[0][i]);
	for (unsigned int row = 0; row < h / 2; row++)
		for (unsigned int x = 0; x < w / 2; x++) {
			for (int c = 1; c <= 2; c++) {
				const guint8 *p = planes[c] +
						  (gsize)row * sy * cw + x * sx;
				int sum = 0;

				for (unsigned int dy = 0; dy < sy; dy++)
					for (unsigned int dx = 0; dx < sx; dx++)
						sum += p[dy * cw + dx];
				(c == 1 ? u : v)[row * (w / 2) + x] =
					video_chroma((sum + sx * sy / 2) /
						     (sx * sy));
			}
		}
	*frame = (struct line_camera_frame){
		.plane = { y, u, v },
		.stride = { (int)w, (int)w / 2, (int)w / 2 },
		.width = w,
		.height = h,
	};
	return TRUE;
}

static void copy_frame(struct line_camera *camera, const guint8 *source,
		       struct line_camera_frame *frame)
{
	unsigned int w = camera->width, h = camera->height;
	guint8 *y = camera->pixels, *u = y + w * h, *v = u + w * h / 4;

	*frame = (struct line_camera_frame){
		.plane = { y, u, v },
		.stride = { (int)w, (int)w / 2, (int)w / 2 },
		.width = w,
		.height = h,
	};
	for (unsigned int row = 0; row < h; row += 2) {
		const guint8 *top = source + row * camera->pitch;
		const guint8 *bottom = top + camera->pitch;

		for (unsigned int x = 0; x < w; x += 2) {
			unsigned int offset = x * 2;

			y[x] = top[offset];
			y[x + 1] = top[offset + 2];
			y[w + x] = bottom[offset];
			y[w + x + 1] = bottom[offset + 2];
			u[x / 2] =
				(top[offset + 1] + bottom[offset + 1] + 1) / 2;
			v[x / 2] =
				(top[offset + 3] + bottom[offset + 3] + 1) / 2;
		}
		y += 2 * w;
		u += w / 2;
		v += w / 2;
	}
}

int line_camera_read(struct line_camera *camera,
		     struct line_camera_frame *frame, GError **error)
{
	struct v4l2_buffer buffer = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				      .memory = V4L2_MEMORY_MMAP };

	if (camera_ioctl(camera->fd, VIDIOC_DQBUF, &buffer)) {
		if (errno == EAGAIN)
			return 0;
		goto failed;
	}
	if (buffer.index >= camera->count)
		goto failed;
	const struct camera_buffer *mapped = &camera->buffers[buffer.index];
	size_t required = camera->jpeg ? 4 :
					 (camera->height - 1) * camera->pitch +
						 camera->width * 2;
	gboolean complete = !(buffer.flags & V4L2_BUF_FLAG_ERROR) &&
			    buffer.bytesused >= required &&
			    buffer.bytesused <= mapped->size;

	if (complete && camera->jpeg)
		complete = decode_jpeg(camera, mapped->data, buffer.bytesused,
				       frame);
	else if (complete)
		copy_frame(camera, mapped->data, frame);
	if (camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer))
		goto failed;
	return complete ? 1 : 0;

failed:
	g_set_error_literal(error, LINE_ERROR, 1,
			    "The camera stopped sending video.");
	return -1;
}
