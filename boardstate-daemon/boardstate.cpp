#include "boardstate.hpp"

#include <libfdt.h>

#include <cerrno>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static int pin_from_name(const std::string &name) {
  if (name.compare(0, 4, "gpio") != 0)
    throw std::runtime_error("unexpected pin name: " + name);
  size_t end = 0;
  int n = std::stoi(name.substr(4), &end);
  if (end != name.size() - 4 || n < 0 || n >= 30)
    throw std::runtime_error("invalid RP2040 pin: " + name);
  return n;
}

static int gpio_property(const void *fdt, int node, const char *name) {
  int len;
  const auto *cells = static_cast<const fdt32_t *>(fdt_getprop(fdt, node, name, &len));
  if (!cells || len != 3 * sizeof(fdt32_t) || fdt32_to_cpu(cells[1]) >= 30)
    throw std::runtime_error(std::string("missing/invalid ") + name);
  int controller = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(cells[0]));
  if (controller < 0 || !fdt_getprop(fdt, controller, "gpio-controller", nullptr))
    throw std::runtime_error(std::string("invalid GPIO controller in ") + name);
  return fdt32_to_cpu(cells[1]);
}

BoardState::BoardState(const std::string &dtb) {
  std::ifstream file(dtb, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open DTB: " + dtb);
  std::vector<char> blob((std::istreambuf_iterator<char>(file)), {});
  if (blob.size() < sizeof(fdt_header) || fdt_check_header(blob.data()) ||
      fdt_totalsize(blob.data()) > blob.size())
    throw std::runtime_error("invalid DTB: " + dtb);
  const void *fdt = blob.data();
  const char *uart_path = fdt_get_alias(fdt, "serial1");
  if (!uart_path) throw std::runtime_error("serial1 alias missing");
  int uart = fdt_path_offset(fdt, uart_path);
  int len;
  const auto *phandle = static_cast<const fdt32_t *>(
      fdt_getprop(fdt, uart, "pinctrl-0", &len));
  if (uart < 0 || !phandle || len != sizeof(fdt32_t))
    throw std::runtime_error("UART1 missing pinctrl-0");
  int group = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*phandle));
  const char *pins = static_cast<const char *>(fdt_getprop(fdt, group, "pins", &len));
  if (!pins || len <= 0)
    throw std::runtime_error("UART1 pin group has no pins");
  std::string first(pins);
  if (first.size() + 1 >= static_cast<size_t>(len))
    throw std::runtime_error("UART1 requires TX and RX pins");
  std::string second(pins + first.size() + 1);
  if (first.size() + second.size() + 2 != static_cast<size_t>(len))
    throw std::runtime_error("UART1 pin group requires exactly two pins");
  pico_tx = pin_from_name(first);
  pico_uart_rx = pin_from_name(second);
  if (pico_tx == pico_uart_rx ||
      !fdt_getprop(fdt, group, "function", nullptr) ||
      std::strcmp(static_cast<const char *>(fdt_getprop(fdt, group, "function", nullptr)), "uart"))
    throw std::runtime_error("UART1 pin group is not UART TX/RX");

  int wifi = fdt_subnode_offset(fdt, uart, "wifi");
  if (wifi < 0 || fdt_node_check_compatible(fdt, wifi, "espressif,esp8266"))
    throw std::runtime_error("ESP8266 missing from UART1");
  en = gpio_property(fdt, wifi, "enable-gpios");
  rst = gpio_property(fdt, wifi, "reset-gpios");
  boot = gpio_property(fdt, wifi, "boot-gpios");
  strap2 = gpio_property(fdt, wifi, "strap-gpio2-gpios");
  strap15 = gpio_property(fdt, wifi, "strap-gpio15-gpios");

  auto connect = [this](int rp, int ep) {
    if (rp == pico_tx || rp == pico_uart_rx || pico[rp].peer != -1)
      throw std::runtime_error("conflicting ESP wire in board.dts");
    pico[rp].peer = ep;
    esp[ep].peer = rp;
  };
  connect(en, 17);      // CHIP_EN, QFN pin 7, not ESP GPIO7
  connect(rst, 18);     // EXT_RSTB, QFN pin 32, not ESP GPIO16
  connect(boot, 0);     // GPIO0, QFN pin 15
  connect(strap2, 2);   // GPIO2, QFN pin 14
  connect(strap15, 15); // MTDO/GPIO15, QFN pin 13
  pico[pico_tx].peer = esp_uart_rx;
  esp[esp_uart_rx].peer = pico_tx;
  pico[pico_uart_rx].peer = esp_tx;
  esp[esp_tx].peer = pico_uart_rx;
  pico[pico_tx].uart_tx = true;
  esp[esp_tx].uart_tx = true;
  // Board pull resistors documented beside these properties in board.dts.
  pico[en].external_down = true;
  pico[rst].external_up = true;
  pico[boot].external_up = true;
  pico[strap2].external_up = true;
  pico[strap15].external_down = true;
  esp[esp_tx].function = "uart";
  esp[esp_uart_rx].function = "uart";
}

bool BoardState::drives(bool is_esp, int number) const {
  const Pin &pin = is_esp ? esp[number] : pico[number];
  if (pin.function == "sio" && pin.output) return true;
  if (pin.function != "uart" || !pin.uart_tx) return false;
  return !is_esp || mode == ESPMode::Flash || mode == ESPMode::Rom;
}

int BoardState::level(bool is_esp, int number) const {
  const Pin &p = is_esp ? esp[number] : pico[number];
  const Pin *other = p.peer < 0 ? nullptr :
      (is_esp ? &pico[p.peer] : &esp[p.peer]);
  bool drive = drives(is_esp, number);
  bool other_drive = other && drives(!is_esp, p.peer);
  bool value = p.function == "uart" ? true : p.latch;
  bool other_value = other && other->function == "uart" ? true :
                     other && other->latch;
  if (drive && other_drive && value != other_value)
    return -2; // electrical contention; do not silently choose a winner
  if (drive) return value;
  if (other_drive) return other_value;
  if (p.pull_up || p.external_up ||
      (other && (other->pull_up || other->external_up))) return 1;
  if (p.pull_down || p.external_down ||
      (other && (other->pull_down || other->external_down))) return 0;
  return 0; // floating, no voltage model yet
}

int BoardState::irq_status(int number) const {
  int voltage = level(false, number);
  uint8_t events = irq_edges[number];
  if (voltage >= 0)
    events |= voltage ? 0x2 : 0x1; // RP2040 high/low level status
  return events & irq_enabled[number];
}

void BoardState::update_irq_edges(const std::array<int, 30> &before) {
  for (size_t pin = 0; pin < pico.size(); ++pin) {
    int after = level(false, pin);
    if (before[pin] >= 0 && after >= 0 && before[pin] != after)
      irq_edges[pin] |= after ? 0x8 : 0x4; // rise/fall are latched
  }
}

bool BoardState::esp_running() const {
  return mode == ESPMode::Flash;
}

void BoardState::reset_pico() {
  for (Pin &pin : pico) {
    pin.function = "sio";
    pin.output = false;
    pin.latch = false;
    pin.pull_up = false;
    pin.pull_down = false;
  }
  irq_edges.fill(0);
  irq_enabled.fill(0);
  pico_rx.clear();
  esp_rx.clear();
  update_boot_state();
  std::fprintf(stderr, "boardstate: Pico board reset\n");
}

void BoardState::update_boot_state() {
  bool ready = level(false, en) == 1 && level(false, rst) == 1;
  if (!ready && reset_released) {
    mode = ESPMode::Off;
    rom_gpio2_risk_logged = false;
    for (int i = 0; i <= 16; ++i) {
      esp[i].function = (i == esp_tx || i == esp_uart_rx) ? "uart" : "sio";
      esp[i].output = false;
      esp[i].latch = false;
      esp[i].pull_up = false;
      esp[i].pull_down = false;
      esp[i].uart_tx = i == esp_tx;
    }
    pico_rx.clear();
    esp_rx.clear();
    std::fprintf(stderr, "boardstate: ESP reset/disabled\n");
  } else if (ready && !reset_released) {
    ++boot_generation;
    int gpio15 = level(false, strap15);
    int gpio2 = level(false, strap2);
    int gpio0 = level(false, boot);
    if (gpio15 != 0 || gpio2 != 1 || gpio0 < 0) {
      mode = ESPMode::Invalid;
      std::fprintf(stderr, "boardstate: invalid boot straps GPIO15=%d GPIO2=%d GPIO0=%d\n",
                   gpio15, gpio2, gpio0);
    } else if (gpio0 == 0) {
      mode = ESPMode::Rom;
      esp[2].function = "uart";
      esp[2].uart_tx = true;
      esp[2].output = true;
      std::fprintf(stderr, "boardstate: Entered bootrom upload mode (GPIO15=0 GPIO2=1 GPIO0=0)\n");
    } else {
      mode = ESPMode::Flash;
      esp[2].function = "sio";
      esp[2].uart_tx = false;
      esp[2].output = false;
      std::fprintf(stderr, "boardstate: booting normal mode (GPIO15=0 GPIO2=1 GPIO0=1)\n");
    }
  }
  if (mode == ESPMode::Rom && ready && drives(false, strap2) &&
      !rom_gpio2_risk_logged) {
    std::fprintf(stderr,
                 "boardstate: warning: ROM GPIO2 TX may contend with Pico GP%d output; release it to input after sampling\n",
                 strap2);
    rom_gpio2_risk_logged = true;
  }
  reset_released = ready;
}

std::string BoardState::request(const std::string &line) {
  std::istringstream in(line);
  std::string kind, board, action, extra;
  in >> kind >> board;
  if (board != "pico" && board != "esp") return "ERR board";
  bool is_esp = board == "esp";
  if (kind == "RESET") {
    if (is_esp || (in >> extra)) return "ERR operation";
    reset_pico();
    return "OK";
  }
  if (kind == "POWER") {
    if (in >> extra) return "ERR syntax";
    return esp_running() ? "1" : "0";
  }
  if (kind == "MODE") {
    if (in >> extra) return "ERR syntax";
    switch (mode) {
    case ESPMode::Off: return "off";
    case ESPMode::Flash: return "flash";
    case ESPMode::Rom: return "rom";
    case ESPMode::Invalid: return "invalid";
    }
  }
  if (kind == "BOOT") {
    if (in >> extra) return "ERR syntax";
    return std::to_string(boot_generation);
  }
  if (kind == "IRQ") {
    if (board != "pico" || !(in >> action) || action != "PENDING" ||
        (in >> extra)) return "ERR operation";
    for (size_t pin = 0; pin < pico.size(); ++pin)
      if (irq_status(pin)) return "1";
    return "0";
  }
  if (kind == "GPIO") {
    std::string pin_text;
    int max_pin = is_esp ? 16 : static_cast<int>(pico.size()) - 1;
    std::string range = "0-" + std::to_string(max_pin);
    if (!(in >> pin_text))
      return "ERR GPIO " + board + " missing pin (valid " + range + ")";
    long long parsed_pin;
    try {
      size_t end;
      parsed_pin = std::stoll(pin_text, &end, 10);
      if (end != pin_text.size()) throw std::invalid_argument("pin");
    } catch (const std::exception &) {
      std::string shown = pin_text.substr(0, 16);
      if (pin_text.size() > 16) shown += "...";
      return "ERR GPIO " + board + " pin '" + shown +
             "' is not an integer (valid " + range + ")";
    }
    if (parsed_pin < 0 || parsed_pin > max_pin)
      return "ERR GPIO " + board + " pin " + pin_text +
             " out of range (valid " + range +
             (is_esp ? "; EN/RESET aren't GPIO)" : ")");
    if (!(in >> action))
      return "ERR GPIO " + board + " pin " + pin_text + " missing operation";
    int number = static_cast<int>(parsed_pin);
    Pin &pin = is_esp ? esp[number] : pico[number];
    if (action == "FUNCTION") {
      if (in >> extra) return "ERR syntax";
      return pin.function;
    }
    if (action == "DRIVE") {
      if (in >> extra) return "ERR syntax";
      if (!drives(is_esp, number)) return "input";
      return pin.function == "uart" ? "uart-tx" : "sio-out";
    }
    if (action == "GET") {
      if (in >> extra) return "ERR syntax";
      int value = level(is_esp, number);
      return value < 0 ? "ERR contention" : std::to_string(value);
    }
    if (action == "IRQ_STATUS") {
      if (is_esp) return "ERR operation";
      if (in >> extra) return "ERR syntax";
      return std::to_string(irq_status(number));
    }
    std::string value;
    if (!(in >> value) || (in >> extra)) return "ERR syntax";
    if (action == "IRQ_ENABLE" || action == "IRQ_DISABLE" ||
        action == "IRQ_ACK") {
      if (is_esp) return "ERR operation";
      int mask;
      try {
        size_t end;
        mask = std::stoi(value, &end);
        if (end != value.size() || mask < 0 || mask > 15)
          return "ERR value";
      } catch (const std::exception &) {
        return "ERR value";
      }
      irq_edges[number] &= ~(mask & 0xc);
      if (action == "IRQ_ENABLE") irq_enabled[number] |= mask;
      if (action == "IRQ_DISABLE") irq_enabled[number] &= ~mask;
      return "OK";
    }
    if (is_esp && number == 2 && mode == ESPMode::Rom)
      return "ERR ROM owns GPIO2";
    if (is_esp && mode != ESPMode::Flash)
      return "ERR ESP sketch not running";
    std::array<int, 30> before;
    for (size_t i = 0; i < pico.size(); ++i)
      before[i] = level(false, i);
    if (action == "MUX") {
      if (value != "sio" && value != "uart" && value != "spi" &&
          value != "i2c" && value != "pwm" && value != "clk") return "ERR mux";
      pin.function = value;
    } else if (action == "DIR" || action == "PUT") {
      if (value != "0" && value != "1") return "ERR value";
      if (action == "DIR") pin.output = value == "1";
      else pin.latch = value == "1";
    } else if (action == "PULL") {
      if (value.size() != 2 || (value[0] != '0' && value[0] != '1') ||
          (value[1] != '0' && value[1] != '1')) return "ERR pull";
      pin.pull_up = value[0] == '1';
      pin.pull_down = value[1] == '1';
    } else return "ERR operation";
    update_boot_state();
    update_irq_edges(before);
    return "OK";
  }
  if (kind == "UART") {
    if (!(in >> action)) return "ERR operation";
    if (action == "RX") {
      if (in >> extra) return "ERR syntax";
      auto &queue = is_esp ? esp_rx : pico_rx;
      if (queue.empty()) return "-1";
      int byte = queue.front();
      queue.pop_front();
      return std::to_string(byte);
    }
    int byte;
    if (action != "TX" || !(in >> byte) || (in >> extra) || byte < 0 || byte > 255)
      return "ERR byte";
    if (is_esp && mode != ESPMode::Flash)
      return "ERR ESP sketch not running";
    if ((is_esp ? esp[esp_tx].function : pico[pico_tx].function) != "uart")
      return "ERR UART TX pin not muxed";
    if (mode != ESPMode::Flash) return "OK"; // Unpowered or ROM receiver: no decoder.
    if ((is_esp ? pico[pico_uart_rx].function : esp[esp_uart_rx].function) != "uart")
      return "OK"; // TX still occurs, but no peripheral is listening on the wire.
    auto &queue = is_esp ? pico_rx : esp_rx;
    if (queue.size() >= 4096) return "ERR uart queue full";
    queue.push_back(static_cast<unsigned char>(byte));
    return "OK";
  }
  return "ERR operation";
}

static volatile sig_atomic_t stop = 0;
static void on_signal(int) { stop = 1; }

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s board.dtb socket-path\n", argv[0]);
    return 2;
  }
  try {
    BoardState board(argv[1]);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (std::strlen(argv[2]) >= sizeof(addr.sun_path))
      throw std::runtime_error("socket path too long");
    std::strcpy(addr.sun_path, argv[2]);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    // Do not unlink arbitrary pre-existing files (or another daemon's socket).
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        listen(fd, 16) < 0) {
      std::string error = std::strerror(errno);
      close(fd);
      throw std::runtime_error(error);
    }
    chmod(argv[2], 0600);
    struct sigaction handler{};
    handler.sa_handler = on_signal;
    sigemptyset(&handler.sa_mask);
    if (sigaction(SIGINT, &handler, nullptr) < 0 ||
        sigaction(SIGTERM, &handler, nullptr) < 0)
      throw std::runtime_error(std::strerror(errno));
    while (!stop) {
      int client = accept(fd, nullptr, nullptr);
      if (client < 0) { if (errno == EINTR) continue; break; }
      char buffer[256]{};
      ssize_t n = recv(client, buffer, sizeof(buffer) - 1, 0);
      std::string answer = n <= 0 || n == static_cast<ssize_t>(sizeof(buffer) - 1)
                               ? "ERR request length" : board.request(buffer);
      answer += '\n';
      (void)send(client, answer.data(), answer.size(), MSG_NOSIGNAL);
      close(client);
    }
    close(fd);
    unlink(argv[2]);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "boardstate: %s\n", e.what());
    return 1;
  }
}