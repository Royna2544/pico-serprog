#pragma once

#include "IPAddress.h"
#include "Stream.h"
#include "WString.h"
#include <chrono>
#include <random>
#include <stddef.h>
#include <stdint.h>
#include <thread>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

#define LSBFIRST 0
#define MSBFIRST 1

using byte = uint8_t;
using boolean = bool;
inline unsigned long millis() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}
inline unsigned long micros() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}
inline void yield() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
inline uint32_t mockRandom32() {
  thread_local std::random_device source;
  return source();
}
#define RANDOM_REG32 mockRandom32()
#ifndef DEBUGV
#define DEBUGV(...) ((void)0)
#endif

void pinMode(unsigned pin, int mode);
void digitalWrite(unsigned pin, int value);
int digitalRead(unsigned pin);
void delay(unsigned long ms);

// Serial

class MockSerial : public Stream {
public:
  void begin(unsigned long baud);
  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t byte) override;
  using Print::write;
  using Stream::read;
  void reset();
};

extern MockSerial Serial;

// IPAddress

// WiFi

typedef enum WiFiMode {
  WIFI_OFF = 0,
  WIFI_STA = 1,
  WIFI_AP = 2,
  WIFI_AP_STA = 3
} WiFiMode_t;

typedef enum {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_WRONG_PASSWORD = 6,
  WL_DISCONNECTED = 7
} wl_status_t;

#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class ESP8266WiFiClass {
  WiFiMode_t _mode = WIFI_OFF;

public:
  // Generic
  bool mode(WiFiMode_t mode);
  WiFiMode_t getMode();
  static void persistent(bool enabled);
  bool enableSTA(bool enabled);
  bool enableAP(bool enabled);
  uint8_t channel();
  int hostByName(const char *host, IPAddress &ip, uint32_t timeout_ms = 10000);

  // Station
  wl_status_t begin(const char *ssid, const char *passphrase = nullptr,
                    int32_t channel = 0, const uint8_t *bssid = nullptr,
                    bool connect = true);
  wl_status_t begin();

  bool disconnect(bool wifioff = false);
  bool disconnect(bool wifioff, bool eraseCredentials);
  bool reconnect();
  bool isConnected();

  wl_status_t status();
  String SSID() const;
  String psk() const;
  int8_t RSSI();

  IPAddress localIP();
  IPAddress subnetMask();
  IPAddress gatewayIP();
  IPAddress dnsIP(uint8_t dns_no = 0);

  String macAddress();
  bool config(IPAddress local_ip, IPAddress gateway, IPAddress subnet,
              IPAddress dns1 = IPAddress(), IPAddress dns2 = IPAddress());

  bool setAutoReconnect(bool enabled);
  bool getAutoReconnect();

  // Access point
  bool softAP(const char *ssid, const char *psk = nullptr, int channel = 1,
              int ssid_hidden = 0, int max_connection = 4,
              int beacon_interval = 100);

  bool softAPdisconnect(bool wifioff = false);
  bool softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet);
  IPAddress softAPIP();
  uint8_t softAPgetStationNum();
  String softAPSSID() const;

  // Scanning
  int8_t scanNetworks(bool async = false, bool show_hidden = false,
                      uint8_t channel = 0, uint8_t *ssid = nullptr);

  int8_t scanComplete();
  void scanDelete();
  String SSID(uint8_t networkItem);
  int32_t RSSI(uint8_t networkItem);
  int32_t channel(uint8_t networkItem);
  uint8_t encryptionType(uint8_t networkItem);
};

extern ESP8266WiFiClass WiFi;

// SPI
