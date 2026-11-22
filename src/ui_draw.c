// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "ui_gpu.h"
#include "ui_logo.h"
#include "line.h"
#include "ui_chat_lists.h"
#include "ui_emoji.h"
#include "ui_friends.h"
#include "ui_flex.h"
#include "ui_find.h"
#include "ui_groups.h"
#include "ui_gpu_scroll.h"
#include "ui_motion.h"
#include "ui_theme.h"
#include "ui_profile.h"
#include "flex_asset.h"
#include <math.h>
#include <string.h>
#include <time.h>

#define WHITE 0xffffff

static cairo_t *cr;
static cairo_surface_t *logo;
static cairo_surface_t *welcome_art;
static Rect sign_card, sign_heading, sign_portrait;
static PangoContext *context;
static gboolean composited_text;
static SDL_Texture *canvas_texture;
static Uint32 canvas_format;
static gboolean log_renderer, native_repaint;
static double video_offset;

/* Circular history rows use the same texture as the rest of the canvas. */
static struct {
	SDL_Rect area;
	int origin, offset, hover, scale;
	gboolean reusable;
} history_scroll;
static struct {
	SDL_Rect bands[4];
	int count;
} history_damage;

static void color(guint rgb)
{
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.,
			     ((rgb >> 8) & 255) / 255., (rgb & 255) / 255.);
}

static void path(double x, double y, double w, double h, double r)
{
	r = MIN(r, MIN(w, h) / 2);
	if (r == 0.) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
	cairo_close_path(cr);
}

static void box(double x, double y, double w, double h, double r, guint rgb)
{
	if (w < 1 || h < 1)
		return;
	color(rgb);
	path(x, y, w, h, r);
	cairo_fill(cr);
}

static void outline(Rect r, int radius, guint rgb, double width)
{
	color(rgb);
	cairo_set_line_width(cr, width);
	path(r.x + width / 2, r.y + width / 2, r.w - width, r.h - width,
	     radius);
	cairo_stroke(cr);
}

static void dot(double x, double y, double radius, guint rgb)
{
	color(rgb);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, radius, 0, 2 * G_PI);
	cairo_fill(cr);
}

static void line(double x, double y, double xx, double yy, guint rgb,
		 double width)
{
	color(rgb);
	cairo_set_line_width(cr, width);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, x, y);
	cairo_line_to(cr, xx, yy);
	cairo_stroke(cr);
}

static PangoLayout *layout(const char *s, int size, int weight, int width,
			   gboolean wrap)
{
	PangoLayout *item = pango_layout_new(context);
	PangoFontDescription *font = pango_font_description_new();

	pango_font_description_set_family(
		font, "Inter, Noto Sans, Noto Sans Thai, sans-serif");
	pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
	pango_font_description_set_weight(font, (PangoWeight)weight);
	pango_layout_set_font_description(item, font);
	pango_font_description_free(font);
	pango_layout_set_text(item, s ? s : "", -1);
	if (width >= 0) {
		pango_layout_set_width(item, width * PANGO_SCALE);
		if (wrap)
			pango_layout_set_wrap(item, PANGO_WRAP_WORD_CHAR);
		else {
			pango_layout_set_single_paragraph_mode(item, TRUE);
			pango_layout_set_ellipsize(item, PANGO_ELLIPSIZE_END);
			/*
			 * The layout is not mirrored, so a one-line label in a
			 * right-to-left script still starts at its box's left.
			 */
			pango_layout_set_auto_dir(item, FALSE);
		}
	}
	return item;
}

/*
 * Cache shaped controls and short labels across animation frames. Include
 * alignment in the key: PangoLayout is mutable, so centered controls must
 * never share a layout with body text. The fixed table bounds retained text.
 */
enum {
	LABEL_SETS = 64,
	LABEL_WAYS = 2
};

struct cached_label {
	PangoLayout *layout;
	guint hash;
	int size, weight, width;
	gboolean wrap, center;
};

static struct cached_label labels[LABEL_SETS][LABEL_WAYS];
static guint8 victim[LABEL_SETS];
static double text_scale_x, text_scale_y;
static const char *context_lang; /* the language the labels were shaped in */

void ui_forget_labels(void)
{
	for (guint i = 0; i < LABEL_SETS; i++)
		for (guint j = 0; j < LABEL_WAYS; j++)
			g_clear_object(&labels[i][j].layout);
}

static PangoLayout *label_layout(const char *s, int size, int weight, int width,
				 gboolean wrap, gboolean center)
{
	if (!s)
		s = "";
	guint hash = g_str_hash(s);
	guint slot = (hash ^ ((guint)width * 31u) ^
		      (guint)(size * 13 + weight + wrap * 7 + center)) %
		     LABEL_SETS;
	for (guint j = 0; j < LABEL_WAYS; j++) {
		struct cached_label *entry = &labels[slot][j];

		if (entry->layout && entry->hash == hash &&
		    entry->size == size && entry->weight == weight &&
		    entry->width == width && entry->wrap == wrap &&
		    entry->center == center &&
		    !strcmp(s, pango_layout_get_text(entry->layout))) {
			victim[slot] = 1 - j;
			return g_object_ref(entry->layout);
		}
	}
	PangoLayout *item = layout(s, size, weight, width, wrap);

	if (center)
		pango_layout_set_alignment(item, PANGO_ALIGN_CENTER);
	if (strlen(s) <= 512) {
		struct cached_label *entry = &labels[slot][victim[slot]];

		g_clear_object(&entry->layout);
		*entry = (struct cached_label){ g_object_ref(item),
						hash,
						size,
						weight,
						width,
						wrap,
						center };
		victim[slot] ^= 1;
	}
	return item;
}

static void paint(PangoLayout *item, double x, double y, guint rgb)
{
	color(rgb);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, item);
}

static int text(const char *s, int x, int y, int w, int size, int weight,
		guint rgb, gboolean wrap)
{
	g_autoptr(PangoLayout) item =
		label_layout(s, size, weight, w, wrap, FALSE);
	int height;

	pango_layout_get_pixel_size(item, NULL, &height);
	paint(item, x, y, rgb);
	return height;
}

static void centered(const char *s, Rect r, int size, int weight, guint rgb)
{
	g_autoptr(PangoLayout) item =
		label_layout(s, size, weight, r.w - 8, FALSE, TRUE);
	int h;

	pango_layout_get_pixel_size(item, NULL, &h);
	paint(item, r.x + 4, r.y + (r.h - h) / 2, rgb);
}

static gboolean hovered(Rect r)
{
	return ui.hover_x >= r.x && ui.hover_x < r.x + r.w &&
	       ui.hover_y >= r.y && ui.hover_y < r.y + r.h;
}

/*
 * Small, bounded property tracks. Retargeting samples the current value so
 * fast pointer moves and interrupted keyboard navigation do not jump.
 */
struct color_transition {
	Rect r, damage;
	int kind;
	double from, to;
	gint64 start, end;
};

enum {
	HOVER = 1,
	SELECTED,
	FOCUSED,
	SCRIM
};
static struct color_transition tracks[48];
static gint64 frame_us;
static double scrim;
static struct {
	UiPress timing;
	int x, y, layer;
	gboolean connected;
	double depth;
	gint64 next, end;
	SDL_Rect damage;
} press;
static int painting_layer;

struct slide_transition {
	int key;
	double from;
	gint64 start, end;
};
static struct slide_transition pane_slide = { .key = -1 },
			       dialog_slide = { .key = -1 };
/* The call stage grows and shrinks instead of jumping. */
static struct {
	double from, to;
	gint64 start, end;
} stage_motion;

static double remaining(gint64 start, gint64 end)
{
	if (frame_us >= end || end <= start || ui_reduce_motion())
		return 0.;
	double left = CLAMP((double)(end - frame_us) / (end - start), 0., 1.);

	return left * left * left;
}

static double sample(const struct color_transition *t)
{
	return t->to + (t->from - t->to) * remaining(t->start, t->end);
}

static double track_strength(int kind, Rect r, gboolean on, Rect damage)
{
	if (ui_reduce_motion())
		return on ? 1. : 0.;
	struct color_transition *free_track = NULL, *oldest = tracks;

	for (guint i = 0; i < G_N_ELEMENTS(tracks); i++) {
		struct color_transition *t = &tracks[i];

		if (t->kind == kind && !memcmp(&t->r, &r, sizeof(r))) {
			double v = sample(t), target = on ? 1. : 0.;

			t->damage = damage;
			if (t->to != target) {
				t->from = v;
				t->to = target;
				t->start = frame_us;
				t->end = frame_us +
					 (kind == SELECTED ? 150000 : 110000);
			}
			if (!on && v <= 0.)
				t->kind = 0;
			return v;
		}
		if (!t->kind || (!t->to && t->end <= frame_us))
			free_track = t;
		if (t->end < oldest->end)
			oldest = t;
	}
	if (!on)
		return 0.;
	struct color_transition *t = free_track ? free_track : oldest;
	*t = (struct color_transition){ .r = r,
					.damage = damage,
					.kind = kind,
					.from = 0,
					.to = 1,
					.start = frame_us,
					.end = frame_us + 120000 };
	return 0.;
}

static double strength(int kind, Rect r, gboolean on)
{
	return track_strength(kind, r, on, (Rect){ 0 });
}

void ui_draw_pointer(const SDL_Event *e)
{
	if (e->type == SDL_WINDOWEVENT) {
		if (e->window.windowID == SDL_GetWindowID(ui.window) &&
		    (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
		     e->window.event == SDL_WINDOWEVENT_HIDDEN)) {
			press.timing = (UiPress){ 0 };
			press.end = 0;
		}
		return;
	}
	if (e->button.windowID != SDL_GetWindowID(ui.window) ||
	    e->button.button != SDL_BUTTON_LEFT || ui_reduce_motion())
		return;
	if (e->type == SDL_MOUSEBUTTONDOWN) {
		press.timing = (UiPress){ .down = g_get_monotonic_time() };
		press.x = e->button.x;
		press.y = e->button.y;
		press.layer = ui.connected ? ui_media_layer() : 0;
		press.connected = ui.connected;
	} else if (press.timing.down)
		press.timing.up = g_get_monotonic_time();
	else
		return;
	ui.dirty = TRUE;
}

static double button_depth(Rect r)
{
	if (!press.timing.down || ui_reduce_motion() ||
	    press.connected != ui.connected || press.layer != painting_layer ||
	    press.x < r.x || press.x >= r.x + r.w || press.y < r.y ||
	    press.y >= r.y + r.h)
		return 0.;
	if (press.next) {
		SDL_Rect bounds = { r.x - 3, r.y - 3, r.w + 6, r.h + 6 };

		SDL_UnionRect(&press.damage, &bounds, &press.damage);
		press.end = press.next;
	}
	return press.depth;
}

static int blend(guint a, guint b, double v)
{
	if (v <= 0.)
		return a;
	if (v >= 1.)
		return b;
	guint r = 0;

	for (int i = 0; i <= 16; i += 8) {
		double x = (a >> i) & 255, y = (b >> i) & 255;

		r |= (guint)(x + (y - x) * v + .5) << i;
	}
	return r;
}

static double slide(struct slide_transition *s, int key, double distance)
{
	enum {
		SETTLE_US = 180000
	};
	gboolean reduced = ui_reduce_motion();
	double position =
		s->from * ui_settle((double)(frame_us - s->start) / SETTLE_US);

	if (s->key != key || reduced) {
		s->from = s->key < 0 || reduced || !distance ? 0. :
			  frame_us < s->end		     ? position :
							       distance;
		s->key = key;
		s->start = frame_us;
		s->end = s->from == 0 ? frame_us : frame_us + SETTLE_US;
		return s->from;
	}
	return position;
}

gint64 ui_animation_deadline(gint64 now_us)
{
	if (ui_reduce_motion())
		return 0;
	gint64 end = MAX(MAX(pane_slide.end, dialog_slide.end),
			 MAX(stage_motion.end, press.end));

	for (guint i = 0; i < G_N_ELEMENTS(tracks); i++)
		if (tracks[i].kind)
			end = MAX(end, tracks[i].end);
	return end > now_us ? end : 0;
}

static void active_ring(Rect r)
{
	outline((Rect){ r.x - 3, r.y - 3, r.w + 6, r.h + 6 }, 12,
		UI_ACCENT_HOVER, 1.5);
}

static void action(Rect r, const char *s, gboolean solid, gboolean focus)
{
	double h = strength(HOVER, r, hovered(r));
	double depth = button_depth(r);
	/* Dark lettering keeps the hot-pink primary button readable. */
	guint bg = blend(solid ? UI_ACCENT : UI_RAISED,
			 solid ? UI_ACCENT_HOVER : UI_SELECTED, h);

	box(r.x + 2. * depth, r.y + 2. * depth, r.w - 4. * depth,
	    r.h - 3. * depth, 8, bg);
	centered(s, r, 14, 600, solid ? UI_ON_ACCENT : UI_HEADING);
	double f = strength(FOCUSED, r, focus && ui.active);

	if (f > 0.)
		outline((Rect){ r.x - 3, r.y - 3, r.w + 6, r.h + 6 }, 12,
			blend(UI_BG, UI_ACCENT_HOVER, f), 1.5);
}

static int rail_w(void)
{
	return ui_compact() ? 0 : ui_theme_is_line() ? 68 : 72;
}

static int sidebar_w(void)
{
	if (ui_compact())
		return ui.width;
	return rail_w() + (ui.width < 900     ? 220 :
			   ui_theme_is_line() ? 284 :
						240);
}

enum {
	HEADER_H = 48,
	USER_PANEL_H = 52,
	RAIL_ITEMS = 6
};

/* Unread chats are pinned to the rail like Discord's direct-message pills. */
static struct {
	Rect box[RAIL_ITEMS];
	Json *row[RAIL_ITEMS]; /* Borrowed until the chat list changes. */
	int count, available;
	gboolean valid;
} rail_items;

const char *ui_rail_chat_at(int x, int y)
{
	for (int i = 0; i < rail_items.count; i++)
		if (x >= rail_items.box[i].x &&
		    x < rail_items.box[i].x + rail_items.box[i].w &&
		    y >= rail_items.box[i].y &&
		    y < rail_items.box[i].y + rail_items.box[i].h)
			return ui_string(rail_items.row[i], "id", "");
	return NULL;
}

void ui_forget_rail(void)
{
	rail_items.count = 0;
	rail_items.available = 0;
	rail_items.valid = FALSE;
}

static int rail_shortcuts_top(void)
{
	return 76;
}

/* Your avatar at the foot of the LINE navigation bar. */
static Rect bar_self;

/* LINE puts the tabs and account buttons in the bar and drops the panel. */
static gboolean line_nav(void)
{
	return ui_theme_is_line() && rail_w() > 0;
}

static void collect_rail(void)
{
	rail_items.count = 0;
	/* LINE's bar holds tabs only; unread chats show as one count. */
	if (ui_compact() || !ui.chats || ui_theme_is_line())
		return;
	if (!rail_items.valid) {
		size_t n = json_object_array_length(ui.chats);

		for (size_t i = 0; i < n && rail_items.available < RAIL_ITEMS;
		     i++) {
			Json *row = json_object_array_get_idx(ui.chats, i);

			if (json_object_get_int64(ui_field(row, "unread")) > 0)
				rail_items.row[rail_items.available++] = row;
		}
		rail_items.valid = TRUE;
	}
	for (int i = 0; i < rail_items.available; i++) {
		int y = rail_shortcuts_top() + i * 56;

		if (y + 48 > ui.height - 70)
			break;
		rail_items.box[rail_items.count++] =
			(Rect){ (rail_w() - 48) / 2, y, 48, 48 };
	}
}

static int qr_step(void)
{
	int target = MIN(200, MIN(ui.width - 112, ui.height - 360));

	return MAX(1, target / (ui.qr->width + 8));
}

static int qr_pixels(void)
{
	return qr_step() * (ui.qr->width + 8);
}

static int sign_in_body(void)
{
	if (ui.qr)
		return qr_pixels() + 64;
	if (ui.pin)
		return 112;
	return ui.email_login && !ui.authenticating ? 136 : 0;
}

static void layout_sign_in(void)
{
	gboolean welcome = !ui.email_login && !ui.authenticating && !ui.pin &&
			   !ui.qr;
	gboolean wide = welcome && ui.width >= 760 && ui.height >= 360;
	int pad = ui_compact() ? 24 : 32;
	int w = MIN(wide ? 832 : 480, ui.width - 32);
	int body = sign_in_body(), form_h = 88 + body + 44;
	int side = 0, x, y, inner;

	if (!ui.authenticating)
		form_h += 102;
	else if (!ui.qr && !ui.pin && ui.notice && *ui.notice)
		form_h += 56;
	int h = 2 * pad + form_h;

	if (wide) {
		side = MIN(320,
			   MIN(w - 3 * pad - 340, ui.height - 2 * pad - 32));
		h = MAX(h, side + 2 * pad);
	} else if (welcome) {
		/* Give the buttons and the session notice first claim on space. */
		side = MIN(208, MIN(w - 2 * pad, ui.height - h - 44));
		if (side >= 64)
			h += side + 12;
		else
			side = 0;
	}
	sign_card = (Rect){ (ui.width - w) / 2, MAX(16, (ui.height - h) / 2), w,
			    h };
	x = sign_card.x + pad;
	y = sign_card.y + pad;
	inner = w - 2 * pad;
	sign_portrait = (Rect){ 0 };
	if (wide) {
		sign_portrait =
			(Rect){ x, sign_card.y + (h - side) / 2, side, side };
		x += side + pad;
		inner -= side + pad;
		y = sign_card.y + (h - form_h) / 2;
	} else if (side) {
		sign_portrait =
			(Rect){ sign_card.x + (w - side) / 2, y, side, side };
		y += side + 12;
	}
	sign_heading = (Rect){ x, y, inner, 34 };
	y += 88;
	ui.login = (Rect){ x, y + body, inner, 44 };
	ui.login_mode = (Rect){ x, ui.login.y + 54, inner, 34 };
	ui.email_box = (Rect){ x, y + 19, inner, 38 };
	ui.password_box = (Rect){ x, y + 83, inner, 38 };
}

void ui_layout(void)
{
	SDL_GetWindowSize(ui.window, &ui.width, &ui.height);
	int s = sidebar_w(), h = ui_media_content_height(), w = ui.width;
	int chat_x = ui_compact() ? 0 : s;
	int rail = rail_w(), list_w = s - rail;

	if (!ui.connected)
		layout_sign_in();
	if (welcome_art && (ui.connected || !sign_portrait.w))
		g_clear_pointer(&welcome_art, cairo_surface_destroy);
	ui.back = (Rect){ 8, 8, 58, 32 };
	/* Sidebar: search header, Friends, section heading, rows, user panel. */
	gboolean line_style = ui_theme_is_line(), nav = line_nav();
	int top = HEADER_H + 8;

	ui.search_box = (Rect){ rail + 10, 10, list_w - 20, 28 };
	if (line_style) {
		/* LINE: a "Chats" title row, a search pill, then the folder. */
		ui.search_box = (Rect){ rail + 14, 56, list_w - 28, 34 };
		top = 98;
	}
	ui.friends_button = (Rect){ 0 };
	ui.rail_chats = (Rect){ 0 };
	if (nav) {
		/* The bar lists Friends above Chats, as LINE does. */
		int y = 16;

		if (ui.friends) {
			ui.friends_button =
				(Rect){ (rail - 48) / 2, y, 48, 48 };
			y += 60;
		}
		ui.rail_chats = (Rect){ (rail - 48) / 2, y, 48, 48 };
	} else if (ui.friends) {
		ui.friends_button = (Rect){ rail + 8, top, list_w - 16, 42 };
		top += 50;
	}
	int heading = top + 10;

	ui.new_group = (Rect){ s - 30, heading - 2, 20, 20 };
	ui.reload = (Rect){ s - (ui.groups ? 54 : 30), heading - 2, 20, 20 };
	if (line_style) {
		ui.new_group = (Rect){ s - 44, 14, 30, 30 };
		ui.reload = (Rect){ s - (ui.groups ? 78 : 44), 14, 30, 30 };
		heading = top + 6;
	}
	ui.folders_button = (Rect){ 0 };
	if (ui_lists_ready()) {
		int fw = ui.reload.x - rail - 14;

		/* LINE's folder menu hugs its label instead of the row. */
		if (line_style)
			fw = MIN(list_w - 20,
				 ui_text_width(ui_folder_title(), 13, 600) +
					 36);
		ui.folders_button = (Rect){ rail + 8, heading - 3, fw, 22 };
	}
	int list_y = heading + 22;

	ui.list_box = (Rect){
		rail, list_y, list_w,
		MAX(60,
		    h - list_y - (nav ? 0 : USER_PANEL_H) -
			    (ui_compact() && ui.notice && *ui.notice ? 60 : 0))
	};
	ui.logout = (Rect){ s - 42, h - 42, 32, 32 };
	if (nav) {
		/* Account buttons stack at the foot of the bar. */
		int x = (rail - 40) / 2;

		ui.logout = (Rect){ x, h - 52, 40, 40 };
		bar_self = (Rect){ x - 2, h - 200, 44, 44 };
		ui_profile_place(bar_self, (Rect){ x, h - 144, 40, 40 },
				 (Rect){ x, h - 98, 40, 40 });
	}
	ui.rail_add = (Rect){ 0 };
	collect_rail();
	if (rail && ui.groups && !nav)
		ui.rail_add =
			(Rect){ (rail - 48) / 2,
				rail_shortcuts_top() + rail_items.count * 56 +
					(rail_items.count ? 8 : 0),
				48, 48 };
	/* Chat header icons sit right to left, 32 px apart. */
	int icon_x = w - 16 - 32;

	ui.members = (Rect){ 0 };
	ui.call = ui.video = (Rect){ 0 };
	ui.chat_tools = (Rect){ icon_x, 8, 32, 32 };
	icon_x -= 40;
	if (ui.groups && ui.selected && *ui.selected == 'c') {
		ui.members = (Rect){ icon_x, 8, 32, 32 };
		icon_x -= 40;
	}
	ui.find_button = (Rect){ 0 };
	if (ui.find_messages) {
		ui.find_button = (Rect){ icon_x, 8, 32, 32 };
		icon_x -= 40;
	}
	if (ui_media_can_call()) {
		if (ui.video_calls) {
			ui.video = (Rect){ icon_x, 8, 32, 32 };
			icon_x -= 40;
		}
		ui.call = (Rect){ icon_x, 8, 32, 32 };
	}
	/* One rounded composer holds attach, the text, emoji and send. */
	ui.composer = (Rect){ chat_x + 16, h - 68, w - chat_x - 32, 44 };
	Rect c = ui.composer;

	ui.attach = (Rect){ c.x + 6, c.y + 6, 32, 32 };
	ui.send = (Rect){ c.x + c.w - 38, c.y + 6, 32, 32 };
	ui.emoji = (Rect){
		c.x + c.w -
			((ui.message.value && ui.message.value->len) ? 76 : 38),
		c.y + 6, 32, 32
	};
	int entry_x = c.x + ((ui.photos || ui.files) ? 44 : 4);
	int entry_right = (ui.emojis || ui.stickers) ? ui.emoji.x - 4 :
						       ui.send.x - 4;

	ui.entry = (Rect){ entry_x, c.y, MAX(68, entry_right - entry_x), c.h };
	int bottom = c.y - 8;

	if (line_style) {
		/*
		 * LINE's input pane: the text spans the full width, with the
		 * tool icons on a row below it and a Send button at the right.
		 */
		ui.composer = c = (Rect){ chat_x, h - 100, w - chat_x, 100 };
		ui.entry = (Rect){ c.x + 12, c.y + 6, c.w - 24, 44 };
		int tool_x = c.x + 14, tool_y = h - 44;

		if (ui.emojis || ui.stickers) {
			ui.emoji = (Rect){ tool_x, tool_y, 32, 32 };
			tool_x += 38;
		}
		ui.attach = (Rect){ tool_x, tool_y, 32, 32 };
		ui.send = (Rect){ c.x + c.w - 90, tool_y - 1, 74, 34 };
		bottom = c.y;
	}
	ui.conversation =
		(Rect){ chat_x, HEADER_H + 1, w - chat_x,
			MAX(80, bottom - HEADER_H - 1 -
					(ui.notice && *ui.notice ? 52 : 0) -
					(ui.reply_id ? 34 : 0)) };
	/* A call in this chat sits above its messages, as in Discord. */
	int target = ui_media_stage_height();
	gint64 now = g_get_monotonic_time();
	double left = 0.;

	if (stage_motion.end > now && !ui_reduce_motion()) {
		left = (double)(stage_motion.end - now) /
		       (stage_motion.end - stage_motion.start);
		left = left * left * left;
	}
	double shown = stage_motion.to +
		       (stage_motion.from - stage_motion.to) * left;

	if (target != (int)stage_motion.to) {
		stage_motion.from = shown;
		stage_motion.to = target;
		stage_motion.start = now;
		stage_motion.end = now + 180000;
	}
	int stage = left > 0. ? (int)(shown + .5) : target;

	ui.stage_h = stage;

	if (stage) {
		ui.conversation.y += stage;
		ui.conversation.h = MAX(60, ui.conversation.h - stage);
	}
	if (ui.older_page || ui.has_older) {
		int y = ui.conversation.y + 7;

		ui.older_button = (Rect){ chat_x + 16, y, 132, 28 };
		ui.latest_button = (Rect){ w - 138, y, 122, 28 };
		ui.conversation.y += 40;
		ui.conversation.h -= 40;
	}
}

static void draw_editor(Editor *edit, Rect r, gboolean focus,
			const char *placeholder);
static void icon_search(double cx, double cy, guint ink, double r);
static void draw_image(cairo_surface_t *image, Rect r, unsigned int angle,
		       cairo_filter_t filter);

static Editor password_mask(GString *mask)
{
	Editor *password = &ui.login_password;
	const char *p = password->value->str;
	const char bullet[] = "•";
	int stride = sizeof(bullet) - 1;
	int cursor = g_utf8_pointer_to_offset(p, p + password->cursor);
	int anchor = g_utf8_pointer_to_offset(p, p + password->anchor);
	size_t len = g_utf8_strlen(p, password->value->len);

	for (size_t i = 0; i < len; i++)
		memcpy(mask->str + i * stride, bullet, stride);
	mask->len = len * stride;
	mask->str[mask->len] = 0;
	return (Editor){ mask, cursor * stride, anchor * stride,
			 password->scroll };
}

int ui_password_index(int x)
{
	char masked[3 * LINE_CREDENTIAL_LIMIT + 1];
	GString text = { .str = masked, .allocated_len = sizeof(masked) };
	Editor mask = password_mask(&text);
	int index = ui_edit_hit(&mask, ui.password_box, x);
	const char *p = ui.login_password.value->str;

	return g_utf8_offset_to_pointer(p, index / 3) - p;
}

static void sign_in(void)
{
	int w = ui.width, h = ui.height;
	Rect card = sign_card;
	int x = sign_heading.x, y = sign_heading.y, inner = sign_heading.w;

	box(0, 0, w, h, 0, UI_RAIL);
	/* Draw the background rings at the current window size. */
	color(UI_PANEL);
	cairo_set_line_width(cr, 1);
	for (int i = 0; i < 6; i++) {
		cairo_new_path(cr);
		cairo_arc(cr, w / 2., h / 2., 260 + i * 90, 0, 2 * G_PI);
		cairo_stroke(cr);
	}
	box(card.x - 1, card.y - 1, card.w + 2, card.h + 2, 13, UI_LINE);
	box(card.x, card.y, card.w, card.h, 12, UI_DIALOG);
	if (sign_portrait.w) {
		Rect art = sign_portrait;
		int cx = art.x + art.w / 2, cy = art.y + art.h / 2;

		dot(cx, cy, art.w * .44, UI_SELECTED);
		if (!welcome_art)
			welcome_art = ui_logo_portrait();
		if (!welcome_art && logo)
			welcome_art = cairo_surface_reference(logo);
		if (welcome_art == logo)
			art = (Rect){ cx - UI_LOGO_EDGE / 2,
				      cy - UI_LOGO_EDGE / 2, UI_LOGO_EDGE,
				      UI_LOGO_EDGE };
		draw_image(welcome_art, art, 0, CAIRO_FILTER_BEST);
	}

	const char *heading = ui.qr	     ? _("Log in with QR code") :
			      ui.pin	     ? _("Verify this device") :
			      ui.email_login ? _("Connect your LINE account") :
					       _("Welcome to Avernella");
	const char *sub =
		ui.qr  ? _("Scan this with the LINE app on your phone.") :
		ui.pin ? _("Enter this code in LINE on your phone.") :
		ui.email_login ?
			 _("LINE may ask you to verify on your phone.") :
			 _("Independent LINE desktop client.");

	centered(heading, sign_heading, 24, 700, UI_HEADING);
	y += 40;
	centered(sub, (Rect){ x, y, inner, 24 }, 16, 400, UI_MUTED);
	y += 48;
	if (ui.qr) {
		int step = qr_step(), side = qr_pixels();
		int qr_x = x + (inner - side) / 2;

		box(qr_x - 8, y - 8, side + 16, side + 16, 8, WHITE);
		color(0x09110d);
		for (int a = 0; a < ui.qr->width; a++)
			for (int b = 0; b < ui.qr->width; b++)
				if (ui.qr->data[a * ui.qr->width + b] & 1)
					cairo_rectangle(cr,
							qr_x + (b + 4) * step,
							y + (a + 4) * step,
							step, step);
		cairo_fill(cr);
		y += side + 16;
		if (ui.notice && *ui.notice)
			centered(ui.notice, (Rect){ x, y, inner, 40 }, 14, 450,
				 UI_MUTED);
	} else if (ui.pin) {
		centered(ui.pin, (Rect){ x, y, inner, 56 }, 44, 800,
			 UI_HEADING);
		y += 64;
		if (ui.notice && *ui.notice)
			centered(ui.notice, (Rect){ x, y, inner, 40 }, 14, 450,
				 UI_MUTED);
	} else if (ui.email_login && !ui.authenticating) {
		char masked[3 * LINE_CREDENTIAL_LIMIT + 1];
		GString buffer = { .str = masked,
				   .allocated_len = sizeof(masked) };
		Editor mask = password_mask(&buffer);

		text(_("Email address"), ui.email_box.x, ui.email_box.y - 20,
		     inner, 13, 600, UI_HEADING, FALSE);
		draw_editor(&ui.login_email, ui.email_box,
			    ui.focus == F_LOGIN_EMAIL, "");
		text(_("Password"), ui.password_box.x, ui.password_box.y - 20,
		     inner, 13, 600, UI_HEADING, FALSE);
		draw_editor(&mask, ui.password_box,
			    ui.focus == F_LOGIN_PASSWORD, "");
		ui.login_password.scroll = mask.scroll;
	}
	action(ui.login,
	       ui.authenticating ? _("Cancel") :
	       ui.email_login	 ? _("Sign in") :
				   _("Sign in with QR code"),
	       !ui.authenticating, ui.focus == F_LOGIN);
	int bottom = ui.login.y + ui.login.h + 16;

	if (!ui.authenticating) {
		action(ui.login_mode,
		       ui.email_login ? _("Use a QR code instead") :
					_("Sign in with email"),
		       FALSE, ui.focus == F_LOGIN_MODE);
		bottom = ui.login_mode.y + ui.login_mode.h + 12;
	}
	if (!ui.qr && !ui.pin && ui.notice && *ui.notice)
		text(ui.notice, x, bottom, inner, 13, 550, UI_HEADING, TRUE);
	else if (!ui.authenticating)
		text(_("Approving a new desktop session may sign you out on another desktop."),
		     x, bottom, inner, 12, 400, UI_SUBTLE, TRUE);
}

static int caret_x(PangoLayout *layout, int index)
{
	PangoRectangle strong;

	pango_layout_get_cursor_pos(layout, index, &strong, NULL);
	return PANGO_PIXELS(strong.x);
}

static int edit_padding(Editor *edit)
{
	if (edit == &ui.search)
		return ui_theme_is_line() ? 34 : 10;
	return edit == &ui.message ? 8 : 14;
}

int ui_edit_hit(Editor *edit, Rect r, int x)
{
	if (!context)
		return (int)edit->value->len;
	g_autoptr(PangoLayout) item =
		layout(edit->value->str, 15, 400, -1, FALSE);
	if (edit == &ui.message)
		ui_emoji_shape_editor(item, 0, 0, 0);
	int index = 0, trailing = 0;

	pango_layout_xy_to_index(item,
				 (x - r.x - edit_padding(edit) + edit->scroll) *
					 PANGO_SCALE,
				 0, &index, &trailing);
	const char *s = edit->value->str;

	return (int)(g_utf8_offset_to_pointer(s + index, trailing) - s);
}

int ui_text_index(Editor *edit, int x)
{
	return ui_edit_hit(edit, edit == &ui.search ? ui.search_box : ui.entry,
			   x);
}

static void draw_editor(Editor *edit, Rect r, gboolean focus,
			const char *placeholder)
{
	double focus_amount = strength(FOCUSED, r, focus && ui.active);

	/* The composer draws its own rounded bar around the message field. */
	if (edit == &ui.search) {
		int radius = ui_theme_is_line() ? r.h / 2 : 6;

		box(r.x, r.y, r.w, r.h, radius, UI_RAIL);
		if (focus_amount > 0.)
			outline(r, radius,
				blend(UI_RAIL, UI_ACCENT, focus_amount), 1.);
		if (ui_theme_is_line())
			icon_search(r.x + 19, r.y + r.h / 2. - 1,
				    focus ? UI_BODY : UI_SUBTLE, 5.5);
	} else if (edit != &ui.message) {
		box(r.x, r.y, r.w, r.h, 8, UI_RAIL);
		outline(r, 8, blend(UI_INPUT, UI_ACCENT, focus_amount), 1.);
	}
	int pad = edit_padding(edit);

	gboolean composing = focus && ui.composition && *ui.composition;
	int from = MIN(edit->cursor, edit->anchor),
	    to = MAX(edit->cursor, edit->anchor);
	g_autoptr(GString) preedit = NULL;
	const char *display = edit->value->str;
	int caret = edit->cursor;

	if (composing) {
		preedit = g_string_new(display);
		g_string_erase(preedit, from, to - from);
		g_string_insert(preedit, from, ui.composition);
		display = preedit->str;
		caret = from + (int)strlen(ui.composition);
	}
	gboolean rich = edit == &ui.message && ui_emoji_has_tokens();

	g_autoptr(PangoLayout) item =
		rich ? layout(display, 15, 400, -1, FALSE) :
		       label_layout(display, 15, 400, -1, FALSE, FALSE);
	if (rich)
		ui_emoji_shape_editor(
			item, composing ? from : 0, composing ? to : 0,
			composing ? (int)strlen(ui.composition) : 0);
	int pos = caret_x(item, caret), width = r.w - pad - 12, ox = r.x + pad;

	if (pos - edit->scroll > width - 2)
		edit->scroll = pos - width + 2;
	if (pos < edit->scroll)
		edit->scroll = pos;
	int origin = ox - edit->scroll, height;

	pango_layout_get_pixel_size(item, NULL, &height);
	int top = r.y + (r.h - height) / 2;

	cairo_save(cr);
	cairo_rectangle(cr, ox, r.y + 2, width, r.h - 4);
	cairo_clip(cr);
	if (!*display)
		text(placeholder, ox, top + (edit == &ui.search ? 1 : 0), width,
		     edit == &ui.search ? 14 : 15,
		     edit == &ui.search ? 500 : 400,
		     edit == &ui.search ? UI_SUBTLE : UI_MUTED, FALSE);
	else {
		if (focus && !composing && from != to) {
			int a = caret_x(item, from), b = caret_x(item, to);

			box(origin + MIN(a, b), top, ABS(b - a), height, 2,
			    UI_TEXT_SELECTION);
		}
		paint(item, origin, top, UI_HEADING);
		if (composing)
			line(origin + caret_x(item, from), top + height - 1,
			     origin + pos, top + height - 1, UI_ACCENT, 1.5);
	}
	if (focus && ui.active) {
		line(origin + pos, top + 1, origin + pos, top + height - 1,
		     UI_HEADING, 1);
		/* Anchor the IME candidate popover to the caret, including horizontal scrolling. */
		static SDL_Rect last_ime;
		SDL_Rect caret = { CLAMP(origin + pos, ox, ox + width), top, 1,
				   height };
		if (memcmp(&caret, &last_ime, sizeof(caret))) {
			SDL_SetTextInputRect(&caret);
			last_ime = caret;
		}
	}
	cairo_restore(cr);
}

Json *ui_selected_chat(void)
{
	static size_t last_index;

	if (!ui.chats || !ui.selected)
		return NULL;
	size_t count = json_object_array_length(ui.chats);

	if (last_index < count) {
		Json *last = json_object_array_get_idx(ui.chats, last_index);

		if (!g_strcmp0(ui.selected, ui_string(last, "id", "")))
			return last;
	}
	for (size_t i = 0; i < count; i++) {
		Json *c = json_object_array_get_idx(ui.chats, i);

		if (!g_strcmp0(ui.selected, ui_string(c, "id", ""))) {
			last_index = i;
			return c;
		}
	}
	return NULL;
}

static void avatar(const char *name, int x, int y, int side, const char *seed,
		   const char *chat, const char *key)
{
	cairo_surface_t *image = ui_avatar(chat, seed, key);

	if (image) {
		int w = cairo_image_surface_get_width(image),
		    h = cairo_image_surface_get_height(image);
		double z = (double)side / MIN(w, h);

		cairo_save(cr);
		cairo_new_path(cr);
		cairo_arc(cr, x + side / 2., y + side / 2., side / 2., 0,
			  2 * G_PI);
		cairo_clip(cr);
		cairo_translate(cr, x + (side - w * z) / 2,
				y + (side - h * z) / 2);
		cairo_scale(cr, z, z);
		cairo_set_source_surface(cr, image, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	}
	char initial[7] = "?";

	if (name && *name) {
		int n = g_unichar_to_utf8(
			g_unichar_toupper(g_utf8_get_char(name)), initial);
		initial[n] = 0;
	}
	box(x, y, side, side, side / 2, ui_avatar_shade(seed));
	centered(initial, (Rect){ x, y, side, side }, side / 2, 650, WHITE);
}

guint ui_avatar_shade(const char *seed)
{
	static const guint shades[] = { 0x267d72, 0x44816b, 0x9e6651, 0x82609d,
					0x497897 };

	return shades[g_str_hash(seed ? seed : "") % G_N_ELEMENTS(shades)];
}

/*
 * Tooltips float above whatever is drawn after their anchor, such as the
 * chat list beside the server rail, so they wait for the end of their layer.
 */
static struct {
	char *text;
	Rect anchor;
	cairo_matrix_t matrix;
	gboolean right;
} pending_tip;

static void tooltip(Rect anchor, const char *s, gboolean right)
{
	g_free(pending_tip.text);
	pending_tip.text = g_strdup(s);
	pending_tip.anchor = anchor;
	pending_tip.right = right;
	cairo_get_matrix(cr, &pending_tip.matrix);
}

static void draw_tooltip(Rect anchor, const char *s, gboolean right)
{
	g_autoptr(PangoLayout) t = layout(s, 14, 600, -1, FALSE);
	int tw, th;

	pango_layout_get_pixel_size(t, &tw, &th);
	Rect tip = right ? (Rect){ anchor.x + anchor.w + 12,
				   anchor.y + (anchor.h - 34) / 2, tw + 24,
				   34 } :
			   (Rect){ anchor.x + (anchor.w - tw - 24) / 2,
				   anchor.y + anchor.h + 6, tw + 24, 34 };

	tip.x = CLAMP(tip.x, 4, MAX(4, ui.width - tip.w - 4));
	box(tip.x, tip.y, tip.w, tip.h, 8, UI_RAIL);
	outline(tip, 8, UI_INPUT, 1);
	paint(t, tip.x + 12, tip.y + (tip.h - th) / 2, UI_HEADING);
}

/* Draws the tooltip requested in the layer just drawn, where it was asked. */
static void flush_tooltip(void)
{
	g_autofree char *text = g_steal_pointer(&pending_tip.text);

	if (!text)
		return;
	cairo_save(cr);
	cairo_set_matrix(cr, &pending_tip.matrix);
	draw_tooltip(pending_tip.anchor, text, pending_tip.right);
	cairo_restore(cr);
}

static void icon_people(double x, double y, guint ink)
{
	color(ink);
	cairo_new_path(cr);
	cairo_arc(cr, x + 9, y + 7, 4, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_arc(cr, x + 9, y + 20, 8, G_PI, 2 * G_PI);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_arc(cr, x + 17, y + 8, 3.2, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_arc(cr, x + 18, y + 19, 6, 1.25 * G_PI, 2 * G_PI);
	cairo_line_to(cr, x + 24, y + 20);
	cairo_line_to(cr, x + 18, y + 20);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void icon_plus(double cx, double cy, double half, guint ink,
		      double width)
{
	line(cx - half, cy, cx + half, cy, ink, width);
	line(cx, cy - half, cx, cy + half, ink, width);
}

static void icon_reload(double cx, double cy, guint ink)
{
	cairo_set_line_width(cr, 1.8);
	color(ink);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 6, .3, 5.7);
	cairo_stroke(cr);
	line(cx + 5, cy - 7, cx + 5, cy - 2.5, ink, 1.8);
	line(cx + 5, cy - 2.5, cx + .5, cy - 2.5, ink, 1.8);
}

static void icon_exit(double x, double y, guint ink)
{
	cairo_set_line_width(cr, 2);
	color(ink);
	cairo_new_path(cr);
	cairo_move_to(cr, x + 10, y + 4);
	cairo_line_to(cr, x + 5, y + 4);
	cairo_line_to(cr, x + 5, y + 20);
	cairo_line_to(cr, x + 10, y + 20);
	cairo_stroke(cr);
	line(x + 10, y + 12, x + 20, y + 12, ink, 2);
	cairo_new_path(cr);
	cairo_move_to(cr, x + 16, y + 8);
	cairo_line_to(cr, x + 20, y + 12);
	cairo_line_to(cr, x + 16, y + 16);
	cairo_stroke(cr);
}

static void icon_search(double cx, double cy, guint ink, double r)
{
	cairo_set_line_width(cr, 2);
	color(ink);
	cairo_new_path(cr);
	cairo_arc(cr, cx - r * .25, cy - r * .25, r, 0, 2 * G_PI);
	cairo_stroke(cr);
	line(cx + r * .45, cy + r * .45, cx + r * 1.15, cy + r * 1.15, ink, 2);
}

/* A server-rail button: circle at rest, rounded square when active. */
static void rail_button(Rect r, gboolean selected, gboolean unread, guint fill)
{
	double hover = strength(HOVER, r, hovered(r));
	double depth = fill ? button_depth(r) : 0.;
	double radius = selected ? 16 : 24 - 8 * hover;
	int pill = selected   ? 40 :
		   hover > 0. ? (int)(8 + 12 * hover) :
		   unread     ? 8 :
				0;

	if (pill)
		box(-4, r.y + (r.h - pill) / 2, 8, pill, 4,
		    selected ? UI_ACCENT : UI_BODY);
	if (fill)
		box(r.x + depth, r.y + depth, r.w - 2. * depth,
		    r.h - 2. * depth, radius, fill);
}

static void rail_avatar(Rect r, Json *c)
{
	const char *id = ui_string(c, "id", "");
	gboolean selected = !g_strcmp0(ui.selected, id);

	rail_button(r, selected, TRUE, 0);
	avatar(ui_string(c, "name", id), r.x, r.y, r.w, id, id,
	       ui_string(c, "avatarKey", NULL));
	gint64 unread = json_object_get_int64(ui_field(c, "unread"));
	char count[4] = "99+";

	if (unread < 100)
		g_snprintf(count, sizeof(count), "%d", (int)unread);
	int bw = unread < 10 ? 16 : unread < 100 ? 22 : 28;
	Rect b = { r.x + r.w - bw + 4, r.y + r.h - 16 + 4, bw, 16 };

	box(b.x - 4, b.y - 4, b.w + 8, b.h + 8, 12, UI_RAIL);
	box(b.x, b.y, b.w, b.h, 8, UI_ACCENT);
	centered(count, b, 12, 700, UI_ON_ACCENT);
	if (hovered(r))
		tooltip(r, ui_string(c, "name", id), TRUE);
}

/* A LINE tab: a 24 px glyph on a rounded tile, lit in the accent when open. */
static void nav_tab(Rect r, enum ui_icon icon, gboolean active, int count,
		    gboolean focus, const char *tip)
{
	double hover = strength(HOVER, r, hovered(r));
	double depth = button_depth(r);

	if (active || hover > 0.)
		box(r.x + depth, r.y + depth, r.w - 2. * depth,
		    r.h - 2. * depth, 12,
		    blend(UI_NAV_RAIL, UI_NAV_HOVER, active ? 1. : hover));
	ui_draw_icon(r, icon,
		     active	? UI_NAV_ACTIVE :
		     hover > .5 ? UI_HEADING :
				  UI_NAV_INK);
	if (count > 0) {
		char label[4] = "99+";

		if (count < 100)
			g_snprintf(label, sizeof(label), "%d", count);
		int bw = count < 10 ? 18 : count < 100 ? 24 : 30;
		Rect b = { r.x + r.w - bw + 6, r.y - 2, bw, 18 };

		box(b.x - 2, b.y - 2, b.w + 4, b.h + 4, 11, UI_NAV_RAIL);
		box(b.x, b.y, b.w, b.h, 9, UI_ACCENT);
		centered(label, b, 11, 700, UI_ON_ACCENT);
	} else if (count < 0) {
		dot(r.x + r.w - 9, r.y + 9, 5, UI_NAV_RAIL);
		dot(r.x + r.w - 9, r.y + 9, 3.5, UI_ACCENT);
	}
	if (focus)
		active_ring(r);
	if (hovered(r))
		tooltip(r, tip, TRUE);
}

static int unread_chats(void)
{
	gint64 total = 0;

	for (size_t i = 0; ui.chats && i < json_object_array_length(ui.chats);
	     i++)
		total += MAX(0, json_object_get_int64(ui_field(
					json_object_array_get_idx(ui.chats, i),
					"unread")));
	return (int)MIN(total, 999);
}

/* LINE's navigation bar: Friends and Chats tabs, account buttons below. */
static void line_bar(void)
{
	int h = ui.height, rail = rail_w();
	gboolean friends = ui_friends_active();

	box(0, 0, rail, h, 0, UI_NAV_RAIL);
	if (ui.friends)
		nav_tab(ui.friends_button, UI_ICON_PEOPLE, friends,
			ui_profile_badge() > 0 ? -1 : 0, ui.focus == F_FRIENDS,
			_("Friends"));
	nav_tab(ui.rail_chats, UI_ICON_MESSAGE, !friends, unread_chats(), FALSE,
		_("Chats"));
	Rect me = bar_self;
	double mh = strength(HOVER, me, hovered(me));

	if (mh > 0.)
		dot(me.x + me.w / 2., me.y + me.h / 2., me.w / 2.,
		    blend(UI_NAV_RAIL, UI_NAV_HOVER, mh));
	avatar(ui.name ? ui.name : "?", me.x + 4, me.y + 4, me.w - 8,
	       ui.self_id ? ui.self_id : "self", ui.self_id,
	       ui_profile_self_avatar());
	dot(me.x + me.w - 9, me.y + me.h - 9, 6, UI_NAV_RAIL);
	dot(me.x + me.w - 9, me.y + me.h - 9, 4, UI_ONLINE);
	if (hovered(me))
		tooltip(me, ui.name && *ui.name ? ui.name : _("Profile"), TRUE);
	ui_profile_panel_draw();
	Rect r = ui.logout;
	double hv = strength(HOVER, r, hovered(r));

	if (hv > 0.)
		box(r.x, r.y, r.w, r.h, 12,
		    blend(UI_NAV_RAIL, UI_NAV_HOVER, hv));
	if (ui.focus == F_LOGOUT)
		active_ring(r);
	icon_exit(r.x + 8, r.y + 8, hovered(r) ? UI_DANGER : UI_NAV_INK);
	if (hovered(r))
		tooltip(r, _("Sign out"), TRUE);
}

static void server_rail(void)
{
	int h = ui.height;

	if (line_nav()) {
		line_bar();
		return;
	}
	box(0, 0, rail_w(), h, 0, UI_RAIL);
	Rect home = { 12, 12, 48, 48 };

	rail_button(home, TRUE, FALSE, UI_SELECTED);
	ui_draw_image(logo, (Rect){ home.x + 2, home.y + 2, 44, 44 });
	box(24, 68, 24, 2, 1, UI_LINE);
	if (hovered(home))
		tooltip(home, _("Direct Messages"), TRUE);
	for (int i = 0; i < rail_items.count; i++)
		rail_avatar(rail_items.box[i], rail_items.row[i]);
	if (ui.groups && ui.rail_add.w) {
		Rect a = ui.rail_add;
		double hover = strength(HOVER, a, hovered(a));

		rail_button(a, FALSE, FALSE,
			    blend(UI_PANEL, UI_SELECTED, hover));
		icon_plus(a.x + 24, a.y + 24, 9,
			  blend(UI_ACCENT, UI_ACCENT_HOVER, hover), 2.2);
		if (hovered(a))
			tooltip(a, _("New group"), TRUE);
	}
}

static void sidebar_icon_button(Rect r, int kind, const char *tip,
				gboolean focus)
{
	gboolean hover = hovered(r);
	guint ink = hover || focus ? UI_HEADING : UI_MUTED;

	if (ui_theme_is_line()) {
		double hv = strength(HOVER, r, hover);

		if (hv > 0.)
			dot(r.x + r.w / 2., r.y + r.h / 2., r.w / 2.,
			    blend(UI_SIDEBAR, UI_HOVER, hv));
		if (kind == 0) {
			if (focus)
				active_ring(r);
			icon_plus(r.x + r.w / 2., r.y + r.h / 2., 7, ink, 2);
			if (hover)
				tooltip(r, tip, FALSE);
			return;
		}
	}
	if (focus)
		active_ring(r);
	if (kind == 0)
		icon_plus(r.x + r.w / 2., r.y + r.h / 2., 6, ink, 1.8);
	else
		icon_reload(r.x + r.w / 2., r.y + r.h / 2., ink);
	if (hover)
		tooltip(r, tip, FALSE);
}

static void user_panel(void)
{
	int s = sidebar_w(), h = ui_media_content_height(), rail = rail_w();
	Rect p = { rail, h - USER_PANEL_H, s - rail, USER_PANEL_H };

	box(p.x, p.y, p.w, p.h, 0, UI_PANEL);
	Rect me = ui_profile_self_area(p);
	double mh = strength(HOVER, me, hovered(me));

	if (mh > 0.)
		box(me.x, me.y, me.w, me.h, 6, blend(UI_PANEL, UI_INPUT, mh));
	avatar(ui.name ? ui.name : "?", p.x + 8, p.y + 10, 32,
	       ui.self_id ? ui.self_id : "self", ui.self_id,
	       ui_profile_self_avatar());
	box(p.x + 30, p.y + 32, 14, 14, 7,
	    mh > 0. ? blend(UI_PANEL, UI_INPUT, mh) : UI_PANEL);
	dot(p.x + 37, p.y + 39, 5, UI_ONLINE);
	text(ui.name ? ui.name : "", p.x + 48, p.y + 8, MAX(20, me.w - 48), 14,
	     600, UI_HEADING, FALSE);
	const char *status = ui_profile_self_status();

	text(status && *status ? status : _("Online"), p.x + 48, p.y + 27,
	     MAX(20, me.w - 48), 12, 450, UI_MUTED, FALSE);
	ui_profile_panel_draw();
	Rect r = ui.logout;
	double hv = strength(HOVER, r, hovered(r));

	if (hv > 0.)
		box(r.x, r.y, r.w, r.h, 6, blend(UI_PANEL, UI_INPUT, hv));
	if (ui.focus == F_LOGOUT)
		active_ring(r);
	icon_exit(r.x + 4, r.y + 4, hovered(r) ? UI_DANGER : UI_MUTED);
	if (hovered(r))
		tooltip((Rect){ r.x, r.y - 80, r.w, 34 }, _("Sign out"), FALSE);
}

static void pin_glyph(int px, int py)
{
	cairo_save(cr);
	cairo_translate(cr, px, py);
	cairo_rotate(cr, .6);
	color(UI_MUTED);
	cairo_set_line_width(cr, 1.4);
	cairo_new_path(cr);
	cairo_move_to(cr, -3, -5);
	cairo_line_to(cr, 3, -5);
	cairo_line_to(cr, 2, -1);
	cairo_line_to(cr, 4, 2);
	cairo_line_to(cr, -4, 2);
	cairo_line_to(cr, -2, -1);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_move_to(cr, 0, 2);
	cairo_line_to(cr, 0, 7);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/*
 * LINE lists each chat's latest message. The chat list RPC carries only the
 * message ID, so remember the newest text of every chat opened here and show
 * it while that message is still the chat's latest.
 */
struct preview {
	char *id, *text;
};

static GHashTable *previews;

static void drop_preview(gpointer data)
{
	struct preview *p = data;

	g_free(p->id);
	g_free(p->text);
	g_free(p);
}

static void remember_preview(void)
{
	size_t n = ui.history ? json_object_array_length(ui.history) : 0;

	if (!ui.selected || !n || ui.older_page)
		return;
	Json *m = json_object_array_get_idx(ui.history, n - 1);
	const char *id = ui_string(m, "id", NULL),
		   *body = ui_string(m, "text", NULL);

	if (!id || !*id || !body)
		return;
	if (!previews)
		previews = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, drop_preview);
	struct preview *old = g_hash_table_lookup(previews, ui.selected);

	if (old && !g_strcmp0(old->id, id))
		return;
	if (*body == '[')
		body = _(body);
	size_t len = strlen(body);
	/* Attachments arrive as "[Photo]"; LINE shows the bare word. */
	char *text = len > 2 && body[0] == '[' && body[len - 1] == ']' ?
			     g_strndup(body + 1, len - 2) :
			     g_strdup(body);

	g_strdelimit(text, "\r\n\t", ' ');
	struct preview *p = g_new(struct preview, 1);

	*p = (struct preview){ g_strdup(id), text };
	g_hash_table_insert(previews, g_strdup(ui.selected), p);
}

static const char *chat_preview(Json *c)
{
	struct preview *p =
		previews ?
			g_hash_table_lookup(previews, ui_string(c, "id", "")) :
			NULL;

	return p && !g_strcmp0(p->id, ui_string(c, "lastId", NULL)) ? p->text :
								      NULL;
}

static guint julian_day(GDateTime *t)
{
	GDate day;
	int y, m, d;

	g_date_time_get_ymd(t, &y, &m, &d);
	g_date_clear(&day, 1);
	g_date_set_dmy(&day, d, m, y);
	return g_date_get_julian(&day);
}

/* LINE's list clock: the time today, then Yesterday, a weekday, a date. */
static void list_stamp(gint64 ms, char *out, size_t size)
{
	*out = 0;
	if (ms <= 0)
		return;
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(ms / 1000);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();

	if (!when || !now)
		return;
	int days = (int)julian_day(now) - (int)julian_day(when);
	g_autofree char *s = NULL;

	if (days <= 0)
		s = g_date_time_format(when, "%H:%M");
	else if (days == 1)
		s = g_strdup(_("Yesterday"));
	else if (days < 7)
		s = ui_lang_date(when, "%a");
	else if (g_date_time_get_year(when) == g_date_time_get_year(now))
		s = ui_lang_date(when, _("%-m/%-d"));
	else
		s = ui_lang_date(when, _("%Y/%-m/%-d"));
	g_strlcpy(out, s ? s : "", size);
}

/*
 * A LINE chat row: a large avatar, the name over the latest message, the
 * time at the top right and the unread count under it.
 */
static void line_chat_row(Json *c, Rect row, gboolean selected, double sel,
			  double row_hover)
{
	const char *id = ui_string(c, "id", ""),
		   *name = ui_string(c, "name", id);
	int side = 46, right = row.x + row.w - 14;

	if (sel > 0. || row_hover > 0.)
		box(row.x, row.y, row.w, row.h, 0,
		    blend(blend(UI_SIDEBAR, UI_HOVER, row_hover), UI_SELECTED,
			  sel));
	if (sel > 0.) {
		double mark = (row.h - 22) * sel;

		box(row.x, row.y + (row.h - mark) / 2., 3, mark, 1.5,
		    blend(UI_SELECTED, UI_ACCENT, sel));
	}
	if (selected && ui.focus == F_LIST)
		outline((Rect){ row.x + 2, row.y + 1, row.w - 4, row.h - 2 }, 6,
			UI_ACCENT, 1);
	avatar(name, row.x + 14, row.y + (row.h - side) / 2, side, id, id,
	       ui_string(c, "avatarKey", NULL));
	gint64 unread = json_object_get_int64(ui_field(c, "unread"));
	char stamp[64];

	list_stamp(json_object_get_int64(ui_field(c, "time")), stamp,
		   sizeof(stamp));
	int stamp_w = ui_text_width(stamp, 11, 500);

	if (*stamp)
		text(stamp, right - stamp_w, row.y + 15, stamp_w + 2, 11, 500,
		     unread > 0 ? UI_ACCENT : UI_SUBTLE, FALSE);
	int name_x = row.x + 14 + side + 12;
	int name_right = right - (stamp_w ? stamp_w + 8 : 0);
	gboolean pinned = ui_list_pinned(id);

	if (pinned) {
		pin_glyph(name_right - 6, row.y + 21);
		name_right -= 16;
	}
	const char *saved = !g_strcmp0(ui.selected, id) ?
				    ui.message.value->str :
			    ui.drafts ? g_hash_table_lookup(ui.drafts, id) :
					NULL;
	gboolean draft = saved && *saved;
	const char *subtitle = draft ? saved : chat_preview(c);
	int badge = 0;

	if (unread > 0) {
		char count[4] = "99+";

		if (unread < 100)
			g_snprintf(count, sizeof(count), "%d", (int)unread);
		badge = unread < 10 ? 20 : unread < 100 ? 26 : 32;
		Rect pill = { right - badge, row.y + row.h - 30, badge, 20 };

		box(pill.x, pill.y, pill.w, pill.h, 10, UI_ACCENT);
		centered(count, pill, 12, 700, UI_ON_ACCENT);
	}
	guint ink = selected || unread > 0 ? UI_HEADING : UI_BODY;

	text(name, name_x, row.y + (subtitle ? 13 : (row.h - 20) / 2),
	     MAX(1, name_right - name_x), 15, unread > 0 ? 650 : 550, ink,
	     FALSE);
	if (!subtitle)
		return;
	int x = name_x, width = right - (badge ? badge + 10 : 0) - name_x;

	if (draft) {
		int dw = ui_text_width(_("Draft"), 13, 600) + 6;

		/* Exact width: right-to-left text aligns to its box's end. */
		text(_("Draft"), x, row.y + 37, dw - 6, 13, 600, UI_ACCENT,
		     FALSE);
		x += dw;
		width -= dw;
	}
	text(subtitle, x, row.y + 37, MAX(1, width), 13, 400, UI_MUTED, FALSE);
}

static void chats_panel(void)
{
	int s = sidebar_w(), h = ui.height, rail = rail_w();
	gboolean line_style = ui_theme_is_line();
	int row_h = ui_chat_row_height();

	box(0, 0, s, h, 0, UI_SIDEBAR);
	if (rail)
		server_rail();
	if (line_style) {
		text(_("Chats"), rail + 18, 15, ui.reload.x - rail - 26, 22,
		     700, UI_HEADING, FALSE);
		remember_preview();
	}
	draw_editor(&ui.search, ui.search_box, ui.focus == F_SEARCH,
		    line_style ? _("Search chats") :
				 _("Find or start a conversation"));
	if (!line_style)
		line(rail, HEADER_H, s, HEADER_H, UI_RAIL, 1);
	gboolean friends_open = ui_friends_active();

	if (ui.friends && (!line_style || !rail)) {
		Rect f = ui.friends_button;
		double hv = strength(HOVER, f, hovered(f));
		gboolean focus = ui.focus == F_FRIENDS;
		gboolean lit = hovered(f) || focus || friends_open;

		if (friends_open) {
			box(f.x, f.y, f.w, f.h, 6, UI_SELECTED);
			box(f.x, f.y + 11, 3, f.h - 22, 1.5, UI_ACCENT);
		} else if (hv > 0. || focus)
			box(f.x, f.y, f.w, f.h, 6,
			    blend(UI_SIDEBAR, UI_HOVER, focus ? 1. : hv));
		icon_people(f.x + 10, f.y + 9,
			    friends_open ? UI_ACCENT :
			    lit		 ? UI_HEADING :
					   UI_MUTED);
		text(_("Friends"), f.x + 44, f.y + 11, f.w - 54, 16, 500,
		     lit ? UI_HEADING : UI_MUTED, FALSE);
		int badge = ui_profile_badge();

		if (badge > 0) {
			g_autofree char *count =
				g_strdup_printf("%d", MIN(badge, 99));
			int bw = badge > 9 ? 22 : 16;
			Rect pill = { f.x + f.w - bw - 10, f.y + 13, bw, 16 };

			box(pill.x, pill.y, pill.w, pill.h, 8, UI_ACCENT);
			centered(count, pill, 12, 700, UI_ON_ACCENT);
		}
	}
	int heading_y = ui.list_box.y - 22;

	if (ui_lists_ready()) {
		Rect b = ui.folders_button;
		gboolean hv = hovered(b);

		const char *title = ui_folder_title();
		g_autofree char *upper = line_style ? NULL :
						      g_utf8_strup(title, -1);

		text(upper ? upper : title, b.x + 8,
		     heading_y - (line_style ? 1 : 0), b.w - 26,
		     line_style ? 13 : 12, line_style ? 600 : 650,
		     hv ? UI_HEADING : UI_MUTED, FALSE);
		cairo_new_path(cr);
		color(hv ? UI_HEADING : UI_MUTED);
		cairo_set_line_width(cr, 1.5);
		cairo_move_to(cr, b.x + b.w - 14, heading_y + 5);
		cairo_line_to(cr, b.x + b.w - 10, heading_y + 9);
		cairo_line_to(cr, b.x + b.w - 6, heading_y + 5);
		cairo_stroke(cr);
	} else
		text(line_style ? _("All chats") : _("DIRECT MESSAGES"),
		     rail + 16, heading_y, ui.reload.x - rail - 22, 12, 650,
		     UI_MUTED, FALSE);
	if (ui.groups)
		sidebar_icon_button(ui.new_group, 0, _("New group"),
				    ui.focus == F_GROUP_NEW);
	sidebar_icon_button(ui.reload, 1, _("Refresh"), ui.focus == F_RELOAD);
	Rect r = ui.list_box;

	cairo_save(cr);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_clip(cr);
	if (!ui.visible->len)
		text(ui.chats ?
			     (ui.search.value->len ? _("No matches") :
						     _("No chats available")) :
			     _("Loading chats…"),
		     rail + 18, r.y + 8, s - rail - 36, 14, 450, UI_MUTED,
		     FALSE);
	guint first = MAX(0, ui.list_offset / row_h);

	for (guint i = first; i < ui.visible->len; i++) {
		int y = r.y + (int)i * row_h - ui.list_offset;

		if (y >= r.y + r.h)
			break;
		Json *c = json_object_array_get_idx(
			ui.chats, g_array_index(ui.visible, guint, i));
		const char *id = ui_string(c, "id", ""),
			   *name = ui_string(c, "name", id);
		Rect row = line_style ? (Rect){ rail, y, s - rail, row_h } :
					(Rect){ rail + 8, y + 1, s - rail - 16, 42 };
		gboolean selected = ui_compact() && ui.focus == F_LIST ?
					    (int)i == ui.list_cursor :
					    !friends_open &&
						    !g_strcmp0(ui.selected, id);
		Rect track_key = { (int)g_str_hash(id), 0, 0,
				   0 }; /* stays fixed as the list scrolls */
		double sel = track_strength(SELECTED, track_key, selected, row),
		       row_hover = track_strength(HOVER, track_key,
						  hovered(row), row);

		if (line_style) {
			line_chat_row(c, row, selected, sel, row_hover);
			continue;
		}
		if (sel > 0. || row_hover > 0.)
			box(row.x, row.y, row.w, row.h, 6,
			    blend(blend(UI_SIDEBAR, UI_HOVER, row_hover),
				  UI_SELECTED, sel));
		if (sel > 0.) {
			double mark = (row.h - 22) * sel;

			box(row.x, row.y + (row.h - mark) / 2., 3, mark, 1.5,
			    blend(UI_SELECTED, UI_ACCENT, sel));
		}
		if (selected && ui.focus == F_LIST)
			outline(row, 6, UI_ACCENT, 1);
		avatar(name, row.x + 8, row.y + 5, 32, id, id,
		       ui_string(c, "avatarKey", NULL));
		gint64 unread = json_object_get_int64(ui_field(c, "unread"));
		int badge = 0;

		if (unread > 0) {
			char count[4] = "99+";

			if (unread < 100)
				g_snprintf(count, sizeof(count), "%d",
					   (int)unread);
			badge = unread < 10 ? 16 : unread < 100 ? 22 : 28;
			Rect pill = { row.x + row.w - 10 - badge, row.y + 13,
				      badge, 16 };

			box(pill.x, pill.y, pill.w, pill.h, 8, UI_ACCENT);
			centered(count, pill, 12, 700, UI_ON_ACCENT);
		}
		gboolean pinned = ui_list_pinned(id);

		if (pinned)
			pin_glyph(row.x + row.w - 16 - (badge ? badge + 8 : 0),
				  row.y + row.h / 2);
		guint ink = selected || unread > 0 ? UI_HEADING :
			    row_hover > .5	   ? UI_BODY :
						     UI_MUTED;

		text(name, row.x + 50, row.y + 11,
		     row.w - 60 - (badge ? badge + 8 : 0) - (pinned ? 16 : 0),
		     16, unread > 0 ? 600 : 500, ink, FALSE);
	}
	cairo_restore(cr);
	if (ui_compact() && ui.notice && *ui.notice)
		text(ui.notice, 20, h - 127, s - 40, 13, 500, UI_MUTED, TRUE);
	if (!line_nav())
		user_panel();
	if (line_style && !ui_compact())
		line(s - 1, 0, s - 1, h, UI_LINE, 1);
}

static void empty_chat(void)
{
	Rect r = ui.conversation;
	int x = r.x + r.w / 2, y = r.y + r.h / 2 - 40;

	ui_draw_image(logo, (Rect){ x - 32, y - 28, 64, 64 });
	centered(_("Select a chat"), (Rect){ r.x, y + 43, r.w, 35 }, 24, 650,
		 UI_HEADING);
}

enum {
	/* Grouped timeline */
	MESSAGE_AVATAR = 40,
	MESSAGE_LEFT = 72,
	GROUP_GAP = 17,
	DIVIDER_H = 32,
	INTRO_H = 196,
	/* Chat bubbles */
	CHAT_MARGIN = 16,
	CHAT_AVATAR = 36,
	CHAT_LEFT = CHAT_MARGIN + CHAT_AVATAR + 8,
	BUBBLE_PAD_X = 12,
	BUBBLE_PAD_Y = 8
};

struct message_layout {
	PangoLayout *body, *byline, *clock, *short_clock, *date, *reactions;
	struct ui_flex_layout *flex;
	int y, w, h, top; /* top: offset of the message below any divider */
	int x, body_top, body_h, text_origin;
	gboolean mine, group, photo, video, rich, receipt;
	Json *message; /* borrowed from the cached history */
};

static struct {
	GArray *rows;
	PangoLayout *sent, *read;
	int receipt_height;
	int width, height;
	double scale_x, scale_y;
	gboolean intro, line_style;
	Json *source; /* borrowed from ui.history */
} cache;

static void drop_bubble(gpointer data)
{
	struct message_layout *row = data;

	g_clear_object(&row->body);
	g_clear_object(&row->byline);
	g_clear_object(&row->clock);
	g_clear_object(&row->short_clock);
	g_clear_object(&row->date);
	g_clear_object(&row->reactions);
	ui_flex_free(row->flex);
}

void ui_forget_history(void)
{
	history_scroll.reusable = FALSE;
	g_clear_pointer(&cache.rows, g_array_unref);
	g_clear_object(&cache.sent);
	g_clear_object(&cache.read);
	cache.source = NULL;
	cache.height = 0;
}

void ui_draw_shutdown(void)
{
	ui_gpu_close();
	g_clear_pointer(&welcome_art, cairo_surface_destroy);
	g_clear_pointer(&logo, cairo_surface_destroy);
	ui_forget_rail();
	ui_forget_labels();
	ui_forget_history();
	g_clear_pointer(&previews, g_hash_table_destroy);
	g_clear_object(&context);
	ui_draw_reset_renderer();
	g_clear_pointer(&ui.renderer, SDL_DestroyRenderer);
	g_clear_pointer(&ui.buffer, SDL_FreeSurface);
}

/* "Today at 17:13", "Yesterday at 17:13" or a dated stamp, as in Discord. */
static char *message_stamp(GDateTime *dt)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	int days = g_date_time_get_year(now) * 400 +
		   g_date_time_get_day_of_year(now) -
		   (g_date_time_get_year(dt) * 400 +
		    g_date_time_get_day_of_year(dt));

	return g_date_time_format(dt, days == 0 ? _("Today at %H:%M") :
				      days == 1 ? _("Yesterday at %H:%M") :
						  _("%d/%m/%Y %H:%M"));
}

static gboolean show_intro(void)
{
	if (!ui.history || ui.has_older || ui.older_page)
		return FALSE;
	if (json_object_array_length(ui.history))
		return TRUE;
	/* An empty latest page can coexist with a known message in the chat list. */
	return !ui_read_id(ui_string(ui_selected_chat(), "lastId", NULL));
}

/* Whether a rich message's first scene draws anything from LINE's CDN. */
static gboolean rich_loadable(Json *rich)
{
	Json *scene = ui_field(ui_field(rich, "scenes"),
			       ui_string(rich, "scene", ""));
	Json *draws = ui_field(scene, "draws");

	for (size_t i = 0; draws && i < json_object_array_length(draws); i++)
		if (line_flex_image_allowed(ui_string(
			    json_object_array_get_idx(draws, i), "url", NULL)))
			return TRUE;
	return FALSE;
}

static void prepare_bubbles(Rect area)
{
	cairo_matrix_t matrix;
	gboolean line_style = ui_theme_is_line();
	gboolean intro = show_intro() &&
			 (!line_style || !json_object_array_length(ui.history));

	cairo_get_matrix(cr, &matrix);
	if (cache.rows && cache.source == ui.history && cache.width == area.w &&
	    cache.scale_x == matrix.xx && cache.scale_y == matrix.yy &&
	    cache.intro == intro && cache.line_style == line_style)
		return;
	ui_forget_history();
	size_t count = ui.history ? json_object_array_length(ui.history) : 0;

	cache.rows = g_array_sized_new(
		FALSE, FALSE, sizeof(struct message_layout), (guint)count);
	g_array_set_clear_func(cache.rows, drop_bubble);
	cache.source = ui.history;
	cache.width = area.w;
	cache.intro = intro;
	cache.line_style = line_style;
	cache.height = intro && !line_style ? INTRO_H : 8;
	cache.scale_x = matrix.xx;
	cache.scale_y = matrix.yy;
	const char *last = NULL;
	int last_date = 0;
	gint64 last_time = 0;
	int maxw = MAX(70, area.w - MESSAGE_LEFT - 24);

	/* Leave room beside either bubble for an avatar and a short clock. */
	if (line_style)
		maxw = MAX(1, MIN(520, MIN(area.w * 3 / 4,
					   area.w - CHAT_LEFT - 60)));
	for (size_t i = 0; i < count; i++) {
		Json *msg = json_object_array_get_idx(ui.history, i);
		const char *sid =
			ui_string(msg, "senderId", ui_string(msg, "from", ""));
		struct message_layout row = { .y = cache.height,
					      .message = msg };
		row.mine = json_object_get_boolean(ui_field(msg, "own"));
		row.receipt = row.mine && ui.selected &&
			      g_strcmp0(ui.selected, ui.self_id) &&
			      ui_read_id(ui_string(msg, "id", NULL));
		if (row.receipt && !cache.sent) {
			int sent_h, read_h;

			cache.sent = layout(_("Sent"), 11, 500, -1, FALSE);
			cache.read = layout(_("Read"), 11, 500, -1, FALSE);
			pango_layout_get_pixel_size(cache.sent, NULL, &sent_h);
			pango_layout_get_pixel_size(cache.read, NULL, &read_h);
			cache.receipt_height = MAX(sent_h, read_h);
		}
		const char *who = row.mine ? (ui.name ? ui.name : _("You")) :
					     ui_string(msg, "from", "LINE");
		gint64 stamp = json_object_get_int64(ui_field(msg, "time"));

		g_autoptr(GDateTime) dt =
			stamp > 0 ?
				g_date_time_new_from_unix_local(stamp / 1000) :
				NULL;
		int local_day =
			dt ? g_date_time_get_year(dt) * 10000 +
					g_date_time_get_month(dt) * 100 +
					g_date_time_get_day_of_month(dt) :
			     0;
		row.group =
			last && *sid && !strcmp(sid, last) &&
			!ui_field(msg, "replyTo") && local_day == last_date &&
			(!stamp ||
			 (stamp >= last_time && stamp - last_time < 420000));
		if (dt && local_day != last_date) {
			g_autofree char *date =
				ui_lang_date(dt, _("%B %e, %Y"));
			/* Unbounded, so the pill can centre RTL text too. */
			row.date = layout(date, 12, 600, -1, FALSE);
		}
		if (dt) {
			g_autofree char *hm = g_date_time_format(dt, "%H:%M");

			if (!line_style && !row.group) {
				g_autofree char *clock = message_stamp(dt);

				row.clock = layout(clock, 12, 500, 200, FALSE);
			}
			row.short_clock = layout(hm, 11, 500, 52, FALSE);
		}
		last_time = stamp;
		last_date = local_day;
		last = sid;
		if (!row.group && (!line_style || (!row.mine && ui.selected &&
						   *ui.selected != 'u')))
			row.byline = layout(who, line_style ? 12 : 16, 500,
					    maxw, FALSE);
		row.photo = ui_is_photo(msg);
		row.video = ui_is_video(msg);
		Json *flex = ui_field(msg, "flex");
		int body_height = 0;

		Json *rich = ui_field(msg, "rich");
		int rich_w = json_object_get_int(ui_field(rich, "w")),
		    rich_h = json_object_get_int(ui_field(rich, "h"));

		if (flex)
			row.flex = ui_flex_new(context, flex, maxw);
		if (!row.flex && rich_w > 0 && rich_h > 0 &&
		    rich_loadable(rich)) {
			/* Rich messages keep their canvas aspect, as on LINE. */
			row.rich = TRUE;
			row.w = MIN(maxw, 360);
			body_height = MIN(row.w * rich_h / rich_w, 720) + 4;
		} else if (row.flex) {
			row.w = ui_flex_width(row.flex);
			body_height = ui_flex_height(row.flex);
		} else if (row.photo || row.video) {
			row.w = MIN(maxw, 320);
			body_height = row.photo ? MIN(220, maxw * 2 / 3) :
						  MAX(74, row.w * 9 / 16);
			body_height += 4;
		} else {
			const char *body = ui_string(msg, "text", "");

			if (!*body)
				body = ui_string(msg, "filename",
						 ui_string(msg, "type",
							   "Message"));
			/* Attachments arrive labelled "[Photo]" and so on. */
			else if (*body == '[')
				body = _(body);
			g_autofree char *with_reply =
				ui_field(msg, "replyTo") ?
					g_strdup_printf("↪ Reply\n%s", body) :
					NULL;

			int text_w = line_style ?
					     MAX(1, maxw - 2 * BUBBLE_PAD_X) :
					     maxw;

			row.body = layout(with_reply ? with_reply : body,
					  line_style ? 15 : 16, 400, text_w,
					  TRUE);
			ui_emoji_shape_history(row.body, body,
					       with_reply ? "↪ Reply\n" : "",
					       ui_field(msg, "emojis"));
			PangoRectangle logical;

			pango_layout_get_pixel_extents(row.body, NULL,
						       &logical);
			body_height = logical.height;
			if (line_style) {
				/* Pango places RTL text at the far edge of its wrap width. */
				row.text_origin = -logical.x;
				row.w = MAX(1, logical.width) +
					2 * BUBBLE_PAD_X;
				body_height += 2 * BUBBLE_PAD_Y;
			} else
				row.w = MAX(32, logical.width);
		}
		Json *reactions = ui_field(msg, "reactions");

		if (reactions &&
		    json_object_is_type(reactions, json_type_object)) {
			const char *k[] = { "nice",    "love", "fun",
					    "amazing", "sad",  "omg" },
				   *v[] = { "👍", "♥", "😄", "✨", "😢", "😮" };
			g_autoptr(GString) line = g_string_new(NULL);
			for (guint j = 0; j < G_N_ELEMENTS(k); j++) {
				int n = json_object_get_int(
					ui_field(reactions, k[j]));
				if (n > 0)
					g_string_append_printf(
						line, "%s%s %d",
						line->len ? "   " : "", v[j],
						n);
			}
			if (line->len)
				row.reactions =
					layout(line->str, 14, 600, maxw, FALSE);
		}
		/* A new sender group gets the gap above its avatar row. */
		int gap = row.group || !i ? 0 : (line_style ? 10 : GROUP_GAP);

		if (row.date)
			gap = DIVIDER_H;
		row.top = gap;
		row.x = line_style ? (row.mine ? area.w - CHAT_MARGIN - row.w :
						 CHAT_LEFT) :
				     MESSAGE_LEFT;
		row.body_top =
			gap + 2 + (row.byline ? (line_style ? 18 : 22) : 0);
		if (line_style && row.receipt) {
			int clock_h = 0;

			if (row.short_clock)
				pango_layout_get_pixel_size(row.short_clock,
							    NULL, &clock_h);
			body_height = MAX(body_height,
					  cache.receipt_height + clock_h + 2);
		}
		row.body_h = body_height;
		row.h = row.body_top + body_height + (row.reactions ? 30 : 0);
		if (!line_style && row.receipt)
			row.h += cache.receipt_height + 2;
		if (line_style && !row.mine && !row.group)
			row.h = MAX(row.h, gap + 4 + CHAT_AVATAR);
		row.h += line_style ? 4 : 2;
		cache.height += row.h;
		g_array_append_val(cache.rows, row);
	}
	cache.height += 16;
}

/* Row bottoms increase monotonically even when media make row heights vary. */
static guint first_row_at(int pos)
{
	guint lo = 0, hi = cache.rows->len;

	while (lo < hi) {
		guint mid = lo + (hi - lo) / 2;
		const struct message_layout *r =
			&g_array_index(cache.rows, struct message_layout, mid);

		if (r->y + r->h <= pos)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static struct message_layout *bubble_at(int x, int y)
{
	Rect area = ui.conversation;

	if (!ui_chat_visible() || !ui.selected || !cache.rows ||
	    cache.source != ui.history || x < area.x || x >= area.x + area.w ||
	    y < area.y || y >= area.y + area.h)
		return NULL;
	int pos = y - area.y + ui.history_offset;

	guint i = first_row_at(pos);

	if (i == cache.rows->len)
		return NULL;
	struct message_layout *r =
		&g_array_index(cache.rows, struct message_layout, i);

	return pos >= r->y ? r : NULL;
}

static Rect message_rect(const struct message_layout *r, Rect area)
{
	return (Rect){ area.x + r->x,
		       area.y + r->y - ui.history_offset + r->body_top, r->w,
		       r->body_h };
}

static Rect reaction_rect(const struct message_layout *r, Rect body)
{
	int w;

	pango_layout_get_pixel_size(r->reactions, &w, NULL);
	w += 16;
	return (Rect){ cache.line_style && r->mine ? body.x + body.w - w :
						     body.x,
		       body.y + body.h + 4, w, 24 };
}

static gboolean contains_point(Rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

Json *ui_sender_at(int x, int y, Rect *anchor)
{
	struct message_layout *r = bubble_at(x, y);

	if (!r || r->group || (cache.line_style && r->mine))
		return NULL;
	Rect area = ui.conversation;
	int top = area.y + r->y - ui.history_offset + r->top + 2;
	int size = cache.line_style ? CHAT_AVATAR : MESSAGE_AVATAR;
	Rect face = { area.x + CHAT_MARGIN, top + 2, size, size };
	Rect name = { area.x + r->x, top, 0, r->body_top - r->top - 2 };

	if (r->byline)
		pango_layout_get_pixel_size(r->byline, &name.w, NULL);
	gboolean on_face = contains_point(face, x, y);

	if (!on_face && !contains_point(name, x, y))
		return NULL;
	if (anchor)
		*anchor = on_face ? face : name;
	return r->message;
}

int ui_message_at(int x, int y)
{
	struct message_layout *row = bubble_at(x, y);

	return row ? (int)(row - (struct message_layout *)cache.rows->data) :
		     -1;
}

Json *ui_row_at(int x, int y)
{
	struct message_layout *row = bubble_at(x, y);

	if (row && cache.line_style) {
		Rect body = message_rect(row, ui.conversation);

		if (!contains_point(body, x, y) &&
		    !(row->reactions &&
		      contains_point(reaction_rect(row, body), x, y)))
			return NULL;
	}
	return row ? row->message : NULL;
}

const char *ui_flex_link_at(int x, int y)
{
	struct message_layout *row = bubble_at(x, y);

	if (!row || !row->flex)
		return NULL;
	Rect body = message_rect(row, ui.conversation);

	return ui_flex_uri_at(row->flex, x - body.x, y - body.y);
}

static cairo_surface_t *flex_image(const char *url, gpointer unused)
{
	(void)unused;
	return ui_flex_image(url);
}

static void video_card(Json *row, Rect card)
{
	const int cx = card.x + card.w / 2, cy = card.y + card.h / 2;
	const gboolean hover = hovered(card);

	box(card.x, card.y, card.w, card.h, 6, UI_RAIL);
	cairo_save(cr);
	path(card.x, card.y, card.w, card.h, 6);
	cairo_clip(cr);
	ui_media_video_poster(row, card);
	cairo_restore(cr);
	outline(card, 6, hover ? UI_ACCENT : UI_LINE, 1);
	dot(cx, cy, 21, hover ? UI_ACCENT : UI_RAISED);
	cairo_new_path(cr);
	cairo_move_to(cr, cx - 4, cy - 8);
	cairo_line_to(cr, cx + 9, cy);
	cairo_line_to(cr, cx - 4, cy + 8);
	cairo_close_path(cr);
	color(hover ? UI_ON_ACCENT : UI_HEADING);
	cairo_fill(cr);

	gint64 duration = json_object_get_int64(ui_field(row, "durationMs"));
	char label[48];

	g_strlcpy(label, _("Video"), sizeof(label));
	if (duration > 0) {
		gint64 seconds = (duration + 999) / 1000;

		g_snprintf(label, sizeof(label),
			   "%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
			   seconds / 60, seconds % 60);
	}
	Rect badge = { card.x + card.w - 63, card.y + card.h - 25, 56, 19 };

	box(badge.x, badge.y, badge.w, badge.h, 4, UI_RAIL);
	centered(label, badge, 11, 600, UI_HEADING);
}

/* Discord opens a conversation with a large avatar and a short preface. */
static void conversation_intro(Rect area, int y)
{
	Json *chat = ui_selected_chat();
	const char *name = ui_string(chat, "name", ui.selected);
	gboolean group = ui.selected && *ui.selected == 'c';
	int x = area.x + 16;

	avatar(name, x, y + 16, 80, ui.selected, ui.selected,
	       ui_string(chat, "avatarKey", NULL));
	text(name, x, y + 108, area.w - 32, 30, 800, UI_HEADING, FALSE);
	g_autofree char *line =
		group ? g_strdup_printf(_("Welcome to the beginning of %s."),
					name) :
			g_strdup_printf(
				_("This is the beginning of your direct message history with %s."),
				name);
	text(line, x, y + 150, area.w - 32, 16, 400, UI_MUTED, FALSE);
}

/*
 * Rich messages: the scene each one shows. Transitions are local, as in
 * LINE's own client, and reset when the history is rebuilt.
 */
static GHashTable *rich_scenes;

static Json *rich_scene(Json *message)
{
	Json *rich = ui_field(message, "rich");
	const char *id = ui_string(message, "id", NULL);
	const char *name =
		id && rich_scenes ? g_hash_table_lookup(rich_scenes, id) : NULL;
	Json *scenes = ui_field(rich, "scenes");
	Json *scene = name ? ui_field(scenes, name) : NULL;

	return scene ? scene : ui_field(scenes, ui_string(rich, "scene", ""));
}

void ui_rich_set_scene(Json *message, const char *scene)
{
	const char *id = ui_string(message, "id", NULL);

	if (!id || !scene)
		return;
	if (!rich_scenes)
		rich_scenes = g_hash_table_new_full(g_str_hash, g_str_equal,
						    g_free, g_free);
	if (g_hash_table_size(rich_scenes) >= 64)
		g_hash_table_remove_all(rich_scenes);
	g_hash_table_replace(rich_scenes, g_strdup(id), g_strdup(scene));
	ui.dirty = TRUE;
}

static Rect rich_rect(Json *o, Rect r, double k)
{
	int x = json_object_get_int(ui_field(o, "x")),
	    y = json_object_get_int(ui_field(o, "y")),
	    w = json_object_get_int(ui_field(o, "w")),
	    h = json_object_get_int(ui_field(o, "h"));

	return (Rect){ r.x + (int)lround(x * k), r.y + (int)lround(y * k),
		       (int)lround(w * k), (int)lround(h * k) };
}

static void rich_draw(Json *message, Rect r)
{
	Json *rich = ui_field(message, "rich"), *scene = rich_scene(message);
	double k =
		(double)r.w / MAX(1, json_object_get_int(ui_field(rich, "w")));
	Json *draws = ui_field(scene, "draws"),
	     *areas = ui_field(scene, "areas");
	gboolean shown = FALSE, loadable = FALSE;

	cairo_save(cr);
	cairo_new_path(cr);
	path(r.x, r.y, r.w, r.h, 8);
	cairo_clip(cr);
	box(r.x, r.y, r.w, r.h, 0, UI_PANEL);
	for (size_t i = 0; draws && i < json_object_array_length(draws); i++) {
		Json *d = json_object_array_get_idx(draws, i);
		const char *url = ui_string(d, "url", NULL);

		cairo_surface_t *image = ui_flex_image(url);

		loadable |= line_flex_image_allowed(url) &&
			    !ui_flex_image_failed(url);

		if (!image)
			continue;
		Rect at = rich_rect(d, r, k);
		double iw = cairo_image_surface_get_width(image),
		       ih = cairo_image_surface_get_height(image);
		double sx = 0, sy = 0, sw = iw, sh = ih;

		if (!json_object_get_boolean(ui_field(d, "whole"))) {
			/* Base-image regions are in canvas units. */
			double f = iw / MAX(1, json_object_get_int(
						       ui_field(rich, "w")));
			sx = json_object_get_int(ui_field(d, "sx")) * f;
			sy = json_object_get_int(ui_field(d, "sy")) * f;
			sw = json_object_get_int(ui_field(d, "sw")) * f;
			sh = json_object_get_int(ui_field(d, "sh")) * f;
		}
		if (sw <= 0 || sh <= 0 || at.w <= 0 || at.h <= 0)
			continue;
		cairo_save(cr);
		cairo_rectangle(cr, at.x, at.y, at.w, at.h);
		cairo_clip(cr);
		cairo_translate(cr, at.x, at.y);
		cairo_scale(cr, at.w / sw, at.h / sh);
		cairo_set_source_surface(cr, image, -sx, -sy);
		cairo_pattern_set_filter(cairo_get_source(cr),
					 CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		shown = TRUE;
	}
	if (!shown)
		text(loadable ? _("Loading…") :
				ui_string(message, "text", "Rich message"),
		     r.x + 16, r.y + r.h / 2 - 10, r.w - 32, 15, 500, UI_MUTED,
		     FALSE);
	for (size_t i = 0; areas && i < json_object_array_length(areas); i++) {
		Json *a = json_object_array_get_idx(areas, i);
		Rect at = rich_rect(a, r, k);

		if (!hovered(at))
			continue;
		cairo_set_source_rgba(cr, 1, 1, 1, .12);
		cairo_rectangle(cr, at.x, at.y, at.w, at.h);
		cairo_fill(cr);
		break;
	}
	cairo_restore(cr);
	outline(r, 8, UI_LINE, 1);
	for (size_t i = 0; areas && i < json_object_array_length(areas); i++) {
		Json *a = json_object_array_get_idx(areas, i);
		Rect at = rich_rect(a, r, k);
		const char *kind = ui_string(a, "kind", "");
		const char *tip = ui_string(a, "label", NULL);

		if (!hovered(at))
			continue;
		if (!tip)
			tip = !strcmp(kind, "link") ?
				      ui_string(a, "value", "") :
			      !strcmp(kind, "message") ? "Send message" :
			      !strcmp(kind, "copy")    ? "Copy" :
							 NULL;
		if (tip && *tip)
			tooltip((Rect){ at.x, MAX(r.y, at.y), at.w, 0 }, tip,
				FALSE);
		break;
	}
}

/* The rich-message area under the pointer, and its message. */
Json *ui_rich_area_at(int x, int y, Json **message, int *key)
{
	struct message_layout *row = bubble_at(x, y);

	if (!row || !row->rich)
		return NULL;
	Rect r = message_rect(row, ui.conversation);

	r.y += 2;
	r.h -= 4;
	Json *rich = ui_field(row->message, "rich");
	double k =
		(double)r.w / MAX(1, json_object_get_int(ui_field(rich, "w")));
	Json *areas = ui_field(rich_scene(row->message), "areas");

	for (size_t i = 0; areas && i < json_object_array_length(areas); i++) {
		Json *a = json_object_array_get_idx(areas, i);
		Rect at = rich_rect(a, r, k);

		if (x >= at.x && x < at.x + at.w && y >= at.y &&
		    y < at.y + at.h) {
			if (message)
				*message = row->message;
			if (key)
				*key = (int)i;
			return a;
		}
	}
	return NULL;
}

static gboolean history_row_damaged(int top, int height)
{
	if (!history_damage.count)
		return TRUE;
	int y = top * history_scroll.scale;
	int end = (top + height) * history_scroll.scale;

	for (int i = 0; i < history_damage.count; i++) {
		SDL_Rect band = history_damage.bands[i];

		if (y < band.y + band.h && end > band.y)
			return TRUE;
	}
	return FALSE;
}

static void text_bubble(const struct message_layout *r, Rect body)
{
	guint fill = r->mine ? UI_BUBBLE_OUT : UI_BUBBLE_IN;

	box(body.x, body.y, body.w, body.h, MIN(18, body.h / 2), fill);
	if (!r->group) {
		int edge = r->mine ? body.x + body.w : body.x;
		int dir = r->mine ? 1 : -1;
		int y = body.y + 3;

		cairo_move_to(cr, edge - dir * 13, y);
		cairo_curve_to(cr, edge - dir * 5, y + 2, edge, y + 2,
			       edge + dir * 4, y);
		cairo_curve_to(cr, edge + dir * 3, y + 6, edge - dir * 2,
			       y + 10, edge - dir * 10, y + 13);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	paint(r->body, body.x + BUBBLE_PAD_X + r->text_origin,
	      body.y + BUBBLE_PAD_Y,
	      r->mine ? UI_BUBBLE_OUT_INK : UI_BUBBLE_IN_INK);
}

static void message_status(const struct message_layout *r, Rect body)
{
	int bottom = body.y + body.h - 1;

	if (cache.line_style && r->short_clock) {
		int w, h;

		pango_layout_get_pixel_size(r->short_clock, &w, &h);
		bottom -= h;
		paint(r->short_clock,
		      r->mine ? body.x - w - 6 : body.x + body.w + 6, bottom,
		      UI_SUBTLE);
	}
	if (!r->receipt)
		return;
	/* Live receipts update the label without reflowing the history. */
	PangoLayout *status = ui_message_is_read(r->message) ? cache.read :
							       cache.sent;
	int x = body.x, y = body.y + body.h + (r->reactions ? 30 : 0) + 2;

	if (cache.line_style) {
		int w, h;

		pango_layout_get_pixel_size(status, &w, &h);
		x -= w + 6;
		y = bottom - h - 1;
	}
	paint(status, x, y, UI_SUBTLE);
}

static void message_rows(Rect area)
{
	prepare_bubbles(area);
	ui.max_history_offset = MAX(0, cache.height - area.h);
	ui.history_offset = ui.stick_bottom ? ui.max_history_offset :
					      CLAMP(ui.history_offset, 0,
						    ui.max_history_offset);
	if (cache.intro && ui.history) {
		if (cache.line_style)
			centered(_("No messages yet"), area, 14, 400, UI_MUTED);
		else if (INTRO_H - ui.history_offset > 0)
			conversation_intro(area, area.y - ui.history_offset);
	}
	int hover = ui.hover_x >= area.x && ui.hover_x < area.x + area.w ?
			    ui_message_at(ui.hover_x, ui.hover_y) :
			    -1;

	for (guint i = first_row_at(ui.history_offset); i < cache.rows->len;
	     i++) {
		struct message_layout *r =
			&g_array_index(cache.rows, struct message_layout, i);
		int y = area.y + r->y - ui.history_offset;
		Rect body = message_rect(r, area);
		int x = body.x;

		if (y > area.y + area.h)
			break;
		if (!history_row_damaged(y - 2, MAX(r->h, r->top + 48) + 4))
			continue;
		if (r->date) {
			int tw;

			pango_layout_get_pixel_size(r->date, &tw, NULL);
			int left = area.x + (area.w - tw) / 2,
			    mid = y + r->top / 2 + 2;

			if (cache.line_style)
				box(left - 10, mid - 11, tw + 20, 22, 11,
				    UI_INPUT);
			else {
				line(area.x + 16, mid, area.x + area.w - 16,
				     mid, UI_LINE, 1);
				box(left - 6, mid - 9, tw + 12, 18, 0, UI_BG);
			}
			paint(r->date, left, mid - 8, UI_SUBTLE);
		}
		y += r->top;
		Rect bg = { area.x, y, area.w, r->h - r->top };

		if (!cache.line_style && (int)i == hover)
			box(bg.x, bg.y, bg.w, bg.h, 0, UI_PANEL);
		y += 2;
		if (!r->group && (!cache.line_style || !r->mine)) {
			const char *name =
				r->mine ? (ui.name ? ui.name : _("You")) :
					  ui_string(r->message, "from", "LINE");
			const char *sid = ui_string(r->message, "senderId",
						    r->mine ? ui.self_id : "");
			int size = cache.line_style ? CHAT_AVATAR :
						      MESSAGE_AVATAR;

			avatar(name, area.x + CHAT_MARGIN, y + 2, size,
			       sid ? sid : "", ui.selected,
			       ui_string(r->message, "avatarKey", NULL));
		}
		if (r->byline) {
			paint(r->byline, x, y,
			      cache.line_style ? UI_MUTED : UI_HEADING);
			if (r->clock) {
				int tw;

				pango_layout_get_pixel_size(r->byline, &tw,
							    NULL);
				if (x + tw + 120 < area.x + area.w)
					paint(r->clock, x + tw + 8, y + 4,
					      UI_SUBTLE);
			}
		} else if (!cache.line_style && (int)i == hover &&
			   r->short_clock)
			paint(r->short_clock, area.x + 22, y + 4, UI_SUBTLE);
		message_status(r, body);
		y = body.y;
		if (r->flex)
			ui_flex_draw(cr, r->flex, x, y, flex_image, NULL);
		else if (r->rich)
			rich_draw(r->message,
				  (Rect){ x, y + 2, r->w, r->body_h - 4 });
		else if (r->photo)
			ui_media_thumb(r->message,
				       (Rect){ x, y + 2, r->w, r->body_h - 4 });
		else if (r->video)
			video_card(r->message,
				   (Rect){ x, y + 2, r->w, r->body_h - 4 });
		else if (cache.line_style)
			text_bubble(r, body);
		else
			paint(r->body, x, y, UI_BODY);
		if (r->reactions) {
			int th;
			Rect pill = reaction_rect(r, body);

			pango_layout_get_pixel_size(r->reactions, NULL, &th);

			box(pill.x, pill.y, pill.w, pill.h, 8, UI_INPUT);
			outline(pill, 8, UI_LINE, 1);
			paint(r->reactions, pill.x + 8, pill.y + (24 - th) / 2,
			      UI_BODY);
		}
	}
	if (ui.history && !cache.rows->len && !cache.intro)
		centered(ui.older_page ?
				 _("No messages on this page.") :
				 _("LINE returned no messages on this device."),
			 area, 14, 400, UI_MUTED);
}

enum header_icon {
	ICON_CALL,
	ICON_VIDEO,
	ICON_SEARCH,
	ICON_MEMBERS,
	ICON_MORE
};

static void glyph_call(double x, double y)
{
	cairo_new_path(cr);
	cairo_move_to(cr, x + 5, y + 2.5);
	cairo_line_to(cr, x + 7.8, y + 2.5);
	cairo_line_to(cr, x + 10, y + 7.2);
	cairo_line_to(cr, x + 7.4, y + 9.7);
	cairo_curve_to(cr, x + 8.4, y + 12.5, x + 11.5, y + 15.8, x + 14.3,
		       y + 16.6);
	cairo_line_to(cr, x + 16.9, y + 14.1);
	cairo_line_to(cr, x + 21.5, y + 16.1);
	cairo_line_to(cr, x + 21.5, y + 19.1);
	cairo_curve_to(cr, x + 21.4, y + 21, x + 20, y + 22, x + 18.1,
		       y + 21.7);
	cairo_curve_to(cr, x + 9.7, y + 20.2, x + 3.9, y + 14, x + 2.5,
		       y + 6.2);
	cairo_curve_to(cr, x + 2.2, y + 4.2, x + 3.6, y + 2.5, x + 5, y + 2.5);
	cairo_fill(cr);
}

static void glyph_video(double x, double y)
{
	cairo_new_path(cr);
	path(x + 2, y + 6, 14, 12, 3);
	cairo_fill(cr);
	cairo_move_to(cr, x + 17, y + 10);
	cairo_line_to(cr, x + 22, y + 7);
	cairo_line_to(cr, x + 22, y + 17);
	cairo_line_to(cr, x + 17, y + 14);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* A 24 px glyph in a 32 px hit box, muted until hovered, with a tooltip. */
static void header_icon(Rect r, enum header_icon kind, gboolean focus)
{
	gboolean hover = hovered(r);
	guint ink = hover || focus ? UI_ACCENT_HOVER : UI_MUTED;
	double x = r.x + (r.w - 24) / 2., y = r.y + (r.h - 24) / 2.;
	const char *tips[] = { _("Start Voice Call"), _("Start Video Call"),
			       _("Search"), _("Member List"), _("More") };

	if (focus)
		active_ring(r);
	y += 2. * button_depth(r);
	color(ink);
	switch (kind) {
	case ICON_CALL:
		glyph_call(x, y);
		break;
	case ICON_VIDEO:
		glyph_video(x, y);
		break;
	case ICON_SEARCH:
		icon_search(x + 11, y + 11, ink, 6);
		break;
	case ICON_MEMBERS:
		icon_people(x, y, ink);
		break;
	case ICON_MORE:
		for (int i = -1; i <= 1; i++)
			dot(x + 12 + i * 6.5, y + 12, 2.2, ink);
		break;
	}
	if (hover)
		tooltip(r, tips[kind], FALSE);
}

static void glyph_smile(Rect e, guint ink)
{
	double x = e.x + e.w / 2., y = e.y + e.h / 2.;

	color(ink);
	cairo_set_line_width(cr, 1.8);
	cairo_new_path(cr);
	cairo_arc(cr, x, y, 9.5, 0, 2 * G_PI);
	cairo_stroke(cr);
	dot(x - 3.5, y - 2.5, 1.5, ink);
	dot(x + 3.5, y - 2.5, 1.5, ink);
	cairo_new_path(cr);
	cairo_arc(cr, x, y + .5, 5, .3, G_PI - .3);
	cairo_stroke(cr);
}

static void glyph_clip(Rect e, guint ink)
{
	cairo_save(cr);
	cairo_translate(cr, e.x + e.w / 2., e.y + e.h / 2.);
	cairo_rotate(cr, G_PI / 4);
	color(ink);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_path(cr);
	cairo_move_to(cr, 4, -4);
	cairo_line_to(cr, 4, 5);
	cairo_arc(cr, 0, 5, 4, 0, G_PI);
	cairo_line_to(cr, -4, -6);
	cairo_arc(cr, -1, -6, 3, G_PI, 2 * G_PI);
	cairo_line_to(cr, 2, 4);
	cairo_arc(cr, .5, 4, 1.5, 0, G_PI);
	cairo_line_to(cr, -1, -3);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/*
 * LINE's input pane: a flat band across the chat with the text on top, the
 * emoji and attachment tools under it and a Send button at the right.
 */
static void line_composer(void)
{
	Rect c = ui.composer;
	gboolean replying = ui.reply_id &&
			    !g_strcmp0(ui.reply_chat, ui.selected);

	box(c.x, c.y, c.w, c.h, 0, UI_SIDEBAR);
	if (replying) {
		Rect bar = { c.x, c.y - 34, c.w, 34 };

		box(bar.x, bar.y, bar.w, bar.h, 0, UI_DIALOG);
		box(bar.x, bar.y, 3, bar.h, 0, UI_ACCENT);
		text(_("Replying to message"), bar.x + 16, bar.y + 9,
		     bar.w - 140, 13, 500, UI_BODY, FALSE);
		text(_("Esc to cancel"), bar.x + bar.w - 112, bar.y + 10, 100,
		     12, 500, UI_SUBTLE, FALSE);
	}
	line(c.x, c.y - (replying ? 34 : 0), c.x + c.w,
	     c.y - (replying ? 34 : 0), UI_LINE, 1);
	draw_editor(&ui.message, ui.entry, ui.focus == F_MESSAGE,
		    _("Enter a message"));
	if (ui.emojis || ui.stickers) {
		Rect e = ui.emoji;
		double hv = strength(HOVER, e, hovered(e));

		if (hv > 0.)
			box(e.x, e.y, e.w, e.h, 8,
			    blend(UI_SIDEBAR, UI_HOVER, hv));
		glyph_smile(e, hv > .5 ? UI_HEADING : UI_NAV_INK);
		if (hovered(e))
			tooltip((Rect){ e.x, e.y - 82, e.w, 34 },
				ui.stickers && ui.emojis ?
					_("Stickers & Emoji") :
				ui.stickers ? _("Stickers") :
					      _("Emoji"),
				FALSE);
	}
	if (ui.photos || ui.files) {
		Rect a = ui.attach;
		double hv = strength(HOVER, a, hovered(a));

		if (hv > 0.)
			box(a.x, a.y, a.w, a.h, 8,
			    blend(UI_SIDEBAR, UI_HOVER, hv));
		glyph_clip(a, hv > .5 ? UI_HEADING : UI_NAV_INK);
		if (hovered(a))
			tooltip((Rect){ a.x, a.y - 82, a.w, 34 },
				_("Upload a File"), FALSE);
	}
	gboolean sending = g_hash_table_contains(ui.sending, ui.selected);
	gboolean ready = ui.message.value->len && !sending;
	Rect r = ui.send;
	double hv = strength(HOVER, r, ready && hovered(r));

	box(r.x, r.y, r.w, r.h, r.h / 2,
	    ready ? blend(UI_ACCENT, UI_ACCENT_HOVER, hv) : UI_INPUT);
	if (ui.focus == F_SEND)
		active_ring(r);
	centered(sending ? _("Sending…") : _("Send"), r, 13, 650,
		 ready ? UI_ON_ACCENT : UI_SUBTLE);
}

/* Discord's composer: one rounded bar with attach, text, emoji and send. */
static void composer(int s, int w)
{
	Rect c = ui.composer;

	if (ui_theme_is_line()) {
		line_composer();
		return;
	}
	Json *chat = ui_selected_chat();
	const char *name = ui_string(chat, "name", ui.selected);

	if (ui.reply_id && !g_strcmp0(ui.reply_chat, ui.selected)) {
		Rect bar = { c.x, c.y - 34, c.w, 42 };

		box(bar.x, bar.y, bar.w, bar.h, 8, UI_PANEL);
		text(_("Replying to message"), bar.x + 16, bar.y + 8,
		     bar.w - 140, 14, 500, UI_MUTED, FALSE);
		text(_("Esc to cancel"), bar.x + bar.w - 116, bar.y + 8, 104,
		     12, 500, UI_SUBTLE, FALSE);
	}
	box(c.x, c.y, c.w, c.h, 8, UI_INPUT);
	if (ui.photos || ui.files) {
		Rect a = ui.attach;
		double cx = a.x + a.w / 2., cy = a.y + a.h / 2.;

		dot(cx, cy, 12, hovered(a) ? UI_ACCENT_HOVER : UI_MUTED);
		icon_plus(cx, cy, 5.5, UI_INPUT, 2.4);
		if (hovered(a))
			tooltip((Rect){ a.x, a.y - 82, a.w, 34 },
				_("Upload a File"), FALSE);
	}
	g_autofree char *placeholder =
		ui_theme_is_line() ?
			NULL :
			g_strdup_printf(_("Message %s%s"),
					*ui.selected == 'c' ? "" : "@",
					name ? name : "");
	draw_editor(&ui.message, ui.entry, ui.focus == F_MESSAGE,
		    placeholder ? placeholder : _("Write a message"));
	if (ui.emojis || ui.stickers) {
		Rect e = ui.emoji;
		double x = e.x + e.w / 2., y = e.y + e.h / 2.;
		guint ink = hovered(e) ? UI_ACCENT_HOVER : UI_MUTED;

		color(ink);
		cairo_new_path(cr);
		cairo_arc(cr, x, y, 10, 0, 2 * G_PI);
		cairo_fill(cr);
		dot(x - 3.5, y - 2.5, 1.6, UI_INPUT);
		dot(x + 3.5, y - 2.5, 1.6, UI_INPUT);
		cairo_new_path(cr);
		cairo_set_line_width(cr, 1.8);
		color(UI_INPUT);
		cairo_arc(cr, x, y + .5, 5, .25, G_PI - .25);
		cairo_stroke(cr);
		if (hovered(e))
			tooltip((Rect){ e.x, e.y - 82, e.w, 34 },
				ui.stickers && ui.emojis ?
					_("Stickers & Emoji") :
				ui.stickers ? _("Stickers") :
					      _("Emoji"),
				FALSE);
	}
	gboolean sending = g_hash_table_contains(ui.sending, ui.selected);

	if (ui.message.value->len || sending || ui.focus == F_SEND) {
		Rect r = ui.send;
		double x = r.x + r.w / 2., y = r.y + r.h / 2.;
		guint ink = sending    ? UI_SUBTLE :
			    hovered(r) ? UI_ACCENT_HOVER :
					 UI_ACCENT;

		if (ui.focus == F_SEND)
			active_ring(r);
		color(ink);
		cairo_new_path(cr);
		cairo_move_to(cr, x - 9, y - 8);
		cairo_line_to(cr, x + 10, y);
		cairo_line_to(cr, x - 9, y + 8);
		cairo_line_to(cr, x - 6, y);
		cairo_close_path(cr);
		cairo_fill(cr);
		if (hovered(r))
			tooltip((Rect){ r.x, r.y - 82, r.w, 34 },
				sending ? _("Sending…") : _("Send"), FALSE);
	}
	(void)s;
	(void)w;
}

/* A one-to-one chat header opens the peer's profile, as in Discord. */
static Rect header_peer;

gboolean ui_header_peer_at(int x, int y, Rect *anchor)
{
	Rect r = header_peer;

	if (!ui.selected || *ui.selected != 'u' || !ui_chat_visible() ||
	    x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
		return FALSE;
	if (anchor)
		*anchor = r;
	return TRUE;
}

static void chat_view(void)
{
	int s = ui_compact() ? 0 : sidebar_w(), w = ui.width, h = ui.height;

	gboolean line_style = ui_theme_is_line();

	box(s, 0, w - s, h, 0, line_style ? UI_CHAT_BG : UI_BG);
	if (line_style)
		box(s, 0, w - s, HEADER_H, 0, UI_SIDEBAR);
	int left = s + (line_style ? 20 : 16);

	if (ui_compact() && ui.selected) {
		action(ui.back, _("Back"), FALSE, ui.focus == F_BACK);
		left = ui.back.x + ui.back.w + 12;
	}
	if (ui.selected) {
		Json *chat = ui_selected_chat();
		const char *name = ui_string(chat, "name", ui.selected);
		int right = ui.call.w	     ? ui.call.x :
			    ui.find_button.w ? ui.find_button.x :
			    ui.members.w     ? ui.members.x :
					       ui.chat_tools.x;

		/* LINE titles the chat with its name alone. */
		int name_x = line_style ? left : left + 32;

		if (!line_style)
			avatar(name, left, 12, 24, ui.selected, ui.selected,
			       ui_string(chat, "avatarKey", NULL));
		text(name, name_x, line_style ? 12 : 13,
		     MAX(12, right - name_x - 16), line_style ? 17 : 16,
		     line_style ? 700 : 600, UI_HEADING, FALSE);
		header_peer =
			*ui.selected == 'u' ?
				(Rect){ left, 8,
					MIN(240, MAX(12, right - left - 16)),
					32 } :
				(Rect){ 0 };
		header_icon(ui.chat_tools, ICON_MORE, FALSE);
	} else
		text(_("Messages"), left, 13, w - left - 16, 16, 600,
		     UI_HEADING, FALSE);
	if (line_style)
		line(s, HEADER_H, w, HEADER_H, UI_LINE, 1);
	else {
		line(s, HEADER_H, w, HEADER_H, UI_RAIL, 1);
		line(s, HEADER_H + 1, w, HEADER_H + 1, UI_PANEL, 1);
	}
	if (ui.find_messages && ui.selected && ui.find_button.w)
		header_icon(ui.find_button, ICON_SEARCH,
			    ui.focus == F_FIND_TEXT);
	if (ui.groups && ui.selected && *ui.selected == 'c' && ui.members.w)
		header_icon(ui.members, ICON_MEMBERS, ui.focus == F_MEMBERS);
	if (ui_media_can_call()) {
		header_icon(ui.call, ICON_CALL, FALSE);
		if (ui.video_calls)
			header_icon(ui.video, ICON_VIDEO, FALSE);
	}
	if (!ui.selected)
		empty_chat();
	else {
		Rect r = ui.conversation;

		if (ui.has_older)
			action(ui.older_button,
			       ui.older_pending ? _("Loading…") :
						  _("Earlier messages"),
			       FALSE, FALSE);
		if (ui.older_page)
			action(ui.latest_button, _("Back to latest"), FALSE,
			       FALSE);
		cairo_save(cr);
		cairo_rectangle(cr, r.x, r.y, r.w, r.h);
		cairo_clip(cr);
		message_rows(r);
		cairo_restore(cr);
		composer(s, w);
	}
	if (ui.notice && *ui.notice)
		text(ui.notice, s + 24,
		     ui.composer.y - (ui.reply_id ? 34 : 0) - 46, w - s - 46,
		     13, 500, UI_MUTED, TRUE);
	(void)h;
}

gboolean ui_draw_init(void)
{
	g_clear_pointer(&logo, cairo_surface_destroy);
	logo = ui_logo_load(ui.window);
	log_renderer = !g_strcmp0(g_getenv("LINE_RENDER_LOG"), "1");
	if (ui_gpu_active())
		return TRUE;
	if (g_strcmp0(g_getenv("LINE_DISABLE_GPU"), "1")) {
		int drivers = SDL_GetNumRenderDrivers();

		for (int i = 0; i < drivers; i++) {
			SDL_RendererInfo info;

			if (SDL_GetRenderDriverInfo(i, &info) ||
			    g_strcmp0(info.name, "vulkan"))
				continue;
			ui.renderer = SDL_CreateRenderer(
				ui.window, i,
				SDL_RENDERER_ACCELERATED |
					SDL_RENDERER_PRESENTVSYNC);
			if (!ui.renderer)
				ui.renderer = SDL_CreateRenderer(
					ui.window, i, SDL_RENDERER_ACCELERATED);
			if (ui.renderer &&
			    (SDL_GetRendererInfo(ui.renderer, &info) ||
			     g_strcmp0(info.name, "vulkan")))
				g_clear_pointer(&ui.renderer,
						SDL_DestroyRenderer);
			break;
		}
	}
	if (ui.renderer) {
		SDL_RendererInfo info;

		canvas_format = SDL_PIXELFORMAT_RGB888;
		if (!SDL_GetRendererInfo(ui.renderer, &info)) {
			/* Both formats match Cairo RGB24 in native byte order.
			 * Vulkan exposes ARGB8888 directly on little-endian Linux.
			 * Requesting an unsupported XRGB texture here makes SDL
			 * convert the entire dirty rectangle on every upload.
			 */
			for (Uint32 i = 0; i < info.num_texture_formats; i++) {
				if (info.texture_formats[i] ==
				    SDL_PIXELFORMAT_ARGB8888) {
					canvas_format =
						SDL_PIXELFORMAT_ARGB8888;
					break;
				}
			}
			if (log_renderer)
				g_printerr(
					"LINE renderer: %s on %s, accelerated=%s, vsync=%s\n",
					info.name, SDL_GetCurrentVideoDriver(),
					info.flags & SDL_RENDERER_ACCELERATED ?
						"yes" :
						"no",
					info.flags & SDL_RENDERER_PRESENTVSYNC ?
						"yes" :
						"no");
		}
		return TRUE;
	}
	SDL_Surface *surface = SDL_GetWindowSurface(ui.window);

	if (log_renderer && surface)
		g_printerr("LINE renderer: SDL window surface on %s\n",
			   SDL_GetCurrentVideoDriver());
	return surface != NULL;
}

void ui_draw_reset_renderer(void)
{
	history_scroll.reusable = FALSE;
	history_scroll.origin = 0;
	history_damage.count = 0;
	if (ui.renderer) {
		ui_media_reset_renderer();
		g_clear_pointer(&canvas_texture, SDL_DestroyTexture);
	}
	ui.dirty = TRUE;
}

static void disable_renderer(void)
{
	if (log_renderer)
		g_printerr(
			"LINE renderer: switching to SDL window surface (%s)\n",
			SDL_GetError());
	ui_draw_reset_renderer();
	g_clear_pointer(&ui.renderer, SDL_DestroyRenderer);
	g_clear_pointer(&ui.buffer, SDL_FreeSurface);
}

static SDL_Surface *backing_buffer(int width, int height)
{
	if (ui.buffer && (ui.buffer->w != width || ui.buffer->h != height))
		g_clear_pointer(&ui.buffer, SDL_FreeSurface);
	if (!ui.buffer)
		ui.buffer = SDL_CreateRGBSurfaceWithFormat(
			0, width, height, 32, SDL_PIXELFORMAT_RGB888);
	return ui.buffer;
}

static int copy_canvas(void)
{
	struct ui_scroll_copy copies[6];
	SDL_Rect canvas = { 0, 0, ui.buffer->w, ui.buffer->h };
	int count = ui_scroll_quads(canvas, history_scroll.area,
				    history_scroll.origin, copies);

	for (int i = 0; i < count; i++)
		if (SDL_RenderCopy(ui.renderer, canvas_texture, &copies[i].src,
				   &copies[i].dst))
			return -1;
	return 0;
}

static gboolean present_canvas(void)
{
	float sx = (float)ui.buffer->w / ui.width;
	float sy = (float)ui.buffer->h / ui.height;

	/* Cairo already drew the canvas in drawable pixels. Only live video
	 * rectangles still use window coordinates.
	 */
	if (SDL_RenderSetScale(ui.renderer, 1, 1) ||
	    SDL_RenderSetViewport(ui.renderer, NULL) || copy_canvas() ||
	    SDL_RenderSetScale(ui.renderer, sx, sy)) {
		disable_renderer();
		ui.dirty = TRUE;
		return FALSE;
	}
	ui_media_present_video(video_offset);
	SDL_RenderPresent(ui.renderer);
	return TRUE;
}

static int history_uploads(struct ui_scroll_copy spans[8])
{
	int count = 0;

	for (int i = 0; i < history_damage.count; i++)
		count += ui_scroll_slices(history_scroll.area,
					  history_scroll.origin,
					  history_damage.bands[i],
					  spans + count);
	return count;
}

static struct gpu_rect gpu_rect(SDL_Rect r)
{
	return (struct gpu_rect){ r.x, r.y, r.w, r.h };
}

static void native_scroll_scene(struct gpu_scene *scene)
{
	struct ui_scroll_copy spans[8];
	SDL_Rect canvas = { 0, 0, scene->width, scene->height };

	if (history_scroll.origin) {
		scene->copy_count = ui_scroll_quads(canvas, history_scroll.area,
						    history_scroll.origin,
						    spans);
		for (unsigned int i = 0; i < scene->copy_count; i++)
			scene->copies[i] = (struct gpu_copy){
				.src = gpu_rect(spans[i].src),
				.dst = gpu_rect(spans[i].dst),
			};
	}
	scene->upload_count = history_uploads(spans);
	for (unsigned int i = 0; i < scene->upload_count; i++)
		scene->uploads[i] = (struct gpu_copy){
			.src = gpu_rect(spans[i].dst),
			.dst = gpu_rect(spans[i].src),
		};
}

static gboolean present_native(SDL_Rect dirty)
{
	struct gpu_scene scene = {
		.pixels = ui.buffer->pixels,
		.width = ui.buffer->w,
		.height = ui.buffer->h,
		.pitch = ui.buffer->pitch,
		.damage = { dirty.x, dirty.y, dirty.w, dirty.h },
	};

	native_scroll_scene(&scene);
	ui_media_gpu_video(&scene, video_offset);
	int result = ui_gpu_draw(&scene);

	native_repaint = result <= 0;
	if (!native_repaint)
		return TRUE;
	/* A skipped acquire also skips this frame's texture updates. */
	history_scroll.reusable = FALSE;
	history_scroll.origin = 0;
	history_damage.count = 0;
	ui.dirty = TRUE;
	if (!result)
		return FALSE;
	/* A Vulkan window cannot provide an SDL2 software window surface. */
	ui_media_leave_fullscreen();
	int x, y, w, h, min_w, min_h;
	SDL_Window *old = ui.window;
	Uint32 flags = SDL_GetWindowFlags(old) &
		       (SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
			SDL_WINDOW_MAXIMIZED);

	SDL_GetWindowPosition(old, &x, &y);
	SDL_GetWindowSize(old, &w, &h);
	SDL_GetWindowMinimumSize(old, &min_w, &min_h);
	SDL_Window *next =
		SDL_CreateWindow(SDL_GetWindowTitle(old), x, y, w, h, flags);

	ui_gpu_close();
	ui_draw_reset_renderer();
	ui_media_reset_renderer();
	if (next) {
		ui.window = next;
		SDL_DestroyWindow(old);
		SDL_SetWindowMinimumSize(next, min_w, min_h);
		if (ui_draw_init())
			return FALSE;
	}
	g_printerr("Avernella: could not recover the display: %s\n",
		   SDL_GetError());
	ui.quitting = TRUE;
	return FALSE;
}

void ui_draw_video(void)
{
	int width, height;

	if (ui_gpu_active() && ui.buffer && !native_repaint) {
		ui_gpu_size(&width, &height);
		if (width == ui.buffer->w && height == ui.buffer->h) {
			present_native((SDL_Rect){ 0 });
			return;
		}
	}
	if (ui.renderer && canvas_texture &&
	    !SDL_GetRendererOutputSize(ui.renderer, &width, &height) &&
	    width == ui.buffer->w && height == ui.buffer->h)
		present_canvas();
	else
		ui_draw();
}

static gboolean reusable_history(SDL_Surface *target, SDL_Rect *pixels)
{
	Rect r = ui.conversation;

	if ((!ui.renderer && !ui_gpu_active()) || !ui.connected ||
	    !ui.selected || !ui.history || !ui_chat_visible() ||
	    ui_friends_active() || ui_media_layer() ||
	    ui_profile_toast_visible() || ui.stage_h || ui_emoji_deadline() ||
	    ui_animation_deadline(g_get_monotonic_time()) || !cache.rows ||
	    cache.source != ui.history || cache.width != r.w ||
	    ui.stick_bottom || target->h % ui.height || target->w % ui.width ||
	    r.h <= 0 || r.y < 0 || r.y + r.h > ui.height || r.x < 0 ||
	    r.x + r.w > ui.width || (ui.hover_x >= 0 && !hovered(r)) ||
	    ui.history_offset < 0 ||
	    ui.history_offset > MAX(0, cache.height - r.h))
		return FALSE;
	for (guint i = first_row_at(ui.history_offset); i < cache.rows->len;
	     i++) {
		struct message_layout *row =
			&g_array_index(cache.rows, struct message_layout, i);

		if (row->y >= ui.history_offset + r.h)
			break;
		/* These can paint animated pixels, delayed media or tooltips. */
		if (row->photo || row->video || row->rich || row->flex ||
		    ui_field(row->message, "emojis"))
			return FALSE;
	}
	int sx = target->w / ui.width, sy = target->h / ui.height;

	*pixels = (SDL_Rect){ r.x * sx, r.y * sy, r.w * sx, r.h * sy };
	return TRUE;
}

static void hover_damage(int index)
{
	if (index < 0 || (guint)index >= cache.rows->len)
		return;
	const struct message_layout *row =
		&g_array_index(cache.rows, struct message_layout, index);
	int scale = history_scroll.scale;
	SDL_Rect band = history_scroll.area;

	band.y += (row->y - ui.history_offset - 2) * scale;
	band.h = (MAX(row->h, row->top + 48) + 4) * scale;
	if (SDL_IntersectRect(&history_scroll.area, &band,
			      &history_damage.bands[history_damage.count]))
		history_damage.count++;
}

/* Union the full-width bands before emitting texture copy regions. */
static void merge_history_bands(void)
{
	SDL_Rect *bands = history_damage.bands;
	int count = history_damage.count;

	for (int i = 1; i < count; i++) {
		SDL_Rect band = bands[i];
		int j = i;

		while (j && bands[j - 1].y > band.y) {
			bands[j] = bands[j - 1];
			j--;
		}
		bands[j] = band;
	}
	int used = 1;

	for (int i = 1; i < count; i++) {
		SDL_Rect *prev = &bands[used - 1];

		if (bands[i].y <= prev->y + prev->h)
			prev->h =
				MAX(prev->h, bands[i].y + bands[i].h - prev->y);
		else
			bands[used++] = bands[i];
	}
	history_damage.count = used;
}

/* Cairo repaints only the entering scanlines and any changed hover highlight. */
static gboolean advance_history(SDL_Surface *target, SDL_Rect pixels)
{
	SDL_Rect area;

	if (!history_scroll.reusable || !reusable_history(target, &area) ||
	    !SDL_RectEquals(&pixels, &area) ||
	    !SDL_RectEquals(&history_scroll.area, &area))
		return FALSE;
	int dy = (ui.history_offset - history_scroll.offset) *
		 history_scroll.scale;

	if (!dy || ABS(dy) >= area.h)
		return FALSE;
	history_scroll.origin = (history_scroll.origin + dy + area.h) % area.h;
	/* The fixed header's antialiased edge touches the first chat row.
	 * Repair both edges and any edge pixels shifted into the interior.
	 */
	int guard = MIN(2 * history_scroll.scale, area.h);
	SDL_Rect exposed = area, edge = area;

	exposed.h = MIN(area.h, ABS(dy) + guard);
	edge.h = guard;
	if (dy > 0)
		exposed.y += area.h - exposed.h;
	else
		edge.y += area.h - guard;
	history_damage.bands[0] = exposed;
	history_damage.bands[1] = edge;
	history_damage.count = 2;
	int hover = ui_message_at(ui.hover_x, ui.hover_y);

	if (hover != history_scroll.hover) {
		hover_damage(history_scroll.hover);
		hover_damage(hover);
	}
	merge_history_bands();
	history_scroll.hover = hover;
	history_scroll.offset = ui.history_offset;
	return TRUE;
}

static int upload_canvas(SDL_Surface *target, SDL_Rect pixels)
{
	if (!history_damage.count) {
		const guint8 *data = (const guint8 *)target->pixels +
				     (size_t)pixels.y * target->pitch +
				     pixels.x * 4;
		return SDL_UpdateTexture(canvas_texture, &pixels, data,
					 target->pitch);
	}
	struct ui_scroll_copy spans[8];
	int count = history_uploads(spans);

	for (int i = 0; i < count; i++) {
		const guint8 *data = (const guint8 *)target->pixels +
				     (size_t)spans[i].dst.y * target->pitch +
				     spans[i].dst.x * 4;
		if (SDL_UpdateTexture(canvas_texture, &spans[i].src, data,
				      target->pitch))
			return -1;
	}
	return 0;
}

static void draw_frame(const Rect *region)
{
	static SDL_Surface *previous_surface;
	static void *previous_pixels;
	static int previous_width, previous_height;
	gboolean fullscreen = ui_media_fullscreen();
	gboolean painted_chat = FALSE;
	double p = 0., dy = 0.;
	int layer = 0;

	if (fullscreen)
		SDL_GetWindowSize(ui.window, &ui.width, &ui.height);
	else
		ui_layout();
	if (ui.width < 1 || ui.height < 1)
		return;
	SDL_Surface *surface = NULL, *target = NULL;
	gboolean retained = FALSE;

	if (ui_gpu_active()) {
		int width, height;

		ui_gpu_size(&width, &height);
		if (width < 1 || height < 1)
			return;
		retained = ui.buffer && ui.buffer->w == width &&
			   ui.buffer->h == height;
		target = backing_buffer(width, height);
		if (!target)
			return;
	} else if (ui.renderer) {
		int width, height;

		if (SDL_GetRendererOutputSize(ui.renderer, &width, &height))
			disable_renderer();
		else {
			if (width < 1 || height < 1)
				return;
			retained = canvas_texture && ui.buffer &&
				   ui.buffer->w == width &&
				   ui.buffer->h == height;
			if (!retained) {
				g_clear_pointer(&canvas_texture,
						SDL_DestroyTexture);
				canvas_texture = SDL_CreateTexture(
					ui.renderer, canvas_format,
					SDL_TEXTUREACCESS_STATIC, width,
					height);
				/* Cairo RGB24's fourth byte is padding. */
				if (canvas_texture &&
				    SDL_SetTextureBlendMode(canvas_texture,
							    SDL_BLENDMODE_NONE))
					g_clear_pointer(&canvas_texture,
							SDL_DestroyTexture);
			}
			if (canvas_texture)
				target = backing_buffer(width, height);
			if (!target)
				disable_renderer();
		}
	}
	if (!target && !ui.renderer) {
		surface = SDL_GetWindowSurface(ui.window);
		if (!surface)
			return;
		/* Some backends rotate the window pixels after presentation. */
		retained = previous_surface == surface &&
			   previous_pixels == surface->pixels &&
			   previous_width == surface->w &&
			   previous_height == surface->h &&
			   !g_strcmp0(SDL_GetCurrentVideoDriver(), "x11");
		previous_surface = surface;
		previous_pixels = surface->pixels;
		previous_width = surface->w;
		previous_height = surface->h;
		/* Draw directly when the surface matches Cairo's native RGB24. */
		if (!surface->format->Amask &&
		    surface->format->Rmask == 0x00ff0000 &&
		    surface->format->Gmask == 0x0000ff00 &&
		    surface->format->Bmask == 0x000000ff &&
		    surface->format->BytesPerPixel == 4)
			target = surface;
		else
			target = backing_buffer(surface->w, surface->h);
		if (!target)
			return;
	}
	if (SDL_MUSTLOCK(target) && SDL_LockSurface(target) < 0)
		return;
	gboolean gpu = ui.renderer || ui_gpu_active();
	gboolean partial = region && !fullscreen && ui.connected && retained &&
			   !(ui_gpu_active() && native_repaint);
	SDL_Rect pixels = { 0, 0, target->w, target->h };

	if (partial) {
		int left = MAX(region->x, 0), top = MAX(region->y, 0);
		int right = MIN(region->x + region->w, ui.width),
		    bottom = MIN(region->y + region->h, ui.height);
		if (right <= left || bottom <= top)
			partial = FALSE;
		else {
			pixels.x = (gint64)left * target->w / ui.width;
			pixels.y = (gint64)top * target->h / ui.height;
			pixels.w = ((gint64)right * target->w + ui.width - 1) /
					   ui.width -
				   pixels.x;
			pixels.h = ((gint64)bottom * target->h + ui.height -
				    1) / ui.height -
				   pixels.y;
		}
	}
	history_damage.count = 0;
	gboolean sparse = partial && gpu && advance_history(target, pixels);

	if (!sparse) {
		/* An unrelated repaint needs canonical canvas coordinates. */
		if (history_scroll.origin) {
			partial = FALSE;
			pixels = (SDL_Rect){ 0, 0, target->w, target->h };
		}
		history_scroll.origin = 0;
		history_scroll.reusable = FALSE;
	}
	cairo_surface_t *image = cairo_image_surface_create_for_data(
		target->pixels, CAIRO_FORMAT_RGB24, target->w, target->h,
		target->pitch);
	cr = cairo_create(image);
	double sx = (double)target->w / ui.width,
	       sy = (double)target->h / ui.height;
	if (partial) {
		if (sparse)
			for (int i = 0; i < history_damage.count; i++) {
				SDL_Rect band = history_damage.bands[i];

				cairo_rectangle(cr, band.x, band.y, band.w,
						band.h);
			}
		else
			cairo_rectangle(cr, pixels.x, pixels.y, pixels.w,
					pixels.h);
		cairo_clip(cr);
	}
	cairo_scale(cr, sx, sy);
	if (context && composited_text != gpu) {
		ui_forget_history();
		g_clear_object(&context);
	}
	if (!context || sx != text_scale_x || sy != text_scale_y) {
		ui_forget_labels();
		if (!context) {
			context = pango_cairo_create_context(cr);
			composited_text = gpu;
			if (gpu) {
				/* A scrolled or scaled texture has no physical LCD
				 * subpixel order. Use coverage stable across clips.
				 */
				cairo_font_options_t *opts =
					cairo_font_options_create();

				cairo_font_options_set_antialias(
					opts, CAIRO_ANTIALIAS_GRAY);
				pango_cairo_context_set_font_options(context,
								     opts);
				cairo_font_options_destroy(opts);
			}
		} else
			pango_cairo_update_context(cr, context);
		text_scale_x = sx;
		text_scale_y = sy;
		context_lang = NULL;
	}
	/*
	 * Tell Pango the interface language so Han characters take the
	 * Japanese, Korean, Simplified or Traditional forms the reader expects.
	 */
	if (context_lang != ui_lang_tag()) {
		context_lang = ui_lang_tag();
		pango_context_set_language(
			context, pango_language_from_string(context_lang));
		ui_forget_labels();
		ui_forget_history();
	}
	frame_us = g_get_monotonic_time();
	press.end = 0;
	press.damage = (SDL_Rect){ 0 };
	press.depth = ui_press_depth(press.timing, frame_us, &press.next);
	if (press.timing.up && !press.next)
		press.timing = (UiPress){ 0 };
	painting_layer = 0;
	ui_emoji_frame_start(frame_us, !partial);
	if (fullscreen) {
		ui_media_draw_fullscreen();
		goto finish_canvas;
	}
	int pane_key = !ui.connected	 ? 0 :
		       !ui_compact()	 ? 1 :
		       ui_chat_visible() ? 3 :
					   2;
	p = slide(&pane_slide, pane_key,
		  ui_compact() ? (pane_key == 2 ? -10. : 10.) : 0.);
	if (p != 0.) {
		color(UI_BG);
		cairo_paint(cr);
	}
	cairo_save(cr);
	cairo_translate(cr, p, 0);
	if (ui.connected && ui_friends_active()) {
		/* Like Discord, Friends opens in the main pane beside the list. */
		if (!ui_compact() && (!partial || region->x < sidebar_w()))
			chats_panel();
		ui_friends_draw();
	} else if (ui.connected) {
		if (ui_list_visible() &&
		    (!partial || region->x < sidebar_w() || ui_compact()))
			chats_panel();
		if (ui_chat_visible() &&
		    (!partial || region->x + region->w > sidebar_w() ||
		     ui_compact())) {
			chat_view();
			painted_chat =
				!partial ||
				(region->y <= ui.conversation.y &&
				 region->y + region->h >=
					 ui.conversation.y + ui.conversation.h);
		}
	} else
		sign_in();
	cairo_restore(cr);
	if (ui.connected)
		ui_media_draw_call_dock();
	/* Above the chat list and call dock, below any dialog. */
	flush_tooltip();
	layer = ui.connected ? ui_media_layer() : 0;

	scrim = strength(SCRIM, (Rect){ 0, 0, 1, 1 },
			 layer > 1 && layer != UI_LAYER_POPOUT &&
				 layer != UI_LAYER_INBOX);
	dy = slide(&dialog_slide, layer, layer ? 8. : 0.);

	if (layer) {
		painting_layer = layer;
		cairo_save(cr);
		cairo_translate(cr, 0, dy);
		ui_media_overlay();
		cairo_restore(cr);
	} else if (scrim > 0.)
		ui_scrim();
	flush_tooltip();
	if (ui.connected)
		ui_profile_toast_draw();
finish_canvas:
	cairo_destroy(cr);
	cairo_surface_flush(image);
	cairo_surface_destroy(image);
	if (SDL_MUSTLOCK(target))
		SDL_UnlockSurface(target);
	gboolean presented;

	if (ui_gpu_active()) {
		video_offset = dy;
		presented = present_native(pixels);
	} else if (ui.renderer) {
		video_offset = dy;
		if (upload_canvas(target, pixels)) {
			disable_renderer();
			ui.dirty = TRUE;
			return;
		}
		presented = present_canvas();
	} else {
		if (target != surface) {
			SDL_Rect dst = pixels;

			SDL_BlitSurface(target, partial ? &pixels : NULL,
					surface, partial ? &dst : NULL);
		}
		presented = partial && !SDL_UpdateWindowSurfaceRects(
					       ui.window, &pixels, 1);
		if (!presented)
			presented = !SDL_UpdateWindowSurface(ui.window);
	}
	if (gpu && !sparse && presented && painted_chat &&
	    reusable_history(target, &history_scroll.area)) {
		history_scroll.scale = target->h / ui.height;
		history_scroll.offset = ui.history_offset;
		history_scroll.hover = ui_message_at(ui.hover_x, ui.hover_y);
		history_scroll.reusable = TRUE;
	}
	history_damage.count = 0;
	if (painted_chat && presented)
		ui_conversation_presented(ui_friends_active() || layer ||
					  scrim > 0. || p != 0.);
}

void ui_draw(void)
{
	draw_frame(NULL);
}

void ui_draw_region(Rect logical)
{
	draw_frame(&logical);
}

void ui_draw_animation(void)
{
	SDL_Rect damage = press.damage;
	SDL_Rect list = { ui.list_box.x, ui.list_box.y, ui.list_box.w,
			  ui.list_box.h };

	/* Use the previous sample so the final, resting frame is repainted. */
	if (!ui.connected || ui_media_layer() || ui_friends_active() ||
	    ui_emoji_deadline() || pane_slide.end > frame_us ||
	    dialog_slide.end > frame_us || stage_motion.end > frame_us)
		goto full;
	for (guint i = 0; i < G_N_ELEMENTS(tracks); i++) {
		const struct color_transition *t = &tracks[i];
		Rect r = t->damage;

		if (!t->kind || t->end <= frame_us)
			continue;
		if (!r.w)
			goto full;
		SDL_Rect row = { r.x - 2, r.y - 2, r.w + 4, r.h + 4 };

		if (SDL_IntersectRect(&row, &list, &row))
			SDL_UnionRect(&damage, &row, &damage);
	}
	if (!SDL_RectEmpty(&damage)) {
		ui_draw_region(
			(Rect){ damage.x, damage.y, damage.w, damage.h });
		return;
	}
full:
	ui_draw();
}

void ui_draw_box(Rect r, guint rgb)
{
	if (rgb == UI_DIALOG && r.w >= 160 && r.h >= 100) {
		box(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 13, UI_LINE);
		box(r.x, r.y, r.w, r.h, 12, rgb);
		return;
	}
	box(r.x, r.y, r.w, r.h, 0, rgb);
}

void ui_draw_rounded(Rect r, int radius, guint rgb)
{
	box(r.x, r.y, r.w, r.h, radius, rgb);
}

void ui_draw_avatar(const char *name, const char *seed, Rect r)
{
	avatar(name, r.x, r.y, MIN(r.w, r.h), seed ? seed : "", ui.selected,
	       NULL);
}

int ui_content_x(void)
{
	return ui_compact() ? 0 : sidebar_w();
}

void ui_draw_focus(Rect r)
{
	active_ring(r);
}

/* 24 px glyphs centered in r. */
void ui_draw_icon(Rect r, enum ui_icon icon, guint ink)
{
	double x = r.x + (r.w - 24) / 2., y = r.y + (r.h - 24) / 2.;

	color(ink);
	switch (icon) {
	case UI_ICON_PEOPLE:
		icon_people(x, y, ink);
		break;
	case UI_ICON_MESSAGE:
		cairo_new_path(cr);
		path(x + 3, y + 4, 18, 13, 4);
		cairo_fill(cr);
		cairo_move_to(cr, x + 7, y + 15);
		cairo_line_to(cr, x + 7, y + 21);
		cairo_line_to(cr, x + 13, y + 16);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case UI_ICON_MORE:
		for (int i = -1; i <= 1; i++)
			dot(x + 12, y + 12 + i * 6, 2.2, ink);
		break;
	case UI_ICON_GEAR:
		/* Eight teeth around a ring, as Discord's settings cog. */
		cairo_new_path(cr);
		cairo_set_line_width(cr, 3.2);
		for (int i = 0; i < 8; i++) {
			double a = i * G_PI / 4;

			cairo_move_to(cr, x + 12 + cos(a) * 6.5,
				      y + 12 + sin(a) * 6.5);
			cairo_line_to(cr, x + 12 + cos(a) * 9.5,
				      y + 12 + sin(a) * 9.5);
		}
		cairo_stroke(cr);
		cairo_new_path(cr);
		cairo_set_line_width(cr, 2.6);
		cairo_arc(cr, x + 12, y + 12, 5.6, 0, 2 * G_PI);
		cairo_stroke(cr);
		break;
	case UI_ICON_INBOX:
		cairo_new_path(cr);
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, x + 4, y + 13);
		cairo_line_to(cr, x + 7, y + 5);
		cairo_line_to(cr, x + 17, y + 5);
		cairo_line_to(cr, x + 20, y + 13);
		cairo_line_to(cr, x + 20, y + 19);
		cairo_line_to(cr, x + 4, y + 19);
		cairo_close_path(cr);
		cairo_move_to(cr, x + 4, y + 13);
		cairo_line_to(cr, x + 9, y + 13);
		cairo_line_to(cr, x + 10, y + 15);
		cairo_line_to(cr, x + 14, y + 15);
		cairo_line_to(cr, x + 15, y + 13);
		cairo_line_to(cr, x + 20, y + 13);
		cairo_stroke(cr);
		break;
	case UI_ICON_CHECK:
		cairo_new_path(cr);
		cairo_set_line_width(cr, 2.4);
		cairo_move_to(cr, x + 6, y + 12.5);
		cairo_line_to(cr, x + 10.5, y + 17);
		cairo_line_to(cr, x + 18.5, y + 8);
		cairo_stroke(cr);
		break;
	case UI_ICON_CLOSE:
		cairo_new_path(cr);
		cairo_set_line_width(cr, 2.2);
		cairo_move_to(cr, x + 7, y + 7);
		cairo_line_to(cr, x + 17, y + 17);
		cairo_move_to(cr, x + 17, y + 7);
		cairo_line_to(cr, x + 7, y + 17);
		cairo_stroke(cr);
		break;
	}
}

double ui_pixel_scale(void)
{
	return MAX(1., MAX(text_scale_x, text_scale_y));
}

int ui_text_width(const char *s, int size, int weight)
{
	if (!context || !s || !*s)
		return 0;
	PangoLayout *item = layout(s, size, weight, -1, FALSE);
	int w;

	pango_layout_get_pixel_size(item, &w, NULL);
	g_object_unref(item);
	return w;
}

int ui_text_height(const char *s, int w, int size, int weight)
{
	if (!context || !s || !*s)
		return 0;
	PangoLayout *item = layout(s, size, weight, MAX(1, w), TRUE);
	int h;

	pango_layout_get_pixel_size(item, NULL, &h);
	g_object_unref(item);
	return h;
}

void ui_draw_outline(Rect r, int radius, guint rgb)
{
	outline(r, radius, rgb, 1);
}

double ui_draw_hover(Rect r)
{
	return strength(HOVER, r, hovered(r));
}

guint ui_blend(guint a, guint b, double v)
{
	return blend(a, b, v);
}

void ui_draw_switch(Rect r, gboolean on)
{
	double v = strength(SELECTED, r, on);
	guint track = blend(UI_SUBTLE, UI_ACCENT, v);

	box(r.x, r.y, r.w, r.h, r.h / 2., track);
	double knob = r.h - 6, left = r.x + 3 + v * (r.w - knob - 6);

	box(left, r.y + 3, knob, knob, knob / 2, UI_ON_ACCENT);
	color(track);
	cairo_new_path(cr);
	cairo_set_line_width(cr, 1.8);
	double cx = left + knob / 2, cy = r.y + r.h / 2.;

	if (v > .5) {
		cairo_move_to(cr, cx - 4, cy);
		cairo_line_to(cr, cx - 1, cy + 3);
		cairo_line_to(cr, cx + 4, cy - 3);
	} else {
		cairo_move_to(cr, cx - 3, cy - 3);
		cairo_line_to(cr, cx + 3, cy + 3);
		cairo_move_to(cr, cx + 3, cy - 3);
		cairo_line_to(cr, cx - 3, cy + 3);
	}
	cairo_stroke(cr);
}

void ui_draw_centered(const char *s, Rect r, int size, int weight, guint rgb)
{
	centered(s ? s : "", r, size, weight, rgb);
}

void ui_draw_tooltip(Rect anchor, const char *s)
{
	tooltip(anchor, s, FALSE);
}

void ui_draw_tooltip_beside(Rect anchor, const char *s)
{
	tooltip(anchor, s, TRUE);
}

static void glyph_mic(double x, double y)
{
	cairo_new_path(cr);
	path(x + 8, y + 2, 8, 13, 4);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_set_line_width(cr, 2);
	cairo_arc(cr, x + 12, y + 11, 7, 0, G_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, x + 12, y + 18);
	cairo_line_to(cr, x + 12, y + 22);
	cairo_move_to(cr, x + 8, y + 22);
	cairo_line_to(cr, x + 16, y + 22);
	cairo_stroke(cr);
}

/* A Discord voice control: a 56 px circle with a centered 24 px glyph. */
void ui_draw_round_button(Rect r, enum ui_call_glyph glyph, guint fill,
			  guint ink, const char *tip)
{
	double hover = strength(HOVER, r, hovered(r));
	double depth = button_depth(r);
	double x = r.x + (r.w - 24) / 2., y = r.y + (r.h - 24) / 2.;
	gboolean slash = glyph == UI_GLYPH_CAMERA_OFF ||
			 glyph == UI_GLYPH_MIC_OFF;

	y += 1.5 * depth;
	box(r.x + 2. * depth, r.y + 2. * depth, r.w - 4. * depth,
	    r.h - 4. * depth, MIN(r.w, r.h) / 2.,
	    blend(fill, blend(fill, WHITE, .35), hover * .4));
	color(ink);
	switch (glyph) {
	case UI_GLYPH_CAMERA:
	case UI_GLYPH_CAMERA_OFF:
		glyph_video(x, y);
		break;
	case UI_GLYPH_MIC:
	case UI_GLYPH_MIC_OFF:
		glyph_mic(x, y);
		break;
	case UI_GLYPH_HANG_UP:
		cairo_save(cr);
		cairo_translate(cr, x + 12, y + 12);
		cairo_rotate(cr, G_PI * .75);
		glyph_call(-12, -12);
		cairo_restore(cr);
		break;
	case UI_GLYPH_ACCEPT:
		glyph_call(x, y);
		break;
	case UI_GLYPH_CLOSE:
		line(x + 6, y + 6, x + 18, y + 18, ink, 2.4);
		line(x + 18, y + 6, x + 6, y + 18, ink, 2.4);
		break;
	case UI_GLYPH_MINIMIZE:
		line(x + 6, y + 12, x + 18, y + 12, ink, 2.4);
		break;
	case UI_GLYPH_SCREEN:
	case UI_GLYPH_SCREEN_STOP:
		/* A monitor on a stand with an arrow (or a cross) inside. */
		cairo_set_line_width(cr, 2);
		cairo_new_sub_path(cr);
		cairo_arc(cr, x + 4.5, y + 5.5, 2.5, G_PI, 1.5 * G_PI);
		cairo_arc(cr, x + 19.5, y + 5.5, 2.5, 1.5 * G_PI, 2 * G_PI);
		cairo_arc(cr, x + 19.5, y + 15.5, 2.5, 0, .5 * G_PI);
		cairo_arc(cr, x + 4.5, y + 15.5, 2.5, .5 * G_PI, G_PI);
		cairo_close_path(cr);
		cairo_stroke(cr);
		line(x + 12, y + 18, x + 12, y + 21, ink, 2);
		line(x + 8, y + 21.5, x + 16, y + 21.5, ink, 2);
		if (glyph == UI_GLYPH_SCREEN) {
			line(x + 12, y + 14, x + 12, y + 7.5, ink, 2);
			line(x + 9, y + 10.5, x + 12, y + 7.5, ink, 2);
			line(x + 15, y + 10.5, x + 12, y + 7.5, ink, 2);
		} else {
			line(x + 9, y + 7.5, x + 15, y + 13.5, ink, 2);
			line(x + 15, y + 7.5, x + 9, y + 13.5, ink, 2);
		}
		break;
	}
	if (slash) {
		line(x + 2, y + 2, x + 22, y + 22, fill, 5);
		line(x + 2, y + 2, x + 22, y + 22,
		     fill == UI_ACCENT ? UI_ON_ACCENT : UI_DANGER, 2.4);
	}
	if (tip && hovered(r))
		tooltip((Rect){ r.x, r.y - 92, r.w, 34 }, tip, FALSE);
}

void ui_draw_button(Rect r, const char *s, gboolean primary)
{
	action(r, s, primary, FALSE);
}

void ui_draw_label(const char *s, Rect r, int size, int weight, guint rgb,
		   gboolean wrap)
{
	cairo_save(cr);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_clip(cr);
	text(s ? s : "", r.x, r.y, r.w, size, weight, rgb, wrap);
	cairo_restore(cr);
}

static void draw_image(cairo_surface_t *image, Rect r, unsigned int angle,
		       cairo_filter_t filter)
{
	if (!image || r.w <= 0 || r.h <= 0)
		return;
	int w = cairo_image_surface_get_width(image),
	    h = cairo_image_surface_get_height(image);
	gboolean sideways = angle == 90 || angle == 270;
	double z = MIN((double)r.w / (sideways ? h : w),
		       (double)r.h / (sideways ? w : h));

	if (z <= 0)
		return;
	cairo_save(cr);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_clip(cr);
	cairo_translate(cr, r.x + r.w / 2., r.y + r.h / 2.);
	if (angle)
		cairo_rotate(cr, angle * (G_PI / 180.));
	cairo_scale(cr, z, z);
	cairo_set_source_surface(cr, image, -w / 2., -h / 2.);
	cairo_pattern_set_filter(cairo_get_source(cr), filter);
	cairo_paint(cr);
	cairo_restore(cr);
}

void ui_draw_rotated_image(cairo_surface_t *image, Rect r, unsigned int angle)
{
	draw_image(image, r, angle, CAIRO_FILTER_BILINEAR);
}

void ui_draw_image(cairo_surface_t *image, Rect r)
{
	draw_image(image, r, 0, CAIRO_FILTER_BILINEAR);
}

void ui_draw_path_entry(Editor *editor, Rect r)
{
	draw_editor(editor, r, TRUE, _("Path"));
}

void ui_scrim(void)
{
	cairo_set_source_rgba(cr, 0, 0, 0, .65 * scrim);
	cairo_paint(cr);
}

void ui_draw_edit(Editor *editor, Rect r, gboolean focus,
		  const char *placeholder)
{
	draw_editor(editor, r, focus, placeholder);
}

void ui_draw_profile(Json *row, Rect r)
{
	const char *id = ui_string(row, "id", ""),
		   *name = ui_string(row, "name", "LINE");
	avatar(name, r.x, r.y, r.w, id, id, ui_string(row, "avatarKey", NULL));
}

void ui_draw_code(QRcode *qr, int cx, int top, int bound)
{
	if (!qr)
		return;
	int step = MAX(1, bound / (qr->width + 8)),
	    side = (qr->width + 8) * step, x = cx - side / 2;
	box(x, top, side, side, 0, WHITE);
	color(0x10181c);
	cairo_new_path(cr);
	for (int y = 0; y < qr->width; y++)
		for (int col = 0; col < qr->width; col++)
			if (qr->data[y * qr->width + col] & 1)
				cairo_rectangle(cr, x + (col + 4) * step,
						top + (y + 4) * step, step,
						step);
	cairo_fill(cr);
}

PangoLayout *ui_result_layout(const char *body, int width)
{
	return layout(body, 15, 400, width, TRUE);
}

void ui_result_paint(PangoLayout *body, int x, int y, Rect clip)
{
	cairo_save(cr);
	cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
	cairo_clip(cr);
	paint(body, x, y, UI_HEADING);
	cairo_restore(cr);
}
