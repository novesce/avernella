# How LINE works

This page describes LINE's personal-account service as this client uses it. It comes from LINE for Android 26.16.0, the public LIFF SDK, LINE's encryption whitepaper and the open interoperability projects credited in the [README](../README.md). LINE doesn't document this protocol and can change it. The client speaks as LINE's Windows desktop app.

## Accounts, chats and IDs

Every user, group and room has a MID, a 33-character ID whose first letter gives its kind:

| Prefix | Kind |
| ------ | ---- |
| `u` | A user, including official accounts |
| `c` | A group (chat type GROUP, 0) |
| `r` | A multi-person room (chat type ROOM, 1) |

A phone is the account's primary device. Desktop apps are linked secondary devices: they sign in with the phone's approval or with email and password, and LINE may end a desktop session when another one signs in.

## Servers

| Host | Used for |
| ---- | -------- |
| `legy.line-apps.com` | All RPCs: sign-in, messages, sync, contacts, groups, calls, LIFF |
| `obs.line-apps.com` | Object storage: attachments, previews, profile pictures |
| `profile.line-scdn.net` | Profile pictures |
| `stickershop.line-scdn.net` | Sticker and emoji artwork and pack manifests |
| Any HTTPS host | Flex and rich-message artwork chosen by the sender |

## RPC: Compact Thrift over HTTPS

LINE's RPCs are Apache Thrift calls in the Compact protocol, POSTed over HTTP/2 to a path on `legy.line-apps.com`. Each request carries these headers:

```
x-line-application: DESKTOPWIN	9.7.0.3556	WINDOWS	10.0.0-NT-x64
x-line-access: <access token>
x-lal: en_US
x-lpv: 1
User-Agent: Line/9.7.0.3556
```

A reply is a Thrift result struct. Field 0 holds the result. Field 1 holds a service exception, a struct whose field 1 is an error code and field 2 a message. Some services add a payload in field 3. The client's encoder ([`thrift.c`](../src/thrift.c)) takes JSON with typed keys (`"1:str"`, `"2:i32"`, `"3:obj"`, `"4:list:str"`) and decodes replies to JSON keyed by field number. Every field ID below comes from the static `TField` tables in LINE's Android code.

| Path | Service | Methods this client calls |
| ---- | ------- | ------------------------- |
| `/S4` | TalkService | `getProfile`, `getChats`, `getAllChatMids`, `getMessageBoxes`, `getRecentMessagesV2`, `getPreviousMessagesV2WithRequest`, `sendMessage`, `sendChatChecked`, `react`, `createChat`, `updateChat`, `inviteIntoChat`, `cancelChatInvitation`, `acceptChatInvitation`, `rejectChatInvitation`, `deleteOtherFromChat`, `deleteSelfFromChat`, `reissueChatTicket`, `getContactsV2`, `getFriendRequests`, `removeFriendRequest`, `updateProfileAttributes`, `getUserTicket`, `negotiateE2EEPublicKey`, `getLastE2EEGroupSharedKey`, `getChatRoomAnnouncements`, `createChatRoomAnnouncement`, `removeChatRoomAnnouncement`, `acquireEncryptedAccessToken`, `getLastOpRevision` |
| `/RE4` | Relation service | `getContactsV3`, `addFriendByMid`, `updateFriendFavorite`, `updateFriendHidden`, `updateUserFriendOverridenName`, `blockRelation`, `unblockRelation` |
| `/SYNC4` | Sync service | `sync` (long poll) |
| `/AS4` | Auth service | `logoutV2` |
| `/acct/lgn/sq/v1`, `/acct/lp/lgn/sq/v1` | Secondary login (QR) | `createSession`, `createQrCodeForSecure`, `createPinCode`, `qrCodeLoginV2ForSecure`, `verifyCertificate` |
| `/api/v3/TalkService.do`, `/api/v3p/rs` | Email login | `getRSAKeyInfo`, `loginV2`, `confirmE2EELogin` |
| `/LF1` | Login polling | Waiting for phone verification |
| `/EXT/auth/tokenrefresh/v1` | Token refresh | `refresh` |
| `/TSHOP4` | Shop | `getOwnedProductSummaries` (owned stickers and emoji) |
| `/CH4` | Channel service | `approveChannelAndIssueChannelToken` (albums and notes) |
| `/V4` | Call service | `acquireCallRoute` |
| `/LIFF1` | LiffService | `issueLiffView` |

## Signing in

### QR code ([`pair.c`](../src/pair.c))

1. **Create a session.** `createSession` returns a session ID, and `createQrCodeForSecure` returns a `line://` login URL.
2. **Add a key to the QR code.** The client creates an X25519 key pair and adds its public key to that URL as `secret=<base64>&e2eeVersion=1`. The QR code you scan is this URL.
3. **Wait for the phone.** The client long-polls on `/acct/lp/lgn/sq/v1` until the phone scans it. LINE may then have the desktop show a PIN (`createPinCode`) for you to enter on the phone.
4. **Receive the tokens and keys.** `qrCodeLoginV2ForSecure` returns the access token, refresh token and MID, plus the account's Letter Sealing key chain. The phone encrypts that chain to the desktop's X25519 public key. The client decrypts it and checks its hash, so the device can read past encrypted messages.

### Email and password ([`password_login.c`](../src/password_login.c))

1. **Encrypt the credentials.** `getRSAKeyInfo` returns an RSA key, and the client encrypts the email and password with it.
2. **Log in.** `loginV2` returns either a session or a verifier. With a verifier, the phone must approve the sign-in or enter a code shown on the desktop; the client waits on `/LF1`.
3. **Receive the keys.** `confirmE2EELogin` delivers the Letter Sealing keys the same way as the QR flow.

### Staying signed in ([`transport.c`](../src/transport.c))

LINE issues the access token with `durationUntilRefreshInSec` and `tokenIssueTimeEpochSec`. The worker renews it on that schedule with `/EXT/auth/tokenrefresh/v1`, and on demand when a call returns an expired-token error. A reply may carry `x-line-next-access`, a rotated token, which the client adopts. Only an explicit rejection of the refresh token signs the device out. Network failures retry with backoff.

## Sync: how messages arrive

LINE describes every change to your account as an **operation** with a revision number: a received message, a sent message from another device, a read receipt, a profile change, a group change, an incoming call. The client keeps a cursor ([`talk_sync.c`](../src/talk_sync.c)) and calls `sync` on `/SYNC4` with it. LINE holds the request open until there is something new, then returns the operations and the next cursor. LINE may instead answer with a full or partial resync, and the client then reloads the affected chats. If the long poll fails, `getLastOpRevision` on `/S4` serves as a cheap fallback check.

## Messages

`sendMessage` takes a sequence number and a Message struct containing the recipient MID, content type, text or metadata (`contentMetadata`, a string map) and, for encrypted chats, a chunked Letter Sealing payload. Content types the client handles include text, image, video, audio, file, sticker, location, contact, Flex, rich message (imagemap) and call records. `react` adds a reaction.

The sequence number must be unique per device, so the client reserves it on disk before each send.

**Loading history.** A linked desktop can receive an empty page from `getRecentMessagesV2` even when its chat list shows a last message. If the chat summary has a valid message ID and delivery time beyond the user's cleared history, Avernella uses them to request a page with `getPreviousMessagesV2WithRequest`. This recovery page includes the known last message. Ordinary backward paging excludes its boundary message so it appears only once. [History retrieval](../src/messaging.c)

**Read receipts.** The window clears the local unread badge once it has displayed the newest message at the bottom of the focused, unobscured conversation. The worker sends `sendChatChecked` with that message ID when a receipt is due. A later chat-list refresh can arrive before LINE acknowledges the receipt. The client uses the ID it already displayed to keep a stale count from restoring the badge. [Read tracking](../src/ui_private.h) · [Chat-list updates](../src/ui.c)

## Letter Sealing: end-to-end encryption

LINE encrypts one-to-one and group messages end to end ([`sealing.c`](../src/sealing.c)). Each account has key generations of X25519 key pairs, and each device that signs in receives the private keys in a key chain encrypted to it.

**One-to-one chats.**
- **Exchange.** The client gets the peer's current public key with `negotiateE2EEPublicKey`, and both sides compute the X25519 shared secret.
- **Version 1.** The AES-256 key is SHA-256 of the secret, a salt and `"Key"`. The IV is SHA-256 of the same with `"IV"`, folded to 16 bytes by XORing its halves. The text is encrypted with AES-256-CBC. The MAC is AES-256-ECB, under the same key, of the folded SHA-256 of the ciphertext.
- **Version 2.** The key is SHA-256 of the secret, a random 16-byte salt and `"Key"`. The text is encrypted with AES-256-GCM under a random 12-byte nonce. The additional data binds the recipient MID, sender MID, both key IDs, the version and the content type. The client sends version 2 and reads both versions.

**Groups.** The group has a shared key generation (`getLastE2EEGroupSharedKey`), encrypted to each member's public key. Messages are sealed with it the same way.

**Payload.** The sealed message travels in `chunks`: salt, ciphertext, MAC or nonce, and the key IDs. Metadata such as content type stays outside the encryption.

A message encrypted to a key generation this device doesn't have can't be opened. The client shows a missing-key notice instead. Announcements are stored on LINE's servers outside Letter Sealing.

## Media: object storage

Attachments live on `obs.line-apps.com` ([`media.c`](../src/media.c), [`obs_http.c`](../src/obs_http.c), [`media_crypto.c`](../src/media_crypto.c)).

- **Uploading.** In encrypted chats, the client derives an AES-256-CTR key and an HMAC-SHA256 key from a random secret with HKDF-SHA256. It encrypts the file and uploads it with an OBS token from `acquireEncryptedAccessToken`. Images get an encrypted preview, and videos a hash object with 128 KiB chunk digests. The message then announces the object ID and the sealed secret.
- **Downloading.** The client fetches the object, checks the HMAC and decrypts it. Attachments are capped at 64 MiB.
- **Profile pictures** are cropped, scaled to 640 × 640 and uploaded to the profile object store, as the phone app does.

## Stickers and emoji

Stickers and LINE emoji (sticons) are shop products. `getOwnedProductSummaries` on `/TSHOP4` lists what the account owns. Each pack's manifest and artwork come from `stickershop.line-scdn.net`, as still PNG or animated PNG (`_animation.png`). A sticker message names its package and sticker ID. An emoji inside text is an `sticon` reference carrying the product ID, item ID and alternative text, kept inside the sealed message.

## Official-account messages

Official accounts send messages that personal accounts can't:

- **Flex Messages** describe cards in JSON: bubbles and carousels of boxes, text, images, buttons and separators, each with optional actions. The client lays them out natively ([`ui_flex.c`](../src/ui_flex.c)).
- **Rich messages** (LINE's imagemap) are an image with tappable areas. The markup in `MARKUP_JSON` gives the canvas, images, scenes and actions: open a link, send text, copy text or switch scenes ([`rich_message.c`](../src/rich_message.c)).

Artwork for both can come from any HTTPS host, as on the phone. The client fetches it without cookies and refuses private network addresses ([`flex_asset.c`](../src/flex_asset.c)).

## LINE apps (LIFF)

A LIFF app is a web page that runs inside LINE with the user's LINE identity: coupons, member cards, reservations and mini apps. Full field tables are in [analysis/liff.md](../analysis/liff.md).

1. **Recognize the link.** A link names a LIFF ID, a channel ID and suffix such as `1661444788-rnvmoOo8`: `https://liff.line.me/<id>/…` or `line://app/<id>/…`.
2. **Ask LINE for a view.** The client calls `issueLiffView` on `/LIFF1` with the LIFF ID, the chat it was opened from as context, and the language.
3. **Read the response.** It gives the app's endpoint URL and size (`FULL`, `TALL` or `COMPACT`), plus a context token, access token, feature token, ID token and channel ID.
4. **Load the page.** The client loads the endpoint with the link's extra path and query appended. The tokens go in the URL fragment, `#context_token=…&feature_token=…&access_token=…&id_token=…&client_id=…`, where the LIFF SDK's `liff.init()` reads and then removes them.
5. **Be recognizably LINE.**
   - **Client check.** `liff.isInClient()` is true when the user agent matches `Line/x.y.z LIFF`. Pages that say "this link only works in LINE" test this.
   - **Native bridge.** On LINE 9.5 and newer, `liff.init()` waits for `window._liff.features`. The page reaches client features through `window._liff.postMessage(action, featureToken, callbackId, json)` and waits for a `liffEvent` with the answer.
   - **Closing.** `liff.closeWindow()` navigates to `liff://close`.
6. **Handle exceptions.** If LINE needs the user's consent first, it throws a LiffException with code 3 and a consent page URL, which the client opens in the same window. Code 7 carries a fallback page.

## Calls

Voice and video calls ([`call.c`](../src/call.c) and the `call_*.c` files; details in [analysis/video-call-wire.md](../analysis/video-call-wire.md)):

1. **Get a route.** `acquireCallRoute` on `/V4` returns a call token, the relay host and port, zone and session IDs, and a configuration with the media key material.
2. **Negotiate.** The two sides negotiate over LINE's PLANET signaling, protobuf messages that the client encodes in [`call_schema.c`](../src/call_schema.c) and frames in [`call_wire.c`](../src/call_wire.c).
3. **Send media.** Audio is Opus and video is VP8 in LINE's EVS3 packet format. Both travel as SRTP over UDP through the relay, with separate keys for audio and video.
4. **Share the screen and stream data.** Screen sharing and data streams use PDTP ([`call_pdtp.c`](../src/call_pdtp.c)), a reliable stream protocol inside the call, matching LINE's `libandromeda.so`.

Incoming calls arrive as sync operations and ring in the window.
