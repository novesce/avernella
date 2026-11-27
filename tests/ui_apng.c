// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_apng.h"
#include <string.h>
#include <zlib.h>

/*
 * Tiny, independently assembled PNG chunks. FFmpeg's apng demuxer and decoder
 * produced the four RGBA reference frames for the first fixture.
 */
static const char mixed[] =
	"iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAYAAACddGYaAAAACGFjVEwAAAAEAAAAApLDB/wAAAAaZmNU"
	"TAAAAAAAAAADAAAAAgAAAAAAAAAAAAEACgAAP7ZcWAAAABFJREFUeJxj+M/A8B+GGZA5AJt+C/VysLk8"
	"AAAAGmZjVEwAAAABAAAAAQAAAAIAAAABAAAAAAABAAUCAV+SepQAAAAIZmRBVAAAAAJ4nGNgIVFVYgAA"
	"AA9mZEFUAAAAA2D438AAIgAMgQL/bAvLBQAAABpmY1RMAAAABAAAAAIAAAABAAAAAAAAAAEAAwAKAQA8"
	"2X5/AAAACGZkQVQAAAAFeJxjYJNxiXIAAAARZmRBVAAAAAb4z/CfgYGBAQAL/QH/xh8M6gAAABpmY1RM"
	"AAAABwAAAAIAAAABAAAAAQAAAAAABAAKAACG/n5nAAAACGZkQVQAAAAIeJxjYGvhTcMAAAARZmRBVAAA"
	"AAlgYGD4/5/hPwAIAQL+KHx64QAAAABJRU5ErkJggg==";
static const char palette[] =
	"iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAMAAACqqpYoAAAACGFjVEwAAAACAAAAAPONk3AAAAAJUExU"
	"RQAAAP8AAAD/APSDv34AAAADdFJOUwD//0RQ1iEAAAAQSURBVHicY2BiYmJgYmICAAA4AA3FXaubAAAA"
	"GmZjVEwAAAAAAAAAAQAAAAEAAAABAAAAAAACAAACAOPwcpQAAAAIZmRBVAAAAAF4nGNgZvEvsgAAAApm"
	"ZEFUAAAAAgQAAAMAAugJ1OMAAAAaZmNUTAAAAAMAAAABAAAAAQAAAAAAAAABAAAAAQAAu9jECAAAAAhm"
	"ZEFUAAAABHicY2CuEaDCAAAACmZkQVQAAAAFAgAABAADib8vYwAAAABJRU5ErkJggg==";

#define R 0xffff0000u
#define B_OVER_R 0xff7f0080u
#define G 0xff00ff00u
#define Y 0xffffff00u

static const guint32 frames[][6] = { { R, R, R, R, R, R },
				     { R, B_OVER_R, R, R, B_OVER_R, R },
				     { R, R, R, G, 0, R },
				     { R, 0, Y, 0, 0, R } };

static void check_pixels(cairo_surface_t *surface, const guint32 expected[6])
{
	g_assert_cmpint(cairo_image_surface_get_width(surface), ==, 3);
	g_assert_cmpint(cairo_image_surface_get_height(surface), ==, 2);
	cairo_surface_flush(surface);
	guint8 *data = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);

	for (int i = 0; i < 6; i++) {
		guint32 argb;

		memcpy(&argb, data + (i / 3) * stride + (i % 3) * 4, 4);
		g_assert_cmphex(argb, ==, expected[i]);
	}
}

static void check_frame(cairo_surface_t *poster, gint64 now, gint64 next,
			const guint32 expected[6])
{
	gint64 until = -1;
	cairo_surface_t *frame = ui_apng_frame(poster, now, &until);

	g_assert_true(frame != poster);
	g_assert_cmpint(until, ==, next);
	check_pixels(frame, expected);
}

static void check_poster(cairo_surface_t *poster)
{
	gint64 until = -1;

	g_assert_true(ui_apng_frame(poster, 42, &until) == poster);
	g_assert_cmpint(until, ==, 0);
}

static void composition(void)
{
	gsize length;

	g_autofree guint8 *raw = g_base64_decode(mixed, &length);
	cairo_surface_t *poster =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 3, 2);
	ui_apng_attach(poster, raw, length);
	const gint64 start = 12000000;

	check_frame(poster, start, start + 100000, frames[0]);
	check_frame(poster, start + 99999, start + 100000, frames[0]);
	check_frame(poster, start + 100000, start + 300000, frames[1]);
	check_frame(poster, start + 300000, start + 600000, frames[2]);
	check_frame(poster, start + 600000, start + 1000000, frames[3]);
	check_frame(poster, start + 1000000, start + 1100000, frames[0]);
	check_frame(poster, start + 1600000, 0, frames[3]);
	check_frame(poster, G_MAXINT64, 0, frames[3]);
	cairo_surface_destroy(poster);
}

static void separate_default_and_palette(void)
{
	gsize length;
	g_autofree guint8 *raw = g_base64_decode(palette, &length);
	cairo_surface_t *poster =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 3, 2);
	cairo_t *cr = cairo_create(poster);

	cairo_set_source_rgb(cr, 0, 1, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	ui_apng_attach(poster, raw, length);
	const guint32 f0[] = { 0, R, 0, 0, 0, 0 }, f1[] = { 0, 0, 0, G, 0, 0 };
	/* The all-green default PNG image never seeds an animation frame. */
	check_frame(poster, 0, 20000, f0);
	check_frame(poster, 20000, 0, f1);
	check_frame(poster, 1000000, 0,
		    f1); /* indefinite loop mapped to one play */
	ui_apng_replay(poster);
	check_frame(poster, 4000000, 4020000, f0);
	check_frame(poster, 4020000, 0, f1);
	cairo_surface_destroy(poster);
}

static guint8 *chunk(guint8 *raw, gsize length, const char *kind, guint index)
{
	for (gsize i = 8; i + 12 <= length;) {
		guint32 n;

		memcpy(&n, raw + i, 4);
		n = GUINT32_FROM_BE(n);
		g_assert_cmpuint(n, <=, length - i - 12);
		if (!memcmp(raw + i + 4, kind, 4) && !index--)
			return raw + i;
		i += 12 + n;
	}
	g_error("missing fixture chunk %.4s", kind);
}

static void mutate(guint8 *raw, gsize length, const char *kind, guint index,
		   guint offset, guint32 value)
{
	guint8 *p = chunk(raw, length, kind, index);
	guint32 n;

	memcpy(&n, p, 4);
	n = GUINT32_FROM_BE(n);
	g_assert_cmpuint(offset + 4, <=, n);
	value = GUINT32_TO_BE(value);
	memcpy(p + 8 + offset, &value, 4);
	guint32 crc = GUINT32_TO_BE((guint32)crc32(0, p + 4, 4 + n));

	memcpy(p + 8 + n, &crc, 4);
}

static void invalid(void)
{
	gsize length;
	g_autofree guint8 *original = g_base64_decode(mixed, &length);
	cairo_surface_t *poster =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 3, 2);
	for (int i = 0; i < 8; i++) {
		g_autofree guint8 *raw = g_memdup2(original, length);
		gsize n = length;

		switch (i) {
		case 0:
			raw[length - 1] ^= 1;
			break;
		case 1:
			n--;
			break;
		case 2:
			mutate(raw, n, "acTL", 0, 0, 129);
			break;
		case 3:
			mutate(raw, n, "fcTL", 1, 12, 3);
			break;
		case 4:
			mutate(raw, n, "fdAT", 0, 0, 17);
			break;
		case 5:
			mutate(raw, n, "acTL", 0, 0, 3);
			break;
		case 6:
			mutate(raw, n, "IHDR", 0, 0, 513);
			break;
		case 7:
			n = 4 * 1024 * 1024 + 1;
			break;
		}
		ui_apng_attach(poster, raw, n);
		check_poster(poster);
	}
	/* Release partial decodes and let a corrected image attach to this poster. */
	ui_apng_attach(poster, original, length);
	check_frame(poster, 0, 100000, frames[0]);
	cairo_surface_destroy(poster);
}

static void one_frame(void)
{
	gsize length;
	g_autofree guint8 *raw = g_base64_decode(palette, &length);

	mutate(raw, length, "acTL", 0, 0, 1);
	guint8 *end = chunk(raw, length, "fcTL", 1);

	memcpy(end, chunk(raw, length, "IEND", 0), 12);
	cairo_surface_t *poster =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 3, 2);
	ui_apng_attach(poster, raw, (gsize)(end - raw) + 12);
	const guint32 expected[] = { 0, R, 0, 0, 0, 0 };

	check_frame(poster, 0, 0, expected);
	check_frame(poster, 999999, 0, expected);
	cairo_surface_destroy(poster);
}

static void aggregate_limit(void)
{
	cairo_surface_t *posters[9];
	gsize length;
	g_autofree guint8 *raw = g_base64_decode(mixed, &length);

	for (guint i = 0; i < G_N_ELEMENTS(posters); i++) {
		posters[i] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							512, 512);
		ui_apng_attach(posters[i], raw, length);
	}
	for (int i = 0; i < 8; i++)
		g_assert_true(ui_apng_frame(posters[i], 0, NULL) != posters[i]);
	check_poster(posters[8]); /* 9 × 4 frames × 512² × 4 bytes > 32 MiB */
	cairo_surface_destroy(posters[0]);
	ui_apng_attach(posters[8], raw, length);
	g_assert_true(ui_apng_frame(posters[8], 0, NULL) != posters[8]);
	for (int i = 1; i < 9; i++)
		cairo_surface_destroy(posters[i]);
}

static void gif_animation(void)
{
	/* Minimal 2x1 2-frame GIF89a: frame 0 red (#ff0000), frame 1 blue (#0000ff), 50ms each, infinite loop. */
	static const guint8 gif[] = {
		'G', 'I', 'F', '8', '9', 'a', 2, 0, 1, 0, 0x80, 0,
		0, /* 2x1, GCT 2 entries */
		0xff, 0x00, 0x00, /* color 0: red */
		0x00, 0x00, 0xff, /* color 1: blue */
		/* NETSCAPE2.0 loop=0 */
		0x21, 0xff, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2',
		'.', '0', 3, 1, 0, 0, 0,
		/* GCE frame 0: delay=5 (50ms) */
		0x21, 0xf9, 4, 0, 5, 0, 0, 0,
		/* Image 0: 2x1, color 0,0 -> LZW min=2: clear(4), 0, 0, eoi(5) = 0b00101_000_000_100 = 0x44, 0x01? */
		0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 2, 0x04, 0x0a,
		0, /* 3-bit codes: 4(100), 0(000), 0(000), 5(101) -> byte0=00_000_100=0x04, byte1=0000101_0=0x0a */
		/* GCE frame 1: delay=5 (50ms) */
		0x21, 0xf9, 4, 0, 5, 0, 0, 0,
		/* Image 1: 2x1, color 1,1 -> 3-bit codes: 4(100), 1(001), 1(001), 5(101) -> byte0=01_001_100=0x4c, byte1=0000101_0=0x0a */
		0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 2, 0x4c, 0x0a, 0, 0x3b
	};
	cairo_surface_t *poster =
		ui_gif_decode(gif, sizeof(gif), 64, 512, TRUE);
	g_assert_nonnull(poster);
	g_assert_cmpint(cairo_image_surface_get_width(poster), ==, 2);
	g_assert_cmpint(cairo_image_surface_get_height(poster), ==, 1);

	gint64 until = 0;
	cairo_surface_t *f0 = ui_apng_frame(poster, 1000, &until);
	g_assert_nonnull(f0);
	g_assert_cmpint(until, ==, 51000);
	g_assert_cmphex(*(const guint32 *)cairo_image_surface_get_data(f0), ==,
			0xffff0000u);

	cairo_surface_t *f1 = ui_apng_frame(poster, 51000, &until);
	g_assert_nonnull(f1);
	g_assert_cmpint(until, ==, 101000);
	g_assert_cmphex(*(const guint32 *)cairo_image_surface_get_data(f1), ==,
			0xff0000ffu);

	/* Loops continuously back to frame 0. */
	cairo_surface_t *f2 = ui_apng_frame(poster, 101000, &until);
	g_assert_cmphex(*(const guint32 *)cairo_image_surface_get_data(f2), ==,
			0xffff0000u);
	cairo_surface_destroy(poster);

	/* Truncated / corrupt GIF returns NULL safely. */
	g_assert_null(ui_gif_decode(gif, 10, 64, 512, TRUE));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/apng/timing-blend-dispose", composition);
	g_test_add_func("/apng/separate-default-palette",
			separate_default_and_palette);
	g_test_add_func("/apng/malformed-static-fallback", invalid);
	g_test_add_func("/apng/single-frame-separate-default", one_frame);
	g_test_add_func("/apng/bounded-aggregate", aggregate_limit);
	g_test_add_func("/apng/gif-animation", gif_animation);
	return g_test_run();
}
