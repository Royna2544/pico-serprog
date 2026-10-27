#pragma once
#include "Print.h"
#include <sys/types.h>
class Stream : public Print {
protected:
  unsigned long _timeout = 1000;
  int timedRead();
  int timedPeek();

public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual int read(uint8_t *data, size_t n);
  int read(char *data, size_t n) {
    return read(reinterpret_cast<uint8_t *>(data), n);
  }
  void setTimeout(unsigned long value) { _timeout = value; }
  unsigned long getTimeout() const { return _timeout; }
  virtual size_t readBytes(char *data, size_t n);
  size_t readBytes(uint8_t *data, size_t n) {
    return readBytes(reinterpret_cast<char *>(data), n);
  }
  size_t readBytesUntil(char end, char *data, size_t n);
  String readString();
  String readStringUntil(char end);
  bool find(const char *target);
  bool find(const char *target, size_t length);
  bool findUntil(const char *target, const char *end);
  long parseInt();
  float parseFloat();
  virtual bool hasPeekBufferAPI() const { return false; }
  virtual size_t peekAvailable() { return 0; }
  virtual const char *peekBuffer() { return nullptr; }
  virtual void peekConsume(size_t) {}
  virtual bool inputCanTimeout() { return true; }
  virtual ssize_t streamRemaining() { return -1; }
  ssize_t size() { return streamRemaining(); }
  size_t sendSize(Print *out, size_t n, unsigned long timeout = 0);
  size_t sendSize(Print &out, size_t n, unsigned long timeout = 0) {
    return sendSize(&out, n, timeout);
  }
  size_t sendAll(Print *out, unsigned long timeout = 0);
  size_t sendAll(Print &out, unsigned long timeout = 0) {
    return sendAll(&out, timeout);
  }
};
