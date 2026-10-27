#pragma once
#include "IPAddress.h"
#include "Stream.h"
class UDP : public Stream {
public:
  virtual uint8_t begin(uint16_t) = 0;
  virtual void stop() = 0;
  virtual int beginPacket(IPAddress, uint16_t) = 0;
  virtual int beginPacket(const char *, uint16_t) = 0;
  virtual int endPacket() = 0;
  virtual int parsePacket() = 0;
  virtual IPAddress remoteIP() = 0;
  virtual uint16_t remotePort() = 0;
};
