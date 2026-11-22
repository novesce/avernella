# Signing in and the saved session

[Index](README.md) · Previous: [Protocol basics](protocol-basics.md) · Next: [Sync](sync.md)

A desktop is a *secondary device* of a LINE account. Signing in has to produce three things:

1. an **access token** (and a **refresh token** to renew it),
2. the account's **MID**,
3. the account's **Letter Sealing key chain**, encrypted by the phone to this desktop, so the desktop can read and send encrypted messages.

Avernella supports two ways to get them: scanning a QR code, and email and password with a confirmation code. Both end with the same session saved on disk.

## The flow in the app

1. The user chooses a sign-in method on the sign-in screen. `login()` in [`ui.c`](../../src/ui.c) starts the worker (`--worker` for QR, `--worker-password` for email, see [Architecture](architecture.md#how-it-is-started)) and sends `login`.
2. The worker's `login()` in [`main.c`](../../src/main.c) opens the state directory (`line_store_open()`). If no session is saved, it runs `pair_worker()`, which calls `line_pair()` (QR) or `line_password_login()` (email).
3. While pairing, the worker sends `qr` and `pin` events, and the UI shows the code.
4. After pairing, the worker saves the session (`line_store_save()`). If saving fails, it immediately signs the new session out with `logoutV2`, because the phone has already approved a device the desktop can't keep.
5. The worker calls `getProfile` on `/S4` to check the session and learn the user's name (field 20), MID (field 1) and picture path (field 33). If the MID differs from the saved one, the session is discarded ("The account changed").
6. It replies with `{name, selfId, capabilities}`. The capabilities object tells the UI which features to offer (photos, files, calls, video calls and so on).
7. The worker arms its maintenance timer, which establishes the sync baseline (see [Sync](sync.md)).

When a session is already saved, the UI calls `login(TRUE)` at launch and step 2 skips pairing.

## QR code pairing

Implemented in `pair_with_key()` in [`pair.c`](../../src/pair.c). All calls are Compact Thrift on `/acct/lgn/sq/v1`, except the waits, which are long polls on `/acct/lp/lgn/sq/v1`.

1. **Make a key.** `line_pair()` generates an X25519 key pair with OpenSSL. The private half never leaves memory and is wiped with `OPENSSL_cleanse` at the end.
2. **Create a session.** `createSession()` with no arguments. Reply field 1 is the pairing session ID.
3. **Get the QR link.** `createQrCodeForSecure({1: {1: session}})`. Reply field 1 is a `line://` or `https://line.me/` link, field 2 the number of poll rounds, field 3 the seconds per round, and field 4 a nonce.
4. **Add the key to the link.** The client appends `secret=<base64 public key, URL-escaped>&e2eeVersion=1` to the link and emits it as a `qr` event. That URL *is* the QR code the phone scans. This is how the phone learns the desktop's public key.
5. **Wait for the scan.** `poll_approval()` calls `checkQrCodeVerified` on the long-poll path, with the pairing session ID as the `x-line-access` header and `x-lst` set to the seconds per round. Each round either succeeds or times out (`LINE_TIMEOUT`), up to the given number of rounds (at most 12, at most 120 s each, LINE for Android's own default in `dc5.p`). A round that LINE ends early, such as an immediate 408, 410 or 504, still lasts its full length before the next poll, so an early answer never becomes a rapid loop. Running out of rounds gives "The QR code expired".
6. **Check whether a PIN is needed.** `verifyCertificate` with the same arguments. A fresh pairing has no device certificate, so LINE usually answers service code 1 or 2. Then:
   - `createPinCode` returns a 4 to 12 digit PIN in field 1. The client emits a `pin` event, and the user types the PIN on the phone.
   - `poll_approval()` waits again, with `checkPinCodeVerified`.
7. **Log in.** `qrCodeLoginV2ForSecure` with the request struct extended to `{1: session, 2: "Avernella", 3: "Avernella", 4: true, 5: nonce}`. Fields 2 and 3 are the system and model names shown as the linked device's name on the phone.
8. **Read the result.**
   - Field 3 is a `TokenV3IssueResult`: access token in field 1, refresh token in 2, `durationUntilRefreshInSec` in 3 and `tokenIssueTimeEpochSec` in 6. `line_schedule_refresh()` records the renewal time.
   - Field 10 (or older forms: field 6's `e2eeInfo`, or field 6 itself) is the Letter Sealing metadata: `encryptedKeyChain`, `publicKey` (the phone's X25519 public key), `keyId` (the active generation) and `hashKeyChain`.
9. **Import the keys.** `line_import_keys()` in [`sealing.c`](../../src/sealing.c) derives the shared secret with the desktop's private key and the phone's public key, checks the key chain's MAC, decrypts it and verifies every key pair (see [Letter Sealing](letter-sealing.md#getting-the-key-chain)). If anything fails, the client calls `logoutV2` with the new token to release the half-made session, and reports the error.

The MID isn't in the pairing reply. It comes from `getProfile` afterwards.

## Email and password

Implemented in [`password_login.c`](../../src/password_login.c). These endpoints use **Thrift Binary**, not Compact (see [Protocol basics](protocol-basics.md#thrift-binary-for-email-login-only)).

1. **The credentials arrive.** The worker reads one `struct line_credentials` (email and password, up to 255 bytes each) from fd 3, then closes and wipes it (`pair_worker()` in `main.c`).
2. **Get LINE's RSA key.** `getRSAKeyInfo({1: {2: 0}})` on `/api/v3/TalkService.do`. Reply: field 1 a key identifier, field 2 the RSA modulus in hex, field 3 the exponent in hex, field 4 a session key.
3. **Encrypt the credentials.** The plaintext is three length-prefixed strings, `len(session) session len(email) email len(password) password`, each length one byte. It is encrypted with RSA PKCS#1 v1.5 padding (`encrypt()`) and hex-encoded. The modulus must be at least 1024 bits. The plaintext and the password are wiped.
4. **Make a confirmation key.** `make_pairing_key()` generates an X25519 key pair and a random six-digit PIN. The desktop's public key is encrypted with AES-256-ECB under SHA-256 of the PIN (`make_pin_secret()`). This lets the phone, once the user types the PIN, recover the desktop's public key.
5. **First `loginV2`** on `/api/v3p/rs` with request field 2:

   | Field | Value |
   | ----- | ----- |
   | 1 | 2 (login type: ID credential with E2EE) |
   | 2 | 1 (identity provider: LINE) |
   | 3 | RSA key identifier |
   | 4 | encrypted credentials (hex) |
   | 5 | true (keep logged in) |
   | 6 | "" (access location) |
   | 7 | "Avernella" (system name) |
   | 10 | encrypted desktop public key (32 bytes) |
   | 11 | 1 (E2EE version) |
   | 12 | "Avernella" (model name) |

   Service codes 18, 19, 21 and 23 mean wrong credentials. If LINE issues a token straight away (field 9) without a verifier, the client signs it out again: such a session has no encryption keys. If field 5 is 4, LINE wants another verification method, and the user is told to use the QR code instead.
6. **Show the PIN and wait.** Field 3 of the reply is a **verifier**. The client emits a `pin` event with the six digits, then `line_auth_wait()` does a JSON GET on `/LF1` with the verifier as `x-line-access`, for up to 180 seconds, until the user enters the PIN on the phone.
7. **Import the keys.** The approval's `result.metadata` holds the same key-chain fields as the QR flow. `line_import_login_keys()` imports them and also returns a 16-byte **proof**, the key chain's MAC, which shows LINE that this device decrypted the chain.
8. **Confirm.** `confirmE2EELogin({1: verifier, 2: proof})` returns a new verifier string.
9. **Second `loginV2`** with field 1 changed to 1 and field 9 set to that verifier. Reply field 9 holds the tokens (1 access, 2 refresh, 3 duration, 6 issue time), and field 10 the MID.

### Failed sign-ins and pacing

Email sign-in never retries automatically. A refusal returns to the form and waits for another explicit sign-in action. The UI keeps the delay across the fresh worker processes used by separate attempts:

- A 429 or 503 reply carries its remaining `retryAfter` seconds back to the window. Sign-in stays unavailable until that cooldown expires, and a visible countdown updates once per second.
- The first two rejected email/password attempts have no added delay. Further rejections wait 10 seconds, then 30 seconds, then 2 minutes between attempts. A successful sign-in clears this run. These delays are cautious local pacing, not limits published by LINE.
- Code 44 explains how to enable **Allow login** on the phone, code 16 recommends QR sign-in when no verification method is available, and code 22 reports a deactivated account. None of these responses is retried automatically.

## Staying signed in

- **Launch without network.** `login()` returns `LINE_RETRY` only when `transient_failure()` says a network could have caused the failure: no HTTP answer at all, HTTP 408, 429 or 5xx, or a local cooldown. The UI then keeps the session and retries (`retry_login()` in `ui.c`) after 5, 10, 20… seconds, at most every five minutes, showing "Still signed in". During a cooldown those retries fail locally without a request.
- **LINE's own refusals stop the retries.** Any TalkException, or an HTTP 4xx such as 401 or 403, ends the attempt with "LINE refused this desktop session". The session stays saved, but nothing retries until the user chooses sign in. A 401 or 403 first gets one token renewal and one more `getProfile` (`checked_profile()`).
- **Rejection.** `session_rejected()` in `main.c` treats the latch (`s->denied`), `LINE_SESSION_EXPIRED` and TalkException codes 1, 7, 8, 14 and 17 as "LINE ended this session". The worker forgets the session (`forget()`), and the UI returns to sign-in with the reason. See [Protocol basics](protocol-basics.md#when-line-refuses-the-session).
- **Renewal** is described in [Protocol basics](protocol-basics.md#token-renewal).
- **Another desktop signing in** may make LINE end this one. That arrives as a rejection on the next request or revision check.

## Signing out

**Sign out** sends `logout`. The worker calls `logoutV2` on `/AS4` (no arguments) to end the linked-device session at LINE. If that succeeds, or fails with a rejection (the session was already dead), it removes the local session with `line_store_forget()`. The UI closes the browser helper and resets every module.

## The session file

[`state.c`](../../src/state.c) stores one JSON object in `$XDG_STATE_HOME/avernella/session.json`:

```json
{
  "access": "<access token>",
  "refresh": "<refresh token>",
  "mid": "u0123456789abcdef0123456789abcdef",
  "refreshAt": 1766000000,
  "keys": {
    "active": 7,
    "own":    {"6": "<base64 private key>", "7": "<base64 private key>"},
    "peers":  {"u…/12": "<base64 public key>"},
    "groups": {"c…/3": "<base64 group private key>"}
  },
  "seq": 1234
}
```

| Key | Meaning |
| --- | ------- |
| `access`, `refresh` | Tokens. Printable ASCII, at most 16 KiB each. |
| `mid` | Your MID. Missing in sessions saved before the first `getProfile`. |
| `refreshAt` | Unix time when LINE wants the token renewed. 0 or missing means "renew now". |
| `keys.active` | The key ID of your current Letter Sealing generation |
| `keys.own` | Every generation of your private keys the phone shared, by key ID |
| `keys.peers` | A cache of other users' public keys, keyed `<mid>/<key id>` |
| `keys.groups` | A cache of unwrapped group keys, keyed `<chat>/<key id>` |
| `seq` | The highest request sequence number that may have been used |

`load()` rejects the file unless it is a regular file owned by you, not readable by group or others, between 1 byte and 8 MiB, valid JSON with a non-empty `access`, an object `keys` and an integer `seq` from 0 to 2³¹−1. The buffer holding the file is wiped after parsing.

`line_store_save()` writes `session.new` with `O_EXCL | O_NOFOLLOW` and mode 0600, `fdatasync`s it, renames it over `session.json` and `fsync`s the directory. If the JSON would exceed 8 MiB, it first drops the `peers` and `groups` caches, which can be fetched again, and keeps your own keys and tokens, which can't.

The worker re-saves the session whenever `s->token_changed` is set: after pairing, a renewal, a rotated token or a new renewal schedule.

### The request sequence

Every state-changing RPC carries a request sequence number (`reqSeq`), and LINE expects them never to repeat for a device. `line_store_sequence()` hands out numbers from a reserved range:

- `s->seq` is the last number used, `s->seq_reserved` the end of the range saved on disk.
- When the range is used up, it reserves 32 more and **saves the file before returning** the next number.
- On restart, `seq` is loaded as the saved high-water mark, so the whole previously reserved range counts as used.

`dispatch()` in `main.c` calls `reserve_sequence()` before each mutating request, then decrements `s->seq` so the sending function's own `++s->seq` lands on the reserved number. Functions that take `reserved_seq` as an argument use it directly.

Why so careful? A request that times out may still have reached LINE. Reusing its number could make LINE treat a new message as a duplicate. When the sequence reaches 2³¹−1, the user must pair again.

### Security

The session file contains the tokens and Letter Sealing private keys **in plain text**, protected only by file permissions and the 0700 directory. Anyone who can read it can act as this linked device and read encrypted messages. The worker holds an exclusive `flock` on the directory while it runs, so two Avernella instances can't use one session.
