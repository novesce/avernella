# Letter Sealing (end-to-end encryption)

[Index](README.md) · Previous: [Messages](messaging.md) · Next: [Media](media.md)

Letter Sealing is LINE's end-to-end encryption for messages. LINE's servers carry and store sealed messages but can't read them. Everything here is in [`sealing.c`](../../src/sealing.c). The public design is in [LINE's encryption whitepaper](https://www.lycorp.co.jp/en/privacy-security/line-encryption-whitepaper-ver2.2.pdf), and the exact byte formats were cross-checked against the LINEJS interoperability implementation (credited in [analysis/portability.md](../../analysis/portability.md)).

## The idea in plain words

- Every account has **X25519 key pairs**. Each pair is a **key generation** with a number, its **key ID**. When the account creates a new pair (for example after reinstalling LINE on the phone), the old generations remain, so old messages can still be opened.
- Every device of the account needs the **private** keys. A new desktop gets them from the phone during sign-in, in an encrypted **key chain**.
- To send to a person, you combine *your* private key with *their* public key (X25519 Diffie–Hellman). Both sides arrive at the same **shared secret** without ever sending it. A per-message key is derived from it.
- A group has its own key pair, the **group key**. Its private half is encrypted separately to each member's public key, so each member can unwrap it.
- Each message names the key IDs it used, so the receiver knows which generation to pick.

## Getting the key chain

During sign-in ([QR](sign-in.md#qr-code-pairing) or [email](sign-in.md#email-and-password)) the desktop makes a fresh X25519 key pair and gives its public key to the phone. The phone answers with:

| Field | Meaning |
| ----- | ------- |
| `publicKey` | The phone's X25519 public key for this exchange (base64, 32 bytes) |
| `encryptedKeyChain` | The key chain, AES-256-CBC encrypted (base64) |
| `hashKeyChain` | A 16-byte MAC of the encrypted chain (base64, optional) |
| `keyId` | The ID of the account's active generation |

`line_import_login_keys()` does this:

1. `shared = X25519(desktop private key, phone public key)`.
2. `key = SHA-256(shared ‖ "Key")`, and `iv = fold(SHA-256(shared ‖ "IV"))`. **fold** XORs the second 16 bytes of a 32-byte hash into the first 16 (`fold_digest()`).
3. If a MAC is present (always, for email sign-in), check it: `MAC = AES-256-ECB(key, fold(SHA-256(encryptedKeyChain)))` (`chain_proof()`), compared in constant time.
4. Decrypt the chain with AES-256-CBC under `key` and `iv`.
5. The plaintext is a **Thrift Compact struct**. Its field 1 is a list of entries, each with field 2 the key ID, field 4 the 32-byte public key and field 5 the 32-byte private key.
6. For every entry, recompute the public key from the private key and require it to match. Reject duplicates, and require the active ID to be present (`key_generations()`).
7. Only when everything validates, replace `s->keys` with:

   ```json
   {"active": 7, "own": {"6": "<b64 private>", "7": "<b64 private>"}, "peers": {}}
   ```

For email sign-in, the MAC is also returned as the 16-byte **proof** sent with `confirmE2EELogin`. All intermediate secrets are wiped with `OPENSSL_cleanse`.

## When LINE rejects the sender key

When TalkService declines `sendMessage` with code 83 (`E2EE_UPDATE_SENDER_KEY`), copy your draft, sign out of Avernella, then sign in with the QR code and approve the link in LINE on your main phone. Pairing supplies the current Letter Sealing key chain. Until you sign out, Avernella keeps your session and the failed text stays in the composer. [LINE Help Center on verifying a secondary device](https://help.line.me/line/desktop/?contentId=50000087)

## Getting other people's public keys

`peer_key()` gets a contact's public key. There are two cases.

**Sending** (negotiate the current key): `negotiateE2EEPublicKey` on `/S4` with `{2: peer MID}`. The reply:

| Field | Meaning |
| ----- | ------- |
| 1 | Allowed content types (list of i32) |
| 2 | The public key: field 2 key ID, field 4 the 32 key bytes |
| 3 | Specification version. Must be 2. |

If the version isn't 2, or the content type isn't allowed, the send fails with "This contact does not accept Letter Sealing v2 for this message type". The allowed-types check applies only to inline content (text 0 and location 15). Attachments use the separate media flow.

**Reading** (the exact generation named in the message): first the cache `keys.peers["<mid>/<id>"]`, then `getE2EEPublicKey` with `{2: peer, 3: 1, 4: key ID}`. If that method isn't offered on this route, it falls back to negotiation, but accepts the answer only if it is the requested generation.

Keys are cached in `keys.peers` and saved with the session.

## Group keys

`group_private_key()` gets a group's private key.

**Sending** uses the latest group key: `getLastE2EEGroupSharedKey` with `{2: 1, 3: chat}`. **Reading** uses the generation named in the message: first the cache `keys.groups["<chat>/<id>"]`, then `getE2EEGroupSharedKey` with `{2: 1, 3: chat, 4: group key ID}`.

The reply:

| Field | Meaning |
| ----- | ------- |
| 2 | Group key ID |
| 3 | Creator's MID (who wrapped this copy) |
| 4 | Creator's key ID |
| 5 | Receiver's MID (must be you) |
| 6 | Receiver's key ID (one of your generations) |
| 7 | The wrapped group private key, 48 bytes |
| 8 | Allowed content types (latest key only) |
| 9 | Specification version, must be 2 (latest key only) |

Unwrapping: `shared = X25519(your private key [receiver key ID], creator's public key [creator key ID])`, then the same `key`/`iv` derivation as the key chain, then AES-256-CBC decryption. The result must be exactly 32 bytes. It is cached in `keys.groups`.

## Which secret a message uses

`message_shared_secret()` picks the inputs.

| Chat | Sending | Reading |
| ---- | ------- | ------- |
| One-to-one | `X25519(your private [active ID], peer public [their current ID])` | Your own messages: `X25519(your private [sender ID], peer public [receiver ID])`. Theirs: `X25519(your private [receiver ID], sender public [sender ID])`. |
| Group or room | `X25519(group private [latest group key ID], your public [active ID])` | `X25519(group private [group key ID in the message], sender public [sender key ID])` |

In a group, the "receiver key ID" field of the envelope carries the **group key ID**. `public_by_id()` returns your own public key, computed from your private key, when the sender is you.

## The message envelope

A sealed message has `contentMetadata.e2eeVersion` set to `"1"` or `"2"` (missing means 2) and exactly **five chunks** in Message field 20.

### Version 2 (what the client sends)

| Chunk | Contents |
| ----- | -------- |
| 0 | Salt, 16 random bytes |
| 1 | Ciphertext followed by the 16-byte GCM tag |
| 2 | Nonce, 12 random bytes (readers accept 1 to 32) |
| 3 | Sender key ID, 4 bytes big-endian |
| 4 | Receiver key ID (or group key ID), 4 bytes big-endian |

- `key = SHA-256(shared ‖ salt ‖ "Key")`
- Cipher: **AES-256-GCM** with that key and nonce.
- **Additional authenticated data** (82 bytes, `make_aad()`): `to` MID (33 bytes) ‖ `from` MID (33) ‖ sender key ID (4, big-endian) ‖ receiver key ID (4) ‖ version `2` (4) ‖ content type (4). The tag therefore also protects who the message is from and to, which keys were used, and what kind of content it is. A server that swapped recipients or relabeled a photo as text would break the tag.

### Version 1 (read only)

| Chunk | Contents |
| ----- | -------- |
| 0 | Salt, 8 bytes |
| 1 | Ciphertext, a multiple of 16 bytes |
| 2 | MAC, 16 bytes |
| 3, 4 | Sender and receiver key IDs |

- `key = SHA-256(shared ‖ salt ‖ "Key")`, `iv = fold(SHA-256(shared ‖ salt ‖ "IV"))`
- MAC check first: `AES-256-ECB(key, fold(SHA-256(ciphertext)))` must equal chunk 2.
- Then **AES-256-CBC** decryption.

### The plaintext

The decrypted bytes must be a single strict JSON object, valid UTF-8, with nothing after it. For text it is `{"text": "…"}`, optionally with `REPLACE` for LINE emoji. For attachments it is `{"keyMaterial": "…", "fileName": "…"}` (see [Media](media.md)). The content type passed to `line_seal_payload()` and `line_open_payload()` must match the message's field 15, because it is part of the AAD.

## The API

| Function | Does |
| -------- | ---- |
| `line_import_keys(s, meta, secret, &error)` | Import the phone's key chain (QR sign-in) |
| `line_import_login_keys(s, meta, secret, proof, &error)` | The same, also returning the proof (email sign-in) |
| `line_seal_payload(s, chat, content_type, payload, &error)` | Encrypt a JSON object for a chat. Returns the five chunks. |
| `line_open_payload(s, message, content_type, &error)` | Decrypt a raw Message. Returns the JSON object. |
| `line_seal_text()`, `line_open_text()` | Shortcuts for `{"text": …}` |

Errors:

- `LINE_OWN_KEY_MISSING`: the message used one of *your* generations that this device never received. The UI shows the "key missing on this device" notice. Nothing can fix this short of the phone sharing that generation.
- "Letter Sealing could not authenticate this message": the tag or MAC didn't match.
- "…unsupported chat type or Letter Sealing version": an unknown version, wrong content type, or a one-to-one message that is neither from nor to you.

## The shared-secret cache

Opening a long history would repeat the same X25519 computation for every message. `line_open_payload()` therefore caches shared secrets in `s->history_secrets`, a ring of 32 entries keyed by (to, from, sender key ID, receiver key ID), only **after** a message authenticated successfully. Importing a new key chain or signing out wipes the whole array. `tests/sealing.c` has a performance case (`/sealing/authenticated-history-cache`) that measures the benefit.

## What isn't encrypted

- `contentMetadata`: content type, mentions, sticker IDs, attachment object IDs, file sizes. LINE needs these for delivery and notifications.
- Announcements (stored as plain text by LINE).
- Stickers (they reference public artwork).
- Profile data, group names and member lists.

The `REPLACE` data for LINE emoji in text *is* inside the sealed payload. The attachment's key material is too.
