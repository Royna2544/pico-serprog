#!/usr/bin/env python3
"""Prove GPIO output transitions and binary UART diagnostics are opt-in."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def request(sock, line):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.connect(str(sock))
        peer.sendall(line.encode())
        return peer.recv(128).decode().strip()


def exercise(verbose, temp):
    sock = Path(temp) / "board.sock"
    log = Path(temp) / "board.log"
    env = {**os.environ, "BOARDSTATE_VERBOSE": "1" if verbose else "0"}
    with log.open("w+") as output:
        daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                   str(BUILD / "board.dtb"), str(sock)],
                                  stdout=output, stderr=subprocess.STDOUT, env=env)
        try:
            for _ in range(150):
                if sock.exists():
                    break
                assert daemon.poll() is None, log.read_text()
                time.sleep(.02)
            else:
                raise AssertionError("board daemon did not start")
            for cmd in ("GPIO pico 28 PUT 1", "GPIO pico 28 DIR 1",
                        "GPIO pico 28 DIR 0", "GPIO pico 8 MUX uart",
                        "UART pico TX 0", "UART pico TX 255"):
                assert request(sock, cmd) == "OK", cmd
        finally:
            daemon.terminate()
            daemon.wait(timeout=3)
    text = log.read_text()
    for line in ("pico> pin 28 set high", "pico> pin 28 reset to hi-Z",
                 "uart> pico->esp [00]", "uart> pico->esp [ff]"):
        assert (line in text) == verbose, (line, verbose, text)
    assert not sock.exists(), "daemon failed to unlink its own socket"


def run():
    with tempfile.TemporaryDirectory(prefix="board-verbose-", dir=os.environ.get("TMPDIR")) as temp:
        exercise(False, temp)
        exercise(True, temp)
    print("PASS: physical GPIO transitions and binary UART dumps gated by verbose")


if __name__ == "__main__":
    run()
