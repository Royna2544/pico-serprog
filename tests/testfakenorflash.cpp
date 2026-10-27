// Run from the repository root:
// clang++ -std=c++17 -pthread -I. -fsanitize=address,undefined \
//   tests/testfakenorflash.cpp -o build-host/testfakenorflash
#include "../drivers/fakenorflash.cpp"

#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <random>
#include <vector>

extern "C" bool driver_register(struct driver *) { return true; }
extern "C" void printk(int, const char *, ...) {}
static unsigned property_baud = 7000000;
static unsigned property_error = 0;
extern "C" unsigned get_prop_uint32(struct device *, const char *, unsigned *out,
                                    unsigned) {
  *out = property_baud;
  return property_error;
}

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

static FakeNor::Clock::time_point virtual_time(std::chrono::seconds(1));
static FakeNor::Clock::time_point test_now() { return virtual_time; }

struct Fixture {
  struct device master = {};
  struct device chip = {};

  explicit Fixture(bool settled = true) {
    master.name = "test-master";
    master.drv = &fake_spi_master_driver.drv;
    chip.name = "test-flash";
    chip.drv = &fake_spi_chip_driver.drv;
    chip.parent = &master;
    require(master.drv->probe(&master) == 0, "master probe failed");
    static_cast<FakeSpiMaster *>(master.platform_data)->bus->now = test_now;
    require(chip.drv->probe(&chip) == 0, "chip probe failed");
    if (settled)
      advance(5000);
  }

  ~Fixture() {
    if (chip.drv->remove)
      chip.drv->remove(&chip);
    if (master.drv->remove)
      master.drv->remove(&master);
  }

  void select() { fake_spi_chip_driver.assert_cs(&chip); }
  void deselect() { fake_spi_chip_driver.deassert_cs(&chip); }

  void write(const std::vector<uint8_t> &bytes) {
    require(fake_spi_master_driver.write_nbytes(&master, bytes.data(),
                                                bytes.size()) ==
                static_cast<int>(bytes.size()),
            "SPI write failed");
  }

  std::vector<uint8_t> read(size_t size) {
    std::vector<uint8_t> result(size);
    require(fake_spi_master_driver.read_nbytes(&master, result.data(), size) ==
                static_cast<int>(size),
            "SPI read failed");
    return result;
  }

  std::vector<uint8_t> command(const std::vector<uint8_t> &bytes,
                               size_t size = 0) {
    select();
    write(bytes);
    auto result = read(size);
    deselect();
    return result;
  }

  uint8_t status(uint8_t opcode = 0x05) { return command({opcode}, 1)[0]; }

  void ready() {
    advance(201000000);
    require(!(status() & 1), "BUSY never cleared");
  }

  void advance(uint64_t us) { virtual_time += FakeNor::Microseconds(us); }

  void reset() {
    command({0x66});
    command({0x99});
    advance(30);
  }

  std::vector<uint8_t> read_array(uint32_t address, size_t size) {
    return command({0x03, static_cast<uint8_t>(address >> 16),
                    static_cast<uint8_t>(address >> 8),
                    static_cast<uint8_t>(address)}, size);
  }

  void program(uint32_t address, const std::vector<uint8_t> &data) {
    command({0x06});
    select();
    write({0x02, static_cast<uint8_t>(address >> 16),
           static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)});
    write(data);
    deselect();
    ready();
  }

  void erase(uint8_t opcode, uint32_t address) {
    command({0x06});
    command({opcode, static_cast<uint8_t>(address >> 16),
             static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)});
    ready();
  }

  void write_status(uint8_t opcode, std::vector<uint8_t> data,
                    bool temporary = false) {
    command({temporary ? uint8_t(0x50) : uint8_t(0x06)});
    data.insert(data.begin(), opcode);
    command(data);
    if (!temporary)
      ready();
  }
};

static void test_jedec_id() {
  Fixture f;
  f.select();
  f.write({0x9f});
  require(f.read(1) == std::vector<uint8_t>({0xef}),
          "JEDEC ID must be EF 40 18, not echoed MOSI bytes");
  require(f.read(2) == std::vector<uint8_t>({0x40, 0x18}),
          "JEDEC ID must continue across read calls");
  f.deselect();
  require(f.command({0x9f}, 3) == std::vector<uint8_t>({0xef, 0x40, 0x18}),
          "CS must restart the JEDEC ID stream");
}

static void test_page_program() {
  Fixture f;
  require(f.status() == 0, "power-on BUSY and WEL must be clear");
  require(f.read_array(0x12fe, 4) == std::vector<uint8_t>(4, 0xff),
          "array must power on erased");
  f.command({0x02, 0, 0x12, 0xff, 0});
  require(f.read_array(0x12ff, 1)[0] == 0xff, "program requires WEL");
  f.command({0x06});
  require(f.status() == 2, "Write Enable must set WEL");
  f.select();
  f.write({0x02});
  f.write({0, 0x12});
  f.write({0xff, 0xf0, 0x0f});
  f.deselect();
  require((f.status() & 1) != 0, "program must enter BUSY on CS rising");
  require(f.command({0x9f}, 3) == std::vector<uint8_t>(3, 0xff),
          "non-status commands must be ignored while BUSY");
  f.ready();
  require(f.status() == 0, "program completion must clear WEL and BUSY");
  require(f.read_array(0x12ff, 2) == std::vector<uint8_t>({0xf0, 0xff}),
          "page program must not cross to the next page");
  require(f.read_array(0x1200, 1)[0] == 0x0f, "page program must wrap");
  f.program(0x12ff, {0xaa});
  require(f.read_array(0x12ff, 1)[0] == 0xa0, "NOR programming is bitwise AND");
  std::vector<uint8_t> data(257, 0);
  data[256] = 0xf5;
  f.program(0x20fe, data);
  require(f.read_array(0x20fe, 1)[0] == 0xf5,
          "last byte overwrites the wrapped page buffer, not AND with old input");
}

static void test_erase_and_array_wrap() {
  Fixture f;
  f.program(0, {0xa5});
  f.program(0xffffff, {0x5a});
  require(f.read_array(0xffffff, 2) == std::vector<uint8_t>({0x5a, 0xa5}),
          "sequential reads must wrap at 16 MiB");
  for (uint32_t address : {0xfff, 0x1000, 0x7fff, 0x8000, 0xffff, 0x10000})
    f.program(address, {0});
  f.command({0x20, 0, 0x0f, 0xfe});
  require(f.read_array(0xfff, 1)[0] == 0, "erase requires Write Enable");
  f.erase(0x20, 0xffe);
  require(f.read_array(0xfff, 2) == std::vector<uint8_t>({0xff, 0}),
          "sector erase must align down to 4 KiB");
  f.erase(0x52, 0x1234);
  require(f.read_array(0x7fff, 2) == std::vector<uint8_t>({0xff, 0}),
          "32 KiB erase must stay in the addressed block");
  f.erase(0xd8, 0xabcd);
  require(f.read_array(0xffff, 2) == std::vector<uint8_t>({0xff, 0}),
          "64 KiB erase must stay in the addressed block");
  for (uint8_t opcode : {0xc7, 0x60}) {
    f.program(0x10000, {0});
    f.command({0x06});
    f.command({opcode});
    require(f.status() & 1, "chip erase must set BUSY");
    f.ready();
    require(f.read_array(0x10000, 1)[0] == 0xff &&
                f.read_array(0xffffff, 1)[0] == 0xff,
            "both chip erase opcodes must erase the complete array");
  }
}

static void test_status_register_writes() {
  Fixture f;
  f.command({0x01, 0x1c});
  require(f.status() == 0, "status writes require a write-enable latch");
  f.write_status(0x01, {0x03});
  require(f.status() == 0, "BUSY and WEL are read-only");
  f.write_status(0x01, {0x04}, true);
  require(f.status() == 4, "50h must write volatile status without BUSY or WEL");
  f.write_status(0x01, {0, 0x40}, true);
  require(f.status() == 0 && f.status(0x35) == 0x42,
          "01h must support the two-register form and fixed IQ QE=1");
  f.write_status(0x01, {0x08}, true);
  require(f.status(0x35) == 0x42, "one-byte 01h must leave SR2 untouched");
  f.write_status(0x31, {0xbc});
  require(f.status(0x35) == 0x3a, "SR2 must mask SUS/reserved bits and keep QE=1");
  f.write_status(0x31, {0});
  require(f.status(0x35) == 0x3a, "security OTP locks cannot be cleared");
  f.write_status(0x11, {0xff});
  require(f.status(0x15) == 0x64, "SR3 only implements DRV[1:0] and WPS");
  require(f.command({0x15}, 4) == std::vector<uint8_t>(4, 0x64),
          "status bytes must repeat continuously");
  f.write_status(0x31, {0x03});
  f.write_status(0x01, {0x1c});
  require((f.status() & 0xfc) == 8, "SRL must lock all status registers");
}

static void test_block_protection_table() {
  Fixture f;
  const uint32_t sizes[2][8] = {
      {0, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000},
      {0, 0x1000, 0x2000, 0x4000, 0x8000, 0x8000, 0x1000000, 0x1000000}};
  for (unsigned sec = 0; sec < 2; ++sec)
    for (unsigned tb = 0; tb < 2; ++tb)
      for (unsigned cmp = 0; cmp < 2; ++cmp)
        for (unsigned bp = 0; bp < 8; ++bp) {
          if (sec && bp == 6) // Undefined row: tested separately as model policy.
            continue;
          f.write_status(0x01, {0, 2}, true);
          f.command({0x06});
          f.command({0xc7});
          f.ready();
          f.write_status(0x01, {static_cast<uint8_t>((sec << 6) | (tb << 5) |
                                                    (bp << 2)),
                                static_cast<uint8_t>(2 | (cmp << 6))}, true);
          uint32_t size = sizes[sec][bp];
          uint32_t start = tb ? 0 : 0x1000000 - size;
          std::vector<uint32_t> points = {0, 0xffffff};
          if (start && start < 0x1000000) {
            points.push_back(start - 1);
            points.push_back(start);
          }
          if (size && size < 0x1000000) {
            points.push_back(size - 1);
            points.push_back(size);
          }
          for (uint32_t address : points) {
            bool protected_byte = (address >= start && address < start + size);
            if (cmp)
              protected_byte = !protected_byte;
            f.program(address, {0});
            if (f.read_array(address, 1)[0] != (protected_byte ? 0xff : 0))
              throw std::runtime_error("protection table mismatch: SEC=" +
                                       std::to_string(sec) + " TB=" +
                                       std::to_string(tb) + " CMP=" +
                                       std::to_string(cmp) + " BP=" +
                                       std::to_string(bp));
          }
        }
  f.write_status(0x01, {0, 2}, true);
  f.program(0xff0000, {0});
  f.write_status(0x01, {0x44}, true); // Top 4 KiB.
  f.erase(0xd8, 0xff0000);
  require(f.read_array(0xff0000, 1)[0] == 0,
          "erase overlapping any protected byte must be rejected completely");
  f.command({0x06});
  f.command({0x60});
  require(!(f.status() & 1), "chip erase must reject any array protection");
}

static void test_fast_reads_and_wrap() {
  Fixture f;
  std::vector<uint8_t> page(256);
  for (unsigned i = 0; i < page.size(); ++i)
    page[i] = static_cast<uint8_t>(i);
  f.program(0x800, page);
  // Dummy fields are logical lane-wide bytes from table 8.1.3, not MOSI clocks.
  const std::vector<std::vector<uint8_t>> headers = {
      {0x0b, 0, 8, 0, 0}, {0x3b, 0, 8, 0, 0, 0},
      {0x6b, 0, 8, 0, 0, 0, 0, 0}, {0xbb, 0, 8, 0, 0xf0},
      {0xeb, 0, 8, 0, 0xf0, 0, 0}};
  for (const auto &header : headers)
    require(f.command(header, 256) == page, "fast-read data/dummy phase mismatch");
  f.command({0x06});
  f.command({0x32, 0, 9, 0, 0x12, 0x34});
  f.ready();
  require(f.read_array(0x900, 2) == std::vector<uint8_t>({0x12, 0x34}),
          "quad page program must use NOR programming rules");
  for (unsigned shift = 0; shift < 4; ++shift) {
    uint32_t length = 8u << shift;
    f.command({0x77, 0, 0, 0, static_cast<uint8_t>(shift << 5)});
    std::vector<uint8_t> expected(length * 2);
    for (unsigned i = 0; i < expected.size(); ++i)
      expected[i] = static_cast<uint8_t>((length - 1 + i) % length);
    require(f.command({0xeb, 0, 8, static_cast<uint8_t>(length - 1), 0xf0, 0, 0},
                       expected.size()) == expected,
            "EBh must wrap within the selected 8/16/32/64-byte burst");
    require(f.read_array(0x807, 2) == std::vector<uint8_t>({7, 8}),
            "wrap setting must not affect ordinary reads");
  }
  f.command({0x77, 0, 0, 0, 0x10});
  require(f.command({0xeb, 0, 8, 7, 0xf0, 0, 0}, 2) ==
              std::vector<uint8_t>({7, 8}), "W4=1 must disable wrap");
}

static void test_legacy_and_unique_ids() {
  Fixture f;
  for (const auto &header : std::vector<std::vector<uint8_t>>{
           {0x90, 0, 0, 0}, {0x92, 0, 0, 0, 0xf0},
           {0x94, 0, 0, 0, 0xf0, 0, 0}})
    require(f.command(header, 4) == std::vector<uint8_t>({0xef, 0x17, 0xef, 0x17}),
            "legacy IDs must alternate EF/17 on each supported bus width");
  require(f.command({0xab, 0, 0, 0}, 3) == std::vector<uint8_t>(3, 0x17),
          "ABh device ID must repeat after three dummy bytes");
  auto id = f.command({0x4b, 0, 0, 0, 0}, 8);
  require(id != std::vector<uint8_t>(8, 0xff) && id != std::vector<uint8_t>(8, 0),
          "unique ID must be a stable, non-empty emulated identifier");
  require(f.command({0x4b, 0, 0, 0, 0}, 8) == id,
          "unique ID must not change between transactions");
}

static void test_security_registers() {
  Fixture f;
  for (uint8_t bank : {0x10, 0x20, 0x30}) {
    f.command({0x06});
    f.command({0x42, 0, bank, 0xfe, 0xf0, 0x0f, 0x55});
    require(f.status() & 1, "security programming must set BUSY");
    f.ready();
    require(f.command({0x48, 0, bank, 0xfe, 0}, 3) ==
                std::vector<uint8_t>({0xf0, 0x0f, 0x55}),
            "security reads/programming must wrap within their 256-byte bank");
    require(f.read_array(uint32_t(bank) << 8, 1)[0] == 0xff,
            "security registers must be separate from the main array");
    f.command({0x06});
    f.command({0x44, 0, bank, 0x7b});
    f.ready();
    require(f.command({0x48, 0, bank, 0xfe, 0}, 3) == std::vector<uint8_t>(3, 0xff),
            "security erase must affect the selected bank only");
    f.command({0x06});
    f.command({0x42, 0, bank, 0, 0xa5});
    f.ready();
    f.write_status(0x31, {static_cast<uint8_t>(2 | (1 << (bank / 16 + 2)))});
    f.command({0x06});
    f.command({0x44, 0, bank, 0});
    f.command({0x42, 0, bank, 0, 0});
    f.ready();
    require(f.command({0x48, 0, bank, 0, 0}, 1)[0] == 0xa5,
            "LB1/LB2/LB3 must prevent both security erase and programming");
  }
  for (uint8_t bank : {0, 0x11, 0x40}) {
    f.command({0x06});
    f.command({0x42, 0, bank, 0, 0});
    require(!(f.status() & 1), "invalid security banks must not alias a real bank");
    require(f.command({0x48, 0, bank, 0, 0}, 1)[0] == 0xff,
            "invalid security reads must return an undriven bus");
  }
}

static void test_individual_locks() {
  Fixture f;
  f.write_status(0x11, {0x64});
  require(f.command({0x3d, 0, 0, 0}, 1)[0] & 1,
          "all individual locks must power on set");
  f.program(0x100, {0});
  require(f.read_array(0x100, 1)[0] == 0xff, "WPS=1 must select individual locks");
  f.command({0x04});
  f.command({0x39, 0, 0, 0});
  require(f.command({0x3d, 0, 0, 0}, 1)[0] & 1, "unlock requires WEL");
  f.command({0x06});
  f.command({0x39, 0, 0, 0});
  require(!(f.command({0x3d, 0, 0, 0}, 1)[0] & 1), "unlock must clear lock");
  require(f.command({0x3d, 0, 0x10, 0}, 1)[0] & 1,
          "bottom 64 KiB must have separate 4 KiB locks");
  f.program(0x100, {0x55});
  require(f.read_array(0x100, 1)[0] == 0x55, "unlocked sector must be writable");
  f.command({0x06});
  f.command({0x39, 1, 0x23, 0x45});
  require(!(f.command({0x3d, 1, 0xff, 0xff}, 1)[0] & 1),
          "interior block lock must cover its whole 64 KiB");
  require(f.command({0x3d, 2, 0, 0}, 1)[0] & 1,
          "adjacent interior block must remain locked");
  f.command({0x06});
  f.command({0x39, 0xff, 0xf0, 0});
  require(!(f.command({0x3d, 0xff, 0xff, 0xff}, 1)[0] & 1) &&
              (f.command({0x3d, 0xff, 0xef, 0xff}, 1)[0] & 1),
          "top 64 KiB must have separate 4 KiB locks");
  f.command({0x06});
  f.command({0x36, 0, 0, 0});
  require(f.command({0x3d, 0, 0, 0}, 1)[0] & 1, "36h must relock a sector");
  f.command({0x06});
  f.command({0x98});
  for (unsigned address = 0; address < 0x1000000; address += 4096)
    require(!(f.command({0x3d, static_cast<uint8_t>(address >> 16),
                        static_cast<uint8_t>(address >> 8), 0}, 1)[0] & 1),
            "global unlock must clear every block/sector lock");
  f.command({0x06});
  f.command({0x7e});
  f.erase(0x20, 0);
  require(f.read_array(0x100, 1)[0] == 0x55,
          "global lock must protect previously unlocked sectors");
}

static void test_power_down_and_reset() {
  Fixture f;
  f.program(0x123, {0x55});
  f.write_status(0x01, {4});
  f.write_status(0x11, {0x20});
  f.write_status(0x01, {0x28}, true);
  f.write_status(0x11, {0x44}, true);
  f.command({0x06});
  f.command({0x99});
  require(f.status() == 0x2a, "99h alone must not reset the chip");
  f.command({0x66});
  f.command({0x05}, 1);
  f.command({0x99});
  require(f.status() == 0x2a, "an intervening command must cancel reset enable");
  f.command({0x66});
  f.command({0x99});
  require(f.status() == 0xff, "all commands must be ignored during tRST");
  f.advance(29);
  require(f.status() == 0xff, "reset recovery must last 30 microseconds");
  f.advance(1);
  require(f.status() == 4 && f.status(0x15) == 0x20,
          "reset must restore nonvolatile status and clear WEL");
  require(f.read_array(0x123, 1)[0] == 0x55, "reset must preserve completed data");
  f.command({0x06});
  f.command({0x39, 0, 0, 0});
  f.reset();
  require(f.command({0x3d, 0, 0, 0}, 1)[0] & 1, "reset must restore all locks");
  f.command({0xb9});
  f.advance(3);
  require(f.status() == 0xff, "status reads must be ignored in power-down");
  f.command({0x06});
  f.reset();
  require(f.command({0x9f}, 3) == std::vector<uint8_t>(3, 0xff),
          "software reset must not exit deep power-down");
  require(f.command({0xab, 0, 0, 0}, 2) == std::vector<uint8_t>(2, 0x17),
          "ABh must be usable for identification while asleep");
  require(f.status() == 0xff, "wake-up recovery must block new commands");
  f.advance(2);
  require(f.status() == 4, "release must restore standby without setting WEL");
  f.command({0xb9});
  f.advance(3);
  f.command({0xab});
  f.advance(2);
  require(f.status() == 0xff, "ABh without ID needs the full tRES1");
  f.advance(1);
  require(f.status() == 4, "ABh without ID must wake after tRES1");
  f.write_status(0x31, {3}, true);
  f.reset();
  f.write_status(0x01, {0});
  require((f.status() & 0xfc) == 4,
          "software reset must not defeat SRL power-supply lock-down");
}

static void test_suspend_resume() {
  Fixture f;
  f.program(0, {0});
  f.program(0x2000, {0x33});
  f.command({0x06});
  f.command({0x20, 0, 0, 0});
  f.command({0x75});
  f.advance(20);
  require(!(f.status() & 1) && (f.status(0x35) & 0x80),
          "erase suspend must clear BUSY and set SUS within tSUS");
  require(f.read_array(0x2000, 1)[0] == 0x33, "other sectors must remain readable");
  require(f.read_array(0, 1)[0] == 0xff, "suspended array region must not be read");
  f.program(0x2001, {0xa5});
  require(f.status(0x35) & 0x80, "foreground program must preserve erase suspension");
  require(f.read_array(0x2001, 1)[0] == 0xa5,
          "other sectors must be programmable during erase suspend");
  f.write_status(0x01, {0x1c}, true);
  require((f.status() & 0xfc) == 0, "status writes must be ignored during suspend");
  f.erase(0x20, 0x2000);
  require(f.read_array(0x2000, 1)[0] == 0x33, "erase is forbidden during erase suspend");
  f.command({0x7a});
  require((f.status() & 1) && !(f.status(0x35) & 0x80),
          "resume must restore BUSY and clear SUS");
  f.command({0x75});
  require(!(f.status(0x35) & 0x80), "suspend before tSUS after resume must be ignored");
  f.advance(20);
  f.command({0x75});
  f.advance(20);
  require(!(f.status() & 1) && (f.status(0x35) & 0x80), "resuspension must work");
  f.command({0x7a});
  f.ready();
  require(f.read_array(0, 1)[0] == 0xff && f.read_array(0x2001, 1)[0] == 0xa5,
          "resumed erase must commit only its original region");

  f.command({0x06});
  f.command({0x02, 0, 0x10, 0, 0x5a});
  f.command({0x75});
  f.advance(20);
  f.program(0x3000, {0});
  require(f.read_array(0x3000, 1)[0] == 0xff,
          "program is forbidden during program suspend");
  f.erase(0x20, 0x2000);
  require(f.status(0x35) & 0x80, "other sector erase must preserve program suspend");
  require(f.read_array(0x2000, 1)[0] == 0xff,
          "other sectors must be erasable during program suspend");
  f.erase(0x20, 0x1000);
  require(!(f.status() & 1), "erase overlapping a suspended program must be rejected");
  f.command({0x7a});
  f.ready();
  require(f.read_array(0x1000, 1)[0] == 0x5a, "resume must complete original program");
  f.command({0x06});
  f.command({0x60});
  f.command({0x75});
  f.advance(20);
  require((f.status() & 1) && !(f.status(0x35) & 0x80), "chip erase is not suspendable");
  f.reset();
  require(!(f.status() & 1) && !(f.status(0x35) & 0x80),
          "reset must abort even a busy internal operation");
  require(f.read_array(0x1000, 1)[0] == 0x5a,
          "aborted operations must not commit their deferred buffers");
}

static void test_sfdp() {
  Fixture f;
  auto bytes = f.command({0x5a, 0, 0, 0, 0}, 256);
  require(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 4) ==
              std::vector<uint8_t>({'S', 'F', 'D', 'P'}),
          "5Ah must expose an SFDP signature, not main flash or an empty bus");
  require(bytes[4] == 0 && bytes[5] == 1 && bytes[6] == 0,
          "emulator SFDP must describe one JESD216 1.0 parameter table");
  require(bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 1 && bytes[11] == 9,
          "SFDP BFPT header must have the correct ID, revision and length");
  size_t base = bytes[12] | (size_t(bytes[13]) << 8) | (size_t(bytes[14]) << 16);
  require(base + 36 <= bytes.size(), "SFDP table pointer must stay within the register");
  auto dword = [&](size_t offset) {
    return uint32_t(bytes[base + offset]) | (uint32_t(bytes[base + offset + 1]) << 8) |
           (uint32_t(bytes[base + offset + 2]) << 16) |
           (uint32_t(bytes[base + offset + 3]) << 24);
  };
  require(dword(4) == 0x07ffffff, "SFDP density must report 128 Mbit");
  require(dword(28) == 0x520f200c && (dword(32) & 0xffff) == 0xd810,
          "SFDP must describe the implemented 4/32/64 KiB erase commands");
  require(f.command({0x5a, 0, 0, 0xff, 0}, 2) ==
              std::vector<uint8_t>({0xff, 'S'}), "SFDP address counter must wrap at 256");
  require(f.command({0x5a, 1, 0, 0, 0}, 4) == std::vector<uint8_t>(4, 0xff),
          "SFDP requires A23:A8=0 and must not alias invalid addresses");
}

static void test_power_up_timing() {
  Fixture f(false);
  require(f.command({0x9f}, 3) == std::vector<uint8_t>(3, 0xff),
          "power-up must inhibit reads until tVSL");
  f.advance(20);
  require(f.command({0x9f}, 3) == std::vector<uint8_t>({0xef, 0x40, 0x18}),
          "reads must become available at tVSL");
  f.command({0x06});
  require(f.status() == 0, "Write Enable must be inhibited before tPUW");
  f.advance(4979);
  f.command({0x06});
  require(f.status() == 0, "write inhibit must last the full 5 ms");
  f.advance(1);
  f.command({0x06});
  require(f.status() == 2, "Write Enable must become available after tPUW");
}

static void test_framing_and_transport() {
  Fixture f;
  require(f.read(4) == std::vector<uint8_t>(4, 0xff), "deselected bus must read high");
  f.write({0x06});
  require(f.status() == 0, "deselected writes must be ignored");
  for (const auto &invalid : std::vector<std::vector<uint8_t>>{
           {0x06, 0}, {0xff, 0x06}, {0}, {0x01}, {0x31}}) {
    f.command(invalid);
    require(f.status() == 0, "invalid frames must not change WEL/status");
  }
  f.command({0x06});
  for (const auto &invalid : std::vector<std::vector<uint8_t>>{
           {0x02}, {0x02, 0, 0}, {0x02, 0, 0, 0}, {0x20, 0, 0},
           {0x20, 0, 0, 0, 0}, {0xc7, 0}, {0x01, 0, 0, 0}}) {
    f.command(invalid);
    require(f.status() == 2, "incomplete/overlong writes must preserve WEL and data");
  }
  f.command({0x04});
  require(f.status() == 0, "Write Disable must clear WEL");
  f.program(0, {1, 2, 3, 4});
  f.select();
  f.write({0x03});
  f.select(); // No second falling edge: must not restart a frame.
  f.write({0, 0, 0});
  f.write({0, 0}); // Two read bytes discarded by the master's write API.
  require(f.read(2) == std::vector<uint8_t>({3, 4}),
          "write calls must clock and discard MISO, never queue stale responses");
  f.deselect();
  f.deselect();
  require(f.command({0x9f}, 3) == std::vector<uint8_t>({0xef, 0x40, 0x18}),
          "a terminated frame must not leak into the next transaction");
  uint8_t byte = 0;
  require(fake_spi_read(&f.master, nullptr, 1) == -EINVAL &&
              fake_spi_write(&f.master, nullptr, 1) == -EINVAL,
          "null nonempty buffers must be rejected");
  require(fake_spi_read(&f.master, &byte, uint32_t(INT_MAX) + 1) == -EINVAL &&
              fake_spi_write(&f.master, &byte, uint32_t(INT_MAX) + 1) == -EINVAL,
          "lengths that cannot be returned as int must be rejected");
  require(fake_spi_read(&f.master, nullptr, 0) == 0 &&
              fake_spi_write(&f.master, nullptr, 0) == 0,
          "zero-length transfers must be valid no-ops");
}

static void test_operation_timing() {
  Fixture f;
  f.command({0x06});
  f.command({0x02, 0, 0, 0, 0});
  f.select();
  f.write({0x05});
  f.advance(399);
  require(f.read(1)[0] & 1, "program BUSY must last tPP");
  f.advance(1);
  require(!(f.read(1)[0] & 3), "continuous status reads must notice time-based completion");
  f.deselect();
  f.command({0x06});
  f.command({0x11, 0x20});
  f.advance(9999);
  require(f.status() & 1, "nonvolatile status write must last tW");
  f.advance(1);
  require(!(f.status() & 3) && f.status(0x15) == 0x20, "status write must commit at tW");
  for (auto entry : std::vector<std::pair<uint8_t, uint64_t>>{
           {0x20, 45000}, {0x52, 120000}, {0xd8, 150000}, {0xc7, 40000000}}) {
    f.command({0x06});
    f.command(entry.first == 0xc7 ? std::vector<uint8_t>{entry.first}
                                 : std::vector<uint8_t>{entry.first, 0, 0, 0});
    f.advance(entry.second - 1);
    require(f.status() & 1, "erase BUSY must last its specified typical time");
    f.advance(1);
    require(!(f.status() & 3), "erase completion must clear BUSY and WEL");
  }
}

static void test_lifecycle_and_isolation() {
  Fixture first;
  Fixture second;
  first.program(0, {0xa5});
  require(second.read_array(0, 1)[0] == 0xff, "different masters must not share storage");
  require(first.command({0x4b, 0, 0, 0, 0}, 8) !=
              second.command({0x4b, 0, 0, 0, 0}, 8), "chip identities must be distinct");
  struct device chip = {};
  chip.name = "second-chip";
  chip.drv = &fake_spi_chip_driver.drv;
  chip.parent = &first.master;
  require(chip.drv->probe(&chip) == 0, "second chip probe failed");
  first.advance(5000);
  first.select();
  fake_spi_assert_cs(&chip); // Must not steal an active selection.
  fake_spi_deassert_cs(&chip);
  first.write({0x9f});
  require(first.read(3) == std::vector<uint8_t>({0xef, 0x40, 0x18}),
          "select/deassert on another chip must not corrupt the selected frame");
  first.deselect();
  fake_spi_assert_cs(&chip);
  first.write({0x03, 0, 0, 0});
  require(first.read(1)[0] == 0xff, "chips on one master must have independent storage");
  fake_spi_chip_remove(&chip);
  require(first.read(1)[0] == 0xff, "removing a selected chip must deselect it safely");
  fake_spi_chip_remove(&chip);
  require(fake_spi_master_probe(&first.master) == -EBUSY &&
              fake_spi_chip_probe(&first.chip) == -EBUSY, "double probe must be rejected");
  first.select();
  fake_spi_master_remove(&first.master);
  fake_spi_deassert_cs(&first.chip);
  require(fake_spi_read(&first.master, nullptr, 0) == -ENODEV,
          "removed masters must reject transfers");
  struct device invalid = {};
  require(fake_spi_chip_probe(&invalid) == -ENODEV, "chip probe requires a fake parent");
  invalid.parent = &first.master;
  require(fake_spi_chip_probe(&invalid) == EPROBE_DEFER, "unprobed parent must defer chip probe");
}

static void test_simple_bus_parent_rejected() {
  struct device parent = {};
  parent.name = "test-simple-bus";
  parent.drv = static_cast<const struct driver *>(SIMPLE_BUS_DRV);
  struct device chip = {};
  chip.name = "test-flash";
  chip.drv = &fake_spi_chip_driver.drv;
  chip.parent = &parent;
  require(chip.drv->probe(&chip) == -ENODEV,
          "chip probe must reject a simple-bus parent without dereferencing its sentinel");
  require(!chip.platform_data, "rejected chip probe must not create chip state");
}

static void test_concurrent_masters() {
  Fixture first;
  Fixture second;
  auto exercise = [](Fixture &f) {
    for (unsigned i = 0; i < 1000; ++i)
      require(f.command({0x9f}, 3) == std::vector<uint8_t>({0xef, 0x40, 0x18}),
              "independent masters must be concurrently usable");
  };
  std::thread a([&] { exercise(first); });
  std::thread b([&] { exercise(second); });
  a.join();
  b.join();
}

static void test_randomized_array_model() {
  Fixture f;
  std::mt19937 random(0x128);
  std::vector<uint8_t> reference(0x1000000, 0xff);
  for (unsigned round = 0; round < 300; ++round) {
    uint32_t address = random() & 0xffffff;
    if (round % 7) {
      std::vector<uint8_t> bytes(1 + random() % 800);
      std::array<uint8_t, 256> page;
      std::array<bool, 256> written = {};
      for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(random());
        unsigned column = (address + i) & 255;
        page[column] = bytes[i];
        written[column] = true;
      }
      f.program(address, bytes);
      for (unsigned i = 0; i < 256; ++i)
        if (written[i])
          reference[(address & ~255u) + i] &= page[i];
    } else {
      unsigned shift = 12 + (round % 3 == 0 ? 0 : round % 3 == 1 ? 3 : 4);
      uint32_t size = 1u << shift;
      f.erase(shift == 12 ? 0x20 : shift == 15 ? 0x52 : 0xd8, address);
      std::fill_n(reference.begin() + (address & ~(size - 1)), size, 0xff);
    }
    std::vector<uint8_t> expected(513);
    for (unsigned i = 0; i < expected.size(); ++i)
      expected[i] = reference[(address + i) & 0xffffff];
    require(f.read_array(address, expected.size()) == expected,
            "randomized command stream diverged from independent NOR reference model");
  }
  require(f.read_array(0, reference.size()) == reference,
          "full 16 MiB readback must match the independent reference model");
}

static void test_baud_configuration() {
  struct device dev = {};
  dev.name = "configured-master";
  dev.drv = &fake_spi_master_driver.drv;
  dev.fdt = &property_baud;
  require(fake_spi_master_probe(&dev) == 0, "configured master probe failed");
  uint32_t baud = fake_spi_get_baud(&dev);
  fake_spi_master_remove(&dev);
  require(baud == 7000000, "master probe must honor device-tree default-baud");
  property_baud = 200000000;
  require(fake_spi_master_probe(&dev) == 0, "high default baud probe failed");
  require(fake_spi_get_baud(&dev) == 133000000, "baud must clamp to datasheet FR");
  fake_spi_set_baud(&dev, 1000000);
  fake_spi_set_baud(&dev, 0);
  require(fake_spi_get_baud(&dev) == 1000000, "zero baud must not change the clock");
  fake_spi_master_remove(&dev);
  property_baud = 0;
  require(fake_spi_master_probe(&dev) == -EINVAL && !dev.platform_data,
          "zero default-baud must be rejected without allocating data");
  property_error = EINVAL;
  require(fake_spi_master_probe(&dev) == -EINVAL && !dev.platform_data,
          "malformed default-baud must be rejected without leaking data");
  property_error = 0;
}

static void test_volatile_otp_retention() {
  Fixture f;
  auto id = f.command({0x4b, 0, 0, 0, 0}, 8);
  f.write_status(0x31, {0x0a}, true);
  require(f.status(0x35) & 8, "volatile status writes must set LB1");
  f.reset();
  require(f.status(0x35) & 8, "reset must not make an OTP-locked security bank writable");
  f.command({0x06});
  f.command({0x42, 0, 0x10, 0, 0});
  f.ready();
  require(f.command({0x48, 0, 0x10, 0, 0}, 1)[0] == 0xff,
          "OTP protection must survive reset even when set using 50h");
  require(f.command({0x4b, 0, 0, 0, 0}, 8) == id, "reset must preserve chip identity");
}

static void test_suspended_sector_exclusion() {
  Fixture f;
  f.program(0x1100, {0x3c});
  f.program(0x2000, {0xc3});
  f.command({0x06});
  f.command({0x02, 0, 0x10, 0, 0x55});
  f.command({0x75});
  f.advance(20);
  require(f.read_array(0x1100, 1)[0] == 0xff,
          "program suspend only permits reads from other sectors/blocks");
  require(f.read_array(0x2000, 1)[0] == 0xc3,
          "program suspend must allow reads of other sectors");
  f.reset();
  require(!(f.status(0x35) & 0x80) && f.read_array(0x1000, 1)[0] == 0xff &&
              f.read_array(0x1100, 1)[0] == 0x3c,
          "reset must abort suspended work without changing completed bytes");
}

static void test_undefined_protection_fails_closed() {
  Fixture f;
  for (uint8_t cmp : {0, 0x40}) {
    f.write_status(0x01, {0x58, static_cast<uint8_t>(2 | cmp)}, true);
    for (uint32_t address : {0, 0x800000, 0xffffff}) {
      f.program(address, {0});
      require(f.read_array(address, 1)[0] == 0xff,
              "undefined SEC=1/BP=110 must fail closed regardless of CMP");
    }
  }
}

static void test_read_clocked_headers_and_splits() {
  Fixture f;
  f.program(0, {0x12, 0x34, 0x56, 0x78});
  for (const auto &header : std::vector<std::vector<uint8_t>>{
           {0x03, 0, 0, 0}, {0x0b, 0, 0, 0, 0},
           {0x6b, 0, 0, 0, 0, 0, 0, 0}, {0xeb, 0, 0, 0, 0xf0, 0, 0}}) {
    for (size_t split = 0; split <= header.size(); ++split) {
      f.select();
      f.write(std::vector<uint8_t>(header.begin(), header.begin() + split));
      f.write(std::vector<uint8_t>(header.begin() + split, header.end()));
      require(f.read(1)[0] == 0x12 &&
                  f.read(3) == std::vector<uint8_t>({0x34, 0x56, 0x78}),
              "command/header/data chunking must not affect state transitions");
      f.deselect();
    }
  }
  f.select();
  f.write({0x0b});
  require(f.read(4) == std::vector<uint8_t>(4, 0xff),
          "read calls must clock zero address/dummy bytes, with no queued echo");
  require(f.read(1)[0] == 0x12, "read-clocked zero address must select array offset 0");
  f.deselect();
  require(f.command({0xbb, 0, 0, 0, 0}, 4) == std::vector<uint8_t>(4, 0xff),
          "undocumented non-Fx mode bytes must not enable guessed XIP behavior");
}

static void test_suspend_foreground_and_nonsuspendable() {
  Fixture f;
  f.command({0x06});
  f.command({0x20, 0, 0, 0});
  f.command({0x75});
  f.advance(20);
  f.command({0x06});
  f.command({0x02, 0, 0x20, 0, 0x5a});
  require((f.status() & 1) && (f.status(0x35) & 0x80),
          "foreground BUSY and background SUS must coexist");
  f.command({0x75});
  f.command({0x7a});
  f.advance(20);
  require((f.status() & 1) && (f.status(0x35) & 0x80),
          "nested suspend and resume during foreground BUSY must be ignored");
  f.ready();
  require(f.status(0x35) & 0x80, "foreground completion must retain background SUS");
  f.command({0x7a});
  f.ready();
  require(f.read_array(0x2000, 1)[0] == 0x5a, "background resume must preserve foreground data");
  for (const auto &command : std::vector<std::vector<uint8_t>>{
           {0x11, 0x20}, {0x42, 0, 0x10, 0, 0}, {0x44, 0, 0x10, 0}}) {
    f.command({0x06});
    f.command(command);
    f.command({0x75});
    f.advance(20);
    require((f.status() & 1) && !(f.status(0x35) & 0x80),
            "status and security register operations must not be suspendable");
    f.ready();
  }
}

int main() {
  try {
    test_jedec_id();
    std::puts("PASS jedec_id");
    test_page_program();
    std::puts("PASS page_program");
    test_erase_and_array_wrap();
    std::puts("PASS erase_and_array_wrap");
    test_status_register_writes();
    std::puts("PASS status_register_writes");
    test_block_protection_table();
    std::puts("PASS block_protection_table");
    test_fast_reads_and_wrap();
    std::puts("PASS fast_reads_and_wrap");
    test_legacy_and_unique_ids();
    std::puts("PASS legacy_and_unique_ids");
    test_security_registers();
    std::puts("PASS security_registers");
    test_individual_locks();
    std::puts("PASS individual_locks");
    test_power_down_and_reset();
    std::puts("PASS power_down_and_reset");
    test_suspend_resume();
    std::puts("PASS suspend_resume");
    test_sfdp();
    std::puts("PASS sfdp");
    test_power_up_timing();
    std::puts("PASS power_up_timing");
    test_framing_and_transport();
    std::puts("PASS framing_and_transport");
    test_operation_timing();
    std::puts("PASS operation_timing");
    test_lifecycle_and_isolation();
    std::puts("PASS lifecycle_and_isolation");
    test_simple_bus_parent_rejected();
    std::puts("PASS simple_bus_parent_rejected");
    test_concurrent_masters();
    std::puts("PASS concurrent_masters");
    test_randomized_array_model();
    std::puts("PASS randomized_array_model");
    test_baud_configuration();
    std::puts("PASS baud_configuration");
    test_volatile_otp_retention();
    std::puts("PASS volatile_otp_retention");
    test_suspended_sector_exclusion();
    std::puts("PASS suspended_sector_exclusion");
    test_undefined_protection_fails_closed();
    std::puts("PASS undefined_protection_fails_closed");
    test_read_clocked_headers_and_splits();
    std::puts("PASS read_clocked_headers_and_splits");
    test_suspend_foreground_and_nonsuspendable();
    std::puts("PASS suspend_foreground_and_nonsuspendable");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
