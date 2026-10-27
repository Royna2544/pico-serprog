#pragma once
#include "Arduino.h"
#include "Udp.h"
#include <memory>
#define UDP_TX_PACKET_MAX_SIZE 8192
namespace esp_mock {
struct UdpState;
}
class WiFiUDP : public UDP {
  std::shared_ptr<esp_mock::UdpState> state_;

public:
  WiFiUDP() = default;
  WiFiUDP(const WiFiUDP &) = default;
  WiFiUDP &operator=(const WiFiUDP &) = default;
  ~WiFiUDP() override = default;
  operator bool() const;
  uint8_t begin(uint16_t port) override;
  uint8_t begin(IPAddress address, uint16_t port);
  void stop() override;
  uint8_t beginMulticast(IPAddress interfaceAddress, IPAddress multicast,
                         uint16_t port);
  int beginPacket(IPAddress ip, uint16_t port) override;
  int beginPacket(const char *host, uint16_t port) override;
  virtual int beginPacketMulticast(IPAddress group, uint16_t port,
                                   IPAddress interfaceAddress, int ttl = 1);
  int endPacket() override;
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *data, size_t n) override;
  using Print::write;
  int parsePacket() override;
  int available() override;
  int read() override;
  int read(uint8_t *data, size_t n) override;
  int read(char *data, size_t n) {
    return read(reinterpret_cast<uint8_t *>(data), n);
  }
  int peek() override;
  void flush() override;
  IPAddress remoteIP() override;
  uint16_t remotePort() override;
  IPAddress destinationIP() const;
  uint16_t localPort() const;
  static void stopAll();
  static void stopAllExcept(WiFiUDP *except);
};
