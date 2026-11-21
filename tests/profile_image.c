// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_image.h"
#include <png.h>
#include <string.h>
#include <turbojpeg.h>
#include <webp/encode.h>

/* line_own_picture_update is not exercised here; it needs these. */
J *line_own_profile(Line *s, GError **error)
{
	(void)s;
	(void)error;
	g_assert_not_reached();
	return NULL;
}

char *line_obs_token(Line *s, GError **error)
{
	(void)s;
	(void)error;
	g_assert_not_reached();
	return NULL;
}

gboolean line_obs_upload_profile(const char *token, const char *mid,
				 GBytes *jpeg, GError **error)
{
	(void)token;
	(void)mid;
	(void)jpeg;
	(void)error;
	g_assert_not_reached();
	return FALSE;
}

static void decoded(GBytes *jpeg, int *w, int *h, guint8 *center)
{
	gsize n;
	const guint8 *data = g_bytes_get_data(jpeg, &n);
	tjhandle tj = tjInitDecompress();
	int sub, space;

	g_assert_nonnull(tj);
	g_assert_cmpint(tjDecompressHeader3(tj, data, n, w, h, &sub, &space),
			==, 0);
	g_autofree guint8 *rgb = g_malloc((gsize)*w * *h * 3);

	g_assert_cmpint(tjDecompress2(tj, data, n, rgb, *w, 0, *h, TJPF_RGB, 0),
			==, 0);
	memcpy(center, rgb + ((gsize)(*h / 2) * *w + *w / 2) * 3, 3);
	tjDestroy(tj);
}

/* A wide PNG: the centered square is red, the side bars are blue. */
static void png_square(void)
{
	enum {
		W = 1500,
		H = 900
	};
	g_autofree guint8 *rgba = g_malloc0((gsize)W * H * 4);

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			guint8 *p = rgba + ((gsize)y * W + x) * 4;
			gboolean inside = x >= (W - H) / 2 && x < (W + H) / 2;

			p[0] = inside ? 255 : 0;
			p[2] = inside ? 0 : 255;
			p[3] = 255;
		}
	png_image image = { .version = PNG_IMAGE_VERSION,
			    .width = W,
			    .height = H,
			    .format = PNG_FORMAT_RGBA };
	png_alloc_size_t size = 0;

	g_assert_true(
		png_image_write_get_memory_size(image, size, 0, rgba, 0, NULL));
	g_autofree guint8 *png = g_malloc(size);

	g_assert_true(png_image_write_to_memory(&image, png, &size, 0, rgba, 0,
						NULL));
	g_autoptr(GBytes) in = g_bytes_new(png, size);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) jpeg = line_profile_jpeg(in, &error);
	g_assert_no_error(error);
	int w, h;
	guint8 c[3];

	decoded(jpeg, &w, &h, c);
	g_assert_cmpint(w, ==, LINE_PROFILE_SIDE);
	g_assert_cmpint(h, ==, LINE_PROFILE_SIDE);
	g_assert_cmpint(c[0], >, 230);
	g_assert_cmpint(c[2], <, 25);
}

/* Small pictures keep their size; transparent WebP turns white. */
static void webp_transparent(void)
{
	enum {
		S = 64
	};
	g_autofree guint8 *rgba = g_malloc0((gsize)S * S * 4);
	guint8 *out = NULL;
	size_t n = WebPEncodeLosslessRGBA(rgba, S, S, S * 4, &out);

	g_assert_cmpuint(n, >, 0);
	g_autoptr(GBytes) in = g_bytes_new(out, n);
	WebPFree(out);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) jpeg = line_profile_jpeg(in, &error);
	g_assert_no_error(error);
	int w, h;
	guint8 c[3];

	decoded(jpeg, &w, &h, c);
	g_assert_cmpint(w, ==, S);
	g_assert_cmpint(h, ==, S);
	g_assert_cmpint(c[0], >, 245);
	g_assert_cmpint(c[1], >, 245);
}

/* A big JPEG is scaled inside the decoder, then averaged to 640. */
static void jpeg_tall(void)
{
	enum {
		W = 1700,
		H = 3000
	};
	g_autofree guint8 *rgb = g_malloc((gsize)W * H * 3);

	memset(rgb, 0x40, (gsize)W * H * 3);
	tjhandle tj = tjInitCompress();
	unsigned char *buf = NULL;
	unsigned long size = 0;

	g_assert_cmpint(tjCompress2(tj, rgb, W, 0, H, TJPF_RGB, &buf, &size,
				    TJSAMP_420, 90, 0),
			==, 0);
	tjDestroy(tj);
	g_autoptr(GBytes) in = g_bytes_new(buf, size);
	tjFree(buf);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) jpeg = line_profile_jpeg(in, &error);
	g_assert_no_error(error);
	int w, h;
	guint8 c[3];

	decoded(jpeg, &w, &h, c);
	g_assert_cmpint(w, ==, LINE_PROFILE_SIDE);
	g_assert_cmpint(h, ==, LINE_PROFILE_SIDE);
	g_assert_cmpint(ABS(c[0] - 0x40), <, 4);
}

static void rejects(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) gif = g_bytes_new_static("GIF89a....", 10);
	g_autoptr(GBytes) broken =
		g_bytes_new_static("\x89PNG\r\n\x1a\nxx", 10);

	g_assert_null(line_profile_jpeg(gif, &error));
	g_clear_error(&error);
	g_assert_null(line_profile_jpeg(broken, &error));
	g_clear_error(&error);
	g_assert_null(line_profile_jpeg(NULL, &error));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/profile-image/png-centered-square", png_square);
	g_test_add_func("/profile-image/webp-alpha-on-white", webp_transparent);
	g_test_add_func("/profile-image/jpeg-decoder-scaling", jpeg_tall);
	g_test_add_func("/profile-image/rejects-other-data", rejects);
	return g_test_run();
}
