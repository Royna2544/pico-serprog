
#include "wlantalk.h"
#include <Arduino.h>
#include <string.h>

// This sketch is also compiled unchanged with the real ESP8266 Arduino core.
// Replace loop() with the application protocol when firmware is ready.
void setup() {
  Serial.begin(74880);
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
}

static int wlantalk_getc_func(void *privdata) { return Serial.read(); }

static void wlantalk_putc_func(void *privdata, int c) {
  Serial.write((uint8_t)c);
}

void loop() {
  struct wlan_talk_message msg;
  if (Serial.available()) {
    if (parse_wlan_talk_message(wlantalk_getc_func, NULL, &msg) < 0) {
      return;
    }
    if (!strcmp(msg.command, "SLAVE")) {
      make_wlan_talk_message(wlantalk_putc_func, NULL, "MASTER", "ESP8266");
    }
  }
  delay(1);
}
