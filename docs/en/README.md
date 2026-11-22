# Avernella developer documentation (English)

[ภาษาไทย](../th/README.md)

Avernella is an independent LINE desktop client for GNU/Linux, written in C. These pages are for programmers, and for AI coding agents such as Claude or Codex, who are new to the repository and need to work on it. They explain how the app is built, how each feature works and where it lives in `src/`, and how LINE's service works at the level this client speaks it.

Read them in order the first time. Later, jump straight to the page you need.

| # | Page | What you learn |
| - | ---- | -------------- |
| 1 | [Overview](overview.md) | What Avernella is, the words you will meet everywhere (MID, operation, revision, Letter Sealing, OBS, LIFF), and a map of the repository |
| 2 | [Building, running and debugging](building.md) | Dependencies, `make` targets and options, installing, environment variables, debug switches |
| 3 | [Architecture](architecture.md) | The UI process, the worker process and the browser helper, their threads, the JSON channel between them, saved files and the Linux facilities the app relies on |
| 4 | [The user interface](ui.md) | The event loop, the drawing pipeline (Cairo, Pango, SDL, the Vulkan module, damage and scroll reuse), themes, and every screen and feature with the file that implements it |
| 5 | [LINE protocol basics](protocol-basics.md) | Servers and endpoints, Thrift Compact and Binary encoding, the JSON form the code uses for Thrift, HTTP headers, errors, token renewal and rate limits |
| 6 | [Signing in and the saved session](sign-in.md) | QR pairing, email and password sign-in, the session file, sign-out |
| 7 | [Sync: how changes arrive](sync.md) | Operations and revisions, the long poll, resync, the fallback revision check, the operation types the client handles |
| 8 | [Messages](messaging.md) | The chat list, history paging, the Message struct, content types, sending, replies, mentions, emoji, reactions, unsend, read receipts, announcements, forwarding |
| 9 | [Letter Sealing (end-to-end encryption)](letter-sealing.md) | Key chains, key generations, v1 and v2 message formats, groups, caching |
| 10 | [Media, stickers, emoji and rich content](media.md) | Encrypted attachments on the object store, previews, profile pictures, stickers, LINE emoji, Flex Messages, rich messages, albums and notes |
| 11 | [Friends, profiles and groups](social.md) | Contact lookup and adding, favorites, hiding, blocking, profiles, friend requests, groups and invitations |
| 12 | [LINE apps (LIFF) and the in-app browser](liff.md) | `issueLiffView`, token hand-off, the native bridge, the browser helper process |
| 13 | [Calls](calls.md) | Call routes, PLANET signalling and its encryption, SRTP keys, Opus audio, VP8 video in EVS3 packets, PDTP and screen sharing, incoming calls |
| 14 | [Testing](testing.md) | How the offline tests work, how to run one, how to write a new one, benchmarks and GPU tests |
| 15 | [Contributing](contributing.md) | Code style, the project's conventions, recipes for adding an RPC or a UI feature, common pitfalls, reverse-engineering method |

## Other material

- [Project README](../../README.md): the user-facing feature list, shortcuts and packages.
- [`analysis/`](../../analysis): the reverse-engineering evidence behind the protocol notes. [Android build analysis](../../analysis/portability.md), [video call wire format](../../analysis/video-call-wire.md), [LIFF](../../analysis/liff.md), [desktop feature comparison](../../analysis/desktop-features.md).
- The older single-page notes, [architecture.md](../architecture.md), [line-protocol.md](../line-protocol.md) and [development.md](../development.md), now point into these pages.

## A note on accuracy

LINE doesn't publish this protocol, and it can change it. Everything here was checked against the source in this repository when it was written. When the docs and the code disagree, the code wins: fix the docs in the same change. Field numbers are written as LINE's Thrift field IDs, such as "field 4". The functions named in each section are the place to confirm a detail.

Avernella is an independent project. LINE is a trademark of LY Corporation.
