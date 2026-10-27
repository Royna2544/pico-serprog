#pragma once
#include "WString.h"
namespace base64 {
String encode(const uint8_t *data, size_t n, bool newline = true);
inline String encode(const String &s, bool newline = true) {
  return encode(reinterpret_cast<const uint8_t *>(s.data()), s.size(), newline);
}
} // namespace base64
