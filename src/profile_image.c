// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_image.h"
#include "friends.h"
#include "obs_http.h"
#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <string.h>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <webp/decode.h>

enum {
	MAX_DIMENSION = 16384,
	MAX_PIXELS = 64 * 1024 * 1024,
	JPEG_QUALITY = 92
};

static GBytes *fail(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return NULL;
}

struct picture {
	guint8 *rgb; /* packed RGB, w * h * 3 bytes */
	int w, h;
};

static gboolean sane(int w, int h)
{
	return w > 0 && h > 0 && w <= MAX_DIMENSION && h <= MAX_DIMENSION &&
	       (gint64)w * h <= MAX_PIXELS;
}

/*
 * libjpeg-turbo scales by 1/2, 1/4 or 1/8 inside the IDCT, which is far
 * cheaper than decoding a full camera picture and averaging it down.
 */
static gboolean decode_jpeg(const guint8 *data, gsize n, struct picture *p)
{
	tjhandle tj = tjInitDecompress();
	int w, h, sub, space;
	gboolean ok = FALSE;

	if (!tj)
		return FALSE;
	if (tjDecompressHeader3(tj, data, n, &w, &h, &sub, &space) ||
	    !sane(w, h))
		goto done;
	int count;
	tjscalingfactor *factors = tjGetScalingFactors(&count);
	int sw = w, sh = h;

	for (int i = 0; factors && i < count; i++) {
		int fw = TJSCALED(w, factors[i]), fh = TJSCALED(h, factors[i]);

		if (MIN(fw, fh) >= LINE_PROFILE_SIDE && fw * fh < sw * sh) {
			sw = fw;
			sh = fh;
		}
	}
	p->rgb = g_malloc((gsize)sw * sh * 3);
	if (tjDecompress2(tj, data, n, p->rgb, sw, 0, sh, TJPF_RGB,
			  TJFLAG_FASTDCT)) {
		g_clear_pointer(&p->rgb, g_free);
		goto done;
	}
	p->w = sw;
	p->h = sh;
	ok = TRUE;
done:
	tjDestroy(tj);
	return ok;
}

static gboolean decode_png(const guint8 *data, gsize n, struct picture *p)
{
	png_image image = { .version = PNG_IMAGE_VERSION };

	if (!png_image_begin_read_from_memory(&image, data, n))
		return FALSE;
	if (!sane(image.width, image.height)) {
		png_image_free(&image);
		return FALSE;
	}
	image.format = PNG_FORMAT_RGB;
	gsize size = PNG_IMAGE_SIZE(image);
	png_color white = { 255, 255, 255 };

	p->rgb = g_malloc(size);
	if (!png_image_finish_read(&image, &white, p->rgb, 0, NULL)) {
		png_image_free(&image);
		g_clear_pointer(&p->rgb, g_free);
		return FALSE;
	}
	p->w = image.width;
	p->h = image.height;
	return TRUE;
}

static gboolean decode_webp(const guint8 *data, gsize n, struct picture *p)
{
	int w, h;

	if (!WebPGetInfo(data, n, &w, &h) || !sane(w, h))
		return FALSE;
	guint8 *rgba = WebPDecodeRGBA(data, n, &w, &h);

	if (!rgba)
		return FALSE;
	gsize count = (gsize)w * h;

	p->rgb = g_malloc(count * 3);
	for (gsize i = 0; i < count; i++) {
		const guint8 *in = rgba + i * 4;
		guint a = in[3];

		for (int c = 0; c < 3; c++)
			p->rgb[i * 3 + c] =
				(in[c] * a + 255 * (255 - a) + 127) / 255;
	}
	WebPFree(rgba);
	p->w = w;
	p->h = h;
	return TRUE;
}

/*
 * Centered square, area-averaged down to the output side. Each output
 * pixel sums the whole source box, so no detail aliases away.
 */
static guint8 *square(const struct picture *p, int *out_side)
{
	int side = MIN(p->w, p->h), out = MIN(side, LINE_PROFILE_SIDE);
	int x0 = (p->w - side) / 2, y0 = (p->h - side) / 2;
	guint8 *dst = g_malloc((gsize)out * out * 3);

	for (int y = 0; y < out; y++) {
		int sy0 = y0 + (int)((gint64)y * side / out);
		int sy1 = y0 + (int)((gint64)(y + 1) * side / out);

		for (int x = 0; x < out; x++) {
			int sx0 = x0 + (int)((gint64)x * side / out);
			int sx1 = x0 + (int)((gint64)(x + 1) * side / out);
			guint64 sum[3] = { 0 };

			for (int sy = sy0; sy < sy1; sy++) {
				const guint8 *row =
					p->rgb + ((gsize)sy * p->w + sx0) * 3;

				for (int sx = sx0; sx < sx1; sx++, row += 3) {
					sum[0] += row[0];
					sum[1] += row[1];
					sum[2] += row[2];
				}
			}
			guint64 area = (guint64)(sx1 - sx0) * (sy1 - sy0);
			guint8 *o = dst + ((gsize)y * out + x) * 3;

			for (int c = 0; c < 3; c++)
				o[c] = (sum[c] + area / 2) / area;
		}
	}
	*out_side = out;
	return dst;
}

GBytes *line_profile_jpeg(GBytes *image, GError **error)
{
	gsize n;
	const guint8 *data = image ? g_bytes_get_data(image, &n) : NULL;
	struct picture p = { 0 };
	gboolean ok = FALSE;

	if (!data || !n)
		return fail(error, "Choose a picture for your profile.");
	if (n >= 3 && !memcmp(data, "\xff\xd8\xff", 3))
		ok = decode_jpeg(data, n, &p);
	else if (n >= 8 && !memcmp(data, "\211PNG\r\n\032\n", 8))
		ok = decode_png(data, n, &p);
	else if (n >= 12 && !memcmp(data, "RIFF", 4) &&
		 !memcmp(data + 8, "WEBP", 4))
		ok = decode_webp(data, n, &p);
	else
		return fail(
			error,
			"Choose a JPEG, PNG or WebP picture for your profile.");
	if (!ok)
		return fail(error, "This picture could not be read.");
	int side = 0;
	guint8 *pixels = square(&p, &side);

	g_free(p.rgb);
	tjhandle tj = tjInitCompress();
	unsigned char *jpeg = NULL;
	unsigned long size = 0;

	if (!tj || tjCompress2(tj, pixels, side, 0, side, TJPF_RGB, &jpeg,
			       &size, TJSAMP_420, JPEG_QUALITY, 0)) {
		if (tj)
			tjDestroy(tj);
		tjFree(jpeg);
		g_free(pixels);
		return fail(error, "The profile picture could not be encoded.");
	}
	tjDestroy(tj);
	g_free(pixels);
	GBytes *bytes = g_bytes_new(jpeg, size);

	tjFree(jpeg);
	return bytes;
}

/* A named pipe or device must never block the worker. */
static GBytes *read_picture(const char *path, GError **error)
{
	int fd = path && *path ? open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK) :
				 -1;
	struct stat st;

	if (fd < 0)
		return fail(error, "Could not open the picture.");
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > LINE_PROFILE_FILE_LIMIT) {
		close(fd);
		return fail(error, "Choose a picture file of up to 20 MiB.");
	}
	gsize size = st.st_size, used = 0;
	guint8 *buf = g_malloc(size);

	while (used < size) {
		ssize_t got = read(fd, buf + used, size - used);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		used += got;
	}
	close(fd);
	if (used != size) {
		g_free(buf);
		return fail(error, "The picture could not be read completely.");
	}
	return g_bytes_new_take(buf, size);
}

J *line_own_picture_update(Line *s, const char *path, GError **error)
{
	g_autoptr(GBytes) original = read_picture(path, error);
	if (!original)
		return NULL;
	g_autoptr(GBytes) jpeg = line_profile_jpeg(original, error);
	if (!jpeg)
		return NULL;
	g_autoptr(LineSecret) token = line_obs_token(s, error);

	if (!token || !line_obs_upload_profile(token, s->mid, jpeg, error))
		return NULL;
	/* OBS updates the profile itself; read back the new picture path. */
	return line_own_profile(s, error);
}
