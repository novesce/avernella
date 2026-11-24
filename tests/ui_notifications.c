// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_notifications.h"
#include <string.h>

/* A stranger's name reaches the body, so markup servers get it escaped. */
static void body_escaped(void)
{
	static const char name[] =
		"<a href=\"https://example.com/\">Verify</a> & <img src=x>";
	g_autofree char *escaped = ui_notification_text(name, TRUE);
	g_autofree char *plain = ui_notification_text(name, FALSE);

	g_assert_cmpstr(escaped, ==,
			"&lt;a href=&quot;https://example.com/&quot;&gt;"
			"Verify&lt;/a&gt; &amp; &lt;img src=x&gt;");
	g_assert_null(strchr(escaped, '<'));
	g_assert_cmpstr(plain, ==, name);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/notifications/body-escaped", body_escaped);
	return g_test_run();
}
