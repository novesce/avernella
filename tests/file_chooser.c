// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_chooser.h"
#include <string.h>

#define PORTAL "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define REQUEST "org.freedesktop.portal.Request"

static const char portal_xml[] =
	"<node><interface name='org.freedesktop.portal.FileChooser'>"
	"<method name='OpenFile'><arg type='s' direction='in'/>"
	"<arg type='s' direction='in'/><arg type='a{sv}' direction='in'/>"
	"<arg type='o' direction='out'/></method></interface>"
	"<interface name='" REQUEST
	"'><method name='Close'/></interface></node>";

enum scenario {
	SELECT_LOCAL,
	SELECT_DOCUMENT,
	USER_CANCEL,
	APP_CANCEL,
	REJECT_REMOTE,
	REJECT_MULTIPLE,
};

struct fixture {
	enum scenario scenario;
	GDBusInterfaceInfo *request;
	GCancellable *cancel;
	GError *error;
	char *path;
	guint request_id;
	gint done;
	gboolean closed;
};

static gpointer choose(gpointer data)
{
	struct fixture *f = data;
	const char *parent = f->scenario == SELECT_DOCUMENT ?
				     "wayland:test_export" :
				     "x11:17ac0";
	const char *images = f->scenario == SELECT_LOCAL ? "Photos" : NULL;

	f->path = line_choose_file(parent, "Choose attachment", images,
				   f->cancel, &f->error);
	g_atomic_int_set(&f->done, TRUE);
	return NULL;
}

static void close_request(GDBusConnection *bus, const char *sender,
			  const char *path, const char *interface,
			  const char *method, GVariant *params,
			  GDBusMethodInvocation *call, gpointer data)
{
	struct fixture *f = data;

	(void)bus;
	(void)sender;
	(void)path;
	(void)interface;
	(void)params;
	g_assert_cmpstr(method, ==, "Close");
	f->closed = TRUE;
	g_dbus_method_invocation_return_value(call, NULL);
}

static const GDBusInterfaceVTable request_table = {
	.method_call = close_request,
};

static gboolean cancel_dialog(gpointer data)
{
	g_cancellable_cancel(data);
	return G_SOURCE_REMOVE;
}

static void open_file(GDBusConnection *bus, const char *sender,
		      const char *path, const char *interface,
		      const char *method, GVariant *params,
		      GDBusMethodInvocation *call, gpointer data)
{
	struct fixture *f = data;
	const char *parent, *title, *token;
	gboolean modal = FALSE;

	g_autoptr(GVariant) options = NULL;

	(void)path;
	(void)interface;
	g_assert_cmpstr(method, ==, "OpenFile");
	g_variant_get(params, "(&s&s@a{sv})", &parent, &title, &options);
	g_assert_cmpstr(title, ==, "Choose attachment");
	g_assert_cmpstr(parent, ==,
			f->scenario == SELECT_DOCUMENT ? "wayland:test_export" :
							 "x11:17ac0");
	g_assert_true(g_variant_lookup(options, "modal", "b", &modal));
	g_assert_true(modal);
	g_assert_true(g_variant_lookup(options, "handle_token", "&s", &token));
	g_autoptr(GVariant) filters = g_variant_lookup_value(
		options, "filters", G_VARIANT_TYPE("a(sa(us))"));

	if (f->scenario == SELECT_LOCAL)
		g_assert_cmpuint(g_variant_n_children(filters), ==, 1);
	else
		g_assert_null(filters);
	g_autofree char *caller = g_strdup(sender + 1);

	g_strdelimit(caller, ".", '_');
	g_autofree char *expected =
		g_strdup_printf(PORTAL_PATH "/request/%s/%s", caller, token);
	const char *handle = f->scenario == SELECT_DOCUMENT ? PORTAL_PATH
				     "/request/legacy/1" :
							      expected;
	g_autoptr(GError) error = NULL;

	f->request_id = g_dbus_connection_register_object(
		bus, handle, f->request, &request_table, f, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(f->request_id, >, 0);
	g_dbus_method_invocation_return_value(call,
					      g_variant_new("(o)", handle));
	if (f->scenario == APP_CANCEL) {
		g_timeout_add(20, cancel_dialog, f->cancel);
		return;
	}
	GVariantBuilder results;
	const char *uri = "file:///tmp/test%20photo.jpg";

	if (f->scenario == SELECT_DOCUMENT)
		uri = "file:///run/user/1000/doc/0a17f2/Report%20%231.pdf";
	else if (f->scenario == REJECT_REMOTE)
		uri = "file://other-host/tmp/test.txt";
	GVariantBuilder files;

	g_variant_builder_init(&files, G_VARIANT_TYPE_STRING_ARRAY);
	g_variant_builder_add(&files, "s", uri);
	if (f->scenario == REJECT_MULTIPLE)
		g_variant_builder_add(&files, "s", "file:///tmp/second.txt");
	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "uris",
			      g_variant_builder_end(&files));
	if (f->scenario == SELECT_DOCUMENT) {
		GVariantBuilder wrong;

		g_variant_builder_init(&wrong, G_VARIANT_TYPE_VARDICT);
		g_dbus_connection_emit_signal(
			bus, sender, expected, REQUEST, "Response",
			g_variant_new("(ua{sv})", 1u, &wrong), NULL);
	}
	g_dbus_connection_emit_signal(
		bus, sender, handle, REQUEST, "Response",
		g_variant_new("(ua{sv})", f->scenario == USER_CANCEL ? 1u : 0u,
			      &results),
		&error);
	g_assert_no_error(error);
}

static const GDBusInterfaceVTable portal_table = {
	.method_call = open_file,
};

static void portal(gconstpointer scenario)
{
	g_autoptr(GTestDBus) dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();

	g_test_dbus_up(dbus);
	g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
		g_test_dbus_get_bus_address(dbus),
		G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
		NULL, NULL, &error);
	g_assert_no_error(error);
	g_autoptr(GDBusNodeInfo) info =
		g_dbus_node_info_new_for_xml(portal_xml, &error);
	g_assert_no_error(error);
	struct fixture f = {
		.scenario = GPOINTER_TO_INT(scenario),
		.request = info->interfaces[1],
		.cancel = cancel,
	};
	guint object = g_dbus_connection_register_object(bus, PORTAL_PATH,
							 info->interfaces[0],
							 &portal_table, &f,
							 NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(object, >, 0);
	g_autoptr(GVariant) name = g_dbus_connection_call_sync(
		bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "RequestName",
		g_variant_new("(su)", PORTAL, 4u), G_VARIANT_TYPE("(u)"),
		G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(name);
	GThread *thread = g_thread_new("fixture-chooser", choose, &f);
	gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	while (g_get_monotonic_time() < end) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		if (g_atomic_int_get(&f.done) &&
		    (f.scenario != APP_CANCEL || f.closed))
			break;
		g_usleep(1000);
	}
	g_assert_true(g_atomic_int_get(&f.done));
	g_thread_join(thread);
	switch (f.scenario) {
	case SELECT_LOCAL:
		g_assert_no_error(f.error);
		g_assert_cmpstr(f.path, ==, "/tmp/test photo.jpg");
		break;
	case SELECT_DOCUMENT:
		g_assert_no_error(f.error);
		g_assert_cmpstr(f.path, ==,
				"/run/user/1000/doc/0a17f2/Report #1.pdf");
		break;
	case APP_CANCEL:
		g_assert_true(f.closed);
		G_GNUC_FALLTHROUGH;
	case USER_CANCEL:
		g_assert_error(f.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		g_assert_null(f.path);
		break;
	case REJECT_REMOTE:
	case REJECT_MULTIPLE:
		g_assert_error(f.error, G_IO_ERROR, G_IO_ERROR_FAILED);
		g_assert_null(f.path);
		break;
	}
	g_clear_error(&f.error);
	g_free(f.path);
	g_dbus_connection_unregister_object(bus, f.request_id);
	g_dbus_connection_unregister_object(bus, object);
	g_dbus_connection_close_sync(bus, NULL, NULL);
}

static void run(gconstpointer scenario)
{
	if (g_test_subprocess())
		portal(scenario);
	else {
		g_test_trap_subprocess(NULL, 10000000, 0);
		g_test_trap_assert_passed();
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/file-chooser/x11", GINT_TO_POINTER(SELECT_LOCAL),
			     run);
	g_test_add_data_func("/file-chooser/wayland-document",
			     GINT_TO_POINTER(SELECT_DOCUMENT), run);
	g_test_add_data_func("/file-chooser/user-cancel",
			     GINT_TO_POINTER(USER_CANCEL), run);
	g_test_add_data_func("/file-chooser/app-cancel",
			     GINT_TO_POINTER(APP_CANCEL), run);
	g_test_add_data_func("/file-chooser/reject-remote",
			     GINT_TO_POINTER(REJECT_REMOTE), run);
	g_test_add_data_func("/file-chooser/reject-multiple",
			     GINT_TO_POINTER(REJECT_MULTIPLE), run);
	return g_test_run();
}
