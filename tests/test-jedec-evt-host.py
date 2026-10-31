#!/usr/bin/env python3
"""Early JEDEC event survives the real Pico handshake and waits for EVT:OK."""
import os
from pathlib import Path
import pty
import select
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"


def request(sock, text):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(2)
        peer.connect(str(sock))
        peer.sendall(text.encode())
        return peer.recv(128).decode().strip()


def frame(command, data):
    payload = f":{command}:{data}\n"
    digits = 1
    while True:
        total = len(payload) + 1 + digits
        new_digits = len(str(total))
        if digits == new_digits:
            return f":{total}:{command}:{data}\n"
        digits = new_digits


class ScriptedESP:
    def __init__(self, sock):
        self.sock = sock
        self.slave_seen = threading.Event()
        self.release_slave = threading.Event()
        self.event_seen = threading.Event()
        self.flash_event_seen = threading.Event()
        self.reject_first = threading.Event()
        self.release_ack = threading.Event()
        self.stop = threading.Event()
        self.events = []
        self.flash_events = []
        self.flash_rejections_remaining = 3
        self.commands = []
        self.failure = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def send(self, command, data):
        for byte in frame(command, data).encode("ascii"):
            result = request(self.sock, f"UART esp TX {byte}")
            assert result == "OK", result

    def run(self):
        try:
            while not self.stop.is_set() and request(self.sock, "MODE esp") != "flash":
                time.sleep(.002)
            if self.stop.is_set():
                return
            for cmd in ("GPIO esp 15 PUT 1", "GPIO esp 15 DIR 1"):
                assert request(self.sock, cmd) == "OK"
            buffer = bytearray()
            answers = {"SLAVE": ("MASTER", "ESP8266"),
                       "ADDR": ("ADDR", "00:00:00:00:00:00"),
                       "CONN?": ("CONN", "0"), "CONN": ("CONN", "OK"),
                       "SCAN": ("SCAN", "none"),
                       "IPADDR": ("IPADDR", "0.0.0.0"),
                       "RSSI": ("RSSI", "-50"), "SSID": ("SSID", "test"),
                       "SDTEST": ("SDTEST", "PASS"), "SRVIT": ("SRVIT", "OK")}
            while not self.stop.is_set():
                byte = int(request(self.sock, "UART esp RX"))
                if byte < 0:
                    time.sleep(.001)
                    continue
                buffer.append(byte)
                if byte != 10:
                    assert len(buffer) < 128, "Pico sent oversized frame"
                    continue
                parts = bytes(buffer).decode("ascii").strip().split(":", 3)
                assert len(parts) == 4 and parts[0] == "", "invalid Pico frame"
                assert int(parts[1]) == len(buffer), "Pico frame length mismatch"
                command, data = parts[2:]
                buffer.clear()
                self.commands.append(command)
                if command == "SLAVE":
                    self.slave_seen.set()
                    while not self.stop.is_set() and not self.release_slave.wait(.01):
                        pass
                if command == "EVT":
                    if data.startswith("F1,"):
                        self.flash_events.append(data)
                        self.flash_event_seen.set()
                        reject = self.flash_rejections_remaining > 0
                        if reject:
                            self.flash_rejections_remaining -= 1
                        self.send("EVT", "BAD" if reject else "OK")
                        continue
                    self.events.append(data)
                    self.event_seen.set()
                    if len(self.events) == 1:
                        while not self.stop.is_set() and not self.reject_first.wait(.01):
                            pass
                        if not self.stop.is_set():
                            self.send("EVT", "BAD")
                        continue
                    while not self.stop.is_set() and not self.release_ack.wait(.01):
                        pass
                    if not self.stop.is_set():
                        self.send("EVT", "OK")
                elif command in answers and not self.stop.is_set():
                    self.send(*answers[command])
                else:
                    raise AssertionError(f"unexpected command: {command}")
        except Exception as exc:
            if not self.stop.is_set():
                self.failure = exc
                self.slave_seen.set()
                self.event_seen.set()
                self.flash_event_seen.set()


def wait_event(event, responder, seconds, name):
    assert event.wait(seconds), f"timed out waiting for {name}; commands={responder.commands}"
    assert responder.failure is None, f"responder failed: {responder.failure}"


def read_exact(fd, count, seconds):
    data = bytearray()
    deadline = time.monotonic() + seconds
    while len(data) < count and time.monotonic() < deadline:
        if select.select([fd], [], [], .1)[0]:
            data.extend(os.read(fd, count - len(data)))
    return bytes(data)


def run():
    with tempfile.TemporaryDirectory(prefix="jedec-evt-", dir=os.environ.get("TMPDIR")) as temp:
        sock = Path(temp) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(sock), "MOCK_PTY_DIR": temp}
        fds = []
        with (Path(temp) / "daemon.log").open("w+") as daemon_log, \
             (Path(temp) / "pico.log").open("w+") as pico_log:
            daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
                                       str(BUILD / "board.dtb"), str(sock)],
                                      env=env, stdout=daemon_log, stderr=subprocess.STDOUT)
            pico = None
            responder = None
            try:
                for _ in range(150):
                    if sock.exists():
                        break
                    assert daemon.poll() is None, "board daemon exited"
                    time.sleep(.02)
                else:
                    raise AssertionError("board daemon not ready")
                cdc0 = -1
                for name in ("uart0", "cdc0", "cdc1", "cdc2"):
                    master, slave = pty.openpty()
                    fds.extend((master, slave))
                    (Path(temp) / f"mock-{name}").symlink_to(os.ttyname(slave))
                    if name == "cdc0":
                        cdc0 = master
                responder = ScriptedESP(sock)
                responder.start()
                pico = subprocess.Popen([str(BUILD / "pico_serprog")],
                                        env=env, stdout=pico_log, stderr=subprocess.STDOUT)
                wait_event(responder.slave_seen, responder, 5, "initial SLAVE")
                os.write(cdc0, b"\x13\x01\x00\x00\x03\x00\x00\x9f")
                assert read_exact(cdc0, 4, 4) == b"\x06\xef\x40\x18", "JEDEC transfer failed"
                responder.release_slave.set()
                wait_event(responder.event_seen, responder, 8, "early EVT")
                assert len(responder.events) == 1 and responder.events[0].startswith(
                    "JEDEC_ID: EF 40 18 "), responder.events
                os.write(cdc0, b"\x13\x01\x00\x00\x03\x00\x00\x9f")
                assert read_exact(cdc0, 4, 4) == b"\x06\xef\x40\x18", "second JEDEC failed"
                time.sleep(.3)
                assert request(sock, "UART esp RX") == "-1", \
                    "second EVT was emitted before first EVT:OK"
                responder.reject_first.set()
                deadline = time.monotonic() + 3
                while len(responder.events) < 2:
                    assert time.monotonic() < deadline, "first EVT not retried after bad ACK"
                    time.sleep(.02)
                assert responder.events[0] == responder.events[1], \
                    "bad ACK skipped the pending event"
                assert request(sock, "UART esp RX") == "-1", \
                    "next event emitted before retried event was acknowledged"
                responder.release_ack.set()
                deadline = time.monotonic() + 3
                while len(responder.events) < 3:
                    assert time.monotonic() < deadline, "next EVT did not follow retry ACK"
                    time.sleep(.02)
                assert responder.events[2].startswith("JEDEC_ID: EF 40 18 "), responder.events
                wait_event(responder.flash_event_seen, responder, 5,
                           "unsupported-chip flash telemetry")
                sample = responder.flash_events[0].split(",")
                assert len(sample) == 10 and sample[0] == "F1" and sample[3] == "UNSUP", sample
                assert int(sample[1]) > 0 and int(sample[2]) > 0, sample
                assert int(sample[4]) == int("ef4018", 16), sample
                assert sample[6:9] == ["0", "0", "0"], sample
                os.write(cdc0, b"\x00")
                assert read_exact(cdc0, 1, 2) == b"\x06", \
                    "telemetry rejection blocked serprog CDC0"
                deadline = time.monotonic() + 8
                while len(responder.flash_events) < 4:
                    assert pico.poll() is None, "telemetry retry failure stopped the Pico"
                    assert responder.failure is None, responder.failure
                    assert time.monotonic() < deadline, "F1 was not retried after three BAD ACKs"
                    time.sleep(.02)
                assert responder.flash_events[:4] == [responder.flash_events[0]] * 4, \
                    "F1 retry changed its payload or sequence"
                deadline = time.monotonic() + 4
                while not any(",IDLE," in data for data in responder.flash_events):
                    assert pico.poll() is None, "idle telemetry stopped the Pico"
                    assert responder.failure is None, responder.failure
                    assert time.monotonic() < deadline, "idle SPI activity was never reported"
                    time.sleep(.02)
                time.sleep(1.8)
                assert sum(",IDLE," in data for data in responder.flash_events) == 1, \
                    "idle activity was reported repeatedly without another SPI sample"
                pico.send_signal(signal.SIGINT)  # Flush mock diagnostics on exit.
                pico.wait(timeout=3)
                assert (Path(temp) / "pico.log").read_text().count(
                    "Sent event to slave device: JEDEC_ID") == 2, "JEDEC ACKs not consumed"
                print("PASS: early JEDEC events retained and unsupported-chip F1 delivered")
            finally:
                if responder is not None:
                    responder.stop.set()
                    responder.release_slave.set()
                    responder.reject_first.set()
                    responder.release_ack.set()
                    responder.thread.join(timeout=3)
                if pico is not None:
                    if pico.poll() is None:
                        pico.terminate()
                        pico.wait(timeout=3)
                daemon.terminate()
                daemon.wait(timeout=3)
                for fd in fds:
                    os.close(fd)


if __name__ == "__main__":
    run()
