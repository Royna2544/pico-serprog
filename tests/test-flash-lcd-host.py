#!/usr/bin/env python3
"""Decode actual host LCD backpack writes after a serprog JEDEC request."""
import http.client
import importlib.util
import os
from pathlib import Path
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


def decode_lcd(writes):
    rows = [[" "] * 16 for _ in range(2)]
    row = col = 0
    first = None
    for value in writes:
        if not value & 4:  # Decode each enable-high sample, not all three bus writes.
            continue
        rs, nibble = value & 1, value >> 4
        if first is None:
            first = (rs, nibble)
            continue
        if first[0] != rs:
            first = (rs, nibble)
            continue
        byte = first[1] << 4 | nibble
        first = None
        if rs:
            if row < 2 and col < 16:
                rows[row][col] = chr(byte)
            col += 1
        elif byte == 1:
            rows = [[" "] * 16 for _ in range(2)]
            row = col = 0
        elif byte & 0x80:
            address = byte & 0x7f
            row, col = (0, address) if address < 0x40 else (1, address - 0x40)
    return ["".join(chars) for chars in rows]


def run():
    with tempfile.TemporaryDirectory(prefix="flash-lcd-host-",
                                     dir=os.environ.get("TMPDIR")) as temp:
        trace = Path(temp) / "lcd.raw"
        previous = os.environ.get("MOCK_LCD_TRACE")
        os.environ["MOCK_LCD_TRACE"] = str(trace)
        host = None
        try:
            host = nor_host.Host(BUILD / "pico_serprog", Path(temp), {
                "MOCK_WIFI_LOOPBACK": "",
                "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent/mock-system-bus",
            })
            ready_until = time.monotonic() + 30
            while time.monotonic() < ready_until:
                status = host.process.poll()
                if status is not None:
                    log = (Path(temp) / "fakenorflash-host.log").read_text(errors="replace")
                    relevant = [line for line in log.splitlines() if
                                "unified:" in line or "ERROR: AddressSanitizer" in line or
                                "runtime error:" in line or "Initialization failed:" in line or
                                ("pico_serprog>" in line and ("<3>" in line or "<4>" in line))]
                    raise AssertionError(f"host supervisor exited {status}: {relevant[:12]!r}; {relevant[-12:]!r}")
                try:
                    conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=1)
                    try:
                        conn.request("GET", "/ping")
                        response = conn.getresponse()
                        if response.status == 200 and response.read() == b"Hello from ESP8266!":
                            break
                    finally:
                        conn.close()
                except (ConnectionError, TimeoutError, OSError):
                    pass
                time.sleep(.05)
            else:
                log = (Path(temp) / "fakenorflash-host.log").read_text(errors="replace")
                relevant = [line for line in log.splitlines() if
                            "WLAN TALK:" in line or "Slave device Server:" in line or
                            "Initialization failed:" in line or "Flash telemetry" in line or
                            "unified:" in line]
                raise AssertionError(f"ESP HTTP unavailable; supervisor={host.process.poll()}; "
                                     f"diagnostics={relevant[-8:]!r}")
            assert host.spi(b"\x9f", 3) == b"\xef\x40\x18"
            deadline = time.monotonic() + 8
            rows = ["", ""]
            while time.monotonic() < deadline:
                assert host.process.poll() is None, "host supervisor exited during LCD update"
                if trace.exists():
                    rows = decode_lcd(trace.read_bytes())
                    if rows[0].strip() == "ID unsupported":
                        break
                time.sleep(.05)
            if rows[0].strip() != "ID unsupported":
                log = (Path(temp) / "fakenorflash-host.log").read_text(errors="replace")
                relevant = [line for line in log.splitlines() if
                            "F1," in line or "display0" in line or
                            "pcf8574t-16x2-lcd-drv display:" in line or
                            "Flash telemetry" in line]
                raise AssertionError(f"LCD not updated: {rows!r}; {relevant[-12:]!r}")
            assert rows[1].strip() == "E:0 P:0", rows
            raw = trace.read_bytes()
            assert raw and all(byte & 0x08 for byte in raw), \
                "LCD backpack P3 was driven low, disabling its backlight"
            print("PASS real host JEDEC transfer -> LCD observed-status rows")
        finally:
            if host is not None:
                host.close()
            if previous is None:
                os.environ.pop("MOCK_LCD_TRACE", None)
            else:
                os.environ["MOCK_LCD_TRACE"] = previous


if __name__ == "__main__":
    run()
