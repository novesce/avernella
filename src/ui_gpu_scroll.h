/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL_rect.h>

/* Pixel rectangles in an existing canvas. The area's first visible scanline
 * lives at area.y + origin in the texture. A slice may cross the seam once.
 */
struct ui_scroll_copy {
	SDL_Rect src, dst;
};

int ui_scroll_slices(SDL_Rect area, int origin, SDL_Rect destination,
		     struct ui_scroll_copy copies[2]);
int ui_scroll_quads(SDL_Rect canvas, SDL_Rect area, int origin,
		    struct ui_scroll_copy copies[6]);
