#pragma once
#include "Arduino.h"
#include "Client.h"
#include "wl_definitions.h"
#include <memory>
#define WIFICLIENT_MAX_PACKET_SIZE 1460
#define WIFICLIENT_MAX_FLUSH_WAIT_MS 300
#define TCP_DEFAULT_KEEPALIVE_IDLE_SEC 7200
#define TCP_DEFAULT_KEEPALIVE_INTERVAL_SEC 75
#define TCP_DEFAULT_KEEPALIVE_COUNT 9
namespace esp_mock {
struct TcpState;
}
class WiFiServer;
class WiFiClient : public Client {
  std::shared_ptr<esp_mock::TcpState> state_;
  explicit WiFiClient(std::shared_ptr<esp_mock::TcpState> state);
  friend class WiFiServer;

public:
  WiFiClient();
  ~WiFiClient() override = default;
  WiFiClient(const WiFiClient &) = default;
  WiFiClient &operator=(const WiFiClient &) = default;
  virtual std::unique_ptr<WiFiClient> clone() const;
  virtual uint8_t status();
  int connect(IPAddress ip, uint16_t port) override;
  int connect(const char *host, uint16_t port) override;
  virtual int connect(const String &host, uint16_t port) {
    return connect(host.c_str(), port);
  }
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *data, size_t n) override;
  using Print::write;
  virtual size_t write_P(PGM_P data, size_t n) { return write(data, n); }
  size_t write(Stream &source) { return source.sendAll(*this); }
  int available() override;
  int read() override;
  int read(uint8_t *data, size_t n) override;
  int read(char *data, size_t n) {
    return read(reinterpret_cast<uint8_t *>(data), n);
  }
  int peek() override;
  virtual size_t peekBytes(uint8_t *data, size_t n);
  size_t peekBytes(char *data, size_t n) {
    return peekBytes(reinterpret_cast<uint8_t *>(data), n);
  }
  void flush() override { (void)flush(0); }
  void stop() override { (void)stop(0); }
  bool flush(unsigned int maxWaitMs);
  bool stop(unsigned int maxWaitMs);
  uint8_t connected() override;
  operator bool() override { return connected(); }
  virtual IPAddress remoteIP();
  virtual uint16_t remotePort();
  virtual IPAddress localIP();
  virtual uint16_t localPort();
  static void setLocalPortStart(uint16_t port);
  int availableForWrite() override;
  static void stopAll();
  static void stopAllExcept(WiFiClient *except);
  void keepAlive(uint16_t idle = TCP_DEFAULT_KEEPALIVE_IDLE_SEC,
                 uint16_t interval = TCP_DEFAULT_KEEPALIVE_INTERVAL_SEC,
                 uint8_t count = TCP_DEFAULT_KEEPALIVE_COUNT);
  bool isKeepAliveEnabled() const;
  uint16_t getKeepAliveIdle() const;
  uint16_t getKeepAliveInterval() const;
  uint8_t getKeepAliveCount() const;
  void disableKeepAlive() { keepAlive(0, 0, 0); }
  static void setDefaultNoDelay(bool value);
  static bool getDefaultNoDelay();
  bool getNoDelay() const;
  void setNoDelay(bool value);
  static void setDefaultSync(bool value);
  static bool getDefaultSync();
  bool getSync() const;
  void setSync(bool value);
  bool hasPeekBufferAPI() const override { return true; }
  size_t peekAvailable() override;
  const char *peekBuffer() override;
  void peekConsume(size_t n) override;
  bool outputCanTimeout() override { return connected(); }
  bool inputCanTimeout() override { return connected(); }
  void abort();
};
