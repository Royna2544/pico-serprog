# Focused source regressions

After building with `MOCK_HARDWARE=ON`, run:

```sh
python3 tests/source-regressions/run.py --build-dir build-host
```

The runner uses the CMake build's libfdt and compiles the production core,
GPIO/pinctrl drivers, and ESP sketch with AddressSanitizer and UBSan. Hardware
operations and the IRQ-controller lookup are fixture boundaries; matching,
registration, cleanup, and framing use the production implementations.

Cases cover pending GPIO events and flag conversion, handler publication and
rollback, disabling before handler removal, claim release/reacquisition, named
state lookup, secondary match entries and compatible fallbacks, deferred-loop
termination, missing IRQ operations, mapping rollback/retry, and fragmented or
malformed UART frames. Each case has a five-second timeout.

If the host's PID/proc namespace prevents LeakSanitizer thread enumeration, run
with `ASAN_OPTIONS=detect_leaks=0`; address and undefined-behavior checks remain
enabled.
