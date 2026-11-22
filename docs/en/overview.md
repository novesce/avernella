# Overview

[Index](README.md) · Next: [Building, running and debugging](building.md)

## What Avernella is

Avernella is a native desktop client for LINE personal accounts. It runs on GNU/Linux, is written in C17, and draws its own interface with SDL2, Cairo and Pango. It isn't a wrapper around a web page, and it doesn't run LINE's Android or Windows code. It talks to LINE's servers directly, using the same protocol LINE's Windows desktop app uses.

LINE treats a desktop app as a *linked secondary device* of the phone. You sign in by scanning a QR code with LINE on your phone, or with your email address and password plus a confirmation on the phone. LINE then issues tokens and hands the desktop the account's end-to-end encryption keys. Avernella identifies itself as the Windows desktop app (`DESKTOPWIN 9.7.0.3556`) and names the linked device "Avernella".

The app has no affiliation with LY Corporation. LINE doesn't document the protocol and controls whether a linked device may connect. The protocol was recovered from LINE for Android 26.16.0 and cross-checked against open interoperability projects (see [Contributing](contributing.md#how-the-protocol-was-recovered)).

### What it can do

In short, nearly everything a LINE desktop user does day to day:

- **Sign in** with a QR code or email and password. The session survives restarts.
- **Chats**: the chat list with unread counts, history with paging, sending text with LINE emoji and mentions, replies, reactions, unsending, read receipts, announcements, forwarding, local folders and pins, mutes, drafts, in-chat and cross-chat search.
- **Letter Sealing**: LINE's end-to-end encryption, versions 1 and 2, for one-to-one and group chats.
- **Media**: sending and receiving photos, videos, audio and files (encrypted), video playback, camera video messages, screenshots, clipboard paste, stickers, LINE emoji (including animated).
- **Official-account content**: Flex Messages, rich messages (imagemaps) and LINE apps (LIFF) in an in-app browser.
- **People**: friend lookup by LINE ID, phone or QR link, favorites, display names, hiding and blocking, profile cards, friend requests, your own profile and avatar.
- **Groups**: create, rename, invite, remove, invitation links and QR codes, leave, accept and decline invitations.
- **Shared content**: albums, notes and poll references.
- **Calls**: one-to-one voice and video calls, mute, camera toggle, screen sharing, full-screen video, RNNoise noise suppression.
- **Desktop integration**: a tray icon, desktop notifications, NES-style chiptune sounds, keyboard shortcuts, reduced motion, Wayland and X11.

The user-facing details of each feature are in the [project README](../../README.md). The feature comparison with LINE's Windows and Mac apps is in [analysis/desktop-features.md](../../analysis/desktop-features.md).

## Words you will meet everywhere

| Word | Meaning |
| ---- | ------- |
| **MID** | LINE's ID for a user, group or room. It is 33 characters: a letter giving the kind, then 32 hexadecimal digits. `u…` is a user (including official accounts), `c…` a group, `r…` a multi-person room. A one-to-one chat is identified by the other person's `u…` MID. Your own MID identifies your *Keep Memo* chat. |
| **RPC** | A remote call to one of LINE's services, such as `sendMessage`. LINE's RPCs are Apache Thrift calls sent as HTTP POSTs. See [protocol basics](protocol-basics.md). |
| **Thrift field ID** | Thrift structs number their fields. The code writes and reads fields by number, never by name. "Message field 4" means the message ID. |
| **Operation** | LINE's record of one change to your account: a received message, a message you sent from another device, a profile change, an incoming call. Each has a type number. See [Sync](sync.md). |
| **Revision** | The sequence number of the latest operation the client has seen. Sync asks LINE for everything after it. |
| **Long poll** | A sync request LINE holds open until something happens, so news arrives in about one network round trip. |
| **Request sequence (`seq`)** | A per-device counter that every state-changing RPC carries. It must never repeat, so the client reserves numbers on disk first. |
| **Letter Sealing** | LINE's end-to-end encryption for messages. See [Letter Sealing](letter-sealing.md). |
| **Key chain** | The account's Letter Sealing private keys, which the phone encrypts to a new desktop during sign-in. |
| **Key generation** | One version of a Letter Sealing key pair, numbered by a key ID. Old messages name the generation they were sealed with. |
| **OBS** | LINE's object storage at `obs.line-apps.com`, where attachments and profile pictures live. See [Media](media.md). |
| **Sticon** | LINE's name for LINE emoji, the small pictures inside text. |
| **Flex Message** | A card described in JSON that official accounts send. |
| **Rich message** | An image with tappable areas (LINE's *imagemap*), also from official accounts. |
| **LIFF** | *LINE Front-end Framework*: a web page that runs inside LINE with the user's identity, such as a coupon or a member card. See [LIFF](liff.md). |
| **PLANET** | LINE's call signalling system, which carries protobuf messages over UDP to a relay. See [Calls](calls.md). |
| **PDTP** | LINE's reliable stream protocol inside a call, used for screen sharing. |
| **Worker** | The second Avernella process, which holds your tokens and keys and does all network work. See [Architecture](architecture.md). |
| **`J`** | The code's short name for `struct json_object` from json-c, used for every piece of structured data. |

## The repository

```
avernella/
├── Makefile               build, test, install, format and lint targets
├── README.md              user-facing documentation
├── src/                   all C sources (about 49,000 lines)
│   └── render/            the optional native Vulkan renderer module and its shaders
├── tests/                 offline GLib tests, one file per module, plus benchmarks
├── data/                  icons, the welcome portrait and the .desktop file
├── docs/                  this documentation (en/ and th/)
├── analysis/              reverse-engineering evidence and third-party license notices
├── .clang-format          code formatter settings
├── .checkpatch-typedefs   library types for the kernel's checkpatch.pl
└── .github/workflows/     packaging automation
```

`make` produces up to four programs in the repository root:

| Output | What it is |
| ------ | ---------- |
| `avernella` | The client. The same executable runs both the window (`avernella`) and the network worker (`avernella --worker`). |
| `avernella-vulkan.so` | The optional native Vulkan renderer, loaded with `dlopen` when it is found beside the executable or in `../lib/avernella`. Built only when the Vulkan SDK pieces are installed. |
| `avernella-liff` | The in-app browser for LINE apps, built on WPE WebKit. Built only when WPE is installed. |
| `avernella-liff-gtk` | The same browser built on WebKitGTK 6.0, used as a fallback. |

### Source files by area

The full map, with what each file does, is in [Architecture](architecture.md#where-everything-lives). As a first orientation:

- `main.c` starts everything. It is also the worker's main loop and its request dispatcher.
- `ui.c` is the window's main loop, the worker connection and keyboard and mouse handling. Every other `ui_*.c` file is one screen, panel or drawing service.
- `transport.c` and `thrift.c` are how every RPC reaches LINE.
- `pair.c`, `password_login.c` and `state.c` are signing in and the saved session.
- `talk_sync.c` and `live_sync.c` receive changes.
- `messaging.c`, `sealing.c`, `media*.c`, `obs_http.c`, `friends.c`, `groups.c`, `stickers.c`, `emoji.c`, `shared_content.c` and `liff.c` each implement one family of LINE services.
- `call*.c`, `camera*.c`, `screencast.c` are calls.

### How big things are

The largest files are `ui_media.c` (attachments, the photo viewer and the call stage, about 4,400 lines), `ui_draw.c` (drawing and layout, about 4,000), `call.c` (the call engine, about 2,700), `ui.c` (about 2,600) and `main.c` (about 2,300). Most protocol modules are 200 to 1,300 lines.

## What the app deliberately doesn't do

- **No local message database.** History, drafts and the chat list live in memory and are fetched again from LINE. Only the session, chat preferences and a few settings are written to disk.
- **No LINE code.** Nothing from LINE's apps is linked or executed. The browser helper runs LINE apps' *web* pages, as LINE's own in-app browser does.
- **Not a server or a bot.** It is a personal desktop client for a person's own account.
- **Some features LINE doesn't offer desktop clients**: presence, typing indicators, server-side folders and pins. See the README for the full list.

## Where to go next

- To build and run it: [Building, running and debugging](building.md).
- To understand the moving parts before changing anything: [Architecture](architecture.md).
- To change something on screen: [The user interface](ui.md).
- To change what is sent to LINE: start with [LINE protocol basics](protocol-basics.md).
