#!/usr/bin/env python3
"""engine.py -- !Write's document layer in a headless box (test/engine.c):
typing, styles, undo, a click, the word at the caret, saving in Write's
format and as text, loading it back.

    python3 ports/write/test/engine.py      (ROSGD_ARCH=aarch64: the Apple Silicon box)

Exit status 0 when the box printed "engine: OK" and wrote what it should."""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROSGD = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROSGD, "tests", "lib"))
import boxrun  # noqa: E402

B = os.path.join(ROSGD, "build", "aarch64" if os.environ.get("ROSGD_ARCH") == "aarch64" else "",
                 "ports", "write")


def main():
    share = tempfile.mkdtemp(prefix="write-engine-")
    shutil.copy(os.path.join(B, "EngineTest,ff8"), os.path.join(share, "EngineTest,ff8"))
    with open(os.path.join(share, "Notes,fff"), "w") as fh:
        fh.write("The quick brown fox jumps over the lazy dog.\nA second paragraph of txet, long enough "
                 "that it should wrap round onto another line in the window when Paige lays it out.\n")
    with open(os.path.join(share, "In,c32"), "w") as fh:
        fh.write(r"{\rtf1\ansi\deff0{\fonttbl{\f0\froman Times New Roman;}{\f1\fswiss Helvetica;}}"
                 r"\f0\fs24 Hello {\b bold} and {\i italic} words.\par"
                 r"{\f1\fs36 A larger second paragraph.}\par}")
    shutil.copy(os.path.join(HERE, "test", "cocoa.rtf"), os.path.join(share, "Cocoa,c32"))
    shutil.copy(os.path.join(HERE, "test", "page.html"), os.path.join(share, "Page,faf"))
    with open(os.path.join(share, "Words,fff"), "w") as fh:
        fh.write("rosgd\n")
    os.makedirs(os.path.join(share, "Resources", "Dictionaries"))
    shutil.copy(os.path.join(ROSGD, "disc", "Resources", "Dictionaries", "spelldict,ffd"),
                os.path.join(share, "Resources", "Dictionaries", "spelldict,ffd"))
    r = boxrun.run(share, boxrun.obey(share, "Go", ["WimpSlot -min 8M -max 8M",
                                                    "Run HostFS::Host.$.EngineTest"]), timeout=300)
    for ln in r.serial.splitlines():
        if ln.startswith("engine:") or "exception" in ln or "rror" in ln:
            print(ln[:160])
    text = os.path.join(share, "OutText,fff")
    ok = ("engine: OK" in r.serial and "FAIL" not in r.serial
          and os.path.exists(os.path.join(share, "Out,0a0")))
    if os.path.exists(text):
        print("text:", repr(open(text, encoding="latin-1").read()[:120]))
    words = os.path.join(share, "Words,fff")
    if not os.path.exists(words):
        words = os.path.join(share, "Words")
    learnt = open(words).read().split() if os.path.exists(words) else []
    print("the user's words now:", learnt)
    rtf = os.path.join(share, "OutRTF,c32")
    if os.path.exists(rtf):
        body = open(rtf, encoding="latin-1").read()
        print("RTF out:", len(body), "bytes:", repr(body[:160]))
        ok = (ok and body.startswith("{\\rtf1") and "\\b" in body
              and "Another paragraph that wraps round. Ano" in body)
    else:
        ok = False
        print("no RTF written")
    intext = os.path.join(share, "InText")
    got = open(intext, encoding="latin-1").read() if os.path.exists(intext) else ""
    print("foreign RTF as text:", repr(got))
    ok = ok and "Hello bold and italic words." in got and "A larger second paragraph." in got
    ok = ok and learnt == ["liek"]
    html = os.path.join(share, "OutHTML,faf")
    body = open(html, encoding="latin-1").read() if os.path.exists(html) else ""
    print("HTML out:", repr(body[:200]))
    ok = (ok and body.startswith("<html>") and "<title>Untitled</title>" in body
          and "Another paragraph that wraps round. Ano" in body and "<b>" in body)
    page = os.path.join(share, "PageText")
    got = open(page, encoding="latin-1").read() if os.path.exists(page) else ""
    print("HTML page as text:", repr(got))
    ok = ok and "Write reads HTML" in got and "The second item" in got
    rtf = os.path.join(share, "PageRTF,c32")
    got = open(rtf, encoding="latin-1").read() if os.path.exists(rtf) else ""
    ok = ok and "\\red192\\green0\\blue0" in got and "Trinity" in got   # <font color>, fonts as pg_chars
    cocoa = os.path.join(share, "CocoaText")
    got = open(cocoa, encoding="latin-1").read() if os.path.exists(cocoa) else ""
    print("Cocoa's RTF as text:", repr(got))
    ok = ok and got.split("\n")[:3] == [
        "A letter in bold, italic and underlined type, with red words in it.",
        "A heading in Helvetica",
        "A last paragraph, long enough to wrap round the window at least once so that the layout "
        "of imported text can be seen at a glance."]
    print("PASS" if ok else "FAIL", share)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
