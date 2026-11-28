// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_tray.h"
#include "ui_logo.h"
#include "ui_window.h"
#include <gio/gio.h>
#include <string.h>

#define WATCH "org.kde.StatusNotifierWatcher"
#define WPATH "/StatusNotifierWatcher"
#define ITEM "org.kde.StatusNotifierItem"
#define IPATH "/StatusNotifierItem"
#define MENU "com.canonical.dbusmenu"
#define APP "io.github.novesce.Avernella"
#define PORTAL "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
static GDBusConnection *bus;
static char *item;
static gboolean panel_host;
static SDL_Window *window;
static gboolean quit;
static gboolean background_requested;
static const char xml[] =
	"<node><interface name='" WATCH "'>"
	"<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
	"<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
	"<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
	"<signal name='StatusNotifierHostRegistered'/></interface></node>";

static void registered(GDBusConnection *c, const char *sender, const char *path,
		       const char *iface, const char *method, GVariant *params,
		       GDBusMethodInvocation *call, gpointer data)
{
	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)data;
	g_assert_cmpstr(method, ==, "RegisterStatusNotifierItem");
	g_free(item);
	g_variant_get(params, "(s)", &item);
	g_dbus_method_invocation_return_value(call, NULL);
}

static GVariant *prop(GDBusConnection *c, const char *sender, const char *path,
		      const char *iface, const char *name, GError **error,
		      gpointer data)
{
	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)error;
	(void)data;
	if (!strcmp(name, "IsStatusNotifierHostRegistered"))
		return g_variant_new_boolean(panel_host);
	g_autofree char *full = item ? g_strconcat(item, IPATH, NULL) : NULL;
	const char *const ids[] = { full, NULL };

	return g_variant_new_strv(ids, full ? 1 : 0);
}

static const GDBusInterfaceVTable mock = { .method_call = registered,
					   .get_property = prop };

static GVariant *rpc(const char *dest, const char *path, const char *iface,
		     const char *method, GVariant *args)
{
	g_autoptr(GError) e = NULL;
	GVariant *reply = g_dbus_connection_call_sync(bus, dest, path, iface,
						      method, args, NULL,
						      G_DBUS_CALL_FLAGS_NONE,
						      1500, NULL, &e);
	g_assert_no_error(e);
	g_assert_nonnull(reply);
	return reply;
}

static void own_name(const char *name, gboolean acquire)
{
	g_autoptr(GVariant) reply = rpc(
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", acquire ? "RequestName" : "ReleaseName",
		acquire ? g_variant_new("(su)", name, 4u) :
			  g_variant_new("(s)", name));
	guint status = 0;

	g_variant_get(reply, "(u)", &status);
	g_assert_cmpint(status, ==, 1);
}

static void own(gboolean acquire)
{
	own_name(WATCH, acquire);
}

static void request_background(GDBusConnection *c, const char *sender,
			       const char *path, const char *iface,
			       const char *method, GVariant *params,
			       GDBusMethodInvocation *call, gpointer data)
{
	g_autoptr(GVariant) opts = NULL;
	const char *parent, *token;
	gboolean autostart;

	(void)path;
	(void)iface;
	g_assert_cmpstr(method, ==, "RequestBackground");
	g_assert_false(background_requested);
	g_variant_get(params, "(&s@a{sv})", &parent, &opts);
	g_assert_true(g_variant_lookup(opts, "handle_token", "&s", &token));
	g_assert_true(g_variant_lookup(opts, "autostart", "b", &autostart));
	g_assert_false(autostart);
	g_autofree char *caller = g_strdup(sender + 1);

	g_strdelimit(caller, ".", '_');
	g_autofree char *answer =
		g_strdup_printf(PORTAL_PATH "/request/%s/%s", caller, token);
	GVariantBuilder results;

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "background",
			      g_variant_new_boolean(GPOINTER_TO_INT(data)));
	g_dbus_method_invocation_return_value(call,
					      g_variant_new("(o)", answer));
	g_dbus_connection_emit_signal(
		c, sender, answer, "org.freedesktop.portal.Request", "Response",
		g_variant_new("(ua{sv})", 0u, &results), NULL);
	background_requested = TRUE;
}

static void pump(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
	SDL_Event e;

	while (SDL_PollEvent(&e))
		ui_tray_event(&e, window, &quit);
	g_usleep(1000);
}

static gboolean hidden(void)
{
	return (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0;
}

enum wait_for {
	REGISTERED,
	HIDDEN_BY_CLOSE,
	VISIBLE,
	HIDDEN,
	QUIT_APP
};

static void wait_for(enum wait_for condition)
{
	gint64 until = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	do {
		pump();
		switch (condition) {
		case REGISTERED:
			if (item)
				return;
			break;
		case HIDDEN_BY_CLOSE:
			if (ui_tray_hide(window))
				return;
			break;
		case VISIBLE:
			if (!hidden())
				return;
			break;
		case HIDDEN:
			if (hidden())
				return;
			break;
		case QUIT_APP:
			if (quit)
				return;
			break;
		}
	} while (g_get_monotonic_time() < until);
	g_error("Timed out waiting for tray condition %d", condition);
}

static void action(int id)
{
	g_autoptr(GVariant) r = rpc(item, "/Menu", MENU, "Event",
				    g_variant_new("(isvu)", id, "clicked",
						  g_variant_new_int32(0), 0u));
}

static void check_icon(GVariant *properties, const char *expected_name)
{
	const char *name;
	int width, height;
	gsize size;
	guint32 expected[UI_LOGO_EDGE * UI_LOGO_EDGE];
	g_autoptr(GVariant) pixmaps = g_variant_lookup_value(
		properties, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
	g_autoptr(GVariant) bytes = NULL;

	g_assert_true(g_variant_lookup(properties, "IconName", "&s", &name));
	g_assert_cmpstr(name, ==, expected_name);
	g_assert_nonnull(pixmaps);
	g_assert_cmpuint(g_variant_n_children(pixmaps), ==, 1);
	g_variant_get_child(pixmaps, 0, "(ii@ay)", &width, &height, &bytes);
	g_assert_cmpint(width, ==, UI_LOGO_EDGE);
	g_assert_cmpint(height, ==, UI_LOGO_EDGE);
	const void *data = g_variant_get_fixed_array(bytes, &size, 1);

	g_assert_true(ui_logo_read((guint8 *)expected, FALSE));
	for (guint i = 0; i < G_N_ELEMENTS(expected); i++)
		expected[i] = GUINT32_TO_BE(expected[i]);
	g_assert_cmpmem(data, size, expected, sizeof(expected));
}

static void tray(gconstpointer fixture)
{
	gboolean sandboxed = fixture != NULL;
	gboolean permitted = GPOINTER_TO_INT(fixture) == 1;
	guint portal_object = 0;

	g_autoptr(GVariant) properties = NULL, dict = NULL, show = NULL,
			    menu = NULL, root = NULL, children = NULL,
			    provided = NULL;

	quit = panel_host = background_requested = FALSE;
	const char *driver = g_getenv("AVERNELLA_TEST_TRAY_DRIVER");

	g_setenv("SDL_VIDEODRIVER", driver ? driver : "dummy", TRUE);
	g_autofree char *xdisplay = g_strdup(g_getenv("DISPLAY"));
	g_autofree char *wdisplay = g_strdup(g_getenv("WAYLAND_DISPLAY"));
	g_autofree char *runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));

	g_unsetenv("LINE_DISABLE_TRAY");
	if (sandboxed)
		g_setenv("FLATPAK_ID", APP, TRUE);
	g_autoptr(GTestDBus) dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(dbus);
	if (driver && xdisplay)
		g_setenv("DISPLAY", xdisplay, TRUE);
	if (driver && wdisplay)
		g_setenv("WAYLAND_DISPLAY", wdisplay, TRUE);
	if (driver && runtime)
		g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_autoptr(GError) error = NULL;
	bus = g_dbus_connection_new_for_address_sync(
		g_test_dbus_get_bus_address(dbus),
		G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
		NULL, NULL, &error);
	g_assert_no_error(error);
	g_autoptr(GDBusNodeInfo) info =
		g_dbus_node_info_new_for_xml(xml, &error);
	g_assert_no_error(error);
	guint object = g_dbus_connection_register_object(
		bus, WPATH, info->interfaces[0], &mock, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(object, >, 0);
	own(TRUE);
	g_autoptr(GDBusNodeInfo) background = NULL;

	if (sandboxed) {
		static const GDBusInterfaceVTable portal_mock = {
			.method_call = request_background,
		};

		background = g_dbus_node_info_new_for_xml(
			"<node><interface name='org.freedesktop.portal.Background'>"
			"<method name='RequestBackground'>"
			"<arg type='s' direction='in'/>"
			"<arg type='a{sv}' direction='in'/>"
			"<arg type='o' direction='out'/>"
			"</method></interface></node>",
			&error);
		g_assert_no_error(error);
		portal_object = g_dbus_connection_register_object(
			bus, PORTAL_PATH, background->interfaces[0],
			&portal_mock, GINT_TO_POINTER(permitted), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(portal_object, >, 0);
		own_name(PORTAL, TRUE);
	}
	g_assert_cmpint(SDL_Init(SDL_INIT_VIDEO), ==, 0);
	window = SDL_CreateWindow("Tray fixture", 0, 0, 160, 100, 0);
	g_assert_nonnull(window);
	if (driver) {
		g_autofree char *parent = NULL;
		struct ui_window_export *exported =
			ui_window_export(window, &parent);

		if (!strcmp(driver, "x11")) {
			g_assert_nonnull(parent);
			g_assert_true(g_str_has_prefix(parent, "x11:"));
			g_assert_null(exported);
		} else if (g_getenv("AVERNELLA_TEST_REQUIRE_WAYLAND_EXPORT")) {
			g_assert_nonnull(parent);
			g_assert_true(g_str_has_prefix(parent, "wayland:"));
			g_assert_nonnull(exported);
		}
		ui_window_unexport(exported);
	}
	g_assert_false(ui_tray_hide(window));
	ui_tray_start();
	wait_for(REGISTERED);
	for (int i = 0; i < 30; i++)
		pump();
	g_assert_false(ui_tray_hide(
		window)); /* Watcher exists but no rendering host. */
	panel_host = TRUE;
	g_dbus_connection_emit_signal(bus, NULL, WPATH, WATCH,
				      "StatusNotifierHostRegistered", NULL,
				      &error);
	g_assert_no_error(error);
	if (sandboxed) {
		g_assert_cmpstr(item, ==, APP ".StatusNotifierItem");
		g_assert_false(ui_tray_hide(window));
		ui_tray_close(window);
		for (int i = 0; i < 200 && !background_requested; i++)
			pump();
		g_assert_true(background_requested);
		if (!permitted) {
			for (int i = 0; i < 30; i++)
				pump();
			g_assert_false(hidden());
			g_assert_false(ui_tray_hide(window));
			goto stop;
		}
	}
	wait_for(HIDDEN_BY_CLOSE);
	g_assert_true(hidden());
	properties = rpc(item, IPATH, "org.freedesktop.DBus.Properties",
			 "GetAll", g_variant_new("(s)", ITEM));
	dict = g_variant_get_child_value(properties, 0);
	const char *title = NULL;

	g_assert_true(g_variant_lookup(dict, "Title", "&s", &title));
	g_assert_cmpstr(title, ==, "Avernella");
	check_icon(dict, sandboxed ? APP : "avernella");
	provided = rpc(item, IPATH, ITEM, "ProvideXdgActivationToken",
		       g_variant_new("(s)", "fixture-token"));
	g_assert_nonnull(provided);
	show = rpc(item, IPATH, ITEM, "Activate", g_variant_new("(ii)", 0, 0));
	wait_for(VISIBLE);
	ui_tray_presented(window);
	g_clear_pointer(&show, g_variant_unref);
	show = rpc(item, IPATH, ITEM, "Activate", g_variant_new("(ii)", 0, 0));
	for (int i = 0; i < 20; i++)
		pump();
	g_assert_false(hidden());
	menu = rpc(item, "/Menu", MENU, "GetLayout",
		   g_variant_new("(ii@as)", 0, -1,
				 g_variant_new_strv(NULL, 0)));
	root = g_variant_get_child_value(menu, 1);
	children = g_variant_get_child_value(root, 2);
	g_assert_cmpuint(g_variant_n_children(children), ==, 3);
	action(2);
	wait_for(HIDDEN);
	action(1);
	wait_for(VISIBLE);
	action(2);
	wait_for(HIDDEN);
	own(FALSE);
	wait_for(VISIBLE);
	g_assert_false(ui_tray_hide(window));
	own(TRUE);
	wait_for(HIDDEN_BY_CLOSE);
	action(3);
	wait_for(QUIT_APP);
stop:
	ui_tray_stop();
	ui_window_shutdown();
	SDL_DestroyWindow(window);
	SDL_Quit();
	own(FALSE);
	if (portal_object)
		g_dbus_connection_unregister_object(bus, portal_object);
	g_dbus_connection_unregister_object(bus, object);
	g_dbus_connection_close_sync(bus, NULL, NULL);
	g_clear_object(&bus);
	g_clear_pointer(&item, g_free);
	g_test_dbus_down(dbus);
	if (sandboxed)
		g_unsetenv("FLATPAK_ID");
}

static void fixture(gconstpointer data)
{
	if (g_test_subprocess())
		tray(data);
	else {
		g_test_trap_subprocess(NULL, 20000000, 0);
		g_test_trap_assert_passed();
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	if (!g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS))
		g_test_add_data_func(
			"/tray/private-bus-menu-host-loss-and-fallback", NULL,
			fixture);
	g_test_add_data_func("/tray/flatpak-background-allowed",
			     GINT_TO_POINTER(1), fixture);
	g_test_add_data_func("/tray/flatpak-background-denied",
			     GINT_TO_POINTER(2), fixture);
	return g_test_run();
}
