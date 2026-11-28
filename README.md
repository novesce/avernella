<p align="center">
  <img src="data/avernella.png" width="160" height="160" alt="Avernella’s pink-horned anime mascot with a chat bubble">
</p>

<h1 align="center">Avernella</h1>

Avernella is an independent Linux desktop client compatible with LINE personal accounts. It is written in C and runs directly on Linux. Avernella has no affiliation with LY Corporation. The LINE name identifies the service it connects to.

Avernella uses LINE’s companion protocol for personal accounts. LINE controls access and keeps its suspension criteria private, so the account risk of using this client is unknown. For an account where losing access would be serious, use an official LINE app. [LINE’s terms](https://terms.line.me/line_terms?lang=en) · [LINE’s suspension policy](https://help.line.me/line/smartphone?contentId=200002135&lang=en)

How the app is built and how LINE's service works, from sign-in and sync to Letter Sealing, media, official-account messages, LINE apps and calls, is documented in [docs/](docs/README.md), in [English](docs/en/README.md) and [Thai (ภาษาไทย)](docs/th/README.md).

## Run

Install the development libraries listed under [Build and test](#build-and-test), then build and run from this directory, or [install it](#install).

```sh
make
./avernella
```

Choose **Sign in with QR code** and scan the code with LINE on your phone, or choose **Sign in with email** and enter the email address and password registered to your LINE account. LINE may require a device check. If a code appears on the desktop, enter it in LINE on your main device. LINE's desktop sign-in flow uses the app for this check. If LINE asks for a different verification method, use the QR code. The client saves the approved session for later launches and keeps your password out of the session file. [LINE desktop sign-in help](https://help.line.me/?contentId=50001182)

The client uses the Windows companion wire profile and gives the linked device the name **Avernella**. Approval may replace an existing desktop session. [Companion protocol reference](https://github.com/softnetics/line-rs/tree/addf69c2e886a2364a396ce360093a62119dcb32)

## Install

After building, install the client, its in-app browser, a menu entry and an icon. Build as yourself first, then install as root.

```sh
make
sudo make install
```

This installs `avernella` in `/usr/local/bin` and the in-app browser in `/usr/local/lib/avernella`. Add `PREFIX=/usr` to install elsewhere, or install only for yourself, without root, with `make install PREFIX=$HOME/.local`. `sudo make uninstall` removes the installed files. Your session stays in `~/.local/state/avernella` by default.

## Appearance and motion

Press **Ctrl+,** or click the settings gear, then open **Appearance**. Changes take effect immediately.

- **Theme.** **LINE** is the default. The left bar switches between Friends and Chats, the chat list shows times, unread counts, drafts and recent message previews for chats you’ve opened, and messages appear in left and right bubbles. Choose **Avernella** for the denser chat list and messages grouped by sender. Both layouts use dark plum and hot pink.
- **Language.** The interface follows your desktop language until you select one here. Each language is listed by its own name. Supported languages are English, Japanese, Korean, Thai, Simplified Chinese, Traditional Chinese, Vietnamese, Indonesian, Malay, French, German, Italian, Spanish, Portuguese, Russian, Arabic and Turkish. Chinese defaults to Traditional for Taiwan, Hong Kong and Macau. Dates follow the selected language. Arabic text reads right to left within the same window layout. Chat messages and errors from LINE retain their original wording.
- **Accessibility → Reduce Motion.** Buttons dip on press and rebound on release, the selected chat’s pink accent grows from its center, and dialogs ease into place. Turn on **Reduce Motion** for immediate transitions and still emoji. Choose **Use desktop setting** to follow the desktop animation preference again.

Theme and language are saved on this computer. The motion setting is saved per account. For a single launch with reduced motion, run `LINE_REDUCE_MOTION=1 ./avernella`.

## Friends and groups

Open **Friends → Add friend** to enter a LINE ID, an international phone number with its country code (such as `+66812345678`), or a personal LINE link. **Read QR image** accepts a saved QR image. Check the returned name and photo, then choose **Add friend**. **My QR code** shows your existing personal link when LINE supplies it, and **Keep Memo** opens your personal self-chat directly. Open a friend to chat even if you've never messaged them. Use the **…** beside a friend for favorites, display names, hiding, blocking or deleting friends. **Friends → Manage** lists hidden and blocked friends so you can restore or delete them.

Use the **+** in the left navigation bar or beside the chat folder name to create a group. Name it, select any friends you want to invite, and review the list before creating it. The member list icon in a group's header opens the roster and invitation controls. Select a roster row for member removal or invitation revocation. **Member List → Settings** contains rename, **Change group photo**, invite link and QR controls, and leave. Available actions depend on group permissions. Choose **Invitations** from the new-group screen to join or decline a group you've been invited to.

**Friends → Manage** also opens recommendations and your profile. You can edit your display name and status and choose which recommended accounts to add.

## OpenChat and LINE links

Click **OpenChat** (`#`) in the navigation bar or sidebar to list your joined OpenChats, browse public OpenChats under **Discover** (popular keyword chips and search), join an OpenChat by invite link or ticket (`https://line.me/ti/g2/…` or `line://ti/g2/…`, including passcode and admin-approval rooms), or create a new OpenChat. Joined OpenChat rooms also appear in the chat list and under the built-in **OpenChats** folder.

In an OpenChat conversation you can read history and earlier pages, send text, replies, mentions (with inline `@` autocomplete), LINE emoji, stickers, photos, videos, audio and files, forward messages (text, media, stickers, locations and contacts), react, unsend, manage announcements, and open the header **Member List** icon to inspect OpenChat info, edit your per-OpenChat nickname/bio/photo, view or copy the invite link and QR code, join or create sub-chats, edit settings, approve or reject join requests, and moderate members (promote/demote co-admin, kick, ban, or delete messages for everyone).

URLs in messages (`https://`, `http://`, `line://`, `lineb://`) are underlined and clickable, and installing `avernella.desktop` registers Avernella as a handler for `line://` and `lineb://` desktop links (such as `line://ti/g2/…` for OpenChat, `line://ti/g/…` for groups, `line://ti/p/…` for friends, and `line://app/…` for LIFF). OpenChat requests and link routing are checked offline in `tests/square.c`, `tests/obs_http.c` and `tests/ui_instance.c` and have not yet been exercised against a live account. [OpenChat protocol notes](analysis/openchat.md)

## Profiles, settings and friend activity

These features use the capabilities supported by the LINE service.

- **Profile popout.** Click a sender's avatar or name in a chat, a shared contact card, or the avatar and name in a one-to-one chat header, to open a profile card. It shows the person's photo on a banner tinted with the photo's color, their display name, their original name when you set your own name for them, Friend, Favorite, Official and Blocked labels, and their LINE status message under **About me**. **Message** opens a chat with them. **Add Friend** adds someone you met in a group or who messaged you, and **Block** or **Unblock** applies to friends. The card opens immediately with what the window already knows and fills in from LINE (`getContactsV3`, or `getContactsV2` on older servers). Click your own name in the user panel to see your card.
- **User Settings.** Press the gear in the user panel (or in the LINE layout's navigation bar) or **Ctrl+,**. **My Account** shows your banner, avatar, LINE ID and status, with **Log Out**. **Profiles** has **Change Avatar** and editors for your display name and status message, with a live preview card and an unsaved-changes bar. **Privacy & Safety** opens your blocked, hidden and suggested lists, the Inbox, and server-side LINE account privacy toggles (LINE ID search, phone search, non-friend messages, secondary device login, and Letter Sealing). **Notifications** turns desktop notifications and sounds on or off and exposes server-side LINE notification toggles (account notifications, message preview, mention notifications, and reaction notifications). **Keybinds** lists every shortcut and opens the chat-list shortcut editor. **Ctrl+↑** and **Ctrl+↓** move between sections, and **Escape** closes the window.
- **Change your avatar.** **Change Avatar** accepts JPEG, PNG or WebP. The client crops the centered square, scales it to 640 × 640 with area averaging (JPEG files are reduced in libjpeg-turbo's decoder first) and uploads a JPEG to LINE's profile object store, as the phone app does. Transparent areas become white.
- **Friend activity.** When someone adds you, or sends you a friend request where LINE offers requests, a card appears at the bottom right. While the window is in the background, you also get a desktop notification. The Inbox button in the user panel and the **Friends** row show a count. The Inbox lists incoming friend requests with accept and ignore buttons, and recent adds with **Add** to add them back.
- **Live profile updates.** When a friend changes their name or photo, or a block or friend list change happens on another device, the long-poll sync updates that one chat row, the open card and the Inbox in place, without reloading the chat list.
- **Mute a conversation.** Right-click a chat and choose **Mute conversation** to stop its desktop notifications. Mutes, like pins and folders, are saved locally for each account.

LINE has no presence, typing indicators for desktop clients, server-side mutual groups or profile banners on its personal-account API, so the client doesn't show them. Requests and profile reads are checked against synthetic fixtures in `tests/friends.c`. The avatar upload request is checked in `tests/obs_http.c`, and picture conversion in `tests/profile_image.c`. These features haven't been exercised against a live account yet.

The [Windows and Mac feature comparison](analysis/desktop-features.md) tracks supported workflows, live verification and remaining features.

## In a conversation

The sidebar shows LINE contact and group photos and unread counts. Drag the divider to change the sidebar width. Scrollbars appear when the chat list or message history has more content to scroll. Open a conversation to see up to 50 recent messages. Its unread badge clears when the newest message is visible at the bottom of the focused chat. While you read an older page or keep the app in the tray, new arrivals keep their unread badge. Choose **Earlier messages** to page back or **Back to latest** to return.

Your outgoing messages show **Sent** once LINE accepts them and **Read** when LINE reports that someone else has read them. In a group, **Read** means at least one other member has read that message. If a receipt is unavailable, the message continues to show **Sent**.

**Ctrl+F** finds a conversation by name. **Ctrl+Shift+F** searches the open conversation and can continue into earlier pages. **Ctrl+Alt+F** searches recent messages across chats in batches of up to six chats, using the latest 50 messages from each chat. **Enter** sends. Drafts stay with their conversations while the app is open.

When your connection comes back, Avernella refreshes the chat list and the latest messages in the open conversation. Visible images that failed to download, including pictures in Flex cards, retry automatically. You can also click the refresh arrow above the chat list or press **Ctrl+R**. If you’re reading an earlier page, it stays open.

Incoming text supports Letter Sealing v1 and v2. A message encrypted to a key generation that this device lacks displays a separate missing-key notice. [LINE Encryption Overview](https://www.lycorp.co.jp/en/privacy-security/line-encryption-whitepaper-ver2.2.pdf)

- Use the smiley in the message bar to browse LINE emoji and insert them inline. The picker contains an index of 9,175 items from 45 public packs and also checks for packs owned by the signed-in account. Artwork and individual emoji labels load from LINE on demand. Search by pack, item number, or a loaded label. The sender preserves an accessible text label, pack ID and item ID inside the encrypted message. Animated PNGs play inline. With reduced motion enabled, the client shows a still. [LINE emoji list](https://developers.line.biz/en/docs/messaging-api/emoji-list/)
- Open the emoji picker and choose **Stickers** to browse eligible packs on your account. Select a sticker to preview it, then choose **Send sticker**. Animated stickers play in the picker and chat, and sound stickers display a speaker badge and play audio in-app via `ffplay`. You can send a sticker as a reply and keep an unsent text draft. [LINE stickers](https://help.line.me/line/desktop/?contentId=20000182)
- Choose **+** in the message bar, drop a file, or press **Ctrl+V** to share a photo, video, audio clip or document. Click a voice or audio message to play it in-app via `ffplay`. Click a video card to play it in the LINE media player window. The card shows a thumbnail when LINE supplies one. Right-click the video or audio message to save a copy. Shared contact cards open that person's profile card on click, and location cards open coordinates on OpenStreetMap. Encrypted attachments use Letter Sealing and have a 64 MiB transfer limit. The photo viewer decodes JPEG, PNG and WebP. For a camera message, choose **Record video**, stop recording, then review or send the clip.
- Right-click a message to copy, reply, react, play sticker sound, or unsend your own message. **Forward** supports text, photos, audio, videos, files, stickers, locations and contact cards. Type `@` in a group or OpenChat to open the inline mention autocomplete popup, or use **Mention sender** from the message menu. **Announce** shares a text preview in the chat's server-managed announcement list. The confirmation shows that announcement previews are outside Letter Sealing. On compatible SDL backends, **Copy image** puts a PNG on the system clipboard. With **Ctrl+V** in the message bar, a picture you copied in any app (PNG, WebP or JPEG, up to 32 MiB) opens the **Send photo** preview, and a file copied in your file manager opens as an attachment. Other clipboard contents paste as text. The pasted picture is staged in a private `$XDG_RUNTIME_DIR/avernella` file, which is deleted after sending or when you close the preview. Picture paste uses SDL3's clipboard through sdl2-compat, as **Copy image** does, so it works on X11 and Wayland. With classic SDL2, Ctrl+V pastes text only.
- Rich messages, the image menus that official accounts send (LINE's imagemap), appear as an image card at the sender's aspect ratio. Hover an area to see what it does. Clicking it opens an https link, sends its preset text as you, copies text, or switches the card to another scene, matching LINE's own client. The client reads the same markup as LINE for Android (canvas, images, actions and scenes in `MARKUP_JSON`) and loads the 700-pixel base image from `DOWNLOAD_URL`, or the plain `DOWNLOAD_URL` when LINE's object store doesn't offer that width. Like Flex artwork, rich images load from any HTTPS host, as LINE's phone app does, so the sender's server sees your IP address. They can be JPEG, PNG or WebP at any resolution. Requests carry no cookies or credentials and never connect to your local network. Each picture is capped at 64 MiB only to stop a misbehaving server. If a card's picture stays blank, run `LINE_DEBUG_MEDIA=1 ./avernella` to print each failed image address, its HTTP status and any decode failure in the terminal. A rich message hosted elsewhere, or with markup the client can't read, shows its alternative text.
- Received Flex Messages appear as native cards. Carousels wrap to the chat width. The client loads artwork from any HTTPS host the sender uses, opens HTTPS actions on click, and shows the sender's alternative text when a card cannot be displayed. [LINE Flex Message elements](https://developers.line.biz/en/docs/messaging-api/flex-message-elements/)
- Links in Flex cards and rich messages open in LINE's in-app browser, as they do on a phone. LINE apps (LIFF) open from `https://liff.line.me/…`, `https://miniapp.line.me/…`, `https://line.me/R/app/…` and `line://app/…` links. The client asks LINE for the app's view (LiffService `issueLiffView`) and opens the page in its own window. The page gets the tokens `liff.init()` reads, and a user agent `liff.isInClient()` accepts, so coupons and member cards that say "this link only works in LINE" open as they do in LINE. A LIFF app opened from a chat sees that chat as its context. If LINE first needs your consent, its consent page opens in the same window. Other https links open in the window with LINE's in-app user agent. In the window, **Ctrl+Shift+O** opens the page in your own browser without LINE's tokens. **Alt+←** and **Alt+→** go back and forward, **Ctrl+R** reloads, **Ctrl+±** and **Ctrl+0** zoom, and **Ctrl+W** closes it. Camera, location and notification requests are refused. The window provides the part of LINE's native bridge (`window._liff`) that `liff.init()` waits for, closes on `liff.closeWindow()`, and opens `liff.openWindow()` pages. Phone-only features such as QR scanning and the share picker report that they are unavailable, so the page can carry on. If a LINE app stays on its loading screen, run `LINE_DEBUG_LIFF=1 ./avernella` and open it again to see page-load stages and supported bridge operations. Without the in-app browser installed, links open in your system browser. [LIFF](https://developers.line.biz/en/docs/liff/overview/) · [Protocol notes](analysis/liff.md)
- Open the chat's **⋯** menu for announcements and shared content. You can browse, create, rename and delete albums, open or delete album photos, read, post, edit and delete group text notes, like/unlike notes, view and post note comments, and find polls referenced by group notes. Choose **Take screenshot** to open your desktop's screenshot picker and review the result as an attachment. A compatible XDG Screenshot portal is required.
- In a one-to-one chat, the handset dials a voice call and the video button dials with your camera on. Both open a call stage at the top of that chat, and its messages and message bar stay usable underneath. Opening another chat shrinks the call to a bar at the bottom of the window; open the call's chat again, or press **Escape** and **Alt+C**, to switch between the two. Use **Mute** or **Unmute** to toggle your microphone, or press **Alt+M**. Use **Alt+V** for the camera. When either side turns a camera on, the stage shows a video tile for each person. Incoming calls ring as a card with green accept (**Alt+A**) and red decline (**Alt+D**) buttons. Voice uses native SDL audio, Opus and SRTP. Before your voice is encoded, RNNoise, a small neural noise suppressor, removes fan noise, wind and the hiss some laptop microphones pick up when the webcam beside them turns on, as LINE's own apps do. It costs about 0.5% of one CPU core. Set `LINE_NOISE_SUPPRESSION=0` to send the raw microphone.

## Experimental group calls

Open a group chat and click the handset or camera to start or join a call. The call stage shows the participant roster and lets you keep browsing chats. In a video call, select a participant to focus their camera. Group calls are enabled by default. Set `LINE_EXPERIMENTAL_GROUP_VIDEO=0` to hide the group camera button or `LINE_EXPERIMENTAL_GROUP_CALLS=0` to disable group calls. Group video uses LINE’s EVS3 VP8 format.

Live group calls with the official LINE Android app confirmed voice and camera video in both directions. A local relay fixture also exercises camera transmission and switching between two remote cameras. [Group call test results](docs/en/calls.md#group-calls)

## Experimental video calls

Experimental video is enabled by default. To disable it, launch the client with this setting.

```sh
LINE_EXPERIMENTAL_VIDEO=0 ./avernella
```

Use the camera button in a one-to-one chat. The client selects baseline VP8 with LINE's EVS3 packet format, including offers that advertise both VP8 and VP8A. It uses separate SRTP keys for audio and video. If a peer accepts audio and declines the supported video profile, the app keeps the voice connection. **Camera off** closes local capture during a call. The app asks the peer for HD video at 30 fps and sends the largest picture the peer's advertised bitrate and resolution class allow, up to 1080p. It reads a V4L2 camera in YUYV or MJPEG mode, choosing the larger smooth mode, and defaults to `/dev/video0`. Set `LINE_CALL_VIDEO_KBPS` to override the sending bitrate. Set `LINE_CAMERA_DEVICE` to select another device. Incoming and outgoing signaling and two-way media pass local relay tests with synthetic frames. A live call started from GNU/Linux also verified receiving and displaying video from the official LINE mobile app. [LINE desktop calls](https://help.line.me/line/desktop/?contentId=20000270&lang=en) · [LINE encryption overview](https://www.lycorp.co.jp/en/privacy-security/line-encryption-whitepaper-ver2.2.pdf)

### Screen sharing

During a one-to-one call or a connected group video call, press **Share Your Screen** or **Alt+S**. Choose a screen or window in the desktop picker. On Cinnamon Wayland with its default XApp/GTK portal configuration, Avernella can offer its own monitor picker when the desktop portal lacks ScreenCast. Select a monitor, check its name, then confirm **Share your screen**. Everyone in the call can see that entire monitor. Press the share button again to stop.

Avernella sends your screen in your video tile by default. In a group, viewers can select your tile to watch. The encoder uses VP8 screen-content mode and varies the resolution and frame rate to keep text sharp within the peer's advertised bitrate. If your camera was on, the app pauses it while sharing and restores it when you stop. Set `LINE_SHARE_WIDTH`, `LINE_SHARE_HEIGHT`, `LINE_SHARE_FPS` or `LINE_SHARE_KBPS` to cap the capture or encoding budget.

To watch the other person in full screen, double-click their video or click **Full screen** beneath it. **F11** opens the focused video, or the other person's video when the grid is showing. Press **Esc** or click **Exit full screen** to return to the chat and continue the call. A single click still focuses a tile within the call stage.

Incoming LINE presentations have a separate **LIVE** tile with fullscreen controls. In a group call you can change whose camera you watch while the shared screen stays visible. The group receiver has been tested against a local conference relay. Playback from a live Android group presenter still needs confirmation. The window displays the newest decoded frame independently of chat synchronization. The sender's device controls its resolution and frame rate. Set `LINE_SHARE_TRANSPORT=presentation` to try sending a separate native presentation on one-to-one calls with a PDTP data channel. Native outgoing presentation remains experimental. [Protocol notes and verification](analysis/video-call-wire.md)

Set `LINE_CALL_DEBUG=1` to report video negotiation, call control messages, packet counts every five seconds and first-frame milestones in the terminal.

## Sounds

Calls and messages have NES chiptune sounds, played on an emulated NES sound chip (two pulse channels, a triangle and a noise channel, ported from the Mesen emulator). An incoming call rings with an arpeggio ringtone, and an outgoing call plays a soft ringback until the other person answers. Answering, hanging up, turning your camera on or off, muting, and starting or stopping a screen share each have their own short sound. A new message chimes when the window is in the background or the message is in another chat; muted conversations stay silent. The sounds are written as notes on the chip's registers and rendered once in under 10 ms, with no sound files. Playback opens an audio stream named "Avernella sounds" only while a sound plays. Turn them off in **User Settings → Notifications → Sounds**.

## GNU/Linux desktop integration

Use the folder selector or right-click a chat to organize pins and custom folders. The folder menu also contains notification and shortcut preferences. These preferences are saved locally per account. Desktop notifications appear when the window is out of focus and show the chat name without a message preview.

Launching Avernella again brings back the existing window for that profile, including when it is minimized or hidden in the tray. With a StatusNotifier tray host, closing the window hides it in the tray. The tray menu opens or hides the window. Choosing Quit Avernella there opens a confirmation in the app. Ctrl+Q quits from the window. Set `LINE_DISABLE_TRAY=1` to make closing the window quit.

The authenticated worker uses LINE’s operation revision to establish a fresh sync baseline at startup and to detect changes if the long poll fails. Quitting Avernella stops the worker. **Sign out** ends the linked desktop session and removes its credentials. The desktop session lasts until you choose **Sign out** or LINE ends it. The tokens and their renewal schedule are saved with the session, so restarts and long idle periods keep you signed in. While the app runs, including from the tray, it renews its LINE tokens on the schedule LINE issues with them. An expired access token is also renewed on demand. A renewal or launch that cannot reach LINE keeps the saved session and retries with backoff, up to hourly for renewals and every five minutes at launch. The app asks you to pair again only when LINE explicitly rejects the session or its refresh token. Set `LINE_DEBUG_SESSION=1` to report renewals in the terminal.

## Performance

The window follows the display’s refresh rate while something is moving. Once the UI settles, it waits for input, new messages or the next media frame. It suspends drawing while minimized or hidden in the tray. On an older machine, `AVERNELLA_MAX_FPS=30 avernella` caps window redraws at 30 fps. Chat input, call audio and network events keep their usual timing.

A chat-row highlight can repaint just that row. On supported Vulkan displays, the renderer also reuses the unchanged parts of each swapchain image. Incoming call cameras and shared screens use NVIDIA NVDEC or the selected AMD or Intel GPU’s VA-API driver when it supports VP8. The worker copies decoded frames into the existing I420 display path. Outgoing cameras and screen shares use a hardware VP8 encoder when the driver exposes one. Otherwise libvpx handles encoding on the CPU. On a hybrid laptop, Avernella can use a second GPU for encoding if the display GPU lacks a VP8 encoder. The native Vulkan renderer converts and displays the pixels. To see which renderer is active, run `LINE_RENDER_LOG=1 avernella`.

Decoded thumbnails share a 12 MiB cache target. Recent pictures and pending downloads can use up to 32 MiB. Minimizing the window or hiding it in the tray releases cold decoded pictures. Animation frames have a separate 32 MiB budget.

New messages arrive by long poll. A separate thread keeps one LINE sync request open on its own HTTPS connection. Revision checks resume when the long poll fails, and RPC requests respect LINE’s `Retry-After` on HTTP 429 or 503, including after you quit and reopen the app. Native HTTPS requests use libcurl with TLS 1.2 or newer, certificate and hostname verification, and HTTP/2 negotiation. The RPC and media threads can reuse their own connections and share DNS and TLS session caches. glibc keeps two malloc arenas and returns large picture and video buffers to the kernel as soon as they are freed, and the window trims the heap when it loses focus. The call's media thread asks the kernel for tight timer slack, and background image decoding runs under `SCHED_BATCH` at lower priority. Pixel conversion loops are built with `-O3`.

The in-app browser is a separate process. It prefers WPE WebKit (`avernella-liff`) on Wayland and falls back to WebKitGTK (`avernella-liff-gtk`) when WPE is unavailable. The client never maps WebKit itself. The browser starts on the first link, with `pidfd_spawn` (clone3 with a pidfd), and exits when its window closes, so it costs no memory otherwise. The client and browser exchange messages over a `SOCK_SEQPACKET` socket, and a thread in the client sleeps in `recv` until the browser has something to say. LINE's tokens travel over that socket, never in arguments or the environment. The browser moves itself and WebKit's processes into a systemd transient scope, its own cgroup v2 leaf, with `MemoryHigh=768M`, `CPUWeight=50` and `IOWeight=50`, so pages can't take memory, CPU or disk time from the chat. WebKit's memory pressure monitor reads that cgroup limit. It raises its `oom_score_adj`, so the kernel reclaims the browser before the client. `PR_SET_PDEATHSIG` ends it with the client. It also sets a 1 ms timer slack, which WebKit's processes inherit, so page timers coalesce into fewer wakeups. Web processes are capped at 512 MB, after which WebKit releases memory or reloads the page, and the network process trims its caches beyond 128 MB. The browser keeps a small memory cache and no back-forward page cache. Its HTTP disk cache and cookies live in `~/.cache/avernella/liff-profiles/<account-hash>` and `~/.local/share/avernella/liff-profiles/<account-hash>`, so a LIFF app's scripts come from the kernel's page cache on the next visit. Videos wait for a click. WebKit's bubblewrap sandbox stays on.

## Install on Arch Linux or CachyOS

Install [avernella-bin](https://aur.archlinux.org/packages/avernella-bin) from the AUR with an AUR helper.

```sh
paru -S avernella-bin
```

## Build and test

Build with GCC 12 or newer, GNU Make, GNU binutils with the BFD linker, pkg-config and wayland-scanner. Development packages include SDL2, Wayland, wayland-protocols, Cairo/Pango, GLib/GIO, libqrencode, ZBar, json-c, libcurl, OpenSSL 3, libjpeg-turbo, libpng, WebP, zlib, Opus, libvpx, libsrtp2, RNNoise for call noise suppression and PipeWire 0.3 or newer for screen sharing. Screen sharing uses the running desktop portal or the Cinnamon Wayland monitor picker described below. The in-app browser builds a helper for each installed SDK, WPE WebKit (`wpe-webkit-2.0`) and WebKitGTK (`webkitgtk-6.0`). Keep the helpers next to `avernella` or use `make install`. Build with `LIFF_BROWSER=0` to omit both. The app loads ZBar when you import a QR image. Camera messages use the system FFmpeg executable with V4L2, PulseAudio, H.264 and AAC support. On NVIDIA, video messages use H.264 NVENC when the driver and encoder initialize. Video playback and clip review use the system `ffplay` executable, with NVDEC on NVIDIA or Vulkan Video on supported AMD and Intel drivers. The recording and playback tests use generated audio and video.

Install the packages on CachyOS or Arch Linux. The first command is enough to build and run the client.

```sh
sudo pacman -S --needed gcc binutils make pkgconf sdl2-compat wayland wayland-protocols \
  cairo pango glib2 qrencode zbar json-c curl openssl \
  libjpeg-turbo libpng libwebp opus libsrtp libvpx pipewire rnnoise
```

To build the native Vulkan module too, install the shader tools, recent Vulkan headers and volk. Binary releases include the module. A Vulkan loader and the driver for your GPU are needed at runtime.

```sh
sudo pacman -S --needed vulkan-headers volk glslang spirv-tools vulkan-icd-loader
```

Add WPE WebKit for the in-app browser on Wayland, and WebKitGTK for fallback or X11. WebKit uses bubblewrap and xdg-dbus-proxy for its sandbox.

```sh
sudo pacman -S --needed wpewebkit webkitgtk-6.0
```

Install `libsecret` before compiling to enable desktop keyring protection for saved sessions.

```sh
sudo pacman -S --needed libsecret
```

Install the portal backend for the session where you run Avernella. On Cinnamon Wayland, use the following command. XApp supplies the screenshot portal. When ScreenCast is missing, Avernella can share one monitor through Cinnamon’s Muffin compositor and PipeWire.

```sh
sudo pacman -S --needed xdg-desktop-portal xdg-desktop-portal-xapp
```

GNOME, Plasma, Hyprland and Sway use their own ScreenCast backends (`xdg-desktop-portal-gnome`, `xdg-desktop-portal-kde`, `xdg-desktop-portal-hyprland` and `xdg-desktop-portal-wlr`, respectively).

Install FFmpeg before compiling to enable hardware VP8 call encoding and decoding, attachment playback and camera recording. The optional Ubuntu build packages are `libavcodec-dev` and `libavutil-dev`. For software-only VP8 calls, build with `make CALL_HWCODECS=0 CALL_VP8_JETSON=0`.

```sh
sudo pacman -S --needed ffmpeg
```

For development, clang provides `clang-format`, the kernel headers provide `checkpatch.pl`, and Xvfb runs the X11 benchmark. Install the headers that match your kernel: `linux-headers` on Arch, or `linux-cachyos-headers` (or your CachyOS kernel variant's headers package, such as `linux-cachyos-bore-headers`) on CachyOS.

```sh
sudo pacman -S --needed clang linux-headers xorg-server-xvfb
```

zlib comes with every base system (`zlib`, or `zlib-ng-compat` on CachyOS).

Build and test from the repository directory.

```sh
make
make check
make bench
```

The Makefile requires GNU GCC for the application and test builds. Set `CC` to a GCC driver if you use a versioned executable or a compiler wrapper.

GNU Make 4.3 and newer use the CPU count reported by `nproc` for parallel builds by default. Use `make -j4` to set the number of jobs or `make -j1` for a serial build.

The C sources and tests follow Linux kernel layout conventions. Run `make format` to apply the project settings and `make format-check` to check them with clang-format. The formatter was tested with clang-format 23. Run `make checkpatch` to check the sources with the kernel's own `checkpatch.pl`. It is found in installed kernel headers (`linux-headers` on Arch and CachyOS), or set `CHECKPATCH` to its path in a kernel tree. `.checkpatch-typedefs` lists the GLib, SDL and other library types checkpatch does not know. The Makefile turns off the few kernel-only rules that do not fit a GLib program, and allows the long split JSON strings in test fixtures. [Linux kernel coding style](https://docs.kernel.org/process/coding-style.html)

GCC builds the stripped release with size optimization and link-time optimization. GNU ld drops unused sections and libraries. The build uses a single read-only data segment and packed relative relocations when the target toolchain supports them. The binary dynamically links system libraries, so its on-disk size excludes those dependencies. On another distribution or CPU architecture, rebuild against the local system libraries.

The size-optimized build omits unwind tables. For an unstripped diagnostic build with stack unwinding and debug symbols, run `make clean` followed by `make DEBUG=1`. Clean again before switching back to the release build.

The native Vulkan renderer composites the window, scrolls cached chat content and converts planar call video on the GPU. It prefers descriptor heaps and uses descriptor buffers on compatible Vulkan 1.3 drivers. Cairo and Pango rasterize new controls, text and images. The renderer reuses those pixels and uploads changed regions. When the native backend is unavailable, Avernella tries SDL Vulkan and then the software window surface.

The client prefers native Wayland in a Wayland session and falls back to X11 if Wayland is unavailable. It uses logical window coordinates for layout and input and draws at the drawable's pixel resolution. To select X11 explicitly, run `SDL_VIDEODRIVER=x11 ./avernella`. On a hybrid laptop, the native Vulkan renderer tries a compatible NVIDIA discrete GPU first, then other discrete GPUs and integrated graphics. The media worker receives the chosen adapter’s DRM render node. Set `AVERNELLA_RENDER_NODE` to the integrated GPU's render node to use that instead. With the SDL renderer on a hybrid NVIDIA laptop, `SDL_VIDEODRIVER=x11 prime-run ./avernella` selects the NVIDIA GPU through X11 or XWayland.

Profile and emoji downloads run independently of message sends. The image worker passes sealed Linux memory files to the UI. Decode workers can map those bytes directly. The optimized PNG pixel loop produces exact premultiplied colors and lets GCC vectorize across pixels. Letter Sealing caches recently verified shared secrets in session memory. The chat UI starts the network worker on the first sign-in attempt. The two processes communicate through a Unix socket pair. glibc starts the worker with `posix_spawn`. Linux `eventfd` wakes the I/O thread when there is outbound data. During healthy long polling, the worker sleeps until input arrives, a call invitation expires, or token maintenance is due. Separate one-shot timers track token renewal and fallback revision checks. On glibc 2.34 and newer, the spawn file actions close inherited descriptors above stderr.

The state directory is `$XDG_STATE_HOME/avernella` or `~/.local/state/avernella` by default. On first use, if this directory is absent, Avernella moves in a `kitatide` state directory or a `line-linux` one along with its in-app browser cookies and cache. The move carries over the sign-in and chat preferences. Close an older build before starting Avernella. The directory is mode 0700 and the session file is mode 0600. An exclusive directory lock, durable rename and reserved message sequence numbers protect against concurrent writers and interrupted sends. Messages and drafts stay in memory.

When the desktop keyring is available, Avernella stores the session tokens and Letter Sealing keys in an AES-256-GCM-encrypted session file. Existing plaintext sessions are migrated in place. An encrypted session needs the original keyring to reopen and a build that supports the format. A new sign-in without a keyring lasts until the app exits. See [session storage](docs/en/sign-in.md#the-session-file) for native and Flatpak details.

The [Android build analysis](analysis/portability.md) identifies the packaged architectures, startup activity, message RPC and Letter Sealing components. The C protocol implementation also draws on [the desktop companion reference](https://github.com/softnetics/line-rs/tree/addf69c2e886a2364a396ce360093a62119dcb32). Its [MIT notice](analysis/protocol-reference-LICENSE) is included.

## License

Avernella starts from the [line-linux source snapshot](https://github.com/novesce/line-linux/commit/86958170a61cadf8bda0d4edfb8b784e9954e0da). See the original project for its development history and contributor credits. line-linux was first released under GPL-2.0-only; its copyright holder, who is also Avernella's author, relicensed it under GPL-3.0-or-later, including the snapshot Avernella starts from.

Avernella is released under the GNU General Public License, version 3 or (at your option) any later version. The NES sound-chip emulation in `src/chiptune.c` is ported from [Mesen](https://github.com/nesdev-org/MesenCE), copyright Sour and the Mesen contributors, under the same license. The protocol references retain their MIT notices. [GPL-3.0-or-later](LICENSE) · [line-rs notice](analysis/protocol-reference-LICENSE) · [LINEJS notice](analysis/linejs-LICENSE) · [Beeper LINE notice](analysis/beeper-line-LICENSE)

If you redistribute a linked executable, review the terms for your system libraries. The default build links to OpenSSL 3, which uses the Apache License 2.0, a license compatible with GPL version 3. [OpenSSL license](https://openssl-library.org/source/license/index.html) · [GPL compatibility](https://www.gnu.org/licenses/license-list.html#apache2)

## Verification on CachyOS x86-64

The release uses GCC 16.2.1, GNU ld and glibc. LINE issued a real pairing QR code, the user approved the linked device and confirmed text delivery. Offline fixtures cover encrypted custom emoji containing Thai and astral Unicode, media object authentication, voice and experimental video signaling and bidirectional media, LINE sticker sending, file replacement, image handling, and animation timing. A public animated BROWN emoji decoded identically to FFmpeg across all 15 frames. Account-owned pack discovery has been tested against a simulated shop response. Live media delivery and voice interoperability with a phone have separate verification status above. [Voice protocol reference](https://github.com/evex-dev/linejs/tree/v3.6.5/packages/linejs/client/features/call) · [License](analysis/linejs-LICENSE)

`make bench` repeatedly renders a 920 × 630 fixture with 900 contacts and 50 messages using SDL’s dummy video driver. It reports CPU-side render and submit times. To exercise the X11 scrolling path and compare the final partial frame against a full redraw, use this command.

```sh
xvfb-run -a env LINE_REDUCE_MOTION=1 LINE_BENCH_VIDEODRIVER=x11 LINE_BENCH_DAMAGE=1 ./build/render-bench
```

`build/test-ui-motion` measures SDL timing on a headless fixture. `build/test-sealing -m perf -p /sealing/authenticated-history-cache` compares the cost of repeatedly decrypting the same history with and without reusing verified keys. Physical display pacing and network delays depend on the compositor, hardware and LINE service.

On this host the signed-out release window under Xvfb used about 14.2 MiB proportional memory. Its threads consumed 0.046 ms of CPU during a five-second idle sample. A 360-frame X11 scroll benchmark with reduced motion and 900 fixture contacts took 0.30 ms at the 95th percentile to render and submit the changed region. The headless timing test delivered about 60, 144 and 239 frames per second at targets of 60, 144 and 240 Hz.

In a 60-frame Wayland test across three window sizes, tracking damage per swapchain image reduced the total render area from 14,142,000 pixels to 2,587,700 pixels (82%). This measures how much of the window Vulkan had to redraw in that scene. [Vulkan test scene](tests/vulkan_preview.c)
