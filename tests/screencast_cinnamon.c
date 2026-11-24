// SPDX-License-Identifier: GPL-3.0-or-later
#include "screencast_cinnamon.h"
#include "monitor_picker.h"
#include <unistd.h>

#define CAST "org.cinnamon.Muffin.ScreenCast"
#define SESSION "/org/cinnamon/Muffin/ScreenCast/Session/1"
#define STREAM "/org/cinnamon/Muffin/ScreenCast/Stream/1"
#define SAVER "org.cinnamon.ScreenSaver"
#define SAVER_PATH "/org/cinnamon/ScreenSaver"

enum scenario {
	CONFIRM,
	CANCEL,
	INVALID,
	HOTPLUG,
	LOCKED,
	LOCK_DURING_PICK,
	CLOSED,
	BAD_NODE,
	LOCK_DURING_SHARE,
	DENIED,
};
static GDBusConnection *service;
static enum scenario scenario;
static GMainLoop *main_loop;
static guint picked, states, unlocked, created, recorded, started, stopped;

char *line_pick_share_monitor(const char *const *connectors,
			      const char *const *labels, guint count,
			      GCancellable *cancel, GError **error)
{
	picked++;
	g_assert_cmpuint(count, ==, 2);
	g_assert_cmpstr(connectors[0], ==, "eDP-1");
	g_assert_cmpstr(connectors[1], ==, "DP-2");
	g_assert_cmpstr(labels[1], ==, "External display");
	if (scenario == CANCEL) {
		g_cancellable_cancel(cancel);
		g_cancellable_set_error_if_cancelled(cancel, error);
		return NULL;
	}
	return g_strdup(scenario == INVALID ? "" : connectors[1]);
}

static void add_monitor(GVariantBuilder *monitors, const char *connector,
			const char *name, gboolean current)
{
	GVariantBuilder modes, props, flags, scales;

	g_variant_builder_init(&modes, G_VARIANT_TYPE("a(siiddada{sv})"));
	g_variant_builder_init(&scales, G_VARIANT_TYPE("ad"));
	g_variant_builder_add(&scales, "d", 1.0);
	g_variant_builder_init(&flags, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&flags, "{sv}", "is-current",
			      g_variant_new_boolean(current));
	g_variant_builder_add(&modes, "(siiddada{sv})", "1920x1080", 1920, 1080,
			      60.0, 1.0, &scales, &flags);
	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&props, "{sv}", "display-name",
			      g_variant_new_string(name));
	g_variant_builder_add(monitors, "((ssss)a(siiddada{sv})a{sv})",
			      connector, "Vendor", "Model", "", &modes, &props);
}

static GVariant *current_state(void)
{
	GVariantBuilder monitors;

	g_variant_builder_init(&monitors,
			       G_VARIANT_TYPE("a((ssss)a(siiddada{sv})a{sv})"));
	add_monitor(&monitors, "eDP-1", "Built-in display", TRUE);
	add_monitor(&monitors, "HDMI-A-1", "Inactive", FALSE);
	add_monitor(&monitors, "DP-2", "External display", TRUE);
	states++;
	return g_variant_new(
		"(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})",
		scenario == HOTPLUG ? states : 1u, &monitors, NULL, NULL);
}

static void method(GDBusConnection *bus, const char *sender, const char *path,
		   const char *iface, const char *name, GVariant *params,
		   GDBusMethodInvocation *request, gpointer data)
{
	GVariant *reply = NULL;

	(void)path;
	(void)iface;
	(void)data;
	if (!strcmp(name, "GetCurrentState")) {
		reply = current_state();
	} else if (!strcmp(name, "GetActive")) {
		unlocked++;
		reply = g_variant_new("(b)",
				      scenario == LOCKED ||
					      (scenario == LOCK_DURING_PICK &&
					       unlocked > 1));
	} else if (!strcmp(name, "CreateSession")) {
		created++;
		reply = g_variant_new("(o)", SESSION);
	} else if (!strcmp(name, "RecordMonitor")) {
		g_autoptr(GVariant) props =
			g_variant_get_child_value(params, 1);
		const char *connector = NULL;
		guint32 cursor;

		recorded++;
		if (scenario == DENIED) {
			g_dbus_method_invocation_return_error_literal(
				request, G_DBUS_ERROR,
				G_DBUS_ERROR_ACCESS_DENIED, "Denied");
			return;
		}
		g_variant_get_child(params, 0, "&s", &connector);
		g_assert_cmpstr(connector, ==, "DP-2");
		g_assert_true(
			g_variant_lookup(props, "cursor-mode", "u", &cursor));
		g_assert_cmpuint(cursor, ==, 1);
		reply = g_variant_new("(o)", STREAM);
	} else if (!strcmp(name, "Start")) {
		started++;
		const gboolean closed = scenario == CLOSED;
		const int node = 73;

		g_assert_true(g_dbus_connection_emit_signal(
			bus, sender, closed ? SESSION : STREAM,
			closed ? CAST ".Session" : CAST ".Stream",
			closed ? "Closed" : "PipeWireStreamAdded",
			closed ? NULL :
				 g_variant_new("(u)", scenario == BAD_NODE ?
							      G_MAXUINT32 :
							      (guint32)node),
			NULL));
	} else if (!strcmp(name, "Stop")) {
		stopped++;
	} else {
		g_assert_not_reached();
	}
	g_dbus_method_invocation_return_value(request, reply);
}

static gboolean finished(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static gpointer client(gpointer unused)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	guint32 node = 0;

	(void)unused;
	g_main_context_push_thread_default(context);
	struct line_cinnamon_cast *cast =
		line_cinnamon_start(context, cancel, &node, &error);

	if (scenario == CONFIRM || scenario == LOCK_DURING_SHARE) {
		g_assert_no_error(error);
		g_assert_nonnull(cast);
		g_assert_cmpuint(node, ==, 73);
		if (scenario == LOCK_DURING_SHARE) {
			g_assert_true(g_dbus_connection_emit_signal(
				service, NULL, SAVER_PATH, SAVER,
				"ActiveChanged", g_variant_new("(b)", TRUE),
				NULL));
			while (!g_cancellable_is_cancelled(cancel))
				g_main_context_iteration(context, TRUE);
		}
		line_cinnamon_stop(cast);
	} else {
		g_assert_null(cast);
		g_assert_cmpuint(node, ==, 0);
		g_assert_nonnull(error);
		if (scenario == LOCKED || scenario == LOCK_DURING_PICK)
			g_assert_error(error, G_IO_ERROR,
				       G_IO_ERROR_PERMISSION_DENIED);
		if (scenario == DENIED)
			g_assert_error(error, G_DBUS_ERROR,
				       G_DBUS_ERROR_ACCESS_DENIED);
		if (scenario == CLOSED || scenario == CANCEL)
			g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	}
	g_main_context_pop_thread_default(context);
	g_main_context_invoke(NULL, finished, main_loop);
	return NULL;
}

static void test(gconstpointer data)
{
	scenario = GPOINTER_TO_INT(data);
	picked = states = unlocked = created = recorded = started = stopped = 0;
	alarm(20);
	GThread *worker = g_thread_new("cinnamon-fixture", client, NULL);

	g_main_loop_run(main_loop);
	g_thread_join(worker);
	alarm(0);
	guint active = scenario == CONFIRM || scenario == LOCK_DURING_SHARE ||
		       scenario == BAD_NODE || scenario == CLOSED;
	g_assert_cmpuint(picked, ==, scenario != LOCKED);
	g_assert_cmpuint(created, ==, active || scenario == DENIED);
	g_assert_cmpuint(recorded, ==, active || scenario == DENIED);
	g_assert_cmpuint(started, ==, active);
	g_assert_cmpuint(stopped, ==, active || scenario == DENIED);
}

static void own(const char *name)
{
	g_autoptr(GError) err = NULL;
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		service, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "RequestName",
		g_variant_new("(su)", name, 4u), G_VARIANT_TYPE("(u)"),
		G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
	guint32 result;

	g_assert_no_error(err);
	g_variant_get(reply, "(u)", &result);
	g_assert_cmpuint(result, ==, 1u);
}

#define IN(t) "<arg type='" t "' direction='in'/>"
#define OUT(t) "<arg type='" t "' direction='out'/>"
#define METHOD(m, args) "<method name='" m "'>" args "</method>"
#define IFACE(i, methods) "<interface name='" i "'>" methods "</interface>"
static const char xml[] = "<node>" IFACE(CAST, METHOD("CreateSession",
						      IN("a{sv}") OUT("o")))
	IFACE(CAST ".Session",
	      METHOD("RecordMonitor", IN("s") IN("a{sv}") OUT("o"))
		      METHOD("Start", "") METHOD("Stop", ""))
		IFACE("org.cinnamon.Muffin.DisplayConfig",
		      METHOD("GetCurrentState",
			     OUT("u") OUT("a((ssss)a(siiddada{sv})a{sv})")
				     OUT("a(iiduba(ssss)a{sv})") OUT("a{sv}")))
			IFACE(SAVER, METHOD("GetActive", OUT("b"))) "</node>";

int main(int argc, char **argv)
{
	static const GDBusInterfaceVTable table = { .method_call = method };
	static const char *const paths[] = {
		"/org/cinnamon/Muffin/ScreenCast", SESSION,
		"/org/cinnamon/Muffin/DisplayConfig", SAVER_PATH
	};
	static const char *const cases[] = { "/cinnamon/confirm",
					     "/cinnamon/cancel",
					     "/cinnamon/invalid-selection",
					     "/cinnamon/hotplug",
					     "/cinnamon/locked",
					     "/cinnamon/locked-during-picker",
					     "/cinnamon/closed",
					     "/cinnamon/invalid-node",
					     "/cinnamon/locked-during-share",
					     "/cinnamon/denied" };
	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusNodeInfo) info =
		g_dbus_node_info_new_for_xml(xml, &error);
	g_autoptr(GTestDBus) env = g_test_dbus_new(G_TEST_DBUS_NONE);

	g_assert_no_error(error);
	g_test_init(&argc, &argv, NULL);
	g_test_dbus_up(env);
	service = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error(error);
	own(CAST);
	own(SAVER);
	for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
		g_assert_cmpuint(g_dbus_connection_register_object(
					 service, paths[i], info->interfaces[i],
					 &table, NULL, NULL, &error),
				 >, 0);
		g_assert_no_error(error);
	}
	main_loop = g_main_loop_new(NULL, FALSE);
	for (guint i = 0; i < G_N_ELEMENTS(cases); i++)
		g_test_add_data_func(cases[i], GINT_TO_POINTER(i), test);
	int result = g_test_run();

	g_main_loop_unref(main_loop);
	g_clear_object(&service);
	g_test_dbus_down(env);
	return result;
}
