#include "Arduino.h"
#include "boardstate-daemon/client.h"
#include "vpmic/vpmic.ino"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void checked(int result, const char *operation) {
  if (result >= 0) return;
  std::fprintf(stderr, "Uno mock: %s failed\n", operation);
  std::exit(EXIT_FAILURE);
}

void pinMode(unsigned pin, int mode) {
  checked(board_gpio("uno", pin, "MUX", "sio"), "pin mux");
  checked(board_gpio("uno", pin, "DIR", mode == OUTPUT ? "1" : "0"),
          "pin direction");
}

void digitalWrite(unsigned pin, int value) {
  checked(board_gpio("uno", pin, "PUT", value == HIGH ? "1" : "0"),
          "digitalWrite");
}

int main() {
  setup();
  for (;;) {
    loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
