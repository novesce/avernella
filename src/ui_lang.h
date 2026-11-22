/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UI_LANG_H
#define UI_LANG_H

#include <glib.h>

/*
 * Interface language. English strings in the sources are the keys: _()
 * returns the chosen language's text when it has a translation, and the
 * English text otherwise. N_() marks strings in static tables, which are
 * translated with _() where they are drawn.
 *
 * To add a string, wrap the English in _() and add a { English,
 * translation } pair to every src/ui_lang_<tag>.c. A missing pair falls
 * back to English. tests/ui_lang.c checks that the tables keep each
 * string's printf conversions.
 */
enum ui_language {
	UI_LANG_EN,
	UI_LANG_JA,
	UI_LANG_KO,
	UI_LANG_TH,
	UI_LANG_ZH_HANS,
	UI_LANG_ZH_HANT,
	UI_LANG_VI,
	UI_LANG_ID,
	UI_LANG_MS,
	UI_LANG_FR,
	UI_LANG_DE,
	UI_LANG_IT,
	UI_LANG_ES,
	UI_LANG_PT,
	UI_LANG_RU,
	UI_LANG_AR,
	UI_LANG_TR,
	UI_LANG_COUNT
};

void ui_lang_init(void);
enum ui_language ui_lang_current(void);
gboolean ui_lang_set(enum ui_language lang, GError **error);
/* The language's name in itself, such as "日本語". */
const char *ui_lang_native_name(enum ui_language lang);
/* A BCP 47 tag for font selection, such as "zh-Hant". */
const char *ui_lang_tag(void);
gboolean ui_lang_rtl(void);
const char *ui_tr(const char *english) G_GNUC_FORMAT(1);
/* g_date_time_format with month and weekday names in the chosen language. */
char *ui_lang_date(GDateTime *when, const char *format);

#define _(s) ui_tr(s)
#define N_(s) (s)

#endif
