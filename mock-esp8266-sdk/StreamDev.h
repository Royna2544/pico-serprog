#pragma once
#include "Stream.h"
class StreamConstPtr : public Stream {
  const char *data_;
  size_t length_, position_ = 0;

public:
  StreamConstPtr(const char *data, size_t length)
      : data_(data), length_(data ? length : 0) {}
  explicit StreamConstPtr(const char *data)
      : StreamConstPtr(data, data ? std::strlen(data) : 0) {}
  explicit StreamConstPtr(const __FlashStringHelper *data)
      : StreamConstPtr(reinterpret_cast<const char *>(data)) {}
  StreamConstPtr(String &&) = delete;
  explicit StreamConstPtr(const String &s)
      : StreamConstPtr(s.data(), s.size()) {}
  int available() override {
    return int(std::min(length_ - position_, size_t(INT32_MAX)));
  }
  int read() override {
    return position_ < length_ ? static_cast<unsigned char>(data_[position_++])
                               : -1;
  }
  int peek() override {
    return position_ < length_ ? static_cast<unsigned char>(data_[position_])
                               : -1;
  }
  size_t write(uint8_t) override { return 0; }
  bool inputCanTimeout() override { return false; }
  ssize_t streamRemaining() override { return ssize_t(length_ - position_); }
};
class S2Stream : public Stream {
  String &data_;
  size_t position_ = 0;

public:
  explicit S2Stream(String &s) : data_(s) {}
  int available() override {
    return int(std::min(data_.size() - position_, size_t(INT32_MAX)));
  }
  int read() override {
    return position_ < data_.size()
               ? static_cast<unsigned char>(data_[position_++])
               : -1;
  }
  int peek() override {
    return position_ < data_.size()
               ? static_cast<unsigned char>(data_[position_])
               : -1;
  }
  using Print::write;
  size_t write(uint8_t c) override {
    data_ += char(c);
    return 1;
  }
  size_t write(const uint8_t *p, size_t n) override {
    return data_.concat(reinterpret_cast<const char *>(p), n) ? n : 0;
  }
  bool inputCanTimeout() override { return false; }
  ssize_t streamRemaining() override {
    return ssize_t(data_.size() - position_);
  }
};
