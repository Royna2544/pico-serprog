#include <Arduino.h>

static unsigned int starts;

void setup() {
  pinMode(4, OUTPUT);
  digitalWrite(4, (++starts & 1) ? HIGH : LOW);
}

void loop() { delay(100); }
