#!/usr/bin/env python3
"""A failed child must bring down the full host stack and all private links."""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def wait_for(predicate, limit=8):
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.02)
    raise AssertionError("supervisor child start/exit timed out")


def run():
    with tempfile.TemporaryDirectory(prefix="child-exit-", dir=os.environ.get("TMPDIR")) as temp:
        root = Path(temp)
        with (root / "log").open("w+") as output:
            parent = subprocess.Popen([str(BUILD / "unified_daemon"), str(BUILD)],
                                      env={**os.environ, "TMPDIR": temp},
                                      stdout=output, stderr=subprocess.STDOUT)
            children = {}
            try:
                def started():
                    lines = (root / "log").read_text(errors="replace")
                    found = re.findall(r"started (\S+) pid=(\d+)", lines)
                    return found if len(found) == 8 and parent.poll() is None else None
                for name, pid in wait_for(started):
                    children.setdefault(name, []).append(int(pid))
                os.kill(children["vpmic-mock"][0], signal.SIGTERM)
                assert parent.wait(timeout=6) != 0, "child failure must fail supervisor"
                for pids in children.values():
                    for pid in pids:
                        try:
                            os.kill(pid, 0)
                        except ProcessLookupError:
                            continue
                        raise AssertionError(f"orphaned child {pid}")
                assert not list(root.glob("unified-board-*")), "private run dir leaked"
                print("PASS: unexpected Uno exit stops daemon, Pico, ESP and PTY relays")
            finally:
                if parent.poll() is None:
                    parent.terminate()
                    try:
                        parent.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        parent.kill()
                        parent.wait(timeout=3)


if __name__ == "__main__":
    run()
