// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * avernella-liff: LINE's in-app browser for LIFF apps and official-account
 * links, on WPE WebKit or WebKitGTK. It is its own program so the client never maps
 * WebKit, and it runs only while its window is open.
 *
 * The UI starts it with one end of a SOCK_SEQPACKET socket on fd 3 (see
 * liff.h) and its own PID as argv[1]. Kernel facilities keep it cheap and
 * subordinate to the chat client:
 *   - a systemd transient scope (a cgroup v2 leaf) with MemoryHigh and a
 *     low CPUWeight and IOWeight, which WebKit's own memory pressure
 *     monitor also reads;
 *   - a higher oom_score_adj, so the kernel reclaims the browser before
 *     the client;
 *   - PR_SET_PDEATHSIG, so it never outlives the client;
 *   - a 1 ms timer slack, inherited by WebKit's processes, so page timers
 *     coalesce into fewer wakeups.
 */
#include "liff_url.h"
#ifdef LINE_LIFF_GTK
#include <webkit/webkit.h>
#else
#include <wpe/webkit.h>
#include <wpe/wayland/wpe-wayland.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define WORLD "avernella-liff"
/* Far above an idle chat client, below a whole desktop browser. */
#define SCOPE_MEMORY_HIGH (768ULL << 20)
#define WEB_MEMORY_LIMIT_MB 512

static struct {
	GMainLoop *loop;
	WebKitWebView *view;
#ifdef LINE_LIFF_GTK
	GtkWindow *window;
#else
	WPEDisplay *display;
#endif
	WebKitSettings *settings;
	GHashTable *dialogs; /* dialog ID -> WebKitScriptDialog */
	guint next_dialog;
	int fd;
	gboolean liff, debug;
} b = { .fd = LINE_LIFF_FD };

static void quit(void)
{
	if (b.loop)
		g_main_loop_quit(b.loop);
}

#ifdef LINE_LIFF_GTK
static gboolean on_window_close(GtkWindow *window, gpointer data)
{
	(void)window;
	(void)data;
	quit();
	return TRUE;
}
#endif

static void send_packet(char kind, const char *a, const char *c)
{
	g_autofree char *packet =
		c ? g_strdup_printf("%c\t%s\t%s", kind, a, c) :
		    g_strdup_printf("%c\t%s", kind, a);
	gsize n = strlen(packet);

	if (n >= LINE_LIFF_PACKET)
		return;
	while (send(b.fd, packet, n, MSG_NOSIGNAL) < 0 && errno == EINTR)
		;
}

/* The page's address without the tokens LINE put in its fragment. */
static char *shareable_uri(void)
{
	const char *uri = webkit_web_view_get_uri(b.view);

	if (!uri)
		return NULL;
	return b.liff ? g_strndup(uri, strcspn(uri, "#")) : g_strdup(uri);
}

/* A cgroup of our own: systemd moves this process, WebKit's join it. */
static void join_scope(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusConnection) bus =
		g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

	if (!bus)
		return;
	guint32 pid = (guint32)getpid();
	g_autofree char *name =
		g_strdup_printf("app-line_linux-liff-%u.scope", pid);
	GVariantBuilder props;

	g_variant_builder_init(&props, G_VARIANT_TYPE("a(sv)"));
	g_variant_builder_add(&props, "(sv)", "PIDs",
			      g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
							&pid, 1, sizeof(pid)));
	g_variant_builder_add(&props, "(sv)", "Description",
			      g_variant_new_string("Avernella in-app browser"));
	g_variant_builder_add(&props, "(sv)", "CollectMode",
			      g_variant_new_string("inactive-or-failed"));
	g_variant_builder_add(&props, "(sv)", "MemoryHigh",
			      g_variant_new_uint64(SCOPE_MEMORY_HIGH));
	g_variant_builder_add(&props, "(sv)", "CPUWeight",
			      g_variant_new_uint64(50));
	g_variant_builder_add(&props, "(sv)", "IOWeight",
			      g_variant_new_uint64(50));
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
		"org.freedesktop.systemd1.Manager", "StartTransientUnit",
		g_variant_new("(ssa(sv)a(sa(sv)))", name, "fail", &props, NULL),
		G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500,
		NULL, &error);

	if (!reply) {
		g_debug("No systemd scope: %s", error->message);
		return;
	}
	/*
	 * systemd moves us when its job runs. WebKit's processes inherit our
	 * cgroup at fork, so wait until the kernel reports the new one.
	 */
	g_autofree char *leaf = g_strconcat("/", name, "\n", NULL);

	for (int tries = 0; tries < 250; tries++) {
		g_autofree char *cgroup = NULL;

		if (g_file_get_contents("/proc/self/cgroup", &cgroup, NULL,
					NULL) &&
		    g_str_has_suffix(cgroup, leaf))
			return;
		g_usleep(1000);
	}
}

static void tune_process(pid_t parent)
{
	prctl(PR_SET_NAME, "avernella-liff");
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() != parent)
		_exit(0);
	prctl(PR_SET_TIMERSLACK, 1000000UL);
	int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, "500", 3) < 0)
			g_debug("oom_score_adj: %s", g_strerror(errno));
		close(fd);
	}
	join_scope();
}

static void resize(const char *size)
{
	int height = !strcmp(size, "compact") ? 480 :
		     !strcmp(size, "tall")    ? 740 :
		     !strcmp(size, "web")     ? 820 :
						860;
	int width = !strcmp(size, "web") ? 960 : 420;

#ifdef LINE_LIFF_GTK
	gtk_window_set_default_size(b.window, width, height);
	gtk_window_present(b.window);
#else
	WPEView *view = webkit_web_view_get_wpe_view(b.view);
	WPEToplevel *top = view ? wpe_view_get_toplevel(view) : NULL;

	if (top)
		wpe_toplevel_resize(top, width, height);
#endif
}

/* LINE_DEBUG_LIFF=1 also loads local test pages from 127.0.0.1. */
static gboolean debug_page(const char *url)
{
	return b.debug && g_str_has_prefix(url, "http://127.0.0.1:");
}

static void open_page(gboolean liff, const char *size, const char *url)
{
	if (g_ascii_strncasecmp(url, "https://", 8) && !debug_page(url))
		return;
	b.liff = liff;
	webkit_settings_set_user_agent(b.settings, liff ? LINE_LIFF_USER_AGENT :
							  LINE_WEB_USER_AGENT);
	resize(size);
	webkit_web_view_load_uri(b.view, url);
}

static void answer_dialog(const char *id, const char *answer)
{
	WebKitScriptDialog *dialog = g_hash_table_lookup(b.dialogs, id);

	if (!dialog)
		return;
	gboolean ok = !strcmp(answer, "1");

	switch (webkit_script_dialog_get_dialog_type(dialog)) {
	case WEBKIT_SCRIPT_DIALOG_CONFIRM:
	case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
		webkit_script_dialog_confirm_set_confirmed(dialog, ok);
		break;
	case WEBKIT_SCRIPT_DIALOG_PROMPT:
		if (ok)
			webkit_script_dialog_prompt_set_text(
				dialog,
				webkit_script_dialog_prompt_get_default_text(
					dialog));
		break;
	default:
		break;
	}
	webkit_script_dialog_close(dialog);
	g_hash_table_remove(b.dialogs, id);
}

static gboolean on_packet(int fd, GIOCondition condition, gpointer data)
{
	char packet[LINE_LIFF_PACKET];
	ssize_t n = recv(fd, packet, sizeof(packet) - 1, MSG_DONTWAIT);

	(void)data;
	if (n < 0 && (errno == EAGAIN || errno == EINTR) &&
	    !(condition & (G_IO_HUP | G_IO_ERR)))
		return G_SOURCE_CONTINUE;
	if (n <= 0) {
		/* The client closed or died. */
		quit();
		return G_SOURCE_REMOVE;
	}
	packet[n] = '\0';
	g_auto(GStrv) parts = g_strsplit(packet, "\t", 3);

	if (g_strv_length(parts) != 3)
		return G_SOURCE_CONTINUE;
	if (!strcmp(parts[0], "L") || !strcmp(parts[0], "W"))
		open_page(!strcmp(parts[0], "L"), parts[1], parts[2]);
	else if (!strcmp(parts[0], "R"))
		answer_dialog(parts[1], parts[2]);
	return G_SOURCE_CONTINUE;
}

static gboolean web_scheme(const char *uri)
{
	return !g_ascii_strncasecmp(uri, "https://", 8) ||
	       !g_ascii_strncasecmp(uri, "http://", 7) ||
	       !g_ascii_strncasecmp(uri, "about:", 6) ||
	       !g_ascii_strncasecmp(uri, "data:", 5) ||
	       !g_ascii_strncasecmp(uri, "blob:", 5);
}

static gboolean on_decide(WebKitWebView *view, WebKitPolicyDecision *decision,
			  WebKitPolicyDecisionType type, gpointer data)
{
	(void)data;
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
		return FALSE;
	WebKitNavigationAction *action =
		webkit_navigation_policy_decision_get_navigation_action(
			WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	const char *uri = webkit_uri_request_get_uri(
		webkit_navigation_action_get_request(action));
	gboolean gesture = webkit_navigation_action_is_user_gesture(action);

	if (!uri)
		return FALSE;
	/* The LIFF SDK closes its window with liff://close. */
	if (!g_ascii_strncasecmp(uri, "liff://", 7)) {
		if (!g_ascii_strncasecmp(uri, "liff://close", 12))
			quit();
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	/* Another LIFF app needs its own view from LINE: ask the client. */
	if (line_liff_url(uri, NULL, NULL) || line_liff_line_scheme(uri)) {
		send_packet('O', uri, NULL);
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	if (web_scheme(uri)) {
		if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
			return FALSE;
		/* One window, like LINE's: new windows replace the page. */
		webkit_policy_decision_ignore(decision);
		if (!g_ascii_strncasecmp(uri, "http", 4))
			webkit_web_view_load_uri(view, uri);
		return TRUE;
	}
	/* tel:, mailto: and app links go to the desktop, on a click only. */
	if (gesture)
		send_packet('X', uri, NULL);
	webkit_policy_decision_ignore(decision);
	return TRUE;
}

static gboolean on_dialog(WebKitWebView *view, WebKitScriptDialog *dialog,
			  gpointer data)
{
	(void)view;
	(void)data;
	g_autofree char *id = g_strdup_printf("%u", ++b.next_dialog);
	gboolean confirm = webkit_script_dialog_get_dialog_type(dialog) !=
			   WEBKIT_SCRIPT_DIALOG_ALERT;
	const char *message = webkit_script_dialog_get_message(dialog);
	g_autofree char *body = g_strdup_printf("%s\t%s", confirm ? "c" : "a",
						message ? message : "");

	g_hash_table_insert(b.dialogs, g_strdup(id),
			    webkit_script_dialog_ref(dialog));
	send_packet('D', id, body);
	return TRUE;
}

static gboolean on_permission(WebKitWebView *view,
			      WebKitPermissionRequest *request, gpointer data)
{
	(void)view;
	(void)data;
	/* Camera, location and notifications stay off, as LINE asks first. */
	webkit_permission_request_deny(request);
	return TRUE;
}

static void on_title(WebKitWebView *view, GParamSpec *spec, gpointer data)
{
	(void)spec;
	(void)data;
	const char *title = webkit_web_view_get_title(view);

#ifdef LINE_LIFF_GTK
	gtk_window_set_title(b.window, title && *title ? title : "Avernella");
#else
	WPEView *wpe = webkit_web_view_get_wpe_view(view);
	WPEToplevel *top = wpe ? wpe_view_get_toplevel(wpe) : NULL;

	if (top)
		wpe_toplevel_set_title(top, title && *title ? title : "Avernella");
#endif
}

static void on_crashed(WebKitWebView *view,
		       WebKitWebProcessTerminationReason reason, gpointer data)
{
	(void)data;
	/* Over the memory limit WebKit kills the page: show it again. */
	if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT)
		webkit_web_view_reload(view);
	else
		quit();
}

static void on_key(WebKitUserContentManager *manager, JSCValue *value,
		   gpointer data)
{
	(void)manager;
	(void)data;
	g_autofree char *key = jsc_value_to_string(value);

	if (!g_strcmp0(key, "close")) {
		quit();
	} else if (!g_strcmp0(key, "reload")) {
		webkit_web_view_reload(b.view);
	} else if (!g_strcmp0(key, "back")) {
		webkit_web_view_go_back(b.view);
	} else if (!g_strcmp0(key, "forward")) {
		webkit_web_view_go_forward(b.view);
	} else if (!g_strcmp0(key, "external")) {
		g_autofree char *uri = shareable_uri();

		if (uri && !g_ascii_strncasecmp(uri, "http", 4))
			send_packet('X', uri, NULL);
	} else if (!g_strcmp0(key, "zoom-in") || !g_strcmp0(key, "zoom-out") ||
		   !g_strcmp0(key, "zoom-reset")) {
		double zoom = webkit_web_view_get_zoom_level(b.view);

		zoom = !strcmp(key, "zoom-reset") ? 1.0 :
		       !strcmp(key, "zoom-in")	  ? MIN(zoom * 1.1, 3.0) :
						    MAX(zoom / 1.1, 0.3);
		webkit_web_view_set_zoom_level(b.view, zoom);
	}
}

/*
 * Window shortcuts, from an isolated script world: the page can neither
 * see nor call the message handler.
 */
static const char *const shortcuts[] = {
	"addEventListener('keydown', e => {",
	" const c = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();",
	" const a = c && k === 'w' ? 'close'",
	"  : (c && k === 'r') || k === 'f5' ? 'reload'",
	"  : e.altKey && k === 'arrowleft' ? 'back'",
	"  : e.altKey && k === 'arrowright' ? 'forward'",
	"  : c && e.shiftKey && k === 'o' ? 'external'",
	"  : c && (k === '=' || k === '+') ? 'zoom-in'",
	"  : c && k === '-' ? 'zoom-out'",
	"  : c && k === '0' ? 'zoom-reset' : null;",
	" if (!a) return;",
	" e.preventDefault(); e.stopImmediatePropagation();",
	" webkit.messageHandlers.keys.postMessage(a);",
	"}, true);",
	NULL,
};

/*
 * LINE's native bridge, in the page's own world. On LINE 9.5 and newer the
 * LIFF SDK's liff.init() waits for window._liff.features, then reaches
 * client features through window._liff.postMessage(action, featureToken,
 * callbackId, json) and waits for a "liffEvent" with the answer.
 */
static const char *const bridge[] = {
	"(() => {",
	" if (window._liff) return;",
	" const native = webkit.messageHandlers.liff;",
	" const post = (action, token, callback, json) => native.postMessage(",
	"  [String(action), String(callback || ''), String(json || '')]);",
	" Object.defineProperty(window, '_liff', { value: Object.freeze({",
	"  features: Object.freeze([]), postMessage: post }) });",
	"})();",
	NULL,
};

/* LIFF addresses contain tokens. Log only WebKit's load state. */
static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event,
			    gpointer data)
{
	(void)view;
	(void)data;
	switch (event) {
	case WEBKIT_LOAD_STARTED:
		g_printerr("avernella-liff: load started\n");
		break;
	case WEBKIT_LOAD_REDIRECTED:
		g_printerr("avernella-liff: load redirected\n");
		break;
	case WEBKIT_LOAD_COMMITTED:
		g_printerr("avernella-liff: load committed\n");
		break;
	case WEBKIT_LOAD_FINISHED:
		g_printerr("avernella-liff: load finished\n");
		break;
	}
}

/* Bridge names are SDK identifiers: openWindow, iap.createPayment. */
static gboolean bridge_name(const char *s)
{
	if (!s || strlen(s) > 64)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && !strchr("._-", *s))
			return FALSE;
	return TRUE;
}

static void bridge_reply(const char *action, const char *callback,
			 gboolean done)
{
	static const char refused[] =
		"error: { code: 'FORBIDDEN', message: 'Not available on Avernella.' }";
	static const char event[] =
		"window.dispatchEvent(new CustomEvent('liffEvent', { detail: { type: '%s', callbackId: '%s', %s } }));";
	g_autofree char *script = g_strdup_printf(event, action, callback,
						  done ? "data: {}" : refused);

	webkit_web_view_evaluate_javascript(b.view, script, -1, NULL, NULL,
					    NULL, NULL, NULL);
}

/* liff.openWindow({ url, external }) on LINE before 10.15. */
static gboolean bridge_open(JSCValue *value, const char *json)
{
	g_autoptr(JSCValue) params =
		jsc_value_new_from_json(jsc_value_get_context(value), json);
	g_autoptr(JSCValue) url_value =
		params && jsc_value_is_object(params) ?
			jsc_value_object_get_property(params, "url") :
			NULL;
	g_autoptr(JSCValue) external =
		params && jsc_value_is_object(params) ?
			jsc_value_object_get_property(params, "external") :
			NULL;
	g_autofree char *url = url_value && jsc_value_is_string(url_value) ?
				       jsc_value_to_string(url_value) :
				       NULL;

	if (!url || g_ascii_strncasecmp(url, "https://", 8))
		return FALSE;
	if (external && jsc_value_to_boolean(external))
		send_packet('X', url, NULL);
	else
		webkit_web_view_load_uri(b.view, url);
	return TRUE;
}

static void on_bridge(WebKitUserContentManager *manager, JSCValue *value,
		      gpointer data)
{
	(void)manager;
	(void)data;
	if (!jsc_value_is_array(value))
		return;
	g_autoptr(JSCValue) a =
		jsc_value_object_get_property_at_index(value, 0);
	g_autoptr(JSCValue) c =
		jsc_value_object_get_property_at_index(value, 1);
	g_autoptr(JSCValue) j =
		jsc_value_object_get_property_at_index(value, 2);
	g_autofree char *action = jsc_value_to_string(a);
	g_autofree char *callback = jsc_value_to_string(c);
	g_autofree char *json = jsc_value_to_string(j);

	if (!bridge_name(action) || !bridge_name(callback))
		return;
	if (!strcmp(action, "closeWindow")) {
		if (b.debug)
			g_printerr("avernella-liff: bridge closeWindow\n");
		quit();
		return;
	}
	gboolean open = !strcmp(action, "openWindow");
	gboolean done = open && bridge_open(value, json);

	if (b.debug)
		g_printerr("avernella-liff: bridge %s\n",
			   open ? "openWindow" : "unsupported");
	bridge_reply(action, callback, done);
}

static void add_script(WebKitUserContentManager *content,
		       const char *const *lines, const char *world)
{
	g_autofree char *source = g_strjoinv("\n", (char **)lines);

	g_autoptr(WebKitUserScript) script =
		world ? webkit_user_script_new_for_world(
				source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
				WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
				world, NULL, NULL) :
			webkit_user_script_new(
				source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
				WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
				NULL, NULL);

	webkit_user_content_manager_add_script(content, script);
}

static WebKitNetworkSession *network_session(void)
{
	/* Cookies and cache, carried over from kitatide or line-linux. */
	g_autofree char *data = line_liff_storage_dir(g_get_user_data_dir());
	g_autofree char *cache = line_liff_storage_dir(g_get_user_cache_dir());
	WebKitMemoryPressureSettings *memory =
		webkit_memory_pressure_settings_new();

	/* The network process trims its caches early under pressure. */
	webkit_memory_pressure_settings_set_memory_limit(memory, 128);
	webkit_network_session_set_memory_pressure_settings(memory);
	webkit_memory_pressure_settings_free(memory);
	return webkit_network_session_new(data, cache);
}

static WebKitWebView *create_view(void)
{
	WebKitMemoryPressureSettings *memory =
		webkit_memory_pressure_settings_new();

	webkit_memory_pressure_settings_set_memory_limit(memory,
							 WEB_MEMORY_LIMIT_MB);
	g_autoptr(WebKitWebContext) context =
		g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
			     "memory-pressure-settings", memory, NULL);

	webkit_memory_pressure_settings_free(memory);
	/* A small memory cache: LIFF pages are visited, not browsed. */
	webkit_web_context_set_cache_model(context,
					   WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
	g_autoptr(WebKitNetworkSession) session = network_session();
	g_autoptr(WebKitUserContentManager) content =
		webkit_user_content_manager_new();

	g_signal_connect(content, "script-message-received::keys",
			 G_CALLBACK(on_key), NULL);
	webkit_user_content_manager_register_script_message_handler(
		content, "keys", WORLD);
	add_script(content, shortcuts, WORLD);
	/* The bridge lives in the page's world, where the LIFF SDK runs. */
	g_signal_connect(content, "script-message-received::liff",
			 G_CALLBACK(on_bridge), NULL);
	webkit_user_content_manager_register_script_message_handler(
		content, "liff", NULL);
	add_script(content, bridge, NULL);
	b.settings = webkit_settings_new_with_settings(
		"user-agent", LINE_LIFF_USER_AGENT, "enable-developer-extras",
		FALSE, "enable-page-cache", FALSE,
		"media-playback-requires-user-gesture", TRUE,
		"javascript-can-open-windows-automatically", FALSE,
		"enable-write-console-messages-to-stdout", FALSE, NULL);
	WebKitWebView *view =
		g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context,
			     "network-session", session, "settings", b.settings,
#ifndef LINE_LIFF_GTK
			     "display", b.display,
#endif
			     "user-content-manager", content, NULL);

	if (b.debug)
		g_signal_connect(view, "load-changed",
				 G_CALLBACK(on_load_changed), NULL);
	g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide), NULL);
	g_signal_connect(view, "script-dialog", G_CALLBACK(on_dialog), NULL);
	g_signal_connect(view, "permission-request", G_CALLBACK(on_permission),
			 NULL);
	g_signal_connect(view, "notify::title", G_CALLBACK(on_title), NULL);
	g_signal_connect(view, "close", G_CALLBACK(quit), NULL);
	g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_crashed),
			 NULL);
	return view;
}

int main(int argc, char **argv)
{
	if (argc != 2 || fcntl(LINE_LIFF_FD, F_GETFD) < 0) {
		g_printerr("avernella-liff is started by avernella.\n");
		return 2;
	}
	char *end;
	long parent = strtol(argv[1], &end, 10);

	if (*end || parent <= 1)
		return 2;
	tune_process((pid_t)parent);
	b.debug = !g_strcmp0(g_getenv("LINE_DEBUG_LIFF"), "1");
	fcntl(LINE_LIFF_FD, F_SETFD, FD_CLOEXEC);
#ifdef LINE_LIFF_GTK
	if (!gtk_init_check()) {
		g_printerr(
			"avernella-liff: no GTK display for the in-app browser.\n");
		return 1;
	}
	b.window = GTK_WINDOW(gtk_window_new());
	g_signal_connect(b.window, "close-request", G_CALLBACK(on_window_close),
			 NULL);
#else
	g_autoptr(WPEDisplay) display = NULL;

	if (b.debug && !g_strcmp0(g_getenv("WPE_PLATFORM"), "headless")) {
		b.display = wpe_display_get_default();
	} else {
		g_autoptr(GError) error = NULL;

		display = wpe_display_wayland_new();
		if (!wpe_display_connect(display, &error)) {
			g_printerr("avernella-liff: %s\n", error->message);
			return 1;
		}
		b.display = display;
	}
#endif
	b.dialogs = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)webkit_script_dialog_unref);
	b.loop = g_main_loop_new(NULL, FALSE);
	b.view = create_view();
#ifdef LINE_LIFF_GTK
	g_object_ref_sink(b.view);
	gtk_window_set_child(b.window, GTK_WIDGET(b.view));
#else
	WPEView *wpe = webkit_web_view_get_wpe_view(b.view);

	if (!wpe) {
		g_printerr("avernella-liff: no display for the in-app browser.\n");
		return 1;
	}
	g_signal_connect(wpe, "closed", G_CALLBACK(quit), NULL);
	wpe_view_set_visible(wpe, TRUE);
#endif
	on_title(b.view, NULL, NULL);
	g_unix_fd_add(LINE_LIFF_FD, G_IO_IN | G_IO_HUP | G_IO_ERR, on_packet,
		      NULL);
	send_packet('B', "ready", NULL);
	g_main_loop_run(b.loop);
#ifdef LINE_LIFF_GTK
	gtk_window_destroy(b.window);
#endif
	g_hash_table_destroy(b.dialogs);
	g_object_unref(b.view);
	g_object_unref(b.settings);
	g_main_loop_unref(b.loop);
	return 0;
}
