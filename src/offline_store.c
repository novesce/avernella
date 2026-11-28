// SPDX-License-Identifier: GPL-3.0-or-later
#include "offline_store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROFILE_FILE "profile"
#define CHAT_LIST_FILE "chats"
#define OUTBOX_FILE "outbox"
#define STAGE_FILE ".pending"
#define CHAT_PREFIX "chat-"
#define PROFILE_LIMIT (16U * 1024U)
#define DIRECTORY_LIMIT (2U * 1024U * 1024U)
#define HISTORY_LIMIT (1024U * 1024U)
#define OUTBOX_LIMIT (1024U * 1024U)

enum {
	ARCHIVE_CHATS = 96,
	ARCHIVE_ROWS = 256,
	ARCHIVE_PAGE = 50,
};

struct line_offline {
	int dir;
	char account[65];
	guint8 key[LINE_STATE_KEY_SIZE];
	gboolean outbox_writable;
};

static gboolean io_error(GError **error)
{
	int code = errno;

	g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code),
		    "Could not read or write offline data: %s",
		    g_strerror(code));
	return FALSE;
}

static gboolean invalid_data(GError **error)
{
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Could not verify the encrypted offline data.");
	return FALSE;
}

static int private_dir(int parent, const char *name, gboolean create)
{
	struct stat st;

	if (create && mkdirat(parent, name, 0700) && errno != EEXIST)
		return -1;
	int fd = openat(parent, name,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (!fstat(fd, &st)) {
		if (st.st_uid == geteuid() && !(st.st_mode & 077))
			return fd;
		errno = EACCES;
	}
	int saved = errno;

	close(fd);
	errno = saved;
	return -1;
}

static gboolean valid_mid(const char *mid)
{
	return mid && strlen(mid) == 33 && strchr("ucrm", mid[0]) &&
	       strspn(mid + 1, "0123456789abcdefABCDEF") == 32;
}

LineOffline *line_offline_open(int state_dir, const char *mid,
			       const guint8 key[LINE_STATE_KEY_SIZE])
{
	if (state_dir < 0 || !valid_mid(mid) || *mid != 'u' || !key)
		return NULL;
	g_autofree char *account =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, mid, -1);
	int parent = private_dir(state_dir, "offline", TRUE);

	if (parent < 0)
		return NULL;
	int dir = private_dir(parent, account, TRUE);

	if (dir >= 0) {
		/* Publish newly created directories before accepting a send. */
		if (fsync(parent) || fsync(state_dir)) {
			close(dir);
			dir = -1;
		}
	}
	close(parent);
	if (dir < 0)
		return NULL;
	LineOffline *store = g_new0(LineOffline, 1);

	store->dir = dir;
	memcpy(store->account, account, sizeof(store->account));
	memcpy(store->key, key, sizeof(store->key));
	return store;
}

void line_offline_close(LineOffline *store)
{
	if (!store)
		return;
	close(store->dir);
	explicit_bzero(store->key, sizeof(store->key));
	g_free(store);
}

static J *parse_payload(LineOffline *store, const char *slot,
			const guint8 *cipher, size_t size)
{
	size_t len = 0;

	g_autofree char *plain =
		line_state_decrypt(store->key, cipher, size, &len, NULL);

	if (!plain)
		return NULL;
	struct json_tokener *parser = json_tokener_new();

	if (!parser) {
		explicit_bzero(plain, len);
		return NULL;
	}
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
	g_autoptr(J) record = json_tokener_parse_ex(parser, plain, len);
	gboolean valid =
		json_tokener_get_error(parser) == json_tokener_success &&
		json_tokener_get_parse_end(parser) == len &&
		!g_strcmp0(strfield(record, "account"), store->account) &&
		!g_strcmp0(strfield(record, "slot"), slot);

	json_tokener_free(parser);
	explicit_bzero(plain, len);
	return valid ? json_object_get(field(record, "payload")) : NULL;
}

static J *read_slot(LineOffline *store, const char *slot, size_t limit,
		    gboolean *missing, GError **error)
{
	if (missing)
		*missing = FALSE;
	if (!store)
		return NULL;
	int fd = openat(store->dir, slot,
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	if (fd < 0) {
		if (errno == ENOENT) {
			if (missing)
				*missing = TRUE;
		} else {
			io_error(error);
		}
		return NULL;
	}
	struct stat st;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    st.st_nlink != 1 || (st.st_mode & 077) ||
	    st.st_size <= LINE_STATE_CRYPTO_OVERHEAD ||
	    (guint64)st.st_size > limit + LINE_STATE_CRYPTO_OVERHEAD) {
		close(fd);
		invalid_data(error);
		return NULL;
	}
	g_autofree guint8 *cipher = g_malloc(st.st_size);
	size_t used = 0;

	while (used < (size_t)st.st_size) {
		ssize_t n = read(fd, cipher + used, st.st_size - used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += n;
	}
	close(fd);
	J *value = used == (size_t)st.st_size ?
			   parse_payload(store, slot, cipher, used) :
			   NULL;

	if (!value)
		invalid_data(error);
	return value;
}

static gboolean flush_slot(int dir, const char *slot, const guint8 *data,
			   size_t len, GError **error)
{
	if (unlinkat(dir, STAGE_FILE, 0) && errno != ENOENT)
		return io_error(error);
	int fd = openat(dir, STAGE_FILE,
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			0600);

	if (fd < 0)
		return io_error(error);
	size_t done = 0;

	while (done < len) {
		ssize_t n = write(fd, data + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (!n)
			errno = EIO;
		if (n <= 0)
			break;
		done += n;
	}
	gboolean ok = done == len && !fdatasync(fd);
	int saved = errno;

	if (close(fd) && ok) {
		ok = FALSE;
		saved = errno;
	}
	if (!ok) {
		unlinkat(dir, STAGE_FILE, 0);
		errno = saved;
		return io_error(error);
	}
	if (renameat(dir, STAGE_FILE, dir, slot)) {
		saved = errno;
		unlinkat(dir, STAGE_FILE, 0);
		errno = saved;
		return io_error(error);
	}
	return !fsync(dir) || io_error(error);
}

static gboolean write_slot(LineOffline *store, const char *slot, J *payload,
			   size_t limit, GError **error)
{
	if (!store || !payload)
		return invalid_data(error);
	g_autoptr(J) record = obj();
	size_t len;

	json_object_object_add(record, "slot", json_object_new_string(slot));
	json_object_object_add(record, "account",
			       json_object_new_string(store->account));
	json_object_object_add(record, "payload", json_object_get(payload));
	const char *text = json_object_to_json_string_length(
		record, JSON_C_TO_STRING_PLAIN, &len);

	if (!text)
		return invalid_data(error);
	g_autofree guint8 *cipher = NULL;
	size_t size = 0;

	if (len <= limit)
		cipher =
			line_state_encrypt(store->key, text, len, &size, error);
	explicit_bzero((char *)text, len);
	if (len > limit) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "The offline data exceeds its size limit.");
		return FALSE;
	}
	return cipher && flush_slot(store->dir, slot, cipher, size, error);
}

static char *history_slot(const char *chat)
{
	if (!valid_mid(chat))
		return NULL;
	g_autofree char *digest =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, chat, -1);

	return g_strconcat(CHAT_PREFIX, digest, NULL);
}

static gboolean digest_name(const char *name)
{
	return strlen(name) == 64 && strspn(name, "0123456789abcdef") == 64;
}

static gboolean history_name(const char *name)
{
	return g_str_has_prefix(name, CHAT_PREFIX) &&
	       digest_name(name + sizeof(CHAT_PREFIX) - 1);
}

static DIR *open_listing(int dir)
{
	int fd = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	DIR *listing = fdopendir(fd);

	if (!listing)
		close(fd);
	return listing;
}

/* When adding a new chat, evict the least recently updated archive. */
static gboolean archive_room(LineOffline *store, const char *slot)
{
	struct stat st;

	if (!fstatat(store->dir, slot, &st, AT_SYMLINK_NOFOLLOW))
		return S_ISREG(st.st_mode);
	if (errno != ENOENT)
		return FALSE;
	DIR *listing = open_listing(store->dir);

	if (!listing)
		return FALSE;
	char oldest[sizeof(CHAT_PREFIX) + 64] = "";
	struct timespec age = { 0 };
	guint count = 0;
	int err;

	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(listing);

		if (!entry)
			break;
		const char *name = entry->d_name;

		if (!history_name(name) ||
		    fstatat(store->dir, name, &st, AT_SYMLINK_NOFOLLOW) ||
		    !S_ISREG(st.st_mode))
			continue;
		count++;
		if (!oldest[0] || st.st_mtim.tv_sec < age.tv_sec ||
		    (st.st_mtim.tv_sec == age.tv_sec &&
		     st.st_mtim.tv_nsec < age.tv_nsec)) {
			memcpy(oldest, name, sizeof(oldest));
			age = st.st_mtim;
		}
	}
	err = errno;
	closedir(listing);
	return !err &&
	       (count < ARCHIVE_CHATS || !unlinkat(store->dir, oldest, 0));
}

static J *typed_slot(LineOffline *store, const char *slot, size_t limit,
		     enum json_type type)
{
	g_autoptr(J) value = read_slot(store, slot, limit, NULL, NULL);

	return json_object_is_type(value, type) ? g_steal_pointer(&value) :
						  NULL;
}

J *line_offline_load_profile(LineOffline *store)
{
	return typed_slot(store, PROFILE_FILE, PROFILE_LIMIT, json_type_object);
}

gboolean line_offline_save_profile(LineOffline *store, J *profile)
{
	return json_object_is_type(profile, json_type_object) &&
	       write_slot(store, PROFILE_FILE, profile, PROFILE_LIMIT, NULL);
}

J *line_offline_load_chats(LineOffline *store)
{
	return typed_slot(store, CHAT_LIST_FILE, DIRECTORY_LIMIT,
			  json_type_array);
}

gboolean line_offline_save_chats(LineOffline *store, J *rows)
{
	return json_object_is_type(rows, json_type_array) &&
	       write_slot(store, CHAT_LIST_FILE, rows, DIRECTORY_LIMIT, NULL);
}

static guint64 row_id(const char *id)
{
	guint64 value;

	return id && g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &value,
						NULL) ?
		       value :
		       0;
}

static int row_order(const void *a, const void *b)
{
	J *lhs = *(J *const *)a, *rhs = *(J *const *)b;
	gint64 lt = json_object_get_int64(field(lhs, "time"));
	gint64 rt = json_object_get_int64(field(rhs, "time"));

	if (lt != rt)
		return lt < rt ? -1 : 1;
	guint64 li = row_id(strfield(lhs, "id"));
	guint64 ri = row_id(strfield(rhs, "id"));

	return (li > ri) - (li < ri);
}

static J *unsent_stub(J *raw, const char *id)
{
	if (!raw)
		return NULL;
	J *stub = obj();

	json_object_object_add(stub, "4", json_object_new_string(id));
	json_object_object_add(stub, "5", json_object_get(field(raw, "5")));
	json_object_object_add(stub, "6", json_object_get(field(raw, "6")));
	json_object_object_add(stub, "unsent", json_object_new_boolean(TRUE));
	return stub;
}

static void merge_rows(J *merged, J *index, J *rows, J *raw)
{
	if (!json_object_is_type(rows, json_type_array))
		return;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		const char *id = strfield(row, "id");

		if (!row_id(id))
			continue;
		J *known = field(merged, id);
		J *message = field(raw, id);

		if (!g_strcmp0(strfield(known, "type"), "unsent"))
			continue;
		json_object_object_add(merged, id, json_object_get(row));
		if (!g_strcmp0(strfield(row, "type"), "unsent")) {
			J *stub = unsent_stub(
				message ? message : field(index, id), id);

			json_object_object_del(index, id);
			if (stub)
				json_object_object_add(index, id, stub);
		} else if (json_object_is_type(message, json_type_object)) {
			json_object_object_add(index, id,
					       json_object_get(message));
		}
	}
}

static void append_row(J *rows, J *index, J *row, J *raw)
{
	const char *id = strfield(row, "id");

	if (!id)
		return;
	J *source = field(raw, id);

	json_object_array_add(rows, json_object_get(row));
	if (source)
		json_object_object_add(index, id, json_object_get(source));
}

static size_t archive_start(J *rows, J *raw)
{
	/* Space for the outer JSON object and encrypted record identifier. */
	size_t total = 256;
	size_t end = json_object_array_length(rows);
	size_t start = end;

	while (start && end - start < ARCHIVE_ROWS) {
		J *row = json_object_array_get_idx(rows, start - 1);
		const char *id = strfield(row, "id");
		J *entry = field(raw, id);
		size_t row_size, raw_size = 0;

		json_object_to_json_string_length(row, JSON_C_TO_STRING_PLAIN,
						  &row_size);
		if (entry)
			json_object_to_json_string_length(
				entry, JSON_C_TO_STRING_PLAIN, &raw_size);
		total += row_size + raw_size + strlen(id) + 8;
		if (total > HISTORY_LIMIT)
			break;
		start--;
	}
	return start;
}

gboolean line_offline_save_history(LineOffline *store, const char *chat,
				   J *rows, J *raw)
{
	g_autofree char *slot = history_slot(chat);

	if (!store || !slot || !json_object_is_type(rows, json_type_array))
		return FALSE;
	g_autoptr(J) old =
		typed_slot(store, slot, HISTORY_LIMIT, json_type_object);
	g_autoptr(J) merged = obj();
	g_autoptr(J) index = obj();
	g_autoptr(J) ordered = json_object_new_array();

	merge_rows(merged, index, field(old, "rows"), field(old, "raw"));
	merge_rows(merged, index, rows, raw);
	json_object_object_foreach(merged, id, row) {
		(void)id;
		json_object_array_add(ordered, json_object_get(row));
	}
	json_object_array_sort(ordered, row_order);
	size_t start = archive_start(ordered, index);
	size_t end = json_object_array_length(ordered);

	if (end && start == end)
		return FALSE;
	g_autoptr(J) next = obj();
	J *next_rows = json_object_new_array(), *next_index = obj();

	json_object_object_add(next, "rows", next_rows);
	json_object_object_add(next, "raw", next_index);
	for (size_t i = start; i < end; i++)
		append_row(next_rows, next_index,
			   json_object_array_get_idx(ordered, i), index);
	if (old && json_object_equal(old, next))
		return TRUE;
	return archive_room(store, slot) &&
	       write_slot(store, slot, next, HISTORY_LIMIT, NULL);
}

J *line_offline_load_history(LineOffline *store, const char *chat,
			     const char *before, J **raw, gboolean *more)
{
	if (raw)
		*raw = NULL;
	if (more)
		*more = FALSE;
	g_autofree char *slot = history_slot(chat);

	g_autoptr(J) cached = slot ? typed_slot(store, slot, HISTORY_LIMIT,
						json_type_object) :
				     NULL;
	J *rows = field(cached, "rows"), *index = field(cached, "raw");

	if (!json_object_is_type(rows, json_type_array) ||
	    !json_object_is_type(index, json_type_object))
		return NULL;
	size_t end = json_object_array_length(rows);

	if (before) {
		while (end && g_strcmp0(strfield(json_object_array_get_idx(
							 rows, end - 1),
						 "id"),
					before))
			end--;
		if (!end)
			return NULL;
		end--;
	}
	size_t start = end > ARCHIVE_PAGE ? end - ARCHIVE_PAGE : 0;
	J *page = json_object_new_array(), *page_index = obj();

	for (size_t i = start; i < end; i++)
		append_row(page, page_index, json_object_array_get_idx(rows, i),
			   index);
	if (raw)
		*raw = page_index;
	else
		json_object_put(page_index);
	if (more)
		*more = start > 0;
	return page;
}

static void discard_slot(LineOffline *store, const char *slot)
{
	if (!unlinkat(store->dir, slot, 0))
		fsync(store->dir);
}

void line_offline_drop_history(LineOffline *store, const char *chat)
{
	g_autofree char *slot = history_slot(chat);

	if (store && slot)
		discard_slot(store, slot);
}

static void redact_history(LineOffline *store, const char *slot, const char *id,
			   gint64 time)
{
	g_autoptr(J) history =
		typed_slot(store, slot, HISTORY_LIMIT, json_type_object);
	J *rows = field(history, "rows");
	J *raw = field(history, "raw");

	if (!json_object_is_type(rows, json_type_array) ||
	    !json_object_is_type(raw, json_type_object))
		return;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);

		if (g_strcmp0(strfield(row, "id"), id))
			continue;
		gint64 created = json_object_get_int64(field(row, "time"));
		J *stub = obj();

		json_object_object_add(stub, "id", json_object_new_string(id));
		json_object_object_add(stub, "type",
				       json_object_new_string("unsent"));
		json_object_object_add(
			stub, "text", json_object_new_string("Message unsent"));
		json_object_object_add(stub, "time",
				       json_object_new_int64(created ? created :
								       time));
		json_object_array_put_idx(rows, i, stub);
		J *cursor = unsent_stub(field(raw, id), id);

		if (cursor)
			json_object_object_add(raw, id, cursor);
		if (!write_slot(store, slot, history, HISTORY_LIMIT, NULL))
			discard_slot(store, slot);
		return;
	}
}

void line_offline_unsend(LineOffline *store, const char *chat, const char *id,
			 gint64 time)
{
	g_autofree char *slot = history_slot(chat);

	if (!store || !slot || !row_id(id))
		return;
	redact_history(store, slot, id, time);
	g_autoptr(J) chats = line_offline_load_chats(store);

	if (!chats)
		return;
	for (size_t i = 0; i < json_object_array_length(chats); i++) {
		J *row = json_object_array_get_idx(chats, i);

		if (g_strcmp0(chat, strfield(row, "id")) ||
		    g_strcmp0(id, strfield(row, "lastId")))
			continue;
		json_object_object_del(row, "preview");
		json_object_object_add(row, "lastUnsent",
				       json_object_new_boolean(TRUE));
		if (!line_offline_save_chats(store, chats))
			discard_slot(store, CHAT_LIST_FILE);
		break;
	}
}

J *line_offline_load_outbox(LineOffline *store, GError **error)
{
	gboolean missing;

	if (!store) {
		invalid_data(error);
		return NULL;
	}
	g_autoptr(J) rows =
		read_slot(store, OUTBOX_FILE, OUTBOX_LIMIT, &missing, error);

	if (missing) {
		store->outbox_writable = TRUE;
		return json_object_new_array();
	}
	store->outbox_writable = json_object_is_type(rows, json_type_array);
	if (!store->outbox_writable) {
		if (rows)
			invalid_data(error);
		return NULL;
	}
	return g_steal_pointer(&rows);
}

gboolean line_offline_save_outbox(LineOffline *store, J *rows, GError **error)
{
	if (!store || !store->outbox_writable ||
	    !json_object_is_type(rows, json_type_array))
		return invalid_data(error);
	return write_slot(store, OUTBOX_FILE, rows, OUTBOX_LIMIT, error);
}

static gboolean clear_directory(int dir, GError **error)
{
	DIR *listing = open_listing(dir);

	if (!listing)
		return io_error(error);
	int failed = 0;

	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(listing);

		if (!entry) {
			if (!failed)
				failed = errno;
			break;
		}
		const char *name = entry->d_name;

		if (!history_name(name) && strcmp(name, PROFILE_FILE) &&
		    strcmp(name, CHAT_LIST_FILE) && strcmp(name, OUTBOX_FILE) &&
		    strcmp(name, STAGE_FILE))
			continue;
		if (unlinkat(dir, name, 0) && errno != ENOENT && !failed)
			failed = errno;
	}
	closedir(listing);
	if (fsync(dir) && !failed)
		failed = errno;
	if (failed) {
		errno = failed;
		return io_error(error);
	}
	return TRUE;
}

static gboolean clear_account(int parent, const char *name, GError **error)
{
	int dir = private_dir(parent, name, FALSE);

	if (dir < 0)
		return errno == ENOENT || io_error(error);
	gboolean ok = clear_directory(dir, error);

	close(dir);
	return ok && (!unlinkat(parent, name, AT_REMOVEDIR) ||
		      errno == ENOENT || io_error(error));
}

gboolean line_offline_clear_all(int state_dir, GError **error)
{
	int dir = private_dir(state_dir, "offline", FALSE);

	if (dir < 0)
		return errno == ENOENT || io_error(error);
	DIR *listing = open_listing(dir);

	if (!listing) {
		int saved = errno;

		close(dir);
		errno = saved;
		return io_error(error);
	}
	gboolean ok = TRUE;

	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(listing);

		if (!entry) {
			ok = !errno || io_error(error);
			break;
		}
		if (digest_name(entry->d_name) &&
		    !clear_account(dir, entry->d_name, error)) {
			ok = FALSE;
			break;
		}
	}
	closedir(listing);
	if (ok && fsync(dir))
		ok = io_error(error);
	close(dir);
	return ok;
}
