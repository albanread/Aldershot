#!/usr/bin/env python3
"""Run the differential harness across a farm of emulator instances.

One emulator assembles a unit in a few seconds, so the corpus takes half an
hour serially. The work is embarrassingly parallel — each unit is independent —
so run N instances at once.

    farm.py setup [--workers N]
    farm.py run <RiscOS dir> [--workers N] [--max-gets K] [--out report.txt]

Each worker gets its own emulator directory under `rpcemu/farm/<i>`: RPCEmu
writes `cmos.ram` on exit, and instances sharing a directory would race. The
85 MB `hostfs` is *not* copied — it is shared, which is safe because staging is
per-process (`xc<pid>`) and the DDE tools are only read.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402

BASE = paths.RPCEMU
FARM = paths.FARM
HOSTFS = paths.HOSTFS
HERE = os.path.dirname(os.path.abspath(__file__))

# Everything an instance needs, minus hostfs and the large saved states.
SKIP_DIRS = {"hostfs"}
SKIP_EXT = {".snap", ".state", ".png", ".hdf"}


def worker_dir(i):
    return os.path.join(FARM, str(i))


def safe_rmtree(path, root):
    """Delete a tree without ever following a link out of it.

    `shutil.rmtree` walks into a junction and deletes what is on the far side.
    The emulator directory holds one, and a farm teardown once followed it and
    took the boot disc and the DDE with it. A junction is removed here as the
    link it is, with `os.rmdir`, which leaves its target alone.

    `root` is the only place deletion is allowed to happen, checked after
    resolving both, so a path that escapes by any means is refused rather than
    obeyed.
    """
    path, root = os.path.abspath(path), os.path.abspath(root)
    if os.path.commonpath([path, root]) != root or path == root:
        raise ValueError(f"refusing to delete {path}, which is not inside {root}")
    if not os.path.isdir(path):
        return
    for entry in os.scandir(path):
        # A junction or symlink is removed, never entered.
        if entry.is_junction() or entry.is_symlink():
            (os.rmdir if entry.is_dir(follow_symlinks=False) else os.unlink)(entry.path)
        elif entry.is_dir(follow_symlinks=False):
            safe_rmtree(entry.path, root)
        else:
            os.chmod(entry.path, 0o600)
            os.unlink(entry.path)
    os.rmdir(path)


def setup(workers):
    os.makedirs(FARM, exist_ok=True)
    for i in range(workers):
        d = worker_dir(i)
        if os.path.isdir(d):
            safe_rmtree(d, FARM)
        os.makedirs(d)
        for name in os.listdir(BASE):
            src = os.path.join(BASE, name)
            if name in SKIP_DIRS:
                continue
            # Copying a junction would put one in the worker directory, and
            # the next teardown would be deleting through it again.
            if os.path.isjunction(src) or os.path.islink(src):
                continue
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(d, name), symlinks=True)
            else:
                if os.path.splitext(name)[1].lower() in SKIP_EXT:
                    continue
                shutil.copy2(src, os.path.join(d, name))
        # hostfs is shared: a directory junction costs nothing and keeps the
        # DDE and the staging areas in one place.
        link = os.path.join(d, "hostfs")
        rc = subprocess.run(
            ["cmd", "/c", "mklink", "/J", link, HOSTFS],
            capture_output=True, text=True,
        )
        if rc.returncode != 0:
            # Fall back to a copy if junctions are unavailable.
            shutil.copytree(HOSTFS, link)
        print(f"  worker {i}: {d}")
    print(f"{workers} instances ready")


def run(args):
    # One export root, built before any worker starts. A worker rebuilding it
    # would rmtree the directory its peers are reading from -- a race the
    # self-contained subset cannot expose, because those units need no headers.
    sys.path.insert(0, HERE)
    import corpus_diff
    corpus_diff.build_export_root(args.root)

    # A directory per run. Overwriting one set of worker reports makes a bad
    # run impossible to post-mortem, which is exactly when you want it.
    stamp = time.strftime("%Y%m%d-%H%M%S")
    rundir = os.path.join(HERE, "runs", stamp)
    os.makedirs(rundir, exist_ok=True)
    print(f"run directory: {rundir}", file=sys.stderr)

    procs = []
    for i in range(args.workers):
        cmd = [
            sys.executable, os.path.join(HERE, "corpus_diff.py"), args.root,
            "--emu-dir", worker_dir(i),
            "--shard", f"{i}/{args.workers}",
            "--shared-export",
            "--out", os.path.join(rundir, f"worker-{i}.txt"),
            "--jsonl", os.path.join(rundir, f"worker-{i}.jsonl"),
        ]
        if args.max_gets is not None:
            cmd += ["--max-gets", str(args.max_gets)]
        env = dict(os.environ, ROSASM_HOSTFS=os.path.join(worker_dir(i), "hostfs"))
        # Keep each worker's output: a crashed worker is otherwise invisible.
        log = open(os.path.join(rundir, f"worker-{i}.log"), "w", encoding="utf-8")
        procs.append((subprocess.Popen(cmd, env=env, stdout=log, stderr=log), log))

    t0 = time.time()
    failed = []
    for i, (p, log) in enumerate(procs):
        rc = p.wait()
        log.close()
        if rc != 0:
            failed.append(i)
        print(f"  worker {i} done rc={rc} ({time.time()-t0:.0f}s)", file=sys.stderr)
    if failed:
        print(f"  WORKERS THAT FAILED: {failed} -- see {rundir}", file=sys.stderr)

    report(rundir, args.workers, failed)
    if args.out:
        shutil.copy2(os.path.join(rundir, "report.txt"), args.out)


def report(rundir, workers, failed):
    """Aggregate the per-unit JSON records."""
    recs = []
    seen_workers = 0
    for i in range(workers):
        p = os.path.join(rundir, f"worker-{i}.jsonl")
        if not os.path.isfile(p):
            continue
        seen_workers += 1
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if line:
                recs.append(json.loads(line))

    both_ok = [r for r in recs if r["objasm_ok"] and r["ours_ok"]]
    both_bad = [r for r in recs if not r["objasm_ok"] and not r["ours_ok"]]
    only_obj = [r for r in recs if r["objasm_ok"] and not r["ours_ok"]]
    only_our = [r for r in recs if not r["objasm_ok"] and r["ours_ok"]]

    tot = {k: sum(r.get(k, 0) for r in both_ok)
           for k in ("rows", "text", "addr", "bytes", "absent", "missing", "extra")}
    pct = lambda x, n: 0.0 if not n else 100.0 * x / n

    out = []
    out.append(f"workers reporting    {seen_workers}/{workers}"
               + (f"   FAILED: {failed}" if failed else ""))
    out.append(f"units attempted      {len(recs)}")
    out.append(f"  both assembled     {len(both_ok)}")
    out.append(f"  both failed        {len(both_bad)}")
    out.append(f"  only ObjAsm        {len(only_obj)}   <- our gaps")
    out.append(f"  only ours          {len(only_our)}   <- suspect the harness")
    out.append("")
    out.append(f"rows compared        {tot['rows']}")
    for label, key in (("text matches", "text"), ("addr matches", "addr"),
                       ("bytes match", "bytes")):
        out.append(f"  {label:<17}{tot[key]:>6}  {pct(tot[key], tot['rows']):5.1f}%")
    out.append(f"  bytes awaited    {tot['absent']:>6}  {pct(tot['absent'], tot['rows']):5.1f}%"
               "   (encoder)")
    out.append(f"  rows only ObjAsm {tot['missing']:>6}")
    out.append(f"  rows only ours   {tot['extra']:>6}")

    # Why units failed, so a run says what to fix rather than only how much.
    for title, group, key in (("our failures", only_obj, "ours_err"),
                              ("both failed", both_bad, "objasm_err")):
        if not group:
            continue
        tally = collections.Counter(
            re.sub(r"'[^']*'", "'X'", re.sub(r"^[^:]*:[0-9]+:[ ]*", "", r[key] or "?"))[:70]
            for r in group
        )
        out.append("")
        out.append(f"== {title}: top reasons ==")
        for msg, n in tally.most_common(8):
            out.append(f"  {n:>4}  {msg}")

    text = chr(10).join(out)
    print(text)
    open(os.path.join(rundir, "report.txt"), "w", encoding="utf-8").write(text + chr(10))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--workers", type=int, default=8)
    r = sub.add_parser("run")
    r.add_argument("root")
    r.add_argument("--workers", type=int, default=8)
    r.add_argument("--max-gets", type=int, default=None)
    r.add_argument("--out", default=None)
    a = ap.parse_args()
    if a.cmd == "setup":
        setup(a.workers)
    else:
        run(a)


if __name__ == "__main__":
    main()
