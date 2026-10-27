#pragma once
#include "Stream.h"
#include <array>
#include <memory>
class MD5Builder {
  struct State;
  std::unique_ptr<State> state_;
  std::array<uint8_t, 16> digest_{};

public:
  MD5Builder();
  ~MD5Builder();
  void begin();
  void add(const uint8_t *data, size_t n);
  void add(const String &s) {
    add(reinterpret_cast<const uint8_t *>(s.data()), s.size());
  }
  bool addStream(Stream &source, size_t n);
  void calculate();
  void getBytes(uint8_t *out) const {
    if (out)
      std::memcpy(out, digest_.data(), digest_.size());
  }
  String toString() const;
};
