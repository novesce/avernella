// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_tray.h"
#include "ui_lang.h"
#include "ui_logo.h"
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#define ITEM_PATH "/StatusNotifierItem"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define MENU_PATH "/Menu"
#define MENU_IFACE "com.canonical.dbusmenu"
#define INPUT(t) "<arg type='" t "' direction='in'/>"
#define OUTPUT(t) "<arg type='" t "' direction='out'/>"
#define METHOD(n, a) "<method name='" n "'>" a "</method>"
#define PROP(n, t) "<property name='" n "' type='" t "' access='read'/>"
#define SIGNAL(n) "<signal name='" n "'/>"
/* clang-format off */
#define ITEM(iface) \
	"<interface name='" iface "'>" \
	PROP("Category", "s") \
	PROP("Id", "s") \
	PROP("Title", "s") \
	PROP("Status", "s") \
	PROP("WindowId", "u") \
	PROP("IconName", "s") \
	PROP("IconPixmap", "a(iiay)") \
	PROP("OverlayIconName", "s") \
	PROP("OverlayIconPixmap", "a(iiay)") \
	PROP("AttentionIconName", "s") \
	PROP("AttentionIconPixmap", "a(iiay)") \
	PROP("ToolTip", "(sa(iiay)ss)") \
	PROP("ItemIsMenu", "b") \
	PROP("Menu", "o") \
	METHOD("Activate", INPUT("i") INPUT("i")) \
	METHOD("SecondaryActivate", INPUT("i") INPUT("i")) \
	METHOD("ContextMenu", INPUT("i") INPUT("i")) \
	METHOD("Scroll", INPUT("i") INPUT("s")) \
	SIGNAL("NewIcon") \
	SIGNAL("NewToolTip") \
	SIGNAL("NewTitle") \
	"</interface>"

#define MENU_XML \
	"<interface name='" MENU_IFACE "'>" \
	PROP("Version", "u") \
	PROP("TextDirection", "s") \
	PROP("Status", "s") \
	PROP("IconThemePath", "as") \
	METHOD("GetLayout", INPUT("i") INPUT("i") INPUT("as") OUTPUT("u") OUTPUT("(ia{sv}av)")) \
	METHOD("GetGroupProperties", INPUT("ai") INPUT("as") OUTPUT("a(ia{sv})")) \
	METHOD("GetProperty", INPUT("i") INPUT("s") OUTPUT("v")) \
	METHOD("Event", INPUT("i") INPUT("s") INPUT("v") INPUT("u")) \
	METHOD("EventGroup", INPUT("a(isvu)") OUTPUT("ai")) \
	METHOD("AboutToShow", INPUT("i") OUTPUT("b")) \
	METHOD("AboutToShowGroup", INPUT("ai") OUTPUT("ai") OUTPUT("ai")) \
	"</interface>"
/* clang-format on */

enum tray_action {
	SHOW = 1,
	HIDE,
	QUIT,
	TOGGLE,
	HOST_GONE
};

struct host {
	const char *interface;
	GDBusProxy *proxy;
	guint generation;
	gboolean registered, visible;
};

struct registration {
	struct host *host;
	guint generation;
};

static struct {
	GMainContext *context;
	GMainLoop *loop;
	GThread *thread;
	GCancellable *cancel;
	GVariant *icon;
	char *address, *name;
	Uint32 event;
	gint available;
	guint pending;
	struct host hosts[2];
} tray;

static const char *const labels[] = { "", N_("Open Avernella"),
				      N_("Hide window"), N_("Quit Avernella") };

static void queue_action(enum tray_action which)
{
	SDL_Event e = { 0 };

	e.type = tray.event;
	e.user.code = which;
	SDL_PushEvent(&e);
}

static void availability(void)
{
	gboolean active = tray.hosts[0].visible || tray.hosts[1].visible;

	if (g_atomic_int_exchange(&tray.available, active) && !active)
		queue_action(HOST_GONE);
}

/* D-Bus StatusNotifierItem pixmaps carry network-order ARGB bytes. */
static GVariant *pixmaps(gboolean with_icon)
{
	GVariantBuilder items;

	g_variant_builder_init(&items, G_VARIANT_TYPE("a(iiay)"));
	if (with_icon && !tray.icon) {
		gsize size = UI_LOGO_EDGE * UI_LOGO_EDGE * 4;
		guint8 *pixels = g_malloc(size);

		if (ui_logo_read(pixels, TRUE))
			tray.icon = g_variant_ref_sink(g_variant_new_from_data(
				G_VARIANT_TYPE_BYTESTRING, pixels, size, TRUE,
				g_free, pixels));
		else
			g_free(pixels);
	}
	if (with_icon && tray.icon)
		g_variant_builder_add(&items, "(ii@ay)", UI_LOGO_EDGE,
				      UI_LOGO_EDGE, tray.icon);
	return g_variant_builder_end(&items);
}

static GVariant *property(GDBusConnection *bus, const char *sender,
			  const char *path, const char *iface, const char *name,
			  GError **e, gpointer user)
{
	(void)bus;
	(void)sender;
	(void)path;
	(void)e;
	(void)user;
	if (!strcmp(iface, MENU_IFACE)) {
		if (!strcmp(name, "Version"))
			return g_variant_new_uint32(3);
		if (!strcmp(name, "TextDirection"))
			return g_variant_new_string("ltr");
		if (!strcmp(name, "IconThemePath"))
			return g_variant_new_strv(NULL, 0);
		return g_variant_new_string("normal");
	}
	if (!strcmp(name, "Category"))
		return g_variant_new_string("Communications");
	if (!strcmp(name, "Id") || !strcmp(name, "IconName"))
		return g_variant_new_string("avernella");
	if (!strcmp(name, "Title"))
		return g_variant_new_string("Avernella");
	if (!strcmp(name, "Status"))
		return g_variant_new_string("Active");
	if (!strcmp(name, "WindowId"))
		return g_variant_new_uint32(0);
	if (!strcmp(name, "ItemIsMenu"))
		return g_variant_new_boolean(FALSE);
	if (!strcmp(name, "Menu"))
		return g_variant_new_object_path(MENU_PATH);
	if (!strcmp(name, "ToolTip"))
		return g_variant_new("(s@a(iiay)ss)", "avernella", pixmaps(FALSE),
				     "Avernella", "");
	if (g_str_has_suffix(name, "Pixmap"))
		return pixmaps(!strcmp(name, "IconPixmap"));
	return g_variant_new_string("");
}

enum menu_property {
	CHILDREN = 1,
	LABEL = 2,
	VISIBLE = 4,
	ENABLED = 8,
	ALL = 15
};

static int requested(GVariant *p, guint index)
{
	g_autoptr(GVariant) names = g_variant_get_child_value(p, index);
	if (!g_variant_n_children(names))
		return ALL;
	GVariantIter it;
	const char *name;
	int mask = 0;

	g_variant_iter_init(&it, names);
	while (g_variant_iter_next(&it, "&s", &name)) {
		if (!strcmp(name, "children-display"))
			mask |= CHILDREN;
		else if (!strcmp(name, "label"))
			mask |= LABEL;
		else if (!strcmp(name, "visible"))
			mask |= VISIBLE;
		else if (!strcmp(name, "enabled"))
			mask |= ENABLED;
	}
	return mask;
}

static GVariant *attributes(int id, int mask)
{
	GVariantBuilder a;

	g_variant_builder_init(&a, G_VARIANT_TYPE("a{sv}"));
	if (!id && (mask & CHILDREN))
		g_variant_builder_add(&a, "{sv}", "children-display",
				      g_variant_new_string("submenu"));
	if (id && (mask & LABEL))
		g_variant_builder_add(&a, "{sv}", "label",
				      g_variant_new_string(_(labels[id])));
	if (id && (mask & VISIBLE))
		g_variant_builder_add(&a, "{sv}", "visible",
				      g_variant_new_boolean(TRUE));
	if (id && (mask & ENABLED))
		g_variant_builder_add(&a, "{sv}", "enabled",
				      g_variant_new_boolean(TRUE));
	return g_variant_builder_end(&a);
}

static GVariant *layout(int id, int depth, int mask)
{
	GVariantBuilder children;

	g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
	if (!id && depth)
		for (int i = SHOW; i <= QUIT; i++)
			g_variant_builder_add(&children, "v",
					      layout(i, 0, mask));
	return g_variant_new("(i@a{sv}@av)", id, attributes(id, mask),
			     g_variant_builder_end(&children));
}

static int number(GVariant *p, guint i)
{
	int v;

	g_variant_get_child(p, i, "i", &v);
	return v;
}

static gboolean menu_event(GVariant *p)
{
	int id = number(p, 0);
	const char *event;

	g_variant_get_child(p, 1, "&s", &event);
	if (id < SHOW || id > QUIT)
		return FALSE;
	if (!strcmp(event, "clicked"))
		queue_action(id);
	return TRUE;
}

static GVariant *menu_call(const char *method, GVariant *p)
{
	if (!strcmp(method, "GetLayout")) {
		int id = number(p, 0);

		if (id < 0 || id > QUIT)
			return NULL;
		return g_variant_new("(u@(ia{sv}av))", 1,
				     layout(id, number(p, 1), requested(p, 2)));
	}
	if (!strcmp(method, "GetProperty")) {
		const char *key;
		int id = number(p, 0);

		g_variant_get_child(p, 1, "&s", &key);
		if (id < 0 || id > QUIT)
			return NULL;
		g_autoptr(GVariant) props =
			g_variant_ref_sink(attributes(id, ALL));
		g_autoptr(GVariant) val =
			g_variant_lookup_value(props, key, NULL);
		return val ? g_variant_new("(v)", val) : NULL;
	}
	if (!strcmp(method, "GetGroupProperties")) {
		g_autoptr(GVariant) ids = g_variant_get_child_value(p, 0);
		GVariantBuilder out;

		g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
		gsize n = g_variant_n_children(ids);
		int mask = requested(p, 1);

		for (gsize i = 0; i < (n ? n : G_N_ELEMENTS(labels)); i++) {
			int id = n ? number(ids, i) : (int)i;

			if (id >= 0 && id <= QUIT)
				g_variant_builder_add(&out, "(i@a{sv})", id,
						      attributes(id, mask));
		}
		return g_variant_new("(a(ia{sv}))", &out);
	}
	if (!strcmp(method, "Event"))
		return menu_event(p) ? g_variant_new("()") : NULL;
	if (!strcmp(method, "EventGroup")) {
		g_autoptr(GVariant) events = g_variant_get_child_value(p, 0);
		GVariantBuilder errors;

		g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
		for (gsize i = 0; i < g_variant_n_children(events); i++) {
			g_autoptr(GVariant) event =
				g_variant_get_child_value(events, i);
			if (!menu_event(event))
				g_variant_builder_add(&errors, "i",
						      number(event, 0));
		}
		return g_variant_new("(ai)", &errors);
	}
	if (!strcmp(method, "AboutToShowGroup"))
		return g_variant_new(
			"(@ai@ai)",
			g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0),
			g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0));
	return g_variant_new("(b)", FALSE);
}

static void method_call(GDBusConnection *bus, const char *sender,
			const char *path, const char *iface, const char *method,
			GVariant *p, GDBusMethodInvocation *call, gpointer user)
{
	(void)bus;
	(void)sender;
	(void)path;
	(void)user;
	if (!strcmp(iface, MENU_IFACE)) {
		GVariant *out = menu_call(method, p);

		if (out)
			g_dbus_method_invocation_return_value(call, out);
		else
			g_dbus_method_invocation_return_error_literal(
				call, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"Unknown tray menu item or property.");
		return;
	}
	if (!strcmp(method, "Activate"))
		queue_action(TOGGLE);
	else if (!strcmp(method, "SecondaryActivate") ||
		 !strcmp(method, "ContextMenu"))
		queue_action(SHOW);
	g_dbus_method_invocation_return_value(call, NULL);
}

static const GDBusInterfaceVTable table = { .method_call = method_call,
					    .get_property = property };

static void check_host(struct host *h)
{
	h->visible = FALSE;
	if (h->registered) {
		g_autoptr(GVariant) args =
			g_variant_ref_sink(g_variant_new("(s)", h->interface));
		g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(
			h->proxy, "org.freedesktop.DBus.Properties.GetAll",
			args, G_DBUS_CALL_FLAGS_NO_AUTO_START, 800, tray.cancel,
			NULL);
		if (reply) {
			g_autoptr(GVariant) props =
				g_variant_get_child_value(reply, 0);
			gboolean has_host = FALSE;

			g_autoptr(GVariant) items = g_variant_lookup_value(
				props, "RegisteredStatusNotifierItems",
				G_VARIANT_TYPE_STRING_ARRAY);
			if (g_variant_lookup(props,
					     "IsStatusNotifierHostRegistered",
					     "b", &has_host) &&
			    has_host && items) {
				GVariantIter iter;
				const char *id;

				g_autofree char *full =
					g_strconcat(tray.name, ITEM_PATH, NULL);
				g_variant_iter_init(&iter, items);
				while (g_variant_iter_next(&iter, "&s", &id))
					if (!strcmp(id, full) ||
					    !strcmp(id, tray.name)) {
						h->visible = TRUE;
						break;
					}
			}
		}
	}
	availability();
}

static void registered(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autofree struct registration *job = data;

	g_autoptr(GVariant) reply =
		g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, NULL);
	tray.pending--;
	if (job->generation != job->host->generation ||
	    g_cancellable_is_cancelled(tray.cancel))
		return;
	job->host->registered = reply != NULL;
	check_host(job->host);
}

static void owner_changed(GObject *obj, GParamSpec *pspec, gpointer user)
{
	(void)obj;
	(void)pspec;
	struct host *h = user;

	h->generation++;
	h->registered = h->visible = FALSE;
	availability();
	g_autofree char *owner = g_dbus_proxy_get_name_owner(h->proxy);

	if (!owner || g_cancellable_is_cancelled(tray.cancel))
		return;
	struct registration *job = g_new(struct registration, 1);
	*job = (struct registration){ h, h->generation };
	tray.pending++;
	g_autoptr(GVariant) args =
		g_variant_ref_sink(g_variant_new("(s)", tray.name));
	g_dbus_proxy_call(h->proxy, "RegisterStatusNotifierItem", args,
			  G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, tray.cancel,
			  registered, job);
}

static void watcher_signal(GDBusProxy *proxy, const char *sender,
			   const char *signal, GVariant *args, gpointer user)
{
	(void)proxy;
	(void)sender;
	(void)args;
	if (g_str_has_prefix(signal, "StatusNotifierHost") ||
	    !strcmp(signal, "StatusNotifierItemUnregistered"))
		check_host(user);
}

static void watcher_properties(GDBusProxy *proxy, GVariant *changes,
			       const char *const *invalid, gpointer user)
{
	(void)proxy;
	(void)changes;
	(void)invalid;
	check_host(user);
}

static void lost_bus(GDBusConnection *bus, gboolean vanished, GError *error,
		     gpointer user)
{
	(void)bus;
	(void)vanished;
	(void)error;
	(void)user;
	for (guint i = 0; i < 2; i++)
		tray.hosts[i].visible = FALSE;
	availability();
}

static gpointer service(gpointer unused)
{
	(void)unused;
	g_main_context_push_thread_default(tray.context);
	g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
		tray.address,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, tray.cancel, NULL);
	guint ids[3] = { 0 };
	gulong closed = 0;
	const char *xml = "<node>" ITEM("org.kde.StatusNotifierItem")
		ITEM("org.freedesktop.StatusNotifierItem") MENU_XML "</node>";
	g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, NULL);
	g_autoptr(GVariant) name = NULL;
	g_autoptr(GVariant) request =
		g_variant_ref_sink(g_variant_new("(su)", tray.name, 4u));
	if (!bus || !info)
		goto out;
	g_dbus_connection_set_exit_on_close(bus, FALSE);
	closed = g_signal_connect(bus, "closed", G_CALLBACK(lost_bus), NULL);
	for (guint i = 0; i < 3; i++) {
		ids[i] = g_dbus_connection_register_object(
			bus, i == 2 ? MENU_PATH : ITEM_PATH,
			info->interfaces[i], &table, NULL, NULL, NULL);
		if (!ids[i])
			goto out;
	}
	name = g_dbus_connection_call_sync(
		bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "RequestName", request,
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000,
		tray.cancel, NULL);
	guint result = 0;

	if (name)
		g_variant_get(name, "(u)", &result);
	if (result != 1)
		goto out;
	for (guint i = 0; i < 2; i++) {
		struct host *h = &tray.hosts[i];

		h->interface = i ? "org.freedesktop.StatusNotifierWatcher" :
				   "org.kde.StatusNotifierWatcher";
		h->proxy = g_dbus_proxy_new_sync(
			bus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
			h->interface, WATCHER_PATH, h->interface, tray.cancel,
			NULL);
		if (!h->proxy)
			continue;
		g_signal_connect(h->proxy, "notify::g-name-owner",
				 G_CALLBACK(owner_changed), h);
		g_signal_connect(h->proxy, "g-signal",
				 G_CALLBACK(watcher_signal), h);
		g_signal_connect(h->proxy, "g-properties-changed",
				 G_CALLBACK(watcher_properties), h);
		owner_changed(NULL, NULL, h);
	}
	if (!g_cancellable_is_cancelled(tray.cancel))
		g_main_loop_run(tray.loop);
out:
	for (guint i = 0; i < 2; i++)
		if (tray.hosts[i].proxy)
			g_signal_handlers_disconnect_by_data(
				tray.hosts[i].proxy, &tray.hosts[i]);
	while (tray.pending)
		g_main_context_iteration(tray.context, TRUE);
	for (guint i = 0; i < 2; i++)
		g_clear_object(&tray.hosts[i].proxy);
	if (bus) {
		for (guint i = 0; i < 3; i++)
			if (ids[i])
				g_dbus_connection_unregister_object(bus,
								    ids[i]);
		if (closed)
			g_signal_handler_disconnect(bus, closed);
		g_dbus_connection_close_sync(bus, NULL, NULL);
	}
	g_atomic_int_set(&tray.available, FALSE);
	g_main_context_pop_thread_default(tray.context);
	return NULL;
}

void ui_tray_start(void)
{
	if (g_strcmp0(g_getenv("LINE_DISABLE_TRAY"), "1") == 0 || tray.thread)
		return;
	const char *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
	g_autofree char *socket = NULL;

	if (!address || !*address) {
		socket =
			g_build_filename(g_get_user_runtime_dir(), "bus", NULL);
		if (!g_file_test(socket, G_FILE_TEST_EXISTS))
			return;
		tray.address = g_strdup_printf("unix:path=%s", socket);
	} else
		tray.address = g_strdup(address);
	tray.event = SDL_RegisterEvents(1);
	if (tray.event == (Uint32)-1) {
		g_clear_pointer(&tray.address, g_free);
		return;
	}
	tray.name = g_strdup_printf("org.kde.StatusNotifierItem-%u-1",
				    (unsigned int)getpid());
	tray.context = g_main_context_new();
	tray.loop = g_main_loop_new(tray.context, FALSE);
	tray.cancel = g_cancellable_new();
	tray.thread = g_thread_try_new("line-tray", service, NULL, NULL);
	if (!tray.thread)
		ui_tray_stop();
}

static gboolean stop(gpointer unused)
{
	(void)unused;
	g_main_loop_quit(tray.loop);
	return G_SOURCE_REMOVE;
}

void ui_tray_stop(void)
{
	if (tray.thread) {
		g_cancellable_cancel(tray.cancel);
		g_main_context_invoke(tray.context, stop, NULL);
		g_thread_join(tray.thread);
		tray.thread = NULL;
	}
	g_clear_object(&tray.cancel);
	g_clear_pointer(&tray.icon, g_variant_unref);
	g_clear_pointer(&tray.loop, g_main_loop_unref);
	g_clear_pointer(&tray.context, g_main_context_unref);
	g_clear_pointer(&tray.address, g_free);
	g_clear_pointer(&tray.name, g_free);
	tray.event = 0;
	memset(tray.hosts, 0, sizeof(tray.hosts));
}

gboolean ui_tray_hide(SDL_Window *window)
{
	if (!g_atomic_int_get(&tray.available))
		return FALSE;
	SDL_HideWindow(window);
	return TRUE;
}

gboolean ui_tray_event(const SDL_Event *event, SDL_Window *window,
		       gboolean *quitting)
{
	if (!tray.event || event->type != tray.event)
		return FALSE;
	enum tray_action action = event->user.code;
	gboolean hidden = SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN;

	if (action == QUIT)
		*quitting = TRUE;
	else if (action == SHOW ||
		 (action == HOST_GONE && hidden &&
		  !g_atomic_int_get(&tray.available)) ||
		 (action == TOGGLE && hidden)) {
		SDL_ShowWindow(window);
		SDL_RestoreWindow(window);
		SDL_RaiseWindow(window);
	} else if (action == TOGGLE || action == HIDE)
		ui_tray_hide(window);
	return TRUE;
}
