// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat_lists.h"
#include "state.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_BYTES (2 * 1024 * 1024)

typedef struct json_object Json;

struct chat_lists {
	int dir;
	char *file;
	Json *settings;
};

static const char *const defaults[] = { [LIST_FOLDERS] = "Ctrl+Shift+L",
					[LIST_PIN] = "Ctrl+Shift+P",
					[LIST_SEARCH] = "Ctrl+F",
					[LIST_FIND] = "Ctrl+Shift+F",
					[LIST_REFRESH] = "Ctrl+R" };
static const char *const keys[] = { "folders", "pin", "search", "find",
				    "refresh" };

static gboolean fail(GError **e, const char *text)
{
	g_set_error_literal(e, G_FILE_ERROR, G_FILE_ERROR_FAILED, text);
	return FALSE;
}

static gboolean io_error(GError **e)
{
	int err = errno;

	g_set_error(e, G_FILE_ERROR, g_file_error_from_errno(err),
		    "Could not save or read chat preferences: %s",
		    g_strerror(err));
	return FALSE;
}

static Json *get(Json *obj, const char *key)
{
	Json *v = NULL;

	if (obj && key)
		json_object_object_get_ex(obj, key, &v);
	return v;
}

static Json *map(Json *obj, const char *key)
{
	Json *v = get(obj, key);

	if (!v) {
		v = json_object_new_object();
		json_object_object_add(obj, key, v);
	}
	return v;
}

static gboolean valid_mid(const char *id)
{
	return id && strlen(id) == 33 && strchr("ucr", id[0]) &&
	       strspn(id + 1, "0123456789abcdef") == 32;
}

static gboolean valid_name(const char *name)
{
	if (!name || !*name || strlen(name) > 192 ||
	    !g_utf8_validate(name, -1, NULL) || g_utf8_strlen(name, -1) > 48)
		return FALSE;
	for (const char *s = name; *s; s = g_utf8_next_char(s))
		if (g_unichar_iscntrl(g_utf8_get_char(s)))
			return FALSE;
	return TRUE;
}

static gboolean write_all(int fd, const char *s, size_t len)
{
	while (len) {
		ssize_t n = write(fd, s, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return FALSE;
		s += n;
		len -= n;
	}
	return TRUE;
}

/*
 * The paired worker already excludes another writer for this profile. Publish a
 * complete replacement before updating the in-memory view.
 */
static gboolean commit(struct chat_lists *l, Json *new, GError **e)
{
	if (!new)
		return fail(e, "Chat preferences are unavailable.");
	size_t len;
	const char *data = json_object_to_json_string_length(
		new, JSON_C_TO_STRING_PLAIN, &len);
	gboolean ok = FALSE;

	g_autofree char *temp = NULL;

	if (len > MAX_BYTES) {
		fail(e, "Chat preferences exceed their size limit.");
		goto out;
	}
	int fd = -1;

	for (int i = 0; i < 8 && fd < 0; i++) {
		g_free(temp);
		temp = g_strdup_printf(".lists-%08x.tmp", g_random_int());
		fd = openat(l->dir, temp,
			    O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC |
				    O_NOFOLLOW,
			    0600);
		if (fd < 0 && errno != EEXIST)
			break;
	}
	if (fd < 0) {
		io_error(e);
		goto out;
	}
	ok = write_all(fd, data, len) && fdatasync(fd) == 0;
	if (close(fd) && ok)
		ok = FALSE;
	if (!ok || renameat(l->dir, temp, l->dir, l->file)) {
		io_error(e);
		unlinkat(l->dir, temp, 0);
		ok = FALSE;
		goto out;
	}
	json_object_put(l->settings);
	l->settings = json_object_get(new);
	if (fsync(l->dir))
		ok = io_error(e);
out:
	json_object_put(new);
	return ok;
}

static Json *copy(struct chat_lists *l)
{
	Json *out = NULL;

	if (!l || json_object_deep_copy(l->settings, &out, NULL))
		return NULL;
	return out;
}

static gboolean valid_set(Json *set)
{
	if (!json_object_is_type(set, json_type_object))
		return FALSE;
	json_object_object_foreach(set, id, v) {
		if (!valid_mid(id) ||
		    !json_object_is_type(v, json_type_boolean))
			return FALSE;
	}
	return TRUE;
}

static gboolean valid_settings(Json *o)
{
	if (!json_object_is_type(o, json_type_object))
		return FALSE;
	Json *folders = get(o, "folders"), *pins = get(o, "pins"),
	     *bindings = get(o, "bindings"), *muted = get(o, "muted"),
	     *strict = get(o, "strictMuted");
	if ((pins && !valid_set(pins)) || (muted && !valid_set(muted)) ||
	    (strict && !valid_set(strict)) ||
	    (get(o, "motion") &&
	     !json_object_is_type(get(o, "motion"), json_type_boolean)) ||
	    (bindings && !json_object_is_type(bindings, json_type_object)) ||
	    (get(o, "view") &&
	     !json_object_is_type(get(o, "view"), json_type_string)) ||
	    (get(o, "notify") &&
	     !json_object_is_type(get(o, "notify"), json_type_boolean)))
		return FALSE;
	if (bindings) {
		json_object_object_foreach(bindings, key, value) {
			(void)key;
			if (!json_object_is_type(value, json_type_string) ||
			    json_object_get_string_len(value) > 40)
				return FALSE;
		}
	}
	if (folders) {
		if (!json_object_is_type(folders, json_type_object))
			return FALSE;
		json_object_object_foreach(folders, name, set) {
			if (!valid_name(name) || !valid_set(set))
				return FALSE;
		}
	}
	return TRUE;
}

struct chat_lists *chat_lists_open(const char *account, GError **e)
{
	if (!valid_mid(account) || account[0] != 'u') {
		fail(e, "A personal account is required for chat folders.");
		return NULL;
	}
	g_autofree char *path =
		g_build_filename(g_get_user_state_dir(), LINE_STATE_DIR, NULL);
	g_autofree char *hash =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, account, -1);
	struct chat_lists *l = g_new0(struct chat_lists, 1);

	l->dir = -1;
	l->file = g_strdup_printf("lists-%s.json", hash);
	l->settings = json_object_new_object();
	if (g_mkdir_with_parents(path, 0700))
		goto io;
	l->dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (l->dir < 0)
		goto io;
	struct stat st;

	if (fstat(l->dir, &st) || st.st_uid != geteuid() || (st.st_mode & 077))
		goto invalid;
	int fd = openat(l->dir, l->file,
			O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			return l;
		goto io;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 077) || st.st_size <= 0 || st.st_size > MAX_BYTES) {
		close(fd);
		goto invalid;
	}
	char *bytes = g_malloc(st.st_size + 1);
	size_t have = 0, need = st.st_size;

	while (have < need) {
		ssize_t n = read(fd, bytes + have, need - have);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		have += n;
	}
	close(fd);
	struct json_tokener *tok = json_tokener_new_ex(16);

	json_tokener_set_flags(tok, JSON_TOKENER_STRICT |
					    JSON_TOKENER_VALIDATE_UTF8);
	Json *loaded = json_tokener_parse_ex(tok, bytes, have);
	gboolean ok = have == need &&
		      json_tokener_get_error(tok) == json_tokener_success &&
		      json_tokener_get_parse_end(tok) == have &&
		      valid_settings(loaded);
	json_tokener_free(tok);
	g_free(bytes);
	if (!ok) {
		json_object_put(loaded);
		goto invalid;
	}
	json_object_put(l->settings);
	l->settings = loaded;
	return l;
invalid:
	fail(e,
	     "The local chat preferences have invalid permissions or contents.");
	goto failed;
io:
	io_error(e);
failed:
	chat_lists_free(l);
	return NULL;
}

void chat_lists_free(struct chat_lists *l)
{
	if (!l)
		return;
	if (l->dir >= 0)
		close(l->dir);
	g_free(l->file);
	json_object_put(l->settings);
	g_free(l);
}

Json *chat_folders(struct chat_lists *l)
{
	return l ? get(l->settings, "folders") : NULL;
}

const char *chat_current_view(struct chat_lists *l)
{
	Json *v = l ? get(l->settings, "view") : NULL;

	return v ? json_object_get_string(v) : "all";
}

gboolean chat_view_set(struct chat_lists *l, const char *view, GError **e)
{
	if (!view || (strcmp(view, "all") && strcmp(view, "unread") &&
		      strcmp(view, "pinned") && strcmp(view, "people") &&
		      strcmp(view, "groups") &&
		      !(g_str_has_prefix(view, "folder:") &&
			get(chat_folders(l), view + 7))))
		return fail(e, "Choose an existing folder.");
	Json *o = copy(l);

	if (o)
		json_object_object_add(o, "view", json_object_new_string(view));
	return commit(l, o, e);
}

gboolean chat_folder_set(struct chat_lists *l, const char *old,
			 const char *name, const char *chat, GError **e)
{
	if (!l || (!old && !name) || (chat && !valid_mid(chat)) ||
	    (old && !get(chat_folders(l), old)))
		return fail(e, "Choose an existing chat or folder.");
	g_autofree char *trim = name ? g_strdup(name) : NULL;

	if (trim && !valid_name(g_strstrip(trim)))
		return fail(e, "Enter a folder name of up to 48 characters.");
	Json *o = copy(l);

	if (!o)
		return commit(l, NULL, e);
	Json *folders = map(o, "folders");

	if (trim) {
		g_autofree char *fold = g_utf8_casefold(trim, -1);

		json_object_object_foreach(folders, key, v) {
			(void)v;
			g_autofree char *other = g_utf8_casefold(key, -1);

			if (g_strcmp0(old, key) && !strcmp(fold, other)) {
				json_object_put(o);
				return fail(e,
					    "A folder already has that name.");
			}
		}
	}
	Json *set = old ? json_object_get(get(folders, old)) :
			  json_object_new_object();
	if (old)
		json_object_object_del(folders, old);
	if (chat)
		json_object_object_add(set, chat,
				       json_object_new_boolean(TRUE));
	if (trim)
		json_object_object_add(folders, trim, set);
	else
		json_object_put(set);
	const char *view = chat_current_view(l);

	if (old && g_str_has_prefix(view, "folder:") &&
	    !strcmp(view + 7, old)) {
		g_autofree char *next =
			trim ? g_strconcat("folder:", trim, NULL) :
			       g_strdup("all");
		json_object_object_add(o, "view", json_object_new_string(next));
	}
	return commit(l, o, e);
}

gboolean chat_folder_contains(struct chat_lists *l, const char *name,
			      const char *chat)
{
	return json_object_get_boolean(get(get(chat_folders(l), name), chat));
}

gboolean chat_has_pins(struct chat_lists *l)
{
	Json *p = l ? get(l->settings, "pins") : NULL;

	return p && json_object_object_length(p) > 0;
}

gboolean chat_pinned(struct chat_lists *l, const char *chat)
{
	return l &&
	       json_object_get_boolean(get(get(l->settings, "pins"), chat));
}

static gboolean toggle_in(struct chat_lists *l, const char *name,
			  const char *set_key, const char *chat, GError **e)
{
	if (!valid_mid(chat) || (name && !get(chat_folders(l), name)))
		return fail(e, "Choose an available conversation.");
	Json *o = copy(l);

	if (!o)
		return commit(l, NULL, e);
	Json *set = name ? get(get(o, "folders"), name) : map(o, set_key);

	if (json_object_get_boolean(get(set, chat)))
		json_object_object_del(set, chat);
	else
		json_object_object_add(set, chat,
				       json_object_new_boolean(TRUE));
	return commit(l, o, e);
}

gboolean chat_pin_toggle(struct chat_lists *l, const char *chat, GError **e)
{
	return toggle_in(l, NULL, "pins", chat, e);
}

gboolean chat_folder_toggle(struct chat_lists *l, const char *name,
			    const char *chat, GError **e)
{
	return toggle_in(l, name, NULL, chat, e);
}

gboolean chat_muted(struct chat_lists *l, const char *chat)
{
	return l && chat &&
	       json_object_get_boolean(get(get(l->settings, "muted"), chat));
}

gboolean chat_mute_toggle(struct chat_lists *l, const char *chat, GError **e)
{
	return toggle_in(l, NULL, "muted", chat, e);
}

gboolean chat_strict_muted(struct chat_lists *l, const char *chat)
{
	return l && chat && *chat == 'c' &&
	       json_object_get_boolean(
		       get(get(l->settings, "strictMuted"), chat));
}

gboolean chat_strict_mute_toggle(struct chat_lists *l, const char *chat,
				 GError **e)
{
	if (!chat || *chat != 'c')
		return fail(e, "Choose a group conversation.");
	return toggle_in(l, NULL, "strictMuted", chat, e);
}

int chat_motion(struct chat_lists *l)
{
	Json *v = l ? get(l->settings, "motion") : NULL;

	return v ? json_object_get_boolean(v) : -1;
}

gboolean chat_motion_set(struct chat_lists *l, int reduce, GError **e)
{
	Json *o = copy(l);

	if (o && reduce < 0)
		json_object_object_del(o, "motion");
	else if (o)
		json_object_object_add(o, "motion",
				       json_object_new_boolean(reduce));
	return commit(l, o, e);
}

gboolean chat_notifications(struct chat_lists *l)
{
	Json *v = l ? get(l->settings, "notify") : NULL;

	return !v || json_object_get_boolean(v);
}

gboolean chat_notifications_set(struct chat_lists *l, gboolean on, GError **e)
{
	Json *o = copy(l);

	if (o)
		json_object_object_add(o, "notify",
				       json_object_new_boolean(on));
	return commit(l, o, e);
}

gboolean chat_sounds(struct chat_lists *l)
{
	Json *v = l ? get(l->settings, "sounds") : NULL;

	return !v || json_object_get_boolean(v);
}

gboolean chat_sounds_set(struct chat_lists *l, gboolean on, GError **e)
{
	Json *o = copy(l);

	if (o)
		json_object_object_add(o, "sounds",
				       json_object_new_boolean(on));
	return commit(l, o, e);
}

const char *chat_binding(struct chat_lists *l, enum list_command cmd)
{
	if (cmd < 0 || cmd >= LIST_COMMANDS)
		return "";
	Json *v = l ? get(get(l->settings, "bindings"), keys[cmd]) : NULL;

	return v ? json_object_get_string(v) : defaults[cmd];
}

gboolean chat_binding_set(struct chat_lists *l, enum list_command cmd,
			  const char *chord, GError **e)
{
	if (cmd < 0 || cmd >= LIST_COMMANDS || !chord || !*chord ||
	    strlen(chord) > 40)
		return fail(e, "Choose a keyboard shortcut.");
	for (int i = 0; i < LIST_COMMANDS; i++)
		if (i != (int)cmd && !strcmp(chat_binding(l, i), chord))
			return fail(e, "That shortcut is already assigned.");
	Json *o = copy(l);

	if (o)
		json_object_object_add(map(o, "bindings"), keys[cmd],
				       json_object_new_string(chord));
	return commit(l, o, e);
}
