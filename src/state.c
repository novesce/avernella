// SPDX-License-Identifier: GPL-3.0-or-later
#include "state.h"
#include "state_crypto.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

#define SESSION LINE_STATE_SESSION_FILE
#define TEMP "session.new"
/*
 * Earlier names of this client's state directory, newest first: kitatide,
 * and line-linux before it. Their session and chat-preference files have
 * the same format as Avernella's.
 */
static const char *const legacy_dirs[] = { "kitatide", "line-linux" };
#define STORE_DIR_FLAGS (O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_RDONLY)
#define UNSENT_FILE "unsent.json"
#define MAX_UNSENT_SIZE ((size_t)512 * 1024)
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif

/*
 * The worker initializes this before forking the Vault broker. Neither
 * the UI nor the Vault talks to the keyring. The volatile session uses
 * a separate, per-worker key and an anonymous file descriptor.
 */
static guint8 store_key[LINE_STATE_KEY_SIZE];
static gboolean key_store_started, keyring_available, legacy_plain;
static int volatile_session = -1;

gboolean line_store_start(const guint8 *key)
{
	keyring_available = key != NULL;
	if (key)
		memcpy(store_key, key, sizeof(store_key));
	else if (RAND_priv_bytes(store_key, sizeof(store_key)) != 1)
		return FALSE;
	key_store_started = TRUE;
	return TRUE;
}

void line_store_stop(void)
{
	if (volatile_session >= 0)
		close(volatile_session);
	volatile_session = -1;
	explicit_bzero(store_key, sizeof(store_key));
	key_store_started = keyring_available = legacy_plain = FALSE;
}

gboolean line_store_temporary(void)
{
	return volatile_session >= 0;
}

int line_store_session_fd(int dir)
{
	return line_store_temporary() ? volatile_session : dir;
}

gboolean line_store_is_memory_file(int fd, const struct stat *st)
{
	if (!S_ISREG(st->st_mode) || st->st_nlink || st->st_uid != geteuid() ||
	    (st->st_mode & 077))
		return FALSE;
	int seals = fcntl(fd, F_GET_SEALS);

	return seals >= 0 && (seals & ~F_SEAL_EXEC) == F_SEAL_SEAL;
}

J *line_store_read_unsent(int dir, const char *mid)
{
	int fd = openat(dir, UNSENT_FILE,
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;

	g_autofree char *text = NULL;

	g_autoptr(J) record = NULL;
	size_t used = 0;
	J *chats;

	if (fd < 0)
		return obj();
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
	    (st.st_mode & 077) || st.st_size < 2 ||
	    (guint64)st.st_size > MAX_UNSENT_SIZE)
		goto done;

	text = g_malloc(st.st_size + 1);
	while (used < (size_t)st.st_size) {
		ssize_t n = read(fd, text + used, st.st_size - used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto done;
		used += n;
	}
	text[used] = 0;
	record = json_tokener_parse(text);
done:
	close(fd);
	chats = field(record, "chats");
	return !g_strcmp0(strfield(record, "account"), mid) &&
			       json_object_is_type(chats, json_type_object) ?
		       json_object_get(chats) :
		       obj();
}

void line_store_write_unsent(const char *mid, J *rows)
{
	g_autoptr(J) record = obj();
	g_autofree char *path = NULL;
	size_t size;

	if (!mid || !rows)
		return;
	json_object_object_add(record, "account", json_object_new_string(mid));
	json_object_object_add(record, "chats", json_object_get(rows));
	const char *text = json_object_to_json_string_length(
		record, JSON_C_TO_STRING_PLAIN, &size);

	if (size > MAX_UNSENT_SIZE)
		return;
	path = g_build_filename(g_get_user_state_dir(), LINE_STATE_DIR,
				UNSENT_FILE, NULL);
	g_file_set_contents_full(path, text, size,
				 G_FILE_SET_CONTENTS_CONSISTENT |
					 G_FILE_SET_CONTENTS_DURABLE,
				 0600, NULL);
}

static gboolean io_failure(const char *action, GError **error)
{
	g_set_error(error, LINE_ERROR, 1,
		    "Could not %s the local LINE session (%s).", action,
		    g_strerror(errno));
	return FALSE;
}

void line_session_clear(Line *s)
{
	line_secret_free(s->access);
	line_secret_free(s->refresh);
	line_secret_free(s->obs_token);
	g_clear_pointer(&s->emoji_packs, g_ptr_array_unref);
	g_free(s->mid);
	json_object_put(s->keys);
	json_object_put(s->pictures);
	g_clear_pointer(&s->profiles, g_hash_table_unref);
	g_clear_pointer(&s->history_secrets, g_array_unref);
	*s = (Line){ .service_code = -1 };
}

static gboolean load_session(int dir, Line *s, gboolean *plain, GError **error)
{
	g_autoptr(J) session = NULL;
	struct stat st;
	gboolean in_memory = !fstat(dir, &st) &&
			     line_store_is_memory_file(dir, &st);
	int fd = in_memory ?
			 dir :
			 openat(dir, SESSION,
				O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return errno == ENOENT || io_failure("open", error);
	if (in_memory && !st.st_size)
		return TRUE;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 077) || st.st_size <= 0 ||
	    (guint64)st.st_size >
		    LINE_STATE_MAX_BYTES + LINE_STATE_CRYPTO_OVERHEAD) {
		if (!in_memory)
			close(fd);
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"The local session file has invalid permissions or size.");
		return FALSE;
	}
	g_autofree char *data = g_malloc(st.st_size + 1);
	size_t used = 0;

	while (used < (size_t)st.st_size) {
		ssize_t n = pread(fd, data + used, st.st_size - used, used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += n;
	}
	if (!in_memory)
		close(fd);
	gboolean encrypted = line_state_is_encrypted(data, used);

	data[used] = 0;
	if (used == (size_t)st.st_size && encrypted) {
		g_autofree char *text = NULL;

		gsize length = 0;

		if (!(in_memory ? key_store_started : keyring_available))
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"Unlock your desktop keyring to resume the saved LINE session.");
		else
			text = line_state_decrypt(store_key, data, used,
						  &length, error);
		explicit_bzero(data, used);
		if (!text)
			return FALSE;
		session = json_tokener_parse(text);
		explicit_bzero(text, length);
	} else {
		if (!in_memory && used == (size_t)st.st_size &&
		    used <= LINE_STATE_MAX_BYTES)
			session = json_tokener_parse(data);
		explicit_bzero(data, used);
	}
	const char *token = strfield(session, "access");
	J *keys = field(session, "keys"), *seq = field(session, "seq");

	if (used != (size_t)st.st_size || !token || !*token ||
	    !json_object_is_type(keys, json_type_object) ||
	    !json_object_is_type(seq, json_type_int) ||
	    json_object_get_int64(seq) < 0 ||
	    json_object_get_int64(seq) > G_MAXINT32) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "The saved session could not be read.");
		return FALSE;
	}
	s->access = g_strdup(token);
	s->refresh = g_strdup(strfield(session, "refresh"));
	s->mid = g_strdup(strfield(session, "mid"));
	/* The parsed copies go with session; the keys stay in use. */
	line_secret_json_wipe(field(session, "access"));
	line_secret_json_wipe(field(session, "refresh"));
	J *refresh_at = field(session, "refreshAt");

	if (json_object_is_type(refresh_at, json_type_int))
		s->refresh_at = MAX(0, json_object_get_int64(refresh_at));
	s->seq = json_object_get_int64(seq);
	s->seq_reserved = s->seq;
	s->keys = json_object_get(keys);
	*plain = !encrypted;
	if (*plain && key_store_started && !keyring_available)
		legacy_plain = TRUE;
	return TRUE;
}

gboolean line_store_load(int dir, Line *s, GError **error)
{
	gboolean plain = FALSE;

	return load_session(dir, s, &plain, error);
}

static gboolean has_session(const char *base, const char *name)
{
	g_autofree char *session = g_build_filename(base, name, SESSION, NULL);

	return g_file_test(session, G_FILE_TEST_IS_REGULAR);
}

/*
 * An existing Avernella directory decides on its own, even when signed out.
 * Only without one does an earlier client's saved session count.
 */
gboolean line_store_has_session(void)
{
	const char *base = g_get_user_state_dir();
	g_autofree char *current = g_build_filename(base, LINE_STATE_DIR, NULL);
	GFileTest exists = G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK;

	if (g_file_test(current, exists))
		return has_session(base, LINE_STATE_DIR);
	for (gsize i = 0; i < G_N_ELEMENTS(legacy_dirs); i++)
		if (has_session(base, legacy_dirs[i]))
			return TRUE;
	return FALSE;
}

static gboolean lock_store(int dir, const char *name, GError **error)
{
	struct stat st;

	if (fstat(dir, &st))
		return io_failure("inspect", error);
	if (st.st_uid != geteuid() || (st.st_mode & 077)) {
		g_autofree char *path =
			g_build_filename(g_get_user_state_dir(), name, NULL);

		g_set_error(
			error, LINE_ERROR, 1,
			"The session directory must be private to your user (0700): %s",
			path);
		return FALSE;
	}
	if (!flock(dir, LOCK_EX | LOCK_NB))
		return TRUE;
	if (errno != EWOULDBLOCK)
		return io_failure("lock", error);
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Another Avernella window is using this session.");
	return FALSE;
}

/*
 * Runs only when Avernella has no state directory yet. Moves the newest
 * earlier client's directory into place as a unit, session and chat
 * preferences together. A directory whose session this version cannot
 * read is left where it is, so it never stands in the way of signing in.
 */
static gboolean migrate_store(int root, GError **error)
{
	for (gsize i = 0; i < G_N_ELEMENTS(legacy_dirs); i++) {
		const char *name = legacy_dirs[i];
		int dir = openat(root, name, STORE_DIR_FLAGS);

		if (dir < 0) {
			if (errno == ENOENT)
				continue;
			return io_failure("open", error);
		}
		if (!lock_store(dir, name, error)) {
			close(dir);
			return FALSE;
		}
		g_autoptr(GError) unreadable = NULL;
		Line probe = { .service_code = -1 };
		gboolean usable = line_store_load(dir, &probe, &unreadable);

		line_session_clear(&probe);
		if (!usable) {
			close(dir);
			continue;
		}
		gboolean ok;

		if (renameat2(root, name, root, LINE_STATE_DIR,
			      RENAME_NOREPLACE) == 0)
			ok = fsync(root) == 0 || io_failure("sync", error);
		else
			ok = errno == EEXIST || errno == ENOENT ||
			     io_failure("move", error);
		close(dir);
		return ok;
	}
	return TRUE;
}

/* A crashed save can leave a second copy of the session keys. */
static gboolean remove_incomplete(int dir, GError **error)
{
	if (unlinkat(dir, TEMP, 0) < 0)
		return errno == ENOENT ||
		       io_failure("remove the incomplete update to", error);
	return fsync(dir) == 0 || io_failure("sync", error);
}

int line_store_open(Line *s, GError **error)
{
	const char *base = g_get_user_state_dir();
	g_autofree char *direct = g_build_filename(base, LINE_STATE_DIR, NULL);
	int dir = open(direct, STORE_DIR_FLAGS);

	if (dir < 0 && errno == ENOENT) {
		int root = -1;

		if (!g_mkdir_with_parents(base, 0700))
			root = open(base, O_DIRECTORY | O_CLOEXEC | O_RDONLY);
		if (root < 0) {
			io_failure("open", error);
			return -1;
		}
		dir = openat(root, LINE_STATE_DIR, STORE_DIR_FLAGS);
		if (dir < 0 && errno == ENOENT) {
			if (!migrate_store(root, error)) {
				close(root);
				return -1;
			}
			if (!mkdirat(root, LINE_STATE_DIR, 0700) ||
			    errno == EEXIST)
				dir = openat(root, LINE_STATE_DIR,
					     STORE_DIR_FLAGS);
		}
		if (dir < 0)
			io_failure("open", error);
		close(root);
	} else if (dir < 0) {
		io_failure("open", error);
	}
	if (dir < 0)
		return -1;
	gboolean plain = FALSE;

	legacy_plain = FALSE;
	if (lock_store(dir, LINE_STATE_DIR, error) &&
	    remove_incomplete(dir, error) &&
	    load_session(dir, s, &plain, error)) {
		if (!plain || !keyring_available ||
		    line_store_save(dir, s, error))
			return dir;
		line_session_clear(s);
	}
	close(dir);
	return -1;
}

static gboolean store_record(int dir, Line *s, J *record, GError **error);

gboolean line_store_save(int dir, Line *s, GError **error)
{
	g_autoptr(J) record = obj();
	J *access = json_object_new_string(s->access);
	J *refresh = s->refresh ? json_object_new_string(s->refresh) : NULL;

	json_object_object_add(record, "access", access);
	if (refresh)
		json_object_object_add(record, "refresh", refresh);
	if (s->mid)
		json_object_object_add(record, "mid",
				       json_object_new_string(s->mid));
	if (s->refresh_at > 0)
		json_object_object_add(record, "refreshAt",
				       json_object_new_int64(s->refresh_at));
	json_object_object_add(record, "keys", json_object_get(s->keys));
	json_object_object_add(
		record, "seq",
		json_object_new_int64(MAX(s->seq, s->seq_reserved)));
	gboolean saved = store_record(dir, s, record, error);

	/*
	 * The token copies belong to record alone. Its key objects are shared
	 * with s->keys, and json-c frees those without wiping them.
	 */
	line_secret_json_wipe(access);
	line_secret_json_wipe(refresh);
	return saved;
}

/* Wipes the session text json-c printed into record's own buffer. */
static void wipe_text(const char *text, size_t size)
{
	if (text)
		explicit_bzero((char *)text, size);
}

static gboolean write_session(int fd, const char *data, size_t size,
			      GError **error)
{
	size_t done = 0;

	while (done < size) {
		ssize_t n = pwrite(fd, data + done, size - done, done);

		if (n < 0 && errno == EINTR)
			continue;
		if (!n)
			errno = EIO;
		if (n <= 0)
			return io_failure("save", error);
		done += n;
	}
	return TRUE;
}

static int session_memory(GError **error)
{
	if (volatile_session >= 0)
		return volatile_session;
	int fd =
		memfd_create("avernella-temporary-session",
			     MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);

	if (fd < 0 && errno == EINVAL)
		fd = memfd_create("avernella-temporary-session",
				  MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd >= 0 && !fchmod(fd, 0600) &&
	    fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0) {
		volatile_session = fd;
		return fd;
	}
	int saved = errno;

	if (fd >= 0)
		close(fd);
	errno = saved;
	io_failure("create a temporary store for", error);
	return -1;
}

static gboolean store_record(int dir, Line *s, J *record, GError **error)
{
	size_t left;
	const char *p = json_object_to_json_string_length(
		record, JSON_C_TO_STRING_PLAIN, &left);
	const char *text = p;
	size_t size = left;

	if (left > LINE_STATE_MAX_BYTES) {
		/*
		 * Peer and group keys can be retrieved again by ID. Keep the device's
		 * own approved generations and tokens, which cannot be reconstructed.
		 */
		json_object_object_add(s->keys, "peers", obj());
		json_object_object_del(s->keys, "groups");
		p = json_object_to_json_string_length(
			record, JSON_C_TO_STRING_PLAIN, &left);
		/* A shorter text reuses the buffer: wipe the longer tail. */
		if (p == text && size > left)
			wipe_text(p + left, size - left);
		text = p;
		size = left;
		if (left > LINE_STATE_MAX_BYTES) {
			wipe_text(text, size);
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"The LINE identity exceeds the local session size limit.");
			return FALSE;
		}
	}
	struct stat st;
	gboolean in_memory = !fstat(dir, &st) &&
			     line_store_is_memory_file(dir, &st);

	if (!in_memory && key_store_started && !keyring_available &&
	    !legacy_plain) {
		dir = session_memory(error);
		if (dir < 0) {
			wipe_text(text, size);
			return FALSE;
		}
		in_memory = TRUE;
	}
	g_autofree guint8 *cipher = NULL;

	if (in_memory || keyring_available) {
		cipher =
			line_state_encrypt(store_key, text, size, &left, error);
		wipe_text(text, size);
		text = NULL;
		if (!cipher)
			return FALSE;
		p = (const char *)cipher;
	}
	gboolean ok;

	if (in_memory) {
		ok = write_session(dir, p, left, error) &&
		     (!ftruncate(dir, left) || io_failure("save", error));
	} else {
		/* Supersede an incomplete update under the directory lock. */
		if (unlinkat(dir, TEMP, 0) < 0 && errno != ENOENT) {
			wipe_text(text, size);
			return io_failure("prepare", error);
		}
		int fd = openat(dir, TEMP,
				O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
					O_CLOEXEC,
				0600);

		if (fd < 0) {
			wipe_text(text, size);
			return io_failure("save", error);
		}
		ok = write_session(fd, p, left, error) &&
		     (!fdatasync(fd) || io_failure("save", error));
		if (close(fd) && ok)
			ok = io_failure("save", error);
		if (ok && renameat(dir, TEMP, dir, SESSION))
			ok = io_failure("save", error);
		if (!ok)
			unlinkat(dir, TEMP, 0);
		else if (fsync(dir))
			ok = io_failure("save", error);
	}
	wipe_text(text, size);
	if (!ok)
		return FALSE;
	s->token_changed = FALSE;
	return TRUE;
}

gint32 line_store_sequence(int dir, Line *s, GError **error)
{
	enum {
		SEQUENCE_BATCH = 32
	};

	if (s->seq >= G_MAXINT32) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Pair a new session to continue sending.");
		return 0;
	}
	if (s->seq >= s->seq_reserved) {
		gint64 previous = s->seq_reserved;

		/* On restart the entire persisted range is treated as used. */
		s->seq_reserved = MIN(s->seq + SEQUENCE_BATCH, G_MAXINT32);
		if (!line_store_save(dir, s, error)) {
			s->seq_reserved = previous;
			return 0;
		}
	}
	return ++s->seq;
}

gboolean line_store_forget(int dir, GError **error)
{
	int failure = 0;
	struct stat st;
	int fd = line_store_session_fd(dir);

	legacy_plain = FALSE;
	if (!fstat(fd, &st) && line_store_is_memory_file(fd, &st))
		return !ftruncate(fd, 0) || io_failure("remove", error);
	if (unlinkat(dir, SESSION, 0) < 0 && errno != ENOENT)
		failure = errno;
	if (unlinkat(dir, TEMP, 0) < 0 && errno != ENOENT && !failure)
		failure = errno;
	int sync_error = fsync(dir);

	if (failure) {
		errno = failure;
		return io_failure("remove", error);
	}
	return !sync_error || io_failure("sync", error);
}

gboolean line_store_discard(GError **error)
{
	g_autofree char *path =
		g_build_filename(g_get_user_state_dir(), LINE_STATE_DIR, NULL);
	int dir = open(path, STORE_DIR_FLAGS);

	if (dir < 0)
		return errno == ENOENT || io_failure("open", error);
	gboolean ok = lock_store(dir, LINE_STATE_DIR, error) &&
		      line_store_forget(dir, error);

	close(dir);
	return ok;
}
