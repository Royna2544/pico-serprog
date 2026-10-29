#!/usr/bin/env bash
# Compile and exercise the real mock Pico queue API without CMake or board IO.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo/build-host"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
    -I"$repo/mock-pico-sdk/inc" \
    "$repo/tests/test-pico-queue.cpp" \
    "$repo/mock-pico-sdk/src/queue.cpp" \
    -o "$repo/build-host/test-pico-queue"
if (( $# )); then
    cases=("$@")
else
    cases=(capacity try_add try_remove cross_thread concurrent_fifo concurrent_try)
fi
for case in "${cases[@]}"; do
    timeout 15s "$repo/build-host/test-pico-queue" "$case"
done
