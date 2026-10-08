#!/usr/bin/env python3
"""Run ObjAsm on the headless RPCEmu and bring its output back.

The DDE's ObjAsm is the reference implementation; where the manual is silent or
ambiguous, this is what settles the question. Built on the same JSON-RPC channel
as compiler/tools/run_on_emu.py.

    objasm_oracle.py <source under hostfs, RISC OS form> [objasm args...]

e.g. objasm_oracle.py rosasm.s.QuoteT
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
# Our own snapshot: boot.snap belongs to the user's session and may
# have been taken against a different ROM.
SNAP = os.path.join(CWD, "rosasm-boot.snap")


class Rpc:
    def __init__(self):
        self.p = subprocess.Popen(
            [EMU, "--rpc"], cwd=CWD,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
        )
        self.id = 0

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

    def close(self):
        try:
            self.call("quit")
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def boot(rpc):
    if os.path.exists(SNAP):
        try:
            rpc.call("snapshot.load", path=SNAP)
            return "snapshot"
        except RuntimeError as e:
            print(f"[snapshot unusable: {e}]", file=sys.stderr)
    t0 = time.time()
    for _ in range(180):
        st = rpc.call("status")
        if st.get("idle") or st.get("state") == "running":
            time.sleep(6)
            break
        time.sleep(1)
    try:
        rpc.call("snapshot.save", path=SNAP)
    except RuntimeError:
        pass
    return f"cold {time.time()-t0:.0f}s"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    source = sys.argv[1]
    extra = " ".join(sys.argv[2:])

    rpc = Rpc()
    try:
        how = boot(rpc)
        print(f"[booted from {how}]", file=sys.stderr)

        before = rpc.call("vdu.read")
        since = before.get("cursor", 0) if isinstance(before, dict) else 0

        # -g keeps it quiet about listings; the object goes to a scratch file.
        cmd = f"objasm {extra} -o <Wimp$ScrapDir>.rosasm_o {source}"
        result = rpc.call("cli", command=cmd)

        vdu = rpc.call("vdu.read", since=since)
        text = vdu.get("text", "") if isinstance(vdu, dict) else str(vdu)
        print(text)
        print("--- rpc result ---", file=sys.stderr)
        print(json.dumps(result, indent=2)[:400], file=sys.stderr)
    finally:
        rpc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
