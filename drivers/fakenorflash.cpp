/*
 * W25Q128JV-IQ/JQ command model, datasheet Revision F (March 27, 2018).
 *
 * The SPI API exchanges whole logical bytes, not pin samples. Dual/quad
 * instructions flatten the byte fields of table 8.1.3: 3Bh has two dummy
 * fields, 6Bh four, and EBh/94h one mode field plus two dummy fields. Fx mode
 * values are supported; this revision does not specify opcode-elision modes.
 * /WP, /HOLD and hardware /RESET are deasserted; their pins are not in the API.
 *
 * The master's mutex serializes individual callbacks, not complete CS frames.
 * Callers must serialize each shared-master transaction from CS assertion
 * through all transfers to CS deassertion, and quiesce callbacks before remove.
 * This is not a transaction or lifetime API. Writes discard MISO; reads clock
 * zeroes on MOSI, and undriven MISO reads as FFh.
 * Timed operations use a monotonic clock and the typical times in section 9.6;
 * buffers commit atomically at completion. Reset aborts uncommitted work
 * without simulating the unspecified partial corruption of physical flash.
 * See tests/fakenorflash.md for the remaining explicitly chosen model policies.
 */
extern "C" {
#include "core/devicetree.h"
#include "drivers/spidrv.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

struct FakeNor;

struct FakeSpiBus {
  using Clock = std::chrono::steady_clock;
  std::mutex mutex;
  Clock::time_point (*now)() = Clock::now;
  FakeNor *selected = nullptr;
  uint32_t baud = 10000000;
  bool enabled = true;
};

struct FakeSpiMaster {
  std::shared_ptr<FakeSpiBus> bus = std::make_shared<FakeSpiBus>();
};

struct FakeNor {
  using Clock = std::chrono::steady_clock;
  using Microseconds = std::chrono::microseconds;
  static constexpr uint32_t capacity = 16 * 1024 * 1024;
  static constexpr uint32_t page_size = 256;

  enum class Phase {
    Command,
    Address,
    Mode,
    Dummy,
    Input,
    Output,
    Done,
    Ignore
  };
  enum class Operation {
    None,
    Program,
    Erase,
    WriteRegisters,
    ProgramSecurity,
    EraseSecurity
  };

  struct Frame {
    Phase phase = Phase::Command;
    uint8_t opcode = 0;
    unsigned address_left = 0;
    unsigned dummy_left = 0;
    bool reset_armed = false;
    uint32_t address = 0;
    uint32_t length = 0;
    uint32_t count = 0;
    uint8_t column = 0;
    size_t output = 0;
    std::array<uint8_t, page_size> data = {};
    std::array<bool, page_size> written = {};
  } frame;

  struct Pending {
    Operation operation = Operation::None;
    uint32_t address = 0;
    uint32_t size = 0;
    uint8_t register_mask = 0;
    std::array<uint8_t, 3> registers = {};
    std::array<uint8_t, page_size> data = {};
    std::array<bool, page_size> written = {};
    Clock::time_point deadline;
    Clock::duration remaining = Clock::duration::zero();
  } pending, suspended;

  std::shared_ptr<FakeSpiBus> bus;
  std::vector<uint8_t> flash;
  std::array<uint8_t, 3 * page_size> security;
  std::array<bool, 254 + 32> locks;
  std::array<uint8_t, 3> registers = {0, 0x02, 0x60};
  std::array<uint8_t, 3> nonvolatile_registers = registers;
  bool wel = false;
  bool volatile_write = false;
  bool reset_enabled = false;
  bool sleeping = false;
  bool suspend_transition = false;
  Clock::time_point suspend_until;
  Clock::time_point resuspend_after = Clock::time_point::min();
  Clock::time_point available_at = Clock::time_point::min();
  Clock::time_point writes_ready_at = Clock::time_point::min();
  uint8_t wrap = 0x10;
  uint64_t unique_id;
  Clock::time_point (*now)() = Clock::now;

  explicit FakeNor(std::shared_ptr<FakeSpiBus> parent)
      : bus(parent), flash(capacity, 0xff), now(parent->now) {
    static std::atomic<uint64_t> sequence{1};
    // Generated emulator identity, not a claimed factory serial number.
    unique_id = 0x46414b4500000000ULL | sequence.fetch_add(1);
    security.fill(0xff);
    locks.fill(true);
    available_at = now() + Microseconds(20);
    writes_ready_at = now() + Microseconds(5000);
  }

  bool has_suspended() const { return suspended.operation != Operation::None; }

  bool busy() const {
    return pending.operation != Operation::None || suspend_transition;
  }

  void update() {
    if (suspend_transition && now() >= suspend_until)
      suspend_transition = false;
    if (pending.operation == Operation::None || now() < pending.deadline)
      return;
    auto *memory = pending.operation == Operation::ProgramSecurity ||
                           pending.operation == Operation::EraseSecurity
                       ? security.data()
                       : flash.data();
    if (pending.operation == Operation::Program ||
        pending.operation == Operation::ProgramSecurity) {
      for (size_t i = 0; i < page_size; ++i)
        if (pending.written[i])
          memory[pending.address + i] &= pending.data[i];
    } else if (pending.operation == Operation::Erase ||
               pending.operation == Operation::EraseSecurity) {
      std::fill_n(memory + pending.address, pending.size, 0xff);
    } else if (pending.operation == Operation::WriteRegisters) {
      for (unsigned i = 0; i < registers.size(); ++i)
        if (pending.register_mask & (1 << i))
          registers[i] = nonvolatile_registers[i] = pending.registers[i];
    }
    pending.operation = Operation::None;
    wel = false;
  }

  uint8_t status(unsigned index) const {
    uint8_t value = registers[index];
    if (index == 0)
      value |= (busy() ? 1 : 0) | (wel ? 2 : 0);
    else if (index == 1 && has_suspended())
      value |= 0x80;
    return value;
  }

  void select() { frame = Frame{}; }

  bool forbidden_while_suspended(uint8_t opcode) const {
    if (!has_suspended())
      return false;
    switch (opcode) {
    case 0x01:
    case 0x31:
    case 0x11:
      return true;
    case 0x02:
    case 0x32:
    case 0x42:
      return suspended.operation == Operation::Program;
    case 0x20:
    case 0x52:
    case 0xd8:
    case 0xc7:
    case 0x60:
    case 0x44:
      return suspended.operation == Operation::Erase;
    default:
      return false;
    }
  }

  static bool is_write(uint8_t opcode) {
    switch (opcode) {
    case 0x06:
    case 0x50:
    case 0x01:
    case 0x31:
    case 0x11:
    case 0x02:
    case 0x32:
    case 0x42:
    case 0x20:
    case 0x52:
    case 0xd8:
    case 0xc7:
    case 0x60:
    case 0x44:
    case 0x36:
    case 0x39:
    case 0x7e:
    case 0x98:
      return true;
    default:
      return false;
    }
  }

  void decode(uint8_t opcode) {
    frame.opcode = opcode;
    frame.length = 1;
    frame.reset_armed = reset_enabled;
    reset_enabled = false;
    if (now() < available_at || (sleeping && opcode != 0xab) ||
        (now() < writes_ready_at && is_write(opcode)) ||
        forbidden_while_suspended(opcode) ||
        (busy() && opcode != 0x05 && opcode != 0x35 && opcode != 0x15 &&
         opcode != 0x66 && opcode != 0x99 && opcode != 0x75)) {
      frame.phase = Phase::Ignore;
      return;
    }
    switch (opcode) {
    case 0x9f:
    case 0x05:
    case 0x35:
    case 0x15:
      frame.phase = Phase::Output;
      break;
    case 0x06:
    case 0x04:
    case 0x50:
    case 0xc7:
    case 0x60:
    case 0x7e:
    case 0x98:
    case 0xb9:
    case 0x66:
    case 0x99:
    case 0x75:
    case 0x7a:
      frame.phase = Phase::Done;
      break;
    case 0x01:
    case 0x31:
    case 0x11:
      frame.phase = Phase::Input;
      break;
    case 0xab:
    case 0x4b:
    case 0x77:
      frame.dummy_left = opcode == 0x4b ? 4 : 3;
      frame.phase = Phase::Dummy;
      break;
    case 0x02:
    case 0x32:
    case 0x03:
    case 0x0b:
    case 0x3b:
    case 0x6b:
    case 0xbb:
    case 0xeb:
    case 0x90:
    case 0x92:
    case 0x94:
    case 0x42:
    case 0x44:
    case 0x48:
    case 0x5a:
    case 0x36:
    case 0x39:
    case 0x3d:
    case 0x20:
    case 0x52:
    case 0xd8:
      frame.phase = Phase::Address;
      frame.address_left = 3;
      break;
    default:
      frame.phase = Phase::Ignore;
      break;
    }
  }

  static uint8_t sfdp(uint8_t address) {
    // Emulator-defined JESD216 1.0 BFPT, not a factory SFDP dump. Section
    // 8.2.28 does not publish those bytes. Header/layout: Macronix AN-114;
    // read/erase fields describe the commands implemented here (8.1.2/3).
    static constexpr uint8_t header[] = {
        'S', 'F', 'D', 'P', 0, 1, 0, 0xff, 0, 0, 1, 9, 0x10, 0, 0, 0xff,
    };
    static constexpr uint32_t bfpt[] = {
        0xfff120e5, // 3-byte addressing, 20h erase, dual/quad reads, no DTR.
        capacity * 8 - 1, // Density in bits minus one.
        0x6b08eb44, // 1-4-4: 2 mode + 4 wait clocks; 1-1-4: 8 wait clocks.
        0xbb803b08, // 1-2-2: 4 mode clocks; 1-1-2: 8 wait clocks.
        0xffffffee, // No 2-2-2 or 4-4-4 protocol.
        0xffffffff,       0xffffffff,
        0x520f200c, // Erase types 1/2: 4 KiB / 20h, 32 KiB / 52h.
        0x0000d810, // Erase type 3: 64 KiB / D8h; type 4 unsupported.
    };
    if (address < sizeof(header))
      return header[address];
    unsigned offset = address - sizeof(header);
    return offset < sizeof(bfpt) ? bfpt[offset / 4] >> (8 * (offset % 4))
                                 : 0xff;
  }

  uint8_t output() {
    static constexpr uint8_t jedec[] = {0xef, 0x40, 0x18};
    switch (frame.opcode) {
    case 0x9f: {
      return frame.output < sizeof(jedec) ? jedec[frame.output++] : 0xff;
    }
    case 0x05:
      return status(0);
    case 0x35:
      return status(1);
    case 0x15:
      return status(2);
    case 0xab:
      return 0x17;
    case 0x90:
    case 0x92:
    case 0x94:
      return (frame.address++ & 1) ? 0x17 : 0xef;
    case 0x4b:
      return frame.output < 8 ? unique_id >> (56 - 8 * frame.output++) : 0xff;
    case 0x48: {
      int bank = security_bank(frame.address);
      return bank < 0 ? 0xff : security[bank * page_size + frame.column++];
    }
    case 0x3d:
      return locks[lock_index(frame.address)] ? 1 : 0;
    case 0x5a:
      return sfdp(frame.column++);
    case 0x03:
    case 0x0b:
    case 0x3b:
    case 0x6b:
    case 0xbb:
    case 0xeb: {
      uint8_t byte =
          overlaps_suspended(frame.address, 1) ? 0xff : flash[frame.address];
      uint32_t mask = frame.opcode == 0xeb && !(wrap & 0x10)
                          ? (8u << ((wrap >> 5) & 3)) - 1
                          : capacity - 1;
      frame.address = (frame.address & ~mask) | ((frame.address + 1) & mask);
      return byte;
    }
    default:
      return 0xff;
    }
  }

  void address_ready() {
    frame.column = static_cast<uint8_t>(frame.address);
    switch (frame.opcode) {
    case 0x02:
    case 0x32:
    case 0x42:
      frame.phase = Phase::Input;
      break;
    case 0x03:
    case 0x90:
    case 0x3d:
      frame.phase = Phase::Output;
      break;
    case 0x0b:
    case 0x3b:
    case 0x6b:
    case 0x48:
    case 0x5a:
      frame.dummy_left = frame.opcode == 0x3b   ? 2
                         : frame.opcode == 0x6b ? 4
                                                : 1;
      frame.phase = frame.opcode == 0x5a && frame.address >= 256 ? Phase::Ignore
                                                                 : Phase::Dummy;
      break;
    case 0xbb:
    case 0x92:
    case 0xeb:
    case 0x94:
      frame.dummy_left = frame.opcode == 0xeb || frame.opcode == 0x94 ? 2 : 0;
      frame.phase = Phase::Mode;
      break;
    default:
      frame.phase = Phase::Done;
      break;
    }
  }

  uint8_t transfer(uint8_t input) {
    update();
    if (frame.phase == Phase::Command) {
      decode(input);
      return 0xff;
    }
    if (frame.length != UINT32_MAX)
      ++frame.length;
    switch (frame.phase) {
    case Phase::Address:
      frame.address = (frame.address << 8) | input;
      if (!--frame.address_left)
        address_ready();
      break;
    case Phase::Mode:
      // Revision F specifies Fx mode bits; undocumented XIP modes are rejected.
      frame.phase = (input & 0xf0) != 0xf0 ? Phase::Ignore
                    : frame.dummy_left     ? Phase::Dummy
                                           : Phase::Output;
      break;
    case Phase::Dummy:
      if (!--frame.dummy_left)
        frame.phase = frame.opcode == 0x77 ? Phase::Input : Phase::Output;
      break;
    case Phase::Input:
      if (frame.opcode == 0x02 || frame.opcode == 0x32 ||
          frame.opcode == 0x42) {
        frame.data[frame.column] = input;
        frame.written[frame.column++] = true;
      } else if (frame.count < 2) {
        frame.data[frame.count] = input;
      }
      if (frame.count < page_size + 1)
        ++frame.count;
      break;
    case Phase::Output:
      return output();
    default:
      break;
    }
    return 0xff;
  }

  static int security_bank(uint32_t address) {
    uint32_t bank = address >> 8;
    return bank == 0x10 ? 0 : bank == 0x20 ? 1 : bank == 0x30 ? 2 : -1;
  }

  static size_t lock_index(uint32_t address) {
    uint32_t block = address >> 16;
    return block == 0     ? address >> 12
           : block == 255 ? 270 + ((address & 0xffff) >> 12)
                          : 15 + block;
  }

  bool protected_region(uint32_t address, uint32_t size) const {
    if (registers[2] & 4) {
      for (uint32_t current = address; current < address + size;) {
        if (locks[lock_index(current)])
          return true;
        uint32_t unit =
            current < 65536 || current >= capacity - 65536 ? 4096 : 65536;
        current = (current & ~(unit - 1)) + unit;
      }
      return false;
    }
    unsigned bp = (registers[0] >> 2) & 7;
    // SEC=1, BP=110 is absent from 7.1.8/9: protect all, even with CMP=1.
    if ((registers[0] & 0x40) && bp == 6)
      return true;
    uint32_t protected_size = 0;
    if (bp == 7) {
      protected_size = capacity;
    } else if (bp) {
      if (registers[0] & 0x40) {
        protected_size = 4096u << std::min(bp - 1, 3u);
      } else {
        protected_size = 262144u << (bp - 1);
      }
    }
    uint32_t start = (registers[0] & 0x20) ? 0 : capacity - protected_size;
    if (registers[1] & 0x40)
      return address < start || address + size > start + protected_size;
    return address < start + protected_size && address + size > start;
  }

  void write_status() {
    if ((!wel && !volatile_write) || (registers[1] & 1) || !frame.count ||
        frame.count > (frame.opcode == 0x01 ? 2u : 1u))
      return;
    unsigned first = frame.opcode == 0x01 ? 0 : frame.opcode == 0x31 ? 1 : 2;
    auto values = registers;
    uint8_t mask = 0;
    for (unsigned i = 0; i < frame.count; ++i) {
      unsigned index = first + i;
      uint8_t value = frame.data[i];
      if (index == 0)
        value &= 0xfc;
      else if (index == 1)
        value = (value & 0x79) | 0x02 | (registers[1] & 0x39);
      else
        value &= 0x64;
      values[index] = value;
      mask |= 1 << index;
    }
    if (volatile_write) {
      registers = values;
      // LB bits are permanently OTP-locked (7.1.3), even via 50h (8.2.5).
      nonvolatile_registers[1] |= registers[1] & 0x38;
      volatile_write = false;
    } else {
      pending.operation = Operation::WriteRegisters;
      pending.registers = values;
      pending.register_mask = mask;
      pending.deadline = now() + Microseconds(10000);
    }
  }

  bool overlaps_suspended(uint32_t address, uint32_t size) const {
    if (!has_suspended())
      return false;
    // 8.2.19 permits access to other sectors/blocks, not the same sector.
    bool program = suspended.operation == Operation::Program;
    uint32_t start = program ? suspended.address & ~4095u : suspended.address;
    uint32_t length = program ? 4096 : suspended.size;
    return address < start + length && address + size > start;
  }

  void program() {
    if (!wel || frame.phase != Phase::Input || !frame.count)
      return;
    bool secure = frame.opcode == 0x42;
    int bank = security_bank(frame.address);
    uint32_t page = frame.address & ~(page_size - 1);
    if (secure) {
      if (bank < 0 || (registers[1] & (8 << bank)))
        return;
    } else if (protected_region(page, page_size) ||
               overlaps_suspended(page, page_size)) {
      return;
    }
    pending.operation =
        secure ? Operation::ProgramSecurity : Operation::Program;
    pending.address = secure ? bank * page_size : page;
    pending.size = page_size;
    pending.data = frame.data;
    pending.written = frame.written;
    pending.deadline = now() + Microseconds(400);
  }

  void erase_security() {
    int bank = security_bank(frame.address);
    if (!wel || frame.length != 4 || frame.phase != Phase::Done || bank < 0 ||
        (registers[1] & (8 << bank)))
      return;
    pending.operation = Operation::EraseSecurity;
    pending.address = bank * page_size;
    pending.size = page_size;
    pending.deadline = now() + Microseconds(45000);
  }

  void erase(uint32_t size, Microseconds duration) {
    if (!wel || frame.phase != Phase::Done ||
        frame.length != (size == capacity ? 1u : 4u) ||
        protected_region(frame.address & ~(size - 1), size) ||
        overlaps_suspended(frame.address & ~(size - 1), size))
      return;
    pending.operation = Operation::Erase;
    pending.address = frame.address & ~(size - 1);
    pending.size = size;
    pending.deadline = now() + duration;
  }

  void suspend() {
    if (has_suspended() || now() < resuspend_after ||
        !(pending.operation == Operation::Program ||
          (pending.operation == Operation::Erase && pending.size < capacity)))
      return;
    suspended = pending;
    suspended.remaining = pending.deadline - now();
    pending = Pending{};
    suspend_transition = true;
    suspend_until = now() + Microseconds(20);
  }

  void resume() {
    if (!has_suspended() || busy())
      return;
    pending = suspended;
    pending.deadline = now() + suspended.remaining;
    suspended = Pending{};
    resuspend_after = now() + Microseconds(20);
  }

  void reset() {
    pending = Pending{};
    suspended = Pending{};
    suspend_transition = false;
    resuspend_after = Clock::time_point::min();
    // SRL is a power-supply lock-down (7.1.1), not a software-reset unlock.
    uint8_t srl = registers[1] & 1;
    registers = nonvolatile_registers;
    registers[1] |= srl;
    locks.fill(true);
    wel = false;
    volatile_write = false;
    reset_enabled = false;
    sleeping = false;
    wrap = 0x10;
    available_at = now() + Microseconds(30);
  }

  void deselect() {
    update();
    if (frame.phase == Phase::Ignore || frame.phase == Phase::Command)
      return;
    switch (frame.opcode) {
    case 0x75:
      if (frame.length == 1)
        suspend();
      break;
    case 0x7a:
      if (frame.length == 1)
        resume();
      break;
    case 0x66:
      if (frame.length == 1)
        reset_enabled = true;
      break;
    case 0x99:
      if (frame.length == 1 && frame.reset_armed)
        reset();
      break;
    case 0xb9:
      if (frame.length == 1) {
        sleeping = true;
        available_at = now() + Microseconds(3);
      }
      break;
    case 0xab:
      if (sleeping && (frame.length == 1 || frame.length >= 4)) {
        sleeping = false;
        available_at =
            now() + std::chrono::nanoseconds(frame.length == 1 ? 3000 : 1800);
      }
      break;
    case 0x06:
    case 0x04:
      if (frame.length == 1) {
        wel = frame.opcode == 0x06;
        volatile_write = false;
      }
      break;
    case 0x50:
      if (frame.length == 1)
        volatile_write = true;
      break;
    case 0x01:
    case 0x31:
    case 0x11:
      write_status();
      break;
    case 0x02:
    case 0x32:
    case 0x42:
      program();
      break;
    case 0x44:
      erase_security();
      break;
    case 0x36:
    case 0x39:
      if (wel && frame.length == 4 && frame.phase == Phase::Done) {
        locks[lock_index(frame.address)] = frame.opcode == 0x36;
        wel = false;
      }
      break;
    case 0x7e:
    case 0x98:
      if (wel && frame.length == 1) {
        locks.fill(frame.opcode == 0x7e);
        wel = false;
      }
      break;
    case 0x77:
      if (frame.length == 5 && frame.count == 1)
        wrap = frame.data[0] & 0x70;
      break;
    case 0x20:
      erase(4096, Microseconds(45000));
      break;
    case 0x52:
      erase(32768, Microseconds(120000));
      break;
    case 0xd8:
      erase(65536, Microseconds(150000));
      break;
    case 0xc7:
    case 0x60:
      erase(capacity, Microseconds(40000000));
      break;
    default:
      break;
    }
  }
};

static int fake_spi_master_probe(struct device *dev) {
  if (dev->platform_data)
    return -EBUSY;
  unsigned baud = 10000000;
  if ((dev->fdt && get_prop_uint32_default(dev, "default-baud", &baud, baud)) ||
      !baud)
    return -EINVAL;
  try {
    auto *master = new FakeSpiMaster;
    master->bus->baud = std::min(baud, 133000000u);
    dev->platform_data = master;
  } catch (const std::bad_alloc &) {
    return -ENOMEM;
  }
  dev_info(dev, "Probing fake SPI master driver");
  return 0;
}

static int fake_spi_chip_probe(struct device *dev) {
  if (dev->platform_data)
    return -EBUSY;
  if (!dev->parent || !dev->parent->drv ||
      dev->parent->drv->probe != fake_spi_master_probe)
    return -ENODEV;
  auto *parent = static_cast<FakeSpiMaster *>(dev->parent->platform_data);
  if (!parent)
    return EPROBE_DEFER;
  try {
    dev->platform_data = new FakeNor(parent->bus);
  } catch (const std::bad_alloc &) {
    return -ENOMEM;
  }
  dev_info(dev, "Probing W25Q128JV-IQ fake SPI flash");
  return 0;
}

static void fake_spi_master_remove(struct device *dev) {
  auto *master = static_cast<FakeSpiMaster *>(dev->platform_data);
  if (!master)
    return;
  {
    std::lock_guard<std::mutex> lock(master->bus->mutex);
    master->bus->enabled = false;
    master->bus->selected = nullptr;
  }
  delete master;
  dev->platform_data = nullptr;
}

static void fake_spi_chip_remove(struct device *dev) {
  auto *chip = static_cast<FakeNor *>(dev->platform_data);
  if (!chip)
    return;
  {
    std::lock_guard<std::mutex> lock(chip->bus->mutex);
    if (chip->bus->selected == chip)
      chip->bus->selected = nullptr;
  }
  delete chip;
  dev->platform_data = nullptr;
}

static void fake_spi_set_baud(const struct device *dev, uint32_t baud) {
  auto *master = static_cast<FakeSpiMaster *>(dev->platform_data);
  if (!master || !baud)
    return;
  std::lock_guard<std::mutex> lock(master->bus->mutex);
  master->bus->baud = std::min(baud, uint32_t(133000000));
}

static uint32_t fake_spi_get_baud(const struct device *dev) {
  auto *master = static_cast<FakeSpiMaster *>(dev->platform_data);
  if (!master)
    return 0;
  std::lock_guard<std::mutex> lock(master->bus->mutex);
  return master->bus->baud;
}

static int fake_spi_transfer(const struct device *dev, const void *tx, void *rx,
                             uint32_t len) {
  auto *master = static_cast<FakeSpiMaster *>(dev->platform_data);
  if (!master)
    return -ENODEV;
  if (len > INT_MAX)
    return -EINVAL;
  std::lock_guard<std::mutex> lock(master->bus->mutex);
  if (!master->bus->enabled)
    return -ENODEV;
  const auto *input = static_cast<const uint8_t *>(tx);
  auto *output = static_cast<uint8_t *>(rx);
  for (uint32_t i = 0; i < len; ++i) {
    // Reads clock zeroes on MOSI, just like spidrv_master_read_nbytes().
    uint8_t byte = master->bus->selected
                       ? master->bus->selected->transfer(input ? input[i] : 0)
                       : 0xff;
    if (output)
      output[i] = byte;
  }
  return static_cast<int>(len);
}

static int fake_spi_read(const struct device *dev, void *buf, uint32_t len) {
  if (!buf && len)
    return -EINVAL;
  return fake_spi_transfer(dev, nullptr, buf, len);
}

static int fake_spi_write(const struct device *dev, const void *buf,
                          uint32_t len) {
  if (!buf && len)
    return -EINVAL;
  return fake_spi_transfer(dev, buf, nullptr, len);
}

static void fake_spi_assert_cs(const struct device *dev) {
  auto *chip = static_cast<FakeNor *>(dev->platform_data);
  if (!chip)
    return;
  std::lock_guard<std::mutex> lock(chip->bus->mutex);
  if (chip->bus->enabled && !chip->bus->selected) {
    chip->select();
    chip->bus->selected = chip;
  }
}

static void fake_spi_deassert_cs(const struct device *dev) {
  auto *chip = static_cast<FakeNor *>(dev->platform_data);
  if (!chip)
    return;
  std::lock_guard<std::mutex> lock(chip->bus->mutex);
  if (chip->bus->selected == chip) {
    chip->deselect();
    chip->bus->selected = nullptr;
  }
}

static const struct driver_fdt_match_id fake_spi_master_ids[] = {
    {.compatible = "fake,spi-master"},
    {},
};

static const struct driver_fdt_match_id fake_spi_chip_ids[] = {
    {.compatible = "fake,spi-chip"},
    {},
};

static struct spi_master_driver fake_spi_master_driver = {
    .drv =
        {
            .name = "fake-spi-master",
            .probe = fake_spi_master_probe,
            .remove = fake_spi_master_remove,
            .match_table = fake_spi_master_ids,
        },
    .set_baud = fake_spi_set_baud,
    .get_baud = fake_spi_get_baud,
    .read_nbytes = fake_spi_read,
    .write_nbytes = fake_spi_write,
};

static struct spi_chip_driver fake_spi_chip_driver = {
    .drv =
        {
            .name = "fake-spi-chip",
            .probe = fake_spi_chip_probe,
            .remove = fake_spi_chip_remove,
            .match_table = fake_spi_chip_ids,
        },
    .assert_cs = fake_spi_assert_cs,
    .deassert_cs = fake_spi_deassert_cs,
};

PLATFORM_DRIVER(fake_spi_master_driver);
PLATFORM_DRIVER(fake_spi_chip_driver);