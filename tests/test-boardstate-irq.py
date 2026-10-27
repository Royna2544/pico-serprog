#!/usr/bin/env python3
"""Board electrical GPIO interrupt latches and masks over the real socket."""
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
        return client.recv(128).decode().strip()


def expect(path, text, wanted):
    actual = request(path, text)
    assert actual == wanted, (text, wanted, actual)


def run():
    with tempfile.TemporaryDirectory(prefix="board-irq-", dir=os.environ.get("TMPDIR", str(ROOT))) as tmp:
        sock = Path(tmp) / "board.sock"
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
                    raise RuntimeError("boardstate socket not ready")

                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 4", "OK")
                expect(sock, "IRQ pico PENDING", "0")
                for pin in (18, 19):
                    expect(sock, f"GPIO pico {pin} PUT 1", "OK")
                    expect(sock, f"GPIO pico {pin} DIR 1", "OK")
                expect(sock, "MODE esp", "flash")
                expect(sock, "GPIO esp 0 PUT 0", "OK")
                expect(sock, "GPIO esp 0 DIR 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "4")
                expect(sock, "IRQ pico PENDING", "1")
                expect(sock, "GPIO pico 20 IRQ_ACK 4", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 8", "OK")
                expect(sock, "GPIO esp 0 DIR 0", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "8")
                expect(sock, "GPIO pico 20 IRQ_ACK 8", "OK")
                expect(sock, "GPIO pico 20 IRQ_DISABLE 8", "OK")
                expect(sock, "GPIO esp 0 DIR 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "4")
                expect(sock, "GPIO pico 20 IRQ_DISABLE 4", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")
                expect(sock, "GPIO esp 0 DIR 0", "OK")
                expect(sock, "GPIO esp 0 DIR 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 4", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")  # stale edge cleared
                expect(sock, "GPIO esp 0 DIR 0", "OK")
                expect(sock, "GPIO esp 0 DIR 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "4")
                expect(sock, "GPIO pico 20 IRQ_ACK 4", "OK")
                expect(sock, "GPIO pico 20 IRQ_DISABLE 4", "OK")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "1")
                expect(sock, "GPIO pico 20 IRQ_ACK 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "1")  # level reasserts
                expect(sock, "GPIO esp 0 DIR 0", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 2", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "2")
                expect(sock, "GPIO pico 20 IRQ_ACK 2", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "2")
                expect(sock, "GPIO pico 20 IRQ_DISABLE 2", "OK")
                expect(sock, "IRQ pico PENDING", "0")
                expect(sock, "GPIO pico 20 IRQ_DISABLE 1", "OK")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 16", "ERR value")
                expect(sock, "GPIO esp 0 IRQ_STATUS", "ERR operation")
                expect(sock, "GPIO pico 20 IRQ_ENABLE 4", "OK")
                expect(sock, "GPIO pico 20 PUT 1", "OK")
                expect(sock, "GPIO pico 20 DIR 1", "OK")
                expect(sock, "GPIO esp 0 DIR 1", "OK")
                expect(sock, "GPIO pico 20 GET", "ERR contention")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")
                expect(sock, "GPIO pico 20 DIR 0", "OK")
                expect(sock, "GPIO pico 20 IRQ_STATUS", "0")  # no edge through contention
                expect(sock, "GPIO esp 0 DIR 0", "OK")
                expect(sock, "GPIO pico 20 IRQ_DISABLE 4", "OK")
                for mode in ("raw", "default"):
                    subprocess.run([str(BUILD / "boardstate-pico-irq"), mode],
                                   env={**os.environ, "BOARDSTATE_SOCKET": str(sock)},
                                   check=True, timeout=5)
                print("PASS: GPIO edge latch, mask, ACK, level and cross-board wiring")
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                daemon.wait(timeout=3)


if __name__ == "__main__":
    run()
