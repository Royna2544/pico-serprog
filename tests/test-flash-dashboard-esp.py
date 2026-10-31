#!/usr/bin/env python3
"""Exercise the real ESP sketch over DT-backed UART and loopback TCP."""
import http.client
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("ESP_MOCK_BUILD", str(ROOT / "build-host")))


def board(sock, command):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(2)
        peer.connect(str(sock))
        peer.sendall(command.encode("ascii"))
        return peer.recv(128).decode("ascii").strip()


def frame(command, data):
    body = f":{command}:{data}\n"
    digits = 1
    while True:
        size = len(body) + 1 + digits
        if len(str(size)) == digits:
            return f":{size}{body}"
        digits = len(str(size))


def free_pair():
    for _ in range(50):
        with socket.socket() as first:
            first.bind(("127.0.0.1", 0))
            port = first.getsockname()[1]
            if not 1024 <= port <= 65534:
                continue
            with socket.socket() as second:
                try:
                    second.bind(("127.0.0.1", port + 1))
                except OSError:
                    continue
            return port
    raise RuntimeError("no free consecutive TCP ports")


class DashboardSketch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="flash-esp-", dir=os.environ.get("TMPDIR"))
        cls.sock = Path(cls.temp.name) / "board.sock"
        env = {**os.environ, "BOARDSTATE_SOCKET": str(cls.sock), "MOCK_PTY_DIR": cls.temp.name}
        cls.log = open(Path(cls.temp.name) / "process.log", "w+b")
        cls.daemon = subprocess.Popen([str(BUILD / "boardstate-daemon"),
            str(BUILD / "board.dtb"), str(cls.sock)], env=env,
            stdout=cls.log, stderr=subprocess.STDOUT)
        cls.esp = None
        try:
            for _ in range(150):
                if cls.sock.exists():
                    break
                if cls.daemon.poll() is not None:
                    raise RuntimeError("boardstate daemon exited")
                time.sleep(.01)
            else:
                raise RuntimeError("boardstate socket did not appear")
            for pin in (8, 9):
                cls.expect(f"GPIO pico {pin} MUX uart", "OK")
            cls.esp = subprocess.Popen([str(BUILD / "esp8266-mock")], env=env,
                stdout=cls.log, stderr=subprocess.STDOUT)
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        for proc in (getattr(cls, "esp", None), getattr(cls, "daemon", None)):
            if proc is not None:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
        if hasattr(cls, "log"):
            cls.log.close()
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    @classmethod
    def expect(cls, command, wanted):
        actual = board(cls.sock, command)
        if actual != wanted:
            raise AssertionError((command, wanted, actual))

    def setUp(self):
        self.port = free_pair()
        self.expect("GPIO pico 18 PUT 0", "OK")
        self.expect("GPIO pico 18 DIR 1", "OK")
        for pin, level in ((20, 1), (21, 1), (22, 0), (19, 1), (18, 1)):
            self.expect(f"GPIO pico {pin} PUT {level}", "OK")
            self.expect(f"GPIO pico {pin} DIR 1", "OK")
        self.expect("MODE esp", "flash")
        for _ in range(200):
            if board(self.sock, "GPIO esp 15 DRIVE") == "sio-out":
                break
            assert self.esp is not None
            self.assertIsNone(self.esp.poll(), "ESP exited before sketch setup")
            time.sleep(.005)
        else:
            self.fail("sketch did not boot")
        self.send("SRVIT", str(self.port), "OK")

    def tearDown(self):
        self.expect("GPIO pico 18 PUT 0", "OK")

    def send(self, command, data, expected="OK", timeout=2, response_command=None):
        for value in frame(command, data).encode("ascii"):
            self.expect(f"UART pico TX {value}", "OK")
        received = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = int(board(self.sock, "UART pico RX"))
            if value == -1:
                time.sleep(.002)
                continue
            received.append(value)
            if value == 10:
                self.assertLess(len(received), 128, received)
                reply = received.decode("ascii")
                self.assertEqual(reply, frame(response_command or command, expected))
                return
        self.fail(f"no {command} reply: {received!r}")

    def http(self, path, method="GET"):
        client = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        try:
            client.request(method, path)
            response = client.getresponse()
            return response.status, response.read()
        finally:
            client.close()

    def test_dashboard_page_after_srvit(self):
        status, page = self.http("/")
        self.assertEqual(status, 200)
        self.assertIn(b"SPI NOR activity", page)
        self.assertIn(b"EventSource", page)
        self.assertIn(b'<section id="coverage" hidden>', page)
        self.assertEqual(self.http("/ping"), (200, b"Hello from ESP8266!"))

    def test_sse_replays_id_and_pushes_updates_without_blocking_ping(self):
        ident = "F1,7,1,ID,12722199,0,0,0,0,0"
        erase = "F1,7,2,SE,4096,4096,1,0,0,0"
        self.send("EVT", ident)
        self.send("EVT", erase)
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(2)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            wire = self.read_until(stream, erase.encode())
            self.assertIn(b"HTTP/1.1 200 OK\r\n", wire)
            self.assertIn(b"Content-Type: text/event-stream\r\n", wire)
            self.assertIn(b"Access-Control-Allow-Origin: *\r\n", wire)
            self.assertIn(b"data: " + ident.encode() + b"\n\n", wire)
            self.assertIn(b"data: " + erase.encode() + b"\n\n", wire)
            self.assertLess(wire.index(ident.encode()), wire.index(erase.encode()))
            self.assertEqual(self.http("/ping"), (200, b"Hello from ESP8266!"))
            live = "F1,7,3,PP,256,2,1,1,0,0"
            self.send("EVT", live)
            self.assertIn(b"data: " + live.encode() + b"\n\n",
                          self.read_until(stream, live.encode()))

    @staticmethod
    def read_until(stream, marker, timeout=3):
        received = bytearray()
        deadline = time.monotonic() + timeout
        while marker not in received and time.monotonic() < deadline:
            try:
                chunk = stream.recv(2048)
            except socket.timeout:
                continue
            if not chunk:
                break
            received.extend(chunk)
        assert marker in received, (marker, received)
        return bytes(received)

    def test_ring_replay_is_bounded_and_dedupes_retries(self):
        ident = "F1,4,1,ID,12722199,0,0,0,0,0"
        self.send("EVT", ident)
        for sequence in range(2, 22):
            kind = "SE" if sequence % 2 == 0 else "BE32"
            length = 4096 if kind == "SE" else 32768
            self.send("EVT", f"F1,4,{sequence},{kind},0,{length},{sequence},0,0,0")
        self.send("EVT", "F1,4,21,BE32,0,32768,21,0,0,0")
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(.5)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            wire = self.read_until(stream, b"id: 4-21\nevent: flash", 3)
            self.assertIn(b"id: 4-1\nevent: flash", wire)  # ID pinned beyond ring
            self.assertNotIn(b"id: 4-2\nevent: flash", wire)
            self.assertNotIn(b"id: 4-5\nevent: flash", wire)
            self.assertEqual(wire.count(b"event: flash\n"), 17)
            self.assertEqual(self.http("/ping")[0], 200)

    def test_unsupported_id_resets_profile_and_history(self):
        self.send("EVT", "F1,2,1,ID,12722199,0,0,0,0,0")
        self.send("EVT", "F1,2,2,PP,256,1,0,1,0,0")
        unknown = "F1,3,1,UNSUP,15679512,0,0,0,0,0"
        self.send("EVT", unknown)
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(.5)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            wire = self.read_until(stream, b"data: " + unknown.encode() + b"\n\n")
            self.assertIn(b"id: 3-1", wire)
            self.assertNotIn(b"12722199", wire)
            self.assertNotIn(b"0,1,0,0", wire)

    def test_slow_header_cannot_delay_uart_or_http(self):
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as drip:
            drip.settimeout(2)
            drip.sendall(b"GET /events HTTP/1.1\r\nHost: ")
            start = time.monotonic()
            self.send("EVT", "F1,9,1,ID,12722199,0,0,0,0,0")
            self.assertLess(time.monotonic() - start, 1)
            self.assertEqual(self.http("/ping")[0], 200)
            time.sleep(1.1)
            try:
                self.assertEqual(drip.recv(512), b"")
            except ConnectionResetError:
                pass
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(2)
            stream.sendall(b"GET /wrong HTTP/1.1\r\nHost: localhost\r\n\r\n")
            self.assertIn(b"HTTP/1.1 404 Not Found", self.read_until(stream, b"404 Not Found"))

    def test_srvsp_closes_both_ports_and_restarts(self):
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(2)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            self.read_until(stream, b"retry: 1000\n\n")
            self.send("SRVSP", "", "OK")
            with self.assertRaises(ConnectionRefusedError):
                socket.create_connection(("127.0.0.1", self.port + 1), timeout=.3)
            with self.assertRaises(ConnectionRefusedError):
                socket.create_connection(("127.0.0.1", self.port), timeout=.3)
            self.send("SRVIT", str(self.port), "OK")
            self.assertEqual(self.http("/ping")[0], 200)

    def test_reset_drops_old_socket_and_partial_uart_frame(self):
        self.send("EVT", "F1,5,1,ID,12722199,0,0,0,0,0")
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as old:
            old.settimeout(2)
            old.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            self.read_until(old, b"data: F1,5,1,ID,12722199,0,0,0,0,0\n\n")
            for value in b":18:EVT:F1,1234":
                self.expect(f"UART pico TX {value}", "OK")
            self.expect("GPIO pico 19 PUT 0", "OK")
            self.expect("GPIO pico 19 PUT 1", "OK")
            for _ in range(200):
                if board(self.sock, "GPIO esp 15 DRIVE") == "sio-out":
                    break
                time.sleep(.005)
            else:
                self.fail("ESP sketch did not restart")
            self.send("PING", "", "", response_command="PONG")
            try:
                self.assertEqual(old.recv(512), b"", "old SSE socket stayed open")
            except ConnectionResetError:
                pass
            self.send("SRVIT", str(self.port), "OK")
            with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as fresh:
                fresh.settimeout(2)
                fresh.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
                wire = self.read_until(fresh, b"retry: 1000\n\n")
                self.assertNotIn(b"id: 5-1", wire)

    def test_invalid_ports_are_rejected_without_stopping_existing_http(self):
        for port in ("", "0", "1023", "65535", "99999", "-1", "not-a-port"):
            with self.subTest(port=port):
                self.send("SRVIT", port, "ERROR")
        self.assertEqual(self.http("/ping"), (200, b"Hello from ESP8266!"))

    def test_browser_sized_sse_headers_are_bounded_but_accepted(self):
        # Chromium includes Origin, Referer, UA and Accept in addition to Host.
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(2)
            request = (b"GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                       b"Origin: http://127.0.0.1\r\nReferer: http://127.0.0.1/\r\n"
                       b"Accept: text/event-stream\r\nUser-Agent: Mozilla/5.0 " +
                       b"x" * 240 + b"\r\n\r\n")
            self.assertGreater(len(request), 256)
            self.assertLess(len(request), 768)
            stream.sendall(request)
            self.assertIn(b"HTTP/1.1 200 OK", self.read_until(stream, b"retry: 1000\n\n"))

    def test_second_viewer_waits_until_first_disconnects(self):
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as first:
            first.settimeout(2)
            first.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            self.read_until(first, b"retry: 1000\n\n")
            with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as second:
                second.settimeout(.1)
                second.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
                with self.assertRaises(socket.timeout):
                    second.recv(1)  # Not replaced, dropped, or served concurrently.
                first.shutdown(socket.SHUT_RDWR)
                self.assertIn(b"HTTP/1.1 200 OK", self.read_until(second, b"HTTP/1.1 200 OK"))

    def test_stalled_stream_reader_does_not_delay_uart_ack(self):
        self.send("EVT", "F1,6,1,ID,12722199,0,0,0,0,0")
        with socket.socket() as stream:
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256)
            stream.settimeout(2)
            stream.connect(("127.0.0.1", self.port + 1))
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            # Never read the SSE stream; keep filling its small receive window.
            for sequence in range(2, 82):
                kind = "SE" if sequence % 2 == 0 else "BE32"
                length = 4096 if kind == "SE" else 32768
                start = time.monotonic()
                self.send("EVT", f"F1,6,{sequence},{kind},0,{length},{sequence},0,0,0")
                self.assertLess(time.monotonic() - start, 1)
            self.assertEqual(self.http("/ping")[0], 200)

    def test_restarted_session_sequence_one_discards_previous_milestones(self):
        self.send("EVT", "F1,8,1,ID,12722199,0,0,0,0,0")
        self.send("EVT", "F1,8,2,SE,4096,4096,1,0,0,0")
        self.send("EVT", "F1,8,1,ID,12722199,0,0,0,0,0")
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(2)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            wire = self.read_until(stream, b"data: F1,8,1,ID,12722199,0,0,0,0,0\n\n")
            self.assertNotIn(b"SE,4096", wire)
            self.assertEqual(wire.count(b"event: flash\n"), 1)

    def test_restarted_unsupported_chip_sequence_one_clears_old_history(self):
        unknown = "F1,8,1,UNSUP,15679512,0,0,0,0,0"
        self.send("EVT", unknown)
        self.send("EVT", "F1,8,2,IDLE,0,0,0,0,0,0")
        self.send("EVT", unknown)
        with socket.create_connection(("127.0.0.1", self.port + 1), timeout=2) as stream:
            stream.settimeout(2)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            wire = self.read_until(stream, b"data: " + unknown.encode() + b"\n\n")
            stream.settimeout(.1)
            deadline = time.monotonic() + .5
            while time.monotonic() < deadline:
                try:
                    wire += stream.recv(2048)
                except socket.timeout:
                    continue
            self.assertNotIn(b"IDLE", wire)
            self.assertEqual(wire.count(b"event: flash\n"), 1)

    def test_oversized_uart_frame_is_discarded_before_next_command(self):
        for value in b":" + b"9" * 130 + b"\n":
            self.expect(f"UART pico TX {value}", "OK")
        self.send("PING", "", "", response_command="PONG")

    def test_strict_evt_validation_and_legacy(self):
        self.send("EVT", "JEDEC_ID: EF 40 18 ", "OK")
        self.send("EVT", "F1,7,1,ID,12722199,0,0,0,0,0", "OK")
        for bad in (
            "F2,7,2,PP,256,2,0,1,0,0",  # unknown protocol version
            "F1,7,2,PP,256,2,0,1,0,0,0",  # extra field
            "F1,7,2,PP,256,2,0,1,0",      # missing field
            "F1,7,2,PP,256,,0,1,0,0",     # empty field
            "F1,7,2,PP,256,2,0,1,0,0 ",  # whitespace
            "F1,+7,2,PP,256,2,0,1,0,0",  # signs
            "F1,7,2,PP,256,2,0,1,0,4294967296",  # overflow
            "F1,0,2,PP,256,2,0,1,0,0",            # zero session
            "F1,7,0,PP,256,2,0,1,0,0",            # zero sequence
            "F1,7,2,PP,256,2,2049,1,0,0",        # sector count
            "F1,7,2,RD,256,2,0,1,32769,0",       # read page count
            "F1,7,2,PP,8388607,2,0,1,0,0",       # over chip boundary
            "F1,7,2,PP,256,2,0,32769,0,0",       # counter bound
            "F1,7,2,PP,256,2,0,1,0,0:bad",       # embedded colon
            "F1,7,2,<script>,256,2,0,1,0,0",     # no HTML/JS strings
            "F1,7,2,ID,15679512,0,0,0,0,0",      # Winbond cannot be ID
        ):
            with self.subTest(bad=bad):
                self.send("EVT", bad, "BAD")
        self.send("EVT", "F1,7,2,PP,256,2,0,1,0,0", "OK")
        self.send("EVT", "F1,8,1,UNSUP,15679512,0,0,0,0,0", "OK")


if __name__ == "__main__":
    unittest.main(verbosity=2)
