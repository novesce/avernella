// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_lang.h"
#include "ui_lang_table.h"
#include <string.h>

static const struct ui_lang_table *const all[] = {
	&ui_lang_ja,	  &ui_lang_ko, &ui_lang_th, &ui_lang_zh_hans,
	&ui_lang_zh_hant, &ui_lang_vi, &ui_lang_id, &ui_lang_ms,
	&ui_lang_fr,	  &ui_lang_de, &ui_lang_it, &ui_lang_es,
	&ui_lang_pt,	  &ui_lang_ru, &ui_lang_ar, &ui_lang_tr,
};

/* The printf conversions of s, in order, skipping date fields. */
static GString *conversions(const char *s)
{
	GString *out = g_string_new(NULL);

	for (const char *p = strchr(s, '%'); p; p = strchr(p + 1, '%')) {
		const char *c = p + 1;

		while (*c == '-' || g_ascii_isdigit(*c))
			c++;
		if (strchr("sdu", *c))
			g_string_append_c(out, *c);
		if (*c == '%')
			p = c;
	}
	return out;
}

/* Every translation is complete, unique and keeps the English arguments. */
static void tables(void)
{
	size_t expected = all[0]->count;

	for (guint t = 0; t < G_N_ELEMENTS(all); t++) {
		const struct ui_lang_table *lang = all[t];
		g_autoptr(GHashTable) seen =
			g_hash_table_new(g_str_hash, g_str_equal);

		g_test_message("%s: %zu strings", lang->tag, lang->count);
		g_assert_true(g_utf8_validate(lang->name, -1, NULL));
		for (int i = 0; i < 12; i++)
			g_assert_true(lang->months[i] && lang->short_months[i]);
		for (int i = 0; i < 7; i++)
			g_assert_true(lang->days[i] && lang->short_days[i]);
		/* A table missing many keys was generated from a stale list. */
		g_assert_cmpuint(lang->count + 40, >, expected);
		for (size_t i = 0; i < lang->count; i++) {
			const char *en = lang->strings[i][0],
				   *tr = lang->strings[i][1];
			g_autoptr(GString) want = conversions(en);
			g_autoptr(GString) got = conversions(tr);

			g_assert_true(g_utf8_validate(tr, -1, NULL));
			g_assert_cmpstr(tr, !=, "");
			g_assert_true(g_hash_table_add(seen, (gpointer)en));
			/* A translation may drop only trailing arguments. */
			if (!strstr(en, "%H") && !strstr(en, "%B") &&
			    !strstr(en, "%Y") && !strstr(en, "%-m") &&
			    !g_str_has_prefix(want->str, got->str))
				g_error("%s: \"%s\" -> \"%s\"", lang->tag, en,
					tr);
		}
	}
}

static void preference(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree char *path = g_build_filename(g_get_user_config_dir(),
						 "avernella", "language", NULL);

	/* Without a saved choice, follow the desktop: Taiwan → Traditional. */
	g_setenv("LANGUAGE", "zh_TW", TRUE);
	ui_lang_init();
	g_assert_cmpint(ui_lang_current(), ==, UI_LANG_ZH_HANT);
	g_assert_cmpstr(ui_lang_tag(), ==, "zh-Hant");

	for (int i = 0; i < UI_LANG_COUNT; i++) {
		g_assert_true(ui_lang_set(i, &error));
		g_assert_no_error(error);
		ui_lang_init();
		g_assert_cmpint(ui_lang_current(), ==, i);
		if (i == UI_LANG_EN)
			g_assert_cmpstr(_("Send"), ==, "Send");
		else
			g_assert_cmpstr(_("Send"), !=, "Send");
		g_assert_true(ui_lang_rtl() == (i == UI_LANG_AR));
		/* Strings with no translation pass through. */
		g_assert_cmpstr(_("Grace Hopper"), ==, "Grace Hopper");
	}

	g_autoptr(GDateTime) when = g_date_time_new_utc(2026, 11, 20, 9, 5, 0);
	g_autofree char *thai = NULL, *japanese = NULL, *english = NULL;

	g_assert_true(ui_lang_set(UI_LANG_TH, &error));
	thai = ui_lang_date(when, _("%B %e, %Y"));
	g_assert_cmpstr(thai, ==, "20 พฤศจิกายน 2026");
	g_assert_true(ui_lang_set(UI_LANG_JA, &error));
	japanese = ui_lang_date(when, _("%B %e, %Y"));
	g_assert_cmpstr(japanese, ==, "2026年11月20日");
	g_assert_true(ui_lang_set(UI_LANG_EN, &error));
	english = ui_lang_date(when, _("%B %e, %Y"));
	g_assert_cmpstr(english, ==, "November 20, 2026");

	/* An unreadable choice falls back to the desktop language. */
	g_assert_true(g_file_set_contents(path, "xx\n", -1, &error));
	ui_lang_init();
	g_assert_cmpint(ui_lang_current(), ==, UI_LANG_ZH_HANT);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui/lang/tables", tables);
	g_test_add_func("/ui/lang/preference", preference);
	return g_test_run();
}
