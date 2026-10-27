#!/usr/bin/env python3
"""Daemon GPIO pin errors and the real C client's stderr context."""
import os
from pathlib import Path
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
        return client.recv(256).decode().strip()


def run():
    with tempfile.TemporaryDirectory(prefix="boardstate-errors-", dir=os.environ.get("TMPDIR", str(ROOT))) as tmp:
        sock = Path(tmp) / "board.sock"
        client = Path(tmp) / "error-client"
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(ROOT / "tests/boardstate-error-client.c"),
                        str(ROOT / "boardstate-daemon/client.c"), "-o", str(client)],
                       check=True, timeout=20)
        with open(Path(tmp) / "daemon.log", "w+b") as log:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)],
                                      stdout=subprocess.DEVNULL, stderr=log)
            try:
                for _ in range(100):
                    if sock.exists():
                        break
                    if daemon.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read().decode())
                    time.sleep(.01)
                else:
                    raise RuntimeError("boardstate socket unavailable")
                assert request(sock, "GPIO pico 29 GET") == "0"
                assert request(sock, "GPIO esp 16 GET") == "0"
                assert request(sock, "GPIO pico 30 DIR 1") == (
                    "ERR GPIO pico pin 30 out of range (valid 0-29)")
                assert request(sock, "GPIO esp 17 GET") == (
                    "ERR GPIO esp pin 17 out of range (valid 0-16; EN/RESET aren't GPIO)")
                assert request(sock, "GPIO pico nope GET") == (
                    "ERR GPIO pico pin 'nope' is not an integer (valid 0-29)")
                assert request(sock, "GPIO pico 20") == (
                    "ERR GPIO pico pin 20 missing operation")
                result = subprocess.run([str(client)], env={**os.environ,
                                        "BOARDSTATE_SOCKET": str(sock)},
                                        capture_output=True, text=True, timeout=5)
                assert result.returncode == 0, result
                assert result.stderr.splitlines() == [
                    "boardstate: ERR GPIO pico pin 30 out of range (valid 0-29)"
                    " (request: GPIO pico 30 DIR 1)",
                    "boardstate: ERR GPIO esp pin 17 out of range"
                    " (valid 0-16; EN/RESET aren't GPIO)"
                    " (request: GPIO esp 17 GET)",
                ], result.stderr
                print("PASS: GPIO pin errors identify board, pin, range and client request")
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                daemon.wait(timeout=3)


if __name__ == "__main__":
    run()
