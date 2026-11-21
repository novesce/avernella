// SPDX-License-Identifier: GPL-3.0-or-later
#include "qr_scan.h"
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>
#include <zbar.h>

/* libzbar is optional, so its functions are loaded at run time. */
struct qr_api {
	__typeof__(&zbar_image_scanner_create) zbar_image_scanner_create;
	__typeof__(&zbar_image_scanner_destroy) zbar_image_scanner_destroy;
	__typeof__(&zbar_image_scanner_set_config) zbar_image_scanner_set_config;
	__typeof__(&zbar_image_create) zbar_image_create;
	__typeof__(&zbar_image_destroy) zbar_image_destroy;
	__typeof__(&zbar_image_set_format) zbar_image_set_format;
	__typeof__(&zbar_image_set_size) zbar_image_set_size;
	__typeof__(&zbar_image_set_data) zbar_image_set_data;
	__typeof__(&zbar_image_first_symbol) zbar_image_first_symbol;
	__typeof__(&zbar_scan_image) zbar_scan_image;
	__typeof__(&zbar_symbol_get_type) zbar_symbol_get_type;
	__typeof__(&zbar_symbol_get_data) zbar_symbol_get_data;
	__typeof__(&zbar_symbol_get_data_length) zbar_symbol_get_data_length;
};

static const struct {
	const char *name;
	size_t offset;
} qr_symbols[] = {
	{ "zbar_image_scanner_create",
	  offsetof(struct qr_api, zbar_image_scanner_create) },
	{ "zbar_image_scanner_destroy",
	  offsetof(struct qr_api, zbar_image_scanner_destroy) },
	{ "zbar_image_scanner_set_config",
	  offsetof(struct qr_api, zbar_image_scanner_set_config) },
	{ "zbar_image_create", offsetof(struct qr_api, zbar_image_create) },
	{ "zbar_image_destroy", offsetof(struct qr_api, zbar_image_destroy) },
	{ "zbar_image_set_format",
	  offsetof(struct qr_api, zbar_image_set_format) },
	{ "zbar_image_set_size", offsetof(struct qr_api, zbar_image_set_size) },
	{ "zbar_image_set_data", offsetof(struct qr_api, zbar_image_set_data) },
	{ "zbar_image_first_symbol",
	  offsetof(struct qr_api, zbar_image_first_symbol) },
	{ "zbar_scan_image", offsetof(struct qr_api, zbar_scan_image) },
	{ "zbar_symbol_get_type",
	  offsetof(struct qr_api, zbar_symbol_get_type) },
	{ "zbar_symbol_get_data",
	  offsetof(struct qr_api, zbar_symbol_get_data) },
	{ "zbar_symbol_get_data_length",
	  offsetof(struct qr_api, zbar_symbol_get_data_length) },
};

static void *qr_api_open(struct qr_api *api)
{
	void *library = dlopen("libzbar.so.0", RTLD_NOW | RTLD_LOCAL);

	if (!library)
		return NULL;
	/* Every member is a function pointer the size of dlsym's result. */
	G_STATIC_ASSERT(sizeof(*api) ==
			G_N_ELEMENTS(qr_symbols) * sizeof(void *));
	for (gsize i = 0; i < G_N_ELEMENTS(qr_symbols); i++) {
		void *symbol = dlsym(library, qr_symbols[i].name);

		if (!symbol) {
			dlclose(library);
			return NULL;
		}
		memcpy((char *)api + qr_symbols[i].offset, &symbol,
		       sizeof(symbol));
	}
	return library;
}

static char *scan_pixels(const struct qr_api *api, const guint8 *gray, int w,
			 int h)
{
	zbar_image_scanner_t *scanner = api->zbar_image_scanner_create();
	zbar_image_t *image = api->zbar_image_create();
	char *value = NULL;

	if (scanner && image &&
	    !api->zbar_image_scanner_set_config(scanner, ZBAR_NONE,
						ZBAR_CFG_ENABLE, 0) &&
	    !api->zbar_image_scanner_set_config(scanner, ZBAR_QRCODE,
						ZBAR_CFG_ENABLE, 1)) {
		api->zbar_image_set_format(image,
					   zbar_fourcc('Y', '8', '0', '0'));
		api->zbar_image_set_size(image, w, h);
		api->zbar_image_set_data(image, gray, (gsize)w * h, NULL);
		if (api->zbar_scan_image(scanner, image) == 1) {
			const zbar_symbol_t *qr =
				api->zbar_image_first_symbol(image);

			if (qr &&
			    api->zbar_symbol_get_type(qr) == ZBAR_QRCODE) {
				const char *data =
					api->zbar_symbol_get_data(qr);
				guint n = api->zbar_symbol_get_data_length(qr);

				if (data && n > 0 && n <= 2048 &&
				    !memchr(data, 0, n) &&
				    g_utf8_validate(data, n, NULL))
					value = g_strndup(data, n);
			}
		}
	}
	if (image)
		api->zbar_image_destroy(image);
	if (scanner)
		api->zbar_image_scanner_destroy(scanner);
	return value;
}

char *line_qr_scan(cairo_surface_t *surface, GError **error)
{
	if (!surface ||
	    cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
	    cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return NULL;
	cairo_format_t format = cairo_image_surface_get_format(surface);
	int w = cairo_image_surface_get_width(surface);
	int h = cairo_image_surface_get_height(surface);

	if ((format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGB24) ||
	    w <= 0 || h <= 0)
		return NULL;
	struct qr_api api;
	void *library = qr_api_open(&api);

	if (!library) {
		g_set_error_literal(
			error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
			"Install the ZBar runtime library to read QR images.");
		return NULL;
	}
	g_autofree guint8 *gray = g_try_malloc_n(w, h);
	char *value = NULL;

	if (gray) {
		cairo_surface_flush(surface);
		const guint8 *pixels = cairo_image_surface_get_data(surface);
		int stride = cairo_image_surface_get_stride(surface);

		for (int y = 0; y < h; y++) {
			const guint32 *row =
				(const void *)(pixels + (gsize)y * stride);

			for (int x = 0; x < w; x++) {
				guint v = row[x];
				guint lum = (77 * ((v >> 16) & 255) +
					     150 * ((v >> 8) & 255) +
					     29 * (v & 255)) >>
					    8;
				guint alpha = format == CAIRO_FORMAT_ARGB32 ?
						      v >> 24 :
						      255;

				gray[(gsize)y * w + x] =
					MIN(255u, lum + 255 - alpha);
			}
		}
		value = scan_pixels(&api, gray, w, h);
	}
	dlclose(library);
	return value;
}
