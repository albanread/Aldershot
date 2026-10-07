#!/usr/bin/env python3
"""drivecheck.py [--keep] -- NetSurf's Wimp front end driven with the pointer and keys, in the box.

!NetSurf (build.sh rosgd) runs in a headless box (tests/lib/deskdrive.py:
QEMU's virtio tablet and keyboard through QMP), as its first Wimp task,
on a page whose first line is a link.  Then, as a user would:

  - Adjust on a link to a text file: downloaded into the share's Downloads
    directory (twice: the second is notes-1.txt); Select on a link to a zip
    file, which NetSurf cannot show: downloaded too;
  - the pointer over the link: its address in the status line;
  - Select on the link: the next page;
  - Back, then Forward, on the toolbar;
  - Menu over the page: the window's menu, whose Back is chosen;
  - the URL field: cleared (Ctrl-U), a URL typed, Return;
  - Home on the toolbar: the home page;
  - the icon bar icon: Select opens a window; its menu's New window opens
    another, and Quit ends the task.

Each step is checked in the task's log (-log: titles, URLs, the status
line); shots of the screen are kept in build/ports/netsurf/drivecheck/.
The window opens at a known place (window.c), so the link's is known: the
page's top left is 56 pixels (the toolbar) below the window's visible top.
"""
import argparse
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROSGD, "tests", "lib"))
import boxrun  # noqa: E402
import deskdrive  # noqa: E402

PAGE = """<html><head><title>{title}</title></head>
<body style="margin: 0; font-family: sans-serif; font-size: 20px; line-height: 30px">
<p style="margin: 0; padding: 0 4px; height: 30px"><a href="{href}">{link}</a></p>
<p style="margin: 0; padding: 0 4px; height: 30px"><a href="notes.txt">notes.txt</a></p>
<p style="margin: 0; padding: 0 4px; height: 30px"><a href="archive.zip">archive.zip</a></p>
<p style="padding: 4px">{text}</p></body></html>
"""
NOTES = "Downloaded by NetSurf on ROSGD.\n" * 50
ZIP = bytes(range(256)) * 64            # not a real zip: NetSurf goes by the name's type
PAGES = {
    "one.html": dict(title="Page one", href="two.html", link="Go to page two", text="The first page."),
    "two.html": dict(title="Page two", href="one.html", link="Back to page one", text="The second page."),
    "three.html": dict(title="Page three", href="one.html", link="Page one", text="Reached by typing."),
}

# Screen pixels (1280 x 800): the window's visible area's top left is at
# (48, 48) -- window.c opens it at x0 = 96, y1 = screen top - 96 OS units
# -- and the page begins 56 pixels lower, below the toolbar
LINK = (80, 119)
NOTES_LINK, ZIP_LINK = (60, 149), (60, 179)
BACK, FORWARD, HOME = (81, 63), (143, 63), (329, 63)
URL = (600, 63)
BLANK = (600, 500)
# The icon bar icon, at the right; its menu opens up from the icon bar and
# is kept on the screen: its two items' rows, at an x inside it either way
ICON = (1254, 770)
ICON_NEW, ICON_QUIT = (1240, 719), (1240, 741)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    out = os.path.join(ROSGD, "build", "ports", "netsurf", "drivecheck")
    shutil.rmtree(out, ignore_errors=True)
    share = os.path.join(out, "share")
    os.makedirs(os.path.join(share, "t"))
    shutil.copytree(os.path.join(ROSGD, "build", "ports", "netsurf", "!NetSurf"), os.path.join(share, "!NetSurf"))
    cmos = os.path.join(ROSGD, "runtime", "cmos_default.bin")
    if os.path.exists(cmos):
        shutil.copyfile(cmos, os.path.join(share, "CMOS,ff2"))
    for name, page in PAGES.items():
        with open(os.path.join(share, "t", name), "w") as f:
            f.write(PAGE.format(**page))
    with open(os.path.join(share, "t", "notes.txt"), "w") as f:
        f.write(NOTES)
    with open(os.path.join(share, "t", "archive.zip"), "wb") as f:
        f.write(ZIP)
    downloads = os.path.join(share, "Downloads")
    log_path = os.path.join(share, "fe.log")
    cmd = boxrun.obey(share, "NS/Run", [
        "Set NetSurf$Dir HostFS::Host.$.!NetSurf",
        "IconSprites <NetSurf$Dir>.!Sprites",
        "WimpSlot -min 8M -max 8M",
        "Run <NetSurf$Dir>.NetSurf -log /host/fe.log --homepage_url=file:///host/t/three.html "
        "file:///host/t/one.html",
    ])

    def log():
        return open(log_path, errors="replace").read().splitlines() if os.path.exists(log_path) else []

    failed = 0

    def check(what, want, since, timeout=10.0):
        nonlocal failed
        t0 = time.time()
        while True:
            lines = log()[since:]
            if any(l.startswith(want) for l in lines):
                print(f"  ok    {what}: {want}")
                return
            if time.time() - t0 > timeout:
                print(f"  FAIL  {what}: {want}")
                for l in lines[-6:]:
                    print(f"        | {l}")
                failed += 1
                return
            time.sleep(0.3)

    # Under HVF, QEMU now and then fails to enter the guest at all ("unhandled
    # exit 80000021"): such a boot is tried again
    for attempt in range(3):
        try:
            desk = deskdrive.Desk(share, out, append=[f"rosgd.run={cmd}"])
            break
        except RuntimeError as e:
            serial = open(os.path.join(out, "serial.log"), errors="replace").read()
            if attempt == 2 or "unhandled exit" not in serial:
                raise
            print(f"  info  the box did not start ({e}); again")
    with desk as d:
        check("the first page", "title Page one", 0, timeout=30)
        d.shot("1-page-one")
        n = len(log())
        d.click(*NOTES_LINK, button="adjust")
        check("Adjust on a link: downloaded", "downloaded /host/Downloads/notes.txt", n)
        n = len(log())
        d.click(*NOTES_LINK, button="adjust")
        check("again: a new name", "downloaded /host/Downloads/notes-1.txt", n)
        n = len(log())
        d.click(*ZIP_LINK)
        check("Select on a zip file: downloaded", "downloaded /host/Downloads/archive.zip", n)
        d.shot("1b-downloaded")
        for leaf, want in (("notes.txt", NOTES.encode()), ("notes-1.txt", NOTES.encode()), ("archive.zip", ZIP)):
            path = os.path.join(downloads, leaf)
            ok = os.path.exists(path) and open(path, "rb").read() == want
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'}  Downloads/{leaf} on the share, as the original")
        n = len(log())
        d.move(*LINK)
        d.move(LINK[0] + 4, LINK[1])
        check("the pointer over the link: its address", "status file:///host/t/two.html", n)
        n = len(log())
        d.click(*LINK)
        check("Select on the link", "title Page two", n)
        d.shot("2-page-two")
        n = len(log())
        d.click(*BACK)
        check("Back", "title Page one", n)
        n = len(log())
        d.click(*FORWARD)
        check("Forward", "title Page two", n)
        n = len(log())
        d.click(*BLANK, button="menu")
        time.sleep(0.8)
        d.shot("3-window-menu")
        d.click(BLANK[0] + 40, BLANK[1] + 22)           # the menu's first item: Back
        check("the window menu's Back", "title Page one", n)
        n = len(log())
        d.click(*URL)
        d.shot("3b-url-clicked")
        d.key("ctrl", "u")
        d.shot("3c-url-cleared")
        d.type("file:///host/t/three.html")
        d.shot("3d-url-typed")
        d.key("ret")
        check("a URL typed", "title Page three", n)
        n = len(log())
        d.click(*BACK)
        check("Back again", "title Page one", n)
        n = len(log())
        d.click(*HOME)
        check("Home (the home page, three.html)", "title Page three", n)
        d.shot("4-home")
        n = len(log())
        d.click(*ICON)
        check("Select on the icon bar icon: a window on the home page", "title Page three", n)
        n = len(log())
        d.click(*ICON, button="menu")
        time.sleep(0.8)
        d.shot("5-iconbar-menu")
        d.click(*ICON_NEW)
        check("the icon bar menu's New window", "title Page three", n)
        d.shot("6-three-windows")
        n = len(log())
        d.click(*ICON, button="menu")
        time.sleep(0.8)
        d.click(*ICON_QUIT)
        check("the icon bar menu's Quit", "quit", n)
        faults = d.faults()
        if faults:
            print(f"  FAIL  a fault: {faults[0]}")
            failed += 1
    print(f"  info  shots: {os.path.join(out, 'shots')}")
    print(f"netsurf-drivecheck: {'PASS' if not failed else f'{failed} FAILED'}")
    if not a.keep:
        shutil.rmtree(share, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
