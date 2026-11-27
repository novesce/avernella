# Calls

[Index](README.md) · Previous: [LIFF](liff.md) · Next: [Testing](testing.md)

Avernella makes and answers **one-to-one** voice and video calls, with screen sharing. Calls use Thrift RPC, encrypted protobuf signalling, and SRTP media. The byte-level evidence with addresses in LINE's `libandromeda.so` is in [analysis/video-call-wire.md](../../analysis/video-call-wire.md).

Video is on by default and can be turned off with `LINE_EXPERIMENTAL_VIDEO=0` (`line_video_enabled()`).

## Group calls

Click the handset for voice or the camera for video in a group chat. Group calls are enabled by default. Set `LINE_EXPERIMENTAL_GROUP_CALLS=0` to disable both buttons or `LINE_EXPERIMENTAL_GROUP_VIDEO=0` to disable group video. The worker checks the current call with `getGroupCall`, requests a route, and waits for the conference relay's final `PARTICIPATE_RSP` before opening the call devices. It registers the assigned streams, derives the SRTP keys from the conference media secrets, and sends EAS2 audio in 40 ms packets. On hangup the worker closes the microphone and camera, then waits up to two seconds for the relay to confirm the release. Quitting the desktop app also gives the worker time to leave the call. If LINE reports that this account is already in a call on another device, leave the call on that device before joining from the laptop. Other participants can stay in the call.

The call stage shows the people returned by the call service and refreshes the roster while connected. Group microphone capture selects a stable input channel on stereo devices and lifts quiet input before noise suppression. The group audio receiver unwraps LINE’s XRTP mixer packets, decodes each speaker with a separate Opus decoder, and mixes the results for playback. It starts with up to 100 ms of playout cushion, adjusts it from measured transit variation, and uses RTP timestamps to avoid replaying speech. The worker acknowledges relay media-channel and audio stream requests with the negotiated stream IDs. Authenticated mixer audio and camera packets can arrive from a separate UDP port. The microphone reports silence, unvoiced sound or voiced sound for each 20 ms frame and sends the louder frame’s level for each pair to the assigned conference relay. Avernella estimates voicing from the captured audio.

In a group video call, Avernella sends the local camera and decodes one remote camera at a time. Select a participant in the roster to focus their camera. The media worker only subscribes to camera sources named in authenticated main-conference membership packets. The camera uses VP8 over EVS3. Avernella offers LINE Android's HD layer ceiling for its camera: 3000 kbit/s, 30 fps and resolution class 3. Each viewer then requests a class to fit the size of its tile. When the relay requests a smaller supported camera layer, the worker scales the outgoing picture before encoding and sends a keyframe at the new dimensions. The scaled picture keeps the camera's shape and fills the class, so a grid tile receives 554 × 310 from a 720p camera. The bitrate stays within the class budget of 150, 300, 800 or 3000 kbit/s. The local preview keeps its capture resolution. Avernella offers one software VP8 video layer for the group and normal-video as a fallback. The sender uses the payload map returned in the answer. The receiver also handles layered streams forwarded by the relay. The first registration reports the local camera as active only if capture opened successfully. When the relay acknowledges the outgoing camera source, Avernella sends a fresh keyframe. If the relay selects an unsupported codec, the call continues by voice.

Live group calls with the official Android app confirmed voice in both directions and Android camera video on Linux. In a follow-up call, the relay accepted the laptop camera, requested a smaller video layer and acknowledged over 13,800 camera packets. Android still showed the participant’s avatar. The sender now uses the main conference video transport, identifies the requested layer on every packet, and adjusts its encoding rate to that layer. In an offline replay, LINE’s Android video filter rejected all 190 packets from the previous build and accepted all 380 corrected test packets. A later live call with LINE for Android 26.18.2 confirmed that the phone displays the laptop camera and a shared screen. The phone requested class 1 for its two-person grid, class 2 when its call controls were hidden and class 0 for a thumbnail.

The group sender marks the base camera layer in each picture and includes the capture source and encode time in the first fragment. The source is always tagged as a camera. LINE Android shows the avatar for a participant whose video carries any other source tag, including the display tag. Changing between webcam and screen still starts a new capture generation. Normal-video uses a three-byte codec header. For a layered stream, a six-byte profile precedes the codec header. VP8 uses a three-byte length and VP8A a four-byte length. Camera START requests trigger a keyframe and preserve this framing. Tests compare outgoing descriptors with the Android packetizer.

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

The call thread uses one UDP socket for everything ([`udp_batch.c`](../../src/udp_batch.c)). It drains all waiting datagrams with one `recvmmsg()` and skips `poll()` while some are left. A video picture's packets go out back to back with no pacing between them, so `send_video_packet()` queues them and sends the burst when the packet with the RTP marker bit arrives (RTCP has the same bit set and goes at once). The burst leaves in one `sendmmsg()`, with runs of equal-size packets as `UDP_SEGMENT` messages that the kernel splits into the same datagrams. Every other send (`send_to()`) sends the queue first, so datagrams keep their order. With `LINE_DISABLE_UDP_BATCH=1`, or on a kernel or sandbox that refuses these calls, the socket sends and receives one datagram per system call, as before.

`./build/test-udp-batch -m perf -p /udp-batch/perf/send/UDP_SEGMENT` (and the `sendto`, `sendmmsg`, `receive/recvfrom` and `receive/recvmmsg` cases) measures CPU time per datagram for a 48-packet burst over loopback. On a Ryzen machine with Linux 7.3 it measured 2.27 µs (`sendto`), 1.93 µs (`sendmmsg`) and 0.17 µs (`UDP_SEGMENT`) to send, and 0.64 µs (`recvfrom`) and 0.49 µs (`recvmmsg`) to receive. Loopback defers segmentation to delivery, so expect a smaller gain over a real network card without UDP segmentation offload.

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

`call_video_peer_limits()` reads the bitrate, frame rate and resolution class the peer advertises for *its* incoming video. `choose_send_limits()` sizes the outgoing picture from them, up to 1080p. LINE's apps advertise 400 kbit/s and VGA even when HD calls are enabled on Wi-Fi. They still display 1280 × 720 at 1.5 Mbit/s, so a direct call sends at least that. `LINE_CALL_VIDEO_KBPS` overrides the bitrate, for example `400` on a slow link.

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
- **Encoding.** One-to-one calls use Opus voice at 32 kbit/s with in-band FEC. Expected loss starts at 5%. A valid authenticated RTCP sender or receiver report for the microphone replaces that estimate with the peer’s measured loss percentage. Opus decides how many bits it can use for recovery within that budget. Conference EAS2 uses CELT and cannot carry SILK in-band FEC.
- **The EAS2 format.** LINE calls its normal-audio codec EAS2. On the wire, Opus frames are wrapped with LINE's own frame headers: a single frame with an activity bit, or grouped frames with an activity bitmap, and mixed groups where each table-of-contents byte's low bit marks the last. The receiver normalizes these back to plain Opus before decoding.
- **Playback.** Received packets are authenticated against the negotiated payload type and SSRC. In a one-to-one call they wait in an adaptive jitter buffer, and the speaker's demand drives decoding (`line_audio_playout()`, see [Bad networks](#8-bad-networks)). Group calls keep the mixer playout: a short reorder hold, then playback on schedule. Decoder state resets on a long gap.
- **Mute** stops sending microphone audio (`line_audio_mute()`).

For a stereo group-call microphone, Avernella measures the changing part of each channel so a stuck input or a DC offset cannot win the selection. If one channel clips repeatedly and the other has a usable signal, it favors the cleaner channel. If both channels clip, reduce the input gain in the system sound settings.

With `LINE_GROUP_CALL_STATS=1`, compare `mic-blocks` (complete 40 ms blocks captured) and `mic-ac-power-max` (peak block mean-square signal before noise suppression) with `tx-power` (average signal after processing) and `mic-fb-received` (microphone packet arrivals reported by the relay). Listening on a second device verifies the last step.

For group screen sharing, the `group-stats share` line appears every three seconds and at hangup. `captured` counts frames taken from the desktop. `screen-tx-frames` counts complete screen pictures sent on the video track. `screen-rx-frames` counts decoded pictures marked as a display on that track. `native-rx-chunks` and `native-rx-frames` count native presentation chunks delivered to the call worker and pictures the decoder produced. Compare `native-rx-messages` with `native-rx-rejected` to check whether complete messages reached the decoder and whether it accepted their framing and video data. These totals cover the current call. To check playback on the other device, have its user open the sender’s tile.

## 6. Video

[`call_video.c`](../../src/call_video.c):

- **Camera.** [`camera.c`](../../src/camera.c) opens `/dev/video0` (or `LINE_CAMERA_DEVICE`) with V4L2, picks the largest smooth YUYV or MJPEG mode within the limits, and converts frames to I420 ([`camera_pixels.c`](../../src/camera_pixels.c), built `-O3`). The frame size is capped at 1920 × 1080 (`line_camera_frame_size_valid()`). An MJPEG frame is decoded to full-range planes by libjpeg-turbo, then `line_camera_jpeg_to_i420()` maps it to the video range VP8 expects. It uses exact integer division by 255 that GCC vectorizes, with one specialized loop per chroma layout (4:2:0, 4:2:2, 4:4:0 and 4:4:4). A 720p frame takes about 0.5 ms, roughly six times faster than the earlier per-pixel division, with identical output (`/video/camera-jpeg-range`).
- **Decoding.** The worker uses the renderer’s DRM device for incoming camera and shared-screen VP8. NVIDIA uses NVDEC. AMD and Intel use VA-API when the driver supplies a VP8 decode profile. When the renderer accepts external images, the worker exports a supported VA-API NV12 surface as an explicit-modifier DMA-BUF. The worker synchronizes the decoder with FFmpeg's read mapping and holds the surface until the renderer releases it. Unsupported layouts and NVDEC use the I420 handoff with the original orientation. A renderer rejection replays the latest hardware image through the I420 mailbox, including a stationary shared screen. The installed AMD test machine supports VA-API NV12 upload and export, but its driver does not decode VP8. Exercise the producer with `AVERNELLA_TEST_VAAPI_NODE=/dev/dri/renderD129 build/test-vp8-dma`, selecting the render node for your system. Missing hardware support or decoder initialization failure selects libvpx. NVIDIA surfaces that pad an odd-sized VP8 frame can lose chroma at the edge. Avernella preserves those pictures with libvpx and can resume hardware decoding when the resolution changes.
- **Encoding.** The camera and both screen-share transports try hardware VP8 at the negotiated size, frame rate and bitrate. On AMD and Intel, FFmpeg can use VA-API when the driver exposes a VP8 encode entry point. Desktop NVIDIA NVENC has no VP8 encoder. Jetson Nano and TX2 can use JetPack’s VP8 encoder on AArch64 builds. The renderer’s adapter is tried first. Automatic selection can use another adapter that supports VP8, and an explicit `AVERNELLA_RENDER_NODE` pins the adapter. If hardware declines the stream or fails mid-call, libvpx encodes the next picture to send as a keyframe. To see the backend actually producing pictures, run `LINE_RENDER_LOG=1 ./avernella` and look for `Outgoing camera VP8 encoder` or `Outgoing screen VP8 encoder`. [NVIDIA desktop codec support](https://developer.nvidia.com/video-codec-sdk) · [NVIDIA Jetson encoder support](https://docs.nvidia.com/jetson/archives/l4t-archived/l4t-3275/Tegra%20Linux%20Driver%20Package%20Development%20Guide/accelerated_gstreamer.html)
- **Software encoding.** Camera and RTP screen video use real-time libvpx settings and a keyframe at least every three seconds. The speed starts at `-6` for 720p and adapts once a second like WebRTC's CPU-overuse detector (`call_video_pace()`). If encoding averaged more than 60% of the frame interval, the encoder moves one step faster, down to `-12`. Below 25% it steps back toward the starting speed. The screen-share encoder adapts the same way. The libvpx software decoder uses up to four threads. `./build/test-call-video -m perf -p /video/encode-720p` reports the cost of a 720p frame through the full send path.
- **EVS3 packets.** LINE doesn't use the standard RTP VP8 payload format in PLANET calls. Each frame is split into fragments, each with an **EVS3 picture descriptor**: a flags byte (bit 3 start, bit 2 end, bit 5 "layer and reference byte follows", bit 1 "orientation byte follows"), a 16-bit big-endian picture ID, and, on the first fragment, a resolution and key byte and a three-byte length that carries *N* + 3 in groups of 2, 8 and 8 bits, least significant first. Examples are in [analysis/video-call-wire.md](../../analysis/video-call-wire.md#media-packets).
- **Rotation.** The orientation byte's two high bits give 0°, 90°, 180° or 270°. The UI rotates the whole picture.
- **Feedback.** RTCP payload-specific feedback with format 17 (**FIR**, a full intra request) asks the sender for a keyframe after loss. Retransmission requests, receiver reports and REMB adapt the stream to the network (`call_video_poll()`, `call_video_receive_rtcp()`, see [Bad networks](#8-bad-networks)).
- **Turning the camera on mid-call.** Before each camera change, the client sends a media-control **stream report** (`call_stream_spec()`) listing its transmit SSRC as running, because peers otherwise keep the earlier paused state. Media-control stream actions are 1 START, 2 STOP, 3 PAUSE and 4 RESUME.
- **To the window.** Decoded frames go to the UI as sealed memfds over the frame socket (see [Architecture](architecture.md#call-video-frames)), along with your own camera preview.

## 7. Screen sharing

The portal picker opens a PipeWire stream. Before advertising NV12 DMA-BUF, the capture worker imports a test surface into the selected VA-API encoder and requires a valid VP8 keyframe at the requested size. When the compositor supplies the tested modifier, exact size and limited-range BT.601, the encoder retains the producer’s buffer until it finishes reading. Both screen-over-RTP and native presentation use this path. An unsupported layout, explicit fence, requested software scaling, or failed import switches the same stream to shared memory. Shared-memory capture supports planar I420 and packed RGB with bilinear scaling. The size and frame rate limits are `LINE_SHARE_WIDTH`, `LINE_SHARE_HEIGHT`, and `LINE_SHARE_FPS`. [Capture implementation](../../src/screencast.c) · [PipeWire DMA-BUF negotiation](https://docs.pipewire.org/page_dma_buf.html)

To verify import independently of VP8 support, run `AVERNELLA_TEST_VAAPI_NODE=/dev/dri/renderD129 build/test-call-video -m perf -p /video/vaapi-capture-import` with your VA-API node. The test checks every pixel of a synthetic 720p frame. Its p50/p95/p99 figures measure CPU time spent preparing a hardware frame and exclude compression and network delivery. Over 300 720p frames on the Radeon 780M, CPU NV12 upload took 105/304/361 µs at p50/p95/p99. Direct import and release took 8/15/24 µs and the readback matched all 1,382,400 bytes. Neither it nor the RTX 5050 reported a hardware VP8 encode profile.

On Cinnamon Wayland with the default XApp/GTK portal configuration, if the portal omits ScreenCast, the native client can list active monitors from Muffin. Selecting a monitor and confirming **Share your screen** starts a stream for that connector. Cancelling, disconnecting or locking the session stops the operation. A change in the display configuration while the picker is open requires a fresh selection. Window-only sharing uses the standard desktop portal.

There are two ways to send them:

1. **In the video stream** (the default). The shared screen replaces the camera in the sender's video tile. The encoder uses a screen-specific bitrate budget (`call_video_screen()`, `LINE_SHARE_KBPS`) and varies resolution and frame rate to keep text sharp. In a group call, Avernella uses the negotiated group video track and tags the screen as camera video so that LINE's mobile apps display it. The screen is always sent as the HD layer, up to 1920 × 1080 at 3000 kbit/s, even when a viewer's grid tile requests a smaller layer. This keeps shared text readable. LINE's relay forwards the larger layer and the phone displays it in the tile. Share a window with **Alt+S** and press it again to stop. Avernella restores the previous camera state. A new subscriber can request a keyframe even while the desktop image stays still.
2. **As a native presentation in a one-to-one call** (`LINE_SHARE_TRANSPORT=presentation`, experimental), the way LINE's apps share: over **PDTP** in the **D** session.

Avernella shows an incoming native presentation in a separate **LIVE** tile. In a group call the presentation stays visible when you select another participant’s camera. You can open the shared screen in fullscreen. When the presenter stops, the tile closes. Some peers share through their camera track instead. In that case the focused participant’s video tile carries the **LIVE** badge until they switch back to the camera. The receiver follows one presenter in the main group call. It can receive the screen even when the laptop joined by voice. Separate side rooms are outside this implementation. The group path has offline relay coverage and still needs a live Android playback check. [Group media tests](testing.md#group-video-and-screen-sharing)

If LINE announces a presentation before sending the group roster, Avernella waits for the roster to identify the presenter and then requests the stream. Incoming native presentations support portrait and landscape frames up to 4,096 pixels on either edge, with a total limit of 3,840 × 2,160 pixels. For an outgoing screen share, a new viewer can request an immediate keyframe even while the desktop is still.

### PDTP

LINE calls a screen share a *presentation*: media kind 4 (DATA) with data service type 1 (PRESENTATION), carried by **PDTP**, a QUIC-like reliable stream protocol inside SRTP packets of the D session. [`call_pdtp.c`](../../src/call_pdtp.c) implements it:

- **Keys:** the same derivation as audio and video, labeled `"DATA"`.
- **A packet** (the RTP payload): the high bytes of a QUIC-style varint packet number (its low 16 bits are the RTP sequence number), a NUL-terminated service name (empty for transport elements, `LINE` for the stream every call opens, `PLANET` for presentations), a big-endian 16-bit bitmap of the element types present, then one group per type from 15 down to 1.
- **Element types:** 1 STREAM_DATA, 2 STREAM_CLOSE, 3 STREAM_CTRL, 4 STREAM_OPEN, 5 STREAM_RESET, 9 INCLUDE_RETRANS, 10 SET_MAX_ACK_DELAY, 11 SWITCH_ACK_ALL, 12 REQUEST_WINDOW, 13 WINDOW_UPDATE, 15 ACK.
- **Reliability:** data is acknowledged, retransmitted and flow-controlled by a window (327,680 bytes until updated). A client **must** ACK PDTP: LINE's apps open stream 110 under `LINE` on every call and retransmit until acknowledged. `parse_ack()` checks that every ACK range stays at or above packet zero before applying any of them, then applies them in one pass over the in-flight list, so an ACK costs the same however many packet numbers it claims. Stream data must end inside the receive window and at or below the largest varint offset, 2⁶² − 1. These checks subtract instead of add, so no peer value can wrap.
- **Presentation streams:** a sender opens `PLANET` streams 3 (commands, such as START VIDEO `01 01`) and 4 (one EVS3 VP8 fragment per PDTP message, with a slightly different descriptor and no length field).
- **Pause and resume:** a receiver may send REQ_PAUSE and REQ_RESUME controls. The sender answers PAUSE and resumes with a keyframe.
- **Announcement:** a share is also announced as feature-share item 16 in a plane-3 `big_data_req`, with a zlib-compressed JSON body.
- `LINE_CALL_PDTP_DUMP=<dir>` together with `LINE_CALL_DEBUG=1` appends raw LINE and PLANET stream contents to `<dir>/<service>-<stream>.bin`. These files can contain a participant’s screen share. Use an existing owner-only directory (mode 0700). Capture files are private to the owner. Ordinary call diagnostics report packet sizes, counters and route changes.

[analysis/video-call-wire.md](../../analysis/video-call-wire.md#screen-sharing-presentation) records exactly what has and hasn't been verified against LINE's apps. The direct-call receive path has been exercised with the official app. Sending a separate native presentation remains experimental.

## 8. Bad networks

The wire format is fixed by LINE, so Avernella improves calls on lossy, jittery or slow links by how it uses that format. Everything below works with any peer. What depends on the peer's feedback is marked.

**Video receive** ([`call_video.c`](../../src/call_video.c)):

- **A reorder buffer before picture assembly.** Packets in sequence go straight to the EVS3 assembler, with no added delay. After a gap, later packets wait until the missing one arrives, or until the hold expires. The hold is about the usual reordering delay. Once the peer has answered a retransmission request, it also covers a request and one repeat (grace + 2.5 × RTT + 30 ms, at most 500 ms). A complete keyframe never waits behind a gap, because nothing before a keyframe matters to the decoder. A late fragment of the picture still assembling is still accepted.
- **Retransmission requests.** A packet missing for longer than about three-quarters of the observed reordering is requested with LINE's own RTPFB format 5 (RRR, the list of 16-bit sequence numbers the group relay also sends). It is requested again after 1.25 × RTT, three times at most. RTT is measured from request to arrival. One-to-one calls only.
- **Keyframe requests.** FIR goes out as soon as decoding needs a keyframe, then again after 300 ms, 600 ms and every second. The pace resets after each decoded picture. Group calls keep one request a second, because the relay rate-limits keyframes itself.
- **Receiver reports.** Once a second the receiver sends a standard RTCP receiver report. Its loss fraction counts only packets that were neither received nor repaired, so reordering does not look like loss.
- **REMB.** The receiver times each picture's last packet against its RTP timestamp. Delay above the lowest of the last 15–30 s is queueing. If the queue passes 50 ms and keeps growing, or passes 120 ms, the estimate becomes 85% of the measured arrival rate, cut deeper for longer queues (down to half). It grows 10% a second while the queue stays short, or 25% after three calm seconds, and is dropped once it no longer limits the sender. The estimate is sent as a standard REMB packet (PSFB format 15).

The delay estimate uses increasing 90 kHz capture timestamps. Late retransmissions count toward link throughput and skip the delay sample. The receiving end sends a protected reception report for direct audio and video. It counts authenticated packets, including repairs, and reports interarrival jitter in the stream’s clock units. For RFC 7741 video, losing fragments confined to a marked non-reference picture lets the next complete picture decode against the preceding reference.

Avernella sends camera video on one negotiated EVS3 temporal stream. The current VED profile signals a referenceable base layer in both the RTP extension and the compressed-picture descriptor. In the offline `/video/evs3-temporal-recovery` case, libvpx decoded the next delta byte-for-byte after skipping a disposable picture. With those same VP8 bytes in the group transport, a missing picture left the receiver waiting for a keyframe. Enabling another temporal layer requires peer negotiation and matching reference and subscription metadata. [Android filter fixtures](../../analysis/video-call-wire.md)

**Video send:**

- **Retransmission.** The sender keeps the last 512 protected packets and resends the identical bytes on request (RRR or RFC 4585 NACK), up to three times each within a second. The SRTP replay check makes a duplicate of a packet that did arrive harmless. Resends may add about a third to the rate. The conference relay's RRR requests are answered for the camera, but not while you share your screen.
- **Rate control, after Google Congestion Control.** Above 10% loss the rate falls by half the loss rate, at most once a second and never below 70% of a fresh REMB. Below 2% it grows 10% a second, or 25% after three calm seconds. In between it holds. REMB caps the rate at once. Loss comes from receiver reports when the peer sends them, and otherwise from retransmission requests. A peer that sends only keyframe requests still lowers the rate when it asks more than once in half a second. Without any feedback, the rate stays at the negotiated ceiling, as before. libvpx changes its rate between frames without a keyframe. A hardware encoder restarts, so it follows only changes of 25% or more.
- **Resolution and frame rate.** In a one-to-one call the camera picture steps down to 3/4, 1/2, 3/8 or 1/4 of its size when each pixel would get fewer than 0.05 bits. It steps back up when the larger size would get more than 0.09, after at least 4 s. Below 250 kbit/s the camera sends 15 fps, and below 120 kbit/s 10 fps. Screen shares and group calls keep their size. A group screen share also keeps the HD layer's rate, so shared text stays readable.
- **Keyframe spacing.** Forced keyframes stay at least 300 ms apart. A request that arrives sooner is honoured when the spacing allows.

For an uplink requesting repairs on at least 2% of original packets, the encoder reserves the observed fraction of transmitted bytes used by repairs. It estimates the number of fragments a picture can afford from the request fraction `p`. Assuming independent attempts, the original packet plus two repair attempts give a residual loss probability of `p³`. The size limit targets 99% whole-picture delivery with `log(0.99) / log(1 − p³)` fragments, rounded up to at least one. Correlated loss can miss that target. A group screen share retains its negotiated HD budget.

`./build/test-call-net -p /net/video/asymmetric-burst-25%` runs a 20-second synthetic 720p30 camera, a 2.4 Mbit/s uplink with four-packet mean loss bursts and a separate 128 kbit/s return link. On the same input the old and updated controllers delivered 13.68 and 22.89 fps. Screen luma PSNR increased from 26.47 to 27.26 dB and the final encoder size increased from 320 × 180 to 1280 × 720. Simulated capture-to-decoded p50/p95/p99 changed from 93/424/608 to 159/367/425 ms. Total freeze time changed from 9.20 to 9.33 seconds over the 19-second measurement. These times exclude physical camera, display and compositor delays.

Vary `AVERNELLA_NET_SEED` to replay independent channel realizations. With seed offsets 1, 2 and 3, the updated 720p controller delivered 23.63, 24.11 and 23.11 fps, versus 17.42, 9.58 and 11.63 on `d4aaebc`. All three improved screen PSNR. Their p99 delays changed from 485 to 444 ms, 405 to 463 ms and 523 to 372 ms. The second run illustrates the tradeoff under correlated loss even when more frames reach the viewer.

The packet-size estimate is limited to at least one fragment per picture and a 60 kbit/s encoder floor. It treats repair requests as a loss proxy, smoothing counts with 100 prior observations. A Gilbert–Elliott burst can erase a packet and both repair attempts. On the weak Wi-Fi uplink seed, delivered frame rate stayed at 14.84 fps, screen PSNR fell from 35.05 to 34.10 dB, bitrate fell from 280.7 to 267.2 kbit/s and p99 stayed at 315 ms.

On the direct audio 25% burst fixture, 28 authenticated reports adjusted Opus’s expected loss from 6% to 63% over successive one-second intervals. At 25.2% measured forward loss, libopus 1.6.1 confirmed LBRR in 58 of 64 attempted gap decodes for both adaptive and fixed 5% settings. The remaining attempts used concealment. Microphone-to-speaker p50/p95/p99 measured 178/225/234 ms with feedback and 174/227/234 ms with the fixed setting. Opus 1.5 and newer can report actual LBRR presence. Group EAS2 continues to use the negotiated CELT payload. [Opus packet inspection](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)

**Audio playout** ([`call_audio.c`](../../src/call_audio.c))

- After unwrapping the 48 kHz RTP clock, the receiver subtracts sender time from kernel arrival time. It ranks the last 200 authenticated arrivals, including late packets, and measures variation above the fastest arrival. The playout target uses the larger of the observed 99th percentile and a fast EWMA mean plus 2.326 standard deviations (bounded by the largest observed delay), plus one packet, clamped to 40–500 ms. The 1% late-loss objective applies to arriving packets. Packets lost in transit are measured separately.
- When less than 40 ms waits at the speaker, the next packet is decoded. The RTP clock decides what is missing. A missing packet waits while the speaker still holds more than 21 ms of audio (PipeWire can drain 1024 samples at once), and longer while less than the target is buffered behind it. It is then rebuilt from the following packet's in-band Opus FEC, or with Opus loss concealment. A gap of more than 100 ms is skipped.
- With nothing buffered, playout continues with concealment for up to 200 ms. It then stops and buffers to the target again.
- For direct calls, delay exceeding the target by one 20 ms frame for half a second accrues a time-scaling budget. Both direct and group playout shed delay by overlapping similar waveforms one speech pitch period apart. Extreme direct-call backlog over 200 ms above target still skips frames.
- Conferences request 10 ms SDL device periods at 48 kHz. Direct calls use the same request. SDL’s PipeWire backend passes the request as `node.latency`, and the graph may negotiate a different quantum. Group playout also uses the transit estimate for reordering and queue depth. When the speaker needs audio before another packet arrives, Opus predicts up to 200 ms. In an XRTP mix only decoders active in the last mix contribute.

With two virtual audio devices in an isolated PipeWire graph, the SDL call settings produced `node.latency=480/48000` on both the capture and playback streams. This verifies the 10 ms request independently of the desktop sound server’s chosen hardware quantum.

**Media workers.** The SDL microphone callback publishes complete PCM blocks for Opus to a bounded single-producer ring and signals an eventfd. The call thread sleeps until a packet or another deadline is ready. After a stall it advances the RTP clock past discarded samples. Setting `LINE_CALL_CAPTURE_EVENTS=0` selects the original capture polling path. Video capture and VP8 encoding use a separate worker. For PDTP screen sharing the call thread hands over a reference to the captured frame. While one picture encodes, only the latest pending picture is retained. The call thread owns packet numbering, retransmissions and keyframe requests. `LINE_CALL_ENCODER_THREAD=0` selects synchronous encoding for comparison.

After the first keyframe selects libvpx, RTP camera and PDTP screen receivers decode on a worker. The network thread can receive audio during a video decode and reads completed images directly from the decoder planes. One additional compressed picture may wait behind the active one. A broken dependency or a full queue skips dependent pictures until a fresh keyframe. Hardware probing, hardware decoding and DMA-BUF leases retain their original owner. Set `LINE_CALL_DECODER_THREAD=0` to select inline decoding.

**Opus service time.** Run `./build/test-call-net -m perf -r /net/service` to compare running the codec on the call thread with handing the same work to one persistent worker. The virtual input produces voiced PCM at 48 kHz in 10 ms callbacks. The test runs RNNoise, the real Opus codec and SRTP in both directions and drains the virtual speaker. Each row gives p50 / p95 / p99 in microseconds on a Ryzen 9 270, taking the median of the corresponding percentile across three runs of 1,000 packets each after warm-up.

| Per-packet measurement | Direct call, 20 ms | Group, one sender, 40 ms | Group, eight XRTP tracks, 40 ms |
| --- | ---: | ---: | ---: |
| Call-thread encode and protect, CPU | 335 / 366 / 400 | 381 / 412 / 452 | 379 / 419 / 448 |
| Call-thread unprotect, decode and due playout, CPU | 40 / 45 / 51 | 36 / 41 / 45 | 214 / 238 / 244 |
| Capture ready to protected packet, elapsed | 337 / 371 / 407 | 385 / 419 / 461 | 383 / 426 / 463 |
| Combined service on the call thread, elapsed | 379 / 417 / 453 | 424 / 459 / 504 | 602 / 653 / 694 |
| Combined service through a codec worker, elapsed | 404 / 455 / 564 | 453 / 566 / 659 | 626 / 684 / 752 |

The callback that published a complete packet's PCM took 1 µs at p99 in every inline run. Combined time adds callback, sender and receiver service for one packet and excludes device buffering and network transit. The eight-track fixture wraps the same voiced payload in eight independent conference streams and excludes simulated relay processing from the client timings. Even mixing eight tracks, combined p99 was below 0.7 ms against a 10 ms device period on this machine. Moving Opus to a worker added 58–155 µs to combined p99 across these cases, so it stays on the call thread. VP8 encoding runs independently.

**Kernel receive times.** The call socket asks the kernel to stamp every datagram (`SO_TIMESTAMPNS_NEW`, Linux 5.1, else `SO_TIMESTAMPNS`). `recvmmsg()` returns the stamp in each message's control data, and `line_udp_rx_arrival()` converts it to the monotonic clock. The audio jitter buffer, the video reorder buffer, the REMB delay estimate and RTT then measure when a packet arrived, not when the call thread got to it. A stamp outside the last two seconds (a clock step) counts as now. In the network harness this made no measurable difference once the encoder thread had freed the call thread, because a packet can still not be played before the thread reads it. It costs nothing, and keeps the estimates right when the thread is held up for other reasons. `LINE_DISABLE_RX_TIMESTAMPS=1` turns the stamps off.

**The kernel's send queue.** Before each picture, the call thread asks how much the kernel still holds for the call socket (`ioctl(SIOCOUTQ)`): datagrams waiting in the qdisc, in the Wi-Fi stack's queues or in the driver for the radio. On a weak Wi-Fi uplink this queue grows long before anything is lost, and every picture sent into it only waits. From successive samples and the bytes sent in between, `call_video_local_queue()` measures what the uplink really carries. More than 60 ms of queue for 100 ms cuts the rate to 85% of that measured rate (70% when the queue is above 200 ms), at most half at once, again every 300 ms while the queue still grows, and growth pauses until it drains. The kernel counts whole socket buffers, about twice a full packet's payload, so the delay is coarse. Skipping pictures while the queue is long was tried and dropped: libvpx spends the saved budget on one large picture that refills the queue at once. `LINE_DISABLE_SEND_QUEUE=1` turns the signal off. Busy polling (`SO_BUSY_POLL`) needs `CAP_NET_ADMIN` to raise above the system default, which is 0, so it is not used.

**UDP buffers and scheduling.** The call starts with the existing 1 MiB socket buffer requests. Once an authenticated RTT sample is available, the client estimates one bandwidth-delay product per direction from bytes transferred over a one-second interval and the minimum measured RTT. It rounds up to a full send batch and caps the request at 1 MiB. Received datagrams stay in their batch slot until processed. Oversized datagrams are discarded whole. When the local send queue is temporarily full, unsent voice and video packets are dropped and the call continues. The bandwidth sample counts only bytes accepted by the kernel. Microphone packets carry DSCP EF. Video, data and signalling use AF41. Set `LINE_CALL_DSCP=0` to disable marking. The call normally uses the Linux fair scheduler. `LINE_CALL_REALTIME=1` requests real-time scheduling for network and audio deadlines, with a Realtime portal or RTKit fallback. `LINE_CALL_CODEC_NUMA=1` confines newly created software codec threads to the current NUMA node when it has enough CPUs in the process’s allowed set. Neither option requires changing system-wide settings.

**Received video on the GPU.** The Vulkan renderer can sample an imported NV12 DMA-BUF directly when the decoder and driver export a supported layout. The worker retains that surface until GPU retirement. A rejected import replays the latest picture through the I420 path. For I420 the renderer samples luma with a Catmull–Rom bicubic filter on enlarged tiles and uses bilinear sampling for chroma and unscaled or reduced tiles. The SDL fallback uses bilinear scaling. The [Wayland video benchmark](testing.md#wayland-video-benchmark-results) covers color, frame lifetime and timing.

**What was left out, and why.** Desktop NVENC has no VP8 encoder, so CUDA cannot take over encoding. NVIDIA's Tensor-core video and audio effects (Video Effects SDK, Audio Effects SDK, RTX Video Super Resolution) need proprietary SDKs, models and TensorRT. None of them was installed on the development machine (an RTX 5050 laptop with only the driver), so no benefit could be measured, and they are not used. GPU colour conversion would not pay for itself: converting a 720p camera frame on the CPU takes about 0.5 ms, less than the upload and readback would. RNNoise costs about 0.5% of one core, so a GPU denoiser would add latency without a measurable gain. Encoder frame dropping (`rc_dropframe_thresh`) made no difference in the harness and stays off. Kernel pacing (`SO_MAX_PACING_RATE`, `SO_TXTIME`) only works under the `fq` qdisc, which typical desktops do not use for Wi-Fi, so pictures still leave as one burst. Opus DTX stays off. A peer that supplies no audio reception reports keeps the initial Opus loss estimate.

**Interoperability.** Packet formats and descriptors are unchanged, and the Android packet comparisons still pass. The receiver now also sends RRR, receiver reports and REMB to the peer. How LINE for Android treats them in a one-to-one call, and whether it answers RRR or honours REMB, still needs a live test. Between two Avernella clients both work. Set `LINE_CALL_FEEDBACK=0` to stop sending RRR, receiver reports and REMB, for troubleshooting.

**Measurements.** `build/test-call-net` (`tests/call_net.c`) runs these paths over a simulated network on a virtual clock. It uses 640 × 360 VP8 at 15 fps with an 800 kbit/s ceiling and 30 s of synthetic voice. The table compares the code before this work with the current code. *Screen PSNR* compares what the viewer sees with the newest four pictures the path could have delivered, so a frozen picture scores low.

| Scenario | fps before → after | Freeze per 19 s, before → after | Screen PSNR before → after |
| -------- | ------------------ | ------------------------------- | -------------------------- |
| Clean, 30 ms | 15.0 → 15.0 | 0 → 0 ms | 45.1 → 45.1 dB |
| 5% burst loss | 3.7 → 15.0 | 15.3 → 1.4 s | 28.0 → 44.7 dB |
| Wi-Fi jitter (40 ms, reordering, 120 ms stalls every 2 s) | 7.6 → 14.8 | 10.2 → 1.0 s | 34.2 → 44.7 dB |
| 400 kbit/s bottleneck, 250 ms queue | 0.2 → 14.7 | 18.8 → 0.4 s | 13.6 → 42.6 dB |
| 1.2 Mbit/s dropping to 300 kbit/s for 7 s | 9.2 → 13.4 | 7.3 → 2.1 s | 32.7 → 36.0 dB |
| 600 ms outage | 14.4 → 14.4 | 0.8 → 0.8 s, recovery 178 ms | 44.3 → 44.2 dB |
| Mobile hotspot (70 ms, 50 ms jitter, 1 Mbit/s falling to 450 kbit/s) | 2.0 → 14.7 | 17.1 → 3.4 s | 19.9 → 41.7 dB |

The harness also measures capture-to-screen latency. A weak Wi-Fi uplink, 700 kbit/s falling to 260 kbit/s for 8 s with up to 600 ms queued in the driver and the air, compares the sender with and without the kernel's send queue:

| Weak Wi-Fi uplink | Send queue ignored | Send queue used |
| ----------------- | ------------------ | --------------- |
| Latency, 95th percentile | 570 ms | 170 ms |
| Latency, mean | 115 ms | 79 ms |
| Frame rate | 13.7 fps | 14.8 fps |
| Freeze per 19 s | 420 ms | 287 ms |
| Screen PSNR | 32.4 dB | 35.1 dB |

A 720p call, with 25 ms delay, 8 ms jitter and 0.5% loss, compares audio when the call thread encodes inline (busy 14 ms of every 33 ms, 45 ms for a keyframe every 3 s) with the encoder thread (3 ms decoding remains):

| Audio during a 720p call | Encoding inline | Encoder thread |
| ------------------------ | --------------- | -------------- |
| Mean playout delay | 120 ms | 85 ms |
| Dead air per minute | 54 ms | 0 ms |
| Concealed audio per minute | 869 ms | 290 ms |

| Audio scenario | Dead air per minute, before → after | Mean playout delay, before → after |
| -------------- | ----------------------------------- | ---------------------------------- |
| Clean | 0 → 0 ms | 66 → 63 ms |
| 5% burst loss | 235 → 0 ms | 189 → 110 ms |
| 15% burst loss | 822 → 0 ms | 171 → 109 ms |
| Wi-Fi jitter | 130 → 0 ms | 174 → 145 ms |
| Mobile hotspot | 174 → 0 ms | 145 → 117 ms |

Those figures describe the earlier jitter-buffer rollout. The current harness also injects a 3 kHz, 40 ms pilot once per second into synthetic speech and searches the decoded speaker waveform in 1 ms steps. The sound device, microphone and network all run on a virtual clock. Compare `d4aaebc` with the current audio controller using the same tests and seed:

| Scenario | Microphone-to-speaker p50 / p95 / p99 before → after (ms) |
| --- | --- |
| Direct, clean | 129 / 130 / 130 → 104 / 104 / 104 |
| Direct, 5% burst loss | 188 / 242 / 253 → 134 / 214 / 231 |
| Direct, periodic Wi-Fi stalls | 229 / 230 / 233 → 234 / 234 / 234 |
| Group, clean | 185 / 185 / 185 → 116 / 152 / 173 |
| Group, 24.2% observed burst loss | 209 / 296 / 308 → 170 / 219 / 250 |

Under group burst loss, both versions reproduced 25 of 29 probes. Speaker underruns and inserted silence fell from 12.2 to 2.8 s/min as the decoder concealed gaps at their playout deadlines. With periodic Wi-Fi stalls, direct-call concealment fell from 993 to 828 ms/min while median latency increased by 5 ms. The percentiles include only detected pilots. Run `build/test-call-net -p /net/audio --verbose` for the per-profile results, counts and modeled delays.

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

- **No video but audio works:** the peer declined the VP8 profile, or no keyframe arrived. Look for the negotiated payload and "first frame". The `offered video` or `answered video` line lists the peer's video payload maps and session features, such as `maps=2 features=0:1` for baseline VP8.
- **No audio:** check that packets authenticate. A wrong SSRC or payload type makes every packet fail SRTP.
- **The call never connects:** the relay or media address isn't reachable over UDP from your network.

## Tests

`tests/call.c` runs complete calls against a **local relay fixture**: incoming and outgoing signalling, bidirectional SRTP audio and video, media address changes, rejection of tampered and wrong-stream packets, continuing as voice when video fails, and screen sharing with and without the camera. SDL audio is replaced with `-Wl,--wrap`. `tests/call_video.c` checks VP8 encoding and decoding through EVS3 fragments and SRTP, including reordering, loss, replays and keyframe recovery, against the Android descriptor bytes. `tests/call_audio.c` covers the EAS2 framing and playout, and `tests/call_denoise.c` the noise suppressor. `tests/call_net.c` measures video and audio over the simulated bad networks in [Bad networks](#8-bad-networks).
