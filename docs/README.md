# Avernella documentation

Avernella is an independent native client for LINE personal accounts, written in C for GNU/Linux.

| Using the desktop client | Where to look |
| ---- | ---- |
| Switch between the LINE and Avernella layouts, change the language, or reduce motion | [Appearance and motion](../README.md#appearance-and-motion) |
| Check when unread badges clear or browse older messages | [In a conversation](../README.md#in-a-conversation) |
| Limit the frame rate on an older machine or check which renderer is active | [Performance](../README.md#performance) |

The developer documentation is available in two languages, with the same pages in each:

- **English:** [docs/en/](en/README.md)
- **ภาษาไทย (Thai):** [docs/th/](th/README.md)

Both cover the app (building, architecture, the user interface, testing, contributing) and LINE's protocol as this client speaks it (Thrift RPC, sign-in, sync, messages, Letter Sealing, media, friends and groups, LINE apps and calls). AI coding agents should also read [CLAUDE.md](../CLAUDE.md) at the repository root.

For installation, packages and keyboard shortcuts, see the [project README](../README.md). The protocol analysis includes the following reports.

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
