// SPDX-License-Identifier: GPL-3.0-or-later
#include "screencast.h"
#include "screencast_cinnamon.h"
#include "screencast_dma.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <gio/gio.h>
#include <spa/buffer/buffer.h>

#define PORTAL "org.freedesktop.portal.Desktop"
#define PATH "/org/freedesktop/portal/desktop"
#define CREATE "org.freedesktop.portal.ScreenCast"

enum scenario {
	CINNAMON,
	OTHER_DESKTOP,
	X11,
	SANDBOX,
	NO_SERVICE,
	DISABLED,
	DEFAULT_NONE,
	CUSTOM_BACKEND,
	DENIED,
	CANCELLED,
	LATER_METHOD
};

static guint native_calls;

struct line_cinnamon_cast *line_cinnamon_start(GMainContext *context,
					       GCancellable *cancel,
					       guint32 *node, GError **error)
{
	(void)context;
	(void)cancel;
	(void)node;
	native_calls++;
	g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN,
			    "No test compositor");
	return NULL;
}

void line_cinnamon_stop(struct line_cinnamon_cast *cast)
{
	(void)cast;
	g_assert_not_reached();
}

static void create_session(GDBusConnection *bus, const char *sender,
			   const char *path, const char *interface,
			   const char *method, GVariant *args,
			   GDBusMethodInvocation *call, gpointer data)
{
	enum scenario test = GPOINTER_TO_INT(data);
	const char *token = NULL;

	g_autoptr(GVariant) opts = g_variant_get_child_value(args, 0);

	(void)path;
	(void)interface;
	g_assert_cmpstr(method, ==, "CreateSession");
	if (test == DENIED) {
		g_dbus_method_invocation_return_error_literal(
			call, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
			"Access denied by policy");
		return;
	}
	g_assert_true(g_variant_lookup(opts, "handle_token", "&s", &token));
	g_autofree char *name = g_strdup(sender + 1);
	g_autofree char *request = g_strdup_printf(
		PATH "/request/%s/%s", g_strdelimit(name, ".", '_'), token);
	GVariantBuilder results;

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	if (test == LATER_METHOD)
		g_variant_builder_add(&results, "{sv}", "session_handle",
				      g_variant_new_string(PATH
							   "/session/mock"));
	g_dbus_method_invocation_return_value(call,
					      g_variant_new("(o)", request));
	g_assert_true(g_dbus_connection_emit_signal(
		bus, sender, request, "org.freedesktop.portal.Request",
		"Response",
		g_variant_new("(ua{sv})", test == CANCELLED ? 1u : 0u,
			      &results),
		NULL));
}

static void portal_conf(const char *dir, const char *name, const char *prefs)
{
	g_autofree char *directory =
		g_build_filename(dir, "xdg-desktop-portal", NULL);
	g_autofree char *path = g_build_filename(directory, name, NULL);

	g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, prefs, -1, NULL));
}

static void isolated(gconstpointer data)
{
	g_autofree char *path =
		g_strconcat(g_test_get_path(), "/subprocess", NULL);

	(void)data;
	g_test_trap_subprocess(path, 10 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
}

static void run(gconstpointer data)
{
	enum scenario test = GPOINTER_TO_INT(data);
	static const char *const message[] = {
		[CINNAMON] =
			"Screen sharing is unavailable in this Cinnamon session. Try GNOME or Plasma Wayland.",
		[OTHER_DESKTOP] =
			"Screen sharing is unavailable. Enable a ScreenCast portal for your desktop.",
		[DENIED] = "Access denied by policy",
		[CANCELLED] = "Screen sharing was cancelled.",
		[LATER_METHOD] = "SelectSources",
	};
	gboolean native_cinnamon =
		test == CINNAMON &&
		!g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
	const char *expected = !native_cinnamon && test < DENIED ?
				       message[OTHER_DESKTOP] :
				       message[test];

	const char *user = g_get_user_config_dir();
	const char *stock = "[preferred]\ndefault=xapp;gtk;\n";

	portal_conf(g_get_system_config_dirs()[0], "x-cinnamon-portals.conf",
		    stock);
	if (test == DISABLED || test == CUSTOM_BACKEND) {
		/* The first desktop name wins even over another Cinnamon file. */
		portal_conf(user, "x-cinnamon-portals.conf", stock);
		g_autofree char *policy = g_strconcat(
			stock, "org.freedesktop.impl.portal.ScreenCast=",
			test == DISABLED ? "none" : "gnome", "\n", NULL);
		portal_conf(user, "test-portals.conf", policy);
	} else if (test == DEFAULT_NONE) {
		/* User's generic config precedes the desktop's system config. */
		portal_conf(user, "portals.conf",
			    "[preferred]\ndefault=none\n");
	}
	g_unsetenv("XDG_DESKTOP_PORTAL_DIR");
	g_setenv("XDG_CURRENT_DESKTOP",
		 test == OTHER_DESKTOP ? "other-Cinnamon" : "test:X-Cinnamon",
		 TRUE);
	g_setenv("XDG_SESSION_TYPE", test == X11 ? "x11" : "wayland", TRUE);
	g_unsetenv("FLATPAK_ID");
	g_unsetenv("SNAP");
	if (test == SANDBOX)
		g_setenv("FLATPAK_ID", "fixture", TRUE);
	g_autoptr(GTestDBus) dbus = g_test_dbus_new(G_TEST_DBUS_NONE);

	g_test_dbus_up(dbus);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusConnection) bus =
		g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bus);
	const char *xml =
		test < DENIED ?
			"<node><interface name='org.freedesktop.portal.Settings'/></node>" :
			"<node><interface name='" CREATE
			"'><method name='CreateSession'>"
			"<arg type='a{sv}' direction='in'/><arg type='o' direction='out'/>"
			"</method></interface></node>";
	g_autoptr(GDBusNodeInfo) info =
		g_dbus_node_info_new_for_xml(xml, &error);
	static const GDBusInterfaceVTable mock = { .method_call =
							   create_session };

	g_assert_no_error(error);
	guint object = g_dbus_connection_register_object(
		bus, PATH, info->interfaces[0], &mock, GINT_TO_POINTER(test),
		NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(object, >, 0);
	if (test != NO_SERVICE) {
		g_autoptr(GVariant) name = g_dbus_connection_call_sync(
			bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "RequestName",
			g_variant_new("(su)", PORTAL, 4u),
			G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000,
			NULL, &error);
		guint32 acquired;

		g_assert_no_error(error);
		g_variant_get(name, "(u)", &acquired);
		g_assert_cmpuint(acquired, ==, 1);
	}
	struct line_screencast *cast =
		line_screencast_start(64, 64, 5, 0, NULL, &error);
	g_autofree char *failure = NULL;
	enum line_screencast_state state = LINE_SCREENCAST_STARTING;

	g_assert_no_error(error);
	g_assert_nonnull(cast);
	while (state == LINE_SCREENCAST_STARTING) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
		state = line_screencast_poll(cast, &failure);
	}
	line_screencast_stop(cast);
	g_assert_cmpint(state, ==, LINE_SCREENCAST_FAILED);
	g_assert_nonnull(failure);
	if (test == LATER_METHOD || test == DENIED)
		g_assert_nonnull(strstr(failure, expected));
	else
		g_assert_cmpstr(failure, ==, expected);
	g_assert_cmpuint(native_calls, ==, native_cinnamon ? 1 : 0);
	g_dbus_connection_unregister_object(bus, object);
	g_clear_object(&bus);
	g_test_dbus_down(dbus);
}

static void planar_capture(void)
{
	guint8 planes[3][8 * 4] = { { 0 } };
	struct spa_chunk chunk[3] = { 0 };
	struct spa_data data[3] = { 0 };
	struct spa_buffer buf = { .n_datas = 3, .datas = data };
	guint8 expected[24];

	for (guint i = 0, k = 0; i < 3; i++) {
		guint cols = 4 >> !!i;

		chunk[i] = (struct spa_chunk){ .offset = 2,
					       .size = 30,
					       .stride = 8 };
		data[i] = (struct spa_data){ .data = planes[i],
					     .chunk = &chunk[i],
					     .maxsize = 32 };
		for (guint row = 0; row < cols; row++)
			for (guint x = 0; x < cols; x++, k++)
				planes[i][2 + row * 8 + x] = expected[k] = k;
	}
	g_autoptr(GBytes) packed = line_screencast_planar(&buf, 4, 4);
	g_assert_nonnull(packed);
	g_assert_cmpmem(g_bytes_get_data(packed, NULL),
			g_bytes_get_size(packed), expected, sizeof(expected));
	chunk[1].size = 8;
	g_assert_null(line_screencast_planar(&buf, 4, 4));
	chunk[1].size = 30;
	chunk[1].offset = G_MAXUINT32;
	g_assert_null(line_screencast_planar(&buf, 4, 4));
}

static guint returns, renegotiations;
static struct pw_buffer *borrowed;

__attribute__((visibility("default"))) int
__wrap_pw_stream_queue_buffer(struct pw_stream *stream,
			      struct pw_buffer *buffer)
{
	g_assert_nonnull(stream);
	g_assert_true(buffer == borrowed);
	returns++;
	return 0;
}

static void want_shm(void *data, uint64_t count)
{
	(void)data;
	renegotiations += count;
}

static void capture_leases(void)
{
	struct spa_chunk chunks[2] = {
		{ .stride = 32, .size = 512 },
		{ .stride = 32, .offset = 512, .size = 256 }
	};
	struct spa_data planes[2] = {
		{ .type = SPA_DATA_DmaBuf, .maxsize = 768, .chunk = &chunks[0] },
		{ .type = SPA_DATA_DmaBuf, .maxsize = 768, .chunk = &chunks[1] }
	};
	struct spa_buffer bytes = { .n_datas = 2, .datas = planes };
	struct pw_buffer buffer = { .buffer = &bytes };
	struct spa_video_info_raw fmt = {
		.format = SPA_VIDEO_FORMAT_NV12,
		.size = { 32, 16 },
		.flags = SPA_VIDEO_FLAG_MODIFIER,
		.color_matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.color_range = SPA_VIDEO_COLOR_RANGE_16_235
	};
	int fd = memfd_create("fixture-yuv-layout", MFD_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(ftruncate(fd, 768), ==, 0);
	planes[0].fd = planes[1].fd = fd;
	borrowed = &buffer;
	returns = renegotiations = 0;
	pw_init(NULL, NULL);
	struct pw_loop *loop = pw_loop_new(NULL);

	g_assert_nonnull(loop);
	struct cast_dma_pool *pool = cast_dma_pool_new(
		(struct pw_stream *)&buffer, loop, want_shm, NULL);
	g_assert_nonnull(pool);
	g_assert_null(cast_dma_take(pool, &buffer, &fmt, 64, 16, 0));
	g_assert_null(cast_dma_take(pool, &buffer, &fmt, 32, 16, 1));
	fmt.color_range = SPA_VIDEO_COLOR_RANGE_0_255;
	g_assert_null(cast_dma_take(pool, &buffer, &fmt, 32, 16, 0));
	fmt.color_range = SPA_VIDEO_COLOR_RANGE_16_235;
	chunks[1].offset++;
	g_assert_null(cast_dma_take(pool, &buffer, &fmt, 32, 16, 0));
	chunks[1].offset--;

	GBytes *held = cast_dma_take(pool, &buffer, &fmt, 32, 16, 0);

	g_assert_nonnull(held);
	const struct line_capture_dma *d = g_bytes_get_data(held, NULL);
	int lease_fd = d->fd;

	g_assert_cmpint(fcntl(lease_fd, F_GETFD), ==, FD_CLOEXEC);
	GBytes *job = g_bytes_ref(held);

	g_bytes_unref(held);
	g_assert_cmpuint(returns, ==, 0);
	g_bytes_unref(job);
	g_assert_cmpuint(returns, ==, 1);
	g_assert_cmpint(fcntl(lease_fd, F_GETFD), ==, -1);

	held = cast_dma_take(pool, &buffer, &fmt, 32, 16, 0);
	cast_dma_remove(pool, &buffer);
	g_bytes_unref(held);
	g_assert_cmpuint(returns, ==, 1);
	held = cast_dma_take(pool, &buffer, &fmt, 32, 16, 0);
	line_capture_reject(held);
	line_capture_reject(held);
	pw_loop_enter(loop);
	pw_loop_iterate(loop, 0);
	pw_loop_leave(loop);
	g_assert_cmpuint(renegotiations, ==, 1);
	g_assert_null(cast_dma_take(pool, &buffer, &fmt, 32, 16, 0));
	cast_dma_pool_stop(pool);
	line_capture_reject(held);
	g_bytes_unref(held);
	g_assert_cmpuint(returns, ==, 1);
	pw_loop_destroy(loop);
	close(fd);
}

int main(int argc, char **argv)
{
	static const char *const tests[] = {
		"cinnamon",   "other-desktop", "x11",	       "sandbox",
		"no-service", "disabled",      "default-none", "custom-backend",
		"denied",     "cancelled",     "later-method"
	};

	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/screencast/dma-lease-and-fallback", capture_leases);
	g_test_add_func("/screencast/planar-capture", planar_capture);
	for (size_t i = 0; i < G_N_ELEMENTS(tests); i++) {
		g_autofree char *path =
			g_strconcat("/screencast/portal/", tests[i], NULL);
		g_autofree char *child = g_strconcat(path, "/subprocess", NULL);

		g_test_add_data_func(path, NULL, isolated);
		g_test_add_data_func(child, GSIZE_TO_POINTER(i), run);
	}
	return g_test_run();
}
