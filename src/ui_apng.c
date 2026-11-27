// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_apng.h"
#include "ui_pixels.h"
#include <png.h>
#include <zlib.h>
#include <string.h>

/*
 * For stickers and inline emoji, decode native deltas and retain only the
 * bounded size already chosen for this poster by the thumbnail worker.
 */
enum {
	MAX_EDGE = 512,
	MAX_FRAMES = 128,
	MAX_CHUNKS = 4096,
	MAX_ENCODED = 4 * 1024 * 1024,
	MAX_COLOR_CHUNKS = 64 * 1024,
	MAX_ANIMATION_PIXELS = 32 * 1024 * 1024,
	MAX_ONE = 12 * 1024 * 1024,
	MAX_ALL = 32 * 1024 * 1024,
	MAX_GIF_FRAMES = 256,
	MAX_GIF_ENCODED = 32 * 1024 * 1024
};

struct apng_frame {
	cairo_surface_t *surface;
	gint64 end;
};

struct apng_animation {
	guint count, plays;
	gsize reserved;
	gint64 start;
	struct apng_frame frames[];
};

struct apng_frame_control {
	guint w, h, x, y, dispose, blend;
	gint64 delay;
};

struct apng_decoder {
	const guint8 *ihdr;
	guint width, height, output_w, output_h, index;
	struct apng_animation *animation;
	cairo_surface_t *canvas;
	GByteArray *color, *compressed;
	struct apng_frame_control control;
};

static cairo_user_data_key_t animation_key;
G_LOCK_DEFINE_STATIC(animation_memory);
static gsize bytes_used;
static const guint8 signature[] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

static guint32 be32(const guint8 *p)
{
	guint32 value;

	memcpy(&value, p, 4);
	return GUINT32_FROM_BE(value);
}

static void write32(guint8 *p, guint32 value)
{
	value = GUINT32_TO_BE(value);
	memcpy(p, &value, 4);
}

static void release_animation(void *data)
{
	struct apng_animation *a = data;

	for (guint i = 0; i < a->count; i++)
		if (a->frames[i].surface)
			cairo_surface_destroy(a->frames[i].surface);
	G_LOCK(animation_memory);
	bytes_used -= a->reserved;
	G_UNLOCK(animation_memory);
	g_free(a);
}

static struct apng_animation *reserve_frames(guint count, guint plays,
					     gsize frame_bytes)
{
	if (!count || count > MAX_FRAMES || !frame_bytes ||
	    frame_bytes > MAX_ONE / count)
		return NULL;
	gsize amount = count * frame_bytes;

	G_LOCK(animation_memory);
	gboolean ok = amount <= MAX_ALL - bytes_used;

	if (ok)
		bytes_used += amount;
	G_UNLOCK(animation_memory);
	if (!ok)
		return NULL;
	struct apng_animation *a =
		g_malloc0(sizeof(*a) + sizeof(struct apng_frame) * count);
	a->plays = MAX(
		plays,
		1u); /* A looping chat emoji plays once, then lets the UI sleep. */
	a->count = count;
	a->reserved = amount;
	a->start = -1;
	return a;
}

static void png_chunk(GByteArray *png, const char type[4], const guint8 *bytes,
		      guint32 n)
{
	guint8 field[4];

	write32(field, n);
	g_byte_array_append(png, field, 4);
	g_byte_array_append(png, (const guint8 *)type, 4);
	if (n)
		g_byte_array_append(png, bytes, n);
	uLong crc = crc32(0, (const Bytef *)type, 4);

	if (n)
		crc = crc32(crc, bytes, n);
	write32(field, crc);
	g_byte_array_append(png, field, 4);
}

static cairo_surface_t *image(guint width, guint height)
{
	cairo_surface_t *s =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
		return s;
	cairo_surface_destroy(s);
	return NULL;
}

static cairo_surface_t *decode_delta(struct apng_decoder *d)
{
	struct apng_frame_control *f = &d->control;

	g_autoptr(GByteArray) png =
		g_byte_array_sized_new(d->compressed->len + d->color->len + 64);
	g_byte_array_append(png, signature, sizeof(signature));
	guint8 hdr[13];

	memcpy(hdr, d->ihdr, sizeof(hdr));
	write32(hdr, f->w);
	write32(hdr + 4, f->h);
	png_chunk(png, "IHDR", hdr, sizeof(hdr));
	if (d->color->len)
		g_byte_array_append(png, d->color->data, d->color->len);
	png_chunk(png, "IDAT", d->compressed->data, d->compressed->len);
	png_chunk(png, "IEND", NULL, 0);
	png_image info = { .version = PNG_IMAGE_VERSION };
	cairo_surface_t *s = NULL;

	if (png_image_begin_read_from_memory(&info, png->data, png->len)) {
		info.format = UI_LITTLE_ENDIAN ? PNG_FORMAT_BGRA :
						 PNG_FORMAT_ARGB;
		s = image(f->w, f->h);
		if (s && png_image_finish_read(
				 &info, NULL, cairo_image_surface_get_data(s),
				 cairo_image_surface_get_stride(s), NULL)) {
			ui_premultiply_argb(
				(guint32 *)cairo_image_surface_get_data(s),
				(gsize)f->w * f->h);
			cairo_surface_mark_dirty(s);
		} else if (s) {
			cairo_surface_destroy(s);
			s = NULL;
		}
	}
	png_image_free(&info);
	return s;
}

static cairo_surface_t *snapshot(struct apng_decoder *d)
{
	cairo_surface_t *s = image(d->output_w, d->output_h);

	if (!s)
		return NULL;
	cairo_t *cr = cairo_create(s);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_scale(cr, (double)d->output_w / d->width,
		    (double)d->output_h / d->height);
	cairo_set_source_surface(cr, d->canvas, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint(cr);
	gboolean ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;

	cairo_destroy(cr);
	if (!ok) {
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

static gboolean finish_frame(struct apng_decoder *d)
{
	struct apng_frame_control *f = &d->control;
	struct apng_animation *a = d->animation;

	if (!f->w || !d->compressed->len || d->index >= a->count)
		return FALSE;
	cairo_surface_t *tile = decode_delta(d);

	if (!tile)
		return FALSE;
	cairo_surface_flush(d->canvas);
	guint8 *base = cairo_image_surface_get_data(d->canvas);
	gsize stride = cairo_image_surface_get_stride(d->canvas),
	      row_bytes = f->w * 4;
	base += f->y * stride + f->x * 4;
	g_autofree guint8 *previous = NULL;

	if (f->dispose == 2) {
		previous = g_malloc(row_bytes * f->h);
		for (guint y = 0; y < f->h; y++)
			memcpy(previous + y * row_bytes, base + y * stride,
			       row_bytes);
	}
	cairo_t *cr = cairo_create(d->canvas);

	cairo_set_operator(cr, f->blend ? CAIRO_OPERATOR_OVER :
					  CAIRO_OPERATOR_SOURCE);
	cairo_rectangle(cr, f->x, f->y, f->w, f->h);
	cairo_clip(cr);
	cairo_set_source_surface(cr, tile, f->x, f->y);
	cairo_paint(cr);
	gboolean ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;

	cairo_destroy(cr);
	cairo_surface_destroy(tile);
	if (!ok)
		return FALSE;
	struct apng_frame *out = &a->frames[d->index];

	out->surface = snapshot(d);
	if (!out->surface)
		return FALSE;
	out->end = (d->index ? a->frames[d->index - 1].end : 0) + f->delay;
	if (f->dispose) {
		cairo_surface_flush(d->canvas);
		for (guint y = 0; y < f->h; y++) {
			if (previous)
				memcpy(base + y * stride,
				       previous + y * row_bytes, row_bytes);
			else
				memset(base + y * stride, 0, row_bytes);
		}
		cairo_surface_mark_dirty_rectangle(d->canvas, f->x, f->y, f->w,
						   f->h);
	}
	d->index++;
	g_byte_array_set_size(d->compressed, 0);
	return TRUE;
}

static gboolean read_control(struct apng_decoder *d, const guint8 *p,
			     gboolean default_frame)
{
	struct apng_frame_control *f = &d->control;

	f->w = be32(p + 4);
	f->h = be32(p + 8);
	f->x = be32(p + 12);
	f->y = be32(p + 16);
	guint numerator = p[20] * 256u + p[21],
	      denominator = p[22] * 256u + p[23];
	if (!denominator)
		denominator = 100;
	f->delay = numerator ? ((gint64)numerator * G_USEC_PER_SEC +
				denominator / 2) /
				       denominator :
			       10000;
	f->dispose = p[24];
	f->blend = p[25];
	return f->w && f->h && f->w <= d->width && f->h <= d->height &&
	       f->x <= d->width - f->w && f->y <= d->height - f->h &&
	       f->dispose <= 2 && f->blend <= 1 &&
	       (!default_frame ||
		(f->w == d->width && f->h == d->height && !f->x && !f->y));
}

static gboolean color_type(const guint8 *t)
{
	static const char names[][5] = { "PLTE", "tRNS", "sBIT", "gAMA",
					 "cHRM", "sRGB", "iCCP", "cICP" };
	for (guint i = 0; i < G_N_ELEMENTS(names); i++)
		if (!memcmp(t, names[i], 4))
			return TRUE;
	return FALSE;
}

static gboolean parse(struct apng_decoder *d, const guint8 *p, gsize n)
{
	gboolean idat = FALSE, idat_closed = FALSE, using_default = FALSE;
	guint32 seq = 0;
	guint chunks = 0;

	for (gsize offset = 8; offset + 12 <= n && ++chunks <= MAX_CHUNKS;) {
		const guint8 *chunk = p + offset, *type = chunk + 4,
			     *data = chunk + 8;
		guint32 size = be32(chunk);

		if (size > n - offset - 12)
			return FALSE;
		offset += size + 12;
		if (!memcmp(type, "IDAT", 4) && !d->animation)
			return FALSE; /* ordinary static PNG */
		if (crc32(0, type, size + 4) != be32(data + size))
			return FALSE;
		if (idat && memcmp(type, "IDAT", 4))
			idat_closed = TRUE;
		if (!memcmp(type, "IHDR", 4)) {
			if (chunks != 1 || size != 13)
				return FALSE;
			d->ihdr = data;
			d->width = be32(data);
			d->height = be32(data + 4);
			if (!d->width || !d->height || d->width > MAX_EDGE ||
			    d->height > MAX_EDGE)
				return FALSE;
		} else if (!d->ihdr)
			return FALSE;
		else if (!memcmp(type, "acTL", 4)) {
			if (d->animation || idat || size != 8)
				return FALSE;
			guint32 count = be32(data);

			if ((guint64)count * d->width * d->height >
			    MAX_ANIMATION_PIXELS)
				return FALSE;
			d->animation = reserve_frames(count, be32(data + 4),
						      (gsize)d->output_w *
							      d->output_h * 4);
			if (!d->animation)
				return FALSE;
			d->canvas = image(d->width, d->height);
			if (!d->canvas)
				return FALSE;
		} else if (!memcmp(type, "fcTL", 4)) {
			if (!d->animation || size != 26 || be32(data) != seq++)
				return FALSE;
			if (d->control.w && !finish_frame(d))
				return FALSE;
			if (d->index >= d->animation->count)
				return FALSE;
			using_default = !idat;
			if (!read_control(d, data, using_default))
				return FALSE;
		} else if (!memcmp(type, "IDAT", 4)) {
			if (idat_closed)
				return FALSE;
			idat = TRUE;
			if (using_default && size)
				g_byte_array_append(d->compressed, data, size);
		} else if (!memcmp(type, "fdAT", 4)) {
			if (!idat || !d->control.w || using_default ||
			    size < 4 || be32(data) != seq++)
				return FALSE;
			if (size > 4)
				g_byte_array_append(d->compressed, data + 4,
						    size - 4);
		} else if (!memcmp(type, "IEND", 4)) {
			return !size && offset == n && d->animation && idat &&
			       finish_frame(d) &&
			       d->index == d->animation->count;
		} else if (color_type(type)) {
			if (idat ||
			    size + 12 > MAX_COLOR_CHUNKS - d->color->len)
				return FALSE;
			g_byte_array_append(d->color, chunk, size + 12);
		} else if (!(type[0] & 32))
			return FALSE;
	}
	return FALSE;
}

void ui_apng_attach(cairo_surface_t *poster, const guint8 *encoded, gsize bytes)
{
	if (!poster || !encoded || bytes < 53 || bytes > MAX_ENCODED ||
	    memcmp(encoded, signature, sizeof(signature)) ||
	    cairo_surface_get_type(poster) != CAIRO_SURFACE_TYPE_IMAGE ||
	    cairo_surface_get_user_data(poster, &animation_key))
		return;
	struct apng_decoder d = {
		.output_w = cairo_image_surface_get_width(poster),
		.output_h = cairo_image_surface_get_height(poster)
	};
	if (!d.output_w || !d.output_h || d.output_w > MAX_EDGE ||
	    d.output_h > MAX_EDGE)
		return;
	d.compressed = g_byte_array_new();
	d.color = g_byte_array_new();
	if (parse(&d, encoded, bytes) &&
	    cairo_surface_set_user_data(poster, &animation_key, d.animation,
					release_animation) ==
		    CAIRO_STATUS_SUCCESS)
		d.animation = NULL;
	if (d.canvas)
		cairo_surface_destroy(d.canvas);
	if (d.animation)
		release_animation(d.animation);
	g_byte_array_unref(d.compressed);
	g_byte_array_unref(d.color);
}

void ui_apng_replay(cairo_surface_t *poster)
{
	struct apng_animation *a =
		poster ? cairo_surface_get_user_data(poster, &animation_key) :
			 NULL;

	if (a)
		a->start = -1;
}

cairo_surface_t *ui_apng_frame(cairo_surface_t *poster, gint64 now,
			       gint64 *until)
{
	if (until)
		*until = 0;
	struct apng_animation *a =
		poster ? cairo_surface_get_user_data(poster, &animation_key) :
			 NULL;
	if (!a)
		return poster;
	if (a->count == 1)
		return a->frames[0].surface;
	now = MAX(now, (gint64)0);
	if (a->start < 0)
		a->start = now;
	gint64 elapsed = MAX((gint64)0, now - a->start),
	       duration = a->frames[a->count - 1].end;
	if ((guint64)(elapsed / duration) >= a->plays)
		return a->frames[a->count - 1].surface;
	gint64 phase = elapsed % duration;
	guint lo = 0, hi = a->count - 1;

	while (lo < hi) {
		guint mid = lo + (hi - lo) / 2;

		if (phase < a->frames[mid].end)
			hi = mid;
		else
			lo = mid + 1;
	}
	if (until && !(lo == a->count - 1 &&
		       (guint64)(elapsed / duration) + 1 >= a->plays)) {
		gint64 delta = a->frames[lo].end - phase;
		*until = now <= G_MAXINT64 - delta ? now + delta : G_MAXINT64;
	}
	return a->frames[lo].surface;
}

/*
 * Bounded GIF87a/GIF89a decoder sharing the APNG animation playback engine.
 * Supports global/local palettes, LZW (2..8 bit min code size), 4-pass
 * interlacing, transparency, all disposal modes, and NETSCAPE2.0 looping.
 */
static guint16 le16(const guint8 *p)
{
	return (guint16)p[0] | ((guint16)p[1] << 8);
}

static gboolean gif_skip_subblocks(const guint8 *d, gsize n, gsize *pos)
{
	while (*pos < n) {
		guint8 len = d[(*pos)++];

		if (!len)
			return TRUE;
		if (len > n - *pos)
			return FALSE;
		*pos += len;
	}
	return FALSE;
}

static gboolean gif_lzw_decode(const guint8 *d, gsize n, gsize *pos,
			       guint8 min_bits, guint8 *out, gsize out_len)
{
	if (min_bits < 2 || min_bits > 8)
		return FALSE;
	guint16 prefix[4096];
	guint8 suffix[4096], stack[4096];
	guint clear_code = 1u << min_bits;
	guint eoi_code = clear_code + 1;
	guint code_size = min_bits + 1;
	guint next_code = eoi_code + 1;
	guint mask = (1u << code_size) - 1;
	gint prev = -1;
	guint8 first = 0;

	for (guint i = 0; i < clear_code; i++) {
		prefix[i] = 0;
		suffix[i] = (guint8)i;
	}
	guint32 bit_buf = 0;
	guint bit_cnt = 0;
	gsize written = 0;
	gboolean done = FALSE;

	while (*pos < n) {
		guint8 block_len = d[(*pos)++];

		if (!block_len)
			return written > 0;
		if (block_len > n - *pos)
			return FALSE;
		const guint8 *blk = d + *pos;

		*pos += block_len;
		if (done)
			continue;
		for (guint i = 0; i < block_len; i++) {
			bit_buf |= (guint32)blk[i] << bit_cnt;
			bit_cnt += 8;
			while (bit_cnt >= code_size) {
				guint code = bit_buf & mask;

				bit_buf >>= code_size;
				bit_cnt -= code_size;
				if (code == clear_code) {
					code_size = min_bits + 1;
					next_code = eoi_code + 1;
					mask = (1u << code_size) - 1;
					prev = -1;
					continue;
				}
				if (code == eoi_code) {
					done = TRUE;
					break;
				}
				if (code > next_code)
					return FALSE;
				guint cur = code;
				guint sp = 0;

				if (code == next_code) {
					if (prev < 0)
						return FALSE;
					stack[sp++] = first;
					cur = (guint)prev;
				}
				while (cur >= clear_code) {
					if (sp >= 4096)
						return FALSE;
					stack[sp++] = suffix[cur];
					cur = prefix[cur];
				}
				first = (guint8)cur;
				stack[sp++] = first;
				while (sp > 0 && written < out_len)
					out[written++] = stack[--sp];
				if (prev >= 0 && next_code < 4096) {
					prefix[next_code] = (guint16)prev;
					suffix[next_code] = first;
					next_code++;
					if (next_code == (1u << code_size) &&
					    code_size < 12) {
						code_size++;
						mask = (1u << code_size) - 1;
					}
				}
				prev = (gint)code;
			}
		}
	}
	return FALSE;
}

static cairo_surface_t *gif_scale_frame(cairo_surface_t *canvas, guint src_w,
					guint src_h, guint dst_w, guint dst_h)
{
	cairo_surface_t *frame = image(dst_w, dst_h);

	if (!frame)
		return NULL;
	if (src_w == dst_w && src_h == dst_h) {
		memcpy(cairo_image_surface_get_data(frame),
		       cairo_image_surface_get_data(canvas),
		       (gsize)cairo_image_surface_get_stride(frame) * dst_h);
		cairo_surface_mark_dirty(frame);
		return frame;
	}
	cairo_t *cr = cairo_create(frame);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_scale(cr, (double)dst_w / src_w, (double)dst_h / src_h);
	cairo_set_source_surface(cr, canvas, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(frame);
	return frame;
}

cairo_surface_t *ui_gif_decode(const guint8 *d, gsize n, int limit,
			       int max_edge, gboolean animate)
{
	if (!d || n < 14 || n > MAX_GIF_ENCODED ||
	    (memcmp(d, "GIF89a", 6) && memcmp(d, "GIF87a", 6)))
		return NULL;
	guint w = le16(d + 6), h = le16(d + 8);

	if (!w || !h || w > (guint)max_edge || h > (guint)max_edge ||
	    (gsize)w * h > MAX_ANIMATION_PIXELS)
		return NULL;
	guint out_w = w, out_h = h;
	int eff_limit = animate && limit > 640 ? 640 : limit;

	if (eff_limit > 0 && MAX(w, h) > (guint)eff_limit) {
		if (w >= h) {
			out_w = (guint)eff_limit;
			out_h = MAX(1u, (guint)((guint64)h * eff_limit / w));
		} else {
			out_h = (guint)eff_limit;
			out_w = MAX(1u, (guint)((guint64)w * eff_limit / h));
		}
	}
	gsize pos = 13;
	guint8 packed = d[10];
	const guint8 *gct = NULL;
	guint gct_len = 0;

	if (packed & 0x80) {
		gct_len = 1u << ((packed & 7u) + 1u);
		if (gct_len * 3u > n - pos)
			return NULL;
		gct = d + pos;
		pos += gct_len * 3u;
	}
	cairo_surface_t *canvas = image(w, h);

	if (!canvas)
		return NULL;
	guint32 *cpix = (guint32 *)cairo_image_surface_get_data(canvas);
	guint cstride = (guint)(cairo_image_surface_get_stride(canvas) / 4);
	g_autofree guint32 *saved = NULL;
	g_autofree guint8 *idx_buf = NULL;
	gsize idx_cap = 0;
	GArray *frames = g_array_new(FALSE, FALSE, sizeof(struct apng_frame));
	gsize frame_bytes = (gsize)out_w * out_h * 4;
	gsize reserved = 0;
	guint plays = 1;
	gboolean has_loop = FALSE;
	guint8 disposal = 0;
	gboolean has_trans = FALSE;
	guint8 trans_idx = 0;
	gint64 delay_us = 100 * 1000;
	gint64 total_us = 0;
	cairo_surface_t *poster = NULL;

	while (pos < n) {
		guint8 intro = d[pos++];

		if (intro == 0x3B) /* Trailer */
			break;
		if (intro == 0x21) { /* Extension */
			if (pos >= n)
				break;
			guint8 label = d[pos++];

			if (label == 0xF9 && pos + 6 <= n && d[pos] == 4) {
				guint8 gce_p = d[pos + 1];
				guint16 cs = le16(d + pos + 2);

				disposal = (gce_p >> 2) & 7u;
				has_trans = (gce_p & 1u) != 0;
				trans_idx = d[pos + 4];
				delay_us = (gint64)(cs <= 1 ? 10 : cs) * 10000;
				pos += 5;
				if (!gif_skip_subblocks(d, n, &pos))
					break;
			} else if (label == 0xFF && pos + 12 <= n &&
				   d[pos] == 11 &&
				   (!memcmp(d + pos + 1, "NETSCAPE2.0", 11) ||
				    !memcmp(d + pos + 1, "ANIMEXTS1.0", 11))) {
				pos += 12;
				if (pos + 4 <= n && d[pos] == 3 &&
				    d[pos + 1] == 1) {
					guint16 loops = le16(d + pos + 2);

					has_loop = TRUE;
					plays = loops == 0 ? G_MAXUINT :
							     (guint)loops + 1u;
					pos += 4;
				}
				if (!gif_skip_subblocks(d, n, &pos))
					break;
			} else if (!gif_skip_subblocks(d, n, &pos)) {
				break;
			}
			continue;
		}
		if (intro != 0x2C) /* Not an Image Descriptor */
			break;
		if (pos + 9 > n)
			break;
		guint left = le16(d + pos), top = le16(d + pos + 2);
		guint iw = le16(d + pos + 4), ih = le16(d + pos + 6);
		guint8 ipacked = d[pos + 8];

		pos += 9;
		const guint8 *ct = gct;
		guint ct_len = gct_len;

		if (ipacked & 0x80) {
			ct_len = 1u << ((ipacked & 7u) + 1u);
			if (ct_len * 3u > n - pos)
				break;
			ct = d + pos;
			pos += ct_len * 3u;
		}
		if (pos >= n)
			break;
		guint8 min_bits = d[pos++];
		gsize npx = (gsize)iw * ih;

		if (!iw || !ih || left + iw > w || top + ih > h || !ct) {
			if (!gif_skip_subblocks(d, n, &pos))
				break;
			continue;
		}
		if (npx > idx_cap) {
			g_free(idx_buf);
			idx_buf = g_try_malloc(npx);
			if (!idx_buf)
				break;
			idx_cap = npx;
		}
		if (!gif_lzw_decode(d, n, &pos, min_bits, idx_buf, npx))
			break;
		if (disposal == 3) {
			if (!saved)
				saved = g_try_malloc_n((gsize)cstride * h,
						       sizeof(guint32));
			if (saved)
				memcpy(saved, cpix,
				       (gsize)cstride * h * sizeof(guint32));
		}
		gboolean interlaced = (ipacked & 0x40) != 0;
		static const guint pass_start[4] = { 0, 4, 2, 1 };
		static const guint pass_step[4] = { 8, 8, 4, 2 };
		guint src_row = 0;

		for (guint pass = 0; pass < (interlaced ? 4u : 1u); pass++) {
			guint y0 = interlaced ? pass_start[pass] : 0;
			guint dy = interlaced ? pass_step[pass] : 1;

			for (guint y = y0; y < ih; y += dy) {
				const guint8 *srow =
					idx_buf + (gsize)src_row * iw;
				guint32 *drow = cpix +
						(gsize)(top + y) * cstride +
						left;

				src_row++;
				for (guint x = 0; x < iw; x++) {
					guint8 idx = srow[x];

					if ((has_trans && idx == trans_idx) ||
					    idx >= ct_len)
						continue;
					const guint8 *rgb = ct + (gsize)idx * 3;

					drow[x] = 0xff000000u |
						  ((guint32)rgb[0] << 16) |
						  ((guint32)rgb[1] << 8) |
						  (guint32)rgb[2];
				}
			}
		}
		cairo_surface_mark_dirty(canvas);
		if (!poster) {
			poster = gif_scale_frame(canvas, w, h, out_w, out_h);
			if (!poster)
				break;
		}
		if (animate && frames->len < MAX_GIF_FRAMES &&
		    reserved + frame_bytes <= MAX_ONE) {
			gboolean budget_ok = FALSE;

			G_LOCK(animation_memory);
			if (frame_bytes <= MAX_ALL - bytes_used) {
				bytes_used += frame_bytes;
				budget_ok = TRUE;
			}
			G_UNLOCK(animation_memory);
			if (budget_ok) {
				cairo_surface_t *fs =
					frames->len == 0 ?
						cairo_surface_reference(
							poster) :
						gif_scale_frame(canvas, w, h,
								out_w, out_h);
				if (fs) {
					total_us += delay_us;
					struct apng_frame f = {
						.surface = fs, .end = total_us
					};

					g_array_append_val(frames, f);
					reserved += frame_bytes;
				} else {
					G_LOCK(animation_memory);
					bytes_used -= frame_bytes;
					G_UNLOCK(animation_memory);
				}
			}
		}
		if (!animate)
			break;
		if (disposal == 2) {
			for (guint y = 0; y < ih; y++)
				memset(cpix + (gsize)(top + y) * cstride + left,
				       0, (gsize)iw * sizeof(guint32));
		} else if (disposal == 3 && saved) {
			memcpy(cpix, saved,
			       (gsize)cstride * h * sizeof(guint32));
		}
		disposal = 0;
		has_trans = FALSE;
		delay_us = 100 * 1000;
	}
	cairo_surface_destroy(canvas);
	if (poster && animate && frames->len >= 2) {
		struct apng_animation *a = g_malloc0(
			sizeof(*a) + sizeof(struct apng_frame) * frames->len);
		a->count = frames->len;
		a->plays = has_loop ? plays : G_MAXUINT;
		a->reserved = reserved;
		a->start = -1;
		memcpy(a->frames, frames->data,
		       sizeof(struct apng_frame) * frames->len);
		if (cairo_surface_set_user_data(poster, &animation_key, a,
						release_animation) !=
		    CAIRO_STATUS_SUCCESS)
			release_animation(a);
	} else {
		for (guint i = 0; i < frames->len; i++)
			cairo_surface_destroy(
				g_array_index(frames, struct apng_frame, i)
					.surface);
		if (reserved) {
			G_LOCK(animation_memory);
			bytes_used -= reserved;
			G_UNLOCK(animation_memory);
		}
	}
	g_array_free(frames, TRUE);
	return poster;
}
