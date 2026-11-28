"""Run with python3 tests/liff_browser_debug.py ./avernella-liff (or -gtk)."""

from contextlib import contextmanager
import http.server
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time


# Exercise both browser-controlled diagnostic channels with synthetic data.
MARKER = b"synthetic-private-value"
PAGE = b"""<!doctype html><script>
const delivered = location.hash === '#access_token=synthetic-private-value';
addEventListener('liffEvent', () => {
    const asset = document.getElementById('asset').naturalWidth === 1;
    alert(delivered && asset ? 'ready' : 'missing fragment or image');
});
addEventListener('load', () => {
    console.error(location.hash);
    window._liff.postMessage('synthetic-private-value', '', 'callback', '{}');
});
</script><img id="asset" src="FIXTURE_IMAGE">"""

CLOSE_PAGE = b"""<!doctype html><script>
addEventListener('load', () => window._liff.postMessage('closeWindow', '', '', '{}'));
</script>"""
ORDINARY_PAGE = b"""<!doctype html><script>
addEventListener('load', () => {
    const plain = !window._liff && !window.webkit?.messageHandlers?.liff;
    setTimeout(() => alert(plain ? 'ordinary' : 'unexpected bridge'), 100);
    location.href = 'line://app/12345-example';
});
</script>"""


PROFILE_PAGE = b"""<!doctype html><script>
addEventListener('load', () => {
    const previous = localStorage.getItem('profile-check');
    const next = location.pathname.split('/').pop();
    if (next !== 'read') localStorage.setItem('profile-check', next);
    alert(previous || 'empty');
});
</script>"""


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/same", "/outside"):
            self.send_response(302)
            target = "/" if self.path == "/same" else self.server.outside
            self.send_header("Location", target)
            self.end_headers()
            return
        self.send_response(200)
        if self.path == "/pixel.svg":
            self.send_header("Content-Type", "image/svg+xml")
            self.end_headers()
            self.wfile.write(b'<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>')
            return
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        if self.path == "/ordinary":
            self.wfile.write(ORDINARY_PAGE)
        elif self.path.startswith("/profile/"):
            self.wfile.write(PROFILE_PAGE)
        elif self.path == "/close":
            self.wfile.write(CLOSE_PAGE)
        else:
            image = self.server.outside.encode() + b"pixel.svg"
            self.wfile.write(PAGE.replace(b"FIXTURE_IMAGE", image))

    def log_message(self, *args):
        pass


def expect_dialog(ui, message):
    fields = ui.recv(1024).split(b"\t")
    assert fields[0] == b"D" and fields[2:] == [b"a", message], fields
    ui.sendall(b"R\t" + fields[1] + b"\t1")


@contextmanager
def launch(helper, state, account):
    env = dict(os.environ, XDG_DATA_HOME=state, XDG_CACHE_HOME=state,
               LINE_DEBUG_LIFF="1", LINE_DEBUG_SANDBOX="1",
               WPE_PLATFORM="headless")
    ui, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    with ui, child, tempfile.TemporaryFile() as log:
        # The normal UI puts its socket on fd 3 of the helper.
        with subprocess.Popen(
            ["sh", "-c", 'exec 3<&0; exec "$@"', "liff-test",
             str(helper), str(os.getpid()), account],
            env=env, stdin=child, stdout=log, stderr=log,
        ) as browser:
            child.close()
            try:
                ui.settimeout(15)
                assert ui.recv(1024) == b"B\tready"
                yield ui, browser, log
                ui.close()
                assert browser.wait(timeout=10) == 0
            finally:
                if browser.poll() is None:
                    browser.kill()
        log.seek(0)
        output = log.read()
        assert b"line-sandbox liff no-dump=1" in output
        assert b"avernella-liff: load committed" in output
        assert MARKER not in output


def check(helper, state):
    with http.server.HTTPServer(("127.0.0.1", 0), Page) as server, \
            http.server.HTTPServer(("127.0.0.1", 0), Page) as other:
        server.outside = f"http://127.0.0.1:{other.server_port}/"
        other.outside = server.outside
        for origin in (server, other):
            threading.Thread(target=origin.serve_forever, daemon=True).start()
        try:
            base = f"http://127.0.0.1:{server.server_port}".encode()
            profile = b"W\tweb\t" + base + b"/profile/"
            request = b"L\tfull\t" + base
            with launch(helper, state, "a" * 64) as (ui, browser, log):
                status = dict(line.split(":", 1) for line in Path(
                    f"/proc/{browser.pid}/status"
                ).read_text().splitlines() if ":" in line)
                assert status["NoNewPrivs"].strip() == "1"
                assert status["Seccomp"].strip() == "2"
                ui.sendall(profile + b"account-a")
                expect_dialog(ui, b"empty")
                fragment = b"#access_token=" + MARKER
                ui.sendall(request + b"/same" + fragment)
                expect_dialog(ui, b"ready")
                ui.sendall(request + b"/outside" + fragment)
                timeout = time.monotonic() + 10
                while b"outside origin blocked" not in os.pread(
                        log.fileno(), 65536, 0):
                    assert time.monotonic() < timeout, "redirect was not blocked"
                    time.sleep(0.02)
                ui.sendall(b"W\tweb\t" + base + b"/ordinary")
                expect_dialog(ui, b"ordinary")
                ui.sendall(request + b"/close")
                assert browser.wait(timeout=10) == 0
                output = os.pread(log.fileno(), 65536, 0)
                assert b"avernella-liff: bridge unsupported" in output
                assert b"avernella-liff: bridge closeWindow" in output
            # All helpers below open the same origin. B and anonymous sessions
            # start empty; returning to A can recover only A's marker.
            for account, operation, expected in (
                    ("b" * 64, b"account-b", b"empty"),
                    ("a" * 64, b"read", b"account-a"),
                    ("-", b"guest", b"empty"),
                    ("-", b"read", b"empty")):
                with launch(helper, state, account) as (ui, _, _):
                    ui.sendall(profile + operation)
                    expect_dialog(ui, expected)
        finally:
            server.shutdown()
            other.shutdown()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        check(Path(sys.argv[1]).resolve(), tmp)
