#!/usr/bin/env python3
"""BBC BASIC text to a tokenised program, as BASIC itself does it.

    basictok.py BASICSRC IN.bas OUT        one program
    basictok.py BASICSRC --tree SRC DST    every ,fd1 file under SRC, as ,ffb
                                           under DST (other files copied)

BASICSRC is RISC OS's BASIC source directory (Sources/Programmer/BASIC).
The keywords, their token values and how each is treated are read from it
(s/Lexical's table, hdr/Tokens), and the tokeniser is a transliteration of
BASIC's own: MATCH, which crunches one line (s/Lexical), and INSRT, which
stores it (s/Basic) -- what TEXTLOAD does to a text file.  So a program
comes out as RISC OS's BASIC would save it, byte for byte, and nothing about
the language is retyped here.

The file format: each line is 13, the line number high and low, the line's
length (these four bytes included), then its tokenised text; the program
ends 13, 255.

Used to put the example programs on the PC image's RISC OS disc
(disc/), whose sources are text.
"""
import os
import re
import shutil
import sys


# ---- BASIC's tables --------------------------------------------------------

def tokens(path):
    """hdr/Tokens: '^ base' then 'NAME # n' allocates"""
    values, at = {}, 0
    for line in open(path, encoding="latin-1"):
        line = line.split(";")[0].rstrip()
        m = re.match(r"^\s+\^\s+&([0-9A-Fa-f]+)", line)
        if m:
            at = int(m.group(1), 16)
            continue
        m = re.match(r"^(\w+)\s+#\s+(\d+)", line)
        if m:
            values[m.group(1)] = at
            at += int(m.group(2))
    return values


def keywords(path, values):
    """s/Lexical's table, PLEXA to WIDTH, in order: (word, token, job)"""
    table, on = [], False
    for line in open(path, encoding="latin-1"):
        if line.startswith("PLEXA"):
            on = True
        if not on:
            continue
        m = re.match(r'^\S*\s+=\s+"([^"]+)",(\w+),([\d+]+)', line)
        if m:
            job = sum(int(x) for x in m.group(3).split("+"))
            table.append((m.group(1).encode("latin-1"), values[m.group(2)], job))
            if m.group(2) == "TWIDTH":
                break
    return table


class Basic:
    def __init__(self, src):
        self.t = tokens(os.path.join(src, "hdr", "Tokens"))
        self.table = keywords(os.path.join(src, "s", "Lexical"), self.t)
        self.index = {}
        for i, (word, _, _) in enumerate(self.table):
            self.index.setdefault(word[0], i)
        self.listop = 0             # LISTO when a program is loaded: 0

    # WORDCQ: a character of a name
    @staticmethod
    def wordc(c):
        return 48 <= c <= 57 or 65 <= c <= 90 or 95 <= c <= 122

    @staticmethod
    def numbc(c):
        return 48 <= c <= 57

    def consti(self, n):
        """CONSTI: a line number, as the three bytes after TCONST"""
        return bytes(((((n >> 8) >> 4) & 0x0C | (n & 0xC0) >> 2) ^ 0x54,
                      (n & 0x3F) | 0x40, ((n >> 8) & 0x3F) | 0x40))

    def match(self, src):
        """MATCH: one line (src ends with 13), crunched; the constant mode
        and left mode it starts in are the ones a program line gets"""
        t = self.t
        out = bytearray()
        i = 0
        smode, consta, mode = 0, t["TCONST"], 0

        def get():
            nonlocal i
            c = src[i]
            i += 1
            return c

        state = "99"
        c = 0
        while True:
            if state == "99":                       # copy a byte, assuming no token
                c = get()
                out.append(c)
                state = "00"
            if state == "00":
                if c == 32:
                    state = "99"
                    continue
                if c == 10:
                    c = 13
                    out[-1] = 13
                if c == 13:
                    return bytes(out), smode
                if c == 34:
                    smode ^= 1
                if smode & 255:
                    state = "99"
                    continue
                if smode >= 0:                      # (a negative level stays so)
                    if c == 40:
                        smode += 0x1000
                    if c == 41:
                        smode -= 0x1000
                if c == 38:                         # &: hex digits copied
                    while True:
                        c = get()
                        out.append(c)
                        if self.numbc(c) or 65 <= c < 71 or 97 <= c < 103:
                            continue
                        break
                    if c < 65:
                        state = "00"
                        continue
                state = "10"
            if state == "10":
                if c == 58:                         # ':' a new statement
                    consta, mode = 0, 0
                    state = "99"
                    continue
                if c == 44:
                    state = "99"
                    continue
                if c == 42:                         # '*': a star command, left
                    if mode == 0:
                        smode |= 4
                        state = "99"
                        continue
                    consta, mode = 0, 1
                    state = "99"
                    continue
                if c == 46:
                    state = "MATCHZ"
                elif self.numbc(c):
                    if consta == 0:
                        state = "MATCHZ"
                    else:
                        save = (i, smode, consta, mode)
                        n = c & 15
                        while True:
                            c = get()
                            if not self.numbc(c):
                                break
                            n = n * 10 + (c & 15)
                            if n >= 65280:
                                break
                        if self.numbc(c):           # too large: not a constant
                            i, smode, consta, mode = save
                            smode |= 256
                            state = "MATCHZ"
                        else:
                            out[-1] = consta
                            out += self.consti(n)
                            out.append(c)
                            smode, consta, mode = save[1], save[2], save[3]
                            state = "00"
                            continue
                else:
                    state = "30"
            if state == "MATCHZ":
                while True:
                    c = get()
                    out.append(c)
                    if not (self.numbc(c) or c == 46):
                        break
                consta, mode = 0, 1
                state = "00"
                continue
            if state == "30":
                if c < 65:
                    consta, mode = 0, 1
                    state = "99"
                    continue
                if c > 87:                          # past W: no keyword
                    if self.wordc(c):
                        state = "MATCHH"
                    else:
                        consta, mode = 0, 1
                        state = "99"
                        continue
                else:
                    state = self.keyword(src, out, c, i)
                    if isinstance(state, tuple):
                        i, smode, consta, mode, state = self.apply(state, src, out, smode,
                                                                   consta, mode)
                        continue
                    state = "MATCHH"
            if state == "MATCHH":                   # the rest of a name, copied
                while True:
                    c = get()
                    out.append(c)
                    if not self.wordc(c):
                        break
                consta, mode = 0, 1
                state = "00"
                continue

    def keyword(self, src, out, c, i):
        """The table's entries for c, in order: the first that matches"""
        k = self.index.get(c)
        if k is None:
            return "MATCHH"
        while k < len(self.table):
            word, token, job = self.table[k]
            if word[0] != c:
                return "MATCHH"
            j = i
            ok = True
            for w in word[1:]:
                s = src[j]
                j += 1
                if s == w:
                    continue
                if s == 46:                         # abbreviated with '.'
                    break
                ok = False
                break
            if ok:
                return ("found", k, j, i)
            if token == self.t["TWIDTH"]:
                return "MATCHH"
            k += 1
        return "MATCHH"

    def apply(self, found, src, out, smode, consta, mode):
        """What a keyword does: the job byte's bits"""
        t = self.t
        _, k, j, start = found
        word, token, job = self.table[k]
        if mode == 1 and token == t["TTRACE"]:
            job &= ~16
        if job & 1 and self.wordc(src[j]):         # a name that begins with it
            return start, smode, consta, mode, "MATCHH"
        if job & 8:                                 # two-byte token
            esc = t["TESCCOM"]
            if job & 64:
                esc = t["TESCSTMT"]
            if job & 4:
                esc = t["TESCFN"]
                job &= ~4
            out[-1] = esc
            out.append(token)
        else:
            if job & 64 and mode == 0:              # the left-hand form
                token += t["TPTR2"] - 0x8F
            out[-1] = token
        i = j
        if job & 2:
            mode, consta = 1, 0
        if job & 4:
            mode, consta = 0, 0
        if token in (t["TFN"], t["TPROC"]):
            while self.wordc(src[i]):               # the name, as it is
                out.append(src[i])
                i += 1
        if job & 16:
            consta = t["TCONST"]
        if job & 32:
            smode |= 4                              # REM, DATA: the rest as it is
        if job & 128:
            smode += 0x1000
        return i, smode, consta, mode, "99"

    def program(self, text):
        """TEXTLOAD: each line crunched and stored in line-number order"""
        t = self.t
        lines = {}
        for raw in text.replace(b"\r\n", b"\n").split(b"\n"):
            if not raw.strip():
                continue
            crunched, _ = self.match(raw + b"\r")
            p = 0
            while crunched[p] == 32:
                p += 1
            if crunched[p] != t["TCONST"]:
                raise ValueError(f"a line without a number: {raw!r}")
            b0, b1, b2 = crunched[p + 1:p + 4]
            r0 = b0 << 2
            r1 = (r0 & 0xC0) ^ b1
            r0 = (b2 ^ (r0 << 2)) & 255
            number = r1 | r0 << 8
            body = crunched[p + 4:]
            if body[0] == 13:
                lines.pop(number, None)            # a bare number deletes
                continue
            if self.listop:
                while body[0] == 32:
                    body = body[1:]
            body = body[:-1].rstrip(b" ")
            q = 0
            while q < len(body) and body[q] == 32:
                q += 1
            if q < len(body) and body[q] == t["TELSE"]:
                body = body[:q] + bytes((t["TELSE2"],)) + body[q + 1:]
            if len(body) + 4 >= 256:
                raise ValueError(f"line {number} is too long")
            lines[number] = body
        out = bytearray()
        for number in sorted(lines):
            body = lines[number]
            out += bytes((13, number >> 8, number & 255, len(body) + 4)) + body
        return bytes(out + b"\r\xff")


def main():
    if len(sys.argv) == 4:
        b = Basic(sys.argv[1])
        open(sys.argv[3], "wb").write(b.program(open(sys.argv[2], "rb").read()))
    elif len(sys.argv) == 5 and sys.argv[2] == "--tree":
        b = Basic(sys.argv[1])
        src, dst = sys.argv[3], sys.argv[4]
        for root, dirs, files in os.walk(src):
            rel = os.path.relpath(root, src)
            os.makedirs(os.path.join(dst, rel), exist_ok=True)
            for f in files:
                if f == ".DS_Store":
                    continue
                s = os.path.join(root, f)
                if f.lower().endswith(",fd1"):
                    d = os.path.join(dst, rel, f[:-4] + ",ffb")
                else:
                    d = os.path.join(dst, rel, f)
                # a copy made before, read-only as disc/'s is (RISC OS
                # 5.30's !Fonts): taken away, so it can be made again
                if os.path.lexists(d) and not os.access(d, os.W_OK):
                    os.remove(d)
                if f.lower().endswith(",fd1"):
                    open(d, "wb").write(b.program(open(s, "rb").read()))
                else:
                    shutil.copyfile(s, d)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
