#!/usr/bin/env python3
"""Exercise the real WLAN boot API across a repeated NORMAL transition."""
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
    with tempfile.TemporaryDirectory(prefix="wlan-reentry-", dir=os.environ.get("TMPDIR")) as temp:
        sock = Path(temp) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(sock), "MOCK_PTY_DIR": temp}
        with (Path(temp) / "daemon.log").open("w+") as daemon_log, \
             (Path(temp) / "esp.log").open("w+") as esp_log:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)],
                                      stdout=daemon_log, stderr=subprocess.STDOUT, env=env)
            esp = None
            try:
                for _ in range(150):
                    if sock.exists():
                        break
                    assert daemon.poll() is None, "board daemon exited"
                    time.sleep(.02)
                else:
                    raise AssertionError("board daemon not ready")
                esp = subprocess.Popen([str(BUILD / "esp8266-mock")], env=env,
                                       stdout=esp_log, stderr=subprocess.STDOUT)
                for case in ("invalid", "reenter-normal"):
                    assert request(sock, "RESET pico") == "OK"
                    with (Path(temp) / f"{case}.log").open("w+") as log:
                        result = subprocess.run([str(BUILD / "boardstate-wlan-host"), case],
                                                env=env, stdout=log,
                                                stderr=subprocess.STDOUT, timeout=8)
                    assert result.returncode == 0, (case, (Path(temp) / f"{case}.log").read_text()[-4000:])
                    assert request(sock, "MODE esp") == "flash", case
                print("PASS: invalid transition reports error; NORMAL->NONE->NORMAL re-arms IRQ")
            finally:
                if esp is not None:
                    esp.terminate()
                    esp.wait(timeout=3)
                daemon.terminate()
                daemon.wait(timeout=3)


if __name__ == "__main__":
    run()
