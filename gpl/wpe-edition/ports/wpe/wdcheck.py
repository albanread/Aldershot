#!/usr/bin/env python3
"""wdcheck.py -- design 23's W2 check: WPE renders a real page in the box.

    ports/wpe/wdcheck.py BOX-ADDRESS URL OUT.png [--arch aarch64|x86_64]

The box runs WPE's WebDriver in the WPE root, listening on the network:

    *RunBox wperun /usr/bin/WPEWebDriver --port=4444 --host=all

This asks it, from the Mac, for a session with MiniBrowser headless, goes
to URL, waits for the page to load, saves its screenshot as OUT.png (the
page as WPE drew it, in the box) and ends the session.  Prints the page's
title.
"""
import base64
import json
import sys
import urllib.request

TRIPLE = {"aarch64": "aarch64-linux-gnu", "x86_64": "x86_64-linux-gnu"}


def call(base, method, path, body=None, timeout=180):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)["value"]
    except urllib.error.HTTPError as e:
        raise SystemExit(f"wdcheck: {method} {path}: {e.code} {e.read()[:400]!r}")


def main():
    args = sys.argv[1:]
    arch = "aarch64"
    if "--arch" in args:
        i = args.index("--arch")
        arch = args[i + 1]
        del args[i:i + 2]
    host, url, out = args
    base = f"http://{host}:4444"
    print("status:", call(base, "GET", "/status"))
    caps = {"capabilities": {"alwaysMatch": {"wpe:browserOptions": {
        "binary": f"/usr/lib/{TRIPLE[arch]}/wpe-webkit-2.0/MiniBrowser",
        "args": ["--automation", "--headless", "--size=1024x768"]}}}}
    sid = call(base, "POST", "/session", caps)["sessionId"]
    try:
        call(base, "POST", f"/session/{sid}/url", {"url": url})
        print("title:", call(base, "GET", f"/session/{sid}/title"))
        png = base64.b64decode(call(base, "GET", f"/session/{sid}/screenshot"))
        with open(out, "wb") as f:
            f.write(png)
        print(f"wdcheck: {out}, {len(png)} bytes")
    finally:
        call(base, "DELETE", f"/session/{sid}")


if __name__ == "__main__":
    main()
