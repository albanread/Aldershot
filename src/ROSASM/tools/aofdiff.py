#!/usr/bin/env python3
"""Assemble a unit both ways and compare the objects, byte for byte.

    aofdiff.py <RiscOS dir> <unit> [--build BCM2835] [--keep] [-n count]
    aofdiff.py <RiscOS dir> --all [--limit N]

Every other measure this project takes is a proxy. A unit that assembles may
have assembled wrongly; two hundred instructions go out as zero words with
nothing checking them; a listing that matches line for line can still carry
the wrong bytes in the byte column. The only test that answers the question
directly is to hand the same source to ObjAsm and to rosasm and compare what
comes out.

ObjAsm runs on the far side of the emulator, where it always has. What is new
is that it is asked for an object rather than a listing, and that the two
objects are compared by content rather than by eye -- see `aofdump --against`,
which hashes everything a compiler is responsible for and diffs only where the
hashes disagree.

## What has to match, and what does not

The bytes of each area, their attributes, the relocations and the symbols. Not
the identification string, which names the tool; not the chunk order, which the
specification leaves open and which the C compiler and the assembler already
differ on.

## Getting the source there and the object back

The staging directory is `xd` at the HostFS root and the export tree is `xh`,
both short, because a RISC OS command line is truncated at 256 bytes and a
component's own path eats most of it. The predefines go in a via-file for the
same reason: there are fourteen of them and `-via` reads arguments from a file.

HostFS buffers its writes, so the emulator returns to the `*` prompt before
Windows can see the object. Waiting for the file to appear is not optional; it
is what stopped 213 of 259 units being misreported as failures once already.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from component_flags import (  # noqa: E402
    assembler_flags,
    component_dirs,
    component_options,
    generated_sources,
)
from corpus_diff import chr_literal  # noqa: E402
from export_hdrs import export_hdrs  # noqa: E402
from roshell import Shell  # noqa: E402
import paths  # noqa: E402

HOSTFS = paths.HOSTFS
STAGE_NAME = "xd"
HDR_NAME = "xh"
STAGE = os.path.join(HOSTFS, STAGE_NAME)
HDRSTAGE = os.path.join(HOSTFS, HDR_NAME)
OBJASM = "HostFS::HostFS.$.oa"
ROSASM = paths.ROSASM
AOFDUMP = paths.AOFDUMP


def rm(path, patience=20.0):
    """Remove a tree, never following a link out of it.

    Patient, because the emulator has only just written the object file and
    HostFS may still hold it open: Windows refuses to unlink a file another
    process has, and giving up on that would throw away the whole run.
    """
    deadline = time.time() + patience
    while True:
        try:
            _rm(path)
            return
        except OSError:
            if time.time() >= deadline:
                raise
            time.sleep(0.25)


def _rm(path):
    if not os.path.isdir(path):
        return
    for entry in os.scandir(path):
        if entry.is_junction() or entry.is_symlink():
            (os.rmdir if entry.is_dir(follow_symlinks=False) else os.unlink)(entry.path)
        elif entry.is_dir(follow_symlinks=False):
            _rm(entry.path)
        else:
            os.unlink(entry.path)
    os.rmdir(path)


def stage_headers(hdrdirs):
    """Put the export tree where the emulator can reach it, once.

    Per directory rather than per tree: an earlier harness staged the headers
    flat, and skipping the whole job because the parent already existed left
    that tree in place with no `Global` in it for `Hdr$Path` to point at.
    Every header lookup then failed, silently, as an unknown opcode wherever
    a macro should have been.
    """
    for d in hdrdirs:
        leaf = os.path.basename(d)
        dest = os.path.join(HDRSTAGE, leaf)
        if os.path.isdir(dest) or not os.path.isdir(d):
            continue
        shutil.copytree(d, dest, dirs_exist_ok=True)


STAGED = [None]


def stage_unit(unit, generated):
    """Copy the unit's whole component across, and anything generated for it.

    The whole `s/`, not just the unit: `GetAll` is one file that GETs two
    hundred others, and a component that assembles from one directory expects
    the rest of it to be there.

    A component holding several units is staged once. The Kernel's is nine
    hundred files and copying it again for each of its units was most of the
    time a run spent, so what changes between them -- the object directory --
    is cleared and the rest left where it is.
    """
    comp = os.path.dirname(os.path.dirname(unit))
    if STAGED[0] == (comp, tuple(generated)):
        rm(os.path.join(STAGE, "o"))
        os.makedirs(os.path.join(STAGE, "o"), exist_ok=True)
        return comp
    rm(STAGE)
    STAGED[0] = (comp, tuple(generated))
    os.makedirs(os.path.join(STAGE, "o"), exist_ok=True)
    for name in ("s", "hdr", "Hdr"):
        src = os.path.join(comp, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(STAGE, name), dirs_exist_ok=True)
            both_ways(src, name)
    # And the loose files at the component's root. `VersionASM` is one, and
    # ninety other components have one: `GET VersionASM` names it with no
    # directory, so it has to sit where `-i` points. Assembling here finds it
    # because the component's own directory is on the include path; ObjAsm has
    # only what is staged.
    for name in os.listdir(comp):
        src = os.path.join(comp, name)
        if os.path.isfile(src) and not name.startswith("."):
            shutil.copy2(src, os.path.join(STAGE, name))
    # A generated source -- `s.TokHelpSrc` and the like -- sits alongside.
    for i, arg in enumerate(generated):
        if arg == "-I" and i + 1 < len(generated):
            d = generated[i + 1]
            if os.path.isdir(d):
                shutil.copytree(d, STAGE, dirs_exist_ok=True)
    return comp


def both_ways(src, kind):
    """Offer a component's own headers under the other spelling too.

    The sources ask for one of their headers both ways round -- `GET
    hdr.WIMPMacros` and `GET BCM2835.hdr` -- and ObjAsm reads the second
    literally: directory `BCM2835`, file `hdr`. The git tree holds it as
    `hdr/BCM2835`, so a component that writes it that way fails on its first
    header and then on everything the header would have defined. Both
    spellings are staged and ObjAsm can have whichever it asks for.
    """
    for name in os.listdir(src):
        f = os.path.join(src, name)
        if not os.path.isfile(f):
            continue
        d = os.path.join(STAGE, name)
        os.makedirs(d, exist_ok=True)
        shutil.copy2(f, os.path.join(d, kind))


def via_file(predefines):
    """ObjAsm's `-via`, because fourteen predefines will not fit a command line.

    A string value cannot simply be quoted. ObjAsm's argument parser collapses
    a doubled quote to nothing, so `-pd "APCS SETS "APCS-32""` reaches the
    evaluator with the value unquoted, where it reads as an undefined symbol.
    No quoting form survives it, and building the value from character codes
    sidesteps quoting altogether -- which is what the corpus harness settled
    on, and where `chr_literal` comes from.
    """
    path = os.path.join(STAGE, "via")
    with open(path, "w", encoding="latin-1") as f:
        for pd in predefines:
            m = re.match(r'^(\S+)\s+SETS\s+"(.*)"$', pd)
            if m:
                pd = m.group(1) + " SETS " + chr_literal(m.group(2))
            f.write('-pd "' + pd + '"\n')
    return STAGE_NAME + ".via"


def objasm(sh, unit, predefines, extra_i, variables, hdrdirs):
    """Assemble on the emulator, returning the host path of the object.

    The build's variables are set twice over, and they are not the same thing
    both times. As ObjAsm predefines they are assembly-time variables, which is
    what a conditional reads. As RISC OS system variables they are what the
    *filesystem* substitutes into a name: `GET Hdr:APCS.<APCS>` is resolved
    before ObjAsm ever sees it, and without `Set APCS APCS-32` there is no such
    file.
    """
    name = os.path.basename(unit)
    sh.cmd("HostFS")
    # From the component's own directory, which is where the build runs it.
    # The sources spell a header of their own both ways round -- `GET
    # hdr.WIMPMacros` and `GET BCM2835.hdr` -- and the reversed spelling is
    # resolved against the current directory. Run from anywhere else it has
    # nowhere to land, and a whole component fails on its first header.
    sh.cmd(f"Dir HostFS::HostFS.$.{STAGE_NAME}")
    for k, v in sorted(variables.items()):
        sh.cmd(f"Set {k} {v}")
    root = "HostFS::HostFS.$"
    # The same path the build sets, in the same order: the component's own
    # headers first, then each exported tree. Naming them from `hdrdirs`
    # rather than listing them keeps `Interface2` -- which the build has and
    # which was missing here -- from being dropped again.
    exported = "".join(f",{root}.{HDR_NAME}.{os.path.basename(d)}." for d in hdrdirs)
    sh.cmd(f"Set Hdr$Path @.hdr.{exported}")
    via_file(predefines)
    # Redirected into a file rather than read off the screen. ObjAsm prints
    # the offending listing line before each error, so a unit with twenty
    # errors says four times more than a terminal holds, and the one line
    # that explains the run is always the first.
    # HostFS spells a file's type as a suffix, so the log is `log,ffd`.
    logs = [os.path.join(STAGE, "log" + x) for x in (",ffd", "")]
    for p in logs:
        if os.path.isfile(p):
            os.unlink(p)
    # What the build gives it, from BuildSys/Makefiles/StdTools:
    #
    #     ASFLAGS += -ihdr -i<Hdr$Dir>.Global -i<Hdr$Dir>.Interface     #                -i<Hdr$Dir>.Interface2
    #
    # `Hdr$Path` is only consulted for a name written `Hdr:Foo`; the sources
    # more often write `GET ListOpts` bare, and that is resolved against the
    # include list. Without it every header a unit reads this way is missing,
    # which arrives as an unknown opcode wherever one of its macros is used.
    includes = " -i @ -i hdr" + "".join(
        f" -i {root}.{HDR_NAME}.{os.path.basename(d)}" for d in hdrdirs
    )
    cmd = (
        # `-quit` because the build passes it: BuildSys documents ASFLAGS as
        # `-Stamp -quit ...`, and without it ObjAsm can finish its work and
        # still not return -- VFPSupport's Power32 wrote `0 Errors, 165
        # Warnings` and then sat there, object unwritten, until the harness
        # gave up and called it wedged.
        f"Run {OBJASM} -quit -via via -o o.{name}"
        f"{includes}{extra_i} s.{name}"
        " { > log }"
    )
    # The output is redirected, so nothing reaches the screen until the
    # prompt comes back; quiescence means nothing here.
    # The log getting longer is ObjAsm working, so the wait follows that
    # rather than a clock: VFPSupport's Power32 warns about every VFP
    # instruction it has and takes ten minutes to say so.
    def written():
        return tuple(os.path.getsize(p) if os.path.isfile(p) else -1 for p in logs)

    screen = sh.cmd(
        cmd, settle=1.0, max_wait=90.0, require_prompt=True, progress=written
    )
    out = screen
    deadline = time.time() + 5.0
    while not any(os.path.isfile(p) for p in logs) and time.time() < deadline:
        time.sleep(0.2)
    for p in logs:
        if os.path.isfile(p):
            try:
                out = open(p, "rb").read().decode("latin-1").replace("\r", "\n")
            except OSError:
                pass
            break
    # HostFS buffers: wait for Windows to see the file.
    for suffix in (",ffd", ""):
        path = os.path.join(STAGE, "o", name + suffix)
        deadline = time.time() + 5.0
        while not os.path.isfile(path) and time.time() < deadline:
            time.sleep(0.2)
        if os.path.isfile(path):
            return path, out
    # No object and the log says nothing: whatever went wrong went to the
    # screen instead, which the redirection does not capture. The Kernel
    # comes back with the banner and nothing else, and the reason has to
    # be somewhere.
    if screen.strip() and screen.strip() not in out:
        out = (out or "").rstrip() + chr(10) + "[screen] " + screen.strip()
    return None, out


def ours(unit, comp, hdrdirs, predefines, extra, out_path):
    """Assemble here, with the same inputs."""
    args = [ROSASM, unit, "-I", comp, "-I", os.path.join(comp, "hdr")]
    for d in hdrdirs:
        args += ["-I", d]
    args += extra
    for pd in predefines:
        args += ["-PD", pd]
    args += ["-o", out_path]
    return subprocess.run(args, capture_output=True, text=True)


class Setup:
    """Everything both assemblers need, worked out once."""

    def __init__(self, root, build, stage=True):
        self.root = root
        self.options = component_options(root, build)
        self.dirs = component_dirs(root)
        tmp = tempfile.gettempdir()
        self.variables, self.hdrdirs = export_hdrs(
            root, os.path.join(tmp, "rosasm-export"), build, quiet=True
        )
        self.base = [f'{k} SETS "{v}"' for k, v in sorted(self.variables.items())]
        self.staged = os.path.join(tmp, "rosasm-generated")
        # Only the emulator needs the headers where it can reach them.
        if stage:
            stage_headers(self.hdrdirs)

    def inputs_for(self, unit):
        """The predefines and generated sources this unit needs."""
        u = unit.replace("\\", "/")
        flags = assembler_flags(self.root, u, self.options, self.dirs)
        pds = self.base + [flags[i + 1] for i, a in enumerate(flags) if a == "-PD"]
        generated = generated_sources(
            self.root, u, self.options, self.dirs, self.staged, self.hdrdirs
        )
        return pds, generated


def compare_one(setup, sh, unit, limit, keep, full=False):
    """Assemble one unit both ways. Returns (verdict, detail).

    The verdict is one of `same`, `differ`, `objasm-failed`, `ours-failed`.
    """
    predefines, generated = setup.inputs_for(unit)
    comp = stage_unit(unit, generated)
    theirs, log = objasm(sh, unit, predefines, "", setup.variables, setup.hdrdirs)
    mine = os.path.join(tempfile.gettempdir(), "rosasm-ours.o")
    if os.path.isfile(mine):
        os.unlink(mine)
    r = ours(unit, comp, setup.hdrdirs, predefines, generated, mine)

    if theirs is None:
        # ObjAsm prints the offending listing line before its error, so
        # the tail of the log is what says why.
        return "objasm-failed", "\n".join(
            x for x in log.strip().splitlines()[-14:] if x.strip()
        )
    if r.returncode != 0 or not os.path.isfile(mine):
        return "ours-failed", "\n".join(r.stderr.strip().splitlines()[-8:])
    d = subprocess.run(
        [AOFDUMP, mine, "--against", theirs, "-n", str(limit)],
        capture_output=True,
        text=True,
    )
    if not keep:
        rm(STAGE)
        STAGED[0] = None
    return ("same" if d.returncode == 0 else "differ"), d.stdout.strip()


def one(root, unit, build, keep, limit, full=False):
    setup = Setup(root, build)
    sh = Shell(quiet=True)
    sh.boot()
    verdict, detail = compare_one(setup, sh, unit, limit, keep, full)
    try:
        sh.close()
    except Exception:
        pass
    print(f"=== {os.path.relpath(unit, os.path.join(root, 'Sources'))}")
    print(detail)
    return 0 if verdict == "same" else 1


def write_row(report, verdict, label, detail):
    """One unit's verdict, on disc before the next one starts."""
    if report is None:
        return
    report.write(f"{verdict:14} {label}\n")
    if verdict != "same":
        for line in (detail or "").splitlines():
            report.write(f"    {line}\n")
    report.flush()


def many(root, build, limit, count, out):
    """Every unit, through one emulator session.

    Booting takes four seconds, so a run of two hundred units is four seconds
    plus the assembling rather than fourteen minutes of booting.
    """
    from corpus_diff import units

    setup = Setup(root, build)
    us = units(root)
    if count:
        us = us[:count]
    sh = Shell(quiet=True)
    sh.boot()
    tally = {}
    rows = []
    # Written as they are decided, not at the end. A run is hours long, and
    # one that has to be stopped part way through should still have said
    # everything it found.
    report = open(out, "w", encoding="utf-8") if out else None
    for n, unit in enumerate(us, 1):
        label = os.path.relpath(unit, os.path.join(root, "Sources"))
        try:
            verdict, detail = compare_one(setup, sh, unit, limit, False)
        except OSError as e:
            # Staging, not the emulator: keep the instance and lose one unit.
            tally["staging-failed"] = tally.get("staging-failed", 0) + 1
            rows.append(("staging-failed", label, str(e)))
            write_row(report, "staging-failed", label, str(e))
            print(f"[staging failed for {label}: {e}]", file=sys.stderr)
            continue
        except Exception as e:
            # A dead instance loses every unit after it unless it is restarted.
            print(f"[restart after {label}: {e}]", file=sys.stderr)
            try:
                sh.close()
            except Exception:
                pass
            sh = Shell(quiet=True)
            sh.boot()
            verdict, detail = "emulator-died", str(e)
        tally[verdict] = tally.get(verdict, 0) + 1
        rows.append((verdict, label, detail))
        write_row(report, verdict, label, detail)
        if n % 10 == 0:
            print(f"  {n}/{len(us)}  {tally}", flush=True)
    try:
        sh.close()
    except Exception:
        pass

    order = ["same", "differ", "ours-failed", "objasm-failed",
             "staging-failed", "emulator-died"]
    print()
    print(f"units compared   {len(rows)}")
    for k in order:
        if k in tally:
            print(f"  {k:15} {tally[k]}")
    agreed = tally.get("same", 0)
    both = agreed + tally.get("differ", 0)
    if both:
        print(f"\nof the {both} both assembled, {agreed} are identical "
              f"({100 * agreed / both:.1f}%)")
    if report is not None:
        report.close()
        print(f"written to {out}")
    return 0 if tally.get("differ", 0) == 0 else 1


def reference_paths(ref_dir, root, unit):
    """Where one unit's reference object and log are kept.

    Named by the unit's place in the tree, so the set can be read by anyone
    who has the sources and says plainly which unit each object came from.
    """
    rel = os.path.relpath(unit, os.path.join(root, "Sources")).replace("\\", "/")
    base = os.path.join(ref_dir, rel)
    return base + ".o", base + ".log"


def chosen(us, only):
    """The units whose path contains `only`, for working on one component."""
    if not only:
        return us
    needle = only.replace(chr(92), "/").lower()
    return [u for u in us if needle in u.replace(chr(92), "/").lower()]


def collect(root, build, ref_dir, count, only=None):
    """ObjAsm over every unit, once, keeping what it produces.

    ObjAsm's answer depends on the sources and on nothing else. It is the
    same answer every time *this* assembler changes, which is many times an
    hour, and it costs a minute of emulation each time it is asked. Asking
    once and keeping the objects turns every comparison after it into a
    local one.
    """
    from corpus_diff import units

    setup = Setup(root, build)
    us = chosen(units(root), only)
    if count:
        us = us[:count]
    # Only what is not already there, so a collection is extended in small
    # batches rather than started again. A unit that has a log has been
    # asked, whether or not it produced an object.
    todo = [u for u in us if not os.path.isfile(reference_paths(ref_dir, root, u)[1])]
    done = len(us) - len(todo)
    if done:
        print(f"{done} already collected, {len(todo)} to go", flush=True)
    if not todo:
        return 0
    sh = Shell(quiet=True)
    sh.boot()
    kept = failed = 0
    for n, unit in enumerate(todo, 1):
        obj_path, log_path = reference_paths(ref_dir, root, unit)
        os.makedirs(os.path.dirname(obj_path), exist_ok=True)
        # Twice at most. A unit that leaves the emulator not answering takes
        # the next one down with it, and a fresh instance usually assembles it
        # -- but if it does not, the collection has to move on and say so
        # rather than stop here or come back to it every run.
        theirs = log = None
        staging = False
        for attempt in (1, 2):
            try:
                predefines, generated = setup.inputs_for(unit)
                stage_unit(unit, generated)
                theirs, log = objasm(
                    sh, unit, predefines, "", setup.variables, setup.hdrdirs
                )
                break
            except TimeoutError as e:
                # Before OSError, which it is a kind of: an emulator that
                # never came back to the prompt is not a staging problem.
                print(f"[restart after {unit}: {e}]", file=sys.stderr)
                try:
                    sh.close()
                except Exception:
                    pass
                sh = Shell(quiet=True)
                sh.boot()
                if attempt == 2:
                    log = f"[the emulator stopped answering twice: {e}]"
            except OSError as e:
                # An emulator that has exited leaves a broken pipe, which
                # arrives here as an ordinary file error with no filename on
                # it. Which it is, is settled by asking whether the process
                # is still there rather than by reading the message.
                if sh.p.poll() is not None:
                    print(f"[restart after {unit}: {e}]", file=sys.stderr)
                    try:
                        sh.close()
                    except Exception:
                        pass
                    sh = Shell(quiet=True)
                    sh.boot()
                    if attempt == 2:
                        log = f"[the emulator stopped twice: {e}]"
                    continue
                # Staging, not the emulator: lose one unit, keep the instance.
                # Named, because "Invalid argument" on its own says nothing
                # about which file the host would not have.
                where = f" on {e.filename}" if getattr(e, "filename", None) else ""
                print(f"[staging failed for {unit}: {e}{where}]", file=sys.stderr)
                staging = True
                break
            except Exception as e:
                print(f"[restart after {unit}: {e}]", file=sys.stderr)
                try:
                    sh.close()
                except Exception:
                    pass
                sh = Shell(quiet=True)
                sh.boot()
                if attempt == 2:
                    log = f"[the emulator stopped answering twice: {e}]"
        if staging:
            continue
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(log or "")
        if theirs is None:
            failed += 1
        else:
            shutil.copy2(theirs, obj_path)
            kept += 1
        if n % 10 == 0:
            print(f"  {n}/{len(todo)}  {kept} kept, {failed} refused", flush=True)
    try:
        sh.close()
    except Exception:
        pass
    print()
    print(f"units asked      {len(todo)}")
    print(f"objects kept     {kept}")
    print(f"ObjAsm refused   {failed}")
    print(f"written to {ref_dir}")
    return 0


def against(root, build, ref_dir, limit, count, out, only=None):
    """Compare with the reference set, which needs no emulator at all."""
    from corpus_diff import units

    setup = Setup(root, build, stage=False)
    us = chosen(units(root), only)
    if count:
        us = us[:count]
    tally = {}
    report = open(out, "w", encoding="utf-8") if out else None
    mine = os.path.join(tempfile.gettempdir(), "rosasm-ours.o")
    for n, unit in enumerate(us, 1):
        label = os.path.relpath(unit, os.path.join(root, "Sources"))
        obj_path, _ = reference_paths(ref_dir, root, unit)
        if not os.path.isfile(obj_path):
            # ObjAsm produced nothing for this unit, so there is nothing to
            # be right or wrong against.
            verdict, detail = "no-reference", ""
        else:
            predefines, generated = setup.inputs_for(unit)
            comp = os.path.dirname(os.path.dirname(unit))
            if os.path.isfile(mine):
                os.unlink(mine)
            r = ours(unit, comp, setup.hdrdirs, predefines, generated, mine)
            if r.returncode != 0 or not os.path.isfile(mine):
                verdict = "ours-failed"
                detail = "\n".join(r.stderr.strip().splitlines()[-8:])
            else:
                d = subprocess.run(
                    [AOFDUMP, mine, "--against", obj_path, "-n", str(limit)],
                    capture_output=True,
                    text=True,
                )
                verdict = "same" if d.returncode == 0 else "differ"
                detail = d.stdout.strip()
        tally[verdict] = tally.get(verdict, 0) + 1
        write_row(report, verdict, label, detail)
        if n % 25 == 0:
            print(f"  {n}/{len(us)}  {tally}", flush=True)

    order = ["same", "differ", "ours-failed", "no-reference"]
    print()
    print(f"units compared   {sum(tally.values())}")
    for k in order:
        if k in tally:
            print(f"  {k:15} {tally[k]}")
    agreed = tally.get("same", 0)
    both = agreed + tally.get("differ", 0)
    if both:
        print(f"\nof the {both} both assembled, {agreed} are identical "
              f"({100 * agreed / both:.1f}%)")
    if report is not None:
        report.close()
        print(f"written to {out}")
    return 0 if tally.get("differ", 0) == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("unit", nargs="?")
    ap.add_argument("--build", default="BCM2835")
    ap.add_argument("--all", action="store_true", help="every unit in the corpus")
    ap.add_argument("--limit", type=int, help="stop after this many units")
    ap.add_argument("--out", help="write the per-unit verdicts here")
    ap.add_argument("--keep", action="store_true", help="leave the staged files")
    ap.add_argument("-n", type=int, default=12, help="differing words to show")
    ap.add_argument("--full", action="store_true", help="print ObjAsm's whole log")
    ap.add_argument(
        "--collect",
        metavar="DIR",
        help="assemble every unit with ObjAsm once and keep the objects here",
    )
    ap.add_argument(
        "--against",
        metavar="DIR",
        help="compare with a collected set, without the emulator",
    )
    ap.add_argument(
        "--only", metavar="TEXT", help="units whose path contains this"
    )
    a = ap.parse_args()
    if a.collect:
        raise SystemExit(collect(a.root, a.build, a.collect, a.limit, a.only))
    if a.against:
        raise SystemExit(against(a.root, a.build, a.against, a.n, a.limit, a.out, a.only))
    if a.all:
        raise SystemExit(many(a.root, a.build, a.n, a.limit, a.out))
    if not a.unit:
        raise SystemExit("give a unit to compare, or --all")
    raise SystemExit(one(a.root, a.unit, a.build, a.keep, a.n, a.full))


if __name__ == "__main__":
    main()
