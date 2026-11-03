"""Inject metadata publication failures and malformed remote descriptors."""

import http.server
import json
import pathlib
import subprocess
import sys
import tempfile
import threading
import urllib.parse


def main():
    session = pathlib.Path(tempfile.mkdtemp(prefix="ubsmem-metadata-"))
    values = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, status, value=None):
            body = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def key(self):
            return urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)["key"][
                0
            ]

        def do_PUT(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            if (session / "fail_put").exists():
                self.reply(503)
            else:
                values[self.key()] = json.loads(body)
                self.reply(200)

        def do_GET(self):
            key = self.key()
            name = key.rsplit("/", 1)[-1]
            malformed = {"number": 1, "array": [], "null": None, "string": "bad"}
            if name in malformed:
                self.reply(
                    200,
                    {"name": name, "protocol": "ubsmem", "buffers": [malformed[name]]},
                )
            else:
                self.reply(200 if key in values else 404, values.get(key))

        def do_DELETE(self):
            values.pop(self.key(), None)
            self.reply(200)

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            failures = []
            for mode in ("registration", "json"):
                (session / "fail_put").unlink(missing_ok=True)
                result = subprocess.run(
                    [
                        sys.argv[1],
                        mode,
                        f"http://127.0.0.1:{server.server_port}/metadata",
                        str(session),
                    ],
                    timeout=30,
                )
                if result.returncode:
                    failures.append(mode)
            if failures:
                raise RuntimeError(f"Metadata tests failed: {failures}")
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
