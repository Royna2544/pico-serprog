#!/usr/bin/env python3
"""Host integration: real DTB + both host firmware executables, no real USB/serial."""
import os
from pathlib import Path
import pty
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"
SCRATCH = Path(os.environ.get("TMPDIR", ROOT))


def request(path, text):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(str(path))
        client.sendall(text.encode())
        answer = client.recv(128).decode().strip()
    return answer


def expect(path, cmd, expected):
    actual = request(path, cmd)
    assert actual == expected, (cmd, expected, actual)


def run():
    with tempfile.TemporaryDirectory(prefix="boardstate-", dir=SCRATCH) as tmp:
        sock = Path(tmp) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(sock), "MOCK_PTY_DIR": tmp}
        processes = []
        ptys = []
        links = []
        pico_log = None
        daemon_log = open(Path(tmp) / "daemon.log", "w+b")
        try:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)], env=env,
                                      stdout=subprocess.DEVNULL, stderr=daemon_log)
            processes.append(daemon)
            for _ in range(100):
                if sock.exists():
                    break
                if daemon.poll() is not None:
                    daemon_log.seek(0)
                    raise RuntimeError(daemon_log.read().decode())
                time.sleep(.01)
            else:
                raise RuntimeError("boardstate socket not ready")
            subprocess.run([str(BUILD / "boardstate-pico-smoke"), "no-uart0"],
                           env=env, check=True, timeout=3)
            expect(sock, "POWER esp", "0")
            expect(sock, "GPIO pico 18 GET", "0")
            expect(sock, "GPIO pico 19 GET", "1")
            expect(sock, "GPIO pico 20 GET", "1")
            expect(sock, "GPIO pico 21 GET", "1")
            expect(sock, "GPIO pico 22 GET", "0")
            expect(sock, "GPIO pico 20 PULL 00", "OK")
            expect(sock, "GPIO pico 20 GET", "1")  # external resistor survives
            expect(sock, "GPIO pico 8 MUX uart", "OK")
            expect(sock, "GPIO pico 9 MUX uart", "OK")
            subprocess.run([str(BUILD / "boardstate-pico-smoke"), "writable-off"],
                           env=env, check=True, timeout=3)
            expect(sock, "UART pico TX 65", "OK")  # TX works with receiver off
            expect(sock, "UART esp RX", "-1")
            for pin, value in ((18, 1), (19, 1)):
                expect(sock, f"GPIO pico {pin} PUT {value}", "OK")
                expect(sock, f"GPIO pico {pin} DIR 1", "OK")
            expect(sock, "POWER esp", "1")
            ctl = subprocess.run(["python", str(ROOT / "boardstate-daemon" / "ctl.py"),
                                  "POWER", "esp"], env=env, capture_output=True,
                                 text=True, check=True, timeout=3)
            assert ctl.stdout.strip() == "1", ctl.stdout
            expect(sock, "GPIO esp 7 GET", "0")  # GPIO7 is not CHIP_EN
            expect(sock, "GPIO esp 16 GET", "0")  # GPIO16 is not reset
            expect(sock, "GPIO esp 0 PUT 0", "OK")
            expect(sock, "GPIO esp 0 DIR 1", "OK")
            expect(sock, "GPIO pico 20 PUT 1", "OK")
            expect(sock, "GPIO pico 20 DIR 1", "OK")
            expect(sock, "GPIO pico 20 GET", "ERR contention")
            expect(sock, "GPIO esp 0 DIR 0", "OK")
            expect(sock, "GPIO pico 20 DIR 0", "OK")
            esp = subprocess.Popen([str(BUILD / "esp8266-mock")], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            processes.append(esp)
            expect(sock, "UART pico TX 76", "OK")
            for _ in range(200):
                if esp.poll() is not None:
                    raise RuntimeError(esp.communicate()[1].decode())
                if request(sock, "UART pico RX") == "76":
                    break
                time.sleep(.01)
            else:
                raise AssertionError("ESP sketch did not echo Pico UART1 byte")
            subprocess.run([str(BUILD / "boardstate-pico-smoke")], env=env,
                           check=True, timeout=5)
            expect(sock, "GPIO pico 8 MUX sio", "OK")
            expect(sock, "UART pico TX 67", "ERR UART TX pin not muxed")
            expect(sock, "GPIO pico 8 MUX uart", "OK")
            expect(sock, "GPIO pico 19 PUT 0", "OK")
            expect(sock, "POWER esp", "0")
            expect(sock, "UART pico TX 66", "OK")
            expect(sock, "UART esp RX", "-1")
            for pin, value in ((18, 1), (19, 1), (20, 0), (21, 0), (22, 1)):
                expect(sock, f"GPIO pico {pin} PUT {value}", "OK")
                expect(sock, f"GPIO pico {pin} DIR 1", "OK")
            expect(sock, "GPIO pico 8 MUX sio", "OK")
            expect(sock, "GPIO pico 9 MUX sio", "OK")
            # Exercise the existing Pico executable against the same daemon.
            # Its USB CDC and UART0 mocks still use PTYs; never attach real hardware.
            for name in ("uart0", "cdc0", "cdc1", "cdc2"):
                master, slave = pty.openpty()
                ptys.extend((master, slave))
                link = Path(tmp) / f"mock-{name}"
                if link.exists() or link.is_symlink():
                    raise RuntimeError(f"refusing to replace {link}")
                link.symlink_to(os.ttyname(slave))
                links.append(link)
            pico_log = open(Path(tmp) / "pico.log", "w+b")
            pico = subprocess.Popen([str(BUILD / "pico_serprog")], env=env,
                                    stdout=pico_log, stderr=subprocess.STDOUT)
            processes.append(pico)
            for _ in range(200):
                if pico.poll() is not None:
                    raise RuntimeError(f"Pico exited: {pico.returncode}")
                if all(request(sock, f"GPIO pico {pin} GET") == str(value)
                       for pin, value in ((18, 0), (19, 0), (20, 1),
                                          (21, 1), (22, 0))) and all(
                        request(sock, f"GPIO pico {pin} FUNCTION") == "uart"
                        for pin in (8, 9)):
                    break
                time.sleep(.01)
            else:
                levels = {pin: request(sock, f"GPIO pico {pin} GET")
                          for pin in (18, 19, 20, 21, 22)}
                pico.terminate()
                pico.communicate(timeout=3)
                pico_log.seek(0)
                out = pico_log.read()
                raise AssertionError(f"Pico ESP boot GPIOs: {levels}; "
                                     f"stdout={out[-4000:]!r}")
            with open(Path(tmp) / "wlan.log", "w+b") as wlan_log:
                upload = subprocess.run([str(BUILD / "boardstate-wlan-host")],
                                        env=env, stdout=wlan_log,
                                        stderr=subprocess.STDOUT, timeout=10)
                if upload.returncode:
                    wlan_log.seek(0)
                    raise AssertionError(f"WLAN upload path failed: {wlan_log.read()[-4000:]!r}")
            expect(sock, "MODE esp", "rom")
            expect(sock, "GPIO pico 21 DRIVE", "sio-out")
            expect(sock, "GPIO esp 2 DRIVE", "uart-tx")
            expect(sock, "GPIO pico 21 GET", "1")
            daemon_log.seek(0)
            assert "ROM GPIO2 TX may contend" in daemon_log.read().decode()
            print("PASS: DTB wiring, pulls, contention, boot gating, ESP echo, Pico host startup")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.communicate(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate()
            for link in links:
                link.unlink()
            for fd in ptys:
                os.close(fd)
            if pico_log is not None:
                pico_log.close()
            daemon_log.close()


if __name__ == "__main__":
    run()
