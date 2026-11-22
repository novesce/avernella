/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UI_LANG_TABLE_H
#define UI_LANG_TABLE_H

#include <glib.h>

/*
 * One translation. Each src/ui_lang_<tag>.c defines a table; the strings are
 * pairs of { English, translation }. Keep printf conversions in the English
 * order. A translation may leave out trailing ones, such as an English
 * plural suffix. Weekday arrays start on Monday, as GLib numbers them.
 */
struct ui_lang_table {
	const char *tag, *name;
	gboolean rtl;
	const char *months[12], *short_months[12];
	const char *days[7], *short_days[7];
	const char *const (*strings)[2];
	size_t count;
};

extern const struct ui_lang_table ui_lang_ja, ui_lang_ko, ui_lang_th,
	ui_lang_zh_hans, ui_lang_zh_hant, ui_lang_vi, ui_lang_id, ui_lang_ms,
	ui_lang_fr, ui_lang_de, ui_lang_it, ui_lang_es, ui_lang_pt, ui_lang_ru,
	ui_lang_ar, ui_lang_tr;

#endif
