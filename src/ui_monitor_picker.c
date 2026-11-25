// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor_picker.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include <SDL.h>
#include <pango/pangocairo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

enum {
	WIDTH = 540,
	MARGIN = 18,
	LIST_TOP = 82,
	ROW = 44,
	MAX_ROWS = 6,
	FOOTER = 60,
	CANCEL = -2,
	SHARE = -3,
};

struct picker {
	SDL_Window *window;
	int count, selected, first, rows, height, pressed;
	char **names;
};

static void fill(cairo_t *cr, int x, int y, int w, int h, guint rgb)
{
	cairo_set_source_rgb(cr, (rgb >> 16) / 255., (rgb >> 8 & 255) / 255.,
			     (rgb & 255) / 255.);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void label(cairo_t *cr, const char *text, int x, int y, int width,
		  int size, guint rgb, gboolean centered)
{
	g_autoptr(PangoLayout) layout = pango_cairo_create_layout(cr);
	g_autoptr(PangoFontDescription) font =
		pango_font_description_from_string("Sans");

	pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	if (centered)
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	cairo_set_source_rgb(cr, (rgb >> 16) / 255., (rgb >> 8 & 255) / 255.,
			     (rgb & 255) / 255.);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
}

/* The chooser repaints only on exposure, a selection change or scrolling. */
static gboolean draw(const struct picker *p)
{
	SDL_Surface *window = SDL_GetWindowSurface(p->window);

	if (!window)
		return FALSE;
	cairo_surface_t *image = cairo_image_surface_create(
		CAIRO_FORMAT_RGB24, window->w, window->h);
	cairo_t *cr = cairo_create(image);

	cairo_scale(cr, (double)window->w / WIDTH,
		    (double)window->h / p->height);
	fill(cr, 0, 0, WIDTH, p->height, UI_PANEL);
	label(cr, _("Choose a monitor to share"), MARGIN, 12,
	      WIDTH - 2 * MARGIN, 21, UI_HEADING, FALSE);
	g_autofree char *question =
		p->selected >= 0 ?
			g_strdup_printf(
				_("Share the entire monitor “%s” with everyone in this call?"),
				p->names[p->selected]) :
			NULL;

	label(cr,
	      question ?
		      question :
		      _("Select the monitor you want everyone in this call to see."),
	      MARGIN, 48, WIDTH - 2 * MARGIN, 14, UI_MUTED, FALSE);
	int list_w = WIDTH - 2 * MARGIN - (p->rows < p->count ? 8 : 0);

	for (int row = 0; row < p->rows; row++) {
		int i = row + p->first, y = LIST_TOP + row * ROW;

		fill(cr, MARGIN, y, list_w, ROW - 3,
		     i == p->selected ? UI_SELECTED : UI_INPUT);
		if (i == p->selected)
			fill(cr, MARGIN, y, 3, ROW - 3, UI_ACCENT);
		label(cr, p->names[i], MARGIN + 12, y + 9, list_w - 24, 16,
		      UI_BODY, FALSE);
	}
	if (p->rows < p->count) {
		int h = p->rows * ROW;

		fill(cr, WIDTH - MARGIN - 4, LIST_TOP + p->first * h / p->count,
		     4, p->rows * h / p->count, UI_SUBTLE);
	}
	int y = p->height - FOOTER + 8;

	fill(cr, MARGIN, y, 160, 36, UI_RAISED);
	label(cr, _("Cancel"), MARGIN + 4, y + 7, 152, 15, UI_BODY, TRUE);
	fill(cr, WIDTH - MARGIN - 210, y, 210, 36,
	     p->selected < 0 ? UI_INPUT : UI_ACCENT);
	label(cr, _("Share your screen"), WIDTH - MARGIN - 206, y + 7, 202, 15,
	      p->selected < 0 ? UI_SUBTLE : UI_ON_ACCENT, TRUE);
	cairo_surface_flush(image);
	SDL_Surface *pixels = NULL;

	if (cairo_status(cr) == CAIRO_STATUS_SUCCESS)
		pixels = SDL_CreateRGBSurfaceWithFormatFrom(
			cairo_image_surface_get_data(image), window->w,
			window->h, 32, cairo_image_surface_get_stride(image),
			SDL_PIXELFORMAT_RGB888);
	gboolean ok = pixels && !SDL_BlitSurface(pixels, NULL, window, NULL) &&
		      !SDL_UpdateWindowSurface(p->window);

	SDL_FreeSurface(pixels);
	cairo_destroy(cr);
	cairo_surface_destroy(image);
	return ok;
}

static int hit(const struct picker *p, int x, int y)
{
	int w, h;

	SDL_GetWindowSize(p->window, &w, &h);
	if (w <= 0 || h <= 0)
		return -1;
	x = (gint64)x * WIDTH / w;
	y = (gint64)y * p->height / h;
	if (x < MARGIN || x >= WIDTH - MARGIN)
		return -1;
	if (y >= LIST_TOP && y < LIST_TOP + p->rows * ROW)
		return p->first + (y - LIST_TOP) / ROW;
	if (y < p->height - FOOTER + 8 || y >= p->height - FOOTER + 44)
		return -1;
	if (x < MARGIN + 160)
		return CANCEL;
	if (x >= WIDTH - MARGIN - 210 && p->selected >= 0)
		return SHARE;
	return -1;
}

static int pick(struct picker *p)
{
	SDL_Event event;
	gboolean dirty = TRUE;

	for (;;) {
		if ((dirty && !draw(p)) || !SDL_WaitEvent(&event))
			return 1;
		dirty = FALSE;
		switch (event.type) {
		case SDL_QUIT:
			return 0;
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_CLOSE)
				return 0;
			if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				p->pressed = -1;
			dirty = event.window.event == SDL_WINDOWEVENT_EXPOSED ||
				event.window.event ==
					SDL_WINDOWEVENT_SIZE_CHANGED;
			break;
		case SDL_KEYDOWN: {
			SDL_Keycode key = event.key.keysym.sym;

			if (key == SDLK_ESCAPE)
				return 0;
			if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) &&
			    !event.key.repeat && p->selected >= 0)
				return LINE_MONITOR_SELECTED + p->selected;
			if (key != SDLK_UP && key != SDLK_DOWN &&
			    key != SDLK_TAB)
				break;
			gboolean back = key == SDLK_UP ||
					(key == SDLK_TAB &&
					 (event.key.keysym.mod & KMOD_SHIFT));

			p->selected =
				p->selected < 0 ?
					(back ? p->count - 1 : 0) :
					CLAMP(p->selected + (back ? -1 : 1), 0,
					      p->count - 1);
			p->first = CLAMP(p->first, p->selected - p->rows + 1,
					 p->selected);
			dirty = TRUE;
			break;
		}
		case SDL_MOUSEWHEEL: {
			/* The desktop has already applied natural scrolling. */
			gint64 steps = event.wheel.y;

			p->first =
				CLAMP(p->first - steps, 0, p->count - p->rows);
			dirty = TRUE;
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			if (event.button.button != SDL_BUTTON_LEFT)
				break;
			int target = hit(p, event.button.x, event.button.y);

			if (event.type == SDL_MOUSEBUTTONDOWN) {
				p->pressed = target;
				break;
			}
			if (target == p->pressed) {
				if (target == CANCEL)
					return 0;
				if (target == SHARE)
					return LINE_MONITOR_SELECTED +
					       p->selected;
				if (target >= 0) {
					p->selected = target;
					dirty = TRUE;
				}
			}
			p->pressed = -1;
			break;
		}
		}
	}
}

int ui_pick_share_monitor(int count, char **labels)
{
	pid_t parent = getppid();
	struct picker p = { .count = count,
			    .selected = -1,
			    .pressed = -1,
			    .names = labels,
			    .rows = MIN(count, MAX_ROWS) };

	if (count < 1 || count > LINE_MONITOR_LIMIT || parent == 1 ||
	    prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
		return 1;
	ui_lang_init();
	SDL_SetHintWithPriority(SDL_HINT_NO_SIGNAL_HANDLERS, "1",
				SDL_HINT_OVERRIDE);
	SDL_SetHint("SDL_APP_ID", "avernella");
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		return 1;
	p.height = LIST_TOP + p.rows * ROW + FOOTER;
	p.window = SDL_CreateWindow(_("Choose a monitor to share"),
				    SDL_WINDOWPOS_CENTERED,
				    SDL_WINDOWPOS_CENTERED, WIDTH, p.height,
				    SDL_WINDOW_ALLOW_HIGHDPI);
	int result = p.window ? pick(&p) : 1;

	if (p.window)
		SDL_DestroyWindow(p.window);
	SDL_Quit();
	return result;
}
