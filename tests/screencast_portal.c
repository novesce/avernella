// SPDX-License-Identifier: GPL-3.0-or-later
#include "screencast.h"
#include "screencast_cinnamon.h"
#include <gio/gio.h>

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
	const char *expected = test > CINNAMON && test < DENIED ?
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
	struct line_screencast *cast = line_screencast_start(64, 64, 5, &error);
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
	g_assert_cmpuint(native_calls, ==, test == CINNAMON ? 1 : 0);
	g_dbus_connection_unregister_object(bus, object);
	g_clear_object(&bus);
	g_test_dbus_down(dbus);
}

int main(int argc, char **argv)
{
	static const char *const tests[] = {
		"cinnamon",   "other-desktop", "x11",	       "sandbox",
		"no-service", "disabled",      "default-none", "custom-backend",
		"denied",     "cancelled",     "later-method"
	};

	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	for (size_t i = 0; i < G_N_ELEMENTS(tests); i++) {
		g_autofree char *path =
			g_strconcat("/screencast/portal/", tests[i], NULL);
		g_autofree char *child = g_strconcat(path, "/subprocess", NULL);

		g_test_add_data_func(path, NULL, isolated);
		g_test_add_data_func(child, GSIZE_TO_POINTER(i), run);
	}
	return g_test_run();
}
