// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_keyring.h"
#include "security/ipc.h"
#include "state.h"
#include "state_crypto.h"
#include <errno.h>
#include <fcntl.h>
#include <libsecret/secret.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEYRING_TIMEOUT 6000

enum {
	KEY_ENCODED_SIZE = 4 * ((LINE_KEYRING_KEY_BYTES + 2) / 3)
};

G_STATIC_ASSERT((int)LINE_KEYRING_KEY_BYTES == LINE_STATE_KEY_SIZE);

static const SecretSchema key_schema = {
	.name = "io.github.novesce.Avernella.SessionWrappingKey",
	.attributes = { { "profile", SECRET_SCHEMA_ATTRIBUTE_STRING },
			{ NULL, 0 } },
};

/* Serializes first use without creating the app state before migration. */
static int lock_profile(int dir)
{
	struct stat st;

	if (fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 022))
		return -1;
	int lock = openat(
		dir, ".avernella-keyring.lock",
		O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_CREAT | O_RDWR, 0600);

	if (lock < 0)
		return -1;
	if (fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
	    st.st_uid != geteuid() || (st.st_mode & 077) ||
	    TEMP_FAILURE_RETRY(flock(lock, LOCK_EX))) {
		close(lock);
		return -1;
	}
	return lock;
}

static int lock_runtime(void)
{
	const char *runtime = g_getenv("XDG_RUNTIME_DIR");
	struct stat st;

	if (!runtime || runtime[0] != '/')
		return -1;
	int root = open(runtime, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

	if (root < 0)
		return -1;
	int dir = -1;

	if (!fstat(root, &st) && st.st_uid == geteuid() &&
	    !(st.st_mode & 077)) {
		dir = openat(root, LINE_STATE_DIR,
			     O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (dir < 0 && errno == ENOENT &&
		    (!mkdirat(root, LINE_STATE_DIR, 0700) || errno == EEXIST))
			dir = openat(root, LINE_STATE_DIR,
				     O_PATH | O_DIRECTORY | O_NOFOLLOW |
					     O_CLOEXEC);
	}
	close(root);
	if (dir < 0)
		return -1;

	int lock = -1;

	if (!fstat(dir, &st) && !(st.st_mode & 077))
		lock = lock_profile(dir);
	close(dir);
	return lock;
}

static gboolean may_create_key(int dir)
{
	int app = openat(dir, LINE_STATE_DIR,
			 O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_RDONLY);

	if (app < 0)
		return errno == ENOENT;
	int session = openat(app, LINE_STATE_SESSION_FILE,
			     O_NOFOLLOW | O_CLOEXEC | O_RDONLY | O_NONBLOCK);
	int open_errno = errno;

	close(app);
	if (session < 0)
		return open_errno == ENOENT;
	guint8 prefix[LINE_STATE_CRYPTO_OVERHEAD];
	struct stat st;
	ssize_t len = -1;

	if (!fstat(session, &st) && S_ISREG(st.st_mode) &&
	    st.st_uid == geteuid() && !(st.st_mode & 077))
		len = TEMP_FAILURE_RETRY(read(session, prefix, sizeof(prefix)));
	close(session);

	/* Preserve an existing sealed or unrecognizable session on key loss. */
	gboolean legacy = FALSE;

	if (len > 0 && !line_state_is_encrypted(prefix, len)) {
		for (ssize_t i = 0; i < len; ++i) {
			if (g_ascii_isspace(prefix[i]))
				continue;
			legacy = prefix[i] == '{';
			break;
		}
	}
	explicit_bzero(prefix, sizeof(prefix));
	return legacy;
}

static gboolean read_value(SecretValue *value, guint8 *key)
{
	gsize len;
	const char *bytes = value ? secret_value_get(value, &len) : NULL;

	if (!bytes || len != KEY_ENCODED_SIZE ||
	    g_strcmp0(secret_value_get_content_type(value), "text/plain"))
		return FALSE;
	guint8 decoded[3 * KEY_ENCODED_SIZE / 4];
	guint8 encoded[KEY_ENCODED_SIZE + 1];
	gboolean ok =
		EVP_DecodeBlock(decoded, (const guint8 *)bytes, len) ==
			sizeof(decoded) &&
		EVP_EncodeBlock(encoded, decoded, LINE_KEYRING_KEY_BYTES) ==
			KEY_ENCODED_SIZE &&
		!CRYPTO_memcmp(bytes, encoded, len);

	if (ok)
		memcpy(key, decoded, LINE_KEYRING_KEY_BYTES);
	explicit_bzero(decoded, sizeof(decoded));
	explicit_bzero(encoded, sizeof(encoded));
	return ok;
}

static SecretValue *new_value(guint8 *key)
{
	guint8 encoded[KEY_ENCODED_SIZE + 1];

	if (RAND_priv_bytes(key, LINE_KEYRING_KEY_BYTES) != 1)
		return NULL;
	int len = EVP_EncodeBlock(encoded, key, LINE_KEYRING_KEY_BYTES);
	SecretValue *value =
		secret_value_new((const char *)encoded, len, "text/plain");

	explicit_bzero(encoded, sizeof(encoded));
	return value;
}

static gboolean read_item(SecretItem *item, guint8 *key)
{
	if (secret_item_get_locked(item) ||
	    !secret_item_load_secret_sync(item, NULL, NULL))
		return FALSE;
	g_autoptr(SecretValue) value = secret_item_get_secret(item);

	return read_value(value, key);
}

static gboolean create_item(SecretService *service, GHashTable *attributes,
			    guint8 *key)
{
	g_autoptr(SecretCollection) collection =
		secret_collection_for_alias_sync(service,
						 SECRET_COLLECTION_DEFAULT,
						 SECRET_COLLECTION_NONE, NULL,
						 NULL);

	if (!collection || secret_collection_get_locked(collection))
		return FALSE;
	g_autoptr(SecretValue) value = new_value(key);

	if (!value)
		return FALSE;
	g_autoptr(SecretItem) item = secret_item_create_sync(
		collection, &key_schema, attributes, "Avernella session key",
		value, SECRET_ITEM_CREATE_NONE, NULL, NULL);

	guint8 stored[LINE_KEYRING_KEY_BYTES];
	gboolean ok = item && read_item(item, stored) &&
		      !CRYPTO_memcmp(key, stored, sizeof(stored));

	explicit_bzero(stored, sizeof(stored));
	return ok;
}

static gboolean service_key(int dir, GHashTable *attributes, guint8 *key)
{
	g_autoptr(SecretService) service =
		secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);

	if (!service)
		return FALSE;
	g_autoptr(GError) error = NULL;
	GList *matches =
		secret_service_search_sync(service, &key_schema, attributes,
					   SECRET_SEARCH_ALL, NULL, &error);
	gboolean ok = FALSE;

	/* Locked, malformed or ambiguous items must never be replaced. */
	if (!error && matches && !matches->next)
		ok = read_item(matches->data, key);
	else if (!error && !matches && may_create_key(dir))
		ok = create_item(service, attributes, key);
	g_list_free_full(matches, g_object_unref);
	return ok;
}

static gboolean scope_portal_store(void)
{
	const char *base = g_getenv("XDG_DATA_HOME");

	if (!base || !g_path_is_absolute(base))
		return FALSE;
	g_autofree char *path = g_build_filename(base, LINE_STATE_DIR, NULL);

	/*
	 * libsecret stores the portal-encrypted keyring under XDG_DATA_HOME.
	 * Keep it within the app directory allowed by Landlock. Only this
	 * short-lived helper uses the modified directory.
	 */
	if (!g_setenv("XDG_DATA_HOME", path, TRUE) ||
	    g_strcmp0(g_get_user_data_dir(), path))
		return FALSE;
	return !g_mkdir_with_parents(path, 0700);
}

static gboolean portal_key(int dir, GHashTable *attributes, guint8 *key)
{
	/* Force the scoped file backend even if a caller granted bus access. */
	if (!scope_portal_store() || !g_setenv("SECRET_BACKEND", "file", TRUE))
		return FALSE;
	g_unsetenv("SECRET_FILE_TEST_PASSWORD");
	g_unsetenv("SECRET_FILE_TEST_PATH");

	g_autoptr(GError) error = NULL;
	g_autoptr(SecretValue) found = secret_password_lookupv_binary_sync(
		&key_schema, attributes, NULL, &error);

	if (error)
		return FALSE;
	if (found)
		return read_value(found, key);
	if (!may_create_key(dir))
		return FALSE;
	g_autoptr(SecretValue) value = new_value(key);

	if (!value)
		return FALSE;

	if (!secret_password_storev_binary_sync(
		    &key_schema, attributes, SECRET_COLLECTION_DEFAULT,
		    "Avernella session key", value, NULL, NULL))
		return FALSE;
	found = secret_password_lookupv_binary_sync(&key_schema, attributes,
						    NULL, &error);
	guint8 stored[LINE_KEYRING_KEY_BYTES];
	gboolean ok = !error && read_value(found, stored) &&
		      !CRYPTO_memcmp(key, stored, sizeof(stored));

	explicit_bzero(stored, sizeof(stored));
	return ok;
}

static gboolean get_key(guint8 *key)
{
	const char *base = g_get_user_state_dir();

	if (g_mkdir_with_parents(base, 0700))
		return FALSE;
	g_autofree char *path = realpath(base, NULL);

	if (!path)
		return FALSE;
	int dir = open(path, O_DIRECTORY | O_PATH | O_NOFOLLOW | O_CLOEXEC);

	if (dir < 0)
		return FALSE;
	int lock = lock_runtime();

	if (lock < 0)
		lock = lock_profile(dir);
	if (lock < 0) {
		close(dir);
		return FALSE;
	}
	g_autofree char *profile =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, path, -1);
	g_autoptr(GHashTable) attributes =
		secret_attributes_build(&key_schema, "profile", profile, NULL);
	gboolean flatpak = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
	gboolean ok = flatpak ? portal_key(dir, attributes, key) :
				service_key(dir, attributes, key);

	close(lock);
	close(dir);
	return ok;
}

static void keyring_child(int fd, pid_t parent)
{
	int socket_fd = STDERR_FILENO + 1;
	const struct rlimit no_core = { 0, 0 };

	if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent ||
	    prctl(PR_SET_DUMPABLE, 0) ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
	    setrlimit(RLIMIT_CORE, &no_core))
		_exit(1);
	if (fd != socket_fd && dup3(fd, socket_fd, O_CLOEXEC) < 0)
		_exit(1);
	if (avernella_close_extra_fds(socket_fd) < 0)
		_exit(1);

	/* Never let service diagnostics reach the UI protocol or the log. */
	int sink = open("/dev/null", O_RDWR | O_CLOEXEC);

	if (sink < 0)
		_exit(1);
	for (int stream = STDIN_FILENO; stream <= STDERR_FILENO; stream++)
		if (dup2(sink, stream) < 0)
			_exit(1);
	if (sink > STDERR_FILENO)
		close(sink);
	guint8 key[LINE_KEYRING_KEY_BYTES];
	gboolean ok = g_setenv("GIO_USE_VFS", "local", TRUE) && get_key(key) &&
		      TEMP_FAILURE_RETRY(send(socket_fd, key, sizeof(key),
					      MSG_NOSIGNAL)) == sizeof(key);

	explicit_bzero(key, sizeof(key));
	_exit(ok ? 0 : 1);
}

static gboolean receive_key(int fd, guint8 *key)
{
	struct pollfd ready = { .fd = fd, .events = POLLIN };
	gint64 deadline = g_get_monotonic_time() + KEYRING_TIMEOUT * 1000LL;

	for (;;) {
		gint64 left = deadline - g_get_monotonic_time();

		if (left <= 0)
			return FALSE;
		int rc = poll(&ready, 1, (left + 999) / 1000);

		if (rc < 0 && errno == EINTR)
			continue;
		return rc > 0 && (ready.revents & POLLIN) &&
		       TEMP_FAILURE_RETRY(recv(fd, key, LINE_KEYRING_KEY_BYTES,
					       MSG_DONTWAIT | MSG_TRUNC)) ==
			       LINE_KEYRING_KEY_BYTES;
	}
}

gboolean line_keyring_key(guint8 key[LINE_KEYRING_KEY_BYTES])
{
	int pair[2];

	explicit_bzero(key, LINE_KEYRING_KEY_BYTES);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
		return FALSE;
	pid_t parent = getpid();
	pid_t child = fork();

	if (!child) {
		close(pair[0]);
		keyring_child(pair[1], parent);
	}
	close(pair[1]);
	gboolean ok = child > 0 && receive_key(pair[0], key);

	close(pair[0]);
	if (child > 0) {
		int status = 0;

		if (!ok)
			kill(child, SIGKILL);
		ok = TEMP_FAILURE_RETRY(waitpid(child, &status, 0)) == child &&
		     WIFEXITED(status) && !WEXITSTATUS(status) && ok;
	}
	if (!ok)
		explicit_bzero(key, LINE_KEYRING_KEY_BYTES);
	return ok;
}
