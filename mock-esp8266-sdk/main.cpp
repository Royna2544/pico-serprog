#include <Arduino.h>
#include "boardstate-daemon/client.h"

#include <exception>
#include <cstdio>

void setup();
void loop();

int main() {
  try {
    bool active = false;
    int generation = -1;
    for (;;) {
      int power = board_power();
      int current_generation = board_boot_generation();
      if (power < 0 || current_generation < 0) return 1;
      if (power && (!active || current_generation != generation)) {
        Serial.reset();
        setup();
      }
      if (!power && active) Serial.reset();
      active = power == 1;
      generation = current_generation;
      if (active) loop();
      else delay(1);
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "ESP mock: %s\n", e.what());
    return 1;
  }
}
