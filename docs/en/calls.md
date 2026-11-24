# Calls

[Index](README.md) · Previous: [LIFF](liff.md) · Next: [Testing](testing.md)

Avernella makes and answers **one-to-one** voice and video calls, with screen sharing. Calls use Thrift RPC, encrypted protobuf signalling, and SRTP media. The byte-level evidence with addresses in LINE's `libandromeda.so` is in [analysis/video-call-wire.md](../../analysis/video-call-wire.md).

Video is on by default and can be turned off with `LINE_EXPERIMENTAL_VIDEO=0` (`line_video_enabled()`).

## Group calls

Click the handset for voice or the camera for video in a group chat. Group calls are enabled by default. Set `LINE_EXPERIMENTAL_GROUP_CALLS=0` to disable both buttons or `LINE_EXPERIMENTAL_GROUP_VIDEO=0` to disable group video. The worker checks the current call with `getGroupCall`, requests a route, and waits for the conference relay's final `PARTICIPATE_RSP` before opening the call devices. It registers the assigned streams, derives the SRTP keys from the conference media secrets, and sends EAS2 audio in 40 ms packets. On hangup the worker closes the microphone and camera, then waits up to two seconds for the relay to confirm the release. Quitting the desktop app also gives the worker time to leave the call. If LINE reports that this account is already in a call on another device, leave the call on that device before joining from the laptop. Other participants can stay in the call.

The call stage shows the people returned by the call service and refreshes the roster while connected. Group microphone capture selects a stable input channel on stereo devices and lifts quiet input before noise suppression. The group audio receiver unwraps LINE’s XRTP mixer packets, decodes each speaker with a separate Opus decoder, and mixes the results for playback. It buffers 100 ms at startup and uses RTP timestamps to avoid replaying speech. The worker acknowledges relay media-channel and audio stream requests with the negotiated stream IDs. Authenticated mixer audio and camera packets can arrive from a separate UDP port. The microphone reports silence, unvoiced sound or voiced sound for each 20 ms frame and sends the louder frame’s level for each pair to the assigned conference relay. Avernella estimates voicing from the captured audio.

In a group video call, Avernella sends the local camera and decodes one remote camera at a time. Select a participant in the roster to focus their camera. The media worker only subscribes to camera sources named in authenticated main-conference membership packets. The camera uses VP8 over EVS3. When the relay requests a smaller supported camera layer, the worker scales the outgoing picture before encoding and sends a keyframe at the new dimensions. The local preview keeps its capture resolution. Avernella offers one software VP8 video layer for the group and normal-video as a fallback. The sender uses the payload map returned in the answer. The receiver also handles layered streams forwarded by the relay. The first registration reports the local camera as active only if capture opened successfully. When the relay acknowledges the outgoing camera source, Avernella sends a fresh keyframe. If the relay selects an unsupported codec, the call continues by voice.

Live group calls with the official Android app confirmed voice in both directions and Android camera video on Linux. In a follow-up call, the relay accepted the laptop camera, requested a smaller video layer and acknowledged over 13,800 camera packets. Android still showed the participant’s avatar. The sender now uses the main conference video transport, identifies the requested layer on every packet, and adjusts its encoding rate to that layer. In an offline replay, LINE’s Android video filter rejected all 190 packets from the previous build and accepted all 380 corrected test packets. Delivery of the updated stream to a live Android viewer still needs confirmation.

The group sender marks the base camera layer in each picture and includes the capture source and encode time in the first fragment. Changing between webcam and screen updates the source generation. Normal-video uses a three-byte codec header. For a layered stream, a six-byte profile precedes the codec header. VP8 uses a three-byte length and VP8A a four-byte length. Camera START requests trigger a keyframe and preserve this framing. Tests compare outgoing descriptors with the Android packetizer.

The default group transport derives outgoing stream keys from the local offer and incoming keys from the relay answer. The group microphone starts its RTP clock at the first 20 ms frame and advances it by 40 ms per packet. Avernella acknowledges authenticated control packets on the negotiated data channel and applies supported camera size requests from the relay. When LINE negotiates congestion feedback, Avernella reports receipt of authenticated media packets. Set `LINE_EXPERIMENTAL_GROUP_UPLINK=0` to disable these reports for troubleshooting. For diagnostics, `LINE_GROUP_CALL_STATS=1` prints aggregate stream counts, audio timing and decoder results to standard error. [LINE.js group call reference](https://github.com/evex-dev/linejs/blob/v3.6.5/docs/docs/call.md#group-calls)

## The files

| File | Job |
| ---- | --- |
| [`call.c`](../../src/call.c) | The call engine: call routes, the `line-voice` thread, signalling state, incoming invitations, camera and share control, frames to the UI |
| [`call_schema.c`](../../src/call_schema.c) | Builds and parses the PLANET protobuf messages: setup, verify, offers, envelopes, stream reports |
| [`call_wire.c`](../../src/call_wire.c) | A tiny protobuf encoder and decoder, PLANET packet framing and encryption, and media key derivation |
| [`call_srtp.c`](../../src/call_srtp.c) | libsrtp2 contexts (AES-CM-128 with HMAC-SHA1-80) |
| [`call_audio.c`](../../src/call_audio.c) | Microphone and speaker through SDL audio, Opus, the EAS2 frame format, jitter and playout |
| [`call_denoise.c`](../../src/call_denoise.c) | RNNoise noise suppression |
| [`call_video.c`](../../src/call_video.c) | Hardware and software VP8, EVS3 packetization, keyframe requests, screen-share coding |
| [`call_pdtp.c`](../../src/call_pdtp.c) | PDTP, LINE's reliable stream transport, used for screen sharing |
| [`camera.c`](../../src/camera.c), [`camera_pixels.c`](../../src/camera_pixels.c) | V4L2 camera capture (YUYV or MJPEG) and conversion to I420 |
| [`screencast.c`](../../src/screencast.c) | Screen capture through the XDG ScreenCast portal and PipeWire |
| [`ui_media.c`](../../src/ui_media.c) | The call stage, dock, controls and video tiles |

## Threads and safety

The worker's main thread handles call requests (`callStart`, `callAccept`, `callMute` and so on). The call itself runs on one `line-voice` thread (`voice_thread()` → `run_call()`), at most one call at a time. That thread receives **only call-local data**: the route, keys it generates, and the two MIDs. It never sees the account's access token. The call token from the route is wiped as soon as the first signalling message is built.

The main thread and the call thread share `struct call_task` (atomics for done, muted, camera and share wishes, and a mutex for the latest frames). The main thread wakes the call thread through an eventfd (`signal_task()`).

## 1. Getting a call route

**Outgoing** (`callStart`, `line_call_start()`): `acquireCallRoute` on `/V4` with `{2: peer MID, 3: 1 (audio) or 2 (video), 4: {"devname": "Avernella"}}`. The route reply:

| Field | Meaning |
| ----- | ------- |
| 1 | Call token |
| 3 | Relay host(s), comma-separated |
| 4 | Relay port |
| 6, 7 | Caller and callee zones |
| 8 | "From zone" flag. If true, the route is unusable. |
| 10 | The peer the route is for (must match) |
| 12 | A JSON *communication config*. Its `mpkey` is the relay's P-256 public key, 33 bytes compressed, base64. |
| 13 | Session type ID (`stid`) |
| 17 | IPv6 relay hosts |

**Incoming** calls carry the same data inside a sync operation (see [below](#incoming-calls)).

`setup_route()` resolves the relay (trying up to eight hosts, IPv4 or IPv6), opens a non-blocking UDP socket, and generates fresh random IDs: the media ID, a nonce, the local channel and media IDs, a transaction counter, a 16-byte media nonce, a 30-byte media secret and a 32-byte device ID. IDs are forced into the range 0x20000000 to 0x3fffffff.

## 2. PLANET packets and their encryption

All signalling travels as encrypted packets over UDP to the relay. `call_wire.c` implements the format, which follows LINEJS v3.6.5.

**Key agreement with the relay.** `call_wire_init()`:

1. Generates an ephemeral **P-256** key pair (`call_keygen()`, compressed 33-byte public key).
2. `secret = ECDH(our private key, relay's mpkey)`.
3. Derives two 64-byte base keys with an HKDF-like construction over HMAC-SHA512 (`kdf()`): `tx_base = kdf(secret, salt = relay key, info = our key)` and `rx_base = kdf(secret, salt = our key, info = relay key)`.
4. Makes an 18-byte random **prefix**. The first 2 bytes are used as `info` and the next 16 as the salt for a second stage: `tx = kdf(tx_base, prefix[2..18], prefix[0..2])`.

**A packet** (`call_wire_pack()`):

| Bytes | Contents |
| ----- | -------- |
| 0–1 | An encoded length and flags word |
| 2–3 | Sequence number |
| 4–5 | Flags and the plaintext length (normal packets) |
| 4–61 | For the first (*bootstrap*) packets: `06 02`, the 18-byte prefix, our 33-byte public key, padding, and the length |
| then | The protobuf payload, encrypted with **AES-128-CTR**: key `tx[0..16]`, IV `tx[16..32]` XOR the sequence bytes |
| last 16 | Truncated **HMAC-SHA256** of everything before it, with key `tx[32..64]` |

The relay answers with its own bootstrap packet. `call_wire_open()` derives the receive key from `rx_base` and that packet's prefix, checks the MAC, and from then on uses it for every packet.

Before signalling, the client sends **pinhole** packets (`pinholes()`): 16 small padded unencrypted probes and a final one, to open the path through NAT.

## 3. Signalling

Signalling messages are protobuf. `call_wire.c` has a minimal encoder (`pb_uint()`, `pb_bytes()`, `pb_text()`, `pb_msg()`, fixed64) and a parser (`pb_next()`, `pb_data()`, `pb_number()`). There are no `.proto` files: field numbers are written in the code, matching the descriptors in LINE's library.

### The envelope

Every message is wrapped by `call_envelope()`:

- **Header** (field 1): our MID, a message ID (such as `0x2141` for setup), the 16-byte session ID once known, a 16-byte transaction ID, a sequence number, our nonce, and the peer's nonce once known.
- **Plane**: 2 for messages to the relay itself, **3** for call control between the two users, **4** for media control. Planes 3 and 4 add a channel header: the call ID, our channel (or media) ID and the peer's.
- **Body**: one field of a `oneof`, chosen by the caller.

`handle()` in `call.c` checks every incoming message against the call ID, session, nonces, channel IDs and, for an incoming call, the expected transaction, and silently drops anything that doesn't match.

### The call sequence

**Outgoing call:**

1. `run_call()` builds `call_setup()`: our MID, the peer's, both zones, a user agent `{"Linux", "1", "Avernella"}`, the device ID, capabilities `1 2 3 6 7`, the **offer**, a credential, `freecall.audio` or `freecall.video`, the session type ID and three feature entries. It is sent on plane 3 as message `0x2141`.
2. The **credential** is `SHA-256(caller ‖ "::" ‖ callee ‖ "::" ‖ call token ‖ "::" ‖ call ID)` (`call_credential()`). It proves to the relay that we hold the route's token without sending the token itself.
3. The peer rings. The client plays the ringback sound and waits up to the call's deadline ("The call was not answered").
4. The peer's **answer** arrives. `setup_media()` reads the answer's media description, derives the SRTP keys, and starts audio (and video, if negotiated).

**Incoming call:**

1. The user accepts. `respond()` builds the route from the invitation and `run_call()` sends `call_verify()` (the same identity fields with the credential computed in callee order) as message `0x2142`.
2. The caller's offer arrives as message `0x2242`. The client answers with its own offer (`call_answer_offer()`) and the media starts.
3. Declining (`callDecline`) runs the same handshake with `decline` set, to tell the caller properly.

Messages on plane 3 also carry feature exchanges (`exchange()` sends `exchange_app_str_data`), stream reports, and `big_data_req` used to announce screen shares.

### The offer

`media_offer()` describes three streams, the **A** (audio), **V** (video) and **D** (data) sessions:

| Stream | Payload type | Media kind | Bitrate | Notes |
| ------ | ------------ | ---------- | ------- | ----- |
| A | 96 (or the peer's) | 1 | 32 kbps | Opus as LINE's EAS2 |
| V | 97 (or the peer's) | 2 | 4000 kbps receive limit | Initial state active only for video calls. 30 fps, resolution class 3 (HD/FHD). Feature entry 0 at version 1: baseline VP8 without VP8A. |
| D | 98 (or the peer's) | 6 | 2000 kbps | PDTP, feature 0 at version 2 |

Each stream has an RTP path (payload type and SSRCs) and, for A and V, an RTCP channel. Then come the **key agreement** (our P-256 media public key, our media ID and the 16-byte media nonce) and, in an offer, the 30-byte media secret.

`call_video_profile()` and `call_video_offer_profile()` read the peer's answer or offer. They accept only **EVS3 VP8 with the base descriptor** (payload map 2, VP8 feature version 1), including from peers that also offer VP8A. They return the negotiated payload type and the **direction-specific SSRCs**: in the final answer, field 11 is the callee's transmit SSRC and field 61 the caller's. If the peer declines the supported video profile, the call continues as voice.

`call_video_peer_limits()` reads the bitrate, frame rate and resolution class the peer advertises for *its* incoming video. `choose_send_limits()` sizes the outgoing picture from them, up to 1080p. `LINE_CALL_VIDEO_KBPS` overrides the bitrate.

## 4. Media keys and SRTP

Once both offers are known, each direction of each stream gets its own SRTP key (`media_direction()` in `call_wire.c`):

```
shared = ECDH(our media private key, peer's media public key)
stage1 = kdf_SHA256(shared, salt = sender's public key, info = receiver's public key)
stage2 = kdf_SHA256(stage1, salt = sender's 16-byte media nonce, info = sender's media ID, big-endian, padded to 8 bytes)
key    = HMAC-SHA256(key = label, data = stage2[0..30])[0..30]
```

The label is `"AUDIO"`, `"VIDEO"` or `"DATA"`, LINE's names for the media kinds. The 30 bytes are an SRTP master key and salt. `call_srtp_open()` creates a libsrtp2 context with **AES_CM_128_HMAC_SHA1_80** for RTP and RTCP, bound to the negotiated SSRC, separately for sending and receiving.

Media packets go to the media address the peer's connection signalling gives, which may differ from the signalling relay. Signalling and bootstrap stay on the original relay. A new media address is accepted only from an authenticated packet.

## 5. Audio

[`call_audio.c`](../../src/call_audio.c):

- **Capture.** The encoder takes 48 kHz mono in 20 ms frames (960 samples). Group calls can capture in stereo and select a microphone channel before encoding. Each 10 ms half goes through RNNoise first ([`call_denoise.c`](../../src/call_denoise.c)), unless `LINE_NOISE_SUPPRESSION=0`. RNNoise is a small neural network that removes fan noise, wind and the hiss some laptop microphones pick up when the webcam turns on. It costs about 0.5% of one CPU core.
- **Encoding.** Opus, `OPUS_APPLICATION_VOIP`, 32 kbps, voice signal, in-band FEC, 5% expected loss.
- **The EAS2 format.** LINE calls its normal-audio codec EAS2. On the wire, Opus frames are wrapped with LINE's own frame headers: a single frame with an activity bit, or grouped frames with an activity bitmap, and mixed groups where each table-of-contents byte's low bit marks the last. The receiver normalizes these back to plain Opus before decoding.
- **Playback.** Received packets are authenticated against the negotiated payload type and SSRC, briefly held to absorb reordering, and played out on schedule (`line_audio_playout()`). Decoder state resets on a gap.
- **Mute** stops sending microphone audio (`line_audio_mute()`).

## 6. Video

[`call_video.c`](../../src/call_video.c):

- **Camera.** [`camera.c`](../../src/camera.c) opens `/dev/video0` (or `LINE_CAMERA_DEVICE`) with V4L2, picks the largest smooth YUYV or MJPEG mode within the limits, and converts frames to I420 ([`camera_pixels.c`](../../src/camera_pixels.c), built `-O3`). The frame size is capped at 1920 × 1080 (`line_camera_frame_size_valid()`). An MJPEG frame is decoded to full-range planes by libjpeg-turbo, then `line_camera_jpeg_to_i420()` maps it to the video range VP8 expects. It uses exact integer division by 255 that GCC vectorizes, with one specialized loop per chroma layout (4:2:0, 4:2:2, 4:4:0 and 4:4:4). A 720p frame takes about 0.5 ms, roughly six times faster than the earlier per-pixel division, with identical output (`/video/camera-jpeg-range`).
- **Decoding.** The worker uses the renderer’s DRM device for incoming camera and shared-screen VP8. NVIDIA uses NVDEC. AMD and Intel use VA-API when the driver supplies a VP8 decode profile. The worker downloads hardware surfaces for the existing I420 frame handoff and keeps their orientation. Missing hardware support or decoder initialization failure selects libvpx. NVIDIA surfaces that pad an odd-sized VP8 frame can lose chroma at the edge. Avernella preserves those pictures with libvpx and can resume hardware decoding when the resolution changes.
- **Encoding.** The camera and both screen-share transports try hardware VP8 at the negotiated size, frame rate and bitrate. On AMD and Intel, FFmpeg can use VA-API when the driver exposes a VP8 encode entry point. Desktop NVIDIA NVENC has no VP8 encoder. Jetson Nano and TX2 can use JetPack’s VP8 encoder on AArch64 builds. The renderer’s adapter is tried first. Automatic selection can use another adapter that supports VP8, and an explicit `AVERNELLA_RENDER_NODE` pins the adapter. If hardware declines the stream or fails mid-call, libvpx encodes the next picture to send as a keyframe. To see the backend actually producing pictures, run `LINE_RENDER_LOG=1 ./avernella` and look for `Outgoing camera VP8 encoder` or `Outgoing screen VP8 encoder`. [NVIDIA desktop codec support](https://developer.nvidia.com/video-codec-sdk) · [NVIDIA Jetson encoder support](https://docs.nvidia.com/jetson/archives/l4t-archived/l4t-3275/Tegra%20Linux%20Driver%20Package%20Development%20Guide/accelerated_gstreamer.html)
- **Software encoding.** Camera and RTP screen video use real-time libvpx settings and a keyframe at least every three seconds. The speed starts at `-6` for 720p and adapts once a second like WebRTC's CPU-overuse detector (`call_video_pace()`). If encoding averaged more than 60% of the frame interval, the encoder moves one step faster, down to `-12`. Below 25% it steps back toward the starting speed. The screen-share encoder adapts the same way. The libvpx software decoder uses up to four threads. `./build/test-call-video -m perf -p /video/encode-720p` reports the cost of a 720p frame through the full send path.
- **EVS3 packets.** LINE doesn't use the standard RTP VP8 payload format in PLANET calls. Each frame is split into fragments, each with an **EVS3 picture descriptor**: a flags byte (bit 3 start, bit 2 end, bit 5 "layer and reference byte follows", bit 1 "orientation byte follows"), a 16-bit big-endian picture ID, and, on the first fragment, a resolution and key byte and a three-byte length that carries *N* + 3 in groups of 2, 8 and 8 bits, least significant first. Examples are in [analysis/video-call-wire.md](../../analysis/video-call-wire.md#media-packets).
- **Rotation.** The orientation byte's two high bits give 0°, 90°, 180° or 270°. The UI rotates the whole picture.
- **Feedback.** RTCP payload-specific feedback with format 17 (**FIR**, a full intra request) asks the sender for a keyframe after loss (`call_video_send_feedback()`, `call_video_receive_feedback()`).
- **Turning the camera on mid-call.** Before each camera change, the client sends a media-control **stream report** (`call_stream_spec()`) listing its transmit SSRC as running, because peers otherwise keep the earlier paused state. Media-control stream actions are 1 START, 2 STOP, 3 PAUSE and 4 RESUME.
- **To the window.** Decoded frames go to the UI as sealed memfds over the frame socket (see [Architecture](architecture.md#call-video-frames)), along with your own camera preview.

## 7. Screen sharing

[`screencast.c`](../../src/screencast.c) captures the screen through the XDG Desktop Portal's ScreenCast interface: the portal shows its own picker, then frames arrive through PipeWire, are converted from BGRx/RGBx to I420 with bilinear scaling (`line_screencast_to_i420()`) and fitted to the limits (`LINE_SHARE_WIDTH`, `LINE_SHARE_HEIGHT`, `LINE_SHARE_FPS`).

There are two ways to send them:

1. **In the video stream** (the default). The shared screen replaces the camera in the sender's video tile. The encoder uses a screen-specific bitrate budget (`call_video_screen()`, `LINE_SHARE_KBPS`) and varies resolution and frame rate to keep text sharp. In a group call, Avernella uses the negotiated group video track. Share a window with **Alt+S** and press it again to stop. Avernella restores the previous camera state. A new subscriber can request a keyframe even while the desktop image stays still.
2. **As a native presentation in a one-to-one call** (`LINE_SHARE_TRANSPORT=presentation`, experimental), the way LINE's apps share: over **PDTP** in the **D** session.

Avernella shows an incoming native presentation in a separate **LIVE** tile. In a group call the presentation stays visible when you select another participant’s camera. You can open the shared screen in fullscreen. When the presenter stops, the tile closes. Some peers share through their camera track instead. In that case the focused participant’s video tile carries the **LIVE** badge until they switch back to the camera. The receiver follows one presenter in the main group call. It can receive the screen even when the laptop joined by voice. Separate side rooms are outside this implementation. The group path has offline relay coverage and still needs a live Android playback check. [Group media tests](testing.md#group-video-and-screen-sharing)

### PDTP

LINE calls a screen share a *presentation*: media kind 4 (DATA) with data service type 1 (PRESENTATION), carried by **PDTP**, a QUIC-like reliable stream protocol inside SRTP packets of the D session. [`call_pdtp.c`](../../src/call_pdtp.c) implements it:

- **Keys:** the same derivation as audio and video, labeled `"DATA"`.
- **A packet** (the RTP payload): the high bytes of a QUIC-style varint packet number (its low 16 bits are the RTP sequence number), a NUL-terminated service name (empty for transport elements, `LINE` for the stream every call opens, `PLANET` for presentations), a big-endian 16-bit bitmap of the element types present, then one group per type from 15 down to 1.
- **Element types:** 1 STREAM_DATA, 2 STREAM_CLOSE, 3 STREAM_CTRL, 4 STREAM_OPEN, 5 STREAM_RESET, 9 INCLUDE_RETRANS, 10 SET_MAX_ACK_DELAY, 11 SWITCH_ACK_ALL, 12 REQUEST_WINDOW, 13 WINDOW_UPDATE, 15 ACK.
- **Reliability:** data is acknowledged, retransmitted and flow-controlled by a window (327,680 bytes until updated). A client **must** ACK PDTP: LINE's apps open stream 110 under `LINE` on every call and retransmit until acknowledged.
- **Presentation streams:** a sender opens `PLANET` streams 3 (commands, such as START VIDEO `01 01`) and 4 (one EVS3 VP8 fragment per PDTP message, with a slightly different descriptor and no length field).
- **Pause and resume:** a receiver may send REQ_PAUSE and REQ_RESUME controls. The sender answers PAUSE and resumes with a keyframe.
- **Announcement:** a share is also announced as feature-share item 16 in a plane-3 `big_data_req`, with a zlib-compressed JSON body.
- `LINE_CALL_PDTP_DUMP=<dir>` together with `LINE_CALL_DEBUG=1` appends raw LINE and PLANET stream contents to `<dir>/<service>-<stream>.bin`. These files can contain a participant’s screen share. Use an existing owner-only directory (mode 0700). Capture files are private to the owner. Ordinary call diagnostics report packet sizes, counters and route changes.

[analysis/video-call-wire.md](../../analysis/video-call-wire.md#screen-sharing-presentation) records exactly what has and hasn't been verified against LINE's apps. The direct-call receive path has been exercised with the official app. Sending a separate native presentation remains experimental.

## Incoming calls

An incoming call is a sync operation. `line_voice_operation()` (called for every operation, see [Sync](sync.md)) looks for OpType **50** (invitation) and **51** (canceled):

- param1 (field 10) is the caller's MID, param2 (field 11) a route token, and param3 (field 12) a JSON object with the call details: `vs` the call ID, `h` and `hv6` the relay hosts, `p` the port, `vfz` and `vtz` the zones, `vc` the communication config (with `mpkey`), `k` the kind (`CA` audio, `CV` video), and `n`, which must equal the route token. Group calls (`g`) and other session kinds (`sei`) are ignored.
- Every field is validated. Operations older than 45 seconds, from more than 5 seconds in the future, or at or below the last seen call revision are ignored, so replays and history never ring.
- A valid invitation becomes `pending` and the worker emits `call_incoming`. The UI rings (ringtone, card, Alt+A or Alt+D). Unanswered invitations expire 45 seconds after they were created (`line_voice_tick()`).
- OpType 51 for the pending call, or for the active incoming call, ends it.

## Call events to the UI

| Event | Meaning |
| ----- | ------- |
| `call` (`chat`, `state`, `video`, `cameraEnabled`, `message`) | State changes, chiefly `ringing`, `connected` and `ended` |
| `call_incoming` (`chat`, `video`) | Ring |
| `call_error`, `call_camera_error` | Something failed |
| `call_share` | The peer started or stopped presenting |
| `call_frame`, `call_self_frame`, `call_share_frame` | New frames for the polling path (`callFrame`); the main path is the frame socket |

## Debugging a call

Set `LINE_CALL_DEBUG=1`. The terminal shows negotiation (payload types, SSRCs, limits), each control message by plane and field, packet counts every five seconds, and first-frame milestones. Common findings:

- **No video but audio works:** the peer declined the VP8 profile, or no keyframe arrived. Look for the negotiated payload and "first frame".
- **No audio:** check that packets authenticate. A wrong SSRC or payload type makes every packet fail SRTP.
- **The call never connects:** the relay or media address isn't reachable over UDP from your network.

## Tests

`tests/call.c` runs complete calls against a **local relay fixture**: incoming and outgoing signalling, bidirectional SRTP audio and video, media address changes, rejection of tampered and wrong-stream packets, continuing as voice when video fails, and screen sharing with and without the camera. SDL audio is replaced with `-Wl,--wrap`. `tests/call_video.c` checks VP8 encoding and decoding through EVS3 fragments and SRTP, including reordering, loss, replays and keyframe recovery, against the Android descriptor bytes. `tests/call_audio.c` covers the EAS2 framing and playout, and `tests/call_denoise.c` the noise suppressor.
