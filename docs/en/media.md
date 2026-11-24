# Media, stickers, emoji and rich content

[Index](README.md) · Previous: [Letter Sealing](letter-sealing.md) · Next: [Friends, profiles and groups](social.md)

This page covers everything that isn't plain text: encrypted attachments, profile pictures, stickers, LINE emoji, official accounts' Flex and rich messages, and albums and notes.

## Encrypted attachments

Photos, videos, audio and files live on LINE's **object storage** (OBS) at `obs.line-apps.com`. The message itself carries only a reference to the stored object and, sealed with Letter Sealing, the key to decrypt it. The code is in [`media.c`](../../src/media.c) (the flow), [`media_crypto.c`](../../src/media_crypto.c) (the cipher) and [`obs_http.c`](../../src/obs_http.c) (the HTTP calls).

### Kinds

| Kind | Content type | OBS namespace (`SID`) | Uploaded name |
| ---- | ------------ | --------------------- | ------------- |
| Image | 1 | `emi` | `<uuid>.jpg`, `.png`, `.gif` or `.webp` |
| Video | 2 | `emv` | `<uuid>.mp4` |
| Audio | 3 | `ema` | `<uuid>.m4a` |
| File | 14 | `emf` | `<uuid>.bin` |

The `e` stands for "encrypted". Old, unencrypted media live in namespace `m` under the message ID.

Limits: 64 MiB per attachment (`LINE_MEDIA_LIMIT`), 2 MiB for a preview. Images must really be JPEG, PNG, GIF or WebP: `image_format()` checks the magic bytes.

### The cipher

`line_media_encrypt()` and `line_media_decrypt()`:

1. A random 32-byte **key material** is generated per attachment.
2. **HKDF-SHA256** with no salt and the info string `"FileEncryption"` expands it to 76 bytes: a 32-byte AES key, a 32-byte HMAC key and a 12-byte nonce. The 16-byte AES-CTR counter block is the nonce followed by four zero bytes.
3. The file is encrypted with **AES-256-CTR**.
4. A 32-byte **HMAC-SHA256** tag is appended:
   - for images, audio and files, over the whole ciphertext;
   - for videos, over the concatenated **SHA-256 digests of each 128 KiB chunk** of the ciphertext. The digest list is uploaded too, so a player can verify a video piece by piece.

Decryption checks the tag in constant time, trying both forms, before decrypting.

### Sending an attachment

`send_attachment_bytes()` does these steps, in this order:

1. **Validate** the chat (a user or group) and the kind, and read the file (`read_attachment()`).
2. **Make the key material**, and the sealed payload `{"keyMaterial": "<base64>", "fileName": "<name>"}`. The file name is included for files only.
3. **Seal the payload first** with `line_seal_payload(chat, content type, payload)`. This negotiates the recipient's key *before* any bytes go to OBS. If the contact can't receive Letter Sealing v2, nothing is uploaded.
4. **Encrypt** the file. Videos also produce the chunk-digest list.
5. **Get an OBS token**: `acquireEncryptedAccessToken` on `/S4` with `{2: 2}`. The reply is a string `"<lifetime seconds>\x1e<token>\x1e…"`. `line_obs_token()` caches it in `s->obs_token` until it expires. It is never saved to disk.
6. **Upload** the ciphertext: `POST https://obs.line-apps.com/r/talk/<sid>/reqid-<uuid>` with headers
   - `x-line-access: <OBS token>`
   - `x-line-application: DESKTOPWIN…`
   - `x-obs-params: <base64 of {"type":"FILE","ver":"2.0","name":"<uploaded name>","range":"bytes 0-<n-1>/<n>"}>`
   - `Content-Type: application/octet-stream`

   The response header `x-obs-oid` gives the real object ID (`OID`).
7. **Upload the side object**, if any, at `<OID>__ud-preview` (images) or `<OID>__ud-hash` (videos, the digest list). For images, the **preview is the same ciphertext as the original**. Encrypting a separate thumbnail under the same CTR key stream would leak information, so the client reuses the original.
8. **Send the message** with `line_send_sealed()`, content type 1, 2, 3 or 14, the five chunks from step 3, and metadata:

   | Key | Value |
   | --- | ----- |
   | `SID`, `OID` | Where the object is |
   | `FILE_SIZE` | Plaintext size in bytes |
   | `DURATION` | Video length in milliseconds, read from the MP4 header (`video_duration()`) |
   | `MEDIA_CONTENT_INFO` | Images: JSON `{"category":"original","fileSize":…,"extension":"…","width":…,"height":…,"animated":…}` |
   | `e2eeVersion` | `"2"` |

Uploading doesn't give anyone access. Recipients can find and decrypt the object only after `sendMessage` delivers its location and key.

### Downloading

`line_download_attachment()` (and `line_download_image()`, `line_download_video_preview()`):

1. Open the sealed payload with `line_open_payload()` to get `keyMaterial` (exactly 44 base64 characters) and, for files, `fileName`.
2. Check the metadata's `SID` matches the content type.
3. `GET https://obs.line-apps.com/r/talk/<sid>/<oid>` with the OBS token and an `x-talk-meta` header: the base64 of `{"message": "<base64 of a Thrift Binary Message struct {4: message ID, 27: empty list}>"}` (`download_header()`). It tells OBS which message the request is for.
4. Decrypt and verify (`line_media_decrypt()`).

Video posters come from `<sid>/<oid>/ud-preview` (falling back to `<oid>__ud-preview`), with a 6-second timeout. Legacy unencrypted media use `m/<message ID>` and `/preview`.

An image message from an official account often isn't stored in OBS at all. Its metadata carries `DOWNLOAD_URL` (the picture) and `PREVIEW_URL` (a smaller copy), and asking OBS for `m/<message ID>` returns 404. LINE's phone app loads the thumbnail from `PREVIEW_URL` and the picture from `DOWNLOAD_URL` when they are set. `line_hosted_image_url()` picks the first of the two that passes the [arbitrary-host policy](#pictures-from-arbitrary-hosts), and the worker fetches it with `line_flex_image_download()` for both the thumbnail and **Save**.

If OBS rejects the token (`LINE_OBS_UNAUTHORIZED`), the worker forgets it and tries once more with a new one, as LINE's own OBS client does. A 429 or 503 starts that host's cooldown, and requests to it fail locally until it passes (see [Protocol basics](protocol-basics.md#rate-limits)).

### Getting bytes to the window

Downloaded bytes reach the UI as a [media lease](architecture.md#large-data-media-leases): a sealed memfd the UI opens through `/proc/<worker>/fd/<n>`. The worker methods are:

| Method | Returns |
| ------ | ------- |
| `photo` | A full image (or a sticker picture, for sticker messages) |
| `videoPoster` | A video's preview picture |
| `openVideo` | The whole video, for `ffplay` |
| `saveFile` | Writes the attachment to a path the user chose, atomically (`line_save_media()`) |

### Forwarding

`line_forward_attachment()` downloads and decrypts the original, then sends it as a new attachment with a **fresh key** for the destination chat. Keys are never reused between chats.

## Profile pictures

Avatars are public. A contact's profile carries a picture path such as `/0h…`. `line_avatar_download()` fetches `https://profile.line-scdn.net<path>` anonymously (at most 2 MiB) on the worker's image thread, and the UI requests it with `avatar`.

Changing your own picture (`updateMyPicture`, [`profile_image.c`](../../src/profile_image.c)):

1. `line_profile_jpeg()` decodes JPEG, PNG or WebP (up to 20 MiB). JPEG files are reduced in libjpeg-turbo's decoder first, for speed.
2. It crops the centered square and scales it to 640 × 640 by area averaging. Transparent pixels become white.
3. It encodes a baseline JPEG.
4. `line_obs_upload_profile()` posts it to `https://obs.line-apps.com/r/talk/p/<your MID>` with `x-obs-params` `{"name":"profile.jpg","quality":"100","type":"image","ver":"2.0"}`. OBS updates the profile itself; there is no separate RPC.

## Stickers

[`stickers.c`](../../src/stickers.c). Stickers are shop products. You can send only stickers your account owns.

**The catalog** (`stickerCatalog`, `line_sticker_catalog()`): `getOwnedProductSummaries` on `/TSHOP4`, paged by offset, with the shop type and a locale built from your language and region (`shop_request()`). Each summary gives the package ID, version, name, kind and the ranges of sticker IDs. The catalog is cached in memory for a while and cleared if a send fails with code 32 (USER_NOT_STICKER_OWNER). ShopException codes are a separate namespace, so `s->service_code` is reset afterwards.

**Kinds and artwork:**

| Kind | `STKOPT` | Picture file |
| ---- | -------- | ------------ |
| STATIC | — | `sticker.png` |
| ANIMATION | `A` | `sticker_animation.png` (animated PNG) |
| SOUND | `S` | `sticker.png` |
| ANIMATION_SOUND | `AS` | `sticker_animation.png` |
| POPUP | `P` | `sticker_popup.png` |
| POPUP_SOUND | `PS` | `sticker_popup.png` |

Artwork URLs are `https://stickershop.line-scdn.net/stickershop/v1/sticker/<id>/android/<file>`, or `…/v2/sticker/<id>/<hash>/android/<file>` when the pack has a hash. `line_sticker_fetch()` accepts only URLs under that origin. Sound stickers show their picture; the sound isn't played.

**Sending** (`sendSticker`, `line_send_sticker()`): `sendMessage` with content type 7, **no encryption** and metadata `STKID`, `STKPKGID`, `STKVER`, `STKTXT` (the pack name), `STKOPT` and `STKHASH` when they apply. A reply adds the usual fields 21, 22 and 24.

**Receiving:** `line_sticker_message_asset()` builds the artwork URL from a received message's metadata.

## LINE emoji

[`emoji.c`](../../src/emoji.c). LINE emoji (*sticons*) are small pictures inside text, identified by a **product ID** (24 lowercase hex digits) and an **item ID** (three decimal digits, `001` and up). `line_emoji_id_valid()` checks both forms before they go anywhere near a URL.

**The catalog** (`emojiCatalog`, `line_emoji_catalog_page()`) merges two sources:

1. **The offline index** `emoji_pack_index.inc`: the 45 public packs from LINE's published emoji list, with product IDs, names, versions and item counts (9,175 items in total).
2. **Owned packs**: `getOwnedProductSummaries` on `/TSHOP4` with shop type `"sticonshop"`, 100 per page, stopping on an empty or repeated page.

For a pack's real item list and labels, it fetches the pack manifest at `https://stickershop.line-scdn.net/sticonshop/v1/product/<product>/android/meta.json`. Pages of the catalog are returned to the picker with `{productId, emojiId, label, pack, version, resourceType}` entries, and search matches pack names, item numbers and loaded labels.

**Pictures:** `https://stickershop.line-scdn.net/sticonshop/v1/sticon/<product>/android/<item>.png` for the still image, and `<item>_animation.png` for the animated PNG. When the animation returns 404, the still is used. The UI asks with `emoji` (with `resourceType: "ANIMATION"` for animated ones).

**In messages:** see [Messages: LINE emoji in text](messaging.md#line-emoji-in-text).

## Flex Messages

Official accounts send cards described in JSON: bubbles and carousels made of boxes, text, images, icons, buttons, separators and spacers, each with optional actions. See [LINE's Flex Message reference](https://developers.line.biz/en/docs/messaging-api/flex-message-elements/).

- **Reading:** content type 22 with the card in `FLEX_JSON` (at most 64 KiB, limited nesting) and `ALT_TEXT`. The worker's `add_flex()` in `messaging.c` validates it and passes it to the UI.
- **Drawing:** [`ui_flex.c`](../../src/ui_flex.c) builds a layout tree, measures it to the chat width (carousels wrap) and paints it. If a card can't be drawn, the alternative text is shown.
- **Actions:** `line_flex_link_allowed()` in [`flex_asset.c`](../../src/flex_asset.c) accepts HTTPS links, LIFF links and `line://` links. They open through [the in-app browser](liff.md).

## Rich messages

Rich messages are LINE's **imagemap**: one picture with tappable areas, possibly with several scenes. They are content type 17. [`rich_message.c`](../../src/rich_message.c) reads the same markup LINE for Android reads.

Metadata keys (or the same with a `RICH_` prefix): `DOWNLOAD_URL` (the base picture), `ALT_TEXT`, and `MARKUP_JSON`:

```
canvas  {width, height, initialScene}
images  {name: {x, y, w, h, imageUrl?}}      regions of the base picture
actions {name: {type, text, params}}         web, postback, sendMessage, clipboard, transition, app
scenes  {name: {draws: [{x, y, w, h, image}],
                listeners: [{type: "touch", params: [x, y, w, h], action}],
                video}}
```

`line_rich_parse()` validates all of it and returns a compact layout for the UI:

```
{w, h, scene, scenes: {name: {draws: [{x, y, w, h, url, sx, sy, sw, sh, whole}],
                              areas: [{x, y, w, h, kind, value, label}]}}}
```

where an area's `kind` is `link` (an HTTPS URL), `message` (text to send as you), `copy` (text to copy) or `scene` (switch to another scene, locally). The base picture is requested at `DOWNLOAD_URL/700` (`LINE_RICH_IMAGE_WIDTH`), LINE's imagemap convention, and the downloader falls back to the plain `DOWNLOAD_URL` when the object store rejects the width. Anything malformed shows the alternative text instead.

## Pictures from arbitrary hosts

Flex and rich-message artwork can come from **any HTTPS host** the sender chooses, as on the phone. That means the sender's server sees your IP address. `line_flex_image_download()` in [`flex_asset.c`](../../src/flex_asset.c) keeps the risk small:

- HTTPS only. Requests carry no cookies or credentials.
- A custom socket callback (`asset_socket()`) checks the resolved address and refuses private, loopback, link-local and other non-public IPv4 and IPv6 addresses, so a message can't make the client probe your local network.
- Redirects are followed only to other allowed HTTPS URLs.
- Only JPEG, PNG or WebP is accepted, up to 64 MiB, a limit that only guards against a server that never stops sending.
- A host that answers 429 or 503 gets no further request until its `Retry-After` passes, and the imagemap plain-URL fallback is skipped.
- `LINE_DEBUG_MEDIA=1` reports the failure stage, attempt number, curl code, HTTP status and bytes received. Image addresses and server-supplied text stay out of these logs.

## Albums, notes and polls

[`shared_content.c`](../../src/shared_content.c). These aren't Thrift services but REST endpoints on `legy.line-apps.com`, authorized by a **channel token**:

1. `approveChannelAndIssueChannelToken` on `/CH4` with `{1: channel ID}`, where the channel is `1375220249` for albums and `1341209850` for notes. Reply field 5 is the token, cached per session.
2. Requests carry `x-line-access`, `x-line-channeltoken`, `x-line-mid` (your MID, or the chat for album requests), `x-line-chat-id` or `x-line-album`, and `x-lhm` with the logical HTTP method.

| Worker method | Endpoint |
| ------------- | -------- |
| `sharedList` (albums) | `GET /ext/album/moa/v2/albums?cursor=…&orderBy=createTimeDesc` |
| `sharedPhotos` | `GET /ext/album/api/v6/albums/<album>/photos?cursor=…&pageSize=50&orderBy=createTimeDesc&include=all` |
| `sharedPhoto` | `GET /oa/r/album/a/<photo>`, a picture returned as a media lease |
| `sharedList` (notes) | `GET /ext/note/nt/api/v57/post/list.json?homeId=<chat>&sourceType=TALKROOM` |
| `sharedPost` | `POST /ext/note/nt/api/v57/post/create.json?homeId=<chat>&sourceType=TALKROOM` |
| poll search | `/mh/api/v46/search/note.json` |

The client can browse albums and open photos, read and post text notes, and list the polls notes refer to. Managing albums, editing notes and voting aren't implemented yet.
