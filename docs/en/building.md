# Building, running and debugging

[Index](README.md) · Previous: [Overview](overview.md) · Next: [Architecture](architecture.md)

## Requirements

The build needs **GNU GCC 12 or newer**, GNU Make, GNU binutils with the BFD linker (`ld.bfd`) and `pkg-config`. The Makefile checks that `$(CC)` really is GCC (it runs `$(CC) -v` and looks for `gcc version`) and stops otherwise. Clang isn't supported for building, although `clang-format` is used for formatting.

### Libraries

These are the `pkg-config` packages in `PKGS` at the top of the [Makefile](../../Makefile). All are required:

| Package | Used for |
| ------- | -------- |
| `sdl2` (sdl2-compat on SDL3 is preferred) | Window, input, audio, clipboard |
| `wayland-client` and `wayland-protocols` plus `wayland-scanner` | Portal window parenting and tray activation |
| `pangocairo` | Drawing (Cairo) and text layout (Pango), including Thai and emoji |
| `glib-2.0`, `gio-2.0`, `gio-unix-2.0` | Containers, threads, D-Bus (tray, notifications, portals) |
| `json-c` | All structured data, including the Thrift bridge |
| `libcurl` | HTTPS with HTTP/2 |
| `libcrypto` (OpenSSL 3) | X25519, P-256, AES, HKDF, RSA, random numbers |
| `libqrencode` | Drawing the sign-in and invite QR codes |
| `libturbojpeg`, `libpng`, `libwebp` | Picture decoding and the profile JPEG encoder |
| `zlib` | Call feature-share payloads, PNG |
| `opus` | Call audio |
| `vpx` | VP8 call video |
| `libsrtp2` | Encrypted call media |
| `libpipewire-0.3` | Screen sharing |
| `rnnoise` | Call noise suppression |

Optional pieces, each detected automatically:

| Piece | Packages | Effect when missing |
| ----- | -------- | ------------------- |
| WPE WebKit browser | `wpe-webkit-2.0 wpe-platform-wayland-2.0` | No `avernella-liff` |
| WebKitGTK browser | `webkitgtk-6.0` | No `avernella-liff-gtk`. With neither browser, links open in the system browser. |
| Desktop keyring | `libsecret-1` (Ubuntu `libsecret-1-dev`, Fedora `libsecret-devel`, Arch `libsecret`) | Desktop keyring protection for saved sessions is unavailable. |
| Native Vulkan renderer | `glslangValidator`, `spirv-val`, Vulkan headers that declare `VK_EXT_descriptor_heap`, and volk (`/usr/include/volk.c` or `-lvolk`) | No `avernella-vulkan.so`. The app draws through SDL instead. |
| ZBar | `libzbar.so.0` at run time, loaded by `dlopen` in [`qr_scan.c`](../../src/qr_scan.c) | **Read QR image** is unavailable. |
| Hardware VP8 calls | FFmpeg development headers for `libavcodec` and `libavutil` (Ubuntu packages `libavcodec-dev libavutil-dev`, Arch package `ffmpeg`) | libvpx handles calls in software. Set `CALL_HWCODECS=0` to disable the FFmpeg call backends. The older `CALL_HWDECODE=0` switch also works. |
| Jetson VP8 encoder | On AArch64 the build detects `gstreamer-app-1.0` and `gstreamer-video-1.0`. Jetson Nano and TX2 use the JetPack `nvvidconv` and `nvv4l2vp8enc` plugins. | The other encoders remain available. Set `CALL_VP8_JETSON=0` to omit the Jetson backend. |
| FFmpeg | `ffmpeg`, `ffplay`, `ffprobe` executables | No camera video messages or video playback, and the recording tests fail. |
| XDG desktop portal | `xdg-desktop-portal` with a backend for your desktop | The attachment chooser, screenshots and screen sharing are unavailable. |

On Arch Linux or CachyOS, the README lists the exact `pacman` commands. In short:

```sh
sudo pacman -S --needed gcc binutils make pkgconf sdl2-compat wayland wayland-protocols \
  cairo pango glib2 qrencode zbar json-c curl openssl \
  libjpeg-turbo libpng libwebp opus libsrtp libvpx pipewire rnnoise
sudo pacman -S --needed vulkan-headers volk glslang spirv-tools vulkan-icd-loader   # Vulkan module
sudo pacman -S --needed wpewebkit webkitgtk-6.0                                     # in-app browser
sudo pacman -S --needed libsecret                                                   # desktop keyring
sudo pacman -S --needed xdg-desktop-portal xdg-desktop-portal-xapp ffmpeg           # Cinnamon Wayland, video
sudo pacman -S --needed clang linux-headers xorg-server-xvfb                        # development tools
```

Use the portal backend for the active desktop. GNOME uses `xdg-desktop-portal-gnome`, Plasma uses `xdg-desktop-portal-kde`, and Hyprland uses `xdg-desktop-portal-hyprland`. With Cinnamon Wayland and XApp, Avernella can ask the user to choose a monitor and share it through Muffin.

Native keyring-backed session storage uses an unlocked Secret Service provider, such as GNOME Keyring or KWallet with Secret Service enabled. Flatpak uses the desktop's per-app Secret portal.

On other distributions, install the development packages with the same `pkg-config` names.

## Building

From the repository root:

```sh
make            # avernella, plus avernella-liff(-gtk) and avernella-vulkan.so when possible
make check      # build and run every test in tests/
make bench      # the render benchmark
```

Objects go to `build/`. The final programs are moved to the repository root. Header dependencies are tracked automatically (`-MMD -MP`), so editing a header rebuilds what includes it.

GNU Make 4.3 and newer runs one job per CPU (`nproc`) by default, because the Makefile adds `-j$(nproc)` when you didn't pass a job count. Use `make -j1` for a serial build, for example to read error messages in order.

### Make targets

| Target | What it does |
| ------ | ------------ |
| `all` (default) | Builds `avernella` and every optional program whose SDK is installed |
| `check` | Builds every test in `TESTS` and runs them one after another, stopping at the first failure |
| `bench` | Builds and runs `build/render-bench` |
| `fuzz` | Builds the sanitizer fuzzers and their seed inputs in `build-fuzz/`. See [Fuzzing](testing.md#fuzzing). |
| `fuzz-run`, `fuzz-run-NAME` | Runs every fuzzer, or one, for `FUZZ_TIME` seconds (600 by default) |
| `fuzz-check` | Replays each fuzzer's seeds and saved corpus once |
| `vulkan-smoke` | Offscreen Vulkan renderer test. `VK_VENDOR=<PCI vendor ID>`, such as `0x10de` (NVIDIA) or `0x1002` (AMD), picks a GPU. |
| `vulkan-preview` | Vulkan presenter test in a real window. Pass `VK_PREVIEW_ARGS=--test` for the automatic version. |
| `format` | Rewrites all sources in place with `clang-format` |
| `format-check` | Fails if any source isn't formatted |
| `checkpatch` | Runs the Linux kernel's `checkpatch.pl` on every source, one file per run |
| `analyze` | Runs GCC's static analyzer (`-fanalyzer`) on every program source, in parallel. It only reports, and takes about 15 seconds |
| `install`, `uninstall` | See [Installing](#installing) |
| `clean` | Removes build products. It keeps the fuzzing corpus; `fuzz-clean` removes all of `build-fuzz/`. |

### Build options

Pass them on the command line, as in `make DEBUG=1`.

| Option | Effect |
| ------ | ------ |
| `DEBUG=1` | Unstripped `-Og -g3` build with frame pointers and unwind tables, for `gdb`. Run `make clean` when you switch between debug and release builds, because objects aren't rebuilt for a flag change. |
| `LIFF_BROWSER=0` | Leaves out both browser helpers |
| `KEYRING=1` / `0` | Requires desktop keyring support or disables it. The default `auto` enables it when `libsecret-1` is available. |
| `NATIVE_VULKAN=1` / `0` | Require the Vulkan module, or never build it. The default `auto` builds it when the tools are found. |
| `VOLK_DIR=…`, `VK_HEADERS=…` | Build volk from a checkout, or use a specific Vulkan-Headers include directory |
| `GLSLANG=…`, `SPIRV_VAL=…` | Shader compiler and validator to use |
| `CC=…` | Another GCC driver, such as `gcc-14` or a wrapper. Non-GCC compilers are refused. |
| `CFLAGS=…` | Replaces the optimization flags. The warning and hardening flags are always added. |
| `HARDEN=…` | Replaces the memory-hardening compiler flags described below; `HARDEN=` builds without them. Objects aren't rebuilt for a flag change, so run `make clean` first. |
| `PREFIX=…`, `DESTDIR=…` | Install location, see below |
| `CHECKPATCH=…`, `CLANG_FORMAT=…` | Paths to the style tools |

### What the release build does

The default flags are `-Oz -flto -fno-plt -fvisibility=hidden` without unwind tables, plus `-std=c17 -Wall -Wextra -Wpedantic -Wformat=2 -Werror=format-security -fstack-protector-strong -ffunction-sections -fdata-sections -fPIE`. When the compiler supports them, the Makefile also adds `-fstack-clash-protection` and, for optimizing builds without AddressSanitizer, `-D_FORTIFY_SOURCE=3` (replacing any lower level from the environment). Linking uses `-fuse-ld=bfd -pie`, `--gc-sections`, `--as-needed`, full RELRO, `-z separate-code`, `-z noexecstack`, and, when the toolchain supports them, `--rosegment` and `-z pack-relative-relocs` (glibc 2.36 or newer). The result is stripped.

The Makefile also probes for these memory-hardening flags (`HARDEN`) and adds the ones the compiler accepts, to the program, the Vulkan module, volk and the browser helpers:

- `-fcf-protection=full` on x86-64: shadow-stack and IBT marking, which lets the worker run with a shadow stack ([memory hardening](architecture.md#memory-hardening)). On arm64, `-mbranch-protection=standard` (return-address signing and BTI), which older cores execute as no-ops.
- `-ftrivial-auto-var-init=zero` (GCC 12): stack variables that a path forgets to set start as zero instead of stale data.
- `-fstrict-flex-arrays=3` (GCC 13): only `[]` counts as a flexible array, so `_FORTIFY_SOURCE` and `-Warray-bounds` know the size of every fixed array at the end of a structure.
- `-fsanitize=bounds-strict -fsanitize-trap=bounds-strict`: every index into an array of known size is checked, and an index out of range stops the process with `SIGILL`. The trap needs no UBSan runtime library. It is left out when `CFLAGS` or `CPPFLAGS` already ask for a sanitizer, so that sanitizer keeps its reports.

A few files are built for speed instead of size: `ui_pixels.c`, `camera_pixels.c` and `screencast.c` get `-O3` so GCC can vectorize their per-pixel loops. `screencast.c` also gets `-Wno-pedantic`, because PipeWire's headers use GNU statement expressions.

### Generated files

| File | Made from | Used by |
| ---- | --------- | ------- |
| `build/line_build_commit.h` | `git rev-parse HEAD` (first 7 characters), or `unknown commit` | The window title and **User Settings** (`ui_build_commit()`). It is rewritten only when the commit changes. |
| `build/avernella_mark.inc`, `build/avernella_welcome.inc` | `data/avernella-mark.png`, `data/avernella-welcome.png`, dumped as bytes with `od` | [`ui_logo.c`](../../src/ui_logo.c): the window icon, tray icon and sign-in portrait |
| `build/ui_uber.vert.spv`, `build/ui_uber.frag.spv`, `build/line_ui_spirv.h` | `src/render/ui_uber.vert` and `.frag` via `glslangValidator`, checked by `spirv-val` | The Vulkan module's baked shaders |

`src/emoji_pack_index.inc`, the offline index of LINE's 45 public emoji packs, is checked in. `src/emoji_defaults.inc` is ignored by git: it is a private file that only builds with `LINE_PRIVATE_EMOJI_LABELS` defined, and normal builds don't use it.

## Running

```sh
./avernella
```

There are no command-line options except `--help`. (`--worker` and `--worker-password` are internal: the window uses them to start its network worker. Don't run them by hand.)

The first launch shows the sign-in screen. Choose **Sign in with QR code** and scan it with LINE on your phone, or **Sign in with email**. After sign-in the session is saved, and later launches resume it automatically. The sign-in flows are described in [Signing in](sign-in.md).

Only one Avernella can use a session at a time: the session directory is locked with `flock`. A second window shows "Another Avernella window is using this session."

### Wayland and X11

In a Wayland session the client asks SDL for `wayland,x11`, so it uses native Wayland and falls back to X11. Set `SDL_VIDEODRIVER=x11` to force X11. On a laptop with two GPUs, the native Vulkan module prefers a compatible NVIDIA discrete GPU, then other discrete GPUs, then integrated graphics. If a device fails to open it tries the next candidate. It still checks that the GPU can present to the window. To use the integrated GPU instead, for longer battery life, set `AVERNELLA_RENDER_NODE` to its render node. If the NVIDIA driver is loaded but no NVIDIA GPU is bound to it with a display device, as after a laptop GPU drops off the PCI bus, NVIDIA's Vulkan driver would block forever when it opens the device. In that case Avernella sets `VK_LOADER_DRIVERS_DISABLE=*nvidia*` before Vulkan starts and draws on another GPU. Video messages then skip NVENC, and a line on standard error says why. Your own `VK_LOADER_DRIVERS_DISABLE`, `VK_LOADER_DRIVERS_SELECT`, `VK_DRIVER_FILES` or `VK_ICD_FILENAMES` setting is kept ([`gpu_health.c`](../../src/gpu_health.c)). `LINE_RENDER_LOG=1` prints the chosen device. With the SDL renderer, `SDL_VIDEODRIVER=x11 prime-run ./avernella` selects the NVIDIA GPU through X11 or XWayland.

## Installing

Build as yourself first, then install as root:

```sh
make
sudo make install            # into /usr/local
sudo make uninstall
make install PREFIX=$HOME/.local     # for one user, without root
```

`make install` copies:

| From | To |
| ---- | -- |
| `avernella` | `$(PREFIX)/bin/avernella` |
| `avernella-vulkan.so`, `avernella-liff`, `avernella-liff-gtk` (whichever exist) | `$(PREFIX)/lib/avernella/` |
| `data/avernella.desktop` | `$(PREFIX)/share/applications/` |
| `data/avernella.png`, `data/avernella-mark.png` | the hicolor icon theme at 256 px and 64 px |

The client finds its helpers at run time, beside its own executable or in `../lib/avernella` relative to it (by reading `/proc/self/exe`), so any `PREFIX` works without rebuilding. `DESTDIR` is honored for packaging. The window's app ID is `avernella`, which desktops match to the `.desktop` file.

Arch Linux and CachyOS users can install the prebuilt [avernella-bin](https://aur.archlinux.org/packages/avernella-bin) package from the AUR.

## Environment variables

None are needed for normal use. They exist for debugging, tuning and testing.

### Debug output

| Variable | Effect | Read in |
| -------- | ------ | ------- |
| `LINE_DEBUG_TIMING=1` | Prints `line-timing <method> <ms>` for every worker request | `handle()` in `main.c` |
| `LINE_DEBUG_SYNC=1` | Prints each long-poll round: result, duration, operation count | `live_sync.c` |
| `LINE_DEBUG_SESSION=1` | Reports token renewals and protocol replies it can't decode | `maintain_tokens()` in `main.c`, `line_decode_reply()` in `thrift.c` |
| `LINE_DEBUG_MEDIA=1` | Reports the failure stage, transfer status and byte count for Flex and rich-message pictures | `flex_asset.c`, `ui_media.c` |
| `LINE_DEBUG_LIFF=1` | Reports WebKit load stages and the names of supported bridge operations. Other operations appear as `unsupported`. Also lets the browser load `http://127.0.0.1:<port>/` pages for local testing. | `liff_browser.c` |
| `LINE_CALL_DEBUG=1` | Reports call negotiation, control messages, packet counts every five seconds and first-frame milestones | `call.c`, `camera.c`, `ui_media.c` |
| `LINE_CALL_PDTP_DUMP=<dir>` | With `LINE_CALL_DEBUG=1`, stores raw LINE and PLANET stream data in an existing owner-only directory (mode 0700) | `call.c` |
| `LINE_RENDER_LOG=1` | Says which renderer was chosen and why fallbacks happened | `ui_gpu.c`, `ui_draw.c` |
| `LINE_DEBUG_SANDBOX=1` | Prints which sandbox and memory-hardening layers are active, once for the UI and once for the worker: `line-sandbox worker no-dump=1 no-new-privs=1 seccomp=1 landlock-net=1 landlock-signal=1 mdwe=1 seal=1(513) shadow-stack=1 memory-tagging=0 heap-wipe=1 guard-pages=1` | `sandbox.c` |

### Behavior

| Variable | Effect |
| -------- | ------ |
| `LINE_EXPERIMENTAL_VIDEO=0` | Turns off video calls (voice still works) |
| `LINE_DISABLE_SANDBOX=1` | Starts the worker without its sandbox and memory hardening, and the UI without its own, for example to attach `gdb` or `strace` to the worker. A comma-separated list leaves out only the named layers: `seccomp`, `landlock`, `mdwe`, `seal`, `shstk`, `mte`, `wipe`, `guard` |
| `LINE_DISABLE_UDP_BATCH=1` | Sends and receives call media one datagram per system call instead of with `sendmmsg`, `recvmmsg` and `UDP_SEGMENT` |
| `LINE_NOISE_SUPPRESSION=0` | Sends the raw microphone, without RNNoise |
| `LINE_CALL_FEEDBACK=0` | Stops sending video retransmission requests, receiver reports and REMB in one-to-one calls, for troubleshooting ([Bad networks](calls.md#8-bad-networks)) |
| `LINE_CALL_DSCP=0` | Sends call packets without the DSCP marks (EF for voice, AF41 for video) |
| `LINE_CALL_ENCODER_THREAD=0` | Encodes call video on the call thread instead of its own `line-encode` thread |
| `LINE_DISABLE_RX_TIMESTAMPS=1` | Times call packets when the call thread reads them, instead of with the kernel's receive stamps |
| `LINE_DISABLE_SEND_QUEUE=1` | Stops reading the call socket's kernel send queue (`SIOCOUTQ`) for rate control |
| `LINE_CALL_VIDEO_KBPS=<n>` | Overrides the video sending bitrate. Direct calls send at least 1500 kbit/s HD unless this is set lower |
| `LINE_CAMERA_DEVICE=<path>` | Camera device (default `/dev/video0`) |
| `LINE_SHARE_WIDTH`, `LINE_SHARE_HEIGHT`, `LINE_SHARE_FPS`, `LINE_SHARE_KBPS` | Caps on screen-share capture and encoding |
| `LINE_SHARE_TRANSPORT=presentation` | Sends a screen share as a native PDTP presentation instead of in the video stream (experimental) |
| `LINE_DISABLE_TRAY=1` | Closing the window quits instead of hiding to the tray |
| `LINE_REDUCE_MOTION=1` | Turns off animation (overrides the desktop setting) |
| `AVERNELLA_MAX_FPS=<n>` | Caps redraws, for example `30` on older machines |

### Renderer selection

| Variable | Effect |
| -------- | ------ |
| `LINE_DISABLE_GPU=1` | Uses the SDL window surface, libvpx for incoming and outgoing call video, and software video-message encoding |
| `AVERNELLA_VULKAN=sdl` | Skips the native module but still tries SDL's Vulkan renderer |
| `AVERNELLA_VK_DESCRIPTORS=buffer` | Makes the native module use descriptor buffers instead of descriptor heaps |
| `AVERNELLA_RENDER_NODE=/dev/dri/renderD128` | Picks the GPU by DRM render node, on drivers that expose DRM properties |
| `AVERNELLA_PRESENT_MODE=fifo` | Makes the native module present with plain FIFO on NVIDIA too. `latest` uses FIFO latest ready on any driver that offers it |
| `SDL_VIDEODRIVER=x11` | Forces X11 |

### Benchmarks

| Variable | Effect |
| -------- | ------ |
| `LINE_BENCH_VIDEODRIVER=x11` | Runs `render-bench` on X11 instead of SDL's dummy driver |
| `LINE_BENCH_DAMAGE=1` | Also compares the last partial frame against a full redraw |

## Debugging

- **A request fails.** The error text from the worker appears in the status line at the top of the chat. Run with `LINE_DEBUG_TIMING=1` to see which request failed or was slow. A message such as "LINE declined sendMessage (service code 10)" carries LINE's own error number. [Protocol basics](protocol-basics.md#errors) explains the common codes.
- **Messages arrive late.** Run with `LINE_DEBUG_SYNC=1`. Each long-poll round should take about 50 seconds when idle and return immediately when something happens.
- **A card picture stays blank.** Run with `LINE_DEBUG_MEDIA=1`.
- **The UI or the worker dies with `SIGILL` or `SIGSEGV` and nothing else.** `SIGILL` in Avernella's own code is the bounds check: an array index was out of range. `SIGSEGV` with `si_code` `SEGV_CPERR` (shown by `gdb` and `coredumpctl info`, though the worker writes no core dump) is the shadow stack catching an overwritten return address. A fault right after a LINE reply's last byte is the guard page. All three are bugs to report. `LINE_DISABLE_SANDBOX=shstk,guard` and a `HARDEN=` build take the checks away to compare, never to work around a crash.
- **A LINE app never finishes loading.** Run with `LINE_DEBUG_LIFF=1` and open it again. Check whether the load reached `committed` and `finished`, and whether the bridge reported an unsupported operation.
- **A call has no video.** Run with `LINE_CALL_DEBUG=1`.
- **Stepping through code.** Build with `make clean && make DEBUG=1`. The window is the `avernella` process. The worker is a child process named `avernella --worker`; attach to it separately with `gdb -p $(pgrep -f 'avernella --worker')`. Because the worker exits when its parent dies (`PR_SET_PDEATHSIG`), killing the window also ends the worker.
- **Testing `avernella-liff` alone.** Start it with a `SOCK_SEQPACKET` socket on file descriptor 3 and pass the parent PID as its first argument. Omitting the optional account digest selects an ephemeral profile. Send it `L\tfull\t<url>` datagrams. With `LINE_DEBUG_LIFF=1` it also loads pages from `http://127.0.0.1:<port>/`, so a local page that loads the real LIFF SDK can exercise the bridge.
- **The session seems broken.** Sign out from the app. If you can't, close the app and remove `~/.local/state/avernella/session.json`. Then remove the linked device in LINE on your phone.

## Where your data is

| Path | Contents |
| ---- | -------- |
| `$XDG_STATE_HOME/avernella/` (default `~/.local/state/avernella/`) | `session.json` (tokens, MID, Letter Sealing keys, request sequence) and `lists-<sha256 of your MID>.json` (folders, the selected folder view, pins, mutes, shortcuts, the notification and sound switches, reduced motion) |
| `$XDG_CONFIG_HOME/avernella/` (default `~/.config/avernella/`) | `layout` (the theme choice) and the interface-language choice |
| `$XDG_RUNTIME_DIR/avernella/` | Pictures pasted for sending, deleted after use |
| `~/.cache/avernella/liff-profiles/<account-hash>`, `~/.local/share/avernella/liff-profiles/<account-hash>` | The in-app browser's HTTP cache and cookies |

The details of each file are in [Architecture](architecture.md#files-on-disk).
