// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat_lists.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define USER "u11111111111111111111111111111111"
#define OTHER "u22222222222222222222222222222222"
#define CHAT "c33333333333333333333333333333333"
#define PEER "u44444444444444444444444444444444"
static char *state_dir;

static char *settings_file(const char *user)
{
	g_autofree char *hash =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, user, -1);
	g_autofree char *name = g_strdup_printf("lists-%s.json", hash);

	return g_build_filename(state_dir, "avernella", name, NULL);
}

static void round_trip(void)
{
	g_autoptr(GError) e = NULL;
	struct chat_lists *s = chat_lists_open(USER, &e);

	g_assert_no_error(e);
	g_assert_nonnull(s);
	g_assert_true(chat_notifications(s));
	g_assert_cmpstr(chat_binding(s, LIST_PIN), ==, "Ctrl+Shift+P");
	g_assert_true(chat_folder_set(s, NULL, " ทริป 🌳 ", CHAT, &e));
	g_assert_true(chat_folder_contains(s, "ทริป 🌳", CHAT));
	g_assert_true(chat_folder_toggle(s, "ทริป 🌳", PEER, &e));
	g_assert_true(chat_view_set(s, "folder:ทริป 🌳", &e));
	g_assert_true(chat_pin_toggle(s, PEER, &e));
	g_assert_true(chat_notifications_set(s, FALSE, &e));
	g_assert_true(chat_binding_set(s, LIST_PIN, "F6", &e));
	g_assert_no_error(e);
	g_autofree char *file = settings_file(USER);
	struct stat st;

	g_assert_cmpint(stat(file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	chat_lists_free(s);
	s = chat_lists_open(OTHER, &e);
	g_assert_no_error(e);
	g_assert_false(chat_pinned(s, PEER));
	g_assert_null(chat_folders(s));
	g_assert_true(chat_notifications(s));
	g_assert_cmpstr(chat_current_view(s), ==, "all");
	chat_lists_free(s);
	s = chat_lists_open(USER, &e);
	g_assert_no_error(e);
	g_assert_true(chat_pinned(s, PEER));
	g_assert_false(chat_notifications(s));
	g_assert_cmpstr(chat_binding(s, LIST_PIN), ==, "F6");
	g_assert_true(chat_folder_set(s, "ทริป 🌳", "Clubs", NULL, &e));
	g_assert_cmpstr(chat_current_view(s), ==, "folder:Clubs");
	g_assert_true(chat_folder_contains(s, "Clubs", CHAT));
	g_assert_true(chat_folder_contains(s, "Clubs", PEER));
	g_assert_true(chat_folder_toggle(s, "Clubs", CHAT, &e));
	g_assert_false(chat_folder_contains(s, "Clubs", CHAT));
	g_assert_true(chat_folder_set(s, "Clubs", NULL, NULL, &e));
	g_assert_cmpstr(chat_current_view(s), ==, "all");
	g_assert_true(chat_pinned(s, PEER));
	g_assert_true(chat_pin_toggle(s, PEER, &e));
	g_assert_false(chat_pinned(s, PEER));
	chat_lists_free(s);
	g_remove(file);
}

static void rejected(void)
{
	g_autoptr(GError) e = NULL;
	struct chat_lists *s = chat_lists_open(USER, &e);

	g_assert_no_error(e);
	g_assert_true(chat_folder_set(s, NULL, "Club", NULL, &e));
	g_assert_false(chat_folder_set(s, NULL, "cLuB", NULL, &e));
	g_clear_error(&e);
	g_assert_false(chat_folder_set(s, NULL, "\t\n ", NULL, &e));
	g_clear_error(&e);
	g_assert_false(chat_pin_toggle(s, "../other", &e));
	g_clear_error(&e);
	g_assert_false(chat_folder_toggle(s, "unknown", PEER, &e));
	g_clear_error(&e);
	g_assert_false(chat_binding_set(s, LIST_PIN, "Ctrl+R", &e));
	g_clear_error(&e);
	g_assert_cmpstr(chat_binding(s, LIST_PIN), ==, "Ctrl+Shift+P");
	chat_lists_free(s);
	g_autofree char *file = settings_file(USER);

	g_assert_cmpint(chmod(file, 0644), ==, 0);
	g_assert_null(chat_lists_open(USER, &e));
	g_clear_error(&e);
	g_assert_cmpint(chmod(file, 0600), ==, 0);
	g_assert_true(g_file_set_contents(file, "{\"folders\":{\"bad\":[]}}",
					  -1, &e));
	g_assert_null(chat_lists_open(USER, &e));
	g_clear_error(&e);
	g_remove(file);
	g_assert_cmpint(symlink("/dev/null", file), ==, 0);
	g_assert_null(chat_lists_open(USER, &e));
	g_clear_error(&e);
	g_remove(file);
	g_assert_null(chat_lists_open("../../bad", &e));
}

static void mute_and_motion(void)
{
	g_autoptr(GError) e = NULL;
	struct chat_lists *s = chat_lists_open(USER, &e);

	g_assert_no_error(e);
	g_assert_false(chat_muted(s, CHAT, FALSE));
	g_assert_cmpint(chat_motion(s), ==, -1);
	static const char room[] = "m03030303030303030303030303030303";

	g_assert_true(chat_muted(s, CHAT, TRUE));
	g_assert_true(chat_set_muted(s, CHAT, FALSE, &e));
	g_assert_false(chat_muted(s, CHAT, TRUE));
	g_assert_true(chat_set_muted(s, room, TRUE, &e));
	g_assert_true(chat_muted(s, room, FALSE));
	g_assert_true(chat_motion_set(s, 1, &e));
	g_assert_false(chat_set_muted(s, "../other", TRUE, &e));
	g_clear_error(&e);
	chat_lists_free(s);
	s = chat_lists_open(USER, &e);
	g_assert_no_error(e);
	g_assert_false(chat_muted(s, CHAT, TRUE));
	g_assert_true(chat_muted(s, room, FALSE));
	g_assert_false(chat_muted(s, PEER, FALSE));
	g_assert_false(chat_pinned(s, CHAT));
	g_assert_cmpint(chat_motion(s), ==, 1);
	g_assert_true(chat_set_muted(s, CHAT, TRUE, &e));
	g_assert_true(chat_set_muted(s, room, FALSE, &e));
	g_assert_true(chat_motion_set(s, -1, &e));
	g_assert_true(chat_muted(s, CHAT, FALSE));
	g_assert_false(chat_muted(s, room, TRUE));
	g_assert_cmpint(chat_motion(s), ==, -1);
	chat_lists_free(s);
	g_autofree char *file = settings_file(USER);

	g_assert_true(
		g_file_set_contents(file, "{\"motion\":\"yes\"}", -1, &e));
	g_assert_cmpint(chmod(file, 0600), ==, 0);
	g_assert_null(chat_lists_open(USER, &e));
	g_clear_error(&e);
	g_remove(file);
}

static void strict_mute(void)
{
	g_autoptr(GError) e = NULL;
	struct chat_lists *s = chat_lists_open(USER, &e);

	g_assert_no_error(e);
	g_assert_false(chat_strict_muted(s, CHAT));
	g_assert_false(chat_strict_mute_toggle(s, PEER, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	g_assert_true(chat_strict_mute_toggle(s, CHAT, &e));
	g_assert_true(chat_strict_muted(s, CHAT));
	g_assert_false(chat_muted(s, CHAT, FALSE));
	chat_lists_free(s);

	s = chat_lists_open(OTHER, &e);
	g_assert_no_error(e);
	g_assert_false(chat_strict_muted(s, CHAT));
	chat_lists_free(s);
	s = chat_lists_open(USER, &e);
	g_assert_no_error(e);
	g_assert_true(chat_strict_muted(s, CHAT));
	g_assert_true(chat_strict_mute_toggle(s, CHAT, &e));
	g_assert_false(chat_strict_muted(s, CHAT));
	chat_lists_free(s);
	g_autofree char *file = settings_file(USER);

	g_remove(file);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	state_dir = g_dir_make_tmp("line-lists-XXXXXX", NULL);
	g_assert_nonnull(state_dir);
	g_setenv("XDG_STATE_HOME", state_dir, TRUE);
	g_test_add_func("/lists/isolated-persistent-folders-pins-and-settings",
			round_trip);
	g_test_add_func("/lists/invalid-input-and-local-files", rejected);
	g_test_add_func("/lists/muted-chats-and-motion", mute_and_motion);
	g_test_add_func("/lists/strict-mute-per-group-and-account",
			strict_mute);
	int rc = g_test_run();
	g_autofree char *dir = g_build_filename(state_dir, "avernella", NULL);

	g_rmdir(dir);
	g_rmdir(state_dir);
	g_free(state_dir);
	return rc;
}
