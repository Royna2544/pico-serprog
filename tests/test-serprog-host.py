#!/usr/bin/env python3
"""Serprog protocol regression tests against the real host firmware and private PTYs."""
import importlib.util
import os
from pathlib import Path
import select
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("fakenor_host", HERE / "testfakenorflash-host.py")
assert spec is not None and spec.loader is not None
host_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host_module)


class SerprogHostTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="serprog-", dir=os.environ.get("TMPDIR"))
        self.host = host_module.Host(ROOT / "build-host/pico_serprog", Path(self.tmp.name))

    def tearDown(self):
        self.host.close()
        log = (Path(self.tmp.name) / "fakenorflash-host.log").read_text(errors="replace")
        self.tmp.cleanup()
        self.assertNotIn("ERROR: AddressSanitizer", log)
        self.assertNotIn("runtime error:", log)

    def test_spi_send_257_bytes_does_not_overflow(self):
        self.assertEqual(self.host.spi(b"\x9f" + bytes(256)), b"")
        self.assertEqual(self.host.spi(b"\x9f", 3), b"\xef\x40\x18")

    def test_spi_send_across_multiple_cdc_reads(self):
        self.assertEqual(self.host.spi(b"\x9f" + bytes(4095)), b"")
        self.assertEqual(self.host.spi(b"\x9f", 3), b"\xef\x40\x18")

    def test_spi_receive_256_bytes_has_one_ack(self):
        self.assertEqual(self.host.spi(b"\x03\0\0\0", 256), b"\xff" * 256)
        self.assertEqual(self.host.exchange(b"\x00", 1), b"\x06")

    def test_spi_receive_1025_bytes_is_exactly_framed(self):
        self.assertEqual(self.host.spi(b"\x03\0\0\0", 1025), b"\xff" * 1025)
        self.assertEqual(self.host.exchange(b"\x00", 1), b"\x06")

    def test_spi_at_advertised_read_limit(self):
        self.assertEqual(self.host.spi(b"\x03\0\0\0", 4096), b"\xff" * 4096)
        self.assertEqual(self.host.exchange(b"\x00", 1), b"\x06")

    def test_queries_report_spi_only_and_real_limits(self):
        self.assertEqual(self.host.exchange(b"\x01", 3), b"\x06\x01\x00")
        bits = self.host.exchange(b"\x02", 33)
        self.assertEqual(bits[0], 6)
        implemented = {0, 1, 2, 3, 4, 5, 8, 16, 17, 18, 19, 20}
        self.assertEqual({i for i in range(256) if bits[1 + i // 8] & (1 << (i % 8))}, implemented)
        self.assertEqual(self.host.exchange(b"\x05", 2), b"\x06\x08")
        self.assertEqual(self.host.exchange(b"\x08", 4), b"\x06\x00\x10\x00")
        self.assertEqual(self.host.exchange(b"\x11", 4), b"\x06\x00\x10\x00")
        self.assertEqual(self.host.exchange(b"\x07", 1), b"\x15")

    def test_unsupported_address_commands_consume_parameters_and_nak(self):
        for request in (b"\x09\x11\x22\x33", b"\x0a\x01\x02\x03\x04\x05\x06",
                        b"\x0b", b"\x0c\x11\x22\x33\x44", b"\x0f"):
            with self.subTest(request=request):
                self.assertEqual(self.host.exchange(request + b"\x10", 3), b"\x15\x15\x06")
        self.assertEqual(self.host.spi(b"\x9f", 3), b"\xef\x40\x18")

    def test_rejected_spi_lengths_drain_payload_and_preserve_framing(self):
        req = b"\x13" + (4097).to_bytes(3, "little") + (0).to_bytes(3, "little") + bytes(4097)
        self.assertEqual(self.host.exchange(req + b"\x10", 3), b"\x15\x15\x06")
        self.assertEqual(self.host.exchange(b"\x13\x00\x00\x00\x01\x10\x00\x10", 3), b"\x15\x15\x06")

    def test_spi_frequency_is_reported_from_hardware_and_zero_is_nak(self):
        self.assertEqual(self.host.exchange(b"\x14\0\0\0\0", 1), b"\x15")
        self.assertEqual(self.host.exchange(b"\x14" + (150_000_000).to_bytes(4, "little"), 5),
                         b"\x06" + (133_000_000).to_bytes(4, "little"))

    def test_unsupported_spi_configuration_commands_consume_argument(self):
        for command in (0x15, 0x16, 0x17, 0x18):
            with self.subTest(command=command):
                self.assertEqual(self.host.exchange(bytes((command, 1, 0x10)), 3),
                                 b"\x15\x15\x06")

    def test_fragmented_spi_header_and_payload(self):
        fd = self.host.pairs["cdc0"][0]
        os.write(fd, b"\x13\x01")
        time.sleep(.05)
        os.write(fd, b"\x00\x00\x03\x00\x00\x9f")
        answer = bytearray()
        deadline = time.monotonic() + 3
        while len(answer) < 4 and time.monotonic() < deadline:
            if select.select([fd], [], [], .1)[0]:
                answer.extend(os.read(fd, 4 - len(answer)))
        self.assertEqual(bytes(answer), b"\x06\xef\x40\x18")


if __name__ == "__main__":
    unittest.main(verbosity=2)
