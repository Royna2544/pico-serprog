#pragma once

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

#define LSBFIRST 0
#define MSBFIRST 1

using String = std::string;
using byte = uint8_t;

void pinMode(unsigned pin, int mode);
void digitalWrite(unsigned pin, int value);
int digitalRead(unsigned pin);
void delay(unsigned long ms);

// Serial

class MockSerial {
public:
  void begin(unsigned long baud);
  int available();
  int read();
  size_t write(uint8_t byte);
  void reset();
};

extern MockSerial Serial;

// IPAddress

class IPAddress {
public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes_{a, b, c, d} {}

  uint8_t operator[](int index) const { return bytes_[index]; }

  String toString() const;

private:
  uint8_t bytes_[4]{};
};

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
  WiFiMode_t _mode;

public:
  // Generic
  bool mode(WiFiMode_t mode);
  WiFiMode_t getMode();
  static void persistent(bool enabled);
  bool enableSTA(bool enabled);
  bool enableAP(bool enabled);
  uint8_t channel();

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

#define SPI_HAS_TRANSACTION 1

#define SPI_CLOCK_DIV2 0x00101001
#define SPI_CLOCK_DIV4 0x00241001
#define SPI_CLOCK_DIV8 0x004c1001
#define SPI_CLOCK_DIV16 0x009c1001
#define SPI_CLOCK_DIV32 0x013c1001
#define SPI_CLOCK_DIV64 0x027c1001
#define SPI_CLOCK_DIV128 0x04fc1001

constexpr uint8_t SPI_MODE0 = 0x00;
constexpr uint8_t SPI_MODE1 = 0x01;
constexpr uint8_t SPI_MODE2 = 0x10;
constexpr uint8_t SPI_MODE3 = 0x11;

class SPISettings {
public:
  SPISettings() : _clock(1000000), _bitOrder(LSBFIRST), _dataMode(SPI_MODE0) {}

  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
      : _clock(clock), _bitOrder(bitOrder), _dataMode(dataMode) {}

  uint32_t _clock;
  uint8_t _bitOrder;
  uint8_t _dataMode;
};

class SPIClass {
public:
  SPIClass();

  bool pins(int8_t sck, int8_t miso, int8_t mosi, int8_t ss);

  void begin();
  void end();

  void setHwCs(bool use);
  void setBitOrder(uint8_t bitOrder);
  void setDataMode(uint8_t dataMode);
  void setFrequency(uint32_t freq);
  void setClockDivider(uint32_t clockDiv);

  void beginTransaction(SPISettings settings);
  void endTransaction();

  uint8_t transfer(uint8_t data);
  uint16_t transfer16(uint16_t data);
  void transfer(void *buf, uint16_t count);

  void write(uint8_t data);
  void write16(uint16_t data);
  void write16(uint16_t data, bool msb);
  void write32(uint32_t data);
  void write32(uint32_t data, bool msb);

  void writeBytes(const uint8_t *data, uint32_t size);
  void writePattern(const uint8_t *data, uint8_t size, uint32_t repeat);

  void transferBytes(const uint8_t *out, uint8_t *in, uint32_t size);
};

extern SPIClass SPI;
