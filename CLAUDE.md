# CLAUDE.md

Guidance for AI coding agents working in this repository. Read it first, then the developer docs in [docs/en/](docs/en/README.md) (Thai translation in [docs/th/](docs/th/README.md)).

## What this is

Avernella is an independent native LINE desktop client for GNU/Linux, written in C17 with SDL2, Cairo, Pango, GLib, json-c, libcurl and OpenSSL 3. It speaks LINE's private protocol (Thrift Compact over HTTPS, as the Windows desktop app `DESKTOPWIN 9.7.0.3556`), implements Letter Sealing end-to-end encryption, encrypted media, LIFF apps and one-to-one voice and video calls. There is no affiliation with LY Corporation.

## Build, test, lint

```sh
make                 # GCC 12+ only; Clang is refused. Parallel by default (-j$(nproc)).
make check           # every offline test in tests/, stops at the first failure
make build/test-<name> && ./build/test-<name> [-p /case/path]
make format          # clang-format, Linux kernel style
make format-check
make checkpatch      # the kernel's checkpatch.pl
make DEBUG=1         # -Og -g3 build; run `make clean` when switching DEBUG on or off
```

Optional parts build only when their SDKs exist: `avernella-vulkan.so` (Vulkan + volk + glslang), `avernella-liff` (WPE WebKit), `avernella-liff-gtk` (WebKitGTK 6.0). Full dependency and option list: [docs/en/building.md](docs/en/building.md).

## How the code is laid out

- **Two processes from one binary.** `avernella` is the UI (`ui.c` and `ui_*.c`); `avernella --worker` holds the tokens and keys and does all network work (`main.c` → `worker()`). They exchange one JSON object per line over a socketpair on the worker's stdin/stdout. `dispatch()` in `main.c` is the complete list of worker methods; `handle_packet()` / `handle_reply()` in `ui.c` handle replies and events. Large data crosses as sealed memfds ("media leases"), call video as memfds over a `SOCK_SEQPACKET` socket.
- **Protocol modules** each own one LINE service family: `transport.c` (HTTPS, token renewal), `thrift.c` (Compact and Binary codec), `pair.c` / `password_login.c` / `state.c` (sign-in, session file), `talk_sync.c` / `live_sync.c` (operations, long poll), `messaging.c`, `sealing.c`, `media*.c` + `obs_http.c`, `friends.c`, `groups.c`, `stickers.c`, `emoji.c`, `shared_content.c`, `liff.c`, `call*.c`.
- **UI modules** follow one pattern: `ui_<m>_open/_active/_event/_reply/_draw/_reset/_resume_input`. Drawing is Cairo/Pango into one backing image (`ui_draw.c`), presented by the native Vulkan module (`src/render/`, loaded with `dlopen` via `ui_gpu.c`), SDL's Vulkan renderer, or the SDL window surface.
- The file-by-file map is in [docs/en/architecture.md](docs/en/architecture.md#where-everything-lives).

## Rules that matter

- **Thrift as JSON.** Requests are json-c objects with typed keys (`"1:i32"`, `"2:str"`, `"3:obj"`, `"4:list:str"`, `"5:map:str:str"`). Decoded replies use bare numeric keys (`"1"`, `"4"`). Read with `field()` / `strfield()` from `line.h`, which return `NULL` on a missing or wrongly typed value. Result field 0 is success, field 1 the exception. Details: [docs/en/protocol-basics.md](docs/en/protocol-basics.md).
- **Request sequence.** Every state-changing RPC carries a `reqSeq` that must never repeat. Reserve it with `reserve_sequence()` in `dispatch()` before the RPC (it persists to disk first). Never add an extra `++s->seq`.
- **Secrets stay in the worker.** Never give the access token, refresh token or private keys to the UI, the call thread or the image thread. Wipe secrets with `OPENSSL_cleanse()` / `explicit_bzero()`. Never print tokens, keys, LIFF page URLs or message contents, including in debug output.
- **The worker is sandboxed** (`sandbox.c`): non-dumpable, and under seccomp it gets `EPERM` for `execve`, namespaces, `ptrace`, `bpf` and similar, and `ENOSYS` for `io_uring` and `clone3`. Code in the worker must never run programs, and media go to the UI as passed descriptors (`line_wire_send()`), never as `/proc/<pid>` paths. `LINE_DISABLE_SANDBOX=1` turns it off for debugging. See [docs/en/architecture.md](docs/en/architecture.md#the-worker-sandbox).
- **Memory hardening** (`sandbox.c`, [docs](docs/en/architecture.md#memory-hardening)): the worker, and the UI on a hardware GPU, refuse writable-and-executable memory (MDWE), so never generate code at run time. Libraries loaded at start-up are sealed with `mseal()`; never `dlclose()` one, and seal before `dlopen()`. The worker runs with an x86 shadow stack, so no stack switching or return-address tricks. Freed worker heap is overwritten (`M_PERTURB`). Thrift replies are decoded from `line_guard_copy()`. Every layer can be switched off by name in `LINE_DISABLE_SANDBOX`.
- **Look like one ordinary client.** Every request uses `LINE_APP_HEADER`, `LINE_USER_AGENT` and `line_language_tag()` from `line.h`; never hard-code another version or language. Respect `Retry-After`, keep polling gentle, and pace user-triggered bursts: friend searches and adds go through `paced()` in `main.c` (see [docs/en/social.md](docs/en/social.md#pacing-searches-and-adds)). Never add loops that search, add, message or fetch history in bulk.
- **`contentMetadata` (Message field 18) is not encrypted.** Confidential data goes in the sealed payload (`line_seal_payload()`).
- **Emoji and mention ranges are UTF-16 code units**, not bytes.
- **json-c ownership:** `json_object_object_add()` takes ownership; use `json_object_get()` to add something you keep. Prefer `g_autoptr(J)`, `g_autoptr(GError)`, `g_autofree`.
- **Errors** are `GError` in `LINE_ERROR` (codes in `line.h`) and are shown to users verbatim: write short plain sentences. After calling RelationService, ShopService or the token-refresh endpoint, reset `s->service_code = -1`.
- **Threads:** only the UI main thread touches `ui` and module state; only the worker main thread touches `Line`. Helper threads post SDL user events or eventfd wakeups.
- **Validate everything** from LINE and from the UI (MID format via `talk_id()` / `valid_mid()`, lengths, types, URLs). The worker acts only on chats, messages, friends and cards it has itself seen.
- **UI strings** are wrapped in `_()` (or `N_()` in static tables) for translation via `src/ui_lang.c`. A new string needs a `{ English, translation }` pair in every `src/ui_lang_<tag>.c` (17 languages); missing pairs fall back to English, and `tests/ui_lang.c` checks printf conversions.
- **Rendering:** content that changes on its own (animation, late-loading images) must disable scroll reuse in `reusable_history()`. Check both themes (LINE and Avernella), compact width (≤ 640 px), reduced motion, and `LINE_DISABLE_GPU=1`.

## Style

Linux kernel coding style: tabs, 80 columns, K&R braces, `/* */` comments, an SPDX `GPL-3.0-or-later` line at the top of every file, warning-free under `-Wall -Wextra -Wpedantic -Wformat=2`. Run `make format` and `make checkpatch` before finishing.

## Tests

All tests are offline GLib tests with synthetic data. Protocol tests replace `line_rpc()` with a scripted fixture that checks the exact Thrift request and feeds replies through the real decoder (model: `tests/friends.c`). HTTP tests wrap libcurl with `-Wl,--wrap`. To add one: write `tests/<name>.c`, add `build/test-<name>` to `TESTS` in the Makefile, and add a rule listing its objects. Never put real tokens, keys, MIDs or message contents in tests. See [docs/en/testing.md](docs/en/testing.md).

## Debugging switches

`LINE_DEBUG_TIMING=1`, `LINE_DEBUG_SANDBOX=1`, `LINE_DEBUG_SYNC=1`, `LINE_DEBUG_SESSION=1`, `LINE_DEBUG_MEDIA=1`, `LINE_DEBUG_LIFF=1`, `LINE_CALL_DEBUG=1`, `LINE_RENDER_LOG=1`, `LINE_DISABLE_GPU=1`. Full table: [docs/en/building.md](docs/en/building.md#environment-variables).

## Keep in sync

- When you change behavior, update the matching page in **both** `docs/en/` and `docs/th/` (same file names, same structure).
- New protocol findings (field IDs, class names, evidence) go in `analysis/`, without personal data.
- Never commit the LINE `.apkm`/`.apk` bundle, session files or anything under `build/`.
