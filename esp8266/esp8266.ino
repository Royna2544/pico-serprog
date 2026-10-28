
#include "ESP8266WebServer.h"
#include "wlantalk.h"
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <stdarg.h>
#include <string.h>

static ESP8266WebServer server;
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

  server.handleClient();
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
        frame.position == frame.size) {
#define send_response(command, data)                                           \
  make_wlan_talk_message(wlantalk_putc_func, NULL, command, data)
#define is(s) (!strcmp(msg.command, s))

      if (is("SLAVE")) {
        send_response("MASTER", "ESP8266");
      } else if (is("CONN?")) {
        bool connected = WiFi.status() == WL_CONNECTED;
        send_response("CONN", connected ? "1" : "0");
      } else if (is("CONN")) {
        // Args: SSID,PASSWORD,timeout_ms
        char *comma = strchr(msg.data, ',');
        if (!comma) {
          send_response("CONN", "ERROR: Invalid format");
        } else {
          *comma = '\0';
          const char *ssid = msg.data;
          const char *password = comma + 1;
          char *timeout_str = strchr(comma + 1, ',');
          if (!timeout_str) {
            send_response("CONN", "ERROR: Invalid format");
          } else {
            *timeout_str = '\0';
            const char *timeout = timeout_str + 1;
            WiFi.begin(ssid, password);
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED &&
                   (millis() - start) < strtoul(timeout, NULL, 10)) {
              delay(100);
            }
            bool connected = WiFi.status() == WL_CONNECTED;
            send_response("CONN", connected ? "OK" : "ERROR: Timeout");
          }
        }

      } else if (is("ADDR")) {
        String mac = WiFi.macAddress();
        send_response("ADDR", mac.c_str());
      } else if (is("SCAN")) {
        int n = WiFi.scanNetworks();
        char buf[128];
        switch (n) {}
        snprintf(buf, sizeof(buf), "Found %d networks", n);
        send_response("SCAN", buf);
      } else if (is("SSID")) {
        send_response("SSID", WiFi.SSID().c_str());
      } else if (is("RSSI")) {
        int32_t rssi = WiFi.RSSI();
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", rssi);
        send_response("RSSI", buf);
      } else if (is("PING")) {
        send_response("PONG", "");
      } else if (is("IPADDR")) {
        IPAddress ip = WiFi.localIP();
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        send_response("IPADDR", buf);
      } else if (is("DICONN")) {
        WiFi.disconnect();
        send_response("DICONN", "OK");
      } else if (is("CONECT")) {
        // Expecting data in the format "SSID,PASSWORD"
        char *comma = strchr(msg.data, ',');
        if (!comma) {
          send_response("CONECT", "ERROR: Invalid format");
        } else {
          *comma = '\0';
          const char *ssid = msg.data;
          const char *password = comma + 1;
          WiFi.begin(ssid, password);
          send_response("CONECT", "OK");
        }
      } else if (is("SDTEST")) {
#define SD_CS 5
        SPI.begin(); // HSPI: SCK=14, MISO=12, MOSI=13

        if (!SD.begin(SD_CS)) {
          send_response("SDTEST", "Fail: init sdcard");
          return;
        }

        // Remove previous test file
        SD.remove("/test.txt");

        // Write
        File file = SD.open("/test.txt", FILE_WRITE);
        if (!file) {
          send_response("SDTEST", "Fail: open file for write");
          return;
        }

        file.println("Hello from ESP8266!");
        file.close();

        // Read back
        file = SD.open("/test.txt", FILE_READ);
        if (!file) {
          send_response("SDTEST", "Fail, re-open file for read.");
          return;
        }

        String contents = file.readStringUntil('\n');
        file.close();

        contents.trim();
        bool ok = contents == "Hello from ESP8266!";
        send_response("SDTEST", ok ? "PASS" : "Fail, content mismatch");
      } else if (is("SRVIT")) {
        int port = atoi(msg.data);
        server.onNotFound(
            []() { server.send(404, "text/plain", "Not found"); });
        server.on("/ping", HTTP_GET, []() {
          server.send(200, "text/plain", "Hello from ESP8266!");
        });
        server.begin(port);
        send_response("SRVIT", "OK");
      } else if (is("SRVSP")) {
        server.stop();
        send_response("SRVSP", "OK");
      } else if (is("EVT")) {
        send_response("EVT", "OK");
      } else {
        send_response("ERROR", "Unknown command");
      }
    }
    delay(1);
  }
}