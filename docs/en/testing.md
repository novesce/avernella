# Testing

[Index](README.md) · Previous: [Calls](calls.md) · Next: [Contributing](contributing.md)

The automated tests run **offline**. None of them talk to LINE, and none need an account. They use synthetic accounts, keys, pictures, audio and video, and replace the network or sound card at the edge of the module under test.

## Running tests

```sh
make check                      # build and run everything
make build/test-liff            # build one test
./build/test-liff               # run it
./build/test-sealing -p /sealing/authenticated-history-cache   # run one case
./build/test-sealing -l         # list the cases
./build/test-sealing --verbose  # print each case as it runs
```

`make check` runs the tests one after another and stops at the first failure. The tests use [GLib's test framework](https://docs.gtk.org/glib/testing.html), so all of GLib's options work, including `-m perf` for performance cases (`./build/test-sealing -m perf -p /sealing/authenticated-history-cache`).

Some tests need extra tools at run time:

- FFmpeg (`ffmpeg`, `ffprobe`) for `test-video-record` and `test-video-play`.
- SDL's dummy video driver for the UI tests (they set it themselves).

## Group video and screen sharing

Supply either the LINE **26.16.0 APKM** (version code `261600106`, with its ARM64 split) or the **26.18.2 base APK**. Use the APKM for the native packet comparison. That check needs LLVM’s `llvm-objdump` on your PATH. Both use Python 3 and the usual project build dependencies.

```sh
make check-group-media LINE_APK="/path/to/LINE-26.16.0.apkm"
make check-group-media LINE_APK="/path/to/LINE-26.18.2.apk"
```

The harness recognizes the artifact by SHA-256, so the filename can change. It reads the official DEX instructions for camera state, participant selection and presentation callbacks in each build. For the APKM it also inspects the matching ARM64 call library and compares its packet format with 30 synthetic pictures from the Linux sender. Each of five conference formats switches from camera to screen and back. Temporary copies of the nested APKs are removed after inspection. The regular `make check` runs without either download.

The fixtures run Avernella’s call worker against a loopback relay. They check participant focus, the sharing indicator on decoded frames, missing or late source metadata, late viewers of a static share, camera restoration, a canceled picker, one-to-one presentation start and stop, and the group share controls. For a native group presentation the relay waits for the Linux receiver to request the presenter before sending a synthetic VP8 picture. The fixture checks its decoded pixels and stops the presentation. The scene test checks that the shared screen and camera appear together and that a delayed frame cannot reopen a stopped share. A missing or skipped required case fails the run.

With the Python `unicorn` module installed, you can also run the control reader and writer from the supplied Android ARM64 split against the Linux receiver’s requests. Build the fixture binaries first with `make check-group-media`.

```sh
python3 tests/group_media_apk.py --execute-presentation-codec "/path/to/LINE-26.16.0.apkm"
```

To exercise artifact selection and its rejection paths with both files available, run:

```sh
python3 tests/test_group_media_artifact.py "/path/to/LINE-26.16.0.apkm" "/path/to/LINE-26.18.2.apk"
```

The loopback receiver can decode Avernella’s media without using the public relay. To verify delivery on real devices, build and open the current Avernella binary and start a group video call with two consenting Android participants. Run the following interactive check in a terminal you can share. It checks each direction, the switch back to camera and whether the viewers can read a changing code from the shared terminal.

```sh
python3 tests/group_media_live.py
```

Add `--native-presentation` to the handset check to verify that Android’s **Share screen** action opens a separate tile on Linux, that it updates when the presenter scrolls, and that it closes on stop. Skipped or failed requested checks return a nonzero exit status.

## The tests

Each `tests/<name>.c` builds to `build/test-<name>` (with a few historical names, listed below). The rule for each test in the [Makefile](../../Makefile) lists exactly which objects it links, which keeps the test small and shows what it depends on.

| Test binary | Source | Covers |
| ----------- | ------ | ------ |
| `test-thrift` | `thrift.c` | Compact and Binary encoding and decoding, limits, malformed input |
| `test-transport` | `transport.c` | Headers, token rotation, renewal on code 119, refresh rejection, `Retry-After` (wrapped libcurl) |
| `test-state` | `state.c` | Session load and save, permissions, size limits, the sequence reservation, migration |
| `test-pair` | `pair.c` | QR pairing flow, PIN path, key import failure |
| `test-password-login` | `password_login.c` | Email sign-in: RSA wrapping, PIN secret, verifier and confirmation |
| `test-sealing` | `sealing.c` | Key chain import and verification, v1 and v2 direct messages, groups, two key generations, mismatched IDs, authentication failures, the secret cache |
| `test-messaging` | `messaging.c` | Chat list, history rows, sending, replies, mentions, reactions, receipts, announcements |
| `test-emoji-message` | `emoji_message.c` | Sealed LINE emoji in text, including Thai and astral Unicode |
| `test-media` | `media.c` | Attachment envelopes, previews, video hash objects, file names, rejected uploads, corruption, metadata relabeling |
| `test-obs` | `obs_http.c` | OBS requests and headers (wrapped libcurl) |
| `test-media-io` | `media_io.c` | Leases, sealing, frame passing, lease descriptors passed with their JSON line (also split into 1-byte reads), a non-dumpable worker |
| `test-gpu-health` | `gpu_health.c` | NVIDIA GPU health on synthetic sysfs trees (healthy, no driver, no display device, passthrough, GPU gone, no NVIDIA driver); the start-up switch and the user's own Vulkan driver settings |
| `test-udp-batch` | `udp_batch.c` | `sendto`, `sendmmsg`, `UDP_SEGMENT`, `recvfrom` and `recvmmsg` over loopback deliver the same datagrams in order; fallback when the kernel refuses `UDP_SEGMENT` and inside a seccomp filter that refuses `sendmmsg` and `recvmmsg`; `-m perf` CPU time per datagram |
| `test-sandbox` | `sandbox.c` | In a child process: what the worker sandbox refuses (exec, namespaces, ptrace, io_uring, clone3, BPF, terminal-typing ioctls including with high bits set, TCP listening, outside signals), that threads started earlier are covered, that threads, fork, memfds, batched UDP and outgoing TCP keep working, `LINE_DISABLE_SANDBOX=1`, applying it twice |
| `test-friends` | `friends.c` | Lookup, add, rosters and paging, favorites, names, hide and block, requests, cards |
| `test-groups` | `groups.c` | Create, invite preflight, members, invitations, rename, links, leave |
| `test-stickers` | `stickers.c` | Catalog, entitlement, asset URLs, sending |
| `test-emoji` | `emoji.c` | Catalog, pack index, manifests, asset policy |
| `test-shared-content` | `shared_content.c` | Albums, notes, channel tokens (wrapped libcurl) |
| `test-liff` | `liff.c` | `issueLiffView`, launch URLs, consent and fallback |
| `test-rich-message` | `rich_message.c` | Imagemap markup parsing |
| `test-flex-asset` | `flex_asset.c` | Allowed hosts, public-address checks, redirects (wrapped libcurl) |
| `test-talk-sync` | `talk_sync.c` | Cursor, paging, full and partial resync |
| `test-live-sync` | `live_sync.c` | The long-poll thread, batching, token rotation, and recovery with an unchanged revision |
| `test-ui-chat-refresh` | `ui_chat_refresh.c` | A headless chat window and a synthetic worker exercise automatic and manual refresh, draft retention, and reading an earlier page during reconnect |
| `test-ui-refresh` | `ui_refresh.c` | Retries a failed Flex image, checks the decoded pixel, and exercises late failures and corrupt files |
| `test-call` | `call.c` | Whole calls against a local relay fixture (wrapped SDL audio) |
| `test-call-video` | `call_video.c` | VP8 through EVS3 and SRTP, loss, reordering, replay, keyframes, descriptors |
| `test-screencast-portal` | `screencast.c` | Portal selection, permission denial, cancellation and Cinnamon fallback policy on a private D-Bus |
| `test-screencast-cinnamon` | `screencast_cinnamon.c` | Monitor selection, display changes, screen lock and capture-session cleanup |
| `test-monitor-picker` | `ui_monitor_picker.c` | Mouse and keyboard selection, explicit confirmation, cancellation and call teardown |
| `test-audio` | `call_audio.c` | Opus and EAS2 framing, playout (wrapped `SDL_QueueAudio`) |
| `test-denoise` | `call_denoise.c` | RNNoise on synthetic audio |
| `test-video-record` | `video_record.c` | Recording a generated picture and tone, checked with ffprobe, cancellation, cleanup |
| `test-video-play` | `video_play.c` | Playback process handling |
| `test-profile-image` | `profile_image.c` | Crop, scale, transparency, JPEG output |
| `test-clipboard-paste` | `clipboard_paste.c` | Picture paste staging |
| `test-qr-scan` | `qr_scan.c` | ZBar loading and decoding (wrapped `dlopen`) |
| `test-chat-lists` | `chat_lists.c` | Folders, pins, mutes, shortcuts, file safety |
| `test-chiptune` | `chiptune.c` | The sound chip and every sound |
| `test-ui-motion` | `ui_motion.c` | Frame clock, scrolling and scroll targets, press depth, the glide curve, wheel direction and touchpad row carry, including timing at 60, 144 and 240 Hz |
| `test-ui-pixels` | `ui_pixels.c` | Premultiplication, scroll copy maths |
| `test-ui-apng` | `ui_apng.c` | Animated PNG frames and timing |
| `test-ui-emoji` | `ui_emoji.c` | Draft tokens and export ranges |
| `test-ui-flex` | `ui_flex.c` | Flex layout |
| `test-ui-friends` | `ui_friends.c` | Friends screen behavior |
| `test-ui-tray` | `ui_tray.c` | Tray item properties and menu |
| `test-ui-read` | `ui_read.c` | The read-receipt rules in `ui_private.h` |
| `test-ui-theme` | `ui_theme.c` | Theme file reading and toggling |
| `test-ui-scroll-scene` | `ui_scroll_scene.c` | Scroll reuse against full redraws; the new-message glide and chat fade-in, with and without reduced motion |
| `test-ui-call-fullscreen` | `ui_call_fullscreen.c` | Full-screen call video |
| `test-ui-lang` | `ui_lang.c` | Language selection and the translation tables, including printf conversions |

The `TESTS` list in the Makefile is the authoritative set.

## How the tests isolate a module

### 1. Replacing `line_rpc`

Protocol tests link the module under test with `thrift.o` but **not** `transport.o`, and define their own `line_rpc()`. The fixture in [`tests/friends.c`](../../tests/friends.c) is the model:

```c
struct rpc_step {
	const char *path, *method, *request, *reply;
};

J *line_rpc(Line *s, const char *path, const char *method, J *args, GError **error)
{
	const struct rpc_step *step = &script[calls++];

	g_assert_cmpstr(path, ==, step->path);
	g_assert_cmpstr(method, ==, step->method);
	/* Encode the real request with the real encoder, decode it again,
	 * and compare it field by field with the expected JSON. */
	...
	/* Encode the scripted reply as a Thrift result and decode it with the
	 * real reply decoder, so success and service errors take the real path. */
	return line_decode_reply(method, response, bytes, &s->service_code, error);
}
```

A test is then a script of expected calls:

```c
static const struct rpc_step steps[] = {
	{ "/RE4", "findContactBySearchIdOrTicketV3", FIND_ID, "{\"0:obj\": {…}}" },
	{ "/RE4", "addFriendByMid", "{…}", "{\"0:obj\": {}}" },
};
use(steps, G_N_ELEMENTS(steps));
```

This checks two things at once: the exact Thrift **request** the code sends (paths, methods, field IDs, types, values), and how the code handles realistic **replies**, including service exceptions (`"{\"1:obj\": {\"1:i32\": 10}}"`), timeouts (`"timeout"`) and unavailable methods (`"unavailable"`).

### 2. Wrapping libcurl

HTTP-level tests (`test-transport`, `test-obs`, `test-shared-content`, `test-flex-asset`) link with `-Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo`. The linker then sends calls to `__wrap_curl_easy_perform()` and friends in the test, which record the options and headers and play back a scripted status, headers and body. See [`tests/transport.c`](../../tests/transport.c).

### 3. Wrapping SDL and dlopen

The call and audio tests wrap `SDL_OpenAudioDevice`, `SDL_CloseAudioDevice` and `SDL_QueueAudio`, so no sound card is needed and the test can inspect what would have been played. `test-qr-scan` wraps `dlopen`. The UI render tests and the benchmark wrap `SDL_RenderPresent`.

### 4. Generated media and local fixtures

Media tests make their own pictures, tones and videos. The call tests run a local UDP relay that speaks enough PLANET to connect two ends, so the real call engine, SRTP and codecs run end to end on your machine.

### 5. Synthetic identities

All MIDs in tests are obviously fake (`u00000000000000000000000000000001`), and all keys are generated or fixed test vectors. Never put real tokens, keys or message contents in a test.

## Writing a new test

1. Create `tests/<name>.c`:

   ```c
   // SPDX-License-Identifier: GPL-3.0-or-later
   #include "<module>.h"

   static void my_case(void)
   {
   	g_autoptr(GError) error = NULL;
   	...
   	g_assert_no_error(error);
   }

   int main(int argc, char **argv)
   {
   	g_test_init(&argc, &argv, NULL);
   	g_test_add_func("/<name>/my-case", my_case);
   	return g_test_run();
   }
   ```

2. Add `build/test-<name>` to `TESTS` in the Makefile.
3. Add a rule listing its objects, plus any wrap flags:

   ```make
   build/test-<name>: tests/<name>.c build/<module>.o build/thrift.o
   ```

   The shared recipe `$(TESTS):` does the linking.
4. Run `make build/test-<name> && ./build/test-<name>`, then `make check`.
5. Run `make format` and `make checkpatch`. Test files may keep long JSON strings split across lines; the Makefile relaxes those checkpatch rules for `tests/`.

**What a good protocol test checks:** the exact request (so a field-number typo fails), the happy reply, at least one service error, input validation (empty, too long, wrong kind of MID), and that nothing is sent when validation fails.

## Benchmarks and renderer tests

**Render benchmark.** `make bench` renders a 920 × 630 fixture with 900 contacts and 50 messages using SDL's dummy video driver, and reports CPU render and submit times. To exercise the X11 scrolling path and compare the final partial frame with a full redraw:

```sh
xvfb-run -a env LINE_REDUCE_MOTION=1 LINE_BENCH_VIDEODRIVER=x11 LINE_BENCH_DAMAGE=1 ./build/render-bench
```

**Vulkan.** These need a GPU and the Vulkan SDK:

```sh
make vulkan-smoke                                   # offscreen: renders and reads back pixels
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation make vulkan-smoke VK_VENDOR=0x10de
AVERNELLA_VK_DESCRIPTORS=buffer make vulkan-smoke VK_VENDOR=0x1002
make vulkan-preview VK_PREVIEW_ARGS=--test          # the real presenter: resize, hide, present
```

Run them with the validation layer whenever you touch `src/render/`.

## Fuzzing

The parsers that read network data have coverage-guided fuzzers in [`fuzz/`](../../fuzz). They are not part of `make check`.

| Fuzzer | Covers |
| ------ | ------ |
| `fuzz-pdtp` | `call_pdtp_receive()`: SRTP, RTP and conference route words, packet numbers, every element, reassembly, ACKs, presenter selection |
| `fuzz-video` | `call_video_receive()`: EVS3 and RFC 7741 descriptors, fragment assembly, VP8 decode, and `call_share_decode()` |
| `fuzz-pb` | `pb_next()` and its users in `call_wire.c` and `call_schema.c`, and sealed signaling frames |
| `fuzz-group` | The group answer, presence, stream and camera requests, RTCP feedback in `call_group.c` |
| `fuzz-thrift` | The Compact and Binary readers in `thrift.c` |

```sh
make fuzz                       # build into build-fuzz/ and write the seed inputs
make fuzz-run FUZZ_TIME=600     # run every fuzzer for 10 minutes, side by side
make fuzz-run-pdtp              # one fuzzer
make fuzz-check                 # replay the seeds and saved corpus once
./build-fuzz/fuzz-pdtp build-fuzz/artifacts/pdtp-crash-…   # reproduce a failure
```

GCC has no libFuzzer, so [`fuzz/driver.c`](../../fuzz/driver.c) is the engine. GCC's `-fsanitize-coverage=trace-pc,trace-cmp` feeds it edge coverage and the operands of failed comparisons. Everything is built with AddressSanitizer and UndefinedBehaviorSanitizer in `build-fuzz/`, apart from `build/`. The harnesses use libFuzzer's `LLVMFuzzerTestOneInput()`, and the driver accepts libFuzzer's main flags (`-max_total_time`, `-runs`, `-max_len`, `-timeout`, `-artifact_prefix`). Pass extra flags with `FUZZ_ARGS`.

Each fuzzer's comment explains its input format. The PDTP and video fuzzers hold the synthetic SRTP keys of `tests/pdtp.c` and `tests/call_video.c`, so they encrypt fuzzed plaintext before it reaches the parser. [`fuzz/seeds.c`](../../fuzz/seeds.c) writes the seed inputs. It rebuilds the test fixtures and records what Avernella's own encoders send with the same synthetic keys and pictures.

New inputs collect in `build-fuzz/corpus/NAME/` and log in `build-fuzz/NAME.log`. A crash, leak, timeout or out-of-memory input is saved as `build-fuzz/artifacts/NAME-crash-HASH` (`leak-`, `timeout-`, `oom-`). Fix it with a regression test in the matching `tests/` file.

## What tests can't cover

Live interoperability with LINE: real sign-in, delivery to another account, media on LINE's servers, calls with LINE's phone and desktop apps. Those need a real account and a second device. [analysis/desktop-features.md](../../analysis/desktop-features.md) and [analysis/video-call-wire.md](../../analysis/video-call-wire.md) record what has been verified live. When you verify something live, say so there, without any personal data.
