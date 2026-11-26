#!/usr/bin/env python3
"""The site, filter list and resources guard-content-blocking.sh blocks with.

The page is on 127.0.0.1 and its "ads" on localhost, so they are two sites.

  /page            waits until the engine has fetched /list.txt and
                   /resources.json, then goes to /test, whose loads start
                   after the engine has built its filters.
  /test            loads, in order:
                   - localhost /ad.js       the list blocks it (third party)
                   - localhost /ok.js       nothing blocks it
                   - /own-ad.js             the list names it, but it is the
                                            site's own, which standard mode
                                            lets through
                   - /mine.js               the user's own rule blocks it
                   - localhost /stub.js     the list answers it with the
                                            guard-stub.js resource, which sets
                                            window.stubRan
                   - localhost /pixel.gif?utm_source=guard&id=1
                                            the list removes utm_source
                   then says `GUARD page-loaded stub=<true|false>`.
  /list.txt        the list.
  /resources.json  adblock-rust's resources: guard-stub.js.

Every request is logged as `asked <path>`, so a request the engine blocked
is one the log never shows.
"""

import argparse
import base64
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

LIST = """! Title: guard-content-blocking
/ad.js$script,third-party
/own-ad.js$script
||localhost^*/stub.js$script,redirect=guard-stub.js
||localhost^$removeparam=utm_source
"""

RESOURCES = json.dumps(
    [
        {
            "name": "guard-stub.js",
            "aliases": [],
            "kind": {"mime": "application/javascript"},
            "content": base64.b64encode(b"window.stubRan = true;").decode(),
        }
    ]
)

PAGE_HTML = """<!doctype html>
<title>waiting for the filters</title>
<script>
  (async () => {
    while ((await (await fetch("/ready")).text()) !== "yes") {
      await new Promise((go) => setTimeout(go, 250));
    }
    // The engine builds its filters once both have arrived.
    await new Promise((go) => setTimeout(go, 2000));
    location = "/test";
  })();
</script>
"""

TEST_HTML = """<!doctype html>
<title>a page with ads</title>
<script src="http://localhost:{port}/ad.js"></script>
<script src="http://localhost:{port}/ok.js"></script>
<script src="/own-ad.js"></script>
<script src="/mine.js"></script>
<script src="http://localhost:{port}/stub.js"></script>
<img src="http://localhost:{port}/pixel.gif?utm_source=guard&id=1">
<script>
  addEventListener("load", () => {{
    console.log(`GUARD page-loaded stub=${{window.stubRan === true}}`);
  }});
</script>
"""

# A 1x1 transparent GIF.
PIXEL = base64.b64decode("R0lGODlhAQABAAAAACH5BAEAAAAALAAAAAABAAEAAAIBAAA=")


class Handler(BaseHTTPRequestHandler):
    fetched = set()
    lock = threading.Lock()

    def log_message(self, *_):
        pass

    def answer(self, body, content_type):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlsplit(self.path).path
        print("asked %s host=%s" % (self.path, self.headers.get("Host", "")),
              flush=True)
        port = self.server.server_address[1]
        if path in ("/list.txt", "/resources.json"):
            with Handler.lock:
                Handler.fetched.add(path)
        if path == "/page":
            self.answer(PAGE_HTML, "text/html")
        elif path == "/ready":
            with Handler.lock:
                ready = len(Handler.fetched) == 2
            self.answer("yes" if ready else "no", "text/plain")
        elif path == "/test":
            self.answer(TEST_HTML.format(port=port), "text/html")
        elif path == "/list.txt":
            self.answer(LIST, "text/plain")
        elif path == "/resources.json":
            self.answer(RESOURCES, "application/json")
        elif path == "/pixel.gif":
            self.answer(PIXEL, "image/gif")
        elif path.endswith(".js"):
            self.answer("console.log('GUARD ran %s');" % path,
                        "application/javascript")
        else:
            self.send_error(404)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=0)
    arguments = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", arguments.port), Handler)
    print("serving the site on 127.0.0.1:%d" % server.server_address[1],
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
