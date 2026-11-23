/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_lang.h"
#include "ui_theme.h"
#include <SDL.h>
#include <cairo.h>
#include <glib.h>
#include <json-c/json.h>
#include <pango/pangocairo.h>
#include <qrencode.h>

typedef struct json_object Json;

typedef struct {
	int x, y, w, h;
} Rect;

typedef enum {
	F_LOGIN,
	F_LOGIN_MODE,
	F_LOGIN_EMAIL,
	F_LOGIN_PASSWORD,
	F_SEARCH,
	F_LIST,
	F_MESSAGE,
	F_SEND,
	F_RELOAD,
	F_LOGOUT,
	F_BACK,
	F_FRIENDS,
	F_GROUP_NEW,
	F_MEMBERS,
	F_FIND_TEXT
} Focus;

typedef struct {
	GString *value;
	int cursor, anchor; /* UTF-8 byte offsets */
	int scroll;
} Editor;

typedef struct {
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Surface *buffer;
	int width, height;
	/* Animated height of the call stage above the messages. */
	int stage_h;
	gboolean dirty, active, connected, authenticating, logging_out,
		getting_chats, quitting;
	gboolean video_dirty;
	gboolean email_login;
	/*
	 * A session is saved on this computer but isn't signed in: the
	 * sign-in screen offers to resume it or to sign it out.
	 */
	gboolean saved_session;
	gboolean find_messages, friends, friend_invite, groups, emojis,
		stickers, avatars, photos, files, calls, video_calls,
		incoming_calls, mute_calls;
	char *self_id, *reply_id, *reply_chat;
	char *name, *notice, *selected, *pin, *composition;
	QRcode *qr;
	Json *chats, *history, *opened;
	GArray *visible; /* indexes into chats */
	GHashTable *drafts, *sending;
	Editor search, message, login_email, login_password;
	Focus focus;
	int list_offset, list_cursor, history_offset, max_history_offset,
		hover_x, hover_y;
	gboolean stick_bottom;
	gboolean older_page, older_pending, has_older;
	Rect older_button, latest_button, folders_button;
	Rect login_mode, email_box, password_box;
	Rect chat_tools, find_button, friends_button, new_group, members, emoji,
		attach, call, video, login, back, logout, reload, search_box,
		list_box, conversation, entry, send, composer, rail_add,
		rail_chats;
} Interface;

enum {
	UI_HEADER_H = 48
};

static inline int ui_chat_row_height(void)
{
	return ui_theme_is_line() ? 68 : 44;
}

void ui_theme_changed(int old_row_height);
void ui_language_changed(void);

/* Glyphs for round call controls. */
enum ui_call_glyph {
	UI_GLYPH_CAMERA,
	UI_GLYPH_CAMERA_OFF,
	UI_GLYPH_MIC,
	UI_GLYPH_MIC_OFF,
	UI_GLYPH_HANG_UP,
	UI_GLYPH_ACCEPT,
	UI_GLYPH_CLOSE,
	UI_GLYPH_SCREEN,
	UI_GLYPH_SCREEN_STOP,
	UI_GLYPH_MINIMIZE
};
void ui_draw_round_button(Rect r, enum ui_call_glyph glyph, guint fill,
			  guint ink, const char *tip);
void ui_draw_rounded(Rect r, int radius, guint rgb);
void ui_draw_avatar(const char *name, const char *seed, Rect r);
void ui_draw_tooltip(Rect anchor, const char *s);
/* A tooltip to the right of anchor, for the navigation bar. */
void ui_draw_tooltip_beside(Rect anchor, const char *s);
void ui_draw_centered(const char *s, Rect r, int size, int weight, guint rgb);

/* Left edge of the main pane: the rail and list stay visible beside it. */
int ui_content_x(void);
enum ui_icon {
	UI_ICON_PEOPLE,
	UI_ICON_MESSAGE,
	UI_ICON_MORE,
	UI_ICON_GEAR,
	UI_ICON_INBOX,
	UI_ICON_CHECK,
	UI_ICON_CLOSE
};
/* Overlay layers the profile module owns; popovers dim nothing behind. */
enum {
	UI_LAYER_POPOUT = 90,
	UI_LAYER_SETTINGS = 91,
	UI_LAYER_INBOX = 92
};
/* Device pixels per logical pixel for the current window. */
double ui_pixel_scale(void);
/* Pixel width of one line of text. */
int ui_text_width(const char *s, int size, int weight);
/* Pixel height of wrapped text at width w. */
int ui_text_height(const char *s, int w, int size, int weight);
void ui_draw_outline(Rect r, int radius, guint rgb);
/* A Discord toggle switch, 40 x 24, sliding between off and on. */
void ui_draw_switch(Rect r, gboolean on);
/* 0..1 hover fade for r, sampled at this frame. */
double ui_draw_hover(Rect r);
guint ui_blend(guint a, guint b, double v);
/* The fill of a letter avatar without a picture. */
guint ui_avatar_shade(const char *seed);
/* The sender avatar or name under the pointer, with its message row. */
Json *ui_sender_at(int x, int y, Rect *anchor);
/* A clickable area of a rich message, and the scene it switches to. */
Json *ui_rich_area_at(int x, int y, Json **message, int *key);
void ui_rich_set_scene(Json *message, const char *scene);
/* The avatar and name in a one-to-one chat header. */
gboolean ui_header_peer_at(int x, int y, Rect *anchor);
void ui_draw_icon(Rect r, enum ui_icon icon, guint ink);
/* Draws a one-pixel focus ring outside r. */
void ui_draw_focus(Rect r);
void ui_forget_rail(void);
/* Rail shortcut under the pointer (an unread chat), or NULL. */
const char *ui_rail_chat_at(int x, int y);
/* Index of the message row under the pointer, or -1. */
int ui_message_at(int x, int y);

extern Interface ui;
int ui_password_index(int x);

static inline gboolean ui_compact(void)
{
	return ui.width <= 640;
}

static inline gboolean ui_chat_visible(void)
{
	return !ui_compact() ||
	       (ui.selected && (ui.focus == F_MESSAGE || ui.focus == F_SEND ||
				ui.focus == F_BACK || ui.focus == F_MEMBERS ||
				ui.focus == F_FIND_TEXT));
}

static inline gboolean ui_list_visible(void)
{
	return !ui_compact() || !ui_chat_visible();
}

/*
 * Keep the head actually shown on this device separate from the receipt
 * confirmed by LINE. A delayed ACK must not clear a newer delivery.
 */
typedef struct {
	guint64 viewed, confirmed, attempted, pending;
} UiReadCursor;

static inline guint64 ui_read_id(const char *text)
{
	guint64 id;

	return text && *text >= '1' && *text <= '9' &&
			       g_ascii_string_to_unsigned(
				       text, 10, 1, G_MAXINT64, &id, NULL) ?
		       id :
		       0;
}

static inline gboolean ui_read_visible(const Interface *view, Uint32 window,
				       gboolean covered, gboolean latest,
				       gboolean chat_visible)
{
	return view->connected && !view->logging_out && !view->quitting &&
	       view->active && latest && chat_visible && !covered &&
	       !view->older_page && !view->older_pending &&
	       view->stick_bottom &&
	       view->history_offset == view->max_history_offset &&
	       (window & SDL_WINDOW_SHOWN) &&
	       (window & SDL_WINDOW_INPUT_FOCUS) &&
	       !(window & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
}

/* A presentation can advance the local badge while an earlier RPC is pending. */
static inline gboolean ui_read_present(UiReadCursor *cursor, guint64 target,
				       guint64 server_seen)
{
	cursor->confirmed = MAX(cursor->confirmed, server_seen);
	cursor->viewed = MAX(cursor->viewed, target);
	if (!target || cursor->pending || target <= cursor->confirmed ||
	    target <= cursor->attempted)
		return FALSE;
	cursor->pending = cursor->attempted = target;
	return TRUE;
}

static inline gboolean ui_read_covers(const UiReadCursor *cursor, guint64 head,
				      guint64 server_seen)
{
	return head &&
	       (head <= server_seen ||
		(cursor && head <= MAX(cursor->viewed, cursor->confirmed)));
}

static inline gboolean ui_read_finish(UiReadCursor *cursor, guint64 target,
				      gboolean confirmed, guint64 latest)
{
	if (!target || cursor->pending != target)
		return FALSE;
	cursor->pending = 0;
	if (confirmed)
		cursor->confirmed = MAX(cursor->confirmed, target);
	return confirmed && target == latest;
}

void ui_conversation_presented(gboolean covered);
gboolean ui_message_is_read(Json *message);
void ui_message_reads(Json *packet);
Json *ui_field(Json *obj, const char *key);
const char *ui_string(Json *obj, const char *key, const char *fallback);
gboolean ui_draw_init(void);
void ui_draw(void);
void ui_draw_region(Rect logical);
void ui_draw_animation(void);
void ui_draw_pointer(const SDL_Event *event);
void ui_draw_video(void);
void ui_draw_reset_renderer(void);
void ui_forget_history(void);
void ui_draw_shutdown(void);
void ui_forget_labels(void);
void ui_layout(void);
int ui_text_index(Editor *editor, int x);

void ui_rpc(const char *method, Json *params, guint tag); /* consumes params */
void ui_sign_out(void);
const char *ui_build_commit(void);
/* The file picker in avatar mode; the choice goes to ui_profile. */
void ui_media_pick_avatar(void);
/* Pastes a clipboard picture or copied file into a send preview. */
gboolean ui_media_paste(void);
void ui_status(const char *message);
void ui_refresh(void);
void ui_show_latest(void);
void ui_restore_input(void);
void ui_edit_key(Editor *edit, SDL_KeyboardEvent key);
void ui_insert(Editor *edit, const char *text);
void ui_mention_member(const char *id, const char *name);
void ui_media_login(Json *result);
void ui_media_reset(void);
void ui_media_shutdown(void);
void ui_media_screenshot(void);
gboolean ui_media_event(SDL_Event *e);
void ui_media_reply(const char *method, Json *params, guint tag, Json *result,
		    const char *error);
void ui_media_call_event(Json *packet);
void ui_media_window_visibility(gboolean visible);
gboolean ui_media_fullscreen(void);
void ui_media_leave_fullscreen(void);
void ui_media_draw_fullscreen(void);
int ui_media_content_height(void);
/* Height of the call stage above the selected chat's messages, or 0. */
int ui_media_stage_height(void);
/* After the selected chat changes: show or minimize the call stage. */
void ui_media_chat_shown(void);
int ui_media_dock_hover(int x, int y);
void ui_media_draw_call_dock(void);
struct line_preview_frame;
void ui_media_preview_frame(const struct line_preview_frame *frame, int fd);
void ui_media_overlay(void);
void ui_media_present_video(double y_offset);
struct gpu_scene;
void ui_media_gpu_video(struct gpu_scene *scene, double y_offset);
void ui_media_reset_renderer(void);
void ui_media_thumb(Json *row, Rect area);
void ui_media_select_chat(void);
gboolean ui_media_can_call(void);
void ui_media_reply_sent(const char *chat);
Json *ui_row_at(int x, int y);
const char *ui_flex_link_at(int x, int y);
cairo_surface_t *ui_flex_image(const char *url);
/* The picture at url could not be loaded or decoded. */
gboolean ui_flex_image_failed(const char *url);
Json *ui_selected_chat(void);
gboolean ui_is_photo(Json *row);
gboolean ui_is_video(Json *row);
void ui_media_video_poster(Json *row, Rect area);
void ui_draw_box(Rect r, guint rgb);
void ui_draw_label(const char *s, Rect r, int size, int weight, guint rgb,
		   gboolean wrap);
void ui_draw_button(Rect r, const char *s, gboolean primary);
void ui_draw_image(cairo_surface_t *image, Rect r);
void ui_draw_rotated_image(cairo_surface_t *image, Rect r, unsigned int angle);
void ui_draw_path_entry(Editor *editor, Rect r);
cairo_surface_t *ui_avatar(const char *chat, const char *id, const char *key);
void ui_scrim(void);

gint64 ui_animation_deadline(gint64 now_us);
gint64 ui_emoji_deadline(void);
int ui_media_layer(void);
