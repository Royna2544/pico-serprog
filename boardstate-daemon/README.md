# Virtual Pico ↔ ESP8266 board

The host build links the existing Pico firmware to `mock-pico-sdk`, compiles
`esp8266/esp8266.ino` with `mock-esp8266-sdk`, and runs the actual
`vpmic/vpmic.ino` against its Uno GPIO shim. The ESP sketch is also a normal
ESP8266 Arduino sketch: the mock `Arduino.h` is only on the host target's
include path. Do **not** add that directory to a real Arduino build.

`boardstate-daemon` reads the **compiled** `board.dtb` on startup: `serial1`
selects the RP2040 UART, `pinctrl-0` supplies the ordered TX/RX GPIOs, and
its `espressif,esp8266` child supplies enable/reset/boot/strap GPIO wires.
The board's external pulls are modeled from the resistor annotations in
`board.dts`, independently of the GPIO peripheral's internal pulls. The
ESP8266's EN and EXT_RSTB are separate signals, **not** ESP GPIO7/GPIO16.
The physical UART connections are GP8 (UART1 TX) → ESP U0RXD/GPIO3 and
ESP U0TXD/GPIO1 → GP9 (UART1 RX), with common ground and 3.3 V logic.
GP18 → CHIP_EN, GP19 → EXT_RSTB, GP20 → GPIO0, GP21 → GPIO2, and
GP22 → GPIO15 are also connected bidirectionally in the daemon's pin model.
The host-only `dts/mock-board.dts` models Uno D2 driving an NPN base with a
pulldown. Its grounded emitter and collector on Pico GP6 pull that input low
when D2 is high; a separate 3.3 V resistor pulls GP6 high when the NPN is off.
The Pico trigger GPIO descriptor is active-low, so collector LOW reads as
logical active. This models the switch and sketch, not an entire AVR.
The mock wiring node has no `compatible`: the daemon reads the compiled DTB,
but Pico's driver core must not register host-only metadata as an unbound device.

Verified against downloaded primary datasheets:

- `pdf/rp2040-datasheet.pdf` (Raspberry Pi, section 1.4, GPIO bank 0
  function table): GPIO8 = UART1 TX, GPIO9 = UART1 RX (function F2).
  https://pip.raspberrypi.com/documents/RP-008371-DS-rp2040-datasheet.pdf
- `pdf/esp8266ex-datasheet.pdf` (Espressif, section 2, pin definitions):
  U0RXD/GPIO3 is QFN pin 25, U0TXD/GPIO1 is pin 26; CHIP_EN pin 7 is
  active high, EXT_RSTB pin 32 active low; GPIO0/2/15 are boot straps
  (pins 15/14/13). UART0 is the two-way serial port; ESP Serial1 isn't.
  https://documentation.espressif.com/0a-esp8266ex_datasheet_en.pdf
- Espressif's [boot-mode guidance](https://docs.espressif.com/projects/esptool/en/latest/esp8266/advanced-topics/boot-mode-selection.html)
  confirms that the ROM samples GPIO0 at reset and changes GPIO2 to a UART
  TX output in serial download mode.

## Build and test

    cmake -S . -B build-host -DMOCK_HARDWARE=ON
    cmake --build build-host --target unified_daemon boardstate-daemon pico_serprog esp8266-mock vpmic-mock boardstate-pico-smoke boardstate-pico-irq esp-restart-probe boardstate-wlan-host -j4
    python tests/test-unified-daemon.py
    python tests/test-unified-child-exit.py
    python tests/test-unified-hard-kill.py
    python tests/test-vpmic-board.py
    python tests/test-vpmic-host.py
    python tests/test-boardstate-verbose.py
    python tests/test-wlan-autoboot-timeout.py
    python tests/test-wlan-reentry.py
    python tests/test-jedec-evt-host.py
    bash tests/test-event-fixtures.sh
    python tests/test-serprog-host.py -v
    python tests/testfakenorflash-host.py
    python tests/test-boardstate.py
    python tests/test-esp-rom.py
    python tests/test-boardstate-irq.py
    python tests/test-boardstate-reset.py
    python tests/test-boardstate-errors.py

The boardstate test starts the daemon, ESP and Pico processes, creates temporary PTYs for the Pico's
existing UART0/CDC mocks, checks GPIO polarity/contention and ESP boot gating,
then exchanges a length-framed SLAVE/MASTER message with the **same sketch**
compiled with the ESP mock. It cleans up its sockets, PTYs and processes.
The second test checks boot-strap sampling, GPIO2 ownership and contention,
normal/invalid modes, quick-reset sketch restart, and daemon transition logs.
The first test also runs the real WLAN driver upload transition through a
host-only firmware entry point and checks that GP21 is an input while the ROM
owns GPIO2 TX. It deliberately drives GP21 afterward to test the daemon's
contention-risk warning.
The host test sets `MOCK_PTY_DIR` to its private directory rather than touching
another instance's `/tmp/mock-*` links.
The JEDEC event test pauses a scripted ESP UART responder during the real Pico
handshake, submits two serprog CDC0 SPI requests, rejects one EVT acknowledgment,
and verifies the first event is retried before the second is sent. The ordinary
boardstate test also checks the real ESP sketch's `EVT:OK` reply. Host serprog
tests use the unified runner's private `host-cdc0` link instead of obsolete
global CDC1 PTYs.
The DTS currently enables ESP autoboot; the Pico samples the bootmode switch
and brings up NORMAL mode when the switch reads as normal. The ROM/upload
driver fixture transitions through NONE before requesting UPLOAD.
The initial `bootmode-source` miss is retried after the switch device registers;
it is not a terminal Wi-Fi probe failure. Without an ESP firmware-ready IRQ,
the WLAN driver now times out, powers EN off and fails startup rather than
waiting forever. `display` still defers because no `nxp,pcf8574t` driver exists
in this tree; implement that driver or disable the display node when unused.

For an all-in-one host run:

    build-host/unified_daemon --verbose build-host

The supervisor starts four private PTY relays, the DT-backed board daemon,
the Uno sketch, the ESP sketch, and the Pico firmware in dependency order.
It prints `BOARDSTATE_SOCKET` and `MOCK_PTY_DIR`; set the former when using
`boardstate-daemon/ctl.py`. The run directory also exposes `host-cdc0..2`
and `host-uart0` PTY links. SIGINT/SIGTERM shuts down every child and removes
its own links/socket; it never unlinks someone else's global `/tmp` links.
SIGKILL also terminates direct children via Linux parent-death signals, but
the supervisor cannot remove its private links after an uncatchable SIGKILL.
`--verbose` adds `pico> pin ...`, `uno> pin ...` and hex-byte
`uart> pico->esp [xx]` / `uart> esp->pico [xx]` diagnostics. A TX accepted
while the ESP is off or RX is unmuxed is labeled discarded/not received.

For daemon-only experiments, start `build-host/boardstate-daemon` with the
compiled DTB and a private socket path, then set `BOARDSTATE_SOCKET` for
`boardstate-daemon/ctl.py` and clients. The Pico firmware now autoboots the
ESP; do not manually override its GPIOs while it is running. UART1 uses the
daemon rather than a PTY. The old `start-host` script creates global `/tmp`
links and is not needed when using the supervisor.

To build for the physical ESP8266 with an installed ESP8266 Arduino core:

    arduino-cli compile --fqbn esp8266:esp8266:generic esp8266/

This does **not** link the host shim. The core/CLI must already be installed;
the host test alone cannot certify a physical firmware build or electrical
behavior. No flash storage, ROM command interpreter or Wi-Fi is emulated; UART queues are bounded
at 4096 bytes and ignore baud timing. `GPIO GET` reports physical levels,
returns `ERR contention` for opposing outputs, and models floating as low.
On the transition where EN and reset become high, the daemon samples
GPIO15=low and GPIO2=high, then latches GPIO0=high as normal flash boot or
GPIO0=low as ROM upload mode. Changes to straps after that transition do not
change mode until the next reset. In ROM mode the sketch is stopped, GPIO2 is
owned by the ROM as UART1 TX and is driven idle-high. No ROM command decoder,
boot-log bytes, UART bit timing or flash image execution is implemented: Pico
UART TX bytes sent in ROM mode or while ESP is off are accepted but discarded.
The daemon logs `Entered bootrom upload mode`, `booting normal mode`, reset and
invalid straps to stderr. A pull-up on GPIO2 is safe for the ROM transmitter;
the Pico driver releases GP21 to input in upload mode. The daemon emits a
once-per-boot contention-risk warning if software drives it after boot.
GPIO0 is also held high by the current Pico driver in normal mode: do not drive
ESP GPIO0 low as a firmware-ready acknowledgment against that output.
GPIO writes from the ESP sketch are rejected while disabled, in ROM mode or
with invalid straps; reset returns sketch-controlled ESP GPIOs to input state.

## Socket protocol

One short ASCII request per connection, one newline-terminated response.

    GPIO pico|esp PIN MUX sio|uart|spi|i2c|pwm|clk
    GPIO pico|esp PIN DIR 0|1
    GPIO pico|esp PIN PUT 0|1
    GPIO pico|esp PIN PULL 00|01|10|11
    GPIO uno 2 MUX sio|DIR 0|1|PUT 0|1|PULL 00|01|10|11|GET|FUNCTION|DRIVE
    GPIO pico|esp PIN GET
    GPIO pico|esp PIN FUNCTION
    GPIO pico|esp PIN DRIVE
    GPIO pico PIN IRQ_STATUS
    GPIO pico PIN IRQ_ENABLE MASK
    GPIO pico PIN IRQ_DISABLE MASK
    GPIO pico PIN IRQ_ACK MASK
    IRQ pico PENDING
    RESET pico
    UART pico|esp TX 0..255
    UART pico|esp RX
    POWER esp
    MODE esp
    BOOT esp

Writes return `OK`; `GET` and `POWER` return 0/1; `MODE` returns
`off|flash|rom|invalid`; `BOOT` returns a reset generation for the sketch
runner; `FUNCTION` returns the mux and `DRIVE` reports
`input|sio-out|uart-tx`. `RX` returns -1 when empty; failures return `ERR ...`.
Invalid GPIO pins identify the board, requested pin, and valid range (Pico
0-29, ESP GPIO 0-16); CHIP_EN and EXT_RSTB are not ESP GPIOs. The C client also
prints the rejected request alongside the daemon's error.
`RESET pico` restores Pico GPIO mux, output, latch, pulls and IRQ state while
retaining board wiring and external resistors. The falling EN signal resets the
ESP and clears its UART/sketch state; the next EN/reset release samples straps
and advances `BOOT esp`. The host `pico_serprog` sends this reset after a 5 ms
startup delay, so relaunching it resets an ESP left running by an earlier host
process. Other mock clients do not reset the Pico on connection.
UART transfer requires the sender's TX mux; the receiving UART must also be
muxed for the byte to enter its queue. An unpowered ESP or ROM loader does not
respond. This is a board-visible wiring and mode simulation, not an
instruction-accurate RP2040/ESP8266 emulator.

Pico GPIO IRQ event bits are low level=1, high level=2, falling edge=4,
rising edge=8. The daemon latches electrical edges across Pico/ESP wires,
masks them per pin, clears old edges when changing enables, and keeps enabled
level events pending while the level persists. Acknowledgment clears edge
latches, not levels. The mock Pico SDK polls pending bank events on a host
worker thread and invokes the installed IO_IRQ_BANK0 raw handler or default
GPIO callback; the callback runs outside the daemon socket transaction. It
models one logical Pico IRQ core (the mock's get_core_num() returns 0), not
RP2040 per-core IRQ routing or interrupt timing. A still-pending level or an
unacknowledged edge can fire repeatedly, as on the device.
