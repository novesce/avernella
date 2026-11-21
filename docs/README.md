# Avernella documentation

Avernella is an unofficial native client for LINE personal accounts, written in C for GNU/Linux. These pages explain how LINE's service works and how this app talks to it.

| Page | What it covers |
| ---- | -------------- |
| [Architecture](architecture.md) | The processes and threads, how they talk, where each feature lives in `src/`, saved state, and the Linux facilities that keep it fast and light |
| [How LINE works](line-protocol.md) | LINE's servers, Compact Thrift RPC, sign-in, sync, messages, Letter Sealing, media, stickers and emoji, official-account messages, LINE apps (LIFF) and calls |
| [Development](development.md) | Building, build options, tests, code style, environment variables, debugging and how the protocol was reverse engineered |

Using the app (features, shortcuts, packages) is covered in the [project README](../README.md). The detailed reverse-engineering evidence lives in [`analysis/`](../analysis):

- [Android build analysis](../analysis/portability.md): what the LINE 26.16.0 APK contains, and the evidence for messages, Letter Sealing, attachments, emoji and groups.
- [Video call wire format](../analysis/video-call-wire.md): PLANET, SRTP, PDTP and screen sharing.
- [LIFF](../analysis/liff.md): LiffService field IDs, the token handoff and the native bridge.
- [Windows and Mac feature comparison](../analysis/desktop-features.md): what LINE's desktop apps do and what this client covers.

Avernella is an independent project. LINE is a trademark of LY Corporation.
