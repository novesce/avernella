// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_gpu_scroll.h"

int ui_scroll_slices(SDL_Rect area, int origin, SDL_Rect destination,
		     struct ui_scroll_copy copies[2])
{
	SDL_Rect dst;

	if (!SDL_IntersectRect(&area, &destination, &dst))
		return 0;
	int row = (origin + dst.y - area.y) % area.h;
	int first = SDL_min(dst.h, area.h - row);

	copies[0] = (struct ui_scroll_copy){
		.src = { dst.x, area.y + row, dst.w, first },
		.dst = { dst.x, dst.y, dst.w, first },
	};
	if (first == dst.h)
		return 1;
	copies[1] = (struct ui_scroll_copy){
		.src = { dst.x, area.y, dst.w, dst.h - first },
		.dst = { dst.x, dst.y + first, dst.w, dst.h - first },
	};
	return 2;
}

int ui_scroll_quads(SDL_Rect canvas, SDL_Rect area, int origin,
		    struct ui_scroll_copy copies[6])
{
	if (!origin) {
		copies[0] = (struct ui_scroll_copy){ canvas, canvas };
		return 1;
	}
	SDL_Rect fixed[] = {
		{ canvas.x, canvas.y, canvas.w, area.y - canvas.y },
		{ canvas.x, area.y + area.h, canvas.w,
		  canvas.y + canvas.h - area.y - area.h },
		{ canvas.x, area.y, area.x - canvas.x, area.h },
		{ area.x + area.w, area.y,
		  canvas.x + canvas.w - area.x - area.w, area.h },
	};
	int count = 0;

	for (size_t i = 0; i < SDL_arraysize(fixed); i++) {
		if (!SDL_RectEmpty(&fixed[i]))
			copies[count++] =
				(struct ui_scroll_copy){ fixed[i], fixed[i] };
	}
	return count + ui_scroll_slices(area, origin, area, copies + count);
}
