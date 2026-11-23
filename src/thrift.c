// SPDX-License-Identifier: GPL-3.0-or-later
/* The subset of Apache Thrift Compact used by the LINE desktop RPCs. */
#include "line.h"
#include <string.h>

enum {
	STOP,
	TRUE_T,
	FALSE_T,
	BYTE,
	I16,
	I32,
	I64,
	DOUBLE,
	BINARY,
	LIST,
	SET,
	MAP,
	STRUCT
};

enum {
	MAX_DEPTH = 48,
	MAX_VALUES = 300000
};

static void byte(GByteArray *out, guint8 x)
{
	g_byte_array_append(out, &x, 1);
}

static void varint(GByteArray *out, guint64 n)
{
	while (n >= 128) {
		byte(out, (n & 127) | 128);
		n >>= 7;
	}
	byte(out, n);
}

static void integer(GByteArray *out, gint64 x)
{
	guint64 sign_mask = 0 - (guint64)(x < 0);

	varint(out, ((guint64)x << 1) ^ sign_mask);
}

static void binary(GByteArray *out, const void *p, size_t len)
{
	varint(out, len);
	if (len)
		g_byte_array_append(out, p, len);
}

static int type_id(const char *spec)
{
	static const char *const names[] = { "",    "bool", "",	   "byte",
					     "i16", "i32",  "i64", "double",
					     "str", "list", "set", "map",
					     "obj" };
	size_t n = strcspn(spec, ":");

	for (size_t i = 1; i < G_N_ELEMENTS(names); i++)
		if (n && strlen(names[i]) == n && !strncmp(spec, names[i], n))
			return i;
	return -1;
}

static gboolean write_value(GByteArray *, const char *, J *, int);

static gboolean write_fields(GByteArray *out, J *fields, int depth)
{
	int last = 0;

	if (depth > MAX_DEPTH ||
	    (fields && !json_object_is_type(fields, json_type_object)))
		return FALSE;
	if (fields) {
		json_object_object_foreach(fields, key, value) {
			char *end;
			gint64 id = g_ascii_strtoll(key, &end, 10);

			if (end == key || *end != ':' || id < G_MININT16 ||
			    id > G_MAXINT16)
				return FALSE;
			const char *spec = end + 1;
			int tag = type_id(spec);

			if (tag < 0)
				return FALSE;
			if (tag == TRUE_T && !json_object_get_boolean(value))
				tag = FALSE_T;
			gint64 delta = id - last;

			if (delta > 0 && delta <= 15)
				byte(out, (delta << 4) | tag);
			else {
				byte(out, tag);
				integer(out, id);
			}
			last = id;
			if (tag != TRUE_T && tag != FALSE_T &&
			    !write_value(out, spec, value, depth + 1))
				return FALSE;
		}
	}
	byte(out, STOP);
	return TRUE;
}

static gboolean write_value(GByteArray *out, const char *spec, J *v, int depth)
{
	if (depth > MAX_DEPTH)
		return FALSE;
	switch (type_id(spec)) {
	case TRUE_T:
		byte(out, json_object_get_boolean(v) ? TRUE_T : FALSE_T);
		break;
	case BYTE:
		byte(out, json_object_get_int(v));
		break;
	case I16:
	case I32:
	case I64:
		integer(out, json_object_get_int64(v));
		break;
	case BINARY:
		if (!json_object_is_type(v, json_type_string))
			return FALSE;
		binary(out, json_object_get_string(v),
		       json_object_get_string_len(v));
		break;
	case STRUCT:
		return write_fields(out, v, depth);
	case SET:
	case LIST: {
		const char *item = strchr(spec, ':');

		if (!item || !json_object_is_type(v, json_type_array))
			return FALSE;
		int tag = type_id(++item);

		if (tag < 0)
			return FALSE;
		size_t n = json_object_array_length(v);

		byte(out, (MIN(n, 15) << 4) | tag);
		if (n >= 15)
			varint(out, n);
		for (size_t i = 0; i < n; i++)
			if (!write_value(out, item,
					 json_object_array_get_idx(v, i),
					 depth + 1))
				return FALSE;
		break;
	}
	case MAP: {
		int key_type = g_str_has_prefix(spec, "map:i32:") ? I32 :
								    BINARY;

		if ((key_type == BINARY &&
		     !g_str_has_prefix(spec, "map:str:")) ||
		    !json_object_is_type(v, json_type_object))
			return FALSE;
		const char *item = spec + strlen("map:str:");
		int tag = type_id(item);

		if (tag < 0)
			return FALSE;
		int n = json_object_object_length(v);

		varint(out, n);
		if (n)
			byte(out, key_type << 4 | tag);
		json_object_object_foreach(v, k, value) {
			if (key_type == I32) {
				gint64 number;

				if (!g_ascii_string_to_signed(k, 10, G_MININT32,
							      G_MAXINT32,
							      &number, NULL))
					return FALSE;
				integer(out, number);
			} else
				binary(out, k, strlen(k));
			if (!write_value(out, item, value, depth + 1))
				return FALSE;
		}
		break;
	}
	default:
		return FALSE;
	}
	return TRUE;
}

GBytes *line_encode_call(const char *method, J *args, GError **error)
{
	g_autoptr(GByteArray) out = g_byte_array_new();
	byte(out, 0x82);
	byte(out, 0x21); /* version 1, CALL */
	byte(out, 0); /* one outstanding request per HTTP exchange */
	binary(out, method, strlen(method));
	if (!write_fields(out, args, 0)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid RPC request structure.");
		return NULL;
	}
	return g_byte_array_free_to_bytes(g_steal_pointer(&out));
}

static const guint8 binary_tags[] = {
	[TRUE_T] = 2, [BYTE] = 3, [DOUBLE] = 4,	 [I16] = 6,
	[I32] = 8,    [I64] = 10, [BINARY] = 11, [STRUCT] = 12,
	[MAP] = 13,   [SET] = 14, [LIST] = 15,
};

static void fixed(GByteArray *out, guint32 value, unsigned int size)
{
	while (size)
		byte(out, value >> (8 * --size));
}

static void fixed_string(GByteArray *out, const char *text, guint len)
{
	fixed(out, len, 4);
	g_byte_array_append(out, (const guint8 *)text, len);
}

/* The desktop authentication endpoints use Thrift Binary instead of Compact. */
static gboolean write_auth_fields(GByteArray *out, J *fields, int depth)
{
	if (depth > MAX_DEPTH || !json_object_is_type(fields, json_type_object))
		return FALSE;
	json_object_object_foreach(fields, key, value) {
		char *end;
		gint64 id = g_ascii_strtoll(key, &end, 10);
		int tag = *end == ':' ? type_id(end + 1) : -1;

		if (end == key || id < G_MININT16 || id > G_MAXINT16 || tag < 0)
			return FALSE;
		byte(out, binary_tags[tag]);
		fixed(out, id, 2);
		switch (tag) {
		case TRUE_T:
			byte(out, json_object_get_boolean(value));
			break;
		case I32:
			fixed(out, json_object_get_int(value), 4);
			break;
		case BINARY:
			if (!json_object_is_type(value, json_type_string))
				return FALSE;
			fixed_string(out, json_object_get_string(value),
				     json_object_get_string_len(value));
			break;
		case STRUCT:
			if (!write_auth_fields(out, value, depth + 1))
				return FALSE;
			break;
		default:
			return FALSE;
		}
	}
	byte(out, STOP);
	return TRUE;
}

GBytes *line_encode_auth(const char *method, J *args, GError **error)
{
	g_autoptr(GByteArray) out = g_byte_array_new();

	fixed(out, 0x80010001, 4);
	fixed_string(out, method, strlen(method));
	fixed(out, 0, 4);
	if (!write_auth_fields(out, args, 0)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Invalid authentication request structure.");
		return NULL;
	}
	return g_byte_array_free_to_bytes(g_steal_pointer(&out));
}

struct thrift_reader {
	const guint8 *p, *end;
	gboolean bad;
	unsigned int values;
	gboolean binary_protocol;
};

static guint8 take(struct thrift_reader *r)
{
	if (r->p == r->end) {
		r->bad = TRUE;
		return 0;
	}
	return *r->p++;
}

static guint64 number(struct thrift_reader *r)
{
	guint64 n = 0;

	for (unsigned int shift = 0; shift < 64; shift += 7) {
		guint8 b = take(r);

		if (shift == 63 && b > 1)
			break;
		n |= (guint64)(b & 127) << shift;
		if (!(b & 128))
			return n;
	}
	r->bad = TRUE;
	return 0;
}

static gint64 signed_number(struct thrift_reader *r)
{
	guint64 z = number(r);

	return (gint64)((z >> 1) ^ (0 - (z & 1)));
}

static guint64 take_fixed(struct thrift_reader *r, unsigned int n)
{
	guint64 value = 0;

	while (n--)
		value = (value << 8) | take(r);
	return value;
}

static guint8 binary_tag(guint8 tag)
{
	for (guint8 i = 1; i < G_N_ELEMENTS(binary_tags); i++)
		if (binary_tags[i] == tag)
			return i;
	return 255;
}

static J *read_value(struct thrift_reader *r, guint8 tag, int depth,
		     gboolean in_field)
{
	if (depth > MAX_DEPTH || ++r->values > MAX_VALUES)
		r->bad = TRUE;
	if (r->bad)
		return NULL;
	switch (tag) {
	case TRUE_T:
	case FALSE_T:
		if (r->binary_protocol) {
			guint8 b = take(r);

			if (b > 1)
				break;
			return json_object_new_boolean(b != 0);
		}
		if (!in_field)
			tag = take(r);
		if (tag != TRUE_T && tag != FALSE_T)
			break;
		return json_object_new_boolean(tag == TRUE_T);
	case BYTE:
		return json_object_new_int((gint8)take(r));
	case I16:
	case I32: {
		if (r->binary_protocol)
			return tag == I16 ? json_object_new_int(
						    (gint16)take_fixed(r, 2)) :
					    json_object_new_int(
						    (gint32)take_fixed(r, 4));
		gint64 value = signed_number(r);

		if (tag == I16 ? value < G_MININT16 || value > G_MAXINT16 :
				 value < G_MININT32 || value > G_MAXINT32)
			break;
		return json_object_new_int64(value);
	}
	case I64:
		if (r->binary_protocol)
			return json_object_new_int64((gint64)take_fixed(r, 8));
		return json_object_new_int64(signed_number(r));
	case DOUBLE: {
		if (r->end - r->p < 8)
			break;
		guint64 bits;
		double value;

		memcpy(&bits, r->p, 8);
		bits = r->binary_protocol ? GUINT64_FROM_BE(bits) :
					    GUINT64_FROM_LE(bits);
		memcpy(&value, &bits, 8);
		r->p += 8;
		return json_object_new_double(value);
	}
	case BINARY: {
		guint64 n = r->binary_protocol ? take_fixed(r, 4) : number(r);

		if (n > (gsize)(r->end - r->p) || n > G_MAXINT || r->bad)
			break;
		J *s = json_object_new_string_len((const char *)r->p, n);

		r->p += n;
		return s;
	}
	case STRUCT: {
		g_autoptr(J) result = obj();
		gint64 id = 0;

		for (;;) {
			guint8 head = take(r);

			if (r->bad)
				break;
			if (!head)
				return g_steal_pointer(&result);
			if (r->binary_protocol)
				id = (gint16)take_fixed(r, 2);
			else
				id = (head >> 4) ? id + (head >> 4) :
						   signed_number(r);
			if (id < G_MININT16 || id > G_MAXINT16)
				break;
			char key[12];

			g_snprintf(key, sizeof(key), "%d", (int)id);
			if (json_object_object_get_ex(result, key, NULL))
				break;
			guint8 field_tag = r->binary_protocol ?
						   binary_tag(head) :
						   head & 15;
			J *v = read_value(r, field_tag, depth + 1, TRUE);

			if (!v)
				break;
			json_object_object_add(result, key, v);
		}
		break;
	}
	case SET:
	case LIST: {
		guint8 header = take(r);
		guint8 item = r->binary_protocol ? binary_tag(header) :
						   header & 15;
		guint64 n = r->binary_protocol ? take_fixed(r, 4) : header >> 4;

		if (!r->binary_protocol && n == 15)
			n = number(r);
		if (n > MAX_VALUES || r->bad)
			break;
		g_autoptr(J) list = json_object_new_array_ext(MIN(n, 128));
		for (guint64 i = 0; i < n; i++) {
			J *v = read_value(r, item, depth + 1, FALSE);

			if (!v)
				return NULL;
			json_object_array_add(list, v);
		}
		return g_steal_pointer(&list);
	}
	case MAP: {
		guint8 key_tag, value_tag;
		guint64 n;

		if (r->binary_protocol) {
			key_tag = binary_tag(take(r));
			value_tag = binary_tag(take(r));
			n = take_fixed(r, 4);
		} else {
			n = number(r);
			guint8 types = n ? take(r) : 0;

			key_tag = types >> 4;
			value_tag = types & 15;
		}

		if (n > MAX_VALUES || r->bad)
			break;

		g_autoptr(J) map = obj();
		for (guint64 i = 0; i < n; i++) {
			g_autoptr(J) key =
				read_value(r, key_tag, depth + 1, FALSE);
			if (!key)
				return NULL;
			J *v = read_value(r, value_tag, depth + 1, FALSE);

			if (!v)
				return NULL;
			json_object_object_add(map, json_object_get_string(key),
					       v);
		}
		return g_steal_pointer(&map);
	}
	}
	r->bad = TRUE;
	return NULL;
}

J *line_decode_struct(const guint8 *bytes, gsize n, GError **error)
{
	struct thrift_reader r = { .p = bytes, .end = bytes + n };

	g_autoptr(J) fields = read_value(&r, STRUCT, 0, TRUE);
	if (fields && !r.bad && r.p == r.end)
		return g_steal_pointer(&fields);
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Invalid Compact Thrift structure.");
	return NULL;
}

/* The last service exception this thread decoded, for its payload. */
static __thread J *last_exception;

J *line_service_exception(void)
{
	return last_exception;
}

static J *read_reply(struct thrift_reader *r, const char *method,
		     gboolean exception_kind, int *code, GError **error)
{
	g_autoptr(J) result = read_value(r, STRUCT, 0, TRUE);

	if (!result || r->bad || r->p != r->end) {
		g_set_error(error, LINE_ERROR, 1, "Invalid Thrift reply to %s.",
			    method);
		return NULL;
	}
	if (exception_kind) {
		int type = json_object_get_int(field(result, "2"));

		g_set_error(
			error, LINE_ERROR,
			type == 1 ? LINE_METHOD_UNAVAILABLE : 1,
			"LINE returned an application exception for %s (type %d).",
			method, type);
		return NULL;
	}
	J *success = field(result, "0");

	if (success)
		return json_object_get(success);
	if (!json_object_object_length(result))
		return obj();
	J *exception = field(result, "1");
	J *id = field(exception, "1");

	if (exception)
		last_exception = json_object_get(exception);
	const char *message = strfield(exception, "2");

	g_autofree char *reason =
		message ? g_ascii_strdown(message, MIN(256, strlen(message))) :
			  NULL;
	if (reason && (strstr(reason, "api method not capable") ||
		       strstr(reason, "unknown method"))) {
		g_set_error(error, LINE_ERROR, LINE_METHOD_UNAVAILABLE,
			    "LINE does not offer %s for this session.", method);
		return NULL;
	}
	if (json_object_is_type(id, json_type_int)) {
		*code = json_object_get_int(id);
		g_set_error(error, LINE_ERROR, 1,
			    "LINE declined %s (service code %d).", method,
			    *code);
	} else {
		g_set_error(error, LINE_ERROR, 1, "LINE declined %s.", method);
	}
	return NULL;
}

static gboolean reply_name(struct thrift_reader *r, const char *method)
{
	g_autoptr(J) name = read_value(r, BINARY, 0, FALSE);
	size_t n = strlen(method);

	return name && (gsize)json_object_get_string_len(name) == n &&
	       !memcmp(json_object_get_string(name), method, n);
}

J *line_decode_reply(const char *method, const guint8 *bytes, gsize n,
		     int *code, GError **error)
{
	g_clear_pointer(&last_exception, json_object_put);
	struct thrift_reader r = { .p = bytes, .end = bytes + n };
	guint8 protocol = take(&r), kind = take(&r);

	/* One request per HTTPS exchange; method matching correlates the reply. */
	guint64 sequence = number(&r);
	gboolean matching = reply_name(&r, method);

	if (protocol == 0x82 && (kind == 0x41 || kind == 0x61) && !r.bad &&
	    matching)
		return read_reply(&r, method, kind == 0x61, code, error);
	if (!g_strcmp0(g_getenv("LINE_DEBUG_SESSION"), "1"))
		g_printerr(
			"line-session invalid reply to %s: %zu bytes, protocol 0x%02x, kind 0x%02x, sequence %llu, method match %s\n",
			method, n, protocol, kind, (unsigned long long)sequence,
			matching ? "yes" : "no");
	g_set_error(error, LINE_ERROR, 1, "Unexpected protocol reply to %s.",
		    method);
	return NULL;
}

J *line_decode_auth(const char *method, const guint8 *bytes, gsize n, int *code,
		    GError **error)
{
	g_clear_pointer(&last_exception, json_object_put);
	struct thrift_reader r = {
		.p = bytes,
		.end = bytes + n,
		.binary_protocol = TRUE,
	};
	guint64 kind = take_fixed(&r, 4);

	if ((kind == 0x80010002 || kind == 0x80010003) &&
	    reply_name(&r, method) && take_fixed(&r, 4) == 0)
		return read_reply(&r, method, kind == 0x80010003, code, error);
	g_set_error(error, LINE_ERROR, 1, "Unexpected protocol reply to %s.",
		    method);
	return NULL;
}
