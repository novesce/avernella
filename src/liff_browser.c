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
#include "security/liff_filter.h"
#include "security/sandbox.h"
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define WORLD "avernella-liff"
/* Far above an idle chat client, below a whole desktop browser. */
#define SCOPE_MEMORY_HIGH (768ULL << 20)
#define WEB_MEMORY_LIMIT_MB 512
/* The pause before the one reload after WebKit kills a page for memory. */
#define CRASH_RETRY_SECONDS 3

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
	const char *account; /* SHA-256 profile name, or NULL */
	gboolean liff, debug;
	char *origin; /* Authorized endpoint for the current LIFF view. */
	guint launch;
	WebKitUserScript *fragment;
	guint crashes, retry; /* memory kills of this page, pending reload */
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

/* A token-bearing LIFF fragment must never accompany a native handoff. */
static void send_link(char kind, const char *uri)
{
	g_autofree char *clean = b.liff ? g_strndup(uri, strcspn(uri, "#")) :
					  NULL;

	send_packet(kind, clean ? clean : uri, NULL);
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

static gboolean control_peer(pid_t parent)
{
	struct ucred peer;
	socklen_t size = sizeof(peer);
	int type, domain;
	socklen_t int_size = sizeof(int);

	return !getsockopt(b.fd, SOL_SOCKET, SO_TYPE, &type, &int_size) &&
	       type == SOCK_SEQPACKET &&
	       !getsockopt(b.fd, SOL_SOCKET, SO_DOMAIN, &domain, &int_size) &&
	       domain == AF_UNIX &&
	       !getsockopt(b.fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) &&
	       size == sizeof(peer) && peer.pid == parent &&
	       peer.uid == geteuid();
}

/* Set restrictions before GLib or WebKit create any threads or children. */
static int confine_browser(void)
{
	const struct rlimit no_core = { 0, 0 };
	const struct avernella_sandbox_opts opts = { .role = SANDBOX_ROLE_LIFF };
	uint32_t active = 0;
	int err;

	if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) < 0 ||
	    setrlimit(RLIMIT_CORE, &no_core) < 0 ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		return -errno;
	err = line_liff_filter_apply();
	if (err < 0)
		return err;

	err = avernella_landlock_apply(&opts, &active);
	if (!g_strcmp0(g_getenv("LINE_DEBUG_SANDBOX"), "1"))
		g_printerr(
			"line-sandbox liff no-dump=1 no-new-privs=1 seccomp=1 landlock-net=%d landlock-signal=%d status=%d\n",
			!!(active & AVSB_ACTIVE_LANDLOCK_NET),
			!!(active & AVSB_ACTIVE_LANDLOCK_SCOPE), err);
	return err;
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

static void open_page(gboolean liff, const char *size, const char *url);

/* Compare origins using the same URL parser as the WebView. */
static char *http_origin(const char *uri, char **pattern)
{
	if (!uri || (g_ascii_strncasecmp(uri, "https://", 8) &&
		     !(b.debug && line_liff_debug_url(uri))))
		return NULL;
	g_autoptr(WebKitSecurityOrigin) origin =
		webkit_security_origin_new_for_uri(uri);

	const char *host = origin ? webkit_security_origin_get_host(origin) :
				    NULL;

	if (!host || !*host || strchr(host, '*'))
		return NULL;
	/* WebKit's host patterns omit the port; the script checks it too. */
	if (pattern)
		*pattern = g_strdup_printf(
			"%s://%s/*",
			webkit_security_origin_get_protocol(origin), host);
	return webkit_security_origin_to_string(origin);
}

static gboolean app_origin(const char *uri)
{
	g_autofree char *origin = http_origin(uri, NULL);

	return b.origin && !g_strcmp0(origin, b.origin);
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

static gboolean ignore_outside_app(WebKitPolicyDecision *decision)
{
	if (b.debug)
		g_printerr("avernella-liff: outside origin blocked\n");
	webkit_policy_decision_ignore(decision);
	return TRUE;
}

static gboolean on_decide(WebKitWebView *view, WebKitPolicyDecision *decision,
			  WebKitPolicyDecisionType type, gpointer data)
{
	(void)data;
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
		WebKitResponsePolicyDecision *response =
			WEBKIT_RESPONSE_POLICY_DECISION(decision);

		/* Check the final document, including after HTTP redirects. */
		if (b.liff &&
		    webkit_response_policy_decision_is_main_frame_main_resource(
			    response) &&
		    !app_origin(webkit_uri_response_get_uri(
			    webkit_response_policy_decision_get_response(
				    response))))
			return ignore_outside_app(decision);
		return FALSE;
	}
	WebKitNavigationAction *action =
		webkit_navigation_policy_decision_get_navigation_action(
			WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	const char *uri = webkit_uri_request_get_uri(
		webkit_navigation_action_get_request(action));
	gboolean gesture = webkit_navigation_action_is_user_gesture(action) &&
			   !webkit_navigation_action_is_redirect(action);

	if (!uri)
		return FALSE;
	/* The LIFF SDK closes its window with liff://close. */
	if (!g_ascii_strncasecmp(uri, "liff://", 7)) {
		if (b.liff && gesture &&
		    !g_ascii_strcasecmp(uri, "liff://close"))
			quit();
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	/* Another LIFF app needs its own view from LINE: ask the client. */
	if (line_liff_url(uri, NULL, NULL) || line_liff_line_scheme(uri)) {
		if (gesture &&
		    webkit_navigation_action_get_navigation_type(action) ==
			    WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
			send_link('O', uri);
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	/*
	 * Links and redirects alike: the page stays on https, so nothing on
	 * the network can rewrite what runs beside the user's LINE identity.
	 */
	if (line_liff_web_url(uri, b.debug)) {
		if (b.liff && !app_origin(uri)) {
			/* A clicked outside link can leave without its fragment. */
			if (gesture && !g_ascii_strncasecmp(uri, "https://", 8))
				send_link('X', uri);
			return ignore_outside_app(decision);
		}
		if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
			return FALSE;
		/* One window, like LINE's: new windows replace the page. */
		webkit_policy_decision_ignore(decision);
		if (!g_ascii_strncasecmp(uri, "http", 4))
			webkit_web_view_load_uri(view, uri);
		return TRUE;
	}
	/* Plain http, tel:, mailto: and app links go to the desktop, on a
	 * click only.
	 */
	if (gesture)
		send_link('X', uri);
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
		wpe_toplevel_set_title(top,
				       title && *title ? title : "Avernella");
#endif
}

static gboolean retry_page(gpointer data)
{
	(void)data;
	b.retry = 0;
	webkit_web_view_reload(b.view);
	return G_SOURCE_REMOVE;
}

static void on_crashed(WebKitWebView *view,
		       WebKitWebProcessTerminationReason reason, gpointer data)
{
	(void)view;
	(void)data;
	/*
	 * Over the memory limit WebKit kills the page. Show it again once,
	 * after a pause; a page that outgrows the limit again closes the
	 * browser rather than loading and crashing forever.
	 */
	if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT &&
	    !b.crashes++) {
		g_clear_handle_id(&b.retry, g_source_remove);
		b.retry = g_timeout_add_seconds(CRASH_RETRY_SECONDS, retry_page,
						NULL);
		return;
	}
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
		g_clear_handle_id(&b.retry, g_source_remove);
		webkit_web_view_reload(b.view);
	} else if (!g_strcmp0(key, "back")) {
		webkit_web_view_go_back(b.view);
	} else if (!g_strcmp0(key, "forward")) {
		webkit_web_view_go_forward(b.view);
	} else if (!g_strcmp0(key, "external")) {
		const char *uri = webkit_web_view_get_uri(b.view);

		if (uri && !g_ascii_strncasecmp(uri, "http", 4))
			send_link('X', uri);
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
	" if (!e.isTrusted) return;",
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
static const char bridge[] =
	"if (window._liff) return;"
	"const post = (action, token, callback, json) => window.postMessage(\n"
	"{avernellaLiff: [String(action), String(callback || ''), String(json || '')]},\n"
	"location.origin);"
	"Object.defineProperty(window, '_liff', { value: Object.freeze({"
	"features: Object.freeze([]), postMessage: post }) });";

/* Only the top document's own window can address this isolated world. */
static const char relay[] =
	"addEventListener('message', e => {"
	"const m = e.data?.avernellaLiff;"
	"if (!e.isTrusted || e.source !== window || e.origin !== origin ||\n"
	"!Array.isArray(m) || m.length !== 3 ||\n"
	"!m.every(v => typeof v === 'string')) return;"
	"webkit.messageHandlers.liff.postMessage([launch, ...m]); });";

/* LIFF addresses contain tokens. Log only WebKit's load state. */
static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event,
			    gpointer data)
{
	(void)data;
	if (event == WEBKIT_LOAD_COMMITTED && b.fragment) {
		webkit_user_content_manager_remove_script(
			webkit_web_view_get_user_content_manager(view),
			b.fragment);
		g_clear_pointer(&b.fragment, webkit_user_script_unref);
	}
	if (!b.debug)
		return;
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
	if ((external && jsc_value_to_boolean(external)) || !app_origin(url))
		send_link('X', url);
	else
		webkit_web_view_load_uri(b.view, url);
	return TRUE;
}

static void on_bridge(WebKitUserContentManager *manager, JSCValue *value,
		      gpointer data)
{
	(void)manager;
	(void)data;
	if (!b.liff || !jsc_value_is_array(value))
		return;
	g_autoptr(JSCValue) launch =
		jsc_value_object_get_property_at_index(value, 0);

	if (!jsc_value_is_number(launch) ||
	    jsc_value_to_double(launch) != b.launch)
		return;
	g_autoptr(JSCValue) a =
		jsc_value_object_get_property_at_index(value, 1);
	g_autoptr(JSCValue) c =
		jsc_value_object_get_property_at_index(value, 2);
	g_autoptr(JSCValue) j =
		jsc_value_object_get_property_at_index(value, 3);
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

static WebKitUserScript *add_script(WebKitUserContentManager *content,
				    const char *source, const char *world,
				    const char *pattern)
{
	const char *const allow[] = { pattern, NULL };
	WebKitUserScript *script;

	if (world)
		script = webkit_user_script_new_for_world(
			source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, world,
			pattern ? allow : NULL, NULL);
	else
		script = webkit_user_script_new(
			source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
			pattern ? allow : NULL, NULL);

	webkit_user_content_manager_add_script(content, script);
	return script;
}

static void open_page(gboolean liff, const char *size, const char *url)
{
	g_autofree char *pattern = NULL;
	g_autofree char *origin = http_origin(url, liff ? &pattern : NULL);
	WebKitUserContentManager *content =
		webkit_web_view_get_user_content_manager(b.view);

	if (!origin)
		return;
	g_clear_handle_id(&b.retry, g_source_remove);
	b.crashes = 0;
	b.liff = liff;
	b.launch++;
	g_clear_pointer(&b.origin, g_free);
	webkit_user_content_manager_remove_all_scripts(content);
	g_clear_pointer(&b.fragment, webkit_user_script_unref);
	g_autofree char *keys = g_strjoinv("\n", (char **)shortcuts);

	webkit_user_script_unref(add_script(content, keys, WORLD, NULL));
	g_autofree char *address = NULL;

	if (liff) {
		b.origin = g_steal_pointer(&origin);
		g_autofree char *escaped =
			g_uri_escape_string(b.origin, NULL, FALSE);
		g_autofree char *guard = g_strdup_printf(
			"(() => { const origin = decodeURIComponent('%s');"
			"if (self.origin !== origin || location.origin !== origin) return;",
			escaped);
		g_autofree char *sdk =
			g_strconcat(guard, bridge, "})();", NULL);
		g_autofree char *native =
			g_strdup_printf("%s const launch = %u; %s })();", guard,
					b.launch, relay);
		const char *hash = strchr(url, '#');

		/*
		 * Send WebKit a fragment-free request. Only the authorized
		 * document receives the SDK fragment, before page scripts run.
		 */
		if (hash) {
			g_autofree char *value =
				g_uri_escape_string(hash, NULL, FALSE);
			g_autofree char *seed = g_strdup_printf(
				"%s if (!location.hash) history.replaceState(history.state,\n"
				"'', location.href.split('#')[0] + decodeURIComponent('%s'));"
				"})();",
				guard, value);

			b.fragment = add_script(content, seed, WORLD, pattern);
			address = g_strndup(url, hash - url);
		}
		webkit_user_script_unref(
			add_script(content, native, WORLD, pattern));
		webkit_user_script_unref(
			add_script(content, sdk, NULL, pattern));
	}
	webkit_settings_set_user_agent(b.settings, liff ? LINE_LIFF_USER_AGENT :
							  LINE_WEB_USER_AGENT);
	resize(size);
	webkit_web_view_load_uri(b.view, address ? address : url);
}

static WebKitNetworkSession *network_session(void)
{
	g_autofree char *data =
		line_liff_storage_dir(g_get_user_data_dir(), b.account);
	g_autofree char *cache =
		line_liff_storage_dir(g_get_user_cache_dir(), b.account);
	WebKitMemoryPressureSettings *memory =
		webkit_memory_pressure_settings_new();

	/* The network process trims its caches early under pressure. */
	webkit_memory_pressure_settings_set_memory_limit(memory, 128);
	webkit_network_session_set_memory_pressure_settings(memory);
	webkit_memory_pressure_settings_free(memory);
	if (!data || !cache) {
		/* Never store cookies where someone else could read them. */
		g_printerr("avernella-liff: using an ephemeral profile\n");
		return webkit_network_session_new_ephemeral();
	}
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
	/* The native endpoint is never visible in the page script world. */
	g_signal_connect(content, "script-message-received::liff",
			 G_CALLBACK(on_bridge), NULL);
	webkit_user_content_manager_register_script_message_handler(
		content, "liff", WORLD);
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

	g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed),
			 NULL);
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
	if (argc != 2 && argc != 3) {
		g_printerr("avernella-liff is started by avernella.\n");
		return 2;
	}
	char *end;

	errno = 0;
	long parent = strtol(argv[1], &end, 10);

	if (errno || *end || parent <= 1 || (pid_t)parent != parent ||
	    getppid() != parent || !control_peer((pid_t)parent))
		return 2;
	if (fcntl(b.fd, F_SETFD, FD_CLOEXEC) < 0)
		return 1;
	int err = confine_browser();

	if (err < 0) {
		g_printerr("avernella-liff: Linux isolation failed (%s).\n",
			   g_strerror(-err));
		return 1;
	}
	b.account = argc == 3 ? argv[2] : NULL;
	tune_process((pid_t)parent);
	b.debug = !g_strcmp0(g_getenv("LINE_DEBUG_LIFF"), "1");
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
		g_printerr(
			"avernella-liff: no display for the in-app browser.\n");
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
