#include "Arduino.h"
#include "boardstate-daemon/client.h"

#include <cerrno>
#include <stdexcept>
#include <thread>
#include <chrono>

MockSerial Serial;
static int cached_byte = -1;

static void checked(int result) {
  if (result < 0) throw std::runtime_error("boardstate socket operation failed");
}

void pinMode(unsigned pin, int mode) {
  checked(board_gpio("esp", pin, "MUX", "sio"));
  if (mode == INPUT_PULLUP)
    checked(board_gpio("esp", pin, "PULL", "10"));
  checked(board_gpio("esp", pin, "DIR", mode == OUTPUT ? "1" : "0"));
}
void digitalWrite(unsigned pin, int value) {
  checked(board_gpio("esp", pin, "PUT", value ? "1" : "0"));
}
int digitalRead(unsigned pin) {
  int value = board_gpio("esp", pin, "GET", nullptr);
  checked(value);
  return value;
}
void delay(unsigned long ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
void MockSerial::begin(unsigned long) {
  checked(board_gpio("esp", 1, "MUX", "uart"));
  checked(board_gpio("esp", 3, "MUX", "uart"));
}
void MockSerial::reset() { cached_byte = -1; }
int MockSerial::available() {
  if (cached_byte >= 0) return 1;
  errno = 0;
  cached_byte = board_uart("esp", "RX", -1);
  if (cached_byte < 0 && errno) checked(-1);
  return cached_byte >= 0 ? 1 : 0;
}
int MockSerial::read() {
  if (!available()) return -1;
  int byte = cached_byte;
  cached_byte = -1;
  return byte;
}
int MockSerial::peek() {
  return available() ? cached_byte : -1;
}
size_t MockSerial::write(uint8_t byte) {
  checked(board_uart("esp", "TX", byte));
  return 1;
}
