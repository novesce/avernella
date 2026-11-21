// SPDX-License-Identifier: GPL-3.0-or-later
#include "talk_sync.h"

void line_talk_sync_clear(struct line_talk_sync *sync)
{
	json_object_put(sync->partial);
	*sync = (struct line_talk_sync){ .revision = -1 };
}

J *line_talk_sync_request(const struct line_talk_sync *sync)
{
	J *args = obj(), *req = obj();

	json_object_object_add(args, "1:obj", req);
	json_object_object_add(req, "1:i64",
			       json_object_new_int64(sync->revision));
	json_object_object_add(req, "2:i32", json_object_new_int(100));
	json_object_object_add(req, "3:i64",
			       json_object_new_int64(sync->global_revision));
	json_object_object_add(
		req, "4:i64", json_object_new_int64(sync->individual_revision));
	if (sync->partial)
		json_object_object_add(req, "6:map:i32:i64",
				       json_object_get(sync->partial));
	return args;
}

static gint64 revision(J *value)
{
	return json_object_is_type(value, json_type_int) ?
		       json_object_get_int64(value) :
		       -1;
}

static int compare_revisions(const void *left, const void *right)
{
	J *const *a = left, *const *b = right;
	gint64 lhs = revision(field(*a, "1")), rhs = revision(field(*b, "1"));

	return (lhs > rhs) - (lhs < rhs);
}

static gboolean targets_valid(J *targets)
{
	if (!json_object_is_type(targets, json_type_object) ||
	    !json_object_object_length(targets))
		return FALSE;
	json_object_object_foreach(targets, category, cursor) {
		gint64 id;

		if (!g_ascii_string_to_signed(category, 10, G_MININT32,
					      G_MAXINT32, &id, NULL) ||
		    revision(cursor) < 0)
			return FALSE;
	}
	return TRUE;
}

gboolean line_talk_sync_apply(struct line_talk_sync *sync, J *response,
			      LineTalkOperation operation, gpointer data,
			      gboolean *reload)
{
	J *page = field(response, "1"), *full = field(response, "2"),
	  *partial = field(response, "3");

	*reload = FALSE;
	if (!!page + !!full + !!partial != 1)
		return FALSE;
	if (full) {
		gint64 next = revision(field(full, "2"));

		if (next < 0 || next < sync->revision)
			return FALSE;
		*reload = next > sync->revision;
		sync->revision = next;
		sync->more = FALSE;
		return FALSE;
	}
	if (partial) {
		J *targets = field(partial, "1");

		if (!targets_valid(targets))
			return FALSE;
		if (!sync->partial ||
		    !json_object_equal(sync->partial, targets)) {
			json_object_put(sync->partial);
			sync->partial = json_object_get(targets);
			*reload = TRUE;
		}
		sync->more = TRUE;
		return *reload;
	}
	if (!json_object_is_type(page, json_type_object))
		return FALSE;
	J *ops = field(page, "1"), *more = field(page, "2");

	if ((ops && !json_object_is_type(ops, json_type_array)) ||
	    (more && !json_object_is_type(more, json_type_boolean)))
		return FALSE;
	gsize count = ops ? json_object_array_length(ops) : 0;
	gint64 last = -1, before = sync->revision;
	gboolean ordered = TRUE;

	for (gsize i = 0; i < count; i++) {
		gint64 next =
			revision(field(json_object_array_get_idx(ops, i), "1"));

		if (next < 0)
			return FALSE;
		ordered &= next >= last;
		last = next;
	}
	if (!ordered)
		json_object_array_sort(ops, compare_revisions);
	for (gsize i = 0; i < count; i++) {
		J *op = json_object_array_get_idx(ops, i);
		gint64 next = revision(field(op, "1"));

		if (next <= sync->revision)
			continue;
		operation(op, data);
		sync->revision = next;
	}
	sync->global_revision = MAX(sync->global_revision,
				    revision(field(field(page, "3"), "2")));
	sync->individual_revision = MAX(sync->individual_revision,
					revision(field(field(page, "4"), "2")));
	sync->more = json_object_get_boolean(more);
	return sync->more && sync->revision > before;
}
