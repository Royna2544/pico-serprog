#include "flash_dashboard.h"
#include <Arduino.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool decimal32(const char *s, uint32_t *out) {
  if (!s || !*s) return false;
  uint32_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
    uint32_t d = static_cast<uint32_t>(*s - '0');
    if (n > (UINT32_MAX - d) / 10) return false;
    n = n * 10 + d;
  }
  *out = n;
  return true;
}

void FlashDashboard::reset() {
  stop();
  has_latest_ = has_id_ = false;
  ring_head_ = ring_count_ = 0;
}

void FlashDashboard::drop_client() {
  client_.abort(); // stop() can flush/wait on a slow receiver.
  state_ = NO_CLIENT;
  header_used_ = replay_count_ = replay_index_ = 0;
  pending_size_ = pending_offset_ = 0;
  sent_session_ = sent_sequence_ = 0;
}

bool FlashDashboard::begin(uint16_t http_port) {
  stop();
  if (http_port < 1024 || http_port > 65534) return false;
  listener_.begin(http_port + 1);
  return listener_.status() != 0;
}

void FlashDashboard::stop() {
  drop_client();
  listener_.stop();
}

bool FlashDashboard::receive(const char *payload, size_t size) {
  if (!payload || size < 3 || size >= sizeof(latest_.payload)) return false;
  char copy[sizeof(latest_.payload)];
  memcpy(copy, payload, size);
  copy[size] = '\0';
  char *field[10];
  field[0] = copy;
  unsigned count = 1;
  for (size_t i = 0; i < size; ++i) {
    if (copy[i] == ',') {
      if (count == 10 || i == 0 || i + 1 == size || copy[i - 1] == ',')
        return false;
      copy[i] = '\0';
      field[count++] = copy + i + 1;
    } else if (copy[i] < '!' || copy[i] > '~' || copy[i] == ':') {
      return false;
    }
  }
  if (count != 10 || strcmp(field[0], "F1") != 0) return false;
  static const char *const kinds[] = {"ID", "UNSUP", "CE", "SE", "BE32", "BE64",
                                       "PP", "RD", "BUSY", "READY", "IDLE"};
  unsigned k = 0;
  for (; k < sizeof(kinds) / sizeof(kinds[0]); ++k)
    if (!strcmp(field[3], kinds[k])) break;
  if (k == sizeof(kinds) / sizeof(kinds[0])) return false;
  uint32_t session, sequence, address, length, erase, program, read, lost;
  if (!decimal32(field[1], &session) || !decimal32(field[2], &sequence) ||
      !decimal32(field[4], &address) || !decimal32(field[5], &length) ||
      !decimal32(field[6], &erase) || !decimal32(field[7], &program) ||
      !decimal32(field[8], &read) || !decimal32(field[9], &lost) ||
      !session || !sequence || erase > 2048 || program > 32768 || read > 32768)
    return false;
  constexpr uint32_t flash_bytes = 8u * 1024u * 1024u;
  if (k == ID || k == UNSUP) {
    if (address > 0xffffff || length || (k == ID) != (address == 0xc22017) ||
        (k == UNSUP && (erase || program || read))) return false;
  } else if (k == CE) {
    if (address || length != flash_bytes) return false;
  } else if (k >= SE && k <= RD) {
    if (address >= flash_bytes || !length || length > flash_bytes - address ||
        (k == SE && length != 4096) || (k == BE32 && length != 32768) ||
        (k == BE64 && length != 65536) || (k == PP && length > 256))
      return false;
  } else if (address || length) {
    return false;
  }
  bool new_session = !has_latest_ || latest_.session != session ||
                     ((k == ID || k == UNSUP) && sequence == 1 &&
                      latest_.sequence > 1);
  if (!new_session && sequence <= latest_.sequence) return true; // ACK retry.
  if (new_session) {
    ring_head_ = ring_count_ = 0;
    has_id_ = has_latest_ = false;
    replay_count_ = replay_index_ = 0;
    // An active stream gets the new session on the next pump, without
    // resetting either socket or waiting for a browser reconnection.
    sent_session_ = sent_sequence_ = 0;
  }
  Event event = {};
  memcpy(event.payload, payload, size);
  event.payload[size] = '\0';
  event.session = session;
  event.sequence = sequence;
  event.kind = static_cast<Kind>(k);
  if (k == ID || k == UNSUP) {
    id_ = event; // Retained separately, even after ring eviction.
    has_id_ = true;
  }
  if (k == ID || k == UNSUP || k == CE ||
      (k != BUSY && k != READY && (!has_latest_ || latest_.kind != k))) {
    ring_[ring_head_] = event;
    ring_head_ = (ring_head_ + 1) % 16;
    if (ring_count_ < 16) ++ring_count_;
  }
  latest_ = event;
  has_latest_ = true;
  return true;
}

void FlashDashboard::queue_text(const char *text) {
  size_t size = strlen(text);
  if (pending_size_ || size >= sizeof(pending_)) return;
  memcpy(pending_, text, size);
  pending_size_ = size;
  pending_offset_ = 0;
  pending_since_ = millis();
}

void FlashDashboard::queue_event(const Event &event) {
  if (pending_size_) return;
  int n = snprintf(pending_, sizeof(pending_),
                   "id: %lu-%lu\nevent: flash\ndata: %s\n\n",
                   static_cast<unsigned long>(event.session),
                   static_cast<unsigned long>(event.sequence), event.payload);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(pending_)) {
    drop_client();
    return;
  }
  pending_size_ = static_cast<size_t>(n);
  pending_offset_ = 0;
  pending_since_ = millis();
  sent_session_ = event.session;
  sent_sequence_ = event.sequence;
}

void FlashDashboard::snapshot_replay() {
  replay_count_ = replay_index_ = 0;
  if (has_id_) replay_[replay_count_++] = id_;
  for (uint8_t i = 0; i < ring_count_; ++i) {
    const Event &event = ring_[(ring_head_ + 16 - ring_count_ + i) % 16];
    if (has_id_ && event.session == id_.session &&
        event.sequence == id_.sequence) continue;
    replay_[replay_count_++] = event;
  }
  if (has_latest_ && (!replay_count_ ||
      replay_[replay_count_ - 1].session != latest_.session ||
      replay_[replay_count_ - 1].sequence != latest_.sequence))
    replay_[replay_count_++] = latest_;
}

void FlashDashboard::next_event() {
  if (replay_index_ < replay_count_) {
    queue_event(replay_[replay_index_++]);
    return;
  }
  if (!has_latest_) return;
  // A milestone that arrived during replay is pushed before the snapshot.
  for (uint8_t i = 0; i < ring_count_; ++i) {
    const Event &event = ring_[(ring_head_ + 16 - ring_count_ + i) % 16];
    if (event.session == sent_session_ && event.sequence <= sent_sequence_)
      continue;
    queue_event(event);
    return;
  }
  if (latest_.session != sent_session_ || latest_.sequence > sent_sequence_)
    queue_event(latest_);
}

void FlashDashboard::pump() {
  if (!listener_.status()) return;
  if (state_ == NO_CLIENT) {
    client_ = listener_.accept();
    if (!client_) return;
    client_.setTimeout(5); // Bound even a surprising TCP write to milliseconds.
    state_ = READ_HEADERS;
    header_used_ = 0;
    retry_sent_ = false;
    header_since_ = millis();
    return;
  }
  // While streaming, pending viewers remain in the server's bounded backlog;
  // accept the next one only after the current client disconnects.
  if (!client_.connected()) {
    drop_client();
    return;
  }
  if (state_ == READ_HEADERS) {
    if (static_cast<uint32_t>(millis() - header_since_) > 1000) {
      drop_client();
      return;
    }
    for (unsigned i = 0; i < 32 && client_.available() > 0; ++i) {
      int c = client_.read();
      if (c < 0 || header_used_ == sizeof(headers_) - 1) {
        drop_client();
        return;
      }
      headers_[header_used_++] = static_cast<char>(c);
      headers_[header_used_] = '\0';
      if (header_used_ >= 4 &&
          memcmp(headers_ + header_used_ - 4, "\r\n\r\n", 4) == 0) {
        static const char route[] = "GET /events HTTP/1.1\r\n";
        if (strncmp(headers_, route, sizeof(route) - 1) != 0) {
          state_ = REJECT;
          queue_text("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
          state_ = STREAM;
          snapshot_replay();
          queue_text("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n");
        }
        break;
      }
    }
  }
  if (pending_size_) {
    if (static_cast<uint32_t>(millis() - pending_since_) > 2000) {
      drop_client();
      return;
    }
    int room = client_.availableForWrite();
    if (room > 0) {
      size_t left = pending_size_ - pending_offset_;
      size_t size = left < static_cast<size_t>(room) ? left : static_cast<size_t>(room);
      size_t wrote = client_.write(reinterpret_cast<const uint8_t *>(pending_ + pending_offset_), size);
      if (wrote) {
        pending_offset_ += wrote;
        pending_since_ = millis();
        if (pending_offset_ == pending_size_) pending_size_ = pending_offset_ = 0;
      }
    }
    return; // At most one bounded network write per loop.
  }
  if (state_ == REJECT) {
    drop_client();
  } else if (state_ == STREAM) {
    if (!retry_sent_) {
      retry_sent_ = true;
      queue_text("retry: 1000\n\n");
    } else {
      next_event();
    }
  }
}
