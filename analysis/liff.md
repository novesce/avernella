# LIFF in LINE's clients

How LINE 26.16.0 for Android (`jp.naver.line.android_26.16.0`, `base.apk`) and the public LIFF SDK 2.32 (`https://static.line-scdn.net/liff/edge/2/sdk.js`) open a LIFF app. The class names below are R8-obfuscated names from that APK. The Thrift structs are in package `lf0`. Field IDs come from each struct's static `TField` table.

## Recognizing a LIFF link

`pf0/c` matches `liff.line.me` and `miniapp.line.me`, and `(line|lineb)://(.*)`. A LIFF ID is a channel ID, a dash and a suffix, such as `1661444788-rnvmoOo8`. Anything after the ID (path, query, fragment) is the deep link.

## LiffService on `/LIFF1`

`issueLiffView(1: LiffViewRequest request) → LiffViewResponse` throws `LiffException` (1) or `TalkException` (2). The client in `sb5/c` sends it on `/LIFF1`.

`LiffViewRequest` (`lf0/n1`):

| ID | Name | Type |
| -- | ---- | ---- |
| 1 | liffId | string |
| 2 | context | LiffContext |
| 3 | lang | string |
| 4 | deviceSetting | LiffDeviceSetting (optional) |
| 5 | msit | string |
| 6 | subsequentLiff | bool |
| 7 | domain | string |
| 8 | interWindowSession | string |
| 9 | requestedUrl | string |

`LiffContext` (`lf0/s`) is a union: `none` (1, empty `LiffNoneContext`), `chat` (2, `LiffChatContext { 1: chatMid }`), `squareChat` (3, `{ 1: squareChatMid }`). `LiffDeviceSetting` (`lf0/t`): `videoAutoPlayAllowed` (1, bool), `advertisingId` (2), `inAppPurchaseAvailable` (3, bool).

`LiffViewResponse` (`lf0/o1`), in part:

| ID | Name | Type |
| -- | ---- | ---- |
| 1 | view | LiffView |
| 2 | contextToken | string |
| 3 | accessToken | string |
| 4 | featureToken | string |
| 5 | features | list<i32> (`lf0/e0`) |
| 6 | channelId | string |
| 7 | idToken | string |
| 8 | scopes | list<string> |
| 11 | subLiffView | |
| 13 | accessTokenExpiresIn | i64 |
| 15 | liffId | string |

`LiffView` (`lf0/m1`): `type` (1, `FULL`, `TALL` or `COMPACT`), `url` (2, the endpoint), `titleTextColor` (4) and other colors, `appName` (22) and more.

`LiffException` (`lf0/z`): `code` (1, `LiffErrorCode`), `message` (2), `payload` (3). `LiffErrorCode` (`lf0/u`): INVALID_REQUEST 1, UNAUTHORIZED 2, CONSENT_REQUIRED 3, VERSION_UPDATE_REQUIRED 4, COMPREHENSIVE_AGREEMENT_REQUIRED 5, SPLASH_SCREEN_REQUIRED 6, PERMANENT_LINK_INVALID_REQUEST 7, NO_DESTINATION_URL 8, SERVICE_ALREADY_TERMINATED 9, ID_FEDERATION_REQUIRED 10, SERVER_ERROR 100. The payload union (`lf0/x`) holds `consentRequired` (3, `{ 1: channelId, 2: consentUrl }`), `permanentLinkInvalidRequest` (4, `{ 1: liffId, 2: fallbackUrl }`) and `idFederationRequired` (5, `{ 1: useCaseCode, 2: cancelUrl }`). On CONSENT_REQUIRED, `we0/m` loads `consentUrl` in the LIFF web view. On PERMANENT_LINK_INVALID_REQUEST, it loads `fallbackUrl`.

Other LiffService methods in the same client: `issueSubLiffView`, `getLiffViewWithoutUserContext`, `issueLiffAppParams`, `getReturnUrlWithRequestTokenForMultiLiffLogin`, `getWebLoginDisallowedUrlForMultiLiffLogin`.

## Handing the view to the page

`we0/r1` reads `contextToken`, `accessToken`, `featureToken`, `channelId` and `idToken` from the response. `pf0/b` names their URL parameters: `context_token`, `access_token`, `feature_token`, `id_token`, `client_id`, `mst_challenge`, `mst_verifier`, `msit`, `liff.provider_session_id`. `pf0/c.a` builds the page address from the endpoint: the deep link's path goes below the endpoint path, its query parameters are appended, and the fragment is kept.

The SDK's `liff.init()` reads the tokens from `location.hash` (`ke(window.location.hash)` with the keys above), stores them in `sessionStorage`, and strips them from the address.

`liff.isInClient()` is true when the user agent matches `/Line\/\d+\.\d+\.\d+ LIFF/`, or when it matches `/Line\/\d+\.\d+\.\d+/` and the hash has `access_token=`, or when an earlier page in the session set `LIFF_STORE:isInClient`. Pages that say "this link only works in LINE" test this. `liff.closeWindow()` navigates to `liff://close` on newer clients, posts `closeWindow` through `window._liff.postMessage(action, featureToken, callbackId, json)` on older ones, and calls `window.close()` without a native bridge. `liff.getOS()` returns `web` for a Linux user agent.

## Avernella

`src/liff.c` sends `issueLiffView` with the LIFF ID, the open chat as context (or `none`) and the locale as `lang`, then builds the page address as above with the tokens in the fragment. `avernella-liff` (`src/liff_browser.c`) loads it with the user agent `… Line/9.7.0 LIFF`, closes on `liff://close` and `window.close()`, and hands LIFF and `line://` navigations back to the client for a new view. `liff.init()` first waits for the native bridge when the user agent's LINE version is 9.5.0 or newer: it resolves at once if `window._liff.features` exists, and otherwise listens for a `ready` `liffEvent` that only LINE's app sends, so a page without the bridge stays on its loading screen. `avernella-liff` defines `window._liff = { features: [], postMessage }` at document start in the page's world. `postMessage(action, featureToken, callbackId, json)` reaches the browser through a WebKit script message handler. It closes the window on `closeWindow`, loads `openWindow`'s `url` (or hands it to the desktop when `external` is true), and answers everything else with a `liffEvent` whose `detail` is `{ type, callbackId, error: { code: "FORBIDDEN" } }`, so SDK calls such as `scanCodeV2` reject instead of waiting.
