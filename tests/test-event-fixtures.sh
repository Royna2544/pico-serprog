#!/usr/bin/env bash
# Build and run production-path SPI/UART fixtures and the bounded host queue.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo/build-host"
cc=${CC:-clang}
common=(-std=gnu11 -DMOCK_HARDWARE -I"$repo" \
        -I"$repo/mock-pico-sdk/inc" -fsanitize=address,undefined \
        -fno-omit-frame-pointer -g)
for name in test-serprog-cs-order test-serprog-failed-callback uartdrv-io-fixture; do
    "$cc" "${common[@]}" "$repo/tests/$name.c" -o "$repo/build-host/$name"
    "$repo/build-host/$name"
done
"$cc" "${common[@]}" -I"$repo/build-host/_deps/dtc-src/libfdt" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "$repo/tests/wlan-talk-io-fixture.c" -o "$repo/build-host/wlan-talk-io-fixture"
"$repo/build-host/wlan-talk-io-fixture"
CXX=${CXX:-clang++} bash "$repo/tests/test-pico-queue.sh"
