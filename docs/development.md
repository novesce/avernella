# Development

## Build

Install the packages listed in the [README](../README.md#build-and-test), then build from the repository directory.

```sh
make            # avernella, plus helpers for installed WebKit SDKs
make check      # offline regression suite
make bench      # render benchmark
```

| Option | Effect |
| ------ | ------ |
| `DEBUG=1` | Unstripped `-Og -g3` build with frame pointers and unwind tables. Run `make clean` when switching. |
| `LIFF_BROWSER=0` | Leaves out both in-app browser helpers |
| `CC=…` | Selects another GCC driver. The Makefile refuses non-GCC compilers. |
| `-jN` | Overrides the default of one job per CPU from `nproc` |

The release build uses `-Oz -flto -fno-plt -fvisibility=hidden`, `--gc-sections`, `-z separate-code`, `--rosegment` and packed relative relocations when the toolchain supports them, and strips the result. Pixel loops in `ui_pixels.c`, `camera_pixels.c` and `screencast.c` are built with `-O3`.

`avernella` looks for the browser helpers beside its executable and then in `../lib/avernella`, where `make install` puts them. It prefers `avernella-liff` (WPE WebKit) on Wayland and falls back to `avernella-liff-gtk` (WebKitGTK 6.0). Any `PREFIX` works without rebuilding. `make install` also installs `data/avernella.desktop` and the 64 px and 256 px Avernella PNG icons in the hicolor icon theme. The window uses the app ID `avernella` so desktops match it to that menu entry. The Arch package installs these icons from the verified Ubuntu release. [avernella-bin on the AUR](https://aur.archlinux.org/packages/avernella-bin)

### Native Vulkan renderer

The application loads `avernella-vulkan.so` beside the executable or from the installed lib directory. Build it with GCC, recent volk and Vulkan headers that declare descriptor heaps, glslang and SPIR-V Tools. Set `NATIVE_VULKAN=1` to require the module or `NATIVE_VULKAN=0` for an SDL-only build. Set `VOLK_DIR` to build from a volk checkout and `VK_HEADERS` to select a Vulkan-Headers include directory.

The renderer chooses a presentation-capable device and probes its features. It tries descriptor heaps first, then descriptor buffers on Vulkan 1.3 or later. Both paths reuse image and sampler descriptors, baked SPIR-V and a single graphics pipeline. The shader uses four vertices per quad, clips before rasterization, and takes short paths for an opaque canvas, glyph masks and I420 video. Two frame slots reuse mapped upload memory. Synchronization2 barriers order texture transfers and fragment reads. [Khronos descriptor heaps](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html) · [Descriptor buffers](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_buffer.html) · [Synchronization2](https://docs.vulkan.org/guide/latest/synchronization2.html)

During eligible chat scrolling the renderer keeps history in a circular region of its existing canvas texture. It samples the retained rows in their new positions and uploads the exposed rows and any changed hover highlight. On an unrotated surface with incremental presentation, it also tracks the changes since each swapchain image was last drawn. When that image returns, the renderer restricts rasterization to the accumulated region and tells the compositor what changed since the previous presentation. The first frame after a resize repaints the whole image. [Khronos incremental presentation](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_incremental_present.html)

```sh
LINE_RENDER_LOG=1 ./avernella
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation make vulkan-smoke VK_VENDOR=0x10de
AVERNELLA_VK_DESCRIPTORS=buffer make vulkan-smoke VK_VENDOR=0x1002
make vulkan-preview VK_PREVIEW_ARGS=--test
```

The offscreen tests read back rendered pixels. The swapchain test uses the production presenter, resizes the window and checks hidden-window behavior. Use `AVERNELLA_RENDER_NODE=/dev/dri/renderD128` to select a Linux render node on drivers exposing DRM device properties. [Khronos physical device DRM](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_physical_device_drm.html)

## Tests

`make check` runs the GLib test binaries listed in `TESTS` in the Makefile. The fixtures run offline.

- **RPC fixtures.** Protocol tests replace `line_rpc` with a fixture that checks the Thrift-encoded request field by field and feeds a synthetic reply through the real decoder (see `tests/friends.c` and `tests/liff.c`).
- **Wrapped libcurl.** HTTP tests wrap `curl_easy_*` with `-Wl,--wrap`.
- **Wrapped SDL audio.** Call tests wrap the SDL audio functions.
- **Generated media.** Media tests generate their own pictures, audio and video. FFmpeg is needed for the recording tests.

Run a single test binary or select one case by its GLib test path.

```sh
make build/test-liff build/test-sealing
./build/test-liff
./build/test-sealing -p /sealing/authenticated-history-cache
```

To add a test, write `tests/foo.c`, add `build/test-foo` to `TESTS` in the Makefile, and add a rule listing its objects.

### Checking motion and repainting

Run the motion and scene tests when changing a control, its hit area, or the size of a chat row. Both run with SDL's dummy video driver.

```sh
make build/test-ui-motion build/test-ui-scroll-scene
./build/test-ui-motion
./build/test-ui-scroll-scene
```

The motion test samples a short tap at 60, 90, 120, 144 and 240 Hz. It checks the press, rebound and final deadline, then measures SDL wakeup timing separately at 60, 144 and 240 Hz. The scene test draws a hover fade frame by frame in both layouts and compares each partial repaint with a full redraw at the same timestamp. When changing row geometry, pass the whole painted row to `track_strength()` so the last frame of the fade clears every highlighted pixel.

To check the VP8 receive path, run the single-packet cases. They decode complete EVS3 and RFC 7741 frames and exercise declared-length validation.

```sh
make build/test-call-video
./build/test-call-video -r /video/single-packet
```

### Translating interface text

Use English literals as lookup keys, for example `_("Send")`. Mark labels in static tables with `N_()` and translate them with `_()` when displayed. Add the matching English and translated pair to each `src/ui_lang_<tag>.c` table. Keep the order and types of format arguments such as `%s` and `%d`. Use `ui_lang_date()` for month and weekday names in the selected language. [Language API](../src/ui_lang.h) · [Translation table format](../src/ui_lang_table.h)

```sh
make build/test-ui-lang
./build/test-ui-lang
```

The language tests check UTF-8 text, duplicate keys and format arguments. They also exercise saved preferences, locale fallback and date formatting in isolated configuration directories.

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
| `AVERNELLA_MAX_FPS=30` | Cap window presentation at 30 frames per second on either renderer. The default follows the display refresh rate. |
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
