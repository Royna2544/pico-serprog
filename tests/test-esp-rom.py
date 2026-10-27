#!/usr/bin/env python3
"""Real daemon/DTB electrical and ROM boot-mode regression tests."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def command(sock, text):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(str(sock))
        client.sendall(text.encode())
        return client.recv(128).decode().strip()


def expect(sock, text, result):
    actual = command(sock, text)
    assert actual == result, (text, result, actual)


def boot(sock, gpio0, gpio2=1, gpio15=0, drive_gpio2=True):
    expect(sock, "GPIO pico 19 PUT 0", "OK")
    expect(sock, "GPIO pico 19 DIR 1", "OK")
    for pin, value in ((20, gpio0), (21, gpio2), (22, gpio15)):
        expect(sock, f"GPIO pico {pin} PUT {value}", "OK")
        expect(sock, f"GPIO pico {pin} DIR 1", "OK")
    if not drive_gpio2:
        expect(sock, "GPIO pico 21 DIR 0", "OK")
    expect(sock, "GPIO pico 18 PUT 1", "OK")
    expect(sock, "GPIO pico 18 DIR 1", "OK")
    expect(sock, "GPIO pico 19 PUT 1", "OK")


def run():
    with tempfile.TemporaryDirectory(prefix="esp-rom-", dir=os.environ.get("TMPDIR", str(ROOT))) as tmp:
        sock = Path(tmp) / "board.sock"
        daemon_log = open(Path(tmp) / "daemon.log", "w+b")
        daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                   str(BUILD / "board.dtb"), str(sock)],
                                  stdout=subprocess.DEVNULL, stderr=daemon_log)
        probe = None
        try:
            for _ in range(100):
                if sock.exists():
                    break
                if daemon.poll() is not None:
                    daemon_log.seek(0)
                    raise RuntimeError(daemon_log.read().decode())
                time.sleep(.01)
            else:
                raise RuntimeError("daemon socket unavailable")
            expect(sock, "MODE esp", "off")
            expect(sock, "GPIO esp 0 DIR 1", "ERR ESP sketch not running")
            expect(sock, "GPIO pico 20 GET", "1")
            boot(sock, 0, drive_gpio2=False)
            expect(sock, "MODE esp", "rom")
            expect(sock, "POWER esp", "0")  # sketch must not run in ROM mode
            expect(sock, "GPIO esp 2 FUNCTION", "uart")
            expect(sock, "GPIO esp 2 DRIVE", "uart-tx")
            expect(sock, "GPIO esp 2 MUX sio", "ERR ROM owns GPIO2")
            expect(sock, "GPIO esp 4 PUT 1", "ERR ESP sketch not running")
            expect(sock, "GPIO pico 21 GET", "1")  # ROM UART1 TX idle high
            daemon_log.seek(0)
            assert "ROM GPIO2 TX may contend" not in daemon_log.read().decode()
            expect(sock, "GPIO pico 21 DIR 1", "OK")
            daemon_log.seek(0)
            assert "ROM GPIO2 TX may contend" in daemon_log.read().decode()
            expect(sock, "GPIO pico 8 MUX uart", "OK")
            expect(sock, "UART pico TX 85", "OK")
            expect(sock, "UART pico RX", "-1")  # no ROM command interpreter
            expect(sock, "GPIO pico 20 PUT 1", "OK")
            expect(sock, "MODE esp", "rom")  # sampled only on reset edge
            expect(sock, "GPIO pico 21 PUT 0", "OK")
            expect(sock, "GPIO pico 21 GET", "ERR contention")
            expect(sock, "GPIO pico 21 PUT 1", "OK")
            expect(sock, "GPIO pico 21 GET", "1")
            expect(sock, "GPIO pico 21 DIR 0", "OK")
            expect(sock, "GPIO pico 19 PUT 0", "OK")
            expect(sock, "MODE esp", "off")
            boot(sock, 1)
            expect(sock, "MODE esp", "flash")
            expect(sock, "POWER esp", "1")
            expect(sock, "GPIO esp 2 FUNCTION", "sio")
            expect(sock, "GPIO esp 2 DRIVE", "input")
            expect(sock, "GPIO pico 8 MUX uart", "OK")
            expect(sock, "GPIO pico 9 MUX uart", "OK")
            expect(sock, "GPIO esp 3 GET", "1")  # Pico UART1 TX idle high
            expect(sock, "GPIO pico 9 GET", "1")  # ESP UART0 TX idle high
            expect(sock, "GPIO esp 1 DRIVE", "uart-tx")
            expect(sock, "GPIO pico 9 MUX sio", "OK")
            expect(sock, "GPIO pico 9 PUT 0", "OK")
            expect(sock, "GPIO pico 9 DIR 1", "OK")
            expect(sock, "GPIO pico 9 GET", "ERR contention")
            expect(sock, "GPIO pico 9 DIR 0", "OK")
            expect(sock, "GPIO pico 9 MUX uart", "OK")
            probe = subprocess.Popen([str(BUILD / "esp-restart-probe")],
                                     env={**os.environ, "BOARDSTATE_SOCKET": str(sock)},
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            for _ in range(100):
                if command(sock, "GPIO esp 4 DRIVE") == "sio-out":
                    break
                time.sleep(.001)
            else:
                raise AssertionError("ESP mock setup did not run")
            expect(sock, "GPIO esp 4 GET", "1")
            expect(sock, "GPIO pico 19 PUT 0", "OK")
            expect(sock, "GPIO pico 19 PUT 1", "OK")
            expect(sock, "GPIO esp 4 DRIVE", "input")
            for _ in range(200):
                if command(sock, "GPIO esp 4 DRIVE") == "sio-out":
                    break
                time.sleep(.002)
            else:
                raise AssertionError("ESP setup not rerun after quick reset")
            expect(sock, "GPIO esp 4 GET", "0")
            expect(sock, "GPIO pico 20 PUT 0", "OK")
            expect(sock, "MODE esp", "flash")
            expect(sock, "GPIO esp 0 PUT 1", "OK")
            expect(sock, "GPIO esp 0 DIR 1", "OK")
            expect(sock, "GPIO esp 0 DRIVE", "sio-out")
            expect(sock, "GPIO pico 8 MUX uart", "OK")
            expect(sock, "GPIO pico 9 MUX uart", "OK")
            expect(sock, "UART pico TX 65", "OK")
            expect(sock, "GPIO pico 19 PUT 0", "OK")
            expect(sock, "GPIO esp 0 DRIVE", "input")
            expect(sock, "UART esp RX", "-1")
            boot(sock, 1, gpio15=1)
            expect(sock, "MODE esp", "invalid")
            expect(sock, "POWER esp", "0")
            expect(sock, "GPIO pico 19 PUT 0", "OK")
            boot(sock, 1, gpio2=0)
            expect(sock, "MODE esp", "invalid")
            probe.terminate()
            probe.communicate(timeout=3)
            probe = None
            time.sleep(.05)
            daemon.terminate()
            daemon.wait(timeout=1)
            assert not sock.exists(), "daemon left its socket behind"
        finally:
            if probe is not None:
                if probe.poll() is None:
                    probe.terminate()
                try:
                    probe.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    probe.kill()
                    probe.communicate()
            if daemon.poll() is None:
                daemon.terminate()
            try:
                daemon.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.communicate()
            daemon_log.seek(0)
            log = daemon_log.read().decode()
            daemon_log.close()
        assert "Entered bootrom upload mode" in log, log
        assert "booting normal mode" in log, log
        assert "invalid boot straps" in log, log
        print("PASS: reset-latched straps, GPIO2 ROM TX, contention, mode logs")


if __name__ == "__main__":
    run()
