# Contributing

[Index](README.md) · Previous: [Testing](testing.md)

This page collects the conventions the code follows, step-by-step recipes for common changes, the mistakes that are easy to make, and how the protocol was recovered.

## Code style

The C sources and tests follow the [Linux kernel coding style](https://docs.kernel.org/process/coding-style.html): tabs for indentation, 80 columns, K&R braces, `/* */` comments, and lowercase names with underscores.

```sh
make format         # apply .clang-format to every source
make format-check   # fail if anything isn't formatted
make checkpatch     # the kernel's checkpatch.pl, one file per run
```

- `clang-format` was tested with version 23. Long tables are protected with `/* clang-format off */` … `/* clang-format on */` where needed.
- `checkpatch.pl` comes from your kernel's headers (`linux-headers` on Arch, `linux-cachyos-headers` or your kernel variant's package on CachyOS). After a kernel upgrade, reboot or set `CHECKPATCH=/usr/lib/modules/<version>/build/scripts/checkpatch.pl`.
- `.checkpatch-typedefs` lists GLib, SDL and other library types that checkpatch doesn't know.
- The Makefile ignores kernel-only rules that don't fit a GLib program (`NEW_TYPEDEFS`, `AVOID_EXTERNS`, `EMBEDDED_FUNCTION_NAME`, `FILE_PATH_CHANGES`, `REPEATED_WORD`, `MACRO_WITH_FLOW_CONTROL`), and for tests also `SPLIT_STRING` and `MISSING_SPACE`.
- Every file starts with an SPDX line: `// SPDX-License-Identifier: GPL-3.0-or-later` in `.c` files and `/* SPDX-License-Identifier: GPL-3.0-or-later */` in headers.
- The code builds cleanly with `-Wall -Wextra -Wpedantic -Wformat=2`. Keep it that way.

## Conventions

### Memory and ownership

- Use GLib's automatic cleanup: `g_autoptr(J)`, `g_autoptr(GError)`, `g_autofree char *`. `line.h` defines `G_DEFINE_AUTOPTR_CLEANUP_FUNC(J, json_object_put)`.
- Function comments say who owns a result. "Caller owns" means you must free it. "Borrowed" means you must not.
- json-c: `json_object_object_add()` **takes** ownership of the value. To add an object you keep, pass `json_object_get(value)`. `field()` and `strfield()` return borrowed pointers.
- Wipe secrets when done: `OPENSSL_cleanse()` or `explicit_bzero()` for keys, passwords, tokens and decrypted payloads. Look at how `sealing.c` and `password_login.c` do it.

### Errors

- Functions that can fail take `GError **error` last and return `NULL` or `FALSE` on failure.
- Use the `LINE_ERROR` domain and the codes in `line.h` when the caller needs to tell failures apart. Code 1 is a generic error.
- **Error messages are shown to the user as they are.** Write them as short, plain sentences that say what happened and, when possible, what to do: "Select a conversation from your chat list." or "The QR code expired. Choose sign in to request a new one."
- After calling a service with its own error enum (RelationService, ShopService, token refresh), reset `s->service_code = -1`, so callers that read TalkService codes aren't misled.

### Thread rules

- **UI:** only the main thread touches `ui` and module state. Other threads post SDL user events.
- **Worker:** only the main thread touches `Line` (the session). The sync, image, call and screencast threads get copies of what they need.
- Never give the call thread, the image thread or the UI the account's access token.

### Input from LINE is untrusted

Check every value before use: types (`strfield()` returns `NULL` unless the value is a string), lengths, character sets (MIDs, IDs, tokens), ranges and URLs. Look at `talk_id()`, `valid_mid()`, `line_emoji_id_valid()` and `route_token()` for the style. Reject rather than repair.

### Input from the UI is untrusted too

The worker treats the UI as less trusted than itself. It re-validates parameters, looks messages up in its own copy (`w->messages`) instead of accepting message contents from the UI, and allows actions only on chats, friends, lookup results and cards it has itself shown.

### User-visible text

Wrap interface strings in `_()` (or `N_()` in static tables) so they can be translated via `src/ui_lang.c`. Worker error messages travel to the UI as text.

## Recipe: calling a new LINE RPC

Say LINE has `TalkService.getSomething(1: GetSomethingRequest request)` and you want a "Something" panel.

1. **Find the definition.** Get the method's argument and result field IDs and types from LINE's code (see [below](#how-the-protocol-was-recovered)) or a reference such as LINEJS's `line.thrift`. Write them down in `analysis/` if they're new.
2. **Add a protocol function** in the right module, for example `messaging.c`:

   ```c
   J *line_something(Line *s, const char *chat, GError **error)
   {
   	if (!talk_id(chat)) {
   		g_set_error_literal(error, LINE_ERROR, 1, "Select a conversation.");
   		return NULL;
   	}
   	g_autoptr(J) args = obj();
   	J *req = obj();

   	json_object_object_add(args, "1:obj", req);
   	json_object_object_add(req, "1:str", json_object_new_string(chat));
   	g_autoptr(J) reply = line_rpc(s, "/S4", "getSomething", args, error);
   	if (!reply)
   		return NULL;
   	/* Validate and convert to a small row for the UI. */
   	J *row = obj();
   	const char *name = strfield(reply, "2");

   	json_object_object_add(row, "name", json_object_new_string(name ? name : ""));
   	return row;
   }
   ```

   Declare it in the module's header, with an ownership comment.
3. **If it changes state,** take a request sequence: reserve it in `dispatch()` with `reserve_sequence()` and pass it in (as `groups.c` does), or let the function do `++s->seq` after `dispatch()` reserved it (as `messaging.c` does). Never send a mutating RPC without a reserved sequence.
4. **Expose it to the UI** with a new branch in `dispatch()` in `main.c`. Validate the parameters there too.
5. **Write a test** with the `line_rpc` fixture (see [Testing](testing.md#1-replacing-line_rpc)): the exact request, a good reply, a service error, invalid input.
6. **Call it from the UI** with `ui_rpc("something", params, tag)` and handle the reply in your module's `_reply()` function.
7. **Update the docs**: the method tables in [Protocol basics](protocol-basics.md#service-paths) and [Architecture](architecture.md#worker-methods), and the page for that area, in both `docs/en/` and `docs/th/`.

## Recipe: handling a new operation type

1. Find the OpType number and what its params mean.
2. If it can patch the UI in place, add a case to `social_operation()` (or a new function called from `live_operations()`) in `main.c` that emits a small event. Otherwise do nothing: unhandled operations already trigger a full refresh.
3. Handle the event in `handle_packet()` in `ui.c` or the relevant module.
4. Add a test case to `tests/live_sync.c` or `tests/talk_sync.c` if the paging logic is involved.
5. Add it to the table in [Sync](sync.md#operation-types-the-client-handles).

## Recipe: adding a UI feature

See [The user interface: adding a screen or overlay](ui.md#adding-a-screen-or-overlay).

## Common pitfalls

- **Reusing a request sequence.** Any mutating RPC without `reserve_sequence()` (or with an extra `++s->seq`) can make LINE drop or duplicate an action. Read [the request sequence](sign-in.md#the-request-sequence).
- **Putting secrets in metadata.** `contentMetadata` isn't encrypted. Confidential data belongs in the sealed payload.
- **Byte offsets instead of UTF-16 offsets.** Emoji and mention ranges are UTF-16 code units. Thai text and emoji make the difference visible.
- **Forgetting that `json_object_object_add()` takes ownership.** Adding a borrowed object without `json_object_get()` causes a double free later.
- **Treating any service code as a TalkException.** Reset `s->service_code` after other services.
- **Blocking the UI thread.** No file or network I/O on the main UI thread. Ask the worker, or use a helper thread that posts an SDL event.
- **Breaking scroll reuse.** If your new content can change on its own (animation, delayed loading), make `reusable_history()` return false while it is visible, or scrolling will show stale pixels.
- **Assuming the Vulkan module is present.** Test with `LINE_DISABLE_GPU=1` too.
- **Testing only one theme or width.** Check the LINE and Avernella themes, a compact window (≤ 640 px) and reduced motion.
- **Leaking tokens in logs.** Debug output must never print tokens, keys, LIFF page addresses or message contents.
- **Running a second instance.** The session lock refuses it. Close the first window, including from the tray.
- **Switching `DEBUG=1` without `make clean`.** Objects aren't rebuilt for a flag change.
- **Editing `build/` files.** They are generated. Change the sources and the Makefile instead.

## How the protocol was recovered

The protocol comes from LINE for Android 26.16.0 (`jp.naver.line.android`). The `.apkm` bundle is kept at the repository root and ignored by git. Never commit it.

1. **Extract the code.** Unzip `base.apk` from the bundle, then the `classes*.dex` files from `base.apk`. The ABI splits contain the native libraries, such as `libandromeda.so` for calls.
2. **Read Thrift structs.** LINE's code is obfuscated (classes have names like `lf0/n1`), but Thrift-generated classes keep their field tables. Each struct's static initializer (`<clinit>`) constructs `TField(name, type, id)` objects with constant arguments, and each enum's constructs `(NAME, ordinal, value)`. A small script that walks the DEX `const-string`, `const` and `invoke-direct <init>` instructions in each `<clinit>` recovers every struct's field names, types and IDs, and every enum's values.
3. **Read service clients.** Service clients reference the method name as a string (`"issueLiffView"`) next to the `_args` and `_result` classes they build. The result class's fields show the success type and the exceptions.
4. **Read JSON models.** Kotlin serialization descriptors list the JSON keys of non-Thrift models, such as the rich-message markup.
5. **Trace behavior.** A small Dalvik disassembler that prints `const-string`, `new-instance`, field access and `invoke` lines is enough to follow how values flow, for example which response fields become URL parameters.
6. **Native code.** Call signalling and media formats were read from `libandromeda.so`: its protobuf-c descriptors keep field names and numbers, and specific functions were disassembled or run under CPU emulation with synthetic inputs.
7. **Cross-check public sources.** LINE publishes some client code, such as the LIFF SDK at `https://static.line-scdn.net/liff/edge/2/sdk.js`. The open interoperability projects ([line-rs](https://github.com/softnetics/line-rs/tree/addf69c2e886a2364a396ce360093a62119dcb32), [LINEJS](https://github.com/evex-dev/linejs/tree/v3.6.5), Beeper's LINE bridge) were used to confirm formats. Their MIT notices are in `analysis/`.

Write down what you find in `analysis/`, with class names, field IDs, addresses and the evidence, as the existing notes do. Keep personal data (accounts, phone numbers, message contents, tokens) out of notes, tests and commits.

## Before you send a change

- [ ] `make` builds without warnings.
- [ ] `make check` passes.
- [ ] `make format-check` and `make checkpatch` pass.
- [ ] New protocol code has a fixture test.
- [ ] New UI works in both themes, at compact width and with `LINE_DISABLE_GPU=1`.
- [ ] Docs are updated, in **both** [`docs/en/`](README.md) and [`docs/th/`](../th/README.md).

## License

Avernella is released under the GNU General Public License, version 3 or (at your option) any later version. The NES sound-chip emulation in `chiptune.c` is ported from Mesen under the same license. Contributions are accepted under the same terms. See the [README](../../README.md#license).
