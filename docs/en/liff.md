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

The helper removes the fragment before asking WebKit to navigate. At document start, an isolated script checks the top frame’s scheme, host and port against LINE’s issued endpoint. It restores the fragment in that document for the SDK to read from `location.hash`. The SDK can then keep its session in `sessionStorage` and remove the fragment from the address.

**When LINE says no.** A `LiffException` (code in field 1, payload union in field 3) is handled in `exception_page()`:

| Code | Meaning | Client action |
| ---- | ------- | ------------- |
| 3 CONSENT_REQUIRED | The user must agree to the app's permissions first | Open the consent page (payload 3, field 2) as an ordinary web page |
| 7 PERMANENT_LINK_INVALID_REQUEST | The deep link is no longer valid | Open the fallback page (payload 4, field 2) as an ordinary web page |
| Others | Unauthorized, update required, service terminated and so on | Show a readable reason (`refusal()`) |

The worker replies with `url`, `size` and an optional `title`. A `web: true` reply opens in ordinary browsing mode for consent or fallback. After consent, the user can reopen the original app link from the chat.

## The browser helper

`avernella-liff` is a separate program. It is built with **WPE WebKit** when available (preferred on Wayland), and as `avernella-liff-gtk` with **WebKitGTK 6.0** as a fallback (and on X11). Both come from the same `liff_browser.c`; `LINE_LIFF_GTK` selects the GTK build.

### Starting and stopping

`ui_liff.c`:

- finds the helper beside `avernella` or in `../lib/avernella`, choosing WPE when a Wayland display is present;
- starts the helper on demand with `pidfd_spawn()`, passing its parent PID, the account profile name and an `AF_UNIX` `SOCK_SEQPACKET` socket on fd 3. The profile name is a SHA-256 digest of the account ID. A dash selects an ephemeral session.
- supervises it through the pidfd, and runs a reader thread that sleeps in `recv()` and posts an SDL event only when the browser sends something;
- treats end of file on the socket as "the browser exited" and reaps it.

Opening a LIFF app starts a fresh helper. Opening an ordinary page after a LIFF app also starts a fresh helper, so the previous view’s navigation history and session storage end with it. Ordinary pages can reuse the ordinary helper. Closing its window, ending the account session or quitting Avernella closes the helper.

### The socket protocol

One datagram per message, fields separated by tabs, at most 16 KiB (`LINE_LIFF_PACKET`):

| Direction | Message | Meaning |
| --------- | ------- | ------- |
| UI → browser | `L\t<size>\t<url>` | Load a LIFF page: `full`, `tall` or `compact`, with the LIFF user agent |
| UI → browser | `W\tweb\t<url>` | Load an ordinary web page with LINE's in-app user agent |
| UI → browser | `R\t<dialog id>\t<1\|0>` | The answer to a JavaScript dialog |
| browser → UI | `O\t<url>` | A user activated a LINE link. If it names a LIFF app, request a new view with no chat context |
| browser → UI | `X\t<url>` | Open this link on the desktop (system browser, mail, phone) |
| browser → UI | `D\t<dialog id>\t<a\|c>\t<text>` | Show an `alert()` (`a`) or `confirm()` (`c`) in the client's window |

The tokens travel only over this socket, never in arguments or the environment, and they are never logged.

### Linux isolation

Before it accepts messages, the helper checks that fd 3 is an `AF_UNIX` `SOCK_SEQPACKET` socket. It uses the kernel's `SO_PEERCRED` to verify that the peer PID matches its parent and that the peer UID matches its effective UID. The socket is marked close-on-exec.

- Before GLib or WebKit can start threads or subprocesses, the helper sets `PR_SET_NO_NEW_PRIVS` and `PR_SET_DUMPABLE=0`, and sets the soft and hard `RLIMIT_CORE` limits to zero. It exits if this setup or seccomp installation fails.
- The seccomp filter blocks `ptrace`, `process_vm_readv`, `process_vm_writev`, and `pidfd_getfd`, along with `bpf`, `perf_event_open`, and `userfaultfd`. It returns `ENOSYS` for `io_uring` so libraries can use their ordinary I/O paths. WebKit manages its own filesystem namespace and the filter permits its JavaScript JIT.
- Where Landlock is available, ABI 4 and later block explicit TCP `bind()` calls. Native sessions also use ABI 6 and later to limit signals to the helper's own Landlock domain and nested domains. Flatpak skips this signal layer so WebKit can manage processes started by the Flatpak portal.

If the kernel exposes the required Landlock ABI and installing its rules fails, the helper exits before loading a page. The helper's Linux policies are inherited by its actual descendants. When WebKit starts a process through the Flatpak portal, Flatpak provides that process's sandbox.

### Looking like LINE to the page

- **User agent.** LIFF pages get `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Line/9.7.0 LIFF` (`LINE_LIFF_USER_AGENT`). `liff.isInClient()` is true when the user agent matches `Line/x.y.z LIFF`. Ordinary pages get the same without ` LIFF` (`LINE_WEB_USER_AGENT`).
- **Native shortcuts.** Browser shortcuts require trusted keyboard events.
- **The native bridge.** At document start, the helper exposes `window._liff` to the authorized LIFF top frame. The SDK’s `postMessage(action, featureToken, callbackId, json)` goes through an isolated script world that checks the sending window, its origin and the active launch. The native endpoint is private to that world.
  - `closeWindow` closes the window;
  - `openWindow` loads a same-origin HTTPS URL. External requests and other HTTPS origins open on the desktop with the fragment removed.
  - anything else (QR scanning, the share picker and other phone-only features) is answered at once with a `liffEvent` whose `detail` is `{type, callbackId, error: {code: "FORBIDDEN"}}`, so the SDK call rejects instead of hanging.
- **Closing.** A user-activated `liff://close` link can close the LIFF window. The SDK can request `closeWindow` through the authorized bridge.
- **LINE links.** Activating a LINE link opens it in Avernella. A LIFF app launched from a web page receives no chat context. Opening it directly from a chat can supply that chat after the worker verifies it.
- **Permissions.** Camera, microphone, location and notification requests are refused.
- **Navigation.** The authenticated document and redirects must stay on its issued origin. A user can follow an outside HTTPS link in the desktop browser. Ordinary web mode permits HTTPS navigation and local document schemes such as `about:`, `blob:` and `data:`.

### Keys in the browser window

Ctrl+Shift+O opens the current page in your own browser (without LINE's tokens), Alt+← and Alt+→ go back and forward, Ctrl+R reloads, Ctrl+plus, Ctrl+minus and Ctrl+0 zoom, and Ctrl+W closes.

### Resource limits

The browser protects the chat from heavy pages:

- It asks `org.freedesktop.systemd1.Manager` to move it into a **systemd transient scope**, its own cgroup v2 leaf, with `MemoryHigh=768M`, `CPUWeight=50` and `IOWeight=50`. Locally spawned WebKit processes inherit the scope after the move.
- It raises its `oom_score_adj` to 500, so the kernel reclaims it before the client, and sets `PR_SET_PDEATHSIG` so it ends with the client.
- It sets a 1 ms timer slack (`PR_SET_TIMERSLACK`), which its descendants inherit, so page timers coalesce into fewer wakeups.
- WebKit's web processes are capped at 512 MB and the network process trims its caches above 128 MB. It keeps a small memory cache and no back-forward page cache. Videos wait for a click. When WebKit stops a page for using too much memory, the browser reloads it once after 3 seconds. If it happens again, the window closes instead of reloading the page forever.
- Each signed-in account has private data and cache directories at `$XDG_DATA_HOME/avernella/liff-profiles/<digest>` and `$XDG_CACHE_HOME/avernella/liff-profiles/<digest>`. WebKit uses an ephemeral session if the account is unknown or either directory fails its ownership and mode checks. Shared profiles from older releases stay on disk and new sessions start with fresh cookies.

## Debugging a LINE app

Run with `LINE_DEBUG_LIFF=1` and open the app again. The terminal reports WebKit load stages and bridge operations as `openWindow`, `closeWindow` or `unsupported`. These fixed labels keep URLs, page console text and tokens out of Avernella's LIFF diagnostics. With the same variable, the browser also loads `http://127.0.0.1:<port>/` pages (`line_liff_debug_url()`: port 1024 or higher, and nothing else before the path, such as user info), so a local test page that loads the real SDK (`https://static.line-scdn.net/liff/edge/2/sdk.js`) can exercise the bridge. See [Building: debugging](building.md#debugging).

## Tests

`tests/liff.c` checks the `issueLiffView` request field by field, the launch-URL construction, the consent and fallback paths, and link recognition, using the RPC fixture described in [Testing](testing.md).

Run `python3 tests/liff_browser_debug.py ./avernella-liff` to check a same-origin redirect, SDK token delivery and reply, rejection of an outside redirect, and ordinary browsing. The fixture uses two loopback origins and a synthetic token. It opens separate helpers for accounts A, B and A again to verify that each receives its own local storage. Two anonymous launches verify that ephemeral storage expires. To run the GTK helper on a virtual display, use `xvfb-run -a python3 tests/liff_browser_debug.py ./avernella-liff-gtk`.
