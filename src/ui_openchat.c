// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_openchat.h"
#include "ui_friends.h"
#include "ui_groups.h"
#include "ui_lang.h"
#include "ui_motion.h"
#include "ui_theme.h"
#include <string.h>

enum oc_page {
	OC_CLOSED,
	OC_HUB,
	OC_DISCOVER,
	OC_JOIN,
	OC_CREATE,
	OC_GROUP_LINK,
	OC_INFO,
	OC_MEMBERS,
	OC_REQUESTS,
	OC_MY_PROFILE,
	OC_INVITE,
	OC_SETTINGS,
	OC_ROOMS,
	OC_CONFIRM
};

enum oc_confirm_kind {
	CONF_NONE,
	CONF_LEAVE,
	CONF_LEAVE_CHAT,
	CONF_KICK,
	CONF_BAN,
	CONF_PROMOTE,
	CONF_DEMOTE
};

struct hub_item {
	char *square_mid, *chat_mid, *name, *subtitle, *avatar;
	Json *chat_row;
};

static struct {
	enum oc_page page, return_page;
	enum oc_confirm_kind confirm;
	guint serial, pending;
	char *chat, *square_mid, *notice;
	char *confirm_member, *confirm_name;
	char *invite_url, *member_cursor, *search_cursor;
	QRcode *qr;
	Json *detail, *preview, *group_preview, *categories, *members,
		*requests, *keywords, *search_results;
	GPtrArray *hub_rows;
	int cursor, offset, field_focus;
	int cat_index, join_method;
	gboolean searchable, invite_enabled;
	Editor ed[5]; /* Reusable text fields per page. */
	/* Cached hit rectangles computed during draw. */
	Rect dialog, list, tab[4], close_btn, primary, secondary, extra[8];
	Rect ed_rect[5], sw_rect[2], cycle_rect[2];
} oc;

static gboolean hit(Rect r, int x, int y)
{
	return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y &&
	       y < r.y + r.h;
}

static void set_str(char **slot, const char *s)
{
	char *copy = g_strdup(s);

	g_free(*slot);
	*slot = copy;
	ui.dirty = TRUE;
}

static void set_ed(Editor *ed, const char *s)
{
	if (!ed->value)
		ed->value = g_string_new("");
	g_string_assign(ed->value, s ? s : "");
	ed->cursor = ed->anchor = (int)ed->value->len;
	ed->scroll = 0;
}

static void free_hub_item(gpointer data)
{
	struct hub_item *it = data;

	g_free(it->square_mid);
	g_free(it->chat_mid);
	g_free(it->name);
	g_free(it->subtitle);
	g_free(it->avatar);
	json_object_put(it->chat_row);
	g_free(it);
}

static gboolean has_editor(void)
{
	return oc.page == OC_DISCOVER || oc.page == OC_JOIN ||
	       oc.page == OC_CREATE || oc.page == OC_MEMBERS ||
	       oc.page == OC_MY_PROFILE || oc.page == OC_SETTINGS ||
	       oc.page == OC_ROOMS;
}

static int editor_count(void)
{
	switch (oc.page) {
	case OC_DISCOVER:
		return 1;
	case OC_JOIN:
		return oc.preview ? 3 : 1;
	case OC_CREATE:
		return oc.join_method == 0 ? 4 : 5;
	case OC_MEMBERS:
		return 1;
	case OC_MY_PROFILE:
		return 2;
	case OC_SETTINGS:
		return 4;
	case OC_ROOMS:
		return 1;
	default:
		return 0;
	}
}

static Editor *active_editor(void)
{
	int n = editor_count();

	if (!n || oc.field_focus < 0 || oc.field_focus >= n)
		return NULL;
	return &oc.ed[oc.field_focus];
}

void ui_openchat_resume_input(void)
{
	if (active_editor())
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

void ui_openchat_reset(void)
{
	g_free(oc.chat);
	g_free(oc.square_mid);
	g_free(oc.notice);
	g_free(oc.confirm_member);
	g_free(oc.confirm_name);
	g_free(oc.invite_url);
	g_free(oc.member_cursor);
	g_free(oc.search_cursor);
	g_clear_pointer(&oc.qr, QRcode_free);
	json_object_put(oc.detail);
	json_object_put(oc.preview);
	json_object_put(oc.group_preview);
	json_object_put(oc.categories);
	json_object_put(oc.members);
	json_object_put(oc.requests);
	json_object_put(oc.keywords);
	json_object_put(oc.search_results);
	g_clear_pointer(&oc.hub_rows, g_ptr_array_unref);
	for (guint i = 0; i < G_N_ELEMENTS(oc.ed); i++)
		if (oc.ed[i].value)
			g_string_free(oc.ed[i].value, TRUE);
	guint serial = oc.serial;

	memset(&oc, 0, sizeof(oc));
	oc.serial = serial;
	oc.cursor = -1;
	ui.dirty = TRUE;
}

gboolean ui_openchat_active(void)
{
	return oc.page != OC_CLOSED;
}

static void close_openchat(void)
{
	ui_openchat_reset();
	ui_restore_input();
}

static void rpc(const char *method, Json *params)
{
	if (oc.pending) {
		json_object_put(params);
		return;
	}
	oc.serial = (oc.serial + 1) & 0x3fffffffu;
	oc.pending = 0x40000000u | oc.serial;
	set_str(&oc.notice, NULL);
	ui_rpc(method, params ? params : json_object_new_object(), oc.pending);
}

static void ensure_editors(void)
{
	for (guint i = 0; i < G_N_ELEMENTS(oc.ed); i++)
		if (!oc.ed[i].value)
			oc.ed[i].value = g_string_new("");
}

static void enter_page(enum oc_page page)
{
	ensure_editors();
	oc.page = page;
	oc.cursor = -1;
	oc.offset = 0;
	oc.field_focus = has_editor() ? 0 : -1;
	set_str(&oc.notice, NULL);
	memset(oc.extra, 0, sizeof(oc.extra));
	memset(oc.ed_rect, 0, sizeof(oc.ed_rect));
	memset(oc.sw_rect, 0, sizeof(oc.sw_rect));
	memset(oc.cycle_rect, 0, sizeof(oc.cycle_rect));
	ui_openchat_resume_input();
	ui.dirty = TRUE;
}

static void load_hub(void)
{
	enter_page(OC_HUB);
	rpc("squareList", NULL);
}

static void open_discover_tab(void)
{
	enter_page(OC_DISCOVER);
	if (!oc.keywords)
		rpc("squarePopularKeywords", NULL);
}

static void run_discover_search(gboolean more)
{
	if (!oc.ed[0].value || !oc.ed[0].value->len) {
		set_str(&oc.notice, _("Enter a keyword to search OpenChats."));
		return;
	}
	Json *p = json_object_new_object();

	json_object_object_add(p, "query",
			       json_object_new_string(oc.ed[0].value->str));
	if (more && oc.search_cursor)
		json_object_object_add(
			p, "cursor", json_object_new_string(oc.search_cursor));
	else {
		g_clear_pointer(&oc.search_cursor, g_free);
		g_clear_pointer(&oc.search_results, json_object_put);
		oc.cursor = -1;
		oc.offset = 0;
	}
	rpc("squareSearch", p);
}

static void load_info(const char *square_mid, const char *chat_mid)
{
	if (square_mid)
		set_str(&oc.square_mid, square_mid);
	if (chat_mid)
		set_str(&oc.chat, chat_mid);
	enter_page(OC_INFO);
	Json *p = json_object_new_object();

	if (oc.square_mid)
		json_object_object_add(p, "squareMid",
				       json_object_new_string(oc.square_mid));
	if (oc.chat)
		json_object_object_add(p, "chat",
				       json_object_new_string(oc.chat));
	rpc("squareDetail", p);
}

static void load_members(gboolean more)
{
	Json *p = json_object_new_object();

	if (oc.square_mid)
		json_object_object_add(p, "squareMid",
				       json_object_new_string(oc.square_mid));
	if (oc.ed[0].value && oc.ed[0].value->len)
		json_object_object_add(
			p, "query",
			json_object_new_string(oc.ed[0].value->str));
	if (more && oc.member_cursor)
		json_object_object_add(
			p, "cursor", json_object_new_string(oc.member_cursor));
	else {
		g_clear_pointer(&oc.member_cursor, g_free);
		g_clear_pointer(&oc.members, json_object_put);
		oc.cursor = -1;
		oc.offset = 0;
	}
	rpc("squareMembers", p);
}

static void load_requests(void)
{
	enter_page(OC_REQUESTS);
	g_clear_pointer(&oc.requests, json_object_put);
	Json *p = json_object_new_object();

	if (oc.square_mid)
		json_object_object_add(p, "squareMid",
				       json_object_new_string(oc.square_mid));
	rpc("squarePendingMembers", p);
}

static void open_create_tab(void)
{
	enter_page(OC_CREATE);
	set_ed(&oc.ed[0], "");
	set_ed(&oc.ed[1], "");
	set_ed(&oc.ed[2], "");
	set_ed(&oc.ed[3], ui.name ? ui.name : "");
	set_ed(&oc.ed[4], "");
	oc.searchable = TRUE;
	oc.join_method = 0;
	oc.cat_index = 0;
	if (!oc.categories)
		rpc("squareCategories", NULL);
}

static void open_join_tab(const char *prefill)
{
	g_clear_pointer(&oc.preview, json_object_put);
	enter_page(OC_JOIN);
	set_ed(&oc.ed[0], prefill ? prefill : "");
	set_ed(&oc.ed[1], ui.name ? ui.name : "");
	set_ed(&oc.ed[2], "");
	if (prefill && *prefill) {
		Json *p = json_object_new_object();

		json_object_object_add(p, "input",
				       json_object_new_string(prefill));
		rpc("squareFindTicket", p);
	}
}

void ui_openchat_open_hub(void)
{
	if (!ui.connected || !ui.openchats)
		return;
	ui_openchat_reset();
	load_hub();
}

void ui_openchat_open_info(void)
{
	if (!ui.connected || !ui.openchats || !ui.selected ||
	    ui.selected[0] != 'm')
		return;
	Json *chat = ui_selected_chat();
	const char *smid = ui_string(chat, "squareMid", NULL);

	ui_openchat_reset();
	load_info(smid, ui.selected);
}

void ui_openchat_open_link(const char *url)
{
	if (!ui.connected || !ui.openchats || !url || !*url)
		return;
	ui_openchat_reset();
	open_join_tab(url);
}

void ui_openchat_open_group_link(const char *url)
{
	if (!ui.connected || !url || !*url)
		return;
	ui_openchat_reset();
	enter_page(OC_GROUP_LINK);
	Json *p = json_object_new_object();

	json_object_object_add(p, "input", json_object_new_string(url));
	rpc("groupFindTicket", p);
}

void ui_openchat_avatar_picked(const char *path)
{
	if (!ui_openchat_active() || !oc.square_mid || !path || !*path)
		return;
	Json *p = json_object_new_object();

	json_object_object_add(p, "squareMid",
			       json_object_new_string(oc.square_mid));
	json_object_object_add(p, "path", json_object_new_string(path));
	rpc("squareUpdatePicture", p);
}

static size_t list_count(void)
{
	if (oc.page == OC_HUB)
		return oc.hub_rows ? oc.hub_rows->len : 0;
	if (oc.page == OC_DISCOVER)
		return oc.search_results ?
			       json_object_array_length(oc.search_results) :
			       0;
	if (oc.page == OC_MEMBERS)
		return oc.members ? json_object_array_length(oc.members) : 0;
	if (oc.page == OC_REQUESTS)
		return oc.requests ? json_object_array_length(oc.requests) : 0;
	if (oc.page == OC_ROOMS) {
		Json *subs = ui_field(oc.detail, "subchats");

		return subs ? json_object_array_length(subs) : 0;
	}
	return 0;
}

static int row_height(void)
{
	return oc.page == OC_REQUESTS ? 58 : 48;
}

static int visible_rows(void)
{
	return MAX(1, oc.list.h / row_height());
}

static void submit_primary(void)
{
	if (oc.pending)
		return;
	if (oc.page == OC_DISCOVER) {
		if (oc.cursor >= 0 && oc.search_results &&
		    (size_t)oc.cursor <
			    json_object_array_length(oc.search_results)) {
			Json *sel = json_object_array_get_idx(oc.search_results,
							      oc.cursor);

			json_object_put(oc.preview);
			oc.preview = json_object_get(sel);
			enter_page(OC_JOIN);
			set_ed(&oc.ed[1], ui.name ? ui.name : "");
			set_ed(&oc.ed[2], "");
			oc.field_focus = 1;
			ui_openchat_resume_input();
			const char *invite =
				ui_string(sel, "invitationUrl", NULL);

			if (invite) {
				set_ed(&oc.ed[0], invite);
				Json *p = json_object_new_object();

				json_object_object_add(
					p, "input",
					json_object_new_string(invite));
				rpc("squareFindTicket", p);
			}
			return;
		}
		run_discover_search(FALSE);
		return;
	}
	if (oc.page == OC_JOIN) {
		if (!oc.preview) {
			if (!oc.ed[0].value->len) {
				set_str(&oc.notice,
					_("Paste an OpenChat invite link or ticket."));
				return;
			}
			Json *p = json_object_new_object();

			json_object_object_add(
				p, "input",
				json_object_new_string(oc.ed[0].value->str));
			rpc("squareFindTicket", p);
			return;
		}
		if (json_object_get_boolean(ui_field(oc.preview, "joined"))) {
			const char *cid = ui_string(oc.preview, "chatMid", "");

			if (*cid) {
				Json *existing = NULL;

				for (size_t i = 0;
				     ui.chats &&
				     i < json_object_array_length(ui.chats);
				     i++) {
					Json *c = json_object_array_get_idx(
						ui.chats, i);

					if (!g_strcmp0(ui_string(c, "id", ""),
						       cid)) {
						existing = c;
						break;
					}
				}
				if (existing) {
					close_openchat();
					ui_open_conversation(existing);
				} else {
					/* Register/join the room in the worker before opening. */
					Json *p = json_object_new_object();

					json_object_object_add(
						p, "chat",
						json_object_new_string(cid));
					rpc("squareJoinChat", p);
				}
			}
			return;
		}
		const char *jm = ui_string(oc.preview, "joinMethod", "open");
		Json *p = json_object_new_object();

		json_object_object_add(p, "squareMid",
				       json_object_new_string(ui_string(
					       oc.preview, "squareMid", "")));
		if (ui_string(oc.preview, "chatMid", NULL))
			json_object_object_add(
				p, "chatMid",
				json_object_new_string(
					ui_string(oc.preview, "chatMid", "")));
		json_object_object_add(
			p, "displayName",
			json_object_new_string(oc.ed[1].value->str));
		if (!strcmp(jm, "code"))
			json_object_object_add(
				p, "joinCode",
				json_object_new_string(oc.ed[2].value->str));
		else if (!strcmp(jm, "approval"))
			json_object_object_add(
				p, "approvalMessage",
				json_object_new_string(oc.ed[2].value->str));
		rpc("squareJoin", p);
	} else if (oc.page == OC_GROUP_LINK && oc.group_preview) {
		Json *p = json_object_new_object();

		json_object_object_add(p, "chat",
				       json_object_new_string(ui_string(
					       oc.group_preview, "id", "")));
		rpc("groupJoinTicket", p);
	} else if (oc.page == OC_CREATE) {
		int cat_id = 1;

		if (oc.categories &&
		    (size_t)oc.cat_index <
			    json_object_array_length(oc.categories))
			cat_id = json_object_get_int(
				ui_field(json_object_array_get_idx(
						 oc.categories, oc.cat_index),
					 "id"));
		Json *p = json_object_new_object();

		json_object_object_add(
			p, "name", json_object_new_string(oc.ed[0].value->str));
		json_object_object_add(
			p, "description",
			json_object_new_string(oc.ed[1].value->str));
		json_object_object_add(
			p, "welcome",
			json_object_new_string(oc.ed[2].value->str));
		json_object_object_add(
			p, "displayName",
			json_object_new_string(oc.ed[3].value->str));
		json_object_object_add(p, "categoryId",
				       json_object_new_int(cat_id));
		json_object_object_add(p, "searchable",
				       json_object_new_boolean(oc.searchable));
		json_object_object_add(p, "joinMethod",
				       json_object_new_int(oc.join_method));
		if (oc.join_method == 2)
			json_object_object_add(
				p, "joinCode",
				json_object_new_string(oc.ed[4].value->str));
		else if (oc.join_method == 1)
			json_object_object_add(
				p, "approvalQuestion",
				json_object_new_string(oc.ed[4].value->str));
		rpc("squareCreate", p);
	} else if (oc.page == OC_MY_PROFILE) {
		Json *p = json_object_new_object();

		json_object_object_add(p, "squareMid",
				       json_object_new_string(oc.square_mid));
		json_object_object_add(
			p, "displayName",
			json_object_new_string(oc.ed[0].value->str));
		json_object_object_add(
			p, "bio", json_object_new_string(oc.ed[1].value->str));
		rpc("squareUpdateProfile", p);
	} else if (oc.page == OC_SETTINGS) {
		Json *p = json_object_new_object();

		json_object_object_add(p, "squareMid",
				       json_object_new_string(oc.square_mid));
		json_object_object_add(
			p, "name", json_object_new_string(oc.ed[0].value->str));
		json_object_object_add(
			p, "description",
			json_object_new_string(oc.ed[2].value->str));
		json_object_object_add(
			p, "welcome",
			json_object_new_string(oc.ed[3].value->str));
		json_object_object_add(p, "searchable",
				       json_object_new_boolean(oc.searchable));
		json_object_object_add(
			p, "inviteEnabled",
			json_object_new_boolean(oc.invite_enabled));
		rpc("squareUpdate", p);
	} else if (oc.page == OC_INVITE && oc.invite_url) {
		if (SDL_SetClipboardText(oc.invite_url) == 0)
			set_str(&oc.notice, _("Invite link copied."));
	} else if (oc.page == OC_MEMBERS) {
		load_members(FALSE);
	} else if (oc.page == OC_ROOMS) {
		if (oc.ed[0].value->len &&
		    json_object_get_boolean(
			    ui_field(oc.detail, "canCreateChat"))) {
			Json *p = json_object_new_object();

			json_object_object_add(
				p, "squareMid",
				json_object_new_string(oc.square_mid));
			json_object_object_add(
				p, "name",
				json_object_new_string(oc.ed[0].value->str));
			rpc("squareCreateChat", p);
		} else if (oc.cursor >= 0) {
			Json *subs = ui_field(oc.detail, "subchats");
			Json *row =
				subs && (size_t)oc.cursor <
							json_object_array_length(
								subs) ?
					json_object_array_get_idx(subs,
								  oc.cursor) :
					NULL;
			const char *cid = ui_string(row, "id", NULL);

			if (cid) {
				Json *p = json_object_new_object();

				json_object_object_add(
					p, "chat", json_object_new_string(cid));
				rpc("squareJoinChat", p);
			}
		}
	} else if (oc.page == OC_CONFIRM) {
		Json *p = json_object_new_object();

		if (oc.square_mid)
			json_object_object_add(
				p, "squareMid",
				json_object_new_string(oc.square_mid));
		if (oc.confirm == CONF_LEAVE) {
			if (oc.chat)
				json_object_object_add(
					p, "chat",
					json_object_new_string(oc.chat));
			rpc("squareLeave", p);
		} else if (oc.confirm == CONF_LEAVE_CHAT && oc.chat) {
			json_object_object_add(p, "chat",
					       json_object_new_string(oc.chat));
			rpc("squareLeaveChat", p);
		} else if (oc.confirm_member) {
			const char *act = oc.confirm == CONF_KICK ? "kick" :
					  oc.confirm == CONF_BAN  ? "ban" :
					  oc.confirm == CONF_PROMOTE ?
								   "promote" :
								   "demote";

			json_object_object_add(
				p, "member",
				json_object_new_string(oc.confirm_member));
			json_object_object_add(p, "action",
					       json_object_new_string(act));
			rpc("squareModerateMember", p);
		} else {
			json_object_put(p);
		}
	} else if (oc.page == OC_HUB && oc.cursor >= 0 && oc.hub_rows &&
		   (guint)oc.cursor < oc.hub_rows->len) {
		struct hub_item *it = g_ptr_array_index(oc.hub_rows, oc.cursor);

		if (it->chat_row) {
			Json *row = json_object_get(it->chat_row);

			close_openchat();
			ui_open_conversation(row);
			json_object_put(row);
		} else {
			load_info(it->square_mid, NULL);
		}
	}
}

static void start_confirm(enum oc_confirm_kind kind, enum oc_page back_to,
			  const char *member_id, const char *member_name)
{
	oc.confirm = kind;
	oc.return_page = back_to;
	set_str(&oc.confirm_member, member_id);
	set_str(&oc.confirm_name, member_name);
	enter_page(OC_CONFIRM);
}

static void rebuild_hub_rows(Json *result)
{
	g_clear_pointer(&oc.hub_rows, g_ptr_array_unref);
	oc.hub_rows = g_ptr_array_new_with_free_func(free_hub_item);
	Json *squares = ui_field(result, "squares");

	for (size_t i = 0; squares && i < json_object_array_length(squares);
	     i++) {
		Json *sq = json_object_array_get_idx(squares, i);
		const char *smid = ui_string(sq, "squareMid", "");
		const char *sname = ui_string(sq, "name", smid);
		const char *role = ui_string(sq, "myRole", "member");
		const char *sav = ui_string(sq, "avatarKey", NULL);
		Json *chats = ui_field(sq, "chats");

		if (chats && json_object_array_length(chats)) {
			for (size_t j = 0; j < json_object_array_length(chats);
			     j++) {
				Json *c = json_object_array_get_idx(chats, j);
				struct hub_item *it =
					g_new0(struct hub_item, 1);

				it->square_mid = g_strdup(smid);
				it->chat_mid = g_strdup(ui_string(c, "id", ""));
				it->name =
					g_strdup(ui_string(c, "name", sname));
				it->subtitle = g_strdup_printf(
					"%s · %s", sname,
					!strcmp(role, "admin") ? _("Admin") :
					!strcmp(role, "coadmin") ?
								 _("Co-admin") :
								 _("Member"));
				it->avatar = g_strdup(
					ui_string(c, "avatarKey", sav));
				it->chat_row = json_object_get(c);
				g_ptr_array_add(oc.hub_rows, it);
			}
		} else {
			struct hub_item *it = g_new0(struct hub_item, 1);

			it->square_mid = g_strdup(smid);
			it->name = g_strdup(sname);
			it->subtitle =
				g_strdup(ui_string(sq, "description", ""));
			it->avatar = g_strdup(sav);
			g_ptr_array_add(oc.hub_rows, it);
		}
	}
}

gboolean ui_openchat_reply(const char *method, guint tag, Json *result,
			   const char *error)
{
	if (!g_str_has_prefix(method, "square") &&
	    strcmp(method, "groupFindTicket") &&
	    strcmp(method, "groupJoinTicket"))
		return FALSE;
	if (!strcmp(method, "squareDestroyMessage")) {
		if (error)
			ui_status(error);
		else {
			ui_status(_("Message deleted."));
			ui_show_latest();
		}
		return TRUE;
	}
	if (!oc.pending || tag != oc.pending)
		return TRUE;
	oc.pending = 0;
	if (error) {
		set_str(&oc.notice, error);
		return TRUE;
	}
	if (!strcmp(method, "squareList")) {
		rebuild_hub_rows(result);
	} else if (!strcmp(method, "squarePopularKeywords")) {
		json_object_put(oc.keywords);
		oc.keywords = json_object_get(result);
	} else if (!strcmp(method, "squareSearch")) {
		Json *list = ui_field(result, "squares");

		if (!oc.search_results)
			oc.search_results = json_object_new_array();
		for (size_t i = 0; list && i < json_object_array_length(list);
		     i++)
			json_object_array_add(
				oc.search_results,
				json_object_get(
					json_object_array_get_idx(list, i)));
		set_str(&oc.search_cursor, ui_string(result, "cursor", NULL));
		if (!json_object_array_length(oc.search_results))
			set_str(&oc.notice, _("No matching OpenChats found."));
	} else if (!strcmp(method, "squareCategories")) {
		json_object_put(oc.categories);
		oc.categories = json_object_get(result);
	} else if (!strcmp(method, "squareFindTicket")) {
		json_object_put(oc.preview);
		oc.preview = json_object_get(result);
		oc.field_focus = 1;
		ui_openchat_resume_input();
	} else if (!strcmp(method, "groupFindTicket")) {
		json_object_put(oc.group_preview);
		oc.group_preview = json_object_get(result);
	} else if (!strcmp(method, "groupJoinTicket")) {
		Json *row = json_object_get(result);

		close_openchat();
		if (row) {
			ui_open_conversation(row);
			json_object_put(row);
		}
		ui_status(_("Joined group."));
		ui_refresh();
	} else if (!strcmp(method, "squareJoin") ||
		   !strcmp(method, "squareCreate")) {
		gboolean pending =
			json_object_get_boolean(ui_field(result, "pending"));
		Json *chat = ui_field(result, "chat");

		if (pending) {
			set_str(&oc.notice,
				_("Join request sent. Waiting for admin approval."));
		} else if (chat) {
			Json *row = json_object_get(chat);

			close_openchat();
			ui_open_conversation(row);
			json_object_put(row);
			ui_refresh();
		} else {
			load_hub();
		}
	} else if (!strcmp(method, "squareJoinChat") ||
		   !strcmp(method, "squareCreateChat")) {
		Json *row = json_object_get(result);

		close_openchat();
		if (row) {
			ui_open_conversation(row);
			json_object_put(row);
		}
		ui_refresh();
	} else if (!strcmp(method, "squareDetail")) {
		json_object_put(oc.detail);
		oc.detail = json_object_get(result);
		if (ui_string(oc.detail, "squareMid", NULL))
			set_str(&oc.square_mid,
				ui_string(oc.detail, "squareMid", ""));
		if (ui_string(oc.detail, "chatMid", NULL) && !oc.chat)
			set_str(&oc.chat, ui_string(oc.detail, "chatMid", ""));
	} else if (!strcmp(method, "squareInviteLink")) {
		set_str(&oc.invite_url, ui_string(result, "url", ""));
		g_clear_pointer(&oc.qr, QRcode_free);
		if (oc.invite_url && *oc.invite_url)
			oc.qr = QRcode_encodeString8bit(oc.invite_url, 0,
							QR_ECLEVEL_M);
	} else if (!strcmp(method, "squareUpdate")) {
		const char *room_name = oc.ed[1].value ? oc.ed[1].value->str :
							 "";
		const char *old_room = ui_string(oc.detail, "chatName", "");

		if (oc.chat && *room_name && strcmp(room_name, old_room)) {
			Json *p = json_object_new_object();

			json_object_object_add(p, "chat",
					       json_object_new_string(oc.chat));
			json_object_object_add(
				p, "name", json_object_new_string(room_name));
			rpc("squareUpdateChat", p);
			return TRUE;
		}
		ui_refresh();
		load_info(oc.square_mid, oc.chat);
		set_str(&oc.notice, _("OpenChat settings saved."));
	} else if (!strcmp(method, "squareUpdateChat")) {
		ui_refresh();
		load_info(oc.square_mid, oc.chat);
		set_str(&oc.notice, _("OpenChat settings saved."));
	} else if (!strcmp(method, "squareUpdateProfile") ||
		   !strcmp(method, "squareUpdatePicture")) {
		load_info(oc.square_mid, oc.chat);
		set_str(&oc.notice, _("OpenChat profile updated."));
	} else if (!strcmp(method, "squareMembers")) {
		Json *list = ui_field(result, "members");

		if (!oc.members)
			oc.members = json_object_new_array();
		for (size_t i = 0; list && i < json_object_array_length(list);
		     i++)
			json_object_array_add(
				oc.members,
				json_object_get(
					json_object_array_get_idx(list, i)));
		set_str(&oc.member_cursor, ui_string(result, "cursor", NULL));
	} else if (!strcmp(method, "squarePendingMembers")) {
		json_object_put(oc.requests);
		oc.requests = json_object_get(ui_field(result, "members"));
	} else if (!strcmp(method, "squareModerateMember")) {
		enter_page(OC_MEMBERS);
		load_members(FALSE);
		set_str(&oc.notice, _("Member updated."));
	} else if (!strcmp(method, "squareApproveMember") ||
		   !strcmp(method, "squareRejectMember")) {
		load_requests();
	} else if (!strcmp(method, "squareLeave") ||
		   !strcmp(method, "squareLeaveChat")) {
		g_autofree char *left_chat = g_strdup(oc.chat);
		gboolean room_only = !strcmp(method, "squareLeaveChat");

		close_openchat();
		if (left_chat)
			ui_group_left(left_chat);
		else
			ui_refresh();
		ui_status(room_only ? _("Left room.") : _("Left OpenChat."));
	}
	ui.dirty = TRUE;
	return TRUE;
}

static void open_profile_editor(void)
{
	enter_page(OC_MY_PROFILE);
	set_ed(&oc.ed[0], ui_string(oc.detail, "myName", ""));
	set_ed(&oc.ed[1], ui_string(oc.detail, "myBio", ""));
}

static void open_settings_editor(void)
{
	enter_page(OC_SETTINGS);
	set_ed(&oc.ed[0], ui_string(oc.detail, "name", ""));
	set_ed(&oc.ed[1], ui_string(oc.detail, "chatName", ""));
	set_ed(&oc.ed[2], ui_string(oc.detail, "description", ""));
	set_ed(&oc.ed[3], ui_string(oc.detail, "welcome", ""));
	oc.searchable =
		json_object_get_boolean(ui_field(oc.detail, "searchable"));
	oc.invite_enabled =
		json_object_get_boolean(ui_field(oc.detail, "inviteEnabled"));
}

static void open_invite_page(void)
{
	enter_page(OC_INVITE);
	set_str(&oc.invite_url, ui_string(oc.detail, "invitationUrl", NULL));
	g_clear_pointer(&oc.qr, QRcode_free);
	if (oc.invite_url && *oc.invite_url)
		oc.qr = QRcode_encodeString8bit(oc.invite_url, 0, QR_ECLEVEL_M);
	else {
		Json *p = json_object_new_object();

		if (oc.chat)
			json_object_object_add(p, "chat",
					       json_object_new_string(oc.chat));
		if (oc.square_mid)
			json_object_object_add(
				p, "squareMid",
				json_object_new_string(oc.square_mid));
		rpc("squareInviteLink", p);
	}
}

static void step_list(int delta)
{
	size_t n = list_count();

	if (!n)
		return;
	oc.cursor = CLAMP(oc.cursor + delta, 0, (int)n - 1);
	oc.offset = CLAMP(oc.offset, MAX(0, oc.cursor - visible_rows() + 1),
			  oc.cursor);
	ui.dirty = TRUE;
}

gboolean ui_openchat_event(SDL_Event *e)
{
	if (!ui_openchat_active())
		return FALSE;
	switch (e->type) {
	case SDL_KEYDOWN: {
		SDL_KeyboardEvent k = e->key;
		SDL_Keycode sym = k.keysym.sym;

		if (sym == SDLK_ESCAPE && !oc.pending) {
			if (oc.page == OC_CONFIRM)
				enter_page(oc.return_page);
			else if (oc.page == OC_MEMBERS ||
				 oc.page == OC_REQUESTS ||
				 oc.page == OC_MY_PROFILE ||
				 oc.page == OC_INVITE ||
				 oc.page == OC_SETTINGS || oc.page == OC_ROOMS)
				enter_page(OC_INFO);
			else
				close_openchat();
			return TRUE;
		}
		if (oc.pending)
			return TRUE;
		if (sym == SDLK_TAB && editor_count() > 1) {
			int n = editor_count();
			int dir = (k.keysym.mod & KMOD_SHIFT) ? -1 : 1;

			oc.field_focus = (oc.field_focus + dir + n) % n;
			ui_openchat_resume_input();
			ui.dirty = TRUE;
			return TRUE;
		}
		if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && !k.repeat) {
			submit_primary();
			return TRUE;
		}
		if (sym == SDLK_UP) {
			step_list(-1);
			return TRUE;
		}
		if (sym == SDLK_DOWN) {
			step_list(1);
			return TRUE;
		}
		Editor *ed = active_editor();

		if (ed)
			ui_edit_key(ed, k);
		return TRUE;
	}
	case SDL_TEXTINPUT: {
		Editor *ed = active_editor();

		if (!oc.pending && ed && ed->value->len < 512)
			ui_insert(ed, e->text.text);
		return TRUE;
	}
	case SDL_MOUSEWHEEL: {
		static double carry;
		int rows = ui_wheel_rows(ui_wheel_y(&e->wheel), &carry);

		if (!oc.pending && list_count()) {
			oc.offset = CLAMP(
				oc.offset - rows * 2, 0,
				MAX(0, (int)list_count() - visible_rows()));
			ui.dirty = TRUE;
		}
		return TRUE;
	}
	case SDL_MOUSEBUTTONDOWN: {
		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		int x = e->button.x, y = e->button.y;

		if (oc.pending)
			return TRUE;
		if (!hit(oc.dialog, x, y) || hit(oc.close_btn, x, y)) {
			close_openchat();
			return TRUE;
		}
		/* Top hub tabs (My OpenChats / Discover / Join / Create). */
		if (oc.page == OC_HUB || oc.page == OC_DISCOVER ||
		    oc.page == OC_JOIN || oc.page == OC_CREATE) {
			if (hit(oc.tab[0], x, y)) {
				load_hub();
				return TRUE;
			}
			if (hit(oc.tab[1], x, y)) {
				open_discover_tab();
				return TRUE;
			}
			if (hit(oc.tab[2], x, y)) {
				open_join_tab(NULL);
				return TRUE;
			}
			if (hit(oc.tab[3], x, y)) {
				open_create_tab();
				return TRUE;
			}
		}
		/* Editors */
		for (int i = 0; i < editor_count(); i++) {
			if (hit(oc.ed_rect[i], x, y)) {
				oc.field_focus = i;
				oc.ed[i].cursor = oc.ed[i].anchor = ui_edit_hit(
					&oc.ed[i], oc.ed_rect[i], x);
				ui_openchat_resume_input();
				ui.dirty = TRUE;
				return TRUE;
			}
		}
		/* Toggles & cyclers */
		if (hit(oc.sw_rect[0], x, y)) {
			oc.searchable = !oc.searchable;
			ui.dirty = TRUE;
			return TRUE;
		}
		if (hit(oc.sw_rect[1], x, y)) {
			oc.invite_enabled = !oc.invite_enabled;
			ui.dirty = TRUE;
			return TRUE;
		}
		if (oc.page == OC_CREATE && hit(oc.cycle_rect[0], x, y) &&
		    oc.categories && json_object_array_length(oc.categories)) {
			oc.cat_index =
				(oc.cat_index + 1) %
				(int)json_object_array_length(oc.categories);
			ui.dirty = TRUE;
			return TRUE;
		}
		if (oc.page == OC_CREATE && hit(oc.cycle_rect[1], x, y)) {
			oc.join_method = (oc.join_method + 1) % 3;
			ui.dirty = TRUE;
			return TRUE;
		}
		if (hit(oc.primary, x, y)) {
			submit_primary();
			return TRUE;
		}
		if (hit(oc.secondary, x, y)) {
			if (oc.page == OC_CONFIRM)
				enter_page(oc.return_page);
			else if (oc.page == OC_MEMBERS ||
				 oc.page == OC_REQUESTS ||
				 oc.page == OC_MY_PROFILE ||
				 oc.page == OC_INVITE ||
				 oc.page == OC_SETTINGS || oc.page == OC_ROOMS)
				enter_page(OC_INFO);
			else if (oc.page == OC_JOIN && oc.preview) {
				g_clear_pointer(&oc.preview, json_object_put);
				oc.field_focus = 0;
				ui.dirty = TRUE;
			} else
				close_openchat();
			return TRUE;
		}
		/* Page-specific action buttons */
		if (oc.page == OC_INFO && oc.detail) {
			if (hit(oc.extra[0], x, y)) {
				enter_page(OC_MEMBERS);
				set_ed(&oc.ed[0], "");
				load_members(FALSE);
			} else if (hit(oc.extra[1], x, y))
				open_profile_editor();
			else if (hit(oc.extra[2], x, y))
				open_invite_page();
			else if (hit(oc.extra[3], x, y)) {
				enter_page(OC_ROOMS);
				set_ed(&oc.ed[0], "");
			} else if (hit(oc.extra[4], x, y))
				open_settings_editor();
			else if (hit(oc.extra[5], x, y))
				load_requests();
			else if (hit(oc.extra[6], x, y))
				start_confirm(CONF_LEAVE, OC_INFO, NULL,
					      ui_string(oc.detail, "name", ""));
			else if (oc.chat && hit(oc.extra[7], x, y))
				start_confirm(CONF_LEAVE_CHAT, OC_INFO, NULL,
					      ui_string(oc.detail, "chatName",
							ui_string(oc.detail,
								  "name", "")));
			return TRUE;
		}
		if (oc.page == OC_MY_PROFILE && hit(oc.extra[0], x, y)) {
			ui_media_pick_avatar();
			return TRUE;
		}
		if (oc.page == OC_ROOMS && hit(oc.extra[0], x, y) &&
		    oc.cursor >= 0) {
			Json *subs = ui_field(oc.detail, "subchats");
			Json *row =
				subs && (size_t)oc.cursor <
							json_object_array_length(
								subs) ?
					json_object_array_get_idx(subs,
								  oc.cursor) :
					NULL;
			const char *cid = ui_string(row, "id", NULL);

			if (cid) {
				Json *p = json_object_new_object();

				json_object_object_add(
					p, "chat", json_object_new_string(cid));
				rpc("squareJoinChat", p);
			}
			return TRUE;
		}
		if (oc.page == OC_DISCOVER) {
			if (hit(oc.extra[0], x, y)) {
				run_discover_search(FALSE);
				return TRUE;
			}
			if (hit(oc.extra[1], x, y) && oc.search_cursor) {
				run_discover_search(TRUE);
				return TRUE;
			}
			for (int k = 0;
			     k < 6 && oc.keywords &&
			     (size_t)k < json_object_array_length(oc.keywords);
			     k++) {
				if (hit(oc.extra[2 + k], x, y)) {
					const char *kw = json_object_get_string(
						json_object_array_get_idx(
							oc.keywords, k));

					set_ed(&oc.ed[0], kw);
					run_discover_search(FALSE);
					return TRUE;
				}
			}
		}
		if (oc.page == OC_HUB && hit(oc.extra[0], x, y) &&
		    oc.cursor >= 0 && oc.hub_rows &&
		    (guint)oc.cursor < oc.hub_rows->len) {
			struct hub_item *it =
				g_ptr_array_index(oc.hub_rows, oc.cursor);

			load_info(it->square_mid, it->chat_mid);
			return TRUE;
		}
		if (oc.page == OC_MEMBERS) {
			if (hit(oc.extra[0], x, y) && oc.member_cursor) {
				load_members(TRUE);
				return TRUE;
			}
			Json *sel =
				oc.cursor >= 0 && oc.members &&
						(size_t)oc.cursor <
							json_object_array_length(
								oc.members) ?
					json_object_array_get_idx(oc.members,
								  oc.cursor) :
					NULL;
			if (sel) {
				const char *mid = ui_string(sel, "id", "");
				const char *name = ui_string(sel, "name", mid);
				const char *role = ui_string(sel, "role", "");

				if (hit(oc.extra[1], x, y))
					start_confirm(!strcmp(role, "coadmin") ?
							      CONF_DEMOTE :
							      CONF_PROMOTE,
						      OC_MEMBERS, mid, name);
				else if (hit(oc.extra[2], x, y))
					start_confirm(CONF_KICK, OC_MEMBERS,
						      mid, name);
				else if (hit(oc.extra[3], x, y))
					start_confirm(CONF_BAN, OC_MEMBERS, mid,
						      name);
			}
		}
		if (oc.page == OC_REQUESTS && oc.cursor >= 0 && oc.requests &&
		    (size_t)oc.cursor < json_object_array_length(oc.requests)) {
			Json *sel = json_object_array_get_idx(oc.requests,
							      oc.cursor);
			const char *mid = ui_string(sel, "id", "");

			if (hit(oc.extra[0], x, y) || hit(oc.extra[1], x, y)) {
				gboolean approve = hit(oc.extra[0], x, y);
				Json *p = json_object_new_object();

				json_object_object_add(
					p, "squareMid",
					json_object_new_string(oc.square_mid));
				json_object_object_add(
					p, "member",
					json_object_new_string(mid));
				rpc(approve ? "squareApproveMember" :
					      "squareRejectMember",
				    p);
				return TRUE;
			}
		}
		if (hit(oc.list, x, y)) {
			int idx = oc.offset + (y - oc.list.y) / row_height();

			if (idx >= 0 && (size_t)idx < list_count()) {
				oc.cursor = idx;
				if (e->button.clicks >= 2 &&
				    (oc.page == OC_HUB ||
				     oc.page == OC_DISCOVER ||
				     oc.page == OC_ROOMS))
					submit_primary();
				ui.dirty = TRUE;
			}
		}
		return TRUE;
	}
	case SDL_MOUSEMOTION:
		ui.dirty = TRUE;
		return TRUE;
	default:
		return FALSE;
	}
}

static void draw_square_avatar(const char *id, const char *name,
			       const char *key, Rect r)
{
	Json *row = json_object_new_object();

	json_object_object_add(row, "id", json_object_new_string(id ? id : ""));
	json_object_object_add(
		row, "name", json_object_new_string(name ? name : "OpenChat"));
	if (key && *key)
		json_object_object_add(row, "avatarKey",
				       json_object_new_string(key));
	ui_draw_profile(row, r);
	json_object_put(row);
}

static void draw_tabs(int x, int y, int w)
{
	const char *const labels[4] = { _("My OpenChats"), _("Discover"),
					_("Join by link"), _("Create") };
	int tw = (w - 24) / 4;

	for (int i = 0; i < 4; i++) {
		oc.tab[i] = (Rect){ x + i * (tw + 8), y, tw, 34 };
		gboolean active = (i == 0 && oc.page == OC_HUB) ||
				  (i == 1 && oc.page == OC_DISCOVER) ||
				  (i == 2 && oc.page == OC_JOIN) ||
				  (i == 3 && oc.page == OC_CREATE);

		ui_draw_button(oc.tab[i], labels[i], active);
	}
}

void ui_openchat_draw(void)
{
	if (!ui_openchat_active())
		return;
	int w = MIN(620, ui.width - 24);
	int h = MIN(oc.page == OC_CONFIRM || oc.page == OC_GROUP_LINK ? 280 :
									580,
		    ui.height - 24);
	int x = (ui.width - w) / 2, y = (ui.height - h) / 2;

	oc.dialog = (Rect){ x, y, w, h };
	oc.list = (Rect){ 0 };
	oc.primary = (Rect){ 0 };
	oc.secondary = (Rect){ 0 };
	memset(oc.tab, 0, sizeof(oc.tab));
	memset(oc.extra, 0, sizeof(oc.extra));
	memset(oc.ed_rect, 0, sizeof(oc.ed_rect));
	memset(oc.sw_rect, 0, sizeof(oc.sw_rect));
	memset(oc.cycle_rect, 0, sizeof(oc.cycle_rect));

	ui_scrim();
	ui_draw_rounded(oc.dialog, 12, UI_DIALOG);
	ui_draw_outline(oc.dialog, 12, UI_LINE);

	const char *title = oc.page == OC_GROUP_LINK ? _("Join LINE group") :
			    oc.page == OC_INFO	     ? _("OpenChat info") :
			    oc.page == OC_MEMBERS    ? _("OpenChat members") :
			    oc.page == OC_REQUESTS   ? _("Join requests") :
			    oc.page == OC_MY_PROFILE ?
						     _("My OpenChat profile") :
			    oc.page == OC_INVITE   ? _("OpenChat invite link") :
			    oc.page == OC_SETTINGS ? _("OpenChat settings") :
			    oc.page == OC_ROOMS	   ? _("OpenChat rooms") :
			    oc.page == OC_CONFIRM  ? _("Confirm action") :
						     _("OpenChat");

	ui_draw_label(title, (Rect){ x + 20, y + 16, w - 72, 32 }, 20, 700,
		      UI_HEADING, FALSE);
	oc.close_btn = (Rect){ x + w - 44, y + 14, 30, 30 };
	ui_draw_icon(oc.close_btn, UI_ICON_CLOSE, UI_MUTED);

	int cx = x + 20, cw = w - 40, cy = y + 58;
	int bottom = y + h - 54;

	if (oc.page == OC_HUB || oc.page == OC_DISCOVER || oc.page == OC_JOIN ||
	    oc.page == OC_CREATE) {
		draw_tabs(cx, cy, cw);
		cy += 46;
	}

	if (oc.page == OC_DISCOVER) {
		oc.ed_rect[0] = (Rect){ cx, cy, cw - 108, 36 };
		ui_draw_edit(&oc.ed[0], oc.ed_rect[0], oc.field_focus == 0,
			     _("Search public OpenChats…"));
		oc.extra[0] = (Rect){ cx + cw - 100, cy, 100, 36 };
		ui_draw_button(oc.extra[0], _("Search"), TRUE);
		cy += 46;
		if (oc.keywords && json_object_array_length(oc.keywords)) {
			int kx = cx;

			for (int k = 0;
			     k < 6 &&
			     (size_t)k < json_object_array_length(oc.keywords);
			     k++) {
				const char *kw = json_object_get_string(
					json_object_array_get_idx(oc.keywords,
								  k));
				int kw_w =
					CLAMP(ui_text_width(kw, 12, 550) + 20,
					      52, 140);

				if (kx + kw_w > cx + cw)
					break;
				oc.extra[2 + k] = (Rect){ kx, cy, kw_w, 26 };
				ui_draw_button(oc.extra[2 + k], kw, FALSE);
				kx += kw_w + 6;
			}
			cy += 34;
		}
		oc.list = (Rect){ cx, cy, cw, MAX(60, bottom - cy - 36) };
		ui_draw_rounded(oc.list, 8, UI_PANEL);
		size_t n = list_count();

		if (oc.pending)
			ui_draw_label(_("Searching OpenChats…"),
				      (Rect){ cx + 14, cy + 16, cw - 28, 28 },
				      14, 500, UI_MUTED, FALSE);
		else if (!n)
			ui_draw_label(
				_("Search by keyword or click a popular topic above."),
				(Rect){ cx + 14, cy + 16, cw - 28, 44 }, 14,
				450, UI_MUTED, TRUE);
		int rows = visible_rows();

		for (int i = 0; i < rows && (size_t)(oc.offset + i) < n; i++) {
			Json *sq = json_object_array_get_idx(oc.search_results,
							     oc.offset + i);
			Rect r = { oc.list.x + 6,
				   oc.list.y + 4 + i * row_height(),
				   oc.list.w - 12, row_height() - 4 };

			if (oc.cursor == oc.offset + i)
				ui_draw_rounded(r, 6, UI_SELECTED);
			draw_square_avatar(ui_string(sq, "squareMid", "s"),
					   ui_string(sq, "name", "OpenChat"),
					   ui_string(sq, "avatarKey", NULL),
					   (Rect){ r.x + 6, r.y + 4, 34, 34 });
			ui_draw_label(ui_string(sq, "name", "OpenChat"),
				      (Rect){ r.x + 48, r.y + 3, r.w - 56, 20 },
				      14, 600, UI_HEADING, FALSE);
			const char *count_text =
				ui_string(sq, "memberCountText", NULL);
			const char *description =
				ui_string(sq, "description", "");
			g_autofree char *sub =
				count_text && *count_text ?
					g_strdup_printf("%s · %s", count_text,
							description) :
					g_strdup_printf(
						_("%u members · %s"),
						(unsigned int)CLAMP(
							json_object_get_int64(ui_field(
								sq,
								"memberCount")),
							0, G_MAXINT),
						description);
			ui_draw_label(
				sub, (Rect){ r.x + 48, r.y + 23, r.w - 56, 18 },
				12, 450, UI_MUTED, FALSE);
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Close"), FALSE);
		if (oc.search_cursor) {
			oc.extra[1] = (Rect){ cx + 118, bottom, 100, 36 };
			ui_draw_button(oc.extra[1], _("More"), FALSE);
		}
		if (oc.cursor >= 0 && (size_t)oc.cursor < n) {
			oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
			ui_draw_button(oc.primary, _("Preview & join"), TRUE);
		}
	} else if (oc.page == OC_HUB) {
		oc.list = (Rect){ cx, cy, cw, MAX(60, bottom - cy - 36) };
		ui_draw_rounded(oc.list, 8, UI_PANEL);
		size_t n = list_count();

		if (oc.pending)
			ui_draw_label(_("Loading OpenChats…"),
				      (Rect){ cx + 14, cy + 16, cw - 28, 28 },
				      14, 500, UI_MUTED, FALSE);
		else if (!n)
			ui_draw_label(
				_("You have not joined any OpenChats yet. Use Join by link or Create."),
				(Rect){ cx + 14, cy + 16, cw - 28, 56 }, 14,
				450, UI_MUTED, TRUE);
		int rows = visible_rows();

		for (int i = 0; i < rows && (size_t)(oc.offset + i) < n; i++) {
			struct hub_item *it =
				g_ptr_array_index(oc.hub_rows, oc.offset + i);
			Rect r = { oc.list.x + 6,
				   oc.list.y + 4 + i * row_height(),
				   oc.list.w - 12, row_height() - 4 };

			if (oc.cursor == oc.offset + i)
				ui_draw_rounded(r, 6, UI_SELECTED);
			draw_square_avatar(it->chat_mid ? it->chat_mid :
							  it->square_mid,
					   it->name, it->avatar,
					   (Rect){ r.x + 6, r.y + 4, 34, 34 });
			ui_draw_label(it->name,
				      (Rect){ r.x + 48, r.y + 3, r.w - 56, 20 },
				      14, 600, UI_HEADING, FALSE);
			if (it->subtitle && *it->subtitle)
				ui_draw_label(it->subtitle,
					      (Rect){ r.x + 48, r.y + 23,
						      r.w - 56, 18 },
					      12, 450, UI_MUTED, FALSE);
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Close"), FALSE);
		if (oc.cursor >= 0 && (size_t)oc.cursor < n) {
			oc.extra[0] = (Rect){ x + w - 280, bottom, 122, 36 };
			ui_draw_button(oc.extra[0], _("Manage"), FALSE);
			oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
			ui_draw_button(oc.primary, _("Open chat"), TRUE);
		}
	} else if (oc.page == OC_JOIN) {
		ui_draw_label(_("Invite link or ticket"),
			      (Rect){ cx, cy, cw, 20 }, 13, 600, UI_MUTED,
			      FALSE);
		cy += 22;
		oc.ed_rect[0] = (Rect){ cx, cy, cw, 38 };
		ui_draw_edit(&oc.ed[0], oc.ed_rect[0], oc.field_focus == 0,
			     "https://line.me/ti/g2/…");
		cy += 50;
		if (oc.preview) {
			Rect card = { cx, cy, cw, 110 };

			ui_draw_rounded(card, 8, UI_PANEL);
			draw_square_avatar(
				ui_string(oc.preview, "squareMid", "s"),
				ui_string(oc.preview, "name", "OpenChat"),
				ui_string(oc.preview, "avatarKey", NULL),
				(Rect){ card.x + 12, card.y + 12, 52, 52 });
			ui_draw_label(ui_string(oc.preview, "name", "OpenChat"),
				      (Rect){ card.x + 74, card.y + 12,
					      card.w - 86, 24 },
				      16, 700, UI_HEADING, FALSE);
			g_autofree char *meta = g_strdup_printf(
				_("%u members · %s"),
				(unsigned int)CLAMP(
					json_object_get_int64(ui_field(
						oc.preview, "memberCount")),
					0, G_MAXINT),
				!g_strcmp0(ui_string(oc.preview, "joinMethod",
						     ""),
					   "code") ?
					_("Passcode required") :
				!g_strcmp0(ui_string(oc.preview, "joinMethod",
						     ""),
					   "approval") ?
					_("Approval required") :
					_("Open to join"));
			ui_draw_label(meta,
				      (Rect){ card.x + 74, card.y + 38,
					      card.w - 86, 20 },
				      13, 500, UI_MUTED, FALSE);
			ui_draw_label(ui_string(oc.preview, "description", ""),
				      (Rect){ card.x + 12, card.y + 70,
					      card.w - 24, 34 },
				      13, 450, UI_BODY, TRUE);
			cy += 122;
			if (!json_object_get_boolean(
				    ui_field(oc.preview, "joined"))) {
				ui_draw_label(_("Your OpenChat nickname"),
					      (Rect){ cx, cy, cw, 20 }, 13, 600,
					      UI_MUTED, FALSE);
				cy += 22;
				oc.ed_rect[1] = (Rect){ cx, cy, cw, 38 };
				ui_draw_edit(&oc.ed[1], oc.ed_rect[1],
					     oc.field_focus == 1,
					     _("Nickname"));
				cy += 46;
				const char *jm =
					ui_string(oc.preview, "joinMethod", "");

				if (!strcmp(jm, "code") ||
				    !strcmp(jm, "approval")) {
					const char *q = ui_string(
						oc.preview, "approvalQuestion",
						NULL);

					ui_draw_label(
						!strcmp(jm, "code") ?
							_("Join passcode") :
						q && *q ?
							q :
							_("Join request message"),
						(Rect){ cx, cy, cw, 20 }, 13,
						600, UI_MUTED, FALSE);
					cy += 22;
					oc.ed_rect[2] =
						(Rect){ cx, cy, cw, 38 };
					ui_draw_edit(&oc.ed[2], oc.ed_rect[2],
						     oc.field_focus == 2,
						     !strcmp(jm, "code") ?
							     _("Passcode") :
							     _("Your answer"));
				}
			}
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary,
			       oc.preview ? _("Back") : _("Close"), FALSE);
		oc.primary = (Rect){ x + w - 170, bottom, 150, 36 };
		ui_draw_button(oc.primary,
			       oc.pending  ? _("Working…") :
			       !oc.preview ? _("Look up") :
			       json_object_get_boolean(
				       ui_field(oc.preview, "joined")) ?
					     _("Open chat") :
					     _("Join OpenChat"),
			       TRUE);
	} else if (oc.page == OC_GROUP_LINK) {
		if (oc.pending)
			ui_draw_label(_("Looking up LINE group…"),
				      (Rect){ cx, cy, cw, 28 }, 15, 500,
				      UI_MUTED, FALSE);
		else if (oc.group_preview) {
			Rect card = { cx, cy, cw, 92 };

			ui_draw_rounded(card, 8, UI_PANEL);
			ui_draw_profile(oc.group_preview,
					(Rect){ card.x + 14, card.y + 18, 56,
						56 });
			ui_draw_label(ui_string(oc.group_preview, "name",
						"Group"),
				      (Rect){ card.x + 84, card.y + 24,
					      card.w - 96, 28 },
				      17, 700, UI_HEADING, FALSE);
			ui_draw_label(
				_("Join this LINE group using the invite link."),
				(Rect){ card.x + 84, card.y + 52, card.w - 96,
					22 },
				13, 450, UI_MUTED, FALSE);
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Cancel"), FALSE);
		if (oc.group_preview) {
			oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
			ui_draw_button(oc.primary,
				       oc.pending ? _("Joining…") :
						    _("Join group"),
				       TRUE);
		}
	} else if (oc.page == OC_CREATE) {
		const char *const labels[4] = { _("OpenChat name"),
						_("Description"),
						_("Welcome message"),
						_("Your nickname") };

		for (int i = 0; i < 4; i++) {
			ui_draw_label(labels[i], (Rect){ cx, cy, cw, 18 }, 12,
				      600, UI_MUTED, FALSE);
			cy += 20;
			oc.ed_rect[i] = (Rect){ cx, cy, cw, 34 };
			ui_draw_edit(&oc.ed[i], oc.ed_rect[i],
				     oc.field_focus == i, labels[i]);
			cy += 42;
		}
		/* Category + Join method cyclers */
		const char *cat_name = _("General");

		if (oc.categories &&
		    (size_t)oc.cat_index <
			    json_object_array_length(oc.categories))
			cat_name =
				ui_string(json_object_array_get_idx(
						  oc.categories, oc.cat_index),
					  "name", cat_name);
		g_autofree char *cat_btn =
			g_strdup_printf(_("Category: %s"), cat_name);
		const char *jm_label =
			oc.join_method == 1 ? _("Join: Approval") :
			oc.join_method == 2 ? _("Join: Passcode") :
					      _("Join: Open");

		oc.cycle_rect[0] = (Rect){ cx, cy, (cw - 12) / 2, 34 };
		oc.cycle_rect[1] = (Rect){ cx + (cw - 12) / 2 + 12, cy,
					   (cw - 12) / 2, 34 };
		ui_draw_button(oc.cycle_rect[0], cat_btn, FALSE);
		ui_draw_button(oc.cycle_rect[1], jm_label, FALSE);
		cy += 42;
		ui_draw_label(_("Allow search"), (Rect){ cx, cy + 4, 200, 22 },
			      14, 500, UI_HEADING, FALSE);
		oc.sw_rect[0] = (Rect){ cx + cw - 40, cy + 2, 40, 24 };
		ui_draw_switch(oc.sw_rect[0], oc.searchable);
		cy += 34;
		if (oc.join_method != 0) {
			oc.ed_rect[4] = (Rect){ cx, cy, cw, 34 };
			ui_draw_edit(&oc.ed[4], oc.ed_rect[4],
				     oc.field_focus == 4,
				     oc.join_method == 2 ?
					     _("Join passcode (4-32 chars)") :
					     _("Approval question"));
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Close"), FALSE);
		oc.primary = (Rect){ x + w - 170, bottom, 150, 36 };
		ui_draw_button(oc.primary,
			       oc.pending ? _("Creating…") :
					    _("Create OpenChat"),
			       TRUE);
	} else if (oc.page == OC_INFO) {
		if (oc.pending && !oc.detail)
			ui_draw_label(_("Loading OpenChat details…"),
				      (Rect){ cx, cy, cw, 28 }, 15, 500,
				      UI_MUTED, FALSE);
		else if (oc.detail) {
			Rect card = { cx, cy, cw, 118 };

			ui_draw_rounded(card, 8, UI_PANEL);
			draw_square_avatar(
				ui_string(oc.detail, "squareMid", "s"),
				ui_string(oc.detail, "name", "OpenChat"),
				ui_string(oc.detail, "avatarKey", NULL),
				(Rect){ card.x + 14, card.y + 14, 56, 56 });
			ui_draw_label(ui_string(oc.detail, "name", "OpenChat"),
				      (Rect){ card.x + 82, card.y + 14,
					      card.w - 94, 24 },
				      17, 700, UI_HEADING, FALSE);
			const char *role =
				ui_string(oc.detail, "myRole", "member");
			g_autofree char *sub = g_strdup_printf(
				_("%u members · %s (%s)"),
				(unsigned int)CLAMP(
					json_object_get_int64(ui_field(
						oc.detail, "memberCount")),
					0, G_MAXINT),
				ui_string(oc.detail, "myName", ""),
				!strcmp(role, "admin")	 ? _("Admin") :
				!strcmp(role, "coadmin") ? _("Co-admin") :
							   _("Member"));

			ui_draw_label(sub,
				      (Rect){ card.x + 82, card.y + 40,
					      card.w - 94, 20 },
				      13, 500, UI_MUTED, FALSE);
			ui_draw_label(ui_string(oc.detail, "description", ""),
				      (Rect){ card.x + 14, card.y + 78,
					      card.w - 28, 34 },
				      13, 450, UI_BODY, TRUE);
			cy += 132;
			int bw = (cw - 12) / 2;

			oc.extra[0] = (Rect){ cx, cy, bw, 38 };
			oc.extra[1] = (Rect){ cx + bw + 12, cy, bw, 38 };
			ui_draw_button(oc.extra[0], _("Members"), FALSE);
			ui_draw_button(oc.extra[1], _("My profile"), FALSE);
			cy += 48;
			oc.extra[2] = (Rect){ cx, cy, bw, 38 };
			oc.extra[3] = (Rect){ cx + bw + 12, cy, bw, 38 };
			ui_draw_button(oc.extra[2], _("Invite link"), FALSE);
			ui_draw_button(oc.extra[3], _("Rooms"), FALSE);
			cy += 48;
			if (json_object_get_boolean(
				    ui_field(oc.detail, "canEdit"))) {
				oc.extra[4] = (Rect){ cx, cy, bw, 38 };
				ui_draw_button(oc.extra[4], _("Settings"),
					       FALSE);
			}
			if (json_object_get_boolean(
				    ui_field(oc.detail, "canApprove"))) {
				gint64 reqs = json_object_get_int64(ui_field(
					oc.detail, "joinRequestCount"));
				g_autofree char *rlbl =
					reqs > 0 ?
						g_strdup_printf(
							_("Join requests (%u)"),
							(unsigned int)CLAMP(
								reqs, 0,
								G_MAXINT)) :
						g_strdup(_("Join requests"));

				oc.extra[5] =
					(Rect){ cx + bw + 12, cy, bw, 38 };
				ui_draw_button(oc.extra[5], rlbl, FALSE);
			}
			cy += 54;
			oc.extra[6] = (Rect){ cx, cy, bw, 38 };
			ui_draw_button(oc.extra[6], _("Leave OpenChat"), FALSE);
			if (oc.chat) {
				oc.extra[7] =
					(Rect){ cx + bw + 12, cy, bw, 38 };
				ui_draw_button(oc.extra[7], _("Leave room"),
					       FALSE);
			}
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Close"), FALSE);
	} else if (oc.page == OC_MEMBERS) {
		oc.ed_rect[0] = (Rect){ cx, cy, cw - 96, 36 };
		ui_draw_edit(&oc.ed[0], oc.ed_rect[0], oc.field_focus == 0,
			     _("Search members…"));
		oc.primary = (Rect){ cx + cw - 88, cy, 88, 36 };
		ui_draw_button(oc.primary, _("Search"), FALSE);
		cy += 46;
		gboolean can_role = json_object_get_boolean(
			ui_field(oc.detail, "canGrantRole"));
		gboolean can_kick = json_object_get_boolean(
			ui_field(oc.detail, "canRemoveMember"));
		int mod_h = (can_role || can_kick) ? 46 : 0;

		oc.list =
			(Rect){ cx, cy, cw, MAX(60, bottom - cy - mod_h - 36) };
		ui_draw_rounded(oc.list, 8, UI_PANEL);
		size_t n = list_count();
		int rows = visible_rows();

		for (int i = 0; i < rows && (size_t)(oc.offset + i) < n; i++) {
			Json *m = json_object_array_get_idx(oc.members,
							    oc.offset + i);
			Rect r = { oc.list.x + 6,
				   oc.list.y + 4 + i * row_height(),
				   oc.list.w - 12, row_height() - 4 };

			if (oc.cursor == oc.offset + i)
				ui_draw_rounded(r, 6, UI_SELECTED);
			ui_draw_profile(m, (Rect){ r.x + 6, r.y + 4, 34, 34 });
			const char *role = ui_string(m, "role", "member");
			g_autofree char *label =
				!strcmp(role, "admin") ?
					g_strdup_printf("%s · %s",
							ui_string(m, "name",
								  ""),
							_("Admin")) :
				!strcmp(role, "coadmin") ?
					g_strdup_printf("%s · %s",
							ui_string(m, "name",
								  ""),
							_("Co-admin")) :
					g_strdup(ui_string(m, "name", ""));

			ui_draw_label(label,
				      (Rect){ r.x + 48, r.y + 10, r.w - 56,
					      22 },
				      14, 550, UI_HEADING, FALSE);
		}
		int my = oc.list.y + oc.list.h + 8;

		if (oc.cursor >= 0 && (size_t)oc.cursor < n &&
		    (can_role || can_kick)) {
			Json *sel = json_object_array_get_idx(oc.members,
							      oc.cursor);
			const char *sel_id = ui_string(sel, "id", "");
			const char *sel_role = ui_string(sel, "role", "");
			gboolean self = !g_strcmp0(
				sel_id, ui_string(oc.detail, "myMemberId", ""));

			if (!self && strcmp(sel_role, "admin")) {
				int bx = cx;

				if (can_role) {
					oc.extra[1] = (Rect){ bx, my, 150, 34 };
					ui_draw_button(
						oc.extra[1],
						!strcmp(sel_role, "coadmin") ?
							_("Remove co-admin") :
							_("Make co-admin"),
						FALSE);
					bx += 158;
				}
				if (can_kick) {
					oc.extra[2] = (Rect){ bx, my, 82, 34 };
					ui_draw_button(oc.extra[2], _("Kick"),
						       FALSE);
					bx += 90;
					oc.extra[3] = (Rect){ bx, my, 82, 34 };
					ui_draw_button(oc.extra[3], _("Ban"),
						       FALSE);
				}
			}
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Back"), FALSE);
		if (oc.member_cursor) {
			oc.extra[0] = (Rect){ x + w - 150, bottom, 130, 36 };
			ui_draw_button(oc.extra[0], _("Load more"), FALSE);
		}
	} else if (oc.page == OC_REQUESTS) {
		oc.list = (Rect){ cx, cy, cw, MAX(60, bottom - cy - 36) };
		ui_draw_rounded(oc.list, 8, UI_PANEL);
		size_t n = list_count();

		if (!n && !oc.pending)
			ui_draw_label(_("No pending join requests."),
				      (Rect){ cx + 14, cy + 16, cw - 28, 28 },
				      14, 450, UI_MUTED, FALSE);
		int rows = visible_rows();

		for (int i = 0; i < rows && (size_t)(oc.offset + i) < n; i++) {
			Json *req = json_object_array_get_idx(oc.requests,
							      oc.offset + i);
			Rect r = { oc.list.x + 6,
				   oc.list.y + 4 + i * row_height(),
				   oc.list.w - 12, row_height() - 4 };

			if (oc.cursor == oc.offset + i)
				ui_draw_rounded(r, 6, UI_SELECTED);
			ui_draw_profile(req,
					(Rect){ r.x + 6, r.y + 8, 36, 36 });
			ui_draw_label(ui_string(req, "name", ""),
				      (Rect){ r.x + 50, r.y + 6, r.w - 58, 22 },
				      14, 600, UI_HEADING, FALSE);
			ui_draw_label(ui_string(req, "joinMessage", ""),
				      (Rect){ r.x + 50, r.y + 28, r.w - 58,
					      22 },
				      12, 450, UI_MUTED, FALSE);
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Back"), FALSE);
		if (oc.cursor >= 0 && (size_t)oc.cursor < n) {
			oc.extra[1] = (Rect){ x + w - 266, bottom, 116, 36 };
			ui_draw_button(oc.extra[1], _("Reject"), FALSE);
			oc.extra[0] = (Rect){ x + w - 140, bottom, 120, 36 };
			ui_draw_button(oc.extra[0], _("Approve"), TRUE);
		}
	} else if (oc.page == OC_MY_PROFILE) {
		draw_square_avatar(ui_string(oc.detail, "myMemberId", "p"),
				   ui_string(oc.detail, "myName", ""),
				   ui_string(oc.detail, "myAvatarKey", NULL),
				   (Rect){ cx, cy, 56, 56 });
		oc.extra[0] = (Rect){ cx + 70, cy + 10, 150, 36 };
		ui_draw_button(oc.extra[0], _("Change photo"), FALSE);
		cy += 72;
		ui_draw_label(_("OpenChat nickname"), (Rect){ cx, cy, cw, 20 },
			      13, 600, UI_MUTED, FALSE);
		cy += 22;
		oc.ed_rect[0] = (Rect){ cx, cy, cw, 38 };
		ui_draw_edit(&oc.ed[0], oc.ed_rect[0], oc.field_focus == 0,
			     _("Nickname"));
		cy += 50;
		ui_draw_label(_("Bio"), (Rect){ cx, cy, cw, 20 }, 13, 600,
			      UI_MUTED, FALSE);
		cy += 22;
		oc.ed_rect[1] = (Rect){ cx, cy, cw, 38 };
		ui_draw_edit(&oc.ed[1], oc.ed_rect[1], oc.field_focus == 1,
			     _("Self-introduction"));
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Back"), FALSE);
		oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
		ui_draw_button(oc.primary,
			       oc.pending ? _("Saving…") : _("Save"), TRUE);
	} else if (oc.page == OC_INVITE) {
		if (oc.invite_url && *oc.invite_url) {
			ui_draw_label(oc.invite_url, (Rect){ cx, cy, cw, 40 },
				      14, 550, UI_HEADING, TRUE);
			if (oc.qr)
				ui_draw_code(oc.qr, x + w / 2, cy + 54,
					     MIN(220, bottom - cy - 90));
		} else if (oc.pending) {
			ui_draw_label(_("Loading invite link…"),
				      (Rect){ cx, cy, cw, 28 }, 14, 500,
				      UI_MUTED, FALSE);
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Back"), FALSE);
		if (oc.invite_url && *oc.invite_url) {
			oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
			ui_draw_button(oc.primary, _("Copy link"), TRUE);
		}
	} else if (oc.page == OC_SETTINGS) {
		const char *const labels[4] = { _("OpenChat name"),
						_("Room name"),
						_("Description"),
						_("Welcome message") };

		for (int i = 0; i < 4; i++) {
			ui_draw_label(labels[i], (Rect){ cx, cy, cw, 18 }, 12,
				      600, UI_MUTED, FALSE);
			cy += 20;
			oc.ed_rect[i] = (Rect){ cx, cy, cw, 34 };
			ui_draw_edit(&oc.ed[i], oc.ed_rect[i],
				     oc.field_focus == i, labels[i]);
			cy += 42;
		}
		ui_draw_label(_("Allow search"), (Rect){ cx, cy + 4, 240, 22 },
			      14, 500, UI_HEADING, FALSE);
		oc.sw_rect[0] = (Rect){ cx + cw - 40, cy + 2, 40, 24 };
		ui_draw_switch(oc.sw_rect[0], oc.searchable);
		cy += 34;
		ui_draw_label(_("Allow invite link"),
			      (Rect){ cx, cy + 4, 240, 22 }, 14, 500,
			      UI_HEADING, FALSE);
		oc.sw_rect[1] = (Rect){ cx + cw - 40, cy + 2, 40, 24 };
		ui_draw_switch(oc.sw_rect[1], oc.invite_enabled);
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Back"), FALSE);
		oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
		ui_draw_button(oc.primary,
			       oc.pending ? _("Saving…") : _("Save"), TRUE);
	} else if (oc.page == OC_ROOMS) {
		gboolean can_create = json_object_get_boolean(
			ui_field(oc.detail, "canCreateChat"));

		if (can_create) {
			oc.ed_rect[0] = (Rect){ cx, cy, cw - 132, 36 };
			ui_draw_edit(&oc.ed[0], oc.ed_rect[0],
				     oc.field_focus == 0, _("New room name…"));
			oc.primary = (Rect){ cx + cw - 124, cy, 124, 36 };
			ui_draw_button(oc.primary, _("Create room"), TRUE);
			cy += 46;
		}
		oc.list = (Rect){ cx, cy, cw, MAX(60, bottom - cy - 36) };
		ui_draw_rounded(oc.list, 8, UI_PANEL);
		Json *subs = ui_field(oc.detail, "subchats");
		size_t n = list_count();
		int rows = visible_rows();

		if (!n)
			ui_draw_label(
				_("No additional rooms in this OpenChat."),
				(Rect){ cx + 14, cy + 16, cw - 28, 28 }, 14,
				450, UI_MUTED, FALSE);
		for (int i = 0; i < rows && (size_t)(oc.offset + i) < n; i++) {
			Json *rrow =
				json_object_array_get_idx(subs, oc.offset + i);
			Rect r = { oc.list.x + 6,
				   oc.list.y + 4 + i * row_height(),
				   oc.list.w - 12, row_height() - 4 };

			if (oc.cursor == oc.offset + i)
				ui_draw_rounded(r, 6, UI_SELECTED);
			ui_draw_profile(rrow,
					(Rect){ r.x + 6, r.y + 4, 34, 34 });
			ui_draw_label(ui_string(rrow, "name", ""),
				      (Rect){ r.x + 48, r.y + 10, r.w - 56,
					      22 },
				      14, 550, UI_HEADING, FALSE);
		}
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Back"), FALSE);
		if (oc.cursor >= 0 && (size_t)oc.cursor < n) {
			oc.extra[0] = (Rect){ x + w - 150, bottom, 130, 36 };
			ui_draw_button(oc.extra[0], _("Join room"), TRUE);
		}
	} else if (oc.page == OC_CONFIRM) {
		const char *who = oc.confirm_name ? oc.confirm_name : "";
		g_autofree char *msg =
			oc.confirm == CONF_LEAVE ?
				g_strdup_printf(_("Leave %s?"), who) :
			oc.confirm == CONF_LEAVE_CHAT ?
				g_strdup_printf(_("Leave room %s?"), who) :
			oc.confirm == CONF_KICK ?
				g_strdup_printf(
					_("Kick %s from this OpenChat?"), who) :
			oc.confirm == CONF_BAN ?
				g_strdup_printf(_("Ban %s from this OpenChat?"),
						who) :
			oc.confirm == CONF_PROMOTE ?
				g_strdup_printf(_("Make %s a co-admin?"), who) :
				g_strdup_printf(
					_("Remove co-admin role from %s?"),
					who);

		ui_draw_label(msg, (Rect){ cx, cy + 16, cw, 64 }, 16, 550,
			      UI_HEADING, TRUE);
		oc.secondary = (Rect){ cx, bottom, 110, 36 };
		ui_draw_button(oc.secondary, _("Cancel"), FALSE);
		oc.primary = (Rect){ x + w - 150, bottom, 130, 36 };
		ui_draw_button(oc.primary,
			       oc.pending ? _("Working…") : _("Confirm"), TRUE);
	}

	if (oc.notice && *oc.notice)
		ui_draw_label(oc.notice, (Rect){ cx, bottom - 40, cw, 36 }, 13,
			      500, UI_MUTED, TRUE);
}
