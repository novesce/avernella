# Development

## Build

Install the packages listed in the [README](../README.md#build-and-test), then build from the repository directory.

```sh
make            # avernella, plus helpers for installed WebKit SDKs
make check      # every test in tests/
make bench      # render benchmark
```

| Option | Effect |
| ------ | ------ |
| `DEBUG=1` | Unstripped `-Og -g3` build with frame pointers and unwind tables. Run `make clean` when switching. |
| `LIFF_BROWSER=0` | Leaves out both in-app browser helpers |
| `CC=…` | Selects another GCC driver. The Makefile refuses non-GCC compilers. |
| `-jN` | Overrides the default of one job per CPU from `nproc` |

The release build uses `-Oz -flto -fno-plt -fvisibility=hidden`, `--gc-sections`, `-z separate-code`, `--rosegment` and packed relative relocations when the toolchain supports them, and strips the result. Pixel loops in `ui_pixels.c`, `camera_pixels.c` and `screencast.c` are built with `-O3`.

`avernella` looks for the browser helpers beside its executable and then in `../lib/avernella`, where `make install` puts them. It prefers `avernella-liff` (WPE WebKit) on Wayland and falls back to `avernella-liff-gtk` (WebKitGTK 6.0). Any `PREFIX` works without rebuilding. `make install` also installs `data/avernella.desktop` and `data/avernella.svg`, and the window reports the app ID `avernella`, so desktops match it to that menu entry. The Arch Linux package, `avernella-git`, lives in its own repository, [avernella-pkgbuild](https://github.com/novesce/avernella-pkgbuild), and builds the latest commit of this one.

### Vulkan descriptor-heap prototype

The opt-in offscreen test builds the C renderer in `src/render/` with GCC and volk. It requires Vulkan headers and volk with `VK_EXT_descriptor_heap` support, glslang and SPIR-V Tools. It embeds the validated shader binaries at build time. With a volk source checkout, pass `VOLK_DIR=/path/to/volk` to build against it.

```sh
make vulkan-smoke VK_VENDOR=0x10de
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation make vulkan-smoke VK_VENDOR=0x1002
```

The offscreen test exercises Vulkan 1.4 device selection, shader device addresses, mapped descriptor heaps, host image copy and a single instanced dynamic-rendering draw. It checks rounded fills, borders, glyph coverage, RGBA sampling and clipping by reading back pixels. The render code prefers device-local, host-coherent memory and handles noncoherent mapped memory.

The standalone swapchain preview uses the same pipeline. It logs the GPU, SDL backend, logical and drawable sizes, and the time spent on the host preparing and submitting frames. Pass `--test` for a 60-frame test that resizes twice, hides briefly, and checks that no frames were submitted while hidden. Preview presentation requires `VK_KHR_swapchain_maintenance1`.

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 \
    make vulkan-preview VK_VENDOR=0x10de VK_PREVIEW_ARGS=--test
LINE_RENDER_LOG=1 ./avernella
```

The second command reports production LINE's SDL renderer and any runtime fallback. The descriptor-heap renderer currently serves the standalone preview. Live chat layout, the glyph atlas and WPE DMA-BUF import remain separate integration work.

## Tests

Each `tests/<name>.c` builds to `build/test-<name>` and uses GLib's test framework. The tests run offline:

- **RPC fixtures.** Protocol tests replace `line_rpc` with a fixture that checks the Thrift-encoded request field by field and feeds a synthetic reply through the real decoder (see `tests/friends.c` and `tests/liff.c`).
- **Wrapped libcurl.** HTTP tests wrap `curl_easy_*` with `-Wl,--wrap`.
- **Wrapped SDL audio.** Call tests wrap the SDL audio functions.
- **Generated media.** Media tests generate their own pictures, audio and video. FFmpeg is needed for the recording tests.

Run one test, or one case of it:

```sh
make build/test-liff && ./build/test-liff
./build/test-sealing -p /sealing/authenticated-history-cache
```

To add a test, write `tests/foo.c`, add `build/test-foo` to `TESTS` in the Makefile, and add a rule listing its objects.

## Code style

The sources follow the Linux kernel coding style: tabs, 80 columns, K&R braces and `/* */` comments.

```sh
make format         # apply .clang-format
make format-check   # check with clang-format
make checkpatch     # the kernel's checkpatch.pl, one file per run
```

`checkpatch.pl` comes from the kernel headers of the running kernel. After a kernel upgrade, reboot or point `CHECKPATCH` at the new copy:

```sh
make checkpatch CHECKPATCH=/usr/lib/modules/<version>/build/scripts/checkpatch.pl
```

`.checkpatch-typedefs` lists library types checkpatch doesn't know. The Makefile ignores the kernel-only rules that don't fit a GLib program, and allows split strings and missing spaces in test fixtures.

## Environment variables

| Variable | Effect |
| -------- | ------ |
| `LINE_DEBUG_SESSION=1` | Report token renewals |
| `LINE_DEBUG_SYNC=1` | Report each long-poll sync round |
| `LINE_DEBUG_TIMING=1` | Print `line-timing <method> <ms>` for each worker request |
| `LINE_DEBUG_MEDIA=1` | Report failed image downloads with their HTTP status and decode errors |
| `LINE_DEBUG_LIFF=1` | Print a LINE app page's console and native-bridge calls, without the page address, which carries tokens |
| `LINE_CALL_DEBUG=1` | Report call negotiation, control messages, packet counts and first frames |
| `LINE_CALL_PDTP_DUMP=<dir>` | Append each in-order PDTP call stream to `<dir>/<service>-<stream>.bin` for analysis |
| `LINE_EXPERIMENTAL_VIDEO=0` | Turn off video calls |
| `LINE_NOISE_SUPPRESSION=0` | Send the raw microphone in calls, without RNNoise |
| `LINE_CALL_VIDEO_KBPS` | Override the video sending bitrate |
| `LINE_CAMERA_DEVICE` | Camera device (default `/dev/video0`) |
| `LINE_SHARE_WIDTH`, `LINE_SHARE_HEIGHT`, `LINE_SHARE_FPS`, `LINE_SHARE_KBPS` | Cap screen-sharing capture and encoding |
| `LINE_SHARE_TRANSPORT=presentation` | Send screen sharing as a separate native presentation |
| `LINE_DISABLE_GPU=1` | Draw through the SDL window surface instead of Vulkan |
| `LINE_DISABLE_TRAY=1` | Quit when the window closes instead of hiding in the tray |
| `LINE_REDUCE_MOTION=1` | Turn off animation |
| `SDL_VIDEODRIVER=x11` | Use X11 instead of Wayland |
| `LINE_BENCH_VIDEODRIVER`, `LINE_BENCH_DAMAGE=1` | Render benchmark driver and damage-region check |

## Debugging

- **Worker failures** show in the status line, the request's error text. Run with `LINE_DEBUG_TIMING=1` to see which request failed or was slow.
- **A blank card picture:** run with `LINE_DEBUG_MEDIA=1`.
- **A LINE app that stays on its loading screen:** run with `LINE_DEBUG_LIFF=1` and open it again. The SDK's own console messages usually say what it's waiting for.
- **Stepping through code:** build with `DEBUG=1`, then attach `gdb` to the UI or to `avernella --worker`.
- **Testing `avernella-liff` alone:** start it with a `SOCK_SEQPACKET` socket on fd 3 and the parent PID as its only argument, then send it `L full <url>` datagrams. With `LINE_DEBUG_LIFF=1` it also loads pages from `http://127.0.0.1:<port>/`, so a local page that loads the real LIFF SDK can test the bridge.

## Reverse engineering LINE

The protocol was recovered from LINE for Android 26.16.0. The `.apkm` bundle sits at the repository root and git ignores it.

1. **Extract the code.** Unzip `base.apk` from the bundle, then the `classes*.dex` files from `base.apk`. The ABI splits contain the native libraries, such as `libandromeda.so` for calls.
2. **Read Thrift structs.** LINE's code is obfuscated, so classes have names like `lf0/n1`, but Thrift-generated classes keep their field tables. Each struct's `<clinit>` constructs `TField(name, type, id)` objects with constant arguments, and each enum's `<clinit>` constructs `(NAME, ordinal, value)`. A small script that walks the DEX `const-string`, `const` and `invoke-direct <init>` instructions in each `<clinit>` recovers every struct's field names, types and IDs, and every enum's values.
3. **Read service clients.** Service clients reference the method name as a string (`"issueLiffView"`) next to the `_args` and `_result` classes they build. The result class's fields show the success type and the exceptions.
4. **Read JSON models.** Kotlin serialization descriptors list the JSON keys of non-Thrift models.
5. **Trace behavior.** A small Dalvik disassembler that prints `const-string`, `new-instance`, field access and `invoke` lines is enough to follow how values flow, for example which response fields become URL parameters.
6. **Cross-check public sources.** Where LINE publishes code, such as the LIFF SDK at `https://static.line-scdn.net/liff/edge/2/sdk.js`, it shows exactly what a page expects from the client.

Write down what you find in `analysis/`, with class names, field IDs and the evidence, as the existing notes do.
