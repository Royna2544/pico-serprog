#!/usr/bin/env bash
# Host-only production device/LED lifecycle fixture; no board or device required.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=${TMPDIR:-${XDG_CACHE_HOME:-$HOME/.cache}}
mkdir -p "$scratch"
build=$(mktemp -d "$scratch/device-led-lifecycle.XXXXXX")
trap 'rm -rf -- "$build"' EXIT
cd "$repo"
flags=(-std=gnu11 -g -O0 -Wall -Wextra -Werror -Wno-unused-parameter
       -Wno-unused-variable
       -Itests/device-led-fixture/include -I.)
link=(-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free -lfdt)
"${CC:-cc}" "${flags[@]}" -Wno-calloc-transposed-args \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/device-led-fixture/lifecycle.c "${link[@]}" -o "$build/lifecycle"
cases=(pinctrl_failure_no_node_leak failed_probe_releases_pinctrl
       deferred_unprobed_remove_is_not_called retry_then_remove_once
       led_allocation_failure led_register_failure_releases_current_descriptor
       led_gpio_failure_unwinds_registered_sibling led_success_removes_each_once)
for case in "${cases[@]}"; do
    "$build/lifecycle" "$case"
done
# MSan reaches the allocation-failure error path that used to read uninitialized i.
if command -v clang >/dev/null 2>&1; then
    clang "${flags[@]}" -O1 -fsanitize=memory -fno-omit-frame-pointer \
        tests/device-led-fixture/lifecycle.c "${link[@]}" -o "$build/lifecycle-msan"
    MSAN_OPTIONS=exit_code=86:halt_on_error=1 "$build/lifecycle-msan" led_allocation_failure
fi
