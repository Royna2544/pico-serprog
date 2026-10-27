#!/usr/bin/env python3
"""Run focused host regressions after a MOCK_HARDWARE CMake build."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-host")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    dtc = Path(cache.get("FETCHCONTENT_SOURCE_DIR_DTC") or
               Path(cache.get("FETCHCONTENT_BASE_DIR", build / "_deps")) / "dtc-src")
    includes = ["-I" + str(ROOT), "-I" + str(ROOT / "mock-pico-sdk/inc"),
                "-I" + str(Path(dtc) / "libfdt")]
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    fixtures = [
        ("gpio.c", ["events", "claims", "publication", "type-failure", "enable-failure"]),
        ("core.c", ["matching", "fallback", "deferred", "missing-domain", "no-ops",
                    "no-xlate", "no-map", "xlate-failure", "map-failure"]),
        ("esp.cpp", [None]),
    ]
    with tempfile.TemporaryDirectory(prefix="pico-source-regressions-") as tmp:
        for source, cases in fixtures:
            cxx = source.endswith(".cpp")
            compiler = shlex.split(os.environ.get("CXX" if cxx else "CC",
                                                  "c++" if cxx else "cc"))
            binary = str(Path(tmp) / Path(source).stem)
            cmd = compiler + flags + includes
            if cxx:
                cmd += ["-std=c++17", "-I" + str(ROOT / "mock-esp8266-sdk")]
            cmd += [str(Path(__file__).parent / source), str(build / "libfdt.a"),
                    "-pthread", "-o", binary]
            subprocess.run(cmd, check=True)
            for case in cases:
                subprocess.run([binary] + ([case] if case else []),
                               check=True, timeout=5)
                print("PASS", source, case or "fragmentation/recovery", flush=True)


if __name__ == "__main__":
    main()
