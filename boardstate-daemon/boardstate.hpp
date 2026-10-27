#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>

// Wire protocol: one ASCII request per connection, one ASCII response.
// GPIO <board> <pin> MUX|DIR|PULL|PUT|GET|FUNCTION|DRIVE [value]
// GPIO pico <pin> IRQ_STATUS|IRQ_ACK|IRQ_ENABLE|IRQ_DISABLE [mask]
// IRQ pico PENDING returns 1 if any enabled GPIO event is pending.
// UART <board> TX|RX <byte for TX>; RX returns -1 when empty.
// RESET pico restores the Pico's power-on GPIO state and resets the wired ESP.
// POWER|MODE|BOOT esp queries sketch availability, boot mode, boot generation.
// <board> is pico or esp; values are physical (not active-low logical).
// Errors start with ERR. UART bytes are decimal, not newline-delimited data.
struct Pin {
  std::string function = "sio";
  bool output = false;
  bool latch = false;
  bool pull_up = false;
  bool pull_down = false;
  bool external_up = false;
  bool external_down = false;
  bool uart_tx = false; // UART peripheral drives an idle-high TX line
  int peer = -1;
};

struct BoardState {
  enum class ESPMode { Off, Flash, Rom, Invalid };
  std::array<Pin, 30> pico;
  std::array<Pin, 19> esp; // 0..16 GPIO; 17 CHIP_EN, 18 EXT_RSTB
  std::array<uint8_t, 30> irq_edges{};
  std::array<uint8_t, 30> irq_enabled{};
  std::deque<unsigned char> pico_rx;
  std::deque<unsigned char> esp_rx;
  int pico_tx = -1, pico_uart_rx = -1;
  int esp_tx = 1, esp_uart_rx = 3;
  int en = -1, rst = -1, boot = -1, strap2 = -1, strap15 = -1;
  ESPMode mode = ESPMode::Off;
  bool reset_released = false;
  bool rom_gpio2_risk_logged = false;
  unsigned int boot_generation = 0;

  explicit BoardState(const std::string &dtb);
  std::string request(const std::string &line);
  int level(bool is_esp, int number) const;
  int irq_status(int number) const;
  void update_irq_edges(const std::array<int, 30> &before);
  bool esp_running() const;
  void reset_pico();
  void update_boot_state();
  bool drives(bool is_esp, int number) const;
};