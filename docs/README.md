# Avernella documentation

Avernella is an independent native client for LINE personal accounts, written in C for GNU/Linux.

The developer documentation is available in two languages, with the same pages in each:

- **English:** [docs/en/](en/README.md)
- **ภาษาไทย (Thai):** [docs/th/](th/README.md)

Both cover the app (building, architecture, the user interface, testing, contributing) and LINE's protocol as this client speaks it (Thrift RPC, sign-in, sync, messages, Letter Sealing, media, friends and groups, LINE apps and calls). AI coding agents should also read [CLAUDE.md](../CLAUDE.md) at the repository root.

Using the app (features, shortcuts, packages) is covered in the [project README](../README.md). The detailed reverse-engineering evidence lives in [`analysis/`](../analysis):

- [Android build analysis](../analysis/portability.md): what the LINE 26.16.0 APK contains, and the evidence for messages, Letter Sealing, attachments, emoji and groups.
- [Video call wire format](../analysis/video-call-wire.md): PLANET, SRTP, PDTP and screen sharing.
- [LIFF](../analysis/liff.md): LiffService field IDs, the token handoff and the native bridge.
- [Windows and Mac feature comparison](../analysis/desktop-features.md): what LINE's desktop apps do and what this client covers.

## Older pages

These single pages came before the split documentation. Their content now lives in the pages above, and each one points to where its sections went:

- [architecture.md](architecture.md)
- [line-protocol.md](line-protocol.md)
- [development.md](development.md)

Avernella is an independent project. LINE is a trademark of LY Corporation.
