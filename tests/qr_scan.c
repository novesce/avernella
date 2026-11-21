// SPDX-License-Identifier: GPL-3.0-or-later
#include "qr_scan.h"
#include <dlfcn.h>
#include <qrencode.h>
#include <string.h>

static const char first[] = "https://line.me/ti/p/fixture-first";
static const char second[] = "https://line.me/ti/p/fixture-other";
static gboolean hide_library;

void *__real_dlopen(const char *path, int flags);

void *__wrap_dlopen(const char *path, int flags)
{
	if (hide_library && path && !strcmp(path, "libzbar.so.0"))
		return NULL;
	return __real_dlopen(path, flags);
}

static void paint_code(cairo_t *cr, const char *payload, int left)
{
	QRcode *qr = QRcode_encodeString8bit(payload, 0, QR_ECLEVEL_M);

	g_assert_nonnull(qr);
	for (int y = 0; y < qr->width; y++) {
		for (int x = 0; x < qr->width; x++) {
			if (qr->data[y * qr->width + x] & 1)
				cairo_rectangle(cr, left + x * 5, 20 + y * 5, 5,
						5);
		}
	}
	cairo_fill(cr);
	QRcode_free(qr);
}

static cairo_surface_t *fixture(gboolean multiple)
{
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 190);
	cairo_t *cr = cairo_create(surface);

	paint_code(cr, first, 20);
	if (multiple)
		paint_code(cr, second, 220);
	cairo_destroy(cr);
	return surface;
}

static void read_single(void)
{
	void *zbar = dlopen("libzbar.so.0", RTLD_NOW | RTLD_LOCAL);

	if (!zbar) {
		g_test_skip("ZBar runtime is not installed");
		return;
	}
	dlclose(zbar);
	cairo_surface_t *image = fixture(FALSE);

	g_autoptr(GError) error = NULL;
	g_autofree char *result = line_qr_scan(image, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, first);
	cairo_surface_destroy(image);
	image = fixture(TRUE);
	g_assert_null(line_qr_scan(image, &error));
	g_assert_no_error(error);
	cairo_surface_destroy(image);
}

static void missing_library(void)
{
	cairo_surface_t *image = fixture(FALSE);

	g_autoptr(GError) error = NULL;

	hide_library = TRUE;
	g_assert_null(line_qr_scan(image, &error));
	hide_library = FALSE;
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
	g_assert_nonnull(strstr(error->message, "ZBar"));
	cairo_surface_destroy(image);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/qr-scan/personal-image", read_single);
	g_test_add_func("/qr-scan/missing-runtime", missing_library);
	return g_test_run();
}
