#pragma once
#include "Print.h"
#include "WString.h"
#include <cstdint>
class IPAddress {
  uint8_t bytes_[4]{};

public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes_{a, b, c, d} {}
  explicit IPAddress(uint32_t value) { std::memcpy(bytes_, &value, 4); }
  explicit IPAddress(const uint8_t *p) {
    if (p)
      std::memcpy(bytes_, p, 4);
  }
  uint8_t operator[](int i) const { return bytes_[i]; }
  uint8_t &operator[](int i) { return bytes_[i]; }
  operator uint32_t() const {
    uint32_t value;
    std::memcpy(&value, bytes_, 4);
    return value;
  }
  bool operator==(const IPAddress &ip) const {
    return uint32_t(*this) == uint32_t(ip);
  }
  bool operator!=(const IPAddress &ip) const { return !(*this == ip); }
  bool isSet() const { return uint32_t(*this) != 0; }
  String toString() const;
  bool fromString(const char *s);
  bool fromString(const String &s) { return fromString(s.c_str()); }
  size_t printTo(Print &out) const { return out.print(toString()); }
};
