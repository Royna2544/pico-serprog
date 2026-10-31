#!/usr/bin/env python3
"""Exercise the real CMake host firmware through USB CDC and serprog.

The unified host runner owns the daemon, sketch processes and private PTYs.
"""

import argparse
import os
from pathlib import Path
import pty
import select
import subprocess
import time
import tty


class Host:
    def __init__(self, binary, output, env_overrides=None):
        if binary.name != "pico_serprog":
            raise ValueError("--binary must be the pico_serprog target in its host build directory")
        self.output = output
        self.pairs = {}
        self.process = None
        output.mkdir(parents=True, exist_ok=True)
        try:
            self.log = (output / "fakenorflash-host.log").open("wb")
            env = {**os.environ, "TMPDIR": str(output),
                   "ASAN_OPTIONS": "abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
            if env_overrides:
                env.update(env_overrides)
            self.process = subprocess.Popen(
                [str(binary.parent / "unified_daemon"), str(binary.parent)],
                env=env, stdout=self.log,
                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            deadline = time.monotonic() + 5
            run_dir = None
            while time.monotonic() < deadline:
                dirs = list(output.glob("unified-board-*/host-cdc0"))
                if dirs:
                    run_dir = dirs[0].parent
                    break
                if self.process.poll() is not None:
                    raise RuntimeError(f"host supervisor exited; see {self.log.name}")
                time.sleep(.02)
            if run_dir is None:
                raise TimeoutError(f"host PTYs unavailable; see {self.log.name}")
            for name in ("uart0", "cdc0", "cdc1", "cdc2"):
                fd = os.open(run_dir / f"host-{name}", os.O_RDWR | os.O_NOCTTY)
                tty.setraw(fd)
                self.pairs[name] = (fd,)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        for pair in self.pairs.values():
            for fd in pair:
                os.close(fd)
        self.pairs.clear()
        if hasattr(self, "log"):
            self.log.close()

    def exchange(self, request, count):
        assert self.process is not None
        fd = self.pairs["cdc0"][0]
        os.write(fd, request)
        data = bytearray()
        deadline = time.monotonic() + 10
        while len(data) < count:
            if self.process.poll() is not None:
                raise RuntimeError(f"host firmware exited; see {self.log.name}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"CDC0 response timed out; see {self.log.name}")
            if select.select([fd], [], [], min(remaining, 0.1))[0]:
                data.extend(os.read(fd, count - len(data)))
        return bytes(data)

    def spi(self, request, count=0):
        packet = (b"\x13" + len(request).to_bytes(3, "little") +
                  count.to_bytes(3, "little") + request)
        result = self.exchange(packet, count + 1)
        assert result[0] == 0x06, f"serprog rejected SPI operation: {result.hex()}"
        return result[1:]

    def ready(self):
        deadline = time.monotonic() + 2
        while self.spi(b"\x05", 1)[0] & 1:
            if time.monotonic() >= deadline:
                raise TimeoutError("NOR BUSY did not clear")
            time.sleep(0.0001)

    def flashrom_probe(self):
        master, slave = pty.openpty()
        tty.setraw(slave)
        path = self.output / "fakenorflash-flashrom.log"
        firmware = self.pairs["cdc0"][0]
        process = None
        try:
            with path.open("wb") as log:
                process = subprocess.Popen(
                    ["flashrom", "-p", f"serprog:dev={os.ttyname(slave)}",
                     "-c", "W25Q128.V", "--flash-name"], stdout=log,
                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
                deadline = time.monotonic() + 30
                while process.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"flashrom probe timed out; see {path}")
                    for fd in select.select([master, firmware], [], [], 0.05)[0]:
                        try:
                            data = os.read(fd, 4096)
                        except OSError:
                            continue
                        if data:
                            os.write(firmware if fd == master else master, data)
                if process.returncode:
                    raise RuntimeError(f"flashrom failed ({process.returncode}); see {path}")
            text = path.read_text(errors="replace")
            assert "W25Q128.V" in text and "Winbond" in text, text
            print("PASS real flashrom serprog probe: Winbond W25Q128.V (16 MiB)")
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("--binary", type=Path, default=root / "build-host/pico_serprog")
    parser.add_argument("--output", type=Path, default=root / "build-host")
    parser.add_argument("--flashrom", action="store_true")
    args = parser.parse_args()
    host = Host(args.binary.resolve(), args.output.resolve())
    try:
        assert host.exchange(b"\x10", 2) == b"\x15\x06"
        time.sleep(0.006)  # Datasheet tPUW: writes inhibited for 5 ms after power-up.
        assert host.spi(b"\x9f", 3) == b"\xef\x40\x18"
        assert host.spi(b"\x5a\0\0\0\0", 4) == b"SFDP"
        assert host.spi(b"\x03\0\x12\x34", 4) == b"\xff" * 4
        host.spi(b"\x06")
        host.spi(b"\x02\0\x12\x34\xa5\x5a\x00\x81")
        host.ready()
        assert host.spi(b"\x03\0\x12\x34", 4) == b"\xa5\x5a\x00\x81"
        host.spi(b"\x06")
        host.spi(b"\x20\0\x12\x34")
        host.ready()
        assert host.spi(b"\x03\0\x12\x34", 4) == b"\xff" * 4
        print("PASS CMake host USB CDC -> serprog -> SPI API -> NOR ID/SFDP/program/read/erase")
        if args.flashrom:
            host.flashrom_probe()
    finally:
        host.close()
    log = (args.output / "fakenorflash-host.log").read_text(errors="replace")
    assert "ERROR: AddressSanitizer" not in log and "runtime error:" not in log, log
    print(f"Host execution log: {args.output.resolve() / 'fakenorflash-host.log'}")


if __name__ == "__main__":
    main()
