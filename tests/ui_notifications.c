// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_notifications.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#define NOTIFICATIONS "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"

static const char fixture_body[] = "Fixture <name> & friends";

/* A stranger's name reaches the body, so markup servers get it escaped. */
static void body_escaped(void)
{
	static const char name[] =
		"<a href=\"https://example.com/\">Verify</a> & <img src=x>";
	g_autofree char *escaped = ui_notification_text(name, TRUE);
	g_autofree char *plain = ui_notification_text(name, FALSE);

	g_assert_cmpstr(escaped, ==,
			"&lt;a href=&quot;https://example.com/&quot;&gt;"
			"Verify&lt;/a&gt; &amp; &lt;img src=x&gt;");
	g_assert_null(strchr(escaped, '<'));
	g_assert_cmpstr(plain, ==, name);
}

struct receiver {
	gboolean markup;
	char *body;
};

static void receive(GDBusConnection *bus, const char *sender, const char *path,
		    const char *interface, const char *method, GVariant *args,
		    GDBusMethodInvocation *call, gpointer data)
{
	struct receiver *receiver = data;

	(void)bus;
	(void)sender;
	(void)path;
	(void)interface;
	if (!strcmp(method, "GetCapabilities")) {
		static const char *const caps[] = { "body-markup", NULL };

		g_dbus_method_invocation_return_value(
			call, g_variant_new("(@as)",
					    g_variant_new_strv(
						    caps, receiver->markup)));
		return;
	}
	g_assert_cmpstr(method, ==, "Notify");
	const char *body;

	g_variant_get_child(args, 4, "&s", &body);
	g_clear_pointer(&receiver->body, g_free);
	receiver->body = g_strdup(body);
	g_dbus_method_invocation_return_value(call, g_variant_new("(u)", 1u));
}

static void own_notification_name(GDBusConnection *bus, gboolean own)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", own ? "RequestName" : "ReleaseName",
		own ? g_variant_new("(su)", NOTIFICATIONS, 4u) :
		      g_variant_new("(s)", NOTIFICATIONS),
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL,
		&error);
	guint result = 0;

	g_assert_no_error(error);
	g_variant_get(reply, "(u)", &result);
	g_assert_cmpuint(result, ==, 1);
}

static void check_receiver(GTestDBus *dbus, GDBusInterfaceInfo *iface,
			   gboolean markup, const char *expected)
{
	static const GDBusInterfaceVTable methods = { .method_call = receive };
	struct receiver receiver = { .markup = markup };

	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
		g_test_dbus_get_bus_address(dbus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);

	g_assert_no_error(error);
	guint object = g_dbus_connection_register_object(
		bus, NOTIFICATIONS_PATH, iface, &methods, &receiver, NULL,
		&error);

	g_assert_no_error(error);
	g_assert_cmpuint(object, >, 0);
	own_notification_name(bus, TRUE);
	gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

	do {
		ui_desktop_notify_text("LINE", fixture_body);
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(10000);
	} while (!receiver.body && g_get_monotonic_time() < deadline);
	g_assert_cmpstr(receiver.body, ==, expected);
	g_free(receiver.body);
	own_notification_name(bus, FALSE);
	g_dbus_connection_unregister_object(bus, object);
	g_assert_true(g_dbus_connection_close_sync(bus, NULL, &error));
	g_assert_no_error(error);
}

static void daemon_replacement(gconstpointer data)
{
	gboolean filtered = GPOINTER_TO_INT(data);

	if (!g_test_subprocess()) {
		g_autofree char *proxy =
			g_find_program_in_path("xdg-dbus-proxy");

		if (filtered && !proxy) {
			g_test_skip("xdg-dbus-proxy is unavailable");
			return;
		}
		g_test_trap_subprocess(NULL, 12000000, 0);
		g_test_trap_assert_passed();
		return;
	}
	static const char xml[] =
		"<node><interface name='" NOTIFICATIONS "'>"
		"<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
		"<method name='Notify'><arg type='s' direction='in'/>"
		"<arg type='u' direction='in'/><arg type='s' direction='in'/>"
		"<arg type='s' direction='in'/><arg type='s' direction='in'/>"
		"<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/>"
		"<arg type='i' direction='in'/><arg type='u' direction='out'/>"
		"</method></interface></node>";
	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusNodeInfo) info =
		g_dbus_node_info_new_for_xml(xml, &error);
	g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_autoptr(GSubprocess) proxy = NULL;
	g_autofree char *dir = NULL, *socket_path = NULL;

	g_assert_no_error(error);
	g_test_dbus_up(bus);
	if (filtered) {
		dir = g_dir_make_tmp("avernella-notifications-XXXXXX", &error);
		g_assert_no_error(error);
		socket_path = g_build_filename(dir, "bus", NULL);
		proxy = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
					 "xdg-dbus-proxy",
					 g_test_dbus_get_bus_address(bus),
					 socket_path, "--filter",
					 "--talk=" NOTIFICATIONS, NULL);
		g_assert_no_error(error);
		gint64 deadline =
			g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;

		while (!g_file_test(socket_path, G_FILE_TEST_EXISTS) &&
		       g_get_monotonic_time() < deadline)
			g_usleep(1000);
		g_assert_true(g_file_test(socket_path, G_FILE_TEST_EXISTS));
		g_autofree char *address =
			g_strdup_printf("unix:path=%s", socket_path);

		g_setenv("DBUS_SESSION_BUS_ADDRESS", address, TRUE);
	}
	check_receiver(bus, info->interfaces[0], FALSE, fixture_body);
	check_receiver(bus, info->interfaces[0], TRUE,
		       "Fixture &lt;name&gt; &amp; friends");
	g_test_dbus_down(bus);
	if (proxy) {
		g_subprocess_force_exit(proxy);
		g_assert_true(g_subprocess_wait(proxy, NULL, &error));
		g_assert_no_error(error);
		(void)g_unlink(socket_path);
		g_assert_cmpint(g_rmdir(dir), ==, 0);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/notifications/body-escaped", body_escaped);
	g_test_add_data_func("/notifications/daemon-replacement", NULL,
			     daemon_replacement);
	g_test_add_data_func("/notifications/filtered-daemon-replacement",
			     GINT_TO_POINTER(1), daemon_replacement);
	return g_test_run();
}
