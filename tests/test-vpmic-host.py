#!/usr/bin/env python3
"""Launch the actual vpmic.ino host runner against the DT-backed board."""
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
    with tempfile.TemporaryDirectory(prefix="vpmic-host-", dir=os.environ.get("TMPDIR")) as temp:
        sock = Path(temp) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(sock)}
        with (Path(temp) / "board.log").open("w+") as board_log, \
             (Path(temp) / "uno.log").open("w+") as uno_log:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)],
                                      stdout=board_log, stderr=subprocess.STDOUT, env=env)
            uno = None
            try:
                for _ in range(150):
                    if sock.exists():
                        break
                    assert daemon.poll() is None, "board daemon exited"
                    time.sleep(.02)
                else:
                    raise AssertionError("board daemon did not start")
                assert request(sock, "GPIO pico 6 GET") == "1"
                uno = subprocess.Popen([str(BUILD / "vpmic-mock")],
                                       stdout=uno_log, stderr=subprocess.STDOUT, env=env)
                for _ in range(150):
                    if request(sock, "GPIO pico 6 GET") == "0":
                        break
                    assert uno.poll() is None, "Uno sketch exited during setup"
                    time.sleep(.02)
                else:
                    raise AssertionError("Uno sketch did not pull Pico GP6 low")
                assert request(sock, "GPIO uno 2 DRIVE") == "sio-out"
                assert request(sock, "GPIO uno 2 GET") == "1"
                assert uno.poll() is None, "Uno sketch did not remain running"
                print("PASS: vpmic.ino drives NPN base, collector pulls GP6 low")
            finally:
                if uno is not None:
                    uno.terminate()
                    uno.wait(timeout=3)
                daemon.terminate()
                daemon.wait(timeout=3)


if __name__ == "__main__":
    run()
