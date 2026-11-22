# Sync: how changes arrive

[Index](README.md) · Previous: [Signing in](sign-in.md) · Next: [Messages](messaging.md)

LINE doesn't push messages to a desktop over a socket. Instead the desktop asks "what changed since revision N?", and LINE holds the question open until there is an answer. This page explains that loop and what the client does with the answers.

## Operations and revisions

LINE describes every change to an account as an **operation**: a message received, a message sent from another device, a profile change, a new friend, an incoming call. Operations are numbered by a **revision** that only increases. A client keeps a cursor, the highest revision it has applied, and asks for everything after it.

An Operation struct, as the client reads it:

| Field | Meaning |
| ----- | ------- |
| 1 | revision (i64) |
| 2 | created time, Unix milliseconds |
| 3 | type (the OpType number, see [the table below](#operation-types-the-client-handles)) |
| 10, 11, 12 | param1, param2, param3: strings whose meaning depends on the type. For profile operations param1 names the person. For calls they carry the caller, a token and a JSON description. |
| 20 | the Message, for message operations |

## The cursor

[`talk_sync.c`](../../src/talk_sync.c) keeps the cursor in `struct line_talk_sync`:

| Member | Meaning |
| ------ | ------- |
| `revision` | The last applied operation revision. -1 before the baseline is known. |
| `global_revision`, `individual_revision` | Two further cursors LINE returns and expects back |
| `partial` | The target categories of a pending partial resync, or `NULL` |
| `more` | LINE said more operations are waiting |

`line_talk_sync_request()` builds the `sync` arguments: `{1: {1: revision, 2: 100 (count), 3: global revision, 4: individual revision, 6: partial categories (map<i32, i64>)}}`.

The reply is a union with exactly one of three arms. `line_talk_sync_apply()` handles each:

| Arm | Contents | What the client does |
| --- | -------- | -------------------- |
| 1, operations | Field 1 a list of operations, 2 "has more", 3 and 4 the global and individual events (field 2 of each is the new cursor) | Sorts the operations by revision if LINE didn't, applies each one newer than the cursor in order, advances the cursor, and takes the larger global and individual revisions. If "has more" is set and progress was made, it asks again at once. |
| 2, full resync | Field 2 the next revision | Jumps the cursor forward and sets `reload`. The UI reloads the chat list and the open chat. |
| 3, partial resync | Field 1 a map of category to revision | Stores the categories, sets `reload` the first time they change, and keeps asking with field 6 filled in until LINE goes back to operations |

Anything else (more than one arm, a negative revision, a malformed list) is rejected and changes nothing.

## The long poll

[`live_sync.c`](../../src/live_sync.c) runs the `line-sync` thread. It owns a private copy of the cursor and the access token, and its own HTTPS connection.

Each round:

1. It calls `sync` on `/SYNC4` with `x-lst: 50000`, so LINE may hold the request open for 50 seconds.
2. LINE answers as soon as something happens, or after about 50 seconds with nothing.
3. The thread applies the reply to its cursor, collecting the operations, and adds them to a batch under a mutex. It also records a rotated access token if the reply carried one.
4. If anything new arrived (operations, a reload, a rotated token, or recovery from failures) it writes to its **eventfd**, which wakes the worker's `poll()`.
5. Pacing:
   - After a failure it waits `max(Retry-After, 1, 2, 4… up to 32 s)`.
   - After an idle reply that came back in under 0.8 s (LINE didn't hold it open), it waits out the rest of a second, so a server that doesn't long-poll is polled at most once a second.
   - Otherwise it starts the next round immediately.

An idle 50-second timeout isn't a failure. Other errors increase `failures` (at most 6), and the batch reports `failing` while they continue.

The worker takes the batch in `live_operations()` in [`main.c`](../../src/main.c) with `line_live_sync_take()`. When the worker renews or receives a new access token it passes it to the thread with `line_live_sync_set_access()`. Stopping the thread (`line_live_sync_stop()`) detaches it: it exits after its current request returns.

With `LINE_DEBUG_SYNC=1` each round prints `line-sync reply after <ms> ms, <n> operations`.

## The baseline and the fallback check

The long poll can't start until the client knows where "now" is. Otherwise, on first sign-in, it would replay the account's entire history, including old call invitations. `sync_revision()` in `main.c`, run by the worker's one-shot maintenance timer, handles this:

1. **Baseline.** The first time, it calls `getLastOpRevision` on `/S4` and sets the cursor to the answer. Nothing before it is applied. Then it starts the long-poll thread.
2. **While the long poll works,** the timer fires every 2 seconds only for maintenance: expiring unanswered call invitations (`line_voice_tick()`), renewing tokens (`maintain_tokens()`) and saving a rotated token.
3. **While the long poll is failing,** the timer falls back to the cheap revision check. If `getLastOpRevision` is higher than the cursor, it runs one ordinary `sync` call (with a 2-second poll, `poll_call_operations()`) to pick up the operations, and sends `changed` to the UI. It checks every second while the window is focused or there is more to fetch, otherwise every 3 seconds. Once the long poll has been failing for two minutes, it checks every 15 seconds instead (`fallback_interval()`), so a long outage never turns into hours of one-second probes.
4. **When the network is down,** it shows "Automatic chat refresh is unavailable" once and backs off: 3, 6, 12, 24, then every 30 seconds.
5. **When LINE rejects the session,** it forgets it and sends `signed_out`.

The `foreground` request from the UI tells the worker whether the window is focused. When it becomes focused, the worker re-arms the timer to check sooner.

## Applying operations

For each operation in a batch, `live_operations()`:

1. calls `apply_operation()`, which gives the operation to `line_profile_operation()` (in [`messaging.c`](../../src/messaging.c), which clears the cached display names on profile and chat changes) and to `line_voice_operation()` (in [`call.c`](../../src/call.c), which recognizes incoming calls);
2. tries `message_operation()` for message operations;
3. otherwise tries `social_operation()` for friend and profile operations (at most `LIVE_CARD_LIMIT` per batch);
4. otherwise notes that "something else" changed.

Then:

- If something else changed, or a resync was requested, it sends one `changed` event, and the UI reloads the chat list and the open chat.
- Otherwise, if a message arrived in the open chat, it calls `revalidate_history()` to fetch that chat's latest page and send a `history` event.

### Message operations

`message_operation()` handles SEND_MESSAGE (25, sent from this account, on any device) and RECEIVE_MESSAGE (26). From the Message in field 20 it works out the chat: for a one-to-one message (`toType` 0, Message field 3) received from someone else, the chat is the sender's MID; otherwise it is the recipient (field 2), which is the group, the room, or the person you wrote to. It then:

- sends a `chat_message` event with the chat, message ID, time and whether it was yours, so the UI can move that chat to the top without reloading;
- records the newest message ID per chat in `unread_heads`, never moving backwards, even when LINE replays operations;
- marks the chat's cached history stale, or drops it if the chat isn't open.

### Friend and profile operations

`social_operation()` turns these into small events that patch the UI in place, as a chat app updates one user rather than reloading everything:

| OpType | Name | Event sent |
| ------ | ---- | ---------- |
| 1 | UPDATE_PROFILE (you, from any device) | `self`, with your fresh profile |
| 5 | NOTIFIED_ADD_CONTACT (someone added you) | `friend_added`, with their card |
| 88 | NOTIFIED_FRIEND_REQUEST | `friend_request` |
| 2, 4, 6, 7, 49, 63, 78, 141 | NOTIFIED_UPDATE_PROFILE, ADD_CONTACT, BLOCK_CONTACT, UNBLOCK_CONTACT, UPDATE_CONTACT, FRIEND_REQUEST_ACCEPTED, NOTIFIED_BUDDY_UPDATE_PROFILE, NOTIFIED_UPDATE_PROFILE_CONTENT | `contact`, with that person's card |

The card is fetched with `line_friend_card()` (see [Friends](social.md#profile-cards)). The UI's `ui_profile_packet()` updates the chat row, any open profile card and the Inbox.

## Operation types the client handles

| OpType | Meaning | Handled by |
| ------ | ------- | ---------- |
| 1 | Your profile changed | `social_operation()`, `line_profile_operation()` |
| 2 | A contact's profile changed | both |
| 4 | You added a contact | `social_operation()` |
| 5 | Someone added you | `social_operation()` |
| 6, 7 | Block or unblock | `social_operation()` |
| 10, 11, 121, 122, 150 | Group and chat changes | `line_profile_operation()` clears cached names. The operation counts as "something else", so the chat list reloads. |
| 25 | You sent a message (any device) | `message_operation()` |
| 26 | You received a message | `message_operation()` |
| 49 | A contact was updated | both |
| 50 | Incoming call invitation | `line_voice_operation()`, see [Calls](calls.md#incoming-calls) |
| 51 | Call invitation canceled | `line_voice_operation()` |
| 63 | A friend request was accepted | `social_operation()` |
| 78 | An official account's profile changed | both |
| 88 | A friend request arrived | `social_operation()` |
| 141 | Profile content changed | both |

Any other operation type makes the UI reload the chat list and open chat. That is always correct, just slower, so a new operation type is safe to leave unhandled at first.

## Read receipts and unread counts

Unread counts come from the chat list (`getMessageBoxes` returns an unread count per chat, see [Messages](messaging.md#the-chat-list)). The UI updates them locally as `chat_message` events arrive, and marks a chat read with `markRead` when you've actually seen its latest message (see [UI: read receipts](ui.md#read-receipts)). `markRead` calls `sendChatChecked`, described in [Messages](messaging.md#read-receipts).
