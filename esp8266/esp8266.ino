
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

struct wlantalk_frame {
  const char *data;
  size_t size;
  size_t position;
};

static int wlantalk_getc_func(void *privdata) {
  struct wlantalk_frame *frame = (struct wlantalk_frame *)privdata;
  if (frame->position == frame->size)
    return -1;
  return (unsigned char)frame->data[frame->position++];
}

static void wlantalk_putc_func(void *privdata, int c) {
  Serial.write((uint8_t)c);
}

void loop() {
  // Keep partial UART frames across loop() calls. The parser can then consume
  // a complete frame without relying on Serial.read() to block.
  static char input[128];
  static size_t used;
  static bool discarding;
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0)
      break;
    if (discarding) {
      if (c == '\n')
        discarding = false;
      continue;
    }
    if (!used && c != ':')
      continue;
    if (used == sizeof(input)) {
      used = 0;
      discarding = c != '\n';
      continue;
    }
    input[used++] = (char)c;
    if (c != '\n')
      continue;

    struct wlantalk_frame frame = {input, used, 0};
    struct wlan_talk_message msg;
    used = 0;
    if (parse_wlan_talk_message(wlantalk_getc_func, &frame, &msg) == 0 &&
        frame.position == frame.size && !strcmp(msg.command, "SLAVE")) {
      make_wlan_talk_message(wlantalk_putc_func, NULL, "MASTER", "ESP8266");
    }
  }
  delay(1);
}
