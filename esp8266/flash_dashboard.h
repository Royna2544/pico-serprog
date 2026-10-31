#pragma once
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stddef.h>
#include <stdint.h>

// All telemetry and per-client buffers have fixed capacity. The sketch sends
// UART ACKs before pump() performs any network I/O.
class FlashDashboard {
public:
  void reset();
  bool begin(uint16_t http_port);
  void stop();
  bool receive(const char *payload, size_t size);
  void pump();

private:
  enum Kind : uint8_t { ID, UNSUP, CE, SE, BE32, BE64, PP, RD, BUSY, READY, IDLE };
  struct Event {
    char payload[99];
    uint32_t session, sequence;
    Kind kind;
  };
  Event latest_ = {};
  Event id_ = {};
  Event ring_[16] = {};
  uint8_t ring_head_ = 0, ring_count_ = 0;
  bool has_latest_ = false, has_id_ = false;

  enum ClientState : uint8_t { NO_CLIENT, READ_HEADERS, STREAM, REJECT };
  WiFiServer listener_{0};
  WiFiClient client_;
  ClientState state_ = NO_CLIENT;
  // Normal browser EventSource requests often exceed 256 bytes of headers.
  char headers_[1025] = {};
  size_t header_used_ = 0;
  uint32_t header_since_ = 0;
  bool retry_sent_ = false;
  Event replay_[18] = {};
  uint8_t replay_count_ = 0, replay_index_ = 0;
  uint32_t sent_session_ = 0, sent_sequence_ = 0;
  char pending_[256] = {};
  size_t pending_size_ = 0, pending_offset_ = 0;
  uint32_t pending_since_ = 0;

  void drop_client();
  void queue_text(const char *text);
  void queue_event(const Event &event);
  void snapshot_replay();
  void next_event();
};
