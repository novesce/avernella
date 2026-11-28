// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_chooser.h"
#include <string.h>

#define DESKTOP_PORTAL "org.freedesktop.portal.Desktop"
#define DESKTOP_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_REQUEST "org.freedesktop.portal.Request"
#define FILE_CHOOSER "org.freedesktop.portal.FileChooser"

struct choice {
	GMainLoop *loop;
	const char *handle;
	char *path;
	guint result;
	gboolean answered;
};

static void file_response(GDBusConnection *bus, const char *sender,
			  const char *path, const char *interface,
			  const char *name, GVariant *parameters, gpointer data)
{
	struct choice *choice = data;

	(void)bus;
	(void)sender;
	(void)interface;
	(void)name;
	if (strcmp(path, choice->handle) || choice->answered ||
	    !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
		return;
	g_autoptr(GVariant) results = NULL;

	g_variant_get(parameters, "(u@a{sv})", &choice->result, &results);
	g_autoptr(GVariant) uris =
		g_variant_lookup_value(results, "uris", G_VARIANT_TYPE("as"));

	if (!choice->result && uris && g_variant_n_children(uris) == 1) {
		const char *uri;

		g_autofree char *host = NULL;

		g_variant_get_child(uris, 0, "&s", &uri);
		choice->path = g_filename_from_uri(uri, &host, NULL);
		if (host && g_ascii_strcasecmp(host, "localhost"))
			g_clear_pointer(&choice->path, g_free);
	}
	choice->answered = TRUE;
	g_main_loop_quit(choice->loop);
}

static gboolean stop_wait(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static gboolean cancelled(GCancellable *cancel, gpointer data)
{
	(void)cancel;
	return stop_wait(data);
}

static void add_image_filter(GVariantBuilder *options, const char *label)
{
	static const char *const mime_types[] = { "image/jpeg", "image/png",
						  "image/gif", "image/webp" };
	GVariantBuilder types, filters;

	g_variant_builder_init(&types, G_VARIANT_TYPE("a(us)"));
	for (gsize i = 0; i < G_N_ELEMENTS(mime_types); i++)
		g_variant_builder_add(&types, "(us)", 1u, mime_types[i]);
	g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
	g_variant_builder_add(&filters, "(sa(us))", label, &types);
	g_variant_builder_add(options, "{sv}", "filters",
			      g_variant_builder_end(&filters));
}

static char *choose_file(GDBusConnection *bus, GMainContext *context,
			 const char *parent, const char *title,
			 const char *image_label, GCancellable *cancel,
			 GError **error)
{
	const char *unique = g_dbus_connection_get_unique_name(bus);
	g_autofree char *sender = g_strdup(unique + 1);
	g_autofree char *token = g_uuid_string_random();

	g_strdelimit(sender, ".", '_');
	g_strdelimit(token, "-", '_');
	g_autofree char *handle_token = g_strconcat("avernella_", token, NULL);
	g_autofree char *predicted = g_strdup_printf(
		DESKTOP_PATH "/request/%s/%s", sender, handle_token);
	g_autoptr(GMainLoop) loop = g_main_loop_new(context, FALSE);
	struct choice choice = { .loop = loop, .handle = predicted };
	/*
	 * Subscribe before OpenFile. The new request path is predictable, but
	 * older portals may use a different handle. Our private main context
	 * queues either reply until we know the actual path.
	 */
	guint signal = g_dbus_connection_signal_subscribe(
		bus, DESKTOP_PORTAL, PORTAL_REQUEST, "Response", NULL, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, file_response, &choice, NULL);
	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options, "{sv}", "handle_token",
			      g_variant_new_string(handle_token));
	g_variant_builder_add(&options, "{sv}", "modal",
			      g_variant_new_boolean(TRUE));
	if (image_label)
		add_image_filter(&options, image_label);
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		bus, DESKTOP_PORTAL, DESKTOP_PATH, FILE_CHOOSER, "OpenFile",
		g_variant_new("(ssa{sv})", parent ? parent : "", title,
			      &options),
		G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, cancel,
		error);
	if (reply) {
		g_variant_get(reply, "(&o)", &choice.handle);
		g_autoptr(GSource) timeout = g_timeout_source_new_seconds(600);
		g_autoptr(GSource) abort = g_cancellable_source_new(cancel);

		g_source_set_callback(timeout, stop_wait, loop, NULL);
		g_source_set_callback(abort, G_SOURCE_FUNC(cancelled), loop,
				      NULL);
		g_source_attach(timeout, context);
		g_source_attach(abort, context);
		g_main_loop_run(loop);
		g_source_destroy(timeout);
		g_source_destroy(abort);
		if (!choice.answered &&
		    !g_cancellable_set_error_if_cancelled(cancel, error))
			g_set_error_literal(error, G_IO_ERROR,
					    G_IO_ERROR_TIMED_OUT,
					    "The file chooser timed out.");
		else if (choice.answered && choice.result == 1)
			g_set_error_literal(error, G_IO_ERROR,
					    G_IO_ERROR_CANCELLED,
					    "File selection cancelled.");
		else if (choice.answered && !choice.path)
			g_set_error_literal(
				error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"The desktop did not return a local file.");
	}
	if (!choice.answered && (reply || g_cancellable_is_cancelled(cancel))) {
		g_autoptr(GVariant) closed = g_dbus_connection_call_sync(
			bus, DESKTOP_PORTAL, choice.handle, PORTAL_REQUEST,
			"Close", NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
			1000, NULL, NULL);
	}

	g_dbus_connection_signal_unsubscribe(bus, signal);
	return choice.path;
}

char *line_choose_file(const char *parent, const char *title,
		       const char *image_label, GCancellable *cancel,
		       GError **error)
{
	g_autoptr(GMainContext) context = g_main_context_new();

	g_main_context_push_thread_default(context);
	g_autoptr(GDBusConnection) bus =
		g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, error);
	char *path = bus ? choose_file(bus, context, parent, title, image_label,
				       cancel, error) :
			   NULL;

	g_main_context_pop_thread_default(context);
	return path;
}
