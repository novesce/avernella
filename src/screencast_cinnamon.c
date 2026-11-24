// SPDX-License-Identifier: GPL-3.0-or-later
#include "screencast_cinnamon.h"
#include "monitor_picker.h"

#define CAST "org.cinnamon.Muffin.ScreenCast"
#define CAST_PATH "/org/cinnamon/Muffin/ScreenCast"
#define DISPLAY "org.cinnamon.Muffin.DisplayConfig"
#define DISPLAY_PATH "/org/cinnamon/Muffin/DisplayConfig"
#define SAVER "org.cinnamon.ScreenSaver"
#define SAVER_PATH "/org/cinnamon/ScreenSaver"
#define STATE_TYPE "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"

struct line_cinnamon_cast {
	GDBusConnection *bus;
	GCancellable *cancel;
	char *owner, *session;
	guint subscriptions[4];
	guint32 node;
	gulong disconnected;
};

static GVariant *call(GDBusConnection *bus, const char *name, const char *path,
		      const char *iface, const char *method, GVariant *args,
		      const char *type, GCancellable *cancel, GError **error)
{
	return g_dbus_connection_call_sync(bus, name, path, iface, method, args,
					   G_VARIANT_TYPE(type),
					   G_DBUS_CALL_FLAGS_NO_AUTO_START,
					   3000, cancel, error);
}

static gboolean unlocked(struct line_cinnamon_cast *c, GError **error)
{
	g_autoptr(GVariant) active = call(c->bus, SAVER, SAVER_PATH, SAVER,
					  "GetActive", NULL, "(b)", c->cancel,
					  error);
	gboolean locked;

	if (!active)
		return FALSE;
	g_variant_get(active, "(b)", &locked);
	if (locked)
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			"Unlock the desktop before sharing a monitor.");
	return !locked;
}

static GVariant *display_state(struct line_cinnamon_cast *c, GError **error)
{
	return call(c->bus, c->owner, DISPLAY_PATH, DISPLAY, "GetCurrentState",
		    NULL, STATE_TYPE, c->cancel, error);
}

static gboolean active_mode(GVariant *monitor)
{
	g_autoptr(GVariant) modes = g_variant_get_child_value(monitor, 1);

	for (gsize i = 0; i < g_variant_n_children(modes); i++) {
		g_autoptr(GVariant) mode = g_variant_get_child_value(modes, i);
		g_autoptr(GVariant) props = g_variant_get_child_value(mode, 6);
		gboolean current = FALSE;

		if (g_variant_lookup(props, "is-current", "b", &current) &&
		    current)
			return TRUE;
	}
	return FALSE;
}

static char *pick_monitor(struct line_cinnamon_cast *c, GError **error)
{
	g_autoptr(GVariant) state = display_state(c, error);
	g_autoptr(GPtrArray) connectors =
		g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	guint32 serial, latest;

	if (!state)
		return NULL;
	g_variant_get_child(state, 0, "u", &serial);
	g_autoptr(GVariant) monitors = g_variant_get_child_value(state, 1);

	for (gsize i = 0; i < g_variant_n_children(monitors); i++) {
		g_autoptr(GVariant) m = g_variant_get_child_value(monitors, i);
		g_autoptr(GVariant) spec = g_variant_get_child_value(m, 0);
		g_autoptr(GVariant) props = g_variant_get_child_value(m, 2);
		const char *connector, *name = NULL;

		g_variant_get_child(spec, 0, "&s", &connector);
		/* Muffin treats an empty connector as an implicit primary display. */
		if (!*connector || !active_mode(m))
			continue;
		g_variant_lookup(props, "display-name", "&s", &name);
		g_ptr_array_add(connectors, g_strdup(connector));
		g_ptr_array_add(names,
				g_strdup(name && *name ? name : connector));
	}
	if (!connectors->len) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				    "No active monitor is available to share.");
		return NULL;
	}
	g_autofree char *chosen =
		line_pick_share_monitor((const char *const *)connectors->pdata,
					(const char *const *)names->pdata,
					connectors->len, c->cancel, error);

	if (!chosen || g_cancellable_set_error_if_cancelled(c->cancel, error))
		return NULL;
	g_autoptr(GVariant) now = display_state(c, error);

	if (!now)
		return NULL;
	g_variant_get_child(now, 0, "u", &latest);
	if (serial != latest ||
	    !g_ptr_array_find_with_equal_func(connectors, chosen, g_str_equal,
					      NULL)) {
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"The display selection changed. Choose a monitor again.");
		return NULL;
	}
	return g_steal_pointer(&chosen);
}

static void signal_received(GDBusConnection *bus, const char *sender,
			    const char *path, const char *iface,
			    const char *signal, GVariant *params, gpointer data)
{
	struct line_cinnamon_cast *c = data;

	(void)bus;
	(void)sender;
	(void)path;
	(void)iface;
	if (!strcmp(signal, "PipeWireStreamAdded")) {
		if (g_variant_is_of_type(params, G_VARIANT_TYPE("(u)")))
			g_variant_get(params, "(u)", &c->node);
		return;
	}
	if (!strcmp(signal, "ActiveChanged")) {
		gboolean locked = FALSE;

		if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(b)")))
			return;
		g_variant_get(params, "(b)", &locked);
		if (!locked)
			return;
	}
	if (!strcmp(signal, "NameOwnerChanged")) {
		const char *new_owner;

		g_variant_get_child(params, 2, "&s", &new_owner);
		if (!g_strcmp0(new_owner, c->owner))
			return;
	}
	g_cancellable_cancel(c->cancel);
}

static void bus_closed(GDBusConnection *bus, gboolean vanished, GError *error,
		       gpointer data)
{
	(void)bus;
	(void)vanished;
	(void)error;
	g_cancellable_cancel(data);
}

static gboolean expire(gpointer data)
{
	*(gboolean *)data = TRUE;
	return G_SOURCE_REMOVE;
}

static gboolean cancelled(GCancellable *cancel, gpointer data)
{
	(void)cancel;
	(void)data;
	return G_SOURCE_REMOVE;
}

static gboolean wait_node(struct line_cinnamon_cast *c, GMainContext *context,
			  GError **error)
{
	g_autoptr(GSource) timeout = g_timeout_source_new_seconds(10);
	g_autoptr(GSource) abort = g_cancellable_source_new(c->cancel);
	gboolean expired = FALSE;

	g_source_set_callback(timeout, expire, &expired, NULL);
	g_source_set_callback(abort, G_SOURCE_FUNC(cancelled), NULL, NULL);
	g_source_attach(timeout, context);
	g_source_attach(abort, context);
	while (!c->node && !expired && !g_cancellable_is_cancelled(c->cancel))
		g_main_context_iteration(context, TRUE);
	g_source_destroy(abort);
	g_source_destroy(timeout);
	if (g_cancellable_set_error_if_cancelled(c->cancel, error))
		return FALSE;
	/* UINT32_MAX tells PipeWire to autoselect an unrelated node. */
	if (!c->node || c->node == G_MAXUINT32) {
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Cinnamon did not provide a screen sharing stream.");
		return FALSE;
	}
	return TRUE;
}

void line_cinnamon_stop(struct line_cinnamon_cast *c)
{
	if (!c)
		return;
	if (c->bus) {
		for (guint i = 0; i < G_N_ELEMENTS(c->subscriptions); i++) {
			if (c->subscriptions[i])
				g_dbus_connection_signal_unsubscribe(
					c->bus, c->subscriptions[i]);
		}
		if (c->disconnected)
			g_signal_handler_disconnect(c->bus, c->disconnected);
		if (c->session && !g_dbus_connection_is_closed(c->bus)) {
			g_autoptr(GVariant) stopped = call(
				c->bus, c->owner, c->session, CAST ".Session",
				"Stop", NULL, "()", NULL, NULL);
		}
		/* Releasing this dedicated owner also closes a session whose
		 * CreateSession reply was lost to cancellation.
		 */
		g_dbus_connection_close_sync(c->bus, NULL, NULL);
		g_object_unref(c->bus);
	}
	g_object_unref(c->cancel);
	g_free(c->session);
	g_free(c->owner);
	g_free(c);
}

struct line_cinnamon_cast *line_cinnamon_start(GMainContext *context,
					       GCancellable *cancel,
					       guint32 *node, GError **error)
{
	g_autofree char *address = g_dbus_address_get_for_bus_sync(
		G_BUS_TYPE_SESSION, cancel, error);
	g_autoptr(GVariant) owner = NULL, session = NULL, stream = NULL,
			    started = NULL;
	g_autofree char *connector = NULL;
	const char *stream_path;
	GVariantBuilder options;

	if (!address)
		return NULL;
	struct line_cinnamon_cast *c = g_new0(struct line_cinnamon_cast, 1);

	c->cancel = g_object_ref(cancel);
	c->bus = g_dbus_connection_new_for_address_sync(
		address,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, cancel, error);
	if (!c->bus)
		goto fail;
	g_dbus_connection_set_exit_on_close(c->bus, FALSE);
	c->disconnected = g_signal_connect(c->bus, "closed",
					   G_CALLBACK(bus_closed), cancel);
	c->subscriptions[0] = g_dbus_connection_signal_subscribe(
		c->bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
		"NameOwnerChanged", "/org/freedesktop/DBus", CAST,
		G_DBUS_SIGNAL_FLAGS_NONE, signal_received, c, NULL);
	owner = call(c->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		     "org.freedesktop.DBus", "GetNameOwner",
		     g_variant_new("(s)", CAST), "(s)", cancel, error);
	if (!owner)
		goto fail;
	g_variant_get(owner, "(s)", &c->owner);
	c->subscriptions[1] = g_dbus_connection_signal_subscribe(
		c->bus, SAVER, SAVER, "ActiveChanged", SAVER_PATH, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, signal_received, c, NULL);
	if (!unlocked(c, error))
		goto fail;
	connector = pick_monitor(c, error);
	if (!connector || !unlocked(c, error))
		goto fail;
	session = call(c->bus, c->owner, CAST_PATH, CAST, "CreateSession",
		       g_variant_new("(a{sv})", NULL), "(o)", cancel, error);
	if (!session)
		goto fail;
	g_variant_get(session, "(o)", &c->session);
	c->subscriptions[2] = g_dbus_connection_signal_subscribe(
		c->bus, c->owner, CAST ".Session", "Closed", c->session, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, signal_received, c, NULL);
	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options, "{sv}", "cursor-mode",
			      g_variant_new_uint32(1));
	stream = call(c->bus, c->owner, c->session, CAST ".Session",
		      "RecordMonitor",
		      g_variant_new("(sa{sv})", connector, &options), "(o)",
		      cancel, error);
	if (!stream)
		goto fail;
	g_variant_get(stream, "(&o)", &stream_path);
	c->subscriptions[3] = g_dbus_connection_signal_subscribe(
		c->bus, c->owner, CAST ".Stream", "PipeWireStreamAdded",
		stream_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_received, c,
		NULL);
	started = call(c->bus, c->owner, c->session, CAST ".Session", "Start",
		       NULL, "()", cancel, error);
	if (started && wait_node(c, context, error)) {
		*node = c->node;
		return c;
	}
fail:
	line_cinnamon_stop(c);
	return NULL;
}
