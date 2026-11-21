// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera.h"
#ifdef __SSE2__
#include <emmintrin.h>
#endif

static inline guint32 pixel(int y, int r, int g, int b)
{
	int luma = 298 * MAX(y - 16, 0);

	return (CLAMP((luma + r) >> 8, 0, 255) << 16) |
	       (CLAMP((luma + g) >> 8, 0, 255) << 8) |
	       CLAMP((luma + b) >> 8, 0, 255);
}

#ifdef __SSE2__
/* Eight neighboring pixels use four chroma samples. Split the coefficients
 * at multiples of 256 (298 = 256 + 42, 409 = 256 + 153, -208 = -256 + 48,
 * 516 = 512 + 4). The fractional sums then fit signed 16-bit lanes while
 * preserving the same rounding and limited-range black as the scalar path.
 */
static inline __m128i chroma4(const guint8 *p)
{
	guint32 four;

	memcpy(&four, p, sizeof(four));
	__m128i bytes = _mm_cvtsi32_si128(four);

	bytes = _mm_unpacklo_epi8(bytes, bytes);
	return _mm_sub_epi16(_mm_unpacklo_epi8(bytes, _mm_setzero_si128()),
			     _mm_set1_epi16(128));
}

static inline void xrgb8(const guint8 *y, const guint8 *u, const guint8 *v,
			 guint32 *dst)
{
	__m128i zero = _mm_setzero_si128();
	__m128i cb = chroma4(u), cr = chroma4(v);
	__m128i luma = _mm_loadl_epi64((const __m128i_u *)y);

	luma = _mm_unpacklo_epi8(_mm_subs_epu8(luma, _mm_set1_epi8(16)), zero);
	__m128i frac = _mm_add_epi16(_mm_mullo_epi16(luma, _mm_set1_epi16(42)),
				     _mm_set1_epi16(128));
	__m128i r =
		_mm_add_epi16(frac, _mm_mullo_epi16(cr, _mm_set1_epi16(153)));
	__m128i g = _mm_sub_epi16(
		_mm_add_epi16(frac, _mm_mullo_epi16(cr, _mm_set1_epi16(48))),
		_mm_mullo_epi16(cb, _mm_set1_epi16(100)));
	__m128i b = _mm_add_epi16(frac, _mm_slli_epi16(cb, 2));

	r = _mm_add_epi16(_mm_add_epi16(luma, cr), _mm_srai_epi16(r, 8));
	g = _mm_add_epi16(_mm_sub_epi16(luma, cr), _mm_srai_epi16(g, 8));
	b = _mm_add_epi16(_mm_add_epi16(luma, _mm_slli_epi16(cb, 1)),
			  _mm_srai_epi16(b, 8));
	__m128i bg = _mm_unpacklo_epi8(_mm_packus_epi16(b, b),
				       _mm_packus_epi16(g, g));
	__m128i rx = _mm_unpacklo_epi8(_mm_packus_epi16(r, r), zero);

	_mm_storeu_si128((__m128i_u *)dst, _mm_unpacklo_epi16(bg, rx));
	_mm_storeu_si128((__m128i_u *)(dst + 4), _mm_unpackhi_epi16(bg, rx));
}
#endif

void line_camera_xrgb(const struct line_camera_frame *frame, guint32 *pixels)
{
	unsigned int width = frame->width, height = frame->height;
	unsigned int paired_width = width & ~1u;

	for (unsigned int row = 0; row < height; row++) {
		const guint8 *y =
			frame->plane[0] + (gssize)row * frame->stride[0];
		const guint8 *u =
			frame->plane[1] + (gssize)(row / 2) * frame->stride[1];
		const guint8 *v =
			frame->plane[2] + (gssize)(row / 2) * frame->stride[2];
		unsigned int x = 0;

#ifdef __SSE2__
		for (; x < (width & ~7u); x += 8)
			xrgb8(y + x, u + x / 2, v + x / 2, pixels + x);
#endif
		for (; x < paired_width; x += 2) {
			int cb = u[x / 2] - 128;
			int cr = v[x / 2] - 128;
			int r = 409 * cr + 128;
			int g = -100 * cb - 208 * cr + 128;
			int b = 516 * cb + 128;

			pixels[x] = pixel(y[x], r, g, b);
			pixels[x + 1] = pixel(y[x + 1], r, g, b);
		}
		if (paired_width != width) {
			int cb = u[paired_width / 2] - 128;
			int cr = v[paired_width / 2] - 128;

			pixels[paired_width] = pixel(y[paired_width],
						     409 * cr + 128,
						     -100 * cb - 208 * cr + 128,
						     516 * cb + 128);
		}
		pixels += width;
	}
}
