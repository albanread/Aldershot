"""archivers.py -- archive writers for SparkFS's codec tests, in Python.

The formats SparkFS reads but nothing on a Mac writes -- LHA/LZH, Zoo,
ARJ, StuffIt, PackIt, Compact Pro, ArcFS, PackDir -- made here from their
published descriptions, each with a real compression method as well as
stored where the format has one, so the codecs' decoders are exercised:

    lzh(entries, level, method)       -lh0-, -lh4-, -lh5- (levels 0, 1, 2)
    zoo(entries, method)              0 stored, 1 LZW, 2 lh5
    arj(entries, method)              0 stored, 1 (LZ77+Huffman), 4 (fastest)
    stuffit(entries, method)          0 stored, 1 RLE90, 2 LZW (compress)
    packit(entries, method)           0 PMag (stored), 1 PMa4 (Huffman)
    compactpro(entries)               its RLE layer (no LZH)
    arcfs(entries, method, bits)      &82 stored, &83 packed, &FF compressed
    packdir(entries, root, bits)      PackDir's LZW

entries: a list of (path, bytes[, extra]) -- path's parts separated by
"/"; a path ending "/" a directory; extra a Mac file's resource fork or a
RISC OS file type -- every file dated TIME (fixed, so the archives are
the same every run).  Each archive is checked by an
independent extractor where one exists (codectest.py: lhasa, unar, 7zz),
never by SparkFS itself.
"""

import struct
import time
import zlib

# 2 October 2026, 06:08:30 UTC: the archives' file time
TIME = 1790921310


# ---------------------------------------------------------------- CRCs

def _crc16_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        t.append(c)
    return t


_CRC16 = _crc16_table()


def crc16(data, crc=0):
    """CRC-16/ARC (reflected &A001): LHA's, Zoo's, ARC's, StuffIt's"""
    for b in data:
        crc = (crc >> 8) ^ _CRC16[(crc ^ b) & 0xFF]
    return crc


def crc16_ccitt(data, crc=0):
    """CRC-16/XMODEM (&1021, MSB first): PackIt's, BinHex's"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def dostime(t=TIME):
    """MS-DOS date and time, as a 32-bit word (date high), UTC"""
    tm = time.gmtime(t)
    d = ((tm.tm_year - 1980) << 9) | (tm.tm_mon << 5) | tm.tm_mday
    s = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec // 2)
    return (d << 16) | s


def mactime(t=TIME):
    """Seconds since 1 January 1904"""
    return t + 2082844800


# ---------------------------------------------------------------- bits

class BitsMSB:
    """Bits written most significant first (LHA, ARJ, Zoo's lh5, PackIt)"""
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, value, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc, self.n = 0, 0

    def bytes(self):
        if self.n:
            return bytes(self.out) + bytes([self.acc << (8 - self.n)])
        return bytes(self.out)


class BitsLSB:
    """Bits written least significant first (compress's LZW, Zoo's LZW)"""
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, value, bits):
        self.acc |= (value & ((1 << bits) - 1)) << self.n
        self.n += bits
        while self.n >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def bytes(self):
        return bytes(self.out) + (bytes([self.acc]) if self.n else b"")


# ---------------------------------------------------------------- LZ77 and Huffman

def lz77(data, window, maxmatch=256, minmatch=3, chain=64):
    """Tokens: ints (literals) and (length, distance) pairs, distance >= 1,
    greedy over hash chains"""
    out = []
    head = {}
    prev = [0] * len(data)
    i, n = 0, len(data)

    def insert(p):
        if p + 3 <= n:
            k = data[p:p + 3]
            prev[p] = head.get(k, -1)
            head[k] = p

    while i < n:
        best_len, best_dist = 0, 0
        if i + minmatch <= n:
            p = head.get(data[i:i + 3], -1)
            tries = chain
            while p >= 0 and i - p <= window and tries:
                l = 0
                lim = min(maxmatch, n - i)
                while l < lim and data[p + l] == data[i + l]:
                    l += 1
                if l > best_len:
                    best_len, best_dist = l, i - p
                    if l == lim:
                        break
                p = prev[p]
                tries -= 1
        if best_len >= minmatch:
            out.append((best_len, best_dist))
            for k in range(best_len):
                insert(i + k)
            i += best_len
        else:
            out.append(data[i])
            insert(i)
            i += 1
    return out


def huffman_lengths(freq, maxlen):
    """Code lengths for the frequencies (0 for unused symbols), none longer
    than maxlen (package-merge would be exact; this halves and retries)"""
    import heapq
    used = [i for i, f in enumerate(freq) if f]
    lens = [0] * len(freq)
    if not used:
        return lens
    if len(used) == 1:
        lens[used[0]] = 1
        return lens
    f = list(freq)
    while True:
        heap = [(f[i], i, (i,)) for i in used]
        heapq.heapify(heap)
        depth = {i: 0 for i in used}
        uid = len(freq)
        while len(heap) > 1:
            a = heapq.heappop(heap)
            b = heapq.heappop(heap)
            for s in a[2] + b[2]:
                depth[s] += 1
            heapq.heappush(heap, (a[0] + b[0], uid, a[2] + b[2]))
            uid += 1
        if max(depth.values()) <= maxlen:
            for s, d in depth.items():
                lens[s] = d
            return lens
        f = [(x + 1) // 2 if x else 0 for x in f]


def canonical_codes(lens):
    """LHA's make_code: codes by length, then symbol, shortest first"""
    maxlen = max(lens) if lens else 0
    count = [0] * (maxlen + 2)
    for l in lens:
        if l:
            count[l] += 1
    start = [0] * (maxlen + 2)
    for l in range(1, maxlen + 1):
        start[l + 1] = (start[l] + count[l]) << 1
    codes = [0] * len(lens)
    for s, l in enumerate(lens):
        if l:
            codes[s] = start[l]
            start[l] += 1
    return codes


# ---------------------------------------------------------------- LHA's static Huffman (lh4-7, ARJ 1-3, Zoo 2)

NC = 256 + 256 + 2 - 3    # 510: literals, then lengths 3..256
NT = 19


def lh_encode(data, dicbit, np, pbit, window=None, end=False):
    """data compressed as LHA's -lh4-/-lh5- (dicbit 12, 13; np 14, pbit 4),
    or ARJ's methods 1-3 (np 17, pbit 5, window 26624), in blocks"""
    window = window or (1 << dicbit)
    toks = lz77(data, window - 1)
    bw = BitsMSB()
    for b0 in range(0, len(toks), 0x4000):
        block = toks[b0:b0 + 0x4000]
        cf, pf = [0] * NC, [0] * np
        coded = []
        for t in block:
            if isinstance(t, int):
                cf[t] += 1
                coded.append((t, None))
            else:
                ln, dist = t
                c = ln - 3 + 256
                cf[c] += 1
                d = dist - 1
                pcode = d.bit_length()
                pf[pcode] += 1
                coded.append((c, (pcode, d)))
        clen = huffman_lengths(cf, 16)
        ccode = canonical_codes(clen)
        plen = huffman_lengths(pf, 16)
        pcode_ = canonical_codes(plen)
        bw.put(len(block), 16)
        _write_c_len(bw, clen)
        _write_pt_len(bw, plen, np, pbit, -1)
        for c, p in coded:
            if clen.count(0) == NC - 1:
                pass                    # one code: no bits
            else:
                bw.put(ccode[c], clen[c])
            if p is not None:
                pc, d = p
                if plen.count(0) != np - 1:
                    bw.put(pcode_[pc], plen[pc])
                if pc > 1:
                    bw.put(d & ((1 << (pc - 1)) - 1), pc - 1)
    if end:
        bw.put(0, 16)       # Zoo's: a block of no codes ends the stream
    return bw.bytes()


def _write_pt_len(bw, lens, n, nbit, special):
    used = [i for i, l in enumerate(lens) if l]
    if len(used) <= 1:
        bw.put(0, nbit)
        bw.put(used[0] if used else 0, nbit)
        return
    while n > 0 and lens[n - 1] == 0:
        n -= 1
    bw.put(n, nbit)
    i = 0
    while i < n:
        k = lens[i]
        if k <= 6:
            bw.put(k, 3)
        else:
            bw.put((1 << (k - 3)) - 2, k - 3)     # k-7 ones then a zero, after 111
        i += 1
        if i == special:
            z = 0
            while i < n and lens[i] == 0 and z < 3:
                i += 1
                z += 1
            bw.put(z, 2)


def _write_c_len(bw, clen):
    used = [i for i, l in enumerate(clen) if l]
    if len(used) <= 1:
        # one code: the t-table and c-table both say so
        bw.put(0, 5)
        bw.put(0, 5)
        bw.put(0, 9)
        bw.put(used[0] if used else 0, 9)
        return
    n = NC
    while n > 0 and clen[n - 1] == 0:
        n -= 1
    # the c lengths as t-symbols: 0 one zero, 1 3-18 zeros (4 bits), 2 20+
    # zeros (9 bits), else length + 2
    syms = []
    i = 0
    while i < n:
        k = clen[i]
        i += 1
        if k == 0:
            run = 1
            while i < n and clen[i] == 0:
                i += 1
                run += 1
            while run:
                if run <= 2:
                    syms.append((0, None))
                    run -= 1
                elif run <= 18:
                    syms.append((1, run - 3))
                    run = 0
                elif run == 19:
                    syms.append((0, None))
                    syms.append((1, 15))
                    run = 0
                else:
                    r = min(run, 20 + 511)
                    syms.append((2, r - 20))
                    run -= r
        else:
            syms.append((k + 2, None))
    tf = [0] * NT
    for s, _ in syms:
        tf[s] += 1
    tlen = huffman_lengths(tf, 16)
    tcode = canonical_codes(tlen)
    _write_pt_len(bw, tlen, NT, 5, 3)
    bw.put(n, 9)
    single = sum(1 for l in tlen if l) == 1
    for s, extra in syms:
        if not single:
            bw.put(tcode[s], tlen[s])
        if s == 1:
            bw.put(extra, 4)
        elif s == 2:
            bw.put(extra, 9)


# ---------------------------------------------------------------- LZH / LHA

def _lzh_entry(path, data, level, method):
    isdir = path.endswith("/")
    parts = path.rstrip("/").split("/")
    if isdir:
        method = "-lhd-"
        packed = b""
        data = b""
    elif method == "-lh0-":
        packed = data
    elif method == "-lh4-":
        packed = lh_encode(data, 12, 14, 4)
    elif method == "-lh5-":
        packed = lh_encode(data, 13, 14, 4)
    else:
        raise ValueError(method)
    if method != "-lh0-" and not isdir and len(packed) >= len(data):
        method, packed = "-lh0-", data
    crc = crc16(data)
    m = method.encode()
    if level == 0:
        name = "\\".join(parts).encode("latin-1") + (b"\\" if isdir else b"")
        body = m + struct.pack("<IIIBB", len(packed), len(data), dostime(), 0x20, 0) + \
            bytes([len(name)]) + name + struct.pack("<H", crc)
        return bytes([len(body), sum(body) & 0xFF]) + body + packed
    if level == 1:
        name = b"" if isdir else parts[-1].encode("latin-1")
        dirs = parts if isdir else parts[:-1]
        ext = b""
        if dirs:
            d = b"\xff".join(p.encode("latin-1") for p in dirs) + b"\xff"
            ext += struct.pack("<HB", 3 + len(d), 2) + d
        ext += struct.pack("<H", 0)        # the next-size of the last
        # level 1: the size field counts the extended headers too; the
        # first extended header's size is the base header's last word
        first = ext[:2]
        rest = ext[2:]
        body = m + struct.pack("<IIIBB", len(packed) + len(rest), len(data), dostime(), 0x20, 1) + \
            bytes([len(name)]) + name + struct.pack("<H", crc) + b"U" + first
        return bytes([len(body), sum(body) & 0xFF]) + body + rest + packed
    if level == 2:
        name = b"" if isdir else parts[-1].encode("latin-1")
        dirs = parts if isdir else parts[:-1]
        exts = [struct.pack("<B", 1) + name]
        if dirs:
            exts.append(struct.pack("<B", 2) + b"\xff".join(p.encode("latin-1") for p in dirs) + b"\xff")
        # each extended header: type, data, then the next one's size
        sizes = [len(e) + 2 for e in exts]
        extb = b""
        for i, e in enumerate(exts):
            nxt = sizes[i + 1] if i + 1 < len(exts) else 0
            extb += e + struct.pack("<H", nxt)
        base = m + struct.pack("<IIIBB", len(packed), len(data), TIME, 0x20, 2) + \
            struct.pack("<H", crc) + b"U" + struct.pack("<H", sizes[0])
        total = 2 + len(base) + len(extb)
        if total % 256 == 0:
            extb += b"\x00"          # LHA avoids a header size that ends 00
            total += 1
        return struct.pack("<H", total) + base + extb + packed
    raise ValueError(level)


def lzh(entries, level=1, method="-lh5-"):
    out = b"".join(_lzh_entry(p, d, level, method) for p, d in entries)
    return out + b"\x00"


# ---------------------------------------------------------------- LZW

def zoo_lzw(data):
    """Zoo's method 1 (lzc): CLEAR first, then codes LSB first from 9 bits
    up to 13, 256 CLEAR, 257 the end, 258 the first free code.  The width
    follows the decoder (lzd): each code after the first after a CLEAR adds
    an entry, and when the next free entry reaches 512, 1024 ... the width
    grows.  A full table (8192 entries) is cleared."""
    bw = BitsLSB()
    nbits, maxcode = 9, 512
    bw.put(256, nbits)
    table, enc_next, dfree, first = None, 258, 258, True

    def reset():
        nonlocal table, enc_next, dfree, first, nbits, maxcode
        table = {bytes([i]): i for i in range(256)}
        enc_next, dfree, first, nbits, maxcode = 258, 258, True, 9, 512

    def emit(code):
        nonlocal dfree, first, nbits, maxcode
        bw.put(code, nbits)
        if first:
            first = False
        else:
            dfree += 1
            if dfree >= maxcode and nbits < 13:
                nbits += 1
                maxcode <<= 1

    reset()
    w = b""
    for b in data:
        wc = w + bytes([b])
        if wc in table:
            w = wc
            continue
        emit(table[w])
        if enc_next < 8192:
            table[wc] = enc_next
            enc_next += 1
            w = bytes([b])
        else:
            # the table is full: the decoder has an entry for every code
            # it has read; CLEAR, then w's last byte starts afresh
            bw.put(256, nbits)
            reset()
            w = bytes([b])
    if w:
        emit(table[w])
    bw.put(257, nbits)
    return bw.bytes()


def compress_lzw(data, maxbits, header=False):
    """Unix compress (block mode): codes LSB first from 9 bits, 256 CLEAR,
    257 the first free code.  Written to compress's reader: before each
    code it widens when its next free code is past the width's largest
    (the last width's largest is 1 << maxbits), and then reads the codes
    from a fresh group of 8 -- what is left of the old group is padding.
    Each code after the first adds an entry, until the table is full; no
    CLEAR is written.  header: compress's own &1F &9D &80|maxbits first."""
    bw = BitsLSB()
    maxmax = 1 << maxbits
    nbits, maxcode = 9, 511
    dfree, first, count = 257, True, 0
    table = {bytes([i]): i for i in range(256)}
    enc_next = 257

    def emit(code):
        nonlocal nbits, maxcode, dfree, first, count
        if dfree > maxcode:
            while count % 8:
                bw.put(0, nbits)
                count += 1
            nbits += 1
            maxcode = maxmax if nbits == maxbits else (1 << nbits) - 1
            count = 0
        bw.put(code, nbits)
        count += 1
        if first:
            first = False
        elif dfree < maxmax:
            dfree += 1

    w = b""
    for b in data:
        wc = w + bytes([b])
        if wc in table:
            w = wc
            continue
        emit(table[w])
        if enc_next < maxmax:
            table[wc] = enc_next
            enc_next += 1
        w = bytes([b])
    if w:
        emit(table[w])
    out = bw.bytes()
    return (bytes([0x1F, 0x9D, 0x80 | maxbits]) + out) if header else out


# ---------------------------------------------------------------- RLE90

def rle90(data):
    """BinHex/StuffIt/ARC run-length: 0x90 n repeats the previous byte to
    n in all; 0x90 0 is a literal 0x90"""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        run = 1
        while i + run < n and data[i + run] == b and run < 255:
            run += 1
        if b == 0x90:
            out += b"\x90\x00"
            i += 1
            continue
        if run >= 3:
            out += bytes([b, 0x90, run])
            i += run
        else:
            out.append(b)
            i += 1
    return bytes(out)


# ---------------------------------------------------------------- Zoo

def zoo(entries, method=2):
    """A Zoo 2.10 archive: the header, then a directory entry (type 2, with
    its variable part: the long name and the directory) and the data for
    each file, then the null entry that ends the chain.  Directories are not
    entries in Zoo; each file carries its directory."""
    files = [(p, d) for p, d in entries if not p.endswith("/")]
    out = bytearray()
    text = b"ZOO 2.10 Archive.\x1a"
    start = 43                  # text, tag, start, minus, versions, type, comment, vdata
    out += text.ljust(20, b"\0") + struct.pack("<III", 0xFDC4A7DC, start, (-start) & 0xFFFFFFFF) + \
        bytes([2, 0]) + bytes([1]) + struct.pack("<IHH", 0, 0, 0)
    assert len(out) == start
    dt = dostime()
    for path, data in files:
        parts = path.split("/")
        if method == 0:
            packed, m = data, 0
        elif method == 1:
            packed, m = zoo_lzw(data), 1
        else:
            packed, m = lh_encode(data, 13, 14, 4, end=True), 2
        if m and len(packed) >= len(data):
            packed, m = data, 0
        name = parts[-1].encode("latin-1")
        dirn = ("/".join(parts[:-1])).encode("latin-1")
        short = name[:12]
        var = bytes([len(name) + 1, len(dirn) + 1 if dirn else 0]) + name + b"\0" + \
            (dirn + b"\0" if dirn else b"") + struct.pack("<H", 0) + bytes([0, 0, 0]) + struct.pack("<HH", 0, 0)
        here = len(out)
        hdrlen = 56 + len(var)
        nxt = here + hdrlen + len(packed)

        def fixed(dircrc):
            return struct.pack("<IBBIIHHHII", 0xFDC4A7DC, 2, m, nxt, here + hdrlen, dt >> 16, dt & 0xFFFF,
                               crc16(data), len(data), len(packed)) + \
                bytes([2 if m == 2 else 1, 1 if m == 2 else 0, 0, 0]) + struct.pack("<IH", 0, 0) + \
                short.ljust(13, b"\0") + struct.pack("<hBH", len(var), 127, dircrc)
        f = fixed(0)
        assert len(f) == 56
        out += fixed(crc16(f + var)) + var + packed
    # the null entry: its next is 0
    here = len(out)
    out += struct.pack("<IBBII", 0xFDC4A7DC, 2, 0, 0, 0) + bytes(56 - 14)
    return bytes(out)


# ---------------------------------------------------------------- ARJ

def arj_fastest(data):
    """ARJ's method 4: a 0 bit and 8 bits a literal; else the length less 2
    and the distance less 1, each in ARJ's unary-prefixed widths (lengths
    from width 0 up to 7, distances from 9 up to 13; MSB first)"""
    toks = lz77(data, 15872, maxmatch=256)
    bw = BitsMSB()

    def code(v, start, stop):
        plus, pwr, width = 0, 1 << start, start
        while width < stop and v >= plus + pwr:
            plus += pwr
            pwr <<= 1
            width += 1
            bw.put(1, 1)
        if width < stop:
            bw.put(0, 1)
        if width:
            bw.put(v - plus, width)

    for t in toks:
        if isinstance(t, int):
            bw.put(0, 1)
            bw.put(t, 8)
        else:
            ln, dist = t
            code(ln - 2, 0, 7)
            code(dist - 1, 9, 13)
    return bw.bytes()


def arj(entries, method=1):
    """An ARJ archive: the main header, a local header and data for each
    file (paths with /, as ARJ stores them), the end (a header of size 0).
    Method 1 is ARJ's LZ77 and Huffman (LHA's -lh6- coding, a 26624-byte
    window); 4, ARJ's fastest; 0, stored."""
    def header(first, name, comment=b""):
        basic = first + name + b"\0" + comment + b"\0"
        return b"\x60\xea" + struct.pack("<H", len(basic)) + basic + \
            struct.pack("<I", zlib.crc32(basic) & 0xFFFFFFFF) + struct.pack("<H", 0)

    dt = dostime()
    main = bytes([30, 11, 1, 0, 0x10, 0, 2, 0]) + struct.pack("<IIIIHHBB", dt, dt, 0, 0, 0, 0, 0, 0)
    assert len(main) == 30
    out = bytearray(header(main, b"TEST.ARJ"))
    for path, data in entries:
        if path.endswith("/"):
            continue
        if method == 0:
            packed, m = data, 0
        elif method == 4:
            packed, m = arj_fastest(data), 4
        else:
            packed, m = lh_encode(data, 15, 17, 5, window=26624), method
        if m and len(packed) >= len(data):
            packed, m = data, 0
        name = path.encode("latin-1")
        first = bytes([30, 11, 1, 0, 0x10, m, 0, 0]) + \
            struct.pack("<IIIIHHBB", dt, len(packed), len(data), zlib.crc32(data) & 0xFFFFFFFF,
                        path.rfind("/") + 1, 0x20, 0, 0)
        assert len(first) == 30
        out += header(first, name) + packed
    out += b"\x60\xea\x00\x00"
    return bytes(out)


# ---------------------------------------------------------------- Mac archives

def _macname(s):
    return s.encode("mac_roman")[:63]


def _mac_tree(entries):
    """entries as a nested list: ("dir", name, children) / ("file", name,
    data, rsrc), in the order given (a directory before what is in it)"""
    root = []
    dirs = {"": root}
    for e in entries:
        path, data = e[0], e[1]
        rsrc = e[2] if len(e) > 2 else b""
        parts = path.rstrip("/").split("/")
        parent = dirs["/".join(parts[:-1])] if len(parts) > 1 else root
        if path.endswith("/"):
            node = ("dir", parts[-1], [])
            dirs["/".join(parts)] = node[2]
        else:
            node = ("file", parts[-1], data, rsrc)
        parent.append(node)
    return root


def stuffit(entries, method=2):
    """A classic StuffIt (1.5) archive: SIT! header, then for each file its
    112-byte header, resource fork, data fork; folders as a start header
    (method 32) and an end header (33).  Methods per fork: 0 none, 1 RLE90,
    2 LZW (compress, 14 bits).  entries: (path, data[, resource fork])."""
    def pack(fork):
        if not fork or method == 0:
            return fork, 0
        p = rle90(fork) if method == 1 else compress_lzw(fork, 14)
        return (p, method) if len(p) < len(fork) else (fork, 0)

    body = bytearray()
    count = [0]

    def hdr(rm, dm, name, ftype, creator, rlen, dlen, crlen, cdlen, rcrc, dcrc):
        n = _macname(name)
        h = bytes([rm, dm, len(n)]) + n.ljust(63, b"\0") + ftype + creator + struct.pack(
            ">HIIIIIIHH", 0, mactime(), mactime(), rlen, dlen, crlen, cdlen, rcrc, dcrc) + bytes(6)
        assert len(h) == 110
        return h + struct.pack(">H", crc16(h))

    def walk(nodes):
        for node in nodes:
            if node[0] == "dir":
                body.extend(hdr(32, 32, node[1], b"\0" * 4, b"\0" * 4, 0, 0, 0, 0, 0, 0))
                walk(node[2])
                body.extend(hdr(33, 33, node[1], b"\0" * 4, b"\0" * 4, 0, 0, 0, 0, 0, 0))
            else:
                _, name, data, rsrc = node
                cr, rm = pack(rsrc)
                cd, dm = pack(data)
                body.extend(hdr(rm, dm, name, b"TEXT", b"ttxt", len(rsrc), len(data), len(cr), len(cd),
                                crc16(rsrc), crc16(data)))
                body.extend(cr + cd)
            count[0] += 1

    tree = _mac_tree(entries)
    walk(tree)
    head = b"SIT!" + struct.pack(">HI", len(tree), 22 + len(body)) + b"rLau" + bytes([1]) + bytes(7)
    return head + bytes(body)


def _huffman_tree_bits(data):
    """PackIt's Huffman: a tree (1 then 8 bits: a leaf; 0, then the zero
    and one subtrees: a node) and each byte's code, MSB first"""
    import heapq
    freq = {}
    for b in data:
        freq[b] = freq.get(b, 0) + 1
    if len(freq) == 1:
        freq[(next(iter(freq)) + 1) & 0xFF] = 0
    heap = [(f, i, b) for i, (b, f) in enumerate(sorted(freq.items()))]
    heapq.heapify(heap)
    uid = 1000
    while len(heap) > 1:
        a = heapq.heappop(heap)
        b = heapq.heappop(heap)
        heapq.heappush(heap, (a[0] + b[0], uid, (a[2], b[2])))
        uid += 1
    tree = heap[0][2]
    codes = {}
    bw = BitsMSB()

    def walk(t, prefix, n):
        if isinstance(t, int):
            bw.put(1, 1)
            bw.put(t, 8)
            codes[t] = (prefix, n)
        else:
            bw.put(0, 1)
            walk(t[0], prefix << 1, n + 1)
            walk(t[1], (prefix << 1) | 1, n + 1)
    walk(tree, 0, 0)
    for b in data:
        c, n = codes[b]
        bw.put(c, n)
    return bw.bytes()


def packit(entries, method=1):
    """A PackIt archive: for each file "PMag" (stored) or "PMa4" (Huffman),
    then its 94-byte header (name, type, creator, flags, lengths, dates,
    header CRC), data fork, resource fork and their CRC -- all of it after
    the magic Huffman coded for PMa4 -- and "PEnd".  No folders in PackIt."""
    out = bytearray()
    for e in entries:
        path, data = e[0], e[1]
        if path.endswith("/"):
            continue
        rsrc = e[2] if len(e) > 2 else b""
        n = _macname(path.split("/")[-1])
        h = bytes([len(n)]) + n.ljust(63, b"\0") + b"TEXT" + b"ttxt" + struct.pack(
            ">HHIIII", 0, 0, len(data), len(rsrc), mactime(), mactime())
        assert len(h) == 92
        h += struct.pack(">H", crc16_ccitt(h))
        rest = h + data + rsrc + struct.pack(">H", crc16_ccitt(data + rsrc))
        if method:
            out += b"PMa4" + _huffman_tree_bits(rest)
        else:
            out += b"PMag" + rest
    return bytes(out + b"PEnd")


def _cpt_rle(data):
    """Compact Pro's run-length layer: a byte, then 0x81 0x82 n repeats it
    to n in all; a lone 0x81 is itself"""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        run = 1
        while i + run < n and data[i + run] == b and run < 255:
            run += 1
        assert b not in (0x81, 0x82), "Compact Pro test data avoids the escapes"
        if run >= 4:
            out += bytes([b, 0x81, 0x82, run])
            i += run
        else:
            out.append(b)
            i += 1
    return bytes(out)


def _cpt_crc(b):
    """Compact Pro's CRC-32: zlib's, without its final inversion"""
    return (zlib.crc32(b) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def compactpro(entries):
    """A Compact Pro archive (RLE, no LZH): the 8-byte header (1, volume 1,
    &0001, the directory's offset), each file's resource fork then data
    fork, each through the RLE layer, then the directory: CRC, entries,
    comment, and each entry -- a name (bit 7 of its length: a folder, with
    the count of entries in it) or a file's 45 bytes"""
    data_area = bytearray()
    dirb = bytearray()
    tree = _mac_tree(entries)

    def count(nodes):
        return sum(1 + (count(nd[2]) if nd[0] == "dir" else 0) for nd in nodes)

    def walk(nodes):
        for nd in nodes:
            name = _macname(nd[1])
            if nd[0] == "dir":
                dirb.extend(bytes([len(name) | 0x80]) + name + struct.pack(">H", count(nd[2])))
                walk(nd[2])
            else:
                _, _, data, rsrc = nd
                pos = 8 + len(data_area)
                cr, cd = _cpt_rle(rsrc), _cpt_rle(data)
                data_area.extend(cr + cd)
                crc = _cpt_crc(rsrc + data)
                dirb.extend(bytes([len(name)]) + name + bytes([1]) + struct.pack(
                    ">I4s4sIIHIHIIII", pos, b"TEXT", b"ttxt", mactime(), mactime(), 0, crc, 0,
                    len(rsrc), len(data), len(cr), len(cd)))

    walk(tree)
    second = struct.pack(">HB", count(tree), 0) + bytes(dirb)
    hdr2 = struct.pack(">I", _cpt_crc(second)) + second
    off = 8 + len(data_area)
    return bytes([1, 1]) + struct.pack(">HI", 1, off) + bytes(data_area) + hdr2


# ---------------------------------------------------------------- ArcFS

def riscos_time(t=TIME):
    """Centiseconds since 1900, the 5 bytes RISC OS keeps in load and exec"""
    return (t + 2208988800) * 100


def loadexec(ftype, t=TIME):
    cs = riscos_time(t)
    return 0xFFF00000 | (ftype << 8) | (cs >> 32), cs & 0xFFFFFFFF


def arcfs(entries, method=0xFF, bits=12):
    """An ArcFS archive (Mark Smith's, RISC OS): "Archive\\0", the length of
    the headers and the data's offset, the versions and format, then a
    36-byte header for each object -- method (0 ends a directory), name (11
    bytes), length, load, exec, attributes (bits 8-15 the LZW width, 16-31
    the CRC), stored length, and the data's offset (bit 31: a directory,
    whose objects follow it) -- then the data.  Methods: &82 stored, &83
    packed (ARC's run-length), &FF compressed (LZW of the given width).
    entries: (path, data[, filetype])"""
    heads = bytearray()
    data_area = bytearray()
    tree = _mac_tree([(e[0], e[1]) for e in entries])
    types = {e[0].split("/")[-1]: (e[2] if len(e) > 2 else 0xFFD) for e in entries}

    def walk(nodes):
        for nd in nodes:
            name = nd[1].encode("latin-1")[:11].ljust(11, b"\0")
            if nd[0] == "dir":
                load, exe = loadexec(0x1000 >> 0 & 0xFFF)
                load = 0xFFFFFD00 | (riscos_time() >> 32)
                heads.extend(bytes([0x82]) + name + struct.pack("<IIIIII", 1, load, exe, 0x3, 0,
                                                                0x80000000 | len(data_area)))
                walk(nd[2])
                heads.extend(bytes(36))
            else:
                _, _, d, _ = nd
                if method == 0x82 or not d:
                    p, m = d, 0x82
                elif method == 0x83:
                    p, m = rle90(d), 0x83
                else:
                    p, m = compress_lzw(d, bits), 0xFF
                if m != 0x82 and len(p) >= len(d):
                    p, m = d, 0x82
                load, exe = loadexec(types[nd[1]])
                acc = 0x33 | ((bits if m == 0xFF else 0) << 8) | (crc16(d) << 16)
                heads.extend(bytes([m]) + name + struct.pack("<IIIIII", len(d), load, exe, acc, len(p),
                                                             len(data_area)))
                data_area.extend(p)

    walk(tree)
    heads.extend(bytes(36))                  # the end of the top level
    hdr = b"Archive\0" + struct.pack("<IIIII", len(heads), 96 + len(heads), 0x104, 0x104, 0) + bytes(68)
    assert len(hdr) == 96
    return hdr + bytes(heads) + bytes(data_area)


# ---------------------------------------------------------------- PackDir

def packdir_lzw(data, maxbits):
    """PackDir's LZW: codes LSB first from 9 bits, 256 CLEAR, 257 the end,
    258 the first free code; no CLEAR before the first code.  Written to
    PackDir's reader: each code after the first adds an entry, and when the
    entry added is the width's top code the next code is a bit wider (no
    padding); the table stops growing at (1 << maxbits) - 1."""
    bw = BitsLSB()
    maxcode = (1 << maxbits) - 1
    nbits, highcode = 9, 511
    dfree, first, full = 258, True, False

    def emit(code):
        nonlocal nbits, highcode, dfree, first, full
        bw.put(code, nbits)
        if first:
            first = False
        elif not full:
            idx = dfree
            dfree += 1
            if idx == highcode:
                if highcode >= maxcode:
                    full = True
                else:
                    nbits += 1
                    highcode = (1 << nbits) - 1

    table = {bytes([i]): i for i in range(256)}
    enc_next = 258
    w = b""
    for b in data:
        wc = w + bytes([b])
        if wc in table:
            w = wc
            continue
        emit(table[w])
        if enc_next <= maxcode:
            table[wc] = enc_next
            enc_next += 1
        w = bytes([b])
    if w:
        emit(table[w])
    bw.put(257, nbits)
    return bw.bytes()


def packdir(entries, root="Packed", bits=13):
    """A PackDir archive (John Kortink's): "PACK\\0", the LZW width less 12,
    the packed directory's name, its load, exec, object count and
    attributes, then each object in order -- name, load, exec, length (a
    directory: its object count), attributes, 1 for a directory, else the
    stored length (-1 stored, -2 empty) and the data -- and an empty name.
    entries: (path, data[, filetype])"""
    tree = _mac_tree([(e[0], e[1]) for e in entries])
    types = {e[0].split("/")[-1]: (e[2] if len(e) > 2 else 0xFFD) for e in entries}
    dload = 0xFFFFFD00 | (riscos_time() >> 32)
    dexec = riscos_time() & 0xFFFFFFFF
    out = bytearray(b"PACK\0" + struct.pack("<i", bits - 12) + root.encode("latin-1") + b"\0" +
                    struct.pack("<IIII", dload, dexec, len(tree), 0x13))

    def walk(nodes):
        for nd in nodes:
            name = nd[1].encode("latin-1") + b"\0"
            if nd[0] == "dir":
                out.extend(name + struct.pack("<IIIII", dload, dexec, len(nd[2]), 0x13, 1))
                walk(nd[2])
            else:
                d = nd[2]
                load, exe = loadexec(types[nd[1]])
                out.extend(name + struct.pack("<IIIII", load, exe, len(d), 0x33, 0))
                if not d:
                    out.extend(struct.pack("<i", -2))
                    continue
                p = packdir_lzw(d, bits)
                if len(p) >= len(d):
                    out.extend(struct.pack("<i", -1) + d)
                else:
                    out.extend(struct.pack("<i", len(p)) + p)
    walk(tree)
    out.extend(b"\0")
    return bytes(out)
