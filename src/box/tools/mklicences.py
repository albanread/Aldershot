#!/usr/bin/env python3
"""Make disc/Documents/Licences: the licence of every package BOX uses.

    python3 tools/mklicences.py [OUT]       (run from rosgd/; OUT defaults
                                             to disc/Documents/Licences)

One text file per package (,fff), named after it, and ReadMe, the index.
Each file gives the package's version, where it comes from, what in BOX
uses it, and then its licence as the package itself ships it, copied
verbatim (only turned into Latin-1, the box's alphabet).

The texts are read from where the build already keeps the packages: the
tarballs in .cache (deps/*.sh fetch them), the unpacked trees there
(clang-box-src, dynarmic-src, the Mojo set), third_party/ and riscos-src
beside this tree, the ports, cargo's registry and the toolchains.  The
few that are nowhere in the build are fetched once into .cache/licences:
GCC's (Mojo's compiler is one static program with Alpine's libstdc++ and
libgcc linked in, and Alpine's packages carry no licence texts, so they
come from GCC 15.2.0's source), the fonts', the SymSpell word list's and
MPL 2.0.  Nothing in the box uses glibc; the WPE edition's root has its
own list (tools/mkwpelicences.py).  ROSGD_CACHE names another .cache
(a worktree has none of its own); THIRD_PARTY, RISCOS_SRC and DEPS_SRC
the others.  Versions are read from the deps scripts, so a new version
there is a new file here when this is run again.
"""
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import textwrap

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.environ.get("ROSGD_CACHE", os.path.join(HERE, ".cache"))
XB = os.path.dirname(os.path.dirname(HERE))
TP = os.environ.get("THIRD_PARTY", os.path.join(XB, "third_party"))
ROS = os.environ.get("RISCOS_SRC", os.path.join(XB, "riscos-src/BCM2835/RiscOS"))
DEPSRC = os.environ.get("DEPS_SRC", os.path.join(XB, "deps-macos-intel/src"))
FETCHED = os.path.join(CACHE, "licences")

RULE = "-" * 72


def var(script, name):
    """NAME=value in one of our shell scripts."""
    text = open(os.path.join(HERE, script)).read()
    m = re.search(r"^\s*%s=(\S+)" % name, text, re.M)
    if not m:
        sys.exit("mklicences: no %s in %s" % (name, script))
    return m.group(1).strip("'\"")


# ---- where texts come from ------------------------------------------------

def F(path):
    return ("file", path)


def T(tarball, member):
    return ("tar", tarball, member)


def C(name, url):
    """A text fetched once into .cache/licences (or put there by hand)."""
    return ("fetch", name, url)


def SH(manual, first, last):
    """A licence quoted in a StrongHelp manual's page: its text from the
    line starting first to the line ending last, the manual's own
    formatting commands (lines starting #) left out."""
    return ("stronghelp", manual, first, last)


def read(src):
    kind = src[0]
    if kind == "stronghelp":
        data = open(src[1], "rb").read()
        start = data.index(src[2].encode())
        end = data.index(src[3].encode(), start) + len(src[3])
        lines = data[start:end].replace(b"\r", b"").split(b"\n")
        paras = []
        for l in lines:
            if not l or l.startswith(b"#"):
                continue
            t = l.decode("latin-1")
            if t.startswith("\x8f\t"):                  # RISC OS's bullet, then a tab
                paras.append(textwrap.fill(t[2:], 76, initial_indent="* ",
                                           subsequent_indent="  "))
            else:
                paras.append(textwrap.fill(t, 76))
        return ("\n\n".join(paras) + "\n").encode("latin-1")
    if kind == "file":
        return open(src[1], "rb").read()
    if kind == "tar":
        with tarfile.open(src[1]) as t:
            return t.extractfile(src[2]).read()
    if kind == "fetch":
        path = os.path.join(FETCHED, src[1])
        if not os.path.exists(path):
            os.makedirs(FETCHED, exist_ok=True)
            subprocess.run(["curl", "-fsSL", "-o", path, src[2]], check=True)
        return open(path, "rb").read()
    raise ValueError(kind)


def where(src):
    if src[0] in ("file", "stronghelp"):
        return os.path.basename(src[1])
    if src[0] == "tar":
        return src[2]
    return src[2]


LATIN = {"‘": "'", "’": "'", "“": '"', "”": '"',
         "–": "-", "—": "--", "…": "...", "•": "*",
         " ": " ", " ": " ", " ": " ", " ": " ",
         "​": "", "﻿": "", "™": "(TM)", "−": "-",
         "→": "->", "←": "<-", "≤": "<=", "≥": ">=",
         "‐": "-", "‑": "-", "­": ""}


def latin1(data):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = data.decode("latin-1")
    text = text.replace("\r\n", "\n")
    out = []
    for ch in text:
        ch = LATIN.get(ch, ch)
        try:
            ch.encode("latin-1")
        except UnicodeEncodeError:
            ch = "?"
        out.append(ch)
    return "".join(out)


# ---- the packages ---------------------------------------------------------

def tarball(pattern):
    hits = sorted(glob.glob(os.path.join(CACHE, pattern)))
    if not hits:
        sys.exit("mklicences: no %s in %s" % (pattern, CACHE))
    return hits[-1]


def gitrev(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "--short=10", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() or "?"


def mojo_dir():
    for d in (os.path.join(CACHE, "aarch64/mojo-box-musl-aarch64/mojo"),
              os.path.join(CACHE, "mojo-box-musl-x86_64/mojo"),
              os.path.join(CACHE, "mojo-box-musl-aarch64/mojo")):
        if os.path.isdir(d):
            return d
    sys.exit("mklicences: no Mojo set in .cache (deps/get-mojo.sh)")


def zig_lib():
    out = subprocess.run(["zig", "env"], capture_output=True, text=True).stdout
    try:
        return json.loads(out)["lib_dir"]
    except ValueError:
        return re.search(r'\.lib_dir = "([^"]+)"', out).group(1)


def zig_version():
    return subprocess.run(["zig", "version"], capture_output=True, text=True).stdout.strip()


def rust_licences():
    hits = sorted(glob.glob(os.path.expanduser("~/.rustup/toolchains/*/share/doc/rust/licenses")))
    if not hits:
        sys.exit("mklicences: no Rust toolchain's licences under ~/.rustup")
    return hits[-1]


def rust_nightly():
    r = subprocess.run(["rustup", "run", "nightly", "rustc", "--version"],
                       capture_output=True, text=True)
    m = re.search(r"rustc (\S+) \(\S+ (\S+)\)", r.stdout)
    return "%s %s" % m.groups() if m else "nightly"


def crates(manifest_dir):
    """The crates a Rust program is built from (no build-time-only ones)."""
    tree = subprocess.run(
        ["cargo", "tree", "--offline", "-e", "normal,no-proc-macro", "--prefix", "none",
         "--format", "{p}|{l}"], cwd=manifest_dir, capture_output=True, text=True, check=True).stdout
    meta = json.loads(subprocess.run(
        ["cargo", "metadata", "--offline", "--format-version", "1"], cwd=manifest_dir,
        capture_output=True, text=True, check=True).stdout)
    dirs = {(p["name"], p["version"]): os.path.dirname(p["manifest_path"]) for p in meta["packages"]}
    found = {}
    for line in tree.splitlines():
        if "|" not in line:
            continue
        p, lic = line.split("|", 1)
        m = re.match(r"(\S+) v(\S+)", p)
        name, ver = m.group(1), m.group(2)
        if (name, ver) in found:
            continue
        found[(name, ver)] = (lic.replace(" (*)", "").strip(), dirs.get((name, ver)))
    return found


def packages():
    mojo = mojo_dir()
    zlib_v = var("deps/build-imagelibs.sh", "ZV")
    png_v = var("deps/build-imagelibs.sh", "PV")
    jpeg_v = var("deps/build-imagelibs.sh", "JV")
    linux_v = var("kernel/build-kernel.sh", "V")
    llvm_v = var("deps/build-clang-box.sh", "LLVM_V")
    musl_v = var("deps/build-clang-box.sh", "MUSL_V")
    mi_v = var("deps/build-clang-box.sh", "MI_V")
    ossl_v = var("deps/build-openssl.sh", "V")
    curl_v = var("deps/build-curl.sh", "V")
    ssh_v = var("deps/build-openssh.sh", "V")
    ksmbd_v = var("deps/build-ksmbd.sh", "KSMBD_V")
    libnl_v = var("deps/build-ksmbd.sh", "LIBNL_V")
    glib_v = var("deps/build-ksmbd.sh", "GLIB_V")
    dyn_v = var("deps/build-dynarmic.sh", "DSHA")
    boost_v = var("deps/build-dynarmic.sh", "BV")
    dsrc = os.path.join(CACHE, "dynarmic-src")
    llvm = os.path.join(CACHE, "clang-box-src")
    ns = os.path.join(TP, "NetSurf")
    zl = zig_lib()
    rl = rust_licences()
    expat = sorted(glob.glob(os.path.join(DEPSRC, "expat-*.tar.xz")))[-1]
    utf8 = sorted(glob.glob(os.path.join(DEPSRC, "utf8proc-*.tar.gz")))[-1]
    expat_v = re.search(r"expat-(.+)\.tar", expat).group(1)
    utf8_v = re.search(r"utf8proc-(.+)\.tar", utf8).group(1)
    sparks = sorted(d for d in os.listdir(os.path.join(TP, "SparkFS"))
                    if os.path.exists(os.path.join(TP, "SparkFS", d, "LICENCE")))
    ns_libs = ["libwapcaplet", "libparserutils", "libhubbub", "libdom", "libcss", "libnsgif",
               "libnsbmp", "libnsutils", "libnspsl", "libnslog", "libsvgtiny"]
    rool_other = [("MimeMap", "Networking/MimeMap"), ("Squash", "Programmer/Squash"),
                  ("SharedSound", "Audio/SharedSnd"), ("VFPSupport", "HWSupport/VFPSupport"),
                  ("SpriteExtend", "Video/Render/SprExtend"), ("InetRes", "SystemRes/InetRes"),
                  ("Internet", "Networking/AUN/Internet"), ("TCPIPLibs", "Lib/TCPIPLibs")]
    fonts = "RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them"

    P = []

    def add(file, name, version, url, used, licence, texts, note=None):
        P.append(dict(file=file, name=name, version=version, url=url, used=used,
                      licence=licence, texts=texts, note=note))

    # BOX itself
    add("BOX", "BOX (ROSGD)", "this disc's build", "https://github.com/albanread/A7232ToolChain",
        "only the code written new for BOX, and translated from no one else's: the runtime "
        "(/init) and the modules written for it, the glue that joins the translated modules to "
        "it, rosbas, roscc, rosasm, pdfsvc, GraphTask, !Write's own code, the Mojo riscos "
        "package, and the disc's own applications and examples",
        "MIT", [("BOX's licence", F(os.path.join(os.path.dirname(HERE), "LICENSE")))],
        note="Every package BOX ports, translates or builds on keeps its own licence, and BOX's "
             "port of it, translation and changes included, is under that licence too, not "
             "MIT: RISC OS (Apache 2.0), PipeDream (MPL 2.0), NetSurf (GPL 2), StrongED and "
             "StrongHelp (BSD), Paige (LGPL 2.1) and the rest, each in its own file here.")

    # The ROM: RISC OS, from RISC OS Open's sources
    add("RISCOS", "RISC OS 5 (RISC OS Open's sources)", "5.30 (Kernel 6.70)",
        "https://gitlab.riscosopen.org/RiscOS/Sources",
        "the modules and applications translated to C for the box (the Kernel's SWIs, FileSwitch, "
        "the Wimp, the Font Manager and the ROM fonts Corpus, Homerton, Trinity, Selwyn and Sidney, "
        "Draw, ColourTrans, BASIC (BASICVFP), the Toolbox and its modules, DrawFile, MessageTrans, "
        "Territory, the networking modules and the rest); !Maestro and !CloseUp, and the Maestro "
        "tunes in Documents.Music, from RISC OS 5.30's HardDisc4",
        "Apache License 2.0; some components BSD-style",
        [("The Apache License 2.0, as RISC OS Open ships it (Kernel/LICENSE; most components "
          "carry the same file)", F(os.path.join(ROS, "Sources/Kernel/LICENSE")))] +
        [("%s (%s/LICENSE)" % (n, p), F(os.path.join(ROS, "Sources", p, "LICENSE"))) for n, p in rool_other],
        note="!Maestro, !CloseUp and the Maestro tunes come from RISC OS Open's HardDisc4 image, "
             "which states no licence of its own; RISC OS Open's sources for those applications "
             "are under the Apache License 2.0.")

    # The disc's binary-only RISC OS applications, as RISC OS 5.30's image has them
    disc = os.path.join(HERE, "disc")
    add("AWViewer", "AWViewer, the ArtWorks viewer", "2.18 (12-Apr-2018)",
        "http://www.mw-software.com/software/awmodules/awrender.html",
        "Apps.!AWViewer, as RISC OS 5.30's image has it, unaltered (ARM code, run by the ARM "
        "container); its modules render ArtWorks pictures for it and for OvationPro",
        "Freely distributable, unaltered and complete, free of charge (Computer Concepts and "
        "MW Software)",
        [("!AWViewer.!ReadMe", F(os.path.join(disc, "Apps", "!AWViewer", "!ReadMe")))],
        note="The ArtWorks file icon BOX shows is the disc's own (Resources.ArtWorksSprites), "
             "so that !AWViewer is shipped unaltered, as these conditions require.")
    add("OvationPro", "OvationPro (David Pilling)", "2.78 (26-July-2022)",
        "https://www.davidpilling.com/",
        "Apps.!OvnPro, as RISC OS 5.30's image has it, unaltered (ARM code, run by the ARM "
        "container)",
        "Not stated in the application; distributed by RISC OS Open in RISC OS 5.30's image",
        [],
        note="OvationPro carries no licence text of its own.  BOX ships it unaltered, as RISC OS "
             "Open distributes it in its RISC OS 5.30 disc image for the Raspberry Pi.")

    sh = os.path.join(HERE, "ports", "stronghelp", "app", "!StrongHlp")
    add("StrongHelp", "StrongHelp (StrongHelp Developers)", "2.90 (its data files)",
        "https://sourceforge.net/projects/stronghelp/",
        "Resources:$.Apps.!StrongHlp (in the ROM), its data: its manual, Messages, Templates, menus, sprites, configuration "
        "and tools, as RISC OS 5.30's image has them, with !Run and !Boot changed where they "
        "used 2.90's module. The program is new C (design 31), written from StrongHelp's "
        "manuals, not from its source; it is under StrongHelp's licence",
        "BSD 3-clause",
        [("StrongHelp's licence, from its own manual (Resources.UK.StrongHelp, page Licence)",
          SH(os.path.join(sh, "Resources", "UK", "StrongHelp,3d6"),
             "Copyright (c) 2017, StrongHelp Developers", "SUCH DAMAGE."))])
    add("Manuals", "The StrongHelp programming manuals (!Manuals)", "RISC OS 5.30",
        "https://www.riscosopen.org/content/downloads/raspberry-pi",
        "Utilities.!Manuals, as RISC OS 5.30's image has it, unaltered: the manuals Assembly, "
        "Basic, FloatingPt, GPIO, InetSocket, InetSWIs, MiscSWIs, OS, SH-RefMan, Toolbox, "
        "VCache, VDU and Wimp",
        "No licence stated; described in the manuals as public domain; distributed by RISC "
        "OS Open in RISC OS 5.30's image",
        [],
        note="The manuals state no licence.  They describe themselves as the main public domain "
             "programming reference for RISC OS, written and kept up by many contributors; "
             "their sources are published by riscos.info (github.com/riscos-dot-info), also "
             "without a licence file.  Some pages quote the "
             "terms of the software they document (VCache, and Dynamite in MiscSWIs); those "
             "terms are the software's, not the manuals'.  BOX ships the manuals unaltered, as "
             "RISC OS Open distributes them in its RISC OS 5.30 disc image.")

    add("SparkFS", "SparkFS", "1.50+ (git, 2026)",
        "https://gitlab.riscosopen.org/RiscOS/Sources/FileSys/ImageFS/SparkFS",
        "!SparkFS in the ROM's Apps, its module and codecs",
        "CDDL 1.0 and Apache License 2.0, file by file; SparkZip adds the Info-ZIP licence",
        [("%s/LICENCE" % d, F(os.path.join(TP, "SparkFS", d, "LICENCE"))) for d in sparks])

    add("ChangeFSI", "ChangeFSI", "1.70 (%s)" % gitrev(os.path.join(TP, "ChangeFSI")),
        "https://gitlab.riscosopen.org/RiscOS/Sources/Apps/ChangeFSI",
        "!ChangeFSI in the ROM's Apps, and the CFSI module",
        "Apache License 2.0 and New BSD, IJG for JPEG/, btpc's terms (PhotoCD is left out)",
        [("ChangeFSI's LICENSE", F(os.path.join(TP, "ChangeFSI/LICENSE")))])

    add("PipeDream", "PipeDream", "4.63 (%s)" % gitrev(os.path.join(TP, "PipeDream")),
        "https://github.com/coltsoft/PipeDream",
        "Apps.!PipeDream and Documents.PipeDream. BOX's port of PipeDream, its changes and "
        "the C that replaces its assembler, is under PipeDream's licence",
        "Mozilla Public License 2.0",
        [("PipeDream's LICENCE", F(os.path.join(TP, "PipeDream/LICENCE"))),
         ("The Mozilla Public License 2.0", C("mpl-2.0.txt", "https://www.mozilla.org/media/MPL/2.0/index.txt"))])

    add("NetSurf", "NetSurf", "3.11+ (%s)" % gitrev(os.path.join(ns, "netsurf")),
        "https://www.netsurf-browser.org/",
        "Apps.!NetSurf, the web browser, and the libraries it is built from: " + ", ".join(ns_libs)
        + ". BOX's changes to NetSurf and its RISC OS front end are under NetSurf's licence",
        "GNU GPL 2 (NetSurf); MIT (its libraries)",
        [("netsurf/COPYING", F(os.path.join(ns, "netsurf/COPYING")))] +
        [("%s/COPYING" % l, F(os.path.join(ns, l, "COPYING"))) for l in ns_libs])

    add("Expat", "Expat", expat_v, "https://libexpat.github.io/", "!NetSurf (XML)", "MIT",
        [("COPYING", T(expat, "expat-%s/COPYING" % expat_v))])
    add("utf8proc", "utf8proc", utf8_v, "https://juliastrings.github.io/utf8proc/",
        "!NetSurf (Unicode)", "MIT, and the Unicode data licence",
        [("LICENSE.md", T(utf8, "utf8proc-%s/LICENSE.md" % utf8_v))])

    add("Paige", "Paige (HERMES-Paige)", "3.0b (%s)" % gitrev(os.path.join(TP, "Paige")),
        "https://github.com/nmatavka/HERMES-Paige",
        "!Write's text engine. BOX's changes to Paige are under Paige's licence",
        "GNU LGPL 2.1", [("Paige's LICENSE", F(os.path.join(TP, "Paige/LICENSE")))])

    add("StrongED", "StrongED", "4.70a22", "https://stronged.iconbar.com/",
        "Apps.!StrongED: BOX's StrongED is new C, which uses StrongED's own data -- its syntax "
        "modes, help, messages, templates and key maps -- from the 4.70a22 release. BOX's "
        "StrongED is under StrongED's licence",
        "3-clause BSD", [],
        note="StrongED's archives carry no licence file.  They say only \"(c) StrongED "
             "Developers, 1991-2026\".  The download page "
             "(https://stronged.iconbar.com/noframes/pages/download/down.html) says that "
             "4.69f7 and newer are distributed under the 3-clause BSD licence.")

    add("SQLite", "SQLite", "3.53.4", "https://www.sqlite.org/",
        "Resources.Dictionaries.spelldict (an SQLite database) and the box's SQLite module (ROSSQLITE)",
        "public domain", [("vendor/LICENSE", F(os.path.join(HERE, "ports/rossqlite/vendor/LICENSE")))])

    add("SymSpell", "SymSpell (symspell-c99, and the SymSpell English word list)", "2025",
        "https://github.com/sumanpokhrel-11/symspell-c99, https://github.com/wolfgarbe/SymSpell",
        "the spelling checker (ROSSPELL) and the word list behind Resources.Dictionaries.spelldict",
        "MIT", [("symspell-c99's LICENSE", F(os.path.join(HERE, "ports/rosspell/vendor/LICENSE"))),
                ("SymSpell's LICENSE (the frequency_en_82765 word list)",
                 C("symspell-LICENSE", "https://raw.githubusercontent.com/wolfgarbe/SymSpell/master/LICENSE"))])

    # Fonts
    add("Cabin", "Cabin (font)", "2011", "https://github.com/google/fonts/tree/main/ofl/cabin",
        fonts, "SIL Open Font License 1.1",
        [("OFL.txt", C("ofl-cabin.txt", "https://raw.githubusercontent.com/google/fonts/main/ofl/cabin/OFL.txt"))])
    add("Exo", "Exo (font)", "2011", "https://github.com/google/fonts/tree/main/ofl/exo",
        fonts, "SIL Open Font License 1.1",
        [("OFL.txt", C("ofl-exo.txt", "https://raw.githubusercontent.com/google/fonts/main/ofl/exo/OFL.txt"))])
    add("Fanwood", "Fanwood and Fanwood Text (fonts)", "2011",
        "https://github.com/google/fonts/tree/main/ofl/fanwoodtext",
        fonts, "SIL Open Font License 1.1",
        [("OFL.txt", C("ofl-fanwoodtext.txt", "https://raw.githubusercontent.com/google/fonts/main/ofl/fanwoodtext/OFL.txt"))])
    add("Simonetta", "Simonetta (font)", "2011-2012", "https://github.com/google/fonts/tree/main/ofl/simonetta",
        fonts, "SIL Open Font License 1.1",
        [("OFL.txt", C("ofl-simonetta.txt", "https://raw.githubusercontent.com/google/fonts/main/ofl/simonetta/OFL.txt"))])
    add("Roboto", "Roboto and Roboto Condensed (fonts)", "2011", "https://github.com/google/roboto",
        fonts, "Apache License 2.0",
        [("LICENSE", C("roboto-LICENSE", "https://raw.githubusercontent.com/googlefonts/roboto-2/main/LICENSE"))])
    add("FreeFont", "GNU FreeFont: FreeSans, FreeSerif, FreeMono", "20120503",
        "https://www.gnu.org/software/freefont/", fonts,
        "GNU GPL 3 or later, with the font exception",
        [("README (its Licensing section states the font exception)",
          C("freefont-README", "https://ftp.gnu.org/gnu/freefont/ (README; TeX Live's gnu-freefont has it)")),
         ("COPYING", C("freefont-COPYING", "https://ftp.gnu.org/gnu/freefont/ (COPYING; TeX Live's gnu-freefont has it)"))])
    add("AcornFonts", "Selwyn, Sidney, NewHall, System, Portrhouse and Sassoon (fonts)", "RISC OS 5.30",
        "https://www.riscosopen.org/", fonts + " (Selwyn and Sidney are also in the ROM's sources)",
        "Selwyn, Sidney: Apache License 2.0 (RISC OS Open's sources); the others: licence not known",
        [("ROMFonts' LICENSE (Selwyn and Sidney)",
          F(os.path.join(ROS, "Sources/Video/Render/Fonts/ROMFonts/LICENSE")))],
        note="Selwyn and Sidney are in RISC OS Open's ROMFonts sources, under the Apache License "
             "2.0, below.  NewHall, System (Fixed and Medium), Portrhouse and Sassoon (Primary) "
             "come from RISC OS 5.30's HardDisc4 image, which states no licence for them, and "
             "they are not in RISC OS Open's published sources: their licence is not known.  "
             "Sassoon is the Sassoon type design (Rosemary Sassoon and Adrian Williams).")

    # The Linux side: the kernel and what is in the initramfs
    add("Linux", "Linux", linux_v, "https://www.kernel.org/",
        "the kernel BOX runs on (both machines, and the PC image)",
        "GNU GPL 2, with the Linux syscall note",
        [("COPYING", T(tarball("linux-%s.tar.xz" % linux_v), "linux-%s/COPYING" % linux_v)),
         ("LICENSES/preferred/GPL-2.0", T(tarball("linux-%s.tar.xz" % linux_v), "linux-%s/LICENSES/preferred/GPL-2.0" % linux_v)),
         ("LICENSES/exceptions/Linux-syscall-note",
          T(tarball("linux-%s.tar.xz" % linux_v), "linux-%s/LICENSES/exceptions/Linux-syscall-note" % linux_v))])
    add("musl", "musl", musl_v, "https://musl.libc.org/",
        "the C library of /init, ssh, sshd and ksmbd-tools (through zig), of the box's clang "
        "and the libraries it links with, of the box's POSIX C library, and the Mojo "
        "compiler (*Mojo), linked statically (Alpine 3.24's musl-dev %s)" % musl_v,
        "MIT", [("COPYRIGHT", T(tarball("musl-%s.tar.gz" % musl_v), "musl-%s/COPYRIGHT" % musl_v))])
    add("LLVM", "LLVM (clang, lld, compiler-rt, libc++, libc++abi, libunwind)", llvm_v,
        "https://llvm.org/",
        "both boxes' clang and ld.lld (*CC, *RosBas), compiler-rt's builtins; libc++, "
        "libc++abi and libunwind in /init (by zig)",
        "Apache License 2.0 with LLVM Exceptions",
        [("LICENSE.TXT", F(os.path.join(llvm, "LICENSE.TXT"))),
         ("compiler-rt/LICENSE.TXT", F(os.path.join(llvm, "compiler-rt/LICENSE.TXT"))),
         ("libcxx/LICENSE.TXT", F(os.path.join(llvm, "libcxx/LICENSE.TXT"))),
         ("libcxxabi/LICENSE.TXT", F(os.path.join(llvm, "libcxxabi/LICENSE.TXT"))),
         ("libunwind/LICENSE.TXT", F(os.path.join(llvm, "libunwind/LICENSE.TXT")))])
    lwip_v = var("deps/get-lwip.sh", "V")
    add("lwIP", "lwIP (a lightweight TCP/IP stack)", lwip_v, "https://savannah.nongnu.org/projects/lwip/",
        "HybrisOS, the HAL: its TCP/IP stack (built into the HAL edition's kernel image, "
        "hal.img; not in the Linux boxes)",
        "BSD 3-clause",
        [("COPYING", F(next((p for p in (os.path.join(c, "lwip-%s" % lwip_v, "COPYING")
                                         for c in (CACHE, os.path.join(HERE, ".cache")))
                             if os.path.exists(p)), "lwip COPYING (deps/get-lwip.sh)")))])
    add("Zig", "Zig", zig_version(), "https://ziglang.org/",
        "the cross compiler /init and the Linux programs are built with; its compiler_rt is in them",
        "MIT", [("LICENSE", F(os.path.join(os.path.dirname(os.path.dirname(zl)), "LICENSE")))])
    add("mimalloc", "mimalloc", mi_v, "https://github.com/microsoft/mimalloc",
        "both boxes' clang and the Mojo compiler (its memory allocator)", "MIT",
        [("LICENSE", T(tarball("mimalloc-%s.tar.gz" % mi_v), "mimalloc-%s/LICENSE" % mi_v))])
    add("OpenSSL", "OpenSSL", ossl_v, "https://www.openssl.org/",
        "/init (AcornSSL, URL fetching), ssh and sshd", "Apache License 2.0",
        [("LICENSE.txt", T(tarball("openssl-%s.tar.gz" % ossl_v), "openssl-%s/LICENSE.txt" % ossl_v))])
    add("curl", "curl (libcurl)", curl_v, "https://curl.se/",
        "/init (AcornHTTP and the URL fetcher)", "curl licence (MIT-style)",
        [("COPYING", T(tarball("curl-%s.tar.xz" % curl_v), "curl-%s/COPYING" % curl_v))])
    add("zlib", "zlib", zlib_v, "https://zlib.net/", "/init (the ZLib module, CompressPNG), !NetSurf",
        "zlib licence", [("LICENSE", T(tarball("zlib-%s.tar.xz" % zlib_v), "zlib-%s/LICENSE" % zlib_v))])
    add("libpng", "libpng", png_v, "http://www.libpng.org/pub/png/libpng.html",
        "/init (CompressPNG, SpriteExtend's PNG, ChangeFSI's CFSI), !NetSurf",
        "libpng licence (PNG Reference Library License 2)",
        [("LICENSE", T(tarball("libpng-%s.tar.xz" % png_v), "libpng-%s/LICENSE" % png_v))])
    jt = tarball("libjpeg-turbo-%s.tar.gz" % jpeg_v)
    add("libjpeg-turbo", "libjpeg-turbo", jpeg_v, "https://libjpeg-turbo.org/",
        "/init (CompressJPEG, SpriteExtend's JPEG, ChangeFSI's CFSI), !NetSurf",
        "IJG licence, Modified (3-clause) BSD, zlib",
        [("LICENSE.md", T(jt, "libjpeg-turbo-%s/LICENSE.md" % jpeg_v)),
         ("README.ijg (the IJG licence)", T(jt, "libjpeg-turbo-%s/README.ijg" % jpeg_v))])
    add("OpenSSH", "OpenSSH", ssh_v, "https://www.openssh.com/",
        "ssh, ssh-keygen and sshd in the ROM (*SSH, logging in to the box)", "BSD-style (several)",
        [("LICENCE", T(tarball("openssh-%s.tar.gz" % ssh_v), "openssh-%s/LICENCE" % ssh_v))])
    add("ksmbd-tools", "ksmbd-tools", ksmbd_v, "https://github.com/cifsd-team/ksmbd-tools",
        "ksmbd.tools in the ROM (*Share, the box's SMB server)", "GNU GPL 2 or later",
        [("COPYING", T(tarball("ksmbd-tools-%s.tar.gz" % ksmbd_v), "ksmbd-tools-%s/COPYING" % ksmbd_v))])
    add("libnl", "libnl", libnl_v, "https://github.com/thom311/libnl", "ksmbd.tools", "GNU LGPL 2.1",
        [("COPYING", T(tarball("libnl-%s.tar.gz" % libnl_v), "libnl-%s/COPYING" % libnl_v))])
    add("GLib", "GLib", glib_v, "https://gitlab.gnome.org/GNOME/glib", "ksmbd.tools",
        "GNU LGPL 2.1 or later",
        [("COPYING", T(tarball("glib-%s.tar.xz" % glib_v), "glib-%s/COPYING" % glib_v))])
    smb2_c = var("deps/build-libsmb2.sh", "C")
    smb2_t = tarball("libsmb2-%s.tar.gz" % smb2_c)
    add("libsmb2", "libsmb2", "commit " + smb2_c[:10], "https://github.com/sahlberg/libsmb2",
        "/init (LanManFS: *LMConnect's SMB2 and SMB3 client)", "GNU LGPL 2.1 or later",
        [("COPYING", T(smb2_t, "libsmb2-%s/COPYING" % smb2_c)),
         ("LICENCE-LGPL-2.1.txt", T(smb2_t, "libsmb2-%s/LICENCE-LGPL-2.1.txt" % smb2_c))],
        note="libsmb2 is linked statically into /init, unchanged. To use a changed libsmb2, "
             "rebuild /init from BOX's sources (rosgd in "
             "https://github.com/albanread/A7232ToolChain): deps/build-libsmb2.sh builds the "
             "library from a source tree, and make links /init with it. BOX's own code is MIT, "
             "so all of the source /init is built from is available for that.")

    # The ARM container
    arm = "the ARM container in /init, which runs ARM code (runtime/armrun)"
    add("dynarmic", "dynarmic (Azahar's fork)", dyn_v[:10], "https://github.com/azahar-emu/dynarmic",
        arm, "0BSD", [("LICENSE.txt", F(os.path.join(dsrc, "LICENSE.txt")))])
    for f, n, sub, lic, lf, why in (
            ("fmt", "fmt", "fmt", "MIT, with fmt's binary exception", "LICENSE", arm),
            ("mcl", "mcl", "mcl", "MIT", "LICENSE", arm),
            ("oaknut", "oaknut (AArch64 assembler)", "oaknut", "MIT", "LICENSE", arm + ", on Apple Silicon"),
            ("robin-map", "robin-map", "robin-map", "MIT", "LICENSE", arm),
            ("xbyak", "xbyak (x86-64 assembler)", "xbyak", "3-clause BSD", "COPYRIGHT", arm + ", on the Intel box"),
            ("Zydis", "Zydis (x86-64 disassembler)", "zydis", "MIT", "LICENSE", arm + ", on the Intel box"),
            ("Zycore", "Zycore", "zycore", "MIT", "LICENSE", arm + ", on the Intel box")):
        add(f, n, gitrev(os.path.join(dsrc, "externals", sub)), "https://github.com/azahar-emu/dynarmic (externals/%s)" % sub,
            why, lic, [(lf, F(os.path.join(dsrc, "externals", sub, lf)))])
    add("Boost", "Boost (headers: icl, variant)", boost_v.replace("_", "."), "https://www.boost.org/",
        arm + " (dynarmic is built with them)", "Boost Software License 1.0",
        [("LICENSE_1_0.txt", T(tarball("boost_%s.tar.bz2" % boost_v), "boost_%s/LICENSE_1_0.txt" % boost_v))])

    # Mojo and what it runs on
    lic = os.path.join(mojo, "licenses")
    add("Mojo", "Mojo (the MojoRISCOS fork of Modular's Mojo)", "1.1",
        "https://github.com/albanread/MojoRISCOS (fork of https://github.com/modular/modular)",
        "*Mojo, BoxTools' Mojo compiler, and its standard library, under /usr/lib/mojo",
        "Apache License 2.0 with LLVM Exceptions",
        [("NOTICE", F(os.path.join(lic, "NOTICE"))),
         ("LICENSE-Modular-Apache-2.0-LLVM", F(os.path.join(lic, "LICENSE-Modular-Apache-2.0-LLVM"))),
         ("Third-Party-Notices", F(os.path.join(lic, "Third-Party-Notices")))],
        note="BOX's Mojo compiler is one static program linked against musl, with "
             "libstdc++, libgcc and mimalloc linked in; nothing else is installed beside it.  "
             "It and its standard library were built from the source of the "
             "MojoRISCOS fork of Modular's open-source repository: the compiler (KGEN), the "
             "standard library, AsyncRT and Support, every file of which carries the Apache "
             "License 2.0 with LLVM Exceptions, as the NOTICE above says.  No binary of "
             "Modular's was used, and MAX is not included.  Modular's Community License "
             "governs Modular's own SDK downloads and MAX, so it does not apply to what BOX "
             "ships.  \"Mojo\" is a trademark of Modular, Inc., used here to name the "
             "language.")
    add("GCC", "GCC runtime libraries: libstdc++ and libgcc (Alpine 3.24)", "15.2.0-r5",
        "https://gcc.gnu.org/", "*Mojo: the compiler's C++ runtime, linked into it",
        "GNU GPL 3 with the GCC Runtime Library Exception 3.1",
        [("COPYING.RUNTIME", C("gcc-COPYING.RUNTIME", "https://gcc.gnu.org/git/?p=gcc.git;a=blob_plain;f=COPYING.RUNTIME;hb=refs/tags/releases/gcc-15.2.0")),
         ("COPYING3", C("gcc-COPYING3", "https://gcc.gnu.org/git/?p=gcc.git;a=blob_plain;f=COPYING3;hb=refs/tags/releases/gcc-15.2.0"))])

    # Rust: the standard library and the crates
    add("Rust", "Rust's standard library (core, alloc, std, compiler-builtins)", rust_nightly(),
        "https://www.rust-lang.org/", "!PDF's module (rospdf) and pdfsvc (the printer driver's PDF writer)",
        "MIT or Apache License 2.0",
        [("MIT", F(os.path.join(rl, "MIT.txt"))), ("Apache-2.0", F(os.path.join(rl, "Apache-2.0.txt")))])
    used = {}
    for prog, d in (("!PDF (rospdf)", os.path.join(HERE, "ports/rospdf")),
                    ("pdfsvc", os.path.join(os.path.dirname(HERE), "pdfsvc"))):
        for k, (l, path) in crates(d).items():
            if k[0] in ("rospdf-core", "pdfsvc", "hostview"):
                continue
            used.setdefault(k, [l, path, []])[2].append(prog)
    texts = []
    lines = []
    for (n, v), (l, path, progs) in sorted(used.items()):
        lines.append("  %-28s %-9s %-36s %s" % (n, v, l, ", ".join(progs)))
        for f in sorted(os.listdir(path)) if path else []:
            if re.match(r"(?i)(licen[cs]e|copying|copyright|unlicense)", f) and os.path.isfile(os.path.join(path, f)):
                texts.append(("%s %s: %s" % (n, v, f), F(os.path.join(path, f))))
    add("RustCrates", "Rust crates (from crates.io)", "Cargo.lock's", "https://crates.io/",
        "!PDF (rospdf: hayro, the PDF interpreter, and vello_cpu, the rasteriser) and pdfsvc "
        "(pdf-writer)", "MIT, Apache 2.0, BSD, zlib, Unicode, Unlicense, per crate", texts,
        note="The crates, with the licence each declares:\n\n" + "\n".join(lines))
    return P


# ---- writing them ---------------------------------------------------------

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "disc/Documents/Licences")
    P = packages()
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    for p in P:
        head = [p["name"], "=" * len(p["name"]), ""]
        for k, v in (("Version:", p["version"]), ("From:", p["url"]),
                     ("Used by:", p["used"]), ("Licence:", p["licence"])):
            head += textwrap.wrap(v, 76, initial_indent="%-10s" % k, subsequent_indent=" " * 10,
                                  break_on_hyphens=False, break_long_words=False)
        head.append("")
        body = []
        if p["note"]:
            for para in p["note"].split("\n"):
                body += textwrap.wrap(para, 76, break_on_hyphens=False, break_long_words=False) \
                    if not para.startswith(" ") else [para]
                if not para:
                    body.append("")
            body.append("")
        seen = {}
        for label, src in p["texts"]:
            text = latin1(read(src)).rstrip() + "\n"
            h = hashlib.sha256(text.encode("latin-1")).hexdigest()
            body += [RULE, label, RULE, ""]
            if h in seen:
                body += ["The same text as %s, above." % seen[h], ""]
            else:
                seen[h] = label
                body += [text]
        doc = "\n".join(head + body).rstrip() + "\n"
        with open(os.path.join(out, p["file"] + ",fff"), "wb") as fh:
            fh.write(doc.encode("latin-1"))
    w = max(len(p["file"]) for p in P)
    vw = max(len(p["version"]) for p in P)
    idx = ["Licences", "========", "",
           "BOX does not claim ownership of any code that the project did not",
           "originally write.  All of the code will be published when it is ready.", "",
           "The licence of every package BOX is made from, one file each, named after",
           "the package.  Each file gives the version, where it comes from, what in BOX",
           "uses it, and then the licence as the package ships it.  BOX's own code is",
           "under the MIT licence (the file BOX).  Code BOX has translated or ported",
           "keeps the licence of the package it came from.", "",
           "Made by tools/mklicences.py in BOX's sources.", "",
           "Not known: the licences of the fonts NewHall, System, Portrhouse and Sassoon",
           "(AcornFonts), and of the picture Docs.Apple, whose origin is not recorded.",
           "Mojo is built from Modular's open-source compiler and standard library",
           "(Apache 2.0 with LLVM Exceptions); MAX is not included (the file Mojo).", "",
           "%-*s  %-*s  %s" % (w, "File", vw, "Version", "Licence"),
           "%-*s  %-*s  %s" % (w, "-" * w, vw, "-" * vw, "-" * (76 - w - vw))]
    for p in sorted(P, key=lambda p: p["file"].lower()):
        lic = textwrap.wrap(p["licence"], 78 - w - vw - 4, break_on_hyphens=False)
        idx.append("%-*s  %-*s  %s" % (w, p["file"], vw, p["version"], lic[0]))
        idx += [" " * (w + vw + 4) + l for l in lic[1:]]
    with open(os.path.join(out, "ReadMe,fff"), "wb") as fh:
        fh.write(("\n".join(idx) + "\n").encode("latin-1"))
    print("mklicences: %d packages in %s" % (len(P), out))


if __name__ == "__main__":
    main()
