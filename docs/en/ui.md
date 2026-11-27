# The user interface

[Index](README.md) · Previous: [Architecture](architecture.md) · Next: [LINE protocol basics](protocol-basics.md)

Everything on this page runs in the **UI process**. It never talks to LINE directly: it asks the worker (see [Architecture](architecture.md#how-the-ui-and-the-worker-talk)) and draws what comes back.

Opening Avernella again raises the existing window. If the primary UI thread has exited and a driver thread still holds the activation socket, a new launch can reclaim it. A busy, living UI keeps its window. A window that has dropped its instance lock while exiting, but whose socket a blocked device driver keeps open, never reads requests again. A new launch therefore sends "show" and waits up to half a second for it to be read (`SIOCOUTQ` drops to zero). If the request stays unread, or the old window's queue is full, the new launch starts its own window. Each launch prints on standard error whether it handed off or started a new window.

## The main loop

`line_ui()` in [`ui.c`](../../src/ui.c) sets up SDL and then loops until `ui.quitting`:

1. **Is a frame due?** `frame_wait()` asks the frame clock ([`ui_motion.c`](../../src/ui_motion.c)) how long until the next frame. A frame is due when something is dirty (`ui.dirty`, `ui.video_dirty`), an animation is running, a smooth scroll is in progress, or an animated emoji needs its next picture. When nothing is due, the wait is infinite. When the window is hidden or minimized, nothing is drawn at all, but events are still received.
2. **Draw** if a frame is due. There are three sizes of redraw:
   - `ui_draw()` repaints the whole window: when anything is dirty, during an animation, or when both the chat list and the history scrolled.
   - `ui_draw_region(rect)` repaints only the chat list or only the conversation, when just that one scrolled.
   - `ui_draw_video()` presents new call video without repainting anything else.
3. **Wait for events** with `SDL_WaitEventTimeout(wait_ms)` (or `SDL_WaitEvent` when the wait is infinite). Input and worker packets wake it immediately.
4. **Handle events** through `event()`. At most 64 events are handled before drawing again, so continuous mouse movement can't starve painting.

Frame timing follows the monitor's refresh rate (`display_rate()` reads it from SDL), capped by `AVERNELLA_MAX_FPS`. Deadlines use the monotonic clock.

### Where events go

`event()` in `ui.c` routes each SDL event:

- **Worker packets** (SDL user event `wire.packet_event`) go to `handle_packet()`.
- **Call video frames** (`wire.preview_event`) go to `ui_media_preview_frame()`.
- **Input** first goes to whichever overlay is open. Each feature module exposes `ui_<module>_active()` and `ui_<module>_event()`. If the module is active it gets the event first and returns `TRUE` when it consumed it. The order in `event()` decides which overlay is on top. If no overlay takes the event, `key()` and `click()` in `ui.c` handle the main window.

### Interface state

The single `Interface ui` struct (declared in [`ui_private.h`](../../src/ui_private.h)) holds the main window's state: window size, the current chat list (`ui.chats`, a JSON array from the worker) and the indexes visible after filtering (`ui.visible`), the selected chat (`ui.selected`), its history (`ui.history`), the editors (search box, message box, sign-in fields), drafts per chat (`ui.drafts`), scroll offsets, which button rectangles are where (filled in by `ui_layout()`), and the capability flags the worker sent at sign-in (`ui.photos`, `ui.calls`, `ui.video_calls` and so on).

Feature modules keep their own state in a `static` struct inside their `.c` file. Only the main thread touches any of it.

**Focus.** `ui.focus` is one of the `Focus` values in `ui_private.h` (`F_SEARCH`, `F_LIST`, `F_MESSAGE`, `F_SEND`…). Tab and Shift+Tab move through them in `tab_focus()`.

**Compact mode.** When the window is 640 logical pixels wide or narrower (`ui_compact()`), only the chat list *or* the conversation is shown, and Escape goes back to the list. `ui_chat_visible()` and `ui_list_visible()` say which.

**Editors.** An `Editor` is a `GString` plus a cursor and selection anchor in UTF-8 byte offsets. `ui_edit_key()` and `ui_insert()` in `ui.c` implement editing, word movement, selection and clipboard. Text is measured with Pango, so Thai, combining marks and emoji work.

## The drawing pipeline

Drawing has two halves: **rasterizing** pixels on the CPU with Cairo and Pango, and **presenting** them through the GPU or SDL.

### Rasterizing

[`ui_draw.c`](../../src/ui_draw.c) draws everything into one backing image the size of the window in *device* pixels (logical size × display scale), held in `ui.buffer`, an `SDL_Surface` in `RGB888`. The Cairo context `cr` wraps that surface, and Pango lays out text into it.

- **Logical coordinates.** Layout and input use logical pixels. `ui_pixel_scale()` gives the device-pixel ratio, and Cairo is scaled once, so sharp text and lines come out at the real resolution on HiDPI screens.
- **Layout first.** `ui_layout()` computes every rectangle (rail, chat list, header, conversation, composer, buttons) for the current window size and theme. Drawing and hit-testing both use the same rectangles.
- **Caches.** Short labels and controls keep their Pango layouts in a fixed-size table (`label_layout()`), keyed by text, size, weight, width and alignment. Message bubbles are laid out once per history (`prepare_bubbles()`) into a `struct message_layout` array, so scrolling doesn't re-shape text.
- **Drawing helpers.** Other modules draw through the helpers exported in `ui_private.h`: `ui_draw_box()`, `ui_draw_rounded()`, `ui_draw_label()`, `ui_draw_button()`, `ui_draw_icon()`, `ui_draw_avatar()`, `ui_draw_switch()`, `ui_draw_tooltip()`, `ui_draw_image()`, `ui_scrim()` and so on.
- **Pictures.** Decoding happens on the decoder thread in [`ui_media.c`](../../src/ui_media.c) (`decode_image()`), which produces Cairo image surfaces. [`ui_pixels.c`](../../src/ui_pixels.c) premultiplies alpha in a loop GCC vectorizes (`-O3`). [`ui_apng.c`](../../src/ui_apng.c) decodes animated PNG frames and tells the loop when the next frame is due.

### Presenting

There are three presentation paths. The app picks the first that works and remembers it.

1. **The native Vulkan module** (`avernella-vulkan.so`, sources in [`src/render/`](../../src/render)). [`ui_gpu.c`](../../src/ui_gpu.c) looks for it beside the executable or in `../lib/avernella`, loads it with `dlopen`, and gets one symbol, `avernella_gpu_v1`, a `struct gpu_backend` with `open`, `close`, `size` and `draw` (see [`gpu_backend.h`](../../src/render/gpu_backend.h)). The window is created with `SDL_WINDOW_VULKAN`. Each frame the UI hands the module a `struct gpu_scene` ([`gpu_scene.h`](../../src/render/gpu_scene.h)): a pointer to the Cairo pixels, the damaged rectangle, up to 8 upload rectangles and up to 8 texture-to-screen copies, and up to three video planes (peer camera, own camera, shared screen).
2. **SDL's Vulkan renderer.** If the module is missing or fails, `ui_draw_init()` asks SDL for its `vulkan` render driver and keeps the canvas in an `SDL_Texture`.
3. **The SDL window surface.** Without Vulkan, the backing image is copied to the window surface. On Wayland this still uses SDL presentation.

`LINE_RENDER_LOG=1` prints which path was chosen. `LINE_DISABLE_GPU=1` forces path 3, and `AVERNELLA_VULKAN=sdl` skips path 1.

#### Inside the Vulkan module

The module ([`vk_ctx.c`](../../src/render/vk_ctx.c), [`vk_frame.c`](../../src/render/vk_frame.c), [`vk_canvas.c`](../../src/render/vk_canvas.c), [`vk_tex.c`](../../src/render/vk_tex.c), [`vk_present.c`](../../src/render/vk_present.c), [`vk_desc_buffer.c`](../../src/render/vk_desc_buffer.c)) does very little, on purpose:

- It picks a device that can present to the window and probes its features. It prefers **descriptor heaps** (`VK_EXT_descriptor_heap`) and falls back to **descriptor buffers** on Vulkan 1.3. `AVERNELLA_VK_DESCRIPTORS=buffer` forces the second.
- One graphics pipeline and one "uber" shader pair ([`ui_uber.vert`](../../src/render/ui_uber.vert), [`ui_uber.frag`](../../src/render/ui_uber.frag)), compiled to SPIR-V at build time, draw every primitive. A primitive (`struct ui_prim` in [`ui_batch.h`](../../src/render/ui_batch.h), 64 bytes) is a quad, glyph, image, dma-buf, the canvas or an I420 video frame. Four vertices per quad, clipping before rasterization, and short paths for the opaque canvas, glyph masks and I420 video.
- The Cairo canvas lives in a device texture. Only changed regions are uploaded. Two frame slots reuse mapped upload memory, and Synchronization2 barriers order transfers and shader reads.
- Call video is converted from I420 to RGB in the shader, so the CPU never converts video frames.
- During a call, a camera texture can change size while the GPU is reading it. The renderer checks for completion and retries on a later frame, so the window can process input in between.
- On drivers with `VK_KHR_incremental_present`, it tells the compositor which window region changed.
- On NVIDIA it presents with **FIFO latest ready** (`VK_KHR_present_mode_fifo_latest_ready`, or the EXT name on older drivers) when the window surface offers it. The display still updates once per refresh without tearing, but a newer ready frame replaces an older queued one. With plain FIFO, NVIDIA's Wayland driver blocks `vkQueuePresentKHR` on the UI thread for about one refresh interval. The UI paces its own frames to the display rate (`ui_frame_wait_ms()`), so the faster mode does not spin. A replaced frame never reaches the screen, so in this mode the presenter sends no incremental-present region. Other drivers keep FIFO, and `LINE_RENDER_LOG=1` names the mode. `AVERNELLA_PRESENT_MODE=fifo` turns it off, and `AVERNELLA_PRESENT_MODE=latest` also allows it on other drivers that offer it.

### Damage: drawing only what changed

`draw_frame()` in `ui_draw.c` is called with a region, or `NULL` for the whole window. It clips Cairo to that region and paints only what intersects it. The presenter then uploads only that rectangle. When only video changed, `ui_draw_video()` presents without any Cairo work.

### Scroll reuse

Scrolling the conversation is the most common redraw, so it has a special path. When the history can be reused (`reusable_history()`), the canvas texture keeps the conversation area as a **circular buffer**: `history_scroll.origin` says which texture row holds the first visible line. Scrolling by *dy* pixels:

1. moves the origin by *dy* (`advance_history()`),
2. repaints only the newly exposed rows, plus a two-pixel guard at the edges and any message row whose hover highlight changed (`history_damage.bands`, merged by `merge_history_bands()`),
3. uploads only those bands (`history_uploads()`),
4. draws the conversation as up to two texture copies that unwrap the circle (`ui_scroll_slices()` and `ui_scroll_quads()` in [`ui_gpu_scroll.c`](../../src/ui_gpu_scroll.c)).

Reuse is turned off whenever the visible rows could paint something that changes on its own: photos, videos, rich messages, Flex cards, LINE emoji, an overlay, an animation, the call stage, or a stuck-to-bottom history. Any doubt means a full repaint, which is always correct. `tests/ui_scroll_scene.c` and `make bench` with `LINE_BENCH_DAMAGE=1` check that the reused frame equals a full redraw.

### Animation and reduced motion

[`ui_motion.c`](../../src/ui_motion.c) provides the frame clock (`UiFrameClock`), smooth scrolling (`UiScroll`, sampled each frame) and the reduced-motion switch. `ui_reduce_motion()` is true when:

- the account's setting says so (**User Settings → Appearance**), stored as `motion` in the chat-lists file; or
- the setting follows the desktop and GNOME's `org.gnome.desktop.interface enable-animations` is off; or
- `LINE_REDUCE_MOTION=1`.

With reduced motion, scrolls jump, transitions end immediately and animated pictures show a still frame. `ui_draw.c` keeps small "property tracks" (`struct color_transition`, `struct slide_transition`) for hover fades and slides, and `ui_animation_deadline()` reports when the next one needs a frame.

Motion follows a few rules. Input takes effect on the frame it arrives in, and the animation only shows the change. Every motion is short (40–250 ms), starts fast and eases out, and can be interrupted. A retarget starts from the sampled value, so nothing jumps or queues. When the last motion settles, the deadline drops to zero and the loop waits for input again. What moves today:

| Interaction | Motion | Where |
| --- | --- | --- |
| Wheel, touchpad, Page Up/Down | 120 ms ease-out per impulse; notches add up and a reversal follows at once | `ui_scroll_by()` |
| Up/Down in the chat list | the list eases the selected row into view; a held key retargets from where the list is heading | `ui_scroll_to()`, `highlight()` in `ui.c` |
| Hover, selection, focus | 110–150 ms colour fade | `track_strength()` |
| Pressing a button | 40 ms press-in, 145 ms release with one soft overshoot (`ui_press_depth()`): action buttons, header and call buttons, the rail, and the composer's attach, emoji and Send | `button_depth()` |
| A new message while the history follows its bottom edge | the history glides up over 180 ms instead of jumping; a burst adds to what is left (`UiGlide`) | `follow_bottom()` |
| Opening a chat | the first page fades in and rises 6 px over 140 ms | `reveal_history()` |
| Compact pane change, dialogs | 10 px / 8 px slide with a soft settle (`ui_settle()`); the scrim fades | `slide()` |

The new-message glide runs only when the reader is at the bottom and the previous newest row is still there, followed by new rows. Loading an earlier page, a resize, a theme change, or a batch taller than the view snaps instead. While the glide or fade runs, `ui_draw_animation()` repaints only the conversation area, and `reusable_history()` turns scroll reuse off. Message-row hover in the history stays instant, so scroll reuse keeps working while the pointer rests on the history. With reduced motion, every row of the table reaches its end state on the same frame.

#### Scroll direction

Scrolling follows the desktop's natural-scrolling setting, separately for the touchpad and the mouse if the desktop splits them (GNOME, Cinnamon and KDE all do). The desktop applies the setting through libinput before SDL sees the event, so Avernella uses the values as they arrive: `ui_wheel_y()` in [`ui_motion.h`](../../src/ui_motion.h) returns SDL's precise delta (`preciseY`) when there is one, and the whole-notch count otherwise. `SDL_MOUSEWHEEL_FLIPPED` only reports that the values were already inverted. Compositors with `wl_seat` version 9 or later (GNOME 45 and later, KDE Plasma 6) send it, and inverting such an event again would undo natural scrolling, so no view does that. Every scrolled view goes through `ui_wheel_y()`: the message history and chat list (smooth, in pixels), Find, User Settings and shared-content details (pixels), and the friend, group, folder, chat-tool, attachment-picker, call-roster and emoji lists (rows). Row lists add fractional touchpad steps up to whole rows with `ui_wheel_rows()`, which drops the remainder when the direction reverses. The monitor picker runs in its own process and reads SDL's notch count, which follows the desktop in the same way. `tests/ui_motion.c` (`/motion/wheel-direction`) checks the mapping.

## Themes

[`ui_theme.c`](../../src/ui_theme.c) and [`ui_theme.h`](../../src/ui_theme.h) define two layouts. Under **User Settings → Appearance**, select Pink, Blue, Mint or Amber for either layout. The app saves the choice under `$XDG_CONFIG_HOME/avernella/accent` and applies it immediately.

| Theme | Look | How it differs in code |
| ----- | ---- | ---------------------- |
| **LINE** (default) | Modeled on LINE's desktop app: a narrow navigation bar with Friends and Chats tabs and the account buttons at its foot, 68-pixel chat rows showing the latest message, time and unread count, incoming bubbles on the left and your bubbles on the right, an input pane with emoji, file and **Send** buttons | `ui_theme_is_line()` is true. `line_bar()`, `line_chat_row()` and `line_composer()` in `ui_draw.c` draw it. `ui_profile_place()` moves the profile, Inbox and settings buttons into the bar. |
| **Avernella** | A compact layout with a server-style rail (unread chats pinned as round shortcuts), 44-pixel chat rows, messages grouped by sender, a user panel at the bottom left, and one rounded composer bar | `server_rail()`, `chats_panel()`, `user_panel()` and `composer()` in `ui_draw.c` |

Both layouts use Avernella’s dark plum surfaces and default to Pink. The chosen accent colors the sent bubbles, unread badges and active tab (`UI_BUBBLE_OUT`, `UI_NAV_ACTIVE`). The renderer uses `0xRRGGBB` values from the active palette.

The choice is read once at start (`ui_theme_init()`) from `~/.config/avernella/layout`: the file containing exactly `avernella\n` selects the Avernella theme, and anything else selects LINE. `ui_theme_toggle()` writes the file atomically and flips the theme. `ui_theme_changed()` in `ui.c` then fixes scroll offsets for the new row height (`ui_chat_row_height()`) and relayouts.

When you add a surface, use the palette constants rather than new literals, and test both themes.

## Interface language

The interface is translatable via [`src/ui_lang.c`](../../src/ui_lang.c), into 17 languages (`enum ui_language` in `ui_lang.h`). User-visible English strings in the sources are the keys: they are wrapped in `_()` (or `N_()` in static tables, translated where they are drawn), and each language has a table of `{ English, translation }` pairs in `src/ui_lang_<tag>.c`. A missing pair falls back to English. The language is chosen under **User Settings → Appearance** and saved in `~/.config/avernella/language`; without a saved choice it follows the desktop locale. To add a string, wrap it in `_()` and add a pair to every table. `tests/ui_lang.c` checks that translations keep each string's printf conversions.

## Features and where they live

Each feature module follows the same pattern, so once you know one you know them all:

| Function | Purpose |
| -------- | ------- |
| `ui_<m>_open()` / `_show()` | Open the screen or overlay |
| `ui_<m>_active()` | Is it open? |
| `ui_<m>_event(SDL_Event *)` | Handle input while open. Return `TRUE` if consumed. |
| `ui_<m>_reply(method, tag, result, error)` | Handle a worker reply. Return `TRUE` if the method is this module's. |
| `ui_<m>_draw()` | Paint it |
| `ui_<m>_resume_input()` | Restore text input when it regains focus |
| `ui_<m>_reset()` | Forget everything, at sign-out |

### Sign-in screen

`sign_in()` and `layout_sign_in()` in `ui_draw.c` draw it. `login()`, `toggle_sign_in()`, `start_password_worker()`, `retry_login()` and `reset_login()` in `ui.c` drive it. The QR code arrives as a `qr` event and is encoded with libqrencode. A `pin` event replaces it with a number to type on the phone. The password field is masked (`password_mask()`), and its buffer is wiped after use (`clear_password()`). The welcome portrait comes from [`ui_logo.c`](../../src/ui_logo.c).

### Chat list

- **Drawing:** `chats_panel()` and `line_chat_row()` in `ui_draw.c`. The time stamp follows LINE's rules (`list_stamp()`: time today, then "Yesterday", a weekday, a date). The latest message text is shown only for chats opened in this session, because the chat-list RPC carries only the message ID (`remember_preview()`).
- `refresh()` requests the chat list. Once the directory replies, the UI requests the open conversation’s latest page using the updated delivery ID. An earlier page stays open while the list updates. `chat_message()` moves one row when a new message arrives.
- The refresh button, **Ctrl+R**, and a recovered connection also retry failed image downloads as they come into view. Images already loaded stay cached.
- **Filtering:** typing in the search box (Ctrl+F) filters by name (`ui_refilter_chats()`).
- **Folders and pins:** [`ui_chat_lists.c`](../../src/ui_chat_lists.c) draws the folder selector and its menu, and [`chat_lists.c`](../../src/chat_lists.c) stores folders, pins, mutes and shortcuts per account. Right-click a chat to pin, mute or add it to a folder.
- **Muted chats:** A crossed-out bell appears next to the name in the chat list and the header. It reflects the LINE notification setting for a group or room and any local mute. Right-click a group and toggle **Strict mute** to suppress notifications, sounds and unread badges, including for mentions and `@All`. This setting is local to the signed-in account on this device. The chat still receives messages and sends read receipts when viewed.
- **Unread badges and read receipts:** see [Read receipts](#read-receipts) below.

### Conversation

- **History:** `prepare_bubbles()` lays out messages and `message_rows()` paints them, with sender grouping, day separators and reactions. The first page is the latest 50 messages. "Earlier messages" sends `olderHistory`, and "Back to latest" returns (`history_page()` in `ui.c`).
- **Composer:** `line_composer()` or `composer()` in `ui_draw.c`. Enter sends (`send_message()`). Drafts are kept per chat in `ui.drafts` while the app runs.
- **Message menu:** right-click a message. `actions()` in `ui_media.c` builds the list for that message: reply, copy, react, view photo, copy image, play video, save, forward, announce, mention sender (groups), unsend (your own). `menu_action()` carries it out.
- **Replies:** choosing Reply sets `ui.reply_id`, shown above the composer. Escape cancels it.
- **Mentions:** "Mention sender" inserts `@Name` with `ui_mention_member()`. `ui.c` tracks each mention's byte range as the draft is edited (`edit_mentions()`) and converts it to UTF-16 ranges when sending (`mention_ranges()`).
- **Reactions:** like, love, laugh, amazing, sad, surprised. Counts show under the bubble.
- **Unreadable messages:** a message sealed to a key this device lacks shows "[Letter Sealing key missing on this device]".

### LINE emoji and stickers

[`ui_emoji.c`](../../src/ui_emoji.c) is the picker (Ctrl+E or the smiley). It has an emoji tab, backed by the worker's `emojiCatalog` (the 45 public packs from `emoji_pack_index.inc` plus packs the account owns), and a stickers tab, backed by `stickerCatalog`. Chosen emoji are inserted into the draft as placeholder tokens, and `ui_emoji_export()` turns the draft into text plus `emojis` ranges when sending. In history, `ui_emoji_shape_history()` replaces the emoji ranges with inline pictures through Pango attributes. Artwork is requested through `emoji` and `sticker` requests, and animated PNGs play through `ui_apng.c`.

### Attachments, photos, video, screenshots

All in [`ui_media.c`](../../src/ui_media.c):

- **Choosing a file:** **+** or Ctrl+O opens the built-in file picker (`open_picker()`, `directory()`), or drop a file on the window. `prepare_attachment()` decides between photo, video, audio and file, and the preview sends `sendPhoto` or `sendFile`.
- **Pasting:** Ctrl+V in the composer calls `ui_media_paste()`. A picture on the clipboard (PNG, WebP or JPEG up to 32 MiB) goes through [`clipboard_paste.c`](../../src/clipboard_paste.c) into a private file under `$XDG_RUNTIME_DIR/avernella` and opens the Send photo preview. A copied file opens as an attachment. Picture paste needs SDL3's clipboard through sdl2-compat. With classic SDL2 only text pastes.
- **Copy image:** puts a PNG on the clipboard (`image_clipboard()`, again needing SDL3).
- **Photo viewer:** `picture()` requests the full image with `photo` and shows it. JPEG EXIF orientation is honored (`exif_orientation()`).
- **Thumbnails:** `ui_media_thumb()` with a bounded cache (`reserve_thumbnail()`, `evict_thumbnail()`).
- **Video:** `open_video()` asks for the file with `openVideo` and plays it with `ffplay` through [`video_play.c`](../../src/video_play.c). With the native renderer, FFplay uses the selected Vulkan GPU to display the clip. On compatible drivers it also decodes video with NVDEC on NVIDIA or Vulkan Video on AMD and Intel. Video cards show a poster from `videoPoster` when LINE has one.
- **Camera video messages:** **Record video** uses [`video_record.c`](../../src/video_record.c), which runs `ffmpeg` with V4L2 and PulseAudio to write an H.264/AAC MP4 into a private file. On NVIDIA, the recorder tests H.264 NVENC before opening the camera and falls back to the software encoder if startup fails. You review the clip and then send it.
- **Screenshots:** Ctrl+Alt+S or **Take screenshot** in the chat tools. [`screenshot.c`](../../src/screenshot.c) asks the XDG Screenshot portal, and the result opens as an attachment. The client closes Chat Tools and presents the uncovered chat before asking the portal.
- **Saving:** Save writes the downloaded bytes through `saveFile`, using `line_save_media()` in the worker, with an atomic rename.

### Flex Messages and rich messages

- [`ui_flex.c`](../../src/ui_flex.c) turns a Flex JSON (bubbles, carousels, boxes, text, images, buttons, separators) into a layout tree (`ui_flex_new()`), measures it to the chat width and paints it (`ui_flex_draw()`). `ui_flex_uri_at()` finds the action under the pointer.
- Rich messages are drawn by `rich_draw()` in `ui_draw.c` from the layout the worker built with `line_rich_parse()` ([`rich_message.c`](../../src/rich_message.c)). `ui_rich_area_at()` finds the tapped area, and `rich_action()` in `ui.c` opens a link, sends text, copies, or switches scene.
- Their pictures load through `flexImage` requests, which only the worker's image thread fetches.
- Links open through `ui_liff_open_link()`. See [LIFF](liff.md).

### Search

[`ui_find.c`](../../src/ui_find.c):

- **Ctrl+Shift+F** searches the open conversation (`ui_find_open()`, worker method `findMessages`) and can continue into older pages.
- **Ctrl+Alt+F** searches recent messages across chats (`ui_find_open_all()`, `findAllMessages`) in batches of up to six chats, using the latest 50 messages of each.

### Chat tools

[`ui_chat_tools.c`](../../src/ui_chat_tools.c), opened from the chat header's **⋯** or Ctrl+Shift+T: announcements (list, open, remove), shared content, screenshot. It also implements the **Forward** picker (`ui_chat_tools_forward()`) and the **Announce** confirmation (`ui_chat_tools_announce()`), which warns that announcements are stored outside Letter Sealing.

### Shared content

[`ui_shared_content.c`](../../src/ui_shared_content.c): albums and their photos, group notes (read and post text notes), and the polls notes refer to. Worker methods `sharedList`, `sharedPhotos`, `sharedPhoto` and `sharedPost`.

### Friends

[`ui_friends.c`](../../src/ui_friends.c): the friend directory with paging, **Add friend** (LINE ID, phone number or personal link), **Read QR image** (ZBar through [`qr_scan.c`](../../src/qr_scan.c)), **My QR code**, and **Manage**, which lists hidden, blocked and recommended accounts (`ui_friends_show_view()`). The **…** beside a friend sets favorite, display name, hidden or blocked.

### Groups

[`ui_groups.c`](../../src/ui_groups.c): create a group (name, pick friends, review), the member roster with invite, remove and revoke, group settings (rename, invitation link and QR, leave), and pending invitations to join or decline.

### Profiles, settings and the Inbox

[`ui_profile.c`](../../src/ui_profile.c):

- **Profile popout** (`ui_profile_open()`): opened from a sender's avatar or name, or a one-to-one chat header. It shows the picture on a banner tinted from the picture's colors, names, badges and status message, with Message, Add Friend and Block or Unblock.
- **User Settings** (`ui_settings_open()`, Ctrl+,): My Account, Profiles (avatar, name and status editors with a live preview and an unsaved-changes bar), Privacy & Safety, Notifications (desktop notifications and sounds), Keybinds, and Appearance (theme, reduced motion, language). Ctrl+↑ and Ctrl+↓ move between sections, and Escape closes.
- **Inbox** (`ui_inbox_open()`): incoming friend requests with accept and ignore, and recent adds.
- **Toast:** a card at the bottom right when someone adds you or sends a request (`ui_profile_toast_draw()`).
- **Live updates:** the `self`, `contact`, `friend_added` and `friend_request` events patch the open card, the chat row and the Inbox in place (`ui_profile_packet()`).

### Calls

The call stage, dock and controls are in [`ui_media.c`](../../src/ui_media.c):

- `dial()` starts a call (`callStart` with `video` false or true).
- `ui_media_call_event()` applies `call`, `call_incoming` and the other call events. An incoming call shows a card with accept (Alt+A) and decline (Alt+D).
- The **stage** sits at the top of the call's chat, and its height animates (`ui.stage_h`). Open another chat and the call shrinks to a **dock** at the bottom (`ui_media_draw_call_dock()`). Alt+C switches between them.
- Tiles show the peer's camera, your camera and the peer's screen, with a red **LIVE** badge for screen shares. Video arrives through `ui_media_preview_frame()` and is drawn by the presenter, or by `draw_call_frame()` on the SDL paths.
- Mute (Alt+M), camera (Alt+V), share screen (Alt+S), hang up. Double-click or **Full screen** for full-screen video, F11 for the focused tile, Escape to leave (`ui_media_draw_fullscreen()`).

### Read receipts

The UI decides when you have *seen* the latest message, and the worker tells LINE. `ui_conversation_presented()` in `ui.c` runs after a frame shows the conversation. If `ui_read_visible()` is true (connected, focused, visible window, latest page shown, scrolled to the bottom, nothing covering it) it sends `markRead` for the newest message.

`UiReadCursor` in `ui_private.h` keeps four IDs per chat: `viewed` (the newest shown here), `confirmed` (LINE acknowledged it), `attempted` and `pending`. They make sure that a slow acknowledgement can't clear a badge for a newer message, and that the same receipt isn't sent twice. `tests/ui_read.c` covers these rules.

## Desktop integration

### Tray

[`ui_tray.c`](../../src/ui_tray.c) exports a StatusNotifierItem and an Open, Hide and Quit menu over GDBus. Closing the window hides it in the tray or minimizes it when a tray icon is unavailable. Sync and notifications continue in the background. In Flatpak, the app asks the Background portal for permission before hiding. Use Ctrl+Q or Quit in the tray menu to exit.

### Notifications

[`ui_notifications.c`](../../src/ui_notifications.c) calls `org.freedesktop.Notifications.Notify` on a short-lived thread. Notifications show the chat name without a message preview, appear only while the window is in the background, and respect per-chat mutes and the Notifications switch. A body can carry someone else's name (a friend request), so it is escaped with `g_markup_escape_text()` when the notification server announces `body-markup`, or doesn't answer `GetCapabilities`.

### Sounds

[`chiptune.c`](../../src/chiptune.c) is an emulated NES audio chip (two pulse channels, a triangle and a noise channel, ported from the Mesen emulator). Each sound in `enum chip_sound` (ringtone, ringback, connected, hang-up, camera on and off, share start and stop, mute, unmute, message) is written as notes on the chip's registers and rendered once to 48 kHz PCM, in under 10 ms. [`ui_sounds.c`](../../src/ui_sounds.c) plays them through SDL audio: one looping sound (ringtone or ringback) and one effect at a time. The audio device is opened only while a sound plays, under the stream name "Avernella sounds". **User Settings → Notifications → Sounds** turns them off.

A sound thread opens and closes the playback device. If a call connects while the device is opening, the thread plays the current cue and discards the obsolete ringtone.

### Keyboard shortcuts

| Keys | Action | Handled in |
| ---- | ------ | ---------- |
| Enter | Send | `ui.c` |
| Tab, Shift+Tab | Move focus | `tab_focus()` |
| ↑, ↓ | Previous or next chat | `key()` |
| Page Up, Page Down | Scroll the conversation | `key()` |
| Ctrl+Page Up, Ctrl+End | Earlier messages, back to latest | `key()` |
| Ctrl+F | Find a chat by name (rebindable) | `ui_chat_lists.c` |
| Ctrl+Shift+F | Search this conversation (rebindable) | `ui_chat_lists.c`, `ui_find.c` |
| Ctrl+Alt+F | Search recent messages across chats | `ui_media.c` |
| Ctrl+R | Refresh (rebindable) | `ui_chat_lists.c` |
| Ctrl+Shift+L, Ctrl+Shift+P | Folders, pin (rebindable) | `ui_chat_lists.c` |
| Ctrl+E | Emoji and stickers | `ui_media.c` |
| Ctrl+O | Attach a file | `ui_media.c` |
| Ctrl+V | Paste text, a picture or a file | `ui.c`, `ui_media.c` |
| Enter / Shift+Enter | Send the message / insert a newline | `ui.c` |
| Ctrl+Q | Quit Avernella | `ui.c` |
| Ctrl+Shift+T | Chat tools | `ui_media.c` |
| Ctrl+Alt+S | Take a screenshot | `ui_media.c` |
| Ctrl+, | User Settings | `ui_profile.c` |
| Escape | Close the top overlay, cancel a reply, or go back in compact mode | several |
| Alt+A, Alt+D | Accept, decline an incoming call | `ui_media.c` |
| Alt+M, Alt+V, Alt+S | Mute, camera, share screen during a call | `ui_media.c` |
| Alt+C | Switch between the call and the open chat | `ui_media.c` |
| F11 | Full-screen call video | `ui_media.c` |

The five rebindable shortcuts have defaults in `chat_lists.c` (`defaults[]`) and can be changed under **User Settings → Keybinds**.

## Adding a screen or overlay

1. Create `src/ui_<thing>.c` and `.h` with the functions in the pattern table above. Keep state in a `static` struct.
2. Draw with the helpers in `ui_private.h` and the palette in `ui_theme.h`. Wrap visible strings in `_()`.
3. In `ui.c`'s `event()`, give your module its input while it is active, at the right place in the overlay order.
4. Call your draw function from `draw_frame()` in `ui_draw.c`, again in overlay order. While your overlay is open, scroll reuse must stay off: check `reusable_history()`.
5. Send worker requests with `ui_rpc(method, params, tag)`, and add your `_reply()` to the chain in `handle_reply()` in `ui.c`.
6. Call your `_reset()` at sign-out.
7. Add the object to `SOURCES` in the Makefile, and to the `render-bench` and UI test link lines that link `ui.o`.
8. Check both themes, compact width (≤ 640 px), HiDPI and reduced motion.
