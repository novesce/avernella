// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_tray.h"
#include <gio/gio.h>
#include <string.h>

#define WATCH "org.kde.StatusNotifierWatcher"
#define WPATH "/StatusNotifierWatcher"
#define ITEM "org.kde.StatusNotifierItem"
#define IPATH "/StatusNotifierItem"
#define MENU "com.canonical.dbusmenu"
static GDBusConnection *bus;
static char *item;
static gboolean panel_host;
static SDL_Window *window;
static gboolean quit;
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

static void own(gboolean acquire)
{
	g_autoptr(GVariant) reply = rpc(
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", acquire ? "RequestName" : "ReleaseName",
		acquire ? g_variant_new("(su)", WATCH, 4u) :
			  g_variant_new("(s)", WATCH));
	guint status = 0;

	g_variant_get(reply, "(u)", &status);
	g_assert_cmpint(status, ==, 1);
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

static void tray(void)
{
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_unsetenv("LINE_DISABLE_TRAY");
	g_autoptr(GTestDBus) dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(dbus);
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
	g_assert_cmpint(SDL_Init(SDL_INIT_VIDEO), ==, 0);
	window = SDL_CreateWindow("Tray fixture", 0, 0, 160, 100, 0);
	g_assert_nonnull(window);
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
	wait_for(HIDDEN_BY_CLOSE);
	g_assert_true(hidden());
	g_autoptr(GVariant) properties =
		rpc(item, IPATH, "org.freedesktop.DBus.Properties", "GetAll",
		    g_variant_new("(s)", ITEM));
	g_autoptr(GVariant) dict = g_variant_get_child_value(properties, 0);
	const char *title = NULL;

	g_assert_true(g_variant_lookup(dict, "Title", "&s", &title));
	g_assert_cmpstr(title, ==, "Avernella");
	g_autoptr(GVariant) pixels = g_variant_lookup_value(
		dict, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
	g_assert_cmpuint(g_variant_n_children(pixels), ==, 1);
	g_autoptr(GVariant) show =
		rpc(item, IPATH, ITEM, "Activate", g_variant_new("(ii)", 0, 0));
	wait_for(VISIBLE);
	g_autoptr(GVariant) menu = rpc(
		item, "/Menu", MENU, "GetLayout",
		g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(NULL, 0)));
	g_autoptr(GVariant) root = g_variant_get_child_value(menu, 1),
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
	ui_tray_stop();
	SDL_DestroyWindow(window);
	SDL_Quit();
	own(FALSE);
	g_dbus_connection_unregister_object(bus, object);
	g_dbus_connection_close_sync(bus, NULL, NULL);
	g_clear_object(&bus);
	g_clear_pointer(&item, g_free);
	g_test_dbus_down(dbus);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tray/private-bus-menu-host-loss-and-fallback", tray);
	return g_test_run();
}
