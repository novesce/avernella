# LINE 26.18.2 client-fidelity check

The supplied `LINE.apk` is a standalone Android package with SHA-256 `5432c8ad902609b4ce4773d834206da63f5c441610346bec0249235116b2e4c5`. Its manifest string pool declares version `26.18.2`. REA 6.8.0 inventoried all 13,466 central-directory file entries in parent evidence `ev_27d3ae58d7f9aa5f90a3165ee47065b8ea4ae33a8258b14e6f3935defe183e74`; the summary view is `ev_54c48e342cd3007ab0c43ab1d7ae0f1d65aee504b5ec1f2f8e4987c4983a44c0`. APK signing inspection `ev_486f29ae7a734768d1bbb7206f82540d593b277417da846b58242d3cc0091ce1` found self-consistent v2, v3 and v3.1 signing blocks. This is byte identity and internal digest consistency, not an authenticity verdict.

## Static request models

JADX 1.5.6 decompiled the following shipped classes without executing the app:

| Class | Observation relevant to Avernella |
| --- | --- |
| `iu8.ho` (`sendMessage_args`) | Field 1 is the required i32 sequence and field 2 is the Message struct. |
| `iu8.pd` (Message) | The standard serializer writes `to` (2), `createdTime` (5), `deliveredTime` (6), `hasContent` (14), `contentType` (15), `contentMetadata` (18), `sessionId` (19) and `chunks` (20) in field order when their object fields are present. Fields 5, 6, 14 and 19 are unconditional in that serializer. |
| `iu8.id` (LoginRequest) | Confirms the email-login field IDs and types used by Avernella, including `keepLoggedIn` (5), `systemName` (7), `secret` (10), `e2eeVersion` (11) and `modelName` (12). |
| `iu8.qh` (SyncRequest) | Confirms `lastRevision` (1), `count` (2), `lastGlobalRevision` (3), `lastIndividualRevision` (4), optional full-sync reason (5) and partial-sync cursors (6). Fields 3 and 4 are conditional in the model; construction behavior was not established, so no change was made from this observation alone. |

For image messages, `km8.s.j()` in `classes13.dex` builds the image record from the `contentMetadata` keys named in the `cp8.b$b` enum: `OBS_POP`, `IS_OBS_CONTENT_EXPIRED`, `DOWNLOAD_URL` and `PREVIEW_URL`. `jd8.n` loads a thumbnail from `PREVIEW_URL` when it is non-empty and from OBS otherwise, and `gf8.c` passes `DOWNLOAD_URL` with the OBS reference when saving the full picture. Official-account image messages carry these URLs, and an OBS request by message ID returns 404 for them. Avernella's `line_hosted_image_url()` follows the same preference.

The Android Message serializer exposed a concrete mismatch: Avernella previously omitted field 19 for text but included zero for other content. `line_send_sealed()` now writes byte field 19 as zero for every sealed send. This matches the generated serializer's field set and ordering; it does not make the HTTP/TLS stack byte-identical to Android or Windows.

The Android package contains desktop application-type strings, but it does not establish the Windows client's actual headers or behavior. Avernella therefore retains its existing `DESKTOPWIN 9.7.0.3556` identity instead of combining an Android version with a desktop session.

## Limits

These are static observations from the supplied bytes. No logged-in traffic was captured, no account action was performed, and LINE's anti-abuse or account-enforcement rules were not observed. Protocol fidelity can remove a malformed-request difference, but it cannot establish or guarantee a lower ban risk. Request-sequence persistence, avoiding automatic resubmission after ambiguous send failures, long-poll pacing and rate-limit handling remain separate safeguards already covered by their own fixtures.
