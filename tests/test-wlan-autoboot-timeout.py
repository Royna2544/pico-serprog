#!/usr/bin/env python3
"""Autoboot without an ESP ack must fail closed instead of waiting forever."""
import os
from pathlib import Path
import pty
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def request(sock, text):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(2)
        peer.connect(str(sock))
        peer.sendall(text.encode())
        return peer.recv(128).decode().strip()


def run():
    with tempfile.TemporaryDirectory(prefix="wlan-ack-timeout-", dir=os.environ.get("TMPDIR")) as temp:
        sock = Path(temp) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(sock), "MOCK_PTY_DIR": temp}
        fds = []
        with (Path(temp) / "daemon.log").open("w+") as daemon_log, \
             (Path(temp) / "pico.log").open("w+") as pico_log:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)],
                                      env=env, stdout=daemon_log, stderr=subprocess.STDOUT)
            pico = None
            try:
                for _ in range(150):
                    if sock.exists():
                        break
                    assert daemon.poll() is None, "board daemon exited"
                    time.sleep(.02)
                else:
                    raise AssertionError("board daemon not ready")
                for name in ("uart0", "cdc0", "cdc1", "cdc2"):
                    master, slave = pty.openpty()
                    fds.extend((master, slave))
                    (Path(temp) / f"mock-{name}").symlink_to(os.ttyname(slave))
                pico = subprocess.Popen([str(BUILD / "pico_serprog")],
                                        env=env, stdout=pico_log, stderr=subprocess.STDOUT)
                try:
                    code = pico.wait(timeout=5)
                except subprocess.TimeoutExpired as exc:
                    pico_log.flush()
                    raise AssertionError("autoboot still waiting for absent ESP ack") from exc
                assert code != 0, "missing firmware ack must fail startup"
                assert request(sock, "MODE esp") == "off", "failure must disable ESP"
                assert "firmware ack timed out" in (Path(temp) / "pico.log").read_text()
                print("PASS: absent ESP ack fails Pico startup and powers ESP off")
            finally:
                if pico is not None and pico.poll() is None:
                    pico.terminate()
                    pico.wait(timeout=3)
                daemon.terminate()
                daemon.wait(timeout=3)
                for fd in fds:
                    os.close(fd)


if __name__ == "__main__":
    run()
