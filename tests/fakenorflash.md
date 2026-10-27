# W25Q128JV fake NOR model

## Reference and scope

Production implementation: `drivers/fakenorflash.cpp`.
Reference: `pdf/w25q128jv_revf_03272018_plus-1489608.pdf`, Revision F,
March 27, 2018. Section numbers and page numbers below refer to the printed
Winbond document, not the PDF viewer's page index.

The model uses the existing SPI master/chip callbacks and the existing
`fake,spi-master` / `fake,spi-chip` device-tree bindings. Each probed chip has
its own erased 16 MiB array, three separate 256-byte security registers,
status/nonvolatile-status banks, emulated UID, and individual protection bits.
Memory lasts for that chip instance; it is not saved to a backing file.

The selected ordering option is IQ/JQ: JEDEC ID `EF 40 18`, electronic device
ID `17`, and QE fixed to 1, as specified in sections 8.1.1 and 11. The IM/JM
variant, DTR, QPI instructions, and undocumented vendor OTP-prefix sequences
are not imported from other datasheets.

Tables 8.1.2/8.1.3 contain these 43 distinct opcode values (including both
chip-erase encodings):

    01 02 03 04 05 06 0B 11 15 20 31 32 35 36 39 3B 3D 42 44 48
    4B 50 52 5A 60 66 6B 75 77 7A 7E 90 92 94 98 99 9F AB B9 BB
    C7 D8 EB

## SPI API representation

Both read and write callbacks clock the same synchronous parser. A write
consumes MOSI and discards simultaneous MISO. A read supplies MOSI zeroes,
like the real master driver. There are no queues, polling threads, or detached
workers. Undriven MISO is represented as FF. Repeated CS assertions do not
restart an already selected frame. Mutations start only at the CS rising edge
of a valid frame, with fixed-length commands checked for exact length.

Callers own serialization of each complete shared-master CS transaction:
assert CS, perform all reads/writes, then deassert CS. The master's mutex is
per callback; it neither reserves the bus across callbacks nor protects device
lifetime. Before removing a chip/master, callers must stop new callbacks and
wait for in-flight callbacks on the affected device to finish. This mutex is
not a transaction or lifetime API.

The API cannot represent pin samples, lane widths, or partial-byte frames.
For dual/quad commands it consumes the logical byte fields of table 8.1.3:

| Opcode | Fields after opcode, before data |
| --- | --- |
| 03 | Three address bytes |
| 0B | Three address bytes, one dummy byte |
| 3B | Three address bytes, two dual-byte dummy fields |
| 6B | Three address bytes, four quad-byte dummy fields |
| BB, 92 | Three address bytes, one mode byte |
| EB, 94 | Three address bytes, one mode byte, two quad-byte dummy fields |
| 77 | Three quad-byte dummy fields, one wrap-setting byte |
| 4B | Four dummy bytes |
| 5A, 48 | Three address bytes, one dummy byte |
| AB identification | Three dummy bytes |

3B/6B's dummy fields represent eight physical clock cycles, not two/four
single-lane bytes on real hardware. Mode bytes must be Fx. Revision F does
not define transitions to opcode-elision modes for other values; the model
ignores those frames. For 94, the model follows table 8.1.3/Figure 41 rather
than the contradictory dummy-before-address prose in section 8.2.25.

/WP, /HOLD, and hardware /RESET remain deasserted because the API has no
callbacks for those pins. Baud configuration honors `default-baud`, supports
set/get, rejects zero defaults, and clamps to 133 MHz. Transfer waveform,
voltage, clock-rate-dependent errors, endurance wear, and analog effects are
not simulated.

## State and protection rules

- Page/security programming stages a bounded 256-byte buffer. Overlong input
  overwrites previously staged bytes at wrapped offsets; completion applies
  bitwise AND to storage. Repeated input is not prematurely ANDed together.
- Erases align to their full 4/32/64 KiB target. Any protected byte rejects the
  entire command. Chip erase rejects any main-array protection.
- WPS selects status-based protection or individual locks, not both. The
  correct geometry is 254 interior 64 KiB blocks and 32 edge 4 KiB sectors
  (Figure 4d); the prose's 126-block count is a typo.
- The protection tables omit SEC=1/BP=110. The explicit emulator policy is to
  protect the whole array for that undefined combination, regardless of CMP.
- 50 enables volatile status writes separately from WEL. It does not authorize
  array or security programming. Volatile status writes do not set BUSY.
  One-byte 01 leaves SR2 unchanged; two-byte 01 updates both registers.
- SRP is writable despite its omission from section 8.2.5's list, because
  section 7.1.1's protection scheme requires it. Reserved status bits read zero.
- LB1/LB2/LB3 are permanent OTP locks, including when set through 50. Neither
  another status write nor software reset clears them. SRL is treated as a
  power-supply lock-down and survives software reset; no external power-cycle
  control is provided by this SPI API. These choices prioritize the explicit
  OTP and power-supply protection descriptions over ambiguous volatile wording.
- Accepted lock/unlock operations consume WEL and complete without an artificial
  BUSY interval. WPS gates protection enforcement, not lock-bit storage/readback.
  The PDF does not specify those lock-command timing/WEL details explicitly.
- Invalid, truncated, or protected operations preserve WEL and do not mutate
  storage or start BUSY. Discarded output never leaks to a later read.

## Timing, suspend, reset, power-down

The default clock is monotonic host time. Completion is independent of polling
and API buffer chunking. It is evaluated on the next SPI interaction without
a background worker. Tests substitute only the clock, not the command model.

| Event | Modeled delay |
| --- | --- |
| Initial read enable / write enable | 20 microseconds / 5 milliseconds |
| Page / security program | 400 microseconds |
| Nonvolatile status write | 10 milliseconds |
| Sector / security erase | 45 milliseconds |
| 32 KiB / 64 KiB erase | 120 / 150 milliseconds |
| Chip erase | 40 seconds |
| Suspend / minimum resume-to-suspend gap | 20 microseconds |
| Software reset recovery | 30 microseconds |
| Deep-power-down entry | 3 microseconds |
| Release without / with ID | 3 / 1.8 microseconds |

Program/erase timing uses the typical values in section 9.6, not worst-case
values. Writes are staged until completion. An interrupted operation leaves
its original bytes intact instead of inventing a physical corruption pattern.

Suspended work retains its buffer and remaining duration separately from
foreground work. SUS can coexist with foreground BUSY. Nested suspend and
resume during foreground BUSY are ignored. Chip erase, status writes, and
security-register operations cannot be suspended. Erase suspend permits
programming elsewhere but not another erase; program suspend permits erasing
elsewhere but not another program. Status writes are prohibited in either
suspended state. Access to the suspended erase target or the containing 4 KiB
sector of a suspended program is excluded; forbidden reads return FF.

66/99 are separate CS-framed instructions, accepted even during BUSY. Any
intervening instruction cancels reset enable. Reset aborts active/suspended
work, restores nonvolatile status, clears WEL/SUS, disables wrap, and relocks
all individual regions, without erasing completed array/security data or UID.
Deep power-down is not a power cycle: only AB is accepted there, not reset or
status reads, and waking does not erase/reinitialize the model.

## SFDP and identity provenance

The UID is emulator-assigned, stable for the chip instance, distinct across
chip instances, and unchanged by reset. It is not a captured factory UID.

Section 8.2.28 does not provide the chip's SFDP bytes. The model therefore
exposes a clearly emulator-defined JESD216 1.0 basic flash parameter table,
with one 9-DWORD BFPT, the actual modeled density/read/erase capabilities, and
FF-filled unused space in the 256-byte register. It is not claimed to be an
authentic Revision F factory dump. Header/table structure is documented in
[Macronix AN-114, sections 2.1–2.4](https://www.macronix.com/Lists/ApplicationNote/Attachments/1870/AN114v1-SFDP%20Introduction.pdf),
and BFPT read/address fields can be cross-checked against Linux's
`drivers/mtd/spi-nor/sfdp.h`. High initial SFDP address bits must be zero;
the register's byte counter wraps at 256.

JEDEC ID returns its defined three bytes, UID its eight bytes, then FF for
unspecified extra clocks. AB repeats 17; manufacturer/device reads alternate
EF/17. These streams are independent of array/security data.

## Reproducing verification

Run from the repository root. The standalone tests include the production C++
driver and invoke its actual probe/CS/read/write/remove callbacks; only the
surrounding registration/log/property services and clock are test fixtures.
They do not use source-text assertions as behavioral tests.

    clang++ -std=c++17 -pthread -I. -Wall -Wextra -Werror \
        -Wno-missing-field-initializers -g -O1 -fsanitize=address,undefined \
        tests/testfakenorflash.cpp -o build-host/testfakenorflash
    build-host/testfakenorflash

For the concurrency sanitizer, replace `-fsanitize=address,undefined` with
`-fsanitize=thread` and use a different binary filename. The suite includes
all documented opcodes, protection boundaries, malformed frames, power/timing,
OTP retention, suspend/foreground interactions, multi-device lifecycle,
concurrent independent masters, and 300 seeded reference-model operations
followed by a complete 16 MiB comparison.

For the actual CMake host firmware plus PTY/CDC/serprog and flashrom probe:

    cmake -S . -B build-host -DMOCK_HARDWARE=ON \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        '-DCMAKE_C_FLAGS=-std=gnu23 -mllvm -asan-globals=0' \
        '-DCMAKE_CXX_FLAGS=-mllvm -asan-globals=0'
    cmake --build build-host -j 4
    python tests/testfakenorflash-host.py --flashrom

When switching an existing build cache from GCC to Clang, CMake can discard
MOCK_HARDWARE during its compiler-change reconfigure; rerun the same configure
command if it tries to import the physical Pico SDK. The existing populated
DTC source can be reused with `-DFETCHCONTENT_SOURCE_DIR_DTC=$PWD/build-host/_deps/dtc-src`
and `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON`.

GNU C23 makes the existing no-argument C definitions compatible with the
initcall function-pointer types under Clang's function sanitizer. Disabling
ASan global instrumentation only in this host build avoids padded redzones
between the existing linker-collected initcall records; heap/stack ASan and
UBSan remain enabled. Standalone driver tests run with full normal ASan
instrumentation, including globals, without this workaround. No core or linker
source changes are needed.

The host test builds a test-only preload shim to redirect mock SDK paths to
private PTYs. It creates no /tmp links, does not use physical hardware, and
terminates and waits for its own firmware/flashrom processes before closing
all PTYs. Logs and binaries stay under the selected on-disk output directory.
`--flashrom` verifies real flashrom identification, not a full flashrom write
or whole-chip flashrom read; program/read/erase are separately exercised over
the production CDC/serprog path.
