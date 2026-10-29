#!/usr/bin/env python3
"""Exercise the Uno power-good GPIO through the real DT-backed board model."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def request(sock, command):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(2)
        peer.connect(str(sock))
        peer.sendall(command.encode())
        return peer.recv(128).decode().strip()


def run():
    with tempfile.TemporaryDirectory(prefix="uno-board-", dir=os.environ.get("TMPDIR")) as temp:
        sock = Path(temp) / "board.sock"
        with (Path(temp) / "daemon.log").open("w+") as output:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)],
                                      stdout=output, stderr=subprocess.STDOUT)
            try:
                for _ in range(150):
                    if sock.exists():
                        break
                    assert daemon.poll() is None, "board daemon exited"
                    time.sleep(.02)
                else:
                    raise AssertionError("board daemon did not start")
                assert request(sock, "GPIO pico 6 GET") == "1"  # 3.3 V collector pull-up
                assert request(sock, "GPIO uno 2 GET") == "0"  # base pulldown
                assert request(sock, "GPIO pico 6 IRQ_ENABLE 4") == "OK"
                assert request(sock, "GPIO uno 2 PUT 1") == "OK"
                assert request(sock, "GPIO pico 6 GET") == "1"  # input cannot turn NPN on
                assert request(sock, "GPIO uno 2 DIR 1") == "OK"
                assert request(sock, "GPIO uno 2 GET") == "1"
                assert request(sock, "GPIO pico 6 GET") == "0"
                assert request(sock, "GPIO pico 6 IRQ_STATUS") == "4"
                assert request(sock, "GPIO pico 6 PUT 1") == "OK"
                assert request(sock, "GPIO pico 6 DIR 1") == "OK"
                assert request(sock, "GPIO pico 6 GET") == "ERR contention"
                assert request(sock, "RESET pico") == "OK"
                assert request(sock, "GPIO pico 6 GET") == "0", "NPN must survive Pico reset"
                assert request(sock, "GPIO uno 2 DIR 0") == "OK"
                assert request(sock, "GPIO pico 6 GET") == "1"
                assert request(sock, "GPIO uno 2 GET") == "0"
                assert request(sock, "GPIO uno 3 PUT 1").startswith("ERR")
                print("PASS: Uno D2 NPN collector -> GP6, pull-ups, IRQ, contention, reset")
            finally:
                daemon.terminate()
                daemon.wait(timeout=3)


if __name__ == "__main__":
    run()
