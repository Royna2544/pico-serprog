#!/usr/bin/env python3
"""Pico host relaunch resets Pico GPIOs and the connected ESP mock."""
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
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(str(sock))
        client.sendall(text.encode())
        return client.recv(128).decode().strip()


def expect(sock, text, wanted):
    actual = request(sock, text)
    assert actual == wanted, (text, wanted, actual)


def boot(sock):
    for pin, value in ((20, 1), (21, 1), (22, 0), (19, 1), (18, 1)):
        expect(sock, f"GPIO pico {pin} PUT {value}", "OK")
        expect(sock, f"GPIO pico {pin} DIR 1", "OK")
    expect(sock, "MODE esp", "flash")


def wait_for(sock, text, wanted, process=None):
    for _ in range(200):
        if request(sock, text) == wanted:
            return
        if process is not None and process.poll() is not None:
            raise AssertionError(f"process exited before {text} became {wanted}")
        time.sleep(.005)
    raise AssertionError(f"{text} never became {wanted}: {request(sock, text)}")


def run():
    with tempfile.TemporaryDirectory(prefix="board-reset-", dir=os.environ.get("TMPDIR", str(ROOT))) as tmp:
        sock = Path(tmp) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(sock), "MOCK_PTY_DIR": tmp}
        processes = []
        fds = []
        with open(Path(tmp) / "daemon.log", "w+b") as daemon_log, \
             open(Path(tmp) / "pico.log", "w+b") as pico_log:
            try:
                daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                           str(BUILD / "board.dtb"), str(sock)],
                                          env=env, stdout=subprocess.DEVNULL,
                                          stderr=daemon_log)
                processes.append(daemon)
                for _ in range(100):
                    if sock.exists():
                        break
                    if daemon.poll() is not None:
                        daemon_log.seek(0)
                        raise RuntimeError(daemon_log.read().decode())
                    time.sleep(.01)
                else:
                    raise RuntimeError("daemon socket unavailable")
                expect(sock, "GPIO pico 8 MUX uart", "OK")
                expect(sock, "GPIO pico 9 MUX uart", "OK")
                boot(sock)
                probe = subprocess.Popen([str(BUILD / "esp-restart-probe")],
                                         env=env, stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
                processes.append(probe)
                wait_for(sock, "GPIO esp 4 DRIVE", "sio-out", probe)
                expect(sock, "GPIO esp 4 GET", "1")
                expect(sock, "GPIO pico 24 PUT 1", "OK")
                expect(sock, "GPIO pico 24 DIR 1", "OK")
                expect(sock, "GPIO pico 24 DRIVE", "sio-out")
                generation = int(request(sock, "BOOT esp"))

                expect(sock, "RESET pico", "OK")
                expect(sock, "MODE esp", "off")
                expect(sock, "GPIO esp 4 DRIVE", "input")
                expect(sock, "GPIO pico 24 DRIVE", "input")
                expect(sock, "GPIO pico 8 FUNCTION", "sio")
                expect(sock, "BOOT esp", str(generation))
                boot(sock)
                expect(sock, "BOOT esp", str(generation + 1))
                wait_for(sock, "GPIO esp 4 DRIVE", "sio-out", probe)
                expect(sock, "GPIO esp 4 GET", "0")

                expect(sock, "GPIO pico 24 PUT 1", "OK")
                expect(sock, "GPIO pico 24 DIR 1", "OK")
                for name in ("uart0", "cdc0", "cdc1", "cdc2"):
                    master, slave = pty.openpty()
                    fds.extend((master, slave))
                    link = Path(tmp) / f"mock-{name}"
                    link.symlink_to(os.ttyname(slave))
                pico = subprocess.Popen([str(BUILD / "pico_serprog")], env=env,
                                        stdout=pico_log, stderr=subprocess.STDOUT)
                processes.append(pico)
                wait_for(sock, "GPIO pico 24 DRIVE", "input")
                if pico.poll() is None:
                    pico.terminate()
                pico.wait(timeout=3)

                boot(sock)
                wait_for(sock, "GPIO esp 4 DRIVE", "sio-out", probe)
                expect(sock, "GPIO pico 24 PUT 1", "OK")
                expect(sock, "GPIO pico 24 DIR 1", "OK")
                pico = subprocess.Popen([str(BUILD / "pico_serprog")], env=env,
                                        stdout=pico_log, stderr=subprocess.STDOUT)
                processes.append(pico)
                wait_for(sock, "GPIO pico 24 DRIVE", "input")
                daemon_log.flush()
                daemon_log.seek(0)
                log = daemon_log.read()
                assert log.count(b"Pico board reset") == 3
                assert log.count(b"ESP reset/disabled") >= 3
                print("PASS: Pico reset clears GPIOs and restarts ESP; host relaunch signals reset")
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.communicate(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate()
                for fd in fds:
                    os.close(fd)


if __name__ == "__main__":
    run()
