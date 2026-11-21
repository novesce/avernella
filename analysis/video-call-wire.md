# Direct video calls on LINE Android 26.16.0

The ARM64 split of the supplied APKM contains `libandromeda.so`, SHA-256 `27bed6d41ad1ba14b7f81dc2ad197b4872261e5a3842c3457f3a9e69bb32f8db`. REA 6.9.0 recorded its ELF exports as `ev_c3a9a101a96576333422bb07c50b42aeb7aa880bf669af17234fae6327da111f`. The addresses below are ELF virtual addresses in that exact file. The investigation used synthetic inputs and aggregate diagnostics from a live call to the official mobile app.

## Negotiation

The protobuf-c descriptors in the shipped library retain field names, numbers and cardinalities. The outer `cc_msg` has update request field 15 and update reply field 16. The update request carries offer bytes in field 1. The reply carries its answer in field 4. The standard call path, `ampCallStdVideoStart` → `ampCallVideoStart`, configures a stream and sends a call update at `0x491320` and `0x491518`. Direct Planet initiation uses `ampCallPlnMakeCall` → `jup_call_initiate_call` at `0x460de4`. A separate explicit camera start, `ampCallPlnVideoStart`, invokes `jup_call_media_start` with video media kind 2 at `0x467a0c`. These paths leave open whether an already-active initial video session needs any subsequent control message.

| Shipped descriptor | Fields used for direct media |
| --- | --- |
| `huygens_message` at `0xc39ac8` | 1 repeated named sessions, 2 default crypto, 3 default features |
| `huygens_named_session` at `0xc39a08` | 1 head, 2 RTP, 3 FEC, 50 session crypto, 51 repeated session features |
| `huygens_session_head` at `0xc39468` | 1 name, 2 direction, 3 initial state, 4 receive kbps, 5 receive fps, 6 receive resolution, 50 repeated payload map |
| `huygens_trans_rtp` at `0xc395d0` | 1 RTP payload type, 11 local SSRC, 61 remote SSRC, 12 and 62 additional typed SSRCs |
| `huygens_xfeature` at `0xc39938` | 1 feature ID, 2 version |
| `cc_conn_req` at `0xc2bb40` | 1 answer, 2 media channel ID, 101 media address, 103 public UE address |
| `cc_conn_rsp` at `0xc2bc38` | 4 media channel ID, 101 media address, 102 public UE address |

In a live call, the official Android app answered a Linux voice call with a `V` session whose initial state was 0 and which had no session features. The session head kept VP8 payload map 2 and the transport carried both SSRCs. When the tablet's camera turned on, the peer sent an MC stream-control START for media kind 2 on that session. Linux therefore treats an inactive VP8 session without features as baseline VP8 that can start later.

Session head field 6 uses the `resolution_type` protobuf enum. Its value table in the shipped descriptor gives 0 `THUM`, 1 `QVGA`, 2 `VGA`, 3 `HD_FHD` and 4 `UHD`. When a Linux offer requested class 2, the tablet's answer advertised 400 kbps, 24 fps and class 2. Linux now requests class 3 at 30 fps and sizes its outgoing picture from the peer's advertised limits.

When the tablet turned its camera on during the call, it first sent an MC data request with a type-2 stream report. That report listed its transmit video SSRC as running. Linux sends the same report before each mid-call camera change, because the peer otherwise keeps the earlier paused state.

The Linux client offers the bidirectional `V` stream with initial state 1 and the first payload map set to 2. In `huy_pmap_get_enum_value` at `0xa199f4`, 2 names `normal-video`, 3 names `avc-video`, 4 and 5 name EVS3 software/hardware SVC, 7 names SVC and 10 names AV1. The `_huy_pmap_create` table at `0xa1910c–0xa1912c` assigns `normal-video` the native codec name `evs3` and a 90,000 Hz clock. At `0x8a4db8–0x8a4e60`, the engine converts this string to codec 10, which `vns_video_codec_type_get_name` identifies as `EVS3_VP8`. The separate conference `video_codec_type` enumeration uses different numbers.

The exported session feature constants give ID 0 for VP8, 1 for AVC, 2 for packet-format version and 3 for AV1. The video feature parser at `0x640ca4` interprets VP8 version 1 as baseline and version 2 as baseline plus VP8A. The native feature intersection at `0xa16154` selects the lower version for matching IDs, and the packet-version parser at `0x640e7c` returns zero when the feature is absent. The Linux client advertises VP8 version 1, reads repeated payload maps, and accepts a baseline VP8 selection from a peer that also supports VP8A. It uses the finalized answer's RTP payload type and direction-specific SSRCs. The Android audio-call capture advertises an inactive video stream at PT 97, 800 kbps, 24 fps in the offer, and 10 fps in the answer. Those values provide historical defaults. LINE’s public SDK specification documents hardware H.264 and fallback VP8 for one-to-one Android, Windows and macOS calls. [Original offer capture schema](https://github.com/evex-dev/linejs/blob/v3.6.5/packages/linejs/client/features/call/planet/schema.ts) · [LINE Planet codec specification](https://docs.lineplanet.me/overview/specification/call-quality)

## Media packets

The node initialization at `0x7ee9b8–0x7eea20` names its two modes `Planet` and `NonPlanet`. Its caller at `0x7eeb10–0x7eeb50` selects factory format 1 or 2 in Planet mode and format 3 in NonPlanet mode. The factory at `0x7f1b0c` routes codec 10 and format 1 to the EVS3 VP8 packetizer. Format 3 reaches the standard VP8 packetizer. For base-layer Planet RTP, the writer at `0x7f4658` generates an EVS3 picture descriptor and the code at `0x7f469c–0x7f46bc` adds a three-byte length to the first fragment. The receiver independently reads the length at `0x7f000c–0x7f0024`.

With picture ID `0x1234`, resolution class 1 and no rotation or optional extensions, the Android serializer and parser produce these descriptor shapes. The table ends before the first fragment’s three-byte length.

| Fragment | Bytes |
| --- | --- |
| Keyframe start | `ba 12 34 00 22` |
| Delta frame start | `fa 12 34 00 03 01 00 02` |
| Middle | `80 12 34` |
| End | `84 12 34` |

Flag bit 3 marks the start and bit 2 marks the end. A single-fragment keyframe starts with `be`. The picture ID has 16 bits, big-endian. For an encoded frame of N bytes, the 18-bit length carries N + 3 as successive groups of 2, 8 and 8 bits, least significant group first. CPU emulation of the ARM64 `vns_video_packer_evs3_helper_write_pd_from_frame` at `0x7f2e64` generated reference descriptors. Running `evs3_pd_parse` at `0x8ba44c` independently recovered all four lengths, the picture ID, the keyframe flag and the resolution from the synthetic vectors.

When bit 5 is set, the byte following the picture ID carries layer and reference flags. The native parser extracts those fields at `0x8ba548–0x8ba584`. A live official-peer call exposed the consequence of incorrectly requiring a zero byte here. Packets authenticated, but every frame start was discarded. With the restriction removed, the first five seconds of a subsequent call decoded 75 pictures from 249 authenticated packets. A first-frame event confirmed delivery to the window.

When bit 1 is set, the orientation byte follows the layer and reference fields. Its two high bits select 0°, 90°, 180° or 270°. The native parser reads them at `0x8ba5e8–0x8ba600`, and the rotation names are indexed by `vns_video_rotation_type_get_name` at `0x8a3218`. The Linux display uses that angle for the complete picture and keeps the texture when the angle changes.

The RTP endpoint is supplied by connection signaling. The `V` session carries the payload type and direction-specific SSRCs. `jup_media_apply_huygens` chooses the transmitting and receiving SSRC according to the negotiated entity's role at `0x64b90c` and `0x64ba6c`. In the final answer, field 11 gives the callee’s transmit SSRC and field 61 gives the caller’s transmit SSRC. At `0xc41ab0` the MC `strm_spec` groups local receive streams in field 1 `strms`, with `retx_rx_attr`, and local transmit streams in field 6 `tx_strms`, with `retx_tx_attr`. At `0xc415c0`, `strm_state` field 1 is `paused`, a Boolean, and field 2 is a numeric code. Network address fields are carried by `planet_addr`. The available evidence shows a shared media address and leaves per-route relay behavior and SRTCP feedback timing to be confirmed with live endpoints.

The Android caller creates a primary media channel on the active session at `0x55579c–0x5557b4`. The channel constructor allocates a fresh source ID at `0x68f6f4–0x68f70c`. The caller reads that ID and supplies it to `pln_cassini_send_conn_rsp` at `0x55616c–0x5561c0`. Linux now advertises its own media channel ID for outgoing video calls. The local relay fixture uses separate signaling and negotiated media ports and waits for a matching stream-control reply on the media port before sending a portrait frame.

LINE documents authenticated ephemeral P-256 key exchange for direct calls, separate directional audio and video key material, and the AES-CM-128/HMAC-SHA1-80 SRTP suite. The Linux transport keeps separate inbound and outbound video SRTP contexts with the negotiated SSRC. [LINE Encryption Overview v2.2, pp. 8–9](https://www.lycorp.co.jp/en/privacy-security/line-encryption-whitepaper-ver2.2.pdf)

The shipped Huygens payload-map table at `0xa190c4` names the normal-audio codec EAS2. Its receiver at `0x805060` handles a single frame with an activity bit and grouped CELT frames with an activity bitmap. For mixed-TOC groups, the loop at `0x805418–0x805438` reads additional TOCs until bit 0 is set. The Linux receiver authenticates SRTP against the negotiated audio payload type and SSRC, then normalizes the EAS2 frame headers for Opus decoding. The audio fixtures cover constant and variable frame lengths and groups with different TOCs.

## Explicit stream controls and feedback

For an explicit Planet media start, `jup_media_start` at `0x561440` builds a stream control request and sends an MC data request. The native `mc_msg` descriptor assigns `data_req` field 16. The `mc_data_req` payload at `0xc26748` has fields 1 `src_type`, 2 `dst_type`, 3 `dispatch_id` and 4 `data`. The native send wrapper copies 32 bytes from `0x2bb550` into its argument block. Its first three little-endian 32-bit values are 0, 0 and 2.

The serializer at `0x614b68` writes a big-endian header with 16-bit values 1, 0 and body length, followed by two reserved bytes. The body writer at `0x6134d0–0x6135e4` writes the 16-bit action and 16-bit SSRC count, 32-bit media kind and 32-bit code, then one 32-bit entry per SSRC. An optional sender string has a 16-bit byte length including its NUL terminator. The native action table at `0x2bd818` gives 1 START, 2 STOP, 3 PAUSE and 4 RESUME. The explicit start pathway selects media kind 2 for video and code 0. Live verification is needed to establish when a relay expects these controls for an initial active video session.

The shipped `vns_fci_fmt_name` at `0x8a2de0`, executed offline with all packet type/format combinations, identifies payload-specific feedback format 16 as PEI and 17 as FIR for RTCP packet type 206. The FIR writer at `0x8a0aac` creates a 28-byte control packet beginning `91 ce 00 06`, followed by the local and media SSRCs and a 16-byte control block. The engine calls the FIR parser at `0x7c1acc`. An official-peer exchange is still needed to establish feedback timing and how this control travels on the selected route.

## Local checks

`build/test-call-video` encodes a synthetic I420 image with libvpx, authenticates fragmented EVS3 video with libsrtp2, reverses packet arrival order, and checks the decoded I420 output. It independently decrypts the wire packets to check Android descriptor bytes and declared sizes. Cases also check altered and replayed packets, loss, keyframe recovery, frame pacing and RTP negotiation. The relay fixture in `build/test-call` checks authenticated media address changes, including rejection of tampered and wrong-stream packets, and continuing an audio call when video negotiation fails. A live official-peer call is still needed to validate relay acceptance, peer updates, feedback timing and observed video quality.

## Screen sharing (presentation)

LINE calls a screen share a "presentation". In `libandromeda.so`, it is media kind 4, `DATA` (`jup_media_kind_get_name` at `0x5fdd58`), with data service type 1, `PRESENTATION` (`jup_data_svc_type_get_name` at `0x5fddbc`). It is not a second RTP video stream. It travels over PDTP inside the `D` Huygens session, which the offer already carries (payload map 6, xfeature 0 at version 2). `ampPlnIsSupportPresentationSess` at `0x475ee4` reports support when bit 2, `DATA`, of the negotiated media mask is set. The tablet in the live tests ran LINE for Android 26.18.2. Its `libandromeda.so`, pulled with `adb`, keeps the same element encoders at shifted addresses, so the 26.16.0 addresses below still apply.

### Keys and transport

PDTP uses the audio and video key derivation with the label `DATA`, the name of media kind 4 (`jup_pdtp_trans_set_crypto`, inner function at `0x655284`). In live calls this decrypted every `D` packet from the tablet: PT 98, with SSRCs 122 and 222 as given in the answer.

The RTP payload is a PDTP packet with these parts (`vns_pdtp_packet_build` at `0x826604`, parser at `0x823fec`):
- The high two or six bytes of a QUIC varint packet number. Its low 16 bits travel as the RTP sequence number.
- A NUL-terminated service name: empty for transport elements, `LINE` for the stream the app opens on every call, `PLANET` for presentation.
- A big-endian 16-bit bitmap of element types, with bit N−1 for type N.

One group per present type follows, from type 15 down to 1. Each group starts with a varint length. Types 1–5, 12 and 13 then carry a varint count. For those counted groups, the engine adds the size of the length varint to the length. A group of 64 bytes or more therefore declares one byte more than it holds, and the official parser walks it by count. The types are:
- 1 `X_STRM_DATA`: stream data
- 2 `R_STRM_CLOSE`: closes a stream
- 3 `R_STRM_CTRL`: stream control
- 4 `R_STRM_OPEN`: opens a stream
- 5 `R_STRM_RESET`: resets a stream
- 9 `R_INCLUDE_RETRANS`: names the original packet number a retransmission repeats
- 10 `R_SET_MAX_ACK_DELAY`: sets the maximum ACK delay
- 11 `R_SWITCH_ACK_ALL_STRM`
- 12 `R_FREQ_MAX_RECVABLE_BYTES`: asks for a window update
- 13 `N_FUP_MAX_RECVABLE_BYTES`: window update
- 15 `N_ACK`

| Element | Fields |
| --- | --- |
| ACK | Largest number, reason, late time when the reason is 5, delay, pair count, then (gap, count) pairs walking down from the largest |
| STRM_OPEN | Stream, flags, start offset |
| Window update | Stream, maximum receivable offset (327680 until updated) |
| STRM_CTRL | Stream, direction byte (2 from a receiver), offset, control. Only the `_USER` variants carry more fields. |
| STRM_DATA | Stream; chained flag bytes (bit 7 continues the chain); one extension per set bit; offset; length; bytes |

In the first STRM_DATA flag byte:
- Bits 0x40 (end-to-end latency), 0x20 and 0x01 (the frame's source time in nanoseconds) carry a length-prefixed varint.
- Bits 0x10, 0x08, 0x04 and 0x02 carry one varint each.
- 0x08 marks the first chunk of a message and 0x04 its last.

The controls are 1 `PAUSE`, 2 `RESUME`, 6 `REQ_PAUSE` and 7 `REQ_RESUME`, and 8–12 are user variants.

On every call the tablet opens stream 110 under the service `LINE` and sets a maximum ACK delay of 200 ms. It retransmits these until they are acknowledged, so a client has to ACK PDTP.

### Presentation streams

A sender opens `PLANET` streams 3 and 4 with flags `0x12` (`0x5f1750–0x5f17a0`):
- **Stream 3** carries commands from `jup_presn_ctrl_send_cmmd` at `0x65c250`: a varint command, the media as a one-byte varint, and a varint release code for STOP. START VIDEO is `01 01`. The tablet ended its share with `02 01 33`.
- **Stream 4** carries one EVS3 VP8 fragment per PDTP message. Start fragments differ from the RTP form: layer byte `0x10`, the delta references `03 01 00`, then the resolution and key byte with its extension bit set. Two extensions follow, type 5 with a two-byte encode delay in milliseconds and type 9 with the four-byte frame size plus three. The three-byte length field is absent. Middle fragments begin `80` and end fragments `84`, each followed by the picture ID.

A tablet keyframe of 6210 bytes arrived as six 888-byte fragments and an 882-byte end. The tablet shared its 800 × 1340 screen at about four pictures a second, with a keyframe every three or four.

The app also announces a share as feature-share item 16 in a `cc_msg` `big_data_req` (field 27, reply `big_data_rsp` 28). Its content is message `{1: 16, 2: operation, 3: {1: 1, 2: mid, 3: 0, 4: zlib data, 5: 1}}`, where the data is `{"s":<uuid>,"m":"s","p":{"t":<start ns>,"p":true}}`. Operation 1 withdraws it. The tablet also uses `big_data_req` for `{"mic-st":true}`. On LINE Android 26.18.2 the request ID was `0x2155`, and replies use the request ID plus `0x100`.

On Android, `bv7.q` computes the share support state from `isSupportPresentation`, the native check above, and `isSupportVideo`, true for an audio-video call with a video control. `bv7.l.n` starts a presentation when presentation is supported. Otherwise it falls back to the ordinary video stream. The control sits under the call's Watch together menu, on the Screen share tab.

### Live results

- **Receiving: verified.** In calls the tablet placed, Linux decoded the tablet's share and showed it as a LIVE tile.
- **Sending, tablet-placed calls: partly working.** The tablet acknowledged every packet of a Linux share and widened its window to 4 MB. It opened its "…'s screen" view after the feature-share announcement, but showed a loading spinner. The tablet sent `REQ_PAUSE` on stream 4 at offset 0 right after the open. The client now answers `PAUSE`, stops sending and resumes with a keyframe on `REQ_RESUME`. That path has not yet been seen to complete live.
- **Sending, Linux-placed calls: not working.** The tablet received no `D` packet from Linux, including the ACKs for its stream 110. It retransmitted its open for the whole call, although `D` packets from the tablet arrived. Listing the data stream in the stream report and sending a stream-control START for media kind 4 did not change this. The cause is still open.

Outgoing shares now use the negotiated call video stream by default when it is available. The tablet sees the screen in the sender's video tile. Local relay fixtures cover both camera-on and camera-off calls, including repeat delivery when the portal stops supplying frames and restoring the previous camera state after sharing. Live tablet verification of this path is pending. The native PDTP sender remains selectable for testing.
