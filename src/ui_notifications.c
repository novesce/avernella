// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_notifications.h"
#include "ui_lang.h"
#include <gio/gio.h>

#define NOTIFICATIONS "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"

struct notice {
	char *name, *body;
	gboolean call;
};

static gint in_flight;
/* Only the in-flight notification thread accesses this cache. */
static char *markup_owner, *markup_bus;
static gboolean markup;

char *ui_notification_text(const char *text, gboolean markup)
{
	return markup ? g_markup_escape_text(text, -1) : g_strdup(text);
}

/*
 * Desktop services can change owners. Both calls go to the same unique
 * name, and only that peer's capabilities apply to its notification.
 */
static gboolean body_markup(GDBusConnection *bus, const char *owner)
{
	const char *guid = g_dbus_connection_get_guid(bus);

	if (markup_owner && !g_strcmp0(guid, markup_bus) &&
	    !strcmp(owner, markup_owner))
		return markup;
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		bus, owner, NOTIFICATIONS_PATH, NOTIFICATIONS,
		"GetCapabilities", NULL, G_VARIANT_TYPE("(as)"),
		G_DBUS_CALL_FLAGS_NO_AUTO_START, 1200, NULL, NULL);

	if (!reply)
		return TRUE;
	g_autofree const char **caps = NULL;

	g_variant_get(reply, "(^a&s)", &caps);
	g_free(markup_bus);
	markup_bus = g_strdup(guid);
	g_free(markup_owner);
	markup_owner = g_strdup(owner);
	markup = caps && g_strv_contains(caps, "body-markup");
	return markup;
}

static gpointer notify(gpointer data)
{
	struct notice *notice = data;

	g_autoptr(GDBusConnection) bus =
		g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (bus)
		g_dbus_connection_set_exit_on_close(bus, FALSE);
	g_autoptr(GVariant) target =
		bus ? g_dbus_connection_call_sync(
			      bus, "org.freedesktop.DBus",
			      "/org/freedesktop/DBus", "org.freedesktop.DBus",
			      "GetNameOwner",
			      g_variant_new("(s)", NOTIFICATIONS),
			      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
			      1200, NULL, NULL) :
		      NULL;

	if (target) {
		const char *owner;
		const char *summary = notice->name;

		g_variant_get(target, "(&s)", &owner);
		g_autofree char *body = ui_notification_text(
			notice->body ? notice->body :
			notice->call ? _("Incoming LINE call") :
				       _("New LINE message"),
			body_markup(bus, owner));
		GVariantBuilder hints;

		g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&hints, "{sv}", "desktop-entry",
				      g_variant_new_string("avernella"));
		g_variant_builder_add(
			&hints, "{sv}", "category",
			g_variant_new_string(notice->call ? "im.received" :
							    "im"));
		g_variant_builder_add(&hints, "{sv}", "transient",
				      g_variant_new_boolean(TRUE));
		g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
			bus, owner, NOTIFICATIONS_PATH, NOTIFICATIONS, "Notify",
			g_variant_new("(susss^asa{sv}i)", "Avernella", 0u,
				      "avernella", summary, body,
				      (const char *[]){ NULL }, &hints, -1),
			G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
			1200, NULL, NULL);
	}
	g_free(notice->name);
	g_free(notice->body);
	g_free(notice);
	g_atomic_int_set(&in_flight, 0);
	return NULL;
}

static void post(const char *name, const char *body, gboolean incoming_call)
{
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS") ||
	    !g_atomic_int_compare_and_exchange(&in_flight, 0, 1))
		return;
	struct notice *notice = g_new(struct notice, 1);

	notice->name = g_utf8_make_valid(name && *name ? name : "LINE", -1);
	notice->body = body ? g_utf8_make_valid(body, -1) : NULL;
	notice->call = incoming_call;
	GThread *thread = g_thread_try_new("line-notify", notify, notice, NULL);

	if (thread) {
		g_thread_unref(thread);
	} else {
		g_free(notice->name);
		g_free(notice->body);
		g_free(notice);
		g_atomic_int_set(&in_flight, 0);
	}
}

void ui_desktop_notify(const char *name, gboolean incoming_call)
{
	post(name, NULL, incoming_call);
}

void ui_desktop_notify_text(const char *summary, const char *body)
{
	post(summary, body, FALSE);
}
