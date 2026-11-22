// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_lang.h"
#include "ui_lang_table.h"
#include <errno.h>
#include <string.h>

/* In enum ui_language order; English has no table. */
static const struct ui_lang_table *const tables[UI_LANG_COUNT] = {
	[UI_LANG_JA] = &ui_lang_ja,
	[UI_LANG_KO] = &ui_lang_ko,
	[UI_LANG_TH] = &ui_lang_th,
	[UI_LANG_ZH_HANS] = &ui_lang_zh_hans,
	[UI_LANG_ZH_HANT] = &ui_lang_zh_hant,
	[UI_LANG_VI] = &ui_lang_vi,
	[UI_LANG_ID] = &ui_lang_id,
	[UI_LANG_MS] = &ui_lang_ms,
	[UI_LANG_FR] = &ui_lang_fr,
	[UI_LANG_DE] = &ui_lang_de,
	[UI_LANG_IT] = &ui_lang_it,
	[UI_LANG_ES] = &ui_lang_es,
	[UI_LANG_PT] = &ui_lang_pt,
	[UI_LANG_RU] = &ui_lang_ru,
	[UI_LANG_AR] = &ui_lang_ar,
	[UI_LANG_TR] = &ui_lang_tr,
};

static enum ui_language current;
static GHashTable *strings; /* English → translation, borrowed */

static char *lang_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "avernella",
				"language", NULL);
}

static const char *tag_of(enum ui_language lang)
{
	return tables[lang] ? tables[lang]->tag : "en";
}

static int from_tag(const char *tag)
{
	for (int i = 0; i < UI_LANG_COUNT; i++)
		if (!g_ascii_strcasecmp(tag, tag_of(i)))
			return i;
	return -1;
}

/*
 * Map a POSIX locale name such as "zh_TW.UTF-8" to a language. Chinese
 * picks its script from the region unless the locale names one.
 */
static int from_locale(const char *name)
{
	g_autofree char *base = g_strndup(name, strcspn(name, ".@"));
	char *region = strchr(base, '_');

	if (region)
		*region++ = 0;
	if (!g_ascii_strcasecmp(base, "zh")) {
		gboolean hant = strstr(name, "Hant") ||
				(region && (!strcmp(region, "TW") ||
					    !strcmp(region, "HK") ||
					    !strcmp(region, "MO")));

		return hant ? UI_LANG_ZH_HANT : UI_LANG_ZH_HANS;
	}
	/* Indonesian is also written "in" by older systems. */
	if (!g_ascii_strcasecmp(base, "in"))
		return UI_LANG_ID;
	if (!g_ascii_strcasecmp(base, "en"))
		return UI_LANG_EN;
	return from_tag(base);
}

static enum ui_language desktop_language(void)
{
	const char *const *names = g_get_language_names();

	for (int i = 0; names && names[i]; i++) {
		int lang = from_locale(names[i]);

		if (lang >= 0)
			return lang;
	}
	return UI_LANG_EN;
}

static void use(enum ui_language lang)
{
	current = lang;
	g_clear_pointer(&strings, g_hash_table_destroy);
}

void ui_lang_init(void)
{
	g_autofree char *path = lang_path();
	g_autofree char *saved = NULL;
	int lang = -1;

	if (g_file_get_contents(path, &saved, NULL, NULL))
		lang = from_tag(g_strstrip(saved));
	use(lang >= 0 ? (enum ui_language)lang : desktop_language());
}

enum ui_language ui_lang_current(void)
{
	return current;
}

gboolean ui_lang_set(enum ui_language lang, GError **error)
{
	g_autofree char *path = lang_path();
	g_autofree char *dir = g_path_get_dirname(path);
	g_autofree char *line = g_strconcat(tag_of(lang), "\n", NULL);

	g_return_val_if_fail(lang < UI_LANG_COUNT, FALSE);
	if (g_mkdir_with_parents(dir, 0700) < 0) {
		int err = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
			    "Cannot create the settings directory: %s",
			    g_strerror(err));
		return FALSE;
	}
	if (!g_file_set_contents_full(path, line, -1,
				      G_FILE_SET_CONTENTS_CONSISTENT, 0600,
				      error))
		return FALSE;
	use(lang);
	return TRUE;
}

const char *ui_lang_native_name(enum ui_language lang)
{
	return lang < UI_LANG_COUNT && tables[lang] ? tables[lang]->name :
						      "English";
}

const char *ui_lang_tag(void)
{
	return tag_of(current);
}

gboolean ui_lang_rtl(void)
{
	return tables[current] && tables[current]->rtl;
}

const char *ui_tr(const char *english)
{
	const struct ui_lang_table *t = tables[current];

	if (!t || !english)
		return english;
	if (!strings) {
		strings = g_hash_table_new(g_str_hash, g_str_equal);
		for (size_t i = 0; i < t->count; i++)
			g_hash_table_insert(strings, (gpointer)t->strings[i][0],
					    (gpointer)t->strings[i][1]);
	}
	const char *found = g_hash_table_lookup(strings, english);

	return found ? found : english;
}

char *ui_lang_date(GDateTime *when, const char *format)
{
	const struct ui_lang_table *t = tables[current];

	if (!t)
		return g_date_time_format(when, format);
	int month = g_date_time_get_month(when) - 1;
	int day = g_date_time_get_day_of_week(when) - 1;
	g_autoptr(GString) local = g_string_new(NULL);

	/* Spell out names here; GLib formats them from the C locale. */
	for (const char *p = format; *p; p++) {
		if (p[0] != '%' || !p[1]) {
			g_string_append_c(local, *p);
			continue;
		}
		const char *name = p[1] == 'B' ? t->months[month] :
				   p[1] == 'b' ? t->short_months[month] :
				   p[1] == 'A' ? t->days[day] :
				   p[1] == 'a' ? t->short_days[day] :
						 NULL;

		if (name)
			for (; *name; name++) {
				/* A literal % in a name must survive. */
				if (*name == '%')
					g_string_append_c(local, '%');
				g_string_append_c(local, *name);
			}
		else
			g_string_append_len(local, p, 2);
		p++;
	}
	return g_date_time_format(when, local->str);
}
