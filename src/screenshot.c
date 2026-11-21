// SPDX-License-Identifier: GPL-3.0-or-later
#include "screenshot.h"

static const char portal[] = "org.freedesktop.portal.Desktop";
static const char desktop[] = "/org/freedesktop/portal/desktop";
static const char request[] = "org.freedesktop.portal.Request";

struct screenshot {
	GMainLoop *loop;
	char *path;
	guint result;
	gboolean answered;
};

static void response(GDBusConnection *bus, const char *sender, const char *path,
		     const char *interface, const char *name,
		     GVariant *parameters, gpointer data)
{
	struct screenshot *image = data;

	(void)bus;
	(void)sender;
	(void)path;
	(void)interface;
	(void)name;
	if (image->answered ||
	    !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
		return;
	g_autoptr(GVariant) results = NULL;
	const char *uri = NULL;

	g_variant_get(parameters, "(u@a{sv})", &image->result, &results);
	if (!image->result && g_variant_lookup(results, "uri", "&s", &uri)) {
		g_autofree char *host = NULL;

		image->path = g_filename_from_uri(uri, &host, NULL);
		if (host && g_ascii_strcasecmp(host, "localhost"))
			g_clear_pointer(&image->path, g_free);
	}
	image->answered = TRUE;
	g_main_loop_quit(image->loop);
}

static gboolean stop_waiting(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static gboolean cancelled(GCancellable *cancel, gpointer data)
{
	(void)cancel;
	return stop_waiting(data);
}

char *line_desktop_screenshot(const char *parent, GCancellable *cancel,
			      GError **error)
{
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS")) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "A desktop Screenshot portal is required.");
		return NULL;
	}
	g_autoptr(GMainContext) context = g_main_context_new();

	g_main_context_push_thread_default(context);
	g_autoptr(GDBusConnection) bus =
		g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, error);
	if (!bus) {
		g_main_context_pop_thread_default(context);
		return NULL;
	}
	const char *unique = g_dbus_connection_get_unique_name(bus);
	g_autofree char *sender = g_strdup(unique + 1);

	g_strdelimit(sender, ".", '_');
	g_autofree char *token = g_uuid_string_random();

	g_strdelimit(token, "-", '_');
	g_autofree char *predicted = g_strdup_printf("%s/request/%s/line_%s",
						     desktop, sender, token);
	g_autoptr(GMainLoop) loop = g_main_loop_new(context, FALSE);
	struct screenshot image = { .loop = loop };
	guint subscription = g_dbus_connection_signal_subscribe(
		bus, portal, request, "Response", predicted, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, response, &image, NULL);
	g_autofree char *handle_token = g_strconcat("line_", token, NULL);
	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options, "{sv}", "handle_token",
			      g_variant_new_string(handle_token));
	g_variant_builder_add(&options, "{sv}", "interactive",
			      g_variant_new_boolean(TRUE));
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		bus, portal, desktop, "org.freedesktop.portal.Screenshot",
		"Screenshot", g_variant_new("(sa{sv})", parent, &options),
		G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, cancel,
		error);
	const char *handle = predicted;

	if (reply) {
		g_variant_get(reply, "(&o)", &handle);
		if (strcmp(handle, predicted)) {
			g_dbus_connection_signal_unsubscribe(bus, subscription);
			subscription = g_dbus_connection_signal_subscribe(
				bus, portal, request, "Response", handle, NULL,
				G_DBUS_SIGNAL_FLAGS_NONE, response, &image,
				NULL);
		}
		g_autoptr(GSource) timeout = g_timeout_source_new_seconds(120);
		g_autoptr(GSource) abort = g_cancellable_source_new(cancel);

		g_source_set_callback(timeout, stop_waiting, loop, NULL);
		g_source_set_callback(abort, G_SOURCE_FUNC(cancelled), loop,
				      NULL);
		g_source_attach(timeout, context);
		g_source_attach(abort, context);
		g_main_loop_run(loop);
		g_source_destroy(timeout);
		g_source_destroy(abort);
		if (!image.answered) {
			g_dbus_connection_call(bus, portal, handle, request,
					       "Close", NULL, NULL,
					       G_DBUS_CALL_FLAGS_NO_AUTO_START,
					       1000, NULL, NULL, NULL);
			if (!g_cancellable_set_error_if_cancelled(cancel,
								  error))
				g_set_error_literal(
					error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
					"The screenshot request timed out.");
		} else if (image.result == 1) {
			g_set_error_literal(error, G_IO_ERROR,
					    G_IO_ERROR_CANCELLED,
					    "Screenshot cancelled.");
		} else if (!image.path) {
			g_set_error_literal(
				error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"The desktop did not return a local screenshot.");
		}
	}
	g_dbus_connection_signal_unsubscribe(bus, subscription);
	g_main_context_pop_thread_default(context);
	return image.path;
}
