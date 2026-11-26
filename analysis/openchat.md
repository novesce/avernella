# LINE OpenChat (`SquareService`) and LINE link protocol notes

These notes record the wire protocol recovered from supplied LINE Android packages and implemented in `src/square.c`, `src/obs_http.c`, `src/groups.c`, `src/liff_url.c`, and `src/ui_openchat.c`.

## Evidence sources

- **LINE Android 26.16.0** (`LINE.apk`, SHA-256 `5432c8ad902609b4ce4773d834206da63f5c441610346bec0249235116b2e4c5`):
  - Endpoint routing: `ea0.a` (`SQUARE` → `/SQ1`, 30 s timeout).
  - MID prefixes: `iu8.md` (`USER = 0` `'u'`, `ROOM = 1` `'r'`, `GROUP = 2` `'c'`, `SQUARE = 3` `'s'`, `SQUARE_CHAT = 4` `'m'`, `SQUARE_MEMBER = 5` `'p'`).
  - Thrift structs & methods: `classes12.dex` (`com.linecorp.square.protocol.thrift.*`, `SquareService`, `SquareEventType`, `SquareErrorCode`, `SquareException`).
  - Object Storage (OBS) paths: `ar8.b` (`/r/g2/m/<msgId>`, `/r/g2/member/<oid>`, `/r/g2/group/<oid>`, and `profile.line-scdn.net/<obsHash>`).
- **LINE Android 26.19.0** (`LINE_26.19.0.xapk`, `classes14.dex`):
  - Group invite-link lookup & join: `jm8.t9` (`FindChatByTicketRequest`: `{1: ticketId}` → `findChatByTicket` on `/S4`) and `jm8.e` (`AcceptChatInvitationByTicketRequest`: `{1: reqSeq, 2: chatMid, 3: ticketId}` → `acceptChatInvitationByTicket` on `/S4`).

## `SquareService` (`/SQ1`) conventions

Every `SquareService` RPC on `/SQ1` wraps its request struct in argument field `1:obj` (`*_args { 1: <Request> request }`). Responses return the response struct in field `0`, or a `SquareException` in field `1`:

| `SquareException` field | Type | Meaning |
| --- | --- | --- |
| `1` | `i32` (`SquareErrorCode`) | `400` illegal argument, `401` auth failure, `403` forbidden, `404` not found, `409` revision mismatch, `410` precondition failed, `500` internal, `501` not implemented, `503` try again later, `505` maintenance, `506` no presence |
| `2` | `struct` | `errorExtraInfo` |
| `3` | `string` | Human-readable reason |

Because `SquareException` uses a separate numeric namespace from `TalkException`, `src/square.c` reads `s->service_code` and `line_service_exception()`, maps the error, and immediately resets `s->service_code = -1`.

## Implemented RPCs on `/SQ1`

| Area | Methods |
| --- | --- |
| Directory & detail | `getJoinedSquares`, `getJoinedSquareChats`, `getJoinableSquareChats`, `getSquare`, `getSquareChat`, `getSquareCategories`, `getPopularKeywords`, `searchSquares` |
| History & live sync | `fetchSquareChatEvents` (`BACKWARD = 2` for history & older pages), `fetchMyEvents` (event-stream polling with `subscriptionId` and `syncToken`) |
| Messaging | `sendMessage` (including sticker, location, and contact forwarding), `searchSquareChatMentionables` (inline `@` mention autocomplete), `unsendMessage`, `destroyMessage` (admin/co-admin delete), `reactToMessage`, `markAsRead` |
| Announcements | `getSquareChatAnnouncements`, `createSquareChatAnnouncement`, `deleteSquareChatAnnouncement` |
| Joining & creation | `findSquareByInvitationTicketV2` (with `findSquareByInvitationTicket` V1 fallback), `checkJoinCode`, `joinSquare`, `joinSquareChat`, `leaveSquare`, `leaveSquareChat`, `createSquare`, `createSquareChat`, `getInvitationTicketUrl` |
| Settings, profile & moderation | `updateSquare`, `updateSquareChat`, `updateSquareMember`, `searchSquareMembers`, `getSquareMember`, `approveSquareMembers`, `rejectSquareMembers` |

## Media & avatars (`/r/g2`)

- OpenChat message attachments are **not** Letter-Sealed (Square chats do not use E2EE).
- Uploads send a plaintext placeholder `Message` (`IMAGE=1`, `VIDEO=2`, `AUDIO=3`, `FILE=14`) via `SquareService.sendMessage`, then POST the raw payload to `https://obs.line-apps.com/r/g2/m/<messageId>` with `x-line-access` and `x-obs-params`.
- Downloads fetch `https://obs.line-apps.com/r/g2/m/<messageId>` (and `/preview` for video thumbnails).
- OpenChat member/group avatar uploads POST a 640×640 baseline JPEG to `https://obs.line-apps.com/r/g2/member/<memberMid>` (or `/r/g2/group/<oid>`), read the returned `x-obs-hash` response header, and persist it via `updateSquareMember` (`PROFILE_IMAGE = 2`).
- OpenChat, room, and member avatars download from `https://profile.line-scdn.net/<obsHash>` through the existing anonymous avatar client.

## LINE links & desktop URI handling

- `data/avernella.desktop` registers `Exec=avernella %u` and `MimeType=x-scheme-handler/line;x-scheme-handler/lineb;`.
- Passing a URL to a second `avernella` launch forwards `U<url>` over the single-instance Unix `SOCK_DGRAM` socket (`src/ui_instance.c`) to the primary window, which routes it immediately (or after chats load if signing in).
- Message bubbles highlight and underline `https://`, `http://`, `line://`, and `lineb://` spans (`src/ui_draw.c`), and clicking any chat, Flex, rich-message, or desktop-opened link routes through `src/ui_liff.c`:
  - OpenChat invite links (`line://ti/g2/<ticket>`, `lineb://ti/g2/<ticket>`, `https://line.me/ti/g2/<ticket>`, `https://line.me/R/ti/g2/<ticket>`) → OpenChat Join dialog (`findSquareByInvitationTicketV2`).
  - Group invite links (`line://ti/g/<ticket>`, `https://line.me/ti/g/<ticket>`, `https://line.me/R/ti/g/<ticket>`) → Group Join dialog (`findChatByTicket` + `acceptChatInvitationByTicket`).
  - Personal friend links (`line://ti/p/<ticket>`, `https://line.me/ti/p/<ticket>`, `https://line.me/R/ti/p/<ticket>`) → Friends → Add friend lookup.
  - Chat deep links (`chatId=<mid>`) → selects the conversation if present in the chat list.
  - LIFF URLs (`liff.line.me`, `miniapp.line.me`, `line.me/R/app/`, `line://app/`) → `LiffService.issueLiffView` (passing `LiffContext` field `3:obj` `squareChat` when opened from an `'m'` chat).
  - General `http://` / `https://` chat links → system default browser (`SDL_OpenURL`).

## Verification status

All Thrift encodings, response parsers, error-code resets, ticket parsers, OBS `/r/g2` paths, and single-instance URL forwarding are covered by offline unit tests (`tests/square.c`, `tests/obs_http.c`, `tests/ui_instance.c`). Live testing against a signed-in personal LINE account has not yet been performed in this branch.
