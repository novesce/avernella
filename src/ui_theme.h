/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UI_THEME_H
#define UI_THEME_H

#include <glib.h>

/* Window surfaces, ordered from the rail through raised controls. */
#define UI_RAIL 0x100c13
#define UI_SIDEBAR 0x181019
#define UI_BG 0x19121c
#define UI_PANEL 0x231725
#define UI_DIALOG 0x2b1b2b
#define UI_INPUT 0x2d1e30
#define UI_RAISED 0x362238
#define UI_LINE 0x412e40

#define UI_HOVER 0x322031
enum ui_palette {
	UI_PALETTE_PINK,
	UI_PALETTE_BLUE,
	UI_PALETTE_MINT,
	UI_PALETTE_AMBER,
	UI_PALETTE_COUNT,
};

struct ui_palette_colors {
	guint accent, hover, ink, selected, selection;
};

extern const struct ui_palette_colors ui_palettes[UI_PALETTE_COUNT];
extern enum ui_palette ui_palette;

#define UI_SELECTED (ui_palettes[ui_palette].selected)
#define UI_TEXT_SELECTION (ui_palettes[ui_palette].selection)
#define UI_ACCENT (ui_palettes[ui_palette].accent)
#define UI_ACCENT_HOVER (ui_palettes[ui_palette].hover)
#define UI_ON_ACCENT (ui_palettes[ui_palette].ink)

#define UI_HEADING 0xfff5fa
#define UI_BODY 0xeadce6
#define UI_MUTED 0xc3a7b9
#define UI_SUBTLE 0xaf90a5

#define UI_DANGER 0xf23f43
#define UI_ONLINE 0x23a55a

/*
 * The LINE layout maps LINE's green onto the Avernella accent: sent bubbles,
 * unread counts and the active tab are hot pink, and the conversation sits on
 * a slightly raised plum so the list and the chat read as separate panes.
 */
#define UI_NAV_RAIL UI_RAIL
#define UI_NAV_INK UI_SUBTLE
#define UI_NAV_ACTIVE UI_ACCENT
#define UI_NAV_HOVER UI_HOVER
#define UI_CHAT_BG UI_PANEL
#define UI_BUBBLE_IN UI_RAISED
#define UI_BUBBLE_IN_INK UI_BODY
#define UI_BUBBLE_OUT UI_ACCENT
#define UI_BUBBLE_OUT_INK UI_ON_ACCENT

void ui_theme_init(void);
gboolean ui_theme_is_line(void);
gboolean ui_theme_toggle(GError **error);
gboolean ui_theme_set_palette(enum ui_palette palette, GError **error);

#endif
