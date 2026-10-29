#!/usr/bin/env python3
"""Exercise the real host supervisor, board model, and sketch processes."""
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def request(path, text):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(str(path))
        client.sendall(text.encode())
        return client.recv(128).decode().strip()


def wait_for(predicate, seconds, explanation):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.02)
    raise AssertionError(explanation)


def run():
    with tempfile.TemporaryDirectory(prefix="unified-test-", dir=os.environ.get("TMPDIR")) as temp:
        root = Path(temp)
        with (root / "run.log").open("w+") as log:
            proc = subprocess.Popen([str(BUILD / "unified_daemon"), "--verbose", str(BUILD)],
                                    stdout=log, stderr=subprocess.STDOUT,
                                    env={**os.environ, "TMPDIR": str(root)})
            pids = []
            try:
                def ready():
                    dirs = list(root.glob("unified-board-*/board.sock"))
                    return dirs[0] if dirs and proc.poll() is None else None

                sock = wait_for(ready, 8, "supervisor did not start board socket")
                run_dir = sock.parent
                for name in ("uart0", "cdc0", "cdc1", "cdc2"):
                    assert (run_dir / f"mock-{name}").is_symlink(), name
                    assert (run_dir / f"host-{name}").is_symlink(), name
                assert request(sock, "GPIO pico 28 PUT 1") == "OK"
                assert request(sock, "GPIO pico 28 DIR 1") == "OK"
                assert request(sock, "GPIO pico 28 DIR 0") == "OK"
                assert request(sock, "GPIO pico 8 MUX uart") == "OK"
                assert request(sock, "UART pico TX 0") == "OK"
                assert request(sock, "UART pico TX 255") == "OK"
                def diagnostic():
                    log.flush()
                    output = (root / "run.log").read_text(errors="replace")
                    return ("pico> pin 28 set high" in output and
                            "pico> pin 28 reset to hi-Z" in output and
                            "uart> pico->esp [00]" in output and
                            "uart> pico->esp [ff]" in output)
                wait_for(diagnostic, 4, "verbose pin/UART diagnostics missing")
                def started_all():
                    output = (root / "run.log").read_text(errors="replace")
                    found = [int(pid) for pid in re.findall(r"started \S+ pid=(\d+)", output)]
                    return found if len(found) == 8 and proc.poll() is None else None
                pids = wait_for(started_all, 8, "all eight child/helper processes did not start")
                assert request(sock, "GPIO uno 2 DRIVE") == "sio-out"
                assert request(sock, "GPIO pico 6 GET") == "0"
                wait_for(lambda: request(sock, "GPIO pico 14 GET") == "1", 2,
                         "active-low GP6 should initialize signal-action echo GP14 high")
                print("PASS: four children, Uno NPN pulls GP6 low, PTYs, verbose GPIO/UART")
            finally:
                if proc.poll() is None:
                    proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
                for pid in pids:
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        continue
                    else:
                        raise AssertionError(f"child {pid} survived supervisor exit")
                assert not list(root.glob("unified-board-*")), "private run directory leaked"


if __name__ == "__main__":
    run()
