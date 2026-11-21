// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "ui_gpu.h"
#include "ui_sounds.h"
#include "ui_profile.h"
#include "camera.h"
#include "ui_chat_lists.h"
#include "ui_emoji.h"
#include "ui_friends.h"
#include "ui_find.h"
#include "ui_chat_tools.h"
#include "ui_shared_content.h"
#include "ui_groups.h"
#include "flex_asset.h"
#include <dlfcn.h>
#include <stdbool.h>
#include "qr_scan.h"
#include "ui_apng.h"
#include "ui_motion.h"
#include "ui_pixels.h"
#include "screenshot.h"
#include "clipboard_paste.h"
#include <glib/gstdio.h>
#include "media.h"
#include "media_io.h"
#include "video_play.h"
#include "video_record.h"
#include <SDL_syswm.h>
#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <webp/decode.h>

enum {
	WHITE = 0xfbfbfb,
	MUTED = 0x96979e
};

enum {
	THUMB_EDGE = 320,
	AVATAR_EDGE = 80,
	/*
	 * Flex and rich art: 640 logical px on the longest side, in device
	 * pixels, so cards stay sharp on scaled displays.
	 */
	FLEX_VIEW = 640,
	FLEX_MAX_EDGE = 2048,
	STICKER_EDGE = 144,
	EMOJI_EDGE = 64,
	CACHE_BYTES = 32 * 1024 * 1024
};

enum media_mode {
	CLOSED,
	PICK,
	PREVIEW,
	VIEW,
	PLAY_VIDEO,
	MENU,
	REACT,
	UNSEND,
	REPLACE,
	DIAL,
	CALL,
	CAMERA,
	RECORDING,
	EMOJI_PICKER
};

enum decode_role {
	THUMBNAIL,
	ORIGINAL,
	LOCAL,
	AVATAR,
	EMOJI,
	FLEX_IMAGE,
	FRIEND_QR,
	CLIPBOARD
};

struct file_item {
	char *name;
	gboolean dir;
};

struct media_thumbnail {
	cairo_surface_t *image;
	char *reason, *version;
	gsize bytes;
	guint serial, used, seen;
	gboolean sticker;
	gboolean settled; /* a result arrived, with or without a picture */
};

struct decode_job {
	int fd;
	enum decode_role role;
	gboolean animate;
	int edge; /* longest side to keep, or 0 for the role's default */
	guint epoch, serial;
	GBytes *png;
	char *id, *reason;
	cairo_surface_t *image;
};

struct call_picture {
	void *pixels;
	uint64_t generation;
	size_t size;
	struct line_camera_frame frame;
	SDL_Texture *texture;
	cairo_surface_t *surface;
	SDL_FRect view;
	gboolean upload, convert, texture_failed;
};

enum stage_tile {
	TILE_NONE,
	TILE_PEER,
	TILE_SELF,
	TILE_OWN_SCREEN,
	TILE_PEER_SCREEN,
};

static struct {
	enum media_mode mode;
	guint epoch, serial, call_serial, decode_event, record_event;
	guint screenshot_event;
	GThread *screenshot_thread;
	GCancellable *screenshot_cancel;
	LineVideoPlayback *playback;
	guint play_event, play_serial, play_overlay;
	gboolean play_review;
	char *recorded, *call_error, *call_note;
	GHashTable *uploads;
	/* A pasted picture's staged file, and those being sent. */
	char *paste;
	GHashTable *paste_sent;
	char *chat, *id, *path, *title, *note, *directory, *call_chat,
		*call_name, *call_state, *reply_text;
	gboolean avatar_pick;
	gboolean friend_qr, save, photo, working, thumb_busy, muted, can_mute,
		copying, sticker;
	Editor edit;
	GPtrArray *entries;
	int offset, cursor, menu_x, menu_y;
	Rect dialog, path_entry, items, up, primary, secondary, third;
	GHashTable *thumbs, *avatars, *emoji, *flex_images;
	gsize cache_bytes;
	gboolean emoji_busy;
	guint use_seq, frame;
	gboolean avatar_busy, flex_busy;
	guint session, avatar_seq;
	GThreadPool *pool;
	cairo_surface_t *image;
	struct call_picture call_frame, self_frame, share_frame;
	gboolean video_call, video_ready, camera_enabled;
	/* Screen sharing: the peer's stream, ours, and the focused tile. */
	gboolean peer_sharing, self_sharing, share_working;
	gboolean share_in_video;
	gboolean call_working, preview_paused, window_hidden;
	gint64 call_frames_since, share_ended_at;
	int focus, first_click_focus;
	Rect share_button, tile_rect[4], fullscreen_button[4];
	enum stage_tile fullscreen_tile, first_click_tile;
	gboolean window_expanded, saw_fullscreen, ignore_multiclick;
	Rect call_minimize;
	Json *row;
} m;

static void reset_picture(struct call_picture *picture)
{
	g_clear_pointer(&picture->texture, SDL_DestroyTexture);
	picture->view = (SDL_FRect){ 0 };
	picture->upload = TRUE;
	picture->texture_failed = FALSE;
}

void ui_media_reset_renderer(void)
{
	reset_picture(&m.call_frame);
	reset_picture(&m.self_frame);
	reset_picture(&m.share_frame);
}

static void clear_picture(struct call_picture *picture)
{
	reset_picture(picture);
	g_clear_pointer(&picture->surface, cairo_surface_destroy);
	if (picture->pixels)
		munmap(picture->pixels, picture->size);
	*picture = (struct call_picture){ 0 };
}

gboolean ui_media_fullscreen(void)
{
	return m.fullscreen_tile != 0;
}

void ui_media_leave_fullscreen(void)
{
	if (!ui_media_fullscreen())
		return;
	m.fullscreen_tile = m.first_click_tile = 0;
	m.call_frame.view.w = m.self_frame.view.w = m.share_frame.view.w = 0;
	if (m.window_expanded && SDL_SetWindowFullscreen(ui.window, 0) < 0)
		ui_status("Could not restore the window size.");
	m.window_expanded = m.saw_fullscreen = FALSE;
	ui_restore_input();
	ui.dirty = TRUE;
}

static void clear_call_frame(void)
{
	ui_media_leave_fullscreen();
	m.first_click_tile = 0;
	clear_picture(&m.call_frame);
	clear_picture(&m.self_frame);
	clear_picture(&m.share_frame);
	m.peer_sharing = m.self_sharing = m.share_working = FALSE;
	m.share_in_video = FALSE;
	m.call_frames_since = m.share_ended_at = g_get_monotonic_time();
	m.preview_paused = FALSE;
	m.focus = 0;
}

static void set(char **p, const char *s)
{
	char *v = g_strdup(s);

	g_free(*p);
	*p = v;
	ui.dirty = TRUE;
}

static gboolean at(Rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static Json *args(const char *chat, const char *id)
{
	Json *p = json_object_new_object();

	if (chat)
		json_object_object_add(p, "chat", json_object_new_string(chat));
	if (id)
		json_object_object_add(p, "id", json_object_new_string(id));
	return p;
}

static void thumb_free(gpointer p)
{
	struct media_thumbnail *t = p;

	m.cache_bytes -= t->bytes;
	if (t->image)
		cairo_surface_destroy(t->image);
	g_free(t->reason);
	g_free(t->version);
	g_free(t);
}

static void touch(struct media_thumbnail *t)
{
	t->used = ++m.use_seq;
	t->seen = m.frame;
}

/* A count eviction may also remove failed entries. Byte eviction needs pixels. */
static gboolean evict_thumbnail(GHashTable *table)
{
	GHashTable *caches[] = { m.thumbs, m.avatars, m.emoji, m.flex_images };
	GHashTable *old_table = NULL;
	gpointer old_key = NULL;
	guint oldest = 0;

	for (guint i = 0; i < G_N_ELEMENTS(caches); i++) {
		GHashTable *cache = caches[i];
		GHashTableIter it;
		gpointer key, value;

		if (!cache || (table && cache != table))
			continue;
		g_hash_table_iter_init(&it, cache);
		while (g_hash_table_iter_next(&it, &key, &value)) {
			struct media_thumbnail *item = value;
			guint age = m.use_seq - item->used;

			if (m.frame - item->seen <= 1 ||
			    (!item->image && (!table || item->bytes)))
				continue;
			if (!old_table || age > oldest) {
				oldest = age;
				old_key = key;
				old_table = cache;
			}
		}
	}
	return old_table && g_hash_table_remove(old_table, old_key);
}

/*
 * Reserve the decoded poster before issuing a download. Keep the surfaces
 * seen in the current or last frame. Animation frames have their own bound.
 */
static struct media_thumbnail *reserve_thumbnail(GHashTable *cache,
						 guint entries, guint edge)
{
	gsize bytes = (gsize)edge * edge * 4;

	if (g_hash_table_size(cache) >= entries && !evict_thumbnail(cache))
		return NULL;
	while (m.cache_bytes > CACHE_BYTES - bytes)
		if (!evict_thumbnail(NULL))
			return NULL;
	struct media_thumbnail *t = g_new0(struct media_thumbnail, 1);

	touch(t);
	t->bytes = bytes;
	m.cache_bytes += bytes;
	return t;
}

static void store_thumbnail(struct media_thumbnail *t, cairo_surface_t **image)
{
	gsize bytes = image && *image ?
			      (gsize)cairo_image_surface_get_stride(*image) *
				      cairo_image_surface_get_height(*image) :
			      0;

	m.cache_bytes -= t->bytes;
	t->settled = TRUE;
	g_clear_pointer(&t->image, cairo_surface_destroy);
	if (bytes && bytes <= t->bytes)
		t->image = g_steal_pointer(image);
	t->bytes = t->image ? bytes : 0;
	m.cache_bytes += t->bytes;
}

static void item_free(gpointer p)
{
	struct file_item *f = p;

	g_free(f->name);
	g_free(f);
}

static void decoded_free(struct decode_job *job)
{
	if (job->fd >= 0)
		close(job->fd);
	g_free(job->id);
	g_free(job->reason);
	g_clear_pointer(&job->png, g_bytes_unref);
	if (job->image)
		cairo_surface_destroy(job->image);
	g_free(job);
}

static void sync_call_preview(void);

static void clear_overlay(enum media_mode next)
{
	if (next != CALL)
		ui_media_leave_fullscreen();
	if (m.mode == RECORDING)
		line_record_cancel();
	if (m.mode == EMOJI_PICKER)
		ui_palette_closed();
	if (m.recorded)
		line_record_discard(g_steal_pointer(&m.recorded));
	m.serial++;
	m.mode = next;
	sync_call_preview();
	m.friend_qr = m.avatar_pick = FALSE;
	if (m.paste)
		g_unlink(m.paste);
	g_clear_pointer(&m.paste, g_free);
	m.copying = m.sticker = FALSE;
	m.working = FALSE;
	g_clear_pointer(&m.chat, g_free);
	g_clear_pointer(&m.id, g_free);
	g_clear_pointer(&m.path, g_free);
	g_clear_pointer(&m.title, g_free);
	g_clear_pointer(&m.note, g_free);
	g_clear_pointer(&m.image, cairo_surface_destroy);
	g_clear_pointer(&m.entries, g_ptr_array_unref);
	if (m.row) {
		json_object_put(m.row);
		m.row = NULL;
	}
	ui.dirty = TRUE;
}

static void close_overlay(void)
{
	clear_overlay(CLOSED);
	ui_restore_input();
}

static void begin(enum media_mode mode, const char *chat, const char *id,
		  const char *title)
{
	g_autofree char *ch = g_strdup(chat), *mid = g_strdup(id),
			*name = g_strdup(title);
	clear_overlay(mode);
	set(&m.chat, ch);
	set(&m.id, mid);
	set(&m.title, name);
	/* The call stage leaves the composer usable. */
	if (mode == CALL || mode == DIAL)
		ui_restore_input();
	else
		SDL_StopTextInput();
}

void ui_media_album_photo(const char *album, const char *id)
{
	if (!ui.selected || !album || !id)
		return;
	begin(VIEW, ui.selected, id, "Album photo");
	m.working = TRUE;
	Json *p = args(ui.selected, id);

	json_object_object_add(p, "album", json_object_new_string(album));
	ui_rpc("sharedPhoto", p, m.serial);
	ui.dirty = TRUE;
}

static gboolean photo_kind(Json *row)
{
	const char *t = ui_string(row, "type", "");

	return !strcmp(t, "photo") || !strcmp(t, "sticker");
}

gboolean ui_is_photo(Json *row)
{
	return ui.photos && photo_kind(row);
}

gboolean ui_is_video(Json *row)
{
	return ui.files && !strcmp(ui_string(row, "type", ""), "video");
}

static gboolean file_kind(Json *row)
{
	const char *t = ui_string(row, "type", "");

	return !strcmp(t, "video") || !strcmp(t, "audio") || !strcmp(t, "file");
}

static int open_regular(const char *path, gsize bound)
{
	if (!path || !g_path_is_absolute(path))
		return -1;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	struct stat st;

	if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
			st.st_size <= 0 || (guint64)st.st_size > bound)) {
		close(fd);
		return -1;
	}
	return fd;
}

static gboolean image_shape(int w, int h)
{
	return w > 0 && h > 0 && w <= 32768 && h <= 32768 &&
	       (guint64)w * h <= 32 * 1000 * 1000;
}

static void fit(int w, int h, int limit, int *ow, int *oh)
{
	double scale = MIN(1., (double)limit / MAX(w, h));
	*ow = MAX(1, (int)(w * scale));
	*oh = MAX(1, (int)(h * scale));
}

/* LINE_DEBUG_MEDIA=1: each Flex or rich picture outcome, once per URL. */
static void media_debug(const char *what, const char *url)
{
	static GHashTable *seen;
	static int enabled = -1;

	if (enabled < 0)
		enabled = !g_strcmp0(g_getenv("LINE_DEBUG_MEDIA"), "1");
	if (!enabled || !url)
		return;
	if (!seen)
		seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					     NULL);
	g_autofree char *key = g_strconcat(what, " ", url, NULL);

	if (g_hash_table_add(seen, g_steal_pointer(&key)))
		g_printerr("line-media %s: %s\n", what, url);
}

static cairo_surface_t *new_image(int w, int h)
{
	cairo_surface_t *s =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

static cairo_surface_t *downsample(cairo_surface_t *s, int limit)
{
	int w = cairo_image_surface_get_width(s),
	    h = cairo_image_surface_get_height(s), nw, nh;
	fit(w, h, limit, &nw, &nh);
	if (w == nw && h == nh)
		return s;
	cairo_surface_t *small = new_image(nw, nh);

	if (small) {
		cairo_t *c = cairo_create(small);

		cairo_scale(c, (double)nw / w, (double)nh / h);
		cairo_set_source_surface(c, s, 0, 0);
		/* Area-averaged: bilinear aliases text when shrinking 2x+. */
		cairo_pattern_set_filter(cairo_get_source(c),
					 CAIRO_FILTER_GOOD);
		cairo_paint(c);
		cairo_destroy(c);
	}
	cairo_surface_destroy(s);
	return small;
}

/* Read just the TIFF orientation entry from JPEG APP1, without retaining metadata. */
static guint read16(const guint8 *b, gboolean little)
{
	return little ? (b[0] | b[1] << 8) : (b[0] << 8 | b[1]);
}

static guint32 read32(const guint8 *b, gboolean little)
{
	return little ? (read16(b, TRUE) |
			 ((guint32)read16(b + 2, TRUE) << 16)) :
			(((guint32)read16(b, FALSE) << 16) |
			 read16(b + 2, FALSE));
}

/*
 * The TIFF Orientation tag (0x112, one SHORT) inside an APP1 Exif block,
 * or 0 when the block has none.
 */
static int exif_orientation(const guint8 *t, gsize len)
{
	gboolean le = !memcmp(t, "II", 2);

	if ((!le && memcmp(t, "MM", 2)) || read16(t + 2, le) != 42)
		return 0;
	guint32 offset = read32(t + 4, le);

	if (offset > len - 2)
		return 0;
	guint entries = read16(t + offset, le);
	gsize at = (gsize)offset + 2;

	for (guint k = 0; k < entries && at + 12 <= len; k++, at += 12) {
		if (read16(t + at, le) == 0x112 &&
		    read16(t + at + 2, le) == 3 &&
		    read32(t + at + 4, le) == 1) {
			int o = read16(t + at + 8, le);

			return o >= 1 && o <= 8 ? o : 1;
		}
	}
	return 0;
}

static int jpeg_orientation(const guint8 *b, gsize size)
{
	for (gsize pos = 2; pos + 4 <= size;) {
		if (b[pos++] != 0xff)
			break;
		while (pos < size && b[pos] == 0xff)
			pos++;
		if (pos + 3 > size)
			break;
		guint marker = b[pos++];

		if (marker == 0xda || marker == 0xd9)
			break;
		guint n = read16(b + pos, FALSE);

		if (n < 2 || n > size - pos)
			break;
		int o = marker == 0xe1 && n >= 16 &&
					!memcmp(b + pos + 2, "Exif\0\0", 6) ?
				exif_orientation(b + pos + 8, n - 8) :
				0;

		if (o)
			return o;
		pos += n;
	}
	return 1;
}

static cairo_surface_t *orient(cairo_surface_t *image, int o)
{
	if (!image || o == 1)
		return image;
	int w = cairo_image_surface_get_width(image),
	    h = cairo_image_surface_get_height(image);
	cairo_matrix_t m[] = { { 0 },
			       { 1, 0, 0, 1, 0, 0 },
			       { -1, 0, 0, 1, w, 0 },
			       { -1, 0, 0, -1, w, h },
			       { 1, 0, 0, -1, 0, h },
			       { 0, 1, 1, 0, 0, 0 },
			       { 0, 1, -1, 0, h, 0 },
			       { 0, -1, -1, 0, h, w },
			       { 0, -1, 1, 0, 0, w } };
	cairo_surface_t *out = new_image(o < 5 ? w : h, o < 5 ? h : w);

	if (out) {
		cairo_t *c = cairo_create(out);

		cairo_transform(c, &m[o]);
		cairo_set_source_surface(c, image, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(c),
					 CAIRO_FILTER_NEAREST);
		cairo_paint(c);
		cairo_destroy(c);
	}
	cairo_surface_destroy(image);
	return out;
}

static cairo_surface_t *decode_image(guint8 *data, gsize size, int limit,
				     int max_edge)
{
	if (size < 12)
		return NULL;
	int w = 0, h = 0, outw, outh;
	cairo_surface_t *s = NULL;

	if (data[0] == 0xff && data[1] == 0xd8) {
		tjhandle decoder = tjInitDecompress();
		int subsamp, cs, nscale;

		if (!decoder)
			return NULL;
		/* DCT scaling decodes big JPEGs small: bound the output. */
		static const tjscalingfactor eighth = { 1, 8 };

		if (!tjDecompressHeader3(decoder, data, size, &w, &h, &subsamp,
					 &cs) &&
		    image_shape(w, h) &&
		    MAX(TJSCALED(w, eighth), TJSCALED(h, eighth)) <= max_edge) {
			tjscalingfactor *factors = tjGetScalingFactors(&nscale);
			int chosen = MAX(w, h) + 1;

			outw = w;
			outh = h;
			for (int i = 0; i < nscale; i++) {
				int x = TJSCALED(w, factors[i]),
				    y = TJSCALED(h, factors[i]);
				if (MAX(x, y) >= MIN(MAX(w, h), limit) &&
				    MAX(x, y) < chosen) {
					outw = x;
					outh = y;
					chosen = MAX(x, y);
				}
			}
			s = MAX(outw, outh) <= max_edge ?
				    new_image(outw, outh) :
				    NULL;
			if (s &&
			    tjDecompress2(
				    decoder, data, size,
				    cairo_image_surface_get_data(s), outw,
				    cairo_image_surface_get_stride(s), outh,
				    UI_LITTLE_ENDIAN ? TJPF_BGRA : TJPF_ARGB,
				    0) < 0) {
				cairo_surface_destroy(s);
				s = NULL;
			}
		}
		tjDestroy(decoder);
	} else if (!png_sig_cmp(data, 0, 8)) {
		png_image im = { .version = PNG_IMAGE_VERSION };

		if (png_image_begin_read_from_memory(&im, data, size) &&
		    image_shape(im.width, im.height) &&
		    MAX(im.width, im.height) <= (guint)max_edge) {
			gboolean alpha = (im.format & PNG_FORMAT_FLAG_ALPHA) !=
					 0;
			im.format = UI_LITTLE_ENDIAN ? PNG_FORMAT_BGRA :
						       PNG_FORMAT_ARGB;
			s = new_image(im.width, im.height);
			if (s &&
			    !png_image_finish_read(
				    &im, NULL, cairo_image_surface_get_data(s),
				    cairo_image_surface_get_stride(s), NULL)) {
				cairo_surface_destroy(s);
				s = NULL;
			}
			if (s && alpha)
				ui_premultiply_argb(
					(guint32 *)cairo_image_surface_get_data(
						s),
					(gsize)im.width * im.height);
		}
		png_image_free(&im);
	} else if (!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4) &&
		   WebPGetInfo(data, size, &w, &h) && image_shape(w, h) &&
		   MAX(w, h) <= max_edge) {
		WebPDecoderConfig c;

		if (!WebPInitDecoderConfig(&c))
			return NULL;
		fit(w, h, limit, &outw, &outh);
		s = new_image(outw, outh);
		if (s) {
			c.options.use_scaling = 1;
			c.options.scaled_width = outw;
			c.options.scaled_height = outh;
			c.output.colorspace = UI_LITTLE_ENDIAN ? MODE_bgrA :
								 MODE_Argb;
			c.output.is_external_memory = 1;
			c.output.u.RGBA.rgba = cairo_image_surface_get_data(s);
			c.output.u.RGBA.stride =
				cairo_image_surface_get_stride(s);
			c.output.u.RGBA.size =
				(gsize)c.output.u.RGBA.stride * outh;
			if (WebPDecode(data, size, &c) != VP8_STATUS_OK) {
				cairo_surface_destroy(s);
				s = NULL;
			}
			WebPFreeDecBuffer(&c.output);
		}
	}
	if (s) {
		cairo_surface_mark_dirty(s);
		s = downsample(s, limit);
		if (data[0] == 0xff && data[1] == 0xd8)
			s = orient(s, jpeg_orientation(data, size));
	}
	return s;
}

#define IMAGE_BOUND ((gsize)64 * 1024 * 1024)

static cairo_surface_t *decode_pixels(guint8 *bytes, gsize n, int limit,
				      int max_edge, gboolean animate)
{
	cairo_surface_t *image = decode_image(bytes, n, limit, max_edge);

	if (animate && image)
		ui_apng_attach(image, bytes, n);
	return image;
}

static cairo_surface_t *read_pixels(int fd, gsize n, int limit, int max_edge,
				    gboolean animate)
{
	int seals = fcntl(fd, F_GET_SEALS);
	int required = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW;

	if (seals >= 0 && (seals & required) == required) {
		void *p = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);

		if (p != MAP_FAILED) {
			cairo_surface_t *image =
				decode_pixels(p, n, limit, max_edge, animate);
			munmap(p, n);
			return image;
		}
	}
	g_autofree guint8 *bytes = g_try_malloc(n);

	if (!bytes)
		return NULL;
	for (gsize at = 0; at < n;) {
		ssize_t got = pread(fd, bytes + at, MIN(n - at, 65536), at);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return NULL;
		at += got;
	}
	return decode_pixels(bytes, n, limit, max_edge, animate);
}

/*
 * sdl2-compat owns an initialized SDL3 video backend. Its public MIME
 * clipboard API can publish PNG data on X11 and Wayland using the current
 * window's display connection. A native SDL2 backend still supports text.
 */
typedef const void *(*ClipboardData)(void *, const char *, size_t *);
typedef bool (*SetClipboardData)(ClipboardData, void (*)(void *), void *,
				 const char *const *, size_t);

static SetClipboardData image_clipboard(void)
{
	static SetClipboardData publish;
	static gboolean checked;

	if (!checked) {
		checked = TRUE;
		const char *driver = SDL_GetCurrentVideoDriver();

		if (g_strcmp0(driver, "x11") && g_strcmp0(driver, "wayland"))
			return NULL;
		void *sdl3 = dlopen("libSDL3.so.0",
				    RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
		if (sdl3) {
			guint32 (*initialized)(guint32 flags);
			void *init = dlsym(sdl3, "SDL_WasInit"),
			     *set_data = dlsym(sdl3, "SDL_SetClipboardData");
			memcpy(&initialized, &init, sizeof(initialized));
			if (initialized &&
			    (initialized(SDL_INIT_VIDEO) & SDL_INIT_VIDEO))
				memcpy(&publish, &set_data, sizeof(publish));
			/* The already-active SDL video backend retains its own reference. */
			dlclose(sdl3);
		}
	}
	return publish;
}

/* Reading uses the same SDL3 clipboard: any offered MIME type. */
struct clipboard_reader {
	char **(*types)(size_t *count);
	void *(*get)(const char *mime, size_t *size);
	void (*release)(void *memory);
};

static const struct clipboard_reader *clipboard_reader(void)
{
	static struct clipboard_reader reader;
	static gboolean checked, usable;

	if (checked)
		return usable ? &reader : NULL;
	checked = TRUE;
	const char *driver = SDL_GetCurrentVideoDriver();

	if (g_strcmp0(driver, "x11") && g_strcmp0(driver, "wayland"))
		return NULL;
	void *sdl3 =
		dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);

	if (!sdl3)
		return NULL;
	guint32 (*initialized)(guint32 flags);
	void *init = dlsym(sdl3, "SDL_WasInit"),
	     *types = dlsym(sdl3, "SDL_GetClipboardMimeTypes"),
	     *get = dlsym(sdl3, "SDL_GetClipboardData"),
	     *release = dlsym(sdl3, "SDL_free");

	memcpy(&initialized, &init, sizeof(initialized));
	if (initialized && types && get && release &&
	    (initialized(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
		memcpy(&reader.types, &types, sizeof(reader.types));
		memcpy(&reader.get, &get, sizeof(reader.get));
		memcpy(&reader.release, &release, sizeof(reader.release));
		usable = TRUE;
	}
	dlclose(sdl3);
	return usable ? &reader : NULL;
}

static const void *clipboard_bytes(void *source, const char *mime,
				   size_t *length)
{
	if (!length)
		return NULL;
	if (!mime || strcmp(mime, "image/png")) {
		*length = 0;
		return NULL;
	}
	return g_bytes_get_data(source, length);
}

static void clipboard_release(void *source)
{
	g_bytes_unref(source);
}

static cairo_status_t write_clipboard_png(void *opaque,
					  const unsigned char *bytes,
					  unsigned int count)
{
	GByteArray *out = opaque;

	if (count > 64u * 1024 * 1024 - out->len)
		return CAIRO_STATUS_WRITE_ERROR;
	g_byte_array_append(out, bytes, count);
	return CAIRO_STATUS_SUCCESS;
}

static void decoder(gpointer ptr, gpointer unused)
{
	(void)unused;
	struct decode_job *job = ptr;
	struct stat st;

	if (!fstat(job->fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
	    (guint64)st.st_size <= IMAGE_BOUND) {
		int limit = job->role == CLIPBOARD ? G_MAXINT :
			    job->role == THUMBNAIL ? THUMB_EDGE :
			    job->role == AVATAR	   ? AVATAR_EDGE :
			    job->edge		   ? job->edge :
			    job->role == EMOJI ?
					(g_str_has_prefix(job->id, "s:") ?
						 STICKER_EDGE :
						 EMOJI_EDGE) :
					1800;
		job->image = read_pixels(job->fd, (gsize)st.st_size, limit,
					 G_MAXINT, job->animate);
	}
	if (job->role == CLIPBOARD && job->image) {
		GByteArray *out = g_byte_array_new();

		if (cairo_surface_write_to_png_stream(
			    job->image, write_clipboard_png, out) ==
		    CAIRO_STATUS_SUCCESS)
			job->png = g_byte_array_free_to_bytes(out);
		else {
			g_byte_array_unref(out);
			job->reason = g_strdup(
				"Could not encode this image for the clipboard.");
		}
	}
	if (job->role == FRIEND_QR && job->image) {
		g_autoptr(GError) error = NULL;

		job->id = line_qr_scan(job->image, &error);
		if (!job->id)
			job->reason = g_strdup(
				error ? error->message :
					"Choose a clear image with one personal LINE QR code.");
	}
	close(job->fd);
	job->fd = -1;
	if (!job->image)
		job->reason = g_strdup(
			"Use a JPEG, PNG or WebP image up to 32 MiB and 32 megapixels.");
	SDL_Event e = { 0 };

	e.type = m.decode_event;
	e.user.data1 = job;
	if (SDL_PushEvent(&e) != 1)
		decoded_free(job);
}

static int flex_edge(void)
{
	return CLAMP((int)(FLEX_VIEW * ui_pixel_scale() + .5), FLEX_VIEW,
		     FLEX_MAX_EDGE);
}

static gboolean start_decode(int fd, enum decode_role role, const char *id,
			     guint serial, gboolean animate)
{
	if (fd < 0)
		return FALSE;
	if (!m.pool) {
		m.decode_event = SDL_RegisterEvents(1);
		if (m.decode_event != (Uint32)-1)
			m.pool = g_thread_pool_new(decoder, NULL, 1, FALSE,
						   NULL);
	}
	if (!m.pool) {
		close(fd);
		return FALSE;
	}
	struct decode_job *job = g_new0(struct decode_job, 1);

	job->fd = fd;
	job->epoch = (role == AVATAR || role == EMOJI || role == FLEX_IMAGE) ?
			     m.session :
			     m.epoch;
	job->serial = serial;
	job->role = role;
	job->animate = animate;
	job->edge = role == FLEX_IMAGE ? flex_edge() : 0;
	job->id = g_strdup(id);
	if (!g_thread_pool_push(m.pool, job, NULL)) {
		decoded_free(job);
		return FALSE;
	}
	return TRUE;
}

static void complete_decode(struct decode_job *job)
{
	if (job->role == CLIPBOARD) {
		if (m.copying && m.mode == VIEW && job->serial == m.serial &&
		    job->epoch == m.epoch) {
			const char *const mime[] = { "image/png" };
			SetClipboardData publish = image_clipboard();
			gboolean ok = publish && job->png &&
				      publish(clipboard_bytes,
					      clipboard_release,
					      g_bytes_ref(job->png), mime, 1);
			close_overlay();
			ui_status(ok	      ? "Image copied." :
				  job->reason ? job->reason :
						"Could not copy this image.");
		}
		decoded_free(job);
		return;
	}
	if (job->role == FRIEND_QR) {
		if (job->serial == m.serial && m.mode == PICK && m.friend_qr) {
			close_overlay();
			ui_friends_scan_result(job->id, job->reason);
		}
		decoded_free(job);
		ui.dirty = TRUE;
		return;
	}
	if (job->role == FLEX_IMAGE && job->epoch == m.session) {
		struct media_thumbnail *image =
			m.flex_images ?
				g_hash_table_lookup(m.flex_images, job->id) :
				NULL;

		if (image && image->serial == job->serial) {
			if (!job->image)
				media_debug("could not decode", job->id);
			store_thumbnail(image, &job->image);
			m.flex_busy = FALSE;
		}
	} else if ((job->role == AVATAR || job->role == EMOJI) &&
		   job->epoch == m.session) {
		GHashTable *table = job->role == EMOJI ? m.emoji : m.avatars;
		struct media_thumbnail *t =
			table ? g_hash_table_lookup(table, job->id) : NULL;
		if (t && t->serial == job->serial) {
			store_thumbnail(t, &job->image);
			set(&t->reason, job->reason);
		}
		if (job->role == EMOJI) {
			m.emoji_busy = FALSE;
			if (job->reason)
				ui_forget_history();
		} else
			m.avatar_busy = FALSE;
	} else if (job->epoch == m.epoch && job->role == THUMBNAIL) {
		struct media_thumbnail *t =
			m.thumbs ? g_hash_table_lookup(m.thumbs, job->id) :
				   NULL;
		if (t) {
			store_thumbnail(t, &job->image);
			set(&t->reason, job->reason);
		}
		m.thumb_busy = FALSE;
	} else if ((job->role == ORIGINAL || job->role == LOCAL) &&
		   job->epoch == m.epoch && job->serial == m.serial &&
		   (m.mode == PREVIEW || m.mode == VIEW)) {
		m.image = job->image;
		job->image = NULL;
		m.working = FALSE;
		set(&m.note, job->reason);
	}
	decoded_free(job);
	ui.dirty = TRUE;
}

void ui_media_select_chat(void)
{
	ui_find_reset();
	ui_chat_tools_reset();
	ui_shared_reset();
	if (m.screenshot_cancel)
		g_cancellable_cancel(m.screenshot_cancel);
	m.epoch++;
	m.thumb_busy = FALSE;
	if (m.thumbs)
		g_hash_table_remove_all(m.thumbs);
	if (m.mode != CALL && !m.friend_qr)
		clear_overlay(CLOSED);
}

void ui_media_reset(void)
{
	ui_media_select_chat();
	clear_overlay(CLOSED);
	ui_profile_reset();
	g_clear_pointer(&m.playback, line_video_play_free);
	if (m.uploads)
		g_hash_table_remove_all(m.uploads);
	m.session++;
	ui_emoji_reset();
	ui_friends_reset();
	ui_groups_reset();
	ui.find_messages = ui.friends = ui.friend_invite = ui.groups = FALSE;
	m.emoji_busy = FALSE;
	ui.emojis = ui.stickers = FALSE;
	if (m.emoji)
		g_hash_table_remove_all(m.emoji);
	m.avatar_busy = FALSE;
	if (m.avatars)
		g_hash_table_remove_all(m.avatars);
	g_clear_pointer(&m.flex_images, g_hash_table_unref);
	m.flex_busy = FALSE;
	g_clear_pointer(&m.call_chat, g_free);
	g_clear_pointer(&m.call_name, g_free);
	g_clear_pointer(&m.call_state, g_free);
	g_clear_pointer(&m.call_error, g_free);
	g_clear_pointer(&m.call_note, g_free);
	m.call_serial++;
	m.call_working = FALSE;
	clear_call_frame();
	m.video_call = m.video_ready = m.camera_enabled = FALSE;
	g_clear_pointer(&ui.self_id, g_free);
	g_clear_pointer(&ui.reply_id, g_free);
	g_clear_pointer(&ui.reply_chat, g_free);
	g_clear_pointer(&m.reply_text, g_free);
	m.muted = FALSE;
	ui.avatars = ui.photos = ui.files = ui.calls = ui.video_calls =
		ui.incoming_calls = ui.mute_calls = FALSE;
}

struct recording_event {
	char *path, *error;
	guint serial;
};

struct clip_event {
	char *error;
	guint serial;
};

static void free_clip_event(struct clip_event *clip)
{
	g_free(clip->error);
	g_free(clip);
}

static void clip_finished(const char *error, gpointer opaque)
{
	struct clip_event *clip = g_new(struct clip_event, 1);
	SDL_Event event = { 0 };

	clip->error = g_strdup(error);
	clip->serial = GPOINTER_TO_UINT(opaque);
	event.type = m.play_event;
	event.user.data1 = clip;
	if (SDL_PushEvent(&event) != 1)
		free_clip_event(clip);
}

/* The player owns fd from this point, including when it fails to start. */
static gboolean play_clip(int fd, gboolean review)
{
	if (!m.play_event)
		m.play_event = SDL_RegisterEvents(1);
	if (m.play_event == (Uint32)-1) {
		close(fd);
		set(&m.note, "Could not start the video player monitor.");
		return FALSE;
	}
	g_clear_pointer(&m.playback, line_video_play_free);
	m.play_serial++;
	m.play_overlay = m.serial;
	m.play_review = review;
	g_autoptr(GError) error = NULL;

	m.playback = line_video_play(fd, ui.renderer || ui_gpu_active(),
				     clip_finished,
				     GUINT_TO_POINTER(m.play_serial), &error);
	if (!m.playback) {
		set(&m.note, error ? error->message :
				     "Could not start the video player.");
		return FALSE;
	}
	set(&m.note, NULL);
	return TRUE;
}

struct screenshot_event {
	char *parent, *path, *error;
	GCancellable *cancel;
	guint type, epoch;
};

static void free_screenshot_event(struct screenshot_event *event)
{
	g_clear_object(&event->cancel);
	g_free(event->parent);
	g_free(event->path);
	g_free(event->error);
	g_free(event);
}

static gpointer take_screenshot(gpointer data)
{
	struct screenshot_event *out = data;

	g_autoptr(GError) error = NULL;

	out->path = line_desktop_screenshot(out->parent, out->cancel, &error);
	out->error = g_strdup(error ? error->message : NULL);
	SDL_Event e = { 0 };

	e.type = out->type;
	e.user.data1 = out;
	return SDL_PushEvent(&e) == 1 ? NULL : out;
}

void ui_media_screenshot(void)
{
	if (!ui.selected || !ui.photos || m.mode != CLOSED)
		return;
	if (m.screenshot_thread) {
		ui_status("A screenshot request is already open.");
		return;
	}
	if (!m.screenshot_event)
		m.screenshot_event = SDL_RegisterEvents(1);
	if (m.screenshot_event == (Uint32)-1) {
		ui_status("Could not start a screenshot request.");
		return;
	}
	struct screenshot_event *out = g_new0(struct screenshot_event, 1);
	SDL_SysWMinfo window;

	SDL_VERSION(&window.version);
#ifdef SDL_VIDEO_DRIVER_X11
	if (SDL_GetWindowWMInfo(ui.window, &window) &&
	    window.subsystem == SDL_SYSWM_X11)
		out->parent =
			g_strdup_printf("x11:%lx", window.info.x11.window);
#endif
	if (!out->parent)
		out->parent = g_strdup("");
	m.screenshot_cancel = g_cancellable_new();
	out->cancel = g_object_ref(m.screenshot_cancel);
	out->type = m.screenshot_event;
	out->epoch = m.epoch;
	m.screenshot_thread =
		g_thread_try_new("line-screenshot", take_screenshot, out, NULL);
	if (!m.screenshot_thread) {
		free_screenshot_event(out);
		g_clear_object(&m.screenshot_cancel);
		ui_status("Could not start a screenshot request.");
	}
}

static void free_record_event(struct recording_event *event)
{
	line_record_discard(event->path);
	g_free(event->error);
	g_free(event);
}

static void record_finished(char *path, char *error, gpointer state)
{
	struct recording_event *out = g_new0(struct recording_event, 1);

	out->path = path;
	out->error = error;
	out->serial = GPOINTER_TO_UINT(state);
	SDL_Event e = { 0 };

	e.type = m.record_event;
	e.user.data1 = out;
	if (SDL_PushEvent(&e) != 1)
		free_record_event(out);
}

void ui_media_shutdown(void)
{
	ui_media_reset();
	if (m.play_event && m.play_event != (Uint32)-1) {
		SDL_Event event;

		while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, m.play_event,
				      m.play_event) == 1)
			free_clip_event(event.user.data1);
	}
	if (m.screenshot_thread) {
		struct screenshot_event *out =
			g_thread_join(m.screenshot_thread);
		SDL_Event event;

		if (out)
			free_screenshot_event(out);
		while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,
				      m.screenshot_event,
				      m.screenshot_event) == 1)
			free_screenshot_event(event.user.data1);
		m.screenshot_thread = NULL;
		g_clear_object(&m.screenshot_cancel);
	}
	line_record_shutdown();
	if (m.record_event) {
		SDL_Event ev;

		while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, m.record_event,
				      m.record_event) == 1)
			free_record_event(ev.user.data1);
	}
	g_clear_pointer(&m.uploads, g_hash_table_unref);
	g_clear_pointer(&m.paste_sent, g_hash_table_unref);
	if (m.paste)
		g_unlink(m.paste);
	g_clear_pointer(&m.paste, g_free);
	if (m.pool) {
		g_thread_pool_free(m.pool, FALSE, TRUE);
		SDL_Event ev;

		while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, m.decode_event,
				      m.decode_event) == 1)
			decoded_free(ev.user.data1);
		m.pool = NULL;
	}
	g_clear_pointer(&m.thumbs, g_hash_table_unref);
	g_clear_pointer(&m.avatars, g_hash_table_unref);
	g_clear_pointer(&m.emoji, g_hash_table_unref);
	g_clear_pointer(&m.directory, g_free);
	if (m.edit.value) {
		g_string_free(m.edit.value, TRUE);
		m.edit.value = NULL;
	}
}

void ui_media_login(Json *r)
{
	Json *c = ui_field(r, "capabilities");

	ui.avatars = json_object_get_boolean(ui_field(c, "avatars"));
	ui.emojis = json_object_get_boolean(ui_field(c, "emojis"));
	ui.stickers = json_object_get_boolean(ui_field(c, "stickers"));
	ui.find_messages = json_object_get_boolean(ui_field(c, "findMessages"));
	ui.friends = json_object_get_boolean(ui_field(c, "friends"));
	ui.friend_invite = json_object_get_boolean(ui_field(c, "friendInvite"));
	ui.groups = json_object_get_boolean(ui_field(c, "groups"));
	if (!m.emoji)
		m.emoji = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						thumb_free);
	if (!m.avatars)
		m.avatars = g_hash_table_new_full(g_str_hash, g_str_equal,
						  g_free, thumb_free);
	ui.photos = json_object_get_boolean(ui_field(c, "photos"));
	ui.files = json_object_get_boolean(ui_field(c, "files"));
	ui.video_calls = json_object_get_boolean(ui_field(c, "videoCalls"));
	ui.calls = json_object_get_boolean(ui_field(c, "calls"));
	ui.incoming_calls =
		json_object_get_boolean(ui_field(c, "incomingCalls"));
	ui.mute_calls = json_object_get_boolean(ui_field(c, "muteCalls"));
	set(&ui.self_id, ui_string(r, "selfId", ""));
	if (!m.thumbs)
		m.thumbs = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, thumb_free);
}

gboolean ui_media_can_call(void)
{
	return ui.calls && ui.selected && *ui.selected == 'u' &&
	       g_strcmp0(ui.selected, ui.self_id);
}

static gint64 emoji_until, emoji_now;

static cairo_surface_t *animation_frame(cairo_surface_t *poster)
{
	gint64 until = 0;

	if (!poster || ui_reduce_motion())
		return poster;
	cairo_surface_t *frame = ui_apng_frame(poster, emoji_now, &until);

	if (until && (!emoji_until || until < emoji_until))
		emoji_until = until;
	return frame;
}

static struct media_thumbnail *thumbnail(Json *row, const char *method)
{
	const char *id = ui_string(row, "id", "");

	if (!*id || !m.thumbs)
		return NULL;
	struct media_thumbnail *t = g_hash_table_lookup(m.thumbs, id);

	if (!t && !m.thumb_busy) {
		t = reserve_thumbnail(m.thumbs, 48, THUMB_EDGE);
		if (!t)
			return NULL;
		t->sticker = !strcmp(ui_string(row, "type", ""), "sticker");
		g_hash_table_insert(m.thumbs, g_strdup(id), t);
		m.thumb_busy = TRUE;
		ui_rpc(method, args(ui.selected, id), 0);
	}
	if (t)
		touch(t);
	return t;
}

void ui_media_video_poster(Json *row, Rect area)
{
	struct media_thumbnail *poster = thumbnail(row, "videoPoster");

	if (poster && poster->image)
		ui_draw_image(poster->image, area);
}

void ui_media_thumb(Json *row, Rect r)
{
	struct media_thumbnail *t = thumbnail(row, "photo");

	if (t && t->image)
		ui_draw_image(t->sticker ? animation_frame(t->image) : t->image,
			      r);
	else
		ui_draw_label(t && t->reason ? "Open image to retry" :
					       "Loading image…",
			      (Rect){ r.x + 8, r.y + r.h / 2 - 10, r.w - 16,
				      24 },
			      13, 500, MUTED, FALSE);
}

cairo_surface_t *ui_avatar(const char *chat, const char *id, const char *key)
{
	if (!ui.avatars || !m.avatars || !chat || !id || !*id)
		return NULL;
	struct media_thumbnail *t = g_hash_table_lookup(m.avatars, id);

	if (t && key && g_strcmp0(key, t->version)) {
		g_hash_table_remove(m.avatars, id);
		t = NULL;
	}
	if (key && !*key)
		return NULL;
	if (!t && !m.avatar_busy) {
		t = reserve_thumbnail(m.avatars, 96, AVATAR_EDGE);
		if (!t)
			return NULL;
		t->serial = ++m.avatar_seq;
		t->version = g_strdup(key);
		g_hash_table_insert(m.avatars, g_strdup(id), t);
		m.avatar_busy = TRUE;
		Json *p = args(chat, id);

		if (key)
			json_object_object_add(p, "avatarKey",
					       json_object_new_string(key));
		ui_rpc("avatar", p, t->serial);
	}
	if (t)
		touch(t);
	return t ? t->image : NULL;
}

cairo_surface_t *ui_flex_image(const char *url)
{
	if (!ui.connected || !url)
		return NULL;
	struct media_thumbnail *entry =
		m.flex_images ? g_hash_table_lookup(m.flex_images, url) : NULL;

	if (!entry) {
		if (!m.flex_busy && !line_flex_image_allowed(url)) {
			media_debug("skipped (not a public HTTPS URL)", url);
			return NULL;
		}
		if (m.flex_busy)
			return NULL;
		if (!m.flex_images)
			m.flex_images = g_hash_table_new_full(
				g_str_hash, g_str_equal, g_free, thumb_free);
		entry = reserve_thumbnail(m.flex_images, 32, flex_edge());
		if (!entry)
			return NULL;
		entry->serial = ++m.avatar_seq;
		g_hash_table_insert(m.flex_images, g_strdup(url), entry);
		m.flex_busy = TRUE;
		Json *params = json_object_new_object();

		json_object_object_add(params, "url",
				       json_object_new_string(url));
		ui_rpc("flexImage", params, entry->serial);
	}
	touch(entry);
	return entry->image;
}

gboolean ui_flex_image_failed(const char *url)
{
	struct media_thumbnail *entry =
		m.flex_images && url ? g_hash_table_lookup(m.flex_images, url) :
				       NULL;

	return entry && entry->settled && !entry->image;
}

void ui_emoji_frame_start(gint64 now)
{
	emoji_now = now;
	emoji_until = 0;
	m.frame++;
}

gint64 ui_emoji_deadline(void)
{
	return emoji_until;
}

static gboolean decimal_id(const char *s)
{
	size_t n = strlen(s);

	return n && n <= 19 && s[0] != '0' && strspn(s, "0123456789") == n;
}

static gboolean palette_key(Json *meta, char key[96])
{
	const char *type = ui_string(meta, "resourceType", "STATIC");
	int version = json_object_get_int(ui_field(meta, "version"));
	const char *p, *id;

	if (ui_field(meta, "stickerId")) {
		p = ui_string(meta, "packageId", "");
		id = ui_string(meta, "stickerId", "");
		if (!ui.stickers || !decimal_id(p) || !decimal_id(id) ||
		    strlen(type) > 16)
			return FALSE;
		g_snprintf(key, 96, "s:%s/%s/%d/%s", p, id, version, type);
	} else {
		p = ui_string(meta, "productId", "");
		id = ui_string(meta, "emojiId", "");
		if (!ui.emojis || strlen(p) != 24 || strlen(id) != 3)
			return FALSE;
		g_snprintf(key, 96, "%s/%s/%d/%c", p, id, version,
			   !strcmp(type, "ANIMATION") ? 'A' : 'S');
	}
	return TRUE;
}

cairo_surface_t *ui_emoji_image(Json *meta, gboolean load, gboolean *failed)
{
	char key[96];

	if (!m.emoji || !palette_key(meta, key))
		return NULL;
	struct media_thumbnail *t = g_hash_table_lookup(m.emoji, key);

	if (!t && load && !m.emoji_busy) {
		t = reserve_thumbnail(m.emoji, 128,
				      g_str_has_prefix(key, "s:") ?
					      STICKER_EDGE :
					      EMOJI_EDGE);
		if (!t)
			return NULL;
		t->serial = ++m.avatar_seq;
		g_hash_table_insert(m.emoji, g_strdup(key), t);
		m.emoji_busy = TRUE;
		Json *p = json_object_new_object();
		const char *const f[] = {
			"productId", "emojiId", "packageId",
			"stickerId", "version", "resourceType"
		};
		for (guint i = 0; i < G_N_ELEMENTS(f); i++)
			if (ui_field(meta, f[i]))
				json_object_object_add(
					p, f[i],
					json_object_get(ui_field(meta, f[i])));
		ui_rpc(ui_field(meta, "stickerId") ? "sticker" : "emoji", p,
		       t->serial);
	}
	if (!t)
		return NULL;
	if (load)
		touch(t);
	if (failed)
		*failed = t->reason != NULL;
	return load ? animation_frame(t->image) : t->image;
}

static gboolean failed_emoji(gpointer k, gpointer v, gpointer data)
{
	(void)k;
	(void)data;
	return ((struct media_thumbnail *)v)->reason != NULL;
}

static void show_emoji(void)
{
	if (m.emoji && g_hash_table_foreach_remove(m.emoji, failed_emoji, NULL))
		ui_forget_history();
	begin(EMOJI_PICKER, ui.selected, NULL, "LINE emoji");
	ui_emoji_open();
}

void ui_emoji_pick_close(void)
{
	close_overlay();
	ui.focus = F_MESSAGE;
	ui_restore_input();
}

void ui_media_reply_sent(const char *chat)
{
	if (!g_strcmp0(chat, ui.reply_chat)) {
		g_clear_pointer(&ui.reply_id, g_free);
		g_clear_pointer(&ui.reply_chat, g_free);
		g_clear_pointer(&m.reply_text, g_free);
		ui.dirty = TRUE;
	}
}

static void picture(Json *row)
{
	const char *id = ui_string(row, "id", "");
	gboolean sticker = !strcmp(ui_string(row, "type", ""), "sticker");

	begin(VIEW, ui.selected, id, sticker ? "Sticker" : "Photo");
	m.sticker = sticker;
	m.working = TRUE;
	ui_rpc("photo", args(ui.selected, m.id), m.serial);
}

static void request_video_clip(void)
{
	m.working = TRUE;
	set(&m.note, "Downloading video…");
	ui_rpc("openVideo", args(m.chat, m.id), m.serial);
}

static void open_video(Json *row)
{
	Json *video = json_object_get(row);
	const char *id = ui_string(video, "id", "");

	begin(PLAY_VIDEO, ui.selected, id, "Play video");
	m.row = video;
	request_video_clip();
}

static gboolean is_image_path(const char *path)
{
	g_autofree char *lower = g_ascii_strdown(path, -1);

	return g_str_has_suffix(lower, ".jpeg") ||
	       g_str_has_suffix(lower, ".jpg") ||
	       g_str_has_suffix(lower, ".png") ||
	       g_str_has_suffix(lower, ".webp");
}

static void prepare_attachment(const char *path)
{
	if (!ui.selected)
		return;
	g_autofree char *copy = g_canonicalize_filename(path, m.directory);
	gboolean picture_file = ui.photos && is_image_path(copy);

	if (!picture_file && !ui.files) {
		set(&m.note, "Choose a JPEG, PNG or WebP photo.");
		return;
	}
	int fd = open_regular(copy, picture_file ? IMAGE_BOUND : G_MAXSIZE);

	if (fd < 0) {
		if (m.mode == CLOSED)
			ui_status("Could not open this attachment.");
		else
			set(&m.note,
			    "Could not open this attachment, or it exceeds the size limit.");
		return;
	}
	g_autofree char *name = g_path_get_basename(copy);

	begin(PREVIEW, ui.selected, NULL,
	      picture_file ? "Send photo" : "Send file");
	set(&m.path, copy);
	set(&m.title, name);
	m.photo = picture_file;
	if (picture_file) {
		m.working = start_decode(fd, LOCAL, NULL, m.serial, FALSE);
		if (!m.working)
			set(&m.note, "Could not read this image.");
	} else
		close(fd);
}

static void unlink_path(gpointer path)
{
	g_unlink(path);
	g_free(path);
}

/*
 * Ctrl+V in the message bar: a copied picture opens the photo preview, as
 * in Discord, and a file copied in a file manager opens as an attachment.
 * FALSE leaves the paste to ordinary text.
 */
gboolean ui_media_paste(void)
{
	const struct clipboard_reader *c = clipboard_reader();

	if (!c || !ui.selected || m.mode != CLOSED || (!ui.photos && !ui.files))
		return FALSE;
	size_t count = 0;
	char **types = c->types(&count);

	if (!types)
		return FALSE;
	const char *extension = NULL,
		   *offered = ui.photos ? line_paste_image_type(
						  (const char *const *)types,
						  count, &extension) :
					  NULL;
	g_autofree char *mime = g_strdup(offered);
	gboolean files = FALSE;

	for (size_t i = 0; i < count; i++)
		files |= types[i] &&
			 !g_ascii_strcasecmp(types[i], "text/uri-list");
	c->release(types);
	if (mime) {
		g_autoptr(GError) error = NULL;
		size_t n = 0;
		void *data = c->get(mime, &n);

		g_autofree char *path =
			data ? line_paste_save(data, n, mime, &error) : NULL;

		if (data)
			c->release(data);
		if (!path) {
			ui_status(error ? error->message :
					  "Could not read the pasted picture.");
			return TRUE;
		}
		prepare_attachment(path);
		if (m.mode == PREVIEW && !g_strcmp0(m.path, path)) {
			m.paste = g_steal_pointer(&path);
			set(&m.title, "Pasted image");
		} else {
			g_unlink(path);
		}
		return TRUE;
	}
	if (!files)
		return FALSE;
	size_t n = 0;
	void *data = c->get("text/uri-list", &n);
	g_autofree char *file = data ? line_paste_file(data, n) : NULL;

	if (data)
		c->release(data);
	if (!file)
		return FALSE;
	prepare_attachment(file);
	return TRUE;
}

static gint compare_item(gconstpointer a, gconstpointer b)
{
	struct file_item *x = *(struct file_item **)a,
			 *y = *(struct file_item **)b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return g_utf8_collate(x->name, y->name);
}

static void edit_path(const char *path)
{
	if (!m.edit.value)
		m.edit.value = g_string_new("");
	g_string_assign(m.edit.value, path);
	m.edit.cursor = m.edit.anchor = (int)m.edit.value->len;
	m.edit.scroll = 0;
}

static void directory(const char *folder)
{
	g_autofree char *path = g_canonicalize_filename(folder, NULL);

	g_autoptr(GError) e = NULL;
	GDir *dir = g_dir_open(path, 0, &e);

	if (!dir) {
		set(&m.note, e->message);
		return;
	}
	g_clear_pointer(&m.entries, g_ptr_array_unref);
	m.entries = g_ptr_array_new_with_free_func(item_free);
	set(&m.directory, path);
	const char *name;

	while (m.entries->len < 8192 && (name = g_dir_read_name(dir))) {
		if (*name == '.' || !g_utf8_validate(name, -1, NULL))
			continue;
		g_autofree char *full = g_build_filename(path, name, NULL);
		gboolean isdir = g_file_test(full, G_FILE_TEST_IS_DIR);

		if (!isdir && (m.friend_qr || m.avatar_pick || !ui.files) &&
		    !is_image_path(name))
			continue;
		struct file_item *item = g_new0(struct file_item, 1);

		item->name = g_strdup(name);
		item->dir = isdir;
		g_ptr_array_add(m.entries, item);
	}
	g_dir_close(dir);
	g_ptr_array_sort(m.entries, compare_item);
	m.cursor = -1;
	m.offset = 0;
	set(&m.note,
	    m.entries->len == 8192 ?
		    "Showing the first 8,192 entries. Enter a path to open another." :
		    NULL);
	if (m.save) {
		g_autofree char *full = g_build_filename(path, m.title, NULL);

		edit_path(full);
	} else
		edit_path(path);
}

static void open_picker(gboolean save, Json *row, gboolean friend_qr)
{
	g_autofree char *dir =
		g_strdup(m.directory ? m.directory : g_get_home_dir());
	const char *name = save ? ui_string(row, "filename", "attachment") :
				  "Choose an attachment";
	g_autofree char *base = g_path_get_basename(name);
	g_autofree char *id = g_strdup(ui_string(row, "id", ""));

	begin(PICK, ui.selected, id, base);
	m.save = save;
	m.friend_qr = friend_qr;
	directory(dir);
	SDL_StartTextInput();
}

void ui_friend_pick_qr(void)
{
	open_picker(FALSE, NULL, TRUE);
}

void ui_media_pick_avatar(void)
{
	open_picker(FALSE, NULL, FALSE);
	m.avatar_pick = TRUE;
	set(&m.title, "Choose a profile picture");
	directory(m.directory ? m.directory : g_get_home_dir());
}

void ui_friend_scan_file(const char *path)
{
	if (m.working)
		return;
	if (m.mode != PICK || !m.friend_qr) {
		begin(PICK, NULL, NULL, "Read QR image");
		m.friend_qr = TRUE;
	}
	int fd = open_regular(path, IMAGE_BOUND);

	if (fd < 0 || !start_decode(fd, FRIEND_QR, NULL, m.serial, FALSE)) {
		close_overlay();
		ui_friends_scan_result(NULL, "Could not open this QR image.");
		return;
	}
	m.working = TRUE;
	set(&m.note, "Reading QR code…");
}

static void save_to(gboolean overwrite)
{
	Json *p = args(m.chat, m.id);

	json_object_object_add(p, "path", json_object_new_string(m.path));
	if (overwrite)
		json_object_object_add(p, "replace",
				       json_object_new_boolean(TRUE));
	m.working = TRUE;
	set(&m.note, "Saving…");
	ui_rpc("saveFile", p, m.serial);
}

static void choose_path(void)
{
	if (!m.edit.value)
		return;
	g_autofree char *input = g_strdup(m.edit.value->str), *path = NULL;

	if (!strcmp(input, "~") || g_str_has_prefix(input, "~/"))
		path = g_build_filename(g_get_home_dir(), input + 1, NULL);
	else
		path = g_canonicalize_filename(input, m.directory);
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		directory(path);
		return;
	}
	if (m.friend_qr) {
		ui_friend_scan_file(path);
		return;
	}
	if (m.avatar_pick) {
		g_autofree char *chosen = g_strdup(path);

		clear_overlay(CLOSED);
		ui_profile_picture_chosen(chosen);
		return;
	}
	if (!m.save) {
		prepare_attachment(path);
		return;
	}
	set(&m.path, path);
	if (g_file_test(path, G_FILE_TEST_EXISTS)) {
		m.mode = REPLACE;
		set(&m.note, "Replace the existing file?");
		SDL_StopTextInput();
	} else
		save_to(FALSE);
}

static void choose_item(int pos, gboolean open)
{
	if (!m.entries || pos < 0 || (guint)pos >= m.entries->len)
		return;
	struct file_item *f = g_ptr_array_index(m.entries, pos);

	m.cursor = pos;
	g_autofree char *path = g_build_filename(m.directory, f->name, NULL);

	edit_path(path);
	if (open)
		choose_path();
	ui.dirty = TRUE;
}

enum message_action {
	A_REPLY,
	A_COPY,
	A_REACT,
	A_VIEW,
	A_SAVE,
	A_UNSEND,
	A_COPY_IMAGE,
	A_FORWARD,
	A_ANNOUNCE,
	A_MENTION,
	A_PLAY_VIDEO
};

static int actions(enum message_action *out)
{
	int n = 0;

	out[n++] = A_REPLY;
	const char *kind = ui_string(m.row, "type", "text");

	if (!json_object_get_boolean(ui_field(m.row, "unreadable")) &&
	    (!strcmp(kind, "text") || !strcmp(kind, "link") ||
	     !strcmp(kind, "location") || !strcmp(kind, "contact")) &&
	    *ui_string(m.row, "text", ""))
		out[n++] = A_COPY;
	out[n++] = A_REACT;
	if (ui_is_photo(m.row)) {
		out[n++] = A_VIEW;
		if (image_clipboard())
			out[n++] = A_COPY_IMAGE;
	}
	if (ui_is_video(m.row))
		out[n++] = A_PLAY_VIDEO;
	if (ui.files && (file_kind(m.row) || photo_kind(m.row)))
		out[n++] = A_SAVE;
	gboolean text =
		!strcmp(kind, "text") &&
		!json_object_get_boolean(ui_field(m.row, "unreadable")) &&
		*ui_string(m.row, "text", "");

	if (text || !strcmp(kind, "photo") || file_kind(m.row))
		out[n++] = A_FORWARD;
	if (text)
		out[n++] = A_ANNOUNCE;
	if (ui.selected && ui.selected[0] == 'c' &&
	    !json_object_get_boolean(ui_field(m.row, "own")) &&
	    ui_string(m.row, "senderId", "")[0] == 'u')
		out[n++] = A_MENTION;
	if (json_object_get_boolean(ui_field(m.row, "own")))
		out[n++] = A_UNSEND;
	return n;
}

static const char *const reaction_names[] = { "nice", "love", "fun", "amazing",
					      "sad",  "omg",  "none" };
static const char *const reaction_labels[] = {
	"Like", "Love",	     "Laugh",	       "Amazing",
	"Sad",	"Surprised", "Remove reaction"
};

static void menu_action(enum message_action a)
{
	switch (a) {
	case A_REPLY:
		set(&ui.reply_chat, m.chat);
		set(&ui.reply_id, m.id);
		set(&m.reply_text, ui_string(m.row, "from", "message"));
		close_overlay();
		break;
	case A_COPY: {
		gboolean ok =
			SDL_SetClipboardText(ui_string(m.row, "text", "")) == 0;
		close_overlay();
		ui_status(ok ? "Text copied." : "Could not copy this message.");
		break;
	}
	case A_COPY_IMAGE:
		picture(m.row);
		m.copying = TRUE;
		set(&m.title, "Copy image");
		set(&m.note, "Preparing image…");
		break;
	case A_REACT:
		m.mode = REACT;
		m.cursor = 0;
		break;
	case A_VIEW:
		picture(m.row);
		break;
	case A_PLAY_VIDEO:
		open_video(m.row);
		break;
	case A_SAVE:
		open_picker(TRUE, m.row, FALSE);
		break;
	case A_FORWARD:
	case A_ANNOUNCE: {
		Json *row = json_object_get(m.row);

		close_overlay();
		if (a == A_FORWARD)
			ui_chat_tools_forward(row);
		else
			ui_chat_tools_announce(row);
		json_object_put(row);
		break;
	}
	case A_MENTION: {
		g_autofree char *id =
			g_strdup(ui_string(m.row, "senderId", ""));
		g_autofree char *name =
			g_strdup(ui_string(m.row, "from", "LINE"));

		close_overlay();
		ui_mention_member(id, name);
		break;
	}
	case A_UNSEND:
		m.mode = UNSEND;
		set(&m.note, "Remove this message from the conversation?");
		break;
	}
	ui.dirty = TRUE;
}

static void menu_row(Json *row)
{
	if (!row || !*ui_string(row, "id", ""))
		return;
	Json *owned = json_object_get(row);

	begin(MENU, ui.selected, ui_string(row, "id", ""), "Message actions");
	m.row = owned;
	m.cursor = 0;
}

static gboolean call_active(void)
{
	return m.call_chat && m.call_state && strcmp(m.call_state, "ended");
}

static gboolean call_docked(void)
{
	return call_active() && m.mode != CALL && m.mode != DIAL &&
	       g_strcmp0(m.call_state, "incoming");
}

enum { CALL_DOCK_HEIGHT = 56 };

/*
 * A connected or dialing call shows as a stage at the top of its chat, and
 * the messages and composer stay usable underneath.
 */
static gboolean call_stage_inline(void)
{
	return (m.mode == CALL || m.mode == DIAL) &&
	       g_strcmp0(m.call_state, "incoming");
}

int ui_media_stage_height(void)
{
	if (!call_stage_inline())
		return 0;
	int avail = ui_media_content_height() - UI_HEADER_H - 1 - 80;
	gboolean video = m.mode == CALL &&
			 (m.video_call || m.peer_sharing || m.self_sharing);
	int h = video ? avail * 3 / 5 : 236;

	return CLAMP(h, 180, MAX(180, avail - 140));
}

int ui_media_content_height(void)
{
	return ui.height - (call_docked() ? CALL_DOCK_HEIGHT : 0);
}

static gboolean call_connected(void);

static void sync_call_preview(void)
{
	gboolean paused = m.window_hidden || m.mode != CALL;

	if (!m.call_chat || !call_connected() || m.preview_paused == paused)
		return;
	m.preview_paused = paused;
	if (!paused)
		m.call_frames_since = g_get_monotonic_time();
	Json *p = args(m.call_chat, NULL);

	json_object_object_add(p, "visible", json_object_new_boolean(!paused));
	ui_rpc("callPreview", p, m.call_serial);
}

void ui_media_window_visibility(gboolean visible)
{
	m.window_hidden = !visible;
	if (!visible)
		ui_media_leave_fullscreen();
	sync_call_preview();
}

static void request_self_frame(const char *chat);

static void show_call(void)
{
	if (m.mode == CALL)
		return;
	begin(CALL, m.call_chat, NULL, m.call_name);
	if (m.window_hidden)
		return;
	ui_rpc("callFrame", args(m.call_chat, NULL), m.serial);
	request_self_frame(m.call_chat);
}

static void minimize_call(void)
{
	if (!call_active() || !g_strcmp0(m.call_state, "incoming"))
		return;
	close_overlay();
}

void ui_media_chat_shown(void)
{
	if (!call_active() || !g_strcmp0(m.call_state, "incoming"))
		return;
	if (!g_strcmp0(ui.selected, m.call_chat)) {
		if (m.mode == CLOSED)
			show_call();
	} else if (m.mode == CALL || m.mode == DIAL) {
		minimize_call();
	}
}

static gboolean request_video;
static void call_rpc(const char *method);

static gboolean call_connected(void)
{
	return !g_strcmp0(m.call_state, "connected") ||
	       !g_strcmp0(m.call_state, "active");
}

/* Screen sharing needs a connected call; the backend checks the rest. */
static gboolean call_share_available(void)
{
	return m.mode == CALL && call_connected() &&
	       g_strcmp0(m.call_state, "incoming");
}

/* Either side may turn its camera on once the call negotiated video. */
static gboolean call_camera_available(void)
{
	return m.mode == CALL && m.video_ready && call_connected() &&
	       !m.share_in_video;
}

static void dial(gboolean video)
{
	if (call_active()) {
		show_call();
		return;
	}
	if (!ui_media_can_call())
		return;
	const char *peer = ui.selected;

	for (size_t i = 0; ui.chats && i < json_object_array_length(ui.chats);
	     i++) {
		Json *c = json_object_array_get_idx(ui.chats, i);

		if (!g_strcmp0(ui_string(c, "id", ""), ui.selected)) {
			peer = ui_string(c, "name", ui.selected);
			break;
		}
	}
	request_video = video;
	m.call_serial++;
	begin(DIAL, ui.selected, NULL, peer);
	set(&m.call_note, NULL);
	if (video)
		set(&m.call_note,
		    "Your camera and microphone start when the call connects.");
	call_rpc("callStart");
}

static void call_rpc(const char *method)
{
	if (m.call_working)
		return;
	Json *p = args(m.mode == DIAL ? m.chat : m.call_chat, NULL);

	if (!strcmp(method, "callMute"))
		json_object_object_add(p, "muted",
				       json_object_new_boolean(!m.muted));
	if (!strcmp(method, "callStart"))
		json_object_object_add(p, "video",
				       json_object_new_boolean(request_video));
	if (!strcmp(method, "callCamera"))
		json_object_object_add(
			p, "enabled",
			json_object_new_boolean(!m.camera_enabled));
	if (!strcmp(method, "callShare")) {
		json_object_object_add(
			p, "enabled", json_object_new_boolean(!m.self_sharing));
		m.share_working = TRUE;
	}
	ui_rpc(method, p, m.call_serial);
	m.call_working = TRUE;
	ui.dirty = TRUE;
}

static void request_share_frame(const char *chat)
{
	Json *share = args(chat, NULL);

	json_object_object_add(share, "share", json_object_new_boolean(TRUE));
	ui_rpc("callFrame", share, m.serial);
}

static void stop_picture(struct call_picture *picture, enum stage_tile tile)
{
	if (m.fullscreen_tile == tile)
		ui_media_leave_fullscreen();
	clear_picture(picture);
}

static void request_self_frame(const char *chat)
{
	if (!m.camera_enabled || m.share_in_video) {
		if (m.self_frame.pixels) {
			stop_picture(&m.self_frame, TILE_SELF);
			ui.dirty = TRUE;
		}
		return;
	}
	Json *self = args(chat, NULL);

	json_object_object_add(self, "self", json_object_new_boolean(TRUE));
	ui_rpc("callFrame", self, m.serial);
}

void ui_media_call_event(Json *p)
{
	const char *event = ui_string(p, "event", "");
	const char *chat = ui_string(p, "chat", "");

	if (!strcmp(event, "call_frame") || !strcmp(event, "call_self_frame")) {
		if (m.mode == CALL && *chat && !g_strcmp0(chat, m.call_chat)) {
			if (!strcmp(event, "call_frame"))
				ui_rpc("callFrame", args(chat, NULL), m.serial);
			else
				request_self_frame(chat);
		}
		return;
	}
	if (!strcmp(event, "call_share_frame")) {
		if (m.mode == CALL && *chat && !g_strcmp0(chat, m.call_chat))
			request_share_frame(chat);
		return;
	}
	if (!strcmp(event, "call_share")) {
		if (!*chat || g_strcmp0(chat, m.call_chat))
			return;
		Json *peer = ui_field(p, "peerSharing"),
		     *self = ui_field(p, "selfSharing");

		if (peer) {
			gboolean was = m.peer_sharing;

			m.peer_sharing = json_object_get_boolean(peer);
			if (m.peer_sharing != was)
				ui_sound_play(m.peer_sharing ?
						      CHIP_SHARE_START :
						      CHIP_SHARE_STOP);
			if (m.peer_sharing) {
				/* Like Discord, a new stream takes the stage. */
				m.focus = 4;
			} else {
				m.share_ended_at = json_object_get_int64(
					ui_field(p, "changedAt"));
				if (!m.share_ended_at)
					m.share_ended_at =
						g_get_monotonic_time();
				stop_picture(&m.share_frame, TILE_PEER_SCREEN);
				if (m.focus == 4)
					m.focus = 0;
			}
		}
		if (self) {
			gboolean was = m.self_sharing;

			m.self_sharing = json_object_get_boolean(self);
			if (m.self_sharing != was)
				ui_sound_play(m.self_sharing ?
						      CHIP_SHARE_START :
						      CHIP_SHARE_STOP);
			m.share_in_video = json_object_get_boolean(
				ui_field(p, "shareInVideo"));
			m.share_working = FALSE;
			if (m.share_in_video)
				stop_picture(&m.self_frame, TILE_SELF);
			if ((!m.self_sharing && m.focus == 3) ||
			    (m.share_in_video && m.focus == 2))
				m.focus = 0;
		}
		if (ui_field(p, "message"))
			set(&m.call_note, ui_string(p, "message", NULL));
		ui.dirty = TRUE;
		return;
	}
	if (!strcmp(event, "call_camera_error")) {
		if (!call_active() || (*chat && g_strcmp0(chat, m.call_chat)) ||
		    !m.video_ready)
			return;
		m.camera_enabled = FALSE;
		stop_picture(&m.self_frame, TILE_SELF);
		set(&m.call_note, ui_string(p, "message", "The camera stopped."));
		ui.dirty = TRUE;
		return;
	}
	const char *state = ui_string(p, "state",
				      !strcmp(ui_string(p, "event", ""),
					      "call_incoming") ?
					      "incoming" :
					      ""),
		   *who = ui_string(p, "name", chat);
	if (!strcmp(ui_string(p, "event", ""), "call_error")) {
		set(&m.call_error,
		    ui_string(p, "message",
			      ui_string(p, "error", "Call failed.")));
		set(&m.call_note, m.call_error);
		m.call_working = FALSE;
		ui_status(m.call_note);
		return;
	}
	if (!strcmp(state, "ended") || !strcmp(state, "failed") ||
	    !strcmp(state, "idle")) {
		ui_sound_stop_loop();
		if (m.call_chat)
			ui_sound_play(CHIP_HANGUP);
		if (m.mode == CALL || m.mode == DIAL)
			close_overlay();
		set(&m.call_state, NULL);
		set(&m.call_chat, NULL);
		set(&m.call_note, NULL);
		m.call_working = FALSE;
		m.call_serial++;
		clear_call_frame();
		ui_status(m.call_error ?
				  m.call_error :
				  ui_string(p, "message", "Call ended."));
		g_clear_pointer(&m.call_error, g_free);
		return;
	}
	if (!*chat || (!strcmp(state, "incoming") && !ui.incoming_calls))
		return;
	gboolean new_call = g_strcmp0(chat, m.call_chat) != 0 || !call_active();
	gboolean connected = !strcmp(state, "connected") ||
			     !strcmp(state, "active");
	gboolean newly_connected = connected && g_strcmp0(m.call_state, state);
	Json *video = ui_field(p, "video");
	Json *camera = ui_field(p, "cameraEnabled");
	gboolean video_ready = json_object_get_boolean(video);
	gboolean video_changed = FALSE;

	if (!new_call &&
	    (!g_strcmp0(m.call_state, "active") ||
	     !g_strcmp0(m.call_state, "connected")) &&
	    strcmp(state, "active") && strcmp(state, "connected"))
		return;
	if (new_call) {
		if (m.mode != DIAL)
			m.call_serial++;
		g_clear_pointer(&m.call_error, g_free);
		clear_call_frame();
		m.video_call = video_ready;
		m.video_ready = FALSE;
		m.camera_enabled = m.video_call;
	}
	if (connected && video) {
		gboolean show_video = video_ready &&
				      (m.video_call || !camera ||
				       json_object_get_boolean(camera));

		video_changed = m.video_ready != video_ready;
		m.video_ready = video_ready;
		if (!video_ready && (m.fullscreen_tile == TILE_PEER ||
				     m.fullscreen_tile == TILE_SELF))
			ui_media_leave_fullscreen();
		m.video_call = show_video || m.call_frame.pixels;
		if (camera)
			m.camera_enabled = json_object_get_boolean(camera);
		else if (video_changed)
			m.camera_enabled = video_ready;
		if (!m.camera_enabled && m.self_frame.pixels)
			stop_picture(&m.self_frame, TILE_SELF);
	}
	if (!ui_field(p, "name")) {
		for (size_t i = 0;
		     ui.chats && i < json_object_array_length(ui.chats); i++) {
			Json *c = json_object_array_get_idx(ui.chats, i);

			if (!g_strcmp0(ui_string(c, "id", ""), chat)) {
				who = ui_string(c, "name", chat);
				break;
			}
		}
	}
	set(&m.call_name, who);
	set(&m.call_chat, chat);
	set(&m.call_state, state);
	if (ui_field(p, "muted"))
		m.muted = json_object_get_boolean(ui_field(p, "muted"));
	else if (new_call)
		m.muted = FALSE;
	m.can_mute = ui.mute_calls &&
		     (!ui_field(p, "canMute") ||
		      json_object_get_boolean(ui_field(p, "canMute")));
	if (new_call || m.mode == DIAL)
		begin(CALL, chat, NULL, who);
	if (newly_connected)
		sync_call_preview();
	if (newly_connected || video_changed)
		set(&m.call_note, ui_string(p, "message", NULL));
	if (m.mode == CALL && !m.window_hidden && m.video_ready &&
	    (newly_connected || video_changed)) {
		ui_rpc("callFrame", args(chat, NULL), m.serial);
		request_self_frame(chat);
	}
	if (new_call && m.video_call && !strcmp(state, "incoming"))
		set(&m.call_note, "Accepting starts your camera and microphone.");
	/* The ringtone and ringback last only while the call rings. */
	if (strcmp(state, "incoming") && strcmp(state, "ringing"))
		ui_sound_stop_loop();
	if (newly_connected)
		ui_sound_play(CHIP_CONNECTED);
	else if (!strcmp(state, "incoming"))
		ui_sound_loop(CHIP_RINGTONE);
	else if (!strcmp(state, "ringing"))
		ui_sound_loop(CHIP_RINGBACK);
	m.call_working = FALSE;
	ui.dirty = TRUE;
}

static void update_call_picture(int fd, int width, int height, int rotation,
				gboolean self, gboolean share,
				gboolean camera_enabled)
{
	if (m.window_hidden)
		return;
	if (rotation < 0 || rotation >= 360 || rotation % 90)
		rotation = 0;

	if (!line_camera_frame_size_valid(width, height))
		return;
	size_t size = line_camera_i420_size(width, height);
	struct stat st;
	int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
	int seals = fcntl(fd, F_GET_SEALS);
	void *pixels = MAP_FAILED;

	if (seals >= 0 && (seals & required) == required && !fstat(fd, &st) &&
	    (size_t)st.st_size == size)
		pixels = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (pixels == MAP_FAILED)
		return;
	struct call_picture *picture = share ? &m.share_frame :
				       self  ? &m.self_frame :
					       &m.call_frame;
	gboolean first = !picture->pixels;

	if (picture->frame.width != (unsigned int)width ||
	    picture->frame.height != (unsigned int)height)
		clear_picture(picture);
	else if (picture->pixels)
		munmap(picture->pixels, picture->size);
	if (picture->frame.rotation != (unsigned int)rotation)
		picture->view.w = 0;
	picture->pixels = pixels;
	static uint64_t generation;

	picture->generation = ++generation;
	picture->size = size;
	line_camera_i420_frame(&picture->frame, pixels, width, height);
	picture->frame.rotation = rotation;
	picture->upload = picture->convert = TRUE;
	enum stage_tile tile = share ? TILE_PEER_SCREEN :
			       self  ? TILE_SELF :
				       TILE_PEER;
	gboolean visible = !m.fullscreen_tile || m.fullscreen_tile == tile;

	if (share) {
		if (!m.peer_sharing)
			m.focus = 4;
		m.peer_sharing = TRUE;
		if (!visible)
			return;
		if ((ui.renderer || ui_gpu_active()) && picture->view.w > 0 &&
		    !first)
			ui.video_dirty = TRUE;
		else
			ui.dirty = TRUE;
		return;
	}
	if (self)
		m.video_call = TRUE;
	else if (!m.video_call) {
		m.video_call = TRUE;
		m.video_ready = TRUE;
		m.camera_enabled = camera_enabled;
		set(&m.call_note, NULL);
	}
	if (!visible)
		return;
	if (first && !g_strcmp0(g_getenv("LINE_CALL_DEBUG"), "1"))
		g_printerr("line-call video first frame reached window\n");
	if ((ui.renderer || ui_gpu_active()) && picture->view.w > 0)
		ui.video_dirty = TRUE;
	else
		ui.dirty = TRUE;
}

void ui_media_preview_frame(const struct line_preview_frame *frame, int fd)
{
	gboolean self = frame->source == LINE_PREVIEW_SELF;
	gboolean share = frame->source == LINE_PREVIEW_SCREEN;

	if (m.mode != CALL || g_strcmp0(frame->chat, m.call_chat) ||
	    frame->decoded_at < m.call_frames_since ||
	    (share && frame->decoded_at < m.share_ended_at) ||
	    (self && (!m.camera_enabled || m.share_in_video)))
		return;
	update_call_picture(fd, frame->width, frame->height, frame->rotation,
			    self, share, frame->camera_enabled);
}

static void video_frame(Json *result, gboolean self, gboolean share)
{
	int width = json_object_get_int(ui_field(result, "width"));
	int height = json_object_get_int(ui_field(result, "height"));

	if (!line_camera_frame_size_valid(width, height) ||
	    strcmp(ui_string(result, "format", ""), "i420"))
		return;
	int fd = open_regular(ui_string(result, "path", NULL),
			      line_camera_i420_size(width, height));

	if (fd < 0)
		return;
	update_call_picture(
		fd, width, height,
		json_object_get_int(ui_field(result, "rotation")), self, share,
		json_object_get_boolean(ui_field(result, "cameraEnabled")));
	close(fd);
}

static gboolean upload_call_frame(struct call_picture *picture)
{
	const struct line_camera_frame *frame = &picture->frame;

	if (!ui.renderer || picture->texture_failed)
		return FALSE;
	if (!picture->texture) {
		/* Match the software path's limited-range VP8 colors. */
		SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT601);
		picture->texture =
			SDL_CreateTexture(ui.renderer, SDL_PIXELFORMAT_IYUV,
					  SDL_TEXTUREACCESS_STREAMING,
					  frame->width, frame->height);
		if (!picture->texture)
			goto failed;
		SDL_SetTextureScaleMode(picture->texture, SDL_ScaleModeLinear);
	}
	if (picture->upload &&
	    SDL_UpdateYUVTexture(picture->texture, NULL, frame->plane[0],
				 frame->stride[0], frame->plane[1],
				 frame->stride[1], frame->plane[2],
				 frame->stride[2]) < 0)
		goto failed;
	picture->upload = FALSE;
	return TRUE;
failed:
	g_clear_pointer(&picture->texture, SDL_DestroyTexture);
	picture->texture_failed = TRUE;
	return FALSE;
}

static gboolean draw_call_frame(struct call_picture *picture, Rect box)
{
	const struct line_camera_frame *frame = &picture->frame;

	if (!picture->pixels || box.w <= 0 || box.h <= 0)
		return FALSE;
	if (ui_gpu_active() || upload_call_frame(picture)) {
		gboolean sideways = frame->rotation == 90 ||
				    frame->rotation == 270;
		float scale = MIN((float)box.w / (sideways ? frame->height :
							     frame->width),
				  (float)box.h / (sideways ? frame->width :
							     frame->height));
		float w = frame->width * scale, h = frame->height * scale;

		picture->view = (SDL_FRect){ box.x + (box.w - w) / 2,
					     box.y + (box.h - h) / 2, w, h };
		return TRUE;
	}
	if (!picture->surface) {
		picture->surface = cairo_image_surface_create(
			CAIRO_FORMAT_RGB24, frame->width, frame->height);
		if (cairo_surface_status(picture->surface) !=
		    CAIRO_STATUS_SUCCESS) {
			g_clear_pointer(&picture->surface,
					cairo_surface_destroy);
			return FALSE;
		}
	}
	if (picture->convert) {
		cairo_surface_flush(picture->surface);
		line_camera_xrgb(frame, (guint32 *)cairo_image_surface_get_data(
						picture->surface));
		cairo_surface_mark_dirty(picture->surface);
		picture->convert = FALSE;
	}
	ui_draw_rotated_image(picture->surface, box, frame->rotation);
	return TRUE;
}

enum {
	FULLSCREEN_BAR_H = 40
};

/* Stage tile numbers match the existing focus selection. */
static struct call_picture *tile_picture(enum stage_tile tile)
{
	switch (tile) {
	case TILE_PEER:
		return &m.call_frame;
	case TILE_SELF:
		return m.camera_enabled && !m.share_in_video ? &m.self_frame :
							       NULL;
	case TILE_PEER_SCREEN:
		return m.peer_sharing ? &m.share_frame : NULL;
	default:
		return NULL;
	}
}

static gboolean enter_fullscreen(enum stage_tile tile)
{
	struct call_picture *picture = tile_picture(tile);

	if (m.mode != CALL || !call_connected() || !picture || !picture->pixels)
		return FALSE;
	if (!(SDL_GetWindowFlags(ui.window) & SDL_WINDOW_FULLSCREEN)) {
		if (SDL_SetWindowFullscreen(
			    ui.window, SDL_WINDOW_FULLSCREEN_DESKTOP) < 0) {
			ui_status(
				"Full screen is unavailable on this display.");
			return FALSE;
		}
		m.window_expanded = TRUE;
	}
	m.saw_fullscreen =
		(SDL_GetWindowFlags(ui.window) & SDL_WINDOW_FULLSCREEN) != 0;
	m.fullscreen_tile = tile;
	m.call_frame.view.w = m.self_frame.view.w = m.share_frame.view.w = 0;
	memset(m.fullscreen_button, 0, sizeof(m.fullscreen_button));
	SDL_StopTextInput();
	ui.dirty = TRUE;
	return TRUE;
}

static Rect fullscreen_exit(void)
{
	int w = MIN(144, MAX(1, ui.width - 12));

	return (Rect){ ui.width - w - 6, 4, w, FULLSCREEN_BAR_H - 8 };
}

void ui_media_draw_fullscreen(void)
{
	struct call_picture *picture = tile_picture(m.fullscreen_tile);
	Rect exit = fullscreen_exit();
	Rect area = { 0, FULLSCREEN_BAR_H, ui.width,
		      MAX(0, ui.height - FULLSCREEN_BAR_H) };
	const char *name =
		m.fullscreen_tile == TILE_SELF ? (ui.name ? ui.name : "You") :
		m.fullscreen_tile == TILE_PEER_SCREEN ? "Shared screen" :
							m.call_name;

	m.call_frame.view.w = m.self_frame.view.w = m.share_frame.view.w = 0;
	ui_draw_box((Rect){ 0, 0, ui.width, ui.height }, 0);
	if (!picture || !draw_call_frame(picture, area))
		ui_draw_centered("Waiting for video…", area, 15, 500, 0xdbdee1);
	ui_draw_box((Rect){ 0, 0, ui.width, FULLSCREEN_BAR_H }, 0x111214);
	if (exit.x > 20)
		ui_draw_label(name ? name : "Video call",
			      (Rect){ 12, 10, exit.x - 18, 24 }, 15, 600,
			      0xf2f3f5, FALSE);
	ui_draw_button(exit, "Exit full screen", FALSE);
}

static const char *call_state_label(void)
{
	if (m.mode == DIAL)
		return m.call_working ? "Calling…" : "";
	if (call_connected())
		return "Connected";
	if (!g_strcmp0(m.call_state, "incoming"))
		return m.video_call ? "Incoming Video Call…" : "Incoming Call…";
	if (!g_strcmp0(m.call_state, "outgoing") ||
	    !g_strcmp0(m.call_state, "ringing"))
		return "Ringing…";
	return "Connecting…";
}

static Rect dock_button(int position)
{
	return (Rect){ ui.width - 12 - position * 44, ui.height - 46, 36, 36 };
}

static Rect dock_open(void)
{
	return (Rect){ 10, ui.height - 46,
		       ui.width - (m.can_mute && call_connected() ? 110 : 64),
		       36 };
}

int ui_media_dock_hover(int x, int y)
{
	if (!call_docked() || ui_media_layer())
		return 0;
	if (at(dock_button(1), x, y))
		return -201;
	if (m.can_mute && call_connected() && at(dock_button(2), x, y))
		return -202;
	return at(dock_open(), x, y) ? -203 : 0;
}

void ui_media_draw_call_dock(void)
{
	if (!call_docked())
		return;
	Rect dock = { 0, ui.height - CALL_DOCK_HEIGHT,
		      ui.width, CALL_DOCK_HEIGHT };
	Rect open = dock_open();
	int x = open.x + 44, width = open.w - 52;

	ui_draw_box(dock, 0x18191d);
	ui_draw_box((Rect){ 0, dock.y, dock.w, 1 }, 0x383a43);
	if (at(open, ui.hover_x, ui.hover_y))
		ui_draw_rounded(open, 8, 0x24262d);
	ui_draw_avatar(m.call_name, m.call_chat,
		       (Rect){ open.x + 5, open.y + 2, 32, 32 });
	ui_draw_label(m.call_name, (Rect){ x, open.y + 1, width, 16 },
		      13, 700, 0xf2f3f5, FALSE);
	ui_draw_label(m.self_sharing ? "Sharing screen · Return to call" :
				       "Return to call",
		      (Rect){ x, open.y + 18, width, 17 }, 12, 500,
		      call_connected() ? 0x86d8b0 : 0xb3b5bd, FALSE);
	if (m.can_mute && call_connected())
		ui_draw_round_button(dock_button(2),
				     m.muted ? UI_GLYPH_MIC_OFF : UI_GLYPH_MIC,
				     0x2b2d34, 0xf2f3f5,
				     m.muted ? "Unmute (Alt+M)" : "Mute (Alt+M)");
	ui_draw_round_button(dock_button(1), UI_GLYPH_HANG_UP, 0xa52c35,
			     0xffffff, "Disconnect");
}

/* Discord marks a tile that is streaming a screen with a red LIVE pill. */
static void live_badge(Rect tile)
{
	Rect pill = { tile.x + 10, tile.y + 10, 40, 20 };

	ui_draw_rounded(pill, 4, 0xda373c);
	ui_draw_centered("LIVE", (Rect){ pill.x, pill.y + 1, pill.w, pill.h },
			 11, 800, 0xffffff);
}

/* One participant: live video when present, otherwise their avatar. */
static void draw_expand_button(Rect r)
{
	gboolean hover = at(r, ui.hover_x, ui.hover_y);
	ui_draw_rounded(r, 4, hover ? 0x41434a : 0x2b2d31);
	if (r.w >= 68) {
		ui_draw_centered("Full screen", r, 11, 600, 0xf2f3f5);
	} else {
		int x = r.x + r.w / 2 - 6, y = r.y + r.h / 2 - 5;

		for (int n = 0; n < 4; n++) {
			int right = n & 1, bottom = n >> 1;

			ui_draw_box((Rect){ x + (right ? 8 : 0), y + bottom * 9,
					    5, 2 },
				    0xf2f3f5);
			ui_draw_box((Rect){ x + right * 11,
					    y + (bottom ? 6 : 0), 2, 5 },
				    0xf2f3f5);
		}
	}
	if (hover)
		ui_draw_tooltip(r, "Full screen (F11)");
}

static void call_tile(int index, struct call_picture *picture, const char *name,
		      const char *seed, gboolean live, gboolean streaming)
{
	Rect tile = m.tile_rect[index];

	ui_draw_rounded(tile, 8, 0x1a1a1e);
	gboolean shown = live && draw_call_frame(picture, tile);

	if (!shown) {
		int side = MIN(80, MIN(tile.w, tile.h) - 16);

		picture->view = (SDL_FRect){ 0 };
		if (side > 8)
			ui_draw_avatar(name, seed,
				       (Rect){ tile.x + (tile.w - side) / 2,
					       tile.y + (tile.h - side) / 2,
					       side, side });
	}
	if (streaming)
		live_badge(tile);
	int label_w = tile.w;
	if (shown && tile.w >= 24) {
		int w = tile.w >= 128 ? 72 : 24;
		Rect r = { tile.x + tile.w - w, tile.y + tile.h + 2, w, 22 };

		m.fullscreen_button[index] = r;
		draw_expand_button(r);
		label_w -= w + 4;
	}
	if (label_w > 0)
		ui_draw_label(name,
			      (Rect){ tile.x, tile.y + tile.h + 6, label_w,
				      20 },
			      13, 600, 0xdbdee1, FALSE);
}

/* Incoming calls ring as a small card with green and red buttons. */
static void draw_incoming_card(Rect area)
{
	Rect card = { area.x + (area.w - 260) / 2,
		      area.y + MAX(16, (area.h - 300) / 2), 260, 300 };

	ui_draw_rounded((Rect){ card.x - 1, card.y - 1, card.w + 2,
				card.h + 2 },
			13, 0x2a2a30);
	ui_draw_rounded(card, 12, 0x111214);
	ui_draw_avatar(m.call_name ? m.call_name : "", m.call_chat,
		       (Rect){ card.x + 90, card.y + 32, 80, 80 });
	ui_draw_centered(m.call_name,
			 (Rect){ card.x + 16, card.y + 124, card.w - 32, 30 },
			 20, 700, 0xf2f3f5);
	ui_draw_centered(call_state_label(),
			 (Rect){ card.x + 16, card.y + 156, card.w - 32, 22 },
			 14, 500, 0x96979e);
	m.secondary = (Rect){ card.x + 54, card.y + 212, 56, 56 };
	m.primary = (Rect){ card.x + card.w - 110, card.y + 212, 56, 56 };
	ui_draw_round_button(m.secondary, UI_GLYPH_HANG_UP, 0xda373c, 0xffffff,
			     "Decline (Alt+D)");
	ui_draw_round_button(m.primary,
			     m.video_call ? UI_GLYPH_CAMERA : UI_GLYPH_ACCEPT,
			     0x248046, 0xffffff, "Accept (Alt+A)");
}

/*
 * Discord shows a direct-message call as a black stage above the chat:
 * a tile per participant and a row of round controls underneath.
 */
static void draw_call_stage(void)
{
	int left = ui.conversation.x, top = UI_HEADER_H + 1;
	Rect area = { left, top, ui.width - left,
		      MAX(120, ui.composer.y - top - 12) };

	m.primary = m.secondary = m.third = (Rect){ 0 };
	m.call_minimize = (Rect){ 0 };
	m.items = m.up = (Rect){ 0 };
	if (m.mode == CALL && !g_strcmp0(m.call_state, "incoming")) {
		m.dialog = area;
		draw_incoming_card(area);
		return;
	}
	gboolean video = m.mode == CALL &&
			 (m.video_call || m.peer_sharing || m.self_sharing);
	Rect stage = { area.x, area.y, area.w,
		       ui.stage_h > 0 ? ui.stage_h :
					MAX(120, ui_media_stage_height()) };

	m.dialog = stage;
	ui_draw_box(stage, 0x000000);
	/* Fill in the tiles and controls once the stage finished growing. */
	if (ui.stage_h > 0 && ui.stage_h + 2 < ui_media_stage_height())
		return;
	if (m.call_note && *m.call_note)
		ui_draw_centered(m.call_note,
				 (Rect){ stage.x + 16, stage.y + 10,
					 stage.w - 104, 24 },
				 13, 500, 0x96979e);
	if (call_active()) {
		m.call_minimize = (Rect){ stage.x + stage.w - 48,
					  stage.y + 10, 36, 36 };
		ui_draw_round_button(m.call_minimize, UI_GLYPH_MINIMIZE,
				     0x2b2d34, 0xf2f3f5,
				     "Minimize call (Esc)");
	}
	int controls_y = stage.y + stage.h - 76;
	const char *self = ui.name ? ui.name : "You";

	memset(m.tile_rect, 0, sizeof(m.tile_rect));
	memset(m.fullscreen_button, 0, sizeof(m.fullscreen_button));
	if (video) {
		/*
		 * Tiles: 0 the peer, 1 you, 2 your screen, 3 the peer's screen.
		 * Without a focus they share a grid; a focused tile fills the
		 * stage and the rest sit in a strip beneath it, as in Discord.
		 */
		int order[4], n = 0;
		g_autofree char *peer_screen = g_strdup_printf(
			"%s's screen", m.call_name ? m.call_name : "Screen");

		order[n++] = 0;
		if (!m.share_in_video)
			order[n++] = 1;
		if (m.self_sharing)
			order[n++] = 2;
		if (m.peer_sharing)
			order[n++] = 3;
		int gap = 16, top = stage.y + 44, bottom = controls_y - 26 - 8;
		int focus = m.focus ? m.focus - 1 : -1;

		if (focus >= 0) {
			int strip = n > 1 ? MIN(120, (bottom - top) / 4) : 0;
			Rect big = { stage.x + gap, top, stage.w - 2 * gap,
				     bottom - top -
					     (strip ? strip + 26 + 8 : 0) };
			int sw = strip * 16 / 9, k = 0,
			    sx = stage.x +
				 (stage.w - (n - 1) * (sw + gap) + gap) / 2;

			m.tile_rect[focus] = big;
			for (int i = 0; i < n; i++)
				if (order[i] != focus)
					m.tile_rect[order[i]] =
						(Rect){ sx + k++ * (sw + gap),
							big.y + big.h + 26 + 8,
							sw, strip };
		} else {
			int cols = n > 2 ? 2 : n, rows = (n + 1) / 2;
			int tw = (stage.w - (cols + 1) * gap) / cols;
			int th = MIN(tw * 9 / 16,
				     (bottom - top - (rows - 1) * (26 + 8)) /
						     rows -
					     26);

			th = MAX(th, 60);
			tw = MIN(tw, th * 16 / 9);
			int x0 = stage.x +
				 (stage.w - cols * tw - (cols - 1) * gap) / 2,
			    y0 = top +
				 (bottom - top - rows * (th + 26 + 8)) / 2;

			for (int i = 0; i < n; i++)
				m.tile_rect[order[i]] =
					(Rect){ x0 + (i % cols) * (tw + gap),
						y0 + (i / cols) * (th + 26 + 8),
						tw, th };
		}
		call_tile(0, &m.call_frame, m.call_name, m.call_chat,
			  m.call_frame.pixels != NULL, FALSE);
		if (!m.share_in_video)
			call_tile(1, &m.self_frame, self, ui.self_id,
				  m.self_frame.pixels != NULL, FALSE);
		if (m.self_sharing) {
			/* Your own stream: Discord shows a placeholder too. */
			Rect t = m.tile_rect[2];

			ui_draw_rounded(t, 8, 0x1a1a1e);
			ui_draw_centered("You are sharing your screen",
					 (Rect){ t.x + 8, t.y + t.h / 2 - 11,
						 t.w - 16, 22 },
					 t.h > 80 ? 15 : 11, 600, 0xf2f3f5);
			live_badge(t);
			ui_draw_label("Your screen",
				      (Rect){ t.x, t.y + t.h + 6, t.w, 20 }, 13,
				      600, 0xdbdee1, FALSE);
		}
		if (m.peer_sharing)
			call_tile(3, &m.share_frame, peer_screen, m.call_chat,
				  m.share_frame.pixels != NULL, TRUE);
		else
			m.share_frame.view = (SDL_FRect){ 0 };
	} else {
		int cy = stage.y + (controls_y - stage.y - 80) / 2;
		int cx = stage.x + stage.w / 2;

		ui_draw_avatar(m.call_name ? m.call_name : "", m.call_chat,
			       (Rect){ cx - 92, cy, 80, 80 });
		ui_draw_avatar(self, ui.self_id, (Rect){ cx + 12, cy, 80, 80 });
		ui_draw_centered(call_state_label(),
				 (Rect){ stage.x + 16, cy + 94, stage.w - 32,
					 22 },
				 14, 500, 0x96979e);
	}
	/* Controls: camera, screen, microphone, then the disconnect button. */
	Rect buttons[4];
	int count = 0;

	m.share_button = (Rect){ 0 };
	if (call_camera_available())
		buttons[count++] = (Rect){ 0 };
	if (call_share_available())
		buttons[count++] = (Rect){ 0 };
	if (m.mode == CALL && m.can_mute)
		buttons[count++] = (Rect){ 0 };
	if (m.mode == CALL || !m.call_working)
		buttons[count++] = (Rect){ 0 };
	if (m.mode == DIAL && !m.call_working)
		buttons[count++] = (Rect){ 0 };
	int bx = stage.x + (stage.w - count * 56 - (count - 1) * 16) / 2;

	for (int i = 0; i < count; i++)
		buttons[i] = (Rect){ bx + i * 72, controls_y, 56, 56 };
	int i = 0;

	if (call_camera_available()) {
		m.third = buttons[i++];
		ui_draw_round_button(m.third,
				     m.camera_enabled ? UI_GLYPH_CAMERA :
							UI_GLYPH_CAMERA_OFF,
				     m.camera_enabled ? 0xf2f3f5 : 0x2b2b30,
				     m.camera_enabled ? 0x111214 : 0xf2f3f5,
				     m.camera_enabled ?
					     "Turn Off Camera (Alt+V)" :
					     "Turn On Camera (Alt+V)");
	}
	if (call_share_available()) {
		m.share_button = buttons[i++];
		ui_draw_round_button(
			m.share_button,
			m.self_sharing ? UI_GLYPH_SCREEN_STOP : UI_GLYPH_SCREEN,
			m.self_sharing ? 0xf2f3f5 : 0x2b2b30,
			m.self_sharing ? 0x111214 : 0xf2f3f5,
			m.self_sharing ? "Stop Streaming (Alt+S)" :
					 "Share Your Screen (Alt+S)");
	}
	if (m.mode == CALL && m.can_mute) {
		m.secondary = buttons[i++];
		const char *action = m.muted ? "Unmute" : "Mute";

		ui_draw_round_button(
			m.secondary, m.muted ? UI_GLYPH_MIC_OFF : UI_GLYPH_MIC,
			m.muted ? 0xf2f3f5 : 0x2b2b30,
			m.muted ? 0x111214 : 0xf2f3f5,
			m.muted ? "Unmute (Alt+M)" : "Mute (Alt+M)");
		/* The label may be wider than its button. */
		ui_draw_centered(action,
				 (Rect){ m.secondary.x - 12, m.secondary.y + 58,
					 m.secondary.w + 24, 18 },
				 12, 600, 0xf2f3f5);
		m.secondary.h += 20;
	}
	if (m.mode == CALL) {
		m.primary = buttons[i++];
		ui_draw_round_button(m.primary, UI_GLYPH_HANG_UP, 0xda373c,
				     0xffffff, "Disconnect");
	} else if (!m.call_working) {
		/* A failed dial can be retried or dismissed. */
		m.secondary = buttons[i++];
		ui_draw_round_button(m.secondary, UI_GLYPH_CLOSE, 0x2b2b30,
				     0xf2f3f5, "Cancel");
		m.primary = buttons[i++];
		ui_draw_round_button(m.primary, UI_GLYPH_ACCEPT, 0x248046,
				     0xffffff, "Call Again");
	}
}

static void present_picture(struct call_picture *picture, double y_offset)
{
	SDL_FRect view = picture->view;

	if (!picture->pixels || view.w <= 0)
		return;
	view.y += y_offset;
	if (upload_call_frame(picture) &&
	    !SDL_RenderCopyExF(ui.renderer, picture->texture, NULL, &view,
			       picture->frame.rotation, NULL, SDL_FLIP_NONE))
		return;
	g_clear_pointer(&picture->texture, SDL_DestroyTexture);
	picture->texture_failed = TRUE;
	picture->view = (SDL_FRect){ 0 };
	ui.dirty = TRUE;
}

void ui_media_present_video(double y_offset)
{
	if (m.mode != CALL ||
	    !(m.video_call || m.peer_sharing || m.self_sharing))
		return;
	present_picture(&m.call_frame, y_offset);
	present_picture(&m.self_frame, y_offset);
	present_picture(&m.share_frame, y_offset);
}

void ui_media_gpu_video(struct gpu_scene *scene, double y_offset)
{
	if (m.mode != CALL ||
	    !(m.video_call || m.peer_sharing || m.self_sharing))
		return;
	const struct call_picture *pics[] = { &m.call_frame, &m.self_frame,
					      &m.share_frame };
	float sx = (float)scene->width / ui.width,
	      sy = (float)scene->height / ui.height;

	for (size_t i = 0; i < G_N_ELEMENTS(pics); i++) {
		const struct call_picture *pic = pics[i];
		const struct line_camera_frame *f = &pic->frame;
		SDL_FRect r = pic->view;

		if (!pic->pixels || r.w <= 0 || r.h <= 0)
			continue;
		gboolean side = f->rotation == 90 || f->rotation == 270;
		float w = side ? r.h : r.w, h = side ? r.w : r.h;

		scene->video[i] = (struct gpu_video){
			.width = f->width,
			.height = f->height,
			.rotation = f->rotation,
			.generation = pic->generation,
			.x = (r.x + (r.w - w) / 2) * sx,
			.y = (r.y + (r.h - h) / 2 + y_offset) * sy,
			.w = w * sx,
			.h = h * sy,
		};
		memcpy(scene->video[i].plane, f->plane, sizeof(f->plane));
		memcpy(scene->video[i].stride, f->stride, sizeof(f->stride));
	}
}

void ui_media_reply(const char *method, Json *p, guint tag, Json *result,
		    const char *error)
{
	if (ui_chat_tools_reply(method, tag, result, error) ||
	    ui_shared_reply(method, tag, result, error))
		return;
	if (!strcmp(method, "flexImage")) {
		const char *url = ui_string(p, "url", "");
		struct media_thumbnail *entry =
			m.flex_images ?
				g_hash_table_lookup(m.flex_images, url) :
				NULL;
		gboolean current = entry && entry->serial == tag;
		int fd = current && !error ?
				 open_regular(ui_string(result, "path", NULL),
					      LINE_FLEX_ASSET_LIMIT) :
				 -1;
		Json *lease = ui_field(result, "lease");

		if (lease) {
			Json *params = json_object_new_object();

			json_object_object_add(params, "lease",
					       json_object_get(lease));
			ui_rpc("releaseMedia", params, 0);
		}
		if (current && error)
			media_debug(error, url);
		if (current && !start_decode(fd, FLEX_IMAGE, url, tag, FALSE)) {
			store_thumbnail(entry, NULL);
			m.flex_busy = FALSE;
			ui.dirty = TRUE;
		}
		return;
	}
	if (!strcmp(method, "callFrame")) {
		if (!error && m.mode == CALL && tag == m.serial &&
		    !g_strcmp0(ui_string(p, "chat", NULL), m.call_chat))
			video_frame(
				result,
				json_object_get_boolean(ui_field(p, "self")),
				json_object_get_boolean(ui_field(p, "share")));
		Json *lease = ui_field(result, "lease");

		if (lease) {
			Json *params = json_object_new_object();

			json_object_object_add(params, "lease",
					       json_object_get(lease));
			ui_rpc("releaseMedia", params, 0);
		}
		return;
	}
	const char *chat = ui_string(p, "chat", ""),
		   *id = ui_string(p, "id", "");
	if (!strcmp(method, "openVideo")) {
		gboolean current = m.mode == PLAY_VIDEO && tag == m.serial &&
				   !g_strcmp0(chat, m.chat) &&
				   !g_strcmp0(id, m.id);
		int fd = current && !error ?
				 open_regular(ui_string(result, "path", NULL),
					      LINE_MEDIA_LIMIT) :
				 -1;
		Json *lease = ui_field(result, "lease");

		if (lease) {
			Json *a = args(NULL, NULL);

			json_object_object_add(a, "lease",
					       json_object_get(lease));
			ui_rpc("releaseMedia", a, 0);
		}
		if (current) {
			m.working = FALSE;
			if (fd < 0)
				set(&m.note,
				    error ? error :
					    "Could not open the downloaded video.");
			else if (play_clip(fd, FALSE))
				close_overlay();
		}
		return;
	}
	if ((!strcmp(method, "sendPhoto") || !strcmp(method, "sendFile")) &&
	    m.paste_sent) {
		const char *path = ui_string(p, "path", "");
		gpointer key = NULL;

		if (g_hash_table_lookup_extended(m.paste_sent, path, &key,
						 NULL)) {
			/* A failed send keeps the preview: allow a retry. */
			if (error && m.serial == tag && m.mode == PREVIEW &&
			    !m.paste) {
				g_hash_table_steal(m.paste_sent, path);
				m.paste = key;
			} else
				g_hash_table_remove(m.paste_sent, path);
		}
	}
	if (!strcmp(method, "sendFile") && m.uploads) {
		const char *path = ui_string(p, "path", "");
		gpointer key = NULL;

		if (g_hash_table_lookup_extended(m.uploads, path, &key, NULL)) {
			if (error && m.serial == tag && m.mode == PREVIEW) {
				g_hash_table_steal(m.uploads, path);
				m.recorded = key;
			} else
				g_hash_table_remove(m.uploads, path);
		}
	}
	if (!strcmp(method, "releaseMedia"))
		return;
	if (!strcmp(method, "emojiCatalog") ||
	    !strcmp(method, "stickerCatalog")) {
		ui_emoji_catalog(tag, result, error);
		return;
	}
	if (!strcmp(method, "sendSticker")) {
		if (!error && !*ui_string(result, "id", ""))
			error = "Sticker delivery was not confirmed.";
		if (!error && !g_strcmp0(chat, ui.reply_chat) &&
		    !g_strcmp0(ui.reply_id, ui_string(p, "replyTo", NULL)))
			ui_media_reply_sent(chat);
		ui_sticker_sent(tag, error);
		if (!error && !g_strcmp0(chat, ui.selected))
			ui_show_latest();
		return;
	}
	if (!strcmp(method, "emoji") || !strcmp(method, "sticker")) {
		char key[96];
		struct media_thumbnail *owner =
			palette_key(p, key) && m.emoji ?
				g_hash_table_lookup(m.emoji, key) :
				NULL;
		gboolean current = owner && owner->serial == tag;
		gsize limit = !strcmp(method, "sticker") ? 8 * 1024 * 1024 :
							   4 * 1024 * 1024;
		int fd = current && !error ?
				 open_regular(ui_string(result, "path", NULL),
					      limit) :
				 -1;
		Json *lease = ui_field(result, "lease");

		if (lease) {
			Json *a = json_object_new_object();

			json_object_object_add(a, "lease",
					       json_object_get(lease));
			ui_rpc("releaseMedia", a, 0);
		}
		if (!current)
			return;
		if (fd >= 0 && start_decode(fd, EMOJI, key, tag, TRUE))
			return;
		store_thumbnail(owner, NULL);
		m.emoji_busy = FALSE;
		set(&owner->reason,
		    error ? error : "Could not load this image.");
		ui_forget_history();
		return;
	}
	if (!strcmp(method, "photo") || !strcmp(method, "avatar") ||
	    !strcmp(method, "sharedPhoto") || !strcmp(method, "videoPoster")) {
		gboolean avatar = !strcmp(method, "avatar");
		struct media_thumbnail *owner =
			avatar && m.avatars ?
				g_hash_table_lookup(m.avatars, id) :
				NULL;
		Json *lease = ui_field(result, "lease");
		gboolean current = !g_strcmp0(chat, ui.selected);
		gboolean display =
			avatar ? (owner && owner->serial == tag) :
				 (tag ? (m.mode == VIEW && m.serial == tag) :
					current);
		int fd = display ? open_regular(ui_string(result, "path", NULL),
						IMAGE_BOUND) :
				   -1;
		if (lease) {
			Json *a = args(NULL, NULL);

			json_object_object_add(a, "lease",
					       json_object_get(lease));
			ui_rpc("releaseMedia", a, 0);
		}
		if (!display) {
			if (avatar)
				m.avatar_busy = FALSE;
			return;
		}
		enum decode_role role =
			avatar ? AVATAR :
			tag    ? (m.copying ? CLIPBOARD : ORIGINAL) :
				 THUMBNAIL;
		struct media_thumbnail *thumb =
			!avatar && !tag && m.thumbs ?
				g_hash_table_lookup(m.thumbs, id) :
				NULL;
		gboolean animate = (role == ORIGINAL && m.sticker) ||
				   (thumb && thumb->sticker);
		if (!error && fd >= 0 &&
		    start_decode(fd, role, id, tag, animate))
			return;
		if (error && fd >= 0)
			close(fd);
		const char *reason = error ? error :
					     "Could not open this image.";
		if (avatar) {
			m.avatar_busy = FALSE;
			store_thumbnail(owner, NULL);
			set(&owner->reason, reason);
		} else if (tag) {
			m.working = FALSE;
			set(&m.note, reason);
		} else {
			m.thumb_busy = FALSE;
			if (thumb) {
				store_thumbnail(thumb, NULL);
				set(&thumb->reason, reason);
			}
		}
		return;
	}
	if (g_str_has_prefix(method, "call")) {
		/* The worker can publish a newer call state before its command reply. */
		if (!strcmp(method, "callPreview") || tag != m.call_serial)
			return;
		if (error) {
			m.call_working = FALSE;
			if (!strcmp(method, "callShare"))
				m.share_working = FALSE;
			if (m.mode == CALL || m.mode == DIAL)
				set(&m.call_note, error);
			else
				ui_status(error);
			return;
		}
		if (!strcmp(method, "callMute") && ui_field(result, "muted")) {
			gboolean was = m.muted;

			m.muted = json_object_get_boolean(
				ui_field(result, "muted"));
			if (m.muted != was)
				ui_sound_play(m.muted ? CHIP_MUTE : CHIP_UNMUTE);
		}
		if (!strcmp(method, "callCamera") &&
		    ui_field(result, "cameraEnabled")) {
			gboolean was = m.camera_enabled;

			m.camera_enabled = json_object_get_boolean(
				ui_field(result, "cameraEnabled"));
			if (m.camera_enabled != was)
				ui_sound_play(m.camera_enabled ?
						      CHIP_CAMERA_ON :
						      CHIP_CAMERA_OFF);
			m.video_call |= m.camera_enabled;
			if (!m.camera_enabled)
				stop_picture(&m.self_frame, TILE_SELF);
		}
		/* Sharing reports back through call_share once capture starts. */
		if (!strcmp(method, "callShare") &&
		    ui_field(result, "selfSharing")) {
			if (!json_object_get_boolean(
				    ui_field(result, "pending")))
				m.share_working = FALSE;
		}
		if (ui_field(result, "state"))
			ui_media_call_event(result);
		m.call_working = FALSE;
		ui.dirty = TRUE;
		return;
	}
	if (error) {
		if (m.mode != CLOSED && m.serial == tag) {
			m.working = FALSE;
			set(&m.note, error);
		} else
			ui_status(error);
		return;
	}
	if (tag && tag == m.serial && m.mode != CLOSED)
		close_overlay();
	if (!strcmp(method, "saveFile"))
		ui_status("File saved.");
	else {
		ui.stick_bottom = TRUE;
		ui_refresh();
	}
}

static const char *attachment_type(const char *path)
{
	g_autofree char *lower = g_ascii_strdown(path ? path : "", -1);

	if (g_str_has_suffix(lower, ".mp4") ||
	    g_str_has_suffix(lower, ".mov") || g_str_has_suffix(lower, ".webm"))
		return "video";
	if (g_str_has_suffix(lower, ".m4a") ||
	    g_str_has_suffix(lower, ".mp3") ||
	    g_str_has_suffix(lower, ".wav") || g_str_has_suffix(lower, ".ogg"))
		return "audio";
	return "file";
}

static void play_review(void)
{
	int fd = open_regular(m.path, LINE_MEDIA_LIMIT);

	if (fd < 0)
		set(&m.note, "Choose a readable clip up to 64 MiB.");
	else
		play_clip(fd, TRUE);
}

static void record_now(void)
{
	if (!m.record_event)
		m.record_event = SDL_RegisterEvents(1);
	if (m.record_event == (Uint32)-1) {
		set(&m.note, "Could not start the recording monitor.");
		return;
	}
	g_autoptr(GError) e = NULL;
	if (!line_record_start(record_finished, GUINT_TO_POINTER(m.serial),
			       &e)) {
		set(&m.note, e ? e->message : "Could not open the camera.");
		return;
	}
	m.mode = RECORDING;
	set(&m.note, "The camera and microphone are recording.");
}

static void primary(void)
{
	if (m.working && m.mode != VIEW)
		return;
	switch (m.mode) {
	case PICK:
		choose_path();
		break;
	case PREVIEW: {
		if (m.photo && !m.image)
			return;
		Json *p = args(m.chat, NULL);

		json_object_object_add(p, "path",
				       json_object_new_string(m.path));
		if (!m.photo)
			json_object_object_add(
				p, "type",
				json_object_new_string(
					attachment_type(m.path)));
		if (m.recorded) {
			if (!m.uploads)
				m.uploads = g_hash_table_new_full(
					g_str_hash, g_str_equal,
					(GDestroyNotify)line_record_discard,
					NULL);
			g_hash_table_add(m.uploads,
					 g_steal_pointer(&m.recorded));
		}
		if (m.paste && !g_strcmp0(m.paste, m.path)) {
			/* The worker reads the staged file after this. */
			if (!m.paste_sent)
				m.paste_sent = g_hash_table_new_full(
					g_str_hash, g_str_equal, unlink_path,
					NULL);
			g_hash_table_add(m.paste_sent,
					 g_steal_pointer(&m.paste));
		}
		m.working = TRUE;
		set(&m.note, "Sending…");
		ui_rpc(m.photo ? "sendPhoto" : "sendFile", p, m.serial);
		break;
	}
	case CAMERA:
		record_now();
		break;
	case RECORDING:
		line_record_stop();
		m.working = TRUE;
		set(&m.note, "Finishing the recording…");
		break;
	case REPLACE:
		save_to(TRUE);
		break;
	case UNSEND:
		m.working = TRUE;
		ui_rpc("unsend", args(m.chat, m.id), m.serial);
		break;
	case DIAL:
		call_rpc("callStart");
		break;
	case CALL:
		if (!g_strcmp0(m.call_state, "incoming"))
			call_rpc("callAccept");
		else
			call_rpc("callEnd");
		break;
	case VIEW:
		close_overlay();
		break;
	case PLAY_VIDEO:
		request_video_clip();
		break;
	default:
		break;
	}
}

static void second(void)
{
	if (m.mode == CALL) {
		if (m.call_working)
			return;
		if (!g_strcmp0(m.call_state, "incoming"))
			call_rpc("callDecline");
		else if (m.can_mute)
			call_rpc("callMute");
	} else if (!m.working || m.mode == VIEW || m.mode == PLAY_VIDEO)
		close_overlay();
}

static int visible_rows(void)
{
	return MAX(1, m.items.h / 34);
}

static void menu_select(int pos)
{
	if (m.mode == MENU) {
		enum message_action rows[12];
		int n = actions(rows);

		if (pos >= 0 && pos < n)
			menu_action(rows[pos]);
	} else if (m.mode == REACT && pos >= 0 &&
		   pos < (int)G_N_ELEMENTS(reaction_names) && !m.working) {
		Json *p = args(m.chat, m.id);

		json_object_object_add(
			p, "reaction",
			json_object_new_string(reaction_names[pos]));
		m.working = TRUE;
		ui_rpc("react", p, m.serial);
	}
}

static gboolean handle_key(SDL_KeyboardEvent k)
{
	SDL_Keycode code = k.keysym.sym;

	if (m.mode == CALL && code == SDLK_F11) {
		const enum stage_tile tiles[] = {
			m.focus,
			TILE_PEER,
			TILE_PEER_SCREEN,
			TILE_SELF,
		};
		for (guint i = 0; !k.repeat && i < G_N_ELEMENTS(tiles); i++) {
			const struct call_picture *picture =
				tile_picture(tiles[i]);

			if (picture && picture->pixels) {
				enter_fullscreen(tiles[i]);
				break;
			}
		}
		return TRUE;
	}
	gboolean ctrl = (k.keysym.mod & KMOD_CTRL) != 0;

	if (call_docked() && !k.repeat && (k.keysym.mod & KMOD_ALT)) {
		if (code == SDLK_c && !ui_media_layer()) {
			show_call();
			return TRUE;
		}
		if (code == SDLK_m && m.can_mute && call_connected()) {
			call_rpc("callMute");
			return TRUE;
		}
	}
	if (m.mode == CLOSED) {
		if (ctrl && (k.keysym.mod & KMOD_ALT) && code == SDLK_s &&
		    !k.repeat && ui.selected && ui.photos) {
			ui_media_screenshot();
			return TRUE;
		}
		if (ctrl && (k.keysym.mod & KMOD_ALT) && code == SDLK_f &&
		    !k.repeat && ui.connected && ui.find_messages) {
			ui_find_open_all();
			return TRUE;
		}
		if ((k.keysym.mod & KMOD_SHIFT) && ctrl && code == SDLK_t &&
		    !k.repeat && ui_chat_visible() && ui.selected) {
			ui_chat_tools_open();
			return TRUE;
		}
		if (ctrl && code == SDLK_e && (ui.emojis || ui.stickers) &&
		    ui.selected && ui_chat_visible()) {
			show_emoji();
			return TRUE;
		}
		if (ctrl && code == SDLK_o && ui_chat_visible() &&
		    ui.selected && (ui.files || ui.photos)) {
			open_picker(FALSE, NULL, FALSE);
			return TRUE;
		}
		if (code == SDLK_ESCAPE && ui.reply_id &&
		    !g_strcmp0(ui.reply_chat, ui.selected)) {
			ui_media_reply_sent(ui.selected);
			return TRUE;
		}
		return FALSE;
	}
	if (code == SDLK_ESCAPE) {
		if (k.repeat)
			return TRUE;
		if (m.mode == CALL && call_active()) {
			minimize_call();
			return TRUE;
		}
		if (m.mode == DIAL && m.call_working)
			return TRUE;
		if (!m.working || m.mode == VIEW || m.mode == PLAY_VIDEO)
			close_overlay();
		return TRUE;
	}
	if (m.mode == CALL && !g_strcmp0(m.call_state, "incoming")) {
		if (!k.repeat && (k.keysym.mod & KMOD_ALT) && code == SDLK_a)
			primary();
		if (!k.repeat && (k.keysym.mod & KMOD_ALT) && code == SDLK_d)
			second();
		return TRUE;
	}
	if (m.mode == CALL && (k.keysym.mod & KMOD_ALT) && code == SDLK_m &&
	    !k.repeat) {
		second();
		return TRUE;
	}
	if (call_share_available() && (k.keysym.mod & KMOD_ALT) &&
	    code == SDLK_s && !k.repeat) {
		if (!m.share_working)
			call_rpc("callShare");
		return TRUE;
	}
	if (call_camera_available() && (k.keysym.mod & KMOD_ALT) &&
	    code == SDLK_v && !k.repeat) {
		if (!m.call_working)
			call_rpc("callCamera");
		return TRUE;
	}
	/* Other keys reach the composer and chat list. */
	if (call_stage_inline())
		return FALSE;
	if (m.mode == PICK) {
		if (code == SDLK_UP || code == SDLK_DOWN) {
			if (m.entries && m.entries->len) {
				m.cursor = CLAMP(
					m.cursor + (code == SDLK_UP ? -1 : 1),
					0, (int)m.entries->len - 1);
				choose_item(m.cursor, FALSE);
				m.offset = CLAMP(
					m.offset,
					MAX(0, m.cursor - visible_rows() + 1),
					m.cursor);
			}
		} else if (code == SDLK_RETURN || code == SDLK_KP_ENTER) {
			if (!k.repeat)
				primary();
		} else
			ui_edit_key(&m.edit, k);
	} else if (m.mode == MENU || m.mode == REACT) {
		enum message_action arr[12];
		int count = m.mode == MENU ? actions(arr) :
					     (int)G_N_ELEMENTS(reaction_names);
		if (code == SDLK_UP || code == SDLK_DOWN) {
			m.cursor = CLAMP(m.cursor + (code == SDLK_UP ? -1 : 1),
					 0, count - 1);
			ui.dirty = TRUE;
		} else if (code == SDLK_RETURN && !k.repeat)
			menu_select(m.cursor);
	} else if ((code == SDLK_RETURN || code == SDLK_KP_ENTER) && !k.repeat)
		primary();
	return TRUE;
}

static void click_overlay(int x, int y, int clicks)
{
	if (m.mode == CALL) {
		if (clicks == 2 && m.first_click_tile &&
		    enter_fullscreen(m.first_click_tile)) {
			/* The first click may already have rearranged the tiles. */
			m.focus = m.first_click_focus;
			return;
		}
		m.first_click_tile = 0;
		for (int i = 0; i < 4; i++) {
			if (at(m.fullscreen_button[i], x, y) &&
			    enter_fullscreen(i + 1)) {
				m.ignore_multiclick = TRUE;
				return;
			}
		}
	}
	if ((m.mode == MENU || m.mode == REACT) && !at(m.dialog, x, y)) {
		close_overlay();
		return;
	}
	if (at(m.third, x, y) && m.mode == PICK && !m.save && !m.friend_qr &&
	    !m.avatar_pick) {
		begin(CAMERA, ui.selected, NULL, "Record a video");
		set(&m.note,
		    "Start the camera and microphone to record a video message.");
		return;
	}
	if (at(m.third, x, y) && m.mode == PREVIEW && m.path) {
		play_review();
		return;
	}
	if (at(m.third, x, y) && m.mode == PLAY_VIDEO && !m.working) {
		open_picker(TRUE, m.row, FALSE);
		return;
	}
	if (m.mode == CALL && at(m.call_minimize, x, y)) {
		minimize_call();
		return;
	}
	if (call_camera_available() && at(m.third, x, y)) {
		if (!m.call_working)
			call_rpc("callCamera");
		return;
	}
	if (call_share_available() && at(m.share_button, x, y)) {
		if (!m.share_working)
			call_rpc("callShare");
		return;
	}
	/* Clicking a tile focuses it; clicking the focused tile unfocuses. */
	for (int i = 0; m.mode == CALL && i < 4; i++)
		if (m.tile_rect[i].w && at(m.tile_rect[i], x, y)) {
			if (clicks == 2 && enter_fullscreen(i + 1))
				return;
			m.first_click_tile = i + 1;
			m.first_click_focus = m.focus;
			m.focus = m.focus == i + 1 ? 0 : i + 1;
			ui.dirty = TRUE;
			return;
		}
	if (at(m.primary, x, y))
		primary();
	else if (at(m.secondary, x, y))
		second();
	else if (m.mode == PICK && at(m.up, x, y)) {
		g_autofree char *p = g_path_get_dirname(m.directory);

		directory(p);
	} else if (at(m.items, x, y)) {
		int pos = m.offset + (y - m.items.y) / 34;

		if (m.mode == PICK)
			choose_item(pos, clicks >= 2);
		else
			menu_select((y - m.items.y) / 34);
	}
}

static gboolean fullscreen_event(SDL_Event *e)
{
	switch (e->type) {
	case SDL_WINDOWEVENT:
		if (e->window.windowID != SDL_GetWindowID(ui.window))
			return FALSE;
		if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
		    e->window.event == SDL_WINDOWEVENT_RESTORED) {
			gboolean full = (SDL_GetWindowFlags(ui.window) &
					 SDL_WINDOW_FULLSCREEN) != 0;

			/* Wayland acknowledges a fullscreen request asynchronously. */
			if (m.saw_fullscreen && !full) {
				m.window_expanded = FALSE;
				ui_media_leave_fullscreen();
			} else if (full)
				m.saw_fullscreen = TRUE;
			return FALSE;
		}
		if (e->window.event != SDL_WINDOWEVENT_CLOSE)
			return FALSE;
		ui_media_leave_fullscreen();
		SDL_FlushEvent(SDL_QUIT);
		return TRUE;
	case SDL_KEYDOWN:
		if (!e->key.repeat && (e->key.keysym.sym == SDLK_ESCAPE ||
				       e->key.keysym.sym == SDLK_F11))
			ui_media_leave_fullscreen();
		else if (e->key.keysym.mod & KMOD_ALT)
			handle_key(e->key);
		return TRUE;
	case SDL_MOUSEBUTTONDOWN:
		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (e->button.clicks <= 1)
			m.ignore_multiclick = FALSE;
		else if (m.ignore_multiclick)
			return TRUE;
		if (at(fullscreen_exit(), e->button.x, e->button.y) ||
		    (e->button.clicks == 2 &&
		     e->button.y >= FULLSCREEN_BAR_H)) {
			ui_media_leave_fullscreen();
			m.ignore_multiclick = TRUE;
		}
		return TRUE;
	case SDL_MOUSEMOTION: {
		Rect r = fullscreen_exit();
		if (at(r, ui.hover_x, ui.hover_y) !=
		    at(r, e->motion.x, e->motion.y))
			ui.dirty = TRUE;
		ui.hover_x = e->motion.x;
		ui.hover_y = e->motion.y;
		return TRUE;
	}
	case SDL_DROPFILE:
		SDL_free(e->drop.file);
		return TRUE;
	case SDL_TEXTINPUT:
	case SDL_TEXTEDITING:
	case SDL_KEYUP:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEWHEEL:
		return TRUE;
	default:
		return FALSE;
	}
}

gboolean ui_media_event(SDL_Event *e)
{
	if (m.play_event && e->type == m.play_event) {
		struct clip_event *clip = e->user.data1;

		if (m.playback && clip->serial == m.play_serial) {
			g_clear_pointer(&m.playback, line_video_play_free);
			if (clip->error) {
				if (m.play_review && m.mode == PREVIEW &&
				    m.serial == m.play_overlay)
					set(&m.note, clip->error);
				else
					ui_status(clip->error);
			}
		}
		free_clip_event(clip);
		return TRUE;
	}
	if (m.screenshot_event && e->type == m.screenshot_event) {
		struct screenshot_event *out = e->user.data1;

		g_thread_join(m.screenshot_thread);
		m.screenshot_thread = NULL;
		g_clear_object(&m.screenshot_cancel);
		if (ui.connected && ui.selected && out->epoch == m.epoch) {
			if (out->path && m.mode == CLOSED &&
			    !ui_media_layer()) {
				prepare_attachment(out->path);
				if (m.mode == PREVIEW)
					set(&m.title, "Screenshot");
			} else {
				ui_status(
					out->error ?
						out->error :
						"Screenshot saved by the desktop.");
			}
		}
		free_screenshot_event(out);
		return TRUE;
	}
	if (m.record_event && e->type == m.record_event) {
		struct recording_event *evt = e->user.data1;

		if (evt->serial == m.serial && m.mode == RECORDING) {
			if (evt->path) {
				m.mode = CAMERA;
				prepare_attachment(evt->path);
				if (m.mode == PREVIEW) {
					m.recorded =
						g_steal_pointer(&evt->path);
					set(&m.title, "Recorded video.mp4");
				}
			} else {
				m.mode = CAMERA;
				m.working = FALSE;
				set(&m.note, evt->error ? evt->error :
							  "Recording stopped.");
			}
		}
		free_record_event(evt);
		return TRUE;
	}
	if (m.decode_event && e->type == m.decode_event) {
		complete_decode(e->user.data1);
		return TRUE;
	}
	if (!ui.connected) {
		if (e->type == SDL_DROPFILE) {
			SDL_free(e->drop.file);
			return TRUE;
		}
		return FALSE;
	}
	if (ui_media_fullscreen() && fullscreen_event(e))
		return TRUE;
	if (e->type == SDL_MOUSEBUTTONDOWN &&
	    e->button.button == SDL_BUTTON_LEFT) {
		if (e->button.clicks <= 1) {
			m.first_click_tile = TILE_NONE;
			m.ignore_multiclick = FALSE;
		} else if (m.ignore_multiclick)
			return TRUE;
	}
	if (e->type == SDL_MOUSEBUTTONDOWN &&
	    e->button.button == SDL_BUTTON_LEFT) {
		int target = ui_media_dock_hover(e->button.x, e->button.y);

		if (target) {
			if (target == -201)
				call_rpc("callEnd");
			else if (target == -202)
				call_rpc("callMute");
			else
				show_call();
			return TRUE;
		}
	}
	if (m.mode == CLOSED && ui_profile_event(e))
		return TRUE;
	if (m.mode == CLOSED && !ui_lists_active() &&
	    (ui_chat_tools_event(e) || ui_shared_event(e)))
		return TRUE;
	if (m.mode == CLOSED && ui_lists_active() && ui_lists_event(e))
		return TRUE;
	if (m.mode == CLOSED && ui_groups_active() && ui_groups_event(e))
		return TRUE;
	if (m.mode == CLOSED && ui_find_event(e))
		return TRUE;
	if (m.mode == CLOSED && ui_friends_event(e))
		return TRUE;
	if (m.mode == CLOSED && !ui_find_active() && !ui_groups_active() &&
	    !ui_friends_active() && !ui_chat_tools_active() &&
	    !ui_shared_active() && !ui_lists_active() && ui_lists_event(e))
		return TRUE;
	if (m.mode == EMOJI_PICKER && ui_emoji_input(e))
		return TRUE;
	if (e->type == SDL_DROPFILE) {
		if (ui.selected && (ui.photos || ui.files) && m.mode != CALL &&
		    !m.working)
			prepare_attachment(e->drop.file);
		SDL_free(e->drop.file);
		return TRUE;
	}
	if (e->type == SDL_KEYDOWN)
		return handle_key(e->key);
	/* Around an inline call stage, the chat behaves as usual. */
	if (call_stage_inline()) {
		int x = -1, y = -1;

		if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP)
			x = e->button.x, y = e->button.y;
		else if (e->type == SDL_MOUSEMOTION)
			x = e->motion.x, y = e->motion.y;
		else if (e->type == SDL_MOUSEWHEEL)
			SDL_GetMouseState(&x, &y);
		if (e->type == SDL_TEXTINPUT || e->type == SDL_TEXTEDITING ||
		    (x >= 0 && !at(m.dialog, x, y))) {
			if (e->type == SDL_MOUSEMOTION &&
			    at(m.dialog, ui.hover_x, ui.hover_y))
				ui.dirty = TRUE;
			return FALSE;
		}
	}
	if (m.mode != CLOSED) {
		switch (e->type) {
		case SDL_MOUSEBUTTONDOWN:
			if (e->button.button == SDL_BUTTON_LEFT)
				click_overlay(e->button.x, e->button.y,
					      e->button.clicks);
			return TRUE;
		case SDL_MOUSEMOTION:
			if (m.mode == CALL || m.mode == DIAL) {
				const Rect controls[] = {
					m.primary,
					m.secondary,
					m.third,
					m.share_button,
					m.call_minimize,
					m.fullscreen_button[0],
					m.fullscreen_button[1],
					m.fullscreen_button[3]
				};
				int x = e->motion.x, y = e->motion.y;

				for (guint i = 0; i < G_N_ELEMENTS(controls); i++)
					if (at(controls[i], x, y) !=
					    at(controls[i], ui.hover_x, ui.hover_y))
						ui.dirty = TRUE;
				ui.hover_x = x;
				ui.hover_y = y;
			}
			if ((m.mode == MENU || m.mode == REACT) &&
			    at(m.items, e->motion.x, e->motion.y)) {
				int pos = (e->motion.y - m.items.y) / 34;

				if (pos != m.cursor) {
					m.cursor = pos;
					ui.dirty = TRUE;
				}
			}
			return TRUE;
		case SDL_MOUSEWHEEL:
			if (m.mode == PICK && m.entries) {
				m.offset =
					CLAMP(m.offset - 3 * e->wheel.y, 0,
					      MAX(0, (int)m.entries->len -
							     visible_rows()));
				ui.dirty = TRUE;
			}
			return TRUE;
		case SDL_TEXTINPUT:
			if (m.mode == PICK && !m.working)
				ui_insert(&m.edit, e->text.text);
			return TRUE;
		case SDL_TEXTEDITING:
			if (m.mode == PICK)
				set(&ui.composition, e->edit.text);
			return TRUE;
		default:
			return FALSE;
		}
	}
	if (e->type != SDL_MOUSEBUTTONDOWN || !ui_chat_visible() ||
	    !ui.selected)
		return FALSE;
	int x = e->button.x, y = e->button.y;

	if (e->button.button == SDL_BUTTON_LEFT) {
		if (at(ui.chat_tools, x, y)) {
			ui_chat_tools_open();
			return TRUE;
		}
		if ((ui.emojis || ui.stickers) && at(ui.emoji, x, y)) {
			show_emoji();
			return TRUE;
		}
		if ((ui.files || ui.photos) && at(ui.attach, x, y)) {
			open_picker(FALSE, NULL, FALSE);
			return TRUE;
		}
		if (ui_media_can_call() && at(ui.call, x, y)) {
			dial(FALSE);
			return TRUE;
		}
		if (ui.video_calls && ui_media_can_call() &&
		    at(ui.video, x, y)) {
			dial(TRUE);
			return TRUE;
		}
	}
	Json *row = ui_row_at(x, y);

	if (!row)
		return FALSE;
	if (e->button.button == SDL_BUTTON_RIGHT) {
		m.menu_x = x;
		m.menu_y = y;
		menu_row(row);
		return TRUE;
	}
	if (e->button.button == SDL_BUTTON_LEFT && ui_is_photo(row)) {
		picture(row);
		return TRUE;
	}
	if (e->button.button == SDL_BUTTON_LEFT && ui_is_video(row)) {
		open_video(row);
		return TRUE;
	}
	if (e->button.button == SDL_BUTTON_LEFT && ui.files && file_kind(row)) {
		open_picker(TRUE, row, FALSE);
		return TRUE;
	}
	return FALSE;
}

/* The same canvas handles image review, file browsing and call controls. */
int ui_media_layer(void)
{
	if (m.mode == CLOSED && ui_profile_active())
		return ui_profile_layer();
	if (m.mode == CLOSED && ui_lists_active())
		return 82;
	if (m.mode == CLOSED && (ui_chat_tools_active() || ui_shared_active()))
		return 83;
	if (m.mode == CLOSED && ui_find_active())
		return 81;
	if (m.mode == CLOSED && ui_groups_active())
		return 80;
	return m.mode == CLOSED			 ? 0 :
	       m.mode == MENU || m.mode == REACT ? 1 :
						   (int)m.mode + 2;
}

void ui_media_overlay(void)
{
	m.call_frame.view.w = m.self_frame.view.w = m.share_frame.view.w = 0;
	/* Settings stay behind the avatar picker. */
	if (ui_profile_active()) {
		ui_profile_draw();
		if (m.mode == CLOSED)
			return;
	}
	if (m.mode == CLOSED && ui_lists_active()) {
		ui_lists_draw();
		return;
	}
	if (m.mode == CLOSED && ui_chat_tools_active()) {
		ui_chat_tools_draw();
		return;
	}
	if (m.mode == CLOSED && ui_shared_active()) {
		ui_shared_draw();
		return;
	}
	if (m.mode == CLOSED && ui_find_active()) {
		ui_find_draw();
		return;
	}
	if (m.mode == CLOSED && ui_groups_active()) {
		ui_groups_overlay();
		return;
	}
	if (m.mode == CLOSED)
		return;
	if (m.mode == EMOJI_PICKER) {
		ui_emoji_draw();
		return;
	}
	m.third = (Rect){ 0 };
	if (m.mode == MENU || m.mode == REACT) {
		const char *const labels[] = {
			"Reply",	  "Copy text",	      "React…",
			"View image",	  "Save attachment…", "Unsend…",
			"Copy image",	  "Forward…",	      "Announce…",
			"Mention sender", "Play video"
		};
		enum message_action arr[12];
		int count = m.mode == MENU ? actions(arr) :
					     (int)G_N_ELEMENTS(reaction_labels);
		int width = MIN(ui.width - 16, 220), height = count * 34 + 16;
		int x = CLAMP(m.menu_x, 8, ui.width - width - 8),
		    y = CLAMP(m.menu_y, 8, ui.height - height - 8);
		m.dialog = (Rect){ x, y, width, height };
		m.items = (Rect){ x + 6, y + 8, width - 12, count * 34 };
		m.primary = (Rect){ 0 };
		m.secondary = (Rect){ 0 };
		ui_draw_rounded((Rect){ x - 1, y - 1, width + 2, height + 2 },
				9, 0x2a2a30);
		ui_draw_rounded(m.dialog, 8, 0x111214);
		for (int j = 0; j < count; j++) {
			Rect r = { m.items.x, m.items.y + j * 34, m.items.w,
				   32 };
			if (m.cursor == j)
				ui_draw_rounded(r, 4, 0x267d72);
			ui_draw_label(m.mode == MENU ? labels[arr[j]] :
						       reaction_labels[j],
				      (Rect){ r.x + 9, r.y + 5, r.w - 15, 25 },
				      14, 500, WHITE, FALSE);
		}
		return;
	}
	if (m.mode == DIAL || m.mode == CALL) {
		draw_call_stage();
		return;
	}
	int w = MIN(620, ui.width - 24),
	    h = MIN(m.mode == VIEW ? ui.height - 24 : 530, ui.height - 24);
	if (m.mode == UNSEND || m.mode == REPLACE || m.mode == CAMERA ||
	    m.mode == RECORDING || m.mode == PLAY_VIDEO)
		h = MIN(h, 246);
	if (m.mode == MENU || m.mode == REACT)
		h = MIN(h, 350);
	int x = (ui.width - w) / 2, y = (ui.height - h) / 2;

	m.dialog = (Rect){ x, y, w, h };
	m.items = (Rect){ 0 };
	m.up = (Rect){ 0 };
	m.primary = (Rect){ 0 };
	m.secondary = (Rect){ 0 };
	ui_scrim();
	ui_draw_box(m.dialog, 0x242429);
	const char *heading =
		m.mode == PICK ? (m.avatar_pick ? "Choose a profile picture" :
				  m.friend_qr	? "Read QR image" :
				  m.save	? "Save attachment" :
						  "Choose an attachment") :
		m.mode == PREVIEW ? (m.photo ? "Send photo" : "Send file") :
		m.mode == REACT	  ? "React to message" :
		m.mode == UNSEND  ? "Unsend message" :
		m.mode == REPLACE ? "File already exists" :
				    m.title;
	ui_draw_label(heading,
		      (Rect){ x + 20, y + 16,
			      w - (m.mode == PICK && !m.save && !m.friend_qr &&
						   !m.avatar_pick && ui.files ?
					   160 :
					   40),
			      35 },
		      21, 700, WHITE, FALSE);
	int bottom = y + h - 55;

	m.primary = (Rect){ x + w - 150, bottom, 130, 36 };
	m.secondary = (Rect){ x + 20, bottom, 116, 36 };
	const char *yes = NULL, *no = NULL;
	guint muted = 0xabacb2;

	if (m.mode == PICK) {
		m.path_entry = (Rect){ x + 20, y + 65, w - 85, 38 };
		m.up = (Rect){ x + w - 55, y + 65, 35, 38 };
		ui_draw_path_entry(&m.edit, m.path_entry);
		ui_draw_button(m.up, "↑", FALSE);
		m.items = (Rect){ x + 20, y + 118, w - 40,
				  MAX(34, bottom - y - 173) };
		int rows = visible_rows();

		for (int j = 0; m.entries && j < rows &&
				(guint)(m.offset + j) < m.entries->len;
		     j++) {
			struct file_item *f =
				g_ptr_array_index(m.entries, m.offset + j);
			Rect r = { m.items.x, m.items.y + j * 34, m.items.w,
				   33 };
			if (m.cursor == m.offset + j)
				ui_draw_box(r, 0x2c2d32);
			ui_draw_label(f->dir ? "▸" : "·",
				      (Rect){ r.x + 7, r.y + 5, 20, 26 }, 16,
				      600, muted, FALSE);
			ui_draw_label(f->name,
				      (Rect){ r.x + 31, r.y + 5, r.w - 40, 26 },
				      15, f->dir ? 650 : 400, WHITE, FALSE);
		}
		if (!m.save && !m.friend_qr && !m.avatar_pick && ui.files) {
			m.third = (Rect){ x + w - 132, y + 16, 112, 34 };
			ui_draw_button(m.third, "Record video", FALSE);
		}
		yes = m.save ? "Save" : "Choose";
		no = "Cancel";
	} else if (m.mode == VIEW || m.mode == PREVIEW) {
		if (m.mode == PREVIEW)
			ui_draw_label(m.title,
				      (Rect){ x + 20, y + 55, w - 40, 30 }, 15,
				      500, muted, FALSE);
		Rect image = { x + 20, y + (m.mode == PREVIEW ? 93 : 62),
			       w - 40, h - (m.mode == PREVIEW ? 210 : 178) };
		if (m.image)
			ui_draw_image(m.sticker ? animation_frame(m.image) :
						  m.image,
				      image);
		else if (m.working)
			ui_draw_label("Loading…",
				      (Rect){ image.x, image.y + 12, image.w,
					      32 },
				      15, 500, muted, FALSE);
		if (m.mode == PREVIEW && !m.photo &&
		    strcmp(attachment_type(m.path), "file")) {
			m.third = (Rect){ x + 20, y + 102, 136, 36 };
			ui_draw_button(m.third, "Play preview", FALSE);
		}
		if (m.mode == VIEW)
			yes = "Close";
		else {
			yes = m.working ? "Please wait…" : "Send";
			no = "Cancel";
		}
	} else if (m.mode == PLAY_VIDEO) {
		no = "Close";
		if (!m.working) {
			yes = "Retry";
			m.third = (Rect){ x + 20, y + 67, 118, 34 };
			ui_draw_button(m.third, "Save video", FALSE);
		}
	} else if (m.mode == CAMERA || m.mode == RECORDING) {
		yes = m.mode == CAMERA ? "Start recording" :
		      m.working	       ? "Finishing…" :
					 "Stop recording";
		no = "Cancel";
	} else {
		yes = m.mode == REPLACE ? "Replace" : "Unsend";
		no = "Cancel";
	}
	if (m.note && *m.note)
		ui_draw_label(m.note, (Rect){ x + 20, bottom - 48, w - 40, 46 },
			      13, 500, muted, TRUE);
	if (yes)
		ui_draw_button(m.primary, yes, TRUE);
	else
		m.primary = (Rect){ 0 };
	if (no)
		ui_draw_button(m.secondary, no, FALSE);
	else
		m.secondary = (Rect){ 0 };
}
