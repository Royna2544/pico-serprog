#!/usr/bin/env python3
"""Real host flashrom probe -> serprog -> Pico UART1 -> ESP SSE, no hardware."""
import http.client
import importlib.util
from contextlib import closing
import os
from pathlib import Path
import socket
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host"
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("nor_host", ROOT / "tests/testfakenorflash-host.py")
assert spec is not None and spec.loader is not None
nor_host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nor_host)


def diagnostics(log_path):
    lines = log_path.read_text(errors="replace").splitlines() if log_path.exists() else []
    return [line for line in lines if "unified:" in line or "WLAN TALK:" in line or
            "Initialization failed:" in line or "Slave device Server:" in line or
            "ERROR: AddressSanitizer" in line or "runtime error:" in line or
            ("pico_serprog>" in line and ("<3>" in line or "<4>" in line))][-20:]


def wait_for_http(port, seconds, host, log_path):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if host.process.poll() is not None:
            raise AssertionError(f"supervisor exited {host.process.returncode}: {diagnostics(log_path)!r}")
        try:
            with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=1)) as conn:
                conn.request("GET", "/ping")
                response = conn.getresponse()
                if response.status == 200 and response.read() == b"Hello from ESP8266!":
                    return
        except (ConnectionError, TimeoutError, OSError):
            pass
        time.sleep(.05)
    raise AssertionError(f"host ESP did not start /ping: {diagnostics(log_path)!r}")


def next_flash_event(stream, seconds):
    deadline = time.monotonic() + seconds
    buffer = b""
    while time.monotonic() < deadline:
        try:
            part = stream.recv(4096)
        except socket.timeout:
            continue
        if not part:
            break
        buffer += part
        assert len(buffer) < 32768, "unbounded SSE response"
        for line in buffer.split(b"\n"):
            if line.startswith(b"data: F1,"):
                return line.removeprefix(b"data: ").strip().decode("ascii")
    raise AssertionError(f"no F1 event pushed after flashrom probe: {buffer[-800:]!r}")


def run():
    with tempfile.TemporaryDirectory(prefix="flash-dashboard-host-",
                                     dir=os.environ.get("TMPDIR")) as temp:
        host = nor_host.Host(BUILD / "pico_serprog", Path(temp), {
            "MOCK_WIFI_LOOPBACK": "",
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent/mock-system-bus",
        })
        stream = None
        try:
            wait_for_http(8000, 30, host, Path(temp) / "fakenorflash-host.log")
            stream = socket.create_connection(("127.0.0.1", 8001), timeout=2)
            stream.settimeout(.5)
            stream.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
            host.flashrom_probe()  # Only --flash-name on the in-memory Winbond fake NOR.
            payload = next_flash_event(stream, 8).split(",")
            assert len(payload) == 10 and payload[0] == "F1", payload
            assert payload[3] == "UNSUP", payload
            assert int(payload[4]) == int("ef4018", 16), payload
            assert payload[6:9] == ["0", "0", "0"], payload
            with closing(http.client.HTTPConnection("127.0.0.1", 8000, timeout=2)) as conn:
                conn.request("GET", "/ping")
                response = conn.getresponse()
                assert response.status == 200, response.status
                response.read()
            print("PASS real flashrom probe -> unsupported-chip F1 -> live SSE and HTTP")
        finally:
            if stream is not None:
                stream.close()
            host.close()


if __name__ == "__main__":
    run()
