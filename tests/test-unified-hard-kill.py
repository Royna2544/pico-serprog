#!/usr/bin/env python3
"""SIGKILL of the supervisor must not leave its direct children running."""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def running(pid):
    status = Path(f"/proc/{pid}/stat")
    try:
        # A zombie is already dead; an unrelated PID must never be signaled here.
        return status.read_text().split(") ", 1)[1][0] != "Z"
    except FileNotFoundError:
        return False


def run():
    with tempfile.TemporaryDirectory(prefix="hard-kill-", dir=os.environ.get("TMPDIR")) as temp:
        root = Path(temp)
        with (root / "log").open("w+") as output:
            parent = subprocess.Popen([str(BUILD / "unified_daemon"), str(BUILD)],
                                      env={**os.environ, "TMPDIR": temp},
                                      stdout=output, stderr=subprocess.STDOUT)
            children = []
            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    text = (root / "log").read_text(errors="replace")
                    children = [int(pid) for pid in re.findall(r"started \S+ pid=(\d+)", text)]
                    if len(children) == 8 and parent.poll() is None:
                        break
                    time.sleep(.02)
                assert len(children) == 8 and parent.poll() is None, "not all children started"
                os.kill(parent.pid, signal.SIGKILL)
                parent.wait(timeout=3)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and any(map(running, children)):
                    time.sleep(.02)
                assert not any(map(running, children)), "parent-death signal left a child running"
                print("PASS: SIGKILL supervisor triggers parent-death termination of all children")
            finally:
                if parent.poll() is None:
                    parent.terminate()
                    parent.wait(timeout=3)
                for pid in children:
                    if running(pid):
                        os.kill(pid, signal.SIGKILL)


if __name__ == "__main__":
    run()
