// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include <glib/gstdio.h>
#include <unistd.h>

static const char url[] = "https://example.org/card.png";
static Json *request;
static guint request_tag;

void __wrap_ui_rpc(const char *method, Json *params, guint tag)
{
	g_assert_cmpstr(method, ==, "flexImage");
	g_assert_null(request);
	g_assert_cmpstr(ui_string(params, "url", NULL), ==, url);
	request = params;
	request_tag = tag;
}

static void reply(const char *path, const char *error)
{
	Json *params = g_steal_pointer(&request);
	Json *result = json_object_new_object();

	g_assert_nonnull(params);
	if (path)
		json_object_object_add(result, "path",
				       json_object_new_string(path));
	ui_media_reply("flexImage", params, request_tag, result, error);
	json_object_put(params);
	json_object_put(result);
}

/* A network outage leaves the signed-in UI connected to the worker. */
static void fail_download(void)
{
	g_assert_null(ui_flex_image(url));
	reply(NULL, "Network is unreachable");
	g_assert_true(ui_flex_image_failed(url));
	g_assert_null(ui_flex_image(url));
	g_assert_null(request);
}

static void wait_for_decode(void)
{
	SDL_Event ev;

	while (SDL_WaitEventTimeout(&ev, 5000)) {
		if (ev.type >= SDL_USEREVENT && ui_media_event(&ev))
			return;
	}
	g_error("Timed out waiting for image decode");
}

static void flex_recovers(void)
{
	g_autofree char *path = NULL;
	guint32 pixel = 0xffa034f2;
	cairo_surface_t *png, *image;
	int fd = g_file_open_tmp("avernella-refresh-XXXXXX", &path, NULL);

	g_assert_cmpint(fd, >=, 0);
	close(fd);
	png = cairo_image_surface_create_for_data((unsigned char *)&pixel,
						  CAIRO_FORMAT_ARGB32, 1, 1, 4);
	g_assert_cmpint(cairo_surface_write_to_png(png, path), ==,
			CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(png);

	fail_download();
	ui_media_refresh();
	g_assert_false(ui_flex_image_failed(url));
	g_assert_null(request);
	g_assert_null(ui_flex_image(url));
	g_assert_nonnull(request);
	g_assert_null(ui_flex_image(url));
	reply(path, NULL);
	ui_media_refresh();
	wait_for_decode();
	image = ui_flex_image(url);
	g_assert_nonnull(image);
	g_assert_cmpint(cairo_image_surface_get_width(image), ==, 1);
	g_assert_cmpint(cairo_image_surface_get_height(image), ==, 1);
	g_assert_cmpmem(cairo_image_surface_get_data(image), sizeof(pixel),
			&pixel, sizeof(pixel));

	ui_media_refresh();
	g_assert_true(ui_flex_image(url) == image);
	g_assert_null(request);
	g_assert_cmpint(g_remove(path), ==, 0);
	ui_media_reset();
}

static void flex_late_failure(void)
{
	guint before;

	g_assert_null(ui_flex_image(url));
	g_assert_nonnull(request);
	before = request_tag;

	ui_media_refresh();
	ui_media_refresh();
	g_assert_null(ui_flex_image(url));
	reply(NULL, "Request timed out");
	g_assert_false(ui_flex_image_failed(url));
	g_assert_null(ui_flex_image(url));
	g_assert_nonnull(request);
	g_assert_cmpuint(request_tag, !=, before);
	reply(NULL, "Still offline");
	g_assert_true(ui_flex_image_failed(url));
	g_assert_null(ui_flex_image(url));
	g_assert_null(request);
	ui_media_reset();
}

static void flex_invalid_image(void)
{
	g_autofree char *path = NULL;
	static const char corrupt[] = "invalid image";
	int fd = g_file_open_tmp("avernella-refresh-XXXXXX", &path, NULL);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(write(fd, corrupt, sizeof(corrupt)), ==,
			sizeof(corrupt));
	close(fd);
	g_assert_null(ui_flex_image(url));
	reply(path, NULL);
	wait_for_decode();
	g_assert_true(ui_flex_image_failed(url));

	ui_media_refresh();
	g_assert_true(ui_flex_image_failed(url));
	g_assert_null(ui_flex_image(url));
	g_assert_null(request);
	g_assert_cmpint(g_remove(path), ==, 0);
	ui_media_reset();
}

static void flex_manual_refresh(void)
{
	g_autofree char *path = NULL;
	guint32 pixel = 0xff112233;
	cairo_surface_t *png;
	int fd = g_file_open_tmp("avernella-refresh-XXXXXX", &path, NULL);

	g_assert_cmpint(fd, >=, 0);
	close(fd);
	png = cairo_image_surface_create_for_data((unsigned char *)&pixel,
						  CAIRO_FORMAT_ARGB32, 1, 1, 4);
	g_assert_cmpint(cairo_surface_write_to_png(png, path), ==,
			CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(png);

	/* Manual refresh evicts a cached Flex/Rich image and re-fetches it. */
	g_assert_null(ui_flex_image(url));
	reply(path, NULL);
	wait_for_decode();
	g_assert_nonnull(ui_flex_image(url));
	g_assert_null(request);

	ui_media_refresh_flex();
	g_assert_null(ui_flex_image(url));
	g_assert_nonnull(request);

	/* A late reply from before another manual refresh must not wedge busy. */
	guint stale_tag = request_tag;
	Json *stale_params = g_steal_pointer(&request);

	ui_media_refresh_flex();
	ui_media_reply("flexImage", stale_params, stale_tag, NULL, "Cancelled");
	json_object_put(stale_params);

	g_assert_null(ui_flex_image(url));
	g_assert_nonnull(request);
	reply(path, NULL);
	wait_for_decode();
	g_assert_nonnull(ui_flex_image(url));
	g_assert_cmpint(g_remove(path), ==, 0);
	ui_media_reset();
}

int main(int argc, char **argv)
{
	int result;

	g_test_init(&argc, &argv, NULL);
	g_assert_cmpint(SDL_Init(SDL_INIT_EVENTS), ==, 0);
	ui.connected = TRUE;
	g_test_add_func("/refresh/flex-image", flex_recovers);
	g_test_add_func("/refresh/flex-late-failure", flex_late_failure);
	g_test_add_func("/refresh/flex-invalid-image", flex_invalid_image);
	g_test_add_func("/refresh/flex-manual-refresh", flex_manual_refresh);
	result = g_test_run();

	ui_media_shutdown();
	SDL_Quit();
	return result;
}
