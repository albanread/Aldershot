#!/bin/sh
# analyze.sh REPORT_DIR PATH...  -- run the source analyzers over PATH
# (files or directories of C, relative to rosgd/), writing one report file
# per tool into REPORT_DIR:
#
#   clang-tidy.txt    bugs and security (.clang-tidy), with the clang
#                     static analyzer's own checkers
#   clang-analyze.txt the static analyzer with the extra checkers
#                     (security, unix, optin.portability, nullability)
#   cppcheck.txt      cppcheck, warning and portability
#   flawfinder.txt    flawfinder, level 2
#   semgrep.txt       semgrep's C rules (needs the network the first time)
#
# Run from rosgd/, after the generated headers are made (see mkcompdb.py).
# Each report is the tool's own output; the pass triages it by hand.
set -u
OUT=${1:?usage: analyze.sh REPORT_DIR PATH...}
shift
mkdir -p "$OUT"
HERE=$(cd "$(dirname "$0")" && pwd)
LLVM=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
DB=build/publish
mkdir -p $DB
[ -f $DB/compile_commands.json ] || python3 "$HERE/mkcompdb.py" $DB/compile_commands.json || exit 1
FILES=$(python3 - "$@" <<'PY'
import json, os, sys
db = json.load(open("build/publish/compile_commands.json"))
want = [p.rstrip("/") for p in sys.argv[1:]]
for e in db:
    f = e["file"]
    if any(f == w or f.startswith(w + "/") for w in want):
        print(f)
PY
)
[ -n "$FILES" ] || { echo "analyze.sh: no C files in $*" >&2; exit 1; }
n=$(echo "$FILES" | wc -l | tr -d ' ')
echo "analyze: $n files"

echo "== clang-tidy"
echo "$FILES" | xargs -P 6 -n 4 "$LLVM/clang-tidy" -p $DB --quiet 2>/dev/null > "$OUT/clang-tidy.txt"
echo "   $(grep -c -E ': (warning|error):' "$OUT/clang-tidy.txt") findings"

echo "== clang static analyzer"
: > "$OUT/clang-analyze.txt"
python3 - "$OUT/clang-analyze.txt" $FILES <<'PY'
import json, subprocess, sys, concurrent.futures as cf
out = sys.argv[1]
db = {e["file"]: e for e in json.load(open("build/publish/compile_commands.json"))}
# names valid in clang 23; a name the analyzer does not know is an error below
CHECK = ("core,deadcode,nullability,security,unix,optin.portability.UnixAPI,"
         "optin.core.EnumCastOutOfRange,optin.taint.TaintedAlloc,security.ArrayBound,"
         "alpha.unix.cstring.OutOfBounds")
def run(f):
    e = db[f]
    a = [x for x in e["arguments"] if x not in ("-c", f, "-MMD", "-MP")]
    a = a[:1] + ["--analyze", "-Xclang", "-analyzer-checker=" + CHECK,
                 "-Xclang", "-analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling",
                 "-Xclang", "-analyzer-output=text", "-w"] + a[1:] + [f]
    a = [x for x in a if not x.startswith("-Werror")]
    r = subprocess.run(a, capture_output=True, text=True, cwd=e["directory"])
    return r.stderr
bad = 0
with cf.ThreadPoolExecutor(6) as ex, open(out, "w") as fh:
    for text in ex.map(run, sys.argv[2:]):
        if "no analyzer checkers" in text:
            bad += 1
        fh.write(text)
if bad:
    sys.exit("analyze.sh: the analyzer did not know a checker name (%d files): fix CHECK" % bad)
PY
echo "   $(grep -c -E ': warning:' "$OUT/clang-analyze.txt") findings"

echo "== cppcheck"
for f in $FILES; do echo "$f"; done > "$OUT/.files"
cppcheck --project=$DB/compile_commands.json --enable=warning,portability --inconclusive \
    --suppress=missingIncludeSystem --suppress=unmatchedSuppression -q \
    --template='{file}:{line}: {severity}: {message} [{id}]' \
    $(sed 's/^/--file-filter=/' "$OUT/.files") 2> "$OUT/cppcheck.txt"
rm -f "$OUT/.files"
echo "   $(grep -c -E ': (warning|error|portability)' "$OUT/cppcheck.txt") findings"

echo "== flawfinder"
flawfinder --minlevel=2 --columns --quiet --dataonly $FILES > "$OUT/flawfinder.txt" 2>&1
echo "   $(grep -c -E ' \[[0-5]\] \(' "$OUT/flawfinder.txt") findings"

echo "== semgrep"
if command -v semgrep > /dev/null; then
    semgrep --config p/c --quiet --metrics=off --no-git-ignore $FILES > "$OUT/semgrep.txt" 2>&1 || true
    echo "   done ($(wc -l < "$OUT/semgrep.txt") lines)"
fi
