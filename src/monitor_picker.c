// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor_picker.h"
#include <string.h>

/* Only the index of an explicitly confirmed monitor crosses back. The
 * worker keeps the connector names from this particular display snapshot.
 */
char *line_pick_share_monitor(const char *const *connectors,
			      const char *const *labels, guint count,
			      GCancellable *cancel, GError **error)
{
	const char *argv[LINE_MONITOR_LIMIT + 3] = {
		"/proc/self/exe",
		"--pick-share-monitor",
	};
	g_auto(GStrv) names = NULL;
	g_autoptr(GSubprocess) picker = NULL;

	if (g_cancellable_set_error_if_cancelled(cancel, error))
		return NULL;
	if (!count || count > LINE_MONITOR_LIMIT || !connectors || !labels)
		goto unavailable;
	names = g_new0(char *, count + 1);
	for (guint i = 0; i < count; i++) {
		const char *label = labels[i];

		if (!connectors[i] || !*connectors[i] ||
		    !g_utf8_validate(connectors[i], -1, NULL))
			goto unavailable;
		if (label && *label && strcmp(label, connectors[i]) &&
		    g_utf8_validate(label, -1, NULL))
			names[i] = g_strdup_printf("%s · %s", connectors[i],
						   label);
		else
			names[i] = g_strdup(connectors[i]);
		argv[i + 2] = names[i];
	}
	picker = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
				   error);

	if (!picker)
		return NULL;
	if (!g_subprocess_wait(picker, cancel, error)) {
		/* GSubprocess reaps the child when it exits. */
		g_subprocess_force_exit(picker);
		return NULL;
	}
	if (g_cancellable_set_error_if_cancelled(cancel, error))
		return NULL;
	if (g_subprocess_get_if_exited(picker)) {
		int status = g_subprocess_get_exit_status(picker);

		if (status >= LINE_MONITOR_SELECTED &&
		    (guint)(status - LINE_MONITOR_SELECTED) < count)
			return g_strdup(
				connectors[status - LINE_MONITOR_SELECTED]);
		if (!status) {
			g_set_error_literal(error, G_IO_ERROR,
					    G_IO_ERROR_CANCELLED,
					    "Screen sharing was cancelled.");
			return NULL;
		}
	}
unavailable:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "Could not open the monitor picker.");
	return NULL;
}
