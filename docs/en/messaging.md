# Messages

[Index](README.md) · Previous: [Sync](sync.md) · Next: [Letter Sealing](letter-sealing.md)

This page covers the chat list, reading history, the Message struct, sending text, and everything you can do to a message. Encryption is on the [Letter Sealing](letter-sealing.md) page, and attachments on the [Media](media.md) page. Almost all of it is in [`messaging.c`](../../src/messaging.c), with the worker-side request handling in [`main.c`](../../src/main.c).

## Chat IDs

A chat is identified by a MID (see [Overview](overview.md#words-you-will-meet-everywhere)):

- a **one-to-one chat** by the *other person's* `u…` MID;
- a **group** by its `c…` MID;
- a **room** (an older kind of multi-person chat) by its `r…` MID;
- your own `u…` MID is your **Keep Memo** chat.

`talk_id()` in `messaging.c` validates the 33-character form. The worker keeps every chat ID it has seen in `w->chats` and refuses chat requests for any other ID, so the UI can't be tricked into acting on an arbitrary MID.

## The chat list

`line_chats()` builds the list from **message boxes**, LINE's per-chat delivery state, not from the full chat objects.

**Request.** `getMessageBoxes` on `/S4`, with `{2: {1: cursor, 3: true (activeOnly), 4: 100 (count), 5: true (withUnreadCount), 6: 0, 7: false}, 3: 7}`. The cursor is omitted on the first page and is the last box's chat ID afterwards. If the reply's field 2 ("has more") is true it fetches the next page. A cursor that repeats is an error ("LINE did not advance the chat list").

**Each box** (reply field 1 is the list) is read as:

| Field | Meaning |
| ----- | ------- |
| 1 | Chat ID |
| 4 | Last delivered message: field 1 its time, field 2 its ID |
| 5 | The last message ID you've seen (read up to) |
| 6 | Unread count |
| 8, 10 | "Removed" and "hidden" message IDs. A chat whose last message is at or below the larger of them was cleared or hidden, and is skipped. |

**Names.** The boxes carry no names. `line_resolve_names()` fills them in:

- users with `getContactsV3` on `/RE4` (or `getContactsV2` on `/S4` on older servers), cached in `s->profiles` for the session;
- groups and rooms with `getChats` on `/S4`, `{1: {1: [ids], 2: false, 3: false}}`, reading the name from chat field 6;
- your own chat is called "Keep Memo".

**Rows returned to the UI**, sorted newest first:

```json
{"id": "c…", "name": "Family", "time": 1764000000000, "unread": 3,
 "lastId": "5125…", "lastSeenId": "5120…", "avatarKey": "/0h…", "clearThrough": 0}
```

The worker's `chat_directory()` in `main.c` also remembers the rows, for validating chat IDs, unread heads and announcements.

## The Message struct

Messages from history and from sync use the same Thrift struct. The fields the client reads and writes:

| Field | Name | Notes |
| ----- | ---- | ----- |
| 1 | from | Sender MID |
| 2 | to | Recipient MID: a user, group or room |
| 3 | toType | 0 user, 1 room, 2 group |
| 4 | id | Message ID, a decimal string. IDs increase over time. |
| 5 | createdTime | Unix milliseconds |
| 6 | deliveredTime | Unix milliseconds. Used for history paging. |
| 10 | text | Plain text, for unencrypted messages |
| 11 | location | For location messages: field 2 is the address |
| 14 | hasContent | |
| 15 | contentType | See the next table |
| 18 | contentMetadata | `map<string,string>`: metadata that travels **outside** the encryption |
| 19 | sessionId | Sent as 0 |
| 20 | chunks | `list<binary>`: the Letter Sealing envelope |
| 21 | relatedMessageId | The message this one replies to |
| 22 | messageRelationType | 3 means a reply |
| 24 | relatedMessageServiceCode | 1 for a reply in the same Talk service |
| 27 | reactions | List of reactions. Each has field 3, whose field 1 is the reaction type. |

### Content types

`contentType` (field 15) values and the names the worker gives them in rows (`kinds[]` in `history_rows()`):

| Value | Name | Value | Name |
| ----- | ---- | ----- | ---- |
| 0 | text | 12 | link |
| 1 | photo | 13 | contact |
| 2 | video | 14 | file |
| 3 | audio | 15 | location |
| 4 | html | 16 | post |
| 5 | pdf | 17 | rich (imagemap) |
| 6 | call | 18 | event |
| 7 | sticker | 19 | music |
| 8 | presence | 20 | payment |
| 9 | gift | 21 | externalImage |
| 10 | board | 22 | flex |
| 11 | app | | |

Anything else is shown as "other".

### Metadata keys you'll meet

| Key | Used for |
| --- | -------- |
| `e2eeVersion` | `"1"` or `"2"`: the Letter Sealing version. Missing or empty means 2. |
| `contentType` | The content type again, as a string (`"0"` for text) |
| `e2eeMark` | `"2"` on messages this client seals |
| `MENTION` | Mentions, see below |
| `STICON_OWNERSHIP` | The LINE emoji packs used, see below |
| `REPLACE` | LINE emoji in *unencrypted* text |
| `SID`, `OID`, `FILE_SIZE`, `FILE_NAME`, `DURATION`, `MEDIA_CONTENT_INFO` | Attachments, see [Media](media.md) |
| `STKID`, `STKPKGID`, `STKVER`, `STKTXT`, `STKOPT` | Stickers |
| `FLEX_JSON`, `ALT_TEXT` | Flex Messages |
| `DOWNLOAD_URL`, `MARKUP_JSON`, `ALT_TEXT` (or their `RICH_…` aliases) | Rich messages |
| `displayName` | Contact cards |

## Reading history

### The latest page

`line_history_with_summary()` calls `getRecentMessagesV2` on `/S4` with `{2: chat, 3: 50}`. If LINE returns an empty page while the chat list says the chat has a latest message, it retries from that message's position with the older-page call. Some servers answer the recent call empty after a gap.

### Older pages

`line_history_older_since()` calls `getPreviousMessagesV2WithRequest` with `{2: {1: chat, 2: {1: deliveredTime, 2: message ID}, 3: 50}, 3: 7}`. The boundary is the oldest message on the current page (`line_history_cursor()` picks the smallest `deliveredTime`, then the smallest ID). Paging stops at the chat's "cleared through" ID, so history you cleared on the phone stays cleared.

### From Message to row

`history_rows()` turns raw Messages into rows the UI can draw:

```json
{"id": "5125…", "from": "Alice", "senderId": "u…", "own": false, "time": 1764000000000,
 "type": "text", "text": "hello", "emojis": [...], "replyTo": "5120…",
 "reactions": {"love": 2}, "avatarKey": "/0h…"}
```

On the way:

- **Text** (type 0): sealed text is opened with `line_open_payload()`. If this device lacks the key generation, the text becomes "[Letter Sealing key missing on this device]". Other failures become "[Unable to decrypt this message]". Both rows get `"unreadable": true`. LINE emoji ranges come from the payload's `REPLACE.sticon.resources` (or the metadata, for unencrypted text).
- **Flex** (22): `add_flex()` validates `FLEX_JSON` (at most 64 KiB) and puts it in the row. The text becomes the alternative text.
- **Rich** (17): `line_rich_parse()` ([`rich_message.c`](../../src/rich_message.c)) builds a compact layout. See [Media](media.md#rich-messages).
- **Video** (2): `DURATION` becomes `durationMs`.
- **Files and videos**: `FILE_NAME` becomes `filename`.
- **Location** (15), **contact** (13): the address or display name becomes the text.
- **Reactions**: counted by type.
- **Names**: senders are resolved through the profile cache, and your own messages say "You".

Rows are sorted by time, then ID. The raw Messages are also kept, keyed by ID, in `w->messages`. The worker needs them later to reply, react, forward, download attachments or build sticker URLs, and it never trusts the UI to send a message back.

### The history cache

The worker keeps recently opened chats' pages in an LRU cache (`history_cache` in `struct line_worker`). Reopening a chat answers from memory at once and then **revalidates** with LINE right after (`revalidate_history()`), emitting a `history` event if anything changed. A new message in a chat marks its cache entry stale, or drops it if the chat isn't open.

## Sending text

`send` with `{chat, text, replyTo?, emojis?, mentions?}` reaches `line_send_decorated()`:

1. **Validate.** The text must be non-empty UTF-8 of at most 20,000 bytes. `replyTo` must be a message in the open chat.
2. **Build the payload.** `{"text": "…"}`, plus, when there are LINE emoji, `{"REPLACE": {"sticon": {"resources": [...]}}}`.
3. **Seal it** with `line_seal_payload()` (content type 0). See [Letter Sealing](letter-sealing.md).
4. **Build the metadata**: `e2eeVersion: "2"`, `contentType: "0"`, `e2eeMark: "2"`, plus `MENTION` and `STICON_OWNERSHIP` when used.
5. **Send** with `line_send_sealed()`: `sendMessage` on `/S4` with

   ```
   {1: reqSeq,
    2: {2: chat, 5: 0, 6: 0, 14: false, 15: contentType, 18: metadata,
        19: 0, 20: chunks,
        21: replyTo, 22: 3, 24: 1 (replies only)}}
   ```

6. **Read the reply**: the new message ID is in field 4 and its time in field 5. No ID means "Check the chat before resending": the message may or may not have been delivered, so the client doesn't retry on its own.

### LINE emoji in text

The UI converts each emoji in the draft into a range. Positions are counted in **UTF-16 code units**, as LINE's apps count them (`utf16_units()` in `ui.c`). In the sealed payload each resource is:

```json
{"S": 0, "E": 2, "productId": "670e0cce840a8236ddd4ee4c", "sticonId": "001",
 "version": 1, "resourceType": "STATIC"}
```

`S` and `E` are the start and end offsets, and the text at that range is the emoji's alternative label, so clients without the picture still show something sensible. `STICON_OWNERSHIP` in the plain metadata lists the product IDs used. `emoji_ranges()` validates every range against the text before sending and after receiving.

### Mentions

In groups, `mentions` is a list of `{id, start, length}` in UTF-16 units, each pointing at an `@Name` in the text. `mention_metadata()` checks each one (at most 20, each a `u…` MID, each starting with `@`) and produces:

```json
{"MENTIONEES": [{"S": "0", "E": "6", "M": "u…"}]}
```

It is stored as the `MENTION` metadata value. **Note:** metadata isn't encrypted, so LINE's servers can see who was mentioned and where, as with LINE's own apps.

## Reactions and unsend

`line_message_action()`:

- **React:** `react` on `/S4` with `{1: {1: reqSeq, 2: message ID (i64), 3: {1: type}}}`. Types: 1 none (remove), 2 nice, 3 love, 4 fun, 5 amazing, 6 sad, 7 omg. The UI labels them Like, Love, Laugh, Amazing, Sad and Surprised.
- **Unsend:** `unsendMessage` with `{1: reqSeq, 2: message ID}`. The worker allows it only for your own messages.

## Read receipts

`line_mark_read()` calls `sendChatChecked` on `/S4` with `{1: reqSeq, 2: chat, 3: last message ID}`, omitting the optional session ID as the desktop app does. It refuses your own Keep Memo chat. The UI decides when to send it (see [UI: read receipts](ui.md#read-receipts)), and the worker keeps receipt targets monotonic (`read_acks`, `unread_heads`), so a late acknowledgement never moves a receipt backwards.

## Forwarding

`forwardMessage` takes a message ID from the open chat and a destination chat. `forward_message()` in `main.c` allows text and the four attachment types:

- **Text** is re-sent with `line_forward_text()`: opened and sealed again for the new chat, with LINE emoji preserved.
- **Attachments** are downloaded, decrypted, and re-uploaded with a fresh key for the destination (`line_forward_attachment()` in `media.c`). A key is never shared between chats.

## Announcements

Announcements are pinned notes at the top of a chat. They live on LINE's servers **outside Letter Sealing**, so the UI warns before creating one.

| Action | RPC on `/S4` | Arguments |
| ------ | ------------ | --------- |
| List | `getChatRoomAnnouncements` | `{2: chat}`. Each entry: 1 ID, 3 contents (field 2 text), 4 creator, 6 permission (0 anyone may remove, 1 only the creator). |
| Create | `createChatRoomAnnouncement` | `{1: reqSeq, 2: chat, 3: 0, 4: {1: 5, 2: text, 3: "line://nv/chatMsg?chatId=<chat>&messageId=<id>"}}` |
| Remove | `removeChatRoomAnnouncement` | `{1: reqSeq, 2: chat, 3: announcement ID (i64)}` |

## Searching

There is no server-side search API for personal chats, so search runs over pages the worker fetches:

- **In one chat** (`findMessages`, `find_messages()` in `main.c`): searches the open chat's pages, newest first, case-insensitively (`search_text()`), and can continue into older pages, remembering the boundary between calls.
- **Across chats** (`findAllMessages`, `find_all_messages()`): takes the latest 50 messages of up to six chats per call, within a 350 ms budget, and continues from where it stopped on the next call.

## Things to keep in mind

- **Always reserve a sequence before a mutating RPC.** See [Signing in](sign-in.md#the-request-sequence).
- **Never trust the UI's copy of a message.** Look the raw Message up in `w->messages` by ID.
- **Metadata is public; chunks are private.** Anything confidential goes in the sealed payload, not in `contentMetadata`.
- **Offsets for emoji and mentions are UTF-16 units**, not bytes or characters.
