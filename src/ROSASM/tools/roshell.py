#!/usr/bin/env python3
"""Drive the headless RPCEmu's supervisor prompt and capture output.

The emulator exposes a small JSON-RPC channel: `status`, `type` (inject
keystrokes), `vdu.read` (screen text since an offset), `reset`, `continue`,
`step`, and `snapshot.save`/`snapshot.load`. There is no command method — you
type at the `*` prompt like a person would.

Used as a library by the ObjAsm oracle, or directly:

    roshell.py "Cat" "Show ."
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402

CWD = paths.RPCEMU
EMU = paths.EMU
# Our own snapshot; boot.snap belongs to the user's session.
SNAP = os.path.join(CWD, "rosasm-boot.snap")


LOCK = ".roshell-lock"
# Directories this process is already driving. The lock file catches another
# process; this catches a second `Shell` here, which is the easier mistake.
HELD = set()


def claim(cwd):
    """Take the instance directory, or say who has it.

    Two emulators sharing one HostFS tree each delete the staging the other
    is assembling from and read objects the other has just removed. Nothing
    errors: units come back with no object and an empty log, which reads
    exactly like ObjAsm refusing them, and thirty units of a collection went
    that way before anyone noticed the second window.

    A farm of instances is fine -- that is what `cwd` is for -- as long as
    each has a directory of its own, which is what this checks.
    """
    key = os.path.normcase(os.path.abspath(cwd))
    if key in HELD:
        raise RuntimeError(
            f"this process is already driving the emulator in {cwd}; close "
            "that Shell before opening another"
        )
    path = os.path.join(cwd, LOCK)
    try:
        with open(path) as f:
            held = int(f.read().strip() or 0)
    except (OSError, ValueError):
        held = 0
    if held and held != os.getpid() and alive(held):
        raise RuntimeError(
            f"process {held} is already driving the emulator in {cwd}; stop "
            "it first -- two instances share one HostFS tree and would "
            "quietly corrupt each other's staging"
        )
    # A driver that was killed leaves its emulator behind, and an orphan is
    # invisible to the lock: it has no live process to name. It is still
    # sharing the HostFS tree, so it is stopped here rather than left to
    # corrupt the run that is about to start.
    orphans(cwd)
    with open(path, "w") as f:
        f.write(str(os.getpid()))
    HELD.add(key)
    return path


def orphans(cwd):
    """Stop any emulator running from this directory with no driver left."""
    if os.name != "nt":
        # tasklist and taskkill are Windows'. RPCEmu headless is a
        # Windows build, so there is nothing to orphan anywhere else.
        return
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq rpcemu-headless.exe", "/NH"],
            capture_output=True, text=True, timeout=20,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return
    pids = [
        line.split()[1]
        for line in out.splitlines()
        if line.strip().lower().startswith("rpcemu-headless.exe")
    ]
    for pid in pids:
        print(f"[stopping orphaned emulator {pid}]", file=sys.stderr)
        subprocess.run(["taskkill", "/F", "/PID", pid],
                       capture_output=True, text=True)
    if pids:
        time.sleep(1.0)


def alive(pid):
    """Is that process still running? A stale lock must not block a run."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/NH"],
            capture_output=True, text=True, timeout=20,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return str(pid) in out


def release(cwd):
    HELD.discard(os.path.normcase(os.path.abspath(cwd)))
    try:
        os.unlink(os.path.join(cwd, LOCK))
    except OSError:
        pass


class Shell:
    def __init__(self, quiet=True, cwd=None):
        """`cwd` selects which emulator instance to drive, so a farm of copied
        instances can run in parallel. Each needs its own directory: RPCEmu
        writes cmos.ram on exit, and two instances sharing one would race."""
        self.quiet = quiet
        self.cwd = cwd or CWD
        self.lock = claim(self.cwd)
        self.p = subprocess.Popen(
            [os.path.join(self.cwd, "rpcemu-headless" + paths.EXE), "--rpc"], cwd=self.cwd,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
        )
        self.id = 0

    # ---- raw channel -----------------------------------------------------

    def call(self, method, **params):
        self.id += 1
        req = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params:
            req["params"] = params
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("emulator closed the channel")
            obj = json.loads(line)
            if obj.get("id") == self.id:
                if "error" in obj:
                    raise RuntimeError(f"{method}: {obj['error']}")
                return obj.get("result")

    def log(self, msg):
        if not self.quiet:
            print(msg, file=sys.stderr)

    # ---- boot ------------------------------------------------------------

    def boot(self, timeout=90):
        """Wait for the `*` supervisor prompt."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            text = self.call("vdu.read").get("text", "")
            if text.rstrip().endswith("*"):
                self.log(f"[cold boot {time.time()-t0:.0f}s]")
                return
            time.sleep(1)
        raise RuntimeError("no supervisor prompt within timeout")

    # ---- commands --------------------------------------------------------

    def cmd(self, command, settle=2.0, max_wait=120.0, require_prompt=False,
            progress=None):
        """Type a command, wait for the prompt to come back, return its output.

        Two quirks of the keyboard injection are worked around here. The first
        character of a burst arrives with the wrong shift state (`Cat` came out
        as `cCat`), so the command is prefixed with a space, which RISC OS
        ignores at a `*` prompt. And `vdu.read`'s `since` parameter did not
        filter reliably, so output is sliced by remembered length instead.
        """
        base = len(self.call("vdu.read").get("text", ""))
        self.call("type", text=" " + command + "\r")

        t0 = time.time()
        last = ""
        stable = 0.0
        # `max_wait` bounds how long the emulator may be *idle*, not how long
        # the command may take. A caller that can see the command working --
        # a log file getting longer -- says so here, and a unit that takes
        # ten minutes of emulation is not mistaken for one that has stopped.
        moved = progress() if progress else None
        while time.time() - t0 < max_wait:
            time.sleep(0.5)
            if progress:
                now = progress()
                if now != moved:
                    moved, t0, stable = now, time.time(), 0.0
            text = self.call("vdu.read").get("text", "")[base:]
            if text == last:
                stable += 0.5
                # Back at the prompt and nothing changing: done.
                if text.rstrip().endswith("*") and stable >= settle:
                    break
                # A command that says nothing looks exactly like one that
                # has finished, so a caller that has redirected the output
                # waits for the prompt and nothing else. Without that, any
                # assembly taking more than six seconds was abandoned while
                # it ran -- the log read half-written, the next unit staged
                # on top of the files still in use, and every unit after it
                # producing nothing at all.
                if not require_prompt and stable >= max(settle * 3, 6.0):
                    break
            else:
                stable = 0.0
                last = text
        # A caller that waits for the prompt is waiting for the command
        # to finish, so not seeing one means it did not. Saying so lets
        # the run restart the instance rather than carry on reading
        # files that are still being written.
        if require_prompt and not last.rstrip().endswith("*"):
            raise TimeoutError(
                f"no prompt after {max_wait:.0f}s: {command.split()[0]}"
            )
        # Drop the echoed command line and the trailing prompt.
        out = last
        head, sep, rest = out.partition(chr(10))
        if sep and command.split()[0] in head:
            out = rest
        return out.strip().removesuffix("*").rstrip()

    def close(self):
        try:
            self.call("quit")
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()
        # Killed rather than left: an instance that outlives its driver is
        # what the lock exists to catch, and it should not need catching.
        if self.p.poll() is None:
            self.p.kill()
        release(self.cwd)


def main():
    sh = Shell(quiet=False)
    try:
        sh.boot()
        for c in sys.argv[1:]:
            print(f"*{c}")
            print(sh.cmd(c))
            print()
    finally:
        sh.close()


if __name__ == "__main__":
    main()
