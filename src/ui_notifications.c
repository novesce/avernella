// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_notifications.h"
#include <gio/gio.h>

struct notice {
	char *name, *body;
	gboolean call;
};

static gint in_flight;

static gpointer notify(gpointer data)
{
	struct notice *notice = data;

	g_autoptr(GDBusConnection) bus =
		g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

	if (bus) {
		const char *summary = notice->name;
		const char *body = notice->body ? notice->body :
				   notice->call ? "Incoming LINE call" :
						  "New LINE message";
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
			bus, "org.freedesktop.Notifications",
			"/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", "Notify",
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
