# LINE apps (LIFF) and the in-app browser

[Index](README.md) · Previous: [Friends, profiles and groups](social.md) · Next: [Calls](calls.md)

A **LIFF app** (LINE Front-end Framework) is a web page that runs *inside* LINE with the user's LINE identity: coupons, member cards, reservations, mini apps. Official accounts link to them from Flex cards and rich messages. Many of them refuse to work outside LINE ("this link only works in LINE"). Avernella opens them the way LINE's phone app does: it asks LINE for a *view*, hands the page LINE's tokens, and runs it in a browser window that looks like LINE to the page.

The full field tables from the Android app and the LIFF SDK are in [analysis/liff.md](../../analysis/liff.md).

## The pieces

| Piece | Process | File |
| ----- | ------- | ---- |
| Recognizing LIFF links, user agents, browser protocol | both | [`liff_url.c`](../../src/liff_url.c), [`liff_url.h`](../../src/liff_url.h) |
| Asking LINE for a view and building the page address | worker | [`liff.c`](../../src/liff.c) |
| Deciding where a link opens, starting and supervising the browser | UI | [`ui_liff.c`](../../src/ui_liff.c) |
| The browser window, the native bridge, the sandbox and cgroup | `avernella-liff` | [`liff_browser.c`](../../src/liff_browser.c) |

## Opening a link

`ui_liff_open_link(url)` is called for every link a user activates in a Flex card or rich message:

1. **A LIFF link**: `line_liff_url()` recognizes `https://liff.line.me/<liffId>…`, `https://miniapp.line.me/<liffId>…`, `https://line.me/R/app/<liffId>…` and `line://app/<liffId>…`. A LIFF ID is a channel ID, a dash and a suffix, such as `1661444788-rnvmoOo8`. Everything after it (path, query, fragment) is the *deep link*. The UI sends `liffView {url, chat}` to the worker, and the reply becomes an `L` message to the browser.
2. **Any other HTTPS link** goes to the browser as a `W` message and opens with LINE's in-app user agent.
3. **No browser helper installed:** the link opens in the system browser (`SDL_OpenURL()`).

## Asking LINE for a view

`line_liff_view()` in the worker calls `issueLiffView` on `/LIFF1`:

```
issueLiffView({1: LiffViewRequest {
    1: liffId,
    2: context  — {2: {1: chat MID}} when opened from a chat, else {1: {}} (none),
    3: lang     — the desktop locale, such as "en_US" or "th_TH" (`liff_lang()`)
}})
```

The reply (`LiffViewResponse`) gives:

| Field | Meaning |
| ----- | ------- |
| 1 | The view: field 1 the size (`FULL`, `TALL` or `COMPACT`), field 2 the endpoint URL, field 22 the app name |
| 2 | Context token |
| 3 | Access token for the app |
| 4 | Feature token |
| 6 | Channel ID |
| 7 | ID token |

**Building the page address.** `line_liff_launch_url()` starts from the endpoint (HTTPS only), appends the deep link's path below the endpoint's path and its query parameters, and puts the tokens in the **fragment**:

```
https://app.example/path/deep?x=1#context_token=…&feature_token=…&access_token=…&id_token=…&client_id=…
```

The LIFF SDK's `liff.init()` reads the tokens from `location.hash`, stores them in `sessionStorage` and removes them from the address. Fragments are never sent to the web server, so the tokens only reach the page's own JavaScript.

**When LINE says no.** A `LiffException` (code in field 1, payload union in field 3) is handled in `exception_page()`:

| Code | Meaning | Client action |
| ---- | ------- | ------------- |
| 3 CONSENT_REQUIRED | The user must agree to the app's permissions first | Open the consent page (payload 3, field 2) in the same window |
| 7 PERMANENT_LINK_INVALID_REQUEST | The deep link is no longer valid | Open the fallback page (payload 4, field 2) |
| Others | Unauthorized, update required, service terminated and so on | Show a readable reason (`refusal()`) |

The worker replies `{url, size, title}`.

## The browser helper

`avernella-liff` is a separate program. It is built with **WPE WebKit** when available (preferred on Wayland), and as `avernella-liff-gtk` with **WebKitGTK 6.0** as a fallback (and on X11). Both come from the same `liff_browser.c`; `LINE_LIFF_GTK` selects the GTK build.

### Starting and stopping

`ui_liff.c`:

- finds the helper beside `avernella` or in `../lib/avernella`, choosing WPE when a Wayland display is present;
- starts it on the **first** link with `pidfd_spawn()`, passing a `SOCK_SEQPACKET` socket as fd 3 and the parent PID as the only argument;
- supervises it through the pidfd, and runs a reader thread that sleeps in `recv()` and posts an SDL event only when the browser sends something;
- treats end of file on the socket as "the browser exited" and reaps it.

The browser exits when its window closes, so it costs nothing while unused. Sign-out and quitting close it (`ui_liff_close()`).

### The socket protocol

One datagram per message, fields separated by tabs, at most 16 KiB (`LINE_LIFF_PACKET`):

| Direction | Message | Meaning |
| --------- | ------- | ------- |
| UI → browser | `L\t<size>\t<url>` | Load a LIFF page: `full`, `tall` or `compact`, with the LIFF user agent |
| UI → browser | `W\tweb\t<url>` | Load an ordinary web page with LINE's in-app user agent |
| UI → browser | `R\t<dialog id>\t<1\|0>` | The answer to a JavaScript dialog |
| browser → UI | `O\t<url>` | The page navigated to a LIFF or `line://` link: ask LINE for a new view |
| browser → UI | `X\t<url>` | Open this link on the desktop (system browser, mail, phone) |
| browser → UI | `D\t<dialog id>\t<a\|c>\t<text>` | Show an `alert()` (`a`) or `confirm()` (`c`) in the client's window |

The tokens travel only over this socket, never in arguments or the environment, and they are never logged.

### Looking like LINE to the page

- **User agent.** LIFF pages get `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Line/9.7.0 LIFF` (`LINE_LIFF_USER_AGENT`). `liff.isInClient()` is true when the user agent matches `Line/x.y.z LIFF`. Ordinary pages get the same without ` LIFF` (`LINE_WEB_USER_AGENT`).
- **Native shortcuts.** Browser shortcuts require trusted keyboard events.
- **The native bridge.** The helper accepts bridge requests in LIFF mode. With a LINE version of 9.5 or newer in the user agent, `liff.init()` waits for `window._liff.features` before it resolves. A script injected at document start defines `window._liff = { features: [], postMessage }`, frozen. `postMessage(action, featureToken, callbackId, json)` reaches the browser through a WebKit script message handler:
  - `closeWindow` closes the window;
  - `openWindow` loads its `url`, or hands it to the desktop when `external` is true;
  - anything else (QR scanning, the share picker and other phone-only features) is answered at once with a `liffEvent` whose `detail` is `{type, callbackId, error: {code: "FORBIDDEN"}}`, so the SDK call rejects instead of hanging.
- **Closing.** In LIFF mode, navigating to `liff://close` closes the window. Pages can also use WebKit's standard `window.close()` behavior.
- **LINE links** inside the page (`line_liff_url()` or `line_liff_line_scheme()`) are sent back to the UI as `O`, so each gets its own view from LINE.
- **Permissions.** Camera, microphone, location and notification requests are refused.
- **HTTPS only.** Every navigation, whether a link, a script or a server redirect, must stay on `https://` or on the `about:`, `data:` and `blob:` documents that pages create (`line_liff_web_url()`). A plain `http://` link the user clicks goes to the desktop browser instead, and any other move to one is dropped, so nothing on the network can replace a page that holds LINE's tokens.

### Keys in the browser window

Ctrl+Shift+O opens the current page in your own browser (without LINE's tokens), Alt+← and Alt+→ go back and forward, Ctrl+R reloads, Ctrl+plus, Ctrl+minus and Ctrl+0 zoom, and Ctrl+W closes.

### Resource limits

The browser protects the chat from heavy pages:

- It moves itself and every WebKit process into a **systemd transient scope**, its own cgroup v2 leaf, with `MemoryHigh=768M`, `CPUWeight=50` and `IOWeight=50`, via `org.freedesktop.systemd1.Manager`. It waits until `/proc/self/cgroup` shows the move before WebKit forks, so the children land in the scope too.
- It raises its `oom_score_adj` to 500, so the kernel reclaims it before the client, and sets `PR_SET_PDEATHSIG` so it ends with the client.
- It sets a 1 ms timer slack (`PR_SET_TIMERSLACK`), which WebKit's processes inherit, so page timers coalesce into fewer wakeups.
- WebKit's web processes are capped at 512 MB and the network process trims its caches above 128 MB. It keeps a small memory cache and no back-forward page cache. Videos wait for a click. When WebKit stops a page for using too much memory, the browser reloads it once after 3 seconds. If it happens again, the window closes instead of reloading the page forever.
- WebKit's bubblewrap sandbox stays on.
- HTTP cache and cookies live in `~/.cache/avernella/liff` and `~/.local/share/avernella/liff`. When those don't exist yet, `line_liff_storage_dir()` in `liff_url.c` moves in the same directories of an earlier client (`kitatide`, then `line-linux`), so LINE apps keep their cookies. The directory must be a real directory you own, inside a parent no other user can write to, and it is made mode 0700. A symlink, a file or another user's directory is refused, and the browser then keeps nothing on disk.

## Debugging a LINE app

Run with `LINE_DEBUG_LIFF=1` and open the app again. The terminal reports WebKit load stages and bridge operations as `openWindow`, `closeWindow` or `unsupported`. These fixed labels keep URLs, page console text and tokens out of Avernella's LIFF diagnostics. With the same variable, the browser also loads `http://127.0.0.1:<port>/` pages (`line_liff_debug_url()`: port 1024 or higher, and nothing else before the path, such as user info), so a local test page that loads the real SDK (`https://static.line-scdn.net/liff/edge/2/sdk.js`) can exercise the bridge. See [Building: debugging](building.md#debugging).

## Tests

`tests/liff.c` checks the `issueLiffView` request field by field, the launch-URL construction, the consent and fallback paths, and link recognition, using the RPC fixture described in [Testing](testing.md).

Run `python3 tests/liff_browser_debug.py ./avernella-liff` to check diagnostic output with a local test page and a synthetic token. To test the GTK helper with a virtual display, run `xvfb-run -a python3 tests/liff_browser_debug.py ./avernella-liff-gtk`.
