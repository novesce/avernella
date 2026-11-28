# SPDX-License-Identifier: GPL-3.0-or-later
# Keep the parent's jobserver and serialize goals that remove or rewrite files.
ifeq ($(MAKELEVEL),0)
ifeq ($(filter clean format,$(MAKECMDGOALS)),)
# GNU Make 4.3 hides job options in the makefile's MAKEFLAGS during parsing.
JOB_FLAGS := $(MAKEFLAGS) $(shell printf '%s %s' "$$MAKEFLAGS" "$$GNUMAKEFLAGS")
ifeq ($(filter -j% --jobs --jobs=% --jobserver-auth=%,$(JOB_FLAGS)),)
DEFAULT_JOBS := $(shell nproc 2>/dev/null)
MAKEFLAGS += $(addprefix -j,$(DEFAULT_JOBS))
endif
endif
endif

ifeq ($(origin CC),default)
CC = gcc
endif
PKGS = sdl2 pangocairo glib-2.0 libqrencode json-c libcurl libcrypto libturbojpeg libpng libwebp opus libsrtp2 gio-2.0 gio-unix-2.0 wayland-client zlib vpx libpipewire-0.3 rnnoise libseccomp
KEYRING ?= auto
ifeq ($(KEYRING),auto)
HAVE_KEYRING := $(shell pkg-config --exists libsecret-1 && echo 1)
else
HAVE_KEYRING := $(KEYRING)
endif
ifeq ($(HAVE_KEYRING),1)
KEYRING_OBJECT = build/state_keyring.o
build/state_keyring.o: override CPPFLAGS += $(shell pkg-config --cflags libsecret-1)
avernella: LDLIBS += $(shell pkg-config --libs libsecret-1)
endif
KEYRING_DEFINE := \#define LINE_HAVE_LIBSECRET $(if $(KEYRING_OBJECT),1,0)
CPPFLAGS += -D_GNU_SOURCE -Isrc $(shell pkg-config --cflags $(PKGS))
ifeq ($(DEBUG),1)
CFLAGS ?= -Og -g3 -fno-omit-frame-pointer -fasynchronous-unwind-tables
else
CFLAGS ?= -Oz -flto -fno-plt -fvisibility=hidden \
	-fno-unwind-tables -fno-asynchronous-unwind-tables
STRIP_RELEASE = -s
endif
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Wformat=2 -Werror=format-security -fstack-protector-strong -ffunction-sections -fdata-sections -fPIE
LDLIBS += $(shell pkg-config --libs $(PKGS)) -pthread -ldl

# Check the driver itself so GCC aliases and wrappers also work.
ifneq ($(filter-out clean fuzz-clean format format-check checkpatch install uninstall,$(or $(MAKECMDGOALS),all)),)
ifeq ($(shell LC_ALL=C $(CC) -v 2>&1 | awk '/^gcc version [0-9]/ { print "yes"; exit }'),)
$(error GNU GCC is required to build Avernella. Set CC=gcc or choose another GCC driver)
endif

# Probe the target compiler and linker. Packed relative relocations require
# glibc 2.36, even if an older target's linker recognizes the option.
RO_SEGMENT := $(shell printf 'int main(void) { return 0; }\n' | \
	$(CC) $(LDFLAGS) -x c - -fuse-ld=bfd -pie \
	-Wl,--fatal-warnings,-z,separate-code,--rosegment -o /dev/null \
	>/dev/null 2>&1 && printf '%s' '-Wl,--rosegment')
PACKED_RELOCS := $(shell printf '\043include <features.h>\n\043if __GLIBC_PREREQ(2,36)\nint main(void) { return 0; }\n\043endif\n' | \
	$(CC) $(CPPFLAGS) $(LDFLAGS) -x c - -fuse-ld=bfd -pie \
	-Wl,--fatal-warnings,-z,pack-relative-relocs -o /dev/null \
	>/dev/null 2>&1 && printf '%s' '-Wl,-z,pack-relative-relocs')
# _FORTIFY_SOURCE=3 needs an optimizing build and is left out under
# AddressSanitizer, which checks the same calls itself. It replaces any
# lower level a distribution's flags set.
FORTIFY := $(if $(findstring -fsanitize=address,$(CFLAGS) $(CPPFLAGS)),,$(shell \
	printf '\043ifndef __OPTIMIZE__\n\043error no optimization\n\043endif\n' | \
	$(CC) $(CFLAGS) -E -x c - >/dev/null 2>&1 && \
	printf '%s' '-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3'))
# Stack clash probes exist on x86-64, AArch64 and most other Linux targets.
STACK_CLASH := $(shell printf 'int main(void) { return 0; }\n' | \
	$(CC) -Werror -fstack-clash-protection -x c - -o /dev/null \
	>/dev/null 2>&1 && printf '%s' '-fstack-clash-protection')
# Control-flow protection: x86 CET marks every object for shadow stacks
# (the worker runs with one, see sandbox.c) and adds IBT landing pads;
# arm64 gets return-address signing (PAC) and BTI landing pads, which
# older cores execute as no-ops. -ftrivial-auto-var-init=zero (GCC 12)
# zeroes stack variables a path forgot to set, so they never carry stale
# data; -fstrict-flex-arrays=3 (GCC 13) lets _FORTIFY_SOURCE and
# -Warray-bounds treat only [] as unbounded. The bounds sanitizer checks
# every index into an array of known size and stops the process with an
# illegal instruction, without UBSan's runtime library; it is left out
# when CFLAGS already ask for a sanitizer, to keep its reports.
# HARDEN= turns all of these off.
HARDEN_PROBE = $(shell printf 'int main(void) { return 0; }\n' | \
	$(CC) -Werror $(1) -x c - -o /dev/null >/dev/null 2>&1 && printf '%s' '$(1)')
ifeq ($(origin HARDEN),undefined)
HARDEN := $(or $(call HARDEN_PROBE,-fcf-protection=full),$(call HARDEN_PROBE,-mbranch-protection=standard)) \
	$(call HARDEN_PROBE,-ftrivial-auto-var-init=zero) \
	$(call HARDEN_PROBE,-fstrict-flex-arrays=3) \
	$(if $(findstring -fsanitize=,$(CFLAGS) $(CPPFLAGS)),,$(call HARDEN_PROBE,-fsanitize=bounds-strict -fsanitize-trap=bounds-strict))
endif
CFLAGS += $(FORTIFY) $(STACK_CLASH) $(HARDEN)
endif
LINK_FLAGS = -fuse-ld=bfd -pie -Wl,--gc-sections,--as-needed,-z,relro,-z,now,-z,separate-code,-z,noexecstack,-O1
SOURCES = main pair password_login state thrift transport messaging friends groups square square_search sealing media media_crypto obs_http media_io call call_schema call_wire call_audio ui ui_draw ui_media ui_image_view video_record ui_motion ui_pixels emoji ui_emoji ui_apng ui_friends ui_groups ui_openchat
SOURCES += ui_find stickers camera camera_pixels call_video call_srtp call_fec call_pdtp screencast screencast_dma live_sync net_route tune http_tune
SOURCES += call_group call_vp8 call_vp8_device call_vp8_encode udp_batch sandbox gpu_health
SOURCES += security/ipc security/sandbox security/vault state_crypto
SOURCES += screencast_cinnamon monitor_picker ui_monitor_picker
# Older NVIDIA Tegra boards expose VP8 through their JetPack GStreamer plugin.
JETSON_PKGS = gstreamer-app-1.0 gstreamer-video-1.0
ifeq ($(origin CALL_VP8_JETSON),undefined)
CALL_VP8_JETSON := $(if $(filter aarch64%,$(shell $(CC) -dumpmachine)),$(shell pkg-config --exists $(JETSON_PKGS) && echo 1 || echo 0),0)
endif
ifeq ($(CALL_VP8_JETSON),1)
SOURCES += call_vp8_jetson
endif
SOURCES += chat_lists shared_content ui_chat_lists ui_chat_tools ui_shared_content ui_notifications screenshot
SOURCES += ui_instance ui_tray ui_window file_chooser ui_logo video_play talk_sync flex_asset ui_flex qr_scan profile_image ui_profile rich_message clipboard_paste
SOURCES += liff liff_url ui_liff chiptune ui_sounds call_denoise ui_gpu_scroll ui_gpu ui_theme ui_lang
# Interface translations, one table per language (see src/ui_lang_table.h).
UI_LANGS = ui_lang_ja ui_lang_ko ui_lang_th ui_lang_zh_hans ui_lang_zh_hant ui_lang_vi ui_lang_id ui_lang_ms ui_lang_fr ui_lang_de ui_lang_it ui_lang_es ui_lang_pt ui_lang_ru ui_lang_ar ui_lang_tr
SOURCES += $(UI_LANGS)
UI_LANG_OBJECTS = build/ui_lang.o $(UI_LANGS:%=build/%.o)
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_NAMES = xdg-activation-v1 xdg-foreign-unstable-v1 xdg-foreign-unstable-v2
WAYLAND_OBJECTS = $(WAYLAND_NAMES:%=build/%-code.o)
UI_WINDOW_OBJECTS = build/ui_window.o $(WAYLAND_OBJECTS)
OBJECTS = $(SOURCES:%=build/%.o) $(WAYLAND_OBJECTS)
GIT_COMMIT := $(if $(wildcard .git),$(shell git rev-parse HEAD 2>/dev/null | cut -c1-7))
COMMIT_DEFINE := \#define LINE_BUILD_COMMIT "$(or $(GIT_COMMIT),unknown commit)"
CLANG_FORMAT ?= clang-format
STYLE_SOURCES := $(sort $(wildcard src/*.c src/*.h src/security/*.c src/security/*.h src/render/*.c src/render/*.h tests/*.c tests/*.h fuzz/*.c fuzz/*.h))
# The kernel's own style checker, from linux-headers or a kernel tree.
CHECKPATCH ?= $(firstword $(shell command -v checkpatch.pl 2>/dev/null) \
	$(wildcard /usr/lib/modules/$(shell uname -r)/build/scripts/checkpatch.pl \
		   /usr/src/linux/scripts/checkpatch.pl))
# Kernel-only rules that do not apply to a GLib userspace program: GLib's
# g_autoptr needs typedefs, environ is declared extern, protocol words in
# messages are mistaken for function names, `Type type` declarations look
# like repeated words, and the curl option macros jump to their cleanup.
# ENOSYS: user space reads it as "no such system call" from an old kernel or
# a seccomp filter (udp_batch.c, sandbox.c), which is its only meaning here.
CHECKPATCH_IGNORE = NEW_TYPEDEFS,AVOID_EXTERNS,EMBEDDED_FUNCTION_NAME,FILE_PATH_CHANGES,REPEATED_WORD,MACRO_WITH_FLOW_CONTROL,ENOSYS
CHECKPATCH_FLAGS = --no-tree --file --terse --quiet --show-types --typedefsfile .checkpatch-typedefs --ignore $(CHECKPATCH_IGNORE)
# Test fixtures keep long JSON and protocol strings split across lines.
CHECKPATCH_TEST_FLAGS = $(CHECKPATCH_FLAGS) --ignore SPLIT_STRING,MISSING_SPACE
TESTS = build/test-thrift build/test-state build/test-sealing build/test-messaging build/test-pair build/test-transport build/test-media build/test-call build/test-audio build/test-media-io build/test-obs build/test-video-record build/test-ui-motion build/test-ui-pixels build/test-emoji-message build/test-ui-apng build/test-emoji build/test-ui-emoji
TESTS += build/test-ui-image-view build/test-ui-image-overlay build/test-state-crypto
TESTS += build/test-friends build/test-groups build/test-square build/test-ui-friends build/test-ui-read
TESTS += build/test-stickers build/test-call-video
TESTS += build/test-call-group build/test-pdtp
TESTS += build/test-chat-lists build/test-shared-content build/test-ui-tray build/test-ui-instance
TESTS += build/test-ui-notifications build/test-file-chooser
TESTS += build/test-video-play build/test-talk-sync build/test-live-sync
TESTS += build/test-flex-asset build/test-ui-flex
TESTS += build/test-ui-refresh build/test-ui-chat-refresh
TESTS += build/test-ui-worker-stop
TESTS += build/test-screencast-portal
TESTS += build/test-monitor-picker build/test-screencast-cinnamon
TESTS += build/test-qr-scan
TESTS += build/test-password-login
TESTS += build/test-profile-image build/test-rich-message build/test-clipboard-paste
TESTS += build/test-ui-liff
TESTS += build/test-liff build/test-chiptune build/test-denoise build/test-ui-scroll-scene build/test-ui-call-fullscreen
TESTS += build/test-ui-theme build/test-ui-lang build/test-ui-sounds
TESTS += build/test-udp-batch build/test-sandbox build/test-security-sandbox build/test-gpu-health
TESTS += build/test-call-net build/test-call-fec

# The LIFF helpers share the same browser code and IPC.
# LIFF_BROWSER=0 leaves both out.
LIFF_WPE_PKGS = wpe-webkit-2.0 wpe-platform-wayland-2.0 gio-unix-2.0 libseccomp
LIFF_GTK_PKGS = webkitgtk-6.0 gio-unix-2.0 libseccomp
LIFF_BROWSER ?= 1
ifneq ($(filter-out clean fuzz-clean format format-check checkpatch install uninstall,$(or $(MAKECMDGOALS),all)),)
ifeq ($(LIFF_BROWSER),1)
LIFF_WPE ?= $(shell pkg-config --exists $(LIFF_WPE_PKGS) && echo 1)
LIFF_GTK ?= $(shell pkg-config --exists $(LIFF_GTK_PKGS) && echo 1)
endif
endif

.PHONY: all check bench clean format format-check checkpatch analyze install uninstall
all: avernella
ifeq ($(LIFF_WPE),1)
all: avernella-liff
endif
ifeq ($(LIFF_GTK),1)
all: avernella-liff-gtk
endif

format:
	$(CLANG_FORMAT) -i $(STYLE_SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(STYLE_SOURCES)

checkpatch:
	@if [ -z "$(CHECKPATCH)" ]; then \
		echo "checkpatch.pl not found. Set CHECKPATCH to its path in a Linux kernel tree." >&2; \
		exit 1; \
	fi
	@# One file per run: checkpatch carries parser state between files.
	@status=0; \
	for f in $(filter src/%,$(STYLE_SOURCES)); do \
		perl "$(CHECKPATCH)" $(CHECKPATCH_FLAGS) "$$f" || status=1; \
	done; \
	for f in $(filter tests/% fuzz/%,$(STYLE_SOURCES)); do \
		perl "$(CHECKPATCH)" $(CHECKPATCH_TEST_FLAGS) "$$f" || status=1; \
	done; \
	rm -f .checkpatch-camelcase.git.; exit $$status

# GCC's static analyzer follows each path through a function and reports
# leaks, double frees, uses after free, NULL dereferences and out-of-bounds
# accesses. It is slow and only reports, so it is not part of `make check`.
ANALYZE_SOURCES ?= $(SOURCES:%=src/%.c)
analyze: avernella
	@printf '%s\n' $(ANALYZE_SOURCES) | xargs -P "$$(nproc)" -I{} \
		$(CC) $(CPPFLAGS) -Ibuild -std=c17 -O1 -fanalyzer \
		-Wno-analyzer-too-complex -c {} -o /dev/null

avernella: $(OBJECTS) $(KEYRING_OBJECT)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LINK_FLAGS) $(RO_SEGMENT) $(PACKED_RELOCS) $(STRIP_RELEASE) -o build/$@ $^ $(LDLIBS)
	mv -f build/$@ $@

LIFF_OBJECTS = build/liff_url.o build/security/liff_filter.o build/security/sandbox.o build/security/ipc.o
avernella-liff: LIFF_PKGS = $(LIFF_WPE_PKGS)
avernella-liff: build/liff_browser.o $(LIFF_OBJECTS)
avernella-liff-gtk: LIFF_PKGS = $(LIFF_GTK_PKGS)
avernella-liff-gtk: build/liff_browser_gtk.o $(LIFF_OBJECTS)
avernella-liff avernella-liff-gtk:
	$(CC) $(CFLAGS) $(LDFLAGS) $(LINK_FLAGS) $(RO_SEGMENT) $(PACKED_RELOCS) $(STRIP_RELEASE) -o build/$@ $^ $(shell pkg-config --libs $(LIFF_PKGS))
	mv -f build/$@ $@

build/liff_browser.o: CPPFLAGS += $(shell pkg-config --cflags $(LIFF_WPE_PKGS))
build/liff_browser_gtk.o: CPPFLAGS += -DLINE_LIFF_GTK $(shell pkg-config --cflags $(LIFF_GTK_PKGS))
build/liff_browser_gtk.o: src/liff_browser.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Installation. avernella finds avernella-liff in ../lib/avernella beside its
# own directory, so the prefix needs no build-time setting. Build first as
# yourself, then install: make && sudo make install.
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
LIBEXECDIR = $(PREFIX)/lib/avernella
DATADIR = $(PREFIX)/share

install:
	@test -x avernella || { echo "Run make before make install." >&2; exit 1; }
	install -Dm755 avernella "$(DESTDIR)$(BINDIR)/avernella"
	if [ -f avernella-vulkan.so ]; then install -Dm644 avernella-vulkan.so "$(DESTDIR)$(LIBEXECDIR)/avernella-vulkan.so"; fi
	if [ -x avernella-liff ]; then install -Dm755 avernella-liff "$(DESTDIR)$(LIBEXECDIR)/avernella-liff"; fi
	if [ -x avernella-liff-gtk ]; then install -Dm755 avernella-liff-gtk "$(DESTDIR)$(LIBEXECDIR)/avernella-liff-gtk"; fi
	install -Dm644 data/avernella.desktop "$(DESTDIR)$(DATADIR)/applications/avernella.desktop"
	install -Dm644 data/avernella.png "$(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/avernella.png"
	install -Dm644 data/avernella-mark.png "$(DESTDIR)$(DATADIR)/icons/hicolor/64x64/apps/avernella.png"
	$(RM) "$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/avernella.svg"

uninstall:
	rm -f "$(DESTDIR)$(LIBEXECDIR)/avernella-vulkan.so"
	rm -f "$(DESTDIR)$(BINDIR)/avernella" "$(DESTDIR)$(LIBEXECDIR)/avernella-liff" "$(DESTDIR)$(LIBEXECDIR)/avernella-liff-gtk" \
		"$(DESTDIR)$(DATADIR)/applications/avernella.desktop" \
		"$(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/avernella.png" \
		"$(DESTDIR)$(DATADIR)/icons/hicolor/64x64/apps/avernella.png" \
		"$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/avernella.svg"
	rmdir "$(DESTDIR)$(LIBEXECDIR)" 2>/dev/null || true

build:
	mkdir -p $@

# Regenerate the header only when the revision changes.
ifneq ($(file <build/line_build_commit.h),$(COMMIT_DEFINE))
.PHONY: force-commit-header
build/line_build_commit.h: force-commit-header
endif
build/line_build_commit.h: | build
	@printf '%s\n' '$(COMMIT_DEFINE)' > $@.tmp
	mv -f $@.tmp $@

build/ui.o: override CPPFLAGS += -Ibuild
build/ui.o: build/line_build_commit.h

ifneq ($(file <build/state_keyring_config.h),$(KEYRING_DEFINE))
.PHONY: force-keyring-header
build/state_keyring_config.h: force-keyring-header
endif
build/state_keyring_config.h: | build
	@printf '%s\n' '$(KEYRING_DEFINE)' > $@.tmp
	mv -f $@.tmp $@

build/main.o: override CPPFLAGS += -Ibuild
build/main.o: build/state_keyring_config.h

build/avernella_%.inc: data/avernella-%.png | build
	@od -An -v -t u1 $< | awk '{for (i = 1; i <= NF; i++) printf "%s,", $$i; print ""}' > $@.tmp
	mv -f $@.tmp $@

build/ui_logo.o: override CPPFLAGS += -Ibuild
build/ui_logo.o: build/avernella_mark.inc build/avernella_welcome.inc

build/xdg-activation-v1-%.wayland: $(WAYLAND_PROTOCOLS)/staging/xdg-activation/xdg-activation-v1.xml | build
	$(WAYLAND_SCANNER) $* $< $@

build/xdg-foreign-unstable-v1-%.wayland: $(WAYLAND_PROTOCOLS)/unstable/xdg-foreign/xdg-foreign-unstable-v1.xml | build
	$(WAYLAND_SCANNER) $* $< $@

build/xdg-foreign-unstable-v2-%.wayland: $(WAYLAND_PROTOCOLS)/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml | build
	$(WAYLAND_SCANNER) $* $< $@

.SECONDARY: $(WAYLAND_NAMES:%=build/%-client-header.wayland) $(WAYLAND_NAMES:%=build/%-private-code.wayland)
build/%-client.h: build/%-client-header.wayland
	cp $< $@

build/%-code.o: build/%-private-code.wayland Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c -c $< -o $@

build/ui_window.o: override CPPFLAGS += -Ibuild
build/ui_window.o: $(WAYLAND_NAMES:%=build/%-client.h)

build/%.o: src/%.c Makefile | build
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/test-emoji-message: tests/emoji_message.c build/messaging.o build/rich_message.o build/sealing.o build/thrift.o build/liff_url.o
build/test-ui-emoji: tests/ui_emoji.c build/ui_emoji.o build/ui_theme.o build/ui_motion.o $(UI_LANG_OBJECTS)
build/test-ui-apng: tests/ui_apng.c build/ui_apng.o build/ui_pixels.o
build/test-emoji: tests/emoji.c build/emoji.o
build/test-media: tests/media.c build/media.o build/media_crypto.o build/media_io.o build/sealing.o build/thrift.o build/messaging.o build/rich_message.o build/liff_url.o
build/test-call: LDFLAGS += -Wl,--wrap=SDL_OpenAudioDevice,--wrap=SDL_CloseAudioDevice,--wrap=SDL_QueueAudio,--wrap=SDL_DequeueAudio
build/test-call: tests/call.c build/call.o build/call_group.o build/call_schema.o build/call_wire.o build/call_audio.o build/call_denoise.o build/call_video.o build/call_srtp.o build/call_pdtp.o build/camera_pixels.o build/media_io.o build/tune.o build/udp_batch.o
build/test-call-group: tests/call_group.c build/call_group.o build/call_schema.o build/call_wire.o
build/test-call-fec: tests/call_fec.c build/call_fec.o build/call_srtp.o
build/test-pdtp: tests/pdtp.c build/call_pdtp.o build/call_srtp.o
build/test-udp-batch: tests/udp_batch.c build/udp_batch.o
build/test-sandbox: tests/sandbox.c build/sandbox.o build/security/sandbox.o build/security/ipc.o build/udp_batch.o
build/test-security-sandbox: tests/test_sandbox.c build/security/liff_filter.o build/security/sandbox.o build/security/ipc.o build/security/vault.o build/sealing.o build/state.o build/state_crypto.o build/thrift.o
build/test-gpu-health: tests/gpu_health.c build/gpu_health.o
build/test-call-video: tests/call_video.c build/call_video.o build/call_srtp.o build/call_schema.o build/call_wire.o build/screencast.o build/screencast_dma.o build/screencast_cinnamon.o build/monitor_picker.o build/camera_pixels.o build/tune.o
build/test-call build/test-call-video: build/call_fec.o
# Bad-network harness: a virtual clock and simulated audio devices.
build/test-call-net: private LDFLAGS += -Wl,--wrap=line_denoise_new,--wrap=g_get_monotonic_time,--wrap=SDL_InitSubSystem,--wrap=SDL_OpenAudioDevice,--wrap=SDL_CloseAudioDevice,--wrap=SDL_PauseAudioDevice,--wrap=SDL_ClearQueuedAudio,--wrap=SDL_QueueAudio,--wrap=SDL_GetQueuedAudioSize,--wrap=SDL_DequeueAudio
build/test-call-net: tests/call_net.c build/call_fec.o build/call_video.o build/call_srtp.o build/call_audio.o build/test-call_denoise.o build/camera_pixels.o build/tune.o
build/test-screencast-portal: private LDFLAGS += -Wl,--wrap=pw_stream_queue_buffer
build/test-screencast-portal: private CFLAGS += -Wno-pedantic
build/test-screencast-portal: tests/screencast_portal.c build/screencast.o build/screencast_dma.o build/camera_pixels.o
ifeq ($(CALL_VP8_JETSON),1)
build/call_vp8_jetson.o: private CPPFLAGS += $(shell pkg-config --cflags $(JETSON_PKGS))
avernella build/test-call build/test-call-video build/test-call-net: private LDLIBS += $(shell pkg-config --libs $(JETSON_PKGS))
build/test-call build/test-call-video build/test-call-net: build/call_vp8_jetson.o
endif
build/test-call build/test-call-video build/test-call-net: build/call_vp8.o build/call_vp8_device.o
build/test-call build/test-call-net: build/call_vp8_encode.o
build/test-call-video: build/test-call_vp8_encode.o
build/test-call-video: private LDFLAGS += -Wl,--wrap=call_vp8_encoder_open,--wrap=call_vp8_encoder_frame,--wrap=call_vp8_encoder_close,--wrap=call_vp8_encoder_name
build/test-call-video: private CPPFLAGS += -Ibuild
# Hardware call codecs are optional. Retain the original build switch.
CALL_HWDECODE_PKGS = libavcodec libavutil
CALL_HWDECODE ?= $(shell pkg-config --exists $(CALL_HWDECODE_PKGS) && echo 1 || echo 0)
CALL_HWCODECS ?= $(CALL_HWDECODE)
ifeq ($(CALL_HWCODECS),1)
TESTS += build/test-vp8-dma
build/test-vp8-dma: private CPPFLAGS += $(shell pkg-config --cflags $(CALL_HWDECODE_PKGS))
build/test-vp8-dma: private LDLIBS += $(shell pkg-config --libs $(CALL_HWDECODE_PKGS))
build/test-vp8-dma: private LDFLAGS += -Wl,--wrap=av_hwframe_map
build/test-vp8-dma: tests/call_vp8_dma.c build/call_vp8_dma.o
avernella build/test-call build/test-call-video build/test-call-net: build/call_vp8_dma.o
build/call_vp8_dma.o: private CPPFLAGS += $(shell pkg-config --cflags $(CALL_HWDECODE_PKGS))
build/call_vp8.o build/call_vp8_encode.o build/test-call_vp8_encode.o build/test-call-video: private CPPFLAGS += $(shell pkg-config --cflags $(CALL_HWDECODE_PKGS))
build/test-call-video: private LDFLAGS += -Wl,--wrap=av_hwframe_transfer_data
build/test-call-video: private LDFLAGS += -Wl,--wrap=av_hwdevice_ctx_create,--wrap=g_file_read_link,--wrap=g_file_get_contents
avernella build/test-call build/test-call-video build/test-call-net: private LDLIBS += $(shell pkg-config --libs $(CALL_HWDECODE_PKGS))
endif
CALL_HWDECODE_DEFINE := \#define CALL_HWDECODE $(CALL_HWCODECS)
ifneq ($(file <build/call_hwdecode_config.h),$(CALL_HWDECODE_DEFINE))
.PHONY: force-hwdecode-config
build/call_hwdecode_config.h: force-hwdecode-config
endif
build/call_hwdecode_config.h: | build
	@printf '%s\n' '$(CALL_HWDECODE_DEFINE)' > $@.tmp
	mv -f $@.tmp $@
JETSON_DEFINE := \#define CALL_VP8_JETSON $(CALL_VP8_JETSON)
ifneq ($(file <build/call_jetson_config.h),$(JETSON_DEFINE))
.PHONY: force-jetson-config
build/call_jetson_config.h: force-jetson-config
endif
build/call_jetson_config.h: | build
	@printf '%s\n' '$(JETSON_DEFINE)' > $@.tmp
	mv -f $@.tmp $@
build/call_vp8.o build/call_vp8_encode.o build/test-call_vp8_encode.o: private CPPFLAGS += -Ibuild
build/call_vp8.o build/call_vp8_encode.o: build/call_hwdecode_config.h
build/call_vp8_encode.o: build/call_jetson_config.h
# Keep the real denoiser behind the test wrapper even under GCC LTO.
build/test-call_denoise.o: src/call_denoise.c Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-lto -MMD -MP -c $< -o $@
# Keep linker wrappers at the encoder boundary visible in the video tests.
build/test-call_vp8_encode.o: src/call_vp8_encode.c Makefile build/call_hwdecode_config.h build/call_jetson_config.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-lto -MMD -MP -c $< -o $@
# GCC vectorizes the per-pixel inner loop while keeping the rest of the
# portable release built for size. Preserve explicit debug/sanitizer flags.
build/ui_pixels.o: CFLAGS += -O3
# Per-pixel conversion loops: speed over size, vectorized per CPU.
build/camera_pixels.o build/screencast.o: CFLAGS += -O3
# PipeWire's SPA headers use GNU statement expressions.
build/screencast_dma.o build/screencast.o: CFLAGS += -Wno-pedantic
build/qr_scan.o: CPPFLAGS += $(shell pkg-config --cflags zbar)
build/test-ui-pixels: tests/ui_pixels.c build/ui_pixels.o build/ui_gpu_scroll.o
build/test-ui-image-view: tests/ui_image_view.c build/ui_image_view.o

build/test-ui-motion: tests/ui_motion.c build/ui_motion.o
build/test-ui-theme: tests/ui_theme.c build/ui_theme.o
build/test-ui-lang: tests/ui_lang.c $(UI_LANG_OBJECTS)
build/test-audio: LDFLAGS += -Wl,--wrap=SDL_QueueAudio,--wrap=SDL_GetQueuedAudioSize,--wrap=SDL_DequeueAudio
build/test-audio: tests/call_audio.c build/call_audio.o build/call_srtp.o build/call_denoise.o
build/test-video-record: CPPFLAGS += -DLINE_RECORD_TEST
build/test-video-record: tests/video_record.c src/video_record.c src/gpu_health.c
build/test-video-play: tests/video_play.c build/video_play.o
build/test-media-io: tests/media_io.c build/media_io.o
build/test-obs: LDFLAGS += -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo
build/test-obs: tests/obs_http.c build/obs_http.o build/http_tune.o
build/test-thrift: tests/thrift.c build/thrift.o
build/test-state: tests/state.c build/state.o build/state_crypto.o
build/test-state-crypto: tests/state_crypto.c build/state_crypto.o
build/test-messaging: tests/messaging.c build/messaging.o build/rich_message.o build/liff_url.o
build/test-friends: tests/friends.c build/friends.o build/thrift.o
build/test-liff: tests/liff.c build/liff.o build/liff_url.o build/thrift.o
build/test-chiptune: tests/chiptune.c build/chiptune.o
build/test-ui-sounds: private LDFLAGS += -Wl,--wrap=SDL_OpenAudioDevice,--wrap=SDL_CloseAudioDevice,--wrap=SDL_PauseAudioDevice
build/test-ui-sounds: tests/ui_sounds.c build/ui_sounds.o build/chiptune.o
build/test-denoise: tests/call_denoise.c build/call_denoise.o
build/test-profile-image: tests/profile_image.c build/profile_image.o build/media_io.o
build/test-rich-message: tests/rich_message.c build/rich_message.o build/liff_url.o
build/test-clipboard-paste: tests/clipboard_paste.c build/clipboard_paste.o
build/test-groups: tests/groups.c build/groups.o build/thrift.o build/liff_url.o
build/test-square: LDFLAGS += -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo
build/test-square: tests/square.c build/square.o build/square_search.o build/groups.o build/rich_message.o build/profile_image.o build/media_io.o build/thrift.o build/liff_url.o build/http_tune.o
build/test-chat-lists: tests/chat_lists.c build/chat_lists.o
build/test-shared-content: LDFLAGS += -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo
build/test-shared-content: tests/shared_content.c build/shared_content.o build/http_tune.o
build/test-ui-instance: tests/ui_instance.c build/ui_instance.o
build/test-ui-tray: tests/ui_tray.c build/ui_tray.o build/ui_logo.o build/ui_pixels.o $(UI_WINDOW_OBJECTS) $(UI_LANG_OBJECTS)
build/test-file-chooser: tests/file_chooser.c build/file_chooser.o
build/test-ui-notifications: tests/ui_notifications.c build/ui_notifications.o $(UI_LANG_OBJECTS)
build/test-ui-friends: tests/ui_friends.c build/ui_friends.o build/ui_motion.o build/ui_theme.o $(UI_LANG_OBJECTS)
build/test-ui-read: tests/ui_read.c
build/test-stickers: tests/stickers.c build/stickers.o build/thrift.o
build/test-sealing: tests/sealing.c build/sealing.o build/thrift.o
build/test-pair: tests/pair.c build/pair.o
build/test-password-login: tests/password_login.c build/password_login.o
build/test-transport: LDFLAGS += -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo,--wrap=curl_slist_free_all
build/test-transport: tests/transport.c build/transport.o build/thrift.o build/http_tune.o build/sandbox.o build/security/sandbox.o build/security/ipc.o
build/test-talk-sync: tests/talk_sync.c build/talk_sync.o build/thrift.o
build/test-live-sync: tests/live_sync.c build/live_sync.o build/talk_sync.o
build/test-flex-asset: LDFLAGS += -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo
build/test-flex-asset: tests/flex_asset.c build/flex_asset.o build/http_tune.o
build/test-ui-flex: tests/ui_flex.c build/ui_flex.o build/liff_url.o $(UI_LANG_OBJECTS)
build/test-qr-scan: LDFLAGS += -Wl,--wrap=dlopen
build/test-qr-scan: tests/qr_scan.c build/qr_scan.o
$(TESTS):
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LINK_FLAGS) $^ $(LDLIBS) -o $@

build/render-bench build/test-ui-call-fullscreen: private LDFLAGS += -Wl,--wrap=SDL_RenderPresent
build/test-ui-call-fullscreen build/test-ui-refresh: private LDFLAGS += -Wl,--wrap=ui_rpc
build/test-ui-refresh: tests/ui_refresh.c build/ui_gpu.o
build/test-ui-image-overlay: private LDFLAGS += -Wl,--wrap=ui_rpc
build/test-ui-image-overlay: tests/ui_image_overlay.c build/ui_gpu.o
build/test-ui-call-fullscreen: tests/ui_call_fullscreen.c build/ui_gpu.o
build/test-ui-chat-refresh: private LDFLAGS += -Wl,--wrap=SDL_PollEvent
build/test-ui-chat-refresh: tests/ui_chat_refresh.c build/ui_gpu.o build/tune.o
build/test-ui-liff: private LDFLAGS += -Wl,--wrap=readlink
build/test-ui-liff: tests/ui_liff.c build/ui_liff.o build/liff_url.o
build/test-ui-worker-stop: private LDFLAGS += -Wl,--wrap=SDL_PollEvent,--wrap=waitpid
build/test-ui-worker-stop: tests/ui_worker_stop.c build/ui_gpu.o build/tune.o
build/test-monitor-picker: private LDFLAGS += -Wl,--wrap=SDL_WaitEvent
build/test-monitor-picker: tests/monitor_picker.c build/monitor_picker.o build/ui_monitor_picker.o build/ui_theme.o $(UI_LANG_OBJECTS)
build/test-screencast-cinnamon: tests/screencast_cinnamon.c build/screencast_cinnamon.o
build/test-ui-scroll-scene: tests/ui_scroll_scene.c build/tune.o
build/test-ui-scroll-scene: private LDFLAGS += -Wl,--wrap=g_get_monotonic_time
build/render-bench: tests/render_bench.c build/ui_gpu.o
UI_FIXTURE_TARGETS = build/render-bench build/test-ui-call-fullscreen build/test-ui-scroll-scene build/test-ui-refresh
UI_FIXTURE_TARGETS += build/test-ui-chat-refresh build/test-ui-image-overlay
UI_FIXTURE_TARGETS += build/test-ui-worker-stop
$(UI_FIXTURE_TARGETS): build/ui.o build/gpu_health.o build/ui_draw.o build/ui_media.o build/video_record.o build/ui_motion.o build/ui_pixels.o build/ui_emoji.o build/ui_apng.o build/ui_friends.o build/ui_groups.o build/ui_openchat.o build/ui_find.o
$(UI_FIXTURE_TARGETS): build/ui_chat_lists.o build/chat_lists.o build/ui_chat_tools.o build/ui_shared_content.o build/ui_notifications.o build/ui_tray.o build/ui_instance.o build/video_play.o build/ui_flex.o build/flex_asset.o build/qr_scan.o
$(filter-out build/test-ui-scroll-scene,$(UI_FIXTURE_TARGETS)): build/screenshot.o
$(UI_FIXTURE_TARGETS): build/ui_image_view.o build/camera_pixels.o build/media_io.o build/ui_profile.o build/clipboard_paste.o build/ui_liff.o build/liff_url.o
$(UI_FIXTURE_TARGETS): $(UI_WINDOW_OBJECTS) build/file_chooser.o
$(UI_FIXTURE_TARGETS): build/ui_sounds.o build/chiptune.o build/ui_gpu_scroll.o build/ui_logo.o build/ui_theme.o build/sandbox.o build/security/sandbox.o build/security/ipc.o $(UI_LANG_OBJECTS)
build/render-bench:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LINK_FLAGS) $^ $(LDLIBS) -o $@

bench: build/render-bench
	./build/render-bench

.PHONY: bench-calls
bench-calls: build/test-call-net build/test-call-video build/test-udp-batch build/test-call-scheduler
	./build/test-call-net --verbose
	./build/test-call-net -m perf -r /net/service
	./build/test-call-video -m perf -p /video/presentation-blocking
	./build/test-call-video -m perf -p /video/receive-blocking
	./build/test-udp-batch -m perf -r /udp-batch/perf
	./build/test-call-scheduler -m perf

build/test-call-scheduler: tests/call_scheduler.c build/tune.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LINK_FLAGS) $^ $(LDLIBS) -o $@

# The main executable loads the native renderer only when opening a window.
GLSLANG ?= glslangValidator
SPIRV_VAL ?= spirv-val
VOLK_DIR ?= $(if $(wildcard /usr/include/volk.c),/usr/include)
VK_HEADERS ?=
VOLK_FLAGS = $(addprefix -I,$(VK_HEADERS) $(VOLK_DIR))
ifneq ($(VOLK_DIR),)
VOLK_LINK = build/volk.o
build/volk.o: $(VOLK_DIR)/volk.c $(VOLK_DIR)/volk.h Makefile | build
	$(CC) -Os -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections $(HARDEN) $(VOLK_FLAGS) -c $< -o $@
else
VOLK_LINK = -lvolk
endif

NATIVE_VULKAN ?= auto
ifeq ($(NATIVE_VULKAN),auto)
HAVE_NATIVE_VK := $(shell command -v $(GLSLANG) >/dev/null && command -v $(SPIRV_VAL) >/dev/null && \
	printf '\043include <volk.h>\n\043ifndef VK_EXT_descriptor_heap\n\043error older SDK\n\043endif\nint main(void){return 0;}\n' | \
	$(CC) $(VOLK_FLAGS) -x c - $(filter -l%,$(VOLK_LINK)) -o /dev/null >/dev/null 2>&1 && echo 1)
else
HAVE_NATIVE_VK := $(NATIVE_VULKAN)
endif
ifeq ($(HAVE_NATIVE_VK),1)
all: avernella-vulkan.so
endif

VK_SHADERS = ui_uber.vert ui_uber.frag ui_nv12.frag
VK_SPIRV = $(VK_SHADERS:%=build/%.spv)
$(VK_SPIRV): build/%.spv: src/render/% src/render/ui_limits.h Makefile | build
	$(GLSLANG) -V -Os --target-env vulkan1.3 $< -o $@
	$(SPIRV_VAL) --target-env vulkan1.3 $@

build/line_ui_spirv.h: $(VK_SPIRV)
	@{ for shader in $(VK_SHADERS); do \
		symbol=$$(echo "$$shader" | tr . _); \
		echo "static const uint32_t $${symbol}_spv[] = {"; \
		od -An -v --endian=little -t x4 build/$$shader.spv | \
			awk '{ for (i = 1; i <= NF; i++) printf "0x%s,", $$i; print "" }'; \
		echo '};'; \
	done; } > $@.tmp
	mv -f $@.tmp $@

VK_SOURCES = src/render/vk_nv12.c src/render/vk_ctx.c src/render/vk_frame.c src/render/vk_desc_buffer.c
VK_PREREQS = $(VK_SOURCES) $(wildcard src/render/*.h) build/line_ui_spirv.h $(filter build/%,$(VOLK_LINK))
build/vulkan-smoke: tests/vulkan_smoke.c tests/nv12_fixture.h src/render/vk_tex.c src/render/vk_canvas.c $(VK_PREREQS)
	$(CC) $(CFLAGS) $(VOLK_FLAGS) -Isrc -Ibuild $(LDFLAGS) $(LINK_FLAGS) -o $@ $(filter %.c,$^) $(VOLK_LINK) -ldl

build/vulkan-preview: tests/vulkan_preview.c src/render/vk_cache.c src/render/vk_present.c $(VK_PREREQS)
	$(CC) $(CFLAGS) $(VOLK_FLAGS) -Isrc -Ibuild $(shell pkg-config --cflags sdl2 glib-2.0) $(LDFLAGS) $(LINK_FLAGS) -o $@ $(filter %.c,$^) $(VOLK_LINK) $(shell pkg-config --libs sdl2 glib-2.0) -ldl

build/vulkan-video-bench: tests/vulkan_video_bench.c tests/nv12_fixture.h src/render/vk_canvas.c src/render/vk_tex.c src/render/vk_cache.c src/render/vk_present.c $(VK_PREREQS)
	$(CC) $(CFLAGS) $(VOLK_FLAGS) -Isrc -Ibuild $(shell pkg-config --cflags sdl2 glib-2.0) $(LDFLAGS) $(LINK_FLAGS) -o $@ $(filter %.c,$^) $(VOLK_LINK) $(shell pkg-config --libs sdl2 glib-2.0) -ldl

avernella-vulkan.so: src/render/vk_backend.c src/render/vk_backend.map src/render/vk_canvas.c src/render/vk_tex.c src/render/vk_cache.c src/render/vk_present.c $(VK_PREREQS)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden $(VOLK_FLAGS) -Isrc -Ibuild $(shell pkg-config --cflags sdl2 glib-2.0) \
		$(LDFLAGS) -shared -Wl,--exclude-libs,ALL,--gc-sections,-z,defs,-z,relro,-z,now,-z,noexecstack $(STRIP_RELEASE) \
		-Wl,--version-script=src/render/vk_backend.map \
		-o build/$@ $(filter %.c,$^) $(VOLK_LINK) $(shell pkg-config --libs sdl2 glib-2.0) -ldl
	cp build/$@ $@.tmp
	mv -f $@.tmp $@

.PHONY: vulkan-smoke vulkan-preview
vulkan-smoke: build/vulkan-smoke
	./build/vulkan-smoke $(VK_VENDOR)

vulkan-preview: build/vulkan-preview
	./build/vulkan-preview $(or $(VK_VENDOR),0) $(VK_PREVIEW_ARGS)

.PHONY: check-group-media
check-group-media: build/test-call-group build/test-call build/test-call-video build/test-pdtp build/test-ui-call-fullscreen build/test-ui-scroll-scene
	python3 tests/group_media.py

check: $(TESTS)
	@set -e; for test in $(TESTS); do ./$$test; done

# Coverage-guided fuzzing of the parsers that read network data, outside
# make check. GCC has no libFuzzer, so fuzz/driver.c is the engine, fed by
# -fsanitize-coverage. Sanitizer objects stay in build-fuzz/, apart from
# build/. See docs/en/testing.md#fuzzing.
FUZZERS = pdtp video pb group thrift
FUZZ_DIR = build-fuzz
FUZZ_CFLAGS ?= -O1 -g -fno-omit-frame-pointer
FUZZ_TIME ?= 600
FUZZ_ARGS ?=
# Video inputs carry several fragments of a picture.
FUZZ_MAX_LEN = $(if $(filter video,$*),16384,4096)
FUZZ_WARNINGS = -std=c17 -Wall -Wextra -Wpedantic -Wformat=2
FUZZ_SANITIZE = $(FUZZ_WARNINGS) $(FUZZ_CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_COVERAGE = -fsanitize-coverage=trace-pc,trace-cmp
FUZZ_CPPFLAGS = $(CPPFLAGS) -Ifuzz -I$(FUZZ_DIR)
FUZZ_BINARIES = $(FUZZERS:%=$(FUZZ_DIR)/fuzz-%)
FUZZ_ENGINE = $(FUZZ_DIR)/obj/fuzz-driver.o $(FUZZ_DIR)/obj/fuzz-coverage.o
FUZZ_RUNS = $(FUZZERS:%=fuzz-run-%)
FUZZ_CHECKS = $(FUZZERS:%=fuzz-check-%)

.PHONY: fuzz fuzz-run fuzz-check fuzz-clean $(FUZZ_RUNS) $(FUZZ_CHECKS)
fuzz: $(FUZZ_BINARIES) $(FUZZ_DIR)/seeds/.stamp

$(FUZZ_DIR)/obj:
	mkdir -p $@

$(FUZZ_DIR)/obj/%.o: src/%.c Makefile | $(FUZZ_DIR)/obj
	$(CC) $(FUZZ_CPPFLAGS) $(FUZZ_SANITIZE) $(FUZZ_COVERAGE) -MMD -MP -c $< -o $@

$(FUZZ_DIR)/obj/fuzz-%.o: fuzz/%.c Makefile | $(FUZZ_DIR)/obj
	$(CC) $(FUZZ_CPPFLAGS) $(FUZZ_SANITIZE) $(FUZZ_COVERAGE) -MMD -MP -c $< -o $@

# The engine itself is not traced. The coverage callbacks run on every basic
# block, so they are built without sanitizers too.
$(FUZZ_DIR)/obj/fuzz-driver.o: fuzz/driver.c Makefile | $(FUZZ_DIR)/obj
	$(CC) $(FUZZ_CPPFLAGS) $(FUZZ_SANITIZE) -MMD -MP -c $< -o $@

$(FUZZ_DIR)/obj/fuzz-coverage.o: fuzz/coverage.c Makefile | $(FUZZ_DIR)/obj
	$(CC) $(FUZZ_CPPFLAGS) $(FUZZ_WARNINGS) -O2 -g -MMD -MP -c $< -o $@

# Software VP8 only: the fuzzers never probe GPUs.
$(FUZZ_DIR)/call_hwdecode_config.h: | $(FUZZ_DIR)/obj
	@printf '%s\n' '#define CALL_HWDECODE 0' > $@
$(FUZZ_DIR)/call_jetson_config.h: | $(FUZZ_DIR)/obj
	@printf '%s\n' '#define CALL_VP8_JETSON 0' > $@
$(FUZZ_DIR)/obj/call_vp8.o $(FUZZ_DIR)/obj/call_vp8_encode.o: $(FUZZ_DIR)/call_hwdecode_config.h $(FUZZ_DIR)/call_jetson_config.h

$(FUZZ_DIR)/fuzz-pdtp: $(FUZZ_DIR)/obj/fuzz-pdtp.o $(FUZZ_DIR)/obj/call_pdtp.o $(FUZZ_DIR)/obj/call_srtp.o
$(FUZZ_DIR)/fuzz-video: $(FUZZ_DIR)/obj/fuzz-video.o $(FUZZ_DIR)/obj/call_fec.o $(FUZZ_DIR)/obj/call_video.o $(FUZZ_DIR)/obj/call_srtp.o
$(FUZZ_DIR)/fuzz-video: $(FUZZ_DIR)/obj/call_vp8.o $(FUZZ_DIR)/obj/call_vp8_device.o $(FUZZ_DIR)/obj/call_vp8_encode.o $(FUZZ_DIR)/obj/tune.o
$(FUZZ_DIR)/fuzz-pb: $(FUZZ_DIR)/obj/fuzz-pb.o $(FUZZ_DIR)/obj/call_wire.o $(FUZZ_DIR)/obj/call_schema.o
$(FUZZ_DIR)/fuzz-group: $(FUZZ_DIR)/obj/fuzz-group.o $(FUZZ_DIR)/obj/call_group.o $(FUZZ_DIR)/obj/call_schema.o $(FUZZ_DIR)/obj/call_wire.o
$(FUZZ_DIR)/fuzz-thrift: $(FUZZ_DIR)/obj/fuzz-thrift.o $(FUZZ_DIR)/obj/thrift.o
$(FUZZ_BINARIES): $(FUZZ_ENGINE)
	$(CC) $(FUZZ_SANITIZE) $(LDFLAGS) -Wl,--as-needed $^ $(LDLIBS) -o $@

# Seed inputs rebuilt from the synthetic fixtures in tests/.
$(FUZZ_DIR)/fuzz-seeds: $(FUZZ_DIR)/obj/fuzz-seeds.o $(FUZZ_DIR)/obj/call_group.o $(FUZZ_DIR)/obj/call_schema.o $(FUZZ_DIR)/obj/call_wire.o
$(FUZZ_DIR)/fuzz-seeds: $(FUZZ_DIR)/obj/call_fec.o $(FUZZ_DIR)/obj/call_pdtp.o $(FUZZ_DIR)/obj/call_srtp.o $(FUZZ_DIR)/obj/call_video.o $(FUZZ_DIR)/obj/thrift.o
$(FUZZ_DIR)/fuzz-seeds: $(FUZZ_DIR)/obj/call_vp8.o $(FUZZ_DIR)/obj/call_vp8_device.o $(FUZZ_DIR)/obj/call_vp8_encode.o $(FUZZ_DIR)/obj/tune.o $(FUZZ_DIR)/obj/fuzz-coverage.o
	$(CC) $(FUZZ_SANITIZE) $(LDFLAGS) -Wl,--as-needed $^ $(LDLIBS) -o $@

$(FUZZ_DIR)/seeds/.stamp: $(FUZZ_DIR)/fuzz-seeds
	$(RM) -r $(FUZZ_DIR)/seeds
	./$< $(FUZZ_DIR)/seeds
	touch $@

# Each fuzzer runs for FUZZ_TIME seconds; make -j runs them side by side.
# New inputs collect in build-fuzz/corpus/NAME, failures in artifacts/.
fuzz-run: $(FUZZ_RUNS)
$(FUZZ_RUNS): fuzz-run-%: $(FUZZ_DIR)/fuzz-% $(FUZZ_DIR)/seeds/.stamp
	@mkdir -p $(FUZZ_DIR)/corpus/$* $(FUZZ_DIR)/artifacts
	@echo "fuzzing $* for $(FUZZ_TIME) s, log in $(FUZZ_DIR)/$*.log"
	@./$< -max_total_time=$(FUZZ_TIME) -max_len=$(FUZZ_MAX_LEN) -artifact_prefix=$(FUZZ_DIR)/artifacts/$*- $(FUZZ_ARGS) \
		$(FUZZ_DIR)/corpus/$* $(FUZZ_DIR)/seeds/$* > $(FUZZ_DIR)/$*.log 2>&1 || \
		{ tail -n 60 $(FUZZ_DIR)/$*.log; exit 1; }
	@tail -n 2 $(FUZZ_DIR)/$*.log

# Replays the seeds and the saved corpus once, without mutation.
fuzz-check: $(FUZZ_CHECKS)
$(FUZZ_CHECKS): fuzz-check-%: $(FUZZ_DIR)/fuzz-% $(FUZZ_DIR)/seeds/.stamp
	@mkdir -p $(FUZZ_DIR)/corpus/$*
	./$< -runs=0 -max_len=$(FUZZ_MAX_LEN) $(FUZZ_DIR)/corpus/$* $(FUZZ_DIR)/seeds/$*

fuzz-clean:
	$(RM) -r $(FUZZ_DIR)

clean:
	$(RM) avernella-vulkan.so build/avernella-vulkan.so
	$(RM) build/state_keyring.o build/state_keyring.d build/state_keyring_config.h
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(TESTS) build/call_vp8_dma.o build/call_vp8_dma.d build/render-bench build/line_build_commit.h build/avernella_mark.inc build/avernella_welcome.inc avernella
	$(RM) build/test-call_denoise.o build/test-call_denoise.d
	$(RM) build/test-call_vp8_encode.o build/test-call_vp8_encode.d build/call_vp8_jetson.o build/call_vp8_jetson.d build/call_jetson_config.h
	$(RM) build/liff_browser.o build/liff_browser.d build/liff_browser_gtk.o build/liff_browser_gtk.d avernella-liff avernella-liff-gtk
	$(RM) $(WAYLAND_NAMES:%=build/%-client.h) $(WAYLAND_NAMES:%=build/%-client-header.wayland) $(WAYLAND_NAMES:%=build/%-private-code.wayland)
	$(RM) build/test-call-scheduler build/vulkan-smoke build/vulkan-preview build/vulkan-video-bench build/volk.o build/line_ui_spirv.h $(VK_SPIRV)
	$(RM) -r $(FUZZ_DIR)/obj $(FUZZ_DIR)/seeds
	$(RM) $(FUZZ_BINARIES) $(FUZZ_DIR)/fuzz-seeds $(FUZZ_DIR)/call_hwdecode_config.h $(FUZZ_DIR)/call_jetson_config.h

-include $(KEYRING_OBJECT:.o=.d) build/call_vp8_dma.d $(OBJECTS:.o=.d) build/test-call_denoise.d build/test-call_vp8_encode.d build/liff_browser.d build/liff_browser_gtk.d
-include $(wildcard $(FUZZ_DIR)/obj/*.d)
