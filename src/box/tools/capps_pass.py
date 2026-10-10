"""capps_pass.py -- the source pass over staged RISC OS C (package T4b).

Norcroft's dialect differs from clang's in three ways that no flag reaches,
so tools/capps.py rewrites the staged text.  The pass reads clang's own AST
(-ast-dump=json) of every unit, with exactly the unit's flags, and edits the
tokens the AST points at:

  B  a plain bit-field is unsigned.  Norcroft reads `int f:3` (and a field
     whose type is a typedef of plain int, short or long) as unsigned; clang
     reads it as signed.  `int` gets `unsigned ` before it; a typedef name
     (BOOL) becomes `unsigned int`.  Explicitly signed fields, and typedefs
     that say `signed`, are kept.  char is already unsigned (-funsigned-char);
     enum and _Bool fields are left alone.  A plain member declared with
     the bit-field (`int x, y:3;`) would become unsigned with it, which
     Norcroft does not do: the pass cannot edit that, and stops.
  A  every structure and union is word-aligned.  Norcroft pads {char} to 4
     bytes and puts a {char} member at a word boundary; the flags' pack(4)
     only caps alignment.  Each complete definition that is not packed gets
     ` __attribute__((aligned(4)))` after its `struct` or `union`.
  T  the library build only: every writable static is thread-local.  The
     ROM SharedCLibrary has no writable data of its own; each client's copy
     of its statics is reached through one base (%gs, design section 2.2).
     Each declaration with static storage in a library unit (file scope, or
     `static` or `extern` in a block) whose object is writable, or which is
     one of the contract's exported statics (__iob, __ctype, __huge_val ...),
     gets `_Thread_local __attribute__((aligned(4))) ` in front -- an
     `extern` one only if a library unit defines it or it is exported: a
     variable the client program defines (kernel.h's __root_stack_size) is
     the client's own, not a library static.  The
     alignment is the one Norcroft's static data has, and it stops clang
     assuming that a 16-byte or larger array is 16-aligned (movaps): roscc
     lays the block out at RISC OS's 4-byte offsets.  An edit in a header
     under inc/ goes to a copy in inc-lib/, which only library units see;
     clients keep RISC OS's headers without thread-local anywhere.

The pass runs on the pristine staged tree (OUT/pre) and its output is
written to OUT/src, OUT/inc and OUT/inc-lib.  It is idempotent: run over its
own output it finds nothing to do, which `capps.py check` proves every
time the tree changes (audit()).  Offsets are bytes; files are latin-1.
"""
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys

ALIGN_ATTR = " __attribute__((aligned(4)))"
TLS_TEXT = "_Thread_local __attribute__((aligned(4))) "
INT_WORDS = ("int", "short", "long")
SIGN_WORDS = {"signed", "unsigned", "__signed", "__signed__"}
QUALIFIERS = {"const", "volatile", "__volatile__", "__const", "restrict", "__restrict"}

_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_TOKEN = re.compile(r"[A-Za-z_]\w*|\S")


def tokens(text):
    """[(offset, token)] of a region, comments blanked (offsets kept)."""
    clean = _COMMENT.sub(lambda m: " " * len(m.group(0)), text)
    return [(m.start(), m.group(0)) for m in _TOKEN.finditer(clean)]


def base_int_type(qual):
    """'int', 'short', 'long' or 'long long' when a type string names one
    (after typedefs, qualifiers dropped), else None."""
    words = [w for w in qual.split() if w not in QUALIFIERS]
    t = " ".join(words)
    t = {"short int": "short", "long int": "long", "long long int": "long long"}.get(t, t)
    return t if t in ("int", "short", "long", "long long") else None


def writable(qual):
    """Is an object of this type (clang's spelling) writable?  An array is
    what its elements are; otherwise the top-level declarator's qualifier."""
    s = qual.strip()
    while s.endswith("]"):
        depth, i = 0, len(s) - 1
        while i >= 0:
            depth += {"]": 1, "[": -1}.get(s[i], 0)
            if depth == 0:
                break
            i -= 1
        s = s[:i].rstrip()
    if s.endswith(")"):
        # a pointer to a function or an array: T (*q)(...) or T (*q)[n]
        m = re.search(r"\(\s*\*+([^()]*)\)", s)
        return not (m and re.search(r"\bconst\b", m.group(1)))
    if "*" in s:
        return not re.search(r"\bconst\s*$", s)
    return "const" not in s.split()


# ---------------------------------------------------------------------------
# one unit: clang's AST, then the edits it asks for

class Loc:
    __slots__ = ("file", "off", "tok", "macro")

    def __init__(self, file, off, tok, macro):
        self.file, self.off, self.tok, self.macro = file, off, tok, macro


def _resolve(tree):
    """Give every bare location in the dump its file.  clang writes "file"
    only when it changes from the last location written, so the walk follows
    the document order; includedFrom does not count."""
    cur = [None]
    seen = set()
    stack = [tree]
    while stack:
        n = stack.pop()
        if isinstance(n, dict):
            if "offset" in n and "tokLen" in n:
                if "file" in n:
                    cur[0] = n["file"]
                n["_f"] = cur[0]
                seen.add(cur[0])
                continue
            vals = [v for k, v in n.items() if k != "includedFrom" and isinstance(v, (dict, list))]
            stack.extend(reversed(vals))
        elif isinstance(n, list):
            stack.extend(reversed(n))
    return seen


def _loc(d, prefer="spelling"):
    """A resolved location dict -> Loc (the spelling, or the expansion)."""
    if d is None:
        return None
    if "spellingLoc" in d:
        s, e = d["spellingLoc"], d["expansionLoc"]
        b = s if prefer == "spelling" else e
        if "_f" not in b:
            return None
        return Loc(b["_f"], b["offset"], b["tokLen"], True)
    if "_f" not in d:
        return None
    return Loc(d["_f"], d["offset"], d["tokLen"], False)


class Unit:
    """What run_unit needs: the unit's name, library or not, and its clang
    command (paths already mapped to the tree being read)."""

    def __init__(self, name, lib, argv):
        self.name, self.lib, self.argv = name, lib, argv


def analyse(unit, root, exported):
    """clang's AST of one unit -> (edits, notes, files seen).

    edits: [(file, offset, length, text, kind)] with file relative to root;
    notes: [(kind, file, offset, message)] for what the pass cannot edit or
    deliberately leaves (counted in the report; 'unhandled' fails)."""
    r = subprocess.run(unit.argv, capture_output=True)
    if r.returncode:
        raise RuntimeError("clang failed on %s:\n%s" % (unit.name, r.stderr.decode("latin-1")[-2000:]))
    tree = json.loads(r.stdout)
    del r
    files = _resolve(tree)
    root = os.path.abspath(root) + os.sep
    texts = {}

    def text(f):
        if f not in texts:
            with open(f, "rb") as fh:
                texts[f] = fh.read().decode("latin-1")
        return texts[f]

    def ours(f):
        return f is not None and os.path.abspath(f).startswith(root)

    def rel(f):
        return os.path.relpath(os.path.abspath(f), root)

    typedefs = {}
    edits, notes = [], []
    defs = set()                     # variables this unit defines, external linkage
    # nodes: (node, file scope?)
    todo = [(tree, True)]
    fields = []                      # bit-fields, grouped after the walk
    while todo:
        n, top = todo.pop()
        k = n.get("kind")
        if k == "TypedefDecl":
            typedefs[n["id"]] = n
        elif k == "RecordDecl" and n.get("completeDefinition") and not n.get("isImplicit"):
            attrs = {c.get("kind") for c in n.get("inner", [])}
            b = _loc(n.get("range", {}).get("begin"))
            if b and ours(b.file) and "PackedAttr" not in attrs and "AlignedAttr" not in attrs:
                kw = n.get("tagUsed")
                t = text(b.file)
                if kw in ("struct", "union") and t[b.off:b.off + len(kw)] == kw:
                    edits.append((rel(b.file), b.off + len(kw), 0, ALIGN_ATTR, "A"))
                    if b.macro:
                        notes.append(("macro", rel(b.file), b.off, "struct/union keyword spelled in a macro"))
                else:
                    notes.append(("unhandled", rel(b.file), b.off, "record: no %s keyword at its start" % kw))
        elif k == "FieldDecl":
            fields.append(n)            # every member: a plain one may share a bit-field's declaration
        elif k == "VarDecl" and unit.lib and not n.get("tls") and not n.get("isImplicit"):
            if top or n.get("storageClass") in ("static", "extern"):
                ty = n.get("type", {})
                q = ty.get("desugaredQualType", ty.get("qualType", ""))
                name = n.get("name")
                # a definition: not extern, or extern with an initialiser;
                # one with external linkage may be another unit's extern
                is_def = n.get("storageClass") != "extern" or "init" in n
                if is_def and n.get("storageClass") != "static":
                    defs.add(name)
                if writable(q) or name in exported:
                    # in front of the declaration as written: before a macro
                    # that expands to it (EXTERN int x;), never inside one
                    b = _loc(n.get("range", {}).get("begin"), prefer="expansion")
                    if b and ours(b.file):
                        # an extern declaration waits for run(): only a
                        # library definition, or an export, makes it one
                        edits.append((rel(b.file), b.off, 0, TLS_TEXT, "T" if is_def else "T?" + name))
                    else:
                        notes.append(("unhandled", b.file if b else "?", b.off if b else 0,
                                      "library static %s declared outside the staged tree" % n.get("name")))
        inner = n.get("inner")
        if inner:
            child_top = top and k in ("TranslationUnitDecl", "LinkageSpecDecl")
            for c in reversed(inner):
                if isinstance(c, dict):
                    todo.append((c, child_top))

    # B: bit-fields, one edit per declaration (int a:1, :2, b:3 share one)
    groups = collections.OrderedDict()
    for n in fields:
        b = _loc(n.get("range", {}).get("begin"))
        if b is None or not ours(b.file):
            continue
        groups.setdefault((b.file, b.off), []).append(n)
    for (f, off), members in groups.items():
        ns = [n for n in members if n.get("isBitfield")]
        if not ns:
            continue
        plain = [n.get("name") or "?" for n in members if not n.get("isBitfield")]
        n0 = ns[0]
        ty = n0.get("type", {})
        q = ty.get("desugaredQualType", ty.get("qualType", ""))
        if q.split()[0:1] == ["enum"] or "enum " in q:
            notes.append(("enum", rel(f), off, "enum bit-field left as clang reads it"))
            continue
        base = base_int_type(q)
        if base is None:
            continue                  # unsigned, char (already unsigned), _Bool
        t = text(f)
        b0 = _loc(n0["range"]["begin"])
        if b0.macro:
            notes.append(("unhandled", rel(f), off, "bit-field declared in a macro"))
            continue
        # the specifiers: up to the first declarator's name, or its ':'
        end = t.find(":", off)
        for n in members:
            nl = _loc(n.get("loc"))
            if n.get("name") and nl and nl.file == f and off < nl.off < end:
                end = nl.off
        spec = tokens(t[off:end])
        words = [w for _, w in spec]
        if SIGN_WORDS & set(words):
            continue                  # said signed (or unsigned) in so many words
        # `int x, y:3;` -- one type for both: made unsigned, x would be too,
        # and Norcroft keeps x signed
        shared = ("unhandled", rel(f), off, "a plain member (%s) shares a bit-field's declaration: split it"
                  % ", ".join(plain))
        kwpos = [o for o, w in spec if w in INT_WORDS]
        if kwpos:
            if plain:
                notes.append(shared)
            else:
                edits.append((rel(f), off + kwpos[0], 0, "unsigned ", "B"))
            continue
        # a typedef name: follow the chain; any typedef that says signed keeps it
        tid, explicit, ok = ty.get("typeAliasDeclId"), False, True
        while tid and tid in typedefs:
            td = typedefs[tid]
            tb, tl = _loc(td["range"]["begin"]), _loc(td.get("loc"))
            if tb is None or tl is None or tb.file != tl.file or tb.macro:
                ok = False
                break
            if SIGN_WORDS & {w for _, w in tokens(text(tb.file)[tb.off:tl.off])}:
                explicit = True
                break
            tid = td.get("type", {}).get("typeAliasDeclId")
        if not ok:
            notes.append(("unhandled", rel(f), off, "bit-field typedef declared in a macro"))
            continue
        if explicit:
            continue
        name = [w for w in ty.get("qualType", "").split() if w not in QUALIFIERS]
        hits = [o for o, w in spec if name and w == name[-1]]
        if len(hits) != 1:
            notes.append(("unhandled", rel(f), off, "bit-field type %r not found in %r" % (ty.get("qualType"), words)))
            continue
        if plain:
            notes.append(shared)
            continue
        edits.append((rel(f), off + hits[0], len(name[-1]), "unsigned " + base, "B"))
    return edits, notes, {rel(f) for f in files if ours(f)}, defs


def _run_one(args):
    unit, root, exported = args
    return unit.name, unit.lib, analyse(unit, root, exported)


def run(units, root, exported, jobs):
    """Every unit's edits, merged.  -> (general, library, notes, stats)

    general: {relpath: {(offset, length, text)}} -- B and A, for every build;
    library: {relpath: {(offset, length, text)}} -- T, library builds only."""
    general, library = collections.defaultdict(set), collections.defaultdict(set)
    notes = set()
    seen_nonlib = set()
    kinds = collections.Counter()
    defined = set()                  # what the library's units define
    externs = []                     # (file, edit, name): extern declarations
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as ex:
        for name, lib, (edits, ns, files, defs) in ex.map(_run_one, [(u, root, exported) for u in units]):
            for f, off, ln, txt, kind in edits:
                if kind.startswith("T?"):
                    externs.append((f, (off, ln, txt), kind[2:]))
                else:
                    (library if kind == "T" else general)[f].add((off, ln, txt))
            for x in ns:
                notes.add(x)
            if lib:
                defined |= defs
            else:
                seen_nonlib |= files
    # an extern declaration is a library static's only if the library
    # defines it or the contract exports it (in the client's block)
    for f, e, var in externs:
        if var in defined or var in exported:
            library[f].add(e)
    for d in (general, library):
        for f, es in d.items():
            for off, ln, txt in es:
                kinds["T" if d is library else ("A" if txt == ALIGN_ATTR else "B")] += 1
    # a library edit in a source file a client unit also reads would leak
    # thread-local into the client: headers go to inc-lib/, sources must be the library's
    leaks = sorted(f for f in library if not f.startswith("inc" + os.sep) and f in seen_nonlib)
    for f in leaks:
        notes.add(("unhandled", f, 0, "library static in a file client units read too"))
    stats = {"bitfields": kinds["B"], "records": kinds["A"], "statics": kinds["T"],
             "files": len(set(general) | set(library)),
             "notes": collections.Counter(k for k, _, _, _ in notes)}
    return ({f: sorted(e) for f, e in general.items()}, {f: sorted(e) for f, e in library.items()},
            sorted(notes), stats)


def apply(text, edits):
    """Apply (offset, length, text) edits to a string; overlapping edits stop."""
    out, last = [], len(text)
    for off, ln, txt in sorted(edits, key=lambda e: (e[0], e[1], e[2]), reverse=True):
        if off + ln > last:
            raise SystemExit("capps pass: overlapping edits at offset %d" % off)
        out.append(text[off + ln:last])
        out.append(txt)
        last = off
    out.append(text[:last])
    return "".join(reversed(out))


def unit_argv(cc, cflags, flags, src, mapping):
    """clang's command for the AST of one unit, paths moved by mapping
    [(from prefix, to prefix)] (the final tree -> the tree being read)."""
    def m(p):
        for a, b in mapping:
            if p == a or p.startswith(a + os.sep):
                return b + p[len(a):]
        return p
    fl = [("-I" + m(f[2:])) if f.startswith("-I") else f for f in flags]
    return cc + cflags + fl + ["-w", "-fsyntax-only", "-Xclang", "-ast-dump=json", m(src)]


if __name__ == "__main__":
    print(__doc__, file=sys.stderr)
    sys.exit(2)
