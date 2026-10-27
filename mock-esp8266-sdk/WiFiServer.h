#pragma once
#include "WiFiClient.h"
#include <memory>
namespace esp_mock {
struct ServerState;
}
class WiFiServer {
  std::shared_ptr<esp_mock::ServerState> state_;

public:
  using ClientType = WiFiClient;
  WiFiServer(const IPAddress &address, uint16_t port);
  explicit WiFiServer(uint16_t port);
  virtual ~WiFiServer();
  WiFiServer(const WiFiServer &) = delete;
  WiFiServer &operator=(const WiFiServer &) = delete;
  WiFiClient accept();
  WiFiClient available(uint8_t *status = nullptr);
  bool hasClient();
  size_t hasClientData();
  bool hasMaxPendingClients();
  void begin();
  void begin(uint16_t port);
  void begin(uint16_t port, uint8_t backlog);
  void setNoDelay(bool value);
  bool getNoDelay();
  uint8_t status();
  uint16_t port() const;
  void close();
  void stop() { close(); }
};
