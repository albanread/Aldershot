#!/usr/bin/env python3
"""deskcheck.py [--net] [--url URL] [--log FILE] [--keep] -- NetSurf's Wimp front end in the box.

!NetSurf (build.sh rosgd) on a share, run in one box boot as the first Wimp
task (tests/lib/boxrun.py): it opens a browser window on a test page -- a
heading, styled text, a script that changes it, a PNG -- or on --url, and
when the page has loaded saves the screen (*ScreenSave) and quits.  The
checks read its log (the title, the load, the script's console) and the
screen is kept as build/ports/netsurf/deskcheck.png to look at.  --net gives
the box the network, for a --url on the internet.  ROSGD_ARCH=aarch64 runs
the Apple Silicon box's !NetSurf in rosgd-vz (build/aarch64/ports/netsurf).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROSGD, "tests", "lib"))
import boxrun  # noqa: E402

PAGE = """<html><head><title>ROSGD NetSurf</title>
<style>body { font-family: sans-serif; background: #f4f4e8 } h1 { color: #204080 }
p.x { color: #a01010; font-size: 150% } .box { border: 2px solid #208020; padding: 8px; width: 300px }</style>
</head><body><h1>NetSurf in the RISC OS desktop</h1>
<p class="x" id="p">static text</p>
<p>Plain text in <b>bold</b>, <i>italic</i> and <code>monospace</code>: the Font Manager's.</p>
<div class="box">A box with a border, and an image:
<img src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==" width="32" height="32"></div>
<p><a href="https://www.riscosopen.org/">A link</a></p>
<script>console.log("duktape says " + (6*7)); document.getElementById("p").textContent = "changed by JavaScript";</script>
</body></html>
"""

EXPECT = [
    ("the page's title", "title ROSGD NetSurf"),
    ("the page loaded", "loaded"),
    ("the script ran", "console duktape says 42"),
    ("the screen saved", "snap "),
    ("the task quit", "quit"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url")
    ap.add_argument("--net", action="store_true")
    ap.add_argument("--log")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=240)
    a = ap.parse_args()
    # ROSGD_ARCH=aarch64: the Apple Silicon box's (ARCH=aarch64 build.sh)
    build = os.path.join(ROSGD, "build", *(["aarch64"] if boxrun.VZ else []))
    app = os.path.join(build, "ports", "netsurf", "!NetSurf")
    out_png = os.path.join(build, "ports", "netsurf", "deskcheck.png")
    share = tempfile.mkdtemp(prefix="netsurf-deskcheck-")
    try:
        shutil.copytree(app, os.path.join(share, "!NetSurf"))
        os.makedirs(os.path.join(share, "t"))
        with open(os.path.join(share, "t", "test.html"), "w") as f:
            f.write(PAGE)
        url = a.url or "file:///host/t/test.html"
        cmd = boxrun.obey(share, "NS/Run", [
            "Set NetSurf$Dir HostFS::Host.$.!NetSurf",
            "WimpSlot -min 8M -max 8M",
            f"Run <NetSurf$Dir>.NetSurf -log /host/fe.log -snap HostFS::Host.$.snap -quit {url}",
        ])
        log_path = os.path.join(share, "fe.log")

        def read_log():
            return open(log_path, errors="replace").read().splitlines() if os.path.exists(log_path) else []

        r = boxrun.run(share, cmd, log=a.log, timeout=a.timeout, net=a.net,
                       done=lambda: "quit" in read_log())
        lines = read_log()
        failed = 0
        checks = EXPECT if not a.url else [e for e in EXPECT if "script" not in e[0] and "title" not in e[0]]
        for what, want in checks:
            ok = any(l.startswith(want) for l in lines)
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'}  {what}: {want}")
        for l in lines:
            if l.startswith(("title", "url", "mode")):
                print(f"  info  {l}")
        snap = os.path.join(share, "snap,ff9")
        if os.path.exists(snap):
            subprocess.run([sys.executable, os.path.join(ROSGD, "tools", "sprite2png.py"), snap, out_png],
                           check=True)
            print(f"  info  the screen: {out_png}")
        else:
            print("  FAIL  no screen saved")
            failed += 1
        for l in r.serial.splitlines():
            if "exception" in l or "fault" in l.lower() or l.startswith("unixbridge:"):
                print(f"  info  {l}")
        if r.faults or r.status in ("timed out", "no boot", "fault"):
            print(f"  FAIL  the run: {r.status} {r.why() or ''}")
            failed += 1
        if a.keep:
            print(f"  info  kept {share}")
        print(f"netsurf-deskcheck: {'PASS' if not failed else f'{failed} FAILED'}")
        sys.exit(1 if failed else 0)
    finally:
        if not a.keep:
            shutil.rmtree(share, ignore_errors=True)


if __name__ == "__main__":
    main()
