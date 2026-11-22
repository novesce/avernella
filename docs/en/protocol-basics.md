# LINE protocol basics

[Index](README.md) · Previous: [The user interface](ui.md) · Next: [Signing in](sign-in.md)

This page explains how *every* request to LINE is built, sent and answered. The later protocol pages assume it.

LINE doesn't document any of this. It was recovered from LINE for Android 26.16.0 and cross-checked against open interoperability projects (see [Contributing](contributing.md#how-the-protocol-was-recovered)). The client presents itself as LINE's **Windows desktop app**, version 9.7.0.3556.

## Servers

| Host | Used for | Code |
| ---- | -------- | ---- |
| `legy.line-apps.com` | Every RPC: sign-in, sync, messages, contacts, groups, calls, LIFF, shop, channel tokens, albums and notes | `transport.c`, `shared_content.c` |
| `obs.line-apps.com` | Object storage: attachments, previews, profile picture upload | `obs_http.c` |
| `profile.line-scdn.net` | Profile pictures (public) | `line_avatar_download()` |
| `stickershop.line-scdn.net` | Sticker and emoji artwork and pack manifests (public) | `stickers.c`, `emoji.c`, `line_public_asset()` |
| Any public HTTPS host | Flex and rich-message pictures chosen by the sender | `flex_asset.c` |
| Call relays (address from the call route) | Call signalling and media over UDP | `call.c` |

## Service paths

Every RPC is an HTTP POST to `https://legy.line-apps.com<path>`. The path selects the service.

| Path | Service | Methods the client calls |
| ---- | ------- | ------------------------ |
| `/S4` | TalkService | `getProfile`, `getMessageBoxes`, `getChats`, `getAllChatMids`, `getRecentMessagesV2`, `getPreviousMessagesV2WithRequest`, `sendMessage`, `sendChatChecked`, `react`, `unsendMessage`, `createChat`, `updateChat`, `inviteIntoChat`, `cancelChatInvitation`, `acceptChatInvitation`, `rejectChatInvitation`, `deleteOtherFromChat`, `deleteSelfFromChat`, `reissueChatTicket`, `getContactsV2`, `getFriendRequests`, `removeFriendRequest`, `updateProfileAttributes`, `getUserTicket`, `negotiateE2EEPublicKey`, `getE2EEPublicKey`, `getLastE2EEGroupSharedKey`, `getE2EEGroupSharedKey`, `getChatRoomAnnouncements`, `createChatRoomAnnouncement`, `removeChatRoomAnnouncement`, `acquireEncryptedAccessToken`, `getLastOpRevision` |
| `/RE4` | RelationService | `getContactsV3`, `getUserFriendIds`, `getUserRecommendIds`, `findContactBySearchIdOrTicketV3`, `findContactByPhoneV3`, `addFriendByMid`, `updateFriendFavorite`, `updateFriendHidden`, `updateUserFriendOverridenName`, `blockRelation`, `unblockRelation` |
| `/SYNC4` | SyncService | `sync` |
| `/AS4` | AuthService | `logoutV2` |
| `/acct/lgn/sq/v1` | Secondary QR login | `createSession`, `createQrCodeForSecure`, `verifyCertificate`, `createPinCode`, `qrCodeLoginV2ForSecure` |
| `/acct/lp/lgn/sq/v1` | Secondary QR login, long-poll side | `checkQrCodeVerified`, `checkPinCodeVerified` |
| `/api/v3/TalkService.do` | Email login, step 1 (Thrift **Binary**) | `getRSAKeyInfo` |
| `/api/v3p/rs` | Email login (Thrift **Binary**) | `loginV2`, `confirmE2EELogin` |
| `/LF1` | Phone verification wait (JSON, not Thrift) | — |
| `/EXT/auth/tokenrefresh/v1` | Token renewal | `refresh` |
| `/TSHOP4` | ShopService | `getOwnedProductSummaries` |
| `/CH4` | ChannelService | `approveChannelAndIssueChannelToken` |
| `/V4` | CallService | `acquireCallRoute` |
| `/LIFF1` | LiffService | `issueLiffView` |

Albums and notes are REST endpoints under `legy.line-apps.com/ext/…` with a channel token, described in [Media](media.md#albums-notes-and-polls).

## Thrift Compact encoding

LINE's RPCs are [Apache Thrift](https://thrift.apache.org/) calls in the **Compact protocol**. [`thrift.c`](../../src/thrift.c) implements the subset LINE needs, in about 600 lines, with no Thrift library.

### A request on the wire

`line_encode_call(method, args)` writes:

| Bytes | Meaning |
| ----- | ------- |
| `0x82` | Compact protocol ID |
| `0x21` | Version 1 (low 5 bits) and message type CALL = 1 (high 3 bits) |
| varint `0` | Sequence ID. Always 0: one request per HTTP exchange. |
| varint length + bytes | Method name, such as `sendMessage` |
| struct | The method's argument struct (`<method>_args`) |

A **struct** is a list of fields followed by a stop byte `0x00`. Each field starts with a header:

- If the field ID is 1 to 15 higher than the previous one, one byte holds both: `(delta << 4) | type`.
- Otherwise the type byte is followed by the field ID as a zigzag varint.

| Type | Compact ID | Value encoding |
| ---- | ---------- | -------------- |
| bool | 1 (true) / 2 (false) | In a field, the value *is* the type. In a list, one byte. |
| byte | 3 | One byte |
| i16, i32, i64 | 4, 5, 6 | Zigzag varint |
| double | 7 | 8 bytes, little-endian |
| binary / string | 8 | Varint length, then bytes |
| list, set | 9, 10 | One byte `(count << 4) | element type`, with count 15 meaning "a varint count follows", then the elements |
| map | 11 | Varint count, then (if non-zero) one byte `(key type << 4) | value type`, then key, value, key, value… |
| struct | 12 | Fields, then `0x00` |

### A reply on the wire

The reply starts `0x82`, then `0x41` (REPLY) or `0x61` (EXCEPTION), the sequence ID 0, and the method name, which must match. `line_decode_reply()` checks all four. Then comes the **result struct**:

- **Field 0** holds the success value. A `void` method's success is an empty struct.
- **Field 1** holds the service's exception, typically a `TalkException` whose field 1 is an error code and field 2 a message. Some services add a payload in field 3.

An EXCEPTION message (`0x61`) is a Thrift application exception: "unknown method" and similar. It becomes `LINE_METHOD_UNAVAILABLE` (type 1) or a generic error.

### The JSON form the code uses

Rather than generated structs, the code builds and reads Thrift as **json-c objects**. This is the most important convention in the protocol code.

**Writing.** Request keys are `"<field id>:<type>"`, and the encoder takes the type from the key:

```c
g_autoptr(J) args = obj();             /* sendMessage_args */
J *msg = obj();                         /* Message */
number(args, "1:i32", ++s->seq);        /* field 1: request sequence */
put(args, "2:obj", msg);                /* field 2: the message */
put(msg, "2:str", string(chat));        /* Message.to */
number(msg, "15:i32", 0);               /* Message.contentType = NONE (text) */
put(msg, "18:map:str:str", meta);       /* Message.contentMetadata */
put(msg, "20:list:str", chunks);        /* Message.chunks */
```

(`number()`, `put()` and `string()` are small static helpers in `messaging.c`.)

| Key type | Thrift type |
| -------- | ----------- |
| `bool` | bool |
| `byte`, `i16`, `i32`, `i64` | integers |
| `double` | double |
| `str` | string or binary. Binary data goes in with `json_object_new_string_len()`, so it may contain NUL bytes. |
| `obj` | struct |
| `list:<type>`, `set:<type>` | list or set of that element type, such as `list:str`, `set:str` |
| `map:str:<type>`, `map:i32:<type>` | map with string or i32 keys |

Fields are written in the order they were added. That doesn't have to be ascending, because the encoder falls back to the long field header.

**Reading.** Decoded structs are JSON objects keyed by the **decimal field ID only**, with no type: `"1"`, `"4"`, `"20"`. Lists become arrays, maps become objects (keys are converted to strings), binaries become strings that may contain NUL bytes (read them with `json_object_get_string_len()`). The helpers in [`line.h`](../../src/line.h) read them safely:

```c
const char *id   = strfield(message, "4");          /* NULL unless it is a string */
J *meta          = field(message, "18");            /* NULL unless present */
gint64 type      = json_object_get_int64(field(message, "15"));
```

`field()` returns `NULL` for anything missing or when the parent isn't an object, so chains such as `field(field(op, "20"), "18")` are safe.

**Limits.** The decoder rejects nesting deeper than 48 levels, more than 300,000 values in total, duplicate field IDs, and trailing bytes after the struct. Untrusted replies can't exhaust memory or the stack.

### Thrift Binary, for email login only

The email login endpoints (`/api/v3/TalkService.do` and `/api/v3p/rs`) use the older **Binary protocol**. `line_encode_auth()` and `line_decode_auth()` handle it: a 4-byte version word `0x80010001` (CALL), the method name with a 4-byte length, a 4-byte sequence ID, then fields as a 1-byte type, a 2-byte big-endian ID and fixed-width values. It supports only the types those methods use (bool, i32, string, struct). Replies carry `0x80010002` (REPLY) or `0x80010003` (EXCEPTION).

## The HTTP exchange

`exchange()` in [`transport.c`](../../src/transport.c) sends every RPC. The three public wrappers are:

| Function | Protocol | Used for |
| -------- | -------- | -------- |
| `line_rpc(s, path, method, args, &error)` | Compact, with the session's access token and a 30 s timeout. Retries once after renewing an expired token. | Almost everything |
| `line_call(s, path, method, args, access, poll_seconds, &error)` | Compact, with an explicit token (or none) and an optional long-poll time | Pairing, sync, token refresh |
| `line_auth_call(s, path, method, args, &error)` | Binary, no token | Email login |
| `line_auth_wait(s, verifier, &error)` | JSON GET on `/LF1`, 180 s | Waiting for phone verification during email login |

### Request headers

```
POST /S4 HTTP/2
Host: legy.line-apps.com
x-line-application: DESKTOPWIN	9.7.0.3556	WINDOWS	10.0.0-NT-x64
x-lal: <locale>                        (th_TH, ja_JP…; en_US for C/POSIX)
x-lpv: 1
x-lhm: POST
x-line-access: <access token>          (when signed in)
x-lst: <milliseconds>                  (long polls only)
Accept: application/x-thrift
Content-Type: application/x-thrift
Accept-Encoding: gzip
User-Agent: Line/9.7.0.3556
```

The fields of `x-line-application` are separated by tab characters. `LINE_APP_HEADER` and `LINE_USER_AGENT` in `line.h` hold the client identity, so every request (RPCs, object storage, albums and notes) names the same client version. `x-lal` is the desktop locale from `line_language_tag()` in `line.h`, the same tag LIFF views send as their `lang`, so a Thai desktop says `th_TH` everywhere, as LINE's own client does. `x-lst` tells LINE how long it may hold a long-poll request open. The `/LF1` approval wait uses `x-lhm: GET` and `Accept: application/json` instead.

### Transport rules

- **HTTPS only.** Redirects aren't followed, the connection timeout is 10 s, and the body is capped at 16 MiB.
- **One connection pool per thread.** Each thread keeps its own curl handle in thread-local storage, resets its options after each transfer and reuses the connection. The sync thread's long poll never blocks an RPC.
- **Shared caches.** DNS answers and TLS sessions are shared across threads ([`http_tune.c`](../../src/http_tune.c)). TLS 1.2 or newer, certificates and host names verified, HTTP/2 negotiated, TCP Fast Open and keepalive on.
- **Short revision checks.** `getLastOpRevision` has a 1.8 s timeout, because it is only a quick probe.

### Rotated tokens

A reply may carry the header `x-line-next-access: <token>`. That is LINE rotating the access token. `exchange()` adopts it only when all of these hold: the request was not the refresh call, it used the session's current token, the reply was a real reply (success or service exception), exactly one valid printable token was offered, and it differs from the current one. It then sets `s->token_changed`, and the worker saves the session after the request. The sync thread forwards rotations it sees to the worker in its batch.

## Errors

`exchange()` and the decoder turn every failure into a `GError` in the `LINE_ERROR` domain. The codes are in [`line.h`](../../src/line.h):

| Code | Meaning | Typical cause |
| ---- | ------- | ------------- |
| `1` | Generic failure | A service exception, a malformed reply, an HTTP error |
| `LINE_TIMEOUT` (2) | Timed out | Network, or an idle long poll. Also HTTP 408, 504, and 410 on an approval poll. |
| `LINE_METHOD_UNAVAILABLE` (3) | LINE doesn't offer this method here | "api method not capable", "unknown method" |
| `LINE_SESSION_EXPIRED` (4) | The session can't continue; pair again | Token expired and renewal rejected |
| `LINE_OBS_UNAUTHORIZED` (5) | The object-storage token was rejected | It is dropped and fetched again next time |
| `LINE_ASSET_NOT_FOUND` (6) | HTTP 404 from a CDN or OBS | A missing preview or animation |
| `LINE_OWN_KEY_MISSING` (7) | This device lacks a Letter Sealing key generation | Old message, new device |
| `LINE_RETRY` (8) | A saved session couldn't reach LINE | Offline at launch: keep the session and retry |
| `LINE_RATE_LIMITED` (9) | LINE asked to wait | HTTP 429 or 503, TalkException 4, 58 or 101, or a host still cooling down |
| `LINE_ACCESS_DENIED` (10) | LINE refused the request's credentials | HTTP 401 or 403 |

Separately, `s->service_code` holds **LINE's own error number** from the last reply's exception field 1, or -1. Callers inspect it for specific cases. Some TalkException codes the client relies on:

| Code | Where | Meaning to the client |
| ---- | ----- | --------------------- |
| 119 | Any `/S4` call | Access token expired: renew and retry (`line_rpc()`) |
| 1, 7, 8, 14, 17 | `getProfile`, `getLastOpRevision` | The session was rejected: forget it (`session_rejected()` in `main.c`) |
| 1000 | `/EXT/auth/tokenrefresh/v1` | The refresh token is unusable: sign out |
| 1001 | `/EXT/auth/tokenrefresh/v1` | Try again later |
| 18, 19, 21, 23 | `loginV2` | Wrong email or password |
| 1, 2 | `verifyCertificate` | No certificate: a PIN is needed |

Other services, such as RelationService, ShopService and the refresh endpoint, have their **own** code numbers. Code that calls them resets `s->service_code` to -1 afterwards, so a relation code isn't mistaken for a Talk code (see `relation()` in `friends.c`).

## Rate limits

Every HTTP path that reaches LINE shares one table of **per-host cooldowns** (`line_cooldown_*()` in [`http_tune.c`](../../src/http_tune.c)): RPCs in `transport.c`, object storage and the CDNs in `obs_http.c`, albums and notes in `shared_content.c`, and card artwork in `flex_asset.c`.

- **Starting one.** An HTTP 429 or 503 sets a cooldown for that host from `Retry-After`, or 30 s without it (`line_cooldown_note()`). A TalkException `EXCESSIVE_ACCESS` (4), `CONGESTION_CONTROL` (58) or `SERVER_BUSY` (101) on `/S4` or `/SYNC4` sets 30 s for `legy.line-apps.com`.
- **Honoring it.** Until it passes, every request to that host fails at once with `LINE_RATE_LIMITED`, without touching the network (`line_cooldown_check()`). Other hosts carry on: a busy object store doesn't stop chat.
- **Who waits.** The maintenance timer (`sync_revision()`) and the long-poll thread wait at least as long as legy's cooldown (`line_transport_retry_after()`). Nothing retries a refused request automatically; the user can try again once the time has passed.
- **No "try something else".** A 429 on an imagemap picture is not followed by the plain-URL fallback (`line_flex_image_download()`).

This follows LINE for Android 26.x: its HTTP interceptor (`ti0.d`) retries a 429 only when `Retry-After` is under 5 s, and its object-storage client (`a14.g`) retries once, and only after a 401 or 503.

## When LINE refuses the session

LINE for Android signs a linked device out on the first TalkException `AUTHENTICATION_FAILED` (1), `NOT_AVAILABLE_USER` (7) or `NOT_AUTHORIZED_DEVICE` (8) (`ac5.e`, `ac5.b`), and afterwards refuses locally to send anything with that token (`getClient()` in its Thrift client). Avernella does the same:

- **The latch.** When a `/S4` or `/SYNC4` request made with the session's own token gets one of `line_auth_rejected()`'s codes (1, 7, 8, 14, 17), or the refresh token is refused, `exchange()` sets `s->denied`. From then on any request with that token fails locally with `LINE_SESSION_EXPIRED`. Pairing calls, which use their own temporary token, are unaffected.
- **Signing out.** After a request, `handle()` in `main.c` sees the latch and calls `end_session()`: forget the session, send `signed_out`, return the window to sign-in. The window never signs in again on its own.
- **The long poll.** The `line-sync` thread has its own copy of the token. On a refusal, or HTTP 401 or 403, it reports `denied` once and stops polling. The worker confirms once with `checked_profile()`: a working session restarts the long poll, a refused one signs out, and anything else waits 15 s (network) or 5 minutes (another LINE answer) before the next check.
- **HTTP 401 or 403.** As LINE's interceptor does, `checked_profile()` renews the token once and asks once more. It never loops.

## Token renewal

LINE issues the access token with two numbers: `durationUntilRefreshInSec` and `tokenIssueTimeEpochSec`. `line_schedule_refresh()` stores their sum as `s->refresh_at` and saves it with the session. It ignores implausible values: durations under a minute or over ten years, and issue times more than a year in the past or a day in the future.

Renewal happens in two ways:

1. **On schedule.** `maintain_tokens()` in `main.c`, called from the maintenance timer, calls `line_refresh_session()` when `line_refresh_due()` says so. An unknown schedule (an older saved session) renews at once. A failure retries with exponential backoff (`line_retry_delay()`), up to hourly, and never signs out by itself.
2. **On demand.** When any `line_rpc()` call fails with TalkException 119, it calls `refresh_access()` once and repeats the same request with the same arguments. For a send, that means the same request sequence and the same encrypted chunks.

`refresh_access()` calls `refresh` on `/EXT/auth/tokenrefresh/v1` with `{1: {1: refresh token}}`. The reply has the new access token in field 1, its duration in field 2, the issue time in field 4 and, optionally, a new refresh token in field 5. Without a duration hint, it renews daily so the refresh token stays in use.

Only an explicit rejection ends the session: refresh code 1000, or a 119 that persists after renewal. Network errors keep the session.
