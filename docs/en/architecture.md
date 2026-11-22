# Architecture

[Index](README.md) · Previous: [Building](building.md) · Next: [The user interface](ui.md)

This page explains the moving parts: which processes and threads exist, what each one is allowed to touch, how they talk, and what is written to disk. Read it before changing anything that crosses a process or thread boundary.

## The big picture

Avernella is one executable, `avernella`, that runs as **two processes**, plus an optional **browser helper** program.

```
┌──────────────────────────── avernella (UI process) ─────────────────────────────┐
│ SDL window · Cairo/Pango drawing · Vulkan module · image decoding · tray ·      │
│ notifications · sounds · portals (screenshot) · clipboard · drafts in memory    │
└───────┬───────────────────────────────┬──────────────────────────────┬──────────┘
        │ JSON lines                    │ call video frames            │ tab-separated
        │ (SOCK_STREAM socketpair,      │ (SOCK_SEQPACKET, sealed      │ datagrams
        │  worker's stdin/stdout)       │  memfds via SCM_RIGHTS)      │ (SOCK_SEQPACKET, fd 3)
┌───────┴───────────────────────────────┴────────┐          ┌──────────┴───────────┐
│ avernella --worker (worker process)            │          │ avernella-liff       │
│ session, tokens, Letter Sealing keys · RPCs ·  │          │ (WPE WebKit, or      │
│ media up/download · sync thread · image thread │          │ avernella-liff-gtk)  │
│ · call thread · screen capture                 │          │ LINE apps (LIFF)     │
└───────┬────────────────────────────────────────┘          └──────────┬───────────┘
        │ HTTPS (libcurl, HTTP/2)        UDP (calls)                    │ HTTPS
  legy.line-apps.com · obs.line-apps.com · LINE CDNs · call relays    the app's own site
```

The split has one purpose: **the process that draws untrusted content never holds your LINE credentials, and the process that holds the credentials never draws anything.** Pictures, Flex cards and rich messages from strangers are decoded and drawn in the UI process. Tokens, the refresh token and Letter Sealing private keys exist only in the worker. Web pages from LINE apps run in a third process with its own sandbox.

## The UI process

`main()` in [`main.c`](../../src/main.c) calls `line_ui()` in [`ui.c`](../../src/ui.c) unless the first argument is `--worker` or `--worker-password`. The UI process owns:

- the SDL window, input, the clipboard and drag-and-drop;
- drawing with Cairo and Pango, presented through the native Vulkan module, SDL's Vulkan renderer or the SDL window surface;
- decoding pictures (JPEG, PNG, WebP, animated PNG) on a background thread;
- the chat list, the open conversation's history and every draft, all in memory;
- desktop services over D-Bus: the tray, notifications, the screenshot portal;
- chat preferences (folders, pins, mutes, shortcuts), via [`chat_lists.c`](../../src/chat_lists.c);
- sounds, via [`ui_sounds.c`](../../src/ui_sounds.c);
- starting and supervising the worker and the browser helper.

It never opens a connection to LINE's RPC servers. Everything it needs from LINE it asks the worker for.

### UI threads

| Thread | Started in | Job |
| ------ | ---------- | --- |
| main | `line_ui()` | SDL events, layout, drawing, all interface state. **Only this thread may touch `ui` (the `Interface` struct) or request state.** |
| `line-ipc` | `start_worker()` in `ui.c` | Writes queued requests to the worker, reads replies and events, and receives call video frames. It turns each line into an SDL user event, so the main thread never blocks on a socket. |
| image decoder pool (1 thread) | `start_decode()` in [`ui_media.c`](../../src/ui_media.c) | Decodes pictures, thumbnails and animation frames under `SCHED_BATCH` |
| `avernella-liff` | [`ui_liff.c`](../../src/ui_liff.c) | Sleeps in `recv()` on the browser socket and posts an SDL event when the browser speaks |
| `line-tray` | [`ui_tray.c`](../../src/ui_tray.c) | Runs the StatusNotifierItem D-Bus service |
| `line-notify` | [`ui_notifications.c`](../../src/ui_notifications.c) | Sends one desktop notification |
| `line-screenshot` | `ui_media_screenshot()` | Waits for the screenshot portal |
| `line-player` | [`video_play.c`](../../src/video_play.c) | Watches an `ffplay` child process |
| `camera-recorder` | [`video_record.c`](../../src/video_record.c) | Watches an `ffmpeg` recording |

Helper threads never touch interface state. They hand results back by pushing an SDL user event, and the main thread applies them.

## The worker process

The worker ([`main.c`](../../src/main.c), from `worker()`) is the only process that holds the session: access and refresh tokens, the account MID, the Letter Sealing key chain and the request sequence counter. It runs every RPC, encrypts and decrypts messages, uploads and downloads media, and runs calls.

### How it is started

The UI starts the worker on the first sign-in attempt (or at launch, when a saved session exists) in `start_worker()` in `ui.c`:

1. It creates a `SOCK_STREAM` socketpair for JSON and a `SOCK_SEQPACKET` socketpair for call video frames.
2. It runs `posix_spawn("/proc/self/exe", ["avernella", "--worker"])` with file actions that put the JSON socket on the worker's **stdin and stdout** and the frame socket on **fd 3**. On glibc 2.34 and newer, `posix_spawn_file_actions_addclosefrom_np` closes every other inherited descriptor.
3. For email sign-in, it runs `--worker-password` instead. Then fd 3 is a pipe carrying one `struct line_credentials` (email and password, 256 bytes each), and the frame socket moves to fd 4. The worker reads the pipe once, closes it and wipes the buffer with `explicit_bzero`. The UI wipes its own copy too. The password never appears in arguments, the environment or the JSON channel.

The worker's first action is `prctl(PR_SET_PDEATHSIG, SIGTERM)`, so it dies with the window. It then checks that its parent didn't already exit, which closes the race.

### The worker's main loop

`worker()` blocks in `poll()` on four descriptors and handles whichever is ready:

| Descriptor | Ready when | Handler |
| ---------- | ---------- | ------- |
| stdin | The UI sent a request line | `handle()` → `dispatch()` |
| `image_wake` (eventfd) | The image thread finished a download | `finish_images()` |
| live-sync eventfd | The long-poll thread has operations or a rotated token | `live_operations()` |
| `timer` (one-shot timerfd) | Maintenance is due | `sync_revision()`: token renewal, call-invite expiry, and the fallback revision check when the long poll is failing |

Requests are processed one at a time, in order. A slow RPC therefore delays the next request. That is deliberate: it keeps session state single-threaded. The long poll and the anonymous downloads run on their own threads so they never wait behind a send.

### Worker threads

| Thread | Started in | Job | May touch the session? |
| ------ | ---------- | --- | ---------------------- |
| main | `worker()` | Requests, sync results, token renewal | Yes, exclusively |
| `line-sync` | `line_live_sync_start()` in [`live_sync.c`](../../src/live_sync.c) | Keeps one `sync` long poll open | No. It has a copy of the access token and its own HTTPS connection. |
| `line-images` | `queue_image()` in `main.c` | Downloads avatars, sticker and emoji artwork and Flex pictures from public CDNs | No. Downloads are anonymous. |
| `line-voice` | `launch_call()` in [`call.c`](../../src/call.c) | One call: UDP sockets, signalling, SRTP, audio, video | No. It gets only call-local data, never the account token. |
| `line-screencast` | [`screencast.c`](../../src/screencast.c) | Talks to the ScreenCast portal and PipeWire during a screen share | No |

Each thread that talks HTTPS keeps its own curl handle in thread-local storage (`static __thread CURL *client` in [`transport.c`](../../src/transport.c)), so the long poll never blocks an RPC.

## How the UI and the worker talk

### The JSON channel

Each message is one JSON object on one line, terminated by `\n`. There are three kinds.

**Requests** go from the UI to the worker and carry a numeric `id`:

```json
{"id": 12, "method": "send", "params": {"chat": "u0123…", "text": "hi", "replyTo": "5123…"}}
```

**Replies** go back with the same `id` and either `result` or `error`:

```json
{"id": 12, "result": {"id": "5124…", "sentAt": 1764000000000}}
{"id": 12, "error": "LINE declined sendMessage (service code 10)."}
```

A failed `login` caused by an unreachable network also carries `"retry": true`, so the UI keeps the saved session and tries again with backoff instead of returning to the sign-in screen.

**Events** go from the worker to the UI, have no `id`, and carry an `event` name:

```json
{"event": "chat_message", "chat": "c0123…", "id": "5125…", "time": 1764000000000, "mine": false}
```

On the worker side, `line_emit()` writes a JSON object plus a newline to stdout with `writev`, under a mutex, because the call thread also emits events. `line_event(name, key, value)` is a shortcut for a one-field event.

On the UI side, `submit()` in `ui.c` assigns the next `id`, stores a `struct ui_request` describing what to do with the reply, encodes the line and wakes the `line-ipc` thread through an eventfd. When the reply arrives, `handle_packet()` looks the request up by `id` and calls `handle_reply()`. Replies for core requests (login, chats, history, send, older pages, read receipts, logout) are handled in `ui.c`. Everything else is a `MEDIA` request: `handle_reply()` offers the reply to `ui_liff_reply()`, `ui_profile_reply()`, `ui_find_reply()`, `ui_groups_reply()`, `ui_friends_reply()` and finally `ui_media_reply()`, and the first module that recognises the method name handles it. A module sends such a request with `ui_rpc(method, params, tag)`, where `tag` is its own number for matching the reply.

A **generation** number protects against stale replies: `stop_worker()` increments `wire.generation`, and packets posted by an older `line-ipc` thread are dropped.

### Worker methods

`dispatch()` in `main.c` is the single list of everything the UI can ask for. Before sign-in only `login` is accepted. Most chat methods also require `params.chat` to be a chat the worker has seen in the chat list.

| Area | Methods |
| ---- | ------- |
| Session | `login`, `logout`, `foreground` (tells the worker whether the window is focused, which changes polling speed) |
| Chat list and history | `chats`, `history`, `olderHistory`, `markRead`, `findMessages`, `findAllMessages` |
| Sending | `send`, `sendPhoto`, `sendFile` (with `type` `video`, `audio` or a file), `sendSticker`, `forwardMessage`, `react`, `unsend` |
| Announcements | `announcements`, `announceMessage`, `removeAnnouncement` |
| Media | `photo`, `saveFile`, `openVideo`, `videoPoster`, `releaseMedia` |
| Images (queued on the image thread) | `avatar`, `emoji`, `sticker`, `flexImage`, and `photo` for a sticker message |
| Stickers and emoji | `stickerCatalog`, `emojiCatalog` |
| Friends | `friends`, `findFriend`, `addFriend`, `friendInvite`, `openFriend`, `renameFriend`, `setFriendFavorite`, `setFriendHidden`, `setFriendBlocked`, `addRecommendation`, `friendRequests`, `acceptFriendRequest`, `declineFriendRequest` |
| Profiles | `myProfile`, `updateMyProfile`, `updateMyPicture`, `profileCard`, `openProfileChat`, `addProfileFriend`, `setProfileBlocked` |
| Groups | `createGroup`, `groupMembers`, `inviteGroup`, `removeGroupMember`, `cancelGroupInvite`, `renameGroup`, `leaveGroup`, `groupLink`, `setGroupLink`, `renewGroupLink`, `groupInvitations`, `joinGroup`, `declineGroup` |
| Shared content | `sharedList`, `sharedPost`, `sharedPhotos`, `sharedPhoto` |
| LINE apps | `liffView` |
| Calls | `callStart`, `callAccept`, `callDecline`, `callEnd`, `callMute`, `callCamera`, `callShare`, `callPreview`, `callFrame` |

### Worker events

| Event | Meaning | Handled in the UI by |
| ----- | ------- | -------------------- |
| `qr` (`url`) | Show this sign-in QR code | `handle_packet()` in `ui.c` |
| `pin` (`pin`) | Show this code for the user to enter on the phone | `handle_packet()` |
| `status` (`message`) | Show this text in the status line | `handle_packet()` |
| `signed_out` (`message`) | LINE ended the session; return to sign-in | `reset_login()` |
| `changed` | Something changed that needs a full refresh of the chat list and the open chat | `refresh()` |
| `chat_message` (`chat`, `id`, `time`, `mine`) | A message arrived in or was sent to `chat` | `chat_message()` |
| `history` (`chat`, `messages`) | Fresh rows for the open chat | `pushed_history()` |
| `self`, `contact`, `friend_added`, `friend_request` (`row`) | A profile or friendship changed | `ui_profile_packet()` in [`ui_profile.c`](../../src/ui_profile.c) |
| `call`, `call_incoming`, `call_error`, `call_camera_error`, `call_frame`, `call_self_frame`, `call_share`, `call_share_frame` | Call state and video | `ui_media_call_event()` in [`ui_media.c`](../../src/ui_media.c) |

### Large data: media leases

Pictures, files and video posters are too large for a JSON line. The worker writes them into a **sealed memfd** instead (`line_media_lease()` in [`media_io.c`](../../src/media_io.c)): it creates the file with `memfd_create`, writes the bytes, adds `F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL`, and replies:

```json
{"lease": "4b1c…-uuid", "path": "/proc/<worker pid>/fd/<n>"}
```

The UI opens that path (it runs as the same user), maps or reads it, and sends `releaseMedia` with the lease so the worker closes the descriptor. Because the file is sealed, the UI can map it without fearing that the bytes change under it. At most four leases may be open at once.

### Call video frames

Call video is the one stream that bypasses JSON. The call thread writes each decoded frame (I420 planes) straight into a fresh sealed memfd with `pwritev`, then sends the descriptor over the `SOCK_SEQPACKET` frame socket with `SCM_RIGHTS`, together with a small `struct line_preview_frame` header (size, rotation, source, chat, decode time). There are three sources: the peer's camera, your own camera and the peer's screen.

The UI's `line-ipc` thread receives the descriptor in `pump_preview()` and keeps **only the newest frame per source**: if a frame is already waiting, the older descriptor is closed. The main thread then uploads the frame as a texture. The socket's send buffer is deliberately small (4 KiB), so a busy window never builds up a backlog of old frames. Hiding the window or the call stage stops preview transfers while the call continues (`callPreview`).

### The browser helper channel

The UI and `avernella-liff` share a `SOCK_SEQPACKET` socketpair on the browser's fd 3. Each datagram is one tab-separated message (see [`liff_url.h`](../../src/liff_url.h)). This is described in [LIFF](liff.md#the-browser-helper).

## Files on disk

Avernella stores very little. Messages, history, the chat list and drafts are **never** written to disk.

| Path | Written by | Contents |
| ---- | ---------- | -------- |
| `$XDG_STATE_HOME/avernella/session.json` (default `~/.local/state/avernella/`) | the worker, [`state.c`](../../src/state.c) | The session. See [Signing in](sign-in.md#the-session-file). |
| `$XDG_STATE_HOME/avernella/session.new` | the worker | A temporary file during an atomic save |
| `$XDG_STATE_HOME/avernella/lists-<sha256(MID)>.json` | the UI, [`chat_lists.c`](../../src/chat_lists.c) | Per-account chat preferences: `folders`, `view` (the selected folder), `pins`, `muted`, `bindings` (shortcut overrides), `notify`, `sounds`, `motion` |
| `$XDG_CONFIG_HOME/avernella/layout` (default `~/.config/avernella/`) | the UI, [`ui_theme.c`](../../src/ui_theme.c) | `avernella\n` to choose the Avernella theme. Anything else, or no file, means the default LINE theme. |
| `$XDG_CONFIG_HOME/avernella/` | the UI, [`ui_lang.c`](../../src/ui_lang.c) | The interface-language choice |
| `$XDG_RUNTIME_DIR/avernella/` | the UI, [`clipboard_paste.c`](../../src/clipboard_paste.c) | A picture pasted for sending, deleted after it is sent or the preview closes |
| `~/.cache/kitatide/liff`, `~/.local/share/kitatide/liff` | the browser helper | WebKit's HTTP cache and cookies for LINE apps |

The rules every writer follows:

- **Private permissions.** Directories are created `0700` and files `0600` (the process `umask` is `0077`). Readers refuse files or directories owned by another user or readable by the group or others.
- **Atomic replacement.** A new version is written to a temporary file in the same directory, `fdatasync`ed, renamed over the old one, and the directory is `fsync`ed. A crash leaves either the old file or the new one.
- **Size limits.** The session file may be at most 8 MiB and the chat-lists file 2 MiB. Larger files are rejected rather than parsed.
- **One owner.** The worker holds an exclusive `flock` on the state directory while it runs.

### Migration from the old name

Avernella used to be called *kitatide*. On first use, `line_store_open()` moves an existing `$XDG_STATE_HOME/kitatide` directory to `avernella` with `renameat2(RENAME_NOREPLACE)`, under that directory's lock, so the session and chat preferences carry over. The browser cache paths still use the old name.

## Where everything lives

| Area | Files |
| ---- | ----- |
| Entry point, worker loop, request dispatch, image queue, sync handling | `main.c` |
| Shared types and helpers (`Line`, `J`, `field()`, `strfield()`, error codes) | `line.h` |
| HTTPS transport, token renewal, `Retry-After` | `transport.c` |
| libcurl tuning, shared DNS and TLS caches | `http_tune.c` |
| glibc and scheduler tuning | `tune.c` |
| Thrift Compact and Binary encoder and decoder | `thrift.c` |
| QR pairing | `pair.c` |
| Email and password sign-in | `password_login.c` |
| Saved session, request sequence | `state.c`, `state.h` |
| Sync cursor and operation paging | `talk_sync.c` |
| Long-poll thread | `live_sync.c` |
| Chat list, history, sending, reactions, unsend, read receipts, announcements, mentions, emoji ranges | `messaging.c` |
| Letter Sealing | `sealing.c` |
| Encrypted attachments | `media.c`, `media_crypto.c` |
| Object storage HTTP, avatars, public CDN assets | `obs_http.c` |
| Media leases, preview frames, saving files | `media_io.c` |
| Friends, profiles, friend requests, recommendations | `friends.c` |
| Profile picture conversion and upload | `profile_image.c` |
| Groups and invitations | `groups.c` |
| Stickers | `stickers.c` |
| LINE emoji | `emoji.c`, `emoji_pack_index.inc` |
| Albums, notes and polls | `shared_content.c` |
| Rich messages (imagemaps) | `rich_message.c` |
| Flex and rich-message picture downloads | `flex_asset.c` |
| LIFF view and launch URL | `liff.c` |
| LIFF URL recognition, browser protocol, user agents | `liff_url.c`, `liff_url.h` |
| In-app browser helper | `liff_browser.c` |
| Calls: engine, call routes, incoming invitations | `call.c` |
| PLANET signalling messages | `call_schema.c` |
| PLANET packet encryption, protobuf helpers, media key derivation | `call_wire.c` |
| SRTP contexts | `call_srtp.c` |
| PDTP (screen sharing transport) | `call_pdtp.c` |
| Call audio (SDL, Opus, EAS2) | `call_audio.c` |
| Noise suppression | `call_denoise.c` |
| Call video (VP8, EVS3 packets, feedback, screen-share coding) | `call_video.c` |
| Camera (V4L2) and pixel conversion | `camera.c`, `camera_pixels.c` |
| Screen capture (portal and PipeWire) | `screencast.c` |
| Desktop screenshot portal | `screenshot.c` |
| Camera video messages, video playback | `video_record.c`, `video_play.c` |
| Clipboard picture paste | `clipboard_paste.c` |
| QR image reading (ZBar, loaded on demand) | `qr_scan.c` |
| NES sound chip synth | `chiptune.c` |
| UI: main loop, worker connection, input | `ui.c`, `ui_private.h` |
| UI: drawing, layout, chat list, history, composer | `ui_draw.c` |
| UI: attachments, photo viewer, file picker, message menu, call stage | `ui_media.c` |
| UI: Vulkan module loader, scroll copy maths | `ui_gpu.c`, `ui_gpu_scroll.c` |
| Native Vulkan renderer module | `src/render/` |
| UI: animation timing and scrolling | `ui_motion.c` |
| UI: pixel conversion, animated PNG | `ui_pixels.c`, `ui_apng.c` |
| UI: themes, interface language | `ui_theme.c`, `ui_lang.c` |
| UI: friends, groups, profile and settings, search, emoji and stickers, folders, chat tools, shared content, Flex cards, LIFF links | `ui_friends.c`, `ui_groups.c`, `ui_profile.c`, `ui_find.c`, `ui_emoji.c`, `ui_chat_lists.c`, `ui_chat_tools.c`, `ui_shared_content.c`, `ui_flex.c`, `ui_liff.c` |
| UI: tray, notifications, sounds, logo | `ui_tray.c`, `ui_notifications.c`, `ui_sounds.c`, `ui_logo.c` |
| Chat preferences store | `chat_lists.c` |

## A message, end to end

**Sending.** You type "hi" and press Enter.

1. `send_message()` in `ui.c` calls `request(SEND, chat, text)`. `ui_emoji_export()` converts any LINE emoji in the draft into plain text plus `emojis` ranges, and `mention_ranges()` adds `mentions` in groups. The message is shown as pending.
2. The worker's `dispatch()` validates the text (UTF-8, at most 20,000 bytes) and calls `reserve_sequence()`. That reserves a request sequence number and, when the reserved batch of 32 runs out, saves the new high-water mark to disk **before** the request goes out. A timeout may mean LINE accepted the message, so a number is never reused.
3. `line_send_decorated()` in `messaging.c` builds the payload, and `line_seal_payload()` in `sealing.c` encrypts it with Letter Sealing v2.
4. `line_send_sealed()` calls `sendMessage` on `/S4`.
5. The reply carries the new message ID. `show_sent()` in `ui.c` replaces the pending row and clears the draft if it hasn't changed since.

**Receiving.** The other person sends "hello".

1. LINE answers the open `sync` long poll on `/SYNC4` with a `RECEIVE_MESSAGE` operation (type 26).
2. The `line-sync` thread queues it and signals its eventfd.
3. The worker's `live_operations()` calls `message_operation()`, which emits a `chat_message` event and marks the chat's cached history stale. If that chat is open, `revalidate_history()` fetches its latest page and emits a `history` event. The worker opens the sealed text with `line_open_payload()` while building the rows.
4. The UI's `chat_message()` moves the chat to the top and updates its unread count. `notify_unread()` then shows a desktop notification if the window is in the background, and plays the message chime if the window is away or the message is in a chat other than the open one. Muted chats stay silent.

## Linux facilities the app relies on

Avernella leans on the kernel and glibc instead of frameworks. Knowing these helps when something behaves unexpectedly.

**Processes**
- `posix_spawn` of `/proc/self/exe` for the worker, `pidfd_spawn` (clone3 with a pidfd) for the browser helper, `PR_SET_PDEATHSIG` in both children.
- `posix_spawn_file_actions_addclosefrom_np` so children inherit only the descriptors they need.

**Waiting without timers**
- The worker sleeps in `poll()` on a socket, two eventfds and a one-shot `timerfd`. It wakes only when something happens.
- The long poll replaces periodic checking. The fallback revision check runs only while the long poll is failing.
- The UI waits in `SDL_WaitEventTimeout` with the time until the next frame or animation step, and waits indefinitely when nothing moves or the window is hidden.

**Memory** ([`tune.c`](../../src/tune.c))
- `mallopt` sets two malloc arenas, a 128 KiB mmap threshold and a 512 KiB trim threshold, so large picture buffers go back to the kernel when freed.
- The UI calls `malloc_trim` when the window loses focus.
- Thumbnails share a 12 MiB cache, animations a 32 MiB budget.
- Pictures cross processes as sealed memfds instead of copies.

**Scheduling**
- Image fetching and decoding run under `SCHED_BATCH` at lower priority (`LINE_THREAD_BACKGROUND`).
- The call's media thread asks for tight timer slack (`LINE_THREAD_MEDIA`).

**Network** ([`http_tune.c`](../../src/http_tune.c))
- libcurl with TLS 1.2 or newer, certificate and host name checks, HTTP/2, TCP Fast Open and keepalive.
- Shared DNS and TLS session caches across threads, with a separate connection pool per thread.
- `Retry-After` on HTTP 429 or 503 pauses all RPCs for the given time (`line_transport_retry_after()`).

**The browser helper** ([`liff_browser.c`](../../src/liff_browser.c))
- A systemd transient scope (its own cgroup v2 leaf) with `MemoryHigh=768M`, `CPUWeight=50` and `IOWeight=50`.
- `oom_score_adj` raised, so the kernel reclaims it before the client; a 1 ms timer slack inherited by WebKit's processes.
- WebKit's web processes are capped at 512 MB, and the network process trims caches above 128 MB.

## Security boundaries in one place

- Tokens and keys live only in the worker. The UI never sees the access token, the refresh token or a private key.
- The call thread receives only call-local data, never the account token.
- The browser helper receives LINE app tokens only through its socket, never in arguments or the environment, and they are never logged.
- Picture downloads from arbitrary hosts (Flex, rich messages) are anonymous, HTTPS-only, and refuse private and local network addresses ([`flex_asset.c`](../../src/flex_asset.c)).
- Every input from LINE is size-checked and type-checked before use: the Thrift decoder limits nesting to 48 levels and 300,000 values, and field readers check JSON types.
- The session file is plaintext protected by permissions. Anyone who can read it can act as this linked device.
