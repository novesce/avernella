# Architecture (moved)

This page has been split into the bilingual developer documentation. Its sections now live here:

| Former section | English | ภาษาไทย |
| -------------- | ------- | ------- |
| Processes, the UI process, the worker process | [Architecture](en/architecture.md) | [สถาปัตยกรรม](th/architecture.md) |
| How the processes talk (UI ↔ worker, UI ↔ browser) | [Architecture: how the UI and the worker talk](en/architecture.md#how-the-ui-and-the-worker-talk) | [สถาปัตยกรรม](th/architecture.md) |
| Where things live in `src/` | [Architecture: where everything lives](en/architecture.md#where-everything-lives) | [สถาปัตยกรรม](th/architecture.md) |
| A message, end to end | [Architecture: a message, end to end](en/architecture.md#a-message-end-to-end) | [สถาปัตยกรรม](th/architecture.md) |
| Saved state | [Architecture: files on disk](en/architecture.md#files-on-disk) | [สถาปัตยกรรม](th/architecture.md) |
| LINE apps and the in-app browser | [LIFF](en/liff.md) | [LIFF](th/liff.md) |
| Performance and Linux facilities | [Architecture: Linux facilities](en/architecture.md#linux-facilities-the-app-relies-on) | [สถาปัตยกรรม](th/architecture.md) |
| Drawing and the Vulkan renderer | [The user interface](en/ui.md#the-drawing-pipeline) | [ส่วนติดต่อผู้ใช้](th/ui.md) |

Start at the [English index](en/README.md) or the [Thai index](th/README.md).

## Legacy English reference

### Processes

### The UI process

`main()` in [`src/main.c`](../src/main.c) starts the UI unless it was started with `--worker`. The UI ([`src/ui.c`](../src/ui.c) and the `ui_*.c` files) owns the SDL window and everything you see:

- SDL creates the window and reads input. The native Vulkan module composites it on the GPU. SDL Vulkan and the SDL window surface provide the fallback paths.
- Cairo and Pango rasterize changed UI content, including Thai and emoji. The native renderer retains the canvas in device memory and draws the UI and video tiles in one instanced batch.
- Background threads decode JPEG, PNG, WebP and animated PNG, so scrolling never waits for a picture.
- It talks to the desktop through D-Bus: the StatusNotifier tray ([`ui_tray.c`](../src/ui_tray.c)), notifications ([`ui_notifications.c`](../src/ui_notifications.c)), and the XDG portals for screenshots and screen sharing ([`screenshot.c`](../src/screenshot.c), [`screencast.c`](../src/screencast.c)).

The UI starts the worker on the first sign-in attempt with `posix_spawn` of `/proc/self/exe --worker`. To sign in with email, the UI starts it with `--worker-password` instead and passes the password on a separate pipe (fd 3), then wipes the UI's copy.

### The worker process

The worker ([`src/main.c`](../src/main.c), from `worker()`) is the only process that holds your session: access and refresh tokens, Letter Sealing keys and the message sequence counter. It runs every LINE request, encrypts and decrypts messages, uploads and downloads media, and runs calls. Its main loop blocks in `poll` on these descriptors:

- the UI socket,
- separate one-shot timers for the fallback revision check and the token renewal time LINE supplied,
- eventfds from the image and sync threads.

Token renewal uses an absolute `CLOCK_REALTIME` timer for the date LINE supplies. Call invitations, revision checks and retries use monotonic time. During healthy long polling, an idle worker can sleep until its next deadline or until a descriptor becomes readable.

**The sync thread** keeps one `sync` request open on `/SYNC4` on its own HTTPS connection. When operations arrive, the thread signals the worker through an eventfd. If the long poll fails, the worker checks `getLastOpRevision`. Successful fallback checks recur after one second in the foreground or while catching up, and after three seconds in the background. Failed checks back off and the worker honors the transport’s retry delay. [Long poll](../src/live_sync.c) · [Worker scheduling](../src/main.c)

**The call thread** owns one active call’s UDP sockets, SRTP sessions and Opus and VP8 codecs. After SRTP authentication and decryption, a complete single-packet VP8 picture goes straight from the decrypted buffer to libvpx. This skips both the fragment copy and the assembled-picture copy for EVS3 and RFC 7741 packets. Fragmented pictures are assembled in sequence. Libvpx decodes on the CPU into I420 planes, which the native Vulkan renderer uploads and converts to color for display. [Call transport](../src/call.c) · [VP8 receiver](../src/call_video.c) · [Vulkan canvas](../src/render/vk_canvas.c)

### The in-app browser

`avernella-liff` ([`src/liff_browser.c`](../src/liff_browser.c)) is a separate program on WPE WebKit, built only when WPE is installed. The UI ([`src/ui_liff.c`](../src/ui_liff.c)) starts it on the first link from an official account and it exits when its window closes. It is described under [LINE apps](#line-apps-and-the-in-app-browser) below.

## How the processes talk

### UI ↔ worker

The UI and worker share a `SOCK_STREAM` socketpair carrying one JSON object per line.

- **Requests** from the UI look like `{"id": 12, "method": "send", "params": {"chat": "u…", "text": "hi"}}`. The worker answers `{"id": 12, "result": …}` or `{"id": 12, "error": "…"}`. `dispatch()` in `main.c` lists every method: `login`, `chats`, `history`, `send`, `sendPhoto`, `sendSticker`, `react`, `unsend`, `friends`, `createGroup`, `callStart`, `liffView` and about sixty more.
- **Events** from the worker have no `id`, for example `{"event": "history", "chat": …, "messages": […]}`, `{"event": "status", …}`, `{"event": "call", …}` and `{"event": "qr", "url": …}`. The UI applies them in `handle_packet()`.

The UI side runs an I/O thread (`line-ipc`) that writes queued lines when an `eventfd` wakes it and reads replies into SDL user events, so the drawing thread never blocks on the socket.

The worker writes call previews directly from the decoder or camera planes into sealed `memfd` files with `pwritev`. It passes each descriptor over a dedicated `SOCK_SEQPACKET` socket with `SCM_RIGHTS`. The window keeps the newest pending frame separately for the peer’s camera, local camera and peer’s screen. Hiding the window or closing the call stage pauses preview transfers while the call continues. Downloaded media uses a separate lease that lets the UI open and map the sealed file. [Media IPC implementation](../src/media_io.c).

### UI ↔ in-app browser

The UI and `avernella-liff` share a `SOCK_SEQPACKET` socketpair on the browser's fd 3. Each datagram is one tab-separated message, defined in [`src/liff_url.h`](../src/liff_url.h):

| Direction | Message | Meaning |
| --------- | ------- | ------- |
| UI → browser | `L <size> <url>` | Load a LIFF page (`full`, `tall` or `compact` window) with the LIFF user agent |
| UI → browser | `W web <url>` | Load an ordinary web page with LINE's in-app user agent |
| UI → browser | `R <id> <1\|0>` | Answer a JavaScript dialog |
| browser → UI | `O <url>` | The page followed a LIFF or `line://` link; ask LINE for a new view |
| browser → UI | `X <url>` | Open this link on the desktop (system browser, mail, phone) |
| browser → UI | `D <id> <a\|c> <text>` | Show an `alert()` or `confirm()` |

A reader thread in the UI sleeps in `recv` and posts an SDL event only when the browser speaks. End of file means the browser exited, and the UI collects it through its pidfd.

## Where things live in `src/`

| Area | Files |
| ---- | ----- |
| Entry, worker loop, request dispatch | `main.c` |
| HTTPS transport, token refresh, connection tuning | `transport.c`, `http_tune.c`, `tune.c` |
| Compact Thrift encoder and decoder | `thrift.c`, `line.h` |
| Sign-in: QR pairing, email and password | `pair.c`, `password_login.c` |
| Saved session | `state.c` |
| Sync: operation cursor, long poll | `talk_sync.c`, `live_sync.c` |
| Chats, history, sending, reactions, announcements | `messaging.c` |
| Letter Sealing (end-to-end encryption) | `sealing.c` |
| Attachments and object storage | `media.c`, `media_crypto.c`, `obs_http.c`, `media_io.c` |
| Friends, profiles, recommendations | `friends.c`, `profile_image.c` |
| Groups and invitations | `groups.c` |
| Emoji and stickers | `emoji.c`, `stickers.c`, `emoji_defaults.inc`, `emoji_pack_index.inc` |
| Albums, notes, polls | `shared_content.c` |
| Official-account cards: Flex and rich messages | `ui_flex.c`, `flex_asset.c`, `rich_message.c` |
| LINE apps (LIFF) | `liff.c`, `liff_url.c`, `ui_liff.c`, `liff_browser.c` |
| Calls | `call.c`, `call_schema.c`, `call_wire.c`, `call_srtp.c`, `call_pdtp.c`, `call_audio.c`, `call_denoise.c`, `call_video.c`, `camera.c`, `camera_pixels.c`, `screencast.c` |
| Video clips | `video_record.c`, `video_play.c` |
| Drawing, layout, animation | `ui.c`, `ui_draw.c`, `ui_media.c`, `ui_motion.c`, `ui_pixels.c`, `ui_apng.c` |
| Layout preference, language selection and translation tables | `ui_theme.c`, `ui_lang.c`, `ui_lang_table.h`, `ui_lang_*.c` |
| Screens and panels | `ui_friends.c`, `ui_groups.c`, `ui_profile.c`, `ui_find.c`, `ui_emoji.c`, `ui_chat_lists.c`, `ui_chat_tools.c`, `ui_shared_content.c` |
| Desktop integration | `ui_tray.c`, `ui_notifications.c`, `screenshot.c`, `clipboard_paste.c`, `chat_lists.c`, `qr_scan.c` |
| Sounds: NES APU synth and playback | `chiptune.c`, `ui_sounds.c` |

## A message, end to end

**Sending.** You press Enter. The UI sends `{"method": "send", "params": {"chat", "text"}}` to the worker.

1. The worker reserves the next message sequence number and writes it to disk first, so an interrupted send can't reuse it.
2. It seals the text for the chat's members (`line_seal_text` in `sealing.c`).
3. It calls `sendMessage` on `/S4`.
4. The reply becomes the message row the UI shows in place of the pending one.

**Receiving.** The other person's message arrives on the open `/SYNC4` long poll as a `RECEIVE_MESSAGE` operation (type 26).

1. The sync thread hands it to the worker.
2. The worker emits a `chat_message` event with the chat ID, message ID and time. The UI can update the chat row as soon as that event arrives. If the affected chat is open, the worker revalidates its history. Otherwise it drops that chat’s cached page so the next open fetches fresh messages.
3. Once the focused window has presented the newest message at the bottom of an unobscured chat, the UI clears the local unread badge and submits `markRead` when a receipt is due. Reading an older page or leaving the window minimized keeps newer messages unread. [Read receipt conditions](../src/ui_private.h) · [UI event handling](../src/ui.c)

## Saved state

| Path | Contents |
| ---- | -------- |
| `$XDG_STATE_HOME/avernella/session.json` (default `~/.local/state/avernella`) | Access and refresh tokens, their renewal schedule, your MID, Letter Sealing keys and the message sequence counter. Mode 0600 in a 0700 directory, written by atomic rename under an exclusive directory lock. |
| `$XDG_STATE_HOME/avernella/` (same directory) | Per-account pins, folders, mutes, shortcuts and the Reduce Motion preference ([`chat_lists.c`](../src/chat_lists.c)) |
| `$XDG_CONFIG_HOME/avernella/layout` and `language` (under `~/.config` by default) | Layout and interface language shared across local accounts. A fresh profile starts with the LINE layout and chooses its language from the desktop locale. |
| `$XDG_RUNTIME_DIR/avernella/` | Pictures pasted for sending, deleted after use |
| `~/.cache/kitatide/liff`, `~/.local/share/kitatide/liff` | The in-app browser's HTTP cache and cookies |

Messages, history and drafts stay in memory and are fetched again from LINE when needed. The session file holds keys in plain text, protected by file permissions; anyone who can read it can act as this linked device.

## LINE apps and the in-app browser

A tapped link in an official-account card goes through `ui_liff_open_link()`:

1. **LIFF link** (`liff.line.me`, `miniapp.line.me`, `line.me/R/app`, `line://app`). The UI asks the worker for `liffView`. The worker calls `issueLiffView` on `/LIFF1` and builds the page address with LINE's tokens in its fragment ([How LINE works](line-protocol.md#line-apps-liff)). The UI sends it to the browser as `L`.
2. **Any other https link.** The UI sends it as `W` and the page opens with LINE's in-app user agent.
3. **No `avernella-liff` installed.** The link opens in the system browser.

Inside the page, `avernella-liff` provides what LINE's app provides:

- a user agent with `Line/9.7.0 LIFF`;
- `window._liff`, the native bridge that `liff.init()` waits for;
- `liff://close` and `window.close()` to close the window;
- a hand-back of LIFF and `line://` links to the UI, so they get their own view from LINE.

## Performance and Linux facilities

The app leans on the kernel and glibc instead of frameworks.

**Network**
- **Connection reuse.** Native HTTPS uses libcurl with HTTP/2 negotiation, TCP Fast Open and keepalive. The RPC threads share caches for DNS answers and TLS sessions ([`http_tune.c`](../src/http_tune.c)). Each thread keeps its own curl handle and connection pool, so the long poll can run independently of sends ([`transport.c`](../src/transport.c)).
- **Long poll instead of timers.** Messages arrive by long poll. The worker sleeps in `poll` until a socket, eventfd, or maintenance deadline needs attention.

**Memory**
- **Few allocator arenas.** `mallopt` sets two malloc arenas, a 128 KiB mmap threshold and a 512 KiB trim threshold, so big picture buffers go back to the kernel when freed, and the UI calls `malloc_trim` when it loses focus ([`tune.c`](../src/tune.c)).
- **Decoded image caches.** Image reservations shed old thumbnails above 12 MiB. Recent pictures and pending requests can use up to 32 MiB. A partial repaint keeps previously visible pictures eligible for reuse. Hiding or minimizing the window releases cold decoded entries. Animation frames have a separate 32 MiB budget and are released with their poster.
- **Zero-copy pictures.** Pictures cross processes as sealed memfds.

**Scheduling and drawing**

- **Thread priorities.** Image decoding runs under `SCHED_BATCH`. The call's media thread asks for tight timer slack.
- **Frame pacing.** The UI uses monotonic deadlines paced to the reported display refresh rate. It uses `SDL_WaitEventTimeout` for a future frame deadline and `SDL_WaitEvent` when idle or hidden. Input and worker events wake either wait. A held button stops requesting frames once it reaches its pressed position. [UI loop](../src/ui.c) · [Motion timing](../src/ui_motion.c)
- **Partial redraws.** Chat row highlights and button presses can repaint their combined bounds, including the last frame as they settle. On unrotated surfaces with Vulkan incremental presentation, the renderer tracks the changes each swapchain image has missed and restricts drawing to their bounding rectangle when that image returns. A resize or unknown damage triggers a full redraw. [UI damage tracking](../src/ui_draw.c) · [Vulkan presentation](../src/render/vk_present.c)
- **Vectorized pixel loops.** Pixel loops are built with `-O3`, which lets GCC vectorize them.
- **Sounds without a sound server stream at rest.** Call and message sounds come from an emulated NES APU. Its timers run event by event rather than every CPU cycle, so a whole ringtone renders in under 10 ms. The SDL audio device opens when a sound starts and closes once it ends.

**The in-app browser**
- **Started on demand.** `avernella-liff` is started with `pidfd_spawn` (clone3 with a pidfd) and supervised through that pidfd.
- **Its own cgroup.** It moves itself and every WebKit process into a systemd transient scope, a cgroup v2 leaf with `MemoryHigh=768M`, `CPUWeight=50` and `IOWeight=50`, and waits until `/proc/self/cgroup` shows the move before WebKit forks.
- **Kernel hints.** It raises `oom_score_adj` to 500, sets `PR_SET_PDEATHSIG`, and sets a 1 ms timer slack that WebKit's processes inherit.
- **WebKit limits.** Web processes are capped at 512 MB and the network process trims its caches above 128 MB.
- **Measured cost.** On the development machine a simple page used about 105 MB of private memory across all of its processes, and the first navigation started 0.14 s after a cold start.

**Binary**
- The release build uses `-Oz` with LTO, `--gc-sections`, a single read-only segment and packed relative relocations.
- The signed-out window used about 14 MiB of proportional memory.
