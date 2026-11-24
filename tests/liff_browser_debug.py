"""Run with python3 tests/liff_browser_debug.py ./avernella-liff (or -gtk)."""

import http.server
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading


# Exercise both browser-controlled diagnostic channels with synthetic data.
MARKER = b"synthetic-private-value"
PAGE = b"""<!doctype html><script>
addEventListener('load', () => {
    console.error(location.hash);
    window._liff.postMessage('synthetic-private-value', '', 'callback', '{}');
    alert('ready');
});
</script>"""


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, *args):
        pass


def check(helper, state):
    env = dict(os.environ, XDG_DATA_HOME=state, XDG_CACHE_HOME=state,
               LINE_DEBUG_LIFF="1", WPE_PLATFORM="headless")
    with http.server.HTTPServer(("127.0.0.1", 0), Page) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            ui, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            with ui, child, tempfile.TemporaryFile() as log:
                # The normal UI puts its socket on fd 3 of the helper.
                with subprocess.Popen(
                    ["sh", "-c", 'exec 3<&0; exec "$@"', "liff-test",
                     str(helper), str(os.getpid())],
                    env=env, stdin=child, stdout=log, stderr=log,
                ) as browser:
                    child.close()
                    try:
                        ui.settimeout(15)
                        assert ui.recv(1024) == b"B\tready"
                        url = f"http://127.0.0.1:{server.server_port}/"
                        ui.sendall(b"L\tfull\t" + url.encode()
                                   + b"#access_token=" + MARKER)
                        fields = ui.recv(1024).split(b"\t")
                        assert fields[0] == b"D" and fields[2:] == [b"a", b"ready"]
                        ui.sendall(b"R\t" + fields[1] + b"\t1")
                        ui.close()
                        assert browser.wait(timeout=10) == 0
                    finally:
                        if browser.poll() is None:
                            browser.kill()
                log.seek(0)
                output = log.read()
                assert b"avernella-liff: load committed" in output
                assert b"avernella-liff: bridge unsupported" in output
                assert MARKER not in output
        finally:
            server.shutdown()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        check(Path(sys.argv[1]).resolve(), tmp)
