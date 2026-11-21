// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_pixels.h"
#include "ui_gpu_scroll.h"
#include <SDL.h>

static void exact(void)
{
	guint32 buffer[256];

	for (guint alpha = 0; alpha < 256; alpha++) {
		for (guint i = 0; i < 256; i++)
			buffer[i] = (alpha << 24) | (i << 16) |
				    ((255 - i) << 8) | (i ^ 85);
		ui_premultiply_argb(buffer, G_N_ELEMENTS(buffer));
		for (guint i = 0; i < 256; i++) {
			guint32 r = (i * alpha + 127) / 255,
				g = ((255 - i) * alpha + 127) / 255,
				b = ((i ^ 85) * alpha + 127) / 255;
			g_assert_cmphex(buffer[i], ==,
					(alpha << 24) | (r << 16) | (g << 8) |
						b);
		}
	}
}

static void scroll_canvas(void)
{
	enum {
		W = 17,
		H = 21
	};
	SDL_Rect canvas = { 0, 0, W, H }, area = { 3, 2, 9, 13 };
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
		0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
	SDL_Texture *texture =
		SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				  SDL_TEXTUREACCESS_STATIC, W, H);
	guint32 expected[H][W], actual[H][W];
	const int shifts[] = { 0, 1, 5, 7, -2, -6, 11, -12 };
	int offset = 29, origin = 0;

	g_assert_nonnull(texture);
	g_assert_cmpint(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE),
			==, 0);
	for (guint i = 0; i < G_N_ELEMENTS(shifts); i++) {
		offset += shifts[i];
		origin = (origin + shifts[i] + area.h) % area.h;
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				gboolean inside =
					x >= area.x && x < area.x + area.w &&
					y >= area.y && y < area.y + area.h;
				int content_y = y + (inside ? offset : 0);

				expected[y][x] = 0xff000000u |
						 (content_y << 8) | x;
			}
		}
		if (!i) {
			g_assert_cmpint(SDL_UpdateTexture(texture, NULL,
							  expected,
							  sizeof(expected[0])),
					==, 0);
		} else {
			SDL_Rect band = area;
			struct ui_scroll_copy parts[2];

			band.h = ABS(shifts[i]);
			if (shifts[i] > 0)
				band.y += area.h - band.h;
			int parts_count =
				ui_scroll_slices(area, origin, band, parts);

			for (int j = 0; j < parts_count; j++) {
				SDL_Rect dst = parts[j].dst;

				g_assert_cmpint(SDL_UpdateTexture(
							texture, &parts[j].src,
							&expected[dst.y][dst.x],
							sizeof(expected[0])),
						==, 0);
			}
		}
		struct ui_scroll_copy copies[6];
		int count = ui_scroll_quads(canvas, area, origin, copies);

		for (int j = 0; j < count; j++)
			g_assert_cmpint(SDL_RenderCopy(renderer, texture,
						       &copies[j].src,
						       &copies[j].dst),
					==, 0);
		g_assert_cmpint(SDL_RenderReadPixels(renderer, NULL,
						     SDL_PIXELFORMAT_ARGB8888,
						     actual, sizeof(actual[0])),
				==, 0);
		g_assert_cmpmem(actual, sizeof(actual), expected,
				sizeof(expected));
	}
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_FreeSurface(surface);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pixels/alpha-rounding", exact);
	g_test_add_func("/pixels/scroll-canvas", scroll_canvas);
	return g_test_run();
}
