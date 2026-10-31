#!/usr/bin/env python3
"""Host Pico I2C shim exposes LCD bytes without asserting electrical behavior."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build-host/test-mock-i2c"


def run():
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-I", str(ROOT / "mock-pico-sdk/inc"),
                    str(ROOT / "tests/test-mock-i2c.c"), "-o", str(BINARY)],
                   check=True, timeout=15)
    with tempfile.TemporaryDirectory(prefix="mock-lcd-", dir=os.environ.get("TMPDIR")) as temp:
        trace = Path(temp) / "lcd.bin"
        result = subprocess.run([str(BINARY)], env={**os.environ, "MOCK_LCD_TRACE": str(trace)},
                                capture_output=True, text=True, check=True, timeout=5)
        assert "PASS mock I2C writes report the byte count" in result.stdout
        assert trace.exists(), "LCD byte trace was not created"
        assert trace.read_bytes() == bytes([0x28, 0x0c]), "LCD byte trace is not the sent bytes"
        print("PASS mock LCD I2C byte trace")


if __name__ == "__main__":
    run()
