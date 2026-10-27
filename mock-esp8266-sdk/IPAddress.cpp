#include "IPAddress.h"
#include <arpa/inet.h>
String IPAddress::toString() const {
  char text[INET_ADDRSTRLEN];
  in_addr a{uint32_t(*this)};
  return inet_ntop(AF_INET, &a, text, sizeof(text)) ? String(text) : String();
}
bool IPAddress::fromString(const char *s) {
  in_addr a{};
  if (!s || inet_pton(AF_INET, s, &a) != 1)
    return false;
  std::memcpy(bytes_, &a.s_addr, 4);
  return true;
}
