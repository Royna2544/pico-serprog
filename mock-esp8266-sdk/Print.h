#pragma once
#include "WString.h"
#include "pgmspace.h"
#include <cstdarg>
#include <cstdint>
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2
class Print {
  int error_ = 0;

public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t value) = 0;
  virtual size_t write(const uint8_t *data, size_t n) {
    if (!data && n) {
      setWriteError();
      return 0;
    }
    size_t done = 0;
    while (done < n && write(data[done]))
      ++done;
    return done;
  }
  size_t write(const char *s) {
    return s ? write(reinterpret_cast<const uint8_t *>(s), std::strlen(s)) : 0;
  }
  size_t write(const char *s, size_t n) {
    return write(reinterpret_cast<const uint8_t *>(s), n);
  }
  size_t write_P(PGM_P s) { return write(s); }
  size_t write_P(PGM_P s, size_t n) { return write(s, n); }
  size_t print(const String &s) { return write(s.data(), s.size()); }
  size_t print(const char *s) { return write(s); }
  size_t print(const __FlashStringHelper *s) { return print(String(s)); }
  size_t print(char c) { return write(uint8_t(c)); }
  template <class T>
  auto print(const T &value) -> decltype(value.printTo(*this)) {
    return value.printTo(*this);
  }
  template <class T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
  size_t print(T n, int base = 10) {
    if (base == 0)
      return write(uint8_t(n));
    String text(n, unsigned(base));
    if (base > 10)
      text.toUpperCase();
    return print(text);
  }
  size_t print(double n, int digits = 2) {
    return print(String(n, unsigned(digits)));
  }
  template <class T> size_t println(const T &s) {
    size_t n = print(s);
    return n + println();
  }
  size_t println() { return write("\r\n"); }
  template <class T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
  size_t println(T n, int base) {
    size_t count = print(n, base);
    return count + println();
  }
  size_t println(double n, int digits) {
    size_t count = print(n, digits);
    return count + println();
  }
  size_t printf(const char *format, ...);
  size_t printf_P(const char *format, ...);
  virtual int availableForWrite() { return 0; }
  virtual void flush() {}
  virtual bool outputCanTimeout() { return true; }
  int getWriteError() const { return error_; }
  void clearWriteError() { error_ = 0; }

protected:
  void setWriteError(int error = 1) { error_ = error; }
};
