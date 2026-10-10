#!/usr/bin/env python3
"""bascrunch.py N IN OUT -- RISC OS's BasCrunch, for this machine.

RISC OS builds a BASIC application's !RunImage with Library/Build's
BasCrunch: BASIC itself runs `TEXTLOAD in`, `CRUNCH N`, `SAVE out`
(BasCrunch2).  No BASIC runs in this build, so this is those three
commands transliterated from BASIC's own source (Programmer/BASIC):

  TEXTLOAD  s/Command LOADFILEFINAL: each line of text tokenised by
            s/Lexical MATCH, put at the end of the program by s/Basic
            INSERT (trailing spaces off, a leading ELSE made ELSE2),
            then renumbered from 10 in steps of 1 (s/Command RENUM1,
            line number references included), as a text file without
            line numbers is
  CRUNCH    s/Expr CRUNCHROUTINE, the mask's bits: 1 spaces at the start
            of a statement, 2 spaces in it, 4 REMs (not the first
            line's), 8 a ':' before the end of a statement, 16 empty lines
  SAVE      the program, PAGE to TOP, filetype BASIC

The result is the file BASIC writes, byte for byte: tests/desktop/romapps/
check.py's crunch holds the ROM's programs (tools/mkromapps.py) against
BASIC here running Library/Build's BasCrunch itself.
Lines that carry their own numbers are refused: TEXTLOAD's INSRT (a
numbered line replacing another) is not transliterated, and no ROM
application's source has them.

A tokenised input (it starts with a program's CR and its lines chain to
the CR &FF end) is taken as it is, as TEXTLOAD takes it.

    bascrunch.py [-a] -1 IN OUT     IN may be several files: -a appends
                                    them first, as the build's FAppend
"""
import sys

# hdr/Tokens
TOTHER = 0x7F
(TAND, TDIV, TEOR, TMOD, TOR, TERROR, TLINE, TOFF, TSTEP, TSPC, TTAB, TELSE,
 TTHEN, TCONST, TOPENU, TPTR, TPAGE, TTIME, TLOMEM, THIMEM, TABS, TACS, TADC,
 TASC, TASN, TATN, TBGET, TCOS, TCOUNT, TDEG, TERL, TERR, TEVAL, TEXP, TEXT,
 TFALSE, TFN, TGET, TINKEY, TINSTR, TINT, TLEN, TLN, TLOG, TNOT, TOPENI,
 TOPENO, TPI, TPOINT, TPOS, TRAD, TRND, TSGN, TSIN, TSQR, TTAN, TTO, TTRUE,
 TUSR, TVAL, TVPOS, TCHRD, TGETD, TINKED, TLEFTD, TMIDD, TRIGHTD, TSTRD,
 TSTRND, TEOF, TESCFN, TESCCOM, TESCSTMT, TWHEN, TOF, TENDCA, TELSE2, TENDIF,
 TENDWH, TPTR2, TPAGE2, TTIME2, TLOMM2, THIMM2, TBEEP, TBPUT, TCALL, TCHAIN,
 TCLEAR, TCLOSE, TCLG, TCLS, TDATA, TDEF, TDIM, TDRAW, TEND, TENDPR, TENVEL,
 TFOR, TGOSUB, TGOTO, TGRAPH, TIF, TINPUT, TLET, TLOCAL, TMODE, TMOVE, TNEXT,
 TON, TVDU, TPLOT, TPRINT, TPROC, TREAD, TREM, TREPEAT, TREPORT, TRESTORE,
 TRETURN, TRUN, TSTOP, TTEXT, TTRACE, TUNTIL, TWIDTH, TOSCL) = range(0x80, 0x100)
TSUM, TBEAT = 0x8E, 0x8F
(TCASE, TCIRCLE, TFILL, TORGIN, TPSET, TRECT, TSWAP, TWHILE, TWAIT, TMOUSE,
 TQUIT, TSYS, TINSTALLBAD, TLIBRARY, TTINT, TELLIPSE, TBEATS, TTEMPO, TVOICES,
 TVOICE, TSTEREO, TOVERLAY) = range(0x8E, 0x8E + 22)
(TAPPEND, TAUTO, TCRUNCH, TDELET, TEDIT, THELP, TLIST, TLOAD, TLVAR, TNEW,
 TOLD, TRENUM, TSAVE, TTEXTLOAD, TTEXTSAVE, TTWIN, TTWINO,
 TINSTALL) = range(0x8E, 0x8E + 18)
assert TOSCL == 0xFF and TELSE == 0x8B and TCONST == 0x8D and TPAGE2 == 0xD0

# s/Lexical's table, in its order: the name, the token and the job --
#   128 contains own bracket, 64 polymorphic (two byte: a statement),
#   32 give up completely, 16 constants may follow, 8 two byte token,
#   4 to left mode (two byte: a function), 2 to right mode,
#   1 not if the next character is a word's
LEX = [
    ("AND", TAND, 2), ("ABS", TABS, 0), ("ACS", TACS, 0), ("ADVAL", TADC, 0),
    ("ASC", TASC, 0), ("ASN", TASN, 0), ("ATN", TATN, 0), ("AUTO", TAUTO, 16 + 8),
    ("APPEND", TAPPEND, 8 + 2),
    ("BGET", TBGET, 1), ("BPUT", TBPUT, 2 + 1), ("BEATS", TBEATS, 64 + 8 + 2),
    ("BEAT", TBEAT, 8 + 4 + 2),
    ("COLOUR", TTEXT, 2), ("CALL", TCALL, 2), ("CASE", TCASE, 64 + 8 + 2),
    ("CHAIN", TCHAIN, 2), ("CHR$", TCHRD, 0), ("CLEAR", TCLEAR, 1),
    ("CLOSE", TCLOSE, 2 + 1), ("CLG", TCLG, 1), ("CLS", TCLS, 1), ("COS", TCOS, 0),
    ("COUNT", TCOUNT, 1), ("CIRCLE", TCIRCLE, 64 + 8 + 2), ("CRUNCH", TCRUNCH, 8 + 2),
    ("COLOR", TTEXT, 2),
    ("DATA", TDATA, 32), ("DEG", TDEG, 0), ("DEF", TDEF, 0), ("DELETE", TDELET, 16 + 8),
    ("DIV", TDIV, 0), ("DIM", TDIM, 2), ("DRAW", TDRAW, 2),
    ("ENDPROC", TENDPR, 1), ("EDIT", TEDIT, 32 + 8), ("ENDWHILE", TENDWH, 1),
    ("ENDCASE", TENDCA, 1), ("ENDIF", TENDIF, 1), ("END", TEND, 1),
    ("ENVELOPE", TENVEL, 2), ("ELSE", TELSE, 16 + 4), ("EVAL", TEVAL, 0),
    ("ERL", TERL, 1), ("ERROR", TERROR, 4), ("EOF", TEOF, 1), ("EOR", TEOR, 2),
    ("ERR", TERR, 1), ("EXP", TEXP, 0), ("EXT", TEXT, 1),
    ("ELLIPSE", TELLIPSE, 64 + 8 + 2),
    ("FOR", TFOR, 2), ("FALSE", TFALSE, 1), ("FILL", TFILL, 64 + 8 + 2), ("FN", TFN, 2),
    ("GOTO", TGOTO, 16 + 2), ("GET$", TGETD, 0), ("GET", TGET, 0),
    ("GOSUB", TGOSUB, 16 + 2), ("GCOL", TGRAPH, 2),
    ("HIMEM", THIMEM, 64 + 2 + 1), ("HELP", THELP, 8 + 1),
    ("INPUT", TINPUT, 2), ("IF", TIF, 2), ("INKEY$", TINKED, 0), ("INKEY", TINKEY, 0),
    ("INT", TINT, 0), ("INSTR(", TINSTR, 128), ("INSTALL", TINSTALL, 8 + 2),
    ("LIST", TLIST, 16 + 8), ("LINE", TLINE, 2), ("LOAD", TLOAD, 2 + 8),
    ("LOMEM", TLOMEM, 64 + 2 + 1), ("LOCAL", TLOCAL, 2), ("LEFT$(", TLEFTD, 128),
    ("LEN", TLEN, 0), ("LET", TLET, 4), ("LOG", TLOG, 0), ("LN", TLN, 0),
    ("LIBRARY", TLIBRARY, 64 + 8 + 2), ("LVAR", TLVAR, 8 + 1),
    ("MID$(", TMIDD, 128), ("MODE", TMODE, 2), ("MOD", TMOD, 0), ("MOVE", TMOVE, 2),
    ("MOUSE", TMOUSE, 64 + 8 + 2),
    ("NEXT", TNEXT, 2), ("NEW", TNEW, 8 + 1), ("NOT", TNOT, 0),
    ("OLD", TOLD, 8 + 1), ("ON", TON, 2), ("OFF", TOFF, 0), ("OF", TOF, 0),
    ("ORIGIN", TORGIN, 64 + 8 + 2), ("OR", TOR, 2), ("OPENIN", TOPENU, 0),
    ("OPENOUT", TOPENO, 0), ("OPENUP", TOPENI, 0), ("OSCLI", TOSCL, 2),
    ("OTHERWISE", TOTHER, 4), ("OVERLAY", TOVERLAY, 64 + 8 + 2),
    ("PRINT", TPRINT, 2), ("PAGE", TPAGE, 64 + 2 + 1), ("PTR", TPTR, 64 + 2 + 1),
    ("PI", TPI, 1), ("PLOT", TPLOT, 2), ("POINT(", TPOINT, 128),
    ("POINT", TPSET, 64 + 8 + 2), ("PROC", TPROC, 2), ("POS", TPOS, 1),
    ("QUIT", TQUIT, 64 + 8 + 2),
    ("RETURN", TRETURN, 1), ("REPEAT", TREPEAT, 0), ("REPORT", TREPORT, 1),
    ("READ", TREAD, 2), ("REM", TREM, 32), ("RUN", TRUN, 1), ("RAD", TRAD, 0),
    ("RESTORE", TRESTORE, 16 + 2), ("RIGHT$(", TRIGHTD, 128), ("RND", TRND, 1),
    ("RECTANGLE", TRECT, 64 + 8 + 2), ("RENUMBER", TRENUM, 16 + 8),
    ("STEP", TSTEP, 0), ("SAVE", TSAVE, 8 + 2), ("SGN", TSGN, 0), ("SIN", TSIN, 0),
    ("SQR", TSQR, 0), ("SOUND", TBEEP, 2), ("SPC", TSPC, 0), ("STR$", TSTRD, 0),
    ("STRING$(", TSTRND, 128), ("STOP", TSTOP, 1), ("STEREO", TSTEREO, 64 + 8 + 2),
    ("SUM", TSUM, 8 + 4 + 2), ("SWAP", TSWAP, 64 + 8 + 2), ("SYS", TSYS, 64 + 8 + 2),
    ("TAN", TTAN, 0), ("TAB(", TTAB, 128), ("TEMPO", TTEMPO, 64 + 8 + 2),
    ("TEXTLOAD", TTEXTLOAD, 8 + 2), ("TEXTSAVE", TTEXTSAVE, 8 + 2),
    ("THEN", TTHEN, 16 + 4), ("TIME", TTIME, 64 + 2 + 1), ("TINT", TTINT, 64 + 8 + 2),
    ("TO", TTO, 0), ("TRACE", TTRACE, 16 + 2), ("TRUE", TTRUE, 1),
    ("TWINO", TTWINO, 8 + 2), ("TWIN", TTWIN, 8 + 1),
    ("UNTIL", TUNTIL, 2), ("USR", TUSR, 0),
    ("VDU", TVDU, 2), ("VAL", TVAL, 0), ("VPOS", TVPOS, 1),
    ("VOICES", TVOICES, 64 + 8 + 2), ("VOICE", TVOICE, 64 + 8 + 2),
    ("WHILE", TWHILE, 64 + 8 + 2), ("WHEN", TWHEN, 2), ("WAIT", TWAIT, 64 + 8 + 1),
    ("WIDTH", TWIDTH, 2),
]
# INDEXTAB: each letter's entries run from its first to the next letter's
INDEX = {}
for n, (name, _, _) in enumerate(LEX):
    INDEX.setdefault(name[0], n)


def wordc(c):
    """WORDCQ: 0-9, A-Z, and _ to z"""
    return 0x30 <= c <= 0x39 or 0x41 <= c <= 0x5A or 0x5F <= c <= 0x7A


def digit(c):
    return 0x30 <= c <= 0x39


def consti(n):
    """CONSTI: a line number's three bytes after TCONST"""
    return bytes([(((n >> 12) & 0x0C) | ((n & 0xC0) >> 2)) ^ 0x54,
                  (n & 0x3F) | 0x40, ((n >> 8) & 0x3F) | 0x40])


def getn(b):
    """SPGETN: the line number from CONSTI's three bytes"""
    r0 = (b[0] << 2) & 0xFFFFFFFF
    r1 = (r0 & 0xC0) ^ b[1]
    r0 = (b[2] ^ (r0 << 2)) & 0xFF
    return r1 | r0 << 8


def match(src, i):
    """MATCH: the line of text at src[i] (ended by CR or LF) tokenised;
    returns it with its CR, and where the next line starts"""
    out = bytearray()
    smode, consta, mode = 0, TCONST, 0
    step = 99
    r0 = 0
    stack = []
    while True:
        if step == 99:
            r0 = src[i] if i < len(src) else 13      # the file's end ends its last line
            i += 1
            out.append(r0)
            step = 0
        if step == 0:
            if r0 == 0x20:
                step = 99
                continue
            if r0 == 10:
                r0 = 13
                out[-1] = 13
            if r0 == 13:
                return bytes(out), i
            if r0 == 0x22:
                smode ^= 1
            if smode & 0xFF:
                step = 99
                continue
            if not smode & 0x80000000:
                if r0 == 0x28:
                    smode = (smode + 0x1000) & 0xFFFFFFFF
                if r0 == 0x29:
                    smode = (smode - 0x1000) & 0xFFFFFFFF
            step = 5
        if step == 5:
            if r0 == 0x26:                           # &: hex digits copied
                while True:
                    r0 = src[i]
                    i += 1
                    out.append(r0)
                    if digit(r0) or 0x41 <= r0 < 0x47 or 0x61 <= r0 < 0x67:
                        continue
                    break
                if r0 < 0x41:
                    step = 0
                    continue
            step = 10
        if step == 10:
            if r0 == 0x3A:                           # : a statement starts
                consta, mode = 0, 0
                step = 99
                continue
            if r0 == 0x2C:
                step = 99
                continue
            if r0 == 0x2A:                           # *
                if mode == 0:
                    smode |= 4
                    step = 99
                    continue
                step = "Y"
            else:
                step = 20
        if step == "Y":
            consta, mode = 0, 1
            step = 99
            continue
        if step == 20:
            if r0 == 0x2E:
                step = "Z"
            elif not digit(r0):
                step = 30
            elif consta == 0:
                step = "Z"
            else:                                    # a line number constant?
                saved = (i, smode, consta, mode)
                n = r0 & 15
                while True:
                    r0 = src[i]
                    i += 1
                    if not digit(r0):
                        break
                    n = n * 10 + (r0 & 15)
                    if n >= 65280:
                        break
                if n < 65280 and not digit(r0):
                    out[-1] = consta
                    out += consti(n)
                    out.append(r0)
                    smode, consta, mode = saved[1:]
                    step = 0
                    continue
                i, smode, consta, mode = saved        # too large: not a constant
                smode |= 256
                step = "Z"
        if step == "Z":                              # MATCHZ: a number copied
            while True:
                r0 = src[i]
                i += 1
                out.append(r0)
                if not (digit(r0) or r0 == 0x2E):
                    break
            consta, mode = 0, 1
            step = 0
            continue
        if step == 30:
            if r0 < 0x41:
                step = "Y"
                continue
            if r0 > 0x57:                            # past W: no keyword
                step = "H" if wordc(r0) else "Y"
                continue
            stack.append(smode)
            found = None
            n = INDEX.get(chr(r0), len(LEX))
            while n < len(LEX) and LEX[n][0][0] == chr(r0):
                name, tok, job = LEX[n]
                r8 = i
                k = 1
                r7 = r0
                while k < len(name):
                    r7 = src[i]
                    i += 1
                    if ord(name[k]) == r7:
                        k += 1
                        continue
                    if r7 == 0x2E:                   # abbreviated
                        k = len(name)
                        break
                    i = r8
                    break
                if k == len(name):
                    found = (tok, job, r7, r8)
                    break
                if tok == TWIDTH:
                    break
                n += 1
            if found is None:
                smode = stack.pop()
                step = "H"
                continue
            r6, job, r7, r8 = found
            if mode == 1 and r6 == TTRACE:
                job &= ~16
            if (mode == 1 and r6 == TPRINT and r7 == 0x2E and src[i - 2] == 0x50
                    and out[-2] == TVDU and src[i - 3] == 0x55):
                out.append(0x50)                     # VDUP., VFP's opcode, not VDU PRINT
                r6, job = 0x2E, 4
            else:
                if job & 1 and i < len(src) and wordc(src[i]):
                    i = r8
                    smode = stack.pop()
                    step = "H"
                    continue
                if job & 8:
                    esc = TESCCOM
                    if job & 64:
                        esc = TESCSTMT
                    if job & 4:
                        esc = TESCFN
                        job &= ~4
                    out[-1] = esc
                    out.append(0)
                elif job & 64 and mode == 0:
                    r6 += TPTR2 - TPTR
            out[-1] = r6
            if job & 2:
                mode, consta = 1, 0
            if job & 4:
                mode, consta = 0, 0
            if r6 in (TFN, TPROC):                   # the name is not tokenised
                while True:
                    r0 = src[i]
                    i += 1
                    out.append(r0)
                    if not wordc(r0):
                        break
                i -= 1
                del out[-1]
            if job & 16:
                consta = TCONST
            smode = stack.pop()
            if job & 32:
                smode |= 4
            if job & 128:
                smode = (smode + 0x1000) & 0xFFFFFFFF
            step = 99
            continue
        if step == "H":                              # MATCHH: the rest of a word copied
            while True:
                r0 = src[i]
                i += 1
                out.append(r0)
                if not wordc(r0):
                    break
            consta, mode = 0, 1
            step = 0
            continue


def is_program(data):
    """LOADFILEINCORE: a tokenised program, its lines chained to CR &FF"""
    p = 0
    while p + 3 < len(data) and data[p] == 13:
        if data[p + 1] == 0xFF:
            return True
        if data[p + 3] < 4:
            return False
        p += data[p + 3]
    return p + 1 < len(data) and data[p] == 13 and data[p + 1] == 0xFF


def textload(src):
    """TEXTLOAD: the lines, each [number, tokenised text without its CR]"""
    lines = []
    i = 0
    while i < len(src):
        tok, i = match(src, i)
        body = tok[:-1]
        if body.lstrip(b" ")[:1] == bytes([TCONST]):
            raise SystemExit("bascrunch: a line with its own number: not transliterated")
        # INSERT: trailing spaces off, but never the line's first character
        end = len(body)
        while end > 1 and body[end - 1] == 0x20:
            end -= 1
        body = bytearray(body[:end])
        if len(body) + 4 >= 256:
            raise SystemExit("bascrunch: Line too long")
        # INSLP1: past leading spaces, an ELSE starting the line is ELSE2
        k = 0
        while k < len(body) and body[k] == 0x20:
            k += 1
        if k < len(body) and body[k] == TELSE:
            body[k] = TELSE2
        lines.append([9, bytes(body)])
    # RENUM1: 10 on in steps of 1, then the references (after TCONST,
    # outside strings, REM and DATA)
    old = [n for n, _ in lines]
    for k, line in enumerate(lines):
        if 10 + k >= 65280:
            raise SystemExit("bascrunch: Renumber space")
        line[0] = 10 + k
    for line in lines:
        body = bytearray(line[1])
        p = 0
        state = 0
        while p < len(body):
            c = body[p]
            p += 1
            if c == 0x22:
                state ^= 1
            if state:
                continue
            if c in (TREM, TDATA):
                state = 2
            if c == TCONST:
                ref = getn(body[p:p + 3])
                if ref in old:
                    body[p:p + 3] = consti(lines[old.index(ref)][0])
                else:
                    print(f"Failed with {ref} on line {line[0]}", file=sys.stderr)
                p += 3
        line[1] = bytes(body)
    return lines


def program(lines):
    out = bytearray([13])
    for n, body in lines:
        out += bytes([n >> 8, n & 0xFF, len(body) + 4]) + body + b"\r"
    return bytes(out + b"\xff")


def crunch(prog, mask):
    """CRUNCHROUTINE over a program in memory, returning the new one"""
    src = prog
    dst = bytearray()
    r1 = 0

    def rd():
        nonlocal r1
        c = src[r1]
        r1 += 1
        return c

    first = True
    dst.append(rd())                                 # the CR before the first line
    while True:
        # CRUNCHLINE
        hi = rd()
        dst.append(hi)
        if hi == 0xFF:
            return bytes(dst)
        lo = src[r1]
        r1 += 2
        dst += bytes([lo, 0])
        r4 = len(dst) - 3
        # CRUNCHSTART, a statement at a time, to the end of the line
        step = "start"
        c = 0
        while True:
            if step == "start":
                c = rd()
                while c == 0x20:
                    if not mask & 1:
                        dst.append(c)
                    c = rd()
                if c in (0x2A, TDATA):
                    step = "copyline"
                else:
                    step = "items"
            if step == "items":
                if c == 0x20:
                    if not mask & 2:
                        step = "keep"
                    else:
                        while c == 0x20:
                            c = rd()
                        r5 = dst[-1]
                        panic = False
                        if r5 == 0x22 and c == 0x22:
                            panic = True
                        elif r5 in (0x24, 0x25, TRND):
                            panic = c in (0x28, 0x21, 0x3F)
                        elif r5 in (TEOR, TAND):
                            panic = LV[c] == 0
                        elif c in (0x21, 0x3F) and r5 == 0x29:
                            panic = True
                        elif LV[r5] == 0 or r5 == 0x2E:
                            if LV[c] == 0 or c in (0x2E, 0x24):
                                panic = True
                            else:
                                panic = c in (0x28, 0x21, 0x3F)
                        if panic:
                            dst.append(0x20)
                        step = "notspace"
                else:
                    step = "notspace"
            if step == "notspace":
                if c == TREM:
                    if first or not mask & 4:
                        step = "copyline"
                    else:
                        while c != 13:
                            c = rd()
                        step = "endstmt"
                elif c == 0x22:
                    while True:
                        dst.append(c)
                        c = rd()
                        if c == 13 or c == 0x22:
                            break
                    step = "endstmt2" if c == 13 else "keep"
                elif c in (0x3A, TELSE, TTHEN, 13):
                    step = "endstmt"
                else:
                    step = "keep"
            if step == "keep":
                dst.append(c)
                c = rd()
                step = "items"
                continue
            if step == "copyline":
                while True:
                    dst.append(c)
                    c = rd()
                    if c == 13:
                        break
                step = "endstmt2"
            if step == "endstmt":
                if mask & 8 and dst[-1] == 0x3A:
                    del dst[-1]
                step = "endstmt2"
            if step == "endstmt2":
                dst.append(c)
                if c != 13:
                    step = "start"
                    continue
                r5 = len(dst) - r4
                dst[r4 + 2] = r5 & 0xFF
                first = False
                if mask & 16 and r5 == 4:
                    del dst[r4:]
                break


# LVTABLE: 0 for what may be in a name or a number (0-9, A-Z, _ to z)
LV = [0 if wordc(c) else 1 for c in range(256)]


def main():
    args = sys.argv[1:]
    append = args[:1] == ["-a"]
    if append:
        args = args[1:]
    if len(args) < 3:
        raise SystemExit(__doc__)
    mask = int(args[0], 0) & 0xFFFFFFFF
    ins, out = args[1:-1], args[-1]
    if len(ins) > 1 and not append:
        raise SystemExit("bascrunch: several inputs need -a")
    src = b"".join(open(p, "rb").read() for p in ins)
    prog = src if is_program(src) else program(textload(src))
    with open(out, "wb") as fh:
        fh.write(crunch(prog, mask))


if __name__ == "__main__":
    main()
