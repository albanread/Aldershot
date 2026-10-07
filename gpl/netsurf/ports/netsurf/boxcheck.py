#!/usr/bin/env python3
"""boxcheck.py [--net] [--log FILE] [--keep] -- NetSurf in the box (design 22, section 11).

Runs NetSurf's monkey front end, built as an x32 POSIX application by
build.sh with ROSGD's fetcher and harness, in one box boot
(tests/lib/boxrun.py): its commands from a file through the start file's
"<" redirection, its output to a file on the share.

  - The Unix half: a local page (file:) with CSS, a PNG and a script.  The
    checks read what monkey plotted -- the heading, the text the script
    wrote, the bitmap -- and the script's console log.
  - The network, through URL_Fetcher and AcornHTTP: build.sh's httpd, a
    Linux program started by *RunBox, serves on the box's loopback.  A page
    with its stylesheet and image, a redirection, a cookie set and sent
    back, the User-Agent, a 3 MB page, a 404's own page, and a refused
    connection.  The checks read what monkey plotted and what httpd logged.

  - With --net, the box has the network (QEMU's user networking, as make
    boot has it) and NetSurf loads real sites over http: and https:, the
    certificates checked by AcornHTTP.

The harness's SLEEP runs NetSurf's scheduler while each page loads.
"""
import argparse
import base64
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROSGD, "tests", "lib"))
import boxrun  # noqa: E402

PAGE = """<html><head><title>ROSGD NetSurf</title>
<style>body { font-family: sans-serif } p.x { color: red }</style></head>
<body><h1>NetSurf over the Unix bridge</h1>
<p class="x" id="p">static text</p>
<script>console.log("duktape says " + (6*7)); document.getElementById("p").textContent = "changed by JavaScript";</script>
<img src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==" width="16" height="16">
</body></html>
"""

PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==")

FORM = """<html><head><title>Form</title></head><body style="margin: 0">
<form method="post" action="/form" enctype="{enctype}">{field}
<input type="submit" value="Send" style="position: absolute; left: 0; top: 0; width: 200px; height: 60px">
</form></body></html>
"""
CLICK = "WINDOW CLICK WIN 0 X 30 Y 30 BUTTON LEFT KIND SINGLE"

# The pages httpd serves from the share (www/); its own paths are in httpd.c
WWW = {
    "index.html": """<html><head><title>ROSGD over URL_Fetcher</title>
<link rel="stylesheet" href="style.css"></head>
<body><h1>Fetched through AcornHTTP</h1><img src="pic.png" width="24" height="24"></body></html>
""",
    "style.css": "h1 { color: blue }\n",
    "page2.html": "<html><head><title>Page two</title></head><body><p>redirected here</p></body></html>\n",
    # forms whose button is where a click will find it
    "form.html": FORM.format(enctype="application/x-www-form-urlencoded",
                             field='<input type="hidden" name="a" value="one two&amp;three">'),
    "form2.html": FORM.format(enctype="multipart/form-data",
                              field='<input type="hidden" name="b" value="multi part">'),
}

HTTP = "http://127.0.0.1:8080"
COMMANDS = [
    "WINDOW NEW",
    "WINDOW GO 0 file:///host/t/test.html", "SLEEP 3000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/index.html", "SLEEP 3000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/redirect", "SLEEP 2000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/setcookie", "SLEEP 2000",
    f"WINDOW GO 0 {HTTP}/cookie", "SLEEP 2000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/agent", "SLEEP 2000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/big", "SLEEP 6000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/missing", "SLEEP 2000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/form.html", "SLEEP 2000", CLICK, "SLEEP 2000", "WINDOW REDRAW 0",
    f"WINDOW GO 0 {HTTP}/form2.html", "SLEEP 2000", CLICK, "SLEEP 2000", "WINDOW REDRAW 0",
    "WINDOW GO 0 http://127.0.0.1:9/refused", "SLEEP 4000", "WINDOW REDRAW 0",
    "QUIT",
]

# --net: real sites, http: and https: (chosen for quick TLS: some servers
# take 20 s over the handshake from anywhere, www.netsurf-browser.org among them)
NET_COMMANDS = [
    "WINDOW GO 0 http://example.com/", "SLEEP 8000", "WINDOW REDRAW 0",
    "WINDOW GO 0 https://example.com/", "SLEEP 8000", "WINDOW REDRAW 0",
    "WINDOW GO 0 https://www.riscosopen.org/", "SLEEP 10000", "WINDOW REDRAW 0",
    "WINDOW GO 0 https://en.wikipedia.org/wiki/RISC_OS", "SLEEP 20000", "WINDOW REDRAW 0",
]
NET_EXPECT = [
    ("net: http://example.com/", "WINDOW SET_URL WIN 0 URL http://example.com/"),
    ("net: https://example.com/", "WINDOW SET_URL WIN 0 URL https://example.com/"),
    ("net: its title", "WINDOW TITLE WIN 0 STR Example Domain"),
    ("net: its text plotted", "STR Example Domain"),
    ("net: https, a 301 followed", "WINDOW SET_URL WIN 0 URL https://www.riscosopen.org/content/"),
    ("net: RISC OS Open's page", "WINDOW TITLE WIN 0 STR RISC OS Open: Welcome"),
    ("net: half a megabyte of Wikipedia", "WINDOW TITLE WIN 0 STR RISC OS - Wikipedia"),
    ("net: shown as secure", "WINDOW PAGE_STATUS WIN 0 STATUS SECURE"),
]

# (what, a line of monkey's output must contain this)
EXPECT = [
    ("the title", "WINDOW TITLE WIN 0 STR ROSGD NetSurf"),
    ("the script ran", "LOG duktape says 42"),
    ("the heading plotted", "PLOT TEXT X 8 Y 52 STR NetSurf over the Unix bridge"),
    ("the script's text plotted", "STR changed by JavaScript"),
    ("the PNG plotted", "PLOT BITMAP X 8 Y 122 WIDTH 16 HEIGHT 16"),
    ("http: the page's title", "WINDOW TITLE WIN 0 STR ROSGD over URL_Fetcher"),
    ("http: its heading plotted", "STR Fetched through AcornHTTP"),
    ("http: its PNG plotted", "WIDTH 24 HEIGHT 24"),
    ("http: 302 followed", "WINDOW TITLE WIN 0 STR Page two"),
    ("http: the cookie sent back", "STR cookie header [rosgd=biscuit]"),
    ("http: NetSurf's User-Agent, then AcornHTTP's", "Acorn_HTTP/"),
    ("http: 3 MB of page", "STR big page end"),
    ("http: a 404's own page", "STR not here"),
    ("http: a form posted", "WINDOW TITLE WIN 0 STR Posted"),
    ("http: a refused connection, AcornHTTP's error", "STR Unable to connect to remote host"),
    ("monkey finished", "GENERIC FINISHED"),
]

# (what, a line httpd logged must contain this)
EXPECT_LOG = [
    ("the page asked for", "REQ GET /index.html HTTP/1.1"),
    ("its stylesheet", "REQ GET /style.css HTTP/1.1"),
    ("its image", "REQ GET /pic.png HTTP/1.1"),
    ("with the page as Referer", "HDR Referer: http://127.0.0.1:8080/index.html"),
    ("the redirection's target", "REQ GET /page2.html HTTP/1.1"),
    ("NetSurf's cookie in the request", "HDR Cookie: rosgd=biscuit"),
    ("a url-encoded POST", "REQ POST /form HTTP/1.1"),
    ("its body", "BODY a=one+two%26three"),
    ("a multipart POST's type", "HDR Content-Type: multipart/form-data; boundary=----NetSurfFormBoundary"),
    ("its part", 'Content-Disposition: form-data; name="b"\\r\\n\\r\\nmulti part\\r\\n'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--net", action="store_true", help="real sites too: the box gets the network")
    ap.add_argument("--log")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    w = os.path.join(ROSGD, "build", "ports", "netsurf", "ws", "netsurf")
    share = tempfile.mkdtemp(prefix="netsurf-boxcheck-")
    try:
        shutil.copy(os.path.join(w, "nsmonkey"), os.path.join(share, "nsmonkey,ff8"))
        shutil.copytree(os.path.join(w, "frontends", "monkey", "res"), os.path.join(share, "res"),
                        symlinks=False)
        os.makedirs(os.path.join(share, "t"))
        with open(os.path.join(share, "t", "test.html"), "w") as f:
            f.write(PAGE)
        os.makedirs(os.path.join(share, "www"))
        for name, text in WWW.items():
            with open(os.path.join(share, "www", name), "w") as f:
                f.write(text)
        with open(os.path.join(share, "www", "pic.png"), "wb") as f:
            f.write(PNG)
        shutil.copy(os.path.join(ROSGD, "build", "ports", "netsurf", "httpd"), os.path.join(share, "httpd,e1f"))
        commands = COMMANDS[:-1] + (NET_COMMANDS if a.net else []) + COMMANDS[-1:]
        with open(os.path.join(share, "cmds"), "w") as f:
            f.write("\n".join(commands) + "\n")
        cmd = boxrun.obey(share, "NS/Run", [
            "Set NETSURFRES /host/res/",
            "WimpSlot -min 8M -max 8M",        # the image (4.5 MB); heap and stack are its dynamic area's
            "Dir HostFS::Host.$",
            "RunBox HostFS::Host.$.httpd 8080 /host/www /host/httpd.log",
            "Echo httpd: rc <Sys$ReturnCode>",
            "Run HostFS::Host.$.nsmonkey --enable_javascript=1 <HostFS::Host.$.cmds >HostFS::Host.$.out",
            "Echo netsurf: rc <Sys$ReturnCode>",
        ])
        r = boxrun.run(share, cmd, log=a.log, timeout=300, net=a.net)
        out = open(os.path.join(share, "out"), errors="replace").read() \
            if os.path.exists(os.path.join(share, "out")) else ""
        lines = [l.strip() for l in out.splitlines() if "POLL" not in l and "PLOT CLIP" not in l]
        failed = 0
        for what, want in EXPECT + (NET_EXPECT if a.net else []):
            ok = any(want in l for l in lines)
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'}  {what}: {want}")
        hlog = os.path.join(share, "httpd.log")
        logged = open(hlog, errors="replace").read().splitlines() if os.path.exists(hlog) else []
        for what, want in EXPECT_LOG:
            ok = any(want in l for l in logged)
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'}  httpd: {what}: {want}")
        agent = [l for l in lines if "agent [" in l]
        print(f"  info  {agent[0] if agent else 'no agent line'}")
        rc = [l for l in r.serial.splitlines() if l.startswith(("netsurf: rc", "httpd"))]
        print(f"  info  {'; '.join(rc) if rc else 'no return code'}; {len(out.splitlines())} lines of output")
        for l in r.serial.splitlines():
            if l.startswith("unixbridge:") or "exception" in l:
                print(f"  info  {l}")
        if not r.ok:
            print(f"  FAIL  the run: {r.status} {r.why() or ''}")
            failed += 1
        if a.keep:
            print(f"  info  kept {share}")
        print(f"netsurf-boxcheck: {'PASS' if not failed else f'{failed} FAILED'}")
        sys.exit(1 if failed else 0)
    finally:
        if not a.keep:
            shutil.rmtree(share, ignore_errors=True)


if __name__ == "__main__":
    main()
